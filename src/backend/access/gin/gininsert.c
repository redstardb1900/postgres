/*-------------------------------------------------------------------------
 *
 * gininsert.c
 *	  Postgres 倒排索引访问方法的插入例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/gininsert.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

typedef struct
{
	GinState	ginstate;
	double		indtuples;
	GinStatsData buildStats;
	MemoryContext tmpCtx;
	MemoryContext funcCtx;
	BuildAccumulator accum;
} GinBuildState;


/*
 * 把项指针数组加入元组的倒排列表；若
 * 空间不够，则创建倒排树以及指向该树的元组。
 * 元组的最大尺寸在
 * GinFormTuple() 中定义。返回一个新的、修改后的索引元组。
 * items[] 必须已排序且无重复。
 */
static IndexTuple
addItemPointersToLeafTuple(GinState *ginstate,
						   IndexTuple old,
						   ItemPointerData *items, uint32 nitem,
						   GinStatsData *buildStats, Buffer buffer)
{
	OffsetNumber attnum;
	Datum		key;
	GinNullCategory category;
	IndexTuple	res;
	ItemPointerData *newItems,
			   *oldItems;
	int			oldNPosting,
				newNPosting;
	GinPostingList *compressedList;

	Assert(!GinIsPostingTree(old));

	attnum = gintuple_get_attrnum(ginstate, old);
	key = gintuple_get_key(ginstate, old, &category);

	/* 合并新旧倒排列表 */
	oldItems = ginReadTuple(ginstate, attnum, old, &oldNPosting);

	newItems = ginMergeItemPointers(items, nitem,
									oldItems, oldNPosting,
									&newNPosting);

	/* 压缩倒排列表，并尝试构建一个放得下它的元组 */
	res = NULL;
	compressedList = ginCompressPostingList(newItems, newNPosting, GinMaxItemSize,
											NULL);
	pfree(newItems);
	if (compressedList)
	{
		res = GinFormTuple(ginstate, attnum, key, category,
						   (char *) compressedList,
						   SizeOfGinPostingList(compressedList),
						   newNPosting,
						   false);
		pfree(compressedList);
	}
	if (!res)
	{
		/* 倒排列表会太大，转换为倒排树 */
		BlockNumber postingRoot;

		/*
		 * 用旧元组的倒排列表初始化倒排树。它
		 * 肯定小到能放进一张倒排树页，并且应当
		 * 已经有序且无重复。
		 */
		postingRoot = createPostingTree(ginstate->index,
										oldItems,
										oldNPosting,
										buildStats,
										buffer);

		/* 现在把待加入的 TIDs 插入倒排树 */
		ginInsertItemPointers(ginstate->index, postingRoot,
							  items, nitem,
							  buildStats);

		/* 然后构建只含倒排树的新结果元组 */
		res = GinFormTuple(ginstate, attnum, key, category, NULL, 0, 0, true);
		GinSetPostingTree(res, postingRoot);
	}
	pfree(oldItems);

	return res;
}

/*
 * 构建一条新的叶子元组，采用倒排列表或倒排树格式，
 * 取决于给定 items 列表是否放得下。
 * items[] 必须已排序且无重复。
 *
 * 这与 addItemPointersToLeafTuple 中的逻辑基本相同，
 * 只是输入略有不同。
 */
static IndexTuple
buildFreshLeafTuple(GinState *ginstate,
					OffsetNumber attnum, Datum key, GinNullCategory category,
					ItemPointerData *items, uint32 nitem,
					GinStatsData *buildStats, Buffer buffer)
{
	IndexTuple	res = NULL;
	GinPostingList *compressedList;

	/* 尝试用全部项构建一个倒排列表元组 */
	compressedList = ginCompressPostingList(items, nitem, GinMaxItemSize, NULL);
	if (compressedList)
	{
		res = GinFormTuple(ginstate, attnum, key, category,
						   (char *) compressedList,
						   SizeOfGinPostingList(compressedList),
						   nitem, false);
		pfree(compressedList);
	}
	if (!res)
	{
		/* 倒排列表会太大，构建倒排树 */
		BlockNumber postingRoot;

		/*
		 * 构建只含倒排树的结果元组。先做这一步，以便
		 * 在键太大时尽快失败。
		 */
		res = GinFormTuple(ginstate, attnum, key, category, NULL, 0, 0, true);

		/*
		 * 用这些 TIDs 初始化一棵新的倒排树。
		 */
		postingRoot = createPostingTree(ginstate->index, items, nitem,
										buildStats, buffer);

		/* 然后把根链接存入结果元组 */
		GinSetPostingTree(res, postingRoot);
	}

	return res;
}

/*
 * 插入与给定键值关联的一个或多个堆 TIDs。
 * 这要么加入单条键条目，要么扩大一条已有条目。
 *
 * 索引构建期间，buildStats 非空，其中的计数器
 * 应按需递增。
 */
void
ginEntryInsert(GinState *ginstate,
			   OffsetNumber attnum, Datum key, GinNullCategory category,
			   ItemPointerData *items, uint32 nitem,
			   GinStatsData *buildStats)
{
	GinBtreeData btree;
	GinBtreeEntryInsertData insertdata;
	GinBtreeStack *stack;
	IndexTuple	itup;
	Page		page;

	insertdata.isDelete = false;

	ginPrepareEntryScan(&btree, attnum, key, category, ginstate);
	btree.isBuild = (buildStats != NULL);

	stack = ginFindLeafPage(&btree, false, false, NULL);
	page = BufferGetPage(stack->buffer);

	if (btree.findItem(&btree, stack))
	{
		/* 找到已有条目 */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, stack->off));

		if (GinIsPostingTree(itup))
		{
			/* 向已有倒排树加入条目 */
			BlockNumber rootPostingTree = GinGetPostingTree(itup);

			/* 释放整个栈 */
			LockBuffer(stack->buffer, GIN_UNLOCK);
			freeGinBtreeStack(stack);

			/* 插入到倒排树中 */
			ginInsertItemPointers(ginstate->index, rootPostingTree,
								  items, nitem,
								  buildStats);
			return;
		}

		CheckForSerializableConflictIn(ginstate->index, NULL,
									   BufferGetBlockNumber(stack->buffer));
		/* 修改已有的叶子条目 */
		itup = addItemPointersToLeafTuple(ginstate, itup,
										  items, nitem, buildStats, stack->buffer);

		insertdata.isDelete = true;
	}
	else
	{
		CheckForSerializableConflictIn(ginstate->index, NULL,
									   BufferGetBlockNumber(stack->buffer));
		/* 没有匹配，因此构造一条新的叶子条目 */
		itup = buildFreshLeafTuple(ginstate, attnum, key, category,
								   items, nitem, buildStats, stack->buffer);

		/*
		 * nEntries 统计叶子元组，因此只在新建
		 * 一条时才递增。
		 */
		if (buildStats)
			buildStats->nEntries++;
	}

	/* 插入新建或修改后的叶子元组 */
	insertdata.entry = itup;
	ginInsertValue(&btree, stack, &insertdata, buildStats);
	pfree(itup);
}

/*
 * 为单个可索引项提取索引条目，并加入
 * BuildAccumulator 的状态。
 *
 * 此函数只在最初创建索引时使用。
 */
static void
ginHeapTupleBulkInsert(GinBuildState *buildstate, OffsetNumber attnum,
					   Datum value, bool isNull,
					   ItemPointer heapptr)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		nentries;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(buildstate->funcCtx);
	entries = ginExtractEntries(buildstate->accum.ginstate, attnum,
								value, isNull,
								&nentries, &categories);
	MemoryContextSwitchTo(oldCtx);

	ginInsertBAEntries(&buildstate->accum, heapptr, attnum,
					   entries, categories, nentries);

	buildstate->indtuples += nentries;

	MemoryContextReset(buildstate->funcCtx);
}

static void
ginBuildCallback(Relation index, ItemPointer tid, Datum *values,
				 bool *isnull, bool tupleIsAlive, void *state)
{
	GinBuildState *buildstate = (GinBuildState *) state;
	MemoryContext oldCtx;
	int			i;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	for (i = 0; i < buildstate->ginstate.origTupdesc->natts; i++)
		ginHeapTupleBulkInsert(buildstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i], tid);

	/* 若可用内存已经用尽，就把全部内容写入索引 */
	if (buildstate->accum.allocatedMemory >= (Size) maintenance_work_mem * 1024L)
	{
		ItemPointerData *list;
		Datum		key;
		GinNullCategory category;
		uint32		nlist;
		OffsetNumber attnum;

		ginBeginBAScan(&buildstate->accum);
		while ((list = ginGetBAEntry(&buildstate->accum,
									 &attnum, &key, &category, &nlist)) != NULL)
		{
			/* 条目可能很多，因此这里愿意中止 */
			CHECK_FOR_INTERRUPTS();
			ginEntryInsert(&buildstate->ginstate, attnum, key, category,
						   list, nlist, &buildstate->buildStats);
		}

		MemoryContextReset(buildstate->tmpCtx);
		ginInitBA(&buildstate->accum);
	}

	MemoryContextSwitchTo(oldCtx);
}

IndexBuildResult *
ginbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	GinBuildState buildstate;
	Buffer		RootBuffer,
				MetaBuffer;
	ItemPointerData *list;
	Datum		key;
	GinNullCategory category;
	uint32		nlist;
	MemoryContext oldCtx;
	OffsetNumber attnum;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	initGinState(&buildstate.ginstate, index);
	buildstate.indtuples = 0;
	memset(&buildstate.buildStats, 0, sizeof(GinStatsData));

	/* 初始化元页 */
	MetaBuffer = GinNewBuffer(index);

	/* 初始化根页 */
	RootBuffer = GinNewBuffer(index);

	START_CRIT_SECTION();
	GinInitMetabuffer(MetaBuffer);
	MarkBufferDirty(MetaBuffer);
	GinInitBuffer(RootBuffer, GIN_LEAF);
	MarkBufferDirty(RootBuffer);


	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
	END_CRIT_SECTION();

	/* 把根页计为第一张条目页 */
	buildstate.buildStats.nEntryPages++;

	/*
	 * 创建一个临时内存上下文，用来保存尚未
	 * 写入索引的数据
	 */
	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "Gin build temporary context",
											  ALLOCSET_DEFAULT_SIZES);

	/*
	 * 创建一个临时内存上下文，用于调用
	 * ginExtractEntries()，并可在每个元组之后重置
	 */
	buildstate.funcCtx = AllocSetContextCreate(CurrentMemoryContext,
											   "Gin build temporary context for user-defined function",
											   ALLOCSET_DEFAULT_SIZES);

	buildstate.accum.ginstate = &buildstate.ginstate;
	ginInitBA(&buildstate.accum);

	/*
	 * 执行堆扫描。这里不允许同步扫描，因为 dataPlaceToPage
	 * 更希望按 TID 顺序接收元组。
	 */
	reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
									   ginBuildCallback, (void *) &buildstate,
									   NULL);

	/* 把剩余条目写入索引 */
	oldCtx = MemoryContextSwitchTo(buildstate.tmpCtx);
	ginBeginBAScan(&buildstate.accum);
	while ((list = ginGetBAEntry(&buildstate.accum,
								 &attnum, &key, &category, &nlist)) != NULL)
	{
		/* 条目可能很多，因此这里愿意中止 */
		CHECK_FOR_INTERRUPTS();
		ginEntryInsert(&buildstate.ginstate, attnum, key, category,
					   list, nlist, &buildstate.buildStats);
	}
	MemoryContextSwitchTo(oldCtx);

	MemoryContextDelete(buildstate.funcCtx);
	MemoryContextDelete(buildstate.tmpCtx);

	/*
	 * 更新元页统计信息
	 */
	buildstate.buildStats.nTotalPages = RelationGetNumberOfBlocks(index);
	ginUpdateStats(index, &buildstate.buildStats, true);

	/*
	 * 构建索引时没有写 WAL 记录，因此若需要记录 WAL，
	 * 现在就把所有页写入 WAL。
	 */
	if (RelationNeedsWAL(index))
	{
		log_newpage_range(index, MAIN_FORKNUM,
						  0, RelationGetNumberOfBlocks(index),
						  true);
	}

	/*
	 * 返回统计信息
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 *	ginbuildempty() -- 在初始化分支中构建一个空的 GIN 索引
 */
void
ginbuildempty(Relation index)
{
	Buffer		RootBuffer,
				MetaBuffer;

	/* 空的 GIN 索引有两页。 */
	MetaBuffer = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								   EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	RootBuffer = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								   EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);

	/* 初始化元页缓冲区和根缓冲区，并写入 WAL。 */
	START_CRIT_SECTION();
	GinInitMetabuffer(MetaBuffer);
	MarkBufferDirty(MetaBuffer);
	log_newpage_buffer(MetaBuffer, true);
	GinInitBuffer(RootBuffer, GIN_LEAF);
	MarkBufferDirty(RootBuffer);
	log_newpage_buffer(RootBuffer, false);
	END_CRIT_SECTION();

	/* 解锁并释放这些缓冲区。 */
	UnlockReleaseBuffer(MetaBuffer);
	UnlockReleaseBuffer(RootBuffer);
}

/*
 * 在"常规"（非快速更新）插入期间，
 * 为单个可索引项插入索引条目
 */
static void
ginHeapTupleInsert(GinState *ginstate, OffsetNumber attnum,
				   Datum value, bool isNull,
				   ItemPointer item)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		i,
				nentries;

	entries = ginExtractEntries(ginstate, attnum, value, isNull,
								&nentries, &categories);

	for (i = 0; i < nentries; i++)
		ginEntryInsert(ginstate, attnum, entries[i], categories[i],
					   item, 1, NULL);
}

bool
gininsert(Relation index, Datum *values, bool *isnull,
		  ItemPointer ht_ctid, Relation heapRel,
		  IndexUniqueCheck checkUnique,
		  bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	GinState   *ginstate = (GinState *) indexInfo->ii_AmCache;
	MemoryContext oldCtx;
	MemoryContext insertCtx;
	int			i;

	/* 若本语句首次调用，则初始化 GinState 缓存 */
	if (ginstate == NULL)
	{
		oldCtx = MemoryContextSwitchTo(indexInfo->ii_Context);
		ginstate = (GinState *) palloc(sizeof(GinState));
		initGinState(ginstate, index);
		indexInfo->ii_AmCache = (void *) ginstate;
		MemoryContextSwitchTo(oldCtx);
	}

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Gin insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(insertCtx);

	if (GinGetUseFastUpdate(index))
	{
		GinTupleCollector collector;

		memset(&collector, 0, sizeof(GinTupleCollector));

		for (i = 0; i < ginstate->origTupdesc->natts; i++)
			ginHeapTupleFastCollect(ginstate, &collector,
									(OffsetNumber) (i + 1),
									values[i], isnull[i],
									ht_ctid);

		ginHeapTupleFastInsert(ginstate, &collector);
	}
	else
	{
		for (i = 0; i < ginstate->origTupdesc->natts; i++)
			ginHeapTupleInsert(ginstate, (OffsetNumber) (i + 1),
							   values[i], isnull[i],
							   ht_ctid);
	}

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}
