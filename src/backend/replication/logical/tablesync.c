/*-------------------------------------------------------------------------
 * tablesync.c
 *	  PostgreSQL 逻辑复制：初始表数据同步                   
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/tablesync.c
 *
 * NOTES
 *	  本文件包含逻辑复制的初始表数据同步               
 *	  代码。           
 *
 *	  初始数据同步是针对每张表单独进行的，             
 *	  由单独的应用工作进程完成，该进程只从发布者获取 
 *	  初始快照数据，然后与领导者应用工作进程               
 *	  同步流中的位置。
 *
 *	  以这种方式进行同步有几个原因：                    
 *	   - 它让我们可以并行化初始数据同步，            
 *		 从而缩短了所需的时间。             
 *	   - 初始同步无需在复制所有表数据所需的              
 *		 时间内持有 xid 和 LSN，与为整个数据库           
 *		 在单个进程中做全部同步相比，这减少了
 *		 膨胀并降低了磁盘消耗。                          
 *	   - 它让我们可以同步任何在初始表同步完成        
 *		 之后才添加的表。     
 *
 *	  流位置同步分多个步骤进行：                     
 *	   - 应用工作进程请求启动表同步工作进程，把新表   
 *		 状态设为 INIT。
 *	   - 表同步工作进程启动；表状态从 INIT 改为 DATASYNC，正在
 *		 复制  
 *	   - 表同步工作进程执行初始表复制；FINISHEDCOPY（表同步  
 *		 工作进程专属）状态用于指示复制阶段何时完成，若   
 *		 工作进程在此（非内存）状态下崩溃，复制将不会       
 *		 重新尝试。 
 *	   - 表同步工作进程将表状态设为 SYNCWAIT；等待状态改变。   
 *	   - 应用工作进程周期性检查处于 SYNCWAIT 状态的表。当 
 *		 出现表时，它将表状态设为 CATCHUP 并开始循环等待， 
 *		 直到表状态被设为 SYNCDONE，或者该同步工作进程   
 *		 退出
 *	   - 在同步工作进程看到状态变更为 CATCHUP 之后，它将 
 *		 读取流并应用变更（如同应用工作进程那样），直到
 *		 追上指定的流位置。然后它将状态设为              
 *		 SYNCDONE。在 CATCHUP 与 SYNCDONE 之间应用的            
 *		 变更可能为零，因为同步工作进程可能领先于应用 
 *		 工作进程 
 *	   - 一旦状态设为 SYNCDONE，应用将持续跟踪该表，直到 
 *		 追上 SYNCDONE 流位置，届时它将状态设为             
 *		 READY 并停止跟踪。同样，中间可能有                    
 *		 零个变更。            
 *
 *	  因此状态推进始终是：INIT -> DATASYNC -> FINISHEDCOPY      
 *	  -> SYNCWAIT -> CATCHUP -> SYNCDONE -> READY.
 *
 *	  系统表 pg_subscription_rel 用于保存关于已订阅        
 *	  表及其状态的信息。该系统表保存除 SYNCWAIT 和   
 *	  CATCHUP 之外的所有状态，仅存于共享内存中。 
 *
 *	  示例流程如下所示：  
 *	   - 应用领先时：
 *		  sync:8
 *			-> 系统表设 FINISHEDCOPY  
 *			-> 在内存设 SYNCWAIT 
 *		  apply:10
 *			-> 在内存设 CATCHUP 
 *			-> 入等待循环
 *		  sync:10
 *			-> 系统表设 SYNCDONE  
 *			-> exit
 *		  apply:10
 *			-> 退出循环  
 *			-> 继续复制
 *		  apply:11
 *			-> 系统表设 READY  
 *
 *	   - 同步领先：  
 *		  sync:10
 *			-> 系统表设 FINISHEDCOPY  
 *			-> 在内存设 SYNCWAIT 
 *		  apply:8
 *			-> 在内存设 CATCHUP 
 *			-> 继续按表过滤          
 *		  sync:10
 *			-> 系统表设 SYNCDONE  
 *			-> exit
 *		  apply:10
 *			-> 系统表设 READY  
 *			-> 停止按表过滤      
 *			-> 继续复制
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "replication/logicallauncher.h"
#include "replication/logicalrelation.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "replication/slot.h"
#include "replication/origin.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/usercontext.h"

typedef enum
{
	SYNC_TABLE_STATE_NEEDS_REBUILD,
	SYNC_TABLE_STATE_REBUILD_STARTED,
	SYNC_TABLE_STATE_VALID,
} SyncingTablesState;

static SyncingTablesState table_states_validity = SYNC_TABLE_STATE_NEEDS_REBUILD;
static List *table_states_not_ready = NIL;
static bool FetchTableStates(bool *started_tx);

static StringInfo copybuf = NULL;

/*
 * 同步工作进程的退出例程。    
 */
static void
pg_attribute_noreturn()
finish_sync_worker(void)
{
	/*
	 * 提交任何未完成的事务。这是常见情况，除非      
	 * 对该表无事可做。              
	 */
	if (IsTransactionState())
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	/* 刷写所有写入。 */
	XLogFlush(GetXLogWriteRecPtr());

	StartTransactionCommand();
	ereport(LOG,
			(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has finished",
					MySubscription->name,
					get_rel_name(MyLogicalRepWorker->relid))));
	CommitTransactionCommand();

	/* 向领导者应用工作进程发信号。  */
	logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);

	/* 优雅地停止 */
	proc_exit(0);
}

/*
 * 等待关系的同步状态在系统表中被设置为预期            
 * 值；发生时返回 true。    
 *
 * 如果表同步工作进程或表本身已消失，或表状态
 * 已被重置，则返回 false。              
 *
 * 当前，这用于应用工作进程从 CATCHUP 状态             
 * 转换到 SYNCDONE 时。 
 */
static bool
wait_for_relation_state_change(Oid relid, char expected_state)
{
	char		state;

	for (;;)
	{
		LogicalRepWorker *worker;
		XLogRecPtr	statelsn;

		CHECK_FOR_INTERRUPTS();

		InvalidateCatalogSnapshot();
		state = GetSubscriptionRelState(MyLogicalRepWorker->subid,
										relid, &statelsn);

		if (state == SUBREL_STATE_UNKNOWN)
			break;

		if (state == expected_state)
			return true;

		/* 检查同步工作进程是否仍运行，否则退出。  */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid, relid,
										false);
		LWLockRelease(LogicalRepWorkerLock);
		if (!worker)
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000L, WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE);

		ResetLatch(MyLatch);
	}

	return false;
}

/*
 * 等待应用工作进程将我们同步工作进程的              
 * 状态改为预期值。   
 *
 * 在从 SYNCWAIT 状态转换到 CATCHUP 时使用。   
 *
 * 若应用工作进程已消失则返回 false。  
 */
static bool
wait_for_worker_state_change(char expected_state)
{
	int			rc;

	for (;;)
	{
		LogicalRepWorker *worker;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 如果已处于正确状态则完成。（我们假定此次      
		 * 读取足够原子化，即使无锁也不会误导。）        
		 */
		if (MyLogicalRepWorker->relstate == expected_state)
			return true;

		/*
		 * 应用工作进程死亡则退出，否则告知它我们  
		 * 等待  
		 */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid,
										InvalidOid, false);
		if (worker && worker->proc)
			logicalrep_worker_wakeup_ptr(worker);
		LWLockRelease(LogicalRepWorkerLock);
		if (!worker)
			break;

		/*
		 * 等待。我们预期会从应用工作进程收到锁存器      
		 * 信号，但设置超时以防它死亡而未发送。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   1000L, WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
	}

	return false;
}

/*
 * syscache 失效时的回调。      
 */
void
invalidate_syncing_table_states(Datum arg, int cacheid, uint32 hashvalue)
{
	table_states_validity = SYNC_TABLE_STATE_NEEDS_REBUILD;
}

/*
 * 从同步工作进程侧处理表同步协                       
 * 作。 
 *
 * 如果同步工作进程处于 CATCHUP 状态并且已到达      
 * （或越过）WAL 流中预定的同步点，则把该表标记为     
 * SYNCDONE，完成。
 */
static void
process_syncing_tables_for_sync(XLogRecPtr current_lsn)
{
	SpinLockAcquire(&MyLogicalRepWorker->relmutex);

	if (MyLogicalRepWorker->relstate == SUBREL_STATE_CATCHUP &&
		current_lsn >= MyLogicalRepWorker->relstate_lsn)
	{
		TimeLineID	tli;
		char		syncslotname[NAMEDATALEN] = {0};
		char		originname[NAMEDATALEN] = {0};

		MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCDONE;
		MyLogicalRepWorker->relstate_lsn = current_lsn;

		SpinLockRelease(&MyLogicalRepWorker->relmutex);

		/*
		 * UpdateSubscriptionRelState 必须在事务内调用。         
		 */
		if (!IsTransactionState())
			StartTransactionCommand();

		UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
								   MyLogicalRepWorker->relid,
								   MyLogicalRepWorker->relstate,
								   MyLogicalRepWorker->relstate_lsn);

		/*
		 * 结束流式传输，以便 LogRepWorkerWalRcvConn 能删除    
		 * 该槽。
		 */
		walrcv_endstreaming(LogRepWorkerWalRcvConn, &tli);

		/*
		 * 清理表同步槽。      
		 *
		 * 必须在更新状态之后执行，因为否则如果在执行  
		 * 数据库操作期间发生错误，我们将无法回滚已     
		 * 删除的槽。               
		 */
		ReplicationSlotNameForTablesync(MyLogicalRepWorker->subid,
										MyLogicalRepWorker->relid,
										syncslotname,
										sizeof(syncslotname));

		/*
		 * 如果无法删除槽就必须报错，这一点很重要。       
		 * 否则，在对应的订阅被删除之前它不会被删除。   
		 * 因此传入 missing_ok = false。        
		 */
		ReplicationSlotDropAtPubNode(LogRepWorkerWalRcvConn, syncslotname, false);

		CommitTransactionCommand();
		pgstat_report_stat(false);

		/*
		 * 启动一个新事务来清理表同步的复制源跟踪。      
		 * 该事务将在 finish_sync_worker() 内结束。              
		 * 现在，即使此处移除失败，应用工作进程也会    
		 * 确保事后将其清理。     
		 *
		 * 我们需要在表状态被设为 SYNCDONE 之后处理。  
		 * 否则，若在执行数据库                             
		 * 操作期间发生错误，工作进程会被重启，内存中的
		 * 复制进度（remote_lsn）将不会被回滚，而这本应在  
		 * 重启前被清除。因此重启后的工作进程将使用       
		 * 无效的复制进度状态，导致重放已经         
		 * 被应用过的事务。                    
		 */
		StartTransactionCommand();

		ReplicationOriginNameForLogicalRep(MyLogicalRepWorker->subid,
										   MyLogicalRepWorker->relid,
										   originname,
										   sizeof(originname));

		/*
		 * 重置复制源会话会移除该槽的所属关系。         
		 * 这是允许删除该复制源所必需的。    
		 */
		replorigin_session_reset();
		replorigin_session_origin = InvalidRepOriginId;
		replorigin_session_origin_lsn = InvalidXLogRecPtr;
		replorigin_session_origin_timestamp = 0;

		/*
		 * 存在时删除表同步复制源跟踪。     
		 *
		 * 存在这样的可能：用户正在并发地为订阅执行      
		 * refresh 时我们会移除表状态及其复制源，或者应用  
		 * 工作进程已删除了此复制源。因此传入 missing_ok
		 * = true。         
		 */
		replorigin_drop_by_name(originname, true, false);

		finish_sync_worker();
	}
	else
		SpinLockRelease(&MyLogicalRepWorker->relmutex);
}

/*
 * 从应用工作进程侧处理表同步协作。               
 *
 * 遍历所有由应用进程单独跟踪的订阅表（当前即所有 
 * 状态不是 SUBREL_STATE_READY 的表）并管理      
 * 它们的同步。                                      
 *
 * 如果有需要同步且尚未被同步的表，则为它们启动同步  
 * 工作进程（如果有空闲的同步工作进程槽位）。   
 * 为防止失败后以高频率为同一关系启动同步工作进程， 
 * 我们把其上次启动时间存储在每条同步状态信息中。      
 * 在等待至少 wal_retrieve_retry_interval 之后，我们才为同一  
 * 关系启动同步工作进程。    
 *
 * 对于已在同步中的表，检查同步工作进程               
 * 是需要应用工作进程采取行动，还是已经完成。        
 * SYNCWAIT 到 CATCHUP 转变。 
 *
 * 如果到达了同步位置（SYNCDONE），则该表可以被           
 * 标记为 READY，且不再被跟踪。     
 */
static void
process_syncing_tables_for_apply(XLogRecPtr current_lsn)
{
	struct tablesync_start_time_mapping
	{
		Oid			relid;
		TimestampTz last_start_time;
	};
	static HTAB *last_start_times = NULL;
	ListCell   *lc;
	bool		started_tx = false;
	bool		should_exit = false;
	Relation	rel = NULL;

	Assert(!IsTransactionState());

	/* 此处需要订阅表的最新同步状态信息。              */
	FetchTableStates(&started_tx);

	/*
	 * 准备哈希表来跟踪工作进程的上次启动时间，以避免  
	 * 立即重启。若没有需要同步的表则无需                   
	 * 它。  
	 */
	if (table_states_not_ready != NIL && !last_start_times)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(struct tablesync_start_time_mapping);
		last_start_times = hash_create("Logical replication table sync worker start times",
									   256, &ctl, HASH_ELEM | HASH_BLOBS);
	}

	/*
	 * 处理完所有表后清理该哈希表（只是为了          
	 * 释放一小块内存）。
	 */
	else if (table_states_not_ready == NIL && last_start_times)
	{
		hash_destroy(last_start_times);
		last_start_times = NULL;
	}

	/*
	 * 处理所有正在被同步的表。           
	 */
	foreach(lc, table_states_not_ready)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);

		if (rstate->state == SUBREL_STATE_SYNCDONE)
		{
			/*
			 * 应用已追上表同步完成时所处的位置。         
			 * 将表标记为就绪，应用只需                          
			 * 继续正常复制该表。       
			 */
			if (current_lsn >= rstate->lsn)
			{
				char		originname[NAMEDATALEN];

				rstate->state = SUBREL_STATE_READY;
				rstate->lsn = current_lsn;
				if (!started_tx)
				{
					StartTransactionCommand();
					started_tx = true;
				}

				/*
				 * 存在时删除表同步复制源跟踪。     
				 *
				 * 存在这样的可能：用户正在并发地执行       
				 * refresh，此时会移除表状态及其复制源，  
				 * 或者表同步工作进程已删除此复制源。    
				 * 我们无法指望表同步工作进程去删除       
				 * 复制源跟踪，因为若删除期间发生任何    
				 * 错误，不会重启它来删除该复制源。    
				 * 因此传入 missing_ok = true。    
				 *
				 * 按照 DDL 命令期间采用的相同顺序锁定订阅
				 * 和复制源，以避免死锁。参见              
				 * AlterSubscription_refresh 
				 */
				LockSharedObject(SubscriptionRelationId, MyLogicalRepWorker->subid,
								 0, AccessShareLock);
				if (!rel)
					rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);

				ReplicationOriginNameForLogicalRep(MyLogicalRepWorker->subid,
												   rstate->relid,
												   originname,
												   sizeof(originname));
				replorigin_drop_by_name(originname, true, false);

				/*
				 * 仅在复制源清理后才将状态更新为 READY。  
				 */
				UpdateSubscriptionRelStateEx(MyLogicalRepWorker->subid,
											 rstate->relid, rstate->state,
											 rstate->lsn, true);
			}
		}
		else
		{
			LogicalRepWorker *syncworker;

			/*
			 * 查找此关系的同步工作进程。  
			 */
			LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

			syncworker = logicalrep_worker_find(MyLogicalRepWorker->subid,
												rstate->relid, false);

			if (syncworker)
			{
				/* 找到一个，更新其状态副本。 */
				SpinLockAcquire(&syncworker->relmutex);
				rstate->state = syncworker->relstate;
				rstate->lsn = syncworker->relstate_lsn;
				if (rstate->state == SUBREL_STATE_SYNCWAIT)
				{
					/*
					 * 同步工作进程在等待应用。告知它现在   
					 * 可以追上。 
					 */
					syncworker->relstate = SUBREL_STATE_CATCHUP;
					syncworker->relstate_lsn =
						Max(syncworker->relstate_lsn, current_lsn);
				}
				SpinLockRelease(&syncworker->relmutex);

				/* 若已告知其追上，则等待它。     */
				if (rstate->state == SUBREL_STATE_SYNCWAIT)
				{
					/* 同步工作进程可能在等我们，发信号。  */
					if (syncworker->proc)
						logicalrep_worker_wakeup_ptr(syncworker);

					/* 现在可安全释放 LWLock   */
					LWLockRelease(LogicalRepWorkerLock);

					if (started_tx)
					{
						/*
						 * 必须提交现有事务来释放现有锁，     
						 * 然后再进入忙循环。                    
						 * 这是为了避免任何未被发现的死锁，  
						 * 它由现有锁导致，死锁检测器无法      
						 * 检测到锁存器上的等待。        
						 *
						 * 同时在提交之前关闭任何表。   
						 */
						if (rel)
						{
							table_close(rel, NoLock);
							rel = NULL;
						}
						CommitTransactionCommand();
						pgstat_report_stat(false);
					}

					/*
					 * 进入忙循环，等待同步工作进程达到      
					 * 预期状态（或力尽而亡）。 
					 */
					StartTransactionCommand();
					started_tx = true;

					wait_for_relation_state_change(rstate->relid,
												   SUBREL_STATE_SYNCDONE);
				}
				else
					LWLockRelease(LogicalRepWorkerLock);
			}
			else
			{
				/*
				 * 如果此表尚无同步工作进程，则在持有 
				 * 锁期间统计此订阅正在运行的同步工作进程
				 * 数量。
				 */
				int			nsyncworkers =
					logicalrep_sync_worker_count(MyLogicalRepWorker->subid);

				/* 现在可安全释放 LWLock   */
				LWLockRelease(LogicalRepWorkerLock);

				/*
				 * 若有空闲同步工作进程槽位，为该表启动 
				 * 同步工作进程。
				 */
				if (nsyncworkers < max_sync_workers_per_subscription)
				{
					TimestampTz now = GetCurrentTimestamp();
					struct tablesync_start_time_mapping *hentry;
					bool		found;

					hentry = hash_search(last_start_times, &rstate->relid,
										 HASH_ENTER, &found);

					if (!found ||
						TimestampDifferenceExceeds(hentry->last_start_time, now,
												   wal_retrieve_retry_interval))
					{
						/*
						 * 即使我们未能启动工作进程，也设置
						 * last_start_time，这样重试会等到 
						 * wal_retrieve_retry_interval 经过。   
						 */
						hentry->last_start_time = now;
						(void) logicalrep_worker_launch(MyLogicalRepWorker->dbid,
														MySubscription->oid,
														MySubscription->name,
														MyLogicalRepWorker->userid,
														rstate->relid,
														DSM_HANDLE_INVALID);
					}
				}
			}
		}
	}

	/* 已打开则关闭表 */
	if (rel)
		table_close(rel, NoLock);

	if (started_tx)
	{
		/*
		 * 即使用户请求了 two_phase 模式，它也会保持为      
		 * 'pending'，直到所有表同步都达到 READY 状态。 
		 *
		 * 发生这种情况时，将重启应用工作进程，    
		 * 并且（若条件仍满足）届时 two_phase 三态将变为   
		 * 'enabled'。           
		 *
		 * 注意：如果订阅没有表，则将状态保留为         
		 * PENDING，允许 ALTER SUBSCRIPTION ... REFRESH PUBLICATION 执行 
		 * 。  
		 */
		if (MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING)
		{
			CommandCounterIncrement();	/* 使更新可见      */
			if (AllTablesyncsReady())
			{
				ereport(LOG,
						(errmsg("logical replication apply worker for subscription \"%s\" will restart so that two_phase can be enabled",
								MySubscription->name)));
				should_exit = true;
			}
		}

		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	if (should_exit)
	{
		/*
		 * 重置此工作进程的上次启动时间，让启动器          
		 * 无需等待 wal_retrieve_retry_interval 即可重启它。
		 */
		ApplyLauncherForgetWorkerStartTime(MySubscription->oid);

		proc_exit(0);
	}
}

/*
 * 处理正在被同步的表可能发生的状态变更。              
 */
void
process_syncing_tables(XLogRecPtr current_lsn)
{
	/*
	 * 对于并行应用工作进程则跳过，因为它们只对处于 
	 * READY 状态表操作。参见 pa_can_start() 和
	 * should_apply_changes_for_rel() 
	 */
	if (am_parallel_apply_worker())
		return;

	if (am_tablesync_worker())
		process_syncing_tables_for_sync(current_lsn);
	else
		process_syncing_tables_for_apply(current_lsn);
}

/*
 * 基于逻辑关系映射为 COPY 创建列的列表。            
 */
static List *
make_copy_attnamelist(LogicalRepRelMapEntry *rel)
{
	List	   *attnamelist = NIL;
	int			i;

	for (i = 0; i < rel->remoterel.natts; i++)
	{
		attnamelist = lappend(attnamelist,
							  makeString(rel->remoterel.attnames[i]));
	}


	return attnamelist;
}

/*
 * COPY FROM 的数据源回调，它从远程连接读取数据，   
 * 并把数据传回给我们本地的 COPY。          
 */
static int
copy_read_data(void *outbuf, int minread, int maxread)
{
	int			bytesread = 0;
	int			avail;

	/* 若上次读取残留一些数据，则使用它们。      */
	avail = copybuf->len - copybuf->cursor;
	if (avail)
	{
		if (avail > maxread)
			avail = maxread;
		memcpy(outbuf, &copybuf->data[copybuf->cursor], avail);
		copybuf->cursor += avail;
		maxread -= avail;
		bytesread += avail;
	}

	while (maxread > 0 && bytesread < minread)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			len;
		char	   *buf = NULL;

		for (;;)
		{
			/* 尝试读数据。 */
			len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);

			CHECK_FOR_INTERRUPTS();

			if (len == 0)
				break;
			else if (len < 0)
				return bytesread;
			else
			{
				/* 处理数据     */
				copybuf->data = buf;
				copybuf->len = len;
				copybuf->cursor = 0;

				avail = copybuf->len - copybuf->cursor;
				if (avail > maxread)
					avail = maxread;
				memcpy(outbuf, &copybuf->data[copybuf->cursor], avail);
				outbuf = (void *) ((char *) outbuf + avail);
				copybuf->cursor += avail;
				maxread -= avail;
				bytesread += avail;
			}

			if (maxread <= 0 || bytesread >= minread)
				return bytesread;
		}

		/*
		 * 等更多数据或锁存器 
		 */
		(void) WaitLatchOrSocket(MyLatch,
								 WL_SOCKET_READABLE | WL_LATCH_SET |
								 WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 fd, 1000L, WAIT_EVENT_LOGICAL_SYNC_DATA);

		ResetLatch(MyLatch);
	}

	return bytesread;
}


/*
 * 以类似于复制期间 RELATION 消息所提供的方式获取     
 * 远程关系的信息。此函数还返回要在 COPY 命令中             
 * 使用的关系限定条件。                
 */
static void
fetch_remote_table_info(char *nspname, char *relname,
						LogicalRepRelation *lrel, List **qual)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[] = {OIDOID, CHAROID, CHAROID};
	Oid			attrRow[] = {INT2OID, TEXTOID, OIDOID, BOOLOID};
	Oid			qualRow[] = {TEXTOID};
	bool		isnull;
	int			natt;
	ListCell   *lc;
	Bitmapset  *included_cols = NULL;

	lrel->nspname = nspname;
	lrel->relname = relname;

	/* 首先获取 Oid 和复制标识。   */
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "SELECT c.oid, c.relreplident, c.relkind"
					 "  FROM pg_catalog.pg_class c"
					 "  INNER JOIN pg_catalog.pg_namespace n"
					 "        ON (c.relnamespace = n.oid)"
					 " WHERE n.nspname = %s"
					 "   AND c.relname = %s",
					 quote_literal_cstr(nspname),
					 quote_literal_cstr(relname));
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
					  lengthof(tableRow), tableRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("table \"%s.%s\" not found on publisher",
						nspname, relname)));

	lrel->remoteid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));
	Assert(!isnull);
	lrel->replident = DatumGetChar(slot_getattr(slot, 2, &isnull));
	Assert(!isnull);
	lrel->relkind = DatumGetChar(slot_getattr(slot, 3, &isnull));
	Assert(!isnull);

	ExecDropSingleTupleTableSlot(slot);
	walrcv_clear_result(res);


	/*
	 * 获取每个关系的列列表。  
	 *
	 * 我们需要在获取列名和类型的信息之前做这件事，   
	 * 以便跳过不应被复制的列。                      
	 */
	if (walrcv_server_version(LogRepWorkerWalRcvConn) >= 150000)
	{
		WalRcvExecResult *pubres;
		TupleTableSlot *tslot;
		Oid			attrsRow[] = {INT2VECTOROID};
		StringInfoData pub_names;

		initStringInfo(&pub_names);
		foreach(lc, MySubscription->publications)
		{
			if (foreach_current_index(lc) > 0)
				appendStringInfoString(&pub_names, ", ");
			appendStringInfoString(&pub_names, quote_literal_cstr(strVal(lfirst(lc))));
		}

		/*
		 * 获取该关系的列列表信息（来自所有            
		 * 发布）。  
		 */
		resetStringInfo(&cmd);
		appendStringInfo(&cmd,
						 "SELECT DISTINCT"
						 "  (CASE WHEN (array_length(gpt.attrs, 1) = c.relnatts)"
						 "   THEN NULL ELSE gpt.attrs END)"
						 "  FROM pg_publication p,"
						 "  LATERAL pg_get_publication_tables(p.pubname) gpt,"
						 "  pg_class c"
						 " WHERE gpt.relid = %u AND c.oid = gpt.relid"
						 "   AND p.pubname IN ( %s )",
						 lrel->remoteid,
						 pub_names.data);

		pubres = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
							 lengthof(attrsRow), attrsRow);

		if (pubres->status != WALRCV_OK_TUPLES)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not fetch column list info for table \"%s.%s\" from publisher: %s",
							nspname, relname, pubres->err)));

		/*
		 * 我们不支持在合并多个发布时同一张表的列列表 
		 * 不相同的情况。参见 fetch_table_list 顶部的       
		 * 注释。因此应该只返回一行。                    
		 * 尽管在创建订阅时我们已经检查过这一点，但       
		 * 仍需在此处检查，以防列列表在创建订阅之后、   
		 * 同步工作进程启动之前发生变化。                   
		 */
		if (tuplestore_tuple_count(pubres->tuplestore) > 1)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   nspname, relname));

		/*
		 * 获取列列表并用 attnum 构建一个位图。             
		 *
		 * 若发现 NULL 值，则意味着所有列都应被        
		 * 复制。  
		 */
		tslot = MakeSingleTupleTableSlot(pubres->tupledesc, &TTSOpsMinimalTuple);
		if (tuplestore_gettupleslot(pubres->tuplestore, true, false, tslot))
		{
			Datum		cfval = slot_getattr(tslot, 1, &isnull);

			if (!isnull)
			{
				ArrayType  *arr;
				int			nelems;
				int16	   *elems;

				arr = DatumGetArrayTypeP(cfval);
				nelems = ARR_DIMS(arr)[0];
				elems = (int16 *) ARR_DATA_PTR(arr);

				for (natt = 0; natt < nelems; natt++)
					included_cols = bms_add_member(included_cols, elems[natt]);
			}

			ExecClearTuple(tslot);
		}
		ExecDropSingleTupleTableSlot(tslot);

		walrcv_clear_result(pubres);

		pfree(pub_names.data);
	}

	/*
	 * 现在获取列名和类型。   
	 */
	resetStringInfo(&cmd);
	appendStringInfo(&cmd,
					 "SELECT a.attnum,"
					 "       a.attname,"
					 "       a.atttypid,"
					 "       a.attnum = ANY(i.indkey)"
					 "  FROM pg_catalog.pg_attribute a"
					 "  LEFT JOIN pg_catalog.pg_index i"
					 "       ON (i.indexrelid = pg_get_replica_identity_index(%u))"
					 " WHERE a.attnum > 0::pg_catalog.int2"
					 "   AND NOT a.attisdropped %s"
					 "   AND a.attrelid = %u"
					 " ORDER BY a.attnum",
					 lrel->remoteid,
					 (walrcv_server_version(LogRepWorkerWalRcvConn) >= 120000 ?
					  "AND a.attgenerated = ''" : ""),
					 lrel->remoteid);
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
					  lengthof(attrRow), attrRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	/* 不知道会有多少行，所以要分配足够的空间。       */
	lrel->attnames = palloc0(MaxTupleAttributeNumber * sizeof(char *));
	lrel->atttyps = palloc0(MaxTupleAttributeNumber * sizeof(Oid));
	lrel->attkeys = NULL;

	/*
	 * 将这些列存储为名称列表。如果存在列列表，    
	 * 则忽略不在该列表中的列。        
	 */
	natt = 0;
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *rel_colname;
		AttrNumber	attnum;

		attnum = DatumGetInt16(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);

		/* 如果该列不在列列表中，则跳过它。  */
		if (included_cols != NULL && !bms_is_member(attnum, included_cols))
		{
			ExecClearTuple(slot);
			continue;
		}

		rel_colname = TextDatumGetCString(slot_getattr(slot, 2, &isnull));
		Assert(!isnull);

		lrel->attnames[natt] = rel_colname;
		lrel->atttyps[natt] = DatumGetObjectId(slot_getattr(slot, 3, &isnull));
		Assert(!isnull);

		if (DatumGetBool(slot_getattr(slot, 4, &isnull)))
			lrel->attkeys = bms_add_member(lrel->attkeys, natt);

		/* 不应发生。      */
		if (++natt >= MaxTupleAttributeNumber)
			elog(ERROR, "too many columns in remote table \"%s.%s\"",
				 nspname, relname);

		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	lrel->natts = natt;

	walrcv_clear_result(res);

	/*
	 * 获取关系的行过滤表达式。DISTINCT 可避免         
	 * 同一张表在多个发布中的同一表达式被多次         
	 * 包含进最终表达式中。         
	 *
	 * 即使某行只匹配其中一个发布，也需要复制 
	 * 该行，因此稍后将所有条件用 OR 组合。    
	 *
	 * 对于初始表同步，在以下情况下可以忽略行过滤       
	 * ：   
	 *
	 * 1) 该表的某个订阅发布未指定任何行过滤              
	 * 条件。     
	 *
	 * 2) 该表的某个订阅发布将 puballtables 设为 true        
	 *
	 * 3) 该表的某个订阅发布被声明为 TABLES IN SCHEMA          
	 * （涵盖此关系）。   
	 */
	if (walrcv_server_version(LogRepWorkerWalRcvConn) >= 150000)
	{
		StringInfoData pub_names;

		/* 构建发布名列表。*/
		initStringInfo(&pub_names);
		foreach(lc, MySubscription->publications)
		{
			char	   *pubname = strVal(lfirst(lc));

			if (foreach_current_index(lc) > 0)
				appendStringInfoString(&pub_names, ", ");

			appendStringInfoString(&pub_names, quote_literal_cstr(pubname));
		}

		/* 检查行过滤。     */
		resetStringInfo(&cmd);
		appendStringInfo(&cmd,
						 "SELECT DISTINCT pg_get_expr(gpt.qual, gpt.relid)"
						 "  FROM pg_publication p,"
						 "  LATERAL pg_get_publication_tables(p.pubname) gpt"
						 " WHERE gpt.relid = %u"
						 "   AND p.pubname IN ( %s )",
						 lrel->remoteid,
						 pub_names.data);

		res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data, 1, qualRow);

		if (res->status != WALRCV_OK_TUPLES)
			ereport(ERROR,
					(errmsg("could not fetch table WHERE clause info for table \"%s.%s\" from publisher: %s",
							nspname, relname, res->err)));

		/*
		 * 同一张表的多个行过滤表达式将由 COPY 用             
		 * OR 组合。如果此表的任何过滤表达式为              
		 * NULL，则意味着整张表都会被复制。此时              
		 * 根本无需构建统一的行过滤表达式                   
		 * 。 
		 */
		slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
		while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		{
			Datum		rf = slot_getattr(slot, 1, &isnull);

			if (!isnull)
				*qual = lappend(*qual, makeString(TextDatumGetCString(rf)));
			else
			{
				/* 按需忽略过滤器并进行清理。  */
				if (*qual)
				{
					list_free_deep(*qual);
					*qual = NIL;
				}
				break;
			}

			ExecClearTuple(slot);
		}
		ExecDropSingleTupleTableSlot(slot);

		walrcv_clear_result(res);
	}

	pfree(cmd.data);
}

/*
 * 从发布者复制表的现有数据。      
 *
 * 调用者负责锁定本地关系。                 
 */
static void
copy_table(Relation rel)
{
	LogicalRepRelMapEntry *relmapentry;
	LogicalRepRelation lrel;
	List	   *qual = NIL;
	WalRcvExecResult *res;
	StringInfoData cmd;
	CopyFromState cstate;
	List	   *attnamelist;
	ParseState *pstate;
	List	   *options = NIL;

	/* 获取发布者关系信息。   */
	fetch_remote_table_info(get_namespace_name(RelationGetNamespace(rel)),
							RelationGetRelationName(rel), &lrel, &qual);

	/* 把该关系放入 relmap。  */
	logicalrep_relmap_update(&lrel);

	/* 映射发布者关系到本地关系。  */
	relmapentry = logicalrep_rel_open(lrel.remoteid, NoLock);
	Assert(rel == relmapentry->localrel);

	/* 在发布者开始复制。  */
	initStringInfo(&cmd);

	/* 无行过滤的常规表         */
	if (lrel.relkind == RELKIND_RELATION && qual == NIL)
	{
		appendStringInfo(&cmd, "COPY %s",
						 quote_qualified_identifier(lrel.nspname, lrel.relname));

		/* 如果表有列，则指定这些列               */
		if (lrel.natts)
		{
			appendStringInfoString(&cmd, " (");

			/*
			 * XXX 是否无论如何都需要列出各列？也许我们  
			 * 正在复制所有列？
			 */
			for (int i = 0; i < lrel.natts; i++)
			{
				if (i > 0)
					appendStringInfoString(&cmd, ", ");

				appendStringInfoString(&cmd, quote_identifier(lrel.attnames[i]));
			}

			appendStringInfoString(&cmd, ")");
		}

		appendStringInfoString(&cmd, " TO STDOUT");
	}
	else
	{
		/*
		 * 对于非表和带行过滤的表，我们需要执行 COPY   
		 * (SELECT ...)，但无法只执行 SELECT *，因为需要         
		 * 不复制生成列。对于带任何行过滤的表，为 COPY  
		 * 构建 OR 组合行过滤的 SELECT 查询。
		 */
		appendStringInfoString(&cmd, "COPY (SELECT ");
		for (int i = 0; i < lrel.natts; i++)
		{
			appendStringInfoString(&cmd, quote_identifier(lrel.attnames[i]));
			if (i < lrel.natts - 1)
				appendStringInfoString(&cmd, ", ");
		}

		appendStringInfoString(&cmd, " FROM ");

		/*
		 * 对于常规表，确保我们不复制那些继承此命名表的
		 * 子表的数据，因为它们会被单独复制。         
		 */
		if (lrel.relkind == RELKIND_RELATION)
			appendStringInfoString(&cmd, "ONLY ");

		appendStringInfoString(&cmd, quote_qualified_identifier(lrel.nspname, lrel.relname));
		/* OR 组合过滤列表 */
		if (qual != NIL)
		{
			ListCell   *lc;
			char	   *q = strVal(linitial(qual));

			appendStringInfo(&cmd, " WHERE %s", q);
			for_each_from(lc, qual, 1)
			{
				q = strVal(lfirst(lc));
				appendStringInfo(&cmd, " OR %s", q);
			}
			list_free_deep(qual);
		}

		appendStringInfoString(&cmd, ") TO STDOUT");
	}

	/*
	 * 在 v16 之前，即使订阅启用 binary 选项，初始表同步  
	 * 也会使用文本格式。                        
	 */
	if (walrcv_server_version(LogRepWorkerWalRcvConn) >= 160000 &&
		MySubscription->binary)
	{
		appendStringInfoString(&cmd, " WITH (FORMAT binary)");
		options = list_make1(makeDefElem("format",
										 (Node *) makeString("binary"), -1));
	}

	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data, 0, NULL);
	pfree(cmd.data);
	if (res->status != WALRCV_OK_COPY_OUT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not start initial contents copy for table \"%s.%s\": %s",
						lrel.nspname, lrel.relname, res->err)));
	walrcv_clear_result(res);

	copybuf = makeStringInfo();

	pstate = make_parsestate(NULL);
	(void) addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										 NULL, false, false);

	attnamelist = make_copy_attnamelist(relmapentry);
	cstate = BeginCopyFrom(pstate, rel, NULL, NULL, false, copy_read_data, attnamelist, options);

	/* 执行复制*/
	(void) CopyFrom(cstate);

	logicalrep_rel_close(relmapentry, NoLock);
}

/*
 * 确定表同步槽的名称。    
 *
 * 由于远程节点对槽名称长度的限制，名称必须保持         
 * 在 NAMEDATALEN - 1 以内。追加 system_identifier 以避免    
 * slot_name 与其他集簇中订阅的冲突。按照当前方案       
 * pg_%u_sync_%u_UINT64_FORMAT (3 + 10 + 6 + 10 + 20 + '\0')，          
 * slot_name 最长将为 50。   
 *
 * 返回的槽名称以给定大小存储在缓冲区（syncslotname）      
 * 中。         
 *
 * 注意：我们不把订阅槽名称作为表同步槽名的组成部分， 
 * 因为我们要负责清理这些槽，而一旦订阅槽名称           
 * 发生变更，就可能无法重新算出要清理的                      
 * 名称。   
 */
void
ReplicationSlotNameForTablesync(Oid suboid, Oid relid,
								char *syncslotname, Size szslot)
{
	snprintf(syncslotname, szslot, "pg_%u_sync_%u_" UINT64_FORMAT, suboid,
			 relid, GetSystemIdentifier());
}

/*
 * 在同步工作进程中开始同步表。 
 *
 * 如果同步该表不需要做任何事，我们将退出工作进程， 
 * 无其他动作。 
 *
 * 返回的槽名称在当前内存上下文中 palloc。       
 */
char *
LogicalRepSyncTableStart(XLogRecPtr *origin_startpos)
{
	char	   *slotname;
	char	   *err;
	char		relstate;
	XLogRecPtr	relstate_lsn;
	Relation	rel;
	AclResult	aclresult;
	WalRcvExecResult *res;
	char		originname[NAMEDATALEN];
	RepOriginId originid;
	UserContext ucxt;
	bool		must_use_password;
	bool		run_as_owner;

	/* 检查表同步的状态。                   */
	StartTransactionCommand();
	relstate = GetSubscriptionRelState(MyLogicalRepWorker->subid,
									   MyLogicalRepWorker->relid,
									   &relstate_lsn);

	/* 是否必须使用密码？         */
	must_use_password = MySubscription->passwordrequired &&
		!superuser_arg(MySubscription->owner);

	/* 注意 superuser_arg 调用可以访问 DB         */
	CommitTransactionCommand();

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = relstate;
	MyLogicalRepWorker->relstate_lsn = relstate_lsn;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * 如果同步已经完成或不再需要，既然已更新了共享 
	 * 内存状态，现在就退出。      
	 */
	switch (relstate)
	{
		case SUBREL_STATE_SYNCDONE:
		case SUBREL_STATE_READY:
		case SUBREL_STATE_UNKNOWN:
			finish_sync_worker();	/* 不会返回   */
	}

	/* 计算表同步槽的名称。            */
	slotname = (char *) palloc(NAMEDATALEN);
	ReplicationSlotNameForTablesync(MySubscription->oid,
									MyLogicalRepWorker->relid,
									slotname,
									NAMEDATALEN);

	/*
	 * 此处我们使用槽名称而非订阅名称来作为           
	 * application_name，使它不同于领导者应用工作进程，       
	 * 以便同步复制能区分它们。                 
	 */
	LogRepWorkerWalRcvConn =
		walrcv_connect(MySubscription->conninfo, true,
					   must_use_password,
					   slotname, &err);
	if (LogRepWorkerWalRcvConn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to the publisher: %s", err)));

	Assert(MyLogicalRepWorker->relstate == SUBREL_STATE_INIT ||
		   MyLogicalRepWorker->relstate == SUBREL_STATE_DATASYNC ||
		   MyLogicalRepWorker->relstate == SUBREL_STATE_FINISHEDCOPY);

	/* 分配复制源跟踪记录的名称。 */
	ReplicationOriginNameForLogicalRep(MySubscription->oid,
									   MyLogicalRepWorker->relid,
									   originname,
									   sizeof(originname));

	if (MyLogicalRepWorker->relstate == SUBREL_STATE_DATASYNC)
	{
		/*
		 * 我们此前在完成复制之前已因出错而退出，因此
		 * 复制槽可能存在。我们要在该槽已存在时将其  
		 * 移除并继续。         
		 *
		 * XXX 我们也可以改为尝试删除该槽，上次我们失败，
		 * 为此我们可能需要清理复制状态，因为它可能正处
		 * 于获取行的中途。另外，若发生网络中断，它就   
		 * 不会成功，因此下次尝试                               
		 * 是更好的选择。   
		 */
		ReplicationSlotDropAtPubNode(LogRepWorkerWalRcvConn, slotname, true);
	}
	else if (MyLogicalRepWorker->relstate == SUBREL_STATE_FINISHEDCOPY)
	{
		/*
		 * COPY 阶段之前已经完成，但表同步随后崩溃了，
		 * 未能正常完成。                 
		 */
		StartTransactionCommand();

		/*
		 * 复制源跟踪名称必须已经存在。它是在首次        
		 * 启动此表同步时创建的。
		 */
		originid = replorigin_by_name(originname, false);
		replorigin_session_setup(originid, 0);
		replorigin_session_origin = originid;
		*origin_startpos = replorigin_session_get_progress(false);

		CommitTransactionCommand();

		goto copy_table_done;
	}

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = SUBREL_STATE_DATASYNC;
	MyLogicalRepWorker->relstate_lsn = InvalidXLogRecPtr;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/* 更新状态并使其对其他进程可见。   */
	StartTransactionCommand();
	UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
							   MyLogicalRepWorker->relid,
							   MyLogicalRepWorker->relstate,
							   MyLogicalRepWorker->relstate_lsn);
	CommitTransactionCommand();
	pgstat_report_stat(true);

	StartTransactionCommand();

	/*
	 * 此处使用标准的写锁。也许最好禁止在表正            
	 * 被同步期间访问该表。但我们不想阻止主应用          
	 * 进程工作，而且它在把远程关系 id 重映射为本地      
	 * 关系时必须以 RowExclusiveLock 打开它。                
	 */
	rel = table_open(MyLogicalRepWorker->relid, RowExclusiveLock);

	/*
	 * 在远程节点上以 REPEATABLE READ 模式启动一个事务。    
	 * 这确保我们创建的复制槽（见下文）与 COPY            
	 * 彼此一致。                     
	 */
	res = walrcv_exec(LogRepWorkerWalRcvConn,
					  "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ",
					  0, NULL);
	if (res->status != WALRCV_OK_COMMAND)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table copy could not start transaction on publisher: %s",
						res->err)));
	walrcv_clear_result(res);

	/*
	 * 创建一个新的永久逻辑解码槽。该槽将在 COPY         
	 * 完成之后的追赶阶段中使用，因此告诉它使用   
	 * 快照以使最终数据一致。          
	 */
	walrcv_create_slot(LogRepWorkerWalRcvConn,
					   slotname, false /* 永久    */ , false /* two_phase */ ,
					   CRS_USE_SNAPSHOT, origin_startpos);

	/*
	 * 设置复制源跟踪。在复制之前做此事的目的是           
	 * 避免因设置复制源跟踪时的任何错误而再次           
	 * 进行复制。 
	 */
	originid = replorigin_by_name(originname, true);
	if (!OidIsValid(originid))
	{
		/*
		 * 复制源跟踪尚不存在，现在创建它。 
		 *
		 * 然后前进到从 walrcv_create_slot 得到的 LSN。这是    
		 * 为了恢复目的而记录的 WAL 日志。加锁是为了 
		 * 防止复制源在前进期间消失。           
		 */
		originid = replorigin_create(originname);

		LockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);
		replorigin_advance(originid, *origin_startpos, InvalidXLogRecPtr,
						   true /* 后退      */ , true /* WAL log */ );
		UnlockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);

		replorigin_session_setup(originid, 0);
		replorigin_session_origin = originid;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("replication origin \"%s\" already exists",
						originname)));
	}

	/*
	 * 确保 COPY 命令以表所有者身份运行，除非用户       
	 * 已选择退出该行为。          
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->rd_rel->relowner, &ucxt);

	/*
	 * 检查我们的表同步工作进程是否有权限向目标      
	 * 表插入。 
	 */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_INSERT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	/*
	 * COPY FROM 不遵循 RLS 策略。对于拥有                    
	 * BYPASSRLS 权限的角色（或超级用户，其隐式拥有       
	 * 该权限）来说这不是问题，但其他角色应无法 
	 * 绕过 RLS。禁止此类角色对启用 RLS 的关系进行  
	 * 逻辑复制。          
	 */
	if (check_enable_rls(RelationGetRelid(rel), InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user \"%s\" cannot replicate into relation with row-level security enabled: \"%s\"",
						GetUserNameFromId(GetUserId(), true),
						RelationGetRelationName(rel))));

	/* 现在复制初始数据     */
	PushActiveSnapshot(GetTransactionSnapshot());
	copy_table(rel);
	PopActiveSnapshot();

	res = walrcv_exec(LogRepWorkerWalRcvConn, "COMMIT", 0, NULL);
	if (res->status != WALRCV_OK_COMMAND)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table copy could not finish transaction on publisher: %s",
						res->err)));
	walrcv_clear_result(res);

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	table_close(rel, NoLock);

	/* 使复制可见。     */
	CommandCounterIncrement();

	/*
	 * 更新持久化状态以指示 COPY 阶段已完成；使其          
	 * 对外可见。   
	 */
	UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
							   MyLogicalRepWorker->relid,
							   SUBREL_STATE_FINISHEDCOPY,
							   MyLogicalRepWorker->relstate_lsn);

	CommitTransactionCommand();

copy_table_done:

	elog(DEBUG1,
		 "LogicalRepSyncTableStart: '%s' origin_startpos lsn %X/%X",
		 originname, LSN_FORMAT_ARGS(*origin_startpos));

	/*
	 * 我们已完成初始数据同步，更新该状态。              
	 */
	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCWAIT;
	MyLogicalRepWorker->relstate_lsn = *origin_startpos;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * 最后，等待领导者应用工作进程通知我们追赶，     
	 * 然后返回让 LogicalRepApplyLoop 处理。
	 */
	wait_for_worker_state_change(SUBREL_STATE_CATCHUP);
	return slotname;
}

/*
 * 将最新同步状态信息提取到静态列表中的公共代码。     
 *
 * 若订阅至少有 1 张表则返回 true，否则 false。   
 *
 * 注意：如果此函数启动了事务（由参数指示），那么      
 * 提交它是调用者的责任。                   
 */
static bool
FetchTableStates(bool *started_tx)
{
	static bool has_subrels = false;

	*started_tx = false;

	if (table_states_validity != SYNC_TABLE_STATE_VALID)
	{
		MemoryContext oldctx;
		List	   *rstates;
		ListCell   *lc;
		SubscriptionRelState *rstate;

		table_states_validity = SYNC_TABLE_STATE_REBUILD_STARTED;

		/* 清理旧列表。   */
		list_free_deep(table_states_not_ready);
		table_states_not_ready = NIL;

		if (!IsTransactionState())
		{
			StartTransactionCommand();
			*started_tx = true;
		}

		/* 获取全部未就绪表。 */
		rstates = GetSubscriptionRelations(MySubscription->oid, true);

		/* 在永久内存上下文中分配跟踪信息。          */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		foreach(lc, rstates)
		{
			rstate = palloc(sizeof(SubscriptionRelState));
			memcpy(rstate, lfirst(lc), sizeof(SubscriptionRelState));
			table_states_not_ready = lappend(table_states_not_ready, rstate);
		}
		MemoryContextSwitchTo(oldctx);

		/*
		 * 订阅有表吗？                
		 *
		 * 如果发现了非 READY 的关系，我们就知道它有表。 
		 * 但如果 table_states_not_ready 为空，我们仍需再次       
		 * 检查是否有0张表。 
		 */
		has_subrels = (table_states_not_ready != NIL) ||
			HasSubscriptionRelations(MySubscription->oid);

		/*
		 * 如果在进入此例程之后订阅关系缓存已被失效， 
		 * 我们仍使用并返回刚刚构建完成的关系，以避免    
		 * 无限循环，但我们让表状态保持标记为过时，    
		 * 以便下次访问时重新构建它。否则，我们将表状态 
		 * 标记为有效。                                   
		 */
		if (table_states_validity == SYNC_TABLE_STATE_REBUILD_STARTED)
			table_states_validity = SYNC_TABLE_STATE_VALID;
	}

	return has_subrels;
}

/*
 * 如果订阅没有表则返回 false。             
 *
 * 否则，各表同步是否 READY？
 *
 * 注意：此函数不适合在应用或表同步工作进程之外调用 
 * 因为 MySubscription 需要已经被初始化。                        
 */
bool
AllTablesyncsReady(void)
{
	bool		started_tx = false;
	bool		has_subrels = false;

	/* 此处需要订阅表的最新同步状态信息。              */
	has_subrels = FetchTableStates(&started_tx);

	if (started_tx)
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	/*
	 * 当订阅中没有表或并非所有表都处于就绪状态时返回  
	 * false；否则返回 true。       
	 */
	return has_subrels && (table_states_not_ready == NIL);
}

/*
 * 更新 pg_subscription 中指定订阅的 two_phase 状态。               
 */
void
UpdateTwoPhaseState(Oid suboid, char new_state)
{
	Relation	rel;
	HeapTuple	tup;
	bool		nulls[Natts_pg_subscription];
	bool		replaces[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];

	Assert(new_state == LOGICALREP_TWOPHASE_STATE_DISABLED ||
		   new_state == LOGICALREP_TWOPHASE_STATE_PENDING ||
		   new_state == LOGICALREP_TWOPHASE_STATE_ENABLED);

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(suboid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR,
			 "cache lookup failed for subscription oid %u",
			 suboid);

	/* 构造新元组。*/
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	/* 并更新 two_phase 状态     */
	values[Anum_pg_subscription_subtwophasestate - 1] = CharGetDatum(new_state);
	replaces[Anum_pg_subscription_subtwophasestate - 1] = true;

	tup = heap_modify_tuple(tup, RelationGetDescr(rel),
							values, nulls, replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}
