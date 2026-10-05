/*-------------------------------------------------------------------------
 *
 * localbuf.c
 *	  local buffer manager. Fast buffer manager for temporary tables,
 *	  which never need to be WAL-logged or checkpointed, etc.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/localbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "catalog/catalog.h"
#include "executor/instrument.h"
#include "pgstat.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"


/*调试开关 LBDEBUG*/

/* 缓冲区查找散列表的条目 */
typedef struct
{
	BufferTag	key;			/* 磁盘页的标签 */
	int			id;				/* 关联的本地缓冲区下标 */
} LocalBufferLookupEnt;

/* 注意：这个宏只对本地缓冲区有效，不适用于共享缓冲区！ */
#define LocalBufHdrGetBlock(bufHdr) \
	LocalBufferBlockPointers[-((bufHdr)->buf_id + 2)]

int			NLocBuffer = 0;		/* 直到缓冲区被初始化 */

BufferDesc *LocalBufferDescriptors = NULL;
Block	   *LocalBufferBlockPointers = NULL;
int32	   *LocalRefCount = NULL;

static int	nextFreeLocalBufId = 0;

static HTAB *LocalBufHash = NULL;

/* 至少被钉住过一次的本地缓冲区个数 */
static int	NLocalPinnedBuffers = 0;


static void InitLocalBuffers(void);
static Block GetLocalBufferStorage(void);
static Buffer GetLocalVictimBuffer(void);


/*
 * PrefetchLocalBuffer -
 *	  发起对关系中一个块的异步读取。
 *
 * 做 PrefetchBuffer 对临时关系的工作。
 * 若未编译预读，则为空操作。
 */
PrefetchBufferResult
PrefetchLocalBuffer(SMgrRelation smgr, ForkNumber forkNum,
					BlockNumber blockNum)
{
	PrefetchBufferResult result = {InvalidBuffer, false};
	BufferTag	newTag;			/* 所请求块的标识 */
	LocalBufferLookupEnt *hresult;

	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* 若本会话首次请求，则初始化本地缓冲区 */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	/* 查看所需缓冲区是否已存在 */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		/* 是的，因此无事可做 */
		result.recent_buffer = -hresult->id - 1;
	}
	else
	{
#ifdef USE_PREFETCH
		/* 不在缓冲区里，因此发起预读 */
		if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
			smgrprefetch(smgr, forkNum, blockNum))
		{
			result.initiated_io = true;
		}
#endif							/* 预读宏 USE_PREFETCH */
	}

	return result;
}


/*
 * LocalBufferAlloc -
 *	  为给定关系的给定页查找或创建本地缓冲区。
 *
 * API 与 bufmgr.c 的 BufferAlloc 类似，只是这里全是本地的，
 * 不需要加锁。另外，不会设置 IO_IN_PROGRESS。
 * 最后，我们只支持默认访问策略
 * （因此 usage_count 总是递增）。
 */
BufferDesc *
LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum, BlockNumber blockNum,
				 bool *foundPtr)
{
	BufferTag	newTag;			/* 所请求块的标识 */
	LocalBufferLookupEnt *hresult;
	BufferDesc *bufHdr;
	Buffer		victim_buffer;
	int			bufid;
	bool		found;

	InitBufferTag(&newTag, &smgr->smgr_rlocator.locator, forkNum, blockNum);

	/* 若本会话首次请求，则初始化本地缓冲区 */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	/* 查看所需缓冲区是否已存在 */
	hresult = (LocalBufferLookupEnt *)
		hash_search(LocalBufHash, &newTag, HASH_FIND, NULL);

	if (hresult)
	{
		bufid = hresult->id;
		bufHdr = GetLocalBufferDescriptor(bufid);
		Assert(BufferTagsEqual(&bufHdr->tag, &newTag));

		*foundPtr = PinLocalBuffer(bufHdr, true);
	}
	else
	{
		uint32		buf_state;

		victim_buffer = GetLocalVictimBuffer();
		bufid = -victim_buffer - 1;
		bufHdr = GetLocalBufferDescriptor(bufid);

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, &newTag, HASH_ENTER, &found);
		if (found)				/* 不应当发生 */
			elog(ERROR, "local buffer hash table corrupted");
		hresult->id = bufid;

		/*
		 * 现在完全归我们。
		 */
		bufHdr->tag = newTag;

		buf_state = pg_atomic_read_u32(&bufHdr->state);
		buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
		buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

		*foundPtr = false;
	}

	return bufHdr;
}

static Buffer
GetLocalVictimBuffer(void)
{
	int			victim_bufid;
	int			trycounter;
	uint32		buf_state;
	BufferDesc *bufHdr;

	ResourceOwnerEnlargeBuffers(CurrentResourceOwner);

	/*
	 * 需要取得一个新缓冲区。我们使用时钟清扫算法（基本上
	 * 与 freelist.c 现在的做法相同……）。
	 */
	trycounter = NLocBuffer;
	for (;;)
	{
		victim_bufid = nextFreeLocalBufId;

		if (++nextFreeLocalBufId >= NLocBuffer)
			nextFreeLocalBufId = 0;

		bufHdr = GetLocalBufferDescriptor(victim_bufid);

		if (LocalRefCount[victim_bufid] == 0)
		{
			buf_state = pg_atomic_read_u32(&bufHdr->state);

			if (BUF_STATE_GET_USAGECOUNT(buf_state) > 0)
			{
				buf_state -= BUF_USAGECOUNT_ONE;
				pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
				trycounter = NLocBuffer;
			}
			else
			{
				/* 找到一个可用缓冲区 */
				PinLocalBuffer(bufHdr, false);
				break;
			}
		}
		else if (--trycounter == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no empty local buffer available")));
	}

	/*
	 * 惰性内存分配：第一次使用某个缓冲区时再分配空间。
	 */
	if (LocalBufHdrGetBlock(bufHdr) == NULL)
	{
		/* 设置指针，供 BufferGetBlock() 宏使用 */
		LocalBufHdrGetBlock(bufHdr) = GetLocalBufferStorage();
	}

	/*
	 * 这个缓冲区没有被引用，但仍可能是脏页。若是这样，
	 * 在重用之前先把它写出去！
	 */
	if (buf_state & BM_DIRTY)
	{
		instr_time	io_start;
		SMgrRelation oreln;
		Page		localpage = (char *) LocalBufHdrGetBlock(bufHdr);

		/* 查找该缓冲区的 smgr 关系 */
		oreln = smgropen(BufTagGetRelFileLocator(&bufHdr->tag), MyBackendId);

		PageSetChecksumInplace(localpage, bufHdr->tag.blockNum);

		io_start = pgstat_prepare_io_time();

		/* 然后写入…… */
		smgrwrite(oreln,
				  BufTagGetForkNum(&bufHdr->tag),
				  bufHdr->tag.blockNum,
				  localpage,
				  false);

		/* 临时表 I/O 不使用缓冲区访问策略 */
		pgstat_count_io_op_time(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL,
								IOOP_WRITE, io_start, 1);

		/* 现在标成非脏页，以免下面出错 */
		buf_state &= ~BM_DIRTY;
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);

		pgBufferUsage.local_blks_written++;
	}

	/*
	 * 从散列表去掉牺牲缓冲区，并标为无效。
	 */
	if (buf_state & BM_TAG_VALID)
	{
		LocalBufferLookupEnt *hresult;

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, &bufHdr->tag, HASH_REMOVE, NULL);
		if (!hresult)			/* 不应当发生 */
			elog(ERROR, "local buffer hash table corrupted");
		/* 万一散列插入失败，先把缓冲区标为无效 */
		ClearBufferTag(&bufHdr->tag);
		buf_state &= ~(BUF_FLAG_MASK | BUF_USAGECOUNT_MASK);
		pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
		pgstat_count_io_op(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL, IOOP_EVICT);
	}

	return BufferDescriptorGetBuffer(bufHdr);
}

/* 见 LimitAdditionalPins() */
static void
LimitAdditionalLocalPins(uint32 *additional_pins)
{
	uint32		max_pins;

	if (*additional_pins <= 1)
		return;

	/*
	 * 与 LimitAdditionalPins() 不同，其他后端在这里不起作用。
	 * 我们总共可以允许最多 NLocBuffer 个钉住。
	 */
	max_pins = (NLocBuffer - NLocalPinnedBuffers);

	if (*additional_pins >= max_pins)
		*additional_pins = max_pins;
}

/*
 * ExtendBufferedRelBy() 与 ExtendBufferedRelTo() 对
 * 临时缓冲区的实现。
 */
BlockNumber
ExtendBufferedRelLocal(BufferManagerRelation bmr,
					   ForkNumber fork,
					   uint32 flags,
					   uint32 extend_by,
					   BlockNumber extend_upto,
					   Buffer *buffers,
					   uint32 *extended_by)
{
	BlockNumber first_block;
	instr_time	io_start;

	/* 若本会话首次请求，则初始化本地缓冲区 */
	if (LocalBufHash == NULL)
		InitLocalBuffers();

	LimitAdditionalLocalPins(&extend_by);

	for (uint32 i = 0; i < extend_by; i++)
	{
		BufferDesc *buf_hdr;
		Block		buf_block;

		buffers[i] = GetLocalVictimBuffer();
		buf_hdr = GetLocalBufferDescriptor(-buffers[i] - 1);
		buf_block = LocalBufHdrGetBlock(buf_hdr);

		/* 新缓冲区用零填充 */
		MemSet((char *) buf_block, 0, BLCKSZ);
	}

	first_block = smgrnblocks(bmr.smgr, fork);

	if (extend_upto != InvalidBlockNumber)
	{
		/*
		 * 与共享关系不同，没有什么能并发改变关系
		 * 的大小。因此我们不应当最终发现其实什么
		 * 也不用做。
		 */
		Assert(first_block <= extend_upto);

		Assert((uint64) first_block + extend_by <= extend_upto);
	}

	/* 若关系已达到最大可能长度则失败 */
	if ((uint64) first_block + extend_by >= MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend relation %s beyond %u blocks",
						relpath(bmr.smgr->smgr_rlocator, fork),
						MaxBlockNumber)));

	for (int i = 0; i < extend_by; i++)
	{
		int			victim_buf_id;
		BufferDesc *victim_buf_hdr;
		BufferTag	tag;
		LocalBufferLookupEnt *hresult;
		bool		found;

		victim_buf_id = -buffers[i] - 1;
		victim_buf_hdr = GetLocalBufferDescriptor(victim_buf_id);

		InitBufferTag(&tag, &bmr.smgr->smgr_rlocator.locator, fork, first_block + i);

		hresult = (LocalBufferLookupEnt *)
			hash_search(LocalBufHash, (void *) &tag, HASH_ENTER, &found);
		if (found)
		{
			BufferDesc *existing_hdr;
			uint32		buf_state;

			UnpinLocalBuffer(BufferDescriptorGetBuffer(victim_buf_hdr));

			existing_hdr = GetLocalBufferDescriptor(hresult->id);
			PinLocalBuffer(existing_hdr, false);
			buffers[i] = BufferDescriptorGetBuffer(existing_hdr);

			buf_state = pg_atomic_read_u32(&existing_hdr->state);
			Assert(buf_state & BM_TAG_VALID);
			Assert(!(buf_state & BM_DIRTY));
			buf_state &= ~BM_VALID;
			pg_atomic_unlocked_write_u32(&existing_hdr->state, buf_state);
		}
		else
		{
			uint32		buf_state = pg_atomic_read_u32(&victim_buf_hdr->state);

			Assert(!(buf_state & (BM_VALID | BM_TAG_VALID | BM_DIRTY | BM_JUST_DIRTIED)));

			victim_buf_hdr->tag = tag;

			buf_state |= BM_TAG_VALID | BUF_USAGECOUNT_ONE;

			pg_atomic_unlocked_write_u32(&victim_buf_hdr->state, buf_state);

			hresult->id = victim_buf_id;
		}
	}

	io_start = pgstat_prepare_io_time();

	/* 真正扩展该关系 */
	smgrzeroextend(bmr.smgr, fork, first_block, extend_by, false);

	pgstat_count_io_op_time(IOOBJECT_TEMP_RELATION, IOCONTEXT_NORMAL, IOOP_EXTEND,
							io_start, extend_by);

	for (int i = 0; i < extend_by; i++)
	{
		Buffer		buf = buffers[i];
		BufferDesc *buf_hdr;
		uint32		buf_state;

		buf_hdr = GetLocalBufferDescriptor(-buf - 1);

		buf_state = pg_atomic_read_u32(&buf_hdr->state);
		buf_state |= BM_VALID;
		pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);
	}

	*extended_by = extend_by;

	pgBufferUsage.local_blks_written += extend_by;

	return first_block;
}

/*
 * MarkLocalBufferDirty -
 *	  把一个本地缓冲区标为脏页。
 */
void
MarkLocalBufferDirty(Buffer buffer)
{
	int			bufid;
	BufferDesc *bufHdr;
	uint32		buf_state;

	Assert(BufferIsLocal(buffer));

#ifdef LBDEBUG
	fprintf(stderr, "LB DIRTY %d\n", buffer);
#endif

	bufid = -buffer - 1;

	Assert(LocalRefCount[bufid] > 0);

	bufHdr = GetLocalBufferDescriptor(bufid);

	buf_state = pg_atomic_read_u32(&bufHdr->state);

	if (!(buf_state & BM_DIRTY))
		pgBufferUsage.local_blks_dirtied++;

	buf_state |= BM_DIRTY;

	pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
}

/*
 * DropRelationLocalBuffers
 *		此函数从缓冲池去掉指定关系中
 *		块号 >= firstDelBlock 的所有页。
 *		（特别地，firstDelBlock = 0 时去掉所有页。）
 *		这些缓冲区中的脏页直接丢掉，不先写出去。
 *		因此这不能回滚，所以
 *		只能极其谨慎地使用！
 *
 *		更多说明见 bufmgr.c 里的 DropRelationBuffers。
 */
void
DropRelationLocalBuffers(RelFileLocator rlocator, ForkNumber forkNum,
						 BlockNumber firstDelBlock)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = GetLocalBufferDescriptor(i);
		LocalBufferLookupEnt *hresult;
		uint32		buf_state;

		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if ((buf_state & BM_TAG_VALID) &&
			BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator) &&
			BufTagGetForkNum(&bufHdr->tag) == forkNum &&
			bufHdr->tag.blockNum >= firstDelBlock)
		{
			if (LocalRefCount[i] != 0)
				elog(ERROR, "block %u of %s is still referenced (local %u)",
					 bufHdr->tag.blockNum,
					 relpathbackend(BufTagGetRelFileLocator(&bufHdr->tag),
									MyBackendId,
									BufTagGetForkNum(&bufHdr->tag)),
					 LocalRefCount[i]);

			/* 从散列表去掉该条目 */
			hresult = (LocalBufferLookupEnt *)
				hash_search(LocalBufHash, &bufHdr->tag, HASH_REMOVE, NULL);
			if (!hresult)		/* 不应当发生 */
				elog(ERROR, "local buffer hash table corrupted");
			/* 把缓冲区标为无效 */
			ClearBufferTag(&bufHdr->tag);
			buf_state &= ~BUF_FLAG_MASK;
			buf_state &= ~BUF_USAGECOUNT_MASK;
			pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
		}
	}
}

/*
 * DropRelationAllLocalBuffers
 *		此函数从缓冲池去掉指定关系
 *		所有分叉的全部缓冲区页。
 *
 *		更多说明见 bufmgr.c 里的 DropRelationsAllBuffers。
 */
void
DropRelationAllLocalBuffers(RelFileLocator rlocator)
{
	int			i;

	for (i = 0; i < NLocBuffer; i++)
	{
		BufferDesc *bufHdr = GetLocalBufferDescriptor(i);
		LocalBufferLookupEnt *hresult;
		uint32		buf_state;

		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if ((buf_state & BM_TAG_VALID) &&
			BufTagMatchesRelFileLocator(&bufHdr->tag, &rlocator))
		{
			if (LocalRefCount[i] != 0)
				elog(ERROR, "block %u of %s is still referenced (local %u)",
					 bufHdr->tag.blockNum,
					 relpathbackend(BufTagGetRelFileLocator(&bufHdr->tag),
									MyBackendId,
									BufTagGetForkNum(&bufHdr->tag)),
					 LocalRefCount[i]);
			/* 从散列表去掉该条目 */
			hresult = (LocalBufferLookupEnt *)
				hash_search(LocalBufHash, &bufHdr->tag, HASH_REMOVE, NULL);
			if (!hresult)		/* 不应当发生 */
				elog(ERROR, "local buffer hash table corrupted");
			/* 把缓冲区标为无效 */
			ClearBufferTag(&bufHdr->tag);
			buf_state &= ~BUF_FLAG_MASK;
			buf_state &= ~BUF_USAGECOUNT_MASK;
			pg_atomic_unlocked_write_u32(&bufHdr->state, buf_state);
		}
	}
}

/*
 * InitLocalBuffers -
 *	  初始化本地缓冲区缓存。由于多数查询（尤其多用户查询）
 *	  不涉及本地缓冲区，我们推迟分配实际内存，
 *	  直到需要它们；这里只建立缓冲区头。
 */
static void
InitLocalBuffers(void)
{
	int			nbufs = num_temp_buffers;
	HASHCTL		info;
	int			i;

	/*
	 * 并行工作进程没法访问临时表里的数据，因为它们
	 * 看不到其领导进程的本地缓冲区。这里是提供
	 * 一道低开销后备检查的方便位置。注意，
	 * 我们并不希望阻止并行工作进程访问临时表的
	 * 系统目录元数据，因此在更高层做检查
	 * 并不合适。
	 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot access temporary tables during a parallel operation")));

	/* 分配并清零缓冲区头和辅助数组 */
	LocalBufferDescriptors = (BufferDesc *) calloc(nbufs, sizeof(BufferDesc));
	LocalBufferBlockPointers = (Block *) calloc(nbufs, sizeof(Block));
	LocalRefCount = (int32 *) calloc(nbufs, sizeof(int32));
	if (!LocalBufferDescriptors || !LocalBufferBlockPointers || !LocalRefCount)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	nextFreeLocalBufId = 0;

	/* 初始化需要从非零开始的字段 */
	for (i = 0; i < nbufs; i++)
	{
		BufferDesc *buf = GetLocalBufferDescriptor(i);

		/*
		 * 用负数表示本地缓冲区。这很微妙：共享缓冲区
		 * 从 0 开始。我们必须从 -2 开始。（注意例程
		 * BufferDescriptorGetBuffer 把 buf_id 加 1，因此第一个缓冲区 id
		 * 是 -1。）
		 */
		buf->buf_id = -i - 2;

		/*
		 * 故意不初始化该缓冲区的原子变量
		 * （上面已经把底层内存清零）。这样，在没有原子操作的平台上，
		 * 若有人（重新）为本地缓冲区引入原子操作，
		 * 就会出错。
		 */
	}

	/* 创建查找散列表 */
	info.keysize = sizeof(BufferTag);
	info.entrysize = sizeof(LocalBufferLookupEnt);

	LocalBufHash = hash_create("Local Buffer Lookup Table",
							   nbufs,
							   &info,
							   HASH_ELEM | HASH_BLOBS);

	if (!LocalBufHash)
		elog(ERROR, "could not initialize local buffer hash table");

	/* 初始化完成，把缓冲区标为已分配 */
	NLocBuffer = nbufs;
}

/*
 * XXX：我们可以有一个稍高效的 PinLocalBuffer() 版本，
 * 不支持调整 usagecount，但到目前为止
 * 似乎不值得这么做。
 */
bool
PinLocalBuffer(BufferDesc *buf_hdr, bool adjust_usagecount)
{
	uint32		buf_state;
	Buffer		buffer = BufferDescriptorGetBuffer(buf_hdr);
	int			bufid = -buffer - 1;

	buf_state = pg_atomic_read_u32(&buf_hdr->state);

	if (LocalRefCount[bufid] == 0)
	{
		NLocalPinnedBuffers++;
		if (adjust_usagecount &&
			BUF_STATE_GET_USAGECOUNT(buf_state) < BM_MAX_USAGE_COUNT)
		{
			buf_state += BUF_USAGECOUNT_ONE;
			pg_atomic_unlocked_write_u32(&buf_hdr->state, buf_state);
		}
	}
	LocalRefCount[bufid]++;
	ResourceOwnerRememberBuffer(CurrentResourceOwner,
								BufferDescriptorGetBuffer(buf_hdr));

	return buf_state & BM_VALID;
}

void
UnpinLocalBuffer(Buffer buffer)
{
	int			buffid = -buffer - 1;

	Assert(BufferIsLocal(buffer));
	Assert(LocalRefCount[buffid] > 0);
	Assert(NLocalPinnedBuffers > 0);

	ResourceOwnerForgetBuffer(CurrentResourceOwner, buffer);
	if (--LocalRefCount[buffid] == 0)
		NLocalPinnedBuffers--;
}

/*
 * temp_buffers 的 GUC check_hook
 */
bool
check_temp_buffers(int *newval, void **extra, GucSource source)
{
	/*
	 * 一旦本地缓冲区已经初始化，再改这个就太迟了。
	 * 不过，若这只是一次测试调用，则允许。
	 */
	if (source != PGC_S_TEST && NLocBuffer && NLocBuffer != *newval)
	{
		GUC_check_errdetail("\"temp_buffers\" cannot be changed after any temporary tables have been accessed in the session.");
		return false;
	}
	return true;
}

/*
 * GetLocalBufferStorage - 为一个本地缓冲区分配内存。
 *
 * 这个函数的想法是把存储请求聚合起来，
 * 使内存管理器看不到一大堆相对较小的
 * 请求。某个本地缓冲区一旦在特定进程里创建，
 * 我们就再也不会交还，因此没有必要
 * 用分开管理的块去加重 memmgr 的负担。
 */
static Block
GetLocalBufferStorage(void)
{
	static char *cur_block = NULL;
	static int	next_buf_in_block = 0;
	static int	num_bufs_in_block = 0;
	static int	total_bufs_allocated = 0;
	static MemoryContext LocalBufferContext = NULL;

	char	   *this_buf;

	Assert(total_bufs_allocated < NLocBuffer);

	if (next_buf_in_block >= num_bufs_in_block)
	{
		/* 需要向 memmgr 提出新请求 */
		int			num_bufs;

		/*
		 * 我们在它们自己的上下文里分配本地缓冲区，以便
		 * 它们占用的空间在 MemoryContextStats
		 * 输出里容易辨认。第一次使用时创建该上下文。
		 */
		if (LocalBufferContext == NULL)
			LocalBufferContext =
				AllocSetContextCreate(TopMemoryContext,
									  "LocalBufferContext",
									  ALLOCSET_DEFAULT_SIZES);

		/* 先请求 16 个缓冲区；其后每次加倍 */
		num_bufs = Max(num_bufs_in_block * 2, 16);
		/* 但不超过剩余本地缓冲区所需的量 */
		num_bufs = Min(num_bufs, NLocBuffer - total_bufs_allocated);
		/* 也不要溢出 MaxAllocSize */
		num_bufs = Min(num_bufs, MaxAllocSize / BLCKSZ);

		/* 缓冲区应当按 I/O 对齐。 */
		cur_block = (char *)
			TYPEALIGN(PG_IO_ALIGN_SIZE,
					  MemoryContextAlloc(LocalBufferContext,
										 num_bufs * BLCKSZ + PG_IO_ALIGN_SIZE));
		next_buf_in_block = 0;
		num_bufs_in_block = num_bufs;
	}

	/* 在当前内存块里分配下一个缓冲区 */
	this_buf = cur_block + next_buf_in_block * BLCKSZ;
	next_buf_in_block++;
	total_bufs_allocated++;

	return (Block) this_buf;
}

/*
 * CheckForLocalBufferLeaks - 确保此后端不持有本地缓冲区钉住。
 *
 * 这与 CheckForBufferLeaks() 类似，但是针对本地缓冲区。
 */
static void
CheckForLocalBufferLeaks(void)
{
#ifdef USE_ASSERT_CHECKING
	if (LocalRefCount)
	{
		int			RefCountErrors = 0;
		int			i;

		for (i = 0; i < NLocBuffer; i++)
		{
			if (LocalRefCount[i] != 0)
			{
				Buffer		b = -i - 1;

				PrintBufferLeakWarning(b);
				RefCountErrors++;
			}
		}
		Assert(RefCountErrors == 0);
	}
#endif
}

/*
 * AtEOXact_LocalBuffers - 在事务结束时清理。
 *
 * 这与 AtEOXact_Buffers 类似，但是针对本地缓冲区。
 */
void
AtEOXact_LocalBuffers(bool isCommit)
{
	CheckForLocalBufferLeaks();
}

/*
 * AtProcExit_LocalBuffers - 确保在后端退出期间已经解除钉住。
 *
 * 这与 AtProcExit_Buffers 类似，但是针对本地缓冲区。
 */
void
AtProcExit_LocalBuffers(void)
{
	/*
	 * 我们不应当还持有任何剩余的钉住；若仍持有，且未启用断言，
	 * 稍后在 DropRelationBuffers 里试图丢掉
	 * 临时关系时会失败。
	 */
	CheckForLocalBufferLeaks();
}
