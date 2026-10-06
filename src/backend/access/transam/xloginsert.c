/*-------------------------------------------------------------------------
 *
 * xloginsert.c
 *		构造 WAL 记录的函数
 *
 * 构造一条 WAL 记录从调用 XLogBeginInsert 开始，
 * 随后是若干次 XLogRegister* 调用。注册的数据收集在
 * 私有工作内存中，最后由 XLogRecordAssemble() 组装成
 * XLogRecData 结构链。细节见
 * access/transam/README。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xloginsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "catalog/pg_control.h"
#include "common/pg_lzcompress.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "replication/origin.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "utils/memutils.h"

/*
 * 估算存储备份块映像压缩版本所需的最大缓冲区大小。
 *
 */
#ifdef USE_LZ4
#define	LZ4_MAX_BLCKSZ		LZ4_COMPRESSBOUND(BLCKSZ)
#else
#define LZ4_MAX_BLCKSZ		0
#endif

#ifdef USE_ZSTD
#define ZSTD_MAX_BLCKSZ		ZSTD_COMPRESSBOUND(BLCKSZ)
#else
#define ZSTD_MAX_BLCKSZ		0
#endif

#define PGLZ_MAX_BLCKSZ		PGLZ_MAX_OUTPUT(BLCKSZ)

/* 存储备份块映像压缩版本所需的缓冲区大小 */
#define COMPRESS_BUFSIZE	Max(Max(PGLZ_MAX_BLCKSZ, LZ4_MAX_BLCKSZ), ZSTD_MAX_BLCKSZ)

/*
 * 对每个用 XLogRegisterBuffer 注册的块引用，我们填写一个
 * registered_buffer 结构。
 */
typedef struct
{
	bool		in_use;			/* 此槽是否在使用中？ */
	uint8		flags;			/* REGBUF_* 标志 */
	RelFileLocator rlocator;	/* 标识关系和块 */
	ForkNumber	forkno;
	BlockNumber block;
	Page		page;			/* 页内容 */
	uint32		rdata_len;		/* rdata 链中数据的总长度 */
	XLogRecData *rdata_head;	/* 与此块一起注册的数据链的
								 * 头 */
	XLogRecData *rdata_tail;	/* 链中的最后一项，若为空则为
								 * &rdata_head */

	XLogRecData bkp_rdatas[2];	/* 临时 rdata，用于在 XLogRecordAssemble() 中
								 * 保存对备份块数据的引用 */

	/* 存储备份块映像压缩版本的缓冲区 */
	char		compressed_page[COMPRESS_BUFSIZE];
} registered_buffer;

static registered_buffer *registered_buffers;
static int	max_registered_buffers; /* 已分配大小 */
static int	max_registered_block_id = 0;	/* 当前已注册的最高
											 * block_id + 1 */

/*
 * 一条 XLogRecDatas 链，保存用 XLogRegisterData(...) 注册的
 * WAL 记录"主数据"。
 */
static XLogRecData *mainrdata_head;
static XLogRecData *mainrdata_last = (XLogRecData *) &mainrdata_head;
static uint64 mainrdata_len;	/* 链中的总字节数 */

/* 进行中插入的标志 */
static uint8 curinsert_flags = 0;

/*
 * 构造记录时用这些存放记录头。
 * 'hdr_scratch' 不是普通变量，而是在初始化时 palloc 的，
 * 因为我们希望它是 MAXALIGNed 的且填充字节为零。
 *
 * 为简单起见，分配得足够大，可容纳任意
 * WAL 记录的头部。
 */
static XLogRecData hdr_rdt;
static char *hdr_scratch = NULL;

#define SizeOfXlogOrigin	(sizeof(RepOriginId) + sizeof(char))
#define SizeOfXLogTransactionId	(sizeof(TransactionId) + sizeof(char))

#define HEADER_SCRATCH_SIZE \
	(SizeOfXLogRecord + \
	 MaxSizeOfXLogRecordBlockHeader * (XLR_MAX_BLOCK_ID + 1) + \
	 SizeOfXLogRecordDataHeaderLong + SizeOfXlogOrigin + \
	 SizeOfXLogTransactionId)

/*
 * XLogRecData 结构数组，用于保存已注册的数据。
 */
static XLogRecData *rdatas;
static int	num_rdatas;			/* 当前已用条目数 */
static int	max_rdatas;			/* 已分配大小 */

static bool begininsert_called = false;

/* 保存已注册缓冲区与数据引用的内存上下文。 */
static MemoryContext xloginsert_cxt;

static XLogRecData *XLogRecordAssemble(RmgrId rmid, uint8 info,
									   XLogRecPtr RedoRecPtr, bool doPageWrites,
									   XLogRecPtr *fpw_lsn, int *num_fpi,
									   bool *topxid_included);
static bool XLogCompressBackupBlock(char *page, uint16 hole_offset,
									uint16 hole_length, char *dest, uint16 *dlen);

/*
 * 开始构造一条 WAL 记录。必须在 XLogRegister*
 * 函数和 XLogInsert() 之前调用。
 */
void
XLogBeginInsert(void)
{
	Assert(max_registered_block_id == 0);
	Assert(mainrdata_last == (XLogRecData *) &mainrdata_head);
	Assert(mainrdata_len == 0);

	/* 交叉检查我们是否本应到达此处 */
	if (!XLogInsertAllowed())
		elog(ERROR, "cannot make new WAL entries during recovery");

	if (begininsert_called)
		elog(ERROR, "XLogBeginInsert was already called");

	begininsert_called = true;
}

/*
 * 确保工作区有足够的缓冲区与数据槽，供后续的
 * XLogRegisterBuffer、XLogRegisterData 和 XLogRegisterBufData
 * 调用使用。
 *
 * 总是为少量缓冲区与数据块预留空间，对大多数
 * 记录类型已足够。本函数用于需要更多空间的
 * 例外情况。
 */
void
XLogEnsureRecordSpace(int max_block_id, int ndatas)
{
	int			nbuffers;

	/*
	 * 必须在进入临界区之前调用，因为在临界区内
	 * 分配内存可能失败。repalloc() 也会检查同样的
	 * 条件，但最好在这里也检查，以便即使数组碰巧已经
	 * 足够大，也能一致地失败。
	 */
	Assert(CritSectionCount == 0);

	/* 最小值不能被减小 */
	if (max_block_id < XLR_NORMAL_MAX_BLOCK_ID)
		max_block_id = XLR_NORMAL_MAX_BLOCK_ID;
	if (ndatas < XLR_NORMAL_RDATAS)
		ndatas = XLR_NORMAL_RDATAS;

	if (max_block_id > XLR_MAX_BLOCK_ID)
		elog(ERROR, "maximum number of WAL record block references exceeded");
	nbuffers = max_block_id + 1;

	if (nbuffers > max_registered_buffers)
	{
		registered_buffers = (registered_buffer *)
			repalloc(registered_buffers, sizeof(registered_buffer) * nbuffers);

		/*
		 * 至少结构中的填充字节必须清零，因为它们会
		 * 包含在 WAL 数据中，但为整洁起见全部初始化。
		 */
		MemSet(&registered_buffers[max_registered_buffers], 0,
			   (nbuffers - max_registered_buffers) * sizeof(registered_buffer));
		max_registered_buffers = nbuffers;
	}

	if (ndatas > max_rdatas)
	{
		rdatas = (XLogRecData *) repalloc(rdatas, sizeof(XLogRecData) * ndatas);
		max_rdatas = ndatas;
	}
}

/*
 * 重置 WAL 记录构造缓冲区。
 */
void
XLogResetInsertion(void)
{
	int			i;

	for (i = 0; i < max_registered_block_id; i++)
		registered_buffers[i].in_use = false;

	num_rdatas = 0;
	max_registered_block_id = 0;
	mainrdata_len = 0;
	mainrdata_last = (XLogRecData *) &mainrdata_head;
	curinsert_flags = 0;
	begininsert_called = false;
}

/*
 * 向正在构造的 WAL 记录注册一个缓冲区引用。
 * 凡被 WAL 记录的操作所修改的每一页都必须调用。
 */
void
XLogRegisterBuffer(uint8 block_id, Buffer buffer, uint8 flags)
{
	registered_buffer *regbuf;

	/* NO_IMAGE 与 FORCE_IMAGE 同时使用无意义 */
	Assert(!((flags & REGBUF_FORCE_IMAGE) && (flags & (REGBUF_NO_IMAGE))));
	Assert(begininsert_called);

	if (block_id >= max_registered_block_id)
	{
		if (block_id >= max_registered_buffers)
			elog(ERROR, "too many registered buffers");
		max_registered_block_id = block_id + 1;
	}

	regbuf = &registered_buffers[block_id];

	BufferGetTag(buffer, &regbuf->rlocator, &regbuf->forkno, &regbuf->block);
	regbuf->page = BufferGetPage(buffer);
	regbuf->flags = flags;
	regbuf->rdata_tail = (XLogRecData *) &regbuf->rdata_head;
	regbuf->rdata_len = 0;

	/*
	 * 检查此页是否尚未以其他
	 * block_id 注册。
	 */
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		for (i = 0; i < max_registered_block_id; i++)
		{
			registered_buffer *regbuf_old = &registered_buffers[i];

			if (i == block_id || !regbuf_old->in_use)
				continue;

			Assert(!RelFileLocatorEquals(regbuf_old->rlocator, regbuf->rlocator) ||
				   regbuf_old->forkno != regbuf->forkno ||
				   regbuf_old->block != regbuf->block);
		}
	}
#endif

	regbuf->in_use = true;
}

/*
 * 类似 XLogRegisterBuffer，但用于注册不在共享缓冲池中的块
 * （即没有对应 Buffer 时）。
 */
void
XLogRegisterBlock(uint8 block_id, RelFileLocator *rlocator, ForkNumber forknum,
				  BlockNumber blknum, Page page, uint8 flags)
{
	registered_buffer *regbuf;

	Assert(begininsert_called);

	if (block_id >= max_registered_block_id)
		max_registered_block_id = block_id + 1;

	if (block_id >= max_registered_buffers)
		elog(ERROR, "too many registered buffers");

	regbuf = &registered_buffers[block_id];

	regbuf->rlocator = *rlocator;
	regbuf->forkno = forknum;
	regbuf->block = blknum;
	regbuf->page = page;
	regbuf->flags = flags;
	regbuf->rdata_tail = (XLogRecData *) &regbuf->rdata_head;
	regbuf->rdata_len = 0;

	/*
	 * 检查此页是否尚未以其他
	 * block_id 注册。
	 */
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		for (i = 0; i < max_registered_block_id; i++)
		{
			registered_buffer *regbuf_old = &registered_buffers[i];

			if (i == block_id || !regbuf_old->in_use)
				continue;

			Assert(!RelFileLocatorEquals(regbuf_old->rlocator, regbuf->rlocator) ||
				   regbuf_old->forkno != regbuf->forkno ||
				   regbuf_old->block != regbuf->block);
		}
	}
#endif

	regbuf->in_use = true;
}

/*
 * 向正在构造的 WAL 记录添加数据。
 *
 * 数据追加到"主数据块"中，重放时可通过
 * XLogRecGetData() 取得。
 */
void
XLogRegisterData(char *data, uint32 len)
{
	XLogRecData *rdata;

	Assert(begininsert_called);

	if (num_rdatas >= max_rdatas)
		ereport(ERROR,
				(errmsg_internal("too much WAL data"),
				 errdetail_internal("%d out of %d data segments are already in use.",
									num_rdatas, max_rdatas)));
	rdata = &rdatas[num_rdatas++];

	rdata->data = data;
	rdata->len = len;

	/*
	 * 我们用 mainrdata_last 指针跟踪链尾，因此这里
	 * 无需清除 'next'。
	 */

	mainrdata_last->next = rdata;
	mainrdata_last = rdata;

	mainrdata_len += len;
}

/*
 * 向正在构造的 WAL 记录添加特定于缓冲区的数据。
 *
 * Block_id 必须引用先前用
 * XLogRegisterBuffer() 注册的块。若对同一
 * block_id 多次调用，则数据追加。
 *
 * 每个块最多可注册 65535
 * 字节数据。这应足够；若重建页上的更改需要超过 BLCKSZ
 * 字节，不如直接记录一整份
 * 副本。（与块无关的"主数据"不受
 * 此限）
 */
void
XLogRegisterBufData(uint8 block_id, char *data, uint32 len)
{
	registered_buffer *regbuf;
	XLogRecData *rdata;

	Assert(begininsert_called);

	/* 查找已注册的缓冲区结构 */
	regbuf = &registered_buffers[block_id];
	if (!regbuf->in_use)
		elog(ERROR, "no block with id %d registered with WAL insertion",
			 block_id);

	/*
	 * 对照 max_rdatas 检查，并确保每个缓冲区注册的数据
	 * 不超过物理数据格式所能承载的量；即
	 * regbuf->rdata_len 不得增长到超出
	 * XLogRecordBlockHeader->data_length 所能表示的范围。
	 */
	if (num_rdatas >= max_rdatas)
		ereport(ERROR,
				(errmsg_internal("too much WAL data"),
				 errdetail_internal("%d out of %d data segments are already in use.",
									num_rdatas, max_rdatas)));
	if (regbuf->rdata_len + len > UINT16_MAX || len > UINT16_MAX)
		ereport(ERROR,
				(errmsg_internal("too much WAL data"),
				 errdetail_internal("Registering more than maximum %u bytes allowed to block %u: current %u bytes, adding %u bytes.",
									UINT16_MAX, block_id, regbuf->rdata_len, len)));

	rdata = &rdatas[num_rdatas++];

	rdata->data = data;
	rdata->len = len;

	regbuf->rdata_tail->next = rdata;
	regbuf->rdata_tail = rdata;
	regbuf->rdata_len += len;
}

/*
 * 为即将写入的 WAL 记录设置插入状态标志。
 *
 * 此处可用的标志有：
 * - XLOG_INCLUDE_ORIGIN：决定是否应在记录中
 *	 包含复制源。
 * - XLOG_MARK_UNIMPORTANT：表示该记录对持久性
 *	 不重要，从而可避免触发 WAL 归档及其他
 *	 后台活动。
 */
void
XLogSetRecordFlags(uint8 flags)
{
	Assert(begininsert_called);
	curinsert_flags |= flags;
}

/*
 * 插入一条具有指定 RMID 与 info 字节的 XLOG 记录，记录体为
 * 先前用 XLogRegister* 调用注册的数据与缓冲区
 * 引用。
 *
 * 返回指向记录末尾（下一条记录起始）的 XLOG 指针。
 * 可用作受此次记入日志操作影响的数据页的 LSN。
 * （LSN 是在写出数据页之前必须把 XLOG 刷写到磁盘
 * 的 XLOG 位置。这实现了基本的
 * WAL 规则"先写日志再写数据"。）
 */
XLogRecPtr
XLogInsert(RmgrId rmid, uint8 info)
{
	XLogRecPtr	EndPos;

	/* 必须已调用 XLogBeginInsert()。 */
	if (!begininsert_called)
		elog(ERROR, "XLogBeginInsert was not called");

	/*
	 * 调用方可设置 rmgr 位、XLR_SPECIAL_REL_UPDATE 和
	 * XLR_CHECK_CONSISTENCY；其余保留给我使用。
	 */
	if ((info & ~(XLR_RMGR_INFO_MASK |
				  XLR_SPECIAL_REL_UPDATE |
				  XLR_CHECK_CONSISTENCY)) != 0)
		elog(PANIC, "invalid xlog info mask %02X", info);

	TRACE_POSTGRESQL_WAL_INSERT(rmid, info);

	/*
	 * 在 bootstrap 模式下，除 XLOG 资源外我们实际上不写日志；
	 * 返回一个伪记录指针。
	 */
	if (IsBootstrapProcessingMode() && rmid != RM_XLOG_ID)
	{
		XLogResetInsertion();
		EndPos = SizeOfXLogLongPHD; /* 第 1 条检查点记录的起始 */
		return EndPos;
	}

	do
	{
		XLogRecPtr	RedoRecPtr;
		bool		doPageWrites;
		bool		topxid_included = false;
		XLogRecPtr	fpw_lsn;
		XLogRecData *rdt;
		int			num_fpi = 0;

		/*
		 * 取得决定是否做整页写所需的值。由于我们
		 * 尚无插入锁，这些值可能在我们脚下改变，
		 * 但 XLogInsertRecord 拿到锁后会重新检查。
		 */
		GetFullPageWriteInfo(&RedoRecPtr, &doPageWrites);

		rdt = XLogRecordAssemble(rmid, info, RedoRecPtr, doPageWrites,
								 &fpw_lsn, &num_fpi, &topxid_included);

		EndPos = XLogInsertRecord(rdt, fpw_lsn, curinsert_flags, num_fpi,
								  topxid_included);
	} while (EndPos == InvalidXLogRecPtr);

	XLogResetInsertion();

	return EndPos;
}

/*
 * 把已注册的数据与缓冲区组装成一条 WAL 记录，形成
 * XLogRecData 链，准备用 XLogInsertRecord() 插入。
 *
 * 记录头字段已填好，但 xl_prev 除外。计算出的
 * CRC 尚未包含记录头。
 *
 * 若有已注册的缓冲区，且并非对它们全都拍摄了整页映像，
 * 则 *fpw_lsn 设为这些页中最低的 LSN。这表示
 * 组装好的记录仅在假定 RedoRecPtr 与 doPageWrites
 * 值是最新的情况下才适合插入。
 *
 * 若随当前子事务记录了顶层事务 ID，则
 * *topxid_included 被置位。
 */
static XLogRecData *
XLogRecordAssemble(RmgrId rmid, uint8 info,
				   XLogRecPtr RedoRecPtr, bool doPageWrites,
				   XLogRecPtr *fpw_lsn, int *num_fpi, bool *topxid_included)
{
	XLogRecData *rdt;
	uint64		total_len = 0;
	int			block_id;
	pg_crc32c	rdata_crc;
	registered_buffer *prev_regbuf = NULL;
	XLogRecData *rdt_datas_last;
	XLogRecord *rechdr;
	char	   *scratch = hdr_scratch;

	/*
	 * 注意：同一条记录可多次调用本函数。
	 * 下面我们对 rdata 链所做的全部修改都必须能处理这一点。
	 */

	/* 记录以固定大小的头部开始 */
	rechdr = (XLogRecord *) scratch;
	scratch += SizeOfXLogRecord;

	hdr_rdt.next = NULL;
	rdt_datas_last = &hdr_rdt;
	hdr_rdt.data = hdr_scratch;

	/*
	 * 若用户要求对本记录做一致性检查则强制启用。
	 * 在本例程开头就做，以便为 XLogInsert() 的调用方
	 * 提供直接为某条记录传入 XLR_CHECK_CONSISTENCY
	 * 的可能。
	 */
	if (wal_consistency_checking[rmid])
		info |= XLR_CHECK_CONSISTENCY;

	/*
	 * 构造一条包含所有块引用数据部分的 rdata 链。
	 * 这包括整页映像的数据。同时把块引用的
	 * 头部追加到暂存缓冲区中。
	 */
	*fpw_lsn = InvalidXLogRecPtr;
	for (block_id = 0; block_id < max_registered_block_id; block_id++)
	{
		registered_buffer *regbuf = &registered_buffers[block_id];
		bool		needs_backup;
		bool		needs_data;
		XLogRecordBlockHeader bkpb;
		XLogRecordBlockImageHeader bimg;
		XLogRecordBlockCompressHeader cbimg = {0};
		bool		samerel;
		bool		is_compressed = false;
		bool		include_image;

		if (!regbuf->in_use)
			continue;

		/* 判定此块是否需要备份 */
		if (regbuf->flags & REGBUF_FORCE_IMAGE)
			needs_backup = true;
		else if (regbuf->flags & REGBUF_NO_IMAGE)
			needs_backup = false;
		else if (!doPageWrites)
			needs_backup = false;
		else
		{
			/*
			 * 我们假定可传给 XLogInsert 的*每一*页上，
			 * 页 LSN 都是最前面的数据，无论其是否采用
			 * 标准页布局。
			 */
			XLogRecPtr	page_lsn = PageGetLSN(regbuf->page);

			needs_backup = (page_lsn <= RedoRecPtr);
			if (!needs_backup)
			{
				if (*fpw_lsn == InvalidXLogRecPtr || page_lsn < *fpw_lsn)
					*fpw_lsn = page_lsn;
			}
		}

		/* 判定缓冲区数据是否需要包含 */
		if (regbuf->rdata_len == 0)
			needs_data = false;
		else if ((regbuf->flags & REGBUF_KEEP_DATA) != 0)
			needs_data = true;
		else
			needs_data = !needs_backup;

		bkpb.id = block_id;
		bkpb.fork_flags = regbuf->forkno;
		bkpb.data_length = 0;

		if ((regbuf->flags & REGBUF_WILL_INIT) == REGBUF_WILL_INIT)
			bkpb.fork_flags |= BKPBLOCK_WILL_INIT;

		/*
		 * 若 needs_backup 为真，或当前资源管理器启用了
		 * WAL 检查，则为当前块记录一次整页写。
		 */
		include_image = needs_backup || (info & XLR_CHECK_CONSISTENCY) != 0;

		if (include_image)
		{
			Page		page = regbuf->page;
			uint16		compressed_len = 0;

			/*
			 * 该页需要备份，因此计算其空洞长度
			 * 与偏移。
			 */
			if (regbuf->flags & REGBUF_STANDARD)
			{
				/* 假定可省略 pd_lower 与 pd_upper 之间的数据 */
				uint16		lower = ((PageHeader) page)->pd_lower;
				uint16		upper = ((PageHeader) page)->pd_upper;

				if (lower >= SizeOfPageHeaderData &&
					upper > lower &&
					upper <= BLCKSZ)
				{
					bimg.hole_offset = lower;
					cbimg.hole_length = upper - lower;
				}
				else
				{
					/* 没有可去除的"空洞" */
					bimg.hole_offset = 0;
					cbimg.hole_length = 0;
				}
			}
			else
			{
				/* 非标准页头，不要试图消除"空洞" */
				bimg.hole_offset = 0;
				cbimg.hole_length = 0;
			}

			/*
			 * 若启用了 wal_compression，尝试压缩块映像
			 */
			if (wal_compression != WAL_COMPRESSION_NONE)
			{
				is_compressed =
					XLogCompressBackupBlock(page, bimg.hole_offset,
											cbimg.hole_length,
											regbuf->compressed_page,
											&compressed_len);
			}

			/*
			 * 填写 XLogRecordBlockHeader 结构中的
			 * 其余字段
			 */
			bkpb.fork_flags |= BKPBLOCK_HAS_IMAGE;

			/* 报告为本 WAL 记录构造了一幅整页映像 */
			*num_fpi += 1;

			/*
			 * 为页内容构造 XLogRecData 项。
			 */
			rdt_datas_last->next = &regbuf->bkp_rdatas[0];
			rdt_datas_last = rdt_datas_last->next;

			bimg.bimg_info = (cbimg.hole_length == 0) ? 0 : BKPIMAGE_HAS_HOLE;

			/*
			 * 若为本 WAL 记录的资源管理器启用了 WAL 一致性检查，
			 * 则记录中会包含所修改块的整页映像。
			 * 重做时，仅当设置了 BKPIMAGE_APPLY 时才
			 * 重放该整页。
			 */
			if (needs_backup)
				bimg.bimg_info |= BKPIMAGE_APPLY;

			if (is_compressed)
			{
				/* 当前压缩结果存入 WAL 记录 */
				bimg.length = compressed_len;

				/* 设置此块所用的压缩方法 */
				switch ((WalCompression) wal_compression)
				{
					case WAL_COMPRESSION_PGLZ:
						bimg.bimg_info |= BKPIMAGE_COMPRESS_PGLZ;
						break;

					case WAL_COMPRESSION_LZ4:
#ifdef USE_LZ4
						bimg.bimg_info |= BKPIMAGE_COMPRESS_LZ4;
#else
						elog(ERROR, "LZ4 is not supported by this build");
#endif
						break;

					case WAL_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
						bimg.bimg_info |= BKPIMAGE_COMPRESS_ZSTD;
#else
						elog(ERROR, "zstd is not supported by this build");
#endif
						break;

					case WAL_COMPRESSION_NONE:
						Assert(false);	/* 不可能发生 */
						break;
						/* 无 default 分支，以便编译器发出警告 */
				}

				rdt_datas_last->data = regbuf->compressed_page;
				rdt_datas_last->len = compressed_len;
			}
			else
			{
				bimg.length = BLCKSZ - cbimg.hole_length;

				if (cbimg.hole_length == 0)
				{
					rdt_datas_last->data = page;
					rdt_datas_last->len = BLCKSZ;
				}
				else
				{
					/* 必须跳过空洞 */
					rdt_datas_last->data = page;
					rdt_datas_last->len = bimg.hole_offset;

					rdt_datas_last->next = &regbuf->bkp_rdatas[1];
					rdt_datas_last = rdt_datas_last->next;

					rdt_datas_last->data =
						page + (bimg.hole_offset + cbimg.hole_length);
					rdt_datas_last->len =
						BLCKSZ - (bimg.hole_offset + cbimg.hole_length);
				}
			}

			total_len += bimg.length;
		}

		if (needs_data)
		{
			/*
			 * 复制到 XLogRecordBlockHeader 时，长度被收窄为
			 * uint16。再次确认它仍然正确。
			 */
			Assert(regbuf->rdata_len <= UINT16_MAX);

			/*
			 * 把调用方为此缓冲区提供的 rdata 链连接到
			 * 总列表上。
			 */
			bkpb.fork_flags |= BKPBLOCK_HAS_DATA;
			bkpb.data_length = (uint16) regbuf->rdata_len;
			total_len += regbuf->rdata_len;

			rdt_datas_last->next = regbuf->rdata_head;
			rdt_datas_last = regbuf->rdata_tail;
		}

		if (prev_regbuf && RelFileLocatorEquals(regbuf->rlocator, prev_regbuf->rlocator))
		{
			samerel = true;
			bkpb.fork_flags |= BKPBLOCK_SAME_REL;
		}
		else
			samerel = false;
		prev_regbuf = regbuf;

		/* 好，把头部复制到暂存缓冲区 */
		memcpy(scratch, &bkpb, SizeOfXLogRecordBlockHeader);
		scratch += SizeOfXLogRecordBlockHeader;
		if (include_image)
		{
			memcpy(scratch, &bimg, SizeOfXLogRecordBlockImageHeader);
			scratch += SizeOfXLogRecordBlockImageHeader;
			if (cbimg.hole_length != 0 && is_compressed)
			{
				memcpy(scratch, &cbimg,
					   SizeOfXLogRecordBlockCompressHeader);
				scratch += SizeOfXLogRecordBlockCompressHeader;
			}
		}
		if (!samerel)
		{
			memcpy(scratch, &regbuf->rlocator, sizeof(RelFileLocator));
			scratch += sizeof(RelFileLocator);
		}
		memcpy(scratch, &regbuf->block, sizeof(BlockNumber));
		scratch += sizeof(BlockNumber);
	}

	/* 随后是记录的源（若有） */
	if ((curinsert_flags & XLOG_INCLUDE_ORIGIN) &&
		replorigin_session_origin != InvalidRepOriginId)
	{
		*(scratch++) = (char) XLR_BLOCK_ID_ORIGIN;
		memcpy(scratch, &replorigin_session_origin, sizeof(replorigin_session_origin));
		scratch += sizeof(replorigin_session_origin);
	}

	/* 随后是顶层 XID（若尚未包含在先前记录中） */
	if (IsSubxactTopXidLogPending())
	{
		TransactionId xid = GetTopTransactionIdIfAny();

		/* 设置标志：顶层 xid 已包含在 WAL 中 */
		*topxid_included = true;

		*(scratch++) = (char) XLR_BLOCK_ID_TOPLEVEL_XID;
		memcpy(scratch, &xid, sizeof(TransactionId));
		scratch += sizeof(TransactionId);
	}

	/* 随后是主数据（若有） */
	if (mainrdata_len > 0)
	{
		if (mainrdata_len > 255)
		{
			uint32		mainrdata_len_4b;

			if (mainrdata_len > PG_UINT32_MAX)
				ereport(ERROR,
						(errmsg_internal("too much WAL data"),
						 errdetail_internal("Main data length is %llu bytes for a maximum of %u bytes.",
											(unsigned long long) mainrdata_len,
											PG_UINT32_MAX)));

			mainrdata_len_4b = (uint32) mainrdata_len;
			*(scratch++) = (char) XLR_BLOCK_ID_DATA_LONG;
			memcpy(scratch, &mainrdata_len_4b, sizeof(uint32));
			scratch += sizeof(uint32);
		}
		else
		{
			*(scratch++) = (char) XLR_BLOCK_ID_DATA_SHORT;
			*(scratch++) = (uint8) mainrdata_len;
		}
		rdt_datas_last->next = mainrdata_head;
		rdt_datas_last = mainrdata_last;
		total_len += mainrdata_len;
	}
	rdt_datas_last->next = NULL;

	hdr_rdt.len = (scratch - hdr_scratch);
	total_len += hdr_rdt.len;

	/*
	 * 计算数据的 CRC
	 *
	 * 注意：起初不会把记录头加入 CRC，因为我们
	 * 还不知道 prev 链接。因此 CRC 表示整条记录按
	 * 如下顺序的 CRC：rdata，然后备份块，然后记录
	 * 头。
	 */
	INIT_CRC32C(rdata_crc);
	COMP_CRC32C(rdata_crc, hdr_scratch + SizeOfXLogRecord, hdr_rdt.len - SizeOfXLogRecord);
	for (rdt = hdr_rdt.next; rdt != NULL; rdt = rdt->next)
		COMP_CRC32C(rdata_crc, rdt->data, rdt->len);

	/*
	 * 确保 XLogRecord 不会过大。
	 *
	 * XLogReader 机制只能处理一定大小的记录
	 * （忽略机器资源限制），因此确保我们不会发出超过
	 * 所宣称支持大小的记录。该上限基于
	 * DecodeXLogRecordRequiredSpace()。
	 */
	if (total_len > XLogRecordMaxSize)
		ereport(ERROR,
				(errmsg_internal("oversized WAL record"),
				 errdetail_internal("WAL record would be %llu bytes (of maximum %u bytes); rmid %u flags %u.",
									(unsigned long long) total_len, XLogRecordMaxSize, rmid, info)));

	/*
	 * 填写记录头中的字段。prev 链接稍后填写，
	 * 一旦我们知道记录将插入 WAL 的何处。CRC 尚
	 * 不包含记录头。
	 */
	rechdr->xl_xid = GetCurrentTransactionIdIfAny();
	rechdr->xl_tot_len = (uint32) total_len;
	rechdr->xl_info = info;
	rechdr->xl_rmid = rmid;
	rechdr->xl_prev = InvalidXLogRecPtr;
	rechdr->xl_crc = rdata_crc;

	return &hdr_rdt;
}

/*
 * 创建备份块映像的压缩版本。
 *
 * 若压缩失败（即压缩结果实际上比原版
 * 更大）则返回 false。否则返回 true，并将 'dlen' 设为
 * 压缩块映像的长度。
 */
static bool
XLogCompressBackupBlock(char *page, uint16 hole_offset, uint16 hole_length,
						char *dest, uint16 *dlen)
{
	int32		orig_len = BLCKSZ - hole_length;
	int32		len = -1;
	int32		extra_bytes = 0;
	char	   *source;
	PGAlignedBlock tmp;

	if (hole_length != 0)
	{
		/* 必须跳过空洞 */
		source = tmp.data;
		memcpy(source, page, hole_offset);
		memcpy(source + hole_offset,
			   page + (hole_offset + hole_length),
			   BLCKSZ - (hole_length + hole_offset));

		/*
		 * 若存在空洞，压缩版块映像需要在 WAL 记录中
		 * 额外存放一些数据。
		 */
		extra_bytes = SizeOfXLogRecordBlockCompressHeader;
	}
	else
		source = page;

	switch ((WalCompression) wal_compression)
	{
		case WAL_COMPRESSION_PGLZ:
			len = pglz_compress(source, orig_len, dest, PGLZ_strategy_default);
			break;

		case WAL_COMPRESSION_LZ4:
#ifdef USE_LZ4
			len = LZ4_compress_default(source, dest, orig_len,
									   COMPRESS_BUFSIZE);
			if (len <= 0)
				len = -1;		/* 失败 */
#else
			elog(ERROR, "LZ4 is not supported by this build");
#endif
			break;

		case WAL_COMPRESSION_ZSTD:
#ifdef USE_ZSTD
			len = ZSTD_compress(dest, COMPRESS_BUFSIZE, source, orig_len,
								ZSTD_CLEVEL_DEFAULT);
			if (ZSTD_isError(len))
				len = -1;		/* 失败 */
#else
			elog(ERROR, "zstd is not supported by this build");
#endif
			break;

		case WAL_COMPRESSION_NONE:
			Assert(false);		/* 不可能发生 */
			break;
			/* 无 default 分支，以便编译器发出警告 */
	}

	/*
	 * 即使压缩报告成功，我们也重新检查实际大小，看
	 * 压缩节省的字节数是否大于压缩版块映像所需的
	 * 额外数据长度。
	 */
	if (len >= 0 &&
		len + extra_bytes < orig_len)
	{
		*dlen = (uint16) len;	/* 压缩成功 */
		return true;
	}
	return false;
}

/*
 * 判定所引用的缓冲区是否需要备份。
 *
 * 由于我们尚无插入锁，fullPageWrites 与 runningBackups
 * （后者会强制整页写）之后可能改变，因此结果仅应
 * 用于优化目的。
 */
bool
XLogCheckBufferNeedsBackup(Buffer buffer)
{
	XLogRecPtr	RedoRecPtr;
	bool		doPageWrites;
	Page		page;

	GetFullPageWriteInfo(&RedoRecPtr, &doPageWrites);

	page = BufferGetPage(buffer);

	if (doPageWrites && PageGetLSN(page) <= RedoRecPtr)
		return true;			/* 缓冲区需要备份 */

	return false;				/* 缓冲区不需要备份 */
}

/*
 * 在设置提示时若需要则写入备份块。注意这可能针对
 * 多种页类型调用，不只是堆。
 *
 * 仅持有缓冲区内容共享锁时即可调用。
 *
 * 不能使用普通的备份块机制，因为那依赖对 Buffer 的
 * 排他锁。由于某些修改（设置 LSN、提示
 * 位）在共享锁缓冲区上是允许的，那可能导致 WAL 校验和
 * 失败。因此我们改为复制该页，并把副本数据作为普通
 * 记录数据插入。
 *
 * 仅当该页在本检查点轮次中尚未做过整页写时才需要
 * 处理。若我们不得不写入，则返回所插入 WAL 记录的 LSN，
 * 否则返回 InvalidXLogRecPtr。
 *
 * 多个并发后端可能尝试写 WAL
 * 记录。那时，同一块的多份副本会由不同后端记入各自的
 * WAL 记录，但从正确性角度看仍然
 * 没问题。
 */
XLogRecPtr
XLogSaveBufferForHint(Buffer buffer, bool buffer_std)
{
	XLogRecPtr	recptr = InvalidXLogRecPtr;
	XLogRecPtr	lsn;
	XLogRecPtr	RedoRecPtr;

	/*
	 * 确保没有检查点能改变我们对 RedoRecPtr 的视图。
	 */
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) != 0);

	/*
	 * 更新 RedoRecPtr，以便做出正确决策
	 */
	RedoRecPtr = GetRedoRecPtr();

	/*
	 * 我们假定可传给 XLogInsert 的*每一*页上，页 LSN 都是
	 * 最前面的数据，无论是否采用标准页布局。由于我们
	 * 仅持有该页的共享锁，查看 LSN 时必须获取缓冲区头
	 * 锁。
	 */
	lsn = BufferGetLSNAtomic(buffer);

	if (lsn <= RedoRecPtr)
	{
		int			flags = 0;
		PGAlignedBlock copied_buffer;
		char	   *origdata = (char *) BufferGetBlock(buffer);
		RelFileLocator rlocator;
		ForkNumber	forkno;
		BlockNumber blkno;

		/*
		 * 复制缓冲区，这样就不必担心并发的提示位或
		 * lsn 更新。我们假定无排他锁时 pd_lower/upper 不会被改，
		 * 因此备份内容不会有竞态。
		 */
		if (buffer_std)
		{
			/* 假定可省略 pd_lower 与 pd_upper 之间的数据 */
			Page		page = BufferGetPage(buffer);
			uint16		lower = ((PageHeader) page)->pd_lower;
			uint16		upper = ((PageHeader) page)->pd_upper;

			memcpy(copied_buffer.data, origdata, lower);
			memcpy(copied_buffer.data + upper, origdata + upper, BLCKSZ - upper);
		}
		else
			memcpy(copied_buffer.data, origdata, BLCKSZ);

		XLogBeginInsert();

		if (buffer_std)
			flags |= REGBUF_STANDARD;

		BufferGetTag(buffer, &rlocator, &forkno, &blkno);
		XLogRegisterBlock(0, &rlocator, forkno, blkno, copied_buffer.data, flags);

		recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI_FOR_HINT);
	}

	return recptr;
}

/*
 * 写入包含页完整映像的 WAL 记录。调用方负责在调用本例程后
 * 将该页写到磁盘。
 *
 * 注意：若使用本函数，应在私有内存中构建页面并直接
 * 写到 smgr。若使用缓冲区，请改为调用
 * log_newpage_buffer。
 *
 * 若页遵循标准页布局，带有 PageHeader 且 pd_lower 与
 * pd_upper 之间有未用空间，则将 'page_std' 设为 true。这样可
 * 把未用空间排除在 WAL 记录之外，使记录更小。
 */
XLogRecPtr
log_newpage(RelFileLocator *rlocator, ForkNumber forknum, BlockNumber blkno,
			Page page, bool page_std)
{
	int			flags;
	XLogRecPtr	recptr;

	flags = REGBUF_FORCE_IMAGE;
	if (page_std)
		flags |= REGBUF_STANDARD;

	XLogBeginInsert();
	XLogRegisterBlock(0, rlocator, forknum, blkno, page, flags);
	recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI);

	/*
	 * 该页可能尚未初始化。若是，我们不能设置 LSN，因为那会
	 * 损坏该页。
	 */
	if (!PageIsNew(page))
	{
		PageSetLSN(page, recptr);
	}

	return recptr;
}

/*
 * 类似 log_newpage()，但允许在一次操作中记录多页。
 * 比分别对每页调用 log_newpage() 更高效，因为我们可在
 * 单条 WAL 记录中写入多页。
 */
void
log_newpages(RelFileLocator *rlocator, ForkNumber forknum, int num_pages,
			 BlockNumber *blknos, Page *pages, bool page_std)
{
	int			flags;
	XLogRecPtr	recptr;
	int			i;
	int			j;

	flags = REGBUF_FORCE_IMAGE;
	if (page_std)
		flags |= REGBUF_STANDARD;

	/*
	 * 遍历所有页。它们被收集成每批
	 * XLR_MAX_BLOCK_ID 页，并为每批写一条
	 * WAL 记录。
	 */
	XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID - 1, 0);

	i = 0;
	while (i < num_pages)
	{
		int			batch_start = i;
		int			nbatch;

		XLogBeginInsert();

		nbatch = 0;
		while (nbatch < XLR_MAX_BLOCK_ID && i < num_pages)
		{
			XLogRegisterBlock(nbatch, rlocator, forknum, blknos[i], pages[i], flags);
			i++;
			nbatch++;
		}

		recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI);

		for (j = batch_start; j < i; j++)
		{
			/*
			 * 该页可能尚未初始化。若是，我们不能设置 LSN，
			 * 因为那会损坏该页。
			 */
			if (!PageIsNew(pages[j]))
			{
				PageSetLSN(pages[j], recptr);
			}
		}
	}
}

/*
 * 写入包含页完整映像的 WAL 记录。
 *
 * 调用方应在调用本函数前初始化缓冲区并标脏。
 * 本函数会设置页 LSN。
 *
 * 若页遵循标准页布局，带有 PageHeader 且 pd_lower 与
 * pd_upper 之间有未用空间，则将 'page_std' 设为 true。这样可
 * 把未用空间排除在 WAL 记录之外，使记录更小。
 */
XLogRecPtr
log_newpage_buffer(Buffer buffer, bool page_std)
{
	Page		page = BufferGetPage(buffer);
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;

	/* 共享缓冲区应在临界区内修改。 */
	Assert(CritSectionCount > 0);

	BufferGetTag(buffer, &rlocator, &forknum, &blkno);

	return log_newpage(&rlocator, forknum, blkno, page, page_std);
}

/*
 * 对关系中一段块范围做 WAL 记录。
 *
 * 块号满足 'startblk' <= X < 'endblk' 的所有页的映像
 * 会写入 WAL。若范围很大，则拆成多条 WAL
 * 记录完成。
 *
 * 若所有页都遵循标准页布局，带有 PageHeader 且
 * pd_lower 与 pd_upper 之间有未用空间，则将 'page_std' 设为 true。
 * 这样可把未用空间排除在 WAL 记录之外，使记录更小。
 *
 * 注意：本函数对这些页获取排他锁。通常用于
 * 新建的关系，且调用方持有
 * AccessExclusiveLock，因而不会有其他后端同时访问。
 * 若非如此，你必须通过其他方式确保这不会
 * 造成死锁。
 */
void
log_newpage_range(Relation rel, ForkNumber forknum,
				  BlockNumber startblk, BlockNumber endblk,
				  bool page_std)
{
	int			flags;
	BlockNumber blkno;

	flags = REGBUF_FORCE_IMAGE;
	if (page_std)
		flags |= REGBUF_STANDARD;

	/*
	 * 遍历范围内的所有页。它们被收集成每批
	 * XLR_MAX_BLOCK_ID 页，并为每批写一条
	 * WAL 记录。
	 */
	XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID - 1, 0);

	blkno = startblk;
	while (blkno < endblk)
	{
		Buffer		bufpack[XLR_MAX_BLOCK_ID];
		XLogRecPtr	recptr;
		int			nbufs;
		int			i;

		CHECK_FOR_INTERRUPTS();

		/* 收集一批块。 */
		nbufs = 0;
		while (nbufs < XLR_MAX_BLOCK_ID && blkno < endblk)
		{
			Buffer		buf = ReadBufferExtended(rel, forknum, blkno,
												 RBM_NORMAL, NULL);

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * 完全空的页不做 WAL 记录。写 WAL 记录会
			 * 改变 LSN，而我们不希望那样。我们希望页
			 * 保持为空。
			 */
			if (!PageIsNew(BufferGetPage(buf)))
				bufpack[nbufs++] = buf;
			else
				UnlockReleaseBuffer(buf);
			blkno++;
		}

		/* 若其余块都为空，则无需再做任何事。 */
		if (nbufs == 0)
			break;

		/* 为本批写入 WAL 记录。 */
		XLogBeginInsert();

		START_CRIT_SECTION();
		for (i = 0; i < nbufs; i++)
		{
			XLogRegisterBuffer(i, bufpack[i], flags);
			MarkBufferDirty(bufpack[i]);
		}

		recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI);

		for (i = 0; i < nbufs; i++)
		{
			PageSetLSN(BufferGetPage(bufpack[i]), recptr);
			UnlockReleaseBuffer(bufpack[i]);
		}
		END_CRIT_SECTION();
	}
}

/*
 * 分配构造 WAL 记录所需的工作缓冲区。
 */
void
InitXLogInsert(void)
{
#ifdef USE_ASSERT_CHECKING

	/*
	 * 检查组装出的任何记录都能被解码。上限基于
	 * XLogReader 在其最大边界处所需的空间。此代码路径
	 * 每个后端调用一次，对本检查已绰绰有余。
	 */
	size_t		max_required = DecodeXLogRecordRequiredSpace(XLogRecordMaxSize);

	Assert(AllocSizeIsValid(max_required));
#endif

	/* 初始化工作区 */
	if (xloginsert_cxt == NULL)
	{
		xloginsert_cxt = AllocSetContextCreate(TopMemoryContext,
											   "WAL record construction",
											   ALLOCSET_DEFAULT_SIZES);
	}

	if (registered_buffers == NULL)
	{
		registered_buffers = (registered_buffer *)
			MemoryContextAllocZero(xloginsert_cxt,
								   sizeof(registered_buffer) * (XLR_NORMAL_MAX_BLOCK_ID + 1));
		max_registered_buffers = XLR_NORMAL_MAX_BLOCK_ID + 1;
	}
	if (rdatas == NULL)
	{
		rdatas = MemoryContextAlloc(xloginsert_cxt,
									sizeof(XLogRecData) * XLR_NORMAL_RDATAS);
		max_rdatas = XLR_NORMAL_RDATAS;
	}

	/*
	 * 分配用于保存 WAL 记录头信息的缓冲区。
	 */
	if (hdr_scratch == NULL)
		hdr_scratch = MemoryContextAllocZero(xloginsert_cxt,
											 HEADER_SCRATCH_SIZE);
}
