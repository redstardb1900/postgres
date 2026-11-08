/*-------------------------------------------------------------------------
 * applyparallelworker.c
 *	   并行应用工作进程用于应用事务的支持例程  
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/applyparallelworker.c
 *
 * 本文件包含启动、建立和拆除并行应用工作进程的代码， 
 * 该工作进程从领导者工作进程接收变更，并调用例程将     
 * 其应用到订阅者数据库上。此外，本文件还包含旨在支   
 * 持建立、使用和拆除 ParallelApplyWorkerInfo 的例程，领导者  
 * 工作进程与并行应用工作进程需要它，以便彼此之间能   
 * 够相互通信。                            
 *
 * 对于将 'streaming' 选项设为 parallel 的订阅，只要          
 * 收到事务的第一个流就会分配并行应用工作进程（若     
 * 有可用的工作进程）。领导者应用工作进程经共享内存 
 * 向这个新工作进程发送变更。我们会让该工作进程保持分
 * 配，直到收到事务提交并在提交时等待它完成。这样既 
 * 能保持提交顺序并避免文件 I/O，尽管在没有可用       
 * 工作进程时仍需转储到文件。关于变更被转储到磁盘的流
 * 式事务，参见 logical/worker 顶部的注释。维护提交顺序 
 * 至关重要，以免：(a) 事务依赖关系 - 比如在第一个事务  
 * 插入一行，随后第二个事务在发布者上更新它，若允许  
 * 订阅者并行应用两者，可能导致更新失败；(b) 死锁  
 * ——允许那些以与提交顺序相反的顺序更新同一组          
 * 行/表的事务进行并行应用，这可能                           
 * 会导致                                                              
 * 死锁。 
 *
 * 工作进程池避免为每个流式事务重启工作进程。我  
 * 们会把每个工作进程的信息（ParallelApplyWorkerInfo）维护在  
 *  ParallelApplyWorkerPool 中。成功启动新工作进程后，其信息 
 * 会被加入 ParallelApplyWorkerPool。工作进程完成事务应用   
 * 后，会被标记为可供重用。在启动新的工作进程来应   
 * 用流式事务之前，我们会检查列表中是否有可用工作进
 * 程。注意，我们在池中最多保留 max_parallel_apply_workers    
 * _per_subscription 数量一半的工作进程，超过之后，我们只    
 * 会在应用完事务后让该工作进程退出。                  
 *
 * XXX 工作进程池阈值是随意定的，如有需要，将来    
 * 可为此提供一个 GUC 变量。         
 *
 * 领导者应用工作进程在每个并行应用工作进程启动时创建 
 * 单独的动态共享内存段。这样设计是因为我们无法预测  
 * 需要多少工作进程。虽然或许能根据并行应用工作进程
 * 的最大数量（max_parallel_apply_workers_per_subscription）在一个  
 * 段中分配足够的共享内存，但如果实际没有启动任何进 
 * 程，这会浪费内存。                               
 *
 * 动态共享内存段包含：(a) 一个 shm_mq，用于从事务中把 
 * 变更从领导者应用工作进程发送给并行应用工作进程；  
 * (b) 另一个 shm_mq，用于从并行应用工作进程向领导应用工
 * 作进程发送错误及其他经 elog/ereport 报告的消息；        
 * (c) 并行应用工作进程与领导者应用工作进程间需要共享  
 * 必要信息（即 ParallelApplyWorkerShared 的成员）。            
 *
 * 锁注意事项       
 * ----------------------
 * 由于两侧数据库的结构（如订阅表模式、约束等）不同，
 * 发布者侧相互独立的事务，在订阅者侧会相互依     
 * 赖；并行模式下并发应用这些事务存在死锁风险。即使 
 * 不使用并行模式，订阅者上有并发操作也可能发生        
 * 情况。要检测领导者（LA）与并行应用（PA）间死锁，当 
 *  PA 等待下一流（变更集）且 LA 等 PA 完成事务时，我们 
 * 就使用了 lmgr 锁。                                                 
 * 也可以采取另一种做法：当发布者与订阅者两边的表模式
 * 互不相同时，就不允许使用并行，不过这样的做法会过于 
 * 严苛，而且会要求发布者                                         
 * 发送比它目前所发送的多得多的                       
 * 信息。                                
 *
 * 考虑这样一种情况：被订阅的表在发布者上没有唯一键    
 * 而在订阅者上有唯一键。这种情况下，死锁可能按以下    
 * 方式发生：    
 *
 * 1) 领导者应用工作进程与并行应用工作进程间的死锁  
 *
 * 设想并行应用工作进程（PA）正执行 TX-1，领导应用工
 * 作进程（LA）则在订阅者上并发地执行 TX-2。这时，由于 
 * 被订阅表的唯一键约束，LA 等待 PA，而 PA 等 LA 发送 
 * 下一股变更流，或是事务                                     
 * 结束命令消息。                         
 *
 * 为了让 lmgr 能检测到这一点，我们让 LA 在远程事务上获 
 * 取会话锁（pa_lock_stream()），让 PA 在尝试接收下一股        
 * 变更流之前先等待该锁。具体来说，LA 会在发送 STREAM_    
 * STOP 之前以 AccessExclusive 模式获取锁，并在发送 STREA     
 * M_START、STREAM_ABORT（针对顶层事务）、STREAM_PREPARE            
 *  和 STREAM_COMMIT 之后（若已获取）释放锁。PA 会在处理    
 * 完 STREAM_STOP 和 STREAM_ABORT（针对子事务）后，以       
 *  AccessShare 模式获取锁，然后在获取之后立即释放       
 * 该锁。          
 *
 * 上述示例的锁图看起来会如下所示：          
 * LA（正在等待获取唯一索引上的锁）-> PA（正在等待获
 * 取流锁）-> LA             
 *
 * 这样，当 PA 因为等待 LA 的下一股变更流而等待时，我们
 * 就能在 lmgr 中得到 PA 到 LA 的等待边，从而可检测出 
 *  LA 与 PA 间的死锁。 
 *
 * 2) 领导者应用工作进程与多个并行应用工作进程死锁 
 *
 * 这种场景与第一种情况类似，但 TX-1 和 TX-2 这次是由两    
 * 个并行应用工作进程（PA-1 和 PA-2）执行的。在这个场景 
 * 中，PA-2 正在等待 PA-1 完成其事务，而 PA-1 正在等待来   
 * 自 LA 的后续输入。此外，LA 也在等待 PA-2 完成其事务    
 * ，以保持提交顺序。这三个进程之间                            
 * 存在死锁。     
 *
 * 为了让 lmgr 能检测到这点，我们让 PA 在正被应用的事务   
 * 上获取会话锁（不同于前例中的锁，参见  
 * pa_lock_transaction()），并让 LA 在继续处理事务结束命令     
 * 之前等待该锁。PA 将在执行事务的第一条消息之前以       
 *  AccessExclusive 模式获取锁，并在事务结束时释放它。LA      
 * 会在事务结束命令处获取 AccessShare 模式的锁，并          
 * 立即将                                                                  
 * 其释放。
 *
 * 上述示例的锁图看起来会如下所示：          
 * LA（正在等待获取事务锁）-> PA-2（由于唯一索引约束而    
 * 正在等待获取锁）-> PA-1（正在等待获取流锁）->            
 * LA         
 *
 * 这样，当 LA 为了保持提交顺序而等待执行完事务结束命    
 * 令时，就能够检测到死锁（若有）。             
 *
 * 有人可能认为我们可以使用 XactLockTableWait()，但 XactLo  
 * ckTableWait() 会将 PREPARED TRANSACTION 视为仍在进行，这      
 * 意味着即使并行应用工作进程预备该事务，锁也不会  
 * 被释放。
 *
 * 3) shm_mq 缓冲区已满时的死锁     
 *
 * 在前面的场景中（PA-1 与 PA-2 并发执行事务），如果  
 *  LA 与 PA-2 之间的 shm_mq 缓冲区已满，就必须等待才能发  
 * 送消息，而这种等待不会出现在 lmgr 中。      
 *
 * 为了避免这种等待，我们使用非阻塞写入并带超时等待。
 * 如果超时，LA 会把所有待处理的消息序列化到一个文件  
 * 中，并告知 PA-2 它需要读取该文件来获取剩余的消息。  
 * 然后 LA 会像前一情况那样等待提交，进而检测出可能 
 * 存在的死锁。参见 pa_send_data() 和枚举        
 *  TransApplyAction。  
 *
 * 锁类型 
 * ----------
 * 上述流锁和事务锁都是会话级锁，因为它们可能  
 * 在事务之外获取，且领导者中的流锁需跨事务边界  
 * 存在，即直到流式事务                                        
 * 结束。                                                              
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#define PG_LOGICAL_APPLY_SHM_MAGIC 0x787ca067

/*
 * 并行应用工作进程的 DSM 键。与其他并行代码不同，     
 * 由于不必担心 DSM 键与 plan_node_id 相冲突，所以我们         
 * 即可用小整数。  
 */
#define PARALLEL_APPLY_KEY_SHARED		1
#define PARALLEL_APPLY_KEY_MQ			2
#define PARALLEL_APPLY_KEY_ERROR_QUEUE	3

/* DSM 队列大小暂定为 16 MB。*/
#define DSM_QUEUE_SIZE	(16 * 1024 * 1024)

/*
 * DSM 的错误队列大小。它最好足够大，让一条典型的    
 * ErrorResponse 无需阻塞即可发送。这样，因出错而退出的    
 * 工作进程可以将整条消息写入队列后终止，不必            
 * 等待用户后端。        
 */
#define DSM_ERROR_QUEUE_SIZE			(16 * 1024)

/*
 * 并行应用工作进程收到的每条消息都包含三个字段：
 * start_lsn、end_lsn 和 send_time。由于这些统计信息已        
 * 在领导者应用工作进程中更新，因此并行应用工作进程
 * 可忽略这些字段（见 LogicalRepApplyLoop）。    
 */
#define SIZE_STATS_MESSAGE (2 * sizeof(XLogRecPtr) + sizeof(TimestampTz))

/*
 * 对于在逻辑复制订阅者上正被应用的事务，其会话级锁  
 * 的类型。           
 */
#define PARALLEL_APPLY_LOCK_STREAM	0
#define PARALLEL_APPLY_LOCK_XACT	1

/*
 * xid 到并行应用工作进程状态的哈希表条目。     
 */
typedef struct ParallelApplyWorkerEntry
{
	TransactionId xid;			/* 哈希键须居首。     */
	ParallelApplyWorkerInfo *winfo;
} ParallelApplyWorkerEntry;

/*
 * 一张哈希表，用于缓存并行应用工作进程正在应用的       
 * 流式事务的状态。      
 */
static HTAB *ParallelApplyTxnHash = NULL;

/*
* 活动并行应用工作进程的列表（池）。新工作进程 
* 的信息将在成功启动后加入该列表。若在事务结束时工
* 作进程池中已有足够多的工作进程，则移除对应的列  
* 表条目。有关工作进程池的更多信息，请参见本文件    
* 顶部的注释。                
 */
static List *ParallelApplyWorkerPool = NIL;

/*
 * 领导者应用工作进程与并行应用工作进程间共享信息。 
 */
ParallelApplyWorkerShared *MyParallelShared = NULL;

/*
 * 并行应用工作进程是否发送了领导者应用工作进程需要
 * 接收的消息？      
 */
volatile sig_atomic_t ParallelApplyMessagePending = false;

/*
 * 缓存并行应用工作进程信息，即应用当前流式事务所
 * 需的信息。它用于节省在 STREAM_START 与 STREAM_STOP             
 *  之间应用变更时查找哈希表的开销。                         
 */
static ParallelApplyWorkerInfo *stream_apply_worker = NULL;

/* 用于维护子事务（若有）列表。  */
static List *subxactlist = NIL;

static void pa_free_worker_info(ParallelApplyWorkerInfo *winfo);
static ParallelTransState pa_get_xact_state(ParallelApplyWorkerShared *wshared);
static PartialFileSetState pa_get_fileset_state(void);

/*
 * 若可以启动并行应用工作进程则返回 true，否则为 false。 
 */
static bool
pa_can_start(void)
{
	/* 只有领导者才能启动并行应用工作进程。      */
	if (!am_leader_apply_worker())
		return false;

	/*
	 * 最好检查订阅参数有无变更，以免变更长期得       
	 * 不到反映。当持续不断地出现流式事务、且       
	 * 它们由并行应用工作进程处理时，就可能发            
	 * 生这种情况。                                      
	 *
	 * 最好在下面的检查之前完成这项检查，以便检查时能  
	 * 使用订阅的最新值。                
	 */
	maybe_reread_subscription();

	/*
	 * 若订阅未采用并行流式模式，或发布者不支持并   
	 * 行应用，则不要启动新的并行应用                      
	 * 工作进程。
	 */
	if (!MyLogicalRepWorker->parallel_apply)
		return false;

	/*
	 * 若用户设置了 skiplsn，则不要启动并行工作进程，
	 * 因为用户可能想跳过该流式事务。                 
	 * 对于流式事务，要将事务序列化到文件，以便在开始 
	 * 应用变更之前拿到该事务的最后一个 LSN，以判断是否
	 * 要跳过。                             
	 *
	 * 有人可能认为，若事务的第一个 LSN 大于 skiplsn，就    
	 * 可以允许并行。但我们并不随 STREAM START 消息一    
	 * 起发送它，而且为了这种情形而随 STREAM START 额外      
	 * 发送八个字节来启用并行，似乎并不值得。       
	 */
	if (!XLogRecPtrIsInvalid(MySubscription->skiplsn))
		return false;

	/*
	 * 对于正在用并行应用工作进程应用的流式事务，我
	 * 们无法决定是否应用不在 READY 状态的关系的变更
	 * （见 should_apply_changes_for_rel），   
	 * 因为到那时我们还不知道 remote_final_lsn。所以在         
	 * 这种情况下不会启动新的并行应用工作进程。        
	 */
	if (!AllTablesyncsReady())
		return false;

	return true;
}

/*
 * 建立一个动态共享内存段。   
 *
 * 建立控制区域，内含固定大小的工作进程信息（  
 * ParallelApplyWorkerShared）、消息队列和错误队列。    
 *
 * 成功返回 true，失败返回 false。 
 */
static bool
pa_setup_dsm(ParallelApplyWorkerInfo *winfo)
{
	shm_toc_estimator e;
	Size		segsize;
	dsm_segment *seg;
	shm_toc    *toc;
	ParallelApplyWorkerShared *shared;
	shm_mq	   *mq;
	Size		queue_size = DSM_QUEUE_SIZE;
	Size		error_queue_size = DSM_ERROR_QUEUE_SIZE;

	/*
	 * 估算我们需要多少共享内存。 
	 *
	 * 由于 TOC 机制可能为大小古怪的请求插入填充，我们 
	 * 必须分别估算每个块的大小。          
	 *
	 * 需要一个键来注册头部的位置，另需两个键来跟踪   
	 * 消息队列与错误消息队列的位                               
	 * 置。
	 */
	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, sizeof(ParallelApplyWorkerShared));
	shm_toc_estimate_chunk(&e, queue_size);
	shm_toc_estimate_chunk(&e, error_queue_size);

	shm_toc_estimate_keys(&e, 3);
	segsize = shm_toc_estimate(&e);

	/* 创建共享内存段并建立目录表。                          */
	seg = dsm_create(shm_toc_estimate(&e), 0);
	if (!seg)
		return false;

	toc = shm_toc_create(PG_LOGICAL_APPLY_SHM_MAGIC, dsm_segment_address(seg),
						 segsize);

	/* 设置头部区域。     */
	shared = shm_toc_allocate(toc, sizeof(ParallelApplyWorkerShared));
	SpinLockInit(&shared->mutex);

	shared->xact_state = PARALLEL_TRANS_UNKNOWN;
	pg_atomic_init_u32(&(shared->pending_stream_count), 0);
	shared->last_commit_end = InvalidXLogRecPtr;
	shared->fileset_state = FS_EMPTY;

	shm_toc_insert(toc, PARALLEL_APPLY_KEY_SHARED, shared);

	/* 为工作进程设置消息队列。 */
	mq = shm_mq_create(shm_toc_allocate(toc, queue_size), queue_size);
	shm_toc_insert(toc, PARALLEL_APPLY_KEY_MQ, mq);
	shm_mq_set_sender(mq, MyProc);

	/* 附加该队列。*/
	winfo->mq_handle = shm_mq_attach(mq, seg, NULL);

	/* 设置工作进程错误队列。  */
	mq = shm_mq_create(shm_toc_allocate(toc, error_queue_size),
					   error_queue_size);
	shm_toc_insert(toc, PARALLEL_APPLY_KEY_ERROR_QUEUE, mq);
	shm_mq_set_receiver(mq, MyProc);

	/* 附加该队列。*/
	winfo->error_mq_handle = shm_mq_attach(mq, seg, NULL);

	/* 结果返回调用者。  */
	winfo->dsm_seg = seg;
	winfo->shared = shared;

	return true;
}

/*
 * 尝试从工作进程池中获取并行应用工作进程，若            
 * 无则启动。 
 */
static ParallelApplyWorkerInfo *
pa_launch_parallel_worker(void)
{
	MemoryContext oldcontext;
	bool		launched;
	ParallelApplyWorkerInfo *winfo;
	ListCell   *lc;

	/* 尝试从工作进程池获取可用的并行应用工作进程。  */
	foreach(lc, ParallelApplyWorkerPool)
	{
		winfo = (ParallelApplyWorkerInfo *) lfirst(lc);

		if (!winfo->in_use)
			return winfo;
	}

	/*
	 * 新建并行应用工作进程。 
	 *
	 * 工作进程信息可在工作进程存续期间使用，因此要    
	 * 在永久上下文中创建它。
	 */
	oldcontext = MemoryContextSwitchTo(ApplyContext);

	winfo = (ParallelApplyWorkerInfo *) palloc0(sizeof(ParallelApplyWorkerInfo));

	/* 设置共享内存。*/
	if (!pa_setup_dsm(winfo))
	{
		MemoryContextSwitchTo(oldcontext);
		pfree(winfo);
		return NULL;
	}

	launched = logicalrep_worker_launch(MyLogicalRepWorker->dbid,
										MySubscription->oid,
										MySubscription->name,
										MyLogicalRepWorker->userid,
										InvalidOid,
										dsm_segment_handle(winfo->dsm_seg));

	if (launched)
	{
		ParallelApplyWorkerPool = lappend(ParallelApplyWorkerPool, winfo);
	}
	else
	{
		pa_free_worker_info(winfo);
		winfo = NULL;
	}

	MemoryContextSwitchTo(oldcontext);

	return winfo;
}

/*
 * 为一个指定的 xid 分配将要使用的并行应用工作进程。  
 *
 * 我们首先尝试从池中获取可用的工作进程（若有），然  
 * 后再尝试启动新的工作进程。成功后，将工作进程   
 * 信息记入哈希表，以便日后处理流式变更时能够找          
 * 到它。         
 */
void
pa_allocate_worker(TransactionId xid)
{
	bool		found;
	ParallelApplyWorkerInfo *winfo = NULL;
	ParallelApplyWorkerEntry *entry;

	if (!pa_can_start())
		return;

	winfo = pa_launch_parallel_worker();
	if (!winfo)
		return;

	/* 首次进入时，初始化并行应用工作进程状态哈希表。 */
	if (!ParallelApplyTxnHash)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(TransactionId);
		ctl.entrysize = sizeof(ParallelApplyWorkerEntry);
		ctl.hcxt = ApplyContext;

		ParallelApplyTxnHash = hash_create("logical replication parallel apply workers hash",
										   16, &ctl,
										   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/* 为所请求的事务创建一个条目。     */
	entry = hash_search(ParallelApplyTxnHash, &xid, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "hash table corrupted");

	/* 更新共享内存中的事务信息。              */
	SpinLockAcquire(&winfo->shared->mutex);
	winfo->shared->xact_state = PARALLEL_TRANS_UNKNOWN;
	winfo->shared->xid = xid;
	SpinLockRelease(&winfo->shared->mutex);

	winfo->in_use = true;
	winfo->serialize_changes = false;
	entry->winfo = winfo;
	entry->xid = xid;
}

/*
 * 查找给定事务所分配的工作进程（若有）。  
 */
ParallelApplyWorkerInfo *
pa_find_worker(TransactionId xid)
{
	bool		found;
	ParallelApplyWorkerEntry *entry;

	if (!TransactionIdIsValid(xid))
		return NULL;

	if (!ParallelApplyTxnHash)
		return NULL;

	/* 有效时返回缓存并行应用工作进程。  */
	if (stream_apply_worker)
		return stream_apply_worker;

	/* 查找所请求事务对应的条目。      */
	entry = hash_search(ParallelApplyTxnHash, &xid, HASH_FIND, &found);
	if (found)
	{
		/* 工作进程必须尚未退出。 */
		Assert(entry->winfo->in_use);
		return entry->winfo;
	}

	return NULL;
}

/*
 * 使工作进程可供重用。       
 *
 * 这会把并行应用工作进程条目从哈希表中移除，使其不再
 * 能被使用。若池中的工作进程已足够多，则会停止该工作
 * 进程并释放对应信息。否则只将工作进程标记为可     
 * 供重用。        
 *
 * 有关工作进程池的更多信息，请参见本文件顶部注释。
 */
static void
pa_free_worker(ParallelApplyWorkerInfo *winfo)
{
	Assert(!am_parallel_apply_worker());
	Assert(winfo->in_use);
	Assert(pa_get_xact_state(winfo->shared) == PARALLEL_TRANS_FINISHED);

	if (!hash_search(ParallelApplyTxnHash, &winfo->shared->xid, HASH_REMOVE, NULL))
		elog(ERROR, "hash table corrupted");

	/*
	 * 若池中有足够多工作进程，则停止它。     
	 *
	 * XXX 另外，若领导者应用工作进程因发送超时序列化 
	 * 了部分事务数据，则也会停止该工作进程。            
	 * 因为消息可能只写入了一部分到队列中，且除了      
	 * 反复重发消息直到成功以外，没有其他清理队列的办 
	 * 法。与其尝试发送终将被序列化的数据，然后让并  
	 * 行应用工作进程处理这些虚假消息，还不如停止工  
	 * 作进程。                             
	 */
	if (winfo->serialize_changes ||
		list_length(ParallelApplyWorkerPool) >
		(max_parallel_apply_workers_per_subscription / 2))
	{
		logicalrep_pa_worker_stop(winfo);
		pa_free_worker_info(winfo);

		return;
	}

	winfo->in_use = false;
	winfo->serialize_changes = false;
}

/*
 * 释放并行应用工作进程信息并删除含有序列化变更  
 * 的文件（若有）。  
 */
static void
pa_free_worker_info(ParallelApplyWorkerInfo *winfo)
{
	Assert(winfo);

	if (winfo->mq_handle)
		shm_mq_detach(winfo->mq_handle);

	if (winfo->error_mq_handle)
		shm_mq_detach(winfo->error_mq_handle);

	/* 删除包含序列化变更的文件。   */
	if (winfo->serialize_changes)
		stream_cleanup_files(MyLogicalRepWorker->subid, winfo->shared->xid);

	if (winfo->dsm_seg)
		dsm_detach(winfo->dsm_seg);

	/* 从工作进程池移除。  */
	ParallelApplyWorkerPool = list_delete_ptr(ParallelApplyWorkerPool, winfo);

	pfree(winfo);
}

/*
 * 为所有并行应用工作进程分离错误队列。
 */
void
pa_detach_all_error_mq(void)
{
	ListCell   *lc;

	foreach(lc, ParallelApplyWorkerPool)
	{
		ParallelApplyWorkerInfo *winfo = (ParallelApplyWorkerInfo *) lfirst(lc);

		if (winfo->error_mq_handle)
		{
			shm_mq_detach(winfo->error_mq_handle);
			winfo->error_mq_handle = NULL;
		}
	}
}

/*
 * 检查是否存在待处理的暂存消息。   
 */
static bool
pa_has_spooled_message_pending()
{
	PartialFileSetState fileset_state;

	fileset_state = pa_get_fileset_state();

	return (fileset_state != FS_EMPTY);
}

/*
 * 领导者应用工作进程完成变更序列化到文件后，重放
 * 这些暂存消息。           
 *
 * 如果没有待处理的暂存消息则返回 false，否则返回 true。 
 */
static bool
pa_process_spooled_messages_if_required(void)
{
	PartialFileSetState fileset_state;

	fileset_state = pa_get_fileset_state();

	if (fileset_state == FS_EMPTY)
		return false;

	/*
	 * 若领导者应用工作进程正忙于序列化部分变更，立即  
	 * 获取流锁并等待领导工作进程完成序列化变更。     
	 * 否则，并行应用工作进程将没有机会接收                 
	 *  STREAM_STOP（也就无法获取流锁），直到领导者序列化
	 * 完所有变更，这可能导致无法检测到的               
	 * 死锁。
	 *
	 * 注意，领导工作进程完成变更序列化后，FileSet 状态
	 * 就可以是 FS_SERIALIZE_DONE。           
	 */
	if (fileset_state == FS_SERIALIZE_IN_PROGRESS)
	{
		pa_lock_stream(MyParallelShared->xid, AccessShareLock);
		pa_unlock_stream(MyParallelShared->xid, AccessShareLock);

		fileset_state = pa_get_fileset_state();
	}

	/*
	 * 在领导者将所有变更序列化到文件后，我们无法立即  
	 * 读取该文件，因为内存队列中可能还留有消息。而在
	 * 下次调用该函数时应用所有暂存消息，以确保内存  
	 * 队列中不再残留任何消                                        
	 * 息。
	 */
	if (fileset_state == FS_SERIALIZE_DONE)
	{
		pa_set_fileset_state(MyParallelShared, FS_READY);
	}
	else if (fileset_state == FS_READY)
	{
		apply_spooled_messages(&MyParallelShared->fileset,
							   MyParallelShared->xid,
							   InvalidXLogRecPtr);
		pa_set_fileset_state(MyParallelShared, FS_EMPTY);
	}

	return true;
}

/*
 * 并行应用工作进程主循环的中断处理器。   
 */
static void
ProcessParallelApplyInterrupts(void)
{
	CHECK_FOR_INTERRUPTS();

	if (ShutdownRequestPending)
	{
		ereport(LOG,
				(errmsg("logical replication parallel apply worker for subscription \"%s\" has finished",
						MySubscription->name)));

		proc_exit(0);
	}

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}
}

/* 并行应用 worker 主循环。 */
static void
LogicalParallelApplyLoop(shm_mq_handle *mqh)
{
	shm_mq_result shmq_res;
	ErrorContextCallback errcallback;
	MemoryContext oldcxt = CurrentMemoryContext;

	/*
	 * 初始化 ApplyMessageContext，在每条复制协议消息处理    
	 * 后清理它。  
	 */
	ApplyMessageContext = AllocSetContextCreate(ApplyContext,
												"ApplyMessageContext",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * 注册应用错误上下文回调。字段会在应用变更期间     
	 * 填写。
	 */
	errcallback.callback = apply_error_callback;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (;;)
	{
		void	   *data;
		Size		len;

		ProcessParallelApplyInterrupts();

		/* 确保我们将数据读入自己的内存上下文。  */
		MemoryContextSwitchTo(ApplyMessageContext);

		shmq_res = shm_mq_receive(mqh, &len, &data, true);

		if (shmq_res == SHM_MQ_SUCCESS)
		{
			StringInfoData s;
			int			c;

			if (len == 0)
				elog(ERROR, "invalid message length");

			s.cursor = 0;
			s.maxlen = -1;
			s.data = (char *) data;
			s.len = len;

			/*
			 * 领导者应用工作进程发给并行应用工作进程  
			 * 消息，其首字节只能是 'w'。  
			 */
			c = pq_getmsgbyte(&s);
			if (c != 'w')
				elog(ERROR, "unexpected message \"%c\"", c);

			/*
			 * 忽略领导者应用工作进程更新过的统计信息字 
			 * 段。       
			 *
			 * XXX 可以不从领导者应用工作进程发送统计信息 
			 * 字段，但为此它需要重建整条                  
			 * 消息并剔除这些字段，而这可能比在并行应用  
			 * 工作进程中忽略这些字段更省事。             
			 */
			s.cursor += SIZE_STATS_MESSAGE;

			apply_dispatch(&s);
		}
		else if (shmq_res == SHM_MQ_WOULD_BLOCK)
		{
			/* 重放文件中的变更（若有）。   */
			if (!pa_process_spooled_messages_if_required())
			{
				int			rc;

				/* 等更多工作。  */
				rc = WaitLatch(MyLatch,
							   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							   1000L,
							   WAIT_EVENT_LOGICAL_PARALLEL_APPLY_MAIN);

				if (rc & WL_LATCH_SET)
					ResetLatch(MyLatch);
			}
		}
		else
		{
			Assert(shmq_res == SHM_MQ_DETACHED);

			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("lost connection to the logical replication apply worker")));
		}

		MemoryContextReset(ApplyMessageContext);
		MemoryContextSwitchTo(oldcxt);
	}

	/* 弹出错误上下文栈。  */
	error_context_stack = errcallback.previous;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * 确保领导者应用工作进程会再尝试从我们的错误队列读取  
 * 一次。这可以防范这样一种情况：我们未发送              
 *  ErrorResponse 就非正常退出，例如某些代码直接调用了    
 *
 * 另外，显式地从 dsm 段脱离，以调用 on_dsm_detach 回调      
 * （若有）。详见 ParallelWorkerShutdown。
 */
static void
pa_shutdown(int code, Datum arg)
{
	SendProcSignal(MyLogicalRepWorker->leader_pid,
				   PROCSIG_PARALLEL_APPLY_MESSAGE,
				   InvalidBackendId);

	dsm_detach((dsm_segment *) DatumGetPointer(arg));
}

/*
 * 并行应用 worker 入口点。  
 */
void
ParallelApplyWorkerMain(Datum main_arg)
{
	ParallelApplyWorkerShared *shared;
	dsm_handle	handle;
	dsm_segment *seg;
	shm_toc    *toc;
	shm_mq	   *mq;
	shm_mq_handle *mqh;
	shm_mq_handle *error_mqh;
	RepOriginId originid;
	int			worker_slot = DatumGetInt32(main_arg);
	char		originname[NAMEDATALEN];

	InitializingApplyWorker = true;

	/*
	 * 设置信号处理。 
	 *
	 * 注意：我们有意用 SIGUSR2 触发优雅关闭，由领      
	 * 导应用工作进程发起。这有助于区分它和收到        
	 *  SIGTERM 时中止当前事务并退出的                      
	 * 情形。         
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();

	/*
	 * 附加到并行应用所用的动态共享内存段，并找到其目  
	 * 录表。                  
	 *
	 * 和并行查询一样，此时尚不需要资源所有者。参见 
	 * ParallelWorkerMain 
	 */
	memcpy(&handle, MyBgworkerEntry->bgw_extra, sizeof(dsm_handle));
	seg = dsm_attach(handle);
	if (!seg)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not map dynamic shared memory segment")));

	toc = shm_toc_attach(PG_LOGICAL_APPLY_SHM_MAGIC, dsm_segment_address(seg));
	if (!toc)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid magic number in dynamic shared memory segment")));

	/* 查找共享信息。           */
	shared = shm_toc_lookup(toc, PARALLEL_APPLY_KEY_SHARED, false);
	MyParallelShared = shared;

	/*
	 * 附加到消息队列。    
	 */
	mq = shm_toc_lookup(toc, PARALLEL_APPLY_KEY_MQ, false);
	shm_mq_set_receiver(mq, MyProc);
	mqh = shm_mq_attach(mq, seg, NULL);

	/*
	 * 主初始化已完成。现在可以附加到自己的槽位了。 
	 * 这样能确保领导者应用工作进程不会向未初始化的内
	 * 存队列中写入数据。    
	 */
	logicalrep_worker_attach(worker_slot);

	/*
	 * 附加到工作进程槽位后再注册关闭回调。这是为了
	 * 确保当此回调被调用时 MyLogicalRepWorker                      
	 *  仍然有效。    
	 */
	before_shmem_exit(pa_shutdown, PointerGetDatum(seg));

	SpinLockAcquire(&MyParallelShared->mutex);
	MyParallelShared->logicalrep_worker_generation = MyLogicalRepWorker->generation;
	MyParallelShared->logicalrep_worker_slot_no = worker_slot;
	SpinLockRelease(&MyParallelShared->mutex);

	/*
	 * 附加到错误队列。  
	 */
	mq = shm_toc_lookup(toc, PARALLEL_APPLY_KEY_ERROR_QUEUE, false);
	shm_mq_set_sender(mq, MyProc);
	error_mqh = shm_mq_attach(mq, seg, NULL);

	pq_redirect_to_shm_mq(seg, error_mqh);
	pq_set_parallel_leader(MyLogicalRepWorker->leader_pid,
						   InvalidBackendId);

	MyLogicalRepWorker->last_send_time = MyLogicalRepWorker->last_recv_time =
		MyLogicalRepWorker->reply_time = 0;

	InitializeApplyWorker();

	InitializingApplyWorker = false;

	/* 设置复制源跟踪。           */
	StartTransactionCommand();
	ReplicationOriginNameForLogicalRep(MySubscription->oid, InvalidOid,
									   originname, sizeof(originname));
	originid = replorigin_by_name(originname, false);

	/*
	 * 并行应用工作进程无需独占这个已由其领导进程      
	 * 获取的复制源。                                   
	 */
	replorigin_session_setup(originid, MyLogicalRepWorker->leader_pid);
	replorigin_session_origin = originid;
	CommitTransactionCommand();

	/*
	 * 为 syscache 设置回调，以便知道订阅关系状态何时发  
	 * 生变化。                    
	 */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONRELMAP,
								  invalidate_syncing_table_states,
								  (Datum) 0);

	set_apply_error_context_origin(originname);

	LogicalParallelApplyLoop(mqh);

	/*
	 * 并行应用工作进程无法到达此处，因为它只会          
	 * 在收到来自领导者的 SIGTERM 或 SIGUSR2、来自自身的   
	 *  SIGINT，或发生错误时才会停止。任何一种情况都不    
	 * 会让代码执行到此处。          
	 */
	Assert(false);
}

/*
 * 处理所收到的、表示有并行应用工作进程消息的中断。  
 *
 * 注意：此函数会在信号处理器内被调用！我们只能设置 
 * 标志，让下一次 CHECK_FOR_INTERRUPTS() 调用      
 * HandleParallelApplyMessages() 
 */
void
HandleParallelApplyMessageInterrupt(void)
{
	InterruptPending = true;
	ParallelApplyMessagePending = true;
	SetLatch(MyLatch);
}

/*
 * 处理从单个并行应用工作进程收到的一条协议消       
 * 息。 
 */
static void
HandleParallelApplyMessage(StringInfo msg)
{
	char		msgtype;

	msgtype = pq_getmsgbyte(msg);

	switch (msgtype)
	{
		case 'E':				/* ErrorResponse */
			{
				ErrorData	edata;

				/* 解析 ErrorResponse */
				pq_parse_errornotice(msg, &edata);

				/*
				 * 如需要，可添加一行上下文，表明这是  
				 * 由并行应用工作进程传来的消息，否则有     
				 * 时可能很难弄清实际发生了                     
				 * 什么。
				 */
				if (edata.context)
					edata.context = psprintf("%s\n%s", edata.context,
											 _("logical replication parallel apply worker"));
				else
					edata.context = pstrdup(_("logical replication parallel apply worker"));

				/*
				 * 其余上下文应使用错误上下文回调，即       
				 * LogicalRepApplyLoop() 中生效的那些。  
				 */
				error_context_stack = apply_error_context_stack;

				/*
				 * 实际的错误必然是由并行应用工作进程     
				 * 上报的。 
				 */
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("logical replication parallel apply worker exited due to error"),
						 errcontext("%s", edata.context)));
			}

			/*
			 * NoticeResponse、NotifyResponse 无需处理。   
			 * 因为逻辑复制工作进程不需要向客户端          
			 * 发送消息。                
			 */
		case 'N':
		case 'A':
			break;

		default:
			elog(ERROR, "unrecognized message type received from logical replication parallel apply worker: %c (message length %d bytes)",
				 msgtype, msg->len);
	}
}

/*
 * 处理从并行应用工作进程收到的所有排队协议消息。    
 */
void
HandleParallelApplyMessages(void)
{
	ListCell   *lc;
	MemoryContext oldcontext;

	static MemoryContext hpam_context = NULL;

	/*
	 * 此函数由 ProcessInterrupts() 调用；由于它调用的   
	 * 函数包含 CHECK_FOR_INTERRUPTS()，因此如果在执行期间      
	 * 再收到更多信号，就可能递归调用。递归进入是否安 
	 * 全尚不清楚，且即便安全似乎也无用处，所以在完成 
	 * 之前将阻塞中断。                                 
	 */
	HOLD_INTERRUPTS();

	/*
	 * 此外，CurrentMemoryContext 可能几乎指向任何位置。我  
	 * 们不想冒数据泄漏到长生命周期上下文的风险，因此
	 * 将在每次使用后重置的私有上下文中完成工作。  
	 */
	if (!hpam_context)			/* 第一次进入？  */
		hpam_context = AllocSetContextCreate(TopMemoryContext,
											 "HandleParallelApplyMessages",
											 ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(hpam_context);

	oldcontext = MemoryContextSwitchTo(hpam_context);

	ParallelApplyMessagePending = false;

	foreach(lc, ParallelApplyWorkerPool)
	{
		shm_mq_result res;
		Size		nbytes;
		void	   *data;
		ParallelApplyWorkerInfo *winfo = (ParallelApplyWorkerInfo *) lfirst(lc);

		/*
		 * 领导者会脱离错误队列并将其置为 NULL。准备停
		 * 止所有并行应用工作进程之前就这样做，所以我 
		 * 们无需再处理错误消息。参见   
		 * logicalrep_worker_detach 
		 */
		if (!winfo->error_mq_handle)
			continue;

		res = shm_mq_receive(winfo->error_mq_handle, &nbytes, &data, true);

		if (res == SHM_MQ_WOULD_BLOCK)
			continue;
		else if (res == SHM_MQ_SUCCESS)
		{
			StringInfoData msg;

			initStringInfo(&msg);
			appendBinaryStringInfo(&msg, data, nbytes);
			HandleParallelApplyMessage(&msg);
			pfree(msg.data);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("lost connection to the logical replication parallel apply worker")));
	}

	MemoryContextSwitchTo(oldcontext);

	/* 退出时不妨顺便清空该上下文。     */
	MemoryContextReset(hpam_context);

	RESUME_INTERRUPTS();
}

/*
 * 经共享内存队列把数据发给指定的并行应用工作进    
 * 程。
 *
 * 如果经共享内存发送数据的尝试超时，则返回 false，否则
 * 为true。
 */
bool
pa_send_data(ParallelApplyWorkerInfo *winfo, Size nbytes, const void *data)
{
	int			rc;
	shm_mq_result result;
	TimestampTz startTime = 0;

	Assert(!IsTransactionState());
	Assert(!winfo->serialize_changes);

	/*
	 * 对于 'immediate' 模式，我们不会尝试向并行工作进程发 
	 * 送数据，这主要用于测试。   
	 */
	if (unlikely(debug_logical_replication_streaming == DEBUG_LOGICAL_REP_STREAMING_IMMEDIATE))
		return false;

/*
 * 超时值定得有些随意，但测试表明，除非并行应用工作  
 * 程正在等待某个锁，或出现严重的资源紧缺，否则     
 * 它就足以用来发送消息。至于我们为何使用非阻塞的方   
 * 式来发送消息，请参见本文件顶部的注释。       
 */
#define SHM_SEND_RETRY_INTERVAL_MS 1000
#define SHM_SEND_TIMEOUT_MS		(10000 - SHM_SEND_RETRY_INTERVAL_MS)

	for (;;)
	{
		result = shm_mq_send(winfo->mq_handle, nbytes, data, true, true);

		if (result == SHM_MQ_SUCCESS)
			return true;
		else if (result == SHM_MQ_DETACHED)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("could not send data to shared-memory queue")));

		Assert(result == SHM_MQ_WOULD_BLOCK);

		/* 重试前先等待。 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   SHM_SEND_RETRY_INTERVAL_MS,
					   WAIT_EVENT_LOGICAL_APPLY_SEND_DATA);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (startTime == 0)
			startTime = GetCurrentTimestamp();
		else if (TimestampDifferenceExceeds(startTime, GetCurrentTimestamp(),
											SHM_SEND_TIMEOUT_MS))
			return false;
	}
}

/*
 * 为当前事务切换到 PARTIAL_SERIALIZE 模式——这意味着       
 * 该事务当前及后续的所有数据都将被序列化到文件中。  
 * 这是为了防止与其他并行应用工作进程间可能出现的
 * 死锁（参见本文件顶部的注释）。                        
 */
void
pa_switch_to_partial_serialize(ParallelApplyWorkerInfo *winfo,
							   bool stream_locked)
{
	ereport(LOG,
			(errmsg("logical replication apply worker will serialize the remaining changes of remote transaction %u to a file",
					winfo->shared->xid)));

	/*
	 * 并行应用工作进程可能因某些原因卡住（例如在等   
	 * 待某个后端的锁），所以停止直接向它发送数据，     
	 * 转而把数据序列化到文件中。           
	 */
	winfo->serialize_changes = true;

	/* 初始化流文件集。       */
	stream_start_internal(winfo->shared->xid, true);

	/*
	 * 若尚未持有，则获取流锁，以确保并行应用工作进程 
	 * 一直等待领导者释放流锁，直到                            
	 * 事务结束为止。      
	 */
	if (!stream_locked)
		pa_lock_stream(winfo->shared->xid, AccessExclusiveLock);

	pa_set_fileset_state(winfo->shared, FS_SERIALIZE_IN_PROGRESS);
}

/*
 * 等待直到并行应用工作进程的事务状态达到或超过给  
 * 定的 xact_state。          
 */
static void
pa_wait_for_xact_state(ParallelApplyWorkerInfo *winfo,
					   ParallelTransState xact_state)
{
	for (;;)
	{
		/*
		 * 若事务状态达到或超过给定 xact_state，则停       
		 * 止。     
		 */
		if (pa_get_xact_state(winfo->shared) >= xact_state)
			break;

		/* 等待信号唤醒。 */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10L,
						 WAIT_EVENT_LOGICAL_PARALLEL_APPLY_STATE_CHANGE);

		/* 重置锁存器，以免空转。 */
		ResetLatch(MyLatch);

		/* 等待期间可能已经发生了中断。            */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * 等待并行应用工作进程的事务结束。            
 */
static void
pa_wait_for_xact_finish(ParallelApplyWorkerInfo *winfo)
{
	/*
	 * 等待并行应用工作进程把状态设置为     
	 *  PARALLEL_TRANS_STARTED，这意味着它已获取了事务锁。 
	 * 这样是为了防止领导者应用工作进程比并行应用
	 * 工作进程更早地获取事务锁。                 
	 */
	pa_wait_for_xact_state(winfo, PARALLEL_TRANS_STARTED);

	/*
	 * 等待事务锁被释放。这样做才能检测出领导者与  
	 * 并行应用工作进程间的死锁。参见本文件顶部的注   
	 * 释。                  
	 */
	pa_lock_transaction(winfo->shared->xid, AccessShareLock);
	pa_unlock_transaction(winfo->shared->xid, AccessShareLock);

	/*
	 * 检查状态是否已变为 PARALLEL_TRANS_FINISHED，以防并行     
	 * 应用工作进程在应用变更时失败而使锁被释放的  
	 * 情况。
	 */
	if (pa_get_xact_state(winfo->shared) != PARALLEL_TRANS_FINISHED)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lost connection to the logical replication parallel apply worker")));
}

/*
 * 为给定的并行应用工作进程设置事务状态。   
 */
void
pa_set_xact_state(ParallelApplyWorkerShared *wshared,
				  ParallelTransState xact_state)
{
	SpinLockAcquire(&wshared->mutex);
	wshared->xact_state = xact_state;
	SpinLockRelease(&wshared->mutex);
}

/*
 * 获取给定的并行应用工作进程的事务状态。   
 */
static ParallelTransState
pa_get_xact_state(ParallelApplyWorkerShared *wshared)
{
	ParallelTransState xact_state;

	SpinLockAcquire(&wshared->mutex);
	xact_state = wshared->xact_state;
	SpinLockRelease(&wshared->mutex);

	return xact_state;
}

/*
 * 缓存并行应用工作进程信息。     
 */
void
pa_set_stream_apply_worker(ParallelApplyWorkerInfo *winfo)
{
	stream_apply_worker = winfo;
}

/*
 * 为流式事务构成一个唯一的保存点名称。     
 *
 * 注意，位于不同节点上的发布所对应的不同订阅可能收 
 * 到相同的远程 xid，因此还需要加上订阅 id。               
 *
 * 在给定的缓冲区中返回名称。 
 */
static void
pa_savepoint_name(Oid suboid, TransactionId xid, char *spname, Size szsp)
{
	snprintf(spname, szsp, "pg_sp_%u_%u", suboid, xid);
}

/*
 * 按需在并行应用工作进程中为子事务定义保存点。  
 *
 * 并行应用工作进程可检查新变更是否带有不同的     
 *  xid 来判断是否启动了新的子事务。如果是这样，就应    
 * 定义一个命名保存点，以便必要时能够回滚           
 * 到它。   
 */
void
pa_start_subtrans(TransactionId current_xid, TransactionId top_xid)
{
	if (current_xid != top_xid &&
		!list_member_xid(subxactlist, current_xid))
	{
		MemoryContext oldctx;
		char		spname[NAMEDATALEN];

		pa_savepoint_name(MySubscription->oid, current_xid,
						  spname, sizeof(spname));

		elog(DEBUG1, "defining savepoint %s in logical replication parallel apply worker", spname);

		/* 我们必须处于事务块中才能定义 SAVEPOINT。  */
		if (!IsTransactionBlock())
		{
			if (!IsTransactionState())
				StartTransactionCommand();

			BeginTransactionBlock();
			CommitTransactionCommand();
		}

		DefineSavepoint(spname);

		/*
		 * 在事务块内发出 SAVEPOINT 后，需要调用                
		 * CommitTransactionCommand 来启动子事务（见  
		 *  StartSubTransaction() 
		 */
		CommitTransactionCommand();

		oldctx = MemoryContextSwitchTo(TopTransactionContext);
		subxactlist = lappend_xid(subxactlist, current_xid);
		MemoryContextSwitchTo(oldctx);
	}
}

/* 重置用于维护子事务的列表。        */
void
pa_reset_subtrans(void)
{
	/*
	 * 无需显式地释放它，因为已分配的内存会在事务结束
	 * 时被释放。              
	 */
	subxactlist = NIL;
}

/*
 * 当事务由并行应用工作进程应用时，处理 STREAM ABORT       
 *  消息。   
 */
void
pa_stream_abort(LogicalRepStreamAbortData *abort_data)
{
	TransactionId xid = abort_data->xid;
	TransactionId subxid = abort_data->subxid;

	/*
	 * 更新复制源状态，以便崩溃后能从正确的位置恢复   
	 * 流式传输。  
	 */
	replorigin_session_origin_lsn = abort_data->abort_lsn;
	replorigin_session_origin_timestamp = abort_data->abort_time;

	/*
	 * 若两个 XID 相同，那就等于中止顶层事务，因此       
	 * 只需释放该列表。  
	 */
	if (subxid == xid)
	{
		pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);

		/*
		 * 之所以释放锁，是因为我们可能正在处理空的 
		 * 流式事务，此时锁不会在事务回滚              
		 * 期间被释放。   
		 *
		 * 注意，在事务中止前释放事务锁可行，因为即使  
		 * 应用工作进程因崩溃或其他原因死亡，这样的事务
		 * 仍会被视为已                                              
		 * 中止。          
		 */
		pa_unlock_transaction(xid, AccessExclusiveLock);

		AbortCurrentTransaction();

		if (IsTransactionBlock())
		{
			EndTransactionBlock(false);
			CommitTransactionCommand();
		}

		pa_reset_subtrans();

		pgstat_report_activity(STATE_IDLE, NULL);
	}
	else
	{
		/* 好吧，这是子事务。回滚到保存点。  */
		int			i;
		char		spname[NAMEDATALEN];

		pa_savepoint_name(MySubscription->oid, subxid, spname, sizeof(spname));

		elog(DEBUG1, "rolling back to savepoint %s in logical replication parallel apply worker", spname);

		/*
		 * 搜索子事务列表，确定其跟踪的偏移量，      
		 * 然后截断该列表。       
		 *
		 * 注意，对于空子事务，在这里找不到 subxid        
		 * 。  
		 */
		for (i = list_length(subxactlist) - 1; i >= 0; i--)
		{
			TransactionId xid_tmp = lfirst_xid(list_nth_cell(subxactlist, i));

			if (xid_tmp == subxid)
			{
				RollbackToSavepoint(spname);
				CommitTransactionCommand();
				subxactlist = list_truncate(subxactlist, i);
				break;
			}
		}
	}
}

/*
 * 为特定的并行应用工作进程设置文件集状态。一旦领导 
 * 者进程把所有变更序列化到文件后，文件集就会被设
 * 置，供并行应用工作进程使用。      
 */
void
pa_set_fileset_state(ParallelApplyWorkerShared *wshared,
					 PartialFileSetState fileset_state)
{
	SpinLockAcquire(&wshared->mutex);
	wshared->fileset_state = fileset_state;

	if (fileset_state == FS_SERIALIZE_DONE)
	{
		Assert(am_leader_apply_worker());
		Assert(MyLogicalRepWorker->stream_fileset);
		wshared->fileset = *MyLogicalRepWorker->stream_fileset;
	}

	SpinLockRelease(&wshared->mutex);
}

/*
 * 获取当前并行应用工作进程的文件集状态。   
 */
static PartialFileSetState
pa_get_fileset_state(void)
{
	PartialFileSetState fileset_state;

	Assert(am_parallel_apply_worker());

	SpinLockAcquire(&MyParallelShared->mutex);
	fileset_state = MyParallelShared->fileset_state;
	SpinLockRelease(&MyParallelShared->mutex);

	return fileset_state;
}

/*
 * 为每个流块获取和释放锁的辅助函数。                  
 *
 * 将 locktag_field4 设置为 PARALLEL_APPLY_LOCK_STREAM 以表明它是 
 * 流锁。   
 *
 * 至于流锁的使用方式，请参见本文件顶部的注释。      
 */
void
pa_lock_stream(TransactionId xid, LOCKMODE lockmode)
{
	LockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
								   PARALLEL_APPLY_LOCK_STREAM, lockmode);
}

void
pa_unlock_stream(TransactionId xid, LOCKMODE lockmode)
{
	UnlockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
									 PARALLEL_APPLY_LOCK_STREAM, lockmode);
}

/*
 * 这组辅助函数用于为每个本地事务的应用获取与释放    
 * 锁。
 *
 * 将 locktag_field4 设置为 PARALLEL_APPLY_LOCK_XACT，以表明是  
 * 事务锁。     
 *
 * 注意，所有调用者必须传远程事务 ID 而非本地事务 ID  
 *  作为 xid。这是因为本地事务 ID 只有在并行应用期间应  
 * 用第一个变更时才会被分配，但并行应用工作进程中的  
 * 第一个变更有可能被另一个并行应用工作进程中并发执   
 * 行的事务阻塞。我们只有在应用第一个变更之后才能把    
 * 本地事务 id 告知领导者，因此在发送事务结束             
 * 命令之后，它将无法使用这把锁                                
 * 来进行等待。      
 *
 * 至于事务锁的使用方式，请参见本文件顶部的注        
 * 。  
 */
void
pa_lock_transaction(TransactionId xid, LOCKMODE lockmode)
{
	LockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
								   PARALLEL_APPLY_LOCK_XACT, lockmode);
}

void
pa_unlock_transaction(TransactionId xid, LOCKMODE lockmode)
{
	UnlockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
									 PARALLEL_APPLY_LOCK_XACT, lockmode);
}

/*
 * 递减待处理的流块数量，如果没有可用的待处理块，则    
 * 在流锁上等待。                  
 */
void
pa_decr_and_wait_stream_block(void)
{
	Assert(am_parallel_apply_worker());

	/*
	 * 只有当我们在应用暂存消息时，才可能没有任何待处
	 * 理的流块。           
	 */
	if (pg_atomic_read_u32(&MyParallelShared->pending_stream_count) == 0)
	{
		if (pa_has_spooled_message_pending())
			return;

		elog(ERROR, "invalid pending streaming chunk 0");
	}

	if (pg_atomic_sub_fetch_u32(&MyParallelShared->pending_stream_count, 1) == 0)
	{
		pa_lock_stream(MyParallelShared->xid, AccessShareLock);
		pa_unlock_stream(MyParallelShared->xid, AccessShareLock);
	}
}

/*
 * 在领导者应用工作进程中完成流式事务的处理。        
 */
void
pa_xact_finish(ParallelApplyWorkerInfo *winfo, XLogRecPtr remote_lsn)
{
	Assert(am_leader_apply_worker());

	/*
	 * 解锁共享对象锁，让并行应用工作进程能继续   
	 * 接收并应用变更。              
	 */
	pa_unlock_stream(winfo->shared->xid, AccessExclusiveLock);

	/*
	 * 等待该工作进程完成。这是维护提交顺序所需，     
	 * 以便避免因事务依赖和死锁而导致                  
	 * 失败。 
	 */
	pa_wait_for_xact_finish(winfo);

	if (!XLogRecPtrIsInvalid(remote_lsn))
		store_flush_position(remote_lsn, winfo->shared->last_commit_end);

	pa_free_worker(winfo);
}
