/*-------------------------------------------------------------------------
 *
 * nbtinsert.c
 *	  Postgres 中 Lehman 与 Yao B-树的项插入。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "common/pg_prng.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"

/* fastpath 优化适用的最小树高 */
#define BTREE_FASTPATH_MIN_LEVEL	2


static BTStack _bt_search_insert(Relation rel, Relation heaprel,
								 BTInsertState insertstate);
static TransactionId _bt_check_unique(Relation rel, BTInsertState insertstate,
									  Relation heapRel,
									  IndexUniqueCheck checkUnique, bool *is_unique,
									  uint32 *speculativeToken);
static OffsetNumber _bt_findinsertloc(Relation rel,
									  BTInsertState insertstate,
									  bool checkingunique,
									  bool indexUnchanged,
									  BTStack stack,
									  Relation heapRel);
static void _bt_stepright(Relation rel, Relation heaprel,
						  BTInsertState insertstate, BTStack stack);
static void _bt_insertonpg(Relation rel, Relation heaprel, BTScanInsert itup_key,
						   Buffer buf,
						   Buffer cbuf,
						   BTStack stack,
						   IndexTuple itup,
						   Size itemsz,
						   OffsetNumber newitemoff,
						   int postingoff,
						   bool split_only_page);
static Buffer _bt_split(Relation rel, Relation heaprel, BTScanInsert itup_key,
						Buffer buf, Buffer cbuf, OffsetNumber newitemoff,
						Size newitemsz, IndexTuple newitem, IndexTuple orignewitem,
						IndexTuple nposting, uint16 postingoff);
static void _bt_insert_parent(Relation rel, Relation heaprel, Buffer buf,
							  Buffer rbuf, BTStack stack, bool isroot, bool isonly);
static Buffer _bt_newlevel(Relation rel, Relation heaprel, Buffer lbuf, Buffer rbuf);
static inline bool _bt_pgaddtup(Page page, Size itemsize, IndexTuple itup,
								OffsetNumber itup_off, bool newfirstdataitem);
static void _bt_delete_or_dedup_one_page(Relation rel, Relation heapRel,
										 BTInsertState insertstate,
										 bool simpleonly, bool checkingunique,
										 bool uniquedup, bool indexUnchanged);
static void _bt_simpledel_pass(Relation rel, Buffer buffer, Relation heapRel,
							   OffsetNumber *deletable, int ndeletable,
							   IndexTuple newitem, OffsetNumber minoff,
							   OffsetNumber maxoff);
static BlockNumber *_bt_deadblocks(Page page, OffsetNumber *deletable,
								   int ndeletable, IndexTuple newitem,
								   int *nblocks);
static inline int _bt_blk_cmp(const void *arg1, const void *arg2);

/*
 *	_bt_doinsert() -- 处理向树中插入单个索引元组。
 *
 *		本例程由公开接口例程 btinsert 调用。
 *		到这里时，itup 已填好，包括 TID。
 *
 *		若 checkUnique 为 UNIQUE_CHECK_NO 或 UNIQUE_CHECK_PARTIAL，则
 *		允许重复项。否则（UNIQUE_CHECK_YES 或
 *		UNIQUE_CHECK_EXISTING）遇到重复项会抛出错误。
 *		对于 UNIQUE_CHECK_EXISTING，我们只做重复项检查，
 *		并不真正插入。
 *
 *		indexUnchanged 执行器提示表明 itup 来自一次
 *		并未从逻辑上改变索引值的 UPDATE，但
 *		仍必须有一条新索引项，指向后继
 *		版本。
 *
 *		返回值只对 UNIQUE_CHECK_PARTIAL 有意义：
 *		若已知该项唯一则为 true，否则为 false。
 *		（当前实现在 UNIQUE_CHECK_YES 或 UNIQUE_CHECK_EXISTING
 *		调用成功后也会返回 true，但
 *		那只是编码上的附带结果。）
 */
bool
_bt_doinsert(Relation rel, IndexTuple itup,
			 IndexUniqueCheck checkUnique, bool indexUnchanged,
			 Relation heapRel)
{
	bool		is_unique = false;
	BTInsertStateData insertstate;
	BTScanInsert itup_key;
	BTStack		stack;
	bool		checkingunique = (checkUnique != UNIQUE_CHECK_NO);

	/* 搜索需要插入型扫描键，因此构造一个 */
	itup_key = _bt_mkscankey(rel, itup);

	if (checkingunique)
	{
		if (!itup_key->anynullkeys)
		{
			/* 在确立唯一性之前不设置（heapkeyspace）scantid */
			itup_key->scantid = NULL;
		}
		else
		{
			/*
			 * 新元组的扫描键含有 NULL 键值。跳过
			 * checkingunique 步骤。这些步骤没有必要，因为核心代码
			 * 认为 NULL 与任何值都不相等，包括 NULL。
			 *
			 * 当唯一索引含有大量键值为 NULL 的"重复项"时，
			 * 这一优化避免 _bt_findinsertloc() 的 heapkeyspace 路径
			 * 出现 O(N^2) 行为。
			 */
			checkingunique = false;
			/* 就核心代码所关心的意义而言，该元组是唯一的 */
			Assert(checkUnique != UNIQUE_CHECK_EXISTING);
			is_unique = true;
		}
	}

	/*
	 * 填写 BTInsertState 工作区，以跟踪当前页以及
	 * 页内要插入的位置。
	 *
	 * 注意，itemsz 会传给下层负责
	 * 插入该项的代码。它必须经过 MAXALIGN()。这样空间
	 * 记账代码就会始终计入我们
	 * 预期 PageAddItem() 稍后会加上的对齐开销。（实际上 index_form_tuple()
	 * 在对齐上已经偏保守，但隔着这么远我们不依赖这一点。
	 * 此外，将来也许会为了 nbtsplitloc.c 而在索引
	 * 元组头里保留"真实"元组大小。
	 * 注意，heapam 并不对每个堆元组的 lp_len 字段做 MAXALIGN()。）
	 */
	insertstate.itup = itup;
	insertstate.itemsz = MAXALIGN(IndexTupleSize(itup));
	insertstate.itup_key = itup_key;
	insertstate.bounds_valid = false;
	insertstate.buf = InvalidBuffer;
	insertstate.postingoff = 0;

search:

	/*
	 * 从根页开始搜索，找到并锁定元组应当被加入的叶子页。
	 * 此后 insertstate.buf 将持有一个
	 * 以排他模式锁定的缓冲区。
	 */
	stack = _bt_search_insert(rel, heapRel, &insertstate);

	/*
	 * 当两个键属性值相等的元组在事务提交后
	 * 会对新的 MVCC 快照可见时，checkingunique 插入不允许继续。
	 * 在这里检查已锁定页/缓冲区中的冲突（若
	 * 需要）。
	 *
	 * 也许有必要在 _bt_check_unique 中再检查右边的一页，
	 * 不过这种情况应当非常少见。实践中，该值
	 * 可能位于的第一页（省略 scantid）几乎总是
	 * 同时也是唯一可能找到匹配元组的那一页。这是由于
	 * _bt_findsplitloc 对重复元组的行为——一组重复项
	 * 只有在没有任何候选叶子页分裂点
	 * 能够避开时，才允许跨越页边界。另外，
	 * _bt_check_unique 可以利用叶子页高键判定
	 * 右兄弟上不会有重复项，
	 * 而无需真正访问它（当新项恰好属于叶子页
	 * 最右侧时，它会使用高键）。
	 *
	 * 注意：显然，_bt_check_unique 只能发现已经
	 * 在索引中的键，因此它防不住对同一键的
	 * 并发插入。我们的防护手段是：对
	 * 该值可能位于的第一页持有写锁，隐式堆 TID 决胜属性
	 * 取省略值/负无穷。任何其他想要插入
	 * 同一键的插入者都必须获取同一页的写锁，因此同一时刻只有一个
	 * 潜在插入者能在做这项检查。此外，
	 * 一旦通过检查，我们就持续持有写锁，直到
	 * 完成插入，后来的插入者不可能看不见我们的
	 * 插入。（这需要在 _bt_findinsertloc 中多加小心。）
	 *
	 * 若必须等待另一个事务，我们在等待期间释放锁，
	 * 然后必须重新搜索。
	 *
	 * 对于部分唯一性检查，我们不等待另一个事务。直接
	 * 让元组进入，若可能不唯一则返回 false，若
	 * 确定唯一则返回 true。
	 */
	if (checkingunique)
	{
		TransactionId xwait;
		uint32		speculativeToken;

		xwait = _bt_check_unique(rel, &insertstate, heapRel, checkUnique,
								 &is_unique, &speculativeToken);

		if (unlikely(TransactionIdIsValid(xwait)))
		{
			/* 必须等待对方 ... */
			_bt_relbuf(rel, insertstate.buf);
			insertstate.buf = InvalidBuffer;

			/*
			 * 若这是推测性插入，则等待它结束（也就是
			 * 继续完成插入，或杀死该元组）。否则
			 * 像往常一样等待事务结束。
			 */
			if (speculativeToken)
				SpeculativeInsertionWait(xwait, speculativeToken);
			else
				XactLockTableWait(xwait, rel, &itup->t_tid, XLTW_InsertIndex);

			/* 重新开始... */
			if (stack)
				_bt_freestack(stack);
			goto search;
		}

		/* 唯一性已确立 -- 把堆 tid 恢复为 scantid */
		if (itup_key->heapkeyspace)
			itup_key->scantid = &itup->t_tid;
	}

	if (checkUnique != UNIQUE_CHECK_EXISTING)
	{
		OffsetNumber newitemoff;

		/*
		 * 对索引而言，谓词锁唯一关心的冲突是
		 * 索引元组插入与已有锁冲突。在
		 * checkingunique 和 !heapkeyspace 情形下，我们还不能确定
		 * 实际要插入的那一页，但可以用该值可能位于的
		 * 第一页（省略 scantid）来代替。
		 */
		CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(insertstate.buf));

		/*
		 * 执行插入。注意，当插入处于
		 * checkingunique 时，insertstate 含有 _bt_check_unique 中建立的
		 * 缓存二分查找边界。
		 */
		newitemoff = _bt_findinsertloc(rel, &insertstate, checkingunique,
									   indexUnchanged, stack, heapRel);
		_bt_insertonpg(rel, heapRel, itup_key, insertstate.buf, InvalidBuffer,
					   stack, itup, insertstate.itemsz, newitemoff,
					   insertstate.postingoff, false);
	}
	else
	{
		/* 只释放缓冲区 */
		_bt_relbuf(rel, insertstate.buf);
	}

	/* 保持整洁 */
	if (stack)
		_bt_freestack(stack);
	pfree(itup_key);

	return is_unique;
}

/*
 *	_bt_search_insert() -- 供插入使用的 _bt_search() 包装
 *
 * 在树中搜索某个特定扫描键，更准确地说，是搜索它
 * 可能位于的第一张叶子页。不过，在真正从根页
 * 搜索整棵树之前，先尝试利用 fastpath 优化的
 * 最右叶子页缓存。
 *
 * 返回值是父页指针栈（不过请看下面关于
 * fastpath 优化和页分裂的说明）。insertstate->buf 被设为
 * 叶子页缓冲区的地址；该缓冲区在所有情况下都已加写锁并钉住
 * （必要时会为调用者创建一个新的空根页）。
 *
 * 当单个后端在索引的最右叶子页上
 * 连续插入新元组时，fastpath 优化避免了反复
 * 搜索树的大部分工作。最右叶子页的后端缓存
 * 在 _bt_insertonpg() 内维护，并在这里使用。当非枢轴元组
 * 必须插入到非最右叶子页上时，这里会使该缓存
 * 失效。
 *
 * 该优化对自增列上的索引有帮助。它对
 * 日期时间列上的索引也有帮助，以及对含有大量
 * NULL 值的索引也是如此。（对单
 * 列索引，NULLs 通常被插到最右页，因为它们通常被当作排在键空间中
 * 其他一切之后。受堆 TID 列的影响，单个 NULL 元组一般会被放在
 * 最右叶子页上。）
 *
 * 注意，当最右页没有足够
 * 空间放下调用者的新项时，我们避免应用该优化。这是必要的，
 * 因为预期会发生页分裂时，我们必须返回真正的下降栈
 * （实际上，调用者能应付使用 NULL
 * 栈的叶子页分裂，但那非常慢，因此必须避免）。还要注意，
 * fastpath 优化有条件地获取该页的锁，以此
 * 减少对最右页并发插入时的额外争用
 * （若必须等待锁，我们就放弃）。我们假定
 * 存在争用时应用该优化并无益处，
 * 因为每个后端自己的缓存不会长期有效。
 */
static BTStack
_bt_search_insert(Relation rel, Relation heaprel, BTInsertState insertstate)
{
	Assert(insertstate->buf == InvalidBuffer);
	Assert(!insertstate->bounds_valid);
	Assert(insertstate->postingoff == 0);

	if (RelationGetTargetBlock(rel) != InvalidBlockNumber)
	{
		/* 模拟一次带条件加锁的 _bt_getbuf() 调用 */
		insertstate->buf = ReadBuffer(rel, RelationGetTargetBlock(rel));
		if (_bt_conditionallockbuf(rel, insertstate->buf))
		{
			Page		page;
			BTPageOpaque opaque;

			_bt_checkpage(rel, insertstate->buf);
			page = BufferGetPage(insertstate->buf);
			opaque = BTPageGetOpaque(page);

			/*
			 * 检查该页是否仍是最右叶子页，并且
			 * 有足够空闲空间容纳新元组。同时检查
			 * 插入扫描键是否严格大于该页上第一个
			 * 非枢轴元组。（注意，当调用者是 checkingunique
			 * 插入者时，我们预期 itup_key 的
			 * scantid 尚未设置。）
			 */
			if (P_RIGHTMOST(opaque) &&
				P_ISLEAF(opaque) &&
				!P_IGNORE(opaque) &&
				PageGetFreeSpace(page) > insertstate->itemsz &&
				PageGetMaxOffsetNumber(page) >= P_HIKEY &&
				_bt_compare(rel, insertstate->itup_key, page, P_HIKEY) > 0)
			{
				/*
				 * 调用者可以使用 fastpath 优化，因为缓存的
				 * 块仍是最右叶子页，并且能放下调用者的
				 * 新元组而无需分裂。把该块留在本地缓存中供
				 * 下一次插入使用，并让调用者使用 NULL 栈。
				 *
				 * 注意，_bt_insert_parent() 有一条断言，用来抓住
				 * 不知何故由 fastpath 插入引出的叶子页分裂
				 * （只有在必须处理
				 * 并发根页分裂时才应向它传入 NULL 栈，绝不是因为这里
				 * 返回了 NULL 栈）。
				 */
				return NULL;
			}

			/* 该页不适合调用者，放下锁和钉住 */
			_bt_relbuf(rel, insertstate->buf);
		}
		else
		{
			/* 锁不可用，放下钉住 */
			ReleaseBuffer(insertstate->buf);
		}

		/* 忘掉该块，因为缓存看起来并无用处 */
		RelationSetTargetBlock(rel, InvalidBlockNumber);
	}

	/* 无法使用该优化 -- 沿树下降，返回正规的下降栈 */
	return _bt_search(rel, heaprel, insertstate->itup_key, &insertstate->buf,
					  BT_WRITE, NULL);
}

/*
 *	_bt_check_unique() -- 检查是否违反唯一索引约束
 *
 * 若没有冲突则返回 InvalidTransactionId，否则返回一个事务 ID，
 * 我们必须等待它，看它是否提交了冲突元组。若检测到实际
 * 冲突，则不返回 --- 直接 ereport()。若返回了事务 ID，
 * 且冲突元组仍有进行中的推测性插入，
 * 则把 *speculativeToken 设为非零，调用者可以用
 * SpeculativeInsertionWait() 等待该插入的结论。
 *
 * 不过，若 checkUnique == UNIQUE_CHECK_PARTIAL，我们总是返回
 * InvalidTransactionId，因为我们不想等待。此时若存在潜在冲突，
 * 我们把 *is_unique 设为 false，
 * 核心代码必须稍后重新做唯一性检查。
 *
 * 作为副作用，会在 insertstate 中设置状态，供稍后
 * _bt_findinsertloc() 复用我们在这里所做的
 * 大部分二分查找工作。
 *
 * 这段代码把 NULLs 当作相等，这与唯一
 * 索引的默认语义不同。因此当扫描键中有 NULL 值且
 * 索引使用默认的 NULLS DISTINCT 模式时，不要调用这里。
 */
static TransactionId
_bt_check_unique(Relation rel, BTInsertState insertstate, Relation heapRel,
				 IndexUniqueCheck checkUnique, bool *is_unique,
				 uint32 *speculativeToken)
{
	IndexTuple	itup = insertstate->itup;
	IndexTuple	curitup = NULL;
	ItemId		curitemid = NULL;
	BTScanInsert itup_key = insertstate->itup_key;
	SnapshotData SnapshotDirty;
	OffsetNumber offset;
	OffsetNumber maxoff;
	Page		page;
	BTPageOpaque opaque;
	Buffer		nbuf = InvalidBuffer;
	bool		found = false;
	bool		inposting = false;
	bool		prevalldead = true;
	int			curposti = 0;

	/* 在找到重复项之前假定唯一 */
	*is_unique = true;

	InitDirtySnapshot(SnapshotDirty);

	page = BufferGetPage(insertstate->buf);
	opaque = BTPageGetOpaque(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * 找到第一个具有相同键的元组。
	 *
	 * 这也会把二分查找边界保存在 insertstate 中。下面的 fastpath 会用到它们，
	 * 稍后调用 _bt_findinsertloc() 时也会用到。
	 */
	Assert(!insertstate->bounds_valid);
	offset = _bt_binsrch_insert(rel, insertstate);

	/*
	 * 扫描所有相等元组，寻找仍然存活的冲突。
	 */
	Assert(!insertstate->bounds_valid || insertstate->low == offset);
	Assert(!itup_key->anynullkeys);
	Assert(itup_key->scantid == NULL);
	for (;;)
	{
		/*
		 * 循环的每次迭代处理一个堆 TID，而不是一个索引
		 * 元组。处理倒排列表元组中的堆 TIDs 时，通常不会推进
		 * 页的当前偏移号。
		 *
		 * 处于倒排列表 _inside_ 时设置 "inposting" 状态 --- 而不是
		 * 位于倒排列表的起点（或终点）时。在倒排列表元组内部，
		 * 我们在迭代结束时推进 curposti。一般而言，
		 * 每次循环迭代要么推进页偏移，
		 * 要么推进 curposti --- 处理倒排列表中最右/最大
		 * 堆 TID 的那次迭代最终会推进页偏移（并
		 * 清除 "inposting"）。
		 *
		 * 在试图检查之前，先确认偏移指向一个真正的索引元组
		 * ...
		 */
		if (offset <= maxoff)
		{
			/*
			 * Fastpath：多数情况下，我们可以用缓存的搜索边界，
			 * 把考虑范围限制为肯定是
			 * 重复项的那些项。当原始页
			 * 为空，或初始偏移已超过
			 * 原始页末尾时，这条 fastpath 不适用；那可能表示我们需要检查
			 * 第二页或后续页。
			 *
			 * 注意，只要键将要放入的偏移不在页尾，
			 * 这一优化就允许我们在没有重复项时避免直接调用
			 * _bt_compare()。
			 */
			if (nbuf == InvalidBuffer && offset == insertstate->stricthigh)
			{
				Assert(insertstate->bounds_valid);
				Assert(insertstate->low >= P_FIRSTDATAKEY(opaque));
				Assert(insertstate->low <= insertstate->stricthigh);
				Assert(_bt_compare(rel, itup_key, page, offset) < 0);
				break;
			}

			/*
			 * 我们可以跳过已经被标记为杀死的项。
			 *
			 * 更新非常频繁时，索引里可能有
			 * 许多键相同的已杀死项；对
			 * 每个已杀死项调用 _bt_compare() 会很贵。尽快跳过已杀死
			 * 项即可。我们只在
			 * 遇到未杀死的项时才调用 _bt_compare()。本来可以复用边界，
			 * 以避免对已知相等的元组调用 _bt_compare()，但
			 * 似乎并不值得。
			 */
			if (!inposting)
				curitemid = PageGetItemId(page, offset);
			if (inposting || !ItemIdIsDead(curitemid))
			{
				ItemPointerData htid;
				bool		all_dead = false;

				if (!inposting)
				{
					/* 普通元组，或倒排列表元组中的第一个 TID */
					if (_bt_compare(rel, itup_key, page, offset) != 0)
						break;	/* 已经越过所有相等元组 */

					/* 已推进 curitup */
					curitup = (IndexTuple) PageGetItem(page, curitemid);
					Assert(!BTreeTupleIsPivot(curitup));
				}

				/* 好，我们得用 htid 去取堆元组 ... */
				if (!BTreeTupleIsPosting(curitup))
				{
					/* ... htid 来自简单的非枢轴元组 */
					Assert(!inposting);
					htid = curitup->t_tid;
				}
				else if (!inposting)
				{
					/* ... htid 是新倒排列表中的第一个 TID */
					inposting = true;
					prevalldead = true;
					curposti = 0;
					htid = *BTreeTupleGetPostingN(curitup, 0);
				}
				else
				{
					/* ... htid 是倒排列表中的第二个或后续 TID */
					Assert(curposti > 0);
					htid = *BTreeTupleGetPostingN(curitup, curposti);
				}

				/*
				 * 若我们正在复查，则预期会找到正在
				 * 复查的那个元组。它不是重复项，但我们必须继续
				 * 扫描。
				 */
				if (checkUnique == UNIQUE_CHECK_EXISTING &&
					ItemPointerCompare(&htid, &itup->t_tid) == 0)
				{
					found = true;
				}

				/*
				 * 检查该索引项是否有任何表元组
				 * 满足 SnapshotDirty。这是必要的，因为对于
				 * 带有堆 HOT 一类优化的访问方法，整条链
				 * 只有一条索引项。
				 */
				else if (table_index_fetch_tuple_check(heapRel, &htid,
													   &SnapshotDirty,
													   &all_dead))
				{
					TransactionId xwait;

					/*
					 * 这是重复项。若我们只做部分
					 * 检查，就不必查看该元组是否正在
					 * 被另一个事务更新。只需返回
					 * 它是潜在冲突这一事实，把完整
					 * 检查留到以后。不要使二分查找
					 * 边界失效。
					 */
					if (checkUnique == UNIQUE_CHECK_PARTIAL)
					{
						if (nbuf != InvalidBuffer)
							_bt_relbuf(rel, nbuf);
						*is_unique = false;
						return InvalidTransactionId;
					}

					/*
					 * 若该元组正被其他事务更新，
					 * 则必须等待它提交/中止。
					 */
					xwait = (TransactionIdIsValid(SnapshotDirty.xmin)) ?
						SnapshotDirty.xmin : SnapshotDirty.xmax;

					if (TransactionIdIsValid(xwait))
					{
						if (nbuf != InvalidBuffer)
							_bt_relbuf(rel, nbuf);
						/* 让 _bt_doinsert 等待... */
						*speculativeToken = SnapshotDirty.speculativeToken;
						/* 调用者立即释放 buf 上的锁 */
						insertstate->bounds_valid = false;
						return xwait;
					}

					/*
					 * 否则我们有确定的冲突。但在
					 * 报错之前，先看我们要插入的元组
					 * 本身现在是否已提交死亡 --- 若是，就不要报错。
					 * 正常情形下这是浪费时间，但我们必须
					 * 这样做，以支持 CREATE INDEX CONCURRENTLY。
					 *
					 * 这里必须跟随 HOT 链，因为在
					 * 并发索引构建期间，我们插入的是根 TID，尽管
					 * 实际元组可能位于 HOT 链的某处。
					 * 沿链行走时，我们也许不会停在
					 * 触发本次插入的那个确切元组上，但这没关系，
					 * 因为若在这条链的任何地方找到存活元组，
					 * 我们就有唯一键冲突。另一个存活元组
					 * 不属于这条链，因为它有不同的索引
					 * 项。
					 */
					htid = itup->t_tid;
					if (table_index_fetch_tuple_check(heapRel, &htid,
													  SnapshotSelf, NULL))
					{
						/* 正常情形 --- 它仍然存活 */
					}
					else
					{
						/*
						 * 它已被删除，因此没有错误，也无需
						 * 继续搜索
						 */
						break;
					}

					/*
					 * 像我们即将写入该页时那样，检查一次 conflict-in。
					 * 我们实际上并不写入，
					 * 但希望有机会报告那些否则会被这次唯一约束
					 * 违反所掩盖的
					 * SSI 冲突。
					 */
					CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(insertstate->buf));

					/*
					 * 这是确定的冲突。把元组拆成
					 * 数据值并报告错误。但首先，务必
					 * 释放我们持有的缓冲区锁 ---
					 * BuildIndexValueDescription 可能访问系统目录，
					 * 最坏情况下可能碰到同一个索引并
					 * 造成死锁。
					 */
					if (nbuf != InvalidBuffer)
						_bt_relbuf(rel, nbuf);
					_bt_relbuf(rel, insertstate->buf);
					insertstate->buf = InvalidBuffer;
					insertstate->bounds_valid = false;

					{
						Datum		values[INDEX_MAX_KEYS];
						bool		isnull[INDEX_MAX_KEYS];
						char	   *key_desc;

						index_deform_tuple(itup, RelationGetDescr(rel),
										   values, isnull);

						key_desc = BuildIndexValueDescription(rel, values,
															  isnull);

						ereport(ERROR,
								(errcode(ERRCODE_UNIQUE_VIOLATION),
								 errmsg("duplicate key value violates unique constraint \"%s\"",
										RelationGetRelationName(rel)),
								 key_desc ? errdetail("Key %s already exists.",
													  key_desc) : 0,
								 errtableconstraint(heapRel,
													RelationGetRelationName(rel))));
					}
				}
				else if (all_dead && (!inposting ||
									  (prevalldead &&
									   curposti == BTreeTupleGetNPosting(curitup) - 1)))
				{
					/*
					 * 冲突元组（或所有倒排列表 TIDs 所指向的
					 * 全部 HOT 链）对所有人都已死，因此把
					 * 索引项标记为杀死。
					 */
					ItemIdMarkDead(curitemid);
					opaque->btpo_flags |= BTP_HAS_GARBAGE;

					/*
					 * 给缓冲区打上脏提示，因为该状态并不
					 * 关键。务必把正确的缓冲区标为脏。
					 */
					if (nbuf != InvalidBuffer)
						MarkBufferDirtyHint(nbuf, true);
					else
						MarkBufferDirtyHint(insertstate->buf, true);
				}

				/*
				 * 记住倒排列表元组是否哪怕有一条 HOT 链，
				 * 其成员并非全部已死
				 */
				if (!all_dead && inposting)
					prevalldead = false;
			}
		}

		if (inposting && curposti < BTreeTupleGetNPosting(curitup) - 1)
		{
			/* 前进到同一倒排列表中的下一个 TID */
			curposti++;
			continue;
		}
		else if (offset < maxoff)
		{
			/* 前进到下一个元组 */
			curposti = 0;
			inposting = false;
			offset = OffsetNumberNext(offset);
		}
		else
		{
			int			highkeycmp;

			/* 若扫描键 == hikey，还得检查下一页 */
			if (P_RIGHTMOST(opaque))
				break;
			highkeycmp = _bt_compare(rel, itup_key, page, P_HIKEY);
			Assert(highkeycmp <= 0);
			if (highkeycmp != 0)
				break;
			/* 前进到下一个非死页 --- 必定有一页 */
			for (;;)
			{
				BlockNumber nblkno = opaque->btpo_next;

				nbuf = _bt_relandgetbuf(rel, nbuf, nblkno, BT_READ);
				page = BufferGetPage(nbuf);
				opaque = BTPageGetOpaque(page);
				if (!P_IGNORE(opaque))
					break;
				if (P_RIGHTMOST(opaque))
					elog(ERROR, "fell off the end of index \"%s\"",
						 RelationGetRelationName(rel));
			}
			/* 也会前进到下一个元组 */
			curposti = 0;
			inposting = false;
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
			/* 不要使二分查找边界失效 */
		}
	}

	/*
	 * 若我们正在复查，则应当已经找到正在
	 * 检查的那个元组。否则就有很严重的问题 --- 很可能
	 * 索引建在非不可变表达式上。
	 */
	if (checkUnique == UNIQUE_CHECK_EXISTING && !found)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to re-find tuple within index \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("This may be because of a non-immutable index expression."),
				 errtableconstraint(heapRel,
									RelationGetRelationName(rel))));

	if (nbuf != InvalidBuffer)
		_bt_relbuf(rel, nbuf);

	return InvalidTransactionId;
}


/*
 *	_bt_findinsertloc() -- 为元组寻找插入位置
 *
 *		进入时，insertstate 的缓冲区含有新元组所属的那一页。
 *		它由调用者以排他方式锁定并钉住。
 *
 *		若 'checkingunique' 为真，进入时的缓冲区是含有
 *		新键重复项的第一页。若重复项分布在
 *		多页上，正确的插入位置可能是右边的某一页，
 *		而不是第一页。此时，本函数
 *		向右移动到正确的目标页。
 *
 *		（在 !heapkeyspace 索引中，可能有多页具有相同的
 *		高键，新元组都可以合法地放在那些页上。此时
 *		调用者传入含有重复项的第一页，
 *		就像 checkingunique=true 时一样。若该页没有足够
 *		空间放下新元组，本函数就向右移动，试图找到
 *		一张放得下的合法页。）
 *
 *		若 'indexUnchanged' 为真，这是一次并未
 *		从逻辑上改变索引值的 UPDATE，但仍必须有一条新
 *		索引项指向后继版本。执行器给出的这一提示
 *		会影响我们在页也许不得不分裂、
 *		而我们必须考虑各种选择时的行为。
 *		自底向上索引删除可以避免
 *		由版本驱动的病态页分裂，但只有在我们已经有一定把握
 *		认为它合适时，才愿意费力去试。除非叶子页上几乎所有插入
 *		都带有该提示，否则随着时间推移，这一提示不应当显著影响我们的
 *		行为。
 *
 *		退出时，insertstate 的缓冲区含有选定的插入页，并
 *		返回该页内的偏移。若 _bt_findinsertloc 需要
 *		向右移动，则释放原页上的锁和钉住，
 *		改为对新缓冲区加排他锁并钉住。
 *
 *		若 insertstate 含有缓存的二分查找边界，我们会加以
 *		利用。这样就可以避免重复我们在
 *		_bt_check_unique() 中已经做过的比较。
 */
static OffsetNumber
_bt_findinsertloc(Relation rel,
				  BTInsertState insertstate,
				  bool checkingunique,
				  bool indexUnchanged,
				  BTStack stack,
				  Relation heapRel)
{
	BTScanInsert itup_key = insertstate->itup_key;
	Page		page = BufferGetPage(insertstate->buf);
	BTPageOpaque opaque;
	OffsetNumber newitemoff;

	opaque = BTPageGetOpaque(page);

	/* 检查页三分之一的限制 */
	if (unlikely(insertstate->itemsz > BTMaxItemSize(page)))
		_bt_check_third_page(rel, heapRel, itup_key->heapkeyspace, page,
							 insertstate->itup);

	Assert(P_ISLEAF(opaque) && !P_INCOMPLETE_SPLIT(opaque));
	Assert(!insertstate->bounds_valid || checkingunique);
	Assert(!itup_key->heapkeyspace || itup_key->scantid != NULL);
	Assert(itup_key->heapkeyspace || itup_key->scantid == NULL);
	Assert(!itup_key->allequalimage || itup_key->heapkeyspace);

	if (itup_key->heapkeyspace)
	{
		/* 记录是否见过 checkingunique 重复项 */
		bool		uniquedup = indexUnchanged;

		/*
		 * 若插入的是唯一索引，我们也许必须向右
		 * 走过若干叶子页，才能找到我们必须插入到的
		 * 那一张叶子页。
		 *
		 * checkingunique 调用者需要这样做，因为调用 _bt_search() 时
		 * 没有使用 scantid。只有在
		 * _bt_check_unique() 检查过重复项之后才能设置 scantid。最初
		 * 存在 insertstate->buf 中的缓冲区是可能找到第一个
		 * 重复键的那一页，它并不总是新
		 * 元组所属的那一页。新元组的堆 TID 属性（scantid）
		 * 可能迫使我们插入到兄弟页上，不过实践中
		 * 这应当非常少见。
		 */
		if (checkingunique)
		{
			if (insertstate->low < insertstate->stricthigh)
			{
				/* 在 _bt_check_unique() 中遇到了重复项 */
				Assert(insertstate->bounds_valid);
				uniquedup = true;
			}

			for (;;)
			{
				/*
				 * 新元组是否属于这一页？
				 *
				 * 先前的 _bt_check_unique() 调用很可能已经
				 * 为新项的偏移建立了严格上界。
				 * 若它不是该页的最后一项（也就是说，若
				 * 页上至少有一个元组排在我们正在插入的元组
				 * 之后），我们就知道该元组属于
				 * 这一页。可以跳过高键检查。
				 */
				if (insertstate->bounds_valid &&
					insertstate->low <= insertstate->stricthigh &&
					insertstate->stricthigh <= PageGetMaxOffsetNumber(page))
					break;

				/* 测试 '<='，而不是 '!='，因为现在已经设置了 scantid */
				if (P_RIGHTMOST(opaque) ||
					_bt_compare(rel, itup_key, page, P_HIKEY) <= 0)
					break;

				_bt_stepright(rel, heapRel, insertstate, stack);
				/* 向右走一步之后更新本地状态 */
				page = BufferGetPage(insertstate->buf);
				opaque = BTPageGetOpaque(page);
				/* 假定存在重复项（若 checkingunique） */
				uniquedup = true;
			}
		}

		/*
		 * 若目标页无法放下 newitem，现在就尝试通过删除或去重
		 * 来避免插入时分裂该页
		 */
		if (PageGetFreeSpace(page) < insertstate->itemsz)
			_bt_delete_or_dedup_one_page(rel, heapRel, insertstate, false,
										 checkingunique, uniquedup,
										 indexUnchanged);
	}
	else
	{
		/*----------
		 * 这是 !heapkeyspace（版本 2 或 3）索引。当前页
		 * 是我们可以把新元组插入进去的第一页，但右边
		 * 也许还有其他页可供选用。
		 *
		 * 若新键等于一个或多个已有键，我们可以
		 * 把它合法地放在这一串相等键中的任何位置。事实上，
		 * 若新键等于该页的"高键"，我们可以把它放在
		 * 下一页。若它等于高键，并且当前页没有
		 * 足够空间在不分裂的情况下插入新元组，
		 * 我们就向右移动，希望找到更多空闲空间并
		 * 避免分裂。
		 *
		 * 持续向右扫描，直到我们
		 *		(a) 找到有足够空闲空间的一页，
		 *		(b) 到达元组可以合法前往的最后一页，或
		 *		(c) 对搜索感到厌倦。
		 * (c) 并不是随口一说；它很重要，因为若有许多
		 * 页的相等键，与其在每次插入时一直扫到相等键
		 * 这一串的末尾，不如分裂较早的某一页。
		 * 我们把"感到厌倦"实现为随机选择，
		 * 因为扫过固定页数后就停下效果并不好
		 * （那样我们就永远到不了先前已分裂
		 * 各页的右侧）。向右移动的概率设为 0.99，这也许
		 * 看起来高得不太会改变行为，但它在防止大量相等键造成
		 * O(N^2) 行为方面效果极好。
		 *----------
		 */
		while (PageGetFreeSpace(page) < insertstate->itemsz)
		{
			/*
			 * 在考虑向右移动之前，先看能否通过擦除 LP_DEAD 项
			 * 获得足够空间
			 */
			if (P_HAS_GARBAGE(opaque))
			{
				/* 执行简单删除 */
				_bt_delete_or_dedup_one_page(rel, heapRel, insertstate, true,
											 false, false, false);

				if (PageGetFreeSpace(page) >= insertstate->itemsz)
					break;		/* 好，现在我们有足够空间了 */
			}

			/*
			 * 不行，因此检查上面列举的条件 (b) 和 (c)
			 *
			 * 先前的 _bt_check_unique() 调用很可能已经
			 * 为新项的偏移建立了严格上界。若它不是
			 * 该页的最后一项（也就是说，若页上至少有一个元组
			 * 大于我们要插入的元组），
			 * 我们就知道该元组属于这一页。可以跳过
			 * 高键检查。
			 */
			if (insertstate->bounds_valid &&
				insertstate->low <= insertstate->stricthigh &&
				insertstate->stricthigh <= PageGetMaxOffsetNumber(page))
				break;

			if (P_RIGHTMOST(opaque) ||
				_bt_compare(rel, itup_key, page, P_HIKEY) != 0 ||
				pg_prng_uint32(&pg_global_prng_state) <= (PG_UINT32_MAX / 100))
				break;

			_bt_stepright(rel, heapRel, insertstate, stack);
			/* 向右走一步之后更新本地状态 */
			page = BufferGetPage(insertstate->buf);
			opaque = BTPageGetOpaque(page);
		}
	}

	/*
	 * 现在应当位于正确的页上。找出新元组在该页内的
	 * 偏移。（可能会复用先前的搜索边界。）
	 */
	Assert(P_RIGHTMOST(opaque) ||
		   _bt_compare(rel, itup_key, page, P_HIKEY) <= 0);

	newitemoff = _bt_binsrch_insert(rel, insertstate);

	if (insertstate->postingoff == -1)
	{
		/*
		 * 有一个重叠的倒排列表元组设置了 LP_DEAD 位。
		 * 我们不想在
		 * 做倒排列表分裂时不必要地清除它的 LP_DEAD 位，因此提早执行简单索引元组
		 * 删除。
		 */
		_bt_delete_or_dedup_one_page(rel, heapRel, insertstate, true,
									 false, false, false);

		/*
		 * 做一次新的二分查找。现在新的插入位置无法与任何
		 * 倒排列表重叠。
		 */
		Assert(!insertstate->bounds_valid);
		insertstate->postingoff = 0;
		newitemoff = _bt_binsrch_insert(rel, insertstate);
		Assert(insertstate->postingoff == 0);
	}

	return newitemoff;
}

/*
 * 插入期间向右走到下一个非死页。
 *
 * 这比搜索时向右移动要稍微复杂一些。我们必须
 * 在释放当前页的写锁之前，先对目标页加写锁；
 * 否则别人的 _bt_check_unique 扫描可能看不见我们的插入。
 * 对中间的死页加写锁没有用，因为我们不知道它们
 * 何时会从树中被摘掉链接。
 *
 * 对于非唯一的 !heapkeyspace
 * 索引，这样做比实际需要的更激进。
 */
static void
_bt_stepright(Relation rel, Relation heaprel, BTInsertState insertstate,
			  BTStack stack)
{
	Page		page;
	BTPageOpaque opaque;
	Buffer		rbuf;
	BlockNumber rblkno;

	Assert(heaprel != NULL);
	page = BufferGetPage(insertstate->buf);
	opaque = BTPageGetOpaque(page);

	rbuf = InvalidBuffer;
	rblkno = opaque->btpo_next;
	for (;;)
	{
		rbuf = _bt_relandgetbuf(rel, rbuf, rblkno, BT_WRITE);
		page = BufferGetPage(rbuf);
		opaque = BTPageGetOpaque(page);

		/*
		 * 若该页的分裂尚未完成，现在就完成分裂。我们在
		 * 持有左兄弟锁的情况下做这件事，这并不理想，
		 * 因为完成分裂可能是相当长的操作。
		 * 但这应当极少发生。
		 */
		if (P_INCOMPLETE_SPLIT(opaque))
		{
			_bt_finish_split(rel, heaprel, rbuf, stack);
			rbuf = InvalidBuffer;
			continue;
		}

		if (!P_IGNORE(opaque))
			break;
		if (P_RIGHTMOST(opaque))
			elog(ERROR, "fell off the end of index \"%s\"",
				 RelationGetRelationName(rel));

		rblkno = opaque->btpo_next;
	}
	/* rbuf 已锁定；解锁 buf，并为调用者更新状态 */
	_bt_relbuf(rel, insertstate->buf);
	insertstate->buf = rbuf;
	insertstate->bounds_valid = false;
}

/*----------
 *	_bt_insertonpg() -- 把元组插入索引中的某一页。
 *
 *		这个递归过程做下列事情：
 *
 *			+  若 postingoff != 0，分裂已有的倒排列表元组
 *			   （因为它与新的 'itup' 元组重叠）。
 *			+  必要时分裂目标页，用 'itup_key' 做
 *			   后缀截断，叶子页上如此（调用者对
 *			   非叶子页传入 NULL）。
 *			+  插入新元组（它可能是从倒排列表分裂出来的）。
 *			+  若该页被分裂，则弹出父栈，并找到
 *			   插入新子指针的正确位置（利用父栈中保存的信息
 *			   向右走）。
 *			+  用右
 *			   子页在父页上对应的合适元组调用自身。
 *			+  若真正的根或快速根被分裂，则更新元数据页。
 *
 *		进入时，我们必须持有用来做
 *		插入的正确缓冲区，并且该缓冲区必须已钉住并加了写锁。返回时，
 *		我们会同时放下该缓冲区上的钉住和锁。
 *
 *		本例程只做零售式元组插入。'itup' 应当
 *		始终要么是非高键的叶子项，要么是下行链接（新的高
 *		键项是在页分裂时间接创建的）。当
 *		插入到非叶子页时，'cbuf' 是我们要为其插入下行链接的那一页的左兄弟。
 *		本函数会清除它上面的
 *		INCOMPLETE_SPLIT 标志，并释放该缓冲区。
 *----------
 */
static void
_bt_insertonpg(Relation rel,
			   Relation heaprel,
			   BTScanInsert itup_key,
			   Buffer buf,
			   Buffer cbuf,
			   BTStack stack,
			   IndexTuple itup,
			   Size itemsz,
			   OffsetNumber newitemoff,
			   int postingoff,
			   bool split_only_page)
{
	Page		page;
	BTPageOpaque opaque;
	bool		isleaf,
				isroot,
				isrightmost,
				isonly;
	IndexTuple	oposting = NULL;
	IndexTuple	origitup = NULL;
	IndexTuple	nposting = NULL;

	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	isleaf = P_ISLEAF(opaque);
	isroot = P_ISROOT(opaque);
	isrightmost = P_RIGHTMOST(opaque);
	isonly = P_LEFTMOST(opaque) && P_RIGHTMOST(opaque);

	/* 当且仅当插入发生在内部页上时，才必须给出子缓冲区 */
	Assert(isleaf == !BufferIsValid(cbuf));
	/* 元组的属性个数必须恰当 */
	Assert(!isleaf ||
		   BTreeTupleGetNAtts(itup, rel) ==
		   IndexRelationGetNumberOfAttributes(rel));
	Assert(isleaf ||
		   BTreeTupleGetNAtts(itup, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(!BTreeTupleIsPosting(itup));
	Assert(MAXALIGN(IndexTupleSize(itup)) == itemsz);
	/* 调用者必须总是替我们完成未完成分裂 */
	Assert(!P_INCOMPLETE_SPLIT(opaque));

	/*
	 * 每一张内部页在任何时候都应当恰好有一个负无穷项。
	 * 只有 _bt_split() 和 _bt_newlevel() 应当添加那些
	 * 经截断后成为负无穷项的项，因为只有它们
	 * 会分配新的内部页。
	 */
	Assert(isleaf || newitemoff > P_FIRSTDATAKEY(opaque));

	/*
	 * 是否需要分裂已有的倒排列表项？
	 */
	if (postingoff != 0)
	{
		ItemId		itemid = PageGetItemId(page, newitemoff);

		/*
		 * 新元组是重复项，其堆 TID 落在叶子页上
		 * 已有倒排列表元组的范围之内。准备
		 * 分裂已有的倒排列表。用分裂后的版本覆盖该倒排列表，
		 * 被当作插入或页分裂临界区中的
		 * 一个额外步骤。
		 */
		Assert(isleaf && itup_key->heapkeyspace && itup_key->allequalimage);
		oposting = (IndexTuple) PageGetItem(page, itemid);

		/*
		 * postingoff 的值来自先前对 _bt_binsrch_posting() 的调用。
		 * 它的二分查找可能认为普通元组必定是需要分裂的
		 * 倒排列表元组。损坏时可能发生这种情况：
		 * 已有普通元组与新
		 * 项重复、直至并包括其表 TID 时，损坏会导致这种情况。这里顺带
		 * 检查这一点。
		 *
		 * 同时验证调用者已经确保已有的倒排
		 * 列表元组没有设置 LP_DEAD 位。
		 */
		if (!BTreeTupleIsPosting(oposting) || ItemIdIsDead(itemid))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("table tid from new index tuple (%u,%u) overlaps with invalid duplicate tuple at offset %u of block %u in index \"%s\"",
									 ItemPointerGetBlockNumber(&itup->t_tid),
									 ItemPointerGetOffsetNumber(&itup->t_tid),
									 newitemoff, BufferGetBlockNumber(buf),
									 RelationGetRelationName(rel))));

		/* 此后用 itup 的一份可变副本作为我们的 itup */
		origitup = itup;
		itup = CopyIndexTuple(origitup);
		nposting = _bt_swap_posting(itup, oposting, postingoff);
		/* 现在 itup 含有来自 oposting 的最右/最大 TID */

		/* 调整偏移，使 newitem 排在倒排列表之后 */
		newitemoff = OffsetNumberNext(newitemoff);
	}

	/*
	 * 是否需要分裂该页才能放下这一项？
	 *
	 * 注意：PageGetFreeSpace() 会从结果中减去 sizeof(ItemIdData)，
	 * 因此这个比较是正确的，尽管看起来我们只计入了
	 * 该项本身，而没有计入它的行指针。
	 */
	if (PageGetFreeSpace(page) < itemsz)
	{
		Buffer		rbuf;

		Assert(!split_only_page);

		/* 把缓冲区分裂成左右两半 */
		rbuf = _bt_split(rel, heaprel, itup_key, buf, cbuf, newitemoff, itemsz,
						 itup, origitup, nposting, postingoff);
		PredicateLockPageSplit(rel,
							   BufferGetBlockNumber(buf),
							   BufferGetBlockNumber(rbuf));

		/*----------
		 * 到这里，
		 *
		 *		+  我们的目标页已经分裂；
		 *		+  原始元组已经插入；
		 *		+  分裂之后，我们对旧的（左半）
		 *		   和新的（右半）缓冲区都持有写锁；并且
		 *		+  我们知道要插入父页的键
		 *		   （它是左子页上的"高键"）。
		 *
		 * 我们准备做父页插入。在定位到父页之前，
		 * 需要一直持有
		 * 子页上的锁，但在做
		 * 实际插入之前，至少可以释放右子页上的锁。左子页上的锁
		 * 会在父页插入时最后释放，在那里它是父
		 * 页的 'cbuf'。
		 *----------
		 */
		_bt_insert_parent(rel, heaprel, buf, rbuf, stack, isroot, isonly);
	}
	else
	{
		Buffer		metabuf = InvalidBuffer;
		Page		metapg = NULL;
		BTMetaPageData *metad = NULL;
		BlockNumber blockcache;

		/*
		 * 若我们做这次插入，是因为分裂了一页，而该页是
		 * 其所在树层上的唯一一页，但不是根，它也许曾是
		 * "快速根"。我们需要确保快速根链接指向
		 * 当前页或更高处。这里可以安全地获取
		 * 元数据页上的锁 --- 见 _bt_newlevel() 的注释。
		 */
		if (unlikely(split_only_page))
		{
			Assert(!isleaf);
			Assert(BufferIsValid(cbuf));

			metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
			metapg = BufferGetPage(metabuf);
			metad = BTPageGetMeta(metapg);

			if (metad->btm_fastlevel >= opaque->btpo_level)
			{
				/* 不需要更新 */
				_bt_relbuf(rel, metabuf);
				metabuf = InvalidBuffer;
			}
		}

		/* 执行更新。在改动被记入日志之前不要 ereport(ERROR) */
		START_CRIT_SECTION();

		if (postingoff != 0)
			memcpy(oposting, nposting, MAXALIGN(IndexTupleSize(nposting)));

		if (PageAddItem(page, (Item) itup, itemsz, newitemoff, false,
						false) == InvalidOffsetNumber)
			elog(PANIC, "failed to add new item to block %u in index \"%s\"",
				 BufferGetBlockNumber(buf), RelationGetRelationName(rel));

		MarkBufferDirty(buf);

		if (BufferIsValid(metabuf))
		{
			/* 必要时升级元数据页 */
			if (metad->btm_version < BTREE_NOVAC_VERSION)
				_bt_upgrademetapage(metapg);
			metad->btm_fastroot = BufferGetBlockNumber(buf);
			metad->btm_fastlevel = opaque->btpo_level;
			MarkBufferDirty(metabuf);
		}

		/*
		 * 若插入新项完成了一次分裂，则清除子页上的
		 * INCOMPLETE_SPLIT 标志
		 */
		if (!isleaf)
		{
			Page		cpage = BufferGetPage(cbuf);
			BTPageOpaque cpageop = BTPageGetOpaque(cpage);

			Assert(P_INCOMPLETE_SPLIT(cpageop));
			cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
			MarkBufferDirty(cbuf);
		}

		/* XLOG 相关 */
		if (RelationNeedsWAL(rel))
		{
			xl_btree_insert xlrec;
			xl_btree_metadata xlmeta;
			uint8		xlinfo;
			XLogRecPtr	recptr;
			uint16		upostingoff;

			xlrec.offnum = newitemoff;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfBtreeInsert);

			if (isleaf && postingoff == 0)
			{
				/* 简单的叶子插入 */
				xlinfo = XLOG_BTREE_INSERT_LEAF;
			}
			else if (postingoff != 0)
			{
				/*
				 * 带倒排列表分裂的叶子插入。必须在
				 * newitem/orignewitem 之前包含 postingoff 字段。
				 */
				Assert(isleaf);
				xlinfo = XLOG_BTREE_INSERT_POST;
			}
			else
			{
				/* 内部页插入，它完成 cbuf 上的一次分裂 */
				xlinfo = XLOG_BTREE_INSERT_UPPER;
				XLogRegisterBuffer(1, cbuf, REGBUF_STANDARD);

				if (BufferIsValid(metabuf))
				{
					/* 实际上，这是内部页插入加上元数据更新 */
					xlinfo = XLOG_BTREE_INSERT_META;

					Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
					xlmeta.version = metad->btm_version;
					xlmeta.root = metad->btm_root;
					xlmeta.level = metad->btm_level;
					xlmeta.fastroot = metad->btm_fastroot;
					xlmeta.fastlevel = metad->btm_fastlevel;
					xlmeta.last_cleanup_num_delpages = metad->btm_last_cleanup_num_delpages;
					xlmeta.allequalimage = metad->btm_allequalimage;

					XLogRegisterBuffer(2, metabuf,
									   REGBUF_WILL_INIT | REGBUF_STANDARD);
					XLogRegisterBufData(2, (char *) &xlmeta,
										sizeof(xl_btree_metadata));
				}
			}

			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			if (postingoff == 0)
			{
				/* 只记录调用者的 itup */
				XLogRegisterBufData(0, (char *) itup, IndexTupleSize(itup));
			}
			else
			{
				/*
				 * 带倒排列表分裂的插入（XLOG_BTREE_INSERT_POST
				 * 记录）情形。
				 *
				 * 记录 postingoff。同时记录 origitup，而不是 itup。REDO 例程
				 * 必须用 _bt_swap_posting() 重建最终的 itup
				 * （以及 nposting）。
				 */
				upostingoff = postingoff;

				XLogRegisterBufData(0, (char *) &upostingoff, sizeof(uint16));
				XLogRegisterBufData(0, (char *) origitup,
									IndexTupleSize(origitup));
			}

			recptr = XLogInsert(RM_BTREE_ID, xlinfo);

			if (BufferIsValid(metabuf))
				PageSetLSN(metapg, recptr);
			if (!isleaf)
				PageSetLSN(BufferGetPage(cbuf), recptr);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/* 释放附属缓冲区 */
		if (BufferIsValid(metabuf))
			_bt_relbuf(rel, metabuf);
		if (!isleaf)
			_bt_relbuf(rel, cbuf);

		/*
		 * 若这是最右叶子页，则缓存其块号。缓存
		 * 可供 _bt_search_insert() 中未来的插入者使用。
		 */
		blockcache = InvalidBlockNumber;
		if (isrightmost && isleaf && !isroot)
			blockcache = BufferGetBlockNumber(buf);

		/* 释放插入目标块的缓冲区 */
		_bt_relbuf(rel, buf);

		/*
		 * 若我们决定在释放其缓冲区锁之前
		 * 缓存插入目标块，现在就缓存它。
		 * 不过先检查树的高度。
		 * 对小索引我们不采用该优化。把最后的检查推迟到这里，以确保我们不会
		 * 在持有缓冲区锁时调用 _bt_getrootheight。
		 */
		if (BlockNumberIsValid(blockcache) &&
			_bt_getrootheight(rel) >= BTREE_FASTPATH_MIN_LEVEL)
			RelationSetTargetBlock(rel, blockcache);
	}

	/* 保持整洁 */
	if (postingoff != 0)
	{
		/* itup 实际上是调用者原始元组的一份修改过的副本 */
		pfree(nposting);
		pfree(itup);
	}
}

/*
 *	_bt_split() -- 分裂 B-树中的一页。
 *
 *		进入时，buf 是要分裂的页，已钉住并加了写锁。
 *		newitemoff 等告诉我们必须与原始页上的数据一起
 *		插入的新项。
 *
 *		itup_key 用于叶子页上的后缀截断（内部
 *		页的调用者传入 NULL）。分裂非叶子页时，'cbuf'
 *		是我们要为其插入下行链接的那一页的左兄弟。
 *		本函数会清除它上面的 INCOMPLETE_SPLIT 标志，并
 *		释放该缓冲区。
 *
 *		当插入 orignewitem 同时导致倒排列表分裂
 *		和页分裂时，需要 orignewitem、nposting
 *		和 postingoff。
 *		这些额外的倒排列表分裂细节在这里的用法，
 *		与倒排列表分裂并不伴随页分裂的更常见情形相同。
 *		我们必须直接处理倒排列表分裂，以确保由插入 orignewitem
 *		引出的一切都作为一次
 *		原子操作来处理（不过调用者把新的枢轴/下行链接
 *		插入父页仍是单独的操作）。关于倒排列表分裂的设计，
 *		见 nbtree/README。
 *
 *		返回 buf 的新右兄弟，已钉住并加了写锁。
 *		buf 上的钉住和锁保持不变。
 */
static Buffer
_bt_split(Relation rel, Relation heaprel, BTScanInsert itup_key, Buffer buf,
		  Buffer cbuf, OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
		  IndexTuple orignewitem, IndexTuple nposting, uint16 postingoff)
{
	Buffer		rbuf;
	Page		origpage;
	Page		leftpage,
				rightpage;
	BlockNumber origpagenumber,
				rightpagenumber;
	BTPageOpaque ropaque,
				lopaque,
				oopaque;
	Buffer		sbuf = InvalidBuffer;
	Page		spage = NULL;
	BTPageOpaque sopaque = NULL;
	Size		itemsz;
	ItemId		itemid;
	IndexTuple	firstright,
				lefthighkey;
	OffsetNumber firstrightoff;
	OffsetNumber afterleftoff,
				afterrightoff,
				minusinfoff;
	OffsetNumber origpagepostingoff;
	OffsetNumber maxoff;
	OffsetNumber i;
	bool		newitemonleft,
				isleaf,
				isrightmost;

	/*
	 * origpage 是要分裂的原始页。leftpage 是一块临时
	 * 缓冲区，接收左兄弟的数据，成功后会复制回
	 * origpage。rightpage 是将接收
	 * 右兄弟数据的新页。
	 *
	 * 选定分裂点之后才分配 leftpage。rightpage 的新
	 * 缓冲区要等到 leftpage 初始化完成并有了新的
	 * 高键之后才获取，那是分裂该页还可能失败的最后一点（损坏
	 * 除外）。在获取新缓冲区之前失败不会有持久
	 * 后果，因为 origpage 尚未被修改，而 leftpage 只是
	 * 工作区。
	 */
	origpage = BufferGetPage(buf);
	oopaque = BTPageGetOpaque(origpage);
	isleaf = P_ISLEAF(oopaque);
	isrightmost = P_RIGHTMOST(oopaque);
	maxoff = PageGetMaxOffsetNumber(origpage);
	origpagenumber = BufferGetBlockNumber(buf);

	/*
	 * 选择把 origpage 分开的位置。
	 *
	 * 可以把分裂点想成 origpage 上两个已有数据
	 * 项之间（_between_）的一点（lastleft 与 firstright 元组），只要你
	 * 假装那个放不下的新项已经在 origpage 上。
	 *
	 * 由于 origpage 实际上并不包含 newitem，分裂点的表示
	 * 必须能处理两种边界情形：newitem 是 lastleft 的分裂，
	 * 以及 newitem 是 firstright 的分裂。
	 * 当 newitemoff == firstrightoff 时，否则会有歧义，由 newitemonleft 来解决。
	 * 在所有其他情形下，从上下文就能看清每个元组落在
	 * 分裂的哪一侧。newitemonleft
	 * 通常（但并不总是）是冗余信息。
	 *
	 * firstrightoff 应当是 origpage 的偏移号，但
	 * 它的值也可能是 maxoff+1，也就是 origpage 的"末尾之后"。
	 * 这发生在少见的情形：newitem 排在所有
	 * 已有项之后（即 newitemoff 为 maxoff+1），而我们最终在
	 * 把 newitem 单独留在新右页的那个点上分裂
	 * origpage。不过，任何
	 * "!newitemonleft && newitemoff == firstrightoff" 分裂点都会使
	 * newitem 成为 firstright 元组，因此这种情况并不是特例。
	 */
	firstrightoff = _bt_findsplitloc(rel, origpage, newitemoff, newitemsz,
									 newitem, &newitemonleft);

	/* 为 leftpage 分配临时缓冲区 */
	leftpage = PageGetTempPage(origpage);
	_bt_pageinit(leftpage, BufferGetPageSize(buf));
	lopaque = BTPageGetOpaque(leftpage);

	/*
	 * 完成后 leftpage 将不再是根。同时清除 SPLIT_END
	 * 和 HAS_GARBAGE 标志。
	 */
	lopaque->btpo_flags = oopaque->btpo_flags;
	lopaque->btpo_flags &= ~(BTP_ROOT | BTP_SPLIT_END | BTP_HAS_GARBAGE);
	/* 在 leftpage 中设置标志，表示 rightpage 还没有下行链接 */
	lopaque->btpo_flags |= BTP_INCOMPLETE_SPLIT;
	lopaque->btpo_prev = oopaque->btpo_prev;
	/* 获取 rightpage 缓冲区之后再处理 btpo_next */
	lopaque->btpo_level = oopaque->btpo_level;
	/* 获取 rightpage 缓冲区之后再处理 btpo_cycleid */

	/*
	 * 把原始页的 LSN 复制到 leftpage，它将成为
	 * 该页的更新版本。我们需要这样做，因为 XLogInsert 会
	 * 检查 LSN，并可能把它转储进页镜像。
	 */
	PageSetLSN(leftpage, PageGetLSN(origpage));

	/*
	 * 在需要顺带做倒排列表分裂时，确定与 orignewitem
	 * 重叠的已有倒排列表的页偏移号。
	 * 注意，调用者已经改过 newitem
	 * （newitem 不再带有 orignewitem 的 TID）。
	 *
	 * 这个页偏移号（origpagepostingoff）用来假装
	 * 倒排分裂已经发生，尽管对 origpage 的
	 * 必要修改要到进入临界区才会发生。我们页分裂
	 * 点的 lastleft 与 firstright 元组，实际上应当来自
	 * origpage 的一个想象版本，其中是 nposting 元组，
	 * 而不是原来的倒排列表元组。
	 *
	 * 注意：_bt_findsplitloc() 至少在理论上应当以同样的方式
	 * 补偿同时发生的倒排列表分裂。
	 * 不过它并不费这个心。
	 * 实践中这不会影响它对分裂点的选择。
	 */
	origpagepostingoff = InvalidOffsetNumber;
	if (postingoff != 0)
	{
		Assert(isleaf);
		Assert(ItemPointerCompare(&orignewitem->t_tid,
								  &newitem->t_tid) < 0);
		Assert(BTreeTupleIsPosting(nposting));
		origpagepostingoff = OffsetNumberPrev(newitemoff);
	}

	/*
	 * 新左页的高键是叶子层上 firstright 的一份可能被截断的副本
	 * （在内部页上它就是"firstright 本身"；见下面的 !isleaf 注释）。
	 * 这看起来似乎
	 * 与 Lehman 与 Yao 在叶子层分裂时用 lastleft 的副本作为新高键的做法相反。
	 * 其实并不相反。
	 *
	 * 当 lastleft 与 firstright 在堆 TID 之前相等时，后缀截断会让左页的高键
	 * 完全等于 lastleft（也就是说，决胜 TID 值来自 lastleft）。新的叶子高键
	 * 实际上并不必须是 lastleft 的副本，L&Y 的
	 * "子树"不变量也能成立。只要保证新的
	 * 叶子高键严格小于 firstright，并且大于或
	 * 等于（不必等于）lastleft 即可。换句话说，当
	 * 叶子页分裂期间无法做后缀截断时，我们采用
	 * L&Y 为左页生成新高键的精确做法。
	 * （实际上这稍有不准确。我们并不只是用 lastleft 的副本。
	 * 用的是带有 firstright 全部键、
	 * 但最大堆
	 * TID 取自 lastleft 的元组，以避免引入特例。）
	 */
	if (!newitemonleft && newitemoff == firstrightoff)
	{
		/* 传入的元组成为 firstright */
		itemsz = newitemsz;
		firstright = newitem;
	}
	else
	{
		/* firstrightoff 处的已有项成为 firstright */
		itemid = PageGetItemId(origpage, firstrightoff);
		itemsz = ItemIdGetLength(itemid);
		firstright = (IndexTuple) PageGetItem(origpage, itemid);
		if (firstrightoff == origpagepostingoff)
			firstright = nposting;
	}

	if (isleaf)
	{
		IndexTuple	lastleft;

		/* 尝试对叶子页分裂做后缀截断 */
		if (newitemonleft && newitemoff == firstrightoff)
		{
			/* 传入的元组成为 lastleft */
			lastleft = newitem;
		}
		else
		{
			OffsetNumber lastleftoff;

			/* firstrightoff 之前的已有项成为 lastleft */
			lastleftoff = OffsetNumberPrev(firstrightoff);
			Assert(lastleftoff >= P_FIRSTDATAKEY(oopaque));
			itemid = PageGetItemId(origpage, lastleftoff);
			lastleft = (IndexTuple) PageGetItem(origpage, itemid);
			if (lastleftoff == origpagepostingoff)
				lastleft = nposting;
		}

		lefthighkey = _bt_truncate(rel, lastleft, firstright, itup_key);
		itemsz = IndexTupleSize(lefthighkey);
	}
	else
	{
		/*
		 * 不要对 firstright 的副本做后缀截断，来为内部页分裂
		 * 生成左页高键。必须直接把 firstright
		 * 用作新高键。
		 *
		 * 每一个不同的分隔键值都起源于叶子层高
		 * 键；所有其他分隔键/枢轴元组都是从下一
		 * 层复制来的。祖父页中的分隔键必须与
		 * 其左侧子树最右父页中的高键完全相同，
		 * 而那个高键本身又必须与
		 * 同一子树最右子页中的高键完全相同（这一点甚至适用于来自
		 * 祖父高键的分隔键）。从祖父开始，
		 * 每一层
		 * 都必须有一条不断开的、相同分隔键组成的"接缝"，在每一
		 * 层引导索引扫描。这就是这里做后缀截断
		 *
		 * 并不安全的原因。
		 * 不过，内部页分裂会在 firstright 被插入下面新右页时，
		 * 把它截断成"负无穷"数据项。
		 * 这发生在为
		 * 右页的新的第一个数据项调用 _bt_pgaddtup() 期间。不要把这一
		 * 机制与后缀截断搞混。它只是实现如下页分裂的一种方便办法：
		 * 在 firstright "内部"分裂内部页。lefthighkey 分隔键无法
		 * 在右页中再次出现（只有 firstright 的下行链接进入右
		 * 页）。
		 */
		lefthighkey = firstright;
	}

	/*
	 * 把新高键加到 leftpage
	 */
	afterleftoff = P_HIKEY;

	Assert(BTreeTupleGetNAtts(lefthighkey, rel) > 0);
	Assert(BTreeTupleGetNAtts(lefthighkey, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	Assert(itemsz == MAXALIGN(IndexTupleSize(lefthighkey)));
	if (PageAddItem(leftpage, (Item) lefthighkey, itemsz, afterleftoff, false,
					false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add high key to the left sibling"
			 " while splitting block %u of index \"%s\"",
			 origpagenumber, RelationGetRelationName(rel));
	afterleftoff = OffsetNumberNext(afterleftoff);

	/*
	 * 既然左页已经有了新的
	 * 高键，就获取一张新的右页来分裂进去。从此以后，若不先把
	 * rightpage 清零，就不可以抛出错误。这条编码规则确保我们不会
	 * 把未来的 VACUUM 操作搞糊涂，否则它们可能会试图重新找到
	 * 指向一张残留垃圾页的下行链接，而该页正在被删除。
	 *
	 * 也可以在获取新的
	 * rightpage 缓冲区之后立即开始临界区；那样就可以避免
	 * 残留垃圾页，而不必费心把 rightpage 清零。我们这样做，
	 * 是因为它能在 origpage 或其
	 * 已有兄弟页损坏时避免一次不必要的 PANIC。
	 */
	rbuf = _bt_allocbuf(rel, heaprel);
	rightpage = BufferGetPage(rbuf);
	rightpagenumber = BufferGetBlockNumber(rbuf);
	/* rightpage 已由 _bt_getbuf 初始化 */
	ropaque = BTPageGetOpaque(rightpage);

	/*
	 * 完成 leftpage 特殊区中其余字段。在 origpage（leftpage）和 rightpage
	 * 两块缓冲区都已获取并
	 * 锁定之前无法设置它们。
	 *
	 * btpo_cycleid 只用于叶子页，不过为了一致，
	 * 这里在所有情况下都设置它。
	 */
	lopaque->btpo_next = rightpagenumber;
	lopaque->btpo_cycleid = _bt_vacuum_cycleid(rel);

	/*
	 * 完成后 rightpage 将不再是根。同时清除 SPLIT_END
	 * 和 HAS_GARBAGE 标志。
	 */
	ropaque->btpo_flags = oopaque->btpo_flags;
	ropaque->btpo_flags &= ~(BTP_ROOT | BTP_SPLIT_END | BTP_HAS_GARBAGE);
	ropaque->btpo_prev = origpagenumber;
	ropaque->btpo_next = oopaque->btpo_next;
	ropaque->btpo_level = oopaque->btpo_level;
	ropaque->btpo_cycleid = lopaque->btpo_cycleid;

	/*
	 * 必要时把新高键加到 rightpage。
	 *
	 * 若我们正在分裂的页不是树中该层的最右页，
	 * 则该页上的第一项是来自
	 * origpage 的高键。
	 */
	afterrightoff = P_HIKEY;

	if (!isrightmost)
	{
		IndexTuple	righthighkey;

		itemid = PageGetItemId(origpage, P_HIKEY);
		itemsz = ItemIdGetLength(itemid);
		righthighkey = (IndexTuple) PageGetItem(origpage, itemid);
		Assert(BTreeTupleGetNAtts(righthighkey, rel) > 0);
		Assert(BTreeTupleGetNAtts(righthighkey, rel) <=
			   IndexRelationGetNumberOfKeyAttributes(rel));
		if (PageAddItem(rightpage, (Item) righthighkey, itemsz, afterrightoff,
						false, false) == InvalidOffsetNumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add high key to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		afterrightoff = OffsetNumberNext(afterrightoff);
	}

	/*
	 * 内部页分裂会截断右页上的第一个数据项 -- 它
	 * 成为该页的"负无穷"项。在这里把它设置好。
	 */
	minusinfoff = InvalidOffsetNumber;
	if (!isleaf)
		minusinfoff = afterrightoff;

	/*
	 * 现在把所有数据项（isleaf 情形下的非枢轴元组，或
	 * !isleaf 情形下的其余枢轴元组）转移到适当的页上。
	 *
	 * 注意：为了 _bt_restore_page()，我们*必须*至少按项号
	 * 顺序插入右页上的各项。
	 */
	for (i = P_FIRSTDATAKEY(oopaque); i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	dataitem;

		itemid = PageGetItemId(origpage, i);
		itemsz = ItemIdGetLength(itemid);
		dataitem = (IndexTuple) PageGetItem(origpage, itemid);

		/* 因倒排分裂而用 nposting 替换原始项？ */
		if (i == origpagepostingoff)
		{
			Assert(BTreeTupleIsPosting(dataitem));
			Assert(itemsz == MAXALIGN(IndexTupleSize(nposting)));
			dataitem = nposting;
		}

		/* 新项是否应当排在这一项之前？ */
		else if (i == newitemoff)
		{
			if (newitemonleft)
			{
				Assert(newitemoff <= firstrightoff);
				if (!_bt_pgaddtup(leftpage, newitemsz, newitem, afterleftoff,
								  false))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the left sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				afterleftoff = OffsetNumberNext(afterleftoff);
			}
			else
			{
				Assert(newitemoff >= firstrightoff);
				if (!_bt_pgaddtup(rightpage, newitemsz, newitem, afterrightoff,
								  afterrightoff == minusinfoff))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the right sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				afterrightoff = OffsetNumberNext(afterrightoff);
			}
		}

		/* 决定把它放到哪一页 */
		if (i < firstrightoff)
		{
			if (!_bt_pgaddtup(leftpage, itemsz, dataitem, afterleftoff, false))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the left sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			afterleftoff = OffsetNumberNext(afterleftoff);
		}
		else
		{
			if (!_bt_pgaddtup(rightpage, itemsz, dataitem, afterrightoff,
							  afterrightoff == minusinfoff))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the right sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			afterrightoff = OffsetNumberNext(afterrightoff);
		}
	}

	/* 处理 newitem 位于 rightpage 末尾的情形 */
	if (i <= newitemoff)
	{
		/*
		 * 这里不可能有 newitemonleft；那将意味着我们被要求把
		 * *所有东西*都放到左页，而左页放不下（若放得下，我们就
		 * 不会分裂该页）。
		 */
		Assert(!newitemonleft && newitemoff == maxoff + 1);
		if (!_bt_pgaddtup(rightpage, newitemsz, newitem, afterrightoff,
						  afterrightoff == minusinfoff))
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add new item to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		afterrightoff = OffsetNumberNext(afterrightoff);
	}

	/*
	 * 我们必须抓住原来的右兄弟（若有），并更新它的 prev
	 * 链接。这保证不会死锁，因为我们按标准顺序
	 * 耦合这些锁：从左到右。
	 */
	if (!isrightmost)
	{
		sbuf = _bt_getbuf(rel, oopaque->btpo_next, BT_WRITE);
		spage = BufferGetPage(sbuf);
		sopaque = BTPageGetOpaque(spage);
		if (sopaque->btpo_prev != origpagenumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("right sibling's left-link doesn't match: "
									 "block %u links to %u instead of expected %u in index \"%s\"",
									 oopaque->btpo_next, sopaque->btpo_prev, origpagenumber,
									 RelationGetRelationName(rel))));
		}

		/*
		 * 看看能否在右侧
		 * 分裂页上设置 SPLIT_END 标志；这能为清理省下一些 I/O，因为它不必
		 * 继续走到右兄弟。若右
		 * 兄弟的 cycleid 不同，就可以设置该标志：那意味着它无法属于
		 * 从同一祖先页全部分裂出来的
		 * 那一组页。若觉得不好懂，设想页 A 分裂成 A B，
		 * 然后再次分裂，得到 A C B，同时清理正在进行。原先在 A 中的元组
		 * 现在可能在 B 或 C 中，因此清理必须
		 * 检查这两页。但若 D，也就是我们的右兄弟，有不同的
		 * cycleid，则它无法包含清理开始时
		 * 还在 A 中的任何元组。
		 */
		if (sopaque->btpo_cycleid != ropaque->btpo_cycleid)
			ropaque->btpo_flags |= BTP_SPLIT_END;
	}

	/*
	 * 右兄弟已锁定，新的兄弟页已准备好，但原始页
	 * 尚未更新。
	 *
	 * 在右兄弟更新之前，不要 EREPORT(ERROR)。我们到这里才开始
	 * 临界区也没问题，因为我们还没有在原始页上
	 * 乱写；见上面的注释。
	 */
	START_CRIT_SECTION();

	/*
	 * 到这里，原始数据页已经分裂成两个新的半页，并且
	 * 它们是正确的。算法需要左页在分裂期间永不
	 * 移动，因此我们把新的左页复制回
	 * 原始页之上。必须在写 WAL 记录之前做这件事，以便
	 * 必要时 XLogInsert 能把该页的镜像记入 WAL。
	 */
	PageRestoreTempPage(leftpage, origpage);
	/* 此后不得再使用 leftpage、lopaque */

	MarkBufferDirty(buf);
	MarkBufferDirty(rbuf);

	if (!isrightmost)
	{
		sopaque->btpo_prev = rightpagenumber;
		MarkBufferDirty(sbuf);
	}

	/*
	 * 若插入新项完成了一次分裂，则清除子页上的
	 * INCOMPLETE_SPLIT 标志
	 */
	if (!isleaf)
	{
		Page		cpage = BufferGetPage(cbuf);
		BTPageOpaque cpageop = BTPageGetOpaque(cpage);

		cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
		MarkBufferDirty(cbuf);
	}

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_split xlrec;
		uint8		xlinfo;
		XLogRecPtr	recptr;

		xlrec.level = ropaque->btpo_level;
		/* 关于 newitem、orignewitem 和倒排列表，见下面的注释 */
		xlrec.firstrightoff = firstrightoff;
		xlrec.newitemoff = newitemoff;
		xlrec.postingoff = 0;
		if (postingoff != 0 && origpagepostingoff < firstrightoff)
			xlrec.postingoff = postingoff;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBtreeSplit);

		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBuffer(1, rbuf, REGBUF_WILL_INIT);
		/* 记录原来的右兄弟，因为我们改了它的 prev 指针 */
		if (!isrightmost)
			XLogRegisterBuffer(2, sbuf, REGBUF_STANDARD);
		if (!isleaf)
			XLogRegisterBuffer(3, cbuf, REGBUF_STANDARD);

		/*
		 * 若新项插在左页上，就把它记入日志。（若它被
		 * 放到右页上，就不必显式为它写 WAL，
		 * 因为它已经包含在右页上的所有其他项之中。）
		 * 把新项显示为属于左页缓冲区，这样若
		 * XLogInsert 决定需要左页的整页镜像，就不会再存储它。
		 * 不过我们总是在记录中存放 newitemoff。
		 *
		 * 与倒排列表分裂同时发生的页分裂，细节有时略有不同。若替换用的
		 * 倒排列表和 newitem 都到了右页，就不需要
		 * 额外记录任何东西，就像简单的 !newitemonleft、
		 * 没有倒排分裂的情形一样（WAL 记录中的 postingoff 被设为零，
		 * 因此恢复完全不必处理倒排列表分裂）。
		 * 否则，我们设置 postingoff，并记录 orignewitem 而不是
		 * newitem，尽管实际插入的是 newitem。REDO 例程
		 * 必须用 _bt_swap_posting() 重建 nposting 和 newitem。
		 * 注意：我们的页分裂点有可能正是使
		 *
		 * 倒排列表成为 lastleft、newitem 成为 firstright 的那个点。
		 * 这是
		 * 唯一即使 newitem 到了右页仍记录 orignewitem/newitem 的情形。
		 * 若 XLogInsert 决定因记录了左页的整页镜像而可以省略
		 * orignewitem，
		 * 一切仍然成立，因为恢复只需要为左页上的项记录
		 * orignewitem（与常规的
		 * 记录 newitem 的情形一样）。
		 */
		if (newitemonleft && xlrec.postingoff == 0)
			XLogRegisterBufData(0, (char *) newitem, newitemsz);
		else if (xlrec.postingoff != 0)
		{
			Assert(isleaf);
			Assert(newitemonleft || firstrightoff == newitemoff);
			Assert(newitemsz == IndexTupleSize(orignewitem));
			XLogRegisterBufData(0, (char *) orignewitem, newitemsz);
		}

		/* 记录左页的新高键 */
		if (!isleaf)
		{
			/* lefthighkey 不是本地副本，取当前指针 */
			itemid = PageGetItemId(origpage, P_HIKEY);
			lefthighkey = (IndexTuple) PageGetItem(origpage, itemid);
		}
		XLogRegisterBufData(0, (char *) lefthighkey,
							MAXALIGN(IndexTupleSize(lefthighkey)));

		/*
		 * 按 _bt_restore_page() 能理解的格式写入右页的内容。
		 * 整个右页都会被重建。
		 *
		 * 直接访问页不太好，但更快 -- 我们应当在页 API 中实现
		 * 某个新函数。注意，我们只存储元组
		 * 本身，因为知道它们是按项号顺序插入的，
		 * 因此行指针可以重建。见
		 * _bt_restore_page() 的注释。
		 */
		XLogRegisterBufData(1,
							(char *) rightpage + ((PageHeader) rightpage)->pd_upper,
							((PageHeader) rightpage)->pd_special - ((PageHeader) rightpage)->pd_upper);

		xlinfo = newitemonleft ? XLOG_BTREE_SPLIT_L : XLOG_BTREE_SPLIT_R;
		recptr = XLogInsert(RM_BTREE_ID, xlinfo);

		PageSetLSN(origpage, recptr);
		PageSetLSN(rightpage, recptr);
		if (!isrightmost)
			PageSetLSN(spage, recptr);
		if (!isleaf)
			PageSetLSN(BufferGetPage(cbuf), recptr);
	}

	END_CRIT_SECTION();

	/* 释放原来的右兄弟 */
	if (!isrightmost)
		_bt_relbuf(rel, sbuf);

	/* 释放子页 */
	if (!isleaf)
		_bt_relbuf(rel, cbuf);

	/* 保持整洁 */
	if (isleaf)
		pfree(lefthighkey);

	/* 分裂完成 */
	return rbuf;
}

/*
 * _bt_insert_parent() -- 把下行链接插入父页，完成分裂。
 *
 * 进入时，buf 和 rbuf 是分裂后的左右页，我们
 * 仍对它们持有写锁。两个锁都会在这里释放。一旦我们对打算
 * 把指向 rbuf 的下行链接插入进去的那一页（即 buf 当前的父页）拿到写锁，
 * 就释放 rbuf 上的锁。
 * buf 上的锁与父页上的锁在同一时刻释放，
 * 因为必须由同一次
 * 原子操作清除 buf 的 INCOMPLETE_SPLIT 标志，并通过插入新的下行链接来完成分裂。
 *
 * stack - 说明我们如何到达这里的栈。分裂真正的
 *			根时，或在并发根分裂期间，它为 NULL，那时我们可以不那么高效
 * isroot - 我们分裂了真正的根
 * isonly - 我们分裂了其所在层上单独的一页（它也许曾是快速根）
 */
static void
_bt_insert_parent(Relation rel,
				  Relation heaprel,
				  Buffer buf,
				  Buffer rbuf,
				  BTStack stack,
				  bool isroot,
				  bool isonly)
{
	Assert(heaprel != NULL);

	/*
	 * 这里我们必须做一件 Lehman 与 Yao 没有谈到的事：处理
	 * 根分裂并构造新根。若栈为空，
	 * 则我们刚刚分裂的结点，在我们
	 * 沿树下降时还位于根层。若它仍然是根，我们就执行
	 * 新根构造。若它*已经不再*是根，就搜索找出
	 * 其间别人构造的下一个更高层，并像正常情形那样找到
	 * 插入的正确位置。
	 *
	 * 若必须搜索父层，我们就从根
	 * 重新下降。这并不特别高效，但足够少见，
	 * 无关紧要。
	 */
	if (isroot)
	{
		Buffer		rootbuf;

		Assert(stack == NULL);
		Assert(isonly);
		/* 在上一层创建一个新的根结点并更新元数据页 */
		rootbuf = _bt_newlevel(rel, heaprel, buf, rbuf);
		/* 释放分裂缓冲区 */
		_bt_relbuf(rel, rootbuf);
		_bt_relbuf(rel, rbuf);
		_bt_relbuf(rel, buf);
	}
	else
	{
		BlockNumber bknum = BufferGetBlockNumber(buf);
		BlockNumber rbknum = BufferGetBlockNumber(rbuf);
		Page		page = BufferGetPage(buf);
		IndexTuple	new_item;
		BTStackData fakestack;
		IndexTuple	ritem;
		Buffer		pbuf;

		if (stack == NULL)
		{
			BTPageOpaque opaque;

			elog(DEBUG2, "concurrent ROOT page split");
			opaque = BTPageGetOpaque(page);

			/*
			 * 当叶子页分裂发生时，我们绝不应当到达这里，
			 * 尽管 newitem 的插入本可以应用 fastpath
			 * 优化。用一条断言来确认这一点。
			 *
			 * 这更多是性能问题，而不是正确性问题。
			 * fastpath 不会有下降栈。在这里使用一个假栈
			 * 也能工作，但绝不要依赖这一点。当最右叶子
			 * 页将要分裂时，应当在 _bt_search_insert() 内拒绝 fastpath，
			 * 因为走 _bt_search() 并按通常方式
			 * 拿到栈会更快。
			 */
			Assert(!(P_ISLEAF(opaque) &&
					 BlockNumberIsValid(RelationGetTargetBlock(rel))));

			/* 找到上一层最左边的页 */
			pbuf = _bt_get_endpoint(rel, opaque->btpo_level + 1, false, NULL);
			/* 设置一个指向那里的假栈项 */
			stack = &fakestack;
			stack->bts_blkno = BufferGetBlockNumber(pbuf);
			stack->bts_offset = InvalidOffsetNumber;
			stack->bts_parent = NULL;
			_bt_relbuf(rel, pbuf);
		}

		/* 从左页取高键，它是新右页的严格下界 */
		ritem = (IndexTuple) PageGetItem(page,
										 PageGetItemId(page, P_HIKEY));

		/* 构造一个指向新右页的索引元组 */
		new_item = CopyIndexTuple(ritem);
		BTreeTupleSetDownLink(new_item, rbknum);

		/*
		 * 重新找到 buf 的父页并加写锁。
		 *
		 * 自我们最初的 _bt_search() 下降以来，
		 * buf 的下行链接位置可能已经改变。
		 * _bt_getstackbuf() 会检测并从此恢复，更新栈，从而确保
		 * 新的下行链接会被插到正确的偏移。甚至 buf 的
		 * 父页也可能已经改变。
		 */
		pbuf = _bt_getstackbuf(rel, heaprel, stack, bknum);

		/*
		 * 解锁右孩子。左孩子将在
		 * _bt_insertonpg() 中解锁。
		 *
		 * 必须把解锁右孩子推迟到这里，以确保
		 * 并发的 VACUUM 操作不会被搞糊涂。无法允许页删除
		 * 找不到指向 rbuf 页的下行链接。
		 * （实际上，这只是旧做法留下的痕迹。
		 * 页删除代码预期会检查左孩子上的 INCOMPLETE_SPLIT
		 * 标志。在分裂完成之前，它不会尝试删除右
		 * 孩子。尽管如此，我们仍选择
		 * 保守地把解锁右孩子推迟到这里。）
		 */
		_bt_relbuf(rel, rbuf);

		if (pbuf == InvalidBuffer)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("failed to re-find parent key in index \"%s\" for split pages %u/%u",
									 RelationGetRelationName(rel), bknum, rbknum)));

		/* 递归插入到父页 */
		_bt_insertonpg(rel, heaprel, NULL, pbuf, buf, stack->bts_parent,
					   new_item, MAXALIGN(IndexTupleSize(new_item)),
					   stack->bts_offset + 1, 0, isonly);

		/* 保持整洁 */
		pfree(new_item);
	}
}

/*
 * _bt_finish_split() -- 完成一次未完成分裂
 *
 * 崩溃或其他失败可能留下未完成的分裂。插入
 * 例程不允许在分裂尚未完成的页上插入。
 * 在这样的页上插入之前，调用 _bt_finish_split()。
 *
 * 进入时，'lbuf' 必须以写模式锁定。退出时，它被解锁
 * 并解除钉住。
 *
 * 调用者必须提供有效的 heaprel，因为完成页分裂需要
 * 在父页接着分裂时分配新页。
 */
void
_bt_finish_split(Relation rel, Relation heaprel, Buffer lbuf, BTStack stack)
{
	Page		lpage = BufferGetPage(lbuf);
	BTPageOpaque lpageop = BTPageGetOpaque(lpage);
	Buffer		rbuf;
	Page		rpage;
	BTPageOpaque rpageop;
	bool		wasroot;
	bool		wasonly;

	Assert(P_INCOMPLETE_SPLIT(lpageop));
	Assert(heaprel != NULL);

	/* 锁定右兄弟，也就是缺少下行链接的那一页 */
	rbuf = _bt_getbuf(rel, lpageop->btpo_next, BT_WRITE);
	rpage = BufferGetPage(rbuf);
	rpageop = BTPageGetOpaque(rpage);

	/* 这会不会是根分裂？ */
	if (!stack)
	{
		Buffer		metabuf;
		Page		metapg;
		BTMetaPageData *metad;

		/* 获取元数据页上的锁 */
		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
		metapg = BufferGetPage(metabuf);
		metad = BTPageGetMeta(metapg);

		wasroot = (metad->btm_root == BufferGetBlockNumber(lbuf));

		_bt_relbuf(rel, metabuf);
	}
	else
		wasroot = false;

	/* 分裂之前，这一层是不是只有这一页？ */
	wasonly = (P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop));

	elog(DEBUG1, "finishing incomplete split of %u/%u",
		 BufferGetBlockNumber(lbuf), BufferGetBlockNumber(rbuf));

	_bt_insert_parent(rel, heaprel, lbuf, rbuf, stack, wasroot, wasonly);
}

/*
 *	_bt_getstackbuf() -- 沿树向上走一步，找到其下行链接指向子页的枢轴
 *						 元组。
 *
 *		调用者传入子页的块号，用来在父页中通过线性搜索识别
 *		相关联的枢轴元组，匹配枢轴的下行链接/块号。
 *		枢轴元组的预期位置取自子页上一层的栈。
 *		以此作为起点。向父
 *		层的插入可能使枢轴元组向右移动；删除
 *		可能使它向左移动，但不会移到我们先前
 *		找到它的那一页的左边。
 *		调用者可以用它的栈，为初始下降所找到的那一页
 *
 *		右侧、同一层上的任何页，重新定位枢轴元组/下行链接。
 *		这是必要的，因为调用者有可能
 *		向右移动，以便从并发页分裂中恢复。
 *		对某些调用者来说，
 *		在并没有并发页分裂时仍能向右走一步、同时继续使用原来的
 *		栈，也很方便。例如，checkingunique 的 _bt_doinsert() 情形可能
 *		不得不在有许多物理重复项时向右走，而其
 *		scantid 迫使插入发生在"该值可能位于的第一页"
 *		的右侧。（我们的所有调用者在处理 !heapkeyspace 索引时
 *		也都依赖这一点。）
 *		返回已加写锁的父页缓冲区；若找不到枢轴元组
 *
 *		则返回 InvalidBuffer（不应当发生）。若有变化，
 *		调整 bts_blkno 与
 *		bts_offset。页分裂的调用者应当在父页上、于偏移号
 *		bts_offset + 1 处，为其新的右兄弟页插入新的
 *		枢轴元组。
 */
Buffer
_bt_getstackbuf(Relation rel, Relation heaprel, BTStack stack, BlockNumber child)
{
	BlockNumber blkno;
	OffsetNumber start;

	blkno = stack->bts_blkno;
	start = stack->bts_offset;

	for (;;)
	{
		Buffer		buf;
		Page		page;
		BTPageOpaque opaque;

		buf = _bt_getbuf(rel, blkno, BT_WRITE);
		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);

		Assert(heaprel != NULL);
		if (P_INCOMPLETE_SPLIT(opaque))
		{
			_bt_finish_split(rel, heaprel, buf, stack->bts_parent);
			continue;
		}

		if (!P_IGNORE(opaque))
		{
			OffsetNumber offnum,
						minoff,
						maxoff;
			ItemId		itemid;
			IndexTuple	item;

			minoff = P_FIRSTDATAKEY(opaque);
			maxoff = PageGetMaxOffsetNumber(page);

			/*
			 * start = InvalidOffsetNumber 表示"搜索整页"。
			 * 由于该页现在可能有高键、而以前没有，
			 * 我们反正需要这个测试。
			 */
			if (start < minoff)
				start = minoff;

			/*
			 * 也需要这个检查，以防该页在我们最初访问之后
			 * 又发生了分裂。
			 */
			if (start > maxoff)
				start = OffsetNumberNext(maxoff);

			/*
			 * 这些循环会检查该页上的每一项 --- 但顺序
			 * 会偏向于它实际所在位置的概率。
			 * 先向右扫，再向左扫。
			 */
			for (offnum = start;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (BTreeTupleGetDownLink(item) == child)
				{
					/* 返回指向该链接现在位置的准确指针 */
					stack->bts_blkno = blkno;
					stack->bts_offset = offnum;
					return buf;
				}
			}

			for (offnum = OffsetNumberPrev(start);
				 offnum >= minoff;
				 offnum = OffsetNumberPrev(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (BTreeTupleGetDownLink(item) == child)
				{
					/* 返回指向该链接现在位置的准确指针 */
					stack->bts_blkno = blkno;
					stack->bts_offset = offnum;
					return buf;
				}
			}
		}

		/*
		 * 我们要找的项至少向右移动了一页。
		 *
		 * Lehman 与 Yao 在这里向右移动时会耦合/链式加锁，我们可以
		 * 避免这样做。见 nbtree/README。
		 */
		if (P_RIGHTMOST(opaque))
		{
			_bt_relbuf(rel, buf);
			return InvalidBuffer;
		}
		blkno = opaque->btpo_next;
		start = InvalidOffsetNumber;
		_bt_relbuf(rel, buf);
	}
}

/*
 *	_bt_newlevel() -- 在根页之上创建新的一层。
 *
 *		我们刚刚分裂了旧根页，需要创建新的根页。
 *		为此，我们向文件中加入新的根页，然后锁定
 *		元数据页并更新它。这保证不会
 *		死锁，
 *		因为所有读者在试图锁定根之前都会释放元数据页上的锁，
 *		而所有写者在试图锁定元数据页之前都会先锁定根。
 *		我们对旧
 *		根页持有写锁，因此没有在等待图中引入任何环。
 *
 *		进入时，lbuf（旧根）和 rbuf（它的新对等页）已加
 *		写锁。退出时，存在一张含有两个新孩子对应项的新根页，
 *		元数据页已更新并解锁/解除钉住。
 *		新根缓冲区返回给调用者，调用者必须解锁/解除钉住
 *		lbuf、rbuf 和 rootbuf。
 */
static Buffer
_bt_newlevel(Relation rel, Relation heaprel, Buffer lbuf, Buffer rbuf)
{
	Buffer		rootbuf;
	Page		lpage,
				rootpage;
	BlockNumber lbkno,
				rbkno;
	BlockNumber rootblknum;
	BTPageOpaque rootopaque;
	BTPageOpaque lopaque;
	ItemId		itemid;
	IndexTuple	item;
	IndexTuple	left_item;
	Size		left_item_sz;
	IndexTuple	right_item;
	Size		right_item_sz;
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;

	lbkno = BufferGetBlockNumber(lbuf);
	rbkno = BufferGetBlockNumber(rbuf);
	lpage = BufferGetPage(lbuf);
	lopaque = BTPageGetOpaque(lpage);

	/* 取得一张新的根页 */
	rootbuf = _bt_allocbuf(rel, heaprel);
	rootpage = BufferGetPage(rootbuf);
	rootblknum = BufferGetBlockNumber(rootbuf);

	/* 获取元数据页上的锁 */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);

	/*
	 * 为左页（旧根）创建下行链接项。所用的键值是
	 * "负无穷"，一个可靠地小于左页上可能出现的任何真实
	 * 键值的哨兵值。
	 */
	left_item_sz = sizeof(IndexTupleData);
	left_item = (IndexTuple) palloc(left_item_sz);
	left_item->t_info = left_item_sz;
	BTreeTupleSetDownLink(left_item, lbkno);
	BTreeTupleSetNAtts(left_item, 0, false);

	/*
	 * 为右页创建下行链接项。它的键取自
	 * 左页中的"高键"位置。
	 */
	itemid = PageGetItemId(lpage, P_HIKEY);
	right_item_sz = ItemIdGetLength(itemid);
	item = (IndexTuple) PageGetItem(lpage, itemid);
	right_item = CopyIndexTuple(item);
	BTreeTupleSetDownLink(right_item, rbkno);

	/* 从这里直到 newroot 操作被记入日志之前，不要 EREPORT(ERROR) */
	START_CRIT_SECTION();

	/* 必要时升级元数据页 */
	if (metad->btm_version < BTREE_NOVAC_VERSION)
		_bt_upgrademetapage(metapg);

	/* 设置 B-树专用数据 */
	rootopaque = BTPageGetOpaque(rootpage);
	rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
	rootopaque->btpo_flags = BTP_ROOT;
	rootopaque->btpo_level =
		(BTPageGetOpaque(lpage))->btpo_level + 1;
	rootopaque->btpo_cycleid = 0;

	/* 更新元数据页数据 */
	metad->btm_root = rootblknum;
	metad->btm_level = rootopaque->btpo_level;
	metad->btm_fastroot = rootblknum;
	metad->btm_fastlevel = rootopaque->btpo_level;

	/*
	 * 把左页指针插入新根页。根页是
	 * 其所在层的最右页，因此其中没有"高键"；
	 * 两项将进入 P_HIKEY 和 P_FIRSTKEY 位置。
	 *
	 * 注意：为了 _bt_restore_page()，我们*必须*按项号顺序
	 * 插入这两项。
	 */
	Assert(BTreeTupleGetNAtts(left_item, rel) == 0);
	if (PageAddItem(rootpage, (Item) left_item, left_item_sz, P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(PANIC, "failed to add leftkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/*
	 * 把右页指针插入新根页。
	 */
	Assert(BTreeTupleGetNAtts(right_item, rel) > 0);
	Assert(BTreeTupleGetNAtts(right_item, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));
	if (PageAddItem(rootpage, (Item) right_item, right_item_sz, P_FIRSTKEY,
					false, false) == InvalidOffsetNumber)
		elog(PANIC, "failed to add rightkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/* 清除左孩子中的未完成分裂标志 */
	Assert(P_INCOMPLETE_SPLIT(lopaque));
	lopaque->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
	MarkBufferDirty(lbuf);

	MarkBufferDirty(rootbuf);
	MarkBufferDirty(metabuf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_newroot xlrec;
		XLogRecPtr	recptr;
		xl_btree_metadata md;

		xlrec.rootblk = rootblknum;
		xlrec.level = metad->btm_level;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBtreeNewroot);

		XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, lbuf, REGBUF_STANDARD);
		XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
		md.version = metad->btm_version;
		md.root = rootblknum;
		md.level = metad->btm_level;
		md.fastroot = rootblknum;
		md.fastlevel = metad->btm_level;
		md.last_cleanup_num_delpages = metad->btm_last_cleanup_num_delpages;
		md.allequalimage = metad->btm_allequalimage;

		XLogRegisterBufData(2, (char *) &md, sizeof(xl_btree_metadata));

		/*
		 * 直接访问页不太好，但更快 -- 我们应当在页 API 中实现
		 * 某个新函数。
		 */
		XLogRegisterBufData(0,
							(char *) rootpage + ((PageHeader) rootpage)->pd_upper,
							((PageHeader) rootpage)->pd_special -
							((PageHeader) rootpage)->pd_upper);

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT);

		PageSetLSN(lpage, recptr);
		PageSetLSN(rootpage, recptr);
		PageSetLSN(metapg, recptr);
	}

	END_CRIT_SECTION();

	/* 元数据页用完了 */
	_bt_relbuf(rel, metabuf);

	pfree(left_item);
	pfree(right_item);

	return rootbuf;
}

/*
 *	_bt_pgaddtup() -- 在分裂期间把一个数据项加到某一页。
 *
 *		本例程与一次单纯的 PageAddItem 调用的差别在于，
 *		这段代码可以顺带处理内部 B-树
 *		页上的第一个数据项。这个数据项（在
 *		_bt_split() 中称为 "firstright"）的键在
 *		分裂之后必须被当作负无穷。因此，当调用者
 *		指明它是页上的第一个数据项时，我们截掉全部属性
 *		（下行链接并不改变）。
 *		这个额外步骤只对内部页分裂的右页需要。
 *		对已有/左页上的第一个数据项不必这样做，因为它在更早的页分裂中
 *		已经被截断过了。
 *
 *		关于为什么在这里截断，见 _bt_split() 的高层说明。
 *		注意，本例程与后缀截断无关，
 *		尽管用到了一些相同的基础设施。
 */
static inline bool
_bt_pgaddtup(Page page,
			 Size itemsize,
			 IndexTuple itup,
			 OffsetNumber itup_off,
			 bool newfirstdataitem)
{
	IndexTupleData trunctuple;

	if (newfirstdataitem)
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (unlikely(PageAddItem(page, (Item) itup, itemsize, itup_off, false,
							 false) == InvalidOffsetNumber))
		return false;

	return true;
}

/*
 * _bt_delete_or_dedup_one_page - 尽量避免叶子页分裂。
 *
 * 这里执行三种操作：简单索引删除、自底向上
 * 索引删除，以及去重。若三种操作都无法为到来的项腾出
 * 足够空间，调用者就会继续分裂该
 * 页。我们总是先考虑简单删除。若那行不通，
 * 再考虑其他办法。只想让我们考虑简单
 * 删除（没有任何后备手段）的调用者，用 'simpleonly'
 * 参数来提出这一要求。
 *
 * 当仅靠简单删除无法阻止页分裂时，我们通常只挑选一种备选的"复杂"操作。
 * 'checkingunique'、
 * 'uniquedup' 和 'indexUnchanged' 参数就是用来做这个选择的。
 *
 * 注意：我们过去只在发现设置了页级 BTP_HAS_GARBAGE
 * 标志时才删除 LP_DEAD 项。在重复元组不一定有单独一页可去的时候
 * （版本 4 索引把堆 TID
 * 纳入键空间之前），这个标志是有用的。但我们
 * 实际上已经不再看这个标志（它不是调用者的门禁条件）。
 * 那样会漏掉可以安全删除的元组，
 * 却得不到任何回报。我们知道另一种选择是
 * 分裂该页；顺带扫描行指针数组不会有
 * 明显开销。（尽管如此，我们仍然维护 BTP_HAS_GARBAGE 标志，
 * 因为 !heapkeyspace 索引仍必须做"感到厌倦"式的
 * 线性搜索，因此把它用作
 * 门禁条件很可能仍能得到一些好处。）
 */
static void
_bt_delete_or_dedup_one_page(Relation rel, Relation heapRel,
							 BTInsertState insertstate,
							 bool simpleonly, bool checkingunique,
							 bool uniquedup, bool indexUnchanged)
{
	OffsetNumber deletable[MaxIndexTuplesPerPage];
	int			ndeletable = 0;
	OffsetNumber offnum,
				minoff,
				maxoff;
	Buffer		buffer = insertstate->buf;
	BTScanInsert itup_key = insertstate->itup_key;
	Page		page = BufferGetPage(buffer);
	BTPageOpaque opaque = BTPageGetOpaque(page);

	Assert(P_ISLEAF(opaque));
	Assert(simpleonly || itup_key->heapkeyspace);
	Assert(!simpleonly || (!checkingunique && !uniquedup && !indexUnchanged));

	/*
	 * 扫描所有项，看根据
	 * LP_DEAD 标志哪些需要删除。通常我们还能顺带多删掉几个
	 * 并未标记为 LP_DEAD 的项。实际最终被删掉的那些额外项，
	 * 往往是很快也会被设置 LP_DEAD 位的项
	 * （若我们选择不把它们当作额外项包括进来）。
	 */
	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsDead(itemId))
			deletable[ndeletable++] = offnum;
	}

	if (ndeletable > 0)
	{
		_bt_simpledel_pass(rel, buffer, heapRel, deletable, ndeletable,
						   insertstate->itup, minoff, maxoff);
		insertstate->bounds_valid = false;

		/* 若已经避免了页分裂，则返回 */
		if (PageGetFreeSpace(page) >= insertstate->itemsz)
			return;

		/* 不妨假定存在重复项（若 checkingunique） */
		uniquedup = true;
	}

	/*
	 * 简单删除已经做完。对那些只是为了让简单删除得到考虑才
	 * 调用这里的调用者提前返回。这包括
	 * 明确提出这一要求的调用者，以及页上大概没有任何版本更迭重复项的
	 * checkingunique 调用者。
	 *
	 * 注意：当我们在这里返回时（或者当我们继续尝试另外两种策略中的一种或两种、
	 * 而它们也失败时），该页的 BTP_HAS_GARBAGE 提示标志可能仍然设置着。
	 * 不过我们并不费一次单独的写操作去清除它。
	 * 调用者在继续分裂该页时一定会清除它
	 * （也请注意，去重
	 * 过程会顺带清除该标志，只是为了保持整洁）。
	 */
	if (simpleonly || (checkingunique && !uniquedup))
	{
		Assert(!indexUnchanged);
		return;
	}

	/* 假定边界即将失效（现在这几乎是肯定的） */
	insertstate->bounds_valid = false;

	/*
	 * 当执行器提示表明到来的项在逻辑上未改变时，
	 * 或者对因其他原因已知有物理重复项的唯一索引，
	 * 执行自底向上索引删除趟。
	 * （对唯一索引，这两种情形有很大重叠。
	 * 两种触发条件都保留是值得的，以便在连续的相关
	 * INSERT 与 DELETE 语句下也能应用该优化。）
	 *
	 * 当自底向上的一趟未能删掉可接受数量的空闲空间时，
	 * 我们会继续做去重趟（页的一个可观部分，
	 * 或新项所需的空间，取较大者）。
	 *
	 * 注意：自底向上索引删除在内部使用与去重相同的相等/等价
	 * 例程。不过它并不把索引元组合并到一起，
	 * 因此同样的正确性考量并不适用。
	 * 这里我们有意省略索引是否 allequalimage 的测试。
	 */
	if ((indexUnchanged || uniquedup) &&
		_bt_bottomupdel_pass(rel, buffer, heapRel, insertstate->itemsz))
		return;

	/* 执行去重趟（在启用且索引为 allequalimage 时） */
	if (BTGetDeduplicateItems(rel) && itup_key->allequalimage)
		_bt_dedup_pass(rel, buffer, insertstate->itup, insertstate->itemsz,
					   (indexUnchanged || uniquedup));
}

/*
 * _bt_simpledel_pass - 简单索引元组删除趟。
 *
 * 我们删除叶子页上所有设置了 LP_DEAD 的索引元组。
 * 所有这些元组的偏移号
 * 由调用者确定（调用者把它们作为
 *
 * 'deletable' 参数传给我们）。
 * 我们也许还会顺带删除一些结果可以安全删除的额外索引元组
 * （不过它们一开始就必须能便宜地顺带检查）。
 * 并不能确定一定会删掉任何额外元组。我们这种方法的
 * 高层目标是让这里的每次调用都尽量物有所值
 * （与仅仅为了能删除该页上标记了 LP_DEAD 的索引元组
 * 所必须做的事相比，不明显增加每次调用的开销）。
 *
 * 结果可删除的额外索引元组数量，可能由于各种
 * 与局部性有关的效应，大大超过标记了 LP_DEAD 的索引元组数量。
 * 例如，该叶子页上所有 TIDs 指向的
 * 表块总数
 * 有可能自然就相当低，此时我们也许最终会检查是否可以
 * 删除该页上 _most_ 索引元组（而 tableam 不必
 * 访问额外的表块）。在这种模式很常见的索引中，
 * tableam 有时会碰到
 * _many_ 额外的可删除索引元组。
 *
 * 关于简单索引元组删除的更多细节，见 nbtree/README。
 */
static void
_bt_simpledel_pass(Relation rel, Buffer buffer, Relation heapRel,
				   OffsetNumber *deletable, int ndeletable, IndexTuple newitem,
				   OffsetNumber minoff, OffsetNumber maxoff)
{
	Page		page = BufferGetPage(buffer);
	BlockNumber *deadblocks;
	int			ndeadblocks;
	TM_IndexDeleteOp delstate;
	OffsetNumber offnum;

	/* 取得由设置了 LP_DEAD 的元组所指向的表块数组 */
	deadblocks = _bt_deadblocks(page, deletable, ndeletable, newitem,
								&ndeadblocks);

	/* 初始化描述索引删除操作的 tableam 状态 */
	delstate.irel = rel;
	delstate.iblknum = BufferGetBlockNumber(buffer);
	delstate.bottomup = false;
	delstate.bottomupfreespace = 0;
	delstate.ndeltids = 0;
	delstate.deltids = palloc(MaxTIDsPerBTreePage * sizeof(TM_IndexDelete));
	delstate.status = palloc(MaxTIDsPerBTreePage * sizeof(TM_IndexStatus));

	for (offnum = minoff;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);
		TM_IndexDelete *odeltid = &delstate.deltids[delstate.ndeltids];
		TM_IndexStatus *ostatus = &delstate.status[delstate.ndeltids];
		BlockNumber tidblock;
		void	   *match;

		if (!BTreeTupleIsPosting(itup))
		{
			tidblock = ItemPointerGetBlockNumber(&itup->t_tid);
			match = bsearch(&tidblock, deadblocks, ndeadblocks,
							sizeof(BlockNumber), _bt_blk_cmp);

			if (!match)
			{
				Assert(!ItemIdIsDead(itemid));
				continue;
			}

			/*
			 * 该 TID 的表块属于页上设置了 LP_DEAD 位的 TIDs 所指向的那些块
			 * -- 把该 TID 加入 deltids
			 */
			odeltid->tid = itup->t_tid;
			odeltid->id = delstate.ndeltids;
			ostatus->idxoffnum = offnum;
			ostatus->knowndeletable = ItemIdIsDead(itemid);
			ostatus->promising = false; /* 未使用 */
			ostatus->freespace = 0; /* 未使用 */

			delstate.ndeltids++;
		}
		else
		{
			int			nitem = BTreeTupleGetNPosting(itup);

			for (int p = 0; p < nitem; p++)
			{
				ItemPointer tid = BTreeTupleGetPostingN(itup, p);

				tidblock = ItemPointerGetBlockNumber(tid);
				match = bsearch(&tidblock, deadblocks, ndeadblocks,
								sizeof(BlockNumber), _bt_blk_cmp);

				if (!match)
				{
					Assert(!ItemIdIsDead(itemid));
					continue;
				}

				/*
				 * 该 TID 的表块属于页上设置了
				 * LP_DEAD 位的 TIDs 所指向的那些块 -- 把该 TID 加入 deltids
				 */
				odeltid->tid = *tid;
				odeltid->id = delstate.ndeltids;
				ostatus->idxoffnum = offnum;
				ostatus->knowndeletable = ItemIdIsDead(itemid);
				ostatus->promising = false; /* 未使用 */
				ostatus->freespace = 0; /* 未使用 */

				odeltid++;
				ostatus++;
				delstate.ndeltids++;
			}
		}
	}

	pfree(deadblocks);

	Assert(delstate.ndeltids >= ndeletable);

	/* 物理删除 LP_DEAD 元组（外加任何可安全删除的额外 TIDs） */
	_bt_delitems_delete_check(rel, buffer, heapRel, &delstate);

	pfree(delstate.deltids);
	pfree(delstate.status);
}

/*
 * _bt_deadblocks() -- 取得与 LP_DEAD 相关的表块。
 *
 * 根据行指针被标记为 LP_DEAD 的索引
 * 元组 TIDs，构造已排序且已去重的表块号数组。同时加入到来的 newitem 的表
 * 块，以防它不在与 LP_DEAD 相关的
 * 表块之中。
 *
 * 总是把 newitem 的表块算作与 LP_DEAD 相关的块是有道理的，
 * 因为代价始终很低；几乎可以肯定
 * 该表块在 tableam 中不会造成缓冲区未命中。另一方面，
 * 收益往往相当高。很有机会从该块上找到
 * 一些可删除的项，因为一般而言大多数垃圾
 * 元组是在不久前才变成垃圾的（很多情况下，这不会是
 * 核心代码最近在该表块中加入/修改的
 * 第一个逻辑行）。
 *
 * 返回最终数组，并为调用者把 *nblocks 设成它的最终大小。
 */
static BlockNumber *
_bt_deadblocks(Page page, OffsetNumber *deletable, int ndeletable,
			   IndexTuple newitem, int *nblocks)
{
	int			spacentids,
				ntids;
	BlockNumber *tidblocks;

	/*
	 * 把每个 TID 的块累积到一个数组中，其初始大小为每个设置了 LP_DEAD 的元组
	 * 留出一个表块的空间（外加 newitem 表
	 * 块的空间）。只有存在标记了 LP_DEAD 的
	 * 倒排列表元组时，数组才需要增长（这并不常见）。
	 */
	spacentids = ndeletable + 1;
	ntids = 0;
	tidblocks = (BlockNumber *) palloc(sizeof(BlockNumber) * spacentids);

	/*
	 * 首先加入到来的 newitem 的表块。这是简单删除会访问
	 * 一张并没有任何已知可删除项的表块的
	 * 唯一情形。
	 */
	Assert(!BTreeTupleIsPosting(newitem) && !BTreeTupleIsPivot(newitem));
	tidblocks[ntids++] = ItemPointerGetBlockNumber(&newitem->t_tid);

	for (int i = 0; i < ndeletable; i++)
	{
		ItemId		itemid = PageGetItemId(page, deletable[i]);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

		Assert(ItemIdIsDead(itemid));

		if (!BTreeTupleIsPosting(itup))
		{
			if (ntids + 1 > spacentids)
			{
				spacentids *= 2;
				tidblocks = (BlockNumber *)
					repalloc(tidblocks, sizeof(BlockNumber) * spacentids);
			}

			tidblocks[ntids++] = ItemPointerGetBlockNumber(&itup->t_tid);
		}
		else
		{
			int			nposting = BTreeTupleGetNPosting(itup);

			if (ntids + nposting > spacentids)
			{
				spacentids = Max(spacentids * 2, ntids + nposting);
				tidblocks = (BlockNumber *)
					repalloc(tidblocks, sizeof(BlockNumber) * spacentids);
			}

			for (int j = 0; j < nposting; j++)
			{
				ItemPointer tid = BTreeTupleGetPostingN(itup, j);

				tidblocks[ntids++] = ItemPointerGetBlockNumber(tid);
			}
		}
	}

	qsort(tidblocks, ntids, sizeof(BlockNumber), _bt_blk_cmp);
	*nblocks = qunique(tidblocks, ntids, sizeof(BlockNumber), _bt_blk_cmp);

	return tidblocks;
}

/*
 * _bt_blk_cmp() -- 供 _bt_simpledel_pass 使用的 qsort 比较函数
 */
static inline int
_bt_blk_cmp(const void *arg1, const void *arg2)
{
	BlockNumber b1 = *((BlockNumber *) arg1);
	BlockNumber b2 = *((BlockNumber *) arg2);

	if (b1 < b2)
		return -1;
	else if (b1 > b2)
		return 1;

	return 0;
}
