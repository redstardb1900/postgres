/*-------------------------------------------------------------------------
 *
 * ginscan.c
 *	  管理倒排索引关系扫描的例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginscan.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/relscan.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/rel.h"


IndexScanDesc
ginbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	GinScanOpaque so;

	/* 不允许 order by 操作符 */
	Assert(norderbys == 0);

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* 分配私有工作区 */
	so = (GinScanOpaque) palloc(sizeof(GinScanOpaqueData));
	so->keys = NULL;
	so->nkeys = 0;
	so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
										"Gin scan temporary context",
										ALLOCSET_DEFAULT_SIZES);
	so->keyCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Gin scan key context",
									   ALLOCSET_DEFAULT_SIZES);
	initGinState(&so->ginstate, scan->indexRelation);

	scan->opaque = so;

	return scan;
}

/*
 * 新建一个 GinScanEntry；若已有等价条目，
 * 就直接返回它
 */
static GinScanEntry
ginFillScanEntry(GinScanOpaque so, OffsetNumber attnum,
				 StrategyNumber strategy, int32 searchMode,
				 Datum queryKey, GinNullCategory queryCategory,
				 bool isPartialMatch, Pointer extra_data)
{
	GinState   *ginstate = &so->ginstate;
	GinScanEntry scanEntry;
	uint32		i;

	/*
	 * 查找已有的等价条目。
	 *
	 * extra_data 非空的条目绝不视为相同，因为
	 * 我们无法确切知道操作符类会拿它做什么。
	 *
	 * 另外，条目达到 100 个后就放弃去重。以免
	 * 在很可能徒劳的大搜索键集合上去重时花费 O(N^2)
	 * 时间。阈值 100 是随意取的，但与
	 * predtest.c 判定何为大数组时所用的阈值一致。
	 */
	if (extra_data == NULL && so->totalentries < 100)
	{
		for (i = 0; i < so->totalentries; i++)
		{
			GinScanEntry prevEntry = so->entries[i];

			if (prevEntry->extra_data == NULL &&
				prevEntry->isPartialMatch == isPartialMatch &&
				prevEntry->strategy == strategy &&
				prevEntry->searchMode == searchMode &&
				prevEntry->attnum == attnum &&
				ginCompareEntries(ginstate, attnum,
								  prevEntry->queryKey,
								  prevEntry->queryCategory,
								  queryKey,
								  queryCategory) == 0)
			{
				/* 匹配成功 */
				return prevEntry;
			}
		}
	}

	/* 没有，新建一个条目 */
	scanEntry = (GinScanEntry) palloc(sizeof(GinScanEntryData));
	scanEntry->queryKey = queryKey;
	scanEntry->queryCategory = queryCategory;
	scanEntry->isPartialMatch = isPartialMatch;
	scanEntry->extra_data = extra_data;
	scanEntry->strategy = strategy;
	scanEntry->searchMode = searchMode;
	scanEntry->attnum = attnum;

	scanEntry->buffer = InvalidBuffer;
	ItemPointerSetMin(&scanEntry->curItem);
	scanEntry->matchBitmap = NULL;
	scanEntry->matchIterator = NULL;
	scanEntry->matchResult = NULL;
	scanEntry->list = NULL;
	scanEntry->nlist = 0;
	scanEntry->offset = InvalidOffsetNumber;
	scanEntry->isFinished = false;
	scanEntry->reduceResult = false;

	/* 把它加入 so 的数组 */
	if (so->totalentries >= so->allocentries)
	{
		so->allocentries *= 2;
		so->entries = (GinScanEntry *)
			repalloc(so->entries, so->allocentries * sizeof(GinScanEntry));
	}
	so->entries[so->totalentries++] = scanEntry;

	return scanEntry;
}

/*
 * 向扫描键追加给定类别的隐藏扫描条目。
 *
 * NB: 每个扫描键最好至多调用一次，因为
 * ginFillScanKey 只为一条隐藏条目留出位置。目前
 * 这一点看起来已经足够清楚，所以我们不必
 * 再做任何交叉检查。
 */
static void
ginScanKeyAddHiddenEntry(GinScanOpaque so, GinScanKey key,
						 GinNullCategory queryCategory)
{
	int			i = key->nentries++;

	/* 策略无关紧要，因为这不是部分匹配项 */
	key->scanEntry[i] = ginFillScanEntry(so, key->attnum,
										 InvalidStrategy, key->searchMode,
										 (Datum) 0, queryCategory,
										 false, NULL);
}

/*
 * 用 extractQueryFn 的输出初始化下一个 GinScanKey
 */
static void
ginFillScanKey(GinScanOpaque so, OffsetNumber attnum,
			   StrategyNumber strategy, int32 searchMode,
			   Datum query, uint32 nQueryValues,
			   Datum *queryValues, GinNullCategory *queryCategories,
			   bool *partial_matches, Pointer *extra_data)
{
	GinScanKey	key = &(so->keys[so->nkeys++]);
	GinState   *ginstate = &so->ginstate;
	uint32		i;

	key->nentries = nQueryValues;
	key->nuserentries = nQueryValues;

	/* 为可能的 "隐藏" 条目多分配一个数组槽 */
	key->scanEntry = (GinScanEntry *) palloc(sizeof(GinScanEntry) *
											 (nQueryValues + 1));
	key->entryRes = (GinTernaryValue *) palloc0(sizeof(GinTernaryValue) *
												(nQueryValues + 1));

	key->query = query;
	key->queryValues = queryValues;
	key->queryCategories = queryCategories;
	key->extra_data = extra_data;
	key->strategy = strategy;
	key->searchMode = searchMode;
	key->attnum = attnum;

	/*
	 * 起初把 GIN_SEARCH_MODE_ALL 模式的扫描键标为
	 * excludeOnly。这一点稍后可能改变。
	 */
	key->excludeOnly = (searchMode == GIN_SEARCH_MODE_ALL);

	ItemPointerSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;
	key->nrequired = 0;
	key->nadditional = 0;
	key->requiredEntries = NULL;
	key->additionalEntries = NULL;

	ginInitConsistentFunction(ginstate, key);

	/* 用 extractQueryFn 的输出建立普通扫描条目 */
	for (i = 0; i < nQueryValues; i++)
	{
		Datum		queryKey;
		GinNullCategory queryCategory;
		bool		isPartialMatch;
		Pointer		this_extra;

		queryKey = queryValues[i];
		queryCategory = queryCategories[i];
		isPartialMatch =
			(ginstate->canPartialMatch[attnum - 1] && partial_matches)
			? partial_matches[i] : false;
		this_extra = (extra_data) ? extra_data[i] : NULL;

		key->scanEntry[i] = ginFillScanEntry(so, attnum,
											 strategy, searchMode,
											 queryKey, queryCategory,
											 isPartialMatch, this_extra);
	}

	/*
	 * 对 GIN_SEARCH_MODE_INCLUDE_EMPTY 和 GIN_SEARCH_MODE_EVERYTHING
	 * 搜索模式，立即加入 "隐藏" 条目。GIN_SEARCH_MODE_ALL
	 * 稍后处理，因为对它也许可以省去隐藏条目。
	 */
	if (searchMode == GIN_SEARCH_MODE_INCLUDE_EMPTY)
		ginScanKeyAddHiddenEntry(so, key, GIN_CAT_EMPTY_ITEM);
	else if (searchMode == GIN_SEARCH_MODE_EVERYTHING)
		ginScanKeyAddHiddenEntry(so, key, GIN_CAT_EMPTY_QUERY);
}

/*
 * 释放当前扫描键（若有）。
 */
void
ginFreeScanKeys(GinScanOpaque so)
{
	uint32		i;

	if (so->keys == NULL)
		return;

	for (i = 0; i < so->totalentries; i++)
	{
		GinScanEntry entry = so->entries[i];

		if (entry->buffer != InvalidBuffer)
			ReleaseBuffer(entry->buffer);
		if (entry->list)
			pfree(entry->list);
		if (entry->matchIterator)
			tbm_end_iterate(entry->matchIterator);
		if (entry->matchBitmap)
			tbm_free(entry->matchBitmap);
	}

	MemoryContextResetAndDeleteChildren(so->keyCtx);

	so->keys = NULL;
	so->nkeys = 0;
	so->entries = NULL;
	so->totalentries = 0;
}

void
ginNewScanKey(IndexScanDesc scan)
{
	ScanKey		scankey = scan->keyData;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	int			i;
	int			numExcludeOnly;
	bool		hasNullQuery = false;
	bool		attrHasNormalScan[INDEX_MAX_KEYS] = {false};
	MemoryContext oldCtx;

	/*
	 * 在键上下文中分配全部扫描键信息。（若
	 * extractQuery 在那里泄漏了什么，要到扫描结束或
	 * 重新扫描才会重置，不过这没问题。）
	 */
	oldCtx = MemoryContextSwitchTo(so->keyCtx);

	/* 若未提供扫描键，就额外分配一个 EVERYTHING GinScanKey */
	so->keys = (GinScanKey)
		palloc(Max(scan->numberOfKeys, 1) * sizeof(GinScanKeyData));
	so->nkeys = 0;

	/* 初始化可扩展的 GinScanEntry 指针数组 */
	so->totalentries = 0;
	so->allocentries = 32;
	so->entries = (GinScanEntry *)
		palloc(so->allocentries * sizeof(GinScanEntry));

	so->isVoidRes = false;

	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		skey = &scankey[i];
		Datum	   *queryValues;
		int32		nQueryValues = 0;
		bool	   *partial_matches = NULL;
		Pointer    *extra_data = NULL;
		bool	   *nullFlags = NULL;
		GinNullCategory *categories;
		int32		searchMode = GIN_SEARCH_MODE_DEFAULT;

		/*
		 * 我们假定 GIN 可索引的操作符是严格的，因此空查询
		 * 参数意味着无法满足的查询。
		 */
		if (skey->sk_flags & SK_ISNULL)
		{
			so->isVoidRes = true;
			break;
		}

		/* 可以调用 extractQueryFn 了 */
		queryValues = (Datum *)
			DatumGetPointer(FunctionCall7Coll(&so->ginstate.extractQueryFn[skey->sk_attno - 1],
											  so->ginstate.supportCollation[skey->sk_attno - 1],
											  skey->sk_argument,
											  PointerGetDatum(&nQueryValues),
											  UInt16GetDatum(skey->sk_strategy),
											  PointerGetDatum(&partial_matches),
											  PointerGetDatum(&extra_data),
											  PointerGetDatum(&nullFlags),
											  PointerGetDatum(&searchMode)));

		/*
		 * 若返回的 searchMode 不合理，就当作 GIN_SEARCH_MODE_ALL；特别
		 * 注意我们不允许 extractQueryFn 选择
		 * GIN_SEARCH_MODE_EVERYTHING。
		 */
		if (searchMode < GIN_SEARCH_MODE_DEFAULT ||
			searchMode > GIN_SEARCH_MODE_ALL)
			searchMode = GIN_SEARCH_MODE_ALL;

		/* 非默认模式需要索引中有占位项 */
		if (searchMode != GIN_SEARCH_MODE_DEFAULT)
			hasNullQuery = true;

		/*
		 * 默认模式下，没有键就意味着无法满足的查询。
		 */
		if (queryValues == NULL || nQueryValues <= 0)
		{
			if (searchMode == GIN_SEARCH_MODE_DEFAULT)
			{
				so->isVoidRes = true;
				break;
			}
			nQueryValues = 0;	/* 保证取值合理 */
		}

		/*
		 * 建立 GinNullCategory 表示。若 extractQueryFn
		 * 没有建立 nullFlags 数组，就假定一切都非空。
		 * 同时检测是否存在任何空键。
		 */
		categories = (GinNullCategory *) palloc0(nQueryValues * sizeof(GinNullCategory));
		if (nullFlags)
		{
			int32		j;

			for (j = 0; j < nQueryValues; j++)
			{
				if (nullFlags[j])
				{
					categories[j] = GIN_CAT_NULL_KEY;
					hasNullQuery = true;
				}
			}
		}

		ginFillScanKey(so, skey->sk_attno,
					   skey->sk_strategy, searchMode,
					   skey->sk_argument, nQueryValues,
					   queryValues, categories,
					   partial_matches, extra_data);

		/* 记住是否出现过任何非 excludeOnly 的键 */
		if (searchMode != GIN_SEARCH_MODE_ALL)
			attrHasNormalScan[skey->sk_attno - 1] = true;
	}

	/*
	 * 处理 GIN_SEARCH_MODE_ALL 扫描键时，需要再对扫描键做
	 * 一遍。上面已把每个这样的扫描键标为
	 * excludeOnly。若所涉列还有任何普通（非 excludeOnly）
	 * 扫描键，就可以保持原样。否则，必须有一个
	 * excludeOnly 扫描键接收 GIN_CAT_EMPTY_QUERY 隐藏条目，
	 * 并设为普通（excludeOnly = false）。
	 */
	numExcludeOnly = 0;
	for (i = 0; i < so->nkeys; i++)
	{
		GinScanKey	key = &so->keys[i];

		if (key->searchMode != GIN_SEARCH_MODE_ALL)
			continue;

		if (!attrHasNormalScan[key->attnum - 1])
		{
			key->excludeOnly = false;
			ginScanKeyAddHiddenEntry(so, key, GIN_CAT_EMPTY_QUERY);
			attrHasNormalScan[key->attnum - 1] = true;
		}
		else
			numExcludeOnly++;
	}

	/*
	 * 若有任何 excludeOnly 扫描键被原样留下，就把它们移到
	 * 扫描键数组末尾：它们必须出现在普通键 key(s) 之后。
	 */
	if (numExcludeOnly > 0)
	{
		GinScanKey	tmpkeys;
		int			iNormalKey;
		int			iExcludeOnly;

		/* 最好至少已经建立过一个普通键 */
		Assert(numExcludeOnly < so->nkeys);
		/* 做一个临时数组来存放重排后的扫描键 */
		tmpkeys = (GinScanKey) palloc(so->nkeys * sizeof(GinScanKeyData));
		/* 重排这些键 ... */
		iNormalKey = 0;
		iExcludeOnly = so->nkeys - numExcludeOnly;
		for (i = 0; i < so->nkeys; i++)
		{
			GinScanKey	key = &so->keys[i];

			if (key->excludeOnly)
			{
				memcpy(tmpkeys + iExcludeOnly, key, sizeof(GinScanKeyData));
				iExcludeOnly++;
			}
			else
			{
				memcpy(tmpkeys + iNormalKey, key, sizeof(GinScanKeyData));
				iNormalKey++;
			}
		}
		Assert(iNormalKey == so->nkeys - numExcludeOnly);
		Assert(iExcludeOnly == so->nkeys);
		/* ... 再复制回 so->keys[] */
		memcpy(so->keys, tmpkeys, so->nkeys * sizeof(GinScanKeyData));
		pfree(tmpkeys);
	}

	/*
	 * 若没有常规扫描键，就生成一个 EVERYTHING 扫描键来
	 * 驱动完整索引扫描。
	 */
	if (so->nkeys == 0 && !so->isVoidRes)
	{
		hasNullQuery = true;
		ginFillScanKey(so, FirstOffsetNumber,
					   InvalidStrategy, GIN_SEARCH_MODE_EVERYTHING,
					   (Datum) 0, 0,
					   NULL, NULL, NULL, NULL);
	}

	/*
	 * 若索引是版本 0，它可能缺少空条目和占位
	 * 条目，从而使空值搜索和完整索引扫描
	 * 不可靠。若是这样，则报错。
	 */
	if (hasNullQuery && !so->isVoidRes)
	{
		GinStatsData ginStats;

		ginGetStats(scan->indexRelation, &ginStats);
		if (ginStats.ginVersion < 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("old GIN indexes do not support whole-index scans nor searches for nulls"),
					 errhint("To fix this, do REINDEX INDEX \"%s\".",
							 RelationGetRelationName(scan->indexRelation))));
	}

	MemoryContextSwitchTo(oldCtx);

	pgstat_count_index_scan(scan->indexRelation);
}

void
ginrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		  ScanKey orderbys, int norderbys)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	ginFreeScanKeys(so);

	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}
}


void
ginendscan(IndexScanDesc scan)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	ginFreeScanKeys(so);

	MemoryContextDelete(so->tempCtx);
	MemoryContextDelete(so->keyCtx);

	pfree(so);
}
