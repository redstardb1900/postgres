/*-------------------------------------------------------------------------
 *
 * ginlogic.c
 *	  执行二值逻辑与三值逻辑一致性检查的例程。
 *
 * GIN 操作符类可以提供布尔一致性函数或三值一致性
 * 函数，也可以两者都提供。即使操作符类只实现了
 * 其中一种，本文件也向其余 GIN 代码同时提供布尔
 * 接口和三值接口。
 *
 * 当操作符类只实现三值函数时，提供布尔接口
 * 很直接：按原样用检查数组调用该三值函数，
 * 并把返回码 GIN_TRUE、GIN_FALSE、GIN_MAYBE
 * 分别映射为 TRUE、FALSE 以及带重新检查标志的
 * TRUE。若操作符类只实现布尔函数，三值接口则
 * 通过多次调用该布尔函数来实现，把全部 MAYBE
 * 参数设为 TRUE 与 FALSE 的各种组合（MAYBE 参数
 * 个数不超过一定上限）。
 *
 * （布尔函数足以判定一项是否匹配。
 * 但若 GIN 扫描能判定一项匹配或不匹配，
 * 即使还不知道某些键是否存在，也可以施加
 * 各种优化。三值一致性函数就是用来做
 * 这件事的。）
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginlogic.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/reloptions.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"


/*
 * shimTriConsistentFn 试图通过全部组合来判定的
 * MAYBE 输入个数上限。
 */
#define MAX_MAYBE_ENTRIES	4

/*
 * EVERYTHING 键所用的恒真一致性函数。直接认定它匹配。
 */
static bool
trueConsistentFn(GinScanKey key)
{
	key->recheckCurItem = false;
	return true;
}
static GinTernaryValue
trueTriConsistentFn(GinScanKey key)
{
	return GIN_TRUE;
}

/*
 * 调用常规二值逻辑一致性函数的辅助函数。
 */
static bool
directBoolConsistentFn(GinScanKey key)
{
	/*
	 * 先初始化 recheckCurItem，以防 consistentFn 不知道
	 * 自己应当设置它。此时安全的假定是强制重新检查。
	 */
	key->recheckCurItem = true;

	return DatumGetBool(FunctionCall8Coll(key->consistentFmgrInfo,
										  key->collation,
										  PointerGetDatum(key->entryRes),
										  UInt16GetDatum(key->strategy),
										  key->query,
										  UInt32GetDatum(key->nuserentries),
										  PointerGetDatum(key->extra_data),
										  PointerGetDatum(&key->recheckCurItem),
										  PointerGetDatum(key->queryValues),
										  PointerGetDatum(key->queryCategories)));
}

/*
 * 调用原生三值逻辑一致性函数的辅助函数。
 */
static GinTernaryValue
directTriConsistentFn(GinScanKey key)
{
	return DatumGetGinTernaryValue(FunctionCall7Coll(key->triConsistentFmgrInfo,
													 key->collation,
													 PointerGetDatum(key->entryRes),
													 UInt16GetDatum(key->strategy),
													 key->query,
													 UInt32GetDatum(key->nuserentries),
													 PointerGetDatum(key->extra_data),
													 PointerGetDatum(key->queryValues),
													 PointerGetDatum(key->queryCategories)));
}

/*
 * 本函数用操作符类提供的三值逻辑一致性函数，实现
 * 二值逻辑一致性检查。返回值 GIN_MAYBE
 * 被解释为带重新检查标志的真。
 */
static bool
shimBoolConsistentFn(GinScanKey key)
{
	GinTernaryValue result;

	result = DatumGetGinTernaryValue(FunctionCall7Coll(key->triConsistentFmgrInfo,
													   key->collation,
													   PointerGetDatum(key->entryRes),
													   UInt16GetDatum(key->strategy),
													   key->query,
													   UInt32GetDatum(key->nuserentries),
													   PointerGetDatum(key->extra_data),
													   PointerGetDatum(key->queryValues),
													   PointerGetDatum(key->queryCategories)));
	if (result == GIN_MAYBE)
	{
		key->recheckCurItem = true;
		return true;
	}
	else
	{
		key->recheckCurItem = false;
		return result;
	}
}

/*
 * 本函数用操作符类提供的布尔一致性函数，实现
 * 三值一致性检查。
 *
 * 策略是调用 consistentFn，把 MAYBE 输入替换成每一种
 * TRUE/FALSE 组合。若 consistentFn 对每种组合返回的值
 * 都相同，那就是总结果。否则返回 MAYBE。测试
 * 全部组合的代价是 O(n^2)，因此只在 MAYBE 输入较少时
 * 才可行。
 *
 * NB: 本函数会修改 key->entryRes 数组。目前这样是可以的，
 * 只要在返回前恢复进入函数时的内容。将来若出现
 * 多线程 GIN 扫描，这一点可能需要重新考虑。
 */
static GinTernaryValue
shimTriConsistentFn(GinScanKey key)
{
	int			nmaybe;
	int			maybeEntries[MAX_MAYBE_ENTRIES];
	int			i;
	bool		boolResult;
	bool		recheck;
	GinTernaryValue curResult;

	/*
	 * 统计有多少个 MAYBE 输入，并把它们的下标存入
	 * maybeEntries。若 MAYBE 输入过多，测试全部组合
	 * 并不可行，于是放弃并返回 MAYBE。
	 */
	nmaybe = 0;
	for (i = 0; i < key->nentries; i++)
	{
		if (key->entryRes[i] == GIN_MAYBE)
		{
			if (nmaybe >= MAX_MAYBE_ENTRIES)
				return GIN_MAYBE;
			maybeEntries[nmaybe++] = i;
		}
	}

	/*
	 * 若没有任何输入是 MAYBE，就可以按原样调用一致性
	 * 函数。
	 */
	if (nmaybe == 0)
		return directBoolConsistentFn(key);

	/* 首先在全部 maybe 输入都设为 FALSE 时调用一致性函数 */
	for (i = 0; i < nmaybe; i++)
		key->entryRes[maybeEntries[i]] = GIN_FALSE;
	curResult = directBoolConsistentFn(key);
	recheck = key->recheckCurItem;

	for (;;)
	{
		/* 调整这些条目，得到下一种组合。 */
		for (i = 0; i < nmaybe; i++)
		{
			if (key->entryRes[maybeEntries[i]] == GIN_FALSE)
			{
				key->entryRes[maybeEntries[i]] = GIN_TRUE;
				break;
			}
			else
				key->entryRes[maybeEntries[i]] = GIN_FALSE;
		}
		if (i == nmaybe)
			break;

		boolResult = directBoolConsistentFn(key);
		recheck |= key->recheckCurItem;

		if (curResult != boolResult)
		{
			curResult = GIN_MAYBE;
			break;
		}
	}

	/* 带重新检查的 TRUE 视为 MAYBE */
	if (curResult == GIN_TRUE && recheck)
		curResult = GIN_MAYBE;

	/* 必须恢复 entryRes 数组的原始状态 */
	for (i = 0; i < nmaybe; i++)
		key->entryRes[maybeEntries[i]] = GIN_MAYBE;

	return curResult;
}

/*
 * 为扫描键设置一致性函数的实现。
 */
void
ginInitConsistentFunction(GinState *ginstate, GinScanKey key)
{
	if (key->searchMode == GIN_SEARCH_MODE_EVERYTHING)
	{
		key->boolConsistentFn = trueConsistentFn;
		key->triConsistentFn = trueTriConsistentFn;
	}
	else
	{
		key->consistentFmgrInfo = &ginstate->consistentFn[key->attnum - 1];
		key->triConsistentFmgrInfo = &ginstate->triConsistentFn[key->attnum - 1];
		key->collation = ginstate->supportCollation[key->attnum - 1];

		if (OidIsValid(ginstate->consistentFn[key->attnum - 1].fn_oid))
			key->boolConsistentFn = directBoolConsistentFn;
		else
			key->boolConsistentFn = shimBoolConsistentFn;

		if (OidIsValid(ginstate->triConsistentFn[key->attnum - 1].fn_oid))
			key->triConsistentFn = directTriConsistentFn;
		else
			key->triConsistentFn = shimTriConsistentFn;
	}
}
