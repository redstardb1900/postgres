/*-------------------------------------------------------------------------
 *
 * shm_mq.c
 *	  单读者、单写者的共享内存消息队列
 *
 * 发送方和接收方都必须有一个 PGPROC；各自的
 * 进程锁存器用于同步。只有发送方可以发送，
 * 也只有接收方可以接收。这是为了让用户
 * 后端与它注册的工作后端通信。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/storage/ipc/shm_mq.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "postmaster/bgworker.h"
#include "storage/procsignal.h"
#include "storage/shm_mq.h"
#include "storage/spin.h"
#include "utils/memutils.h"

/*
 * 本结构表示实际的队列，存放在共享内存中。
 *
 * 关于同步的若干说明：
 *
 * mq_receiver 和 mq_bytes_read 只能由接收方修改；
 * mq_sender 和 mq_bytes_written 只能由发送方修改。
 * mq_receiver 和 mq_sender 由 mq_mutex 保护，不过重要的是，
 * 它们一旦设置就无法再改变，因此在已知这一点之后，
 * 可以不加锁读取。
 *
 * mq_bytes_read 和 mq_bytes_written 不受该互斥锁保护。相反，
 * 它们用 8 字节的加载和存储原子地写入。必须小心地使用内存屏障，
 * 把对这些值的读写与
 * 对 mq_ring 中实际数据的读写同步起来。
 *
 * mq_detached 不需要加锁。发送方或
 * 接收方都可以设置它，但只会从 false 变成 true，因此重复写也
 * 没关系。重要的是，若我们设置 mq_detached 然后再设置
 * 对端的锁存器，对端在醒来之后必须能看见这一变更。
 * 由于 SetLatch 以内存屏障开始，ResetLatch
 * 以内存屏障结束，这样就应当没问题。
 *
 * mq_ring_size 和 mq_ring_offset 在初始化之后不再改变，
 * 因此可以不加锁读取。
 *
 * 重要的是，mq_ring 可以不加锁地安全读写。
 * 在任一时刻，mq_bytes_read 与
 * mq_bytes_written 之差决定 mq_ring 里有多少字节
 * 含有未读数据，而 mq_bytes_read 决定这些字节
 * 从哪里开始。发送方随时可以增加未读字节数，
 * but only the receiver can give license to overwrite those bytes, by
 * 增加 mq_bytes_read。因此，接收方不加锁读取
 * 它知道存在的未读字节是安全的。反过来，
 * 发送方也可以不加锁地写入环形缓冲区中未使用的部分，
 * 因为没有别人会去读或写那些字节。
 * 接收方可能通过增加 mq_bytes_read 让更多字节变为未使用，
 * 但这没关系。注意，接收方去读任何
 * 已经标为已读的数据，或者写入任何数据，都是不安全的；发送方在增加
 * mq_bytes_written 之后再重读任何数据也是不安全的，好在
 * 这些事都没有必要。
 */
struct shm_mq
{
	slock_t		mq_mutex;
	PGPROC	   *mq_receiver;
	PGPROC	   *mq_sender;
	pg_atomic_uint64 mq_bytes_read;
	pg_atomic_uint64 mq_bytes_written;
	Size		mq_ring_size;
	bool		mq_detached;
	uint8		mq_ring_offset;
	char		mq_ring[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * 本结构是访问队列的后端私有句柄。
 *
 * mqh_queue 指向我们已挂接的队列，mqh_segment 是
 * 指向包含该队列的动态共享内存段的可选指针。
 * （若提供了 mqh_segment，我们就登记一个 on_dsm_detach 回调，
 * 以确保在脱离 DSM 之前先脱离队列。）
 *
 * 若这个队列打算把当前进程与启动它的后台
 * 工作进程连起来，用户可以把工作进程句柄的指针
 * 传给 shm_mq_attach()，我们会把它存进 mqh_handle。这样做的意义
 * 是让我们在将要通信的进程甚至还没启动时，
 * 就能开始向该队列发送或从中接收。若它启动
 * 失败，这个句柄让我们能够察觉并干净地失败，而不是
 * 永远等下去；见 shm_mq_wait_internal。这主要在
 * 简单情形下有用——例如只有 2 个进程在通信；在
 * 更复杂的场景里，并非每个进程都有 BackgroundWorkerHandle
 * 可用，或者可能得同时留意不止一个其他
 * 进程的失败。
 *
 * 当一条消息作为连续的字节块存在于队列中时——也就是说，
 * 它小于环形缓冲区的大小，并且没有绕过
 * 末尾——我们就把指向缓冲区内部的指针返回给调用方。
 * 对于更大或恰好回绕的消息，我们在本地重新组装，
 * 把各块拷贝进后端本地缓冲区。mqh_buffer 就是
 * 这块缓冲区，mqh_buflen 是为它分配的字节数。
 *
 * mqh_send_pending 是已经写入队列、但尚未
 * 在共享内存中更新的字节数。要等到已写
 * 数据达到环形区大小的 1/4，或元组队列已满，我们才会更新它。这样可以
 * 避免频繁的 CPU 缓存未命中，也可以避免频繁
 * 调用 SetLatch()，那相当昂贵。
 *
 * mqh_partial_bytes、mqh_expected_bytes 和 mqh_length_word_complete
 * 用来跟踪非阻塞操作的状态。当调用方
 * 尝试一次返回 SHM_MQ_WOULD_BLOCK 的非阻塞操作时，
 * 预期稍后用同样的参数重试这次调用；
 * 我们得保留足够的状态，以便从停下的地方继续。
 * mqh_length_word_complete 跟踪我们是否已经发送或接收完
 * （看我们正在做哪一件）整个长度字。mqh_partial_bytes 跟踪
 * 长度字或消息本身已经读或写的字节数，
 * 而 mqh_expected_bytes——只用于读取——
 * 跟踪有效载荷的预期总大小。
 *
 * mqh_counterparty_attached 跟踪我们是否已经知道对端
 * 在先前某个时刻挂接了队列。这样就可以省掉一些
 * 互斥锁获取。
 *
 * mqh_context 是我们挂接 shm_mq 时生效的内存上下文。
 * shm_mq_handle 本身就分配在这个上下文里，并且
 * 我们确保所做的其他分配也发生在这个上下文里，
 * 以免出现讨厌的意外。
 */
struct shm_mq_handle
{
	shm_mq	   *mqh_queue;
	dsm_segment *mqh_segment;
	BackgroundWorkerHandle *mqh_handle;
	char	   *mqh_buffer;
	Size		mqh_buflen;
	Size		mqh_consume_pending;
	Size		mqh_send_pending;
	Size		mqh_partial_bytes;
	Size		mqh_expected_bytes;
	bool		mqh_length_word_complete;
	bool		mqh_counterparty_attached;
	MemoryContext mqh_context;
};

static void shm_mq_detach_internal(shm_mq *mq);
static shm_mq_result shm_mq_send_bytes(shm_mq_handle *mqh, Size nbytes,
									   const void *data, bool nowait, Size *bytes_written);
static shm_mq_result shm_mq_receive_bytes(shm_mq_handle *mqh,
										  Size bytes_needed, bool nowait, Size *nbytesp,
										  void **datap);
static bool shm_mq_counterparty_gone(shm_mq *mq,
									 BackgroundWorkerHandle *handle);
static bool shm_mq_wait_internal(shm_mq *mq, PGPROC **ptr,
								 BackgroundWorkerHandle *handle);
static void shm_mq_inc_bytes_read(shm_mq *mq, Size n);
static void shm_mq_inc_bytes_written(shm_mq *mq, Size n);
static void shm_mq_detach_callback(dsm_segment *seg, Datum arg);

/* 最小队列大小足以放下头部，外加至少一块数据。 */
const Size	shm_mq_minimum_size =
MAXALIGN(offsetof(shm_mq, mq_ring)) + MAXIMUM_ALIGNOF;

#define MQH_INITIAL_BUFSIZE				8192

/*
 * 初始化一个新的共享消息队列。
 */
shm_mq *
shm_mq_create(void *address, Size size)
{
	shm_mq	   *mq = address;
	Size		data_offset = MAXALIGN(offsetof(shm_mq, mq_ring));

	/* 若大小没有按 MAXALIGN 对齐，就丢掉多余的字节。 */
	size = MAXALIGN_DOWN(size);

	/* 队列大小必须大到能放下一些数据。 */
	Assert(size > data_offset);

	/* 初始化队列头部。 */
	SpinLockInit(&mq->mq_mutex);
	mq->mq_receiver = NULL;
	mq->mq_sender = NULL;
	pg_atomic_init_u64(&mq->mq_bytes_read, 0);
	pg_atomic_init_u64(&mq->mq_bytes_written, 0);
	mq->mq_ring_size = size - data_offset;
	mq->mq_detached = false;
	mq->mq_ring_offset = data_offset - offsetof(shm_mq, mq_ring);

	return mq;
}

/*
 * 设置将从共享消息
 * 队列接收的进程身份。
 */
void
shm_mq_set_receiver(shm_mq *mq, PGPROC *proc)
{
	PGPROC	   *sender;

	SpinLockAcquire(&mq->mq_mutex);
	Assert(mq->mq_receiver == NULL);
	mq->mq_receiver = proc;
	sender = mq->mq_sender;
	SpinLockRelease(&mq->mq_mutex);

	if (sender != NULL)
		SetLatch(&sender->procLatch);
}

/*
 * 设置将向共享消息队列发送的进程身份。
 */
void
shm_mq_set_sender(shm_mq *mq, PGPROC *proc)
{
	PGPROC	   *receiver;

	SpinLockAcquire(&mq->mq_mutex);
	Assert(mq->mq_sender == NULL);
	mq->mq_sender = proc;
	receiver = mq->mq_receiver;
	SpinLockRelease(&mq->mq_mutex);

	if (receiver != NULL)
		SetLatch(&receiver->procLatch);
}

/*
 * 取得已配置的接收方。
 */
PGPROC *
shm_mq_get_receiver(shm_mq *mq)
{
	PGPROC	   *receiver;

	SpinLockAcquire(&mq->mq_mutex);
	receiver = mq->mq_receiver;
	SpinLockRelease(&mq->mq_mutex);

	return receiver;
}

/*
 * 取得已配置的发送方。
 */
PGPROC *
shm_mq_get_sender(shm_mq *mq)
{
	PGPROC	   *sender;

	SpinLockAcquire(&mq->mq_mutex);
	sender = mq->mq_sender;
	SpinLockRelease(&mq->mq_mutex);

	return sender;
}

/*
 * 挂接共享消息队列，以便发送或接收消息。
 *
 * 调用本函数时生效的内存上下文应当
 * 至少与消息队列本身活得一样久。
 * 我们会在该上下文中分配句柄，以后为缓冲进入数据
 * 所需的分配也会发生在该上下文中。
 *
 * 若 seg != NULL，那段动态共享内存脱离时，
 * 队列会自动脱离。
 *
 * 若 handle != NULL，即使另一个进程还没
 * 挂接，也可以读写队列。需要的话我们会等它挂接。该
 * 句柄必须属于一个后台工作进程，其 bgw_notify_pid
 * 等于我们的 PID。
 *
 * 用完后应当调用 shm_mq_detach()。这会释放
 * shm_mq_handle，并把队列本身标为已脱离，这样我们的
 * 对端就不会在我们已经失去兴趣之后，还卡在等待我们
 * 填满或排空队列。
 */
shm_mq_handle *
shm_mq_attach(shm_mq *mq, dsm_segment *seg, BackgroundWorkerHandle *handle)
{
	shm_mq_handle *mqh = palloc(sizeof(shm_mq_handle));

	Assert(mq->mq_receiver == MyProc || mq->mq_sender == MyProc);
	mqh->mqh_queue = mq;
	mqh->mqh_segment = seg;
	mqh->mqh_handle = handle;
	mqh->mqh_buffer = NULL;
	mqh->mqh_buflen = 0;
	mqh->mqh_consume_pending = 0;
	mqh->mqh_send_pending = 0;
	mqh->mqh_partial_bytes = 0;
	mqh->mqh_expected_bytes = 0;
	mqh->mqh_length_word_complete = false;
	mqh->mqh_counterparty_attached = false;
	mqh->mqh_context = CurrentMemoryContext;

	if (seg != NULL)
		on_dsm_detach(seg, shm_mq_detach_callback, PointerGetDatum(mq));

	return mqh;
}

/*
 * 把 BackgroundWorkerHandle 关联到 shm_mq_handle，就像它
 * 当初被传给 shm_mq_attach 一样。
 */
void
shm_mq_set_handle(shm_mq_handle *mqh, BackgroundWorkerHandle *handle)
{
	Assert(mqh->mqh_handle == NULL);
	mqh->mqh_handle = handle;
}

/*
 * 把一条消息写入共享消息队列。
 */
shm_mq_result
shm_mq_send(shm_mq_handle *mqh, Size nbytes, const void *data, bool nowait,
			bool force_flush)
{
	shm_mq_iovec iov;

	iov.data = data;
	iov.len = nbytes;

	return shm_mq_sendv(mqh, &iov, 1, nowait, force_flush);
}

/*
 * 把从多个地址汇集来的一条消息
 * 写入共享消息队列。
 *
 * 当 nowait = false 时，环形缓冲区写满后我们会在自己的进程锁存器上等待，
 * 等接收方排掉一些数据后再继续写。
 * 每次等待之后都会重置进程锁存器。
 *
 * 当 nowait = true 时，我们不改动进程锁存器的状态；
 * 相反，若缓冲区变满，就返回 SHM_MQ_WOULD_BLOCK。在
 * 这种情况下，每当进程锁存器被设置，调用方都应当用同样的
 * 参数再次调用本函数。（一旦开始，除了脱离队列之外，
 * 消息的发送无法中止；改变
 * 长度或有效载荷会破坏队列。）
 *
 * 当 force_flush = true 时，我们立即更新 shm_mq 的 mq_bytes_written，
 * 并通知接收方（若它已经挂接）。否则，要等到已写数据
 * 超过环形区大小的 1/4，我们才
 * 更新它。
 */
shm_mq_result
shm_mq_sendv(shm_mq_handle *mqh, shm_mq_iovec *iov, int iovcnt, bool nowait,
			 bool force_flush)
{
	shm_mq_result res;
	shm_mq	   *mq = mqh->mqh_queue;
	PGPROC	   *receiver;
	Size		nbytes = 0;
	Size		bytes_written;
	int			i;
	int			which_iov = 0;
	Size		offset;

	Assert(mq->mq_sender == MyProc);

	/* 计算写入的总大小。 */
	for (i = 0; i < iovcnt; ++i)
		nbytes += iov[i].len;

	/* 防止写出的消息压垮接收方。 */
	if (nbytes > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot send a message of size %zu via shared memory queue",
						nbytes)));

	/* 尝试把长度字写入缓冲区，或把没写完的写完。 */
	while (!mqh->mqh_length_word_complete)
	{
		Assert(mqh->mqh_partial_bytes < sizeof(Size));
		res = shm_mq_send_bytes(mqh, sizeof(Size) - mqh->mqh_partial_bytes,
								((char *) &nbytes) + mqh->mqh_partial_bytes,
								nowait, &bytes_written);

		if (res == SHM_MQ_DETACHED)
		{
			/* 重置状态，以防调用方再发送另一条消息。 */
			mqh->mqh_partial_bytes = 0;
			mqh->mqh_length_word_complete = false;
			return res;
		}
		mqh->mqh_partial_bytes += bytes_written;

		if (mqh->mqh_partial_bytes >= sizeof(Size))
		{
			Assert(mqh->mqh_partial_bytes == sizeof(Size));

			mqh->mqh_partial_bytes = 0;
			mqh->mqh_length_word_complete = true;
		}

		if (res != SHM_MQ_SUCCESS)
			return res;

		/* 除非大于所需对齐，否则长度字无法被拆开。 */
		Assert(mqh->mqh_length_word_complete || sizeof(Size) > MAXIMUM_ALIGNOF);
	}

	/* 把实际的数据字节写入缓冲区。 */
	Assert(mqh->mqh_partial_bytes <= nbytes);
	offset = mqh->mqh_partial_bytes;
	do
	{
		Size		chunksize;

		/* 弄清接下来要发送哪些字节。 */
		if (offset >= iov[which_iov].len)
		{
			offset -= iov[which_iov].len;
			++which_iov;
			if (which_iov >= iovcnt)
				break;
			continue;
		}

		/*
		 * 我们想尽可能避免拷贝数据，但写入队列的
		 * 每一块字节都必须按 MAXALIGN 对齐，最后一块
		 * 除外。因此，若非最后一块结束在
		 * 非 MAXALIGN 对齐的边界上，就必须把它尾部的
		 * 数据与后面一块或多块的数据拼起来，直到
		 * 到达最后一块，或者累计的字节数
		 * 已经按 MAXALIGN 对齐。
		 */
		if (which_iov + 1 < iovcnt &&
			offset + MAXIMUM_ALIGNOF > iov[which_iov].len)
		{
			char		tmpbuf[MAXIMUM_ALIGNOF];
			int			j = 0;

			for (;;)
			{
				if (offset < iov[which_iov].len)
				{
					tmpbuf[j] = iov[which_iov].data[offset];
					j++;
					offset++;
					if (j == MAXIMUM_ALIGNOF)
						break;
				}
				else
				{
					offset -= iov[which_iov].len;
					which_iov++;
					if (which_iov >= iovcnt)
						break;
				}
			}

			res = shm_mq_send_bytes(mqh, j, tmpbuf, nowait, &bytes_written);

			if (res == SHM_MQ_DETACHED)
			{
				/* 重置状态，以防调用方再发送另一条消息。 */
				mqh->mqh_partial_bytes = 0;
				mqh->mqh_length_word_complete = false;
				return res;
			}

			mqh->mqh_partial_bytes += bytes_written;
			if (res != SHM_MQ_SUCCESS)
				return res;
			continue;
		}

		/*
		 * 若这是最后一块，即使它
		 * 不是 MAXIMUM_ALIGNOF 的倍数，也可以把数据全部写出。否则，写入大小需要
		 * 按 MAXALIGN_DOWN 向下对齐。
		 */
		chunksize = iov[which_iov].len - offset;
		if (which_iov + 1 < iovcnt)
			chunksize = MAXALIGN_DOWN(chunksize);
		res = shm_mq_send_bytes(mqh, chunksize, &iov[which_iov].data[offset],
								nowait, &bytes_written);

		if (res == SHM_MQ_DETACHED)
		{
			/* 重置状态，以防调用方再发送另一条消息。 */
			mqh->mqh_length_word_complete = false;
			mqh->mqh_partial_bytes = 0;
			return res;
		}

		mqh->mqh_partial_bytes += bytes_written;
		offset += bytes_written;
		if (res != SHM_MQ_SUCCESS)
			return res;
	} while (mqh->mqh_partial_bytes < nbytes);

	/* 为下一条消息重置。 */
	mqh->mqh_partial_bytes = 0;
	mqh->mqh_length_word_complete = false;

	/* 若队列已脱离，通知调用方。 */
	if (mq->mq_detached)
		return SHM_MQ_DETACHED;

	/*
	 * 若已知对端已经挂接，就可以不加自旋锁地读取 mq_receiver。
	 * 否则需要更谨慎。
	 */
	if (mqh->mqh_counterparty_attached)
		receiver = mq->mq_receiver;
	else
	{
		SpinLockAcquire(&mq->mq_mutex);
		receiver = mq->mq_receiver;
		SpinLockRelease(&mq->mq_mutex);
		if (receiver != NULL)
			mqh->mqh_counterparty_attached = true;
	}

	/*
	 * 若调用方要求强制刷出，或者我们已经写了超过
	 * 环形区大小的 1/4，就在共享内存中把它标为已写，并通知
	 * 接收方。
	 */
	if (force_flush || mqh->mqh_send_pending > (mq->mq_ring_size >> 2))
	{
		shm_mq_inc_bytes_written(mq, mqh->mqh_send_pending);
		if (receiver != NULL)
			SetLatch(&receiver->procLatch);
		mqh->mqh_send_pending = 0;
	}

	return SHM_MQ_SUCCESS;
}

/*
 * 从共享消息队列接收一条消息。
 *
 * 我们把 *nbytes 设为消息长度，把 *data 指向消息
 * 有效载荷。若整条消息在队列里是单独的、
 * 连续的一块，*data 会直接指向共享内存；否则
 * 会指向一块临时缓冲区。在消息相对缓冲区较短这一
 * 所希望的常见情形下，这大多避免了数据拷贝，
 * 同时仍然允许更长的消息。无论哪种情况，返回值
 * 都保持有效，直到对该队列执行下一次接收操作。
 *
 * 当 nowait = false 时，环形缓冲区为空且我们还没收到完整消息时，
 * 会在自己的进程锁存器上等待。发送方
 * 写入更多数据后会设置我们的进程锁存器，然后我们继续
 * 处理。因此每次调用都会返回一条完整消息
 * （除非发送方脱离了队列）。
 *
 * 当 nowait = true 时，我们不改动进程锁存器的状态；
 * 相反，每当缓冲区为空而我们又需要从中读取时，就
 * 返回 SHM_MQ_WOULD_BLOCK。在这种情况下，进程锁存器被设置之后，
 * 调用方应当再次调用本函数。
 */
shm_mq_result
shm_mq_receive(shm_mq_handle *mqh, Size *nbytesp, void **datap, bool nowait)
{
	shm_mq	   *mq = mqh->mqh_queue;
	shm_mq_result res;
	Size		rb = 0;
	Size		nbytes;
	void	   *rawdata;

	Assert(mq->mq_receiver == MyProc);

	/* 发送方挂接之前，我们无法接收数据。 */
	if (!mqh->mqh_counterparty_attached)
	{
		if (nowait)
		{
			int			counterparty_gone;

			/*
			 * 除非发送方
			 * 还没挂接，否则此刻根本不应当返回。不过，正确的返回值取决于
			 * 发送方是否仍然挂接着。若先测试
			 * 发送方是否曾经挂接，再测试发送方是否已
			 * 脱离，就有竞态：一个很快挂接又很快脱离的发送方
			 * 可能骗过我们，让我们以为
			 * 发送方从未挂接。因此，先测试我们的
			 * 对端是否已经确定离开，然后再
			 * 检查发送方当初是否曾经挂接。
			 */
			counterparty_gone = shm_mq_counterparty_gone(mq, mqh->mqh_handle);
			if (shm_mq_get_sender(mq) == NULL)
			{
				if (counterparty_gone)
					return SHM_MQ_DETACHED;
				else
					return SHM_MQ_WOULD_BLOCK;
			}
		}
		else if (!shm_mq_wait_internal(mq, &mq->mq_sender, mqh->mqh_handle)
				 && shm_mq_get_sender(mq) == NULL)
		{
			mq->mq_detached = true;
			return SHM_MQ_DETACHED;
		}
		mqh->mqh_counterparty_attached = true;
	}

	/*
	 * 若已经消费的数据量超过环形区大小的 1/4，
	 * 就在共享内存中把它标为已消费。只有消费了少量数据时，
	 * 我们尽量避免做这件事，
	 * 因为 SetLatch() 相当贵，我们不想做得太
	 * 频繁。
	 */
	if (mqh->mqh_consume_pending > mq->mq_ring_size / 4)
	{
		shm_mq_inc_bytes_read(mq, mqh->mqh_consume_pending);
		mqh->mqh_consume_pending = 0;
	}

	/* 尝试从缓冲区读出长度字，或把没读完的读完。 */
	while (!mqh->mqh_length_word_complete)
	{
		/* 尝试接收消息长度字。 */
		Assert(mqh->mqh_partial_bytes < sizeof(Size));
		res = shm_mq_receive_bytes(mqh, sizeof(Size) - mqh->mqh_partial_bytes,
								   nowait, &rb, &rawdata);
		if (res != SHM_MQ_SUCCESS)
			return res;

		/*
		 * 最好能一次收到整个消息长度字。
		 * 但若 sizeof(Size) > MAXIMUM_ALIGNOF，它就可能被拆到
		 * 多次读取里。
		 */
		if (mqh->mqh_partial_bytes == 0 && rb >= sizeof(Size))
		{
			Size		needed;

			nbytes = *(Size *) rawdata;

			/* 若已经拿到整条消息，就完成了。 */
			needed = MAXALIGN(sizeof(Size)) + MAXALIGN(nbytes);
			if (rb >= needed)
			{
				mqh->mqh_consume_pending += needed;
				*nbytesp = nbytes;
				*datap = ((char *) rawdata) + MAXALIGN(sizeof(Size));
				return SHM_MQ_SUCCESS;
			}

			/*
			 * 我们还没有整条消息，但至少已经有了整个
			 * 长度字。
			 */
			mqh->mqh_expected_bytes = nbytes;
			mqh->mqh_length_word_complete = true;
			mqh->mqh_consume_pending += MAXALIGN(sizeof(Size));
			rb -= MAXALIGN(sizeof(Size));
		}
		else
		{
			Size		lengthbytes;

			/* 除非大于所需对齐，否则无法被拆开。 */
			Assert(sizeof(Size) > MAXIMUM_ALIGNOF);

			/* 消息字被拆开了；需要缓冲区来重新组装。 */
			if (mqh->mqh_buffer == NULL)
			{
				mqh->mqh_buffer = MemoryContextAlloc(mqh->mqh_context,
													 MQH_INITIAL_BUFSIZE);
				mqh->mqh_buflen = MQH_INITIAL_BUFSIZE;
			}
			Assert(mqh->mqh_buflen >= sizeof(Size));

			/* 拷贝不完整的长度字；记住要消费它。 */
			if (mqh->mqh_partial_bytes + rb > sizeof(Size))
				lengthbytes = sizeof(Size) - mqh->mqh_partial_bytes;
			else
				lengthbytes = rb;
			memcpy(&mqh->mqh_buffer[mqh->mqh_partial_bytes], rawdata,
				   lengthbytes);
			mqh->mqh_partial_bytes += lengthbytes;
			mqh->mqh_consume_pending += MAXALIGN(lengthbytes);
			rb -= lengthbytes;

			/* 若现在已经有了整个字，就可以读有效载荷了。 */
			if (mqh->mqh_partial_bytes >= sizeof(Size))
			{
				Assert(mqh->mqh_partial_bytes == sizeof(Size));
				mqh->mqh_expected_bytes = *(Size *) mqh->mqh_buffer;
				mqh->mqh_length_word_complete = true;
				mqh->mqh_partial_bytes = 0;
			}
		}
	}
	nbytes = mqh->mqh_expected_bytes;

	/*
	 * 发送方那边应当已经禁止了，但最好在接收方也检查并
	 * 报错，而不是试图去读一条
	 * 大得离谱的消息。
	 */
	if (nbytes > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("invalid message size %zu in shared memory queue",
						nbytes)));

	if (mqh->mqh_partial_bytes == 0)
	{
		/*
		 * 尝试一次拿到整条消息。若成功，
		 * 就不必拷贝数据，可以直接返回指向
		 * 共享内存的指针。
		 */
		res = shm_mq_receive_bytes(mqh, nbytes, nowait, &rb, &rawdata);
		if (res != SHM_MQ_SUCCESS)
			return res;
		if (rb >= nbytes)
		{
			mqh->mqh_length_word_complete = false;
			mqh->mqh_consume_pending += MAXALIGN(nbytes);
			*nbytesp = nbytes;
			*datap = rawdata;
			return SHM_MQ_SUCCESS;
		}

		/*
		 * 消息已经绕过缓冲区末端。为了把它作为一整块
		 * 返回给客户端，需要拷贝。首先，确保有
		 * 一块足够大的缓冲区可用。
		 */
		if (mqh->mqh_buflen < nbytes)
		{
			Size		newbuflen;

			/*
			 * 把大小增到 >= nbytes 的下一个 2 的幂，但
			 * 限制在 MaxAllocSize 以内。
			 */
			newbuflen = pg_nextpower2_size_t(nbytes);
			newbuflen = Min(newbuflen, MaxAllocSize);

			if (mqh->mqh_buffer != NULL)
			{
				pfree(mqh->mqh_buffer);
				mqh->mqh_buffer = NULL;
				mqh->mqh_buflen = 0;
			}
			mqh->mqh_buffer = MemoryContextAlloc(mqh->mqh_context, newbuflen);
			mqh->mqh_buflen = newbuflen;
		}
	}

	/* 循环，直到拷贝完整条消息。 */
	for (;;)
	{
		Size		still_needed;

		/* 能拷贝多少就拷贝多少。 */
		Assert(mqh->mqh_partial_bytes + rb <= nbytes);
		if (rb > 0)
		{
			memcpy(&mqh->mqh_buffer[mqh->mqh_partial_bytes], rawdata, rb);
			mqh->mqh_partial_bytes += rb;
		}

		/*
		 * 更新可以消费的字节计数，把对齐
		 * 填充计算在内。注意，除了在消息末尾，这里实际上
		 * 永远不会插入填充，因为缓冲区大小是
		 * MAXIMUM_ALIGNOF 的倍数，每次读写也是如此。
		 */
		Assert(mqh->mqh_partial_bytes == nbytes || rb == MAXALIGN(rb));
		mqh->mqh_consume_pending += MAXALIGN(rb);

		/* 若已经拿到全部数据，就退出循环。 */
		if (mqh->mqh_partial_bytes >= nbytes)
			break;

		/* 再等一些数据。 */
		still_needed = nbytes - mqh->mqh_partial_bytes;
		res = shm_mq_receive_bytes(mqh, still_needed, nowait, &rb, &rawdata);
		if (res != SHM_MQ_SUCCESS)
			return res;
		if (rb > still_needed)
			rb = still_needed;
	}

	/* 返回完整消息，并为下一条消息重置。 */
	*nbytesp = nbytes;
	*datap = mqh->mqh_buffer;
	mqh->mqh_length_word_complete = false;
	mqh->mqh_partial_bytes = 0;
	return SHM_MQ_SUCCESS;
}

/*
 * 等待应当使用本队列的另一个进程挂接
 * 上来。
 *
 * 若工作进程已经脱离或已经死亡，返回值是 SHM_MQ_DETACHED；
 * 若检测到工作进程已经挂接，返回值是 SHM_MQ_SUCCESS。
 * 注意，只有把后台工作进程句柄传给 shm_mq_attach() 时，
 * 我们才能检测到工作进程在挂接之前死亡。
 */
shm_mq_result
shm_mq_wait_for_attach(shm_mq_handle *mqh)
{
	shm_mq	   *mq = mqh->mqh_queue;
	PGPROC	  **victim;

	if (shm_mq_get_receiver(mq) == MyProc)
		victim = &mq->mq_sender;
	else
	{
		Assert(shm_mq_get_sender(mq) == MyProc);
		victim = &mq->mq_receiver;
	}

	if (shm_mq_wait_internal(mq, victim, mqh->mqh_handle))
		return SHM_MQ_SUCCESS;
	else
		return SHM_MQ_DETACHED;
}

/*
 * 脱离共享消息队列，并销毁 shm_mq_handle。
 */
void
shm_mq_detach(shm_mq_handle *mqh)
{
	/* 脱离之前，把已经写过的数据通知接收方。 */
	if (mqh->mqh_send_pending > 0)
	{
		shm_mq_inc_bytes_written(mqh->mqh_queue, mqh->mqh_send_pending);
		mqh->mqh_send_pending = 0;
	}

	/* 通知对端我们这就走了。 */
	shm_mq_detach_internal(mqh->mqh_queue);

	/* 若有 on_dsm_detach 回调，就取消它。 */
	if (mqh->mqh_segment)
		cancel_on_dsm_detach(mqh->mqh_segment,
							 shm_mq_detach_callback,
							 PointerGetDatum(mqh->mqh_queue));

	/* 释放与句柄关联的本地内存。 */
	if (mqh->mqh_buffer != NULL)
		pfree(mqh->mqh_buffer);
	pfree(mqh);
}

/*
 * 通知对端我们正在脱离共享消息队列。
 *
 * 本函数的目的是确保与我们通信的进程
 * 不会在我们已经失去兴趣之后，还永远阻塞着等待我们
 * 去填满或排空队列。发送方
 * 脱离后，接收方仍可以读队列里剩余的消息；
 * 再读就会返回 SHM_MQ_DETACHED。若接收方脱离，
 * 以后再尝试发送消息同样会返回 SHM_MQ_DETACHED。
 *
 * 把它从 shm_mq_detach() 里拆出来，是因为若 on_dsm_detach
 * 回调触发，我们只想做到这一步。我们不去碰
 * 本地的 shm_mq_handle，因为它可能已经被 pfree 掉了。
 */
static void
shm_mq_detach_internal(shm_mq *mq)
{
	PGPROC	   *victim;

	SpinLockAcquire(&mq->mq_mutex);
	if (mq->mq_sender == MyProc)
		victim = mq->mq_receiver;
	else
	{
		Assert(mq->mq_receiver == MyProc);
		victim = mq->mq_sender;
	}
	mq->mq_detached = true;
	SpinLockRelease(&mq->mq_mutex);

	if (victim != NULL)
		SetLatch(&victim->procLatch);
}

/*
 * 从句柄取得 shm_mq。
 */
shm_mq *
shm_mq_get_queue(shm_mq_handle *mqh)
{
	return mqh->mqh_queue;
}

/*
 * 把字节写入共享消息队列。
 */
static shm_mq_result
shm_mq_send_bytes(shm_mq_handle *mqh, Size nbytes, const void *data,
				  bool nowait, Size *bytes_written)
{
	shm_mq	   *mq = mqh->mqh_queue;
	Size		sent = 0;
	uint64		used;
	Size		ringsize = mq->mq_ring_size;
	Size		available;

	while (sent < nbytes)
	{
		uint64		rb;
		uint64		wb;

		/* 计算环形缓冲区已用和可用的字节数。 */
		rb = pg_atomic_read_u64(&mq->mq_bytes_read);
		wb = pg_atomic_read_u64(&mq->mq_bytes_written) + mqh->mqh_send_pending;
		Assert(wb >= rb);
		used = wb - rb;
		Assert(used <= ringsize);
		available = Min(ringsize - used, nbytes - sent);

		/*
		 * 若队列已脱离就退出。注意，若编译器决定把
		 * mq->mq_detached 的值缓存起来，
		 * 放进寄存器或栈上并跨过循环
		 * 各次迭代，我们就会有麻烦。它大概本来也不该这么做，因为我们
		 * 总会返回、调用一个执行系统
		 * 调用的外部函数，或在循环稍后某处遇到内存屏障，
		 * 但为了保险，这里插入一道编译器屏障。
		 */
		pg_compiler_barrier();
		if (mq->mq_detached)
		{
			*bytes_written = sent;
			return SHM_MQ_DETACHED;
		}

		if (available == 0 && !mqh->mqh_counterparty_attached)
		{
			/*
			 * 队列已满，因此若还不知道接收方已经
			 * 挂接，就必须等那件事发生。
			 */
			if (nowait)
			{
				if (shm_mq_counterparty_gone(mq, mqh->mqh_handle))
				{
					*bytes_written = sent;
					return SHM_MQ_DETACHED;
				}
				if (shm_mq_get_receiver(mq) == NULL)
				{
					*bytes_written = sent;
					return SHM_MQ_WOULD_BLOCK;
				}
			}
			else if (!shm_mq_wait_internal(mq, &mq->mq_receiver,
										   mqh->mqh_handle))
			{
				mq->mq_detached = true;
				*bytes_written = sent;
				return SHM_MQ_DETACHED;
			}
			mqh->mqh_counterparty_attached = true;

			/*
			 * 接收方挂接之后可能已经读过一些数据，因此
			 * 不重新检查队列状态就不得等待。
			 */
		}
		else if (available == 0)
		{
			/* 把待发送字节更新到共享内存中。 */
			shm_mq_inc_bytes_written(mq, mqh->mqh_send_pending);

			/*
			 * 既然此时已知 mq->mqh_counterparty_attached 为真，
			 * mq_receiver 已经被设置，而且一旦设置就不会再变。
			 * 因此可以不加自旋锁地读取它。
			 */
			Assert(mqh->mqh_counterparty_attached);
			SetLatch(&mq->mq_receiver->procLatch);

			/*
			 * 刚刚已经把 mqh_send_pending 字节更新到共享
			 * 内存里，所以把它清零。
			 */
			mqh->mqh_send_pending = 0;

			/* 若 nowait = true，就跳过对我们锁存器的操作。 */
			if (nowait)
			{
				*bytes_written = sent;
				return SHM_MQ_WOULD_BLOCK;
			}

			/*
			 * 等待我们的锁存器被设置。它可能已经因某个
			 * 无关的原因被设置了，那不过是多绕循环
			 * 一圈。值得这么做，是为了避免在循环顶部重置锁存器，
			 * 因为设置一个已经设置的锁存器，比设置一个
			 * 已被重置的锁存器便宜得多。
			 */
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
							 WAIT_EVENT_MQ_SEND);

			/* 重置锁存器，以免自旋。 */
			ResetLatch(MyLatch);

			/* 等待期间可能发生了中断。 */
			CHECK_FOR_INTERRUPTS();
		}
		else
		{
			Size		offset;
			Size		sendnow;

			offset = wb % (uint64) ringsize;
			sendnow = Min(available, ringsize - offset);

			/*
			 * 用一次 memcpy() 尽可能多地写出数据。确保
			 * 这些写发生在上面读取 mq_bytes_read 之后。
			 * 这道屏障与 shm_mq_inc_bytes_read 里的那道成对。
			 * （因为我们把读取 mq_bytes_read 与随后对 mq_ring 的写分开，
			 * 这里需要一道全屏障。）
			 */
			pg_memory_barrier();
			memcpy(&mq->mq_ring[mq->mq_ring_offset + offset],
				   (char *) data + sent, sendnow);
			sent += sendnow;

			/*
			 * 更新已写字节计数，带上对齐填充。注意
			 * 除了在一串字节的末尾，这里实际上永远不会插入填充，
			 * 因为缓冲区大小是 MAXIMUM_ALIGNOF 的倍数，
			 * 每次读取也是如此。
			 */
			Assert(sent == nbytes || sendnow == MAXALIGN(sendnow));

			/*
			 * 为了效率，这里不更新共享内存中的已写字节，
			 * 也不在这里设置读者的锁存器。更多
			 * 说明见 shm_mq_handle 结构顶部的
			 * 注释。
			 */
			mqh->mqh_send_pending += MAXALIGN(sendnow);
		}
	}

	*bytes_written = sent;
	return SHM_MQ_SUCCESS;
}

/*
 * 一直等到共享消息队列里至少有 *nbytesp 字节可供读取，
 * 或者等到缓冲区回绕。若队列已
 * 脱离，返回 SHM_MQ_DETACHED。若指定了 nowait 并且需要等待，
 * 则返回 SHM_MQ_WOULD_BLOCK。否则，把 *datap 设为
 * 可以读取数据字节的位置，把 *nbytesp 设为
 * 该地址处可以读取的字节数，返回值
 * 为 SHM_MQ_SUCCESS。
 */
static shm_mq_result
shm_mq_receive_bytes(shm_mq_handle *mqh, Size bytes_needed, bool nowait,
					 Size *nbytesp, void **datap)
{
	shm_mq	   *mq = mqh->mqh_queue;
	Size		ringsize = mq->mq_ring_size;
	uint64		used;
	uint64		written;

	for (;;)
	{
		Size		offset;
		uint64		read;

		/* 取得已写字节数，以便计算有多少可读。 */
		written = pg_atomic_read_u64(&mq->mq_bytes_written);

		/*
		 * 取得已读字节数。把我们可以消费但尚未
		 * 消费的字节也算进去。
		 */
		read = pg_atomic_read_u64(&mq->mq_bytes_read) +
			mqh->mqh_consume_pending;
		used = written - read;
		Assert(used <= ringsize);
		offset = read % (uint64) ringsize;

		/* 若数据已经够，或缓冲区已经回绕，就完成了。 */
		if (used >= bytes_needed || offset + used >= ringsize)
		{
			*nbytesp = Min(used, ringsize - offset);
			*datap = &mq->mq_ring[mq->mq_ring_offset + offset];

			/*
			 * 把上面读取 mq_bytes_written 与调用方
			 * 试图读取数据本身分开。与 shm_mq_inc_bytes_written
			 * 里的屏障成对。
			 */
			pg_read_barrier();
			return SHM_MQ_SUCCESS;
		}

		/*
		 * 若队列已脱离，就在等待之前先退出。
		 *
		 * 注意，我们直到 *考虑过* 已经可用的数据
		 * 是否足够之后才检查这一点，因为接收方即使在发送方已经
		 * 脱离之后，仍可以把缓冲区里存放的消息
		 * 接收完。
		 */
		if (mq->mq_detached)
		{
			/*
			 * 若写者先推进了 mq_bytes_written，然后才设置
			 * mq_detached，我们上面可能还没读到 mq_bytes_written 的
			 * 最终值。插入一道读屏障，然后再检查
			 * mq_bytes_written 是否又向前走了。
			 */
			pg_read_barrier();
			if (written != pg_atomic_read_u64(&mq->mq_bytes_written))
				continue;

			return SHM_MQ_DETACHED;
		}

		/*
		 * 数据不够满足请求，因此把先前已消费、
		 * 但尚未标为已读的数据标成已读，以便腾出更多缓冲区空间。
		 */
		if (mqh->mqh_consume_pending > 0)
		{
			shm_mq_inc_bytes_read(mq, mqh->mqh_consume_pending);
			mqh->mqh_consume_pending = 0;
		}

		/* 若 nowait = true，就跳过对我们锁存器的操作。 */
		if (nowait)
			return SHM_MQ_WOULD_BLOCK;

		/*
		 * 等待我们的锁存器被设置。它可能已经因某个
		 * 无关的原因被设置了，那不过是多绕循环
		 * 一圈。值得这么做，是为了避免在循环顶部重置锁存器，
		 * 因为设置一个已经设置的锁存器，比设置一个
		 * 已被重置的锁存器便宜得多。
		 */
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_MQ_RECEIVE);

		/* 重置锁存器，以免自旋。 */
		ResetLatch(MyLatch);

		/* 等待期间可能发生了中断。 */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * 测试一个也许甚至还没活着的对端是否已经确定离开。
 */
static bool
shm_mq_counterparty_gone(shm_mq *mq, BackgroundWorkerHandle *handle)
{
	pid_t		pid;

	/* 若队列已脱离，对端就肯定已经离开。 */
	if (mq->mq_detached)
		return true;

	/* 若有句柄，就检查工作进程状态。 */
	if (handle != NULL)
	{
		BgwHandleStatus status;

		/* 检查是否有意外的工作进程死亡。 */
		status = GetBackgroundWorkerPid(handle, &pid);
		if (status != BGWH_STARTED && status != BGWH_NOT_YET_STARTED)
		{
			/* 把它标为已脱离，好让这件事成为正式状态。 */
			mq->mq_detached = true;
			return true;
		}
	}

	/* 对端还没有确定离开。 */
	return false;
}

/*
 * 当进程正在等待对端挂接队列时使用本函数。
 * 当另一个进程按预期挂接时我们退出；或者，若
 * handle != NULL，当所引用的后台进程或 postmaster
 * 死亡时我们也退出。注意，若 handle == NULL，而该进程又没能挂接，
 * 我们可能会永远卡在这里，等待一个也许永远不会
 * 启动的进程。不过我们确实会检查中断。
 *
 * ptr 指向我们预期会变成
 * 非 NULL 的内存地址，那表示对端已经挂接队列。
 */
static bool
shm_mq_wait_internal(shm_mq *mq, PGPROC **ptr, BackgroundWorkerHandle *handle)
{
	bool		result = false;

	for (;;)
	{
		BgwHandleStatus status;
		pid_t		pid;

		/* 获锁的时间只够检查这个指针。 */
		SpinLockAcquire(&mq->mq_mutex);
		result = (*ptr != NULL);
		SpinLockRelease(&mq->mq_mutex);

		/* 若已脱离则失败；否则若已初始化则成功。 */
		if (mq->mq_detached)
		{
			result = false;
			break;
		}
		if (result)
			break;

		if (handle != NULL)
		{
			/* 检查是否有意外的工作进程死亡。 */
			status = GetBackgroundWorkerPid(handle, &pid);
			if (status != BGWH_STARTED && status != BGWH_NOT_YET_STARTED)
			{
				result = false;
				break;
			}
		}

		/* 等待被信号唤醒。 */
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_MQ_INTERNAL);

		/* 重置锁存器，以免自旋。 */
		ResetLatch(MyLatch);

		/* 等待期间可能发生了中断。 */
		CHECK_FOR_INTERRUPTS();
	}

	return result;
}

/*
 * 增加已读字节数。
 */
static void
shm_mq_inc_bytes_read(shm_mq *mq, Size n)
{
	PGPROC	   *sender;

	/*
	 * 把此前对 mq_ring 的读取与随后对 mq_bytes_read 的
	 * 增加分开。这与 shm_mq_send_bytes() 里的全屏障成对。
	 * 这里只需要读屏障，因为 mq_bytes_read 的增加实际上是一次读，
	 * 后面跟着一次依赖于它的
	 * 写。
	 */
	pg_read_barrier();

	/*
	 * 这里不必使用 pg_atomic_fetch_add_u64，因为没有别人
	 * 会在改这个值。这个办法应当更便宜。
	 */
	pg_atomic_write_u64(&mq->mq_bytes_read,
						pg_atomic_read_u64(&mq->mq_bytes_read) + n);

	/*
	 * 没有发送方就不应当有任何可读字节，因此这里可以不加锁地
	 * 读取 mq_sender。一旦它被初始化，就不会再变。
	 */
	sender = mq->mq_sender;
	Assert(sender != NULL);
	SetLatch(&sender->procLatch);
}

/*
 * 增加已写字节数。
 */
static void
shm_mq_inc_bytes_written(shm_mq *mq, Size n)
{
	/*
	 * 把此前对 mq_ring 的读取与即将进行的 mq_bytes_written
	 * 写入分开。与 shm_mq_receive_bytes 里的
	 * 读屏障成对。
	 */
	pg_write_barrier();

	/*
	 * 这里不必使用 pg_atomic_fetch_add_u64，因为没有别人
	 * 会在改这个值。这个办法避免不必要地
	 * 获取总线锁。
	 */
	pg_atomic_write_u64(&mq->mq_bytes_written,
						pg_atomic_read_u64(&mq->mq_bytes_written) + n);
}

/* on_dsm_detach 回调的转接。 */
static void
shm_mq_detach_callback(dsm_segment *seg, Datum arg)
{
	shm_mq	   *mq = (shm_mq *) DatumGetPointer(arg);

	shm_mq_detach_internal(mq);
}
