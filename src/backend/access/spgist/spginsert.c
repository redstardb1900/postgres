/*-------------------------------------------------------------------------
 *
 * spginsert.c
 *	  对外可见的索引创建/插入例程
 *
 * 所有实际的插入逻辑都在 spgdoinsert.c 中。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spginsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/spgist_private.h"
#include "access/spgxlog.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"


typedef struct
{
	SpGistState spgstate;		/* SPGiST 的工作状态 */
	int64		indtuples;		/* 已编入索引的元组总数 */
	MemoryContext tmpCtx;		/* 逐元组临时上下文 */
} SpGistBuildState;


/* table_index_build_scan 期间处理一个堆元组的回调 */
static void
spgistBuildCallback(Relation index, ItemPointer tid, Datum *values,
					bool *isnull, bool tupleIsAlive, void *state)
{
	SpGistBuildState *buildstate = (SpGistBuildState *) state;
	MemoryContext oldCtx;

	/* 在临时上下文中工作，并在每个元组之后重置 */
	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/*
	 * 即便此时不会有并发插入发生，我们仍可能因
	 * bgwriter 或 checkpointer 对某个缓冲区加锁而遭遇
	 * 缓冲区加锁失败。因此需要愿意重试。重试时
	 * 可以冲刷掉任何临时数据。
	 */
	while (!spgdoinsert(index, &buildstate->spgstate, tid,
						values, isnull))
	{
		MemoryContextReset(buildstate->tmpCtx);
	}

	/* 更新元组总数 */
	buildstate->indtuples += 1;

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 * 构建一个 SP-GiST 索引。
 */
IndexBuildResult *
spgbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	SpGistBuildState buildstate;
	Buffer		metabuffer,
				rootbuffer,
				nullbuffer;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/*
	 * 初始化元页和根页
	 */
	metabuffer = SpGistNewBuffer(index);
	rootbuffer = SpGistNewBuffer(index);
	nullbuffer = SpGistNewBuffer(index);

	Assert(BufferGetBlockNumber(metabuffer) == SPGIST_METAPAGE_BLKNO);
	Assert(BufferGetBlockNumber(rootbuffer) == SPGIST_ROOT_BLKNO);
	Assert(BufferGetBlockNumber(nullbuffer) == SPGIST_NULL_BLKNO);

	START_CRIT_SECTION();

	SpGistInitMetapage(BufferGetPage(metabuffer));
	MarkBufferDirty(metabuffer);
	SpGistInitBuffer(rootbuffer, SPGIST_LEAF);
	MarkBufferDirty(rootbuffer);
	SpGistInitBuffer(nullbuffer, SPGIST_LEAF | SPGIST_NULLS);
	MarkBufferDirty(nullbuffer);


	END_CRIT_SECTION();

	UnlockReleaseBuffer(metabuffer);
	UnlockReleaseBuffer(rootbuffer);
	UnlockReleaseBuffer(nullbuffer);

	/*
	 * 现在把全部堆数据插入索引
	 */
	initSpGistState(&buildstate.spgstate, index);
	buildstate.spgstate.isBuild = true;
	buildstate.indtuples = 0;

	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "SP-GiST build temporary context",
											  ALLOCSET_DEFAULT_SIZES);

	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   spgistBuildCallback, (void *) &buildstate,
									   NULL);

	MemoryContextDelete(buildstate.tmpCtx);

	SpGistUpdateMetaPage(index);

	/*
	 * 构建索引时我们没有写 WAL 记录，因此若需要
	 * WAL 日志记录，现在把所有页写入 WAL。
	 */
	if (RelationNeedsWAL(index))
	{
		log_newpage_range(index, MAIN_FORKNUM,
						  0, RelationGetNumberOfBlocks(index),
						  true);
	}

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 * 在初始化分支中构建一个空的 SPGiST 索引
 */
void
spgbuildempty(Relation index)
{
	Buffer		metabuffer,
				rootbuffer,
				nullbuffer;

	/*
	 * 初始化元页和根页
	 */
	metabuffer = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(metabuffer, BUFFER_LOCK_EXCLUSIVE);
	rootbuffer = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(rootbuffer, BUFFER_LOCK_EXCLUSIVE);
	nullbuffer = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(nullbuffer, BUFFER_LOCK_EXCLUSIVE);

	Assert(BufferGetBlockNumber(metabuffer) == SPGIST_METAPAGE_BLKNO);
	Assert(BufferGetBlockNumber(rootbuffer) == SPGIST_ROOT_BLKNO);
	Assert(BufferGetBlockNumber(nullbuffer) == SPGIST_NULL_BLKNO);

	START_CRIT_SECTION();

	SpGistInitMetapage(BufferGetPage(metabuffer));
	MarkBufferDirty(metabuffer);
	SpGistInitBuffer(rootbuffer, SPGIST_LEAF);
	MarkBufferDirty(rootbuffer);
	SpGistInitBuffer(nullbuffer, SPGIST_LEAF | SPGIST_NULLS);
	MarkBufferDirty(nullbuffer);

	log_newpage_buffer(metabuffer, true);
	log_newpage_buffer(rootbuffer, true);
	log_newpage_buffer(nullbuffer, true);

	END_CRIT_SECTION();

	UnlockReleaseBuffer(metabuffer);
	UnlockReleaseBuffer(rootbuffer);
	UnlockReleaseBuffer(nullbuffer);
}

/*
 * 向 SPGiST 索引插入一个新元组。
 */
bool
spginsert(Relation index, Datum *values, bool *isnull,
		  ItemPointer ht_ctid, Relation heapRel,
		  IndexUniqueCheck checkUnique,
		  bool indexUnchanged,
		  IndexInfo *indexInfo)
{
	SpGistState spgstate;
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "SP-GiST insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);
	oldCtx = MemoryContextSwitchTo(insertCtx);

	initSpGistState(&spgstate, index);

	/*
	 * 若与并发插入发生冲突，我们可能需要多次重复
	 * 调用 spgdoinsert()。若如此，每次重置 insertCtx，
	 * 以避免内存累计消耗。那意味着也必须重新执行
	 * initSpGistState()，不过它足够便宜，无关紧要。
	 */
	while (!spgdoinsert(index, &spgstate, ht_ctid, values, isnull))
	{
		MemoryContextReset(insertCtx);
		initSpGistState(&spgstate, index);
	}

	SpGistUpdateMetaPage(index);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	/* 返回 false，因为我们没有做任何唯一性检查 */
	return false;
}
