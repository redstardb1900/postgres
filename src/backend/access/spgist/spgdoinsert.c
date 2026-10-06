/*-------------------------------------------------------------------------
 *
 * spgdoinsert.c
 *	  插入算法的实现
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgdoinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/spgist_private.h"
#include "access/spgxlog.h"
#include "access/xloginsert.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


/*
 * SPPageDesc 跟踪我们正在插入的页的全部信息。在某些
 * 情形下它实际标识一个元组，甚至是内部元组中的某个
 * 特定节点。但任一字段都可以无效。若 buffer 字段有效，
 * 则表示我们持有该缓冲区的钉住与排他锁。page 指针应恰好
 * 在 buffer 有效时有效。
 */
typedef struct SPPageDesc
{
	BlockNumber blkno;			/* 块号，或 InvalidBlockNumber */
	Buffer		buffer;			/* 页的缓冲区号，或 InvalidBuffer */
	Page		page;			/* 指向页缓冲区的指针，或 NULL */
	OffsetNumber offnum;		/* 元组的偏移，或 InvalidOffsetNumber */
	int			node;			/* 内部元组内的节点号，或 -1 */
} SPPageDesc;


/*
 * 设置内部元组 tup 中第 nodeN 个条目的项指针。这用于在移动
 * 或分裂操作之后更新父内部元组的
 * 下行链接。
 */
void
spgUpdateNodeLink(SpGistInnerTuple tup, int nodeN,
				  BlockNumber blkno, OffsetNumber offset)
{
	int			i;
	SpGistNodeTuple node;

	SGITITERATE(tup, i, node)
	{
		if (i == nodeN)
		{
			ItemPointerSet(&node->t_tid, blkno, offset);
			return;
		}
	}

	elog(ERROR, "failed to find requested node %d in SPGiST inner tuple",
		 nodeN);
}

/*
 * 构造比给定元组多一个节点的新内部元组，带指定的标签
 * datum，插入到节点数组中偏移 "offset" 处。新元组的前缀
 * 与旧元组相同。
 *
 * 注意新节点最初有无效的下行链接。我们稍后会找到一页
 * 让它指向。
 */
static SpGistInnerTuple
addNode(SpGistState *state, SpGistInnerTuple tuple, Datum label, int offset)
{
	SpGistNodeTuple node,
			   *nodes;
	int			i;

	/* 若 offset 为负，则插在末尾 */
	if (offset < 0)
		offset = tuple->nNodes;
	else if (offset > tuple->nNodes)
		elog(ERROR, "invalid offset for adding node to SPGiST inner tuple");

	nodes = palloc(sizeof(SpGistNodeTuple) * (tuple->nNodes + 1));
	SGITITERATE(tuple, i, node)
	{
		if (i < offset)
			nodes[i] = node;
		else
			nodes[i + 1] = node;
	}

	nodes[offset] = spgFormNodeTuple(state, label, false);

	return spgFormInnerTuple(state,
							 (tuple->prefixSize > 0),
							 SGITDATUM(tuple, state),
							 tuple->nNodes + 1,
							 nodes);
}

/* 用于对 OffsetNumbers 排序的 qsort 比较器 */
static int
cmpOffsetNumbers(const void *a, const void *b)
{
	if (*(const OffsetNumber *) a == *(const OffsetNumber *) b)
		return 0;
	return (*(const OffsetNumber *) a > *(const OffsetNumber *) b) ? 1 : -1;
}

/*
 * 从索引页删除多个元组，同时保留元组偏移号。
 *
 * 给定列表中的第一个元组替换为类型为 "firststate"
 * （REDIRECT/DEAD/PLACEHOLDER）的死元组；其余元组替换为
 * 类型为 "reststate" 的死元组。若 firststate 或 reststate
 * 为 REDIRECT，则 blkno/offnum 指定链接目标。
 *
 * 注意：此函数用于 WAL 重放，因此不要做得过于聪明。特别是，
 * 除调用 spgFormDeadTuple() 外不应使用 "state"。它也在
 * 临界区内使用，因此也不能
 * palloc！
 */
void
spgPageIndexMultiDelete(SpGistState *state, Page page,
						OffsetNumber *itemnos, int nitems,
						int firststate, int reststate,
						BlockNumber blkno, OffsetNumber offnum)
{
	OffsetNumber firstItem;
	OffsetNumber sortednos[MaxIndexTuplesPerPage];
	SpGistDeadTuple tuple = NULL;
	int			i;

	if (nitems == 0)
		return;					/* 无事可做 */

	/*
	 * 为效率起见我们想用 PageIndexMultiDelete，它需要目标按
	 * 已排序顺序列出，因此必须对 itemnos 数组排序。（这也大大
	 * 简化了重新插入替换元组时的计算。）但我们不能涂改调用方
	 * 的数组，因此必须做一份
	 * 拷贝。
	 */
	memcpy(sortednos, itemnos, sizeof(OffsetNumber) * nitems);
	if (nitems > 1)
		qsort(sortednos, nitems, sizeof(OffsetNumber), cmpOffsetNumbers);

	PageIndexMultiDelete(page, sortednos, nitems);

	firstItem = itemnos[0];

	for (i = 0; i < nitems; i++)
	{
		OffsetNumber itemno = sortednos[i];
		int			tupstate;

		tupstate = (itemno == firstItem) ? firststate : reststate;
		if (tuple == NULL || tuple->tupstate != tupstate)
			tuple = spgFormDeadTuple(state, tupstate, blkno, offnum);

		if (PageAddItem(page, (Item) tuple, tuple->size,
						itemno, false, false) != itemno)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 tuple->size);

		if (tupstate == SPGIST_REDIRECT)
			SpGistPageGetOpaque(page)->nRedirection++;
		else if (tupstate == SPGIST_PLACEHOLDER)
			SpGistPageGetOpaque(page)->nPlaceholder++;
	}
}

/*
 * 更新父内部元组的下行链接，并标记父缓冲区为脏（这必须是
 * 当前 WAL 动作中对父页的最后一次
 * 更改）。
 */
static void
saveNodeLink(Relation index, SPPageDesc *parent,
			 BlockNumber blkno, OffsetNumber offnum)
{
	SpGistInnerTuple innerTuple;

	innerTuple = (SpGistInnerTuple) PageGetItem(parent->page,
												PageGetItemId(parent->page, parent->offnum));

	spgUpdateNodeLink(innerTuple, parent->node, blkno, offnum);

	MarkBufferDirty(parent->buffer);
}

/*
 * 向已知有足够空间的叶子页添加一个叶子元组
 */
static void
addLeafTuple(Relation index, SpGistState *state, SpGistLeafTuple leafTuple,
			 SPPageDesc *current, SPPageDesc *parent, bool isNulls, bool isNew)
{
	spgxlogAddLeaf xlrec;

	xlrec.newPage = isNew;
	xlrec.storesNulls = isNulls;

	/* 下面按需要填充 */
	xlrec.offnumLeaf = InvalidOffsetNumber;
	xlrec.offnumHeadLeaf = InvalidOffsetNumber;
	xlrec.offnumParent = InvalidOffsetNumber;
	xlrec.nodeI = 0;

	START_CRIT_SECTION();

	if (current->offnum == InvalidOffsetNumber ||
		SpGistBlockIsRoot(current->blkno))
	{
		/* 元组不属于某条链 */
		SGLT_SET_NEXTOFFSET(leafTuple, InvalidOffsetNumber);
		current->offnum = SpGistPageAddNewItem(state, current->page,
											   (Item) leafTuple, leafTuple->size,
											   NULL, false);

		xlrec.offnumLeaf = current->offnum;

		/* 若有父，必须更新其下行链接 */
		if (parent->buffer != InvalidBuffer)
		{
			xlrec.offnumParent = parent->offnum;
			xlrec.nodeI = parent->node;

			saveNodeLink(index, parent, current->blkno, current->offnum);
		}
	}
	else
	{
		/*
		 * 元组必须插入到已有链中。我们不能改变链头地址，但也不必
		 * 追整条链把它放在末尾；可以插在
		 * 第二位。
		 *
		 * 另外，"链" 可能只由一个 DEAD 元组组成，此时应原地替换
		 * 该 DEAD 元组。
		 */
		SpGistLeafTuple head;
		OffsetNumber offnum;

		head = (SpGistLeafTuple) PageGetItem(current->page,
											 PageGetItemId(current->page, current->offnum));
		if (head->tupstate == SPGIST_LIVE)
		{
			SGLT_SET_NEXTOFFSET(leafTuple, SGLT_GET_NEXTOFFSET(head));
			offnum = SpGistPageAddNewItem(state, current->page,
										  (Item) leafTuple, leafTuple->size,
										  NULL, false);

			/*
			 * 重新获取链表头，因为它可能已在页上移动，
			 * 并设置新的第二元素
			 */
			head = (SpGistLeafTuple) PageGetItem(current->page,
												 PageGetItemId(current->page, current->offnum));
			SGLT_SET_NEXTOFFSET(head, offnum);

			xlrec.offnumLeaf = offnum;
			xlrec.offnumHeadLeaf = current->offnum;
		}
		else if (head->tupstate == SPGIST_DEAD)
		{
			SGLT_SET_NEXTOFFSET(leafTuple, InvalidOffsetNumber);
			PageIndexTupleDelete(current->page, current->offnum);
			if (PageAddItem(current->page,
							(Item) leafTuple, leafTuple->size,
							current->offnum, false, false) != current->offnum)
				elog(ERROR, "failed to add item of size %u to SPGiST index page",
					 leafTuple->size);

			/* WAL 重放用相等的偏移号区分此情形 */
			xlrec.offnumLeaf = current->offnum;
			xlrec.offnumHeadLeaf = current->offnum;
		}
		else
			elog(ERROR, "unexpected SPGiST tuple state: %d", head->tupstate);
	}

	MarkBufferDirty(current->buffer);

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;
		int			flags;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		XLogRegisterData((char *) leafTuple, leafTuple->size);

		flags = REGBUF_STANDARD;
		if (xlrec.newPage)
			flags |= REGBUF_WILL_INIT;
		XLogRegisterBuffer(0, current->buffer, flags);
		if (xlrec.offnumParent != InvalidOffsetNumber)
			XLogRegisterBuffer(1, parent->buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_ADD_LEAF);

		PageSetLSN(current->page, recptr);

		/* 仅当我们确实更改了父时才更新它 */
		if (xlrec.offnumParent != InvalidOffsetNumber)
		{
			PageSetLSN(parent->page, recptr);
		}
	}

	END_CRIT_SECTION();
}

/*
 * 统计从 current->offnum 开始的链中叶子元组的数量与总大小。
 * 数量写入 *nToSplit，总大小作为函数
 * 返回值。
 *
 * 考虑根页时的拙劣特例（即根是叶子页，但我们即将首次分裂）：
 * 返回虚假的大值，以迫使 spgdoinsert() 走 doPickSplit 而非
 * moveLeafs 代码路径。moveLeafs 未准备好处理
 * 根页。
 */
static int
checkSplitConditions(Relation index, SpGistState *state,
					 SPPageDesc *current, int *nToSplit)
{
	int			i,
				n = 0,
				totalSize = 0;

	if (SpGistBlockIsRoot(current->blkno))
	{
		/* 返回不可能的值以强制分裂 */
		*nToSplit = BLCKSZ;
		return BLCKSZ;
	}

	i = current->offnum;
	while (i != InvalidOffsetNumber)
	{
		SpGistLeafTuple it;

		Assert(i >= FirstOffsetNumber &&
			   i <= PageGetMaxOffsetNumber(current->page));
		it = (SpGistLeafTuple) PageGetItem(current->page,
										   PageGetItemId(current->page, i));
		if (it->tupstate == SPGIST_LIVE)
		{
			n++;
			totalSize += it->size + sizeof(ItemIdData);
		}
		else if (it->tupstate == SPGIST_DEAD)
		{
			/* 可能把 DEAD 元组看作链的首项/唯一项 */
			Assert(i == current->offnum);
			Assert(SGLT_GET_NEXTOFFSET(it) == InvalidOffsetNumber);
			/* 不计入结果，因为它不会去到另一页 */
		}
		else
			elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);

		i = SGLT_GET_NEXTOFFSET(it);
	}

	*nToSplit = n;

	return totalSize;
}

/*
 * current 指向我们本想把 newLeafTuple 加入的叶子元组链，但因
 * 页上空间不够而必须移动该链。当链中数据很少、分裂会低效时
 * 使用此方法。我们确信能把该链加上 newLeafTuple 放进
 * 另一
 * 页。
 */
static void
moveLeafs(Relation index, SpGistState *state,
		  SPPageDesc *current, SPPageDesc *parent,
		  SpGistLeafTuple newLeafTuple, bool isNulls)
{
	int			i,
				nDelete,
				nInsert,
				size;
	Buffer		nbuf;
	Page		npage;
	OffsetNumber r = InvalidOffsetNumber,
				startOffset = InvalidOffsetNumber;
	bool		replaceDead = false;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	BlockNumber nblkno;
	spgxlogMoveLeafs xlrec;
	char	   *leafdata,
			   *leafptr;

	/* 这在根页上行不通 */
	Assert(parent->buffer != InvalidBuffer);
	Assert(parent->buffer != current->buffer);

	/* 定位要移动的元组，并累计所需空间 */
	i = PageGetMaxOffsetNumber(current->page);
	toDelete = (OffsetNumber *) palloc(sizeof(OffsetNumber) * i);
	toInsert = (OffsetNumber *) palloc(sizeof(OffsetNumber) * (i + 1));

	size = newLeafTuple->size + sizeof(ItemIdData);

	nDelete = 0;
	i = current->offnum;
	while (i != InvalidOffsetNumber)
	{
		SpGistLeafTuple it;

		Assert(i >= FirstOffsetNumber &&
			   i <= PageGetMaxOffsetNumber(current->page));
		it = (SpGistLeafTuple) PageGetItem(current->page,
										   PageGetItemId(current->page, i));

		if (it->tupstate == SPGIST_LIVE)
		{
			toDelete[nDelete] = i;
			size += it->size + sizeof(ItemIdData);
			nDelete++;
		}
		else if (it->tupstate == SPGIST_DEAD)
		{
			/* 可能把 DEAD 元组看作链的首项/唯一项 */
			Assert(i == current->offnum);
			Assert(SGLT_GET_NEXTOFFSET(it) == InvalidOffsetNumber);
			/* 我们不想移动它，因此不计入大小 */
			toDelete[nDelete] = i;
			nDelete++;
			replaceDead = true;
		}
		else
			elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);

		i = SGLT_GET_NEXTOFFSET(it);
	}

	/* 找一个能容纳它们的叶子页 */
	nbuf = SpGistGetBuffer(index, GBUF_LEAF | (isNulls ? GBUF_NULLS : 0),
						   size, &xlrec.newPage);
	npage = BufferGetPage(nbuf);
	nblkno = BufferGetBlockNumber(nbuf);
	Assert(nblkno != current->blkno);

	leafdata = leafptr = palloc(size);

	START_CRIT_SECTION();

	/* 把所有旧元组拷到新页，死元组除外 */
	nInsert = 0;
	if (!replaceDead)
	{
		for (i = 0; i < nDelete; i++)
		{
			SpGistLeafTuple it;

			it = (SpGistLeafTuple) PageGetItem(current->page,
											   PageGetItemId(current->page, toDelete[i]));
			Assert(it->tupstate == SPGIST_LIVE);

			/*
			 * 更新链链接（注意链顺序会颠倒，但我们不在乎）。我们在此
			 * 修改源页上的元组，但没关系，因为马上就要
			 * 删除它。
			 */
			SGLT_SET_NEXTOFFSET(it, r);

			r = SpGistPageAddNewItem(state, npage, (Item) it, it->size,
									 &startOffset, false);

			toInsert[nInsert] = r;
			nInsert++;

			/* 也将修改后的元组保存到 leafdata */
			memcpy(leafptr, it, it->size);
			leafptr += it->size;
		}
	}

	/* 同样加入新元组 */
	SGLT_SET_NEXTOFFSET(newLeafTuple, r);
	r = SpGistPageAddNewItem(state, npage,
							 (Item) newLeafTuple, newLeafTuple->size,
							 &startOffset, false);
	toInsert[nInsert] = r;
	nInsert++;
	memcpy(leafptr, newLeafTuple, newLeafTuple->size);
	leafptr += newLeafTuple->size;

	/*
	 * 现在删除旧元组，并在第一个位置留下重定向指针，除非我们
	 * 正在做索引构建；构建时不可能有并发扫描，因此不必提供
	 * 重定向。
	 */
	spgPageIndexMultiDelete(state, current->page, toDelete, nDelete,
							state->isBuild ? SPGIST_PLACEHOLDER : SPGIST_REDIRECT,
							SPGIST_PLACEHOLDER,
							nblkno, r);

	/* 更新父的下行链接并标记父页为脏 */
	saveNodeLink(index, parent, nblkno, r);

	/* 也标记叶子页 */
	MarkBufferDirty(current->buffer);
	MarkBufferDirty(nbuf);

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;

		/* 准备 WAL 信息 */
		STORE_STATE(state, xlrec.stateSrc);

		xlrec.nMoves = nDelete;
		xlrec.replaceDead = replaceDead;
		xlrec.storesNulls = isNulls;

		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogMoveLeafs);
		XLogRegisterData((char *) toDelete,
						 sizeof(OffsetNumber) * nDelete);
		XLogRegisterData((char *) toInsert,
						 sizeof(OffsetNumber) * nInsert);
		XLogRegisterData((char *) leafdata, leafptr - leafdata);

		XLogRegisterBuffer(0, current->buffer, REGBUF_STANDARD);
		XLogRegisterBuffer(1, nbuf, REGBUF_STANDARD | (xlrec.newPage ? REGBUF_WILL_INIT : 0));
		XLogRegisterBuffer(2, parent->buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_MOVE_LEAFS);

		PageSetLSN(current->page, recptr);
		PageSetLSN(npage, recptr);
		PageSetLSN(parent->page, recptr);
	}

	END_CRIT_SECTION();

	/* 更新本地空闲空间缓存并释放新缓冲区 */
	SpGistSetLastUsedPage(index, nbuf);
	UnlockReleaseBuffer(nbuf);
}

/*
 * 用适当的目标更新先前创建的重定向元组
 *
 * 当不方便先知道目标时使用此函数。该元组本应以元页这一
 * "不可能" 的目标
 * 创建。
 */
static void
setRedirectionTuple(SPPageDesc *current, OffsetNumber position,
					BlockNumber blkno, OffsetNumber offnum)
{
	SpGistDeadTuple dt;

	dt = (SpGistDeadTuple) PageGetItem(current->page,
									   PageGetItemId(current->page, position));
	Assert(dt->tupstate == SPGIST_REDIRECT);
	Assert(ItemPointerGetBlockNumber(&dt->pointer) == SPGIST_METAPAGE_BLKNO);
	ItemPointerSet(&dt->pointer, blkno, offnum);
}

/*
 * 测试用户定义的 picksplit 函数是否未能完成其工作，即把
 * 所有叶子元组放进了同一节点。
 * 若是，则随机把元组划分到若干节点（均带相同
 * 标签），并返回 true
 * 以为该内部元组选择 allTheSame 模式。
 *
 * （此代码也用于为空值强制选择 allTheSame 模式。）
 *
 * 若已知叶子元组无法全部放进一页，则从检查是否使用了多于
 * 一个节点时排除最后一个元组（即迫使分裂的新传入元组）。
 * 原因是：若已有元组只放进一条链，则即便把它们全部移到
 * 空页，仍没有空间容纳新元组，于是会陷入 picksplit 尝试的
 * 无限循环。强制 allTheSame 模式通过确保旧元组会跨页分裂
 * 来避开此问题。（留给读者的练习：弄清为何即使只有一个旧
 * 元组时这也
 * 有效。）
 */
static bool
checkAllTheSame(spgPickSplitIn *in, spgPickSplitOut *out, bool tooBig,
				bool *includeNew)
{
	int			theNode;
	int			limit;
	int			i;

	/* 此刻假定可以把新叶子元组包括进来 */
	*includeNew = true;

	/* 若只有新叶子元组，不要选择 allTheSame 模式 */
	if (in->nTuples <= 1)
		return false;

	/* 若元组集放不进一页，测试时忽略新元组 */
	limit = tooBig ? in->nTuples - 1 : in->nTuples;

	/* 检查是否有多于一个节点被填充 */
	theNode = out->mapTuplesToNodes[0];
	for (i = 1; i < limit; i++)
	{
		if (out->mapTuplesToNodes[i] != theNode)
			return false;
	}

	/* 没有，因此覆盖 picksplit 函数的决定 */

	/* 若新元组在自己的节点中，则不能纳入分裂 */
	if (tooBig && out->mapTuplesToNodes[in->nTuples - 1] != theNode)
		*includeNew = false;

	out->nNodes = 8;			/* 任意数量的子节点 */

	/* 把元组随机分配到节点（注意包含新元组） */
	for (i = 0; i < in->nTuples; i++)
		out->mapTuplesToNodes[i] = i % out->nNodes;

	/* 操作类可能不用节点标签，但若用了则复制它们 */
	if (out->nodeLabels)
	{
		Datum		theLabel = out->nodeLabels[theNode];

		out->nodeLabels = (Datum *) palloc(sizeof(Datum) * out->nNodes);
		for (i = 0; i < out->nNodes; i++)
			out->nodeLabels[i] = theLabel;
	}

	/* 我们不碰前缀或叶子元组 datum 的赋值 */

	return true;
}

/*
 * current 指向我们本想把
 * newLeafTuple 加入的叶子元组链，但因
 * 页上空间不够而必须分裂该链。
 *
 * 本函数按 picksplit 的规则分裂叶子元组集，创建一条或多条
 * 新链，散布在当前页与另一叶子页上（我们假定两页叶子页
 * 足够）。创建新的内部元组，并把父的下行链接
 * 指针更新为指向该内部元组而非叶子链。
 *
 * 退出时，current 含有新内部元组的地址。
 *
 * 若在本函数中成功插入了
 * newLeafTuple 则返回 true；若调用方
 * 仍须插入（意味着可能还需要另一次
 * picksplit）则返回 false。
 * 失败可能因 picksplit
 * 结果相当不平衡，或 newLeafTuple 简直
 * 太大放不进一页。因为我们强制 picksplit 结果至少有两条链，
 * 每一轮都会从链中去掉至少一个叶子元组，因此若问题是不平衡，
 * 循环终将终止。若元组太大，我们假定反复的 picksplit 会通过
 * 反复剥前缀最终使其足够小。不过有缺陷的操作类可能造成无限
 * 循环，因此 spgdoinsert() 会检查叶子 datum
 * 是否每次都在变小。
 */
static bool
doPickSplit(Relation index, SpGistState *state,
			SPPageDesc *current, SPPageDesc *parent,
			SpGistLeafTuple newLeafTuple,
			int level, bool isNulls, bool isNew)
{
	bool		insertedNew = false;
	spgPickSplitIn in;
	spgPickSplitOut out;
	FmgrInfo   *procinfo;
	bool		includeNew;
	int			i,
				max,
				n;
	SpGistInnerTuple innerTuple;
	SpGistNodeTuple node,
			   *nodes;
	Buffer		newInnerBuffer,
				newLeafBuffer;
	uint8	   *leafPageSelect;
	int		   *leafSizes;
	OffsetNumber *toDelete;
	OffsetNumber *toInsert;
	OffsetNumber redirectTuplePos = InvalidOffsetNumber;
	OffsetNumber startOffsets[2];
	SpGistLeafTuple *oldLeafs;
	SpGistLeafTuple *newLeafs;
	Datum		leafDatums[INDEX_MAX_KEYS];
	bool		leafIsnulls[INDEX_MAX_KEYS];
	int			spaceToDelete;
	int			currentFreeSpace;
	int			totalLeafSizes;
	bool		allTheSame;
	spgxlogPickSplit xlrec;
	char	   *leafdata,
			   *leafptr;
	SPPageDesc	saveCurrent;
	int			nToDelete,
				nToInsert,
				maxToInclude;

	in.level = level;

	/*
	 * 按最大可能大小为每个叶子元组分配工作数组
	 */
	max = PageGetMaxOffsetNumber(current->page);
	n = max + 1;
	in.datums = (Datum *) palloc(sizeof(Datum) * n);
	toDelete = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	toInsert = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	oldLeafs = (SpGistLeafTuple *) palloc(sizeof(SpGistLeafTuple) * n);
	newLeafs = (SpGistLeafTuple *) palloc(sizeof(SpGistLeafTuple) * n);
	leafPageSelect = (uint8 *) palloc(sizeof(uint8) * n);

	STORE_STATE(state, xlrec.stateSrc);

	/*
	 * 形成将作为分裂结果分发的叶子元组列表；同时累计将从
	 * current 释放的空间量。（注意在非根情形下，我们实际上不会
	 * 删除旧元组，而只是用重定向或占位
	 * 替换它们。）
	 */
	nToInsert = 0;
	nToDelete = 0;
	spaceToDelete = 0;
	if (SpGistBlockIsRoot(current->blkno))
	{
		/*
		 * 我们正在分裂根（迄今根也是叶子页）。其元组未链接，因此
		 * 顺序扫描以取得全部。我们忽略 current->offnum 的
		 * 原值。
		 */
		for (i = FirstOffsetNumber; i <= max; i++)
		{
			SpGistLeafTuple it;

			it = (SpGistLeafTuple) PageGetItem(current->page,
											   PageGetItemId(current->page, i));
			if (it->tupstate == SPGIST_LIVE)
			{
				in.datums[nToInsert] =
					isNulls ? (Datum) 0 : SGLTDATUM(it, state);
				oldLeafs[nToInsert] = it;
				nToInsert++;
				toDelete[nToDelete] = i;
				nToDelete++;
				/* 我们将彻底删除该元组，因此计入全部空间 */
				spaceToDelete += it->size + sizeof(ItemIdData);
			}
			else				/* 根上的元组应为活的 */
				elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);
		}
	}
	else
	{
		/* 正常情形，只收集链中的叶子元组 */
		i = current->offnum;
		while (i != InvalidOffsetNumber)
		{
			SpGistLeafTuple it;

			Assert(i >= FirstOffsetNumber && i <= max);
			it = (SpGistLeafTuple) PageGetItem(current->page,
											   PageGetItemId(current->page, i));
			if (it->tupstate == SPGIST_LIVE)
			{
				in.datums[nToInsert] =
					isNulls ? (Datum) 0 : SGLTDATUM(it, state);
				oldLeafs[nToInsert] = it;
				nToInsert++;
				toDelete[nToDelete] = i;
				nToDelete++;
				/* 我们不会删除该元组，只是用死元组替换 */
				Assert(it->size >= SGDTSIZE);
				spaceToDelete += it->size - SGDTSIZE;
			}
			else if (it->tupstate == SPGIST_DEAD)
			{
				/* 可能把 DEAD 元组看作链的首项/唯一项 */
				Assert(i == current->offnum);
				Assert(SGLT_GET_NEXTOFFSET(it) == InvalidOffsetNumber);
				toDelete[nToDelete] = i;
				nToDelete++;
				/* 用重定向替换它不会节省空间 */
			}
			else
				elog(ERROR, "unexpected SPGiST tuple state: %d", it->tupstate);

			i = SGLT_GET_NEXTOFFSET(it);
		}
	}
	in.nTuples = nToInsert;

	/*
	 * 我们可能因值太大而需要另一次 picksplit，从而实际上并不
	 * 插入新元组，但会尽量分配足够空间以包含它；而且无论如何
	 * 它都必须包含在 picksplit 函数的输入中。因此暂不递增
	 * nToInsert。
	 */
	in.datums[in.nTuples] =
		isNulls ? (Datum) 0 : SGLTDATUM(newLeafTuple, state);
	oldLeafs[in.nTuples] = newLeafTuple;
	in.nTuples++;

	memset(&out, 0, sizeof(out));

	if (!isNulls)
	{
		/*
		 * 使用用户定义的方法执行分裂。
		 */
		procinfo = index_getprocinfo(index, 1, SPGIST_PICKSPLIT_PROC);
		FunctionCall2Coll(procinfo,
						  index->rd_indcollation[0],
						  PointerGetDatum(&in),
						  PointerGetDatum(&out));

		/*
		 * 形成新的叶子元组并累计所需总空间。
		 */
		totalLeafSizes = 0;
		for (i = 0; i < in.nTuples; i++)
		{
			if (state->leafTupDesc->natts > 1)
				spgDeformLeafTuple(oldLeafs[i],
								   state->leafTupDesc,
								   leafDatums,
								   leafIsnulls,
								   isNulls);

			leafDatums[spgKeyColumn] = out.leafTupleDatums[i];
			leafIsnulls[spgKeyColumn] = false;

			newLeafs[i] = spgFormLeafTuple(state, &oldLeafs[i]->heapPtr,
										   leafDatums,
										   leafIsnulls);
			totalLeafSizes += newLeafs[i]->size + sizeof(ItemIdData);
		}
	}
	else
	{
		/*
		 * 执行把所有元组放进一个节点的哑分裂。
		 * checkAllTheSame 将覆盖此结果并强制 allTheSame 模式。
		 */
		out.hasPrefix = false;
		out.nNodes = 1;
		out.nodeLabels = NULL;
		out.mapTuplesToNodes = palloc0(sizeof(int) * in.nTuples);

		/*
		 * 形成新的叶子元组并累计所需总空间。
		 */
		totalLeafSizes = 0;
		for (i = 0; i < in.nTuples; i++)
		{
			if (state->leafTupDesc->natts > 1)
				spgDeformLeafTuple(oldLeafs[i],
								   state->leafTupDesc,
								   leafDatums,
								   leafIsnulls,
								   isNulls);

			/*
			 * 空值树只能包含空键值。
			 */
			leafDatums[spgKeyColumn] = (Datum) 0;
			leafIsnulls[spgKeyColumn] = true;

			newLeafs[i] = spgFormLeafTuple(state, &oldLeafs[i]->heapPtr,
										   leafDatums,
										   leafIsnulls);
			totalLeafSizes += newLeafs[i]->size + sizeof(ItemIdData);
		}
	}

	/*
	 * 检查 picksplit 函数是否未能分离这些值，即把它们全部放进
	 * 同一子节点。若是，则选择 allTheSame 模式并改为创建随机
	 * 分裂。关于为何需要知道新叶子元组能否放进一页，见
	 * checkAllTheSame 的
	 * 注释。
	 */
	allTheSame = checkAllTheSame(&in, &out,
								 totalLeafSizes > SPGIST_PAGE_CAPACITY,
								 &includeNew);

	/*
	 * 若 checkAllTheSame 决定必须排除新元组，则不再进一步
	 * 考虑它。
	 */
	if (includeNew)
		maxToInclude = in.nTuples;
	else
	{
		maxToInclude = in.nTuples - 1;
		totalLeafSizes -= newLeafs[in.nTuples - 1]->size + sizeof(ItemIdData);
	}

	/*
	 * 分配每节点工作数组。由于 checkAllTheSame 可能把 out.nNodes
	 * 替换为大于输入页上元组数的值，我们不能在此之前分配这些
	 * 数组。
	 */
	nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) * out.nNodes);
	leafSizes = (int *) palloc0(sizeof(int) * out.nNodes);

	/*
	 * 形成内部元组的节点以及内部元组本身
	 */
	for (i = 0; i < out.nNodes; i++)
	{
		Datum		label = (Datum) 0;
		bool		labelisnull = (out.nodeLabels == NULL);

		if (!labelisnull)
			label = out.nodeLabels[i];
		nodes[i] = spgFormNodeTuple(state, label, labelisnull);
	}
	innerTuple = spgFormInnerTuple(state,
								   out.hasPrefix, out.prefixDatum,
								   out.nNodes, nodes);
	innerTuple->allTheSame = allTheSame;

	/*
	 * 更新 nodes[] 数组使其指向新形成的 innerTuple，以便下面
	 * 调整它们的下行链接。
	 */
	SGITITERATE(innerTuple, i, node)
	{
		nodes[i] = node;
	}

	/*
	 * 重新扫描新叶子元组，并累计每个节点下所需的空间。
	 */
	for (i = 0; i < maxToInclude; i++)
	{
		n = out.mapTuplesToNodes[i];
		if (n < 0 || n >= out.nNodes)
			elog(ERROR, "inconsistent result of SPGiST picksplit function");
		leafSizes[n] += newLeafs[i]->size + sizeof(ItemIdData);
	}

	/*
	 * 要执行分裂，必须插入一个新的内部元组，它不能放在叶子页
	 * 上；并且除非正在分裂根页，还必须把父元组的下行链接更新
	 * 为指向该内部元组。若有空间，我们会把新内部元组放在与父
	 * 元组同一页，否则需要另一个非叶子缓冲区。但若父页是根，
	 * 则不能把新内部元组加到那里，因为根页必须只有一个
	 * 内部
	 * 元组。
	 */
	xlrec.initInner = false;
	if (parent->buffer != InvalidBuffer &&
		!SpGistBlockIsRoot(parent->blkno) &&
		(SpGistPageGetFreeSpace(parent->page, 1) >=
		 innerTuple->size + sizeof(ItemIdData)))
	{
		/* 新内部元组能放进父页 */
		newInnerBuffer = parent->buffer;
	}
	else if (parent->buffer != InvalidBuffer)
	{
		/* 把元组送到下一三倍奇偶的页（见 README） */
		newInnerBuffer = SpGistGetBuffer(index,
										 GBUF_INNER_PARITY(parent->blkno + 1) |
										 (isNulls ? GBUF_NULLS : 0),
										 innerTuple->size + sizeof(ItemIdData),
										 &xlrec.initInner);
	}
	else
	{
		/* 根页分裂……内部元组将放到根页 */
		newInnerBuffer = InvalidBuffer;
	}

	/*
	 * 由已有元组转换而来的新叶子
	 * 元组应需要相同或更少的空间，
	 * 因此应能全部放进一页（虽然不一定是当前页，因为我们不能
	 * 删除旧元组而只能用占位
	 * 替换它们）。不过新传入的元组可能仍放不下，此时可能需要
	 * 再来一轮 picksplit 以进一步缩小它。
	 *
	 * 若没有空间把所有东西放回当前页，则按节点决定哪些元组去
	 * 新页。（这样做是因为叶子元组链不能跨页，因此属于同一父
	 * 节点的所有叶子元组必须放在
	 * 同一页。）
	 *
	 * 若正在分裂根页（把它从叶子页变成内部页）
	 * ，则没有叶子元组
	 * 能回到当前页；它们必须全部去
	 * 别处。
	 */
	if (!SpGistBlockIsRoot(current->blkno))
		currentFreeSpace = PageGetExactFreeSpace(current->page) + spaceToDelete;
	else
		currentFreeSpace = 0;	/* 防止把任何元组分配给 current */

	xlrec.initDest = false;

	if (totalLeafSizes <= currentFreeSpace)
	{
		/* 所有叶子元组都能放进当前页 */
		newLeafBuffer = InvalidBuffer;
		/* 若允许，标记新叶子元组已纳入插入 */
		if (includeNew)
		{
			nToInsert++;
			insertedNew = true;
		}
		for (i = 0; i < nToInsert; i++)
			leafPageSelect[i] = 0;	/* 表示当前页 */
	}
	else if (in.nTuples == 1 && totalLeafSizes > SPGIST_PAGE_CAPACITY)
	{
		/*
		 * 我们正试图通过反复存后缀来拆分长值，但还放不下。不必
		 * 分配一个用不上的第二叶子
		 * 缓冲区。
		 */
		newLeafBuffer = InvalidBuffer;
		Assert(includeNew);
		Assert(nToInsert == 0);
	}
	else
	{
		/* 我们将需要另一叶子页 */
		uint8	   *nodePageSelect;
		int			curspace;
		int			newspace;

		newLeafBuffer = SpGistGetBuffer(index,
										GBUF_LEAF | (isNulls ? GBUF_NULLS : 0),
										Min(totalLeafSizes,
											SPGIST_PAGE_CAPACITY),
										&xlrec.initDest);

		/*
		 * 尝试把节点组分配到两页。即便 totalLeafSizes 小于可用
		 * 空间，仍可能失败，因为我们不能把一组跨页
		 * 拆分。
		 */
		nodePageSelect = (uint8 *) palloc(sizeof(uint8) * out.nNodes);

		curspace = currentFreeSpace;
		newspace = PageGetExactFreeSpace(BufferGetPage(newLeafBuffer));
		for (i = 0; i < out.nNodes; i++)
		{
			if (leafSizes[i] <= curspace)
			{
				nodePageSelect[i] = 0;	/* 表示当前页 */
				curspace -= leafSizes[i];
			}
			else
			{
				nodePageSelect[i] = 1;	/* 表示新叶子页 */
				newspace -= leafSizes[i];
			}
		}
		if (curspace >= 0 && newspace >= 0)
		{
			/* 分配成功，因此可以把新叶子元组包括进来 */
			if (includeNew)
			{
				nToInsert++;
				insertedNew = true;
			}
		}
		else if (includeNew)
		{
			/* 必须把新叶子元组排除在分裂之外 */
			int			nodeOfNewTuple = out.mapTuplesToNodes[in.nTuples - 1];

			leafSizes[nodeOfNewTuple] -=
				newLeafs[in.nTuples - 1]->size + sizeof(ItemIdData);

			/* 重复节点分配过程——现在应成功 */
			curspace = currentFreeSpace;
			newspace = PageGetExactFreeSpace(BufferGetPage(newLeafBuffer));
			for (i = 0; i < out.nNodes; i++)
			{
				if (leafSizes[i] <= curspace)
				{
					nodePageSelect[i] = 0;	/* 表示当前页 */
					curspace -= leafSizes[i];
				}
				else
				{
					nodePageSelect[i] = 1;	/* 表示新叶子页 */
					newspace -= leafSizes[i];
				}
			}
			if (curspace < 0 || newspace < 0)
				elog(ERROR, "failed to divide leaf tuple groups across pages");
		}
		else
		{
			/* 哎呀，我们已经排除了新元组……不应到这里 */
			elog(ERROR, "failed to divide leaf tuple groups across pages");
		}
		/* 把按节点的分配展开为按叶子元组显示 */
		for (i = 0; i < nToInsert; i++)
		{
			n = out.mapTuplesToNodes[i];
			leafPageSelect[i] = nodePageSelect[n];
		}
	}

	/* 开始准备 WAL 记录 */
	xlrec.nDelete = 0;
	xlrec.initSrc = isNew;
	xlrec.storesNulls = isNulls;
	xlrec.isRootSplit = SpGistBlockIsRoot(current->blkno);

	leafdata = leafptr = (char *) palloc(totalLeafSizes);

	/* 从此开始对目标页做更改 */
	START_CRIT_SECTION();

	/*
	 * 从当前缓冲区删除旧叶子元组，除非正在分裂根；分裂根时
	 * 不必，因为下面会重新初始化该页。先做这一步以为重新插入
	 * 新叶子元组腾出空间。
	 */
	if (!SpGistBlockIsRoot(current->blkno))
	{
		/*
		 * 初始化缓冲区而非逐个删除元组，但仅当没有其他活元组且
		 * 仅在构建期间；否则需要为并发扫描设置重定向
		 * 元组。
		 */
		if (state->isBuild &&
			nToDelete + SpGistPageGetOpaque(current->page)->nPlaceholder ==
			PageGetMaxOffsetNumber(current->page))
		{
			SpGistInitBuffer(current->buffer,
							 SPGIST_LEAF | (isNulls ? SPGIST_NULLS : 0));
			xlrec.initSrc = true;
		}
		else if (isNew)
		{
			/* 不要把刚初始化的缓冲区暴露为备份块 */
			Assert(nToDelete == 0);
		}
		else
		{
			xlrec.nDelete = nToDelete;

			if (!state->isBuild)
			{
				/*
				 * 需要创建重定向元组（它将指向新内部元组），但此刻新元组
				 * 的位置尚不清楚。因此把重定向指针设为 "不可能" 的值，并
				 * 记住其位置以便稍后更新
				 * 元组。
				 */
				if (nToDelete > 0)
					redirectTuplePos = toDelete[0];
				spgPageIndexMultiDelete(state, current->page,
										toDelete, nToDelete,
										SPGIST_REDIRECT,
										SPGIST_PLACEHOLDER,
										SPGIST_METAPAGE_BLKNO,
										FirstOffsetNumber);
			}
			else
			{
				/*
				 * 索引构建期间没有并发搜索，因此我们
				 * 不需要创建重定向元组。
				 */
				spgPageIndexMultiDelete(state, current->page,
										toDelete, nToDelete,
										SPGIST_PLACEHOLDER,
										SPGIST_PLACEHOLDER,
										InvalidBlockNumber,
										InvalidOffsetNumber);
			}
		}
	}

	/*
	 * 把叶子元组放到适当的页上，并更新 innerTuple 各节点中的
	 * 下行链接。
	 */
	startOffsets[0] = startOffsets[1] = InvalidOffsetNumber;
	for (i = 0; i < nToInsert; i++)
	{
		SpGistLeafTuple it = newLeafs[i];
		Buffer		leafBuffer;
		BlockNumber leafBlock;
		OffsetNumber newoffset;

		/* 它要去哪一页？ */
		leafBuffer = leafPageSelect[i] ? newLeafBuffer : current->buffer;
		leafBlock = BufferGetBlockNumber(leafBuffer);

		/* 把元组链接到其节点的正确链中 */
		n = out.mapTuplesToNodes[i];

		if (ItemPointerIsValid(&nodes[n]->t_tid))
		{
			Assert(ItemPointerGetBlockNumber(&nodes[n]->t_tid) == leafBlock);
			SGLT_SET_NEXTOFFSET(it, ItemPointerGetOffsetNumber(&nodes[n]->t_tid));
		}
		else
			SGLT_SET_NEXTOFFSET(it, InvalidOffsetNumber);

		/* 把它插入页上 */
		newoffset = SpGistPageAddNewItem(state, BufferGetPage(leafBuffer),
										 (Item) it, it->size,
										 &startOffsets[leafPageSelect[i]],
										 false);
		toInsert[i] = newoffset;

		/* ……并完成链链接 */
		ItemPointerSet(&nodes[n]->t_tid, leafBlock, newoffset);

		/* 也将叶子元组拷贝到 WAL 数据中 */
		memcpy(leafptr, newLeafs[i], newLeafs[i]->size);
		leafptr += newLeafs[i]->size;
	}

	/*
	 * 对另一叶子缓冲区（若有）的修改已完成，因此标记为脏。
	 * current->buffer 将在下面完全修改完后再
	 * 标记。
	 */
	if (newLeafBuffer != InvalidBuffer)
	{
		MarkBufferDirty(newLeafBuffer);
	}

	/* 记住当前缓冲区，因为我们即将更改 "current" */
	saveCurrent = *current;

	/*
	 * 存储新的 innerTuple
	 */
	if (newInnerBuffer == parent->buffer && newInnerBuffer != InvalidBuffer)
	{
		/*
		 * 新内部元组放到父页
		 */
		Assert(current->buffer != parent->buffer);

		/* 把 "current" 重新指向新内部元组 */
		current->blkno = parent->blkno;
		current->buffer = parent->buffer;
		current->page = parent->page;
		xlrec.offnumInner = current->offnum =
			SpGistPageAddNewItem(state, current->page,
								 (Item) innerTuple, innerTuple->size,
								 NULL, false);

		/*
		 * 更新父节点链接并标记父页为脏
		 */
		xlrec.innerIsParent = true;
		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;
		saveNodeLink(index, parent, current->blkno, current->offnum);

		/*
		 * 更新重定向链接（在旧的 current 缓冲区中）
		 */
		if (redirectTuplePos != InvalidOffsetNumber)
			setRedirectionTuple(&saveCurrent, redirectTuplePos,
								current->blkno, current->offnum);

		/* 对旧 current 缓冲区的修改完成，标记为脏 */
		MarkBufferDirty(saveCurrent.buffer);
	}
	else if (parent->buffer != InvalidBuffer)
	{
		/*
		 * 新内部元组将存储在新页上
		 */
		Assert(newInnerBuffer != InvalidBuffer);

		/* 把 "current" 重新指向新内部元组 */
		current->buffer = newInnerBuffer;
		current->blkno = BufferGetBlockNumber(current->buffer);
		current->page = BufferGetPage(current->buffer);
		xlrec.offnumInner = current->offnum =
			SpGistPageAddNewItem(state, current->page,
								 (Item) innerTuple, innerTuple->size,
								 NULL, false);

		/* 对新 current 缓冲区的修改完成，标记为脏 */
		MarkBufferDirty(current->buffer);

		/*
		 * 更新父节点链接并标记父页为脏
		 */
		xlrec.innerIsParent = (parent->buffer == current->buffer);
		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;
		saveNodeLink(index, parent, current->blkno, current->offnum);

		/*
		 * 更新重定向链接（在旧的 current 缓冲区中）
		 */
		if (redirectTuplePos != InvalidOffsetNumber)
			setRedirectionTuple(&saveCurrent, redirectTuplePos,
								current->blkno, current->offnum);

		/* 对旧 current 缓冲区的修改完成，标记为脏 */
		MarkBufferDirty(saveCurrent.buffer);
	}
	else
	{
		/*
		 * 分裂根页，它原先是叶子，现在变成内部页
		 * （因此 "current" 继续指向它）
		 */
		Assert(SpGistBlockIsRoot(current->blkno));
		Assert(redirectTuplePos == InvalidOffsetNumber);

		SpGistInitBuffer(current->buffer, (isNulls ? SPGIST_NULLS : 0));
		xlrec.initInner = true;
		xlrec.innerIsParent = false;

		xlrec.offnumInner = current->offnum =
			PageAddItem(current->page, (Item) innerTuple, innerTuple->size,
						InvalidOffsetNumber, false, false);
		if (current->offnum != FirstOffsetNumber)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 innerTuple->size);

		/* 没有父链接要更新，也没有重定向要做 */
		xlrec.offnumParent = InvalidOffsetNumber;
		xlrec.nodeI = 0;

		/* 对新 current 缓冲区的修改完成，标记为脏 */
		MarkBufferDirty(current->buffer);

		/* saveCurrent 并不代表不同的缓冲区 */
		saveCurrent.buffer = InvalidBuffer;
	}

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;
		int			flags;

		XLogBeginInsert();

		xlrec.nInsert = nToInsert;
		XLogRegisterData((char *) &xlrec, SizeOfSpgxlogPickSplit);

		XLogRegisterData((char *) toDelete,
						 sizeof(OffsetNumber) * xlrec.nDelete);
		XLogRegisterData((char *) toInsert,
						 sizeof(OffsetNumber) * xlrec.nInsert);
		XLogRegisterData((char *) leafPageSelect,
						 sizeof(uint8) * xlrec.nInsert);
		XLogRegisterData((char *) innerTuple, innerTuple->size);
		XLogRegisterData(leafdata, leafptr - leafdata);

		/* 旧叶子页 */
		if (BufferIsValid(saveCurrent.buffer))
		{
			flags = REGBUF_STANDARD;
			if (xlrec.initSrc)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(0, saveCurrent.buffer, flags);
		}

		/* 新叶子页 */
		if (BufferIsValid(newLeafBuffer))
		{
			flags = REGBUF_STANDARD;
			if (xlrec.initDest)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(1, newLeafBuffer, flags);
		}

		/* 内部页 */
		flags = REGBUF_STANDARD;
		if (xlrec.initInner)
			flags |= REGBUF_WILL_INIT;
		XLogRegisterBuffer(2, current->buffer, flags);

		/* 父页，若与内部页不同 */
		if (parent->buffer != InvalidBuffer)
		{
			if (parent->buffer != current->buffer)
				XLogRegisterBuffer(3, parent->buffer, REGBUF_STANDARD);
			else
				Assert(xlrec.innerIsParent);
		}

		/* 发出 WAL 记录 */
		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_PICKSPLIT);

		/* 更新所有受影响页上的 LSNs */
		if (newLeafBuffer != InvalidBuffer)
		{
			Page		page = BufferGetPage(newLeafBuffer);

			PageSetLSN(page, recptr);
		}

		if (saveCurrent.buffer != InvalidBuffer)
		{
			Page		page = BufferGetPage(saveCurrent.buffer);

			PageSetLSN(page, recptr);
		}

		PageSetLSN(current->page, recptr);

		if (parent->buffer != InvalidBuffer)
		{
			PageSetLSN(parent->page, recptr);
		}
	}

	END_CRIT_SECTION();

	/* 更新本地空闲空间缓存并解锁缓冲区 */
	if (newLeafBuffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, newLeafBuffer);
		UnlockReleaseBuffer(newLeafBuffer);
	}
	if (saveCurrent.buffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, saveCurrent.buffer);
		UnlockReleaseBuffer(saveCurrent.buffer);
	}

	return insertedNew;
}

/*
 * spgMatchNode 动作：下降到当前内部元组的第 N 个子节点
 */
static void
spgMatchNodeAction(Relation index, SpGistState *state,
				   SpGistInnerTuple innerTuple,
				   SPPageDesc *current, SPPageDesc *parent, int nodeN)
{
	int			i;
	SpGistNodeTuple node;

	/* 若有则释放先前的父缓冲区 */
	if (parent->buffer != InvalidBuffer &&
		parent->buffer != current->buffer)
	{
		SpGistSetLastUsedPage(index, parent->buffer);
		UnlockReleaseBuffer(parent->buffer);
	}

	/* 把父重新指向当前内部元组的指定节点 */
	parent->blkno = current->blkno;
	parent->buffer = current->buffer;
	parent->page = current->page;
	parent->offnum = current->offnum;
	parent->node = nodeN;

	/* 定位该节点 */
	SGITITERATE(innerTuple, i, node)
	{
		if (i == nodeN)
			break;
	}

	if (i != nodeN)
		elog(ERROR, "failed to find requested node %d in SPGiST inner tuple",
			 nodeN);

	/* 若有，把 current 指向下行链接位置 */
	if (ItemPointerIsValid(&node->t_tid))
	{
		current->blkno = ItemPointerGetBlockNumber(&node->t_tid);
		current->offnum = ItemPointerGetOffsetNumber(&node->t_tid);
	}
	else
	{
		/* 下行链接为空，因此需要找新页 */
		current->blkno = InvalidBlockNumber;
		current->offnum = InvalidOffsetNumber;
	}

	current->buffer = InvalidBuffer;
	current->page = NULL;
}

/*
 * spgAddNode 动作：向 current 处的内部元组添加一个节点
 */
static void
spgAddNodeAction(Relation index, SpGistState *state,
				 SpGistInnerTuple innerTuple,
				 SPPageDesc *current, SPPageDesc *parent,
				 int nodeN, Datum nodeLabel)
{
	SpGistInnerTuple newInnerTuple;
	spgxlogAddNode xlrec;

	/* 不应应用于空值 */
	Assert(!SpGistPageStoresNulls(current->page));

	/* 构造带额外节点的新内部元组 */
	newInnerTuple = addNode(state, innerTuple, nodeLabel, nodeN);

	/* 准备 WAL 记录 */
	STORE_STATE(state, xlrec.stateSrc);
	xlrec.offnum = current->offnum;

	/* 除非需要更改父下行链接，否则不填充这些 */
	xlrec.parentBlk = -1;
	xlrec.offnumParent = InvalidOffsetNumber;
	xlrec.nodeI = 0;

	/* 除非元组必须移动，否则不填充这些 */
	xlrec.offnumNew = InvalidOffsetNumber;
	xlrec.newPage = false;

	if (PageGetExactFreeSpace(current->page) >=
		newInnerTuple->size - innerTuple->size)
	{
		/*
		 * 可以用新版本原地替换内部元组
		 */
		START_CRIT_SECTION();

		PageIndexTupleDelete(current->page, current->offnum);
		if (PageAddItem(current->page,
						(Item) newInnerTuple, newInnerTuple->size,
						current->offnum, false, false) != current->offnum)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 newInnerTuple->size);

		MarkBufferDirty(current->buffer);

		if (RelationNeedsWAL(index) && !state->isBuild)
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xlrec));
			XLogRegisterData((char *) newInnerTuple, newInnerTuple->size);

			XLogRegisterBuffer(0, current->buffer, REGBUF_STANDARD);

			recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_ADD_NODE);

			PageSetLSN(current->page, recptr);
		}

		END_CRIT_SECTION();
	}
	else
	{
		/*
		 * 把内部元组移到另一页，并更新父
		 */
		SpGistDeadTuple dt;
		SPPageDesc	saveCurrent;

		/*
		 * 对根页不应到达这里，因为我们只允许根页上有一个内部
		 * 元组，且 spgFormInnerTuple 总是检查内部元组不超过一页
		 * 的大小。
		 */
		if (SpGistBlockIsRoot(current->blkno))
			elog(ERROR, "cannot enlarge root tuple any more");
		Assert(parent->buffer != InvalidBuffer);

		saveCurrent = *current;

		xlrec.offnumParent = parent->offnum;
		xlrec.nodeI = parent->node;

		/*
		 * 获取与 current 具有相同奇偶性的新缓冲区，因为它将是
		 * 同一父元组的子
		 */
		current->buffer = SpGistGetBuffer(index,
										  GBUF_INNER_PARITY(current->blkno),
										  newInnerTuple->size + sizeof(ItemIdData),
										  &xlrec.newPage);
		current->blkno = BufferGetBlockNumber(current->buffer);
		current->page = BufferGetPage(current->buffer);

		/*
		 * 再确认一下新 current 与旧的不同。目前这不可能，但若
		 * SpGistGetBuffer 将来聪明到在检查空间前先删除占位
		 * 元组，也许就不不可能了。该情形表面能工作，但
		 * WAL 重放会有细微错误，因此我认为此处仅靠
		 * 断言
		 * 不够。
		 */
		if (current->blkno == saveCurrent.blkno)
			elog(ERROR, "SPGiST new buffer shouldn't be same as old buffer");

		/*
		 * 新的 current 与父缓冲区都将被修改；但注意父缓冲区可能
		 * 与新旧 current 中的任一个相同。
		 */
		if (parent->buffer == saveCurrent.buffer)
			xlrec.parentBlk = 0;
		else if (parent->buffer == current->buffer)
			xlrec.parentBlk = 1;
		else
			xlrec.parentBlk = 2;

		START_CRIT_SECTION();

		/* 插入新的…… */
		xlrec.offnumNew = current->offnum =
			SpGistPageAddNewItem(state, current->page,
								 (Item) newInnerTuple, newInnerTuple->size,
								 NULL, false);

		MarkBufferDirty(current->buffer);

		/* 更新父的下行链接并标记父页为脏 */
		saveNodeLink(index, parent, current->blkno, current->offnum);

		/*
		 * 用占位或重定向元组替换旧元组。除非正在做索引构建，否则
		 * 必须插入重定向元组以应对可能的并发扫描。无论如何都不能
		 * 只是删除它，因为那可能改变页上其他元组的偏移，从而破坏
		 * 来自其父的下行
		 * 链接。
		 */
		if (state->isBuild)
			dt = spgFormDeadTuple(state, SPGIST_PLACEHOLDER,
								  InvalidBlockNumber, InvalidOffsetNumber);
		else
			dt = spgFormDeadTuple(state, SPGIST_REDIRECT,
								  current->blkno, current->offnum);

		PageIndexTupleDelete(saveCurrent.page, saveCurrent.offnum);
		if (PageAddItem(saveCurrent.page, (Item) dt, dt->size,
						saveCurrent.offnum,
						false, false) != saveCurrent.offnum)
			elog(ERROR, "failed to add item of size %u to SPGiST index page",
				 dt->size);

		if (state->isBuild)
			SpGistPageGetOpaque(saveCurrent.page)->nPlaceholder++;
		else
			SpGistPageGetOpaque(saveCurrent.page)->nRedirection++;

		MarkBufferDirty(saveCurrent.buffer);

		if (RelationNeedsWAL(index) && !state->isBuild)
		{
			XLogRecPtr	recptr;
			int			flags;

			XLogBeginInsert();

			/* 原页 */
			XLogRegisterBuffer(0, saveCurrent.buffer, REGBUF_STANDARD);
			/* 新页 */
			flags = REGBUF_STANDARD;
			if (xlrec.newPage)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(1, current->buffer, flags);
			/* 父页（若与原页和新页都不同） */
			if (xlrec.parentBlk == 2)
				XLogRegisterBuffer(2, parent->buffer, REGBUF_STANDARD);

			XLogRegisterData((char *) &xlrec, sizeof(xlrec));
			XLogRegisterData((char *) newInnerTuple, newInnerTuple->size);

			recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_ADD_NODE);

			/* 我们不费心检查其中是否有冗余 */
			PageSetLSN(current->page, recptr);
			PageSetLSN(parent->page, recptr);
			PageSetLSN(saveCurrent.page, recptr);
		}

		END_CRIT_SECTION();

		/* 若 saveCurrent 与 current 或 parent 不同则释放它 */
		if (saveCurrent.buffer != current->buffer &&
			saveCurrent.buffer != parent->buffer)
		{
			SpGistSetLastUsedPage(index, saveCurrent.buffer);
			UnlockReleaseBuffer(saveCurrent.buffer);
		}
	}
}

/*
 * spgSplitNode 动作：把 current 处的内部元组分裂为前缀与后缀
 */
static void
spgSplitNodeAction(Relation index, SpGistState *state,
				   SpGistInnerTuple innerTuple,
				   SPPageDesc *current, spgChooseOut *out)
{
	SpGistInnerTuple prefixTuple,
				postfixTuple;
	SpGistNodeTuple node,
			   *nodes;
	BlockNumber postfixBlkno;
	OffsetNumber postfixOffset;
	int			i;
	spgxlogSplitTuple xlrec;
	Buffer		newBuffer = InvalidBuffer;

	/* 不应应用于空值 */
	Assert(!SpGistPageStoresNulls(current->page));

	/* 检查操作类给了我们合理的值 */
	if (out->result.splitTuple.prefixNNodes <= 0 ||
		out->result.splitTuple.prefixNNodes > SGITMAXNNODES)
		elog(ERROR, "invalid number of prefix nodes: %d",
			 out->result.splitTuple.prefixNNodes);
	if (out->result.splitTuple.childNodeN < 0 ||
		out->result.splitTuple.childNodeN >=
		out->result.splitTuple.prefixNNodes)
		elog(ERROR, "invalid child node number: %d",
			 out->result.splitTuple.childNodeN);

	/*
	 * 按请求的节点数构造新的前缀元组。我们将在下面填入
	 * 第 childNodeN 个节点的下行链接。
	 */
	nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) *
									   out->result.splitTuple.prefixNNodes);

	for (i = 0; i < out->result.splitTuple.prefixNNodes; i++)
	{
		Datum		label = (Datum) 0;
		bool		labelisnull;

		labelisnull = (out->result.splitTuple.prefixNodeLabels == NULL);
		if (!labelisnull)
			label = out->result.splitTuple.prefixNodeLabels[i];
		nodes[i] = spgFormNodeTuple(state, label, labelisnull);
	}

	prefixTuple = spgFormInnerTuple(state,
									out->result.splitTuple.prefixHasPrefix,
									out->result.splitTuple.prefixPrefixDatum,
									out->result.splitTuple.prefixNNodes,
									nodes);

	/* 它必须能放进 innerTuple 现在占用的空间 */
	if (prefixTuple->size > innerTuple->size)
		elog(ERROR, "SPGiST inner-tuple split must not produce longer prefix");

	/*
	 * 构造新的后缀元组，包含 innerTuple 的全部节点及相同的
	 * 节点 datum，但前缀由 picksplit
	 * 函数指定。
	 */
	nodes = palloc(sizeof(SpGistNodeTuple) * innerTuple->nNodes);
	SGITITERATE(innerTuple, i, node)
	{
		nodes[i] = node;
	}

	postfixTuple = spgFormInnerTuple(state,
									 out->result.splitTuple.postfixHasPrefix,
									 out->result.splitTuple.postfixPrefixDatum,
									 innerTuple->nNodes, nodes);

	/* 若原元组是 allTheSame，则后缀元组也是 */
	postfixTuple->allTheSame = innerTuple->allTheSame;

	/* 为 WAL 记录准备数据 */
	xlrec.newPage = false;

	/*
	 * 若不能把两个元组都放在当前页上，则为后缀元组获取新页。
	 * 特别是，不能分裂到根页。
	 *
	 * 空间计算时注意：prefixTuple 替换 innerTuple，但
	 * postfixTuple 将是新条目。
	 */
	if (SpGistBlockIsRoot(current->blkno) ||
		SpGistPageGetFreeSpace(current->page, 1) + innerTuple->size <
		prefixTuple->size + postfixTuple->size + sizeof(ItemIdData))
	{
		/*
		 * 选择下一三倍奇偶的页，因为后缀元组是
		 * 前缀元组的子
		 */
		newBuffer = SpGistGetBuffer(index,
									GBUF_INNER_PARITY(current->blkno + 1),
									postfixTuple->size + sizeof(ItemIdData),
									&xlrec.newPage);
	}

	START_CRIT_SECTION();

	/*
	 * 用前缀元组替换旧元组
	 */
	PageIndexTupleDelete(current->page, current->offnum);
	xlrec.offnumPrefix = PageAddItem(current->page,
									 (Item) prefixTuple, prefixTuple->size,
									 current->offnum, false, false);
	if (xlrec.offnumPrefix != current->offnum)
		elog(ERROR, "failed to add item of size %u to SPGiST index page",
			 prefixTuple->size);

	/*
	 * 把后缀元组放到适当的页上
	 */
	if (newBuffer == InvalidBuffer)
	{
		postfixBlkno = current->blkno;
		xlrec.offnumPostfix = postfixOffset =
			SpGistPageAddNewItem(state, current->page,
								 (Item) postfixTuple, postfixTuple->size,
								 NULL, false);
		xlrec.postfixBlkSame = true;
	}
	else
	{
		postfixBlkno = BufferGetBlockNumber(newBuffer);
		xlrec.offnumPostfix = postfixOffset =
			SpGistPageAddNewItem(state, BufferGetPage(newBuffer),
								 (Item) postfixTuple, postfixTuple->size,
								 NULL, false);
		MarkBufferDirty(newBuffer);
		xlrec.postfixBlkSame = false;
	}

	/*
	 * 并把前缀元组中的下行链接指针设为指向后缀元组。
	 * （不能通过颠倒上述两步来避免此步骤，因为页上可能没有
	 * 足够空间先插入后缀元组。）我们也必须更新
	 * prefixTuple 的本地副本，因为写入 WAL 的是
	 * 它。
	 */
	spgUpdateNodeLink(prefixTuple, out->result.splitTuple.childNodeN,
					  postfixBlkno, postfixOffset);
	prefixTuple = (SpGistInnerTuple) PageGetItem(current->page,
												 PageGetItemId(current->page, current->offnum));
	spgUpdateNodeLink(prefixTuple, out->result.splitTuple.childNodeN,
					  postfixBlkno, postfixOffset);

	MarkBufferDirty(current->buffer);

	if (RelationNeedsWAL(index) && !state->isBuild)
	{
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		XLogRegisterData((char *) prefixTuple, prefixTuple->size);
		XLogRegisterData((char *) postfixTuple, postfixTuple->size);

		XLogRegisterBuffer(0, current->buffer, REGBUF_STANDARD);
		if (newBuffer != InvalidBuffer)
		{
			int			flags;

			flags = REGBUF_STANDARD;
			if (xlrec.newPage)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(1, newBuffer, flags);
		}

		recptr = XLogInsert(RM_SPGIST_ID, XLOG_SPGIST_SPLIT_TUPLE);

		PageSetLSN(current->page, recptr);

		if (newBuffer != InvalidBuffer)
		{
			PageSetLSN(BufferGetPage(newBuffer), recptr);
		}
	}

	END_CRIT_SECTION();

	/* 更新本地空闲空间缓存并释放缓冲区 */
	if (newBuffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, newBuffer);
		UnlockReleaseBuffer(newBuffer);
	}
}

/*
 * 向索引插入一项。
 *
 * 成功返回 true；若未能完成插入（通常因与并发插入冲突）
 * 返回 false。后一情形下，调用方应以相同参数重新调用
 * spgdoinsert()。
 */
bool
spgdoinsert(Relation index, SpGistState *state,
			ItemPointer heapPtr, Datum *datums, bool *isnulls)
{
	bool		result = true;
	TupleDesc	leafDescriptor = state->leafTupDesc;
	bool		isnull = isnulls[spgKeyColumn];
	int			level = 0;
	Datum		leafDatums[INDEX_MAX_KEYS];
	int			leafSize;
	int			bestLeafSize;
	int			numNoProgressCycles = 0;
	SPPageDesc	current,
				parent;
	FmgrInfo   *procinfo = NULL;

	/*
	 * 查找用户定义 choose 函数的 FmgrInfo 一次，以节省下面
	 * 循环中的开销。
	 */
	if (!isnull)
		procinfo = index_getprocinfo(index, 1, SPGIST_CHOOSE_PROC);

	/*
	 * 准备要插入的叶子 datum。
	 *
	 * 若提供了可选的 "compress" 方法，则调用它由输入 datum
	 * 形成叶子键 datum。否则按原样存储输入 datum。由于本访问
	 * 方法不使用 index_form_tuple，必须确保待插入值未被 TOAST；
	 * FormIndexDatum 不保证这一点。但我们假定 "compress" 方法
	 * 返回未 TOAST 的
	 * 值。
	 */
	if (!isnull)
	{
		if (OidIsValid(index_getprocid(index, 1, SPGIST_COMPRESS_PROC)))
		{
			FmgrInfo   *compressProcinfo = NULL;

			compressProcinfo = index_getprocinfo(index, 1, SPGIST_COMPRESS_PROC);
			leafDatums[spgKeyColumn] =
				FunctionCall1Coll(compressProcinfo,
								  index->rd_indcollation[spgKeyColumn],
								  datums[spgKeyColumn]);
		}
		else
		{
			Assert(state->attLeafType.type == state->attType.type);

			if (state->attType.attlen == -1)
				leafDatums[spgKeyColumn] =
					PointerGetDatum(PG_DETOAST_DATUM(datums[spgKeyColumn]));
			else
				leafDatums[spgKeyColumn] = datums[spgKeyColumn];
		}
	}
	else
		leafDatums[spgKeyColumn] = (Datum) 0;

	/* 同样，确保任何 INCLUDE 值都未被 TOAST */
	for (int i = spgFirstIncludeColumn; i < leafDescriptor->natts; i++)
	{
		if (!isnulls[i])
		{
			if (TupleDescAttr(leafDescriptor, i)->attlen == -1)
				leafDatums[i] = PointerGetDatum(PG_DETOAST_DATUM(datums[i]));
			else
				leafDatums[i] = datums[i];
		}
		else
			leafDatums[i] = (Datum) 0;
	}

	/*
	 * 计算包含给定数据的叶子元组所需空间。
	 */
	leafSize = SpGistGetLeafTupleSize(leafDescriptor, leafDatums, isnulls);
	/* 也计入一个项指针 */
	leafSize += sizeof(ItemIdData);

	/*
	 * 若放不下，且操作类不能通过存后缀缩小 datum，则现在就
	 * 退出，而不是做大量无用功。
	 */
	if (leafSize > SPGIST_PAGE_CAPACITY &&
		(isnull || !state->config.longValuesOK))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
						leafSize - sizeof(ItemIdData),
						SPGIST_PAGE_CAPACITY - sizeof(ItemIdData),
						RelationGetRelationName(index)),
				 errhint("Values larger than a buffer page cannot be indexed.")));
	bestLeafSize = leafSize;

	/* 把 "current" 初始化为适当的根页 */
	current.blkno = isnull ? SPGIST_NULL_BLKNO : SPGIST_ROOT_BLKNO;
	current.buffer = InvalidBuffer;
	current.page = NULL;
	current.offnum = FirstOffsetNumber;
	current.node = -1;

	/* 此刻 "parent" 无效 */
	parent.blkno = InvalidBlockNumber;
	parent.buffer = InvalidBuffer;
	parent.page = NULL;
	parent.offnum = InvalidOffsetNumber;
	parent.node = -1;

	/*
	 * 进入循环前，尝试清除任何挂起的中断条件。若有查询取消
	 * 挂起，不妨现在而非稍后接受；而若挂起的是非取消条件，
	 * 在此处理可避免不得不重启插入并重做迄今全部
	 * 工作。
	 */
	CHECK_FOR_INTERRUPTS();

	for (;;)
	{
		bool		isNew = false;

		/*
		 * 若有查询取消挂起则退出。循环中某处必须有此检查，因为
		 * 有缺陷的操作类可能产生无限 picksplit
		 * 循环。不过，第一轮之后我们将持有缓冲区 lock(s)，
		 * ProcessInterrupts() 在此无法抛出取消错误。因此若看到
		 * 有中断挂起，就跳出循环并在下面处理。设
		 * result = false，因为若中断不是查询取消或死亡
		 * 情形，我们必须重启
		 * 插入。
		 */
		if (INTERRUPTS_PENDING_CONDITION())
		{
			result = false;
			break;
		}

		if (current.blkno == InvalidBlockNumber)
		{
			/*
			 * 创建叶子页。若 leafSize 太大放不进一页，我们暂时还不会
			 * 真正使用该页，但让 doPickSplit 的 API 总是手头有叶子页
			 * 会更简单；因此悄悄地把请求限制为一页
			 * 大小。
			 */
			current.buffer =
				SpGistGetBuffer(index,
								GBUF_LEAF | (isnull ? GBUF_NULLS : 0),
								Min(leafSize, SPGIST_PAGE_CAPACITY),
								&isNew);
			current.blkno = BufferGetBlockNumber(current.buffer);
		}
		else if (parent.buffer == InvalidBuffer)
		{
			/* 我们未持有父页 lock，因此不可能死锁 */
			current.buffer = ReadBuffer(index, current.blkno);
			LockBuffer(current.buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (current.blkno != parent.blkno)
		{
			/* 下降到新的子页 */
			current.buffer = ReadBuffer(index, current.blkno);

			/*
			 * 尝试获取子页上的锁。必须警惕与另一从该页下降到我们父页
			 * 的插入过程发生死锁（见 README）。若未能获得锁，放弃插入
			 * 并告诉调用方重新
			 * 开始。
			 *
			 * XXX 这一点可以改进，因为未能获得缓冲区锁并不能证明存在
			 * 死锁；锁可能由读进程持有，甚至只是 bgwriter/checkpointer
			 * 进程。也许值得睡眠片刻后
			 * 重试？
			 */
			if (!ConditionalLockBuffer(current.buffer))
			{
				ReleaseBuffer(current.buffer);
				UnlockReleaseBuffer(parent.buffer);
				return false;
			}
		}
		else
		{
			/* 内部元组可以存在与父相同的页上 */
			current.buffer = parent.buffer;
		}
		current.page = BufferGetPage(current.buffer);

		/* 不应到达错误类型的页 */
		if (isnull ? !SpGistPageStoresNulls(current.page) :
			SpGistPageStoresNulls(current.page))
			elog(ERROR, "SPGiST index page %u has wrong nulls flag",
				 current.blkno);

		if (SpGistPageIsLeaf(current.page))
		{
			SpGistLeafTuple leafTuple;
			int			nToSplit,
						sizeToSplit;

			leafTuple = spgFormLeafTuple(state, heapPtr, leafDatums, isnulls);
			if (leafTuple->size + sizeof(ItemIdData) <=
				SpGistPageGetFreeSpace(current.page, 1))
			{
				/* 它能放进页上，因此插入它然后完成 */
				addLeafTuple(index, state, leafTuple,
							 &current, &parent, isnull, isNew);
				break;
			}
			else if ((sizeToSplit =
					  checkSplitConditions(index, state, &current,
										   &nToSplit)) < SPGIST_PAGE_CAPACITY / 2 &&
					 nToSplit < 64 &&
					 leafTuple->size + sizeof(ItemIdData) + sizeToSplit <= SPGIST_PAGE_CAPACITY)
			{
				/*
				 * 数据量相当小，因此把整条链移到另一叶子页，而不是
				 * 分裂它。
				 */
				Assert(!isNew);
				moveLeafs(index, state, &current, &parent, leafTuple, isnull);
				break;			/* 我们完成了 */
			}
			else
			{
				/* picksplit */
				if (doPickSplit(index, state, &current, &parent,
								leafTuple, level, isnull, isNew))
					break;		/* doPickSplit 已安装新元组 */

				/* 叶子元组尚未插入 */
				pfree(leafTuple);

				/*
				 * current 现在描述新内部元组，去向它插入
				 */
				Assert(!SpGistPageIsLeaf(current.page));
				goto process_inner_tuple;
			}
		}
		else					/* 非叶子页 */
		{
			/*
			 * 应用操作类的 choose 函数，以弄清如何把给定 datum 插入
			 * 当前内部元组。
			 */
			SpGistInnerTuple innerTuple;
			spgChooseIn in;
			spgChooseOut out;

			/*
			 * spgAddNode 与 spgSplitTuple 情形会循环回到这里以完成
			 * 插入操作。以防 choose 函数有缺陷并反复产生添加或分裂
			 * 请求，检查查询取消（见上面的
			 * 注释）。
			 */
	process_inner_tuple:
			if (INTERRUPTS_PENDING_CONDITION())
			{
				result = false;
				break;
			}

			innerTuple = (SpGistInnerTuple) PageGetItem(current.page,
														PageGetItemId(current.page, current.offnum));

			in.datum = datums[spgKeyColumn];
			in.leafDatum = leafDatums[spgKeyColumn];
			in.level = level;
			in.allTheSame = innerTuple->allTheSame;
			in.hasPrefix = (innerTuple->prefixSize > 0);
			in.prefixDatum = SGITDATUM(innerTuple, state);
			in.nNodes = innerTuple->nNodes;
			in.nodeLabels = spgExtractNodeLabels(state, innerTuple);

			memset(&out, 0, sizeof(out));

			if (!isnull)
			{
				/* 使用用户定义的 choose 方法 */
				FunctionCall2Coll(procinfo,
								  index->rd_indcollation[0],
								  PointerGetDatum(&in),
								  PointerGetDatum(&out));
			}
			else
			{
				/* 强制 "match" 动作（插入到随机子节点） */
				out.resultType = spgMatchNode;
			}

			if (innerTuple->allTheSame)
			{
				/*
				 * 不允许对 allTheSame 元组做 AddNode。操作类必须说
				 * "match"（此时我们选择一个随机节点下降）或
				 * "split"。
				 */
				if (out.resultType == spgAddNode)
					elog(ERROR, "cannot add a node to an allTheSame inner tuple");
				else if (out.resultType == spgMatchNode)
					out.result.matchNode.nodeN =
						pg_prng_uint64_range(&pg_global_prng_state,
											 0, innerTuple->nNodes - 1);
			}

			switch (out.resultType)
			{
				case spgMatchNode:
					/* 下降到第 N 个子节点 */
					spgMatchNodeAction(index, state, innerTuple,
									   &current, &parent,
									   out.result.matchNode.nodeN);
					/* 按操作类请求调整 level */
					level += out.result.matchNode.levelAdd;
					/* 替换 leafDatum 并重新计算 leafSize */
					if (!isnull)
					{
						leafDatums[spgKeyColumn] = out.result.matchNode.restDatum;
						leafSize = SpGistGetLeafTupleSize(leafDescriptor,
														  leafDatums, isnulls);
						leafSize += sizeof(ItemIdData);
					}

					/*
					 * 检查新元组大小；若放不下则失败，除非
					 * 操作类说它能通过存后缀处理该
					 * 情形。
					 *
					 * 不过，操作类只能缩短叶子 datum，这可能
					 * 仍不足以使元组放得下，因为仅 INCLUDE
					 * 列就可能超过一页。取决于操作类的行为，
					 * 那可能导致无限循环——例如
					 * spgtextproc.c
					 * 在数据用尽后会反复生成
					 * 空串叶子 datum。操作类中的实际缺陷也
					 * 可能造成无限循环。为检测这种循环，检查
					 * 我们是否通过每轮减小 leafSize 取得
					 * 进展。不过这有点棘手。由于对齐方面的
					 * 考虑，总元组大小可能并非每轮都减小。
					 * 也有边缘情形使 choose 方法看起来一两
					 * 轮没有进展。我们略带任意地允许最多 10
					 * 次无进展迭代后失败。（此限制应大于
					 * MAXALIGN，以容纳每轮从叶子 datum
					 * 修剪一个字节的
					 * 操作类。）
					 */
					if (leafSize > SPGIST_PAGE_CAPACITY)
					{
						bool		ok = false;

						if (state->config.longValuesOK && !isnull)
						{
							if (leafSize < bestLeafSize)
							{
								ok = true;
								bestLeafSize = leafSize;
								numNoProgressCycles = 0;
							}
							else if (++numNoProgressCycles < 10)
								ok = true;
						}
						if (!ok)
							ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("index row size %zu exceeds maximum %zu for index \"%s\"",
											leafSize - sizeof(ItemIdData),
											SPGIST_PAGE_CAPACITY - sizeof(ItemIdData),
											RelationGetRelationName(index)),
									 errhint("Values larger than a buffer page cannot be indexed.")));
					}

					/*
					 * 循环并尝试在 "current" 插入新的 leafDatum（它可能引用
					 * 已有子元组，也可能无效以迫使我们为元组找
					 * 新
					 * 页）。
					 */
					break;
				case spgAddNode:
					/* 若节点没有标签，AddNode 没有意义 */
					if (in.nodeLabels == NULL)
						elog(ERROR, "cannot add a node to an inner tuple without node labels");
					/* 按请求向内部元组添加节点 */
					spgAddNodeAction(index, state, innerTuple,
									 &current, &parent,
									 out.result.addNode.nodeN,
									 out.result.addNode.nodeLabel);

					/*
					 * 重试插入到扩大后的节点。我们假定这次会得到
					 * MatchNode 结果。
					 */
					goto process_inner_tuple;
					break;
				case spgSplitTuple:
					/* 按请求分裂内部元组 */
					spgSplitNodeAction(index, state, innerTuple,
									   &current, &out);

					/* 重试插入到分裂后的节点 */
					goto process_inner_tuple;
					break;
				default:
					elog(ERROR, "unrecognized SPGiST choose result: %d",
						 (int) out.resultType);
					break;
			}
		}
	}							/* 结束循环 */

	/*
	 * 释放我们仍持有的任何缓冲区。注意 current 与 parent 可能
	 * 引用同一缓冲区。
	 */
	if (current.buffer != InvalidBuffer)
	{
		SpGistSetLastUsedPage(index, current.buffer);
		UnlockReleaseBuffer(current.buffer);
	}
	if (parent.buffer != InvalidBuffer &&
		parent.buffer != current.buffer)
	{
		SpGistSetLastUsedPage(index, parent.buffer);
		UnlockReleaseBuffer(parent.buffer);
	}

	/*
	 * 我们不支持在某外层函数仍持有缓冲区锁（或任何其他推迟
	 * 查询取消的原因）时被调用。若是那样，告诉调用方重试会
	 * 造成无限
	 * 循环。
	 */
	Assert(INTERRUPTS_CAN_BE_PROCESSED());

	/*
	 * 最后再次检查中断。若有查询取消，ProcessInterrupts() 在此
	 * 能够抛出错误。若只是可清除的其他类中断，返回 false 以
	 * 告诉调用方
	 * 重试。
	 */
	CHECK_FOR_INTERRUPTS();

	return result;
}
