/*-------------------------------------------------------------------------
 *
 * snapmgr.c
 *		PostgreSQL 快照管理器
 *
 * 我们以两种方式跟踪快照：由 resowner.c"注册"的快照，
 * 以及"活动快照"栈。这两者中的快照都存放在持久内存
 * 中。当快照不再属于这些列表（由每个快照各自的引
 * 用计数分别跟踪）时，其内存就可以被释放。
 *
 * 第一个 FirstXactSnapshot（如果有）待遇稍特别：我们
 * 递增其 regd_count 并将其列入 RegisteredSnapshots，但
 * 这个引用不被资源所有者跟踪。我们曾用
 * TopTransactionResourceOwner 来跟踪该快照引用，但那
 * 引入了逻辑上的循环，因而无法以合理的方式清理。更
 * 好的做法是把这一引用当作内部跟踪的注册来处理，使
 * 本模块的层次完全低于 ResourceOwners。
 *
 * 同样，凡被 pg_export_snapshot 导出的快照也是
 * regd_count = 1 且列入 RegisteredSnapshots，但不被任
 * 何资源所有者跟踪。
 *
 * 同样，CatalogSnapshot 在有效时会列入
 * RegisteredSnapshots，不被资源所有者跟踪
 *
 * 逻辑解码期间使用的历史快照也是如此，
 * 其生命周期单独管理（寿命比单个 xact.c 事务
 * 更长）。
 *
 * 这些安排让我们能在没有本事务引用的快照时重置
 * MyProc->xmin，并在最老 Xmin 的快照不再被引用时推进
 * 它。不过为简单起见，只有已注册快照参与
 * 追踪谁最老；活动快照不参与。这样我们就不需要跟踪
 * 哪个快照最老。只有活动快照栈为空时才会尝试去
 * 改它。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/snapmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "datatype/timestamp.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "port/pg_lfind.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/old_snapshot.h"
#include "utils/rel.h"
#include "utils/resowner_private.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/*
 * GUC 参数
 */
int			old_snapshot_threshold; /* 分钟数；-1 表示禁用 */

volatile OldSnapshotControlData *oldSnapshotControl;


/*
 * CurrentSnapshot 指向事务快照模式下取得的唯一快照，
 * 以及读已提交事务中取得的最新快照。
 * SecondarySnapshot 则是始终与当前时刻同步更新的快
 * 照，即使在事务快照模式下也如此。它只应被特殊用
 * 途的代码使用（比如说 RI 检查）。CatalogSnapshot 指
 * 向一个用于系统目录扫描的 MVCC 快照；每当系统目录
 * 发生变化时都必须使其失效。
 *
 * 这些 SnapshotData 结构体是静态的，以简化内存分配
 * （见 GetSnapshotData 里避免反复 malloc/free 的技巧）
 */
static SnapshotData CurrentSnapshotData = {SNAPSHOT_MVCC};
static SnapshotData SecondarySnapshotData = {SNAPSHOT_MVCC};
SnapshotData CatalogSnapshotData = {SNAPSHOT_MVCC};
SnapshotData SnapshotSelfData = {SNAPSHOT_SELF};
SnapshotData SnapshotAnyData = {SNAPSHOT_ANY};

/* 有效快照的指针 */
static Snapshot CurrentSnapshot = NULL;
static Snapshot SecondarySnapshot = NULL;
static Snapshot CatalogSnapshot = NULL;
static Snapshot HistoricSnapshot = NULL;

/*
 * 这些由 GetSnapshotData 更新。这样初始化是为了方
 * 便 TransactionIdIsInProgress：即使在引导模式下，
 * 我们也不希望它说 BootstrapTransactionId 正在进行中。
 */
TransactionId TransactionXmin = FirstNormalTransactionId;
TransactionId RecentXmin = FirstNormalTransactionId;

/* timetravel 的 (table,ctid)=>(cmin,cmax) 映射 */
static HTAB *tuplecid_data = NULL;

/*
 * 活动快照栈的元素。
 *
 * 这里的每个元素对应 SnapshotData 上的一个 active_count
 *
 * 注意：代码假定此列表中的元素按 as_level 非递增顺
 * 序排列；且列表必须以 NULL 结尾。
 */
typedef struct ActiveSnapshotElt
{
	Snapshot	as_snap;
	int			as_level;
	struct ActiveSnapshotElt *as_next;
} ActiveSnapshotElt;

/* 活动快照栈的栈顶 */
static ActiveSnapshotElt *ActiveSnapshot = NULL;

/* 活动快照栈的栈底 */
static ActiveSnapshotElt *OldestActiveSnapshot = NULL;

/*
 * 当前已注册的快照。按 xmin 排成堆，以便能快速找到
 * xmin 最小的那个，用来推进 MyProc->xmin。
 */
static int	xmin_cmp(const pairingheap_node *a, const pairingheap_node *b,
					 void *arg);

static pairingheap RegisteredSnapshots = {&xmin_cmp, NULL, NULL};

/* 事务内第一次 GetTransactionSnapshot 调用？ */
bool		FirstSnapshotSet = false;

/*
 * 记住可串行化事务快照（如果有）。我们不能在
 * IsolationUsesXactSnapshot() 之下信任 FirstSnapshotSet，
 * 因为 GUC 可能先于我们被重置并改变 IsolationUsesXactSnapshot
 */
static Snapshot FirstXactSnapshot = NULL;

/* 定义导出快照文件的路径名 */
#define SNAPSHOT_EXPORT_DIR "pg_snapshots"

/* 保存导出快照信息的结构。 */
typedef struct ExportedSnapshot
{
	char	   *snapfile;
	Snapshot	snapshot;
} ExportedSnapshot;

/* 当前事务导出的快照（ExportedSnapshot 结构列表） */
static List *exportedSnapshots = NIL;

/* 本地函数的原型 */
static TimestampTz AlignTimestampToMinuteBoundary(TimestampTz ts);
static Snapshot CopySnapshot(Snapshot snapshot);
static void FreeSnapshot(Snapshot snapshot);
static void SnapshotResetXmin(void);

/*
 * 需要序列化的快照字段。
 *
 * 只需把这些字段发给协作后端；其余的可（且必
 * 须）由接收方在恢复时设置。
 */
typedef struct SerializedSnapshotData
{
	TransactionId xmin;
	TransactionId xmax;
	uint32		xcnt;
	int32		subxcnt;
	bool		suboverflowed;
	bool		takenDuringRecovery;
	CommandId	curcid;
	TimestampTz whenTaken;
	XLogRecPtr	lsn;
} SerializedSnapshotData;

Size
SnapMgrShmemSize(void)
{
	Size		size;

	size = offsetof(OldSnapshotControlData, xid_by_minute);
	if (old_snapshot_threshold > 0)
		size = add_size(size, mul_size(sizeof(TransactionId),
									   OLD_SNAPSHOT_TIME_MAP_ENTRIES));

	return size;
}

/*
 * 初始化旧快照检测的管理。
 */
void
SnapMgrInit(void)
{
	bool		found;

	/*
	 * 创建或挂接到 OldSnapshotControlData 结构。
	 */
	oldSnapshotControl = (volatile OldSnapshotControlData *)
		ShmemInitStruct("OldSnapshotControlData",
						SnapMgrShmemSize(), &found);

	if (!found)
	{
		SpinLockInit(&oldSnapshotControl->mutex_current);
		oldSnapshotControl->current_timestamp = 0;
		SpinLockInit(&oldSnapshotControl->mutex_latest_xmin);
		oldSnapshotControl->latest_xmin = InvalidTransactionId;
		oldSnapshotControl->next_map_update = 0;
		SpinLockInit(&oldSnapshotControl->mutex_threshold);
		oldSnapshotControl->threshold_timestamp = 0;
		oldSnapshotControl->threshold_xid = InvalidTransactionId;
		oldSnapshotControl->head_offset = 0;
		oldSnapshotControl->head_timestamp = 0;
		oldSnapshotControl->count_used = 0;
	}
}

/*
 * GetTransactionSnapshot
 *		为事务中的新查询取得合适的快照。
 *
 * 注意返回值可能指向静态存储，会被后续调用及
 * CommandCounterIncrement() 修改。调用者若要长期
 * 使用返回的快照，应调 RegisterSnapshot 或 PushActiveSnapshot
 * 。
 */
Snapshot
GetTransactionSnapshot(void)
{
	/*
	 * 做逻辑解码时返回历史快照；这个（子）事务里不会
	 * 再需要非历史的事务快照，所以不必小心为后续的
	 * GetTransactionSnapshot() 调用设置一个
	 * 。
	 */
	if (HistoricSnapshotActive())
	{
		Assert(!FirstSnapshotSet);
		return HistoricSnapshot;
	}

	/* 事务内首次调用？ */
	if (!FirstSnapshotSet)
	{
		/*
		 * 不允许目录快照比事务快照老。必须先做这一步，
		 * 以让空堆的 Assert 成功。
		 */
		InvalidateCatalogSnapshot();

		Assert(pairingheap_is_empty(&RegisteredSnapshots));
		Assert(FirstXactSnapshot == NULL);

		if (IsInParallelMode())
			elog(ERROR,
				 "cannot take query snapshot during a parallel operation");

		/*
		 * 事务快照模式下首个快照须活到事务结束，
		 * 不管调用者怎么用它；必须复制而非直接返回
		 * CurrentSnapshotData。
		 * 此外，如果我们在可串行化模式下运行，
		 * predicate.c 需要把快照获取包进它自己的处理中。
		 */
		if (IsolationUsesXactSnapshot())
		{
			/* 先在 CurrentSnapshotData 中创建快照 */
			if (IsolationIsSerializable())
				CurrentSnapshot = GetSerializableTransactionSnapshot(&CurrentSnapshotData);
			else
				CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);
			/* 复制保存 */
			CurrentSnapshot = CopySnapshot(CurrentSnapshot);
			FirstXactSnapshot = CurrentSnapshot;
			/* 标为 FirstXactSnapshot 的已注册 */
			FirstXactSnapshot->regd_count++;
			pairingheap_add(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
		}
		else
			CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

		FirstSnapshotSet = true;
		return CurrentSnapshot;
	}

	if (IsolationUsesXactSnapshot())
		return CurrentSnapshot;

	/* 不允许目录快照比事务快照更老。 */
	InvalidateCatalogSnapshot();

	CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

	return CurrentSnapshot;
}

/*
 * GetLatestSnapshot
 *		取得与当前时刻同步的最新快照，
 *		即使我们正运行在事务快照模式下。
 */
Snapshot
GetLatestSnapshot(void)
{
	/*
	 * 或许可放宽，但除此之外能正常工作都不需
	 * 要它。
	 */
	if (IsInParallelMode())
		elog(ERROR,
			 "cannot update SecondarySnapshot during a parallel operation");

	/*
	 * 迄今还没有需要在逻辑解码期间支持
	 * GetLatestSnapshot() 的情况，但要加也不难。
	 */
	Assert(!HistoricSnapshotActive());

	/* 若是事务内第一次调用，就直接设置事务快照 */
	if (!FirstSnapshotSet)
		return GetTransactionSnapshot();

	SecondarySnapshot = GetSnapshotData(&SecondarySnapshotData);

	return SecondarySnapshot;
}

/*
 * GetOldestSnapshot
 *
 *		按 LSN 判断，取得事务已知的最早快照。
 *		如果没有活动或已注册快照则返回 NULL。
 */
Snapshot
GetOldestSnapshot(void)
{
	Snapshot	OldestRegisteredSnapshot = NULL;
	XLogRecPtr	RegisteredLSN = InvalidXLogRecPtr;

	if (!pairingheap_is_empty(&RegisteredSnapshots))
	{
		OldestRegisteredSnapshot = pairingheap_container(SnapshotData, ph_node,
														 pairingheap_first(&RegisteredSnapshots));
		RegisteredLSN = OldestRegisteredSnapshot->lsn;
	}

	if (OldestActiveSnapshot != NULL)
	{
		XLogRecPtr	ActiveLSN = OldestActiveSnapshot->as_snap->lsn;

		if (XLogRecPtrIsInvalid(RegisteredLSN) || RegisteredLSN > ActiveLSN)
			return OldestActiveSnapshot->as_snap;
	}

	return OldestRegisteredSnapshot;
}

/*
 * GetCatalogSnapshot
 *		取得一个足以用于扫描指定 OID 的
 *		系统目录的快照。
 */
Snapshot
GetCatalogSnapshot(Oid relid)
{
	/*
	 * 逻辑解码期间返回历史快照，以便看到目录的当
	 * 时状态。
	 *
	 * 这就是完成解码后需要重置系统缓存的主要原因
	 * 。
	 */
	if (HistoricSnapshotActive())
		return HistoricSnapshot;

	return GetNonHistoricCatalogSnapshot(relid);
}

/*
 * GetNonHistoricCatalogSnapshot
 *		取得对扫描指定 OID 的系统目录足够新的快照，即便
 *		此刻已设置了历史快照
 *		。
 */
Snapshot
GetNonHistoricCatalogSnapshot(Oid relid)
{
	/*
	 * 如果调用者想扫描的表没有 syscache，那么它更新时
	 * 不会有 catcache 失效消息发来。对少数关键关系，发
	 * 送的是快照失效消息。如果我们想扫描的关系既没有
	 * catcache 失效也没有快照失效，那就必须每次都刷新
	 * 快照。
	 */
	if (CatalogSnapshot &&
		!RelationInvalidatesSnapshotsOnly(relid) &&
		!RelationHasSysCache(relid))
		InvalidateCatalogSnapshot();

	if (CatalogSnapshot == NULL)
	{
		/* 获取新快照 */
		CatalogSnapshot = GetSnapshotData(&CatalogSnapshotData);

		/*
		 * 确保目录快照会被计入关于是否推进 PGPROC->xmin
		 * 的决策。我们可以用 RegisterSnapshot，但那会产生物
		 * 理复制，太过了；还会依赖某个资源所有者，而这
		 * 不是我们想要的（原因见文件头说明）。所以直接
		 * 把 CatalogSnapshot 手工塞进配对堆。当然，这一
		 * 步要在 InvalidateCatalogSnapshot 里反转。
		 * 。
		 *
		 * 注意：这一步最好不要抛错，因为 CatalogSnapshot
		 * 指针已经有效。
		 */
		pairingheap_add(&RegisteredSnapshots, &CatalogSnapshot->ph_node);
	}

	return CatalogSnapshot;
}

/*
 * InvalidateCatalogSnapshot
 *		把当前目录快照（若有）标记为无效
 *
 * 我们可以改这个 API 让调用者提供更细粒度的失效细
 * 节，这样修改关系 A 就不会妨碍我们用缓存的快照去
 * 扫描关系 B，但迄今没有证据表明花 CPU 周期去
 * 追这些细节是值得的
 * 。
 */
void
InvalidateCatalogSnapshot(void)
{
	if (CatalogSnapshot)
	{
		pairingheap_remove(&RegisteredSnapshots, &CatalogSnapshot->ph_node);
		CatalogSnapshot = NULL;
		SnapshotResetXmin();
	}
}

/*
 * InvalidateCatalogSnapshotConditionally
 *		如果目录快照是我们仅有的就丢弃它
 *
 * 这个函数在即将等待客户端输入时被调用，所以如果
 * 持有目录快照可能意味着全局 xmin 地平线无法推进，
 * 我们就不想继续持有它。但如果还有其他快照处于活
 * 动或已注册状态，目录快照多半不是最老的，所以不
 * 妨留着它。
 */
void
InvalidateCatalogSnapshotConditionally(void)
{
	if (CatalogSnapshot &&
		ActiveSnapshot == NULL &&
		pairingheap_is_singular(&RegisteredSnapshots))
		InvalidateCatalogSnapshot();
}

/*
 * SnapshotSetCommandId
 *		已设置则传 CommandCounterIncrement 给静态快照
 */
void
SnapshotSetCommandId(CommandId curcid)
{
	if (!FirstSnapshotSet)
		return;

	if (CurrentSnapshot)
		CurrentSnapshot->curcid = curcid;
	if (SecondarySnapshot)
		SecondarySnapshot->curcid = curcid;
	/* CatalogSnapshot 也要同样处理吗？ */
}

/*
 * SetTransactionSnapshot
 *		用导入的 MVCC 快照设置事务快照。
 *
 * 注意它与 GetTransactionSnapshot 紧密相关 —— 它必
 * 须处理好与 GetTransactionSnapshot 中首个快照情形相
 * 同的所有事项。
 */
static void
SetTransactionSnapshot(Snapshot sourcesnap, VirtualTransactionId *sourcevxid,
					   int sourcepid, PGPROC *sourceproc)
{
	/* 调用者应该已经检查过了 */
	Assert(!FirstSnapshotSet);

	/* 先做这步以确保后面的 Assert 成功。 */
	InvalidateCatalogSnapshot();

	Assert(pairingheap_is_empty(&RegisteredSnapshots));
	Assert(FirstXactSnapshot == NULL);
	Assert(!HistoricSnapshotActive());

	/*
	 * 即使我们不会使用它算出的快照，也必须调用
	 * GetSnapshotData，原因有二：(1) 确保
	 * CurrentSnapshotData 的 XID 数组已分配，以及 (2) 更新
	 * GlobalVis* 的状态。
	 */
	CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

	/*
	 * 现在从源快照复制相应的字段。
	 */
	CurrentSnapshot->xmin = sourcesnap->xmin;
	CurrentSnapshot->xmax = sourcesnap->xmax;
	CurrentSnapshot->xcnt = sourcesnap->xcnt;
	Assert(sourcesnap->xcnt <= GetMaxSnapshotXidCount());
	if (sourcesnap->xcnt > 0)
		memcpy(CurrentSnapshot->xip, sourcesnap->xip,
			   sourcesnap->xcnt * sizeof(TransactionId));
	CurrentSnapshot->subxcnt = sourcesnap->subxcnt;
	Assert(sourcesnap->subxcnt <= GetMaxSnapshotSubxidCount());
	if (sourcesnap->subxcnt > 0)
		memcpy(CurrentSnapshot->subxip, sourcesnap->subxip,
			   sourcesnap->subxcnt * sizeof(TransactionId));
	CurrentSnapshot->suboverflowed = sourcesnap->suboverflowed;
	CurrentSnapshot->takenDuringRecovery = sourcesnap->takenDuringRecovery;
	/* 注意：curcid 不可复制，属本地事务 */

	CurrentSnapshot->snapXactCompletionCount = 0;

	/*
	 * 现在得修正 GetSnapshotData 对 MyProc->xmin 的处
	 * 理以及 TransactionXmin。这里有竞态：为确保我们不
	 * 会把全局 xmin 弄倒退，必须测源事务仍在运行，
	 * 且必须原子性完成。所以让 procarray.c 来做
	 * 。
	 *
	 * 注意：可串行化模式下 predicate.c 会再做一次。
	 * 为省掉两次调用而扭曲这里的逻辑不值得，何况
	 * 也不清楚 predicate.c 是否*必须*这么做。
	 */
	if (sourceproc != NULL)
	{
		if (!ProcArrayInstallRestoredXmin(CurrentSnapshot->xmin, sourceproc))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("could not import the requested snapshot"),
					 errdetail("The source transaction is not running anymore.")));
	}
	else if (!ProcArrayInstallImportedXmin(CurrentSnapshot->xmin, sourcevxid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not import the requested snapshot"),
				 errdetail("The source process with PID %d is not running anymore.",
						   sourcepid)));

	/*
	 * 事务快照模式下首个快照须活到事务结束为止，
	 * 必须复制它。此外，若运行在可串行化模式下，
	 * predicate.c 需要做它自己的处理。
	 */
	if (IsolationUsesXactSnapshot())
	{
		if (IsolationIsSerializable())
			SetSerializableTransactionSnapshot(CurrentSnapshot, sourcevxid,
											   sourcepid);
		/* 复制保存 */
		CurrentSnapshot = CopySnapshot(CurrentSnapshot);
		FirstXactSnapshot = CurrentSnapshot;
		/* 标为 FirstXactSnapshot 的已注册 */
		FirstXactSnapshot->regd_count++;
		pairingheap_add(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
	}

	FirstSnapshotSet = true;
}

/*
 * CopySnapshot
 *		复制快照。
 *
 * 副本 palloc 于 TopTransactionContext，初始引用计数为
 * 0。返回的快照设置了已复制标志。
 */
static Snapshot
CopySnapshot(Snapshot snapshot)
{
	Snapshot	newsnap;
	Size		subxipoff;
	Size		size;

	Assert(snapshot != InvalidSnapshot);

	/* 在同一个 palloc 块中分配所需的 XID 数组。 */
	size = subxipoff = sizeof(SnapshotData) +
		snapshot->xcnt * sizeof(TransactionId);
	if (snapshot->subxcnt > 0)
		size += snapshot->subxcnt * sizeof(TransactionId);

	newsnap = (Snapshot) MemoryContextAlloc(TopTransactionContext, size);
	memcpy(newsnap, snapshot, sizeof(SnapshotData));

	newsnap->regd_count = 0;
	newsnap->active_count = 0;
	newsnap->copied = true;
	newsnap->snapXactCompletionCount = 0;

	/* 建 XID 数组 */
	if (snapshot->xcnt > 0)
	{
		newsnap->xip = (TransactionId *) (newsnap + 1);
		memcpy(newsnap->xip, snapshot->xip,
			   snapshot->xcnt * sizeof(TransactionId));
	}
	else
		newsnap->xip = NULL;

	/*
	 * 设置 subXID 数组。溢出时无需复制，因为那种
	 * 情况下它不会被用到。但若是在恢复期间取得的快
	 * 照，所有顶层 XID 也都在 subxip 里，所以不能丢掉
	 * 它们。
	 */
	if (snapshot->subxcnt > 0 &&
		(!snapshot->suboverflowed || snapshot->takenDuringRecovery))
	{
		newsnap->subxip = (TransactionId *) ((char *) newsnap + subxipoff);
		memcpy(newsnap->subxip, snapshot->subxip,
			   snapshot->subxcnt * sizeof(TransactionId));
	}
	else
		newsnap->subxip = NULL;

	return newsnap;
}

/*
 * FreeSnapshot
 *		释放与快照关联的内存。
 */
static void
FreeSnapshot(Snapshot snapshot)
{
	Assert(snapshot->regd_count == 0);
	Assert(snapshot->active_count == 0);
	Assert(snapshot->copied);

	pfree(snapshot);
}

/*
 * PushActiveSnapshot
 *		把给定的快照设为当前活动快照
 *
 * 如果传入的快照是静态分配的，或者它将来可能受命
 * 令计数器更新的影响，就以活跃引用计数=1 创建一份
 * 新的长寿命副本。否则只递增引用计数。
 */
void
PushActiveSnapshot(Snapshot snapshot)
{
	PushActiveSnapshotWithLevel(snapshot, GetCurrentTransactionNestLevel());
}

/*
 * PushActiveSnapshotWithLevel
 *		把给定的快照设为当前活动快照
 *
 * 与 PushActiveSnapshot 相同，但调用者可以指定
 * "拥有"该快照的事务嵌套层级，且层级不得比当
 * 快照栈的当前栈顶更深。
 */
void
PushActiveSnapshotWithLevel(Snapshot snapshot, int snap_level)
{
	ActiveSnapshotElt *newactive;

	Assert(snapshot != InvalidSnapshot);
	Assert(ActiveSnapshot == NULL || snap_level >= ActiveSnapshot->as_level);

	newactive = MemoryContextAlloc(TopTransactionContext, sizeof(ActiveSnapshotElt));

	/*
	 * 在这里检查 SecondarySnapshot 可能没用，但求个稳
	 * 妥。
	 */
	if (snapshot == CurrentSnapshot || snapshot == SecondarySnapshot ||
		!snapshot->copied)
		newactive->as_snap = CopySnapshot(snapshot);
	else
		newactive->as_snap = snapshot;

	newactive->as_next = ActiveSnapshot;
	newactive->as_level = snap_level;

	newactive->as_snap->active_count++;

	ActiveSnapshot = newactive;
	if (OldestActiveSnapshot == NULL)
		OldestActiveSnapshot = ActiveSnapshot;
}

/*
 * PushCopiedSnapshot
 *		同上，但强制复制传入的快照。
 *
 * 当活动快照必须可修改时应当使用它，例如调用者打
 * 算调用 UpdateActiveSnapshotCommandId。新快照会在
 * 从栈中弹出时释放。
 */
void
PushCopiedSnapshot(Snapshot snapshot)
{
	PushActiveSnapshot(CopySnapshot(snapshot));
}

/*
 * UpdateActiveSnapshotCommandId
 *
 * 更新活动快照的当前 CID。只适用于别处未引用的
 * 快照。
 */
void
UpdateActiveSnapshotCommandId(void)
{
	CommandId	save_curcid,
				curcid;

	Assert(ActiveSnapshot != NULL);
	Assert(ActiveSnapshot->as_snap->active_count == 1);
	Assert(ActiveSnapshot->as_snap->regd_count == 0);

	/*
	 * 并行中不得改快照：开始并行时快照共享给工
	 * 作进程，快照的任何改动都可能导致不一致。对于
	 * CommandCounterIncrement 我们另有防线，
	 * 但仍有少数地方会直接调用它，
	 * 所以在这里额外加一道防护。
	 * 。
	 */
	save_curcid = ActiveSnapshot->as_snap->curcid;
	curcid = GetCurrentCommandId(false);
	if (IsInParallelMode() && save_curcid != curcid)
		elog(ERROR, "cannot modify commandid in active snapshot during a parallel operation");
	ActiveSnapshot->as_snap->curcid = curcid;
}

/*
 * PopActiveSnapshot
 *
 * 从活动快照栈移除最顶上的快照并递减其引用计数；
 * 若这是最后一个引用则释放它。
 */
void
PopActiveSnapshot(void)
{
	ActiveSnapshotElt *newstack;

	newstack = ActiveSnapshot->as_next;

	Assert(ActiveSnapshot->as_snap->active_count > 0);

	ActiveSnapshot->as_snap->active_count--;

	if (ActiveSnapshot->as_snap->active_count == 0 &&
		ActiveSnapshot->as_snap->regd_count == 0)
		FreeSnapshot(ActiveSnapshot->as_snap);

	pfree(ActiveSnapshot);
	ActiveSnapshot = newstack;
	if (ActiveSnapshot == NULL)
		OldestActiveSnapshot = NULL;

	SnapshotResetXmin();
}

/*
 * GetActiveSnapshot
 *		返回活动栈最顶上的快照。
 */
Snapshot
GetActiveSnapshot(void)
{
	Assert(ActiveSnapshot != NULL);

	return ActiveSnapshot->as_snap;
}

/*
 * ActiveSnapshotSet
 *		返回活动栈中是否至少有一个快照
 */
bool
ActiveSnapshotSet(void)
{
	return ActiveSnapshot != NULL;
}

/*
 * RegisterSnapshot
 *		把快照注册为正被当前资源所有者使用
 *
 * 如果传入 InvalidSnapshot 则不注册。
 */
Snapshot
RegisterSnapshot(Snapshot snapshot)
{
	if (snapshot == InvalidSnapshot)
		return InvalidSnapshot;

	return RegisterSnapshotOnOwner(snapshot, CurrentResourceOwner);
}

/*
 * RegisterSnapshotOnOwner
 *		同上，但使用指定的资源所有者
 */
Snapshot
RegisterSnapshotOnOwner(Snapshot snapshot, ResourceOwner owner)
{
	Snapshot	snap;

	if (snapshot == InvalidSnapshot)
		return InvalidSnapshot;

	/* 静态快照？创建一份持久副本 */
	snap = snapshot->copied ? snapshot : CopySnapshot(snapshot);

	/* 并把它告知 resowner.c */
	ResourceOwnerEnlargeSnapshots(owner);
	snap->regd_count++;
	ResourceOwnerRememberSnapshot(owner, snap);

	if (snap->regd_count == 1)
		pairingheap_add(&RegisteredSnapshots, &snap->ph_node);

	return snap;
}

/*
 * UnregisterSnapshot
 *
 * 递减快照的引用计数，从 CurrentResourceOwner 中移
 * 除相应引用，并在没有剩余引用时释放快照。
 * 。
 */
void
UnregisterSnapshot(Snapshot snapshot)
{
	if (snapshot == NULL)
		return;

	UnregisterSnapshotFromOwner(snapshot, CurrentResourceOwner);
}

/*
 * UnregisterSnapshotFromOwner
 *		同上，但使用指定的资源所有者
 */
void
UnregisterSnapshotFromOwner(Snapshot snapshot, ResourceOwner owner)
{
	if (snapshot == NULL)
		return;

	Assert(snapshot->regd_count > 0);
	Assert(!pairingheap_is_empty(&RegisteredSnapshots));

	ResourceOwnerForgetSnapshot(owner, snapshot);

	snapshot->regd_count--;
	if (snapshot->regd_count == 0)
		pairingheap_remove(&RegisteredSnapshots, &snapshot->ph_node);

	if (snapshot->regd_count == 0 && snapshot->active_count == 0)
	{
		FreeSnapshot(snapshot);
		SnapshotResetXmin();
	}
}

/*
 * RegisteredSnapshots 堆的比较函数。快照按 xmin 排
 * 序，xmin 最小的快照位于堆顶。
 */
static int
xmin_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const SnapshotData *asnap = pairingheap_const_container(SnapshotData, ph_node, a);
	const SnapshotData *bsnap = pairingheap_const_container(SnapshotData, ph_node, b);

	if (TransactionIdPrecedes(asnap->xmin, bsnap->xmin))
		return 1;
	else if (TransactionIdFollows(asnap->xmin, bsnap->xmin))
		return -1;
	else
		return 0;
}

/*
 * SnapshotResetXmin
 *
 * 如果没有别的快照，我们可以把 PGPROC->xmin 重置
 * 为 InvalidTransactionId。注意无需加锁即可这样做，
 * 存 Xid 是原子操作。
 *
 * 即使还剩一些快照，我们也许能把 PGPROC->xmin 推进
 * 到一定程度。这通常发生在 portal 被删除时。为了效
 * 率，只有当活动快照栈为空时我们才考虑重算
 * PGPROC->xmin；这让我们无需跟踪
 * 哪个活动快照最老。
 *
 * 注意：在这里用 GetOldestSnapshot() 把活动快照也算
 * 进来很诱人。但它按 LSN 而非 xmin 比较，所以并不
 * 完全清楚是不是同一回事。而且我们会严重依赖
 * 最底部的活动快照栈条目具有最老 xmin 这一假设。
 * （现状并不关键，但这将是
 * 关键。）
 */
static void
SnapshotResetXmin(void)
{
	Snapshot	minSnapshot;

	if (ActiveSnapshot != NULL)
		return;

	if (pairingheap_is_empty(&RegisteredSnapshots))
	{
		MyProc->xmin = TransactionXmin = InvalidTransactionId;
		return;
	}

	minSnapshot = pairingheap_container(SnapshotData, ph_node,
										pairingheap_first(&RegisteredSnapshots));

	if (TransactionIdPrecedes(MyProc->xmin, minSnapshot->xmin))
		MyProc->xmin = TransactionXmin = minSnapshot->xmin;
}

/*
 * AtSubCommit_Snapshot
 */
void
AtSubCommit_Snapshot(int level)
{
	ActiveSnapshotElt *active;

	/*
	 * 把这个子事务里设置的活动快照重新标记为归父子事
	 * 务所有。
	 */
	for (active = ActiveSnapshot; active != NULL; active = active->as_next)
	{
		if (active->as_level < level)
			break;
		active->as_level = level - 1;
	}
}

/*
 * AtSubAbort_Snapshot
 *		子事务中止后清理快照
 */
void
AtSubAbort_Snapshot(int level)
{
	/* 忘掉这个子事务设置的活动快照 */
	while (ActiveSnapshot && ActiveSnapshot->as_level >= level)
	{
		ActiveSnapshotElt *next;

		next = ActiveSnapshot->as_next;

		/*
		 * 递减该快照的活动计数。若仍被注册，或仍被外
		 * 层子事务标为活动，就还不能释放它。
		 */
		Assert(ActiveSnapshot->as_snap->active_count >= 1);
		ActiveSnapshot->as_snap->active_count -= 1;

		if (ActiveSnapshot->as_snap->active_count == 0 &&
			ActiveSnapshot->as_snap->regd_count == 0)
			FreeSnapshot(ActiveSnapshot->as_snap);

		/* 并释放栈元素 */
		pfree(ActiveSnapshot);

		ActiveSnapshot = next;
		if (ActiveSnapshot == NULL)
			OldestActiveSnapshot = NULL;
	}

	SnapshotResetXmin();
}

/*
 * AtEOXact_Snapshot
 *		快照管理器在事务结束时的清理函数
 */
void
AtEOXact_Snapshot(bool isCommit, bool resetXmin)
{
	/*
	 * 事务快照模式下，须释放私有管理的事务快照引
	 * 用。必须把它从 RegisteredSnapshots 移除以满足下
	 * 面的检查。但不费心做 FreeSnapshot，原因有二：内
	 * 存会随 TopTransactionContext 一起消失；而且如果
	 * 有人把该快照留在活动栈上，我们不想让下面的
	 * 代码去追一个悬空指针。
	 * 。
	 */
	if (FirstXactSnapshot != NULL)
	{
		Assert(FirstXactSnapshot->regd_count > 0);
		Assert(!pairingheap_is_empty(&RegisteredSnapshots));
		pairingheap_remove(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
	}
	FirstXactSnapshot = NULL;

	/*
	 * 如有导出的快照，清理它们。
	 */
	if (exportedSnapshots != NIL)
	{
		ListCell   *lc;

		/*
		 * 删掉这些文件。unlink 失败只是 WARNING，因为 (1)
		 * 中止已太迟，(2) 留下一个泄漏文件反正也
		 * 没什么实际后果。
		 *
		 * 我们还要把快照从 RegisteredSnapshots 中移除，
		 * 免得下面发警告。
		 *
		 * 与 FirstXactSnapshot 一样，我们无需释放快照本身
		 * 的资源，因为它会随内存上下文一起消失。
		 */
		foreach(lc, exportedSnapshots)
		{
			ExportedSnapshot *esnap = (ExportedSnapshot *) lfirst(lc);

			if (unlink(esnap->snapfile))
				elog(WARNING, "could not unlink file \"%s\": %m",
					 esnap->snapfile);

			pairingheap_remove(&RegisteredSnapshots,
							   &esnap->snapshot->ph_node);
		}

		exportedSnapshots = NIL;
	}

	/* 如有目录快照则丢弃 */
	InvalidateCatalogSnapshot();

	/* 提交时对遗留快照提出警告 */
	if (isCommit)
	{
		ActiveSnapshotElt *active;

		if (!pairingheap_is_empty(&RegisteredSnapshots))
			elog(WARNING, "registered snapshots seem to remain after cleanup");

		/* 对未弹出的活动快照提出警告 */
		for (active = ActiveSnapshot; active != NULL; active = active->as_next)
			elog(WARNING, "snapshot %p still active", active);
	}

	/*
	 * 然后重置我们的状态。无需显式释放内存 —— 它随
	 * TopTransactionContext 一起消失。
	 */
	ActiveSnapshot = NULL;
	OldestActiveSnapshot = NULL;
	pairingheap_reset(&RegisteredSnapshots);

	CurrentSnapshot = NULL;
	SecondarySnapshot = NULL;

	FirstSnapshotSet = false;

	/*
	 * 在正常的提交处理中，我们会调用 ProcArrayEndTransac
	 * tion() 来重置 MyProc->xmin。该调用发生在
	 * AtEOXact_Snapshot() 之前，所以这里不必碰 xmin。
	 */
	if (resetXmin)
		SnapshotResetXmin();

	Assert(resetXmin || MyProc->xmin == 0);
}


/*
 * ExportSnapshot
 *		把快照导出到文件，让其他后端可以导入它。
 *		返回可用于导入此快照的令牌（即文件名）
 *		。
 */
char *
ExportSnapshot(Snapshot snapshot)
{
	TransactionId topXid;
	TransactionId *children;
	ExportedSnapshot *esnap;
	int			nchildren;
	int			addTopXid;
	StringInfoData buf;
	FILE	   *f;
	int			i;
	MemoryContext oldcxt;
	char		path[MAXPGPATH];
	char		pathtmp[MAXPGPATH];

	/*
	 * 在这里调用 RequireTransactionBlock 很诱人，因为导出
	 * 一个马上就会消失的快照没什么用。但我们没有足够
	 * 的信息这么做，因为我们不知道自己是否在顶层。比
	 * 如我们可能位于一个将通过 dblink 发起其他事务的
	 * plpgsql 函数里。与其禁止完全合法的用法，就不
	 * 检查
	 * 。
	 *
	 * 另请注意对事务隔离级别不做任何限制；不过，
	 * 导入者若是可串行化则必须检查级别。
	 * 。
	 */

	/*
	 * 如果存在事务 ID，就取得它以放进快照。
	 */
	topXid = GetTopTransactionIdIfAny();

	/*
	 * 我们不能从子事务导出快照：导入者很难验证同一
	 * 个子事务仍在运行。
	 * 。
	 */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot export a snapshot from a subtransaction")));

	/*
	 * 但允许此前已提交的子事务；导入者须把它们视
	 * 作仍在运行，所以取它们的 XID 加进快照。
	 * 。
	 */
	nchildren = xactGetCommittedChildren(&children);

	/*
	 * 生成快照的文件路径。我们从事务内第 1 个开始给快
	 * 照编号。
	 */
	snprintf(path, sizeof(path), SNAPSHOT_EXPORT_DIR "/%08X-%08X-%d",
			 MyProc->backendId, MyProc->lxid, list_length(exportedSnapshots) + 1);

	/*
	 * 把快照复制进 TopTransactionContext，加入
	 * exportedSnapshots 列表，并标记为伪注册。这样做是
	 * 以确保该快照的 xmin 在事务余下时间受尊重。
	 * 。
	 */
	snapshot = CopySnapshot(snapshot);

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	esnap = (ExportedSnapshot *) palloc(sizeof(ExportedSnapshot));
	esnap->snapfile = pstrdup(path);
	esnap->snapshot = snapshot;
	exportedSnapshots = lappend(exportedSnapshots, esnap);
	MemoryContextSwitchTo(oldcxt);

	snapshot->regd_count++;
	pairingheap_add(&RegisteredSnapshots, &snapshot->ph_node);

	/*
	 * 在 buf 中填入快照的文本序列化，外加此事务的标识
	 * 数据。ImportSnapshot 期望的格式相当死板：每行必须
	 * 是 字段名:值。
	 */
	initStringInfo(&buf);

	appendStringInfo(&buf, "vxid:%d/%u\n", MyProc->backendId, MyProc->lxid);
	appendStringInfo(&buf, "pid:%d\n", MyProcPid);
	appendStringInfo(&buf, "dbid:%u\n", MyDatabaseId);
	appendStringInfo(&buf, "iso:%d\n", XactIsoLevel);
	appendStringInfo(&buf, "ro:%d\n", XactReadOnly);

	appendStringInfo(&buf, "xmin:%u\n", snapshot->xmin);
	appendStringInfo(&buf, "xmax:%u\n", snapshot->xmax);

	/*
	 * 我们必须把自己的顶层事务 ID 放进 top-xid 数据，
	 * 因为按定义导入事务采用快照时我们仍在运行，而
	 * GetSnapshotData 从不把我们自己的 XID 放进快照。
	 * （因此必须有足够的空间来加上它。）
	 *
	 * 但是，我们的 topXid 可能位于 xmax 之后，那种情况
	 * 下不应包含它，因为 xip[] 成员被要求在 xmax 之前。
	 * （subxip[] 成员无需同样检查，参见 snapshot.h）
	 * 。
	 */
	addTopXid = (TransactionIdIsValid(topXid) &&
				 TransactionIdPrecedes(topXid, snapshot->xmax)) ? 1 : 0;
	appendStringInfo(&buf, "xcnt:%d\n", snapshot->xcnt + addTopXid);
	for (i = 0; i < snapshot->xcnt; i++)
		appendStringInfo(&buf, "xip:%u\n", snapshot->xip[i]);
	if (addTopXid)
		appendStringInfo(&buf, "xip:%u\n", topXid);

	/*
	 * 同样，我们把子提交的子 XID 加进 subxid 数据。这里
	 * 必须应对可能的溢出。
	 */
	if (snapshot->suboverflowed ||
		snapshot->subxcnt + nchildren > GetMaxSnapshotSubxidCount())
		appendStringInfoString(&buf, "sof:1\n");
	else
	{
		appendStringInfoString(&buf, "sof:0\n");
		appendStringInfo(&buf, "sxcnt:%d\n", snapshot->subxcnt + nchildren);
		for (i = 0; i < snapshot->subxcnt; i++)
			appendStringInfo(&buf, "sxp:%u\n", snapshot->subxip[i]);
		for (i = 0; i < nchildren; i++)
			appendStringInfo(&buf, "sxp:%u\n", children[i]);
	}
	appendStringInfo(&buf, "rec:%u\n", snapshot->takenDuringRecovery);

	/*
	 * 现在把文本表示写入文件。先写 ".tmp" 文件名，
	 * 若无错误再改最终名。这确保没有后端能读到不
	 * 完整的文件（ImportSnapshot 字符检查不允许这
	 * 样）。
	 */
	snprintf(pathtmp, sizeof(pathtmp), "%s.tmp", path);
	if (!(f = AllocateFile(pathtmp, PG_BINARY_W)))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", pathtmp)));

	if (fwrite(buf.data, buf.len, 1, f) != 1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", pathtmp)));

	/* 无需 fsync()，文件不必在系统崩溃后幸存 */

	if (FreeFile(f))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", pathtmp)));

	/*
	 * 现在所有内容都已写入 .tmp 文件，重命名该文件以去
	 * 掉 .tmp 后缀。
	 */
	if (rename(pathtmp, path) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						pathtmp, path)));

	/*
	 * 文件基名就是我们从 pg_export_snapshot() 返回的内
	 * 容。它已按文本形式放在 path 中，且我们知道 path
	 * 以 SNAPSHOT_EXPORT_DIR 开头。跳过前缀和斜杠，然后
	 * pstrdup 它，以免返回局部变量的地址。
	 */
	return pstrdup(path + strlen(SNAPSHOT_EXPORT_DIR) + 1);
}

/*
 * pg_export_snapshot
 *		ExportSnapshot 的 SQL 可调用包装
 */
Datum
pg_export_snapshot(PG_FUNCTION_ARGS)
{
	char	   *snapshotName;

	snapshotName = ExportSnapshot(GetActiveSnapshot());
	PG_RETURN_TEXT_P(cstring_to_text(snapshotName));
}


/*
 * ImportSnapshot 的解析子例程：解析带有给定前缀后跟
 * 一个值的行，把 *s 前进到下一行。文件名用于错
 * 误消息中。
 */
static int
parseIntFromText(const char *prefix, char **s, const char *filename)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);
	int			val;

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%d", &val) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
	return val;
}

static TransactionId
parseXidFromText(const char *prefix, char **s, const char *filename)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);
	TransactionId val;

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%u", &val) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
	return val;
}

static void
parseVxidFromText(const char *prefix, char **s, const char *filename,
				  VirtualTransactionId *vxid)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%d/%u", &vxid->backendId, &vxid->localTransactionId) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
}

/*
 * ImportSnapshot
 *		导入先前导出的快照。参数应是 SNAPSHOT_EXPORT_DIR
 *		中的文件名。从该文件加载快照。
 *		由 "SET TRANSACTION SNAPSHOT 'foo'" 调用。
 */
void
ImportSnapshot(const char *idstr)
{
	char		path[MAXPGPATH];
	FILE	   *f;
	struct stat stat_buf;
	char	   *filebuf;
	int			xcnt;
	int			i;
	VirtualTransactionId src_vxid;
	int			src_pid;
	Oid			src_dbid;
	int			src_isolevel;
	bool		src_readonly;
	SnapshotData snapshot;

	/*
	 * 必须在全新事务的顶层。特别要注意我们会检查自
	 * 己尚未取得的 XID —— 若已取得，快照可能把
	 * 它显示为未在运行，造成诡异的状态。
	 *
	 */
	if (FirstSnapshotSet ||
		GetTopTransactionIdIfAny() != InvalidTransactionId ||
		IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("SET TRANSACTION SNAPSHOT must be called before any query")));

	/*
	 * 如果我们处于读已提交模式，下一个查询将以新快照
	 * 执行，使这次函数调用毫无用处。
	 */
	if (!IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a snapshot-importing transaction must have isolation level SERIALIZABLE or REPEATABLE READ")));

	/*
	 * 验证标识符：只允许 0-9、A-F 和连字符。这样做主
	 * 要是为了防止读取任意文件。
	 */
	if (strspn(idstr, "0123456789ABCDEF-") != strlen(idstr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid snapshot identifier: \"%s\"", idstr)));

	/* 好，读文件 */
	snprintf(path, MAXPGPATH, SNAPSHOT_EXPORT_DIR "/%s", idstr);

	f = AllocateFile(path, PG_BINARY_R);
	if (!f)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid snapshot identifier: \"%s\"", idstr)));

	/* 取文件大小以便知道需要多少内存 */
	if (fstat(fileno(f), &stat_buf))
		elog(ERROR, "could not stat file \"%s\": %m", path);

	/* 然后读入 palloc 的字符串 */
	filebuf = (char *) palloc(stat_buf.st_size + 1);
	if (fread(filebuf, stat_buf.st_size, 1, f) != 1)
		elog(ERROR, "could not read file \"%s\": %m", path);

	filebuf[stat_buf.st_size] = '\0';

	FreeFile(f);

	/*
	 * 通过解析文件内容构造快照结构体。
	 */
	memset(&snapshot, 0, sizeof(snapshot));

	parseVxidFromText("vxid:", &filebuf, path, &src_vxid);
	src_pid = parseIntFromText("pid:", &filebuf, path);
	/* 这里略滥用 parseXidFromText */
	src_dbid = parseXidFromText("dbid:", &filebuf, path);
	src_isolevel = parseIntFromText("iso:", &filebuf, path);
	src_readonly = parseIntFromText("ro:", &filebuf, path);

	snapshot.snapshot_type = SNAPSHOT_MVCC;

	snapshot.xmin = parseXidFromText("xmin:", &filebuf, path);
	snapshot.xmax = parseXidFromText("xmax:", &filebuf, path);

	snapshot.xcnt = xcnt = parseIntFromText("xcnt:", &filebuf, path);

	/* palloc 前先查 xid 计数 */
	if (xcnt < 0 || xcnt > GetMaxSnapshotXidCount())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", path)));

	snapshot.xip = (TransactionId *) palloc(xcnt * sizeof(TransactionId));
	for (i = 0; i < xcnt; i++)
		snapshot.xip[i] = parseXidFromText("xip:", &filebuf, path);

	snapshot.suboverflowed = parseIntFromText("sof:", &filebuf, path);

	if (!snapshot.suboverflowed)
	{
		snapshot.subxcnt = xcnt = parseIntFromText("sxcnt:", &filebuf, path);

		/* palloc 前先查 xid 计数 */
		if (xcnt < 0 || xcnt > GetMaxSnapshotSubxidCount())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid snapshot data in file \"%s\"", path)));

		snapshot.subxip = (TransactionId *) palloc(xcnt * sizeof(TransactionId));
		for (i = 0; i < xcnt; i++)
			snapshot.subxip[i] = parseXidFromText("sxp:", &filebuf, path);
	}
	else
	{
		snapshot.subxcnt = 0;
		snapshot.subxip = NULL;
	}

	snapshot.takenDuringRecovery = parseIntFromText("rec:", &filebuf, path);

	/*
	 * 做些额外完整性检查，只为自保。我们不费心检查
	 * 数组元素，只查最关键的字段。
	 * 。
	 */
	if (!VirtualTransactionIdIsValid(src_vxid) ||
		!OidIsValid(src_dbid) ||
		!TransactionIdIsNormal(snapshot.xmin) ||
		!TransactionIdIsNormal(snapshot.xmax))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", path)));

	/*
	 * 如果我们是可串行化的，源事务也必须是，否则
	 * predicate.c 会有问题（SxactGlobalXmin 可能倒退）。另
	 * 外，非只读事务不能采用只读事务的快照，因为
	 * predicate.c 对这些情况的处理非常不同。
	 */
	if (IsolationIsSerializable())
	{
		if (src_isolevel != XACT_SERIALIZABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a serializable transaction cannot import a snapshot from a non-serializable transaction")));
		if (src_readonly && !XactReadOnly)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a non-read-only serializable transaction cannot import a snapshot from a read-only transaction")));
	}

	/*
	 * 我们不能导入在另一个数据库中取得的快照，因为
	 * vacuum 按每个数据库计算 OldestXmin；所以源事务的
	 * xmin 保护不了我们不丢数据。若源事务把其 xmin 标
	 * 记为全局适用，这个限制本可以去掉。但那需要一
	 * 些额外语法，取得快照时就得知道。
	 * （参见 2011-10-21 的 pgsql-hackers 讨论。）
	 * 
	 */
	if (src_dbid != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot import a snapshot from a different database")));

	/* 好，安装该快照 */
	SetTransactionSnapshot(&snapshot, &src_vxid, src_pid, NULL);
}

/*
 * XactHasExportedSnapshots
 *		测试当前事务是否导出过快照。
 */
bool
XactHasExportedSnapshots(void)
{
	return (exportedSnapshots != NIL);
}

/*
 * DeleteAllExportedSnapshotFiles
 *		清理曾导出过快照随后死掉的后端留下的文件。
 *		。
 *
 * 这应在数据库启动或崩溃恢复期间调用。
 */
void
DeleteAllExportedSnapshotFiles(void)
{
	char		buf[MAXPGPATH + sizeof(SNAPSHOT_EXPORT_DIR)];
	DIR		   *s_dir;
	struct dirent *s_de;

	/*
	 * 读取目录或解除文件链接的问题以 LOG 级别报告。由
	 * 于我们运行在启动进程里，ERROR 级别会阻止数据库
	 * 启动，而这没那么重要。
	 */
	s_dir = AllocateDir(SNAPSHOT_EXPORT_DIR);

	while ((s_de = ReadDirExtended(s_dir, SNAPSHOT_EXPORT_DIR, LOG)) != NULL)
	{
		if (strcmp(s_de->d_name, ".") == 0 ||
			strcmp(s_de->d_name, "..") == 0)
			continue;

		snprintf(buf, sizeof(buf), SNAPSHOT_EXPORT_DIR "/%s", s_de->d_name);

		if (unlink(buf) != 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", buf)));
	}

	FreeDir(s_dir);
}

/*
 * ThereAreNoPriorRegisteredSnapshots
 *		已注册快照数是否小于等于 1？
 *
 * 不要用它来决定重要事务。虽然零注册且无
 * ActiveSnapshot 能确认某种空闲状态，但系统不保证
 * 一个已注册快照的意义。
 */
bool
ThereAreNoPriorRegisteredSnapshots(void)
{
	if (pairingheap_is_empty(&RegisteredSnapshots) ||
		pairingheap_is_singular(&RegisteredSnapshots))
		return true;

	return false;
}

/*
 * HaveRegisteredOrActiveSnapshot
 *		是否有任何已注册或活动快照？
 *
 * 注意：除非被压栈或活动，缓存的目录快照不会让此
 * 函数返回真。这允许把本函数用于强制更长寿命快照
 * 的检查。
 */
bool
HaveRegisteredOrActiveSnapshot(void)
{
	if (ActiveSnapshot != NULL)
		return true;

	/*
	 * 目录快照有效时位于 RegisteredSnapshots 中，但可能
	 * 因失效处理而随时被移除。如果显式注册，那么
	 * RegisteredSnapshots 中必须有多个快照。
	 */
	if (CatalogSnapshot != NULL &&
		pairingheap_is_singular(&RegisteredSnapshots))
		return false;

	return !pairingheap_is_empty(&RegisteredSnapshots);
}


/*
 * 返回一个正好落在分钟边界上的时间戳。
 *
 * 如果参数已对齐，就返回该值，否则移到给定时间之
 * 后的下一个分钟边界。
 */
static TimestampTz
AlignTimestampToMinuteBoundary(TimestampTz ts)
{
	TimestampTz retval = ts + (USECS_PER_MINUTE - 1);

	return retval - (retval % USECS_PER_MINUTE);
}

/*
 * 取快照用的当前时间戳
 *
 * 这基本上就是 GetCurrentTimestamp()，但保证结果绝不
 * 倒退。
 */
TimestampTz
GetSnapshotCurrentTimestamp(void)
{
	TimestampTz now = GetCurrentTimestamp();

	/*
	 * 不让时间倒退；如果没有前进，就用旧值。
	 */
	SpinLockAcquire(&oldSnapshotControl->mutex_current);
	if (now <= oldSnapshotControl->current_timestamp)
		now = oldSnapshotControl->current_timestamp;
	else
		oldSnapshotControl->current_timestamp = now;
	SpinLockRelease(&oldSnapshotControl->mutex_current);

	return now;
}

/*
 * 取得 vacuum 可能已处理到的时间戳，依据是上次存
 * 的 threshold_timestamp 值
 *
 * XXX：迄今我们从不假设 64 位值可以被原子地读取；
 * 如果这一点改变，可以去掉这里的自旋锁。
 */
TimestampTz
GetOldSnapshotThresholdTimestamp(void)
{
	TimestampTz threshold_timestamp;

	SpinLockAcquire(&oldSnapshotControl->mutex_threshold);
	threshold_timestamp = oldSnapshotControl->threshold_timestamp;
	SpinLockRelease(&oldSnapshotControl->mutex_threshold);

	return threshold_timestamp;
}

void
SetOldSnapshotThresholdTimestamp(TimestampTz ts, TransactionId xlimit)
{
	SpinLockAcquire(&oldSnapshotControl->mutex_threshold);
	Assert(oldSnapshotControl->threshold_timestamp <= ts);
	Assert(TransactionIdPrecedesOrEquals(oldSnapshotControl->threshold_xid, xlimit));
	oldSnapshotControl->threshold_timestamp = ts;
	oldSnapshotControl->threshold_xid = xlimit;
	SpinLockRelease(&oldSnapshotControl->mutex_threshold);
}

/*
 * XXX：为了让 old_snapshot_threshold 测试看起来"还在
 * 工作"而设的魔法。它们目前是坏的，处理办法仍
 * 讨论中。
 * https://www.postgresql.org/message-id/20200403001235.e6jfdll3gh2ygbuc%40alap3.anarazel.de
 */
void
SnapshotTooOldMagicForTest(void)
{
	TimestampTz ts = GetSnapshotCurrentTimestamp();

	Assert(old_snapshot_threshold == 0);

	ts -= 5 * USECS_PER_SEC;

	SpinLockAcquire(&oldSnapshotControl->mutex_threshold);
	oldSnapshotControl->threshold_timestamp = ts;
	SpinLockRelease(&oldSnapshotControl->mutex_threshold);
}

/*
 * 该时间戳若有有效映射，就把 *xlimitp 设为它。
 * 返回是否存在这样的映射。
 */
static bool
GetOldSnapshotFromTimeMapping(TimestampTz ts, TransactionId *xlimitp)
{
	bool		in_mapping = false;

	Assert(ts == AlignTimestampToMinuteBoundary(ts));

	LWLockAcquire(OldSnapshotTimeMapLock, LW_SHARED);

	if (oldSnapshotControl->count_used > 0
		&& ts >= oldSnapshotControl->head_timestamp)
	{
		int			offset;

		offset = ((ts - oldSnapshotControl->head_timestamp)
				  / USECS_PER_MINUTE);
		if (offset > oldSnapshotControl->count_used - 1)
			offset = oldSnapshotControl->count_used - 1;
		offset = (oldSnapshotControl->head_offset + offset)
			% OLD_SNAPSHOT_TIME_MAP_ENTRIES;

		*xlimitp = oldSnapshotControl->xid_by_minute[offset];

		in_mapping = true;
	}

	LWLockRelease(OldSnapshotTimeMapLock);

	return in_mapping;
}

/*
 * TransactionIdLimitedForOldSnapshots
 *
 * 应用旧快照限制。这个函数预期在页面修剪和表清理
 * 时被调用，以允许 old_snapshot_threshold 覆盖正常的
 * 全局 xmin 值。快照过旧与否，取决于快照时间戳
 * 是否早于本函数所设的限制。
 * 。
 *
 * 如果受限的地平线允许一项原本不可能的清理动作，
 * 则须先调 SetOldSnapshotThresholdTimestamp(*limit_ts, *limit_xid)。
 * 。
 */
bool
TransactionIdLimitedForOldSnapshots(TransactionId recentXmin,
									Relation relation,
									TransactionId *limit_xid,
									TimestampTz *limit_ts)
{
	TimestampTz ts;
	TransactionId xlimit = recentXmin;
	TransactionId latest_xmin;
	TimestampTz next_map_update_ts;
	TransactionId threshold_timestamp;
	TransactionId threshold_xid;

	Assert(TransactionIdIsNormal(recentXmin));
	Assert(OldSnapshotThresholdActive());
	Assert(limit_ts != NULL && limit_xid != NULL);

	/*
	 * TestForOldSnapshot() 假定提前修剪会推进页面 LSN，
	 * 故跳 WAL 时不能提前修剪。
	 */
	if (!RelationAllowsEarlyPruning(relation) || !RelationNeedsWAL(relation))
		return false;

	ts = GetSnapshotCurrentTimestamp();

	SpinLockAcquire(&oldSnapshotControl->mutex_latest_xmin);
	latest_xmin = oldSnapshotControl->latest_xmin;
	next_map_update_ts = oldSnapshotControl->next_map_update;
	SpinLockRelease(&oldSnapshotControl->mutex_latest_xmin);

	/*
	 * 零阈值在有效时总是覆盖为最新 xmin。没有启发式的
	 * 话，它会发现一个简单 UPDATE 使自己快照过旧
	 * ——这会让它对多数测试没用；无原则性办法确保
	 * 它不会这样失败。用 5 秒延迟试图得到有用测试行
	 * 为，但这可能需要调整。
	 * 。
	 */
	if (old_snapshot_threshold == 0)
	{
		if (TransactionIdPrecedes(latest_xmin, MyProc->xmin)
			&& TransactionIdFollows(latest_xmin, xlimit))
			xlimit = latest_xmin;

		ts -= 5 * USECS_PER_SEC;
	}
	else
	{
		ts = AlignTimestampToMinuteBoundary(ts)
			- (old_snapshot_threshold * USECS_PER_MINUTE);

		/* 不做 LW 锁的快速退出检查。 */
		SpinLockAcquire(&oldSnapshotControl->mutex_threshold);
		threshold_timestamp = oldSnapshotControl->threshold_timestamp;
		threshold_xid = oldSnapshotControl->threshold_xid;
		SpinLockRelease(&oldSnapshotControl->mutex_threshold);

		if (ts == threshold_timestamp)
		{
			/*
			 * 当前时间戳与上次应用的限制同桶。复用。
			 * 。
			 */
			xlimit = threshold_xid;
		}
		else if (ts == next_map_update_ts)
		{
			/*
			 * FIXME：此分支很可疑 —— 但应单独修。
			 * 。
			 */
			xlimit = latest_xmin;
		}
		else if (GetOldSnapshotFromTimeMapping(ts, &xlimit))
		{
		}

		/*
		 * 针对活动事务清理工作的失效保护。
		 *
		 * 这不是断言，因为为性能我们没用自旋锁，
		 * 这留下了 xlimit 可能已推进并更最新的可能；但
		 * 应用这个限制似乎是审慎的。它可能让修剪比本来
		 * 略保守一点，但能防住数据丢失缺陷。
		 * 。
		 */
		if (TransactionIdIsNormal(latest_xmin)
			&& TransactionIdPrecedes(latest_xmin, xlimit))
			xlimit = latest_xmin;
	}

	if (TransactionIdIsValid(xlimit) &&
		TransactionIdFollowsOrEquals(xlimit, recentXmin))
	{
		*limit_ts = ts;
		*limit_xid = xlimit;

		return true;
	}

	return false;
}

/*
 * 照管把时间映射到 xid 的循环缓冲区。
 */
void
MaintainOldSnapshotTimeMapping(TimestampTz whenTaken, TransactionId xmin)
{
	TimestampTz ts;
	TransactionId latest_xmin;
	TimestampTz update_ts;
	bool		map_update_required = false;

	/* 禁用旧快照检查时绝不要调用本函数。 */
	Assert(old_snapshot_threshold >= 0);

	ts = AlignTimestampToMinuteBoundary(whenTaken);

	/*
	 * 跟踪任何进程见到的最新 xmin。跨越桶边界时用新值
	 * 更新映射。
	 */
	SpinLockAcquire(&oldSnapshotControl->mutex_latest_xmin);
	latest_xmin = oldSnapshotControl->latest_xmin;
	update_ts = oldSnapshotControl->next_map_update;
	if (ts > update_ts)
	{
		oldSnapshotControl->next_map_update = ts;
		map_update_required = true;
	}
	if (TransactionIdFollows(xmin, latest_xmin))
		oldSnapshotControl->latest_xmin = xmin;
	SpinLockRelease(&oldSnapshotControl->mutex_latest_xmin);

	/* 我们只需要更新最近的 xmin 值。 */
	if (!map_update_required)
		return;

	/* 0（用于测试）无需进一步跟踪。 */
	if (old_snapshot_threshold == 0)
		return;

	/*
	 * 我们不想用异常值做傻事，但也不想用警告弄脏日志
	 * 或破坏其正常处理；若某事看起来不合理，就
	 * 以 DEBUG 级别记录并直接返回。
	 * 。
	 */
	if (whenTaken < 0)
	{
		elog(DEBUG1,
			 "MaintainOldSnapshotTimeMapping called with negative whenTaken = %ld",
			 (long) whenTaken);
		return;
	}
	if (!TransactionIdIsNormal(xmin))
	{
		elog(DEBUG1,
			 "MaintainOldSnapshotTimeMapping called with xmin = %lu",
			 (unsigned long) xmin);
		return;
	}

	LWLockAcquire(OldSnapshotTimeMapLock, LW_EXCLUSIVE);

	Assert(oldSnapshotControl->head_offset >= 0);
	Assert(oldSnapshotControl->head_offset < OLD_SNAPSHOT_TIME_MAP_ENTRIES);
	Assert((oldSnapshotControl->head_timestamp % USECS_PER_MINUTE) == 0);
	Assert(oldSnapshotControl->count_used >= 0);
	Assert(oldSnapshotControl->count_used <= OLD_SNAPSHOT_TIME_MAP_ENTRIES);

	if (oldSnapshotControl->count_used == 0)
	{
		/* 为空映射设置第一个条目 */
		oldSnapshotControl->head_offset = 0;
		oldSnapshotControl->head_timestamp = ts;
		oldSnapshotControl->count_used = 1;
		oldSnapshotControl->xid_by_minute[0] = xmin;
	}
	else if (ts < oldSnapshotControl->head_timestamp)
	{
		/* 旧 ts；记 DEBUG */
		LWLockRelease(OldSnapshotTimeMapLock);
		elog(DEBUG1,
			 "MaintainOldSnapshotTimeMapping called with old whenTaken = %ld",
			 (long) whenTaken);
		return;
	}
	else if (ts <= (oldSnapshotControl->head_timestamp +
					((oldSnapshotControl->count_used - 1)
					 * USECS_PER_MINUTE)))
	{
		/* 已有映射；可能的话推进 xid */
		int			bucket = (oldSnapshotControl->head_offset
							  + ((ts - oldSnapshotControl->head_timestamp)
								 / USECS_PER_MINUTE))
			% OLD_SNAPSHOT_TIME_MAP_ENTRIES;

		if (TransactionIdPrecedes(oldSnapshotControl->xid_by_minute[bucket], xmin))
			oldSnapshotControl->xid_by_minute[bucket] = xmin;
	}
	else
	{
		/* 我们需要新桶，但它未必正好是下一个。 */
		int			distance_to_new_tail;
		int			distance_to_current_tail;
		int			advance;

		/*
		 * 目标是让映射的新"尾部"（即最新、离"头部"
		 * 条目最远的条目）对应 "ts"。每分钟一桶，
		 * 头部与新尾部的距离就是 ts 距当前
		 * head_timestamp 的分钟差。
		 * 。
		 * 。
		 *
		 * 从当前头部到当前尾部的距离比映射条目数少一，
		 * 因为 head_offset 处的条目对应 head_timestamp
		 * 之后 0 分钟。
		 *
		 * 这两个值之差就是我们要推进映射的分钟数，
		 * 方法是加入新条目或把旧条目轮换出去。
		 * 。
		 */
		distance_to_new_tail =
			(ts - oldSnapshotControl->head_timestamp) / USECS_PER_MINUTE;
		distance_to_current_tail =
			oldSnapshotControl->count_used - 1;
		advance = distance_to_new_tail - distance_to_current_tail;
		Assert(advance > 0);

		if (advance >= OLD_SNAPSHOT_TIME_MAP_ENTRIES)
		{
			/* 推进过远，旧数据皆垃圾；重来。 */
			oldSnapshotControl->head_offset = 0;
			oldSnapshotControl->count_used = 1;
			oldSnapshotControl->xid_by_minute[0] = xmin;
			oldSnapshotControl->head_timestamp = ts;
		}
		else
		{
			/* 把新值存进一个或多个桶。 */
			int			i;

			for (i = 0; i < advance; i++)
			{
				if (oldSnapshotControl->count_used == OLD_SNAPSHOT_TIME_MAP_ENTRIES)
				{
					/* 映射已满且新值替换旧头部。 */
					int			old_head = oldSnapshotControl->head_offset;

					if (old_head == (OLD_SNAPSHOT_TIME_MAP_ENTRIES - 1))
						oldSnapshotControl->head_offset = 0;
					else
						oldSnapshotControl->head_offset = old_head + 1;
					oldSnapshotControl->xid_by_minute[old_head] = xmin;
					oldSnapshotControl->head_timestamp += USECS_PER_MINUTE;
				}
				else
				{
					/* 扩展到未用条目。 */
					int			new_tail = (oldSnapshotControl->head_offset
											+ oldSnapshotControl->count_used)
						% OLD_SNAPSHOT_TIME_MAP_ENTRIES;

					oldSnapshotControl->count_used++;
					oldSnapshotControl->xid_by_minute[new_tail] = xmin;
				}
			}
		}
	}

	LWLockRelease(OldSnapshotTimeMapLock);
}


/*
 * 建立一个替代常规目录快照的快照，使目录访问的行
 * 为就像在过去某个时点一样。
 *
 * 逻辑解码需要它。
 */
void
SetupHistoricSnapshot(Snapshot historic_snapshot, HTAB *tuplecids)
{
	Assert(historic_snapshot != NULL);

	/* 设置时间旅行快照 */
	HistoricSnapshot = historic_snapshot;

	/* (cmin,cmax) 查找哈希 */
	tuplecid_data = tuplecids;
}


/*
 * 让目录快照恢复正常行为。
 */
void
TeardownHistoricSnapshot(bool is_error)
{
	HistoricSnapshot = NULL;
	tuplecid_data = NULL;
}

bool
HistoricSnapshotActive(void)
{
	return HistoricSnapshot != NULL;
}

HTAB *
HistoricSnapshotGetTupleCids(void)
{
	Assert(HistoricSnapshotActive());
	return tuplecid_data;
}

/*
 * EstimateSnapshotSpace
 *		返回存储给定快照所需的大小。
 *
 * 我们只从快照导出必需字段，存入
 * SerializedSnapshotData
 */
Size
EstimateSnapshotSpace(Snapshot snapshot)
{
	Size		size;

	Assert(snapshot != InvalidSnapshot);
	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);

	/* 在同一个 palloc 块中分配所需的 XID 数组。 */
	size = add_size(sizeof(SerializedSnapshotData),
					mul_size(snapshot->xcnt, sizeof(TransactionId)));
	if (snapshot->subxcnt > 0 &&
		(!snapshot->suboverflowed || snapshot->takenDuringRecovery))
		size = add_size(size,
						mul_size(snapshot->subxcnt, sizeof(TransactionId)));

	return size;
}

/*
 * SerializeSnapshot
 *		把序列化的快照（从给定快照中抽取）转储到
 *		start_address 所指内存。
 */
void
SerializeSnapshot(Snapshot snapshot, char *start_address)
{
	SerializedSnapshotData serialized_snapshot;

	Assert(snapshot->subxcnt >= 0);

	/* 复制所有必需字段 */
	serialized_snapshot.xmin = snapshot->xmin;
	serialized_snapshot.xmax = snapshot->xmax;
	serialized_snapshot.xcnt = snapshot->xcnt;
	serialized_snapshot.subxcnt = snapshot->subxcnt;
	serialized_snapshot.suboverflowed = snapshot->suboverflowed;
	serialized_snapshot.takenDuringRecovery = snapshot->takenDuringRecovery;
	serialized_snapshot.curcid = snapshot->curcid;
	serialized_snapshot.whenTaken = snapshot->whenTaken;
	serialized_snapshot.lsn = snapshot->lsn;

	/*
	 * 如果 SubXID 数组已溢出则忽略之，除非快照是在恢复
	 * 期间取得的 —— 那种情况下顶层 XID 也在 subxip 里，
	 * 我们不能丢掉它们。
	 */
	if (serialized_snapshot.suboverflowed && !snapshot->takenDuringRecovery)
		serialized_snapshot.subxcnt = 0;

	/* 结构复制到可能未对齐缓冲区 */
	memcpy(start_address,
		   &serialized_snapshot, sizeof(SerializedSnapshotData));

	/* XID 数组 */
	if (snapshot->xcnt > 0)
		memcpy((TransactionId *) (start_address +
								  sizeof(SerializedSnapshotData)),
			   snapshot->xip, snapshot->xcnt * sizeof(TransactionId));

	/*
	 * 复制 SubXID 数组。溢出时无需复制它，因为那时
	 * 不会被用到。但若是在恢复期间取得的快照，
	 * 所有顶层 XID 也都在 subxip 里，所以不能丢掉它们。
	 * 。
	 */
	if (serialized_snapshot.subxcnt > 0)
	{
		Size		subxipoff = sizeof(SerializedSnapshotData) +
			snapshot->xcnt * sizeof(TransactionId);

		memcpy((TransactionId *) (start_address + subxipoff),
			   snapshot->subxip, snapshot->subxcnt * sizeof(TransactionId));
	}
}

/*
 * RestoreSnapshot
 *		从指定地址恢复一个序列化的快照。
 *
 * 副本 palloc 于 TopTransactionContext，初始引用计数为
 * 0。返回的快照设置了已复制标志。
 */
Snapshot
RestoreSnapshot(char *start_address)
{
	SerializedSnapshotData serialized_snapshot;
	Size		size;
	Snapshot	snapshot;
	TransactionId *serialized_xids;

	memcpy(&serialized_snapshot, start_address,
		   sizeof(SerializedSnapshotData));
	serialized_xids = (TransactionId *)
		(start_address + sizeof(SerializedSnapshotData));

	/* 在同一个 palloc 块中分配所需的 XID 数组。 */
	size = sizeof(SnapshotData)
		+ serialized_snapshot.xcnt * sizeof(TransactionId)
		+ serialized_snapshot.subxcnt * sizeof(TransactionId);

	/* 复制所有必需字段 */
	snapshot = (Snapshot) MemoryContextAlloc(TopTransactionContext, size);
	snapshot->snapshot_type = SNAPSHOT_MVCC;
	snapshot->xmin = serialized_snapshot.xmin;
	snapshot->xmax = serialized_snapshot.xmax;
	snapshot->xip = NULL;
	snapshot->xcnt = serialized_snapshot.xcnt;
	snapshot->subxip = NULL;
	snapshot->subxcnt = serialized_snapshot.subxcnt;
	snapshot->suboverflowed = serialized_snapshot.suboverflowed;
	snapshot->takenDuringRecovery = serialized_snapshot.takenDuringRecovery;
	snapshot->curcid = serialized_snapshot.curcid;
	snapshot->whenTaken = serialized_snapshot.whenTaken;
	snapshot->lsn = serialized_snapshot.lsn;
	snapshot->snapXactCompletionCount = 0;

	/* 复制 XID，若有 */
	if (serialized_snapshot.xcnt > 0)
	{
		snapshot->xip = (TransactionId *) (snapshot + 1);
		memcpy(snapshot->xip, serialized_xids,
			   serialized_snapshot.xcnt * sizeof(TransactionId));
	}

	/* 复制 SubXID，若有 */
	if (serialized_snapshot.subxcnt > 0)
	{
		snapshot->subxip = ((TransactionId *) (snapshot + 1)) +
			serialized_snapshot.xcnt;
		memcpy(snapshot->subxip, serialized_xids + serialized_snapshot.xcnt,
			   serialized_snapshot.subxcnt * sizeof(TransactionId));
	}

	/* 设置已复制标志，让调用者正确设置引用计数。 */
	snapshot->regd_count = 0;
	snapshot->active_count = 0;
	snapshot->copied = true;

	return snapshot;
}

/*
 * 安装一个恢复出来的快照作为事务快照。
 *
 * 第二参数的类型是 void *，这样 snapmgr.h 就无需包含
 * PGPROC 的声明。
 */
void
RestoreTransactionSnapshot(Snapshot snapshot, void *source_pgproc)
{
	SetTransactionSnapshot(snapshot, NULL, InvalidPid, source_pgproc);
}

/*
 * XidInMVCCSnapshot
 *		按快照判断给定 XID 是否仍在进行中？
 *
 * 注意：GetSnapshotData 从不把我们自己后端的顶层
 * xid 或子 xid 存进快照，所以这些 xid 不会被本函数
 * 报为"正在运行"。这对现有用途没问题，因为我们总
 * 是先检查 TransactionIdIsCurrentTransactionId，除非
 * 不可能属于我们。
 */
bool
XidInMVCCSnapshot(TransactionId xid, Snapshot snapshot)
{
	/*
	 * 做一个快速范围检查，无需查看 xip 数组就能排除大
	 * 多数 XID。注意即使我们在下面把子事务 XID 转换成
	 * 其父事务的 XID，这也是可以的，因为 XID < xmin 的
	 * 子事务必然也有 XID < xmin 的父事务，而 XID >= xmax
	 * 的必然属于本快照时刻尚未提交的父事务。
	 */

	/* xid < xmin 不在进行中 */
	if (TransactionIdPrecedes(xid, snapshot->xmin))
		return false;
	/* xid >= xmax 在进行中 */
	if (TransactionIdFollowsOrEquals(xid, snapshot->xmax))
		return true;

	/*
	 * 在恢复期间取得的快照中，快照信息的存储方式略有
	 * 不同。
	 */
	if (!snapshot->takenDuringRecovery)
	{
		/*
		 * 若快照有完整子事务数据，最快的检查是比较
		 * 给定 XID 与子事务 XID 及顶层 XID。若快照已溢出，
		 * 就必须用 pg_subtrans 把子事务 XID 换成父 XID，但
		 * 那时只需看顶层 XID，
		 * 不必看子事务。
		 */
		if (!snapshot->suboverflowed)
		{
			/* 有完整数据，故查 subxip */
			if (pg_lfind32(xid, snapshot->subxip, snapshot->subxcnt))
				return true;

			/* 不在其中，继续搜索 xip[] */
		}
		else
		{
			/*
			 * 快照已溢出，把 xid 转成顶层。这安全，上面已
			 * 淘汰了过老的 XID。
			 */
			xid = SubTransGetTopmostTransaction(xid);

			/*
			 * 若 xid 确是子事务，现在可能得到 xid <
			 * xmin，故重查以避免扫描数组。无需重查
			 * xmax
			 */
			if (TransactionIdPrecedes(xid, snapshot->xmin))
				return false;
		}

		if (pg_lfind32(xid, snapshot->xip, snapshot->xcnt))
			return true;
	}
	else
	{
		/*
		 * 恢复期我们把所有 xid 存进 subxip 数组，因为它
		 * 大得多，且多半不知道哪些 xid 是顶层哪些是
		 * 子事务。xip 数组为空。
		 *
		 * 如果溢出了，我们先搜索 subtrans。
		 */
		if (snapshot->suboverflowed)
		{
			/*
			 * 快照已溢出，把 xid 转成顶层。这安全，上面已
			 * 淘汰了过老的 XID。
			 */
			xid = SubTransGetTopmostTransaction(xid);

			/*
			 * 若 xid 确是子事务，现在可能得到 xid <
			 * xmin，故重查以避免扫描数组。无需重查
			 * xmax
			 */
			if (TransactionIdPrecedes(xid, snapshot->xmin))
				return false;
		}

		/*
		 * 现在要么有顶层 xid 大于 xmin，要么有不
		 * 确定的 xid。不知它是顶层还是子事务，但这无
		 * 妨：只要它在场，该 xid 就可见。
		 */
		if (pg_lfind32(xid, snapshot->subxip, snapshot->subxcnt))
			return true;
	}

	return false;
}
