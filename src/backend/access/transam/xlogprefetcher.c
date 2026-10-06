/*-------------------------------------------------------------------------
 *
 * xlogprefetcher.c
 *		恢复期间的预取支持。
 *
 * Portions Copyright (c) 2022-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogprefetcher.c
 *
 * 本模块提供 XLogReader 的即插即用替代，通过在 WAL 中向前
 * 查看来尽量减少 I/O 停顿。若近期将要访问的块
 * 尚不在缓冲池中，它会发起 I/O，以期在调用者最终
 * 需要数据之前完成。当被引用的块已在缓冲池中时，
 * 会把缓冲区记入已解码记录，以便
 * XLogReadBufferForRedo() 尝试避免第二次缓冲区映射表
 * 查找。
 *
 * 目前，预取只考虑主分支。目前，预取只在
 * PrefetchBuffer() 能发挥实际作用的系统上有效（主要是
 * Linux）。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlogprefetcher.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "catalog/pg_class.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"
#include "funcapi.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "utils/guc_hooks.h"
#include "utils/hsearch.h"

/*
 * 每处理这么多 WAL，就更新
 * pg_stat_recovery_prefetch 中的值。
 */
#define XLOGPREFETCHER_STATS_DISTANCE BLCKSZ

/*
 * 为检测对同一块的重复访问并跳过无用的额外系统
 * 调用，我们记住最近预取过的一小段窗口内的块。
 */
#define XLOGPREFETCHER_SEQ_WINDOW_SIZE 4

/*
 * 当 maintenance_io_concurrency 尚未饱和时，我们准备向前
 * 查看最多 N 倍于该值的块引用数。
 */
#define XLOGPREFETCHER_DISTANCE_MULTIPLIER 4

/* 定义此项以记录内部调试消息。 */
/* #define XLOGPREFETCHER_DEBUG_LEVEL LOG */

/* GUCs */
int			recovery_prefetch = RECOVERY_PREFETCH_TRY;

#ifdef USE_PREFETCH
#define RecoveryPrefetchEnabled() \
		(recovery_prefetch != RECOVERY_PREFETCH_OFF && \
		 maintenance_io_concurrency > 0)
#else
#define RecoveryPrefetchEnabled() false
#endif

static int	XLogPrefetchReconfigureCount = 0;

/*
 * 用于报告是否应启动 IO 的枚举。
 */
typedef enum
{
	LRQ_NEXT_NO_IO,
	LRQ_NEXT_IO,
	LRQ_NEXT_AGAIN
} LsnReadQueueNextStatus;

/*
 * 可决定下一个应预取哪一块的回调类型。目前
 * 只有一种。
 */
typedef LsnReadQueueNextStatus (*LsnReadQueueNextFun) (uintptr_t lrq_private,
													   XLogRecPtr *lsn);

/*
 * 简单的 LSNs 环形队列，用于控制（可能）在途
 * IO 的数量。它暂代日后更通用的 IO
 * 控制机制，因此才看起来多此一举地
 * 通过函数指针间接调用。
 */
typedef struct LsnReadQueue
{
	LsnReadQueueNextFun next;
	uintptr_t	lrq_private;
	uint32		max_inflight;
	uint32		inflight;
	uint32		completed;
	uint32		head;
	uint32		tail;
	uint32		size;
	struct
	{
		bool		io;
		XLogRecPtr	lsn;
	}			queue[FLEXIBLE_ARRAY_MEMBER];
} LsnReadQueue;

/*
 * 预取器。这是包装 XLogReader 的机制，预取
 * 即将被引用的块，以尽量避免 IO 停顿。
 */
struct XLogPrefetcher
{
	/* WAL 读取器与当前读取状态。 */
	XLogReaderState *reader;
	DecodedXLogRecord *record;
	int			next_block_id;

	/* 何时发布统计信息。 */
	XLogRecPtr	next_stats_shm_lsn;

	/* 记账：避免访问尚不存在的块。 */
	HTAB	   *filter_table;
	dlist_head	filter_queue;

	/* 记账：避免重复预取。 */
	RelFileLocator recent_rlocator[XLOGPREFETCHER_SEQ_WINDOW_SIZE];
	BlockNumber recent_block[XLOGPREFETCHER_SEQ_WINDOW_SIZE];
	int			recent_idx;

	/* 记账：临时禁用预取。 */
	XLogRecPtr	no_readahead_until;

	/* IO 深度管理器。 */
	LsnReadQueue *streaming_read;

	XLogRecPtr	begin_ptr;

	int			reconfigure_count;
};

/*
 * 临时过滤器，用于跟踪尚未创建的块范围、
 * 尚未创建的整个关系，以及（我们假定）已经
 * 删除的整个关系，或将由批量 WAL
 * 操作创建的关系。
 */
typedef struct XLogPrefetcherFilter
{
	RelFileLocator rlocator;
	XLogRecPtr	filter_until_replayed;
	BlockNumber filter_from_block;
	dlist_node	link;
} XLogPrefetcherFilter;

/*
 * 在共享内存中暴露给 pg_stat_recovery_prefetch 的计数器。
 */
typedef struct XLogPrefetchStats
{
	pg_atomic_uint64 reset_time;	/* 上次重置的时间。 */
	pg_atomic_uint64 prefetch;	/* 已发起的预取次数。 */
	pg_atomic_uint64 hit;		/* 已在缓存中的块。 */
	pg_atomic_uint64 skip_init; /* 跳过的零初始化块。 */
	pg_atomic_uint64 skip_new;	/* 过滤掉的新建/不存在块。 */
	pg_atomic_uint64 skip_fpw;	/* 跳过的 FPWs。 */
	pg_atomic_uint64 skip_rep;	/* 跳过的重复访问。 */

	/* 动态值 */
	int			wal_distance;	/* 领先的 WAL 字节数。 */
	int			block_distance; /* 领先的块引用数。 */
	int			io_depth;		/* 进行中的 I/O 数。 */
} XLogPrefetchStats;

static inline void XLogPrefetcherAddFilter(XLogPrefetcher *prefetcher,
										   RelFileLocator rlocator,
										   BlockNumber blockno,
										   XLogRecPtr lsn);
static inline bool XLogPrefetcherIsFiltered(XLogPrefetcher *prefetcher,
											RelFileLocator rlocator,
											BlockNumber blockno);
static inline void XLogPrefetcherCompleteFilters(XLogPrefetcher *prefetcher,
												 XLogRecPtr replaying_lsn);
static LsnReadQueueNextStatus XLogPrefetcherNextBlock(uintptr_t pgsr_private,
													  XLogRecPtr *lsn);

static XLogPrefetchStats *SharedStats;

static inline LsnReadQueue *
lrq_alloc(uint32 max_distance,
		  uint32 max_inflight,
		  uintptr_t lrq_private,
		  LsnReadQueueNextFun next)
{
	LsnReadQueue *lrq;
	uint32		size;

	Assert(max_distance >= max_inflight);

	size = max_distance + 1;	/* 满的环形缓冲区留有空隙 */
	lrq = palloc(offsetof(LsnReadQueue, queue) + sizeof(lrq->queue[0]) * size);
	lrq->lrq_private = lrq_private;
	lrq->max_inflight = max_inflight;
	lrq->size = size;
	lrq->next = next;
	lrq->head = 0;
	lrq->tail = 0;
	lrq->inflight = 0;
	lrq->completed = 0;

	return lrq;
}

static inline void
lrq_free(LsnReadQueue *lrq)
{
	pfree(lrq);
}

static inline uint32
lrq_inflight(LsnReadQueue *lrq)
{
	return lrq->inflight;
}

static inline uint32
lrq_completed(LsnReadQueue *lrq)
{
	return lrq->completed;
}

static inline void
lrq_prefetch(LsnReadQueue *lrq)
{
	/* 在限制内尽量启动尽可能多的 IO。 */
	while (lrq->inflight < lrq->max_inflight &&
		   lrq->inflight + lrq->completed < lrq->size - 1)
	{
		Assert(((lrq->head + 1) % lrq->size) != lrq->tail);
		switch (lrq->next(lrq->lrq_private, &lrq->queue[lrq->head].lsn))
		{
			case LRQ_NEXT_AGAIN:
				return;
			case LRQ_NEXT_IO:
				lrq->queue[lrq->head].io = true;
				lrq->inflight++;
				break;
			case LRQ_NEXT_NO_IO:
				lrq->queue[lrq->head].io = false;
				lrq->completed++;
				break;
		}
		lrq->head++;
		if (lrq->head == lrq->size)
			lrq->head = 0;
	}
}

static inline void
lrq_complete_lsn(LsnReadQueue *lrq, XLogRecPtr lsn)
{
	/*
	 * 我们知道 'lsn' 之前的 LSNs 已重放，因此现在可以假定
	 * 在那之前启动的任何 IO 都已完成。
	 */
	while (lrq->tail != lrq->head &&
		   lrq->queue[lrq->tail].lsn < lsn)
	{
		if (lrq->queue[lrq->tail].io)
			lrq->inflight--;
		else
			lrq->completed--;
		lrq->tail++;
		if (lrq->tail == lrq->size)
			lrq->tail = 0;
	}
	if (RecoveryPrefetchEnabled())
		lrq_prefetch(lrq);
}

size_t
XLogPrefetchShmemSize(void)
{
	return sizeof(XLogPrefetchStats);
}

/*
 * 将所有计数器重置为零。
 */
void
XLogPrefetchResetStats(void)
{
	pg_atomic_write_u64(&SharedStats->reset_time, GetCurrentTimestamp());
	pg_atomic_write_u64(&SharedStats->prefetch, 0);
	pg_atomic_write_u64(&SharedStats->hit, 0);
	pg_atomic_write_u64(&SharedStats->skip_init, 0);
	pg_atomic_write_u64(&SharedStats->skip_new, 0);
	pg_atomic_write_u64(&SharedStats->skip_fpw, 0);
	pg_atomic_write_u64(&SharedStats->skip_rep, 0);
}

void
XLogPrefetchShmemInit(void)
{
	bool		found;

	SharedStats = (XLogPrefetchStats *)
		ShmemInitStruct("XLogPrefetchStats",
						sizeof(XLogPrefetchStats),
						&found);

	if (!found)
	{
		pg_atomic_init_u64(&SharedStats->reset_time, GetCurrentTimestamp());
		pg_atomic_init_u64(&SharedStats->prefetch, 0);
		pg_atomic_init_u64(&SharedStats->hit, 0);
		pg_atomic_init_u64(&SharedStats->skip_init, 0);
		pg_atomic_init_u64(&SharedStats->skip_new, 0);
		pg_atomic_init_u64(&SharedStats->skip_fpw, 0);
		pg_atomic_init_u64(&SharedStats->skip_rep, 0);
	}
}

/*
 * 当任何影响预取的 GUC 被更改时调用。
 */
void
XLogPrefetchReconfigure(void)
{
	XLogPrefetchReconfigureCount++;
}

/*
 * 递增共享内存中的计数器。这等价于对普通
 * uint64 做 *counter++，无需内存屏障或加锁，但在读者无法
 * 无撕裂地读取 uint64 的平台上除外。
 */
static inline void
XLogPrefetchIncrement(pg_atomic_uint64 *counter)
{
	Assert(AmStartupProcess() || !IsUnderPostmaster);
	pg_atomic_write_u64(counter, pg_atomic_read_u64(counter) + 1);
}

/*
 * 创建一个已准备好开始预取 WAL 记录所引用块的
 * 预取器。
 */
XLogPrefetcher *
XLogPrefetcherAllocate(XLogReaderState *reader)
{
	XLogPrefetcher *prefetcher;
	static HASHCTL hash_table_ctl = {
		.keysize = sizeof(RelFileLocator),
		.entrysize = sizeof(XLogPrefetcherFilter)
	};

	prefetcher = palloc0(sizeof(XLogPrefetcher));

	prefetcher->reader = reader;
	prefetcher->filter_table = hash_create("XLogPrefetcherFilterTable", 1024,
										   &hash_table_ctl,
										   HASH_ELEM | HASH_BLOBS);
	dlist_init(&prefetcher->filter_queue);

	SharedStats->wal_distance = 0;
	SharedStats->block_distance = 0;
	SharedStats->io_depth = 0;

	/* 首次使用时会分配 streaming_read。 */
	prefetcher->reconfigure_count = XLogPrefetchReconfigureCount - 1;

	return prefetcher;
}

/*
 * 销毁预取器并释放全部资源。
 */
void
XLogPrefetcherFree(XLogPrefetcher *prefetcher)
{
	lrq_free(prefetcher->streaming_read);
	hash_destroy(prefetcher->filter_table);
	pfree(prefetcher);
}

/*
 * 提供对读取器的访问。
 */
XLogReaderState *
XLogPrefetcherGetReader(XLogPrefetcher *prefetcher)
{
	return prefetcher->reader;
}

/*
 * 更新在 pg_stat_recovery_prefetch 视图中可见的统计信息。
 */
void
XLogPrefetcherComputeStats(XLogPrefetcher *prefetcher)
{
	uint32		io_depth;
	uint32		completed;
	int64		wal_distance;


	/* 我们现在领先重放多远？ */
	if (prefetcher->reader->decode_queue_tail)
	{
		wal_distance =
			prefetcher->reader->decode_queue_tail->lsn -
			prefetcher->reader->decode_queue_head->lsn;
	}
	else
	{
		wal_distance = 0;
	}

	/* 当前有多少 IO 在途以及已完成？ */
	io_depth = lrq_inflight(prefetcher->streaming_read);
	completed = lrq_completed(prefetcher->streaming_read);

	/* 更新在 pg_stat_recovery_prefetch 中可见的瞬时统计。 */
	SharedStats->io_depth = io_depth;
	SharedStats->block_distance = io_depth + completed;
	SharedStats->wal_distance = wal_distance;

	prefetcher->next_stats_shm_lsn =
		prefetcher->reader->ReadRecPtr + XLOGPREFETCHER_STATS_DISTANCE;
}

/*
 * 检查 WAL 中下一个块引用的回调，并可能
 * 启动 IO，使稍后的读取更快。
 *
 * 若尚无更多 WAL 数据可用，则返回 LRQ_NEXT_AGAIN。
 *
 * 若下一个块引用属于主分支上且不在缓冲池中的
 * 块，并且已请求内核开始读取以使日后的读
 * 系统调用更快，则返回 LRQ_NEXT_IO。会向 *lsn
 * 写入一个 LSN，一旦该 LSN 被重放，即认为该 I/O
 * 已完成。
 *
 * 若检查了下一个块引用后发现它已在缓冲池中，
 * 或因各种原因决定不预取，则返回
 * LRQ_NEXT_NO_IO。
 */
static LsnReadQueueNextStatus
XLogPrefetcherNextBlock(uintptr_t pgsr_private, XLogRecPtr *lsn)
{
	XLogPrefetcher *prefetcher = (XLogPrefetcher *) pgsr_private;
	XLogReaderState *reader = prefetcher->reader;
	XLogRecPtr	replaying_lsn = reader->ReadRecPtr;

	/*
	 * 我们用 prefetcher->record 与 prefetcher->next_block_id
	 * 在各次调用之间跟踪当前处理到的记录与块。
	 */
	for (;;)
	{
		DecodedXLogRecord *record;

		/* 若尚无记录，则尝试读取一条未来的新记录。 */
		if (prefetcher->record == NULL)
		{
			bool		nonblocking;

			/*
			 * 若已有可重放的记录或错误在队列中，
			 * 我们不想在此阻塞。否则可以
			 * 阻塞等待更多数据：想必调用者
			 * 没有别的事可做。
			 */
			nonblocking = XLogReaderHasQueuedRecordOrError(reader);

			/* 在重放过某一点之前禁用预读。 */
			if (nonblocking && replaying_lsn <= prefetcher->no_readahead_until)
				return LRQ_NEXT_AGAIN;

			record = XLogReadAhead(prefetcher->reader, nonblocking);
			if (record == NULL)
			{
				/*
				 * 由于出错或非阻塞模式下缺少数据，我们无法
				 * 再读。在已解码内容全部重放完之前，
				 * 不要再尝试向前读取。
				 */
				if (nonblocking && prefetcher->reader->decode_queue_tail)
					prefetcher->no_readahead_until =
						prefetcher->reader->decode_queue_tail->lsn;

				return LRQ_NEXT_AGAIN;
			}

			/*
			 * 若预取已禁用，则无需分析记录
			 * 或发出任何预取。我们只需促使一条记录
			 * 被解码。
			 */
			if (!RecoveryPrefetchEnabled())
			{
				*lsn = InvalidXLogRecPtr;
				return LRQ_NEXT_NO_IO;
			}

			/* 我们有一条新记录要处理。 */
			prefetcher->record = record;
			prefetcher->next_block_id = 0;
		}
		else
		{
			/* 从上次调用或上一轮循环继续处理。 */
			record = prefetcher->record;
		}

		/*
		 * 检查需要我们过滤掉块范围，或
		 * 完全暂停预读的操作。
		 */
		if (replaying_lsn < record->lsn)
		{
			uint8		rmid = record->header.xl_rmid;
			uint8		record_type = record->header.xl_info & ~XLR_INFO_MASK;

			if (rmid == RM_XLOG_ID)
			{
				if (record_type == XLOG_CHECKPOINT_SHUTDOWN ||
					record_type == XLOG_END_OF_RECOVERY)
				{
					/*
					 * 这些记录可能会更改 TLI。若允许"读取 TLI"与
					 * "重放 TLI"在未做更多分析的情况下不同，
					 * 可能引入潜在缺陷。
					 */
					prefetcher->no_readahead_until = record->lsn;

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
					elog(XLOGPREFETCHER_DEBUG_LEVEL,
						 "suppressing all readahead until %X/%X is replayed due to possible TLI change",
						 LSN_FORMAT_ARGS(record->lsn));
#endif

					/* Fall through so we move past this record. */
				}
			}
			else if (rmid == RM_DBASE_ID)
			{
				/*
				 * 当用文件复制策略创建数据库时，
				 * 没有 WAL 记录告诉我们各个关系的
				 * 创建情况。
				 */
				if (record_type == XLOG_DBASE_CREATE_FILE_COPY)
				{
					xl_dbase_create_file_copy_rec *xlrec =
						(xl_dbase_create_file_copy_rec *) record->main_data;
					RelFileLocator rlocator =
					{InvalidOid, xlrec->db_id, InvalidRelFileNumber};

					/*
					 * 在该数据库创建完成之前不要预取其中
					 * 任何内容，否则若数据库 OID 或
					 * relfilenumber 被重用，可能混淆
					 * 不同代的块。这也比用 ENOENT 错误
					 * 去发现关系尚不在磁盘上更
					 * 高效。
					 */
					XLogPrefetcherAddFilter(prefetcher, rlocator, 0, record->lsn);

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
					elog(XLOGPREFETCHER_DEBUG_LEVEL,
						 "suppressing prefetch in database %u until %X/%X is replayed due to raw file copy",
						 rlocator.dbOid,
						 LSN_FORMAT_ARGS(record->lsn));
#endif
				}
			}
			else if (rmid == RM_SMGR_ID)
			{
				if (record_type == XLOG_SMGR_CREATE)
				{
					xl_smgr_create *xlrec = (xl_smgr_create *)
						record->main_data;

					if (xlrec->forkNum == MAIN_FORKNUM)
					{
						/*
						 * 在该关系创建完成之前不要为其预取
						 * 任何内容。否则若
						 * relfilenumber 被重用，可能混淆
						 * 不同代的块。这也避免了通过额外
						 * 报告 ENOENT 的系统调用去发现
						 * 问题。
						 */
						XLogPrefetcherAddFilter(prefetcher, xlrec->rlocator, 0,
												record->lsn);

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
						elog(XLOGPREFETCHER_DEBUG_LEVEL,
							 "suppressing prefetch in relation %u/%u/%u until %X/%X is replayed, which creates the relation",
							 xlrec->rlocator.spcOid,
							 xlrec->rlocator.dbOid,
							 xlrec->rlocator.relNumber,
							 LSN_FORMAT_ARGS(record->lsn));
#endif
					}
				}
				else if (record_type == XLOG_SMGR_TRUNCATE)
				{
					xl_smgr_truncate *xlrec = (xl_smgr_truncate *)
						record->main_data;

					/*
					 * 在截断执行完成之前，不要考虑预取
					 * 被截断范围内的任何内容。
					 */
					XLogPrefetcherAddFilter(prefetcher, xlrec->rlocator,
											xlrec->blkno,
											record->lsn);

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
					elog(XLOGPREFETCHER_DEBUG_LEVEL,
						 "suppressing prefetch in relation %u/%u/%u from block %u until %X/%X is replayed, which truncates the relation",
						 xlrec->rlocator.spcOid,
						 xlrec->rlocator.dbOid,
						 xlrec->rlocator.relNumber,
						 xlrec->blkno,
						 LSN_FORMAT_ARGS(record->lsn));
#endif
				}
			}
		}

		/* 从上次停下的地方继续扫描块引用。 */
		while (prefetcher->next_block_id <= record->max_block_id)
		{
			int			block_id = prefetcher->next_block_id++;
			DecodedBkpBlock *block = &record->blocks[block_id];
			SMgrRelation reln;
			PrefetchBufferResult result;

			if (!block->in_use)
				continue;

			Assert(!BufferIsValid(block->prefetch_buffer));

			/*
			 * 记录本条记录的 LSN。当它被重放时，
			 * LsnReadQueue 会认为为更早 LSNs 提交的任何 IO
			 * 均已完成。
			 */
			*lsn = record->lsn;

			/* 目前我们只尝试预取主分支。 */
			if (block->forknum != MAIN_FORKNUM)
			{
				return LRQ_NEXT_NO_IO;
			}

			/*
			 * 若附有整页映像，我们就不会读该
			 * 页，因此不必费心预取。
			 */
			if (block->has_image)
			{
				XLogPrefetchIncrement(&SharedStats->skip_fpw);
				return LRQ_NEXT_NO_IO;
			}

			/* 读取将被清零的页没有意义。 */
			if (block->flags & BKPBLOCK_WILL_INIT)
			{
				XLogPrefetchIncrement(&SharedStats->skip_init);
				return LRQ_NEXT_NO_IO;
			}

			/* 是否因过滤器而应跳过预取该块？ */
			if (XLogPrefetcherIsFiltered(prefetcher, block->rlocator, block->blkno))
			{
				XLogPrefetchIncrement(&SharedStats->skip_new);
				return LRQ_NEXT_NO_IO;
			}

			/* 反复预取同一块没有意义。 */
			for (int i = 0; i < XLOGPREFETCHER_SEQ_WINDOW_SIZE; ++i)
			{
				if (block->blkno == prefetcher->recent_block[i] &&
					RelFileLocatorEquals(block->rlocator, prefetcher->recent_rlocator[i]))
				{
					/*
					 * XXX 若我们还记住了它在哪里，就可以设置
					 * recent_buffer，使恢复能跳过 smgropen()
					 * 和一次缓冲区表查找。
					 */
					XLogPrefetchIncrement(&SharedStats->skip_rep);
					return LRQ_NEXT_NO_IO;
				}
			}
			prefetcher->recent_rlocator[prefetcher->recent_idx] = block->rlocator;
			prefetcher->recent_block[prefetcher->recent_idx] = block->blkno;
			prefetcher->recent_idx =
				(prefetcher->recent_idx + 1) % XLOGPREFETCHER_SEQ_WINDOW_SIZE;

			/*
			 * 我们可以尝试为对同一关系的重复引用提供
			 * 快速路径（用某种方案安全处理失效），
			 * 但目前每次都调用 smgropen()。
			 */
			reln = smgropen(block->rlocator, InvalidBackendId);

			/*
			 * 若关系文件在磁盘上不存在，例如因为我们在
			 * 崩溃后重放，而该文件将由尚未重放的 WAL
			 * 创建随后又被取消链接，则在本条记录
			 * 重放之前抑制对该关系的进一步
			 * 预取。
			 */
			if (!smgrexists(reln, MAIN_FORKNUM))
			{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
				elog(XLOGPREFETCHER_DEBUG_LEVEL,
					 "suppressing all prefetch in relation %u/%u/%u until %X/%X is replayed, because the relation does not exist on disk",
					 reln->smgr_rlocator.locator.spcOid,
					 reln->smgr_rlocator.locator.dbOid,
					 reln->smgr_rlocator.locator.relNumber,
					 LSN_FORMAT_ARGS(record->lsn));
#endif
				XLogPrefetcherAddFilter(prefetcher, block->rlocator, 0,
										record->lsn);
				XLogPrefetchIncrement(&SharedStats->skip_new);
				return LRQ_NEXT_NO_IO;
			}

			/*
			 * 若关系尚不够大，容纳不下被引用的
			 * 块，则在本条记录重放之前抑制对该块及更高
			 * 块号的预取。
			 */
			if (block->blkno >= smgrnblocks(reln, block->forknum))
			{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
				elog(XLOGPREFETCHER_DEBUG_LEVEL,
					 "suppressing prefetch in relation %u/%u/%u from block %u until %X/%X is replayed, because the relation is too small",
					 reln->smgr_rlocator.locator.spcOid,
					 reln->smgr_rlocator.locator.dbOid,
					 reln->smgr_rlocator.locator.relNumber,
					 block->blkno,
					 LSN_FORMAT_ARGS(record->lsn));
#endif
				XLogPrefetcherAddFilter(prefetcher, block->rlocator, block->blkno,
										record->lsn);
				XLogPrefetchIncrement(&SharedStats->skip_new);
				return LRQ_NEXT_NO_IO;
			}

			/* 尝试发起预取。 */
			result = PrefetchSharedBuffer(reln, block->forknum, block->blkno);
			if (BufferIsValid(result.recent_buffer))
			{
				/* 缓存命中，无需做事。 */
				XLogPrefetchIncrement(&SharedStats->hit);
				block->prefetch_buffer = result.recent_buffer;
				return LRQ_NEXT_NO_IO;
			}
			else if (result.initiated_io)
			{
				/* 缓存未命中，I/O（想必）已启动。 */
				XLogPrefetchIncrement(&SharedStats->prefetch);
				block->prefetch_buffer = InvalidBuffer;
				return LRQ_NEXT_IO;
			}
			else if ((io_direct_flags & IO_DIRECT_DATA) == 0)
			{
				/*
				 * 这不应发生，因为我们已经确定
				 * 关系在磁盘上存在且足够大。
				 * 是 smgrexists()、smgrnblocks() 的缓存失效有问题，
				 * 还是文件在我们脚下被取消链接或
				 * 截断了？
				 */
				elog(ERROR,
					 "could not prefetch relation %u/%u/%u block %u",
					 reln->smgr_rlocator.locator.spcOid,
					 reln->smgr_rlocator.locator.dbOid,
					 reln->smgr_rlocator.locator.relNumber,
					 block->blkno);
			}
		}

		/*
		 * 若干调用点需要能恰好读取一条记录，
		 * 且不做任何内部预读。例如：xlog.c 在 emode 为
		 * PANIC 时读取检查点记录，否则可能使
		 * XLogPageRead() 在某个未来页上 panic；以及 xlog.c
		 * 确定下一步从何处写 WAL，这依赖于读完一条记录后
		 * 读取器内部缓冲区的内容。
		 * 因此，在消费完 XLogPrefetcherBeginRead() 之后的
		 * 第一条记录之前，甚至不要考虑预取。
		 */
		if (prefetcher->reader->decode_queue_tail &&
			prefetcher->reader->decode_queue_tail->lsn == prefetcher->begin_ptr)
			return LRQ_NEXT_AGAIN;

		/* 前进到下一条记录。 */
		prefetcher->record = NULL;
	}
	pg_unreachable();
}

/*
 * 暴露关于恢复预取的统计信息。
 */
Datum
pg_stat_get_recovery_prefetch(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_RECOVERY_PREFETCH_COLS 10
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		values[PG_STAT_GET_RECOVERY_PREFETCH_COLS];
	bool		nulls[PG_STAT_GET_RECOVERY_PREFETCH_COLS];

	InitMaterializedSRF(fcinfo, 0);

	for (int i = 0; i < PG_STAT_GET_RECOVERY_PREFETCH_COLS; ++i)
		nulls[i] = false;

	values[0] = TimestampTzGetDatum(pg_atomic_read_u64(&SharedStats->reset_time));
	values[1] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->prefetch));
	values[2] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->hit));
	values[3] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_init));
	values[4] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_new));
	values[5] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_fpw));
	values[6] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_rep));
	values[7] = Int32GetDatum(SharedStats->wal_distance);
	values[8] = Int32GetDatum(SharedStats->block_distance);
	values[9] = Int32GetDatum(SharedStats->io_depth);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	return (Datum) 0;
}

/*
 * 在给定 'rlocator' 中，在 'lsn' 被重放之前，不要预取任何
 * 块号 >= 'blockno' 的块。
 */
static inline void
XLogPrefetcherAddFilter(XLogPrefetcher *prefetcher, RelFileLocator rlocator,
						BlockNumber blockno, XLogRecPtr lsn)
{
	XLogPrefetcherFilter *filter;
	bool		found;

	filter = hash_search(prefetcher->filter_table, &rlocator, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * 在重放之前不允许预取该块或更高块号。
		 */
		filter->filter_until_replayed = lsn;
		filter->filter_from_block = blockno;
		dlist_push_head(&prefetcher->filter_queue, &filter->link);
	}
	else
	{
		/*
		 * 我们已经在过滤此 rlocator。延长过滤器的
		 * 生命周期以覆盖本条 WAL 记录，但保留其中较低的块
		 * 号，因为我们不想跟踪各个
		 * 块。
		 */
		filter->filter_until_replayed = lsn;
		dlist_delete(&filter->link);
		dlist_push_head(&prefetcher->filter_queue, &filter->link);
		filter->filter_from_block = Min(filter->filter_from_block, blockno);
	}
}

/*
 * 我们是否已重放了导致开始过滤某块
 * 范围的记录？这意味着关系应按需被创建、扩展或
 * 删除，因此可以停止过滤对给定
 * relfilenumber 的访问。
 */
static inline void
XLogPrefetcherCompleteFilters(XLogPrefetcher *prefetcher, XLogRecPtr replaying_lsn)
{
	while (unlikely(!dlist_is_empty(&prefetcher->filter_queue)))
	{
		XLogPrefetcherFilter *filter = dlist_tail_element(XLogPrefetcherFilter,
														  link,
														  &prefetcher->filter_queue);

		if (filter->filter_until_replayed >= replaying_lsn)
			break;

		dlist_delete(&filter->link);
		hash_search(prefetcher->filter_table, filter, HASH_REMOVE, NULL);
	}
}

/*
 * 检查给定块是否因过滤器而应跳过。
 */
static inline bool
XLogPrefetcherIsFiltered(XLogPrefetcher *prefetcher, RelFileLocator rlocator,
						 BlockNumber blockno)
{
	/*
	 * 先测试队列是否为空，因为我们预期它多数时间
	 * 为空，此时可避免哈希表查找。
	 */
	if (unlikely(!dlist_is_empty(&prefetcher->filter_queue)))
	{
		XLogPrefetcherFilter *filter;

		/* 看该块范围是否被过滤。 */
		filter = hash_search(prefetcher->filter_table, &rlocator, HASH_FIND, NULL);
		if (filter && filter->filter_from_block <= blockno)
		{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
			elog(XLOGPREFETCHER_DEBUG_LEVEL,
				 "prefetch of %u/%u/%u block %u suppressed; filtering until LSN %X/%X is replayed (blocks >= %u filtered)",
				 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber, blockno,
				 LSN_FORMAT_ARGS(filter->filter_until_replayed),
				 filter->filter_from_block);
#endif
			return true;
		}

		/* 看整个数据库是否被过滤。 */
		rlocator.relNumber = InvalidRelFileNumber;
		rlocator.spcOid = InvalidOid;
		filter = hash_search(prefetcher->filter_table, &rlocator, HASH_FIND, NULL);
		if (filter)
		{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
			elog(XLOGPREFETCHER_DEBUG_LEVEL,
				 "prefetch of %u/%u/%u block %u suppressed; filtering until LSN %X/%X is replayed (whole database)",
				 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber, blockno,
				 LSN_FORMAT_ARGS(filter->filter_until_replayed));
#endif
			return true;
		}
	}

	return false;
}

/*
 * XLogBeginRead() 的包装，同时重置预取器。
 */
void
XLogPrefetcherBeginRead(XLogPrefetcher *prefetcher, XLogRecPtr recPtr)
{
	/* 这将忘记任何在途 IO。 */
	prefetcher->reconfigure_count--;

	/* 记账：避免首次读取时预读。 */
	prefetcher->begin_ptr = recPtr;

	prefetcher->no_readahead_until = 0;

	/* 这将忘记解码器中任何已排队的记录。 */
	XLogBeginRead(prefetcher->reader, recPtr);
}

/*
 * XLogReadRecord() 的包装，提供相同接口，同时还
 * 尝试为未来 WAL 记录所引用的块发起 I/O。
 */
XLogRecord *
XLogPrefetcherReadRecord(XLogPrefetcher *prefetcher, char **errmsg)
{
	DecodedXLogRecord *record;
	XLogRecPtr	replayed_up_to;

	/*
	 * 看是否该重置预取机制，因为相关
	 * GUC 已更改。
	 */
	if (unlikely(XLogPrefetchReconfigureCount != prefetcher->reconfigure_count))
	{
		uint32		max_distance;
		uint32		max_inflight;

		if (prefetcher->streaming_read)
			lrq_free(prefetcher->streaming_read);

		if (RecoveryPrefetchEnabled())
		{
			Assert(maintenance_io_concurrency > 0);
			max_inflight = maintenance_io_concurrency;
			max_distance = max_inflight * XLOGPREFETCHER_DISTANCE_MULTIPLIER;
		}
		else
		{
			max_inflight = 1;
			max_distance = 1;
		}

		prefetcher->streaming_read = lrq_alloc(max_distance,
											   max_inflight,
											   (uintptr_t) prefetcher,
											   XLogPrefetcherNextBlock);

		prefetcher->reconfigure_count = XLogPrefetchReconfigureCount;
	}

	/*
	 * 若有上次返回的记录，则释放它，因为它现已
	 * 被重放。
	 */
	replayed_up_to = XLogReleasePreviousRecord(prefetcher->reader);

	/*
	 * 是否可以丢掉一些过滤器了？若我们在等待关系被
	 * 创建或扩展，现在可以访问覆盖范围内的
	 * 块了。
	 */
	XLogPrefetcherCompleteFilters(prefetcher, replayed_up_to);

	/*
	 * 由更早 WAL 发起的全部 IO 现已完成。这可能触发
	 * 进一步预取。
	 */
	lrq_complete_lsn(prefetcher->streaming_read, replayed_up_to);

	/*
	 * 若尚无内容入队，则开始预取以使至少
	 * 一条记录入队。
	 */
	if (!XLogReaderHasQueuedRecordOrError(prefetcher->reader))
	{
		Assert(lrq_inflight(prefetcher->streaming_read) == 0);
		Assert(lrq_completed(prefetcher->streaming_read) == 0);
		lrq_prefetch(prefetcher->streaming_read);
	}

	/* 读取下一条记录。 */
	record = XLogNextRecord(prefetcher->reader, errmsg);
	if (!record)
		return NULL;

	/*
	 * 我们刚得到的记录是"当前"记录，以供
	 * XLogRecXXX() 宏使用。
	 */
	Assert(record == prefetcher->reader->record);

	/*
	 * 若 maintenance_io_concurrency 设得很低，我们可能已开始
	 * 预取即将返回的记录中部分但非全部被引用的块。
	 * 通过丢掉预取器对该记录的引用，忘记
	 * 其中其余的块。
	 */
	if (record == prefetcher->record)
		prefetcher->record = NULL;

	/*
	 * 看是否该计算一些统计信息，因为已处理了
	 * 足够多的 WAL。
	 */
	if (unlikely(record->lsn >= prefetcher->next_stats_shm_lsn))
		XLogPrefetcherComputeStats(prefetcher);

	Assert(record == prefetcher->reader->record);

	return &record->header;
}

bool
check_recovery_prefetch(int *new_value, void **extra, GucSource source)
{
#ifndef USE_PREFETCH
	if (*new_value == RECOVERY_PREFETCH_ON)
	{
		GUC_check_errdetail("recovery_prefetch is not supported on platforms that lack posix_fadvise().");
		return false;
	}
#endif

	return true;
}

void
assign_recovery_prefetch(int new_value, void *extra)
{
	/* 重新配置预取，因其依赖的设置已更改。 */
	recovery_prefetch = new_value;
	if (AmStartupProcess())
		XLogPrefetchReconfigure();
}
