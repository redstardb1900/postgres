/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES 堆访问方法的输入/输出代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/hio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"


/*
 * RelationPutHeapTuple - 把元组放到指定页面
 *
 * !!! 此处禁止 EREPORT(ERROR)（DISALLOWED）!!!  失败时必须 PANIC!!!
 *
 * 注意——调用方必须对缓冲区持有 BUFFER_LOCK_EXCLUSIVE。
 */
void
RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple,
					 bool token)
{
	Page		pageHeader;
	OffsetNumber offnum;

	/*
	 * 正在被推测性插入的元组应当已经设置好
	 * 它的令牌。
	 */
	Assert(!token || HeapTupleHeaderIsSpeculative(tuple->t_data));

	/*
	 * 不要允许把提示位组合无效的元组放到
	 * 页面上。contrib/amcheck 的逻辑会把这种组合检测为损坏，
	 * 因此若关掉这条断言，请在那里做
	 * 相应的修改。
	 */
	Assert(!((tuple->t_data->t_infomask & HEAP_XMAX_COMMITTED) &&
			 (tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI)));

	/* 把元组加入该页面*/
	pageHeader = BufferGetPage(buffer);

	offnum = PageAddItem(pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, false, true);

	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "failed to add tuple to page");

	/* 把 tuple->t_self 更新为它实际存放的位置*/
	ItemPointerSet(&(tuple->t_self), BufferGetBlockNumber(buffer), offnum);

	/*
	 * 也把正确位置写入所存元组的 CTID（除非
	 * 这是推测性插入，那种情况下令牌放在
	 * CTID 字段里）
	 */
	if (!token)
	{
		ItemId		itemId = PageGetItemId(pageHeader, offnum);
		HeapTupleHeader item = (HeapTupleHeader) PageGetItem(pageHeader, itemId);

		item->t_ctid = tuple->t_self;
	}
}

/*
 * 按 mode 读入一个缓冲区；若 bistate 不是 NULL，则使用批量插入策略。
 */
static Buffer
ReadBufferBI(Relation relation, BlockNumber targetBlock,
			 ReadBufferMode mode, BulkInsertState bistate)
{
	Buffer		buffer;

	/* 若不是批量插入，就与 ReadBuffer 完全一样*/
	if (!bistate)
		return ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								  mode, NULL);

	/* 若期望的块已经钉住，则重新钉住并返回它*/
	if (bistate->current_buf != InvalidBuffer)
	{
		if (BufferGetBlockNumber(bistate->current_buf) == targetBlock)
		{
			/*
			 * 目前 LOCK 变体只用于扩展
			 * 关系，不应当走到这个分支。
			 */
			Assert(mode != RBM_ZERO_AND_LOCK &&
				   mode != RBM_ZERO_AND_CLEANUP_LOCK);

			IncrBufferRefCount(bistate->current_buf);
			return bistate->current_buf;
		}
		/* ... 否则丢掉旧缓冲区*/
		ReleaseBuffer(bistate->current_buf);
		bistate->current_buf = InvalidBuffer;
	}

	/* 用缓冲区策略执行一次读取*/
	buffer = ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								mode, bistate->strategy);

	/* 把选中的块保存为以后插入的目标*/
	IncrBufferRefCount(buffer);
	bistate->current_buf = buffer;

	return buffer;
}

/*
 * 对每个全可见的堆页面，在相应的
 * 可见性映射页上取得钉住，若我们还没有的话。
 *
 * 为避免调用方变复杂，只涉及一个缓冲区时，buffer1 或 buffer2 都可以是
 * InvalidBuffer。出于同样的原因，block2
 * 可以小于 block1。
 *
 * 返回缓冲区锁是否曾被暂时释放。
 */
static bool
GetVisibilityMapPins(Relation relation, Buffer buffer1, Buffer buffer2,
					 BlockNumber block1, BlockNumber block2,
					 Buffer *vmbuffer1, Buffer *vmbuffer2)
{
	bool		need_to_pin_buffer1;
	bool		need_to_pin_buffer2;
	bool		released_locks = false;

	/*
	 * 交换缓冲区，以处理只有单个块/缓冲区的情形，并处理
	 * 锁顺序规则需要先锁 block2 的情形。
	 */
	if (!BufferIsValid(buffer1) ||
		(BufferIsValid(buffer2) && block1 > block2))
	{
		Buffer		tmpbuf = buffer1;
		Buffer	   *tmpvmbuf = vmbuffer1;
		BlockNumber tmpblock = block1;

		buffer1 = buffer2;
		vmbuffer1 = vmbuffer2;
		block1 = block2;

		buffer2 = tmpbuf;
		vmbuffer2 = tmpvmbuf;
		block2 = tmpblock;
	}

	Assert(BufferIsValid(buffer1));
	Assert(buffer2 == InvalidBuffer || block1 <= block2);

	while (1)
	{
		/* 弄清哪些钉住是我们需要但还没有的。*/
		need_to_pin_buffer1 = PageIsAllVisible(BufferGetPage(buffer1))
			&& !visibilitymap_pin_ok(block1, *vmbuffer1);
		need_to_pin_buffer2 = buffer2 != InvalidBuffer
			&& PageIsAllVisible(BufferGetPage(buffer2))
			&& !visibilitymap_pin_ok(block2, *vmbuffer2);
		if (!need_to_pin_buffer1 && !need_to_pin_buffer2)
			break;

		/* 做任何 I/O 之前必须先解锁两个缓冲区。*/
		released_locks = true;
		LockBuffer(buffer1, BUFFER_LOCK_UNLOCK);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_UNLOCK);

		/* 取得钉住。*/
		if (need_to_pin_buffer1)
			visibilitymap_pin(relation, block1, vmbuffer1);
		if (need_to_pin_buffer2)
			visibilitymap_pin(relation, block2, vmbuffer2);

		/* 重新锁定缓冲区。*/
		LockBuffer(buffer1, BUFFER_LOCK_EXCLUSIVE);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * 若涉及两个缓冲区而我们只钉住了其中一个，
		 * 可能在我们忙着钉住第一个时，第二个变成了全可见。
		 * 若看起来有这种
		 * 可能，就需要再走一遍这个循环。
		 */
		if (buffer2 == InvalidBuffer || buffer1 == buffer2
			|| (need_to_pin_buffer1 && need_to_pin_buffer2))
			break;
	}

	return released_locks;
}

/*
 * 扩展该关系。若有好处，就一次扩展多个页面。
 *
 * 若调用方需要多个页面（num_pages > 1），我们总是尝试至少扩展
 * 那么多。
 *
 * 若扩展锁上有争用，我们就不只是"为自己"
 * 扩展，而是设法帮助别人。做法是把空页面
 * 加入 FSM。通常在不能使用 FSM 时并没有争用。
 *
 * 扩展的页面数必须限制在某个值以内，因为
 * 所有被扩展页面的缓冲区都需要暂时被钉住。目前
 * 我们把 MAX_BUFFERS_TO_EXTEND_BY 定义为 64 个缓冲区，很难看出
 * 再大的数字有什么好处。部分原因是 copyfrom.c 的
 * MAX_BUFFERED_TUPLES / MAX_BUFFERED_BYTES 阻止了更大的 multi_inserts。
 *
 * 返回新扩展块的缓冲区。若可能，该缓冲区
 * 以排他锁返回。若锁不得不被释放，*did_unlock 设为真，
 * 否则为假。
 *
 *
 * XXX：对某些工作负载，更激进地扩展可能有好处，
 * 例如根据关系大小使用某种启发式。
 */
static Buffer
RelationAddBlocks(Relation relation, BulkInsertState bistate,
				  int num_pages, bool use_fsm, bool *did_unlock)
{
#define MAX_BUFFERS_TO_EXTEND_BY 64
	Buffer		victim_buffers[MAX_BUFFERS_TO_EXTEND_BY];
	BlockNumber first_block = InvalidBlockNumber;
	BlockNumber last_block = InvalidBlockNumber;
	uint32		extend_by_pages;
	uint32		not_in_fsm_pages;
	Buffer		buffer;
	Page		page;

	/*
	 * 确定尝试扩展多少个页面。
	 */
	if (bistate == NULL && !use_fsm)
	{
		/*
		 * 若既没有 bistate，也不能使用 FSM，就不能批量
		 * 扩展——否则无法找到那些额外的页面。
		 */
		extend_by_pages = 1;
	}
	else
	{
		uint32		waitcount;

		/*
		 * 至少按调用方需要的页面数来扩展。我们
		 * 可以记住额外的页面（经由 FSM 或 bistate）。
		 */
		extend_by_pages = num_pages;

		if (!RELATION_IS_LOCAL(relation))
			waitcount = RelationExtensionLockWaiterCount(relation);
		else
			waitcount = 0;

		/*
		 * 把要扩展的页面数乘以等待者的数量。即便
		 * 没有使用 FSM 也这样做，因为它仍能缓解
		 * 争用：推迟本后端下一次需要
		 * 扩展的时刻。那种情况下，扩展出来的页面经由
		 * bistate->next_free 找到。
		 */
		extend_by_pages += extend_by_pages * waitcount;

		/* ---
		 * 若先前用同一个 bistate 扩展过，很有可能
		 * 还会再扩展一些。尝试按与上次相同的页面数
		 * 来扩展。这对性能可能很重要，原因有几条，
		 * 包括：
		 *
		 * - 这可以避免 mdzeroextend() 在不同的扩展
		 *   关系的方式之间来回切换，对某些
		 *   文件系统来说那样很低效。
		 *
		 * - 争用常常是间歇的。即便当前看不到
		 *   其他等待者（见上），一次扩展更多
		 *   也能防止以后的争用。
		 * ---
		 */
		if (bistate)
			extend_by_pages = Max(extend_by_pages, bistate->already_extended_by);

		/*
		 * 扩展不能超过 MAX_BUFFERS_TO_EXTEND_BY，因为需要同时
		 * 钉住它们全部。
		 */
		extend_by_pages = Min(extend_by_pages, MAX_BUFFERS_TO_EXTEND_BY);
	}

	/*
	 * 扩展出来的页面中有多少应当记入 FSM？
	 *
	 * 若有 bistate，只把我们自己不需要的页面记入
	 * FSM。否则其他每个后端都会立刻试图
	 * 使用本后端自己需要的页面，造成不必要的
	 * 争用。若没有 bistate，就无法避开 FSM。
	 *
	 * 绝不要把即将返回的那一页记入 FSM，我们马上就要用它。
	 */
	if (num_pages > 1 && bistate == NULL)
		not_in_fsm_pages = 1;
	else
		not_in_fsm_pages = num_pages;

	/* 准备把另一个缓冲区放进 bistate*/
	if (bistate && bistate->current_buf != InvalidBuffer)
	{
		ReleaseBuffer(bistate->current_buf);
		bistate->current_buf = InvalidBuffer;
	}

	/*
	 * 扩展该关系。我们要求返回的第一页被锁定，
	 * 以便确定没有人并发地向该页
	 * 插入。
	 *
	 * 以当前的 MAX_BUFFERS_TO_EXTEND_BY，不必担心
	 * [自动]清理去截断后面的页，因为 REL_TRUNCATE_MINIMUM
	 * 要大得多。
	 */
	first_block = ExtendBufferedRelBy(BMR_REL(relation), MAIN_FORKNUM,
									  bistate ? bistate->strategy : NULL,
									  EB_LOCK_FIRST,
									  extend_by_pages,
									  victim_buffers,
									  &extend_by_pages);
	buffer = victim_buffers[0]; /* 本函数将要返回的缓冲区*/
	last_block = first_block + (extend_by_pages - 1);
	Assert(first_block == BufferGetBlockNumber(buffer));

	/*
	 * 关系现已扩展。初始化该页。我们在这里、在
	 * 可能释放该页的锁之前做这件事，因为这样可以
	 * 再次确认页面内容为空（这不应当
	 * 发生，但若发生了，我们不想冒抹掉有效数据的风险）。
	 */
	page = BufferGetPage(buffer);
	if (!PageIsNew(page))
		elog(ERROR, "page %u of relation \"%s\" should be empty but is not",
			 first_block,
			 RelationGetRelationName(relation));

	PageInit(page, BufferGetPageSize(buffer), 0);
	MarkBufferDirty(buffer);

	/*
	 * 若决定把页面放入 FSM，就释放缓冲区锁（但
	 * 不解除钉住），我们不想在持有缓冲区锁时做 IO。这会
	 * 使调用方必须做稍多一些检查。
	 */
	if (use_fsm && not_in_fsm_pages < extend_by_pages)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		*did_unlock = true;
	}
	else
		*did_unlock = false;

	/*
	 * 关系现已扩展。释放除第一个以外所有缓冲区上的钉住
	 * （第一个将返回）。若决定把页面放入 FSM，
	 * 可以在同一个循环里做。
	 */
	for (uint32 i = 1; i < extend_by_pages; i++)
	{
		BlockNumber curBlock = first_block + i;

		Assert(curBlock == BufferGetBlockNumber(victim_buffers[i]));
		Assert(BlockNumberIsValid(curBlock));

		ReleaseBuffer(victim_buffers[i]);

		if (use_fsm && i >= not_in_fsm_pages)
		{
			Size		freespace = BufferGetPageSize(victim_buffers[i]) -
				SizeOfPageHeaderData;

			RecordPageWithFreeSpace(relation, curBlock, freespace);
		}
	}

	if (use_fsm && not_in_fsm_pages < extend_by_pages)
	{
		BlockNumber first_fsm_block = first_block + not_in_fsm_pages;

		FreeSpaceMapVacuumRange(relation, first_fsm_block, last_block);
	}

	if (bistate)
	{
		/*
		 * 记住我们额外扩展出来的页面，以便以后使用它们时
		 * 不必查看 FSM。
		 */
		if (extend_by_pages > 1)
		{
			bistate->next_free = first_block + 1;
			bistate->last_free = last_block;
		}
		else
		{
			bistate->next_free = InvalidBlockNumber;
			bistate->last_free = InvalidBlockNumber;
		}

		/* 维护 bistate->current_buf*/
		IncrBufferRefCount(buffer);
		bistate->current_buf = buffer;
		bistate->already_extended_by += extend_by_pages;
	}

	return buffer;
#undef MAX_BUFFERS_TO_EXTEND_BY
}

/*
 * RelationGetBufferForTuple
 *
 *	返回给定关系中某一页面的已钉住且排他锁定的缓冲区，
 *	其空闲空间 >= 给定的 len。
 *
 *	若 num_pages > 1，当我们决定扩展关系时，会尝试至少扩展
 *	那么多个页面。对知道自己将需要多个页面的调用方，
 *	这更高效
 *	（例如 heap_multi_insert()）。
 *
 *	若 otherBuffer 不是 InvalidBuffer，则它引用同一关系中
 *	另一页上先前钉住的缓冲区；返回时，该
 *	缓冲区也会被排他锁定。（heap_update 使用这种情形；
 *	otherBuffer 含有正在被更新的元组。）
 *
 *	传入 otherBuffer 的原因是：若两个后端在做
 *	并发的 heap_update，而它们试图
 *	以相反顺序锁定相同的两个缓冲区，就可能死锁。为确保这
 *	不会发生，我们规定一个关系的缓冲区必须按
 *	页号递增的顺序锁定。最方便的做法是
 *	让 RelationGetBufferForTuple 把两个都锁上，并适当注意
 *	顺序。
 *
 *	注意：otherBuffer 恰好就是我们为插入新元组选中的
 *	同一个缓冲区，不太可能，但并非完全不可能（只有在
 *	heap_update 发现那里空间不够之后，该页又释放出空间时才会
 *	发生）。那种情况下，该页只会被钉住并锁定一次。
 *
 *	我们也处理全可见标志需要在
 *	其中一个或两个页面上被清除的可能。若是这样，必须在取得缓冲区
 *	lock(s) 之前先取得关联可见性映射页上的钉住，以免可能
 *	在持有缓冲区锁时做 I/O。这些钉住经由输入输出参数 vmbuffer 与
 *	vmbuffer_other 交还给调用方。
 *	注意，有些情况下调用方可能已经取得了这些钉住，
 *	其标志是进入时这些参数不是 InvalidBuffer。
 *
 *	我们通常用 FSM 帮助寻找空闲空间。但是，
 *	若指定了 HEAP_INSERT_SKIP_FSM，而元组放不进当前目标页，
 *	我们就只是在关系末尾追加一个新的空页。
 *	当我们知道该关系是新建的、并不含有
 *	可观的空闲空间时，这能省下一些开销。
 *
 *	HEAP_INSERT_SKIP_FSM 对关系上不记 WAL 的追加也有用，
 *	只要调用方持有排他锁，并在第一次插入前小心地使
 *	关系的 smgr_targblock 失效——这保证
 *	所有插入都发生在新加入的页面上，不会与
 *	其他事务的元组混在一起。这样，崩溃就不会有丢失
 *	其他事务已提交数据的风险。（见 heap_insert 的注释，
 *	其中有安全使用这一行为所需的额外约束。）
 *
 *	调用方还可以提供一个 BulkInsertState 对象，以优化对同一关系的
 *	多次插入。它保持对当前插入目标页的钉住
 *	（以省去钉住/解除钉住的往返），并向缓冲区管理器传入一个
 *	BULKWRITE 缓冲区选择策略对象。
 *	为 bistate 传入 NULL 则选择默认行为。
 *
 *	除了几乎为空的页面上的大元组，我们不会把现有页面填得超过 fillfactor。
 *	这没有问题，因为更新元组并把它留在同一页时不会
 *	来问本例程，而那正是 fillfactor 打算为之预留空间的
 *	场景。
 *
 *	这里允许 ereport(ERROR)，因此本例程*必须*在
 *	缓冲池中做出任何（尚未记入日志的）变更之前被调用。
 */
Buffer
RelationGetBufferForTuple(Relation relation, Size len,
						  Buffer otherBuffer, int options,
						  BulkInsertState bistate,
						  Buffer *vmbuffer, Buffer *vmbuffer_other,
						  int num_pages)
{
	bool		use_fsm = !(options & HEAP_INSERT_SKIP_FSM);
	Buffer		buffer = InvalidBuffer;
	Page		page;
	Size		nearlyEmptyFreeSpace,
				pageFreeSpace = 0,
				saveFreeSpace = 0,
				targetFreeSpace = 0;
	BlockNumber targetBlock,
				otherBlock;
	bool		unlockedTargetBuffer;
	bool		recheckVmPins;

	len = MAXALIGN(len);		/* 偏保守*/

	/* 若调用方不知道要扩展多少页，就扩展 1 页*/
	if (num_pages <= 0)
		num_pages = 1;

	/* 批量插入不支持更新，只支持插入。*/
	Assert(otherBuffer == InvalidBuffer || !bistate);

	/*
	 * 若会因元组过大而失败，就立刻失败
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						len, MaxHeapTupleSize)));

	/* 按 fillfactor 选项计算期望的额外空闲空间*/
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/*
	 * 没有元组的页面仍可能有行指针，因此当不可用空间很少时，
	 * 我们把页面视为"空"。这个阈值
	 * 有些随意，但它应当能在向低 fillfactor 的表插入大元组时，
	 * 防止大多数不必要的关系扩展。
	 */
	nearlyEmptyFreeSpace = MaxHeapTupleSize -
		(MaxHeapTuplesPerPage / 8 * sizeof(ItemIdData));
	if (len + saveFreeSpace > nearlyEmptyFreeSpace)
		targetFreeSpace = Max(len, nearlyEmptyFreeSpace);
	else
		targetFreeSpace = len + saveFreeSpace;

	if (otherBuffer != InvalidBuffer)
		otherBlock = BufferGetBlockNumber(otherBuffer);
	else
		otherBlock = InvalidBlockNumber;	/* 只是为了让编译器安静*/

	/*
	 * 我们先尝试把元组放到上次插入元组的同一页上，
	 * 该页缓存在 BulkInsertState 或 relcache 项里。若那
	 * 不行，就请空闲空间映射（FSM）找一个合适的页面。
	 * 由于 FSM 的信息可能过时，我们必须准备
	 * 循环并多次重试。（为确保这不是无限
	 * 循环，必须用正确的空闲空间量更新 FSM，针对
	 * 每一页被证明不合适的页面。）若 FSM 没有记下
	 * 空闲空间足够的页面，我们就放弃并扩展关系。
	 *
	 * 当 use_fsm 为假时，我们要么把元组放到现有目标
	 * 页上，要么扩展关系。
	 */
	if (bistate && bistate->current_buf != InvalidBuffer)
		targetBlock = BufferGetBlockNumber(bistate->current_buf);
	else
		targetBlock = RelationGetTargetBlock(relation);

	if (targetBlock == InvalidBlockNumber && use_fsm)
	{
		/*
		 * 没有缓存的目标页，因此向 FSM 要一个初始
		 * 目标。
		 */
		targetBlock = GetPageWithFreeSpace(relation, targetFreeSpace);
	}

	/*
	 * 若 FSM 对该关系一无所知，在放弃并扩展之前先试最后一页。
	 * 这可以避免引导期间或刚启动的系统里
	 * 出现每页一元组的症状。
	 */
	if (targetBlock == InvalidBlockNumber)
	{
		BlockNumber nblocks = RelationGetNumberOfBlocks(relation);

		if (nblocks > 0)
			targetBlock = nblocks - 1;
	}

loop:
	while (targetBlock != InvalidBlockNumber)
	{
		/*
		 * 读取并排他锁定目标块，以及给出的另一个
		 * 块（若有），并适当注意锁顺序，以及
		 * 它们可能是同一个块。
		 *
		 * 若页面级全可见标志已置位，调用方将需要
		 * 同时清除它和对应的可见性映射位。然而，
		 * 到我们返回时，缓冲区已经排他锁定，而我们不想
		 * 在那种状态下做任何 I/O。因此在取得锁之前
		 * 先在这里检查该位，若看起来有必要就钉住该页。
		 * 不加锁的检查有得到错误
		 * 答案的风险，因此取得锁之后必须再检查一次。
		 */
		if (otherBuffer == InvalidBuffer)
		{
			/* 简单情形*/
			buffer = ReadBufferBI(relation, targetBlock, RBM_NORMAL, bistate);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);

			/*
			 * 若该页为空，钉住 vmbuffer，以便稍后设置 all_frozen 位。
			 */
			if ((options & HEAP_INSERT_FROZEN) &&
				(PageGetMaxOffsetNumber(BufferGetPage(buffer)) == 0))
				visibilitymap_pin(relation, targetBlock, vmbuffer);

			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock == targetBlock)
		{
			/* 同样是简单情形*/
			buffer = otherBuffer;
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock < targetBlock)
		{
			/* 先锁另一个缓冲区*/
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else
		{
			/* 先锁目标缓冲区*/
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * 现在目标页（以及另一个缓冲区，若有）已经钉住
		 * 并锁定。然而，由于最初的 PageIsAllVisible 检查
		 * 是在取得锁之前做的，结果现在可能已经
		 * 过时，无论是选中的牺牲缓冲区，还是
		 * 调用方传入的另一个缓冲区。那种情况下，我们需要
		 * 放弃自己的锁，去取得先前没拿到的 pin(s)，然后
		 * 重新锁定。这相当痛苦，但希望不会
		 * 经常发生。
		 *
		 * 注意，有一种小可能：上面并没有钉住该页，
		 * 但正确的页反正已经钉住，要么因为
		 * 我们已经走过这个循环的前一轮，要么因为
		 * 调用方反正传入了正确的页。
		 *
		 * 还要注意，等到我们取得钉住并
		 * 重新拿到缓冲区锁时，可见性映射位可能已经被
		 * 别的后端清除了。那种情况下，我们会
		 * 多做一点没有收益的工作，但并没有造成
		 * 真正的害处。
		 */
		GetVisibilityMapPins(relation, buffer, otherBuffer,
							 targetBlock, otherBlock, vmbuffer,
							 vmbuffer_other);

		/*
		 * 现在可以检查这里是否有足够的空闲空间。若有，
		 * 就完成了。
		 */
		page = BufferGetPage(buffer);

		/*
		 * 必要时初始化页面，它很快就会被用到。我们本可以避免
		 * 在这里把缓冲区标脏，而指望调用方在
		 * 把元组放到该页上时再做，但似乎这么做
		 * 好处不大。
		 */
		if (PageIsNew(page))
		{
			PageInit(page, BufferGetPageSize(buffer), 0);
			MarkBufferDirty(buffer);
		}

		pageFreeSpace = PageGetHeapFreeSpace(page);
		if (targetFreeSpace <= pageFreeSpace)
		{
			/* 也把这一页用作以后的插入目标*/
			RelationSetTargetBlock(relation, targetBlock);
			return buffer;
		}

		/*
		 * 空间不够，因此必须放弃页面锁和钉住（若
		 * 有的话），并准备到别处去找。我们不在乎
		 * 按什么顺序解锁这两个缓冲区，因此这可以比上面的
		 * 代码稍简单一些。
		 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (otherBuffer == InvalidBuffer)
			ReleaseBuffer(buffer);
		else if (otherBlock != targetBlock)
		{
			LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
		}

		/* 是否有正在进行的批量扩展？*/
		if (bistate && bistate->next_free != InvalidBlockNumber)
		{
			Assert(bistate->next_free <= bistate->last_free);

			/*
			 * 我们先前批量扩展过该关系，并且那次扩展还有一些
			 * 未使用的页面，因此不必到
			 * FSM 里找新页。但要记下最后一页的空闲空间，
			 * 以后可能有人插入更窄的元组。
			 */
			if (use_fsm)
				RecordPageWithFreeSpace(relation, targetBlock, pageFreeSpace);

			targetBlock = bistate->next_free;
			if (bistate->next_free >= bistate->last_free)
			{
				bistate->next_free = InvalidBlockNumber;
				bistate->last_free = InvalidBlockNumber;
			}
			else
				bistate->next_free++;
		}
		else if (!use_fsm)
		{
			/* 没有 FSM 时，总是跳出循环并扩展*/
			break;
		}
		else
		{
			/*
			 * 按本页的状况更新 FSM，并再要一个
			 * 页面来试。
			 */
			targetBlock = RecordAndGetPageWithFreeSpace(relation,
														targetBlock,
														pageFreeSpace,
														targetFreeSpace);
		}
	}

	/* 必须扩展该关系*/
	buffer = RelationAddBlocks(relation, bistate, num_pages, use_fsm,
							   &unlockedTargetBuffer);

	targetBlock = BufferGetBlockNumber(buffer);
	page = BufferGetPage(buffer);

	/*
	 * 该页为空，钉住 vmbuffer 以设置 all_frozen 位。我们不想在
	 * 缓冲区被锁定时做 IO，因此若需要 IO，就先解锁该页
	 * （因而下面还要再检查）。
	 */
	if (options & HEAP_INSERT_FROZEN)
	{
		Assert(PageGetMaxOffsetNumber(page) == 0);

		if (!visibilitymap_pin_ok(targetBlock, *vmbuffer))
		{
			if (!unlockedTargetBuffer)
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			unlockedTargetBuffer = true;
			visibilitymap_pin(relation, targetBlock, vmbuffer);
		}
	}

	/*
	 * 必要时重新获取锁。
	 *
	 * 若目标缓冲区在上面被解锁了，或者在下面
	 * 重新获取 otherBuffer 上的锁时被解锁，虽然不太可能，但有可能
	 * 另一个后端已经用掉了本页上的空间。我们在下面检查这一点，
	 * 并在必要时重试。
	 */
	recheckVmPins = false;
	if (unlockedTargetBuffer)
	{
		/* 上面已经释放了目标缓冲区上的锁*/
		if (otherBuffer != InvalidBuffer)
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		recheckVmPins = true;
	}
	else if (otherBuffer != InvalidBuffer)
	{
		/*
		 * 我们没有释放目标缓冲区，并且 otherBuffer 有效，
		 * 需要锁定另一个缓冲区。它保证具有比
		 * 新页更小的页号。为符合死锁
		 * 预防规则，我们应当先锁 otherBuffer，但那样会
		 * 给其他后端把元组放到我们这一页上的机会。为降低
		 * 这种可能，尝试有条件地锁定另一个缓冲区，
		 * 这非常可能成功。
		 *
		 * 另一种做法是在扩展关系之前就取得 otherBuffer 上的锁，
		 * 但那就需要在执行 IO 期间一直持有该锁，
		 * 这似乎比一次不太可能的重试更糟。
		 */
		Assert(otherBuffer != buffer);
		Assert(targetBlock > otherBlock);

		if (unlikely(!ConditionalLockBuffer(otherBuffer)))
		{
			unlockedTargetBuffer = true;
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		recheckVmPins = true;
	}

	/*
	 * 若其中一个缓冲区被解锁了（otherBuffer 有效时总是如此），
	 * 虽然不太可能，但有可能全可见标志
	 * 已被置位。可以用 GetVisibilityMapPins 来处理。
	 * GetVisibilityMapPins() 可能需要暂时释放
	 * 缓冲区锁，那种情况下我们需要检查该页上是否仍有足够的
	 * 空间，见下文。
	 */
	if (recheckVmPins)
	{
		if (GetVisibilityMapPins(relation, otherBuffer, buffer,
								 otherBlock, targetBlock, vmbuffer_other,
								 vmbuffer))
			unlockedTargetBuffer = true;
	}

	/*
	 * 若自关系扩展以来目标缓冲区曾被暂时解锁，
	 * 虽然不太可能，但该页上的全部空间
	 * 可能已经被用掉。若是这样，就从头重试。若我们
	 * 没有解锁，而空间却不够，那就出了问题——
	 * 顶部的检查本应防止走到这种情形。
	 */
	pageFreeSpace = PageGetHeapFreeSpace(page);
	if (len > pageFreeSpace)
	{
		if (unlockedTargetBuffer)
		{
			if (otherBuffer != InvalidBuffer)
				LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			UnlockReleaseBuffer(buffer);

			goto loop;
		}
		elog(PANIC, "tuple is too big: size %zu", len);
	}

	/*
	 * 把新页记为我们以后插入的目标。
	 *
	 * XXX 应当立刻把新页记入空闲空间映射，
	 * 还是短期内只留给本后端独占使用
	 * （直到清理（VACUUM）看到它）？这似乎取决于你是否预期
	 * 当前后端还会做更多插入，而这在多数时候
	 * 大概是个不错的赌注。因此目前先不要把它加入 FSM。
	 */
	RelationSetTargetBlock(relation, targetBlock);

	return buffer;
}
