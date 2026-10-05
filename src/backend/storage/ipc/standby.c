/*-------------------------------------------------------------------------
 *
 * standby.c
 *	  热备模式下使用的杂项函数。
 *
 *	处理 RM_STANDBY_ID 的全部函数，涉及
 *	AccessExclusiveLocks 以及为热备模式建立快照。
 *	另有冲突恢复处理。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/standby.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/slot.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/standby.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* 用户可设置的 GUC 参数 */
int			max_standby_archive_delay = 30 * 1000;
int			max_standby_streaming_delay = 30 * 1000;
bool		log_recovery_conflict_waits = false;

/*
 * 跟踪原事务持有的全部排他锁。
 * 每个已知排他锁在 RecoveryLockHash 哈希表里都有一条
 * RecoveryLockEntry。属于同一给定 XID 的全部 RecoveryLockEntrys
 * 串成一条链，以便查找。
 * 每个已知持有此类锁的原事务，在 RecoveryLockXidHash
 * 哈希表里都有一条 RecoveryLockXidEntry，
 * 其中存放该事务锁链的链头。
 */
typedef struct RecoveryLockEntry
{
	xl_standby_lock key;		/* 哈希键：xid, dbOid, relOid */
	struct RecoveryLockEntry *next; /* 链节 */
} RecoveryLockEntry;

typedef struct RecoveryLockXidEntry
{
	TransactionId xid;			/* 哈希键 -- 必须放在首位 */
	struct RecoveryLockEntry *head; /* 链头 */
} RecoveryLockXidEntry;

static HTAB *RecoveryLockHash = NULL;
static HTAB *RecoveryLockXidHash = NULL;

/* 超时处理函数设置的标志 */
static volatile sig_atomic_t got_standby_deadlock_timeout = false;
static volatile sig_atomic_t got_standby_delay_timeout = false;
static volatile sig_atomic_t got_standby_lock_timeout = false;

static void ResolveRecoveryConflictWithVirtualXIDs(VirtualTransactionId *waitlist,
												   ProcSignalReason reason,
												   uint32 wait_event_info,
												   bool report_waiting);
static void SendRecoveryConflictWithBufferPin(ProcSignalReason reason);
static XLogRecPtr LogCurrentRunningXacts(RunningTransactions CurrRunningXacts);
static void LogAccessExclusiveLocks(int nlocks, xl_standby_lock *locks);
static const char *get_recovery_conflict_desc(ProcSignalReason reason);

/*
 * InitRecoveryTransactionEnvironment
 *		初始化对主库进行中事务的跟踪。
 *
 * 我们需要发出共享失效并持有锁。持有锁
 * 意味着别人可能要等我们，因此需要像真实事务那样建立一条锁表
 * vxact 项。可以为每个事务创建并删除
 * 锁表项，但更简单的做法是只创建
 * 一条永久项并始终留着。然后按需
 * 获取和释放锁。是的，这意味着一旦运行过本函数，就能在
 * pg_locks 里看到启动进程。
 */
void
InitRecoveryTransactionEnvironment(void)
{
	VirtualTransactionId vxid;
	HASHCTL		hash_ctl;

	Assert(RecoveryLockHash == NULL);	/* 不要运行两次 */

	/*
	 * 初始化哈希表，用以跟踪每个事务
	 * 持有的锁。
	 */
	hash_ctl.keysize = sizeof(xl_standby_lock);
	hash_ctl.entrysize = sizeof(RecoveryLockEntry);
	RecoveryLockHash = hash_create("RecoveryLockHash",
								   64,
								   &hash_ctl,
								   HASH_ELEM | HASH_BLOBS);
	hash_ctl.keysize = sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(RecoveryLockXidEntry);
	RecoveryLockXidHash = hash_create("RecoveryLockXidHash",
									  64,
									  &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);

	/*
	 * 为启动进程初始化共享失效管理，
	 * 注意把自己登记为 sendOnly 进程，这样不必
	 * 读取消息，队列开始积满时也不会
	 * 收到信号。
	 */
	SharedInvalBackendInit(true);

	/*
	 * 为启动进程锁定一个虚拟事务 id。
	 *
	 * 需要调用 GetNextLocalTransactionId()，因为
	 * SharedInvalBackendInit() 留下的 localTransactionId 无效，锁
	 * 管理器完全不能接受。
	 *
	 * 注意不必运行 XactLockTableInsert()，因为没有人
	 * 需要等待 xid。这听起来有点奇怪，但表锁
	 * 由 vxid 持有，行级锁由 xid 持有。所有查询
	 * 都持有 AccessShareLocks，因此在我们写入或锁定新行时绝不会阻塞。
	 */
	vxid.backendId = MyBackendId;
	vxid.localTransactionId = GetNextLocalTransactionId();
	VirtualXactLockTableInsert(vxid);

	standbyState = STANDBY_INITIALIZED;
}

/*
 * ShutdownRecoveryTransactionEnvironment
 *		关闭事务跟踪
 *
 * 准备从热备模式切换到正常运行。关闭
 * 恢复期间的事务跟踪。
 *
 * 即便在关闭启动进程时，只要事务
 * 跟踪已经初始化，也必须调用本函数。否则被跟踪
 * 事务持有的某些锁不会释放，并可能在启动进程退出时
 * 干扰仍在运行（但很快也会退出）的
 * 进程。
 */
void
ShutdownRecoveryTransactionEnvironment(void)
{
	/*
	 * 若 RecoveryLockHash 为 NULL 则什么都不做，因为这意味着
	 * 事务跟踪尚未初始化，或已经
	 * 关闭。这样在进程退出期间即便重复调用本
	 * 函数也是安全的。
	 */
	if (RecoveryLockHash == NULL)
		return;

	/* 把所有被跟踪的进行中事务标记为已结束。 */
	ExpireAllKnownAssignedTransactionIds();

	/* 释放被跟踪事务所持有的全部锁 */
	StandbyReleaseAllLocks();

	/* 销毁锁哈希表。 */
	hash_destroy(RecoveryLockHash);
	hash_destroy(RecoveryLockXidHash);
	RecoveryLockHash = NULL;
	RecoveryLockXidHash = NULL;

	/* 清理我们的 VirtualTransaction */
	VirtualXactLockTableCleanup();
}


/*
 * -----------------------------------------------------
 *		备库等待计时器与后端取消逻辑
 * -----------------------------------------------------
 */

/*
 * 确定我们希望开始取消冲突
 * 事务的截止时刻。若愿意一直等待，则返回零（一个肯定处于过去的
 * 时刻）。
 */
static TimestampTz
GetStandbyLimitTime(void)
{
	TimestampTz rtime;
	bool		fromStream;

	/*
	 * 截止时刻是最近一次收到 WAL 数据的时间加上相应的
	 * 延迟变量。延迟为 -1 表示一直等待。
	 */
	GetXLogReceiptTime(&rtime, &fromStream);
	if (fromStream)
	{
		if (max_standby_streaming_delay < 0)
			return 0;			/* 一直等待 */
		return TimestampTzPlusMilliseconds(rtime, max_standby_streaming_delay);
	}
	else
	{
		if (max_standby_archive_delay < 0)
			return 0;			/* 一直等待 */
		return TimestampTzPlusMilliseconds(rtime, max_standby_archive_delay);
	}
}

#define STANDBY_INITIAL_WAIT_US  1000
static int	standbyWait_us = STANDBY_INITIAL_WAIT_US;

/*
 * ResolveRecoveryConflictWithVirtualXIDs 的备库等待逻辑。
 * 在此等待一会儿然后返回。若判定不能再等待，
 * 则返回 true；若还能再等一会儿，则返回 false。
 */
static bool
WaitExceedsMaxStandbyDelay(uint32 wait_event_info)
{
	TimestampTz ltime;

	CHECK_FOR_INTERRUPTS();

	/* 是否已超过截止时刻？ */
	ltime = GetStandbyLimitTime();
	if (ltime && GetCurrentTimestamp() >= ltime)
		return true;

	/*
	 * 睡眠一小会儿（这对避免忙等是必需的）。
	 */
	pgstat_report_wait_start(wait_event_info);
	pg_usleep(standbyWait_us);
	pgstat_report_wait_end();

	/*
	 * 逐步加长睡眠时间，但不超过 1 秒，因为
	 * 在某些平台上 pg_usleep 不可中断。
	 */
	standbyWait_us *= 2;
	if (standbyWait_us > 1000000)
		standbyWait_us = 1000000;

	return false;
}

/*
 * 记录恢复冲突。
 *
 * wait_start 是调用方开始等待的时间戳。
 * now 是调用本函数时的时间戳。
 * wait_list 是分配给冲突进程的
 * 虚拟事务 id 列表。still_waiting 表示
 * 启动进程是否仍在等待恢复冲突
 * 得到解决。
 */
void
LogRecoveryConflict(ProcSignalReason reason, TimestampTz wait_start,
					TimestampTz now, VirtualTransactionId *wait_list,
					bool still_waiting)
{
	long		secs;
	int			usecs;
	long		msecs;
	StringInfoData buf;
	int			nprocs = 0;

	/*
	 * 恢复冲突已经解决时，
	 * 不得再有冲突进程。
	 */
	Assert(still_waiting || wait_list == NULL);

	TimestampDifference(wait_start, now, &secs, &usecs);
	msecs = secs * 1000 + usecs / 1000;
	usecs = usecs % 1000;

	if (wait_list)
	{
		VirtualTransactionId *vxids;

		/* 构造冲突进程列表的字符串 */
		vxids = wait_list;
		while (VirtualTransactionIdIsValid(*vxids))
		{
			PGPROC	   *proc = BackendIdGetProc(vxids->backendId);

			/* 若目标后端不活跃，proc 可以为 NULL */
			if (proc)
			{
				if (nprocs == 0)
				{
					initStringInfo(&buf);
					appendStringInfo(&buf, "%d", proc->pid);
				}
				else
					appendStringInfo(&buf, ", %d", proc->pid);

				nprocs++;
			}

			vxids++;
		}
	}

	/*
	 * 若指定了 wait_list，则在详细信息中报告活跃
	 * 冲突后端的 PIDs 列表。注意若列表中的后端
	 * 都不活跃，则不记录详细信息。
	 */
	if (still_waiting)
	{
		ereport(LOG,
				errmsg("recovery still waiting after %ld.%03d ms: %s",
					   msecs, usecs, get_recovery_conflict_desc(reason)),
				nprocs > 0 ? errdetail_log_plural("Conflicting process: %s.",
												  "Conflicting processes: %s.",
												  nprocs, buf.data) : 0);
	}
	else
	{
		ereport(LOG,
				errmsg("recovery finished waiting after %ld.%03d ms: %s",
					   msecs, usecs, get_recovery_conflict_desc(reason)));
	}

	if (nprocs > 0)
		pfree(buf.data);
}

/*
 * 这是与恢复处理冲突的任何查询后端的主要执行者。
 * 具体 rmgr 内已经做出裁决。
 * 这里只向这些进程发出指令。这些进程
 * 再按指示抛出所需的错误。
 *
 * 若 report_waiting 为 true，则在 PS 显示中报告 "waiting"，并在
 * 必要时把等待恢复冲突记入日志。若
 * 由调用方负责报告，则 report_waiting 应为
 * false。否则调用方和本函数会意外地报告
 * 同一件事。
 */
static void
ResolveRecoveryConflictWithVirtualXIDs(VirtualTransactionId *waitlist,
									   ProcSignalReason reason, uint32 wait_event_info,
									   bool report_waiting)
{
	TimestampTz waitStart = 0;
	bool		waiting = false;
	bool		logged_recovery_conflict = false;

	/* 快速退出，若无事可做则避免一次内核调用。 */
	if (!VirtualTransactionIdIsValid(*waitlist))
		return;

	/* 设置用于报告的等待开始时间戳 */
	if (report_waiting && (log_recovery_conflict_waits || update_process_title))
		waitStart = GetCurrentTimestamp();

	while (VirtualTransactionIdIsValid(*waitlist))
	{
		/* 为每个等待的事务重置 standbyWait_us */
		standbyWait_us = STANDBY_INITIAL_WAIT_US;

		/* 一直等到该虚拟 xid 消失 */
		while (!VirtualXactLock(*waitlist, false))
		{
			/* 是否到了该杀掉它的时候？ */
			if (WaitExceedsMaxStandbyDelay(wait_event_info))
			{
				pid_t		pid;

				/*
				 * 现在找出该取消的是谁。
				 */
				Assert(VirtualTransactionIdIsValid(*waitlist));
				pid = CancelVirtualTransaction(*waitlist, reason);

				/*
				 * 稍等片刻让它退出，以免在系统负载很高时
				 * 向无响应的后端洪泛发送信号。
				 */
				if (pid != 0)
					pg_usleep(5000L);
			}

			if (waitStart != 0 && (!logged_recovery_conflict || !waiting))
			{
				TimestampTz now = 0;
				bool		maybe_log_conflict;
				bool		maybe_update_title;

				maybe_log_conflict = (log_recovery_conflict_waits && !logged_recovery_conflict);
				maybe_update_title = (update_process_title && !waiting);

				/* 若尚未报告，则取当前时间戳 */
				if (maybe_log_conflict || maybe_update_title)
					now = GetCurrentTimestamp();

				/*
				 * 若已等待超过 500
				 * 毫秒，则通过 ps 报告（这应当可配置吗？）
				 */
				if (maybe_update_title &&
					TimestampDifferenceExceeds(waitStart, now, 500))
				{
					set_ps_display_suffix("waiting");
					waiting = true;
				}

				/*
				 * 若启动进程等待恢复冲突的时间
				 * 超过 deadlock_timeout，则发出日志消息。
				 */
				if (maybe_log_conflict &&
					TimestampDifferenceExceeds(waitStart, now, DeadlockTimeout))
				{
					LogRecoveryConflict(reason, waitStart, now, waitlist, true);
					logged_recovery_conflict = true;
				}
			}
		}

		/* 该虚拟事务现已消失，等待下一个 */
		waitlist++;
	}

	/*
	 * 若恢复冲突已解决，但启动进程
	 * 等待它的时间超过了 deadlock_timeout，则发出日志消息。
	 */
	if (logged_recovery_conflict)
		LogRecoveryConflict(reason, waitStart, GetCurrentTimestamp(),
							NULL, false);

	/* 若曾添加后缀，则重置 ps 显示以去掉它 */
	if (waiting)
		set_ps_display_remove_suffix();

}

/*
 * 生成消除这类快照所需的恢复冲突：它们可能仍把
 * XIDs <= snapshotConflictHorizon 视为正在运行。
 *
 * 以 snapshotConflictHorizon 为截止点是生成
 * 细粒度恢复冲突的标准做法。注意此处把 InvalidTransactionId
 * 解释为 "肯定不需要任何冲突"，这是 WAL 记录可以（而且经常）
 * 依赖的一般约定。
 */
void
ResolveRecoveryConflictWithSnapshot(TransactionId snapshotConflictHorizon,
									bool isCatalogRel,
									RelFileLocator locator)
{
	VirtualTransactionId *backends;

	/*
	 * 若传入的是 InvalidTransactionId，则什么都不做（没有冲突）。
	 *
	 * 这可能发生在备库崩溃或重启后重放已经应用过的 WAL 记录时，
	 * 或重放一条把已经全部可见的页标记为冻结的 XLOG_HEAP2_VISIBLE
	 * 记录时。索引删除期间产生的记录
	 * 也相当常见
	 * （删除的原始执行可以推断：对删除操作而言足够的恢复冲突
	 * 必须发生在
	 * 重放删除记录本身之前）。
	 */
	if (!TransactionIdIsValid(snapshotConflictHorizon))
		return;

	Assert(TransactionIdIsNormal(snapshotConflictHorizon));
	backends = GetConflictingVirtualXIDs(snapshotConflictHorizon,
										 locator.dbOid);
	ResolveRecoveryConflictWithVirtualXIDs(backends,
										   PROCSIG_RECOVERY_CONFLICT_SNAPSHOT,
										   WAIT_EVENT_RECOVERY_CONFLICT_SNAPSHOT,
										   true);

	/*
	 * 注意这里没有考虑 WaitExceedsMaxStandbyDelay()
	 * （与上面的 ResolveRecoveryConflictWithVirtualXIDs() 不同）。这
	 * 似乎没问题，因为正常情况下不应当走到这类冲突，
	 * 例如因为使用了物理复制槽。
	 */
	if (wal_level >= WAL_LEVEL_LOGICAL && isCatalogRel)
		InvalidateObsoleteReplicationSlots(RS_INVAL_HORIZON, 0, locator.dbOid,
										   snapshotConflictHorizon);
}

/*
 * ResolveRecoveryConflictWithSnapshot 的变体，用于
 * FullTransactionId 值
 */
void
ResolveRecoveryConflictWithSnapshotFullXid(FullTransactionId snapshotConflictHorizon,
										   bool isCatalogRel,
										   RelFileLocator locator)
{
	/*
	 * ResolveRecoveryConflictWithSnapshot 处理的是 32 位 TransactionIds，
	 * 因此截断已记录的 FullTransactionId。若记录的值非常
	 * 旧，以至于已经发生过 XID 回卷，就不会再有
	 * 仍能看见它的快照。
	 */
	FullTransactionId nextXid = ReadNextFullTransactionId();
	uint64		diff;

	diff = U64FromFullTransactionId(nextXid) -
		U64FromFullTransactionId(snapshotConflictHorizon);
	if (diff < MaxTransactionId / 2)
	{
		TransactionId truncated;

		truncated = XidFromFullTransactionId(snapshotConflictHorizon);
		ResolveRecoveryConflictWithSnapshot(truncated,
											isCatalogRel,
											locator);
	}
}

void
ResolveRecoveryConflictWithTablespace(Oid tsid)
{
	VirtualTransactionId *temp_file_users;

	/*
	 * 备库用户当前可能正在用这个表空间存放自己的
	 * 临时文件。我们只关心当前用户，因为
	 * temp_tablespace 参数会直接忽略已经不再
	 * 存在的表空间。
	 *
	 * 让所有人立刻取消查询，以便确认没有
	 * 临时文件残留，从而可以删除该表空间。从轨道上把整个
	 * 现场炸掉，这是唯一能确定的办法。
	 *
	 * XXX：可以检查目录里的临时文件名，找出正在使用这个
	 * 表空间的活跃后端 pid。接着会
	 * 把这些 pid 转换成 VirtualXIDs，然后再尝试取消
	 * 它们。
	 *
	 * 我们不等待提交，因为删除表空间不是事务性的。
	 */
	temp_file_users = GetConflictingVirtualXIDs(InvalidTransactionId,
												InvalidOid);
	ResolveRecoveryConflictWithVirtualXIDs(temp_file_users,
										   PROCSIG_RECOVERY_CONFLICT_TABLESPACE,
										   WAIT_EVENT_RECOVERY_CONFLICT_TABLESPACE,
										   true);
}

void
ResolveRecoveryConflictWithDatabase(Oid dbid)
{
	/*
	 * 这里不调用 ResolveRecoveryConflictWithVirtualXIDs()，因为它
	 * 只等待事务，完全空闲的会话会挡住
	 * 我们。这种情况很少，所以尽量简单处理：不等待，
	 * 直接立刻把它们赶下去。
	 *
	 * 这里不需要加锁，因为我们已经取得
	 * AccessExclusiveLock。在此期间尝试连接的人会
	 * 在 InitPostgres() 中阻塞，然后看到
	 * 数据库已被删除就断开连接。
	 */
	while (CountDBBackends(dbid) > 0)
	{
		CancelDBBackends(dbid, PROCSIG_RECOVERY_CONFLICT_DATABASE, true);

		/*
		 * 稍等片刻让它们退出，以免在系统负载很高时
		 * 向无响应的后端洪泛发送。
		 */
		pg_usleep(10000);
	}
}

/*
 * ResolveRecoveryConflictWithLock 由 ProcSleep()
 * 调用，用来解决与其他持有关系锁的后端之间的冲突。
 *
 * 通常在 ProcSleep() 中完成的 WaitLatch 睡眠
 * （非 InHotStandby 时）改在这里执行，以便代码更清晰。
 *
 * 我们要么立刻解决冲突，要么设置超时，在
 * 耐心耗尽时唤醒自己。
 *
 * 解决冲突的办法是取消所有持有冲突
 * 锁的后端。由于我们已经排在授予该锁的队列中，其间不会再授予
 * 与我们冲突的新锁请求。
 *
 * 还必须检查涉及启动进程和
 * 热备后端进程的死锁。若在
 * 本函数中到达 deadlock_timeout，则所有持有冲突锁的后端
 * 都会被要求自行检查死锁。
 *
 * 若已启用日志但恢复冲突尚未
 * 记录，则 logging_conflict 应为 true。在到达 deadlock_timeout
 * 并发出死锁检查请求之后，若 logging_conflict 为 false，我们会再次等待
 * 锁释放的信号。
 * 否则不再等待就返回，以便调用方报告
 * 恢复冲突。这种情况下，本函数会再次被调用，
 * 且 logging_conflict=false（因为恢复冲突已经
 * 记录过），然后我们会再次等待锁被释放。
 */
void
ResolveRecoveryConflictWithLock(LOCKTAG locktag, bool logging_conflict)
{
	TimestampTz ltime;
	TimestampTz now;

	Assert(InHotStandby);

	ltime = GetStandbyLimitTime();
	now = GetCurrentTimestamp();

	/*
	 * 若这是启动进程开始等待该锁之后的第一次经过，则更新 waitStart。
	 * 等待期间每次调用
	 * ResolveRecoveryConflictWithLock() 时不应更新它。
	 *
	 * 把为了与 ltime 比较而取得的当前时间用作 waitStart
	 * （即本进程开始等待该锁的时刻）。由于
	 * 重新取当前时间会带来开销，我们复用
	 * 已经取得的时间以避免该开销。
	 *
	 * 注意更新 waitStart 时不持有锁表的
	 * 分区锁，以避免额外获取锁的开销。
	 * 这可能导致 pg_locks 中的 "waitstart" 在等待开始后的极短
	 * 时间内变为 NULL，尽管 "granted" 为 false。
	 * 实践中这没问题，因为可以假定用户多半会在
	 * 长时间等待该锁时才去看 "waitstart"。
	 */
	if (pg_atomic_read_u64(&MyProc->waitStart) == 0)
		pg_atomic_write_u64(&MyProc->waitStart, now);

	if (now >= ltime && ltime != 0)
	{
		/*
		 * 我们已经落后，所以尽快清出一条路。
		 */
		VirtualTransactionId *backends;

		backends = GetLockConflicts(&locktag, AccessExclusiveLock, NULL);

		/*
		 * 禁止 ResolveRecoveryConflictWithVirtualXIDs() 在 PS 显示中报告
		 * "waiting"：把它的参数 report_waiting 关掉，
		 * 因为调用方 WaitOnLock() 已经报告过了。
		 */
		ResolveRecoveryConflictWithVirtualXIDs(backends,
											   PROCSIG_RECOVERY_CONFLICT_LOCK,
											   PG_WAIT_LOCK | locktag.locktag_type,
											   false);
	}
	else
	{
		/*
		 * 等待（或再次等待）直到 ltime，并且若等待时间将超过
		 * deadlock_timeout，则同时检查死锁
		 */
		EnableTimeoutParams timeouts[2];
		int			cnt = 0;

		if (ltime != 0)
		{
			got_standby_lock_timeout = false;
			timeouts[cnt].id = STANDBY_LOCK_TIMEOUT;
			timeouts[cnt].type = TMPARAM_AT;
			timeouts[cnt].fin_time = ltime;
			cnt++;
		}

		got_standby_deadlock_timeout = false;
		timeouts[cnt].id = STANDBY_DEADLOCK_TIMEOUT;
		timeouts[cnt].type = TMPARAM_AFTER;
		timeouts[cnt].delay_ms = DeadlockTimeout;
		cnt++;

		enable_timeouts(timeouts, cnt);
	}

	/* 等待关系锁释放时发出的信号 */
	ProcWaitForSignal(PG_WAIT_LOCK | locktag.locktag_type);

	/*
	 * 若已到达 ltime 则退出。随后，所有持有冲突
	 * 锁的后端会在下一次 ResolveRecoveryConflictWithLock()
	 * 调用中被取消。
	 */
	if (got_standby_lock_timeout)
		goto cleanup;

	if (got_standby_deadlock_timeout)
	{
		VirtualTransactionId *backends;

		backends = GetLockConflicts(&locktag, AccessExclusiveLock, NULL);

		/* 若无事可做则快速退出 */
		if (!VirtualTransactionIdIsValid(*backends))
			goto cleanup;

		/*
		 * 向所有持有冲突锁的后端发送信号，
		 * 让它们自行检查死锁。
		 */
		while (VirtualTransactionIdIsValid(*backends))
		{
			SignalVirtualTransaction(*backends,
									 PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,
									 false);
			backends++;
		}

		/*
		 * 若已启用日志但恢复冲突尚未记录，则退出，
		 * 以便调用方去记录。随后
		 * RecoveryConflictWithLock() 会再次被调用，我们会再次等待
		 * 锁被释放。
		 */
		if (logging_conflict)
			goto cleanup;

		/*
		 * 在此再次等待关系锁释放的信号，
		 * 以防随后的 RecoveryConflictWithLock() 再次触发
		 * deadlock_timeout 并再次发送死锁检查请求。
		 * 否则该请求会每个 deadlock_timeout 持续发送，
		 * 直到关系锁被释放或到达 ltime。
		 */
		got_standby_deadlock_timeout = false;
		ProcWaitForSignal(PG_WAIT_LOCK | locktag.locktag_type);
	}

cleanup:

	/*
	 * 清除上面建立的所有超时请求。这里假定
	 * 启动进程除本函数使用的超时外，没有其他未完成的超时。
	 * 若这一假定不再成立，可以逐个取消
	 * 这些超时，但那样会更慢。
	 */
	disable_all_timeouts(false);
	got_standby_lock_timeout = false;
	got_standby_deadlock_timeout = false;
}

/*
 * ResolveRecoveryConflictWithBufferPin 由 LockBufferForCleanup()
 * 调用，用来解决与其他持有缓冲区钉住的后端之间的冲突。
 *
 * 通常在 LockBufferForCleanup() 中完成的 ProcWaitForSignal() 睡眠
 * （非 InHotStandby 时）改在这里执行，以便代码更清晰。
 *
 * 我们要么立刻解决冲突，要么设置超时，在
 * 耐心耗尽时唤醒自己。
 *
 * 解决冲突的办法是向所有后端发送 PROCSIG 信号，检查它们是否
 * 持有挡住启动进程的某个缓冲区钉住。若是，
 * 这些后端会采取适当的错误动作，ERROR 或 FATAL。
 *
 * 还必须检查死锁。死锁的产生是因为：若查询
 * 在等待一把锁，该锁必定排在某个 AccessExclusiveLock 之后，而后者只能
 * 在启动进程重放一条事务完成记录后才能清除。
 * 若启动进程也在等待，那就是死锁。死锁
 * 可能发生在查询正在等待、然后启动进程睡眠时，或者
 * 启动进程正在睡眠、而查询去等待一把锁时。这里只防范
 * 前一种顺序，后一种顺序在
 * 查询睡眠之前由 CheckRecoveryConflictDeadlock() 检查。
 *
 * 死锁极其少见，检查起来也相对昂贵，
 * 所以不立刻做死锁检查 ... 只有在已经不得不等待
 * 至少 deadlock_timeout 时才检查。
 */
void
ResolveRecoveryConflictWithBufferPin(void)
{
	TimestampTz ltime;

	Assert(InHotStandby);

	ltime = GetStandbyLimitTime();

	if (GetCurrentTimestamp() >= ltime && ltime != 0)
	{
		/*
		 * 我们已经落后，所以尽快清出一条路。
		 */
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);
	}
	else
	{
		/*
		 * 在 ltime 醒来，并且若等待时间将超过
		 * deadlock_timeout，则同时检查死锁
		 */
		EnableTimeoutParams timeouts[2];
		int			cnt = 0;

		if (ltime != 0)
		{
			timeouts[cnt].id = STANDBY_TIMEOUT;
			timeouts[cnt].type = TMPARAM_AT;
			timeouts[cnt].fin_time = ltime;
			cnt++;
		}

		got_standby_deadlock_timeout = false;
		timeouts[cnt].id = STANDBY_DEADLOCK_TIMEOUT;
		timeouts[cnt].type = TMPARAM_AFTER;
		timeouts[cnt].delay_ms = DeadlockTimeout;
		cnt++;

		enable_timeouts(timeouts, cnt);
	}

	/*
	 * 等待 UnpinBuffer() 的信号，或等待上面建立的某个超时
	 * 打断这次等待。
	 *
	 * 假定这里只有 UnpinBuffer() 和上面建立的超时请求
	 * 能把我们唤醒。walreceiver 或
	 * SIGHUP 信号处理函数等调用的 WakeupRecovery() 做不到这一点，因为它用的
	 * 锁存器与 ProcWaitForSignal() 等待的不是同一个。
	 */
	ProcWaitForSignal(PG_WAIT_BUFFER_PIN);

	if (got_standby_delay_timeout)
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);
	else if (got_standby_deadlock_timeout)
	{
		/*
		 * 向热备后端发出请求，让它们自行检查
		 * 死锁。
		 *
		 * XXX 随后的 ResolveRecoveryConflictWithBufferPin() 会再次等待
		 * UnpinBuffer() 的信号，并在发生 deadlock_timeout 时再次发送
		 * 死锁检查请求。这会导致
		 * 该请求每个 deadlock_timeout 持续发送，直到
		 * 缓冲区被解除钉住或到达 ltime。这会增加
		 * 启动进程和各后端的工作量。实践中也许
		 * 危害不大，因为缓冲区保持钉住的时间
		 * 基本上并不长。但我们应当修掉这个问题吗？
		 */
		SendRecoveryConflictWithBufferPin(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);
	}

	/*
	 * 清除上面建立的所有超时请求。这里假定
	 * 启动进程除本函数使用的超时外没有其他超时。
	 * 若这一假定不再成立，可以逐个取消
	 * 这些超时，但那样会更慢。
	 */
	disable_all_timeouts(false);
	got_standby_delay_timeout = false;
	got_standby_deadlock_timeout = false;
}

static void
SendRecoveryConflictWithBufferPin(ProcSignalReason reason)
{
	Assert(reason == PROCSIG_RECOVERY_CONFLICT_BUFFERPIN ||
		   reason == PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);

	/*
	 * 向所有后端发送信号，询问它们是否持有
	 * 拖延启动进程的那个缓冲区钉住。还不能设置
	 * 冲突标志，因为大多数后端是无辜的。让每个后端自己的
	 * SIGUSR1 处理去决定它们的命运。
	 */
	CancelDBBackends(InvalidOid, reason, false);
}

/*
 * 在热备中提前检测死锁。若即将在持有启动
 * 进程正在等待的缓冲区钉住时睡眠，就中止这次锁
 * 等待。
 *
 * 注意：这段代码是悲观的，因为它无法
 * 判定是否真的存在死锁：我们
 * 需要等待的锁可能与启动进程持有的任何锁都无关。
 * 迟早应当拆掉这套机制，改为在 DeadLockCheck() 中以某种方式
 * 把缓冲区锁也算进去。不过这里的错误
 * 在实践中概率似乎很低，所以眼下不值得
 * 为此费事。
 */
void
CheckRecoveryConflictDeadlock(void)
{
	Assert(!InRecovery);		/* 不要在启动进程中调用 */

	if (!HoldingBufferPinThatDelaysRecovery())
		return;

	/*
	 * 错误消息应与 ProcessInterrupts() 一致，但这里不调用
	 * 它，因为此时并不是在处理中断。注意
	 * 这里只取消当前事务，因此若处于
	 * 子事务中且钉住由父事务持有，那么启动
	 * 进程仍会继续等待，尽管我们已经避免了死锁。
	 */
	ereport(ERROR,
			(errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
			 errmsg("canceling statement due to conflict with recovery"),
			 errdetail("User transaction caused buffer deadlock with recovery.")));
}


/* --------------------------------
 *		超时处理例程
 * --------------------------------
 */

/*
 * 若超过 STANDBY_DEADLOCK_TIMEOUT，将调用
 * StandbyDeadLockHandler()。
 */
void
StandbyDeadLockHandler(void)
{
	got_standby_deadlock_timeout = true;
}

/*
 * 若超过 STANDBY_TIMEOUT，将调用 StandbyTimeoutHandler()。
 */
void
StandbyTimeoutHandler(void)
{
	got_standby_delay_timeout = true;
}

/*
 * 若超过 STANDBY_LOCK_TIMEOUT，将调用 StandbyLockTimeoutHandler()。
 */
void
StandbyLockTimeoutHandler(void)
{
	got_standby_lock_timeout = true;
}

/*
 * -----------------------------------------------------
 * 恢复模式下的锁
 * -----------------------------------------------------
 *
 * 全部锁由启动进程用一个虚拟
 * 事务持有。这种实现既更简单，在某些意义上
 * 也更正确。所持的锁表示 "某个原事务持有
 * 过这把锁，因此此时不允许查询访问"。因此启动
 * 进程是实现这些原锁的代理。
 *
 * 我们只跟踪 AccessExclusiveLocks，它们始终只由
 * 一个事务在一个关系上持有。
 *
 * 已知锁记在 RecoveryLockHash 哈希表中。
 * 该表的作用是高效地去除重复锁，
 * 这很重要，因为检查点会再次报告已经持有的
 * 同一批锁。另外还有 RecoveryLockXidHash 表，每个 xid 一条，
 * 用来高效找出某个
 * 给定原事务持有的全部锁。
 *
 * 我们使用会话锁而不是普通锁，这样就不需要
 * ResourceOwners。
 */


void
StandbyAcquireAccessExclusiveLock(TransactionId xid, Oid dbOid, Oid relOid)
{
	RecoveryLockXidEntry *xidentry;
	RecoveryLockEntry *lockentry;
	xl_standby_lock key;
	LOCKTAG		locktag;
	bool		found;

	/* 已经处理过？ */
	if (!TransactionIdIsValid(xid) ||
		TransactionIdDidCommit(xid) ||
		TransactionIdDidAbort(xid))
		return;

	elog(trace_recovery(DEBUG4),
		 "adding recovery lock: db %u rel %u", dbOid, relOid);

	/* 锁定共享关系时 dbOid 为 InvalidOid。 */
	Assert(OidIsValid(relOid));

	/* 若还没有，则为这个 xid 创建一条哈希项。 */
	xidentry = hash_search(RecoveryLockXidHash, &xid, HASH_ENTER, &found);
	if (!found)
	{
		Assert(xidentry->xid == xid);	/* dynahash 应当已经设置好 */
		xidentry->head = NULL;
	}

	/* 为这把锁创建一条哈希项，除非已经有了。 */
	key.xid = xid;
	key.dbOid = dbOid;
	key.relOid = relOid;
	lockentry = hash_search(RecoveryLockHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* 这是新的，所以链入该 XID 的列表 ... */
		lockentry->next = xidentry->head;
		xidentry->head = lockentry;

		/* ... 并在本地获取该锁。 */
		SET_LOCKTAG_RELATION(locktag, dbOid, relOid);

		(void) LockAcquire(&locktag, AccessExclusiveLock, true, false);
	}
}

/*
 * 释放与这条 RecoveryLockXidEntry 关联的全部锁。
 */
static void
StandbyReleaseXidEntryLocks(RecoveryLockXidEntry *xidentry)
{
	RecoveryLockEntry *entry;
	RecoveryLockEntry *next;

	for (entry = xidentry->head; entry != NULL; entry = next)
	{
		LOCKTAG		locktag;

		elog(trace_recovery(DEBUG4),
			 "releasing recovery lock: xid %u db %u rel %u",
			 entry->key.xid, entry->key.dbOid, entry->key.relOid);
		/* 释放该锁 ... */
		SET_LOCKTAG_RELATION(locktag, entry->key.dbOid, entry->key.relOid);
		if (!LockRelease(&locktag, AccessExclusiveLock, true))
		{
			elog(LOG,
				 "RecoveryLockHash contains entry for lock no longer recorded by lock manager: xid %u database %u relation %u",
				 entry->key.xid, entry->key.dbOid, entry->key.relOid);
			Assert(false);
		}
		/* ... 并删除这条按锁存放的哈希项 */
		next = entry->next;
		hash_search(RecoveryLockHash, entry, HASH_REMOVE, NULL);
	}

	xidentry->head = NULL;		/* 只是出于谨慎 */
}

/*
 * 释放指定 XID 的锁；若为 InvalidXid，则释放全部锁。
 */
static void
StandbyReleaseLocks(TransactionId xid)
{
	RecoveryLockXidEntry *entry;

	if (TransactionIdIsValid(xid))
	{
		if ((entry = hash_search(RecoveryLockXidHash, &xid, HASH_FIND, NULL)))
		{
			StandbyReleaseXidEntryLocks(entry);
			hash_search(RecoveryLockXidHash, entry, HASH_REMOVE, NULL);
		}
	}
	else
		StandbyReleaseAllLocks();
}

/*
 * 从 xid 向下释放一棵事务树的锁，数据来自
 * RecoveryLockXidHash。
 *
 * 在热备模式下重放 COMMIT/ROLLBACK 的 WAL 时调用，
 * 用来去掉某事务所请求的 AccessExclusiveLocks。
 */
void
StandbyReleaseLockTree(TransactionId xid, int nsubxids, TransactionId *subxids)
{
	int			i;

	StandbyReleaseLocks(xid);

	for (i = 0; i < nsubxids; i++)
		StandbyReleaseLocks(subxids[i]);
}

/*
 * 在恢复结束时以及看到关闭检查点时调用。
 */
void
StandbyReleaseAllLocks(void)
{
	HASH_SEQ_STATUS status;
	RecoveryLockXidEntry *entry;

	elog(trace_recovery(DEBUG2), "release all standby locks");

	hash_seq_init(&status, RecoveryLockXidHash);
	while ((entry = hash_seq_search(&status)))
	{
		StandbyReleaseXidEntryLocks(entry);
		hash_search(RecoveryLockXidHash, entry, HASH_REMOVE, NULL);
	}
}

/*
 * StandbyReleaseOldLocks
 *		释放未在运行的顶层 XIDs 所持的备库锁，
 *		只要它们不是已准备事务。
 */
void
StandbyReleaseOldLocks(TransactionId oldxid)
{
	HASH_SEQ_STATUS status;
	RecoveryLockXidEntry *entry;

	hash_seq_init(&status, RecoveryLockXidHash);
	while ((entry = hash_seq_search(&status)))
	{
		Assert(TransactionIdIsValid(entry->xid));

		/* 若是已准备事务则跳过。 */
		if (StandbyTransactionIdIsPrepared(entry->xid))
			continue;

		/* 若 >= oldxid 则跳过。 */
		if (!TransactionIdPrecedes(entry->xid, oldxid))
			continue;

		/* 删除全部锁以及哈希表项。 */
		StandbyReleaseXidEntryLocks(entry);
		hash_search(RecoveryLockXidHash, entry, HASH_REMOVE, NULL);
	}
}

/*
 * --------------------------------------------------------------------
 *		资源管理器 RM_STANDBY_ID 的恢复处理
 *
 * 这些记录类型仅在 XLogStandbyInfoActive() 时才会创建
 * --------------------------------------------------------------------
 */

void
standby_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* 备库记录不使用备份块 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	/* 若不在热备模式则什么都不做 */
	if (standbyState == STANDBY_DISABLED)
		return;

	if (info == XLOG_STANDBY_LOCK)
	{
		xl_standby_locks *xlrec = (xl_standby_locks *) XLogRecGetData(record);
		int			i;

		for (i = 0; i < xlrec->nlocks; i++)
			StandbyAcquireAccessExclusiveLock(xlrec->locks[i].xid,
											  xlrec->locks[i].dbOid,
											  xlrec->locks[i].relOid);
	}
	else if (info == XLOG_RUNNING_XACTS)
	{
		xl_running_xacts *xlrec = (xl_running_xacts *) XLogRecGetData(record);
		RunningTransactionsData running;

		running.xcnt = xlrec->xcnt;
		running.subxcnt = xlrec->subxcnt;
		running.subxid_status = xlrec->subxid_overflow ? SUBXIDS_MISSING : SUBXIDS_IN_ARRAY;
		running.nextXid = xlrec->nextXid;
		running.latestCompletedXid = xlrec->latestCompletedXid;
		running.oldestRunningXid = xlrec->oldestRunningXid;
		running.xids = xlrec->xids;

		ProcArrayApplyRecoveryInfo(&running);

		/*
		 * 启动进程目前没有方便的办法安排
		 * 统计信息的报告。XLOG_RUNNING_XACTS 记录以
		 * 固定节奏发出，因此这里是报告统计信息的方便位置。
		 * 虽然 wal_level=minimal 时不会生成这些记录，但
		 * WAL 重放期间也无法访问统计信息。
		 */
		pgstat_report_stat(true);
	}
	else if (info == XLOG_INVALIDATIONS)
	{
		xl_invalidations *xlrec = (xl_invalidations *) XLogRecGetData(record);

		ProcessCommittedInvalidationMessages(xlrec->msgs,
											 xlrec->nmsgs,
											 xlrec->relcacheInitFileInval,
											 xlrec->dbId,
											 xlrec->tsId);
	}
	else
		elog(PANIC, "standby_redo: unknown op code %u", info);
}

/*
 * 把当前快照的细节记入 WAL。这样备库和逻辑解码
 * 都能重建快照状态。
 *
 * 热备按如下方式使用它：
 *
 * 若从关闭检查点启动，可以直接进入 STANDBY_SNAPSHOT_READY，
 * 因为我们知道那时没有任何东西在运行，
 * 恢复快照已知为空。更典型的
 * 在线检查点情形下，要拿到正确的恢复快照得费一番
 * 周折，这是两步，或者
 * 有时是三步过程。
 *
 * 初始快照必须包含备库上某一时刻全部正在运行的 xid 以及全部当前
 * AccessExclusiveLocks。在服务器运行期间组装
 * 这些信息会用到许多不同的
 * LWLocks，因此我们选择逐段推导这些信息，
 * 再在备库上重新组装。当这些
 * 信息全部组装完成时，我们进入 STANDBY_SNAPSHOT_READY。
 *
 * 由于在主库上推导这些信息时加锁并不
 * 严格，我们注意到：推导与
 * 把推导结果写入 WAL 之间存在一个时间窗口。由此带来
 * 必须解决的竞态，因为在该窗口内 xid 和锁可能进入或离开
 * 快照。由此产生的问题是：某个 xid 或锁可能
 * 在快照推导完成 *之后*、却在快照被记入
 * running xacts WAL 记录 *之前* 才开始。解决办法是
 * 在主库上、于推导快照之前的某一点就开始积累变更，
 * 稍后应用 running xacts 记录中的快照时再忽略重复项。
 * 这在 CreateCheckPoint() 期间实现：我们把逻辑检查点位置
 * 当作起点，然后紧挨着
 * 主检查点 WAL 记录之前写入 running xacts 记录。由于我们总是从
 * 检查点启动，并且立刻就在起点上，
 * 因此我们无条件进入 STANDBY_INITIALIZED。
 * 过了这一点之后，我们必须做
 * 4 件事：
 *	* 看到新的 xid 时把共享的 nextXid 向前推进
 *	* 每个新 xid 都扩展 clog 和 subtrans
 *	* 跟踪未提交的 known assigned xid
 *	* 跟踪未提交的 AccessExclusiveLocks
 *
 * 看到提交/中止时，必须去掉正在完成的事务中的
 * known assigned xid 和锁。找不到对应项的删除尝试是预期之中的，
 * 处于 STANDBY_INITIALIZED 状态时不得因此报错。
 * 这在 StandbyReleaseLocks() 和
 * KnownAssignedXidsRemove() 中实现。
 *
 * 稍后应用 running xact 数据时，必须小心忽略
 * 已经提交的事务，因为那些提交在
 * 生成 WAL 记录时抢先发生了。
 *
 * 宽松的时序还意味着可能记录到 xid 为
 * 零的锁，因为 xid 会在锁被去掉之前先从 proc 中去掉。
 * 因此必须修剪锁列表，确保只为
 * 当前正在运行的 xid 持有锁，由 StandbyReleaseOldLocks() 完成。
 * 零 xid 按理已不可能，但我们可能在重放
 * 它们仍可能出现的那个时期的 WAL。
 *
 * 逻辑解码只需 running xacts 信息；
 * 不必查看加锁信息，但反正也会记下来，
 * 因为没有单独的开关只打开逻辑解码。关于
 * 其用法的细节，见 snapbuild.c 开头的注释。
 *
 *
 * 返回最后插入记录的 RecPtr。
 */
XLogRecPtr
LogStandbySnapshot(void)
{
	XLogRecPtr	recptr;
	RunningTransactions running;
	xl_standby_lock *locks;
	int			nlocks;

	Assert(XLogStandbyInfoActive());

	/*
	 * 取得此刻正被持有的所有 AccessExclusiveLocks 的细节。
	 */
	locks = GetRunningTransactionLocks(&nlocks);
	if (nlocks > 0)
		LogAccessExclusiveLocks(nlocks, locks);
	pfree(locks);

	/*
	 * 记录所有进行中事务的细节。这应当是我们写入的最后一条
	 * 记录，因为备库看到它就会开放查询。
	 */
	running = GetRunningTransactionData();

	/*
	 * GetRunningTransactionData() 取得了 ProcArrayLock，必须释放它。
	 * 对热备而言，可以在插入 WAL 记录之前做这件事，
	 * 因为 ProcArrayApplyRecoveryInfo() 会用
	 * clog 重新检查提交状态。但对逻辑解码，不能过早
	 * 释放这把锁，因为从历史快照的视角看，clog 可能 "在未来"。
	 * 否则会出现这种情况：我们正在等待
	 * xl_running_xacts 记录中列出的某个事务结束，
	 * 而按 WAL 来看，它在 xl_running_xacts
	 * 记录之前就已经提交。好在本例程执行并不频繁，而且
	 * 只是一把共享锁。
	 */
	if (wal_level < WAL_LEVEL_LOGICAL)
		LWLockRelease(ProcArrayLock);

	recptr = LogCurrentRunningXacts(running);

	/* 若把锁留得更久，则释放它 ... */
	if (wal_level >= WAL_LEVEL_LOGICAL)
		LWLockRelease(ProcArrayLock);

	/* GetRunningTransactionData() 取得了 XidGenLock，必须释放它 */
	LWLockRelease(XidGenLock);

	return recptr;
}

/*
 * 把正在运行事务的增强快照记入 WAL。
 *
 * RunningTransactionsData 与 xl_running_xacts 的定义
 * 相似。分开保存是因为 xl_running_xacts 是一块连续
 * 内存，直到在 WAL 中组装完成前从不以完整形式存在。
 * 插入的记录被标记为对持久性不重要，
 * 以免触发多余的检查点 / 归档活动。
 */
static XLogRecPtr
LogCurrentRunningXacts(RunningTransactions CurrRunningXacts)
{
	xl_running_xacts xlrec;
	XLogRecPtr	recptr;

	xlrec.xcnt = CurrRunningXacts->xcnt;
	xlrec.subxcnt = CurrRunningXacts->subxcnt;
	xlrec.subxid_overflow = (CurrRunningXacts->subxid_status != SUBXIDS_IN_ARRAY);
	xlrec.nextXid = CurrRunningXacts->nextXid;
	xlrec.oldestRunningXid = CurrRunningXacts->oldestRunningXid;
	xlrec.latestCompletedXid = CurrRunningXacts->latestCompletedXid;

	/* 头部 */
	XLogBeginInsert();
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
	XLogRegisterData((char *) (&xlrec), MinSizeOfXactRunningXacts);

	/* TransactionIds 数组 */
	if (xlrec.xcnt > 0)
		XLogRegisterData((char *) CurrRunningXacts->xids,
						 (xlrec.xcnt + xlrec.subxcnt) * sizeof(TransactionId));

	recptr = XLogInsert(RM_STANDBY_ID, XLOG_RUNNING_XACTS);

	if (xlrec.subxid_overflow)
		elog(trace_recovery(DEBUG2),
			 "snapshot of %d running transactions overflowed (lsn %X/%X oldest xid %u latest complete %u next xid %u)",
			 CurrRunningXacts->xcnt,
			 LSN_FORMAT_ARGS(recptr),
			 CurrRunningXacts->oldestRunningXid,
			 CurrRunningXacts->latestCompletedXid,
			 CurrRunningXacts->nextXid);
	else
		elog(trace_recovery(DEBUG2),
			 "snapshot of %d+%d running transaction ids (lsn %X/%X oldest xid %u latest complete %u next xid %u)",
			 CurrRunningXacts->xcnt, CurrRunningXacts->subxcnt,
			 LSN_FORMAT_ARGS(recptr),
			 CurrRunningXacts->oldestRunningXid,
			 CurrRunningXacts->latestCompletedXid,
			 CurrRunningXacts->nextXid);

	/*
	 * 确保 running_xacts 信息在不太远的
	 * 将来同步到磁盘。但不想因此拖住任何事情（即不使用 XLogFlush()），
	 * 所以在正常运行期间让 WAL 写入器来做。
	 * XLogSetAsyncXactLSN() 会顺手把该 LSN 标为待同步，
	 * 并在 WALWriter 睡眠时推动它行动。关于为何没有它
	 * 记录可能不被刷出的细节，见
	 * XLogBackgroundFlush()。
	 */
	XLogSetAsyncXactLSN(recptr);

	return recptr;
}

/*
 * 批量记录 AccessExclusiveLocks。其他锁类型不必
 * 记录，见 backend/storage/lmgr/README。
 */
static void
LogAccessExclusiveLocks(int nlocks, xl_standby_lock *locks)
{
	xl_standby_locks xlrec;

	xlrec.nlocks = nlocks;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, offsetof(xl_standby_locks, locks));
	XLogRegisterData((char *) locks, nlocks * sizeof(xl_standby_lock));
	XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);

	(void) XLogInsert(RM_STANDBY_ID, XLOG_STANDBY_LOCK);
}

/*
 * 在 LockAcquire() 期间逐条记录 AccessExclusiveLocks
 */
void
LogAccessExclusiveLock(Oid dbOid, Oid relOid)
{
	xl_standby_lock xlrec;

	xlrec.xid = GetCurrentTransactionId();

	xlrec.dbOid = dbOid;
	xlrec.relOid = relOid;

	LogAccessExclusiveLocks(1, &xlrec);
	MyXactFlags |= XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK;
}

/*
 * 准备记录一把 AccessExclusiveLock，供 LockAcquire() 期间使用
 */
void
LogAccessExclusiveLockPrepare(void)
{
	/*
	 * 确保本事务已经分配了 TransactionId，有
	 * 两个原因，都与备库上的锁释放有关。首先，必须
	 * 分配一个 xid，这样 RecordTransactionCommit() 和
	 * RecordTransactionAbort() 才不会把恢复赖以释放锁的事务
	 * 完成记录优化掉。这是个
	 * 权宜之计，但这个边角情形不值得往主
	 * 提交路径里加代码。其次，必须在锁被记入
	 * 共享内存之前分配 xid，否则并发执行的
	 * GetRunningTransactionLocks() 可能看到一把与
	 * InvalidTransactionId 关联的锁，而我们稍后会断言这不可能发生。
	 */
	(void) GetCurrentTransactionId();
}

/*
 * 为失效消息发出 WAL。目前只用于没有
 * xid 但包含失效消息的提交。
 */
void
LogStandbyInvalidations(int nmsgs, SharedInvalidationMessage *msgs,
						bool relcacheInitFileInval)
{
	xl_invalidations xlrec;

	/* 准备记录 */
	memset(&xlrec, 0, sizeof(xlrec));
	xlrec.dbId = MyDatabaseId;
	xlrec.tsId = MyDatabaseTableSpace;
	xlrec.relcacheInitFileInval = relcacheInitFileInval;
	xlrec.nmsgs = nmsgs;

	/* 执行插入 */
	XLogBeginInsert();
	XLogRegisterData((char *) (&xlrec), MinSizeOfInvalidations);
	XLogRegisterData((char *) msgs,
					 nmsgs * sizeof(SharedInvalidationMessage));
	XLogInsert(RM_STANDBY_ID, XLOG_INVALIDATIONS);
}

/* 返回恢复冲突的描述 */
static const char *
get_recovery_conflict_desc(ProcSignalReason reason)
{
	const char *reasonDesc = _("unknown reason");

	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			reasonDesc = _("recovery conflict on buffer pin");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			reasonDesc = _("recovery conflict on lock");
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			reasonDesc = _("recovery conflict on tablespace");
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			reasonDesc = _("recovery conflict on snapshot");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
			reasonDesc = _("recovery conflict on replication slot");
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			reasonDesc = _("recovery conflict on buffer deadlock");
			break;
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:
			reasonDesc = _("recovery conflict on database");
			break;
		default:
			break;
	}

	return reasonDesc;
}
