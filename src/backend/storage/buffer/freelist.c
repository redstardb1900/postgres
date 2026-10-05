/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))


/*
 * 共享空闲链表的控制信息。
 */
typedef struct
{
	/* 自旋锁：保护下面这些值 */
	slock_t		buffer_strategy_lock;

	/*
	 * 时钟清扫指针：下一个要考虑抓取的缓冲区的下标。注意
	 * 这不是一个具体的缓冲区，我们只增加该值。因此，要
	 * 得到实际缓冲区，需要把它对 NBuffers 取模。
	 */
	pg_atomic_uint32 nextVictimBuffer;

	int			firstFreeBuffer;	/* 未使用缓冲区链表的头 */
	int			lastFreeBuffer; /* 未使用缓冲区链表的尾 */

	/*
	 * 注意：当 firstFreeBuffer 为 -1（即
	 * 链表为空）时，lastFreeBuffer 是未定义的
	 */

	/*
	 * 统计信息。这些计数器应当足够宽，使它们在一次 bgwriter 周期内
	 * 没法溢出。
	 */
	uint32		completePasses; /* 时钟清扫的完整圈数 */
	pg_atomic_uint32 numBufferAllocs;	/* 自上次重置以来分配的缓冲区 */

	/*
	 * 活动时要通知的 bgworker 进程，若没有则为 -1。见
	 * StrategyNotifyBgWriter。
	 */
	int			bgwprocno;
} BufferStrategyControl;

/* 指向共享状态的指针 */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * 用来重用一环共享缓冲区的私有（非共享）状态。
 * 这目前是唯一一种 BufferAccessStrategy 对象，但将来
 * 我们也许会有更多种类。
 */
typedef struct BufferAccessStrategyData
{
	/* 总体策略类型 */
	BufferAccessStrategyType btype;
	/* buffers[] 缓冲区数组的元素个数 */
	int			nbuffers;

	/*
	 * 环里当前槽的下标，即最近一次
	 * 由 GetBufferFromRing 返回的那个。
	 */
	int			current;

	/*
	 * 缓冲区号数组。InvalidBuffer（即零）表示我们
	 * 尚未为这个环槽选择缓冲区。为了分配
	 * 简单，它与该结构的固定成员一起
	 * palloc。
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


/* 内部函数的原型 */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint32 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);

/*
 * ClockSweepTick - StrategyGetBuffer() 的辅助例程。
 *
 * 把时钟指针向前移动一个缓冲区，并返回
 * 指针现在所指缓冲区的标识。
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;

	/*
	 * 原子地把指针向前移动一个缓冲区。若有多个进程
	 * 在做这件事，可能导致返回的缓冲区稍微
	 * 偏离表面顺序。
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* 在 BufferDescriptors 里查找时总是回绕 */
		victim = victim % NBuffers;

		/*
		 * 若我们就是刚刚造成回绕的那个，就在持有自旋锁时强制
		 * 增加 completePasses。我们
		 * 需要自旋锁，以便 StrategySyncStart() 能返回由
		 * nextVictimBuffer 和 completePasses 组成的一致值。
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * 在增加 completePasses 时取得自旋锁。这
				 * 让其他读者能以一致的方式读取 nextVictimBuffer 和
				 * completePasses，StrategySyncStart() 需要这样。理论上推迟
				 * 这次增加可能导致 nextVictimBuffers 溢出，
				 * 但那极不可能，
				 * 而且不会特别有害。
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

/*
 * have_free_buffer -- 不加锁检查缓冲池里是否有空闲
 *					   缓冲区。
 *
 * 若结果为真，一旦空闲缓冲区被其他操作移走，
 * 它就会过时，因此严格想使用空闲缓冲区的调用者
 * 不应当调用这个。
 */
bool
have_free_buffer(void)
{
	if (StrategyControl->firstFreeBuffer >= 0)
		return true;
	else
		return false;
}

/*
 * StrategyGetBuffer
 *
 *	由 bufmgr 调用，以取得 BufferAlloc() 里下一个要用的候选缓冲区。
 *	BufferAlloc() 唯一的硬性要求是
 *	所选缓冲区目前不得被任何人钉住。
 *
 *	strategy 是 BufferAccessStrategy 对象，或对默认策略为 NULL。
 *
 *	为确保在我们钉住之前没有别人能钉住该缓冲区，我们必须
 *	在仍持有缓冲区描述符自旋锁时返回该缓冲区。
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint32 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;
	uint32		local_buf_state;	/* 以免反复解引用 */

	*from_ring = false;

	/*
	 * 若给定了策略对象，看它能否选出一个缓冲区。我们
	 * 假定策略对象不需要 buffer_strategy_lock。
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			return buf;
		}
	}

	/*
	 * 若被要求，我们需要唤醒 bgwriter。由于我们不想为此依赖
	 * 自旋锁，我们强制从共享内存读一次，然后
	 * 根据该值设置锁存器。我们需要走完这一步，
	 * 因为否则 bgwprocno 可能在我们检查时或之后被重置，因为
	 * 编译器可能直接从内存重读。
	 *
	 * 若 bgwriter 在错误的时刻死去，这可能设置错误进程的锁存器。
	 * 但由于 PGPROC->procLatch 从不
	 * 被释放，最坏的后果是我们设置了
	 * 某个任意进程的锁存器。
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* 先重置 bgwprocno，再设置锁存器 */
		StrategyControl->bgwprocno = -1;

		/*
		 * 这里不取得 ProcArrayLock，有点别扭。其实
		 * 没问题，因为 procLatch 从不会被释放，所以我们只是可能
		 * 设置错误进程（或没有进程）的锁存器。
		 */
		SetLatch(&ProcGlobal->allProcs[bgwprocno].procLatch);
	}

	/*
	 * 我们统计缓冲区分配请求，以便 bgwriter 能估计
	 * 缓冲区消耗速率。注意，被策略对象回收的缓冲区
	 * 有意不在这里计数。
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

	/*
	 * 先不取得锁，检查空闲链表里是否有缓冲区。既然我们
	 * 并非在每次 StrategyGetBuffer() 调用里都需要自旋锁，若在这里取得它
	 * 就可惜了，多数情况下
	 * 没有用。这显然留下一个竞态：缓冲区被
	 * 放上空闲链表，但我们还没看到那次存储。这相当
	 * 无害，它会在下一次缓冲区获取时被使用。
	 *
	 * 若空闲链表上有缓冲区，就取得自旋锁，弹出一个
	 * 缓冲区。然后检查该缓冲区是否可用，
	 * 若不可用就重复。
	 *
	 * 注意 freeNext 成员被视为由
	 * buffer_strategy_lock 保护，而不是由各个缓冲区自旋锁保护，因此不持有自旋锁
	 * 操作它们是可以的。
	 */
	if (StrategyControl->firstFreeBuffer >= 0)
	{
		while (true)
		{
			/* 取得自旋锁，以便从空闲链表去掉元素 */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

			if (StrategyControl->firstFreeBuffer < 0)
			{
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
				break;
			}

			buf = GetBufferDescriptor(StrategyControl->firstFreeBuffer);
			Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

			/* 无条件地从空闲链表去掉缓冲区 */
			StrategyControl->firstFreeBuffer = buf->freeNext;
			buf->freeNext = FREENEXT_NOT_IN_LIST;

			/*
			 * 释放锁，以便我们检查这个缓冲区时
			 * 别人可以访问空闲链表。
			 */
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/*
			 * 若该缓冲区被钉住或 usage_count 非零，我们就无法
			 * 使用它；丢掉它并重试。（这只可能发生在 VACUUM
			 * 把一个有效缓冲区放进空闲链表，然后别人在我们拿到它之前
			 * 使用了它。自 8.3 起这大概完全不可能，但
			 * 我们最好还是检查。）
			 */
			local_buf_state = LockBufHdr(buf);
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
				&& BUF_STATE_GET_USAGECOUNT(local_buf_state) == 0)
			{
				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				return buf;
			}
			UnlockBufHdr(buf, local_buf_state);
		}
	}

	/* 空闲链表上什么也没有，因此运行时钟清扫算法 */
	trycounter = NBuffers;
	for (;;)
	{
		buf = GetBufferDescriptor(ClockSweepTick());

		/*
		 * 若该缓冲区被钉住或 usage_count 非零，我们就无法使用
		 * 它；减少 usage_count（除非被钉住）并继续扫描。
		 */
		local_buf_state = LockBufHdr(buf);

		if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
		{
			if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			{
				local_buf_state -= BUF_USAGECOUNT_ONE;

				trycounter = NBuffers;
			}
			else
			{
				/* 找到一个可用的缓冲区 */
				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				return buf;
			}
		}
		else if (--trycounter == 0)
		{
			/*
			 * 我们扫描了全部缓冲区而没有改变任何状态，
			 * 因此全部缓冲区都被钉住（或我们看它们时是钉住的）。
			 * 我们可以希望最终有人会释放一个，但
			 * 失败大概比冒陷入
			 * 无限循环的风险更好。
			 */
			UnlockBufHdr(buf, local_buf_state);
			elog(ERROR, "no unpinned buffers available");
		}
		UnlockBufHdr(buf, local_buf_state);
	}
}

/*
 * StrategyFreeBuffer：把一个缓冲区放到空闲链表上
 */
void
StrategyFreeBuffer(BufferDesc *buf)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

	/*
	 * 我们可能被要求把已经在空闲链表里的东西放进去；
	 * 若如此，不要把链表弄乱。
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;
	}

	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

/*
 * StrategySyncStart -- 告诉 BgBufferSync 从哪里开始同步。
 *
 * 结果是最好先同步的缓冲区的下标。
 * BgBufferSync() 将从那里绕着缓冲区数组环形前进。
 *
 * 此外，若传入非 NULL 指针，我们返回已完成的圈数（实际上是
 * nextVictimBuffer 的高位）和最近缓冲区
 * 分配的计数。分配计数在
 * 被读取后重置。
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32		nextVictimBuffer;
	int			result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * 另外加上 completePasses 得以增加之前发生的回绕次数。
		 * 参见 ClockSweepTick()。
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- 设置或清除分配通知锁存器。
 *
 * 若 bgwprocno 不是 -1，下一次 StrategyGetBuffer 调用会
 * 设置那个锁存器。传入 -1 可在通知发生前清除待处理的通知。
 * 这个特性由 bgwriter 进程用来把自己从休眠中唤醒，
 * 并不打算给其他任何人使用。
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * 我们取得 buffer_strategy_lock 只是为了确保这次存储对
	 * StrategyGetBuffer 看起来是原子的。bgwriter 应当相当
	 * 不频繁地调用这个，因此稳妥没有性能代价。
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyShmemSize
 *
 * 估计空闲链表相关结构所用的共享内存大小。
 *
 * 注意：由于一些历史原因，缓冲区查找散列表的大小
 * 也在这里决定。
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* 查找散列表的大小……见 StrategyInitialize 里的注释 */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* 共享替换策略控制块的大小 */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	return size;
}

/*
 * StrategyInitialize -- 初始化缓冲池的缓冲区替换
 *		策略。
 *
 * 假定：全部缓冲区都已经建成链表。
 *		只由 postmaster 在初始化期间调用。
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	/*
	 * 初始化共享缓冲区查找散列表。
	 *
	 * 由于我们没法容忍查找表项用尽，我们必须
	 * 确保这里指定足够的表大小。最大稳态
	 * 用量当然是 NBuffers 项，但 BufferAlloc() 试图在删除旧项之前插入
	 * 新项。原则上每个分区可能
	 * 并发发生这件事，因此我们可能需要多达
	 * NBuffers + NUM_BUFFER_PARTITIONS 项。
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * 取得或创建共享策略控制块
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * 只做一次，通常在 postmaster 里
		 */
		Assert(init);

		SpinLockInit(&StrategyControl->buffer_strategy_lock);

		/*
		 * 为我们的策略抓取整条空闲缓冲区链表。我们
		 * 假定它先前已由 InitBufferPool() 建好。
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* 初始化时钟清扫指针 */
		pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0);

		/* 清除统计信息 */
		StrategyControl->completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

		/* 没有待处理的通知 */
		StrategyControl->bgwprocno = -1;
	}
	else
		Assert(!init);
}


/* ----------------------------------------------------------------
 *				后端私有的缓冲区环管理
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- 创建一个 BufferAccessStrategy 对象。
 *
 * 该对象在当前内存上下文里分配。
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * 选择要使用的缓冲区环大小。理由见 buffer/README。
	 *
	 * 注意：若改变 BAS_BULKREAD 的环大小，也见
	 * access/heap/syncscan.c 里的 SYNC_SCAN_REPORT_INTERVAL。
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* 若有人要 NORMAL，就给它一个默认对象 */
			return NULL;

		case BAS_BULKREAD:
			ring_size_kb = 256;
			break;
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 256;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* 消除编译器告警 */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- 创建一个 BufferAccessStrategy 对象，其
 *		缓冲区数量相当于传入的大小。
 *
 * 若给定的环大小为 0，就不会创建 BufferAccessStrategy，并且
 * 函数返回 NULL。ring_size_kb 不得为负。
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* ring_size_kb 相当于多少个缓冲区 */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 表示无限制，因此不需要 BufferAccessStrategy */
	if (ring_buffers == 0)
		return NULL;

	/* 封顶为 shared_buffers 的 1/8 */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers 决不应当小于 16，因此这不应当发生 */
	Assert(ring_buffers > 0);

	/* 分配该对象，并把全部元素初始化为零 */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* 设置那些起初不为零的成员 */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- 环里缓冲区个数的
 *		访问器。
 *
 * 对 NULL 输入返回 0，以匹配 GetAccessStrategyWithSize()
 * 在大小为 0 时返回 NULL 的行为。
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * FreeAccessStrategy -- 释放一个 BufferAccessStrategy 对象。
 *
 * 此刻简单的 pfree 就行，但我们更希望调用者
 * 不要对 BufferAccessStrategy 的表示假定那么多。
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* 若对默认策略调用，不要崩溃 */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- 从环里返回一个缓冲区，若环
 *		为空或不可用则返回 NULL。
 *
 * 返回的缓冲区上持有缓冲区描述符自旋锁。
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint32 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint32		local_buf_state;	/* 以免反复解引用 */


	/* 前进到下一个环槽 */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * 若该槽尚未填入，就告诉调用者用普通分配策略分配一个新
	 * 缓冲区。然后他会调用 AddBufferToRing，用新缓冲区
	 * 填入这个槽。
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	/*
	 * 若该缓冲区被钉住，我们在任何情况下都无法使用它。
	 *
	 * 若 usage_count 为 0 或 1，该缓冲区就可以用（我们预期是 1，
	 * 因为我们自己先前使用该环元素会把它留在
	 * 那里，但此后时钟清扫可能已经把它减少了）。更高的
	 * usage_count 表示别人碰过该缓冲区，因此我们
	 * 不应当重用它。
	 */
	buf = GetBufferDescriptor(bufnum - 1);
	local_buf_state = LockBufHdr(buf);
	if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
		&& BUF_STATE_GET_USAGECOUNT(local_buf_state) <= 1)
	{
		*buf_state = local_buf_state;
		return buf;
	}
	UnlockBufHdr(buf, local_buf_state);

	/*
	 * 告诉调用者用普通分配策略分配一个新缓冲区。
	 * 然后他会通过 AddBufferToRing 替换这个环元素。
	 */
	return NULL;
}

/*
 * AddBufferToRing -- 把一个缓冲区加入缓冲区环。
 *
 * 调用者必须持有该缓冲区的缓冲区描述符自旋锁。由于这
 * 在持有自旋锁时调用，它最好相当便宜。
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * 返回给定 BufferAccessStrategy 的策略环的 IOContext 的
 * 实用函数。
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * 目前，GetAccessStrategy() 对
			 * BufferAccessStrategyType BAS_NORMAL 返回 NULL，因此这个分支
			 * 不可达。
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- 考虑拒绝一个脏页。
 *
 * 使用非默认策略时，缓冲区管理器调用本函数，
 * 当 StrategyGetBuffer 选出的缓冲区需要被写出，
 * 并且这样做需要刷写 WAL。这给我们
 * 一个另选牺牲者的机会。
 *
 * 若缓冲区管理器应当再要一个牺牲者就返回真，
 * 若应当写出并重用这个缓冲区就返回假。
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* 我们只在批量读模式下这样做 */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* 不要乱动普通缓冲区替换策略的行为 */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * 把脏页从缓冲区环里去掉；若环成员全都是脏页，
	 * 这是防止无限循环所必需的。
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}
