/*-------------------------------------------------------------------------
 *
 * xlog.c
 *		PostgreSQL 预写式日志管理器
 *
 * 预写式日志 (WAL) 功能除本文件外，还分散在若干源
 * 文件中：
 *
 * xloginsert.c - 构造 WAL 记录的函数
 * xlogrecovery.c - WAL 恢复与备库代码
 * xlogreader.c - 读取 WAL 文件并解析 WAL 记录的设施
 * xlogutils.c - WAL redo 例程的辅助函数
 *
 * 本文件包含协调数据库启动与检查点、并在系统运行时
 * 管理预写式日志缓冲区的函数。
 *
 * StartupXLOG() 是启动进程的主入口。它协调数据库
 * 启动、执行 WAL 恢复，以及从 WAL 恢复过渡到正常
 * 运行。
 *
 * XLogInsertRecord() 将一条 WAL 记录插入 WAL 缓冲区。多数
 * 调用者不应直接调用它，而应使用 xloginsert.c 中的函数
 * 构造 WAL 记录。可用 XLogFlush() 强制将 WAL
 * 刷盘到磁盘。
 *
 * 此外还有许多用于查询当前系统状态，以及启动/停止
 * 备份的函数。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlog.c
 *
 *-------------------------------------------------------------------------
 *
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/subtrans.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xloginsert.h"
#include "access/xlogprefetcher.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/basebackup.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "catalog/pg_database.h"
#include "common/controldata_utils.h"
#include "common/file_utils.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_iovec.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "postmaster/walwriter.h"
#include "replication/logical.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/large_object.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/reinit.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "storage/sync.h"
#include "utils/guc_hooks.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/relmapper.h"
#include "utils/pg_rusage.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

extern uint32 bootstrap_data_checksum_version;

/* 引导时使用的 时间线 ID */
#define BootstrapTimeLineID		1

/* 用户可设置的参数 */
int			max_wal_size_mb = 1024; /* 1 GB */
int			min_wal_size_mb = 80;	/* 80 MB */
int			wal_keep_size_mb = 0;
int			XLOGbuffers = -1;
int			XLogArchiveTimeout = 0;
int			XLogArchiveMode = ARCHIVE_MODE_OFF;
char	   *XLogArchiveCommand = NULL;
bool		EnableHotStandby = false;
bool		fullPageWrites = true;
bool		wal_log_hints = false;
int			wal_compression = WAL_COMPRESSION_NONE;
char	   *wal_consistency_checking_string = NULL;
bool	   *wal_consistency_checking = NULL;
bool		wal_init_zero = true;
bool		wal_recycle = true;
bool		log_checkpoints = true;
int			sync_method = DEFAULT_SYNC_METHOD;
int			wal_level = WAL_LEVEL_REPLICA;
int			CommitDelay = 0;	/* 提交延迟（微秒） */
int			CommitSiblings = 5; /* 需要睡眠的并发事务数量 */
int			wal_retrieve_retry_interval = 5000;
int			max_slot_wal_keep_size_mb = -1;
int			wal_decode_buffer_size = 512 * 1024;
bool		track_wal_io_timing = false;

#ifdef WAL_DEBUG
bool		XLOG_DEBUG = false;
#endif

int			wal_segment_size = DEFAULT_XLOG_SEG_SIZE;

/*
 * WAL 插入锁的数量。较高的值允许更多的插入操作
 * 并发发生，但会增加刷新WAL时的CPU开销，
 * 因为需要迭代所有锁。
 */
#define NUM_XLOGINSERT_LOCKS  8

/*
 * 与上次检查点的最大距离，在触发新的基于xlog的
 * 检查点之前。
 */
int			CheckPointSegments;

/* 估计的检查点间隔距离，以字节为单位 */
static double CheckPointDistanceEstimate = 0;
static double PrevCheckPointDistance = 0;

/*
 * 跟踪是否有任何自定义资源管理器的延迟检查
 * 在 wal_consistency_checking 中指定。
 */
static bool check_wal_consistency_checking_deferred = false;

/*
 * GUC 支持
 */
const struct config_enum_entry sync_method_options[] = {
	{"fsync", SYNC_METHOD_FSYNC, false},
#ifdef HAVE_FSYNC_WRITETHROUGH
	{"fsync_writethrough", SYNC_METHOD_FSYNC_WRITETHROUGH, false},
#endif
	{"fdatasync", SYNC_METHOD_FDATASYNC, false},
#ifdef O_SYNC
	{"open_sync", SYNC_METHOD_OPEN, false},
#endif
#ifdef O_DSYNC
	{"open_datasync", SYNC_METHOD_OPEN_DSYNC, false},
#endif
	{NULL, 0, false}
};


/*
 * 尽管只记录了"on"、"off"和"always"选项，
 * 我们接受所有可能的"on"和"off"变体。
 */
const struct config_enum_entry archive_mode_options[] = {
	{"always", ARCHIVE_MODE_ALWAYS, false},
	{"on", ARCHIVE_MODE_ON, false},
	{"off", ARCHIVE_MODE_OFF, false},
	{"true", ARCHIVE_MODE_ON, true},
	{"false", ARCHIVE_MODE_OFF, true},
	{"yes", ARCHIVE_MODE_ON, true},
	{"no", ARCHIVE_MODE_OFF, true},
	{"1", ARCHIVE_MODE_ON, true},
	{"0", ARCHIVE_MODE_OFF, true},
	{NULL, 0, false}
};

/*
 * 当前检查点的统计信息收集在此全局结构中。
 * 因为只有检查点进程或独立后端可以执行
 * 检查点操作，所以在普通后端中不会使用此结构。
 */
CheckpointStatsData CheckpointStats;

/*
 * 在恢复期间，
 * lastFullPageWrites跟踪重放的WAL记录所指示的full_page_writes。
 * 它使用恢复开始时检查点记录所指示的full_page_writes初始化，
 * 然后在每次重放XLOG_FPW_CHANGE记录时进行更新。
 */
static bool lastFullPageWrites;

/*
 * LocalRecoveryInProgress 是共享内存中 SharedRecoveryState 状态的本地副本。
 * 如果 SharedRecoveryState 为 RECOVERY_STATE_DONE，则为 false。
 * True 实际上意味着"未知，需要检查共享状态"。
 */
static bool LocalRecoveryInProgress = true;

/*
 * XLogInsertAllowed() 的本地状态：
 *		1: 无条件允许插入XLOG
 *		0: 无条件不允许插入XLOG
 *		-1: 必须检查 RecoveryInProgress(); 在其为假之前不允许
 * 大多数进程以-1开始，在看到恢复未进行后转换为1。
 * 但对于特殊情况我们也可以强制设定该值。
 * XLogInsertAllowed() 中的编码取决于前两种状态
 * 在数值上与布尔型的 true 和 false 相同。
 */
static int	LocalXLogInsertAllowed = -1;

/*
 * ProcLastRecPtr 指向当前后端插入的最后一个 XLOG 记录的开始位置。
 * 它会在所有插入操作中更新。XactLastRecEnd 指向
 * 最后一个记录的末尾+1，并在我们结束顶层事务时重置，
 * 或开始新事务；因此它可以用来判断当前事务是否
 * 创建了任何 XLOG 记录。
 *
 * 在并行模式下，
 * 它可能不是完全最新的。
 * 在提交时，
 * 事务可以假设这涵盖了由用户后端或在事务期间任何时刻存在的任何并行工作进程写入的所有 xlog 记录。但在中止时，
 * 或仍在并行模式下时，
 * 其他并行后端可能已写入了 LSN 更高的 WAL 记录存储在这里。
 * 并行领导者在其副本中推进，必要时在 WaitForParallelWorkersToFinish 中进行。
 */
XLogRecPtr	ProcLastRecPtr = InvalidXLogRecPtr;
XLogRecPtr	XactLastRecEnd = InvalidXLogRecPtr;
XLogRecPtr	XactLastCommitEnd = InvalidXLogRecPtr;

/*
 * RedoRecPtr 是此后端的 REDO 记录指针的本地副本（它几乎但不完全等同于指向最新CHECKPOINT 记录的指针）。
 * 我们在能够安全地执行此操作时从共享内存副本更新它，
 * XLogCtl->Insert.RedoRecPtr（即当我们持有插入锁时）。
 * 有关详细信息，
 * 请参见 XLogInsertRecord。
 * 如果我们持有 info_lck，
 * 我们也允许从 XLogCtl->RedoRecPtr 更新；请参阅 GetRedoRecPtr。
 *
 * 注意：使用此变量的代码不仅要为
 * 它可能任意过时的可能性做好准备，还要为
 * 它可能被设置为 InvalidXLogRecPtr 的可能性做好准备。我们以前
 * 在第一次调用 RecoveryInProgress() 时将其初始化，
 * 这意味着大多数可能使用它的代码可以假定它具有
 * 实际的（即使可能是陈旧的）值。但现在不再是这种情况。
 */
static XLogRecPtr RedoRecPtr;

/*
 * doPageWrites 是此后端的 (fullPageWrites ||
 * runningBackups > 0) 的本地副本。它与 RedoRecPtr 一起
 * 用于决定是否需要获取页面的全页图像。
 *
 * 注意：
 * 最初这是假的，
 * 不能保证在首次使用前会初始化为其他任何值。
 * 任何使用它的代码必须在获得 WALInsertLock 后重新检查值，
 * 如果发现之前的值不准确，需做出适当响应。
 */
static bool doPageWrites;

/*----------
 * XLOG 控制的共享内存数据结构
 *
 * LogwrtRqst 表示我们需要写入和/
 * 或同步日志的字节位置（该点之前的所有记录必须被写入或同步）。
 * LogwrtResult 表示我们已经写入/同步的字节位置。
 * 这些结构是相同的，
 * 但被分别声明以表明它们略有不同的功能。
 *
 * 要读取 XLogCtl->LogwrtResult，
 * 您必须持有 info_lck 或WALWriteLock。
 * 要更新它，您需要同时持有两个锁。
 * 这种安排的要点是已经持有 WALWriteLock 的代码可以在不需要同时获取 info_lck 的情况下检查该值。
 * 此外除了共享变量，
 * 每个后端都有 LogwrtResult 的私有副本，在方便时更新。
 *
 * 请求簿记更简单：存在共享的 XLogCtl->LogwrtRqst
 * （受 info_lck 保护），但我们不需要缓存它的任何副本。
 *
 * info_lck 只在读取/更新受保护变量时被短暂持有，
 * 所以它是一个普通的自旋锁。其他锁持有时间更长（可能
 * 跨越 I/O 操作），所以我们对它们使用 LWLocks。这些锁是：
 *
 * WALBufMappingLock: 替换 WAL 缓冲区缓存中的页面时必须持有。
 * 它仅在初始化和更改映射时被持有。如果
 * 正在替换的缓冲区内容尚未写入，映射
 * 锁在写入完成时释放，然后重新获取。
 *
 * WALWriteLock: 写入 WAL 缓冲区到磁盘时必须持有（XLogWrite 或
 * XLogFlush）。
 *
 * ControlFileLock: 读取/更新控制文件或创建
 * 新日志文件时必须持有。
 *
 * ----------
 */

typedef struct XLogwrtRqst
{
	XLogRecPtr	Write;			/* 要写出的最后一个字节 + 1 */
	XLogRecPtr	Flush;			/* 要刷盘的最后一个字节 + 1 */
} XLogwrtRqst;

typedef struct XLogwrtResult
{
	XLogRecPtr	Write;			/* 已写出的最后一个字节 + 1 */
	XLogRecPtr	Flush;			/* 已刷盘的最后一个字节 + 1 */
} XLogwrtResult;

/*
 * 插入 WAL 受少量固定的 WAL 插入锁保护。
 * 要插入 WAL，
 * 您必须持有其中一个锁 - 具体哪一个并不重要。
 * 要锁定其他并发插入，
 * 您必须持有所有锁。每个 WAL 插入锁由一个轻量级锁和一个插入进度指示器 组成。
 *
 * 当进程想要将 WAL 从内存缓冲区刷盘到磁盘时，
 * 会读取 insertingAt 值，
 * 以检查所有插入到该进程即将写出的区域的操作是否已完成。
 * 您可以简单地等待所有当前正在进行的插入完成，
 * 但 insertingAt指示器允许您忽略
 * WAL 后续位置的插入，
 * 这样您只需等待那些正在修改您即将写出的缓冲区的插入。
 *
 * 这不仅仅是优化。
 * 如果所有 WAL 缓冲区都是脏的，
 * 持有 WAL 插入锁的插入器可能需要驱逐旧的 WAL缓冲区，
 * 这需要刷盘 WAL。
 * 如果插入器可能不必要地阻塞在另一个插入器上，
 * 当两个持有 WAL 插入锁的插入器相互等待完成插入时，可能会发生死锁。
 *
 * 不跨页边界的小 WAL 记录从不更新该值，
 * WAL 记录只是复制到页面并释放锁。但
 * 为了避免上面解释的死锁场景，指示器总是在
 * 持有插入锁时休眠之前更新。
 *
 * lastImportantAt 包含使用给定锁插入的最后一个重要 WAL
 * 记录的 LSN此值用于检测自上次某些操作（如检查点）执行以来是否有重要
 * WAL 活动 - 如果没有则不重复执行操作。
 * LSN 是为所有插入更新的，
 * 除非设置了 XLOG_MARK_UNIMPORTANT 标志。
 * lastImportantAt 永远不会被清除，
 * 只会被较新记录的 LSN 覆盖。
 * 在 WALInsertLock 中直接跟踪 WAL 活动的优势是不需要任何附加锁来更新该值。
 */
typedef struct
{
	LWLock		lock;
	XLogRecPtr	insertingAt;
	XLogRecPtr	lastImportantAt;
} WALInsertLock;

/*
 * 所有 WAL 插入锁都作为数组分配在共享内存中。我们
 * 强制数组步长为 2 的幂，这可以节省一些索引
 * 周期，但更重要的是也确保单个槽不会
 * 跨越缓存行边界。（当然，我们还必须确保
 * 数组起始地址适当对齐。）
 */
typedef union WALInsertLockPadded
{
	WALInsertLock l;
	char		pad[PG_CACHE_LINE_SIZE];
} WALInsertLockPadded;

/*
 * 正在运行的备份的会话状态，用于 SQL 可调用函数中的健全性检查
 * 以启动和停止备份。
 */
static SessionBackupState sessionBackupState = SESSION_BACKUP_NONE;

/*
 * WAL 插入的共享状态数据。
 */
typedef struct XLogCtlInsert
{
	slock_t		insertpos_lck;	/* 保护 CurrBytePos 和 PrevBytePos */

	/*
	 * CurrBytePos 是保留的 WAL 的末尾。下一条记录将
	 * 在该位置插入。PrevBytePos 是之前插入的（或者更准确地说是保留的）
	 * 记录的起始位置 - 它被复制到下一条记录的 prev-link。
	 * 这些存储为"可用字节位置"而不是 XLogRecPtr
	 * （参见 XLogBytePosToRecPtr()）。
	 */
	uint64		CurrBytePos;
	uint64		PrevBytePos;

	/*
	 * 确保上述高竞争的自旋锁和字节位置位于
	 * 自己的缓存行上。特别是，下面的 RedoRecPtr 和全页
	 * 写入变量应该在不同的缓存行上。它们在
	 * 每次 WAL 插入时被读取，但很少更新，我们不希望
	 * 这些读取窃取包含 Curr/PrevBytePos 的缓存行。
	 */
	char		pad[PG_CACHE_LINE_SIZE];

	/*
	 * fullPageWrites 是所有后端用来确定是否将全页图像写入
	 * WAL 的权威值。
	 * 需要这个共享值而不是进程本地的 fullPageWrites，
	 * 因为当 full_page_writes 被 SIGHUP 更改时，
	 * 我们必须在实际影响后端的 WAL 记录之前将其 WAL 记录。
	 * 检查点进程在启动时或 SIGHUP 后设置。
	 *
	 * 要读取这些字段，您必须持有插入锁。要修改它们，
	 * 您必须持有所有锁。
	 */
	XLogRecPtr	RedoRecPtr;		/* 插入的当前重做点 */
	bool		fullPageWrites;

	/*
	 * runningBackups 是一个计数器，
	 * 指示当前正在进行中的备份数量。
	 * lastBackupStart 是用作在线备份起点的最新检查点重做位置。
	 */
	int			runningBackups;
	XLogRecPtr	lastBackupStart;

	/*
	 * WAL 插入锁。
	 */
	WALInsertLockPadded *WALInsertLocks;
} XLogCtlInsert;

/*
 * XLOG 的总共享内存状态。
 */
typedef struct XLogCtlData
{
	XLogCtlInsert Insert;

	/* 受 info_lck 保护： */
	XLogwrtRqst LogwrtRqst;
	XLogRecPtr	RedoRecPtr;		/* Insert->RedoRecPtr 的最近副本 */
	FullTransactionId ckptFullXid;	/* 最新检查点的 nextXid */
	XLogRecPtr	asyncXactLSN;	/* 最新异步提交/中止的 LSN */
	XLogRecPtr	replicationSlotMinLSN;	/* 任何槽所需的最旧 LSN */

	XLogSegNo	lastRemovedSegNo;	/* 最新删除/回收的 XLOG 段 */

	/* 伪造的 LSN 计数器，用于无日志记录的关系。受 ulsn_lck 保护。 */
	XLogRecPtr	unloggedLSN;
	slock_t		ulsn_lck;

	/* 最后一次 xlog 段切换的时间和 LSN。受 WALWriteLock 保护。 */
	pg_time_t	lastSegSwitchTime;
	XLogRecPtr	lastSegSwitchLSN;

	/*
	 * 受 info_lck 和 WALWriteLock 保护（必须持有其中任一锁才能
	 * 读取，但更新时需要同时持有两者）
	 */
	XLogwrtResult LogwrtResult;

	/*
	 * 缓存中最新初始化的页面（最后一个字节位置 + 1）。
	 *
	 * 要更改缓冲区的标识（以及 InitializedUpTo），
	 * 需要持有WALBufMappingLock。
	 * 要更改仍为脏的缓冲区的标识，
	 * 需要先写出旧页面，
	 * 这需要 WALWriteLock，
	 * 并且需要通过调用WaitXLogInsertionsToFinish() 确保没有正在进行的插入到该页面。
	 */
	XLogRecPtr	InitializedUpTo;

	/*
	 * 这些值在启动后不会改变，尽管所指向的页面
	 * 和 xlblocks 值当然会改变。xlblocks 值受到
	 * WALBufMappingLock 保护。
	 */
	char	   *pages;			/* 未写入 XLOG 页面的缓冲区 */
	XLogRecPtr *xlblocks;		/* 第一个字节指针 + XLOG_BLCKSZ */
	int			XLogCacheBlck;	/* 最高分配的 xlog 缓冲区索引 */

	/*
	 * InsertTimeLineID 是正在插入和刷新新 WAL 的时间线。
	 * 在恢复期间为零，并且一旦设置就不会改变。
	 *
	 * 如果我们在系统启动时创建了一个新时间线，
	 * PrevTimeLineID 是我们从中分叉出来的旧时间线的 ID。
	 * 否则它等于 InsertTimeLineID。
	 */
	TimeLineID	InsertTimeLineID;
	TimeLineID	PrevTimeLineID;

	/*
	 * SharedRecoveryState 表示我们是否仍在崩溃或归档
	 * 恢复中。受 info_lck 保护。
	 */
	RecoveryState SharedRecoveryState;

	/*
	 * InstallXLogFileSegmentActive 指示检查点进程是否应该
	 * 通过回收和/或 PreallocXlogFiles() 来安排未来的段。
	 * 受 ControlFileLock 保护。只有启动进程会更改它。如果
	 * 为真，任何人都可以使用 InstallXLogFileSegment()。如果为假，启动
	 * 进程拥有安装段的独占权利，通过从
	 * 归档中读取并可能替换现有文件来实现。
	 */
	bool		InstallXLogFileSegmentActive;

	/*
	 * WalWriterSleeping 指示 WAL 写入器当前是否处于
	 * 低功耗模式（因此如果异步提交发生，应被唤醒）。
	 * 受 info_lck 保护。
	 */
	bool		WalWriterSleeping;

	/*
	 * 在恢复期间，我们将最新检查点记录的副本保存在这里。
	 * lastCheckPointRecPtr 指向检查点记录的开始，
	 * lastCheckPointEndPtr 指向检查点记录的结束+1。由检查点进程
	 * 在想要创建重启点时使用。
	 *
	 * 受 info_lck 保护。
	 */
	XLogRecPtr	lastCheckPointRecPtr;
	XLogRecPtr	lastCheckPointEndPtr;
	CheckPoint	lastCheckPoint;

	/*
	 * lastFpwDisableRecPtr 指向最后重放的
	 * 指示禁用 full_page_writes 的 XLOG_FPW_CHANGE 记录的开始位置。
	 */
	XLogRecPtr	lastFpwDisableRecPtr;

	slock_t		info_lck;		/* 锁定上面显示的共享变量 */
} XLogCtlData;

static XLogCtlData *XLogCtl = NULL;

/* XLogCtl->Insert.WALInsertLocks 的私有副本，为了方便 */
static WALInsertLockPadded *WALInsertLocks = NULL;

/*
 * 我们在共享内存中维护 pg_control 的镜像。
 */
static ControlFileData *ControlFile = NULL;

/*
 * 计算 'endptr' 之后页面上剩余的空间量。
 * 注意多次求值！
 */
#define INSERT_FREESPACE(endptr)	\
	(((endptr) % XLOG_BLCKSZ == 0) ? 0 : (XLOG_BLCKSZ - (endptr) % XLOG_BLCKSZ))

/* 宏，用于前进到下一个缓冲区索引。 */
#define NextBufIdx(idx)		\
		(((idx) == XLogCtl->XLogCacheBlck) ? 0 : ((idx) + 1))

/*
 * XLogRecPtrToBufIdx 返回包含 'recptr' 的页面的 WAL 缓冲区索引，或者
 * 如果在缓存中则为将持有的页面。
 */
#define XLogRecPtrToBufIdx(recptr)	\
	(((recptr) / XLOG_BLCKSZ) % (XLogCtl->XLogCacheBlck + 1))

/*
 * WAL 页面中可用于 WAL 数据的字节数。
 */
#define UsableBytesInPage (XLOG_BLCKSZ - SizeOfXLogShortPHD)

/*
 * 将以兆字节计量的 GUCs 值转换为等效的段数。
 * 向下取整。
 */
#define ConvertToXSegs(x, segsize)	XLogMBVarToSegs((x), (segsize))

/* WAL 段中可用于 WAL 数据的字节数。 */
static int	UsableBytesInSegment;

/*
 * 共享 LogwrtResult 的私有副本，可能已过时。
 * 参见上面的讨论。
 */
static XLogwrtResult LogwrtResult = {0, 0};

/*
 * openLogFile 为 -1 或打开的日志文件段的内核 FD。
 * openLogSegNo 标识该段，
 * openLogTLI 标识对应的时间线。
 * 这些变量仅用于写入 XLOG，因此通常指向活动段。
 *
 * 注意：调用 Reserve/ReleaseExternalFD 来跟踪此 FD 的使用。
 */
static int	openLogFile = -1;
static XLogSegNo openLogSegNo = 0;
static TimeLineID openLogTLI = 0;

/*
 * 控制文件中等效字段的本地副本。运行崩溃恢复时，
 * LocalMinRecoveryPoint 被设置为 InvalidXLogRecPtr，因为我们
 * 期望重放所有可用的 WAL，并且 updateMinRecoveryPoint 被切换为
 * false 以防止在重放记录时进行任何更新。
 * 只要崩溃恢复运行，这些值就保持一致。
 */
static XLogRecPtr LocalMinRecoveryPoint;
static TimeLineID LocalMinRecoveryPointTLI;
static bool updateMinRecoveryPoint = true;

/* 用于 WALInsertLockAcquire/Release 函数 */
static int	MyLockNo = 0;
static bool holdingAllLocks = false;

#ifdef WAL_DEBUG
static MemoryContext walDebugCxt = NULL;
#endif

static void CleanupAfterArchiveRecovery(TimeLineID EndOfLogTLI,
										XLogRecPtr EndOfLog,
										TimeLineID newTLI);
static void CheckRequiredParameterValues(void);
static void XLogReportParameters(void);
static int	LocalSetXLogInsertAllowed(void);
static void CreateEndOfRecoveryRecord(void);
static XLogRecPtr CreateOverwriteContrecordRecord(XLogRecPtr aborted_lsn,
												  XLogRecPtr pagePtr,
												  TimeLineID newTLI);
static void CheckPointGuts(XLogRecPtr checkPointRedo, int flags);
static void KeepLogSeg(XLogRecPtr recptr, XLogRecPtr slotsMinLSN,
					   XLogSegNo *logSegNo);
static XLogRecPtr XLogGetReplicationSlotMinimumLSN(void);

static void AdvanceXLInsertBuffer(XLogRecPtr upto, TimeLineID tli,
								  bool opportunistic);
static void XLogWrite(XLogwrtRqst WriteRqst, TimeLineID tli, bool flexible);
static bool InstallXLogFileSegment(XLogSegNo *segno, char *tmppath,
								   bool find_free, XLogSegNo max_segno,
								   TimeLineID tli);
static void XLogFileClose(void);
static void PreallocXlogFiles(XLogRecPtr endptr, TimeLineID tli);
static void RemoveTempXlogFiles(void);
static void RemoveOldXlogFiles(XLogSegNo segno, XLogRecPtr lastredoptr,
							   XLogRecPtr endptr, TimeLineID insertTLI);
static void RemoveXlogFile(const struct dirent *segment_de,
						   XLogSegNo recycleSegNo, XLogSegNo *endlogSegNo,
						   TimeLineID insertTLI);
static void UpdateLastRemovedPtr(char *filename);
static void ValidateXLOGDirectoryStructure(void);
static void CleanupBackupHistory(void);
static void UpdateMinRecoveryPoint(XLogRecPtr lsn, bool force);
static bool PerformRecoveryXLogAction(void);
static void InitControlFile(uint64 sysidentifier);
static void WriteControlFile(void);
static void ReadControlFile(void);
static void UpdateControlFile(void);
static char *str_time(pg_time_t tnow);

static int	get_sync_bit(int method);

static void CopyXLogRecordToWAL(int write_len, bool isLogSwitch,
								XLogRecData *rdata,
								XLogRecPtr StartPos, XLogRecPtr EndPos,
								TimeLineID tli);
static void ReserveXLogInsertLocation(int size, XLogRecPtr *StartPos,
									  XLogRecPtr *EndPos, XLogRecPtr *PrevPtr);
static bool ReserveXLogSwitch(XLogRecPtr *StartPos, XLogRecPtr *EndPos,
							  XLogRecPtr *PrevPtr);
static XLogRecPtr WaitXLogInsertionsToFinish(XLogRecPtr upto);
static char *GetXLogBuffer(XLogRecPtr ptr, TimeLineID tli);
static XLogRecPtr XLogBytePosToRecPtr(uint64 bytepos);
static XLogRecPtr XLogBytePosToEndRecPtr(uint64 bytepos);
static uint64 XLogRecPtrToBytePos(XLogRecPtr ptr);

static void WALInsertLockAcquire(void);
static void WALInsertLockAcquireExclusive(void);
static void WALInsertLockRelease(void);
static void WALInsertLockUpdateInsertingAt(XLogRecPtr insertingAt);

/*
 * 插入一个由已构造的数据块链表示的 XLOG 记录。
 * 这是一个低级例程；要构造 WAL 记录头
 * 和数据，请使用 xloginsert.c 中的高级例程。
 *
 * 如果 'fpw_lsn' 有效，它是此 WAL 记录适用的页面中
 * 未作为全页图像包含在记录中的最旧 LSN。
 * 如果 fpw_lsn <= RedoRecPtr，函数不执行
 * 插入并返回 InvalidXLogRecPtr。然后调用者可以重新计算
 * 哪些页面需要全页图像，并重试。如果 fpw_lsn 无效，则
 * 记录总是被插入。
 *
 * 'flags' 对正在插入的记录提供更深入的控制。
 * 详见 XLogSetRecordFlags()。
 *
 * 'topxid_included' 告诉顶级事务ID是否与
 * 当前子事务一起记录。参见 XLogRecordAssemble()。
 *
 * 链中的第一个 XLogRecData 必须用于记录头，
 * 其数据必须是 MAXALIGNed。
 * XLogInsertRecord 填充头中的 xl_prev 和xl_crc 字段，
 * 头的其余部分必须已由调用者填充。
 *
 * 返回记录末尾的 XLOG 指针（下一条记录的开始）。
 * 这可以用作受日志记录操作影响的数据页面的 LSN。
 * （LSN 是在数据页面可以写出之前必须将 XLOG 刷盘到磁盘的XLOG 点。
 * 这实现了基本的 WAL 规则"先写日志再写数据"。
 * ）
 */
XLogRecPtr
XLogInsertRecord(XLogRecData *rdata,
				 XLogRecPtr fpw_lsn,
				 uint8 flags,
				 int num_fpi,
				 bool topxid_included)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	pg_crc32c	rdata_crc;
	bool		inserted;
	XLogRecord *rechdr = (XLogRecord *) rdata->data;
	uint8		info = rechdr->xl_info & ~XLR_INFO_MASK;
	bool		isLogSwitch = (rechdr->xl_rmid == RM_XLOG_ID &&
							   info == XLOG_SWITCH);
	XLogRecPtr	StartPos;
	XLogRecPtr	EndPos;
	bool		prevDoPageWrites = doPageWrites;
	TimeLineID	insertTLI;

	/* 我们假设所有记录头都在第一个块中 */
	Assert(rdata->len >= SizeOfXLogRecord);

	/* 交叉检查我们是否应该在这里 */
	if (!XLogInsertAllowed())
		elog(ERROR, "cannot make new WAL entries during recovery");

	/*
	 * 鉴于我们不在恢复中，InsertTimeLineID 已设置且不会改变，
	 * 所以我们可以在不持有锁的情况下读取它。
	 */
	insertTLI = XLogCtl->InsertTimeLineID;

	/*----------
	 *
	 * 我们现在已经完成了所有可以在不持有锁或不修改共享状态的
	 * 情况下完成的准备工作。从这里开始，将新 WAL 记录插入到
	 * 共享 WAL 缓冲区缓存是一个两步过程：
	 *
	 * 1. 从 WAL 中预留适量的空间。
	 *	  保留空间的当前头部保存在 Insert->CurrBytePos 中，
	 *	  受 insertpos_lck 保护。
	 *
	 * 2. 将记录复制到预留的 WAL 空间。这涉及找到包含
	 *	  预留空间的正确 WAL 缓冲区，并将记录复制到适当位置。
	 *	  这可以在多个进程中并发完成。
	 *
	 * 为了跟踪哪些插入仍在进行中，
	 * 每个并发插入器获取一个插入锁。
	 * 除了仅指示插入正在进行外，该锁还告诉其他人插入器已进行到多远。
	 * 有少量固定的插入锁，
	 * 由 NUM_XLOGINSERT_LOCKS 决定。当插入器跨越页面边界时，
	 * 它会更新存储在锁中的值，
	 * 以指示它已插入到多远，从而允许刷新前一个缓冲区。
	 *
	 * 保持插入锁还可以保护 RedoRecPtr 和 fullPageWrites 在
	 * 插入完成之前不被更改。
	 *
	 * 第 2 步通常可以完全并行完成。
	 * 如果所需的 WAL 页面尚未初始化，
	 * 您必须获取 WALBufMappingLock 来初始化它，
	 * 但 WAL 写入器会尝试在插入之前完成此操作，以避免在关键路径中发生这种情况。
	 *
	 * ----------
	 */
	START_CRIT_SECTION();
	if (isLogSwitch)
		WALInsertLockAcquireExclusive();
	else
		WALInsertLockAcquire();

	/*
	 * 检查我的 RedoRecPtr 副本是否已过期。如果是，可能需要
	 * 返回并让调用者重新计算所有内容。这只能
	 * 在检查点之后立即发生，所以在这种情况下慢一点更好，
	 * 其他情况下快一点。
	 *
	 * 还要检查 fullPageWrites 是否刚刚打开，或者是否有
	 * 正在运行的备份（这会强制全页写入）；如果我们之前没有
	 * 进行全页写入，则返回并重新计算。
	 *
	 * 如果我们不进行全页写入，那么 RedoRecPtr 实际上不影响
	 * XLOG 记录的内容，所以我们将更新本地副本
	 * 但不强制重新计算。（如果 doPageWrites 刚刚关闭，
	 * 我们可以重新计算不带全页的记录，但我们选择不
	 * 这么做。）
	 */
	if (RedoRecPtr != Insert->RedoRecPtr)
	{
		Assert(RedoRecPtr < Insert->RedoRecPtr);
		RedoRecPtr = Insert->RedoRecPtr;
	}
	doPageWrites = (Insert->fullPageWrites || Insert->runningBackups > 0);

	if (doPageWrites &&
		(!prevDoPageWrites ||
		 (fpw_lsn != InvalidXLogRecPtr && fpw_lsn <= RedoRecPtr)))
	{
		/*
		 * 哎呀，现在有些缓冲区需要备份，但调用者没有备份。
		 * 重新开始。
		 */
		WALInsertLockRelease();
		END_CRIT_SECTION();
		return InvalidXLogRecPtr;
	}

	/*
	 * 为 WAL 中的记录预留空间。
	 * 这也会设置 xl_prev 指针。
	 */
	if (isLogSwitch)
		inserted = ReserveXLogSwitch(&StartPos, &EndPos, &rechdr->xl_prev);
	else
	{
		ReserveXLogInsertLocation(rechdr->xl_tot_len, &StartPos, &EndPos,
								  &rechdr->xl_prev);
		inserted = true;
	}

	if (inserted)
	{
		/*
		 * 现在 xl_prev 已填充，
		 * 计算记录头的 CRC。
		 */
		rdata_crc = rechdr->xl_crc;
		COMP_CRC32C(rdata_crc, rechdr, offsetof(XLogRecord, xl_crc));
		FIN_CRC32C(rdata_crc);
		rechdr->xl_crc = rdata_crc;

		/*
		 * 所有记录数据（包括头）现在已准备好被插入。
		 * 将记录复制到预留的空间中。
		 */
		CopyXLogRecordToWAL(rechdr->xl_tot_len, isLogSwitch, rdata,
							StartPos, EndPos, insertTLI);

		/*
		 * 除非记录被标记为不重要，
		 * 否则更新当前槽中最后一条重要记录的 LSN。
		 * 当持有所有锁时，只更新第一个。
		 */
		if ((flags & XLOG_MARK_UNIMPORTANT) == 0)
		{
			int			lockno = holdingAllLocks ? 0 : MyLockNo;

			WALInsertLocks[lockno].l.lastImportantAt = StartPos;
		}
	}
	else
	{
		/*
		 * 这是一个 xlog-switch 记录，
		 * 但当前插入位置已经正好在段的开头，
		 * 所以不需要做任何事情。
		 */
	}

	/*
	 * 完成！让其他人知道我们已经完成了。
	 */
	WALInsertLockRelease();

	END_CRIT_SECTION();

	MarkCurrentTransactionIdLoggedIfAny();

	/*
	 * 标记顶级事务 ID 已记录（如果需要），这样我们就不会尝试
	 * 在当前子事务的下一条 WAL 记录中再次记录它。
	 */
	if (topxid_included)
		MarkSubxactTopXidLogged();

	/*
	 * 如果我们跨越了页面边界，更新共享的 LogwrtRqst.Write。
	 */
	if (StartPos / XLOG_BLCKSZ != EndPos / XLOG_BLCKSZ)
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		/* 推进全局请求以包含新块 */
		if (XLogCtl->LogwrtRqst.Write < EndPos)
			XLogCtl->LogwrtRqst.Write = EndPos;
		/* 有机会时更新本地结果副本 */
		LogwrtResult = XLogCtl->LogwrtResult;
		SpinLockRelease(&XLogCtl->info_lck);
	}

	/*
	 * 如果这是 XLOG_SWITCH 记录，
	 * 刷新记录和填充段其余部分的空白填充空间，
	 * 并执行段结束操作（例如，通知归档器）。
	 */
	if (isLogSwitch)
	{
		TRACE_POSTGRESQL_WAL_SWITCH();
		XLogFlush(EndPos);

		/*
		 * 即使我们为自己预留了段的其余部分（反映在
		 * EndPos 中），
		 * 我们返回一个仅指向 xlog-switch 记录末尾的指针。
		 */
		if (inserted)
		{
			EndPos = StartPos + SizeOfXLogRecord;
			if (StartPos / XLOG_BLCKSZ != EndPos / XLOG_BLCKSZ)
			{
				uint64		offset = XLogSegmentOffset(EndPos, wal_segment_size);

				if (offset == EndPos % XLOG_BLCKSZ)
					EndPos += SizeOfXLogLongPHD;
				else
					EndPos += SizeOfXLogShortPHD;
			}
		}
	}

#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
	{
		static XLogReaderState *debug_reader = NULL;
		XLogRecord *record;
		DecodedXLogRecord *decoded;
		StringInfoData buf;
		StringInfoData recordBuf;
		char	   *errormsg = NULL;
		MemoryContext oldCxt;

		oldCxt = MemoryContextSwitchTo(walDebugCxt);

		initStringInfo(&buf);
		appendStringInfo(&buf, "INSERT @ %X/%X: ", LSN_FORMAT_ARGS(EndPos));

		/*
		 * 我们必须从 XLogRecData 组装 WAL 记录数据
		 * 条目，以便我们可以将其作为一个
		 * 连续的数据块传递给 rm_desc 函数。
		 */
		initStringInfo(&recordBuf);
		for (; rdata != NULL; rdata = rdata->next)
			appendBinaryStringInfo(&recordBuf, rdata->data, rdata->len);

		/* 我们还需要临时空间来解码记录。 */
		record = (XLogRecord *) recordBuf.data;
		decoded = (DecodedXLogRecord *)
			palloc(DecodeXLogRecordRequiredSpace(record->xl_tot_len));

		if (!debug_reader)
			debug_reader = XLogReaderAllocate(wal_segment_size, NULL,
											  XL_ROUTINE(.page_read = NULL,
														 .segment_open = NULL,
														 .segment_close = NULL),
											  NULL);
		if (!debug_reader)
		{
			appendStringInfoString(&buf, "error decoding record: out of memory while allocating a WAL reading processor");
		}
		else if (!DecodeXLogRecord(debug_reader,
								   decoded,
								   record,
								   EndPos,
								   &errormsg))
		{
			appendStringInfo(&buf, "error decoding record: %s",
							 errormsg ? errormsg : "no error message");
		}
		else
		{
			appendStringInfoString(&buf, " - ");

			debug_reader->record = decoded;
			xlog_outdesc(&buf, debug_reader);
			debug_reader->record = NULL;
		}
		elog(LOG, "%s", buf.data);

		pfree(decoded);
		pfree(buf.data);
		pfree(recordBuf.data);
		MemoryContextSwitchTo(oldCxt);
	}
#endif

	/*
	 * 更新我们的全局变量
	 */
	ProcLastRecPtr = StartPos;
	XactLastRecEnd = EndPos;

	/* 向仪表报告 WAL 流量。 */
	if (inserted)
	{
		pgWalUsage.wal_bytes += rechdr->xl_tot_len;
		pgWalUsage.wal_records++;
		pgWalUsage.wal_fpi += num_fpi;
	}

	return EndPos;
}

/*
 * 从 WAL 中为给定大小的记录预留适量空间。
 * *StartPos 设为预留段的起点，
 * *EndPos 设为其终点+1。
 * *PrevPtr 设为前一条记录的起点；
 *
 * 用于设置本记录的 xl_prev。
 * 这是 XLogInsert 
 * 中必须跨后端串行化的性能关键部分。
 * 其余大多可并行。
 *
 * 尽量保持此段短小，
 * insertpos_lck 会非常热。
 */
static void
ReserveXLogInsertLocation(int size, XLogRecPtr *StartPos, XLogRecPtr *EndPos,
						  XLogRecPtr *PrevPtr)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint64		startbytepos;
	uint64		endbytepos;
	uint64		prevbytepos;

	size = MAXALIGN(size);

	/* 所有（非 xlog-switch）记录都应包含数据。 */
	Assert(size > SizeOfXLogRecord);

	/*
	 * 持有自旋锁的时间需要最小化，方法是尽量减少
	 * 在持有锁时需要进行的计算。当前保留的 WAL 的
	 * 顶部保存在 CurrBytePos 中，作为一个字节位置，
	 * 只计算 WAL 中的"可用"字节，即排除所有 WAL
	 * 页面头。"可用"字节位置和物理位置
	 * （XLogRecPtrs）之间的映射可以在锁定区域外完成，
	 * 因为可用字节位置不包括任何头，所以从 WAL 中
	 * 预留 X 字节几乎就像"CurrBytePos += X"一样简单。
	 */
	SpinLockAcquire(&Insert->insertpos_lck);

	startbytepos = Insert->CurrBytePos;
	endbytepos = startbytepos + size;
	prevbytepos = Insert->PrevBytePos;
	Insert->CurrBytePos = endbytepos;
	Insert->PrevBytePos = startbytepos;

	SpinLockRelease(&Insert->insertpos_lck);

	*StartPos = XLogBytePosToRecPtr(startbytepos);
	*EndPos = XLogBytePosToEndRecPtr(endbytepos);
	*PrevPtr = XLogBytePosToRecPtr(prevbytepos);

	/*
	 * 检查"可用字节位置"和 XLogRecPtrs 之间的转换
	 * 在两个方向上是否一致工作。
	 */
	Assert(XLogRecPtrToBytePos(*StartPos) == startbytepos);
	Assert(XLogRecPtrToBytePos(*EndPos) == endbytepos);
	Assert(XLogRecPtrToBytePos(*PrevPtr) == prevbytepos);
}

/*
 * 类似于 ReserveXLogInsertLocation()，但用于 xlog-switch 记录。
 *
 * 日志切换记录的处理略有不同。
 * 段的其余部分将为此次插入保留，
 * 如返回的 *EndPos 值所示。但是，
 * 如果我们已经在当前段的开头，
 * *StartPos 和 *EndPos 被设置为当前位置而不预留任何空间，函数返回 false。
*/
static bool
ReserveXLogSwitch(XLogRecPtr *StartPos, XLogRecPtr *EndPos, XLogRecPtr *PrevPtr)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint64		startbytepos;
	uint64		endbytepos;
	uint64		prevbytepos;
	uint32		size = MAXALIGN(SizeOfXLogRecord);
	XLogRecPtr	ptr;
	uint32		segleft;

	/*
	 * 这些计算在持有自旋锁时进行有些重，
	 * 但由于我们持有所有 WAL 插入锁，
	 * 没有其他插入器竞争它。
	 * GetXLogInsertRecPtr() 确实会竞争它，但调用频率不高。
	 */
	SpinLockAcquire(&Insert->insertpos_lck);

	startbytepos = Insert->CurrBytePos;

	ptr = XLogBytePosToEndRecPtr(startbytepos);
	if (XLogSegmentOffset(ptr, wal_segment_size) == 0)
	{
		SpinLockRelease(&Insert->insertpos_lck);
		*EndPos = *StartPos = ptr;
		return false;
	}

	endbytepos = startbytepos + size;
	prevbytepos = Insert->PrevBytePos;

	*StartPos = XLogBytePosToRecPtr(startbytepos);
	*EndPos = XLogBytePosToEndRecPtr(endbytepos);

	segleft = wal_segment_size - XLogSegmentOffset(*EndPos, wal_segment_size);
	if (segleft != wal_segment_size)
	{
		/* 消耗段的其余部分 */
		*EndPos += segleft;
		endbytepos = XLogRecPtrToBytePos(*EndPos);
	}
	Insert->CurrBytePos = endbytepos;
	Insert->PrevBytePos = startbytepos;

	SpinLockRelease(&Insert->insertpos_lck);

	*PrevPtr = XLogBytePosToRecPtr(prevbytepos);

	Assert(XLogSegmentOffset(*EndPos, wal_segment_size) == 0);
	Assert(XLogRecPtrToBytePos(*EndPos) == endbytepos);
	Assert(XLogRecPtrToBytePos(*StartPos) == startbytepos);
	Assert(XLogRecPtrToBytePos(*PrevPtr) == prevbytepos);

	return true;
}

/*
 * XLogInsertRecord 的子例程。将 WAL 记录复制到 WAL 中
 * 已预留的区域。
 */
static void
CopyXLogRecordToWAL(int write_len, bool isLogSwitch, XLogRecData *rdata,
					XLogRecPtr StartPos, XLogRecPtr EndPos, TimeLineID tli)
{
	char	   *currpos;
	int			freespace;
	int			written;
	XLogRecPtr	CurrPos;
	XLogPageHeader pagehdr;

	/*
	 * 获取正确的
	 * WAL 缓冲区中正确位置的指针以开始插入。
	 */
	CurrPos = StartPos;
	currpos = GetXLogBuffer(CurrPos, tli);
	freespace = INSERT_FREESPACE(CurrPos);

	/*
	 * 此页上应该有足够的空间容纳至少第一个字段
	 * (xl_tot_len)。
	 */
	Assert(freespace >= sizeof(uint32));

	/* 复制记录数据 */
	written = 0;
	while (rdata != NULL)
	{
		char	   *rdata_data = rdata->data;
		int			rdata_len = rdata->len;

		while (rdata_len > freespace)
		{
			/*
			 * 写入适合此页面的内容，并在下一页继续。
			 */
			Assert(CurrPos % XLOG_BLCKSZ >= SizeOfXLogShortPHD || freespace == 0);
			memcpy(currpos, rdata_data, freespace);
			rdata_data += freespace;
			rdata_len -= freespace;
			written += freespace;
			CurrPos += freespace;

			/*
			 * 获取下一页开头的指针，并在页面头中设置 xlp_rem_len。
			 * 设置 XLP_FIRST_IS_CONTRECORD。
			 *
			 * 在没有页面锁的情况下设置 contrecord
			 * 标志和 xlp_rem_len 是安全的。
			 * 所有其他标志在 AdvanceXLInsertBuffer 中初始化页面时已设置，
			 * 而我们是唯一需要设置 contrecord 标志的后端。
			 */
			currpos = GetXLogBuffer(CurrPos, tli);
			pagehdr = (XLogPageHeader) currpos;
			pagehdr->xlp_rem_len = write_len - written;
			pagehdr->xlp_info |= XLP_FIRST_IS_CONTRECORD;

			/* 跳过页面头 */
			if (XLogSegmentOffset(CurrPos, wal_segment_size) == 0)
			{
				CurrPos += SizeOfXLogLongPHD;
				currpos += SizeOfXLogLongPHD;
			}
			else
			{
				CurrPos += SizeOfXLogShortPHD;
				currpos += SizeOfXLogShortPHD;
			}
			freespace = INSERT_FREESPACE(CurrPos);
		}

		Assert(CurrPos % XLOG_BLCKSZ >= SizeOfXLogShortPHD || rdata_len == 0);
		memcpy(currpos, rdata_data, rdata_len);
		currpos += rdata_len;
		CurrPos += rdata_len;
		freespace -= rdata_len;
		written += rdata_len;

		rdata = rdata->next;
	}
	Assert(written == write_len);

	/*
	 * 如果这是 xlog-switch，仅写入切换记录是不够的，
	 * 我们还必须消耗 WAL 段中的所有剩余空间。
	 * 我们已经预留了该空间，但需要实际填充它。
	 */
	if (isLogSwitch && XLogSegmentOffset(CurrPos, wal_segment_size) != 0)
	{
		/* xlog-switch 记录除头外不包含任何数据 */
		Assert(write_len == SizeOfXLogRecord);

		/* 断言我们确实预留了正确的空间量 */
		Assert(XLogSegmentOffset(EndPos, wal_segment_size) == 0);

		/* 使用当前页面上的所有剩余空间 */
		CurrPos += freespace;

		/*
		 * 导致段中所有剩余页面被刷盘，使 XLog 位置
		 * 处于它应该在的位置，即下一个段的开头。
		 * 我们一次处理一页，以确保如果 wal_buffers < wal_segment_size，
		 * 我们不会与自己发生死锁。
		 */
		while (CurrPos < EndPos)
		{
			/*
			 * 刷新页面的最小操作是调用
			 * WALInsertLockUpdateInsertingAt(CurrPos) 然后调用
			 * AdvanceXLInsertBuffer(...)。页面将被初始化为
			 * 大部分为零，除了页面头（始终是短变体，
			 * 因为这永远不会是段的第一个页面）。
			 *
			 * 大片的零有利于压缩，
			 * 但每隔 XLOG_BLCKSZ 打断它们的头（其值因页面而异）则不然。
			 * 效果因压缩工具而异，
			 * 例如 bzip2 如果保留这些头，
			 * 压缩效果会差一个数量级。
			 *
			 * 与其使 AdvanceXLInsertBuffer 
			 * 本身复杂化（它在高负载情况下以及这种轻负载情况下都会被调用），
			 * 我们只需使用GetXLogBuffer（
			 * 它本身调用我们需要的两个方法）来获取指针并将大部分页面清零。
			 * 然后我们只需将页面头清零。
			 */
			currpos = GetXLogBuffer(CurrPos, tli);
			MemSet(currpos, 0, SizeOfXLogShortPHD);

			CurrPos += XLOG_BLCKSZ;
		}
	}
	else
	{
		/* 对齐结束位置，以便下一条记录对齐开始 */
		CurrPos = MAXALIGN64(CurrPos);
	}

	if (CurrPos != EndPos)
		elog(PANIC, "space reserved for WAL record does not match what was written");
}

/*
 * 获取 WAL 插入锁，用于插入 WAL。
 */
static void
WALInsertLockAcquire(void)
{
	bool		immed;

	/*
	 * 我们获取哪个 WAL 插入锁并不重要，所以尝试
	 * 我们上次使用的那个。如果系统不是很忙，
	 * 很有可能它仍然可用，而且对特定锁有一定的
	 * 亲和性是好的，这样在没有竞争时您不会不必要地在
	 * 进程之间弹跳缓存行。
	 *
	 * 如果这是此后端的第一次，
	 * (半)随机选择一个锁。
	 * 这允许如果您有大量非常短的连接，锁可以被均匀使用。
	 */
	static int	lockToTry = -1;

	if (lockToTry == -1)
		lockToTry = MyProc->pgprocno % NUM_XLOGINSERT_LOCKS;
	MyLockNo = lockToTry;

	/*
	 * insertingAt 值初始设置为 0，因为我们还不知道
	 * 我们的插入位置。
	 */
	immed = LWLockAcquire(&WALInsertLocks[MyLockNo].l.lock, LW_EXCLUSIVE);
	if (!immed)
	{
		/*
		 * 如果我们不能立即获取锁，
		 * 下次尝试另一个锁。
		 * 在插入锁多于并发插入器的系统上，
		 * 这会导致所有插入器最终迁移到一个没有其他人使用的锁。
		 * 在插入器多于锁的系统上，
		 * 这仍然有助于将插入器均匀分布在各个锁上。
		 */
		lockToTry = (lockToTry + 1) % NUM_XLOGINSERT_LOCKS;
	}
}

/*
 * 获取所有 WAL 插入锁，
 * 以防止其他后端插入 WAL。
 */
static void
WALInsertLockAcquireExclusive(void)
{
	int			i;

	/*
	 * 当持有所有锁时，除最后一个锁外的所有锁的 insertingAt
	 * 指示器设置为 0xFFFFFFFFFFFFFFFF，这比任何真实的
	 * XLogRecPtr 值都高，以确保没有人在那些锁上阻塞等待。
	 */
	for (i = 0; i < NUM_XLOGINSERT_LOCKS - 1; i++)
	{
		LWLockAcquire(&WALInsertLocks[i].l.lock, LW_EXCLUSIVE);
		LWLockUpdateVar(&WALInsertLocks[i].l.lock,
						&WALInsertLocks[i].l.insertingAt,
						PG_UINT64_MAX);
	}
	/* 释放时变量值重置为 0 */
	LWLockAcquire(&WALInsertLocks[i].l.lock, LW_EXCLUSIVE);

	holdingAllLocks = true;
}

/*
 * 释放我们的插入锁（如果我们持有所有锁，则是所有锁）。
 *
 * 注意：将所有变量重置为 0，这样它们会导致 LWLockWaitForVar 在
 * 下次获取锁时阻塞。
 */
static void
WALInsertLockRelease(void)
{
	if (holdingAllLocks)
	{
		int			i;

		for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
			LWLockReleaseClearVar(&WALInsertLocks[i].l.lock,
								  &WALInsertLocks[i].l.insertingAt,
								  0);

		holdingAllLocks = false;
	}
	else
	{
		LWLockReleaseClearVar(&WALInsertLocks[MyLockNo].l.lock,
							  &WALInsertLocks[MyLockNo].l.insertingAt,
							  0);
	}
}

/*
 * 更新我们的 insertingAt 值，让其他人知道我们已经完成
 * 到该点的插入。
 */
static void
WALInsertLockUpdateInsertingAt(XLogRecPtr insertingAt)
{
	if (holdingAllLocks)
	{
		/*
		 * 我们使用最后一个锁来标记我们的实际位置，
		 * 参见 WALInsertLockAcquireExclusive 中的注释。
		 */
		LWLockUpdateVar(&WALInsertLocks[NUM_XLOGINSERT_LOCKS - 1].l.lock,
						&WALInsertLocks[NUM_XLOGINSERT_LOCKS - 1].l.insertingAt,
						insertingAt);
	}
	else
		LWLockUpdateVar(&WALInsertLocks[MyLockNo].l.lock,
						&WALInsertLocks[MyLockNo].l.insertingAt,
						insertingAt);
}

/*
 * 等待任何 < upto 的 WAL 插入完成。
 *
 * 返回仍在进行的最旧插入的位置。
 * 该点之前的任何 WAL 已完全复制到 WAL 缓冲区中，并且
 * 可以刷盘到磁盘。因为这会等待任何比
 * 'upto' 更早的插入完成，所以返回值始终 >= 'upto'。
 *
 * 注意：当您即将写出 WAL 时，必须在获取 WALWriteLock *之前*
 * 调用此函数，以避免死锁。此函数可能
 * 需要等待插入完成（或至少前进到下一个
 * 未初始化页面），而插入器可能需要驱逐旧的 WAL 缓冲区
 * 为新缓冲区腾出空间，而这反过来又需要 WALWriteLock。
 */
static XLogRecPtr
WaitXLogInsertionsToFinish(XLogRecPtr upto)
{
	uint64		bytepos;
	XLogRecPtr	reservedUpto;
	XLogRecPtr	finishedUpto;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	int			i;

	if (MyProc == NULL)
		elog(PANIC, "cannot wait without a PGPROC structure");

	/* 读取当前插入位置 */
	SpinLockAcquire(&Insert->insertpos_lck);
	bytepos = Insert->CurrBytePos;
	SpinLockRelease(&Insert->insertpos_lck);
	reservedUpto = XLogBytePosToEndRecPtr(bytepos);

	/*
	 * 任何人都不应该请求刷新尚未预留的 WAL 片段。
	 * 但是，如果磁盘上有一个伪造 LSN 的块，
	 * 这种情况可能会发生。XLogFlush 会检查这种情况并发出警告，
	 * 但只在刷新之后。
	 * 在这里，我们只是假设这意味着所有已预留的 WAL 都需要完成。
	 * 在这种极端情况下，返回值可能小于 'upto' 参数。
	 */
	if (upto > reservedUpto)
	{
		ereport(LOG,
				(errmsg("request to flush past end of generated WAL; request %X/%X, current position %X/%X",
						LSN_FORMAT_ARGS(upto), LSN_FORMAT_ARGS(reservedUpto))));
		upto = reservedUpto;
	}

	/*
	 * 循环遍历所有锁，
	 * 在早于 'upto' 的任何进行中插入上休眠。
	 *
	 * finishedUpto 是我们的返回值，
	 * 指示所有 WAL 插入已完成到的点。将其初始化为预留 WAL 的头部，
	 * 当我们遍历插入锁时，
	 * 对于仍在进行中的任何插入，将其后退。
	 */
	finishedUpto = reservedUpto;
	for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
	{
		XLogRecPtr	insertingat = InvalidXLogRecPtr;

		do
		{
			/*
			 * 查看此插入是否正在进行。
			 * LWLockWaitForVar 将等待锁被释放，
			 * 或等待 'value' 由LWLockUpdateVar 调用设置。
			 * 当锁最初被获取时，
			 * 其值为 0 (InvalidXLogRecPtr)，这意味着我们不知道它在哪里插入。
			 * 我们必须等待。
			 * 如果它是一个小插入，
			 * 记录很可能会适应同一页，
			 * 插入者将在不调用LWLockUpdateVar 的情况下释放锁。
			 * 但如果它需要休眠，它将在休眠前用 LWLockUpdateVar 广告插入点。
			 */
			if (LWLockWaitForVar(&WALInsertLocks[i].l.lock,
								 &WALInsertLocks[i].l.insertingAt,
								 insertingat, &insertingat))
			{
				/* 锁是空闲的，所以没有进行中的插入 */
				insertingat = InvalidXLogRecPtr;
				break;
			}

			/*
			 * 此插入仍在进行中。必须等待，除非
			 * 插入者已经超过了 'upto'。
			 */
		} while (insertingat < upto);

		if (insertingat != InvalidXLogRecPtr && insertingat < finishedUpto)
			finishedUpto = insertingat;
	}
	return finishedUpto;
}

/*
 * 获取指向包含给定 XLogRecPtr
 * 的 WAL 缓冲区中正确位置的指针。
 *
 * 如果页面尚未初始化，则进行初始化。这可能需要
 * 从缓冲区缓存中驱逐旧的脏缓冲区，这意味着 I/O。
 *
 * 调用者必须确保包含请求位置的页面尚未被驱逐，
 * 并且不会被驱逐。确保这一点的方法是
 * 持有一个 WAL 插入锁，其 insertingAt 位置设置为
 * 某个 <= ptr 的值。如果需要从缓冲区驱逐旧页面，
 * GetXLogBuffer() 将更新 insertingAt。（这意味着一旦您
 * 用给定的 'ptr' 调用 GetXLogBuffer()，您就不能再访问该点之前的
 * 任何内容，以后也不能用更旧的 'ptr' 调用 GetXLogBuffer()，
 * 因为更旧的缓冲区可能已经被回收）
 */
static char *
GetXLogBuffer(XLogRecPtr ptr, TimeLineID tli)
{
	int			idx;
	XLogRecPtr	endptr;
	static uint64 cachedPage = 0;
	static char *cachedPos = NULL;
	XLogRecPtr	expectedEndPtr;

	/*
	 * 快速路径，
	 * 用于我们需要再次访问与上次相同的页面的常见情况。
	 */
	if (ptr / XLOG_BLCKSZ == cachedPage)
	{
		Assert(((XLogPageHeader) cachedPos)->xlp_magic == XLOG_PAGE_MAGIC);
		Assert(((XLogPageHeader) cachedPos)->xlp_pageaddr == ptr - (ptr % XLOG_BLCKSZ));
		return cachedPos + ptr % XLOG_BLCKSZ;
	}

	/*
	 * XLog 缓冲区缓存的组织方式是页面始终加载到
	 * 特定的缓冲区。这样我们可以仅从 XLogRecPtr 轻松计算
	 * 给定页面必须加载到哪个缓冲区。
	 */
	idx = XLogRecPtrToBufIdx(ptr);

	/*
	 * 查看当前缓冲区中加载的是什么页面。
	 * 它可能是我们正在寻找的页面，
	 * 或者是更旧的页面。
	 * 它不可能是更新的页面——那将意味着我们正在寻找的页面已经被写出磁盘并被驱逐，
	 * 调用者负责确保这种情况不会发生。
	 *
	 * 但是，
	 * 我们在读取值时不持有锁。如果有人刚刚初始化了页面，
	 * 如果在此平台上 64 位获取不是原子的，我们可能会得到 XLogRecPtr 的"撕裂读取"。
	 * 在这种情况下，
	 * 我们会看到一个伪造的值。没关系，
	 * 我们会获取映射锁（在 AdvanceXLInsertBuffer 中），如果我们看到的不是我们要找的页面，
	 * 就会重试。
	 * 但这意味着当我们进行这种无锁读取时，我们可能会看到一个看起来在我们正在寻找的页面之前的值。
	 * 在我们持有锁时验证该值之前，不要对此产生 PANIC。
	 */
	expectedEndPtr = ptr;
	expectedEndPtr += XLOG_BLCKSZ - ptr % XLOG_BLCKSZ;

	endptr = XLogCtl->xlblocks[idx];
	if (expectedEndPtr != endptr)
	{
		XLogRecPtr	initializedUpto;

		/*
		 * 在调用可能阻塞的 AdvanceXLInsertBuffer() 之前，让其他人
		 * 知道我们完成插入记录到哪里了。
		 *
		 * 注意：
		 * 如果 'ptr' 指向页面头之后的位置，
		 * 广播一个在页面开头而不是 'ptr' 本身的位置。
		 * 如果没有其他插入在运行，
		 * 有人可能会尝试刷新到我们广播的位置。
		 * 如果我们广播的是页面头之后的位置，
		 * 有人可能会尝试刷新页面头，
		 * 即使页面实际上可能尚未初始化。
		 * 作为页面上的第一个插入者，我们有责任确保在让insertingAt 越过页面头之前它已被初始化。
		 */
		if (ptr % XLOG_BLCKSZ == SizeOfXLogShortPHD &&
			XLogSegmentOffset(ptr, wal_segment_size) > XLOG_BLCKSZ)
			initializedUpto = ptr - SizeOfXLogShortPHD;
		else if (ptr % XLOG_BLCKSZ == SizeOfXLogLongPHD &&
				 XLogSegmentOffset(ptr, wal_segment_size) < XLOG_BLCKSZ)
			initializedUpto = ptr - SizeOfXLogLongPHD;
		else
			initializedUpto = ptr;

		WALInsertLockUpdateInsertingAt(initializedUpto);

		AdvanceXLInsertBuffer(ptr, tli, false);
		endptr = XLogCtl->xlblocks[idx];

		if (expectedEndPtr != endptr)
			elog(PANIC, "could not find WAL buffer for %X/%X",
				 LSN_FORMAT_ARGS(ptr));
	}
	else
	{
		/*
		 * 确保页面的初始化对我们可见，并且不会
		 * 稍后到达并覆盖我们在页面上写入的 WAL 数据。
		 */
		pg_memory_barrier();
	}

	/*
	 * 找到包含此页面的缓冲区。返回指向页面内
	 * 正确偏移量的指针。
	 */
	cachedPage = ptr / XLOG_BLCKSZ;
	cachedPos = XLogCtl->pages + idx * (Size) XLOG_BLCKSZ;

	Assert(((XLogPageHeader) cachedPos)->xlp_magic == XLOG_PAGE_MAGIC);
	Assert(((XLogPageHeader) cachedPos)->xlp_pageaddr == ptr - (ptr % XLOG_BLCKSZ));

	return cachedPos + ptr % XLOG_BLCKSZ;
}

/*
 * 将"可用字节位置"转换为 XLogRecPtr。
 * 可用字节位置是从 WAL 开头开始的位置，
 * 排除所有 WAL 页面头。
 */
static XLogRecPtr
XLogBytePosToRecPtr(uint64 bytepos)
{
	uint64		fullsegs;
	uint64		fullpages;
	uint64		bytesleft;
	uint32		seg_offset;
	XLogRecPtr	result;

	fullsegs = bytepos / UsableBytesInSegment;
	bytesleft = bytepos % UsableBytesInSegment;

	if (bytesleft < XLOG_BLCKSZ - SizeOfXLogLongPHD)
	{
		/* 适合段的第一个页面 */
		seg_offset = bytesleft + SizeOfXLogLongPHD;
	}
	else
	{
		/* 考虑段中带有长头的第一个页面 */
		seg_offset = XLOG_BLCKSZ;
		bytesleft -= XLOG_BLCKSZ - SizeOfXLogLongPHD;

		fullpages = bytesleft / UsableBytesInPage;
		bytesleft = bytesleft % UsableBytesInPage;

		seg_offset += fullpages * XLOG_BLCKSZ + bytesleft + SizeOfXLogShortPHD;
	}

	XLogSegNoOffsetToRecPtr(fullsegs, seg_offset, wal_segment_size, result);

	return result;
}

/*
 * 类似于 XLogBytePosToRecPtr，但如果位置在页面边界处，
 * 返回页面开始的指针（即页面头部之前），
 * 而不是该页面上第一个xlog记录应该去的位置。这被用来
 * 当转换指向记录结尾的指针时。
 */
static XLogRecPtr
XLogBytePosToEndRecPtr(uint64 bytepos)
{
	uint64		fullsegs;
	uint64		fullpages;
	uint64		bytesleft;
	uint32		seg_offset;
	XLogRecPtr	result;

	fullsegs = bytepos / UsableBytesInSegment;
	bytesleft = bytepos % UsableBytesInSegment;

	if (bytesleft < XLOG_BLCKSZ - SizeOfXLogLongPHD)
	{
		/* 适合段的第一个页面 */
		if (bytesleft == 0)
			seg_offset = 0;
		else
			seg_offset = bytesleft + SizeOfXLogLongPHD;
	}
	else
	{
		/* 考虑段中带有长头的第一个页面 */
		seg_offset = XLOG_BLCKSZ;
		bytesleft -= XLOG_BLCKSZ - SizeOfXLogLongPHD;

		fullpages = bytesleft / UsableBytesInPage;
		bytesleft = bytesleft % UsableBytesInPage;

		if (bytesleft == 0)
			seg_offset += fullpages * XLOG_BLCKSZ + bytesleft;
		else
			seg_offset += fullpages * XLOG_BLCKSZ + bytesleft + SizeOfXLogShortPHD;
	}

	XLogSegNoOffsetToRecPtr(fullsegs, seg_offset, wal_segment_size, result);

	return result;
}

/*
 * 将 XLogRecPtr 转换为"可用字节位置"。
 */
static uint64
XLogRecPtrToBytePos(XLogRecPtr ptr)
{
	uint64		fullsegs;
	uint32		fullpages;
	uint32		offset;
	uint64		result;

	XLByteToSeg(ptr, fullsegs, wal_segment_size);

	fullpages = (XLogSegmentOffset(ptr, wal_segment_size)) / XLOG_BLCKSZ;
	offset = ptr % XLOG_BLCKSZ;

	if (fullpages == 0)
	{
		result = fullsegs * UsableBytesInSegment;
		if (offset > 0)
		{
			Assert(offset >= SizeOfXLogLongPHD);
			result += offset - SizeOfXLogLongPHD;
		}
	}
	else
	{
		result = fullsegs * UsableBytesInSegment +
			(XLOG_BLCKSZ - SizeOfXLogLongPHD) + /* 考虑第一个页面 */
			(fullpages - 1) * UsableBytesInPage;	/* 完整页面 */
		if (offset > 0)
		{
			Assert(offset >= SizeOfXLogShortPHD);
			result += offset - SizeOfXLogShortPHD;
		}
	}

	return result;
}

/*
 * 初始化 XLOG 缓冲区，
 * 如果旧缓冲区仍包含未写入数据，则写出旧缓冲区，
 * 直到包含 'upto' 的页面。
 * 或者如果 'opportunistic' 为true，则在不写出未写入数据的情况下初始化尽可能多的页面。
 * 任何新页面都初始化为零，页面头正确初始化。
 */
static void
AdvanceXLInsertBuffer(XLogRecPtr upto, TimeLineID tli, bool opportunistic)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	int			nextidx;
	XLogRecPtr	OldPageRqstPtr;
	XLogwrtRqst WriteRqst;
	XLogRecPtr	NewPageEndPtr = InvalidXLogRecPtr;
	XLogRecPtr	NewPageBeginPtr;
	XLogPageHeader NewPage;
	int			npages pg_attribute_unused() = 0;

	LWLockAcquire(WALBufMappingLock, LW_EXCLUSIVE);

	/*
	 * 现在我们持有锁，
	 * 检查是否有人已经初始化了页面。
	 */
	while (upto >= XLogCtl->InitializedUpTo || opportunistic)
	{
		nextidx = XLogRecPtrToBufIdx(XLogCtl->InitializedUpTo);

		/*
		 * Get ending-offset of the buffer page we need to replace (this may
		 * be zero if the buffer hasn't been used yet).  Fall through if it's
		 * already written out.
		 */
		OldPageRqstPtr = XLogCtl->xlblocks[nextidx];
		if (LogwrtResult.Write < OldPageRqstPtr)
		{
			/*
			 * 不，有工作要做。如果我们只是想在不刷盘的情况下
			 * 预先初始化尽可能多的内容，现在就放弃。
			 */
			if (opportunistic)
				break;

			/* 在等待之前，获取 info_lck 并更新 LogwrtResult */
			SpinLockAcquire(&XLogCtl->info_lck);
			if (XLogCtl->LogwrtRqst.Write < OldPageRqstPtr)
				XLogCtl->LogwrtRqst.Write = OldPageRqstPtr;
			LogwrtResult = XLogCtl->LogwrtResult;
			SpinLockRelease(&XLogCtl->info_lck);

			/*
			 * 现在我们有了最新的 LogwrtResult 值，看看我们
			 * 是否仍然需要写入它，或者其他人已经完成了。
			 */
			if (LogwrtResult.Write < OldPageRqstPtr)
			{
				/*
				 * 必须获取写锁。先释放 WALBufMappingLock，
				 * 以确保我们需要等待的所有插入
				 * 可以完成（直到同一位置）。否则我们有
				 * 死锁的风险。
				 */
				LWLockRelease(WALBufMappingLock);

				WaitXLogInsertionsToFinish(OldPageRqstPtr);

				LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);

				LogwrtResult = XLogCtl->LogwrtResult;
				if (LogwrtResult.Write >= OldPageRqstPtr)
				{
					/* OK，有人已经写入了 */
					LWLockRelease(WALWriteLock);
				}
				else
				{
					/* 我们必须自己写入 */
					TRACE_POSTGRESQL_WAL_BUFFER_WRITE_DIRTY_START();
					WriteRqst.Write = OldPageRqstPtr;
					WriteRqst.Flush = 0;
					XLogWrite(WriteRqst, tli, false);
					LWLockRelease(WALWriteLock);
					PendingWalStats.wal_buffers_full++;
					TRACE_POSTGRESQL_WAL_BUFFER_WRITE_DIRTY_DONE();
				}
				/* 重新获取 WALBufMappingLock 并重试 */
				LWLockAcquire(WALBufMappingLock, LW_EXCLUSIVE);
				continue;
			}
		}

		/*
		 * 现在下一个缓冲区槽是空闲的，我们可以将其设置为
		 * 下一个输出页面。
		 */
		NewPageBeginPtr = XLogCtl->InitializedUpTo;
		NewPageEndPtr = NewPageBeginPtr + XLOG_BLCKSZ;

		Assert(XLogRecPtrToBufIdx(NewPageBeginPtr) == nextidx);

		NewPage = (XLogPageHeader) (XLogCtl->pages + nextidx * (Size) XLOG_BLCKSZ);

		/*
		 * 确保重新将缓冲区清零，这样我们写入之后
		 * 的字节看起来像零而不是有效的 XLOG 记录...
		 */
		MemSet((char *) NewPage, 0, XLOG_BLCKSZ);

		/*
		 * 填充新页面的头
		 */
		NewPage->xlp_magic = XLOG_PAGE_MAGIC;

		/* NewPage->xlp_info = 0; */	/* 由 memset 完成 */
		NewPage->xlp_tli = tli;
		NewPage->xlp_pageaddr = NewPageBeginPtr;

		/* NewPage->xlp_rem_len = 0; */	/* 由 memset 完成 */

		/*
		 * 如果在线备份未在进行中，
		 * 标记头以指示从此页面开始的 WAL 记录具有可移除的备份块。
		 * 这允许 WAL 归档器知道是否可以通过将全块记录转换为非全块格式来安全地压缩归档的
		 * WAL 数据。
		 * 在页面级别记录这就足够了，
		 * 因为我们在开始备份时强制进行页面切换（实际上是段切换），
		 * 所以在备份期间写入任何记录之前该标志将是关闭的。
		 * 在备份结束时，
		 * 最后一页将被标记为全部不安全，
		 * 而实际上可能只有部分不安全，
		 * 但最坏的情况是归档器会错过压缩几条记录的机会。
		 */
		if (Insert->runningBackups == 0)
			NewPage->xlp_info |= XLP_BKP_REMOVABLE;

		/*
		 * 如果是 XLOG 段文件的第一页，使其成为长头。
		 */
		if ((XLogSegmentOffset(NewPage->xlp_pageaddr, wal_segment_size)) == 0)
		{
			XLogLongPageHeader NewLongPage = (XLogLongPageHeader) NewPage;

			NewLongPage->xlp_sysid = ControlFile->system_identifier;
			NewLongPage->xlp_seg_size = wal_segment_size;
			NewLongPage->xlp_xlog_blcksz = XLOG_BLCKSZ;
			NewPage->xlp_info |= XLP_LONG_HEADER;
		}

		/*
		 * 确保页面的初始化在 xlblocks 更新之前对其他人可见。
		 * GetXLogBuffer() 在不持有锁的情况下读取
		 * xlblocks。
		 */
		pg_write_barrier();

		*((volatile XLogRecPtr *) &XLogCtl->xlblocks[nextidx]) = NewPageEndPtr;

		XLogCtl->InitializedUpTo = NewPageEndPtr;

		npages++;
	}
	LWLockRelease(WALBufMappingLock);

#ifdef WAL_DEBUG
	if (XLOG_DEBUG && npages > 0)
	{
		elog(DEBUG1, "initialized %d pages, up to %X/%X",
			 npages, LSN_FORMAT_ARGS(NewPageEndPtr));
	}
#endif
}

/*
 * 根据 max_wal_size_mb 和 checkpoint_completion_target
 * 计算 CheckPointSegments。
 */
static void
CalculateCheckpointSegments(void)
{
	double		target;

	/*-------
	 * 计算触发检查点的距离，以避免
	 * 超过 max_wal_size_mb。这基于两个假设：
	 *
	 * a) 我们只保留一个检查点周期的 WAL（在 PG11 之前，
	 *    我们保留两个检查点周期的 WAL，
	 *    以便在第一个检查点失败时可以从辅助检查点恢复，
	 *    但我们只在主库上这样做，
	 *    而不是在备服务器上。
	 *    只保留一个检查点简化了处理并减少了许多小型数据库的磁盘空间。
	 * ）b) 在检查点期间，
	 *	  我们消耗 checkpoint_completion_target *检查点之间消耗的段数。
	 * -------
	 */
	target = (double) ConvertToXSegs(max_wal_size_mb, wal_segment_size) /
		(1.0 + CheckPointCompletionTarget);

	/* 向下取整 */
	CheckPointSegments = (int) target;

	if (CheckPointSegments < 1)
		CheckPointSegments = 1;
}

void
assign_max_wal_size(int newval, void *extra)
{
	max_wal_size_mb = newval;
	CalculateCheckpointSegments();
}

void
assign_checkpoint_completion_target(double newval, void *extra)
{
	CheckPointCompletionTarget = newval;
	CalculateCheckpointSegments();
}

/*
 * 在检查点时，有多少 WAL 段要回收为预分配的未来
 * XLOG 段？返回应该预分配的最高段号。
 */
static XLogSegNo
XLOGfileslop(XLogRecPtr lastredoptr)
{
	XLogSegNo	minSegNo;
	XLogSegNo	maxSegNo;
	double		distance;
	XLogSegNo	recycleSegNo;

	/*
	 * 计算 min_wal_size_mb 和 max_wal_size_mb 对应的段号。
	 * 始终回收足够的段以满足最小值，
	 * 并删除足够的段以保持在最大值以下。
	 */
	minSegNo = lastredoptr / wal_segment_size +
		ConvertToXSegs(min_wal_size_mb, wal_segment_size) - 1;
	maxSegNo = lastredoptr / wal_segment_size +
		ConvertToXSegs(max_wal_size_mb, wal_segment_size) - 1;

	/*
	 * 在这些限制之间，回收足够的段以使我们能够
	 * 完成到下一个检查点的估计结束。
	 *
	 * 为了估计下一个检查点将在哪里结束，
	 * 假设系统稳定地消耗每个检查点之间的 CheckPointDistanceEstimate
	 * 字节。
	 */
	distance = (1.0 + CheckPointCompletionTarget) * CheckPointDistanceEstimate;
	/* 增加 10% 作为缓冲。 */
	distance *= 1.10;

	recycleSegNo = (XLogSegNo) ceil(((double) lastredoptr + distance) /
									wal_segment_size);

	if (recycleSegNo < minSegNo)
		recycleSegNo = minSegNo;
	if (recycleSegNo > maxSegNo)
		recycleSegNo = maxSegNo;

	return recycleSegNo;
}

/*
 * 检查我们是否消耗了足够的xlog空间以至于需要检查点。
 *
 * new_segno 表示刚刚填满的日志文件（或在恢复
 * 期间读取）。我们测量从 RedoRecPtr 到 new_segno
 * 的距离，并查看是否超过了 CheckPointSegments。
 *
 * 注意：确保 RedoRecPtr 是最新的，这是调用者的责任。
 */
bool
XLogCheckpointNeeded(XLogSegNo new_segno)
{
	XLogSegNo	old_segno;

	XLByteToSeg(RedoRecPtr, old_segno, wal_segment_size);

	if (new_segno >= old_segno + (uint64) (CheckPointSegments - 1))
		return true;
	return false;
}

/*
 * 写入和/或 fsync 日志至少到 WriteRqst 指示的位置。
 *
 * 如果 flexible == true，我们不必写入到 WriteRqst，
 * 但可以在任何方便的边界（如缓存或日志文件边界）停止。
 * 此选项允许我们避免在单次写入就足够时
 * 不必要地发出多次写入。
 *
 * 必须在持有 WALWriteLock 的情况下调用。在获取锁之前，
 * 必须调用 WaitXLogInsertionsToFinish(WriteRqst)，
 * 以确保数据准备好写入。
 */
static void
XLogWrite(XLogwrtRqst WriteRqst, TimeLineID tli, bool flexible)
{
	bool		ispartialpage;
	bool		last_iteration;
	bool		finishing_seg;
	int			curridx;
	int			npages;
	int			startidx;
	uint32		startoffset;

	/* 我们应该始终在关键区域内 */
	Assert(CritSectionCount > 0);

	/*
	 * 更新本地 LogwrtResult（调用者可能已经做了，但...）
	 */
	LogwrtResult = XLogCtl->LogwrtResult;

	/*
	 * 由于 xlog 缓存中的连续页面是连续分配的，
	 * 我们通常可以将多个页面聚集在一起并只发出一次
	 * write() 调用。npages 是我们确定可以
	 * 一起写入的页面数；startidx 是第一个页面的
	 * 缓存块索引，startoffset 是它应该去的文件偏移量。
	 * 后两个变量只有在 npages > 0 时才有效，但我们必须
	 * 初始化所有变量以保持编译器安静。
	 */
	npages = 0;
	startidx = 0;
	startoffset = 0;

	/*
	 * 在循环内，curridx 是要考虑写入的页面的缓存块索引。
	 * 从包含下一个未写入页面的缓冲区开始，
	 * 或最后一个部分写入的页面。
	 */
	curridx = XLogRecPtrToBufIdx(LogwrtResult.Write);

	while (LogwrtResult.Write < WriteRqst.Write)
	{
		/*
		 * 确保我们没有超过插入进程。如果我们被传递了一个
		 * 超过 AdvanceXLInsertBuffer 初始化的最后一页末尾的
		 * 伪造 WriteRqst.Write，这可能会发生。
		 */
		XLogRecPtr	EndPtr = XLogCtl->xlblocks[curridx];

		if (LogwrtResult.Write >= EndPtr)
			elog(PANIC, "xlog write request %X/%X is past end of log %X/%X",
				 LSN_FORMAT_ARGS(LogwrtResult.Write),
				 LSN_FORMAT_ARGS(EndPtr));

		/* 将 LogwrtResult.Write 前进到当前缓冲区页面的末尾 */
		LogwrtResult.Write = EndPtr;
		ispartialpage = WriteRqst.Write < LogwrtResult.Write;

		if (!XLByteInPrevSeg(LogwrtResult.Write, openLogSegNo,
							 wal_segment_size))
		{
			/*
			 * 切换到新的日志文件段。我们这里不能有任何
			 * 挂起的页面（因为我们在段末转储我们有的内容）。
			 */
			Assert(npages == 0);
			if (openLogFile >= 0)
				XLogFileClose();
			XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
							wal_segment_size);
			openLogTLI = tli;

			/* 创建/使用新的日志文件 */
			openLogFile = XLogFileInit(openLogSegNo, tli);
			ReserveExternalFD();
		}

		/* 确保我们打开了当前的日志文件 */
		if (openLogFile < 0)
		{
			XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
							wal_segment_size);
			openLogTLI = tli;
			openLogFile = XLogFileOpen(openLogSegNo, tli);
			ReserveExternalFD();
		}

		/* 将当前页面添加到待转储页面集合中 */
		if (npages == 0)
		{
			/* 组中的第一个 */
			startidx = curridx;
			startoffset = XLogSegmentOffset(LogwrtResult.Write - XLOG_BLCKSZ,
											wal_segment_size);
		}
		npages++;

		/*
		 * 如果这是最后一次循环迭代，或者如果我们处于
		 * 缓存区域的最后一页（因为下一页在内存中将不连续），
		 * 或者如果我们处于日志文件段的末尾，
		 * 则转储该集合。
		 */
		last_iteration = WriteRqst.Write <= LogwrtResult.Write;

		finishing_seg = !ispartialpage &&
			(startoffset + npages * XLOG_BLCKSZ) >= wal_segment_size;

		if (last_iteration ||
			curridx == XLogCtl->XLogCacheBlck ||
			finishing_seg)
		{
			char	   *from;
			Size		nbytes;
			Size		nleft;
			int			written;
			instr_time	start;

			/* 可以写入页面了 */
			from = XLogCtl->pages + startidx * (Size) XLOG_BLCKSZ;
			nbytes = npages * (Size) XLOG_BLCKSZ;
			nleft = nbytes;
			do
			{
				errno = 0;

				/* 测量写入 WAL 数据的 I/O 时间 */
				if (track_wal_io_timing)
					INSTR_TIME_SET_CURRENT(start);
				else
					INSTR_TIME_SET_ZERO(start);

				pgstat_report_wait_start(WAIT_EVENT_WAL_WRITE);
				written = pg_pwrite(openLogFile, from, nleft, startoffset);
				pgstat_report_wait_end();

				/*
				 * 累加 I/O
				 * 时间和 WAL 数据写入磁盘的次数。
				 */
				if (track_wal_io_timing)
				{
					instr_time	duration;

					INSTR_TIME_SET_CURRENT(duration);
					INSTR_TIME_ACCUM_DIFF(PendingWalStats.wal_write_time, duration, start);
				}

				PendingWalStats.wal_write++;

				if (written <= 0)
				{
					char		xlogfname[MAXFNAMELEN];
					int			save_errno;

					if (errno == EINTR)
						continue;

					save_errno = errno;
					XLogFileName(xlogfname, tli, openLogSegNo,
								 wal_segment_size);
					errno = save_errno;
					ereport(PANIC,
							(errcode_for_file_access(),
							 errmsg("could not write to log file %s "
									"at offset %u, length %zu: %m",
									xlogfname, startoffset, nleft)));
				}
				nleft -= written;
				from += written;
				startoffset += written;
			} while (nleft > 0);

			npages = 0;

			/*
			 * 如果我们刚刚写完了日志文件段的最后一页，
			 * 立即 fsync 该段。
			 * 这避免了稍后当 fsync 
			 * 请求到来时必须返回并重新打开之前的段。
			 * 在这里执行确保只有一个后端将执行此 fsync。
			 *
			 * 这也是通知归档器该段已准备好复制到归档存储、更新
			 * archive_timeout
			 * 计时器、
			 * 以及如果自上次检查点以来使用了太多日
			 * 志文件段则发出检查点信号的正确位置。
			 */
			if (finishing_seg)
			{
				issue_xlog_fsync(openLogFile, openLogSegNo, tli);

				/* 信号表示我们稍后需要唤醒 walsenders */
				WalSndWakeupRequest();

				LogwrtResult.Flush = LogwrtResult.Write;	/* 页面末尾 */

				if (XLogArchivingActive())
					XLogArchiveNotifySeg(openLogSegNo, tli);

				XLogCtl->lastSegSwitchTime = (pg_time_t) time(NULL);
				XLogCtl->lastSegSwitchLSN = LogwrtResult.Flush;

				/*
				 * 如果自上次检查点以来我们消耗了太多 xlog，
				 * 请求检查点。
				 * 为了速度，我们首先使用 RedoRecPtr 的本地副本检查，
				 * 这可能已过期；
				 * 如果看起来需要检查点，强制更新 RedoRecPtr 并重新检查。
				 */
				if (IsUnderPostmaster && XLogCheckpointNeeded(openLogSegNo))
				{
					(void) GetRedoRecPtr();
					if (XLogCheckpointNeeded(openLogSegNo))
						RequestCheckpoint(CHECKPOINT_CAUSE_XLOG);
				}
			}
		}

		if (ispartialpage)
		{
			/* 只被要求写入部分页面 */
			LogwrtResult.Write = WriteRqst.Write;
			break;
		}
		curridx = NextBufIdx(curridx);

		/* 如果 flexible，一旦我们写入了内容就跳出循环 */
		if (flexible && npages == 0)
			break;
	}

	Assert(npages == 0);

	/*
	 * 如果被要求刷新，则执行
	 */
	if (LogwrtResult.Flush < WriteRqst.Flush &&
		LogwrtResult.Flush < LogwrtResult.Write)
	{
		/*
		 * 可能不经过上面的循环就到达这里，在这种情况下我们可能
		 * 没有打开文件或打开了错误的文件。但是，我们不需要
		 * fsync 超过一个文件。
		 */
		if (sync_method != SYNC_METHOD_OPEN &&
			sync_method != SYNC_METHOD_OPEN_DSYNC)
		{
			if (openLogFile >= 0 &&
				!XLByteInPrevSeg(LogwrtResult.Write, openLogSegNo,
								 wal_segment_size))
				XLogFileClose();
			if (openLogFile < 0)
			{
				XLByteToPrevSeg(LogwrtResult.Write, openLogSegNo,
								wal_segment_size);
				openLogTLI = tli;
				openLogFile = XLogFileOpen(openLogSegNo, tli);
				ReserveExternalFD();
			}

			issue_xlog_fsync(openLogFile, openLogSegNo, tli);
		}

		/* 信号表示我们稍后需要唤醒 walsenders */
		WalSndWakeupRequest();

		LogwrtResult.Flush = LogwrtResult.Write;
	}

	/*
	 * 更新共享内存状态
	 *
	 * 我们确保共享的'request'值不会落后于'result'值。
	 * 这不是绝对必要的，
	 * 但它在几个地方节省了一些代码。
	 */
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		XLogCtl->LogwrtResult = LogwrtResult;
		if (XLogCtl->LogwrtRqst.Write < LogwrtResult.Write)
			XLogCtl->LogwrtRqst.Write = LogwrtResult.Write;
		if (XLogCtl->LogwrtRqst.Flush < LogwrtResult.Flush)
			XLogCtl->LogwrtRqst.Flush = LogwrtResult.Flush;
		SpinLockRelease(&XLogCtl->info_lck);
	}
}

/*
 * 记录异步事务提交/中止的 LSN
 * 并在 WALWriter 有工作要做时唤醒它。
 * （这不应用于同步提交。）
 */
void
XLogSetAsyncXactLSN(XLogRecPtr asyncXactLSN)
{
	XLogRecPtr	WriteRqstPtr = asyncXactLSN;
	bool		sleeping;

	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	sleeping = XLogCtl->WalWriterSleeping;
	if (XLogCtl->asyncXactLSN < asyncXactLSN)
		XLogCtl->asyncXactLSN = asyncXactLSN;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * 如果 WALWriter 正在休眠，我们应该踢它让它从
	 * 低功耗模式中出来。否则，确定是否有一整页的
	 * WAL 可写入。
	 */
	if (!sleeping)
	{
		/* 插入到缓冲区的 WAL 总是在写入的 WAL 之前 */
		WriteRqstPtr -= WriteRqstPtr % XLOG_BLCKSZ;

		/* 后退到最后完成的页面边界 */
		if (WriteRqstPtr <= LogwrtResult.Flush)
			return;
	}

	/*
	 * 唤醒 WALWriter：它有一整页的 WAL 要写入，或者我们希望它
	 * 从低功耗模式中出来，以便此异步提交在预期的
	 * 时间内到达磁盘。
	 */
	if (ProcGlobal->walwriterLatch)
		SetLatch(ProcGlobal->walwriterLatch);
}

/*
 * 记录我们可以删除 WAL 到哪个 LSN，因为任何
 * 复制槽都不需要它。
 */
void
XLogSetReplicationSlotMinimumLSN(XLogRecPtr lsn)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->replicationSlotMinLSN = lsn;
	SpinLockRelease(&XLogCtl->info_lck);
}


/*
 * 返回我们必须保留以满足某些复制槽需求的最早
 * LSN。
 */
static XLogRecPtr
XLogGetReplicationSlotMinimumLSN(void)
{
	XLogRecPtr	retval;

	SpinLockAcquire(&XLogCtl->info_lck);
	retval = XLogCtl->replicationSlotMinLSN;
	SpinLockRelease(&XLogCtl->info_lck);

	return retval;
}

/*
 * 在控制文件中推进 minRecoveryPoint。
 *
 * 如果我们在恢复期间崩溃，我们必须在数据库一致之前
 * 再次到达此点。
 *
 * 如果 'force' 为 true，则忽略 'lsn' 参数。否则，只有当
 * minRecoveryPoint 尚未大于或等于 'lsn' 时才更新它。
 */
static void
UpdateMinRecoveryPoint(XLogRecPtr lsn, bool force)
{
	/* 使用我们对该变量的本地副本进行快速检查 */
	if (!updateMinRecoveryPoint || (!force && lsn <= LocalMinRecoveryPoint))
		return;

	/*
	 * 无效的 minRecoveryPoint 意味着我们需要恢复所有 WAL，
	 * 即我们正在进行崩溃恢复。
	 * 在这种情况下我们从不修改控制文件的值，
	 * 所以我们也可以在此处短路未来的检查。
	 * minRecoveryPoint 和minRecoveryPointTLI 的本地值在崩溃恢复完成之前不应更新。
	 * 我们只对启动进程这样做，
	 * 因为它在完成崩溃恢复之前不应更新自己的minRecoveryPoint 引用，
	 * 以确保在这种情况下重放所有可用的 WAL。
	 * 这也避免了启动进程对控制文件进行额外的锁定。
	 */
	if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint) && InRecovery)
	{
		updateMinRecoveryPoint = false;
		return;
	}

	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

	/* 更新本地副本 */
	LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
	LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;

	if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint))
		updateMinRecoveryPoint = false;
	else if (force || LocalMinRecoveryPoint < lsn)
	{
		XLogRecPtr	newMinRecoveryPoint;
		TimeLineID	newMinRecoveryPointTLI;

		/*
		 * 为了避免过于频繁地更新控制文件，
		 * 我们将它一直更新到正在重放的最后一条记录，
		 * 尽管 'lsn' 对于正确性来说就足够了。
		 * 这也允许 'force' 情况不需要有效的 'lsn' 值。
		 *
		 * 这样做的另一个重要原因是，
		 * 如果调用者从损坏的堆页面获得了传递的 'lsn' 值，它可能是伪造的，
		 * 即超过了可用 WAL 的末尾。
		 * 接受这样的值作为最小恢复点将阻止我们启动。相反，
		 * 我们只是记录警告并继续恢复。
		 * （另请参阅 XLogFlush 中关于损坏 LSN 的注释。）
		 */
		newMinRecoveryPoint = GetCurrentReplayRecPtr(&newMinRecoveryPointTLI);
		if (!force && newMinRecoveryPoint < lsn)
			elog(WARNING,
				 "xlog min recovery request %X/%X is past current point %X/%X",
				 LSN_FORMAT_ARGS(lsn), LSN_FORMAT_ARGS(newMinRecoveryPoint));

		/* 更新控制文件 */
		if (ControlFile->minRecoveryPoint < newMinRecoveryPoint)
		{
			ControlFile->minRecoveryPoint = newMinRecoveryPoint;
			ControlFile->minRecoveryPointTLI = newMinRecoveryPointTLI;
			UpdateControlFile();
			LocalMinRecoveryPoint = newMinRecoveryPoint;
			LocalMinRecoveryPointTLI = newMinRecoveryPointTLI;

			ereport(DEBUG2,
					(errmsg_internal("updated min recovery point to %X/%X on timeline %u",
									 LSN_FORMAT_ARGS(newMinRecoveryPoint),
									 newMinRecoveryPointTLI)));
		}
	}
	LWLockRelease(ControlFileLock);
}

/*
 * 确保所有到给定位置的 XLOG 数据都已刷盘到磁盘。
 *
 * 注意：这与 XLogWrite 的主要区别在于 WALWriteLock 尚未持有，
 * 并且我们尝试尽可能避免获取它。
 */
void
XLogFlush(XLogRecPtr record)
{
	XLogRecPtr	WriteRqstPtr;
	XLogwrtRqst WriteRqst;
	TimeLineID	insertTLI = XLogCtl->InsertTimeLineID;

	/*
	 * 在 REDO 期间，我们在读取而不是写入 WAL。因此，与其
	 * 尝试刷盘 WAL，我们应该更新 minRecoveryPoint。我们
	 * 测试 XLogInsertAllowed() 而不是 InRecovery，因为我们也需要
	 * 检查点进程以这种方式行动，而且当它尝试写入
	 * 恢复结束检查点时，它确实应该刷新。
	 */
	if (!XLogInsertAllowed())
	{
		UpdateMinRecoveryPoint(record, false);
		return;
	}

	/* 如果已知已刷盘，快速退出 */
	if (record <= LogwrtResult.Flush)
		return;

#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
		elog(LOG, "xlog flush request %X/%X; write %X/%X; flush %X/%X",
			 LSN_FORMAT_ARGS(record),
			 LSN_FORMAT_ARGS(LogwrtResult.Write),
			 LSN_FORMAT_ARGS(LogwrtResult.Flush));
#endif

	START_CRIT_SECTION();

	/*
	 * 由于 fsync 通常是一个非常昂贵的操作，
	 * 我们尝试在每次 fsync 上尽可能多地携带数据：
	 * 如果我们看到更多数据进入 xlog 缓冲区，
	 * 我们也会写入和 fsync 它们，
	 * 以便 LogwrtResult.Flush 的最终值尽可能大。这给我们一些机会避免紧接着再进行一次 fsync。
	 */

	/* 初始化为给定目标；可能在下面增加 */
	WriteRqstPtr = record;

	/*
	 * 现在等待直到我们获得写锁，或者其他人为我们
	 * 完成了刷盘。
	 */
	for (;;)
	{
		XLogRecPtr	insertpos;

		/* 读取 LogwrtResult 并更新本地状态 */
		SpinLockAcquire(&XLogCtl->info_lck);
		if (WriteRqstPtr < XLogCtl->LogwrtRqst.Write)
			WriteRqstPtr = XLogCtl->LogwrtRqst.Write;
		LogwrtResult = XLogCtl->LogwrtResult;
		SpinLockRelease(&XLogCtl->info_lck);

		/* 已经完成了？ */
		if (record <= LogwrtResult.Flush)
			break;

		/*
		 * 在实际执行写入之前，等待所有针对我们即将写入的页的
		 * 进行中的插入完成。
		 */
		insertpos = WaitXLogInsertionsToFinish(WriteRqstPtr);

		/*
		 * 尝试获取写锁。若不能立即获得，则等待
		 * 其释放，并重新检查我们是否仍需刷盘，
		 * 或者持有锁的后端是否已替我们完成。这
		 * 有助于在系统受 fsync 速度瓶颈制约时保持良好的组提交
		 * 速率。
		 */
		if (!LWLockAcquireOrWait(WALWriteLock, LW_EXCLUSIVE))
		{
			/*
			 * 在实际执行写入之前，
			 * 等待所有正在进行的到我们
			 * 要写入的页面的插入完成。
			 */
			continue;
		}

		/* 获得了锁；重新检查请求是否已满足 */
		LogwrtResult = XLogCtl->LogwrtResult;
		if (record <= LogwrtResult.Flush)
		{
			LWLockRelease(WALWriteLock);
			break;
		}

		/*
		 * 刷新前休眠！通过在此添加延迟，
		 * 我们可能会给更多后端机会加入组提交追随者的队列；
		 * 这可以显著提高事务吞吐量，
		 * 但代价是增加事务延迟。
		 *
		 * 如果 enableFsync 未开启，或者活动事务的其他后端
		 * 少于 CommitSiblings，我们不休眠。
		 */
		if (CommitDelay > 0 && enableFsync &&
			MinimumActiveBackends(CommitSiblings))
		{
			pg_usleep(CommitDelay);

			/*
			 * 重新检查我们现在可以刷盘 WAL 到多远。
			 * 通常在持有WALWriteLock 时调用 WaitXLogInsertionsToFinish
			 * 是不安全的，
			 * 因为正在进行的插入可能也需要获取 WALWriteLock 才能继续。
			 * 但我们知道到 insertpos 的所有插入已经完成，
			 * 因为这就是之前的 WaitXLogInsertionsToFinish() 返回的结果。
			 * 我们再次调用它只是为了让 insertpos 进一步向前移动，
			 * 而不是实际等待任何人。
			 */
			insertpos = WaitXLogInsertionsToFinish(insertpos);
		}

		/* 尝试也写入/刷新 XLOG 的后续添加 */
		WriteRqst.Write = insertpos;
		WriteRqst.Flush = insertpos;

		XLogWrite(WriteRqst, insertTLI, false);

		LWLockRelease(WALWriteLock);
		/* 完成 */
		break;
	}

	END_CRIT_SECTION();

	/* 现已释放高竞争锁，唤醒 walsenders */
	WalSndWakeupProcessRequests(true, !RecoveryInProgress());

	/*
	 * 如果我们仍然没有刷新到请求点，那么我们有
	 * 问题；最可能的是，请求的刷新点超过了 XLOG 的末尾。
	 * 这种情况在磁盘页面有损坏的 LSN 时发生过。
	 *
	 * 以前我们将其视为 PANIC 条件，
	 * 但这损害了系统的健壮性而不是帮助它：
	 * 我们不想因为一个数据页上的损坏而关闭整个系统。
	 * 特别是，
	 * 如果在恢复期间再次遇到坏页面，
	 * 我们将完全无法重新启动数据库！（这种情况实际上在 7.1 版本的现场发生过几次。
	 * ）从 8.4 开始，
	 * 恢复期间遇到的坏 LSN 是UpdateMinRecoveryPoint 的问题；
	 * 我们在恢复期间能到达这里的唯一时间是刷新恢复结束检查点记录时，
	 * 我们不期望它有坏的 LSN。
	 *
	 * 注意，对于来自 xact.c 的调用，ERROR 将被提升为 PANIC，
	 * 因为 xact.c 在关键区域内调用此例程。但是，
	 * 来自 bufmgr.c 的调用不在关键区域内，所以我们不会
	 * 因为数据页面上的坏 LSN 而强制重启。
	 */
	if (LogwrtResult.Flush < record)
		elog(ERROR,
			 "xlog flush request %X/%X is not satisfied --- flushed only to %X/%X",
			 LSN_FORMAT_ARGS(record),
			 LSN_FORMAT_ARGS(LogwrtResult.Flush));
}

/*
 * 写入和刷新 xlog，但不指定确切位置。
 *
 * 我们通常只写入完整的块；
 * 但如果在此基础上没有事情可做，我们会检查当前不完整块中未写入的异步提交，
 * 并写入到其中最新的一条。因此，
 * 如果不使用异步提交，我们只会写入完整的块。
 *
 * 如果基于上述判断有内容需要写入，我们会立即执行。但为了避免
 * 以影响并发 I/O 的速率调用 fsync、fdatasync 等，我们只每隔
 * wal_writer_delay 毫秒刷新一次 WAL，或者当有超过 wal_writer_flush_after
 * 个未刷盘的块时才刷新。
 *
 * 我们可以保证异步提交最多在三个 wal_writer_delay 周期后到达磁盘。
 * （当刷新完整块时，
 * 我们允许 XLogWrite "灵活"写入，这意味着它可以在缓冲区环的末尾停止；
 * 这只有在非常高负载或长时间 wal_writer_delay 时才会有区别，
 * 但对于异步提交的最坏情况会多一个周期。）
 *
 * 此例程由后台 walwriter 进程定期调用。
 *
 * 如果有任何工作要做，则返回 true，即使我们因 wal_writer_delay/
 * wal_writer_flush_after 而跳过了刷新。
 */
bool
XLogBackgroundFlush(void)
{
	XLogwrtRqst WriteRqst;
	bool		flexible = true;
	static TimestampTz lastflush;
	TimestampTz now;
	int			flushbytes;
	TimeLineID	insertTLI;

	/* XLOG 在恢复期间不需要刷新 */
	if (RecoveryInProgress())
		return false;

	/*
	 * 由于我们不在恢复中，InsertTimeLineID 已设置且不会改变，
	 * 所以我们可以在不持有锁的情况下读取它。
	 */
	insertTLI = XLogCtl->InsertTimeLineID;

	/* 读取 LogwrtResult 并更新本地状态 */
	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	WriteRqst = XLogCtl->LogwrtRqst;
	SpinLockRelease(&XLogCtl->info_lck);

	/* 回退到最后一个完整页边界 */
	WriteRqst.Write -= WriteRqst.Write % XLOG_BLCKSZ;

	/* 后退到最后完成的页面边界 */
	if (WriteRqst.Write <= LogwrtResult.Flush)
	{
		SpinLockAcquire(&XLogCtl->info_lck);
		WriteRqst.Write = XLogCtl->asyncXactLSN;
		SpinLockRelease(&XLogCtl->info_lck);
		flexible = false;		/* 如果我们已经刷新到那个位置，考虑异步提交记录 */
	}

	/*
	 * 若已知已刷盘，我们就完成了。只需检查我们是否
	 * 仍持有一个已不再使用的日志文件的打开文件句柄，
	 * 从而阻止该文件被删除。
	 */
	if (WriteRqst.Write <= LogwrtResult.Flush)
	{
		if (openLogFile >= 0)
		{
			if (!XLByteInPrevSeg(LogwrtResult.Write, openLogSegNo,
								 wal_segment_size))
			{
				XLogFileClose();
			}
		}
		return false;
	}

	/*
	 * 根据 wal_writer_delay 和 wal_writer_flush_after GUCs
	 * 确定刷盘 WAL 到多远。
	 */
	now = GetCurrentTimestamp();
	flushbytes =
		WriteRqst.Write / XLOG_BLCKSZ - LogwrtResult.Flush / XLOG_BLCKSZ;

	if (WalWriterFlushAfter == 0 || lastflush == 0)
	{
		/* 确保全部被写入 */
		WriteRqst.Flush = WriteRqst.Write;
		lastflush = now;
	}
	else if (TimestampDifferenceExceeds(lastflush, now, WalWriterDelay))
	{
		/*
		 * 至少每隔 WalWriterDelay
		 * 毫秒刷新写入。
		 * 这对于限制异步提交到达磁盘所需的时间很重要。
		 */
		WriteRqst.Flush = WriteRqst.Write;
		lastflush = now;
	}
	else if (flushbytes >= WalWriterFlushAfter)
	{
		/* 超过了 wal_writer_flush_after 个块，刷新 */
		WriteRqst.Flush = WriteRqst.Write;
		lastflush = now;
	}
	else
	{
		/* 首次调用，或基于块的限制已禁用 */
		WriteRqst.Flush = 0;
	}

#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
		elog(LOG, "xlog bg flush request write %X/%X; flush: %X/%X, current is write %X/%X; flush %X/%X",
			 LSN_FORMAT_ARGS(WriteRqst.Write),
			 LSN_FORMAT_ARGS(WriteRqst.Flush),
			 LSN_FORMAT_ARGS(LogwrtResult.Write),
			 LSN_FORMAT_ARGS(LogwrtResult.Flush));
#endif

	START_CRIT_SECTION();

	/* 本次不刷新 */
	WaitXLogInsertionsToFinish(WriteRqst.Write);
	LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);
	LogwrtResult = XLogCtl->LogwrtResult;
	if (WriteRqst.Write > LogwrtResult.Write ||
		WriteRqst.Flush > LogwrtResult.Flush)
	{
		XLogWrite(WriteRqst, insertTLI, flexible);
	}
	LWLockRelease(WALWriteLock);

	END_CRIT_SECTION();

	/* 现在等待任何正在进行的插入完成并获取写锁 */
	WalSndWakeupProcessRequests(true, !RecoveryInProgress());

	/*
	 * 好的，完成了。为了减轻关键路径的工作量，尝试初始化尽可能多的
	 * 不再需要的 WAL 缓冲区以供将来使用。
	 */
	AdvanceXLInsertBuffer(InvalidXLogRecPtr, insertTLI, true);

	/*
	 * 若我们判定需要写入数据，但别人已经
	 * 写入/刷盘，则应视为仍在活动，以避免过早
	 * 进入休眠。
	 */
	return true;
}

/*
 * 测试 XLOG 数据是否已刷盘到（至少）给定位置。
 *
 * 如果仍需要刷新则返回 true。（不过，可能其他人已经在
 * 刷新到该位置的过程中。）
 */
bool
XLogNeedsFlush(XLogRecPtr record)
{
	/*
	 * 在恢复期间，
	 * 我们不刷盘 WAL 而是更新 minRecoveryPoint。
	 * 所以"需要刷新"被理解为是否需要更新 minRecoveryPoint。
	 */
	if (RecoveryInProgress())
	{
		/*
		 * 无效的 minRecoveryPoint 意味着我们需要恢复所有 WAL，
		 * 即我们正在进行崩溃恢复。
		 * 在这种情况下我们从不修改控制文件的值，
		 * 所以我们也可以在此处短路未来的检查。
		 * 这会触发启动进程的快速退出路径，
		 * 启动进程在进行崩溃恢复时不能更新其 minRecoveryPoint 的本地副本，直到它重放了所有可用的 WAL。
		 */
		if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint) && InRecovery)
			updateMinRecoveryPoint = false;

		/* 若已知已更新或无法更新则快速退出 */
		if (record <= LocalMinRecoveryPoint || !updateMinRecoveryPoint)
			return false;

		/*
		 * 更新 minRecoveryPoint 的本地副本。但若锁正忙，
		 * 则仅返回一个保守估计。
		 */
		if (!LWLockConditionalAcquire(ControlFileLock, LW_SHARED))
			return true;
		LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
		LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		LWLockRelease(ControlFileLock);

		/*
		 * 检查 minRecoveryPoint，适用于除正在做崩溃恢复的
		 * 启动进程以外的任何进程；崩溃恢复仍在运行时，启动
		 * 进程不应更新控制文件中的值。
		 */
		if (XLogRecPtrIsInvalid(LocalMinRecoveryPoint))
			updateMinRecoveryPoint = false;

		/* 如果已知已更新或无法更新，快速退出 */
		if (record <= LocalMinRecoveryPoint || !updateMinRecoveryPoint)
			return false;
		else
			return true;
	}

	/* 再次检查 */
	if (record <= LogwrtResult.Flush)
		return false;

	/* 读取 LogwrtResult 并更新本地状态 */
	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	SpinLockRelease(&XLogCtl->info_lck);

	/* 如果已知已刷盘，快速退出 */
	if (record <= LogwrtResult.Flush)
		return false;

	return true;
}

/*
 * 尝试使给定的 XLOG 文件段存在。
 *
 * logsegno: 标识段。
 *
 * *added: 返回时，如果此调用增加了现有段的数量，则为 true。
 *
 * path: 返回时，此 char[MAXPGPATH] 包含 logsegno 文件的路径。
 *
 * 返回 -1 或打开文件的 FD。这里的 -1 不是错误；
 * 想要打开段的调用者应尝试打开 "path"，这通常会成功。
 * （这很奇怪，但对调用者来说很高效。）
 */
static int
XLogFileInitInternal(XLogSegNo logsegno, TimeLineID logtli,
					 bool *added, char *path)
{
	char		tmppath[MAXPGPATH];
	XLogSegNo	installed_segno;
	XLogSegNo	max_segno;
	int			fd;
	int			save_errno;
	int			open_flags = O_RDWR | O_CREAT | O_EXCL | PG_BINARY;

	Assert(logtli != 0);

	XLogFilePath(path, logtli, logsegno, wal_segment_size);

	/*
	 * 再次检查
	 */
	*added = false;
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | O_CLOEXEC |
					   get_sync_bit(sync_method));
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
	}
	else
		return fd;

	/*
	 * 初始化一个空的（全零）段。注意：可能有
	 * 另一个进程在做同样的事。若如此，我们最终会
	 * 预先多创建一个额外的日志段。这似乎可以接受，且好过
	 * 在整个冗长过程中一直持有锁。
	 */
	elog(DEBUG2, "creating and filling new WAL file");

	snprintf(tmppath, MAXPGPATH, XLOGDIR "/xlogtemp.%d", (int) getpid());

	unlink(tmppath);

	if (io_direct_flags & IO_DIRECT_WAL_INIT)
		open_flags |= PG_O_DIRECT;

	/* 这里不使用 get_sync_bit() --- 希望只在填充结束时 fsync */
	fd = BasicOpenFile(tmppath, open_flags);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	pgstat_report_wait_start(WAIT_EVENT_WAL_INIT_WRITE);
	save_errno = 0;
	if (wal_init_zero)
	{
		ssize_t		rc;

		/*
		 * 零填充文件。
		 * 使用此设置时，我们用困难的方式来做，
		 * 以确保所有文件空间确实已分配。
		 * 在允许文件中有"空洞"的平台上，仅仅定位到文件末尾不会分配中间空间。
		 * 这样做，
		 * 我们知道我们拥有所有空间，并且（在下面的 fsync 之后）所有间接块都在磁盘上。
		 * 因此，fdatasync(2) 或 O_DSYNC 足以同步将来对日志文件的写入。
		 */
		rc = pg_pwrite_zeros(fd, wal_segment_size, 0);

		if (rc < 0)
			save_errno = errno;
	}
	else
	{
		/*
		 * 否则，定位到末尾并写入一个单独字节就
		 * 足够了。
		 */
		errno = 0;
		if (pg_pwrite(fd, "\0", 1, wal_segment_size - 1) != 1)
		{
			/* 若 write 未设置 errno，则假定磁盘空间不足 */
			save_errno = errno ? errno : ENOSPC;
		}
	}
	pgstat_report_wait_end();

	if (save_errno)
	{
		/*
		 * 否则，定位到末尾并写入一个单独的字节就足够了。
		 */
		unlink(tmppath);

		close(fd);

		errno = save_errno;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmppath)));
	}

	pgstat_report_wait_start(WAIT_EVENT_WAL_INIT_SYNC);
	if (pg_fsync(fd) != 0)
	{
		save_errno = errno;
		close(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	if (close(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	/*
	 * 现在用最终名称将段移入位置。要应对
	 * 别人在我们填充文件时已创建该文件的可能：
	 * 若如此，用我们的文件去预创建未来的日志段。
	 */
	installed_segno = logsegno;

	/*
	 * XXX:
	 *  我们应该用什么作为 max_segno？当 XLOGfileslop 是常量时我们曾经使用它，但这一直有点可疑：
	 * 通常，在检查点时，
	 * XLOGfileslop 是从检查点记录开始的偏移量，
	 * 但在这里，它是从插入位置开始的偏移量。
	 * 我们不能在这里做正常的XLOGfileslop 计算，因为我们要无法访问前一个检查点的重做位置。
	 * 所以有些随意地，直接使用 CheckPointSegments。
	 */
	max_segno = logsegno + CheckPointSegments;
	if (InstallXLogFileSegment(&installed_segno, tmppath, true, max_segno,
							   logtli))
	{
		*added = true;
		elog(DEBUG2, "done creating and filling new WAL file");
	}
	else
	{
		/*
		 * 不再需要未来的段，或者 InstallXLogFileSegment()
		 * 未能将文件重命名到指定位置。如果重命名失败，
		 * 调用者打开文件可能会失败。
		 */
		unlink(tmppath);
		elog(DEBUG2, "abandoned new WAL file");
	}

	return -1;
}

/*
 * 创建或打开一个新的 XLOG 文件段。
 *
 * logsegno: 标识要创建/打开的段。
 *
 * 返回打开文件的 FD。
 *
 * 注意：
 * 这里的错误是 ERROR 而不是 PANIC，因为我们可能处于也可能不处于关键区域（例如，
 * 在检查点期间没有理由因失败而关闭系统）。
 * 如果我们处于关键区域，它们将被提升为 PANIC。
 */
int
XLogFileInit(XLogSegNo logsegno, TimeLineID logtli)
{
	bool		ignore_added;
	char		path[MAXPGPATH];
	int			fd;

	Assert(logtli != 0);

	fd = XLogFileInitInternal(logsegno, logtli, &ignore_added, path);
	if (fd >= 0)
		return fd;

	/* 现在打开原来的目标段（可能不是我刚创建的文件） */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | O_CLOEXEC |
					   get_sync_bit(sync_method));
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	return fd;
}

/*
 * 通过复制现有段来创建新的 XLOG 文件段。
 *
 * destsegno: 标识要创建的段。
 *
 * srcTLI,
 *		 srcsegno: 标识要复制的段（可能来自不同的时间线）
 *
 * upto:
 *		 复制源文件的多少（其余部分用零填充）
 *
 * 目前这只在恢复期间使用，
 * 因此没有锁定考虑。
 * 但我们应该像 XLogFileInit 一样紧张，避免放置伪造文件。
 */
static void
XLogFileCopy(TimeLineID destTLI, XLogSegNo destsegno,
			 TimeLineID srcTLI, XLogSegNo srcsegno,
			 int upto)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	PGAlignedXLogBlock buffer;
	int			srcfd;
	int			fd;
	int			nbytes;

	/*
	 * 现在打开原始目标段（可能不是我刚刚创建的文件）
	 */
	XLogFilePath(path, srcTLI, srcsegno, wal_segment_size);
	srcfd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/*
	 * 复制到临时文件名。
	 */
	snprintf(tmppath, MAXPGPATH, XLOGDIR "/xlogtemp.%d", (int) getpid());

	unlink(tmppath);

	/* 这里不使用 get_sync_bit() --- 希望只在填充结束时 fsync */
	fd = OpenTransientFile(tmppath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	/*
	 * 复制到临时文件名。
	 */
	for (nbytes = 0; nbytes < wal_segment_size; nbytes += sizeof(buffer))
	{
		int			nread;

		nread = upto - nbytes;

		/*
		 * 未从源文件读取的部分用
		 * 零填充。
		 */
		if (nread < sizeof(buffer))
			memset(buffer.data, 0, sizeof(buffer));

		if (nread > 0)
		{
			int			r;

			if (nread > sizeof(buffer))
				nread = sizeof(buffer);
			pgstat_report_wait_start(WAIT_EVENT_WAL_COPY_READ);
			r = read(srcfd, buffer.data, nread);
			if (r != nread)
			{
				if (r < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not read file \"%s\": %m",
									path)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("could not read file \"%s\": read %d of %zu",
									path, r, (Size) nread)));
			}
			pgstat_report_wait_end();
		}
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_WAL_COPY_WRITE);
		if ((int) write(fd, buffer.data, sizeof(buffer)) != (int) sizeof(buffer))
		{
			int			save_errno = errno;

			/*
	 * 进行数据复制。
	 */
			unlink(tmppath);
			/* * 未从源文件读取的部分用零填充。 */
			errno = save_errno ? save_errno : ENOSPC;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tmppath)));
		}
		pgstat_report_wait_end();
	}

	pgstat_report_wait_start(WAIT_EVENT_WAL_COPY_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	if (CloseTransientFile(srcfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	/*
	 * 现在用最终名称将段移入位置。
	 */
	if (!InstallXLogFileSegment(&destsegno, tmppath, false, 0, destTLI))
		elog(ERROR, "InstallXLogFileSegment should not have failed");
}

/*
 * 安装新的 XLOG 段文件作为当前或未来的日志段。
 *
 * 这用于安装新创建的段（创建时有临时文件名）
 * 以及回收旧段。
 *
 * *segno: 标识要安装为的段（或第一个可能的目标）。
 * 当 find_free 为 true 时，返回时修改此值以指示
 * 实际安装位置或最后搜索的段。
 *
 * tmppath: 要安装的文件的初始名称。它将被重命名到目标位置。
 *
 * find_free: 如果为 true，在传递的段号或之后安装新段
 * 到第一个空的段号位置。如果为 false，将新段安装在
 * 指定的确切位置，删除那里任何现有的段文件。
 *
 * max_segno: 安装新文件的最大段号。如果在 *segno 和
 * max_segno 之间找不到空闲槽位则失败。（当 find_free 为
 * false 时忽略。）
 *
 * tli: 应该安装新段的时间线。
 *
 * 如果文件安装成功则返回 true。false 表示
 * 超过了 max_segno 限制，启动进程已禁用此
 * 功能，或者在将文件重命名到指定位置时发生错误。
 */
static bool
InstallXLogFileSegment(XLogSegNo *segno, char *tmppath,
					   bool find_free, XLogSegNo max_segno, TimeLineID tli)
{
	char		path[MAXPGPATH];
	struct stat stat_buf;

	Assert(tli != 0);

	XLogFilePath(path, tli, *segno, wal_segment_size);

	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	if (!XLogCtl->InstallXLogFileSegmentActive)
	{
		LWLockRelease(ControlFileLock);
		return false;
	}

	if (!find_free)
	{
		/* 如果写入没有设置 errno，假设问题是磁盘空间不足 */
		durable_unlink(path, DEBUG1);
	}
	else
	{
		/* 强制安装：删除任何预先存在的段文件 */
		while (stat(path, &stat_buf) == 0)
		{
			if ((*segno) >= max_segno)
			{
				/* 找一个空闲槽位放进去 */
				LWLockRelease(ControlFileLock);
				return false;
			}
			(*segno)++;
			XLogFilePath(path, tli, *segno, wal_segment_size);
		}
	}

	Assert(access(path, F_OK) != 0 && errno == ENOENT);
	if (durable_rename(tmppath, path, LOG) != 0)
	{
		LWLockRelease(ControlFileLock);
		/* durable_rename 已经输出了日志消息 */
		return false;
	}

	LWLockRelease(ControlFileLock);

	return true;
}

/*
 * 在指定范围内未能找到空闲槽位
 */
int
XLogFileOpen(XLogSegNo segno, TimeLineID tli)
{
	char		path[MAXPGPATH];
	int			fd;

	XLogFilePath(path, tli, segno, wal_segment_size);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | O_CLOEXEC |
					   get_sync_bit(sync_method));
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	return fd;
}

/*
 * 关闭当前用于写入的日志文件段。
 */
static void
XLogFileClose(void)
{
	Assert(openLogFile >= 0);

	/*
	 * WAL 段文件在正常操作中不会被重新读取，所以我们建议
	 * 操作系统释放任何缓存的页面。但如果 WAL 归档或流复制
	 * 处于活动状态，则不要这样做，因为归档器和 walsender 进程
	 * 可能使用缓存来读取 WAL 段。
	 */
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	if (!XLogIsNeeded() && (io_direct_flags & IO_DIRECT_WAL) == 0)
		(void) posix_fadvise(openLogFile, 0, 0, POSIX_FADV_DONTNEED);
#endif

	if (close(openLogFile) != 0)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, openLogTLI, openLogSegNo, wal_segment_size);
		errno = save_errno;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", xlogfname)));
	}

	openLogFile = -1;
	ReleaseExternalFD();
}

/*
 * 预分配超出指定日志终点的日志文件。
 *
 * XXX 这目前极其保守，
 * 因为它只强制存在一个未来的日志段，
 * 而且只有在当前段完成 75% 时才这样做。
 * 这只适用于非常低WAL 量的系统。
 * 高流量系统一旦建立了足够的回收日志段集就会正常工作，
 * 但启动过渡期可能包括大量由前台进程创建的段，这不太好。
 *
 * XLogFileInitInternal() 可以 ereport(ERROR)。
 * 所有已知原因都表明有重大问题；例如，
 * 文件系统满是原因之一。
 * 检查点 WAL和/或 ControlFile 更新已经完成。如果 RequestCheckpoint()发起了当前检查点，
 * 而 ERROR 结束了此函数，则调用RequestCheckpoint() 的命令失败。
 * 这不理想，
 * 但不值得为了使用调用者指定的 elevel 值而扭曲更多函数。（无论是否有RequestCheckpoint()，
 * ERROR 都会阻止一些非必要的报告和资源回收。）
 */
static void
PreallocXlogFiles(XLogRecPtr endptr, TimeLineID tli)
{
	XLogSegNo	_logSegNo;
	int			lf;
	bool		added;
	char		path[MAXPGPATH];
	uint64		offset;

	if (!XLogCtl->InstallXLogFileSegmentActive)
		return;					/* 无锁检查表明否 */

	XLByteToPrevSeg(endptr, _logSegNo, wal_segment_size);
	offset = XLogSegmentOffset(endptr - 1, wal_segment_size);
	if (offset >= (uint32) (0.75 * wal_segment_size))
	{
		_logSegNo++;
		lf = XLogFileInitInternal(_logSegNo, tli, &added, path);
		if (lf >= 0)
			close(lf);
		if (added)
			CheckpointStats.ckpt_segs_added++;
	}
}

/*
 * 如果给定的日志段已被删除或回收，
 * 则抛出错误。
 * 调用者应该只传递它知道在服务器运行期间存在过的段，
 * 因为如果自启动以来没有删除任何 WAL 段，
 * 此函数总是成功。'tli' 仅用于错误消息。
 *
 * 注意：
 * 此函数保证返回时保持 errno 不变。
 * 这支持调用者使用此函数可能提供更好的关于缺失文件的错误消息，
 * 同时仍然能够在返回后抛出正常的文件访问错误。
 */
void
CheckXLogRemoved(XLogSegNo segno, TimeLineID tli)
{
	int			save_errno = errno;
	XLogSegNo	lastRemovedSegNo;

	SpinLockAcquire(&XLogCtl->info_lck);
	lastRemovedSegNo = XLogCtl->lastRemovedSegNo;
	SpinLockRelease(&XLogCtl->info_lck);

	if (segno <= lastRemovedSegNo)
	{
		char		filename[MAXFNAMELEN];

		XLogFileName(filename, tli, segno, wal_segment_size);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						filename)));
	}
	errno = save_errno;
}

/*
 * 返回最后删除的 WAL 段，
 * 如果自启动以来没有删除任何段，则返回 0。
 *
 * 注意：
 * 结果可能任意快地过时，调用者必须处理这种情况。
 */
XLogSegNo
XLogGetLastRemovedSegno(void)
{
	XLogSegNo	lastRemovedSegNo;

	SpinLockAcquire(&XLogCtl->info_lck);
	lastRemovedSegNo = XLogCtl->lastRemovedSegNo;
	SpinLockRelease(&XLogCtl->info_lck);

	return lastRemovedSegNo;
}


/*
 * 更新共享内存中的最后删除段号指针，
 * 以反映给定的 XLOG 文件已被删除。
 */
static void
UpdateLastRemovedPtr(char *filename)
{
	uint32		tli;
	XLogSegNo	segno;

	XLogFromFileName(filename, &tli, &segno, wal_segment_size);

	SpinLockAcquire(&XLogCtl->info_lck);
	if (segno > XLogCtl->lastRemovedSegNo)
		XLogCtl->lastRemovedSegNo = segno;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * 删除 pg_wal 中的所有临时日志文件
 *
 * 这在之前崩溃后的恢复开始时调用，
 * 此时没有其他进程写入新的 WAL 数据。
 */
static void
RemoveTempXlogFiles(void)
{
	DIR		   *xldir;
	struct dirent *xlde;

	elog(DEBUG2, "removing all temporary WAL segments");

	xldir = AllocateDir(XLOGDIR);
	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		char		path[MAXPGPATH];

		if (strncmp(xlde->d_name, "xlogtemp.", 9) != 0)
			continue;

		snprintf(path, MAXPGPATH, XLOGDIR "/%s", xlde->d_name);
		unlink(path);
		elog(DEBUG2, "removed temporary WAL segment \"%s\"", path);
	}
	FreeDir(xldir);
}

/*
 * 回收或删除所有早于或等于传入 segno 的日志文件。
 *
 * endptr 是当前（或最近的）xlog 结束位置，lastredoptr 是
 * 最后一个检查点的重做指针。这些用于确定我们是想回收
 * 而不是删除不再需要的日志文件。
 *
 * insertTLI 是 XLOG 插入的当前时间线。任何回收的段
 * 应该用于此时间线。
 */
static void
RemoveOldXlogFiles(XLogSegNo segno, XLogRecPtr lastredoptr, XLogRecPtr endptr,
				   TimeLineID insertTLI)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		lastoff[MAXFNAMELEN];
	XLogSegNo	endlogSegNo;
	XLogSegNo	recycleSegNo;

	/* 初始化关于尝试回收到何处的信息 */
	XLByteToSeg(endptr, endlogSegNo, wal_segment_size);
	recycleSegNo = XLOGfileslop(lastredoptr);

	/*
	 * 构造要保留的最后一段的文件名。时间线 ID 无关紧要，
	 * 我们在比较中忽略它。（在恢复期间，InsertTimeLineID 未设置，
	 * 所以我们不能使用它。）
	 */
	XLogFileName(lastoff, 0, segno, wal_segment_size);

	elog(DEBUG2, "attempting to remove WAL segments older than log file %s",
		 lastoff);

	xldir = AllocateDir(XLOGDIR);

	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		/* 忽略不是 XLOG 段的文件 */
		if (!IsXLogFileName(xlde->d_name) &&
			!IsPartialXLogFileName(xlde->d_name))
			continue;

		/*
		 * 我们在决定是否仍需要段时忽略 XLOG
		 * 段标识符的时间线部分。
		 * 这确保我们不会过早删除父时间线的段。
		 * 我们可能可以更积极地
		 * 删除非父时间线的段，
		 * 但那会复杂得多。
		 *
		 * 我们使用文件名的字母数字排序属性来决定哪些比
		 * lastoff 段更早。
		 */
		if (strcmp(xlde->d_name + 8, lastoff + 8) <= 0)
		{
			if (XLogArchiveCheckDone(xlde->d_name))
			{
				/* 先更新共享内存中的最后删除位置 */
				UpdateLastRemovedPtr(xlde->d_name);

				RemoveXlogFile(xlde, recycleSegNo, &endlogSegNo, insertTLI);
			}
		}
	}

	FreeDir(xldir);
}

/*
 * 删除不属于给定时间线历史的 WAL 文件。
 *
 * 这在恢复期间调用，
 * 每当我们切换到跟随新时间线时，以及在恢复结束时创建新时间线时调用。
 * 我们通常不会关心 pg_wal 中多余的 WAL 文件，
 * 但它们可能是旧时间线上我们尚未使用的预分配或回收的 WAL 段，包含垃圾数据。
 * 如果我们只是把它们留在 pg_wal 中，
 * 它们最终会被归档，我们不能让这种情况发生。
 * 属于我们时间线历史的文件是有效的，
 * 因为我们已成功重放它们，但对于其他文件我们无法确定。
 *
 * 'switchpoint' 是我们在 WAL 中切换到新时间线的当前点，
 * 'newTLI' 是我们要切换到的新时间线。
 */
void
RemoveNonParentXlogFiles(XLogRecPtr switchpoint, TimeLineID newTLI)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		switchseg[MAXFNAMELEN];
	XLogSegNo	endLogSegNo;
	XLogSegNo	switchLogSegNo;
	XLogSegNo	recycleSegNo;

	/*
	 * 先更新共享内存
	 * 中的最后删除位置
	 */
	XLByteToPrevSeg(switchpoint, switchLogSegNo, wal_segment_size);
	XLByteToSeg(switchpoint, endLogSegNo, wal_segment_size);
	recycleSegNo = endLogSegNo + 10;

	/*
	 * 构造要保留的最后一段的文件名。
	 */
	XLogFileName(switchseg, newTLI, switchLogSegNo, wal_segment_size);

	elog(DEBUG2, "attempting to remove WAL segments newer than log file %s",
		 switchseg);

	xldir = AllocateDir(XLOGDIR);

	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		/* 忽略不是 XLOG 段的文件 */
		if (!IsXLogFileName(xlde->d_name))
			continue;

		/*
		 * 构造要保留的最
		 * 后一段的
		 * 文件名。
		 */
		if (strncmp(xlde->d_name, switchseg, 8) < 0 &&
			strcmp(xlde->d_name + 8, switchseg + 8) > 0)
		{
			/*
			 * 然而，若文件已被标记为 .ready，则先不要
			 * 删除它。删除它应该没问题——不属于我们时间线历史的文件
			 * 不是恢复所必需的——但让它们稍后被归档并删除似乎更
			 * 安全。
			 */
			if (!XLogArchiveIsReady(xlde->d_name))
				RemoveXlogFile(xlde, recycleSegNo, &endLogSegNo, newTLI);
		}
	}

	FreeDir(xldir);
}

/*
 * 回收或删除不再需要的日志文件。
 *
 * segment_de 是要回收或删除的段的 dirent 结构。
 * recycleSegNo 是要回收到达的段号。endlogSegNo 是
 * 当前（或最近的）WAL 结束位置的段号。
 *
 * 如果段被回收，endlogSegNo 会递增，以便在将来的
 * 调用者调用此函数时不会再次检查它。
 *
 * insertTLI 是 XLOG 插入的当前时间线。任何回收的段
 * 应该用于此时间线。
 */
static void
RemoveXlogFile(const struct dirent *segment_de,
			   XLogSegNo recycleSegNo, XLogSegNo *endlogSegNo,
			   TimeLineID insertTLI)
{
	char		path[MAXPGPATH];
#ifdef WIN32
	char		newpath[MAXPGPATH];
#endif
	const char *segname = segment_de->d_name;

	snprintf(path, MAXPGPATH, XLOGDIR "/%s", segname);

	/*
	 * 但是，
	 * 如果文件已被标记为 .ready，
	 * 则不要删除它。删除它应该是可以的 —— 不属于我们时间线历史的文件对于恢复不是必需的 —— 但让它们被归档并稍后删除似乎更安全。
	 */
	if (wal_recycle &&
		*endlogSegNo <= recycleSegNo &&
		XLogCtl->InstallXLogFileSegmentActive &&	/* 被调用者重新检查此条件 */
		get_dirent_type(path, segment_de, false, DEBUG2) == PGFILETYPE_REG &&
		InstallXLogFileSegment(endlogSegNo, path,
							   true, recycleSegNo, insertTLI))
	{
		ereport(DEBUG2,
				(errmsg_internal("recycled write-ahead log file \"%s\"",
								 segname)));
		CheckpointStats.ckpt_segs_recycled++;
		/* 被调用者重新检查此条件 */
		(*endlogSegNo)++;
	}
	else
	{
		/* 不需要在将来的迭代中重新检查那个槽位 */
		int			rc;

		ereport(DEBUG2,
				(errmsg_internal("removing write-ahead log file \"%s\"",
								 segname)));

#ifdef WIN32

		/*
		 * 在 Windows 上，如果另一个进程（例如另一个后端）以
		 * FILE_SHARE_DELETE 模式打开文件，unlink 会成功，但文件
		 * 仍会出现在目录列表中，直到最后一个句柄关闭。为了避免将
		 * 残留的已删除文件与需要归档的活动 WAL 文件混淆，
		 * 在删除之前先重命名它。
		 *
		 * 如果另一个进程在没有 FILE_SHARE_DELETE 标志的情况下
		 * 打开文件，重命名将失败。我们将在下一个检查点重试。
		 */
		snprintf(newpath, MAXPGPATH, "%s.deleted", path);
		if (rename(path, newpath) != 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not rename file \"%s\": %m",
							path)));
			return;
		}
		rc = durable_unlink(newpath, LOG);
#else
		rc = durable_unlink(path, LOG);
#endif
		if (rc != 0)
		{
			/* 消息已由 durable_unlink() 记录 */
			return;
		}
		CheckpointStats.ckpt_segs_removed++;
	}

	XLogArchiveCleanup(segname);
}

/*
 * 验证 pg_wal 和 pg_wal/archive_status
 * 是否存在。
 *
 * 若后者不存在则重建。
 * 本函数的目标不是验证这些目录的内容，
 * 而是帮助处理有人为 PITR 做了数据库集簇拷贝却漏掉 pg_wal 的情况。
 *
 * 我们也可以在 pg_wal 不存在时重建它，
 * 但深思熟虑后决定不这样做。
 * 经常把 pg_wal 做成符号链接，
 * 错误地重建它可能造成混乱。
 */
static void
ValidateXLOGDirectoryStructure(void)
{
	char		path[MAXPGPATH];
	struct stat stat_buf;

	/* 检查 pg_wal；如果不存在，报错退出 */
	if (stat(XLOGDIR, &stat_buf) != 0 ||
		!S_ISDIR(stat_buf.st_mode))
		ereport(FATAL,
				(errmsg("required WAL directory \"%s\" does not exist",
						XLOGDIR)));

	/* 检查 archive_status */
	snprintf(path, MAXPGPATH, XLOGDIR "/archive_status");
	if (stat(path, &stat_buf) == 0)
	{
		/* 检查存在但不是目录的怪异情况 */
		if (!S_ISDIR(stat_buf.st_mode))
			ereport(FATAL,
					(errmsg("required WAL directory \"%s\" does not exist",
							path)));
	}
	else
	{
		ereport(LOG,
				(errmsg("creating missing WAL directory \"%s\"", path)));
		if (MakePGDirectory(path) < 0)
			ereport(FATAL,
					(errmsg("could not create missing directory \"%s\": %m",
							path)));
	}
}

/*
 * 删除以前的备份历史文件。
 * 这也会重试为之前XLogArchiveNotify
 * 失败的任何备份历史文件创建 .ready 文件。
 */
static void
CleanupBackupHistory(void)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		path[MAXPGPATH + sizeof(XLOGDIR)];

	xldir = AllocateDir(XLOGDIR);

	while ((xlde = ReadDir(xldir, XLOGDIR)) != NULL)
	{
		if (IsBackupHistoryFileName(xlde->d_name))
		{
			if (XLogArchiveCheckDone(xlde->d_name))
			{
				elog(DEBUG2, "removing WAL backup history file \"%s\"",
					 xlde->d_name);
				snprintf(path, sizeof(path), XLOGDIR "/%s", xlde->d_name);
				unlink(path);
				XLogArchiveCleanup(xlde->d_name);
			}
		}
	}

	FreeDir(xldir);
}

/*
 * pg_control 的 I/O 例程
 *
 * *ControlFile 是共享内存中的缓冲区，
 * 保存 pg_control 内容的映像。
 * WriteControlFile() 根据预加载的缓冲区初始化 pg_control，
 * ReadControlFile() 从 pg_control 文件加载缓冲区（在 postmaster 或独立后端启动期间），
 * UpdateControlFile() 在我们修改 xlog 状态后重写 pg_control。
 * InitControlFile() 用初始值填充缓冲区。
 *
 * 为简单起见，
 * WriteControlFile() 初始化与检查后端/数据库兼容性相关的 pg_control 字段，
 * ReadControlFile() 验证它们是否正确。
 * 我们可以将 I/O 和兼容性检查函数分离，但目前似乎没有需要。
 */

static void
InitControlFile(uint64 sysidentifier)
{
	char		mock_auth_nonce[MOCK_AUTH_NONCE_LEN];

	/*
	 * 生成一个随机 nonce。用于对不存在的用户发起的、
	 * 注定失败的认证请求。该 nonce 用于为不存在的用户
	 * 创建看起来真实的口令质询，以代替
	 * 实际存储的口令。
	 */
	if (!pg_strong_random(mock_auth_nonce, MOCK_AUTH_NONCE_LEN))
		ereport(PANIC,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate secret authorization token")));

	memset(ControlFile, 0, sizeof(ControlFileData));
	/* 初始化 pg_control 状态字段 */
	ControlFile->system_identifier = sysidentifier;
	memcpy(ControlFile->mock_authentication_nonce, mock_auth_nonce, MOCK_AUTH_NONCE_LEN);
	ControlFile->state = DB_SHUTDOWNED;
	ControlFile->unloggedLSN = FirstNormalUnloggedLSN;

	/* 设置重要参数值供重放 WAL 时使用 */
	ControlFile->MaxConnections = MaxConnections;
	ControlFile->max_worker_processes = max_worker_processes;
	ControlFile->max_wal_senders = max_wal_senders;
	ControlFile->max_prepared_xacts = max_prepared_xacts;
	ControlFile->max_locks_per_xact = max_locks_per_xact;
	ControlFile->wal_level = wal_level;
	ControlFile->wal_log_hints = wal_log_hints;
	ControlFile->track_commit_timestamp = track_commit_timestamp;
	ControlFile->data_checksum_version = bootstrap_data_checksum_version;
}

static void
WriteControlFile(void)
{
	int			fd;
	char		buffer[PG_CONTROL_FILE_SIZE];	/* 不必对齐 */

	/*
	 * 初始化版本与兼容性检查字段
	 */
	ControlFile->pg_control_version = PG_CONTROL_VERSION;
	ControlFile->catalog_version_no = CATALOG_VERSION_NO;

	ControlFile->maxAlign = MAXIMUM_ALIGNOF;
	ControlFile->floatFormat = FLOATFORMAT_VALUE;

	ControlFile->blcksz = BLCKSZ;
	ControlFile->relseg_size = RELSEG_SIZE;
	ControlFile->xlog_blcksz = XLOG_BLCKSZ;
	ControlFile->xlog_seg_size = wal_segment_size;

	ControlFile->nameDataLen = NAMEDATALEN;
	ControlFile->indexMaxKeys = INDEX_MAX_KEYS;

	ControlFile->toast_max_chunk_size = TOAST_MAX_CHUNK_SIZE;
	ControlFile->loblksize = LOBLKSIZE;

	ControlFile->float8ByVal = FLOAT8PASSBYVAL;

	/* 内容受 CRC 保护 */
	INIT_CRC32C(ControlFile->crc);
	COMP_CRC32C(ControlFile->crc,
				(char *) ControlFile,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(ControlFile->crc);

	/*
	 * 我们将 PG_CONTROL_FILE_SIZE 字节写入 pg_control，
	 * 对超过sizeof(ControlFileData) 的部分进行零填充。
	 * 这减少了读取pg_control 时过早 EOF 错误的可能性。
	 * 当我们检查文件内容时仍会失败，
	 * 但希望有比"无法读取 pg_control"更具体的错误。
	 */
	memset(buffer, 0, PG_CONTROL_FILE_SIZE);
	memcpy(buffer, ControlFile, sizeof(ControlFileData));

	fd = BasicOpenFile(XLOG_CONTROL_FILE,
					   O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						XLOG_CONTROL_FILE)));

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_CONTROL_FILE_WRITE);
	if (write(fd, buffer, PG_CONTROL_FILE_SIZE) != PG_CONTROL_FILE_SIZE)
	{
		/* 若 write 未设置 errno，则假定问题是磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						XLOG_CONTROL_FILE)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_CONTROL_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						XLOG_CONTROL_FILE)));
	pgstat_report_wait_end();

	if (close(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						XLOG_CONTROL_FILE)));
}

static void
ReadControlFile(void)
{
	pg_crc32c	crc;
	int			fd;
	static char wal_segsz_str[20];
	int			r;

	/*
	 * 读取数据……
	 */
	fd = BasicOpenFile(XLOG_CONTROL_FILE,
					   O_RDWR | PG_BINARY);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						XLOG_CONTROL_FILE)));

	pgstat_report_wait_start(WAIT_EVENT_CONTROL_FILE_READ);
	r = read(fd, ControlFile, sizeof(ControlFileData));
	if (r != sizeof(ControlFileData))
	{
		if (r < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							XLOG_CONTROL_FILE)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							XLOG_CONTROL_FILE, r, sizeof(ControlFileData))));
	}
	pgstat_report_wait_end();

	close(fd);

	/*
	 * 检查预期的 pg_control 格式版本。
	 * 如果这不对，
	 * CRC 检查很可能会失败，
	 * 因为我们将检查错误数量的字节。抱怨版本错误可能比抱怨 CRC 错误更有启发性。
	 */

	if (ControlFile->pg_control_version != PG_CONTROL_VERSION && ControlFile->pg_control_version % 65536 == 0 && ControlFile->pg_control_version / 65536 != 0)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with PG_CONTROL_VERSION %d (0x%08x),"
						   " but the server was compiled with PG_CONTROL_VERSION %d (0x%08x).",
						   ControlFile->pg_control_version, ControlFile->pg_control_version,
						   PG_CONTROL_VERSION, PG_CONTROL_VERSION),
				 errhint("This could be a problem of mismatched byte ordering.  It looks like you need to initdb.")));

	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with PG_CONTROL_VERSION %d,"
						   " but the server was compiled with PG_CONTROL_VERSION %d.",
						   ControlFile->pg_control_version, PG_CONTROL_VERSION),
				 errhint("It looks like you need to initdb.")));

	/* 现在检查 CRC。 */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
				(char *) ControlFile,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, ControlFile->crc))
		ereport(FATAL,
				(errmsg("incorrect checksum in control file")));

	/*
	 * 立即进行兼容性检查。若数据库与后端可执行
	 * 文件不兼容，我们希望在可能造成任何损害之前
	 * 中止。
	 */
	if (ControlFile->catalog_version_no != CATALOG_VERSION_NO)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with CATALOG_VERSION_NO %d,"
						   " but the server was compiled with CATALOG_VERSION_NO %d.",
						   ControlFile->catalog_version_no, CATALOG_VERSION_NO),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->maxAlign != MAXIMUM_ALIGNOF)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with MAXALIGN %d,"
						   " but the server was compiled with MAXALIGN %d.",
						   ControlFile->maxAlign, MAXIMUM_ALIGNOF),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->floatFormat != FLOATFORMAT_VALUE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster appears to use a different floating-point number format than the server executable."),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->blcksz != BLCKSZ)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with BLCKSZ %d,"
						   " but the server was compiled with BLCKSZ %d.",
						   ControlFile->blcksz, BLCKSZ),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->relseg_size != RELSEG_SIZE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with RELSEG_SIZE %d,"
						   " but the server was compiled with RELSEG_SIZE %d.",
						   ControlFile->relseg_size, RELSEG_SIZE),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->xlog_blcksz != XLOG_BLCKSZ)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with XLOG_BLCKSZ %d,"
						   " but the server was compiled with XLOG_BLCKSZ %d.",
						   ControlFile->xlog_blcksz, XLOG_BLCKSZ),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->nameDataLen != NAMEDATALEN)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with NAMEDATALEN %d,"
						   " but the server was compiled with NAMEDATALEN %d.",
						   ControlFile->nameDataLen, NAMEDATALEN),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->indexMaxKeys != INDEX_MAX_KEYS)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with INDEX_MAX_KEYS %d,"
						   " but the server was compiled with INDEX_MAX_KEYS %d.",
						   ControlFile->indexMaxKeys, INDEX_MAX_KEYS),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->toast_max_chunk_size != TOAST_MAX_CHUNK_SIZE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with TOAST_MAX_CHUNK_SIZE %d,"
						   " but the server was compiled with TOAST_MAX_CHUNK_SIZE %d.",
						   ControlFile->toast_max_chunk_size, (int) TOAST_MAX_CHUNK_SIZE),
				 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->loblksize != LOBLKSIZE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with LOBLKSIZE %d,"
						   " but the server was compiled with LOBLKSIZE %d.",
						   ControlFile->loblksize, (int) LOBLKSIZE),
				 errhint("It looks like you need to recompile or initdb.")));

#ifdef USE_FLOAT8_BYVAL
	if (ControlFile->float8ByVal != true)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized without USE_FLOAT8_BYVAL"
						   " but the server was compiled with USE_FLOAT8_BYVAL."),
				 errhint("It looks like you need to recompile or initdb.")));
#else
	if (ControlFile->float8ByVal != false)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with USE_FLOAT8_BYVAL"
						   " but the server was compiled without USE_FLOAT8_BYVAL."),
				 errhint("It looks like you need to recompile or initdb.")));
#endif

	wal_segment_size = ControlFile->xlog_seg_size;

	if (!IsValidWalSegSize(wal_segment_size))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg_plural("WAL segment size must be a power of two between 1 MB and 1 GB, but the control file specifies %d byte",
									  "WAL segment size must be a power of two between 1 MB and 1 GB, but the control file specifies %d bytes",
									  wal_segment_size,
									  wal_segment_size)));

	snprintf(wal_segsz_str, sizeof(wal_segsz_str), "%d", wal_segment_size);
	SetConfigOption("wal_segment_size", wal_segsz_str, PGC_INTERNAL,
					PGC_S_DYNAMIC_DEFAULT);

	/* 检查并更新依赖于 wal_segment_size 的变量 */
	if (ConvertToXSegs(min_wal_size_mb, wal_segment_size) < 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("\"min_wal_size\" must be at least twice \"wal_segment_size\"")));

	if (ConvertToXSegs(max_wal_size_mb, wal_segment_size) < 2)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("\"max_wal_size\" must be at least twice \"wal_segment_size\"")));

	UsableBytesInSegment =
		(wal_segment_size / XLOG_BLCKSZ * UsableBytesInPage) -
		(SizeOfXLogLongPHD - SizeOfXLogShortPHD);

	CalculateCheckpointSegments();

	/* 也让 initdb 设置作为 GUC 变量可见 */
	SetConfigOption("data_checksums", DataChecksumsEnabled() ? "yes" : "no",
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
}

/*
 * 更新控制文件的实用包装。注意控制
 * 文件会被刷盘。
 */
static void
UpdateControlFile(void)
{
	update_controlfile(DataDir, ControlFile, true);
}

/*
 * 返回控制文件中的唯一系统标识符。
 */
uint64
GetSystemIdentifier(void)
{
	Assert(ControlFile != NULL);
	return ControlFile->system_identifier;
}

/*
 * 更新控制文件的实用封装。注意控制文件会被刷盘。
 */
char *
GetMockAuthenticationNonce(void)
{
	Assert(ControlFile != NULL);
	return ControlFile->mock_authentication_nonce;
}

/*
 * 从控制文件返回唯一的系统标识符。
 */
bool
DataChecksumsEnabled(void)
{
	Assert(ControlFile != NULL);
	return (ControlFile->data_checksum_version > 0);
}

/*
 * 为无日志关系返回一个假的 LSN。
 *
 * 每次调用生成一个大于任何先前返回值的 LSN。
 * 当前计数器值在干净关闭时会保存和恢复，
 * 但像无日志关系一样，
 * 不会在崩溃后存活。
 * 如果你需要一个类似 LSN 的递增数字序列而不写入任何 WAL，可以使用此函数代替 XLogInsert 返回的真实 LSN 值。
 */
XLogRecPtr
GetFakeLSNForUnloggedRel(void)
{
	XLogRecPtr	nextUnloggedLSN;

	/* 增加 unloggedLSN 计数器，需要 SpinLock */
	SpinLockAcquire(&XLogCtl->ulsn_lck);
	nextUnloggedLSN = XLogCtl->unloggedLSN++;
	SpinLockRelease(&XLogCtl->ulsn_lck);

	return nextUnloggedLSN;
}

/*
 * 自动调整 XLOG 缓冲区数量。
 *
 * wal_buffers 的首选设置约为 shared_buffers 的 3%，
 * 最大为一个 XLOG 段（没有理由认为更多会有帮助，
 * 至少只要我们在切换日志文件时强制 fsync），
 * 最小为 8 个块（这是 PostgreSQL 9.1 之前的默认值，
 * 当时添加了自动调整）。
 *
 * 在 NBuffers 接收到最终值之前不应调用此函数。
 */
static int
XLOGChooseNumBuffers(void)
{
	int			xbuffers;

	xbuffers = NBuffers / 32;
	if (xbuffers > (wal_segment_size / XLOG_BLCKSZ))
		xbuffers = (wal_segment_size / XLOG_BLCKSZ);
	if (xbuffers < 8)
		xbuffers = 8;
	return xbuffers;
}

/*
 * wal_buffers 的 GUC check_hook
 */
bool
check_wal_buffers(int *newval, void **extra, GucSource source)
{
	/*
	 * -1 表示请求自动调优。
	 */
	if (*newval == -1)
	{
		/*
		 * 如果我们还没有改变 boot_val 默认值 -1，就让它保持原样。
		 * 当调用 XLOGShmemSize 时我们会修复它。
		 */
		if (XLOGbuffers == -1)
			return true;

		/* 否则，替换为自动调优值 */
		*newval = XLOGChooseNumBuffers();
	}

	/*
	 * 我们将手动设置的值限制为至少 4 个块。
	 * 在 PostgreSQL 9.1 之前，最小值 4 由 guc.c 强制执行，
	 * 但现在不再是这种情况，
	 * 我们只是静默地将这些值视为对最小值的请求。（我们可以改为抛出错误，
	 * 但这似乎不太有帮助。）
	 */
	if (*newval < 4)
		*newval = 4;

	return true;
}

/*
 * wal_consistency_checking 的 GUC check_hook
 */
bool
check_wal_consistency_checking(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	bool		newwalconsistency[RM_MAX_ID + 1];

	/* 否则，替换自动调整值 */
	MemSet(newwalconsistency, 0, (RM_MAX_ID + 1) * sizeof(bool));

	/* 初始化数组 */
	rawstring = pstrdup(*newval);

	/* 需要字符串的可修改副本 */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* 将字符串解析为标识符列表 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);
		int			rmid;

		/* 检查 'all'。 */
		if (pg_strcasecmp(tok, "all") == 0)
		{
			for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
				if (RmgrIdExists(rmid) && GetRmgr(rmid).rm_mask != NULL)
					newwalconsistency[rmid] = true;
		}
		else
		{
			/* 检查 'all'。 */
			bool		found = false;

			for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
			{
				if (RmgrIdExists(rmid) && GetRmgr(rmid).rm_mask != NULL &&
					pg_strcasecmp(tok, GetRmgr(rmid).rm_name) == 0)
				{
					newwalconsistency[rmid] = true;
					found = true;
					break;
				}
			}
			if (!found)
			{
				/*
				 * 在启动期间，这可能是一个尚未加载的自定义
				 * 资源管理器。推迟检查直到
				 * InitializeWalConsistencyChecking()。
				 */
				if (!process_shared_preload_libraries_done)
				{
					check_wal_consistency_checking_deferred = true;
				}
				else
				{
					GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
					pfree(rawstring);
					list_free(elemlist);
					return false;
				}
			}
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	/* 检查标记是否匹配任何已知资源管理器。 */
	*extra = guc_malloc(ERROR, (RM_MAX_ID + 1) * sizeof(bool));
	memcpy(*extra, newwalconsistency, (RM_MAX_ID + 1) * sizeof(bool));
	return true;
}

/*
 * wal_consistency_checking 的 GUC assign_hook
 */
void
assign_wal_consistency_checking(const char *newval, void *extra)
{
	/*
	 * 如果某些检查被推迟，
	 * 稍后在 InitializeWalConsistencyChecking() 中检查可能会失败。
	 * 但在这种情况下，
	 * postmaster 无论如何都会退出，所以继续分配是安全的。
	 *
	 * 任何指定的内置资源管理器会立即分配，
	 * 这会影响在shared_preload_libraries 处理之前创建的 WAL。
	 * 任何指定的自定义资源管理器将在 shared_preload_libraries
	 * 处理之后才分配，
	 * 但这没问题，
	 * 因为自定义资源管理器的 WAL 在模块加载之前无论如何都不能写入。
	 */
	wal_consistency_checking = extra;
}

/*
 * InitializeWalConsistencyChecking: 在加载自定义资源管理器后运行
 *
 * 如果在 wal_consistency_checking GUC 中指定了任何未知的资源管理器，
 * 处理被推迟。
 * 现在 shared_preload_libraries已经加载，
 * 再次处理 wal_consistency_checking。
 */
void
InitializeWalConsistencyChecking(void)
{
	Assert(process_shared_preload_libraries_done);

	if (check_wal_consistency_checking_deferred)
	{
		struct config_generic *guc;

		guc = find_option("wal_consistency_checking", false, false, ERROR);

		check_wal_consistency_checking_deferred = false;

		set_config_option_ext("wal_consistency_checking",
							  wal_consistency_checking_string,
							  guc->scontext, guc->source, guc->srole,
							  GUC_ACTION_SET, true, ERROR, false);

		/* 检查不应再次推迟 */
		Assert(!check_wal_consistency_checking_deferred);
	}
}

/*
 * archive_command 的 GUC show_hook
 */
const char *
show_archive_command(void)
{
	if (XLogArchivingActive())
		return XLogArchiveCommand;
	else
		return "(disabled)";
}

/*
 * in_hot_standby 的 GUC show_hook
 */
const char *
show_in_hot_standby(void)
{
	/*
	 * 我们根据共享内存显示实际状态，以便此 GUC
	 * 在查询内部检查时报告最新状态。底层
	 * 变量 (in_hot_standby_guc) 仅在我们向客户端
	 * 传输新值时才会改变。
	 */
	return RecoveryInProgress() ? "on" : "off";
}

/*
 * 读取控制文件，设置相应的 GUCs。
 *
 * 这应该在启动期间调用，包括崩溃恢复周期，
 * 除非在引导模式下，此时还没有控制文件！由于没有
 * 可用的共享内存！（其大小可能依赖于
 * 控制文件的内容！），首先将内容存储在本地内存中。XLOGShmemInit()
 * 稍后将把它复制到共享内存中。
 *
 * reset 只是控制是否期望之前的内容（在
 * 重置情况下，有一个指向旧共享内存的悬空指针），还是不期望。
 */
void
LocalProcessControlFile(bool reset)
{
	Assert(reset || ControlFile == NULL);
	ControlFile = palloc(sizeof(ControlFileData));
	ReadControlFile();
}

/*
 * 从控制文件获取 wal_level。对备库而言，该值应
 * 视为其活动的 wal_level，因为它可能与备库上
 * 原先配置的不同。
 */
WalLevel
GetActiveWalLevelOnStandby(void)
{
	return ControlFile->wal_level;
}

/*
 * XLOG 共享内存的初始化
 */
Size
XLOGShmemSize(void)
{
	Size		size;

	/*
	 * 若 wal_buffers 的值为 -1，则使用首选的自动调优值。
	 * 这里做这件事并不算特别干净，但我们必须等到
	 * NBuffers 已获得最终值，且必须在使用
	 * XLOGbuffers 的值做任何重要事情之前完成。
	 *
	 * 我们希望将该值的来源报告为 PGC_S_DYNAMIC_DEFAULT。
	 * 然而，若 DBA 在配置文件中显式设置了 wal_buffers = -1，
	 * 则 PGC_S_DYNAMIC_DEFAULT 无法覆盖它，我们必须用
	 * PGC_S_OVERRIDE 强制处理。
	 */
	if (XLOGbuffers == -1)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", XLOGChooseNumBuffers());
		SetConfigOption("wal_buffers", buf, PGC_POSTMASTER,
						PGC_S_DYNAMIC_DEFAULT);
		if (XLOGbuffers == -1)	/* 未能应用？ */
			SetConfigOption("wal_buffers", buf, PGC_POSTMASTER,
							PGC_S_OVERRIDE);
	}
	Assert(XLOGbuffers > 0);

	/* XLogCtl */
	size = sizeof(XLogCtlData);

	/* WAL 插入锁，外加对齐 */
	size = add_size(size, mul_size(sizeof(WALInsertLockPadded), NUM_XLOGINSERT_LOCKS + 1));
	/* xlblocks array */
	size = add_size(size, mul_size(sizeof(XLogRecPtr), XLOGbuffers));
	/* XLOG I/O 缓冲区的额外对齐填充 */
	size = add_size(size, Max(XLOG_BLCKSZ, PG_IO_ALIGN_SIZE));
	/* 以及缓冲区本身 */
	size = add_size(size, mul_size(XLOG_BLCKSZ, XLOGbuffers));

	/*
	 * 注意：
	 * 我们不把 ControlFileData 计入，
	 * 它来自 CreateSharedMemoryAndSemaphores 添加的 "slop factor"。这让我们能在下面再次使用本例程计算实际分配大小。
	 */

	return size;
}

void
XLOGShmemInit(void)
{
	bool		foundCFile,
				foundXLog;
	char	   *allocptr;
	int			i;
	ControlFileData *localControlFile;

#ifdef WAL_DEBUG

	/*
	 * 为 WAL 调试创建一个不受常规 "no pallocs in critical section" 规则约束的内存上下文。
	 * 是的，
	 * 分配失败可能导致 PANIC，但 wal_debug 反正不是给生产环境用的。
	 */
	if (walDebugCxt == NULL)
	{
		walDebugCxt = AllocSetContextCreate(TopMemoryContext,
											"WAL Debug",
											ALLOCSET_DEFAULT_SIZES);
		MemoryContextAllowInCriticalSection(walDebugCxt, true);
	}
#endif


	XLogCtl = (XLogCtlData *)
		ShmemInitStruct("XLOG Ctl", XLOGShmemSize(), &foundXLog);

	localControlFile = ControlFile;
	ControlFile = (ControlFileData *)
		ShmemInitStruct("Control File", sizeof(ControlFileData), &foundCFile);

	if (foundCFile || foundXLog)
	{
		/* 两者应同时存在或同时不存在 */
		Assert(foundCFile && foundXLog);

		/* 初始化 WALInsertLocks 的本地副本 */
		WALInsertLocks = XLogCtl->Insert.WALInsertLocks;

		if (localControlFile)
			pfree(localControlFile);
		return;
	}
	memset(XLogCtl, 0, sizeof(XLogCtlData));

	/*
	 * 除非处于引导模式，否则已在本地读过控制文件。将
	 * 内容移入共享内存。
	 */
	if (localControlFile)
	{
		memcpy(ControlFile, localControlFile, sizeof(ControlFileData));
		pfree(localControlFile);
	}

	/*
	 * 由于 XLogCtlData 包含 XLogRecPtr 字段，其 sizeof 应为
	 * 相应对齐的倍数，因此这里不需要额外的对齐
	 * 填充。
	 */
	allocptr = ((char *) XLogCtl) + sizeof(XLogCtlData);
	XLogCtl->xlblocks = (XLogRecPtr *) allocptr;
	memset(XLogCtl->xlblocks, 0, sizeof(XLogRecPtr) * XLOGbuffers);
	allocptr += sizeof(XLogRecPtr) * XLOGbuffers;


	/* WAL 插入锁。确保它们按完整填充后的大小对齐 */
	allocptr += sizeof(WALInsertLockPadded) -
		((uintptr_t) allocptr) % sizeof(WALInsertLockPadded);
	WALInsertLocks = XLogCtl->Insert.WALInsertLocks =
		(WALInsertLockPadded *) allocptr;
	allocptr += sizeof(WALInsertLockPadded) * NUM_XLOGINSERT_LOCKS;

	for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
	{
		LWLockInitialize(&WALInsertLocks[i].l.lock, LWTRANCHE_WAL_INSERT);
		WALInsertLocks[i].l.insertingAt = InvalidXLogRecPtr;
		WALInsertLocks[i].l.lastImportantAt = InvalidXLogRecPtr;
	}

	/*
	 * 将页缓冲区的起始对齐到完整的 xlog 块大小边界。
	 * 这简化了 XLOG 插入中的一些计算。对
	 * O_DIRECT 也是必需的。
	 */
	allocptr = (char *) TYPEALIGN(XLOG_BLCKSZ, allocptr);
	XLogCtl->pages = allocptr;
	memset(XLogCtl->pages, 0, (Size) XLOG_BLCKSZ * XLOGbuffers);

	/*
	 * 对 XLogCtl 共享数据做基本初始化。（StartupXLOG 会填入
	 * 额外信息。）
	 */
	XLogCtl->XLogCacheBlck = XLOGbuffers - 1;
	XLogCtl->SharedRecoveryState = RECOVERY_STATE_CRASH;
	XLogCtl->InstallXLogFileSegmentActive = false;
	XLogCtl->WalWriterSleeping = false;

	SpinLockInit(&XLogCtl->Insert.insertpos_lck);
	SpinLockInit(&XLogCtl->info_lck);
	SpinLockInit(&XLogCtl->ulsn_lck);
}

/*
 * 本函数在系统安装时必须调用一次。它创建 pg_control
 * 以及初始 XLOG 段。
 */
void
BootStrapXLOG(void)
{
	CheckPoint	checkPoint;
	char	   *buffer;
	XLogPageHeader page;
	XLogLongPageHeader longpage;
	XLogRecord *record;
	char	   *recptr;
	uint64		sysidentifier;
	struct timeval tv;
	pg_crc32c	crc;

	/* 允许普通 WAL 段创建，就像 StartupXLOG() 会做的那样 */
	SetInstallXLogFileSegmentActive();

	/*
	 * 为本安装选择一个尽量唯一的系统标识符代码。
	 * 我们使用 gettimeofday() 的结果（包括小数秒
	 * 字段），这大约是我们能轻易得到的最唯一值。（别想着
	 * 用 random()，因为它尚未播种，且除了系统时钟值外
	 * 没有可移植的播种方式……）uint64 值的上半部分
	 * 就是 tv_sec，下半部分包含
	 * tv_usec（必须能放入 20 位），再加上当前
	 * PID 的 12 位以增加一点唯一性。了解此编码的人可以
	 * 确定安装的初始化时间，这有时
	 * 也许有用。
	 */
	gettimeofday(&tv, NULL);
	sysidentifier = ((uint64) tv.tv_sec) << 32;
	sysidentifier |= ((uint64) tv.tv_usec) << 12;
	sysidentifier |= getpid() & 0xFFF;

	/* 页缓冲区必须为 O_DIRECT 适当对齐 */
	buffer = (char *) palloc(XLOG_BLCKSZ + XLOG_BLCKSZ);
	page = (XLogPageHeader) TYPEALIGN(XLOG_BLCKSZ, buffer);
	memset(page, 0, XLOG_BLCKSZ);

	/*
	 * 为初始检查点记录设置信息。
	 *
	 * 初始检查点记录写在 WAL 段 logid=0 logseg=1 的开头。
	 * 最开始的 WAL 段 0/0 不使用，
	 * 以便我们能用 0/0 表示 "before any valid WAL segment"。
	 */
	checkPoint.redo = wal_segment_size + SizeOfXLogLongPHD;
	checkPoint.ThisTimeLineID = BootstrapTimeLineID;
	checkPoint.PrevTimeLineID = BootstrapTimeLineID;
	checkPoint.fullPageWrites = fullPageWrites;
	checkPoint.nextXid =
		FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId);
	checkPoint.nextOid = FirstGenbkiObjectId;
	checkPoint.nextMulti = FirstMultiXactId;
	checkPoint.nextMultiOffset = 0;
	checkPoint.oldestXid = FirstNormalTransactionId;
	checkPoint.oldestXidDB = Template1DbOid;
	checkPoint.oldestMulti = FirstMultiXactId;
	checkPoint.oldestMultiDB = Template1DbOid;
	checkPoint.oldestCommitTsXid = InvalidTransactionId;
	checkPoint.newestCommitTsXid = InvalidTransactionId;
	checkPoint.time = (pg_time_t) time(NULL);
	checkPoint.oldestActiveXid = InvalidTransactionId;

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;
	MultiXactSetNextMXact(checkPoint.nextMulti, checkPoint.nextMultiOffset);
	AdvanceOldestClogXid(checkPoint.oldestXid);
	SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);
	SetMultiXactIdLimit(checkPoint.oldestMulti, checkPoint.oldestMultiDB, true);
	SetCommitTsLimit(InvalidTransactionId, InvalidTransactionId);

	/* 设置 XLOG 页面头部 */
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = XLP_LONG_HEADER;
	page->xlp_tli = BootstrapTimeLineID;
	page->xlp_pageaddr = wal_segment_size;
	longpage = (XLogLongPageHeader) page;
	longpage->xlp_sysid = sysidentifier;
	longpage->xlp_seg_size = wal_segment_size;
	longpage->xlp_xlog_blcksz = XLOG_BLCKSZ;

	/* 插入初始检查点记录 */
	recptr = ((char *) page + SizeOfXLogLongPHD);
	record = (XLogRecord *) recptr;
	record->xl_prev = 0;
	record->xl_xid = InvalidTransactionId;
	record->xl_tot_len = SizeOfXLogRecord + SizeOfXLogRecordDataHeaderShort + sizeof(checkPoint);
	record->xl_info = XLOG_CHECKPOINT_SHUTDOWN;
	record->xl_rmid = RM_XLOG_ID;
	recptr += SizeOfXLogRecord;
	/* 填充 XLogRecordDataHeaderShort 结构 */
	*(recptr++) = (char) XLR_BLOCK_ID_DATA_SHORT;
	*(recptr++) = sizeof(checkPoint);
	memcpy(recptr, &checkPoint, sizeof(checkPoint));
	recptr += sizeof(checkPoint);
	Assert(recptr - (char *) record == record->xl_tot_len);

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, ((char *) record) + SizeOfXLogRecord, record->xl_tot_len - SizeOfXLogRecord);
	COMP_CRC32C(crc, (char *) record, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);
	record->xl_crc = crc;

	/* 创建第一个 XLOG 段文件 */
	openLogTLI = BootstrapTimeLineID;
	openLogFile = XLogFileInit(1, BootstrapTimeLineID);

	/*
	 * 这里不必费心 Reserve/ReleaseExternalFD，因为我们马上就会
	 * 再次关闭该文件。
	 */

	/* 写入带有初始记录的第一页 */
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_WAL_BOOTSTRAP_WRITE);
	if (write(openLogFile, page, XLOG_BLCKSZ) != XLOG_BLCKSZ)
	{
		/* 用初始记录写入第一页 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write bootstrap write-ahead log file: %m")));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_WAL_BOOTSTRAP_SYNC);
	if (pg_fsync(openLogFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync bootstrap write-ahead log file: %m")));
	pgstat_report_wait_end();

	if (close(openLogFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close bootstrap write-ahead log file: %m")));

	openLogFile = -1;

	/* 现在创建 pg_control */
	InitControlFile(sysidentifier);
	ControlFile->time = checkPoint.time;
	ControlFile->checkPoint = checkPoint.redo;
	ControlFile->checkPointCopy = checkPoint;

	/* WriteControlFile() 中还会设置一些额外的 ControlFile 字段 */
	WriteControlFile();

	/* 如果写入没有设置 errno，假设问题是磁盘空间不足 */
	BootStrapCLOG();
	BootStrapCommitTs();
	BootStrapSUBTRANS();
	BootStrapMultiXact();

	pfree(buffer);

	/*
	 * 强制读取控制文件——与正常处理不同，否则我们
	 * 永远不会运行其中的检查和 GUC 相关初始化。
	 */
	ReadControlFile();
}

static char *
str_time(pg_time_t tnow)
{
	static char buf[128];

	pg_strftime(buf, sizeof(buf),
				"%Y-%m-%d %H:%M:%S %Z",
				pg_localtime(&tnow, log_timezone));

	return buf;
}

/*
 * 在新时间线上初始化第一个 WAL 段。
 */
static void
XLogInitNewTimeline(TimeLineID endTLI, XLogRecPtr endOfLog, TimeLineID newTLI)
{
	char		xlogfname[MAXFNAMELEN];
	XLogSegNo	endLogSegNo;
	XLogSegNo	startLogSegNo;

	/* 同时引导提交日志 */
	Assert(endTLI != newTLI);

	/*
	 * 最后一次更新最小恢复点。
	 */
	UpdateMinRecoveryPoint(InvalidXLogRecPtr, true);

	/*
	 * 计算旧时间线上的最后一段，以及新时间线上的
	 * 第一段。若切换发生在段中间，
	 * 它们相同；但若切换恰好发生在段
	 * 边界，startLogSegNo 将为 endLogSegNo + 1。
	 */
	XLByteToPrevSeg(endOfLog, endLogSegNo, wal_segment_size);
	XLByteToSeg(endOfLog, startLogSegNo, wal_segment_size);

	/*
	 * 为新时间线初始化起始 WAL 段。若切换
	 * 发生在段中间，则把旧时间线最后一段中
	 * 直到切换点的数据复制到新时间线的起始 WAL
	 * 段。
	 */
	if (endLogSegNo == startLogSegNo)
	{
		/*
		 * 在新时间线上制作该文件的副本。
		 *
		 * 尚不允许写 WAL，因此没有加锁方面的
		 * 考虑。但我们应像 XLogFileInit 一样谨慎，以免
		 * 放上一个伪造文件。
		 */
		XLogFileCopy(newTLI, endLogSegNo, endTLI, endLogSegNo,
					 XLogSegmentOffset(endOfLog, wal_segment_size));
	}
	else
	{
		/*
		 * 切换发生在段边界，因此只需在新时间线上
		 * 创建下一段。
		 */
		int			fd;

		fd = XLogFileInit(startLogSegNo, newTLI);

		if (close(fd) != 0)
		{
			int			save_errno = errno;

			XLogFileName(xlogfname, newTLI, startLogSegNo, wal_segment_size);
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", xlogfname)));
		}
	}

	/*
	 * 务必确保没有为新段张贴 .ready 或 .done
	 * 标志。
	 */
	XLogFileName(xlogfname, newTLI, startLogSegNo, wal_segment_size);
	XLogArchiveCleanup(xlogfname);
}

/*
 * 在归档恢复结束时执行清理动作。
 */
static void
CleanupAfterArchiveRecovery(TimeLineID EndOfLogTLI, XLogRecPtr EndOfLog,
							TimeLineID newTLI)
{
	/*
	 * 若有 recovery_end_command 则执行它。
	 */
	if (recoveryEndCommand && strcmp(recoveryEndCommand, "") != 0)
		ExecuteRecoveryCommand(recoveryEndCommand,
							   "recovery_end_command",
							   true,
							   WAIT_EVENT_RECOVERY_END_COMMAND);

	/*
	 * 我们切换到了新时间线。清理旧时间线上的段。
	 *
	 * 若旧时间线上有任何更高编号的段，则删除
	 * 它们。它们可能包含有效 WAL，也可能是
	 * 含有垃圾的预分配文件。无论如何，它们不属于
	 * 新时间线的历史，因此我们不需要它们。
	 */
	RemoveNonParentXlogFiles(EndOfLog, newTLI);

	/*
	 * 若切换发生在段中间，
	 * 旧时间线上最后那个不完整段该怎么办？若我们不归档它，
	 * 而创建该 WAL 的服务器也从不归档它（例如被陨石击中），
	 * 它将永远进不了归档。
	 * 从我们角度看这没问题，
	 * 因为用新 TLI 创建的新段包含旧时间线直到切换点的全部 WAL。但若你稍后试图对旧时间线上 "missing" 的 WAL 做 PITR，
	 * 恢复在归档中找不到它。
	 * 它物理上在带新 TLI 的新文件中，
	 * 但恢复到较旧时间线时不会去那里找。
	 * 另一方面，
	 * 若我们归档该不完整段，
	 * 而该时间线上的原服务器稍后归档同一段的完整版，就会失败。
	 * （我们在 9.4 及更早曾这样做并造成过问题）。
	 * 作为折中，
	 *
	 * 我们给最后一段加上 .partial 后缀并重命名后归档。
	 * 归档恢复从不会读 .partial 段，
	 * 因此通常不用。
	 * 但在古怪的 PITR 情形下，管理员可手动把它们复制到 pg_wal 目录（去掉后缀）。
	 * 它们对调试也有用。
	 *
	 * 然而若旧时间线已有 .done 或 .ready 文件，
	 * 我们此前已确定该段完整，
	 * 因此可让它正常归档。
	 * （特别是若它本来就从归档恢复而来，预期会有 .done 文件）。
	 */
	if (XLogSegmentOffset(EndOfLog, wal_segment_size) != 0 &&
		XLogArchivingActive())
	{
		char		origfname[MAXFNAMELEN];
		XLogSegNo	endLogSegNo;

		XLByteToPrevSeg(EndOfLog, endLogSegNo, wal_segment_size);
		XLogFileName(origfname, EndOfLogTLI, endLogSegNo, wal_segment_size);

		if (!XLogArchiveIsReadyOrDone(origfname))
		{
			char		origpath[MAXPGPATH];
			char		partialfname[MAXFNAMELEN];
			char		partialpath[MAXPGPATH];

			XLogFilePath(origpath, EndOfLogTLI, endLogSegNo, wal_segment_size);
			snprintf(partialfname, MAXFNAMELEN, "%s.partial", origfname);
			snprintf(partialpath, MAXPGPATH, "%s.partial", origpath);

			/*
			 * 确保 .partial 文件没有对应的 .done 或 .ready
			 * 文件。
			 */
			XLogArchiveCleanup(partialfname);

			durable_rename(origpath, partialpath, ERROR);
			XLogArchiveNotify(partialfname);
		}
	}
}

/*
 * 检查本服务器上恢复操作各方面所需的参数是否设置得
 * 足够高。
 *
 * 注意：本函数测试的所有参数都需要列在
 * high-availability.sgml 的管理员概述一节中。
 * 若你更改它们，别忘了更新该列表。
 */
static void
CheckRequiredParameterValues(void)
{
	/*
	 * 对归档恢复，WAL 必须以至少 'replica'
	 * 的 wal_level 生成。
	 */
	if (ArchiveRecoveryRequested && ControlFile->wal_level == WAL_LEVEL_MINIMAL)
	{
		ereport(FATAL,
				(errmsg("WAL was generated with wal_level=minimal, cannot continue recovering"),
				 errdetail("This happens if you temporarily set wal_level=minimal on the server."),
				 errhint("Use a backup taken after setting wal_level to higher than minimal.")));
	}

	/*
	 * 对热备，WAL 必须以 'replica' 模式生成，并且我们
	 * 必须至少有与主库一样多的后端槽位。
	 */
	if (ArchiveRecoveryRequested && EnableHotStandby)
	{
		/* 我们在进行此测试时忽略 autovacuum_max_workers。 */
		RecoveryRequiresIntParameter("max_connections",
									 MaxConnections,
									 ControlFile->MaxConnections);
		RecoveryRequiresIntParameter("max_worker_processes",
									 max_worker_processes,
									 ControlFile->max_worker_processes);
		RecoveryRequiresIntParameter("max_wal_senders",
									 max_wal_senders,
									 ControlFile->max_wal_senders);
		RecoveryRequiresIntParameter("max_prepared_transactions",
									 max_prepared_xacts,
									 ControlFile->max_prepared_xacts);
		RecoveryRequiresIntParameter("max_locks_per_transaction",
									 max_locks_per_xact,
									 ControlFile->max_locks_per_xact);
	}
}

/*
 * 在 postmaster 或独立后端启动期间必须调用一次
 */
void
StartupXLOG(void)
{
	XLogCtlInsert *Insert;
	CheckPoint	checkPoint;
	bool		wasShutdown;
	bool		didCrash;
	bool		haveTblspcMap;
	bool		haveBackupLabel;
	XLogRecPtr	EndOfLog;
	TimeLineID	EndOfLogTLI;
	TimeLineID	newTLI;
	bool		performedWalRecovery;
	EndOfWalRecoveryInfo *endOfRecoveryInfo;
	XLogRecPtr	abortedRecPtr;
	XLogRecPtr	missingContrecPtr;
	TransactionId oldestActiveXID;
	bool		promoted = false;

	/*
	 * 我们应有可用的辅助进程资源所有者，且不应
	 * 处于已安装其他 resowner 的事务中。
	 */
	Assert(AuxProcessResourceOwner != NULL);
	Assert(CurrentResourceOwner == NULL ||
		   CurrentResourceOwner == AuxProcessResourceOwner);
	CurrentResourceOwner = AuxProcessResourceOwner;

	/*
	 * 检查内容看起来是否有效。
	 */
	if (!XRecOffIsValid(ControlFile->checkPoint))
		ereport(FATAL,
				(errmsg("control file contains invalid checkpoint location")));

	switch (ControlFile->state)
	{
		case DB_SHUTDOWNED:

			/*
			 * 这是预期情况，因此在独立模式下不要过于
			 * 啰嗦
			 */
			ereport(IsPostmasterEnvironment ? LOG : NOTICE,
					(errmsg("database system was shut down at %s",
							str_time(ControlFile->time))));
			break;

		case DB_SHUTDOWNED_IN_RECOVERY:
			ereport(LOG,
					(errmsg("database system was shut down in recovery at %s",
							str_time(ControlFile->time))));
			break;

		case DB_SHUTDOWNING:
			ereport(LOG,
					(errmsg("database system shutdown was interrupted; last known up at %s",
							str_time(ControlFile->time))));
			break;

		case DB_IN_CRASH_RECOVERY:
			ereport(LOG,
					(errmsg("database system was interrupted while in recovery at %s",
							str_time(ControlFile->time)),
					 errhint("This probably means that some data is corrupted and"
							 " you will have to use the last backup for recovery.")));
			break;

		case DB_IN_ARCHIVE_RECOVERY:
			ereport(LOG,
					(errmsg("database system was interrupted while in recovery at log time %s",
							str_time(ControlFile->checkPointCopy.time)),
					 errhint("If this has occurred more than once some data might be corrupted"
							 " and you might need to choose an earlier recovery target.")));
			break;

		case DB_IN_PRODUCTION:
			ereport(LOG,
					(errmsg("database system was interrupted; last known up at %s",
							str_time(ControlFile->time))));
			break;

		default:
			ereport(FATAL,
					(errmsg("control file contains invalid database cluster state")));
	}

	/* 这只是为了能用调试器附加到启动进程 */
#ifdef XLOG_REPLAY_DELAY
	if (ControlFile->state != DB_SHUTDOWNED)
		pg_usleep(60000000L);
#endif

	/*
	 * 验证 pg_wal 和 pg_wal/archive_status 存在。在有人为 PITR
	 * 做了拷贝的情况下，这些目录可能被
	 * 排除，需要重新创建。
	 */
	ValidateXLOGDirectoryStructure();

	/* 这只是为了允许使用调试器附加到启动进程 */
	if (!IsBootstrapProcessingMode())
		RegisterTimeout(STARTUP_PROGRESS_TIMEOUT,
						startup_progress_timeout_handler);

	/*----------
	 * 若我们此前崩溃过，执行几项动作：
	 *
	 * - pg_wal 目录中可能仍有创建新段时使用的一些临时 WAL 段，
	 *   因此做一些清理以免
	 *   撑大该路径。这首先完成，因为同步这些
	 *   临时数据没有意义。
	 *
	 * - 可能有我们已写入并打算 fsync、但
	 *   实际尚未 fsync 的数据。因此，不久后的断电可能
	 *   导致较早未刷盘的写入丢失，即使
	 *   此后从这里写入磁盘的更新数据会被
	 *   持久化。为避免这种情况，对整个数据目录做 fsync。
	 */
	if (ControlFile->state != DB_SHUTDOWNED &&
		ControlFile->state != DB_SHUTDOWNED_IN_RECOVERY)
	{
		RemoveTempXlogFiles();
		SyncDataDirectory();
		didCrash = true;
	}
	else
		didCrash = false;

	/*
	 * 若需要则准备 WAL 恢复。
	 *
	 * InitWalRecovery 分析控制文件以及备份标签文件（若
	 * 有）。它按起始检查点更新内存中的 ControlFile 缓冲区，并设置
	 * InRecovery 与 ArchiveRecoveryRequested。
	 * 若有表空间映射文件，也会应用它。
	 */
	InitWalRecovery(ControlFile, &wasShutdown,
					&haveBackupLabel, &haveTblspcMap);
	checkPoint = ControlFile->checkPointCopy;

	/* 设置报告启动进度所需的超时处理程序。 */
	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;
	MultiXactSetNextMXact(checkPoint.nextMulti, checkPoint.nextMultiOffset);
	AdvanceOldestClogXid(checkPoint.oldestXid);
	SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);
	SetMultiXactIdLimit(checkPoint.oldestMulti, checkPoint.oldestMultiDB, true);
	SetCommitTsLimit(checkPoint.oldestCommitTsXid,
					 checkPoint.newestCommitTsXid);
	XLogCtl->ckptFullXid = checkPoint.nextXid;

	/*
	 * 清除任何旧的 relcache 缓存文件。
	 * 若我们做任何 WAL 重放，
	 * 这是必要的，
	 * 因为那很可能会导致缓存文件与数据库真实状态不同步。
	 * 理论上若数据库曾干净关闭我们可以保留它们，
	 * 但似乎最安全的做法是总是删除它们，
	 * 让它们在第一个后端启动时重建。
	 * 这些文件需要从包括 pg_tblspc 在内的所有目录中移除；
	 * 然而在从备份做归档恢复时，
	 * 符号链接仅在读取 tablespace_map 文件之后才创建，因此需要在创建符号链接之后在这里清除旧的 relcache 文件。
	 */
	RelationCacheInitFileRemove();

	/*
	 * 在有机会移除所需资源之前，
	 * 初始化复制槽。
	 */
	StartupReplicationSlots();

	/*
	 * 启动逻辑状态，
	 * 需要现在设置，以便在崩溃恢复期间有正确的数据。
	 */
	StartupReorderBuffer();

	/*
	 * 启动 CLOG。
	 * 这必须在 ShmemVariableCache->nextXid 已初始化之后、在我们接受连接或开始 WAL 重放之前完成。
	 */
	StartupCLOG();

	/*
	 * 启动 MultiXact。
	 * 我们需要尽早做这件事，以便能够重放截断。
	 */
	StartupMultiXact();

	/*
	 * 提交时间戳同理。
	 * 若控制文件中启用了该设置则激活该功能，
	 * 因为设置禁用时不应跟踪提交时间戳。
	 * 在重放 XLOG_PARAMETER_CHANGE 记录时可启动或停止该功能。
	 */
	if (ControlFile->track_commit_timestamp)
		StartupCommitTs();

	/*
	 * 恢复关于已知复制伙伴重放进度的知识。
	 */
	StartupReplicationOrigin();

	/*
	 * 初始化无日志 LSN。在干净关闭时，
	 * 它从控制文件恢复。在恢复时，
	 * 所有无日志关系都被清掉，因此无日志 LSN 计数器也可以重置。
	 */
	if (ControlFile->state == DB_SHUTDOWNED)
		XLogCtl->unloggedLSN = ControlFile->unloggedLSN;
	else
		XLogCtl->unloggedLSN = FirstNormalUnloggedLSN;

	/*
	 * 将 'now' 与恢复目标时间线之间任何缺失的时间线历史文件从归档复制到
	 * pg_wal。
	 * 虽然我们自己不需要那些文件——
	 * 恢复目标时间线的历史文件也覆盖历史上所有先前时间线——但级联备库可能对它们感兴趣。
	 * 或者，
	 * 若你把本服务器的 WAL 归档到与主库不同的归档，
	 * 故障切换后最好让所有历史文件都归档到那里，
	 * 以便能把旧时间线之一用作 PITR 目标。
	 * 时间线历史文件很小，
	 * 宁可多余复制也好过不复制而后悔。
	 */
	restoreTimeLineHistoryFiles(checkPoint.ThisTimeLineID, recoveryTargetTLI);

	/*
	 * 在进入恢复之前，
	 * 扫描 pg_twophase 并填入其状态，
	 * 以便能处理由 redo 生成的条目。
	 * 在采取任何恢复动作之前扫描的优点是丢弃任何比要重放的第一条记录更新的 2PC 文件，
	 * 避免重放时的冲突。
	 * 这也避免了在恢复磁盘上两阶段数据时的后续扫描。
	 */
	restoreTwoPhaseData();

	/*
	 * 以崩溃恢复启动时，
	 * 重置 pgstat 数据——它可能无效。
	 * 否则恢复 pgstat 数据。在这里做是安全的，
	 *
	 * 因为 postmaster 尚未启动任何其他进程。
	 * 注意：恢复复制槽统计依赖于槽状态已从磁盘恢复。
	 *
	 * TODO：
	 * 再多做一点工作，我们就可以从一个与我们起始的检查点 redo 位置相关联的 pgstat 文件开始。
	 */
	if (didCrash)
		pgstat_discard_stats();
	else
		pgstat_restore_stats();

	lastFullPageWrites = checkPoint.fullPageWrites;

	RedoRecPtr = XLogCtl->RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;
	doPageWrites = lastFullPageWrites;

	/* REDO */
	if (InRecovery)
	{
		/* 初始化 RecoveryInProgress() 的状态 */
		SpinLockAcquire(&XLogCtl->info_lck);
		if (InArchiveRecovery)
			XLogCtl->SharedRecoveryState = RECOVERY_STATE_ARCHIVE;
		else
			XLogCtl->SharedRecoveryState = RECOVERY_STATE_CRASH;
		SpinLockRelease(&XLogCtl->info_lck);

		/*
		 * 更新 pg_control 以表明我们正在恢复，
		 * 并表明所选检查点是我们的起始位置。
		 * 我们还用从备份历史文件获得的任何最小恢复停止点标记 pg_control。
		 * 此时尚不必持有 ControlFileLock，
		 *
		 * 我们还没到那一步。
		 */
		UpdateControlFile();

		/*
		 * 若曾有备份标签文件，
		 * 它已完成使命，信息现已传播到 pg_control。
		 * 我们必须去掉该标签文件，以便若在恢复期间崩溃，
		 * 我们将从最新的恢复重启点继续，
		 * 而不是一路回到备份起点。不过，
		 * 似乎谨慎的做法是把它重命名移开，而不是完全删除。
		 */
		if (haveBackupLabel)
		{
			unlink(BACKUP_LABEL_OLD);
			durable_rename(BACKUP_LABEL_FILE, BACKUP_LABEL_OLD, FATAL);
		}

		/*
		 * 若曾有 tablespace_map 文件，
		 * 它已完成使命，符号链接已创建。
		 * 我们必须去掉该映射文件，以便若在恢复期间崩溃，
		 * 我们不会再次创建符号链接。不过，
		 * 似乎谨慎的做法是把它重命名移开，而不是完全删除。
		 */
		if (haveTblspcMap)
		{
			unlink(TABLESPACE_MAP_OLD);
			durable_rename(TABLESPACE_MAP, TABLESPACE_MAP_OLD, FATAL);
		}

		/*
		 * 初始化 minRecoveryPoint 的本地副本。
		 * 做崩溃恢复时我们希望重放到 WAL 末尾。
		 * 特别是，
		 * 在已提升的备库情形下，
		 * 控制文件中的 minRecoveryPoint 值仅在第一次检查点之后才更新。
		 * 然而，
		 * 若实例在第一次恢复后检查点完成之前崩溃，
		 * 则恢复会使用陈旧位置，导致启动进程在检查数据一致性时认为仍有无效页引用。
		 */
		if (InArchiveRecovery)
		{
			LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
			LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		}
		else
		{
			LocalMinRecoveryPoint = InvalidXLogRecPtr;
			LocalMinRecoveryPointTLI = 0;
		}

		/* 检查用于生成 WAL 的 GUCs 是否允许恢复 */
		CheckRequiredParameterValues();

		/*
		 * 我们处于恢复中，
		 * 因此无日志关系可能已损坏且必须重置。
		 * 这应在允许热备连接之前完成（BEFORE），
		 * 以免只读后端试图读取此前残留的垃圾。
		 */
		ResetUnloggedRelations(UNLOGGED_RELATION_CLEANUP);

		/*
		 * 同样，
		 * 删除崩溃后端留下的任何已保存事务快照文件。
		 */
		DeleteAllExportedSnapshotFiles();

		/*
		 * 若启用了热备则为其初始化。
		 * 我们还不会让后端进入，
		 * 直到到达控制文件中指定的最小恢复点，
		 * 并且已从 running-xacts WAL 记录建立恢复快照。
		 */
		if (ArchiveRecoveryRequested && EnableHotStandby)
		{
			TransactionId *xids;
			int			nxids;

			ereport(DEBUG1,
					(errmsg_internal("initializing for hot standby")));

			InitRecoveryTransactionEnvironment();

			if (wasShutdown)
				oldestActiveXID = PrescanPreparedTransactions(&xids, &nxids);
			else
				oldestActiveXID = checkPoint.oldestActiveXid;
			Assert(TransactionIdIsValid(oldestActiveXID));

			/* 告知 procarray 它必须处理的 xid 范围 */
			ProcArrayInitRecovery(XidFromFullTransactionId(ShmemVariableCache->nextXid));

			/*
			 * 仅启动 subtrans。
			 * CLOG、MultiXact 和提交时间戳已经启动，
			 * 其他 SLRU 在恢复期间不维护，尚不必启动。
			 */
			StartupSUBTRANS(oldestActiveXID);

			/*
			 * 若我们从关闭检查点开始，
			 * 则知道此时主库上没有事务在运行。
			 * 因此伪造一条空的 running-xacts 记录并立即使用。
			 * 为已准备事务恢复额外的备库状态。
			 */
			if (wasShutdown)
			{
				RunningTransactionsData running;
				TransactionId latestCompletedXid;

				/* 更新任何准备好的事务的 pg_subtrans 条目 */
				StandbyRecoverPreparedTransactions();

				/*
				 * 构造表示已关闭服务器的 RunningTransactions 快照，
				 * 其中仅已准备事务仍存活。
				 * 此时从不会溢出，
				 * 因为所有子 xid 都与其父已准备事务一起列出。
				 */
				running.xcnt = nxids;
				running.subxcnt = 0;
				running.subxid_status = SUBXIDS_IN_SUBTRANS;
				running.nextXid = XidFromFullTransactionId(checkPoint.nextXid);
				running.oldestRunningXid = oldestActiveXID;
				latestCompletedXid = XidFromFullTransactionId(checkPoint.nextXid);
				TransactionIdRetreat(latestCompletedXid);
				Assert(TransactionIdIsNormal(latestCompletedXid));
				running.latestCompletedXid = latestCompletedXid;
				running.xids = xids;

				ProcArrayApplyRecoveryInfo(&running);
			}
		}

		/*
		 * 我们已为重放 WAL 做好一切准备。开始吧。
		 */
		PerformWalRecovery();
		performedWalRecovery = true;
	}
	else
		performedWalRecovery = false;

	/*
	 * 完成 WAL 恢复。
	 */
	endOfRecoveryInfo = FinishWalRecovery();
	EndOfLog = endOfRecoveryInfo->endOfLog;
	EndOfLogTLI = endOfRecoveryInfo->endOfLogTLI;
	abortedRecPtr = endOfRecoveryInfo->abortedRecPtr;
	missingContrecPtr = endOfRecoveryInfo->missingContrecPtr;

	/*
	 * 重置 ps 状态显示，
	 * 以免显示任何与恢复相关的信息。
	 */
	set_ps_display("");

	/*
	 * 从备份恢复时（我们处于恢复中，
	 * 且请求了归档恢复），
	 * 若我们没有前滚得足够远以达到数据库一致的点，
	 * 则发出抱怨。
	 * 对于常规的主库在线备份，
	 * 这意味着到达 end-of-backup WAL 记录（此时我们将 backupStartPoint 重置为 Invalid）；
	 * 对于从副本备份（无法向 WAL 流注入记录），
	 * 该点是当我们到达 pg_control 中的 minRecoveryPoint 时（从副本备份时我们有意最后复制它）。
	 * 对于 pg_rewind（它创建 method 为 pg_rewind 的
	 * backup_label）或快照式备份（没有 backup_label），
	 * backupEndRequired 将为 false。
	 *
	 * 注意：
	 * 即使 ControlFile->minRecoveryPoint 可能更靠前，
	 * 在这里查看本地变量 LocalMinRecoveryPoint 也确实没问题——ControlFile->minRecoveryPoint
	 * 不可能超过我们已处理的 WAL。
	 */
	if (InRecovery &&
		(EndOfLog < LocalMinRecoveryPoint ||
		 !XLogRecPtrIsInvalid(ControlFile->backupStartPoint)))
	{
		/*
		 * 在到达 end-of-backup WAL 记录或 minRecoveryPoint 之前就用尽了 WAL。
		 * 这是坏兆头，
		 * 表明你试图从在线备份恢复但从未调用 pg_backup_stop()，
		 * 或者你没有归档所需的全部 WAL。
		 */
		if (ArchiveRecoveryRequested || ControlFile->backupEndRequired)
		{
			if (!XLogRecPtrIsInvalid(ControlFile->backupStartPoint) || ControlFile->backupEndRequired)
				ereport(FATAL,
						(errmsg("WAL ends before end of online backup"),
						 errhint("All WAL generated while online backup was taken must be available at recovery.")));
			else
				ereport(FATAL,
						(errmsg("WAL ends before consistent recovery point")));
		}
	}

	/*
	 * 将无日志关系重置为其 INIT 分支的内容。
	 * 这在恢复完成之后（AFTER）进行，
	 * 以便包含恢复期间创建的任何无日志关系，
	 * 但在恢复被标记为成功完成之前（BEFORE）进行。
	 * 否则若恢复结束后的任一步骤失败，我们就不会重试。
	 */
	if (InRecovery)
		ResetUnloggedRelations(UNLOGGED_RELATION_INIT);

	/*
	 * 预扫描已准备事务以找出存在的 XID 范围。
	 * 此信息此时还不急需，
	 * 但放在这里是为了在做任何磁盘更改之前检测到潜在问题。
	 */
	oldestActiveXID = PrescanPreparedTransactions(NULL, NULL);

	/*
	 * 在可能切换到新时间线（会创建新段）之前、并在最后一次 ReadRecord() 之后，
	 * 允许普通 WAL 段创建。
	 */
	SetInstallXLogFileSegmentActive();

	/*
	 * 考虑是否需要分配新的时间线 ID。
	 *
	 * 若我们做了归档恢复，
	 * 总是分配新 ID。这处理几个问题。
	 * 若我们在恢复期间未跑到 WAL 末尾就停止，
	 * 则显然在生成新时间线，必须分配唯一的新 ID。
	 * 即使我们跑到了末尾，
	 * 修改当前最后一段也有问题，
	 * 因为可能导致试图覆盖该段已归档的副本，而我们鼓励 DBA 让其 archive_command 拒绝这样做。
	 * 我们可以通过让新的活动段拥有新的时间线 ID 来避开该问题。
	 *
	 * 在正常崩溃恢复中，我们可以直接延伸原先所在的时间线。
	 */
	newTLI = endOfRecoveryInfo->lastRecTLI;
	if (ArchiveRecoveryRequested)
	{
		newTLI = findNewestTimeLine(recoveryTargetTLI) + 1;
		ereport(LOG,
				(errmsg("selected new timeline ID: %u", newTLI)));

		/*
		 * 制作最后 WAL 段的可写副本。
		 * （注意我们在 endOfRecovery->lastPage 中也有旧 WAL 最后一块的副本；
		 * 下面会用到。）
		 */
		XLogInitNewTimeline(EndOfLogTLI, EndOfLog, newTLI);

		/*
		 * 把信号文件移开，
		 * 以免在随后的崩溃中意外再次进入归档恢复模式。
		 */
		if (endOfRecoveryInfo->standby_signal_file_found)
			durable_unlink(STANDBY_SIGNAL_FILE, FATAL);

		if (endOfRecoveryInfo->recovery_signal_file_found)
			durable_unlink(RECOVERY_SIGNAL_FILE, FATAL);

		/*
		 * 写时间线历史文件并归档它。
		 * 在此之后（更确切地说，
		 * 一旦文件被归档），
		 * 该时间线在 WAL 归档中以及任何备库看来都会显示为 "taken"。
		 * 若我们在实际切换到新时间线之前崩溃，
		 * 备库仍会认为我们切换了，
		 * 并试图连接新时间线。
		 * 为尽量缩小该窗口，尽量少做从这里到写入恢复结束记录之间的事情。
		 */
		writeTimeLineHistory(newTLI, recoveryTargetTLI,
							 EndOfLog, endOfRecoveryInfo->recoveryStopReason);

		ereport(LOG,
				(errmsg("archive recovery complete")));
	}

	/* 也将在共享内存中保存选定的 TimeLineID */
	XLogCtl->InsertTimeLineID = newTLI;
	XLogCtl->PrevTimeLineID = endOfRecoveryInfo->lastRecTLI;

	/*
	 * 实际上，
	 * 若 WAL 结束于一条不完整记录，则跳过已成功通过的部分，
	 * 并在持久化部分之后开始写入。（关键的是首先写入一条 OVERWRITE_CONTRECORD 消息，
	 * 一旦我们开放写入新 WAL 就会这样做。）
	 */
	if (!XLogRecPtrIsInvalid(missingContrecPtr))
	{
		/*
		 * 仅当我们不切换到新时间线时才应有 
		 * missingContrecPtr。
		 * 发生时间线切换时，
		 * WAL 仅从旧时间线复制到新时间线直到最后一条完整记录的末尾，
		 * 因此不可能有需要忽略的不完整 WAL 记录。
		 */
		Assert(newTLI == endOfRecoveryInfo->lastRecTLI);
		Assert(!XLogRecPtrIsInvalid(abortedRecPtr));
		EndOfLog = missingContrecPtr;
	}

	/*
	 * 准备从 EndOfLog 位置开始写 WAL，
	 * 并使用上一轮中包含最后一条记录的块初始化
	 * xlog 缓冲区缓存。
	 */
	Insert = &XLogCtl->Insert;
	Insert->PrevBytePos = XLogRecPtrToBytePos(endOfRecoveryInfo->lastRec);
	Insert->CurrBytePos = XLogRecPtrToBytePos(EndOfLog);

	/*
	 * 这里的棘手点：
	 * lastPage 包含 LastRec 记录跨越的*最后*一块，
	 * 而不是它开始的那一块。最后一块确实是我们想用的。
	 */
	if (EndOfLog % XLOG_BLCKSZ != 0)
	{
		char	   *page;
		int			len;
		int			firstIdx;

		firstIdx = XLogRecPtrToBufIdx(EndOfLog);
		len = EndOfLog - endOfRecoveryInfo->lastPageBeginPtr;
		Assert(len < XLOG_BLCKSZ);

		/* 复制最后一块的有效部分，并将其余部分清零 */
		page = &XLogCtl->pages[firstIdx * XLOG_BLCKSZ];
		memcpy(page, endOfRecoveryInfo->lastPage, len);
		memset(page + len, 0, XLOG_BLCKSZ - len);

		XLogCtl->xlblocks[firstIdx] = endOfRecoveryInfo->lastPageBeginPtr + XLOG_BLCKSZ;
		XLogCtl->InitializedUpTo = endOfRecoveryInfo->lastPageBeginPtr + XLOG_BLCKSZ;
	}
	else
	{
		/*
		 * 没有需要复制的不完整块。
		 * 只需设置 InitializedUpTo，
		 * 并让第一次尝试插入日志记录时初始化下一个缓冲区。
		 */
		XLogCtl->InitializedUpTo = EndOfLog;
	}

	LogwrtResult.Write = LogwrtResult.Flush = EndOfLog;

	XLogCtl->LogwrtResult = LogwrtResult;

	XLogCtl->LogwrtRqst.Write = EndOfLog;
	XLogCtl->LogwrtRqst.Flush = EndOfLog;

	/*
	 * 在 READ WRITE 事务开始之前使所有 sinval 管理的缓存失效。
	 * xl_heap_inplace WAL 记录没有存储足够的失效数据。
	 * 提交记录（若有）带有失效数据。
	 * 然而，
	 * 无论是否到达提交记录，
	 * 就地更新都是永久的。
	 * 幸运的是，
	 *
	 * 只读事务可以容忍缓存未反映最新就地更新。
	 * 只读事务对显著就地更新的体验如下：
	 *
	 * relhasindex=true 仅在 CREATE INDEX 事务提交使索引对其完全可用后才影响读者；
	 * datconnlimit=DATCONNLIMIT_INVALID_DB 仅在 InitPostgres() 时影响读者，
	 *
	 * 且该读取不使用缓存；
	 * relfrozenxid、datfrozenxid、relminmxid 和 datminmxid 对读者无影响。
	 *
	 * 因此，
	 * 热备查询（全部 READ ONLY）在缺少失效的情况下仍能正确工作。
	 * 这避免了在回移植分支中更改 WAL 格式。
	 */
	SIResetAll();

	/*
	 * 若需要，预分配额外的日志文件。
	 */
	PreallocXlogFiles(EndOfLog, newTLI);

	/*
	 * 好了，我们正式启动完成。
	 */
	InRecovery = false;

	/* 启动 archive_timeout 计时器并开始累计 LSN */
	XLogCtl->lastSegSwitchTime = (pg_time_t) time(NULL);
	XLogCtl->lastSegSwitchLSN = EndOfLog;

	/* 同时将 latestCompletedXid 初始化为 nextXid - 1 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	ShmemVariableCache->latestCompletedXid = ShmemVariableCache->nextXid;
	FullTransactionIdRetreat(&ShmemVariableCache->latestCompletedXid);
	LWLockRelease(ProcArrayLock);

	/*
	 * 若尚未为热备启动 subtrans，则启动它。
	 * （若需要，提交时间戳在下面启动。）
	 */
	if (standbyState == STANDBY_DISABLED)
		StartupSUBTRANS(oldestActiveXID);

	/*
	 * 对任何需要的 SLRU 执行恢复结束动作。
	 */
	TrimCLOG();
	TrimMultiXact();

	/*
	 * 重新加载已准备事务的共享内存状态。
	 * 这需要在把旧时间线最后一个不完整段重命名之前发生，
	 * 因为可能必须恢复某些事务。
	 */
	RecoverPreparedTransactions();

	/* 关闭 xlogreader */
	ShutdownWalRecovery();

	/* 仅为此后端启用 WAL 写入。 */
	LocalSetXLogInsertAllowed();

	/* 关闭 xlogreader */
	if (!XLogRecPtrIsInvalid(abortedRecPtr))
	{
		Assert(!XLogRecPtrIsInvalid(missingContrecPtr));
		CreateOverwriteContrecordRecord(abortedRecPtr, missingContrecPtr, newTLI);
	}

	/*
	 * 在资源管理器写入清理 WAL 记录或写入检查点记录之前，
	 * 更新共享内存中的 full_page_writes
	 * 并写入一条 XLOG_FPW_CHANGE 记录。
	 */
	Insert->fullPageWrites = lastFullPageWrites;
	UpdateFullPageWrites();

	/*
	 * 若需要，在 XLOG 中发出检查点或恢复结束记录。
	 */
	if (performedWalRecovery)
		promoted = PerformRecoveryXLogAction();

	/*
	 * 若任何关键 GUCs 已更改，
	 * 在允许后端写 WAL 之前记录它们。
	 */
	XLogReportParameters();

	/* 若这是归档恢复，执行恢复后的清理动作。 */
	if (ArchiveRecoveryRequested)
		CleanupAfterArchiveRecovery(EndOfLogTLI, EndOfLog, newTLI);

	/*
	 * 本地 WAL 插入已启用，
	 * 因此是时候完成提交时间戳的初始化了。
	 */
	CompleteCommitTsInitialization();

	/*
	 * 恢复结束动作全部完成。
	 *
	 * 现在允许后端写 WAL，
	 * 并相应地更新控制文件状态。
	 * 控制后端能否写 WAL 的 SharedRecoveryState 在持有 ControlFileLock 时更新，
	 * 以防止其他后端看到共享内存中控制文件的不一致状态。
	 * 仍有一个小窗口，
	 * 其间后端可以写 WAL，
	 * 而磁盘上的控制文件在查看时仍显示系统未处于 DB_IN_PRODUCTION 状态。
	 *
	 * 此外，
	 * 我们使用 info_lck 更新 SharedRecoveryState，
	 * 以确保不存在关于共享内存中其他近期更新可见性的竞态。
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->state = DB_IN_PRODUCTION;

	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->SharedRecoveryState = RECOVERY_STATE_DONE;
	SpinLockRelease(&XLogCtl->info_lck);

	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	/*
	 * 关闭恢复环境。
	 * 这必须发生在 RecoverPreparedTransactions()
	 * 之后（见 lock_twophase_recover() 中的说明），
	 * 并且在把 SharedRecoveryState 切换为 RECOVERY_STATE_DONE 之后，
	 * 以便任何构建快照的会话在此阶段（RecoveryInProgress() 将返回 false）不会依赖 KnownAssignedXids。
	 * 这对已准备的 2PC 事务尤其关键，
	 * 恢复结束后它们仍需包含在快照中。
	 */
	if (standbyState != STANDBY_DISABLED)
		ShutdownRecoveryTransactionEnvironment();

	/*
	 * 若曾有级联备库连接到我们，
	 * 则轻推任何 wal sender 进程，使其注意到我们已被提升。
	 */
	WalSndWakeup(true, true);

	/*
	 * 若这是一次提升，
	 * 现在请求一次（在线）检查点。一致性并不要求这样做，
	 * 但上一次重启点可能很久以前，
	 * 若发生崩溃，从那里恢复可能需要很长时间。
	 */
	if (promoted)
		RequestCheckpoint(CHECKPOINT_FORCE);
}

/*
 * 来自 PerformWalRecovery() 的回调，
 * 在我们从崩溃恢复切换到归档恢复模式时调用。相应地更新控制文件。
 */
void
SwitchIntoArchiveRecovery(XLogRecPtr EndRecPtr, TimeLineID replayTLI)
{
	/* 将 minRecoveryPoint 初始化为该记录 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->state = DB_IN_ARCHIVE_RECOVERY;
	if (ControlFile->minRecoveryPoint < EndRecPtr)
	{
		ControlFile->minRecoveryPoint = EndRecPtr;
		ControlFile->minRecoveryPointTLI = replayTLI;
	}
	/* 更新本地副本 */
	LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
	LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;

	/*
	 * 从此时起，
	 * 启动进程可以更新其 minRecoveryPoint 的本地副本。
	 */
	updateMinRecoveryPoint = true;

	UpdateControlFile();

	/*
	 * 我们在持有 ControlFileLock 时更新 SharedRecoveryState，
	 * 以便两个状态在共享内存中一致。
	 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->SharedRecoveryState = RECOVERY_STATE_ARCHIVE;
	SpinLockRelease(&XLogCtl->info_lck);

	LWLockRelease(ControlFileLock);
}

/*
 * 来自 PerformWalRecovery() 的回调，
 * 在我们到达备份结束时调用。相应地更新控制文件。
 */
void
ReachedEndOfBackup(XLogRecPtr EndRecPtr, TimeLineID tli)
{
	/*
	 * 我们已到达 pg_control
	 * 所指示的基础备份结束。
	 * 磁盘上的数据现已一致（除非 
	 * minRecoveryPoint
	 * 更靠前，
	 * 这可能在我们上次恢复期间崩溃时发生）。
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

	if (ControlFile->minRecoveryPoint < EndRecPtr)
	{
		ControlFile->minRecoveryPoint = EndRecPtr;
		ControlFile->minRecoveryPointTLI = tli;
	}

	ControlFile->backupStartPoint = InvalidXLogRecPtr;
	ControlFile->backupEndPoint = InvalidXLogRecPtr;
	ControlFile->backupEndRequired = false;
	UpdateControlFile();

	LWLockRelease(ControlFileLock);
}

/*
 * 在 REDO 结束时执行必要的 XLOG 动作。
 *
 * 目标是确保若再次崩溃我们仍能正确恢复。
 * 若我们选择写检查点，
 * 将写关闭检查点而不是在线检查点。
 * 这并非特别关键，
 * 但由于我们可能正在分配新 TLI，
 * 使用关闭检查点可保持"TLI 仅在关闭检查点中改变"的规则，从而允许 xlog_redo 中的一些额外错误检查。
 */
static bool
PerformRecoveryXLogAction(void)
{
	bool		promoted = false;

	/*
	 * 执行检查点以把我们所有恢复活动更新到磁盘。
	 *
	 * 注意我们写关闭检查点而不是在线检查点。
	 * 这并非特别关键，
	 * 但由于我们可能正在分配新 TLI，
	 * 使用关闭检查点可保持 "TLI 仅在关闭检查点中改变" 的规则，
	 * 从而允许 xlog_redo 中的一些额外错误检查。
	 *
	 * 在提升时，
	 * 仅创建轻量的恢复结束记录而不是完整检查点。
	 * 稍后在我们完全离开恢复模式并已接受查询后会请求一次检查点。
	 */
	if (ArchiveRecoveryRequested && IsUnderPostmaster &&
		PromoteIsTriggered())
	{
		promoted = true;

		/*
		 * 插入一个特殊的 WAL 记录来标记恢复的结束，因为我们
		 * 不执行检查点。这意味着检查点进程
		 * 可能正处在时间平滑的重启点中间，并且
		 * 在此之后可能还会持续几分钟。这听起来很奇怪，
		 * 但效果大致相同，试图
		 * 退出重启点然后执行检查点会更奇怪。我们稍后无论如何都会请求
		 * 检查点，只是为了安全起见。
		 */
		CreateEndOfRecoveryRecord();
	}
	else
	{
		RequestCheckpoint(CHECKPOINT_END_OF_RECOVERY |
						  CHECKPOINT_IMMEDIATE |
						  CHECKPOINT_WAIT);
	}

	return promoted;
}

/*
 * 系统仍在恢复中吗？
 *
 * Unlike testing InRecovery, this works in any process that's connected to
 * shared memory.
 */
bool
RecoveryInProgress(void)
{
	/*
	 * 在离开恢复模式之前，
	 * 我们每次都检查共享状态。我们无法重新进入恢复，
	 * 因此一旦看到共享变量为 false，就不必再检查。
	 */
	if (!LocalRecoveryInProgress)
		return false;
	else
	{
		/*
		 * 使用
		 * volatile 指针以确保我们对共享变量做新鲜读取。
		 */
		volatile XLogCtlData *xlogctl = XLogCtl;

		LocalRecoveryInProgress = (xlogctl->SharedRecoveryState != RECOVERY_STATE_DONE);

		/*
		 * 注意：
		 * 仍在恢复中时我们不需要内存屏障。
		 * 我们可能在返回后立即退出恢复，因此调用者不能依赖 true 表示我们仍在恢复中。
		 */

		return LocalRecoveryInProgress;
	}
}

/*
 * 从共享内存返回当前恢复状态。
 *
 * 返回的状态与控制文件内容保持一致。
 * 关于 RecoveryState 可能取值的细节见 xlog.h。
 */
RecoveryState
GetRecoveryState(void)
{
	RecoveryState retval;

	SpinLockAcquire(&XLogCtl->info_lck);
	retval = XLogCtl->SharedRecoveryState;
	SpinLockRelease(&XLogCtl->info_lck);

	return retval;
}

/*
 * 此进程是否允许插入新的 WAL 记录？通常这本质上等价于
 *
 * !RecoveryInProgress()。
 * 但我们也有在特定进程内强制结果为 true 或 false 的机制，
 * 而不论全局状态如何。
 */
bool
XLogInsertAllowed(void)
{
	/*
	 * XLogInsertAllowed() 返回是否允许插入 WAL。
	 * 在恢复期间通常不允许，
	 * 但在恢复结束时会放开。
	 */
	if (LocalXLogInsertAllowed >= 0)
		return (bool) LocalXLogInsertAllowed;

	/*
	 * 快速路径：本地状态已初始化。
	 */
	if (RecoveryInProgress())
		return false;

	/*
	 * 需要从共享内存
	 * 更新本地状态。
	 */
	LocalXLogInsertAllowed = 1;
	return true;
}

/*
 * 使 XLogInsertAllowed() 仅在当前进程中返回 true。
 *
 * 注意：
 * 稍后允许把 LocalXLogInsertAllowed 切换回 -1，
 *
 * 甚至在那之后再次调用 LocalSetXLogInsertAllowed()。返回 LocalXLogInsertAllowed 的先前值。
 */
static int
LocalSetXLogInsertAllowed(void)
{
	int			oldXLogAllowed = LocalXLogInsertAllowed;

	LocalXLogInsertAllowed = 1;

	return oldXLogAllowed;
}

/*
 * 从共享内存返回当前 Redo 指针。
 *
 * 作为副作用，会更新本地 RedoRecPtr 副本。
 */
XLogRecPtr
GetRedoRecPtr(void)
{
	XLogRecPtr	ptr;

	/*
	 * XlogCtl 中可能并非最新的副本已足够。
	 * 即使我们抓取 WAL 插入锁来读取 Insert->RedoRecPtr
	 * 中的权威值，
	 * 也可能在我们释放锁后立刻有人更新它。
	 */
	SpinLockAcquire(&XLogCtl->info_lck);
	ptr = XLogCtl->RedoRecPtr;
	SpinLockRelease(&XLogCtl->info_lck);

	if (RedoRecPtr < ptr)
		RedoRecPtr = ptr;

	return RedoRecPtr;
}

/*
 * 返回决定已修改块是否需要把全页映像包含在 WAL 记录中所需的信息。
 * 返回值是后端私有内存中的缓存副本，
 *
 * 可能已过时，
 * 甚至未初始化（此时分别为 InvalidXLogRecPtr 和 false）。
 * XLogInsertRecord 会在持有
 * WAL 插入锁时对照最新值重新检查它们。
 */
void
GetFullPageWriteInfo(XLogRecPtr *RedoRecPtr_p, bool *doPageWrites_p)
{
	*RedoRecPtr_p = RedoRecPtr;
	*doPageWrites_p = doPageWrites;
}

/*
 * GetInsertRecPtr -- 返回当前插入位置。
 *
 * 注意：
 * 实际返回的值是最后一个完整 xlog 页的位置。它最多落后真实插入位置 1 页。
 * 为此，
 * 我们不必扫描 WAL 插入锁，且对本函数当前用法来说近似值已足够。
 */
XLogRecPtr
GetInsertRecPtr(void)
{
	XLogRecPtr	recptr;

	SpinLockAcquire(&XLogCtl->info_lck);
	recptr = XLogCtl->LogwrtRqst.Write;
	SpinLockRelease(&XLogCtl->info_lck);

	return recptr;
}

/*
 * GetFlushRecPtr -- 返回当前刷盘位置，
 * 即已知已 fsync 到磁盘的最后 WAL 位置。
 * 这应仅在已知不处于恢复中的系统上使用。
 */
XLogRecPtr
GetFlushRecPtr(TimeLineID *insertTLI)
{
	Assert(XLogCtl->SharedRecoveryState == RECOVERY_STATE_DONE);

	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * 若我们正在写并刷盘 WAL，
	 * 时间线不可能正在改变，因此不需要锁。
	 */
	if (insertTLI)
		*insertTLI = XLogCtl->InsertTimeLineID;

	return LogwrtResult.Flush;
}

/*
 * GetWALInsertionTimeLine
 * -- 返回不处于恢复中的系统的当前时间线。
 */
TimeLineID
GetWALInsertionTimeLine(void)
{
	Assert(XLogCtl->SharedRecoveryState == RECOVERY_STATE_DONE);

	/* 由于该值不会改变，不需要锁。 */
	return XLogCtl->InsertTimeLineID;
}

/*
 * GetLastImportantRecPtr
 * -- 返回最后插入的重要记录的 LSN。
 * 所有未显式标记为不重要的记录都视为重要。
 *
 * LSN 通过计算 WALInsertLocks[i].lastImportantAt
 * 的最大值确定。
 */
XLogRecPtr
GetLastImportantRecPtr(void)
{
	XLogRecPtr	res = InvalidXLogRecPtr;
	int			i;

	for (i = 0; i < NUM_XLOGINSERT_LOCKS; i++)
	{
		XLogRecPtr	last_important;

		/*
		 * 需要加锁以防止 LSN 的撕裂读取，
		 * 这在某些受支持平台上可能发生。
		 * WAL 插入锁仅支持排他模式，因此我们必须用排他模式。
		 */
		LWLockAcquire(&WALInsertLocks[i].l.lock, LW_EXCLUSIVE);
		last_important = WALInsertLocks[i].l.lastImportantAt;
		LWLockRelease(&WALInsertLocks[i].l.lock);

		if (res < last_important)
			res = last_important;
	}

	return res;
}

/*
 * 获取上次 xlog 段切换的时间与 LSN
 */
pg_time_t
GetLastSegSwitchData(XLogRecPtr *lastSwitchLSN)
{
	pg_time_t	result;

	/* 需要 WALWriteLock，但共享锁足够 */
	LWLockAcquire(WALWriteLock, LW_SHARED);
	result = XLogCtl->lastSegSwitchTime;
	*lastSwitchLSN = XLogCtl->lastSegSwitchLSN;
	LWLockRelease(WALWriteLock);

	return result;
}

/*
 * XLogCheckpointNeeded() 检查是否该做检查点。
 */
void
ShutdownXLOG(int code, Datum arg)
{
	/*
	 * RequestXLogSwitch --
	 * 请求切换到新的 WAL 段。
	 */
	Assert(AuxProcessResourceOwner != NULL);
	Assert(CurrentResourceOwner == NULL ||
		   CurrentResourceOwner == AuxProcessResourceOwner);
	CurrentResourceOwner = AuxProcessResourceOwner;

	/* 独立模式下不要过于啰嗦 */
	ereport(IsPostmasterEnvironment ? LOG : NOTICE,
			(errmsg("shutting down")));

	/*
	 * GetLastImportantRecPtr -- 获取最后一条重要记录的 LSN。
	 */
	WalSndInitStopping();

	/*
	 * 等待 WAL sender 进入停止状态。
	 * 这可防止命令写入新的 WAL。
	 */
	WalSndWaitStopping();

	if (RecoveryInProgress())
		CreateRestartPoint(CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_IMMEDIATE);
	else
	{
		/*
		 * GetInsert
		 * ionRecPtr
		 * --
		 * 获取当前插入位置（粗略）。
		 */
		if (XLogArchivingActive())
			RequestXLogSwitch(false);

		CreateCheckPoint(CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_IMMEDIATE);
	}
}

/*
 * GetXLogReceiptTime -- 获取最近收到 WAL 的时间。
 */
static void
LogCheckpointStart(int flags, bool restartpoint)
{
	if (restartpoint)
		ereport(LOG,
		/* translator: the placeholders show checkpoint options */
				(errmsg("restartpoint starting:%s%s%s%s%s%s%s%s",
						(flags & CHECKPOINT_IS_SHUTDOWN) ? " shutdown" : "",
						(flags & CHECKPOINT_END_OF_RECOVERY) ? " end-of-recovery" : "",
						(flags & CHECKPOINT_IMMEDIATE) ? " immediate" : "",
						(flags & CHECKPOINT_FORCE) ? " force" : "",
						(flags & CHECKPOINT_WAIT) ? " wait" : "",
						(flags & CHECKPOINT_CAUSE_XLOG) ? " wal" : "",
						(flags & CHECKPOINT_CAUSE_TIME) ? " time" : "",
						(flags & CHECKPOINT_FLUSH_ALL) ? " flush-all" : "")));
	else
		ereport(LOG,
		/* translator: the placeholders show checkpoint options */
				(errmsg("checkpoint starting:%s%s%s%s%s%s%s%s",
						(flags & CHECKPOINT_IS_SHUTDOWN) ? " shutdown" : "",
						(flags & CHECKPOINT_END_OF_RECOVERY) ? " end-of-recovery" : "",
						(flags & CHECKPOINT_IMMEDIATE) ? " immediate" : "",
						(flags & CHECKPOINT_FORCE) ? " force" : "",
						(flags & CHECKPOINT_WAIT) ? " wait" : "",
						(flags & CHECKPOINT_CAUSE_XLOG) ? " wal" : "",
						(flags & CHECKPOINT_CAUSE_TIME) ? " time" : "",
						(flags & CHECKPOINT_FLUSH_ALL) ? " flush-all" : "")));
}

/*
 * GetCurrentChunkReplayStartTime -- 获取当前块重放开始时间。
 */
static void
LogCheckpointEnd(bool restartpoint)
{
	long		write_msecs,
				sync_msecs,
				total_msecs,
				longest_msecs,
				average_msecs;
	uint64		average_sync_time;

	CheckpointStats.ckpt_end_t = GetCurrentTimestamp();

	write_msecs = TimestampDifferenceMilliseconds(CheckpointStats.ckpt_write_t,
												  CheckpointStats.ckpt_sync_t);

	sync_msecs = TimestampDifferenceMilliseconds(CheckpointStats.ckpt_sync_t,
												 CheckpointStats.ckpt_sync_end_t);

	/* 以毫秒累计检查点计时摘要数据。 */
	PendingCheckpointerStats.checkpoint_write_time += write_msecs;
	PendingCheckpointerStats.checkpoint_sync_time += sync_msecs;

	/*
	 * CreateCheckPoint
	 * -- 执行检查点。
	 */
	if (!log_checkpoints)
		return;

	total_msecs = TimestampDifferenceMilliseconds(CheckpointStats.ckpt_start_t,
												  CheckpointStats.ckpt_end_t);

	/*
	 * CheckpointStats 返回的计时值以微秒为单位。
	 * 转换为毫秒以便一致地打印。
	 */
	longest_msecs = (long) ((CheckpointStats.ckpt_longest_sync + 999) / 1000);

	average_sync_time = 0;
	if (CheckpointStats.ckpt_sync_rels > 0)
		average_sync_time = CheckpointStats.ckpt_agg_sync_time /
			CheckpointStats.ckpt_sync_rels;
	average_msecs = (long) ((average_sync_time + 999) / 1000);

	/*
	 * 此处查看 ControlFile->checkPoint 与 ->checkPointCopy
	 * 不需要 ControlFileLock，
	 * 因为此刻我们是这些变量的唯一更新者。
	 */
	if (restartpoint)
		ereport(LOG,
				(errmsg("restartpoint complete: wrote %d buffers (%.1f%%); "
						"%d WAL file(s) added, %d removed, %d recycled; "
						"write=%ld.%03d s, sync=%ld.%03d s, total=%ld.%03d s; "
						"sync files=%d, longest=%ld.%03d s, average=%ld.%03d s; "
						"distance=%d kB, estimate=%d kB; "
						"lsn=%X/%X, redo lsn=%X/%X",
						CheckpointStats.ckpt_bufs_written,
						(double) CheckpointStats.ckpt_bufs_written * 100 / NBuffers,
						CheckpointStats.ckpt_segs_added,
						CheckpointStats.ckpt_segs_removed,
						CheckpointStats.ckpt_segs_recycled,
						write_msecs / 1000, (int) (write_msecs % 1000),
						sync_msecs / 1000, (int) (sync_msecs % 1000),
						total_msecs / 1000, (int) (total_msecs % 1000),
						CheckpointStats.ckpt_sync_rels,
						longest_msecs / 1000, (int) (longest_msecs % 1000),
						average_msecs / 1000, (int) (average_msecs % 1000),
						(int) (PrevCheckPointDistance / 1024.0),
						(int) (CheckPointDistanceEstimate / 1024.0),
						LSN_FORMAT_ARGS(ControlFile->checkPoint),
						LSN_FORMAT_ARGS(ControlFile->checkPointCopy.redo))));
	else
		ereport(LOG,
				(errmsg("checkpoint complete: wrote %d buffers (%.1f%%); "
						"%d WAL file(s) added, %d removed, %d recycled; "
						"write=%ld.%03d s, sync=%ld.%03d s, total=%ld.%03d s; "
						"sync files=%d, longest=%ld.%03d s, average=%ld.%03d s; "
						"distance=%d kB, estimate=%d kB; "
						"lsn=%X/%X, redo lsn=%X/%X",
						CheckpointStats.ckpt_bufs_written,
						(double) CheckpointStats.ckpt_bufs_written * 100 / NBuffers,
						CheckpointStats.ckpt_segs_added,
						CheckpointStats.ckpt_segs_removed,
						CheckpointStats.ckpt_segs_recycled,
						write_msecs / 1000, (int) (write_msecs % 1000),
						sync_msecs / 1000, (int) (sync_msecs % 1000),
						total_msecs / 1000, (int) (total_msecs % 1000),
						CheckpointStats.ckpt_sync_rels,
						longest_msecs / 1000, (int) (longest_msecs % 1000),
						average_msecs / 1000, (int) (average_msecs % 1000),
						(int) (PrevCheckPointDistance / 1024.0),
						(int) (CheckPointDistanceEstimate / 1024.0),
						LSN_FORMAT_ARGS(ControlFile->checkPoint),
						LSN_FORMAT_ARGS(ControlFile->checkPointCopy.redo))));
}

/*
 * 更新检查点间距的估计。
 *
 * 该估计用于计算要预分配的 WAL 段数量，
 * 见 XLOGfileslop()。
 */
static void
UpdateCheckPointDistanceEstimate(uint64 nbytes)
{
	/*
	 * 为估计检查点之间消耗的段数，
	 * 对先前检查点周期中生成的 WAL 量保持移动平均。
	 * 然而，
	 * 若负载突发性强，
	 * 有安静期和繁忙期，我们希望满足峰值负载。
	 * 因此不用普通移动平均，
	 * 而是若上一周期使用的 WAL 少于估计则让平均值缓慢下降，
	 *
	 * 若使用更多则立即上调。
	 * 当检查点由 max_wal_size 触发时，这应收敛到 CheckpointSegments * wal_segment_size。
	 *
	 * 注意：
	 * 这不关注检查点的触发原因。
	 * 用 CHECKPOINT 命令手动触发的检查点，
	 * 或例如启动基础备份触发的检查点，
	 * 与自动创建的计入方式相同。若它们不频繁，
	 * 缓慢下降会在很大程度上掩盖它们。
	 * 若频繁，
	 * 把它们与其他一样计入似乎合理；
	 * 若你每 5 分钟发一次手动检查点且从不让定时检查点发生，则基于该 5 分钟间隔而不是 checkpoint_timeout 来预分配是合理的。
	 */
	PrevCheckPointDistance = nbytes;
	if (CheckPointDistanceEstimate < nbytes)
		CheckPointDistanceEstimate = nbytes;
	else
		CheckPointDistanceEstimate =
			(0.90 * CheckPointDistanceEstimate + 0.10 * (double) nbytes);
}

/*
 * 更新共享
 * 内存中的
 * 检查点相关字段。
 */
static void
update_checkpoint_display(int flags, bool restartpoint, bool reset)
{
	/*
	 * 仅对恢复结束和关闭检查点或关闭重启点报告状态。
	 * 在这些情况下更新 ps 显示很有用，
	 * 因为可能无法依赖 
	 * pg_stat_activity
	 * 查看 checkpointer 或启动进程的状态。
	 */
	if ((flags & (CHECKPOINT_END_OF_RECOVERY | CHECKPOINT_IS_SHUTDOWN)) == 0)
		return;

	if (reset)
		set_ps_display("");
	else
	{
		char		activitymsg[128];

		snprintf(activitymsg, sizeof(activitymsg), "performing %s%s%s",
				 (flags & CHECKPOINT_END_OF_RECOVERY) ? "end-of-recovery " : "",
				 (flags & CHECKPOINT_IS_SHUTDOWN) ? "shutdown " : "",
				 restartpoint ? "restartpoint" : "checkpoint");
		set_ps_display(activitymsg);
	}
}


/*
 * 执行检查点——在关闭期间或动态进行。
 *
 * flags 是以下位的按位或：
 *	CHECKPOINT_IS_SHUTDOWN：为数据库关闭做检查点。
 *	CHECKPOINT_END_OF_RECOVERY：
 *	为 WAL 恢复结束做检查点。
 *		CHECKPOINT_IMMEDIATE：尽快完成检查点，
 *	忽略 checkpoint_completion_target 参数。
 *		CHECKPOINT_FORCE：即使自上次以来没有 XLOG 活动也强制检查点（由 CHECKPOINT_IS_SHUTDOWN 或 CHECKPOINT_END_OF_RECOVERY 隐含）。
 *		CHECKPOINT_FLUSH_ALL：
 *	也刷盘无日志表的缓冲区。
 *
 * 注意：flags 还包含其他位，
 * 这里仅对日志记录有意义。
 * 特别注意本例程是同步的，
 *
 * 不关注 CHECKPOINT_WAIT。若非 shutdown，
 * 则我们在写在线检查点。
 * 这是非常特殊的操作和 WAL 记录，因为检查点动作跨越一段时间却在逻辑上发生在单个 LSN。
 * WAL 记录的逻辑位置（redo ptr）与物理位置相同或更早。
 * 重放 WAL 时，
 * 我们通过物理位置定位检查点，然后读取 redo ptr，
 * 并实际从更早的逻辑位置开始重放。
 * 注意我们不在逻辑位置写入任何内容，
 * 因此该位置可以是任何其他种类的 WAL 记录。整套机制让我们能在检查点时继续工作。
 * 因此，
 * 这里的动作时序至关重要；请注意在繁忙系统上本函数可能需要数分钟执行。
 */
void
CreateCheckPoint(int flags)
{
	bool		shutdown;
	CheckPoint	checkPoint;
	XLogRecPtr	recptr;
	XLogSegNo	_logSegNo;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint32		freespace;
	XLogRecPtr	PriorRedoPtr;
	XLogRecPtr	curInsert;
	XLogRecPtr	last_important_lsn;
	VirtualTransactionId *vxids;
	int			nvxids;
	int			oldXLogAllowed = 0;
	XLogRecPtr	slotsMinReqLSN;

	/*
	 * 同步数
	 * 据目录。
	 */
	if (flags & (CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_END_OF_RECOVERY))
		shutdown = true;
	else
		shutdown = false;

	/* 健全性检查 */
	if (RecoveryInProgress() && (flags & CHECKPOINT_END_OF_RECOVERY) == 0)
		elog(ERROR, "can't create a checkpoint during recovery");

	/*
	 * 准备累计统计信息。
	 *
	 * 注意：
	 * 由于检查点进行期间 log_checkpoints 可能改变，
	 * 我们总是累计统计，即使当前 log_checkpoints 为关。
	 */
	MemSet(&CheckpointStats, 0, sizeof(CheckpointStats));
	CheckpointStats.ckpt_start_t = GetCurrentTimestamp();

	/*
	 * 删除不再
	 * 需要的旧
	 * WAL
	 * 文件。
	 */
	SyncPreCheckpoint();

	/*
	 * 请求检查点。
	 */
	START_CRIT_SECTION();

	if (shutdown)
	{
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->state = DB_SHUTDOWNING;
		UpdateControlFile();
		LWLockRelease(ControlFileLock);
	}

	/* 开始填充检查点 WAL 记录 */
	MemSet(&checkPoint, 0, sizeof(checkPoint));
	checkPoint.time = (pg_time_t) time(NULL);

	/*
	 * 后台写进程
	 * 调用的
	 * 接口。
	 */
	if (!shutdown && XLogStandbyInfoActive())
		checkPoint.oldestActiveXid = GetOldestActiveTransactionId();
	else
		checkPoint.oldestActiveXid = InvalidTransactionId;

	/*
	 * 在获取插入锁之前获取最后重要记录的位置（因为 GetLastImportantRecPtr()
	 * 也会锁定 WAL 锁）。
	 */
	last_important_lsn = GetLastImportantRecPtr();

	/*
	 * 执行一次检查
	 * 点或重启点。
	 */
	WALInsertLockAcquireExclusive();
	curInsert = XLogBytePosToRecPtr(Insert->CurrBytePos);

	/*
	 * 若这不是关闭或强制检查点，且没有需要检查点的 WAL 活动，
	 * 则跳过。这里的想法是在系统空闲时避免插入重复检查点。
	 * 不过若上次检查点已很久，仍要做一个，以免 WAL 无限增长。
	 */
	if ((flags & (CHECKPOINT_IS_SHUTDOWN | CHECKPOINT_END_OF_RECOVERY |
				  CHECKPOINT_FORCE)) == 0)
	{
		if (last_important_lsn == ControlFile->checkPoint)
		{
			WALInsertLockRelease();
			END_CRIT_SECTION();
			ereport(DEBUG1,
					(errmsg_internal("checkpoint skipped because system is idle")));
			return;
		}
	}

	/*
	 * 在允许任何人写 WAL 之前创建恢复结束检查点。
	 * 为允许我们写入检查点记录，
	 * 临时启用 XLogInsertAllowed。
	 */
	if (flags & CHECKPOINT_END_OF_RECOVERY)
		oldXLogAllowed = LocalSetXLogInsertAllowed();

	checkPoint.ThisTimeLineID = XLogCtl->InsertTimeLineID;
	if (flags & CHECKPOINT_END_OF_RECOVERY)
		checkPoint.PrevTimeLineID = XLogCtl->PrevTimeLineID;
	else
		checkPoint.PrevTimeLineID = checkPoint.ThisTimeLineID;

	checkPoint.fullPageWrites = Insert->fullPageWrites;

	/*
	 * 记录
	 *
	 * 检查
	 * 点完
	 * 成。
	 *
	 */
	freespace = INSERT_FREESPACE(curInsert);
	if (freespace == 0)
	{
		if (XLogSegmentOffset(curInsert, wal_segment_size) == 0)
			curInsert += SizeOfXLogLongPHD;
		else
			curInsert += SizeOfXLogShortPHD;
	}
	checkPoint.redo = curInsert;

	/*
	 * 这里我们更新共享的 RedoRecPtr 供将来的 XLogInsert 调用使用；
	 * 这必须在持有所有插入锁时完成。
	 *
	 * 注意：
	 * 若我们未能完成检查点，
	 * RedoRecPtr 会留在超过它真正需要指向的位置。
	 * 这没关系；
	 * 唯一后果是 XLogInsert 可能备份它其实并不需要的整块缓冲区。
	 * 我们不能推迟推进 RedoRecPtr，因为在我们转储缓冲区时发生的 XLogInsert 必须假定其缓冲区更改未包含在检查点中。
	 */
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;

	/*
	 * 现在可以释放 WAL 插入锁，
	 * 允许其他事务在我们刷盘磁盘缓冲区时继续进行。
	 */
	WALInsertLockRelease();

	/* 同时更新受 info_lck 保护的 RedoRecPtr 副本 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->RedoRecPtr = checkPoint.redo;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * 处理
	 * 信号。
	 */
	if (log_checkpoints)
		LogCheckpointStart(flags, false);

	/* 更新进程标题 */
	update_checkpoint_display(flags, false, false);

	TRACE_POSTGRESQL_CHECKPOINT_START(flags);

	/*
	 * Crea
	 *
	 * teRes
	 * tart
	 * Point
	 * 的实现。
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	checkPoint.nextXid = ShmemVariableCache->nextXid;
	checkPoint.oldestXid = ShmemVariableCache->oldestXid;
	checkPoint.oldestXidDB = ShmemVariableCache->oldestXidDB;
	LWLockRelease(XidGenLock);

	LWLockAcquire(CommitTsLock, LW_SHARED);
	checkPoint.oldestCommitTsXid = ShmemVariableCache->oldestCommitTsXid;
	checkPoint.newestCommitTsXid = ShmemVariableCache->newestCommitTsXid;
	LWLockRelease(CommitTsLock);

	LWLockAcquire(OidGenLock, LW_SHARED);
	checkPoint.nextOid = ShmemVariableCache->nextOid;
	if (!shutdown)
		checkPoint.nextOid += ShmemVariableCache->oidCount;
	LWLockRelease(OidGenLock);

	MultiXactGetCheckptMulti(shutdown,
							 &checkPoint.nextMulti,
							 &checkPoint.nextMultiOffset,
							 &checkPoint.oldestMulti,
							 &checkPoint.oldestMultiDB);

	/*
	 * 与 
	 * CreateCh
	 *
	 * eckPoint
	 * 类似，
	 * 但在恢复期间运行。
	 */
	END_CRIT_SECTION();

	/*
	 * 获取当前最小 LSN，
	 * 供稍后 WAL 段清理使用。
	 * 我们只能清理根据复制槽的同步 LSN 不再需要的 WAL 段。
	 * 槽的 LSN 可能并发推进，
	 * 因此我们在 CheckPointReplicationSlots() 同步复制槽之前调用此函数。
	 */
	slotsMinReqLSN = XLogGetReplicationSlotMinimumLSN();

	/*
	 * 在某些情况下，
	 * 有些动作组必须全部发生在检查点记录的一侧或另一侧。
	 * 在刷盘检查点记录之前，
	 * 我们必须显式等待任何当前正在执行那些动作组的后端。
	 *
	 * 一个例子是事务结束，
	 * 因此我们必须等待任何当前处于提交临界区的事务。
	 * 若某事务刚好在 REDO 点之前插入了其提交记录，
	 * 则从 REDO 点崩溃重启不会重放该记录，
	 * 这意味着我们的刷盘最好包含该事务对 pg_xact 的更新。
	 * 因此我们等到他离开提交临界区再继续。
	 * 见 RecordTransactionCommit() 中的说明。
	 *
	 * 由于我们已释放插入锁，此测试有点模糊：
	 * 我们可能等待其实不必等待的事务。
	 * 但延迟应很短，
	 * 让检查点稍长似乎好过不必要地长时间阻止插入。
	 * （实际上，
	 * 我们有此问题的原因是 xact.c 把提交记录 XLOG 插入和 clog 更新作为由不同锁保护的两个步骤，
	 * 但同样基于最小化锁争用，
	 * 那样似乎最好。
	 *
	 * ）我们查看时尚未设置 delayChkptFlags 的事务没有风险，
	 * 因为它尚未插入提交记录；
	 * 已清除该标志的也没有风险，
	 * 因为它已完成修复 clog，
	 * 我们将在下面正确刷盘更新。因此我们不会漏掉任何需要等待的事务。
	 */
	vxids = GetVirtualXIDsDelayingChkpt(&nvxids, DELAY_CHKPT_START);
	if (nvxids > 0)
	{
		do
		{
			/*
			 * 同步
			 * 并更新
			 * 控制文件。
			 */
			AbsorbSyncRequests();
			pg_usleep(10000L);	/* 等待 10 毫秒 */
		} while (HaveVirtualXIDsDelayingChkpt(vxids, nvxids,
											  DELAY_CHKPT_START));
	}
	pfree(vxids);

	CheckPointGuts(checkPoint.redo, flags);

	vxids = GetVirtualXIDsDelayingChkpt(&nvxids, DELAY_CHKPT_COMPLETE);
	if (nvxids > 0)
	{
		do
		{
			AbsorbSyncRequests();
			pg_usleep(10000L);	/* 等待 10 毫秒 */
		} while (HaveVirtualXIDsDelayingChkpt(vxids, nvxids,
											  DELAY_CHKPT_COMPLETE));
	}
	pfree(vxids);

	/*
	 * 对运行中事务拍快照并写入
	 * WAL。
	 * 这使我们能在需要时于归档恢
	 *
	 * 复期间重建运行中事务的状态。
	 * 若在热备中不需要此信息则跳过。
	 */
	if (!shutdown && XLogStandbyInfoActive())
		LogStandbySnapshot();

	START_CRIT_SECTION();

	/*
	 * 更新统计。
	 */
	XLogBeginInsert();
	XLogRegisterData((char *) (&checkPoint), sizeof(checkPoint));
	recptr = XLogInsert(RM_XLOG_ID,
						shutdown ? XLOG_CHECKPOINT_SHUTDOWN :
						XLOG_CHECKPOINT_ONLINE);

	XLogFlush(recptr);

	/*
	 * 关闭检查点之后我们绝不能再写任何新 WAL，
	 * 否则会在下次启动时被覆盖。
	 * 甚至不应有人尝试，
	 * 这只是健全性检查。
	 * 在恢复结束检查点的情况下，我们很快就会重新启用 WAL 写入。
	 */
	if (shutdown)
	{
		if (flags & CHECKPOINT_END_OF_RECOVERY)
			LocalXLogInsertAllowed = oldXLogAllowed;
		else
			LocalXLogInsertAllowed = 0; /* 从此不再写入 WAL */
	}

	/*
	 * 现在我们有 ProcLastRecPtr = 实际检查点记录的起点，
	 * recptr = 实际检查点记录的终点。
	 */
	if (shutdown && checkPoint.redo != ProcLastRecPtr)
		ereport(PANIC,
				(errmsg("concurrent write-ahead log activity while database system is shutting down")));

	/*
	 * 记住先前检查点的 redo ptr，
	 * 供 UpdateCheckPointDistanceEstimate() 使用。
	 */
	PriorRedoPtr = ControlFile->checkPointCopy.redo;

	/*
	 * 切换到下一文件。
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	if (shutdown)
		ControlFile->state = DB_SHUTDOWNED;
	ControlFile->checkPoint = ProcLastRecPtr;
	ControlFile->checkPointCopy = checkPoint;
	/* 崩溃恢复应始终恢复到 WAL 末尾 */
	ControlFile->minRecoveryPoint = InvalidXLogRecPtr;
	ControlFile->minRecoveryPointTLI = 0;

	/*
	 * WaitForWALToBecomeAvailable
	 * -- 等待
	 * WAL 可用。
	 */
	SpinLockAcquire(&XLogCtl->ulsn_lck);
	ControlFile->unloggedLSN = XLogCtl->unloggedLSN;
	SpinLockRelease(&XLogCtl->ulsn_lck);

	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	/* 更新共享内存中的检查点 XID/epoch 副本 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->ckptFullXid = checkPoint.nextXid;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * 尝试从归
	 * 档恢复。
	 */
	END_CRIT_SECTION();

	/*
	 * 尝试从 pg_wal 读取。
	 */
	SyncPostCheckpoint();

	/*
	 * 尝试从流
	 * 复制接收。
	 */
	if (PriorRedoPtr != InvalidXLogRecPtr)
		UpdateCheckPointDistanceEstimate(RedoRecPtr - PriorRedoPtr);

	/*
	 * 处理
	 * 失败。
	 */
	XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
	KeepLogSeg(recptr, slotsMinReqLSN, &_logSegNo);
	if (InvalidateObsoleteReplicationSlots(RS_INVAL_WAL_REMOVED,
										   _logSegNo, InvalidOid,
										   InvalidTransactionId))
	{
		/*
		 * 重新计算用于 WAL 段清理的当前最小 LSN。
		 * 然后必须再次同步复制槽，
		 * 才能安全使用该 LSN。
		 */
		slotsMinReqLSN = XLogGetReplicationSlotMinimumLSN();
		CheckPointReplicationSlots();

		/*
		 * 某些槽已失效；
		 * 从 RedoRecPtr 重新开始重新计算旧段边界。
		 */
		XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
		KeepLogSeg(recptr, slotsMinReqLSN, &_logSegNo);
	}
	_logSegNo--;
	RemoveOldXlogFiles(_logSegNo, RedoRecPtr, recptr,
					   checkPoint.ThisTimeLineID);

	/*
	 * 返回
	 * 状态。
	 */
	if (!shutdown)
		PreallocXlogFiles(recptr, checkPoint.ThisTimeLineID);

	/*
	 * 若可能则截断 pg_subtrans。
	 * 我们可以丢弃任何运行中事务的最旧 XMIN 之前的所有数据。
	 * 未来事务不会试图引用比那更旧的任何 pg_subtrans 条目（见 subtrans.c 中的断言）。
	 * 不过在恢复期间我们绝不能这样做，
	 * 因为尚未调用 StartupSUBTRANS。
	 */
	if (!RecoveryInProgress())
		TruncateSUBTRANS(GetOldestTransactionIdConsideredRunning());

	/* 实际工作已完成；记录日志并更新统计。 */
	LogCheckpointEnd(false);

	/* 实际工作已完成；记录日志并更新统计。 */
	update_checkpoint_display(flags, false, true);

	TRACE_POSTGRESQL_CHECKPOINT_DONE(CheckpointStats.ckpt_bufs_written,
									 NBuffers,
									 CheckpointStats.ckpt_segs_added,
									 CheckpointStats.ckpt_segs_removed,
									 CheckpointStats.ckpt_segs_recycled);
}

/*
 * 在WAL中标记恢复的结束，但不运行完整检查点。
 * 我们可以预料到，当
 * 执行此操作时，重启点可能正在进行中，尽管我们不愿意等待它完成。
 *
 * CreateRestartPoint() 允许恢复可能在
 * 重启点完成之前结束的情况，因此不用担心并发行为。
 */
static void
CreateEndOfRecoveryRecord(void)
{
	xl_end_of_recovery xlrec;
	XLogRecPtr	recptr;

	/* 重置进程标题 */
	if (!RecoveryInProgress())
		elog(ERROR, "can only be used to end recovery");

	xlrec.end_time = GetCurrentTimestamp();

	WALInsertLockAcquireExclusive();
	xlrec.ThisTimeLineID = XLogCtl->InsertTimeLineID;
	xlrec.PrevTimeLineID = XLogCtl->PrevTimeLineID;
	WALInsertLockRelease();

	START_CRIT_SECTION();

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_end_of_recovery));
	recptr = XLogInsert(RM_XLOG_ID, XLOG_END_OF_RECOVERY);

	XLogFlush(recptr);

	/*
	 * XLogFileRead
	 * 的包装。
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	ControlFile->minRecoveryPoint = recptr;
	ControlFile->minRecoveryPointTLI = xlrec.ThisTimeLineID;
	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	END_CRIT_SECTION();
}

/*
 * 写一条 OVERWRITE_CONTRECORD 消息。
 *
 * 在 WAL 重放时，
 * 若我们预期页首有续页记录却没有，
 * 则恢复结束并在该点恢复写 WAL。
 * 但若回到被打断记录的起点恢复写新 WAL 是错误的，
 * 因为该 WAL 的下游消费者（物理副本）并未准备好 "rewind"。
 * 因此，
 * 完成所有有效 WAL 重放后的第一个动作必须是在 contrecord 缺失处写入此类记录；
 * 为支持 xlogreader 检测该特例，
 * 还会在记录所在页头处加上 XLP_FIRST_IS_OVERWRITE_CONTRECORD。
 * xlogreader 有专门机制报告被打断记录的元数据，
 * 我们这里就用它。
 *
 * 重放时，
 * XLP_FIRST_IS_OVERWRITE_CONTRECORD 指示 xlogreader 跳过正在读的记录，
 * 并传回被跳过记录的 LSN，
 * 以便调用者在 "replay" 该记录时验证 XLOG_OVERWRITE_CONTRECORD 与实际覆盖内容匹配。
 *
 * 'aborted_lsn' 是不完整记录的起始位置，
 * 包含在 WAL 记录中。
 * 'pagePtr' 与 'newTLI' 指向将插入记录的 XLOG 页开头。
 * 它们必须与当前 WAL 插入位置匹配，
 * 这里传入只是为了验证。
 */
static XLogRecPtr
CreateOverwriteContrecordRecord(XLogRecPtr aborted_lsn, XLogRecPtr pagePtr,
								TimeLineID newTLI)
{
	xl_overwrite_contrecord xlrec;
	XLogRecPtr	recptr;
	XLogPageHeader pagehdr;
	XLogRecPtr	startPos;

	/* 健全性检查 */
	if (!RecoveryInProgress())
		elog(ERROR, "can only be used at end of recovery");
	if (pagePtr % XLOG_BLCKSZ != 0)
		elog(ERROR, "invalid position for missing continuation record %X/%X",
			 LSN_FORMAT_ARGS(pagePtr));

	/* 当前 WAL 插入位置应该紧跟在页面头之后 */
	startPos = pagePtr;
	if (XLogSegmentOffset(startPos, wal_segment_size) == 0)
		startPos += SizeOfXLogLongPHD;
	else
		startPos += SizeOfXLogShortPHD;
	recptr = GetXLogInsertRecPtr();
	if (recptr != startPos)
		elog(ERROR, "invalid WAL insert position %X/%X for OVERWRITE_CONTRECORD",
			 LSN_FORMAT_ARGS(recptr));

	START_CRIT_SECTION();

	/*
	 * 初始化 XLOG 页头（通过 GetXLogBuffer），
	 * 并设置 XLP_FIRST_IS_OVERWRITE_CONTRECORD 标志。
	 *
	 * 尚不允许其他后端写 WAL，
	 * 因此获取 WAL 插入锁只是形式上的。
	 */
	WALInsertLockAcquire();
	pagehdr = (XLogPageHeader) GetXLogBuffer(pagePtr, newTLI);
	pagehdr->xlp_info |= XLP_FIRST_IS_OVERWRITE_CONTRECORD;
	WALInsertLockRelease();

	/*
	 * 把 XLOG_OVERWRITE_CONTRECORD 记录作为页上第一条记录插入。
	 * 我们知道它会成为第一条记录，
	 * 因为尚不允许其他后端写 WAL。
	 */
	XLogBeginInsert();
	xlrec.overwritten_lsn = aborted_lsn;
	xlrec.overwrite_time = GetCurrentTimestamp();
	XLogRegisterData((char *) &xlrec, sizeof(xl_overwrite_contrecord));
	recptr = XLogInsert(RM_XLOG_ID, XLOG_OVERWRITE_CONTRECORD);

	/* 检查记录是否插入到正确位置 */
	if (ProcLastRecPtr != startPos)
		elog(ERROR, "OVERWRITE_CONTRECORD was inserted to unexpected position %X/%X",
			 LSN_FORMAT_ARGS(ProcLastRecPtr));

	XLogFlush(recptr);

	END_CRIT_SECTION();

	return recptr;
}

/*
 * 读取
 *
 * 记录。
 *
 */
static void
CheckPointGuts(XLogRecPtr checkPointRedo, int flags)
{
	CheckPointRelationMap();
	CheckPointReplicationSlots();
	CheckPointSnapBuild();
	CheckPointLogicalRewriteHeap();
	CheckPointReplicationOrigin();

	/* 写出 SLRU 与主缓冲池中的所有脏数据 */
	TRACE_POSTGRESQL_BUFFER_CHECKPOINT_START(flags);
	CheckpointStats.ckpt_write_t = GetCurrentTimestamp();
	CheckPointCLOG();
	CheckPointCommitTs();
	CheckPointSUBTRANS();
	CheckPointMultiXact();
	CheckPointPredicate();
	CheckPointBuffers(flags);

	/* 写出 SLRU 和主缓冲池中的所有脏数据 */
	TRACE_POSTGRESQL_BUFFER_CHECKPOINT_SYNC_START();
	CheckpointStats.ckpt_sync_t = GetCurrentTimestamp();
	ProcessSyncRequests();
	CheckpointStats.ckpt_sync_end_t = GetCurrentTimestamp();
	TRACE_POSTGRESQL_BUFFER_CHECKPOINT_DONE();

	/* 执行所有排队的 fsync */
	CheckPointTwoPhase(checkPointRedo);
}

/*
 * 若合适则为恢复重启保存检查点。
 *
 * 每当从 XLOG 读取一条检查点记录时调用本函数。
 * 它必须判定该检查点是否代表安全的重启点。
 * 若是，则把检查点记录存放在共享内存中，
 * 以便 CreateRestartPoint 查阅。
 * （注意后者由 checkpointer 执行，
 * 而本函数由启动进程执行。）
 */
static void
RecoveryRestartPoint(const CheckPoint *checkPoint, XLogReaderState *record)
{
	/*
	 * 处理
	 * 错误。
	 *
	 *
	 *
	 */
	if (XLogHaveInvalidPages())
	{
		elog(trace_recovery(DEBUG2),
			 "could not record restart point at %X/%X because there "
			 "are unresolved references to invalid pages",
			 LSN_FORMAT_ARGS(checkPoint->redo));
		return;
	}

	/*
	 * 推进
	 * 位置。
	 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->lastCheckPointRecPtr = record->ReadRecPtr;
	XLogCtl->lastCheckPointEndPtr = record->EndRecPtr;
	XLogCtl->lastCheckPoint = *checkPoint;
	SpinLockRelease(&XLogCtl->info_lck);
}

/*
 * 若可能则建立重启点。
 *
 * 这与 CreateCheckPoint 类似，
 * 但在 WAL 
 * 恢复期间用于建立一个恢复可从之向
 *
 * 前推进而不必重放整个恢复日志的点。
 * 若建立了新重启点则返回 true。
 * 仅当我们自上次重启点以来已重放过安全的检查点记录时才能建立重启点。
 */
bool
CreateRestartPoint(int flags)
{
	XLogRecPtr	lastCheckPointRecPtr;
	XLogRecPtr	lastCheckPointEndPtr;
	CheckPoint	lastCheckPoint;
	XLogRecPtr	PriorRedoPtr;
	XLogRecPtr	receivePtr;
	XLogRecPtr	replayPtr;
	TimeLineID	replayTLI;
	XLogRecPtr	endptr;
	XLogSegNo	_logSegNo;
	TimestampTz xtime;
	XLogRecPtr	slotsMinReqLSN;

	/* 并发检查点/重启点不可能发生 */
	Assert(!IsUnderPostmaster || MyBackendType == B_CHECKPOINTER);

	/* 并发检查点/重启点不可能发生 */
	SpinLockAcquire(&XLogCtl->info_lck);
	lastCheckPointRecPtr = XLogCtl->lastCheckPointRecPtr;
	lastCheckPointEndPtr = XLogCtl->lastCheckPointEndPtr;
	lastCheckPoint = XLogCtl->lastCheckPoint;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * 更新重
	 * 放位置。
	 */
	if (!RecoveryInProgress())
	{
		ereport(DEBUG2,
				(errmsg_internal("skipping restartpoint, recovery has already ended")));
		return false;
	}

	/*
	 * 应用
	 * 记录。
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 */
	if (XLogRecPtrIsInvalid(lastCheckPointRecPtr) ||
		lastCheckPoint.redo <= ControlFile->checkPointCopy.redo)
	{
		ereport(DEBUG2,
				(errmsg_internal("skipping restartpoint, already performed at %X/%X",
								 LSN_FORMAT_ARGS(lastCheckPoint.redo))));

		UpdateMinRecoveryPoint(InvalidXLogRecPtr, true);
		if (flags & CHECKPOINT_IS_SHUTDOWN)
		{
			LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
			ControlFile->state = DB_SHUTDOWNED_IN_RECOVERY;
			UpdateControlFile();
			LWLockRelease(ControlFileLock);
		}
		return false;
	}

	/*
	 * 更新共享的 RedoRecPtr，
	 * 以便启动进程能计算自上次重启点以来重放的段数，
	 * 并在超过 CheckPointSegments 时请求重启点。
	 *
	 * 与 CreateCheckPoint() 中一样，
	 * 更新时暂停插入；
	 * 不过在恢复期间这只是形式上的，因为没有 WAL 插入在发生。
	 */
	WALInsertLockAcquireExclusive();
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = lastCheckPoint.redo;
	WALInsertLockRelease();

	/* 同时更新受 info_lck 保护的副本 */
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->RedoRecPtr = lastCheckPoint.redo;
	SpinLockRelease(&XLogCtl->info_lck);

	/*
	 * 准备累计统计信息。
	 *
	 * 注意：
	 * 由于检查点进行期间 log_checkpoints 可能改变，
	 * 我们总是累计统计，即使当前 log_checkpoints 为关。
	 */
	MemSet(&CheckpointStats, 0, sizeof(CheckpointStats));
	CheckpointStats.ckpt_start_t = GetCurrentTimestamp();

	/*
	 * 获取当前最小 LSN，
	 * 供稍后 WAL 段清理使用。
	 * 我们只能清理根据复制槽的同步 LSN 不再需要的 WAL 段。
	 * 槽的 LSN 可能并发推进，
	 * 因此我们在 CheckPointReplicationSlots() 同步复制槽之前调用此函数。
	 */
	slotsMinReqLSN = XLogGetReplicationSlotMinimumLSN();

	if (log_checkpoints)
		LogCheckpointStart(flags, true);

	/* 更新进程标题 */
	update_checkpoint_display(flags, true, false);

	CheckPointGuts(lastCheckPoint.redo, flags);

	/*
	 * 记住先前检查点的 redo ptr，
	 * 供 UpdateCheckPointDistanceEstimate() 使用。
	 */
	PriorRedoPtr = ControlFile->checkPointCopy.redo;

	/*
	 * 使用当前时间更新 pg_control。
	 * 检查它是否仍显示较旧的检查点，否则什么也不做；
	 * 这是一个快速hack，
	 * 确保若我们在恢复结束检查点之后不知何故到达这里，不会发生真正糟糕的事。
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	if (ControlFile->checkPointCopy.redo < lastCheckPoint.redo)
	{
		/*
		 * 更新检查点信息。
		 * 即使数据库集簇未显示 DB_IN_ARCHIVE_RECOVERY 我们也这样做，
		 * 以与下面回收的 WAL 段集合匹配。
		 */
		ControlFile->checkPoint = lastCheckPointRecPtr;
		ControlFile->checkPointCopy = lastCheckPoint;

		/*
		 * 确保 minRecoveryPoint 超过检查点记录，
		 * 并在控制文件仍显示 DB_IN_ARCHIVE_RECOVERY
		 * 时更新它。
		 * 通常这在写出脏缓冲区时已经发生，
		 * 但不一定——例如因为没有缓冲区变脏。
		 * 我们这样做是因为恢复中执行的备份用 minRecoveryPoint
		 * 决定哪些 WAL 文件必须包含在备份中，
		 * 而至少必须包含含有检查点记录的文件。
		 * 注意对于恢复的普通重启，
		 * 使最小恢复点比这更早反正没有价值，
		 * 因为 redo 将刚好在检查点记录之后开始。
		 */
		if (ControlFile->state == DB_IN_ARCHIVE_RECOVERY)
		{
			if (ControlFile->minRecoveryPoint < lastCheckPointEndPtr)
			{
				ControlFile->minRecoveryPoint = lastCheckPointEndPtr;
				ControlFile->minRecoveryPointTLI = lastCheckPoint.ThisTimeLineID;

				/* 更新本地副本 */
				LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
				LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
			}
			if (flags & CHECKPOINT_IS_SHUTDOWN)
				ControlFile->state = DB_SHUTDOWNED_IN_RECOVERY;
		}
		UpdateControlFile();
	}
	LWLockRelease(ControlFileLock);

	/*
	 * 更新共
	 * 享状态。
	 */
	if (PriorRedoPtr != InvalidXLogRecPtr)
		UpdateCheckPointDistanceEstimate(RedoRecPtr - PriorRedoPtr);

	/*
	 * 更新本
	 * 地副本
	 */
	XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);

	/*
	 * 使用当前已重放或已接收的 xlog
	 * 末尾（取较晚者）回退 _logSegNo。
	 */
	receivePtr = GetWalRcvFlushRecPtr(NULL, NULL);
	replayPtr = GetXLogReplayRecPtr(&replayTLI);
	endptr = (receivePtr < replayPtr) ? replayPtr : receivePtr;
	KeepLogSeg(endptr, slotsMinReqLSN, &_logSegNo);
	if (InvalidateObsoleteReplicationSlots(RS_INVAL_WAL_REMOVED,
										   _logSegNo, InvalidOid,
										   InvalidTransactionId))
	{
		/*
		 * 重新计算用于 WAL 段清理的当前最小 LSN。
		 * 然后必须再次同步复制槽，
		 * 才能安全使用该 LSN。
		 */
		slotsMinReqLSN = XLogGetReplicationSlotMinimumLSN();
		CheckPointReplicationSlots();

		/*
		 * 某些槽已失效；
		 * 从 RedoRecPtr 重新开始重新计算旧段边界。
		 */
		XLByteToSeg(RedoRecPtr, _logSegNo, wal_segment_size);
		KeepLogSeg(endptr, slotsMinReqLSN, &_logSegNo);
	}
	_logSegNo--;

	/*
	 * 尝试在有用的时间线上回收段。
	 * 若自本重启点开始以来我们已被提升，
	 * 则使用恢复结束时选定的新时间线。
	 * 若仍在恢复中，
	 *
	 * 则使用我们正在重放的时间线。无法保证这些 WAL 段在当前时间线上一定有用；
	 * 若恢复在此之后马上进入新时间线，
	 * 本时间线上预分配的 WAL 段将不会被使用，
	 * 直到下一次重启点才会被回收，
	 * 相当于浪费。我们接受这一点。
	 */
	if (!RecoveryInProgress())
		replayTLI = XLogCtl->InsertTimeLineID;

	RemoveOldXlogFiles(_logSegNo, RedoRecPtr, endptr, replayTLI);

	/*
	 * 若需要则创
	 * 建更多段。
	 */
	PreallocXlogFiles(endptr, replayTLI);

	/*
	 * 若可能则截断 pg_subtrans。
	 * 我们可以丢弃任何运行中事务的最旧 XMIN 之前的所有数据。
	 * 未来事务不会试图引用比那更旧的任何 pg_subtrans 条目（见 subtrans.c 中的断言）。
	 * 不过在禁用热备时我们绝不能这样做，
	 * 因为尚未调用 StartupSUBTRANS。
	 */
	if (EnableHotStandby)
		TruncateSUBTRANS(GetOldestTransactionIdConsideredRunning());

	/* 实际工作已完成；记录日志并更新统计。 */
	LogCheckpointEnd(true);

	/* 实际工作已完成；记录日志并更新统计。 */
	update_checkpoint_display(flags, true, true);

	xtime = GetLatestXTime();
	ereport((log_checkpoints ? LOG : DEBUG2),
			(errmsg("recovery restart point at %X/%X",
					LSN_FORMAT_ARGS(lastCheckPoint.redo)),
			 xtime ? errdetail("Last completed transaction was at log time %s.",
							   timestamptz_to_str(xtime)) : 0));

	/*
	 * 执行 archive_cleanup_command。
	 */
	if (archiveCleanupCommand && strcmp(archiveCleanupCommand, "") != 0)
		ExecuteRecoveryCommand(archiveCleanupCommand,
							   "archive_cleanup_command",
							   false,
							   WAIT_EVENT_ARCHIVE_CLEANUP_COMMAND);

	return true;
}

/*
 * 报告给定目标 LSN（通常是槽的 restart_lsn）的 WAL 可用性。
 *		返回以下枚举值之一：
 *
 * WALAVAIL_RESERVED 表示 targetLSN
 *
 * 可用且在 max_wal_size 范围内；
 *   WALAVAIL_EXTENDED 表示通过在 max_wal_size
 *
 * 之外保留额外段仍可用。
 *   若 max_slot_wal_keep_size 小于 max_wal_size，
 *   则不返回此状态；
 *
 * WALAVAIL_UNRESERVED 表示正在丢失，
 *   下一次检查点将移除保留段。
 *   使用此槽的 walsender 可能回到上述状态；
 *
 * WALAVAIL_REMOVED 表示已被移除。
 *   具有此 LSN 的槽上的复制流无法继续。
 *   （任何关联的 walsender 进程应已终止。
 *
 * ）WALAVAIL_INVALID_LSN 表示槽尚未设置为保留 WAL。
 */
WALAvailability
GetWALAvailability(XLogRecPtr targetLSN)
{
	XLogRecPtr	currpos;		/* 当前写 LSN */
	XLogSegNo	currSeg;		/* currpos 的段号 */
	XLogSegNo	targetSeg;		/* targetLSN 的段号 */
	XLogSegNo	oldestSeg;		/* 实际最旧段号 */
	XLogSegNo	oldestSegMaxWalSize;	/* max_wal_size 保留的最旧段号 */
	XLogSegNo	oldestSlotSeg;	/* 槽保留的最旧段号 */
	uint64		keepSegs;
	XLogRecPtr	slotsMinReqLSN;

	/*
	 * 槽未保留 WAL。已停用，或从未活动。
	 */
	if (XLogRecPtrIsInvalid(targetLSN))
		return WALAVAIL_INVALID_LSN;

	/*
	 * 计算所有槽当前保留的最旧段，
	 * 并考虑 wal_keep_size 与 max_slot_wal_keep_size。
	 * 将 oldestSlotSeg 初始化为当前段。
	 */
	currpos = GetXLogWriteRecPtr();
	slotsMinReqLSN = XLogGetReplicationSlotMinimumLSN();
	XLByteToSeg(currpos, oldestSlotSeg, wal_segment_size);
	KeepLogSeg(currpos, slotsMinReqLSN, &oldestSlotSeg);

	/*
	 * 找到现存最旧段文件。
	 * 在检查点移除第一个 WAL
	 * 段之前我们得到 1。
	 */
	oldestSeg = XLogGetLastRemovedSegno() + 1;

	/* 按 max_wal_size 计算最旧段 */
	XLByteToSeg(currpos, currSeg, wal_segment_size);
	keepSegs = ConvertToXSegs(max_wal_size_mb, wal_segment_size) + 1;

	if (currSeg > keepSegs)
		oldestSegMaxWalSize = currSeg - keepSegs;
	else
		oldestSegMaxWalSize = 1;

	/* 我们所关心的段 */
	XLByteToSeg(targetLSN, targetSeg, wal_segment_size);

	/*
	 * 若已知 targetSeg 已被移除，
	 * 则返回保留或扩展状态值没有意义。
	 */
	if (targetSeg >= oldestSlotSeg)
	{
		/* 当 targetSeg 在 max_wal_size 内时显示 "reserved" */
		if (targetSeg >= oldestSegMaxWalSize)
			return WALAVAIL_RESERVED;

		/* 因槽而保留且已超过 max_wal_size */
		return WALAVAIL_EXTENDED;
	}

	/* WAL 段已不再保留但尚未被移除 */
	if (targetSeg >= oldestSeg)
		return WALAVAIL_UNRESERVED;

	/* 确定已丢失 */
	return WALAVAIL_REMOVED;
}


/*
 * 将 *logSegNo 回退到因 wal_keep_size
 * 或复制槽而需要保留的最后一段。
 *
 * 这通过从给定 xlog 位置 recptr 减去 wal_keep_size，
 * 并确保结果低于复制槽的要求来计算。
 * 对于后者，
 * 我们考虑 max_slot_wal_keep_size 的影响：
 * 从 recptr 最多只保留那么多空间。
 *
 * 关于复制槽的说明：
 * 若本函数计算出比槽需要保留的更靠前的值，
 * 则受影响的槽需要失效并再次调用本函数。
 * XXX 重写本函数使失效可选地在这里完成也许是个好主意，
 * 而不是……
 */
static void
KeepLogSeg(XLogRecPtr recptr, XLogRecPtr slotsMinReqLSN, XLogSegNo *logSegNo)
{
	XLogSegNo	currSegNo;
	XLogSegNo	segno;
	XLogRecPtr	keep;

	XLByteToSeg(recptr, currSegNo, wal_segment_size);
	segno = currSegNo;

	/*
	 * 首先计算槽保留的段数，
	 * 并按 max_slot_wal_keep_size 调整。
	 */
	keep = slotsMinReqLSN;
	if (keep != InvalidXLogRecPtr && keep < recptr)
	{
		XLByteToSeg(keep, segno, wal_segment_size);

		/* 由 max_slot_wal_keep_size 限制... */
		if (max_slot_wal_keep_size_mb >= 0)
		{
			uint64		slot_keep_segs;

			slot_keep_segs =
				ConvertToXSegs(max_slot_wal_keep_size_mb, wal_segment_size);

			if (currSegNo - segno > slot_keep_segs)
				segno = currSegNo - slot_keep_segs;
		}
	}

	/* 但至少保留 wal_keep_size（若已设置） */
	if (wal_keep_size_mb > 0)
	{
		uint64		keep_segs;

		keep_segs = ConvertToXSegs(wal_keep_size_mb, wal_segment_size);
		if (currSegNo - segno < keep_segs)
		{
			/* 避免下溢，不要低于 1 */
			if (currSegNo <= keep_segs)
				segno = 1;
			else
				segno = currSegNo - keep_segs;
		}
	}

	/* 不要删除比计算出的段更新的 WAL 段 */
	if (segno < *logSegNo)
		*logSegNo = segno;
}

/*
 * 写一条 NEXTOID 日志记录
 */
void
XLogPutNextOid(Oid nextOid)
{
	XLogBeginInsert();
	XLogRegisterData((char *) (&nextOid), sizeof(Oid));
	(void) XLogInsert(RM_XLOG_ID, XLOG_NEXTOID);

	/*
	 * 我们不必立即刷盘 NEXTOID 记录，
	 * 因为刚分配的 OID 只有作为元组插入或更新的一部分才可能到达磁盘，
	 * 而那会有自己的、必须跟在 NEXTOID 记录之后的 XLOG 记录。
	 * 因此应用于那些记录的标准缓冲区 LSN 互锁将确保任何此类 OID 不会在 NEXTOID 记录之前到达磁盘。
	 * 但请注意，
	 * 上述陈述仅覆盖数据库 "within" 的状态。
	 *
	 * 当我们把生成的 OID 用作文件或目录名时，
	 * 某种意义上违反了基本 WAL 规则，
	 * 因为该文件系统更改可能在 NEXTOID WAL 记录之前到达磁盘。
	 * 其影响是，
	 * 若紧接着发生数据库崩溃，
	 * 我们可能在重启后重新生成相同 OID，
	 * 并发现它与残留文件或目录冲突。
	 * 但为安全起见我们总是循环直到找到不冲突的文件名，
	 * 这在实践中不构成真正问题。见 pgsql-hackers 2006-09-27 的讨论。
	 */
}

/*
 * 写一条 XLOG SWITCH 记录。
 *
 * 这里我们只是盲目地为该记录发出
 * XLogInsert 请求，
 *
 * 实际工作由 
 * XLogInsertRecord
 * 完成。
 */
XLogRecPtr
RequestXLogSwitch(bool mark_unimportant)
{
	XLogRecPtr	RecPtr;

	/* XLOG SWITCH has no data */
	XLogBeginInsert();

	if (mark_unimportant)
		XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
	RecPtr = XLogInsert(RM_XLOG_ID, XLOG_SWITCH);

	return RecPtr;
}

/*
 * 写一条 RESTORE POINT 记录
 */
XLogRecPtr
XLogRestorePoint(const char *rpName)
{
	XLogRecPtr	RecPtr;
	xl_restore_point xlrec;

	xlrec.rp_time = GetCurrentTimestamp();
	strlcpy(xlrec.rp_name, rpName, MAXFNAMELEN);

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_restore_point));

	RecPtr = XLogInsert(RM_XLOG_ID, XLOG_RESTORE_POINT);

	ereport(LOG,
			(errmsg("restore point \"%s\" created at %X/%X",
					rpName, LSN_FORMAT_ARGS(RecPtr))));

	return RecPtr;
}

/*
 * 检查对热备关键的任何 GUC 参数是否已更改，
 * 并更新 pg_control 中的值。
 */
static void
XLogReportParameters(void)
{
	if (wal_level != ControlFile->wal_level ||
		wal_log_hints != ControlFile->wal_log_hints ||
		MaxConnections != ControlFile->MaxConnections ||
		max_worker_processes != ControlFile->max_worker_processes ||
		max_wal_senders != ControlFile->max_wal_senders ||
		max_prepared_xacts != ControlFile->max_prepared_xacts ||
		max_locks_per_xact != ControlFile->max_locks_per_xact ||
		track_commit_timestamp != ControlFile->track_commit_timestamp)
	{
		/*
		 * 若未启用归档，
		 * 后端槽数量的更改不必记入 WAL，
		 * 因为反正无法以 wal_level=minimal 启动归档恢复。若 wal_level=minimal，
		 * 我们其实也不太关心 pg_control 中的值，
		 * 但保持它们最新似乎更好，以免混淆。
		 */
		if (wal_level != ControlFile->wal_level || XLogIsNeeded())
		{
			xl_parameter_change xlrec;
			XLogRecPtr	recptr;

			xlrec.MaxConnections = MaxConnections;
			xlrec.max_worker_processes = max_worker_processes;
			xlrec.max_wal_senders = max_wal_senders;
			xlrec.max_prepared_xacts = max_prepared_xacts;
			xlrec.max_locks_per_xact = max_locks_per_xact;
			xlrec.wal_level = wal_level;
			xlrec.wal_log_hints = wal_log_hints;
			xlrec.track_commit_timestamp = track_commit_timestamp;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xlrec));

			recptr = XLogInsert(RM_XLOG_ID, XLOG_PARAMETER_CHANGE);
			XLogFlush(recptr);
		}

		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

		ControlFile->MaxConnections = MaxConnections;
		ControlFile->max_worker_processes = max_worker_processes;
		ControlFile->max_wal_senders = max_wal_senders;
		ControlFile->max_prepared_xacts = max_prepared_xacts;
		ControlFile->max_locks_per_xact = max_locks_per_xact;
		ControlFile->wal_level = wal_level;
		ControlFile->wal_log_hints = wal_log_hints;
		ControlFile->track_commit_timestamp = track_commit_timestamp;
		UpdateControlFile();

		LWLockRelease(ControlFileLock);
	}
}

/*
 * 更新共享内存中的 
 * full_page_writes，
 *
 * 并在必要时写入 XLOG_FPW_CHANGE
 * 记录。
 */
void
UpdateFullPageWrites(void)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	bool		recoveryInProgress;

	/*
	 * 若 full_page_writes 未更改则什么也不做。
	 *
	 * 检查共享 fullPageWrites 是安全的，
	 * 因为若它刚被更改，
	 * 我们会在下面再检查一次。
	 */
	if (fullPageWrites == Insert->fullPageWrites)
		return;

	/*
	 * 在临界区外执行，
	 * 以便 RecoveryInProgress()
	 * 所做的 WAL 插入初始化不会变成临界操作。
	 */
	recoveryInProgress = RecoveryInProgress();

	START_CRIT_SECTION();

	/*
	 * 即使并非严格需要，
	 * 拍全页映像总是安全的；
	 * 反过来则不然。因此若要把 full_page_writes 设为 true，
	 * 先设为 true 再写 WAL 记录。
	 * 若要设为 false，先写 WAL 记录再设置全局标志。
	 */
	if (fullPageWrites)
	{
		WALInsertLockAcquireExclusive();
		Insert->fullPageWrites = true;
		WALInsertLockRelease();
	}

	/*
	 * 写一条 XLOG_FPW_CHANGE 记录。
	 * 这让我们能在恢复期间跟踪 full_page_writes。
	 */
	if (XLogStandbyInfoActive() && !recoveryInProgress)
	{
		XLogBeginInsert();
		XLogRegisterData((char *) (&fullPageWrites), sizeof(bool));

		XLogInsert(RM_XLOG_ID, XLOG_FPW_CHANGE);
	}

	if (!fullPageWrites)
	{
		WALInsertLockAcquireExclusive();
		Insert->fullPageWrites = false;
		WALInsertLockRelease();
	}
	END_CRIT_SECTION();
}

/*
 * XLOG 资源管理器的例程。
 *
 * info 值的定义见 include/catalog/pg_control.h，
 * 尽管并非所有记录类型都与控制文件更新相关。
 *
 * 注意：
 * 一些与 WAL 恢复直接相关的 XLOG 记录类型在 xlogrecovery_redo() 中处理。
 */
void
xlog_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	XLogRecPtr	lsn = record->EndRecPtr;

	/*
	 * 在 XLOG rmgr 中，
	 * 备份块仅由 XLOG_FPI 和 XLOG_FPI_FOR_HINT 记录使用。
	 */
	Assert(info == XLOG_FPI || info == XLOG_FPI_FOR_HINT ||
		   !XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_NEXTOID)
	{
		Oid			nextOid;

		/*
		 * 我们曾试图取 ShmemVariableCache->nextOid 与记录的 nextOid 的最大值，
		 * 但若 OID 计数器回绕就会失败。
		 * 既然重放期间反正不应发生 OID 分配，
		 * 最好完全相信该记录。
		 * 我们仍在设置变量时持有 OidGenLock，以防万一。
		 */
		memcpy(&nextOid, XLogRecGetData(record), sizeof(Oid));
		LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
		ShmemVariableCache->nextOid = nextOid;
		ShmemVariableCache->oidCount = 0;
		LWLockRelease(OidGenLock);
	}
	else if (info == XLOG_CHECKPOINT_SHUTDOWN)
	{
		CheckPoint	checkPoint;
		TimeLineID	replayTLI;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* 在 SHUTDOWN 检查点中，精确相信计数器 */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		ShmemVariableCache->nextXid = checkPoint.nextXid;
		LWLockRelease(XidGenLock);
		LWLockAcquire(OidGenLock, LW_EXCLUSIVE);
		ShmemVariableCache->nextOid = checkPoint.nextOid;
		ShmemVariableCache->oidCount = 0;
		LWLockRelease(OidGenLock);
		MultiXactSetNextMXact(checkPoint.nextMulti,
							  checkPoint.nextMultiOffset);

		MultiXactAdvanceOldest(checkPoint.oldestMulti,
							   checkPoint.oldestMultiDB);

		/*
		 * 这里也不必设置 oldestClogXid；
		 * 当我们重放 xl_clog_truncate 时会设置。
		 */
		SetTransactionIdLimit(checkPoint.oldestXid, checkPoint.oldestXidDB);

		/*
		 * 若在等待 end-of-backup
		 * 记录时看到关闭检查点，
		 * 则备份无效。
		 */
		if (ArchiveRecoveryRequested &&
			!XLogRecPtrIsInvalid(ControlFile->backupStartPoint) &&
			XLogRecPtrIsInvalid(ControlFile->backupEndPoint))
			ereport(PANIC,
					(errmsg("online backup was canceled, recovery cannot continue")));

		/*
		 * 若看到关
		 * 闭检查点，
		 * 则知道此时主库上
		 * 没有事务在运行。
		 */
		if (standbyState >= STANDBY_INITIALIZED)
		{
			TransactionId *xids;
			int			nxids;
			TransactionId oldestActiveXID;
			TransactionId latestCompletedXid;
			RunningTransactionsData running;

			oldestActiveXID = PrescanPreparedTransactions(&xids, &nxids);

			/* 更新任何准备好的事务的 pg_subtrans 条目 */
			StandbyRecoverPreparedTransactions();

			/*
			 * 构造表示已关闭服务器的 
			 * RunningTransactions
			 * 快照，
			 * 其中仅已准备事务仍存活。
			 */
			running.xcnt = nxids;
			running.subxcnt = 0;
			running.subxid_status = SUBXIDS_IN_SUBTRANS;
			running.nextXid = XidFromFullTransactionId(checkPoint.nextXid);
			running.oldestRunningXid = oldestActiveXID;
			latestCompletedXid = XidFromFullTransactionId(checkPoint.nextXid);
			TransactionIdRetreat(latestCompletedXid);
			Assert(TransactionIdIsNormal(latestCompletedXid));
			running.latestCompletedXid = latestCompletedXid;
			running.xids = xids;

			ProcArrayApplyRecoveryInfo(&running);
		}

		/* ControlFile->checkPointCopy 始终跟踪最新的检查点 XID */
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->checkPointCopy.nextXid = checkPoint.nextXid;
		LWLockRelease(ControlFileLock);

		/* 更新共享内存中的检查点 XID/epoch 副本 */
		SpinLockAcquire(&XLogCtl->info_lck);
		XLogCtl->ckptFullXid = checkPoint.nextXid;
		SpinLockRelease(&XLogCtl->info_lck);

		/*
		 * 在重放此记录之前，
		 * 我们应已切换到新 TLI。
		 */
		(void) GetCurrentReplayRecPtr(&replayTLI);
		if (checkPoint.ThisTimeLineID != replayTLI)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in shutdown checkpoint record",
							checkPoint.ThisTimeLineID, replayTLI)));

		RecoveryRestartPoint(&checkPoint, record);
	}
	else if (info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint	checkPoint;
		TimeLineID	replayTLI;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* 在 ONLINE 检查点中，将 XID 计数器视为最小值 */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		if (FullTransactionIdPrecedes(ShmemVariableCache->nextXid,
									  checkPoint.nextXid))
			ShmemVariableCache->nextXid = checkPoint.nextXid;
		LWLockRelease(XidGenLock);

		/*
		 * 我们忽略 ONLINE 检查点中的 nextOid 计数器，
		 * 宁愿通过 XLOG_NEXTOID 记录跟踪 OID 分配。
		 * nextOid 计数器来自检查点开始时，
		 * 与后来的 XLOG_NEXTOID 记录相比可能已经陈旧。
		 * 我们可以试图取 nextOid 计数器与我们最新值的最大值，
		 * 但由于 OID 计数器回绕速度没有特别保证，
		 * 那样做有风险。
		 * 无论如何，
		 * nextOid 计数器的使用者必须避免分配重复值，因此稍微过时的值应当是安全的。
		 */

		/* 处理 MultiXact */
		MultiXactAdvanceNextMXact(checkPoint.nextMulti,
								  checkPoint.nextMultiOffset);

		/*
		 * 注意：
		 * 在重放由较旧主库生成的 WAL 时，这可能执行 multixact 截断。
		 */
		MultiXactAdvanceOldest(checkPoint.oldestMulti,
							   checkPoint.oldestMultiDB);
		if (TransactionIdPrecedes(ShmemVariableCache->oldestXid,
								  checkPoint.oldestXid))
			SetTransactionIdLimit(checkPoint.oldestXid,
								  checkPoint.oldestXidDB);
		/* ControlFile->checkPointCopy 始终跟踪最新的检查点 XID */
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->checkPointCopy.nextXid = checkPoint.nextXid;
		LWLockRelease(ControlFileLock);

		/* 更新共享内存中的检查点 XID/epoch 副本 */
		SpinLockAcquire(&XLogCtl->info_lck);
		XLogCtl->ckptFullXid = checkPoint.nextXid;
		SpinLockRelease(&XLogCtl->info_lck);

		/* 在线检查点中 TLI 不应改变 */
		(void) GetCurrentReplayRecPtr(&replayTLI);
		if (checkPoint.ThisTimeLineID != replayTLI)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in online checkpoint record",
							checkPoint.ThisTimeLineID, replayTLI)));

		RecoveryRestartPoint(&checkPoint, record);
	}
	else if (info == XLOG_OVERWRITE_CONTRECORD)
	{
		/* 此处无事可做，在 xlogrecovery_redo() 中处理 */
	}
	else if (info == XLOG_END_OF_RECOVERY)
	{
		xl_end_of_recovery xlrec;
		TimeLineID	replayTLI;

		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_end_of_recovery));

		/*
		 * 对热备，
		 * 我们可以把这当作关闭检查点处理，
		 * 但此情况较少见，不值得特别对待。
		 */

		/*
		 * 在重放此记录之前，
		 * 我们应已切换到新 TLI。
		 */
		(void) GetCurrentReplayRecPtr(&replayTLI);
		if (xlrec.ThisTimeLineID != replayTLI)
			ereport(PANIC,
					(errmsg("unexpected timeline ID %u (should be %u) in end-of-recovery record",
							xlrec.ThisTimeLineID, replayTLI)));
	}
	else if (info == XLOG_NOOP)
	{
		/* 此处无事可做 */
	}
	else if (info == XLOG_SWITCH)
	{
		/* 此处无事可做 */
	}
	else if (info == XLOG_RESTORE_POINT)
	{
		/* 此处无事可做，在 xlogrecovery.c 中处理 */
	}
	else if (info == XLOG_FPI || info == XLOG_FPI_FOR_HINT)
	{
		/*
		 * XLOG_FPI 记录除一个或多个块引用外不含其他内容。
		 * 即使生成记录时禁用了 full_page_writes，
		 * 每个块引用也必须包含全页映像——否则此记录就没有意义。
		 * 当因提示位更新而需要把页记入 WAL 时生成
		 *
		 * XLOG_FPI_FOR_HINT 记录。
		 * 仅在启用校验和和/或 wal_log_hints
		 * 时生成。
		 * 若生成时禁用了 full_page_writes，
		 * 它们可能不包含全页映像。
		 *
		 * 此时这里无事可做。
		 * 这些通用记录不产生恢复冲突——若资源管理器需要产生冲突，
		 * 它必须定义单独的 WAL 记录类型和 redo 例程。
		 */
		for (uint8 block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
		{
			Buffer		buffer;

			if (!XLogRecHasBlockImage(record, block_id))
			{
				if (info == XLOG_FPI)
					elog(ERROR, "XLOG_FPI record did not contain a full-page image");
				continue;
			}

			if (XLogReadBufferForRedo(record, block_id, &buffer) != BLK_RESTORED)
				elog(ERROR, "unexpected XLogReadBufferForRedo result when restoring backup block");
			UnlockReleaseBuffer(buffer);
		}
	}
	else if (info == XLOG_BACKUP_END)
	{
		/* 此处无事可做，在 xlogrecovery_redo() 中处理 */
	}
	else if (info == XLOG_PARAMETER_CHANGE)
	{
		xl_parameter_change xlrec;

		/* 更新我们在 pg_control 中的参数副本 */
		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_parameter_change));

		/*
		 * 若我们处于热备且主库没有足以进行逻辑解码的 WAL 级别，
		 * 则使逻辑槽失效。
		 * 若备库以低于 logical 的 wal_level 运行，
		 * 则不必搜索潜在冲突的逻辑槽，
		 * 因为在那种情况下，
		 * 我们要么已禁止创建逻辑槽，要么已使现有槽失效。
		 */
		if (InRecovery && InHotStandby &&
			xlrec.wal_level < WAL_LEVEL_LOGICAL &&
			wal_level >= WAL_LEVEL_LOGICAL)
			InvalidateObsoleteReplicationSlots(RS_INVAL_WAL_LEVEL,
											   0, InvalidOid,
											   InvalidTransactionId);

		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
		ControlFile->MaxConnections = xlrec.MaxConnections;
		ControlFile->max_worker_processes = xlrec.max_worker_processes;
		ControlFile->max_wal_senders = xlrec.max_wal_senders;
		ControlFile->max_prepared_xacts = xlrec.max_prepared_xacts;
		ControlFile->max_locks_per_xact = xlrec.max_locks_per_xact;
		ControlFile->wal_level = xlrec.wal_level;
		ControlFile->wal_log_hints = xlrec.wal_log_hints;

		/*
		 * 更新 minRecoveryPoint，
		 * 以确保若恢复中止，
		 * 我们会恢复到此点之后才再次允许热备。若减小了 max_* 设置，
		 * 这一点很重要，
		 * 以确保你不会对更改之前的 WAL 运行查询。
		 * 只要崩溃恢复仍在进行且我们期望重放全部 WAL，就不能更新本地副本。
		 */
		if (InArchiveRecovery)
		{
			LocalMinRecoveryPoint = ControlFile->minRecoveryPoint;
			LocalMinRecoveryPointTLI = ControlFile->minRecoveryPointTLI;
		}
		if (LocalMinRecoveryPoint != InvalidXLogRecPtr && LocalMinRecoveryPoint < lsn)
		{
			TimeLineID	replayTLI;

			(void) GetCurrentReplayRecPtr(&replayTLI);
			ControlFile->minRecoveryPoint = lsn;
			ControlFile->minRecoveryPointTLI = replayTLI;
		}

		CommitTsParameterChange(xlrec.track_commit_timestamp,
								ControlFile->track_commit_timestamp);
		ControlFile->track_commit_timestamp = xlrec.track_commit_timestamp;

		UpdateControlFile();
		LWLockRelease(ControlFileLock);

		/* 检查是否有任何参数更改会在恢复时造成问题 */
		CheckRequiredParameterValues();
	}
	else if (info == XLOG_FPW_CHANGE)
	{
		bool		fpw;

		memcpy(&fpw, XLogRecGetData(record), sizeof(bool));

		/*
		 * 更新最后重放的 XLOG_FPW_CHANGE 记录的 LSN，
		 * 以便 do_pg_backup_start() 与 do_pg_backup_stop()
		 * 能检查在线备份期间是否禁用了 full_page_writes。
		 */
		if (!fpw)
		{
			SpinLockAcquire(&XLogCtl->info_lck);
			if (XLogCtl->lastFpwDisableRecPtr < record->ReadRecPtr)
				XLogCtl->lastFpwDisableRecPtr = record->ReadRecPtr;
			SpinLockRelease(&XLogCtl->info_lck);
		}

		/* 跟踪 full_page_writes */
		lastFullPageWrites = fpw;
	}
}

/*
 * 根据 GUCs wal_sync_method、fsync 与 io_direct 的值，
 * 返回打开文件时使用的额外打开标志。
 */
static int
get_sync_bit(int method)
{
	int			o_direct_flag = 0;

	/*
	 * 若请求则使用 O_DIRECT，
	 * 但 walreceiver 进程除外。
	 * walreceiver 写入的 WAL
	 * 必须能被其他进程立即读取。
	 */
	if ((io_direct_flags & IO_DIRECT_WAL) && !AmWalReceiverProcess())
		o_direct_flag = PG_O_DIRECT;

	/* 若禁用了 fsync，则从不以 sync 模式打开 */
	if (!enableFsync)
		return o_direct_flag;

	switch (method)
	{
			/*
			 * 即使当前平
			 * 台不支持，
			 * 所有同步选项的
			 * 枚举值也已定义。
			 */
		case SYNC_METHOD_FSYNC:
		case SYNC_METHOD_FSYNC_WRITETHROUGH:
		case SYNC_METHOD_FDATASYNC:
			return o_direct_flag;
#ifdef O_SYNC
		case SYNC_METHOD_OPEN:
			return O_SYNC | o_direct_flag;
#endif
#ifdef O_DSYNC
		case SYNC_METHOD_OPEN_DSYNC:
			return O_DSYNC | o_direct_flag;
#endif
		default:
			/* 如果 fsync 被禁用，永远不要以同步模式打开 */
			elog(ERROR, "unrecognized wal_sync_method: %d", method);
			return 0;			/* 消除警告 */
	}
}

/*
 * 为 XLOG 输出文件发出适当类型的 fsync（若有）。
 */
void
assign_xlog_sync_method(int new_sync_method, void *extra)
{
	if (sync_method != new_sync_method)
	{
		/*
		 * 为确保没有块未
		 * 经同步就逃逸，
		 * 对当前打开的日志段强制
		 * fsync。
		 */
		if (openLogFile >= 0)
		{
			pgstat_report_wait_start(WAIT_EVENT_WAL_SYNC_METHOD_ASSIGN);
			if (pg_fsync(openLogFile) != 0)
			{
				char		xlogfname[MAXFNAMELEN];
				int			save_errno;

				save_errno = errno;
				XLogFileName(xlogfname, openLogTLI, openLogSegNo,
							 wal_segment_size);
				errno = save_errno;
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not fsync file \"%s\": %m", xlogfname)));
			}

			pgstat_report_wait_end();
			if (get_sync_bit(sync_method) != get_sync_bit(new_sync_method))
				XLogFileClose();
		}
	}
}


/*
 * 为 XLOG 输出文件发出适当类型的
 *
 * fsync（若有）。
 * fd 是已打开日志文件的文件描述符。
 */
void
issue_xlog_fsync(int fd, XLogSegNo segno, TimeLineID tli)
{
	char	   *msg = NULL;
	instr_time	start;

	Assert(tli != 0);

	/*
	 * 若禁用了 fsync，
	 * 或 write() 已同步了 WAL 文件，则快速退出。
	 */
	if (!enableFsync ||
		sync_method == SYNC_METHOD_OPEN ||
		sync_method == SYNC_METHOD_OPEN_DSYNC)
		return;

	/* 测量同步 WAL 文件的 I/O 时间 */
	if (track_wal_io_timing)
		INSTR_TIME_SET_CURRENT(start);
	else
		INSTR_TIME_SET_ZERO(start);

	pgstat_report_wait_start(WAIT_EVENT_WAL_SYNC);
	switch (sync_method)
	{
		case SYNC_METHOD_FSYNC:
			if (pg_fsync_no_writethrough(fd) != 0)
				msg = _("could not fsync file \"%s\": %m");
			break;
#ifdef HAVE_FSYNC_WRITETHROUGH
		case SYNC_METHOD_FSYNC_WRITETHROUGH:
			if (pg_fsync_writethrough(fd) != 0)
				msg = _("could not fsync write-through file \"%s\": %m");
			break;
#endif
		case SYNC_METHOD_FDATASYNC:
			if (pg_fdatasync(fd) != 0)
				msg = _("could not fdatasync file \"%s\": %m");
			break;
		case SYNC_METHOD_OPEN:
		case SYNC_METHOD_OPEN_DSYNC:
			/* 不可达 */
			Assert(false);
			break;
		default:
			elog(PANIC, "unrecognized wal_sync_method: %d", sync_method);
			break;
	}

	/* fsync 失败则 PANIC */
	if (msg)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, tli, segno, wal_segment_size);
		errno = save_errno;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg(msg, xlogfname)));
	}

	pgstat_report_wait_end();

	/*
	 * 增加 I/O 计时以及 WAL 文件被同步的次数。
	 */
	if (track_wal_io_timing)
	{
		instr_time	duration;

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_ACCUM_DIFF(PendingWalStats.wal_sync_time, duration, start);
	}

	PendingWalStats.wal_sync++;
}

/*
 * do_pg_backup_start 是用户可见的 pg_backup_start()
 * 函数的工作核心。
 * 它创建必要的起始检查点，
 *
 * 并构造备份状态与表空间映射。
 * 输入参数是 state（备份状态）、fast（若为 true，
 * 我们以 immediate 模式做检查点以加快速度）以及
 * tablespaces（若非 NULL，
 *
 * 表示描述数据库集簇表空间的 tablespaceinfo 结构列表）。
 * 表空间映射内容追加到传入的 tablespace_map 参数，
 * 调用者负责把它作为 'tablespace_map' 包含在备份归档中。
 * tablespace_map 文件主要在 Windows 上的 tar 格式需要，
 * 因为原生 Windows 工具在从 tar 解压文件时无法创建符号链接。
 * 但为一致性和平台无关性，
 *
 * 我们到处都以同样方式做。
 * 它用备份所需信息填充 state，
 * 例如从此备份恢复所必须存在的最小 WAL 位置（starttli）以及对应的时间线
 *
 * ID（starttli）。
 * 每个成功开始的备份必须通过调用 do_pg_backup_stop()
 * 或 do_pg_abort_backup() 停止。
 *
 * 可以同时有许多活动备份。
 * 验证调用用户权限是本函数调用者的责任！
 */
void
do_pg_backup_start(const char *backupidstr, bool fast, List **tablespaces,
				   BackupState *state, StringInfo tblspcmapfile)
{
	bool		backup_started_in_recovery;

	Assert(state != NULL);
	backup_started_in_recovery = RecoveryInProgress();

	/*
	 * 在恢复期间，我们不必检查 WAL 级别。
	 * 因为若 WAL 级别不足，备库根本不会启动。
	 */
	if (!backup_started_in_recovery && !XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL level not sufficient for making an online backup"),
				 errhint("wal_level must be set to \"replica\" or \"logical\" at server start.")));

	if (strlen(backupidstr) > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("backup label too long (max %d bytes)",
						MAXPGPATH)));

	memcpy(state->name, backupidstr, strlen(backupidstr));

	/*
	 * 在共享内存中标记备份活动。
	 * 在线备份期间我们必须做全页 WAL 写入，
	 * 即使在其他时候不做，
	 * 因为若备份转储在我们写入同一页时并发读取该页，
	 * 完全可能获得数据库页的 "torn"（部分写入）副本。
	 * 只要 WAL 序列中对该页的第一次写入是全页写入，
	 * 就可以修复。
	 * 因此，
	 * 我们递增 runningBackups 然后强制一次 CHECKPOINT，
	 * 以确保共享内存中没有脏页可能在备份进行期间被转储却没有对应的 WAL 记录。
	 * （备份完成后，
	 * 我们就不必再强制全页写入，
	 *
	 * 因为我们期望备份间隔内未修改的任何页都已被备份正确捕获。
	 * ）注意：
	 *
	 * 从备库做在线备份时强制全页写入无效。
	 * 我们必须持有所有插入锁才能更改 runningBackups 的值，
	 * 以确保与 XLogInsertRecord() 有足够的互锁。
	 */
	WALInsertLockAcquireExclusive();
	XLogCtl->Insert.runningBackups++;
	WALInsertLockRelease();

	/*
	 * 确保若下面失败我们会递减 runningBackups。
	 * 注意——为此正确工作，
	 * 这必须在第一个可失败步骤之前发生。
	 */
	PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, DatumGetBool(true));
	{
		bool		gotUniqueStartpoint = false;
		DIR		   *tblspcdir;
		struct dirent *de;
		tablespaceinfo *ti;
		int			datadirpathlen;

		/*
		 * 在检查点之前强制切换 XLOG 文件，
		 * 以确保写入检查点的 WAL 段不包含带有旧时间线 ID 的页。
		 * 否则若你在从 PITR 归档恢复后立刻调用
		 * pg_backup_start()
		 * 就会发生这种情况：
		 * 包含启动检查点的第一个 WAL 段在开头有带旧时间线 ID 的页。
		 * 这可能在恢复时造成麻烦：
		 * 若基础备份未包含 pg_wal 目录且在开始备份前也清空了 WAL 归档，
		 * 我们就没有覆盖旧时间线的历史文件。
		 *
		 * 这也确保我们在发出检查点记录之前已发出带有 XLP_BKP_REMOVABLE
		 * 关闭的 WAL 页头。
		 * 因此，
		 * 若 WAL 归档器（如 pglesslog）试图压缩掉可移除的备份块，
		 * 它不会移除此点之后出现的任何块。
		 *
		 * 在恢复期间，
		 * 我们跳过强制 XLOG 文件切换，
		 * 这意味着恢复期间所做的备份不适用于上述特殊恢复情形。
		 */
		if (!backup_started_in_recovery)
			RequestXLogSwitch(false);

		do
		{
			bool		checkpointfpw;

			/*
			 * 强制一次 CHECKPOINT。
			 * 除防止撕裂页问题外，
			 * 这还能保证两次连续备份运行有不同的检查点位置，
			 * 从而有不同的历史文件名，
			 *
			 * 即使其间什么也没发生。
			 * 在恢复期间，
			 * 若可能则建立重启点。
			 * 我们使用上一次重启点作为备份起始检查点。
			 *
			 * 这意味着两次连续备份运行可能有相同的检查点位置。
			 * 既然我们在恢复期间执行 do_pg_backup_start() 意味着 checkpointer 正在运行，
			 * 我们可以用 RequestCheckpoint() 建立重启点。
			 *
			 * 仅当用户请求时（通过传入 fast = true）我们才使用 CHECKPOINT_IMMEDIATE。
			 * 否则这可能需要一段时间。
			 */
			RequestCheckpoint(CHECKPOINT_FORCE | CHECKPOINT_WAIT |
							  (fast ? CHECKPOINT_IMMEDIATE : 0));

			/*
			 * 现在需要获取检查点记录位置及其
			 * REDO 指针。
			 * 从该检查点开始恢复所需的最旧 WAL
			 * 点恰好是检查点的 REDO 指针。
			 */
			LWLockAcquire(ControlFileLock, LW_SHARED);
			state->checkpointloc = ControlFile->checkPoint;
			state->startpoint = ControlFile->checkPointCopy.redo;
			state->starttli = ControlFile->checkPointCopy.ThisTimeLineID;
			checkpointfpw = ControlFile->checkPointCopy.fullPageWrites;
			LWLockRelease(ControlFileLock);

			if (backup_started_in_recovery)
			{
				XLogRecPtr	recptr;

				/*
				 * 检查在线备份期间重放的所有
				 * WAL（
				 * 即自上次重启点以来）是否包含全页写入。
				 */
				SpinLockAcquire(&XLogCtl->info_lck);
				recptr = XLogCtl->lastFpwDisableRecPtr;
				SpinLockRelease(&XLogCtl->info_lck);

				if (!checkpointfpw || state->startpoint <= recptr)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("WAL generated with full_page_writes=off was replayed "
									"since last restartpoint"),
							 errhint("This means that the backup being taken on the standby "
									 "is corrupt and should not be used. "
									 "Enable full_page_writes and run CHECKPOINT on the primary, "
									 "and then try an online backup again.")));

				/*
				 * 在恢复期间，
				 * 由于我们不使用 end-of-backup
				 * WAL 记录也不写备份历史文件，
				 * 备份标签文件中的起点是重启点的
				 * REDO 位置。
				 */
				gotUniqueStartpoint = true;
			}

			/*
			 * 若两个基础备
			 * 份同时
			 * 启动（在
			 * WAL
			 * sender
			 * 进程中），
			 * 我们需
			 * 要确保使
			 * 用较旧的起点。
			 */
			WALInsertLockAcquireExclusive();
			if (XLogCtl->Insert.lastBackupStart < state->startpoint)
			{
				XLogCtl->Insert.lastBackupStart = state->startpoint;
				gotUniqueStartpoint = true;
			}
			WALInsertLockRelease();
		} while (!gotUniqueStartpoint);

		/*
		 * 构造 tablespace_map 文件。
		 */
		datadirpathlen = strlen(DataDir);

		/* 收集关于所有表空间的信息 */
		tblspcdir = AllocateDir("pg_tblspc");
		while ((de = ReadDir(tblspcdir, "pg_tblspc")) != NULL)
		{
			char		fullpath[MAXPGPATH + 10];
			char		linkpath[MAXPGPATH];
			char	   *relpath = NULL;
			char	   *s;
			PGFileType	de_type;

			/* 收集所有表空间的信息 */
			if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
				continue;

			snprintf(fullpath, sizeof(fullpath), "pg_tblspc/%s", de->d_name);

			de_type = get_dirent_type(fullpath, de, false, ERROR);

			if (de_type == PGFILETYPE_LNK)
			{
				StringInfoData escapedpath;
				int			rllen;

				rllen = readlink(fullpath, linkpath, sizeof(linkpath));
				if (rllen < 0)
				{
					ereport(WARNING,
							(errmsg("could not read symbolic link \"%s\": %m",
									fullpath)));
					continue;
				}
				else if (rllen >= sizeof(linkpath))
				{
					ereport(WARNING,
							(errmsg("symbolic link \"%s\" target is too long",
									fullpath)));
					continue;
				}
				linkpath[rllen] = '\0';

				/*
				 * 当表空间目录位于 PGDATA 内时，
				 * Relpath 保存其相对路径；
				 * 若它是符号链接则保存 NULL。
				 */
				if (rllen > datadirpathlen &&
					strncmp(linkpath, DataDir, datadirpathlen) == 0 &&
					IS_DIR_SEP(linkpath[datadirpathlen]))
					relpath = pstrdup(linkpath + datadirpathlen + 1);

				/*
				 * 向表空间映射文件添加链
				 * 接路径的反斜杠转义版本。
				 */
				initStringInfo(&escapedpath);
				for (s = linkpath; *s; s++)
				{
					if (*s == '\n' || *s == '\r' || *s == '\\')
						appendStringInfoChar(&escapedpath, '\\');
					appendStringInfoChar(&escapedpath, *s);
				}
				appendStringInfo(tblspcmapfile, "%s %s\n",
								 de->d_name, escapedpath.data);
				pfree(escapedpath.data);
			}
			else if (de_type == PGFILETYPE_DIR)
			{
				/*
				 * 可以用 allow_in_place_tablespaces
				 * 直接在 pg_tblspc 下创建目录，
				 * 仅用于测试。
				 *
				 * 此时，
				 * 我们把相对路径而不是绝对路径存入 tablespaceinfo。
				 */
				snprintf(linkpath, sizeof(linkpath), "pg_tblspc/%s",
						 de->d_name);
				relpath = pstrdup(linkpath);
			}
			else
			{
				/* 跳过此处出现的任何其他文件类型。 */
				continue;
			}

			ti = palloc(sizeof(tablespaceinfo));
			ti->oid = pstrdup(de->d_name);
			ti->path = pstrdup(linkpath);
			ti->rpath = relpath;
			ti->size = -1;

			if (tablespaces)
				*tablespaces = lappend(*tablespaces, ti);
		}
		FreeDir(tblspcdir);

		state->starttime = (pg_time_t) time(NULL);
	}
	PG_END_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, DatumGetBool(true));

	state->started_in_recovery = backup_started_in_recovery;

	/*
	 * 标记备份的开始阶段已正确完成。
	 */
	sessionBackupState = SESSION_BACKUP_RUNNING;
}

/*
 * 用于获取正在运行的备份的会话级状态的实用例程。
 */
SessionBackupState
get_backup_status(void)
{
	return sessionBackupState;
}

/*
 * do_pg_backup_stop：
 *
 * 在线备份结束时调用的实用函数。
 * 它创建历史文件（若需要）、重置 sessionBackupState 等等。
 * 它可以选择等待 WAL 段被归档。
 *
 * state 填入从此备份恢复所需的信息，
 * 包括其停止 LSN（stoppoint）、
 *
 * 其时间线 ID（stoptli）等。
 * 验证调用用户权限是本函数调用者的责任！
 */
void
do_pg_backup_stop(BackupState *state, bool waitforarchive)
{
	bool		backup_stopped_in_recovery = false;
	char		histfilepath[MAXPGPATH];
	char		lastxlogfilename[MAXFNAMELEN];
	char		histfilename[MAXFNAMELEN];
	XLogSegNo	_logSegNo;
	FILE	   *fp;
	int			seconds_before_warning;
	int			waits = 0;
	bool		reported_waiting = false;

	Assert(state != NULL);

	backup_stopped_in_recovery = RecoveryInProgress();

	/*
	 * 在恢复期间，
	 * 我们不必检查 WAL 级别。
	 */
	if (!backup_stopped_in_recovery && !XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("WAL level not sufficient for making an online backup"),
				 errhint("wal_level must be set to \"replica\" or \"logical\" at server start.")));

	/*
	 * 可以更新备份计数器和会话级锁。
	 *
	 * 注意：
	 * 更新它们时不得发生 CHECK_FOR_INTERRUPTS()，
	 * 否则它们可能被不一致地更新，从而导致 do_pg_abort_backup() 失败。
	 */
	WALInsertLockAcquireExclusive();

	/*
	 * 期望每次 do_pg_backup_start()
	 * 调用都恰好匹配一次 do_pg_backup_stop()。
	 */
	Assert(XLogCtl->Insert.runningBackups > 0);
	XLogCtl->Insert.runningBackups--;

	/*
	 * 清理会话级锁。
	 *
	 * 你可能认为可以在清理会话级锁之前调用 
	 * WALInsertLockRelease()，
	 * 因为会话级锁不必用 WAL 插入锁保护。
	 * 但由于其中可能发生 CHECK_FOR_INTERRUPTS()，
	 * 必须先清理会话级锁。
	 */
	sessionBackupState = SESSION_BACKUP_NONE;

	WALInsertLockRelease();

	/*
	 * 若我们从备库做在线备份，
	 * 确认备库自备份开始以来未被提升。
	 */
	if (state->started_in_recovery && !backup_stopped_in_recovery)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the standby was promoted during online backup"),
				 errhint("This means that the backup being taken is corrupt "
						 "and should not be used. "
						 "Try taking another online backup.")));

	/*
	 * 在恢复期间，
	 * 我们不写 end-of-backup 记录。
	 * 我们假定 pg_control 最后备份，
	 * 其最小恢复点可用作备份结束位置。
	 * 由于没有 end-of-backup 记录，
	 * 我们在从此备份启动恢复时用 pg_control 值检查是否已到达备份结束。不过我们无法检查 pg_control 是否确实最后备份。
	 * 我们不强制切换到新 WAL 文件，
	 *
	 * 但若 waitforarchive 为 true，
	 * 仍可等待所有所需文件被归档。
	 * 若我们用该备份启动备库并用流复制获取缺失 WAL，
	 * 这没问题。
	 * 但在归档恢复情形下，用户应把 waitforarchive 设为 true 并等待它们归档，
	 * 以确保所有所需文件可用。
	 * 我们返回当前最小恢复点作为备份结束位置。
	 *
	 * 注意：
	 * 若在备份 pg_control 之后更新了最小恢复点，
	 * 它可能大于确切的备份结束位置。
	 * 对当前用法这无害。XXX 目前备份历史文件仅用于信息和调试目的。
	 *
	 * 对在线备份并非必需。
	 * 此外，
	 * 即使创建了，
	 * 恢复期间也不会归档它，
	 * 因为不会调用归档器。因此在恢复期间写备份历史文件似乎不值得。
	 */
	if (backup_stopped_in_recovery)
	{
		XLogRecPtr	recptr;

		/*
		 * 检查在线备份期间重放的所有
		 * WAL 是否包含全页写入。
		 */
		SpinLockAcquire(&XLogCtl->info_lck);
		recptr = XLogCtl->lastFpwDisableRecPtr;
		SpinLockRelease(&XLogCtl->info_lck);

		if (state->startpoint <= recptr)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("WAL generated with full_page_writes=off was replayed "
							"during online backup"),
					 errhint("This means that the backup being taken on the standby "
							 "is corrupt and should not be used. "
							 "Enable full_page_writes and run CHECKPOINT on the primary, "
							 "and then try an online backup again.")));


		LWLockAcquire(ControlFileLock, LW_SHARED);
		state->stoppoint = ControlFile->minRecoveryPoint;
		state->stoptli = ControlFile->minRecoveryPointTLI;
		LWLockRelease(ControlFileLock);
	}
	else
	{
		char	   *history_file;

		/*
		 * 写 end-of-backup 记录。
		 */
		XLogBeginInsert();
		XLogRegisterData((char *) (&state->startpoint),
						 sizeof(state->startpoint));
		state->stoppoint = XLogInsert(RM_XLOG_ID, XLOG_BACKUP_END);

		/*
		 * 鉴于我们不在恢复中，
		 * InsertTimeLineID 已设置且不会改变，因此可以在不持有锁的情况下读取。
		 */
		state->stoptli = XLogCtl->InsertTimeLineID;

		/*
		 * 强制切换到新的 xlog 段文件，
		 * 以便一旦归档了该段，备份就立即有效。
		 */
		RequestXLogSwitch(false);

		state->stoptime = (pg_time_t) time(NULL);

		/*
		 * 创建备份历史文件。
		 */
		XLByteToSeg(state->startpoint, _logSegNo, wal_segment_size);
		BackupHistoryFilePath(histfilepath, state->stoptli, _logSegNo,
							  state->startpoint, wal_segment_size);
		fp = AllocateFile(histfilepath, "w");
		if (!fp)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m",
							histfilepath)));

		/* 构建并保存备份历史文件的内容 */
		history_file = build_backup_content(state, true);
		fprintf(fp, "%s", history_file);
		pfree(history_file);

		if (fflush(fp) || ferror(fp) || FreeFile(fp))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							histfilepath)));

		/*
		 * 清理不再需要的历史文件。
		 * 作为副作用，
		 * 这会为刚创建的历史文件张贴
		 * .ready 消息。
		 */
		CleanupBackupHistory();
	}

	/*
	 * 若启用了归档，
	 * 则在返回前等待所有所需 WAL 文件被归档。
	 * 若未启用归档，
	 * 所需 WAL 需要通过流复制传输（希望 wal_keep_size 设置得足够高），
	 * 或通过某些更奇特的机制，
	 * 如轮询并用脚本从 pg_wal 复制文件。
	 * 我们不了解那些机制，
	 *
	 * 因此确保获得所有所需 WAL 取决于用户。我们等待直到备份期间填满的最后一个 WAL 文件以及历史文件都已归档，
	 * 并假定 WAL 文件的字母序排序属性确保更早的 WAL 文件也已安全归档。
	 * 我们永远等待，
	 * 因为假定 archive_command 应能工作，
	 *
	 * 且管理员希望其备份完全可用。
	 * 若你不想等待，
	 * 则要么传入 waitforarchive 为 false，
	 * 要么可以设置 statement_timeout。
	 * 此外会发出一些通知，提示可能在交互式做此事的人。
	 */

	if (waitforarchive &&
		((!backup_stopped_in_recovery && XLogArchivingActive()) ||
		 (backup_stopped_in_recovery && XLogArchivingAlways())))
	{
		XLByteToPrevSeg(state->stoppoint, _logSegNo, wal_segment_size);
		XLogFileName(lastxlogfilename, state->stoptli, _logSegNo,
					 wal_segment_size);

		XLByteToSeg(state->startpoint, _logSegNo, wal_segment_size);
		BackupHistoryFileName(histfilename, state->stoptli, _logSegNo,
							  state->startpoint, wal_segment_size);

		seconds_before_warning = 60;
		waits = 0;

		while (XLogArchiveIsBusy(lastxlogfilename) ||
			   XLogArchiveIsBusy(histfilename))
		{
			CHECK_FOR_INTERRUPTS();

			if (!reported_waiting && waits > 5)
			{
				ereport(NOTICE,
						(errmsg("base backup done, waiting for required WAL segments to be archived")));
				reported_waiting = true;
			}

			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 1000L,
							 WAIT_EVENT_BACKUP_WAIT_WAL_ARCHIVE);
			ResetLatch(MyLatch);

			if (++waits >= seconds_before_warning)
			{
				seconds_before_warning *= 2;	/* 构建并保存备份历史文件的内容 */
				ereport(WARNING,
						(errmsg("still waiting for all required WAL segments to be archived (%d seconds elapsed)",
								waits),
						 errhint("Check that your archive_command is executing properly.  "
								 "You can safely cancel this backup, "
								 "but the database backup will not be usable without all the WAL segments.")));
			}
		}

		ereport(NOTICE,
				(errmsg("all required WAL segments have been archived")));
	}
	else if (waitforarchive)
		ereport(NOTICE,
				(errmsg("WAL archiving is not enabled; you must ensure that all required WAL segments are copied through other means to complete the backup")));
}


/*
 * do_pg_abort_backup：
 *
 * 中止正在运行的备份。
 * 这只做 do_pg_backup_stop() 最基本的步骤，通过使系统退出备份模式，
 * 从而从错误处理程序调用要安全得多。
 *
 * 'arg' 表示它在备份设置期间被调用；
 * 因此 sessionBackupState 尚未修改，但 runningBackups 已递增。
 * 当它为 false 时，
 * 则作为 before_shmem_exit 处理程序调用，
 * 因此除非 sessionBackupState 表明备份实际在运行，否则我们不得更改状态。
 *
 * 注意：
 * 这被用作 PG_ENSURE_ERROR_CLEANUP 回调和 before_shmem_exit 处理程序，因此签名看起来古怪。
 */
void
do_pg_abort_backup(int code, Datum arg)
{
	bool		during_backup_start = DatumGetBool(arg);

	/* 这会在超过 10 年后回绕... */
	Assert(!during_backup_start || sessionBackupState == SESSION_BACKUP_NONE);

	if (during_backup_start || sessionBackupState != SESSION_BACKUP_NONE)
	{
		WALInsertLockAcquireExclusive();
		Assert(XLogCtl->Insert.runningBackups > 0);
		XLogCtl->Insert.runningBackups--;

		sessionBackupState = SESSION_BACKUP_NONE;
		WALInsertLockRelease();

		if (!during_backup_start)
			ereport(WARNING,
					errmsg("aborting backup due to backend exiting before pg_backup_stop was called"));
	}
}

/*
 * 注册一个在会话结束时警告未终止备份的处理程序，
 * 除非已调用此函数禁用。
 */
void
register_persistent_abort_backup_handler(void)
{
	static bool already_done = false;

	if (already_done)
		return;
	before_shmem_exit(do_pg_abort_backup, DatumGetBool(false));
	already_done = true;
}

/*
 * 获取最新的 WAL 插入指针
 */
XLogRecPtr
GetXLogInsertRecPtr(void)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint64		current_bytepos;

	SpinLockAcquire(&Insert->insertpos_lck);
	current_bytepos = Insert->CurrBytePos;
	SpinLockRelease(&Insert->insertpos_lck);

	return XLogBytePosToRecPtr(current_bytepos);
}

/*
 * 获取最新的 WAL 写指针
 */
XLogRecPtr
GetXLogWriteRecPtr(void)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	LogwrtResult = XLogCtl->LogwrtResult;
	SpinLockRelease(&XLogCtl->info_lck);

	return LogwrtResult.Write;
}

/*
 * 返回上次检查点或重启点的 redo 指针。
 * 这是 WAL 中仍可能需要用于恢复的最旧点。
 */
void
GetOldestRestartPoint(XLogRecPtr *oldrecptr, TimeLineID *oldtli)
{
	LWLockAcquire(ControlFileLock, LW_SHARED);
	*oldrecptr = ControlFile->checkPointCopy.redo;
	*oldtli = ControlFile->checkPointCopy.ThisTimeLineID;
	LWLockRelease(ControlFileLock);
}

/* ShutdownWalRcv() 的简单封装。 */
void
XLogShutdownWalRcv(void)
{
	ShutdownWalRcv();
	ResetInstallXLogFileSegmentActive();
}

/* 启用 WAL 文件回收和预分配。 */
void
SetInstallXLogFileSegmentActive(void)
{
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	XLogCtl->InstallXLogFileSegmentActive = true;
	LWLockRelease(ControlFileLock);
}

/* 禁用 WAL 文件回收和预分配。 */
void
ResetInstallXLogFileSegmentActive(void)
{
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	XLogCtl->InstallXLogFileSegmentActive = false;
	LWLockRelease(ControlFileLock);
}

bool
IsInstallXLogFileSegmentActive(void)
{
	bool		result;

	LWLockAcquire(ControlFileLock, LW_SHARED);
	result = XLogCtl->InstallXLogFileSegmentActive;
	LWLockRelease(ControlFileLock);

	return result;
}

/*
 * 更新 WalWriterSleeping 标志。
 */
void
SetWalWriterSleeping(bool sleeping)
{
	SpinLockAcquire(&XLogCtl->info_lck);
	XLogCtl->WalWriterSleeping = sleeping;
	SpinLockRelease(&XLogCtl->info_lck);
}
