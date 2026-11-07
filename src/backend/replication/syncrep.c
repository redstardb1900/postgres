/*-------------------------------------------------------------------------
 *
 * syncrep.c
 *
 * 同步复制自 PostgreSQL 9.1 起新增。         
 *
 * 若有请求，事务提交会一直等待，直到其提交 LSN 
 * 得到同步备库的确认。           
 *
 * 本模块包含后端等待与释放的相关代码。            
 * 本模块的所有代码都在主库上执行。核心的流复制 
 * 传输仍保留在 WALreceiver/WALsender 模块之内。           
 *
 * 该设计的精髓在于把所有关于                       
 * 等待/释放的逻辑隔离到主库上。主库定义它希望      
 * 等待哪些备库。备库完全无法感知主库上事务的  
 * 持久性要求，这降低了代码的复杂度，                 
 * 简化了备库操作，并节省了网络带宽，                
 * 因为不再需要传送                                 
 * 每事务的状态信息。       
 *
 * 复制要么是同步的，要么是非同步的（异步）。若为 
 * 异步的话，我们直接快速离开。若为同步，           
 * 则要先等待备库上的写、刷写或应用位置，随后才
 * 释放等待中的后端。这种交互更多的复杂度     
 * 预计在后续版本中。
 *
 * 管理等待后端的最佳性能方式是维护                   
 * 一条有序的等待后端队列，这样每次收到应答  
 * 就无需遍历所有等待者。                              
 *
 * 在 9.5 或更早版本中，只有一个备库可被视为  
 * 同步备库。9.6 支持基于优先级的多个同步             
 * 备库。10.0 还支持基于法定人数的多个同步              
 * 备库。事务必须等待其应答的同步备库            
 * 数量由 synchronous_standby_names 指定。                        
 * 该参数还指定了备库名称的列表，以及                 
 * 从所列备库中选择同步备库的方法（FIRST 和 ANY）。 
 *
 * 方法 FIRST 指定基于优先级的同步复制，               
 * 并使事务提交等待，直到其 WAL 记录被复制到   
 * 按优先级选出、数量符合要求的同步备库为止。        
 * 名称在列表中较早出现的备库                                 
 * 被赋予更高的优先级，将被视为同步备库。       
 * 列表中较后的其他备库服务器代表潜在的                
 * 同步备库。若当前的任一同步备库                   
 * 因任何原因断开连接，将立即由下一                     
 * 最高优先级的备库接替。 
 *
 * 方法 ANY 指定基于法定人数的同步复制，          
 * 并使事务提交等待，直到其 WAL 记录被复制到   
 * 列表中至少达到所需数量的同步备库                   
 * 为止。列表中出现的所有备库都被视为                  
 * 法定人数同步备库的候选者。    
 *
 * 若 FIRST 与 ANY 均未指定，则使用 FIRST 作为方法。   
 * 这是为了向后兼容 9.6 或更早版本——当时仅支持 
 * 基于优先级的同步复制。             
 *
 * 从 synchronous_standby_names 中选出的备库在          
 * 能成为同步备库之前，必须已经追上             
 * 主库；这可能需要些时间。一旦追上，  
 * 彼时被视为同步备库的那些备库                     
 * 就会从队列中释放等待者。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_hooks.h"
#include "utils/ps_status.h"

/* 同步复制的用户可设置参数  */
char	   *SyncRepStandbyNames;

#define SyncStandbysDefined() \
	(SyncRepStandbyNames != NULL && SyncRepStandbyNames[0] != '\0')

static bool announce_next_takeover = true;

SyncRepConfigData *SyncRepConfig = NULL;
static int	SyncRepWaitMode = SYNC_REP_NO_WAIT;

static void SyncRepQueueInsert(int mode);
static void SyncRepCancelWait(void);
static int	SyncRepWakeQueue(bool all, int mode);

static bool SyncRepGetSyncRecPtr(XLogRecPtr *writePtr,
								 XLogRecPtr *flushPtr,
								 XLogRecPtr *applyPtr,
								 bool *am_sync);
static void SyncRepGetOldestSyncRecPtr(XLogRecPtr *writePtr,
									   XLogRecPtr *flushPtr,
									   XLogRecPtr *applyPtr,
									   SyncRepStandbyData *sync_standbys,
									   int num_standbys);
static void SyncRepGetNthLatestSyncRecPtr(XLogRecPtr *writePtr,
										  XLogRecPtr *flushPtr,
										  XLogRecPtr *applyPtr,
										  SyncRepStandbyData *sync_standbys,
										  int num_standbys,
										  uint8 nth);
static int	SyncRepGetStandbyPriority(void);
static int	standby_priority_comparator(const void *a, const void *b);
static int	cmp_lsn(const void *a, const void *b);

#ifdef USE_ASSERT_CHECKING
static bool SyncRepQueueIsOrderedByLSN(int mode);
#endif

/*
 * ===========================================================
 * 面向普通用户后端的同步复制函数             
 * ===========================================================
 */

/*
 * 若用户有请求，则等待同步复制。          
 *
 * 最初后端处于 SYNC_REP_NOT_WAITING 状态，然后        
 * 将自己加入等待队列之前，先把该状态改为    
 * SYNC_REP_WAITING。在 SyncRepWakeQueue() 期间，WALSender    
 * 在复制得到确认时把状态改为 SYNC_REP_WAIT_COMPLETE。 
 * 该后端随后将状态重置为 SYNC_REP_NOT_WAITING。  
 *
 * 'lsn' 表示要等待的 LSN。'commit' 表明该 LSN                     
 * 是否代表一条提交记录。若不是，则当 synchronous_commit   
 * 被设为更高级别的 remote_apply 时，我们只等待 WAL  
 * 被刷写，因为只有提交记录才会提供应用反馈。  
 */
void
SyncRepWaitForLSN(XLogRecPtr lsn, bool commit)
{
	int			mode;

	/*
	 * 在事务提交期间持有中断时应调用本函数，          
	 * 以防后续的共享内存队列清理                           
	 * 受到外部中断的影响。       
	 */
	Assert(InterruptHoldoffCount > 0);

	/*
	 * 若用户未请求同步复制，或未定义任何同步复制      
	 * 备库名称，则快速退出。      
	 *
	 * 由于本例程在每次提交时都会被调用，因此在未    
	 * 请求同步复制时快速退出很重要。     
	 *
	 * 我们在不加锁的情况下检查 WalSndCtl->sync_standbys_status   
	 * 标志后立即退出。若 SYNC_STANDBY_INIT 被设置（   
	 * 检查点进程已初始化它），但缺少 SYNC_STANDBY_DEFINED 
	 * （未请求）。     
	 *
	 * 若 SYNC_STANDBY_DEFINED 已设置，稍后还需要在持锁          
	 * 的情况下再次检查状态，以原子方式检查标志并   
	 * 操作同步复制队列。这样做是为了避免             
	 * SyncRepUpdateSyncStandbysDefined() 中描述的竞态条件。         
	 * 另一方面，若 SYNC_STANDBY_DEFINED 未设置，我们           
	 * 则无需加锁。    
	 */
	if (!SyncRepRequested() ||
		((((volatile WalSndCtlData *) WalSndCtl)->sync_standbys_status) &
		 (SYNC_STANDBY_INIT | SYNC_STANDBY_DEFINED)) == SYNC_STANDBY_INIT)
		return;

	/* 提交以外的情况一律将级别封顶为仅远程刷写。    */
	if (commit)
		mode = SyncRepWaitMode;
	else
		mode = Min(SyncRepWaitMode, SYNC_REP_WAIT_FLUSH);

	Assert(dlist_node_is_detached(&MyProc->syncRepLinks));
	Assert(WalSndCtl != NULL);

	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
	Assert(MyProc->syncRepState == SYNC_REP_NOT_WAITING);

	/*
	 * SYNC_STANDBY_DEFINED 未设置时不等待同步复制，参见    
	 * SyncRepUpdateSyncStandbysDefined() 
	 *
	 * 另外检查备库是否已应答。虽是不太可能的竞态  
	 * 条件，但反正都要获取那个缓存行，所以这种检查   
	 * 多半代价很低。  
	 *
	 * 若同步备库数据尚未被初始化              
	 * （SYNC_STANDBY_INIT 未设置），则退回基于 LSN 的检查，  
	 * 再直接检查 GUC。     
	 */
	if (WalSndCtl->sync_standbys_status & SYNC_STANDBY_INIT)
	{
		if ((WalSndCtl->sync_standbys_status & SYNC_STANDBY_DEFINED) == 0 ||
			lsn <= WalSndCtl->lsn[mode])
		{
			LWLockRelease(SyncRepLock);
			return;
		}
	}
	else if (lsn <= WalSndCtl->lsn[mode])
	{
		/*
		 * 该 LSN 早于我们需要等待的位置。同步备库数据   
		 * 尚未初始化，但我们可以不等待，               
		 * 因为我们已经知道这样等待毫无意义，依据是   
		 * LSN.
		 */
		LWLockRelease(SyncRepLock);
		return;
	}
	else if (!SyncStandbysDefined())
	{
		/*
		 * 若走到这里，说明同步备库数据尚未被初始化，    
		 * 且该 LSN 比需要等待的位置更新，所以已经落入了 
		 * 这种情形下我们所能采取的最佳方案：检查  
		 * SyncStandbysDefined()，看 GUC 是否被设置。    
		 *
		 * 当 GUC 有值时，我们会一直等到检查点进程更新  
		 * 状态数据，因为还无法确定我们是否应该等待。   
		 * 这里 GUC *没有*值，我们可确定等待毫无意义；  
		 * 例如初始化集簇时这一点很重要，因为那时  
		 * 我们绝不应等待，没有同步备库是                   
		 * 默认行为。  
		 */
		LWLockRelease(SyncRepLock);
		return;
	}

	/*
	 * 设置好 waitLSN 让 WALSender 知道何时唤醒我们，    
	 * 自行加入队列。  
	 */
	MyProc->waitLSN = lsn;
	MyProc->syncRepState = SYNC_REP_WAITING;
	SyncRepQueueInsert(mode);
	Assert(SyncRepQueueIsOrderedByLSN(mode));
	LWLockRelease(SyncRepLock);

	/* 改 ps 显示来表明在等待同步复制。 */
	if (update_process_title)
	{
		char		buffer[32];

		sprintf(buffer, "waiting for %X/%X", LSN_FORMAT_ARGS(lsn));
		set_ps_display_suffix(buffer);
	}

	/*
	 * 等待指定的 LSN 得到确认。    
	 *
	 * 每个 proc 都有自己的等待锁存器，故按常规     
	 * 检查等待循环。
	 */
	for (;;)
	{
		int			rc;

		/* 测试状态前必须先重置锁存器。 */
		ResetLatch(MyLatch);

		/*
		 * 无需获取锁，锁存器能确保正确的内存       
		 * 屏障。若看似已完成，那就必然真的完成了，  
		 * 一旦 walsender 把状态改为 SYNC_REP_WAIT_COMPLETE，         
		 * 它就不会再更新该状态，所以我们不可能看到过期
		 * 值。       
		 */
		if (MyProc->syncRepState == SYNC_REP_WAIT_COMPLETE)
			break;

		/*
		 * 若同步复制等待仍在进行，我们既无法             
		 * 确认提交，也无法报 ERROR 或 FATAL 错误。后者会让 
		 * 客户端误以为事务已中止，但事实并非如此：     
		 * 它已经在本地提交。前者也不行：                  
		 * 客户端已经请求了同步复制，因而有权认定       
		 * 得到确认的提交同时也已被复制，                     
		 * 但实际情况可能并非如此。此时我们会发出一条   
		 * WARNING（某些客户端或可理解），并停止后续输出。
		 * 我们绝不重置 ProcDiePending，以便该进程在            
		 * 提交清理后终止。 
		 */
		if (ProcDiePending)
		{
			ereport(WARNING,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("canceling the wait for synchronous replication and terminating connection due to administrator command"),
					 errdetail("The transaction has already committed locally, but might not have been replicated to the standby.")));
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}

		/*
		 * 若查询取消中断到来，该怎么做尚不明晰。我们 
		 * 此时实际上无法中止，但完全无视该中断        
		 * 并无助益，所以我们只用一条恰当的警告来      
		 * 终止等待。  
		 */
		if (QueryCancelPending)
		{
			QueryCancelPending = false;
			ereport(WARNING,
					(errmsg("canceling wait for synchronous replication due to user request"),
					 errdetail("The transaction has already committed locally, but might not have been replicated to the standby.")));
			SyncRepCancelWait();
			break;
		}

		/*
		 * 在锁存器上等待。凡应唤醒我们的条件都会设置  
		 * 锁存器，故无需超时。
		 */
		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1,
					   WAIT_EVENT_SYNC_REP);

		/*
		 * 若 postmaster 死亡，我们多半永远得不到确认应答， 
		 * 因为所有 WAL 发送进程都会退出。所以干脆离开。
		 */
		if (rc & WL_POSTMASTER_DEATH)
		{
			ProcDiePending = true;
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}
	}

	/*
	 * WAL 发送进程已检查我们的 LSN 并把我们从队列移除。
	 * 清理好状态然后离开。在不持有 SyncRepLock 的情况下  
	 * 重置这些共享内存字段没有问题，因为当我们不在    
	 * 队列上时，任何 WAL 发送进程反正都会忽略我们。我们
	 * 需要一个读屏障以确保看见队列链接的变更（
	 * 无断言非必需，稳妥为上）。 
	 */
	pg_read_barrier();
	Assert(dlist_node_is_detached(&MyProc->syncRepLinks));
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	MyProc->waitLSN = 0;

	/* 重置 ps 显示以移除后缀       */
	if (update_process_title)
		set_ps_display_remove_suffix();
}

/*
 * 把 MyProc 插入指定的 SyncRepQueue，并维持有序不变式。       
 *
 * 通常我们会插入到队列尾部，但有可能我们到达此处   
 * 时顺序不对，所以从尾部开始向前回溯到插入点。   
 */
static void
SyncRepQueueInsert(int mode)
{
	dlist_head *queue;
	dlist_iter	iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	queue = &WalSndCtl->SyncRepQueue[mode];

	dlist_reverse_foreach(iter, queue)
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * 在我们应插入其后的队列元素处停下，以确保       
		 * 队列按 LSN 排序。 
		 */
		if (proc->waitLSN < MyProc->waitLSN)
		{
			dlist_insert_after(&proc->syncRepLinks, &MyProc->syncRepLinks);
			return;
		}
	}

	/*
	 * 若走到这里，说明列表原本为空，或者该进程需要    
	 * 在队首。
	 */
	dlist_push_head(queue, &MyProc->syncRepLinks);
}

/*
 * 获取 SyncRepLock 并取消当前进行中的任何等待。 
 */
static void
SyncRepCancelWait(void)
{
	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
	if (!dlist_node_is_detached(&MyProc->syncRepLinks))
		dlist_delete_thoroughly(&MyProc->syncRepLinks);
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	LWLockRelease(SyncRepLock);
}

void
SyncRepCleanupAtProcExit(void)
{
	/*
	 * 首先在不持锁的情况下检查我们是否从队列移除，  
	 * 以免拖慢退出。  
	 */
	if (!dlist_node_is_detached(&MyProc->syncRepLinks))
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/* 我们或许刚被移除，故再次检查  */
		if (!dlist_node_is_detached(&MyProc->syncRepLinks))
			dlist_delete_thoroughly(&MyProc->syncRepLinks);

		LWLockRelease(SyncRepLock);
	}
}

/*
 * ===========================================================
 * WAL 发送进程专用的同步复制函数               
 * ===========================================================
 */

/*
 * 采取所需行动从配置数据初始化同步复制状态。  
 * 在 WALSender 启动时及每次 SIGHUP 之后调用。 
 */
void
SyncRepInitConfig(void)
{
	int			priority;

	/*
	 * 判断我们是否是潜在同步备库，并记住该结果        
	 * 以便处理备库的应答。    
	 */
	priority = SyncRepGetStandbyPriority();
	if (MyWalSnd->sync_standby_priority != priority)
	{
		SpinLockAcquire(&MyWalSnd->mutex);
		MyWalSnd->sync_standby_priority = priority;
		SpinLockRelease(&MyWalSnd->mutex);

		ereport(DEBUG1,
				(errmsg_internal("standby \"%s\" now has synchronous standby priority %u",
								 application_name, priority)));
	}
}

/*
 * 根据我们的最新状态更新各队列的 LSN。这样     
 * 实现"首个有效同步备库释放等待者"的简单策略。      
 *
 * 其他策略也是可能的，它们会改变我们在这里的    
 * 做法，或许还有我们所存储的信息。
 */
void
SyncRepReleaseWaiters(void)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	XLogRecPtr	writePtr;
	XLogRecPtr	flushPtr;
	XLogRecPtr	applyPtr;
	bool		got_recptr;
	bool		am_sync;
	int			numwrite = 0;
	int			numflush = 0;
	int			numapply = 0;

	/*
	 * 若该 WALSender 正在服务的备库不在潜在同步备库   
	 * 列表上则无事可做。如果我们仍在启动中，          
	 * 仍在运行基础备份，或当前刷写位置无效，那么也     
	 * 快速离开。正在流式传输或停止中的 WAL 发送进程 
	 * 允许释放等待者。               
	 */
	if (MyWalSnd->sync_standby_priority == 0 ||
		(MyWalSnd->state != WALSNDSTATE_STREAMING &&
		 MyWalSnd->state != WALSNDSTATE_STOPPING) ||
		XLogRecPtrIsInvalid(MyWalSnd->flush))
	{
		announce_next_takeover = true;
		return;
	}

	/*
	 * 我们是潜在同步备库。若有足够多的同步备库       
	 * 且被视为同步，则释放等待者。  
	 */
	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

	/*
	 * 检查我们是否是同步备库，并计算所有同步备库     
	 * 的已同步位置。（注意：虽然这一步本身并不          
	 * 需要持有 SyncRepLock，但这样做似乎是                      
	 * 好主意，即在获取锁之后再做。这能确保我们用来     
	 * 释放等待者的 WAL 指针比本例程之前所用的要新  
	 * 。）        
	 */
	got_recptr = SyncRepGetSyncRecPtr(&writePtr, &flushPtr, &applyPtr, &am_sync);

	/*
	 * 若我们正在管理一个同步备库，虽然此前并不是， 
	 * 则宣布现在已是同步备库。    
	 */
	if (announce_next_takeover && am_sync)
	{
		announce_next_takeover = false;

		if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY)
			ereport(LOG,
					(errmsg("standby \"%s\" is now a synchronous standby with priority %u",
							application_name, MyWalSnd->sync_standby_priority)));
		else
			ereport(LOG,
					(errmsg("standby \"%s\" is now a candidate for quorum synchronous standby",
							application_name)));
	}

	/*
	 * 若同步备库数量少于所需数量，或者我们并未      
	 * 在管理同步备库则直接离开。 
	 */
	if (!got_recptr || !am_sync)
	{
		LWLockRelease(SyncRepLock);
		announce_next_takeover = !am_sync;
		return;
	}

	/*
	 * 先设置好 lsn，这样唤醒后端时它们会一直释放到       
	 * 该位置。  
	 */
	if (walsndctl->lsn[SYNC_REP_WAIT_WRITE] < writePtr)
	{
		walsndctl->lsn[SYNC_REP_WAIT_WRITE] = writePtr;
		numwrite = SyncRepWakeQueue(false, SYNC_REP_WAIT_WRITE);
	}
	if (walsndctl->lsn[SYNC_REP_WAIT_FLUSH] < flushPtr)
	{
		walsndctl->lsn[SYNC_REP_WAIT_FLUSH] = flushPtr;
		numflush = SyncRepWakeQueue(false, SYNC_REP_WAIT_FLUSH);
	}
	if (walsndctl->lsn[SYNC_REP_WAIT_APPLY] < applyPtr)
	{
		walsndctl->lsn[SYNC_REP_WAIT_APPLY] = applyPtr;
		numapply = SyncRepWakeQueue(false, SYNC_REP_WAIT_APPLY);
	}

	LWLockRelease(SyncRepLock);

	elog(DEBUG3, "released %d procs up to write %X/%X, %d procs up to flush %X/%X, %d procs up to apply %X/%X",
		 numwrite, LSN_FORMAT_ARGS(writePtr),
		 numflush, LSN_FORMAT_ARGS(flushPtr),
		 numapply, LSN_FORMAT_ARGS(applyPtr));
}

/*
 * 计算各同步备库之间已同步的写、刷写和应用位置。     
 *
 * 若同步备库数量少于                             
 * synchronous_standby_names 指定的数量，否则返回 true 
 * 并把位置存入 *writePtr、*flushPtr 和 *applyPtr。   
 *
 * 返回时，若该 walsender 正连接到同步备库，则 *am_sync 
 * 会被设为 true，否则设为 false。 
 */
static bool
SyncRepGetSyncRecPtr(XLogRecPtr *writePtr, XLogRecPtr *flushPtr,
					 XLogRecPtr *applyPtr, bool *am_sync)
{
	SyncRepStandbyData *sync_standbys;
	int			num_standbys;
	int			i;

	/* 初始化默认结果      */
	*writePtr = InvalidXLogRecPtr;
	*flushPtr = InvalidXLogRecPtr;
	*applyPtr = InvalidXLogRecPtr;
	*am_sync = false;

	/* 若根本未配置为同步，则快速退出      */
	if (SyncRepConfig == NULL)
		return false;

	/* 获取此刻被视为同步的备库                           */
	num_standbys = SyncRepGetCandidateStandbys(&sync_standbys);

	/* 我在候选同步备库之列吗？    */
	for (i = 0; i < num_standbys; i++)
	{
		if (sync_standbys[i].is_me)
		{
			*am_sync = true;
			break;
		}
	}

	/*
	 * 如果我们没有在管理同步备库，或者同步备库         
	 * 不足，则无事可做。     
	 */
	if (!(*am_sync) ||
		num_standbys < SyncRepConfig->num_sync)
	{
		pfree(sync_standbys);
		return false;
	}

	/*
	 * 在基于优先级的同步复制中，已同步位置取         
	 * 同步备库中最旧值。在基于法定人数的复制中则是  
	 * 第N新。  
	 *
	 * SyncRepGetNthLatestSyncRecPtr() 也能计算最旧的        
	 * 位置。但我们使用 SyncRepGetOldestSyncRecPtr() 来做该        
	 * 计算，因为它效率略高。 
	 *
	 * XXX 若当前同步备库数量与所需同步备库数量相同，    
	 * 那么也可以用 SyncRepGetOldestSyncRecPtr() 计算已同步
	 * 位置，即便在法定人数复制中。        
	 */
	if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY)
	{
		SyncRepGetOldestSyncRecPtr(writePtr, flushPtr, applyPtr,
								   sync_standbys, num_standbys);
	}
	else
	{
		SyncRepGetNthLatestSyncRecPtr(writePtr, flushPtr, applyPtr,
									  sync_standbys, num_standbys,
									  SyncRepConfig->num_sync);
	}

	pfree(sync_standbys);
	return true;
}

/*
 * 计算各同步备库之间最旧的写、刷写和应用位置。        
 */
static void
SyncRepGetOldestSyncRecPtr(XLogRecPtr *writePtr,
						   XLogRecPtr *flushPtr,
						   XLogRecPtr *applyPtr,
						   SyncRepStandbyData *sync_standbys,
						   int num_standbys)
{
	int			i;

	/*
	 * 遍历全部同步备库，计算最旧的写、刷写和应用     
	 * 位置。我们假定 *writePtr 等参数已被初始化为        
	 * InvalidXLogRecPtr.
	 */
	for (i = 0; i < num_standbys; i++)
	{
		XLogRecPtr	write = sync_standbys[i].write;
		XLogRecPtr	flush = sync_standbys[i].flush;
		XLogRecPtr	apply = sync_standbys[i].apply;

		if (XLogRecPtrIsInvalid(*writePtr) || *writePtr > write)
			*writePtr = write;
		if (XLogRecPtrIsInvalid(*flushPtr) || *flushPtr > flush)
			*flushPtr = flush;
		if (XLogRecPtrIsInvalid(*applyPtr) || *applyPtr > apply)
			*applyPtr = apply;
	}
}

/*
 * 计算各同步备库中第 N 新的写、刷写和应用           
 * 位置。
 */
static void
SyncRepGetNthLatestSyncRecPtr(XLogRecPtr *writePtr,
							  XLogRecPtr *flushPtr,
							  XLogRecPtr *applyPtr,
							  SyncRepStandbyData *sync_standbys,
							  int num_standbys,
							  uint8 nth)
{
	XLogRecPtr *write_array;
	XLogRecPtr *flush_array;
	XLogRecPtr *apply_array;
	int			i;

	/* 应有足够候选者，否则就是有人搞错了  */
	Assert(nth > 0 && nth <= num_standbys);

	write_array = (XLogRecPtr *) palloc(sizeof(XLogRecPtr) * num_standbys);
	flush_array = (XLogRecPtr *) palloc(sizeof(XLogRecPtr) * num_standbys);
	apply_array = (XLogRecPtr *) palloc(sizeof(XLogRecPtr) * num_standbys);

	for (i = 0; i < num_standbys; i++)
	{
		write_array[i] = sync_standbys[i].write;
		flush_array[i] = sync_standbys[i].flush;
		apply_array[i] = sync_standbys[i].apply;
	}

	/* 按降序对每个数组排序      */
	qsort(write_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);
	qsort(flush_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);
	qsort(apply_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);

	/* 获取第 N 新的写、刷写、应用位置*/
	*writePtr = write_array[nth - 1];
	*flushPtr = flush_array[nth - 1];
	*applyPtr = apply_array[nth - 1];

	pfree(write_array);
	pfree(flush_array);
	pfree(apply_array);
}

/*
 * 比较 lsn，以便按降序对数组排序。         
 */
static int
cmp_lsn(const void *a, const void *b)
{
	XLogRecPtr	lsn1 = *((const XLogRecPtr *) a);
	XLogRecPtr	lsn2 = *((const XLogRecPtr *) b);

	if (lsn1 > lsn2)
		return -1;
	else if (lsn1 == lsn2)
		return 0;
	else
		return 1;
}

/*
 * 返回可作为同步备库候选者的 WAL 发送进程的数据。 
 *
 * *standbys 被设置成一个 palloc 出的数组，元素为          
 * 每个 walsender 的数据结构；返回有效条目（即候选    
 * 同步发送进程）数，可能异于 num_sync，须检查。
 */
int
SyncRepGetCandidateStandbys(SyncRepStandbyData **standbys)
{
	int			i;
	int			n;

	/* 创建结果数组  */
	*standbys = (SyncRepStandbyData *)
		palloc(max_wal_senders * sizeof(SyncRepStandbyData));

	/* 若未请求同步复制则快速退出         */
	if (SyncRepConfig == NULL)
		return 0;

	/* 从共享内存收集原始数据   */
	n = 0;
	for (i = 0; i < max_wal_senders; i++)
	{
		volatile WalSnd *walsnd;	/* Use volatile pointer to prevent code
									 * rearrangement */
		SyncRepStandbyData *stby;
		WalSndState state;		/* 未包含在 SyncRepStandbyData 中*/

		walsnd = &WalSndCtl->walsnds[i];
		stby = *standbys + n;

		SpinLockAcquire(&walsnd->mutex);
		stby->pid = walsnd->pid;
		state = walsnd->state;
		stby->write = walsnd->write;
		stby->flush = walsnd->flush;
		stby->apply = walsnd->apply;
		stby->sync_standby_priority = walsnd->sync_standby_priority;
		SpinLockRelease(&walsnd->mutex);

		/* 必须活动   */
		if (stby->pid == 0)
			continue;

		/* 须为流式或停止状态   */
		if (state != WALSNDSTATE_STREAMING &&
			state != WALSNDSTATE_STOPPING)
			continue;

		/* 必须是同步的  */
		if (stby->sync_standby_priority == 0)
			continue;

		/* 必须有有效的刷写位置   */
		if (XLogRecPtrIsInvalid(stby->flush))
			continue;

		/* 好，它是候选。*/
		stby->walsnd_index = i;
		stby->is_me = (walsnd == MyWalSnd);
		n++;
	}

	/*
	 * 在法定人数模式下，我们返回所有候选者。在优先级 
	 * 模式下，若候选者过多，则只返回优先级最高的       
	 * num_sync 
	 */
	if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY &&
		n > SyncRepConfig->num_sync)
	{
		/* 按优先级排序…*/
		qsort(*standbys, n, sizeof(SyncRepStandbyData),
			  standby_priority_comparator);
		/* ……然后只报告前 num_sync 个        */
		n = SyncRepConfig->num_sync;
	}

	return n;
}

/*
 * 按优先级排序 SyncRepStandbyData 条目的 qsort 比较器
 */
static int
standby_priority_comparator(const void *a, const void *b)
{
	const SyncRepStandbyData *sa = (const SyncRepStandbyData *) a;
	const SyncRepStandbyData *sb = (const SyncRepStandbyData *) b;

	/* 首先按优先级值递增排序        */
	if (sa->sync_standby_priority != sb->sync_standby_priority)
		return sa->sync_standby_priority - sb->sync_standby_priority;

	/*
	 * 可能出现优先级值相等的情况；此时按其在 WalSnd       
	 * 数组中的位置任意打破平局。（这实在毫无道理， 
	 * 因为那取决于到达顺序，但有回归测试依赖。）  
	 */
	return sa->walsnd_index - sb->walsnd_index;
}


/*
 * 检查我们是否在同步备库列表中，若是，则确定   
 * 优先级次序。已设置则返回优先级，否则返回零以 
 * 表明我们非潜在同步备库。
 *
 * 把参数 SyncRepStandbyNames 与该 WALSender 的 application_name   
 * 比较，或若发现通配符 "*" 则允许任意名称。     
 */
static int
SyncRepGetStandbyPriority(void)
{
	const char *standby_name;
	int			priority;
	bool		found = false;

	/*
	 * 由于不允许同步级联复制，我们总是把级联              
	 * walsender 的优先级设为零。      
	 */
	if (am_cascading_walsender)
		return 0;

	if (!SyncStandbysDefined() || SyncRepConfig == NULL)
		return 0;

	standby_name = SyncRepConfig->member_names;
	for (priority = 1; priority <= SyncRepConfig->nmembers; priority++)
	{
		if (pg_strcasecmp(standby_name, application_name) == 0 ||
			strcmp(standby_name, "*") == 0)
		{
			found = true;
			break;
		}
		standby_name += strlen(standby_name) + 1;
	}

	if (!found)
		return 0;

	/*
	 * 在基于法定人数的同步复制中，列表中的所有备库     
	 * 优先级均一。 
	 */
	return (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY) ? priority : 1;
}

/*
 * 从队头开始遍历指定队列。把任何需要被唤醒的        
 * 后端状态设好，将其移出队列，再唤醒它们。     
 * 传入 all = true 以唤醒整个队列；否则只唤醒到   
 * walsender 的 LSN。
 *
 * 调用者必须以独占模式持有 SyncRepLock。
 */
static int
SyncRepWakeQueue(bool all, int mode)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	int			numprocs = 0;
	dlist_mutable_iter iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	Assert(LWLockHeldByMeInMode(SyncRepLock, LW_EXCLUSIVE));
	Assert(SyncRepQueueIsOrderedByLSN(mode));

	dlist_foreach_modify(iter, &WalSndCtl->SyncRepQueue[mode])
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * 假定队列按 LSN 排序        
		 */
		if (!all && walsndctl->lsn[mode] < proc->waitLSN)
			return numprocs;

		/*
		 * 从队列移除。
		 */
		dlist_delete_thoroughly(&proc->syncRepLinks);

		/*
		 * SyncRepWaitForLSN() 会在不持锁的情况下读取              
		 * syncRepState，所以须确保队列链接的移除先于     
		 * syncRepState 变更 
		 */
		pg_write_barrier();

		/*
		 * 把状态设为 complete；SyncRepWaitForLSN() 中              
		 * 讨论各状态。 
		 */
		proc->syncRepState = SYNC_REP_WAIT_COMPLETE;

		/*
		 * 只在已设置状态并从队列移除后才唤醒。  
		 */
		SetLatch(&(proc->procLatch));

		numprocs++;
	}

	return numprocs;
}

/*
 * 检查点进程按需调用本函数来更新共享的    
 * sync_standbys_status 标志，以免后端在                              
 * synchronous_standby_names 未设置时被永久卡住。不加              
 * 锁检查当前值是安全的，因为只有一个进程会更新它。
 * 但必须持锁才能改它。  
 */
void
SyncRepUpdateSyncStandbysDefined(void)
{
	bool		sync_standbys_defined = SyncStandbysDefined();

	if (sync_standbys_defined !=
		((WalSndCtl->sync_standbys_status & SYNC_STANDBY_DEFINED) != 0))
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/*
		 * 若 synchronous_standby_names 已被重置为空，后端        
		 * 继续等待徒劳无益。用户已不再想要同步复制，  
		 * 我们最好把它们唤醒。                    
		 */
		if (!sync_standbys_defined)
		{
			int			i;

			for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
				SyncRepWakeQueue(true, i);
		}

		/*
		 * 只在定义了同步备库时才允许大家加入队列。  
		 * 若无此互锁，会有竞态条件：                  
		 * 我们可能唤醒所有当前等待者；随后，某个尚未
		 * 重新加载配置的后端可能在队列上睡眠（并且    
		 * 不会醒来）。这样即可防止该情况。   
		 */
		WalSndCtl->sync_standbys_status = SYNC_STANDBY_INIT |
			(sync_standbys_defined ? SYNC_STANDBY_DEFINED : 0);

		LWLockRelease(SyncRepLock);
	}
	else if ((WalSndCtl->sync_standbys_status & SYNC_STANDBY_INIT) == 0)
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/*
		 * 注意此处无需唤醒这些队列。只有当                
		 * SyncStandbysDefined() 返回 false 时才会走到这条路       
		 * 径，否则就是有后端在 GUC 已设置时等待。见     
		 * SyncRepWaitForLSN() 
		 */
		Assert(!SyncStandbysDefined());

		/*
		 * 即使没有定义同步备库，也要让本信息的读者     
		 * 知道同步备库数据已被初始化。                       
		 * 这只需做一次，因此此前要检查             
		 * SYNC_STANDBY_INIT 以免无谓工作。 
		 */
		WalSndCtl->sync_standbys_status |= SYNC_STANDBY_INIT;

		LWLockRelease(SyncRepLock);
	}
}

#ifdef USE_ASSERT_CHECKING
static bool
SyncRepQueueIsOrderedByLSN(int mode)
{
	XLogRecPtr	lastLSN;
	dlist_iter	iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);

	lastLSN = 0;

	dlist_foreach(iter, &WalSndCtl->SyncRepQueue[mode])
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * 检查队列按 LSN 排序，且多个 proc 没有             
		 * 相同的 LSN     
		 */
		if (proc->waitLSN <= lastLSN)
			return false;

		lastLSN = proc->waitLSN;
	}

	return true;
}
#endif

/*
 * ===========================================================
 * 由任意进程执行的同步复制函数               
 * ===========================================================
 */

bool
check_synchronous_standby_names(char **newval, void **extra, GucSource source)
{
	if (*newval != NULL && (*newval)[0] != '\0')
	{
		int			parse_rc;
		SyncRepConfigData *pconf;

		/* 重置通信变量以确保全新的开始            */
		syncrep_parse_result = NULL;
		syncrep_parse_error_msg = NULL;

		/* 解析 synchronous_standby_names 字符串 */
		syncrep_scanner_init(*newval);
		parse_rc = syncrep_yyparse();
		syncrep_scanner_finish();

		if (parse_rc != 0 || syncrep_parse_result == NULL)
		{
			GUC_check_errcode(ERRCODE_SYNTAX_ERROR);
			if (syncrep_parse_error_msg)
				GUC_check_errdetail("%s", syncrep_parse_error_msg);
			else
				GUC_check_errdetail("synchronous_standby_names parser failed");
			return false;
		}

		if (syncrep_parse_result->num_sync <= 0)
		{
			GUC_check_errmsg("number of synchronous standbys (%d) must be greater than zero",
							 syncrep_parse_result->num_sync);
			return false;
		}

		/* GUC 额外值须由 guc_malloc 分配而非 palloc */
		pconf = (SyncRepConfigData *)
			guc_malloc(LOG, syncrep_parse_result->config_size);
		if (pconf == NULL)
			return false;
		memcpy(pconf, syncrep_parse_result, syncrep_parse_result->config_size);

		*extra = (void *) pconf;

		/*
		 * 我们无需显式清理 syncrep_parse_result。它以及         
		 * 解析期间产生的其他无用内存将在当前内存   
		 * 上下文被删除时被释放。（这段代码通常运行于   
		 * 处理配置文件所用的短生命周期上下文中，因此它 
		 * 存活不长。）
		 */
	}
	else
		*extra = NULL;

	return true;
}

void
assign_synchronous_standby_names(const char *newval, void *extra)
{
	SyncRepConfig = (SyncRepConfigData *) extra;
}

void
assign_synchronous_commit(int newval, void *extra)
{
	switch (newval)
	{
		case SYNCHRONOUS_COMMIT_REMOTE_WRITE:
			SyncRepWaitMode = SYNC_REP_WAIT_WRITE;
			break;
		case SYNCHRONOUS_COMMIT_REMOTE_FLUSH:
			SyncRepWaitMode = SYNC_REP_WAIT_FLUSH;
			break;
		case SYNCHRONOUS_COMMIT_REMOTE_APPLY:
			SyncRepWaitMode = SYNC_REP_WAIT_APPLY;
			break;
		default:
			SyncRepWaitMode = SYNC_REP_NO_WAIT;
			break;
	}
}
