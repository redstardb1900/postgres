/*-------------------------------------------------------------------------
 *
 * nbtsearch.c
 *	  Postgres B-树的搜索代码。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtsearch.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


static void _bt_drop_lock_and_maybe_pin(IndexScanDesc scan, BTScanPos sp);
static OffsetNumber _bt_binsrch(Relation rel, BTScanInsert key, Buffer buf);
static int	_bt_binsrch_posting(BTScanInsert key, Page page,
								OffsetNumber offnum);
static bool _bt_readpage(IndexScanDesc scan, ScanDirection dir,
						 OffsetNumber offnum);
static void _bt_saveitem(BTScanOpaque so, int itemIndex,
						 OffsetNumber offnum, IndexTuple itup);
static int	_bt_setuppostingitems(BTScanOpaque so, int itemIndex,
								  OffsetNumber offnum, ItemPointer heapTid,
								  IndexTuple itup);
static inline void _bt_savepostingitem(BTScanOpaque so, int itemIndex,
									   OffsetNumber offnum,
									   ItemPointer heapTid, int tupleOffset);
static bool _bt_steppage(IndexScanDesc scan, ScanDirection dir);
static bool _bt_readnextpage(IndexScanDesc scan, BlockNumber blkno, ScanDirection dir);
static bool _bt_parallel_readpage(IndexScanDesc scan, BlockNumber blkno,
								  ScanDirection dir);
static Buffer _bt_walk_left(Relation rel, Buffer buf, Snapshot snapshot);
static bool _bt_endpoint(IndexScanDesc scan, ScanDirection dir);
static inline void _bt_initialize_more_data(BTScanOpaque so, ScanDirection dir);


/*
 *	_bt_drop_lock_and_maybe_pin()
 *
 * 解锁缓冲区；若可以安全释放钉住，也一并释放。
 * 这样可避免清理在游标停在某一页上、试图读取该页时
 * 陷入阻塞。
 *
 * 参见 nbtree/README 中使并发 TID 回收安全的章节。
 */
static void
_bt_drop_lock_and_maybe_pin(IndexScanDesc scan, BTScanPos sp)
{
	_bt_unlockbuf(scan->indexRelation, sp->buf);

	if (IsMVCCSnapshot(scan->xs_snapshot) &&
		RelationNeedsWAL(scan->indexRelation) &&
		!scan->xs_want_itup)
	{
		ReleaseBuffer(sp->buf);
		sp->buf = InvalidBuffer;
	}
}

/*
 *	_bt_search() -- 在树中搜索某个特定扫描键，
 *		更准确地说，是搜索它可能位于的第一张叶子页。
 *
 * 传入的扫描键是插入型扫描键（见 nbtree/README），
 * 但可以省略索引最右侧的 column(s)。
 *
 * 返回值是父页指针栈（也就是说，其中没有叶子层/叶子页
 * 的项）。*bufP 被设为叶子页缓冲区的地址，
 * 该缓冲区已加锁并钉住。不过，父页上并不持有
 * 任何锁！
 *
 * 若 snapshot 参数不是 NULL，沿树下降时会进行
 * "旧快照"检查。插入或删除定位时不需要这一步，
 * 因此那些情形传入 NULL。
 *
 * 返回的缓冲区按 access 参数加锁。此外，
 * access = BT_WRITE 时允许创建并返回一张空的根页。
 * access = BT_READ 且索引为空时，*bufP 被设为
 * InvalidBuffer。另外，在 BT_WRITE 模式下，搜索途中遇到的
 * 任何未完成页分裂都会被完成。
 *
 * 传入 access = BT_WRITE 的调用者必须提供 heaprel，因为我们
 * 可能需要为调用者分配一张新的根页——见 _bt_allocbuf。
 */
BTStack
_bt_search(Relation rel, Relation heaprel, BTScanInsert key, Buffer *bufP,
		   int access, Snapshot snapshot)
{
	BTStack		stack_in = NULL;
	int			page_access = BT_READ;

	/* 只要可能走到 _bt_allocbuf，就必须设置 heaprel */
	Assert(access == BT_READ || access == BT_WRITE);
	Assert(access == BT_READ || heaprel != NULL);

	/* 先取根页作为起点 */
	*bufP = _bt_getroot(rel, heaprel, access);

	/* 若索引为空且 access = BT_READ，则不创建根页。 */
	if (!BufferIsValid(*bufP))
		return (BTStack) NULL;

	/* 循环每下降一层迭代一次 */
	for (;;)
	{
		Page		page;
		BTPageOpaque opaque;
		OffsetNumber offnum;
		ItemId		itemid;
		IndexTuple	itup;
		BlockNumber child;
		BTStack		new_stack;

		/*
		 * 竞态——我们刚取到的页，自读到它在父页
		 * （或元数据页）中的下行之后，可能已经分裂。若已分裂，我们可能
		 * 需要向右走到它的新兄弟。照此处理。
		 *
		 * 写模式下，允许 _bt_moveright 顺路完成遇到的任何未完成页分裂。
		 * 严格说来，我们只需完成
		 * 即将插入的那张叶子页上的未完成页分裂，而不必处理
		 * 更高各层（带未完成页分裂的内部页
		 * 也会在 _bt_getstackbuf 中处理）。不过这正好是
		 * 一并完成内部页分裂的好机会。
		 */
		*bufP = _bt_moveright(rel, heaprel, key, *bufP, (access == BT_WRITE),
							  stack_in, page_access, snapshot);

		/* 若这是叶子页，则已完成 */
		page = BufferGetPage(*bufP);
		opaque = BTPageGetOpaque(page);
		if (P_ISLEAF(opaque))
			break;

		/*
		 * 在本页上找到合适的枢轴元组。它的下行指向
		 * 我们即将下降到的子页。
		 */
		offnum = _bt_binsrch(rel, key, *bufP);
		itemid = PageGetItemId(page, offnum);
		itup = (IndexTuple) PageGetItem(page, itemid);
		Assert(BTreeTupleIsPivot(itup) || !key->heapkeyspace);
		child = BTreeTupleGetDownLink(itup);

		/*
		 * 需要把所选枢轴元组的位置存入本页/本层的
		 * 新栈项。若调用者最终分裂下一层的
		 * 某一页，通常会在此处记下的位置之后立即插入新的枢轴
		 * 元组/下行。
		 */
		new_stack = (BTStack) palloc(sizeof(BTStackData));
		new_stack->bts_blkno = BufferGetBlockNumber(*bufP);
		new_stack->bts_offset = offnum;
		new_stack->bts_parent = stack_in;

		/*
		 * 第 1 层是叶子之上最低的非叶子层。因此，若
		 * 我们位于第 1 层，并且要求以写模式锁定叶子页，
		 * 则下一页也用写模式锁定，因为它必定是叶子页。
		 */
		if (opaque->btpo_level == 1 && access == BT_WRITE)
			page_access = BT_WRITE;

		/* 放下本页的读锁，再获取其子页上的锁 */
		*bufP = _bt_relandgetbuf(rel, *bufP, child, page_access);

		/* 好，可以下降一层了 */
		stack_in = new_stack;
	}

	/*
	 * 若要求以写模式锁定叶子页，但尚未做到，则
	 * 重新加锁。这只应发生在根页本身就是叶子页时（并且
	 * 它是索引中除元数据页之外的唯一一页）。
	 */
	if (access == BT_WRITE && page_access == BT_READ)
	{
		/* 把读锁换成写锁 */
		_bt_unlockbuf(rel, *bufP);
		_bt_lockbuf(rel, *bufP, BT_WRITE);

		/*
		 * 竞态——叶子页可能在我们放下读锁之后、
		 * 取得写锁之前已经分裂。若已分裂，我们可能需要
		 * 向右走到它的新兄弟。照此处理。
		 */
		*bufP = _bt_moveright(rel, heaprel, key, *bufP, true, stack_in, BT_WRITE,
							  snapshot);
	}

	return stack_in;
}

/*
 *	_bt_moveright() -- 必要时在 B-树中向右移动。
 *
 * 当我们跟随指针到达某一页时，该页有可能
 * 在此期间已经改变。若发生这种情况，可以
 * 保证该页已经"向右分裂"——也就是说，原先出现在
 * 该页上的任何数据，要么仍在该页上，
 * 要么严格位于它的右侧。
 *
 * 本例程通过检查页上的高键项，决定是否需要在树中
 * 向右移动。若该项
 * 严格小于扫描键，或者在 key.nextkey=true 时
 * <= 扫描键，则我们跟错了链接，需要
 * 向右移动。
 *
 * 传入的插入型扫描键可以省略索引最右侧的若干列。
 * （见 nbtree/README）
 *
 * 当 key.nextkey 为假（通常情形）时，我们寻找第一个
 * >= key 的项。当 key.nextkey 为真时，我们寻找第一个
 * 严格大于 key 的项。
 *
 * 若 forupdate 为真，我们会尝试完成遇到的任何未完成页分裂。
 * 为插入而锁定目标页时这是必需的，因为在页分裂
 * 完成之前，不允许向该页插入。仅当 forupdate 为真时才使用
 * 'heaprel' 与 'stack'。
 *
 * 进入时，缓冲区已钉住，并持有 'access' 所指定类型的锁。
 * 若向右移动，我们释放该缓冲区及其锁，并在右兄弟上
 * 取得同样的钉住与锁。返回值是我们停下来的那个缓冲区。
 *
 * 若 snapshot 参数不是 NULL，沿树下降时会进行
 * "旧快照"检查。插入或删除定位时不需要这一步，
 * 因此那些情形传入 NULL。
 */
Buffer
_bt_moveright(Relation rel,
			  Relation heaprel,
			  BTScanInsert key,
			  Buffer buf,
			  bool forupdate,
			  BTStack stack,
			  int access,
			  Snapshot snapshot)
{
	Page		page;
	BTPageOpaque opaque;
	int32		cmpval;

	Assert(!forupdate || heaprel != NULL);

	/*
	 * 当 nextkey = false（通常情形）时：若把我们带到
	 * 本页的扫描键 > 页上存储的高键，则该页已经分裂，
	 * 我们需要向右移动。（经 pg_upgrade 升级的 !heapkeyspace 索引
	 * 右侧和左侧都可能有一些重复项，但那只在叶子层、
	 * 并且只在 _bt_search 已经找到初始叶子页之后
	 * 才会处理。）
	 *
	 * 当 nextkey = true 时：若扫描键 >= 页的高键，则向右移动。
	 * （注意：此情形下无法设置 key.scantid。）
	 *
	 * 该页甚至可能分裂了不止一次，因此按需要
	 * 继续扫描。
	 *
	 * 若我们跟随的链接把我们带到一张
	 * 死页，也必须向右移动。
	 */
	cmpval = key->nextkey ? 0 : 1;

	for (;;)
	{
		page = BufferGetPage(buf);
		TestForOldSnapshot(snapshot, rel, page);
		opaque = BTPageGetOpaque(page);

		if (P_RIGHTMOST(opaque))
			break;

		/*
		 * 顺路完成遇到的任何未完成页分裂。
		 */
		if (forupdate && P_INCOMPLETE_SPLIT(opaque))
		{
			BlockNumber blkno = BufferGetBlockNumber(buf);

			/* 必要时升级我们的锁 */
			if (access == BT_READ)
			{
				_bt_unlockbuf(rel, buf);
				_bt_lockbuf(rel, buf, BT_WRITE);
			}

			if (P_INCOMPLETE_SPLIT(opaque))
				_bt_finish_split(rel, heaprel, buf, stack);
			else
				_bt_relbuf(rel, buf);

			/* 以正确模式重新获取锁，并再次检查 */
			buf = _bt_getbuf(rel, blkno, access);
			continue;
		}

		if (P_IGNORE(opaque) || _bt_compare(rel, key, page, P_HIKEY) >= cmpval)
		{
			/* 向右迈一页 */
			buf = _bt_relandgetbuf(rel, buf, opaque->btpo_next, access);
			continue;
		}
		else
			break;
	}

	if (P_IGNORE(opaque))
		elog(ERROR, "fell off the end of index \"%s\"",
			 RelationGetRelationName(rel));

	return buf;
}

/*
 *	_bt_binsrch() -- 在某一页上对键做二分查找。
 *
 * 在叶子页上，_bt_binsrch() 返回第一个
 * >= 给定扫描键的键的 OffsetNumber；若 nextkey 为真，则返回 > 扫描键的键。（注意：
 * 特别地，若扫描键 > 页上全部键，有可能返回比
 * 页上键数大 1 的值。）
 *
 * 在内部（非叶子）页上，_bt_binsrch() 返回最后一个
 * < 给定扫描键的键的 OffsetNumber；若 nextkey 为真，则返回最后一个 <= 扫描键的键。
 * （由于 _bt_compare 把这种页的第一个数据键当作
 * 负无穷，至少会有一个键 < 扫描键，因此结果
 * 总是指向页上的某个键。）该键指出
 * 为确保找到全部 >= 给定扫描键的叶子键而应下降的位置
 * （当 nextkey 为真时，则是 > 给定扫描键的叶子键）。
 *
 * 本例程并不负责向右走，它只检查
 * 给定的页。_bt_binsrch() 对缓冲区没有锁或引用计数方面的
 * 副作用。
 */
static OffsetNumber
_bt_binsrch(Relation rel,
			BTScanInsert key,
			Buffer buf)
{
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber low,
				high;
	int32		result,
				cmpval;

	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);

	/* 在使用 scantid 时请求 nextkey 语义似乎没有意义 */
	Assert(!key->nextkey || key->scantid == NULL);
	/* 设置了 scantid 的调用者在叶子页上必须使用 _bt_binsrch_insert() */
	Assert(!P_ISLEAF(opaque) || key->scantid == NULL);

	low = P_FIRSTDATAKEY(opaque);
	high = PageGetMaxOffsetNumber(page);

	/*
	 * 若页上没有键，返回第一个可用槽。注意
	 * 这覆盖两种情形：页真的是空的（没有键），或者
	 * 页上只有一个高键。后一种情形在清理之后可能出现。
	 * 不过这绝不会发生在内部页上，因为它们
	 * 从不为空（内部页必须有子页）。
	 */
	if (unlikely(high < low))
		return low;

	/*
	 * 二分查找，找出页上第一个 >= 扫描键的键，或者当 nextkey
	 * 为真时第一个 > 扫描键的键。
	 *
	 * 对 nextkey=false（cmpval=1），循环不变式为：'low' 之前的所有槽
	 * 都 < 扫描键，'high' 处及其后的所有槽都 >= 扫描键。
	 *
	 * 对 nextkey=true（cmpval=0），循环不变式为：'low' 之前的所有槽
	 * 都 <= 扫描键，'high' 处及其后的所有槽都 > 扫描键。
	 *
	 * 当 high == low 时可以退出循环。
	 */
	high++;						/* 为 high 建立循环不变式 */

	cmpval = key->nextkey ? 0 : 1;	/* 选择比较值 */

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		/* 此时 low <= mid < high，因此 mid 指向一个真实槽 */

		result = _bt_compare(rel, key, page, mid);

		if (result >= cmpval)
			low = mid + 1;
		else
			high = mid;
	}

	/*
	 * 此时 high == low，但要小心：它们可能指向
	 * 页上最后一个槽之后。
	 *
	 * 在叶子页上，我们总是返回第一个 >= 扫描键的键（相应地，
	 * > 扫描键），它可能是最后一个槽 + 1。
	 */
	if (P_ISLEAF(opaque))
		return low;

	/*
	 * 在非叶子页上，返回最后一个 < 扫描键的键（相应地，<= 扫描键）。
	 * 若 _bt_compare() 遵守规则，这样的键必定存在。
	 */
	Assert(low > P_FIRSTDATAKEY(opaque));

	return OffsetNumberPrev(low);
}

/*
 *
 *	_bt_binsrch_insert() -- 可缓存的、增量式叶子页二分查找。
 *
 * 与 _bt_binsrch() 类似，但支持缓存二分查找的
 * 边界。仅在插入期间使用，并且只用在看起来
 * 调用者将要插入元组的那张叶子页上。排他锁定并钉住的
 * 叶子页包含在 insertstate 中。
 *
 * 把边界字段缓存在 insertstate 中，以便后续调用可以
 * 复用原先二分查找的 low 与严格 high 边界。直接使用
 * 这些字段的调用者必须准备好 low
 * 和/或 stricthigh 不在同一页上的情形（其中一个或两个超过该页的 maxoff）。
 * 页上没有项时（high < low），
 * 边界无效。
 *
 * 若调用者在第二次调用这里之前修改了该页，则负责使边界失效，
 * 并负责处理倒排列表
 * 元组匹配（调用者可用 insertstate 的 postingoff 字段
 * 确定哪个现有堆 TID 需要被一次倒排
 * 列表分裂所替换）。
 */
OffsetNumber
_bt_binsrch_insert(Relation rel, BTInsertState insertstate)
{
	BTScanInsert key = insertstate->itup_key;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber low,
				high,
				stricthigh;
	int32		result,
				cmpval;

	page = BufferGetPage(insertstate->buf);
	opaque = BTPageGetOpaque(page);

	Assert(P_ISLEAF(opaque));
	Assert(!key->nextkey);
	Assert(insertstate->postingoff == 0);

	if (!insertstate->bounds_valid)
	{
		/* 开始新的二分查找 */
		low = P_FIRSTDATAKEY(opaque);
		high = PageGetMaxOffsetNumber(page);
	}
	else
	{
		/* 恢复此前针对同一页的二分查找结果 */
		low = insertstate->low;
		high = insertstate->stricthigh;
	}

	/* 若页上没有键，返回第一个可用槽 */
	if (unlikely(high < low))
	{
		/* 调用者不能复用边界 */
		insertstate->low = InvalidOffsetNumber;
		insertstate->stricthigh = InvalidOffsetNumber;
		insertstate->bounds_valid = false;
		return low;
	}

	/*
	 * 二分查找，找出页上第一个 >= 扫描键的键。（插入时 nextkey
	 * 始终为假。）
	 *
	 * 循环不变式为：'low' 之前的所有槽都 < 扫描键，
	 * 'high' 处及其后的所有槽都 >= 扫描键。'stricthigh' > 扫描键，并加以
	 * 维护，以便为调用者节省额外的查找开销。
	 *
	 * 当 high == low 时可以退出循环。
	 */
	if (!insertstate->bounds_valid)
		high++;					/* 为 high 建立循环不变式 */
	stricthigh = high;			/* high 起初严格更高 */

	cmpval = 1;					/* !nextkey 的比较值 */

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		/* 此时 low <= mid < high，因此 mid 指向一个真实槽 */

		result = _bt_compare(rel, key, page, mid);

		if (result >= cmpval)
			low = mid + 1;
		else
		{
			high = mid;
			if (result != 0)
				stricthigh = high;
		}

		/*
		 * 若二分查找定位到的偏移处的元组是倒排列表，且其
		 * TID 范围与调用者的 scantid 重叠，则对倒排列表做
		 * 二分查找，为调用者设置 postingoff。当 postingoff 已设置时，调用者必须分裂
		 * 该倒排列表。这种情况应当
		 * 很少发生。
		 */
		if (unlikely(result == 0 && key->scantid != NULL))
		{
			/*
			 * 每次叶子页
			 * 二分查找中，postingoff 绝不应被设置超过一次。否则就意味着索引中存在重复的表
			 * TIDs，这是绝不允许的。在此检查这一点。
			 */
			if (insertstate->postingoff != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg_internal("table tid from new index tuple (%u,%u) cannot find insert offset between offsets %u and %u of block %u in index \"%s\"",
										 ItemPointerGetBlockNumber(key->scantid),
										 ItemPointerGetOffsetNumber(key->scantid),
										 low, stricthigh,
										 BufferGetBlockNumber(insertstate->buf),
										 RelationGetRelationName(rel))));

			insertstate->postingoff = _bt_binsrch_posting(key, page, mid);
		}
	}

	/*
	 * 在叶子页上，二分查找总是返回第一个 >= 扫描
	 * 键的键（至少在 !nextkey 情形下），它可能是最后一个槽 + 1。这也是
	 * 缓存查找的下界。
	 *
	 * stricthigh 也可能是最后一个槽 + 1，这使调用者无法
	 * 直接使用边界，但若带着缓存边界被第二次调用，
	 * 它对我们仍然有用（发生这种情况时，缓存的 low
	 * 会 < stricthigh）。
	 */
	insertstate->low = low;
	insertstate->stricthigh = stricthigh;
	insertstate->bounds_valid = true;

	return low;
}

/*----------
 *	_bt_binsrch_posting() -- 倒排列表二分查找。
 *
 * _bt_binsrch_insert() 的辅助例程。
 *
 * 返回调用者的 scantid 在倒排列表中所属的偏移。
 *----------
 */
static int
_bt_binsrch_posting(BTScanInsert key, Page page, OffsetNumber offnum)
{
	IndexTuple	itup;
	ItemId		itemid;
	int			low,
				high,
				mid,
				res;

	/*
	 * 若这不是倒排元组，则索引必定已损坏（若它是
	 * 普通的非枢轴元组，则必定已有一个元组，其
	 * 堆 TID 等于插入者的新堆 TID/scantid）。防御性地
	 * 检查该元组确是倒排列表元组，且其倒排列表范围
	 * 包含调用者的 scantid。
	 *
	 * （这也是因为 contrib/amcheck 的 rootdescend 选项需要
	 * 能够用 _bt_binsrch_insert() 重新定位非枢轴元组。）
	 */
	itemid = PageGetItemId(page, offnum);
	itup = (IndexTuple) PageGetItem(page, itemid);
	if (!BTreeTupleIsPosting(itup))
		return 0;

	Assert(key->heapkeyspace && key->allequalimage);

	/*
	 * 若倒排列表元组设置了 LP_DEAD 位，则向
	 * _bt_binsrch_insert() 的调用者返回 -1 这一哨兵值来表明此事。当调用者
	 * 已经去掉该死亡项之后，可以再次调用
	 * _bt_binsrch_insert()。
	 */
	if (ItemIdIsDead(itemid))
		return -1;

	/* 为了循环不变式，"high" 位于倒排列表末尾之后 */
	low = 0;
	high = BTreeTupleGetNPosting(itup);
	Assert(high >= 2);

	while (high > low)
	{
		mid = low + ((high - low) / 2);
		res = ItemPointerCompare(key->scantid,
								 BTreeTupleGetPostingN(itup, mid));

		if (res > 0)
			low = mid + 1;
		else if (res < 0)
			high = mid;
		else
			return mid;
	}

	/* 未找到精确匹配 */
	return low;
}

/*----------
 *	_bt_compare() -- 把插入型扫描键与页上的元组比较。
 *
 *	page/offnum：要比较的 B-树项的位置。
 *
 *		本例程返回：
 *			若扫描键 < offnum 处的元组，则 <0；
 *			 若扫描键 == offnum 处的元组，则 0；
 *			若扫描键 > offnum 处的元组，则 >0。
 *
 * 键中的 NULL 被当作可排序的值。因此
 * "相等"并不必然意味着该项应当作为匹配键
 * 返回给调用者。类似地，设置了 scantid 的插入扫描键
 * 被当作等于这样一个倒排元组：其 TID
 * 范围与它们的 scantid 重叠。倒排元组中通常不会有
 * 匹配的 TID，调用者必须自行处理
 * （例如，分裂该倒排列表元组）。
 *
 * 关键说明：在非叶子页上，第一个数据键被假定为
 * "负无穷"：本例程将始终声称它小于
 * 扫描键。版本 3 及以上的索引把实际存储的键值明确截断为 0
 * 个属性（明确的负无穷），但
 * 实现并不依赖这一点。这使我们能够实现 Lehman 与
 * Yao 的约定：第一个下行指针位于第一个
 * 键之前。详见 backend/access/nbtree/README。
 *----------
 */
int32
_bt_compare(Relation rel,
			BTScanInsert key,
			Page page,
			OffsetNumber offnum)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	IndexTuple	itup;
	ItemPointer heapTid;
	ScanKey		scankey;
	int			ncmpkey;
	int			ntupatts;
	int32		result;

	Assert(_bt_check_natts(rel, key->heapkeyspace, page, offnum));
	Assert(key->keysz <= IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(key->heapkeyspace || key->scantid == NULL);

	/*
	 * 若目标项是内部页上的第一个数据项，则强制结果为 ">"
	 * ——见上面的说明。
	 */
	if (!P_ISLEAF(opaque) && offnum == P_FIRSTDATAKEY(opaque))
		return 1;

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	ntupatts = BTreeTupleGetNAtts(itup, rel);

	/*
	 * 扫描键为键中的每一项设置了相关联的属性号。
	 * 重要的是，若索引是多键的，扫描必须
	 * 包含前 k 个键属性，并且它们按顺序排列。若
	 * 想一想多键排序如何工作，就会明白
	 * 为什么如此。
	 *
	 * 不过这里并不检查是否违反了该条件。索引扫描的
	 * 初始设置最好已经做对了（见
	 * _bt_first）。
	 */

	ncmpkey = Min(ntupatts, key->keysz);
	Assert(key->heapkeyspace || ncmpkey == key->keysz);
	Assert(!BTreeTupleIsPosting(itup) || key->allequalimage);
	scankey = key->scankeys;
	for (int i = 1; i <= ncmpkey; i++)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(itup, scankey->sk_attno, itupdesc, &isNull);

		if (scankey->sk_flags & SK_ISNULL)	/* 键为 NULL */
		{
			if (isNull)
				result = 0;		/* NULL "=" NULL */
			else if (scankey->sk_flags & SK_BT_NULLS_FIRST)
				result = -1;	/* NULL "<" NOT_NULL */
			else
				result = 1;		/* NULL ">" NOT_NULL */
		}
		else if (isNull)		/* 键为 NOT_NULL 且项为 NULL */
		{
			if (scankey->sk_flags & SK_BT_NULLS_FIRST)
				result = 1;		/* NOT_NULL ">" NULL */
			else
				result = -1;	/* NOT_NULL "<" NULL */
		}
		else
		{
			/*
			 * 需要把索引值作为左参数、
			 * 把 sk_argument 作为右参数传给 sk_func（它们的
			 * 类型可能不同）。由于调用者习惯把
			 * _bt_compare 理解为拿扫描键去比较索引项，我们
			 * 必须翻转比较结果的符号。（除非它是 DESC
			 * 列，那种情况下我们*不*翻转符号。）
			 */
			result = DatumGetInt32(FunctionCall2Coll(&scankey->sk_func,
													 scankey->sk_collation,
													 datum,
													 scankey->sk_argument));

			if (!(scankey->sk_flags & SK_BT_DESC))
				INVERT_COMPARE_RESULT(result);
		}

		/* 若键不相等，返回差值 */
		if (result != 0)
			return result;

		scankey++;
	}

	/*
	 * 除堆 TID 之外，所有未被截断的属性都被发现
	 * 相等。当扫描键带有本来会直接比较的
	 * 键属性值时，把被截断的属性当作负无穷。
	 *
	 * 注意：ntupatts 是否包含非键属性并不重要；
	 * 扫描键不会包含，因此没有必要
	 * 显式排除非键属性。
	 */
	if (key->keysz > ntupatts)
		return 1;

	/*
	 * 用堆 TID 属性与 scantid 尝试打破平局。规则
	 * 与任何其他键属性相同——只是
	 * 表示方式不同。
	 */
	heapTid = BTreeTupleGetHeapTID(itup);
	if (key->scantid == NULL)
	{
		/*
		 * 大多数搜索的扫描键，在扫描键对
		 * 元组中直到并包括最低有效的未被截断
		 * 属性在内的各属性都有相等值时，会被认为大于
		 * 被截断的枢轴元组。
		 *
		 * 例如，若索引只有最少的两个属性（一个
		 * 用户键属性，加上堆 TID 属性），且某页的高键
		 * 是 ('foo', -inf)，扫描键是 ('foo', <省略>)，则搜索
		 * 不会下降到左边那一页。搜索会改为向
		 * 右下降。枢轴元组中被截断的属性意味着
		 * 左边那一页上的所有非枢轴元组都严格 < 'foo'，
		 * 因此没有必要向左下降。换句话说，搜索
		 * 不必向左下降，因为它并不关心堆 TID 值为 -inf
		 * 的匹配。
		 *
		 * 不过，有些搜索（pivotsearch 搜索）在发生这种情况时实际上需要
		 * 我们向左下降。-inf 被当作省略的扫描键属性的一种可能
		 * 匹配。页删除需要这一点，它必须
		 * 用这些叶子页的高键重新找到
		 * 作为删除目标的叶子页。
		 *
		 * 注意：测试中的堆 TID 部分确保扫描键正在与
		 * 带有一个或多个被截断键
		 * 属性的枢轴元组比较。
		 *
		 * 注意：经 pg_upgrade 升级的 !heapkeyspace 索引在这里必须始终向
		 * 左下降，因为它们没有堆 TID 属性（而且无论如何都无法具有
		 * 任何 -inf 键值，因为截断只能去掉
		 * 非键属性）。!heapkeyspace 搜索在到达
		 * 叶子层之后，必须始终准备好处理枢轴两侧的
		 * 匹配。
		 */
		if (key->heapkeyspace && !key->pivotsearch &&
			key->keysz == ntupatts && heapTid == NULL)
			return 1;

		/* 所提供的扫描键参数全部被发现相等 */
		return 0;
	}

	/*
	 * 把被截断的堆 TID 当作负无穷，因为扫描键有一个
	 * 本来会直接比较的键属性值（scantid）
	 */
	Assert(key->keysz == IndexRelationGetNumberOfKeyAttributes(rel));
	if (heapTid == NULL)
		return 1;

	/*
	 * 若扫描键的 scantid 值落在倒排列表范围内，则必须把它当作等于该倒排列表元组。
	 * 在所有其他情形下
	 * 只能有一个堆 TID 值，它被直接与
	 * scantid 比较。
	 */
	Assert(ntupatts >= IndexRelationGetNumberOfKeyAttributes(rel));
	result = ItemPointerCompare(key->scantid, heapTid);
	if (result <= 0 || !BTreeTupleIsPosting(itup))
		return result;
	else
	{
		result = ItemPointerCompare(key->scantid,
									BTreeTupleGetMaxHeapTID(itup));
		if (result > 0)
			return 1;
	}

	return 0;
}

/*
 *	_bt_first() -- 找出扫描中的第一项。
 *
 *		需要精明地处理扫描方向、搜索
 *		条件以及树的排序。我们找出树中满足
 *		扫描键限定条件的第一项（若是
 *		反向扫描，则是最后一项）。成功退出时，包含
 *		当前索引元组的页已钉住但未加锁，并且关于
 *		页上匹配元组的数据已装入 so->currPos。
 *		scan->xs_ctup.t_self 被设为当前元组的堆 TID，
 *		若有请求，scan->xs_itup 指向索引元组的一份副本。
 *
 * 若索引中没有匹配项，我们返回 false，并且不持有
 * 任何钉住或锁。
 *
 * 注意 scan->keyData[] 以及由它构建的 so->keyData[] 扫描键
 * 都是搜索型扫描键（详见 nbtree/README）。
 * 在本例程内部，我们构建一个临时的插入型扫描键，用来
 * 定位扫描的起点。
 */
bool
_bt_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Buffer		buf;
	BTStack		stack;
	OffsetNumber offnum;
	StrategyNumber strat;
	bool		nextkey;
	bool		goback;
	BTScanInsertData inskey;
	ScanKey		startKeys[INDEX_MAX_KEYS];
	ScanKeyData notnullkeys[INDEX_MAX_KEYS];
	int			keysCount = 0;
	int			i;
	bool		status;
	StrategyNumber strat_total;
	BTScanPosItem *currItem;
	BlockNumber blkno;

	Assert(!BTScanPosIsValid(so->currPos));

	pgstat_count_index_scan(rel);

	/*
	 * 检查扫描键并去掉任何冗余键；同时标出
	 * 为继续扫描而必须匹配的键。
	 */
	_bt_preprocess_keys(scan);

	/*
	 * 若 _bt_preprocess_keys() 发现扫描键永远无法
	 * 被满足（例如 x == 1 AND x > 2），则立即退出。
	 */
	if (!so->qual_ok)
	{
		/* 通知其他工作进程：我们对这个扫描键已经处理完。 */
		_bt_parallel_done(scan);
		return false;
	}

	/*
	 * 对并行扫描，从共享状态取得起始页。若扫描
	 * 尚未开始，则按通常方式找出第一张叶子页，
	 * 同时让其他参与进程等待。若扫描
	 * 已经开始，则使用共享结构中的页号。
	 */
	if (scan->parallel_scan != NULL)
	{
		status = _bt_parallel_seize(scan, &blkno);
		if (!status)
			return false;
		else if (blkno == P_NONE)
		{
			_bt_parallel_done(scan);
			return false;
		}
		else if (blkno != InvalidBlockNumber)
		{
			if (!_bt_parallel_readpage(scan, blkno, dir))
				return false;
			goto readcomplete;
		}
	}

	/*----------
	 * 检查扫描键，以发现应当从何处开始扫描。
	 *
	 * 我们希望识别出可用作起始边界的键；
	 * 对正向扫描，它们是 =、> 或 >= 键；对反向扫描，则是 =、<、<= 键。
	 * 只要前面的属性只有 =、>=（相应地，=、<=）键，就可以使用多个属性的键。
	 * 一旦我们接受了
	 * 一个 > 或 < 边界，或者发现某个属性没有边界（这可以
	 * 看作与 "> -infinity" 相同），就不能再使用它右侧任何
	 * 属性的键，因为那会破坏我们对
	 * 应当采用何种初始定位策略的简单设想。
	 *
	 * 当扫描键包含跨类型操作符时，_bt_preprocess_keys
	 * 可能无法去掉冗余键；这种情况下我们会
	 * 为每个属性任意挑选一个可用的键。这是正确的，
	 * 但未必是最优行为。（例如，对于
	 * "x >= 4 AND x >= 5" 这样的键，我们会选择从 x=4 开始扫描，而
	 * 从 x=5 开始会更高效。）由于这种情况只在
	 * 查询措辞不佳且操作符族不完整时出现，就此接受。
	 *
	 * 当单个属性同时出现等值键与不等值键时
	 * （同样，只有出现跨类型操作符时才可能），我们*必须*
	 * 选择其中一个等值键作为起点，因为
	 * 一旦某个等值限定失败，_bt_checkkeys() 就会停止扫描。
	 * 例如，若键是 "x >= 4 AND x = 10"，而我们选择
	 * 从 x=4 开始，就会在到达 x=10 之前失败并停止。若预处理之后仍有多个
	 * 等值限定幸存，用哪一个并不重要——按定义，它们要么冗余，
	 * 要么相互矛盾。
	 * 不存在第三种情形。
	 *
	 * 任何常规（不是 SK_SEARCHNULL）键都蕴含 NOT NULL 限定。
	 * 若索引把空值存放在我们将要开始的那一端，
	 * 并且该列没有边界键（这意味着我们据以推断 NOT NULL 的那个键
	 * 是约束索引另一端的不等值键），则临时构造一个显式的 SK_SEARCHNOTNULL 键，
	 * 用作边界键。若不这样做，
	 * 扫描一开始我们就可能遍历大量空值项。
	 * 那些 NULL 项就堆在扫描起点。
	 *
	 * 在这个循环中，行比较键被当作其
	 * 第一个（最左）列上的键来处理。若可能，下面会补上行比较中
	 * 较低序的列。
	 *
	 * 被选中的扫描键（每个索引列至多一个）通过
	 * 把它们的地址存入局部 startKeys[] 数组来记住。
	 *----------
	 */
	strat_total = BTEqualStrategyNumber;
	if (so->numberOfKeys > 0)
	{
		AttrNumber	curattr;
		ScanKey		chosen;
		ScanKey		impliesNN;
		ScanKey		cur;

		/*
		 * chosen 是到目前为止为当前属性选定的键（若有）。
		 * 在遇到下一个属性的键之前，我们并不把这个决定
		 * 定死。
		 */
		curattr = 1;
		chosen = NULL;
		/* 同时记住任何蕴含 NOT NULL 约束的扫描键 */
		impliesNN = NULL;

		/*
		 * 循环从 0 迭代到 numberOfKeys（含）；最后一轮
		 * 用来处理"最后一个键之后"的收尾。真正退出循环
		 * 发生在下面某一条 "break" 语句。
		 */
		for (cur = so->keyData, i = 0;; cur++, i++)
		{
			if (i >= so->numberOfKeys || cur->sk_attno != curattr)
			{
				/*
				 * 对 curattr 的键已经看完。若没有找到
				 * 可用的边界键，看看能否推断出一个 NOT NULL 键。
				 */
				if (chosen == NULL && impliesNN != NULL &&
					((impliesNN->sk_flags & SK_BT_NULLS_FIRST) ?
					 ScanDirectionIsForward(dir) :
					 ScanDirectionIsBackward(dir)))
				{
					/* 可以，因此在 notnullkeys[keysCount] 中构造该键 */
					chosen = &notnullkeys[keysCount];
					ScanKeyEntryInitialize(chosen,
										   (SK_SEARCHNOTNULL | SK_ISNULL |
											(impliesNN->sk_flags &
											 (SK_BT_DESC | SK_BT_NULLS_FIRST))),
										   curattr,
										   ((impliesNN->sk_flags & SK_BT_NULLS_FIRST) ?
											BTGreaterStrategyNumber :
											BTLessStrategyNumber),
										   InvalidOid,
										   InvalidOid,
										   InvalidOid,
										   (Datum) 0);
				}

				/*
				 * 若仍然没有找到可用的边界键，则退出；否则
				 * 把边界键指针存入 startKeys。
				 */
				if (chosen == NULL)
					break;
				startKeys[keysCount++] = chosen;

				/*
				 * 调整 strat_total，若已经存了一个 > 或 <
				 * 键，则退出。
				 */
				strat = chosen->sk_strategy;
				if (strat != BTEqualStrategyNumber)
				{
					strat_total = strat;
					if (strat == BTGreaterStrategyNumber ||
						strat == BTLessStrategyNumber)
						break;
				}

				/*
				 * 若那是最后一个属性，或者下一个键不
				 * 按顺序出现（意味着下一个属性没有可用的
				 * 边界键），则结束。
				 */
				if (i >= so->numberOfKeys ||
					cur->sk_attno != curattr + 1)
					break;

				/*
				 * 为下一个属性重置。
				 */
				curattr = cur->sk_attno;
				chosen = NULL;
				impliesNN = NULL;
			}

			/*
			 * 这个键能否用作本属性的起始边界？
			 *
			 * 若不能，它是否蕴含 NOT NULL 约束？（因为
			 * SK_SEARCHNULL 键总是被赋为 BTEqualStrategyNumber，
			 * *任何*不等值键都能起到这个作用；不必再测试。）
			 */
			switch (cur->sk_strategy)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					if (chosen == NULL)
					{
						if (ScanDirectionIsBackward(dir))
							chosen = cur;
						else
							impliesNN = cur;
					}
					break;
				case BTEqualStrategyNumber:
					/* 覆盖任何非等值选择 */
					chosen = cur;
					break;
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
					if (chosen == NULL)
					{
						if (ScanDirectionIsForward(dir))
							chosen = cur;
						else
							impliesNN = cur;
					}
					break;
			}
		}
	}

	/*
	 * 若没有找到可用的边界键，就必须从树的某一端
	 * 开始。沿那条边走到第一个或最后一个键，再从
	 * 那里开始扫描。
	 */
	if (keysCount == 0)
	{
		bool		match;

		match = _bt_endpoint(scan, dir);

		if (!match)
		{
			/* 没有匹配，因此把（并行）扫描标为已完成 */
			_bt_parallel_done(scan);
		}

		return match;
	}

	/*
	 * 我们希望从索引内部某处开始扫描。建立一个
	 * 插入型扫描键，用来搜索上面确定的边界点。
	 * 该插入型扫描键用 startKeys[] 所标识的键来构建。
	 * （插入型扫描键的其余字段在初始定位策略
	 * 最终确定之后再初始化。）
	 */
	Assert(keysCount <= INDEX_MAX_KEYS);
	for (i = 0; i < keysCount; i++)
	{
		ScanKey		cur = startKeys[i];

		Assert(cur->sk_attno == i + 1);

		if (cur->sk_flags & SK_ROW_HEADER)
		{
			/*
			 * 行比较头：改为查看第一个行成员。
			 *
			 * 成员扫描键已经是插入格式（也就是说，它们的
			 * sk_func 是三路比较函数），但必须留意
			 * 空值，_bt_preprocess_keys 并没有检查它们。第一个行成员中的
			 * 空值会使条件无法匹配，就像
			 * qual_ok = false 一样。
			 */
			ScanKey		subkey = (ScanKey) DatumGetPointer(cur->sk_argument);

			Assert(subkey->sk_flags & SK_ROW_MEMBER);
			if (subkey->sk_flags & SK_ISNULL)
			{
				_bt_parallel_done(scan);
				return false;
			}
			memcpy(inskey.scankeys + i, subkey, sizeof(ScanKeyData));

			/*
			 * 若该行比较是我们接受的最后一个定位键，
			 * 尝试从较低序的行成员再补充一些键。
			 * （若我们已经接受了其他索引列上的独立条件，
			 * 就改用那些条件——不值得再去判断
			 * 哪一个限制更强。）注意，即使行比较是 ">" 或 "<" 类型，这样做也是可以的，
			 * 因为除最后一个行成员之外，施加的条件实际上是
			 * ">=" 或 "<="，因此额外的键不会破坏定位
			 * 方案。但同理，若我们无法用上全部
			 * 行成员，那么已经用上的那部分行比较就只能当作
			 * 一个 ">=" 或 "<=" 条件，
			 * 因此最好相应地
			 * 调整 strat_total。
			 */
			if (i == keysCount - 1)
			{
				bool		used_all_subkeys = false;

				Assert(!(subkey->sk_flags & SK_ROW_END));
				for (;;)
				{
					subkey++;
					Assert(subkey->sk_flags & SK_ROW_MEMBER);
					if (subkey->sk_attno != keysCount + 1)
						break;	/* 顺序不对，不能使用 */
					if (subkey->sk_strategy != cur->sk_strategy)
						break;	/* 方向不对，不能使用 */
					if (subkey->sk_flags & SK_ISNULL)
						break;	/* 不能使用 NULL 键 */
					Assert(keysCount < INDEX_MAX_KEYS);
					memcpy(inskey.scankeys + keysCount, subkey,
						   sizeof(ScanKeyData));
					keysCount++;
					if (subkey->sk_flags & SK_ROW_END)
					{
						used_all_subkeys = true;
						break;
					}
				}
				if (!used_all_subkeys)
				{
					switch (strat_total)
					{
						case BTLessStrategyNumber:
							strat_total = BTLessEqualStrategyNumber;
							break;
						case BTGreaterStrategyNumber:
							strat_total = BTGreaterEqualStrategyNumber;
							break;
					}
				}
				break;			/* 外层循环到此结束 */
			}
		}
		else
		{
			/*
			 * 普通比较键。把搜索型扫描键
			 * 转换成插入型扫描键，办法是把 sk_func 替换为
			 * 合适的 B-树比较函数。
			 *
			 * 若扫描键操作符不是跨类型比较，可以使用
			 * 缓存的比较函数；否则就得在
			 * 系统目录中查找。（这不会导致无限递归，因为由系统缓存查找发起的
			 * 索引扫描不会使用跨数据类型
			 * 操作符。）
			 *
			 * 我们支持这样的约定：sk_subtype == InvalidOid 表示
			 * 操作符类的输入类型；这是为了简化
			 * ScanKeyInit() 而采用的权宜做法。
			 */
			if (cur->sk_subtype == rel->rd_opcintype[i] ||
				cur->sk_subtype == InvalidOid)
			{
				FmgrInfo   *procinfo;

				procinfo = index_getprocinfo(rel, cur->sk_attno, BTORDER_PROC);
				ScanKeyEntryInitializeWithInfo(inskey.scankeys + i,
											   cur->sk_flags,
											   cur->sk_attno,
											   InvalidStrategy,
											   cur->sk_subtype,
											   cur->sk_collation,
											   procinfo,
											   cur->sk_argument);
			}
			else
			{
				RegProcedure cmp_proc;

				cmp_proc = get_opfamily_proc(rel->rd_opfamily[i],
											 rel->rd_opcintype[i],
											 cur->sk_subtype,
											 BTORDER_PROC);
				if (!RegProcedureIsValid(cmp_proc))
					elog(ERROR, "missing support function %d(%u,%u) for attribute %d of index \"%s\"",
						 BTORDER_PROC, rel->rd_opcintype[i], cur->sk_subtype,
						 cur->sk_attno, RelationGetRelationName(rel));
				ScanKeyEntryInitialize(inskey.scankeys + i,
									   cur->sk_flags,
									   cur->sk_attno,
									   InvalidStrategy,
									   cur->sk_subtype,
									   cur->sk_collation,
									   cmp_proc,
									   cur->sk_argument);
			}
		}
	}

	/*----------
	 * 检查所选的初始定位策略，以准确确定
	 * 应当从何处开始扫描，并设置标志变量来控制
	 * 下面的代码。
	 *
	 * 若 nextkey = false，_bt_search 与 _bt_binsrch 会定位第一个
	 * >= 扫描键的项。若 nextkey = true，它们会定位第一个
	 * > 扫描键的项。
	 *
	 * 若 goback = true，我们随后后退一项；若
	 * goback = false，则从定位到的那一项开始扫描。
	 *----------
	 */
	switch (strat_total)
	{
		case BTLessStrategyNumber:

			/*
			 * 找出第一个 >= 扫描键的项，然后后退一项，到达最后一个
			 * < 扫描键的项。（注意：此定位策略只用于
			 * 反向扫描，因此那总是正确的起始
			 * 位置。）
			 */
			nextkey = false;
			goback = true;
			break;

		case BTLessEqualStrategyNumber:

			/*
			 * 找出第一个 > 扫描键的项，然后后退一项，到达最后一个
			 * <= 扫描键的项。（注意：此定位策略只用于
			 * 反向扫描，因此那总是正确的起始
			 * 位置。）
			 */
			nextkey = true;
			goback = true;
			break;

		case BTEqualStrategyNumber:

			/*
			 * 若指定了反向扫描，需要从最后一个相等项开始，
			 * 而不是第一个。
			 */
			if (ScanDirectionIsBackward(dir))
			{
				/*
				 * 这与 <= 策略相同。我们会在
				 * 结束时检查找到的项是否真的 =。
				 */
				nextkey = true;
				goback = true;
			}
			else
			{
				/*
				 * 这与 >= 策略相同。我们会在
				 * 结束时检查找到的项是否真的 =。
				 */
				nextkey = false;
				goback = false;
			}
			break;

		case BTGreaterEqualStrategyNumber:

			/*
			 * 找出第一个 >= 扫描键的项。（这只用于正向
			 * 扫描。）
			 */
			nextkey = false;
			goback = false;
			break;

		case BTGreaterStrategyNumber:

			/*
			 * 找出第一个 > 扫描键的项。（这只用于正向
			 * 扫描。）
			 */
			nextkey = true;
			goback = false;
			break;

		default:
			/* 到不了这里，但让编译器保持安静 */
			elog(ERROR, "unrecognized strat_total: %d", (int) strat_total);
			return false;
	}

	/* 初始化插入型扫描键的其余字段 */
	_bt_metaversion(rel, &inskey.heapkeyspace, &inskey.allequalimage);
	inskey.anynullkeys = false; /* 未使用 */
	inskey.nextkey = nextkey;
	inskey.pivotsearch = false;
	inskey.scantid = NULL;
	inskey.keysz = keysCount;

	/*
	 * 用构造好的插入型扫描键沿树下降，
	 * 把自己定位到目标叶子页上。
	 */
	stack = _bt_search(rel, NULL, &inskey, &buf, BT_READ, scan->xs_snapshot);

	/* 不需要再保留这个栈…… */
	_bt_freestack(stack);

	if (!BufferIsValid(buf))
	{
		/*
		 * 只有索引完全为空时才会走到这里。锁定整个关系，
		 * 因为不存在更细的可锁对象。若没有缓冲区锁，
		 * 另一个事务有可能在
		 * _bt_search() 与 PredicateLockRelation() 之间插入数据。取得
		 * 关系级谓词锁之后，我们必须重新搜索，以堵住一个狭窄
		 * 窗口：否则我们不会扫描并发插入的元组，而
		 * 写入者却看不到我们的谓词锁。
		 */
		if (IsolationIsSerializable())
		{
			PredicateLockRelation(rel, scan->xs_snapshot);
			stack = _bt_search(rel, NULL, &inskey, &buf, BT_READ,
							   scan->xs_snapshot);
			_bt_freestack(stack);
		}

		if (!BufferIsValid(buf))
		{
			/*
			 * 把并行扫描标为已完成，以便所有工作进程都能结束
			 * 它们的扫描。
			 */
			_bt_parallel_done(scan);
			BTScanPosInvalidate(so->currPos);
			return false;
		}
	}

	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);

	_bt_initialize_more_data(so, dir);

	/* 定位到页上的精确项 */
	offnum = _bt_binsrch(rel, &inskey, buf);

	/*
	 * 若 nextkey = false，我们定位在第一个 >= 扫描键的项上，或者
	 * 可能位于这样一页的末尾：该页上现有项都小于
	 * 扫描键，并且我们知道后面各页上的一切都大于
	 * 或等于扫描键。
	 *
	 * 若 nextkey = true，我们定位在第一个 > 扫描键的项上，或者
	 * 可能位于这样一页的末尾：该页上现有项都小于
	 * 或等于扫描键，并且我们知道后面各页上的一切都
	 * 大于扫描键。
	 *
	 * 真正想要的起点要么是这一项，要么是前一项；
	 * 在页末情形下，则是下一页的第一项，或者
	 * 本页的最后一项。必要时调整起始偏移。（若
	 * 结果偏移位于第一项之前或最后一项之后，
	 * _bt_readpage 会报告没有找到项，然后我们再按需要走到
	 * 下一页。）
	 */
	if (goback)
		offnum = OffsetNumberPrev(offnum);

	/* 记住我们钉住了哪个缓冲区（若有） */
	Assert(!BTScanPosIsValid(so->currPos));
	so->currPos.buf = buf;

	/*
	 * 现在从扫描的第一页装入数据。
	 */
	if (!_bt_readpage(scan, dir, offnum))
	{
		/*
		 * 本页上没有真正匹配的数据。尝试前进到
		 * 下一页。若完全没有匹配数据，则返回 false。
		 */
		_bt_unlockbuf(scan->indexRelation, so->currPos.buf);
		if (!_bt_steppage(scan, dir))
			return false;
	}
	else
	{
		/* 放下当前页上的锁，并视情况放下钉住 */
		_bt_drop_lock_and_maybe_pin(scan, &so->currPos);
	}

readcomplete:
	/* 好，itemIndex 指出该返回哪一项 */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;
	if (scan->xs_want_itup)
		scan->xs_itup = (IndexTuple) (so->currTuples + currItem->tupleOffset);

	return true;
}

/*
 *	_bt_next() -- 取得扫描中的下一项。
 *
 *		进入时，so->currPos 描述当前页，该页可能已钉住
 *		但未加锁，so->currPos.itemIndex 标明此前返回的是
 *		哪一项。
 *
 *		成功退出时，scan->xs_ctup.t_self 被设为下一个
 *		堆元组的 TID；若有请求，scan->xs_itup 指向
 *		索引元组的一份副本。so->currPos 按需要更新。
 *
 *		失败退出时（没有更多元组），我们释放钉住，并把
 *		so->currPos.buf 设为 InvalidBuffer。
 */
bool
_bt_next(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	BTScanPosItem *currItem;

	/*
	 * 前进到当前页上的下一个元组；若没有更多元组，则尝试
	 * 走到下一张含有数据的页。
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (++so->currPos.itemIndex > so->currPos.lastItem)
		{
			if (!_bt_steppage(scan, dir))
				return false;
		}
	}
	else
	{
		if (--so->currPos.itemIndex < so->currPos.firstItem)
		{
			if (!_bt_steppage(scan, dir))
				return false;
		}
	}

	/* 好，itemIndex 指出该返回哪一项 */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;
	if (scan->xs_want_itup)
		scan->xs_itup = (IndexTuple) (so->currTuples + currItem->tupleOffset);

	return true;
}

/*
 *	_bt_readpage() -- 把当前索引页的数据装入 so->currPos
 *
 * 调用者必须已经钉住并读锁定 so->currPos.buf；这里不改变缓冲区的状态。
 * 另外，currPos.moreLeft 与 moreRight 必须有效；
 * 它们会按情况更新。so->currPos 的所有其他字段
 * 都在这里从头初始化。
 *
 * 我们从 offnum 开始、沿指定方向扫描当前页。
 * 所有匹配扫描键的项都装入 currPos.items。
 * 若 _bt_checkkeys 报告当前扫描方向上不可能再有匹配元组，
 * 则清除 moreLeft 或 moreRight（视情况而定）。
 *
 * 对于并行扫描，调用者在调用本函数之前必须已经调用 _bt_parallel_seize；
 * 本函数在返回之前会调用
 * _bt_parallel_release。
 *
 * 若页上找到任何匹配项则返回 true，否则返回 false。
 */
static bool
_bt_readpage(IndexScanDesc scan, ScanDirection dir, OffsetNumber offnum)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	int			itemIndex;
	bool		continuescan;
	int			indnatts;

	/*
	 * 我们必须已经钉住并锁定缓冲区，但这里不能使用通常的宏；
	 * 正是本函数使该宏对 currPos 变得适用。
	 */
	Assert(BufferIsValid(so->currPos.buf));

	page = BufferGetPage(so->currPos.buf);
	opaque = BTPageGetOpaque(page);

	/* 允许并行工作进程处理下一页 */
	if (scan->parallel_scan)
	{
		if (ScanDirectionIsForward(dir))
			_bt_parallel_release(scan, opaque->btpo_next);
		else
			_bt_parallel_release(scan, BufferGetBlockNumber(so->currPos.buf));
	}

	continuescan = true;		/* 默认假设 */
	indnatts = IndexRelationGetNumberOfAttributes(scan->indexRelation);
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * 记下缓冲区的块号，以便稍后释放钉住。
	 * 这样，若以后为了做提示而再次需要该缓冲区，我们可以重新读取它。
	 */
	so->currPos.currPage = BufferGetBlockNumber(so->currPos.buf);

	/*
	 * 读取该页时保存它的 LSN，以便知道稍后
	 * 对页施加 LP_DEAD 提示是否安全。这使我们能够为 MVCC 扫描放下
	 * 钉住，从而使清理可以避免阻塞。
	 */
	so->currPos.lsn = BufferGetLSNAtomic(so->currPos.buf);

	/*
	 * 扫描该页时必须保存它的右链接；这告诉我们
	 * 处理完这些项之后应当向右走到哪里。左链接没有
	 * 对应的需要，因为分裂总是向右进行。
	 */
	so->currPos.nextPage = opaque->btpo_next;

	/* 把元组工作区初始化为空 */
	so->currPos.nextTupleOffset = 0;

	/*
	 * 当前页已经保持一致，该宏现在应当
	 * 可用了。
	 */
	Assert(BTScanPosIsPinned(so->currPos));

	if (ScanDirectionIsForward(dir))
	{
		/* 按升序装入 items[] */
		itemIndex = 0;

		offnum = Max(offnum, minoff);

		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	itup;

			/*
			 * 若扫描指定不返回已杀死的元组，则我们
			 * 把已杀死的元组当作未通过限定条件
			 */
			if (scan->ignore_killed_tuples && ItemIdIsDead(iid))
			{
				offnum = OffsetNumberNext(offnum);
				continue;
			}

			itup = (IndexTuple) PageGetItem(page, iid);

			if (_bt_checkkeys(scan, itup, indnatts, dir, &continuescan))
			{
				/* 元组通过全部扫描键条件 */
				if (!BTreeTupleIsPosting(itup))
				{
					/* 记住它 */
					_bt_saveitem(so, itemIndex, offnum, itup);
					itemIndex++;
				}
				else
				{
					int			tupleOffset;

					/*
					 * 建立返回倒排列表的状态，并记住第一个
					 * TID
					 */
					tupleOffset =
						_bt_setuppostingitems(so, itemIndex, offnum,
											  BTreeTupleGetPostingN(itup, 0),
											  itup);
					itemIndex++;
					/* 记住其余 TIDs */
					for (int i = 1; i < BTreeTupleGetNPosting(itup); i++)
					{
						_bt_savepostingitem(so, itemIndex, offnum,
											BTreeTupleGetPostingN(itup, i),
											tupleOffset);
						itemIndex++;
					}
				}
			}
			/* 当 !continuescan 时不可能再有匹配，因此停止 */
			if (!continuescan)
				break;

			offnum = OffsetNumberNext(offnum);
		}

		/*
		 * 当高键表明右侧不会再找到匹配时，
		 * 就不需要访问右边那一页。
		 *
		 * 像这样检查高键，奏效的次数往往比你以为的更多。
		 * 叶子页分裂会在两个最不相似的元组之间选取分裂点
		 * （这要与均匀分享空闲空间的需要相权衡）。高键属性值
		 * 只能出现在右兄弟页的非枢轴元组上，这样的叶子页
		 * 很常见。
		 * 所以这些高键值往往只出现在右页。
		 */
		if (continuescan && !P_RIGHTMOST(opaque))
		{
			ItemId		iid = PageGetItemId(page, P_HIKEY);
			IndexTuple	itup = (IndexTuple) PageGetItem(page, iid);
			int			truncatt;

			truncatt = BTreeTupleGetNAtts(itup, scan->indexRelation);
			_bt_checkkeys(scan, itup, truncatt, dir, &continuescan);
		}

		if (!continuescan)
			so->currPos.moreRight = false;

		Assert(itemIndex <= MaxTIDsPerBTreePage);
		so->currPos.firstItem = 0;
		so->currPos.lastItem = itemIndex - 1;
		so->currPos.itemIndex = 0;
	}
	else
	{
		/* 按降序装入 items[] */
		itemIndex = MaxTIDsPerBTreePage;

		offnum = Min(offnum, maxoff);

		while (offnum >= minoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	itup;
			bool		tuple_alive;
			bool		passes_quals;

			/*
			 * 若扫描指定不返回已杀死的元组，则我们
			 * 把已杀死的元组当作未通过限定条件。大多数
			 * 时候，不去检查元组的索引
			 * 键、直接跳到下一个元组（实际上是前一个，
			 * 因为我们在反向扫描）是划算的。不过，若这是页上的第一个
			 * 元组，我们确实要检查索引键，以防止
			 * 无谓地前进到左边那一页。这类似于
			 * 正向扫描所用的高键优化。
			 */
			if (scan->ignore_killed_tuples && ItemIdIsDead(iid))
			{
				Assert(offnum >= P_FIRSTDATAKEY(opaque));
				if (offnum > P_FIRSTDATAKEY(opaque))
				{
					offnum = OffsetNumberPrev(offnum);
					continue;
				}

				tuple_alive = false;
			}
			else
				tuple_alive = true;

			itup = (IndexTuple) PageGetItem(page, iid);

			passes_quals = _bt_checkkeys(scan, itup, indnatts, dir,
										 &continuescan);
			if (passes_quals && tuple_alive)
			{
				/* 元组通过全部扫描键条件 */
				if (!BTreeTupleIsPosting(itup))
				{
					/* 记住它 */
					itemIndex--;
					_bt_saveitem(so, itemIndex, offnum, itup);
				}
				else
				{
					int			tupleOffset;

					/*
					 * 建立返回倒排列表的状态，并记住第一个
					 * TID。
					 *
					 * 注意，我们故意按堆 TID 升序保存/返回
					 * 倒排列表中的项，即使是反向
					 * 扫描也如此。这使 _bt_killitems() 可以对
					 * 与同一个倒排列表元组相关联的项的顺序
					 * 做出一致的假设。
					 */
					itemIndex--;
					tupleOffset =
						_bt_setuppostingitems(so, itemIndex, offnum,
											  BTreeTupleGetPostingN(itup, 0),
											  itup);
					/* 记住其余 TIDs */
					for (int i = 1; i < BTreeTupleGetNPosting(itup); i++)
					{
						itemIndex--;
						_bt_savepostingitem(so, itemIndex, offnum,
											BTreeTupleGetPostingN(itup, i),
											tupleOffset);
					}
				}
			}
			if (!continuescan)
			{
				/* 不可能再有匹配，因此停止 */
				so->currPos.moreLeft = false;
				break;
			}

			offnum = OffsetNumberPrev(offnum);
		}

		Assert(itemIndex >= 0);
		so->currPos.firstItem = itemIndex;
		so->currPos.lastItem = MaxTIDsPerBTreePage - 1;
		so->currPos.itemIndex = MaxTIDsPerBTreePage - 1;
	}

	return (so->currPos.firstItem <= so->currPos.lastItem);
}

/* 把一个索引项存入 so->currPos.items[itemIndex] */
static void
_bt_saveitem(BTScanOpaque so, int itemIndex,
			 OffsetNumber offnum, IndexTuple itup)
{
	BTScanPosItem *currItem = &so->currPos.items[itemIndex];

	Assert(!BTreeTupleIsPivot(itup) && !BTreeTupleIsPosting(itup));

	currItem->heapTid = itup->t_tid;
	currItem->indexOffset = offnum;
	if (so->currTuples)
	{
		Size		itupsz = IndexTupleSize(itup);

		currItem->tupleOffset = so->currPos.nextTupleOffset;
		memcpy(so->currTuples + so->currPos.nextTupleOffset, itup, itupsz);
		so->currPos.nextTupleOffset += MAXALIGN(itupsz);
	}
}

/*
 * 建立状态，以便保存来自单个倒排列表元组的 TIDs/项。
 *
 * 把首先返回给扫描的那个 TID 对应的索引项存入 so->currPos.items[itemIndex]。
 * 倒排列表的第二个及后续 TID 应当
 * 通过调用 _bt_savepostingitem() 来保存。
 *
 * 若需要，返回主元组在元组存储空间中
 * 的存放偏移。
 */
static int
_bt_setuppostingitems(BTScanOpaque so, int itemIndex, OffsetNumber offnum,
					  ItemPointer heapTid, IndexTuple itup)
{
	BTScanPosItem *currItem = &so->currPos.items[itemIndex];

	Assert(BTreeTupleIsPosting(itup));

	currItem->heapTid = *heapTid;
	currItem->indexOffset = offnum;
	if (so->currTuples)
	{
		/* 保存基本 IndexTuple（截掉倒排列表） */
		IndexTuple	base;
		Size		itupsz = BTreeTupleGetPostingOffset(itup);

		itupsz = MAXALIGN(itupsz);
		currItem->tupleOffset = so->currPos.nextTupleOffset;
		base = (IndexTuple) (so->currTuples + so->currPos.nextTupleOffset);
		memcpy(base, itup, itupsz);
		/* 防御性地缩小工作区索引元组的头部大小 */
		base->t_info &= ~INDEX_SIZE_MASK;
		base->t_info |= itupsz;
		so->currPos.nextTupleOffset += itupsz;

		return currItem->tupleOffset;
	}

	return 0;
}

/*
 * 把当前倒排
 * 元组对应的索引项存入 so->currPos.items[itemIndex]。
 *
 * 假定已经对当前
 * 倒排列表元组调用过 _bt_setuppostingitems()。调用者把它的返回值作为 tupleOffset 传入。
 */
static inline void
_bt_savepostingitem(BTScanOpaque so, int itemIndex, OffsetNumber offnum,
					ItemPointer heapTid, int tupleOffset)
{
	BTScanPosItem *currItem = &so->currPos.items[itemIndex];

	currItem->heapTid = *heapTid;
	currItem->indexOffset = offnum;

	/*
	 * 让仅索引扫描对来自同一倒排列表的每个 TID
	 * 都返回同一个基本 IndexTuple
	 */
	if (so->currTuples)
		currItem->tupleOffset = tupleOffset;
}

/*
 *	_bt_steppage() -- 走到扫描的下一张含有有效数据的页
 *
 * 进入时，若 so->currPos.buf 有效，则缓冲区已钉住但未加锁；
 * 若已钉住，我们会在移到下一页之前放下钉住。进入时缓冲区
 * 并未加锁。
 *
 * 对于使用非 MVCC 快照的扫描，成功时我们持有该页的钉住，但不持有
 * 读锁。若我们不持有钉住，则把 so->currPos.buf
 * 设为 InvalidBuffer。返回 true 表示成功。
 */
static bool
_bt_steppage(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	BlockNumber blkno = InvalidBlockNumber;
	bool		status;

	Assert(BTScanPosIsValid(so->currPos));

	/* 离开当前页之前，处理所有已杀死的项 */
	if (so->numKilled > 0)
		_bt_killitems(scan);

	/*
	 * 在修改 currPos 之前，若存在需要它的标记位置，
	 * 先复制一份页数据。
	 */
	if (so->markItemIndex >= 0)
	{
		/* 把当前缓冲区的钉住计数加一，以便赋给标记缓冲区 */
		if (BTScanPosIsPinned(so->currPos))
			IncrBufferRefCount(so->currPos.buf);
		memcpy(&so->markPos, &so->currPos,
			   offsetof(BTScanPosData, items[1]) +
			   so->currPos.lastItem * sizeof(BTScanPosItem));
		if (so->markTuples)
			memcpy(so->markTuples, so->currTuples,
				   so->currPos.nextTupleOffset);
		so->markPos.itemIndex = so->markItemIndex;
		so->markItemIndex = -1;
	}

	if (ScanDirectionIsForward(dir))
	{
		/* 向右走到下一张含有数据的页 */
		if (scan->parallel_scan != NULL)
		{
			/*
			 * 接管扫描以取得下一个块号；若扫描
			 * 已经结束，则退出。
			 */
			status = _bt_parallel_seize(scan, &blkno);
			if (!status)
			{
				/* 若仍钉住，释放前一个缓冲区 */
				BTScanPosUnpinIfPinned(so->currPos);
				BTScanPosInvalidate(so->currPos);
				return false;
			}
		}
		else
		{
			/* 不是并行扫描，因此使用此前保存的 nextPage 链接。 */
			blkno = so->currPos.nextPage;
		}

		/* 记住我们离开了一张含有数据的页 */
		so->currPos.moreLeft = true;

		/* 若仍钉住，释放前一个缓冲区 */
		BTScanPosUnpinIfPinned(so->currPos);
	}
	else
	{
		/* 记住我们离开了一张含有数据的页 */
		so->currPos.moreRight = true;

		if (scan->parallel_scan != NULL)
		{
			/*
			 * 接管扫描以取得当前块号；若扫描
			 * 已经结束，则退出。
			 */
			status = _bt_parallel_seize(scan, &blkno);
			BTScanPosUnpinIfPinned(so->currPos);
			if (!status)
			{
				BTScanPosInvalidate(so->currPos);
				return false;
			}
		}
		else
		{
			/* 不是并行扫描，因此直接使用我们自己记录的当前页 */
			blkno = so->currPos.currPage;
		}
	}

	if (!_bt_readnextpage(scan, blkno, dir))
		return false;

	/* 放下当前页上的锁，并视情况放下钉住 */
	_bt_drop_lock_and_maybe_pin(scan, &so->currPos);

	return true;
}

/*
 *	_bt_readnextpage() -- 读取扫描的下一张含有有效数据的页
 *
 * 成功退出时，so->currPos 被更新为包含下一张
 * 有意义的页的数据。成功时由调用者负责释放缓冲区上的锁和钉住。
 * 返回 true 表示成功。
 *
 * 若给定方向上不再有匹配项，我们放下全部
 * 锁和钉住，把 so->currPos.buf 设为 InvalidBuffer，并返回 false。
 */
static bool
_bt_readnextpage(IndexScanDesc scan, BlockNumber blkno, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Relation	rel;
	Page		page;
	BTPageOpaque opaque;
	bool		status;

	rel = scan->indexRelation;

	if (ScanDirectionIsForward(dir))
	{
		for (;;)
		{
			/*
			 * 若已到扫描末尾，放弃并把并行扫描标为
			 * 已完成，以便所有工作进程都能结束它们的扫描
			 */
			if (blkno == P_NONE || !so->currPos.moreRight)
			{
				_bt_parallel_done(scan);
				BTScanPosInvalidate(so->currPos);
				return false;
			}
			/* 在未持有任何缓冲区锁时检查中断 */
			CHECK_FOR_INTERRUPTS();
			/* 向右迈一页 */
			so->currPos.buf = _bt_getbuf(rel, blkno, BT_READ);
			page = BufferGetPage(so->currPos.buf);
			TestForOldSnapshot(scan->xs_snapshot, rel, page);
			opaque = BTPageGetOpaque(page);
			/* 检查是否为已删除页 */
			if (!P_IGNORE(opaque))
			{
				PredicateLockPage(rel, blkno, scan->xs_snapshot);
				/* 查看本页是否有任何匹配 */
				/* 注意：若可以停止，这会清除 moreRight */
				if (_bt_readpage(scan, dir, P_FIRSTDATAKEY(opaque)))
					break;
			}
			else if (scan->parallel_scan != NULL)
			{
				/* 允许并行工作进程处理下一页 */
				_bt_parallel_release(scan, opaque->btpo_next);
			}

			/* 没有，继续往下走 */
			if (scan->parallel_scan != NULL)
			{
				_bt_relbuf(rel, so->currPos.buf);
				status = _bt_parallel_seize(scan, &blkno);
				if (!status)
				{
					BTScanPosInvalidate(so->currPos);
					return false;
				}
			}
			else
			{
				blkno = opaque->btpo_next;
				_bt_relbuf(rel, so->currPos.buf);
			}
		}
	}
	else
	{
		/*
		 * 这只应发生在并行情形下，当某个其他后端
		 * 已经推进了扫描。
		 */
		if (so->currPos.currPage != blkno)
		{
			BTScanPosUnpinIfPinned(so->currPos);
			so->currPos.currPage = blkno;
		}

		/*
		 * 向左走到下一张含有数据的页。这比向右走的情形
		 * 复杂得多，因为我们在赶往左页的途中，它可能
		 * 发生分裂，而且我们离开后，原来所在的页
		 * 也可能被删除。
		 * 详见 nbtree/README。
		 *
		 * 也许可以重排这段代码，以减少
		 * 钉住和加锁的开销，但那就需要在最初读页时捕获左
		 * 指针，并在这里使用它，同时还要对 _bt_walk_left() 和下面的代码做
		 * 大改。并不清楚这是否划算，因为若紧邻左侧的页
		 * 在我们读到本页之后、向左迈步之前发生分裂，
		 * 我们就要多走几页，
		 * 比按当前代码走的还多。
		 *
		 * 注意，若我们改动代码，使使用非 MVCC 快照的扫描也放下钉住，
		 * 就需要修改向左走的代码，
		 * 以允许被引用的页
		 * 已经被删除。只要缓冲区仍被钉住，或者快照
		 * 是 MVCC，该页就无法越过半死状态而变成完全
		 * 删除。
		 */
		if (BTScanPosIsPinned(so->currPos))
			_bt_lockbuf(rel, so->currPos.buf, BT_READ);
		else
			so->currPos.buf = _bt_getbuf(rel, so->currPos.currPage, BT_READ);

		for (;;)
		{
			/* 若已知左侧没有匹配键，则结束 */
			if (!so->currPos.moreLeft)
			{
				_bt_relbuf(rel, so->currPos.buf);
				_bt_parallel_done(scan);
				BTScanPosInvalidate(so->currPos);
				return false;
			}

			/* 走到下一张物理页 */
			so->currPos.buf = _bt_walk_left(rel, so->currPos.buf,
											scan->xs_snapshot);

			/* 若物理上已到索引末尾，返回失败 */
			if (so->currPos.buf == InvalidBuffer)
			{
				_bt_parallel_done(scan);
				BTScanPosInvalidate(so->currPos);
				return false;
			}

			/*
			 * 好，我们成功向左移到了一张未删除页。若它
			 * 不是半死且包含匹配元组，则结束。否则回到循环
			 * 再做一遍。
			 */
			page = BufferGetPage(so->currPos.buf);
			TestForOldSnapshot(scan->xs_snapshot, rel, page);
			opaque = BTPageGetOpaque(page);
			if (!P_IGNORE(opaque))
			{
				PredicateLockPage(rel, BufferGetBlockNumber(so->currPos.buf), scan->xs_snapshot);
				/* 查看本页是否有任何匹配 */
				/* 注意：若可以停止，这会清除 moreLeft */
				if (_bt_readpage(scan, dir, PageGetMaxOffsetNumber(page)))
					break;
			}
			else if (scan->parallel_scan != NULL)
			{
				/* 允许并行工作进程处理下一页 */
				_bt_parallel_release(scan, BufferGetBlockNumber(so->currPos.buf));
			}

			/*
			 * 对并行扫描，取最近扫描过的页，因为
			 * 等到我们尝试接管扫描时，很有可能某个其他
			 * 工作进程已经把扫描推进到了另一页。我们必须
			 * 根据任一工作进程最近扫描的页继续。
			 */
			if (scan->parallel_scan != NULL)
			{
				_bt_relbuf(rel, so->currPos.buf);
				status = _bt_parallel_seize(scan, &blkno);
				if (!status)
				{
					BTScanPosInvalidate(so->currPos);
					return false;
				}
				so->currPos.buf = _bt_getbuf(rel, blkno, BT_READ);
			}
		}
	}

	return true;
}

/*
 *	_bt_parallel_readpage() -- 读取扫描当前含有有效数据的页
 *
 * 成功时，释放缓冲区上的锁，并视情况放下钉住。返回 true
 * 表示成功。
 */
static bool
_bt_parallel_readpage(IndexScanDesc scan, BlockNumber blkno, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	_bt_initialize_more_data(so, dir);

	if (!_bt_readnextpage(scan, blkno, dir))
		return false;

	/* 放下当前页上的锁，并视情况放下钉住 */
	_bt_drop_lock_and_maybe_pin(scan, &so->currPos);

	return true;
}

/*
 * _bt_walk_left() -- 若可能，向左迈一页
 *
 * 给定缓冲区必须已钉住并读锁定。向左迈步之前会放下
 * 它们。返回时，钉住和读锁改为加在
 * 返回的页上。
 *
 * 若左侧没有页，则返回 InvalidBuffer（那种情况下
 * 不持有锁）。
 *
 * 在非叶子层上工作时，返回的页有可能是
 * 半死的；若这一点重要，调用者应当检查该条件并再次
 * 向左迈步。
 */
static Buffer
_bt_walk_left(Relation rel, Buffer buf, Snapshot snapshot)
{
	Page		page;
	BTPageOpaque opaque;

	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);

	for (;;)
	{
		BlockNumber obknum;
		BlockNumber lblkno;
		BlockNumber blkno;
		int			tries;

		/* 若已到树的末端，释放 buf 并返回失败 */
		if (P_LEFTMOST(opaque))
		{
			_bt_relbuf(rel, buf);
			break;
		}
		/* 记住我们向左迈出时所在的原页 */
		obknum = BufferGetBlockNumber(buf);
		/* 向左迈一步 */
		blkno = lblkno = opaque->btpo_prev;
		_bt_relbuf(rel, buf);
		/* 在未持有任何缓冲区锁时检查中断 */
		CHECK_FOR_INTERRUPTS();
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		TestForOldSnapshot(snapshot, rel, page);
		opaque = BTPageGetOpaque(page);

		/*
		 * 若这不是我们想要的页，向右走直到找到
		 * 目标——但最多四跳（一个任意上限）。若到那时
		 * 仍找不到正确的页，最可能的情况是
		 * 原页已被删除，根本不再位于兄弟链中，
		 * 而不是它的左兄弟分裂了超过四次。
		 *
		 * 注意，这里测试 P_ISDELETED 而不是 P_IGNORE 是正确的，
		 * 因为半死页仍在兄弟链中。若需要，调用者
		 * 必须拒绝半死页。
		 */
		tries = 0;
		for (;;)
		{
			if (!P_ISDELETED(opaque) && opaque->btpo_next == obknum)
			{
				/* 找到目标页，将其返回 */
				return buf;
			}
			if (P_RIGHTMOST(opaque) || ++tries > 4)
				break;
			blkno = opaque->btpo_next;
			buf = _bt_relandgetbuf(rel, buf, blkno, BT_READ);
			page = BufferGetPage(buf);
			TestForOldSnapshot(snapshot, rel, page);
			opaque = BTPageGetOpaque(page);
		}

		/* 回到原页，看看发生了什么 */
		buf = _bt_relandgetbuf(rel, buf, obknum, BT_READ);
		page = BufferGetPage(buf);
		TestForOldSnapshot(snapshot, rel, page);
		opaque = BTPageGetOpaque(page);
		if (P_ISDELETED(opaque))
		{
			/*
			 * 它已被删除。向右移到第一张未删除页（必定
			 * 有一张）；那一页已经获得被删除页的
			 * 键空间，因此从它向左走会把我们带到
			 * 想去的地方。
			 */
			for (;;)
			{
				if (P_RIGHTMOST(opaque))
					elog(ERROR, "fell off the end of index \"%s\"",
						 RelationGetRelationName(rel));
				blkno = opaque->btpo_next;
				buf = _bt_relandgetbuf(rel, buf, blkno, BT_READ);
				page = BufferGetPage(buf);
				TestForOldSnapshot(snapshot, rel, page);
				opaque = BTPageGetOpaque(page);
				if (!P_ISDELETED(opaque))
					break;
			}

			/*
			 * 现在回到循环顶部，把 obknum 重置为指向这张
			 * 未删除页，然后重试。
			 */
		}
		else
		{
			/*
			 * 它并未被删除；合理的解释应当是左侧那一页
			 * 发生了分裂或被删除。若没有这项检查，一旦出了问题，
			 * 我们就会进入无限循环。
			 */
			if (opaque->btpo_prev == lblkno)
				elog(ERROR, "could not find left sibling of block %u in index \"%s\"",
					 obknum, RelationGetRelationName(rel));
			/* 可以用新的 lblkno 值再查找一次 */
		}
	}

	return InvalidBuffer;
}

/*
 * _bt_get_endpoint() -- 找出给定树层上的第一页或最后一页
 *
 * 若索引为空，我们将返回 InvalidBuffer；任何其他失败
 * 条件都会引起 ereport()。我们不会返回死页。
 *
 * 返回的缓冲区已钉住并读锁定。
 */
Buffer
_bt_get_endpoint(Relation rel, uint32 level, bool rightmost,
				 Snapshot snapshot)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber offnum;
	BlockNumber blkno;
	IndexTuple	itup;

	/*
	 * 若我们要找的是叶子页，从快速根下降即可；
	 * 否则最好从真正的根下降。（没有必要对
	 * 中间层更聪明。）
	 */
	if (level == 0)
		buf = _bt_getroot(rel, NULL, BT_READ);
	else
		buf = _bt_gettrueroot(rel);

	if (!BufferIsValid(buf))
		return InvalidBuffer;

	page = BufferGetPage(buf);
	TestForOldSnapshot(snapshot, rel, page);
	opaque = BTPageGetOpaque(page);

	for (;;)
	{
		/*
		 * 若落在一张已删除页上，向右走以找到一张活页
		 * （必定有一张）。另外，若我们要的是最右页，必要时向右走
		 * 以到达它（若自我们取得指向它的指针以来该页发生了分裂，
		 * 就会出现这种情况）。
		 */
		while (P_IGNORE(opaque) ||
			   (rightmost && !P_RIGHTMOST(opaque)))
		{
			blkno = opaque->btpo_next;
			if (blkno == P_NONE)
				elog(ERROR, "fell off the end of index \"%s\"",
					 RelationGetRelationName(rel));
			buf = _bt_relandgetbuf(rel, buf, blkno, BT_READ);
			page = BufferGetPage(buf);
			TestForOldSnapshot(snapshot, rel, page);
			opaque = BTPageGetOpaque(page);
		}

		/* 完成了? */
		if (opaque->btpo_level == level)
			break;
		if (opaque->btpo_level < level)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("btree level %u not found in index \"%s\"",
									 level, RelationGetRelationName(rel))));

		/* 下降到最左或最右子页 */
		if (rightmost)
			offnum = PageGetMaxOffsetNumber(page);
		else
			offnum = P_FIRSTDATAKEY(opaque);

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
		blkno = BTreeTupleGetDownLink(itup);

		buf = _bt_relandgetbuf(rel, buf, blkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);
	}

	return buf;
}

/*
 *	_bt_endpoint() -- 找出索引中的第一页或最后一页，并从那里
 * 扫描到满足全部限定条件的第一个键。
 *
 * _bt_first() 在确定扫描必须从索引的开头或末尾开始时
 * （分别对应正向或反向扫描）用它来建立扫描。
 * 退出条件与 _bt_first()
 * 相同。
 */
static bool
_bt_endpoint(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	OffsetNumber start;
	BTScanPosItem *currItem;

	/*
	 * 向下扫描到最左或最右的叶子页。这是 _bt_search() 的简化
	 * 版本。我们知道不会再用到栈，
	 * 因此不维护它。
	 */
	buf = _bt_get_endpoint(rel, 0, ScanDirectionIsBackward(dir), scan->xs_snapshot);

	if (!BufferIsValid(buf))
	{
		/*
		 * 空索引。锁定整个关系，因为不存在更细的
		 * 可锁对象。
		 */
		PredicateLockRelation(rel, scan->xs_snapshot);
		BTScanPosInvalidate(so->currPos);
		return false;
	}

	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);
	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	Assert(P_ISLEAF(opaque));

	if (ScanDirectionIsForward(dir))
	{
		/* 左侧可能还有死页，因此不要这样： */
		/* Assert(P_LEFTMOST(opaque)); 见上一行 */

		start = P_FIRSTDATAKEY(opaque);
	}
	else if (ScanDirectionIsBackward(dir))
	{
		Assert(P_RIGHTMOST(opaque));

		start = PageGetMaxOffsetNumber(page);
	}
	else
	{
		elog(ERROR, "invalid scan direction: %d", (int) dir);
		start = 0;				/* 让编译器保持安静 */
	}

	/* 记住我们钉住了哪个缓冲区 */
	so->currPos.buf = buf;

	_bt_initialize_more_data(so, dir);

	/*
	 * 现在从扫描的第一页装入数据。
	 */
	if (!_bt_readpage(scan, dir, start))
	{
		/*
		 * 本页上没有真正匹配的数据。尝试前进到
		 * 下一页。若完全没有匹配数据，则返回 false。
		 */
		_bt_unlockbuf(scan->indexRelation, so->currPos.buf);
		if (!_bt_steppage(scan, dir))
			return false;
	}
	else
	{
		/* 放下当前页上的锁，并视情况放下钉住 */
		_bt_drop_lock_and_maybe_pin(scan, &so->currPos);
	}

	/* 好，itemIndex 指出该返回哪一项 */
	currItem = &so->currPos.items[so->currPos.itemIndex];
	scan->xs_heaptid = currItem->heapTid;
	if (scan->xs_want_itup)
		scan->xs_itup = (IndexTuple) (so->currTuples + currItem->tupleOffset);

	return true;
}

/*
 * _bt_initialize_more_data() -- 按扫描方向
 * 正确初始化 moreLeft/moreRight
 */
static inline void
_bt_initialize_more_data(BTScanOpaque so, ScanDirection dir)
{
	/* 按扫描方向正确初始化 moreLeft/moreRight */
	if (ScanDirectionIsForward(dir))
	{
		so->currPos.moreLeft = false;
		so->currPos.moreRight = true;
	}
	else
	{
		so->currPos.moreLeft = true;
		so->currPos.moreRight = false;
	}
	so->numKilled = 0;			/* 只是出于谨慎 */
	so->markItemIndex = -1;		/* 同上 */
}
