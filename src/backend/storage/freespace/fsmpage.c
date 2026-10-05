/*-------------------------------------------------------------------------
 *
 * fsmpage.c
 *	  搜索并操作单个 FSM 页的例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/fsmpage.c
 *
 * 说明：
 *
 *	本文件中的公开函数构成一套 API，它隐藏 FSM 页的内部
 *	结构。这样 freespace.c 就可以把每个 FSM 页
 *	当成有 SlotsPerPage 个 "槽" 的黑盒。fsm_set_avail() 与
 *	fsm_get_avail() 用来读取或设置某个槽的值，
 *	fsm_search_avail() 用来搜索值 >= X 的槽。
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
	 * 从目标槽开始搜索。每一步先向右移动一个结点，
	 * 再向上爬到父结点。遇到空闲空间足够的结点
	 * 就停止。根结点的空闲空间一定足够，所以
	 * 搜索总会停下来。
	 *
	 * 思路是逐步扩大 "搜索三角"，
	 * 也就是当前结点所覆盖的全部结点，
	 * 并保证从起点向右搜索。第一步只检查目标槽。
	 * 从左子结点上移到其父结点时，
	 * 是把该父结点的右子树加入搜索三角。
	 * 从右子结点先向右再向上时，
	 * 是丢掉当前搜索三角
	 * （已知其中没有合适的页），
	 * 改为查看其右侧下一个更大的三角。
	 * 因此从不向原来的起点左侧看，而且每一步
	 * 搜索三角的大小都加倍，搜索 N 页只需 log2(N)。
	 *
	 * "向右移动" 若碰到树的右边缘就会回绕，
	 * 所以即使从靠近右侧的地方开始，行为仍然正确。
	 * 另外，这种先移动再上爬的做法也保证
	 * 我们不会停在叶层右侧那些缺少的结点上。
	 *
	 * 例如，考虑下面这棵树：
	 *
	 *		   7
	 *	   7	   6
	 *	 5	 7	 6	 5
	 *	4 5 5 7 2 6 5 2
	 *				T
	 *
	 * 设目标结点就是字母 T 所标出的那个结点，
	 * 要找的是值大于等于 6 的结点。搜索从 T 开始。
	 * 第一次迭代先向右，再到父结点，
	 * 到达最右边的 5。第二次迭代向右并回绕，
	 * 然后向上爬，到达第三层上的 7。
	 * 7 满足条件，于是向下走到最底层，
	 * 沿着一串 7 走。这实际上就是起点右侧
	 * （允许回绕）第一个合适的页。
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
