/*-------------------------------------------------------------------------
 *
 * freespace.c
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/freespace.c
 *
 *
 * NOTES:
 *
 *	Free Space Map keeps track of the amount of free space on pages, and
 *	allows quickly searching for a page with enough free space. The FSM is
 *	stored in a dedicated relation fork of all heap relations, and those
 *	index access methods that need it (see also indexfsm.c). See README for
 *	more information.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/fsm_internals.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"


/*
 * 只用一个字节存放一页上的空闲空间量，因此把一页
 * 可能拥有的空闲空间分成 256 个不同的
 * 类别。最高类别 255 表示至少有 MaxFSMRequestSize
 * 字节空闲空间的页，次高类别表示从
 * 254 * FSM_CAT_STEP（含）到 MaxFSMRequestSize（不含）
 * 的区间。
 *
 * MaxFSMRequestSize 取决于体系结构与 BLCKSZ。假定
 * 默认的 8k BLCKSZ，且 MaxFSMRequestSize 为 8164 字节，
 * 各类别看起来是这样：
 *
 *
 * 范围	类别：
 * 0	- 31   0
 * 32	- 63   1
 * ...    ...  ...
 * 8096 - 8127 253
 * 8128 - 8163 254
 * 8164 - 8192 255
 *
 * MaxFSMRequestSize 之所以特殊，是因为若它不等于
 * 某个区间边界，空闲空间恰好为 MaxFSMRequestSize
 * 字节的页就满足不了对 MaxFSMRequestSize 字节的请求。
 * 若完全空的页上并没有超过 MaxFSMRequestSize 字节的
 * 空闲空间，那就意味着永远无法满足恰好
 * MaxFSMRequestSize 字节的请求。
 */
#define FSM_CATEGORIES	256
#define FSM_CAT_STEP	(BLCKSZ / FSM_CATEGORIES)
#define MaxFSMRequestSize	MaxHeapTupleSize

/*
 * 磁盘上这棵树的深度。我们得能寻址 2^32-1 个块，
 * 而 1626 是满足 X^3 >= 2^32-1 的最小数。同样，
 * 256 是满足 X^4 >= 2^32-1 的最小数。实践中，
 * 这意味着能凑合用三层树的最小 BLCKSZ 是 4096 字节，
 * 而我们支持的最小值是 512。
 */
#define FSM_TREE_DEPTH	((SlotsPerFSMPage >= 1626) ? 3 : 4)

#define FSM_ROOT_LEVEL	(FSM_TREE_DEPTH - 1)
#define FSM_BOTTOM_LEVEL 0

/*
 * 空闲空间映射的内部例程使用一套逻辑寻址方案。树的
 * 每一层都可以看成一个可单独寻址的文件。
 */
typedef struct
{
	int			level;			/* 层 */
	int			logpageno;		/* 层内的页号 */
} FSMAddress;

/* 根页的地址。 */
static const FSMAddress FSM_ROOT_ADDRESS = {FSM_ROOT_LEVEL, 0};

/* 在树中导航的函数 */
static FSMAddress fsm_get_child(FSMAddress parent, uint16 slot);
static FSMAddress fsm_get_parent(FSMAddress child, uint16 *slot);
static FSMAddress fsm_get_location(BlockNumber heapblk, uint16 *slot);
static BlockNumber fsm_get_heap_blk(FSMAddress addr, uint16 slot);
static BlockNumber fsm_logical_to_physical(FSMAddress addr);

static Buffer fsm_readbuf(Relation rel, FSMAddress addr, bool extend);
static Buffer fsm_extend(Relation rel, BlockNumber fsm_nblocks);

/* 把空闲空间量转换成 FSM 类别的函数 */
static uint8 fsm_space_avail_to_cat(Size avail);
static uint8 fsm_space_needed_to_cat(Size needed);
static Size fsm_space_cat_to_avail(uint8 cat);

/* 各种操作的主力函数 */
static int	fsm_set_and_search(Relation rel, FSMAddress addr, uint16 slot,
							   uint8 newValue, uint8 minValue);
static BlockNumber fsm_search(Relation rel, uint8 min_cat);
static uint8 fsm_vacuum_page(Relation rel, FSMAddress addr,
							 BlockNumber start, BlockNumber end,
							 bool *eof_p);
static bool fsm_does_block_exist(Relation rel, BlockNumber blknumber);


/******* 公开 API *******/

/*
 * GetPageWithFreeSpace：在给定关系里找一页，
 *		其空闲空间至少达到指定量。
 *
 * 成功则返回块号；否则返回 InvalidBlockNumber。
 *
 * 调用方必须准备好：等到自己拿到该页的锁时，
 * 返回页的可用空间可能已经过少。
 * 这时应报告该页实际可用的空闲空间量，
 * 然后重试（见 RecordAndGetPageWithFreeSpace）。
 * 若返回 InvalidBlockNumber，
 * 就扩展该关系。
 *
 * 若发现某条 FSM 项指向关系末尾之后的块，
 * 这里可能触发 FSM 更新。
 */
BlockNumber
GetPageWithFreeSpace(Relation rel, Size spaceNeeded)
{
	uint8		min_cat = fsm_space_needed_to_cat(spaceNeeded);

	return fsm_search(rel, min_cat);
}

/*
 * RecordAndGetPageWithFreeSpace：更新一页的信息并重试。
 *
 * 提供这种组合形式，是为了比分开调用
 * RecordPageWithFreeSpace 与 GetPageWithFreeSpace
 * 省一些上锁开销。另外还会尽量返回靠近旧页的页：
 * 若旧页所在的同一张 FSM 页上有空闲空间足够的页，
 * 就优先返回它。
 */
BlockNumber
RecordAndGetPageWithFreeSpace(Relation rel, BlockNumber oldPage,
							  Size oldSpaceAvail, Size spaceNeeded)
{
	int			old_cat = fsm_space_avail_to_cat(oldSpaceAvail);
	int			search_cat = fsm_space_needed_to_cat(spaceNeeded);
	FSMAddress	addr;
	uint16		slot;
	int			search_slot;

	/* 取得表示该堆块的 FSM 字节所在位置 */
	addr = fsm_get_location(oldPage, &slot);

	search_slot = fsm_set_and_search(rel, addr, slot, old_cat, search_cat);

	/*
	 * 若 fsm_set_and_search 找到了合适的新块，就返回它。
	 * 否则按通常方式搜索。
	 */
	if (search_slot != -1)
	{
		BlockNumber blknum = fsm_get_heap_blk(addr, search_slot);

		/*
		 * 检查 blknum 是否确实在该关系中。这种情况不要
		 * 去更新 FSM，退回到另一种情形即可。
		 */
		if (fsm_does_block_exist(rel, blknum))
			return blknum;
	}
	return fsm_search(rel, search_cat);
}

/*
 * RecordPageWithFreeSpace：更新一页的信息。
 *
 * 注意：若新的 spaceAvail 高于 FSM 里存放的旧值，
 * 这块空间可能要到下一次 FreeSpaceMapVacuum 调用、
 * 更新了上层页之后，搜索者才看得见。
 */
void
RecordPageWithFreeSpace(Relation rel, BlockNumber heapBlk, Size spaceAvail)
{
	int			new_cat = fsm_space_avail_to_cat(spaceAvail);
	FSMAddress	addr;
	uint16		slot;

	/* 取得表示该堆块的 FSM 字节所在位置 */
	addr = fsm_get_location(heapBlk, &slot);

	fsm_set_and_search(rel, addr, slot, new_cat, 0);
}

/*
 * XLogRecordPageWithFreeSpace：与 RecordPageWithFreeSpace 类似，用于
 *		WAL 重放。
 */
void
XLogRecordPageWithFreeSpace(RelFileLocator rlocator, BlockNumber heapBlk,
							Size spaceAvail)
{
	int			new_cat = fsm_space_avail_to_cat(spaceAvail);
	FSMAddress	addr;
	uint16		slot;
	BlockNumber blkno;
	Buffer		buf;
	Page		page;

	/* 取得表示该堆块的 FSM 字节所在位置 */
	addr = fsm_get_location(heapBlk, &slot);
	blkno = fsm_logical_to_physical(addr);

	/* 若该页还不存在，就扩展 */
	buf = XLogReadBufferExtended(rlocator, FSM_FORKNUM, blkno,
								 RBM_ZERO_ON_ERROR, InvalidBuffer);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	if (PageIsNew(page))
		PageInit(page, BLCKSZ, 0);

	if (fsm_set_avail(page, slot, new_cat))
		MarkBufferDirtyHint(buf, false);
	UnlockReleaseBuffer(buf);
}

/*
 * GetRecordedFreeSpace：按 FSM 返回某一页的
 *		空闲空间量。
 */
Size
GetRecordedFreeSpace(Relation rel, BlockNumber heapBlk)
{
	FSMAddress	addr;
	uint16		slot;
	Buffer		buf;
	uint8		cat;

	/* 取得表示该堆块的 FSM 字节所在位置 */
	addr = fsm_get_location(heapBlk, &slot);

	buf = fsm_readbuf(rel, addr, false);
	if (!BufferIsValid(buf))
		return 0;
	cat = fsm_get_avail(BufferGetPage(buf), slot);
	ReleaseBuffer(buf);

	return fsm_space_cat_to_avail(cat);
}

/*
 * FreeSpaceMapPrepareTruncateRel：为截断关系做准备。
 *
 * nblocks 是堆的新大小。
 *
 * 返回新的空闲空间映射的块数。
 * 若为 InvalidBlockNumber，就没有什么可截断的；
 * 否则调用方负责调用 smgrtruncate()
 * 来截断 FSM 页，并调用 FreeSpaceMapVacuumRange()
 * 来更新 FSM 中的上层页。
 */
BlockNumber
FreeSpaceMapPrepareTruncateRel(Relation rel, BlockNumber nblocks)
{
	BlockNumber new_nfsmblocks;
	FSMAddress	first_removed_address;
	uint16		first_removed_slot;
	Buffer		buf;

	/*
	 * 若这个关系还没有创建 FSM，就没有什么
	 * 可截断的。
	 */
	if (!smgrexists(RelationGetSmgr(rel), FSM_FORKNUM))
		return InvalidBlockNumber;

	/* 取得 FSM 中第一个被去掉的堆块的位置 */
	first_removed_address = fsm_get_location(nblocks, &first_removed_slot);

	/*
	 * 把最后保留的那张 FSM 页的尾部清零。若表示
	 * 第一个被去掉的堆块的槽正处在页边界上，也就是
	 * first_removed_address 所指 FSM 页的第一个槽，
	 * 就可以把那一页整个截掉。
	 */
	if (first_removed_slot > 0)
	{
		buf = fsm_readbuf(rel, first_removed_address, false);
		if (!BufferIsValid(buf))
			return InvalidBlockNumber;	/* 无事可做；FSM 本来就已经
										 * 更小。 */
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/* 从这里直到变更记入日志之前，不要 EREPORT(ERROR) */
		START_CRIT_SECTION();

		fsm_truncate_avail(BufferGetPage(buf), first_removed_slot);

		/*
		 * 这次修改并非关键，因为 fsm_does_block_exist() 会
		 * 阻止我们返回已被截掉的块。不过这次最多会去掉
		 * SlotsPerFSMPage 个槽，最好避免那么多次
		 * fsm_does_block_exist() 拒绝的开销。请使用完整的
		 * MarkBufferDirty()，而不是 MarkBufferDirtyHint()。
		 */
		MarkBufferDirty(buf);

		/*
		 * 按 MarkBufferDirtyHint() 可能采用的方式记 WAL，以免
		 * 本文件在这方面与其余代码不一致。这是可选的；
		 * 见 README 对整页映像的说明。XXX：可考虑
		 * XLogSaveBufferForHint()，以便更接近。
		 *
		 * 更高层的操作在 WAL 重放时调用我们。若在
		 * XLOG_SMGR_TRUNCATE 刷到磁盘之前崩溃，主分叉长度
		 * 并未改变，我们的分叉仍然有效。若在那次
		 * 刷盘之后崩溃，重做会回到这里。
		 */
		if (!InRecovery && RelationNeedsWAL(rel) && XLogHintBitIsNeeded())
			log_newpage_buffer(buf, false);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		new_nfsmblocks = fsm_logical_to_physical(first_removed_address) + 1;
	}
	else
	{
		new_nfsmblocks = fsm_logical_to_physical(first_removed_address);
		if (smgrnblocks(RelationGetSmgr(rel), FSM_FORKNUM) <= new_nfsmblocks)
			return InvalidBlockNumber;	/* 无事可做；FSM 本来就已经
										 * 更小。 */
	}

	return new_nfsmblocks;
}

/*
 * FreeSpaceMapVacuum：更新该关系空闲空间映射中的上层页。
 *
 * 假定最底层页已经用新的空闲空间信息
 * 更新过了。
 */
void
FreeSpaceMapVacuum(Relation rel)
{
	bool		dummy;

	/* 从根开始递归扫描这棵树 */
	(void) fsm_vacuum_page(rel, FSM_ROOT_ADDRESS,
						   (BlockNumber) 0, InvalidBlockNumber,
						   &dummy);
}

/*
 * FreeSpaceMapVacuumRange：更新该关系空闲空间映射中的上层页。
 *
 * 同上，但假定只有 start 与 end-1（含）之间的堆页
 * 有新的空闲空间信息，因此只更新覆盖该块范围的
 * 上层槽。end == InvalidBlockNumber 等价于
 * 该关系其余的全部。
 */
void
FreeSpaceMapVacuumRange(Relation rel, BlockNumber start, BlockNumber end)
{
	bool		dummy;

	/* 从根开始递归扫描这棵树 */
	if (end > start)
		(void) fsm_vacuum_page(rel, FSM_ROOT_ADDRESS, start, end, &dummy);
}

/******* 内部例程 *******/

/*
 * 返回与 x 字节空闲空间对应的类别
 */
static uint8
fsm_space_avail_to_cat(Size avail)
{
	int			cat;

	Assert(avail < BLCKSZ);

	if (avail >= MaxFSMRequestSize)
		return 255;

	cat = avail / FSM_CAT_STEP;

	/*
	 * 最高类别 255 留给 MaxFSMRequestSize 字节或
	 * 更多。
	 */
	if (cat > 254)
		cat = 254;

	return (uint8) cat;
}

/*
 * 返回给定类别所表示的空闲空间区间的
 * 下界。
 */
static Size
fsm_space_cat_to_avail(uint8 cat)
{
	/* 最高类别恰好表示 MaxFSMRequestSize 字节。 */
	if (cat == 255)
		return MaxFSMRequestSize;
	else
		return cat * FSM_CAT_STEP;
}

/*
 * 一页要属于哪个类别，才能容纳 x 字节的数据？
 * fsm_space_avail_to_cat() 向下取整，这里则需要向上取整。
 */
static uint8
fsm_space_needed_to_cat(Size needed)
{
	int			cat;

	/* 不能索取比最高类别所表示的更多的空间 */
	if (needed > MaxFSMRequestSize)
		elog(ERROR, "invalid FSM request size %zu", needed);

	if (needed == 0)
		return 1;

	cat = (needed + FSM_CAT_STEP - 1) / FSM_CAT_STEP;

	if (cat > 255)
		cat = 255;

	return (uint8) cat;
}

/*
 * 返回一张 FSM 页的物理块号
 */
static BlockNumber
fsm_logical_to_physical(FSMAddress addr)
{
	BlockNumber pages;
	int			leafno;
	int			l;

	/*
	 * 计算给定页之下第一张叶页的
	 * 逻辑页号。
	 */
	leafno = addr.logpageno;
	for (l = 0; l < addr.level; l++)
		leafno *= SlotsPerFSMPage;

	/* 统计寻址该叶页所需的上层结点数 */
	pages = 0;
	for (l = 0; l < FSM_TREE_DEPTH; l++)
	{
		pages += leafno + 1;
		leafno /= SlotsPerFSMPage;
	}

	/*
	 * 若所要的页不在最底层，就减去上面
	 * 多算进去的那些更低层页。
	 */
	pages -= addr.level;

	/* 把页数换成从 0 起算的块号 */
	return pages - 1;
}

/*
 * 返回与给定堆块对应的 FSM 位置。
 */
static FSMAddress
fsm_get_location(BlockNumber heapblk, uint16 *slot)
{
	FSMAddress	addr;

	addr.level = FSM_BOTTOM_LEVEL;
	addr.logpageno = heapblk / SlotsPerFSMPage;
	*slot = heapblk % SlotsPerFSMPage;

	return addr;
}

/*
 * 返回与 FSM 中给定位置对应的堆块号。
 */
static BlockNumber
fsm_get_heap_blk(FSMAddress addr, uint16 slot)
{
	Assert(addr.level == FSM_BOTTOM_LEVEL);
	return ((unsigned int) addr.logpageno) * SlotsPerFSMPage + slot;
}

/*
 * 给定子页的逻辑地址，求出父页的逻辑页号，
 * 以及父页中对应该子页的槽。
 */
static FSMAddress
fsm_get_parent(FSMAddress child, uint16 *slot)
{
	FSMAddress	parent;

	Assert(child.level < FSM_ROOT_LEVEL);

	parent.level = child.level + 1;
	parent.logpageno = child.logpageno / SlotsPerFSMPage;
	*slot = child.logpageno % SlotsPerFSMPage;

	return parent;
}

/*
 * 给定父页的逻辑地址和槽号，求出
 * 对应子页的逻辑地址。
 */
static FSMAddress
fsm_get_child(FSMAddress parent, uint16 slot)
{
	FSMAddress	child;

	Assert(parent.level > FSM_BOTTOM_LEVEL);

	child.level = parent.level - 1;
	child.logpageno = parent.logpageno * SlotsPerFSMPage + slot;

	return child;
}

/*
 * 读取一张 FSM 页。
 *
 * 若该页不存在，则返回 InvalidBuffer；若 extend 为
 * 真，则扩展 FSM 文件。
 */
static Buffer
fsm_readbuf(Relation rel, FSMAddress addr, bool extend)
{
	BlockNumber blkno = fsm_logical_to_physical(addr);
	Buffer		buf;
	SMgrRelation reln;

	/*
	 * 注意：若 relcache 项被关闭，复用这个 smgr 指针
	 * 可能失败。只要从这里到最后一次使用该指针之间
	 * 只做 smgr 层的操作，就是安全的。
	 */
	reln = RelationGetSmgr(rel);

	/*
	 * 若尚未缓存 FSM 的大小，先检查它。若请求的块
	 * 看起来已越过末尾，也要再查一次，因为缓存值
	 * 可能已过时。（截断时会发送 smgr 失效消息，
	 * 扩展时则不会。）
	 */
	if (reln->smgr_cached_nblocks[FSM_FORKNUM] == InvalidBlockNumber ||
		blkno >= reln->smgr_cached_nblocks[FSM_FORKNUM])
	{
		/* 使缓存失效，以便 smgrnblocks 去问内核。 */
		reln->smgr_cached_nblocks[FSM_FORKNUM] = InvalidBlockNumber;
		if (smgrexists(reln, FSM_FORKNUM))
			smgrnblocks(reln, FSM_FORKNUM);
		else
			reln->smgr_cached_nblocks[FSM_FORKNUM] = 0;
	}

	/*
	 * 读取时使用 ZERO_ON_ERROR 模式，必要时初始化该页。
	 * FSM 信息本来就不精确，清掉损坏页好过因此报错。
	 * 由于 FSM 变更不记 WAL，崩溃时所谓的撕裂页问题
	 * 可能导致页头损坏，
	 * 例如。
	 *
	 * 扩展关系时，下面用同一条路径来初始化页，
	 * 因为并发扩展可能使 vm_extend()
	 * 返回一张已经初始化过的页。
	 */
	if (blkno >= reln->smgr_cached_nblocks[FSM_FORKNUM])
	{
		if (extend)
			buf = fsm_extend(rel, blkno + 1);
		else
			return InvalidBuffer;
	}
	else
		buf = ReadBufferExtended(rel, FSM_FORKNUM, blkno, RBM_ZERO_ON_ERROR, NULL);

	/*
	 * 需要时才初始化该页，这比看上去更棘手，因为
	 * 可能有多个后端同时在做，而我们希望在页
	 * 正常的常见路径上，不要无谓地拿缓冲区锁。
	 * 初始化该页必须拿锁，所以拿到锁之后要重新
	 * 检查它是否仍是新页，以免别人已经初始化过。
	 * 另外，最初检查 PageIsNew 时并没有拿锁，
	 * 因此可能跳过初始化、直接返回该缓冲区，而别人
	 * 仍在初始化该页（例如看到 pd_upper 已设置，
	 * 页头其余字段却仍是零）。自己会拿缓冲区锁的
	 * 调用方不受此影响，但有些调用方查看该页时
	 * 完全不上锁。后者只有在不依赖页头内容正确时
	 * 才成立。当前用法是安全的，因为 PageGetContents()
	 * 并不需要页头内容正确。
	 */
	if (PageIsNew(BufferGetPage(buf)))
	{
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(BufferGetPage(buf)))
			PageInit(BufferGetPage(buf), BLCKSZ, 0);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}
	return buf;
}

/*
 * 确保 FSM 分叉至少有 fsm_nblocks 长，必要时用空页
 * 来扩展。这里的空是指页内填满零，
 * 也就是没有空闲空间。
 */
static Buffer
fsm_extend(Relation rel, BlockNumber fsm_nblocks)
{
	return ExtendBufferedRelTo(BMR_REL(rel), FSM_FORKNUM, NULL,
							   EB_CREATE_FORK_IF_NEEDED |
							   EB_CLEAR_SIZE_CACHE,
							   fsm_nblocks,
							   RBM_ZERO_ON_ERROR);
}

/*
 * 设置给定 FSM 页和槽中的值。
 *
 * 若 minValue > 0，还会在更新后的页上搜索至少有
 * minValue 空闲空间的页。找到则返回其槽号，
 * 否则返回 -1。
 */
static int
fsm_set_and_search(Relation rel, FSMAddress addr, uint16 slot,
				   uint8 newValue, uint8 minValue)
{
	Buffer		buf;
	Page		page;
	int			newslot = -1;

	buf = fsm_readbuf(rel, addr, true);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);

	if (fsm_set_avail(page, slot, newValue))
		MarkBufferDirtyHint(buf, false);

	if (minValue != 0)
	{
		/* 持有锁期间进行搜索 */
		newslot = fsm_search_avail(buf, minValue,
								   addr.level == FSM_BOTTOM_LEVEL,
								   true);
	}

	UnlockReleaseBuffer(buf);

	return newslot;
}

/*
 * 在树中搜索至少有 min_cat 空闲空间的堆页
 */
static BlockNumber
fsm_search(Relation rel, uint8 min_cat)
{
	int			restarts = 0;
	FSMAddress	addr = FSM_ROOT_ADDRESS;

	for (;;)
	{
		int			slot;
		Buffer		buf;
		uint8		max_avail = 0;

		/* 读取 FSM 页。 */
		buf = fsm_readbuf(rel, addr, false);

		/* 在页内搜索 */
		if (BufferIsValid(buf))
		{
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			slot = fsm_search_avail(buf, min_cat,
									(addr.level == FSM_BOTTOM_LEVEL),
									false);
			if (slot == -1)
			{
				max_avail = fsm_get_max_avail(BufferGetPage(buf));
				UnlockReleaseBuffer(buf);
			}
			else
			{
				/* 保持钉住，以便下面可能的更新 */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			}
		}
		else
			slot = -1;

		if (slot != -1)
		{
			/*
			 * 沿着树向下；若已在最底层，则返回
			 * 找到的块。
			 */
			if (addr.level == FSM_BOTTOM_LEVEL)
			{
				BlockNumber blkno = fsm_get_heap_blk(addr, slot);
				Page		page;

				if (fsm_does_block_exist(rel, blkno))
				{
					ReleaseBuffer(buf);
					return blkno;
				}

				/*
				 * 块已越过关系末尾。更新 FSM，并
				 * 从根重新开始。这种少见情形下，通常的 advancenext
				 * 行为很糟，因为后面每个槽都同样不可用。
				 * 可以把同一张 FSM 页上受影响的槽全部清零，但别指望
				 * 那种优化的收益能抵上它增加的
				 * 编译后代码体积。
				 */
				page = BufferGetPage(buf);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				fsm_set_avail(page, slot, 0);
				MarkBufferDirtyHint(buf, false);
				UnlockReleaseBuffer(buf);
				if (restarts++ > 10000) /* 理由与下面相同 */
					return InvalidBlockNumber;
				addr = FSM_ROOT_ADDRESS;
			}
			else
			{
				ReleaseBuffer(buf);
			}
			addr = fsm_get_child(addr, slot);
		}
		else if (addr.level == FSM_ROOT_LEVEL)
		{
			/*
			 * 在根上失败，表示 FSM 里没有空闲空间
			 * 足够的页。放弃。
			 */
			return InvalidBlockNumber;
		}
		else
		{
			uint16		parentslot;
			FSMAddress	parent;

			/*
			 * 在更低层失败，可能是因为上层结点的值
			 * 并没有反映下层页上的值。更新上层结点，
			 * 以免再掉进同一个陷阱，然后
			 * 重新开始。
			 *
			 * 这里有竞争：若另一个后端在我们放开该页之后
			 * 立刻更新它，并抢在我们之前拿到父页的锁，
			 * 我们随后就会用已经过时的信息去更新父页。
			 * 这没关系，因为应当很少发生，
			 * 而且下一次清理会把它修好。
			 */
			parent = fsm_get_parent(addr, &parentslot);
			fsm_set_and_search(rel, parent, parentslot, max_avail, 0);

			/*
			 * 若上层页严重过时，我们可能要循环相当多次，
			 * 一边走一边更新它们。不一致最终应当被纠正，
			 * 循环也应当结束。无限循环毕竟吓人，
			 * 所以提供一个紧急
			 * 阀门。
			 */
			if (restarts++ > 10000)
				return InvalidBlockNumber;

			/* 从根把搜索整个重来一遍 */
			addr = FSM_ROOT_ADDRESS;
		}
	}
}


/*
 * FreeSpaceMapVacuum 的递归主体。
 *
 * 检查 addr 标明的 FSM 页及其子页，更新覆盖堆块范围
 * start 到 end-1 的上层结点。
 * （end 超出该映射的实际末尾也没关系。）
 * 向调用方返回本页上的最大空闲空间值。
 *
 * 若 addr 已越过 FSM 末尾，把 *eof_p 设为真并返回 0。
 *
 * 按深度优先顺序遍历这棵树。树在物理上也按
 * 深度优先存放，因此这样应当相当节省 I/O。
 */
static uint8
fsm_vacuum_page(Relation rel, FSMAddress addr,
				BlockNumber start, BlockNumber end,
				bool *eof_p)
{
	Buffer		buf;
	Page		page;
	uint8		max_avail;

	/* 若该页存在就读取，否则返回 EOF */
	buf = fsm_readbuf(rel, addr, false);
	if (!BufferIsValid(buf))
	{
		*eof_p = true;
		return 0;
	}
	else
		*eof_p = false;

	page = BufferGetPage(buf);

	/*
	 * 若位于最底层之上，就递归进入子页，并修正
	 * 本层所存放的关于它们的信息。
	 */
	if (addr.level > FSM_BOTTOM_LEVEL)
	{
		FSMAddress	fsm_start,
					fsm_end;
		uint16		fsm_start_slot,
					fsm_end_slot;
		int			slot,
					start_slot,
					end_slot;
		bool		eof = false;

		/*
		 * 根据要考虑的堆块范围，算出本页上需要更新的槽范围。
		 * 第一个要更新的槽是覆盖 start 块的那个，最后一个槽是
		 * 覆盖 end - 1 的那个。（这些工作在每次递归调用里
		 * 都会重复算一遍，但足够便宜，
		 * 不必担心。）
		 */
		fsm_start = fsm_get_location(start, &fsm_start_slot);
		fsm_end = fsm_get_location(end - 1, &fsm_end_slot);

		while (fsm_start.level < addr.level)
		{
			fsm_start = fsm_get_parent(fsm_start, &fsm_start_slot);
			fsm_end = fsm_get_parent(fsm_end, &fsm_end_slot);
		}
		Assert(fsm_start.level == addr.level);

		if (fsm_start.logpageno == addr.logpageno)
			start_slot = fsm_start_slot;
		else if (fsm_start.logpageno > addr.logpageno)
			start_slot = SlotsPerFSMPage;	/* 不该走到这里…… */
		else
			start_slot = 0;

		if (fsm_end.logpageno == addr.logpageno)
			end_slot = fsm_end_slot;
		else if (fsm_end.logpageno > addr.logpageno)
			end_slot = SlotsPerFSMPage - 1;
		else
			end_slot = -1;		/* 不该走到这里…… */

		for (slot = start_slot; slot <= end_slot; slot++)
		{
			int			child_avail;

			CHECK_FOR_INTERRUPTS();

			/* 碰到文件末尾之后，把其余槽清掉即可 */
			if (!eof)
				child_avail = fsm_vacuum_page(rel, fsm_get_child(addr, slot),
											  start, end,
											  &eof);
			else
				child_avail = 0;

			/* 更新关于该子页的信息 */
			if (fsm_get_avail(page, slot) != child_avail)
			{
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				fsm_set_avail(page, slot, child_avail);
				MarkBufferDirtyHint(buf, false);
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			}
		}
	}

	/* 现在取本页上的最大值，返回给调用方 */
	max_avail = fsm_get_max_avail(page);

	/*
	 * 重置下一槽指针。这鼓励使用编号较小的页，
	 * 从而增加以后清理能截断该关系的机会。
	 * 这里不上锁，若页本来不脏也不把它标脏，
	 * 因为这只是提示。
	 */
	((FSMPage) PageGetContents(page))->fp_next_slot = 0;

	ReleaseBuffer(buf);

	return max_avail;
}


/*
 * 检查块号是否已越过关系末尾。WAL 重放之后可能
 * 发生这种情况：FSM 已落到磁盘，但它所指的新扩展页
 * 还没有。
 */
static bool
fsm_does_block_exist(Relation rel, BlockNumber blknumber)
{
	SMgrRelation smgr = RelationGetSmgr(rel);

	/*
	 * 若低于已缓存的 nblocks，该块肯定存在。否则就面临
	 * 取舍。我们选择与一份新的 nblocks 比较，承担
	 * lseek() 的开销。另一种做法是假定该块不存在，
	 * 但那样会使 FSM 把主分叉扩展刚记录下来的块
	 * 标成可用空间为零。
	 */
	return ((BlockNumberIsValid(smgr->smgr_cached_nblocks[MAIN_FORKNUM]) &&
			 blknumber < smgr->smgr_cached_nblocks[MAIN_FORKNUM]) ||
			blknumber < RelationGetNumberOfBlocks(rel));
}
