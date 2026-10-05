/*-------------------------------------------------------------------------
 *
 * fsmpage.c
 *	  routines to search and manipulate one FSM page.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/fsmpage.c
 *
 * NOTES:
 *
 *	The public functions in this file form an API that hides the internal
 *	structure of a FSM page. This allows freespace.c to treat each FSM page
 *	as a black box with SlotsPerPage "slots". fsm_set_avail() and
 *	fsm_get_avail() let you get/set the value of a slot, and
 *	fsm_search_avail() lets you search for a slot with value >= X.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/fsm_internals.h"

/* 在一页内部遍历树的宏。根的下标为零。 */
#define leftchild(x)	(2 * (x) + 1)
#define rightchild(x)	(2 * (x) + 2)
#define parentof(x)		(((x) - 1) / 2)

/*
 * 找 x 的右邻，在本层内回绕。
 */
static int
rightneighbor(int x)
{
	/*
	 * 向右移动。这可能会回绕，走到下一层
	 * 最左边的结点。
	 */
	x++;

	/*
	 * 检查是否走到了下一层最左边的结点，若是则纠正。
	 * 每一层最左边的结点编号为 x = 2^level - 1，因此要检查
	 * (x + 1) 是否为 2 的幂，用的是二进制补码算术里的
	 * 标准技巧。
	 */
	if (((x + 1) & x) == 0)
		x = parentof(x);

	return x;
}

/*
 * 设置页上某个槽的值。若该页被修改则返回真。
 *
 * 调用方必须对该页持有排他锁。
 */
bool
fsm_set_avail(Page page, int slot, uint8 value)
{
	int			nodeno = NonLeafNodesPerPage + slot;
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	uint8		oldvalue;

	Assert(slot < LeafNodesPerPage);

	oldvalue = fsmpage->fp_nodes[nodeno];

	/* 若值没有变化，就不必做任何事 */
	if (oldvalue == value && value <= fsmpage->fp_nodes[0])
		return false;

	fsmpage->fp_nodes[nodeno] = value;

	/*
	 * 向上传播，直到碰到根，或碰到一个不需要
	 * 更新的结点。
	 */
	do
	{
		uint8		newvalue = 0;
		int			lchild;
		int			rchild;

		nodeno = parentof(nodeno);
		lchild = leftchild(nodeno);
		rchild = lchild + 1;

		newvalue = fsmpage->fp_nodes[lchild];
		if (rchild < NodesPerPage)
			newvalue = Max(newvalue,
						   fsmpage->fp_nodes[rchild]);

		oldvalue = fsmpage->fp_nodes[nodeno];
		if (oldvalue == newvalue)
			break;

		fsmpage->fp_nodes[nodeno] = newvalue;
	} while (nodeno > 0);

	/*
	 * 健全性检查：若新值（仍然）高于顶端的值，
	 * 这棵树已损坏。若如此，则重建。
	 */
	if (value > fsmpage->fp_nodes[0])
		fsm_rebuild_page(page);

	return true;
}

/*
 * 返回页上给定槽的值。
 *
 * 这只是对单个字节的只读访问，因此该页不
 * 需要上锁。
 */
uint8
fsm_get_avail(Page page, int slot)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);

	Assert(slot < LeafNodesPerPage);

	return fsmpage->fp_nodes[NonLeafNodesPerPage + slot];
}

/*
 * 返回一页根处的值。
 *
 * 这只是对单个字节的只读访问，因此该页不
 * 需要上锁。
 */
uint8
fsm_get_max_avail(Page page)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);

	return fsmpage->fp_nodes[0];
}

/*
 * 搜索类别不低于 minvalue 的槽。
 * 返回槽号；若没有找到则返回 -1。
 *
 * 调用方必须至少持有该页的共享锁。若需要更新该页，
 * 本函数可以先解锁，再以排他模式重新锁上。
 * 若调用方已经持有排他锁，应把 exclusive_lock_held
 * 设为真，以免做额外的工作。
 *
 * 若 advancenext 为假，则把 fp_next_slot 指向返回的槽；
 * 若为真，则指向所返回槽之后的那个槽。
 */
int
fsm_search_avail(Buffer buf, uint8 minvalue, bool advancenext,
				 bool exclusive_lock_held)
{
	Page		page = BufferGetPage(buf);
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	int			nodeno;
	int			target;
	uint16		slot;

restart:

	/*
	 * 先检查根。若没有空闲空间足够的叶结点，
	 * 就尽快退出。
	 */
	if (fsmpage->fp_nodes[0] < minvalue)
		return -1;

	/*
	 * 用 fp_next_slot 开始搜索。它只是提示，所以要检查
	 * 它是否合理。（这也处理上一次调用返回了本页
	 * 最后一个槽时的回绕。）
	 */
	target = fsmpage->fp_next_slot;
	if (target < 0 || target >= LeafNodesPerPage)
		target = 0;
	target += NonLeafNodesPerPage;

	/*----------
	 * Start the search from the target slot.  At every step, move one
	 * node to the right, then climb up to the parent.  Stop when we reach
	 * a node with enough free space (as we must, since the root has enough
	 * space).
	 *
	 * The idea is to gradually expand our "search triangle", that is, all
	 * nodes covered by the current node, and to be sure we search to the
	 * right from the start point.  At the first step, only the target slot
	 * is examined.  When we move up from a left child to its parent, we are
	 * adding the right-hand subtree of that parent to the search triangle.
	 * When we move right then up from a right child, we are dropping the
	 * current search triangle (which we know doesn't contain any suitable
	 * page) and instead looking at the next-larger-size triangle to its
	 * right.  So we never look left from our original start point, and at
	 * each step the size of the search triangle doubles, ensuring it takes
	 * only log2(N) work to search N pages.
	 *
	 * The "move right" operation will wrap around if it hits the right edge
	 * of the tree, so the behavior is still good if we start near the right.
	 * Note also that the move-and-climb behavior ensures that we can't end
	 * up on one of the missing nodes at the right of the leaf level.
	 *
	 * For example, consider this tree:
	 *
	 *		   7
	 *	   7	   6
	 *	 5	 7	 6	 5
	 *	4 5 5 7 2 6 5 2
	 *				T
	 *
	 * Assume that the target node is the node indicated by the letter T,
	 * and we're searching for a node with value of 6 or higher. The search
	 * begins at T. At the first iteration, we move to the right, then to the
	 * parent, arriving at the rightmost 5. At the second iteration, we move
	 * to the right, wrapping around, then climb up, arriving at the 7 on the
	 * third level.  7 satisfies our search, so we descend down to the bottom,
	 * following the path of sevens.  This is in fact the first suitable page
	 * to the right of (allowing for wraparound) our start point.
	 *----------
	 */
	nodeno = target;
	while (nodeno > 0)
	{
		if (fsmpage->fp_nodes[nodeno] >= minvalue)
			break;

		/*
		 * 向右移动，必要时在同一层回绕，然后
		 * 向上爬。
		 */
		nodeno = parentof(rightneighbor(nodeno));
	}

	/*
	 * 现在位于树的中部、空闲空间足够的结点。
	 * 向下走到最底层，沿着空闲空间足够的路径，
	 * 有选择时优先向左。
	 */
	while (nodeno < NonLeafNodesPerPage)
	{
		int			childnodeno = leftchild(nodeno);

		if (childnodeno < NodesPerPage &&
			fsmpage->fp_nodes[childnodeno] >= minvalue)
		{
			nodeno = childnodeno;
			continue;
		}
		childnodeno++;			/* 指向右子结点 */
		if (childnodeno < NodesPerPage &&
			fsmpage->fp_nodes[childnodeno] >= minvalue)
		{
			nodeno = childnodeno;
		}
		else
		{
			/*
			 * 糟了。父结点曾保证左子或右子有足够空间，
			 * 实际上却都没有。若先前把该页写到磁盘时崩溃，
			 * 且只有一部分页真正落到了磁盘上，就会出现
			 * 撕裂页。
			 *
			 * 修正损坏并重新开始。
			 */
			RelFileLocator rlocator;
			ForkNumber	forknum;
			BlockNumber blknum;

			BufferGetTag(buf, &rlocator, &forknum, &blknum);
			elog(DEBUG1, "fixing corrupt FSM block %u, relation %u/%u/%u",
				 blknum, rlocator.spcOid, rlocator.dbOid, rlocator.relNumber);

			/* 确保我们持有排他锁 */
			if (!exclusive_lock_held)
			{
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				exclusive_lock_held = true;
			}
			fsm_rebuild_page(page);
			MarkBufferDirtyHint(buf, false);
			goto restart;
		}
	}

	/* 现在位于最底层、空闲空间足够的结点。 */
	slot = nodeno - NonLeafNodesPerPage;

	/*
	 * 更新下一目标指针。即便只持有共享锁也这么做。
	 * 理由是：使用共享锁、偶尔得到一个乱掉的下一指针，
	 * 也好过承受排他锁的并发代价。
	 * 因此这里不把锁升级成排他锁。
	 *
	 * 回绕在本函数开头处理。
	 */
	fsmpage->fp_next_slot = slot + (advancenext ? 1 : 0);

	return slot;
}

/*
 * 把编号 >= nslots 的所有槽的可用空间设为零。
 * 若该页被修改则返回真。
 */
bool
fsm_truncate_avail(Page page, int nslots)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	uint8	   *ptr;
	bool		changed = false;

	Assert(nslots >= 0 && nslots < LeafNodesPerPage);

	/* 清掉所有被截断的叶结点 */
	ptr = &fsmpage->fp_nodes[NonLeafNodesPerPage + nslots];
	for (; ptr < &fsmpage->fp_nodes[NodesPerPage]; ptr++)
	{
		if (*ptr != 0)
			changed = true;
		*ptr = 0;
	}

	/* 修正上层结点。 */
	if (changed)
		fsm_rebuild_page(page);

	return changed;
}

/*
 * 重建一页的上层。若该页被修改
 * 则返回真。
 */
bool
fsm_rebuild_page(Page page)
{
	FSMPage		fsmpage = (FSMPage) PageGetContents(page);
	bool		changed = false;
	int			nodeno;

	/*
	 * 从最低的非叶层、最后一个结点开始倒着走，
	 * 经过各层全部非叶结点，一直走到根。
	 */
	for (nodeno = NonLeafNodesPerPage - 1; nodeno >= 0; nodeno--)
	{
		int			lchild = leftchild(nodeno);
		int			rchild = lchild + 1;
		uint8		newvalue = 0;

		/* 最先检查的几个结点可能有零个或一个子结点。 */
		if (lchild < NodesPerPage)
			newvalue = fsmpage->fp_nodes[lchild];

		if (rchild < NodesPerPage)
			newvalue = Max(newvalue,
						   fsmpage->fp_nodes[rchild]);

		if (fsmpage->fp_nodes[nodeno] != newvalue)
		{
			fsmpage->fp_nodes[nodeno] = newvalue;
			changed = true;
		}
	}

	return changed;
}
