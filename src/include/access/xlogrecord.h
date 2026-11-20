/*
 * xlogrecord.h
 *
 * WAL 记录格式的定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecord.h
 */
#ifndef XLOGRECORD_H
#define XLOGRECORD_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "port/pg_crc32c.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * XLOG 记录的总体布局为：
 *		定长报头 (XLogRecord 结构体)
 *		XLogRecordBlockHeader struct
 *		XLogRecordBlockHeader struct
 *		...
 *		XLogRecordDataHeader[Short|Long] struct
 *		块数据
 *		块数据
 *		...
 *		主数据
 *
 * 可有零个或多个 XLogRecordBlockHeaders，以及 0 或更多字节
 * 的不与块关联的 rmgr 专有数据。XLogRecord 结构体
 * 在 WAL 文件中总是从 MAXALIGN 边界起，但
 * 其余字段则不对齐。
 *
 * XLogRecordBlockHeader、XLogRecordDataHeaderShort 与
 * XLogRecordDataHeaderLong 结构体均以单字节 'id' 开头。它
 * 用来区分块引用与主数据结构体。
 */
typedef struct XLogRecord
{
	uint32		xl_tot_len;		/* 整条记录的总长度 */
	TransactionId xl_xid;		/* xact id */
	XLogRecPtr	xl_prev;		/* 指向日志中前一记录 */
	uint8		xl_info;		/* 标志位，见下 */
	RmgrId		xl_rmid;		/* 本记录的资源管理器 */
	/* 此处 2 字节填充，初始化为零 */
	pg_crc32c	xl_crc;			/* 本记录的 CRC */

	/* 后跟 XLogRecordBlockHeaders 与 XLogRecordDataHeader，无填充 */

} XLogRecord;

#define SizeOfXLogRecord	(offsetof(XLogRecord, xl_crc) + sizeof(pg_crc32c))

/*
 * xl_info 的高 4 位可由 rmgr 自由使用。
 * XLR_SPECIAL_REL_UPDATE 与 XLR_CHECK_CONSISTENCY 位可由 XLogInsert
 * 调用方传入。其余由 XLogInsert 内部设置。
 */
#define XLR_INFO_MASK			0x0F
#define XLR_RMGR_INFO_MASK		0xF0

/*
 * XLogReader 需要把一条 WAL 记录的全部数据分配在单一
 * 块中。这意味着若忽略 XLogReader 的任何分配开销，
 * 单条 XLogRecord 长度无法超过 MaxAllocSize。
 *
 * 为容纳若干开销，该值允许 4M 的分配
 * 开销，对 DecodeXLogRecordRequiredSpace() 
 * 所期望的额外空间应绰绰有余。
 */
#define XLogRecordMaxSize	(1020 * 1024 * 1024)

/*
 * 若 WAL 记录以通常块引用未覆盖的方式修改任何
 * 关系文件，则置此标志。PostgreSQL 自身不用它，
 * 但允许读取 WAL 并跟踪被修改块的外部工具
 * 识别这类特殊记录类型。
 */
#define XLR_SPECIAL_REL_UPDATE	0x01

/*
 * 在恢复时强制对重放的 WAL 做一致性检查。若启用，
 * 每条记录会为其所修改的每个块记录整页映像，
 * 随后复用该映像做一致性检查。XLogInsert
 * 调用方必要时可用此值，但若
 * 某 rmgr 启用了 wal_consistency_checking 则无条件置位。
 */
#define XLR_CHECK_CONSISTENCY	0x02

/*
 * 追加到 XLOG 记录的块数据的报头信息。
 *
 * 'data_length' 是与此块关联的 rmgr 专有载荷数据的
 * 长度。它不含可能的整页映像，也不含 
 * XLogRecordBlockHeader 本身。
 *
 * 注意我们并不试图对齐 XLogRecordBlockHeader 结构体！
 * 故使用前必须复制到已对齐的局部存储。
 */
typedef struct XLogRecordBlockHeader
{
	uint8		id;				/* 块引用 ID */
	uint8		fork_flags;		/* 关系内的分叉，以及标志 */
	uint16		data_length;	/* 载荷字节数(不含页
								 * 映像)*/

	/* 若 BKPBLOCK_HAS_IMAGE，则后跟 XLogRecordBlockImageHeader */
	/* 若未置 BKPBLOCK_SAME_REL，则后跟 RelFileLocator */
	/* 后跟 BlockNumber */
} XLogRecordBlockHeader;

#define SizeOfXLogRecordBlockHeader (offsetof(XLogRecordBlockHeader, data_length) + sizeof(uint16))

/*
 * 包含整页映像时的附加报头信息
 * (即置了 BKPBLOCK_HAS_IMAGE 时)。
 *
 * XLOG 代码知道 PG 数据页中间通常有一段未用的 "空洞"，
 * 其中只有零字节。既然已知 "空洞" 全为零，我们就从
 * 存储数据中去掉它（也不计入 XLOG 记录的 CRC）。因此
 * 实际存在的块数据量为 
 * (BLCKSZ - <"空洞" 字节长度>)。
 *
 * 此外，启用 wal_compression 时，去掉 "空洞" 后我们会尝试
 * 用某种受支持算法压缩整页映像。这能减少 WAL 量，
 * 但 WAL 记录时要额外花费 CPU 做压缩。此时，由于 "空洞"
 * 长度无法用 BLCKSZ 减去页映像字节数算出，基本上
 * 需要作为额外信息存下来。但若不存在 "空洞"，可假定
 * "空洞" 长度为 0，也就不必存储该额外信息。注意
 * 若压缩节省的字节数少于
 * 额外信息的长度，则 WAL 中存放页映像
 * 原版而非压缩版。因此，当页映像
 * 成功压缩时，实际存在的块数据量
 * 小于 
 * BLCKSZ - "空洞" 字节长度 -
 * 额外信息长度。
 */
typedef struct XLogRecordBlockImageHeader
{
	uint16		length;			/* 页映像字节数 */
	uint16		hole_offset;	/* "空洞" 之前的字节数 */
	uint8		bimg_info;		/* 标志位，见下 */

	/*
	 * 若 BKPIMAGE_HAS_HOLE 且 BKPIMAGE_COMPRESSED()，
	 * 则后跟 XLogRecordBlockCompressHeader。
	 */
} XLogRecordBlockImageHeader;

#define SizeOfXLogRecordBlockImageHeader	\
	(offsetof(XLogRecordBlockImageHeader, bimg_info) + sizeof(uint8))

/* 存于 bimg_info 的信息 */
#define BKPIMAGE_HAS_HOLE		0x01	/* 页映像有 "空洞" */
#define BKPIMAGE_APPLY			0x02	/* 页映像应在重放时
										 * 恢复 */
/* 所支持的压缩方法 */
#define BKPIMAGE_COMPRESS_PGLZ	0x04
#define BKPIMAGE_COMPRESS_LZ4	0x08
#define BKPIMAGE_COMPRESS_ZSTD	0x10

#define	BKPIMAGE_COMPRESSED(info) \
	((info & (BKPIMAGE_COMPRESS_PGLZ | BKPIMAGE_COMPRESS_LZ4 | \
			  BKPIMAGE_COMPRESS_ZSTD)) != 0)

/*
 * 页映像有 "空洞" 且已压缩时用的额外
 * 报头。
 */
typedef struct XLogRecordBlockCompressHeader
{
	uint16		hole_length;	/* "空洞" 中的字节数 */
} XLogRecordBlockCompressHeader;

#define SizeOfXLogRecordBlockCompressHeader \
	sizeof(XLogRecordBlockCompressHeader)

/*
 * 块引用报头的最大大小。用来给构造报头的
 * 临时缓冲区定尺寸。
 */
#define MaxSizeOfXLogRecordBlockHeader \
	(SizeOfXLogRecordBlockHeader + \
	 SizeOfXLogRecordBlockImageHeader + \
	 SizeOfXLogRecordBlockCompressHeader + \
	 sizeof(RelFileLocator) + \
	 sizeof(BlockNumber))

/*
 * 分叉号放在 fork_flags 字段的低 4 位。高位
 * 用于标志。
 */
#define BKPBLOCK_FORK_MASK	0x0F
#define BKPBLOCK_FLAG_MASK	0xF0
#define BKPBLOCK_HAS_IMAGE	0x10	/* 块数据是 XLogRecordBlockImage */
#define BKPBLOCK_HAS_DATA	0x20
#define BKPBLOCK_WILL_INIT	0x40	/* 重放将重初始化页 */
#define BKPBLOCK_SAME_REL	0x80	/* 省略 RelFileLocator，同
									 * 前一条*/

/*
 * XLogRecordDataHeaderShort/Long 用于记录的 "主数据" 
 * 部分。数据长度小于 256 字节时用短形式，以单字节
 * 保存长度。否则用长
 * 形式。
 *
 * （这些结构体目前未在代码中使用，放在这里只是为了
 * 文档目的）。
 */
typedef struct XLogRecordDataHeaderShort
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_SHORT */
	uint8		data_length;	/* 载荷字节数 */
}			XLogRecordDataHeaderShort;

#define SizeOfXLogRecordDataHeaderShort (sizeof(uint8) * 2)

typedef struct XLogRecordDataHeaderLong
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_LONG */
	/* 后跟未对齐的 uint32 data_length */
}			XLogRecordDataHeaderLong;

#define SizeOfXLogRecordDataHeaderLong (sizeof(uint8) + sizeof(uint32))

/*
 * 用来区分不同种类记录片段的块 ID。块
 * 引用从 0 编号到 XLR_MAX_BLOCK_ID。rmgr 可自由使用
 * 该范围内任一 ID（不过应坚持用小编号，
 * 因为 WAL 机制对此情形做了优化）。少数 ID 
 * 号保留来表示记录的 "主" 数据部分，
 * 以及支持复制的事务元数据。
 *
 * 上限目前定为 32，颇为随意。多数记录只需
 * 少量块引用，但也有少数例外
 * 需更多
 */
#define XLR_MAX_BLOCK_ID			32

#define XLR_BLOCK_ID_DATA_SHORT		255
#define XLR_BLOCK_ID_DATA_LONG		254
#define XLR_BLOCK_ID_ORIGIN			253
#define XLR_BLOCK_ID_TOPLEVEL_XID	252

#endif							/* XLOGRECORD_H */
