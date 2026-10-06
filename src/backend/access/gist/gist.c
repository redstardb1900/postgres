/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  postgres GiST 索引访问方法的接口例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/xloginsert.h"
#include "catalog/pg_collation.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/index_selfuncs.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* 非导出函数原型 */
static void gistfixsplit(GISTInsertState *state, GISTSTATE *giststate);
static bool gistinserttuple(GISTInsertState *state, GISTInsertStack *stack,
							GISTSTATE *giststate, IndexTuple tuple, OffsetNumber oldoffnum);
static bool gistinserttuples(GISTInsertState *state, GISTInsertStack *stack,
							 GISTSTATE *giststate,
							 IndexTuple *tuples, int ntup, OffsetNumber oldoffnum,
							 Buffer leftchild, Buffer rightchild,
							 bool unlockbuf, bool unlockleftchild);
static void gistfinishsplit(GISTInsertState *state, GISTInsertStack *stack,
							GISTSTATE *giststate, List *splitinfo, bool unlockbuf);
static void gistprunepage(Relation rel, Page page, Buffer buffer,
						  Relation heapRel);


#define ROTATEDIST(d) do { \
	SplitedPageLayout *tmp=(SplitedPageLayout*)palloc0(sizeof(SplitedPageLayout)); \
	tmp->block.blkno = InvalidBlockNumber;	\
	tmp->buffer = InvalidBuffer;	\
	tmp->next = (d); \
	(d)=tmp; \
} while(0)


/*
 * GiST 处理函数：返回带有访问方法参数与回调的
 * IndexAmRoutine。
 */
Datum
gisthandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = GISTNProcs;
	amroutine->amoptsprocnum = GIST_OPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = true;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = true;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_COND_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = gistbuild;
	amroutine->ambuildempty = gistbuildempty;
	amroutine->aminsert = gistinsert;
	amroutine->ambulkdelete = gistbulkdelete;
	amroutine->amvacuumcleanup = gistvacuumcleanup;
	amroutine->amcanreturn = gistcanreturn;
	amroutine->amcostestimate = gistcostestimate;
	amroutine->amoptions = gistoptions;
	amroutine->amproperty = gistproperty;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = gistvalidate;
	amroutine->amadjustmembers = gistadjustmembers;
	amroutine->ambeginscan = gistbeginscan;
	amroutine->amrescan = gistrescan;
	amroutine->amgettuple = gistgettuple;
	amroutine->amgetbitmap = gistgetbitmap;
	amroutine->amendscan = gistendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * 创建并返回供 GiST 使用的临时内存上下文。我们
 * _always_ 在临时内存上下文中调用用户提供的方法，
 * 以免那些函数中的内存泄漏造成问题。此外，
 * GiST 代码自身也使用一些额外的临时上下文，
 * 以避免需要做一些别扭的手工
 * 内存管理。
 */
MemoryContext
createTempGistContext(void)
{
	return AllocSetContextCreate(CurrentMemoryContext,
								 "GiST temporary context",
								 ALLOCSET_DEFAULT_SIZES);
}

/*
 *	gistbuildempty() -- 在初始化分支中构建一个空的 gist 索引
 */
void
gistbuildempty(Relation index)
{
	Buffer		buffer;

	/* 初始化根页 */
	buffer = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							   EB_SKIP_EXTENSION_LOCK | EB_LOCK_FIRST);

	/* 初始化缓冲区并写入 WAL */
	START_CRIT_SECTION();
	GISTInitBuffer(buffer, F_LEAF);
	MarkBufferDirty(buffer);
	log_newpage_buffer(buffer, true);
	END_CRIT_SECTION();

	/* 解锁并释放缓冲区 */
	UnlockReleaseBuffer(buffer);
}

/*
 *	gistinsert -- GiST 元组插入的包装。
 *
 *	  这是在 GiSTs 中做元组插入的公开接口例程。
 *	  它本身不做工作；只是锁住关系并把活转出去。
 */
bool
gistinsert(Relation r, Datum *values, bool *isnull,
		   ItemPointer ht_ctid, Relation heapRel,
		   IndexUniqueCheck checkUnique,
		   bool indexUnchanged,
		   IndexInfo *indexInfo)
{
	GISTSTATE  *giststate = (GISTSTATE *) indexInfo->ii_AmCache;
	IndexTuple	itup;
	MemoryContext oldCxt;

	/* 若本语句中首次调用则初始化 GISTSTATE 缓存 */
	if (giststate == NULL)
	{
		oldCxt = MemoryContextSwitchTo(indexInfo->ii_Context);
		giststate = initGISTstate(r);
		giststate->tempCxt = createTempGistContext();
		indexInfo->ii_AmCache = (void *) giststate;
		MemoryContextSwitchTo(oldCxt);
	}

	oldCxt = MemoryContextSwitchTo(giststate->tempCxt);

	itup = gistFormTuple(giststate, r,
						 values, isnull, true /* 当前大小是假的 */ );
	itup->t_tid = *ht_ctid;

	gistdoinsert(r, itup, 0, giststate, heapRel, false);

	/* 清理 */
	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(giststate->tempCxt);

	return false;
}


/*
 * 把 'itup' 中的元组放到 'buffer'。若 'oldoffnum' 有效，则该 tuple
 * 在插入新元组的同时被原子地移除。
 * 这用于用新元组替换旧元组。
 *
 * 若 'leftchildbuf' 有效，我们正在为 'leftchildbuf' 右侧的页
 * 插入下行指针，或更新 'leftchildbuf' 的下行指针。
 * 'leftchildbuf' 上的 F_FOLLOW_RIGHT 标志被清除，并设置 NSN。
 *
 * 若 'markfollowright' 为真且页发生分裂，则左子页被
 * 标上 F_FOLLOW_RIGHT 标志。这是常规情况。但在缓冲式
 * 索引构建期间没有并发访问，页分裂做法略简，
 * 因此传入 false。
 *
 * 若页上空间不足则分裂。所有分裂出的页保持
 * 钉住并加锁，经 *splitinfo 返回，由调用者负责为它们
 * 插入下行指针。但若 'buffer' 是根页且需要分裂，
 * gistplacetopage() 把分裂作为一次原子操作完成，
 * 并把 *splitinfo 设为 NIL。此时我们继续持有根页锁，
 * 并释放子页；注意新元组并不在根页上，
 * 而在某个新的子页中。
 *
 * 若 'newblkno' 非 NULL，返回第一个新/更新元组被插入到的
 * 页的块号。通常是给定页，但若页分裂了也可能
 * 是其右兄弟。
 *
 * 若页发生分裂返回 'true'，否则
 * 返回 'false'。
 */
bool
gistplacetopage(Relation rel, Size freespace, GISTSTATE *giststate,
				Buffer buffer,
				IndexTuple *itup, int ntup, OffsetNumber oldoffnum,
				BlockNumber *newblkno,
				Buffer leftchildbuf,
				List **splitinfo,
				bool markfollowright,
				Relation heapRel,
				bool is_build)
{
	BlockNumber blkno = BufferGetBlockNumber(buffer);
	Page		page = BufferGetPage(buffer);
	bool		is_leaf = (GistPageIsLeaf(page)) ? true : false;
	XLogRecPtr	recptr;
	bool		is_split;

	/*
	 * 拒绝修改尚未完成分裂的页。这本不该
	 * 发生，因为我们沿树下行时会完成任何未完成的
	 * 分裂。不过，另一并发插入者有可能分裂了
	 * 父页后在完成分裂前出错。那种情况下我们
	 * 只抛错，并让我们自己进行中的任何分裂也保持
	 * 未完成。随后到来的下一次插入会清理
	 * 残局。
	 */
	if (GistFollowRight(page))
		elog(ERROR, "concurrent GiST page split was incomplete");

	/* 绝不应尝试向已删除的页插入 */
	Assert(!GistPageIsDeleted(page));

	*splitinfo = NIL;

	/*
	 * 若是更新，移除旧键：本节点的键已被修改，或因
	 * 子页发生分裂，或因需要为子节点中的插入
	 * 调整我们的键。因此，移除本节点键的
	 * 旧版本。
	 *
	 * 对 WAL 重放，在非分裂情形我们通过设置一个
	 * 单元素 todelete 数组来处理；在分裂情形则隐式处理，
	 * 因为传给 gistSplit 的元组向量不会包含该元组。
	 */
	is_split = gistnospace(page, itup, ntup, oldoffnum, freespace);

	/*
	 * 若叶子页已满，先尝试删除死元组。然后再
	 * 检查一次。
	 */
	if (is_split && GistPageIsLeaf(page) && GistPageHasGarbage(page))
	{
		gistprunepage(rel, page, buffer, heapRel);
		is_split = gistnospace(page, itup, ntup, oldoffnum, freespace);
	}

	if (is_split)
	{
		/* 没有插入空间 */
		IndexTuple *itvec;
		int			tlen;
		SplitedPageLayout *dist = NULL,
				   *ptr;
		BlockNumber oldrlink = InvalidBlockNumber;
		GistNSN		oldnsn = 0;
		SplitedPageLayout rootpg;
		bool		is_rootsplit;
		int			npage;

		is_rootsplit = (blkno == GIST_ROOT_BLKNO);

		/*
		 * 形成用于分裂的索引元组向量。若我们在替换旧元组，
		 * 从向量中移除旧版本。
		 */
		itvec = gistextractpage(page, &tlen);
		if (OffsetNumberIsValid(oldoffnum))
		{
			/* 在内部页上我们应移除旧元组 */
			int			pos = oldoffnum - FirstOffsetNumber;

			tlen--;
			if (pos != tlen)
				memmove(itvec + pos, itvec + pos + 1, sizeof(IndexTuple) * (tlen - pos));
		}
		itvec = gistjoinvector(itvec, &tlen, itup, ntup);
		dist = gistSplit(rel, page, itvec, tlen, giststate);

		/*
		 * 检查分裂没有产生过多页。
		 */
		npage = 0;
		for (ptr = dist; ptr; ptr = ptr->next)
			npage++;
		/* 根分裂时，下面还会向列表再加一页 */
		if (is_rootsplit)
			npage++;
		if (npage > GIST_MAX_SPLIT_PAGES)
			elog(ERROR, "GiST page split into too many halves (%d, maximum %d)",
				 npage, GIST_MAX_SPLIT_PAGES);

		/*
		 * 准备要操作的页。除最左页外为所有页分配
		 * 新缓冲区。原页成为新的最左页，并
		 * 直接替换为新内容。
		 *
		 * 对根分裂，为所有子页分配新缓冲区，
		 * 原页被覆写为包含指向新子页下行指针的
		 * 新根页。
		 */
		ptr = dist;
		if (!is_rootsplit)
		{
			/* 保存旧右链接与 NSN */
			oldrlink = GistPageGetOpaque(page)->rightlink;
			oldnsn = GistPageGetNSN(page);

			dist->buffer = buffer;
			dist->block.blkno = BufferGetBlockNumber(buffer);
			dist->page = PageGetTempPageCopySpecial(BufferGetPage(buffer));

			/* 清除除 F_LEAF 外的全部标志 */
			GistPageGetOpaque(dist->page)->flags = (is_leaf) ? F_LEAF : 0;

			ptr = ptr->next;
		}
		for (; ptr; ptr = ptr->next)
		{
			/* 分配新页 */
			ptr->buffer = gistNewBuffer(rel, heapRel);
			GISTInitBuffer(ptr->buffer, (is_leaf) ? F_LEAF : 0);
			ptr->page = BufferGetPage(ptr->buffer);
			ptr->block.blkno = BufferGetBlockNumber(ptr->buffer);
			PredicateLockPageSplit(rel,
								   BufferGetBlockNumber(buffer),
								   BufferGetBlockNumber(ptr->buffer));
		}

		/*
		 * 既然已知新页所在的块，设置下行指针
		 * 元组指向它们。
		 */
		for (ptr = dist; ptr; ptr = ptr->next)
		{
			ItemPointerSetBlockNumber(&(ptr->itup->t_tid), ptr->block.blkno);
			GistTupleSetValid(ptr->itup);
		}

		/*
		 * 若这是根分裂，我们在此直接用下行指针构造
		 * 新根页，而不要求调用者插入它们。
		 * 把新根页连同子页一起加入列表。
		 */
		if (is_rootsplit)
		{
			IndexTuple *downlinks;
			int			ndownlinks = 0;
			int			i;

			rootpg.buffer = buffer;
			rootpg.page = PageGetTempPageCopySpecial(BufferGetPage(rootpg.buffer));
			GistPageGetOpaque(rootpg.page)->flags = 0;

			/* 准备全部下行指针的向量 */
			for (ptr = dist; ptr; ptr = ptr->next)
				ndownlinks++;
			downlinks = palloc(sizeof(IndexTuple) * ndownlinks);
			for (i = 0, ptr = dist; ptr; ptr = ptr->next)
				downlinks[i++] = ptr->itup;

			rootpg.block.blkno = GIST_ROOT_BLKNO;
			rootpg.block.num = ndownlinks;
			rootpg.list = gistfillitupvec(downlinks, ndownlinks,
										  &(rootpg.lenlist));
			rootpg.itup = NULL;

			rootpg.next = dist;
			dist = &rootpg;
		}
		else
		{
			/* 准备要返回给调用者的分裂信息 */
			for (ptr = dist; ptr; ptr = ptr->next)
			{
				GISTPageSplitInfo *si = palloc(sizeof(GISTPageSplitInfo));

				si->buf = ptr->buffer;
				si->downlink = ptr->itup;
				*splitinfo = lappend(*splitinfo, si);
			}
		}

		/*
		 * 填充全部页。所有页都是新的，即刚分配的空
		 * 页，或旧页的临时副本。
		 */
		for (ptr = dist; ptr; ptr = ptr->next)
		{
			char	   *data = (char *) (ptr->list);

			for (int i = 0; i < ptr->block.num; i++)
			{
				IndexTuple	thistup = (IndexTuple) data;

				if (PageAddItem(ptr->page, (Item) data, IndexTupleSize(thistup), i + FirstOffsetNumber, false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add item to index page in \"%s\"", RelationGetRelationName(rel));

				/*
				 * 若这是第一个插入/更新的元组，让调用者
				 * 知道它落在哪一页。
				 */
				if (newblkno && ItemPointerEquals(&thistup->t_tid, &(*itup)->t_tid))
					*newblkno = ptr->block.blkno;

				data += IndexTupleSize(thistup);
			}

			/* 设置右链接 */
			if (ptr->next && ptr->block.blkno != GIST_ROOT_BLKNO)
				GistPageGetOpaque(ptr->page)->rightlink =
					ptr->next->block.blkno;
			else
				GistPageGetOpaque(ptr->page)->rightlink = oldrlink;

			/*
			 * 给除最右页外的全部页标上 follow-right
			 * 标志。下行指针一插入父就会清除，
			 * 但这确保若我们在那之前出错，索引仍
			 * 保持一致。（在缓冲构建模式下，任何错误都会
			 * 中止索引构建，因此不需要
			 * 这样做。）
			 */
			if (ptr->next && !is_rootsplit && markfollowright)
				GistMarkFollowRight(ptr->page);
			else
				GistClearFollowRight(ptr->page);

			/*
			 * 把原页的 NSN 复制到全部页。
			 * F_FOLLOW_RIGHT 标志确保扫描会跟随
			 * 右链接，直到下行指针被插入。
			 */
			GistPageSetNSN(ptr->page, oldnsn);
		}

		/*
		 * gistXLogSplit() 需要为很多页写 WAL，为此
		 * 准备 WAL 插入。注意：这里指定的页数与数据段数
		 * 必须与 gistXLogSplit() 中的计算一致！
		 */
		if (!is_build && RelationNeedsWAL(rel))
			XLogEnsureRecordSpace(npage, 1 + npage * 2);

		START_CRIT_SECTION();

		/*
		 * 必须在 XLogInsert 之前把缓冲区标脏，尽管我们下面
		 * 仍会改动它们的 opaque 字段。
		 */
		for (ptr = dist; ptr; ptr = ptr->next)
			MarkBufferDirty(ptr->buffer);
		if (BufferIsValid(leftchildbuf))
			MarkBufferDirty(leftchildbuf);

		/*
		 * 链中第一页是用于替换旧页的临时工作副本。
		 * 把它复制到旧页上。
		 */
		PageRestoreTempPage(dist->page, BufferGetPage(dist->buffer));
		dist->page = BufferGetPage(dist->buffer);

		/*
		 * 写 WAL 记录。
		 *
		 * 但若我们正在构建新索引，则还不把变更写入
		 * WAL。父与子之间的 LSN-NSN 互锁需要
		 * LSNs 永不回退，因此把 LSNs 设为小于任何日后可能
		 * 生成的真实或伪造未日志化 LSN 的值。（索引构建
		 * 期间不可能有并发扫描，因此还不需要
		 * 能检测并发分裂。）
		 */
		if (is_build)
			recptr = GistBuildLSN;
		else
		{
			if (RelationNeedsWAL(rel))
				recptr = gistXLogSplit(is_leaf,
									   dist, oldrlink, oldnsn, leftchildbuf,
									   markfollowright);
			else
				recptr = gistGetFakeLSN(rel);
		}

		for (ptr = dist; ptr; ptr = ptr->next)
			PageSetLSN(ptr->page, recptr);

		/*
		 * 把新的子缓冲区返回给调用者。
		 *
		 * 若这是根分裂，我们已以新根页的形式
		 * 插入了下行指针。因此可以释放
		 * 全部新缓冲区，只保持根页加锁。
		 */
		if (is_rootsplit)
		{
			for (ptr = dist->next; ptr; ptr = ptr->next)
				UnlockReleaseBuffer(ptr->buffer);
		}
	}
	else
	{
		/*
		 * 空间足够。若 ntup==0 则总会走到这里。
		 */
		START_CRIT_SECTION();

		/*
		 * 若有则删除旧 tuple，然后若有则插入新 tuple(s)。
		 * 若可能，使用 PageIndexTupleOverwrite 快路径。
		 */
		if (OffsetNumberIsValid(oldoffnum))
		{
			if (ntup == 1)
			{
				/* 一对一替换，因此使用 PageIndexTupleOverwrite */
				if (!PageIndexTupleOverwrite(page, oldoffnum, (Item) *itup,
											 IndexTupleSize(*itup)))
					elog(ERROR, "failed to add item to index page in \"%s\"",
						 RelationGetRelationName(rel));
			}
			else
			{
				/* 删除旧的，然后把新 tuple(s) 追加到页 */
				PageIndexTupleDelete(page, oldoffnum);
				gistfillbuffer(page, itup, ntup, InvalidOffsetNumber);
			}
		}
		else
		{
			/* 只把新元组追加到页末 */
			gistfillbuffer(page, itup, ntup, InvalidOffsetNumber);
		}

		MarkBufferDirty(buffer);

		if (BufferIsValid(leftchildbuf))
			MarkBufferDirty(leftchildbuf);

		if (is_build)
			recptr = GistBuildLSN;
		else
		{
			if (RelationNeedsWAL(rel))
			{
				OffsetNumber ndeloffs = 0,
							deloffs[1];

				if (OffsetNumberIsValid(oldoffnum))
				{
					deloffs[0] = oldoffnum;
					ndeloffs = 1;
				}

				recptr = gistXLogUpdate(buffer,
										deloffs, ndeloffs, itup, ntup,
										leftchildbuf);
			}
			else
				recptr = gistGetFakeLSN(rel);
		}
		PageSetLSN(page, recptr);

		if (newblkno)
			*newblkno = blkno;
	}

	/*
	 * 若我们为子页插入了下行指针，则在左子页上
	 * 设置 NSN 并清除 F_FOLLOW_RIGHT 标志，使并发扫描
	 * 知道：当且仅当它们在我们插入下行指针之前
	 * 看过父页时，才应跟随右链接。
	 *
	 * 注意我们是在写 WAL 记录*之后*做这些的。这意味着
	 * WAL 记录中可能的整页映像不包含这些
	 * 变更，即便页从整页映像恢复也必须重放它们。
	 * 这有先有鸡还是先有蛋的问题：若我们先更新
	 * 子页，就还不知道即将写入的 WAL 记录的
	 * recptr。
	 */
	if (BufferIsValid(leftchildbuf))
	{
		Page		leftpg = BufferGetPage(leftchildbuf);

		GistPageSetNSN(leftpg, recptr);
		GistClearFollowRight(leftpg);

		PageSetLSN(leftpg, recptr);
	}

	END_CRIT_SECTION();

	return is_split;
}

/*
 * 实际完成向 GiST 索引插入的例程。注意
 * 本例程假定在短生命周期的内存上下文中调用，
 * 因此不费心释放 palloc 的分配。
 */
void
gistdoinsert(Relation r, IndexTuple itup, Size freespace,
			 GISTSTATE *giststate, Relation heapRel, bool is_build)
{
	ItemId		iid;
	IndexTuple	idxtuple;
	GISTInsertStack firststack;
	GISTInsertStack *stack;
	GISTInsertState state;
	bool		xlocked = false;

	memset(&state, 0, sizeof(GISTInsertState));
	state.freespace = freespace;
	state.r = r;
	state.heapRel = heapRel;
	state.is_build = is_build;

	/* 从根开始 */
	firststack.blkno = GIST_ROOT_BLKNO;
	firststack.lsn = 0;
	firststack.retry_from_parent = false;
	firststack.parent = NULL;
	firststack.downlinkoffnum = InvalidOffsetNumber;
	state.stack = stack = &firststack;

	/*
	 * 沿罚分最小的路径下行，边走边用我们正在
	 * 插入的键更新父指针。若中途崩溃，
	 * 树仍一致，尽管可能的父
	 * 更新白做了。
	 */
	for (;;)
	{
		/*
		 * 若下行树时分裂了内部页，必须
		 * 在父处重试。（通常下面的 LSN-NSN 互锁也会
		 * 捕获这一点并导致重试。但索引构建期间
		 * 不更新 LSNs。）
		 */
		while (stack->retry_from_parent)
		{
			if (xlocked)
				LockBuffer(stack->buffer, GIST_UNLOCK);
			xlocked = false;
			ReleaseBuffer(stack->buffer);
			state.stack = stack = stack->parent;
		}

		if (XLogRecPtrIsInvalid(stack->lsn))
			stack->buffer = ReadBuffer(state.r, stack->blkno);

		/*
		 * 先乐观地取共享锁。若之后需要更新该页，
		 * 再换成排他锁。
		 */
		if (!xlocked)
		{
			LockBuffer(stack->buffer, GIST_SHARE);
			gistcheckpage(state.r, stack->buffer);
		}

		stack->page = (Page) BufferGetPage(stack->buffer);
		stack->lsn = xlocked ?
			PageGetLSN(stack->page) : BufferGetLSNAtomic(stack->buffer);
		Assert(!RelationNeedsWAL(state.r) || !XLogRecPtrIsInvalid(stack->lsn));

		/*
		 * 若此页已分裂但因插入后端在插入前崩溃
		 * 而未把下行指针插入父，现在
		 * 修复它。
		 */
		if (GistFollowRight(stack->page))
		{
			if (!xlocked)
			{
				LockBuffer(stack->buffer, GIST_UNLOCK);
				LockBuffer(stack->buffer, GIST_EXCLUSIVE);
				xlocked = true;
				/* 我们解锁时可能已有人完成了分裂 */
				if (!GistFollowRight(stack->page))
					continue;
			}
			gistfixsplit(&state, giststate);

			UnlockReleaseBuffer(stack->buffer);
			xlocked = false;
			state.stack = stack = stack->parent;
			continue;
		}

		if ((stack->blkno != GIST_ROOT_BLKNO &&
			 stack->parent->lsn < GistPageGetNSN(stack->page)) ||
			GistPageIsDeleted(stack->page))
		{
			/*
			 * 检测到并发分裂或页删除。不再能
			 * 保证此页的下行指针仍与我们正在插入的
			 * 元组一致，因此回到父并
			 * 重新选择最佳子页。
			 */
			UnlockReleaseBuffer(stack->buffer);
			xlocked = false;
			state.stack = stack = stack->parent;
			continue;
		}

		if (!GistPageIsLeaf(stack->page))
		{
			/*
			 * 这是内部页，因此继续沿树下行。
			 * 找到插入罚分最小的子节点。
			 */
			BlockNumber childblkno;
			IndexTuple	newtup;
			GISTInsertStack *item;
			OffsetNumber downlinkoffnum;

			downlinkoffnum = gistchoose(state.r, stack->page, itup, giststate);
			iid = PageGetItemId(stack->page, downlinkoffnum);
			idxtuple = (IndexTuple) PageGetItem(stack->page, iid);
			childblkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));

			/*
			 * 检查这不是 9.1 之前残留的无效元组
			 */
			if (GistTupleIsInvalid(idxtuple))
				ereport(ERROR,
						(errmsg("index \"%s\" contains an inner tuple marked as invalid",
								RelationGetRelationName(r)),
						 errdetail("This is caused by an incomplete page split at crash recovery before upgrading to PostgreSQL 9.1."),
						 errhint("Please REINDEX it.")));

			/*
			 * 检查代表目标子节点的键是否与我们正在
			 * 插入的键一致。若不一致则更新它。
			 */
			newtup = gistgetadjusted(state.r, idxtuple, itup, giststate);
			if (newtup)
			{
				/*
				 * 把共享锁换成排他锁。当心，我们解锁/加锁
				 * 期间页可能变化……
				 */
				if (!xlocked)
				{
					LockBuffer(stack->buffer, GIST_UNLOCK);
					LockBuffer(stack->buffer, GIST_EXCLUSIVE);
					xlocked = true;
					stack->page = (Page) BufferGetPage(stack->buffer);

					if (PageGetLSN(stack->page) != stack->lsn)
					{
						/* 解锁期间页已变更，重试 */
						continue;
					}
				}

				/*
				 * 更新该元组。
				 *
				 * gistinserttuple() 返回后我们仍持有锁，但它
				 * 可能不得不分裂页以使更新后的元组
				 * 能放下。那时更新后的元组可能迁到
				 * 分裂的另一半，因此必须回到父，
				 * 再下行到对新元组更合适的
				 * 那一半。
				 */
				if (gistinserttuple(&state, stack, giststate, newtup,
									downlinkoffnum))
				{
					/*
					 * 若这是根分裂，根页继续作为
					 * 父，更新后的元组去了某个子页，
					 * 因此只需从根
					 * 页重试。
					 */
					if (stack->blkno != GIST_ROOT_BLKNO)
					{
						UnlockReleaseBuffer(stack->buffer);
						xlocked = false;
						state.stack = stack = stack->parent;
					}
					continue;
				}
			}
			LockBuffer(stack->buffer, GIST_UNLOCK);
			xlocked = false;

			/* 下行到所选子页 */
			item = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
			item->blkno = childblkno;
			item->parent = stack;
			item->downlinkoffnum = downlinkoffnum;
			state.stack = stack = item;
		}
		else
		{
			/*
			 * 叶子页。插入新键。下行途中我们已更新全部
			 * 父，但若放不下可能还要分裂该页。
			 * gistinserttuple() 会处理。
			 */

			/*
			 * 把共享锁换成排他锁。小心，解锁/加锁
			 * 期间页可能变化……
			 */
			if (!xlocked)
			{
				LockBuffer(stack->buffer, GIST_UNLOCK);
				LockBuffer(stack->buffer, GIST_EXCLUSIVE);
				xlocked = true;
				stack->page = (Page) BufferGetPage(stack->buffer);
				stack->lsn = PageGetLSN(stack->page);

				if (stack->blkno == GIST_ROOT_BLKNO)
				{
					/*
					 * 唯一能从叶子变成内部页的
					 * 是根页，因此对根应再检查一次
					 */
					if (!GistPageIsLeaf(stack->page))
					{
						/*
						 * 极少见情况：解锁/加锁期间页数为 1 的
						 * 索引增大了
						 */
						LockBuffer(stack->buffer, GIST_UNLOCK);
						xlocked = false;
						continue;
					}

					/*
					 * 我们不必检查根分裂，因为检查
					 * 叶子/内部就足以识别根的分裂
					 */
				}
				else if ((GistFollowRight(stack->page) ||
						  stack->parent->lsn < GistPageGetNSN(stack->page)) ||
						 GistPageIsDeleted(stack->page))
				{
					/*
					 * 我们短暂解锁期间页被分裂或
					 * 删除。回到父。
					 */
					UnlockReleaseBuffer(stack->buffer);
					xlocked = false;
					state.stack = stack = stack->parent;
					continue;
				}
			}

			/* 现在 state.stack->(page, buffer and blkno) 指向叶子页 */

			gistinserttuple(&state, stack, giststate, itup,
							InvalidOffsetNumber);
			LockBuffer(stack->buffer, GIST_UNLOCK);

			/* 退出前释放我们可能仍持有的任何钉住 */
			for (; stack; stack = stack->parent)
				ReleaseBuffer(stack->buffer);
			break;
		}
	}
}

/*
 * 遍历树，找到从根页到指定 "child" 块的路径。
 *
 * 返回新的插入栈，从 "child" 的父开始，向上
 * 直到根。*downlinkoffnum 设为 child 的直接父中
 * 下行指针的偏移。
 *
 * 为防止死锁，这里一次只应锁一页。
 */
static GISTInsertStack *
gistFindPath(Relation r, BlockNumber child, OffsetNumber *downlinkoffnum)
{
	Page		page;
	Buffer		buffer;
	OffsetNumber i,
				maxoff;
	ItemId		iid;
	IndexTuple	idxtuple;
	List	   *fifo;
	GISTInsertStack *top,
			   *ptr;
	BlockNumber blkno;

	top = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
	top->blkno = GIST_ROOT_BLKNO;
	top->downlinkoffnum = InvalidOffsetNumber;

	fifo = list_make1(top);
	while (fifo != NIL)
	{
		/* 取下一要访问的页 */
		top = linitial(fifo);
		fifo = list_delete_first(fifo);

		buffer = ReadBuffer(r, top->blkno);
		LockBuffer(buffer, GIST_SHARE);
		gistcheckpage(r, buffer);
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsLeaf(page))
		{
			/*
			 * 因为我们自顶向下扫描索引，队列中其余页
			 * 也必定都是叶子页。
			 */
			UnlockReleaseBuffer(buffer);
			break;
		}

		/* 目前，内部页从不会被删除 */
		Assert(!GistPageIsDeleted(page));

		top->lsn = BufferGetLSNAtomic(buffer);

		/*
		 * 若设置了 F_FOLLOW_RIGHT，右侧页没有
		 * 下行指针。这通常不应发生……
		 */
		if (GistFollowRight(page))
			elog(ERROR, "concurrent GiST page split was incomplete");

		if (top->parent && top->parent->lsn < GistPageGetNSN(page) &&
			GistPageGetOpaque(page)->rightlink != InvalidBlockNumber /* 健全性检查 */ )
		{
			/*
			 * 我们看别处时页发生了分裂。扫描父时
			 * 没看到指向右页的下行指针，因此现在
			 * 把它加入队列。
			 *
			 * 把右页放到队列前面，以便接下来
			 * 访问它。这很重要，因为若这是紧挨叶子之上的
			 * 最低内部层，我们可能已排队了一些
			 * 叶子页，并假定叶子页后面不能有
			 * 非叶子页。
			 */
			ptr = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
			ptr->blkno = GistPageGetOpaque(page)->rightlink;
			ptr->downlinkoffnum = InvalidOffsetNumber;
			ptr->parent = top->parent;

			fifo = lcons(ptr, fifo);
		}

		maxoff = PageGetMaxOffsetNumber(page);

		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			iid = PageGetItemId(page, i);
			idxtuple = (IndexTuple) PageGetItem(page, iid);
			blkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
			if (blkno == child)
			{
				/* 找到了！ */
				UnlockReleaseBuffer(buffer);
				*downlinkoffnum = i;
				return top;
			}
			else
			{
				/* 把此子页追加到稍后要访问的页列表 */
				ptr = (GISTInsertStack *) palloc0(sizeof(GISTInsertStack));
				ptr->blkno = blkno;
				ptr->downlinkoffnum = i;
				ptr->parent = top;

				fifo = lappend(fifo, ptr);
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	elog(ERROR, "failed to re-find parent of a page in index \"%s\", block %u",
		 RelationGetRelationName(r), child);
	return NULL;				/* 让编译器安静 */
}

/*
 * 更新栈，使 child->parent 成为 child 的正确父。
 * 进入时 child->parent 必须已排他锁定，退出时
 * 仍如此，但可能已不再是同一页。
 */
static void
gistFindCorrectParent(Relation r, GISTInsertStack *child, bool is_build)
{
	GISTInsertStack *parent = child->parent;
	ItemId		iid;
	IndexTuple	idxtuple;
	OffsetNumber maxoff;
	GISTInsertStack *ptr;

	gistcheckpage(r, parent->buffer);
	parent->page = (Page) BufferGetPage(parent->buffer);
	maxoff = PageGetMaxOffsetNumber(parent->page);

	/* 检查下行指针是否仍在原先位置 */
	if (child->downlinkoffnum != InvalidOffsetNumber && child->downlinkoffnum <= maxoff)
	{
		iid = PageGetItemId(parent->page, child->downlinkoffnum);
		idxtuple = (IndexTuple) PageGetItem(parent->page, iid);
		if (ItemPointerGetBlockNumber(&(idxtuple->t_tid)) == child->blkno)
			return;				/* 仍在那里 */
	}

	/*
	 * 自我们查看以来该页已变更。正常运行时，页的
	 * 每次更新都会改变其 LSN，因此我们记下的 LSN
	 * 也应已改变。
	 *
	 * 但在索引构建期间，我们直到建完索引才 WAL 记录
	 * 变更，因此 LSN 不变。索引构建期间没有并发
	 * 活动，但我们自己可能已改过父。
	 *
	 * 若 child->downlinkoffnum 无效也会到这里。那发生在
	 * 更早对本函数的调用更新了 'parent'（针对其
	 * 孙），而该调用不得不
	 * 右移时。
	 */
	Assert(parent->lsn != PageGetLSN(parent->page) || is_build ||
		   child->downlinkoffnum == InvalidOffsetNumber);

	/*
	 * 扫描该页以重新找到下行指针。若页曾分裂，它可能
	 * 已移到不同页，因此跟随右链接直到找到
	 * 它。
	 */
	while (true)
	{
		OffsetNumber i;

		maxoff = PageGetMaxOffsetNumber(parent->page);
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			iid = PageGetItemId(parent->page, i);
			idxtuple = (IndexTuple) PageGetItem(parent->page, iid);
			if (ItemPointerGetBlockNumber(&(idxtuple->t_tid)) == child->blkno)
			{
				/* 是的！！，找到了 */
				child->downlinkoffnum = i;
				return;
			}
		}

		parent->blkno = GistPageGetOpaque(parent->page)->rightlink;
		parent->downlinkoffnum = InvalidOffsetNumber;
		UnlockReleaseBuffer(parent->buffer);
		if (parent->blkno == InvalidBlockNumber)
		{
			/*
			 * 链已结束仍未找到父。这是根分裂时
			 * 极少见的情况。
			 */
			break;
		}
		parent->buffer = ReadBuffer(r, parent->blkno);
		LockBuffer(parent->buffer, GIST_EXCLUSIVE);
		gistcheckpage(r, parent->buffer);
		parent->page = (Page) BufferGetPage(parent->buffer);
	}

	/*
	 * 糟糕！！，我们需要搜索树来找父……，但在此之前应
	 * 释放全部旧父
	 */

	ptr = child->parent->parent;	/* child->parent 已在上面释放 */
	while (ptr)
	{
		ReleaseBuffer(ptr->buffer);
		ptr = ptr->parent;
	}

	/* 好，找新路径 */
	ptr = parent = gistFindPath(r, child->blkno, &child->downlinkoffnum);

	/* 按调用者所预期读取全部缓冲区 */
	/* 注意这里不加锁，也不对它们做 gistcheckpage！ */
	while (ptr)
	{
		ptr->buffer = ReadBuffer(r, ptr->blkno);
		ptr->page = (Page) BufferGetPage(ptr->buffer);
		ptr = ptr->parent;
	}

	/* 把新的父链安装到栈上 */
	child->parent = parent;

	/* 递归调用以进行正常处理 */
	LockBuffer(child->parent->buffer, GIST_EXCLUSIVE);
	gistFindCorrectParent(r, child, is_build);
}

/*
 * 为 'buf' 中的页形成下行指针。
 */
static IndexTuple
gistformdownlink(Relation rel, Buffer buf, GISTSTATE *giststate,
				 GISTInsertStack *stack, bool is_build)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber maxoff;
	OffsetNumber offset;
	IndexTuple	downlink = NULL;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offset = FirstOffsetNumber; offset <= maxoff; offset = OffsetNumberNext(offset))
	{
		IndexTuple	ituple = (IndexTuple)
			PageGetItem(page, PageGetItemId(page, offset));

		if (downlink == NULL)
			downlink = CopyIndexTuple(ituple);
		else
		{
			IndexTuple	newdownlink;

			newdownlink = gistgetadjusted(rel, downlink, ituple,
										  giststate);
			if (newdownlink)
				downlink = newdownlink;
		}
	}

	/*
	 * 若页完全为空，无法为其形成有意义的下行指针。
	 * 但我们必须为该页插入下行指针。任何键都可以，
	 * 只要与父页的下行指针一致，以便能合法
	 * 插入父。能匹配尽可能少扫描的最小键最好，
	 * 以免扫描做无用功，但我们不知道如何构造它。
	 * 因此只用被分裂的原页的下行指针——那离
	 * 最优尽可能远，但
	 * 够用……
	 */
	if (!downlink)
	{
		ItemId		iid;

		LockBuffer(stack->parent->buffer, GIST_EXCLUSIVE);
		gistFindCorrectParent(rel, stack, is_build);
		iid = PageGetItemId(stack->parent->page, stack->downlinkoffnum);
		downlink = (IndexTuple) PageGetItem(stack->parent->page, iid);
		downlink = CopyIndexTuple(downlink);
		LockBuffer(stack->parent->buffer, GIST_UNLOCK);
	}

	ItemPointerSetBlockNumber(&(downlink->t_tid), BufferGetBlockNumber(buf));
	GistTupleSetValid(downlink);

	return downlink;
}


/*
 * 完成 state->stack->page 上未完成的分裂。
 */
static void
gistfixsplit(GISTInsertState *state, GISTSTATE *giststate)
{
	GISTInsertStack *stack = state->stack;
	Buffer		buf;
	Page		page;
	List	   *splitinfo = NIL;

	ereport(LOG,
			(errmsg("fixing incomplete split in index \"%s\", block %u",
					RelationGetRelationName(state->r), stack->blkno)));

	Assert(GistFollowRight(stack->page));
	Assert(OffsetNumberIsValid(stack->downlinkoffnum));

	buf = stack->buffer;

	/*
	 * 跟随右链接读取分裂页链。为每页
	 * 构造下行指针元组。
	 */
	for (;;)
	{
		GISTPageSplitInfo *si = palloc(sizeof(GISTPageSplitInfo));
		IndexTuple	downlink;

		page = BufferGetPage(buf);

		/* 形成要插入父的新下行指针元组 */
		downlink = gistformdownlink(state->r, buf, giststate, stack, state->is_build);

		si->buf = buf;
		si->downlink = downlink;

		splitinfo = lappend(splitinfo, si);

		if (GistFollowRight(page))
		{
			/* 锁定下一页 */
			buf = ReadBuffer(state->r, GistPageGetOpaque(page)->rightlink);
			LockBuffer(buf, GIST_EXCLUSIVE);
		}
		else
			break;
	}

	/* 插入下行指针 */
	gistfinishsplit(state, stack, giststate, splitinfo, false);
}

/*
 * 在 stack->buffer 中插入或替换元组。若 'oldoffnum' 有效，则
 * 替换 'oldoffnum' 处的元组，否则作为新元组插入。
 * 'stack' 表示从根到正在更新的页的路径。
 *
 * 调用者必须持有 stack->buffer 上的排他锁。返回时锁仍
 * 持有，但若页已分裂则页上可能不含所插入的元组。
 * 若页发生分裂函数返回 true，否则
 * 返回 false。
 */
static bool
gistinserttuple(GISTInsertState *state, GISTInsertStack *stack,
				GISTSTATE *giststate, IndexTuple tuple, OffsetNumber oldoffnum)
{
	return gistinserttuples(state, stack, giststate, &tuple, 1, oldoffnum,
							InvalidBuffer, InvalidBuffer, false, false);
}

/* ----------------
 * gistinserttuple() 的扩展主力版本。此版本允许
 * 插入多个元组，或用多个元组替换单个元组。
 * 这用于页分裂时递归更新父中的
 * 下行指针。
 *
 * 若 leftchild 与 rightchild 有效，我们正在为 rightchild
 * 插入/替换下行指针，且 leftchild 是其左兄弟。我们
 * 清除 F_FOLLOW_RIGHT 标志并更新 leftchild 上的 NSN，与
 * 下行指针的插入原子地一起完成。
 *
 * 为避免持锁过久，在向上递归更新父时，
 * 这里的加锁方式有点特别。进入时，
 * 调用者必须持有 stack->buffer 上的排他锁，以及
 * 若给定则还有 leftchild 与 rightchild。返回时：
 *
 *	- 若 'unlockbuf' 为真，释放 stack->buffer 上的锁。但页
 *	  始终保持钉住。
 *	- 若 'unlockleftchild' 为真，释放 'leftchild' 上的锁。页
 *	  保持钉住。
 *	- 'rightchild' 上的锁与钉住总是被释放。
 *
 * 若页不得不分裂则返回 'true'。注意若页
 * 分裂了，插入/更新的元组可能被插入到
 * stack->buffer 的右兄弟而不是 stack->buffer 本身。
 */
static bool
gistinserttuples(GISTInsertState *state, GISTInsertStack *stack,
				 GISTSTATE *giststate,
				 IndexTuple *tuples, int ntup, OffsetNumber oldoffnum,
				 Buffer leftchild, Buffer rightchild,
				 bool unlockbuf, bool unlockleftchild)
{
	List	   *splitinfo;
	bool		is_split;

	/*
	 * 就在我们打算修改该页之前，检查是否有任何
	 * rw 冲突（在可串行化隔离级别）
	 */
	CheckForSerializableConflictIn(state->r, NULL, BufferGetBlockNumber(stack->buffer));

	/* 把 tuple(s) 插入页，必要时分裂该页 */
	is_split = gistplacetopage(state->r, state->freespace, giststate,
							   stack->buffer,
							   tuples, ntup,
							   oldoffnum, NULL,
							   leftchild,
							   &splitinfo,
							   true,
							   state->heapRel,
							   state->is_build);

	/*
	 * 若页已分裂、在向上递归之前，释放
	 * 子页上的锁。更新父时不必继续
	 * 锁住它们。
	 */
	if (BufferIsValid(rightchild))
		UnlockReleaseBuffer(rightchild);
	if (BufferIsValid(leftchild) && unlockleftchild)
		LockBuffer(leftchild, GIST_UNLOCK);

	/*
	 * 若不得不分裂，在父中插入/更新下行指针。若
	 * 调用者要求我们释放 stack->buffer 上的锁，告诉
	 * gistfinishsplit() 一旦安全就那样做。若我们
	 * 不必分裂，则自己释放。
	 */
	if (splitinfo)
		gistfinishsplit(state, stack, giststate, splitinfo, unlockbuf);
	else if (unlockbuf)
		LockBuffer(stack->buffer, GIST_UNLOCK);

	return is_split;
}

/*
 * 通过在父页中插入/更新下行指针来完成未完成的分裂。
 * 'splitinfo' 包含分裂涉及的全部子页，
 * 从左到右。
 *
 * 进入时，调用者必须持有 stack->buffer 与 'splitinfo' 中全部
 * 子页上的锁。若 'unlockbuf' 为真，返回时释放
 * stack->buffer 上的锁。子页总是被解锁并取消钉住。
 */
static void
gistfinishsplit(GISTInsertState *state, GISTInsertStack *stack,
				GISTSTATE *giststate, List *splitinfo, bool unlockbuf)
{
	GISTPageSplitInfo *right;
	GISTPageSplitInfo *left;
	IndexTuple	tuples[2];

	/* 分裂总是至少包含两半 */
	Assert(list_length(splitinfo) >= 2);

	/*
	 * 我们需要为每个新页插入下行指针，并更新分裂中
	 * 原（最左）页的下行指针。从最右页开始，
	 * 一次插入一个下行指针，直到只剩两页。
	 * 最后把最后一个新页的下行指针插入与更新
	 * 原页下行指针作为一次操作完成。
	 */
	LockBuffer(stack->parent->buffer, GIST_EXCLUSIVE);

	/*
	 * 从右到左为兄弟插入下行指针，直到只剩
	 * 两个兄弟。
	 */
	for (int pos = list_length(splitinfo) - 1; pos > 1; pos--)
	{
		right = (GISTPageSplitInfo *) list_nth(splitinfo, pos);
		left = (GISTPageSplitInfo *) list_nth(splitinfo, pos - 1);

		gistFindCorrectParent(state->r, stack, state->is_build);
		if (gistinserttuples(state, stack->parent, giststate,
							 &right->downlink, 1,
							 InvalidOffsetNumber,
							 left->buf, right->buf, false, false))
		{
			/*
			 * 若父页已分裂，现有下行指针可能已
			 * 移动。
			 */
			stack->downlinkoffnum = InvalidOffsetNumber;
		}
		/* gistinserttuples() 已释放 right->buf 上的锁。 */
	}

	right = (GISTPageSplitInfo *) lsecond(splitinfo);
	left = (GISTPageSplitInfo *) linitial(splitinfo);

	/*
	 * 最后为剩余的右页插入下行指针，并更新
	 * 原页的下行指针，使之不再包含被移到
	 * 新页的那些元组。
	 */
	tuples[0] = left->downlink;
	tuples[1] = right->downlink;
	gistFindCorrectParent(state->r, stack, state->is_build);
	(void) gistinserttuples(state, stack->parent, giststate,
							tuples, 2,
							stack->downlinkoffnum,
							left->buf, right->buf,
							true,	/* 解锁父 */
							unlockbuf	/* 若调用者希望则解锁
										 * stack->buffer */
		);

	/*
	 * 我们更新时下行指针可能已移动。即便页
	 * 未分裂也如此，因为 gistinserttuples() 通过删除再
	 * 重新插入来实现更新旧元组！
	 */
	stack->downlinkoffnum = InvalidOffsetNumber;

	Assert(left->buf == stack->buffer);

	/*
	 * 若我们因在为插入新元组而下行树时不得不调整
	 * 内部页上的下行指针而分裂了该页，
	 * 则此页可能已不再是新元组的正确页。指向
	 * 此页的下行指针可能已不再覆盖新元组，它可能
	 * 需要去到新创建的右兄弟。告诉调用者
	 * 沿栈走回，在父处重新检查应插入
	 * 哪一页。
	 *
	 * 通常，树下行期间的 LSN-NSN 互锁也会
	 * 检测到发生了（由我们自己的）并发分裂，并导致我们
	 * 在父处重试。但该机制在索引构建期间
	 * 不起作用，因为我们不写 WAL，也不更新
	 * LSNs。
	 */
	stack->retry_from_parent = true;
}

/*
 * gistSplit -- 分裂树中的一页并填充用于
 * XLOG 与实际写缓冲区的结构。函数是递归的，即
 * 会一直分裂，直到键能装进每一页。
 */
SplitedPageLayout *
gistSplit(Relation r,
		  Page page,
		  IndexTuple *itup,		/* 包含压缩后的项 */
		  int len,
		  GISTSTATE *giststate)
{
	IndexTuple *lvectup,
			   *rvectup;
	GistSplitVector v;
	int			i;
	SplitedPageLayout *res = NULL;

	/* 这绝不该递归很深，但宁可谨慎 */
	check_stack_depth();

	/* 分裂空页没有意义 */
	Assert(len > 0);

	/*
	 * 若单个元组都装不进一页，再怎么分裂也
	 * 无济于事。
	 */
	if (len == 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
						IndexTupleSize(itup[0]), GiSTPageSize,
						RelationGetRelationName(r))));

	memset(v.spl_lisnull, true,
		   sizeof(bool) * giststate->nonLeafTupdesc->natts);
	memset(v.spl_risnull, true,
		   sizeof(bool) * giststate->nonLeafTupdesc->natts);
	gistSplitByKey(r, page, itup, len, giststate, &v, 0);

	/* 形成左、右向量 */
	lvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (len + 1));
	rvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (len + 1));

	for (i = 0; i < v.splitVector.spl_nleft; i++)
		lvectup[i] = itup[v.splitVector.spl_left[i] - 1];

	for (i = 0; i < v.splitVector.spl_nright; i++)
		rvectup[i] = itup[v.splitVector.spl_right[i] - 1];

	/* 完成分裂（可能还需要再分裂） */
	if (!gistfitpage(rvectup, v.splitVector.spl_nright))
	{
		res = gistSplit(r, page, rvectup, v.splitVector.spl_nright, giststate);
	}
	else
	{
		ROTATEDIST(res);
		res->block.num = v.splitVector.spl_nright;
		res->list = gistfillitupvec(rvectup, v.splitVector.spl_nright, &(res->lenlist));
		res->itup = gistFormTuple(giststate, r, v.spl_rattr, v.spl_risnull, false);
	}

	if (!gistfitpage(lvectup, v.splitVector.spl_nleft))
	{
		SplitedPageLayout *resptr,
				   *subres;

		resptr = subres = gistSplit(r, page, lvectup, v.splitVector.spl_nleft, giststate);

		/* 安装到列表尾 */
		while (resptr->next)
			resptr = resptr->next;

		resptr->next = res;
		res = subres;
	}
	else
	{
		ROTATEDIST(res);
		res->block.num = v.splitVector.spl_nleft;
		res->list = gistfillitupvec(lvectup, v.splitVector.spl_nleft, &(res->lenlist));
		res->itup = gistFormTuple(giststate, r, v.spl_lattr, v.spl_lisnull, false);
	}

	return res;
}

/*
 * 创建 GISTSTATE 并用关于索引的信息填充它
 */
GISTSTATE *
initGISTstate(Relation index)
{
	GISTSTATE  *giststate;
	MemoryContext scanCxt;
	MemoryContext oldCxt;
	int			i;

	/* 保护 GISTSTATE 中定长数组的安全检查 */
	if (index->rd_att->natts > INDEX_MAX_KEYS)
		elog(ERROR, "numberOfAttributes %d > %d",
			 index->rd_att->natts, INDEX_MAX_KEYS);

	/* 创建将持有 GISTSTATE 的内存上下文 */
	scanCxt = AllocSetContextCreate(CurrentMemoryContext,
									"GiST scan context",
									ALLOCSET_DEFAULT_SIZES);
	oldCxt = MemoryContextSwitchTo(scanCxt);

	/* 创建并填充 GISTSTATE */
	giststate = (GISTSTATE *) palloc(sizeof(GISTSTATE));

	giststate->scanCxt = scanCxt;
	giststate->tempCxt = scanCxt;	/* 若需要调用者必须更改此项 */
	giststate->leafTupdesc = index->rd_att;

	/*
	 * 非叶子索引元组的截断 tupdesc，其中不包含
	 * INCLUDE 属性。
	 *
	 * 用于在元组调整与页分裂期间形成元组。
	 * B-tree 为每个截断元组创建缩短的元组描述符，
	 * 因为它做得较少：页分裂时不必形成截断
	 * 元组。另外，B-tree 也不像 GiST 那样在
	 * 内部页上调整元组。
	 */
	giststate->nonLeafTupdesc = CreateTupleDescCopyConstr(index->rd_att);
	giststate->nonLeafTupdesc->natts =
		IndexRelationGetNumberOfKeyAttributes(index);

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
	{
		fmgr_info_copy(&(giststate->consistentFn[i]),
					   index_getprocinfo(index, i + 1, GIST_CONSISTENT_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->unionFn[i]),
					   index_getprocinfo(index, i + 1, GIST_UNION_PROC),
					   scanCxt);

		/* 操作符类不必提供 Compress 方法 */
		if (OidIsValid(index_getprocid(index, i + 1, GIST_COMPRESS_PROC)))
			fmgr_info_copy(&(giststate->compressFn[i]),
						   index_getprocinfo(index, i + 1, GIST_COMPRESS_PROC),
						   scanCxt);
		else
			giststate->compressFn[i].fn_oid = InvalidOid;

		/* 操作符类不必提供 Decompress 方法 */
		if (OidIsValid(index_getprocid(index, i + 1, GIST_DECOMPRESS_PROC)))
			fmgr_info_copy(&(giststate->decompressFn[i]),
						   index_getprocinfo(index, i + 1, GIST_DECOMPRESS_PROC),
						   scanCxt);
		else
			giststate->decompressFn[i].fn_oid = InvalidOid;

		fmgr_info_copy(&(giststate->penaltyFn[i]),
					   index_getprocinfo(index, i + 1, GIST_PENALTY_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->picksplitFn[i]),
					   index_getprocinfo(index, i + 1, GIST_PICKSPLIT_PROC),
					   scanCxt);
		fmgr_info_copy(&(giststate->equalFn[i]),
					   index_getprocinfo(index, i + 1, GIST_EQUAL_PROC),
					   scanCxt);

		/* 操作符类不必提供 Distance 方法 */
		if (OidIsValid(index_getprocid(index, i + 1, GIST_DISTANCE_PROC)))
			fmgr_info_copy(&(giststate->distanceFn[i]),
						   index_getprocinfo(index, i + 1, GIST_DISTANCE_PROC),
						   scanCxt);
		else
			giststate->distanceFn[i].fn_oid = InvalidOid;

		/* 操作符类不必提供 Fetch 方法 */
		if (OidIsValid(index_getprocid(index, i + 1, GIST_FETCH_PROC)))
			fmgr_info_copy(&(giststate->fetchFn[i]),
						   index_getprocinfo(index, i + 1, GIST_FETCH_PROC),
						   scanCxt);
		else
			giststate->fetchFn[i].fn_oid = InvalidOid;

		/*
		 * 若索引列指定了排序规则，做比较时应
		 * 遵守它。不过，我们可能对不可排序的被索引
		 * 数据类型有可排序的存储类型。若没有索引
		 * 排序规则，则指定默认排序规则，以防支持
		 * 函数需要排序规则。若支持函数不关心
		 * 排序规则这无害，因此我们无条件地
		 * 这样做。（也可以调用 get_typcollation，
		 * 但那似乎是昂贵的过度做法——不会有
		 * GiST 存储类型带非默认排序规则的情形。）
		 */
		if (OidIsValid(index->rd_indcollation[i]))
			giststate->supportCollation[i] = index->rd_indcollation[i];
		else
			giststate->supportCollation[i] = DEFAULT_COLLATION_OID;
	}

	/* INCLUDE 属性没有操作符类信息 */
	for (; i < index->rd_att->natts; i++)
	{
		giststate->consistentFn[i].fn_oid = InvalidOid;
		giststate->unionFn[i].fn_oid = InvalidOid;
		giststate->compressFn[i].fn_oid = InvalidOid;
		giststate->decompressFn[i].fn_oid = InvalidOid;
		giststate->penaltyFn[i].fn_oid = InvalidOid;
		giststate->picksplitFn[i].fn_oid = InvalidOid;
		giststate->equalFn[i].fn_oid = InvalidOid;
		giststate->distanceFn[i].fn_oid = InvalidOid;
		giststate->fetchFn[i].fn_oid = InvalidOid;
		giststate->supportCollation[i] = InvalidOid;
	}

	MemoryContextSwitchTo(oldCxt);

	return giststate;
}

void
freeGISTstate(GISTSTATE *giststate)
{
	/* 删除 scanCxt 就足够了 */
	MemoryContextDelete(giststate->scanCxt);
}

/*
 * gistprunepage() -- 尝试从给定页移除 LP_DEAD 项。
 * 函数假定缓冲区已排他锁定。
 */
static void
gistprunepage(Relation rel, Page page, Buffer buffer, Relation heapRel)
{
	OffsetNumber deletable[MaxIndexTuplesPerPage];
	int			ndeletable = 0;
	OffsetNumber offnum,
				maxoff;

	Assert(GistPageIsLeaf(page));

	/*
	 * 扫描全部项，看根据 LP_DEAD 标志哪些需要
	 * 删除。
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsDead(itemId))
			deletable[ndeletable++] = offnum;
	}

	if (ndeletable > 0)
	{
		TransactionId snapshotConflictHorizon = InvalidTransactionId;

		if (XLogStandbyInfoActive() && RelationNeedsWAL(rel))
			snapshotConflictHorizon =
				index_compute_xid_horizon_for_tuples(rel, heapRel, buffer,
													 deletable, ndeletable);

		START_CRIT_SECTION();

		PageIndexMultiDelete(page, deletable, ndeletable);

		/*
		 * 把该页标记为不含任何 LP_DEAD 项。这不一定
		 * 为真（可能有些刚被标记、但未包含在我们的
		 * 目标项列表中），但几乎总是为真，似乎不值得
		 * 再扫一页来检查。记住 F_HAS_GARBAGE 反正
		 * 只是提示。
		 */
		GistClearPageHasGarbage(page);

		MarkBufferDirty(buffer);

		/* XLOG 相关 */
		if (RelationNeedsWAL(rel))
		{
			XLogRecPtr	recptr;

			recptr = gistXLogDelete(buffer,
									deletable, ndeletable,
									snapshotConflictHorizon,
									heapRel);

			PageSetLSN(page, recptr);
		}
		else
			PageSetLSN(page, gistGetFakeLSN(rel));

		END_CRIT_SECTION();
	}

	/*
	 * 注意：若未找到任何 LP_DEAD 项，则该页的
	 * F_HAS_GARBAGE 提示位被错误地置位。不过我们不费心
	 * 单独写一次来清除它。我们会在分裂该页时
	 * 清除它。
	 */
}
