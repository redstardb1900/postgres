/*-------------------------------------------------------------------------
 *
 * xact.h
 *	postgres 事务系统定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/transam.h"
#include "access/xlogreader.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "storage/relfilelocator.h"
#include "storage/sinval.h"

/*
 * 全局事务 ID 的最大长度（含 '\0'）。
 *
 * 注意 GIDSIZE 最大值必须装得进 uint16 gidlen，
 * 见 TwoPhaseFileHeader。
 */
#define GIDSIZE 200

/*
 * 事务隔离级别
 */
#define XACT_READ_UNCOMMITTED	0
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2
#define XACT_SERIALIZABLE		3

extern PGDLLIMPORT int DefaultXactIsoLevel;
extern PGDLLIMPORT int XactIsoLevel;

/*
 * 内部实现三种隔离级别。
 * 较强的两级每个数据库事务用一份快照；
 * 其余每条语句一份快照。
 * 可串行化除快照外还用谓词锁。
 * 检查所选隔离级别应使用这些宏。
 */
#define IsolationUsesXactSnapshot() (XactIsoLevel >= XACT_REPEATABLE_READ)
#define IsolationIsSerializable() (XactIsoLevel == XACT_SERIALIZABLE)

/* 事务只读状态 */
extern PGDLLIMPORT bool DefaultXactReadOnly;
extern PGDLLIMPORT bool XactReadOnly;

/* 本事务中记录语句的标志 */
extern PGDLLIMPORT bool xact_is_sampled;

/*
 * 事务可延迟——目前仅对只读有意义：
 * SERIALIZABLE 事务
 */
extern PGDLLIMPORT bool DefaultXactDeferrable;
extern PGDLLIMPORT bool XactDeferrable;

typedef enum
{
	SYNCHRONOUS_COMMIT_OFF,		/* 异步提交 */
	SYNCHRONOUS_COMMIT_LOCAL_FLUSH, /* 仅等待本地刷写 */
	SYNCHRONOUS_COMMIT_REMOTE_WRITE,	/* 等待本地刷写与远程
										 * 写 */
	SYNCHRONOUS_COMMIT_REMOTE_FLUSH,	/* 等待本地与远程刷写 */
	SYNCHRONOUS_COMMIT_REMOTE_APPLY /* 等待本地与远程刷写以及
									 * 远程应用 */
}			SyncCommitLevel;

/* 定义 synchronous_commit 的默认设置 */
#define SYNCHRONOUS_COMMIT_ON	SYNCHRONOUS_COMMIT_REMOTE_FLUSH

/* 同步提交级别 */
extern PGDLLIMPORT int synchronous_commit;

/* 事务逻辑流式传输期间使用 */
extern PGDLLIMPORT TransactionId CheckXidAlive;
extern PGDLLIMPORT bool bsysscan;

/*
 * 记录顶层事务上发生事件的杂项标志位。
 * 这些标志仅持久化在 MyXactFlags 中，意在让我们
 * 稍后在事务中记得做某些事。此变量全局可访问，
 * 因此代码中任何需要记录标志的地方都可
 * 设置。
 */
extern PGDLLIMPORT int MyXactFlags;

/*
 * XACT_FLAGS_ACCESSEDTEMPNAMESPACE - 访问临时对象时置位。
 * 该情况下不允许 PREPARE TRANSACTION。
 */
#define XACT_FLAGS_ACCESSEDTEMPNAMESPACE		(1U << 0)

/*
 * XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK - 记录顶层事务是否
 * 记过访问排他锁。
 */
#define XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK	(1U << 1)

/*
 * XACT_FLAGS_NEEDIMMEDIATECOMMIT - 记录顶层语句是否
 * 需要立即提交，例如 CREATE DATABASE。
 */
#define XACT_FLAGS_NEEDIMMEDIATECOMMIT			(1U << 2)

/*
 * XACT_FLAGS_PIPELINING - 完成扩展查询协议 Execute 
 * 消息时置位。用于检测是否已通过流水线
 * 创建了隐式事务块。
 */
#define XACT_FLAGS_PIPELINING					(1U << 3)

/*
 *	动态加载模块的事务开始/结束回调
 */
typedef enum
{
	XACT_EVENT_COMMIT,
	XACT_EVENT_PARALLEL_COMMIT,
	XACT_EVENT_ABORT,
	XACT_EVENT_PARALLEL_ABORT,
	XACT_EVENT_PREPARE,
	XACT_EVENT_PRE_COMMIT,
	XACT_EVENT_PARALLEL_PRE_COMMIT,
	XACT_EVENT_PRE_PREPARE
} XactEvent;

typedef void (*XactCallback) (XactEvent event, void *arg);

typedef enum
{
	SUBXACT_EVENT_START_SUB,
	SUBXACT_EVENT_COMMIT_SUB,
	SUBXACT_EVENT_ABORT_SUB,
	SUBXACT_EVENT_PRE_COMMIT_SUB
} SubXactEvent;

typedef void (*SubXactCallback) (SubXactEvent event, SubTransactionId mySubid,
								 SubTransactionId parentSubid, void *arg);

/* Save/RestoreTransactionCharacteristics 的数据结构 */
typedef struct SavedTransactionCharacteristics
{
	int			save_XactIsoLevel;
	bool		save_XactReadOnly;
	bool		save_XactDeferrable;
} SavedTransactionCharacteristics;


/* ----------------
 *	与事务相关的 XLOG 条目
 * ----------------
 */

/*
 * XLOG 允许把一些信息存在日志记录 xl_info 的高 4 位。
 * 我们用 3 位作操作码，1 位表示可选标志变量。
 */
#define XLOG_XACT_COMMIT			0x00
#define XLOG_XACT_PREPARE			0x10
#define XLOG_XACT_ABORT				0x20
#define XLOG_XACT_COMMIT_PREPARED	0x30
#define XLOG_XACT_ABORT_PREPARED	0x40
#define XLOG_XACT_ASSIGNMENT		0x50
#define XLOG_XACT_INVALIDATIONS		0x60
/* 空闲 0x70 */

/* 从 xl_info 滤出操作码的掩码 */
#define XLOG_XACT_OPMASK			0x70

/* 本记录是否有 'xinfo' 字段 */
#define XLOG_XACT_HAS_INFO			0x80

/*
 * 下列存于 xinfo 的标志决定提交/中止记录
 * 包含哪些信息。
 */
#define XACT_XINFO_HAS_DBINFO			(1U << 0)
#define XACT_XINFO_HAS_SUBXACTS			(1U << 1)
#define XACT_XINFO_HAS_RELFILELOCATORS	(1U << 2)
#define XACT_XINFO_HAS_INVALS			(1U << 3)
#define XACT_XINFO_HAS_TWOPHASE			(1U << 4)
#define XACT_XINFO_HAS_ORIGIN			(1U << 5)
#define XACT_XINFO_HAS_AE_LOCKS			(1U << 6)
#define XACT_XINFO_HAS_GID				(1U << 7)
#define XACT_XINFO_HAS_DROPPED_STATS	(1U << 8)

/*
 * 同样存于 xinfo，表示恢复期间模拟事务效果时
 * 还需执行的各种额外动作。
 *
 * 命名为 XactCompletion... 以区别于 
 * EOXact... 例程（在原事务完成末尾）
 * 运行。
 */
#define XACT_COMPLETION_APPLY_FEEDBACK			(1U << 29)
#define XACT_COMPLETION_UPDATE_RELCACHE_FILE	(1U << 30)
#define XACT_COMPLETION_FORCE_SYNC_COMMIT		(1U << 31)

/* 上述标志的访问宏 */
#define XactCompletionApplyFeedback(xinfo) \
	((xinfo & XACT_COMPLETION_APPLY_FEEDBACK) != 0)
#define XactCompletionRelcacheInitFileInval(xinfo) \
	((xinfo & XACT_COMPLETION_UPDATE_RELCACHE_FILE) != 0)
#define XactCompletionForceSyncCommit(xinfo) \
	((xinfo & XACT_COMPLETION_FORCE_SYNC_COMMIT) != 0)

typedef struct xl_xact_assignment
{
	TransactionId xtop;			/* 所赋 XID 的顶层 XID */
	int			nsubxacts;		/* 子事务 XIDs 个数 */
	TransactionId xsub[FLEXIBLE_ARRAY_MEMBER];	/* 已赋 subxids */
} xl_xact_assignment;

#define MinSizeOfXactAssignment offsetof(xl_xact_assignment, xsub)

/*
 * 提交与中止记录可含大量信息。但很大一部分
 * 记录并不需要全部可能字段。因此我们
 * 只包含所需内容。
 *
 * 最小提交/中止记录仅含 xl_xact_commit/abort 
 * 结构体。附加信息是否存在由 'xl_xact_xinfo->xinfo'
 * 中置位表明。xinfo 字段本身
 * 由 xl_info 中 XLOG_XACT_HAS_INFO 置位表示。
 *
 * 注意：各数据块大小应为 sizeof(int) 的倍数，
 * 且只需 int32 对齐。若需更大对齐，
 * 读取时必须拷贝。
 */

/* 提交/中止的子记录 */

typedef struct xl_xact_xinfo
{
	/*
	 * 尽管 xinfo 目前只需两字节空间，我们仍用
	 * 四字节，以免后续记录操心对齐。
	 * 提交记录可能很大，拷贝大段并不
	 * 划算。
	 */
	uint32		xinfo;
} xl_xact_xinfo;

typedef struct xl_xact_dbinfo
{
	Oid			dbId;			/* MyDatabaseId */
	Oid			tsId;			/* MyDatabaseTableSpace */
} xl_xact_dbinfo;

typedef struct xl_xact_subxacts
{
	int			nsubxacts;		/* 子事务 XIDs 个数 */
	TransactionId subxacts[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_subxacts;
#define MinSizeOfXactSubxacts offsetof(xl_xact_subxacts, subxacts)

typedef struct xl_xact_relfilelocators
{
	int			nrels;			/* 关系个数 */
	RelFileLocator xlocators[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_relfilelocators;
#define MinSizeOfXactRelfileLocators offsetof(xl_xact_relfilelocators, xlocators)

/*
 * 事务性删除的统计条目。
 *
 * 声明于此而非 pgstat.h，因为前端代码无法包含 
 * pgstat.h，但 WAL 格式需要能被前端程序
 * 读取。
 */
typedef struct xl_xact_stats_item
{
	int			kind;
	Oid			dboid;
	Oid			objoid;
} xl_xact_stats_item;

typedef struct xl_xact_stats_items
{
	int			nitems;
	xl_xact_stats_item items[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_stats_items;
#define MinSizeOfXactStatsItems offsetof(xl_xact_stats_items, items)

typedef struct xl_xact_invals
{
	int			nmsgs;			/* 共享失效消息个数 */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_invals;
#define MinSizeOfXactInvals offsetof(xl_xact_invals, msgs)

typedef struct xl_xact_twophase
{
	TransactionId xid;
} xl_xact_twophase;

typedef struct xl_xact_origin
{
	XLogRecPtr	origin_lsn;
	TimestampTz origin_timestamp;
} xl_xact_origin;

typedef struct xl_xact_commit
{
	TimestampTz xact_time;		/* 提交时间 */

	/* xl_xact_xinfo 随 XLOG_XACT_HAS_INFO */
	/* xl_xact_dbinfo 随 XINFO_HAS_DBINFO */
	/* xl_xact_subxacts 随 XINFO_HAS_SUBXACT */
	/* xl_xact_relfilelocators 随 XINFO_HAS_RELFILELOCATORS */
	/* xl_xact_stats_items 随 XINFO_HAS_DROPPED_STATS */
	/* xl_xact_invals 随 XINFO_HAS_INVALS */
	/* xl_xact_twophase 随 XINFO_HAS_TWOPHASE */
	/* twophase_gid 随 XINFO_HAS_GID，NUL 结尾串。 */
	/* xl_xact_origin 随 XINFO_HAS_ORIGIN，未对齐！ */
} xl_xact_commit;
#define MinSizeOfXactCommit (offsetof(xl_xact_commit, xact_time) + sizeof(TimestampTz))

typedef struct xl_xact_abort
{
	TimestampTz xact_time;		/* 中止时间 */

	/* xl_xact_xinfo 随 XLOG_XACT_HAS_INFO */
	/* xl_xact_dbinfo 随 XINFO_HAS_DBINFO */
	/* xl_xact_subxacts 随 XINFO_HAS_SUBXACT */
	/* xl_xact_relfilelocators 随 XINFO_HAS_RELFILELOCATORS */
	/* xl_xact_stats_items 随 XINFO_HAS_DROPPED_STATS */
	/* 不需要失效消息。 */
	/* xl_xact_twophase 随 XINFO_HAS_TWOPHASE */
	/* twophase_gid 随 XINFO_HAS_GID，NUL 结尾串。 */
	/* xl_xact_origin 随 XINFO_HAS_ORIGIN，未对齐！ */
} xl_xact_abort;
#define MinSizeOfXactAbort sizeof(xl_xact_abort)

typedef struct xl_xact_prepare
{
	uint32		magic;			/* 格式标识符 */
	uint32		total_len;		/* 实际文件长度 */
	TransactionId xid;			/* 原事务 XID */
	Oid			database;		/* 其所在数据库的 OID */
	TimestampTz prepared_at;	/* 预备时间 */
	Oid			owner;			/* 运行该事务的用户 */
	int32		nsubxacts;		/* 随后子事务 XIDs 个数 */
	int32		ncommitrels;	/* 提交时删除的关系数 */
	int32		nabortrels;		/* 中止时删除的关系数 */
	int32		ncommitstats;	/* 提交时要删的统计数 */
	int32		nabortstats;	/* 中止时要删的统计数 */
	int32		ninvalmsgs;		/* 缓存失效消息个数 */
	bool		initfileinval;	/* relcache 初始化文件需要失效吗？ */
	uint16		gidlen;			/* GID 长度——GID 跟在报头之后 */
	XLogRecPtr	origin_lsn;		/* 源节点上本记录的 lsn */
	TimestampTz origin_timestamp;	/* 源节点上的预备时间 */
} xl_xact_prepare;

/*
 * 上述形式的提交/中止记录解析起来较冗长，因此
 * 有 ParseCommit/AbortRecord() 生成的拆解版本，便于
 * 使用。
 */
typedef struct xl_xact_parsed_commit
{
	TimestampTz xact_time;
	uint32		xinfo;

	Oid			dbId;			/* MyDatabaseId */
	Oid			tsId;			/* MyDatabaseTableSpace */

	int			nsubxacts;
	TransactionId *subxacts;

	int			nrels;
	RelFileLocator *xlocators;

	int			nstats;
	xl_xact_stats_item *stats;

	int			nmsgs;
	SharedInvalidationMessage *msgs;

	TransactionId twophase_xid; /* 仅 2PC */
	char		twophase_gid[GIDSIZE];	/* 仅 2PC */
	int			nabortrels;		/* 仅 2PC */
	RelFileLocator *abortlocators;	/* 仅 2PC */
	int			nabortstats;	/* 仅 2PC */
	xl_xact_stats_item *abortstats; /* 仅 2PC */

	XLogRecPtr	origin_lsn;
	TimestampTz origin_timestamp;
} xl_xact_parsed_commit;

typedef xl_xact_parsed_commit xl_xact_parsed_prepare;

typedef struct xl_xact_parsed_abort
{
	TimestampTz xact_time;
	uint32		xinfo;

	Oid			dbId;			/* MyDatabaseId */
	Oid			tsId;			/* MyDatabaseTableSpace */

	int			nsubxacts;
	TransactionId *subxacts;

	int			nrels;
	RelFileLocator *xlocators;

	int			nstats;
	xl_xact_stats_item *stats;

	TransactionId twophase_xid; /* 仅 2PC */
	char		twophase_gid[GIDSIZE];	/* 仅 2PC */

	XLogRecPtr	origin_lsn;
	TimestampTz origin_timestamp;
} xl_xact_parsed_abort;


/* ----------------
 *	extern 声明
 * ----------------
 */
extern bool IsTransactionState(void);
extern bool IsAbortedTransactionBlockState(void);
extern TransactionId GetTopTransactionId(void);
extern TransactionId GetTopTransactionIdIfAny(void);
extern TransactionId GetCurrentTransactionId(void);
extern TransactionId GetCurrentTransactionIdIfAny(void);
extern TransactionId GetStableLatestTransactionId(void);
extern SubTransactionId GetCurrentSubTransactionId(void);
extern FullTransactionId GetTopFullTransactionId(void);
extern FullTransactionId GetTopFullTransactionIdIfAny(void);
extern FullTransactionId GetCurrentFullTransactionId(void);
extern FullTransactionId GetCurrentFullTransactionIdIfAny(void);
extern void MarkCurrentTransactionIdLoggedIfAny(void);
extern bool SubTransactionIsActive(SubTransactionId subxid);
extern CommandId GetCurrentCommandId(bool used);
extern void SetParallelStartTimestamps(TimestampTz xact_ts, TimestampTz stmt_ts);
extern TimestampTz GetCurrentTransactionStartTimestamp(void);
extern TimestampTz GetCurrentStatementStartTimestamp(void);
extern TimestampTz GetCurrentTransactionStopTimestamp(void);
extern void SetCurrentStatementStartTimestamp(void);
extern int	GetCurrentTransactionNestLevel(void);
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
extern void CommandCounterIncrement(void);
extern void ForceSyncCommit(void);
extern void StartTransactionCommand(void);
extern void SaveTransactionCharacteristics(SavedTransactionCharacteristics *s);
extern void RestoreTransactionCharacteristics(const SavedTransactionCharacteristics *s);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern bool EndTransactionBlock(bool chain);
extern bool PrepareTransactionBlock(const char *gid);
extern void UserAbortTransactionBlock(bool chain);
extern void BeginImplicitTransactionBlock(void);
extern void EndImplicitTransactionBlock(void);
extern void ReleaseSavepoint(const char *name);
extern void DefineSavepoint(const char *name);
extern void RollbackToSavepoint(const char *name);
extern void BeginInternalSubTransaction(const char *name);
extern void ReleaseCurrentSubTransaction(void);
extern void RollbackAndReleaseCurrentSubTransaction(void);
extern bool IsSubTransaction(void);
extern Size EstimateTransactionStateSpace(void);
extern void SerializeTransactionState(Size maxsize, char *start_address);
extern void StartParallelWorkerTransaction(char *tstatespace);
extern void EndParallelWorkerTransaction(void);
extern bool IsTransactionBlock(void);
extern bool IsTransactionOrTransactionBlock(void);
extern char TransactionBlockStatusCode(void);
extern void AbortOutOfAnyTransaction(void);
extern void PreventInTransactionBlock(bool isTopLevel, const char *stmtType);
extern void RequireTransactionBlock(bool isTopLevel, const char *stmtType);
extern void WarnNoTransactionBlock(bool isTopLevel, const char *stmtType);
extern bool IsInTransactionBlock(bool isTopLevel);
extern void RegisterXactCallback(XactCallback callback, void *arg);
extern void UnregisterXactCallback(XactCallback callback, void *arg);
extern void RegisterSubXactCallback(SubXactCallback callback, void *arg);
extern void UnregisterSubXactCallback(SubXactCallback callback, void *arg);

extern bool IsSubxactTopXidLogPending(void);
extern void MarkSubxactTopXidLogged(void);

extern int	xactGetCommittedChildren(TransactionId **ptr);

extern XLogRecPtr XactLogCommitRecord(TimestampTz commit_time,
									  int nsubxacts, TransactionId *subxacts,
									  int nrels, RelFileLocator *rels,
									  int ndroppedstats,
									  xl_xact_stats_item *droppedstats,
									  int nmsgs, SharedInvalidationMessage *msgs,
									  bool relcacheInval,
									  int xactflags,
									  TransactionId twophase_xid,
									  const char *twophase_gid);

extern XLogRecPtr XactLogAbortRecord(TimestampTz abort_time,
									 int nsubxacts, TransactionId *subxacts,
									 int nrels, RelFileLocator *rels,
									 int ndroppedstats,
									 xl_xact_stats_item *droppedstats,
									 int xactflags, TransactionId twophase_xid,
									 const char *twophase_gid);
extern void xact_redo(XLogReaderState *record);

/* xactdesc.c */
extern void xact_desc(StringInfo buf, XLogReaderState *record);
extern const char *xact_identify(uint8 info);

/* 也在 xactdesc.c，以便前后端代码共享 */
extern void ParseCommitRecord(uint8 info, xl_xact_commit *xlrec, xl_xact_parsed_commit *parsed);
extern void ParseAbortRecord(uint8 info, xl_xact_abort *xlrec, xl_xact_parsed_abort *parsed);
extern void ParsePrepareRecord(uint8 info, xl_xact_prepare *xlrec, xl_xact_parsed_prepare *parsed);

extern void EnterParallelMode(void);
extern void ExitParallelMode(void);
extern bool IsInParallelMode(void);

#endif							/* XACT_H */
