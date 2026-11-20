/*
 * xlog_internal.h
 *
 * PostgreSQL 预写式日志的内部声明
 *
 * 注意：本文件收录直接操纵 XLOG 文件所需的
 * 声明，而不应被 rmgr 例程所需要（各记录类型的 
 * redo 支持）。因此 XLogRecord 类型定义及
 * 相关内容放在 xlogrecord.h。
 *
 * 注意：本文件必须可在前端与后端上下文中包含，
 * 以便 pg_receivewal 这类独立工具处理 WAL 文件。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog_internal.h
 */
#ifndef XLOG_INTERNAL_H
#define XLOG_INTERNAL_H

#include "access/xlogdefs.h"
#include "access/xlogreader.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "pgtime.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"


/*
 * XLOG 文件的每一页都有如下报头：
 */
#define XLOG_PAGE_MAGIC 0xD113	/* 可用作 WAL 版本指示 */

typedef struct XLogPageHeaderData
{
	uint16		xlp_magic;		/* 供正确性检查的魔数 */
	uint16		xlp_info;		/* 标志位，见下 */
	TimeLineID	xlp_tli;		/* 页上首条记录的 TimeLineID */
	XLogRecPtr	xlp_pageaddr;	/* 本页的 XLOG 地址 */

	/*
	 * 当前页空间不够放下整条记录时，我们
	 * 在下一页继续。xlp_rem_len 是上一页剩余的
	 * 字节数；它跟踪初始报头中的 xl_tot_len。
	 * 注意续接数据未必对齐。
	 */
	uint32		xlp_rem_len;	/* 记录剩余数据的总长度 */
} XLogPageHeaderData;

#define SizeOfXLogShortPHD	MAXALIGN(sizeof(XLogPageHeaderData))

typedef XLogPageHeaderData *XLogPageHeader;

/*
 * 置了 XLP_LONG_HEADER 标志时，我们在页报头中存放
 * 附加字段。（通常只在 XLOG 文件的第一页
 * 这样做。）附加字段用于准确标识该文件。
 */
typedef struct XLogLongPageHeaderData
{
	XLogPageHeaderData std;		/* 标准报头字段 */
	uint64		xlp_sysid;		/* pg_control 中的系统标识符 */
	uint32		xlp_seg_size;	/* 仅作交叉校验 */
	uint32		xlp_xlog_blcksz;	/* 仅作交叉校验 */
} XLogLongPageHeaderData;

#define SizeOfXLogLongPHD	MAXALIGN(sizeof(XLogLongPageHeaderData))

typedef XLogLongPageHeaderData *XLogLongPageHeader;

/* 记录跨页时，在新页报头置此标志 */
#define XLP_FIRST_IS_CONTRECORD		0x0001
/* 此标志表示 "长" 页报头 */
#define XLP_LONG_HEADER				0x0002
/* 此标志表示本页起始的备份块是可选的 */
#define XLP_BKP_REMOVABLE			0x0004
/* 替换缺失的 contrecord；见 CreateOverwriteContrecordRecord */
#define XLP_FIRST_IS_OVERWRITE_CONTRECORD 0x0008
/* xlp_info 中所有已定义标志位（用于报头有效性检查） */
#define XLP_ALL_FLAGS				0x000F

#define XLogPageHeaderSize(hdr)		\
	(((hdr)->xlp_info & XLP_LONG_HEADER) ? SizeOfXLogLongPHD : SizeOfXLogShortPHD)

/* wal_segment_size 可为 1MB 到 1GB */
#define WalSegMinSize 1024 * 1024
#define WalSegMaxSize 1024 * 1024 * 1024
/* 最小与最大 wal 段的默认个数 */
#define DEFAULT_MIN_WAL_SEGS 5
#define DEFAULT_MAX_WAL_SEGS 64

/* 检查给定大小是否为合法 wal_segment_size */
#define IsPowerOf2(x) (x > 0 && ((x) & ((x)-1)) == 0)
#define IsValidWalSegSize(size) \
	 (IsPowerOf2(size) && \
	 ((size) >= WalSegMinSize && (size) <= WalSegMaxSize))

#define XLogSegmentsPerXLogId(wal_segsz_bytes)	\
	(UINT64CONST(0x100000000) / (wal_segsz_bytes))

#define XLogSegNoOffsetToRecPtr(segno, offset, wal_segsz_bytes, dest) \
		(dest) = (segno) * (wal_segsz_bytes) + (offset)

#define XLogSegmentOffset(xlogptr, wal_segsz_bytes)	\
	((xlogptr) & ((wal_segsz_bytes) - 1))

/*
 * 从 XLogRecPtr 计算段号。
 *
 * 对 XLByteToSeg，按字面计算。对 XLByteToPrevSeg，
 * 边界字节算作属于前一段。这适合
 * 在给定记录结束指针时决定写入哪一段，
 * 例如。
 */
#define XLByteToSeg(xlrp, logSegNo, wal_segsz_bytes) \
	logSegNo = (xlrp) / (wal_segsz_bytes)

#define XLByteToPrevSeg(xlrp, logSegNo, wal_segsz_bytes) \
	logSegNo = ((xlrp) - 1) / (wal_segsz_bytes)

/*
 * 把以兆字节计的 GUCs 值换成等价段数。
 * 下取整。
 */
#define XLogMBVarToSegs(mbvar, wal_segsz_bytes) \
	((mbvar) / ((wal_segsz_bytes) / (1024 * 1024)))

/*
 * XLogRecPtr 是否落在某一 XLOG 段内？
 *
 * 对 XLByteInSeg，按字面计算。对 XLByteInPrevSeg，
 * 边界字节算作属于前一段。
 */
#define XLByteInSeg(xlrp, logSegNo, wal_segsz_bytes) \
	(((xlrp) / (wal_segsz_bytes)) == (logSegNo))

#define XLByteInPrevSeg(xlrp, logSegNo, wal_segsz_bytes) \
	((((xlrp) - 1) / (wal_segsz_bytes)) == (logSegNo))

/* 检查 XLogRecPtr 值是否在合理范围内 */
#define XRecOffIsValid(xlrp) \
		((xlrp) % XLOG_BLCKSZ >= SizeOfXLogShortPHD)

/*
 * XLog 目录与控制文件（相对 $PGDATA）
 */
#define XLOGDIR				"pg_wal"
#define XLOG_CONTROL_FILE	"global/pg_control"

/*
 * 这些宏封装了 XLog 文件名、时间线历史文件名
 * 和归档状态文件名的精确布局知识。
 */
#define MAXFNAMELEN		64

/* XLog 文件名的长度 */
#define XLOG_FNAME_LEN	   24

/*
 * 生成 WAL 段文件名。切勿在会分配其生成结果的
 * 辅助函数中使用本函数。
 */
static inline void
XLogFileName(char *fname, TimeLineID tli, XLogSegNo logSegNo, int wal_segsz_bytes)
{
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)));
}

static inline void
XLogFileNameById(char *fname, TimeLineID tli, uint32 log, uint32 seg)
{
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X", tli, log, seg);
}

static inline bool
IsXLogFileName(const char *fname)
{
	return (strlen(fname) == XLOG_FNAME_LEN && \
			strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN);
}

/*
 * 带 .partial 后缀的 XLOG 段。供 pg_receivewal 及
 * 归档恢复结束时使用，当我们想归档某 WAL 段但它可能
 * 尚不完整。
 */
static inline bool
IsPartialXLogFileName(const char *fname)
{
	return (strlen(fname) == XLOG_FNAME_LEN + strlen(".partial") &&
			strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&
			strcmp(fname + XLOG_FNAME_LEN, ".partial") == 0);
}

static inline void
XLogFromFileName(const char *fname, TimeLineID *tli, XLogSegNo *logSegNo, int wal_segsz_bytes)
{
	uint32		log;
	uint32		seg;

	sscanf(fname, "%08X%08X%08X", tli, &log, &seg);
	*logSegNo = (uint64) log * XLogSegmentsPerXLogId(wal_segsz_bytes) + seg;
}

static inline void
XLogFilePath(char *path, TimeLineID tli, XLogSegNo logSegNo, int wal_segsz_bytes)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)));
}

static inline void
TLHistoryFileName(char *fname, TimeLineID tli)
{
	snprintf(fname, MAXFNAMELEN, "%08X.history", tli);
}

static inline bool
IsTLHistoryFileName(const char *fname)
{
	return (strlen(fname) == 8 + strlen(".history") &&
			strspn(fname, "0123456789ABCDEF") == 8 &&
			strcmp(fname + 8, ".history") == 0);
}

static inline void
TLHistoryFilePath(char *path, TimeLineID tli)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X.history", tli);
}

static inline void
StatusFilePath(char *path, const char *xlog, const char *suffix)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/archive_status/%s%s", xlog, suffix);
}

static inline void
BackupHistoryFileName(char *fname, TimeLineID tli, XLogSegNo logSegNo, XLogRecPtr startpoint, int wal_segsz_bytes)
{
	snprintf(fname, MAXFNAMELEN, "%08X%08X%08X.%08X.backup", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (XLogSegmentOffset(startpoint, wal_segsz_bytes)));
}

static inline bool
IsBackupHistoryFileName(const char *fname)
{
	return (strlen(fname) > XLOG_FNAME_LEN &&
			strspn(fname, "0123456789ABCDEF") == XLOG_FNAME_LEN &&
			strcmp(fname + strlen(fname) - strlen(".backup"), ".backup") == 0);
}

static inline void
BackupHistoryFilePath(char *path, TimeLineID tli, XLogSegNo logSegNo, XLogRecPtr startpoint, int wal_segsz_bytes)
{
	snprintf(path, MAXPGPATH, XLOGDIR "/%08X%08X%08X.%08X.backup", tli,
			 (uint32) (logSegNo / XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (logSegNo % XLogSegmentsPerXLogId(wal_segsz_bytes)),
			 (uint32) (XLogSegmentOffset((startpoint), wal_segsz_bytes)));
}

/*
 * 检测到对热备重要的某一参数发生变化时
 * 记录的信息。
 */
typedef struct xl_parameter_change
{
	int			MaxConnections;
	int			max_worker_processes;
	int			max_wal_senders;
	int			max_prepared_xacts;
	int			max_locks_per_xact;
	int			wal_level;
	bool		wal_log_hints;
	bool		track_commit_timestamp;
} xl_parameter_change;

/* 记录恢复点 */
typedef struct xl_restore_point
{
	TimestampTz rp_time;
	char		rp_name[MAXFNAMELEN];
} xl_restore_point;

/* 覆盖先前的 contrecord */
typedef struct xl_overwrite_contrecord
{
	XLogRecPtr	overwritten_lsn;
	TimestampTz overwrite_time;
} xl_overwrite_contrecord;

/* 恢复结束标记，当我们不做 END_OF_RECOVERY 检查点时 */
typedef struct xl_end_of_recovery
{
	TimestampTz end_time;
	TimeLineID	ThisTimeLineID; /* 新 TLI */
	TimeLineID	PrevTimeLineID; /* 从中分叉出的先前 TLI */
} xl_end_of_recovery;

/*
 * xloginsert.c 中的函数构造一条 XLogRecData 结构体链，
 * 以表示最终 WAL 记录。
 */
typedef struct XLogRecData
{
	struct XLogRecData *next;	/* 链中下一结构，或 NULL */
	char	   *data;			/* 待纳入 rmgr 数据起点 */
	uint32		len;			/* 待纳入的 rmgr 数据长度 */
} XLogRecData;

/*
 * 恢复目标动作。
 */
typedef enum
{
	RECOVERY_TARGET_ACTION_PAUSE,
	RECOVERY_TARGET_ACTION_PROMOTE,
	RECOVERY_TARGET_ACTION_SHUTDOWN
}			RecoveryTargetAction;

struct LogicalDecodingContext;
struct XLogRecordBuffer;

/*
 * 资源管理器的方法表。
 *
 * 本结构体必须与 PG_RMGR 定义保持同步，见
 * rmgr.c.
 *
 * rm_identify 必须根据 xl_info 返回记录名（不引用 
 * rmid）。例如 XLOG_BTREE_VACUUM 会名为 
 * "VACUUM"。随后可调用 rm_desc 获取记录的额外细节（
 * 若有，例如最后一块）。
 *
 * rm_mask 以资源管理器修改过的页为输入，并掩掉
 * 不应被 wal_consistency_checking 打标的位。
 *
 * RmgrTable[] 以 RmgrId 值索引（见 rmgrlist.h）。若 rm_name 为 
 * NULL，则对应 RmgrTable 条目视为无效。
 */
typedef struct RmgrData
{
	const char *rm_name;
	void		(*rm_redo) (XLogReaderState *record);
	void		(*rm_desc) (StringInfo buf, XLogReaderState *record);
	const char *(*rm_identify) (uint8 info);
	void		(*rm_startup) (void);
	void		(*rm_cleanup) (void);
	void		(*rm_mask) (char *pagedata, BlockNumber blkno);
	void		(*rm_decode) (struct LogicalDecodingContext *ctx,
							  struct XLogRecordBuffer *buf);
} RmgrData;

extern PGDLLIMPORT RmgrData RmgrTable[];
extern void RmgrStartup(void);
extern void RmgrCleanup(void);
extern void RmgrNotFound(RmgrId rmid);
extern void RegisterCustomRmgr(RmgrId rmid, const RmgrData *rmgr);

#ifndef FRONTEND
static inline bool
RmgrIdExists(RmgrId rmid)
{
	return RmgrTable[rmid].rm_name != NULL;
}

static inline RmgrData
GetRmgr(RmgrId rmid)
{
	if (unlikely(!RmgrIdExists(rmid)))
		RmgrNotFound(rmid);
	return RmgrTable[rmid];
}
#endif

/*
 * 导出以支持从检查点进程切换 xlog
 */
extern pg_time_t GetLastSegSwitchData(XLogRecPtr *lastSwitchLSN);
extern XLogRecPtr RequestXLogSwitch(bool mark_unimportant);

extern void GetOldestRestartPoint(XLogRecPtr *oldrecptr, TimeLineID *oldtli);

extern void XLogRecGetBlockRefInfo(XLogReaderState *record, bool pretty,
								   bool detailed_format, StringInfo buf,
								   uint32 *fpi_len);

/*
 * 导出供 timeline.c 与 xlogarchive.c 中的函数使用。仅在
 * 启动进程中有效
 */
extern PGDLLIMPORT bool ArchiveRecoveryRequested;
extern PGDLLIMPORT bool InArchiveRecovery;
extern PGDLLIMPORT bool StandbyMode;
extern PGDLLIMPORT char *recoveryRestoreCommand;

#endif							/* XLOG_INTERNAL_H */
