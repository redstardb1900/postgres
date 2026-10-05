/*-------------------------------------------------------------------------
 *
 * nbtree.c
 *	  Lehman 与 Yao 的 B-树管理算法在
 *	  Postgres 中的实现。
 *
 * NOTES
 *	  本文件只包含对外接口例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/relscan.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/condition_variable.h"
#include "storage/indexfsm.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/index_selfuncs.h"
#include "utils/memutils.h"


/*
 * BTPARALLEL_NOT_INITIALIZED 表示扫描尚未开始。
 *
 * BTPARALLEL_ADVANCING 表示某个进程正在把扫描推进到
 * 新的一页；其他进程必须等待。
 *
 * BTPARALLEL_IDLE 表示当前没有后端正在把扫描推进到
 * 新的一页；某个进程可以开始这么做。
 *
 * BTPARALLEL_DONE 表示扫描已完成（包括出错退出）。
 * 每一种不同的数组键组合都会到达这一状态一次。
 */
typedef enum
{
	BTPARALLEL_NOT_INITIALIZED,
	BTPARALLEL_ADVANCING,
	BTPARALLEL_IDLE,
	BTPARALLEL_DONE
} BTPS_State;

/*
 * BTParallelScanDescData 含有并行扫描所需的
 * B-树专用共享信息。
 */
typedef struct BTParallelScanDescData
{
	BlockNumber btps_scanPage;	/* 将要扫描的最新页或下一页 */
	BTPS_State	btps_pageStatus;	/* 表示下一页是否可供扫描。
									 * 并行扫描的可能状态
									 * 见上文。 */
	int			btps_arrayKeyCount; /* 表示并行扫描已处理的
									 * 数组扫描键数目 */
	slock_t		btps_mutex;		/* 保护上述变量 */
	ConditionVariable btps_cv;	/* 用于同步并行扫描 */
}			BTParallelScanDescData;

typedef struct BTParallelScanDescData *BTParallelScanDesc;


static void btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						 IndexBulkDeleteCallback callback, void *callback_state,
						 BTCycleId cycleid);
static void btvacuumpage(BTVacState *vstate, BlockNumber scanblkno);
static BTVacuumPosting btreevacuumposting(BTVacState *vstate,
										  IndexTuple posting,
										  OffsetNumber updatedoffset,
										  int *nremaining);


/*
 * B-树处理函数：返回带有访问方法参数
 * 与回调的 IndexAmRoutine。
 */
Datum
bthandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amoptsprocnum = BTOPTIONS_PROC;
	amroutine->amcanorder = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = true;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = true;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = false;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = true;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_COND_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = btbuild;
	amroutine->ambuildempty = btbuildempty;
	amroutine->aminsert = btinsert;
	amroutine->ambulkdelete = btbulkdelete;
	amroutine->amvacuumcleanup = btvacuumcleanup;
	amroutine->amcanreturn = btcanreturn;
	amroutine->amcostestimate = btcostestimate;
	amroutine->amoptions = btoptions;
	amroutine->amproperty = btproperty;
	amroutine->ambuildphasename = btbuildphasename;
	amroutine->amvalidate = btvalidate;
	amroutine->amadjustmembers = btadjustmembers;
	amroutine->ambeginscan = btbeginscan;
	amroutine->amrescan = btrescan;
	amroutine->amgettuple = btgettuple;
	amroutine->amgetbitmap = btgetbitmap;
	amroutine->amendscan = btendscan;
	amroutine->ammarkpos = btmarkpos;
	amroutine->amrestrpos = btrestrpos;
	amroutine->amestimateparallelscan = btestimateparallelscan;
	amroutine->aminitparallelscan = btinitparallelscan;
	amroutine->amparallelrescan = btparallelrescan;

	PG_RETURN_POINTER(amroutine);
}

/*
 *	btbuildempty() -- 在初始化分支中构建一个空的 B-树索引
 */
void
btbuildempty(Relation index)
{
	bool		allequalimage = _bt_allequalimage(index, false);
	Buffer		metabuf;
	Page		metapage;

	/*
	 * 初始化元页。
	 *
	 * 常规索引构建绕过缓冲区管理器，直接使用 smgr 函数，
	 * 并在结束时调用 smgrimmedsync()。索引很大时这是合理的，
	 * 但对空索引，最好使用缓冲区缓存，
	 * 以避免 smgrimmedsync()。
	 */
	metabuf = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	Assert(BufferGetBlockNumber(metabuf) == BTREE_METAPAGE);
	_bt_lockbuf(index, metabuf, BT_WRITE);

	START_CRIT_SECTION();

	metapage = BufferGetPage(metabuf);
	_bt_initmetapage(metapage, P_NONE, 0, allequalimage);
	MarkBufferDirty(metabuf);
	log_newpage_buffer(metabuf, true);

	END_CRIT_SECTION();

	_bt_unlockbuf(index, metabuf);
	ReleaseBuffer(metabuf);
}

/*
 *	btinsert() -- 把索引元组插入 B-树。
 *
 *		沿树递归下降，为新元组找到合适的位置，
 *		然后把它放在那里。
 */
bool
btinsert(Relation rel, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel,
		 IndexUniqueCheck checkUnique,
		 bool indexUnchanged,
		 IndexInfo *indexInfo)
{
	bool		result;
	IndexTuple	itup;

	/* 生成一个索引元组 */
	itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
	itup->t_tid = *ht_ctid;

	result = _bt_doinsert(rel, itup, checkUnique, indexUnchanged, heapRel);

	pfree(itup);

	return result;
}

/*
 *	btgettuple() -- 取得扫描中的下一个元组。
 */
bool
btgettuple(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		res;

	/* B-树索引从不会有损 */
	scan->xs_recheck = false;

	/*
	 * 若有任何数组键，在某次扫描的首次调用期间初始化它们。
	 * 无法在 btrescan 里做这件事，因为那时还不知道扫描
	 * 方向。
	 */
	if (so->numArrayKeys && !BTScanPosIsValid(so->currPos))
	{
		/* 若有任何无法满足的数组键，则放弃 */
		if (so->numArrayKeys < 0)
			return false;

		_bt_start_array_keys(scan, dir);
	}

	/* 此循环负责前进到下一组数组元素（若有） */
	do
	{
		/*
		 * 若这次扫描已经初始化，只需沿适当方向推进它。
		 * 若尚未初始化，则调用 _bt_first()
		 * 取得扫描中的第一项。
		 */
		if (!BTScanPosIsValid(so->currPos))
			res = _bt_first(scan, dir);
		else
		{
			/*
			 * 检查是否应杀死先前取出的元组。
			 */
			if (scan->kill_prior_tuple)
			{
				/*
				 * 是的，先记住，稍后处理。（离开索引页之前，
				 * 会一次性处理所有这类元组。）检查 numKilled
				 * 是否越界并不只是多虑：若调用者在索引扫描中
				 * 掉转方向，同一项可能被记入多次。不值得
				 * 为此做优化，所以我们不检测这种情况，
				 * 而是直接忘掉
				 * 超出的那些项。
				 */
				if (so->killedItems == NULL)
					so->killedItems = (int *)
						palloc(MaxTIDsPerBTreePage * sizeof(int));
				if (so->numKilled < MaxTIDsPerBTreePage)
					so->killedItems[so->numKilled++] = so->currPos.itemIndex;
			}

			/*
			 * 现在继续扫描。
			 */
			res = _bt_next(scan, dir);
		}

		/* 若已有元组，则返回它 ... */
		if (res)
			break;
		/* ... 否则看是否还有更多数组键要处理 */
	} while (so->numArrayKeys && _bt_advance_array_keys(scan, dir));

	return res;
}

/*
 * btgetbitmap() -- 取得全部匹配元组，并把它们加入位图
 */
int64
btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int64		ntids = 0;
	ItemPointer heapTid;

	/*
	 * 若有任何数组键，则初始化它们。
	 */
	if (so->numArrayKeys)
	{
		/* 若有任何无法满足的数组键，则放弃 */
		if (so->numArrayKeys < 0)
			return ntids;

		_bt_start_array_keys(scan, ForwardScanDirection);
	}

	/* 此循环负责前进到下一组数组元素（若有） */
	do
	{
		/* 取出第一页与元组 */
		if (_bt_first(scan, ForwardScanDirection))
		{
			/* 保存元组 ID，并继续扫描 */
			heapTid = &scan->xs_heaptid;
			tbm_add_tuples(tbm, heapTid, 1, false);
			ntids++;

			for (;;)
			{
				/*
				 * 前进到页内的下一个元组。这与 _bt_next() 中的
				 * 简单情形相同。
				 */
				if (++so->currPos.itemIndex > so->currPos.lastItem)
				{
					/* 让 _bt_next 承担繁重工作 */
					if (!_bt_next(scan, ForwardScanDirection))
						break;
				}

				/* 保存元组 ID，并继续扫描 */
				heapTid = &so->currPos.items[so->currPos.itemIndex].heapTid;
				tbm_add_tuples(tbm, heapTid, 1, false);
				ntids++;
			}
		}
		/* 现在看是否还有更多数组键要处理 */
	} while (so->numArrayKeys && _bt_advance_array_keys(scan, ForwardScanDirection));

	return ntids;
}

/*
 *	btbeginscan() -- 开始对 B-树索引的扫描
 */
IndexScanDesc
btbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	BTScanOpaque so;

	/* 不允许 order by 操作符 */
	Assert(norderbys == 0);

	/* 取得这次扫描 */
	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* 分配私有工作区 */
	so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
	BTScanPosInvalidate(so->currPos);
	BTScanPosInvalidate(so->markPos);
	if (scan->numberOfKeys > 0)
		so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
	else
		so->keyData = NULL;

	so->arrayKeyData = NULL;	/* 暂且假定没有数组键 */
	so->arraysStarted = false;
	so->numArrayKeys = 0;
	so->arrayKeys = NULL;
	so->arrayContext = NULL;

	so->killedItems = NULL;		/* 需要时再分配 */
	so->numKilled = 0;

	/*
	 * 还不知道这次扫描是否为仅索引扫描，因此要到
	 * btrescan 才分配元组工作区数组。不过无论是否需要，
	 * 我们都会设置 scan->xs_itupdesc，因为这样做开销很小。
	 */
	so->currTuples = so->markTuples = NULL;

	scan->xs_itupdesc = RelationGetDescr(rel);

	scan->opaque = so;

	return scan;
}

/*
 *	btrescan() -- 重新扫描索引关系
 */
void
btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* 我们并未持有任何读锁，但必须放下钉住 */
	if (BTScanPosIsValid(so->currPos))
	{
		/* 离开当前页之前，处理所有已杀死的项 */
		if (so->numKilled > 0)
			_bt_killitems(scan);
		BTScanPosUnpinIfPinned(so->currPos);
		BTScanPosInvalidate(so->currPos);
	}

	so->markItemIndex = -1;
	so->arrayKeyCount = 0;
	BTScanPosUnpinIfPinned(so->markPos);
	BTScanPosInvalidate(so->markPos);

	/*
	 * 若仅索引扫描需要，则分配元组工作区数组，并且
	 * 此前的重新扫描调用尚未做过。为节省 palloc
	 * 开销，两块工作区分配在同一个 palloc 块中；只有本
	 * 函数和 btendscan 知道这一点。
	 *
	 * 注意：这一数据结构也使得从 "name" 列返回数据是安全的，
	 * 尽管 btree name_ops 的底层存储数据类型是 cstring。
	 * 风险在于 "name" 应当填充到 NAMEDATALEN，但实际索引
	 * 元组多半更短。不过，由于我们只从位于 currTuples
	 * 数组中的元组返回数据，读取 NAMEDATALEN 字节最坏
	 * 也只会从 markTuples 数组里多取出一些数据——不可能
	 * 越过内存末尾而触发 SIGSEGV。这确实难看，但好过
	 * 在别处为 name_ops
	 * 增加特殊处理。
	 */
	if (scan->xs_want_itup && so->currTuples == NULL)
	{
		so->currTuples = (char *) palloc(BLCKSZ * 2);
		so->markTuples = so->currTuples + BLCKSZ;
	}

	/*
	 * 重置扫描键
	 */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	so->numberOfKeys = 0;		/* 直到 _bt_preprocess_keys 设置它 */

	/* 若有任何键是 SK_SEARCHARRAY 类型，则设置数组键信息 */
	_bt_preprocess_array_keys(scan);
}

/*
 *	btendscan() -- 结束一次扫描
 */
void
btendscan(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* 我们并未持有任何读锁，但必须放下钉住 */
	if (BTScanPosIsValid(so->currPos))
	{
		/* 离开当前页之前，处理所有已杀死的项 */
		if (so->numKilled > 0)
			_bt_killitems(scan);
		BTScanPosUnpinIfPinned(so->currPos);
	}

	so->markItemIndex = -1;
	BTScanPosUnpinIfPinned(so->markPos);

	/* 不必使位置失效，这块内存即将被释放。 */

	/* 释放存储 */
	if (so->keyData != NULL)
		pfree(so->keyData);
	/* so->arrayKeyData 和 so->arrayKeys 位于 arrayContext 中 */
	if (so->arrayContext != NULL)
		MemoryContextDelete(so->arrayContext);
	if (so->killedItems != NULL)
		pfree(so->killedItems);
	if (so->currTuples != NULL)
		pfree(so->currTuples);
	/* 不应 pfree so->markTuples，见 btrescan */
	pfree(so);
}

/*
 *	btmarkpos() -- 保存当前扫描位置
 */
void
btmarkpos(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* 可能有一个带钉住（但没有锁）的旧标记。 */
	BTScanPosUnpinIfPinned(so->markPos);

	/*
	 * 只记录当前的 itemIndex。若稍后在释放标记位置之前
	 * 走到下一页，_bt_steppage 会把 currPos 结构完整复制到
	 * markPos。若标记在我们离开该页之前就被移动了
	 * （这很常见），就不必做那项工作。
	 */
	if (BTScanPosIsValid(so->currPos))
		so->markItemIndex = so->currPos.itemIndex;
	else
	{
		BTScanPosInvalidate(so->markPos);
		so->markItemIndex = -1;
	}

	/* 同时记录所有数组键的当前位置 */
	if (so->numArrayKeys)
		_bt_mark_array_keys(scan);
}

/*
 *	btrestrpos() -- 把扫描恢复到上次保存的位置
 */
void
btrestrpos(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* 恢复所有数组键的标记位置 */
	if (so->numArrayKeys)
		_bt_restore_array_keys(scan);

	if (so->markItemIndex >= 0)
	{
		/*
		 * 自上次标记以来，扫描从未移到新页。只需
		 * 恢复 itemIndex。
		 *
		 * NB: 在这种情况下，不能指望 so->markPos 里的任何内容
		 * 是准确的。
		 */
		so->currPos.itemIndex = so->markItemIndex;
	}
	else
	{
		/*
		 * 上次标记或恢复之后，扫描移到了新页，现在
		 * 正恢复到标记页。我们并未持有任何读
		 * 锁，但若仍持有当前位置的钉住，
		 * 就必须放下它。
		 */
		if (BTScanPosIsValid(so->currPos))
		{
			/* 离开当前页之前，处理所有已杀死的项 */
			if (so->numKilled > 0)
				_bt_killitems(scan);
			BTScanPosUnpinIfPinned(so->currPos);
		}

		if (BTScanPosIsValid(so->markPos))
		{
			/* 把标记缓冲区的钉住计数加一，以便赋给当前缓冲区 */
			if (BTScanPosIsPinned(so->markPos))
				IncrBufferRefCount(so->markPos.buf);
			memcpy(&so->currPos, &so->markPos,
				   offsetof(BTScanPosData, items[1]) +
				   so->markPos.lastItem * sizeof(BTScanPosItem));
			if (so->currTuples)
				memcpy(so->currTuples, so->markTuples,
					   so->markPos.nextTupleOffset);
		}
		else
			BTScanPosInvalidate(so->currPos);
	}
}

/*
 * btestimateparallelscan -- 估计 BTParallelScanDescData 所需的存储
 */
Size
btestimateparallelscan(void)
{
	return sizeof(BTParallelScanDescData);
}

/*
 * btinitparallelscan -- 为并行 B-树扫描初始化 BTParallelScanDesc
 */
void
btinitparallelscan(void *target)
{
	BTParallelScanDesc bt_target = (BTParallelScanDesc) target;

	SpinLockInit(&bt_target->btps_mutex);
	bt_target->btps_scanPage = InvalidBlockNumber;
	bt_target->btps_pageStatus = BTPARALLEL_NOT_INITIALIZED;
	bt_target->btps_arrayKeyCount = 0;
	ConditionVariableInit(&bt_target->btps_cv);
}

/*
 *	btparallelrescan() -- 重置并行扫描
 */
void
btparallelrescan(IndexScanDesc scan)
{
	BTParallelScanDesc btscan;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;

	Assert(parallel_scan);

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	/*
	 * 理论上这里不必获取自旋锁，因为此时
	 * 不应当还有其他工作进程在运行，但为了
	 * 一致性我们仍然这么做。
	 */
	SpinLockAcquire(&btscan->btps_mutex);
	btscan->btps_scanPage = InvalidBlockNumber;
	btscan->btps_pageStatus = BTPARALLEL_NOT_INITIALIZED;
	btscan->btps_arrayKeyCount = 0;
	SpinLockRelease(&btscan->btps_mutex);
}

/*
 * _bt_parallel_seize() -- 开始把扫描推进到新
 *		页的过程。其他扫描必须等待，直到我们调用 _bt_parallel_release()
 *		或 _bt_parallel_done()。
 *
 * 返回值为真表示我们成功接管了扫描，为假
 * 表示没有接管。后一种情况发生在当前这组扫描键
 * 已经没有剩余页时。
 *
 * 若返回值为真，*pageno 返回扫描的下一页或当前页
 * （取决于扫描方向）。无效块号
 * 表示扫描尚未开始，P_NONE 表示已经到达末尾。
 * 某个参与进程第一次到达最后一页时，会返回
 * 真并把 *pageno 设为 P_NONE；此后再次尝试接管
 * 扫描将返回假。
 *
 * 若返回值为假，调用者应忽略 pageno 的值。
 */
bool
_bt_parallel_seize(IndexScanDesc scan, BlockNumber *pageno)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	BTPS_State	pageStatus;
	bool		exit_loop = false;
	bool		status = true;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;

	*pageno = P_NONE;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	while (1)
	{
		SpinLockAcquire(&btscan->btps_mutex);
		pageStatus = btscan->btps_pageStatus;

		if (so->arrayKeyCount < btscan->btps_arrayKeyCount)
		{
			/* 并行扫描已经前进到一组新的扫描键。 */
			status = false;
		}
		else if (pageStatus == BTPARALLEL_DONE)
		{
			/*
			 * 这组扫描键已经处理完。这可能是结束，
			 * 也可能还有更多组要尝试。
			 */
			status = false;
		}
		else if (pageStatus != BTPARALLEL_ADVANCING)
		{
			/*
			 * 我们已经成功接管扫描，以便
			 * 把它推进到新的一页！
			 */
			btscan->btps_pageStatus = BTPARALLEL_ADVANCING;
			*pageno = btscan->btps_scanPage;
			exit_loop = true;
		}
		SpinLockRelease(&btscan->btps_mutex);
		if (exit_loop || !status)
			break;
		ConditionVariableSleep(&btscan->btps_cv, WAIT_EVENT_BTREE_PAGE);
	}
	ConditionVariableCancelSleep();

	return status;
}

/*
 * _bt_parallel_release() -- 完成把扫描推进到
 *		新页的过程。现在已有新的 btps_scanPage 值；其他后端
 *		可以开始推进扫描。
 */
void
_bt_parallel_release(IndexScanDesc scan, BlockNumber scan_page)
{
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	SpinLockAcquire(&btscan->btps_mutex);
	btscan->btps_scanPage = scan_page;
	btscan->btps_pageStatus = BTPARALLEL_IDLE;
	SpinLockRelease(&btscan->btps_mutex);
	ConditionVariableSignal(&btscan->btps_cv);
}

/*
 * _bt_parallel_done() -- 把并行扫描标为完成。
 *
 * 当没有剩余页可扫描时，应调用本函数以
 * 通知其他工作进程。否则它们可能会永远等待扫描
 * 推进到下一页。
 */
void
_bt_parallel_done(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;
	bool		status_changed = false;

	/* 非并行扫描则什么也不做 */
	if (parallel_scan == NULL)
		return;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	/*
	 * 把并行扫描标为对这组扫描键已完成，
	 * 除非某个其他进程已经这么做了。另见
	 * _bt_advance_array_keys。
	 */
	SpinLockAcquire(&btscan->btps_mutex);
	if (so->arrayKeyCount >= btscan->btps_arrayKeyCount &&
		btscan->btps_pageStatus != BTPARALLEL_DONE)
	{
		btscan->btps_pageStatus = BTPARALLEL_DONE;
		status_changed = true;
	}
	SpinLockRelease(&btscan->btps_mutex);

	/* 唤醒与这次并行扫描关联的全部工作进程 */
	if (status_changed)
		ConditionVariableBroadcast(&btscan->btps_cv);
}

/*
 * _bt_parallel_advance_array_keys() -- 为数组
 *			键推进并行扫描。
 *
 * 更新本地扫描与并行扫描都已处理的
 * 数组键计数。
 */
void
_bt_parallel_advance_array_keys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	so->arrayKeyCount++;
	SpinLockAcquire(&btscan->btps_mutex);
	if (btscan->btps_pageStatus == BTPARALLEL_DONE)
	{
		btscan->btps_scanPage = InvalidBlockNumber;
		btscan->btps_pageStatus = BTPARALLEL_NOT_INITIALIZED;
		btscan->btps_arrayKeyCount++;
	}
	SpinLockRelease(&btscan->btps_mutex);
}

/*
 * 批量删除指向一组堆元组的全部索引项。
 * 目标元组集合由回调例程指定，它告知
 * 任一给定堆元组（由 ItemPointer 标识）是否正在被删除。
 *
 * 结果：一个由 palloc 分配的结构，含有供 VACUUM 显示的统计信息。
 */
IndexBulkDeleteResult *
btbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	BTCycleId	cycleid;

	/* 若是第一次则分配 stats，否则复用已有结构 */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* 确定这次扫描要使用的清理周期 ID */
	/* 这些 ENSURE 机制确保失败时清理共享内存 */
	PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	{
		cycleid = _bt_start_vacuum(rel);

		btvacuumscan(info, stats, callback, callback_state, cycleid);
	}
	PG_END_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	_bt_end_vacuum(rel);

	return stats;
}

/*
 * VACUUM 之后的清理。
 *
 * 结果：一个由 palloc 分配的结构，含有供 VACUUM 显示的统计信息。
 */
IndexBulkDeleteResult *
btvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	BlockNumber num_delpages;

	/* 在 ANALYZE ONLY 模式下为空操作 */
	if (info->analyze_only)
		return stats;

	/*
	 * 若已调用 btbulkdelete，就不必再做什么（我们只是维护
	 * _bt_vacuum_needs_cleanup() 所用的信息，做法是在下面调用
	 * _bt_set_cleanup_info()）。
	 *
	 * 若_not_调用 btbulkdelete，则必须做个选择：我们
	 * 必须决定现在是否需要调用 btvacuumscan()（也就是
	 * 进行中的 VACUUM 能否完全避免对索引做一次物理扫描）。
	 * 调用 _bt_vacuum_needs_cleanup() 现在就替我们
	 * 做出决定。
	 */
	if (stats == NULL)
	{
		/* 检查 VACUUM 操作能否完全避免调用 btvacuumscan() */
		if (!_bt_vacuum_needs_cleanup(info->index))
			return NULL;

		/*
		 * 既然实际上不会删除任何叶子项，这里就
		 * 不必走完清理周期 ID 那一整套手续。
		 *
		 * 倒排列表元组是仅清理扫描不准确的一个来源。
		 * btvacuumscan() 会假定每页的索引元组数
		 * 可以直接当作 num_index_tuples，尽管
		 * num_index_tuples 本应表示索引中的 TIDs 数。
		 * 这种简单做法可能严重低估索引中的
		 * 元组数。
		 *
		 * 我们的处理办法是：在仅清理的情况下，把 num_index_tuples
		 * 做成估计值。
		 */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		btvacuumscan(info, stats, NULL, NULL, 0);
		stats->estimated_count = true;
	}

	/*
	 * 为 _bt_vacuum_needs_cleanup() 在元页中维护 num_delpages。
	 *
	 * num_delpages 是索引中那些目前还不能安全放入 FSM
	 * 以供回收的已删除页数。只有当 _bt_pagedel() 在
	 * 我们调用 btvacuumscan() 期间确实删除了页时，num_delpages
	 * 才大于 0。即便如此，_bt_pendingfsm_finalize() 也必定
	 * 刚刚没能把任何新删除的页放入 FSM。
	 * （实际上存在边界情形：当前 VACUUM 新删除的页，
	 * 要到下一次 VACUUM 到来时回收仍不安全。
	 * 见 nbtree/README。）
	 */
	Assert(stats->pages_deleted >= stats->pages_free);
	num_delpages = stats->pages_deleted - stats->pages_free;
	_bt_set_cleanup_info(info->index, num_delpages);

	/*
	 * 并发页分裂很可能会骗过我们，使某些索引元组被
	 * 重复计数，因此不采信任何超过底层堆计数的总数……
	 * 前提是我们准确知道那个计数。否则这样做
	 * 可能只会让情况更糟。
	 */
	if (!info->estimated_count)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	return stats;
}

/*
 * btvacuumscan --- 为 VACUUMing 扫描索引
 *
 * 它同时负责：按清理回调查找可删除的叶子元组、
 * 查找可以删除的空页，以及查找可以回收的
 * 旧已删除页。btbulkdelete 和 btvacuumcleanup 都会调用它
 * （后者仅在没有发生 btbulkdelete 调用，且
 * _bt_vacuum_needs_cleanup 返回真时）。
 *
 * 调用者负责最初分配并清零 stats 结构，
 * 并在需要时取得清理周期 ID。
 */
static void
btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state,
			 BTCycleId cycleid)
{
	Relation	rel = info->index;
	BTVacState	vstate;
	BlockNumber num_pages;
	BlockNumber scanblkno;
	bool		needLock;

	/*
	 * 现在重置跟踪整个索引信息的那些字段。这样
	 * 可以避免一条 VACUUM 命令需要对索引做多次扫描时
	 * 的重复计数。
	 *
	 * 这里不要重置 tuples_removed 和 pages_newly_deleted 字段，
	 * 因为它们跟踪的是这条 VACUUM 命令的信息，因此必须在
	 * 每次调用 btvacuumscan() 之间保持。
	 *
	 * （注意 pages_free 被当作整个索引的状态，而不是
	 * 当前这次 VACUUM 的状态。这是合适的，因为 RecordFreeIndexPage()
	 * 调用是幂等的，在某些情形下会对同一批已删除页
	 * 重复调用。对我们来说，要点是跟踪 VACUUM 命令结束时
	 * 索引中可回收页的数量。）
	 */
	stats->num_pages = 0;
	stats->num_index_tuples = 0;
	stats->pages_deleted = 0;
	stats->pages_free = 0;

	/* 设置要传给 btvacuumpage 的信息 */
	vstate.info = info;
	vstate.stats = stats;
	vstate.callback = callback;
	vstate.callback_state = callback_state;
	vstate.cycleid = cycleid;

	/* 创建一个临时内存上下文，供运行 _bt_pagedel */
	vstate.pagedelcontext = AllocSetContextCreate(CurrentMemoryContext,
												  "_bt_pagedel",
												  ALLOCSET_DEFAULT_SIZES);

	/* 初始化 _bt_pendingfsm_finalize 使用的 vstate 字段 */
	vstate.bufsize = 0;
	vstate.maxbufsize = 0;
	vstate.pendingpages = NULL;
	vstate.npendingpages = 0;
	/* 考虑应用 _bt_pendingfsm_finalize 优化 */
	_bt_pendingfsm_init(rel, &vstate, (callback == NULL));

	/*
	 * 外层循环按物理顺序遍历除元页以外的
	 * 全部索引页（希望内核配合做预读以加快速度）。
	 * 必须访问所有叶子页，
	 * 包括扫描开始之后新增的页，否则可能无法删除
	 * 某些可删除元组。因此必须反复检查
	 * 关系长度。检查时必须获取关系扩展锁，
	 * 以避免竞态条件：若别人正在扩展该
	 * 关系，会有一个窗口，bufmgr/smgr 已经创建了新的
	 * 全零页，但 _bt_getbuf() 尚未对它加写锁。若我们
	 * 在这里扫到这样的页，就会错误地认为它可以
	 * 回收。持有该锁足以同步，从而避免问题：
	 * 要么 num_pages 不含新页，要么 _bt_getbuf
	 * 已经持有缓冲区上的写锁，并且在我们检查之前
	 * 该页会完全初始化。此外，若在我们查看之后
	 * 立刻有页被加入，也不必担心；页分裂代码
	 * 在加入右页之前已经持有左页上的写锁，因此
	 * 任何将要移入该页的元组，我们都已经处理过。
	 *
	 * XXX: 既然新页现在用 RBM_ZERO_AND_LOCK 锁定，
	 * 我认为扩展锁已经不再需要。
	 *
	 * 不过，对新建的或临时关系可以跳过加锁，
	 * 因为不会有别人访问它们。
	 */
	needLock = !RELATION_IS_LOCAL(rel);

	scanblkno = BTREE_METAPAGE + 1;
	for (;;)
	{
		/* 取得当前关系长度 */
		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(rel);
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		if (info->report_progress)
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
										 num_pages);

		/* 若已扫描完整关系则退出 */
		if (scanblkno >= num_pages)
			break;
		/* 遍历各页，然后回到循环重新检查长度 */
		for (; scanblkno < num_pages; scanblkno++)
		{
			btvacuumpage(&vstate, scanblkno);
			if (info->report_progress)
				pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
											 scanblkno);
		}
	}

	/* 把统计信息的 num_pages 字段设为索引的最终大小 */
	stats->num_pages = num_pages;

	MemoryContextDelete(vstate.pagedelcontext);

	/*
	 * 若扫描索引期间调用过 _bt_pagedel()，则看看
	 * 由此产生的页现在能否放入 FSM。若不安全，
	 * 就只好留给以后的 VACUUM 操作。
	 *
	 * 最后，若我们把任何页放入了 FSM（刚刚放入的，或扫描
	 * 期间放入的），就强制更新上层 FSM 页，以确保
	 * 搜索者能够找到它们。
	 */
	_bt_pendingfsm_finalize(rel, &vstate);
	if (stats->pages_free > 0)
		IndexFreeSpaceMapVacuum(rel);
}

/*
 * btvacuumpage --- 对一页做 VACUUM
 *
 * 它为 btvacuumscan() 处理单个页。某些情况下必须
 * 回溯，重新检查并 VACUUM 那些在先前某次调用中
 * 曾是 scanblkno 的页。我们用这种方式处理页分裂（发生在
 * 取得 cycleid 之后），其右半页恰好复用了我们
 * 在回收之前某个时刻可能已经处理过的块
 * （也就是页分裂之前）。
 */
static void
btvacuumpage(BTVacState *vstate, BlockNumber scanblkno)
{
	IndexVacuumInfo *info = vstate->info;
	IndexBulkDeleteResult *stats = vstate->stats;
	IndexBulkDeleteCallback callback = vstate->callback;
	void	   *callback_state = vstate->callback_state;
	Relation	rel = info->index;
	Relation	heaprel = info->heaprel;
	bool		attempt_pagedel;
	BlockNumber blkno,
				backtrack_to;
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;

	blkno = scanblkno;

backtrack:

	attempt_pagedel = false;
	backtrack_to = P_NONE;

	/* 在不持有任何缓冲区锁时调用 vacuum_delay_point */
	vacuum_delay_point();

	/*
	 * 这里不能用 _bt_getbuf()，因为它总会调用
	 * _bt_checkpage()，而后者会在全零页上报错。我们想
	 * 回收全零页，而不是失败。另外，我们想使用非默认的
	 * 缓冲区访问策略。
	 */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
							 info->strategy);
	_bt_lockbuf(rel, buf, BT_READ);
	page = BufferGetPage(buf);
	opaque = NULL;
	if (!PageIsNew(page))
	{
		_bt_checkpage(rel, buf);
		opaque = BTPageGetOpaque(page);
	}

	Assert(blkno <= scanblkno);
	if (blkno != scanblkno)
	{
		/*
		 * 正在回溯。
		 *
		 * 我们跟随右链接来到一张兄弟叶子页（该页
		 * 碰巧来自位于 scanblkno 之前的块）。我们唯一
		 * 想处理的情形是具有当前清理周期 ID 的
		 * 活叶子页。
		 *
		 * 该页的状态最好与我们的预期一致。顺便检查
		 * 那些暗示损坏的条件。它不可能是半死的，因为只有
		 * 被中断的 VACUUM 进程才会把页留在那种状态，所以
		 * 当该页还是 scanblkno 页时我们肯定已经处理过
		 * （半死页总会被 _bt_pagedel() 标成完全删除，
		 * 除非损坏）。
		 */
		if (!opaque || !P_ISLEAF(opaque) || P_ISHALFDEAD(opaque))
		{
			Assert(false);
			ereport(LOG,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("right sibling %u of scanblkno %u unexpectedly in an inconsistent state in index \"%s\"",
									 blkno, scanblkno, RelationGetRelationName(rel))));
			_bt_relbuf(rel, buf);
			return;
		}

		/*
		 * 我们可能在更早的调用中已经处理过该页，当时
		 * 该页就是 scanblkno。这发生在叶子页分裂出现于
		 * 扫描开始之后、右兄弟页成为 scanblkno
		 * 之前。
		 *
		 * 当前这次 btvacuumpage() 调用也可能已经删除了该页，
		 * 因为 _bt_pagedel() 有时会顺便删除 scanblkno 的
		 * 右兄弟页（它是在我们决定回溯到哪里之后
		 * 才这么做的）。现在不必把该页当作已删除页再处理
		 * 一次（事实上，在批量删除统计信息里把它再计为
		 * 已删除页是错误的）。
		 */
		if (opaque->btpo_cycleid != vstate->cycleid || P_ISDELETED(opaque))
		{
			/* 当前 scanblkno（以及所有更低的分裂页）已处理完 */
			_bt_relbuf(rel, buf);
			return;
		}
	}

	if (!opaque || BTPageIsRecyclable(page, heaprel))
	{
		/* 可以回收此页（可以是叶子页或内部页） */
		RecordFreeIndexPage(rel, blkno);
		stats->pages_deleted++;
		stats->pages_free++;
	}
	else if (P_ISDELETED(opaque))
	{
		/*
		 * 已经删除的页（可以是叶子页或内部页）。还不能
		 * 回收。
		 */
		stats->pages_deleted++;
	}
	else if (P_ISHALFDEAD(opaque))
	{
		/* 半死叶子页（来自被中断的 VACUUM）——完成删除 */
		attempt_pagedel = true;

		/*
		 * _bt_pagedel() 在所有情况下都会增加 pages_newly_deleted
		 * 和 pages_deleted 统计值（除非损坏）
		 */
	}
	else if (P_ISLEAF(opaque))
	{
		OffsetNumber deletable[MaxIndexTuplesPerPage];
		int			ndeletable;
		BTVacuumPosting updatable[MaxIndexTuplesPerPage];
		int			nupdatable;
		OffsetNumber offnum,
					minoff,
					maxoff;
		int			nhtidsdead,
					nhtidslive;

		/*
		 * 把最初的读锁换成此页上的完全清理锁。
		 * 在清理扫描过程中，必须对每一张叶子页取得这样的锁，
		 * 无论它是否真的含有任何
		 * 可删除元组——见 nbtree/README。
		 */
		_bt_upgradelockbufcleanup(rel, buf);

		/*
		 * 检查是否需要回溯到更早的页。我们关心的是
		 * 自开始清理扫描以来发生的页分裂。若分裂把
		 * 右半边上的元组（也就是排序靠后的元组）移到了
		 * 我们已经经过的块，就可能漏掉这些元组。现在
		 * 需要回溯。（必须在下面可能清除 btpo_cycleid
		 * 或删除 scanblkno 页之前
		 * 做这件事！）
		 */
		if (vstate->cycleid != 0 &&
			opaque->btpo_cycleid == vstate->cycleid &&
			!(opaque->btpo_flags & BTP_SPLIT_END) &&
			!P_RIGHTMOST(opaque) &&
			opaque->btpo_next < scanblkno)
			backtrack_to = opaque->btpo_next;

		ndeletable = 0;
		nupdatable = 0;
		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		nhtidsdead = 0;
		nhtidslive = 0;
		if (callback)
		{
			/* btbulkdelete 回调告知要删除（或更新）什么 */
			for (offnum = minoff;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				IndexTuple	itup;

				itup = (IndexTuple) PageGetItem(page,
												PageGetItemId(page, offnum));

				Assert(!BTreeTupleIsPivot(itup));
				if (!BTreeTupleIsPosting(itup))
				{
					/* 普通元组，标准的表 TID 表示 */
					if (callback(&itup->t_tid, callback_state))
					{
						deletable[ndeletable++] = offnum;
						nhtidsdead++;
					}
					else
						nhtidslive++;
				}
				else
				{
					BTVacuumPosting vacposting;
					int			nremaining;

					/* 倒排列表元组 */
					vacposting = btreevacuumposting(vstate, itup, offnum,
													&nremaining);
					if (vacposting == NULL)
					{
						/*
						 * 该倒排元组中的全部表 TIDs 都还在，因此不需要
						 * 删除或更新
						 */
						Assert(nremaining == BTreeTupleGetNPosting(itup));
					}
					else if (nremaining > 0)
					{

						/*
						 * 把倒排列表元组的元数据存入
						 * 整页的 updatable 数组。现有元组
						 * 将在稍后调用 _bt_delitems_vacuum() 时
						 * 被更新。
						 */
						Assert(nremaining < BTreeTupleGetNPosting(itup));
						updatable[nupdatable++] = vacposting;
						nhtidsdead += BTreeTupleGetNPosting(itup) - nremaining;
					}
					else
					{
						/*
						 * 倒排列表中的全部表 TIDs 都必须
						 * 删除。我们将完全删除该索引元组
						 * （不需要更新）。
						 */
						Assert(nremaining == 0);
						deletable[ndeletable++] = offnum;
						nhtidsdead += BTreeTupleGetNPosting(itup);
						pfree(vacposting);
					}

					nhtidslive += nremaining;
				}
			}
		}

		/*
		 * 应用所需的删除或更新。每页只发出一次
		 * _bt_delitems_vacuum() 调用，以便尽量减少 WAL 流量。
		 */
		if (ndeletable > 0 || nupdatable > 0)
		{
			Assert(nhtidsdead >= ndeletable + nupdatable);
			_bt_delitems_vacuum(rel, buf, deletable, ndeletable, updatable,
								nupdatable);

			stats->tuples_removed += nhtidsdead;
			/* 必须重新计算 maxoff */
			maxoff = PageGetMaxOffsetNumber(page);

			/* 这里不能泄漏内存 */
			for (int i = 0; i < nupdatable; i++)
				pfree(updatable[i]);
		}
		else
		{
			/*
			 * 若叶子页在本次清理周期中被分裂过，即使
			 * 没有要做的删除，也值得花一次写入来清除 btpo_cycleid。
			 * （若有删除，_bt_delitems_vacuum
			 * 会负责这件事。）这保证我们不会再次处理
			 * 该页。
			 *
			 * 我们把它当作提示位更新，因为没有必要
			 * 把它记入 WAL。
			 */
			Assert(nhtidsdead == 0);
			if (vstate->cycleid != 0 &&
				opaque->btpo_cycleid == vstate->cycleid)
			{
				opaque->btpo_cycleid = 0;
				MarkBufferDirtyHint(buf, true);
			}
		}

		/*
		 * 若叶子页现在为空，就尝试删除它；否则统计
		 * 活元组（倒排列表中的活表 TIDs 按单独的活元组
		 * 计数）。不过回溯时不删除，
		 * 因为那需要让 _bt_pagedel() 了解回溯
		 * （为处理这种情况再增加复杂度似乎不值得）。
		 *
		 * 在对 btvacuumscan 的仅清理调用期间（即未设置 callback 时），
		 * 我们不统计活 TIDs 的数量。我们改为直接统计
		 * 索引元组数。这样避免逐一检查每页上的全部元组，
		 * 开销更小。VACUUM 在仅清理的情况下会把
		 * num_index_tuples 当作估计值，因此某些情况下
		 * 这会显著低估 num_index_tuples
		 * 也没有关系。
		 */
		if (minoff > maxoff)
			attempt_pagedel = (blkno == scanblkno);
		else if (callback)
			stats->num_index_tuples += nhtidslive;
		else
			stats->num_index_tuples += maxoff - minoff + 1;

		Assert(!attempt_pagedel || nhtidslive == 0);
	}

	if (attempt_pagedel)
	{
		MemoryContext oldcontext;

		/* 在临时上下文中运行 pagedel，以避免内存泄漏 */
		MemoryContextReset(vstate->pagedelcontext);
		oldcontext = MemoryContextSwitchTo(vstate->pagedelcontext);

		/*
		 * _bt_pagedel 替我们维护批量删除统计信息；
		 * 调用期间 pages_newly_deleted 和 pages_deleted
		 * 很可能会增加
		 */
		Assert(blkno == scanblkno);
		_bt_pagedel(rel, buf, vstate);

		MemoryContextSwitchTo(oldcontext);
		/* pagedel 已释放缓冲区，所以我们不应再释放 */
	}
	else
		_bt_relbuf(rel, buf);

	if (backtrack_to != P_NONE)
	{
		blkno = backtrack_to;
		goto backtrack;
	}
}

/*
 * btreevacuumposting --- 确定倒排列表中仍然需要的 TIDs
 *
 * 返回描述如何构造替换元组的元数据，其中不含 VACUUM
 * 需要删除的那些 TIDs。常见情况下不需要改动调用者的
 * 倒排列表元组，此时返回值为 NULL（作为优化，我们避免
 * 在这里分配内存）。
 *
 * 倒排列表元组中应保留的 TIDs 数目通过
 * *nremaining 交给调用者。
 */
static BTVacuumPosting
btreevacuumposting(BTVacState *vstate, IndexTuple posting,
				   OffsetNumber updatedoffset, int *nremaining)
{
	int			live = 0;
	int			nitem = BTreeTupleGetNPosting(posting);
	ItemPointer items = BTreeTupleGetPosting(posting);
	BTVacuumPosting vacposting = NULL;

	for (int i = 0; i < nitem; i++)
	{
		if (!vstate->callback(items + i, vstate->callback_state))
		{
			/* 活的表 TID */
			live++;
		}
		else if (vacposting == NULL)
		{
			/*
			 * 遇到的第一个死表 TID。
			 *
			 * 现在已经清楚，需要删除一个或多个死表
			 * TIDs，因此开始维护描述如何更新
			 * 现有倒排列表元组的元数据。
			 */
			vacposting = palloc(offsetof(BTVacuumPostingData, deletetids) +
								nitem * sizeof(uint16));

			vacposting->itup = posting;
			vacposting->updatedoffset = updatedoffset;
			vacposting->ndeletedtids = 0;
			vacposting->deletetids[vacposting->ndeletedtids++] = i;
		}
		else
		{
			/* 第二个或后续的死表 TID */
			vacposting->deletetids[vacposting->ndeletedtids++] = i;
		}
	}

	*nremaining = live;
	return vacposting;
}

/*
 *	btcanreturn() -- 检查 B-树索引是否支持仅索引扫描。
 *
 * B-树总是支持，因此这里很简单。
 */
bool
btcanreturn(Relation index, int attno)
{
	return true;
}
