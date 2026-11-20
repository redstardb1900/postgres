/*-------------------------------------------------------------------------
 *
 * bufmgr.h
 *	  POSTGRES 缓冲管理器定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_H
#define BUFMGR_H

#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

typedef void *Block;

/*
 * GetAccessStrategy() 的可能参数。
 *
 * 若新增 BufferAccessStrategyType，也须新增 IOContext，以便
 * 跟踪使用该策略的 IO 统计。
 */
typedef enum BufferAccessStrategyType
{
	BAS_NORMAL,					/* 普通随机访问 */
	BAS_BULKREAD,				/* Large read-only scan (hint bit updates are
								 * ok) */
	BAS_BULKWRITE,				/* 大型多块写（如 COPY IN） */
	BAS_VACUUM					/* VACUUM */
} BufferAccessStrategyType;

/* ReadBufferExtended() 的可能模式 */
typedef enum
{
	RBM_NORMAL,					/* 普通读 */
	RBM_ZERO_AND_LOCK,			/* 不从磁盘读，由调用方
								 * 初始化。并且锁该页。 */
	RBM_ZERO_AND_CLEANUP_LOCK,	/* 类似 RBM_ZERO_AND_LOCK，但锁页于"
								 * cleanup"模式 */
	RBM_ZERO_ON_ERROR,			/* 读取，但出错时返回全零页 */
	RBM_NORMAL_NO_LOG			/* WAL 重放中勿记页为无效；
								 * 其余同 RBM_NORMAL。 */
} ReadBufferMode;

/*
 * PrefetchBuffer() 返回类型。
 */
typedef struct PrefetchBufferResult
{
	Buffer		recent_buffer;	/* 若有效即命中（再检！） */
	bool		initiated_io;	/* 若真则为未命中引发异步 I/O */
} PrefetchBufferResult;

/*
 * 影响 ExtendBufferedRel* 行为的标志
 */
typedef enum ExtendBufferedFlags
{
	/*
	 * 切勿获取扩展锁。仅当关系非
	 * 共享、已持访问排他锁或为启动进程才
	 * 安全
	 */
	EB_SKIP_EXTENSION_LOCK = (1 << 0),

	/* 此扩展是否属于恢复？ */
	EB_PERFORMING_RECOVERY = (1 << 1),

	/*
	 * 若分支当前不存在，是否应创建？这大概
	 * 仅对关系分支有意义。
	 */
	EB_CREATE_FORK_IF_NEEDED = (1 << 2),

	/* 是否应锁返回的首个（可能也是唯一）缓冲区？ */
	EB_LOCK_FIRST = (1 << 3),

	/* 是否应清空 smgr 大小缓存？ */
	EB_CLEAR_SIZE_CACHE = (1 << 4),

	/* 随后为内部标志 */
	EB_LOCK_TARGET = (1 << 5),
}			ExtendBufferedFlags;

/*
 * 有些函数用关系或 smgr +
 * relpersistence 标识关系。经下方 BMR_REL()/BMR_SMGR() 宏。
 * 这样恢复与正常运行可用同一函数。
 */
typedef struct BufferManagerRelation
{
	Relation	rel;
	struct SMgrRelationData *smgr;
	char		relpersistence;
} BufferManagerRelation;

#define BMR_REL(p_rel) ((BufferManagerRelation){.rel = p_rel})
#define BMR_SMGR(p_smgr, p_relpersistence) ((BufferManagerRelation){.smgr = p_smgr, .relpersistence = p_relpersistence})


/* 前置声明，以免在此暴露 buf_internals.h */
struct WritebackContext;

/* 前置声明，以免在此包含 smgr.h */
struct SMgrRelationData;

/* 在 globals.c … 与 miscadmin.h 重复 */
extern PGDLLIMPORT int NBuffers;

/* in bufmgr.c */
extern PGDLLIMPORT bool zero_damaged_pages;
extern PGDLLIMPORT int bgwriter_lru_maxpages;
extern PGDLLIMPORT double bgwriter_lru_multiplier;
extern PGDLLIMPORT bool track_io_timing;

/* 仅当预取可用时适用 */
#ifdef USE_PREFETCH
#define DEFAULT_EFFECTIVE_IO_CONCURRENCY 1
#define DEFAULT_MAINTENANCE_IO_CONCURRENCY 10
#else
#define DEFAULT_EFFECTIVE_IO_CONCURRENCY 0
#define DEFAULT_MAINTENANCE_IO_CONCURRENCY 0
#endif
extern PGDLLIMPORT int effective_io_concurrency;
extern PGDLLIMPORT int maintenance_io_concurrency;

extern PGDLLIMPORT int checkpoint_flush_after;
extern PGDLLIMPORT int backend_flush_after;
extern PGDLLIMPORT int bgwriter_flush_after;

/* in buf_init.c */
extern PGDLLIMPORT char *BufferBlocks;

/* in localbuf.c */
extern PGDLLIMPORT int NLocBuffer;
extern PGDLLIMPORT Block *LocalBufferBlockPointers;
extern PGDLLIMPORT int32 *LocalRefCount;

/* effective_io_concurrency 上限 */
#define MAX_IO_CONCURRENCY 1000

/* ReadBuffer() 的特殊块号 */
#define P_NEW	InvalidBlockNumber	/* 扩展文件以取得新页 */

/*
 * 缓冲区内容锁模式（LockBuffer() 的 mode 参数）
 */
#define BUFFER_LOCK_UNLOCK		0
#define BUFFER_LOCK_SHARE		1
#define BUFFER_LOCK_EXCLUSIVE	2


/*
 * bufmgr.c 中函数的原型
 */
extern PrefetchBufferResult PrefetchSharedBuffer(struct SMgrRelationData *smgr_reln,
												 ForkNumber forkNum,
												 BlockNumber blockNum);
extern PrefetchBufferResult PrefetchBuffer(Relation reln, ForkNumber forkNum,
										   BlockNumber blockNum);
extern bool ReadRecentBuffer(RelFileLocator rlocator, ForkNumber forkNum,
							 BlockNumber blockNum, Buffer recent_buffer);
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);
extern Buffer ReadBufferExtended(Relation reln, ForkNumber forkNum,
								 BlockNumber blockNum, ReadBufferMode mode,
								 BufferAccessStrategy strategy);
extern Buffer ReadBufferWithoutRelcache(RelFileLocator rlocator,
										ForkNumber forkNum, BlockNumber blockNum,
										ReadBufferMode mode, BufferAccessStrategy strategy,
										bool permanent);
extern void ReleaseBuffer(Buffer buffer);
extern void UnlockReleaseBuffer(Buffer buffer);
extern void MarkBufferDirty(Buffer buffer);
extern void IncrBufferRefCount(Buffer buffer);
extern void CheckBufferIsPinnedOnce(Buffer buffer);
extern Buffer ReleaseAndReadBuffer(Buffer buffer, Relation relation,
								   BlockNumber blockNum);

extern Buffer ExtendBufferedRel(BufferManagerRelation bmr,
								ForkNumber forkNum,
								BufferAccessStrategy strategy,
								uint32 flags);
extern BlockNumber ExtendBufferedRelBy(BufferManagerRelation bmr,
									   ForkNumber fork,
									   BufferAccessStrategy strategy,
									   uint32 flags,
									   uint32 extend_by,
									   Buffer *buffers,
									   uint32 *extended_by);
extern Buffer ExtendBufferedRelTo(BufferManagerRelation bmr,
								  ForkNumber fork,
								  BufferAccessStrategy strategy,
								  uint32 flags,
								  BlockNumber extend_to,
								  ReadBufferMode mode);

extern void InitBufferPoolAccess(void);
extern void AtEOXact_Buffers(bool isCommit);
extern void PrintBufferLeakWarning(Buffer buffer);
extern void CheckPointBuffers(int flags);
extern BlockNumber BufferGetBlockNumber(Buffer buffer);
extern BlockNumber RelationGetNumberOfBlocksInFork(Relation relation,
												   ForkNumber forkNum);
extern void FlushOneBuffer(Buffer buffer);
extern void FlushRelationBuffers(Relation rel);
extern void FlushRelationsAllBuffers(struct SMgrRelationData **smgrs, int nrels);
extern void CreateAndCopyRelationData(RelFileLocator src_rlocator,
									  RelFileLocator dst_rlocator,
									  bool permanent);
extern void FlushDatabaseBuffers(Oid dbid);
extern void DropRelationBuffers(struct SMgrRelationData *smgr_reln,
								ForkNumber *forkNum,
								int nforks, BlockNumber *firstDelBlock);
extern void DropRelationsAllBuffers(struct SMgrRelationData **smgr_reln,
									int nlocators);
extern void DropDatabaseBuffers(Oid dbid);

#define RelationGetNumberOfBlocks(reln) \
	RelationGetNumberOfBlocksInFork(reln, MAIN_FORKNUM)

extern bool BufferIsPermanent(Buffer buffer);
extern XLogRecPtr BufferGetLSNAtomic(Buffer buffer);

#ifdef NOT_USED
extern void PrintPinnedBufs(void);
#endif
extern void BufferGetTag(Buffer buffer, RelFileLocator *rlocator,
						 ForkNumber *forknum, BlockNumber *blknum);

extern void MarkBufferDirtyHint(Buffer buffer, bool buffer_std);

extern void UnlockBuffers(void);
extern void LockBuffer(Buffer buffer, int mode);
extern bool ConditionalLockBuffer(Buffer buffer);
extern void LockBufferForCleanup(Buffer buffer);
extern bool ConditionalLockBufferForCleanup(Buffer buffer);
extern bool IsBufferCleanupOK(Buffer buffer);
extern bool HoldingBufferPinThatDelaysRecovery(void);

extern void AbortBufferIO(Buffer buffer);

extern bool BgBufferSync(struct WritebackContext *wb_context);

extern void TestForOldSnapshot_impl(Snapshot snapshot, Relation relation);

/* in buf_init.c */
extern void InitBufferPool(void);
extern Size BufferShmemSize(void);

/* in localbuf.c */
extern void AtProcExit_LocalBuffers(void);

/* in freelist.c */

extern BufferAccessStrategy GetAccessStrategy(BufferAccessStrategyType btype);
extern BufferAccessStrategy GetAccessStrategyWithSize(BufferAccessStrategyType btype,
													  int ring_size_kb);
extern int	GetAccessStrategyBufferCount(BufferAccessStrategy strategy);

extern void FreeAccessStrategy(BufferAccessStrategy strategy);


/* 内联函数 */

/*
 * 本头文件名义上仅后端用，但某些前端
 * 程序如 pg_waldump 会包含它。对即使未用也发出静态
 * 内联函数的编译器，会导致未满足的
 * 外部引用；故用 #ifndef FRONTEND 隐藏。
 */

#ifndef FRONTEND

/*
 * BufferIsValid
 *		当且仅当给定缓冲区号有效（共享或本地
 *		缓冲区）。
 *
 * 注意：长久以来本宏定义同 BufferIsPinned，
 * 即未钉住该缓冲区时会说 False。
 * 我认为这是错的，只会掩盖逻辑错误。
 * 代码应始终知道自己是否持有缓冲区引用，
 * 与钉住状态无关。
 *
 * 注意：又有很长时间它并非
 * BufferIsInvalid() 宏的反义，因其还做健全性检查以
 * 核实缓冲区号在范围内。本宏当初多半
 * 只打算用于断言，但其用途后来
 * 扩大许多，即便在未启用断言的构建中做这些检查
 * 开销也可观。因此我们现已把范围检查
 * 降为宏内部的断言。
 */
static inline bool
BufferIsValid(Buffer bufnum)
{
	Assert(bufnum <= NBuffers);
	Assert(bufnum >= -NLocBuffer);

	return bufnum != InvalidBuffer;
}

/*
 * BufferGetBlock
 *		返回与缓冲区关联的磁盘页映像引用。
 *
 * Note:
 *		假定缓冲区有效。
 */
static inline Block
BufferGetBlock(Buffer buffer)
{
	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
		return LocalBufferBlockPointers[-buffer - 1];
	else
		return (Block) (BufferBlocks + ((Size) (buffer - 1)) * BLCKSZ);
}

/*
 * BufferGetPageSize
 *		返回缓冲区内的页大小。
 *
 * Notes:
 *		假定缓冲区有效。
 *
 *		缓冲区可为原始磁盘块，不必含有效（格式化）
 *		磁盘页。
 */
/* XXX 应取自缓冲区描述符 */
static inline Size
BufferGetPageSize(Buffer buffer)
{
	AssertMacro(BufferIsValid(buffer));
	return (Size) BLCKSZ;
}

/*
 * BufferGetPage
 *		返回与缓冲区关联的页。
 *
 * 作为扫描的一部分调用时，附近可能需要
 * 调用 TestForOldSnapshot()。详见其定义。
 */
static inline Page
BufferGetPage(Buffer buffer)
{
	return (Page) BufferGetBlock(buffer);
}

/*
 * 检查给定快照是否过旧，以致无法安全读取给定表上
 * 的给定页。若是，则抛出"快照过旧"错误。
 *
 * 此测试通常须在作为扫描一部分执行的每次
 * BufferGetPage() 调用之后进行。为修改页而做的调用
 * 则不需要（例如定位到正确位置以插入新
 * 索引元组，或做清理）。若下层函数已做过该测试，
 * 也可省略。
 *
 * 注意：允许 snapshot 为 NULL，并会快速返回
 * 且不报错；这是为支持既可能从扫描、也可能从
 * 索引修改区域调用的站点。
 *
 * 为求最佳性能，把最快和/或最可能让页免于
 * 旧快照测试的检查放在前面。
 */
static inline void
TestForOldSnapshot(Snapshot snapshot, Relation relation, Page page)
{
	Assert(relation != NULL);

	if (old_snapshot_threshold >= 0
		&& (snapshot) != NULL
		&& ((snapshot)->snapshot_type == SNAPSHOT_MVCC
			|| (snapshot)->snapshot_type == SNAPSHOT_TOAST)
		&& !XLogRecPtrIsInvalid((snapshot)->lsn)
		&& PageGetLSN(page) > (snapshot)->lsn)
		TestForOldSnapshot_impl(snapshot, relation);
}

#endif							/* FRONTEND */

#endif							/* BUFMGR_H */
