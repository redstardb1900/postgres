/*-------------------------------------------------------------------------
 *
 * xlogreader.c
 *		通用 XLog 读取设施
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogreader.c
 *
 * NOTES
 *		更多说明见 xlogreader.h。
 *
 *		本文件同时作为前端与后端代码编译，因此
 *		不得使用 ereport、服务器定义的静态变量等。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifdef USE_LZ4
#include <lz4.h>
#endif
#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "catalog/pg_control.h"
#include "common/pg_lzcompress.h"
#include "replication/origin.h"

#ifndef FRONTEND
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"
#else
#include "common/logging.h"
#endif

static void report_invalid_record(XLogReaderState *state, const char *fmt,...)
			pg_attribute_printf(2, 3);
static void allocate_recordbuf(XLogReaderState *state, uint32 reclength);
static int	ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr,
							 int reqLen);
static void XLogReaderInvalReadState(XLogReaderState *state);
static XLogPageReadResult XLogDecodeNextRecord(XLogReaderState *state, bool nonblocking);
static bool ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
								  XLogRecPtr PrevRecPtr, XLogRecord *record, bool randAccess);
static bool ValidXLogRecord(XLogReaderState *state, XLogRecord *record,
							XLogRecPtr recptr);
static void ResetDecoder(XLogReaderState *state);
static void WALOpenSegmentInit(WALOpenSegment *seg, WALSegmentContext *segcxt,
							   int segsize, const char *waldir);

/* 为错误消息分配的缓冲区大小。 */
#define MAX_ERRORMSG_LEN 1000

/*
 * 默认大小；足够大，使 XLogReader 的典型使用者通常
 * 不必走"超大"内存分配代码路径。
 */
#define DEFAULT_DECODE_BUFFER_SIZE (64 * 1024)

/*
 * 在 state->errormsg_buf 中构造字符串，说明当前
 * 正在读取的记录有何问题。
 */
static void
report_invalid_record(XLogReaderState *state, const char *fmt,...)
{
	va_list		args;

	fmt = _(fmt);

	va_start(args, fmt);
	vsnprintf(state->errormsg_buf, MAX_ERRORMSG_LEN, fmt, args);
	va_end(args);

	state->errormsg_deferred = true;
}

/*
 * 设置解码缓冲区大小。也可传入调用者提供的内存区
 * 指针；此时非超大记录将在该区中解码。
 *
 */
void
XLogReaderSetDecodeBuffer(XLogReaderState *state, void *buffer, size_t size)
{
	Assert(state->decode_buffer == NULL);

	state->decode_buffer = buffer;
	state->decode_buffer_size = size;
	state->decode_buffer_tail = buffer;
	state->decode_buffer_head = buffer;
}

/*
 * 分配并初始化一个新的 XLogReader。
 *
 * 若无法分配 xlogreader，则返回 NULL。
 */
XLogReaderState *
XLogReaderAllocate(int wal_segment_size, const char *waldir,
				   XLogReaderRoutine *routine, void *private_data)
{
	XLogReaderState *state;

	state = (XLogReaderState *)
		palloc_extended(sizeof(XLogReaderState),
						MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
	if (!state)
		return NULL;

	/* 初始化调用者提供的支持函数 */
	state->routine = *routine;

	/*
	 * 永久分配 readBuf。之所以这样做而不是直接做静态
	 * 数组，有两个原因：(1) 在大多数后端实例中不必浪费
	 * 这段存储；(2) 静态 char 数组不保证任何特定对齐，而
	 * palloc_extended() 会提供 MAXALIGN'd 存储。
	 *
	 */
	state->readBuf = (char *) palloc_extended(XLOG_BLCKSZ,
											  MCXT_ALLOC_NO_OOM);
	if (!state->readBuf)
	{
		pfree(state);
		return NULL;
	}

	/* 初始化段信息。 */
	WALOpenSegmentInit(&state->seg, &state->segcxt, wal_segment_size,
					   waldir);

	/* system_identifier 已在上面初始化为零 */
	state->private_data = private_data;
	/* ReadRecPtr、EndRecPtr 与 readLen 已在上面初始化为零 */
	state->errormsg_buf = palloc_extended(MAX_ERRORMSG_LEN + 1,
										  MCXT_ALLOC_NO_OOM);
	if (!state->errormsg_buf)
	{
		pfree(state->readBuf);
		pfree(state);
		return NULL;
	}
	state->errormsg_buf[0] = '\0';

	/*
	 * 分配初始的最小 readRecordBuf，必要时以后可
	 * 再扩大。
	 */
	allocate_recordbuf(state, 0);
	return state;
}

void
XLogReaderFree(XLogReaderState *state)
{
	if (state->seg.ws_file != -1)
		state->routine.segment_close(state);

	if (state->decode_buffer && state->free_decode_buffer)
		pfree(state->decode_buffer);

	pfree(state->errormsg_buf);
	if (state->readRecordBuf)
		pfree(state->readRecordBuf);
	pfree(state->readBuf);
	pfree(state);
}

/*
 * 分配 readRecordBuf，使其至少能容纳给定长度的记录。
 *
 * readRecordBufSize 被设为新的缓冲区大小。
 *
 * 为避免无用的小幅增长，将其大小取整为 XLOG_BLCKSZ
 * 的倍数，并保证起步至少为 5*Max(BLCKSZ, XLOG_BLCKSZ)。
 * （这对所有"正常"记录都够用，但很大的提交或
 * 中止记录可能需要更多空间。）
 *
 * 注意：在记录头尚未完全校验之前，*绝不可*对
 * xl_tot_len 调用本例程。
 */
static void
allocate_recordbuf(XLogReaderState *state, uint32 reclength)
{
	uint32		newSize = reclength;

	newSize += XLOG_BLCKSZ - (newSize % XLOG_BLCKSZ);
	newSize = Max(newSize, 5 * Max(BLCKSZ, XLOG_BLCKSZ));

	if (state->readRecordBuf)
		pfree(state->readRecordBuf);
	state->readRecordBuf = (char *) palloc(newSize);
	state->readRecordBufSize = newSize;
}

/*
 * 初始化传入的段结构。
 */
static void
WALOpenSegmentInit(WALOpenSegment *seg, WALSegmentContext *segcxt,
				   int segsize, const char *waldir)
{
	seg->ws_file = -1;
	seg->ws_segno = 0;
	seg->ws_tli = 0;

	segcxt->ws_segsize = segsize;
	if (waldir)
		snprintf(segcxt->ws_dir, MAXPGPATH, "%s", waldir);
}

/*
 * 从 'RecPtr' 开始读取 WAL。
 *
 * 'RecPtr' 应指向一条有效 WAL 记录的起点。指向页
 * 起点也可以，只要页头之后紧跟着新记录，
 * 而非续写片段。
 *
 * 此时并不尝试真正读 WAL，因此不会失败。
 * 若起始地址不正确，首次调用 XLogReadRecord()
 * 时会报错。
 */
void
XLogBeginRead(XLogReaderState *state, XLogRecPtr RecPtr)
{
	Assert(!XLogRecPtrIsInvalid(RecPtr));

	ResetDecoder(state);

	/* 从传入的记录指针开始。 */
	state->EndRecPtr = RecPtr;
	state->NextRecPtr = RecPtr;
	state->ReadRecPtr = InvalidXLogRecPtr;
	state->DecodeRecPtr = InvalidXLogRecPtr;
}

/*
 * 若存在，释放上次由 XLogNextRecord() 返回的记录以
 * 腾出空间。返回该记录末尾之后的 LSN。
 */
XLogRecPtr
XLogReleasePreviousRecord(XLogReaderState *state)
{
	DecodedXLogRecord *record;
	XLogRecPtr	next_lsn;

	if (!state->record)
		return InvalidXLogRecPtr;

	/*
	 * 将其从已解码记录队列中移除。它必须是最旧的
	 * 已解码项，即 decode_queue_head。
	 */
	record = state->record;
	next_lsn = record->next_lsn;
	Assert(record == state->decode_queue_head);
	state->record = NULL;
	state->decode_queue_head = record->next;

	/* 它也可能是最新已解码项，即 decode_queue_tail。 */
	if (state->decode_queue_tail == record)
		state->decode_queue_tail = NULL;

	/* 释放空间。 */
	if (unlikely(record->oversized))
	{
		/* 它不在解码缓冲区中，因此用 free 释放空间。 */
		pfree(record);
	}
	else
	{
		/* 它必须是解码缓冲区中的队头（最旧）记录。 */
		Assert(state->decode_buffer_head == (char *) record);

		/*
		 * 需要把 head 更新为指向解码缓冲区中的下一条
		 * 记录（若有），并小心地跳过超大记录
		 * （它们不在解码缓冲区中）。
		 */
		record = record->next;
		while (unlikely(record && record->oversized))
			record = record->next;

		if (record)
		{
			/* 调整 head，以释放直到下一条记录为止的空间。 */
			state->decode_buffer_head = (char *) record;
		}
		else
		{
			/*
			 * 否则不妨把 head 与 tail 重置到缓冲区空间
			 * 起点，因为我们已经空了。这意味着若不做
			 * 任何预取，就会反复覆盖同一块内存。
			 *
			 */
			state->decode_buffer_head = state->decode_buffer;
			state->decode_buffer_tail = state->decode_buffer;
		}
	}

	return next_lsn;
}

/*
 * 尝试读取一条 XLOG 记录。
 *
 * 在首次调用 XLogNextRecord() 之前，必须先调用
 * XLogBeginRead() 或 XLogFindNextRecord()，然后再调用 XLogReadAhead()。
 * 本函数返回由 XLogReadAhead() 放入内部队列的记录与错误。
 *
 * 成功时返回一条记录。
 *
 * 返回的记录（或 *errormsg）指向内部缓冲区，在
 * 下次调用 XLogNextRecord 之前有效。
 */
DecodedXLogRecord *
XLogNextRecord(XLogReaderState *state, char **errormsg)
{
	/* 释放上次由 XLogNextRecord() 返回的记录。 */
	XLogReleasePreviousRecord(state);

	if (state->decode_queue_head == NULL)
	{
		*errormsg = NULL;
		if (state->errormsg_deferred)
		{
			if (state->errormsg_buf[0] != '\0')
				*errormsg = state->errormsg_buf;
			state->errormsg_deferred = false;
		}

		/*
		 * 预期 state->EndRecPtr 已由上次对 XLogBeginRead()
		 * 或 XLogNextRecord() 的调用设置，且即为错误
		 * 所在位置。
		 */
		Assert(!XLogRecPtrIsInvalid(state->EndRecPtr));

		return NULL;
	}

	/*
	 * 将此记为最近返回的记录，以便下次释放。
	 * 这也使其对传统的 XLogRecXXX(xlogreader) 宏可见，
	 * 这些宏因历史原因面向解码器而非记录本身。
	 *
	 */
	state->record = state->decode_queue_head;

	/*
	 * 更新指向本记录起点与末尾之后一字节的指针，
	 * 同样是为了照顾那些预期由解码器跟踪这些值、
	 * 而不是访问记录自身字段的历史代码。
	 *
	 */
	state->ReadRecPtr = state->record->lsn;
	state->EndRecPtr = state->record->next_lsn;

	*errormsg = NULL;

	return state->record;
}

/*
 * 尝试读取一条 XLOG 记录。
 *
 * 在首次调用 XLogReadRecord() 之前，必须先调用
 * XLogBeginRead() 或 XLogFindNextRecord()。
 *
 * 若 page_read 回调未能读到所请求数据，则返回 NULL。
 * 预期回调已报告错误；errormsg 被置为 NULL。
 *
 * 若因其他原因读取失败，也返回 NULL，并
 * 将 *errormsg 设为说明失败细节的字符串。
 *
 * 返回的指针（或 *errormsg）指向内部缓冲区，在
 * 下次调用 XLogReadRecord 之前有效。
 *
 */
XLogRecord *
XLogReadRecord(XLogReaderState *state, char **errormsg)
{
	DecodedXLogRecord *decoded;

	/*
	 * 若存在上次返回的记录则释放。这样做是为了
	 * 能准确检查解码队列是否为空。
	 */
	XLogReleasePreviousRecord(state);

	/*
	 * 以阻塞模式调用 XLogReadAhead()，确保队列中
	 * 有内容，尽管我们并不使用返回值。
	 */
	if (!XLogReaderHasQueuedRecordOrError(state))
		XLogReadAhead(state, false /* 非阻塞 */ );

	/* 消费队头记录或错误。 */
	decoded = XLogNextRecord(state, errormsg);
	if (decoded)
	{
		/*
		 * 本函数返回指向记录头的指针，而非实际的
		 * 已解码记录。调用者通过 XLogRecGetXXX() 宏
		 * 访问已解码记录，这些宏经由
		 * xlogreader->record 到达已解码记录。
		 */
		Assert(state->record == decoded);
		return &decoded->header;
	}

	return NULL;
}

/*
 * 为已解码记录分配空间。返回对象中唯一已初始化的
 * 成员是 'oversized' 标志，表示该已解码记录装不进
 * 解码缓冲区，最终必须显式释放。
 *
 * 调用者负责在成功把记录解码进该空间后，用实际
 * 大小调整 decode_buffer_tail。这样若解码失败，
 * 则除 'oversized' 已置位而必须 pfree() 外，无需撤销。
 *
 * 若解码缓冲区无空间且 allow_oversized 为假，或
 * 超大缓冲区内存分配失败，则返回 NULL。
 *
 *
 */
static DecodedXLogRecord *
XLogReadRecordAlloc(XLogReaderState *state, size_t xl_tot_len, bool allow_oversized)
{
	size_t		required_space = DecodeXLogRecordRequiredSpace(xl_tot_len);
	DecodedXLogRecord *decoded = NULL;

	/* 若尚无解码缓冲区，则分配一个环形解码缓冲区。 */
	if (unlikely(state->decode_buffer == NULL))
	{
		if (state->decode_buffer_size == 0)
			state->decode_buffer_size = DEFAULT_DECODE_BUFFER_SIZE;
		state->decode_buffer = palloc(state->decode_buffer_size);
		state->decode_buffer_head = state->decode_buffer;
		state->decode_buffer_tail = state->decode_buffer;
		state->free_decode_buffer = true;
	}

	/* 尝试在环形解码缓冲区中分配空间。 */
	if (state->decode_buffer_tail >= state->decode_buffer_head)
	{
		/* 为空，或 tail 在 head 右侧。 */
		if (required_space <=
			state->decode_buffer_size -
			(state->decode_buffer_tail - state->decode_buffer))
		{
			/*-
			 * 尾部与末尾之间有空间。
			 *
			 * +-----+--------------------+-----+
			 * |     |////////////////////|here!|
			 * +-----+--------------------+-----+
			 *       ^                    ^
			 *       |                    |
			 *       h                    t
			 */
			decoded = (DecodedXLogRecord *) state->decode_buffer_tail;
			decoded->oversized = false;
			return decoded;
		}
		else if (required_space <
				 state->decode_buffer_head - state->decode_buffer)
		{
			/*-
			 * 起点与 head 之间有空间。
			 *
			 * +-----+--------------------+-----+
			 * |here!|////////////////////|     |
			 * +-----+--------------------+-----+
			 *       ^                    ^
			 *       |                    |
			 *       h                    t
			 */
			decoded = (DecodedXLogRecord *) state->decode_buffer;
			decoded->oversized = false;
			return decoded;
		}
	}
	else
	{
		/* Tail 在 head 左侧。 */
		if (required_space <
			state->decode_buffer_head - state->decode_buffer_tail)
		{
			/*-
			 * 尾部与 head 之间有空间。
			 *
			 * +-----+--------------------+-----+
			 * |/////|here!               |/////|
			 * +-----+--------------------+-----+
			 *       ^                    ^
			 *       |                    |
			 *       t                    h
			 */
			decoded = (DecodedXLogRecord *) state->decode_buffer_tail;
			decoded->oversized = false;
			return decoded;
		}
	}

	/* 解码缓冲区空间不足。是否允许再分配？ */
	if (allow_oversized)
	{
		decoded = palloc(required_space);
		decoded->oversized = true;
		return decoded;
	}

	return NULL;
}

static XLogPageReadResult
XLogDecodeNextRecord(XLogReaderState *state, bool nonblocking)
{
	XLogRecPtr	RecPtr;
	XLogRecord *record;
	XLogRecPtr	targetPagePtr;
	bool		randAccess;
	uint32		len,
				total_len;
	uint32		targetRecOff;
	uint32		pageHeaderSize;
	bool		assembled;
	bool		gotheader;
	int			readOff;
	DecodedXLogRecord *decoded;
	char	   *errormsg;		/* 未使用 */

	/*
	 * randAccess 表示是否校验正在读取记录的
	 * 前一记录指针。仅在顺序读取时这样做，而
	 * 我们初始假定即是顺序读取。
	 */
	randAccess = false;

	/* 重置错误状态 */
	state->errormsg_buf[0] = '\0';
	decoded = NULL;

	state->abortedRecPtr = InvalidXLogRecPtr;
	state->missingContrecPtr = InvalidXLogRecPtr;

	RecPtr = state->NextRecPtr;

	if (state->DecodeRecPtr != InvalidXLogRecPtr)
	{
		/* 读取我们刚读完的那条记录之后的记录 */

		/*
		 * NextRecPtr 指向前一条 WAL 记录的 end+1。若
		 * 正处于页边界，当前页装不下更多记录。必须
		 * 跳过页头，但在读入该页之前不能跳，因为
		 * 页头大小是可变的。
		 */
	}
	else
	{
		/*
		 * 调用者提供了起始位置。
		 *
		 * 此时 NextRecPtr 应已指向有效记录起始位置，
		 * 或指向某页的起点。参见 XLogBeginRead
		 * 的头注释。
		 */
		Assert(RecPtr % XLOG_BLCKSZ == 0 || XRecOffIsValid(RecPtr));
		randAccess = true;
	}

restart:
	state->nonblocking = nonblocking;
	state->currRecPtr = RecPtr;
	assembled = false;

	targetPagePtr = RecPtr - (RecPtr % XLOG_BLCKSZ);
	targetRecOff = RecPtr % XLOG_BLCKSZ;

	/*
	 * 把包含该记录的页读入 state->readBuf。请求足够
	 * 字节以覆盖整个记录头，或至少覆盖落在同一页
	 * 上的那部分。
	 */
	readOff = ReadPageInternal(state, targetPagePtr,
							   Min(targetRecOff + SizeOfXLogRecord, XLOG_BLCKSZ));
	if (readOff == XLREAD_WOULDBLOCK)
		return XLREAD_WOULDBLOCK;
	else if (readOff < 0)
		goto err;

	/*
	 * ReadPageInternal 总是至少返回页头，因此现在
	 * 可以检查它。
	 */
	pageHeaderSize = XLogPageHeaderSize((XLogPageHeader) state->readBuf);
	if (targetRecOff == 0)
	{
		/*
		 * 位于页起点，因此跳过页头。
		 */
		RecPtr += pageHeaderSize;
		targetRecOff = pageHeaderSize;
	}
	else if (targetRecOff < pageHeaderSize)
	{
		report_invalid_record(state, "invalid record offset at %X/%X: expected at least %u, got %u",
							  LSN_FORMAT_ARGS(RecPtr),
							  pageHeaderSize, targetRecOff);
		goto err;
	}

	if ((((XLogPageHeader) state->readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
		targetRecOff == pageHeaderSize)
	{
		report_invalid_record(state, "contrecord is requested by %X/%X",
							  LSN_FORMAT_ARGS(RecPtr));
		goto err;
	}

	/* ReadPageInternal 已校验页头 */
	Assert(pageHeaderSize <= readOff);

	/*
	 * 读取记录长度。
	 *
	 * 注意：尽管这里用了 XLogRecord 指针，整个记录
	 * 头未必落在本页。xl_tot_len 是结构体第一个字段，
	 * 因此它必然在本页（记录是 MAXALIGNed），但在
	 * 确认已拿到完整头之前，不得访问任何其他字段。
	 *
	 */
	record = (XLogRecord *) (state->readBuf + RecPtr % XLOG_BLCKSZ);
	total_len = record->xl_tot_len;

	/*
	 * 若整个记录头都在本页，立即校验它。否则仅对
	 * xl_tot_len 做基本健全性检查，待从下一页读完
	 * 其余头部后再校验。这里的 xl_tot_len 检查是必要的，
	 * 以确保进入下方"需要重装记录"代码路径；否则
	 * 可能根本不调用 ValidXLogRecordHeader。
	 *
	 */
	if (targetRecOff <= XLOG_BLCKSZ - SizeOfXLogRecord)
	{
		if (!ValidXLogRecordHeader(state, RecPtr, state->DecodeRecPtr, record,
								   randAccess))
			goto err;
		gotheader = true;
	}
	else
	{
		/* 若太小，可能没有下一页。 */
		if (total_len < SizeOfXLogRecord)
		{
			report_invalid_record(state,
								  "invalid record length at %X/%X: expected at least %u, got %u",
								  LSN_FORMAT_ARGS(RecPtr),
								  (uint32) SizeOfXLogRecord, total_len);
			goto err;
		}
		/* 拿到下一页后再校验头部。 */
		gotheader = false;
	}

	/*
	 * 尝试在不调用 palloc 的情况下为本记录找解码
	 * 空间。若不能，则在下方验证 total_len 并非
	 * 来自回收 WAL 页的垃圾字节之后重试。
	 */
	decoded = XLogReadRecordAlloc(state,
								  total_len,
								  false /* allow_oversized */ );
	if (decoded == NULL && nonblocking)
	{
		/*
		 * 环形解码缓冲区没有空间，且调用者只是在
		 * 向前预读。调用者应消费已有记录以腾出
		 * 空间。
		 */
		return XLREAD_WOULDBLOCK;
	}

	len = XLOG_BLCKSZ - RecPtr % XLOG_BLCKSZ;
	if (total_len > len)
	{
		/* 需要重装记录 */
		char	   *contdata;
		XLogPageHeader pageHeader;
		char	   *buffer;
		uint32		gotlen;

		assembled = true;

		/*
		 * 我们总是有几页的空间，足以校验跨页边界的
		 * 记录头。
		 */
		Assert(state->readRecordBufSize >= XLOG_BLCKSZ * 2);
		Assert(state->readRecordBufSize >= len);

		/* 从第一页复制记录的第一段片段。 */
		memcpy(state->readRecordBuf,
			   state->readBuf + RecPtr % XLOG_BLCKSZ, len);
		buffer = state->readRecordBuf + len;
		gotlen = len;

		do
		{
			/* 计算指向下一页起点的指针 */
			targetPagePtr += XLOG_BLCKSZ;

			/*
			 * 在处理记录数据之前先读页头，以便能处理
			 * 前一条记录以不完整续写片段结束的情况。
			 *
			 */
			readOff = ReadPageInternal(state, targetPagePtr, SizeOfXLogShortPHD);
			if (readOff == XLREAD_WOULDBLOCK)
				return XLREAD_WOULDBLOCK;
			else if (readOff < 0)
				goto err;

			Assert(SizeOfXLogShortPHD <= readOff);

			pageHeader = (XLogPageHeader) state->readBuf;

			/*
			 * 若我们期望续写记录却得到"覆盖 contrecord"
			 * 标志，说明续写记录已被不同记录覆盖。以
			 * 发现该标志的位置为读取地址重新开始；但
			 * 记下我们正在读的记录的 LSN，供稍后校验。
			 *
			 *
			 */
			if (pageHeader->xlp_info & XLP_FIRST_IS_OVERWRITE_CONTRECORD)
			{
				state->overwrittenRecPtr = RecPtr;
				RecPtr = targetPagePtr;
				goto restart;
			}

			/* 检查下一页上的续写片段看起来是否有效 */
			if (!(pageHeader->xlp_info & XLP_FIRST_IS_CONTRECORD))
			{
				report_invalid_record(state,
									  "there is no contrecord flag at %X/%X",
									  LSN_FORMAT_ARGS(RecPtr));
				goto err;
			}

			/*
			 * 交叉检查：xlp_rem_len 应与我们预期该记录
			 * 还剩多少相符。
			 */
			if (pageHeader->xlp_rem_len == 0 ||
				total_len != (pageHeader->xlp_rem_len + gotlen))
			{
				report_invalid_record(state,
									  "invalid contrecord length %u (expected %lld) at %X/%X",
									  pageHeader->xlp_rem_len,
									  ((long long) total_len) - gotlen,
									  LSN_FORMAT_ARGS(RecPtr));
				goto err;
			}

			/* 等待下一页变为可用 */
			readOff = ReadPageInternal(state, targetPagePtr,
									   Min(total_len - gotlen + SizeOfXLogShortPHD,
										   XLOG_BLCKSZ));
			if (readOff == XLREAD_WOULDBLOCK)
				return XLREAD_WOULDBLOCK;
			else if (readOff < 0)
				goto err;

			/* 把本页的续写片段追加到缓冲区 */
			pageHeaderSize = XLogPageHeaderSize(pageHeader);

			if (readOff < pageHeaderSize)
				readOff = ReadPageInternal(state, targetPagePtr,
										   pageHeaderSize);

			Assert(pageHeaderSize <= readOff);

			contdata = (char *) state->readBuf + pageHeaderSize;
			len = XLOG_BLCKSZ - pageHeaderSize;
			if (pageHeader->xlp_rem_len < len)
				len = pageHeader->xlp_rem_len;

			if (readOff < pageHeaderSize + len)
				readOff = ReadPageInternal(state, targetPagePtr,
										   pageHeaderSize + len);

			memcpy(buffer, (char *) contdata, len);
			buffer += len;
			gotlen += len;

			/* 若刚重装完记录头，则校验它。 */
			if (!gotheader)
			{
				record = (XLogRecord *) state->readRecordBuf;
				if (!ValidXLogRecordHeader(state, RecPtr, state->DecodeRecPtr,
										   record, randAccess))
					goto err;
				gotheader = true;
			}

			/*
			 * 可能需要更大的缓冲区。在记录头跨页的情况下
			 * 我们已校验过它。我们也已对照第二页的
			 * xlp_rem_len 交叉检查了 total_len，并校验了
			 * 两页上的 xlp_pageaddr。
			 */
			if (total_len > state->readRecordBufSize)
			{
				char		save_copy[XLOG_BLCKSZ * 2];

				/*
				 * 保存并恢复已有数据。它不可能超过
				 * 两页。
				 */
				Assert(gotlen <= lengthof(save_copy));
				Assert(gotlen <= state->readRecordBufSize);
				memcpy(save_copy, state->readRecordBuf, gotlen);
				allocate_recordbuf(state, total_len);
				memcpy(state->readRecordBuf, save_copy, gotlen);
				buffer = state->readRecordBuf + gotlen;
			}
		} while (gotlen < total_len);
		Assert(gotheader);

		record = (XLogRecord *) state->readRecordBuf;
		if (!ValidXLogRecord(state, record, RecPtr))
			goto err;

		pageHeaderSize = XLogPageHeaderSize((XLogPageHeader) state->readBuf);
		state->DecodeRecPtr = RecPtr;
		state->NextRecPtr = targetPagePtr + pageHeaderSize
			+ MAXALIGN(pageHeader->xlp_rem_len);
	}
	else
	{
		/* 等待记录数据变为可用 */
		readOff = ReadPageInternal(state, targetPagePtr,
								   Min(targetRecOff + total_len, XLOG_BLCKSZ));
		if (readOff == XLREAD_WOULDBLOCK)
			return XLREAD_WOULDBLOCK;
		else if (readOff < 0)
			goto err;

		/* 记录不跨越页边界 */
		if (!ValidXLogRecord(state, record, RecPtr))
			goto err;

		state->NextRecPtr = RecPtr + MAXALIGN(total_len);

		state->DecodeRecPtr = RecPtr;
	}

	/*
	 * 若是 XLOG SWITCH 记录则做特殊处理
	 */
	if (record->xl_rmid == RM_XLOG_ID &&
		(record->xl_info & ~XLR_INFO_MASK) == XLOG_SWITCH)
	{
		/* 假装它延伸到段末 */
		state->NextRecPtr += state->segcxt.ws_segsize - 1;
		state->NextRecPtr -= XLogSegmentOffset(state->NextRecPtr, state->segcxt.ws_segsize);
	}

	/*
	 * 若到达此处时尚无 DecodedXLogRecord，说明我们需要
	 * 先校验 total_len 再信任它，但现在已经完成校验。
	 */
	if (decoded == NULL)
	{
		Assert(!nonblocking);
		decoded = XLogReadRecordAlloc(state,
									  total_len,
									  true /* allow_oversized */ );
		/* 分配应总是在 allow_oversized 下发生 */
		Assert(decoded != NULL);
	}

	if (DecodeXLogRecord(state, decoded, record, RecPtr, &errormsg))
	{
		/* 记录下一条记录的位置。 */
		decoded->next_lsn = state->NextRecPtr;

		/*
		 * 若它在解码缓冲区中，则把该解码缓冲区空间
		 * 标记为已占用。
		 */
		if (!decoded->oversized)
		{
			/* 新的解码缓冲区 head 必须是 MAXALIGNed。 */
			Assert(decoded->size == MAXALIGN(decoded->size));
			if ((char *) decoded == state->decode_buffer)
				state->decode_buffer_tail = state->decode_buffer + decoded->size;
			else
				state->decode_buffer_tail += decoded->size;
		}

		/* 将其插入已解码记录队列。 */
		Assert(state->decode_queue_tail != decoded);
		if (state->decode_queue_tail)
			state->decode_queue_tail->next = decoded;
		state->decode_queue_tail = decoded;
		if (!state->decode_queue_head)
			state->decode_queue_head = decoded;
		return XLREAD_SUCCESS;
	}

err:
	if (assembled)
	{
		/*
		 * 到达此处表示需要装配跨多页的记录，但出了
		 * 问题——也许丢了一段 contrecord。若调用者是
		 * WAL 重放，它会知道中止记录的位置以及后续
		 * WAL 应写到何处，并用 XLP_FIRST_IS_OVERWRITE_CONTRECORD
		 * 标记下一段，从而向下游 WAL 消费者发出信号：
		 * 应忽略这条损坏的 WAL 记录。
		 *
		 */
		state->abortedRecPtr = RecPtr;
		state->missingContrecPtr = targetPagePtr;

		/*
		 * 若到达此处尚未报告错误，确保把错误排入队列，
		 * 以免 XLogPrefetcherReadRecord() 再次把我们带回来
		 * 并破坏上述状态。
		 */
		state->errormsg_deferred = true;
	}

	if (decoded && decoded->oversized)
		pfree(decoded);

	/*
	 * 使读状态失效。失败后可能从不同源读取。
	 *
	 */
	XLogReaderInvalReadState(state);

	/*
	 * 若错误已写入 errmsg_buf，则在读队列中所有成功
	 * 解码的记录之后，会返回给 XLogReadRecord()
	 * 的调用者。
	 */

	return XLREAD_FAIL;
}

/*
 * 尝试解码下一条可用记录并返回它。该记录也会
 * 返回给 XLogNextRecord()，后者必须被调用来"消费"
 * 每条记录。
 *
 * 若 nonblocking 为真，可能因缺少数据或 WAL 解码
 * 空间而返回 NULL。
 */
DecodedXLogRecord *
XLogReadAhead(XLogReaderState *state, bool nonblocking)
{
	XLogPageReadResult result;

	if (state->errormsg_deferred)
		return NULL;

	result = XLogDecodeNextRecord(state, nonblocking);
	if (result == XLREAD_SUCCESS)
	{
		Assert(state->decode_queue_tail != NULL);
		return state->decode_queue_tail;
	}

	return NULL;
}

/*
 * 通过 page_read() 回调读取单个 xlog 页，其中至少包含
 * [pageptr, reqLen] 的有效数据。
 *
 * 若因某种原因无法读取所需页，返回 XLREAD_FAIL；
 * 此时会设置 errormsg_buf（除非错误发生在
 * page_read 回调中）。
 *
 * 若在不等待的情况下无法读到所请求数据，返回
 * XLREAD_WOULDBLOCK。仅当已安装的 page_read 回调
 * 遵守 state->nonblocking 标志、且无法立即读取所请求
 * 数据时，才可能返回该值。
 *
 * 若已知已有所需数据，且自缓存以来未发生错误，
 * 则从读取器本地缓存取页。
 */
static int
ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr, int reqLen)
{
	int			readLen;
	uint32		targetPageOff;
	XLogSegNo	targetSegNo;
	XLogPageHeader hdr;

	Assert((pageptr % XLOG_BLCKSZ) == 0);

	XLByteToSeg(pageptr, targetSegNo, state->segcxt.ws_segsize);
	targetPageOff = XLogSegmentOffset(pageptr, state->segcxt.ws_segsize);

	/* 检查是否已有全部所请求数据 */
	if (targetSegNo == state->seg.ws_segno &&
		targetPageOff == state->segoff && reqLen <= state->readLen)
		return state->readLen;

	/*
	 * 在尝试读取前使内部缓冲区内容失效。只把长度
	 * 置为 0，而不是完整调用 XLogReaderInvalReadState()，
	 * 以免忘记上次成功读取的段。
	 */
	state->readLen = 0;

	/*
	 * 数据不在我们的缓冲区中。
	 *
	 * 每次真正读取该段时，即使以前看过其中部分，
	 * 也需要做校验，因为 page_read 回调现在可能
	 * 正从不同源重新读取数据。
	 *
	 * 每当切换到新的 WAL 段时，即使目标记录不在
	 * 那里，我们也读取文件的第一页并校验其头。
	 * 这样可以检查第一页"长"头中的额外标识信息。
	 *
	 */
	if (targetSegNo != state->seg.ws_segno && targetPageOff != 0)
	{
		XLogRecPtr	targetSegmentPtr = pageptr - targetPageOff;

		readLen = state->routine.page_read(state, targetSegmentPtr, XLOG_BLCKSZ,
										   state->currRecPtr,
										   state->readBuf);
		if (readLen == XLREAD_WOULDBLOCK)
			return XLREAD_WOULDBLOCK;
		else if (readLen < 0)
			goto err;

		/* 我们回退过，因此可以确信有足够的 WAL 可用 */
		Assert(readLen == XLOG_BLCKSZ);

		if (!XLogReaderValidatePageHeader(state, targetSegmentPtr,
										  state->readBuf))
			goto err;
	}

	/*
	 * 首先读取所请求的数据长度，但至少要有短页头，
	 * 以便校验。
	 */
	readLen = state->routine.page_read(state, pageptr, Max(reqLen, SizeOfXLogShortPHD),
									   state->currRecPtr,
									   state->readBuf);
	if (readLen == XLREAD_WOULDBLOCK)
		return XLREAD_WOULDBLOCK;
	else if (readLen < 0)
		goto err;

	Assert(readLen <= XLOG_BLCKSZ);

	/* 是否有足够数据来检查头长度？ */
	if (readLen <= SizeOfXLogShortPHD)
		goto err;

	Assert(readLen >= reqLen);

	hdr = (XLogPageHeader) state->readBuf;

	/* 仍然不够 */
	if (readLen < XLogPageHeaderSize(hdr))
	{
		readLen = state->routine.page_read(state, pageptr, XLogPageHeaderSize(hdr),
										   state->currRecPtr,
										   state->readBuf);
		if (readLen == XLREAD_WOULDBLOCK)
			return XLREAD_WOULDBLOCK;
		else if (readLen < 0)
			goto err;
	}

	/*
	 * 既然已知有完整头部，就校验它。
	 */
	if (!XLogReaderValidatePageHeader(state, pageptr, (char *) hdr))
		goto err;

	/* 更新读状态信息 */
	state->seg.ws_segno = targetSegNo;
	state->segoff = targetPageOff;
	state->readLen = readLen;

	return readLen;

err:
	XLogReaderInvalReadState(state);

	return XLREAD_FAIL;
}

/*
 * 使 xlogreader 的读状态失效以强制重新读取。
 */
static void
XLogReaderInvalReadState(XLogReaderState *state)
{
	state->seg.ws_segno = 0;
	state->segoff = 0;
	state->readLen = 0;
}

/*
 * 校验 XLOG 记录头。
 *
 * 这只是为避免在 XLogReadRecord 中重复代码的便利子程序。
 * 不打算在其他任何地方使用。
 */
static bool
ValidXLogRecordHeader(XLogReaderState *state, XLogRecPtr RecPtr,
					  XLogRecPtr PrevRecPtr, XLogRecord *record,
					  bool randAccess)
{
	if (record->xl_tot_len < SizeOfXLogRecord)
	{
		report_invalid_record(state,
							  "invalid record length at %X/%X: expected at least %u, got %u",
							  LSN_FORMAT_ARGS(RecPtr),
							  (uint32) SizeOfXLogRecord, record->xl_tot_len);
		return false;
	}
	if (!RmgrIdIsValid(record->xl_rmid))
	{
		report_invalid_record(state,
							  "invalid resource manager ID %u at %X/%X",
							  record->xl_rmid, LSN_FORMAT_ARGS(RecPtr));
		return false;
	}
	if (randAccess)
	{
		/*
		 * 我们无法精确校验 prev 链接，但它肯定应小于
		 * 记录自身的地址。
		 */
		if (!(record->xl_prev < RecPtr))
		{
			report_invalid_record(state,
								  "record with incorrect prev-link %X/%X at %X/%X",
								  LSN_FORMAT_ARGS(record->xl_prev),
								  LSN_FORMAT_ARGS(RecPtr));
			return false;
		}
	}
	else
	{
		/*
		 * 记录的 prev 链接应与我们的前一位置完全匹配。
		 * 此检查防范撕裂的 WAL 页：在扇区边界上出现
		 * 陈旧但看起来有效的 WAL 记录。
		 */
		if (record->xl_prev != PrevRecPtr)
		{
			report_invalid_record(state,
								  "record with incorrect prev-link %X/%X at %X/%X",
								  LSN_FORMAT_ARGS(record->xl_prev),
								  LSN_FORMAT_ARGS(RecPtr));
			return false;
		}
	}

	return true;
}


/*
 * 对 XLOG 记录做 CRC 检查。在检查 CRCs 之前，我们不
 * 信任 XLOG 记录的内容（除了为计算要读入的数据量
 * 所需的最小限度）。
 *
 * 假定整条记录（即 xl_tot_len 字节）已读入 *record
 * 所指内存。此外 ValidXLogRecordHeader() 已接受记录头，
 * 这特别意味着 xl_tot_len 至少为 SizeOfXLogRecord。
 *
 */
static bool
ValidXLogRecord(XLogReaderState *state, XLogRecord *record, XLogRecPtr recptr)
{
	pg_crc32c	crc;

	Assert(record->xl_tot_len >= SizeOfXLogRecord);

	/* 计算 CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, ((char *) record) + SizeOfXLogRecord, record->xl_tot_len - SizeOfXLogRecord);
	/* 最后把记录头也算进去 */
	COMP_CRC32C(crc, (char *) record, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(record->xl_crc, crc))
	{
		report_invalid_record(state,
							  "incorrect resource manager data checksum in record at %X/%X",
							  LSN_FORMAT_ARGS(recptr));
		return false;
	}

	return true;
}

/*
 * 校验页头。
 *
 * 检查 'phdr' 作为位置 'recptr' 处 XLog 页的头是否
 * 有效。
 */
bool
XLogReaderValidatePageHeader(XLogReaderState *state, XLogRecPtr recptr,
							 char *phdr)
{
	XLogSegNo	segno;
	int32		offset;
	XLogPageHeader hdr = (XLogPageHeader) phdr;

	Assert((recptr % XLOG_BLCKSZ) == 0);

	XLByteToSeg(recptr, segno, state->segcxt.ws_segsize);
	offset = XLogSegmentOffset(recptr, state->segcxt.ws_segsize);

	if (hdr->xlp_magic != XLOG_PAGE_MAGIC)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "invalid magic number %04X in WAL segment %s, LSN %X/%X, offset %u",
							  hdr->xlp_magic,
							  fname,
							  LSN_FORMAT_ARGS(recptr),
							  offset);
		return false;
	}

	if ((hdr->xlp_info & ~XLP_ALL_FLAGS) != 0)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "invalid info bits %04X in WAL segment %s, LSN %X/%X, offset %u",
							  hdr->xlp_info,
							  fname,
							  LSN_FORMAT_ARGS(recptr),
							  offset);
		return false;
	}

	if (hdr->xlp_info & XLP_LONG_HEADER)
	{
		XLogLongPageHeader longhdr = (XLogLongPageHeader) hdr;

		if (state->system_identifier &&
			longhdr->xlp_sysid != state->system_identifier)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: WAL file database system identifier is %llu, pg_control database system identifier is %llu",
								  (unsigned long long) longhdr->xlp_sysid,
								  (unsigned long long) state->system_identifier);
			return false;
		}
		else if (longhdr->xlp_seg_size != state->segcxt.ws_segsize)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: incorrect segment size in page header");
			return false;
		}
		else if (longhdr->xlp_xlog_blcksz != XLOG_BLCKSZ)
		{
			report_invalid_record(state,
								  "WAL file is from different database system: incorrect XLOG_BLCKSZ in page header");
			return false;
		}
	}
	else if (offset == 0)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		/* 嗯，文件第一页没有长头？ */
		report_invalid_record(state,
							  "invalid info bits %04X in WAL segment %s, LSN %X/%X, offset %u",
							  hdr->xlp_info,
							  fname,
							  LSN_FORMAT_ARGS(recptr),
							  offset);
		return false;
	}

	/*
	 * 检查页上的地址是否与我们的预期一致。当旧的
	 * WAL 段被回收、且尚未用新数据覆盖时，此检查
	 * 通常会失败。
	 */
	if (hdr->xlp_pageaddr != recptr)
	{
		char		fname[MAXFNAMELEN];

		XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

		report_invalid_record(state,
							  "unexpected pageaddr %X/%X in WAL segment %s, LSN %X/%X, offset %u",
							  LSN_FORMAT_ARGS(hdr->xlp_pageaddr),
							  fname,
							  LSN_FORMAT_ARGS(recptr),
							  offset);
		return false;
	}

	/*
	 * 由于子时间线总是被赋予大于其直接父时间线
	 * TLI 的 TLI，在一致的 WAL 序列的相继页上，我们
	 * 不应看到 TLI 回退。
	 *
	 * 有时我们会重新读取已经（部分）读过的段。因此
	 * 只对晚于上次记住的 LSN 的页校验 TLIs。
	 *
	 */
	if (recptr > state->latestPagePtr)
	{
		if (hdr->xlp_tli < state->latestPageTLI)
		{
			char		fname[MAXFNAMELEN];

			XLogFileName(fname, state->seg.ws_tli, segno, state->segcxt.ws_segsize);

			report_invalid_record(state,
								  "out-of-sequence timeline ID %u (after %u) in WAL segment %s, LSN %X/%X, offset %u",
								  hdr->xlp_tli,
								  state->latestPageTLI,
								  fname,
								  LSN_FORMAT_ARGS(recptr),
								  offset);
			return false;
		}
	}
	state->latestPagePtr = recptr;
	state->latestPageTLI = hdr->xlp_tli;

	return true;
}

/*
 * 忘掉 XLogReaderValidatePageHeader() 产生的错误。
 */
void
XLogReaderResetError(XLogReaderState *state)
{
	state->errormsg_buf[0] = '\0';
	state->errormsg_deferred = false;
}

/*
 * 查找第一条 lsn >= RecPtr 的记录。
 *
 * 这与 XLogBeginRead() 不同：RecPtr 不必指向有效
 * 记录边界。便于检查 RecPtr 是否为可读的有效
 * xlog 地址，以及在为调试转储记录时查找某地址
 * 之后的第一个有效地址。
 *
 * 这像 XLogBeginRead() 一样定位读取器，使下次调用
 * XLogReadRecord() 时会读到下一条有效记录。
 */
XLogRecPtr
XLogFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr)
{
	XLogRecPtr	tmpRecPtr;
	XLogRecPtr	found = InvalidXLogRecPtr;
	XLogPageHeader header;
	char	   *errormsg;

	Assert(!XLogRecPtrIsInvalid(RecPtr));

	/* 确保 ReadPageInternal() 不会返回 XLREAD_WOULDBLOCK。 */
	state->nonblocking = false;

	/*
	 * 跳过潜在的续写数据，记住它可能跨越
	 * 多页
	 */
	tmpRecPtr = RecPtr;
	while (true)
	{
		XLogRecPtr	targetPagePtr;
		int			targetRecOff;
		uint32		pageHeaderSize;
		int			readLen;

		/*
		 * 计算 targetRecOff。它通常应大于等于短页头，
		 * 因为有效记录不能从更早处开始，除非调用者
		 * 显式指定了落在其间某处的偏移，或我们正在
		 * 跳过多页续写记录。不过这无关紧要，因为
		 * ReadPageInternal() 已准备好处理该情况，并会
		 * 至少读取短页头长度的数据
		 *
		 */
		targetRecOff = tmpRecPtr % XLOG_BLCKSZ;

		/* 回退到页边界 */
		targetPagePtr = tmpRecPtr - targetRecOff;

		/* 读取包含该记录的页 */
		readLen = ReadPageInternal(state, targetPagePtr, targetRecOff);
		if (readLen < 0)
			goto err;

		header = (XLogPageHeader) state->readBuf;

		pageHeaderSize = XLogPageHeaderSize(header);

		/* 确保有足够数据用于页头 */
		readLen = ReadPageInternal(state, targetPagePtr, pageHeaderSize);
		if (readLen < 0)
			goto err;

		/* 跳过潜在的续写数据 */
		if (header->xlp_info & XLP_FIRST_IS_CONTRECORD)
		{
			/*
			 * 若剩余续写数据的长度超过本页可容纳的量，
			 * 则续写记录跨过本页。读取下一页重试。
			 * 下一页头中的 xlp_rem_len 将包含续写数据
			 * 的剩余长度
			 *
			 * 注意记录头是 MAXALIGN 对齐的
			 *
			 */
			if (MAXALIGN(header->xlp_rem_len) >= (XLOG_BLCKSZ - pageHeaderSize))
				tmpRecPtr = targetPagePtr + XLOG_BLCKSZ;
			else
			{
				/*
				 * 前一条续写记录在本页结束。把
				 * tmpRecPtr 指向第一条有效记录
				 */
				tmpRecPtr = targetPagePtr + pageHeaderSize
					+ MAXALIGN(header->xlp_rem_len);
				break;
			}
		}
		else
		{
			tmpRecPtr = targetPagePtr + pageHeaderSize;
			break;
		}
	}

	/*
	 * 现在我们知道 tmpRecPtr 指向一条有效的 XLogRecord，
	 * 因为我们要么在页起点之后的第一条记录处，
	 * 要么刚跳过了续写的剩余数据。
	 */
	XLogBeginRead(state, tmpRecPtr);
	while (XLogReadRecord(state, &errormsg) != NULL)
	{
		/* 已超过找到的记录，跳出 */
		if (RecPtr <= state->ReadRecPtr)
		{
			/* 把读取器回退到最后一条记录的起点。 */
			found = state->ReadRecPtr;
			XLogBeginRead(state, found);
			return found;
		}
	}

err:
	XLogReaderInvalReadState(state);

	return InvalidXLogRecPtr;
}

/*
 * 便于编写 XLogReaderRoutine->page_read 回调的辅助函数。
 * 若使用本函数，调用者必须在 'state' 中提供
 * segment_open 回调，因为这里会用到它。
 *
 * 从时间线 'tli' 取来的 WAL 中，从位置 'startptr' 起
 * 将 'count' 字节读入 'buf'。
 *
 * 成功返回 true；若发生错误则返回 false，此时
 * 'errinfo' 收到错误细节。
 *
 * XXX 或许应改进为在可能时直接从 WAL
 * 缓冲区吸取数据。
 */
bool
WALRead(XLogReaderState *state,
		char *buf, XLogRecPtr startptr, Size count, TimeLineID tli,
		WALReadError *errinfo)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;

	p = buf;
	recptr = startptr;
	nbytes = count;

	while (nbytes > 0)
	{
		uint32		startoff;
		int			segbytes;
		int			readbytes;

		startoff = XLogSegmentOffset(recptr, state->segcxt.ws_segsize);

		/*
		 * 若想要的数据不在已打开的段中，则关闭当前
		 * 已打开者（若有），并用调用者提供的
		 * segment_open 回调打开下一个。
		 */
		if (state->seg.ws_file < 0 ||
			!XLByteInSeg(recptr, state->seg.ws_segno, state->segcxt.ws_segsize) ||
			tli != state->seg.ws_tli)
		{
			XLogSegNo	nextSegNo;

			if (state->seg.ws_file >= 0)
				state->routine.segment_close(state);

			XLByteToSeg(recptr, nextSegNo, state->segcxt.ws_segsize);
			state->routine.segment_open(state, nextSegNo, &tli);

			/* 不应发生——表明 segment_open 有缺陷 */
			Assert(state->seg.ws_file >= 0);

			/* 更新当前段信息。 */
			state->seg.ws_tli = tli;
			state->seg.ws_segno = nextSegNo;
		}

		/* 本段内有多少字节？ */
		if (nbytes > (state->segcxt.ws_segsize - startoff))
			segbytes = state->segcxt.ws_segsize - startoff;
		else
			segbytes = nbytes;

#ifndef FRONTEND
		pgstat_report_wait_start(WAIT_EVENT_WAL_READ);
#endif

		/* 先重置 errno；便于报告不影响 errno 的错误 */
		errno = 0;
		readbytes = pg_pread(state->seg.ws_file, p, segbytes, (off_t) startoff);

#ifndef FRONTEND
		pgstat_report_wait_end();
#endif

		if (readbytes <= 0)
		{
			errinfo->wre_errno = errno;
			errinfo->wre_req = segbytes;
			errinfo->wre_read = readbytes;
			errinfo->wre_off = startoff;
			errinfo->wre_seg = state->seg;
			return false;
		}

		/* 更新读状态 */
		recptr += readbytes;
		nbytes -= readbytes;
		p += readbytes;
	}

	return true;
}

/* ----------------------------------------
 * 用于解码记录中数据与块引用的函数。
 * ----------------------------------------
 */

/*
 * 若被要求移到新的读取位置，则重置状态、忘掉所有
 * 已解码记录的私有函数。
 */
static void
ResetDecoder(XLogReaderState *state)
{
	DecodedXLogRecord *r;

	/* 重置已解码记录队列，释放任何超大记录。 */
	while ((r = state->decode_queue_head) != NULL)
	{
		state->decode_queue_head = r->next;
		if (r->oversized)
			pfree(r);
	}
	state->decode_queue_tail = NULL;
	state->decode_queue_head = NULL;
	state->record = NULL;

	/* 把解码缓冲区重置为空。 */
	state->decode_buffer_tail = state->decode_buffer;
	state->decode_buffer_head = state->decode_buffer;

	/* 清除错误状态。 */
	state->errormsg_buf[0] = '\0';
	state->errormsg_deferred = false;
}

/*
 * 根据记录头中的 xl_tot_len，计算解码一条记录可能
 * 需要的最大填充量。这是解码一条记录所需的输出
 * 缓冲区空间量，尽管最终未必全部用完。
 *
 * 因缺乏更好信息，此计算偏悲观，假定了最大可能
 * 的块数。
 *
 */
size_t
DecodeXLogRecordRequiredSpace(size_t xl_tot_len)
{
	size_t		size = 0;

	/* 计入已解码记录结构体的固定大小部分。 */
	size += offsetof(DecodedXLogRecord, blocks[0]);
	/* 计入最大可能大小的柔性 blocks 数组。 */
	size += sizeof(DecodedBkpBlock) * (XLR_MAX_BLOCK_ID + 1);
	/* 计入所有原始的 main 与块数据。 */
	size += xl_tot_len;
	/* 可能在 main_data 之前插入填充。 */
	size += (MAXIMUM_ALIGNOF - 1);
	/* 可能在每个块的数据之前插入填充。 */
	size += (MAXIMUM_ALIGNOF - 1) * (XLR_MAX_BLOCK_ID + 1);
	/* 可能在末尾插入填充。 */
	size += (MAXIMUM_ALIGNOF - 1);

	return size;
}

/*
 * 解码一条记录。"decoded" 必须指向 MAXALIGNed
 * 内存区，且至少有 DecodeXLogRecordRequiredSpace(record)
 * 字节的空间。成功时，decoded->size 包含已解码记录
 * 实际占用的空间，可能更小。
 *
 * 只有 decoded->oversized 成员必须已初始化，且不会
 * 被修改。其他成员将按需初始化。
 *
 * 出错时，在 *errormsg 中返回可读错误消息，
 * 返回值为 false。
 */
bool
DecodeXLogRecord(XLogReaderState *state,
				 DecodedXLogRecord *decoded,
				 XLogRecord *record,
				 XLogRecPtr lsn,
				 char **errormsg)
{
	/*
	 * 从记录缓冲区再读 _size 字节，但先检查是否越界。
	 */
#define COPY_HEADER_FIELD(_dst, _size)			\
	do {										\
		if (remaining < _size)					\
			goto shortdata_err;					\
		memcpy(_dst, ptr, _size);				\
		ptr += _size;							\
		remaining -= _size;						\
	} while(0)

	char	   *ptr;
	char	   *out;
	uint32		remaining;
	uint32		datatotal;
	RelFileLocator *rlocator = NULL;
	uint8		block_id;

	decoded->header = *record;
	decoded->lsn = lsn;
	decoded->next = NULL;
	decoded->record_origin = InvalidRepOriginId;
	decoded->toplevel_xid = InvalidTransactionId;
	decoded->main_data = NULL;
	decoded->main_data_len = 0;
	decoded->max_block_id = -1;
	ptr = (char *) record;
	ptr += SizeOfXLogRecord;
	remaining = record->xl_tot_len - SizeOfXLogRecord;

	/* 解码头部 */
	datatotal = 0;
	while (remaining > datatotal)
	{
		COPY_HEADER_FIELD(&block_id, sizeof(uint8));

		if (block_id == XLR_BLOCK_ID_DATA_SHORT)
		{
			/* XLogRecordDataHeaderShort */
			uint8		main_data_len;

			COPY_HEADER_FIELD(&main_data_len, sizeof(uint8));

			decoded->main_data_len = main_data_len;
			datatotal += main_data_len;
			break;				/* 按约定，主数据片段
								 * 总是在最后 */
		}
		else if (block_id == XLR_BLOCK_ID_DATA_LONG)
		{
			/* XLogRecordDataHeaderLong */
			uint32		main_data_len;

			COPY_HEADER_FIELD(&main_data_len, sizeof(uint32));
			decoded->main_data_len = main_data_len;
			datatotal += main_data_len;
			break;				/* 按约定，主数据片段
								 * 总是在最后 */
		}
		else if (block_id == XLR_BLOCK_ID_ORIGIN)
		{
			COPY_HEADER_FIELD(&decoded->record_origin, sizeof(RepOriginId));
		}
		else if (block_id == XLR_BLOCK_ID_TOPLEVEL_XID)
		{
			COPY_HEADER_FIELD(&decoded->toplevel_xid, sizeof(TransactionId));
		}
		else if (block_id <= XLR_MAX_BLOCK_ID)
		{
			/* XLogRecordBlockHeader */
			DecodedBkpBlock *blk;
			uint8		fork_flags;

			/* 把其间任何块 ID 标记为未使用 */
			for (int i = decoded->max_block_id + 1; i < block_id; ++i)
				decoded->blocks[i].in_use = false;

			if (block_id <= decoded->max_block_id)
			{
				report_invalid_record(state,
									  "out-of-order block_id %u at %X/%X",
									  block_id,
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			decoded->max_block_id = block_id;

			blk = &decoded->blocks[block_id];
			blk->in_use = true;
			blk->apply_image = false;

			COPY_HEADER_FIELD(&fork_flags, sizeof(uint8));
			blk->forknum = fork_flags & BKPBLOCK_FORK_MASK;
			blk->flags = fork_flags;
			blk->has_image = ((fork_flags & BKPBLOCK_HAS_IMAGE) != 0);
			blk->has_data = ((fork_flags & BKPBLOCK_HAS_DATA) != 0);

			blk->prefetch_buffer = InvalidBuffer;

			COPY_HEADER_FIELD(&blk->data_len, sizeof(uint16));
			/* 交叉检查：HAS_DATA 标志置位当且仅当 data_length > 0 */
			if (blk->has_data && blk->data_len == 0)
			{
				report_invalid_record(state,
									  "BKPBLOCK_HAS_DATA set, but no data included at %X/%X",
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			if (!blk->has_data && blk->data_len != 0)
			{
				report_invalid_record(state,
									  "BKPBLOCK_HAS_DATA not set, but data length is %u at %X/%X",
									  (unsigned int) blk->data_len,
									  LSN_FORMAT_ARGS(state->ReadRecPtr));
				goto err;
			}
			datatotal += blk->data_len;

			if (blk->has_image)
			{
				COPY_HEADER_FIELD(&blk->bimg_len, sizeof(uint16));
				COPY_HEADER_FIELD(&blk->hole_offset, sizeof(uint16));
				COPY_HEADER_FIELD(&blk->bimg_info, sizeof(uint8));

				blk->apply_image = ((blk->bimg_info & BKPIMAGE_APPLY) != 0);

				if (BKPIMAGE_COMPRESSED(blk->bimg_info))
				{
					if (blk->bimg_info & BKPIMAGE_HAS_HOLE)
						COPY_HEADER_FIELD(&blk->hole_length, sizeof(uint16));
					else
						blk->hole_length = 0;
				}
				else
					blk->hole_length = BLCKSZ - blk->bimg_len;
				datatotal += blk->bimg_len;

				/*
				 * 交叉检查：若置了 HAS_HOLE 标志，则
				 * hole_offset > 0、hole_length > 0 且 bimg_len < BLCKSZ。
				 */
				if ((blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
					(blk->hole_offset == 0 ||
					 blk->hole_length == 0 ||
					 blk->bimg_len == BLCKSZ))
				{
					report_invalid_record(state,
										  "BKPIMAGE_HAS_HOLE set, but hole offset %u length %u block image length %u at %X/%X",
										  (unsigned int) blk->hole_offset,
										  (unsigned int) blk->hole_length,
										  (unsigned int) blk->bimg_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * 交叉检查：若未置 HAS_HOLE 标志，则
				 * hole_offset == 0 且 hole_length == 0。
				 */
				if (!(blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
					(blk->hole_offset != 0 || blk->hole_length != 0))
				{
					report_invalid_record(state,
										  "BKPIMAGE_HAS_HOLE not set, but hole offset %u length %u at %X/%X",
										  (unsigned int) blk->hole_offset,
										  (unsigned int) blk->hole_length,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * 交叉检查：若已压缩，则 bimg_len < BLCKSZ。
				 */
				if (BKPIMAGE_COMPRESSED(blk->bimg_info) &&
					blk->bimg_len == BLCKSZ)
				{
					report_invalid_record(state,
										  "BKPIMAGE_COMPRESSED set, but block image length %u at %X/%X",
										  (unsigned int) blk->bimg_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				/*
				 * 交叉检查：若既未置 HAS_HOLE 也非
				 * COMPRESSED()，则 bimg_len = BLCKSZ。
				 */
				if (!(blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
					!BKPIMAGE_COMPRESSED(blk->bimg_info) &&
					blk->bimg_len != BLCKSZ)
				{
					report_invalid_record(state,
										  "neither BKPIMAGE_HAS_HOLE nor BKPIMAGE_COMPRESSED set, but block image length is %u at %X/%X",
										  (unsigned int) blk->data_len,
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}
			}
			if (!(fork_flags & BKPBLOCK_SAME_REL))
			{
				COPY_HEADER_FIELD(&blk->rlocator, sizeof(RelFileLocator));
				rlocator = &blk->rlocator;
			}
			else
			{
				if (rlocator == NULL)
				{
					report_invalid_record(state,
										  "BKPBLOCK_SAME_REL set but no previous rel at %X/%X",
										  LSN_FORMAT_ARGS(state->ReadRecPtr));
					goto err;
				}

				blk->rlocator = *rlocator;
			}
			COPY_HEADER_FIELD(&blk->blkno, sizeof(BlockNumber));
		}
		else
		{
			report_invalid_record(state,
								  "invalid block_id %u at %X/%X",
								  block_id, LSN_FORMAT_ARGS(state->ReadRecPtr));
			goto err;
		}
	}

	if (remaining != datatotal)
		goto shortdata_err;

	/*
	 * 好，我们已解析片段头，并验证各片段载荷总长度
	 * 等于剩余数据量。把每个片段的数据复制到 blocks
	 * 数组之后的连续空间，并在数据片段前插入对齐
	 * 填充，以便 REDO 例程可将其强制转换为结构体指针。
	 *
	 */
	out = ((char *) decoded) +
		offsetof(DecodedXLogRecord, blocks) +
		sizeof(decoded->blocks[0]) * (decoded->max_block_id + 1);

	/* 先处理块数据 */
	for (block_id = 0; block_id <= decoded->max_block_id; block_id++)
	{
		DecodedBkpBlock *blk = &decoded->blocks[block_id];

		if (!blk->in_use)
			continue;

		Assert(blk->has_image || !blk->apply_image);

		if (blk->has_image)
		{
			/* 映像无需对齐 */
			blk->bkp_image = out;
			memcpy(out, ptr, blk->bimg_len);
			ptr += blk->bimg_len;
			out += blk->bimg_len;
		}
		if (blk->has_data)
		{
			out = (char *) MAXALIGN(out);
			blk->data = out;
			memcpy(blk->data, ptr, blk->data_len);
			ptr += blk->data_len;
			out += blk->data_len;
		}
	}

	/* 最后是主数据 */
	if (decoded->main_data_len > 0)
	{
		out = (char *) MAXALIGN(out);
		decoded->main_data = out;
		memcpy(decoded->main_data, ptr, decoded->main_data_len);
		ptr += decoded->main_data_len;
		out += decoded->main_data_len;
	}

	/* 报告我们实际使用的大小。 */
	decoded->size = MAXALIGN(out - (char *) decoded);
	Assert(DecodeXLogRecordRequiredSpace(record->xl_tot_len) >=
		   decoded->size);

	return true;

shortdata_err:
	report_invalid_record(state,
						  "record with invalid length at %X/%X",
						  LSN_FORMAT_ARGS(state->ReadRecPtr));
err:
	*errormsg = state->errormsg_buf;

	return false;
}

/*
 * 返回块引用所指块的信息。
 *
 * 类似 XLogRecGetBlockTagExtended，但块引用必须存在，
 * 且无法访问 prefetch_buffer。
 */
void
XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
				   RelFileLocator *rlocator, ForkNumber *forknum,
				   BlockNumber *blknum)
{
	if (!XLogRecGetBlockTagExtended(record, block_id, rlocator, forknum,
									blknum, NULL))
	{
#ifndef FRONTEND
		elog(ERROR, "could not locate backup block with ID %d in WAL record",
			 block_id);
#else
		pg_fatal("could not locate backup block with ID %d in WAL record",
				 block_id);
#endif
	}
}

/*
 * 返回块引用所指块的信息，可选地包括该块可能
 * 已在其中的缓冲区。
 *
 * 若 WAL 记录包含给定 ID 的块引用，则填入 *rlocator、
 * *forknum、*blknum 与 *prefetch_buffer（若非 NULL），并
 * 返回 true。否则返回 false。
 */
bool
XLogRecGetBlockTagExtended(XLogReaderState *record, uint8 block_id,
						   RelFileLocator *rlocator, ForkNumber *forknum,
						   BlockNumber *blknum,
						   Buffer *prefetch_buffer)
{
	DecodedBkpBlock *bkpb;

	if (!XLogRecHasBlockRef(record, block_id))
		return false;

	bkpb = &record->record->blocks[block_id];
	if (rlocator)
		*rlocator = bkpb->rlocator;
	if (forknum)
		*forknum = bkpb->forknum;
	if (blknum)
		*blknum = bkpb->blkno;
	if (prefetch_buffer)
		*prefetch_buffer = bkpb->prefetch_buffer;
	return true;
}

/*
 * 返回与块引用关联的数据；若无数据则返回 NULL
 * （例如因为改为拍摄了整页映像）。返回的
 * 指针指向 MAXALIGNed 缓冲区。
 */
char *
XLogRecGetBlockData(XLogReaderState *record, uint8 block_id, Size *len)
{
	DecodedBkpBlock *bkpb;

	if (block_id > record->record->max_block_id ||
		!record->record->blocks[block_id].in_use)
		return NULL;

	bkpb = &record->record->blocks[block_id];

	if (!bkpb->has_data)
	{
		if (len)
			*len = 0;
		return NULL;
	}
	else
	{
		if (len)
			*len = bkpb->data_len;
		return bkpb->data;
	}
}

/*
 * 从附于 XLOG 记录的备份块恢复整页映像。
 *
 * 若恢复了整页映像则返回 true；失败时返回 false，
 * 并留下供调用者消费的错误。
 */
bool
RestoreBlockImage(XLogReaderState *record, uint8 block_id, char *page)
{
	DecodedBkpBlock *bkpb;
	char	   *ptr;
	PGAlignedBlock tmp;

	if (block_id > record->record->max_block_id ||
		!record->record->blocks[block_id].in_use)
	{
		report_invalid_record(record,
							  "could not restore image at %X/%X with invalid block %d specified",
							  LSN_FORMAT_ARGS(record->ReadRecPtr),
							  block_id);
		return false;
	}
	if (!record->record->blocks[block_id].has_image)
	{
		report_invalid_record(record, "could not restore image at %X/%X with invalid state, block %d",
							  LSN_FORMAT_ARGS(record->ReadRecPtr),
							  block_id);
		return false;
	}

	bkpb = &record->record->blocks[block_id];
	ptr = bkpb->bkp_image;

	if (BKPIMAGE_COMPRESSED(bkpb->bimg_info))
	{
		/* 若备份块映像已压缩，则解压它 */
		bool		decomp_success = true;

		if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_PGLZ) != 0)
		{
			if (pglz_decompress(ptr, bkpb->bimg_len, tmp.data,
								BLCKSZ - bkpb->hole_length, true) < 0)
				decomp_success = false;
		}
		else if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_LZ4) != 0)
		{
#ifdef USE_LZ4
			if (LZ4_decompress_safe(ptr, tmp.data,
									bkpb->bimg_len, BLCKSZ - bkpb->hole_length) <= 0)
				decomp_success = false;
#else
			report_invalid_record(record, "could not restore image at %X/%X compressed with %s not supported by build, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  "LZ4",
								  block_id);
			return false;
#endif
		}
		else if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_ZSTD) != 0)
		{
#ifdef USE_ZSTD
			size_t		decomp_result = ZSTD_decompress(tmp.data,
														BLCKSZ - bkpb->hole_length,
														ptr, bkpb->bimg_len);

			if (ZSTD_isError(decomp_result))
				decomp_success = false;
#else
			report_invalid_record(record, "could not restore image at %X/%X compressed with %s not supported by build, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  "zstd",
								  block_id);
			return false;
#endif
		}
		else
		{
			report_invalid_record(record, "could not restore image at %X/%X compressed with unknown method, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  block_id);
			return false;
		}

		if (!decomp_success)
		{
			report_invalid_record(record, "could not decompress image at %X/%X, block %d",
								  LSN_FORMAT_ARGS(record->ReadRecPtr),
								  block_id);
			return false;
		}

		ptr = tmp.data;
	}

	/* 生成页，必要时考虑空洞 */
	if (bkpb->hole_length == 0)
	{
		memcpy(page, ptr, BLCKSZ);
	}
	else
	{
		memcpy(page, ptr, bkpb->hole_offset);
		/* 必须用零填充空洞 */
		MemSet(page + bkpb->hole_offset, 0, bkpb->hole_length);
		memcpy(page + (bkpb->hole_offset + bkpb->hole_length),
			   ptr + bkpb->hole_offset,
			   BLCKSZ - (bkpb->hole_offset + bkpb->hole_length));
	}

	return true;
}

#ifndef FRONTEND

/*
 * 从 WAL 记录中提取 FullTransactionId。
 */
FullTransactionId
XLogRecGetFullXid(XLogReaderState *record)
{
	TransactionId xid,
				next_xid;
	uint32		epoch;

	/*
	 * 本函数仅在重放期间安全，因为它依赖
	 * 重放状态。更多信息见 AdvanceNextFullTransactionIdPastXid()。
	 */
	Assert(AmStartupProcess() || !IsUnderPostmaster);

	xid = XLogRecGetXid(record);
	next_xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	epoch = EpochFromFullTransactionId(ShmemVariableCache->nextXid);

	/*
	 * 若 xid 在数值上大于 next_xid，则它必然来自上一
	 * 纪元。
	 */
	if (unlikely(xid > next_xid))
		--epoch;

	return FullTransactionIdFromEpochAndXid(epoch, xid);
}

#endif
