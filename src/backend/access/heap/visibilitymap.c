/*-------------------------------------------------------------------------
 *
 * visibilitymap.c
 *	  跟踪堆元组可见性的位图
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/visibilitymap.c
 *
 * 接口例程（INTERFACE ROUTINES）
 *		visibilitymap_clear  - 清除可见性映射中某一页面的位
 *		visibilitymap_pin	 - 钉住映射页面以便设置某一位
 *		visibilitymap_pin_ok - 检查正确的映射页面是否已经钉住
 *		visibilitymap_set	 - 在先前钉住的页面上设置某一位
 *		visibilitymap_get_status - 取得各位的状态
 *		visibilitymap_count  - 统计可见性映射中已置位的个数
 *		visibilitymap_prepare_truncate -
 *			为截断可见性映射做准备
 *
 * 说明（NOTES）
 *
 * 可见性映射是一张位图，每个堆页面占两位（全可见与全冻结）。
 * 置位的全可见位表示该页面上所有元组
 * 已知对全部事务可见，因此该页面不需要
 * 清理。置位的全冻结位表示该页面上所有元组
 * 已完全冻结，因此即便需要整表扫描式清理
 * （例如防回卷清理），该页面也不需要清理。
 * 全冻结位只能在该页面已经全可见时设置。
 *
 * 这张映射是保守的：只要某一位置位，我们就确知条件为真；
 * 但若某一位未置位，条件可能为真，
 * 也可能不为真。
 *
 * 清除可见性映射位并不单独记入 WAL。调用方必须保证：
 * 每当某一位被清除时，在更新操作的 WAL
 * 重放中该位同样被清除。
 *
 * 在清理（VACUUM）期间*设置*可见性映射时，必须写 WAL。这
 * 看似有违直觉，因为该位本质上是提示：若它被清除，
 * 仍可能该页面上每个元组对全部事务可见；
 * 我们只是不能就此确定。难点在于
 * 通常一起设置的有两位：页面自身上的 PD_ALL_VISIBLE 位，
 * 以及可见性映射位。若崩溃发生在
 * 可见性映射页已经落盘、但更新后的堆页面还
 * 没有落盘时，重做必须在堆页面上设置该位。否则，下一次
 * 对该堆页面的插入、更新或删除将意识不到
 * 必须清除可见性映射位，可能使仅索引扫描
 * 返回错误结果。
 *
 * 清理（VACUUM）通常会跳过可见性映射位已置位的页面；
 * 这样的页面不可能含有任何死元组，因此不需要清理。
 *
 * 加锁（LOCKING）
 *
 * 在 heapam.c 中，每当修改页面使得其上不再是所有元组
 * 都对所有人可见时，就清除可见性映射中
 * 对应的位。为了崩溃安全，必须在
 * 仍持有堆页面锁，并且处在记录该页修改的同一
 * 临界区中。然而，我们不想在读取可见性
 * 映射页可能需要的任何 I/O 期间一直持有缓冲区锁。
 * 为避免这一点，我们在加锁之前先检查堆页面；
 * 若页面级 PD_ALL_VISIBLE 位已置位，就钉住可见性映射
 * 位。然后锁定缓冲区。但这会造成竞态：
 * 在锁定缓冲区所花费的时间里，
 * PD_ALL_VISIBLE 位可能被置位。若发生这种情况，必须解锁
 * 缓冲区、钉住可见性映射页，再重新锁定缓冲区。这不应当
 * 经常发生，因为目前只有清理（VACUUM）会设置可见性映射位，
 * 并且只有当清理（VACUUM）处理某一页面的时刻几乎
 * 与别人试图进一步修改它完全相同时，才会出现这次竞态。
 *
 * 要设置某一位，必须持有堆页面上的锁。这可以防止
 * 如下竞态：清理（VACUUM）看到该页面上所有元组
 * 对所有人可见，但另一个后端在清理（VACUUM）
 * 于可见性映射中设置该位之前修改了该页。
 *
 * 某一位置位时，会更新可见性映射页的 LSN，以确保
 * 在使该位置位成为可能的那些变更的 WAL 记录刷出之前，
 * 可见性映射更新不会被写到磁盘。
 * 但清除某一位时不必这样做，因为从正确性
 * 角度看，清除映射中的位总是安全的。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/inval.h"


/*#define TRACE_VISIBILITYMAP */

/*
 * 每张可见性映射页上位图的大小，以字节计。没有
 * 额外的头，因此整页减去标准页头之后
 * 都用于位图。
 */
#define MAPSIZE (BLCKSZ - MAXALIGN(SizeOfPageHeaderData))

/* 一个字节所能表示的堆块数*/
#define HEAPBLOCKS_PER_BYTE (BITS_PER_BYTE / BITS_PER_HEAPBLOCK)

/* 一张可见性映射页所能表示的堆块数。*/
#define HEAPBLOCKS_PER_PAGE (MAPSIZE * HEAPBLOCKS_PER_BYTE)

/* 从堆块号映射到可见性映射中对应位*/
#define HEAPBLK_TO_MAPBLOCK(x) ((x) / HEAPBLOCKS_PER_PAGE)
#define HEAPBLK_TO_MAPBYTE(x) (((x) % HEAPBLOCKS_PER_PAGE) / HEAPBLOCKS_PER_BYTE)
#define HEAPBLK_TO_OFFSET(x) (((x) % HEAPBLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK)

/* 用于统计可见性映射中部分位的掩码。*/
#define VISIBLE_MASK64	UINT64CONST(0x5555555555555555) /* 每一对中较低的
														 * 那一位 */
#define FROZEN_MASK64	UINT64CONST(0xaaaaaaaaaaaaaaaa) /* 每一对中较高的
														 * 那一位 */

/* 内部例程的原型*/
static Buffer vm_readbuf(Relation rel, BlockNumber blkno, bool extend);
static Buffer vm_extend(Relation rel, BlockNumber vm_nblocks);


/*
 *	visibilitymap_clear - 清除可见性映射中某一页面的指定位
 *
 * 必须把含有正确映射页的缓冲区传给本函数。
 * 先调用 visibilitymap_pin 钉住正确的那一页。本函数不做
 * 任何 I/O。若有任何位被清除则返回真，否则返回假。
 */
bool
visibilitymap_clear(Relation rel, BlockNumber heapBlk, Buffer vmbuf, uint8 flags)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	int			mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	int			mapOffset = HEAPBLK_TO_OFFSET(heapBlk);
	uint8		mask = flags << mapOffset;
	char	   *map;
	bool		cleared = false;

	/* 绝不能在 all_frozen 位仍置位时清除 all_visible 位*/
	Assert(flags & VISIBILITYMAP_VALID_BITS);
	Assert(flags != VISIBILITYMAP_ALL_VISIBLE);

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_clear %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	if (!BufferIsValid(vmbuf) || BufferGetBlockNumber(vmbuf) != mapBlock)
		elog(ERROR, "wrong buffer passed to visibilitymap_clear");

	LockBuffer(vmbuf, BUFFER_LOCK_EXCLUSIVE);
	map = PageGetContents(BufferGetPage(vmbuf));

	if (map[mapByte] & mask)
	{
		map[mapByte] &= ~mask;

		MarkBufferDirty(vmbuf);
		cleared = true;
	}

	LockBuffer(vmbuf, BUFFER_LOCK_UNLOCK);

	return cleared;
}

/*
 *	visibilitymap_pin - 钉住映射页以便设置某一位
 *
 * 在可见性映射中设置某一位是两阶段操作。首先调用
 * visibilitymap_pin，钉住含有该堆页面对应位的可见性映射页。
 * 因为这可能需要 I/O 才能读入映射页，所以
 * 做这件事时不应持有堆页面上的锁。然后调用
 * visibilitymap_set 真正设置该位。
 *
 * 进入时，*vmbuf 应为 InvalidBuffer，或是同一关系上先前
 * 调用 visibilitymap_pin 或 visibilitymap_get_status 返回的有效缓冲区。
 * 返回时，*vmbuf 是含有 heapBlk 对应位的映射页的有效缓冲区。
 * 该缓冲区中的映射页覆盖 heapBlk 的那一位。
 *
 * 若该页在映射文件中尚不存在，则扩展文件。
 */
void
visibilitymap_pin(Relation rel, BlockNumber heapBlk, Buffer *vmbuf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);

	/* 若可能，复用原先钉住的缓冲区*/
	if (BufferIsValid(*vmbuf))
	{
		if (BufferGetBlockNumber(*vmbuf) == mapBlock)
			return;

		ReleaseBuffer(*vmbuf);
	}
	*vmbuf = vm_readbuf(rel, mapBlock, true);
}

/*
 *	visibilitymap_pin_ok - 正确的页面是否已经钉住？
 *
 * 进入时，vmbuf 应为 InvalidBuffer，或是同一关系上先前
 * 调用 visibilitymap_pin 或 visibilitymap_get_status 返回的有效缓冲区。
 * 返回值表明该缓冲区是否覆盖
 * 给定的 heapBlk。
 */
bool
visibilitymap_pin_ok(BlockNumber heapBlk, Buffer vmbuf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);

	return BufferIsValid(vmbuf) && BufferGetBlockNumber(vmbuf) == mapBlock;
}

/*
 *	visibilitymap_set - 在先前钉住的页面上设置 bit(s)
 *
 * 若正处于恢复，recptr 是正在重放的 XLOG 记录的 LSN，
 * 正常运行时则为 InvalidXLogRecPtr。可见性映射页的 LSN 被推进到
 * 所给的值；正常运行时我们生成一条新的 XLOG 记录，并把
 * 页面 LSN 设为该值（不过堆页面的 LSN 可能*不*更新；
 * 见下文）。cutoff_xid 是正在标为
 * 全可见的页面上最大的 xmin；热备要用它，并且可以为 InvalidTransactionId，
 * 若该页面不含元组。当已经全可见的页面要被标为全冻结时，
 * 也可以把它设为 InvalidTransactionId。
 *
 * 调用方应在调用本函数之前设置堆页面的 PD_ALL_VISIBLE 位。
 * 除恢复期间外，调用方还应传入堆
 * 缓冲区。启用校验和且不在恢复中时，必须把
 * 堆缓冲区加入 WAL 链，以免它被撕裂。
 *
 * 必须把含有正确映射页的缓冲区传给本函数。
 * 先调用 visibilitymap_pin 钉住正确的那一页。本函数不做
 * 任何 I/O。
 */
void
visibilitymap_set(Relation rel, BlockNumber heapBlk, Buffer heapBuf,
				  XLogRecPtr recptr, Buffer vmBuf, TransactionId cutoff_xid,
				  uint8 flags)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	uint32		mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	uint8		mapOffset = HEAPBLK_TO_OFFSET(heapBlk);
	Page		page;
	uint8	   *map;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_set %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	Assert(InRecovery || XLogRecPtrIsInvalid(recptr));
	Assert(InRecovery || PageIsAllVisible((Page) BufferGetPage(heapBuf)));
	Assert((flags & VISIBILITYMAP_VALID_BITS) == flags);

	/* 绝不能在未同时设置 all_visible 位时设置 all_frozen 位*/
	Assert(flags != VISIBILITYMAP_ALL_FROZEN);

	/* 若提供了堆页面，则检查钉住的是否为正确的那一页*/
	if (BufferIsValid(heapBuf) && BufferGetBlockNumber(heapBuf) != heapBlk)
		elog(ERROR, "wrong heap buffer passed to visibilitymap_set");

	/* 检查钉住的是否为正确的可见性映射页*/
	if (!BufferIsValid(vmBuf) || BufferGetBlockNumber(vmBuf) != mapBlock)
		elog(ERROR, "wrong VM buffer passed to visibilitymap_set");

	page = BufferGetPage(vmBuf);
	map = (uint8 *) PageGetContents(page);
	LockBuffer(vmBuf, BUFFER_LOCK_EXCLUSIVE);

	if (flags != (map[mapByte] >> mapOffset & VISIBILITYMAP_VALID_BITS))
	{
		START_CRIT_SECTION();

		map[mapByte] |= (flags << mapOffset);
		MarkBufferDirty(vmBuf);

		if (RelationNeedsWAL(rel))
		{
			if (XLogRecPtrIsInvalid(recptr))
			{
				Assert(!InRecovery);
				recptr = log_heap_visible(rel, heapBuf, vmBuf, cutoff_xid, flags);

				/*
				 * 若启用了数据校验和（或 wal_log_hints=on），
				 * 就需要保护堆页面不被撕裂。
				 *
				 * 否则就绝不能更新堆页面的 LSN。在
				 * 这种情况下，上面插入的 WAL 记录省略了该堆页面的 FPI，
				 * 因此更新堆页面的 LSN
				 * 是不正确的。
				 */
				if (XLogHintBitIsNeeded())
				{
					Page		heapPage = BufferGetPage(heapBuf);

					PageSetLSN(heapPage, recptr);
				}
			}
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}

	LockBuffer(vmBuf, BUFFER_LOCK_UNLOCK);
}

/*
 *	visibilitymap_get_status - 取得各位的状态
 *
 * 按可见性映射，heapBlk 上的所有元组是否对所有人可见，
 * 或者是否已标为冻结？
 *
 * 进入时，*vmbuf 应为 InvalidBuffer，或是同一关系上先前
 * 调用 visibilitymap_pin 或 visibilitymap_get_status 返回的有效缓冲区。
 * 返回时，*vmbuf 是含有 heapBlk 对应位的映射页的有效缓冲区，
 * 或为 InvalidBuffer。调用方负责在测试并设置各位之后
 * 释放 *vmbuf。
 *
 * 注意：本函数通常在不持有堆页面锁时调用，
 * 因此别人可能在我们查看之后立刻改掉该位。事实上，
 * 由于我们也不锁定可见性映射页，甚至可能
 * 别人在我们查看之前刚刚改过该位，而我们
 * 看到的仍是旧值。处理
 * 所有并发问题是调用方的责任！
 */
uint8
visibilitymap_get_status(Relation rel, BlockNumber heapBlk, Buffer *vmbuf)
{
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	uint32		mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	uint8		mapOffset = HEAPBLK_TO_OFFSET(heapBlk);
	char	   *map;
	uint8		result;

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_get_status %s %d", RelationGetRelationName(rel), heapBlk);
#endif

	/* 若可能，复用原先钉住的缓冲区*/
	if (BufferIsValid(*vmbuf))
	{
		if (BufferGetBlockNumber(*vmbuf) != mapBlock)
		{
			ReleaseBuffer(*vmbuf);
			*vmbuf = InvalidBuffer;
		}
	}

	if (!BufferIsValid(*vmbuf))
	{
		*vmbuf = vm_readbuf(rel, mapBlock, false);
		if (!BufferIsValid(*vmbuf))
			return false;
	}

	map = PageGetContents(BufferGetPage(*vmbuf));

	/*
	 * 单字节读取是原子的。这里可能有内存序效应，
	 * 但出于性能考虑，我们把操心这件事
	 * 留给调用方。
	 */
	result = ((map[mapByte] >> mapOffset) & VISIBILITYMAP_VALID_BITS);
	return result;
}

/*
 *	visibilitymap_count  - 统计可见性映射中已置位的个数
 *
 * 注意：表与本次调用并发扩展时，我们忽略竞态的可能。
 * 新加入表中的页面不会被标为全可见或全冻结，
 * 因此不会影响结果。
 */
void
visibilitymap_count(Relation rel, BlockNumber *all_visible, BlockNumber *all_frozen)
{
	BlockNumber mapBlock;
	BlockNumber nvisible = 0;
	BlockNumber nfrozen = 0;

	/* 必须指定 all_visible*/
	Assert(all_visible);

	for (mapBlock = 0;; mapBlock++)
	{
		Buffer		mapBuffer;
		uint64	   *map;
		int			i;

		/*
		 * 一直读到越过映射末尾。假定最后一页里多出来的
		 * 字节已被清零，因此我们不特意把它们
		 * 排除在计数之外。
		 */
		mapBuffer = vm_readbuf(rel, mapBlock, false);
		if (!BufferIsValid(mapBuffer))
			break;

		/*
		 * 我们选择不锁定该页，因为只要有人在并发设置或
		 * 清除这些位，结果反正会立刻过时，
		 * 而我们真正需要的只是一个近似值。
		 */
		map = (uint64 *) PageGetContents(BufferGetPage(mapBuffer));

		StaticAssertStmt(MAPSIZE % sizeof(uint64) == 0,
						 "unsupported MAPSIZE");
		if (all_frozen == NULL)
		{
			for (i = 0; i < MAPSIZE / sizeof(uint64); i++)
				nvisible += pg_popcount64(map[i] & VISIBLE_MASK64);
		}
		else
		{
			for (i = 0; i < MAPSIZE / sizeof(uint64); i++)
			{
				nvisible += pg_popcount64(map[i] & VISIBLE_MASK64);
				nfrozen += pg_popcount64(map[i] & FROZEN_MASK64);
			}
		}

		ReleaseBuffer(mapBuffer);
	}

	*all_visible = nvisible;
	if (all_frozen)
		*all_frozen = nfrozen;
}

/*
 *	visibilitymap_prepare_truncate -
 *			为截断可见性映射做准备
 *
 * nheapblocks 是堆的新大小。
 *
 * 返回新可见性映射的块数。
 * 若为 InvalidBlockNumber，就没有什么可截断的；
 * 否则调用方负责调用 smgrtruncate()
 * 来截断可见性映射页。
 */
BlockNumber
visibilitymap_prepare_truncate(Relation rel, BlockNumber nheapblocks)
{
	BlockNumber newnblocks;

	/* 所剩的最后一个块、字节与位*/
	BlockNumber truncBlock = HEAPBLK_TO_MAPBLOCK(nheapblocks);
	uint32		truncByte = HEAPBLK_TO_MAPBYTE(nheapblocks);
	uint8		truncOffset = HEAPBLK_TO_OFFSET(nheapblocks);

#ifdef TRACE_VISIBILITYMAP
	elog(DEBUG1, "vm_truncate %s %d", RelationGetRelationName(rel), nheapblocks);
#endif

	/*
	 * 若这个关系尚未创建可见性映射，就
	 * 没有什么可截断的。
	 */
	if (!smgrexists(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM))
		return InvalidBlockNumber;

	/*
	 * 除非新大小恰好落在可见性映射页的边界上，否则
	 * 最后剩下的映射页中代表已截断堆
	 * 块的尾部位需要清除。这不仅是为了整齐，而且有必要，
	 * 因为若堆再次被扩展，我们就没有机会
	 * 再去清除这些位。
	 */
	if (truncByte != 0 || truncOffset != 0)
	{
		Buffer		mapBuffer;
		Page		page;
		char	   *map;

		newnblocks = truncBlock + 1;

		mapBuffer = vm_readbuf(rel, truncBlock, false);
		if (!BufferIsValid(mapBuffer))
		{
			/* 无事可做，文件本来就已经更小*/
			return InvalidBlockNumber;
		}

		page = BufferGetPage(mapBuffer);
		map = PageGetContents(page);

		LockBuffer(mapBuffer, BUFFER_LOCK_EXCLUSIVE);

		/* 从这里直到变更记入日志之前，不要 EREPORT(ERROR)*/
		START_CRIT_SECTION();

		/* 清掉不需要的字节。*/
		MemSet(&map[truncByte + 1], 0, MAPSIZE - (truncByte + 1));

		/*----
		 * 屏蔽所剩最后一个字节中不需要的位。
		 *
		 * ((1 << 0) - 1) = 00000000
		 * ((1 << 1) - 1) = 00000001
		 * ...
		 * ((1 << 6) - 1) = 00111111
		 * ((1 << 7) - 1) = 01111111
		 *----
		 */
		map[truncByte] &= (1 << truncOffset) - 1;

		/*
		 * 关系截断在更高层记入 WAL，并且
		 * WAL 重放时会调用我们。但若启用了校验和，仍需要
		 * 再写一条 WAL 记录，以防撕裂页：若该
		 * 页在截断 WAL 记录之前被刷到磁盘。我们无法
		 * 在这里使用 MarkBufferDirtyHint，因为它在恢复期间
		 * 不会把该页标脏。
		 */
		MarkBufferDirty(mapBuffer);
		if (!InRecovery && RelationNeedsWAL(rel) && XLogHintBitIsNeeded())
			log_newpage_buffer(mapBuffer, false);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(mapBuffer);
	}
	else
		newnblocks = truncBlock;

	if (smgrnblocks(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM) <= newnblocks)
	{
		/* 无事可做，文件本来就已经小于所要求的大小*/
		return InvalidBlockNumber;
	}

	return newnblocks;
}

/*
 * 读取一张可见性映射页。
 *
 * 若该页不存在，则返回 InvalidBuffer；若 extend 为
 * 真，则扩展可见性映射文件。
 */
static Buffer
vm_readbuf(Relation rel, BlockNumber blkno, bool extend)
{
	Buffer		buf;
	SMgrRelation reln;

	/*
	 * 注意：若 relcache 项被关闭，复用这个 smgr 指针
	 * 可能失败。只要从这里到最后一次使用该指针之间
	 * 只做 smgr 层的操作，就是安全的。
	 */
	reln = RelationGetSmgr(rel);

	/*
	 * 若尚未缓存可见性映射分叉的大小，先检查
	 * 它。
	 */
	if (reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] == InvalidBlockNumber)
	{
		if (smgrexists(reln, VISIBILITYMAP_FORKNUM))
			smgrnblocks(reln, VISIBILITYMAP_FORKNUM);
		else
			reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = 0;
	}

	/*
	 * 读取时使用 ZERO_ON_ERROR 模式，必要时初始化该页。
	 * 清除位总是安全的，因此清掉
	 * 损坏页好过因此报错。
	 *
	 * 扩展关系时，下面用同一条路径来初始化页面，
	 * 因为并发扩展可能使 vm_extend()
	 * 返回一张已经初始化过的页。
	 */
	if (blkno >= reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM])
	{
		if (extend)
			buf = vm_extend(rel, blkno + 1);
		else
			return InvalidBuffer;
	}
	else
		buf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, blkno,
								 RBM_ZERO_ON_ERROR, NULL);

	/*
	 * 需要时才初始化该页，这比看上去更棘手，因为
	 * 可能有多个后端同时在做，而我们希望在页面
	 * 正常的常见路径上，不要无谓地拿缓冲区锁。
	 * 初始化该页必须拿锁，所以
	 * 拿到锁之后要重新检查它是否仍是新页，以免别人
	 * 已经初始化过。另外，最初检查 PageIsNew 时并没有拿
	 * 锁，因此可能跳过初始化、直接返回该缓冲区，而别人
	 * 仍在初始化该页（例如看到 pd_upper 已设置，
	 * 页头其余字段却仍是零）。自己会拿缓冲区锁的
	 * 调用方不受此影响，但有些调用方查看该页时
	 * 完全不上锁。后者只有在不依赖页头内容正确时
	 * 才成立。
	 * 当前用法是安全的，因为 PageGetContents() 并不需要页头内容正确。
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
 * 确保可见性映射分叉至少有 vm_nblocks 长，必要时用
 * 填零的页面扩展它。
 */
static Buffer
vm_extend(Relation rel, BlockNumber vm_nblocks)
{
	Buffer		buf;

	buf = ExtendBufferedRelTo(BMR_REL(rel), VISIBILITYMAP_FORKNUM, NULL,
							  EB_CREATE_FORK_IF_NEEDED |
							  EB_CLEAR_SIZE_CACHE,
							  vm_nblocks,
							  RBM_ZERO_ON_ERROR);

	/*
	 * 发送一条共享缓存失效消息，迫使其他后端关掉它们可能仍握着的、
	 * 指向本关系的 smgr 引用，因为我们即将改动它。
	 * 这是有用的优化，因为这样一来后端就不必
	 * 不断检查该文件是否被创建或扩展，而这种事
	 * 并不经常发生。
	 */
	CacheInvalidateSmgr(RelationGetSmgr(rel)->smgr_rlocator);

	return buf;
}
