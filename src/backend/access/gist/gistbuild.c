/*-------------------------------------------------------------------------
 *
 * gistbuild.c
 *	  GiST 索引构建算法的实现。
 *
 * 有两种不同的策略：
 *
 * 1. 对全部输入元组排序，按排序顺序把它们装入 GiST 叶子
 *    页，并边走边创建下行指针与内部页。这自底向
 *    上构建索引，类似于 B-tree 索引构建的
 *    方式。
 *
 * 2. 从空索引开始，逐个插入全部元组。
 *
 * 若所有列的操作符类都定义了 'sortsupport'，则使用
 * 排序方法。否则退回到第二种策略。
 *
 * 第二种策略可选择在树的不同层使用缓冲区以
 * 减少 I/O，详见 README 中的"缓冲构建算法"
 * 以获得更详细的说明。它起初反复调用插入，
 * 但在达到一定数量的元组后切换到缓冲算法
 * （除非缓冲模式已禁用）。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistbuild.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistxlog.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"

/* 检查是否切换到缓冲构建模式时的索引元组步长 */
#define BUFFERING_MODE_SWITCH_CHECK_STEP 256

/*
 * 在显式打开缓冲时，以慢速方式处理多少元组后才切换到缓冲
 * 模式。同时，这也是在缓冲模式下，两次重新调整
 * 缓冲区大小参数之间要处理的
 * 元组数。
 */
#define BUFFERING_MODE_TUPLE_SIZE_STATS_TARGET 4096

/*
 * 用于构建索引的策略。它可以在各
 * GIST_BUFFERING_* 模式之间动态切换，但若使用排序方法，
 * 则必须预先决定，之后无法再改。
 */
typedef enum
{
	GIST_SORTED_BUILD,			/* 通过排序自底向上构建 */
	GIST_BUFFERING_DISABLED,	/* 处于常规构建模式，且不打算
								 * 切换 */
	GIST_BUFFERING_AUTO,		/* 处于常规构建模式，但若索引变得过
								 * 大则会切换到
								 * 缓冲构建模式 */
	GIST_BUFFERING_STATS,		/* 在切换到缓冲构建
								 * 模式之前，收集索引元组大小的
								 * 统计信息 */
	GIST_BUFFERING_ACTIVE		/* 处于缓冲构建模式 */
} GistBuildMode;

/* gistbuild 及其回调的工作状态 */
typedef struct
{
	Relation	indexrel;
	Relation	heaprel;
	GISTSTATE  *giststate;

	Size		freespace;		/* 页上要保留的空闲空间量 */

	GistBuildMode buildMode;

	int64		indtuples;		/* 已编入索引的元组数 */

	/*
	 * 缓冲构建期间使用的额外数据结构。'gfbb' 包含
	 * 与管理构建缓冲区相关的信息。'parentMap' 是
	 * 各内部页父页的查找表。
	 */
	int64		indtuplesSize;	/* 全部已编入索引元组的总大小 */
	GISTBuildBuffers *gfbb;
	HTAB	   *parentMap;

	/*
	 * 排序构建期间使用的额外数据结构。
	 */
	Tuplesortstate *sortstate;	/* tuplesort.c 的状态数据 */

	BlockNumber pages_allocated;
	BlockNumber pages_written;

	int			ready_num_pages;
	BlockNumber ready_blknos[XLR_MAX_BLOCK_ID];
	Page		ready_pages[XLR_MAX_BLOCK_ID];
} GISTBuildState;

#define GIST_SORTED_BUILD_PAGE_NUM 4

/*
 * 在排序构建中，我们使用这些结构体的栈，每层一个，
 * 以保存该层末尾各页的内存缓冲区。
 *
 * 排序 GiST 构建需要排序操作符类有良好线性化。在多维
 * 数据中并不总是如此。为应对异常，我们
 * 缓冲索引元组并应用能感知多维的 picksplit。
 */
typedef struct GistSortedBuildLevelState
{
	int			current_page;
	BlockNumber last_blkno;
	struct GistSortedBuildLevelState *parent;	/* 上层，若有 */
	Page		pages[GIST_SORTED_BUILD_PAGE_NUM];
} GistSortedBuildLevelState;

/* 私有函数原型 */

static void gistSortedBuildCallback(Relation index, ItemPointer tid,
									Datum *values, bool *isnull,
									bool tupleIsAlive, void *state);
static void gist_indexsortbuild(GISTBuildState *state);
static void gist_indexsortbuild_levelstate_add(GISTBuildState *state,
											   GistSortedBuildLevelState *levelstate,
											   IndexTuple itup);
static void gist_indexsortbuild_levelstate_flush(GISTBuildState *state,
												 GistSortedBuildLevelState *levelstate);
static void gist_indexsortbuild_flush_ready_pages(GISTBuildState *state);

static void gistInitBuffering(GISTBuildState *buildstate);
static int	calculatePagesPerBuffer(GISTBuildState *buildstate, int levelStep);
static void gistBuildCallback(Relation index,
							  ItemPointer tid,
							  Datum *values,
							  bool *isnull,
							  bool tupleIsAlive,
							  void *state);
static void gistBufferingBuildInsert(GISTBuildState *buildstate,
									 IndexTuple itup);
static bool gistProcessItup(GISTBuildState *buildstate, IndexTuple itup,
							BlockNumber startblkno, int startlevel);
static BlockNumber gistbufferinginserttuples(GISTBuildState *buildstate,
											 Buffer buffer, int level,
											 IndexTuple *itup, int ntup, OffsetNumber oldoffnum,
											 BlockNumber parentblk, OffsetNumber downlinkoffnum);
static Buffer gistBufferingFindCorrectParent(GISTBuildState *buildstate,
											 BlockNumber childblkno, int level,
											 BlockNumber *parentblkno,
											 OffsetNumber *downlinkoffnum);
static void gistProcessEmptyingQueue(GISTBuildState *buildstate);
static void gistEmptyAllBuffers(GISTBuildState *buildstate);
static int	gistGetMaxLevel(Relation index);

static void gistInitParentMap(GISTBuildState *buildstate);
static void gistMemorizeParent(GISTBuildState *buildstate, BlockNumber child,
							   BlockNumber parent);
static void gistMemorizeAllDownlinks(GISTBuildState *buildstate,
									 Buffer parentbuf);
static BlockNumber gistGetParent(GISTBuildState *buildstate, BlockNumber child);


/*
 * GiST 索引构建的主入口。
 */
IndexBuildResult *
gistbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	GISTBuildState buildstate;
	MemoryContext oldcxt = CurrentMemoryContext;
	int			fillfactor;
	Oid			SortSupportFnOids[INDEX_MAX_KEYS];
	GiSTOptions *options = (GiSTOptions *) index->rd_options;

	/*
	 * 我们期望对任何索引关系只被调用恰好一次。若非
	 * 如此，那就麻烦大了。
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	buildstate.indexrel = index;
	buildstate.heaprel = heap;
	buildstate.sortstate = NULL;
	buildstate.giststate = initGISTstate(index);

	/*
	 * 创建一个临时内存上下文，每处理一个元组就
	 * 重置一次。（注意：我们不费心把它做成
	 * giststate 的 scanCxt 的子上下文，因此结束时须单独删除。）
	 */
	buildstate.giststate->tempCxt = createTempGistContext();

	/*
	 * 选择构建策略。首先检查用户是否指定使用
	 * 缓冲模式。（现场使用场景或许有些
	 * 存疑，但对测试目的很重要。）
	 */
	if (options)
	{
		if (options->buffering_mode == GIST_OPTION_BUFFERING_ON)
			buildstate.buildMode = GIST_BUFFERING_STATS;
		else if (options->buffering_mode == GIST_OPTION_BUFFERING_OFF)
			buildstate.buildMode = GIST_BUFFERING_DISABLED;
		else					/* 必为 "auto" */
			buildstate.buildMode = GIST_BUFFERING_AUTO;
	}
	else
	{
		buildstate.buildMode = GIST_BUFFERING_AUTO;
	}

	/*
	 * 除非强制使用缓冲模式，否则看能否改用排序。
	 */
	if (buildstate.buildMode != GIST_BUFFERING_STATS)
	{
		bool		hasallsortsupports = true;
		int			keyscount = IndexRelationGetNumberOfKeyAttributes(index);

		for (int i = 0; i < keyscount; i++)
		{
			SortSupportFnOids[i] = index_getprocid(index, i + 1,
												   GIST_SORTSUPPORT_PROC);
			if (!OidIsValid(SortSupportFnOids[i]))
			{
				hasallsortsupports = false;
				break;
			}
		}
		if (hasallsortsupports)
			buildstate.buildMode = GIST_SORTED_BUILD;
	}

	/*
	 * 计算页上要保留的目标空闲空间量。
	 */
	fillfactor = options ? options->fillfactor : GIST_DEFAULT_FILLFACTOR;
	buildstate.freespace = BLCKSZ * (100 - fillfactor) / 100;

	/*
	 * 用所选策略构建索引。
	 */
	buildstate.indtuples = 0;
	buildstate.indtuplesSize = 0;

	if (buildstate.buildMode == GIST_SORTED_BUILD)
	{
		/*
		 * 对全部数据排序，自底向上构建索引。
		 */
		buildstate.sortstate = tuplesort_begin_index_gist(heap,
														  index,
														  maintenance_work_mem,
														  NULL,
														  TUPLESORT_NONE);

		/* 扫描表，把全部元组加入 tuplesort */
		reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
										   gistSortedBuildCallback,
										   (void *) &buildstate, NULL);

		/*
		 * 执行排序并构建索引页。
		 */
		tuplesort_performsort(buildstate.sortstate);

		gist_indexsortbuild(&buildstate);

		tuplesort_end(buildstate.sortstate);
	}
	else
	{
		/*
		 * 初始化空索引并插入全部元组，可能在
		 * 中间层使用缓冲区。
		 */
		Buffer		buffer;
		Page		page;

		/* 初始化根页 */
		buffer = gistNewBuffer(index, heap);
		Assert(BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO);
		page = BufferGetPage(buffer);

		START_CRIT_SECTION();

		GISTInitBuffer(buffer, F_LEAF);

		MarkBufferDirty(buffer);
		PageSetLSN(page, GistBuildLSN);

		UnlockReleaseBuffer(buffer);

		END_CRIT_SECTION();

		/* 扫描表，把全部元组插入索引。 */
		reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
										   gistBuildCallback,
										   (void *) &buildstate, NULL);

		/*
		 * 若使用了缓冲，则冲刷仍留在
		 * 缓冲区中的全部元组。
		 */
		if (buildstate.buildMode == GIST_BUFFERING_ACTIVE)
		{
			elog(DEBUG1, "all tuples processed, emptying buffers");
			gistEmptyAllBuffers(&buildstate);
			gistFreeBuildBuffers(buildstate.gfbb);
		}

		/*
		 * 构建索引时我们未写 WAL 记录，因此若
		 * 需要 WAL 日志，现在把全部页写入 WAL。
		 */
		if (RelationNeedsWAL(index))
		{
			log_newpage_range(index, MAIN_FORKNUM,
							  0, RelationGetNumberOfBlocks(index),
							  true);
		}
	}

	/* 好了，全部堆元组都已编入索引 */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(buildstate.giststate->tempCxt);

	freeGISTstate(buildstate.giststate);

	/*
	 * 返回统计信息
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = (double) buildstate.indtuples;

	return result;
}

/*-------------------------------------------------------------------------
 * 排序构建例程
 *-------------------------------------------------------------------------
 */

/*
 * table_index_build_scan 的每元组回调。
 */
static void
gistSortedBuildCallback(Relation index,
						ItemPointer tid,
						Datum *values,
						bool *isnull,
						bool tupleIsAlive,
						void *state)
{
	GISTBuildState *buildstate = (GISTBuildState *) state;
	MemoryContext oldCtx;
	Datum		compressed_values[INDEX_MAX_KEYS];

	oldCtx = MemoryContextSwitchTo(buildstate->giststate->tempCxt);

	/* 构造索引元组并指向堆元组 */
	gistCompressValues(buildstate->giststate, index,
					   values, isnull,
					   true, compressed_values);

	tuplesort_putindextuplevalues(buildstate->sortstate,
								  buildstate->indexrel,
								  tid,
								  compressed_values, isnull);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->giststate->tempCxt);

	/* 更新元组计数。 */
	buildstate->indtuples += 1;
}

/*
 * 由预先排序的元组自底向上构建 GiST 索引。
 */
static void
gist_indexsortbuild(GISTBuildState *state)
{
	IndexTuple	itup;
	GistSortedBuildLevelState *levelstate;
	Page		page;

	state->pages_allocated = 0;
	state->pages_written = 0;
	state->ready_num_pages = 0;

	/*
	 * 写一个空页作为根页占位符。结束时会
	 * 用真正的根页替换它。
	 */
	page = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, MCXT_ALLOC_ZERO);
	smgrextend(RelationGetSmgr(state->indexrel), MAIN_FORKNUM, GIST_ROOT_BLKNO,
			   page, true);
	state->pages_allocated++;
	state->pages_written++;

	/* 为第一批叶子页分配临时缓冲区。 */
	levelstate = palloc0(sizeof(GistSortedBuildLevelState));
	levelstate->pages[0] = page;
	levelstate->parent = NULL;
	gistinitpage(page, F_LEAF);

	/*
	 * 按排序顺序用元组填充索引页。
	 */
	while ((itup = tuplesort_getindextuple(state->sortstate, true)) != NULL)
	{
		gist_indexsortbuild_levelstate_add(state, levelstate, itup);
		MemoryContextReset(state->giststate->tempCxt);
	}

	/*
	 * 写出部分填满的非根页。
	 *
	 * 记住冲刷可能构建新根。若页数 > 1
	 * 则需要新根。
	 */
	while (levelstate->parent != NULL || levelstate->current_page != 0)
	{
		GistSortedBuildLevelState *parent;

		gist_indexsortbuild_levelstate_flush(state, levelstate);
		parent = levelstate->parent;
		for (int i = 0; i < GIST_SORTED_BUILD_PAGE_NUM; i++)
			if (levelstate->pages[i])
				pfree(levelstate->pages[i]);
		pfree(levelstate);
		levelstate = parent;
	}

	gist_indexsortbuild_flush_ready_pages(state);

	/* 写出根页 */
	PageSetLSN(levelstate->pages[0], GistBuildLSN);
	PageSetChecksumInplace(levelstate->pages[0], GIST_ROOT_BLKNO);
	smgrwrite(RelationGetSmgr(state->indexrel), MAIN_FORKNUM, GIST_ROOT_BLKNO,
			  levelstate->pages[0], true);
	if (RelationNeedsWAL(state->indexrel))
		log_newpage(&state->indexrel->rd_locator, MAIN_FORKNUM, GIST_ROOT_BLKNO,
					levelstate->pages[0], true);

	pfree(levelstate->pages[0]);
	pfree(levelstate);

	/*
	 * 当我们已把索引页记入 WAL 时，仍必须 fsync 索引文件。
	 * 由于我们在共享缓冲区之外构建，构建期间发生的
	 * CHECKPOINT 无法把先前写入的数据冲刷到
	 * 磁盘（实际上它甚至不知道该索引存在）。稍后崩溃
	 * 会从检查点重放 WAL，因此不会重放我们
	 * 更早的 WAL 条目。若此处不对那些页做 fsync，崩溃发生时
	 * 它们可能仍不在磁盘上。
	 */
	if (RelationNeedsWAL(state->indexrel))
		smgrimmedsync(RelationGetSmgr(state->indexrel), MAIN_FORKNUM);
}

/*
 * 把元组加入页。若各页已满，先写出它们并重新初始化
 * 一个新页。
 */
static void
gist_indexsortbuild_levelstate_add(GISTBuildState *state,
								   GistSortedBuildLevelState *levelstate,
								   IndexTuple itup)
{
	Size		sizeNeeded;

	/* 检查元组能否加入当前页 */
	sizeNeeded = IndexTupleSize(itup) + sizeof(ItemIdData); /* 忽略 fillfactor */
	if (PageGetFreeSpace(levelstate->pages[levelstate->current_page]) < sizeNeeded)
	{
		Page		newPage;
		Page		old_page = levelstate->pages[levelstate->current_page];
		uint16		old_page_flags = GistPageGetOpaque(old_page)->flags;

		if (levelstate->current_page + 1 == GIST_SORTED_BUILD_PAGE_NUM)
		{
			gist_indexsortbuild_levelstate_flush(state, levelstate);
		}
		else
			levelstate->current_page++;

		if (levelstate->pages[levelstate->current_page] == NULL)
			levelstate->pages[levelstate->current_page] =
				palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);

		newPage = levelstate->pages[levelstate->current_page];
		gistinitpage(newPage, old_page_flags);
	}

	gistfillbuffer(levelstate->pages[levelstate->current_page], &itup, 1, InvalidOffsetNumber);
}

static void
gist_indexsortbuild_levelstate_flush(GISTBuildState *state,
									 GistSortedBuildLevelState *levelstate)
{
	GistSortedBuildLevelState *parent;
	BlockNumber blkno;
	MemoryContext oldCtx;
	IndexTuple	union_tuple;
	SplitedPageLayout *dist;
	IndexTuple *itvec;
	int			vect_len;
	bool		isleaf = GistPageIsLeaf(levelstate->pages[0]);

	CHECK_FOR_INTERRUPTS();

	oldCtx = MemoryContextSwitchTo(state->giststate->tempCxt);

	/* 从第一页获取索引元组 */
	itvec = gistextractpage(levelstate->pages[0], &vect_len);
	if (levelstate->current_page > 0)
	{
		/* 追加每一页的元组 */
		for (int i = 1; i < levelstate->current_page + 1; i++)
		{
			int			len_local;
			IndexTuple *itvec_local = gistextractpage(levelstate->pages[i], &len_local);

			itvec = gistjoinvector(itvec, &vect_len, itvec_local, len_local);
			pfree(itvec_local);
		}

		/* 对全部已收集元组的列表应用 picksplit */
		dist = gistSplit(state->indexrel, levelstate->pages[0], itvec, vect_len, state->giststate);
	}
	else
	{
		/* 由单页创建分裂布局 */
		dist = (SplitedPageLayout *) palloc0(sizeof(SplitedPageLayout));
		union_tuple = gistunion(state->indexrel, itvec, vect_len,
								state->giststate);
		dist->itup = union_tuple;
		dist->list = gistfillitupvec(itvec, vect_len, &(dist->lenlist));
		dist->block.num = vect_len;
	}

	MemoryContextSwitchTo(oldCtx);

	/* 重置页计数器 */
	levelstate->current_page = 0;

	/* 为分裂结果中的全部分区创建页 */
	for (; dist != NULL; dist = dist->next)
	{
		char	   *data;
		Page		target;

		/* 每页检查一次 */
		CHECK_FOR_INTERRUPTS();

		/* 创建页并复制数据 */
		data = (char *) (dist->list);
		target = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, MCXT_ALLOC_ZERO);
		gistinitpage(target, isleaf ? F_LEAF : 0);
		for (int i = 0; i < dist->block.num; i++)
		{
			IndexTuple	thistup = (IndexTuple) data;

			if (PageAddItem(target, (Item) data, IndexTupleSize(thistup), i + FirstOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in \"%s\"", RelationGetRelationName(state->indexrel));

			data += IndexTupleSize(thistup);
		}
		union_tuple = dist->itup;

		if (state->ready_num_pages == XLR_MAX_BLOCK_ID)
			gist_indexsortbuild_flush_ready_pages(state);

		/*
		 * 该页现已完成。为其分配块号，并加入
		 * 已完成页列表。（我们不立即写出，
		 * 因为希望批量把这些页记入 WAL。）
		 */
		blkno = state->pages_allocated++;
		state->ready_blknos[state->ready_num_pages] = blkno;
		state->ready_pages[state->ready_num_pages] = target;
		state->ready_num_pages++;
		ItemPointerSetBlockNumber(&(union_tuple->t_tid), blkno);

		/*
		 * 把右链接设为指向前一页。这仅用于
		 * 调试：仅当页与扫描并发分裂时 GiST 才会
		 * 跟随右链接，而索引构建期间
		 * 不可能发生。
		 *
		 * 我们把新页的右链接设为指向前一页、
		 * 而不是反过来，这有点反直觉。但
		 * GiST 页不像 B-tree 页那样有序，因此只要
		 * 右链接在同一层全部页上形成链，
		 * 顺序并不重要。
		 */
		if (levelstate->last_blkno)
			GistPageGetOpaque(target)->rightlink = levelstate->last_blkno;
		levelstate->last_blkno = blkno;

		/*
		 * 把下行指针插入父页。若这曾是根，
		 * 则创建新页作为父，它成为新根。
		 */
		parent = levelstate->parent;
		if (parent == NULL)
		{
			parent = palloc0(sizeof(GistSortedBuildLevelState));
			parent->pages[0] = (Page) palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);
			parent->parent = NULL;
			gistinitpage(parent->pages[0], 0);

			levelstate->parent = parent;
		}
		gist_indexsortbuild_levelstate_add(state, parent, union_tuple);
	}
}

static void
gist_indexsortbuild_flush_ready_pages(GISTBuildState *state)
{
	if (state->ready_num_pages == 0)
		return;

	for (int i = 0; i < state->ready_num_pages; i++)
	{
		Page		page = state->ready_pages[i];
		BlockNumber blkno = state->ready_blknos[i];

		/* 当前，这些块必须按顺序缓冲。 */
		if (blkno != state->pages_written)
			elog(ERROR, "unexpected block number to flush GiST sorting build");

		PageSetLSN(page, GistBuildLSN);
		PageSetChecksumInplace(page, blkno);
		smgrextend(RelationGetSmgr(state->indexrel), MAIN_FORKNUM, blkno, page,
				   true);

		state->pages_written++;
	}

	if (RelationNeedsWAL(state->indexrel))
		log_newpages(&state->indexrel->rd_locator, MAIN_FORKNUM, state->ready_num_pages,
					 state->ready_blknos, state->ready_pages, true);

	for (int i = 0; i < state->ready_num_pages; i++)
		pfree(state->ready_pages[i]);

	state->ready_num_pages = 0;
}


/*-------------------------------------------------------------------------
 * 非排序构建例程
 *-------------------------------------------------------------------------
 */

/*
 * 尝试切换到缓冲模式。
 *
 * 若没有足够内存做缓冲构建，则把 bufferingMode
 * 设为 GIST_BUFFERING_DISABLED，以便不再费心尝试
 * 切换。否则初始化构建缓冲区，并把 bufferingMode 设为
 * GIST_BUFFERING_ACTIVE。
 */
static void
gistInitBuffering(GISTBuildState *buildstate)
{
	Relation	index = buildstate->indexrel;
	int			pagesPerBuffer;
	Size		pageFreeSpace;
	Size		itupAvgSize,
				itupMinSize;
	double		avgIndexTuplesPerPage,
				maxIndexTuplesPerPage;
	int			i;
	int			levelStep;

	/* 计算索引页上可供索引元组使用的空间 */
	pageFreeSpace = BLCKSZ - SizeOfPageHeaderData - sizeof(GISTPageOpaqueData)
		- sizeof(ItemIdData)
		- buildstate->freespace;

	/*
	 * 用已收集的统计信息计算已插入索引元组的
	 * 平均大小。
	 */
	itupAvgSize = (double) buildstate->indtuplesSize /
		(double) buildstate->indtuples;

	/*
	 * 按索引元数据计算索引元组的最小可能大小。
	 * varlena 的最小可能大小为 VARHDRSZ。
	 *
	 * XXX: 这其实并不成立，因为短 varlena 可以只有 2 字节。
	 * 而且这里还应考虑填充。
	 */
	itupMinSize = (Size) MAXALIGN(sizeof(IndexTupleData));
	for (i = 0; i < index->rd_att->natts; i++)
	{
		if (TupleDescAttr(index->rd_att, i)->attlen < 0)
			itupMinSize += VARHDRSZ;
		else
			itupMinSize += TupleDescAttr(index->rd_att, i)->attlen;
	}

	/* 计算能装入一页的索引元组平均与最大个数 */
	avgIndexTuplesPerPage = pageFreeSpace / itupAvgSize;
	maxIndexTuplesPerPage = pageFreeSpace / itupMinSize;

	/*
	 * 我们需要为缓冲算法计算两个参数：
	 * levelStep 与 pagesPerBuffer。
	 *
	 * levelStep 决定清空缓冲区时所操作的子树
	 * 大小。值越高越好，因为构建索引所需的
	 * 缓冲区清空步骤更少。但若设得太高，子树就
	 * 不再装得进缓存，并很快失去
	 * 缓冲区的
	 * 好处。
	 *
	 * 在 Arge 等人的论文中，levelStep 取为 logB(M/4B)，其中 B 是
	 * 页上的元组数（即扇出），M 是可用
	 * 内部内存量。奇怪的是，他们并未解释*为何*
	 * 该设置最优。我们通过取仍使子树装得进缓存的
	 * 最高 levelStep 来计算。对较小的 B，我们计算
	 * levelStep 的方式非常接近 Arge 等人的公式。对
	 * 较大的 B，我们的公式给出的值高 2 倍。
	 *
	 * 深度为 n 的子树的平均大小（以页计）可按
	 * 几何级数计算：
	 *
	 * B^0 + B^1 + B^2 + ... + B^n = (1 - B^(n + 1)) / (1 - B)
	 *
	 * 其中 B 是页上索引元组的平均数。子树缓存在
	 * 共享缓冲缓存与操作系统缓存中，因此我们选择
	 * levelStep，使子树大小舒适地小于
	 * effective_cache_size，并带有安全系数 4。
	 *
	 * 页上索引元组平均数的估计基于切换到缓冲
	 * 构建前所观测到的平均元组大小，因此
	 * 真实子树大小可能略大。此外，独占整个
	 * 缓存做索引构建也不厚道。安全系数 4
	 * 应能覆盖这些影响。
	 *
	 * 设置 levelStep 的另一限制因素是：处理
	 * 子树时，我们需要为下一更低缓冲层的每个缓冲区
	 * 持有一页。为此所需的最大缓冲区数为
	 * maxIndexTuplesPerPage^levelStep。这非常保守，但
	 * 希望 maintenance_work_mem 设得足够高，使你
	 * 受 effective_cache_size 而非 maintenance_work_mem 约束。
	 *
	 * XXX: 缓冲区哈希表每个缓冲区也消耗相当多内存，
	 * 但当前未计入。它随所用缓冲区总数伸缩，
	 * 即随索引大小与 levelStep。注意更高的 levelStep
	 * *减少* 哈希表所需的内存量。
	 */
	levelStep = 1;
	for (;;)
	{
		double		subtreesize;
		double		maxlowestlevelpages;

		/* 此 levelStep 下平均子树的大小（以页计）。 */
		subtreesize =
			(1 - pow(avgIndexTuplesPerPage, (double) (levelStep + 1))) /
			(1 - avgIndexTuplesPerPage);

		/* 子树最底层的最大页数 */
		maxlowestlevelpages = pow(maxIndexTuplesPerPage, (double) levelStep);

		/* 子树必须装得进缓存（安全系数为 4） */
		if (subtreesize > effective_cache_size / 4)
			break;

		/* 子树最底层的每个节点在内存中有一页 */
		if (maxlowestlevelpages > ((double) maintenance_work_mem * 1024) / BLCKSZ)
			break;

		/* 很好，我们能处理此 levelStep。看能否再高一级。 */
		levelStep++;
	}

	/*
	 * 上一循环刚到达不可接受的 levelStep 值。
	 * 因此减小 levelStep 以得到最后一个可接受值。
	 */
	levelStep--;

	/*
	 * 若缓存或 maintenance_work_mem 不足，则退回普通
	 * 插入。
	 */
	if (levelStep <= 0)
	{
		elog(DEBUG1, "failed to switch to buffered GiST build");
		buildstate->buildMode = GIST_BUFFERING_DISABLED;
		return;
	}

	/*
	 * 要设置的第二个参数是 pagesPerBuffer，它决定
	 * 每个缓冲区的大小。我们在构建期间也会调整 pagesPerBuffer，
	 * 因此该计算放在单独函数中。
	 */
	pagesPerBuffer = calculatePagesPerBuffer(buildstate, levelStep);

	/* 用这些参数初始化 GISTBuildBuffers */
	buildstate->gfbb = gistInitBuildBuffers(pagesPerBuffer, levelStep,
											gistGetMaxLevel(index));

	gistInitParentMap(buildstate);

	buildstate->buildMode = GIST_BUFFERING_ACTIVE;

	elog(DEBUG1, "switched to buffered GiST build; level step = %d, pagesPerBuffer = %d",
		 levelStep, pagesPerBuffer);
}

/*
 * 为缓冲算法计算 pagesPerBuffer 参数。
 *
 * 选择缓冲区大小时假定元组随机分布，清空半个
 * 缓冲区平均会填满下一更低层每个缓冲区中的
 * 一页。
 */
static int
calculatePagesPerBuffer(GISTBuildState *buildstate, int levelStep)
{
	double		pagesPerBuffer;
	double		avgIndexTuplesPerPage;
	double		itupAvgSize;
	Size		pageFreeSpace;

	/* 计算索引页上可供索引元组使用的空间 */
	pageFreeSpace = BLCKSZ - SizeOfPageHeaderData - sizeof(GISTPageOpaqueData)
		- sizeof(ItemIdData)
		- buildstate->freespace;

	/*
	 * 用已收集的统计信息计算已插入索引元组的
	 * 平均大小。
	 */
	itupAvgSize = (double) buildstate->indtuplesSize /
		(double) buildstate->indtuples;

	avgIndexTuplesPerPage = pageFreeSpace / itupAvgSize;

	/*
	 * 重新计算所需的缓冲区大小。
	 */
	pagesPerBuffer = 2 * pow(avgIndexTuplesPerPage, levelStep);

	return (int) rint(pagesPerBuffer);
}

/*
 * table_index_build_scan 的每元组回调。
 */
static void
gistBuildCallback(Relation index,
				  ItemPointer tid,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state)
{
	GISTBuildState *buildstate = (GISTBuildState *) state;
	IndexTuple	itup;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(buildstate->giststate->tempCxt);

	/* 构造索引元组并指向堆元组 */
	itup = gistFormTuple(buildstate->giststate, index,
						 values, isnull,
						 true);
	itup->t_tid = *tid;

	/* 更新元组计数与总大小。 */
	buildstate->indtuples += 1;
	buildstate->indtuplesSize += IndexTupleSize(itup);

	/*
	 * XXX 在缓冲构建中，tempCxt 也会在
	 * gistProcessEmptyingQueue() 内部被重置。这不太好，因为有
	 * 混淆及使用悬空指针的风险（例如，控制返回此处时
	 * itup 可能已被释放）。一般而言，内存上下文最好
	 * 只由一个函数"拥有"。不过，
	 * 当前这并未造成问题，因此似乎不值得为避免它而做
	 * 那么多重构。
	 */
	if (buildstate->buildMode == GIST_BUFFERING_ACTIVE)
	{
		/* 我们有缓冲区，因此使用它们。 */
		gistBufferingBuildInsert(buildstate, itup);
	}
	else
	{
		/*
		 * （尚）没有缓冲区。由于我们已经锁住索引关系，
		 * 直接调用 gistdoinsert。
		 */
		gistdoinsert(index, itup, buildstate->freespace,
					 buildstate->giststate, buildstate->heaprel, true);
	}

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->giststate->tempCxt);

	if (buildstate->buildMode == GIST_BUFFERING_ACTIVE &&
		buildstate->indtuples % BUFFERING_MODE_TUPLE_SIZE_STATS_TARGET == 0)
	{
		/* 现在调整目标缓冲区大小 */
		buildstate->gfbb->pagesPerBuffer =
			calculatePagesPerBuffer(buildstate, buildstate->gfbb->levelStep);
	}

	/*
	 * 在 'auto' 模式下，检查索引是否已大到装不进缓存，
	 * 若是则切换到缓冲模式。
	 *
	 * 为避免过多调用 smgrnblocks()，仅每
	 * BUFFERING_MODE_SWITCH_CHECK_STEP 个索引元组检查一次。
	 *
	 * 在 'stats' 状态下，一旦已看到足够元组以对
	 * 平均元组大小有所了解就切换。
	 */
	if ((buildstate->buildMode == GIST_BUFFERING_AUTO &&
		 buildstate->indtuples % BUFFERING_MODE_SWITCH_CHECK_STEP == 0 &&
		 effective_cache_size < smgrnblocks(RelationGetSmgr(index),
											MAIN_FORKNUM)) ||
		(buildstate->buildMode == GIST_BUFFERING_STATS &&
		 buildstate->indtuples >= BUFFERING_MODE_TUPLE_SIZE_STATS_TARGET))
	{
		/*
		 * 索引已装不进有效缓存。尝试切换到
		 * 缓冲构建模式。
		 */
		gistInitBuffering(buildstate);
	}
}

/*
 * 缓冲索引构建的插入函数。
 */
static void
gistBufferingBuildInsert(GISTBuildState *buildstate, IndexTuple itup)
{
	/* 把元组插入缓冲区。 */
	gistProcessItup(buildstate, itup, 0, buildstate->gfbb->rootlevel);

	/* 若我们填满了（半个）缓冲区，则处理缓冲区清空。 */
	gistProcessEmptyingQueue(buildstate);
}

/*
 * 处理一个索引元组。沿树向下运行该元组，直到到达叶子
 * 页或节点缓冲区，并在那里插入元组。若我们必须
 * 停止缓冲区清空过程（因为某个子缓冲区不能再
 * 接受索引元组）则返回 true。
 */
static bool
gistProcessItup(GISTBuildState *buildstate, IndexTuple itup,
				BlockNumber startblkno, int startlevel)
{
	GISTSTATE  *giststate = buildstate->giststate;
	GISTBuildBuffers *gfbb = buildstate->gfbb;
	Relation	indexrel = buildstate->indexrel;
	BlockNumber childblkno;
	Buffer		buffer;
	bool		result = false;
	BlockNumber blkno;
	int			level;
	OffsetNumber downlinkoffnum = InvalidOffsetNumber;
	BlockNumber parentblkno = InvalidBlockNumber;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 循环直到到达叶子页（level == 0）或带缓冲区的层
	 * （不包括我们起始的层，否则将无法
	 * 取得进展）。
	 */
	blkno = startblkno;
	level = startlevel;
	for (;;)
	{
		ItemId		iid;
		IndexTuple	idxtuple,
					newtup;
		Page		page;
		OffsetNumber childoffnum;

		/* 我们是否已到达带缓冲区的层？ */
		if (LEVEL_HAS_BUFFERS(level, gfbb) && level != startlevel)
			break;

		/* 我们是否已到达叶子页？ */
		if (level == 0)
			break;

		/*
		 * 没有。那就下降到下一层。选择一个子节点
		 * 下降到那里。
		 */

		buffer = ReadBuffer(indexrel, blkno);
		LockBuffer(buffer, GIST_EXCLUSIVE);

		page = (Page) BufferGetPage(buffer);
		childoffnum = gistchoose(indexrel, page, itup, giststate);
		iid = PageGetItemId(page, childoffnum);
		idxtuple = (IndexTuple) PageGetItem(page, iid);
		childblkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));

		if (level > 1)
			gistMemorizeParent(buildstate, childblkno, blkno);

		/*
		 * 检查表示目标子节点的键是否与我们正在
		 * 插入的键一致。若否，则更新它。
		 */
		newtup = gistgetadjusted(indexrel, idxtuple, itup, giststate);
		if (newtup)
		{
			blkno = gistbufferinginserttuples(buildstate,
											  buffer,
											  level,
											  &newtup,
											  1,
											  childoffnum,
											  InvalidBlockNumber,
											  InvalidOffsetNumber);
			/* gistbufferinginserttuples() 已释放该缓冲区 */
		}
		else
			UnlockReleaseBuffer(buffer);

		/* 下降到子节点 */
		parentblkno = blkno;
		blkno = childblkno;
		downlinkoffnum = childoffnum;
		Assert(level > 0);
		level--;
	}

	if (LEVEL_HAS_BUFFERS(level, gfbb))
	{
		/*
		 * 我们已到达带缓冲区的层。把索引元组放入
		 * 缓冲区，若溢出则把该缓冲区加入清空队列。
		 */
		GISTNodeBuffer *childNodeBuffer;

		/* 查找缓冲区或创建新的 */
		childNodeBuffer = gistGetNodeBuffer(gfbb, giststate, blkno, level);

		/* 把索引元组加入其中 */
		gistPushItupToNodeBuffer(gfbb, childNodeBuffer, itup);

		if (BUFFER_OVERFLOWED(childNodeBuffer, gfbb))
			result = true;
	}
	else
	{
		/*
		 * 我们已到达叶子页。把元组放在这里。
		 */
		Assert(level == 0);
		buffer = ReadBuffer(indexrel, blkno);
		LockBuffer(buffer, GIST_EXCLUSIVE);
		gistbufferinginserttuples(buildstate, buffer, level,
								  &itup, 1, InvalidOffsetNumber,
								  parentblkno, downlinkoffnum);
		/* gistbufferinginserttuples() 已释放该缓冲区 */
	}

	return result;
}

/*
 * 把元组插入给定页。
 *
 * 这与常规插入代码中的 gistinserttuples() 类似。
 *
 * 返回（第一个）新或更新元组所插入到的页的
 * 块号。通常是原页，但若原页被分裂则可能是
 * 兄弟页。
 *
 * 调用者进入时应持有 'buffer' 上的锁。本函数将解锁
 * 并取消固定它。
 */
static BlockNumber
gistbufferinginserttuples(GISTBuildState *buildstate, Buffer buffer, int level,
						  IndexTuple *itup, int ntup, OffsetNumber oldoffnum,
						  BlockNumber parentblk, OffsetNumber downlinkoffnum)
{
	GISTBuildBuffers *gfbb = buildstate->gfbb;
	List	   *splitinfo;
	bool		is_split;
	BlockNumber placed_to_blk = InvalidBlockNumber;

	is_split = gistplacetopage(buildstate->indexrel,
							   buildstate->freespace,
							   buildstate->giststate,
							   buffer,
							   itup, ntup, oldoffnum, &placed_to_blk,
							   InvalidBuffer,
							   &splitinfo,
							   false,
							   buildstate->heaprel, true);

	/*
	 * 若这是根分裂，更新内存中保存的根路径项。这
	 * 确保所有路径栈始终完整，包括直到根的全部
	 * 父节点。这简化了重新找到正确父的
	 * 算法。
	 */
	if (is_split && BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO)
	{
		Page		page = BufferGetPage(buffer);
		OffsetNumber off;
		OffsetNumber maxoff;

		Assert(level == gfbb->rootlevel);
		gfbb->rootlevel++;

		elog(DEBUG2, "splitting GiST root page, now %d levels deep", gfbb->rootlevel);

		/*
		 * 旧根页上的全部下行指针现在位于某个子
		 * 页上。访问全部新子页以记住孙节点的
		 * 父。
		 */
		if (gfbb->rootlevel > 1)
		{
			maxoff = PageGetMaxOffsetNumber(page);
			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(page, off);
				IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
				BlockNumber childblkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
				Buffer		childbuf = ReadBuffer(buildstate->indexrel, childblkno);

				LockBuffer(childbuf, GIST_SHARE);
				gistMemorizeAllDownlinks(buildstate, childbuf);
				UnlockReleaseBuffer(childbuf);

				/*
				 * 同时记住新子页的父是
				 * 根块。
				 */
				gistMemorizeParent(buildstate, childblkno, GIST_ROOT_BLKNO);
			}
		}
	}

	if (splitinfo)
	{
		/*
		 * 把下行指针插入父。这与常规插入代码中的
		 * gistfinishsplit() 类似，但加锁更
		 * 简单，且我们必须维护内部节点上的缓冲区与
		 * 父映射。
		 */
		IndexTuple *downlinks;
		int			ndownlinks,
					i;
		Buffer		parentBuffer;
		ListCell   *lc;

		/* 自我们记住此路径以来，父可能已改变。 */
		parentBuffer =
			gistBufferingFindCorrectParent(buildstate,
										   BufferGetBlockNumber(buffer),
										   level,
										   &parentblk,
										   &downlinkoffnum);

		/*
		 * 若有与此页关联的缓冲区，那也需要
		 * 分裂。gistRelocateBuildBuffersOnSplit() 还会调整
		 * 'splitinfo' 中的下行指针，确保它们不仅与页上
		 * 已有的元组一致，也与最终将插入其中的
		 * 缓冲区中的元组一致。
		 */
		gistRelocateBuildBuffersOnSplit(gfbb,
										buildstate->giststate,
										buildstate->indexrel,
										level,
										buffer, splitinfo);

		/* 创建全部下行指针元组的数组 */
		ndownlinks = list_length(splitinfo);
		downlinks = (IndexTuple *) palloc(sizeof(IndexTuple) * ndownlinks);
		i = 0;
		foreach(lc, splitinfo)
		{
			GISTPageSplitInfo *splitinfo = lfirst(lc);

			/*
			 * 在父映射中记住每个新子页的父。
			 * 这假定下行指针能装进父页。若在我们
			 * 向上递归插入下行指针时父页也
			 * 分裂，递归的 gistbufferinginserttuples() 调用将
			 * 再次更新该映射。
			 */
			if (level > 0)
				gistMemorizeParent(buildstate,
								   BufferGetBlockNumber(splitinfo->buf),
								   BufferGetBlockNumber(parentBuffer));

			/*
			 * 同时为所有被移到不同页的下行指针更新
			 * 父映射。（实际上这也会遍历仍留在
			 * 原页上的下行指针，但并无
			 * 害处）。
			 */
			if (level > 1)
				gistMemorizeAllDownlinks(buildstate, splitinfo->buf);

			/*
			 * 由于没有并发访问，我们可以立即释放较低
			 * 层的缓冲区。这包括原页。
			 */
			UnlockReleaseBuffer(splitinfo->buf);
			downlinks[i++] = splitinfo->downlink;
		}

		/* 把它们插入父。 */
		gistbufferinginserttuples(buildstate, parentBuffer, level + 1,
								  downlinks, ndownlinks, downlinkoffnum,
								  InvalidBlockNumber, InvalidOffsetNumber);

		list_free_deep(splitinfo);	/* 我们不再需要这个了 */
	}
	else
		UnlockReleaseBuffer(buffer);

	return placed_to_blk;
}

/*
 * 查找指向子页的下行指针。
 *
 * 'childblkno' 指示要为其查找父的子页。'level' 是
 * 子的层号。进入时，*parentblkno 与 *downlinkoffnum 可
 * 指向下行指针曾经所在的位置——我们会先检查该
 * 位置，若尚未移动则可节省一些周期。本函数
 * 返回包含该下行指针、已排他锁定的缓冲区，并且
 * *parentblkno 与 *downlinkoffnum 被设为下行指针的
 * 真实位置。
 *
 * 若子页是叶子（level == 0），调用者必须提供正确的
 * parentblkno。否则我们用父映射哈希表查找父
 * 块。
 *
 * 本函数与正常索引插入期间的 gistFindCorrectParent()
 * 用途相同，但更简单，因为我们不必处理
 * 并发插入。
 */
static Buffer
gistBufferingFindCorrectParent(GISTBuildState *buildstate,
							   BlockNumber childblkno, int level,
							   BlockNumber *parentblkno,
							   OffsetNumber *downlinkoffnum)
{
	BlockNumber parent;
	Buffer		buffer;
	Page		page;
	OffsetNumber maxoff;
	OffsetNumber off;

	if (level > 0)
		parent = gistGetParent(buildstate, childblkno);
	else
	{
		/*
		 * 对叶子页，调用者必须提供正确的父块
		 * 号。
		 */
		if (*parentblkno == InvalidBlockNumber)
			elog(ERROR, "no parent buffer provided of child %u", childblkno);
		parent = *parentblkno;
	}

	buffer = ReadBuffer(buildstate->indexrel, parent);
	page = BufferGetPage(buffer);
	LockBuffer(buffer, GIST_EXCLUSIVE);
	gistcheckpage(buildstate->indexrel, buffer);
	maxoff = PageGetMaxOffsetNumber(page);

	/* 检查它是否未被移动 */
	if (parent == *parentblkno && *parentblkno != InvalidBlockNumber &&
		*downlinkoffnum != InvalidOffsetNumber && *downlinkoffnum <= maxoff)
	{
		ItemId		iid = PageGetItemId(page, *downlinkoffnum);
		IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

		if (ItemPointerGetBlockNumber(&(idxtuple->t_tid)) == childblkno)
		{
			/* 仍在那里 */
			return buffer;
		}
	}

	/*
	 * 下行指针不在它曾经所在的偏移处。扫描该页以
	 * 找到它。在正常 gist 插入期间，它可能已移到另一
	 * 页（向右），但在缓冲构建期间，我们在查找表中
	 * 跟踪每页的父，因此应始终知道它在
	 * 哪一页。
	 */
	for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
	{
		ItemId		iid = PageGetItemId(page, off);
		IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

		if (ItemPointerGetBlockNumber(&(idxtuple->t_tid)) == childblkno)
		{
			/* 是的!!，找到了 */
			*downlinkoffnum = off;
			return buffer;
		}
	}

	elog(ERROR, "failed to re-find parent for block %u", childblkno);
	return InvalidBuffer;		/* 让编译器安静 */
}

/*
 * 处理缓冲区清空栈。清空一个缓冲区可能导致清空
 * 其他缓冲区。本函数迭代直到此级联清空
 * 过程结束，例如直到缓冲区清空栈为空。
 */
static void
gistProcessEmptyingQueue(GISTBuildState *buildstate)
{
	GISTBuildBuffers *gfbb = buildstate->gfbb;

	/* 当缓冲区清空栈中仍有元素时迭代。 */
	while (gfbb->bufferEmptyingQueue != NIL)
	{
		GISTNodeBuffer *emptyingNodeBuffer;

		/* 从清空栈获取节点缓冲区。 */
		emptyingNodeBuffer = (GISTNodeBuffer *) linitial(gfbb->bufferEmptyingQueue);
		gfbb->bufferEmptyingQueue = list_delete_first(gfbb->bufferEmptyingQueue);
		emptyingNodeBuffer->queuedForEmptying = false;

		/*
		 * 我们将装入清空目标缓冲区的末页。
		 * 因此先卸载任何先前已装入的缓冲区。
		 */
		gistUnloadNodeBuffers(gfbb);

		/*
		 * 从缓冲区弹出元组，并把它们下推到较低层的
		 * 缓冲区或叶子页。我们继续直到某个较低
		 * 层缓冲区填满，或本缓冲区变空。
		 *
		 * 在 Arge 等人的论文中，缓冲区清空在处理完
		 * 1/2 节点缓冲区价值的元组后停止，以避免过满
		 * 任何较低层缓冲区。但更高效的是
		 * 一直进行到某个较低层缓冲区真正填满，
		 * 因此我们这样做。这不必精确；若缓冲区
		 * 多溢出几个元组，也无妨。
		 */
		while (true)
		{
			IndexTuple	itup;

			/* 从缓冲区获取下一个索引元组 */
			if (!gistPopItupFromNodeBuffer(gfbb, emptyingNodeBuffer, &itup))
				break;

			/*
			 * 把它下推到下层节点缓冲区或叶子页。
			 *
			 * 注意：我们正在清空的缓冲区可能因本次调用而
			 * 分裂。若发生，我们的 emptyingNodeBuffer
			 * 指向分裂的左半。分裂后，新左缓冲区很
			 * 可能不再超过半满阈值，但我们仍不妨
			 * 继续从它冲刷元组，直到填满某个较低层
			 * 缓冲区。
			 */
			if (gistProcessItup(buildstate, itup, emptyingNodeBuffer->nodeBlocknum, emptyingNodeBuffer->level))
			{
				/*
				 * 较低层缓冲区已填满。停止清空此缓冲区，
				 * 以避免较低层缓冲区溢出。
				 */
				break;
			}

			/* 释放索引元组处理期间分配的全部内存 */
			MemoryContextReset(buildstate->giststate->tempCxt);
		}
	}
}

/*
 * 自上而下清空全部节点缓冲区。这在索引构建
 * 结束时完成，以把全部剩余元组冲刷到索引。
 *
 * 注意：这会销毁 buffersOnLevels 列表，因此调用后
 * 不应再向缓冲区插入。
 */
static void
gistEmptyAllBuffers(GISTBuildState *buildstate)
{
	GISTBuildBuffers *gfbb = buildstate->gfbb;
	MemoryContext oldCtx;
	int			i;

	oldCtx = MemoryContextSwitchTo(buildstate->giststate->tempCxt);

	/*
	 * 自上而下遍历各层。
	 */
	for (i = gfbb->buffersOnLevelsLen - 1; i >= 0; i--)
	{
		/*
		 * 清空本层全部缓冲区。注意处理期间可能因页
		 * 分裂而有新缓冲区出现在列表中，因此简单
		 * 遍历列表行不通。我们在看到缓冲区为空时
		 * 从列表移除；缓冲区一旦被完全清空就
		 * 不能再变为非空。
		 */
		while (gfbb->buffersOnLevels[i] != NIL)
		{
			GISTNodeBuffer *nodeBuffer;

			nodeBuffer = (GISTNodeBuffer *) linitial(gfbb->buffersOnLevels[i]);

			if (nodeBuffer->blocksCount != 0)
			{
				/*
				 * 把此缓冲区加入清空队列，并继续清空
				 * 该队列。
				 */
				if (!nodeBuffer->queuedForEmptying)
				{
					MemoryContextSwitchTo(gfbb->context);
					nodeBuffer->queuedForEmptying = true;
					gfbb->bufferEmptyingQueue =
						lcons(nodeBuffer, gfbb->bufferEmptyingQueue);
					MemoryContextSwitchTo(buildstate->giststate->tempCxt);
				}
				gistProcessEmptyingQueue(buildstate);
			}
			else
				gfbb->buffersOnLevels[i] =
					list_delete_first(gfbb->buffersOnLevels[i]);
		}
		elog(DEBUG2, "emptied all buffers at level %d", i);
	}
	MemoryContextSwitchTo(oldCtx);
}

/*
 * 获取 GiST 索引的深度。
 */
static int
gistGetMaxLevel(Relation index)
{
	int			maxLevel;
	BlockNumber blkno;

	/*
	 * 从根开始向下遍历树，直到碰到叶子
	 * 层。
	 */
	maxLevel = 0;
	blkno = GIST_ROOT_BLKNO;
	while (true)
	{
		Buffer		buffer;
		Page		page;
		IndexTuple	itup;

		buffer = ReadBuffer(index, blkno);

		/*
		 * 索引构建期间没有并发访问，因此加锁只是
		 * 形式上的。
		 */
		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsLeaf(page))
		{
			/* 我们碰到了底部，因此完成了。 */
			UnlockReleaseBuffer(buffer);
			break;
		}

		/*
		 * 取页上第一个下行指针并跟随它。选哪个
		 * 下行指针并不重要，树在各处深度相同，
		 * 因此我们只取第一个。
		 */
		itup = (IndexTuple) PageGetItem(page,
										PageGetItemId(page, FirstOffsetNumber));
		blkno = ItemPointerGetBlockNumber(&(itup->t_tid));
		UnlockReleaseBuffer(buffer);

		/*
		 * 我们正沿树下降。这意味着树中还有
		 * 一层。
		 */
		maxLevel++;
	}
	return maxLevel;
}


/*
 * 管理父映射的例程。
 *
 * 每当页分裂时，我们需要把下行指针插入父。
 * 我们需要设法找到父页才能这样做。在正常插入中，
 * 我们在下降树时保持已访问节点的栈。然而，在
 * 缓冲构建中，当我们通过把元组级联到其子来清空
 * 缓冲区时，可以从任意内部节点开始下降树。因此那时
 * 我们没有直到根的完整栈可用。
 *
 * 因此我们改为维护一个哈希表，跟踪每个内部页的
 * 父。不过我们不需要跟踪叶子节点的父。每当
 * 我们插入叶子时，刚从其父下降下来，因此已知道
 * 其直接父。这大大有助于限制此哈希表
 * 所用的内存。
 *
 * 每当内部节点分裂时，需要更新父映射。
 * 需要把新子页的父记入映射，并且在分裂时
 * 下行指针被移到新页的全部页的条目
 * 也需要更新。
 *
 * 我们在下降树时也会更新父映射。这看似
 * 多余，因为我们在下行指针被移动或创建时就维护
 * 该映射，但这是需要的，因为我们在用常规索引
 * 插入创建树之后才切换到缓冲模式。切换到缓冲
 * 模式之前创建的任何页最初不会出现在父映射中，
 * 但会在我们首次访问它们时加入。
 */

typedef struct
{
	BlockNumber childblkno;		/* 哈希键 */
	BlockNumber parentblkno;
} ParentMapEntry;

static void
gistInitParentMap(GISTBuildState *buildstate)
{
	HASHCTL		hashCtl;

	hashCtl.keysize = sizeof(BlockNumber);
	hashCtl.entrysize = sizeof(ParentMapEntry);
	hashCtl.hcxt = CurrentMemoryContext;
	buildstate->parentMap = hash_create("gistbuild parent map",
										1024,
										&hashCtl,
										HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
gistMemorizeParent(GISTBuildState *buildstate, BlockNumber child, BlockNumber parent)
{
	ParentMapEntry *entry;
	bool		found;

	entry = (ParentMapEntry *) hash_search(buildstate->parentMap,
										   &child,
										   HASH_ENTER,
										   &found);
	entry->parentblkno = parent;
}

/*
 * 扫描页上全部下行指针，并记住它们的父。
 */
static void
gistMemorizeAllDownlinks(GISTBuildState *buildstate, Buffer parentbuf)
{
	OffsetNumber maxoff;
	OffsetNumber off;
	BlockNumber parentblkno = BufferGetBlockNumber(parentbuf);
	Page		page = BufferGetPage(parentbuf);

	Assert(!GistPageIsLeaf(page));

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
		BlockNumber childblkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));

		gistMemorizeParent(buildstate, childblkno, parentblkno);
	}
}

static BlockNumber
gistGetParent(GISTBuildState *buildstate, BlockNumber child)
{
	ParentMapEntry *entry;
	bool		found;

	/* 在哈希表中查找节点缓冲区 */
	entry = (ParentMapEntry *) hash_search(buildstate->parentMap,
										   &child,
										   HASH_FIND,
										   &found);
	if (!found)
		elog(ERROR, "could not find parent of block %u in lookup table", child);

	return entry->parentblkno;
}
