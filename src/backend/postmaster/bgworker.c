/*--------------------------------------------------------------------
 * bgworker.c
 *		POSTGRES 可插拔后台 worker 实现              
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgworker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/ascii.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"

/*
 * postmaster 私有内存中的已注册后台 worker 列表。             
 */
slist_head	BackgroundWorkerList = SLIST_STATIC_INIT(BackgroundWorkerList);

/*
 * BackgroundWorkerSlot 位于共享内存中，postmaster 与普通    
 * 后端均可（经由 BackgroundWorkerArray）访问它。不过          
 * postmaster 无法获取任何锁，连 spinlock 也不行，若共享  
 * 内存一旦损坏，就可能崩溃或卡死。这不可容忍。因此 
 * 我们需要一套无锁协议，用于协调                          
 * 对该数据的访问。             
 *
 * 'in_use' 标志用于在 postmaster 与系统其余部分间交接槽位 
 * 归属权。'in_use' 为 false 时，postmaster 会完全忽略该 
 * 槽，但 'in_use' 标志本身例外，postmaster 仍可读取。        
 * 此状态下，普通后端可修改该槽。一旦某后端将 'in_use'  
 * 设为 true，槽即由 postmaster 负责，普通后端不得再
 * 修改它，但 postmaster 可查看。                                     
 * 因此，后端在初始化槽位时必须完整初始化槽位，     
 * 并插入写内存屏障，然后才能                                    
 * 标记为 in_use。  
 *
 * 不过有个例外：即便槽位正处于 in_use 状态，普通后端 
 * 也可以设置槽的 'terminate' 标志，告知 postmaster 不要
 * 重启它。后台 worker 不再运行后，该槽就会被释放以供  
 * 复用。                  
 *
 * 除与 postmaster 协调外，修改此数据结构的后端之间也   
 * 必须相互协调。由于它们可以获取锁，这很简单：任何想
 * 操纵槽位的后端都必须以独占方式加 BackgroundWorkerLock 
 * 锁。要读取可能被其他后端并发改动的数据，应以    
 * 共享模式持有该锁。总之，读取此数据结构的后端     
 * 都必须能够容忍 postmaster 的并发修改。                  
 *                                                                   
 *            
 */
typedef struct BackgroundWorkerSlot
{
	bool		in_use;
	bool		terminate;
	pid_t		pid;			/* InvalidPid=尚未启动；0=已死亡  */
	uint64		generation;		/* 槽位回收复用时递增       */
	BackgroundWorker worker;
} BackgroundWorkerSlot;

/*
 * 为了限制并行 worker 总数（依据 max_parallel_workers      
 * GUC），需维护活跃并行 worker 的数量。由于 postmaster 
 * 无法获取锁，这一目的要用两个变量来实现：已注册并行 
 * worker 的数量（由后端修改，受 BackgroundWorkerLock 保护） 
 * 和已终止并行 worker 数量（仅 postmaster 可改，无锁）。  
 * 活跃并行 worker 数量为已注册数减已终止数。这些        
 * 计数器当然会溢出，但这无关紧要，因为相减仍能得到
 * 正确的数值。                                                   
 *                                                                       
 */
typedef struct BackgroundWorkerArray
{
	int			total_slots;
	uint32		parallel_register_count;
	uint32		parallel_terminate_count;
	BackgroundWorkerSlot slot[FLEXIBLE_ARRAY_MEMBER];
} BackgroundWorkerArray;

struct BackgroundWorkerHandle
{
	int			slot;
	uint64		generation;
};

static BackgroundWorkerArray *BackgroundWorkerData;

/*
 * 内部后台 worker 入口点列表。设置它的缘由见下文 
 * LookupBackgroundWorkerFunction() 的说明。                
 */
static const struct
{
	const char *fn_name;
	bgworker_main_type fn_addr;
}			InternalBGWorkers[] =

{
	{
		"ParallelWorkerMain", ParallelWorkerMain
	},
	{
		"ApplyLauncherMain", ApplyLauncherMain
	},
	{
		"ApplyWorkerMain", ApplyWorkerMain
	},
	{
		"ParallelApplyWorkerMain", ParallelApplyWorkerMain
	}
};

/* 内部函数。    */
static bgworker_main_type LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname);


/*
 * 计算所需共享内存。    
 */
Size
BackgroundWorkerShmemSize(void)
{
	Size		size;

	/* worker 数组大小可变。        */
	size = offsetof(BackgroundWorkerArray, slot);
	size = add_size(size, mul_size(max_worker_processes,
								   sizeof(BackgroundWorkerSlot)));

	return size;
}

/*
 * 初始化共享内存。 
 */
void
BackgroundWorkerShmemInit(void)
{
	bool		found;

	BackgroundWorkerData = ShmemInitStruct("Background Worker Data",
										   BackgroundWorkerShmemSize(),
										   &found);
	if (!IsUnderPostmaster)
	{
		slist_iter	siter;
		int			slotno = 0;

		BackgroundWorkerData->total_slots = max_worker_processes;
		BackgroundWorkerData->parallel_register_count = 0;
		BackgroundWorkerData->parallel_terminate_count = 0;

		/*
		 * 把 worker 列表内容复制进共享内存。记录分给每个  
		 * worker 的共享内存槽位。这保证了                
		 * postmaster 私有列表与共享内存数组一一对应。       
		 *                  
		 */
		slist_foreach(siter, &BackgroundWorkerList)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
			RegisteredBgWorker *rw;

			rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
			Assert(slotno < max_worker_processes);
			slot->in_use = true;
			slot->terminate = false;
			slot->pid = InvalidPid;
			slot->generation = 0;
			rw->rw_shmem_slot = slotno;
			rw->rw_worker.bgw_notify_pid = 0;	/* 崩溃后或需再初始化 */
			memcpy(&slot->worker, &rw->rw_worker, sizeof(BackgroundWorker));
			++slotno;
		}

		/*
		 * 把其余槽位标记为未占用。   
		 */
		while (slotno < max_worker_processes)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

			slot->in_use = false;
			++slotno;
		}
	}
	else
		Assert(found);
}

/*
 * 在 postmaster 私有的 RegisteredBgWorker 对象列表中查找         
 * 与给定槽号对应的那个对象。        
 */
static RegisteredBgWorker *
FindRegisteredWorkerBySlotNumber(int slotno)
{
	slist_iter	siter;

	slist_foreach(siter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
		if (rw->rw_shmem_slot == slotno)
			return rw;
	}

	return NULL;
}

/*
 * 注意其他后端对共享内存所做的修改。    
 * allow_new_workers 为 true 时才接受新 worker 请求。  
 *
 * 这段代码运行在 postmaster 中，因此绝不能想当然地认为  
 * 共享内存内容是完好可信的。否则失控后端即可        
 * 搞垮 postmaster。    
 */
void
BackgroundWorkerStateChange(bool allow_new_workers)
{
	int			slotno;

	/*
	 * 共享内存中保存的槽位总数应与我们所知的         
	 * max_worker_processes 一致。若不一致，说明出了大      
	 * 问题。下文凡引用该值，一律写作         
	 * max_worker_processes，以防循环期间共享内存被损坏。     
	 *         
	 */
	if (max_worker_processes != BackgroundWorkerData->total_slots)
	{
		ereport(LOG,
				(errmsg("inconsistent background worker state (max_worker_processes=%d, total_slots=%d)",
						max_worker_processes,
						BackgroundWorkerData->total_slots)));
		return;
	}

	/*
	 * 遍历各槽位，查找新注册的 worker 或必须终止的        
	 * worker。    
	 */
	for (slotno = 0; slotno < max_worker_processes; ++slotno)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
		RegisteredBgWorker *rw;

		if (!slot->in_use)
			continue;

		/*
		 * 确保不会先于更新后的槽位内容见到 in_use       
		 * 标志。
		 */
		pg_read_barrier();

		/* 看看我们是否已知晓该 worker。       */
		rw = FindRegisteredWorkerBySlotNumber(slotno);
		if (rw != NULL)
		{
			/*
			 * 一般而言，worker 数据初次注册后不再变化。   
			 * 但有人可以设置 terminate 标志。                
			 */
			if (slot->terminate && !rw->rw_terminate)
			{
				rw->rw_terminate = true;
				if (rw->rw_pid != 0)
					kill(rw->rw_pid, SIGTERM);
				else
				{
					/* 未启动即已终止的 worker 记为已死亡。   */
					ReportBackgroundWorkerPID(rw);
				}
			}
			continue;
		}

		/*
		 * 若不允许新 worker，就立即将其标记为待终止；  
		 * 下一个代码段会负责清理它。                       
		 * 这样做确保任何等待该 worker 的进程都会被唤醒，  
		 * 即使该 worker 永远不会被允许运行。             
		 */
		if (!allow_new_workers)
			slot->terminate = true;

		/*
		 * 如果 worker 被标记待终止，就无需将其加入已注册  
		 * worker 列表，直接释放槽位即可。但如果设置了      
		 * bgw_notify_pid，注册该 worker 的进程可能需要知道    
		 * 我们已处理终止请求，因此务必向它发送信号。    
		 *              
		 */
		if (slot->terminate)
		{
			int			notify_pid;

			/*
			 * 这里需要内存屏障，确保 bgw_notify_pid 读取    
			 * 与 parallel_terminate_count 的更新先于对 in_use   
			 * 的写入完成。                  
			 */
			notify_pid = slot->worker.bgw_notify_pid;
			if ((slot->worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
				BackgroundWorkerData->parallel_terminate_count++;
			slot->pid = 0;

			pg_memory_barrier();
			slot->in_use = false;

			if (notify_pid != 0)
				kill(notify_pid, SIGUSR1);

			continue;
		}

		/*
		 * 把注册数据复制进已注册 worker 列表。          
		 */
		rw = malloc(sizeof(RegisteredBgWorker));
		if (rw == NULL)
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
			return;
		}

		/*
		 * 以偏执的方式复制字符串。如果共享内存已损坏， 
		 * 源数据甚至可能缺 NUL 结尾。       
		 */
		ascii_safe_strlcpy(rw->rw_worker.bgw_name,
						   slot->worker.bgw_name, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_type,
						   slot->worker.bgw_type, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_library_name,
						   slot->worker.bgw_library_name, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_function_name,
						   slot->worker.bgw_function_name, BGW_MAXLEN);

		/*
		 * 复制各种定长字段。    
		 *
		 * flags、start_time、restart_time 由 postmaster 检查，即使   
		 * 损坏也无碍。其余字段只会被子进程检查，  
		 * 它可能崩溃，但我们不会。                            
		 *                           
		 */
		rw->rw_worker.bgw_flags = slot->worker.bgw_flags;
		rw->rw_worker.bgw_start_time = slot->worker.bgw_start_time;
		rw->rw_worker.bgw_restart_time = slot->worker.bgw_restart_time;
		rw->rw_worker.bgw_main_arg = slot->worker.bgw_main_arg;
		memcpy(rw->rw_worker.bgw_extra, slot->worker.bgw_extra, BGW_EXTRALEN);

		/*
		 * 复制需要获知状态变化的 PID，但前提是 postmaster 
		 * 知道拥有该 PID 的后端。如果 postmaster 不知道该    
		 * PID 也非错误，因为请求该 worker 的后端可能刚刚  
		 * 发出请求即死亡（或被杀）了。不过，至少在   
		 * 积累一些实际经验之前，要以相对较高的调试级别 
		 * 记录一条日志。                                           
		 *                  
		 */
		rw->rw_worker.bgw_notify_pid = slot->worker.bgw_notify_pid;
		if (!PostmasterMarkPIDForWorkerNotify(rw->rw_worker.bgw_notify_pid))
		{
			elog(DEBUG1, "worker notification PID %d is not valid",
				 (int) rw->rw_worker.bgw_notify_pid);
			rw->rw_worker.bgw_notify_pid = 0;
		}

		/* 初始化 postmaster 记账。     */
		rw->rw_backend = NULL;
		rw->rw_pid = 0;
		rw->rw_child_slot = 0;
		rw->rw_crashed_at = 0;
		rw->rw_shmem_slot = slotno;
		rw->rw_terminate = false;

		/* 记录  */
		ereport(DEBUG1,
				(errmsg_internal("registering background worker \"%s\"",
								 rw->rw_worker.bgw_name)));

		slist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
	}
}

/*
 * 忘记不再需要的后台 worker。                    
 *
 * 必须通过传入指向它的 slist_mutable_iter 来标识。     
 * 这一惯例允许在遍历 worker 列表期间删除 worker， 
 * 也省去了再次搜索列表的麻烦。                             
 *
 * 如属适当，由调用者负责通知 bgw_notify_pid。          
 *
 * 本函数只能在 postmaster 中调用。           
 */
void
ForgetBackgroundWorker(slist_mutable_iter *cur)
{
	RegisteredBgWorker *rw;
	BackgroundWorkerSlot *slot;

	rw = slist_container(RegisteredBgWorker, rw_lnode, cur->cur);

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	Assert(slot->in_use);

	/*
	 * 这里需要内存屏障，确保 parallel_terminate_count   
	 * 的更新先于对 in_use 的写入完成。                  
	 */
	if ((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
		BackgroundWorkerData->parallel_terminate_count++;

	pg_memory_barrier();
	slot->in_use = false;

	ereport(DEBUG1,
			(errmsg_internal("unregistering background worker \"%s\"",
							 rw->rw_worker.bgw_name)));

	slist_delete_current(cur);
	free(rw);
}

/*
 * 在共享内存中登记新启动后台 worker 的 PID。             
 *
 * 本函数只应由 postmaster 调用。                 
 */
void
ReportBackgroundWorkerPID(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->pid = rw->rw_pid;

	if (rw->rw_worker.bgw_notify_pid != 0)
		kill(rw->rw_worker.bgw_notify_pid, SIGUSR1);
}

/*
 * 登记某后台 worker 的 PID 已变为 0，因为一个先前 
 * 在运行的后台 worker 已退出。          
 *
 * 本函数只应由 postmaster 调用。                 
 */
void
ReportBackgroundWorkerExit(slist_mutable_iter *cur)
{
	RegisteredBgWorker *rw;
	BackgroundWorkerSlot *slot;
	int			notify_pid;

	rw = slist_container(RegisteredBgWorker, rw_lnode, cur->cur);

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->pid = rw->rw_pid;
	notify_pid = rw->rw_worker.bgw_notify_pid;

	/*
	 * 如果该 worker 已列入待注销名单，先完成注销再       
	 * 通知启动它的进程。否则若该进程立即尝试复用   
	 * 槽位，该槽可能尚不可用。理论上，如果进程恰好   
	 * 在错误的时刻检查 slot->pid，这种情形无论如何       
	 * 都可能发生，但这能把窗口收窄。    
	 */
	if (rw->rw_terminate ||
		rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
		ForgetBackgroundWorker(cur);

	if (notify_pid != 0)
		kill(notify_pid, SIGUSR1);
}

/*
 * 为一个正在退出的后端所属的 PID 取消 SIGUSR1 通知。   
 *
 * 本函数只应由 postmaster 调用。                 
 */
void
BackgroundWorkerStopNotifications(pid_t pid)
{
	slist_iter	siter;

	slist_foreach(siter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
		if (rw->rw_worker.bgw_notify_pid == pid)
			rw->rw_worker.bgw_notify_pid = 0;
	}
}

/*
 * 取消那些已有进程在等待的、尚未启动的 worker 请求。
 *
 * 本函数在正常（"smart"/"fast"）数据库关闭期间调用。 
 * 此后不会再启动新的后台 worker，因此任何可能在        
 * 等待它们的进程都必须结束等待。我们的做法          
 * 是完全取消 bgworker 注册，这或许有点小题大做，      
 * 但既然正在关闭，注册记录是否留存已无关紧要。    
 *                                   
 *
 * 本函数只应由 postmaster 调用。                 
 */
void
ForgetUnstartedBackgroundWorkers(void)
{
	slist_mutable_iter iter;

	slist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;
		BackgroundWorkerSlot *slot;

		rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);
		Assert(rw->rw_shmem_slot < max_worker_processes);
		slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];

		/* 若它尚未启动，且有人在等待...               */
		if (slot->pid == InvalidPid &&
			rw->rw_worker.bgw_notify_pid != 0)
		{
			/* ...就删掉它，并通知等待者   */
			int			notify_pid = rw->rw_worker.bgw_notify_pid;

			ForgetBackgroundWorker(&iter);
			if (notify_pid != 0)
				kill(notify_pid, SIGUSR1);
		}
	}
}

/*
 * 重置后台 worker 崩溃状态。 
 *
 * 我们假定，在经历崩溃与重启的一个周期之后，            
 * 不带 never-restart 标志的后台 worker 应立即重启，            
 * 而不必等待 bgw_restart_time 流逝。另一方面，带有           
 * 该标志的 worker 应立即忘记，因为我们永不重启。 
 *
 * 本函数只应由 postmaster 调用。                 
 */
void
ResetBackgroundWorkerCrashTimes(void)
{
	slist_mutable_iter iter;

	slist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
		{
			/*
			 * 标记为 BGW_NEVER_RESTART 的 worker 在崩溃后不应      
			 * 再重启，所以要忘掉它们。（若等到崩溃后    
			 * 才忘记它们，而它们又是并行 worker，那么   
			 * 在我们已将 parallel_register_count 清零之后，  
			 * parallel_terminate_count 仍会递增，那就糟了。）  
			 */
			ForgetBackgroundWorker(&iter);
		}
		else
		{
			/*
			 * 我们经由 parallel_register_count 与                  
			 * parallel_terminate_count 完成的记账，若标记为        
			 * parallel 的 worker 若挺过崩溃重启循环，记账  
			 * 会乱套。这类 worker 都应标为 BGW_NEVER_RESTART， 
			 * 控制流不该到此分支。 
			 */
			Assert((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) == 0);

			/*
			 * 允许该 worker 在我们完成重置后立即              
			 * 重启。 
			 */
			rw->rw_crashed_at = 0;

			/*
			 * 等待它的那些进程就此完蛋了。          
			 */
			rw->rw_worker.bgw_notify_pid = 0;
		}
	}
}

#ifdef EXEC_BACKEND
/*
 * EXEC_BACKEND 模式下，worker 用它从共享内存中取          
 * 回详情。  
 */
BackgroundWorker *
BackgroundWorkerEntry(int slotno)
{
	static BackgroundWorker myEntry;
	BackgroundWorkerSlot *slot;

	Assert(slotno < BackgroundWorkerData->total_slots);
	slot = &BackgroundWorkerData->slot[slotno];
	Assert(slot->in_use);

	/* 必须复制一份，以防不再保留共享内存访问。  */
	memcpy(&myEntry, &slot->worker, sizeof myEntry);
	return &myEntry;
}
#endif

/*
 * 以错误级别 elevel 报告 BackgroundWorker 定义的问题。       
 * 若看起来没问题则返回 true，否则返回 false（不过   
 * elevel >= ERROR 时则根本不会返回）。        
 */
static bool
SanityCheckBackgroundWorker(BackgroundWorker *worker, int elevel)
{
	/* 校验 flags。        */

	/*
	 * 过去我们支持不连接共享内存的 worker，但现在       
	 * 不再支持了。因此该标志现在是必需的。出于兼容性 
	 * 考虑，也由于该标志在阅读代码时仍能提供一些   
	 * 信息，故未移除。 
	 */
	if (!(worker->bgw_flags & BGWORKER_SHMEM_ACCESS))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": background workers without shared memory access are not supported",
						worker->bgw_name)));
		return false;
	}

	if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)
	{
		if (worker->bgw_start_time == BgWorkerStart_PostmasterStart)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("background worker \"%s\": cannot request database access if starting at postmaster start",
							worker->bgw_name)));
			return false;
		}

		/* XXX 其他检查  */
	}

	if ((worker->bgw_restart_time < 0 &&
		 worker->bgw_restart_time != BGW_NEVER_RESTART) ||
		(worker->bgw_restart_time > USECS_PER_DAY / 1000))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": invalid restart interval",
						worker->bgw_name)));
		return false;
	}

	/*
	 * 并行 worker 不可配置为自动重启，因为             
	 * parallel_register_count/parallel_terminate_count 这             
	 * 套记账无法应对挺过崩溃重启周期的并行 worker。  
	 */
	if (worker->bgw_restart_time != BGW_NEVER_RESTART &&
		(worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": parallel workers may not be configured for restart",
						worker->bgw_name)));
		return false;
	}

	/*
	 * bgw_type 未填则用 bgw_name。          
	 */
	if (strcmp(worker->bgw_type, "") == 0)
		strcpy(worker->bgw_type, worker->bgw_name);

	return true;
}

/*
 * 后台 worker 的标准 SIGTERM 处理器      
 */
static void
bgworker_die(SIGNAL_ARGS)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	ereport(FATAL,
			(errcode(ERRCODE_ADMIN_SHUTDOWN),
			 errmsg("terminating background worker \"%s\" due to administrator command",
					MyBgworkerEntry->bgw_type)));
}

/*
 * 启动新后台 worker       
 *
 * 这是后台 worker 的主入口点，由 postmaster 调用。       
 *            
 */
void
StartBackgroundWorker(void)
{
	sigjmp_buf	local_sigjmp_buf;
	BackgroundWorker *worker = MyBgworkerEntry;
	bgworker_main_type entrypt;

	if (worker == NULL)
		elog(FATAL, "unable to find bgworker entry");

	IsBackgroundWorker = true;

	MyBackendType = B_BG_WORKER;
	init_ps_display(worker->bgw_name);

	SetProcessingMode(InitProcessing);

	/* 应用 PostAuthDelay*/
	if (PostAuthDelay > 0)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * 建立信号处理器  
	 */
	if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)
	{
		/*
		 * SIGINT 用于请求取消当前操作                
		 */
		pqsignal(SIGINT, StatementCancelHandler);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/* XXX 还需其他处理器吗？     */
	}
	else
	{
		pqsignal(SIGINT, SIG_IGN);
		pqsignal(SIGUSR1, SIG_IGN);
		pqsignal(SIGFPE, SIG_IGN);
	}
	pqsignal(SIGTERM, bgworker_die);
	/* SIGQUIT 处理器已由 InitPostmasterChild 建立完毕  */
	pqsignal(SIGHUP, SIG_IGN);

	InitializeTimeouts();		/* 建立 SIGALRM 处理器    */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * 若遇到异常，处理流程会从此处继续。     
	 *
	 * 只需清理现场、报告错误，然后退出。     
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* 由于未使用 PG_TRY，必须手工重置错误栈   */
		error_context_stack = NULL;

		/* 清理期间屏蔽中断             */
		HOLD_INTERRUPTS();

		/*
		 * sigsetjmp 已阻塞所有信号，但在与并行 leader 通信时
		 * 我们可能要接收信号。一旦执行 HOLD_INTERRUPTS()，  
		 * 解除信号阻塞就应该是安全的。                  
		 */
		BackgroundWorkerUnblockSignals();

		/* 向并行 leader 与服务器日志报告该错误         */
		EmitErrorReport();

		/*
		 * 此处还需要更多清理吗？对连接共享内存的       
		 * bgworker 会调用下面的 InitProcess，它将安装 ProcKill 
		 * 作为退出回调，它会负责释放锁等事宜。 
		 */

		/* 然后退出*/
		proc_exit(1);
	}

	/* 现在可处理 ereport(ERROR)   */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * 在共享内存中为每个后端创建 PGPROC 结构，          
	 * EXEC_BACKEND 模式除外，那一情形已在 SubPostmasterMain      
	 * 中完成。使用 LWLock 之前必须先做到这一点（而         
	 * EXEC_BACKEND 下已用过它）。 
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/*
	 * 早期初始化。   
	 */
	BaseInit();

	/*
	 * 查找入口点函数，必要时加载其所在的库。          
	 */
	entrypt = LookupBackgroundWorkerFunction(worker->bgw_library_name,
											 worker->bgw_function_name);

	/*
	 * 注意在普通进程中，我们会在此处调用 InitPostgres。   
	 * 但对 worker 而言，我们还不知道要连接哪个数据库；  
	 * 因此需要等待用户代码经由        
	 * BackgroundWorkerInitializeConnection() 
	 */

	/*
	 * 现在调用用户的 worker 代码    
	 */
	entrypt(worker->bgw_main_arg);

	/* ...它若返回，即告完成    */
	proc_exit(0);
}

/*
 * 注册一个新的静态后台 worker。
 *
 * 本函数只能由 postmaster 直接调用，或在由             
 * shared_preload_libraries 加载的库的 _PG_init 函数中调用；   
 * 否则不起任何作用。      
 */
void
RegisterBackgroundWorker(BackgroundWorker *worker)
{
	RegisteredBgWorker *rw;
	static int	numworkers = 0;

	if (!IsUnderPostmaster)
		ereport(DEBUG1,
				(errmsg_internal("registering background worker \"%s\"", worker->bgw_name)));

	if (!process_shared_preload_libraries_in_progress &&
		strcmp(worker->bgw_library_name, "postgres") != 0)
	{
		if (!IsUnderPostmaster)
			ereport(LOG,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("background worker \"%s\": must be registered in shared_preload_libraries",
							worker->bgw_name)));
		return;
	}

	if (!SanityCheckBackgroundWorker(worker, LOG))
		return;

	if (worker->bgw_notify_pid != 0)
	{
		ereport(LOG,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("background worker \"%s\": only dynamic background workers can request notification",
						worker->bgw_name)));
		return;
	}

	/*
	 * 强制执行 worker 数上限。注意这一限制过于严格：我们
	 * 本可以允许更多不连接共享内存的 worker，因为它们    
	 * 不计入别处 MAX_BACKENDS 限制。目前来看，放宽这条  
	 * 限制似乎并不重要。         
	 */
	if (++numworkers > max_worker_processes)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("too many background workers"),
				 errdetail_plural("Up to %d background worker can be registered with the current settings.",
								  "Up to %d background workers can be registered with the current settings.",
								  max_worker_processes,
								  max_worker_processes),
				 errhint("Consider increasing the configuration parameter \"max_worker_processes\".")));
		return;
	}

	/*
	 * 把注册数据复制进已注册 worker 列表。          
	 */
	rw = malloc(sizeof(RegisteredBgWorker));
	if (rw == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}

	rw->rw_worker = *worker;
	rw->rw_backend = NULL;
	rw->rw_pid = 0;
	rw->rw_child_slot = 0;
	rw->rw_crashed_at = 0;
	rw->rw_terminate = false;

	slist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
}

/*
 * 由普通后端注册新的后台 worker。             
 *
 * 成功返回 true，失败返回 false。失败通常表示当前没有   
 * 可用的后台 worker 槽位。                        
 *
 * 如果 handle 非 NULL，我们会把 *handle 设为一个指针，    
 * 该指针随后可用作 GetBackgroundWorkerPid() 的参数。     
 * 调用者可按需以 pfree() 释放。     
 */
bool
RegisterDynamicBackgroundWorker(BackgroundWorker *worker,
								BackgroundWorkerHandle **handle)
{
	int			slotno;
	bool		success = false;
	bool		parallel;
	uint64		generation = 0;

	/*
	 * 我们无法从 postmaster 注册动态后台 worker。如果这是 
	 * 单机后端，我们是唯一进程，无法再启动更多。     
	 * 多进程环境中，理论上或许可行，但由于加锁方面  
	 * 的考虑，暂不支持；参见 BackgroundWorkerSlot    
	 * 数据结构的注释。                                     
	 *           
	 */
	if (!IsUnderPostmaster)
		return false;

	if (!SanityCheckBackgroundWorker(worker, ERROR))
		return false;

	parallel = (worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0;

	LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);

	/*
	 * 如果这是并行 worker，检查并行 worker 是否已太多；    
	 * 若是，就不再注册新的。我们对                       
	 * parallel_terminate_count 的看法可能稍有滞后，但这无关    
	 * 紧要：即便早一点到这里，也会得到同样结果。  
	 * 这一点也无能为力，因为 postmaster 绝不能获取锁； 
	 * 内存屏障保证不了任何有用的东西。                   
	 *                 
	 */
	if (parallel && (BackgroundWorkerData->parallel_register_count -
					 BackgroundWorkerData->parallel_terminate_count) >=
		max_parallel_workers)
	{
		Assert(BackgroundWorkerData->parallel_register_count -
			   BackgroundWorkerData->parallel_terminate_count <=
			   MAX_PARALLEL_WORKER_LIMIT);
		LWLockRelease(BackgroundWorkerLock);
		return false;
	}

	/*
	 * 寻找未使用的槽位。找到就占据它。  
	 */
	for (slotno = 0; slotno < BackgroundWorkerData->total_slots; ++slotno)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

		if (!slot->in_use)
		{
			memcpy(&slot->worker, worker, sizeof(BackgroundWorker));
			slot->pid = InvalidPid; /* 表示尚未启动        */
			slot->generation++;
			slot->terminate = false;
			generation = slot->generation;
			if (parallel)
				BackgroundWorkerData->parallel_register_count++;

			/*
			 * 确保 postmaster 不会在新内容就位前把槽位       
			 * 当作已占用。    
			 */
			pg_write_barrier();

			slot->in_use = true;
			success = true;
			break;
		}
	}

	LWLockRelease(BackgroundWorkerLock);

	/* 若找到了槽位，就提醒 postmaster 注意此变化。  */
	if (success)
		SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);

	/*
	 * 如果找到了槽位且用户提供了 handle，就初始化它。  
	 */
	if (success && handle)
	{
		*handle = palloc(sizeof(BackgroundWorkerHandle));
		(*handle)->slot = slotno;
		(*handle)->generation = generation;
	}

	return success;
}

/*
 * 获取动态注册的后台 worker 的 PID。             
 *
 * 若认定 worker 正在运行，返回值将为 BGWH_STARTED，    
 * *pidp 将得到该 worker 进程的 PID。如果 postmaster 尚未    
 * 尝试启动该 worker，返回值将为 BGWH_NOT_YET_STARTED。           
 * 否则返回值为 BGWH_STOPPED。                                    
 *
 * BGWH_STOPPED 既可表示 worker 暂时停止（因为它配置        
 * 了自动重启且以非零值退出），也可表示 worker 永久 
 * 停止（因它以退出码 0 退出，或未配置自动重启）， 
 * 甚至表示 worker 从未启动即被注销（或因启动失败     
 * 且 worker 未配置自动重启，或因在 worker 成功启动     
 * 之前使用了 TerminateBackgroundWorker）。                          
 *                                                                      
 *          
 */
BgwHandleStatus
GetBackgroundWorkerPid(BackgroundWorkerHandle *handle, pid_t *pidp)
{
	BackgroundWorkerSlot *slot;
	pid_t		pid;

	Assert(handle->slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[handle->slot];

	/*
	 * 我们大概可只靠内存屏障来同步数据访问，但眼下  
	 * 还是保持简单，直接获取锁。这里似乎不太可能有
	 * 足够的流量，从而造成有意义的争用。               
	 *                                 
	 */
	LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

	/*
	 * 持有锁期间，generation 编号不会被并发修改。          
	 * pid 由 postmaster 更新，随时可能变，但我们假定这些  
	 * 变化是原子的。因此我们读到的值不会是垃圾，   
	 * 但在调用者查看时可能已过时（不过这本来就无可  
	 * 避免）。                                
	 *
	 * in_use 标志可能正处于从 true 变为 false 的过程中，但   
	 * 如果它已是 false，就不会再变化。           
	 */
	if (handle->generation != slot->generation || !slot->in_use)
		pid = 0;
	else
		pid = slot->pid;

	/* 完成。 */
	LWLockRelease(BackgroundWorkerLock);

	if (pid == 0)
		return BGWH_STOPPED;
	else if (pid == InvalidPid)
		return BGWH_NOT_YET_STARTED;
	*pidp = pid;
	return BGWH_STARTED;
}

/*
 * 等待一个后台 worker 启动。      
 *
 * 本函数与 GetBackgroundWorkerPid() 类似，但如果 worker         
 * 尚未启动，我们会等它启动；因此绝不会返回             
 * BGWH_NOT_YET_STARTED。但如果 postmaster 已死亡，我们就    
 * 放弃并返回 BGWH_POSTMASTER_DIED，因为知道启动不会       
 * 发生。  
 *
 * 调用者必须已把我们的 PID 设为该 worker 的           
 * bgw_notify_pid，否则状态变化时不会被及时唤醒。      
 */
BgwHandleStatus
WaitForBackgroundWorkerStartup(BackgroundWorkerHandle *handle, pid_t *pidp)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		status = GetBackgroundWorkerPid(handle, &pid);
		if (status == BGWH_STARTED)
			*pidp = pid;
		if (status != BGWH_NOT_YET_STARTED)
			break;

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0,
					   WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_POSTMASTER_DEATH)
		{
			status = BGWH_POSTMASTER_DIED;
			break;
		}

		ResetLatch(MyLatch);
	}

	return status;
}

/*
 * 等待一个后台 worker 停止。  
 *
 * 如果该 worker 尚未启动或正在运行，我们会等它停止，
 * 然后返回 BGWH_STOPPED。但如果 postmaster 已死亡，我们就     
 * 放弃并返回 BGWH_POSTMASTER_DIED，因为 worker 状态变化   
 * 时是由 postmaster 通知我们的。   
 *
 * 调用者必须已把我们的 PID 设为该 worker 的           
 * bgw_notify_pid，否则状态变化时不会被及时唤醒。      
 */
BgwHandleStatus
WaitForBackgroundWorkerShutdown(BackgroundWorkerHandle *handle)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		status = GetBackgroundWorkerPid(handle, &pid);
		if (status == BGWH_STOPPED)
			break;

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0,
					   WAIT_EVENT_BGWORKER_SHUTDOWN);

		if (rc & WL_POSTMASTER_DEATH)
		{
			status = BGWH_POSTMASTER_DIED;
			break;
		}

		ResetLatch(MyLatch);
	}

	return status;
}

/*
 * 指示 postmaster 终止某个后台 worker。           
 *
 * 注意这一操作无需顾及该 worker 是否仍在运行，        
 * 甚至无需顾及它是否已经退出并被注销。               
 *              
 */
void
TerminateBackgroundWorker(BackgroundWorkerHandle *handle)
{
	BackgroundWorkerSlot *slot;
	bool		signal_postmaster = false;

	Assert(handle->slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[handle->slot];

	/* 在共享内存设置 terminate 标志，除非槽位被复用。 */
	LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);
	if (handle->generation == slot->generation)
	{
		slot->terminate = true;
		signal_postmaster = true;
	}
	LWLockRelease(BackgroundWorkerLock);

	/* 确保 postmaster 注意到共享内存的这一变化。     */
	if (signal_postmaster)
		SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);
}

/*
 * 查找（并可能加载）bgworker 入口点函数。      
 *
 * 对核心代码中的函数，我们使用库名 "postgres"，并查    
 * 阅 InternalBGWorkers 数组。外部函数则通过            
 * load_external_function() 查找，必要时加以加载。         
 *
 * 这样做的意义在于以字符串形式跨进程边界传递      
 * 函数名。我们无法传递实际的函数地址，因为函数 
 * 有可能已在不同进程中加载到不同的地址。对可     
 * 加载库中的函数来说，这显然是一种危险；但即使  
 * 对核心代码中的函数，在使用 EXEC_BACKEND 的平台           
 * （如 Windows）上也可能发生。          
 *
 * 将来或许值得去掉 InternalBGWorkers[]，改为对核心        
 * 函数也应用 load_external_function()；但这会带来           
 * 可移植性问题，眼下不值得处理。                        
 */
static bgworker_main_type
LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname)
{
	/*
	 * 如果该函数要从 postgres 本体加载，就搜索         
	 * InternalBGWorkers 数组
	 */
	if (strcmp(libraryname, "postgres") == 0)
	{
		int			i;

		for (i = 0; i < lengthof(InternalBGWorkers); i++)
		{
			if (strcmp(InternalBGWorkers[i].fn_name, funcname) == 0)
				return InternalBGWorkers[i].fn_addr;
		}

		/* 只有编程错误才会走到这里。      */
		elog(ERROR, "internal function \"%s\" not found", funcname);
	}

	/* 否则从外部库加载。           */
	return (bgworker_main_type)
		load_external_function(libraryname, funcname, true, NULL);
}

/*
 * 根据给定 PID 获取后台 worker 的 bgw_type。若非有效        
 * 后台 worker 返回 NULL。  
 *
 * 返回值位于本函数专属的静态内存中，因此必须在        
 * 再次调用本函数之前使用它。这样调用者就不必           
 * 操心后台 worker 的加锁协议。                             
 */
const char *
GetBackgroundWorkerTypeByPid(pid_t pid)
{
	int			slotno;
	bool		found = false;
	static char result[BGW_MAXLEN];

	LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

	for (slotno = 0; slotno < BackgroundWorkerData->total_slots; slotno++)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

		if (slot->pid > 0 && slot->pid == pid)
		{
			strcpy(result, slot->worker.bgw_type);
			found = true;
			break;
		}
	}

	LWLockRelease(BackgroundWorkerLock);

	if (!found)
		return NULL;

	return result;
}
