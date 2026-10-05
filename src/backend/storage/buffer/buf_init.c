/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
ConditionVariableMinimallyPadded *BufferIOCVArray;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;


/*
 * 数据结构：
 *		缓冲区位于空闲链表和查找数据结构中。
 *
 *
 * 缓冲区查找：
 *		有两点重要说明。第一，缓冲区必须在
 *		IO 开始之前就可以被查找。否则
 *		第二个试图读取该缓冲区的进程会
 *		分配自己的副本，缓冲池就会
 *		变得不一致。
 *
 * 缓冲区替换：
 *		见 freelist.c。缓冲区在数据管理器使用期间或 IO 期间
 *		无法被替换。
 *
 *
 * 同步与加锁：
 *
 * IO_IN_PROGRESS 是缓冲区描述符里的一个标志。
 *		IO 开始时必须设置它，并在
 *		IO 结束时清除。它用来确保一个
 *		进程不会在另一个进程正在把它缺页调入时开始使用该缓冲区。
 *		见 WaitIO 及相关例程。
 *
 * refcount 计数表示对一个缓冲区持有钉住的进程数。
 *		缓冲区在 IO 期间以及 BufferAlloc() 之后立即被钉住。
 *		事务结束前必须释放钉住。为了效率，若单个后端多次钉住
 *		同一个缓冲区，共享的 refcount 不会增加。见 bufmgr.c 里的
 *		PrivateRefCount 基础设施。
 */


/*
 * 初始化共享缓冲池。
 *
 * 这在共享内存初始化期间调用一次（或者在
 * postmaster 里，或者在独立后端里）。
 */
void
InitBufferPool(void)
{
	bool		foundBufs,
				foundDescs,
				foundIOCV,
				foundBufCkpt;

	/* 把描述符对齐到缓存行边界。 */
	BufferDescriptors = (BufferDescPadded *)
		ShmemInitStruct("Buffer Descriptors",
						NBuffers * sizeof(BufferDescPadded),
						&foundDescs);

	/* 把缓冲池对齐到 IO 页大小边界。 */
	BufferBlocks = (char *)
		TYPEALIGN(PG_IO_ALIGN_SIZE,
				  ShmemInitStruct("Buffer Blocks",
								  NBuffers * (Size) BLCKSZ + PG_IO_ALIGN_SIZE,
								  &foundBufs));

	/* 把条件变量对齐到缓存行边界。 */
	BufferIOCVArray = (ConditionVariableMinimallyPadded *)
		ShmemInitStruct("Buffer IO Condition Variables",
						NBuffers * sizeof(ConditionVariableMinimallyPadded),
						&foundIOCV);

	/*
	 * 用来排序待检查点缓冲区标识的数组位于
	 * 共享内存中，以免在运行时分配大量
	 * 内存。因为那会发生在检查点进行中，或
	 * 检查点进程重启时，内存分配失败会很
	 * 痛苦。
	 */
	CkptBufferIds = (CkptSortItem *)
		ShmemInitStruct("Checkpoint BufferIds",
						NBuffers * sizeof(CkptSortItem), &foundBufCkpt);

	if (foundDescs || foundBufs || foundIOCV || foundBufCkpt)
	{
		/* 应当全部找到，或者一个都找不到 */
		Assert(foundDescs && foundBufs && foundIOCV && foundBufCkpt);
		/* 注意：这条路径只在 EXEC_BACKEND 情形下才会走到 */
	}
	else
	{
		int			i;

		/*
		 * 初始化全部缓冲区头部。
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf = GetBufferDescriptor(i);

			ClearBufferTag(&buf->tag);

			pg_atomic_init_u32(&buf->state, 0);
			buf->wait_backend_pgprocno = INVALID_PGPROCNO;

			buf->buf_id = i;

			/*
			 * 起初把所有缓冲区链接成未使用的。此后
			 * 这个链表的管理由 freelist.c 完成。
			 */
			buf->freeNext = i + 1;

			LWLockInitialize(BufferDescriptorGetContentLock(buf),
							 LWTRANCHE_BUFFER_CONTENT);

			ConditionVariableInit(BufferDescriptorGetIOCV(buf));
		}

		/* 修正链表的最后一项 */
		GetBufferDescriptor(NBuffers - 1)->freeNext = FREENEXT_END_OF_LIST;
	}

	/* 初始化其他共享缓冲区管理设施 */
	StrategyInitialize(!foundDescs);

	/* 初始化每个后端的文件刷写上下文 */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

/*
 * BufferShmemSize
 *
 * 计算缓冲池的共享内存大小，包括
 * 数据页、缓冲区描述符、散列表等。
 */
Size
BufferShmemSize(void)
{
	Size		size = 0;

	/* 缓冲区描述符的大小 */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferDescPadded)));
	/* 以便对齐缓冲区描述符 */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* 数据页的大小，加上对齐填充 */
	size = add_size(size, PG_IO_ALIGN_SIZE);
	size = add_size(size, mul_size(NBuffers, BLCKSZ));

	/* freelist.c 所控制内容的大小 */
	size = add_size(size, StrategyShmemSize());

	/* I/O 条件变量的大小 */
	size = add_size(size, mul_size(NBuffers,
								   sizeof(ConditionVariableMinimallyPadded)));
	/* 以便对齐上面这些内容 */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* bufmgr.c 里检查点排序数组的大小 */
	size = add_size(size, mul_size(NBuffers, sizeof(CkptSortItem)));

	return size;
}
