/*-------------------------------------------------------------------------
 *
 * xlogreader.h
 *	通用 XLog 读取设施的定义
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/access/xlogreader.h
 *
 * NOTES
 *	XLogReader 基础设施的用法见 
 *	XLogReaderState 结构体定义。
 *
 *	基本思路是经 
 *	XLogReaderAllocate() 分配 XLogReaderState，定位首条，用 
 *	XLogBeginRead() 或 XLogFindNextRecord() 并调用 XLogReadRecord()
 *	 直至返回 NULL。
 *
 *	若要调用 XLogReadRecord 或 
 *	XLogFindNextRecord，调用方须提供 page_read 回调；
 *	否则可传入 NULL。WALRead 函数可用来辅助编写 
 *	page_read 回调，但并非必须；使用它的调用方
 *	必须提供 segment_open 回调。segment_close 
 *	必须始终提供。
 *
 *	用 XLogReadRecord() 读到记录后，会分解为
 *	按块部分与主数据部分，可通过 
 *	XLogRec* 宏与函数访问。也可解码已在
 *	内存中构造好的记录，无需读盘，调用 
 *	DecodeXLogRecord() 即可。
 *-------------------------------------------------------------------------
 */
#ifndef XLOGREADER_H
#define XLOGREADER_H

#ifndef FRONTEND
#include "access/transam.h"
#endif

#include "access/xlogrecord.h"
#include "storage/buf.h"

/* WALOpenSegment 表示正在读取的 WAL 段。 */
typedef struct WALOpenSegment
{
	int			ws_file;		/* 段文件描述符 */
	XLogSegNo	ws_segno;		/* 段号 */
	TimeLineID	ws_tli;			/* 当前打开文件的时间线 ID */
} WALOpenSegment;

/* WALSegmentContext 携带待读 WAL 段的上下文 */
typedef struct WALSegmentContext
{
	char		ws_dir[MAXPGPATH];
	int			ws_segsize;
} WALSegmentContext;

typedef struct XLogReaderState XLogReaderState;

/* xlogreader 各类交互的函数类型定义 */
typedef int (*XLogPageReadCB) (XLogReaderState *xlogreader,
							   XLogRecPtr targetPagePtr,
							   int reqLen,
							   XLogRecPtr targetRecPtr,
							   char *readBuf);
typedef void (*WALSegmentOpenCB) (XLogReaderState *xlogreader,
								  XLogSegNo nextSegNo,
								  TimeLineID *tli_p);
typedef void (*WALSegmentCloseCB) (XLogReaderState *xlogreader);

typedef struct XLogReaderRoutine
{
	/*
	 * 数据输入回调
	 *
	 * 本回调应读取自 targetPagePtr 起的 xlog 页上
	 * 至少 reqLen 个有效字节，存入 readBuf。回调
	 * 应返回已读字节数（不超过 XLOG_BLCKSZ），失败则
	 * 返回 -1。必要时回调应睡眠，以等待
	 * 所请求字节变为可用。同一页上，除非需要的
	 * 字节多于已返回数，否则不会再次
	 * 调用。
	 *
	 * targetRecPtr 是正在读取的 WAL 记录位置。通常
	 * 等于 targetPagePtr + reqLen，但有时 xlogreader 需
	 * 先读并校验页或段头，然后才读它真正
	 * 关心的 WAL 记录。此时可用 targetRecPtr 
	 * 判定应从哪条时间线读该页。
	 *
	 * 回调应将 ->seg.ws_tli 设为读到该页的文件的 
	 * TLI。
	 */
	XLogPageReadCB page_read;

	/*
	 * 打开指定 WAL 段以供读取的回调。->seg.ws_file 
	 * 应设为已打开段的文件描述符。若失败，
	 * 回调应报错并且不得返回。
	 * return.
	 *
	 * "nextSegNo" 是要打开的段号。
	 *
	 * "tli_p" 为输入/输出参数。WALRead() 用它传入
	 * 应查找新段的时间线，但回调也可用它
	 * 返回实际打开的 TLI。
	 */
	WALSegmentOpenCB segment_open;

	/*
	 * WAL 段关闭回调。->seg.ws_file 应设为负
	 * 数。
	 */
	WALSegmentCloseCB segment_close;
} XLogReaderRoutine;

#define XL_ROUTINE(...) &(XLogReaderRoutine){__VA_ARGS__}

typedef struct
{
	/* 此块引用在用？ */
	bool		in_use;

	/* 标识本引用所指的块 */
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;

	/* 预取工作区。 */
	Buffer		prefetch_buffer;

	/* XLogRecordBlockHeader 中 fork_flags 字段的副本 */
	uint8		flags;

	/* 整页映像信息（若有） */
	bool		has_image;		/* 有映像，即使仅作一致性检查 */
	bool		apply_image;	/* 有应被还原的映像 */
	char	   *bkp_image;
	uint16		hole_offset;
	uint16		hole_length;
	uint16		bimg_len;
	uint8		bimg_info;

	/* 保存与本块相关的 rmgr 专用数据的缓冲区 */
	bool		has_data;
	char	   *data;
	uint16		data_len;
	uint16		data_bufsz;
} DecodedBkpBlock;

/*
 * 记录的已解码内容。占一段连续
 * 内存，main_data 与 blocks[n].data 指向此处声明
 * 成员后的内存。
 */
typedef struct DecodedXLogRecord
{
	/* 用于资源管理的私有成员。 */
	size_t		size;			/* 已解码记录总大小 */
	bool		oversized;		/* 在常规解码缓冲区之外？ */
	struct DecodedXLogRecord *next; /* 解码记录队列链接 */

	/* 公有成员。 */
	XLogRecPtr	lsn;			/* 位置 */
	XLogRecPtr	next_lsn;		/* 下条记录的位置 */
	XLogRecord	header;			/* 报头 */
	RepOriginId record_origin;
	TransactionId toplevel_xid; /* 顶层事务的 XID */
	char	   *main_data;		/* 记录的主数据部分 */
	uint32		main_data_len;	/* 主数据部分的长度 */
	int			max_block_id;	/* 在用最大 block_id(-1 即无) */
	DecodedBkpBlock blocks[FLEXIBLE_ARRAY_MEMBER];
} DecodedXLogRecord;

struct XLogReaderState
{
	/*
	 * 操作回调
	 */
	XLogReaderRoutine routine;

	/* ----------------------------------------
	 * 公有参数
	 * ----------------------------------------
	 */

	/*
	 * 即将读取的 xlog 文件的系统标识符。未知或不重要时
	 * 设为零（默认值）。
	 */
	uint64		system_identifier;

	/*
	 * 供回调使用的不透明数据。XLogReader 不用。
	 */
	void	   *private_data;

	/*
	 * 上次所读记录的起止点。EndRecPtr 也用作
	 * 下次读取位置。调用 XLogBeginRead() 会把 EndRecPtr 
	 * 设为起点，ReadRecPtr 设为无效。
	 *
	 * XLogReadRecord() 上次返回记录的起止点。它们
	 * 也可经 record->lsn 与 record->next_lsn 取得。
	 */
	XLogRecPtr	ReadRecPtr;		/* 上次所读记录起点 */
	XLogRecPtr	EndRecPtr;		/* 上次所读记录 end+1 */

	/*
	 * 恢复结束时设置：WAL 末尾那条不完整记录的
	 * 起点（若无则为 InvalidXLogRecPtr），以及其
	 * 第一条丢失的 contrecord 的起始位置。
	 */
	XLogRecPtr	abortedRecPtr;
	XLogRecPtr	missingContrecPtr;
	/* 发现 XLP_FIRST_IS_OVERWRITE_CONTRECORD 时设置 */
	XLogRecPtr	overwrittenRecPtr;


	/* ----------------------------------------
	 * 当前记录的已解码表示
	 *
	 * 用 XLogRecGet* 函数探查记录；这些字段
	 * 不应直接访问。
	 * ----------------------------------------
	 * XLogReadRecordInternal() 上次读并解码的记录的
	 * 起止点。NextRecPtr 也用作下次
	 * 解码位置。调用 XLogBeginRead() 把 NextRecPtr 与 EndRecPtr 
	 * 设为所请求起点。
	 */
	XLogRecPtr	DecodeRecPtr;	/* 上次解码记录的起点 */
	XLogRecPtr	NextRecPtr;		/* 上次解码记录的 end+1 */
	XLogRecPtr	PrevRecPtr;		/* 先前解码记录的起点 */

	/* XLogReadRecord() 上次返回的记录。 */
	DecodedXLogRecord *record;

	/* ----------------------------------------
	 * 私有/内部状态
	 * ----------------------------------------
	 */

	/*
	 * 已解码记录的缓冲区。这是环形缓冲区，但
	 * 单条记录无法从中间切开，因此末尾常会
	 * 浪费一些空间。装不进此处的 oversized 记录
	 * 另行分配。
	 */
	char	   *decode_buffer;
	size_t		decode_buffer_size;
	bool		free_decode_buffer; /* 需释放？ */
	char	   *decode_buffer_head; /* 从头读取数据 */
	char	   *decode_buffer_tail; /* 新数据写到尾部 */

	/*
	 * 已解码记录的队列。这是链表，通常由 
	 * decode_buffer 中连续记录组成，但也可含
	 * 用 palloc() 分配的 oversized 记录。
	 */
	DecodedXLogRecord *decode_queue_head;	/* 最旧已解码记录 */
	DecodedXLogRecord *decode_queue_tail;	/* 最新已解码记录 */

	/*
	 * 当前已读页缓冲区（XLOG_BLCKSZ 字节，有效至少 
	 * readLen）
	 */
	char	   *readBuf;
	uint32		readLen;

	/* readBuf 中当前数据的上次读取 XLOG 位置 */
	WALSegmentContext segcxt;
	WALOpenSegment seg;
	uint32		segoff;

	/*
	 * 上次所读页的起点及其 TLI。不一定对应 
	 * readBuf 中的内容；用于时间线健全性检查。
	 */
	XLogRecPtr	latestPagePtr;
	TimeLineID	latestPageTLI;

	/* 正在读取的 WAL 记录的起点。 */
	XLogRecPtr	currRecPtr;
	/* 从该时间线读取，0 表示需要查找 */
	TimeLineID	currTLI;

	/*
	 * 当前 TLI 为历史时，currTLI 内可读到的安全点即
	 *  tliSwitchPoint；当前时间线则为 InvalidXLogRecPtr。
	 *
	 * 实际设为含结束 currTLI 有效期的时间线切换的
	 * 那段起点，而非切换本身的 LSN，因为
	 * 我们无法假定旧段仍存在。
	 */
	XLogRecPtr	currTLIValidUntil;

	/*
	 * 若 currTLI 不是最近已知时间线，到达 
	 * currTLIValidUntil 后改读的时间线。
	 */
	TimeLineID	nextTLI;

	/*
	 * 当前 ReadRecord 结果的缓冲区（可扩展），用于记录
	 * 跨越页边界时。
	 */
	char	   *readRecordBuf;
	uint32		readRecordBufSize;

	/* 错误消息缓冲区 */
	char	   *errormsg_buf;
	bool		errormsg_deferred;

	/*
	 * 告知 XLogPageReadCB 等待数据时不应阻塞。
	 * data.
	 */
	bool		nonblocking;
};

/*
 * 检查 XLogNextRecord() 是否还有排队记录或错误可返回。
 */
static inline bool
XLogReaderHasQueuedRecordOrError(XLogReaderState *state)
{
	return (state->decode_queue_head != NULL) || state->errormsg_deferred;
}

/* 获取新 XLogReader */
extern XLogReaderState *XLogReaderAllocate(int wal_segment_size,
										   const char *waldir,
										   XLogReaderRoutine *routine,
										   void *private_data);

/* 释放 XLogReader */
extern void XLogReaderFree(XLogReaderState *state);

/* 可选提供环形解码缓冲区以允许预读。 */
extern void XLogReaderSetDecodeBuffer(XLogReaderState *state,
									  void *buffer,
									  size_t size);

/* 把 XLogReader 定位到给定记录 */
extern void XLogBeginRead(XLogReaderState *state, XLogRecPtr RecPtr);
extern XLogRecPtr XLogFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr);

/* XLogPageReadCB 的返回值。 */
typedef enum XLogPageReadResult
{
	XLREAD_SUCCESS = 0,			/* 成功读到记录 */
	XLREAD_FAIL = -1,			/* 读记录过程中失败 */
	XLREAD_WOULDBLOCK = -2		/* 仅非阻塞模式，无数据 */
} XLogPageReadResult;

/* 读下一条 XLog 记录。WAL 结束或失败时返回 NULL */
extern struct XLogRecord *XLogReadRecord(XLogReaderState *state,
										 char **errormsg);

/* 消费下一条记录或错误。 */
extern DecodedXLogRecord *XLogNextRecord(XLogReaderState *state,
										 char **errormsg);

/* 必要时释放先前返回的记录。 */
extern XLogRecPtr XLogReleasePreviousRecord(XLogReaderState *state);

/* 若有数据与空间则尝试预读。 */
extern DecodedXLogRecord *XLogReadAhead(XLogReaderState *state,
										bool nonblocking);

/* 校验一页 */
extern bool XLogReaderValidatePageHeader(XLogReaderState *state,
										 XLogRecPtr recptr, char *phdr);

/* 忘记 XLogReaderValidatePageHeader() 产生的错误。 */
extern void XLogReaderResetError(XLogReaderState *state);

/*
 * WALRead 给出的、后端与前端调用方都能处理的
 * 错误信息。目前仅能报告 pg_pread 的错误。
 */
typedef struct WALReadError
{
	int			wre_errno;		/* 上次 pg_pread() 的 errno */
	int			wre_off;		/* 试图读取的偏移。 */
	int			wre_req;		/* 请求读取的字节数。 */
	int			wre_read;		/* 上次 read() 所读字节数 */
	WALOpenSegment wre_seg;		/* 试图读取的段。 */
} WALReadError;

extern bool WALRead(XLogReaderState *state,
					char *buf, XLogRecPtr startptr, Size count,
					TimeLineID tli, WALReadError *errinfo);

/* 解码 XLogRecord 的函数 */

extern size_t DecodeXLogRecordRequiredSpace(size_t xl_tot_len);
extern bool DecodeXLogRecord(XLogReaderState *state,
							 DecodedXLogRecord *decoded,
							 XLogRecord *record,
							 XLogRecPtr lsn,
							 char **errormsg);

/*
 * 访问 XLogReadRecord() 或 XLogNextRecord() 最近返回
 * 的记录各部分的宏。
 */
#define XLogRecGetTotalLen(decoder) ((decoder)->record->header.xl_tot_len)
#define XLogRecGetPrev(decoder) ((decoder)->record->header.xl_prev)
#define XLogRecGetInfo(decoder) ((decoder)->record->header.xl_info)
#define XLogRecGetRmid(decoder) ((decoder)->record->header.xl_rmid)
#define XLogRecGetXid(decoder) ((decoder)->record->header.xl_xid)
#define XLogRecGetOrigin(decoder) ((decoder)->record->record_origin)
#define XLogRecGetTopXid(decoder) ((decoder)->record->toplevel_xid)
#define XLogRecGetData(decoder) ((decoder)->record->main_data)
#define XLogRecGetDataLen(decoder) ((decoder)->record->main_data_len)
#define XLogRecHasAnyBlockRefs(decoder) ((decoder)->record->max_block_id >= 0)
#define XLogRecMaxBlockId(decoder) ((decoder)->record->max_block_id)
#define XLogRecGetBlock(decoder, i) (&(decoder)->record->blocks[(i)])
#define XLogRecHasBlockRef(decoder, block_id)			\
	(((decoder)->record->max_block_id >= (block_id)) &&	\
	 ((decoder)->record->blocks[block_id].in_use))
#define XLogRecHasBlockImage(decoder, block_id)		\
	((decoder)->record->blocks[block_id].has_image)
#define XLogRecBlockImageApply(decoder, block_id)		\
	((decoder)->record->blocks[block_id].apply_image)
#define XLogRecHasBlockData(decoder, block_id)		\
	((decoder)->record->blocks[block_id].has_data)

#ifndef FRONTEND
extern FullTransactionId XLogRecGetFullXid(XLogReaderState *record);
#endif

extern bool RestoreBlockImage(XLogReaderState *record, uint8 block_id, char *page);
extern char *XLogRecGetBlockData(XLogReaderState *record, uint8 block_id, Size *len);
extern void XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
							   RelFileLocator *rlocator, ForkNumber *forknum,
							   BlockNumber *blknum);
extern bool XLogRecGetBlockTagExtended(XLogReaderState *record, uint8 block_id,
									   RelFileLocator *rlocator, ForkNumber *forknum,
									   BlockNumber *blknum,
									   Buffer *prefetch_buffer);

#endif							/* XLOGREADER_H */
