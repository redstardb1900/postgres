/*-------------------------------------------------------------------------
 *
 * bufmgr.c
 *	  buffer manager interface routines
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/bufmgr.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 主要入口：
 *
 * ReadBuffer() 查找或创建持有所请求页的缓冲区，
 *		并钉住它，使本进程使用期间
 *		没有人能销毁它。
 *
 * ReleaseBuffer() 解除对缓冲区的钉住。
 *
 * MarkBufferDirty() 把已钉住缓冲区的内容标为脏页。
 *		磁盘写被推迟到缓冲区替换或检查点。
 *
 * 另见这些文件：
 *		freelist.c 为缓冲区替换选择牺牲者
 *		buf_table.c 管理缓冲区查找表
 */
#include "postgres.h"

#include <sys/file.h>
#include <unistd.h>

#include "access/tableam.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "executor/instrument.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "storage/standby.h"
#include "utils/memdebug.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/resowner_private.h"
#include "utils/timestamp.h"


/* 注意：这两个宏只对共享缓冲区有效，对本地缓冲区无效！ */
#define BufHdrGetBlock(bufHdr)	((Block) (BufferBlocks + ((Size) (bufHdr)->buf_id) * BLCKSZ))
#define BufferGetLSN(bufHdr)	(PageGetLSN(BufHdrGetBlock(bufHdr)))

/* 注意：这个宏只对本地缓冲区有效，对共享缓冲区无效！ */
#define LocalBufHdrGetBlock(bufHdr) \
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

/* SyncOneBuffer 返回值里的各个位 */
#define BUF_WRITTEN				0x01
#define BUF_REUSABLE			0x02

#define RELS_BSEARCH_THRESHOLD		20

/*
 * 关系大小（以块数计）超过这个阈值时，我们扫描
 * 整个缓冲池，以去掉被删除关系全部页的缓冲区。
 * 对低于该阈值的关系，我们通过在 BufMapping 表里
 * 查找来找到那些缓冲区。
 */
#define BUF_DROP_FULL_SCAN_THRESHOLD		(uint64) (NBuffers / 32)

typedef struct PrivateRefCountEntry
{
	Buffer		buffer;
	int32		refcount;
} PrivateRefCountEntry;

/* 64 字节，大约是常见系统上一条缓存行的大小 */
#define REFCOUNT_ARRAY_ENTRIES 8

/*
 * 某个表空间里待检查点缓冲区的状态，供
 * BufferSync 内部使用。
 */
typedef struct CkptTsStatus
{
	/* 表空间的 oid */
	Oid			tsId;

	/*
	 * 该表空间的检查点进度。为使各表空间之间的进度可比，
	 * 对每个表空间，进度都用一个介于 0 和
	 * 待检查点页总数之间的数来衡量。在该表空间里
	 * 每检查点一页，就把该空间的进度
	 * 增加 progress_slice。
	 */
	float8		progress;
	float8		progress_slice;

	/* 该表空间里待检查点的页数 */
	int			num_to_scan;
	/* 该表空间里已经处理的页数 */
	int			num_scanned;

	/* 该表空间在 CkptBufferIds 里的当前偏移 */
	int			index;
} CkptTsStatus;

/*
 * 用于排序 SMgrRelations 的数组的类型。
 *
 * FlushRelationsAllBuffers 与 DropRelationsAllBuffers 共用同一个比较函数。
 * 指向该结构的指针与 RelFileLocator 必须
 * 兼容。
 */
typedef struct SMgrSortArray
{
	RelFileLocator rlocator;	/* 这必须是第一个成员 */
	SMgrRelation srel;
} SMgrSortArray;

/* GUC 变量 */
bool		zero_damaged_pages = false;
int			bgwriter_lru_maxpages = 100;
double		bgwriter_lru_multiplier = 2.0;
bool		track_io_timing = false;

/*
 * PrefetchBuffer 的调用者应当试图领先其
 * ReadBuffer 调用多少个缓冲区。零表示从不预取。该值只用于
 * 不属于已设置 effective_io_concurrency 参数的表空间的
 * 缓冲区。
 */
int			effective_io_concurrency = DEFAULT_EFFECTIVE_IO_CONCURRENCY;

/*
 * 与 effective_io_concurrency 类似，但用于可能
 * 因代表许多会话工作而受益于更高设置的维护代码路径。
 * 会被同名的表空间设置覆盖。
 */
int			maintenance_io_concurrency = DEFAULT_MAINTENANCE_IO_CONCURRENCY;

/*
 * 关于触发已写缓冲区的内核回写的 GUC 变量；与操作系统
 * 相关的默认值通过 GUC 机制设置。
 */
int			checkpoint_flush_after = DEFAULT_CHECKPOINT_FLUSH_AFTER;
int			bgwriter_flush_after = DEFAULT_BGWRITER_FLUSH_AFTER;
int			backend_flush_after = DEFAULT_BACKEND_FLUSH_AFTER;

/* LockBufferForCleanup 的本地状态 */
static BufferDesc *PinCountWaitBuf = NULL;

/*
 * 后端私有的引用计数管理：
 *
 * 每个缓冲区还有一个私有引用计数，记录
 * 当前进程钉住该缓冲区的次数。这样，若单个后端
 * 多次钉住一个缓冲区，共享引用计数只需要修改
 * 一次。它也用来检查事务末尾以及退出时
 * 没有缓冲区仍被钉住。
 *
 *
 * 为避免像过去那样需要一个有 NBuffers 项的数组来跟踪
 * 本地缓冲区，我们用一个小型顺序查找数组
 * （PrivateRefCountArray）和一个溢出散列表（PrivateRefCountHash）来
 * 跟踪后端本地的钉住。
 *
 * 在同时钉住的缓冲区不超过 REFCOUNT_ARRAY_ENTRIES 之前，全部
 * 引用计数都记在数组里；此后，新的数组项
 * 会把旧项挤进散列表。这样，常用项
 * 不会卡在散列表里，而不常用项堵住数组。
 *
 * 注意，在多数情形下，被钉住的缓冲区数量不会超过
 * REFCOUNT_ARRAY_ENTRIES。
 *
 *
 * 要把一个缓冲区纳入引用计数跟踪，先用
 * ReservePrivateRefCountEntry() 保留一个空闲项，然后在必要时
 * 用 NewPrivateRefCountEntry() 填入。这种拆分让我们避免在
 * NewPrivateRefCountEntry() 里做内存分配，这点可能很重要，
 * 因为有些情形下调用它时持有自旋锁……
 */
static struct PrivateRefCountEntry PrivateRefCountArray[REFCOUNT_ARRAY_ENTRIES];
static HTAB *PrivateRefCountHash = NULL;
static int32 PrivateRefCountOverflowed = 0;
static uint32 PrivateRefCountClock = 0;
static PrivateRefCountEntry *ReservedRefCountEntry = NULL;

static void ReservePrivateRefCountEntry(void);
static PrivateRefCountEntry *NewPrivateRefCountEntry(Buffer buffer);
static PrivateRefCountEntry *GetPrivateRefCountEntry(Buffer buffer, bool do_move);
static inline int32 GetPrivateRefCount(Buffer buffer);
static void ForgetPrivateRefCountEntry(PrivateRefCountEntry *ref);

/*
 * 确保 PrivateRefCountArray 有足够空间再存一项。
 * 这必须在用 NewPrivateRefCountEntry() 填入
 * 新项之前调用，但不用掉已保留的项也完全没问题。
 */
static void
ReservePrivateRefCountEntry(void)
{
	/* 已经保留（或已释放），无事可做 */
	if (ReservedRefCountEntry != NULL)
		return;

	/*
	 * 先在数组里找一个空闲项，多数情形下
	 * 这就够了。
	 */
	{
		int			i;

		for (i = 0; i < REFCOUNT_ARRAY_ENTRIES; i++)
		{
			PrivateRefCountEntry *res;

			res = &PrivateRefCountArray[i];

			if (res->buffer == InvalidBuffer)
			{
				ReservedRefCountEntry = res;
				return;
			}
		}
	}

	/*
	 * 没找到。数组项都满了。把一个数组项移进散
	 * 列表。
	 */
	{
		/*
		 * 把数组里当前时钟位置的项移进
		 * 散列表。使用那个槽。
		 */
		PrivateRefCountEntry *hashent;
		bool		found;

		/* 选择牺牲槽 */
		ReservedRefCountEntry =
			&PrivateRefCountArray[PrivateRefCountClock++ % REFCOUNT_ARRAY_ENTRIES];

		/* 最好是在用的，否则我们不该到这里。 */
		Assert(ReservedRefCountEntry->buffer != InvalidBuffer);

		/* 把牺牲数组项放进散列表 */
		hashent = hash_search(PrivateRefCountHash,
							  &(ReservedRefCountEntry->buffer),
							  HASH_ENTER,
							  &found);
		Assert(!found);
		hashent->refcount = ReservedRefCountEntry->refcount;

		/* 清空现在空闲的数组槽 */
		ReservedRefCountEntry->buffer = InvalidBuffer;
		ReservedRefCountEntry->refcount = 0;

		PrivateRefCountOverflowed++;
	}
}

/*
 * 填入先前保留的引用计数项。
 */
static PrivateRefCountEntry *
NewPrivateRefCountEntry(Buffer buffer)
{
	PrivateRefCountEntry *res;

	/* 只允许在已经做过保留之后调用 */
	Assert(ReservedRefCountEntry != NULL);

	/* 用掉已保留的项 */
	res = ReservedRefCountEntry;
	ReservedRefCountEntry = NULL;

	/* 并填入它 */
	res->buffer = buffer;
	res->refcount = 0;

	return res;
}

/*
 * 返回传入缓冲区的 PrivateRefCount 项。
 *
 * 若缓冲区没有引用计数项，就返回 NULL。否则，若
 * do_move 为真，且该项位于散列表中，就把该项移到数组里，
 * 以便频繁访问。
 */
static PrivateRefCountEntry *
GetPrivateRefCountEntry(Buffer buffer, bool do_move)
{
	PrivateRefCountEntry *res;
	int			i;

	Assert(BufferIsValid(buffer));
	Assert(!BufferIsLocal(buffer));

	/*
	 * 先在数组里查找引用，多数情形下
	 * 这就够了。
	 */
	for (i = 0; i < REFCOUNT_ARRAY_ENTRIES; i++)
	{
		res = &PrivateRefCountArray[i];

		if (res->buffer == buffer)
			return res;
	}

	/*
	 * 到这里我们知道，该缓冲区若已被钉住，并不位于
	 * 数组中。
	 *
	 * 只有在我们先前已经溢出到散列表时，
	 * 才在散列表里查找该缓冲区。
	 */
	if (PrivateRefCountOverflowed == 0)
		return NULL;

	res = hash_search(PrivateRefCountHash, &buffer, HASH_FIND, NULL);

	if (res == NULL)
		return NULL;
	else if (!do_move)
	{
		/* 调用者不希望我们把散列项移进数组 */
		return res;
	}
	else
	{
		/* 把缓冲区从散列表移到空闲数组槽 */
		bool		found;
		PrivateRefCountEntry *free;

		/* 确保有一个空闲数组槽 */
		ReservePrivateRefCountEntry();

		/* 用掉已保留的槽 */
		Assert(ReservedRefCountEntry != NULL);
		free = ReservedRefCountEntry;
		ReservedRefCountEntry = NULL;
		Assert(free->buffer == InvalidBuffer);

		/* 并填入它 */
		free->buffer = buffer;
		free->refcount = res->refcount;

		/* 从散列表中删除 */
		hash_search(PrivateRefCountHash, &buffer, HASH_REMOVE, &found);
		Assert(found);
		Assert(PrivateRefCountOverflowed > 0);
		PrivateRefCountOverflowed--;

		return free;
	}
}

/*
 * 返回本后端钉住传入缓冲区的次数。
 *
 * 只对共享内存缓冲区有效！
 */
static inline int32
GetPrivateRefCount(Buffer buffer)
{
	PrivateRefCountEntry *ref;

	Assert(BufferIsValid(buffer));
	Assert(!BufferIsLocal(buffer));

	/*
	 * 不移动该项。对当前使用者没问题，但我们也许
	 * 有一天会改掉这一点。
	 */
	ref = GetPrivateRefCountEntry(buffer, false);

	if (ref == NULL)
		return 0;
	return ref->refcount;
}

/*
 * 释放用来跟踪某个缓冲区引用计数的资源，我们不再
 * 钉住它，也不想立刻再次钉住。
 */
static void
ForgetPrivateRefCountEntry(PrivateRefCountEntry *ref)
{
	Assert(ref->refcount == 0);

	if (ref >= &PrivateRefCountArray[0] &&
		ref < &PrivateRefCountArray[REFCOUNT_ARRAY_ENTRIES])
	{
		ref->buffer = InvalidBuffer;

		/*
		 * 把刚用过的项标为已保留。在许多情形下，这
		 * 让我们根本不必在数组或散列表里搜索空闲
		 * 项。
		 */
		ReservedRefCountEntry = ref;
	}
	else
	{
		bool		found;
		Buffer		buffer = ref->buffer;

		hash_search(PrivateRefCountHash, &buffer, HASH_REMOVE, &found);
		Assert(found);
		Assert(PrivateRefCountOverflowed > 0);
		PrivateRefCountOverflowed--;
	}
}

/*
 * BufferIsPinned
 *		当且仅当该缓冲区被钉住时为真（也检查缓冲区号是否有效）。
 *
 *		注意：这里检查的是本后端持有对该
 *		缓冲区的钉住。我们不关心其他后端是否持有。
 */
#define BufferIsPinned(bufnum) \
( \
	!BufferIsValid(bufnum) ? \
		false \
	: \
		BufferIsLocal(bufnum) ? \
			(LocalRefCount[-(bufnum) - 1] > 0) \
		: \
	(GetPrivateRefCount(bufnum) > 0) \
)


static Buffer ReadBuffer_common(SMgrRelation smgr, char relpersistence,
								ForkNumber forkNum, BlockNumber blockNum,
								ReadBufferMode mode, BufferAccessStrategy strategy,
								bool *hit);
static BlockNumber ExtendBufferedRelCommon(BufferManagerRelation bmr,
										   ForkNumber fork,
										   BufferAccessStrategy strategy,
										   uint32 flags,
										   uint32 extend_by,
										   BlockNumber extend_upto,
										   Buffer *buffers,
										   uint32 *extended_by);
static BlockNumber ExtendBufferedRelShared(BufferManagerRelation bmr,
										   ForkNumber fork,
										   BufferAccessStrategy strategy,
										   uint32 flags,
										   uint32 extend_by,
										   BlockNumber extend_upto,
										   Buffer *buffers,
										   uint32 *extended_by);
static bool PinBuffer(BufferDesc *buf, BufferAccessStrategy strategy);
static void PinBuffer_Locked(BufferDesc *buf);
static void UnpinBuffer(BufferDesc *buf);
static void BufferSync(int flags);
static uint32 WaitBufHdrUnlocked(BufferDesc *buf);
static int	SyncOneBuffer(int buf_id, bool skip_recently_used,
						  WritebackContext *wb_context);
static void WaitIO(BufferDesc *buf);
static bool StartBufferIO(BufferDesc *buf, bool forInput);
static void TerminateBufferIO(BufferDesc *buf, bool clear_dirty,
							  uint32 set_flag_bits);
static void shared_buffer_write_error_callback(void *arg);
static void local_buffer_write_error_callback(void *arg);
static BufferDesc *BufferAlloc(SMgrRelation smgr,
							   char relpersistence,
							   ForkNumber forkNum,
							   BlockNumber blockNum,
							   BufferAccessStrategy strategy,
							   bool *foundPtr, IOContext io_context);
static Buffer GetVictimBuffer(BufferAccessStrategy strategy, IOContext io_context);
static void FlushBuffer(BufferDesc *buf, SMgrRelation reln,
						IOObject io_object, IOContext io_context);
static void FindAndDropRelationBuffers(RelFileLocator rlocator,
									   ForkNumber forkNum,
									   BlockNumber nForkBlock,
									   BlockNumber firstDelBlock);
static void RelationCopyStorageUsingBuffer(RelFileLocator srclocator,
										   RelFileLocator dstlocator,
										   ForkNumber forkNum, bool permanent);
static void AtProcExit_Buffers(int code, Datum arg);
static void CheckForBufferLeaks(void);
static int	rlocator_comparator(const void *p1, const void *p2);
static inline int buffertag_comparator(const BufferTag *ba, const BufferTag *bb);
static inline int ckpt_buforder_comparator(const CkptSortItem *a, const CkptSortItem *b);
static int	ts_ckpt_progress_comparator(Datum a, Datum b, void *arg);


/*
 * PrefetchBuffer() 对共享缓冲区的实现。
 */
PrefetchBufferResult
PrefetchSharedBuffer(SMgrRelation smgr_reln,
					 ForkNumber forkNum,
					 BlockNumber blockNum)
{
	PrefetchBufferResult result = {InvalidBuffer, false};
	BufferTag	newTag;			/* 所请求块的标识 */
	uint32		newHash;		/* newTag 的散列值 */
	LWLock	   *newPartitionLock;	/* 它的缓冲区分区锁 */
	int			buf_id;

	Assert(BlockNumberIsValid(blockNum));

	/* 创建一个标签，以便查找该缓冲区 */
	InitBufferTag(&newTag, &smgr_reln->smgr_rlocator.locator,
				  forkNum, blockNum);

	/* 确定它的散列码和分区锁标识 */
	newHash = BufTableHashCode(&newTag);
	newPartitionLock = BufMappingPartitionLock(newHash);

	/* 看该块的缓冲区是否已在缓冲池中 */
	LWLockAcquire(newPartitionLock, LW_SHARED);
	buf_id = BufTableLookup(&newTag, newHash);
	LWLockRelease(newPartitionLock);

	/* 若不在缓冲区里，就发起预取 */
	if (buf_id < 0)
	{
#ifdef USE_PREFETCH
		/*
		 * 尝试发起一次异步读。若在恢复中且关系文件
		 * 不存在，这会返回假。
		 */
		if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
			smgrprefetch(smgr_reln, forkNum, blockNum))
		{
			result.initiated_io = true;
		}
#endif							/* USE_PREFETCH（预取） */
	}
	else
	{
		/*
		 * 报告它当时所在的缓冲区。调用者也许能
		 * 避免一次缓冲区表查找，但它没有被钉住，必须
		 * 重新检查！
		 */
		result.recent_buffer = buf_id + 1;
	}

	/*
	 * 若该块已经在缓冲区里，我们什么也不做。这并不理想：
	 * 该块可能正要被逐出，而我们知道很快就要用它，
	 * 那样做就很蠢。但唯一容易的办法
	 * 是增加 usage_count，这看来不是好办法：
	 * 当调用者最终触及该块时，usage_count 会再次
	 * 被增加，结果对参与预取序列的块
	 * 过于偏袒。真正的修复需要一些
	 * 额外的每缓冲区状态，而且不清楚问题是否
	 * 严重到值得那样做。
	 */

	return result;
}

/*
 * PrefetchBuffer -- 发起对关系某个块的异步读。
 *
 * 它按 ReadBuffer 类比命名，但实际上不分配
 * 缓冲区。它试图确保将来对该块的 ReadBuffer
 * 不会被 I/O 耽误。预取是可选的。
 *
 * 有三种可能的结果：
 *
 * 1. 若该块已被缓存，结果包含一个有效缓冲区，调用者
 * 可用它来避免稍后的缓冲区查找，但它
 * 没有被钉住，因此调用者必须重新检查。
 *
 * 2. 若已请求内核发起 I/O，则 initiated_io 成员为
 * 真。目前无法知道数据是否已被内核缓存、因而
 * 并没有真正发起 I/O，也只能靠同步的 ReadBuffer()
 * 才能知道 I/O 何时完成。
 *
 * 3. 否则，该缓冲区尚未被 PostgreSQL 缓存，并且
 * 未定义 USE_PREFETCH（本构建因缺少内核设施而不支持预取），
 * 或者启用了直接 I/O，或者底层
 * 关系文件未找到且我们处于恢复中。（若关系文件
 * 未找到且我们不在恢复中，就会报错）。
 */
PrefetchBufferResult
PrefetchBuffer(Relation reln, ForkNumber forkNum, BlockNumber blockNum)
{
	Assert(RelationIsValid(reln));
	Assert(BlockNumberIsValid(blockNum));

	if (RelationUsesLocalBuffers(reln))
	{
		/* 见 ReadBufferExtended 里的注释 */
		if (RELATION_IS_OTHER_TEMP(reln))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot access temporary tables of other sessions")));

		/* 把它交给 localbuf.c */
		return PrefetchLocalBuffer(RelationGetSmgr(reln), forkNum, blockNum);
	}
	else
	{
		/* 把它交给共享缓冲区版本 */
		return PrefetchSharedBuffer(RelationGetSmgr(reln), forkNum, blockNum);
	}
}

/*
 * ReadRecentBuffer -- 尝试钉住最近观察过的缓冲区里的块。
 *
 * 与 ReadBuffer() 相比，成功时这避免了一次缓冲区映射查找。
 * 若缓冲区有效且仍具有预期的标签，就返回真。
 * 此时缓冲区被钉住，并且使用计数被增加。
 */
bool
ReadRecentBuffer(RelFileLocator rlocator, ForkNumber forkNum, BlockNumber blockNum,
				 Buffer recent_buffer)
{
	BufferDesc *bufHdr;
	BufferTag	tag;
	uint32		buf_state;
	bool		have_private_ref;

	Assert(BufferIsValid(recent_buffer));

	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);
	ReservePrivateRefCountEntry();
	InitBufferTag(&tag, &rlocator, forkNum, blockNum);

	if (BufferIsLocal(recent_buffer))
	{
		int			b = -recent_buffer - 1;

		bufHdr = GetLocalBufferDescriptor(b);
		buf_state = pg_atomic_read_u32(&bufHdr->state);

		/* 它是否仍然有效并持有正确的标签？ */
		if ((buf_state & BM_VALID) && BufferTagsEqual(&tag, &bufHdr->tag))
		{
			PinLocalBuffer(bufHdr, true);

			pgBufferUsage.local_blks_hit++;

			return true;
		}
	}
	else
	{
		bufHdr = GetBufferDescriptor(recent_buffer - 1);
		have_private_ref = GetPrivateRefCount(recent_buffer) > 0;

		/*
		 * 我们是否已经用私有引用钉住了这个缓冲区？若
		 * 是，它必定有效，不持锁检查标签也是安全的。
		 * 若不是，我们必须先锁住缓冲区描述符，然后再检查。
		 */
		if (have_private_ref)
			buf_state = pg_atomic_read_u32(&bufHdr->state);
		else
			buf_state = LockBufHdr(bufHdr);

		if ((buf_state & BM_VALID) && BufferTagsEqual(&tag, &bufHdr->tag))
		{
			/*
			 * 现在钉住该缓冲区是安全的。我们不能先钉住再
			 * 提问，因为若钉住了一个随便的不匹配缓冲区，
			 * 可能会迷惑 InvalidateBuffer() 之类的代码路径。
			 */
			if (have_private_ref)
				PinBuffer(bufHdr, NULL);	/* 增加钉住计数 */
			else
				PinBuffer_Locked(bufHdr);	/* 第一次钉住 */

			pgBufferUsage.shared_blks_hit++;

			return true;
		}

		/* 若上面锁住了缓冲区描述符，现在解锁。 */
		if (!have_private_ref)
			UnlockBufHdr(bufHdr, buf_state);
	}

	return false;
}

/*
 * ReadBuffer -- ReadBufferExtended 的简写，用于以 RBM_NORMAL 模式和默认策略
 *		从主分叉读取。
 */
Buffer
ReadBuffer(Relation reln, BlockNumber blockNum)
{
	return ReadBufferExtended(reln, MAIN_FORKNUM, blockNum, RBM_NORMAL, NULL);
}

/*
 * ReadBufferExtended -- 返回包含所请求关系的
 *		所请求块的缓冲区。若请求的 blknum
 *		是 P_NEW，就扩展关系文件并
 *		分配一个新块。（调用者负责
 *		确保同一时刻只有一个后端试图扩展
 *		一个关系！）
 *
 * 返回值：包含所读块的缓冲区号。
 *		返回的缓冲区已被钉住。
 *		出错时不返回，而是 elog。
 *
 * 假定调用本函数时，reln 已经打开。
 *
 * 在 RBM_NORMAL 模式下，从磁盘读取该页，并校验页头。
 * 若页头无效就报错。（但
 * 注意全零页被视为有效；见
 * PageIsVerifiedExtended()。）
 *
 * RBM_ZERO_ON_ERROR 与普通模式类似，但若页头无效，
 * 就把该页清零，而不是报错。这用于
 * 非关键数据，调用者准备好修复错误。
 *
 * 在 RBM_ZERO_AND_LOCK 模式下，若该页尚未在缓冲池中，就
 * 用零填充它，而不是从磁盘读取。当调用者
 * 要从头填充该页时有用，因为这省掉 I/O，并避免
 * 磁盘上的页有损坏页头时不必要的失败。
 * 返回的页是锁定的，以确保调用者有机会在
 * 它对其它人可见之前初始化该页。
 * 注意：不要用这种模式读取超出关系
 * 当前物理 EOF 的页；当该页被修改并写出时，
 * 这很可能在 md.c 里造成问题。不过 P_NEW 是可以的。
 *
 * RBM_ZERO_AND_CLEANUP_LOCK 与 RBM_ZERO_AND_LOCK 相同，但取得
 * 该页上的清理强度锁。
 *
 * RBM_NORMAL_NO_LOG 模式在这里与 RBM_NORMAL 同样处理。
 *
 * 若 strategy 不是 NULL，就使用非默认的缓冲区访问策略。
 * 详见 buffer/README。
 */
Buffer
ReadBufferExtended(Relation reln, ForkNumber forkNum, BlockNumber blockNum,
				   ReadBufferMode mode, BufferAccessStrategy strategy)
{
	bool		hit;
	Buffer		buf;

	/*
	 * 拒绝读取非本地临时关系的尝试；由于我们看不到
	 * 拥有者会话的本地缓冲区，我们很可能会
	 * 得到错误的数据。
	 */
	if (RELATION_IS_OTHER_TEMP(reln))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	/*
	 * 读取该缓冲区，并更新 pgstat 计数器以反映缓存命中或
	 * 未命中。
	 */
	pgstat_count_buffer_read(reln);
	buf = ReadBuffer_common(RelationGetSmgr(reln), reln->rd_rel->relpersistence,
							forkNum, blockNum, mode, strategy, &hit);
	if (hit)
		pgstat_count_buffer_hit(reln);
	return buf;
}


/*
 * ReadBufferWithoutRelcache -- 像 ReadBufferExtended，但不需要
 *		该关系的 relcache 项。
 *
 * 对 RELPERSISTENCE_PERMANENT 关系传入 permanent = true，对
 * RELPERSISTENCE_UNLOGGED 关系传入 permanent = false。本函数
 * 无法用于临时关系（除非我们只想读取自己的
 * BackendId 的临时关系，否则让它工作可能
 * 很困难）。
 */
Buffer
ReadBufferWithoutRelcache(RelFileLocator rlocator, ForkNumber forkNum,
						  BlockNumber blockNum, ReadBufferMode mode,
						  BufferAccessStrategy strategy, bool permanent)
{
	bool		hit;

	SMgrRelation smgr = smgropen(rlocator, InvalidBackendId);

	return ReadBuffer_common(smgr, permanent ? RELPERSISTENCE_PERMANENT :
							 RELPERSISTENCE_UNLOGGED, forkNum, blockNum,
							 mode, strategy, &hit);
}

/*
 * ExtendBufferedRelBy() 的便捷包装，只扩展一个块。
 */
Buffer
ExtendBufferedRel(BufferManagerRelation bmr,
				  ForkNumber forkNum,
				  BufferAccessStrategy strategy,
				  uint32 flags)
{
	Buffer		buf;
	uint32		extend_by = 1;

	ExtendBufferedRelBy(bmr, forkNum, strategy, flags, extend_by,
						&buf, &extend_by);

	return buf;
}

/*
 * 把关系扩展多个块。
 *
 * 试图把关系扩展 extend_by 个块。取决于
 * 资源是否可用，关系最终可能只扩展了
 * 更少的页（除非报错，否则至少扩展一
 * 页）。*extended_by 被更新为关系被扩展到的
 * 页数。
 *
 * buffers 必须是长度至少为 extend_by 的数组。完成
 * 后，前 extend_by 个数组元素将指向已钉住的
 * 缓冲区。
 *
 * 若 flags 含有 EB_LOCK_FIRST，返回的第一个缓冲区是
 * 锁定的。这对想要一个保证为
 * 空的缓冲区的调用者有用。
 */
BlockNumber
ExtendBufferedRelBy(BufferManagerRelation bmr,
					ForkNumber fork,
					BufferAccessStrategy strategy,
					uint32 flags,
					uint32 extend_by,
					Buffer *buffers,
					uint32 *extended_by)
{
	Assert((bmr.rel != NULL) != (bmr.smgr != NULL));
	Assert(bmr.smgr == NULL || bmr.relpersistence != 0);
	Assert(extend_by > 0);

	if (bmr.smgr == NULL)
	{
		bmr.smgr = RelationGetSmgr(bmr.rel);
		bmr.relpersistence = bmr.rel->rd_rel->relpersistence;
	}

	return ExtendBufferedRelCommon(bmr, fork, strategy, flags,
								   extend_by, InvalidBlockNumber,
								   buffers, extended_by);
}

/*
 * 扩展关系，使它至少有 extend_to 个块，返回缓冲区
 * (extend_to - 1)。
 *
 * 这对想写特定页、而不管
 * 关系当前大小的调用者有用（例如对可见性映射和
 * 崩溃恢复有用）。
 */
Buffer
ExtendBufferedRelTo(BufferManagerRelation bmr,
					ForkNumber fork,
					BufferAccessStrategy strategy,
					uint32 flags,
					BlockNumber extend_to,
					ReadBufferMode mode)
{
	BlockNumber current_size;
	uint32		extended_by = 0;
	Buffer		buffer = InvalidBuffer;
	Buffer		buffers[64];

	Assert((bmr.rel != NULL) != (bmr.smgr != NULL));
	Assert(bmr.smgr == NULL || bmr.relpersistence != 0);
	Assert(extend_to != InvalidBlockNumber && extend_to > 0);

	if (bmr.smgr == NULL)
	{
		bmr.smgr = RelationGetSmgr(bmr.rel);
		bmr.relpersistence = bmr.rel->rd_rel->relpersistence;
	}

	/*
	 * 若希望如此，文件不存在就创建它。若
	 * smgr_cached_nblocks[fork] 为正，则它必定存在，不必
	 * 调用 smgrexists。
	 */
	if ((flags & EB_CREATE_FORK_IF_NEEDED) &&
		(bmr.smgr->smgr_cached_nblocks[fork] == 0 ||
		 bmr.smgr->smgr_cached_nblocks[fork] == InvalidBlockNumber) &&
		!smgrexists(bmr.smgr, fork))
	{
		LockRelationForExtension(bmr.rel, ExclusiveLock);

		/* 等待锁期间可能已被关闭 */
		if (bmr.rel)
			bmr.smgr = RelationGetSmgr(bmr.rel);

		/* 重新检查，分叉可能已被并发创建 */
		if (!smgrexists(bmr.smgr, fork))
			smgrcreate(bmr.smgr, fork, flags & EB_PERFORMING_RECOVERY);

		UnlockRelationForExtension(bmr.rel, ExclusiveLock);
	}

	/*
	 * 若有请求，就使大小缓存失效，以便 smgrnblocks 去问
	 * 内核。
	 */
	if (flags & EB_CLEAR_SIZE_CACHE)
		bmr.smgr->smgr_cached_nblocks[fork] = InvalidBlockNumber;

	/*
	 * 估计我们需要扩展多少页。这避免取得
	 * 不必要的那么多牺牲缓冲区。
	 */
	current_size = smgrnblocks(bmr.smgr, fork);

	/*
	 * 由于还没有其他人能查看页内容，排他锁和
	 * 清理强度的锁没有区别。注意
	 * 回退到读取缓冲区以应对并发的关系扩展时，我们把原来的模式传给
	 * 下面的 ReadBuffer_common()。
	 */
	if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
		flags |= EB_LOCK_TARGET;

	while (current_size < extend_to)
	{
		uint32		num_pages = lengthof(buffers);
		BlockNumber first_block;

		if ((uint64) current_size + num_pages > extend_to)
			num_pages = extend_to - current_size;

		first_block = ExtendBufferedRelCommon(bmr, fork, strategy, flags,
											  num_pages, extend_to,
											  buffers, &extended_by);

		current_size = first_block + extended_by;
		Assert(num_pages != 0 || current_size >= extend_to);

		for (int i = 0; i < extended_by; i++)
		{
			if (first_block + i != extend_to - 1)
				ReleaseBuffer(buffers[i]);
			else
				buffer = buffers[i];
		}
	}

	/*
	 * 另一个后端可能并发地扩展了该关系。
	 * 那种情况下就读取该缓冲区。
	 *
	 * XXX：我们是否应当用一个标志来控制这一点？
	 */
	if (buffer == InvalidBuffer)
	{
		bool		hit;

		Assert(extended_by == 0);
		buffer = ReadBuffer_common(bmr.smgr, bmr.relpersistence,
								   fork, extend_to - 1, mode, strategy,
								   &hit);
	}

	return buffer;
}

/*
 * ReadBuffer_common -- 所有 ReadBuffer 变体的公共逻辑。
 *
 * 若请求由共享缓冲区满足，*hit 被设为真。
 */
static Buffer
ReadBuffer_common(SMgrRelation smgr, char relpersistence, ForkNumber forkNum,
				  BlockNumber blockNum, ReadBufferMode mode,
				  BufferAccessStrategy strategy, bool *hit)
{
	BufferDesc *bufHdr;
	Block		bufBlock;
	bool		found;
	IOContext	io_context;
	IOObject	io_object;
	bool		isLocalBuf = SmgrIsTemp(smgr);

	*hit = false;

	/*
	 * 向后兼容路径，多数代码应当改用 ExtendBufferedRel()，
	 * 因为在 ExtendBufferedRel() 内部取得扩展锁
	 * 伸缩性好得多。
	 */
	if (unlikely(blockNum == P_NEW))
	{
		uint32		flags = EB_SKIP_EXTENSION_LOCK;

		/*
		 * 由于还没有其他人能查看页内容，
		 * 排他锁和清理强度的
		 * 锁没有区别。
		 */
		if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
			flags |= EB_LOCK_FIRST;

		return ExtendBufferedRel(BMR_SMGR(smgr, relpersistence),
								 forkNum, strategy, flags);
	}

	/* 确保我们有地方记住这次缓冲区钉住 */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	TRACE_POSTGRESQL_BUFFER_READ_START(forkNum, blockNum,
									   smgr->smgr_rlocator.locator.spcOid,
									   smgr->smgr_rlocator.locator.dbOid,
									   smgr->smgr_rlocator.locator.relNumber,
									   smgr->smgr_rlocator.backend);

	if (isLocalBuf)
	{
		/*
		 * 临时表的 I/O 不使用 BufferAccessStrategy。
		 * 不过有些情形下 strategy 可能不是 NULL，因此我们没法
		 * 靠 IOContextForStrategy() 为我们设置正确的 IOContext。
		 * CREATE TEMPORARY TABLE AS... 之类的情形可能如此。
		 */
		io_context = IOCONTEXT_NORMAL;
		io_object = IOOBJECT_TEMP_RELATION;
		bufHdr = LocalBufferAlloc(smgr, forkNum, blockNum, &found);
		if (found)
			pgBufferUsage.local_blks_hit++;
		else if (mode == RBM_NORMAL || mode == RBM_NORMAL_NO_LOG ||
				 mode == RBM_ZERO_ON_ERROR)
			pgBufferUsage.local_blks_read++;
	}
	else
	{
		/*
		 * 查找该缓冲区。若请求的块目前
		 * 不在内存中，就设置 IO_IN_PROGRESS。
		 */
		io_context = IOContextForStrategy(strategy);
		io_object = IOOBJECT_RELATION;
		bufHdr = BufferAlloc(smgr, relpersistence, forkNum, blockNum,
							 strategy, &found, io_context);
		if (found)
			pgBufferUsage.shared_blks_hit++;
		else if (mode == RBM_NORMAL || mode == RBM_NORMAL_NO_LOG ||
				 mode == RBM_ZERO_ON_ERROR)
			pgBufferUsage.shared_blks_read++;
	}

	/* 此刻我们不持有任何锁。 */

	/* 若该缓冲区已在缓冲池中，我们就完成了 */
	if (found)
	{
		/* 退出前只需更新统计信息 */
		*hit = true;
		VacuumPageHit++;
		pgstat_count_io_op(io_object, io_context, IOOP_HIT);

		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageHit;

		TRACE_POSTGRESQL_BUFFER_READ_DONE(forkNum, blockNum,
										  smgr->smgr_rlocator.locator.spcOid,
										  smgr->smgr_rlocator.locator.dbOid,
										  smgr->smgr_rlocator.locator.relNumber,
										  smgr->smgr_rlocator.backend,
										  found);

		/*
		 * 在 RBM_ZERO_AND_LOCK 模式下，调用者预期返回时该页
		 * 已被锁定。
		 */
		if (!isLocalBuf)
		{
			if (mode == RBM_ZERO_AND_LOCK)
				LWLockAcquire(BufferDescriptorGetContentLock(bufHdr),
							  LW_EXCLUSIVE);
			else if (mode == RBM_ZERO_AND_CLEANUP_LOCK)
				LockBufferForCleanup(BufferDescriptorGetBuffer(bufHdr));
		}

		return BufferDescriptorGetBuffer(bufHdr);
	}

	/*
	 * 若我们到了这一步，已经为该页分配了缓冲区，
	 * 但其内容尚未有效。若它是共享缓冲区，
	 * 就为它设置了 IO_IN_PROGRESS。
	 */
	Assert(!(pg_atomic_read_u32(&bufHdr->state) & BM_VALID));	/* 不需要自旋锁 */

	bufBlock = isLocalBuf ? LocalBufHdrGetBlock(bufHdr) : BufHdrGetBlock(bufHdr);

	/*
	 * 读入该页，除非调用者打算覆盖它，只是
	 * 想让我们分配一个缓冲区。
	 */
	if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
		MemSet((char *) bufBlock, 0, BLCKSZ);
	else
	{
		instr_time	io_start = pgstat_prepare_io_time();

		smgrread(smgr, forkNum, blockNum, bufBlock);

		pgstat_count_io_op_time(io_object, io_context,
								IOOP_READ, io_start, 1);

		/* 检查垃圾数据 */
		if (!PageIsVerifiedExtended((Page) bufBlock, blockNum,
									PIV_LOG_WARNING | PIV_REPORT_STAT))
		{
			if (mode == RBM_ZERO_ON_ERROR || zero_damaged_pages)
			{
				ereport(WARNING,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid page in block %u of relation %s; zeroing out page",
								blockNum,
								relpath(smgr->smgr_rlocator, forkNum))));
				MemSet((char *) bufBlock, 0, BLCKSZ);
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid page in block %u of relation %s",
								blockNum,
								relpath(smgr->smgr_rlocator, forkNum))));
		}
	}

	/*
	 * 在 RBM_ZERO_AND_LOCK / RBM_ZERO_AND_CLEANUP_LOCK 模式下，在把页标为有效之前
	 * 先取得缓冲区内容锁，以确保没有
	 * 其他后端在调用者有机会初始化它之前
	 * 看到被清零的页。
	 *
	 * 由于还没有其他人能查看页内容，排他锁和
	 * 清理强度的锁没有区别。（注意
	 * 这里无法使用 LockBuffer() 或 LockBufferForCleanup()，因为
	 * 它们断言缓冲区已经有效。）
	 */
	if ((mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK) &&
		!isLocalBuf)
	{
		LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_EXCLUSIVE);
	}

	if (isLocalBuf)
	{
		/* 只需调整标志 */
		uint32		buf_state = pg_atomic_read_u32(&bufHdr->state);

		buf_state |= BM_VALID;
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
	}
	else
	{
		/* 设置 BM_VALID，结束 IO，并唤醒任何等待者 */
		TerminateBufferIO(bufHdr, false, BM_VALID);
	}

	VacuumPageMiss++;
	if (VacuumCostActive)
		VacuumCostBalance += VacuumCostPageMiss;

	TRACE_POSTGRESQL_BUFFER_READ_DONE(forkNum, blockNum,
									  smgr->smgr_rlocator.locator.spcOid,
									  smgr->smgr_rlocator.locator.dbOid,
									  smgr->smgr_rlocator.locator.relNumber,
									  smgr->smgr_rlocator.backend,
									  found);

	return BufferDescriptorGetBuffer(bufHdr);
}

/*
 * BufferAlloc -- ReadBuffer 的子例程。处理共享
 *		缓冲区的查找。若尚不存在缓冲区，就选择一个替换
 *		牺牲者并逐出旧页，但不读入新页。
 *
 * strategy 可以是缓冲区替换策略对象，或者对
 * 默认策略为 NULL。使用默认策略时，所选缓冲区的 usage_count 会前进，
 * 否则可能不会（见 PinBuffer）。
 *
 * 返回的缓冲区已被钉住，并已被标为持有
 * 想要的页。若它本来就有想要的页，*foundPtr 被
 * 设为真。否则 *foundPtr 被设为假，并且缓冲区被标为
 * IO_IN_PROGRESS；ReadBuffer 现在需要做 I/O 来填充它。
 *
 * *foundPtr 实际上与缓冲区的 BM_VALID 标志重复，但
 * 我们为了 ReadBuffer 里的简单而保留它。
 *
 * 把 io_context 作为输出参数传递，以便在共享缓冲区命中且
 * 不必采集 IO 统计信息时，避免调用
 * IOContextForStrategy()。
 *
 * 入口和出口都不持有锁。
 */
static BufferDesc *
BufferAlloc(SMgrRelation smgr, char relpersistence, ForkNumber forkNum,
			BlockNumber blockNum,
			BufferAccessStrategy strategy,
			bool *foundPtr, IOContext io_context)
{
	BufferTag	newTag;			/* 所请求块的标识 */
	uint32		newHash;		/* newTag 的散列值 */
	LWLock	   *newPartitionLock;	/* 它的缓冲区分区锁 */
	int			existing_buf_id;
	Buffer		victim_buffer;
	BufferDesc *victim_buf_hdr;
	uint32		victim_buf_state;

	/* 创建一个标签，以便查找该缓冲区 */
	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* 确定它的散列码和分区锁标识 */
	newHash = BufTableHashCode(&newTag);
	newPartitionLock = BufMappingPartitionLock(newHash);

	/* 看该块的缓冲区是否已在缓冲池中 */
	LWLockAcquire(newPartitionLock, LW_SHARED);
	existing_buf_id = BufTableLookup(&newTag, newHash);
	if (existing_buf_id >= 0)
	{
		BufferDesc *buf;
		bool		valid;

		/*
		 * 找到了。现在钉住该缓冲区，以免有人把它从
		 * 缓冲池里偷走，并检查正确的数据是否已装入
		 * 该缓冲区。
		 */
		buf = GetBufferDescriptor(existing_buf_id);

		valid = PinBuffer(buf, strategy);

		/* 一旦钉住它，就可以释放映射锁 */
		LWLockRelease(newPartitionLock);

		*foundPtr = true;

		if (!valid)
		{
			/*
			 * 我们只有在 (a) 别人仍在读入
			 * 该页，或 (b) 先前的读尝试失败时才能到这里。我们必须
			 * 等待任何进行中的读尝试结束，然后若该页仍不是 BM_VALID，
			 * 就安排我们自己的读尝试。
			 * StartBufferIO 把这些都做了。
			 */
			if (StartBufferIO(buf, true))
			{
				/*
				 * 若我们到了这里，先前读取该缓冲区的尝试必定
				 * 已经失败……但我们将勇敢地重试。
				 */
				*foundPtr = false;
			}
		}

		return buf;
	}

	/*
	 * 在缓冲池里没找到它。我们必须初始化一个新
	 * 缓冲区。做这项工作时记得解开映射锁。
	 */
	LWLockRelease(newPartitionLock);

	/*
	 * 取得一个牺牲缓冲区。别人可能也想做同样的事，我们
	 * 不持有任何冲突的锁。若如此，我们稍后得撤销
	 * 自己的工作。
	 */
	victim_buffer = GetVictimBuffer(strategy, io_context);
	victim_buf_hdr = GetBufferDescriptor(victim_buffer - 1);

	/*
	 * 试图用新标签为该缓冲区做一条散列表项。若
	 * 别人已经为该标签插入了另一个缓冲区，我们就释放
	 * 取得的牺牲缓冲区，改用已经插入的那个。
	 */
	LWLockAcquire(newPartitionLock, LW_EXCLUSIVE);
	existing_buf_id = BufTableInsert(&newTag, newHash, victim_buf_hdr->buf_id);
	if (existing_buf_id >= 0)
	{
		BufferDesc *existing_buf_hdr;
		bool		valid;

		/*
		 * 发生了冲突。别人已经做了我们正要做的事。
		 * 我们就把它当作一开始就在缓冲池里
		 * 找到的来处理。首先，放弃我们原本打算
		 * 使用的缓冲区。
		 *
		 * 我们可以在释放分区锁之后做这件事，但那样我们就得
		 * 在取得锁之前调用 ResourceOwnerEnlargeBuffers() 和
		 * ReservePrivateRefCountEntry()，以应对
		 * 这种少见的冲突。
		 */
		UnpinBuffer(victim_buf_hdr);

		/*
		 * 我们先前取得的牺牲缓冲区是干净且未使用的，让
		 * 它能很快被再次找到
		 */
		StrategyFreeBuffer(victim_buf_hdr);

		/* 其余代码应当与例程顶部的代码匹配 */

		existing_buf_hdr = GetBufferDescriptor(existing_buf_id);

		valid = PinBuffer(existing_buf_hdr, strategy);

		/* 一旦钉住它，就可以释放映射锁 */
		LWLockRelease(newPartitionLock);

		*foundPtr = true;

		if (!valid)
		{
			/*
			 * 我们只有在 (a) 别人仍在读入
			 * 该页，或 (b) 先前的读尝试失败时才能到这里。我们必须
			 * 等待任何进行中的读尝试结束，然后若该页仍不是 BM_VALID，
			 * 就安排我们自己的读尝试。
			 * StartBufferIO 把这些都做了。
			 */
			if (StartBufferIO(existing_buf_hdr, true))
			{
				/*
				 * 若我们到了这里，先前读取该缓冲区的尝试必定
				 * 已经失败……但我们将勇敢地重试。
				 */
				*foundPtr = false;
			}
		}

		return existing_buf_hdr;
	}

	/*
	 * 为了改它的标签，也需要锁住缓冲区描述符。
	 */
	victim_buf_state = LockBufHdr(victim_buf_hdr);

	/* 持有缓冲区描述符锁时做一些健全性检查 */
	Assert(BUF_STATE_GET_REFCOUNT(victim_buf_state) == 1);
	Assert(!(victim_buf_state & (BM_TAG_VALID | BM_VALID | BM_DIRTY | BM_IO_IN_PROGRESS)));

	victim_buf_hdr->tag = newTag;

	/*
	 * 确保对每次检查点都必须写出的缓冲区设置 BM_PERMANENT。
	 * 未日志记录的缓冲区只需要在关闭
	 * 检查点时写出，但其 init 分叉除外，那些需要被当作
	 * 与永久关系一样。
	 */
	victim_buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;
	if (relpersistence == RELPERSISTENCE_PERMANENT || forkNum == INIT_FORKNUM)
		victim_buf_state |= BM_PERMANENT;

	UnlockBufHdr(victim_buf_hdr, victim_buf_state);

	LWLockRelease(newPartitionLock);

	/*
	 * 缓冲区内容目前无效。试图取得开始
	 * I/O 的权利。若 StartBufferIO 返回假，则别人赶在
	 * 我们之前把它读了进来，因此 BufferAlloc() 就
	 * 无事可做了。
	 */
	if (StartBufferIO(victim_buf_hdr, true))
		*foundPtr = false;
	else
		*foundPtr = true;

	return victim_buf_hdr;
}

/*
 * InvalidateBuffer -- 把共享缓冲区标为无效，并把它放回
 * 空闲链表。
 *
 * 入口时必须持有缓冲区描述符自旋锁。我们在
 * 返回前放下它。（这是合理的，因为调用者必须已经锁住
 * 该缓冲区，才能确定它应当被丢掉。）
 *
 * 这只用于丢掉关系之类的情形。我们假定
 * 不可能有其他后端有兴趣使用该页，
 * 因此该缓冲区被钉住的唯一原因是别人
 * 正试图把它写出。我们必须让他们完成，然后才能
 * 收回该缓冲区。
 *
 * 我们等待取得必要的锁时，该缓冲区可能被别人
 * 收回；若如此，就不要把它弄乱。
 */
static void
InvalidateBuffer(BufferDesc *buf)
{
	BufferTag	oldTag;
	uint32		oldHash;		/* oldTag 的散列值 */
	LWLock	   *oldPartitionLock;	/* 它的缓冲区分区锁 */
	uint32		oldFlags;
	uint32		buf_state;

	/* 在放下自旋锁之前保存原来的缓冲区标签 */
	oldTag = buf->tag;

	buf_state = pg_atomic_read_u32(&buf->state);
	Assert(buf_state & BM_LOCKED);
	UnlockBufHdr(buf, buf_state);

	/*
	 * 需要计算旧标签的散列码和分区锁标识。XXX 把散列码存在
	 * BufferDesc 里、免得在这里重算，是否
	 * 值得？大概不值得。
	 */
	oldHash = BufTableHashCode(&oldTag);
	oldPartitionLock = BufMappingPartitionLock(oldHash);

retry:

	/*
	 * 取得排他映射锁，为改变该缓冲区的
	 * 关联做准备。
	 */
	LWLockAcquire(oldPartitionLock, LW_EXCLUSIVE);

	/* 重新锁住缓冲区描述符 */
	buf_state = LockBufHdr(buf);

	/* 若我们等锁时它变了，就什么也不做 */
	if (!BufferTagsEqual(&buf->tag, &oldTag))
	{
		UnlockBufHdr(buf, buf_state);
		LWLockRelease(oldPartitionLock);
		return;
	}

	/*
	 * We assume the only reason for it to be pinned is that someone else is
	 * flushing the page out.  Wait for them to finish.  (This could be an
	 * infinite loop if the refcount is messed up... it would be nice to time
	 * out after awhile, but there seems no way to be sure how many loops may
	 * be needed.  Note that if the other guy has pinned the buffer but not
	 * yet done StartBufferIO, WaitIO will fall through and we'll effectively
	 * be busy-looping here.)
	 */
	if (BUF_STATE_GET_REFCOUNT(buf_state) != 0)
	{
		UnlockBufHdr(buf, buf_state);
		LWLockRelease(oldPartitionLock);
		/* 安全检查：肯定不应当是我们自己的钉住 */
		if (GetPrivateRefCount(BufferDescriptorGetBuffer(buf)) > 0)
			elog(ERROR, "buffer is pinned in InvalidateBuffer");
		WaitIO(buf);
		goto retry;
	}

	/*
	 * 清掉该缓冲区的标签和标志。我们必须这样做，以确保
	 * 对缓冲区数组的线性扫描不会认为该缓冲区有效。
	 */
	oldFlags = buf_state & BUF_FLAG_MASK;
	ClearBufferTag(&buf->tag);
	buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
	UnlockBufHdr(buf, buf_state);

	/*
	 * 若该缓冲区在查找散列表里，就把它去掉。
	 */
	if (oldFlags & BM_TAG_VALID)
		BufTableDelete(&oldTag, oldHash);

	/*
	 * 映射锁用完了。
	 */
	LWLockRelease(oldPartitionLock);

	/*
	 * 把该缓冲区插到空闲缓冲区链表的头部。
	 */
	StrategyFreeBuffer(buf);
}

/*
 * GetVictimBuffer() 的辅助例程。
 *
 * 需要对一个标签有效、已钉住、但未持有
 * 缓冲区描述符自旋锁的缓冲区调用。
 *
 * 若该缓冲区可以重用就返回真，此时它只被
 * 本后端钉住并被标为无效，否则返回假。
 */
static bool
InvalidateVictimBuffer(BufferDesc *buf_hdr)
{
	uint32		buf_state;
	uint32		hash;
	LWLock	   *partition_lock;
	BufferTag	tag;

	Assert(GetPrivateRefCount(BufferDescriptorGetBuffer(buf_hdr)) == 1);

	/* 缓冲区已被钉住，因此不持锁读标签是安全的 */
	tag = buf_hdr->tag;

	hash = BufTableHashCode(&tag);
	partition_lock = BufMappingPartitionLock(hash);

	LWLockAcquire(partition_lock, LW_EXCLUSIVE);

	/* 锁住缓冲区描述符 */
	buf_state = LockBufHdr(buf_hdr);

	/*
	 * 我们已经钉住该缓冲区，别人应当无法同时把
	 * 这一点清掉。
	 */
	Assert(buf_state & BM_TAG_VALID);
	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
	Assert(BufferTagsEqual(&buf_hdr->tag, &tag));

	/*
	 * 若此后别人钉住了该缓冲区，或者更糟，把它弄成脏页，
	 * 就放弃这个缓冲区：它显然在使用中。
	 */
	if (BUF_STATE_GET_REFCOUNT(buf_state) != 1 || (buf_state & BM_DIRTY))
	{
		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);

		UnlockBufHdr(buf_hdr, buf_state);
		LWLockRelease(partition_lock);

		return false;
	}

	/*
	 * 清掉该缓冲区的标签、标志和使用计数。这并非
	 * 严格必须，因为做任何事之前都要检查 BM_TAG_VALID/BM_VALID。
	 * 但目前这样做有好处，因为对共享缓冲区的几次线性扫描，
	 * 较便宜的预检查使用该标签
	 * （例如见 FlushDatabaseBuffers()）。
	 */
	ClearBufferTag(&buf_hdr->tag);
	buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
	UnlockBufHdr(buf_hdr, buf_state);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);

	/* 最后从缓冲区映射表中删除该缓冲区 */
	BufTableDelete(&tag, hash);

	LWLockRelease(partition_lock);

	Assert(!(buf_state & (BM_DIRTY | BM_VALID | BM_TAG_VALID)));
	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
	Assert(BUF_STATE_GET_REFCOUNT(pg_atomic_read_u32(&buf_hdr->state)) > 0);

	return true;
}

static Buffer
GetVictimBuffer(BufferAccessStrategy strategy, IOContext io_context)
{
	BufferDesc *buf_hdr;
	Buffer		buf;
	uint32		buf_state;
	bool		from_ring;

	/*
	 * 在尚未持有自旋锁时，确保有一个空闲的引用计数
	 * 项。
	 */
	ReservePrivateRefCountEntry();
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	/* 若预期的牺牲缓冲区被并发使用，我们就返回到这里 */
again:

	/*
	 * 选择一个牺牲缓冲区。返回时其描述符
	 * 自旋锁仍然持有！
	 */
	buf_hdr = StrategyGetBuffer(strategy, &buf_state, &from_ring);
	buf = BufferDescriptorGetBuffer(buf_hdr);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) == 0);

	/* 钉住该缓冲区，然后释放缓冲区自旋锁 */
	PinBuffer_Locked(buf_hdr);

	/*
	 * 我们不应当对该缓冲区还有任何其他钉住。
	 */
	CheckBufferIsPinnedOnce(buf);

	/*
	 * 若该缓冲区是脏页，就试图把它写出。这里有一个竞态，
	 * 别人可能在我们上面释放缓冲区描述符锁之后把它弄脏，
	 * 甚至在我们写出它时也如此（因为
	 * 我们的共享锁不能阻止提示位更新）。重新锁住缓冲区描述符后，
	 * 我们会重新检查脏位。
	 */
	if (buf_state & BM_DIRTY)
	{
		LWLock	   *content_lock;

		Assert(buf_state & BM_TAG_VALID);
		Assert(buf_state & BM_VALID);

		/*
		 * 我们需要缓冲区内容上的共享锁才能把它写出（否则
		 * 我们可能写出无效数据，例如因为别人在我们写的时候
		 * 正在压缩页内容）。这里必须使用条件锁
		 * 获取，以避免死锁。即使 StrategyGetBuffer
		 * 返回它时该缓冲区未被钉住（因而肯定未锁定），
		 * 别人也可能在我们到达这里之前已经钉住并排他锁定了它。
		 * 若我们无条件地试图取得锁，
		 * 我们就会阻塞等待他们；若他们稍后阻塞等待我们，
		 * 就会死锁。（已经观察到两个
		 * 后端都试图分裂 B 树索引页时发生这种情况，而第二个
		 * 恰好试图分裂第一个从
		 * StrategyGetBuffer 得到的那一页。）
		 */
		content_lock = BufferDescriptorGetContentLock(buf_hdr);
		if (!LWLockConditionalAcquire(content_lock, LW_SHARED))
		{
			/*
			 * 别人已经锁住了该缓冲区，因此放弃它并循环回去
			 * 再取一个。
			 */
			UnpinBuffer(buf_hdr);
			goto again;
		}

		/*
		 * 若使用非默认策略，且写出该缓冲区会
		 * 需要一次 WAL 刷写，就让策略决定是继续
		 * 写出并重用该缓冲区，还是另选一个牺牲者。我们需要
		 * 锁才能查看页的 LSN，因此这不能在
		 * StrategyGetBuffer 内部完成。
		 */
		if (strategy != NULL)
		{
			XLogRecPtr	lsn;

			/* 持有缓冲区描述符锁时读取 LSN */
			buf_state = LockBufHdr(buf_hdr);
			lsn = BufferGetLSN(buf_hdr);
			UnlockBufHdr(buf_hdr, buf_state);

			if (XLogNeedsFlush(lsn)
				&& StrategyRejectBuffer(strategy, buf_hdr, from_ring))
			{
				LWLockRelease(content_lock);
				UnpinBuffer(buf_hdr);
				goto again;
			}
		}

		/* 好，做 I/O */
		FlushBuffer(buf_hdr, NULL, IOOBJECT_RELATION, io_context);
		LWLockRelease(content_lock);

		ScheduleBufferTagForWriteback(&BackendWritebackContext, io_context,
									  &buf_hdr->tag);
	}


	if (buf_state & BM_VALID)
	{
		/*
		 * 使用 BufferAccessStrategy 时，从共享
		 * 缓冲区逐出的块在相应上下文里按 IOOP_EVICT 计数
		 * （例如 IOCONTEXT_BULKWRITE）。策略在两种情况下逐出共享缓冲区：
		 * 1) 最初为策略环认领缓冲区时
		 * 2) 替换已有的策略环缓冲区，
		 * 因为它被钉住或正在使用而无法重用。
		 *
		 * 从已经在策略环里的缓冲区逐出的块，
		 * 在相应策略上下文里按 IOOP_REUSE 计数。
		 *
		 * 到这一步，我们可以准确地统计逐出和重用，
		 * 因为我们已经成功认领了有效缓冲区。此前，
		 * 我们可能因并发的钉住者或出错而被迫
		 * 释放该缓冲区。
		 */
		pgstat_count_io_op(IOOBJECT_RELATION, io_context,
						   from_ring ? IOOP_REUSE : IOOP_EVICT);
	}

	/*
	 * 若该缓冲区在缓冲区映射表里有项，就删掉它。这
	 * 可能失败，因为另一个后端可能已经钉住或把它弄成脏页
	 * 了。
	 */
	if ((buf_state & BM_TAG_VALID) && !InvalidateVictimBuffer(buf_hdr))
	{
		UnpinBuffer(buf_hdr);
		goto again;
	}

	/* 最后一组健全性检查 */
#ifdef USE_ASSERT_CHECKING
	buf_state = pg_atomic_read_u32(&buf_hdr->state);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) == 1);
	Assert(!(buf_state & (BM_TAG_VALID | BM_VALID | BM_DIRTY)));

	CheckBufferIsPinnedOnce(buf);
#endif

	return buf;
}

/*
 * 限制一次批量操作可以额外取得的钉住数量，以免
 * 用尽可钉住的缓冲区。
 *
 * 始终允许再多一个钉住，否则该操作很可能
 * 根本无法执行。
 *
 * 一个后端允许的钉住数根据
 * shared_buffers 和可能的最大连接数计算。这非常
 * 悲观，但在玩具大小的 shared_buffers 之外，它应当允许
 * 足够的钉住。
 */
static void
LimitAdditionalPins(uint32 *additional_pins)
{
	uint32		max_backends;
	int			max_proportional_pins;

	if (*additional_pins <= 1)
		return;

	max_backends = MaxBackends + NUM_AUXILIARY_PROCS;
	max_proportional_pins = NBuffers / max_backends;

	/*
	 * 减去本后端已经钉住的缓冲区的大致数量。
	 * 溢出的钉住数我们可以免费得到，但不知道
	 * PrivateRefCountArray 里的钉住数。精确
	 * 计算它的代价似乎不值得，因此就假定最大值。
	 */
	max_proportional_pins -= PrivateRefCountOverflowed + REFCOUNT_ARRAY_ENTRIES;

	if (max_proportional_pins <= 0)
		max_proportional_pins = 1;

	if (*additional_pins > max_proportional_pins)
		*additional_pins = max_proportional_pins;
}

/*
 * ExtendBufferedRelBy() 与 ExtendBufferedRelTo() 之间共享的逻辑。只是为了
 * 避免重复跟踪和 relpersistence 相关的逻辑。
 */
static BlockNumber
ExtendBufferedRelCommon(BufferManagerRelation bmr,
						ForkNumber fork,
						BufferAccessStrategy strategy,
						uint32 flags,
						uint32 extend_by,
						BlockNumber extend_upto,
						Buffer *buffers,
						uint32 *extended_by)
{
	BlockNumber first_block;

	TRACE_POSTGRESQL_BUFFER_EXTEND_START(fork,
										 bmr.smgr->smgr_rlocator.locator.spcOid,
										 bmr.smgr->smgr_rlocator.locator.dbOid,
										 bmr.smgr->smgr_rlocator.locator.relNumber,
										 bmr.smgr->smgr_rlocator.backend,
										 extend_by);

	if (bmr.relpersistence == RELPERSISTENCE_TEMP)
		first_block = ExtendBufferedRelLocal(bmr, fork, flags,
											 extend_by, extend_upto,
											 buffers, &extend_by);
	else
		first_block = ExtendBufferedRelShared(bmr, fork, strategy, flags,
											  extend_by, extend_upto,
											  buffers, &extend_by);
	*extended_by = extend_by;

	TRACE_POSTGRESQL_BUFFER_EXTEND_DONE(fork,
										bmr.smgr->smgr_rlocator.locator.spcOid,
										bmr.smgr->smgr_rlocator.locator.dbOid,
										bmr.smgr->smgr_rlocator.locator.relNumber,
										bmr.smgr->smgr_rlocator.backend,
										*extended_by,
										first_block);

	return first_block;
}

/*
 * ExtendBufferedRelBy() 和 ExtendBufferedRelTo() 对
 * 共享缓冲区的实现。
 */
static BlockNumber
ExtendBufferedRelShared(BufferManagerRelation bmr,
						ForkNumber fork,
						BufferAccessStrategy strategy,
						uint32 flags,
						uint32 extend_by,
						BlockNumber extend_upto,
						Buffer *buffers,
						uint32 *extended_by)
{
	BlockNumber first_block;
	IOContext	io_context = IOContextForStrategy(strategy);
	instr_time	io_start;

	LimitAdditionalPins(&extend_by);

	/*
	 * 不持有扩展锁就为扩展取得牺牲缓冲区。
	 * 写出牺牲缓冲区是扩展关系最贵的部分，
	 * 尤其是当这样做需要 WAL 刷写时。把缓冲区
	 * 清零也很贵，因此也在持有
	 * 扩展锁之前做。
	 *
	 * 这些页由我们钉住且无效。我们持有钉住期间，
	 * 它们不能被另一个后端当作牺牲缓冲区取得。
	 */
	for (uint32 i = 0; i < extend_by; i++)
	{
		Block		buf_block;

		buffers[i] = GetVictimBuffer(strategy, io_context);
		buf_block = BufHdrGetBlock(GetBufferDescriptor(buffers[i] - 1));

		/* 新缓冲区用零填充 */
		MemSet((char *) buf_block, 0, BLCKSZ);
	}

	/* 以防下面需要钉住一个已有缓冲区 */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	/*
	 * 对关系加锁以防止并发扩展，除非请求不要这样做。
	 *
	 * 所有分叉我们使用同一把扩展锁。这不必要地
	 * 有限制，但目前分叉的扩展发生得不够
	 * 频繁，不值得更细地加锁。
	 *
	 * 注意，我们取得锁时，另一个后端可能已经
	 * 扩展了该关系。
	 */
	if (!(flags & EB_SKIP_EXTENSION_LOCK))
	{
		LockRelationForExtension(bmr.rel, ExclusiveLock);
		if (bmr.rel)
			bmr.smgr = RelationGetSmgr(bmr.rel);
	}

	/*
	 * 若有请求，就使大小缓存失效，以便 smgrnblocks 去问
	 * 内核。
	 */
	if (flags & EB_CLEAR_SIZE_CACHE)
		bmr.smgr->smgr_cached_nblocks[fork] = InvalidBlockNumber;

	first_block = smgrnblocks(bmr.smgr, fork);

	/*
	 * 既然我们有了准确的关系大小，就检查调用者是否只想
	 * 让我们扩展到某个特定大小。若有并发
	 * 扩展，我们可能取得了过多的缓冲区，需要释放
	 * 它们。
	 */
	if (extend_upto != InvalidBlockNumber)
	{
		uint32		orig_extend_by = extend_by;

		if (first_block > extend_upto)
			extend_by = 0;
		else if ((uint64) first_block + extend_by > extend_upto)
			extend_by = extend_upto - first_block;

		for (uint32 i = extend_by; i < orig_extend_by; i++)
		{
			BufferDesc *buf_hdr = GetBufferDescriptor(buffers[i] - 1);

			/*
			 * 我们先前取得的牺牲缓冲区是干净且未使用的，
			 * 让它能很快被再次找到
			 */
			StrategyFreeBuffer(buf_hdr);
			UnpinBuffer(buf_hdr);
		}

		if (extend_by == 0)
		{
			if (!(flags & EB_SKIP_EXTENSION_LOCK))
				UnlockRelationForExtension(bmr.rel, ExclusiveLock);
			*extended_by = extend_by;
			return first_block;
		}
	}

	/* 若关系已经达到最大可能长度就失败 */
	if ((uint64) first_block + extend_by >= MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend relation %s beyond %u blocks",
						relpath(bmr.smgr->smgr_rlocator, fork),
						MaxBlockNumber)));

	/*
	 * 把缓冲区插入缓冲区表，并标为 IO_IN_PROGRESS。
	 *
	 * 这必须在我们扩展关系之前发生，因为一旦
	 * 我们扩展，其他后端就可以开始读入那些页。
	 */
	for (int i = 0; i < extend_by; i++)
	{
		Buffer		victim_buf = buffers[i];
		BufferDesc *victim_buf_hdr = GetBufferDescriptor(victim_buf - 1);
		BufferTag	tag;
		uint32		hash;
		LWLock	   *partition_lock;
		int			existing_id;

		InitBufferTag(&tag, &bmr.smgr->smgr_rlocator.locator, fork, first_block + i);
		hash = BufTableHashCode(&tag);
		partition_lock = BufMappingPartitionLock(hash);

		LWLockAcquire(partition_lock, LW_EXCLUSIVE);

		existing_id = BufTableInsert(&tag, hash, victim_buf_hdr->buf_id);

		/*
		 * 我们只在试图扩展
		 * 关系却发现已有缓冲区这一角落情形才到这里。这可能发生，
		 * 因为先前扩展关系的尝试失败了，并且
		 * mdread 对超出 EOF 的读不抱怨（当
		 * zero_damaged_pages 为 ON 时），因此先前试图读取超出
		 * EOF 的块可能留下一个有效的零填充缓冲区。
		 * 不幸的是，我们也见过这种情况，因为
		 * 有缺陷的 Linux 内核有时返回的 lseek(SEEK_END) 结果
		 * 没有计入最近的写。在那种情况下，
		 * 已有缓冲区会含有我们不想
		 * 覆盖的有效数据。由于合法情形应当总是留下
		 * 零填充的缓冲区，若不是 PageIsNew 就抱怨。
		 */
		if (existing_id >= 0)
		{
			BufferDesc *existing_hdr = GetBufferDescriptor(existing_id);
			Block		buf_block;
			bool		valid;

			/*
			 * 在释放分区锁之前钉住已有缓冲区，
			 * 防止它被逐出。
			 */
			valid = PinBuffer(existing_hdr, strategy);

			LWLockRelease(partition_lock);

			/*
			 * 我们先前取得的牺牲缓冲区是干净且未使用的，
			 * 让它能很快被再次找到
			 */
			StrategyFreeBuffer(victim_buf_hdr);
			UnpinBuffer(victim_buf_hdr);

			buffers[i] = BufferDescriptorGetBuffer(existing_hdr);
			buf_block = BufHdrGetBlock(existing_hdr);

			if (valid && !PageIsNew((Page) buf_block))
				ereport(ERROR,
						(errmsg("unexpected data beyond EOF in block %u of relation %s",
								existing_hdr->tag.blockNum, relpath(bmr.smgr->smgr_rlocator, fork)),
						 errhint("This has been seen to occur with buggy kernels; consider updating your system.")));

			/*
			 * 我们必须在成功之前做 smgr[zero]extend，否则该页
			 * 不会被内核保留，下一次 P_NEW 调用
			 * 会决定返回同一页。清除 BM_VALID 位，
			 * 做 StartBufferIO() 并继续。
			 *
			 * 循环以处理有人在我们清除它和 StartBufferIO 检查
			 * 它之间重新设置
			 * BM_VALID 的极小可能性。
			 */
			do
			{
				uint32		buf_state = LockBufHdr(existing_hdr);

				buf_state &= ~BM_VALID;
				UnlockBufHdr(existing_hdr, buf_state);
			} while (!StartBufferIO(existing_hdr, true));
		}
		else
		{
			uint32		buf_state;

			buf_state = LockBufHdr(victim_buf_hdr);

			/* 持有缓冲区描述符锁时做一些健全性检查 */
			Assert(!(buf_state & (BM_VALID | BM_TAG_VALID | BM_DIRTY | BM_JUST_DIRTIED)));
			Assert(BUF_STATE_GET_REFCOUNT(buf_state) == 1);

			victim_buf_hdr->tag = tag;

			buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;
			if (bmr.relpersistence == RELPERSISTENCE_PERMANENT || fork == INIT_FORKNUM)
				buf_state |= BM_PERMANENT;

			UnlockBufHdr(victim_buf_hdr, buf_state);

			LWLockRelease(partition_lock);

			/* XXX：可以把其中持锁的操作与上面的合并 */
			StartBufferIO(victim_buf_hdr, true);
		}
	}

	io_start = pgstat_prepare_io_time();

	/*
	 * 注意：若 smgrzeroextend 失败，我们会留下已分配
	 * 但未标为 BM_VALID 的缓冲区。下一次关系扩展
	 * 仍会选择同一个块号（因为关系在磁盘上没有
	 * 变长），因此以后扩展关系的尝试会找到
	 * 同样的缓冲区（若它们尚未被回收），但会直接回到
	 * 这里再试 smgrzeroextend。
	 *
	 * 全零页不需要设置校验和。
	 */
	smgrzeroextend(bmr.smgr, fork, first_block, extend_by, false);

	/*
	 * 释放文件扩展锁；现在别人可以再把
	 * 该关系扩展一些了。
	 *
	 * 此后我们去掉 IO_IN_PROGRESS，因为唤醒等待的后端可能
	 * 花掉可观的时间。
	 */
	if (!(flags & EB_SKIP_EXTENSION_LOCK))
		UnlockRelationForExtension(bmr.rel, ExclusiveLock);

	pgstat_count_io_op_time(IOOBJECT_RELATION, io_context, IOOP_EXTEND,
							io_start, extend_by);

	/* 设置 BM_VALID，结束 IO，并唤醒任何等待者 */
	for (int i = 0; i < extend_by; i++)
	{
		Buffer		buf = buffers[i];
		BufferDesc *buf_hdr = GetBufferDescriptor(buf - 1);
		bool		lock = false;

		if (flags & EB_LOCK_FIRST && i == 0)
			lock = true;
		else if (flags & EB_LOCK_TARGET)
		{
			Assert(extend_upto != InvalidBlockNumber);
			if (first_block + i + 1 == extend_upto)
				lock = true;
		}

		if (lock)
			LWLockAcquire(BufferDescriptorGetContentLock(buf_hdr), LW_EXCLUSIVE);

		TerminateBufferIO(buf_hdr, false, BM_VALID);
	}

	pgBufferUsage.shared_blks_written += extend_by;

	*extended_by = extend_by;

	return first_block;
}

/*
 * MarkBufferDirty
 *
 *		把缓冲区内容标为脏页（实际写发生在稍后）。
 *
 * 缓冲区必须被钉住并排他锁定。（若调用者不持有
 * 排他锁，就可能有人正在写出该缓冲区，
 * 导致坏数据被写到磁盘的风险。）
 */
void
MarkBufferDirty(Buffer buffer)
{
	BufferDesc *bufHdr;
	uint32		buf_state;
	uint32		old_buf_state;

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	if (BufferIsLocal(buffer))
	{
		MarkLocalBufferDirty(buffer);
		return;
	}

	bufHdr = GetBufferDescriptor(buffer - 1);

	Assert(BufferIsPinned(buffer));
	Assert(LWLockHeldByMeInMode(BufferDescriptorGetContentLock(bufHdr),
								LW_EXCLUSIVE));

	old_buf_state = pg_atomic_read_u32(&bufHdr->state);
	for (;;)
	{
		if (old_buf_state & BM_LOCKED)
			old_buf_state = WaitBufHdrUnlocked(bufHdr);

		buf_state = old_buf_state;

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
		buf_state |= BM_DIRTY | BM_JUST_DIRTIED;

		if (pg_atomic_compare_exchange_u32(&bufHdr->state, &old_buf_state,
										   buf_state))
			break;
	}

	/*
	 * 若该缓冲区原先不是脏页，就做 vacuum 记账。
	 */
	if (!(old_buf_state & BM_DIRTY))
	{
		VacuumPageDirty++;
		pgBufferUsage.shared_blks_dirtied++;
		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageDirty;
	}
}

/*
 * ReleaseAndReadBuffer -- 把 ReleaseBuffer() 和 ReadBuffer() 合在一起。
 *
 * 从前，与分别调用这两个例程相比，这省掉了一轮
 * 取得和释放 BufMgrLock。现在它主要只是
 * 一个便捷函数。不过，若传入的缓冲区有效且
 * 已经包含想要的块，我们就原样返回它；这与完整的
 * 释放再重新取得相比，确实省掉相当多的工作。
 *
 * 注意：可以传入 buffer == InvalidBuffer，表示实际上不需要
 * 释放旧缓冲区。这种情形与 ReadBuffer 相同，
 * 但可以给调用者省掉一些测试。
 */
Buffer
ReleaseAndReadBuffer(Buffer buffer,
					 Relation relation,
					 BlockNumber blockNum)
{
	ForkNumber	forkNum = MAIN_FORKNUM;
	BufferDesc *bufHdr;

	if (BufferIsValid(buffer))
	{
		Assert(BufferIsPinned(buffer));
		if (BufferIsLocal(buffer))
		{
			bufHdr = GetLocalBufferDescriptor(-buffer - 1);
			if (bufHdr->tag.blockNum == blockNum &&
				BufTagMatchesRelFileLocator(&bufHdr->tag, &relation->rd_locator) &&
				BufTagGetForkNum(&bufHdr->tag) == forkNum)
				return buffer;
			UnpinLocalBuffer(buffer);
		}
		else
		{
			bufHdr = GetBufferDescriptor(buffer - 1);
			/* 我们有钉住，因此不持自旋锁检查标签没问题 */
			if (bufHdr->tag.blockNum == blockNum &&
				BufTagMatchesRelFileLocator(&bufHdr->tag, &relation->rd_locator) &&
				BufTagGetForkNum(&bufHdr->tag) == forkNum)
				return buffer;
			UnpinBuffer(bufHdr);
		}
	}

	return ReadBuffer(relation, blockNum);
}

/*
 * PinBuffer -- 使缓冲区不能被替换。
 *
 * 对默认访问策略，我们第一次钉住它时增加该缓冲区的 usage_count；
 * 对其他策略，我们只确保 usage_count
 * 不是零。（后者的想法是，我们不希望同步的
 * 堆扫描把计数抬高，但需要它不是零，以劝阻
 * 其他后端从我们的环里偷缓冲区。只要我们循环
 * 遍历环的速度快于全局时钟清扫，我们环里的缓冲区
 * 就不会被其他后端选作替换的牺牲者。）
 *
 * 这只应当用于共享缓冲区，决不要用于本地缓冲区。
 *
 * 由于缓冲区被钉住和解除钉住非常频繁，钉住缓冲区时不
 * 拿缓冲区描述符锁；而是在 CAS 操作的循环里更新状态变量。
 * 希望只是一次 CAS。
 *
 * 注意 ResourceOwnerEnlargeBuffers 必须已经做过。
 *
 * 若缓冲区是 BM_VALID 就返回真，否则返回假。这项规定让
 * 某些调用者避免额外一轮自旋锁。
 */
static bool
PinBuffer(BufferDesc *buf, BufferAccessStrategy strategy)
{
	Buffer		b = BufferDescriptorGetBuffer(buf);
	bool		result;
	PrivateRefCountEntry *ref;

	Assert(!BufferIsLocal(b));

	ref = GetPrivateRefCountEntry(b, true);

	if (ref == NULL)
	{
		uint32		buf_state;
		uint32		old_buf_state;

		ReservePrivateRefCountEntry();
		ref = NewPrivateRefCountEntry(b);

		old_buf_state = pg_atomic_read_u32(&buf->state);
		for (;;)
		{
			if (old_buf_state & BM_LOCKED)
				old_buf_state = WaitBufHdrUnlocked(buf);

			buf_state = old_buf_state;

			/* 增加引用计数 */
			buf_state += BUF_REFCOUNT_ONE;

			if (strategy == NULL)
			{
				/* 默认情形：除非已经最大，否则增加使用计数。 */
				if (BUF_STATE_GET_USAGECOUNT(buf_state) < BM_MAX_USAGE_COUNT)
					buf_state += BUF_USAGECOUNT_ONE;
			}
			else
			{
				/*
				 * 环缓冲区不应当把别人从池里逐出。因此我们
				 * 不把使用计数弄到超过 1。
				 */
				if (BUF_STATE_GET_USAGECOUNT(buf_state) == 0)
					buf_state += BUF_USAGECOUNT_ONE;
			}

			if (pg_atomic_compare_exchange_u32(&buf->state, &old_buf_state,
											   buf_state))
			{
				result = (buf_state & BM_VALID) != 0;

				/*
				 * 为了让事情简单，假定我们为 Valgrind 缓冲区客户端检查
				 * 取得了一次缓冲区钉住（即使在 !result 情形下也如此）。
				 * 无论如何，不安全访问的缓冲区
				 * 一般并不保证被标为未定义或
				 * 不可访问。
				 */
				VALGRIND_MAKE_MEM_DEFINED(BufHdrGetBlock(buf), BLCKSZ);
				break;
			}
		}
	}
	else
	{
		/*
		 * 若我们先前钉住过该缓冲区，它肯定有效。
		 *
		 * 注意：我们有意避免这里的 Valgrind 客户端请求。
		 * 各个访问方法可以有选择地在我们的客户端请求之上
		 * 叠加缓冲区页客户端请求，以强制缓冲区只在锁定
		 * （且钉住）时被访问。这里缓冲区页合法地
		 * 不可访问是可能的。我们
		 * 无法插手那件事。
		 */
		result = true;
	}

	ref->refcount++;
	Assert(ref->refcount > 0);
	ResourceOwnerRememberBuffer(CurrentResourceOwner, b);
	return result;
}

/*
 * PinBuffer_Locked -- 同上，但调用者已经锁住了缓冲区描述符。
 * 返回前释放自旋锁。
 *
 * 由于本函数在持有自旋锁时调用，调用者必须
 * 事先调用 ReservePrivateRefCountEntry()。
 *
 * 目前，本函数的调用者都不想修改该缓冲区的
 * usage_count，因此不需要策略参数。
 * 我们也不费心做 BM_VALID 测试（调用者可以自己
 * 检查）。
 *
 * 而且所有调用者只在已知该
 * 缓冲区不可能已有本后端的钉住时使用本函数。这让我们可以跳过
 * 搜索私有引用计数数组和散列表，这是好事，因为
 * 自旋锁仍被持有。
 *
 * 注意：使用本例程常常是强制的，不只是为了省掉一次
 * 自旋锁加锁与解锁，因为我们需要在
 * 它的状态能在我们手下改变之前钉住缓冲区。
 */
static void
PinBuffer_Locked(BufferDesc *buf)
{
	Buffer		b;
	PrivateRefCountEntry *ref;
	uint32		buf_state;

	/*
	 * 如所解释的，我们不预期任何已有的钉住。这让我们可以在释放自旋锁之后
	 * 操作 PrivateRefCount。
	 */
	Assert(GetPrivateRefCountEntry(BufferDescriptorGetBuffer(buf), false) == NULL);

	/*
	 * 缓冲区不可能已有钉住，因此把它的页对
	 * Valgrind 标为已定义（这类似于 PinBuffer() 里后端
	 * 尚未持有缓冲区钉住的情形）
	 */
	VALGRIND_MAKE_MEM_DEFINED(BufHdrGetBlock(buf), BLCKSZ);

	/*
	 * 由于我们持有缓冲区自旋锁，我们可以更新缓冲区状态并
	 * 在一次操作里释放锁。
	 */
	buf_state = pg_atomic_read_u32(&buf->state);
	Assert(buf_state & BM_LOCKED);
	buf_state += BUF_REFCOUNT_ONE;
	UnlockBufHdr(buf, buf_state);

	b = BufferDescriptorGetBuffer(buf);

	ref = NewPrivateRefCountEntry(b);
	ref->refcount++;

	ResourceOwnerRememberBuffer(CurrentResourceOwner, b);
}

/*
 * UnpinBuffer -- 使缓冲区可以被替换。
 *
 * 这只应当用于共享缓冲区，决不要用于本地缓冲区。这
 * 总是调整 CurrentResourceOwner。
 */
static void
UnpinBuffer(BufferDesc *buf)
{
	PrivateRefCountEntry *ref;
	Buffer		b = BufferDescriptorGetBuffer(buf);

	Assert(!BufferIsLocal(b));

	/* 不移动，因为我们很可能马上就要删掉它 */
	ref = GetPrivateRefCountEntry(b, false);
	Assert(ref != NULL);

	ResourceOwnerForgetBuffer(CurrentResourceOwner, b);

	Assert(ref->refcount > 0);
	ref->refcount--;
	if (ref->refcount == 0)
	{
		uint32		buf_state;
		uint32		old_buf_state;

		/*
		 * 把缓冲区对 Valgrind 标为不可访问。
		 *
		 * 注意，缓冲区可能已经在强制缓冲区只在持有缓冲区锁时
		 * 才能访问的访问方法代码里被标为
		 * 不可访问。
		 */
		VALGRIND_MAKE_MEM_NOACCESS(BufHdrGetBlock(buf), BLCKSZ);

		/* 我最好不要仍持有缓冲区内容锁 */
		Assert(!LWLockHeldByMe(BufferDescriptorGetContentLock(buf)));

		/*
		 * 减少共享引用计数。
		 *
		 * 由于缓冲区自旋锁持有者可以只用写来更新状态，
		 * 这里用原子减不安全；因此用 CAS 循环。
		 */
		old_buf_state = pg_atomic_read_u32(&buf->state);
		for (;;)
		{
			if (old_buf_state & BM_LOCKED)
				old_buf_state = WaitBufHdrUnlocked(buf);

			buf_state = old_buf_state;

			buf_state -= BUF_REFCOUNT_ONE;

			if (pg_atomic_compare_exchange_u32(&buf->state, &old_buf_state,
											   buf_state))
				break;
		}

		/* 支持 LockBufferForCleanup() */
		if (buf_state & BM_PIN_COUNT_WAITER)
		{
			/*
			 * 取得缓冲区描述符锁，重新检查是否有等待者。
			 * 另一个后端可能已经解除了该缓冲区的钉住，并且已经
			 * 唤醒了等待者。我们上面解除钉住之后，没有该缓冲区被
			 * 替换的危险，因为它被
			 * 等待者钉住。
			 */
			buf_state = LockBufHdr(buf);

			if ((buf_state & BM_PIN_COUNT_WAITER) &&
				BUF_STATE_GET_REFCOUNT(buf_state) == 1)
			{
				/* 我们刚刚释放了除等待者之外的最后一个钉住 */
				int			wait_backend_pgprocno = buf->wait_backend_pgprocno;

				buf_state &= ~BM_PIN_COUNT_WAITER;
				UnlockBufHdr(buf, buf_state);
				ProcSendSignal(wait_backend_pgprocno);
			}
			else
				UnlockBufHdr(buf, buf_state);
		}
		ForgetPrivateRefCountEntry(ref);
	}
}

#define ST_SORT sort_checkpoint_bufferids
#define ST_ELEMENT_TYPE CkptSortItem
#define ST_COMPARE(a, b) ckpt_buforder_comparator(a, b)
#define ST_SCOPE static
#define ST_DEFINE
#include <lib/sort_template.h>

/*
 * BufferSync -- 把池里全部脏页写出。
 *
 * 这在检查点时调用，写出全部脏的共享缓冲区。
 * 应当传入检查点请求标志。若设置了 CHECKPOINT_IMMEDIATE，
 * 我们就关掉写之间的延迟；若设置了 CHECKPOINT_IS_SHUTDOWN、
 * CHECKPOINT_END_OF_RECOVERY 或 CHECKPOINT_FLUSH_ALL，我们甚至写出
 * 未日志记录的缓冲区，否则会跳过它们。其余标志
 * 目前在这里没有作用。
 */
static void
BufferSync(int flags)
{
	uint32		buf_state;
	int			buf_id;
	int			num_to_scan;
	int			num_spaces;
	int			num_processed;
	int			num_written;
	CkptTsStatus *per_ts_stat = NULL;
	Oid			last_tsid;
	binaryheap *ts_heap;
	int			i;
	int			mask = BM_DIRTY;
	WritebackContext wb_context;

	/* 确保我们能处理 SyncOneBuffer 内部的钉住 */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	/*
	 * 除非这是关闭检查点，或者我们被明确告知，
	 * 我们只写永久缓冲区里的脏页。但在关闭或恢复
	 * 结束时，我们写出全部脏页。
	 */
	if (!((flags & (CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_END_OF_RECOVERY |
					CHECKPOINT_FLUSH_ALL))))
		mask |= BM_PERMANENT;

	/*
	 * 遍历全部缓冲区，把需要写出的那些标上
	 * BM_CHECKPOINT_NEEDED。一边走一边计数（num_to_scan），以便
	 * 估计需要做多少工作。
	 *
	 * 这让我们只写检查点开始时就是脏页的那些页，
	 * 而不写检查点进行期间变脏的那些。
	 * 每当带有 BM_CHECKPOINT_NEEDED 的页被写出，无论是我们
	 * 稍后在本函数里，还是普通后端或 bgwriter 的清理
	 * 扫描，该标志都被清除。此后变脏的任何缓冲区都不会
	 * 带有该标志。
	 *
	 * 注意，若我们没能写出某个缓冲区，可能会留下仍设置着
	 * BM_CHECKPOINT_NEEDED 的缓冲区。这没问题，因为任何这样的缓冲区
	 * 下一次检查点尝试肯定也需要写出。
	 */
	num_to_scan = 0;
	for (buf_id = 0; buf_id < NBuffers; buf_id++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(buf_id);

		/*
		 * 检查 BM_DIRTY 有缓冲区描述符自旋锁就够了，见
		 * SyncOneBuffer 里的注释。
		 */
		buf_state = LockBufHdr(bufHdr);

		if ((buf_state & mask) == mask)
		{
			CkptSortItem *item;

			buf_state |= BM_CHECKPOINT_NEEDED;

			item = &CkptBufferIds[num_to_scan++];
			item->buf_id = buf_id;
			item->tsId = bufHdr->tag.spcOid;
			item->relNumber = BufTagGetRelNumber(&bufHdr->tag);
			item->forkNum = BufTagGetForkNum(&bufHdr->tag);
			item->blockNum = bufHdr->tag.blockNum;
		}

		UnlockBufHdr(bufHdr, buf_state);

		/* 以防 NBuffers 很大，检查屏障事件。 */
		if (ProcSignalBarrierPending)
			ProcessProcSignalBarrier();
	}

	if (num_to_scan == 0)
		return;					/* 无事可做 */

	WritebackContextInit(&wb_context, &checkpoint_flush_after);

	TRACE_POSTGRESQL_BUFFER_SYNC_START(NBuffers, num_to_scan);

	/*
	 * 对需要写出的缓冲区排序，以降低随机
	 * IO 的可能性。排序对在表空间之间平衡
	 * 写的实现也很重要。没有写平衡，我们可能会
	 * 最终一个表空间一个表空间地写；可能使底层系统
	 * 过载。
	 */
	sort_checkpoint_bufferids(CkptBufferIds, num_to_scan);

	num_spaces = 0;

	/*
	 * 为每个有需要刷写的缓冲区的表空间分配进度状态。
	 * 这需要待刷写数组已经排好序。
	 */
	last_tsid = InvalidOid;
	for (i = 0; i < num_to_scan; i++)
	{
		CkptTsStatus *s;
		Oid			cur_tsid;

		cur_tsid = CkptBufferIds[i].tsId;

		/*
		 * 每当发现新的表空间，就增长
		 * 每表空间状态结构的数组。
		 */
		if (last_tsid == InvalidOid || last_tsid != cur_tsid)
		{
			Size		sz;

			num_spaces++;

			/*
			 * 这里不值得加按 2 的幂增长的逻辑，即使有
			 * 几百个表空间也应当没问题。
			 */
			sz = sizeof(CkptTsStatus) * num_spaces;

			if (per_ts_stat == NULL)
				per_ts_stat = (CkptTsStatus *) palloc(sz);
			else
				per_ts_stat = (CkptTsStatus *) repalloc(per_ts_stat, sz);

			s = &per_ts_stat[num_spaces - 1];
			memset(s, 0, sizeof(*s));
			s->tsId = cur_tsid;

			/*
			 * 该表空间里的第一个缓冲区。由于 CkptBufferIds 按
			 * 表空间排序，该表空间里全部 (s->num_to_scan) 个缓冲区
			 * 都会跟在后面。
			 */
			s->index = i;

			/*
			 * 一旦我们知道每个表空间里有多少缓冲区，
			 * 即本循环之后，就会确定 progress_slice。
			 */

			last_tsid = cur_tsid;
		}
		else
		{
			s = &per_ts_stat[num_spaces - 1];
		}

		s->num_to_scan++;

		/* 检查屏障事件。 */
		if (ProcSignalBarrierPending)
			ProcessProcSignalBarrier();
	}

	Assert(num_spaces > 0);

	/*
	 * 在各个表空间的写出进度上建一个最小堆，
	 * 并计算单个已处理缓冲区占
	 * 总进度的多大一份。
	 */
	ts_heap = binaryheap_allocate(num_spaces,
								  ts_ckpt_progress_comparator,
								  NULL);

	for (i = 0; i < num_spaces; i++)
	{
		CkptTsStatus *ts_stat = &per_ts_stat[i];

		ts_stat->progress_slice = (float8) num_to_scan / ts_stat->num_to_scan;

		binaryheap_add_unordered(ts_heap, PointerGetDatum(ts_stat));
	}

	binaryheap_build(ts_heap);

	/*
	 * 遍历待检查点的缓冲区，写出那些（仍然）
	 * 标有 BM_CHECKPOINT_NEEDED 的。写在表空间之间平衡；
	 * 否则排序会导致一次只有一个表空间
	 * 接收写，不能有效利用硬件。
	 */
	num_processed = 0;
	num_written = 0;
	while (!binaryheap_empty(ts_heap))
	{
		BufferDesc *bufHdr = NULL;
		CkptTsStatus *ts_stat = (CkptTsStatus *)
			DatumGetPointer(binaryheap_first(ts_heap));

		buf_id = CkptBufferIds[ts_stat->index].buf_id;
		Assert(buf_id != -1);

		bufHdr = GetBufferDescriptor(buf_id);

		num_processed++;

		/*
		 * 这里不需要取得锁，因为我们只看
		 * 一个位。别人可能在我们检查之后马上写出该缓冲区
		 * 并清除该标志，但这没关系，
		 * 因为 SyncOneBuffer 那时会什么也不做。不过还有
		 * 进一步的竞态：可以设想，在我们
		 * 在这里检查该位和 SyncOneBuffer 取得锁之间，
		 * 别人不仅写出了该缓冲区，还把它换成了另一
		 * 页并弄脏了它。在那种不大可能的情况下，SyncOneBuffer 会
		 * 写出该缓冲区，尽管我们并不需要。不过，似乎不值得
		 * 防范这一点。
		 */
		if (pg_atomic_read_u32(&bufHdr->state) & BM_CHECKPOINT_NEEDED)
		{
			if (SyncOneBuffer(buf_id, false, &wb_context) & BUF_WRITTEN)
			{
				TRACE_POSTGRESQL_BUFFER_SYNC_WRITTEN(buf_id);
				PendingCheckpointerStats.buf_written_checkpoints++;
				num_written++;
			}
		}

		/*
		 * 独立于实际是否必须刷写该缓冲区来衡量进度，
		 * 否则写会变得不平衡。
		 */
		ts_stat->progress += ts_stat->progress_slice;
		ts_stat->num_scanned++;
		ts_stat->index++;

		/* 该表空间的缓冲区是否都已处理？ */
		if (ts_stat->num_scanned == ts_stat->num_to_scan)
		{
			binaryheap_remove_first(ts_heap);
		}
		else
		{
			/* 用新进度更新堆 */
			binaryheap_replace_first(ts_heap, PointerGetDatum(ts_stat));
		}

		/*
		 * 睡眠以限制我们的 I/O 速率。
		 *
		 * （即使它不睡眠，这也会检查屏障事件。）
		 */
		CheckpointWriteDelay(flags, (double) num_processed / num_to_scan);
	}

	/*
	 * 发出全部待处理的刷写。只有检查点进程调用 BufferSync()，因此
	 * IOContext 将始终是 IOCONTEXT_NORMAL。
	 */
	IssuePendingWritebacks(&wb_context, IOCONTEXT_NORMAL);

	pfree(per_ts_stat);
	per_ts_stat = NULL;
	binaryheap_free(ts_heap);

	/*
	 * 更新检查点统计信息。如上所述，这不包括
	 * 其他后端或 bgwriter 扫描写出的缓冲区。
	 */
	CheckpointStats.ckpt_bufs_written += num_written;

	TRACE_POSTGRESQL_BUFFER_SYNC_DONE(NBuffers, num_written, num_to_scan);
}

/*
 * BgBufferSync -- 写出池里的一些脏页。
 *
 * 这由后台写进程周期性地调用。
 *
 * 若 bgwriter 进程进入低功耗休眠模式是合适的，就返回真。
 * （若策略时钟清扫已经被套圈，且最近没有发生缓冲区分配，
 * 或者通过把
 * bgwriter_lru_maxpages 设为 0
 * 而实际上禁用了 bgwriter，就会发生这种情况。）
 */
bool
BgBufferSync(WritebackContext *wb_context)
{
	/* 从 freelist.c 得到的信息 */
	int			strategy_buf_id;
	uint32		strategy_passes;
	uint32		recent_alloc;

	/*
	 * 调用之间保存的信息，以便我们确定策略
	 * 点的前进速率，并避免扫描已经清理过的缓冲区。
	 */
	static bool saved_info_valid = false;
	static int	prev_strategy_buf_id;
	static uint32 prev_strategy_passes;
	static int	next_to_clean;
	static uint32 next_passes;

	/* 分配率和干净缓冲区密度的移动平均 */
	static float smoothed_alloc = 0;
	static float smoothed_density = 10.0;

	/* 这些也许可以做成可调参数，但目前不是 */
	float		smoothing_samples = 16;
	float		scan_whole_pool_milliseconds = 120000.0;

	/* 用来计算我们向前扫描多远 */
	long		strategy_delta;
	int			bufs_to_lap;
	int			bufs_ahead;
	float		scans_per_alloc;
	int			reusable_buffers_est;
	int			upcoming_alloc_est;
	int			min_scan_buffers;

	/* 扫描循环本身的变量 */
	int			num_to_scan;
	int			num_written;
	int			reusable_buffers;

	/* 最终 smoothed_density 更新的变量 */
	long		new_strategy_delta;
	uint32		new_recent_alloc;

	/*
	 * 找出空闲链表时钟清扫目前在哪里，以及自上次调用以来
	 * 发生了多少次缓冲区分配。
	 */
	strategy_buf_id = StrategySyncStart(&strategy_passes, &recent_alloc);

	/* 向 pgstat 报告缓冲区分配计数 */
	PendingBgWriterStats.buf_alloc += recent_alloc;

	/*
	 * 若我们不在跑 LRU 扫描，做完统计
	 * 就停。我们把保存的状态标为无效，以便以后重新打开
	 * LRU 扫描时能正常恢复。
	 */
	if (bgwriter_lru_maxpages <= 0)
	{
		saved_info_valid = false;
		return true;
	}

	/*
	 * 计算 strategy_delta，即自上次以来时钟清扫扫描了多少缓冲区。
	 * 若是第一次通过，就假定一个都没有。然后
	 * 看我们是否仍领先于时钟清扫，若是，在追上它并套圈之前
	 * 还能扫描多少缓冲区。注意：
	 * xxx_passes 比较看起来奇怪的写法是为了避免遍数回绕时的
	 * 虚假行为。
	 */
	if (saved_info_valid)
	{
		int32		passes_delta = strategy_passes - prev_strategy_passes;

		strategy_delta = strategy_buf_id - prev_strategy_buf_id;
		strategy_delta += (long) passes_delta * NBuffers;

		Assert(strategy_delta >= 0);

		if ((int32) (next_passes - strategy_passes) > 0)
		{
			/* 我们比策略点领先一圈 */
			bufs_to_lap = strategy_buf_id - next_to_clean;
#ifdef BGW_DEBUG
			elog(DEBUG2, "bgwriter ahead: bgw %u-%u strategy %u-%u delta=%ld lap=%d",
				 next_passes, next_to_clean,
				 strategy_passes, strategy_buf_id,
				 strategy_delta, bufs_to_lap);
#endif
		}
		else if (next_passes == strategy_passes &&
				 next_to_clean >= strategy_buf_id)
		{
			/* 在同一圈上，但领先，或至少不落后 */
			bufs_to_lap = NBuffers - (next_to_clean - strategy_buf_id);
#ifdef BGW_DEBUG
			elog(DEBUG2, "bgwriter ahead: bgw %u-%u strategy %u-%u delta=%ld lap=%d",
				 next_passes, next_to_clean,
				 strategy_passes, strategy_buf_id,
				 strategy_delta, bufs_to_lap);
#endif
		}
		else
		{
			/*
			 * 我们落后了，因此向前跳到策略点，并从那里开始
			 * 清理。
			 */
#ifdef BGW_DEBUG
			elog(DEBUG2, "bgwriter behind: bgw %u-%u strategy %u-%u delta=%ld",
				 next_passes, next_to_clean,
				 strategy_passes, strategy_buf_id,
				 strategy_delta);
#endif
			next_to_clean = strategy_buf_id;
			next_passes = strategy_passes;
			bufs_to_lap = NBuffers;
		}
	}
	else
	{
		/*
		 * 在启动时初始化，或在 LRU 扫描曾经关闭之后。始终
		 * 从策略点开始。
		 */
#ifdef BGW_DEBUG
		elog(DEBUG2, "bgwriter initializing: strategy %u-%u",
			 strategy_passes, strategy_buf_id);
#endif
		strategy_delta = 0;
		next_to_clean = strategy_buf_id;
		next_passes = strategy_passes;
		bufs_to_lap = NBuffers;
	}

	/* 更新保存的信息供下次使用 */
	prev_strategy_buf_id = strategy_buf_id;
	prev_strategy_passes = strategy_passes;
	saved_info_valid = true;

	/*
	 * 计算每次新分配必须扫描多少缓冲区，即
	 * 可重用缓冲区密度的倒数，并跟踪它的移动平均。
	 *
	 * 若策略点没动，我们就不更新密度估计
	 */
	if (strategy_delta > 0 && recent_alloc > 0)
	{
		scans_per_alloc = (float) strategy_delta / (float) recent_alloc;
		smoothed_density += (scans_per_alloc - smoothed_density) /
			smoothing_samples;
	}

	/*
	 * 根据平滑后的密度估计，估计当前
	 * 策略点和我们已经向前扫描到的地方之间有多少可重用
	 * 缓冲区。
	 */
	bufs_ahead = NBuffers - bufs_to_lap;
	reusable_buffers_est = (float) bufs_ahead / smoothed_density;

	/*
	 * 跟踪最近缓冲区分配的移动平均。这里我们要的不是
	 * 真正的平均，而是快升慢降：我们
	 * 立刻跟随任何增加。
	 */
	if (smoothed_alloc <= (float) recent_alloc)
		smoothed_alloc = recent_alloc;
	else
		smoothed_alloc += ((float) recent_alloc - smoothed_alloc) /
			smoothing_samples;

	/* 用一个 GUC 缩放该估计，以便更激进地调节。 */
	upcoming_alloc_est = (int) (smoothed_alloc * bgwriter_lru_multiplier);

	/*
	 * 若 recent_alloc 许多周期都保持为零，smoothed_alloc 最终会
	 * 下溢到零，而下溢在某些平台上产生烦人的
	 * 内核警告。一旦 upcoming_alloc_est 变成
	 * 零，再跟踪越来越小的 smoothed_alloc 就没有意义，
	 * 因此把它正好重置为零以避免这种
	 * 症状。recent_alloc 一增加，它就会弹回来。
	 */
	if (upcoming_alloc_est == 0)
		smoothed_alloc = 0;

	/*
	 * 即使缓冲区分配活动很少或没有，我们也想
	 * 在缓冲区缓存里取得少量进展，以便空闲
	 * 一段时间后尽可能多的可重用缓冲区
	 * 是干净的。
	 *
	 * （scan_whole_pool_milliseconds / BgWriterDelay）计算在 scan_whole_pool 时间内
	 * BGW 会被调用多少次；把缓冲池切成
	 * 那么多段。
	 */
	min_scan_buffers = (int) (NBuffers / (scan_whole_pool_milliseconds / BgWriterDelay));

	if (upcoming_alloc_est < (min_scan_buffers + reusable_buffers_est))
	{
#ifdef BGW_DEBUG
		elog(DEBUG2, "bgwriter: alloc_est=%d too small, using min=%d + reusable_est=%d",
			 upcoming_alloc_est, min_scan_buffers, reusable_buffers_est);
#endif
		upcoming_alloc_est = min_scan_buffers + reusable_buffers_est;
	}

	/*
	 * 现在写出可重用的脏页，从 next_to_clean 点向前工作，
	 * 直到我们套过策略扫描，或清理了足够的缓冲区以匹配我们对下一周期分配
	 * 需求的估计，或达到
	 * bgwriter_lru_maxpages 限制。
	 */

	/* 确保我们能处理 SyncOneBuffer 内部的钉住 */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	num_to_scan = bufs_to_lap;
	num_written = 0;
	reusable_buffers = reusable_buffers_est;

	/* 执行 LRU 扫描 */
	while (num_to_scan > 0 && reusable_buffers < upcoming_alloc_est)
	{
		int			sync_state = SyncOneBuffer(next_to_clean, true,
											   wb_context);

		if (++next_to_clean >= NBuffers)
		{
			next_to_clean = 0;
			next_passes++;
		}
		num_to_scan--;

		if (sync_state & BUF_WRITTEN)
		{
			reusable_buffers++;
			if (++num_written >= bgwriter_lru_maxpages)
			{
				PendingBgWriterStats.maxwritten_clean++;
				break;
			}
		}
		else if (sync_state & BUF_REUSABLE)
			reusable_buffers++;
	}

	PendingBgWriterStats.buf_written_clean += num_written;

#ifdef BGW_DEBUG
	elog(DEBUG1, "bgwriter: recent_alloc=%u smoothed=%.2f delta=%ld ahead=%d density=%.2f reusable_est=%d upcoming_est=%d scanned=%d wrote=%d reusable=%d",
		 recent_alloc, smoothed_alloc, strategy_delta, bufs_ahead,
		 smoothed_density, reusable_buffers_est, upcoming_alloc_est,
		 bufs_to_lap - num_to_scan,
		 num_written,
		 reusable_buffers - reusable_buffers_est);
#endif

	/*
	 * 把上面的扫描看作一次新的分配扫描。
	 * 刻画它的密度，并据此更新平滑后的那个。
	 * 在策略和后台写进程都在做一些有用扫描的情形下，
	 * 这实际上把移动平均周期减半，
	 * 这有帮助，因为密度估计上的长记忆
	 * 并不那么可取。
	 */
	new_strategy_delta = bufs_to_lap - num_to_scan;
	new_recent_alloc = reusable_buffers - reusable_buffers_est;
	if (new_strategy_delta > 0 && new_recent_alloc > 0)
	{
		scans_per_alloc = (float) new_strategy_delta / (float) new_recent_alloc;
		smoothed_density += (scans_per_alloc - smoothed_density) /
			smoothing_samples;

#ifdef BGW_DEBUG
		elog(DEBUG2, "bgwriter: cleaner density alloc=%u scan=%ld density=%.2f new smoothed=%.2f",
			 new_recent_alloc, new_strategy_delta,
			 scans_per_alloc, smoothed_density);
#endif
	}

	/* 若可以休眠就返回真 */
	return (bufs_to_lap == 0 && recent_alloc == 0);
}

/*
 * SyncOneBuffer -- 同步期间处理单个缓冲区。
 *
 * 若 skip_recently_used 为真，我们不写当前被钉住的缓冲区，也不写
 * 标为最近使用的缓冲区，因为它们不是替换候选。
 *
 * 返回包含下列标志位的位掩码：
 *	BUF_WRITTEN：我们写出了该缓冲区。
 *	BUF_REUSABLE：缓冲区可供替换，即它的
 *		钉住计数为 0 且使用计数为 0。
 *
 * （若 FlushBuffer 锁住缓冲区后发现它是干净的，BUF_WRITTEN 可能被错误设置，
 * 但我们并不太在意。）
 *
 * 注意：调用者必须已经做过 ResourceOwnerEnlargeBuffers。
 */
static int
SyncOneBuffer(int buf_id, bool skip_recently_used, WritebackContext *wb_context)
{
	BufferDesc *bufHdr = GetBufferDescriptor(buf_id);
	int			result = 0;
	uint32		buf_state;
	BufferTag	tag;

	ReservePrivateRefCountEntry();

	/*
	 * 检查缓冲区是否需要写出。
	 *
	 * 只要我们在访问方法里把页标为脏页是在用
	 * XLogInsert() 记录变更之前，就可以不拿缓冲区内容锁做这项检查：
	 * 若有人刚好在我们检查之后把缓冲区弄脏，我们
	 * 不担心，因为我们的 checkpoint.redo 指向即将到来的变更的日志记录之前，
	 * 因此我们不必写出这样的脏页。
	 */
	buf_state = LockBufHdr(bufHdr);

	if (BUF_STATE_GET_REFCOUNT(buf_state) == 0 &&
		BUF_STATE_GET_USAGECOUNT(buf_state) == 0)
	{
		result |= BUF_REUSABLE;
	}
	else if (skip_recently_used)
	{
		/* 调用者告诉我们不要写最近使用的缓冲区 */
		UnlockBufHdr(bufHdr, buf_state);
		return result;
	}

	if (!(buf_state & BM_VALID) || !(buf_state & BM_DIRTY))
	{
		/* 它是干净的，因此无事可做 */
		UnlockBufHdr(bufHdr, buf_state);
		return result;
	}

	/*
	 * 钉住它，共享锁定它，写出它。（若我们锁住时缓冲区是干净的，
	 * FlushBuffer 会什么也不做。）
	 */
	PinBuffer_Locked(bufHdr);
	LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);

	FlushBuffer(bufHdr, NULL, IOOBJECT_RELATION, IOCONTEXT_NORMAL);

	LWLockRelease(BufferDescriptorGetContentLock(bufHdr));

	tag = bufHdr->tag;

	UnpinBuffer(bufHdr);

	/*
	 * SyncOneBuffer() 只由检查点进程和 bgwriter 调用，因此
	 * IOContext 将始终是 IOCONTEXT_NORMAL。
	 */
	ScheduleBufferTagForWriteback(wb_context, IOCONTEXT_NORMAL, &tag);

	return result | BUF_WRITTEN;
}

/*
 *		AtEOXact_Buffers - 在事务结束时清理。
 *
 *		自 PostgreSQL 8.0 起，缓冲区钉住应当由
 *		ResourceOwner 机制释放。本例程只是一个调试用的
 *		交叉检查，确认没有钉住残留。
 */
void
AtEOXact_Buffers(bool isCommit)
{
	CheckForBufferLeaks();

	AtEOXact_LocalBuffers(isCommit);

	Assert(PrivateRefCountOverflowed == 0);
}

/*
 * 初始化对共享缓冲区缓冲池的访问。
 *
 * 这在后端启动期间调用（无论独立还是在
 * postmaster 之下）。它为该后端访问已经存在的
 * 缓冲池做准备。
 */
void
InitBufferPoolAccess(void)
{
	HASHCTL		hash_ctl;

	memset(&PrivateRefCountArray, 0, sizeof(PrivateRefCountArray));

	hash_ctl.keysize = sizeof(int32);
	hash_ctl.entrysize = sizeof(PrivateRefCountEntry);

	PrivateRefCountHash = hash_create("PrivateRefCount", 100, &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);

	/*
	 * AtProcExit_Buffers 需要 LWLock 访问，因此必须在
	 * 后端关闭的相应阶段调用。
	 */
	Assert(MyProc != NULL);
	on_shmem_exit(AtProcExit_Buffers, 0);
}

/*
 * 后端退出期间，确保我们释放了全部共享缓冲区锁，
 * 并断言我们没有残留的钉住。
 */
static void
AtProcExit_Buffers(int code, Datum arg)
{
	UnlockBuffers();

	CheckForBufferLeaks();

	/* localbuf.c 也需要一个机会 */
	AtProcExit_LocalBuffers();
}

/*
 *		CheckForBufferLeaks - 确保本后端不持有缓冲区钉住。
 *
 *		自 PostgreSQL 8.0 起，缓冲区钉住应当由
 *		ResourceOwner 机制释放。本例程只是一个调试用的
 *		交叉检查，确认没有钉住残留。
 */
static void
CheckForBufferLeaks(void)
{
#ifdef USE_ASSERT_CHECKING
	int			RefCountErrors = 0;
	PrivateRefCountEntry *res;
	int			i;

	/* 检查数组 */
	for (i = 0; i < REFCOUNT_ARRAY_ENTRIES; i++)
	{
		res = &PrivateRefCountArray[i];

		if (res->buffer != InvalidBuffer)
		{
			PrintBufferLeakWarning(res->buffer);
			RefCountErrors++;
		}
	}

	/* 必要时搜索散列表 */
	if (PrivateRefCountOverflowed)
	{
		HASH_SEQ_STATUS hstat;

		hash_seq_init(&hstat, PrivateRefCountHash);
		while ((res = (PrivateRefCountEntry *) hash_seq_search(&hstat)) != NULL)
		{
			PrintBufferLeakWarning(res->buffer);
			RefCountErrors++;
		}
	}

	Assert(RefCountErrors == 0);
#endif
}

/*
 * 缓冲区被意外钉住时发出警告的辅助例程
 */
void
PrintBufferLeakWarning(Buffer buffer)
{
	BufferDesc *buf;
	int32		loccount;
	char	   *path;
	BackendId	backend;
	uint32		buf_state;

	Assert(BufferIsValid(buffer));
	if (BufferIsLocal(buffer))
	{
		buf = GetLocalBufferDescriptor(-buffer - 1);
		loccount = LocalRefCount[-buffer - 1];
		backend = MyBackendId;
	}
	else
	{
		buf = GetBufferDescriptor(buffer - 1);
		loccount = GetPrivateRefCount(buffer);
		backend = InvalidBackendId;
	}

	/* 理论上这里我们应当锁住 bufhdr */
	path = relpathbackend(BufTagGetRelFileLocator(&buf->tag), backend,
						  BufTagGetForkNum(&buf->tag));
	buf_state = pg_atomic_read_u32(&buf->state);
	elog(WARNING,
		 "buffer refcount leak: [%03d] "
		 "(rel=%s, blockNum=%u, flags=0x%x, refcount=%u %d)",
		 buffer, path,
		 buf->tag.blockNum, buf_state & BUF_FLAG_MASK,
		 BUF_STATE_GET_REFCOUNT(buf_state), loccount);
	pfree(path);
}

/*
 * CheckPointBuffers
 *
 * 检查点时把缓冲池里各缓冲区的全部脏页刷到磁盘。
 *
 * 注意：临时关系不参与检查点，因此它们不必
 * 被刷写。
 */
void
CheckPointBuffers(int flags)
{
	BufferSync(flags);
}

/*
 * BufferGetBlockNumber
 *		返回与缓冲区关联的块号。
 *
 * 注意：
 *		假定该缓冲区有效且已钉住，否则
 *		该值可能马上就过时……
 */
BlockNumber
BufferGetBlockNumber(Buffer buffer)
{
	BufferDesc *bufHdr;

	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
		bufHdr = GetLocalBufferDescriptor(-buffer - 1);
	else
		bufHdr = GetBufferDescriptor(buffer - 1);

	/* 已钉住，因此不持自旋锁读标签没问题 */
	return bufHdr->tag.blockNum;
}

/*
 * BufferGetTag
 *		返回与缓冲区关联的 relfilelocator、分叉号和
 *		块号。
 */
void
BufferGetTag(Buffer buffer, RelFileLocator *rlocator, ForkNumber *forknum,
			 BlockNumber *blknum)
{
	BufferDesc *bufHdr;

	/* 做与 BufferGetBlockNumber 相同的检查。 */
	Assert(BufferIsPinned(buffer));

	if (BufferIsLocal(buffer))
		bufHdr = GetLocalBufferDescriptor(-buffer - 1);
	else
		bufHdr = GetBufferDescriptor(buffer - 1);

	/* 已钉住，因此不持自旋锁读标签没问题 */
	*rlocator = BufTagGetRelFileLocator(&bufHdr->tag);
	*forknum = BufTagGetForkNum(&bufHdr->tag);
	*blknum = bufHdr->tag.blockNum;
}

/*
 * FlushBuffer
 *		物理地写出一个共享缓冲区。
 *
 * 注意：这实际上只是把缓冲区内容交给内核；真正写到
 * 磁盘要等到内核愿意时才发生。从我们的角度看这没问题，
 * 因为我们可以从 WAL 重做这些变更。
 * 不过，在我们能对 WAL 做检查点之前，需要用 fsync
 * 把这些变更强制到磁盘。
 *
 * 调用者必须对该缓冲区持有钉住，并共享锁定了
 * 缓冲区内容。（注意：共享锁不阻止更新
 * 缓冲区里的提示位，因此写进行期间该页可能改变，
 * 但我们假定那不会使已写出的数据
 * 失效。）
 *
 * 若调用者持有该缓冲区关系的 smgr 引用，就把它作为
 * 第二个参数传入。若没有，就传入 NULL。
 */
static void
FlushBuffer(BufferDesc *buf, SMgrRelation reln, IOObject io_object,
			IOContext io_context)
{
	XLogRecPtr	recptr;
	ErrorContextCallback errcallback;
	instr_time	io_start;
	Block		bufBlock;
	char	   *bufToWrite;
	uint32		buf_state;

	/*
	 * 试图开始一次 I/O 操作。若 StartBufferIO 返回假，则
	 * 别人赶在我们之前刷写了该缓冲区，因此我们不必
	 * 做任何事。
	 */
	if (!StartBufferIO(buf, false))
		return;

	/* 为 ereport() 设置错误回溯支持 */
	errcallback.callback = shared_buffer_write_error_callback;
	errcallback.arg = (void *) buf;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 查找缓冲区的 smgr 关系 */
	if (reln == NULL)
		reln = smgropen(BufTagGetRelFileLocator(&buf->tag), InvalidBackendId);

	TRACE_POSTGRESQL_BUFFER_FLUSH_START(BufTagGetForkNum(&buf->tag),
										buf->tag.blockNum,
										reln->smgr_rlocator.locator.spcOid,
										reln->smgr_rlocator.locator.dbOid,
										reln->smgr_rlocator.locator.relNumber);

	buf_state = LockBufHdr(buf);

	/*
	 * 持有描述符锁时运行 PageGetLSN，因为并非所有情形
	 * 我们都排他锁定了缓冲区。
	 */
	recptr = BufferGetLSN(buf);

	/* 检查刷写期间块内容是否改变。- vadim 01/17/97 */
	buf_state &= ~BM_JUST_DIRTIED;
	UnlockBufHdr(buf, buf_state);

	/*
	 * 强制把 XLOG 刷到该缓冲区的 LSN。这实现了基本的 WAL
	 * 规则：日志更新必须先落到磁盘，然后它们所描述的任何数据文件变更
	 * 才能落到磁盘。
	 *
	 * 不过，这条规则不适用于未日志记录的关系，它们在崩溃后
	 * 反正会丢失。多数未日志记录关系的页不带
	 * LSNs，因为我们从不为它们发出 WAL 记录，因此刷到
	 * 缓冲区 LSN 没有用，但也无害。不过，
	 * GiST 索引在内部用 LSNs 跟踪页分裂，因此
	 * 未日志记录的 GiST 页带有由
	 * GetFakeLSNForUnloggedRel 生成的假 LSN。假
	 * LSN 计数器推进超过 WAL 插入点不太可能，但有可能；若真的
	 * 发生，试图把 WAL 刷到那个位置会失败，带来
	 * 灾难性的全系统后果。为确保那不会发生，
	 * 若该缓冲区不是永久的，就跳过这次刷写。
	 */
	if (buf_state & BM_PERMANENT)
		XLogFlush(recptr);

	/*
	 * 现在把缓冲区写到磁盘是安全的。注意，我们忙于刷日志时，
	 * 别人应当无法写它，因为
	 * 同一时刻只有一个进程能设置 BM_IO_IN_PROGRESS 位。
	 */
	bufBlock = BufHdrGetBlock(buf);

	/*
	 * 若希望如此，就更新页校验和。由于我们只对该
	 * 缓冲区持有共享锁，其他进程可能正在更新它的提示位，因此若做校验和，
	 * 我们必须把该页复制到私有存储。
	 */
	bufToWrite = PageSetChecksumCopy((Page) bufBlock, buf->tag.blockNum);

	io_start = pgstat_prepare_io_time();

	/*
	 * 视情况，bufToWrite 要么是共享缓冲区，要么是一份副本。
	 */
	smgrwrite(reln,
			  BufTagGetForkNum(&buf->tag),
			  buf->tag.blockNum,
			  bufToWrite,
			  false);

	/*
	 * 使用策略时，只有已经在
	 * 策略环里的脏页刷写才算作策略写（IOCONTEXT
	 * [BULKREAD|BULKWRITE|VACUUM] IOOP_WRITE），以便
	 * IO 统计跟踪。
	 *
	 * 若最初加入环的共享缓冲区在使用前必须被刷写，
	 * 这算作 IOCONTEXT_NORMAL IOOP_WRITE。
	 *
	 * 若一个共享缓冲区是后来才加入环的，因为
	 * 当前策略缓冲区被钉住或正在使用，或因为全部策略
	 * 缓冲区都是脏页而被拒绝（仅对 BAS_BULKREAD 操作），
	 * 并且需要刷写，这算作 IOCONTEXT_NORMAL IOOP_WRITE
	 * （from_ring 将为假）。
	 *
	 * 不使用策略时，这次写只能是脏的共享缓冲区的普通写
	 * （IOCONTEXT_NORMAL IOOP_WRITE）。
	 */
	pgstat_count_io_op_time(IOOBJECT_RELATION, io_context,
							IOOP_WRITE, io_start, 1);

	pgBufferUsage.shared_blks_written++;

	/*
	 * 把缓冲区标为干净（除非 BM_JUST_DIRTIED 已被设置），并
	 * 结束 BM_IO_IN_PROGRESS 状态。
	 */
	TerminateBufferIO(buf, true, 0);

	TRACE_POSTGRESQL_BUFFER_FLUSH_DONE(BufTagGetForkNum(&buf->tag),
									   buf->tag.blockNum,
									   reln->smgr_rlocator.locator.spcOid,
									   reln->smgr_rlocator.locator.dbOid,
									   reln->smgr_rlocator.locator.relNumber);

	/* 弹出错误上下文栈 */
	error_context_stack = errcallback.previous;
}

/*
 * RelationGetNumberOfBlocksInFork
 *		确定指定关系分叉里的当前页数。
 *
 * 注意，结果的准确性取决于该关系存储的细节。
 * 对内建访问方法它是准确的，但对外部访问方法
 * 可能不准。
 */
BlockNumber
RelationGetNumberOfBlocksInFork(Relation relation, ForkNumber forkNum)
{
	if (RELKIND_HAS_TABLE_AM(relation->rd_rel->relkind))
	{
		/*
		 * 并非每个表访问方法都使用 BLCKSZ 宽的固定大小块。因此
		 * tableam 以字节返回大小，但对本例程的目的，
		 * 我们要的是块数。因此做除法，向上
		 * 取整。
		 */
		uint64		szbytes;

		szbytes = table_relation_size(relation, forkNum);

		return (szbytes + (BLCKSZ - 1)) / BLCKSZ;
	}
	else if (RELKIND_HAS_STORAGE(relation->rd_rel->relkind))
	{
		return smgrnblocks(RelationGetSmgr(relation), forkNum);
	}
	else
		Assert(false);

	return 0;					/* 消除编译器告警 */
}

/*
 * BufferIsPermanent
 *		判断一个缓冲区在崩溃之后是否仍可能存在。
 *		调用者必须持有缓冲区钉住。
 */
bool
BufferIsPermanent(Buffer buffer)
{
	BufferDesc *bufHdr;

	/* 本地缓冲区只用于临时关系。 */
	if (BufferIsLocal(buffer))
		return false;

	/* 确保我们拿到的是真正的缓冲区，并且我们对它持有钉住。 */
	Assert(BufferIsValid(buffer));
	Assert(BufferIsPinned(buffer));

	/*
	 * 我们持有对该缓冲区的钉住时，BM_PERMANENT 不能被改变，因此
	 * 不必费心拿缓冲区描述符自旋锁。即使别人
	 * 在我们做这件事时改变了缓冲区描述符状态，该状态是
	 * 原子地改变的，因此我们读到的是旧值或新值，而不是
	 * 随机垃圾。
	 */
	bufHdr = GetBufferDescriptor(buffer - 1);
	return (pg_atomic_read_u32(&bufHdr->state) & BM_PERMANENT) != 0;
}

/*
 * BufferGetLSNAtomic
 *		用缓冲区描述符锁原子地取得该缓冲区的 LSN。
 *		这对某些可能没有对该缓冲区排他锁的
 *		调用者是必要的。
 */
XLogRecPtr
BufferGetLSNAtomic(Buffer buffer)
{
	char	   *page = BufferGetPage(buffer);
	BufferDesc *bufHdr;
	XLogRecPtr	lsn;
	uint32		buf_state;

	/*
	 * 若正确性不需要加锁，就走快路径出去。
	 */
	if (!XLogHintBitIsNeeded() || BufferIsLocal(buffer))
		return PageGetLSN(page);

	/* 确保我们拿到的是真正的缓冲区，并且我们对它持有钉住。 */
	Assert(BufferIsValid(buffer));
	Assert(BufferIsPinned(buffer));

	bufHdr = GetBufferDescriptor(buffer - 1);
	buf_state = LockBufHdr(bufHdr);
	lsn = PageGetLSN(page);
	UnlockBufHdr(bufHdr, buf_state);

	return lsn;
}

/* ---------------------------------------------------------------------
 *		DropRelationBuffers
 *
 *		本函数从缓冲池去掉指定关系分叉中
 *		块号 >= firstDelBlock 的全部页。
 *		（特别地，firstDelBlock = 0 时去掉全部页。）
 *		脏页被直接丢掉，不费心先把它们
 *		写出。因此这不能回滚，所以只应当
 *		极其谨慎地使用！
 *
 *		目前，这只在底层文件即将被删除或截断时
 *		从 smgr.c 调用（截断情形需要 firstDelBlock）。
 *		受影响页里的数据反正马上就会被删除，
 *		写出去没有意义。
 *		确保删除或截断不会丢失以后可能需要的
 *		任何数据，是更高层代码的责任。
 *		确保没有其他进程试图把该关系的更多页
 *		装入缓冲区，也是更高层代码的
 *		责任。
 * --------------------------------------------------------------------
 */
void
DropRelationBuffers(SMgrRelation smgr_reln, ForkNumber *forkNum,
					int nforks, BlockNumber *firstDelBlock)
{
	int			i;
	int			j;
	RelFileLocatorBackend rlocator;
	BlockNumber nForkBlock[MAX_FORKNUM];
	uint64		nBlocksToInvalidate = 0;

	rlocator = smgr_reln->smgr_rlocator;

	/* 若它是本地关系，那就是 localbuf.c 的问题。 */
	if (RelFileLocatorBackendIsTemp(rlocator))
	{
		if (rlocator.backend == MyBackendId)
		{
			for (j = 0; j < nforks; j++)
				DropRelationLocalBuffers(rlocator.locator, forkNum[j],
										 firstDelBlock[j]);
		}
		return;
	}

	/*
	 * 要从缓冲池去掉指定关系分叉的全部页，
	 * 我们需要扫描整个缓冲池，但若我们知道该关系每个分叉的
	 * 准确大小，就可以通过从 BufMapping 表找出缓冲区来加快。
	 * 准确大小是必需的，以确保我们不会留下被丢掉关系的任何缓冲区，
	 * 否则后台写进程或检查点进程在刷写对应不存在文件的缓冲区时
	 * 可能导致 PANIC
	 * 错误。
	 *
	 * 为知道准确大小，我们依赖恢复期间为每个分叉缓存的大小，
	 * 这把这种加快限制在恢复和
	 * 后备机上，但一旦我们有了关系大小的共享缓存，
	 * 就可以很容易地扩展它。
	 *
	 * 在恢复中，我们缓存第一次 lseek(SEEK_END) 返回的值，
	 * 以后的写保持缓存值是最新的。见
	 * smgrextend。第一次 lseek 的值可能小于
	 * 文件里实际存在的块数，因为有缺陷的
	 * Linux 内核可能没有计入最近的写。但
	 * 那应当没问题，因为该文件大小之后
	 * 不得有任何缓冲区。
	 */
	for (i = 0; i < nforks; i++)
	{
		/* 取得关系某个分叉的块数 */
		nForkBlock[i] = smgrnblocks_cached(smgr_reln, forkNum[i]);

		if (nForkBlock[i] == InvalidBlockNumber)
		{
			nBlocksToInvalidate = InvalidBlockNumber;
			break;
		}

		/* 计算要失效的块数 */
		nBlocksToInvalidate += (nForkBlock[i] - firstDelBlock[i]);
	}

	/*
	 * 当且仅当要失效的块总数低于
	 * BUF_DROP_FULL_SCAN_THRESHOLD 时，我们才应用这种加快。
	 */
	if (BlockNumberIsValid(nBlocksToInvalidate) &&
		nBlocksToInvalidate < BUF_DROP_FULL_SCAN_THRESHOLD)
	{
		for (j = 0; j < nforks; j++)
			FindAndDropRelationBuffers(rlocator.locator, forkNum[j],
									   nForkBlock[j], firstDelBlock[j]);
		return;
	}

	for (i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 我们可以先预检查缓冲区标签，再试图
		 * 锁住缓冲区，从而稍快一点；这在典型情形下
		 * 省掉大量锁获取。这应当是安全的，因为
		 * 调用者必须对该关系持有 AccessExclusiveLock，或有别的
		 * 理由确信没有人在把该关系的新页装入
		 * 缓冲池。（否则我们很可能完全错过这样的页。）
		 * 因此，我们看它时标签可能正在变，但它不能
		 * 变成我们关心的值，只能离开这样的值。
		 * 因此假阴性不可能，而假阳性是安全的，
		 * 因为取得缓冲区锁之后
		 * 我们会重新检查。
		 *
		 * 我们也可以检查 forkNum 和 blockNum 以及 rlocator，但
		 * 这样做的额外收益似乎很小。
		 */
		if (!BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator.locator))
			continue;

		buf_state = LockBufHdr(bufHdr);

		for (j = 0; j < nforks; j++)
		{
			if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator.locator) &&
				BufTagGetForkNum(&bufHdr->tag) == forkNum[j] &&
				bufHdr->tag.blockNum >= firstDelBlock[j])
			{
				InvalidateBuffer(bufHdr);	/* 释放自旋锁 */
				break;
			}
		}
		if (j >= nforks)
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* ---------------------------------------------------------------------
 *		DropRelationsAllBuffers
 *
 *		本函数从缓冲池去掉指定关系全部缓冲区的
 *		分叉的全部页。它等价于对每个关系的每个分叉以 firstDelBlock = 0
 *		调用一次 DropRelationBuffers。
 *		--------------------------------------------------------------------
 */
void
DropRelationsAllBuffers(SMgrRelation *smgr_reln, int nlocators)
{
	int			i;
	int			n = 0;
	SMgrRelation *rels;
	BlockNumber (*block)[MAX_FORKNUM + 1];
	uint64		nBlocksToInvalidate = 0;
	RelFileLocator *locators;
	bool		cached = true;
	bool		use_bsearch;

	if (nlocators == 0)
		return;

	rels = palloc(sizeof(SMgrRelation) * nlocators);	/* 非本地关系 */

	/* 若它是本地关系，那就是 localbuf.c 的问题。 */
	for (i = 0; i < nlocators; i++)
	{
		if (RelFileLocatorBackendIsTemp(smgr_reln[i]->smgr_rlocator))
		{
			if (smgr_reln[i]->smgr_rlocator.backend == MyBackendId)
				DropRelationAllLocalBuffers(smgr_reln[i]->smgr_rlocator.locator);
		}
		else
			rels[n++] = smgr_reln[i];
	}

	/*
	 * 若没有非本地关系，我们就完成了。释放
	 * 内存并返回。
	 */
	if (n == 0)
	{
		pfree(rels);
		return;
	}

	/*
	 * 这用来记住全部关系
	 * 分叉的块数。
	 */
	block = (BlockNumber (*)[MAX_FORKNUM + 1])
		palloc(sizeof(BlockNumber) * n * (MAX_FORKNUM + 1));

	/*
	 * 若我们知道每个给定关系分叉的准确大小，就可以避免扫描缓冲池里的全部缓冲区。
	 * 见 DropRelationBuffers。
	 */
	for (i = 0; i < n && cached; i++)
	{
		for (int j = 0; j <= MAX_FORKNUM; j++)
		{
			/* 取得关系某个分叉的块数。 */
			block[i][j] = smgrnblocks_cached(rels[i], j);

			/* 我们只需要考虑存在的关系分叉。 */
			if (block[i][j] == InvalidBlockNumber)
			{
				if (!smgrexists(rels[i], j))
					continue;
				cached = false;
				break;
			}

			/* 计算要失效的块总数 */
			nBlocksToInvalidate += block[i][j];
		}
	}

	/*
	 * 当且仅当要失效的块总数低于
	 * BUF_DROP_FULL_SCAN_THRESHOLD 时，我们才应用这种加快。
	 */
	if (cached && nBlocksToInvalidate < BUF_DROP_FULL_SCAN_THRESHOLD)
	{
		for (i = 0; i < n; i++)
		{
			for (int j = 0; j <= MAX_FORKNUM; j++)
			{
				/* 忽略不存在的关系分叉 */
				if (!BlockNumberIsValid(block[i][j]))
					continue;

				/* 丢掉某个关系分叉的全部缓冲区 */
				FindAndDropRelationBuffers(rels[i]->smgr_rlocator.locator,
										   j, block[i][j], 0);
			}
		}

		pfree(block);
		pfree(rels);
		return;
	}

	pfree(block);
	locators = palloc(sizeof(RelFileLocator) * n);	/* 非本地关系 */
	for (i = 0; i < n; i++)
		locators[i] = rels[i]->smgr_rlocator.locator;

	/*
	 * 对要丢掉的关系数量较少时，就用简单遍历，以
	 * 省掉二分查找的开销。所用阈值与其说是精确测定的值，
	 * 不如说是猜测，因为它取决于许多因素（CPU 和内存
	 * 速度、共享缓冲区数量等）。
	 */
	use_bsearch = n > RELS_BSEARCH_THRESHOLD;

	/* 必要时对 rlocator 列表排序 */
	if (use_bsearch)
		pg_qsort(locators, n, sizeof(RelFileLocator), rlocator_comparator);

	for (i = 0; i < NBuffers; i++)
	{
		RelFileLocator *rlocator = NULL;
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 与 DropRelationBuffers 一样，不加锁的预检查应当安全，
		 * 并省掉一些周期。
		 */

		if (!use_bsearch)
		{
			int			j;

			for (j = 0; j < n; j++)
			{
				if (BufTagMatchesRelFileLocator(&bufHdr->tag, &locators[j]))
				{
					rlocator = &locators[j];
					break;
				}
			}
		}
		else
		{
			RelFileLocator locator;

			locator = BufTagGetRelFileLocator(&bufHdr->tag);
			rlocator = bsearch((const void *) &(locator),
							   locators, n, sizeof(RelFileLocator),
							   rlocator_comparator);
		}

		/* 缓冲区不属于任何给定的 relfilelocator；跳过它 */
		if (rlocator == NULL)
			continue;

		buf_state = LockBufHdr(bufHdr);
		if (BufTagMatchesRelFileLocator(&bufHdr->tag, rlocator))
			InvalidateBuffer(bufHdr);	/* 释放自旋锁 */
		else
			UnlockBufHdr(bufHdr, buf_state);
	}

	pfree(locators);
	pfree(rels);
}

/* ---------------------------------------------------------------------
 *		FindAndDropRelationBuffers
 *
 *		本函数在 BufMapping 表里查找，并从缓冲池去掉这些缓冲区，即
 *		指定关系分叉中块号 >= firstDelBlock 的全部页。
 *		（特别地，firstDelBlock = 0 时去掉全部
 *		页。）
 * --------------------------------------------------------------------
 */
static void
FindAndDropRelationBuffers(RelFileLocator rlocator, ForkNumber forkNum,
						   BlockNumber nForkBlock,
						   BlockNumber firstDelBlock)
{
	BlockNumber curBlock;

	for (curBlock = firstDelBlock; curBlock < nForkBlock; curBlock++)
	{
		uint32		bufHash;	/* 标签的散列值 */
		BufferTag	bufTag;		/* 所请求块的标识 */
		LWLock	   *bufPartitionLock;	/* 它的缓冲区分区锁 */
		int			buf_id;
		BufferDesc *bufHdr;
		uint32		buf_state;

		/* 创建一个标签，以便查找该缓冲区 */
		InitBufferTag(&bufTag, &rlocator, forkNum, curBlock);

		/* 确定它的散列码和分区锁标识 */
		bufHash = BufTableHashCode(&bufTag);
		bufPartitionLock = BufMappingPartitionLock(bufHash);

		/* 检查该缓冲区是否在缓冲池中。若不在，就什么也不做。 */
		LWLockAcquire(bufPartitionLock, LW_SHARED);
		buf_id = BufTableLookup(&bufTag, bufHash);
		LWLockRelease(bufPartitionLock);

		if (buf_id < 0)
			continue;

		bufHdr = GetBufferDescriptor(buf_id);

		/*
		 * 我们需要锁住缓冲区描述符，并重新检查该缓冲区是否
		 * 仍与同一块关联，因为我们释放 BufMapping 表上的锁之后，
		 * 该缓冲区可能被某个其他后端为另一个
		 * 关系装入块而逐出。
		 */
		buf_state = LockBufHdr(bufHdr);

		if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator) &&
			BufTagGetForkNum(&bufHdr->tag) == forkNum &&
			bufHdr->tag.blockNum >= firstDelBlock)
			InvalidateBuffer(bufHdr);	/* 释放自旋锁 */
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* ---------------------------------------------------------------------
 *		DropDatabaseBuffers
 *
 *		本函数去掉缓冲池里某个
 *		特定数据库的全部缓冲区。脏页被直接丢掉，不
 *		费心先把它们写出。这在我们销毁
 *		数据库时使用，以避免目录树不再存在时
 *		还试图把数据刷到磁盘。实现与只销毁一个关系的
 *		DropRelationBuffers() 相当类似。
 * --------------------------------------------------------------------
 */
void
DropDatabaseBuffers(Oid dbid)
{
	int			i;

	/*
	 * 我们不必考虑本地缓冲区，因为按假定目标
	 * 数据库不是我们自己的。
	 */

	for (i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 与 DropRelationBuffers 一样，不加锁的预检查应当安全，
		 * 并省掉一些周期。
		 */
		if (bufHdr->tag.dbOid != dbid)
			continue;

		buf_state = LockBufHdr(bufHdr);
		if (bufHdr->tag.dbOid == dbid)
			InvalidateBuffer(bufHdr);	/* 释放自旋锁 */
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* -----------------------------------------------------------------
 *		PrintBufferDescs
 *
 *		本函数打印全部缓冲区描述符，仅供调试
 *		使用。
 * -----------------------------------------------------------------
 */
#ifdef NOT_USED
void
PrintBufferDescs(void)
{
	int			i;

	for (i = 0; i < NBuffers; ++i)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		Buffer		b = BufferDescriptorGetBuffer(buf);

		/* 理论上这里我们应当锁住 bufhdr */
		elog(LOG,
			 "[%02d] (freeNext=%d, rel=%s, "
			 "blockNum=%u, flags=0x%x, refcount=%u %d)",
			 i, buf->freeNext,
			 relpathbackend(BufTagGetRelFileLocator(&buf->tag),
							InvalidBackendId, BufTagGetForkNum(&buf->tag)),
			 buf->tag.blockNum, buf->flags,
			 buf->refcount, GetPrivateRefCount(b));
	}
}
#endif

#ifdef NOT_USED
void
PrintPinnedBufs(void)
{
	int			i;

	for (i = 0; i < NBuffers; ++i)
	{
		BufferDesc *buf = GetBufferDescriptor(i);
		Buffer		b = BufferDescriptorGetBuffer(buf);

		if (GetPrivateRefCount(b) > 0)
		{
			/* 理论上这里我们应当锁住 bufhdr */
			elog(LOG,
				 "[%02d] (freeNext=%d, rel=%s, "
				 "blockNum=%u, flags=0x%x, refcount=%u %d)",
				 i, buf->freeNext,
				 relpathperm(BufTagGetRelFileLocator(&buf->tag),
							 BufTagGetForkNum(&buf->tag)),
				 buf->tag.blockNum, buf->flags,
				 buf->refcount, GetPrivateRefCount(b));
		}
	}
}
#endif

/* ---------------------------------------------------------------------
 *		FlushRelationBuffers
 *
 *		本函数把一个关系的全部脏页写出到磁盘
 *		（更准确地说，写出到内核磁盘缓冲区），确保内核
 *		对该关系有最新视图。
 *
 *		通常，调用者应当对目标关系持有 AccessExclusiveLock，
 *		以确保没有其他后端正忙着把该关系的更多块
 *		弄成脏页；锁释放之后，不能预期
 *		这些效果还会持续。
 *
 *		XXX 目前它顺序搜索缓冲池，应当改成
 *		更聪明的搜索方式。本例程不用于任何性能关键路径，
 *		因此不值得给正常路径增加额外开销来让它
 *		更快。
 * --------------------------------------------------------------------
 */
void
FlushRelationBuffers(Relation rel)
{
	int			i;
	BufferDesc *bufHdr;

	if (RelationUsesLocalBuffers(rel))
	{
		for (i = 0; i < NLocBuffer; i++)
		{
			uint32		buf_state;
			instr_time	io_start;

			bufHdr = GetLocalBufferDescriptor(i);
			if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rel->rd_locator) &&
				((buf_state = pg_atomic_read_u32(&bufHdr->state)) &
				 (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
			{
				ErrorContextCallback errcallback;
				Page		localpage;

				localpage = (char *) LocalBufHdrGetBlock(bufHdr);

				/* 为 ereport() 设置错误回溯支持 */
				errcallback.callback = local_buffer_write_error_callback;
				errcallback.arg = (void *) bufHdr;
				errcallback.previous = error_context_stack;
				error_context_stack = &errcallback;

				PageSetChecksumInplace(localpage, bufHdr->tag.blockNum);

				io_start = pgstat_prepare_io_time();

				smgrwrite(RelationGetSmgr(rel),
						  BufTagGetForkNum(&bufHdr->tag),
						  bufHdr->tag.blockNum,
						  localpage,
						  false);

				pgstat_count_io_op_time(IOOBJECT_TEMP_RELATION,
										IOCONTEXT_NORMAL, IOOP_WRITE,
										io_start, 1);

				buf_state &= ~(BM_DIRTY | BM_JUST_DIRTIED);
				pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

				pgBufferUsage.local_blks_written++;

				/* 弹出错误上下文栈 */
				error_context_stack = errcallback.previous;
			}
		}

		return;
	}

	/* 确保我们能处理循环里的钉住 */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	for (i = 0; i < NBuffers; i++)
	{
		uint32		buf_state;

		bufHdr = GetBufferDescriptor(i);

		/*
		 * 与 DropRelationBuffers 一样，不加锁的预检查应当安全，
		 * 并省掉一些周期。
		 */
		if (!BufTagMatchesRelFileLocator(&bufHdr->tag, &rel->rd_locator))
			continue;

		ReservePrivateRefCountEntry();

		buf_state = LockBufHdr(bufHdr);
		if (BufTagMatchesRelFileLocator(&bufHdr->tag, &rel->rd_locator) &&
			(buf_state & (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
		{
			PinBuffer_Locked(bufHdr);
			LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);
			FlushBuffer(bufHdr, RelationGetSmgr(rel), IOOBJECT_RELATION, IOCONTEXT_NORMAL);
			LWLockRelease(BufferDescriptorGetContentLock(bufHdr));
			UnpinBuffer(bufHdr);
		}
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/* ---------------------------------------------------------------------
 *		FlushRelationsAllBuffers
 *
 *		本函数把指定 smgr 关系全部
 *		分叉的全部页从缓冲池刷出。它等价于对每个关系调用一次
 *		FlushRelationBuffers。假定这些关系不
 *		使用本地缓冲区。
 * --------------------------------------------------------------------
 */
void
FlushRelationsAllBuffers(SMgrRelation *smgrs, int nrels)
{
	int			i;
	SMgrSortArray *srels;
	bool		use_bsearch;

	if (nrels == 0)
		return;

	/* 供 qsort 填入的数组 */
	srels = palloc(sizeof(SMgrSortArray) * nrels);

	for (i = 0; i < nrels; i++)
	{
		Assert(!RelFileLocatorBackendIsTemp(smgrs[i]->smgr_rlocator));

		srels[i].rlocator = smgrs[i]->smgr_rlocator.locator;
		srels[i].srel = smgrs[i];
	}

	/*
	 * 对要同步的关系数量较少时省掉二分查找开销。详见
	 * DropRelationsAllBuffers。
	 */
	use_bsearch = nrels > RELS_BSEARCH_THRESHOLD;

	/* 必要时对 SMgrRelations 列表排序 */
	if (use_bsearch)
		pg_qsort(srels, nrels, sizeof(SMgrSortArray), rlocator_comparator);

	/* 确保我们能处理循环里的钉住 */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	for (i = 0; i < NBuffers; i++)
	{
		SMgrSortArray *srelent = NULL;
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state;

		/*
		 * 与 DropRelationBuffers 一样，不加锁的预检查应当安全，
		 * 并省掉一些周期。
		 */

		if (!use_bsearch)
		{
			int			j;

			for (j = 0; j < nrels; j++)
			{
				if (BufTagMatchesRelFileLocator(&bufHdr->tag, &srels[j].rlocator))
				{
					srelent = &srels[j];
					break;
				}
			}
		}
		else
		{
			RelFileLocator rlocator;

			rlocator = BufTagGetRelFileLocator(&bufHdr->tag);
			srelent = bsearch((const void *) &(rlocator),
							  srels, nrels, sizeof(SMgrSortArray),
							  rlocator_comparator);
		}

		/* 缓冲区不属于任何给定的 relfilelocator；跳过它 */
		if (srelent == NULL)
			continue;

		ReservePrivateRefCountEntry();

		buf_state = LockBufHdr(bufHdr);
		if (BufTagMatchesRelFileLocator(&bufHdr->tag, &srelent->rlocator) &&
			(buf_state & (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
		{
			PinBuffer_Locked(bufHdr);
			LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);
			FlushBuffer(bufHdr, srelent->srel, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
			LWLockRelease(BufferDescriptorGetContentLock(bufHdr));
			UnpinBuffer(bufHdr);
		}
		else
			UnlockBufHdr(bufHdr, buf_state);
	}

	pfree(srels);
}

/* ---------------------------------------------------------------------
 *		RelationCopyStorageUsingBuffer
 *
 *		用 bufmgr 复制分叉的数据。与 RelationCopyStorage 相同，但不用
 *		smgrread 和 smgrextend，而是用 bufmgr APIs 复制。
 *
 *		关于 permanent 参数的细节，见
 *		CreateAndCopyRelationData() 顶部的注释。
 * --------------------------------------------------------------------
 */
static void
RelationCopyStorageUsingBuffer(RelFileLocator srclocator,
							   RelFileLocator dstlocator,
							   ForkNumber forkNum, bool permanent)
{
	Buffer		srcBuf;
	Buffer		dstBuf;
	Page		srcPage;
	Page		dstPage;
	bool		use_wal;
	BlockNumber nblocks;
	BlockNumber blkno;
	PGIOAlignedBlock buf;
	BufferAccessStrategy bstrategy_src;
	BufferAccessStrategy bstrategy_dst;

	/*
	 * 一般而言，只要 wal_level > 'minimal' 我们就想写 WAL，但复制
	 * 未日志记录关系除 init 分叉以外的任何分叉时，我们可以
	 * 跳过它。
	 */
	use_wal = XLogIsNeeded() && (permanent || forkNum == INIT_FORKNUM);

	/* 取得源关系的块数。 */
	nblocks = smgrnblocks(smgropen(srclocator, InvalidBackendId),
						  forkNum);

	/* 没有什么要复制；直接返回。 */
	if (nblocks == 0)
		return;

	/*
	 * 开始逐块复制之前，把目标关系批量扩展到与源
	 * 关系相同的大小。
	 */
	memset(buf.data, 0, BLCKSZ);
	smgrextend(smgropen(dstlocator, InvalidBackendId), forkNum, nblocks - 1,
			   buf.data, true);

	/* 这是批量操作，因此使用缓冲区访问策略。 */
	bstrategy_src = GetAccessStrategy(BAS_BULKREAD);
	bstrategy_dst = GetAccessStrategy(BAS_BULKWRITE);

	/* 遍历源关系文件的每个块。 */
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		CHECK_FOR_INTERRUPTS();

		/* 从源关系读取块。 */
		srcBuf = ReadBufferWithoutRelcache(srclocator, forkNum, blkno,
										   RBM_NORMAL, bstrategy_src,
										   permanent);
		LockBuffer(srcBuf, BUFFER_LOCK_SHARE);
		srcPage = BufferGetPage(srcBuf);

		dstBuf = ReadBufferWithoutRelcache(dstlocator, forkNum, blkno,
										   RBM_ZERO_AND_LOCK, bstrategy_dst,
										   permanent);
		dstPage = BufferGetPage(dstBuf);

		START_CRIT_SECTION();

		/* 把页数据从源复制到目标。 */
		memcpy(dstPage, srcPage, BLCKSZ);
		MarkBufferDirty(dstBuf);

		/* WAL 记录被复制的页。 */
		if (use_wal)
			log_newpage_buffer(dstBuf, true);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(dstBuf);
		UnlockReleaseBuffer(srcBuf);
	}

	FreeAccessStrategy(bstrategy_src);
	FreeAccessStrategy(bstrategy_dst);
}

/* ---------------------------------------------------------------------
 *		CreateAndCopyRelationData
 *
 *		创建目标关系存储，并把源关系的全部分叉
 *		复制到目标。
 *
 *		对永久关系把 permanent 传为真，对
 *		未日志记录关系传为假。目前这个 API 不支持
 *		临时关系。
 * --------------------------------------------------------------------
 */
void
CreateAndCopyRelationData(RelFileLocator src_rlocator,
						  RelFileLocator dst_rlocator, bool permanent)
{
	RelFileLocatorBackend rlocator;
	char		relpersistence;

	/* 设置 relpersistence。 */
	relpersistence = permanent ?
		RELPERSISTENCE_PERMANENT : RELPERSISTENCE_UNLOGGED;

	/*
	 * 创建并复制该关系的全部分叉。创建数据库期间我们
	 * 有单独的清理机制，它删除整个数据库
	 * 目录。因此每个单独的关系不必
	 * 登记以便清理。
	 */
	RelationCreateStorage(dst_rlocator, relpersistence, false);

	/* 复制主分叉。 */
	RelationCopyStorageUsingBuffer(src_rlocator, dst_rlocator, MAIN_FORKNUM,
								   permanent);

	/* 复制那些存在的额外分叉 */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(smgropen(src_rlocator, InvalidBackendId), forkNum))
		{
			smgrcreate(smgropen(dst_rlocator, InvalidBackendId), forkNum, false);

			/*
			 * 若该关系是持久的，或者这是未日志记录关系的
			 * init 分叉，就 WAL 记录创建。
			 */
			if (permanent || forkNum == INIT_FORKNUM)
				log_smgrcreate(&dst_rlocator, forkNum);

			/* 逐块复制一个分叉的数据。 */
			RelationCopyStorageUsingBuffer(src_rlocator, dst_rlocator, forkNum,
										   permanent);
		}
	}

	/* 若存在，就关闭源和目标 smgr。 */
	rlocator.backend = InvalidBackendId;

	rlocator.locator = src_rlocator;
	smgrcloserellocator(rlocator);

	rlocator.locator = dst_rlocator;
	smgrcloserellocator(rlocator);
}

/* ---------------------------------------------------------------------
 *		FlushDatabaseBuffers
 *
 *		本函数把一个数据库的全部脏页写出到磁盘
 *		（更准确地说，写出到内核磁盘缓冲区），确保内核
 *		对该数据库有最新视图。
 *
 *		通常，调用者应当持有适当的锁，以确保
 *		目标数据库里没有其他后端在活动；否则可能有更多
 *		页被弄成脏页。
 *
 *		注意我们不担心刷写临时关系的任何页。
 *		假定那些不会有人感兴趣。
 * --------------------------------------------------------------------
 */
void
FlushDatabaseBuffers(Oid dbid)
{
	int			i;
	BufferDesc *bufHdr;

	/* 确保我们能处理循环里的钉住 */
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	for (i = 0; i < NBuffers; i++)
	{
		uint32		buf_state;

		bufHdr = GetBufferDescriptor(i);

		/*
		 * 与 DropRelationBuffers 一样，不加锁的预检查应当安全，
		 * 并省掉一些周期。
		 */
		if (bufHdr->tag.dbOid != dbid)
			continue;

		ReservePrivateRefCountEntry();

		buf_state = LockBufHdr(bufHdr);
		if (bufHdr->tag.dbOid == dbid &&
			(buf_state & (BM_VALID | BM_DIRTY)) == (BM_VALID | BM_DIRTY))
		{
			PinBuffer_Locked(bufHdr);
			LWLockAcquire(BufferDescriptorGetContentLock(bufHdr), LW_SHARED);
			FlushBuffer(bufHdr, NULL, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
			LWLockRelease(BufferDescriptorGetContentLock(bufHdr));
			UnpinBuffer(bufHdr);
		}
		else
			UnlockBufHdr(bufHdr, buf_state);
	}
}

/*
 * 把一个先前以共享或排他方式锁定并钉住的缓冲区刷到
 * 操作系统。
 */
void
FlushOneBuffer(Buffer buffer)
{
	BufferDesc *bufHdr;

	/* 目前不需要，但没有根本理由不支持 */
	Assert(!BufferIsLocal(buffer));

	Assert(BufferIsPinned(buffer));

	bufHdr = GetBufferDescriptor(buffer - 1);

	Assert(LWLockHeldByMe(BufferDescriptorGetContentLock(bufHdr)));

	FlushBuffer(bufHdr, NULL, IOOBJECT_RELATION, IOCONTEXT_NORMAL);
}

/*
 * ReleaseBuffer -- 释放对缓冲区的钉住
 */
void
ReleaseBuffer(Buffer buffer)
{
	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	if (BufferIsLocal(buffer))
		UnpinLocalBuffer(buffer);
	else
		UnpinBuffer(GetBufferDescriptor(buffer - 1));
}

/*
 * UnlockReleaseBuffer -- 释放缓冲区上的内容锁和钉住。
 *
 * 这只是常见组合的简写。
 */
void
UnlockReleaseBuffer(Buffer buffer)
{
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
}

/*
 * IncrBufferRefCount
 *		增加我们已经至少钉住
 *		过一次的缓冲区上的钉住计数。
 *
 *		本函数无法用于我们没有钉住的缓冲区，
 *		因为它不改变共享缓冲区状态。
 */
void
IncrBufferRefCount(Buffer buffer)
{
	Assert(BufferIsPinned(buffer));
	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);
	if (BufferIsLocal(buffer))
		LocalRefCount[-buffer - 1]++;
	else
	{
		PrivateRefCountEntry *ref;

		ref = GetPrivateRefCountEntry(buffer, true);
		Assert(ref != NULL);
		ref->refcount++;
	}
	ResourceOwnerRememberBuffer(CurrentResourceOwner, buffer);
}

/*
 * MarkBufferDirtyHint
 *
 *	把缓冲区标为脏页，用于非关键变更。
 *
 * 这本质上与 MarkBufferDirty 相同，除了：
 *
 * 1. 调用者不写 WAL；因此若启用了校验和，我们可能需要
 *	  写一条 XLOG_FPI_FOR_HINT WAL 记录，以防止撕裂页。
 * 2. 调用者可能只对缓冲区的内容锁持有共享锁，
 *	  而不是排他锁。
 * 3. 本函数不保证缓冲区总是被标为脏页
 *	  （由于竞态），因此它无法用于重要变更。
 */
void
MarkBufferDirtyHint(Buffer buffer, bool buffer_std)
{
	BufferDesc *bufHdr;
	Page		page = BufferGetPage(buffer);

	if (!BufferIsValid(buffer))
		elog(ERROR, "bad buffer ID: %d", buffer);

	if (BufferIsLocal(buffer))
	{
		MarkLocalBufferDirty(buffer);
		return;
	}

	bufHdr = GetBufferDescriptor(buffer - 1);

	Assert(GetPrivateRefCount(buffer) > 0);
	/* 这里共享锁或排他锁都可以 */
	Assert(LWLockHeldByMe(BufferDescriptorGetContentLock(bufHdr)));

	/*
	 * 若一个事务提交时增加或删除了许多元组，第一次扫描时
	 * 我们可能对同一页多次调用本例程。因此，若缓冲区已经是脏页，
	 * 就尽量快。我们通过在状态位看起来已经设置时
	 * 不取得自旋锁来做到这一点。由于我们不加锁做这项测试，
	 * 有可能我们没注意到标志刚刚被清除，
	 * 并且由于内存序问题而没能重新设置它们。
	 * 但由于本函数只打算用于即使没能写出
	 * 数据也无害的情形，
	 * 这其实没关系。
	 */
	if ((pg_atomic_read_u32(&bufHdr->state) & (BM_DIRTY | BM_JUST_DIRTIED)) !=
		(BM_DIRTY | BM_JUST_DIRTIED))
	{
		XLogRecPtr	lsn = InvalidXLogRecPtr;
		bool		dirtied = false;
		bool		delayChkptFlags = false;
		uint32		buf_state;

		/*
		 * 若我们需要保护提示位更新免于撕裂写，就 WAL 记录
		 * 该页的整页映像。仅当这次提示位更新是自上次
		 * 检查点以来对该页的第一次变更时，
		 * 这个整页映像才有必要。
		 *
		 * 这里我们不检查 full_page_writes，因为那个逻辑包含在
		 * 我们调用 XLogInsert() 时，因为该值会动态变化。
		 */
		if (XLogHintBitIsNeeded() &&
			(pg_atomic_read_u32(&bufHdr->state) & BM_PERMANENT))
		{
			/*
			 * 若由于特定于 relfilelocator 的条件或正处于恢复中，
			 * 我们不得写 WAL，就不要把该页弄成脏页。我们可以
			 * 设置提示，只是不要因此把该页弄成脏页，这样当我们逐出该页或关闭时
			 * 提示就丢失了。
			 *
			 * 更长的讨论见 src/backend/storage/page/README。
			 */
			if (RecoveryInProgress() ||
				RelFileLocatorSkippingWAL(BufTagGetRelFileLocator(&bufHdr->tag)))
				return;

			/*
			 * 若该块已经是脏页，因为我们已经做了变更
			 * 或已经设置了提示，那我们就不需要写整页
			 * 映像。注意，激进地清理因设置提示位而变脏的块
			 * 会提高调用率。批量设置提示
			 * 位会降低调用率。
			 *
			 * 我们必须在把缓冲区标为脏页之前发出 WAL 记录。
			 * 否则我们可能在写 WAL 之前写出该页。那
			 * 会造成竞态，因为检查点可能发生在
			 * 写 WAL 记录和把缓冲区标为脏页之间。我们用一种
			 * 权宜之计解决，而这种办法在事务提交期间已经在用，
			 * 以防止竞态。基本上，我们
			 * 简单地阻止检查点 WAL 记录被写出，
			 * 直到我们把缓冲区标为脏页。我们在标成脏页之前不开始
			 * 检查点刷写，因此我们的检查点
			 * 必须成功地把该变更刷到磁盘，否则检查点
			 * 永远不会被写出，于是崩溃恢复会修复。
			 *
			 * 我们可能在没有 xid 的情况下进入这里，因此
			 * CreateCheckPoint 等待虚拟事务而不是完整的 transactionid
			 * 是必要的。
			 */
			Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
			MyProc->delayChkptFlags |= DELAY_CHKPT_START;
			delayChkptFlags = true;
			lsn = XLogSaveBufferForHint(buffer, buffer_std);
		}

		buf_state = LockBufHdr(bufHdr);

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);

		if (!(buf_state & BM_DIRTY))
		{
			dirtied = true;		/* 表示将被本动作弄成脏页 */

			/*
			 * 若我们写了备份块，就设置页 LSN。只持有共享锁时
			 * 我们本不该设置它，但只要我们
			 * 以某种方式串行化就没问题。
			 * 我们选择在持有缓冲区描述符锁时设置 LSN，这使任何
			 * 只持有共享锁的 LSN 读者在使用 PageGetLSN() 之前
			 * 也要取得缓冲区描述符锁，这在
			 * BufferGetLSNAtomic() 里被强制。
			 *
			 * 若启用了校验和，你也许认为我们应当在这里重置
			 * 校验和。那会在本检查点周期稍后写出该页时
			 * 发生。
			 */
			if (!XLogRecPtrIsInvalid(lsn))
				PageSetLSN(page, lsn);
		}

		buf_state |= BM_DIRTY | BM_JUST_DIRTIED;
		UnlockBufHdr(bufHdr, buf_state);

		if (delayChkptFlags)
			MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;

		if (dirtied)
		{
			VacuumPageDirty++;
			pgBufferUsage.shared_blks_dirtied++;
			if (VacuumCostActive)
				VacuumCostBalance += VacuumCostPageDirty;
		}
	}
}

/*
 * 释放共享缓冲区的内容锁。
 *
 * 用于出错后的清理。
 *
 * 目前，我们可以预期 lwlock.c 的 LWLockReleaseAll() 已经负责
 * 释放缓冲区内容锁本身；这里我们唯一需要处理的
 * 是清除任何正在进行的 PIN_COUNT 请求。
 */
void
UnlockBuffers(void)
{
	BufferDesc *buf = PinCountWaitBuf;

	if (buf)
	{
		uint32		buf_state;

		buf_state = LockBufHdr(buf);

		/*
		 * 若标志位没设置，不要抱怨；它可能已被重置，但我们
		 * 在收到信号之前就收到了取消或死亡中断。
		 */
		if ((buf_state & BM_PIN_COUNT_WAITER) != 0 &&
			buf->wait_backend_pgprocno == MyProc->pgprocno)
			buf_state &= ~BM_PIN_COUNT_WAITER;

		UnlockBufHdr(buf, buf_state);

		PinCountWaitBuf = NULL;
	}
}

/*
 * 取得或释放该缓冲区的 content_lock。
 */
void
LockBuffer(Buffer buffer, int mode)
{
	BufferDesc *buf;

	Assert(BufferIsPinned(buffer));
	if (BufferIsLocal(buffer))
		return;					/* 本地缓冲区不需要锁 */

	buf = GetBufferDescriptor(buffer - 1);

	if (mode == BUFFER_LOCK_UNLOCK)
		LWLockRelease(BufferDescriptorGetContentLock(buf));
	else if (mode == BUFFER_LOCK_SHARE)
		LWLockAcquire(BufferDescriptorGetContentLock(buf), LW_SHARED);
	else if (mode == BUFFER_LOCK_EXCLUSIVE)
		LWLockAcquire(BufferDescriptorGetContentLock(buf), LW_EXCLUSIVE);
	else
		elog(ERROR, "unrecognized buffer lock mode: %d", mode);
}

/*
 * 取得该缓冲区的 content_lock，但仅当我们不必等待时。
 *
 * 这假定调用者想要 BUFFER_LOCK_EXCLUSIVE 模式。
 */
bool
ConditionalLockBuffer(Buffer buffer)
{
	BufferDesc *buf;

	Assert(BufferIsPinned(buffer));
	if (BufferIsLocal(buffer))
		return true;			/* 装作我们拿到了它 */

	buf = GetBufferDescriptor(buffer - 1);

	return LWLockConditionalAcquire(BufferDescriptorGetContentLock(buf),
									LW_EXCLUSIVE);
}

/*
 * 验证本后端恰好钉住该缓冲区一次。
 *
 * 注意：与 BufferIsPinned() 一样，这里检查的是本后端
 * 持有对该缓冲区的钉住。我们不关心其他后端是否持有。
 */
void
CheckBufferIsPinnedOnce(Buffer buffer)
{
	if (BufferIsLocal(buffer))
	{
		if (LocalRefCount[-buffer - 1] != 1)
			elog(ERROR, "incorrect local pin count: %d",
				 LocalRefCount[-buffer - 1]);
	}
	else
	{
		if (GetPrivateRefCount(buffer) != 1)
			elog(ERROR, "incorrect local pin count: %d",
				 GetPrivateRefCount(buffer));
	}
}

/*
 * LockBufferForCleanup - 为删除项而锁定缓冲区。
 *
 * 只有当调用者 (a) 对该缓冲区持有排他锁，且 (b) 已观察到
 * 没有其他后端持有对该缓冲区的钉住时，才能从磁盘页删除项。
 * 若有钉住，则另一个后端
 * 可能持有指向缓冲区内部的指针（例如堆扫描对某个项的引用，
 * 详见 README）。不过，清理开始之后再增加钉住是可以的；
 * 新来的后端在我们释放排他锁之前
 * 将无法查看该页。
 *
 * 要实现这个协议，想删除的一方必须钉住该缓冲区，然后
 * 调用 LockBufferForCleanup()。LockBufferForCleanup() 类似于
 * LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE)，只是它循环直到
 * 成功观察到钉住计数 = 1。
 */
void
LockBufferForCleanup(Buffer buffer)
{
	BufferDesc *bufHdr;
	TimestampTz waitStart = 0;
	bool		waiting = false;
	bool		logged_recovery_conflict = false;

	Assert(BufferIsPinned(buffer));
	Assert(PinCountWaitBuf == NULL);

	CheckBufferIsPinnedOnce(buffer);

	/* 没有别人要等 */
	if (BufferIsLocal(buffer))
		return;

	bufHdr = GetBufferDescriptor(buffer - 1);

	for (;;)
	{
		uint32		buf_state;

		/* 试图取得锁 */
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		buf_state = LockBufHdr(bufHdr);

		Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
		if (BUF_STATE_GET_REFCOUNT(buf_state) == 1)
		{
			/* 成功取得钉住计数为 1 的排他锁 */
			UnlockBufHdr(bufHdr, buf_state);

			/*
			 * 若缓冲区钉住上的恢复冲突已解决，但启动进程等待的时间
			 * 超过了
			 * deadlock_timeout，就发出日志消息。
			 */
			if (logged_recovery_conflict)
				LogRecoveryConflict(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
									waitStart, GetCurrentTimestamp(),
									NULL, false);

			if (waiting)
			{
				/* 若我们加过后缀，就把 ps 显示重置以去掉后缀 */
				set_ps_display_remove_suffix();
				waiting = false;
			}
			return;
		}
		/* 失败了，因此把自己标为正在等待钉住计数为 1 */
		if (buf_state & BM_PIN_COUNT_WAITER)
		{
			UnlockBufHdr(bufHdr, buf_state);
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			elog(ERROR, "multiple backends attempting to wait for pincount 1");
		}
		bufHdr->wait_backend_pgprocno = MyProc->pgprocno;
		PinCountWaitBuf = bufHdr;
		buf_state |= BM_PIN_COUNT_WAITER;
		UnlockBufHdr(bufHdr, buf_state);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/* 等待被 UnpinBuffer() 发信号 */
		if (InHotStandby)
		{
			if (!waiting)
			{
				/* 调整进程标题以表明它在等待 */
				set_ps_display_suffix("waiting");
				waiting = true;
			}

			/*
			 * 若启动进程等待缓冲区钉住上的恢复冲突的时间
			 * 超过 deadlock_timeout，就发出日志消息。
			 *
			 * 若是第一次通过就跳过，因为这种情况下启动进程
			 * 还没开始等待。因此等待开始
			 * 时间戳在这段逻辑之后设置。
			 */
			if (waitStart != 0 && !logged_recovery_conflict)
			{
				TimestampTz now = GetCurrentTimestamp();

				if (TimestampDifferenceExceeds(waitStart, now,
											   DeadlockTimeout))
				{
					LogRecoveryConflict(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
										waitStart, now, NULL, true);
					logged_recovery_conflict = true;
				}
			}

			/*
			 * 若启用了日志且是第一次通过，就设置等待开始
			 * 时间戳。
			 */
			if (log_recovery_conflict_waits && waitStart == 0)
				waitStart = GetCurrentTimestamp();

			/* 发布启动进程正在等待的 bufid */
			SetStartupBufferPinWaitBufId(buffer - 1);
			/* 设置闹钟，然后等待被 UnpinBuffer() 发信号 */
			ResolveRecoveryConflictWithBufferPin();
			/* 重置已发布的 bufid */
			SetStartupBufferPinWaitBufId(-1);
		}
		else
			ProcWaitForSignal(PG_WAIT_BUFFER_PIN);

		/*
		 * 去掉把我们标为等待者的标志。通常它不会再被设置，
		 * 但 ProcWaitForSignal() 也可能因其他信号返回。
		 * 我们小心地只在我们是等待者时重置该标志，因为
		 * 理论上另一个后端可能已经开始等待。由于表级锁定，
		 * 以目前的用法这不可能，但
		 * 还是安全为好。
		 */
		buf_state = LockBufHdr(bufHdr);
		if ((buf_state & BM_PIN_COUNT_WAITER) != 0 &&
			bufHdr->wait_backend_pgprocno == MyProc->pgprocno)
			buf_state &= ~BM_PIN_COUNT_WAITER;
		UnlockBufHdr(bufHdr, buf_state);

		PinCountWaitBuf = NULL;
		/* 循环回去重试 */
	}
}

/*
 * 当启动进程请求取消所有挡住它的钉住持有者时，从
 * RecoveryConflictInterrupt 处理器调用的检查。
 */
bool
HoldingBufferPinThatDelaysRecovery(void)
{
	int			bufid = GetStartupBufferPinWaitBufId();

	/*
	 * 若我们被慢慢唤醒，启动进程可能在我们到达这里之前
	 * 已经被其他后端唤醒。也可能我们因多次中断或在不合适的
	 * 时刻中断而到达这里，因此若 bufid 未设置，
	 * 就确保什么也不做。
	 */
	if (bufid < 0)
		return false;

	if (GetPrivateRefCount(bufid + 1) > 0)
		return true;

	return false;
}

/*
 * ConditionalLockBufferForCleanup - 同上，但不等待取得锁。
 *
 * 我们不循环，只检查一次钉住计数是否可以。若
 * 不行，就返回假，且不持有锁。
 */
bool
ConditionalLockBufferForCleanup(Buffer buffer)
{
	BufferDesc *bufHdr;
	uint32		buf_state,
				refcount;

	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
	{
		refcount = LocalRefCount[-buffer - 1];
		/* 应当恰好有一个钉住 */
		Assert(refcount > 0);
		if (refcount != 1)
			return false;
		/* 没有别人要等 */
		return true;
	}

	/* 应当恰好有一个本地钉住 */
	refcount = GetPrivateRefCount(buffer);
	Assert(refcount);
	if (refcount != 1)
		return false;

	/* 试图取得锁 */
	if (!ConditionalLockBuffer(buffer))
		return false;

	bufHdr = GetBufferDescriptor(buffer - 1);
	buf_state = LockBufHdr(bufHdr);
	refcount = BUF_STATE_GET_REFCOUNT(buf_state);

	Assert(refcount > 0);
	if (refcount == 1)
	{
		/* 成功取得钉住计数为 1 的排他锁 */
		UnlockBufHdr(bufHdr, buf_state);
		return true;
	}

	/* 失败了，因此释放锁 */
	UnlockBufHdr(bufHdr, buf_state);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	return false;
}

/*
 * IsBufferCleanupOK - 同上，但我们已经有锁。
 *
 * 检查对一个我们已经锁定的缓冲区做清理是否可以。
 * 若我们观察到钉住计数为 1，我们的排他锁
 * 恰好就是清理锁，我们就可以进行任何
 * 当初寻求清理锁时被允许的事。
 */
bool
IsBufferCleanupOK(Buffer buffer)
{
	BufferDesc *bufHdr;
	uint32		buf_state;

	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
	{
		/* 应当恰好有一个钉住 */
		if (LocalRefCount[-buffer - 1] != 1)
			return false;
		/* 没有别人要等 */
		return true;
	}

	/* 应当恰好有一个本地钉住 */
	if (GetPrivateRefCount(buffer) != 1)
		return false;

	bufHdr = GetBufferDescriptor(buffer - 1);

	/* 调用者必须对该缓冲区持有排他锁 */
	Assert(LWLockHeldByMeInMode(BufferDescriptorGetContentLock(bufHdr),
								LW_EXCLUSIVE));

	buf_state = LockBufHdr(bufHdr);

	Assert(BUF_STATE_GET_REFCOUNT(buf_state) > 0);
	if (BUF_STATE_GET_REFCOUNT(buf_state) == 1)
	{
		/* 钉住计数可以。 */
		UnlockBufHdr(bufHdr, buf_state);
		return true;
	}

	UnlockBufHdr(bufHdr, buf_state);
	return false;
}


/*
 *	缓冲区 I/O 处理的函数。
 *
 *	注意：我们假定不会发生嵌套的缓冲区 I/O。
 *	即每个进程最多设置一个 BM_IO_IN_PROGRESS 位。
 *
 *	还要注意，这些只用于共享缓冲区，不用于本地缓冲区。
 */

/*
 * WaitIO -- 阻塞直到 buf 上的 IO_IN_PROGRESS 标志被清除。
 */
static void
WaitIO(BufferDesc *buf)
{
	ConditionVariable *cv = BufferDescriptorGetIOCV(buf);

	ConditionVariablePrepareToSleep(cv);
	for (;;)
	{
		uint32		buf_state;

		/*
		 * 这里也许不必取得自旋锁来检查该标志，
		 * 但由于这项测试对正确性至关重要，我们最好
		 * 稳妥行事。
		 */
		buf_state = LockBufHdr(buf);
		UnlockBufHdr(buf, buf_state);

		if (!(buf_state & BM_IO_IN_PROGRESS))
			break;
		ConditionVariableSleep(cv, WAIT_EVENT_BUFFER_IO);
	}
	ConditionVariableCancelSleep();
}

/*
 * StartBufferIO：开始对该缓冲区做 I/O
 *	（假定）
 *	我的进程没有在执行 IO
 *	该缓冲区已被钉住。
 *
 * 有些情形下有竞态，多个后端可能
 * 试图并发做同一 I/O 操作。若别人
 * 已经开始对该缓冲区做 I/O，我们就在
 * I/O 条件变量上阻塞，直到他做完。
 *
 * 输入操作只对不是 BM_VALID 的缓冲区尝试，
 * 输出操作只对是 BM_VALID 且 BM_DIRTY 的缓冲区尝试，
 * 因此我们总能判断工作是否已经完成。
 *
 * 若我们成功把缓冲区标为 I/O 忙就返回真，
 * 若别人已经做完工作就返回假。
 */
static bool
StartBufferIO(BufferDesc *buf, bool forInput)
{
	uint32		buf_state;

	ResourceOwnerEnlargeBufferIOs(CurrentResourceOwner);

	for (;;)
	{
		buf_state = LockBufHdr(buf);

		if (!(buf_state & BM_IO_IN_PROGRESS))
			break;
		UnlockBufHdr(buf, buf_state);
		WaitIO(buf);
	}

	/* 一旦我们到了这里，该缓冲区上肯定没有活动的 I/O */

	if (forInput ? (buf_state & BM_VALID) : !(buf_state & BM_DIRTY))
	{
		/* 别人已经做了 I/O */
		UnlockBufHdr(buf, buf_state);
		return false;
	}

	buf_state |= BM_IO_IN_PROGRESS;
	UnlockBufHdr(buf, buf_state);

	ResourceOwnerRememberBufferIO(CurrentResourceOwner,
								  BufferDescriptorGetBuffer(buf));

	return true;
}

/*
 * TerminateBufferIO：释放一个我们正在做 I/O 的缓冲区
 *	（假定）
 *	我的进程正在为该缓冲区执行 IO
 *	该缓冲区设置了 BM_IO_IN_PROGRESS 位
 *	该缓冲区已被钉住。
 *
 * 若 clear_dirty 为真且未设置 BM_JUST_DIRTIED，我们就清除
 * 该缓冲区的 BM_DIRTY 标志。这适用于结束一次
 * 成功的写。对 BM_JUST_DIRTIED 的检查是必要的，以避免
 * 在我们写的时候它又被弄脏时把缓冲区标为干净。
 *
 * set_flag_bits 被 ORed 进该缓冲区的标志。失败时它必须包含
 * BM_IO_ERROR。成功完成时它可以
 * 是 0，或者若我们刚读入该页则为 BM_VALID。
 */
static void
TerminateBufferIO(BufferDesc *buf, bool clear_dirty, uint32 set_flag_bits)
{
	uint32		buf_state;

	buf_state = LockBufHdr(buf);

	Assert(buf_state & BM_IO_IN_PROGRESS);

	buf_state &= ~(BM_IO_IN_PROGRESS | BM_IO_ERROR);
	if (clear_dirty && !(buf_state & BM_JUST_DIRTIED))
		buf_state &= ~(BM_DIRTY | BM_CHECKPOINT_NEEDED);

	buf_state |= set_flag_bits;
	UnlockBufHdr(buf, buf_state);

	ResourceOwnerForgetBufferIO(CurrentResourceOwner,
								BufferDescriptorGetBuffer(buf));

	ConditionVariableBroadcast(BufferDescriptorGetIOCV(buf));
}

/*
 * AbortBufferIO：出错后清理活动的缓冲区 I/O。
 *
 *	我们可能持有的全部 LWLocks 都已释放，
 *	但我们还没释放缓冲区钉住，因此该缓冲区仍被钉住。
 *
 *	若 I/O 正在进行，我们总是设置 BM_IO_ERROR，即使
 *	错误条件可能与 I/O 无关。
 */
void
AbortBufferIO(Buffer buffer)
{
	BufferDesc *buf_hdr = GetBufferDescriptor(buffer - 1);
	uint32		buf_state;

	buf_state = LockBufHdr(buf_hdr);
	Assert(buf_state & (BM_IO_IN_PROGRESS | BM_TAG_VALID));

	if (!(buf_state & BM_VALID))
	{
		Assert(!(buf_state & BM_DIRTY));
		UnlockBufHdr(buf_hdr, buf_state);
	}
	else
	{
		Assert(buf_state & BM_DIRTY);
		UnlockBufHdr(buf_hdr, buf_state);

		/* 若这不是第一次失败，就发出通知…… */
		if (buf_state & BM_IO_ERROR)
		{
			/* 缓冲区已被钉住，因此我们可以不持自旋锁读标签 */
			char	   *path;

			path = relpathperm(BufTagGetRelFileLocator(&buf_hdr->tag),
							   BufTagGetForkNum(&buf_hdr->tag));
			ereport(WARNING,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("could not write block %u of %s",
							buf_hdr->tag.blockNum, path),
					 errdetail("Multiple failures --- write error might be permanent.")));
			pfree(path);
		}
	}

	TerminateBufferIO(buf_hdr, false, BM_IO_ERROR);
}

/*
 * 共享缓冲区写期间发生错误的错误上下文回调。
 */
static void
shared_buffer_write_error_callback(void *arg)
{
	BufferDesc *bufHdr = (BufferDesc *) arg;

	/* 缓冲区已被钉住，因此我们可以不锁自旋锁就读标签 */
	if (bufHdr != NULL)
	{
		char	   *path = relpathperm(BufTagGetRelFileLocator(&bufHdr->tag),
									   BufTagGetForkNum(&bufHdr->tag));

		errcontext("writing block %u of relation %s",
				   bufHdr->tag.blockNum, path);
		pfree(path);
	}
}

/*
 * 本地缓冲区写期间发生错误的错误上下文回调。
 */
static void
local_buffer_write_error_callback(void *arg)
{
	BufferDesc *bufHdr = (BufferDesc *) arg;

	if (bufHdr != NULL)
	{
		char	   *path = relpathbackend(BufTagGetRelFileLocator(&bufHdr->tag),
										  MyBackendId,
										  BufTagGetForkNum(&bufHdr->tag));

		errcontext("writing block %u of relation %s",
				   bufHdr->tag.blockNum, path);
		pfree(path);
	}
}

/*
 * RelFileLocator 的 qsort/bsearch 比较器；见 RelFileLocatorEquals。
 */
static int
rlocator_comparator(const void *p1, const void *p2)
{
	RelFileLocator n1 = *(const RelFileLocator *) p1;
	RelFileLocator n2 = *(const RelFileLocator *) p2;

	if (n1.relNumber < n2.relNumber)
		return -1;
	else if (n1.relNumber > n2.relNumber)
		return 1;

	if (n1.dbOid < n2.dbOid)
		return -1;
	else if (n1.dbOid > n2.dbOid)
		return 1;

	if (n1.spcOid < n2.spcOid)
		return -1;
	else if (n1.spcOid > n2.spcOid)
		return 1;
	else
		return 0;
}

/*
 * 锁住缓冲区描述符，在缓冲区状态里设置 BM_LOCKED。
 */
uint32
LockBufHdr(BufferDesc *desc)
{
	SpinDelayStatus delayStatus;
	uint32		old_buf_state;

	Assert(!BufferIsLocal(BufferDescriptorGetBuffer(desc)));

	init_local_spin_delay(&delayStatus);

	while (true)
	{
		/* 设置 BM_LOCKED 标志 */
		old_buf_state = pg_atomic_fetch_or_u32(&desc->state, BM_LOCKED);
		/* 若它先前没设置，我们就没问题 */
		if (!(old_buf_state & BM_LOCKED))
			break;
		perform_spin_delay(&delayStatus);
	}
	finish_spin_delay(&delayStatus);
	return old_buf_state | BM_LOCKED;
}

/*
 * 等到 BM_LOCKED 标志不再设置，并返回那时该缓冲区的
 * 状态。
 *
 * 显然，值返回时该缓冲区可能又被锁住，因此
 * 这主要用于 CAS 风格的循环。
 */
static uint32
WaitBufHdrUnlocked(BufferDesc *buf)
{
	SpinDelayStatus delayStatus;
	uint32		buf_state;

	init_local_spin_delay(&delayStatus);

	buf_state = pg_atomic_read_u32(&buf->state);

	while (buf_state & BM_LOCKED)
	{
		perform_spin_delay(&delayStatus);
		buf_state = pg_atomic_read_u32(&buf->state);
	}

	finish_spin_delay(&delayStatus);

	return buf_state;
}

/*
 * BufferTag 比较器。
 */
static inline int
buffertag_comparator(const BufferTag *ba, const BufferTag *bb)
{
	int			ret;
	RelFileLocator rlocatora;
	RelFileLocator rlocatorb;

	rlocatora = BufTagGetRelFileLocator(ba);
	rlocatorb = BufTagGetRelFileLocator(bb);

	ret = rlocator_comparator(&rlocatora, &rlocatorb);

	if (ret != 0)
		return ret;

	if (BufTagGetForkNum(ba) < BufTagGetForkNum(bb))
		return -1;
	if (BufTagGetForkNum(ba) > BufTagGetForkNum(bb))
		return 1;

	if (ba->blockNum < bb->blockNum)
		return -1;
	if (ba->blockNum > bb->blockNum)
		return 1;

	return 0;
}

/*
 * 确定检查点中写出顺序的比较器。
 *
 * 表空间先比较是重要的，在表空间之间平衡写的逻辑
 * 依赖这一点。
 */
static inline int
ckpt_buforder_comparator(const CkptSortItem *a, const CkptSortItem *b)
{
	/* 比较表空间 */
	if (a->tsId < b->tsId)
		return -1;
	else if (a->tsId > b->tsId)
		return 1;
	/* 比较关系 */
	if (a->relNumber < b->relNumber)
		return -1;
	else if (a->relNumber > b->relNumber)
		return 1;
	/* 比较分叉 */
	else if (a->forkNum < b->forkNum)
		return -1;
	else if (a->forkNum > b->forkNum)
		return 1;
	/* 比较块号 */
	else if (a->blockNum < b->blockNum)
		return -1;
	else if (a->blockNum > b->blockNum)
		return 1;
	/* 相等的页标识不大可能，但并非不可能 */
	return 0;
}

/*
 * 各表空间检查点完成进度上的最小堆的
 * 比较器。
 */
static int
ts_ckpt_progress_comparator(Datum a, Datum b, void *arg)
{
	CkptTsStatus *sa = (CkptTsStatus *) a;
	CkptTsStatus *sb = (CkptTsStatus *) b;

	/* 我们要最小堆，因此 a < b 时返回 1 */
	if (sa->progress < sb->progress)
		return 1;
	else if (sa->progress == sb->progress)
		return 0;
	else
		return -1;
}

/*
 * 初始化一个回写上下文，丢掉可能的先前状态。
 *
 * *max_pending 是指针而不是立即值，以便合并
 * 限制可以很容易地被 GUC 机制改变，并且调用代码不必
 * 检查当前配置。值为 0 表示不会执行
 * 回写控制。
 */
void
WritebackContextInit(WritebackContext *context, int *max_pending)
{
	Assert(*max_pending <= WRITEBACK_MAX_PENDING_FLUSHES);

	context->max_pending = max_pending;
	context->nr_pending = 0;
}

/*
 * 把缓冲区加入待回写请求列表。
 */
void
ScheduleBufferTagForWriteback(WritebackContext *wb_context, IOContext io_context,
							  BufferTag *tag)
{
	PendingWriteback *pending;

	if (io_direct_flags & IO_DIRECT_DATA)
		return;

	/*
	 * 把缓冲区加入待回写数组，除非回写控制被
	 * 禁用。
	 */
	if (*wb_context->max_pending > 0)
	{
		Assert(*wb_context->max_pending <= WRITEBACK_MAX_PENDING_FLUSHES);

		pending = &wb_context->pending_writebacks[wb_context->nr_pending++];

		pending->tag = *tag;
	}

	/*
	 * 若超过回写限制，就执行待处理的刷写。这
	 * 包括先前已加入一项、但现在控制
	 * 被禁用的情形。
	 */
	if (wb_context->nr_pending >= *wb_context->max_pending)
		IssuePendingWritebacks(wb_context, io_context);
}

#define ST_SORT sort_pending_writebacks
#define ST_ELEMENT_TYPE PendingWriteback
#define ST_COMPARE(a, b) buffertag_comparator(&a->tag, &b->tag)
#define ST_SCOPE static
#define ST_DEFINE
#include <lib/sort_template.h>

/*
 * 发出先前用 ScheduleBufferTagForWriteback 安排的
 * 全部待回写请求给操作系统。
 *
 * 因为这只用于改善操作系统的 IO 调度，我们尽量永不
 * 出错，它只是一个提示。
 */
void
IssuePendingWritebacks(WritebackContext *wb_context, IOContext io_context)
{
	instr_time	io_start;
	int			i;

	if (wb_context->nr_pending == 0)
		return;

	/*
	 * 按顺序执行写可以让它们快很多，并允许把对连续块的回写请求
	 * 合并成更大的回写。
	 */
	sort_pending_writebacks(wb_context->pending_writebacks,
							wb_context->nr_pending);

	io_start = pgstat_prepare_io_time();

	/*
	 * 合并相邻的写，但不合并别的。为此我们遍历
	 * 现在已排序的待刷写数组，并向前看以找出全部相邻
	 * （或相同）的写。
	 */
	for (i = 0; i < wb_context->nr_pending; i++)
	{
		PendingWriteback *cur;
		PendingWriteback *next;
		SMgrRelation reln;
		int			ahead;
		BufferTag	tag;
		RelFileLocator currlocator;
		Size		nblocks = 1;

		cur = &wb_context->pending_writebacks[i];
		tag = cur->tag;
		currlocator = BufTagGetRelFileLocator(&tag);

		/*
		 * 向前看随后的回写请求，看它们能否
		 * 与当前这个合并。
		 */
		for (ahead = 0; i + ahead + 1 < wb_context->nr_pending; ahead++)
		{

			next = &wb_context->pending_writebacks[i + ahead + 1];

			/* 不同的文件，停止 */
			if (!RelFileLocatorEquals(currlocator,
									  BufTagGetRelFileLocator(&next->tag)) ||
				BufTagGetForkNum(&cur->tag) != BufTagGetForkNum(&next->tag))
				break;

			/* 好，块被排队了两次，跳过 */
			if (cur->tag.blockNum == next->tag.blockNum)
				continue;

			/* 只合并连续的写 */
			if (cur->tag.blockNum + 1 != next->tag.blockNum)
				break;

			nblocks++;
			cur = next;
		}

		i += ahead;

		/* 最后告诉内核把数据写到存储 */
		reln = smgropen(currlocator, InvalidBackendId);
		smgrwriteback(reln, BufTagGetForkNum(&tag), tag.blockNum, nblocks);
	}

	/*
	 * 假定回写请求只对包含永久关系
	 * 块的缓冲区发出。
	 */
	pgstat_count_io_op_time(IOOBJECT_RELATION, io_context,
							IOOP_WRITEBACK, io_start, wb_context->nr_pending);

	wb_context->nr_pending = 0;
}


/*
 * 实现 TestForOldSnapshot 中较慢、较大的部分。
 *
 * 较小、较快的部分被放成内联，但整套逻辑对那样来说
 * 太大了。
 */
void
TestForOldSnapshot_impl(Snapshot snapshot, Relation relation)
{
	if (RelationAllowsEarlyPruning(relation)
		&& (snapshot)->whenTaken < GetOldSnapshotThresholdTimestamp())
		ereport(ERROR,
				(errcode(ERRCODE_SNAPSHOT_TOO_OLD),
				 errmsg("snapshot too old")));
}
