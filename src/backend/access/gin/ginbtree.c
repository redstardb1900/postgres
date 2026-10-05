/*-------------------------------------------------------------------------
 *
 * ginbtree.c
 *	  Postgres 倒排索引访问方法的页实用例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginbtree.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static void ginFindParents(GinBtree btree, GinBtreeStack *stack);
static bool ginPlaceToPage(GinBtree btree, GinBtreeStack *stack,
						   void *insertdata, BlockNumber updateblkno,
						   Buffer childbuf, GinStatsData *buildStats);
static void ginFinishSplit(GinBtree btree, GinBtreeStack *stack,
						   bool freestack, GinStatsData *buildStats);
static void ginFinishOldSplit(GinBtree btree, GinBtreeStack *stack,
							  GinStatsData *buildStats, int access);

/*
 * 按搜索所需的方式锁定缓冲区。
 */
int
ginTraverseLock(Buffer buffer, bool searchMode)
{
	Page		page;
	int			access = GIN_SHARE;

	LockBuffer(buffer, GIN_SHARE);
	page = BufferGetPage(buffer);
	if (GinPageIsLeaf(page))
	{
		if (searchMode == false)
		{
			/* 我们应当重新锁定本页 */
			LockBuffer(buffer, GIN_UNLOCK);
			LockBuffer(buffer, GIN_EXCLUSIVE);

			/* 但根页在重新锁定期间可能变成非叶子页 */
			if (!GinPageIsLeaf(page))
			{
				/* 恢复原来的锁类型（非常少见） */
				LockBuffer(buffer, GIN_UNLOCK);
				LockBuffer(buffer, GIN_SHARE);
			}
			else
				access = GIN_EXCLUSIVE;
		}
	}

	return access;
}

/*
 * 沿树下降到包含或将包含我们正在搜索的键的叶子页。
 * 键应当已经以树类型特定的方式填入 'btree'。
 * 若 btree->fullScan 为 true，则下降到
 * 最左叶子页。
 *
 * 若 'searchmode' 为 false，返回时 stack->buffer 被排他锁定，
 * 且该栈表示通向根的完整路径。否则 stack->buffer
 * 以共享模式锁定，且 stack->parent 为 NULL。
 *
 * 若 'rootConflictCheck' 为 true，则检查树根是否存在可串行化
 * 冲突。
 */
GinBtreeStack *
ginFindLeafPage(GinBtree btree, bool searchMode,
				bool rootConflictCheck, Snapshot snapshot)
{
	GinBtreeStack *stack;

	stack = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));
	stack->blkno = btree->rootBlkno;
	stack->buffer = ReadBuffer(btree->index, btree->rootBlkno);
	stack->parent = NULL;
	stack->predictNumber = 1;

	if (rootConflictCheck)
		CheckForSerializableConflictIn(btree->index, NULL, btree->rootBlkno);

	for (;;)
	{
		Page		page;
		BlockNumber child;
		int			access;

		stack->off = InvalidOffsetNumber;

		page = BufferGetPage(stack->buffer);
		TestForOldSnapshot(snapshot, btree->index, page);

		access = ginTraverseLock(stack->buffer, searchMode);

		/*
		 * 若我们要修改这棵树，则完成沿途遇到的任何
		 * 未完成分裂。
		 */
		if (!searchMode && GinPageIsIncompleteSplit(page))
			ginFinishOldSplit(btree, stack, NULL, access);

		/*
		 * 好，页已正确锁定，应当检查是否要向右走..，
		 * 根从来没有右链接，所以做个小优化
		 */
		while (btree->fullScan == false && stack->blkno != btree->rootBlkno &&
			   btree->isMoveRight(btree, page))
		{
			BlockNumber rightlink = GinPageGetOpaque(page)->rightlink;

			if (rightlink == InvalidBlockNumber)
				/* 最右页 */
				break;

			stack->buffer = ginStepRight(stack->buffer, btree->index, access);
			stack->blkno = rightlink;
			page = BufferGetPage(stack->buffer);
			TestForOldSnapshot(snapshot, btree->index, page);

			if (!searchMode && GinPageIsIncompleteSplit(page))
				ginFinishOldSplit(btree, stack, NULL, access);
		}

		if (GinPageIsLeaf(page))	/* 已找到，返回锁定的页 */
			return stack;

		/* 现在缓冲区正确了，尝试寻找子页 */
		child = btree->findChildPage(btree, stack);

		LockBuffer(stack->buffer, GIN_UNLOCK);
		Assert(child != InvalidBlockNumber);
		Assert(stack->blkno != child);

		if (searchMode)
		{
			/* 搜索模式下可以忘掉通向叶子的路径 */
			stack->blkno = child;
			stack->buffer = ReleaseAndReadBuffer(stack->buffer, btree->index, stack->blkno);
		}
		else
		{
			GinBtreeStack *ptr = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));

			ptr->parent = stack;
			stack = ptr;
			stack->blkno = child;
			stack->buffer = ReadBuffer(btree->index, stack->blkno);
			stack->predictNumber = 1;
		}
	}
}

/*
 * 从当前页向右走一步。
 *
 * 先锁定下一页，再释放当前页。这对防止
 * 并发 VACUUM 删除我们即将走到的页至关重要。
 * （为寻找插入位置而遍历树时，锁耦合
 * 并非严格必要，因为页删除会在根上取得清理锁，
 * 以阻止任何并发插入。见 README 的页删除一节。
 * 但始终这样做并无害处。）
 */
Buffer
ginStepRight(Buffer buffer, Relation index, int lockmode)
{
	Buffer		nextbuffer;
	Page		page = BufferGetPage(buffer);
	bool		isLeaf = GinPageIsLeaf(page);
	bool		isData = GinPageIsData(page);
	BlockNumber blkno = GinPageGetOpaque(page)->rightlink;

	nextbuffer = ReadBuffer(index, blkno);
	LockBuffer(nextbuffer, lockmode);
	UnlockReleaseBuffer(buffer);

	/* 健全性检查：我们走到的页种类应当相近。 */
	page = BufferGetPage(nextbuffer);
	if (isLeaf != GinPageIsLeaf(page) || isData != GinPageIsData(page))
		elog(ERROR, "right sibling of GIN page is of different type");

	return nextbuffer;
}

void
freeGinBtreeStack(GinBtreeStack *stack)
{
	while (stack)
	{
		GinBtreeStack *tmp = stack->parent;

		if (stack->buffer != InvalidBuffer)
			ReleaseBuffer(stack->buffer);

		pfree(stack);
		stack = tmp;
	}
}

/*
 * 尝试为当前栈位置寻找父页。在 stack->parent 中返回正确的父页
 * 以及子页偏移。绝不释放根页，以免与清理进程
 * 冲突。
 */
static void
ginFindParents(GinBtree btree, GinBtreeStack *stack)
{
	Page		page;
	Buffer		buffer;
	BlockNumber blkno,
				leftmostBlkno;
	OffsetNumber offset;
	GinBtreeStack *root;
	GinBtreeStack *ptr;

	/*
	 * 把栈一直回退到根，只留下根
	 * 项。
	 *
	 * 小心不要释放根页上的钉住！根页上的钉住
	 * 用来阻止对该树的并发清理。
	 */
	root = stack->parent;
	while (root->parent)
	{
		ReleaseBuffer(root->buffer);
		root = root->parent;
	}

	Assert(root->blkno == btree->rootBlkno);
	Assert(BufferGetBlockNumber(root->buffer) == btree->rootBlkno);
	root->off = InvalidOffsetNumber;

	blkno = root->blkno;
	buffer = root->buffer;

	ptr = (GinBtreeStack *) palloc(sizeof(GinBtreeStack));

	for (;;)
	{
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);
		if (GinPageIsLeaf(page))
			elog(ERROR, "Lost path");

		if (GinPageIsIncompleteSplit(page))
		{
			Assert(blkno != btree->rootBlkno);
			ptr->blkno = blkno;
			ptr->buffer = buffer;

			/*
			 * 父页可能不对，但若如此，ginFinishSplit 调用会
			 * 递归调用 ginFindParents 来修正它。
			 */
			ptr->parent = root;
			ptr->off = InvalidOffsetNumber;

			ginFinishOldSplit(btree, ptr, NULL, GIN_EXCLUSIVE);
		}

		leftmostBlkno = btree->getLeftMostChild(btree, page);

		while ((offset = btree->findChildPtr(btree, page, stack->blkno, InvalidOffsetNumber)) == InvalidOffsetNumber)
		{
			blkno = GinPageGetOpaque(page)->rightlink;
			if (blkno == InvalidBlockNumber)
			{
				/* 本层不存在该链接 */
				LockBuffer(buffer, GIN_UNLOCK);
				/* 不要释放根缓冲区上的钉住 */
				if (buffer != root->buffer)
					ReleaseBuffer(buffer);
				break;
			}
			buffer = ginStepRight(buffer, btree->index, GIN_EXCLUSIVE);
			page = BufferGetPage(buffer);

			/* 完成任何未完成分裂，同上 */
			if (GinPageIsIncompleteSplit(page))
			{
				Assert(blkno != btree->rootBlkno);
				ptr->blkno = blkno;
				ptr->buffer = buffer;
				ptr->parent = root;
				ptr->off = InvalidOffsetNumber;

				ginFinishOldSplit(btree, ptr, NULL, GIN_EXCLUSIVE);
			}
		}

		if (blkno != InvalidBlockNumber)
		{
			ptr->blkno = blkno;
			ptr->buffer = buffer;
			ptr->parent = root; /* 它可能不对，但下次调用时我们会
								 * 修正 */
			ptr->off = offset;
			stack->parent = ptr;
			return;
		}

		/* 下降到下一层 */
		blkno = leftmostBlkno;
		buffer = ReadBuffer(btree->index, blkno);
	}
}

/*
 * 向一页插入新项。
 *
 * 若插入已完成则返回 true。返回 false 时，页已分裂，
 * 需要更新父页。（根分裂返回 true，因为调用者无需
 * 再做任何动作即可完成。）
 *
 * 向内部页插入下行指针时，'childbuf' 含有被分裂的
 * 子页。其 GIN_INCOMPLETE_SPLIT 标志会与插入
 * 原子地清除。同时，目标页上偏移 stack->off 处的
 * 现有项会被更新为指向 updateblkno。
 *
 * 进入时 stack->buffer 已锁定，并保持锁定。
 * 若给出 childbuf，则同样如此。
 */
static bool
ginPlaceToPage(GinBtree btree, GinBtreeStack *stack,
			   void *insertdata, BlockNumber updateblkno,
			   Buffer childbuf, GinStatsData *buildStats)
{
	Page		page = BufferGetPage(stack->buffer);
	bool		result;
	GinPlaceToPageRC rc;
	uint16		xlflags = 0;
	Page		childpage = NULL;
	Page		newlpage = NULL,
				newrpage = NULL;
	void	   *ptp_workspace = NULL;
	MemoryContext tmpCxt;
	MemoryContext oldCxt;

	/*
	 * 本函数及其子函数的全部工作都在临时内存上下文中
	 * 完成。这避免泄漏并简化 APIs，因为有些子函数分配的
	 * 存储必须存活到我们完成 WAL 插入
	 * 为止。
	 */
	tmpCxt = AllocSetContextCreate(CurrentMemoryContext,
								   "ginPlaceToPage temporary context",
								   ALLOCSET_DEFAULT_SIZES);
	oldCxt = MemoryContextSwitchTo(tmpCxt);

	if (GinPageIsData(page))
		xlflags |= GIN_INSERT_ISDATA;
	if (GinPageIsLeaf(page))
	{
		xlflags |= GIN_INSERT_ISLEAF;
		Assert(!BufferIsValid(childbuf));
		Assert(updateblkno == InvalidBlockNumber);
	}
	else
	{
		Assert(BufferIsValid(childbuf));
		Assert(updateblkno != InvalidBlockNumber);
		childpage = BufferGetPage(childbuf);
	}

	/*
	 * 查看即将插入的元组是否放得进该页。beginPlaceToPage 会
	 * 决定该页是否需要分裂，若需要则计算分裂后的
	 * 内容。关于此处 API 的更多细节，见 beginPlaceToPage 与
	 * execPlaceToPage 函数的注释。
	 */
	rc = btree->beginPlaceToPage(btree, stack->buffer, stack,
								 insertdata, updateblkno,
								 &ptp_workspace,
								 &newlpage, &newrpage);

	if (rc == GPTP_NO_WORK)
	{
		/* 无事可做 */
		result = true;
	}
	else if (rc == GPTP_INSERT)
	{
		/* 放得下，执行插入 */
		START_CRIT_SECTION();

		if (RelationNeedsWAL(btree->index) && !btree->isBuild)
		{
			XLogBeginInsert();
			XLogRegisterBuffer(0, stack->buffer, REGBUF_STANDARD);
			if (BufferIsValid(childbuf))
				XLogRegisterBuffer(1, childbuf, REGBUF_STANDARD);
		}

		/* 执行页更新，并登记任何额外的 WAL 数据 */
		btree->execPlaceToPage(btree, stack->buffer, stack,
							   insertdata, updateblkno, ptp_workspace);

		MarkBufferDirty(stack->buffer);

		/* 向内部页的插入完成子页的分裂。 */
		if (BufferIsValid(childbuf))
		{
			GinPageGetOpaque(childpage)->flags &= ~GIN_INCOMPLETE_SPLIT;
			MarkBufferDirty(childbuf);
		}

		if (RelationNeedsWAL(btree->index) && !btree->isBuild)
		{
			XLogRecPtr	recptr;
			ginxlogInsert xlrec;
			BlockIdData childblknos[2];

			xlrec.flags = xlflags;

			XLogRegisterData((char *) &xlrec, sizeof(ginxlogInsert));

			/*
			 * 若这是一次下行指针的插入，则记录关于子页的
			 * 信息。
			 */
			if (BufferIsValid(childbuf))
			{
				BlockIdSet(&childblknos[0], BufferGetBlockNumber(childbuf));
				BlockIdSet(&childblknos[1], GinPageGetOpaque(childpage)->rightlink);
				XLogRegisterData((char *) childblknos,
								 sizeof(BlockIdData) * 2);
			}

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_INSERT);
			PageSetLSN(page, recptr);
			if (BufferIsValid(childbuf))
				PageSetLSN(childpage, recptr);
		}

		END_CRIT_SECTION();

		/* 插入已完成。 */
		result = true;
	}
	else if (rc == GPTP_SPLIT)
	{
		/*
		 * 放不下，需要分裂。分裂结果已算在 newlpage
		 * 与 newrpage 中，它们是指向 palloc 页的指针，并不关联
		 * 缓冲区。stack->buffer 尚未被改动。
		 */
		Buffer		rbuffer;
		BlockNumber savedRightLink;
		ginxlogSplit data;
		Buffer		lbuffer = InvalidBuffer;
		Page		newrootpg = NULL;

		/* 取得一张新索引页，作为右页 */
		rbuffer = GinNewBuffer(btree->index);

		/* 构建索引期间，计入这张新页 */
		if (buildStats)
		{
			if (btree->isData)
				buildStats->nDataPages++;
			else
				buildStats->nEntryPages++;
		}

		savedRightLink = GinPageGetOpaque(page)->rightlink;

		/* 开始设置 WAL 记录 */
		data.locator = btree->index->rd_locator;
		data.flags = xlflags;
		if (BufferIsValid(childbuf))
		{
			data.leftChildBlkno = BufferGetBlockNumber(childbuf);
			data.rightChildBlkno = GinPageGetOpaque(childpage)->rightlink;
		}
		else
			data.leftChildBlkno = data.rightChildBlkno = InvalidBlockNumber;

		if (stack->parent == NULL)
		{
			/*
			 * 正在分裂根，因此需要分配新的左页，并在根页上
			 * 放置指向左页和右页的指针。
			 */
			lbuffer = GinNewBuffer(btree->index);

			/* 构建索引期间，计入这张新的左页 */
			if (buildStats)
			{
				if (btree->isData)
					buildStats->nDataPages++;
				else
					buildStats->nEntryPages++;
			}

			data.rrlink = InvalidBlockNumber;
			data.flags |= GIN_SPLIT_ROOT;

			GinPageGetOpaque(newrpage)->rightlink = InvalidBlockNumber;
			GinPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

			/*
			 * 构造新根页，其中含有指向新左页和
			 * 新右页的下行指针。（在临时副本中做，而不是直接
			 * 覆盖原始页，因为我们尚未进入
			 * 临界区。）
			 */
			newrootpg = PageGetTempPage(newrpage);
			GinInitPage(newrootpg, GinPageGetOpaque(newlpage)->flags & ~(GIN_LEAF | GIN_COMPRESSED), BLCKSZ);

			btree->fillRoot(btree, newrootpg,
							BufferGetBlockNumber(lbuffer), newlpage,
							BufferGetBlockNumber(rbuffer), newrpage);

			if (GinPageIsLeaf(BufferGetPage(stack->buffer)))
			{

				PredicateLockPageSplit(btree->index,
									   BufferGetBlockNumber(stack->buffer),
									   BufferGetBlockNumber(lbuffer));

				PredicateLockPageSplit(btree->index,
									   BufferGetBlockNumber(stack->buffer),
									   BufferGetBlockNumber(rbuffer));
			}
		}
		else
		{
			/* 正在分裂非根页 */
			data.rrlink = savedRightLink;

			GinPageGetOpaque(newrpage)->rightlink = savedRightLink;
			GinPageGetOpaque(newlpage)->flags |= GIN_INCOMPLETE_SPLIT;
			GinPageGetOpaque(newlpage)->rightlink = BufferGetBlockNumber(rbuffer);

			if (GinPageIsLeaf(BufferGetPage(stack->buffer)))
			{

				PredicateLockPageSplit(btree->index,
									   BufferGetBlockNumber(stack->buffer),
									   BufferGetBlockNumber(rbuffer));
			}
		}

		/*
		 * 好，左页的新内容现在在临时副本
		 * （newlpage）中，新分配的右块的新内容
		 * 也同样如此。原始页仍未改变。
		 *
		 * 若这是根分裂，我们还有一张临时页，其中含有
		 * 根的新内容。
		 */

		START_CRIT_SECTION();

		MarkBufferDirty(rbuffer);
		MarkBufferDirty(stack->buffer);

		/*
		 * 把临时副本恢复到真正的缓冲区上。
		 */
		if (stack->parent == NULL)
		{
			/* 正在分裂根，有三页要更新 */
			MarkBufferDirty(lbuffer);
			memcpy(page, newrootpg, BLCKSZ);
			memcpy(BufferGetPage(lbuffer), newlpage, BLCKSZ);
			memcpy(BufferGetPage(rbuffer), newrpage, BLCKSZ);
		}
		else
		{
			/* 普通分裂，只有两页要更新 */
			memcpy(page, newlpage, BLCKSZ);
			memcpy(BufferGetPage(rbuffer), newrpage, BLCKSZ);
		}

		/* 若传入了 childbuf，也清除它的 INCOMPLETE_SPLIT 标志 */
		if (BufferIsValid(childbuf))
		{
			GinPageGetOpaque(childpage)->flags &= ~GIN_INCOMPLETE_SPLIT;
			MarkBufferDirty(childbuf);
		}

		/* 写入 WAL 记录 */
		if (RelationNeedsWAL(btree->index) && !btree->isBuild)
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();

			/*
			 * 我们只对所有分裂页取整页映像。分裂
			 * 并不常见，不值得把代码弄复杂来
			 * 提高效率。
			 */
			if (stack->parent == NULL)
			{
				XLogRegisterBuffer(0, lbuffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRegisterBuffer(1, rbuffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRegisterBuffer(2, stack->buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			}
			else
			{
				XLogRegisterBuffer(0, stack->buffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
				XLogRegisterBuffer(1, rbuffer, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			}
			if (BufferIsValid(childbuf))
				XLogRegisterBuffer(3, childbuf, REGBUF_STANDARD);

			XLogRegisterData((char *) &data, sizeof(ginxlogSplit));

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_SPLIT);

			PageSetLSN(page, recptr);
			PageSetLSN(BufferGetPage(rbuffer), recptr);
			if (stack->parent == NULL)
				PageSetLSN(BufferGetPage(lbuffer), recptr);
			if (BufferIsValid(childbuf))
				PageSetLSN(childpage, recptr);
		}
		END_CRIT_SECTION();

		/*
		 * 现在可以释放新页上的锁/钉住，但保持
		 * stack->buffer 锁定。childbuf 也不会被解锁。
		 */
		UnlockReleaseBuffer(rbuffer);
		if (stack->parent == NULL)
			UnlockReleaseBuffer(lbuffer);

		/*
		 * 若我们分裂了根，则已完成。否则，直到新页的下行指针
		 * 被插入父页之前，这次分裂都
		 * 尚未完成。
		 */
		result = (stack->parent == NULL);
	}
	else
	{
		elog(ERROR, "invalid return code from GIN beginPlaceToPage method: %d", rc);
		result = false;			/* 让编译器保持安静 */
	}

	/* 清理临时上下文 */
	MemoryContextSwitchTo(oldCxt);
	MemoryContextDelete(tmpCxt);

	return result;
}

/*
 * 通过把新页的下行指针插入父页来完成分裂。
 *
 * 进入时，stack->buffer 被排他锁定。
 *
 * 若 freestack 为 true，则在我们沿树向上走时，所有缓冲区
 * 都被释放并解锁，且 'stack' 被释放。否则 stack->buffer 保持
 * 锁定，stack 不被修改，只是可能向右走以寻找
 * 该页的正确父页。
 */
static void
ginFinishSplit(GinBtree btree, GinBtreeStack *stack, bool freestack,
			   GinStatsData *buildStats)
{
	Page		page;
	bool		done;
	bool		first = true;

	/* 此循环沿栈向上走，直到插入完成 */
	do
	{
		GinBtreeStack *parent = stack->parent;
		void	   *insertdata;
		BlockNumber updateblkno;

		/* 搜索父页以便锁定 */
		LockBuffer(parent->buffer, GIN_EXCLUSIVE);

		/*
		 * 若父页的分裂未完成，先完成那次分裂，
		 * 再继续当前这次。
		 *
		 * 注意：我们必须完成遇到的*所有*未完成分裂，即便
		 * 必须向右走。否则我们可能选中一张在父页中
		 * 没有下行指针的页作为目标，再对它分裂
		 * 就会失败。
		 */
		if (GinPageIsIncompleteSplit(BufferGetPage(parent->buffer)))
			ginFinishOldSplit(btree, parent, buildStats, GIN_EXCLUSIVE);

		/* 需要时向右走 */
		page = BufferGetPage(parent->buffer);
		while ((parent->off = btree->findChildPtr(btree, page, stack->blkno, parent->off)) == InvalidOffsetNumber)
		{
			if (GinPageRightMost(page))
			{
				/*
				 * 已是最右页，但找不到父页，应当使用
				 * 普通搜索...
				 */
				LockBuffer(parent->buffer, GIN_UNLOCK);
				ginFindParents(btree, stack);
				parent = stack->parent;
				Assert(parent != NULL);
				break;
			}

			parent->buffer = ginStepRight(parent->buffer, btree->index, GIN_EXCLUSIVE);
			parent->blkno = BufferGetBlockNumber(parent->buffer);
			page = BufferGetPage(parent->buffer);

			if (GinPageIsIncompleteSplit(BufferGetPage(parent->buffer)))
				ginFinishOldSplit(btree, parent, buildStats, GIN_EXCLUSIVE);
		}

		/* 插入下行指针 */
		insertdata = btree->prepareDownlink(btree, stack->buffer);
		updateblkno = GinPageGetOpaque(BufferGetPage(stack->buffer))->rightlink;
		done = ginPlaceToPage(btree, parent,
							  insertdata, updateblkno,
							  stack->buffer, buildStats);
		pfree(insertdata);

		/*
		 * 若调用者请求释放栈，现在就解锁并释放
		 * 子缓冲区。否则保持它被钉住并锁定，但若必须
		 * 沿树向上递归，可以解锁上层各页，只
		 * 保持栈底那一页锁定。
		 */
		if (!first || freestack)
			LockBuffer(stack->buffer, GIN_UNLOCK);
		if (freestack)
		{
			ReleaseBuffer(stack->buffer);
			pfree(stack);
		}
		stack = parent;

		first = false;
	} while (!done);

	/* 解锁父页 */
	LockBuffer(stack->buffer, GIN_UNLOCK);

	if (freestack)
		freeGinBtreeStack(stack);
}

/*
 * ginFinishSplit() 的一个入口，用于我们在树中偶然遇到
 * 一张已存在的未完成分裂页时，而不是完成我们刚刚
 * 自己做出的分裂。差别在于进入时 stack->buffer 可能
 * 仅仅以共享模式锁定，并将被升级为排他模式。
 *
 * 注意：升级锁会短暂释放它。在扫描中这样做
 * 是不行的，因为并发 VACUUM 可能在我们不持有锁时
 * 删除该页。不过在插入中这样做可以，因为 VACUUM
 * 有另一种机制防止它与插入并发运行。
 * （也就是说，它在根上持有清理锁。）
 */
static void
ginFinishOldSplit(GinBtree btree, GinBtreeStack *stack, GinStatsData *buildStats, int access)
{
	elog(DEBUG1, "finishing incomplete split of block %u in gin index \"%s\"",
		 stack->blkno, RelationGetRelationName(btree->index));

	if (access == GIN_SHARE)
	{
		LockBuffer(stack->buffer, GIN_UNLOCK);
		LockBuffer(stack->buffer, GIN_EXCLUSIVE);

		if (!GinPageIsIncompleteSplit(BufferGetPage(stack->buffer)))
		{
			/*
			 * 在我们不持有锁期间，别人已经完成了
			 * 这次分裂。
			 */
			return;
		}
	}

	ginFinishSplit(btree, stack, false, buildStats);
}

/*
 * 向 stack 所描述的树插入一个值。
 *
 * 待插入的值由 'insertdata' 给出。其格式取决于
 * 这是条目树还是倒排树，ginInsertValue 只是把它
 * 传给树类型特定的回调函数。
 *
 * 构建索引期间，buildStats 非空，其中的计数器
 * 会按需要递增。
 *
 * 注意：传入的栈会被释放，如同调用了 freeGinBtreeStack。
 */
void
ginInsertValue(GinBtree btree, GinBtreeStack *stack, void *insertdata,
			   GinStatsData *buildStats)
{
	bool		done;

	/* 若叶子页的分裂未完成，先完成这次分裂 */
	if (GinPageIsIncompleteSplit(BufferGetPage(stack->buffer)))
		ginFinishOldSplit(btree, stack, buildStats, GIN_EXCLUSIVE);

	done = ginPlaceToPage(btree, stack,
						  insertdata, InvalidBlockNumber,
						  InvalidBuffer, buildStats);
	if (done)
	{
		LockBuffer(stack->buffer, GIN_UNLOCK);
		freeGinBtreeStack(stack);
	}
	else
		ginFinishSplit(btree, stack, true, buildStats);
}
