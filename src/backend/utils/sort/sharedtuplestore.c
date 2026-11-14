/*-------------------------------------------------------------------------
 *
 * sharedtuplestore.c
 *	  后端之间共享元组的简单机制。
 *
 * 本模块包含一种共享临时元组存储机制，提供
 * tuplestore.c 各功能中可感知并行的子集。多个后端
 * 可以写入 SharedTuplestore，随后多个后端可以扫描
 * 已存储的元组。当前支持的扫描类型只有并行扫描，
 * 即每个后端读取所写入元组的任意子集。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/sharedtuplestore.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/lwlock.h"
#include "storage/sharedfileset.h"
#include "utils/sharedtuplestore.h"

/*
 * 分块的大小，以页为单位。此值设定得有些随意，以
 * 匹配 HASH_CHUNK 的大小，使并行哈希获取新元组分块
 * 的速率与其分配新内存分块的速率大致相同
 * 以便插入。
 */
#define STS_CHUNK_PAGES 4
#define STS_CHUNK_HEADER_SIZE offsetof(SharedTuplestoreChunk, data)
#define STS_CHUNK_DATA_SIZE (STS_CHUNK_PAGES * BLCKSZ - STS_CHUNK_HEADER_SIZE)

/* 已落盘分块。 */
typedef struct SharedTuplestoreChunk
{
	int			ntuples;		/* 本分块中的元组数。 */
	int			overflow;		/* 若溢出，含此共多少？ */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} SharedTuplestoreChunk;

/* 参与者共享状态。 */
typedef struct SharedTuplestoreParticipant
{
	LWLock		lock;
	BlockNumber read_page;		/* 下次读取页号。 */
	BlockNumber npages;			/* 已写入页数。 */
	bool		writing;		/* 仅用于断言。 */
} SharedTuplestoreParticipant;

/* 位于共享内存中的控制对象。 */
struct SharedTuplestore
{
	int			nparticipants;	/* 可写入的参与者数量。 */
	int			flags;			/* SHARED_TUPLESTORE_XXX 的标志位 */
	size_t		meta_data_size; /* 元组头部大小。 */
	char		name[NAMEDATALEN];	/* 此元组存储名。 */

	/* 后面为各参与者共享状态。 */
	SharedTuplestoreParticipant participants[FLEXIBLE_ARRAY_MEMBER];
};

/* 位于后端本地内存中的各参与者状态。 */
struct SharedTuplestoreAccessor
{
	int			participant;	/* 参与者编号。 */
	SharedTuplestore *sts;		/* 共享状态。 */
	SharedFileSet *fileset;		/* 持有文件的 SharedFileSet */
	MemoryContext context;		/* 缓冲内存上下文 */

	/* 读取状态。 */
	int			read_participant;	/* 当前读取来源的参与者。 */
	BufFile    *read_file;		/* 当前读取的文件。 */
	int			read_ntuples_available; /* 分块中的元组数。 */
	int			read_ntuples;	/* 本分块已读取了多少元组？ */
	size_t		read_bytes;		/* 本分块已读取多少字节？ */
	char	   *read_buffer;	/* 装载元组缓冲区。 */
	size_t		read_buffer_size;
	BlockNumber read_next_page; /* 考虑读取的最低块。 */

	/* 写入状态。 */
	SharedTuplestoreChunk *write_chunk; /* 写入缓冲区。 */
	BufFile    *write_file;		/* 当前写入的文件。 */
	BlockNumber write_page;		/* 下个写入页。 */
	char	   *write_pointer;	/* 分块内的当前写指针。 */
	char	   *write_end;		/* 当前分块末尾后一位。 */
};

static void sts_filename(char *name, SharedTuplestoreAccessor *accessor,
						 int participant);

/*
 * 返回容纳 SharedTuplestore 所需的共享内存大小，
 * 对应给定参与者数。
 */
size_t
sts_estimate(int participants)
{
	return offsetof(SharedTuplestore, participants) +
		sizeof(SharedTuplestoreParticipant) * participants;
}

/*
 * 在现有共享内存中初始化 SharedTuplestore。必须
 * 有 sts_estimate(participants) 字节的空间。若 flags 含
 * SHARED_TUPLESTORE_SINGLE_PASS，则这些文件将来可能
 * 更早被移除（尚未实现）。
 *
 * 被存储的元组可选择携带一段定长的
 * 元数据，它会随元组一起取回。这对多批次哈希
 * 连接所用的哈希值很有用，但也可能有其他用途。
 *
 *
 * 调用者必须提供一个 SharedFileSet，它实质上是一个
 * 会被自动清理的目录，以及一个在同一 SharedFileSet
 * 中创建的所有 SharedTuplestores 之间唯一的名称。
 */
SharedTuplestoreAccessor *
sts_initialize(SharedTuplestore *sts, int participants,
			   int my_participant_number,
			   size_t meta_data_size,
			   int flags,
			   SharedFileSet *fileset,
			   const char *name)
{
	SharedTuplestoreAccessor *accessor;
	int			i;

	Assert(my_participant_number < participants);

	sts->nparticipants = participants;
	sts->meta_data_size = meta_data_size;
	sts->flags = flags;

	if (strlen(name) > sizeof(sts->name) - 1)
		elog(ERROR, "SharedTuplestore name too long");
	strcpy(sts->name, name);

	/*
	 * 限制元数据，使其加元组大小总能装入单个分块。
	 * sts_puttuple() 与 sts_read_tuple() 可以被改造为支持
	 * 不满足该条件的场景，但目前无此需求。若是，
	 * 元数据大小或许也应改为可变。
	 */
	if (meta_data_size + sizeof(uint32) >= STS_CHUNK_DATA_SIZE)
		elog(ERROR, "meta-data too long");

	for (i = 0; i < participants; ++i)
	{
		LWLockInitialize(&sts->participants[i].lock,
						 LWTRANCHE_SHARED_TUPLESTORE);
		sts->participants[i].read_page = 0;
		sts->participants[i].npages = 0;
		sts->participants[i].writing = false;
	}

	accessor = palloc0(sizeof(SharedTuplestoreAccessor));
	accessor->participant = my_participant_number;
	accessor->sts = sts;
	accessor->fileset = fileset;
	accessor->context = CurrentMemoryContext;

	return accessor;
}

/*
 * 附加到已由另一后端初始化的 SharedTuplestore，
 * 以便本后端可以读取和写入元组。
 */
SharedTuplestoreAccessor *
sts_attach(SharedTuplestore *sts,
		   int my_participant_number,
		   SharedFileSet *fileset)
{
	SharedTuplestoreAccessor *accessor;

	Assert(my_participant_number < sts->nparticipants);

	accessor = palloc0(sizeof(SharedTuplestoreAccessor));
	accessor->participant = my_participant_number;
	accessor->sts = sts;
	accessor->fileset = fileset;
	accessor->context = CurrentMemoryContext;

	return accessor;
}

static void
sts_flush_chunk(SharedTuplestoreAccessor *accessor)
{
	size_t		size;

	size = STS_CHUNK_PAGES * BLCKSZ;
	BufFileWrite(accessor->write_file, accessor->write_chunk, size);
	memset(accessor->write_chunk, 0, size);
	accessor->write_pointer = &accessor->write_chunk->data[0];
	accessor->sts->participants[accessor->participant].npages +=
		STS_CHUNK_PAGES;
}

/*
 * 完成元组写入。所有已写入数据的后端都必须
 * 在任何后端开始读取前调用它。
 */
void
sts_end_write(SharedTuplestoreAccessor *accessor)
{
	if (accessor->write_file != NULL)
	{
		sts_flush_chunk(accessor);
		BufFileClose(accessor->write_file);
		pfree(accessor->write_chunk);
		accessor->write_chunk = NULL;
		accessor->write_file = NULL;
		accessor->sts->participants[accessor->participant].writing = false;
	}
}

/*
 * 准备重新扫描。必须只有一个参与者调用本函数。
 * 它返回后，所有参与者都可调用 sts_begin_parallel_scan()
 * 然后循环调用 sts_parallel_scan_next()。本函数不得
 * 与扫描并发调用，而避免并发的同步由调用者
 * 负责。
 */
void
sts_reinitialize(SharedTuplestoreAccessor *accessor)
{
	int			i;

	/*
	 * 重置所有参与者文件的共享读头。同时将初始分
	 * 块大小设为最小值（任何相对该尺寸的增长都会
	 * 记录到 chunk_expansion_log）
	 */
	for (i = 0; i < accessor->sts->nparticipants; ++i)
	{
		accessor->sts->participants[i].read_page = 0;
	}
}

/*
 * 开始并行扫描其中内容。
 */
void
sts_begin_parallel_scan(SharedTuplestoreAccessor *accessor)
{
	int			i PG_USED_FOR_ASSERTS_ONLY;

	/* 结束任何正在进行中的扫描。 */
	sts_end_parallel_scan(accessor);

	/*
	 * 任何可能已写入此共享元组存储的后端都必须
	 * 已调用过 sts_end_write()，从而刷写所有缓冲区，
	 * 且文件停止增长。
	 */
	for (i = 0; i < accessor->sts->nparticipants; ++i)
		Assert(!accessor->sts->participants[i].writing);

	/*
	 * 我们将从本后端自己写入的文件开始读取。这可能
	 * 存在一些缓存局部性优势。
	 */
	accessor->read_participant = accessor->participant;
	accessor->read_file = NULL;
	accessor->read_next_page = 0;
}

/*
 * 完成一次并行扫描，释放相关的后端本地资源。
 */
void
sts_end_parallel_scan(SharedTuplestoreAccessor *accessor)
{
	/*
	 * 若为 SHARED_TUPLESTORE_SINGLE_PASS，本可删除全部文件，
	 * 但那样很可能需要当前并行扫描者的引用计数，
	 * 以便只在计数归零时才安全地执行。
	 */
	if (accessor->read_file != NULL)
	{
		BufFileClose(accessor->read_file);
		accessor->read_file = NULL;
	}
}

/*
 * 写入一个元组。若 sts_initialize 被提供了元数据大小，
 * 则必须提供指向该大小元数据的指针。
 */
void
sts_puttuple(SharedTuplestoreAccessor *accessor, void *meta_data,
			 MinimalTuple tuple)
{
	size_t		size;

	/* 有自己的文件了吗？ */
	if (accessor->write_file == NULL)
	{
		SharedTuplestoreParticipant *participant;
		char		name[MAXPGPATH];
		MemoryContext oldcxt;

		/* 创建一个。仅本后端会写入其中。 */
		sts_filename(name, accessor, accessor->participant);

		oldcxt = MemoryContextSwitchTo(accessor->context);
		accessor->write_file =
			BufFileCreateFileSet(&accessor->fileset->fs, name);
		MemoryContextSwitchTo(oldcxt);

		/* 为本后端的文件设置共享状态。 */
		participant = &accessor->sts->participants[accessor->participant];
		participant->writing = true;	/* 仅用于断言 */
	}

	/* 有空间吗？ */
	size = accessor->sts->meta_data_size + tuple->t_len;
	if (accessor->write_pointer + size > accessor->write_end)
	{
		if (accessor->write_chunk == NULL)
		{
			/* 首次执行。分配分块。 */
			accessor->write_chunk = (SharedTuplestoreChunk *)
				MemoryContextAllocZero(accessor->context,
									   STS_CHUNK_PAGES * BLCKSZ);
			accessor->write_chunk->ntuples = 0;
			accessor->write_pointer = &accessor->write_chunk->data[0];
			accessor->write_end = (char *)
				accessor->write_chunk + STS_CHUNK_PAGES * BLCKSZ;
		}
		else
		{
			/* 刷写能否奏效 */
			sts_flush_chunk(accessor);
		}

		/* 在元组巨大时这可能仍不够。 */
		if (accessor->write_pointer + size > accessor->write_end)
		{
			size_t		written;

			/*
			 * 我们将写入超长元组的开头，然后以
			 * 若干'溢出'分块写入其余部分。
			 *
			 * sts_initialize() 会验证元组的大小加
			 * 元数据总能装入一个分块。由于该分块已被
			 * 刷写，我们可以确保拥有分块的全部可用空间
			 * 可供使用。
			 */
			Assert(accessor->write_pointer + accessor->sts->meta_data_size +
				   sizeof(uint32) < accessor->write_end);

			/* 将元数据整块写入。 */
			if (accessor->sts->meta_data_size > 0)
				memcpy(accessor->write_pointer, meta_data,
					   accessor->sts->meta_data_size);

			/*
			 * 写入尽可能多的元组内容。这包括开头的
			 * 元组大小。
			 */
			written = accessor->write_end - accessor->write_pointer -
				accessor->sts->meta_data_size;
			memcpy(accessor->write_pointer + accessor->sts->meta_data_size,
				   tuple, written);
			++accessor->write_chunk->ntuples;
			size -= accessor->sts->meta_data_size;
			size -= written;
			/* 接着写入余下所需的溢出分块。 */
			while (size > 0)
			{
				size_t		written_this_chunk;

				sts_flush_chunk(accessor);

				/*
				 * 还剩多少溢出分块？这能让读取者一次
				 * 跳过所有这些分块，而无需逐个读取。
				 */
				accessor->write_chunk->overflow = (size + STS_CHUNK_DATA_SIZE - 1) /
					STS_CHUNK_DATA_SIZE;
				written_this_chunk =
					Min(accessor->write_end - accessor->write_pointer, size);
				memcpy(accessor->write_pointer, (char *) tuple + written,
					   written_this_chunk);
				accessor->write_pointer += written_this_chunk;
				size -= written_this_chunk;
				written += written_this_chunk;
			}
			return;
		}
	}

	/* 元数据和元组拷入缓冲区。 */
	if (accessor->sts->meta_data_size > 0)
		memcpy(accessor->write_pointer, meta_data,
			   accessor->sts->meta_data_size);
	memcpy(accessor->write_pointer + accessor->sts->meta_data_size, tuple,
		   tuple->t_len);
	accessor->write_pointer += size;
	++accessor->write_chunk->ntuples;
}

static MinimalTuple
sts_read_tuple(SharedTuplestoreAccessor *accessor, void *meta_data)
{
	MinimalTuple tuple;
	uint32		size;
	size_t		remaining_size;
	size_t		this_chunk_size;
	char	   *destination;

	/*
	 * 我们会跟踪本分块中已读取的字节数，以便检测
	 * 溢出的元组并切换到读取溢出页。
	 */
	if (accessor->sts->meta_data_size > 0)
	{
		BufFileReadExact(accessor->read_file, meta_data, accessor->sts->meta_data_size);
		accessor->read_bytes += accessor->sts->meta_data_size;
	}
	BufFileReadExact(accessor->read_file, &size, sizeof(size));
	accessor->read_bytes += sizeof(size);
	if (size > accessor->read_buffer_size)
	{
		size_t		new_read_buffer_size;

		if (accessor->read_buffer != NULL)
			pfree(accessor->read_buffer);
		new_read_buffer_size = Max(size, accessor->read_buffer_size * 2);
		accessor->read_buffer =
			MemoryContextAlloc(accessor->context, new_read_buffer_size);
		accessor->read_buffer_size = new_read_buffer_size;
	}
	remaining_size = size - sizeof(uint32);
	this_chunk_size = Min(remaining_size,
						  BLCKSZ * STS_CHUNK_PAGES - accessor->read_bytes);
	destination = accessor->read_buffer + sizeof(uint32);
	BufFileReadExact(accessor->read_file, destination, this_chunk_size);
	accessor->read_bytes += this_chunk_size;
	remaining_size -= this_chunk_size;
	destination += this_chunk_size;
	++accessor->read_ntuples;

	/* 检查是否需要读取溢出分块。 */
	while (remaining_size > 0)
	{
		/* 现在位于某个溢出分块的起始处。 */
		SharedTuplestoreChunk chunk_header;

		BufFileReadExact(accessor->read_file, &chunk_header, STS_CHUNK_HEADER_SIZE);
		accessor->read_bytes = STS_CHUNK_HEADER_SIZE;
		if (chunk_header.overflow == 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("unexpected chunk in shared tuplestore temporary file"),
					 errdetail_internal("Expected overflow chunk.")));
		accessor->read_next_page += STS_CHUNK_PAGES;
		this_chunk_size = Min(remaining_size,
							  BLCKSZ * STS_CHUNK_PAGES -
							  STS_CHUNK_HEADER_SIZE);
		BufFileReadExact(accessor->read_file, destination, this_chunk_size);
		accessor->read_bytes += this_chunk_size;
		remaining_size -= this_chunk_size;
		destination += this_chunk_size;

		/*
		 * 这些将用于统计跟在溢出流入该分块的
		 * 超长元组之后的常规元组。
		 */
		accessor->read_ntuples = 0;
		accessor->read_ntuples_available = chunk_header.ntuples;
	}

	tuple = (MinimalTuple) accessor->read_buffer;
	tuple->t_len = size;

	return tuple;
}

/*
 * 在当前并行扫描中获取下一个元组。
 */
MinimalTuple
sts_parallel_scan_next(SharedTuplestoreAccessor *accessor, void *meta_data)
{
	SharedTuplestoreParticipant *p;
	BlockNumber read_page;
	bool		eof;

	for (;;)
	{
		/* 能否从当前分块读取更多元组？ */
		if (accessor->read_ntuples < accessor->read_ntuples_available)
			return sts_read_tuple(accessor, meta_data);

		/* 寻找要读的新分块位置。 */
		p = &accessor->sts->participants[accessor->read_participant];

		LWLockAcquire(&p->lock, LW_EXCLUSIVE);
		/* 我们可以直接跳过已知的溢出页。 */
		if (p->read_page < accessor->read_next_page)
			p->read_page = accessor->read_next_page;
		eof = p->read_page >= p->npages;
		if (!eof)
		{
			/* 认领下一块 */
			read_page = p->read_page;
			/* 为下一个读取者推进读头。 */
			p->read_page += STS_CHUNK_PAGES;
			accessor->read_next_page = p->read_page;
		}
		LWLockRelease(&p->lock);

		if (!eof)
		{
			SharedTuplestoreChunk chunk_header;

			/* 确保文件已被打开。 */
			if (accessor->read_file == NULL)
			{
				char		name[MAXPGPATH];
				MemoryContext oldcxt;

				sts_filename(name, accessor, accessor->read_participant);

				oldcxt = MemoryContextSwitchTo(accessor->context);
				accessor->read_file =
					BufFileOpenFileSet(&accessor->fileset->fs, name, O_RDONLY,
									   false);
				MemoryContextSwitchTo(oldcxt);
			}

			/* 寻道并加载块头。 */
			if (BufFileSeekBlock(accessor->read_file, read_page) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not seek to block %u in shared tuplestore temporary file",
								read_page)));
			BufFileReadExact(accessor->read_file, &chunk_header, STS_CHUNK_HEADER_SIZE);

			/*
			 * 若这是溢出分块，则连同后续所有溢出分块
			 * 一起跳过。
			 */
			if (chunk_header.overflow > 0)
			{
				accessor->read_next_page = read_page +
					chunk_header.overflow * STS_CHUNK_PAGES;
				continue;
			}

			accessor->read_ntuples = 0;
			accessor->read_ntuples_available = chunk_header.ntuples;
			accessor->read_bytes = STS_CHUNK_HEADER_SIZE;

			/* 再循环一轮，以便从该分块获取元组。 */
		}
		else
		{
			if (accessor->read_file != NULL)
			{
				BufFileClose(accessor->read_file);
				accessor->read_file = NULL;
			}

			/*
			 * 试下一参与者的文件。若已绕完一圈，即
			 * 完成。
			 */
			accessor->read_participant = (accessor->read_participant + 1) %
				accessor->sts->nparticipants;
			if (accessor->read_participant == accessor->participant)
				break;
			accessor->read_next_page = 0;

			/* 再循环一轮，以便从该文件获取分块。 */
		}
	}

	return NULL;
}

/*
 * 创建给定参与者要写入的 BufFile 所使用的名称。
 */
static void
sts_filename(char *name, SharedTuplestoreAccessor *accessor, int participant)
{
	snprintf(name, MAXPGPATH, "%s.p%d", accessor->sts->name, participant);
}
