/*----------------------------------------------------------------------
 *
 * tableam.c
 *		太大、不宜做成内联函数的表访问方法例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/table/tableam.c
 *
 * 说明（NOTES）
 *	  注意，这里的大多数函数写在 tableam.h 里，而不是
 *	  这里。因为 tableam.h 中有大量内联函数，若不断
 *	  在文件之间来回切换，会更难理解。
 *
 *----------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/syncscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "optimizer/plancat.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/smgr.h"

/*
 * 控制并行顺序扫描期间，向并行工作进程分配块
 * 这一行为的常量。严格来说这些值不必是
 * 2 的幂，但取 2 的幂会让计算更合适，
 * 也会让递减步进更均匀。
 */

/* 试图把一次并行顺序扫描拆成的 I/O 块组数 */
#define PARALLEL_SEQSCAN_NCHUNKS			2048
/* 只剩这么多个块组时，缩小分配规模 */
#define PARALLEL_SEQSCAN_RAMPDOWN_CHUNKS	64
/* 把并行 I/O 块组的大小限制为这么多个块 */
#define PARALLEL_SEQSCAN_MAX_CHUNK_SIZE		8192

/* GUC 变量 */
char	   *default_table_access_method = DEFAULT_TABLE_ACCESS_METHOD;
bool		synchronize_seqscans = true;


/* ----------------------------------------------------------------------------
 * 槽函数。
 * ----------------------------------------------------------------------------
 */

const TupleTableSlotOps *
table_slot_callbacks(Relation relation)
{
	const TupleTableSlotOps *tts_cb;

	if (relation->rd_tableam)
		tts_cb = relation->rd_tableam->slot_callbacks(relation);
	else if (relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * 历史上 FDWs 期望在槽里存放堆元组。继续
		 * 交给它们一个，以便把 FDWs 适配到新
		 * 版本时少一些痛苦。堆槽相对虚拟槽的代价相当
		 * 小。
		 */
		tts_cb = &TTSOpsHeapTuple;
	}
	else
	{
		/*
		 * 这些也需要支持，因为代码的某些部分（例如 COPY）
		 * 也要为这类关系创建槽。看起来更好的做法是
		 * 把这种情况下该用堆槽这一知识
		 * 集中在这里。
		 */
		Assert(relation->rd_rel->relkind == RELKIND_VIEW ||
			   relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
		tts_cb = &TTSOpsVirtual;
	}

	return tts_cb;
}

TupleTableSlot *
table_slot_create(Relation relation, List **reglist)
{
	const TupleTableSlotOps *tts_cb;
	TupleTableSlot *slot;

	tts_cb = table_slot_callbacks(relation);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(relation), tts_cb);

	if (reglist)
		*reglist = lappend(*reglist, slot);

	return slot;
}


/* ----------------------------------------------------------------------------
 * 表扫描函数。
 * ----------------------------------------------------------------------------
 */

TableScanDesc
table_beginscan_catalog(Relation relation, int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_SEQSCAN |
		SO_ALLOW_STRAT | SO_ALLOW_SYNC | SO_ALLOW_PAGEMODE | SO_TEMP_SNAPSHOT;
	Oid			relid = RelationGetRelid(relation);
	Snapshot	snapshot = RegisterSnapshot(GetCatalogSnapshot(relid));

	return relation->rd_tableam->scan_begin(relation, snapshot, nkeys, key,
											NULL, flags);
}

void
table_scan_update_snapshot(TableScanDesc scan, Snapshot snapshot)
{
	Assert(IsMVCCSnapshot(snapshot));

	RegisterSnapshot(snapshot);
	scan->rs_snapshot = snapshot;
	scan->rs_flags |= SO_TEMP_SNAPSHOT;
}


/* ----------------------------------------------------------------------------
 * 并行表扫描相关函数。
 * ----------------------------------------------------------------------------
 */

Size
table_parallelscan_estimate(Relation rel, Snapshot snapshot)
{
	Size		sz = 0;

	if (IsMVCCSnapshot(snapshot))
		sz = add_size(sz, EstimateSnapshotSpace(snapshot));
	else
		Assert(snapshot == SnapshotAny);

	sz = add_size(sz, rel->rd_tableam->parallelscan_estimate(rel));

	return sz;
}

void
table_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan,
							  Snapshot snapshot)
{
	Size		snapshot_off = rel->rd_tableam->parallelscan_initialize(rel, pscan);

	pscan->phs_snapshot_off = snapshot_off;

	if (IsMVCCSnapshot(snapshot))
	{
		SerializeSnapshot(snapshot, (char *) pscan + pscan->phs_snapshot_off);
		pscan->phs_snapshot_any = false;
	}
	else
	{
		Assert(snapshot == SnapshotAny);
		pscan->phs_snapshot_any = true;
	}
}

TableScanDesc
table_beginscan_parallel(Relation relation, ParallelTableScanDesc pscan)
{
	Snapshot	snapshot;
	uint32		flags = SO_TYPE_SEQSCAN |
		SO_ALLOW_STRAT | SO_ALLOW_SYNC | SO_ALLOW_PAGEMODE;

	Assert(RelationGetRelid(relation) == pscan->phs_relid);

	if (!pscan->phs_snapshot_any)
	{
		/* 快照已序列化——把它恢复出来 */
		snapshot = RestoreSnapshot((char *) pscan + pscan->phs_snapshot_off);
		RegisterSnapshot(snapshot);
		flags |= SO_TEMP_SNAPSHOT;
	}
	else
	{
		/* 调用方传入的 SnapshotAny（未序列化） */
		snapshot = SnapshotAny;
	}

	return relation->rd_tableam->scan_begin(relation, snapshot, 0, NULL,
											pscan, flags);
}


/* ----------------------------------------------------------------------------
 * 索引扫描相关函数。
 * ----------------------------------------------------------------------------
 */

/*
 * 做这项检查时，只需开始一次索引扫描，创建必要的
 * 槽，做堆查找，然后再全部关掉。这还可以
 * 再做优化，但从性能角度看不太要紧。若经常存在
 * 同样匹配唯一索引键的存活索引指针，本例程的
 * CPU 开销不太可能要紧。
 *
 * 注意：若表 AM 支持经单个索引项到达多个行版本，
 * 则返回 true 时 *tid 可能被修改（例如
 * 堆的 HOT）。
 */
bool
table_index_fetch_tuple_check(Relation rel,
							  ItemPointer tid,
							  Snapshot snapshot,
							  bool *all_dead)
{
	IndexFetchTableData *scan;
	TupleTableSlot *slot;
	bool		call_again = false;
	bool		found;

	slot = table_slot_create(rel, NULL);
	scan = table_index_fetch_begin(rel);
	found = table_index_fetch_tuple(scan, tid, snapshot, slot, &call_again,
									all_dead);
	table_index_fetch_end(scan);
	ExecDropSingleTupleTableSlot(slot);

	return found;
}


/* ------------------------------------------------------------------------
 * 对单个元组做非修改操作的函数
 * ------------------------------------------------------------------------
 */

void
table_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	Relation	rel = scan->rs_rd;
	const TableAmRoutine *tableam = rel->rd_tableam;

	/*
	 * 对于系统目录或普通表，不应当直接带着有效的
	 * CheckXidAlive 调用 table_tuple_get_latest_tid。详见
	 * 声明这些变量处的 xact.c 注释。
	 */
	if (unlikely(TransactionIdIsValid(CheckXidAlive) && !bsysscan))
		elog(ERROR, "unexpected table_tuple_get_latest_tid call during logical decoding");

	/*
	 * 因为调用时可能带上用户提供的 TID，不要过于
	 * 信任输入。
	 */
	if (!tableam->tuple_tid_valid(scan, tid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tid (%u, %u) is not valid for relation \"%s\"",
						ItemPointerGetBlockNumberNoCheck(tid),
						ItemPointerGetOffsetNumberNoCheck(tid),
						RelationGetRelationName(rel))));

	tableam->tuple_get_latest_tid(scan, tid);
}


/* ----------------------------------------------------------------------------
 * 让修改操作稍简单一些的函数。
 * ----------------------------------------------------------------------------
 */

/*
 * simple_table_tuple_insert - 插入一个元组
 *
 * 目前，本例程与 table_tuple_insert 的差别只是提供一个
 * 默认命令 ID，并且不允许使用加速选项。
 */
void
simple_table_tuple_insert(Relation rel, TupleTableSlot *slot)
{
	table_tuple_insert(rel, slot, GetCurrentCommandId(true), 0, NULL);
}

/*
 * simple_table_tuple_delete - 删除一个元组
 *
 * 当预计不会并发更新目标元组时，可用本例程删除元组
 * （例如因为我们持有与该元组相关联的
 * 关系上的锁）。任何失败都经由
 * ereport() 报告。
 */
void
simple_table_tuple_delete(Relation rel, ItemPointer tid, Snapshot snapshot)
{
	TM_Result	result;
	TM_FailureData tmfd;

	result = table_tuple_delete(rel, tid,
								GetCurrentCommandId(true),
								snapshot, InvalidSnapshot,
								true /* 等待提交 */ ,
								&tmfd, false /* changingPart */ );

	switch (result)
	{
		case TM_SelfModified:
			/* 元组已在当前命令中被更新？ */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* 已成功完成 */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized table_tuple_delete status: %u", result);
			break;
	}
}

/*
 * simple_table_tuple_update - 替换一个元组
 *
 * 当预计不会并发更新目标元组时，可用本例程更新元组
 * （例如因为我们持有与该元组相关联的
 * 关系上的锁）。任何失败都经由
 * ereport() 报告。
 */
void
simple_table_tuple_update(Relation rel, ItemPointer otid,
						  TupleTableSlot *slot,
						  Snapshot snapshot,
						  TU_UpdateIndexes *update_indexes)
{
	TM_Result	result;
	TM_FailureData tmfd;
	LockTupleMode lockmode;

	result = table_tuple_update(rel, otid, slot,
								GetCurrentCommandId(true),
								snapshot, InvalidSnapshot,
								true /* 等待提交 */ ,
								&tmfd, &lockmode, update_indexes);

	switch (result)
	{
		case TM_SelfModified:
			/* 元组已在当前命令中被更新？ */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* 已成功完成 */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized table_tuple_update status: %u", result);
			break;
	}
}


/* ----------------------------------------------------------------------------
 * 为面向块的表 AM 实现并行扫描的辅助函数。
 * ----------------------------------------------------------------------------
 */

Size
table_block_parallelscan_estimate(Relation rel)
{
	return sizeof(ParallelBlockTableScanDescData);
}

Size
table_block_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc) pscan;

	bpscan->base.phs_relid = RelationGetRelid(rel);
	bpscan->phs_nblocks = RelationGetNumberOfBlocks(rel);
	/* 将 phs_syncscan 的初始化与 initscan 中的类似逻辑对照 */
	bpscan->base.phs_syncscan = synchronize_seqscans &&
		!RelationUsesLocalBuffers(rel) &&
		bpscan->phs_nblocks > NBuffers / 4;
	SpinLockInit(&bpscan->phs_mutex);
	bpscan->phs_startblock = InvalidBlockNumber;
	pg_atomic_init_u64(&bpscan->phs_nallocated, 0);

	return sizeof(ParallelBlockTableScanDescData);
}

void
table_block_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc) pscan;

	pg_atomic_write_u64(&bpscan->phs_nallocated, 0);
}

/*
 * 找出并设置扫描的 startblock
 *
 * 确定并行顺序扫描应从何处开始。本函数可能被
 * 调用很多次，每个并行工作进程一次。必须小心只
 * 把 startblock 设置一次。
 */
void
table_block_parallelscan_startblock_init(Relation rel,
										 ParallelBlockTableScanWorker pbscanwork,
										 ParallelBlockTableScanDesc pbscan)
{
	BlockNumber sync_startpage = InvalidBlockNumber;

	/* 重置用于控制分配大小的状态。 */
	memset(pbscanwork, 0, sizeof(*pbscanwork));

	StaticAssertStmt(MaxBlockNumber <= 0xFFFFFFFE,
					 "pg_nextpower2_32 may be too small for non-standard BlockNumber width");

	/*
	 * 按关系大小确定块组大小。首先把
	 * 关系拆成 PARALLEL_SEQSCAN_NCHUNKS 个块组，然后
	 * 把块组大小向上取到 2 的幂。这意味着
	 * 关系被拆成介于 PARALLEL_SEQSCAN_NCHUNKS
	 * 与 PARALLEL_SEQSCAN_NCHUNKS / 2 个块组之间。
	 */
	pbscanwork->phsw_chunk_size = pg_nextpower2_32(Max(pbscan->phs_nblocks /
													   PARALLEL_SEQSCAN_NCHUNKS, 1));

	/*
	 * 确保较大的表不超过最大块组大小。这
	 * 意味着较大的表得到的块组数可能远多于 PARALLEL_SEQSCAN_NCHUNKS。
	 * 块组过大已被证明有损
	 * 同步扫描的性能。
	 */
	pbscanwork->phsw_chunk_size = Min(pbscanwork->phsw_chunk_size,
									  PARALLEL_SEQSCAN_MAX_CHUNK_SIZE);

retry:
	/* 获取自旋锁。 */
	SpinLockAcquire(&pbscan->phs_mutex);

	/*
	 * 若扫描的 startblock 尚未初始化，现在就必须
	 * 完成初始化。若这不是同步扫描，就从块 0 开始；但
	 * 若是同步扫描，就必须从
	 * 同步扫描机制取得起始位置。不过做这件事时无法一直持有自旋锁，
	 * 因此先释放自旋锁，取得我们需要的
	 * 信息，然后重试。若在此期间没有别人初始化过扫描，
	 * 第二次进入时就会填上我们取到的值，
	 * 完成设置。
	 */
	if (pbscan->phs_startblock == InvalidBlockNumber)
	{
		if (!pbscan->base.phs_syncscan)
			pbscan->phs_startblock = 0;
		else if (sync_startpage != InvalidBlockNumber)
			pbscan->phs_startblock = sync_startpage;
		else
		{
			SpinLockRelease(&pbscan->phs_mutex);
			sync_startpage = ss_get_location(rel, pbscan->phs_nblocks);
			goto retry;
		}
	}
	SpinLockRelease(&pbscan->phs_mutex);
}

/*
 * 取得下一个要扫描的页面
 *
 * 取得下一个要扫描的页面。即使已经没有页面可扫，
 * 另一个后端也可能已取走一页去扫描，尚未看完，
 * 因此不能因为第一个后端收到
 * InvalidBlockNumber 就认为扫描已经完成。
 */
BlockNumber
table_block_parallelscan_nextpage(Relation rel,
								  ParallelBlockTableScanWorker pbscanwork,
								  ParallelBlockTableScanDesc pbscan)
{
	BlockNumber page;
	uint64		nallocated;

	/*
	 * 下面的逻辑把块号分配给并行工作进程，使得
	 * 每个工作进程都会拿到一组连续的块号去
	 * 扫描。较早的版本会把下一个最高的块
	 * 号交给下一个调用本函数的工作进程。这通常会
	 * 让工作进程永远拿不到连续的块号。有些
	 * 操作系统检测不到顺序 I/O 模式，因为
	 * 每个后端是不同的进程，从而可能因预读低效或没有预读而
	 * 性能很差。为绕开这个问题，
	 * 现在为每个工作进程分配一段块号，并且
	 * 当它们回来再要一块时，就给出该范围内的下一个，
	 * 直到这段范围用完。当工作进程用完这段
	 * 块范围后，再为它分配下一段，并返回
	 * 该范围里的第一个块号。
	 *
	 * 这里把这些块的范围命名为 "chunks"。这些块组的
	 * 初始大小在 table_block_parallelscan_startblock_init 中
	 * 按关系大小确定。扫描临近结束时，我们
	 * 开始缩小块组，试图
	 * 把剩余工作尽可能均匀地分给所有
	 * 工作进程。
	 *
	 * 这里 pbscanwork 是工作进程的本地内存。phsw_chunk_remaining 跟踪
	 * 块组中剩余的块数。当它降到 0 时，
	 * 就必须为该工作进程分配一个新块组。
	 *
	 * phs_nallocated 跟踪已经分配给工作进程的块数。
	 * 当 phs_nallocated >= rs_nblocks 时，所有块都已经
	 * 分配完毕。
	 *
	 * 因为用原子 fetch-and-add 取得当前值，
	 * phs_nallocated 计数会超过 rs_nblocks，因为当工作进程
	 * 试图分配下一块、但所有块都已分配完时，
	 * 仍会递增该值。因此该计数器必须有 64 位
	 * 宽，以免 rs_nblocks 接近
	 * 2^32 时发生回绕。
	 *
	 * 实际要返回的块，是把该计数器加上
	 * 起始块号，再对 nblocks 取模。
	 */

	/*
	 * 先检查这个工作进程在前一个块组里
	 * 是否还有剩余块。必须先用完那些块，然后才能
	 * 给该工作进程分配新块组。
	 */
	if (pbscanwork->phsw_chunk_remaining > 0)
	{
		/*
		 * 给出范围内的下一块，并更新剩余
		 * 块数。
		 */
		nallocated = ++pbscanwork->phsw_nallocated;
		pbscanwork->phsw_chunk_remaining--;
	}
	else
	{
		/*
		 * 当扫描中只剩 PARALLEL_SEQSCAN_RAMPDOWN_CHUNKS 个块组
		 * 时，把块组大小减半。因为在这里缩小了
		 * 块组大小，按新大小再做
		 * PARALLEL_SEQSCAN_RAMPDOWN_CHUNKS 个之后会再次走到这里。如此几次
		 * 之后，最后几个块会在
		 * 块组大小为 1 时处理。
		 */
		if (pbscanwork->phsw_chunk_size > 1 &&
			pbscanwork->phsw_nallocated > pbscan->phs_nblocks -
			(pbscanwork->phsw_chunk_size * PARALLEL_SEQSCAN_RAMPDOWN_CHUNKS))
			pbscanwork->phsw_chunk_size >>= 1;

		nallocated = pbscanwork->phsw_nallocated =
			pg_atomic_fetch_add_u64(&pbscan->phs_nallocated,
									pbscanwork->phsw_chunk_size);

		/*
		 * 设置本块组的剩余块数，使该工作进程随后的
		 * 调用继续沿用这个块组，直到用完。
		 */
		pbscanwork->phsw_chunk_remaining = pbscanwork->phsw_chunk_size - 1;
	}

	if (nallocated >= pbscan->phs_nblocks)
		page = InvalidBlockNumber;	/* 所有块都已分配 */
	else
		page = (nallocated + pbscan->phs_startblock) % pbscan->phs_nblocks;

	/*
	 * 报告扫描位置。通常报告当前页号。
	 * 不过到达扫描末尾时，报告的是起始页，
	 * 而不是结束页，以免以后扫描的起始位置
	 * 往回偏。不过扫描结束时的位置只报告
	 * 一次：之后的调用者什么都不报告。
	 */
	if (pbscan->base.phs_syncscan)
	{
		if (page != InvalidBlockNumber)
			ss_report_location(rel, page);
		else if (nallocated == pbscan->phs_nblocks)
			ss_report_location(rel, pbscan->phs_startblock);
	}

	return page;
}

/* ----------------------------------------------------------------------------
 * 为面向块的表 AM 实现关系大小计算的辅助函数。
 * ----------------------------------------------------------------------------
 */

/*
 * table_block_relation_size
 *
 * 若表 AM 把各个关系分支当作存放数据的唯一地方，
 * 并且按预期方式使用它们（例如实际数据
 * 在主分支里，而不是别的分支），就可以用这个
 * relation_size 回调的实现，而不必自己实现。
 */
uint64
table_block_relation_size(Relation rel, ForkNumber forkNumber)
{
	uint64		nblocks = 0;

	/* InvalidForkNumber 表示返回所有分支的大小 */
	if (forkNumber == InvalidForkNumber)
	{
		for (int i = 0; i < MAX_FORKNUM; i++)
			nblocks += smgrnblocks(RelationGetSmgr(rel), i);
	}
	else
		nblocks = smgrnblocks(RelationGetSmgr(rel), forkNumber);

	return nblocks * BLCKSZ;
}

/*
 * table_block_relation_estimate_size
 *
 * 本函数无法直接用作
 * relation_estimate_size 回调的实现，因为它还有几个额外参数。
 * 它本意是用作辅助函数；调用方可以
 * 把传给自己 relation_estimate_size 的参数再带上
 * 这里需要的附加值传进来。
 *
 * overhead_bytes_per_tuple 应包含大约多少字节，
 * 用于存放元组数据本身之外、元组还要
 * 多占的存储。通常包括
 * 元组头和项指针的大小。这只用于查询
 * 规划，因此该值并不恒定的表 AM 可以选择传入
 * 一个 "最佳猜测"。
 *
 * usable_bytes_per_page 应包含每页大约多少字节
 * 可用于元组数据，要排除页头以及任何预期的
 * 特殊空间。
 */
void
table_block_relation_estimate_size(Relation rel, int32 *attr_widths,
								   BlockNumber *pages, double *tuples,
								   double *allvisfrac,
								   Size overhead_bytes_per_tuple,
								   Size usable_bytes_per_page)
{
	BlockNumber curpages;
	BlockNumber relpages;
	double		reltuples;
	BlockNumber relallvisible;
	double		density;

	/* 关系应当有存储，因此可以调用 smgr */
	curpages = RelationGetNumberOfBlocks(rel);

	/* 把 pg_class 中的值强制转换成更合适的类型 */
	relpages = (BlockNumber) rel->rd_rel->relpages;
	reltuples = (double) rel->rd_rel->reltuples;
	relallvisible = (BlockNumber) rel->rd_rel->relallvisible;

	/*
	 * HACK: 若关系从未做过清理，就用最小为 10 页的大小
	 * 估计。这里是为了避免把
	 * 新建的表当成真的很小，即使它目前确实很小，因为
	 * 一旦装入一些数据，这就可能不成立。一旦做过一轮清理
	 * 或分析，就更有理由相信
	 * 大小大体是稳定的。
	 *
	 * （注意，只有计划被缓存，并在表填满之后再次
	 * 使用时，这才成问题。我们要避免的是对一张
	 * 自计划生成以来已显著增长的表使用嵌套循环型
	 * 计划。通常，插入足够多之后会发生自动清理/自动分析，
	 * 并使已缓存的计划失效；但这
	 * 不会立刻发生，而且对于
	 * 临时表这类情况根本不会发生。）
	 *
	 * 我们用 reltuples < 0 来检验是否 "从未清理过"。
	 *
	 * 若表有继承子表，就不使用这一启发式。
	 * 完全空的父表相当常见，因此应当愿意
	 * 相信它们是空的。
	 */
	if (curpages < 10 &&
		reltuples < 0 &&
		!rel->rd_rel->relhassubclass)
		curpages = 10;

	/* 报告估计的页数 */
	*pages = curpages;
	/* 若 rel 明显为空则快速退出 */
	if (curpages == 0)
	{
		*tuples = 0;
		*allvisfrac = 0;
		return;
	}

	/* 按先前的元组密度估计元组数 */
	if (reltuples >= 0 && relpages > 0)
		density = reltuples / (double) relpages;
	else
	{
		/*
		 * 因为关系从未做过清理而没有数据时，
		 * 从属性的数据类型估计元组宽度。这里假定
		 * 页面是完全装满的，对表来说可以，但对
		 * 索引很可能是高估。所幸
		 * get_relation_info() 可以把这一高估钳制到父
		 * 表的大小。
		 *
		 * 注意：这段代码有意不考虑对齐，
		 * 因为 (a) 估计已经很粗，再考虑对齐是画蛇添足，
		 * (b) 那会让默认计划产生平台依赖，
		 * 对回归测试相当头疼，
		 * 并且 (c) 不同的表 AM 可能使用不同的填充方案。
		 */
		int32		tuple_width;

		tuple_width = get_rel_data_width(rel, attr_widths);
		tuple_width += overhead_bytes_per_tuple;
		/* 注意：这里有意使用整数除法 */
		density = usable_bytes_per_page / tuple_width;
	}
	*tuples = rint(density * (double) curpages);

	/*
	 * relallvisible 按原样使用，而不像处理页数和元组数那样把它放大，
	 * 理由是自上次 VACUUM 以来新增的页面
	 * 很可能没有被标成全可见。但 costsize.c
	 * 希望把它转换成一个分数。
	 */
	if (relallvisible == 0 || curpages <= 0)
		*allvisfrac = 0;
	else if ((double) relallvisible >= curpages)
		*allvisfrac = 1;
	else
		*allvisfrac = (double) relallvisible / curpages;
}
