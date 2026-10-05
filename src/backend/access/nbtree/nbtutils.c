/*-------------------------------------------------------------------------
 *
 * nbtutils.c
 *	  Postgres B-树实现的实用代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>

#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "catalog/catalog.h"
#include "commands/progress.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"


typedef struct BTSortArrayContext
{
	FmgrInfo	flinfo;
	Oid			collation;
	bool		reverse;
} BTSortArrayContext;

static Datum _bt_find_extreme_element(IndexScanDesc scan, ScanKey skey,
									  StrategyNumber strat,
									  Datum *elems, int nelems);
static int	_bt_sort_array_elements(IndexScanDesc scan, ScanKey skey,
									bool reverse,
									Datum *elems, int nelems);
static int	_bt_compare_array_elements(const void *a, const void *b, void *arg);
static bool _bt_compare_scankey_args(IndexScanDesc scan, ScanKey op,
									 ScanKey leftarg, ScanKey rightarg,
									 bool *result);
static bool _bt_fix_scankey_strategy(ScanKey skey, int16 *indoption);
static void _bt_mark_scankey_required(ScanKey skey);
static bool _bt_check_rowcompare(ScanKey skey,
								 IndexTuple tuple, int tupnatts, TupleDesc tupdesc,
								 ScanDirection dir, bool *continuescan);
static int	_bt_keep_natts(Relation rel, IndexTuple lastleft,
						   IndexTuple firstright, BTScanInsert itup_key);


/*
 * _bt_mkscankey
 *		根据 itup 中的比较数据，以及与键数据类型
 *		相称的比较例程，构造插入型扫描键。
 *
 *		当 itup 是非枢轴元组时，返回的插入型扫描键
 *		适合在叶子层为它寻找安放位置。枢轴
 *		元组可用来重新找到高键匹配的叶子页，但
 *		调用者随后要把扫描键的 pivotsearch 字段设为 true。这样
 *		调用者就能搜索高键与之匹配的叶子页，
 *		该页通常位于非枢轴匹配项可能首次出现的
 *		第一张叶子页的左边。
 *
 *		结果供 _bt_compare() 与 _bt_truncate() 使用。
 *		不必填满插入型扫描键参数的调用者
 *		（例如使用专门的比较例程，或仅仅供
 *		_bt_truncate() 使用的扫描键）可以传入 NULL 索引元组。扫描键将
 *		被初始化成如同传入了一个"全部截断"的枢轴元组，
 *		而不是原来的元组。
 *
 *		注意，我们偶尔可能必须对元页加共享锁，
 *		以判断索引中的键是否应当
 *		唯一（也就是这是不是 "heapkeyspace" 索引）。当调用者传入
 *		NULL 元组时，我们假定它是 heapkeyspace 索引，这样索引
 *		构建的调用者就不必访问尚不存在的元页。我们
 *		还假定传入 NULL 元组时索引 _not_ allequalimage；
 *		CREATE INDEX 的调用者会自行调用 _bt_allequalimage() 来设置
 *		该字段。
 */
BTScanInsert
_bt_mkscankey(Relation rel, IndexTuple itup)
{
	BTScanInsert key;
	ScanKey		skey;
	TupleDesc	itupdesc;
	int			indnkeyatts;
	int16	   *indoption;
	int			tupnatts;
	int			i;

	itupdesc = RelationGetDescr(rel);
	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	indoption = rel->rd_indoption;
	tupnatts = itup ? BTreeTupleGetNAtts(itup, rel) : 0;

	Assert(tupnatts <= IndexRelationGetNumberOfAttributes(rel));

	/*
	 * 将用基于键列构造的扫描键执行搜索。
	 * 截断属性与非键属性会从最终
	 * 扫描键中省略。
	 */
	key = palloc(offsetof(BTScanInsertData, scankeys) +
				 sizeof(ScanKeyData) * indnkeyatts);
	if (itup)
		_bt_metaversion(rel, &key->heapkeyspace, &key->allequalimage);
	else
	{
		/* 实用语句的调用者可以自行设置这些字段 */
		key->heapkeyspace = true;
		key->allequalimage = false;
	}
	key->anynullkeys = false;	/* 初始假定 */
	key->nextkey = false;
	key->pivotsearch = false;
	key->keysz = Min(indnkeyatts, tupnatts);
	key->scantid = key->heapkeyspace && itup ?
		BTreeTupleGetHeapTID(itup) : NULL;
	skey = key->scankeys;
	for (i = 0; i < indnkeyatts; i++)
	{
		FmgrInfo   *procinfo;
		Datum		arg;
		bool		null;
		int			flags;

		/*
		 * 可以使用缓存的（默认）支持过程，因为不需要
		 * 跨类型比较。
		 */
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);

		/*
		 * 由截断属性构成的键参数（或调用者
		 * 没有提供元组时）防御性地表示为 NULL。它们
		 * 绝不应当被使用。
		 */
		if (i < tupnatts)
			arg = index_getattr(itup, i + 1, itupdesc, &null);
		else
		{
			arg = (Datum) 0;
			null = true;
		}
		flags = (null ? SK_ISNULL : 0) | (indoption[i] << SK_BT_INDOPTION_SHIFT);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   flags,
									   (AttrNumber) (i + 1),
									   InvalidStrategy,
									   InvalidOid,
									   rel->rd_indcollation[i],
									   procinfo,
									   arg);
		/* 记录是否有任何键属性为 NULL（或已被截断） */
		if (null)
			key->anynullkeys = true;
	}

	/*
	 * 在 NULLS NOT DISTINCT 模式下，我们假装没有空键，以便
	 * 做完整的唯一性检查。
	 */
	if (rel->rd_index->indnullsnotdistinct)
		key->anynullkeys = false;

	return key;
}

/*
 * 释放 _bt_search 建出的回溯栈。
 */
void
_bt_freestack(BTStack stack)
{
	BTStack		ostack;

	while (stack != NULL)
	{
		ostack = stack;
		stack = stack->bts_parent;
		pfree(ostack);
	}
}


/*
 *	_bt_preprocess_array_keys() -- 预处理 SK_SEARCHARRAY 扫描键
 *
 * 若存在任何 SK_SEARCHARRAY 扫描键，拆开 array(s)，并为每一个
 * 相等类型的键建立 BTArrayKeyInfo 信息。
 * 在 so->arrayKeyData 中准备修改后的扫描键，其中保存每次
 * 基本索引扫描期间的当前数组元素。对于不等
 * 数组键，找出极值元素并用该标量值
 * 替换整个数组即可。
 *
 * 注意：之所以需要 so->arrayKeyData，而不是直接改写
 * scan->keyData，是因为允许调用者调用 btrescan 时
 * 不提供一套新的扫描键数据。
 */
void
_bt_preprocess_array_keys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			numberOfKeys = scan->numberOfKeys;
	int16	   *indoption = scan->indexRelation->rd_indoption;
	int			numArrayKeys;
	ScanKey		cur;
	int			i;
	MemoryContext oldContext;

	/* 快速检查是否存在数组键 */
	numArrayKeys = 0;
	for (i = 0; i < numberOfKeys; i++)
	{
		cur = &scan->keyData[i];
		if (cur->sk_flags & SK_SEARCHARRAY)
		{
			numArrayKeys++;
			Assert(!(cur->sk_flags & (SK_ROW_HEADER | SK_SEARCHNULL | SK_SEARCHNOTNULL)));
			/* 若任一数组整体为 NULL，可以立刻退出。 */
			if (cur->sk_flags & SK_ISNULL)
			{
				so->numArrayKeys = -1;
				so->arrayKeyData = NULL;
				return;
			}
		}
	}

	/* 无事可做则退出。 */
	if (numArrayKeys == 0)
	{
		so->numArrayKeys = 0;
		so->arrayKeyData = NULL;
		return;
	}

	/*
	 * 建立与扫描同寿命的上下文，用来存放与数组相关的数据；若上一次
	 * 重新扫描已经建过，则重置它。
	 */
	if (so->arrayContext == NULL)
		so->arrayContext = AllocSetContextCreate(CurrentMemoryContext,
												 "BTree array context",
												 ALLOCSET_SMALL_SIZES);
	else
		MemoryContextReset(so->arrayContext);

	oldContext = MemoryContextSwitchTo(so->arrayContext);

	/* 在工作区上下文中建立 scan->keyData 的可修改副本 */
	so->arrayKeyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
	memcpy(so->arrayKeyData,
		   scan->keyData,
		   scan->numberOfKeys * sizeof(ScanKeyData));

	/* 在工作区上下文中为每个数组的数据分配空间 */
	so->arrayKeys = (BTArrayKeyInfo *) palloc0(numArrayKeys * sizeof(BTArrayKeyInfo));

	/* 现在处理每一个数组键 */
	numArrayKeys = 0;
	for (i = 0; i < numberOfKeys; i++)
	{
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		int			num_nonnulls;
		int			j;

		cur = &so->arrayKeyData[i];
		if (!(cur->sk_flags & SK_SEARCHARRAY))
			continue;

		/*
		 * 首先把数组拆成元素。这里分配的任何东西
		 * （包括可能已解除 TOAST 的数组值）都在
		 * 工作区上下文中。
		 */
		arrayval = DatumGetArrayTypeP(cur->sk_argument);
		/* 这些数据可以缓存，但不清楚是否值得 */
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * 压缩掉任何 NULL 元素。可以忽略它们，因为我们假定
		 * 所有 btree 操作符都是严格的。
		 */
		num_nonnulls = 0;
		for (j = 0; j < num_elems; j++)
		{
			if (!elem_nulls[j])
				elem_values[num_nonnulls++] = elem_values[j];
		}

		/* 现在可以 pfree(elem_nulls)，但不值得为此花周期 */

		/* 若没有非 NULL 元素，扫描限定条件无法满足 */
		if (num_nonnulls == 0)
		{
			numArrayKeys = -1;
			break;
		}

		/*
		 * 若比较操作符不是相等，则数组限定条件
		 * 退化为与最小或最大的
		 * 非 NULL 数组元素做简单比较，视情况而定。
		 */
		switch (cur->sk_strategy)
		{
			case BTLessStrategyNumber:
			case BTLessEqualStrategyNumber:
				cur->sk_argument =
					_bt_find_extreme_element(scan, cur,
											 BTGreaterStrategyNumber,
											 elem_values, num_nonnulls);
				continue;
			case BTEqualStrategyNumber:
				/* 继续循环的其余部分 */
				break;
			case BTGreaterEqualStrategyNumber:
			case BTGreaterStrategyNumber:
				cur->sk_argument =
					_bt_find_extreme_element(scan, cur,
											 BTLessStrategyNumber,
											 elem_values, num_nonnulls);
				continue;
			default:
				elog(ERROR, "unrecognized StrategyNumber: %d",
					 (int) cur->sk_strategy);
				break;
		}

		/*
		 * 对非 NULL 元素排序并去掉重复。必须
		 * 按索引列所用的同一顺序排序，这样相继的
		 * 基本索引扫描才会按索引顺序产出数据。
		 */
		num_elems = _bt_sort_array_elements(scan, cur,
											(indoption[cur->sk_attno - 1] & INDOPTION_DESC) != 0,
											elem_values, num_nonnulls);

		/*
		 * 然后建立 BTArrayKeyInfo 数据。
		 */
		so->arrayKeys[numArrayKeys].scan_key = i;
		so->arrayKeys[numArrayKeys].num_elems = num_elems;
		so->arrayKeys[numArrayKeys].elem_values = elem_values;
		numArrayKeys++;
	}

	so->numArrayKeys = numArrayKeys;

	MemoryContextSwitchTo(oldContext);
}

/*
 * _bt_find_extreme_element() -- 取数组中最小或最大的元素
 *
 * scan 与 skey 标识索引列，其操作符族决定
 * 比较语义。strat 为 BTLessStrategyNumber 时取
 * 最小元素，为 BTGreaterStrategyNumber 时取最大元素。
 */
static Datum
_bt_find_extreme_element(IndexScanDesc scan, ScanKey skey,
						 StrategyNumber strat,
						 Datum *elems, int nelems)
{
	Relation	rel = scan->indexRelation;
	Oid			elemtype,
				cmp_op;
	RegProcedure cmp_proc;
	FmgrInfo	flinfo;
	Datum		result;
	int			i;

	/*
	 * 确定数组元素的名义数据类型。必须
	 * 支持 sk_subtype == InvalidOid 表示操作符类
	 * 输入类型这一约定；这是为简化 ScanKeyInit() 而做的权宜之计。
	 */
	elemtype = skey->sk_subtype;
	if (elemtype == InvalidOid)
		elemtype = rel->rd_opcintype[skey->sk_attno - 1];

	/*
	 * 在操作符族中查找合适的比较操作符。
	 *
	 * 注意：若操作符族不完整，这一步可能失败，
	 * 但操作符族不太可能省略
	 * 它有所支持的任何数据类型上的
	 * 非跨类型比较操作符。
	 */
	cmp_op = get_opfamily_member(rel->rd_opfamily[skey->sk_attno - 1],
								 elemtype,
								 elemtype,
								 strat);
	if (!OidIsValid(cmp_op))
		elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
			 strat, elemtype, elemtype,
			 rel->rd_opfamily[skey->sk_attno - 1]);
	cmp_proc = get_opcode(cmp_op);
	if (!RegProcedureIsValid(cmp_proc))
		elog(ERROR, "missing oprcode for operator %u", cmp_op);

	fmgr_info(cmp_proc, &flinfo);

	Assert(nelems > 0);
	result = elems[0];
	for (i = 1; i < nelems; i++)
	{
		if (DatumGetBool(FunctionCall2Coll(&flinfo,
										   skey->sk_collation,
										   elems[i],
										   result)))
			result = elems[i];
	}

	return result;
}

/*
 * _bt_sort_array_elements() -- 对数组元素排序并去重
 *
 * 数组元素就地排序，返回去掉重复之后
 * 的新元素个数。
 *
 * scan 与 skey 标识索引列，其操作符族决定
 * 比较语义。若 reverse 为真，则按降序排序。
 */
static int
_bt_sort_array_elements(IndexScanDesc scan, ScanKey skey,
						bool reverse,
						Datum *elems, int nelems)
{
	Relation	rel = scan->indexRelation;
	Oid			elemtype;
	RegProcedure cmp_proc;
	BTSortArrayContext cxt;

	if (nelems <= 1)
		return nelems;			/* 无事可做 */

	/*
	 * 确定数组元素的名义数据类型。必须
	 * 支持 sk_subtype == InvalidOid 表示操作符类
	 * 输入类型这一约定；这是为简化 ScanKeyInit() 而做的权宜之计。
	 */
	elemtype = skey->sk_subtype;
	if (elemtype == InvalidOid)
		elemtype = rel->rd_opcintype[skey->sk_attno - 1];

	/*
	 * 在操作符族中查找合适的比较函数。
	 *
	 * 注意：若操作符族不完整，这一步可能失败，
	 * 但操作符族不太可能省略
	 * 它有所支持的任何数据类型上的
	 * 非跨类型支持函数。
	 */
	cmp_proc = get_opfamily_proc(rel->rd_opfamily[skey->sk_attno - 1],
								 elemtype,
								 elemtype,
								 BTORDER_PROC);
	if (!RegProcedureIsValid(cmp_proc))
		elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
			 BTORDER_PROC, elemtype, elemtype,
			 rel->rd_opfamily[skey->sk_attno - 1]);

	/* 对数组元素排序 */
	fmgr_info(cmp_proc, &cxt.flinfo);
	cxt.collation = skey->sk_collation;
	cxt.reverse = reverse;
	qsort_arg(elems, nelems, sizeof(Datum),
			  _bt_compare_array_elements, &cxt);

	/* 现在扫描已排序的元素并去掉重复 */
	return qunique_arg(elems, nelems, sizeof(Datum),
					   _bt_compare_array_elements, &cxt);
}

/*
 * 供 qsort_arg 对数组元素排序的比较器
 */
static int
_bt_compare_array_elements(const void *a, const void *b, void *arg)
{
	Datum		da = *((const Datum *) a);
	Datum		db = *((const Datum *) b);
	BTSortArrayContext *cxt = (BTSortArrayContext *) arg;
	int32		compare;

	compare = DatumGetInt32(FunctionCall2Coll(&cxt->flinfo,
											  cxt->collation,
											  da, db));
	if (cxt->reverse)
		INVERT_COMPARE_RESULT(compare);
	return compare;
}

/*
 * _bt_start_array_keys() -- 在扫描开始时初始化数组键
 *
 * 设置 cur_elem 计数器，并为每个数组扫描键填入
 * 第一个 sk_argument 值。在知道扫描方向之前无法做这件事。
 */
void
_bt_start_array_keys(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			i;

	for (i = 0; i < so->numArrayKeys; i++)
	{
		BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
		ScanKey		skey = &so->arrayKeyData[curArrayKey->scan_key];

		Assert(curArrayKey->num_elems > 0);
		if (ScanDirectionIsBackward(dir))
			curArrayKey->cur_elem = curArrayKey->num_elems - 1;
		else
			curArrayKey->cur_elem = 0;
		skey->sk_argument = curArrayKey->elem_values[curArrayKey->cur_elem];
	}

	so->arraysStarted = true;
}

/*
 * _bt_advance_array_keys() -- 前进到下一组数组元素
 *
 * 若还有另一组值要考虑则返回 true，否则返回 false。
 * 返回 true 时，扫描键已用下一组值初始化。
 */
bool
_bt_advance_array_keys(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		found = false;
	int			i;

	/*
	 * 必须让最后一个数组键前进得最快，因为它会
	 * 对应现有限定条件中阶数最低的索引列。
	 * 存在多个数组键时，这是保证输出
	 * 顺序正确所必需的。
	 */
	for (i = so->numArrayKeys - 1; i >= 0; i--)
	{
		BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
		ScanKey		skey = &so->arrayKeyData[curArrayKey->scan_key];
		int			cur_elem = curArrayKey->cur_elem;
		int			num_elems = curArrayKey->num_elems;

		if (ScanDirectionIsBackward(dir))
		{
			if (--cur_elem < 0)
			{
				cur_elem = num_elems - 1;
				found = false;	/* 需要前进下一个数组键 */
			}
			else
				found = true;
		}
		else
		{
			if (++cur_elem >= num_elems)
			{
				cur_elem = 0;
				found = false;	/* 需要前进下一个数组键 */
			}
			else
				found = true;
		}

		curArrayKey->cur_elem = cur_elem;
		skey->sk_argument = curArrayKey->elem_values[cur_elem];
		if (found)
			break;
	}

	/* 前进并行扫描 */
	if (scan->parallel_scan != NULL)
		_bt_parallel_advance_array_keys(scan);

	/*
	 * 若没有找到新的数组键，扫描已"越过"
	 * 数组键的末尾。若需要重新扫描，_bt_start_array_keys 仍可
	 * "重新开始"这些数组键。
	 */
	if (!found)
		so->arraysStarted = false;

	return found;
}

/*
 * _bt_mark_array_keys() -- 在 btmarkpos 期间处理数组键
 *
 * 把数组键的当前状态存为"标记"位置。
 */
void
_bt_mark_array_keys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			i;

	for (i = 0; i < so->numArrayKeys; i++)
	{
		BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];

		curArrayKey->mark_elem = curArrayKey->cur_elem;
	}
}

/*
 * _bt_restore_array_keys() -- 在 btrestrpos 期间处理数组键
 *
 * 把数组键恢复到设置标记时的位置。
 */
void
_bt_restore_array_keys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		changed = false;
	int			i;

	/* 把每个数组键恢复到设置标记时的位置 */
	for (i = 0; i < so->numArrayKeys; i++)
	{
		BTArrayKeyInfo *curArrayKey = &so->arrayKeys[i];
		ScanKey		skey = &so->arrayKeyData[curArrayKey->scan_key];
		int			mark_elem = curArrayKey->mark_elem;

		if (curArrayKey->cur_elem != mark_elem)
		{
			curArrayKey->cur_elem = mark_elem;
			skey->sk_argument = curArrayKey->elem_values[mark_elem];
			changed = true;
		}
	}

	/*
	 * 若改动了任何键，必须重做 _bt_preprocess_keys。这听起来
	 * 可能过头，但同一索引列上有多个键时，
	 * 似乎有必要做全套处理。
	 *
	 * 每当扫描的数组键集合在上一次基本索引扫描
	 * 结束时"回绕"，也要这样做。回绕之后别处
	 * 不会再调用 _bt_preprocess_keys，因此
	 * 我们自己来做。
	 */
	if (changed || !so->arraysStarted)
	{
		_bt_preprocess_keys(scan);
		/* 标记应当设在一套一致的键上…… */
		Assert(so->qual_ok);
	}
}


/*
 *	_bt_preprocess_keys() -- 预处理扫描键
 *
 * 给定的搜索型键（在 scan->keyData[] 或 so->arrayKeyData[] 中）
 * 被复制到 so->keyData[]，其间可能做变换。
 * scan->numberOfKeys 是输入键的个数，so->numberOfKeys 得到
 * 输出键的个数（可能更少，绝不会更多）。
 *
 * 输出键会标上调用者所提供的系统标准位之外的
 * 额外 sk_flags 位。相关索引属性的 DESC 与 NULLS_FIRST
 * indoption 位被复制进这些标志。
 * 此外，对 DESC 列，我们交换（翻转）所有 sk_strategy 编号，
 * 使索引按所需方向排序。
 *
 * 本例程的一个主要目的，是发现哪些扫描键必须被
 * 满足才能继续扫描。它也试图消除冗余
 * 键并检测互相矛盾的键。（若索引操作符族提供的
 * 跨类型操作符集合不完整，我们可能发现不了冗余
 * 或互相矛盾的键，但仍然可以继续工作。）
 *
 * 输出键必须按索引属性排序。目前我们预期
 * （并会验证）输入键已经如此排序 --- 这由
 * indxpath.c 中的 match_clauses_to_index() 完成。处理过程中可能作为
 * 副产品重排同一属性内的键，
 * 但没有其他代码依赖这一点。
 *
 * 若输出键必须被满足才能分别沿正向或反向
 * 继续扫描，则标上 SK_BT_REQFWD 与/或 SK_BT_REQBKWD。
 * _bt_checkkeys 使用这些标志。例如，若限定条件
 * 是 "x = 1 AND y < 4 AND z < 5"，则 _bt_checkkeys 会拒绝元组
 * (1,2,7)，但必须继续扫描，以防还有元组 (1,3,z)。
 * 一旦到达 (1,4,z) 这样的元组就可以停止扫描，因为后面
 * 不会再有元组匹配。这体现为给 x 与 y 键
 * 标上 SK_BT_REQFWD，而不给 z 键。一般而言，带 "=" 键的前导
 * 属性的键会同时标上 SK_BT_REQFWD 与 SK_BT_REQBKWD。
 * 对第一个没有 "=" 键的属性，任何 "<" 与 "<=" 键
 * 标 SK_BT_REQFWD，任何 ">" 与 ">=" 键标 SK_BT_REQBKWD。
 * 考虑上面的例子可以看出这是对的。特别
 * 注意，若某个属性没有任何键，则后续
 * 属性的键绝不可能成为必需；例如 "WHERE y = 4"
 * 需要一次全索引扫描。
 *
 * 若可能，就消除冗余键：只保留最紧的
 * >/>= 边界和最紧的 </<= 边界；若有 = 键，则
 * 只返回它。（因此，对每个属性我们要么返回单独一个 = 键，
 * 要么返回一到两个边界条件键。）但是，若因缺少合适的
 * 跨类型操作符而无法比较两个键，
 * 就无法去掉其中任何一个。若有两个策略相同的
 * 这样的键，第二个只是被推进输出数组，
 * 这里不再进一步处理。若无法比较，我们也可能同时输出两个 >/>=
 * 或两个 </<= 键。即便不去掉冗余键，关于必需键的逻辑
 * 仍然成立。
 *
 * 需要方向敏感的必需键标志，原因之一正是
 * 我们也许无法消除冗余键。假设
 * 有 "x > 4::int AND x > 10::bigint"，而又因缺少合适的跨类型操作符
 * 无法判断哪个键更严。
 * _bt_first 会任意挑选其中一个键来做初始
 * 定位。若它选了 x > 4，则在到达 > 10 的索引项之前，
 * x > 10 条件都会失败；但不会仅仅因为
 * x > 10 失败就停止扫描。另一方面，若正在反向扫描，则
 * 任一键失败都确实足以停止扫描。（一般而言，当
 * 存在不等键时，初始定位代码只保证
 * 定位到第一个可能匹配之前，而不是恰好在第一个匹配上，
 * 这是对正向扫描而言；对反向扫描则是定位到最后一个匹配之后。）
 *
 * 作为这项工作的副产品，我们可以检测互相矛盾的限定条件，例如
 * "x = 1 AND x > 2"。若看到这种情况，就返回 so->qual_ok = false，
 * 表示扫描根本不必运行，因为没有元组能匹配。
 * （这种情况下我们懒得把输出键数组填完！）
 * 同样，缺少跨类型操作符可能使我们无法证明
 * 限定条件其实互相矛盾，但扫描仍会正确工作。
 *
 * 行比较键目前也不做任何聪明处理：
 * 只是把它们原样转入预处理数组，不做
 * 加工。就必需键的逻辑而言，可以把它们当作
 * 对该行第一个索引列的普通不等
 * 比较。
 *
 * 注意：必须把预处理后的扫描键复制到私有
 * 存储中，是因为我们会根据键参数值的比较来修改数组，
 * 而这些值可能在重新扫描或前进到
 * 数组键的新元素后改变。因此无法覆盖源数据。
 */
void
_bt_preprocess_keys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int			numberOfKeys = scan->numberOfKeys;
	int16	   *indoption = scan->indexRelation->rd_indoption;
	int			new_numberOfKeys;
	int			numberOfEqualCols;
	ScanKey		inkeys;
	ScanKey		outkeys;
	ScanKey		cur;
	ScanKey		xform[BTMaxStrategyNumber];
	bool		test_result;
	int			i,
				j;
	AttrNumber	attno;

	/* 初始化结果变量 */
	so->qual_ok = true;
	so->numberOfKeys = 0;

	if (numberOfKeys < 1)
		return;					/* 没有限定条件的扫描则结束 */

	/*
	 * 若存在数组键则读 so->arrayKeyData，否则读 scan->keyData
	 */
	if (so->arrayKeyData != NULL)
		inkeys = so->arrayKeyData;
	else
		inkeys = scan->keyData;

	outkeys = so->keyData;
	cur = &inkeys[0];
	/* 我们检查输入键的顺序是否正确 */
	if (cur->sk_attno < 1)
		elog(ERROR, "btree index keys must be ordered by attribute");

	/* 若只有一个键，可以短路掉大部分工作 */
	if (numberOfKeys == 1)
	{
		/* 把 indoption 应用到扫描键（可能改变 sk_strategy！） */
		if (!_bt_fix_scankey_strategy(cur, indoption))
			so->qual_ok = false;
		memcpy(outkeys, cur, sizeof(ScanKeyData));
		so->numberOfKeys = 1;
		/* 若限定条件针对第一个索引列，可以把它标为必需 */
		if (cur->sk_attno == 1)
			_bt_mark_scankey_required(outkeys);
		return;
	}

	/*
	 * 否则，做全套处理。
	 */
	new_numberOfKeys = 0;
	numberOfEqualCols = 0;

	/*
	 * 初始化对属性 1 的键的处理。
	 *
	 * xform[i] 指向策略类型 i+1 当前最佳的扫描键；若
	 * 尚未为该属性找到这样的键，则为 NULL。
	 */
	attno = 1;
	memset(xform, 0, sizeof(xform));

	/*
	 * 循环从 0 迭代到 numberOfKeys（含）；最后一趟用来
	 * 处理后一个键之后的收尾。真正退出循环是在
	 * 下面的 "break" 语句。
	 */
	for (i = 0;; cur++, i++)
	{
		if (i < numberOfKeys)
		{
			/* 把 indoption 应用到扫描键（可能改变 sk_strategy！） */
			if (!_bt_fix_scankey_strategy(cur, indoption))
			{
				/* NULL 无法匹配，因此放弃 */
				so->qual_ok = false;
				return;
			}
		}

		/*
		 * 若已到某个属性的键的末尾，就结束
		 * 处理并发出整理后的键。
		 */
		if (i == numberOfKeys || cur->sk_attno != attno)
		{
			int			priorNumberOfEqualCols = numberOfEqualCols;

			/* 检查输入键的顺序是否正确 */
			if (i < numberOfKeys && cur->sk_attno < attno)
				elog(ERROR, "btree index keys must be ordered by attribute");

			/*
			 * 若已指定 =，其余所有键都可以作为
			 * 冗余而消除。若遇到 key = 1 AND key > 2 这样的情形，可以把
			 * qual_ok 设为 false 并放弃进一步处理。
			 *
			 * 还要处理 "key IS NULL" 的情形，它与任何其他
			 * 索引条件组合都无法满足。等到
			 * 这里时，它已被归类为相等
			 * 检查，并且我们已经拒绝了它与普通
			 * 相等条件的任何组合；但还没有拒绝与其他类型条件的组合。
			 */
			if (xform[BTEqualStrategyNumber - 1])
			{
				ScanKey		eq = xform[BTEqualStrategyNumber - 1];

				for (j = BTMaxStrategyNumber; --j >= 0;)
				{
					ScanKey		chk = xform[j];

					if (!chk || j == (BTEqualStrategyNumber - 1))
						continue;

					if (eq->sk_flags & SK_SEARCHNULL)
					{
						/* IS NULL 与其他任何条件都矛盾 */
						so->qual_ok = false;
						return;
					}

					if (_bt_compare_scankey_args(scan, chk, eq, chk,
												 &test_result))
					{
						if (!test_result)
						{
							/* 已证明这些键互相矛盾 */
							so->qual_ok = false;
							return;
						}
						/* 否则丢掉冗余的非相等键 */
						xform[j] = NULL;
					}
					/* 否则无法判断是否冗余，两个键都保留 */
				}
				/* 记录我们拥有 "=" 键的属性个数 */
				numberOfEqualCols++;
			}

			/* 尽量只保留 < 与 <= 中的一个 */
			if (xform[BTLessStrategyNumber - 1]
				&& xform[BTLessEqualStrategyNumber - 1])
			{
				ScanKey		lt = xform[BTLessStrategyNumber - 1];
				ScanKey		le = xform[BTLessEqualStrategyNumber - 1];

				if (_bt_compare_scankey_args(scan, le, lt, le,
											 &test_result))
				{
					if (test_result)
						xform[BTLessEqualStrategyNumber - 1] = NULL;
					else
						xform[BTLessStrategyNumber - 1] = NULL;
				}
			}

			/* 尽量只保留 > 与 >= 中的一个 */
			if (xform[BTGreaterStrategyNumber - 1]
				&& xform[BTGreaterEqualStrategyNumber - 1])
			{
				ScanKey		gt = xform[BTGreaterStrategyNumber - 1];
				ScanKey		ge = xform[BTGreaterEqualStrategyNumber - 1];

				if (_bt_compare_scankey_args(scan, ge, gt, ge,
											 &test_result))
				{
					if (test_result)
						xform[BTGreaterEqualStrategyNumber - 1] = NULL;
					else
						xform[BTGreaterStrategyNumber - 1] = NULL;
				}
			}

			/*
			 * 把整理后的键发到 outkeys[] 数组，然后
			 * 若它们是必需的就做标记。若此属性之前的所有属性都有 "="，
			 * 则它们是必需的（可能只在一个方向上）。
			 */
			for (j = BTMaxStrategyNumber; --j >= 0;)
			{
				if (xform[j])
				{
					ScanKey		outkey = &outkeys[new_numberOfKeys++];

					memcpy(outkey, xform[j], sizeof(ScanKeyData));
					if (priorNumberOfEqualCols == attno - 1)
						_bt_mark_scankey_required(outkey);
				}
			}

			/*
			 * 若已完成，在此退出循环。
			 */
			if (i == numberOfKeys)
				break;

			/* 为新的 attno 重新初始化 */
			attno = cur->sk_attno;
			memset(xform, 0, sizeof(xform));
		}

		/* 检查此键的操作符对应的策略 */
		j = cur->sk_strategy - 1;

		/* 若是行比较，直接推进输出数组 */
		if (cur->sk_flags & SK_ROW_HEADER)
		{
			ScanKey		outkey = &outkeys[new_numberOfKeys++];

			memcpy(outkey, cur, sizeof(ScanKeyData));
			if (numberOfEqualCols == attno - 1)
				_bt_mark_scankey_required(outkey);

			/*
			 * 不支持把 RowCompare 用于相等；这样的限定条件会
			 * 打乱 numberOfEqualCols 的跟踪。
			 */
			Assert(j != (BTEqualStrategyNumber - 1));
			continue;
		}

		/* 以前见过这种键吗？ */
		if (xform[j] == NULL)
		{
			/* 没有，因此记住这个扫描键 */
			xform[j] = cur;
		}
		else
		{
			/* 有，只保留更严的那个键 */
			if (_bt_compare_scankey_args(scan, cur, cur, xform[j],
										 &test_result))
			{
				if (test_result)
					xform[j] = cur;
				else if (j == (BTEqualStrategyNumber - 1))
				{
					/* key == a && key == b，但 a != b */
					so->qual_ok = false;
					return;
				}
				/* 否则旧键更严，保留它 */
			}
			else
			{
				/*
				 * 无法判断哪个键更严。把
				 * 前一个留在 xform[j] 中，并把这一个直接推进
				 * 输出数组。
				 */
				ScanKey		outkey = &outkeys[new_numberOfKeys++];

				memcpy(outkey, cur, sizeof(ScanKeyData));
				if (numberOfEqualCols == attno - 1)
					_bt_mark_scankey_required(outkey);
			}
		}
	}

	so->numberOfKeys = new_numberOfKeys;
}

/*
 * 用指定操作符比较两个扫描键的值。
 *
 * 想做的测试在逻辑上是 "leftarg op rightarg"，其中
 * leftarg 与 rightarg 是这些 ScanKeys 中的 sk_argument 值，
 * 比较操作符是 op 这个 ScanKey 中的那个。但是，在
 * 跨数据类型的情形下，可能需要在
 * 索引的操作符族中查找正确的操作符：它的 amopstrategy = op->sk_strategy，
 * 且 amoplefttype/amoprighttype 等于两个参数的数据类型。
 *
 * 若操作符族没有提供完整的跨类型操作符集合，我们
 * 也许做不成这次比较。若能做成比较，
 * 就把操作符结果存入 *result 并返回 true。若比较
 * 无法完成，则返回 false。
 *
 * 注意：op 总是与 leftarg 或 rightarg 指向同一个 ScanKey。
 * 既然我们不改写扫描键，这种别名不应造成
 * 问题。
 *
 * 注意：本例程必须对索引列上的任何 DESC 选项不敏感。
 * 例如，无论索引按哪个方向排序，"x < 4" 都是比
 * "x < 5" 更紧的约束。
 */
static bool
_bt_compare_scankey_args(IndexScanDesc scan, ScanKey op,
						 ScanKey leftarg, ScanKey rightarg,
						 bool *result)
{
	Relation	rel = scan->indexRelation;
	Oid			lefttype,
				righttype,
				optype,
				opcintype,
				cmp_op;
	StrategyNumber strat;

	/*
	 * 首先处理一个或两个参数为 NULL 的情形。这只应
	 * 发生在扫描键表示 IS NULL/NOT NULL 条件时。
	 */
	if ((leftarg->sk_flags | rightarg->sk_flags) & SK_ISNULL)
	{
		bool		leftnull,
					rightnull;

		if (leftarg->sk_flags & SK_ISNULL)
		{
			Assert(leftarg->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
			leftnull = true;
		}
		else
			leftnull = false;
		if (rightarg->sk_flags & SK_ISNULL)
		{
			Assert(rightarg->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
			rightnull = true;
		}
		else
			rightnull = false;

		/*
		 * 我们把 NULL 当作大于或小于所有其他值。
		 * 由于 true > false，下面的测试对 NULLS LAST
		 * 逻辑是正确的。若索引是 NULLS FIRST，需要翻转策略。
		 */
		strat = op->sk_strategy;
		if (op->sk_flags & SK_BT_NULLS_FIRST)
			strat = BTCommuteStrategyNumber(strat);

		switch (strat)
		{
			case BTLessStrategyNumber:
				*result = (leftnull < rightnull);
				break;
			case BTLessEqualStrategyNumber:
				*result = (leftnull <= rightnull);
				break;
			case BTEqualStrategyNumber:
				*result = (leftnull == rightnull);
				break;
			case BTGreaterEqualStrategyNumber:
				*result = (leftnull >= rightnull);
				break;
			case BTGreaterStrategyNumber:
				*result = (leftnull > rightnull);
				break;
			default:
				elog(ERROR, "unrecognized StrategyNumber: %d", (int) strat);
				*result = false;	/* 让编译器保持安静 */
				break;
		}
		return true;
	}

	/*
	 * 需要关心的操作符族由索引列标识。
	 */
	Assert(leftarg->sk_attno == rightarg->sk_attno);

	opcintype = rel->rd_opcintype[leftarg->sk_attno - 1];

	/*
	 * 确定 ScanKey 参数的实际数据类型。必须
	 * 支持 sk_subtype == InvalidOid 表示操作符类
	 * 输入类型这一约定；这是为简化 ScanKeyInit() 而做的权宜之计。
	 */
	lefttype = leftarg->sk_subtype;
	if (lefttype == InvalidOid)
		lefttype = opcintype;
	righttype = rightarg->sk_subtype;
	if (righttype == InvalidOid)
		righttype = opcintype;
	optype = op->sk_subtype;
	if (optype == InvalidOid)
		optype = opcintype;

	/*
	 * 若 leftarg 与 rightarg 匹配 "op" 扫描键所预期的类型，
	 * 就可以使用已经查好的比较函数。
	 */
	if (lefttype == opcintype && righttype == optype)
	{
		*result = DatumGetBool(FunctionCall2Coll(&op->sk_func,
												 op->sk_collation,
												 leftarg->sk_argument,
												 rightarg->sk_argument));
		return true;
	}

	/*
	 * 否则，需要到系统缓存中找合适的
	 * 操作符。（这不会导致无限递归，因为由系统缓存查找发起的
	 * 索引扫描不会使用跨数据类型
	 * 操作符。）
	 *
	 * 若 sk_strategy 曾被 _bt_fix_scankey_strategy 翻转，必须
	 * 把它翻回来，才能得到正确的操作符族成员。
	 */
	strat = op->sk_strategy;
	if (op->sk_flags & SK_BT_DESC)
		strat = BTCommuteStrategyNumber(strat);

	cmp_op = get_opfamily_member(rel->rd_opfamily[leftarg->sk_attno - 1],
								 lefttype,
								 righttype,
								 strat);
	if (OidIsValid(cmp_op))
	{
		RegProcedure cmp_proc = get_opcode(cmp_op);

		if (RegProcedureIsValid(cmp_proc))
		{
			*result = DatumGetBool(OidFunctionCall2Coll(cmp_proc,
														op->sk_collation,
														leftarg->sk_argument,
														rightarg->sk_argument));
			return true;
		}
	}

	/* 无法做这次比较 */
	*result = false;			/* 抑制编译器警告 */
	return false;
}

/*
 * 按 indoption 的需要调整扫描键的策略与标志设置。
 *
 * 把合适的 indoption 值复制进扫描键的 sk_flags
 * （移位，以免覆盖系统定义的标志位）。此外，若
 * 设置了 DESC 选项，就交换（翻转）操作符策略编号。
 *
 * 次要目的是检查 IS NULL/NOT NULL 扫描键，并为它们
 * 正确设置策略字段。
 *
 * 最后，对普通扫描键（不是 IS NULL/NOT NULL），检查
 * 比较值是否为 NULL。由于假定所有 btree 操作符都是严格的，
 * NULL 意味着限定条件无法满足。若比较值
 * 不是 NULL 则返回 true，若应放弃扫描则返回 false。
 *
 * 本函数作用于*输入*扫描键结构；因此
 * 重新扫描时看到的是已经处理过的扫描键。所以
 * 若已经交换过策略，必须小心不要再次交换。
 * 修改调用者的扫描键副本有点难看，但实践中
 * 不应当有问题，因为在扫描键存活期间，索引的 indoption 肯定
 * 不会改变。
 */
static bool
_bt_fix_scankey_strategy(ScanKey skey, int16 *indoption)
{
	int			addflags;

	addflags = indoption[skey->sk_attno - 1] << SK_BT_INDOPTION_SHIFT;

	/*
	 * 我们把所有 btree 操作符都当作严格的（即使 pg_proc 里
	 * 没有这样标记）。这意味着带 NULL 比较常量的操作符条件
	 * 不可能成功，可以立刻
	 * 拒绝它。
	 *
	 * 不过，现在也支持把 "x IS NULL" 子句当作搜索条件，
	 * 因此那种情况下要继续。规划器在这种情况下没有填入任何
	 * 特定策略，所以把它设为 BTEqualStrategyNumber
	 * --- 可以把 IS NULL 当作相等操作符，就
	 * 搜索策略而言。
	 *
	 * 同样，支持 "x IS NOT NULL"。在 NULLS LAST 索引中把它当作
	 * "小于 NULL"，在 NULLS
	 * FIRST 索引中把它当作"大于 NULL"。
	 *
	 * 注意：将来也许必须用索引
	 * 列的排序规则填 sk_collation。目前这不是问题，因为我们
	 * 实际上绝不会对 NULL 调用比较操作符。
	 */
	if (skey->sk_flags & SK_ISNULL)
	{
		/* 行头扫描键中不应设置 SK_ISNULL */
		Assert(!(skey->sk_flags & SK_ROW_HEADER));

		/* 在扫描键中设置 indoption 标志（可能已经设过） */
		skey->sk_flags |= addflags;

		/* 为 IS NULL 或 NOT NULL 搜索设置正确的策略 */
		if (skey->sk_flags & SK_SEARCHNULL)
		{
			skey->sk_strategy = BTEqualStrategyNumber;
			skey->sk_subtype = InvalidOid;
			skey->sk_collation = InvalidOid;
		}
		else if (skey->sk_flags & SK_SEARCHNOTNULL)
		{
			if (skey->sk_flags & SK_BT_NULLS_FIRST)
				skey->sk_strategy = BTGreaterStrategyNumber;
			else
				skey->sk_strategy = BTLessStrategyNumber;
			skey->sk_subtype = InvalidOid;
			skey->sk_collation = InvalidOid;
		}
		else
		{
			/* 普通限定条件，因此无法满足 */
			return false;
		}

		/* 不必再做其余部分 */
		return true;
	}

	/* 若尚未为 DESC 调整策略，现在调整 */
	if ((addflags & SK_BT_DESC) && !(skey->sk_flags & SK_BT_DESC))
		skey->sk_strategy = BTCommuteStrategyNumber(skey->sk_strategy);
	skey->sk_flags |= addflags;

	/* 若是行头，同样修正行成员的标志与策略 */
	if (skey->sk_flags & SK_ROW_HEADER)
	{
		ScanKey		subkey = (ScanKey) DatumGetPointer(skey->sk_argument);

		for (;;)
		{
			Assert(subkey->sk_flags & SK_ROW_MEMBER);
			addflags = indoption[subkey->sk_attno - 1] << SK_BT_INDOPTION_SHIFT;
			if ((addflags & SK_BT_DESC) && !(subkey->sk_flags & SK_BT_DESC))
				subkey->sk_strategy = BTCommuteStrategyNumber(subkey->sk_strategy);
			subkey->sk_flags |= addflags;
			if (subkey->sk_flags & SK_ROW_END)
				break;
			subkey++;
		}
	}

	return true;
}

/*
 * 把扫描键标为"继续扫描所必需"。
 *
 * 视操作符类型而定，该键可能对两个扫描
 * 方向都必需，或只对一个方向必需。此外，若该键是行比较头，
 * 必须把它的第一个附属 ScanKey 标为必需。（后续
 * 附属 ScanKeys 通常针对更低阶的列，因此
 * 无法成为必需，因为它们位于第一个非相等扫描键之后。）
 *
 * 注意：在附属扫描键中设置必需键标志位时，我们是在
 * 改写属于索引访问方法调用者的数据结构，而不是
 * 我们的私有副本。这应当没问题，因为同一次查询中从一次扫描到
 * 另一次扫描，标记不会改变，重新扫描时反正也会
 * 按同样方式再标一次。不过这件事值得留意。
 */
static void
_bt_mark_scankey_required(ScanKey skey)
{
	int			addflags;

	switch (skey->sk_strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			addflags = SK_BT_REQFWD;
			break;
		case BTEqualStrategyNumber:
			addflags = SK_BT_REQFWD | SK_BT_REQBKWD;
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			addflags = SK_BT_REQBKWD;
			break;
		default:
			elog(ERROR, "unrecognized StrategyNumber: %d",
				 (int) skey->sk_strategy);
			addflags = 0;		/* 让编译器保持安静 */
			break;
	}

	skey->sk_flags |= addflags;

	if (skey->sk_flags & SK_ROW_HEADER)
	{
		ScanKey		subkey = (ScanKey) DatumGetPointer(skey->sk_argument);

		/* 第一个子键的列与操作符应当与头相同 */
		Assert(subkey->sk_flags & SK_ROW_MEMBER);
		Assert(subkey->sk_attno == skey->sk_attno);
		Assert(subkey->sk_strategy == skey->sk_strategy);
		subkey->sk_flags |= addflags;
	}
}

/*
 * 测试索引元组是否满足全部扫描键条件。
 *
 * 满足则返回 true，否则返回 false。若元组未通过限定条件，
 * 还要判断是否需要在此元组之后继续扫描，
 * 并相应设置 *continuescan。做法见上面
 * _bt_preprocess_keys() 的注释。
 *
 * 正向扫描的调用者可以传入高键元组，希望我们
 * 把 *continuescan 设为 false，从而避免不必要地访问
 * 右边那一页。
 *
 * scan：索引扫描描述符（含搜索型扫描键）
 * tuple：要测试的索引元组
 * tupnatts：tupnatts 中的属性个数（高键可能已被截断）
 * dir：我们正在扫描的方向
 * continuescan：输出参数（在所有情况下都会被正确设置）
 */
bool
_bt_checkkeys(IndexScanDesc scan, IndexTuple tuple, int tupnatts,
			  ScanDirection dir, bool *continuescan)
{
	TupleDesc	tupdesc;
	BTScanOpaque so;
	int			keysz;
	int			ikey;
	ScanKey		key;

	Assert(BTreeTupleGetNAtts(tuple, scan->indexRelation) == tupnatts);

	*continuescan = true;		/* 默认假定 */

	tupdesc = RelationGetDescr(scan->indexRelation);
	so = (BTScanOpaque) scan->opaque;
	keysz = so->numberOfKeys;

	for (key = so->keyData, ikey = 0; ikey < keysz; key++, ikey++)
	{
		Datum		datum;
		bool		isNull;
		Datum		test;

		if (key->sk_attno > tupnatts)
		{
			/*
			 * 该属性已被截断（必定是高键）。右边那一页上
			 * 第一个非枢轴元组中该属性的值
			 * 可以是任何可能的值。假定被截断的
			 * 属性通过限定条件。
			 */
			Assert(ScanDirectionIsForward(dir));
			Assert(BTreeTupleIsPivot(tuple));
			continue;
		}

		/* 行比较键需要特殊处理 */
		if (key->sk_flags & SK_ROW_HEADER)
		{
			if (_bt_check_rowcompare(key, tuple, tupnatts, tupdesc, dir,
									 continuescan))
				continue;
			return false;
		}

		datum = index_getattr(tuple,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/* 处理 IS NULL/NOT NULL 测试 */
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (isNull)
					continue;	/* 元组满足此限定条件 */
			}
			else
			{
				Assert(key->sk_flags & SK_SEARCHNOTNULL);
				if (!isNull)
					continue;	/* 元组满足此限定条件 */
			}

			/*
			 * 元组未通过此限定条件。若它对当前
			 * 扫描方向是必需限定条件，则可以断定后面也不会再有
			 * 元组通过。
			 */
			if ((key->sk_flags & SK_BT_REQFWD) &&
				ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((key->sk_flags & SK_BT_REQBKWD) &&
					 ScanDirectionIsBackward(dir))
				*continuescan = false;

			/*
			 * 无论如何，此索引元组不匹配限定条件。
			 */
			return false;
		}

		if (isNull)
		{
			if (key->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
				 * 由于 NULLs 排在非 NULL 之前，可知已经
				 * 到达此索引属性取值范围的下界。
				 * 在反向扫描中，若此限定条件
				 * 属于"必须匹配"的子集，就可以停止。只要它是必需的，
				 * 无论限定条件是 > 还是 < 都可以停止，
				 * 因为后面不可能再有元组通过。在
				 * 正向扫描中则必须继续，因为我们可能
				 * 最初定位在索引的起点。
				 */
				if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
				 * 由于 NULLs 排在非 NULL 之后，可知已经
				 * 到达此索引属性取值范围的上界。
				 * 在正向扫描中，若此限定条件
				 * 属于"必须匹配"的子集，就可以停止。只要它是必需的，
				 * 无论限定条件是 > 还是 < 都可以停止，
				 * 因为后面不可能再有元组通过。在
				 * 反向扫描中则必须继续，因为我们
				 * 可能最初定位在索引的终点。
				 */
				if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * 无论如何，此索引元组不匹配限定条件。
			 */
			return false;
		}

		test = FunctionCall2Coll(&key->sk_func, key->sk_collation,
								 datum, key->sk_argument);

		if (!DatumGetBool(test))
		{
			/*
			 * 元组未通过此限定条件。若它对当前
			 * 扫描方向是必需限定条件，则可以断定后面也不会再有
			 * 元组通过。
			 *
			 * 注意：因为一旦任何必需的相等
			 * 限定条件失败就停止扫描，所以在有相等限定条件时，
			 * _bt_first() 必须用它们做初始定位，这一点至关重要。见
			 * _bt_first() 中的注释。
			 */
			if ((key->sk_flags & SK_BT_REQFWD) &&
				ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((key->sk_flags & SK_BT_REQBKWD) &&
					 ScanDirectionIsBackward(dir))
				*continuescan = false;

			/*
			 * 无论如何，此索引元组不匹配限定条件。
			 */
			return false;
		}
	}

	/* 若到达这里，元组通过了全部索引限定条件。 */
	return true;
}

/*
 * 测试索引元组是否满足行比较扫描条件。
 *
 * 满足则返回 true，否则返回 false。若不满足，且当前扫描方向上
 * 不可能再有后续元组
 * 通过该限定条件，则同时清除 *continuescan。
 *
 * 这是 _bt_checkkeys 的子例程，更多信息见该函数。
 */
static bool
_bt_check_rowcompare(ScanKey skey, IndexTuple tuple, int tupnatts,
					 TupleDesc tupdesc, ScanDirection dir, bool *continuescan)
{
	ScanKey		subkey = (ScanKey) DatumGetPointer(skey->sk_argument);
	int32		cmpresult = 0;
	bool		result;

	/* 第一个子键应当与头所说的一致 */
	Assert(subkey->sk_attno == skey->sk_attno);

	/* 循环遍历行条件的各列 */
	for (;;)
	{
		Datum		datum;
		bool		isNull;

		Assert(subkey->sk_flags & SK_ROW_MEMBER);

		if (subkey->sk_attno > tupnatts)
		{
			/*
			 * 该属性已被截断（必定是高键）。右边那一页上
			 * 第一个非枢轴元组中该属性的值
			 * 可以是任何可能的值。假定被截断的
			 * 属性通过限定条件。
			 */
			Assert(ScanDirectionIsForward(dir));
			Assert(BTreeTupleIsPivot(tuple));
			cmpresult = 0;
			if (subkey->sk_flags & SK_ROW_END)
				break;
			subkey++;
			continue;
		}

		datum = index_getattr(tuple,
							  subkey->sk_attno,
							  tupdesc,
							  &isNull);

		if (isNull)
		{
			if (subkey->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
				 * 由于 NULLs 排在非 NULL 之前，可知已经
				 * 到达此索引属性取值范围的下界。
				 * 在反向扫描中，若此限定条件
				 * 属于"必须匹配"的子集，就可以停止。只要它是必需的，
				 * 无论限定条件是 > 还是 < 都可以停止，
				 * 因为后面不可能再有元组通过。在
				 * 正向扫描中则必须继续，因为我们可能
				 * 最初定位在索引的起点。
				 */
				if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
				 * 由于 NULLs 排在非 NULL 之后，可知已经
				 * 到达此索引属性取值范围的上界。
				 * 在正向扫描中，若此限定条件
				 * 属于"必须匹配"的子集，就可以停止。只要它是必需的，
				 * 无论限定条件是 > 还是 < 都可以停止，
				 * 因为后面不可能再有元组通过。在
				 * 反向扫描中则必须继续，因为我们
				 * 可能最初定位在索引的终点。
				 */
				if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * 无论如何，此索引元组不匹配限定条件。
			 */
			return false;
		}

		if (subkey->sk_flags & SK_ISNULL)
		{
			/*
			 * 与简单扫描键的情形不同，这并非不允许的情况。
			 * 但它永远无法匹配。若该行比较中更早的
			 * 列对扫描方向都是必需的，就可以停止
			 * 扫描，因为不可能再有另一个元组会成功。
			 */
			if (subkey != (ScanKey) DatumGetPointer(skey->sk_argument))
				subkey--;
			if ((subkey->sk_flags & SK_BT_REQFWD) &&
				ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
					 ScanDirectionIsBackward(dir))
				*continuescan = false;
			return false;
		}

		/* 执行测试 --- 三路比较，不是布尔操作符 */
		cmpresult = DatumGetInt32(FunctionCall2Coll(&subkey->sk_func,
													subkey->sk_collation,
													datum,
													subkey->sk_argument));

		if (subkey->sk_flags & SK_BT_DESC)
			INVERT_COMPARE_RESULT(cmpresult);

		/* 若不相等则比较结束，否则前进到下一列 */
		if (cmpresult != 0)
			break;

		if (subkey->sk_flags & SK_ROW_END)
			break;
		subkey++;
	}

	/*
	 * 至此 cmpresult 表示行比较的总体结果，
	 * subkey 指向起决定作用的列（若结果为 "="，则指向
	 * 最后一列）。
	 */
	switch (subkey->sk_strategy)
	{
			/* 这里不允许 EQ 与 NE 情形 */
		case BTLessStrategyNumber:
			result = (cmpresult < 0);
			break;
		case BTLessEqualStrategyNumber:
			result = (cmpresult <= 0);
			break;
		case BTGreaterEqualStrategyNumber:
			result = (cmpresult >= 0);
			break;
		case BTGreaterStrategyNumber:
			result = (cmpresult > 0);
			break;
		default:
			elog(ERROR, "unrecognized RowCompareType: %d",
				 (int) subkey->sk_strategy);
			result = 0;			/* 让编译器保持安静 */
			break;
	}

	if (!result)
	{
		/*
		 * 元组未通过此限定条件。若它对当前
		 * 扫描方向是必需限定条件，则可以断定后面也不会再有元组通过。
		 * 注意必须看起决定作用的列，而不一定是
		 * 行条件的第一列或最后一列。
		 */
		if ((subkey->sk_flags & SK_BT_REQFWD) &&
			ScanDirectionIsForward(dir))
			*continuescan = false;
		else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
				 ScanDirectionIsBackward(dir))
			*continuescan = false;
	}

	return result;
}

/*
 * _bt_killitems - 为索引扫描调用者告知已被杀死的项
 * 设置 LP_DEAD 状态
 *
 * scan->opaque（本地经由 so 引用）含有关于
 * 当前页及其上被杀死元组的信息（一般而言，只有
 * so->numKilled > 0 时才应调用）。
 *
 * 调用者不应持有 so->currPos 页上的锁，但可以持有
 * 缓冲区钉住。返回时该页仍不会被加锁。它会继续
 * 持有调用之前所持有的那些钉住。
 *
 * 在假定它们就是该删除的项之前，我们按堆 TID 匹配。
 * 我们能应对项因插入而向右移动的情形。
 * 若某项因分裂而离开当前页，我们将找不到它
 * 并什么也不做（这不是错误 --- 我们假定该项
 * 最终会在以后的索引扫描中被标记）。
 *
 * 注意，若从最初读到这些项直到应用本函数，
 * 一直钉住目标页，则 VACUUM 无法从该页删除
 * 任何项，因此不必从记下的偏移向左搜索。
 * （这一观察也保证该项仍然是该删除的那一项；
 * 否则会有疑问，因为堆 TIDs 可能被回收。）
 * 即使该页已被插入和页分裂修改过，
 * 这一点仍然成立，因此不必查阅 LSN。
 *
 * 若读页之后释放了钉住，则重新读取它。若自读取以来
 * 它已被修改（由 LSN 判定），就不敢
 * 标记任何项，因为旧项可能已被清理
 * 掉，而 TID 被一个完全不同的堆元组重新使用。
 */
void
_bt_killitems(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	int			i;
	int			numKilled = so->numKilled;
	bool		killedsomething = false;
	bool		droppedpin;
	Buffer		buf;

	Assert(BTScanPosIsValid(so->currPos));

	/*
	 * 总是重置扫描状态，这样就不会在其他
	 * 页上寻找同样的项。
	 */
	so->numKilled = 0;

	if (BTScanPosIsPinned(so->currPos))
	{
		/*
		 * 自读取索引元组以来一直钉住这一页，
		 * 所以只需要给它加锁。钉住会防止
		 * 该页上任何 TID 被重新使用，因此不必检查
		 * LSN。
		 */
		droppedpin = false;
		buf = so->currPos.buf;
		_bt_lockbuf(scan->indexRelation, buf, BT_READ);
	}
	else
	{
		XLogRecPtr	latestlsn;

		droppedpin = true;
		/* 尝试重新读取缓冲区，取得钉住和锁。 */
		buf = _bt_getbuf(scan->indexRelation, so->currPos.currPage, BT_READ);

		latestlsn = BufferGetLSNAtomic(buf);
		Assert(so->currPos.lsn <= latestlsn);
		if (so->currPos.lsn != latestlsn)
		{
			/* 未钉住期间被修改，意味着做提示不安全。 */
			_bt_relbuf(scan->indexRelation, buf);
			return;
		}

		/* 未被修改，做提示是安全的 */
	}

	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	for (i = 0; i < numKilled; i++)
	{
		int			itemIndex = so->killedItems[i];
		BTScanPosItem *kitem = &so->currPos.items[itemIndex];
		OffsetNumber offnum = kitem->indexOffset;

		Assert(itemIndex >= so->currPos.firstItem &&
			   itemIndex <= so->currPos.lastItem);
		if (offnum < minoff)
			continue;			/* 纯属多虑 */
		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	ituple = (IndexTuple) PageGetItem(page, iid);
			bool		killtuple = false;

			if (BTreeTupleIsPosting(ituple))
			{
				int			pi = i + 1;
				int			nposting = BTreeTupleGetNPosting(ituple);
				int			j;

				/*
				 * 我们依赖这一约定：scanpos
				 * 项数组中的堆 TID 按堆 TID 升序存放，针对
				 * 最初来自同一个倒排列表
				 * 元组的一组 TIDs。即使在反向
				 * 扫描中也遵循这一约定，尽管按降序返回 TID 可能
				 * 显得更自然。这关乎效果，不是
				 * 正确性。
				 *
				 * 注意，自我们第一次读取以来，该页几乎可能以任何方式被修改
				 * （在 !droppedpin 的情形下），因此
				 * 这个倒排列表元组在我们第一次遇到它的堆 TID 时，
				 * 可能还不是倒排列表元组。
				 */
				for (j = 0; j < nposting; j++)
				{
					ItemPointer item = BTreeTupleGetPostingN(ituple, j);

					if (!ItemPointerEquals(item, &kitem->heapTid))
						break;	/* 跳出倒排列表循环 */

					/*
					 * 堆 TIDs 匹配时，kitem 的 offnum 也必须匹配，
					 * 不过只在页不可能被
					 * 并发修改的常见情形下如此
					 */
					Assert(kitem->indexOffset == offnum || !droppedpin);

					/*
					 * 在这里向前读后面的 kitem。
					 *
					 * 我们依赖这一假定：这里不前进 kitem，
					 * 就不会因为在下一次循环迭代中匹配不到它的下一个堆 TID，
					 * 而把倒排列表元组
					 * 视为完全死亡。
					 *
					 * 另一方面，若这是倒排列表元组中的
					 * 最后一个堆 TID，则无论如何都会杀死该元组
					 * （也就是说，我们正确处理了最后一个
					 * kitem 同时也是最后一个索引元组中最后一个堆 TID 的情形
					 * -- 倒排元组仍然会被杀死）。
					 */
					if (pi < numKilled)
						kitem = &so->currPos.items[so->killedItems[pi++]];
				}

				/*
				 * 不必前进最外层循环的 int 迭代器来
				 * 避免处理与同一个
				 * offnum/倒排列表元组相关的已杀死项。这种微优化几乎
				 * 不值得。 （最外层循环的后续迭代
				 * 会在这个倒排列表的第一个堆
				 * TID 上匹配失败，因此我们会很快前进到下一个 offnum/索引
				 * 元组。）
				 */
				if (j == nposting)
					killtuple = true;
			}
			else if (ItemPointerEquals(&ituple->t_tid, &kitem->heapTid))
				killtuple = true;

			/*
			 * 若索引项尚未死亡，则把它标为死亡。由于这
			 * 发生在可能以共享模式持有缓冲区锁时，
			 * 多个进程可能同时尝试这样做，
			 * 导致多个整页映像被发送
			 * 到 WAL（若启用了 wal_log_hints 或数据校验和），这
			 * 并不理想。
			 */
			if (killtuple && !ItemIdIsDead(iid))
			{
				/* 找到了该项/全部倒排列表项 */
				ItemIdMarkDead(iid);
				killedsomething = true;
				break;			/* 跳出内层搜索循环 */
			}
			offnum = OffsetNumberNext(offnum);
		}
	}

	/*
	 * 因为以后如有需要可以重做，所以标为脏提示。
	 *
	 * 每当把任何东西标为 LP_DEAD，也设置该页的
	 * BTP_HAS_GARBAGE 标志，它同样只是提示。（注意，我们
	 * 只在 !heapkeyspace 索引中依赖页级标志。）
	 */
	if (killedsomething)
	{
		opaque->btpo_flags |= BTP_HAS_GARBAGE;
		MarkBufferDirtyHint(buf, true);
	}

	if (!droppedpin)
		_bt_unlockbuf(scan->indexRelation, buf);
	else
		_bt_relbuf(scan->indexRelation, buf);
}


/*
 * 下面这些例程管理一块共享内存区，用来跟踪
 * 把"清理周期 ID"分配给当前活动的 btree 清理
 * 操作。有一个计数器，每次我们
 * 开始一次清理、为其分配周期 ID 时递增。由于多个清理可能
 * 并发活动，必须跟踪每一次活动
 * 清理的周期 ID；这最多需要 MaxBackends 项（通常少得多）。
 * 我们假定对给定索引最多只有一次清理可以活动。
 *
 * 对这块共享内存区的访问由 BtreeVacuumLock 控制。
 * 原则上可以为每个索引使用单独的 lmgr locktag，
 * 但单独一把 LWLock 便宜得多，而且鉴于
 * 锁被持有的时间很短，对并发的影响应当很小。
 */

typedef struct BTOneVacInfo
{
	LockRelId	relid;			/* 索引的全局标识符 */
	BTCycleId	cycleid;		/* 其活动 VACUUM 的周期 ID */
} BTOneVacInfo;

typedef struct BTVacInfo
{
	BTCycleId	cycle_ctr;		/* 最近分配的周期 ID */
	int			num_vacuums;	/* 当前活动的 VACUUMs 个数 */
	int			max_vacuums;	/* vacuums[] 数组的已分配长度 */
	BTOneVacInfo vacuums[FLEXIBLE_ARRAY_MEMBER];
} BTVacInfo;

static BTVacInfo *btvacinfo;


/*
 * _bt_vacuum_cycleid --- 取得索引的活动清理周期 ID，
 *		若没有活动的 VACUUM 则为零
 *
 * 注意：为了正确互锁，调用者必须已经对它将要
 * 写入周期 ID 的每个缓冲区持有钉住和
 * 排他锁。这样即使随后立刻开始一次 VACUUM，它也无法
 * 在页分裂完成之前处理那些页。
 */
BTCycleId
_bt_vacuum_cycleid(Relation rel)
{
	BTCycleId	result = 0;
	int			i;

	/* 这是只读操作，共享锁就够了 */
	LWLockAcquire(BtreeVacuumLock, LW_SHARED);

	for (i = 0; i < btvacinfo->num_vacuums; i++)
	{
		BTOneVacInfo *vac = &btvacinfo->vacuums[i];

		if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
			vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId)
		{
			result = vac->cycleid;
			break;
		}
	}

	LWLockRelease(BtreeVacuumLock);
	return result;
}

/*
 * _bt_start_vacuum --- 为刚开始的 VACUUM 操作分配周期 ID
 *
 * 注意：调用者必须保证最终会调用
 * _bt_end_vacuum，否则将永久泄漏一个数组槽。为确保
 * 即使在 elog(FATAL) 的情形下也会如此，合适的写法
 * 不只是 PG_TRY，而是
 *		PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel))
 */
BTCycleId
_bt_start_vacuum(Relation rel)
{
	BTCycleId	result;
	int			i;
	BTOneVacInfo *vac;

	LWLockAcquire(BtreeVacuumLock, LW_EXCLUSIVE);

	/*
	 * 分配下一个周期 ID，小心避开零以及
	 * 保留的高值。
	 */
	result = ++(btvacinfo->cycle_ctr);
	if (result == 0 || result > MAX_BT_CYCLE_ID)
		result = btvacinfo->cycle_ctr = 1;

	/* 确认此索引还没有条目 */
	for (i = 0; i < btvacinfo->num_vacuums; i++)
	{
		vac = &btvacinfo->vacuums[i];
		if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
			vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId)
		{
			/*
			 * 与后端中大多数地方不同，抛出错误之前必须显式
			 * 释放我们的 LWLock。这是因为
			 * 我们预期在事务
			 * 中止清理能够运行并释放 LWLocks 之前，就会调用 _bt_end_vacuum()。
			 */
			LWLockRelease(BtreeVacuumLock);
			elog(ERROR, "multiple active vacuums for index \"%s\"",
				 RelationGetRelationName(rel));
		}
	}

	/* 好，增加一个条目 */
	if (btvacinfo->num_vacuums >= btvacinfo->max_vacuums)
	{
		LWLockRelease(BtreeVacuumLock);
		elog(ERROR, "out of btvacinfo slots");
	}
	vac = &btvacinfo->vacuums[btvacinfo->num_vacuums];
	vac->relid = rel->rd_lockInfo.lockRelId;
	vac->cycleid = result;
	btvacinfo->num_vacuums++;

	LWLockRelease(BtreeVacuumLock);
	return result;
}

/*
 * _bt_end_vacuum --- 把一次 btree VACUUM 操作标为完成
 *
 * 注意：这里故意写成找不到条目也不抱怨；
 * 这样调用者可以把 PG_TRY 套在 start_vacuum 操作外面。
 */
void
_bt_end_vacuum(Relation rel)
{
	int			i;

	LWLockAcquire(BtreeVacuumLock, LW_EXCLUSIVE);

	/* 查找数组条目 */
	for (i = 0; i < btvacinfo->num_vacuums; i++)
	{
		BTOneVacInfo *vac = &btvacinfo->vacuums[i];

		if (vac->relid.relId == rel->rd_lockInfo.lockRelId.relId &&
			vac->relid.dbId == rel->rd_lockInfo.lockRelId.dbId)
		{
			/* 把最后一项下移来删掉它 */
			*vac = btvacinfo->vacuums[btvacinfo->num_vacuums - 1];
			btvacinfo->num_vacuums--;
			break;
		}
	}

	LWLockRelease(BtreeVacuumLock);
}

/*
 * 把 _bt_end_vacuum 包成 on_shmem_exit 回调函数
 */
void
_bt_end_vacuum_callback(int code, Datum arg)
{
	_bt_end_vacuum((Relation) DatumGetPointer(arg));
}

/*
 * BTreeShmemSize --- 报告所需的共享内存空间大小
 */
Size
BTreeShmemSize(void)
{
	Size		size;

	size = offsetof(BTVacInfo, vacuums);
	size = add_size(size, mul_size(MaxBackends, sizeof(BTOneVacInfo)));
	return size;
}

/*
 * BTreeShmemInit --- 初始化本模块的共享内存
 */
void
BTreeShmemInit(void)
{
	bool		found;

	btvacinfo = (BTVacInfo *) ShmemInitStruct("BTree Vacuum State",
											  BTreeShmemSize(),
											  &found);

	if (!IsUnderPostmaster)
	{
		/* 初始化共享内存区 */
		Assert(!found);

		/*
		 * 周期计数器从哪里开始其实无所谓，但
		 * 让它每次都从同一个值开始似乎不好。改用
		 * time() 的低位做种子。
		 */
		btvacinfo->cycle_ctr = (BTCycleId) time(NULL);

		btvacinfo->num_vacuums = 0;
		btvacinfo->max_vacuums = MaxBackends;
	}
	else
		Assert(found);
}

bytea *
btoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(BTOptions, fillfactor)},
		{"vacuum_cleanup_index_scale_factor", RELOPT_TYPE_REAL,
		offsetof(BTOptions, vacuum_cleanup_index_scale_factor)},
		{"deduplicate_items", RELOPT_TYPE_BOOL,
		offsetof(BTOptions, deduplicate_items)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_BTREE,
									  sizeof(BTOptions),
									  tab, lengthof(tab));
}

/*
 *	btproperty() -- 检查索引的布尔属性。
 *
 * 这是可选的，但在这里处理 AMPROP_RETURNABLE 可以省得打开关系
 * 去调用 btcanreturn。
 */
bool
btproperty(Oid index_oid, int attno,
		   IndexAMProperty prop, const char *propname,
		   bool *res, bool *isnull)
{
	switch (prop)
	{
		case AMPROP_RETURNABLE:
			/* 只回答列，不回答 AM 或整个索引 */
			if (attno == 0)
				return false;
			/* 否则，btree 总是可以返回数据 */
			*res = true;
			return true;

		default:
			return false;		/* 交给通用代码 */
	}
}

/*
 *	btbuildphasename() -- 返回索引构建阶段的名称。
 */
char *
btbuildphasename(int64 phasenum)
{
	switch (phasenum)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN:
			return "scanning table";
		case PROGRESS_BTREE_PHASE_PERFORMSORT_1:
			return "sorting live tuples";
		case PROGRESS_BTREE_PHASE_PERFORMSORT_2:
			return "sorting dead tuples";
		case PROGRESS_BTREE_PHASE_LEAF_LOAD:
			return "loading tuples in tree";
		default:
			return NULL;
	}
}

/*
 *	_bt_truncate() -- 建立去掉不需要的后缀属性的元组。
 *
 * 返回在调用者内存上下文中分配的截断枢轴索引元组，
 * 键属性从调用者的 firstright 参数复制。若 rel 是
 * INCLUDE 索引，非键属性一定会被截掉，
 * 因为它们不属于键空间。当可以明确返回的元组不
 * 需要一个或多个后缀键属性时，可以做更激进的后缀
 * 截断。只需要保留 firstright 的
 * 属性，直到并包括第一个与 lastleft 不相等的属性。
 * 用调用者的插入型扫描键比较这些元组；这里不考虑扫描键的
 * 参数值。
 *
 * 注意，返回元组的 t_tid 偏移将保存现存的属性
 * 个数，因此原来的项指针偏移不再表示。调用者
 * 只应修改截断元组的下行链接。还要注意，截断的
 * 键属性被 _bt_compare()
 * 当作含有"负无穷"值。
 *
 * 最坏情况下（必须追加堆 TID 才能区分 lastleft
 * 与 firstright），返回元组的大小是 firstright 的大小
 * 再加上一个额外的、经 MAXALIGN() 对齐的项指针的大小。这一保证
 * 很重要，因为调用者需要遵守元组大小不超过页的 1/3
 * 这一限制。若本例程将来学会在
 * 属性/datum 内部截断，当截断加上 TOAST 压缩反而放大
 * 最终 datum 时，它必须避免向调用者返回变大的
 * 元组。
 */
IndexTuple
_bt_truncate(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			 BTScanInsert itup_key)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int16		nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	int			keepnatts;
	IndexTuple	pivot;
	IndexTuple	tidpivot;
	ItemPointer pivotheaptid;
	Size		newsize;

	/*
	 * 只应截断来自叶子页的非枢轴元组。分裂
	 * 内部页时绝不可以截断。
	 */
	Assert(!BTreeTupleIsPivot(lastleft) && !BTreeTupleIsPivot(firstright));

	/* 确定截断后的元组必须保留多少属性 */
	keepnatts = _bt_keep_natts(rel, lastleft, firstright, itup_key);

#ifdef DEBUG_NO_TRUNCATE
	/* 为测试起见，强制截断无效 */
	keepnatts = nkeyatts + 1;
#endif

	pivot = index_truncate_tuple(itupdesc, firstright,
								 Min(keepnatts, nkeyatts));

	if (BTreeTupleIsPosting(pivot))
	{
		/*
		 * 当没有属性可截断时，index_truncate_tuple() 只是原样返回 firstright
		 * 的一份直接副本。发生这种情况时，我们可能
		 * 需要在这里改而截掉倒排列表。
		 */
		Assert(keepnatts == nkeyatts || keepnatts == nkeyatts + 1);
		Assert(IndexRelationGetNumberOfAttributes(rel) == nkeyatts);
		pivot->t_info &= ~INDEX_SIZE_MASK;
		pivot->t_info |= MAXALIGN(BTreeTupleGetPostingOffset(firstright));
	}

	/*
	 * 若枢轴元组内有一个能区分的键属性，就
	 * 完成了
	 */
	if (keepnatts <= nkeyatts)
	{
		BTreeTupleSetNAtts(pivot, keepnatts, false);
		return pivot;
	}

	/*
	 * 必须在新枢轴元组中存放堆 TID，因为 firstright 中没有任何非 TID
	 * 键属性值能把分裂的右侧与
	 * 左侧区分开。nbtree 把这种情况理解为
	 * 无法截掉任何键属性，因为堆 TID 被
	 * 当作又一个键属性（尽管没有 pg_attribute
	 * 条目）。
	 *
	 * 使用能放下 pivot 副本的扩大空间。需要这块额外空间
	 * 在末尾存放堆 TID（使用特殊的枢轴元组
	 * 表示）。注意，此时原来的 pivot 已经去掉了 firstright 的
	 * 可能存在的倒排列表/非键属性值。
	 */
	newsize = MAXALIGN(IndexTupleSize(pivot)) + MAXALIGN(sizeof(ItemPointerData));
	tidpivot = palloc0(newsize);
	memcpy(tidpivot, pivot, MAXALIGN(IndexTupleSize(pivot)));
	/* 这里不得泄漏内存 */
	pfree(pivot);

	/*
	 * 在扩大后的枢轴元组中存放 firstright 的全部键属性值，外加一个决胜用的堆
	 * TID 值
	 */
	tidpivot->t_info &= ~INDEX_SIZE_MASK;
	tidpivot->t_info |= newsize;
	BTreeTupleSetNAtts(tidpivot, nkeyatts, true);
	pivotheaptid = BTreeTupleGetHeapTID(tidpivot);

	/*
	 * Lehman 与 Yao 在所有情况下都用 lastleft 作为叶子高键，但不
	 * 考虑后缀截断。在不发生截断的情况下沿用那个
	 * 例子似乎是个好主意 -- 使用 lastleft 的堆
	 * TID。（这也是合法可用的、最接近负无穷的
	 * 值。）
	 */
	ItemPointerCopy(BTreeTupleGetMaxHeapTID(lastleft), pivotheaptid);

	/*
	 * 完成了。返回前用 Assert() 确认堆 TID 不变量成立。
	 *
	 * Lehman 与 Yao 需要：在页分裂第二阶段插入父页的、
	 * 指向右页的下行链接，必须是
	 * 右页上各项的严格下界，并且是
	 * 左页上各项的非严格上界。断言堆 TIDs 遵循这些
	 * 不变量，因为显然需要一个堆 TID 值作为
	 * 决胜属性。
	 */
#ifndef DEBUG_NO_TRUNCATE
	Assert(ItemPointerCompare(BTreeTupleGetMaxHeapTID(lastleft),
							  BTreeTupleGetHeapTID(firstright)) < 0);
	Assert(ItemPointerCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(lastleft)) >= 0);
	Assert(ItemPointerCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(firstright)) < 0);
#else

	/*
	 * 当 lastleft 与 firstright 的堆 TID 属性值在这里被考虑，
	 * 仅仅是因为定义了
	 * DEBUG_NO_TRUNCATE 时（堆 TID 很可能实际上并不
	 * 需要作为决胜属性），那些不变量并不保证成立。因此 DEBUG_NO_TRUNCATE 必须使用一个
	 * 始终能作为右侧各项严格下界的堆
	 * TID 值。特别是，当 lastleft 的
	 * TID 恰好大于 firstright 的 TID 时，它必须避免把 firstright 的前导键
	 * 属性值与 lastleft 的堆 TID 值一起使用。
	 */
	ItemPointerCopy(BTreeTupleGetHeapTID(firstright), pivotheaptid);

	/*
	 * 枢轴堆 TID 绝不应当与 firstright 完全相等。注意，
	 * 当那是唯一可用的值时，枢轴堆 TID
	 * 仍会等于 lastleft 的堆 TID。
	 */
	ItemPointerSetOffsetNumber(pivotheaptid,
							   OffsetNumberPrev(ItemPointerGetOffsetNumber(pivotheaptid)));
	Assert(ItemPointerCompare(pivotheaptid,
							  BTreeTupleGetHeapTID(firstright)) < 0);
#endif

	return tidpivot;
}

/*
 * _bt_keep_natts - 截断时要保留多少键属性。
 *
 * 调用者提供围住分裂点的两个元组。用调用者的插入型
 * 扫描键比较这些元组；扫描键的参数值
 * 在这里不予考虑。
 *
 * 返回的属性个数可以比
 * 索引关系的键属性个数大一。这表示
 * 调用者必须在新枢轴元组中用堆 TID 作为唯一化属性。
 */
static int
_bt_keep_natts(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			   BTScanInsert itup_key)
{
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			keepnatts;
	ScanKey		scankey;

	/*
	 * _bt_compare() 把截断的键属性当作负
	 * 无穷，这会破坏 !heapkeyspace 索引内的搜索。不过
	 * 仍然必须截掉非键属性值。
	 */
	if (!itup_key->heapkeyspace)
		return nkeyatts;

	scankey = itup_key->scankeys;
	keepnatts = 1;
	for (int attnum = 1; attnum <= nkeyatts; attnum++, scankey++)
	{
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);

		if (isNull1 != isNull2)
			break;

		if (!isNull1 &&
			DatumGetInt32(FunctionCall2Coll(&scankey->sk_func,
											scankey->sk_collation,
											datum1,
											datum2)) != 0)
			break;

		keepnatts++;
	}

	/*
	 * 断言 _bt_keep_natts_fast() 在顺便检查时与我们一致。在
	 * allequalimage 索引中这是预期的。
	 */
	Assert(!itup_key->allequalimage ||
		   keepnatts == _bt_keep_natts_fast(rel, lastleft, firstright));

	return keepnatts;
}

/*
 * _bt_keep_natts_fast - _bt_keep_natts 的快速按位变体。
 *
 * 导出它，是为了在寻找
 * 分裂位置时，能事先低成本地评估候选分裂点对
 * 后缀截断的影响。对 datum 比较采用朴素的按位方法，以
 * 节省周期。
 *
 * 这里采用的方法通常会给出与 _bt_keep_natts
 * 相同的答案（对来自 heapkeyspace 索引的同一对元组），因为
 * 大多数 btree 操作符类绝不会表示两个 datum 相等，
 * 除非它们在解除 TOAST 之后按位相等。当索引只有
 * "相等映像"列时，本例程保证给出与
 * _bt_keep_natts 相同的结果。
 *
 * 调用者可以依赖：这里视为相等的属性，
 * 按 _bt_keep_natts 也一定相等，即使索引使用的
 * 操作符类或排序规则并不是 "allequalimage"/可安全去重的。
 * 这一较弱的保证对 nbtsplitloc.c 的调用者已经够用，因为假
 * 阴性一般只会使叶子页分裂采用
 * 更均衡的分裂点。
 */
int
_bt_keep_natts_fast(Relation rel, IndexTuple lastleft, IndexTuple firstright)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			keysz = IndexRelationGetNumberOfKeyAttributes(rel);
	int			keepnatts;

	keepnatts = 1;
	for (int attnum = 1; attnum <= keysz; attnum++)
	{
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		Form_pg_attribute att;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);
		att = TupleDescAttr(itupdesc, attnum - 1);

		if (isNull1 != isNull2)
			break;

		if (!isNull1 &&
			!datum_image_eq(datum1, datum2, att->attbyval, att->attlen))
			break;

		keepnatts++;
	}

	return keepnatts;
}

/*
 *  _bt_check_natts() -- 验证元组的属性个数符合预期。
 *
 * 返回值表示在页上某个偏移处是否找到了预期的属性个数。
 * 这可以用作通用的
 * 健全性检查。
 *
 * 直接用 BTreeTupleGetNAtts() 测试元组通常
 * 比调用这里更好。那通常更方便，而且总是
 * 更明确。当 offnum 处的元组可能是使用 v11 之前
 * 磁盘表示的负无穷元组，或适合做低上下文
 * 检查时，再调用这里。本例程对 btree 各个版本上
 * 预期出现的内容尽可能严格。
 */
bool
_bt_check_natts(Relation rel, bool heapkeyspace, Page page, OffsetNumber offnum)
{
	int16		natts = IndexRelationGetNumberOfAttributes(rel);
	int16		nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	IndexTuple	itup;
	int			tupnatts;

	/*
	 * 无法可靠地检查已删除或半死的页，因为它们有
	 * 哑高键
	 */
	if (P_IGNORE(opaque))
		return true;

	Assert(offnum >= FirstOffsetNumber &&
		   offnum <= PageGetMaxOffsetNumber(page));

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	tupnatts = BTreeTupleGetNAtts(itup, rel);

	/* !heapkeyspace 索引不支持去重 */
	if (!heapkeyspace && BTreeTupleIsPosting(itup))
		return false;

	/* 倒排列表元组绝不应当设置"枢轴堆 TID"位 */
	if (BTreeTupleIsPosting(itup) &&
		(ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) &
		 BT_PIVOT_HEAP_TID_ATTR) != 0)
		return false;

	/* INCLUDE 索引不支持去重 */
	if (natts != nkeyatts && BTreeTupleIsPosting(itup))
		return false;

	if (P_ISLEAF(opaque))
	{
		if (offnum >= P_FIRSTDATAKEY(opaque))
		{
			/*
			 * 非枢轴元组绝不应当被显式标成枢轴
			 * 元组
			 */
			if (BTreeTupleIsPivot(itup))
				return false;

			/*
			 * 不是页高键的叶子元组（非枢轴元组）
			 * 绝不应当被截断。（注意 tupnatts 一定是
			 * 推断出来的，即使是倒排列表元组也如此，因为只有枢轴
			 * 元组直接存储 tupnatts。）
			 */
			return tupnatts == natts;
		}
		else
		{
			/*
			 * 最右页不含页高键，因此该元组已在
			 * 上面作为普通叶子元组检查过
			 */
			Assert(!P_RIGHTMOST(opaque));

			/*
			 * !heapkeyspace 高键元组只含键属性。注意
			 * 只有碰巧带有非键属性的 !heapkeyspace 索引，才会显式
			 * 表示 tupnatts。
			 */
			if (!heapkeyspace)
				return tupnatts == nkeyatts;

			/* 使用通用的 heapkeyspace 枢轴元组处理 */
		}
	}
	else						/* 非叶子 !P_ISLEAF(opaque) */
	{
		if (offnum == P_FIRSTDATAKEY(opaque))
		{
			/*
			 * 任何内部页上的第一个元组（可能是其高键之后的
			 * 第一个）是它的负无穷元组。负
			 * 无穷元组总是被截断到零个属性。它们
			 * 是一种特殊的枢轴元组。
			 */
			if (heapkeyspace)
				return tupnatts == 0;

			/*
			 * 若负无穷元组是在 v11 之前的 Postgres
			 * 版本所做的页分裂中生成的，属性个数
			 * 不会被显式表示。当显式表示
			 * 非零，或者没有显式表示而该
			 * 元组显然不是升级前的
			 * pg_upgrade 元组时，必定有问题。
			 *
			 * 在 v11 之前，下行链接的偏移总是 P_HIKEY。
			 * 把这一点当作有效
			 * !heapkeyspace 负无穷元组的另一种指示而接受。
			 */
			return tupnatts == 0 ||
				ItemPointerGetOffsetNumber(&(itup->t_tid)) == P_HIKEY;
		}
		else
		{
			/*
			 * 带分隔键的 !heapkeyspace 下行链接元组只含
			 * 键属性。注意，tupnatts 只会在碰巧
			 * 带有非键属性的 !heapkeyspace 索引中
			 * 被显式表示。
			 */
			if (!heapkeyspace)
				return tupnatts == nkeyatts;

			/* 使用通用的 heapkeyspace 枢轴元组处理 */
		}
	}

	/* 处理 heapkeyspace 枢轴元组（不包括负无穷项） */
	Assert(heapkeyspace);

	/*
	 * 对 heapkeyspace 索引的枢轴元组，属性个数的显式表示是强制的，
	 * 无论是否存在
	 * 非键属性。
	 */
	if (!BTreeTupleIsPivot(itup))
		return false;

	/* 枢轴元组不应使用倒排列表表示（多余） */
	if (BTreeTupleIsPosting(itup))
		return false;

	/*
	 * 堆 TID 是决胜键属性，因此当任何其他键属性被截断时，
	 * 它无法保持未截断
	 */
	if (BTreeTupleGetHeapTID(itup) != NULL && tupnatts != nkeyatts)
		return false;

	/*
	 * 枢轴元组必须至少有一个未截断的键属性（负
	 * 无穷枢轴元组是唯一的例外）。枢轴元组绝不能
	 * 表示某个键属性存在一个值，而该属性
	 * 超过了索引的 pg_index.indnkeyatts。
	 */
	return tupnatts > 0 && tupnatts <= nkeyatts;
}

/*
 *
 *  _bt_check_third_page() -- 检查元组究竟能否放进 btree 页。
 *
 * 实际上每一页都要能放下三项，因此把
 * 任何一项限制为每页可用空间的 1/3。注意 itemsz 不应
 * 包含 ItemId 开销。
 *
 * 在这里应用 TOAST 方法而不是抛出错误也许有用。
 * 不过，使用行外存储会打破后缀截断
 * 以及 contrib/amcheck 所做的假定。
 */
void
_bt_check_third_page(Relation rel, Relation heap, bool needheaptidspace,
					 Page page, IndexTuple newtup)
{
	Size		itemsz;
	BTPageOpaque opaque;

	itemsz = MAXALIGN(IndexTupleSize(newtup));

	/* 对照限制再检查一次项大小 */
	if (itemsz <= BTMaxItemSize(page))
		return;

	/*
	 * 元组很可能太大，放不进页，但索引有可能使用
	 * 版本 2 或版本 3，或者该页是内部页，
	 * 此时适用略高一些的限制。
	 */
	if (!needheaptidspace && itemsz <= BTMaxItemSizeNoHeapTid(page))
		return;

	/*
	 * 内部页插入不可能在这里失败，因为那将意味着
	 * 更早的、本应失败的叶子层插入没有失败
	 */
	opaque = BTPageGetOpaque(page);
	if (!P_ISLEAF(opaque))
		elog(ERROR, "cannot insert oversized tuple of size %zu on internal page of index \"%s\"",
			 itemsz, RelationGetRelationName(rel));

	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("index row size %zu exceeds btree version %u maximum %zu for index \"%s\"",
					itemsz,
					needheaptidspace ? BTREE_VERSION : BTREE_NOVAC_VERSION,
					needheaptidspace ? BTMaxItemSize(page) :
					BTMaxItemSizeNoHeapTid(page),
					RelationGetRelationName(rel)),
			 errdetail("Index row references tuple (%u,%u) in relation \"%s\".",
					   ItemPointerGetBlockNumber(BTreeTupleGetHeapTID(newtup)),
					   ItemPointerGetOffsetNumber(BTreeTupleGetHeapTID(newtup)),
					   RelationGetRelationName(heap)),
			 errhint("Values larger than 1/3 of a buffer page cannot be indexed.\n"
					 "Consider a function index of an MD5 hash of the value, "
					 "or use full text indexing."),
			 errtableconstraint(heap, RelationGetRelationName(rel))));
}

/*
 * rel 中的全部属性都是"相等即映像相等"属性吗？
 *
 * 使用每个属性的 BTEQUALIMAGE_PROC 操作符类过程。若任何
 * 操作符类缺少 BTEQUALIMAGE_PROC 过程或返回 false，我们
 * 返回 false；否则返回 true。
 *
 * 返回的布尔值在索引构建期间存入索引元页。
 * 只有我们返回 true 时才能使用去重。
 */
bool
_bt_allequalimage(Relation rel, bool debugmessage)
{
	bool		allequalimage = true;

	/* INCLUDE 索引永远无法支持去重 */
	if (IndexRelationGetNumberOfAttributes(rel) !=
		IndexRelationGetNumberOfKeyAttributes(rel))
		return false;

	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(rel); i++)
	{
		Oid			opfamily = rel->rd_opfamily[i];
		Oid			opcintype = rel->rd_opcintype[i];
		Oid			collation = rel->rd_indcollation[i];
		Oid			equalimageproc;

		equalimageproc = get_opfamily_proc(opfamily, opcintype, opcintype,
										   BTEQUALIMAGE_PROC);

		/*
		 * 若没有 BTEQUALIMAGE_PROC，则假定去重
		 * 不安全。否则，实际调用该过程并看它怎么说。
		 */
		if (!OidIsValid(equalimageproc) ||
			!DatumGetBool(OidFunctionCall1Coll(equalimageproc, collation,
											   ObjectIdGetDatum(opcintype))))
		{
			allequalimage = false;
			break;
		}
	}

	if (debugmessage)
	{
		if (allequalimage)
			elog(DEBUG1, "index \"%s\" can safely use deduplication",
				 RelationGetRelationName(rel));
		else
			elog(DEBUG1, "index \"%s\" cannot use deduplication",
				 RelationGetRelationName(rel));
	}

	return allequalimage;
}
