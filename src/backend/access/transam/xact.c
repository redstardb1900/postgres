/*-------------------------------------------------------------------------
 *
 * xact.c
 *	  顶层事务系统支持例程
 *
 * 更多信息见 src/backend/access/transam/README。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/xact.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "access/commit_ts.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_enum.h"
#include "catalog/storage.h"
#include "commands/async.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "libpq/be-fsstubs.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/snapbuild.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/combocid.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/*
 * 用户可调整的参数
 */
int			DefaultXactIsoLevel = XACT_READ_COMMITTED;
int			XactIsoLevel = XACT_READ_COMMITTED;

bool		DefaultXactReadOnly = false;
bool		XactReadOnly;

bool		DefaultXactDeferrable = false;
bool		XactDeferrable;

int			synchronous_commit = SYNCHRONOUS_COMMIT_ON;

/*
 * CheckXidAlive 是一个指向可能正在进行中的（子）事务的XID值。
 * 目前，它在逻辑解码中使用。此类事务有可能在解码进行过程中被中止，
 * 在这种情况下我们会跳过对该特定事务的解码。为确保这一点，
 * 我们在从系统表中获取元组后检查 CheckXidAlive 是否已被中止。
 * 我们还确保在逻辑解码期间不直接访问 tableam 或 heap APIs，
 * 因为我们只在 systable_* API 中检查并发中止。
 *
 *
 */
TransactionId CheckXidAlive = InvalidTransactionId;
bool		bsysscan = false;

/*
 * 当作为并行工作进程运行时，我们只在并行工作进程的状态栈上放置单个
 * TransactionStateData，其中反映的XID将是启动并行性的后端中当前活动的*最内层*
 * 子事务的XID。但是，GetTopTransactionId() 和 TransactionIdIsCurrentTransactionId()
 * 需要在并行工作进程中返回与用户后端相同的答案，因此我们需要一些额外的记账。
 *
 * XactTopFullTransactionId 存储我们顶层事务的XID，它在普通后端中
 * 与 TopTransactionStateData.fullTransactionId 相同；但在并行后端中，
 * 由于没有完整的事务状态，它将从启动并行操作的后端复制过来。
 *
 * nParallelCurrentXids 在普通后端中将为0且ParallelCurrentXids为NULL，
 * 但在并行后端中，nParallelCurrentXids 将包含需要被视为当前的 XIDs 数量，
 * 而ParallelCurrentXids将包含 XIDs 本身。这包括在并行操作开始时在父级中
 * 当前或子提交的所有 XIDss。XIDs 按照数值顺序（非逻辑顺序）排序存储，
 * 以便尽可能快地进行查找。
 *
 *
 *
 *
 *
 *
 *
 */
static FullTransactionId XactTopFullTransactionId = {InvalidTransactionId};
static int	nParallelCurrentXids = 0;
static TransactionId *ParallelCurrentXids;

/*
 * 各种标志位用于记录顶层事务中发生的事件。这些标志位仅持久化在
 * MyXactFlags 中，目的是让我们记住稍后在事务中需要做某些事情。
 * 这是全局可访问的，因此可以从任何需要记录标志的地方设置。
 *
 *
 */
int			MyXactFlags;

/*
 * 事务状态 - 从服务器角度的事务状态
 */
typedef enum TransState
{
	TRANS_DEFAULT,				/* 空闲 */
	TRANS_START,				/* 事务启动中 */
	TRANS_INPROGRESS,			/* 在有效事务内部 */
	TRANS_COMMIT,				/* 提交进行中 */
	TRANS_ABORT,				/* 中止进行中 */
	TRANS_PREPARE				/* 预备进行中 */
} TransState;

/*
 * 事务块状态 - 客户端查询的事务状态
 *
 * 注意：子事务状态仅用于非顶层事务；
 * 其他状态仅出现在顶层事务中。
 */
typedef enum TBlockState
{
	/* 非事务块状态 */
	TBLOCK_DEFAULT,				/* 空闲 */
	TBLOCK_STARTED,				/* 运行单查询事务 */

	/* 事务块状态 */
	TBLOCK_BEGIN,				/* 正在启动事务块 */
	TBLOCK_INPROGRESS,			/* 活跃事务 */
	TBLOCK_IMPLICIT_INPROGRESS, /* 隐式 BEGIN 后的活跃事务 */
	TBLOCK_PARALLEL_INPROGRESS, /* 并行工作进程内的活跃事务 */
	TBLOCK_END,					/* 已收到 COMMIT */
	TBLOCK_ABORT,				/* 失败的事务，等待 ROLLBACK */
	TBLOCK_ABORT_END,			/* 失败的事务，已收到 ROLLBACK */
	TBLOCK_ABORT_PENDING,		/* 活跃事务，已收到 ROLLBACK */
	TBLOCK_PREPARE,				/* 活跃事务，已收到 PREPARE */

	/* 子事务状态 */
	TBLOCK_SUBBEGIN,			/* 正在启动子事务 */
	TBLOCK_SUBINPROGRESS,		/* 活跃子事务 */
	TBLOCK_SUBRELEASE,			/* 已收到 RELEASE */
	TBLOCK_SUBCOMMIT,			/* TBLOCK_SUBINPROGRESS 时收到 COMMIT */
	TBLOCK_SUBABORT,			/* 失败的子事务，等待 ROLLBACK */
	TBLOCK_SUBABORT_END,		/* 失败的子事务，已收到 ROLLBACK */
	TBLOCK_SUBABORT_PENDING,	/* 活跃子事务，已收到 ROLLBACK */
	TBLOCK_SUBRESTART,			/* 活跃子事务，已收到 ROLLBACK TO */
	TBLOCK_SUBABORT_RESTART		/* 失败的子事务，已收到 ROLLBACK TO */
} TBlockState;

/*
 * 事务状态结构
 */
typedef struct TransactionStateData
{
	FullTransactionId fullTransactionId;	/* 我的 FullTransactionId */
	SubTransactionId subTransactionId;	/* 我的子事务 ID */
	char	   *name;			/* 保存点名称，如果有的话 */
	int			savepointLevel; /* 保存点级别 */
	TransState	state;			/* 底层状态 */
	TBlockState blockState;		/* 高层状态 */
	int			nestingLevel;	/* 事务嵌套深度 */
	int			gucNestLevel;	/* GUC 上下文嵌套深度 */
	MemoryContext curTransactionContext;	/* 我的事务生命周期上下文 */
	ResourceOwner curTransactionOwner;	/* 我的查询资源 */
	TransactionId *childXids;	/* 子提交的子事务 XIDs，按 XID 顺序排列 */
	int			nChildXids;		/* 子提交的子事务 XIDs 数量 */
	int			maxChildXids;	/* childXids[] 的已分配大小 */
	Oid			prevUser;		/* 之前的 CurrentUserId 设置 */
	int			prevSecContext; /* 之前的 SecurityRestrictionContext */
	bool		prevXactReadOnly;	/* 进入时的事务只读状态 */
	bool		startedInRecovery;	/* 是否在恢复期间启动？ */
	bool		didLogXid;		/* XID 是否已包含在 WAL 记录中？ */
	int			parallelModeLevel;	/* Enter/ExitParallelMode 计数器 */
	bool		chain;			/* 在此事务后启动新块 */
	bool		topXidLogged;	/* 对于子事务：顶层 XID 是否已记录？ */
	struct TransactionStateData *parent;	/* 回指父级的链接 */
} TransactionStateData;

typedef TransactionStateData *TransactionState;

/*
 * 用于通过共享内存向并行工作进程传输事务状态的序列化表示。
 *
 */
typedef struct SerializedTransactionState
{
	int			xactIsoLevel;
	bool		xactDeferrable;
	FullTransactionId topFullTransactionId;
	FullTransactionId currentFullTransactionId;
	CommandId	currentCommandId;
	int			nParallelCurrentXids;
	TransactionId parallelCurrentXids[FLEXIBLE_ARRAY_MEMBER];
} SerializedTransactionState;

/* SerializedTransactionState 的大小，不包括最后的数组。 */
#define SerializedTransactionStateHeaderSize \
	offsetof(SerializedTransactionState, parallelCurrentXids)

/*
 * CurrentTransactionState 始终指向当前事务状态块。
 * 当完全不在事务中时，或者在顶层事务中时，它将指向 TopTransactionStateData。
 *
 */
static TransactionStateData TopTransactionStateData = {
	.state = TRANS_DEFAULT,
	.blockState = TBLOCK_DEFAULT,
	.topXidLogged = false,
};

/*
 * unreportedXids 包含尚未在 XLOG_XACT_ASSIGNMENT 记录中报告的所有子事务的XIDs。
 *
 */
static int	nUnreportedXids;
static TransactionId unreportedXids[PGPROC_MAX_CACHED_SUBXIDS];

static TransactionState CurrentTransactionState = &TopTransactionStateData;

/*
 * 子事务ID和命令ID分配计数器是整个事务全局的，
 * 因此我们不会将它们保存在状态栈中。
 */
static SubTransactionId currentSubTransactionId;
static CommandId currentCommandId;
static bool currentCommandIdUsed;

/*
 * xactStartTimestamp 是 transaction_timestamp() 的值。
 * stmtStartTimestamp 是 statement_timestamp() 的值。
 * xactStopTimestamp 是我们记录提交/中止WAL记录的时间，
 * 或者如果跳过了该记录，则是在此之后第一次调用
 * GetCurrentTransactionStopTimestamp() 的时间。
 *
 * 这些值在进入和退出子事务时不发生变化，所以我们不会
 * 将它们保存在 TransactionState 栈中。
 */
static TimestampTz xactStartTimestamp;
static TimestampTz stmtStartTimestamp;
static TimestampTz xactStopTimestamp;

/*
 * 用于预备当前事务的 GID。这也是整个事务全局的，
 * 因此我们不会将它保存在状态栈中。
 */
static char *prepareGID;

/*
 * 有些命令想要强制同步提交。
 */
static bool forceSyncCommit = false;

/* 事务内语句日志记录标志。 */
bool		xact_is_sampled = false;

/*
 * 事务中止工作的私有上下文 --- 我们在启动时预留此空间，
 * 以确保当内存耗尽时，AbortTransaction 和 AbortSubTransaction 可以工作。
 *
 */
static MemoryContext TransactionAbortContext = NULL;

/*
 * 附加的事务开始和结束回调列表
 */
typedef struct XactCallbackItem
{
	struct XactCallbackItem *next;
	XactCallback callback;
	void	   *arg;
} XactCallbackItem;

static XactCallbackItem *Xact_callbacks = NULL;

/*
 * 附加的子事务开始和结束回调列表
 */
typedef struct SubXactCallbackItem
{
	struct SubXactCallbackItem *next;
	SubXactCallback callback;
	void	   *arg;
} SubXactCallbackItem;

static SubXactCallbackItem *SubXact_callbacks = NULL;


/* 本地函数原型 */
static void AssignTransactionId(TransactionState s);
static void AbortTransaction(void);
static void AtAbort_Memory(void);
static void AtCleanup_Memory(void);
static void AtAbort_ResourceOwner(void);
static void AtCCI_LocalCache(void);
static void AtCommit_Memory(void);
static void AtStart_Cache(void);
static void AtStart_Memory(void);
static void AtStart_ResourceOwner(void);
static void CallXactCallbacks(XactEvent event);
static void CallSubXactCallbacks(SubXactEvent event,
								 SubTransactionId mySubid,
								 SubTransactionId parentSubid);
static void CleanupTransaction(void);
static void CheckTransactionBlock(bool isTopLevel, bool throwError,
								  const char *stmtType);
static void CommitTransaction(void);
static TransactionId RecordTransactionAbort(bool isSubXact);
static void StartTransaction(void);

static void StartSubTransaction(void);
static void CommitSubTransaction(void);
static void AbortSubTransaction(void);
static void CleanupSubTransaction(void);
static void PushTransaction(void);
static void PopTransaction(void);

static void AtSubAbort_Memory(void);
static void AtSubCleanup_Memory(void);
static void AtSubAbort_ResourceOwner(void);
static void AtSubCommit_Memory(void);
static void AtSubStart_Memory(void);
static void AtSubStart_ResourceOwner(void);

static void ShowTransactionState(const char *str);
static void ShowTransactionStateRec(const char *str, TransactionState s);
static const char *BlockStateAsString(TBlockState blockState);
static const char *TransStateAsString(TransState state);


/* ----------------------------------------------------------------
 *	事务状态访问器
 * ----------------------------------------------------------------
 */

/*
 *	IsTransactionState
 *
 *	如果我们在一个有效的事务内部，这将返回true；也就是说，
 *	启动数据库访问、获取重量级锁等操作是安全的。
 */
bool
IsTransactionState(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * TRANS_DEFAULT 和 TRANS_ABORT 显然是不安全的状态。然而，我们也
	 * 拒绝启动/关闭状态 TRANS_START、TRANS_COMMIT、
	 * TRANS_PREPARE，因为在这些转换状态中可能为时过早或过晚，
	 * 无法执行任何有趣的操作。因此，唯一"有效"的状态是 TRANS_INPROGRESS。
 *
 */
	return (s->state == TRANS_INPROGRESS);
}

/*
 *	IsAbortedTransactionBlockState
 *
 *	如果我们在一个已中止的事务块中，这将返回true。
 */
bool
IsAbortedTransactionBlockState(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_ABORT ||
		s->blockState == TBLOCK_SUBABORT)
		return true;

	return false;
}


/*
 *	GetTopTransactionId
 *
 * 这将返回主事务的XID，如果尚未设置则分配一个。请注意只在有效事务内部调用此函数。
 *
 */
TransactionId
GetTopTransactionId(void)
{
	if (!FullTransactionIdIsValid(XactTopFullTransactionId))
		AssignTransactionId(&TopTransactionStateData);
	return XidFromFullTransactionId(XactTopFullTransactionId);
}

/*
 *	GetTopTransactionIdIfAny
 *
 * 如果已分配，这将返回主事务的XID。
 * 如果我们当前不在事务中，或在尚未分配XID的事务中，它将返回 InvalidTransactionId。
 *
 */
TransactionId
GetTopTransactionIdIfAny(void)
{
	return XidFromFullTransactionId(XactTopFullTransactionId);
}

/*
 *	GetCurrentTransactionId
 *
 * 这将返回当前事务（主事务或子事务）的XID，如果尚未设置则分配一个。
 * 请注意只在有效事务内部调用此函数。
 *
 */
TransactionId
GetCurrentTransactionId(void)
{
	TransactionState s = CurrentTransactionState;

	if (!FullTransactionIdIsValid(s->fullTransactionId))
		AssignTransactionId(s);
	return XidFromFullTransactionId(s->fullTransactionId);
}

/*
 *	GetCurrentTransactionIdIfAny
 *
 * 如果已分配，这将返回当前子事务的XID。
 * 如果我们当前不在事务中，或在尚未分配XID的事务中，它将返回 InvalidTransactionId。
 *
 */
TransactionId
GetCurrentTransactionIdIfAny(void)
{
	return XidFromFullTransactionId(CurrentTransactionState->fullTransactionId);
}

/*
 *	GetTopFullTransactionId
 *
 * 这将返回主事务的FullTransactionId，如果尚未设置则分配一个。
 * 请注意只在有效事务内部调用此函数。
 */
FullTransactionId
GetTopFullTransactionId(void)
{
	if (!FullTransactionIdIsValid(XactTopFullTransactionId))
		AssignTransactionId(&TopTransactionStateData);
	return XactTopFullTransactionId;
}

/*
 *	GetTopFullTransactionIdIfAny
 *
 * 如果已分配，这将返回主事务的FullTransactionId。
 * 如果我们当前不在事务中，或在尚未分配FullTransactionId的事务中，
 * 它将返回 InvalidFullTransactionId。
 *
 */
FullTransactionId
GetTopFullTransactionIdIfAny(void)
{
	return XactTopFullTransactionId;
}

/*
 *	GetCurrentFullTransactionId
 *
 * 这将返回当前事务（主事务或子事务）的FullTransactionId，如果尚未设置则分配一个。
 * 请注意只在有效事务内部调用此函数。
 *
 */
FullTransactionId
GetCurrentFullTransactionId(void)
{
	TransactionState s = CurrentTransactionState;

	if (!FullTransactionIdIsValid(s->fullTransactionId))
		AssignTransactionId(s);
	return s->fullTransactionId;
}

/*
 *	GetCurrentFullTransactionIdIfAny
 *
 * 如果已分配，这将返回当前子事务的FullTransactionId。
 * 如果我们当前不在事务中，或在尚未分配FullTransactionId的事务中，
 * 它将返回 InvalidFullTransactionId。
 *
 */
FullTransactionId
GetCurrentFullTransactionIdIfAny(void)
{
	return CurrentTransactionState->fullTransactionId;
}

/*
 *	MarkCurrentTransactionIdLoggedIfAny
 *
 * 记住当前事务ID - 如果已分配 - 现在已经被WAL日志记录。
 */
void
MarkCurrentTransactionIdLoggedIfAny(void)
{
	if (FullTransactionIdIsValid(CurrentTransactionState->fullTransactionId))
		CurrentTransactionState->didLogXid = true;
}

/*
 * IsSubxactTopXidLogPending
 *
 * 这用于决定我们是否需要为子事务中的操作写入顶层XID的WAL日志，
 * 如果尚未写入的话。我们需要为逻辑解码执行此操作，参见
 * LogicalDecodingProcessRecord。
 *
 * 如果wal_level >= logical且我们在有效的子事务中，
 * 但对于该子事务的赋值尚未写入任何WAL记录，则返回true。
 *
 */
bool
IsSubxactTopXidLogPending(void)
{
	/* 检查是否已被日志记录 */
	if (CurrentTransactionState->topXidLogged)
		return false;

	/* wal_level 必须为 logical */
	if (!XLogLogicalInfoActive())
		return false;

	/* 我们需要处于事务状态中 */
	if (!IsTransactionState())
		return false;

	/* 必须是子事务 */
	if (!IsSubTransaction())
		return false;

	/* 子事务必须已分配 XID */
	if (!TransactionIdIsValid(GetCurrentTransactionIdIfAny()))
		return false;

	return true;
}

/*
 * MarkSubxactTopXidLogged
 *
 * 记住当前子事务的顶层事务ID现在已被WAL日志记录。
 *
 */
void
MarkSubxactTopXidLogged(void)
{
	Assert(IsSubxactTopXidLogPending());

	CurrentTransactionState->topXidLogged = true;
}

/*
 *	GetStableLatestTransactionId
 *
 * 获取事务的XID（如果有的话），否则读取下一个待分配的XID。
 * 一旦我们有了一个值，在当前事务的剩余时间内返回相同的值。
 * 这是为了为age(xid)函数提供参考点，但也可能对其他维护任务有用。
 *
 */
TransactionId
GetStableLatestTransactionId(void)
{
	static LocalTransactionId lxid = InvalidLocalTransactionId;
	static TransactionId stablexid = InvalidTransactionId;

	if (lxid != MyProc->lxid)
	{
		lxid = MyProc->lxid;
		stablexid = GetTopTransactionIdIfAny();
		if (!TransactionIdIsValid(stablexid))
			stablexid = ReadNextTransactionId();
	}

	Assert(TransactionIdIsValid(stablexid));

	return stablexid;
}

/*
 * AssignTransactionId
 *
 * 为给定的TransactionStates分配一个新的永久FullTransactionId。
 * 在调用此函数之前/除非调用此函数，否则我们不会为事务分配XIDs。
 * 此外，任何尚无XID的父TransactionState也将被分配一个XID；
 * 这维持了一个不变量：子事务具有紧随其父事务之后的XID。
 *
 */
static void
AssignTransactionId(TransactionState s)
{
	bool		isSubXact = (s->parent != NULL);
	ResourceOwner currentOwner;
	bool		log_unknown_top = false;

	/* 断言调用者没有搞砸 */
	Assert(!FullTransactionIdIsValid(s->fullTransactionId));
	Assert(s->state == TRANS_INPROGRESS);

	/*
	 * 工作进程在每个并行操作开始时同步事务状态，
	 * 所以我们此时无法处理新的 XIDs。
	 */
	if (IsInParallelMode() || IsParallelWorker())
		elog(ERROR, "cannot assign XIDs during a parallel operation");

	/*
	 * 确保父事务有 XIDs，这样子事务的 XID 总是晚于其父事务。
	 * 不能在这里递归，否则如果我们在一个巨大的子事务栈底部
	 * （这些子事务都还没有 XID），可能会导致栈溢出。
 *
 */
	if (isSubXact && !FullTransactionIdIsValid(s->parent->fullTransactionId))
	{
		TransactionState p = s->parent;
		TransactionState *parents;
		size_t		parentOffset = 0;

		parents = palloc(sizeof(TransactionState) * s->nestingLevel);
		while (p != NULL && !FullTransactionIdIsValid(p->fullTransactionId))
		{
			parents[parentOffset++] = p;
			p = p->parent;
		}

		/*
		 * 从技术上讲这是一个递归调用，但递归深度永远不会超过一层。
 *
 */
		while (parentOffset != 0)
			AssignTransactionId(parents[--parentOffset]);

		pfree(parents);
	}

	/*
	 * 当wal_level=logical时，保证子事务的xid只有在其顶层xid之前已经被记录的情况下
	 * 才能在WAL流中被看到。如果必要，我们会用少于PGPROC_MAX_CACHED_SUBXIDS个数
	 * 记录一条xact_assignment记录。注意，即使一个xid出现在WAL记录中而没有设置didLogXid，
	 * 这也是可以的，我们只是可能多余地记录了一些东西。当xid包含在WAL记录内部
	 * 而不是在XLogRecord->xl_xid中时就会发生这种情况，例如在xl_standby_locks中。
 *
 *
 *
 */
	if (isSubXact && XLogLogicalInfoActive() &&
		!TopTransactionStateData.didLogXid)
		log_unknown_top = true;

	/*
	 * 生成一个新的FullTransactionId并在PGPROC和pg_subtrans中记录其xid。
	 *
	 * 注意：我们必须在Xid出现在共享存储中除PGPROC之外的任何地方之前
	 * 创建subtrans条目；因为如果PGPROC中没有空间，subtrans条目是确保
	 * 其他后端将Xid视为"运行中"所需的。参见GetNewTransactionId。
 *
 *
 */
	s->fullTransactionId = GetNewTransactionId(isSubXact);
	if (!isSubXact)
		XactTopFullTransactionId = s->fullTransactionId;

	if (isSubXact)
		SubTransSetParent(XidFromFullTransactionId(s->fullTransactionId),
						  XidFromFullTransactionId(s->parent->fullTransactionId));

	/*
	 * 如果是顶层事务，谓词锁定系统也需要被告知此事。
 *
 */
	if (!isSubXact)
		RegisterPredicateLockingXid(XidFromFullTransactionId(s->fullTransactionId));

	/*
	 * 获取对事务XID的锁。（我们假设这不会阻塞。）我们必须确保
	 * 锁被分配给事务自己的ResourceOwner。
 *
 */
	currentOwner = CurrentResourceOwner;
	CurrentResourceOwner = s->curTransactionOwner;

	XactLockTableInsert(XidFromFullTransactionId(s->fullTransactionId));

	CurrentResourceOwner = currentOwner;

	/*
	 * 在每个顶层事务中，每分配 PGPROC_MAX_CACHED_SUBXIDS 个事务 ID，
	 * 我们就会为该分配发出一条 WAL 记录。我们包含顶层 XIDs 和所有
	 * 尚未使用 XLOG_XACT_ASSIGNMENT 记录报告的子事务 XID。
	 *
	 * 这是限制热备服务器跟踪进行中 XID 所需共享内存量所必需的。
	 * 参见 RecordKnownAssignedTransactionIds() 的注释。
	 *
	 * 我们不跟踪每个子事务 XID 的直接父事务，只跟踪每个子事务所属的
	 * 顶层事务。这在恢复中是正确的，因为中止的子事务会单独进行 WAL 记录。
	 *
	 * 即使在我们之上的几层在我们递归向上处理之前没有分配 XID 的情况下，
	 * 这也是正确的。
 *
 *
 *
 *
 */
	if (isSubXact && XLogStandbyInfoActive())
	{
		unreportedXids[nUnreportedXids] = XidFromFullTransactionId(s->fullTransactionId);
		nUnreportedXids++;

		/*
		 * 确保此测试与RecoverPreparedTransactions()中的类似测试相匹配
 *
 */
		if (nUnreportedXids >= PGPROC_MAX_CACHED_SUBXIDS ||
			log_unknown_top)
		{
			xl_xact_assignment xlrec;

			/*
			 * xtop总是现在设置的，因为我们递归遍历事务栈
			 * 到最高未分配的xid，然后再往下回来
			 */
			xlrec.xtop = GetTopTransactionId();
			Assert(TransactionIdIsValid(xlrec.xtop));
			xlrec.nsubxacts = nUnreportedXids;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, MinSizeOfXactAssignment);
			XLogRegisterData((char *) unreportedXids,
							 nUnreportedXids * sizeof(TransactionId));

			(void) XLogInsert(RM_XACT_ID, XLOG_XACT_ASSIGNMENT);

			nUnreportedXids = 0;
			/* 标记顶层事务（而非当前事务）已被日志记录 */
			TopTransactionStateData.didLogXid = true;
		}
	}
}

/*
 *	GetCurrentSubTransactionId
 */
SubTransactionId
GetCurrentSubTransactionId(void)
{
	TransactionState s = CurrentTransactionState;

	return s->subTransactionId;
}

/*
 *	SubTransactionIsActive
 *
 * 测试指定的子事务ID是否仍然活跃。注意，调用者负责检查
 * 此ID是否与当前事务相关。
 */
bool
SubTransactionIsActive(SubTransactionId subxid)
{
	TransactionState s;

	for (s = CurrentTransactionState; s != NULL; s = s->parent)
	{
		if (s->state == TRANS_ABORT)
			continue;
		if (s->subTransactionId == subxid)
			return true;
	}
	return false;
}


/*
 *	GetCurrentCommandId
 *
 * "used"必须为true，如果调用者打算使用命令ID来标记
 * 插入/更新/删除的元组。false表示ID被获取
 * 用于只读目的（即，作为快照有效性截止点）。参见
 * CommandCounterIncrement() 了解讨论。
 */
CommandId
GetCurrentCommandId(bool used)
{
	/* 这对整个事务是全局的，而非子事务局部的 */
	if (used)
	{
		/*
		 * 禁止在并行工作进程中设置 currentCommandIdUsed，因为我们
		 * 没有机制将此信息回传给领导者。当 currentCommandIdUsed 在
		 * 并行操作开始时已经为 true 时，我们可以放宽此限制。
 *
 */
		Assert(!IsParallelWorker());
		currentCommandIdUsed = true;
	}
	return currentCommandId;
}

/*
 *	SetParallelStartTimestamps
 *
 * 在并行工作进程中，我们应该继承父事务的时间戳
 * 而不是设置自己的时间戳。并行工作进程
 * 基础设施必须在调用StartTransaction()或
 * SetCurrentStatementStartTimestamp()之前调用此函数来提供这些值。
 */
void
SetParallelStartTimestamps(TimestampTz xact_ts, TimestampTz stmt_ts)
{
	Assert(IsParallelWorker());
	xactStartTimestamp = xact_ts;
	stmtStartTimestamp = stmt_ts;
}

/*
 *	GetCurrentTransactionStartTimestamp
 */
TimestampTz
GetCurrentTransactionStartTimestamp(void)
{
	return xactStartTimestamp;
}

/*
 *	GetCurrentStatementStartTimestamp
 */
TimestampTz
GetCurrentStatementStartTimestamp(void)
{
	return stmtStartTimestamp;
}

/*
 *	GetCurrentTransactionStopTimestamp
 *
 * 如果事务停止时间还没有被设置，这可能会发生在
 * 我们决定不需要记录XLOG记录的情况下，设置xactStopTimestamp。
 */
TimestampTz
GetCurrentTransactionStopTimestamp(void)
{
	TransactionState s PG_USED_FOR_ASSERTS_ONLY = CurrentTransactionState;

	/* 应该只在提交/中止处理之后调用 */
	Assert(s->state == TRANS_DEFAULT ||
		   s->state == TRANS_COMMIT ||
		   s->state == TRANS_ABORT ||
		   s->state == TRANS_PREPARE);

	if (xactStopTimestamp == 0)
		xactStopTimestamp = GetCurrentTimestamp();

	return xactStopTimestamp;
}

/*
 *	SetCurrentStatementStartTimestamp
 *
 * 在并行工作进程中，这应该已经通过调用 SetParallelStartTimestamps() 提供了。
 *
 */
void
SetCurrentStatementStartTimestamp(void)
{
	if (!IsParallelWorker())
		stmtStartTimestamp = GetCurrentTimestamp();
	else
		Assert(stmtStartTimestamp != 0);
}

/*
 *	GetCurrentTransactionNestLevel
 *
 * 注意：当不在任何事务中时返回零，在顶层事务中时返回一，依此类推。
 *
 */
int
GetCurrentTransactionNestLevel(void)
{
	TransactionState s = CurrentTransactionState;

	return s->nestingLevel;
}


/*
 *	TransactionIdIsCurrentTransactionId
 */
bool
TransactionIdIsCurrentTransactionId(TransactionId xid)
{
	TransactionState s;

	/*
	 * 我们总是说 BootstrapTransactionId "不是我的事务 ID"，
	 * 即使它是（即在引导期间）。结合 transam.c 总是将
	 * BootstrapTransactionId 视为已提交的事实，这导致
	 * heapam_visibility.c 例程将所有元组视为已提交，
	 * 这正是我们在引导期间所需要的。（引导模式只插入元组，
	 * 从不更新或删除它们，所以所有元组都可以立即被认为是好的。）
	 *
	 * 同样，InvalidTransactionId 和 FrozenTransactionId 肯定
	 * 不是我的事务 ID，所以对于任何非正常 XID，
	 * 我们可以直接返回 "false"。
 *
 */
	if (!TransactionIdIsNormal(xid))
		return false;

	if (TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return true;

	/*
	 * 在并行工作进程中，我们必须视为当前的 XIDs 存储在
	 * ParallelCurrentXids 而不是事务状态栈中。请注意，
	 * 此数组中的 XID 是按数值排序的，而不是按
	 * transactionIdPrecedes 顺序排序的。
	 */
	if (nParallelCurrentXids > 0)
	{
		int			low,
					high;

		low = 0;
		high = nParallelCurrentXids - 1;
		while (low <= high)
		{
			int			middle;
			TransactionId probe;

			middle = low + (high - low) / 2;
			probe = ParallelCurrentXids[middle];
			if (probe == xid)
				return true;
			else if (probe < xid)
				low = middle + 1;
			else
				high = middle - 1;
		}
		return false;
	}

	/*
	 * 我们将对当前子事务的 Xid、其任何子提交的子事务、
	 * 其任何父事务或其任何先前子提交的子事务返回 true。
	 * 但是，正在中止的事务不再是 "当前的"，即使它可能
	 * 仍在状态栈上有条目。
 *
 */
	for (s = CurrentTransactionState; s != NULL; s = s->parent)
	{
		int			low,
					high;

		if (s->state == TRANS_ABORT)
			continue;
		if (!FullTransactionIdIsValid(s->fullTransactionId))
			continue;			/* 它也不可能有任何子事务 XIDs */
		if (TransactionIdEquals(xid, XidFromFullTransactionId(s->fullTransactionId)))
			return true;
		/* 由于 childXids 数组是有序的，我们可以使用二分查找 */
		low = 0;
		high = s->nChildXids - 1;
		while (low <= high)
		{
			int			middle;
			TransactionId probe;

			middle = low + (high - low) / 2;
			probe = s->childXids[middle];
			if (TransactionIdEquals(probe, xid))
				return true;
			else if (TransactionIdPrecedes(probe, xid))
				low = middle + 1;
			else
				high = middle - 1;
		}
	}

	return false;
}

/*
 *	TransactionStartedDuringRecovery
 *
 * 如果当前事务在恢复仍在进行时启动，则返回 true。
 * 恢复可能此后已结束，因此 RecoveryInProgress() 可能已经返回 false。
 *
 */
bool
TransactionStartedDuringRecovery(void)
{
	return CurrentTransactionState->startedInRecovery;
}

/*
 *	EnterParallelMode
 */
void
EnterParallelMode(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parallelModeLevel >= 0);

	++s->parallelModeLevel;
}

/*
 *	ExitParallelMode
 */
void
ExitParallelMode(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parallelModeLevel > 0);
	Assert(s->parallelModeLevel > 1 || !ParallelContextActive());

	--s->parallelModeLevel;
}

/*
 *	IsInParallelMode
 *
 * 我们是否处于并行操作中，作为领导者或工作进程？
 * 检查此项以禁止更改预期在所有工作进程中匹配的后端本地状态的操作。
 * 单纯的缓存通常不需要这样的限制。以严格的压入/弹出方式修改的状态，
 * 如活动快照栈，通常是可以的。
 *
 */
bool
IsInParallelMode(void)
{
	return CurrentTransactionState->parallelModeLevel != 0;
}

/*
 *	CommandCounterIncrement
 */
void
CommandCounterIncrement(void)
{
	/*
	 * 如果命令计数器的当前值还没有被 "使用" 来标记元组，
	 * 我们就不需要递增它，因为不需要区分只读命令和其他命令。
	 * 这有助于推迟命令计数器溢出，并使无操作的 CommandCounterIncrement 操作代价低廉。
 *
 */
	if (currentCommandIdUsed)
	{
		/*
		 * 工作进程在每个并行操作开始时同步事务状态，
		 * 因此我们不能在此之后计入新命令。
 *
 */
		if (IsInParallelMode() || IsParallelWorker())
			elog(ERROR, "cannot start commands during a parallel operation");

		currentCommandId += 1;
		if (currentCommandId == InvalidCommandId)
		{
			currentCommandId -= 1;
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("cannot have more than 2^32-2 commands in a transaction")));
		}
		currentCommandIdUsed = false;

		/* 将新命令 ID 传播到静态快照中 */
		SnapshotSetCommandId(currentCommandId);

		/*
		 * 使刚完成的命令所做的任何目录更改在本地系统缓存中可见。
		 * 显然，我们在只读命令后不需要这样做。（但请参阅 inval.c 中的技巧，
		 * 以确保我们不会认为排队了无效消息的命令是只读的。）
 *
 */
		AtCCI_LocalCache();
	}
}

/*
 * ForceSyncCommit
 *
 * 允许命令强制当前顶层事务同步提交的接口例程。
 * 目前，两阶段提交不会持久化和恢复此变量。
 * 只要所有调用者使用 PreventInTransactionBlock()，该遗漏就没有后果。
 *
 */
void
ForceSyncCommit(void)
{
	forceSyncCommit = true;
}


/* ----------------------------------------------------------------
 *						StartTransaction 相关内容
 * ----------------------------------------------------------------
 */

/*
 *	AtStart_Cache
 */
static void
AtStart_Cache(void)
{
	AcceptInvalidationMessages();
}

/*
 *	AtStart_Memory
 */
static void
AtStart_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 如果这是第一次通过，为 AbortTransaction 创建一个私有上下文。
	 * 通过现在预留一些空间，我们可以使 AbortTransaction 免受内存不足情况的影响。
	 * 像 ErrorContext 一样，我们以较慢的增长速率和非零的最小大小设置它，
	 * 以便空间将立即被预留。
 *
 */
	if (TransactionAbortContext == NULL)
		TransactionAbortContext =
			AllocSetContextCreate(TopMemoryContext,
								  "TransactionAbortContext",
								  32 * 1024,
								  32 * 1024,
								  32 * 1024);

	/*
	 * 我们不应该已经有事务上下文了。
	 */
	Assert(TopTransactionContext == NULL);

	/*
	 * 为事务创建一个顶层上下文。
	 */
	TopTransactionContext =
		AllocSetContextCreate(TopMemoryContext,
							  "TopTransactionContext",
							  ALLOCSET_DEFAULT_SIZES);

	/*
	 * 在顶层事务中，CurTransactionContext 与 TopTransactionContext 相同。
 *
 */
	CurTransactionContext = TopTransactionContext;
	s->curTransactionContext = CurTransactionContext;

	/* 使 CurTransactionContext 成为活动上下文。 */
	MemoryContextSwitchTo(CurTransactionContext);
}

/*
 *	AtStart_ResourceOwner
 */
static void
AtStart_ResourceOwner(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 我们不应该已经有事务资源所有者了。
	 */
	Assert(TopTransactionResourceOwner == NULL);

	/*
	 * 为事务创建一个顶层资源所有者。
	 */
	s->curTransactionOwner = ResourceOwnerCreate(NULL, "TopTransaction");

	TopTransactionResourceOwner = s->curTransactionOwner;
	CurTransactionResourceOwner = s->curTransactionOwner;
	CurrentResourceOwner = s->curTransactionOwner;
}

/* ----------------------------------------------------------------
 *						StartSubTransaction 相关内容
 * ----------------------------------------------------------------
 */

/*
 * AtSubStart_Memory
 */
static void
AtSubStart_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(CurTransactionContext != NULL);

	/*
	 * 创建一个 CurTransactionContext，它将用于保存子事务提交后存活
	 * 但在子事务中止时消失的数据。我们将其作为直接父级的
	 * CurTransactionContext 的子上下文。
	 */
	CurTransactionContext = AllocSetContextCreate(CurTransactionContext,
												  "CurTransactionContext",
												  ALLOCSET_DEFAULT_SIZES);
	s->curTransactionContext = CurTransactionContext;

	/* 使 CurTransactionContext 成为活动上下文。 */
	MemoryContextSwitchTo(CurTransactionContext);
}

/*
 * AtSubStart_ResourceOwner
 */
static void
AtSubStart_ResourceOwner(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parent != NULL);

	/*
	 * 为子事务创建一个资源所有者。我们将其作为直接父级的资源所有者的子级。
 *
 */
	s->curTransactionOwner =
		ResourceOwnerCreate(s->parent->curTransactionOwner,
							"SubTransaction");

	CurTransactionResourceOwner = s->curTransactionOwner;
	CurrentResourceOwner = s->curTransactionOwner;
}

/* ----------------------------------------------------------------
 *						CommitTransaction 相关内容
 * ----------------------------------------------------------------
 */

/*
 *	RecordTransactionCommit
 *
 * 返回事务及其子事务中最新的 XID，如果事务没有 XID 则返回 InvalidTransactionId。
 * （我们在这里计算它只是因为这样更方便。）
 *
 * 如果您修改此函数，请参阅 RecordTransactionCommitPrepared。
 */
static TransactionId
RecordTransactionCommit(void)
{
	TransactionId xid = GetTopTransactionIdIfAny();
	bool		markXidCommitted = TransactionIdIsValid(xid);
	TransactionId latestXid = InvalidTransactionId;
	int			nrels;
	RelFileLocator *rels;
	int			nchildren;
	TransactionId *children;
	int			ndroppedstats = 0;
	xl_xact_stats_item *droppedstats = NULL;
	int			nmsgs = 0;
	SharedInvalidationMessage *invalMessages = NULL;
	bool		RelcacheInitFileInval = false;
	bool		wrote_xlog;

	/*
	 * 为正在进行的事务的逻辑解码记录待处理的无效化消息。
	 * 通常对于 DDLs，我们在每个命令结束时记录这些，
	 * 但是，对于某些我们直接更新系统表而没有事务块的情况，
	 * 无效化消息直到现在才被记录。
 *
 */
	if (XLogLogicalInfoActive())
		LogLogicalInvalidations();

	/* 获取提交记录所需的数据 */
	nrels = smgrGetPendingDeletes(true, &rels);
	nchildren = xactGetCommittedChildren(&children);
	ndroppedstats = pgstat_get_transactional_drops(true, &droppedstats);
	if (XLogStandbyInfoActive())
		nmsgs = xactGetCommittedInvalidationMessages(&invalMessages,
													 &RelcacheInitFileInval);
	wrote_xlog = (XactLastRecEnd != 0);

	/*
	 * 如果我们还没有被分配 XID，我们既不能也不想写入 COMMIT 记录。
 *
 */
	if (!markXidCommitted)
	{
		/*
		 * 我们期望每个 RelationDropStorage 后面都有一个目录更新，
		 * 因此会有 XID 分配，所以我们不应该在有任何待删除的情况下到达这里。
		 * 删除统计信息也是如此。
		 *
		 * 使用真正的测试而不仅仅是 Assert 来检查这一点，因为它有点脆弱。
 *
 */
		if (nrels != 0 || ndroppedstats != 0)
			elog(ERROR, "cannot commit a transaction that deleted files but has no xid");

		/* 也不能有子事务 XIDs；AssignTransactionId 强制执行这一点 */
		Assert(nchildren == 0);

		/*
		 * 没有分配 XID 的事务可以包含无效化消息（例如显式的 relcache 无效化
		 * 或原地更新的 catcache 无效化）；备库需要处理这些。
		 * 我们不能在没有 XID 的情况下发出提交记录，我们也不想强制分配 XID，
		 * 因为那对于例如 vacuum 会有问题。因此我们为无效化发出专门的记录。
		 * 我们不想在发出提交记录的情况下使用它，所以它们与提交同步发生
		 * （除了不想发出更多 WAL 记录）。
 *
 *
 *
 */
		if (nmsgs != 0)
		{
			LogStandbyInvalidations(nmsgs, invalMessages,
									RelcacheInitFileInval);
			wrote_xlog = true;	/* 不是严格必要的 */
		}

		/*
		 * 如果我们没有创建 XLOG 条目，我们在这里就完成了；否则
		 * 我们应该像提交记录一样触发刷新这些条目。这主要发生在
		 * HOT 修剪等操作中；我们希望这些在适当的时候刷新到磁盘。
 *
 */
		if (!wrote_xlog)
			goto cleanup;
	}
	else
	{
		bool		replorigin;

		/*
		 * 我们是否正在使用复制源功能？或者换句话说，我们是否正在重放远程操作？
 *
 */
		replorigin = (replorigin_session_origin != InvalidRepOriginId &&
					  replorigin_session_origin != DoNotReplicateId);

		/*
		 * 将我们自己标记为在"提交临界区"内。这会强制任何并发检查点等待，
		 * 直到我们更新了 pg_xact。如果没有这个，检查点有可能在 XLOG 记录之后
		 * 设置 REDO 但未能将 pg_xact 更新刷新到磁盘，导致如果系统稍后崩溃，
		 * 事务提交会丢失。
		 *
		 * 注意：我们可以（但不麻烦）在 RecordTransactionAbort 中设置此标志。
		 * 那是因为事务中止的丢失是非关键的；假设它中止了就是了。
		 *
		 * 在不持有 ProcArrayLock 的情况下更改我们自己后端的 delayChkptFlags
		 * 标志是安全的，因为只有我们在修改它。这使得检查点确定哪些事务
		 * 正在延迟检查点有点模糊，但这无关紧要。
 *
 *
 *
 *
 */
		Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
		START_CRIT_SECTION();
		MyProc->delayChkptFlags |= DELAY_CHKPT_START;

		/*
		 * 插入提交 XLOG 记录。
		 */
		XactLogCommitRecord(GetCurrentTransactionStopTimestamp(),
							nchildren, children, nrels, rels,
							ndroppedstats, droppedstats,
							nmsgs, invalMessages,
							RelcacheInitFileInval,
							MyXactFlags,
							InvalidTransactionId, NULL /* 普通提交 */ );

		if (replorigin)
			/* 为此复制源向前移动 LSNs */
			replorigin_session_advance(replorigin_session_origin_lsn,
									   XactLastRecEnd);

		/*
		 * 记录提交时间戳。如果没有复制源，该值来自普通提交时间戳；
		 * 否则，时间戳已由复制在 replorigin_session_origin_timestamp 中设置。
		 *
		 * 我们不需要在这里 WAL 记录任何内容，因为上面写入的提交记录
		 * 已经包含了数据。
 *
 *
 */

		if (!replorigin || replorigin_session_origin_timestamp == 0)
			replorigin_session_origin_timestamp = GetCurrentTransactionStopTimestamp();

		TransactionTreeSetCommitTsData(xid, nchildren, children,
									   replorigin_session_origin_timestamp,
									   replorigin_session_origin);
	}

	/*
	 * 检查我们是否要异步提交。如果 synchronous_commit=off，或者当前事务
	 * 没有执行任何 WAL 记录操作或没有分配 XID，我们可以允许 XLOG 刷新
	 * 异步发生。事务可能最终没有写入任何 WAL，即使它有 XID，
	 * 如果它只写入临时表和/或未记录表。它可能在没有 XID 的情况下
	 * 写入了 WAL，如果它进行了 HOT 修剪。在崩溃的情况下，
	 * 这样的事务丢失将是无关紧要的；临时表无论如何都会丢失，
	 * 未记录表将被截断，HOT 修剪稍后会再次完成。（鉴于上述情况，
	 * 您可能认为在这种情况下根本不需要发出 XLOG 记录，
	 * 但我们目前不尝试这样做。这至少在热备模式下会导致问题，
	 * 因为 KnownAssignedXids 机制需要跟踪每个 XID 分配。
	 * 可能只有在 wal_level < replica 时跳过它才是可以的，但目前我们不这样做。）
	 *
	 * 但是，如果我们正在清理任何非临时关系或提交任何想要强制同步提交的命令，
	 * 那么我们必须立即刷新 XLOG。（如果有任何非临时表要删除，
	 * 我们不允许异步提交，因为我们可能会在 COMMIT 记录刷新到磁盘之前
	 * 删除文件。如果要删除的表都是临时表，我们确实允许异步提交，
	 * 因为如果我们崩溃，它们无论如何都会丢失。）
 *
 *
 *
 *
 *
 *
 */
	if ((wrote_xlog && markXidCommitted &&
		 synchronous_commit > SYNCHRONOUS_COMMIT_OFF) ||
		forceSyncCommit || nrels > 0)
	{
		XLogFlush(XactLastRecEnd);

		/*
		 * 现在我们可以更新 CLOG，如果我们在上面写入了 COMMIT 记录
		 */
		if (markXidCommitted)
			TransactionIdCommitTree(xid, nchildren, children);
	}
	else
	{
		/*
		 * 异步提交情况：
		 *
		 * 这使得在 postmaster 崩溃的情况下可能导致已提交事务丢失，
		 * 因为 WAL 缓冲区未写入。理想情况下，我们可以在没有 fsync 的情况下
		 * 发出 WAL 写入，但某些 wal_sync_methods 不允许单独的写入/fsync。
		 *
		 * 报告最新的异步提交 LSN，以便 WAL 写入器知道刷新此提交。
 *
 *
 */
		XLogSetAsyncXactLSN(XactLastRecEnd);

		/*
		 * 我们不能立即更新 CLOG，因为我们没有刷新 XLOG。
		 * 相反，我们存储 XLOG 必须刷新到的 LSN，然后才能更新 CLOG。
 *
 */
		if (markXidCommitted)
			TransactionIdAsyncCommitTree(xid, nchildren, children, XactLastRecEnd);
	}

	/*
	 * 如果我们进入了提交临界区，现在离开它，让检查点继续进行。
 *
 */
	if (markXidCommitted)
	{
		MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
		END_CRIT_SECTION();
	}

	/* 在我们有子事务 XIDs 方便时计算 latestXid */
	latestXid = TransactionIdLatest(xid, nchildren, children);

	/*
	 * 如果需要，等待同步复制。类似于上面关于异步提交的决定，
	 * 我们只想在这个后端分配了 XID 并写入 WAL 时等待。
	 * 如果分配 XID 是因为临时/未记录表或 HOT 修剪，则不需要等待。
	 *
	 * 注意，在这个阶段我们已经标记了 clog，但在 procarray 中
	 * 仍然显示为运行中，并继续持有锁。
 *
 */
	if (wrote_xlog && markXidCommitted)
		SyncRepWaitForLSN(XactLastRecEnd, true);

	/* 记住最后一条提交记录的结束位置 */
	XactLastCommitEnd = XactLastRecEnd;

	/* 重置 XactLastRecEnd 直到下一个事务写入内容 */
	XactLastRecEnd = 0;
cleanup:
	/* 清理本地数据 */
	if (rels)
		pfree(rels);
	if (ndroppedstats)
		pfree(droppedstats);

	return latestXid;
}


/*
 *	AtCCI_LocalCache
 */
static void
AtCCI_LocalCache(void)
{
	/*
	 * 使任何待处理的关系映射更改可见。我们必须在处理本地 sinval 消息之前
	 * 执行此操作，以便在处理 relcache 无效化时映射更改会反映到 relcache 中。
 *
 */
	AtCCI_RelationMap();

	/*
	 * 使目录更改对下一个命令可见。
	 */
	CommandEndInvalidationMessages();
}

/*
 *	AtCommit_Memory
 */
static void
AtCommit_Memory(void)
{
	/*
	 * 现在我们已经"退出"了事务，让系统在顶层内存上下文中分配东西，
	 * 而不是每个事务的上下文。
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * 释放所有事务本地内存。
	 */
	Assert(TopTransactionContext != NULL);
	MemoryContextDelete(TopTransactionContext);
	TopTransactionContext = NULL;
	CurTransactionContext = NULL;
	CurrentTransactionState->curTransactionContext = NULL;
}

/* ----------------------------------------------------------------
 *						CommitSubTransaction 相关
 * ----------------------------------------------------------------
 */

/*
 * AtSubCommit_Memory
 */
static void
AtSubCommit_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parent != NULL);

	/* 返回到父事务级别的内存上下文。 */
	CurTransactionContext = s->parent->curTransactionContext;
	MemoryContextSwitchTo(CurTransactionContext);

	/*
	 * 通常我们不能丢弃子事务的 CurTransactionContext，
	 * 因为其中包含的数据在上级提交时将需要。然而，如果
	 * 其中实际上没有任何内容，我们可以丢弃它。这避免了
	 * 在"平凡"子事务的常见情况下出现小的内存泄漏。
	 */
	if (MemoryContextIsEmpty(s->curTransactionContext))
	{
		MemoryContextDelete(s->curTransactionContext);
		s->curTransactionContext = NULL;
	}
}

/*
 * AtSubCommit_childXids
 *
 * 将我自己的 XIDs 和我的子事务 XID 作为已提交的子事务传递给我的父事务。
 */
static void
AtSubCommit_childXids(void)
{
	TransactionState s = CurrentTransactionState;
	int			new_nChildXids;

	Assert(s->parent != NULL);

	/*
	 * 父级的 childXids 数组将需要容纳我的 XIDs 和我所有的 childXids，
	 * 以及已经存在的 XID。
	 */
	new_nChildXids = s->parent->nChildXids + s->nChildXids + 1;

	/* 如有必要，分配或扩大父级数组 */
	if (s->parent->maxChildXids < new_nChildXids)
	{
		int			new_maxChildXids;
		TransactionId *new_childXids;

		/*
		 * 将其设为当前所需大小的 2 倍，以避免反复扩大。
		 * 但我们不能超过 MaxAllocSize。（后者的限制确保我们不需要担心
		 * 这里或 new_nChildXids 计算中的整数溢出。）
 *
 */
		new_maxChildXids = Min(new_nChildXids * 2,
							   (int) (MaxAllocSize / sizeof(TransactionId)));

		if (new_maxChildXids < new_nChildXids)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("maximum number of committed subtransactions (%d) exceeded",
							(int) (MaxAllocSize / sizeof(TransactionId)))));

		/*
		 * 我们将子事务 XIDs 数组保存在 TopTransactionContext 中；
		 * 这避免了为可能只是几字节的孙事务 XID 设置子事务上下文。
 *
 */
		if (s->parent->childXids == NULL)
			new_childXids =
				MemoryContextAlloc(TopTransactionContext,
								   new_maxChildXids * sizeof(TransactionId));
		else
			new_childXids = repalloc(s->parent->childXids,
									 new_maxChildXids * sizeof(TransactionId));

		s->parent->childXids = new_childXids;
		s->parent->maxChildXids = new_maxChildXids;
	}

	/*
	 * 将我所有的 XIDs 复制到父级数组中。
	 *
	 * 注意：我们依赖于子事务的 XID 总是紧跟在其父事务之后的 XID 这一事实。
	 * 通过在子事务的子事务 XID 之前复制此子事务的 XID，
	 * 我们确保数组保持有序。同样，数组中已有的所有 XID
	 * 都属于在我们之前启动和子提交的子事务，因此它们的 XID 必须在我们的之前。
 *
 */
	s->parent->childXids[s->parent->nChildXids] = XidFromFullTransactionId(s->fullTransactionId);

	if (s->nChildXids > 0)
		memcpy(&s->parent->childXids[s->parent->nChildXids + 1],
			   s->childXids,
			   s->nChildXids * sizeof(TransactionId));

	s->parent->nChildXids = new_nChildXids;

	/* 释放子事务数组以避免泄漏 */
	if (s->childXids != NULL)
		pfree(s->childXids);
	/* 我们必须重置这些以避免在提交稍后失败时双重释放 */
	s->childXids = NULL;
	s->nChildXids = 0;
	s->maxChildXids = 0;
}

/* ----------------------------------------------------------------
 *						AbortTransaction 相关内容
 * ----------------------------------------------------------------
 */

/*
 *	RecordTransactionAbort
 *
 * 返回事务及其子事务中最新的 XID，如果事务没有 XID 则返回 InvalidTransactionId。
 * （我们在这里计算它只是因为这样更方便。）
 */
static TransactionId
RecordTransactionAbort(bool isSubXact)
{
	TransactionId xid = GetCurrentTransactionIdIfAny();
	TransactionId latestXid;
	int			nrels;
	RelFileLocator *rels;
	int			ndroppedstats = 0;
	xl_xact_stats_item *droppedstats = NULL;
	int			nchildren;
	TransactionId *children;
	TimestampTz xact_time;
	bool		replorigin;

	/*
	 * 如果我们还没有被分配 XIDs，没有人会关心我们是否中止了。
	 * 因此，在这种情况下我们就完成了。即使我们要删除的关系也没关系
	 * （注意此例程不负责实际删除它们）。我们也不能有任何子事务 XID。
 *
 */
	if (!TransactionIdIsValid(xid))
	{
		/* 重置 XactLastRecEnd 直到下一个事务写入内容 */
		if (!isSubXact)
			XactLastRecEnd = 0;
		return InvalidTransactionId;
	}

	/*
	 * 我们有一个有效的 XID，所以我们应该为其写入一条 ABORT 记录。
	 *
	 * 我们不在这里将 XLOG 刷新到磁盘，因为崩溃后的默认假设
	 * 就是我们中止了。出于同样的原因，我们不需要担心与检查点启动的互锁。
 *
 */

	/*
	 * 检查我们没有在 RecordTransactionCommit 中途中止。
	 */
	if (TransactionIdDidCommit(xid))
		elog(PANIC, "cannot abort transaction %u, it was already committed",
			 xid);

	/*
	 * 我们是否正在使用复制源功能？或者换句话说，我们是否正在重放远程操作？
 *
 */
	replorigin = (replorigin_session_origin != InvalidRepOriginId &&
				  replorigin_session_origin != DoNotReplicateId);

	/* 获取中止记录所需的数据 */
	nrels = smgrGetPendingDeletes(false, &rels);
	nchildren = xactGetCommittedChildren(&children);
	ndroppedstats = pgstat_get_transactional_drops(false, &droppedstats);

	/* XXX 我们这里真的需要临界区吗？ */
	START_CRIT_SECTION();

	/* 写入 ABORT 记录 */
	if (isSubXact)
		xact_time = GetCurrentTimestamp();
	else
	{
		xact_time = GetCurrentTransactionStopTimestamp();
	}

	XactLogAbortRecord(xact_time,
					   nchildren, children,
					   nrels, rels,
					   ndroppedstats, droppedstats,
					   MyXactFlags, InvalidTransactionId,
					   NULL);

	if (replorigin)
		/* 为此复制源向前移动 LSNs */
		replorigin_session_advance(replorigin_session_origin_lsn,
								   XactLastRecEnd);

	/*
	 * 报告最新的异步中止 LSN，以便 WAL 写入器知道刷新此中止。
	 * 延迟这样做没有什么好处，因为 WALWriter 也可以在可以的时候做这件事。
	 * 这对于流复制很重要，因为如果我们不定期刷新 WAL，
	 * 我们会发现大型中止会在中止后提交时给我们留下很长的积压，
	 * 增加了在那时发生问题时的数据丢失窗口。
 *
 *
 */
	if (!isSubXact)
		XLogSetAsyncXactLSN(XactLastRecEnd);

	/*
	 * 在 clog 中将事务标记为已中止。这不是绝对必要的，
	 * 但我们可以趁在这里做；此外，在子事务情况下这很有帮助，
	 * 因为 XactLockTableWait 利用它来避免等待已经中止的子事务。
	 * 在没有将 ABORT 记录刷新到磁盘的情况下这样做是可以的，
	 * 因为在崩溃的情况下我们会被假设为已经中止了。
 *
 */
	TransactionIdAbortTree(xid, nchildren, children);

	END_CRIT_SECTION();

	/* 在我们有子事务 XIDs 方便时计算 latestXid */
	latestXid = TransactionIdLatest(xid, nchildren, children);

	/*
	 * 如果我们正在中止一个子事务，我们可以立即从 PGPROC 的运行子事务 XIDs 缓存中
	 * 移除失败的 XID。我们在这里为子事务做这件事，因为我们已经有了子事务 XID 数组。
	 * 对于主事务，等效操作在此函数返回后立即发生。
 *
 */
	if (isSubXact)
		XidCacheRemoveRunningXids(xid, nchildren, children, latestXid);

	/* 重置 XactLastRecEnd 直到下一个事务写入内容 */
	if (!isSubXact)
		XactLastRecEnd = 0;

	/* 清理本地数据 */
	if (rels)
		pfree(rels);
	if (ndroppedstats)
		pfree(droppedstats);

	return latestXid;
}

/*
 *	AtAbort_Memory
 */
static void
AtAbort_Memory(void)
{
	/*
	 * 切换到 TransactionAbortContext，即使其他地方没有空闲空间，
	 * 它也应该有一些。我们将在这个上下文中工作，直到完成清理。
	 *
	 * 极有可能在我们还没能创建 TransactionAbortContext 时就到达这里；
	 * 如果是这样，使用 TopMemoryContext。
 *
 */
	if (TransactionAbortContext != NULL)
		MemoryContextSwitchTo(TransactionAbortContext);
	else
		MemoryContextSwitchTo(TopMemoryContext);
}

/*
 * AtSubAbort_Memory
 */
static void
AtSubAbort_Memory(void)
{
	Assert(TransactionAbortContext != NULL);

	MemoryContextSwitchTo(TransactionAbortContext);
}


/*
 *	AtAbort_ResourceOwner
 */
static void
AtAbort_ResourceOwner(void)
{
	/*
	 * 确保我们有一个有效的 ResourceOwner，如果可能的话（否则它将是 NULL，这也是可以的）
 *
 */
	CurrentResourceOwner = TopTransactionResourceOwner;
}

/*
 * AtSubAbort_ResourceOwner
 */
static void
AtSubAbort_ResourceOwner(void)
{
	TransactionState s = CurrentTransactionState;

	/* 确保我们有一个有效的 ResourceOwner */
	CurrentResourceOwner = s->curTransactionOwner;
}


/*
 * AtSubAbort_childXids
 */
static void
AtSubAbort_childXids(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 我们将子事务 XID 数组保存在 TopTransactionContext 中（参见 AtSubCommit_childXids）。
	 * 这意味着我们最好在中止时显式释放数组以避免泄漏。
 *
 */
	if (s->childXids != NULL)
		pfree(s->childXids);
	s->childXids = NULL;
	s->nChildXids = 0;
	s->maxChildXids = 0;

	/*
	 * 我们可以在这里修剪 unreportedXids 数组。但我们不麻烦。
	 * 那可能会减少 XLOG_XACT_ASSIGNMENT 记录的数量，
	 * 但它可能会在更常见的路径中引入更多的 CPU 时间，
	 * 所以我们选择不这样做。
	 */
}

/* ----------------------------------------------------------------
 *						CleanupTransaction 相关内容
 * ----------------------------------------------------------------
 */

/*
 *	AtCleanup_Memory
 */
static void
AtCleanup_Memory(void)
{
	Assert(CurrentTransactionState->parent == NULL);

	/*
	 * 现在我们已经"退出"了事务，让系统在顶层内存上下文中分配东西，
	 * 而不是每个事务的上下文。
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * 为下次清除特殊的中止上下文。
	 */
	if (TransactionAbortContext != NULL)
		MemoryContextResetAndDeleteChildren(TransactionAbortContext);

	/*
	 * 释放所有事务本地内存。
	 */
	if (TopTransactionContext != NULL)
		MemoryContextDelete(TopTransactionContext);
	TopTransactionContext = NULL;
	CurTransactionContext = NULL;
	CurrentTransactionState->curTransactionContext = NULL;
}


/* ----------------------------------------------------------------
 *						CleanupSubTransaction 相关内容
 * ----------------------------------------------------------------
 */

/*
 * AtSubCleanup_Memory
 */
static void
AtSubCleanup_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parent != NULL);

	/* 确保我们不在即将被删除的上下文中 */
	MemoryContextSwitchTo(s->parent->curTransactionContext);
	CurTransactionContext = s->parent->curTransactionContext;

	/*
	 * 为下次清除特殊的中止上下文。
	 */
	if (TransactionAbortContext != NULL)
		MemoryContextResetAndDeleteChildren(TransactionAbortContext);

	/*
	 * 删除子事务本地内存上下文。其 CurTransactionContext 也可以删除
	 * （注意这也会杀死子事务的任何子级的 CurTransactionContexts）。
 *
 */
	if (s->curTransactionContext)
		MemoryContextDelete(s->curTransactionContext);
	s->curTransactionContext = NULL;
}

/* ----------------------------------------------------------------
 *						接口例程
 * ----------------------------------------------------------------
 */

/*
 *	StartTransaction
 */
static void
StartTransaction(void)
{
	TransactionState s;
	VirtualTransactionId vxid;

	/*
	 * 让我们确保状态栈是空的
	 */
	s = &TopTransactionStateData;
	CurrentTransactionState = s;

	Assert(!FullTransactionIdIsValid(XactTopFullTransactionId));

	/* 检查当前事务状态 */
	Assert(s->state == TRANS_DEFAULT);

	/*
	 * 在启动处理期间适当设置当前事务状态信息。
	 * 请注意，一旦事务状态切换，此进程就不能失败，
	 * 直到下面获取用户 ID 和安全上下文标志。
 *
 */
	s->state = TRANS_START;
	s->fullTransactionId = InvalidFullTransactionId;	/* 直到分配 */

	/* 确定是否在此事务中记录语句 */
	xact_is_sampled = log_xact_sample_rate != 0 &&
		(log_xact_sample_rate == 1 ||
		 pg_prng_double(&pg_global_prng_state) <= log_xact_sample_rate);

	/*
	 * 初始化当前事务状态字段
	 *
	 * 注意：prevXactReadOnly 在最外层不使用
	 */
	s->nestingLevel = 1;
	s->gucNestLevel = 1;
	s->childXids = NULL;
	s->nChildXids = 0;
	s->maxChildXids = 0;

	/*
	 * 一旦获取了当前用户 ID 和安全上下文标志，
	 * 即使事务启动失败，两者也会被正确重置。
	 */
	GetUserIdAndSecContext(&s->prevUser, &s->prevSecContext);

	/* SecurityRestrictionContext 在事务外不应被设置 */
	Assert(s->prevSecContext == 0);

	/*
	 * 确保我们已重置事务状态变量
	 *
	 * 如果恢复仍在进行中，将此事务标记为只读。
	 * 我们在 XLogInsert 和其他地方有较低级别的防护来阻止我们
	 * 在恢复期间修改数据，但这向用户提供了事务是只读的正常指示。
 *
 */
	if (RecoveryInProgress())
	{
		s->startedInRecovery = true;
		XactReadOnly = true;
	}
	else
	{
		s->startedInRecovery = false;
		XactReadOnly = DefaultXactReadOnly;
	}
	XactDeferrable = DefaultXactDeferrable;
	XactIsoLevel = DefaultXactIsoLevel;
	forceSyncCommit = false;
	MyXactFlags = 0;

	/*
	 * 重新初始化事务内计数器
	 */
	s->subTransactionId = TopSubTransactionId;
	currentSubTransactionId = TopSubTransactionId;
	currentCommandId = FirstCommandId;
	currentCommandIdUsed = false;

	/*
	 * 初始化已报告的 xid 计账
	 */
	nUnreportedXids = 0;
	s->didLogXid = false;

	/*
	 * 必须先初始化资源管理相关内容
	 */
	AtStart_Memory();
	AtStart_ResourceOwner();

	/*
	 * 分配一个新的 LocalTransactionId，并将其与 backendId 结合
	 * 形成虚拟事务 ID。
	 */
	vxid.backendId = MyBackendId;
	vxid.localTransactionId = GetNextLocalTransactionId();

	/*
	 * 在我们在进程数组中宣布虚拟事务 ID 之前锁定它
	 */
	VirtualXactLockTableInsert(vxid);

	/*
	 * 在进程数组中公布它。我们假设 localTransactionId 的分配是原子的，
	 * 并且 backendId 应该已经设置了。
	 */
	Assert(MyProc->backendId == vxid.backendId);
	MyProc->lxid = vxid.localTransactionId;

	TRACE_POSTGRESQL_TRANSACTION_START(vxid.localTransactionId);

	/*
 * 设置 transaction_timestamp()（也称为 now()）。通常，我们希望它
 * 与第一个命令的 statement_timestamp() 相同，所以不要执行
 * 新的 GetCurrentTimestamp() 调用（这也会很昂贵）。
 * 但对于在存储过程内部启动的事务（即非原子 SPI 上下文），
 * 我们确实需要推进时间戳。同样，在并行工作进程中，
 * 时间戳应该已经通过调用 SetParallelStartTimestamps() 提供了。
 *
 */
	if (!IsParallelWorker())
	{
		if (!SPI_inside_nonatomic_context())
			xactStartTimestamp = stmtStartTimestamp;
		else
			xactStartTimestamp = GetCurrentTimestamp();
	}
	else
		Assert(xactStartTimestamp != 0);
	pgstat_report_xact_timestamp(xactStartTimestamp);
	/* 将 xactStopTimestamp 标记为未设置。 */
	xactStopTimestamp = 0;

	/*
	 * 为新事务初始化其他子系统
	 */
	AtStart_GUC();
	AtStart_Cache();
	AfterTriggerBeginXact();

	/*
	 * 完成启动处理，将当前事务状态设置为"进行中"
 *
 */
	s->state = TRANS_INPROGRESS;

	ShowTransactionState("StartTransaction");
}


/*
 *	CommitTransaction
 *
 * 注意：如果您更改此例程，最好也看看 PrepareTransaction！
 */
static void
CommitTransaction(void)
{
	TransactionState s = CurrentTransactionState;
	TransactionId latestXid;
	bool		is_parallel_worker;

	is_parallel_worker = (s->blockState == TBLOCK_PARALLEL_INPROGRESS);

	/* 在并行工作进程提交期间强制执行并行模式限制。 */
	if (is_parallel_worker)
		EnterParallelMode();

	ShowTransactionState("CommitTransaction");

	/*
	 * 检查当前事务状态
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "CommitTransaction while in %s state",
			 TransStateAsString(s->state));
	Assert(s->parent == NULL);

	/*
	 * 执行涉及调用用户定义代码（如触发器）的提交前处理。
	 * SECURITY_RESTRICTED_OPERATION 上下文不得排队一个会在这里运行的操作，
	 * 因为那会绕过沙箱。由于关闭游标可能排队触发器动作，触发器可能打开游标等，
	 * 我们必须持续循环直到没有事情可做。
 *
 */
	for (;;)
	{
		/*
		 * 触发所有当前待处理的延迟触发器。
		 */
		AfterTriggerFireDeferred();

		/*
		 * 关闭打开的 portal（将可保持的转换为静态 portal）。
		 * 如果没有任何 portal，我们就完成了……否则循环回来检查
		 * 它们是否排队了延迟触发器。如此反复。
		 */
		if (!PreCommit_Portals(false))
			break;
	}

	/*
	 * 剩余的操作不能调用任何用户定义的代码，因此开始关闭事务内服务是安全的。
	 * 但请注意，这些大多数仍然可能抛出错误，这将使我们切换到事务中止路径。
 *
 *
 */

	CallXactCallbacks(is_parallel_worker ? XACT_EVENT_PARALLEL_PRE_COMMIT
					  : XACT_EVENT_PRE_COMMIT);

	/* 如果我们可能有并行工作进程，现在清理它们。 */
	if (IsInParallelMode())
		AtEOXact_Parallel(true);

	/* 关闭延迟触发器管理器 */
	AfterTriggerEndXact(true);

	/*
	 * 让 ON COMMIT 管理执行其操作（必须在关闭游标之后发生，以避免悬空引用问题）
 *
 */
	PreCommit_on_commit_actions();

	/*
	 * 同步在此事务期间创建且未进行 WAL 日志记录的文件。
	 * 这必须在 AtEOXact_RelationMap() 之前发生，以便我们在崩溃后
	 * 不会看到已提交但损坏的文件。
	 */
	smgrDoPendingSyncs(true, is_parallel_worker);

	/* 在较低级别清理之前关闭大对象 */
	AtEOXact_LargeObject(true);

	/*
	 * 将 NOTIFY 命令发送的通知插入队列。这应该在提交前序列中较晚进行，
	 * 以最小化持有通知插入锁的时间。但是，这可能导致创建快照，
	 * 所以我们必须在可序列化清理之前执行它。
 *
 */
	PreCommit_Notify();

	/*
	 * 为谓词锁定目的将可序列化事务标记为完成。这应该尽可能晚地进行，
	 * 但仍然允许为提交时发现的失败模式引发错误。
	 * 然而，这在并行工作进程中不合适，因为我们没有提交领导者的事务，
	 * 其可序列化状态将继续存在。
 *
 */
	if (!is_parallel_worker)
		PreCommit_CheckForSerializationFailure();

	/* 在清理时阻止取消/死亡中断 */
	HOLD_INTERRUPTS();

	/* 提交关系映射更新 --- 尽可能晚地执行此操作 */
	AtEOXact_RelationMap(true, is_parallel_worker);

	/*
	 * 在提交处理期间适当设置当前事务状态信息
 *
 */
	s->state = TRANS_COMMIT;
	s->parallelModeLevel = 0;

	if (!is_parallel_worker)
	{
		/*
		 * 我们需要在 pg_xact 中将我们的 XIDs 标记为已提交。
		 * 这是我们持久提交的地方。
		 */
		latestXid = RecordTransactionCommit();
	}
	else
	{
		/*
		 * 我们不能将我们的 XID 标记为已提交；并行领导者负责此事。
 *
 */
		latestXid = InvalidTransactionId;

		/*
		 * 确保领导者将在提交之前知道我们写入的任何 WAL。
 *
 */
		ParallelWorkerReportLastRecEnd(XactLastRecEnd);
	}

	TRACE_POSTGRESQL_TRANSACTION_COMMIT(MyProc->lxid);

	/*
	 * 让其他人知道我没有进行中的事务。请注意，这必须在释放我们持有的锁之前
	 * _完成_，并且必须在 RecordTransactionCommit 之后执行。
 * (_before_/_after_)
 */
	ProcArrayEndTransaction(MyProc, latestXid);

	/*
	 * 这都是提交后清理。请注意，如果在这里引发错误，
	 * 中止事务已经太晚了。这应该只是非关键的资源释放。
	 *
	 * 操作的顺序不是完全随机的。其思想是：
	 * 释放对其他后端可见的资源（例如文件、缓冲区固定）；
	 * 然后释放锁；然后释放后端本地资源。我们希望在
	 * 任何等待我们的后端看到我们的事务已完全清理的时刻释放锁。
	 *
	 * 可以与单个查询关联的资源由 ResourceOwner 机制处理。
	 * 这里的其他调用是用于后端范围的状态。
 *
 *
 *
 */

	CallXactCallbacks(is_parallel_worker ? XACT_EVENT_PARALLEL_COMMIT
					  : XACT_EVENT_COMMIT);

	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 true, true);

	/* 检查我们已释放所有缓冲区固定 */
	AtEOXact_Buffers(true);

	/* 清理关系缓存 */
	AtEOXact_RelationCache(true);

	/*
	 * 使目录更改对所有后端可见。这必须在删除 relcache 引用之后发生
	 * （参见 AtEOXact_RelationCache 的注释），但在释放锁之前
	 * （如果有人正在等待我们修改的关系上的锁，我们希望他们在开始使用该关系之前
	 * 知道目录更改）。
 *
 */
	AtEOXact_Inval(true);

	AtEOXact_MultiXact();

	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_LOCKS,
						 true, true);
	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 true, true);

	/*
	 * 同样，删除事务期间删除的文件最好在释放 relcache 和缓冲区固定之后进行。
	 * （这在提交期间不是严格必要的，因为这样的固定应该已经被释放了，
	 * 但这种顺序在中止期间绝对是关键的。）由于这可能需要很多秒，
	 * 也延迟到释放锁之后。其他后端将观察到相应的目录更改，
	 * 并且不会尝试访问受影响的文件。
 *
 *
 */
	smgrDoPendingDeletes(true);

	/*
	 * 向其他后端发送通知信号（并进行其他提交后 NOTIFY 清理）。
	 * 这必须等到我们的事务从其他后端的角度完全完成后才能发生。
 *
 */
	AtCommit_Notify();

	/*
	 * 此后的所有内容都应该是纯粹的后端内部清理。
 *
 */
	AtEOXact_GUC(true, 1);
	AtEOXact_SPI(true);
	AtEOXact_Enum();
	AtEOXact_on_commit_actions(true);
	AtEOXact_Namespace(true, is_parallel_worker);
	AtEOXact_SMgr();
	AtEOXact_Files(true);
	AtEOXact_ComboCid();
	AtEOXact_HashTables(true);
	AtEOXact_PgStat(true, is_parallel_worker);
	AtEOXact_Snapshot(true, false);
	AtEOXact_ApplyLauncher(true);
	AtEOXact_LogicalRepWorkers(true);
	pgstat_report_xact_timestamp(0);

	CurrentResourceOwner = NULL;
	ResourceOwnerDelete(TopTransactionResourceOwner);
	s->curTransactionOwner = NULL;
	CurTransactionResourceOwner = NULL;
	TopTransactionResourceOwner = NULL;

	AtCommit_Memory();

	s->fullTransactionId = InvalidFullTransactionId;
	s->subTransactionId = InvalidSubTransactionId;
	s->nestingLevel = 0;
	s->gucNestLevel = 0;
	s->childXids = NULL;
	s->nChildXids = 0;
	s->maxChildXids = 0;

	XactTopFullTransactionId = InvalidFullTransactionId;
	nParallelCurrentXids = 0;

	/*
	 * 完成提交处理，将当前事务状态设置回默认值
 *
 */
	s->state = TRANS_DEFAULT;

	RESUME_INTERRUPTS();
}


/*
 *	PrepareTransaction
 *
 * 注意：如果您更改此例程，最好也看看 CommitTransaction！
 */
static void
PrepareTransaction(void)
{
	TransactionState s = CurrentTransactionState;
	TransactionId xid = GetCurrentTransactionId();
	GlobalTransaction gxact;
	TimestampTz prepared_at;

	Assert(!IsInParallelMode());

	ShowTransactionState("PrepareTransaction");

	/*
	 * 检查当前事务状态
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "PrepareTransaction while in %s state",
			 TransStateAsString(s->state));
	Assert(s->parent == NULL);

	/*
	 * 执行涉及调用用户定义代码（如触发器）的提交前处理。
	 * 由于关闭游标可能排队触发器动作，触发器可能打开游标等，
	 * 我们必须持续循环直到没有事情可做。
 *
 */
	for (;;)
	{
		/*
		 * 触发所有当前待处理的延迟触发器。
		 */
		AfterTriggerFireDeferred();

		/*
		 * 关闭打开的 portal（将可保持的转换为静态 portal）。
		 * 如果没有任何 portal，我们就完成了……否则循环回来检查
		 * 它们是否排队了延迟触发器。如此反复。
		 */
		if (!PreCommit_Portals(true))
			break;
	}

	CallXactCallbacks(XACT_EVENT_PRE_PREPARE);

	/*
	 * 剩余的操作不能调用任何用户定义的代码，因此开始关闭事务内服务是安全的。
	 * 但请注意，这些大多数仍然可能抛出错误，这将使我们切换到事务中止路径。
 *
 *
 */

	/* 关闭延迟触发器管理器 */
	AfterTriggerEndXact(true);

	/*
	 * 让 ON COMMIT 管理执行其操作（必须在关闭游标之后发生，以避免悬空引用问题）
 *
 */
	PreCommit_on_commit_actions();

	/*
	 * 同步在此事务期间创建且未进行 WAL 日志记录的文件。
	 * 这必须在 EndPrepare() 之前发生，以便我们在崩溃和 COMMIT PREPARED 后
	 * 不会看到已提交但损坏的文件。
	 */
	smgrDoPendingSyncs(true, false);

	/* 在较低级别清理之前关闭大对象 */
	AtEOXact_LargeObject(true);

	/* NOTIFY 在此时不需要工作 */

	/*
	 * 为谓词锁定目的将可序列化事务标记为完成。
	 * 这应该尽可能晚地进行，但仍然允许为提交时发现的失败模式引发错误。
 *
 */
	PreCommit_CheckForSerializationFailure();

	/*
	 * 同样，如果在此事务中访问了临时表，则不允许 PREPARE TRANSACTION。
	 * 让预备事务持有另一个后端的临时表上的锁似乎是个坏主意 ---
	 * 例如它会阻止后端退出。还有其他问题，例如如果预备事务
	 * 包含临时表的 DROP，如何清理源后端的本地缓冲区和 ON COMMIT 状态。
	 *
	 * 其他对象类型，如函数、运算符或扩展，共享相同的限制，
	 * 因为它们不应该被创建、锁定或删除，因为这可能会搞乱此会话
	 * 或甚至尝试使用相同临时命名空间的后续会话。
	 *
	 * 我们必须在执行任何 ON COMMIT 动作之后检查此问题，因为它们
	 * 可能仍然访问临时关系。
	 *
	 * XXX 原则上这可以放宽以允许一些有用的特殊情况，
	 * 例如在事务内创建并删除的临时表。但这似乎需要更多的记账工作。
 *
 *
 *
 *
 */
	if ((MyXactFlags & XACT_FLAGS_ACCESSEDTEMPNAMESPACE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has operated on temporary objects")));

	/*
	 * 同样，在 pg_export_snapshot 之后不允许 PREPARE。
	 * 如果我们向 twophase.c 添加清理逻辑，这可能是支持的，
	 * 但目前看来不值得麻烦。
	 */
	if (XactHasExportedSnapshots())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has exported snapshots")));

	/* 在清理时阻止取消/死亡中断 */
	HOLD_INTERRUPTS();

	/*
	 * 在预备处理期间适当设置当前事务状态信息
 *
 */
	s->state = TRANS_PREPARE;

	prepared_at = GetCurrentTimestamp();

	/*
	 * 为此事务保留 GID。如果请求的 GID 无效或已被使用，这可能会失败。
 *
 */
	gxact = MarkAsPreparing(xid, prepareGID, prepared_at,
							GetUserId(), MyDatabaseId);
	prepareGID = NULL;

	/*
	 * 收集 2PC 状态文件的数据。请注意，通常情况下，在此步骤中调用的模块中
	 * 不应发生实际的状态更改，因为在提交之前仍然可能失败，
	 * 在这种情况下我们希望事务中止能够清理。（特别是，AtPrepare 例程
	 * 如果发现无法处理的情况可能会出错。）状态清理应该在下面的
	 * PostPrepare 例程中进行。但是，一些模块可以在这里继续清除状态，
	 * 因为它们在中止期间无论如何都不会对其进行任何操作。
	 *
	 * 注意：由于 2PC 状态文件记录将按其创建的相同顺序重放，
	 * 这些调用的顺序必须与我们在 COMMIT PREPARED 或 ROLLBACK PREPARED 期间
	 * 希望事情发生的顺序匹配；特别是，注意事情应该在释放事务锁之前
	 * 还是之后发生。
 *
 *
 *
 */
	StartPrepare(gxact);

	AtPrepare_Notify();
	AtPrepare_Locks();
	AtPrepare_PredicateLocks();
	AtPrepare_PgStat();
	AtPrepare_MultiXact();
	AtPrepare_RelationMap();

	/*
	 * 这里是我们真正执行预备的地方。
	 *
	 * 即使没有进行任何更新，我们也必须记录事务预备，
	 * 因为如果我们丢失一个全局事务，事务管理器可能会混乱。
 *
 */
	EndPrepare(gxact);

	/*
	 * 现在我们清理后端内部状态并释放内部资源。
	 */

	/* 重置 XactLastRecEnd 直到下一个事务写入内容 */
	XactLastRecEnd = 0;

	/*
	 * 将我们的锁转移到虚拟 PGPROC。这必须在 ProcArrayClearTransaction() 之前完成。
	 * 否则，GetLockConflicts() 会为我们的锁得出"xact already committed or aborted"的结论。
 *
 */
	PostPrepare_Locks(xid);

	/*
	 * 让其他人知道我没有进行中的事务。这必须在预备事务被标记为有效之后执行，
	 * 否则有人可能认为它已解锁且可回收。
 *
 */
	ProcArrayClearTransaction(MyProc);

	/*
	 * 在正常的提交处理中，这都是非关键的事务后清理。
	 * 当事务已预备时，锁和其他每后端资源转移到预备事务的 PGPROC 条目很重要。
	 * 注意，如果在这里引发错误，中止事务已经太晚了。
	 * XXX: 这可能应该在临界区中，以强制 PANIC 如果任何操作失败，
	 * 但这种疗法可能比疾病更糟糕。
 *
 *
 */

	CallXactCallbacks(XACT_EVENT_PREPARE);

	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 true, true);

	/* 检查我们已释放所有缓冲区固定 */
	AtEOXact_Buffers(true);

	/* 清理关系缓存 */
	AtEOXact_RelationCache(true);

	/* notify 在这里不需要 postprepare 调用 */

	PostPrepare_PgStat();

	PostPrepare_Inval();

	PostPrepare_smgr();

	PostPrepare_MultiXact(xid);

	PostPrepare_PredicateLocks(xid);

	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_LOCKS,
						 true, true);
	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 true, true);

	/*
	 * 允许另一个后端完成事务。在 PostPrepare_Twophase() 之后，
	 * 事务完全脱离我们的后端。剩下的只是后端本地状态的非关键清理。
 *
 */
	PostPrepare_Twophase();

	/* 对于 GUC 而言，PREPARE 的行为与 COMMIT 相同 */
	AtEOXact_GUC(true, 1);
	AtEOXact_SPI(true);
	AtEOXact_Enum();
	AtEOXact_on_commit_actions(true);
	AtEOXact_Namespace(true, false);
	AtEOXact_SMgr();
	AtEOXact_Files(true);
	AtEOXact_ComboCid();
	AtEOXact_HashTables(true);
	/* 不要在这里调用 AtEOXact_PgStat；我们上面已经修复了 pgstat 状态 */
	AtEOXact_Snapshot(true, true);
	/* 就唤醒工作进程而言，我们将 PREPARE 视为 ROLLBACK */
	AtEOXact_ApplyLauncher(false);
	AtEOXact_LogicalRepWorkers(false);
	pgstat_report_xact_timestamp(0);

	CurrentResourceOwner = NULL;
	ResourceOwnerDelete(TopTransactionResourceOwner);
	s->curTransactionOwner = NULL;
	CurTransactionResourceOwner = NULL;
	TopTransactionResourceOwner = NULL;

	AtCommit_Memory();

	s->fullTransactionId = InvalidFullTransactionId;
	s->subTransactionId = InvalidSubTransactionId;
	s->nestingLevel = 0;
	s->gucNestLevel = 0;
	s->childXids = NULL;
	s->nChildXids = 0;
	s->maxChildXids = 0;

	XactTopFullTransactionId = InvalidFullTransactionId;
	nParallelCurrentXids = 0;

	/*
	 * 完成第一阶段提交处理，将当前事务状态设置回默认值
 *
 */
	s->state = TRANS_DEFAULT;

	RESUME_INTERRUPTS();
}


/*
 *	AbortTransaction
 */
static void
AbortTransaction(void)
{
	TransactionState s = CurrentTransactionState;
	TransactionId latestXid;
	bool		is_parallel_worker;

	/* 在清理时阻止取消/死亡中断 */
	HOLD_INTERRUPTS();

	/* 确保我们有一个有效的内存上下文和资源所有者 */
	AtAbort_Memory();
	AtAbort_ResourceOwner();

	/*
	 * 尽可能快地释放我们可能持有的任何 LW 锁。（但是，常规锁必须保持到我们完成中止。）
	 * 释放 LW 锁至关重要，因为我们可能会在清理时再次尝试获取它们！
 *
 *
 */
	LWLockReleaseAll();

	/* 清除等待信息和命令进度指示器 */
	pgstat_report_wait_end();
	pgstat_progress_end_command();

	/* 也清理缓冲区上下文锁 */
	UnlockBuffers();

	/* 重置 WAL 记录构造状态 */
	XLogResetInsertion();

	/* 取消条件变量睡眠 */
	ConditionVariableCancelSleep();

	/*
	 * 还要清理任何打开的锁等待，因为如果我们在此操作之前尝试等待另一个锁，
	 * 锁管理器会出问题。
	 */
	LockErrorCleanup();

	/*
	 * 如果有任何超时事件仍然活动，确保超时中断已安排。这覆盖了由于
	 * 从 SIGINT 处理程序 longjmp 而可能丢失超时中断的情况
	 * （参见 handle_sig_alarm 中的注释）。我们延迟到 LockErrorCleanup 之后，
	 * 以便我们不会无用地重新安排锁或死锁检查超时。
 *
 */
	reschedule_timeouts();

	/*
	 * 重新启用信号，以防我们通过从信号处理程序 longjmp 到达这里。
	 * 我们在序列中相当早地执行此操作，以便在中止时如果需要，
	 * 超时基础设施将正常工作。
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 检查当前事务状态
	 */
	is_parallel_worker = (s->blockState == TBLOCK_PARALLEL_INPROGRESS);
	if (s->state != TRANS_INPROGRESS && s->state != TRANS_PREPARE)
		elog(WARNING, "AbortTransaction while in %s state",
			 TransStateAsString(s->state));
	Assert(s->parent == NULL);

	/*
	 * 在中止处理期间适当设置当前事务状态信息
 *
 */
	s->state = TRANS_ABORT;

	/*
	 * 重置可能已被临时更改的用户 ID。我们需要这个来清理，
	 * 以防控制从 SECURITY DEFINER 函数或 CurrentUserId 的其他本地更改中逃逸；
	 * 因此，SecurityRestrictionContext 的先前值也需要恢复。
	 *
	 * （注意：这里不需要恢复会话授权或角色设置，因为那些只能通过 GUC 更改，
	 * GUC 会在需要时负责回滚它们。）
 *
 *
 */
	SetUserIdAndSecContext(s->prevUser, s->prevSecContext);

	/* 忘记任何活动的 REINDEX。 */
	ResetReindexState(s->nestingLevel);

	/* 重置逻辑流状态。 */
	ResetLogicalStreamingState();

	/* 重置快照导出状态。 */
	SnapBuildResetExportedSnapshotState();

	/* 如果在并行模式中，清理工作进程并退出并行模式。 */
	if (IsInParallelMode())
	{
		AtEOXact_Parallel(false);
		s->parallelModeLevel = 0;
	}

	/*
	 * 执行中止处理
	 */
	AfterTriggerEndXact(false); /* 'false' 表示这是中止 */
	AtAbort_Portals();
	smgrDoPendingSyncs(false, is_parallel_worker);
	AtEOXact_LargeObject(false);
	AtAbort_Notify();
	AtEOXact_RelationMap(false, is_parallel_worker);
	AtAbort_Twophase();

	/*
	 * 在 pg_xact 中公布我们中止的事实（假设我们已经到了分配 XID 来公布的地步）。
	 * 但如果我们在并行工作进程内部，跳过此操作；用户后端必须是写入中止记录的那个。
 *
 *
 */
	if (!is_parallel_worker)
		latestXid = RecordTransactionAbort(false);
	else
	{
		latestXid = InvalidTransactionId;

		/*
		 * 由于在这种情况下并行领导者不会获取我们的 XactLastRecEnd 值，
		 * 我们在这种情况下自己推动 WAL-writer。有关为什么这很重要的相关注释，
		 * 请参见 RecordTransactionAbort。
		 */
		XLogSetAsyncXactLSN(XactLastRecEnd);
	}

	TRACE_POSTGRESQL_TRANSACTION_ABORT(MyProc->lxid);

	/*
	 * 让其他人知道我没有进行中的事务。请注意，这必须在释放我们持有的锁之前
	 * _完成_，并且必须在 RecordTransactionAbort 之后执行。
 * (_before_/_after_)
 */
	ProcArrayEndTransaction(MyProc, latestXid);

	/*
	 * 中止后清理。参见 CommitTransaction() 中关于顺序的注释。
	 * 如果事务在创建资源所有者之前失败，我们可以跳过所有这些。
 *
 */
	if (TopTransactionResourceOwner != NULL)
	{
		if (is_parallel_worker)
			CallXactCallbacks(XACT_EVENT_PARALLEL_ABORT);
		else
			CallXactCallbacks(XACT_EVENT_ABORT);

		ResourceOwnerRelease(TopTransactionResourceOwner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 false, true);
		AtEOXact_Buffers(false);
		AtEOXact_RelationCache(false);
		AtEOXact_Inval(false);
		AtEOXact_MultiXact();
		ResourceOwnerRelease(TopTransactionResourceOwner,
							 RESOURCE_RELEASE_LOCKS,
							 false, true);
		ResourceOwnerRelease(TopTransactionResourceOwner,
							 RESOURCE_RELEASE_AFTER_LOCKS,
							 false, true);
		smgrDoPendingDeletes(false);

		AtEOXact_GUC(false, 1);
		AtEOXact_SPI(false);
		AtEOXact_Enum();
		AtEOXact_on_commit_actions(false);
		AtEOXact_Namespace(false, is_parallel_worker);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_ComboCid();
		AtEOXact_HashTables(false);
		AtEOXact_PgStat(false, is_parallel_worker);
		AtEOXact_ApplyLauncher(false);
		AtEOXact_LogicalRepWorkers(false);
		pgstat_report_xact_timestamp(0);
	}

	/*
	 * 状态保持 TRANS_ABORT 直到 CleanupTransaction()。
	 */
	RESUME_INTERRUPTS();
}

/*
 *	CleanupTransaction
 */
static void
CleanupTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 状态应该仍然是 AbortTransaction() 设置的 TRANS_ABORT。
	 */
	if (s->state != TRANS_ABORT)
		elog(FATAL, "CleanupTransaction: unexpected state %s",
			 TransStateAsString(s->state));

	/*
	 * 执行中止清理处理
	 */
	AtCleanup_Portals();		/* 现在可以安全释放 portal 内存 */
	AtEOXact_Snapshot(false, true); /* 并释放事务的快照 */

	CurrentResourceOwner = NULL;	/* 以及资源所有者 */
	if (TopTransactionResourceOwner)
		ResourceOwnerDelete(TopTransactionResourceOwner);
	s->curTransactionOwner = NULL;
	CurTransactionResourceOwner = NULL;
	TopTransactionResourceOwner = NULL;

	AtCleanup_Memory();			/* 以及事务内存 */

	s->fullTransactionId = InvalidFullTransactionId;
	s->subTransactionId = InvalidSubTransactionId;
	s->nestingLevel = 0;
	s->gucNestLevel = 0;
	s->childXids = NULL;
	s->nChildXids = 0;
	s->maxChildXids = 0;
	s->parallelModeLevel = 0;

	XactTopFullTransactionId = InvalidFullTransactionId;
	nParallelCurrentXids = 0;

	/*
	 * 完成中止处理，将当前事务状态设置回默认值
 *
 */
	s->state = TRANS_DEFAULT;
}

/*
 *	StartTransactionCommand
 */
void
StartTransactionCommand(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * 如果我们不在事务块中，我们只是执行通常的启动事务。
 *
 */
		case TBLOCK_DEFAULT:
			StartTransaction();
			s->blockState = TBLOCK_STARTED;
			break;

			/*
			 * 我们在事务块或子事务的某处，即将开始一个新命令。
			 * 目前我们什么都不做，但有一天我们可能会做命令本地资源初始化。
			 * （注意，任何需要的 CommandCounterIncrement 已由前一个
			 * CommitTransactionCommand 完成。）
 *
 */
		case TBLOCK_INPROGRESS:
		case TBLOCK_IMPLICIT_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
			break;

			/*
			 * 这里我们在一个失败的事务块中（其中一个命令导致了中止），
			 * 所以我们什么都不做，但保持在中止状态。最终我们会收到一个
			 * ROLLBACK 命令，它会让我们退出此状态。
			 * （由其他代码确保在这些状态下不会处理 ROLLBACK 以外的命令。）
 *
 *
 */
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(ERROR, "StartTransactionCommand: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	/*
	 * 我们必须在返回之前切换到 CurTransactionContext。如果我们调用了 StartTransaction，
	 * 这已经完成了，否则没有。
	 */
	Assert(CurTransactionContext != NULL);
	MemoryContextSwitchTo(CurTransactionContext);
}


/*
 * 保存和恢复事务特性（隔离级别、只读、可延迟）的简单系统。
 * 我们需要这个用于事务链，以便我们可以将新事务的特性设置为与前一个相同。
 * （我们需要这样的东西，因为 GUC 系统在事务结束时重置特性，
 * 所以例如仅仅在 StartTransaction() 中跳过重置是不行的。）
 *
 *
 */
void
SaveTransactionCharacteristics(SavedTransactionCharacteristics *s)
{
	s->save_XactIsoLevel = XactIsoLevel;
	s->save_XactReadOnly = XactReadOnly;
	s->save_XactDeferrable = XactDeferrable;
}

void
RestoreTransactionCharacteristics(const SavedTransactionCharacteristics *s)
{
	XactIsoLevel = s->save_XactIsoLevel;
	XactReadOnly = s->save_XactReadOnly;
	XactDeferrable = s->save_XactDeferrable;
}


/*
 *	CommitTransactionCommand
 */
void
CommitTransactionCommand(void)
{
	TransactionState s = CurrentTransactionState;
	SavedTransactionCharacteristics savetc;

	/* 必须保存以防我们需要在下面恢复 */
	SaveTransactionCharacteristics(&savetc);

	switch (s->blockState)
	{
			/*
			 * 这些不应该发生。TBLOCK_DEFAULT 意味着前一个 StartTransactionCommand
			 * 没有正确设置 STARTED 状态，而 TBLOCK_PARALLEL_INPROGRESS 应该由
			 * EndParallelWorkerTransaction() 结束，而不是这个函数。
 *
 */
		case TBLOCK_DEFAULT:
		case TBLOCK_PARALLEL_INPROGRESS:
			elog(FATAL, "CommitTransactionCommand: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;

			/*
			 * 如果我们不在事务块中，只执行通常的事务提交，并返回到空闲状态。
 *
 */
		case TBLOCK_STARTED:
			CommitTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 我们正在完成一个 "BEGIN TRANSACTION" 命令，所以我们切换到
			 * "事务块进行中" 状态并返回。（我们假设 BEGIN 没有对数据库做任何事情，
			 * 所以我们不需要 CommandCounterIncrement。）
 *
 */
		case TBLOCK_BEGIN:
			s->blockState = TBLOCK_INPROGRESS;
			break;

			/*
			 * 这是我们在事务块内的某处完成执行命令的情况。
			 * 我们递增命令计数器并返回。
 *
 */
		case TBLOCK_INPROGRESS:
		case TBLOCK_IMPLICIT_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
			CommandCounterIncrement();
			break;

			/*
			 * 我们正在完成一个 "COMMIT" 命令。执行它并返回到空闲状态。
 *
 */
		case TBLOCK_END:
			CommitTransaction();
			s->blockState = TBLOCK_DEFAULT;
			if (s->chain)
			{
				StartTransaction();
				s->blockState = TBLOCK_INPROGRESS;
				s->chain = false;
				RestoreTransactionCharacteristics(&savetc);
			}
			break;

			/*
			 * 这里我们在事务块中间，但其中一个命令导致了中止，
			 * 所以我们什么都不做，但保持在中止状态。最终我们会收到一个 ROLLBACK 命令。
 *
 */
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
			break;

			/*
			 * 这里我们处于一个已中止的事务块中，我们刚刚从用户那里收到 ROLLBACK 命令，
			 * 所以清理已经中止的事务并返回到空闲状态。
 *
 */
		case TBLOCK_ABORT_END:
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			if (s->chain)
			{
				StartTransaction();
				s->blockState = TBLOCK_INPROGRESS;
				s->chain = false;
				RestoreTransactionCharacteristics(&savetc);
			}
			break;

			/*
			 * 这里我们处于一个完美的事务块中，但用户还是告诉我们要 ROLLBACK。
			 * 我们必须中止事务然后清理。
 *
 */
		case TBLOCK_ABORT_PENDING:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			if (s->chain)
			{
				StartTransaction();
				s->blockState = TBLOCK_INPROGRESS;
				s->chain = false;
				RestoreTransactionCharacteristics(&savetc);
			}
			break;

			/*
			 * 我们正在完成一个 "PREPARE TRANSACTION" 命令。执行它并返回到空闲状态。
 *
 */
		case TBLOCK_PREPARE:
			PrepareTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 用户在事务块内发出了 SAVEPOINT。启动一个子事务。
			 * （DefineSavepoint 已经执行了 PushTransaction，
			 * 以便有地方放置 SUBBEGIN 状态。）
			 */
		case TBLOCK_SUBBEGIN:
			StartSubTransaction();
			s->blockState = TBLOCK_SUBINPROGRESS;
			break;

			/*
			 * 用户发出了 RELEASE 命令，所以我们结束当前的子事务
			 * 并返回到父事务。父事务也可能结束，所以重复直到
			 * 找到一个 INPROGRESS 事务或子事务。
 *
 */
		case TBLOCK_SUBRELEASE:
			do
			{
				CommitSubTransaction();
				s = CurrentTransactionState;	/* 由 pop 更改 */
			} while (s->blockState == TBLOCK_SUBRELEASE);

			Assert(s->blockState == TBLOCK_INPROGRESS ||
				   s->blockState == TBLOCK_SUBINPROGRESS);
			break;

			/*
			 * 用户发出了 COMMIT，所以我们结束当前的子事务层次并执行最终提交。
			 * 我们通过将任何子事务卷入其父事务来实现这一点，
			 * 这会导致关于资源所有者的 O(N^2) 操作 —— 在我们接近数千个保存点之前
			 * 这并不是那么糟糕，但如果 after 触发器创建新的资源所有者，
			 * 这对于正确性是必要的。
 *
 *
 */
		case TBLOCK_SUBCOMMIT:
			do
			{
				CommitSubTransaction();
				s = CurrentTransactionState;	/* 由 pop 改变 */
			} while (s->blockState == TBLOCK_SUBCOMMIT);
			/* 如果我们有一个 COMMIT 命令，也完成主事务 */
			if (s->blockState == TBLOCK_END)
			{
				Assert(s->parent == NULL);
				CommitTransaction();
				s->blockState = TBLOCK_DEFAULT;
				if (s->chain)
				{
					StartTransaction();
					s->blockState = TBLOCK_INPROGRESS;
					s->chain = false;
					RestoreTransactionCharacteristics(&savetc);
				}
			}
			else if (s->blockState == TBLOCK_PREPARE)
			{
				Assert(s->parent == NULL);
				PrepareTransaction();
				s->blockState = TBLOCK_DEFAULT;
			}
			else
				elog(ERROR, "CommitTransactionCommand: unexpected state %s",
					 BlockStateAsString(s->blockState));
			break;

			/*
			 * 当前已经失败的子事务由于 ROLLBACK 或 ROLLBACK TO 命令而结束，
			 * 所以弹出它并递归检查父事务（父事务可能处于多种状态之一）。
 *
 */
		case TBLOCK_SUBABORT_END:
			CleanupSubTransaction();
			CommitTransactionCommand();
			break;

			/*
			 * 同上，但它还没有死，所以先中止。
			 */
		case TBLOCK_SUBABORT_PENDING:
			AbortSubTransaction();
			CleanupSubTransaction();
			CommitTransactionCommand();
			break;

			/*
			 * 当前子事务是 ROLLBACK TO 命令的目标。
			 * 中止并弹出它，然后启动一个具有相同名称的新子事务。
 *
 */
		case TBLOCK_SUBRESTART:
			{
				char	   *name;
				int			savepointLevel;

				/* 保存名称并防止 Cleanup 释放它 */
				name = s->name;
				s->name = NULL;
				savepointLevel = s->savepointLevel;

				AbortSubTransaction();
				CleanupSubTransaction();

				DefineSavepoint(NULL);
				s = CurrentTransactionState;	/* 由 push 改变 */
				s->name = name;
				s->savepointLevel = savepointLevel;

				/* 这与 TBLOCK_SUBBEGIN 情况相同 */
				Assert(s->blockState == TBLOCK_SUBBEGIN);
				StartSubTransaction();
				s->blockState = TBLOCK_SUBINPROGRESS;
			}
			break;

			/*
			 * 与上面相同，但子事务已经失败，所以我们不需要 AbortSubTransaction。
 *
 */
		case TBLOCK_SUBABORT_RESTART:
			{
				char	   *name;
				int			savepointLevel;

				/* 保存名称并防止 Cleanup 释放它 */
				name = s->name;
				s->name = NULL;
				savepointLevel = s->savepointLevel;

				CleanupSubTransaction();

				DefineSavepoint(NULL);
				s = CurrentTransactionState;	/* 由 push 改变 */
				s->name = name;
				s->savepointLevel = savepointLevel;

				/* 这与 TBLOCK_SUBBEGIN 情况相同 */
				Assert(s->blockState == TBLOCK_SUBBEGIN);
				StartSubTransaction();
				s->blockState = TBLOCK_SUBINPROGRESS;
			}
			break;
	}
}

/*
 *	AbortCurrentTransaction
 */
void
AbortCurrentTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
		case TBLOCK_DEFAULT:
			if (s->state == TRANS_DEFAULT)
			{
				/* 我们处于空闲状态，所以没有什么可做的 */
			}
			else
			{
				/*
				 * 我们可能在事务启动期间出错后到达这里（状态将是 TRANS_START）。
				 * 需要清理未完全启动的事务。首先，调整底层状态以抑制
				 * AbortTransaction 的警告消息。
 *
 *
 */
				if (s->state == TRANS_START)
					s->state = TRANS_INPROGRESS;
				AbortTransaction();
				CleanupTransaction();
			}
			break;

			/*
			 * 如果我们不在事务块中，我们只是执行基本的中止和清理事务。
			 * 为此目的，我们将隐式事务块视为简单语句。
 *
 */
		case TBLOCK_STARTED:
		case TBLOCK_IMPLICIT_INPROGRESS:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 如果我们在 TBLOCK_BEGIN 中，这意味着在读取 "BEGIN TRANSACTION" 之后
			 * 立即出现问题。我们假设用户会将错误解释为 BEGIN 未能让他进入事务块，
			 * 所以我们应该中止并返回空闲状态。
 *
 *
 */
		case TBLOCK_BEGIN:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 我们在事务块的某处并且遇到了失败，所以我们中止事务
			 * 并设置持久的 ABORT 状态。我们将保持 ABORT 直到收到 ROLLBACK。
 *
 */
		case TBLOCK_INPROGRESS:
		case TBLOCK_PARALLEL_INPROGRESS:
			AbortTransaction();
			s->blockState = TBLOCK_ABORT;
			/* CleanupTransaction 在我们退出 TBLOCK_ABORT_END 时发生 */
			break;

			/*
			 * 这里，我们在尝试 COMMIT 时失败了。清理事务并返回空闲状态
			 * （我们不想留在事务中）。
 *
 */
		case TBLOCK_END:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 这里，我们已经在中止的事务状态中并等待 ROLLBACK，
			 * 但由于某种原因我们再次失败！所以我们只是保持在中止状态。
 *
 */
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
			break;

			/*
			 * 我们在失败的事务中并收到了 ROLLBACK 命令。
			 * 我们已经中止了，只需要清理并转到空闲状态。
 *
 */
		case TBLOCK_ABORT_END:
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 我们在活跃的事务中并收到了 ROLLBACK 命令。
			 * 中止、清理、转到空闲状态。
			 */
		case TBLOCK_ABORT_PENDING:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 这里，我们在尝试 PREPARE 时失败了。清理事务并返回空闲状态
			 * （我们不想留在事务中）。
 *
 */
		case TBLOCK_PREPARE:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * 我们在子事务内部遇到了错误。只中止子事务，
			 * 并进入持久的 SUBABORT 状态直到收到 ROLLBACK。
 *
 */
		case TBLOCK_SUBINPROGRESS:
			AbortSubTransaction();
			s->blockState = TBLOCK_SUBABORT;
			break;

			/*
			 * 如果我们在尝试创建子事务时失败，清理损坏的子事务并中止父事务。
			 * 同样适用于我们在结束子事务时遇到失败。
 *
 */
		case TBLOCK_SUBBEGIN:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
			AbortSubTransaction();
			CleanupSubTransaction();
			AbortCurrentTransaction();
			break;

			/*
			 * 与上面相同，但 Abort() 已经完成。
			 */
		case TBLOCK_SUBABORT_END:
		case TBLOCK_SUBABORT_RESTART:
			CleanupSubTransaction();
			AbortCurrentTransaction();
			break;
	}
}

/*
 *	PreventInTransactionBlock
 *
 *	此例程由不能在事务块内运行的语句调用，通常是因为它们有不可回滚的副作用
 *	或执行内部提交。
 *
 *	如果此例程成功完成，则调用语句可以保证如果它无错误完成，其结果将立即提交。
 *
 *	如果我们已经启动了事务块，则发出错误；如果我们似乎在用户定义函数内运行
 *	（这可能发出更多命令并可能在语句完成后导致失败），也发出错误。子事务也被禁止。
 *
 *	我们还必须在 MyXactFlags 中设置 XACT_FLAGS_NEEDIMMEDIATECOMMIT，
 *	以确保 postgres.c 在语句完成后提交。
 *
 *	isTopLevel: 从 ProcessUtility 向下传递，以确定我们是否在函数内部。
 *	（如果这为 false，我们将总是失败，但在这里集中检查比让调用者做更方便。）
 *	stmtType: 语句类型名称，用于错误消息。
 *
 *
 *
 *
 *
 *
 */
void
PreventInTransactionBlock(bool isTopLevel, const char *stmtType)
{
	/*
	 * 事务块已经启动？
	 */
	if (IsTransactionBlock())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		/* translator: %s represents an SQL statement name */
				 errmsg("%s cannot run inside a transaction block",
						stmtType)));

	/*
	 * 子事务？
	 */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		/* translator: %s represents an SQL statement name */
				 errmsg("%s cannot run inside a subtransaction",
						stmtType)));

	/*
	 * 在已启动隐式事务的管道内？
	 */
	if (MyXactFlags & XACT_FLAGS_PIPELINING)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		/* translator: %s represents an SQL statement name */
				 errmsg("%s cannot be executed within a pipeline",
						stmtType)));

	/*
	 * 在函数调用内部？
	 */
	if (!isTopLevel)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		/* translator: %s represents an SQL statement name */
				 errmsg("%s cannot be executed from a function", stmtType)));

	/* 如果我们通过了 IsTransactionBlock 测试，应该处于默认状态 */
	if (CurrentTransactionState->blockState != TBLOCK_DEFAULT &&
		CurrentTransactionState->blockState != TBLOCK_STARTED)
		elog(FATAL, "cannot prevent transaction chain");

	/* 一切正常。设置标志以确保稍后发生正确的事情。 */
	MyXactFlags |= XACT_FLAGS_NEEDIMMEDIATECOMMIT;
}

/*
 *	WarnNoTransactionBlock
 *	RequireTransactionBlock
 *
 *	这两个函数允许在事务块外执行命令时发出警告或错误。
 *	这对于在事务结束后没有持久效果的命令很有用（所以在事务块外调用它们
 *	大概是错误的）。DECLARE CURSOR 是一个例子。
 *	虽然顶层事务控制命令（BEGIN/COMMIT/ABORT）和无效的 SET 发出警告，
 *	但所有其他无效命令生成错误。
 *
 *	如果我们似乎在用户定义函数内运行，我们不发出任何内容，
 *	因为函数可能发出更多使用当前语句结果的命令。子事务也是如此。
 *	因此这些是 PreventInTransactionBlock 的逆操作。
 *
 *	isTopLevel: 从 ProcessUtility 向下传递，以确定我们是否在函数内部。
 *	stmtType: 语句类型名称，用于警告或错误消息。
 *
 *
 *
 *
 */
void
WarnNoTransactionBlock(bool isTopLevel, const char *stmtType)
{
	CheckTransactionBlock(isTopLevel, false, stmtType);
}

void
RequireTransactionBlock(bool isTopLevel, const char *stmtType)
{
	CheckTransactionBlock(isTopLevel, true, stmtType);
}

/*
 * 这是上述两个函数的实现。
 */
static void
CheckTransactionBlock(bool isTopLevel, bool throwError, const char *stmtType)
{
	/*
	 * 事务块已经启动？
	 */
	if (IsTransactionBlock())
		return;

	/*
	 * 子事务？
	 */
	if (IsSubTransaction())
		return;

	/*
	 * 在函数调用内部？
	 */
	if (!isTopLevel)
		return;

	ereport(throwError ? ERROR : WARNING,
			(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
	/* translator: %s represents an SQL statement name */
			 errmsg("%s can only be used in transaction blocks",
					stmtType)));
}

/*
 *	IsInTransactionBlock
 *
 *	此例程用于需要在事务块内与作为单个命令运行时表现不同的语句。
 *	ANALYZE 目前是唯一的例子。
 *
 *	如果此例程返回 "false"，则调用语句被允许执行内部事务提交和启动循环；
 *	没有搞乱任何正在进行的事务的风险。（请注意，这不是 PreventInTransactionBlock
 *	提供的相同保证，因为我们不会强制语句后提交。）
 *
 *	isTopLevel: 从 ProcessUtility 向下传递，以确定我们是否在函数内部。
 *
 *
 *
 *
 */
bool
IsInTransactionBlock(bool isTopLevel)
{
	/*
	 * 在会使 PreventInTransactionBlock 出错的相同条件下返回 true
 *
 */
	if (IsTransactionBlock())
		return true;

	if (IsSubTransaction())
		return true;

	if (MyXactFlags & XACT_FLAGS_PIPELINING)
		return true;

	if (!isTopLevel)
		return true;

	if (CurrentTransactionState->blockState != TBLOCK_DEFAULT &&
		CurrentTransactionState->blockState != TBLOCK_STARTED)
		return true;

	return false;
}


/*
 * 注册或注销事务开始和结束操作的回调函数。
 *
 * 这些函数旨在供动态加载的模块使用。
 * 对于内置模块，我们通常只是硬编码适当的调用
 * （主要是因为这样可以更容易地在需要时控制顺序）。
 *
 * 在事务结束时，回调发生在提交后或中止后，
 * 因此回调函数只能执行非关键的清理。
 *
 */
void
RegisterXactCallback(XactCallback callback, void *arg)
{
	XactCallbackItem *item;

	item = (XactCallbackItem *)
		MemoryContextAlloc(TopMemoryContext, sizeof(XactCallbackItem));
	item->callback = callback;
	item->arg = arg;
	item->next = Xact_callbacks;
	Xact_callbacks = item;
}

void
UnregisterXactCallback(XactCallback callback, void *arg)
{
	XactCallbackItem *item;
	XactCallbackItem *prev;

	prev = NULL;
	for (item = Xact_callbacks; item; prev = item, item = item->next)
	{
		if (item->callback == callback && item->arg == arg)
		{
			if (prev)
				prev->next = item->next;
			else
				Xact_callbacks = item->next;
			pfree(item);
			break;
		}
	}
}

static void
CallXactCallbacks(XactEvent event)
{
	XactCallbackItem *item;
	XactCallbackItem *next;

	for (item = Xact_callbacks; item; item = next)
	{
		/* 允许回调在调用时注销自己 */
		next = item->next;
		item->callback(event, item->arg);
	}
}


/*
 * 注册或注销子事务开始和结束操作的回调函数。
 *
 * 与上述基本相同，但用于子事务事件。
 *
 * 在子事务结束时，回调发生在子提交后或子中止后，
 * 因此回调函数只能执行非关键的清理。在子事务开始时，
 * 当子事务完成初始化时调用回调。
 *
 *
 */
void
RegisterSubXactCallback(SubXactCallback callback, void *arg)
{
	SubXactCallbackItem *item;

	item = (SubXactCallbackItem *)
		MemoryContextAlloc(TopMemoryContext, sizeof(SubXactCallbackItem));
	item->callback = callback;
	item->arg = arg;
	item->next = SubXact_callbacks;
	SubXact_callbacks = item;
}

void
UnregisterSubXactCallback(SubXactCallback callback, void *arg)
{
	SubXactCallbackItem *item;
	SubXactCallbackItem *prev;

	prev = NULL;
	for (item = SubXact_callbacks; item; prev = item, item = item->next)
	{
		if (item->callback == callback && item->arg == arg)
		{
			if (prev)
				prev->next = item->next;
			else
				SubXact_callbacks = item->next;
			pfree(item);
			break;
		}
	}
}

static void
CallSubXactCallbacks(SubXactEvent event,
					 SubTransactionId mySubid,
					 SubTransactionId parentSubid)
{
	SubXactCallbackItem *item;
	SubXactCallbackItem *next;

	for (item = SubXact_callbacks; item; item = next)
	{
		/* 允许回调在调用时注销自己 */
		next = item->next;
		item->callback(event, mySubid, parentSubid, item->arg);
	}
}


/* ----------------------------------------------------------------
 *					   事务块支持
 * ----------------------------------------------------------------
 */

/*
 *	BeginTransactionBlock
 *		这执行一个 BEGIN 命令。
 */
void
BeginTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * 我们不在事务块内，所以允许开始一个。
			 */
		case TBLOCK_STARTED:
			s->blockState = TBLOCK_BEGIN;
			break;

			/*
			 * BEGIN 将隐式事务块转换为常规事务块。
			 * （请注意，即使我们已经执行了一些命令，我们也允许这样做，
			 * 这有点奇怪但符合历史实践。）
			 */
		case TBLOCK_IMPLICIT_INPROGRESS:
			s->blockState = TBLOCK_BEGIN;
			break;

			/*
			 * 已经有一个事务块在进行中。
			 */
		case TBLOCK_INPROGRESS:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
			ereport(WARNING,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("there is already a transaction in progress")));
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(FATAL, "BeginTransactionBlock: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}
}

/*
 *	PrepareTransactionBlock
 *		这执行一个 PREPARE 命令。
 *
 * 由于 PREPARE 实际上可能执行 ROLLBACK，结果指示发生了什么：
 * true 表示 PREPARE，false 表示 ROLLBACK。
 *
 * 请注意，我们实际上不在这里做任何事情，只是更改 blockState。
 * 真正的工作将在接下来的 PrepareTransaction() 中完成。
 * 我们这样做是因为在 Portal 内执行时更改内存上下文、资源所有者等不方便。
 *
 */
bool
PrepareTransactionBlock(const char *gid)
{
	TransactionState s;
	bool		result;

	/* 设置以提交当前事务 */
	result = EndTransactionBlock(false);

	/* 如果成功，将外部 tblock 状态更改为 PREPARE */
	if (result)
	{
		s = CurrentTransactionState;

		while (s->parent != NULL)
			s = s->parent;

		if (s->blockState == TBLOCK_END)
		{
			/* 保存 GID，以便 PrepareTransaction 可以再次找到它 */
			prepareGID = MemoryContextStrdup(TopTransactionContext, gid);

			s->blockState = TBLOCK_PREPARE;
		}
		else
		{
			/*
			 * 忽略我们不在事务中的情况；
			 * EndTransactionBlock 已经发出了警告。
			 */
			Assert(s->blockState == TBLOCK_STARTED ||
				   s->blockState == TBLOCK_IMPLICIT_INPROGRESS);
			/* 不发送回 PREPARE 结果标签... */
			result = false;
		}
	}

	return result;
}

/*
 *	EndTransactionBlock
 *		这执行一个 COMMIT 命令。
 *
 * 由于 COMMIT 实际上可能执行 ROLLBACK，结果指示发生了什么：
 * true 表示 COMMIT，false 表示 ROLLBACK。
 *
 * 请注意，我们实际上不在这里做任何事情，只是更改 blockState。
 * 真正的工作将在接下来的 CommitTransactionCommand() 中完成。
 * 我们这样做是因为在 Portal 内执行时更改内存上下文、资源所有者等不方便。
 *
 */
bool
EndTransactionBlock(bool chain)
{
	TransactionState s = CurrentTransactionState;
	bool		result = false;

	switch (s->blockState)
	{
			/*
			 * 我们在事务块中，所以告诉 CommitTransactionCommand 执行 COMMIT。
 *
 */
		case TBLOCK_INPROGRESS:
			s->blockState = TBLOCK_END;
			result = true;
			break;

			/*
			 * 我们在隐式事务块中。如果指定了 AND CHAIN，则报错。
			 * 否则提交，但发出警告，因为在此之前没有显式的 BEGIN。
 *
 */
		case TBLOCK_IMPLICIT_INPROGRESS:
			if (chain)
				ereport(ERROR,
						(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
				/* translator: %s represents an SQL statement name */
						 errmsg("%s can only be used in transaction blocks",
								"COMMIT AND CHAIN")));
			else
				ereport(WARNING,
						(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
						 errmsg("there is no transaction in progress")));
			s->blockState = TBLOCK_END;
			result = true;
			break;

			/*
			 * 我们在失败的事务块中。告诉 CommitTransactionCommand
			 * 是时候退出该块了。
			 */
		case TBLOCK_ABORT:
			s->blockState = TBLOCK_ABORT_END;
			break;

			/*
			 * 我们在活跃的子事务块中。设置以子提交所有打开的子事务，
			 * 然后提交主事务。
			 */
		case TBLOCK_SUBINPROGRESS:
			while (s->parent != NULL)
			{
				if (s->blockState == TBLOCK_SUBINPROGRESS)
					s->blockState = TBLOCK_SUBCOMMIT;
				else
					elog(FATAL, "EndTransactionBlock: unexpected state %s",
						 BlockStateAsString(s->blockState));
				s = s->parent;
			}
			if (s->blockState == TBLOCK_INPROGRESS)
				s->blockState = TBLOCK_END;
			else
				elog(FATAL, "EndTransactionBlock: unexpected state %s",
					 BlockStateAsString(s->blockState));
			result = true;
			break;

			/*
			 * 这里我们在已中止的子事务内部。将 COMMIT 视为 ROLLBACK：
			 * 设置以中止所有内容并退出主事务。
 *
 */
		case TBLOCK_SUBABORT:
			while (s->parent != NULL)
			{
				if (s->blockState == TBLOCK_SUBINPROGRESS)
					s->blockState = TBLOCK_SUBABORT_PENDING;
				else if (s->blockState == TBLOCK_SUBABORT)
					s->blockState = TBLOCK_SUBABORT_END;
				else
					elog(FATAL, "EndTransactionBlock: unexpected state %s",
						 BlockStateAsString(s->blockState));
				s = s->parent;
			}
			if (s->blockState == TBLOCK_INPROGRESS)
				s->blockState = TBLOCK_ABORT_PENDING;
			else if (s->blockState == TBLOCK_ABORT)
				s->blockState = TBLOCK_ABORT_END;
			else
				elog(FATAL, "EndTransactionBlock: unexpected state %s",
					 BlockStateAsString(s->blockState));
			break;

			/*
			 * 用户在不在事务内部时发出了 COMMIT。对于没有 CHAIN 的 COMMIT，
			 * 发出 WARNING，保持在 TBLOCK_STARTED 状态。
			 * 接下来的 CommitTransactionCommand() 调用将关闭事务
			 * 并将我们放回默认状态。对于 COMMIT AND CHAIN，报错。
 *
 *
 */
		case TBLOCK_STARTED:
			if (chain)
				ereport(ERROR,
						(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
				/* translator: %s represents an SQL statement name */
						 errmsg("%s can only be used in transaction blocks",
								"COMMIT AND CHAIN")));
			else
				ereport(WARNING,
						(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
						 errmsg("there is no transaction in progress")));
			result = true;
			break;

			/*
			 * 用户发出了一个在并行工作进程内运行的 COMMIT。
			 * 我们无法处理这种情况。
			 */
		case TBLOCK_PARALLEL_INPROGRESS:
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("cannot commit during a parallel operation")));
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(FATAL, "EndTransactionBlock: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	Assert(s->blockState == TBLOCK_STARTED ||
		   s->blockState == TBLOCK_END ||
		   s->blockState == TBLOCK_ABORT_END ||
		   s->blockState == TBLOCK_ABORT_PENDING);

	s->chain = chain;

	return result;
}

/*
 *	UserAbortTransactionBlock
 *		这执行一个 ROLLBACK 命令。
 *
 * 与上述相同，我们实际上不在这里做任何事情，只是更改 blockState。
 */
void
UserAbortTransactionBlock(bool chain)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * 我们在事务块内部，从用户那里收到了 ROLLBACK 命令，
			 * 所以告诉 CommitTransactionCommand 中止并退出事务块。
 *
 */
		case TBLOCK_INPROGRESS:
			s->blockState = TBLOCK_ABORT_PENDING;
			break;

			/*
			 * 我们在失败的事务块内部，从用户那里收到了 ROLLBACK 命令。
			 * 中止处理已经完成，所以 CommitTransactionCommand 只需要清理
			 * 并返回空闲状态。
 *
 */
		case TBLOCK_ABORT:
			s->blockState = TBLOCK_ABORT_END;
			break;

			/*
			 * 我们在子事务内部。将所有内容标记到顶层为可退出。
 *
 */
		case TBLOCK_SUBINPROGRESS:
		case TBLOCK_SUBABORT:
			while (s->parent != NULL)
			{
				if (s->blockState == TBLOCK_SUBINPROGRESS)
					s->blockState = TBLOCK_SUBABORT_PENDING;
				else if (s->blockState == TBLOCK_SUBABORT)
					s->blockState = TBLOCK_SUBABORT_END;
				else
					elog(FATAL, "UserAbortTransactionBlock: unexpected state %s",
						 BlockStateAsString(s->blockState));
				s = s->parent;
			}
			if (s->blockState == TBLOCK_INPROGRESS)
				s->blockState = TBLOCK_ABORT_PENDING;
			else if (s->blockState == TBLOCK_ABORT)
				s->blockState = TBLOCK_ABORT_END;
			else
				elog(FATAL, "UserAbortTransactionBlock: unexpected state %s",
					 BlockStateAsString(s->blockState));
			break;

			/*
			 * 用户在不在事务内部时发出了 ABORT。对于没有 CHAIN 的 ROLLBACK，
			 * 发出 WARNING 并进入中止状态。接下来的 CommitTransactionCommand() 调用
			 * 将把我们放回默认状态。对于 ROLLBACK AND CHAIN，报错。
			 *
			 * 我们对隐式事务内的 ABORT 做同样的事情，
			 * 尽管在这种情况下我们可能正在回滚实际的数据库状态更改。
			 * （在这种情况下是否应该发出 WARNING 是有争议的，但我们历史上一直这样做。）
 *
 *
 */
		case TBLOCK_STARTED:
		case TBLOCK_IMPLICIT_INPROGRESS:
			if (chain)
				ereport(ERROR,
						(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
				/* translator: %s represents an SQL statement name */
						 errmsg("%s can only be used in transaction blocks",
								"ROLLBACK AND CHAIN")));
			else
				ereport(WARNING,
						(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
						 errmsg("there is no transaction in progress")));
			s->blockState = TBLOCK_ABORT_PENDING;
			break;

			/*
			 * 用户发出了一个在并行工作进程内运行的 ABORT。
			 * 我们无法处理这种情况。
			 */
		case TBLOCK_PARALLEL_INPROGRESS:
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("cannot abort during a parallel operation")));
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(FATAL, "UserAbortTransactionBlock: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	Assert(s->blockState == TBLOCK_ABORT_END ||
		   s->blockState == TBLOCK_ABORT_PENDING);

	s->chain = chain;
}

/*
 * BeginImplicitTransactionBlock
 *		如果我们还没有在隐式事务块中，则启动一个。
 *
 * 与 BeginTransactionBlock 不同，这是直接从 postgres.c 的主循环调用的，
 * 而不是在 Portal 内。所以我们可以直接更改 blockState 而不需要太多仪式。
 * 我们不期望调用者执行 CommitTransactionCommand/StartTransactionCommand。
 *
 */
void
BeginImplicitTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 如果我们处于 STARTED 状态（即没有打开事务块），
	 * 切换到 IMPLICIT_INPROGRESS 状态，创建一个隐式事务块。
	 *
	 * 为了调用者方便，我们在这里认为所有其他事务状态都是合法的；
	 * 否则调用者需要自己的状态检查，这似乎相当无意义。
 *
 *
 */
	if (s->blockState == TBLOCK_STARTED)
		s->blockState = TBLOCK_IMPLICIT_INPROGRESS;
}

/*
 * EndImplicitTransactionBlock
 *		结束一个隐式事务块（如果我们处于其中）。
 *
 * 与 EndTransactionBlock 类似，我们只在这里进行任何需要的 blockState 更改。
 * 真正的工作将在接下来的 CommitTransactionCommand() 中完成。
 */
void
EndImplicitTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 如果我们处于 IMPLICIT_INPROGRESS 状态，切换回 STARTED 状态，
	 * 允许 CommitTransactionCommand 提交隐式事务块期间发生的内容，
	 * 就像它是单个语句一样。
	 *
	 * 为调用者方便，我们在这里认为所有其他事务状态都是合法的；
	 * 否则调用者需要自己的状态检查，这似乎相当无意义。
 *
 */
	if (s->blockState == TBLOCK_IMPLICIT_INPROGRESS)
		s->blockState = TBLOCK_STARTED;
}

/*
 * DefineSavepoint
 *		这执行一个 SAVEPOINT 命令。
 */
void
DefineSavepoint(const char *name)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 工作进程在每个并行操作开始时同步事务状态，
	 * 所以我们在此之后无法处理新的子事务。
	 * （注意，如果 s->blockState 是 TBLOCK_PARALLEL_INPROGRESS，
	 * 此检查肯定会出错，所以我们可以在下面将其视为无效情况。）
 *
 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot define savepoints during a parallel operation")));

	switch (s->blockState)
	{
		case TBLOCK_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
			/* 正常的子事务启动 */
			PushTransaction();
			s = CurrentTransactionState;	/* 由 push 改变 */

			/*
			 * 保存点名称（像 TransactionState 块本身一样）
			 * 存储在 TopTransactionContext 中。
			 */
			if (name)
				s->name = MemoryContextStrdup(TopTransactionContext, name);
			break;

			/*
			 * 我们禁止在隐式事务块中使用保存点命令。
			 * 就本模块而言，允许它们没有太大的困难，
			 * 但保存点似乎与 exec_simple_query 在出错时
			 * 放弃整个查询字符串的行为不一致。此外，
			 * 隐式事务块（相对于常规事务块）的目的是在
			 * 出错后自动关闭，所以很难看出保存点如何适应这一点。
			 *
			 * 此错误的提示消息被表述为根本没有活动的事务块，
			 * 这是历史性的，但也许可以改进。
 *
 *
 *
 */
		case TBLOCK_IMPLICIT_INPROGRESS:
			ereport(ERROR,
					(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
			/* translator: %s represents an SQL statement name */
					 errmsg("%s can only be used in transaction blocks",
							"SAVEPOINT")));
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(FATAL, "DefineSavepoint: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}
}

/*
 * ReleaseSavepoint
 *		这执行一个 RELEASE 命令。
 *
 * 如上所述，我们实际上不在这里做任何事情，只是更改 blockState。
 */
void
ReleaseSavepoint(const char *name)
{
	TransactionState s = CurrentTransactionState;
	TransactionState target,
				xact;

	/*
	 * 工作进程在每个并行操作开始时同步事务状态，
	 * 所以我们在此之后无法处理事务状态更改。
	 * （注意，如果 s->blockState 是 TBLOCK_PARALLEL_INPROGRESS，
	 * 此检查肯定会出错，所以我们可以在下面将其视为无效情况。）
 *
 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot release savepoints during a parallel operation")));

	switch (s->blockState)
	{
			/*
			 * 如果没有定义保存点，我们就无法释放保存点。
			 */
		case TBLOCK_INPROGRESS:
			ereport(ERROR,
					(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
					 errmsg("savepoint \"%s\" does not exist", name)));
			break;

		case TBLOCK_IMPLICIT_INPROGRESS:
			/* 参见 DefineSavepoint 中关于隐式事务的注释 */
			ereport(ERROR,
					(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
			/* translator: %s represents an SQL statement name */
					 errmsg("%s can only be used in transaction blocks",
							"RELEASE SAVEPOINT")));
			break;

			/*
			 * 我们处于一个未中止的子事务中。这是唯一有效的情况。
 *
 */
		case TBLOCK_SUBINPROGRESS:
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(FATAL, "ReleaseSavepoint: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	for (target = s; PointerIsValid(target); target = target->parent)
	{
		if (PointerIsValid(target->name) && strcmp(target->name, name) == 0)
			break;
	}

	if (!PointerIsValid(target))
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("savepoint \"%s\" does not exist", name)));

	/* 禁止跨越保存点级别边界 */
	if (target->savepointLevel != s->savepointLevel)
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("savepoint \"%s\" does not exist within current savepoint level", name)));

	/*
	 * 将目标子事务之前的所有子事务标记为"提交待定"。
	 * 实际的提交将在控制到达 CommitTransactionCommand 时发生。
 *
 */
	xact = CurrentTransactionState;
	for (;;)
	{
		Assert(xact->blockState == TBLOCK_SUBINPROGRESS);
		xact->blockState = TBLOCK_SUBRELEASE;
		if (xact == target)
			break;
		xact = xact->parent;
		Assert(PointerIsValid(xact));
	}
}

/*
 * RollbackToSavepoint
 *		这执行一个 ROLLBACK TO <savepoint> 命令。
 *
 * 如上所述，我们实际上不在这里做任何事情，只是更改 blockState。
 */
void
RollbackToSavepoint(const char *name)
{
	TransactionState s = CurrentTransactionState;
	TransactionState target,
				xact;

	/*
	 * 工作进程在每个并行操作开始时同步事务状态，
	 * 所以我们在此之后无法处理事务状态更改。
	 * （注意，如果 s->blockState 是 TBLOCK_PARALLEL_INPROGRESS，
	 * 此检查肯定会出错，所以我们可以在下面将其视为无效情况。）
 *
 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot rollback to savepoints during a parallel operation")));

	switch (s->blockState)
	{
			/*
			 * 如果没有定义保存点，我们就无法回滚到保存点。
 *
 */
		case TBLOCK_INPROGRESS:
		case TBLOCK_ABORT:
			ereport(ERROR,
					(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
					 errmsg("savepoint \"%s\" does not exist", name)));
			break;

		case TBLOCK_IMPLICIT_INPROGRESS:
			/* 参见 DefineSavepoint 中关于隐式事务的注释 */
			ereport(ERROR,
					(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
			/* translator: %s represents an SQL statement name */
					 errmsg("%s can only be used in transaction blocks",
							"ROLLBACK TO SAVEPOINT")));
			break;

			/*
			 * 至少有一个保存点，所以继续处理。
			 */
		case TBLOCK_SUBINPROGRESS:
		case TBLOCK_SUBABORT:
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(FATAL, "RollbackToSavepoint: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	for (target = s; PointerIsValid(target); target = target->parent)
	{
		if (PointerIsValid(target->name) && strcmp(target->name, name) == 0)
			break;
	}

	if (!PointerIsValid(target))
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("savepoint \"%s\" does not exist", name)));

	/* 禁止跨越保存点级别边界 */
	if (target->savepointLevel != s->savepointLevel)
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("savepoint \"%s\" does not exist within current savepoint level", name)));

	/*
	 * 将目标子事务之前的所有子事务标记为"中止待定"。
	 * 实际的中止将在控制到达 CommitTransactionCommand 时发生。
 *
 */
	xact = CurrentTransactionState;
	for (;;)
	{
		if (xact == target)
			break;
		if (xact->blockState == TBLOCK_SUBINPROGRESS)
			xact->blockState = TBLOCK_SUBABORT_PENDING;
		else if (xact->blockState == TBLOCK_SUBABORT)
			xact->blockState = TBLOCK_SUBABORT_END;
		else
			elog(FATAL, "RollbackToSavepoint: unexpected state %s",
				 BlockStateAsString(xact->blockState));
		xact = xact->parent;
		Assert(PointerIsValid(xact));
	}

	/* 并将目标标记为"重启待定" */
	if (xact->blockState == TBLOCK_SUBINPROGRESS)
		xact->blockState = TBLOCK_SUBRESTART;
	else if (xact->blockState == TBLOCK_SUBABORT)
		xact->blockState = TBLOCK_SUBABORT_RESTART;
	else
		elog(FATAL, "RollbackToSavepoint: unexpected state %s",
			 BlockStateAsString(xact->blockState));
}

/*
 * BeginInternalSubTransaction
 *		这与 DefineSavepoint 相同，但它允许 TBLOCK_STARTED、
 *		TBLOCK_IMPLICIT_INPROGRESS、TBLOCK_END 和 TBLOCK_PREPARE 状态，
 *		因此可以安全地在可能在不处于 BEGIN 块内或在 COMMIT/PREPARE 时
 *		运行延迟触发器的函数中调用。此外，它自动执行
 *		CommitTransactionCommand/StartTransactionCommand，而不是期望
 *		调用者来做。
 *
 */
void
BeginInternalSubTransaction(const char *name)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 工作进程在每个并行操作开始时同步事务状态，
	 * 所以我们在此之后无法处理新的子事务。我们可能可以为此函数
	 * 建立的子事务类型做一个例外，这种子事务通常用于
	 * 在继续之前会释放或回滚子事务的上下文中，
	 * 因此不会对事务状态产生持久更改。
	 * 但目前，我们与其他所有情况一起禁止这种情况。
 *
 *
 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot start subtransactions during a parallel operation")));

	switch (s->blockState)
	{
		case TBLOCK_STARTED:
		case TBLOCK_INPROGRESS:
		case TBLOCK_IMPLICIT_INPROGRESS:
		case TBLOCK_END:
		case TBLOCK_PREPARE:
		case TBLOCK_SUBINPROGRESS:
			/* 正常的子事务启动 */
			PushTransaction();
			s = CurrentTransactionState;	/* 由 push 改变 */

			/*
			 * 保存点名称（像 TransactionState 块本身一样）
			 * 存储在 TopTransactionContext 中。
			 */
			if (name)
				s->name = MemoryContextStrdup(TopTransactionContext, name);
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
			elog(FATAL, "BeginInternalSubTransaction: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	CommitTransactionCommand();
	StartTransactionCommand();
}

/*
 * ReleaseCurrentSubTransaction
 *
 * RELEASE（即提交）最内层的子事务，无论其保存点名称是什么（如果有的话）。
 * 注意：不要与此一起使用 CommitTransactionCommand/StartTransactionCommand。
 *
 */
void
ReleaseCurrentSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 工作进程在每个并行操作开始时同步事务状态，
	 * 所以我们在此之后无法处理子事务的提交。这无论如何
	 * 不应该发生。调用此函数的代码通常先调用
	 * BeginInternalSubTransaction()，在那里失败。
 *
 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot commit subtransactions during a parallel operation")));

	if (s->blockState != TBLOCK_SUBINPROGRESS)
		elog(ERROR, "ReleaseCurrentSubTransaction: unexpected state %s",
			 BlockStateAsString(s->blockState));
	Assert(s->state == TRANS_INPROGRESS);
	MemoryContextSwitchTo(CurTransactionContext);
	CommitSubTransaction();
	s = CurrentTransactionState;	/* 由 pop 更改 */
	Assert(s->state == TRANS_INPROGRESS);
}

/*
 * RollbackAndReleaseCurrentSubTransaction
 *
 * ROLLBACK 和 RELEASE（即中止）最内层的子事务，无论其保存点名称是什么（如果有的话）。
 * 注意：不要与此一起使用 CommitTransactionCommand/StartTransactionCommand。
 *
 */
void
RollbackAndReleaseCurrentSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * 与 ReleaseCurrentSubTransaction() 不同，这在名义上
	 * 在并行操作期间是允许的。那是因为我们可能在领导者中，
	 * 从我们在并行模式时抛出的错误中恢复。我们不会在
	 * 工作进程中到达这里，因为 BeginInternalSubTransaction()
	 * 会失败。
	 */

	switch (s->blockState)
	{
			/* 必须处于子事务中 */
		case TBLOCK_SUBINPROGRESS:
		case TBLOCK_SUBABORT:
			break;

			/* 这些情况是无效的。 */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_IMPLICIT_INPROGRESS:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_INPROGRESS:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_ABORT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
		case TBLOCK_PREPARE:
			elog(FATAL, "RollbackAndReleaseCurrentSubTransaction: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	/*
	 * 如果需要，中止当前子事务。
	 */
	if (s->blockState == TBLOCK_SUBINPROGRESS)
		AbortSubTransaction();

	/* 并清理它 */
	CleanupSubTransaction();

	s = CurrentTransactionState;	/* 由 pop 更改 */
	Assert(s->blockState == TBLOCK_SUBINPROGRESS ||
		   s->blockState == TBLOCK_INPROGRESS ||
		   s->blockState == TBLOCK_IMPLICIT_INPROGRESS ||
		   s->blockState == TBLOCK_STARTED);
}

/*
 *	AbortOutOfAnyTransaction
 *
 *	此例程用于错误恢复目的。它中止任何活动的事务或事务块，
 *	使系统处于已知的空闲状态。
 *
 */
void
AbortOutOfAnyTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/* 确保我们不在注定要销毁的内存上下文中运行 */
	AtAbort_Memory();

	/*
	 * 退出任何事务或嵌套事务
	 */
	do
	{
		switch (s->blockState)
		{
			case TBLOCK_DEFAULT:
				if (s->state == TRANS_DEFAULT)
				{
					/* 不在事务中，什么都不做 */
				}
				else
				{
					/*
					 * 我们可能在事务启动期间出错后到达这里
					 * （状态将是 TRANS_START）。需要清理未完全启动的事务。
					 * 首先，调整底层状态以抑制 AbortTransaction 的警告消息。
 *
 *
 */
					if (s->state == TRANS_START)
						s->state = TRANS_INPROGRESS;
					AbortTransaction();
					CleanupTransaction();
				}
				break;
			case TBLOCK_STARTED:
			case TBLOCK_BEGIN:
			case TBLOCK_INPROGRESS:
			case TBLOCK_IMPLICIT_INPROGRESS:
			case TBLOCK_PARALLEL_INPROGRESS:
			case TBLOCK_END:
			case TBLOCK_ABORT_PENDING:
			case TBLOCK_PREPARE:
				/* 我们在事务中，所以清理 */
				AbortTransaction();
				CleanupTransaction();
				s->blockState = TBLOCK_DEFAULT;
				break;
			case TBLOCK_ABORT:
			case TBLOCK_ABORT_END:

				/*
				 * AbortTransaction 已完成，仍需要 Cleanup。
				 * 但是，如果我们在运行 ROLLBACK 的过程中失败，
				 * 将有一个运行该命令的活动 portal，我们需要
				 * 在执行 CleanupTransaction 之前关闭它。
				 */
				AtAbort_Portals();
				CleanupTransaction();
				s->blockState = TBLOCK_DEFAULT;
				break;

				/*
				 * 在子事务中，所以清理它并中止父事务
				 */
			case TBLOCK_SUBBEGIN:
			case TBLOCK_SUBINPROGRESS:
			case TBLOCK_SUBRELEASE:
			case TBLOCK_SUBCOMMIT:
			case TBLOCK_SUBABORT_PENDING:
			case TBLOCK_SUBRESTART:
				AbortSubTransaction();
				CleanupSubTransaction();
				s = CurrentTransactionState;	/* 由 pop 更改 */
				break;

			case TBLOCK_SUBABORT:
			case TBLOCK_SUBABORT_END:
			case TBLOCK_SUBABORT_RESTART:
				/* 同上，但 AbortSubTransaction 已完成 */
				if (s->curTransactionOwner)
				{
					/* 如 TBLOCK_ABORT，可能有一个活动的 portal 要清除 */
					AtSubAbort_Portals(s->subTransactionId,
									   s->parent->subTransactionId,
									   s->curTransactionOwner,
									   s->parent->curTransactionOwner);
				}
				CleanupSubTransaction();
				s = CurrentTransactionState;	/* 由 pop 更改 */
				break;
		}
	} while (s->blockState != TBLOCK_DEFAULT);

	/* 现在应该已经退出所有子事务了 */
	Assert(s->parent == NULL);

	/* 如果我们实际上没有什么可做的，恢复到 TopMemoryContext */
	AtCleanup_Memory();
}

/*
 * IsTransactionBlock --- 我们是否在事务块内？
 */
bool
IsTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_DEFAULT || s->blockState == TBLOCK_STARTED)
		return false;

	return true;
}

/*
 * IsTransactionOrTransactionBlock --- 我们是否在事务或事务块内？
 * （只有当此函数返回 false 时，后端才真正"空闲"。）
 *
 * 这应该与 IsTransactionBlock 和 IsTransactionState 匹配。
 *
 */
bool
IsTransactionOrTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_DEFAULT)
		return false;

	return true;
}

/*
 * TransactionBlockStatusCode - 返回在 ReadyForQuery 中发送的状态码
 */
char
TransactionBlockStatusCode(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
			return 'I';			/* 空闲 --- 不在事务中 */
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_INPROGRESS:
		case TBLOCK_IMPLICIT_INPROGRESS:
		case TBLOCK_PARALLEL_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
		case TBLOCK_END:
		case TBLOCK_SUBRELEASE:
		case TBLOCK_SUBCOMMIT:
		case TBLOCK_PREPARE:
			return 'T';			/* 在事务中 */
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
		case TBLOCK_ABORT_END:
		case TBLOCK_SUBABORT_END:
		case TBLOCK_ABORT_PENDING:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBRESTART:
		case TBLOCK_SUBABORT_RESTART:
			return 'E';			/* 在失败的事务中 */
	}

	/* 不应该到达这里 */
	elog(FATAL, "invalid transaction block state: %s",
		 BlockStateAsString(s->blockState));
	return 0;					/* 使编译器安静 */
}

/*
 * IsSubTransaction
 */
bool
IsSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->nestingLevel >= 2)
		return true;

	return false;
}

/*
 * StartSubTransaction
 *
 * 如果您想知道为什么这与 PushTransaction 分开：这是因为
 * 我们无法方便地在 DefineSavepoint 内部做这些事情。
 * SAVEPOINT 实用命令将在 Portal 内执行，如果我们
 * 搞乱 CurrentMemoryContext 或 CurrentResourceOwner，
 * 那么从 Portal 退出将撤销这些设置。所以我们让 DefineSavepoint
 * 只推送一个虚拟事务块，当控制返回到主空闲循环时，
 * CommitTransactionCommand 将被调用，我们将来到这里
 * 完成子事务的启动。
 */
static void
StartSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->state != TRANS_DEFAULT)
		elog(WARNING, "StartSubTransaction while in %s state",
			 TransStateAsString(s->state));

	s->state = TRANS_START;

	/*
	 * 为新子事务初始化子系统
	 *
	 * 必须首先初始化资源管理相关内容
	 */
	AtSubStart_Memory();
	AtSubStart_ResourceOwner();
	AfterTriggerBeginSubXact();

	s->state = TRANS_INPROGRESS;

	/*
	 * 调用子事务开始回调
	 */
	CallSubXactCallbacks(SUBXACT_EVENT_START_SUB, s->subTransactionId,
						 s->parent->subTransactionId);

	ShowTransactionState("StartSubTransaction");
}

/*
 * CommitSubTransaction
 *
 * 调用者必须确保在调用此函数后，
 * 如果有指向 CurrentTransactionState 的本地指针，总是重新赋值。
 */
static void
CommitSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	ShowTransactionState("CommitSubTransaction");

	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "CommitSubTransaction while in %s state",
			 TransStateAsString(s->state));

	/* 提交前处理放在这里 */

	CallSubXactCallbacks(SUBXACT_EVENT_PRE_COMMIT_SUB, s->subTransactionId,
						 s->parent->subTransactionId);

	/* 如果在并行模式中，清理工作进程并退出并行模式。 */
	if (IsInParallelMode())
	{
		AtEOSubXact_Parallel(true, s->subTransactionId);
		s->parallelModeLevel = 0;
	}

	/* 执行实际的"提交"，如此而已 */
	s->state = TRANS_COMMIT;

	/* 必须执行 CCI 以确保子事务的命令被视为已完成 */
	CommandCounterIncrement();

	/*
	 * 在 8.4 之前，我们在此处将子提交标记在 clog 中。现在我们只在顶层提交或中止时
	 * 作为整个事务树的原子更新的一部分执行该步骤（如果需要）。
 *
 */

	/* 提交后清理 */
	if (FullTransactionIdIsValid(s->fullTransactionId))
		AtSubCommit_childXids();
	AfterTriggerEndSubXact(true);
	AtSubCommit_Portals(s->subTransactionId,
						s->parent->subTransactionId,
						s->parent->nestingLevel,
						s->parent->curTransactionOwner);
	AtEOSubXact_LargeObject(true, s->subTransactionId,
							s->parent->subTransactionId);
	AtSubCommit_Notify();

	CallSubXactCallbacks(SUBXACT_EVENT_COMMIT_SUB, s->subTransactionId,
						 s->parent->subTransactionId);

	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 true, false);
	AtEOSubXact_RelationCache(true, s->subTransactionId,
							  s->parent->subTransactionId);
	AtEOSubXact_Inval(true);
	AtSubCommit_smgr();

	/*
	 * 我们这里实际释放的唯一锁是子事务 XID 锁。
	 */
	CurrentResourceOwner = s->curTransactionOwner;
	if (FullTransactionIdIsValid(s->fullTransactionId))
		XactLockTableDelete(XidFromFullTransactionId(s->fullTransactionId));

	/*
	 * 其他锁应该转移到其父资源所有者。
	 */
	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_LOCKS,
						 true, false);
	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 true, false);

	AtEOXact_GUC(true, s->gucNestLevel);
	AtEOSubXact_SPI(true, s->subTransactionId);
	AtEOSubXact_on_commit_actions(true, s->subTransactionId,
								  s->parent->subTransactionId);
	AtEOSubXact_Namespace(true, s->subTransactionId,
						  s->parent->subTransactionId);
	AtEOSubXact_Files(true, s->subTransactionId,
					  s->parent->subTransactionId);
	AtEOSubXact_HashTables(true, s->nestingLevel);
	AtEOSubXact_PgStat(true, s->nestingLevel);
	AtSubCommit_Snapshot(s->nestingLevel);

	/*
	 * 我们需要恢复上层事务的只读状态，以防上层是读写而子事务是只读；
	 * GUC 会错误地认为它应该保留子事务的状态。
 *
 */
	XactReadOnly = s->prevXactReadOnly;

	CurrentResourceOwner = s->parent->curTransactionOwner;
	CurTransactionResourceOwner = s->parent->curTransactionOwner;
	ResourceOwnerDelete(s->curTransactionOwner);
	s->curTransactionOwner = NULL;

	AtSubCommit_Memory();

	s->state = TRANS_DEFAULT;

	PopTransaction();
}

/*
 * AbortSubTransaction
 */
static void
AbortSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/* 在清理时阻止取消/死亡中断 */
	HOLD_INTERRUPTS();

	/* 确保我们有一个有效的内存上下文和资源所有者 */
	AtSubAbort_Memory();
	AtSubAbort_ResourceOwner();

	/*
	 * 尽可能快地释放我们可能持有的任何 LW 锁。（但是，常规锁必须保持到我们完成中止。）
	 * 释放 LW 锁至关重要，因为我们可能会在清理时再次尝试获取它们！
	 *
	 * FIXME 这可能不正确 --- 是否有一些我们应该保留的锁？
	 * 例如缓冲区锁？我不这么认为，但我不确定。
 *
 *
 */
	LWLockReleaseAll();

	pgstat_report_wait_end();
	pgstat_progress_end_command();
	UnlockBuffers();

	/* 重置 WAL 记录构造状态 */
	XLogResetInsertion();

	/* 取消条件变量睡眠 */
	ConditionVariableCancelSleep();

	/*
	 * 还要清理任何打开的锁等待，因为如果我们在此操作之前尝试等待另一个锁，
	 * 锁管理器会出问题。
	 */
	LockErrorCleanup();

	/*
	 * 如果有任何超时事件仍然活动，确保超时中断已安排。这覆盖了由于
	 * 从 SIGINT 处理程序 longjmp 而可能丢失超时中断的情况
	 * （参见 handle_sig_alarm 中的注释）。我们延迟到 LockErrorCleanup 之后，
	 * 以便我们不会无用地重新安排锁或死锁检查超时。
 *
 */
	reschedule_timeouts();

	/*
	 * 重新启用信号，以防我们通过从信号处理程序 longjmp 到达这里。
	 * 我们在序列中相当早地执行此操作，以便在中止时如果需要，
	 * 超时基础设施将正常工作。
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 检查当前事务状态
	 */
	ShowTransactionState("AbortSubTransaction");

	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "AbortSubTransaction while in %s state",
			 TransStateAsString(s->state));

	s->state = TRANS_ABORT;

	/*
	 * 重置可能已被临时更改的用户 ID。（参见 AbortTransaction 中的注释。）
 *
 */
	SetUserIdAndSecContext(s->prevUser, s->prevSecContext);

	/* 忘记任何活动的 REINDEX。 */
	ResetReindexState(s->nestingLevel);

	/* 重置逻辑流状态。 */
	ResetLogicalStreamingState();

	/*
	 * 这里不需要 SnapBuildResetExportedSnapshotState()，
	 * 子事务不支持快照导出。
	 */

	/* 如有必要，从并行模式退出。 */
	if (IsInParallelMode())
	{
		AtEOSubXact_Parallel(false, s->subTransactionId);
		s->parallelModeLevel = 0;
	}

	/*
	 * 如果子事务在创建 ResourceOwner 之前失败，
	 * 我们可以跳过所有这些内容...
	 */
	if (s->curTransactionOwner)
	{
		AfterTriggerEndSubXact(false);
		AtSubAbort_Portals(s->subTransactionId,
						   s->parent->subTransactionId,
						   s->curTransactionOwner,
						   s->parent->curTransactionOwner);
		AtEOSubXact_LargeObject(false, s->subTransactionId,
								s->parent->subTransactionId);
		AtSubAbort_Notify();

		/* 在 pg_xact 中公布我们中止的事实。 */
		(void) RecordTransactionAbort(true);

		/* 中止后清理 */
		if (FullTransactionIdIsValid(s->fullTransactionId))
			AtSubAbort_childXids();

		CallSubXactCallbacks(SUBXACT_EVENT_ABORT_SUB, s->subTransactionId,
							 s->parent->subTransactionId);

		ResourceOwnerRelease(s->curTransactionOwner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 false, false);
		AtEOSubXact_RelationCache(false, s->subTransactionId,
								  s->parent->subTransactionId);
		AtEOSubXact_Inval(false);
		ResourceOwnerRelease(s->curTransactionOwner,
							 RESOURCE_RELEASE_LOCKS,
							 false, false);
		ResourceOwnerRelease(s->curTransactionOwner,
							 RESOURCE_RELEASE_AFTER_LOCKS,
							 false, false);
		AtSubAbort_smgr();

		AtEOXact_GUC(false, s->gucNestLevel);
		AtEOSubXact_SPI(false, s->subTransactionId);
		AtEOSubXact_on_commit_actions(false, s->subTransactionId,
									  s->parent->subTransactionId);
		AtEOSubXact_Namespace(false, s->subTransactionId,
							  s->parent->subTransactionId);
		AtEOSubXact_Files(false, s->subTransactionId,
						  s->parent->subTransactionId);
		AtEOSubXact_HashTables(false, s->nestingLevel);
		AtEOSubXact_PgStat(false, s->nestingLevel);
		AtSubAbort_Snapshot(s->nestingLevel);
	}

	/*
	 * 恢复上层事务的只读状态。这应该与 GUC 的清理
	 * 重复，但为了与提交情况一致，我们也可以这样做。
 *
 */
	XactReadOnly = s->prevXactReadOnly;

	RESUME_INTERRUPTS();
}

/*
 * CleanupSubTransaction
 *
 * 调用者必须确保在调用此函数后，
 * 如果有指向 CurrentTransactionState 的本地指针，总是重新赋值。
 */
static void
CleanupSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	ShowTransactionState("CleanupSubTransaction");

	if (s->state != TRANS_ABORT)
		elog(WARNING, "CleanupSubTransaction while in %s state",
			 TransStateAsString(s->state));

	AtSubCleanup_Portals(s->subTransactionId);

	CurrentResourceOwner = s->parent->curTransactionOwner;
	CurTransactionResourceOwner = s->parent->curTransactionOwner;
	if (s->curTransactionOwner)
		ResourceOwnerDelete(s->curTransactionOwner);
	s->curTransactionOwner = NULL;

	AtSubCleanup_Memory();

	s->state = TRANS_DEFAULT;

	PopTransaction();
}

/*
 * PushTransaction
 *		为子事务创建事务状态栈条目
 *
 * 调用者必须确保在调用此函数后，
 * 如果有指向 CurrentTransactionState 的本地指针，总是重新赋值。
 */
static void
PushTransaction(void)
{
	TransactionState p = CurrentTransactionState;
	TransactionState s;

	/*
	 * 我们将子事务状态节点保存在 TopTransactionContext 中。
	 */
	s = (TransactionState)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(TransactionStateData));

	/*
	 * 分配一个子事务 ID，注意计数器回绕。
	 */
	currentSubTransactionId += 1;
	if (currentSubTransactionId == InvalidSubTransactionId)
	{
		currentSubTransactionId -= 1;
		pfree(s);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot have more than 2^32-1 subtransactions in a transaction")));
	}

	/*
	 * 我们现在可以堆叠一个最小有效的子事务，无需担心失败。
 *
 */
	s->fullTransactionId = InvalidFullTransactionId;	/* 直到分配 */
	s->subTransactionId = currentSubTransactionId;
	s->parent = p;
	s->nestingLevel = p->nestingLevel + 1;
	s->gucNestLevel = NewGUCNestLevel();
	s->savepointLevel = p->savepointLevel;
	s->state = TRANS_DEFAULT;
	s->blockState = TBLOCK_SUBBEGIN;
	GetUserIdAndSecContext(&s->prevUser, &s->prevSecContext);
	s->prevXactReadOnly = XactReadOnly;
	s->startedInRecovery = p->startedInRecovery;
	s->parallelModeLevel = 0;
	s->topXidLogged = false;

	CurrentTransactionState = s;

	/*
	 * AbortSubTransaction 和 CleanupSubTransaction 从现在开始必须能够
	 * 处理这个子事务；特别是它们不应假设它一定有事务上下文、
	 * 资源所有者或 XID。
 *
 */
}

/*
 * PopTransaction
 *		弹出回到父事务状态
 *
 * 调用者必须确保在调用此函数后，
 * 如果有指向 CurrentTransactionState 的本地指针，总是重新赋值。
 */
static void
PopTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->state != TRANS_DEFAULT)
		elog(WARNING, "PopTransaction while in %s state",
			 TransStateAsString(s->state));

	if (s->parent == NULL)
		elog(FATAL, "PopTransaction with no parent");

	CurrentTransactionState = s->parent;

	/* 让我们确保 CurTransactionContext 是好的 */
	CurTransactionContext = s->parent->curTransactionContext;
	MemoryContextSwitchTo(CurTransactionContext);

	/* ResourceOwner 链接也是如此 */
	CurTransactionResourceOwner = s->parent->curTransactionOwner;
	CurrentResourceOwner = s->parent->curTransactionOwner;

	/* 释放旧的子结构 */
	if (s->name)
		pfree(s->name);
	pfree(s);
}

/*
 * EstimateTransactionStateSpace
 *		估算 SerializeTransactionState 所需的空间量。
 *		稍微高估是可以的，但我们可以轻松计算出精确值，所以我们这样做。
 *
 */
Size
EstimateTransactionStateSpace(void)
{
	TransactionState s;
	Size		nxids = 0;
	Size		size = SerializedTransactionStateHeaderSize;

	for (s = CurrentTransactionState; s != NULL; s = s->parent)
	{
		if (FullTransactionIdIsValid(s->fullTransactionId))
			nxids = add_size(nxids, 1);
		nxids = add_size(nxids, s->nChildXids);
	}

	return add_size(size, mul_size(sizeof(TransactionId), nxids));
}

/*
 * SerializeTransactionState
 *		写出我们事务状态的相关细节，这些细节将被并行工作进程需要。
 *
 * 我们需要保存和恢复 XactDeferrable、XactIsoLevel 以及与此事务
 * 关联的 XIDs。这些被序列化到调用者提供的缓冲区中，该缓冲区
 * 大小足以容纳 EstimateTransactionStateSpace() 报告的字节数。
 * 我们按排序顺序发出 XID，以便接收进程方便处理。
 *
 *
 */
void
SerializeTransactionState(Size maxsize, char *start_address)
{
	TransactionState s;
	Size		nxids = 0;
	Size		i = 0;
	TransactionId *workspace;
	SerializedTransactionState *result;

	result = (SerializedTransactionState *) start_address;

	result->xactIsoLevel = XactIsoLevel;
	result->xactDeferrable = XactDeferrable;
	result->topFullTransactionId = XactTopFullTransactionId;
	result->currentFullTransactionId =
		CurrentTransactionState->fullTransactionId;
	result->currentCommandId = currentCommandId;

	/*
	 * 如果我们在并行工作进程中运行并启动自己的并行工作进程，
	 * 我们可以直接传递传递给我们的信息。
 *
 */
	if (nParallelCurrentXids > 0)
	{
		result->nParallelCurrentXids = nParallelCurrentXids;
		memcpy(&result->parallelCurrentXids[0], ParallelCurrentXids,
			   nParallelCurrentXids * sizeof(TransactionId));
		return;
	}

	/*
	 * 好的，我们需要生成一个工作进程应视为当前的 XIDs 排序列表。
	 * 首先，计算有多少个。
	 */
	for (s = CurrentTransactionState; s != NULL; s = s->parent)
	{
		if (FullTransactionIdIsValid(s->fullTransactionId))
			nxids = add_size(nxids, 1);
		nxids = add_size(nxids, s->nChildXids);
	}
	Assert(SerializedTransactionStateHeaderSize + nxids * sizeof(TransactionId)
		   <= maxsize);

	/* 将它们复制到我们的暂存空间。 */
	workspace = palloc(nxids * sizeof(TransactionId));
	for (s = CurrentTransactionState; s != NULL; s = s->parent)
	{
		if (FullTransactionIdIsValid(s->fullTransactionId))
			workspace[i++] = XidFromFullTransactionId(s->fullTransactionId);
		if (s->nChildXids > 0)
			memcpy(&workspace[i], s->childXids,
				   s->nChildXids * sizeof(TransactionId));
		i += s->nChildXids;
	}
	Assert(i == nxids);

	/* 对它们进行排序。 */
	qsort(workspace, nxids, sizeof(TransactionId), xidComparator);

	/* 将数据复制到输出区域。 */
	result->nParallelCurrentXids = nxids;
	memcpy(&result->parallelCurrentXids[0], workspace,
		   nxids * sizeof(TransactionId));
}

/*
 * StartParallelWorkerTransaction
 *		启动并行工作进程事务，恢复由 SerializeTransactionState
 *		序列化的事务状态。
 */
void
StartParallelWorkerTransaction(char *tstatespace)
{
	SerializedTransactionState *tstate;

	Assert(CurrentTransactionState->blockState == TBLOCK_DEFAULT);
	StartTransaction();

	tstate = (SerializedTransactionState *) tstatespace;
	XactIsoLevel = tstate->xactIsoLevel;
	XactDeferrable = tstate->xactDeferrable;
	XactTopFullTransactionId = tstate->topFullTransactionId;
	CurrentTransactionState->fullTransactionId =
		tstate->currentFullTransactionId;
	currentCommandId = tstate->currentCommandId;
	nParallelCurrentXids = tstate->nParallelCurrentXids;
	ParallelCurrentXids = &tstate->parallelCurrentXids[0];

	CurrentTransactionState->blockState = TBLOCK_PARALLEL_INPROGRESS;
}

/*
 * EndParallelWorkerTransaction
 *		结束并行工作进程事务。
 */
void
EndParallelWorkerTransaction(void)
{
	Assert(CurrentTransactionState->blockState == TBLOCK_PARALLEL_INPROGRESS);
	CommitTransaction();
	CurrentTransactionState->blockState = TBLOCK_DEFAULT;
}

/*
 * ShowTransactionState
 *		调试支持
 */
static void
ShowTransactionState(const char *str)
{
	/* 如果消息肯定不会被打印，则跳过工作 */
	if (message_level_is_interesting(DEBUG5))
		ShowTransactionStateRec(str, CurrentTransactionState);
}

/*
 * ShowTransactionStateRec
 *		ShowTransactionState 的递归子例程
 */
static void
ShowTransactionStateRec(const char *str, TransactionState s)
{
	StringInfoData buf;

	initStringInfo(&buf);

	if (s->nChildXids > 0)
	{
		int			i;

		appendStringInfo(&buf, ", children: %u", s->childXids[0]);
		for (i = 1; i < s->nChildXids; i++)
			appendStringInfo(&buf, " %u", s->childXids[i]);
	}

	if (s->parent)
		ShowTransactionStateRec(str, s->parent);

	ereport(DEBUG5,
			(errmsg_internal("%s(%d) name: %s; blockState: %s; state: %s, xid/subid/cid: %u/%u/%u%s%s",
							 str, s->nestingLevel,
							 PointerIsValid(s->name) ? s->name : "unnamed",
							 BlockStateAsString(s->blockState),
							 TransStateAsString(s->state),
							 (unsigned int) XidFromFullTransactionId(s->fullTransactionId),
							 (unsigned int) s->subTransactionId,
							 (unsigned int) currentCommandId,
							 currentCommandIdUsed ? " (used)" : "",
							 buf.data)));

	pfree(buf.data);
}

/*
 * BlockStateAsString
 *		调试支持
 */
static const char *
BlockStateAsString(TBlockState blockState)
{
	switch (blockState)
	{
		case TBLOCK_DEFAULT:
			return "DEFAULT";
		case TBLOCK_STARTED:
			return "STARTED";
		case TBLOCK_BEGIN:
			return "BEGIN";
		case TBLOCK_INPROGRESS:
			return "INPROGRESS";
		case TBLOCK_IMPLICIT_INPROGRESS:
			return "IMPLICIT_INPROGRESS";
		case TBLOCK_PARALLEL_INPROGRESS:
			return "PARALLEL_INPROGRESS";
		case TBLOCK_END:
			return "END";
		case TBLOCK_ABORT:
			return "ABORT";
		case TBLOCK_ABORT_END:
			return "ABORT_END";
		case TBLOCK_ABORT_PENDING:
			return "ABORT_PENDING";
		case TBLOCK_PREPARE:
			return "PREPARE";
		case TBLOCK_SUBBEGIN:
			return "SUBBEGIN";
		case TBLOCK_SUBINPROGRESS:
			return "SUBINPROGRESS";
		case TBLOCK_SUBRELEASE:
			return "SUBRELEASE";
		case TBLOCK_SUBCOMMIT:
			return "SUBCOMMIT";
		case TBLOCK_SUBABORT:
			return "SUBABORT";
		case TBLOCK_SUBABORT_END:
			return "SUBABORT_END";
		case TBLOCK_SUBABORT_PENDING:
			return "SUBABORT_PENDING";
		case TBLOCK_SUBRESTART:
			return "SUBRESTART";
		case TBLOCK_SUBABORT_RESTART:
			return "SUBABORT_RESTART";
	}
	return "UNRECOGNIZED";
}

/*
 * TransStateAsString
 *		调试支持
 */
static const char *
TransStateAsString(TransState state)
{
	switch (state)
	{
		case TRANS_DEFAULT:
			return "DEFAULT";
		case TRANS_START:
			return "START";
		case TRANS_INPROGRESS:
			return "INPROGRESS";
		case TRANS_COMMIT:
			return "COMMIT";
		case TRANS_ABORT:
			return "ABORT";
		case TRANS_PREPARE:
			return "PREPARE";
	}
	return "UNRECOGNIZED";
}

/*
 * xactGetCommittedChildren
 *
 * 获取当前事务的已提交子事务列表。返回值是子事务的数量。
 * *ptr 设置为指向一个 TransactionIds 数组。该数组在 TopTransactionContext 中分配；
 * 调用者*不应* pfree() 它（这是与 8.4 之前代码的区别！）。
 * 如果没有子事务，*ptr 设置为 NULL。
 *
 */
int
xactGetCommittedChildren(TransactionId **ptr)
{
	TransactionState s = CurrentTransactionState;

	if (s->nChildXids == 0)
		*ptr = NULL;
	else
		*ptr = s->childXids;

	return s->nChildXids;
}

/*
 *	XLOG 支持例程
 */


/*
 * 为普通或两阶段事务提交记录提交日志。
 *
 * 当 twophase_xid 有效时发出 2pc 提交，否则发出普通提交。
 *
 */
XLogRecPtr
XactLogCommitRecord(TimestampTz commit_time,
					int nsubxacts, TransactionId *subxacts,
					int nrels, RelFileLocator *rels,
					int ndroppedstats, xl_xact_stats_item *droppedstats,
					int nmsgs, SharedInvalidationMessage *msgs,
					bool relcacheInval,
					int xactflags, TransactionId twophase_xid,
					const char *twophase_gid)
{
	xl_xact_commit xlrec;
	xl_xact_xinfo xl_xinfo;
	xl_xact_dbinfo xl_dbinfo;
	xl_xact_subxacts xl_subxacts;
	xl_xact_relfilelocators xl_relfilelocators;
	xl_xact_stats_items xl_dropped_stats;
	xl_xact_invals xl_invals;
	xl_xact_twophase xl_twophase;
	xl_xact_origin xl_origin;
	uint8		info;

	Assert(CritSectionCount > 0);

	xl_xinfo.xinfo = 0;

	/* 在普通提交和 2pc 提交之间决定 */
	if (!TransactionIdIsValid(twophase_xid))
		info = XLOG_XACT_COMMIT;
	else
		info = XLOG_XACT_COMMIT_PREPARED;

	/* 首先计算并收集所有需要的信息 */

	xlrec.xact_time = commit_time;

	if (relcacheInval)
		xl_xinfo.xinfo |= XACT_COMPLETION_UPDATE_RELCACHE_FILE;
	if (forceSyncCommit)
		xl_xinfo.xinfo |= XACT_COMPLETION_FORCE_SYNC_COMMIT;
	if ((xactflags & XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK))
		xl_xinfo.xinfo |= XACT_XINFO_HAS_AE_LOCKS;

	/*
	 * 检查调用者是否希望在此提交应用后立即请求备库反馈。
 *
 */
	if (synchronous_commit >= SYNCHRONOUS_COMMIT_REMOTE_APPLY)
		xl_xinfo.xinfo |= XACT_COMPLETION_APPLY_FEEDBACK;

	/*
	 * 关系缓存失效需要有关当前数据库的信息，逻辑解码也是如此。
 *
 */
	if (nmsgs > 0 || XLogLogicalInfoActive())
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_DBINFO;
		xl_dbinfo.dbId = MyDatabaseId;
		xl_dbinfo.tsId = MyDatabaseTableSpace;
	}

	if (nsubxacts > 0)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_SUBXACTS;
		xl_subxacts.nsubxacts = nsubxacts;
	}

	if (nrels > 0)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_RELFILELOCATORS;
		xl_relfilelocators.nrels = nrels;
		info |= XLR_SPECIAL_REL_UPDATE;
	}

	if (ndroppedstats > 0)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_DROPPED_STATS;
		xl_dropped_stats.nitems = ndroppedstats;
	}

	if (nmsgs > 0)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_INVALS;
		xl_invals.nmsgs = nmsgs;
	}

	if (TransactionIdIsValid(twophase_xid))
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_TWOPHASE;
		xl_twophase.xid = twophase_xid;
		Assert(twophase_gid != NULL);

		if (XLogLogicalInfoActive())
			xl_xinfo.xinfo |= XACT_XINFO_HAS_GID;
	}

	/* 转储事务源信息 */
	if (replorigin_session_origin != InvalidRepOriginId)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_ORIGIN;

		xl_origin.origin_lsn = replorigin_session_origin_lsn;
		xl_origin.origin_timestamp = replorigin_session_origin_timestamp;
	}

	if (xl_xinfo.xinfo != 0)
		info |= XLOG_XACT_HAS_INFO;

	/* 然后将所有收集的数据包含到提交记录中。 */

	XLogBeginInsert();

	XLogRegisterData((char *) (&xlrec), sizeof(xl_xact_commit));

	if (xl_xinfo.xinfo != 0)
		XLogRegisterData((char *) (&xl_xinfo.xinfo), sizeof(xl_xinfo.xinfo));

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_DBINFO)
		XLogRegisterData((char *) (&xl_dbinfo), sizeof(xl_dbinfo));

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_SUBXACTS)
	{
		XLogRegisterData((char *) (&xl_subxacts),
						 MinSizeOfXactSubxacts);
		XLogRegisterData((char *) subxacts,
						 nsubxacts * sizeof(TransactionId));
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_RELFILELOCATORS)
	{
		XLogRegisterData((char *) (&xl_relfilelocators),
						 MinSizeOfXactRelfileLocators);
		XLogRegisterData((char *) rels,
						 nrels * sizeof(RelFileLocator));
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_DROPPED_STATS)
	{
		XLogRegisterData((char *) (&xl_dropped_stats),
						 MinSizeOfXactStatsItems);
		XLogRegisterData((char *) droppedstats,
						 ndroppedstats * sizeof(xl_xact_stats_item));
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_INVALS)
	{
		XLogRegisterData((char *) (&xl_invals), MinSizeOfXactInvals);
		XLogRegisterData((char *) msgs,
						 nmsgs * sizeof(SharedInvalidationMessage));
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_TWOPHASE)
	{
		XLogRegisterData((char *) (&xl_twophase), sizeof(xl_xact_twophase));
		if (xl_xinfo.xinfo & XACT_XINFO_HAS_GID)
			XLogRegisterData(unconstify(char *, twophase_gid), strlen(twophase_gid) + 1);
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_ORIGIN)
		XLogRegisterData((char *) (&xl_origin), sizeof(xl_xact_origin));

	/* 我们允许按事务过滤 */
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	return XLogInsert(RM_XACT_ID, info);
}

/*
 * 为普通或两阶段事务中止记录提交日志。
 *
 * 当 twophase_xid 有效时发出 2pc 中止，否则发出普通中止。
 *
 */
XLogRecPtr
XactLogAbortRecord(TimestampTz abort_time,
				   int nsubxacts, TransactionId *subxacts,
				   int nrels, RelFileLocator *rels,
				   int ndroppedstats, xl_xact_stats_item *droppedstats,
				   int xactflags, TransactionId twophase_xid,
				   const char *twophase_gid)
{
	xl_xact_abort xlrec;
	xl_xact_xinfo xl_xinfo;
	xl_xact_subxacts xl_subxacts;
	xl_xact_relfilelocators xl_relfilelocators;
	xl_xact_stats_items xl_dropped_stats;
	xl_xact_twophase xl_twophase;
	xl_xact_dbinfo xl_dbinfo;
	xl_xact_origin xl_origin;

	uint8		info;

	Assert(CritSectionCount > 0);

	xl_xinfo.xinfo = 0;

	/* 在普通中止和 2pc 中止之间决定 */
	if (!TransactionIdIsValid(twophase_xid))
		info = XLOG_XACT_ABORT;
	else
		info = XLOG_XACT_ABORT_PREPARED;


	/* 首先计算并收集所有需要的信息 */

	xlrec.xact_time = abort_time;

	if ((xactflags & XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK))
		xl_xinfo.xinfo |= XACT_XINFO_HAS_AE_LOCKS;

	if (nsubxacts > 0)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_SUBXACTS;
		xl_subxacts.nsubxacts = nsubxacts;
	}

	if (nrels > 0)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_RELFILELOCATORS;
		xl_relfilelocators.nrels = nrels;
		info |= XLR_SPECIAL_REL_UPDATE;
	}

	if (ndroppedstats > 0)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_DROPPED_STATS;
		xl_dropped_stats.nitems = ndroppedstats;
	}

	if (TransactionIdIsValid(twophase_xid))
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_TWOPHASE;
		xl_twophase.xid = twophase_xid;
		Assert(twophase_gid != NULL);

		if (XLogLogicalInfoActive())
			xl_xinfo.xinfo |= XACT_XINFO_HAS_GID;
	}

	if (TransactionIdIsValid(twophase_xid) && XLogLogicalInfoActive())
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_DBINFO;
		xl_dbinfo.dbId = MyDatabaseId;
		xl_dbinfo.tsId = MyDatabaseTableSpace;
	}

	/*
	 * 转储事务源信息。我们在恢复期间需要这个来更新复制源进度。
 *
 */
	if (replorigin_session_origin != InvalidRepOriginId)
	{
		xl_xinfo.xinfo |= XACT_XINFO_HAS_ORIGIN;

		xl_origin.origin_lsn = replorigin_session_origin_lsn;
		xl_origin.origin_timestamp = replorigin_session_origin_timestamp;
	}

	if (xl_xinfo.xinfo != 0)
		info |= XLOG_XACT_HAS_INFO;

	/* 然后将所有收集的数据包含到中止记录中。 */

	XLogBeginInsert();

	XLogRegisterData((char *) (&xlrec), MinSizeOfXactAbort);

	if (xl_xinfo.xinfo != 0)
		XLogRegisterData((char *) (&xl_xinfo), sizeof(xl_xinfo));

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_DBINFO)
		XLogRegisterData((char *) (&xl_dbinfo), sizeof(xl_dbinfo));

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_SUBXACTS)
	{
		XLogRegisterData((char *) (&xl_subxacts),
						 MinSizeOfXactSubxacts);
		XLogRegisterData((char *) subxacts,
						 nsubxacts * sizeof(TransactionId));
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_RELFILELOCATORS)
	{
		XLogRegisterData((char *) (&xl_relfilelocators),
						 MinSizeOfXactRelfileLocators);
		XLogRegisterData((char *) rels,
						 nrels * sizeof(RelFileLocator));
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_DROPPED_STATS)
	{
		XLogRegisterData((char *) (&xl_dropped_stats),
						 MinSizeOfXactStatsItems);
		XLogRegisterData((char *) droppedstats,
						 ndroppedstats * sizeof(xl_xact_stats_item));
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_TWOPHASE)
	{
		XLogRegisterData((char *) (&xl_twophase), sizeof(xl_xact_twophase));
		if (xl_xinfo.xinfo & XACT_XINFO_HAS_GID)
			XLogRegisterData(unconstify(char *, twophase_gid), strlen(twophase_gid) + 1);
	}

	if (xl_xinfo.xinfo & XACT_XINFO_HAS_ORIGIN)
		XLogRegisterData((char *) (&xl_origin), sizeof(xl_xact_origin));

	/* 包含复制源 */
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	return XLogInsert(RM_XACT_ID, info);
}

/*
 * 在 9.0 之前这是一个相当短的函数，但现在它执行许多操作，
 * 这些操作的执行顺序至关重要。
 */
static void
xact_redo_commit(xl_xact_parsed_commit *parsed,
				 TransactionId xid,
				 XLogRecPtr lsn,
				 RepOriginId origin_id)
{
	TransactionId max_xid;
	TimestampTz commit_time;

	Assert(TransactionIdIsValid(xid));

	max_xid = TransactionIdLatest(xid, parsed->nsubxacts, parsed->subxacts);

	/* 确保 nextXid 超过记录中提到的任何 XID。 */
	AdvanceNextFullTransactionIdPastXid(max_xid);

	Assert(((parsed->xinfo & XACT_XINFO_HAS_ORIGIN) == 0) ==
		   (origin_id == InvalidRepOriginId));

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
		commit_time = parsed->origin_timestamp;
	else
		commit_time = parsed->xact_time;

	/* 设置事务提交时间戳和元数据 */
	TransactionTreeSetCommitTsData(xid, parsed->nsubxacts, parsed->subxacts,
								   commit_time, origin_id);

	if (standbyState == STANDBY_DISABLED)
	{
		/*
		 * 在 pg_xact 中将事务标记为已提交。
		 */
		TransactionIdCommitTree(xid, parsed->nsubxacts, parsed->subxacts);
	}
	else
	{
		/*
		 * 如果事务完成记录到达时尚有未观察到的子事务，
		 * 那么这在 xlog.c 主恢复循环中的 RecordKnownAssignedTransactionIds()
		 * 调用中不会被完全处理。所以我们需要再次进行记账以覆盖这种情况。
		 * 这很令人困惑，很容易认为此调用无关紧要，
		 * 这在开发中已经发生过三次了。保留它。
 *
 *
 */
		RecordKnownAssignedTransactionIds(max_xid);

		/*
		 * 在 pg_xact 中将事务标记为已提交。我们在恢复期间使用异步提交协议，
		 * 以便在用户尝试设置提示位时提供有关数据库一致性的信息。
		 * 重要的是我们不要设置提示位，直到 minRecoveryPoint 超过此提交记录。
		 * 这确保如果我们崩溃，我们不会看到由尚未恢复的事务
		 * 所做更改的提示位已设置。这不太可能，但安全是好的。
 *
 *
 */
		TransactionIdAsyncCommitTree(xid, parsed->nsubxacts, parsed->subxacts, lsn);

		/*
		 * 我们必须在更新 ProcArray 之前标记 clog。
		 */
		ExpireTreeKnownAssignedTransactionIds(xid, parsed->nsubxacts, parsed->subxacts, max_xid);

		/*
		 * 发送附加到提交的任何缓存失效。我们必须保持与
		 * CommitTransaction() 中相同的失效然后释放锁的顺序。
 *
 */
		ProcessCommittedInvalidationMessages(parsed->msgs, parsed->nmsgs,
											 XactCompletionRelcacheInitFileInval(parsed->xinfo),
											 parsed->dbId, parsed->tsId);

		/*
		 * 释放锁（如果有的话）。我们对两阶段和普通一阶段事务都这样做。
		 * 实际上我们忽略了预备阶段，直接进行锁释放。
 *
 */
		if (parsed->xinfo & XACT_XINFO_HAS_AE_LOCKS)
			StandbyReleaseLockTree(xid, parsed->nsubxacts, parsed->subxacts);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		/* 恢复应用进度 */
		replorigin_advance(origin_id, parsed->origin_lsn, lsn,
						   false /* backward */ , false /* WAL */ );
	}

	/* 确保应该被删除的文件被删除 */
	if (parsed->nrels > 0)
	{
		/*
		 * 首先更新最小恢复点以覆盖此 WAL 记录。一旦关系被删除，
		 * 就无法回头。缓冲区管理器对关系文件的正常更新强制执行
		 * WAL 优先规则，以便在将数据文件中的相应更改刷新到磁盘之前
		 * 始终更新最小恢复点，但由于我们绕过了缓冲区管理器，
		 * 我们必须在这里做同样的事情。
		 *
		 * 在删除文件之前这样做意味着如果删除由于某种原因失败，
		 * 您将无法启动系统，即使在重新启动后也不行，
		 * 直到您修复底层情况以便删除成功。或者，
		 * 我们可以在删除后更新最小恢复点，但这会留下一个小的窗口，
		 * 在该窗口中违反 WAL 优先规则。
 *
 *
 */
		XLogFlush(lsn);

		/* 确保应该被删除的文件被删除 */
		DropRelationFiles(parsed->xlocators, parsed->nrels, true);
	}

	if (parsed->nstats > 0)
	{
		/* 参见上面关系的等效调用 */
		XLogFlush(lsn);

		pgstat_execute_transactional_drops(parsed->nstats, parsed->stats, true);
	}

	/*
	 * 我们发出 XLogFlush() 的原因与我们在正常操作中发出 ForceSyncCommit() 相同。
	 * 例如，在 CREATE DATABASE 中，我们从模板数据库复制所有文件，
	 * 然后提交事务。如果我们在复制了所有文件但提交之前崩溃，
	 * 您将在数据目录中拥有没有 pg_database 条目的文件。
	 * 为了最小化该窗口，我们使用 ForceSyncCommit() 尽快将提交记录刷新到磁盘。
	 * 我们在恢复期间有相同的窗口，强制 XLogFlush()（在恢复期间更新 minRecoveryPoint）
	 * 有助于减少该问题窗口，对于任何请求 ForceSyncCommit() 的用户。
 *
 *
 *
 */
	if (XactCompletionForceSyncCommit(parsed->xinfo))
		XLogFlush(lsn);

	/*
	 * 如果主库请求（因为有人在等待 synchronous_commit = remote_apply），
	 * 我们将需要请求 walreceiver 立即发送回复。
 *
 */
	if (XactCompletionApplyFeedback(parsed->xinfo))
		XLogRequestWalReceiverReply();
}

/*
 * 注意执行顺序，与 xact_redo_commit() 一样。
 * 这两个函数类似但在关键地方不同。
 *
 * 还要注意，中止可以是子事务及其子事务的中止，
 * 而不仅仅是顶层中止。这意味着我们必须考虑
 * topxid != xid，而在提交中我们总是会发现 topxid == xid，
 * 因为子事务提交从不进行 WAL 日志记录。
 */
static void
xact_redo_abort(xl_xact_parsed_abort *parsed, TransactionId xid,
				XLogRecPtr lsn, RepOriginId origin_id)
{
	TransactionId max_xid;

	Assert(TransactionIdIsValid(xid));

	/* 确保 nextXid 超过记录中提到的任何 XID。 */
	max_xid = TransactionIdLatest(xid,
								  parsed->nsubxacts,
								  parsed->subxacts);
	AdvanceNextFullTransactionIdPastXid(max_xid);

	if (standbyState == STANDBY_DISABLED)
	{
		/* 在 pg_xact 中将事务标记为已中止，不需要异步处理 */
		TransactionIdAbortTree(xid, parsed->nsubxacts, parsed->subxacts);
	}
	else
	{
		/*
		 * 如果事务完成记录到达时尚有未观察到的子事务，
		 * 那么这在 xlog.c 主恢复循环中的 RecordKnownAssignedTransactionIds()
		 * 调用中不会被完全处理。所以我们需要再次进行记账以覆盖这种情况。
		 * 这很令人困惑，很容易认为此调用无关紧要，
		 * 这在开发中已经发生过三次了。保留它。
 *
 *
 */
		RecordKnownAssignedTransactionIds(max_xid);

		/* 在 pg_xact 中将事务标记为已中止，不需要异步处理 */
		TransactionIdAbortTree(xid, parsed->nsubxacts, parsed->subxacts);

		/*
		 * 我们必须在标记 clog 之后更新 ProcArray。
		 */
		ExpireTreeKnownAssignedTransactionIds(xid, parsed->nsubxacts, parsed->subxacts, max_xid);

		/*
		 * 没有要发送或撤销的失效消息。
		 */

		/*
		 * 释放锁（如果有的话）。没有要发送的失效消息。
		 */
		if (parsed->xinfo & XACT_XINFO_HAS_AE_LOCKS)
			StandbyReleaseLockTree(xid, parsed->nsubxacts, parsed->subxacts);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		/* 恢复应用进度 */
		replorigin_advance(origin_id, parsed->origin_lsn, lsn,
						   false /* backward */ , false /* WAL */ );
	}

	/* 确保应该被删除的文件被删除 */
	if (parsed->nrels > 0)
	{
		/*
		 * 参见 xact_redo_commit() 中关于截断时更新最小恢复点的注释。
 *
 */
		XLogFlush(lsn);

		DropRelationFiles(parsed->xlocators, parsed->nrels, true);
	}

	if (parsed->nstats > 0)
	{
		/* 参见上面关系的等效调用 */
		XLogFlush(lsn);

		pgstat_execute_transactional_drops(parsed->nstats, parsed->stats, true);
	}
}

void
xact_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;

	/* 备份块不用于 xact 记录 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_XACT_COMMIT)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(record);
		xl_xact_parsed_commit parsed;

		ParseCommitRecord(XLogRecGetInfo(record), xlrec, &parsed);
		xact_redo_commit(&parsed, XLogRecGetXid(record),
						 record->EndRecPtr, XLogRecGetOrigin(record));
	}
	else if (info == XLOG_XACT_COMMIT_PREPARED)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(record);
		xl_xact_parsed_commit parsed;

		ParseCommitRecord(XLogRecGetInfo(record), xlrec, &parsed);
		xact_redo_commit(&parsed, parsed.twophase_xid,
						 record->EndRecPtr, XLogRecGetOrigin(record));

		/* 删除 TwoPhaseState gxact 条目和/或 2PC 文件。 */
		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
		PrepareRedoRemove(parsed.twophase_xid, false);
		LWLockRelease(TwoPhaseStateLock);
	}
	else if (info == XLOG_XACT_ABORT)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(record);
		xl_xact_parsed_abort parsed;

		ParseAbortRecord(XLogRecGetInfo(record), xlrec, &parsed);
		xact_redo_abort(&parsed, XLogRecGetXid(record),
						record->EndRecPtr, XLogRecGetOrigin(record));
	}
	else if (info == XLOG_XACT_ABORT_PREPARED)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(record);
		xl_xact_parsed_abort parsed;

		ParseAbortRecord(XLogRecGetInfo(record), xlrec, &parsed);
		xact_redo_abort(&parsed, parsed.twophase_xid,
						record->EndRecPtr, XLogRecGetOrigin(record));

		/* 删除 TwoPhaseState gxact 条目和/或 2PC 文件。 */
		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
		PrepareRedoRemove(parsed.twophase_xid, false);
		LWLockRelease(TwoPhaseStateLock);
	}
	else if (info == XLOG_XACT_PREPARE)
	{
		/*
		 * 将 WAL 记录的 xid 和起始/结束指针存储在 TwoPhaseState gxact 条目中。
 *
 */
		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
		PrepareRedoAdd(XLogRecGetData(record),
					   record->ReadRecPtr,
					   record->EndRecPtr,
					   XLogRecGetOrigin(record));
		LWLockRelease(TwoPhaseStateLock);
	}
	else if (info == XLOG_XACT_ASSIGNMENT)
	{
		xl_xact_assignment *xlrec = (xl_xact_assignment *) XLogRecGetData(record);

		if (standbyState >= STANDBY_INITIALIZED)
			ProcArrayApplyXidAssignment(xlrec->xtop,
										xlrec->nsubxacts, xlrec->xsub);
	}
	else if (info == XLOG_XACT_INVALIDATIONS)
	{
		/*
		 * XXX 我们目前忽略这个，重要的是写入提交记录的失效消息。
 *
 */
	}
	else
		elog(PANIC, "xact_redo: unknown op code %u", info);
}
