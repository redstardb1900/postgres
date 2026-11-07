/*-------------------------------------------------------------------------
 * launcher.c
 *	   PostgreSQL 逻辑复制工作进程启动器进程    
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/launcher.c
 *
 * NOTES
 *	  本模块包含逻辑复制工作进程启动器，它            
 *	  利用后台工作进程基础设施，为每个启用        
 *	  的订阅启动逻辑复制工作进程。         
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/fork_process.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"

/* 循环间最长休眠时间（3min）*/
#define DEFAULT_NAPTIME_PER_CYCLE 180000L

/* GUC 变量    */
int			max_logical_replication_workers = 4;
int			max_sync_workers_per_subscription = 2;
int			max_parallel_apply_workers_per_subscription = 2;

LogicalRepWorker *MyLogicalRepWorker = NULL;

typedef struct LogicalRepCtxStruct
{
	/* 监督进程。     */
	pid_t		launcher_pid;

	/* 哈希表，保存订阅应用工作进程的最近启动时间      */
	dsa_handle	last_start_dsa;
	dshash_table_handle last_start_dsh;

	/* 后台工作进程  */
	LogicalRepWorker workers[FLEXIBLE_ARRAY_MEMBER];
} LogicalRepCtxStruct;

static LogicalRepCtxStruct *LogicalRepCtx;

/* last-start-times 共享哈希表中的条目       */
typedef struct LauncherLastStartTimesEntry
{
	Oid			subid;			/* 逻辑复制订阅 OID（哈希键） */
	TimestampTz last_start_time;	/* 应用工作进程上次启动时间   */
} LauncherLastStartTimesEntry;

/* last-start-times 共享哈希表的参数             */
static const dshash_parameters dsh_params = {
	sizeof(Oid),
	sizeof(LauncherLastStartTimesEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_LAUNCHER_HASH
};

static dsa_area *last_start_times_dsa = NULL;
static dshash_table *last_start_times = NULL;

static bool on_commit_launcher_wakeup = false;


static void ApplyLauncherWakeup(void);
static void logicalrep_launcher_onexit(int code, Datum arg);
static void logicalrep_worker_onexit(int code, Datum arg);
static void logicalrep_worker_detach(void);
static void logicalrep_worker_cleanup(LogicalRepWorker *worker);
static int	logicalrep_pa_worker_count(Oid subid);
static void logicalrep_launcher_attach_dshmem(void);
static void ApplyLauncherSetWorkerStartTime(Oid subid, TimestampTz start_time);
static TimestampTz ApplyLauncherGetWorkerStartTime(Oid subid);


/*
 * 加载订阅列表。          
 *
 * 只为每个订阅填充对工作进程启停函数有意义              
 * 的字段。      
 */
static List *
get_subscription_list(void)
{
	List	   *res = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext resultcxt;

	/* 这是我们将用于分配输出数据的内存上下文    */
	resultcxt = CurrentMemoryContext;

	/*
	 * 开启一个事务以便能访问 pg_database，并获取快照。   
	 * 我们本身并不使用该快照，但我们感兴趣的           
	 * 是它设置的 RecentGlobalXmin 这一副作用。（这对          
	 * 任何读取堆页面的操作都至关重要，因为 HOT 可能  
	 * 决定修剪它们，即便进程未尝试修改元组。）   
	 *
	 * FIXME：此注释不准确/代码有缺陷。未被推入/激活      
	 * 的快照无法可靠地阻止 HOT 修剪（->xmin 例如可能      
	 * 在处理缓存失效消息时被清除）。           
	 */
	StartTransactionCommand();
	(void) GetTransactionSnapshot();

	rel = table_open(SubscriptionRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_subscription subform = (Form_pg_subscription) GETSTRUCT(tup);
		Subscription *sub;
		MemoryContext oldcxt;

		/*
		 * 在调用者而非事务上下文中分配结果     
		 * 我们在循环内执行此操作，并在结束时恢复原始    
		 * 上下文，以免 heap_getnext() 这类有泄漏的对象在可
		 * 存活的上下文中被调用。              
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		sub = (Subscription *) palloc0(sizeof(Subscription));
		sub->oid = subform->oid;
		sub->dbid = subform->subdbid;
		sub->owner = subform->subowner;
		sub->enabled = subform->subenabled;
		sub->name = pstrdup(NameStr(subform->subname));
		/* 我们不填充自己不感兴趣的字段。  */

		res = lappend(res, sub);
		MemoryContextSwitchTo(oldcxt);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return res;
}

/*
 * 等待后台工作进程启动并附加到共享内存上下文。       
 *
 * 只有在该工作进程附加失败时，才需要用它来清理      
 * 共享内存。 
 *
 * 返回附加是否成功。               
 */
static bool
WaitForReplicationWorkerAttach(LogicalRepWorker *worker,
							   uint16 generation,
							   BackgroundWorkerHandle *handle)
{
	bool		result = false;
	bool		dropped_latch = false;

	for (;;)
	{
		BgwHandleStatus status;
		pid_t		pid;
		int			rc;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/* 工作进程已死或已启动。已死则返回 false。*/
		if (!worker->in_use || worker->proc)
		{
			result = worker->in_use;
			LWLockRelease(LogicalRepWorkerLock);
			break;
		}

		LWLockRelease(LogicalRepWorkerLock);

		/* 检查工作进程是否在附加前已死，并为其善后。   */
		status = GetBackgroundWorkerPid(handle, &pid);

		if (status == BGWH_STOPPED)
		{
			LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);
			/* 确保这确实是我们等待的工作进程。      */
			if (generation == worker->generation)
				logicalrep_worker_cleanup(worker);
			LWLockRelease(LogicalRepWorkerLock);
			break;				/* 结果已为假。      */
		}

		/*
		 * 我们需要超时，因为工作进程附加时一般不经由  
		 * 锁存器得到通知。但我们预计不需要等待太久。    
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			dropped_latch = true;
		}
	}

	/*
	 * 如果为了等待而必须清除锁存器事件，务必在退      
	 * 出前恢复，否则调用者可能错过事件。  
	 */
	if (dropped_latch)
		SetLatch(MyLatch);

	return result;
}

/*
 * 遍历工作进程数组，搜索匹配给定订阅 id 与     
 * relid 的工作进程。  
 *
 * 我们只关心领导应用工作进程或表同步工作进程。     
 */
LogicalRepWorker *
logicalrep_worker_find(Oid subid, Oid relid, bool only_running)
{
	int			i;
	LogicalRepWorker *res = NULL;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* 搜索给定订阅 id 的已附加工作进程。       */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		/* 跳过并行应用进程。  */
		if (isParallelApplyWorker(w))
			continue;

		if (w->in_use && w->subid == subid && w->relid == relid &&
			(!only_running || w->proc))
		{
			res = w;
			break;
		}
	}

	return res;
}

/*
 * 与 logicalrep_worker_find() 类似，但返回订阅的所有工作      
 * 进程列表，而不只是单个。  
 */
List *
logicalrep_workers_find(Oid subid, bool only_running)
{
	int			i;
	List	   *res = NIL;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* 搜索给定订阅 id 的已附加工作进程。       */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (w->in_use && w->subid == subid && (!only_running || w->proc))
			res = lappend(res, w);
	}

	return res;
}

/*
 * 尽可能启动新的逻辑复制后台工作进程。       
 *
 * 成功返回 true，失败返回 false。 
 */
bool
logicalrep_worker_launch(Oid dbid, Oid subid, const char *subname, Oid userid,
						 Oid relid, dsm_handle subworker_dsm)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	uint16		generation;
	int			i;
	int			slot = 0;
	LogicalRepWorker *worker = NULL;
	int			nsyncworkers;
	int			nparallelapplyworkers;
	TimestampTz now;
	bool		is_parallel_apply_worker = (subworker_dsm != DSM_HANDLE_INVALID);

	/* 健全性检查：tablesync 无法为子工作进程  */
	Assert(!(is_parallel_apply_worker && OidIsValid(relid)));

	ereport(DEBUG1,
			(errmsg_internal("starting logical replication worker for subscription \"%s\"",
							 subname)));

	/* 为一致性起见，在初始启动消息之后再报告它。 */
	if (max_replication_slots == 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("cannot start logical replication workers when max_replication_slots = 0")));

	/*
	 * 我们需要在锁的保护下修改共享内存，以便获得一    
	 * 致的视图。         
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

retry:
	/* 查找未用槽位。    */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (!w->in_use)
		{
			worker = w;
			slot = i;
			break;
		}
	}

	nsyncworkers = logicalrep_sync_worker_count(subid);

	now = GetCurrentTimestamp();

	/*
	 * 如果没有找到空闲槽位，则尝试进行垃圾回收。  
	 * 之所以这样做，是因为如果某个工作进程启动失败， 
	 * 且父进程在等待时已崩溃，in_use 状态从未清除。     
	 */
	if (worker == NULL || nsyncworkers >= max_sync_workers_per_subscription)
	{
		bool		did_cleanup = false;

		for (i = 0; i < max_logical_replication_workers; i++)
		{
			LogicalRepWorker *w = &LogicalRepCtx->workers[i];

			/*
			 * 如果工作进程被标记 in_use 却未能及时附加，   
			 * 则将其清理。
			 */
			if (w->in_use && !w->proc &&
				TimestampDifferenceExceeds(w->launch_time, now,
										   wal_receiver_timeout))
			{
				elog(WARNING,
					 "logical replication worker for subscription %u took too long to start; canceled",
					 w->subid);

				logicalrep_worker_cleanup(w);
				did_cleanup = true;
			}
		}

		if (did_cleanup)
			goto retry;
	}

	/*
	 * 一旦已达每个订阅的同步工作进程上限，我们就    
	 * 不允许再调用更多同步工作进程。因此只需静默返
	 * 回，原因可能是一个本身无害的竞态条件。      
	 */
	if (OidIsValid(relid) && nsyncworkers >= max_sync_workers_per_subscription)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return false;
	}

	nparallelapplyworkers = logicalrep_pa_worker_count(subid);

	/*
	 * 如果并行应用工作进程的数量已达每个订阅上限，    
	 * 则返回假。  
	 */
	if (is_parallel_apply_worker &&
		nparallelapplyworkers >= max_parallel_apply_workers_per_subscription)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return false;
	}

	/*
	 * 不过，如果已无空闲工作进程槽位，则在退出前     
	 * 告知用户。
	 */
	if (worker == NULL)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of logical replication worker slots"),
				 errhint("You might need to increase %s.", "max_logical_replication_workers")));
		return false;
	}

	/* 准备工作进程槽。 */
	worker->launch_time = now;
	worker->in_use = true;
	worker->generation++;
	worker->proc = NULL;
	worker->dbid = dbid;
	worker->userid = userid;
	worker->subid = subid;
	worker->relid = relid;
	worker->relstate = SUBREL_STATE_UNKNOWN;
	worker->relstate_lsn = InvalidXLogRecPtr;
	worker->stream_fileset = NULL;
	worker->leader_pid = is_parallel_apply_worker ? MyProcPid : InvalidPid;
	worker->parallel_apply = is_parallel_apply_worker;
	worker->last_lsn = InvalidXLogRecPtr;
	TIMESTAMP_NOBEGIN(worker->last_send_time);
	TIMESTAMP_NOBEGIN(worker->last_recv_time);
	worker->reply_lsn = InvalidXLogRecPtr;
	TIMESTAMP_NOBEGIN(worker->reply_time);

	/* 在释放锁之前记住 generation，以便将来识别。           */
	generation = worker->generation;

	LWLockRelease(LogicalRepWorkerLock);

	/* 注册新的动态工作进程。*/
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");

	if (is_parallel_apply_worker)
		snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ParallelApplyWorkerMain");
	else
		snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ApplyWorkerMain");

	if (OidIsValid(relid))
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "logical replication worker for subscription %u sync %u", subid, relid);
	else if (is_parallel_apply_worker)
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "logical replication parallel apply worker for subscription %u", subid);
	else
		snprintf(bgw.bgw_name, BGW_MAXLEN,
				 "logical replication apply worker for subscription %u", subid);

	if (is_parallel_apply_worker)
		snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication parallel worker");
	else
		snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication worker");

	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = Int32GetDatum(slot);

	if (is_parallel_apply_worker)
		memcpy(bgw.bgw_extra, &subworker_dsm, sizeof(dsm_handle));

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		/* 工作进程启动失败，因此清理该槽位。  */
		LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);
		Assert(generation == worker->generation);
		logicalrep_worker_cleanup(worker);
		LWLockRelease(LogicalRepWorkerLock);

		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of background worker slots"),
				 errhint("You might need to increase %s.", "max_worker_processes")));
		return false;
	}

	/* 现在等待它附加。    */
	return WaitForReplicationWorkerAttach(worker, generation, bgw_handle);
}

/*
 * 内部函数，用于停止工作进程并等待其从槽分离         
 * 。  
 */
static void
logicalrep_worker_stop_internal(LogicalRepWorker *worker, int signo)
{
	uint16		generation;

	Assert(LWLockHeldByMeInMode(LogicalRepWorkerLock, LW_SHARED));

	/*
	 * 记住我们的工作进程使用的是哪一代，以便检查        
	 * 所见是否未变。 
	 */
	generation = worker->generation;

	/*
	 * 如果我们找到的工作进程没有设置 proc，那么它仍  
	 * 在启动中；等它启动完成后再将其杀死。       
	 */
	while (worker->in_use && !worker->proc)
	{
		int			rc;

		LWLockRelease(LogicalRepWorkerLock);

		/* 稍等片刻 —— 预计不需要等太久。      */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		/* 复查 worker 状态。*/
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/*
		 * 检查工作进程槽位是否已不再被使用，这意味着  
		 * 该工作进程已退出，或者其 generation 是否已经   
		 * 不同，意味着另一个工作进程已接管该槽位。  
		 */
		if (!worker->in_use || worker->generation != generation)
			return;

		/* 工作进程已设置 proc，即已启动。 */
		if (worker->proc)
			break;
	}

	/* 现在终止工作进程…  */
	kill(worker->proc->pid, signo);

	/* …然后等待它死亡。 */
	for (;;)
	{
		int			rc;

		/* 已退出？*/
		if (!worker->proc || worker->generation != generation)
			break;

		LWLockRelease(LogicalRepWorkerLock);

		/* 稍等片刻 —— 预计不需要等太久。      */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_SHUTDOWN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
	}
}

/*
 * 停止 subid/relid 逻辑复制工作进程（若有）。  
 */
void
logicalrep_worker_stop(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, false);

	if (worker)
	{
		Assert(!isParallelApplyWorker(worker));
		logicalrep_worker_stop_internal(worker, SIGTERM);
	}

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * 停止给定的逻辑复制并行应用工作进程。   
 *
 * 注意该函数向并行应用工作进程发送 SIGUSR2                     
 * 而非 SIGTERM，让它干净退出。  
 */
void
logicalrep_pa_worker_stop(ParallelApplyWorkerInfo *winfo)
{
	int			slot_no;
	uint16		generation;
	LogicalRepWorker *worker;

	SpinLockAcquire(&winfo->shared->mutex);
	generation = winfo->shared->logicalrep_worker_generation;
	slot_no = winfo->shared->logicalrep_worker_slot_no;
	SpinLockRelease(&winfo->shared->mutex);

	Assert(slot_no >= 0 && slot_no < max_logical_replication_workers);

	/*
	 * 在停止它之前，先脱离并行应用工作进程的           
	 * error_mq_handle。这可以防止领导应用工作进程尝试  
	 * 从可能已被并行应用工作进程脱离的错误队列           
	 * 接收消息。              
	 */
	if (winfo->error_mq_handle)
	{
		shm_mq_detach(winfo->error_mq_handle);
		winfo->error_mq_handle = NULL;
	}

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = &LogicalRepCtx->workers[slot_no];
	Assert(isParallelApplyWorker(worker));

	/*
	 * 仅当 generation 匹配且工作进程仍存活时才停止。        
	 */
	if (worker->generation == generation && worker->proc)
		logicalrep_worker_stop_internal(worker, SIGUSR2);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * 唤醒（用锁存器）指定 sub/rel 的任一逻辑复制工作进程。
 */
void
logicalrep_worker_wakeup(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, true);

	if (worker)
		logicalrep_worker_wakeup_ptr(worker);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * 唤醒（用锁存器）指定的逻辑复制工作进程。   
 *
 * 调用者必须持锁，否则 worker->proc 会中途改变。 
 */
void
logicalrep_worker_wakeup_ptr(LogicalRepWorker *worker)
{
	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	SetLatch(&worker->proc->procLatch);
}

/*
 * 附加到槽。  
 */
void
logicalrep_worker_attach(int slot)
{
	/* 阻止并发访问。    */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	Assert(slot >= 0 && slot < max_logical_replication_workers);
	MyLogicalRepWorker = &LogicalRepCtx->workers[slot];

	if (!MyLogicalRepWorker->in_use)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication worker slot %d is empty, cannot attach",
						slot)));
	}

	if (MyLogicalRepWorker->proc)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication worker slot %d is already used by "
						"another worker, cannot attach", slot)));
	}

	MyLogicalRepWorker->proc = MyProc;
	before_shmem_exit(logicalrep_worker_onexit, (Datum) 0);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * 若有并行应用工作进程则停止之，并脱离领导应用        
 * 工作进程（清理）。 
 */
static void
logicalrep_worker_detach(void)
{
	/* 停止并行应用工作进程。*/
	if (am_leader_apply_worker())
	{
		List	   *workers;
		ListCell   *lc;

		/*
		 * 在终止所有并行应用工作进程之前，先从        
		 * error_mq_handle 脱离。这可避免领导应用工作进程再次
		 * 接收进程终止消息并写日志（若并行工作进程已
		 * 这样做）。                                      
		 */
		pa_detach_all_error_mq();

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		workers = logicalrep_workers_find(MyLogicalRepWorker->subid, true);
		foreach(lc, workers)
		{
			LogicalRepWorker *w = (LogicalRepWorker *) lfirst(lc);

			if (isParallelApplyWorker(w))
				logicalrep_worker_stop_internal(w, SIGTERM);
		}

		LWLockRelease(LogicalRepWorkerLock);
	}

	/* 阻止并发访问。    */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	logicalrep_worker_cleanup(MyLogicalRepWorker);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * 清理进程信息。
 */
static void
logicalrep_worker_cleanup(LogicalRepWorker *worker)
{
	Assert(LWLockHeldByMeInMode(LogicalRepWorkerLock, LW_EXCLUSIVE));

	worker->in_use = false;
	worker->proc = NULL;
	worker->dbid = InvalidOid;
	worker->userid = InvalidOid;
	worker->subid = InvalidOid;
	worker->relid = InvalidOid;
	worker->leader_pid = InvalidPid;
	worker->parallel_apply = false;
}

/*
 * 逻辑复制启动器的清理函数。           
 *
 * 在逻辑复制启动器退出时调用。  
 */
static void
logicalrep_launcher_onexit(int code, Datum arg)
{
	LogicalRepCtx->launcher_pid = 0;
}

/*
 * 清理函数。  
 *
 * 逻辑复制工作进程退出时调用。
 */
static void
logicalrep_worker_onexit(int code, Datum arg)
{
	/* 从远端优雅地断开连接。           */
	if (LogRepWorkerWalRcvConn)
		walrcv_disconnect(LogRepWorkerWalRcvConn);

	logicalrep_worker_detach();

	/* 清理用于流式传输事务的文件集。    */
	if (MyLogicalRepWorker->stream_fileset != NULL)
		FileSetDeleteAll(MyLogicalRepWorker->stream_fileset);

	/*
	 * 会话级锁可在并行应用模式下于事务外获取，且
	 * 在工作进程终止时不被释放，因此需在工作进
	 * 程退出前手动释放所有锁。                              
	 *
	 * 这些锁将在工作进程初始化后被获取。       
	 */
	if (!InitializingApplyWorker)
		LockReleaseAll(DEFAULT_LOCKMETHOD, true);

	ApplyLauncherWakeup();
}

/*
 * 统计订阅已注册（未必在运行）的同步工作进程      
 * 的数量。       
 */
int
logicalrep_sync_worker_count(Oid subid)
{
	int			i;
	int			res = 0;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* 搜索给定订阅 id 的已附加工作进程。       */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (w->subid == subid && OidIsValid(w->relid))
			res++;
	}

	return res;
}

/*
 * 统计某订阅已注册（但未必正在运行）的并行应用         
 * 工作进程数量。      
 */
static int
logicalrep_pa_worker_count(Oid subid)
{
	int			i;
	int			res = 0;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/*
	 * 扫描所有已附加的并行应用工作进程，只统计其中 
	 * 含给定订阅 id 的那些。
	 */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (w->subid == subid && isParallelApplyWorker(w))
			res++;
	}

	return res;
}

/*
 * ApplyLauncherShmemSize
 *		计算复制启动器共享内存所需的空间           
 */
Size
ApplyLauncherShmemSize(void)
{
	Size		size;

	/*
	 * 需要固定结构体和 LogicalRepWorker 数组。     
	 */
	size = sizeof(LogicalRepCtxStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(max_logical_replication_workers,
								   sizeof(LogicalRepWorker)));
	return size;
}

/*
 * ApplyLauncherRegister
 *		注册一个运行逻辑复制启动器的后台工作进程。       
 */
void
ApplyLauncherRegister(void)
{
	BackgroundWorker bgw;

	if (max_logical_replication_workers == 0)
		return;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ApplyLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN,
			 "logical replication launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN,
			 "logical replication launcher");
	bgw.bgw_restart_time = 5;
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&bgw);
}

/*
 * ApplyLauncherShmemInit
 *		分配并初始化复制启动器共享内存             
 */
void
ApplyLauncherShmemInit(void)
{
	bool		found;

	LogicalRepCtx = (LogicalRepCtxStruct *)
		ShmemInitStruct("Logical Replication Launcher Data",
						ApplyLauncherShmemSize(),
						&found);

	if (!found)
	{
		int			slot;

		memset(LogicalRepCtx, 0, ApplyLauncherShmemSize());

		LogicalRepCtx->last_start_dsa = DSA_HANDLE_INVALID;
		LogicalRepCtx->last_start_dsh = DSHASH_HANDLE_INVALID;

		/* 为每个工作进程槽初始化内存和自旋锁。 */
		for (slot = 0; slot < max_logical_replication_workers; slot++)
		{
			LogicalRepWorker *worker = &LogicalRepCtx->workers[slot];

			memset(worker, 0, sizeof(LogicalRepWorker));
			SpinLockInit(&worker->relmutex);
		}
	}
}

/*
 * 初始化或附加到存储最近启动时间的动态共享哈希   
 * 表（若尚未做过）。           
 * 必须在访问该表之前调用本函数。  
 */
static void
logicalrep_launcher_attach_dshmem(void)
{
	MemoryContext oldcontext;

	/* 若已做过则快速退出。     */
	if (LogicalRepCtx->last_start_dsh != DSHASH_HANDLE_INVALID &&
		last_start_times != NULL)
		return;

	/* 否则，使用锁来确保只有一个进程创建该表。        */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	/* 确保 DSA 例程分配的任何本地内存都是持久的。    */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (LogicalRepCtx->last_start_dsh == DSHASH_HANDLE_INVALID)
	{
		/* 为最近启动时间初始化动态共享哈希表。     */
		last_start_times_dsa = dsa_create(LWTRANCHE_LAUNCHER_DSA);
		dsa_pin(last_start_times_dsa);
		dsa_pin_mapping(last_start_times_dsa);
		last_start_times = dshash_create(last_start_times_dsa, &dsh_params, 0);

		/* 将句柄存储在共享内存中供其他后端使用。 */
		LogicalRepCtx->last_start_dsa = dsa_get_handle(last_start_times_dsa);
		LogicalRepCtx->last_start_dsh = dshash_get_hash_table_handle(last_start_times);
	}
	else if (!last_start_times)
	{
		/* 附加到已存在的动态共享哈希表。 */
		last_start_times_dsa = dsa_attach(LogicalRepCtx->last_start_dsa);
		dsa_pin_mapping(last_start_times_dsa);
		last_start_times = dshash_attach(last_start_times_dsa, &dsh_params,
										 LogicalRepCtx->last_start_dsh, 0);
	}

	MemoryContextSwitchTo(oldcontext);
	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * 设置订阅的最近启动时间。         
 */
static void
ApplyLauncherSetWorkerStartTime(Oid subid, TimestampTz start_time)
{
	LauncherLastStartTimesEntry *entry;
	bool		found;

	logicalrep_launcher_attach_dshmem();

	entry = dshash_find_or_insert(last_start_times, &subid, &found);
	entry->last_start_time = start_time;
	dshash_release_lock(last_start_times, entry);
}

/*
 * 返回订阅的最近启动时间，若没有则返回 0。              
 */
static TimestampTz
ApplyLauncherGetWorkerStartTime(Oid subid)
{
	LauncherLastStartTimesEntry *entry;
	TimestampTz ret;

	logicalrep_launcher_attach_dshmem();

	entry = dshash_find(last_start_times, &subid, false);
	if (entry == NULL)
		return 0;

	ret = entry->last_start_time;
	dshash_release_lock(last_start_times, entry);

	return ret;
}

/*
 * 移除该订阅的最近启动时间条目（若存在）。         
 *
 * 有两个用例：移除与已删除或已禁用订阅相关的      
 * 条目（只是为了避免泄漏共享内存），以及允许已退 
 * 出的应用工作进程立即重启，它因订阅参数变更  
 * 而退出。                          
 */
void
ApplyLauncherForgetWorkerStartTime(Oid subid)
{
	logicalrep_launcher_attach_dshmem();

	(void) dshash_delete_key(last_start_times, &subid);
}

/*
 * 若已请求，提交时唤醒启动器。 
 */
void
AtEOXact_ApplyLauncher(bool isCommit)
{
	if (isCommit)
	{
		if (on_commit_launcher_wakeup)
			ApplyLauncherWakeup();
	}

	on_commit_launcher_wakeup = false;
}

/*
 * 请求在事务提交时唤醒启动器。                  
 *
 * 本函数用于在当前事务提交时向启动器发送信号，   
 * 使其停止休眠并处理订阅。当新的元组被加入           
 * pg_subscription 目录时应当使用。       
*/
void
ApplyLauncherWakeupAtCommit(void)
{
	if (!on_commit_launcher_wakeup)
		on_commit_launcher_wakeup = true;
}

static void
ApplyLauncherWakeup(void)
{
	if (LogicalRepCtx->launcher_pid != 0)
		kill(LogicalRepCtx->launcher_pid, SIGUSR1);
}

/*
 * 应用启动器进程的主循环。     
 */
void
ApplyLauncherMain(Datum main_arg)
{
	ereport(DEBUG1,
			(errmsg_internal("logical replication launcher started")));

	before_shmem_exit(logicalrep_launcher_onexit, (Datum) 0);

	Assert(LogicalRepCtx->launcher_pid == 0);
	LogicalRepCtx->launcher_pid = MyProcPid;

	/* 建立信号处理程序。*/
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * 建立到固定目录的连接（仅访问 pg_subscription  
	 * ）。           
	 */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/* 进入主循环 */
	for (;;)
	{
		int			rc;
		List	   *sublist;
		ListCell   *lc;
		MemoryContext subctx;
		MemoryContext oldctx;
		long		wait_time = DEFAULT_NAPTIME_PER_CYCLE;

		CHECK_FOR_INTERRUPTS();

		/* 使用临时上下文以避免跨循环泄漏内存。       */
		subctx = AllocSetContextCreate(TopMemoryContext,
									   "Logical Replication Launcher sublist",
									   ALLOCSET_DEFAULT_SIZES);
		oldctx = MemoryContextSwitchTo(subctx);

		/* 为已启用的订阅启动缺少的工作进程。  */
		sublist = get_subscription_list();
		foreach(lc, sublist)
		{
			Subscription *sub = (Subscription *) lfirst(lc);
			LogicalRepWorker *w;
			TimestampTz last_start;
			TimestampTz now;
			long		elapsed;

			if (!sub->enabled)
				continue;

			LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
			w = logicalrep_worker_find(sub->oid, InvalidOid, false);
			LWLockRelease(LogicalRepWorkerLock);

			if (w != NULL)
				continue;		/* 工作进程已在运行  */

			/*
			 * 若该工作进程现在可启动，就启动它。否则，  
			 * 调整 wait_time，以便在它能启动时立即醒来    
			 * 。     
			 *
			 * 每个订阅的应用工作进程在每个                     
			 * wal_retrieve_retry_interval 内只能重启一次，以免    
			 * 错误使我们尽可能快地反复重启工作进程。在
			 * 重启属预期（如订阅参数变更）时另一进程
			 * 应移除其最近启动条目，使工作进程无需      
			 * 等待 wal_retrieve_retry_interval 过去即可重启   
			 * 。                                                       
			 */
			last_start = ApplyLauncherGetWorkerStartTime(sub->oid);
			now = GetCurrentTimestamp();
			if (last_start == 0 ||
				(elapsed = TimestampDifferenceMilliseconds(last_start, now)) >= wal_retrieve_retry_interval)
			{
				ApplyLauncherSetWorkerStartTime(sub->oid, now);
				if (!logicalrep_worker_launch(sub->dbid, sub->oid, sub->name,
											  sub->owner, InvalidOid,
											  DSM_HANDLE_INVALID))
				{
					/*
					 * 可能是未能启动工作进程（也许因为  
					 * 资源耗尽），也可能是启动它后立即  
					 * 退出了。无论哪种情况，似乎都适合在  
					 * wal_retrieve_retry_interval 过后  
					 * 重试。                   
					 */
					wait_time = Min(wait_time,
									wal_retrieve_retry_interval);
				}
			}
			else
			{
				wait_time = Min(wait_time,
								wal_retrieve_retry_interval - elapsed);
			}
		}

		/* 切换回原来的内存上下文。    */
		MemoryContextSwitchTo(oldctx);
		/* 清理临时内存。       */
		MemoryContextDelete(subctx);

		/* 等更多工作。  */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   wait_time,
					   WAIT_EVENT_LOGICAL_LAUNCHER_MAIN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}

	/* 不可达     */
}

/*
 * 当前进程是否为逻辑复制启动器？       
 */
bool
IsLogicalLauncher(void)
{
	return LogicalRepCtx->launcher_pid == MyProcPid;
}

/*
 * 如果给定的 pid 是并行应用工作进程的 pid，则返回        
 * 领导应用工作进程 pid，否则 InvalidPid。 
 */
pid_t
GetLeaderApplyWorkerPid(pid_t pid)
{
	int			leader_pid = InvalidPid;
	int			i;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (isParallelApplyWorker(w) && w->proc && pid == w->proc->pid)
		{
			leader_pid = w->leader_pid;
			break;
		}
	}

	LWLockRelease(LogicalRepWorkerLock);

	return leader_pid;
}

/*
 * 返回各订阅的状态。        
 */
Datum
pg_stat_get_subscription(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SUBSCRIPTION_COLS	9
	Oid			subid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	int			i;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	/* 确保我们获得工作进程的一致视图。 */
	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		/* 对每一行 */
		Datum		values[PG_STAT_GET_SUBSCRIPTION_COLS] = {0};
		bool		nulls[PG_STAT_GET_SUBSCRIPTION_COLS] = {0};
		int			worker_pid;
		LogicalRepWorker worker;

		memcpy(&worker, &LogicalRepCtx->workers[i],
			   sizeof(LogicalRepWorker));
		if (!worker.proc || !IsBackendPid(worker.proc->pid))
			continue;

		if (OidIsValid(subid) && worker.subid != subid)
			continue;

		worker_pid = worker.proc->pid;

		values[0] = ObjectIdGetDatum(worker.subid);
		if (OidIsValid(worker.relid))
			values[1] = ObjectIdGetDatum(worker.relid);
		else
			nulls[1] = true;
		values[2] = Int32GetDatum(worker_pid);

		if (isParallelApplyWorker(&worker))
			values[3] = Int32GetDatum(worker.leader_pid);
		else
			nulls[3] = true;

		if (XLogRecPtrIsInvalid(worker.last_lsn))
			nulls[4] = true;
		else
			values[4] = LSNGetDatum(worker.last_lsn);
		if (worker.last_send_time == 0)
			nulls[5] = true;
		else
			values[5] = TimestampTzGetDatum(worker.last_send_time);
		if (worker.last_recv_time == 0)
			nulls[6] = true;
		else
			values[6] = TimestampTzGetDatum(worker.last_recv_time);
		if (XLogRecPtrIsInvalid(worker.reply_lsn))
			nulls[7] = true;
		else
			values[7] = LSNGetDatum(worker.reply_lsn);
		if (worker.reply_time == 0)
			nulls[8] = true;
		else
			values[8] = TimestampTzGetDatum(worker.reply_time);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);

		/*
		 * 如果只请求了单个订阅且我们已找到它，则    
		 * 跳出
		 */
		if (OidIsValid(subid))
			break;
	}

	LWLockRelease(LogicalRepWorkerLock);

	return (Datum) 0;
}
