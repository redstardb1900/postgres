/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  缓冲管理器及缓冲区替换策略的内部
 *	  定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/relcache.h"

/*
 * 缓冲区状态是单个 32 位变量，其中合并了下列数据。
 *
 * 18 位 refcount
 * 4 位使用计数
 * 10 位标志
 *
 * 合在一起即可不锁缓冲区头而做某些操作，
 * 通过 CAS 循环一起修改它们。
 *
 * 缓冲区状态各成分的定义见下。
 */
#define BUF_REFCOUNT_ONE 1
#define BUF_REFCOUNT_MASK ((1U << 18) - 1)
#define BUF_USAGECOUNT_MASK 0x003C0000U
#define BUF_USAGECOUNT_ONE (1U << 18)
#define BUF_USAGECOUNT_SHIFT 18
#define BUF_FLAG_MASK 0xFFC00000U

/* 从缓冲区状态取 refcount 与 usagecount */
#define BUF_STATE_GET_REFCOUNT(state) ((state) & BUF_REFCOUNT_MASK)
#define BUF_STATE_GET_USAGECOUNT(state) (((state) & BUF_USAGECOUNT_MASK) >> BUF_USAGECOUNT_SHIFT)

/*
 * 缓冲区描述符的标志
 *
 * 注意：BM_TAG_VALID 实质上表示有缓冲区哈希表
 * 条目与该缓冲区 tag 相关联。
 */
#define BM_LOCKED				(1U << 22)	/* 缓冲区头已锁 */
#define BM_DIRTY				(1U << 23)	/* 数据需要写出 */
#define BM_VALID				(1U << 24)	/* 数据有效 */
#define BM_TAG_VALID			(1U << 25)	/* tag 已赋值 */
#define BM_IO_IN_PROGRESS		(1U << 26)	/* 读或写进行中 */
#define BM_IO_ERROR				(1U << 27)	/* 先前 I/O 失败 */
#define BM_JUST_DIRTIED			(1U << 28)	/* 自写出开始后已弄脏 */
#define BM_PIN_COUNT_WAITER		(1U << 29)	/* 有等待独占钉住者 */
#define BM_CHECKPOINT_NEEDED	(1U << 30)	/* 检查点必须写出 */
#define BM_PERMANENT			(1U << 31)	/* 永久缓冲（非未日志,
											 * 非init fork) */
/*
 * usage_count 允许的最大值体现时钟扫描算法在
 * 精度与速度间的折中。大值（与 NBuffers 相当）
 * 会近似 LRU 语义。但找到空闲缓冲区最多可能要
 * BM_MAX_USAGE_COUNT+1 轮完整时钟扫描，故实践中不希望
 * 该值
 * 很大。
 */
#define BM_MAX_USAGE_COUNT	5

/*
 * 缓冲区标签标识缓冲区含哪个磁盘块。
 *
 * 注意：BufferTag 数据必须足以确定把块写到何处，无需
 * 参照 pg_class 或 pg_tablespace 条目。
 * 冲刷该缓冲的后端甚至可能还不认为该
 * 关系可见（其 xact 可能开始于创建该 rel 的
 * 事务之前）。存储管理器仍须能应付。
 *
 * 注意：若结构有填充字节，InitBufferTag 须修正以
 * 将其清零，因本结构用作哈希键。
 */
typedef struct buftag
{
	Oid			spcOid;			/* 表空间 oid */
	Oid			dbOid;			/* 数据库oid */
	RelFileNumber relNumber;	/* 关系文件号 */
	ForkNumber	forkNum;		/* 分支号 */
	BlockNumber blockNum;		/* 相对关系起始的块号 */
} BufferTag;

static inline RelFileNumber
BufTagGetRelNumber(const BufferTag *tag)
{
	return tag->relNumber;
}

static inline ForkNumber
BufTagGetForkNum(const BufferTag *tag)
{
	return tag->forkNum;
}

static inline void
BufTagSetRelForkDetails(BufferTag *tag, RelFileNumber relnumber,
						ForkNumber forknum)
{
	tag->relNumber = relnumber;
	tag->forkNum = forknum;
}

static inline RelFileLocator
BufTagGetRelFileLocator(const BufferTag *tag)
{
	RelFileLocator rlocator;

	rlocator.spcOid = tag->spcOid;
	rlocator.dbOid = tag->dbOid;
	rlocator.relNumber = BufTagGetRelNumber(tag);

	return rlocator;
}

static inline void
ClearBufferTag(BufferTag *tag)
{
	tag->spcOid = InvalidOid;
	tag->dbOid = InvalidOid;
	BufTagSetRelForkDetails(tag, InvalidRelFileNumber, InvalidForkNumber);
	tag->blockNum = InvalidBlockNumber;
}

static inline void
InitBufferTag(BufferTag *tag, const RelFileLocator *rlocator,
			  ForkNumber forkNum, BlockNumber blockNum)
{
	tag->spcOid = rlocator->spcOid;
	tag->dbOid = rlocator->dbOid;
	BufTagSetRelForkDetails(tag, rlocator->relNumber, forkNum);
	tag->blockNum = blockNum;
}

static inline bool
BufferTagsEqual(const BufferTag *tag1, const BufferTag *tag2)
{
	return (tag1->spcOid == tag2->spcOid) &&
		(tag1->dbOid == tag2->dbOid) &&
		(tag1->relNumber == tag2->relNumber) &&
		(tag1->blockNum == tag2->blockNum) &&
		(tag1->forkNum == tag2->forkNum);
}

static inline bool
BufTagMatchesRelFileLocator(const BufferTag *tag,
							const RelFileLocator *rlocator)
{
	return (tag->spcOid == rlocator->spcOid) &&
		(tag->dbOid == rlocator->dbOid) &&
		(BufTagGetRelNumber(tag) == rlocator->relNumber);
}


/*
 * 共享缓冲区映射表分区以减少争用。
 * 要确定给定 tag 需哪把分区锁，用 BufTableHashCode() 计算
 * tag 的哈希码，再应用 BufMappingPartitionLock()。
 * NUM_BUFFER_PARTITIONS 须为 2 的幂！
 */
static inline uint32
BufTableHashPartition(uint32 hashcode)
{
	return hashcode % NUM_BUFFER_PARTITIONS;
}

static inline LWLock *
BufMappingPartitionLock(uint32 hashcode)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET +
							BufTableHashPartition(hashcode)].lock;
}

static inline LWLock *
BufMappingPartitionLockByIndex(uint32 index)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET + index].lock;
}

/*
 *		BufferDesc -- 单个共享缓冲区的共享描述符/状态数据。
 *
 * 注意：须持缓冲区头锁（BM_LOCKED 标志）才能检查或更改
 * tag、state 或 wait_backend_pgprocno 字段。缓冲区头锁
 * 是与 flags、refcount 和 usagecount 合入
 * 单个原子变量的自旋锁。此布局允许我们在一次
 * 原子操作中做某些操作，而无需实际获取和释放自旋锁；
 * 例如增减 refcount。buf_id 字段在初始化后永不
 * 改变，故无需加锁。freeNext 由
 * buffer_strategy_lock 而非缓冲区头锁保护。LWLock 能自己
 * 照顾自己。缓冲区头锁*不*用于控制对缓冲区内
 * 数据的访问！
 *
 * 假定持有缓冲区头锁时无人更改 state 字段。
 * 因此缓冲区头锁持有者可单次写入即对 state 变量
 * 做复杂更新，同时释放锁（清除
 * BM_LOCKED 标志）。另一方面，不持有
 * 缓冲区头锁时更新 state 仅限于 CAS，以确保 BM_LOCKED 标志
 * 未置位。不允许原子增减、OR/AND 等。
 *
 * 例外是若我们已钉住该缓冲区，其 tag 不会在
 * 眼皮下改变，故可不锁缓冲区头而检查 tag。
 * 另外有些地方我们做一次标志读取而不去
 * 锁缓冲区头；这一般是因为我们不
 * 预期被测标志位会变化。
 *
 * 若另一后端已钉住该缓冲区，我们无法从磁盘页
 * 物理移除项。因此后端可能需要等待所有其他钉住
 * 消失。这通过把它自己的 pgprocno 存入
 * wait_backend_pgprocno 并设置标志位 BM_PIN_COUNT_WAITER。目前
 * 每缓冲区仅能有一等待者。
 *
 * 本地缓冲区头也用同一结构，但锁不
 * 使用，且并非所有标志位都有用。为避免不必要
 * 开销，对 state 字段的操作应不使用真正
 * 原子操作（即仅 pg_atomic_read_u32() 和
 * pg_atomic_unlocked_write_u32()).
 *
 * 添加或重排成员时当心勿增大结构
 * 大小。保持在 64 字节以下（最常见的 CPU
 * 缓存行大小）对性能相当重要。
 *
 * 每缓冲区 I/O 条件变量目前放在本结构外的
 * 单独数组中。它们可以移入此处且在常见系统上仍
 * 符合该限制，但目前未这样做。
 */
typedef struct BufferDesc
{
	BufferTag	tag;			/* 缓冲区内所含页的 ID */
	int			buf_id;			/* 缓冲区下标（从 0 起） */

	/* tag 的状态，含 flags、refcount 和 usagecount */
	pg_atomic_uint32 state;

	int			wait_backend_pgprocno;	/* 钉住等待者后端 */
	int			freeNext;		/* 空闲链链接 */
	LWLock		content_lock;	/* 用以锁访问缓冲区内容 */
} BufferDesc;

/*
 * 并发访问缓冲区头时，若按缓存行对齐则更高效。
 * 故我们强制 BufferDescriptors 数组起点位于缓存行
 * 边界，并强制元素为缓存
 * 行大小
 *
 * XXX: 这主要影响高度并发负载，如今大概都是
 * 64 位；32 位上浪费更明显，故不强制
 * 其步长为缓存行大小。若有人实测，我们可
 * 再评
 * 估
 *
 * 注意本地缓冲区描述符不强制对齐——因为对其没有
 * 并发访问，不太可能有益。
 *
 * 这里用 64 字节缓存行大小，因为这是最常见的
 * 大小。再大会浪费内存。即使运行在
 * 32 或 128 字节行大小的平台上，对齐到
 * 边界并免伪共享。
 */
#define BUFFERDESC_PAD_TO_SIZE	(SIZEOF_VOID_P == 8 ? 64 : 1)

typedef union BufferDescPadded
{
	BufferDesc	bufferdesc;
	char		pad[BUFFERDESC_PAD_TO_SIZE];
} BufferDescPadded;

/*
 * PendingWriteback 与 WritebackContext 结构用于保存
 * 待向 OS 发出的未完成刷写请求的信息。
 */
typedef struct PendingWriteback
{
	/* 此处可存放不同类型的未完成刷写 */
	BufferTag	tag;
} PendingWriteback;

/* bufmgr.h 中前置声明 */
typedef struct WritebackContext
{
	/* 指向要合并的回写请求最大数目 */
	int		   *max_pending;

	/* 当前未完成回写请求数目 */
	int			nr_pending;

	/* 未完成请求 */
	PendingWriteback pending_writebacks[WRITEBACK_MAX_PENDING_FLUSHES];
} WritebackContext;

/* in buf_init.c */
extern PGDLLIMPORT BufferDescPadded *BufferDescriptors;
extern PGDLLIMPORT ConditionVariableMinimallyPadded *BufferIOCVArray;
extern PGDLLIMPORT WritebackContext BackendWritebackContext;

/* in localbuf.c */
extern PGDLLIMPORT BufferDesc *LocalBufferDescriptors;


static inline BufferDesc *
GetBufferDescriptor(uint32 id)
{
	return &(BufferDescriptors[id]).bufferdesc;
}

static inline BufferDesc *
GetLocalBufferDescriptor(uint32 id)
{
	return &LocalBufferDescriptors[id];
}

static inline Buffer
BufferDescriptorGetBuffer(const BufferDesc *bdesc)
{
	return (Buffer) (bdesc->buf_id + 1);
}

static inline ConditionVariable *
BufferDescriptorGetIOCV(const BufferDesc *bdesc)
{
	return &(BufferIOCVArray[bdesc->buf_id]).cv;
}

static inline LWLock *
BufferDescriptorGetContentLock(const BufferDesc *bdesc)
{
	return (LWLock *) (&bdesc->content_lock);
}

/*
 * freeNext 字段要么是下一空闲链表条目的下标，
 * 或为下列特殊值：
 */
#define FREENEXT_END_OF_LIST	(-1)
#define FREENEXT_NOT_IN_LIST	(-2)

/*
 * 获取/释放共享缓冲区头自旋锁的函数。切勿
 * 用于本地缓冲区！
 */
extern uint32 LockBufHdr(BufferDesc *desc);

static inline void
UnlockBufHdr(BufferDesc *desc, uint32 buf_state)
{
	pg_write_barrier();
	pg_atomic_write_u32(&desc->state, buf_state & (~BM_LOCKED));
}

/* in bufmgr.c */

/*
 * 检查点时按文件排序缓冲区的结构。
 *
 * 本结构在共享内存中按缓冲区分配，故应尽可能
 * 保持小。
 */
typedef struct CkptSortItem
{
	Oid			tsId;
	RelFileNumber relNumber;
	ForkNumber	forkNum;
	BlockNumber blockNum;
	int			buf_id;
} CkptSortItem;

extern PGDLLIMPORT CkptSortItem *CkptBufferIds;

/*
 * 内部缓冲管理例程
 */
/* bufmgr.c */
extern void WritebackContextInit(WritebackContext *context, int *max_pending);
extern void IssuePendingWritebacks(WritebackContext *wb_context, IOContext io_context);
extern void ScheduleBufferTagForWriteback(WritebackContext *wb_context,
										  IOContext io_context, BufferTag *tag);

/* freelist.c */
extern IOContext IOContextForStrategy(BufferAccessStrategy strategy);
extern BufferDesc *StrategyGetBuffer(BufferAccessStrategy strategy,
									 uint32 *buf_state, bool *from_ring);
extern void StrategyFreeBuffer(BufferDesc *buf);
extern bool StrategyRejectBuffer(BufferAccessStrategy strategy,
								 BufferDesc *buf, bool from_ring);

extern int	StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc);
extern void StrategyNotifyBgWriter(int bgwprocno);

extern Size StrategyShmemSize(void);
extern void StrategyInitialize(bool init);
extern bool have_free_buffer(void);

/* buf_table.c */
extern Size BufTableShmemSize(int size);
extern void InitBufTable(int size);
extern uint32 BufTableHashCode(BufferTag *tagPtr);
extern int	BufTableLookup(BufferTag *tagPtr, uint32 hashcode);
extern int	BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id);
extern void BufTableDelete(BufferTag *tagPtr, uint32 hashcode);

/* localbuf.c */
extern bool PinLocalBuffer(BufferDesc *buf_hdr, bool adjust_usagecount);
extern void UnpinLocalBuffer(Buffer buffer);
extern PrefetchBufferResult PrefetchLocalBuffer(SMgrRelation smgr,
												ForkNumber forkNum,
												BlockNumber blockNum);
extern BufferDesc *LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum,
									BlockNumber blockNum, bool *foundPtr);
extern BlockNumber ExtendBufferedRelLocal(BufferManagerRelation bmr,
										  ForkNumber fork,
										  uint32 flags,
										  uint32 extend_by,
										  BlockNumber extend_upto,
										  Buffer *buffers,
										  uint32 *extended_by);
extern void MarkLocalBufferDirty(Buffer buffer);
extern void DropRelationLocalBuffers(RelFileLocator rlocator,
									 ForkNumber forkNum,
									 BlockNumber firstDelBlock);
extern void DropRelationAllLocalBuffers(RelFileLocator rlocator);
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif							/* BUFMGR_INTERNALS_H */
