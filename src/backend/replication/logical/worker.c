/*-------------------------------------------------------------------------
 * worker.c
 *	   PostgreSQL 逻辑复制应用工作进程    
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/worker.c
 *
 * NOTES
 *	  本文件包含将到达的逻辑变更付诸应用的工作进程。   
 *	  变更来自远端逻辑复制流。   
 *
 *	  主工作进程（apply）由逻辑复制工作进程启动器  
 *	  为数据库中每个启用的订阅所启动。它使用     
 *	  walsender 协议与发布者通信。            
 *
 *	  本模块包含面向服务器的代码，与 libpqwalreceiver     
 *	  模块共享，由其提供 libpq 相关功能。                      
 *
 *
 * 流式事务         
 * ---------------------
 * 流式事务（在上游超出内存限制的大事务）的应用采用 
 * 以下两种方式之一：                       
 *
 * 1) 先写入临时文件，待最终提交到达后再应用       
 *
 * 当用户将订阅的 streaming 选项设为                            
 * on 时。    
 *
 * 与常规（非流式）情形不同，流式事务的处理还必须     
 * 同时处理顶层事务与子事务的中止。这一点                  
 * 通过跟踪子事务的偏移量来实现，该偏移量随后用于  
 * 截断存放已序列化变更的文件。   
 *
 * 这些文件默认放在临时文件目录中，文件名同时包含   
 * 顶层事务的 XID 与订阅的 OID。                        
 * 之所以必须这样，是为了让处理同一 XID 远程事务的  
 * 不同工作进程互不干扰。                      
 *
 * 我们用 BufFile 而非普通临时文件，因为 (a)                
 * BufFile 支持超过 OS 文件大小上限的临时文件，                
 * (b) 提供出错时自动清理的途径，且 (c) 允许这些               
 * 文件跨本地事务存续，并允许在流开始与结束时打开和    
 * 关闭。我们决定使用 FileSet                       
 * 基础设施，因为没有它，文件会在关闭时被删除；       
 * 而如果让流文件在 start/stop stream 期间保持打开，            
 * 就会消耗大量内存（每个 BufFile 超过 8K，而且           
 * 可能存在多个这样的 BufFile，因为订阅者可能在       
 * 得到提交之前收到针对不同事务的多轮 start/stop stream， 
 * commit）。此外，若不使用 FileSet，还需要另行设计     
 * 一种把文件名传给 BufFile API 的新方法，以便能在针对    
 * 同一事务的多次流打开调用中，打开想要的那个   
 * 文件。   
 *
 * 2) 并行应用进程。  
 *
 * 当用户将订阅的 streaming 选项设为                            
 * parallel。详情参见 logical/applyparallelworker.c。                   
 * 此方式。  
 *
 * 两阶段事务       
 * ----------------------
 * 两阶段事务在 prepare 时重放，随后在 commit prepared    
 * 时提交或在 rollback prepared 时回滚。可能出现这样        
 * 的情况：已 prepare 的事务在应用工作进程正忙于         
 * 初始复制时到达。这种情况下，应用工作进程会在        
 * 表同步尚忙时跳过所有已 prepare 的操作（例如插入）。   
 * （条件见 should_apply_changes_for_rel）。表同步              
 * 工作进程可能收不到这样的已 prepare 事务，比如说它    
 * 在初始一致点之前，但可能已收到一些更晚的提交。  
 * 此时，表同步工作进程将直接退出，对被应用工作进程   
 * 跳过的 prepare 事务不做处理，因其同步位置早已             
 * 领先于应用工作进程的当前位置。这会导致一次              
 * "空 prepare"：因为之后当应用工作进程执行 commit            
 * prepare 时，其中已空无一物（插入早已被跳过）。  
 *
 * 为避免此类以及类似的 prepare 混乱，订阅的 two_phase       
 * 提交只在初始同步结束之后才启用。two_phase 选项           
 * 以三态实现，取值为 DISABLED、PENDING 和                    
 * ENABLED.
 *
 * 即使用户指定想要 two_phase = on 的订阅，                    
 * 内部也会从三态 PENDING 开始，它只有在所有表同步     
 * 初始化都完成后才会变为 ENABLED —— 也就是当所有      
 * 表同步工作进程都达到 READY 状态时。换言之，值           
 * PENDING 只是订阅启动期间的临时状态。          
 *
 * 在 two_phase 真正可用（ENABLED）之前，订阅的                
 * 行为就如同 two_phase = off。当应用工作进程检测到    
 * 所有表同步都已变为 READY（三态仍为 PENDING）时，     
 * 就会重启应用工作进程。这发生在    
 * process_syncing_tables_for_apply.
 *
 * 当（重启后的）应用工作进程发现所有表同步对于          
 * PENDING 的 two_phase 三态均为 READY，就会开始以         
 * two_phase 选项流式发送消息，这进而让发布者能够解码     
 * 两阶段提交。随后它把三态值从 PENDING 更新为                
 * ENABLED。现在，在我们尚未启用 two_phase 期间，               
 * 发布者（复制服务器）可能已跳过某些 prepare，但     
 * 我们确保这类 prepare 会随 commit prepare 一起发送， 
 * ReorderBufferFinishPrepared.
 *
 * 若订阅没有表，则 two_phase 三态 PENDING 保持               
 * 不变。这让用户仍然能够执行 ALTER SUBSCRIPTION               
 * REFRESH PUBLICATION（否则会被禁止，见下文）。  
 *
 * 如果用户需要了解三态值，可以从 pg_subscription             
 * 系统表中获取（参见列 subtwophasestate）。          
 *
 * 我们不允许切换订阅的 two_phase 选项，因为这会            
 * 导致副本不一致。设想最初它为 on，且我们已经收到      
 * 一些 prepare，然后将其关闭，那么提交时服务器会随    
 * 提交一起发送整个事务的数据。经过更多分析后，         
 * 我们可以允许把该选项从 off 改为 on，但不确定            
 * 仅此是否有用。      
 *
 * 最后，为避免前述问题因后续（尚未 READY）的表     
 * 同步再次出现，需要把 two_phase 选项从 'on' 切换              
 * 到 'off' 再改回 'on'），因此对以下命令有约束： 
 * ALTER SUBSCRIPTION REFRESH PUBLICATION。当 two_phase 三态             
 * 为 ENABLED 时不允许，除非 copy_data = false。            
 *
 * 对于确实存在的场景，同一 GID 的 prepare 可能不止      
 * 一次出现：在同一服务器上为发布定义了多个订阅，且 
 * 已 prepare 的事务对被这些订阅的表有操作。对于这类       
 * 情况，如果我们使用发布者发来的 GID，则只有一个      
 * prepare 会成功，其余会失败；此时服务器会把它们      
 * 再次发送。而如果用户为订阅者上的所有订阅都设置了 
 * synchronous_standby_names，为避免这种死锁，我们会            
 * 为每个 prepare 事务生成唯一的 GID（由订阅 oid      
 * 与该 prepare 事务的 xid 组成），对订阅者上的每个         
 * prepare 事务生成。       
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeModifyTable.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "postmaster/walwriter.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/logicallauncher.h"
#include "replication/logicalproto.h"
#include "replication/logicalrelation.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "rewrite/rewriteHandler.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/dynahash.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/usercontext.h"

#define NAPTIME_PER_CYCLE 1000	/* 两轮循环间的最大休眠 (1s)*/

typedef struct FlushPosition
{
	dlist_node	node;
	XLogRecPtr	local_end;
	XLogRecPtr	remote_end;
} FlushPosition;

static dlist_head lsn_mapping = DLIST_STATIC_INIT(lsn_mapping);

typedef struct ApplyExecutionData
{
	EState	   *estate;			/* 执行器状态，用于跟踪资源    */

	LogicalRepRelMapEntry *targetRel;	/* 复制目标关系     */
	ResultRelInfo *targetRelInfo;	/* 对应的 ResultRelInfo*/

	/* 目标关系已分区时使用以下字段：                  */
	ModifyTableState *mtstate;	/* 哑 ModifyTable 状态  */
	PartitionTupleRouting *proute;	/* 分区路由信息     */
} ApplyExecutionData;

/* 用于保存和恢复应用错误上下文信息的结构体 */
typedef struct ApplyErrorCallbackArg
{
	LogicalRepMsgType command;	/* 无效为 0  */
	LogicalRepRelMapEntry *rel;

	/* 远端节点信息      */
	int			remote_attnum;	/* 无效为 -1  */
	TransactionId remote_xid;
	XLogRecPtr	finish_lsn;
	char	   *origin_name;
} ApplyErrorCallbackArg;

/*
 * 对事务中的变更要采取的行动。                
 *
 * TRANS_LEADER_APPLY:
 * 该行动表示我们处于领导者应用工作进程或表同步    
 * 工作进程。事务变更要么被直接应用，要么从临时   
 * 文件读出（流式事务情形），随后再由工作进程    
 * 应用。             
 *
 * TRANS_LEADER_SERIALIZE:
 * 该行动表示我们处于领导者应用工作进程或表同步    
 * 工作进程。变更写入临时文件，然后在最终提交到达   
 * 时才应用。      
 *
 * TRANS_LEADER_SEND_TO_PARALLEL:
 * 该行动表示我们处于领导者应用工作进程，需要把变更 
 * 发送给并行应用工作进程。     
 *
 * TRANS_LEADER_PARTIAL_SERIALIZE:
 * 该行动表示我们处于领导者应用工作进程，且已把部分   
 * 变更直接发送给并行应用工作进程，其余变更则因         
 * 发送数据超时而被序列化到文件。并行应用工作进程      
 * 会在最终提交到达时应用这些已序列化的变更。          
 *
 * 这种情形无法使用 TRANS_LEADER_SERIALIZE，因为除              
 * 了序列化变更之外，领导工作进程还需要把         
 * STREAM_XXX 消息序列化到文件，并在处理事务结束         
 * 命令时等待并行应用工作进程完成事务。因此引入       
 * 这一新行动，以保持代码和逻辑清晰。             
 *
 * TRANS_PARALLEL_APPLY:
 * 该行动表示我们处于并行应用工作进程，事务的变更    
 * 由该工作进程直接应用。                  
 */
typedef enum
{
	/* 非流式事务的行动。                */
	TRANS_LEADER_APPLY,

	/* 流式事务的行动。            */
	TRANS_LEADER_SERIALIZE,
	TRANS_LEADER_SEND_TO_PARALLEL,
	TRANS_LEADER_PARTIAL_SERIALIZE,
	TRANS_PARALLEL_APPLY
} TransApplyAction;

/* 错误上下文    */
ApplyErrorCallbackArg apply_error_callback_arg =
{
	.command = 0,
	.rel = NULL,
	.remote_attnum = -1,
	.remote_xid = InvalidTransactionId,
	.finish_lsn = InvalidXLogRecPtr,
	.origin_name = NULL,
};

ErrorContextCallback *apply_error_context_stack = NULL;

MemoryContext ApplyMessageContext = NULL;
MemoryContext ApplyContext = NULL;

/* 流式事务的每个流上下文             */
static MemoryContext LogicalStreamingContext = NULL;

WalReceiverConn *LogRepWorkerWalRcvConn = NULL;

Subscription *MySubscription = NULL;
static bool MySubscriptionValid = false;

static List *on_commit_wakeup_workers_subids = NIL;

bool		in_remote_transaction = false;
static XLogRecPtr remote_final_lsn = InvalidXLogRecPtr;

/* 仅在处理流式事务时有效的字段             */
static bool in_streamed_transaction = false;

static TransactionId stream_xid = InvalidTransactionId;

/*
 * 并行应用工作进程在一个流式块期间应用的变更数量      
 * 。   
 */
static uint32 parallel_stream_nchanges = 0;

/* 初始化应用工作进程中？   */
bool		InitializingApplyWorker = false;

/*
 * 如果远程事务的结束 LSN 与 subskiplsn 相匹配，我们          
 * 就为该订阅启用跳过所有数据修改变更（INSERT、UPDATE 等）。
 * 一旦开始跳过变更，在跳过该事务的全部变更之前           
 * 我们不会停止，即使 pg_subscription 被更新且                      
 * MySubscription->skiplsn 在此期间被修改或重置。                                
 * 此外，在流式事务情形（streaming = on）下，我们不              
 * 跳过接收与溢写变更，因为要等到开始应用变更时            
 * 才决定是否跳过应用变更。subskiplsn 会在成功跳过        
 * 该事务或应用了非空事务后被清除。后者可防止          
 * 误设的 subskiplsn 残留。注意：使用并行应用工作进程     
 * 时无法跳过流式事务，因为在应用变更之前拿不到         
 * 结束 LSN；用户设置它时便不会启动并行应用工作进程      
 * 。         
 */
static XLogRecPtr skip_xact_finish_lsn = InvalidXLogRecPtr;
#define is_skipping_changes() (unlikely(!XLogRecPtrIsInvalid(skip_xact_finish_lsn)))

/* 当前流式文件的 BufFile 句柄         */
static BufFile *stream_fd = NULL;

typedef struct SubXactInfo
{
	TransactionId xid;			/* 子事务的 XID   */
	int			fileno;			/* buffile 中的文件编号 */
	off_t		offset;			/* 文件内偏移量 */
} SubXactInfo;

/* 当前流式事务的子事务数据                       */
typedef struct ApplySubXactData
{
	uint32		nsubxacts;		/* 子事务的数量         */
	uint32		nsubxacts_max;	/* 子事务的当前容量     */
	TransactionId subxact_last; /* 最后一个子事务的 xid    */
	SubXactInfo *subxacts;		/* 子事务变更文件偏移量  */
} ApplySubXactData;

static ApplySubXactData subxact_data = {0, 0, InvalidTransactionId, NULL};

static inline void subxact_filename(char *path, Oid subid, TransactionId xid);
static inline void changes_filename(char *path, Oid subid, TransactionId xid);

/*
 * 给定顶层事务的各子事务的信息。                     
 */
static void subxact_info_write(Oid subid, TransactionId xid);
static void subxact_info_read(Oid subid, TransactionId xid);
static void subxact_info_add(TransactionId xid);
static inline void cleanup_subxact_info(void);

/*
 * 对顶层事务的变更进行序列化与反序列化。    
 */
static void stream_open_file(Oid subid, TransactionId xid,
							 bool first_segment);
static void stream_write_change(char action, StringInfo s);
static void stream_open_and_write_change(TransactionId xid, char action, StringInfo s);
static void stream_close_file(void);

static void send_feedback(XLogRecPtr recvpos, bool force, bool requestReply);

static void DisableSubscriptionAndExit(void);

static void apply_handle_commit_internal(LogicalRepCommitData *commit_data);
static void apply_handle_insert_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot);
static void apply_handle_update_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot,
										 LogicalRepTupleData *newtup,
										 Oid localindexoid);
static void apply_handle_delete_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot,
										 Oid localindexoid);
static bool FindReplTupleInLocalRel(ApplyExecutionData *edata, Relation localrel,
									LogicalRepRelation *remoterel,
									Oid localidxoid,
									TupleTableSlot *remoteslot,
									TupleTableSlot **localslot);
static void apply_handle_tuple_routing(ApplyExecutionData *edata,
									   TupleTableSlot *remoteslot,
									   LogicalRepTupleData *newtup,
									   CmdType operation);

/* 为两阶段事务计算 GID           */
static void TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid, int szgid);

/* 用于跳过变更的函数    */
static void maybe_start_skipping_changes(XLogRecPtr finish_lsn);
static void stop_skipping_changes(void);
static void clear_subscription_skip_lsn(XLogRecPtr finish_lsn);

/* 应用错误回调用的函数     */
static inline void set_apply_error_context_xact(TransactionId xid, XLogRecPtr lsn);
static inline void reset_apply_error_context_info(void);

static TransApplyAction get_transaction_apply_action(TransactionId xid,
													 ParallelApplyWorkerInfo **winfo);

static void replorigin_reset(int code, Datum arg);

/*
 * 构成该订阅的复制源名称。      
 *
 * 这是表同步工作进程与其他工作进程的共用函数。表同步 
 * 工作进程必须传入有效的 relid。其他调用者必须传  
 *
 * 在给定缓冲区中返回名称。   
 */
void
ReplicationOriginNameForLogicalRep(Oid suboid, Oid relid,
								   char *originname, Size szoriginname)
{
	if (OidIsValid(relid))
	{
		/* 表同步工作进程的复制源名称。     */
		snprintf(originname, szoriginname, "pg_%u_%u", suboid, relid);
	}
	else
	{
		/* 非表同步工作进程的复制源名称。      */
		snprintf(originname, szoriginname, "pg_%u", suboid);
	}
}

/*
 * 此工作进程是否应用给定关系的变更。 
 *
 * 这主要是初始关系数据同步所需，因为同步运行在  
 * 单独的并行工作进程中，我们需要某种办法来跳过      
 * 表同步期间到达领导者应用工作进程的变更。         
 *
 * 注意：对 SYNCDONE 状态必须用小于等于比较，因为          
 * 它持有的位置可能是初始槽一致点 WAL 记录末尾    
 * +1（即下一条记录的起点），而下一条记录可能       
 * 正是当前正在处理的事务的 COMMIT；remote_final_lsn 设于   
 * apply_handle_begin。     
 *
 * 注意：对于正由并行应用工作进程应用的流式事务，      
 * 如果订阅中的目标表未处于 READY 状态，则不允许     
 * 应用变更，因为届时还不知道 remote_final_lsn，                
 * 因此无法决定是否应用该变更。                      
 *
 * 分配流式事务给并行工作进程前，我们已在     
 * pa_can_start() 中检查过这一点，但这里也需要再次      
 * 检查：因为如果用户同时执行 ALTER SUBSCRIPTION ...          
 * REFRESH PUBLICATION，在应用该事务期间，新表就可能          
 * 加入 pg_subscription_rel。   
 */
static bool
should_apply_changes_for_rel(LogicalRepRelMapEntry *rel)
{
	if (am_tablesync_worker())
		return MyLogicalRepWorker->relid == rel->localreloid;
	else if (am_parallel_apply_worker())
	{
		/* 我们不同步处于未知状态的关系。         */
		if (rel->state != SUBREL_STATE_READY &&
			rel->state != SUBREL_STATE_UNKNOWN)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication parallel apply worker for subscription \"%s\" will stop",
							MySubscription->name),
					 errdetail("Cannot handle streamed replication transactions using parallel apply workers until all tables have been synchronized.")));

		return rel->state == SUBREL_STATE_READY;
	}
	else
		return (rel->state == SUBREL_STATE_READY ||
				(rel->state == SUBREL_STATE_SYNCDONE &&
				 rel->statelsn <= remote_final_lsn));
}

/*
 * 开始复制事务的一步（一条 INSERT、UPDATE 等）。        
 *
 * 若这是第一步则开启一个事务（否则继续沿用现有    
 * 的事务）。       
 * 同时提供全局快照，并确保运行于 ApplyMessageContext。    
 */
static void
begin_replication_step(void)
{
	SetCurrentStatementStartTimestamp();

	if (!IsTransactionState())
	{
		StartTransactionCommand();
		maybe_reread_subscription();
	}

	PushActiveSnapshot(GetTransactionSnapshot());

	MemoryContextSwitchTo(ApplyMessageContext);
}

/*
 * 完成复制事务的一步。                  
 * begin_replication_step() 的调用者也须调用。    
 *
 * 这里并不结束事务，但应递增命令计数器，       
 * 以使本步骤的效果可见。                            
 */
static void
end_replication_step(void)
{
	PopActiveSnapshot();

	CommandCounterIncrement();
}

/*
 * 为领导者应用工作进程与并行应用工作进程两者处理
 * 流式事务。        
 *
 * 流式情形（收到流式事务的一个块）下：序列化模式     
 * 将其重定向到对应顶层事务的文件；并行模式        
 * 下，领导者应用工作进程会把变更发送给并行应用工作 
 * 进程，并行应用工作进程会按需定义保存点。               
 * （LOGICAL_REP_MSG_RELATION 或 LOGICAL_REP_MSG_TYPE                   
 * 消息由领导者应用工作进程与并行应用工作进程应用  
 * ）。   
 *
 * 对流式事务返回 true（变更要么已序列化到文件，           
 * 要么已发送给并行应用工作进程），否则返回 false         
 * （常规模式或需并行应用处理）。   
 *
 * 例外：若正在处理的消息是 LOGICAL_REP_MSG_RELATION        
 * 或 LOGICAL_REP_MSG_TYPE，即使消息要发给并行应用工作进程  
 * ，也返回 false。      
 */
static bool
handle_streamed_transaction(LogicalRepMsgType action, StringInfo s)
{
	TransactionId current_xid;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;
	StringInfoData original_msg;

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	/* 非流式模式       */
	if (apply_action == TRANS_LEADER_APPLY)
		return false;

	Assert(TransactionIdIsValid(stream_xid));

	/*
	 * 并行应用工作进程需要本消息中的 xid 来决定      
	 * 是否定义保存点，因此要保存一份游标尚未越过     
	 * xid 的原始消息。在 PARTIAL_SERIALIZE 模式下，               
	 * 它将被序列化到文件。 
	 */
	original_msg = *s;

	/*
	 * 子事务的 XID 应已作为消息的第一部分收到，        
	 * 将其提取出来。  
	 */
	current_xid = pq_getmsgint(s, 4);

	if (!TransactionIdIsValid(current_xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid transaction ID in streamed replication transaction")));

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:
			Assert(stream_fd);

			/* 把新子事务加入数组（除非已存在）。      */
			subxact_info_add(current_xid);

			/* 把该变更写入当前文件       */
			stream_write_change(action, s);
			return true;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * XXX 发布者不一定在流式事务后发送关系/类型    
			 * 更新消息；领导者应用工作进程的关系/类型  
			 * 亦要更新。参见函数                       
			 * cleanup_rel_sync_cache.
			 */
			if (pa_send_data(winfo, s->len, s->data))
				return (action != LOGICAL_REP_MSG_RELATION &&
						action != LOGICAL_REP_MSG_TYPE);

			/*
			 * 无法把变更发送给并行应用工作进程时，   
			 * 切换到序列化模式。     
			 */
			pa_switch_to_partial_serialize(winfo, false);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			stream_write_change(action, &original_msg);

			/* 理由同 TRANS_LEADER_SEND_TO_PARALLEL 情形。  */
			return (action != LOGICAL_REP_MSG_RELATION &&
					action != LOGICAL_REP_MSG_TYPE);

		case TRANS_PARALLEL_APPLY:
			parallel_stream_nchanges += 1;

			/* 按需为子事务定义保存点。        */
			pa_start_subtrans(current_xid, stream_xid);
			return false;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			return false;		/* 消除编译器警告    */
	}
}

/*
 * 为求值约束表达式、索引和触发器准备指定关系的  
 * 执行器状态。                              
 *
 * 注意调用者必须自行打开并关闭所有待更新索引。 
 */
static ApplyExecutionData *
create_edata_for_relation(LogicalRepRelMapEntry *rel)
{
	ApplyExecutionData *edata;
	EState	   *estate;
	RangeTblEntry *rte;
	List	   *perminfos = NIL;
	ResultRelInfo *resultRelInfo;

	edata = (ApplyExecutionData *) palloc0(sizeof(ApplyExecutionData));
	edata->targetRel = rel;

	edata->estate = estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel->localrel);
	rte->relkind = rel->localrel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;

	addRTEPermissionInfo(&perminfos, rte);

	ExecInitRangeTable(estate, list_make1(rte), perminfos);

	edata->targetRelInfo = resultRelInfo = makeNode(ResultRelInfo);

	/*
	 * 用 logicalrep_rel_open() 打开过的 Relation，而非再打开 
	 * 。   
	 */
	InitResultRelInfo(resultRelInfo, rel->localrel, 1, NULL, 0);

	/*
	 * 我们把 ResultRelInfo 放入 es_opened_result_relations 列表，  
	 * 尽管并不填充 es_result_relations 数组。这                  
	 * 有点不合常规，但足以让 ExecGetTriggerResultRel() 找到 
	 *
	 * 这里同样不调用 ExecOpenIndices()，由各执行应用操作    
	 * 的路径自己负责此事。                
	 */
	estate->es_opened_result_relations =
		lappend(estate->es_opened_result_relations, resultRelInfo);

	estate->es_output_cid = GetCurrentCommandId(true);

	/* 准备捕获 AFTER 触发器。  */
	AfterTriggerBeginQuery();

	/* edata 的其余字段暂为 NULL          */

	return edata;
}

/*
 * 完成与下列函数创建的执行器状态相关操作：  
 * create_edata_for_relation().
 */
static void
finish_edata(ApplyExecutionData *edata)
{
	EState	   *estate = edata->estate;

	/* 处理排队的 AFTER 触发器。*/
	AfterTriggerEndQuery(estate);

	/* 若做过元组路由则将其关闭。   */
	if (edata->proute)
		ExecCleanupTupleRouting(edata->mtstate, edata->proute);

	/*
	 * 清理。看起来我们似乎应该调用 ExecCloseResultRelations() 
	 * ，但我们有意不这么做。那会关闭我们先前加入      
	 * es_opened_result_relations 的关系，这不对，因为我们    
	 * 没有持有相应的引用计数。其余执行期间打开的关系 
	 * 靠 ExecCleanupTupleRouting() 关闭。     
	 */
	ExecResetTupleTable(estate->es_tupleTable, false);
	FreeExecutorState(estate);
	pfree(edata);
}

/*
 * 对于那些无法映射到远端关系列的列，执行           
 * 默认值。     
 *
 * 这让我们能够支持那些下游列数多于上游列数              
 * 的表。            
 */
static void
slot_fill_defaults(LogicalRepRelMapEntry *rel, EState *estate,
				   TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel->localrel);
	int			num_phys_attrs = desc->natts;
	int			i;
	int			attnum,
				num_defaults = 0;
	int		   *defmap;
	ExprState **defexprs;
	ExprContext *econtext;

	econtext = GetPerTupleExprContext(estate);

	/* 所有数据都已通过复制获得，无需再求任何值。    */
	if (num_phys_attrs == rel->remoterel.natts)
		return;

	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	Assert(rel->attrmap->maplen == num_phys_attrs);
	for (attnum = 0; attnum < num_phys_attrs; attnum++)
	{
		Expr	   *defexpr;

		if (TupleDescAttr(desc, attnum)->attisdropped || TupleDescAttr(desc, attnum)->attgenerated)
			continue;

		if (rel->attrmap->attnums[attnum] >= 0)
			continue;

		defexpr = (Expr *) build_column_default(rel->localrel, attnum + 1);

		if (defexpr != NULL)
		{
			/* 用规划器处理该表达式     */
			defexpr = expression_planner(defexpr);

			/* 在 copycontext 中初始化可执行表达式  */
			defexprs[num_defaults] = ExecInitExpr(defexpr, NULL);
			defmap[num_defaults] = attnum;
			num_defaults++;
		}
	}

	for (i = 0; i < num_defaults; i++)
		slot->tts_values[defmap[i]] =
			ExecEvalExpr(defexprs[i], econtext, &slot->tts_isnull[defmap[i]]);
}

/*
 * 把元组数据存入 slot 
 *
 * 传入数据可以是文本或二进制格式。  
 */
static void
slot_store_data(TupleTableSlot *slot, LogicalRepRelMapEntry *rel,
				LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	ExecClearTuple(slot);

	/* 对每个未删除且非空的属性调用 "in" 函数          */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			Assert(remoteattnum < tupleData->ncols);

			/* 为错误回调设置 attnum  */
			apply_error_callback_arg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * 有的代码路径可能要求重新解析同一       
				 * 元组数据。为此要重置 StringInfo 的游标。   
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* 若未吃掉整个缓冲区则有麻烦   */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/*
				 * 远端 NULL 值。（按理不会见到         
				 * LOGICALREP_COLUMN_UNCHANGED，若见到就视为 NULL）   
				 * 。   
				 */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			/* 为错误回调重置 attnum    */
			apply_error_callback_arg.remote_attnum = -1;
		}
		else
		{
			/*
			 * 我们给已删除的属性和缺少的值赋 NULL     
			 * （缺少的值稍后由 slot_fill_defaults 
			 * 填充）。        
			 */
			slot->tts_values[i] = (Datum) 0;
			slot->tts_isnull[i] = true;
		}
	}

	ExecStoreVirtualTuple(slot);
}

/*
 * 用 LogicalRepTupleData 结构体中的数据替换已更新列。    
 * 这有点类似于 heap_modify_tuple，但还会对用户数据      
 * 调用类型输入函数。      
 *
 * "slot" 会被填充为 "srcslot" 中元组的副本，替换掉  
 * "tupleData" 中给出的列，其余列保持不变。   
 *
 * 注意："slot" 中未被替换的按引用传递列将指向        
 * "srcslot" 的存储。现有用法没问题，但将来可能需要     
 * 在最后物化 "slot"，使其独立于 "srcslot"。                     
 */
static void
slot_modify_data(TupleTableSlot *slot, TupleTableSlot *srcslot,
				 LogicalRepRelMapEntry *rel,
				 LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	/* 将用虚拟元组填充 "slot"，所以必须先 ...             */
	ExecClearTuple(slot);

	/*
	 * 从 srcslot 复制所有列数据，这样未替换的列就有了    
	 * 有效值。           
	 */
	Assert(natts == srcslot->tts_tupleDescriptor->natts);
	slot_getallattrs(srcslot);
	memcpy(slot->tts_values, srcslot->tts_values, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, srcslot->tts_isnull, natts * sizeof(bool));

	/* 对每个被替换的属性调用 "in" 函数      */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (remoteattnum < 0)
			continue;

		Assert(remoteattnum < tupleData->ncols);

		if (tupleData->colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			/* 为错误回调设置 attnum  */
			apply_error_callback_arg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * 有的代码路径可能要求重新解析同一       
				 * 元组数据。为此要重置 StringInfo 的游标。   
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* 若未吃掉整个缓冲区则有麻烦   */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/* 应为 LOGICALREP_COLUMN_NULL  */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			/* 为错误回调重置 attnum    */
			apply_error_callback_arg.remote_attnum = -1;
		}
	}

	/* 最后，声明 "slot" 包含一个有效的虚拟元组        */
	ExecStoreVirtualTuple(slot);
}

/*
 * 处理 BEGIN 消息  
 */
static void
apply_handle_begin(StringInfo s)
{
	LogicalRepBeginData begin_data;

	/* 此处不允许存在活动中的流式事务。   */
	Assert(!TransactionIdIsValid(stream_xid));

	logicalrep_read_begin(s, &begin_data);
	set_apply_error_context_xact(begin_data.xid, begin_data.final_lsn);

	remote_final_lsn = begin_data.final_lsn;

	maybe_start_skipping_changes(begin_data.final_lsn);

	in_remote_transaction = true;

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * 处理 COMMIT 消息  
 *
 * TODO：支持跟踪多个复制源        
 */
static void
apply_handle_commit(StringInfo s)
{
	LogicalRepCommitData commit_data;

	logicalrep_read_commit(s, &commit_data);

	if (commit_data.commit_lsn != remote_final_lsn)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("incorrect commit LSN %X/%X in commit message (expected %X/%X)",
								 LSN_FORMAT_ARGS(commit_data.commit_lsn),
								 LSN_FORMAT_ARGS(remote_final_lsn))));

	apply_handle_commit_internal(&commit_data);

	/* 处理所有正在并行同步的表。                     */
	process_syncing_tables(commit_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * 处理 BEGIN PREPARE 消息  
 */
static void
apply_handle_begin_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData begin_data;

	/* 表同步绝不应收到 prepare。     */
	if (am_tablesync_worker())
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("tablesync worker received a BEGIN PREPARE message")));

	/* 此处不允许存在活动中的流式事务。   */
	Assert(!TransactionIdIsValid(stream_xid));

	logicalrep_read_begin_prepare(s, &begin_data);
	set_apply_error_context_xact(begin_data.xid, begin_data.prepare_lsn);

	remote_final_lsn = begin_data.prepare_lsn;

	maybe_start_skipping_changes(begin_data.prepare_lsn);

	in_remote_transaction = true;

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * 构成 GID 的共用函数。      
 */
static void
apply_handle_prepare_internal(LogicalRepPreparedTxnData *prepare_data)
{
	char		gid[GIDSIZE];

	/*
	 * 为两阶段事务计算唯一的 GID。我们不使用服务器  
	 * 发送的已准备事务的 GID，因为当同一节点上的多个  
	 * 订阅指向同一节点的发布时，就可能导致死锁。       
	 * 参见 worker.c 顶部的注释          
	 */
	TwoPhaseTransactionGid(MySubscription->oid, prepare_data->xid,
						   gid, sizeof(gid));

	/*
	 * BeginTransactionBlock 必须与下方 PrepareTransactionBlock        
	 * 内调用的 EndTransactionBlock 相配对。   
	 */
	if (!IsTransactionBlock())
	{
		BeginTransactionBlock();
		CommitTransactionCommand(); /* 完成前一个 Begin 命令。        */
	}

	/*
	 * 更新复制源状态，以便崩溃后能从正确位置重启      
	 * 流式传输。  
	 */
	replorigin_session_origin_lsn = prepare_data->end_lsn;
	replorigin_session_origin_timestamp = prepare_data->prepare_time;

	PrepareTransactionBlock(gid);
}

/*
 * 处理 PREPARE 消息  
 */
static void
apply_handle_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData prepare_data;

	logicalrep_read_prepare(s, &prepare_data);

	if (prepare_data.prepare_lsn != remote_final_lsn)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("incorrect prepare LSN %X/%X in prepare message (expected %X/%X)",
								 LSN_FORMAT_ARGS(prepare_data.prepare_lsn),
								 LSN_FORMAT_ARGS(remote_final_lsn))));

	/*
	 * 与提交不同，这里我们总是准备事务，即使该事务   
	 * 没有发生变更或变更全部被跳过。这样做是因为在    
	 * 提交已准备事务时，我们无法知道是否因上述原因而  
	 * 跳过了对某个事务的准备。                             
	 *
	 * XXX，可以优化为在提交已准备事务时先检查是否已    
	 * 准备过该事务，但这似乎不值得，因为这种情况      
	 * 不应该很常见。                             
	 */
	begin_replication_step();

	apply_handle_prepare_internal(&prepare_data);

	end_replication_step();
	CommitTransactionCommand();
	pgstat_report_stat(false);

	store_flush_position(prepare_data.end_lsn, XactLastCommitEnd);

	in_remote_transaction = false;

	/* 处理所有正在并行同步的表。                     */
	process_syncing_tables(prepare_data.end_lsn);

	/*
	 * 由于已准备好该事务，如果在清除 subskiplsn 之前    
	 * 服务器崩溃，subskiplsn 会残留，但该事务不会被重新  
	 * 发送。不过没关系，因为这种情况很罕见，而且      
	 * subskiplsn 会在结束下一个事务时被清除。                  
	 */
	stop_skipping_changes();
	clear_subscription_skip_lsn(prepare_data.prepare_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * 处理先前已准备事务的 COMMIT PREPARED。             
 *
 * 注意，如果该事务是在并行应用工作进程中准备的，    
 * 这里就无需等待。因为那种情况下，我们已经在            
 * apply_handle_stream_prepare() 中等待过准备完成，这会     
 * 确保该事务的所有操作已在订阅者发生，因此不会   
 * 有并发事务导致死锁或事务依赖问题。                        
 */
static void
apply_handle_commit_prepared(StringInfo s)
{
	LogicalRepCommitPreparedTxnData prepare_data;
	char		gid[GIDSIZE];

	logicalrep_read_commit_prepared(s, &prepare_data);
	set_apply_error_context_xact(prepare_data.xid, prepare_data.commit_lsn);

	/* 为两阶段事务计算 GID。         */
	TwoPhaseTransactionGid(MySubscription->oid, prepare_data.xid,
						   gid, sizeof(gid));

	/* 调用 COMMIT PREPARED 时没有事务                 */
	begin_replication_step();

	/*
	 * 更新复制源状态，以便崩溃后能从正确位置重启      
	 * 流式传输。  
	 */
	replorigin_session_origin_lsn = prepare_data.end_lsn;
	replorigin_session_origin_timestamp = prepare_data.commit_time;

	FinishPreparedTransaction(gid, true);
	end_replication_step();
	CommitTransactionCommand();
	pgstat_report_stat(false);

	store_flush_position(prepare_data.end_lsn, XactLastCommitEnd);
	in_remote_transaction = false;

	/* 处理所有正在并行同步的表。                     */
	process_syncing_tables(prepare_data.end_lsn);

	clear_subscription_skip_lsn(prepare_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * 处理先前已准备事务的 ROLLBACK PREPARED。             
 *
 * 注意，如果该事务是在并行应用工作进程中准备的，    
 * 这里就无需等待。因为那种情况下，我们已经在            
 * apply_handle_stream_prepare() 中等待过准备完成，这会     
 * 确保该事务的所有操作已在订阅者发生，因此不会   
 * 有并发事务导致死锁或事务依赖问题。                        
 */
static void
apply_handle_rollback_prepared(StringInfo s)
{
	LogicalRepRollbackPreparedTxnData rollback_data;
	char		gid[GIDSIZE];

	logicalrep_read_rollback_prepared(s, &rollback_data);
	set_apply_error_context_xact(rollback_data.xid, rollback_data.rollback_end_lsn);

	/* 为两阶段事务计算 GID。         */
	TwoPhaseTransactionGid(MySubscription->oid, rollback_data.xid,
						   gid, sizeof(gid));

	/*
	 * 有可能我们尚未收到 prepare，因为它发生在 walsender 
	 * 到达一致性点之前，或当时 two_phase 尚未启用，因此  
	 * 在这种情况下，我们需要跳过已准备事务的回滚     
	 * 操作   
	 */
	if (LookupGXact(gid, rollback_data.prepare_end_lsn,
					rollback_data.prepare_time))
	{
		/*
		 * 更新复制源状态，以便崩溃后能从正确位置   
		 * 重启流式传输。     
		 */
		replorigin_session_origin_lsn = rollback_data.rollback_end_lsn;
		replorigin_session_origin_timestamp = rollback_data.rollback_time;

		/* 调用 ABORT/ROLLBACK PREPARED 时没有事务                 */
		begin_replication_step();
		FinishPreparedTransaction(gid, false);
		end_replication_step();
		CommitTransactionCommand();

		clear_subscription_skip_lsn(rollback_data.rollback_end_lsn);
	}

	pgstat_report_stat(false);

	store_flush_position(rollback_data.rollback_end_lsn, XactLastCommitEnd);
	in_remote_transaction = false;

	/* 处理所有正在并行同步的表。                     */
	process_syncing_tables(rollback_data.rollback_end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * 处理 STREAM PREPARE 
 */
static void
apply_handle_stream_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData prepare_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* 在消息被消费前保存它。       */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM PREPARE message without STREAM STOP")));

	/* 表同步绝不应收到 prepare。     */
	if (am_tablesync_worker())
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("tablesync worker received a STREAM PREPARE message")));

	logicalrep_read_stream_prepare(s, &prepare_data);
	set_apply_error_context_xact(prepare_data.xid, prepare_data.prepare_lsn);

	apply_action = get_transaction_apply_action(prepare_data.xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * 该事务已被序列化到文件，因此需要重放所有  
			 * 溢写的操作。 
			 */
			apply_spooled_messages(MyLogicalRepWorker->stream_fileset,
								   prepare_data.xid, prepare_data.prepare_lsn);

			/* 将该事务标记为已准备。 */
			apply_handle_prepare_internal(&prepare_data);

			CommitTransactionCommand();

			store_flush_position(prepare_data.end_lsn, XactLastCommitEnd);

			in_remote_transaction = false;

			/* 取消链接已序列化变更与子事务信息文件。  */
			stream_cleanup_files(MyLogicalRepWorker->subid, prepare_data.xid);

			elog(DEBUG1, "finished processing the STREAM PREPARE command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			if (pa_send_data(winfo, s->len, s->data))
			{
				/* 完成流式事务的处理。               */
				pa_xact_finish(winfo, prepare_data.end_lsn);
				break;
			}

			/*
			 * 无法把变更发送给并行应用工作进程时，   
			 * 切换到序列化模式。     
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			stream_open_and_write_change(prepare_data.xid,
										 LOGICAL_REP_MSG_STREAM_PREPARE,
										 &original_msg);

			pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);

			/* 完成流式事务的处理。               */
			pa_xact_finish(winfo, prepare_data.end_lsn);
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * 如果并行应用工作进程正在应用溢写的消息，  
			 * 则在准备前关闭文件。  
			 */
			if (stream_fd)
				stream_close_file();

			begin_replication_step();

			/* 将该事务标记为已准备。 */
			apply_handle_prepare_internal(&prepare_data);

			end_replication_step();

			CommitTransactionCommand();

			MyParallelShared->last_commit_end = XactLastCommitEnd;

			pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);
			pa_unlock_transaction(MyParallelShared->xid, AccessExclusiveLock);

			pa_reset_subtrans();

			elog(DEBUG1, "finished processing the STREAM PREPARE command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	pgstat_report_stat(false);

	/* 处理所有正在并行同步的表。                     */
	process_syncing_tables(prepare_data.end_lsn);

	/*
	 * 与 prepare 的情况类似，在服务器崩溃时 subskiplsn    
	 * 可能残留，不过没关系，注释见 apply_handle_prepare()      
	 */
	stop_skipping_changes();
	clear_subscription_skip_lsn(prepare_data.prepare_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * 处理 ORIGIN 消息  
 *
 * TODO，支持跟踪多个复制源        
 */
static void
apply_handle_origin(StringInfo s)
{
	/*
	 * ORIGIN 消息只会出现在流式事务内部，或者出现在   
	 * 远程事务中，且在实际写入之前。   
	 */
	if (!in_streamed_transaction &&
		(!in_remote_transaction ||
		 (IsTransactionState() && !am_tablesync_worker())))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("ORIGIN message sent out of order")));
}

/*
 * 初始化 fileset（如未初始化）   
 *
 * first_segment 为真时创建新文件，否则打开已有的文        
 * 件  
 */
void
stream_start_internal(TransactionId xid, bool first_segment)
{
	begin_replication_step();

	/*
	 * 如果尚未初始化，则初始化工作进程的 stream_fileset。 
	 * 它在工作进程的整个生命周期都会用到，因此要       
	 * 在永久上下文中创建它。我们在来自任意事务的     
	 * 第一条流式消息上创建它，然后将它用于这个及       
	 * 其他流式事务。其实也可以在工作进程启动时创      
	 * 建 fileset，但不确定它是否被用到。 
	 */
	if (!MyLogicalRepWorker->stream_fileset)
	{
		MemoryContext oldctx;

		oldctx = MemoryContextSwitchTo(ApplyContext);

		MyLogicalRepWorker->stream_fileset = palloc(sizeof(FileSet));
		FileSetInit(MyLogicalRepWorker->stream_fileset);

		MemoryContextSwitchTo(oldctx);
	}

	/* 为该事务打开溢写文件。         */
	stream_open_file(MyLogicalRepWorker->subid, xid, first_segment);

	/* 若非第一个分段，则打开已有的子事务文件。  */
	if (!first_segment)
		subxact_info_read(MyLogicalRepWorker->subid, xid);

	end_replication_step();
}

/*
 * 处理 STREAM START 消息  
 */
static void
apply_handle_stream_start(StringInfo s)
{
	bool		first_segment;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* 在消息被消费前保存它。       */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("duplicate STREAM START message")));

	/* 此处不允许存在活动中的流式事务。   */
	Assert(!TransactionIdIsValid(stream_xid));

	/* 通知各处理方法：我们正在处理远程事务      */
	in_streamed_transaction = true;

	/* 提取顶层事务的 XID                */
	stream_xid = logicalrep_read_stream_start(s, &first_segment);

	if (!TransactionIdIsValid(stream_xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid transaction ID in streamed replication transaction")));

	set_apply_error_context_xact(stream_xid, InvalidXLogRecPtr);

	/* 尝试为该流式事务分配一个工作进程。     */
	if (first_segment)
		pa_allocate_worker(stream_xid);

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:

			/*
			 * 函数 stream_start_internal 会启动一个事务。这 
			 * 个事务会在流停止时提交；但如果这是 tablesync  
			 * 工作进程，则在处理完所有消息后提交。   
			 * 需要它来处理 BufFile，用于序列化流式数据 
			 * 和子事务                                                 
			 * 信息。        
			 */
			stream_start_internal(stream_xid, first_segment);
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * 一旦开始序列化变更，并行应用工作进程   
			 * 会等待领导者释放流锁直到事务结束。因此 
			 * 这种情况下无需释放锁或递增流计                 
			 * 数。                                              
			 */
			if (pa_send_data(winfo, s->len, s->data))
			{
				/*
				 * 解锁共享对象锁，让并行应用工作进程继  
				 * 续接收变更。                     
				 */
				if (!first_segment)
					pa_unlock_stream(winfo->shared->xid, AccessExclusiveLock);

				/*
				 * 递增等待并行应用工作进程处理的流式   
				 * 块的数量。                    
				 */
				pg_atomic_add_fetch_u32(&winfo->shared->pending_stream_count, 1);

				/* 为该事务缓存并行应用工作进程。         */
				pa_set_stream_apply_worker(winfo);
				break;
			}

			/*
			 * 无法把变更发送给并行应用工作进程时，   
			 * 切换到序列化模式。     
			 */
			pa_switch_to_partial_serialize(winfo, !first_segment);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			/*
			 * 打开溢写文件，除非在切换到序列化模式时已   
			 * 经将其打开。stream_start_internal 中  
			 * 启动的事务会在流停止时提交。                 
			 */
			if (apply_action != TRANS_LEADER_SEND_TO_PARALLEL)
				stream_start_internal(stream_xid, first_segment);

			stream_write_change(LOGICAL_REP_MSG_STREAM_START, &original_msg);

			/* 为该事务缓存并行应用工作进程。         */
			pa_set_stream_apply_worker(winfo);
			break;

		case TRANS_PARALLEL_APPLY:
			if (first_segment)
			{
				/* 持有该锁直到事务结束。               */
				pa_lock_transaction(MyParallelShared->xid, AccessExclusiveLock);
				pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_STARTED);

				/*
				 * 向领导者应用工作进程发信号，它正等待  
				 * us.
				 */
				logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);
			}

			parallel_stream_nchanges = 0;
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * 更新子事务信息并关闭文件。                  
 *
 * 该函数应当在 stream_start_internal 函数已被调用               
 * 之后。   
 */
void
stream_stop_internal(TransactionId xid)
{
	/*
	 * 序列化顶层事务的子事务信息，然后关闭流消息        
	 * 溢写文件。                      
	 */
	subxact_info_write(MyLogicalRepWorker->subid, xid);
	stream_close_file();

	/* 我们必须处于有效的事务状态 */
	Assert(IsTransactionState());

	/* 提交每个流的事务          */
	CommitTransactionCommand();

	/* 重置每流上下文    */
	MemoryContextReset(LogicalStreamingContext);
}

/*
 * 处理 STREAM STOP 消息  
 */
static void
apply_handle_stream_stop(StringInfo s)
{
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	if (!in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM STOP message without STREAM START")));

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:
			stream_stop_internal(stream_xid);
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * 在发送 STREAM_STOP 消息之前先加锁，这样领导者 
			 * 能先持有锁，并行应用工作进程则会等待领导   
			 * 者释放锁。参见 applyparallelworker.c 顶部的          
			 * 加锁注意事项    
			 */
			pa_lock_stream(winfo->shared->xid, AccessExclusiveLock);

			if (pa_send_data(winfo, s->len, s->data))
			{
				pa_set_stream_apply_worker(NULL);
				break;
			}

			/*
			 * 无法把变更发送给并行应用工作进程时，   
			 * 切换到序列化模式。     
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			stream_write_change(LOGICAL_REP_MSG_STREAM_STOP, s);
			stream_stop_internal(stream_xid);
			pa_set_stream_apply_worker(NULL);
			break;

		case TRANS_PARALLEL_APPLY:
			elog(DEBUG1, "applied %u changes in the streaming chunk",
				 parallel_stream_nchanges);

			/*
			 * 当并行应用工作进程处理当前流式块变更时，  
			 * 领导者应用工作进程可能已经发送了多个流式 
			 * 块。这会使并行应用工作进程在队列中还有更   
			 * 更多流数据块时就开始等待。因此只有当队列  
			 * 中没有剩余消息时才尝试加锁。参见              
			 * applyparallelworker.c 顶部加锁注意     
			 * 事项。             
			 *
			 * 注意，这里存在竞态条件，即使队列中仍有 
			 * 待处理的流数据块，我们也可能开始等待。这  
			 * 种情况可能发生在：领导者又发送了一个流      
			 * 新获取流锁之后，并且在并行应用工作进程   
			 * 确认没有待处理的流块之后、真正开始在锁上   
			 * 等待之前。我们可以禁止领导者在此期间  
			 * 内递增流块计数来处理，但尚不清楚这是否     
			 * 值得引入这样的复杂度                                
			 * 。                                        
			 *
			 * 另外，如果错过的这个块含有回滚到保存点，  
			 * 就有死锁风险，不过重启之后应该不会再发  
			 * 生。        
			 */
			pa_decr_and_wait_stream_block();
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	in_streamed_transaction = false;
	stream_xid = InvalidTransactionId;

	/*
	 * 并行应用工作进程可能正处于事务中，这种情况下  
	 * 须将状态报告为 STATE_IDLEINTRANSACTION。    
	 */
	if (IsTransactionOrTransactionBlock())
		pgstat_report_activity(STATE_IDLEINTRANSACTION, NULL);
	else
		pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * 辅助函数：当事务已被序列化到文件时处理              
 * STREAM ABORT       
 */
static void
stream_abort_internal(TransactionId xid, TransactionId subxid)
{
	/*
	 * 如果两个 XID 相同，那实际上就是顶层事务的中止， 
	 * 只需删除已序列化信息的文件。 
	 */
	if (xid == subxid)
		stream_cleanup_files(MyLogicalRepWorker->subid, xid);
	else
	{
		/*
		 * 好，这说明是子事务。我们需要读取顶层事务   
		 * 的子事务文件，确定为该子事务跟踪的偏移量，    
		 * 并截断含变更的文件。还要移除具有更高偏移   
		 * 量的子事务（或者说更高 XID）。 
		 *
		 * 我们特意从数组尾部开始扫描，因为我们中止的    
		 * 很可能就是最近那些子事务的变更。      
		 *
		 * 这里无法用二分查找，因为子事务 XID 未必 
		 * 会按排序后的顺序到达，设想这样一种情况：我    
		 * 们曾为多个子事务释放保存点，再对早前的   
		 * 一个子事务执行回滚到保存点的操         
		 * 作。          
		 */
		int64		i;
		int64		subidx;
		BufFile    *fd;
		bool		found = false;
		char		path[MAXPGPATH];

		subidx = -1;
		begin_replication_step();
		subxact_info_read(MyLogicalRepWorker->subid, xid);

		for (i = subxact_data.nsubxacts; i > 0; i--)
		{
			if (subxact_data.subxacts[i - 1].xid == subxid)
			{
				subidx = (i - 1);
				found = true;
				break;
			}
		}

		/*
		 * 如果这是一个空子事务，那么在这里找不到该     
		 * 子 XID，只需清理子事务信息并返回   
		 */
		if (!found)
		{
			/* 清理子事务信息    */
			cleanup_subxact_info();
			end_replication_step();
			CommitTransactionCommand();
			return;
		}

		/* 打开变更文件    */
		changes_filename(path, MyLogicalRepWorker->subid, xid);
		fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path,
								O_RDWR, false);

		/* 好，在正确偏移量处截断文件   */
		BufFileTruncateFileSet(fd, subxact_data.subxacts[subidx].fileno,
							   subxact_data.subxacts[subidx].offset);
		BufFileClose(fd);

		/* 丢弃之后添加的子事务   */
		subxact_data.nsubxacts = subidx;

		/* 写入更新后子事务列表 */
		subxact_info_write(MyLogicalRepWorker->subid, xid);

		end_replication_step();
		CommitTransactionCommand();
	}
}

/*
 * 处理 STREAM ABORT 消息  
 */
static void
apply_handle_stream_abort(StringInfo s)
{
	TransactionId xid;
	TransactionId subxid;
	LogicalRepStreamAbortData abort_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* 在消息被消费前保存它。       */
	StringInfoData original_msg = *s;
	bool		toplevel_xact;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM ABORT message without STREAM STOP")));

	/* 只有能够并行应用时，我们才会收到中止信息。  */
	logicalrep_read_stream_abort(s, &abort_data,
								 MyLogicalRepWorker->parallel_apply);

	xid = abort_data.xid;
	subxid = abort_data.subxid;
	toplevel_xact = (xid == subxid);

	set_apply_error_context_xact(subxid, abort_data.abort_lsn);

	apply_action = get_transaction_apply_action(xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * 我们在领导者应用工作进程中，该事务已被序  
			 * 列化到文件。 
			 */
			stream_abort_internal(xid, subxid);

			elog(DEBUG1, "finished processing the STREAM ABORT command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * 对于中止子事务的情况，我们会在发送          
			 * STREAM_ABORT 前递增流式块数并重新取锁，     
			 * 以确保并行应用工作进程在处理 STREAM_ABORT   
			 * 消息之后，会为下一批变更在锁上等待，只要  
			 * 它尚未在等待 STREAM_STOP                          
			 * 消息。                       
			 *
			 * 务必在发送 STREAM_ABORT 消息之前执行这个加  
			 * 锁，这样领导者能先持有锁，并行应用工作进   
			 * 程将等待领导者释放锁。这与我们在         
			 * apply_handle_stream_stop 中的做法相同。     
			 * 参见 applyparallelworker.c 顶部的                   
			 * 加锁注意事项。 
			 */
			if (!toplevel_xact)
			{
				pa_unlock_stream(xid, AccessExclusiveLock);
				pg_atomic_add_fetch_u32(&winfo->shared->pending_stream_count, 1);
				pa_lock_stream(xid, AccessExclusiveLock);
			}

			if (pa_send_data(winfo, s->len, s->data))
			{
				/*
				 * 与 STREAM_COMMIT 和 STREAM_PREPARE 不同，这里     
				 * 无需等待并行应用工作进程完成，因为      
				 * 维护提交顺序并不需要它，也不会有因事务  
				 * 赖和死锁而失败的风险。但可能在它完成  
				 * 并清除工作进程信息之前，上游就发生 XID 回
				 * 卷，从而出现相同 XID 的新事务，导致   
				 * ParallelApplyTxnHash 中产生重复条目。           
				 * 另一个可能的问题是：我们可能已用部分序  
				 * 列化模式来序列化变更，包含事务变更的文 
				 * 件可能已经存在，XID 回卷后再次尝试    
				 * 为同一个 XID 创建该文件，就可能导        
				 * 致错误。为了避免这些问                       
				 * 题，我们决定等待中止完                       
				 * 成。                                                     
				 *
				 * 注意，中止时可不更新刷写位置，最坏情况 
				 * 意味着这类事务重启后不会再被               
				 * 发送。                         
				 */
				if (toplevel_xact)
					pa_xact_finish(winfo, InvalidXLogRecPtr);

				break;
			}

			/*
			 * 无法把变更发送给并行应用工作进程时，   
			 * 切换到序列化模式。     
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			/*
			 * 并行应用工作进程可能已应用了变更，因此要   
			 * 写入 STREAM_ABORT 消息，以便需要时回滚  
			 * 该子事务。          
			 */
			stream_open_and_write_change(xid, LOGICAL_REP_MSG_STREAM_ABORT,
										 &original_msg);

			if (toplevel_xact)
			{
				pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);
				pa_xact_finish(winfo, InvalidXLogRecPtr);
			}
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * 如果并行应用工作进程正在应用溢写的消息，  
			 * 则在中止前关闭文件。 
			 */
			if (toplevel_xact && stream_fd)
				stream_close_file();

			pa_stream_abort(&abort_data);

			/*
			 * 在处理回滚到保存点之后，我们需要等待下一  
			 * 批变更。        
			 *
			 * 这里存在竞态条件，导致我们在队列中还有更   
			 * 多流数据块时就开始等待。参见                 
			 * apply_handle_stream_stop 
			 */
			if (!toplevel_xact)
				pa_decr_and_wait_stream_block();

			elog(DEBUG1, "finished processing the STREAM ABORT command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	reset_apply_error_context_info();
}

/*
 * 确保传入的位置是 fileset 的末尾。    
 */
static void
ensure_last_message(FileSet *stream_fileset, TransactionId xid, int fileno,
					off_t offset)
{
	char		path[MAXPGPATH];
	BufFile    *fd;
	int			last_fileno;
	off_t		last_offset;

	Assert(!IsTransactionState());

	begin_replication_step();

	changes_filename(path, MyLogicalRepWorker->subid, xid);

	fd = BufFileOpenFileSet(stream_fileset, path, O_RDONLY, false);

	BufFileSeek(fd, 0, 0, SEEK_END);
	BufFileTell(fd, &last_fileno, &last_offset);

	BufFileClose(fd);

	end_replication_step();

	if (last_fileno != fileno || last_offset != offset)
		elog(ERROR, "unexpected message left in streaming transaction's changes file \"%s\"",
			 path);
}

/*
 * 通用溢写文件处理。 
 */
void
apply_spooled_messages(FileSet *stream_fileset, TransactionId xid,
					   XLogRecPtr lsn)
{
	StringInfoData s2;
	int			nchanges;
	char		path[MAXPGPATH];
	char	   *buffer = NULL;
	MemoryContext oldcxt;
	ResourceOwner oldowner;
	int			fileno;
	off_t		offset;

	if (!am_parallel_apply_worker())
		maybe_start_skipping_changes(lsn);

	/* 确保我们已有打开的事务     */
	begin_replication_step();

	/*
	 * 在 TopTransactionContext 中分配处理所有消息所需            
	 * 的文件句柄和内存，以免它们在每条消息被处理后     
	 * 被重置 
	 */
	oldcxt = MemoryContextSwitchTo(TopTransactionContext);

	/* 为已提交/已准备的事务打开溢写文件          */
	changes_filename(path, MyLogicalRepWorker->subid, xid);
	elog(DEBUG1, "replaying changes from file \"%s\"", path);

	/*
	 * 确保该文件归顶层事务所有，这样在中止子事务    
	 * 时，文件不会被意外关闭。                                
	 */
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = TopTransactionResourceOwner;

	stream_fd = BufFileOpenFileSet(stream_fileset, path, O_RDONLY, false);

	CurrentResourceOwner = oldowner;

	buffer = palloc(BLCKSZ);
	initStringInfo(&s2);

	MemoryContextSwitchTo(oldcxt);

	remote_final_lsn = lsn;

	/*
	 * 确保 apply_dispatch 的各个处理方法知晓我们正处于远程 
	 * 事务中。
	 */
	in_remote_transaction = true;
	pgstat_report_activity(STATE_RUNNING, NULL);

	end_replication_step();

	/*
	 * 逐个读取条目，并按与 apply_dispatch 相同的逻辑         
	 * 进行处理。
	 */
	nchanges = 0;
	while (true)
	{
		size_t		nbytes;
		int			len;

		CHECK_FOR_INTERRUPTS();

		/* 读取磁盘上记录的长度    */
		nbytes = BufFileReadMaybeEOF(stream_fd, &len, sizeof(len), true);

		/* 是否已到达文件末尾？   */
		if (nbytes == 0)
			break;

		/* 长度是否正确？        */
		if (len <= 0)
			elog(ERROR, "incorrect length %d in streaming transaction's changes file \"%s\"",
				 len, path);

		/* 确保我们拥有足够大的缓冲区     */
		buffer = repalloc(buffer, len);

		/* 最后将数据读入缓冲区            */
		BufFileReadExact(stream_fd, buffer, len);

		BufFileTell(stream_fd, &fileno, &offset);

		/* 把缓冲区复制到 stringinfo 并调用 apply_dispatch */
		resetStringInfo(&s2);
		appendBinaryStringInfo(&s2, buffer, len);

		/* 确保将数据读入我们的内存上下文中。     */
		oldcxt = MemoryContextSwitchTo(ApplyMessageContext);

		apply_dispatch(&s2);

		MemoryContextReset(ApplyMessageContext);

		MemoryContextSwitchTo(oldcxt);

		nchanges++;

		/*
		 * 有可能文件已被关闭，因为我们处理了              
		 * stream_commit 之类的事务结束消息，此时它必然是   
		 * 最后一条消息。    
		 */
		if (!stream_fd)
		{
			ensure_last_message(stream_fileset, xid, fileno, offset);
			break;
		}

		if (nchanges % 1000 == 0)
			elog(DEBUG1, "replayed %d changes from file \"%s\"",
				 nchanges, path);
	}

	if (stream_fd)
		stream_close_file();

	elog(DEBUG1, "replayed %d (all) changes from file \"%s\"",
		 nchanges, path);

	return;
}

/*
 * STREAM COMMIT 消息处理。
 */
static void
apply_handle_stream_commit(StringInfo s)
{
	TransactionId xid;
	LogicalRepCommitData commit_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* 在消息被消费前保存它。       */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM COMMIT message without STREAM STOP")));

	xid = logicalrep_read_stream_commit(s, &commit_data);
	set_apply_error_context_xact(xid, commit_data.commit_lsn);

	apply_action = get_transaction_apply_action(xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * 该事务已被序列化到文件，因此需要重放所有  
			 * 溢写的操作。 
			 */
			apply_spooled_messages(MyLogicalRepWorker->stream_fileset, xid,
								   commit_data.commit_lsn);

			apply_handle_commit_internal(&commit_data);

			/* 取消链接已序列化变更与子事务信息文件。  */
			stream_cleanup_files(MyLogicalRepWorker->subid, xid);

			elog(DEBUG1, "finished processing the STREAM COMMIT command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			if (pa_send_data(winfo, s->len, s->data))
			{
				/* 完成流式事务的处理。               */
				pa_xact_finish(winfo, commit_data.end_lsn);
				break;
			}

			/*
			 * 无法把变更发送给并行应用工作进程时，   
			 * 切换到序列化模式。     
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			stream_open_and_write_change(xid, LOGICAL_REP_MSG_STREAM_COMMIT,
										 &original_msg);

			pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);

			/* 完成流式事务的处理。               */
			pa_xact_finish(winfo, commit_data.end_lsn);
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * 若并行应用工作进程正在应用溢写的消息，     
			 * 则在提交之前关闭文件。
			 */
			if (stream_fd)
				stream_close_file();

			apply_handle_commit_internal(&commit_data);

			MyParallelShared->last_commit_end = XactLastCommitEnd;

			/*
			 * 在释放锁之前务必将事务状态设置为已完成。   
			 * 参见 pa_wait_for_xact_finish。               
			 */
			pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);
			pa_unlock_transaction(xid, AccessExclusiveLock);

			pa_reset_subtrans();

			elog(DEBUG1, "finished processing the STREAM COMMIT command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	/* 处理所有正在并行同步的表。                     */
	process_syncing_tables(commit_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * apply_handle_commit 与 apply_handle_stream_commit 的辅助函数。  
 */
static void
apply_handle_commit_internal(LogicalRepCommitData *commit_data)
{
	if (is_skipping_changes())
	{
		stop_skipping_changes();

		/*
		 * 若事务尚未开始，则启动新事务清除 subskiplsn。 
		 *     
		 */
		if (!IsTransactionState())
			StartTransactionCommand();
	}

	if (IsTransactionState())
	{
		/*
		 * 该事务非空或已跳过，故清除其 subskiplsn。       
		 *            
		 */
		clear_subscription_skip_lsn(commit_data->commit_lsn);

		/*
		 * 更新复制源状态，以便崩溃后能从正确位置   
		 * 重启流式传输。     
		 */
		replorigin_session_origin_lsn = commit_data->end_lsn;
		replorigin_session_origin_timestamp = commit_data->committime;

		CommitTransactionCommand();

		if (IsTransactionBlock())
		{
			EndTransactionBlock(false);
			CommitTransactionCommand();
		}

		pgstat_report_stat(false);

		store_flush_position(commit_data->end_lsn, XactLastCommitEnd);
	}
	else
	{
		/* 处理任何可能已积累的失效消息。                  */
		AcceptInvalidationMessages();
		maybe_reread_subscription();
	}

	in_remote_transaction = false;
}

/*
 * RELATION 消息处理。
 *
 * 注意此处不做针对本地模式的验证。该验证将推迟到
 * 给定关系的首个变更到来为止；反正只有在为它应用  
 * 变更时才会关心它，且这样做加锁更少。                   
 *                          
 */
static void
apply_handle_relation(StringInfo s)
{
	LogicalRepRelation *rel;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_RELATION, s))
		return;

	rel = logicalrep_read_rel(s);
	logicalrep_relmap_update(rel);

	/* 同时重置分区映射中所有引用 remoterel 的条目。       */
	logicalrep_partmap_reset_relmap(rel);
}

/*
 * TYPE 消息处理。
 *
 * 本实现不理会 TYPE 消息；我们预期用户已做好设置，     
 * 使传入的数据能被本地订阅表的输入函数所接受。        
 * 因此，我们只需读取并丢弃该消息。                     
 *                     
 */
static void
apply_handle_type(StringInfo s)
{
	LogicalRepTyp typ;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_TYPE, s))
		return;

	logicalrep_read_typ(s, &typ);
}

/*
 * 检查我们（订阅所有者）是否拥有对目标关系执行给定
 * 操作所需的足够权限。                 
 */
static void
TargetPrivilegesCheck(Relation rel, AclMode mode)
{
	Oid			relid;
	AclResult	aclresult;

	relid = RelationGetRelid(rel);
	aclresult = pg_class_aclcheck(relid, GetUserId(), mode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   get_rel_name(relid));

	/*
	 * 我们缺少遵循 RLS 策略的基础设施。或许可在此添加   
	 * 此类基础设施，但表同步工作进程同样缺少，故我们  
	 * 并不为此费心。RLS 通常并不适用于 TRUNCATE 命令，    
	 * 但先复制 TRUNCATE 然后拒绝复制后续 INSERT，似乎   
	 * 很危险，因此我们对所有命令同样禁止。           
	 */
	if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user \"%s\" cannot replicate into relation with row-level security enabled: \"%s\"",
						GetUserNameFromId(GetUserId(), true),
						RelationGetRelationName(rel))));
}

/*
 * INSERT 消息处理。
 */

static void
apply_handle_insert(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData newtup;
	LogicalRepRelId relid;
	UserContext ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * 若正在跳过数据修改变更或正在处理流式事务，则   
	 * 快速返回。       
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_INSERT, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_insert(s, &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * 该关系无法在事务中途变得值得关注，因此 
		 * 解锁它是安全的。              
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/*
	 * 确保任何用户提供的代码以表所有者身份运行，除非
	 * 用户已选择退出该行为。       
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* 为错误回调设置关系     */
	apply_error_callback_arg.rel = rel;

	/* 初始化执行器状态。    */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/* 处理远端元组并将其存储到槽中 */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &newtup);
	slot_fill_defaults(rel, estate, remoteslot);
	MemoryContextSwitchTo(oldctx);

	/* 对于分区表，将元组插入到某个分区中。      */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, NULL, CMD_INSERT);
	else
		apply_handle_insert_internal(edata, edata->targetRelInfo,
									 remoteslot);

	finish_edata(edata);

	/* 为错误回调重置关系       */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * apply_handle_insert() 主力函数 
 * relinfo 是我们实际要插入到的关系             
 * （可能是 edata->targetRelInfo 的子分区）   
 */
static void
apply_handle_insert_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot)
{
	EState	   *estate = edata->estate;

	/* 必须在此打开索引。*/
	ExecOpenIndices(relinfo, false);

	/* 执行插入。*/
	TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_INSERT);
	ExecSimpleRelationInsert(relinfo, estate, remoteslot);

	/* 清理。*/
	ExecCloseIndices(relinfo);
}

/*
 * 检查逻辑复制关系是否可更新，若不可则抛出    
 * 相应的错误。            
 */
static void
check_relation_updatable(LogicalRepRelMapEntry *rel)
{
	/*
	 * 对于分区表，我们只需关心目标分区是否可更新（即  
	 * 为它定义了 PK 或 RI）。             
	 */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* 可更新无错误。*/
	if (rel->updatable)
		return;

	/*
	 * 我们处于错误模式，因此这里慢一点也无妨。给用户  
	 * 提供正确的错误。
	 */
	if (OidIsValid(GetRelationIdentityOrPK(rel->localrel)))
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publisher did not send replica identity column "
						"expected by the logical replication target relation \"%s.%s\"",
						rel->remoterel.nspname, rel->remoterel.relname)));
	}

	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("logical replication target relation \"%s.%s\" has "
					"neither REPLICA IDENTITY index nor PRIMARY "
					"KEY and published relation does not have "
					"REPLICA IDENTITY FULL",
					rel->remoterel.nspname, rel->remoterel.relname)));
}

/*
 * UPDATE 消息处理。
 *
 * TODO: FDW 支持 
 */
static void
apply_handle_update(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepRelId relid;
	UserContext ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	LogicalRepTupleData oldtup;
	LogicalRepTupleData newtup;
	bool		has_oldtup;
	TupleTableSlot *remoteslot;
	RTEPermissionInfo *target_perminfo;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * 若正在跳过数据修改变更或正在处理流式事务，则   
	 * 快速返回。       
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_UPDATE, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_update(s, &has_oldtup, &oldtup,
								   &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * 该关系无法在事务中途变得值得关注，因此 
		 * 解锁它是安全的。              
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/* 为错误回调设置关系     */
	apply_error_callback_arg.rel = rel;

	/* 检查能否执行更新。    */
	check_relation_updatable(rel);

	/*
	 * 确保任何用户提供的代码以表所有者身份运行，除非
	 * 用户已选择退出该行为。       
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* 初始化执行器状态。    */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/*
	 * 填充 updatedCols 以便每列触发器能够触发，且便于 
	 * 执行器正确地向下传递 indexUnchanged 提示。它       
	 * 包含的列可能多于发布者上实际更改的列，因为 
	 * 逻辑复制协议不含该信息。但例如会排除仅在 
	 * 订阅者上存在的列，因为我们不会触碰那些列。       
	 *                                                    
	 */
	target_perminfo = list_nth(estate->es_rteperminfos, 0);
	for (int i = 0; i < remoteslot->tts_tupleDescriptor->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(remoteslot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			Assert(remoteattnum < newtup.ncols);
			if (newtup.colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
				target_perminfo->updatedCols =
					bms_add_member(target_perminfo->updatedCols,
								   i + 1 - FirstLowInvalidHeapAttributeNumber);
		}
	}

	/* 构建搜索元组。   */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel,
					has_oldtup ? &oldtup : &newtup);
	MemoryContextSwitchTo(oldctx);

	/* 对于分区表，将更新应用到正确的分区。      */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, &newtup, CMD_UPDATE);
	else
		apply_handle_update_internal(edata, edata->targetRelInfo,
									 remoteslot, &newtup, rel->localindexoid);

	finish_edata(edata);

	/* 为错误回调重置关系       */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * apply_handle_update() 主力函数 
 * relinfo 是我们实际要在其中更新的关系    
 * （可能是 edata->targetRelInfo 的子分区）   
 */
static void
apply_handle_update_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot,
							 LogicalRepTupleData *newtup,
							 Oid localindexoid)
{
	EState	   *estate = edata->estate;
	LogicalRepRelMapEntry *relmapentry = edata->targetRel;
	Relation	localrel = relinfo->ri_RelationDesc;
	EPQState	epqstate;
	TupleTableSlot *localslot;
	bool		found;
	MemoryContext oldctx;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1, NIL);
	ExecOpenIndices(relinfo, false);

	found = FindReplTupleInLocalRel(edata, localrel,
									&relmapentry->remoterel,
									localindexoid,
									remoteslot, &localslot);
	ExecClearTuple(remoteslot);

	/*
	 * 已找到。
	 *
	 * 注意若存在其他冲突的唯一索引，这将会失败。   
	 */
	if (found)
	{
		/* 处理远端元组并将其存储到槽中 */
		oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		slot_modify_data(remoteslot, localslot, relmapentry, newtup);
		MemoryContextSwitchTo(oldctx);

		EvalPlanQualSetSlot(&epqstate, remoteslot);

		/* 实际执行更新。 */
		TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_UPDATE);
		ExecSimpleRelationUpdate(relinfo, estate, &epqstate, localslot,
								 remoteslot);
	}
	else
	{
		/*
		 * 要更新的元组无法找到。除发出一条日志消息外   
		 * 别无其他操作。  
		 *
		 * XXX 是否应将其提升为 ereport(LOG)？        
		 */
		elog(DEBUG1,
			 "logical replication did not find row to be updated "
			 "in replication target relation \"%s\"",
			 RelationGetRelationName(localrel));
	}

	/* 清理。*/
	ExecCloseIndices(relinfo);
	EvalPlanQualEnd(&epqstate);
}

/*
 * DELETE 消息处理。
 *
 * TODO: FDW 支持 
 */
static void
apply_handle_delete(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData oldtup;
	LogicalRepRelId relid;
	UserContext ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * 若正在跳过数据修改变更或正在处理流式事务，则   
	 * 快速返回。       
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_DELETE, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_delete(s, &oldtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * 该关系无法在事务中途变得值得关注，因此 
		 * 解锁它是安全的。              
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/* 为错误回调设置关系     */
	apply_error_callback_arg.rel = rel;

	/* 检查能否执行删除。    */
	check_relation_updatable(rel);

	/*
	 * 确保任何用户提供的代码以表所有者身份运行，除非
	 * 用户已选择退出该行为。       
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* 初始化执行器状态。    */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/* 构建搜索元组。   */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &oldtup);
	MemoryContextSwitchTo(oldctx);

	/* 对于分区表，将删除应用到正确的分区。      */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, NULL, CMD_DELETE);
	else
		apply_handle_delete_internal(edata, edata->targetRelInfo,
									 remoteslot, rel->localindexoid);

	finish_edata(edata);

	/* 为错误回调重置关系       */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * apply_handle_delete() 主力函数 
 * relinfo 是我们实际要从中删除的关系         
 * （可能是 edata->targetRelInfo 的子分区）   
 */
static void
apply_handle_delete_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot,
							 Oid localindexoid)
{
	EState	   *estate = edata->estate;
	Relation	localrel = relinfo->ri_RelationDesc;
	LogicalRepRelation *remoterel = &edata->targetRel->remoterel;
	EPQState	epqstate;
	TupleTableSlot *localslot;
	bool		found;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1, NIL);
	ExecOpenIndices(relinfo, false);

	found = FindReplTupleInLocalRel(edata, localrel, remoterel, localindexoid,
									remoteslot, &localslot);

	/* 找到则删除。  */
	if (found)
	{
		EvalPlanQualSetSlot(&epqstate, localslot);

		/* 实际执行删除。 */
		TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_DELETE);
		ExecSimpleRelationDelete(relinfo, estate, &epqstate, localslot);
	}
	else
	{
		/*
		 * 要删除的元组无法找到。除发出一条日志消息外   
		 * 别无其他操作。  
		 *
		 * XXX 是否应将其提升为 ereport(LOG)？        
		 */
		elog(DEBUG1,
			 "logical replication did not find row to be deleted "
			 "in replication target relation \"%s\"",
			 RelationGetRelationName(localrel));
	}

	/* 清理。*/
	ExecCloseIndices(relinfo);
	EvalPlanQualEnd(&epqstate);
}

/*
 * 尝试在对应的本地关系中查找从发布者侧接收的            
 * 元组（在 'remoteslot' 中），可使用复制标识索引、    
 * 主键、索引或必要时进行顺序扫描。 
 *
 * 若找到，本地元组将放入 '*localslot'。  
 */
static bool
FindReplTupleInLocalRel(ApplyExecutionData *edata, Relation localrel,
						LogicalRepRelation *remoterel,
						Oid localidxoid,
						TupleTableSlot *remoteslot,
						TupleTableSlot **localslot)
{
	EState	   *estate = edata->estate;
	bool		found;

	/*
	 * 无论顶层操作为何，我们在此执行的都是读取，故     
	 * 需检查 SELECT 权限。  
	 */
	TargetPrivilegesCheck(localrel, ACL_SELECT);

	*localslot = table_slot_create(localrel, &estate->es_tupleTable);

	Assert(OidIsValid(localidxoid) ||
		   (remoterel->replident == REPLICA_IDENTITY_FULL));

	if (OidIsValid(localidxoid))
	{
#ifdef USE_ASSERT_CHECKING
		Relation	idxrel = index_open(localidxoid, AccessShareLock);

		/* 索引必须是 PK、RI 或可用于 REPLICA IDENTITY FULL 表   */
		Assert(GetRelationIdentityOrPK(idxrel) == localidxoid ||
			   IsIndexUsableForReplicaIdentityFull(BuildIndexInfo(idxrel),
												   edata->targetRel->attrmap));
		index_close(idxrel, AccessShareLock);
#endif

		found = RelationFindReplTupleByIndex(localrel, localidxoid,
											 LockTupleExclusive,
											 remoteslot, *localslot);
	}
	else
		found = RelationFindReplTupleSeq(localrel, LockTupleExclusive,
										 remoteslot, *localslot);

	return found;
}

/*
 * 此处处理分区表的插入、更新和删除操作。  
 */
static void
apply_handle_tuple_routing(ApplyExecutionData *edata,
						   TupleTableSlot *remoteslot,
						   LogicalRepTupleData *newtup,
						   CmdType operation)
{
	EState	   *estate = edata->estate;
	LogicalRepRelMapEntry *relmapentry = edata->targetRel;
	ResultRelInfo *relinfo = edata->targetRelInfo;
	Relation	parentrel = relinfo->ri_RelationDesc;
	ModifyTableState *mtstate;
	PartitionTupleRouting *proute;
	ResultRelInfo *partrelinfo;
	Relation	partrel;
	TupleTableSlot *remoteslot_part;
	TupleConversionMap *map;
	MemoryContext oldctx;
	LogicalRepRelMapEntry *part_entry = NULL;
	AttrMap    *attrmap = NULL;

	/* ExecFindPartition() 需要 ModifyTableState。      */
	edata->mtstate = mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = operation;
	mtstate->resultRelInfo = relinfo;

	/* PartitionTupleRouting 亦然。  */
	edata->proute = proute = ExecSetupPartitionTupleRouting(estate, parentrel);

	/*
	 * 找到该"搜索元组"所属的分区。              
	 */
	Assert(remoteslot != NULL);
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	partrelinfo = ExecFindPartition(mtstate, relinfo, proute,
									remoteslot, estate);
	Assert(partrelinfo != NULL);
	partrel = partrelinfo->ri_RelationDesc;

	/*
	 * 检查受支持的 relkind。我们需要这项检查，因为分区   
	 * 的 relkind 可能不受支持；且分区集合可能变化，因此  
	 * CREATE/ALTER SUBSCRIPTION 时检查并不足够。 
	 */
	CheckSubscriptionRelkind(partrel->rd_rel->relkind,
							 get_namespace_name(RelationGetNamespace(partrel)),
							 RelationGetRelationName(partrel));

	/*
	 * 为执行下述任一操作，元组必须匹配分区的行类 
	 * 型。如有需要则进行转换，或直接复制，且无论如何 
	 * 都使用专用槽来存储元组。
	 */
	remoteslot_part = partrelinfo->ri_PartitionTupleSlot;
	if (remoteslot_part == NULL)
		remoteslot_part = table_slot_create(partrel, &estate->es_tupleTable);
	map = ExecGetRootToChildMap(partrelinfo, estate);
	if (map != NULL)
	{
		attrmap = map->attrMap;
		remoteslot_part = execute_attr_map_slot(attrmap, remoteslot,
												remoteslot_part);
	}
	else
	{
		remoteslot_part = ExecCopySlot(remoteslot_part, remoteslot);
		slot_getallattrs(remoteslot_part);
	}
	MemoryContextSwitchTo(oldctx);

	/* 检查能否对叶子分区执行更新或删除。            */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		part_entry = logicalrep_partition_open(relmapentry, partrel,
											   attrmap);
		check_relation_updatable(part_entry);
	}

	switch (operation)
	{
		case CMD_INSERT:
			apply_handle_insert_internal(edata, partrelinfo,
										 remoteslot_part);
			break;

		case CMD_DELETE:
			apply_handle_delete_internal(edata, partrelinfo,
										 remoteslot_part,
										 part_entry->localindexoid);
			break;

		case CMD_UPDATE:

			/*
			 * 对 UPDATE，若更新后元组满足分区的约束，  
			 * 则对该分区执行简单的 UPDATE，否则将更新后   
			 * 的元组移动到另一个合适的分区。              
			 *                    
			 */
			{
				TupleTableSlot *localslot;
				ResultRelInfo *partrelinfo_new;
				Relation	partrel_new;
				bool		found;

				/* 从分区中获取匹配的本地元组。       */
				found = FindReplTupleInLocalRel(edata, partrel,
												&part_entry->remoterel,
												part_entry->localindexoid,
												remoteslot_part, &localslot);
				if (!found)
				{
					/*
					 * 要更新的元组无法找到。不做其他事情， 
					 * 仅发出一条日志消息。    
					 *
					 * XXX 是否应将其提升为 ereport(LOG)？        
					 */
					elog(DEBUG1,
						 "logical replication did not find row to be updated "
						 "in replication target relation's partition \"%s\"",
						 RelationGetRelationName(partrel));
					return;
				}

				/*
				 * 把更新应用到本地元组，并将结果放入       
				 * remoteslot_part 
				 */
				oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
				slot_modify_data(remoteslot_part, localslot, part_entry,
								 newtup);
				MemoryContextSwitchTo(oldctx);

				/*
				 * 更新后的元组是否仍满足当前         
				 * 分区的约束？     
				 */
				if (!partrel->rd_rel->relispartition ||
					ExecPartitionCheck(partrelinfo, remoteslot_part, estate,
									   false))
				{
					/*
					 * 是的，因此直接 UPDATE 该分区，不调用 
					 * apply_handle_update_internal()，它会执行   
					 * 以下工作，为避免重复上面已完成的       
					 * 在分区中查找本地元组的那部分            
					 * 工作。 
					 */
					EPQState	epqstate;

					EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1, NIL);
					ExecOpenIndices(partrelinfo, false);

					EvalPlanQualSetSlot(&epqstate, remoteslot_part);
					TargetPrivilegesCheck(partrelinfo->ri_RelationDesc,
										  ACL_UPDATE);
					ExecSimpleRelationUpdate(partrelinfo, estate, &epqstate,
											 localslot, remoteslot_part);
					ExecCloseIndices(partrelinfo);
					EvalPlanQualEnd(&epqstate);
				}
				else
				{
					/* 将元组移动到新分区中。      */

					/*
					 * 新分区将通过元组路由查找，该过程      
					 * 只能经由父表发生。可能需要把该元组   
					 * 转换为父表的行类型。注意，这是        
					 * 在该分区中找到的元组，而不是       
					 * 本函数收到的原始搜索元组。         
					 */
					if (map)
					{
						TupleConversionMap *PartitionToRootMap =
							convert_tuples_by_name(RelationGetDescr(partrel),
												   RelationGetDescr(parentrel));

						remoteslot =
							execute_attr_map_slot(PartitionToRootMap->attrMap,
												  remoteslot_part, remoteslot);
					}
					else
					{
						remoteslot = ExecCopySlot(remoteslot, remoteslot_part);
						slot_getallattrs(remoteslot);
					}

					/* 查找新分区。      */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					partrelinfo_new = ExecFindPartition(mtstate, relinfo,
														proute, remoteslot,
														estate);
					MemoryContextSwitchTo(oldctx);
					Assert(partrelinfo_new != partrelinfo);
					partrel_new = partrelinfo_new->ri_RelationDesc;

					/* 检查新分区是否也具有受支持的 relkind。*/
					CheckSubscriptionRelkind(partrel_new->rd_rel->relkind,
											 get_namespace_name(RelationGetNamespace(partrel_new)),
											 RelationGetRelationName(partrel_new));

					/* DELETE 旧分区中找到的旧元组。     */
					apply_handle_delete_internal(edata, partrelinfo,
												 localslot,
												 part_entry->localindexoid);

					/* INSERT 新元组到新分区中。       */

					/*
					 * 把替换元组转换为匹配目标分区的         
					 * 行类型。      
					 */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					remoteslot_part = partrelinfo_new->ri_PartitionTupleSlot;
					if (remoteslot_part == NULL)
						remoteslot_part = table_slot_create(partrel_new,
															&estate->es_tupleTable);
					map = ExecGetRootToChildMap(partrelinfo_new, estate);
					if (map != NULL)
					{
						remoteslot_part = execute_attr_map_slot(map->attrMap,
																remoteslot,
																remoteslot_part);
					}
					else
					{
						remoteslot_part = ExecCopySlot(remoteslot_part,
													   remoteslot);
						slot_getallattrs(remoteslot);
					}
					MemoryContextSwitchTo(oldctx);
					apply_handle_insert_internal(edata, partrelinfo_new,
												 remoteslot_part);
				}
			}
			break;

		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) operation);
			break;
	}
}

/*
 * 处理 TRUNCATE 消息  
 *
 * TODO: FDW 支持 
 */
static void
apply_handle_truncate(StringInfo s)
{
	bool		cascade = false;
	bool		restart_seqs = false;
	List	   *remote_relids = NIL;
	List	   *remote_rels = NIL;
	List	   *rels = NIL;
	List	   *part_rels = NIL;
	List	   *relids = NIL;
	List	   *relids_logged = NIL;
	ListCell   *lc;
	LOCKMODE	lockmode = AccessExclusiveLock;

	/*
	 * 若正在跳过数据修改变更或正在处理流式事务，      
	 * 则快速返回。    
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_TRUNCATE, s))
		return;

	begin_replication_step();

	remote_relids = logicalrep_read_truncate(s, &cascade, &restart_seqs);

	foreach(lc, remote_relids)
	{
		LogicalRepRelId relid = lfirst_oid(lc);
		LogicalRepRelMapEntry *rel;

		rel = logicalrep_rel_open(relid, lockmode);
		if (!should_apply_changes_for_rel(rel))
		{
			/*
			 * 该关系无法在事务中途变得值得关注，因此 
			 * 解锁它是安全的。              
			 */
			logicalrep_rel_close(rel, lockmode);
			continue;
		}

		remote_rels = lappend(remote_rels, rel);
		TargetPrivilegesCheck(rel->localrel, ACL_TRUNCATE);
		rels = lappend(rels, rel->localrel);
		relids = lappend_oid(relids, rel->localreloid);
		if (RelationIsLogicallyLogged(rel->localrel))
			relids_logged = lappend_oid(relids_logged, rel->localreloid);

		/*
		 * 若收到的消息指示截断分区表，则截断其分区     
		 * 。   
		 */
		if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			ListCell   *child;
			List	   *children = find_all_inheritors(rel->localreloid,
													   lockmode,
													   NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);
				Relation	childrel;

				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors 已持有锁     */
				childrel = table_open(childrelid, NoLock);

				/*
				 * 忽略其他后端的临时表。类似代码参见       
				 * ExecuteTruncate() 
				 */
				if (RELATION_IS_OTHER_TEMP(childrel))
				{
					table_close(childrel, lockmode);
					continue;
				}

				TargetPrivilegesCheck(childrel, ACL_TRUNCATE);
				rels = lappend(rels, childrel);
				part_rels = lappend(part_rels, childrel);
				relids = lappend_oid(relids, childrelid);
				/* 仅在逻辑解码需要时才记录该关系。      */
				if (RelationIsLogicallyLogged(childrel))
					relids_logged = lappend_oid(relids_logged, childrelid);
			}
		}
	}

	/*
	 * 即使我们在上游主库上使用了 CASCADE，也明确默认    
	 * 回放变更时不再进行级联。这一点日后也许可以    
	 * 通过用户指定的选项来改变。 
	 *
	 * MySubscription->runasowner 告诉我们是否要以           
	 * 订阅所有者的身份执行复制操作；TruncateGuts 的      
	 * 最后一个参数则告诉它是否要切换为表所有者。    
	 * 这两个条件恰好相反。        
	 */
	ExecuteTruncateGuts(rels,
						relids,
						relids_logged,
						DROP_RESTRICT,
						restart_seqs,
						!MySubscription->runasowner);
	foreach(lc, remote_rels)
	{
		LogicalRepRelMapEntry *rel = lfirst(lc);

		logicalrep_rel_close(rel, NoLock);
	}
	foreach(lc, part_rels)
	{
		Relation	rel = lfirst(lc);

		table_close(rel, NoLock);
	}

	end_replication_step();
}


/*
 * 逻辑复制协议消息分发器。            
 */
void
apply_dispatch(StringInfo s)
{
	LogicalRepMsgType action = pq_getmsgbyte(s);
	LogicalRepMsgType saved_command;

	/*
	 * 设置正在应用的命令。在应用溢写的变更时，     
	 * 此函数可能被递归调用，因此要保存当前命令      
	 * 。     
	 */
	saved_command = apply_error_callback_arg.command;
	apply_error_callback_arg.command = action;

	switch (action)
	{
		case LOGICAL_REP_MSG_BEGIN:
			apply_handle_begin(s);
			break;

		case LOGICAL_REP_MSG_COMMIT:
			apply_handle_commit(s);
			break;

		case LOGICAL_REP_MSG_INSERT:
			apply_handle_insert(s);
			break;

		case LOGICAL_REP_MSG_UPDATE:
			apply_handle_update(s);
			break;

		case LOGICAL_REP_MSG_DELETE:
			apply_handle_delete(s);
			break;

		case LOGICAL_REP_MSG_TRUNCATE:
			apply_handle_truncate(s);
			break;

		case LOGICAL_REP_MSG_RELATION:
			apply_handle_relation(s);
			break;

		case LOGICAL_REP_MSG_TYPE:
			apply_handle_type(s);
			break;

		case LOGICAL_REP_MSG_ORIGIN:
			apply_handle_origin(s);
			break;

		case LOGICAL_REP_MSG_MESSAGE:

			/*
			 * 逻辑复制尚未使用通用逻辑消息。不过，使用  
			 * 此输出插件的其他应用可能也会用到              
			 * 它。        
			 */
			break;

		case LOGICAL_REP_MSG_STREAM_START:
			apply_handle_stream_start(s);
			break;

		case LOGICAL_REP_MSG_STREAM_STOP:
			apply_handle_stream_stop(s);
			break;

		case LOGICAL_REP_MSG_STREAM_ABORT:
			apply_handle_stream_abort(s);
			break;

		case LOGICAL_REP_MSG_STREAM_COMMIT:
			apply_handle_stream_commit(s);
			break;

		case LOGICAL_REP_MSG_BEGIN_PREPARE:
			apply_handle_begin_prepare(s);
			break;

		case LOGICAL_REP_MSG_PREPARE:
			apply_handle_prepare(s);
			break;

		case LOGICAL_REP_MSG_COMMIT_PREPARED:
			apply_handle_commit_prepared(s);
			break;

		case LOGICAL_REP_MSG_ROLLBACK_PREPARED:
			apply_handle_rollback_prepared(s);
			break;

		case LOGICAL_REP_MSG_STREAM_PREPARE:
			apply_handle_stream_prepare(s);
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid logical replication message type \"??? (%d)\"", action)));
	}

	/* 重置当前命令        */
	apply_error_callback_arg.command = saved_command;
}

/*
 * 计算出应当向 walsender 进程报告哪些写入/刷写位置。     
 *
 * 我们无法简单地回报 walsender 发送给我们的最后            
 * LSN，因为本地事务可能尚未在本地刷写到磁盘。       
 * 作为替代，我们为每次提交构建一个将本地 LSN 与远程   
 * LSN 关联起来的列表。在向发送者回报刷写位置时，     
 * 我们会遍历该列表，检查哪些条目已在本地刷写。        
 * 这些即可报告。  
 *
 * 若存在尚未刷写完的未决事务，则 have_pending_txes         
 * 为真。          
 */
static void
get_flush_position(XLogRecPtr *write, XLogRecPtr *flush,
				   bool *have_pending_txes)
{
	dlist_mutable_iter iter;
	XLogRecPtr	local_flush = GetFlushRecPtr(NULL);

	*write = InvalidXLogRecPtr;
	*flush = InvalidXLogRecPtr;

	dlist_foreach_modify(iter, &lsn_mapping)
	{
		FlushPosition *pos =
			dlist_container(FlushPosition, node, iter.cur);

		*write = pos->remote_end;

		if (pos->local_end <= local_flush)
		{
			*flush = pos->remote_end;
			dlist_delete(iter.cur);
			pfree(pos);
		}
		else
		{
			/*
			 * 不想无谓地遍历列表其余部分，它可能很长。   
			 * 作为替代，直接取最后一个元素并从           
			 * 那里获取写入位置。        
			 */
			pos = dlist_tail_element(FlushPosition, node,
									 &lsn_mapping);
			*write = pos->remote_end;
			*have_pending_txes = true;
			return;
		}
	}

	*have_pending_txes = !dlist_is_empty(&lsn_mapping);
}

/*
 * 把当前的远程/本地 LSN 对存入跟踪列表。   
 */
void
store_flush_position(XLogRecPtr remote_lsn, XLogRecPtr local_lsn)
{
	FlushPosition *flushpos;

	/*
	 * 并行应用工作进程要跳过这一步，因为 lsn_mapping       
	 * 由领导者维护。      
	 */
	if (am_parallel_apply_worker())
		return;

	/* 需在永久上下文中进行。    */
	MemoryContextSwitchTo(ApplyContext);

	/* 跟踪提交 LSN  */
	flushpos = (FlushPosition *) palloc(sizeof(FlushPosition));
	flushpos->local_end = local_lsn;
	flushpos->remote_end = remote_lsn;

	dlist_push_tail(&lsn_mapping, &flushpos->node);
	MemoryContextSwitchTo(ApplyMessageContext);
}


/* 更新工作进程的统计。   */
static void
UpdateWorkerStats(XLogRecPtr last_lsn, TimestampTz send_time, bool reply)
{
	MyLogicalRepWorker->last_lsn = last_lsn;
	MyLogicalRepWorker->last_send_time = send_time;
	MyLogicalRepWorker->last_recv_time = GetCurrentTimestamp();
	if (reply)
	{
		MyLogicalRepWorker->reply_lsn = last_lsn;
		MyLogicalRepWorker->reply_time = send_time;
	}
}

/*
 * 应用主循环 
 */
static void
LogicalRepApplyLoop(XLogRecPtr last_received)
{
	TimestampTz last_recv_timestamp = GetCurrentTimestamp();
	bool		ping_sent = false;
	TimeLineID	tli;
	ErrorContextCallback errcallback;

	/*
	 * 初始化 ApplyMessageContext，它会在每条复制协议          
	 * 消息后清理  
	 */
	ApplyMessageContext = AllocSetContextCreate(ApplyContext,
												"ApplyMessageContext",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * 流式模式启用时，该内存上下文用于每个流的数据。  
	 * 此上下文在每次流停止时重置。            
	 */
	LogicalStreamingContext = AllocSetContextCreate(ApplyContext,
													"LogicalStreamingContext",
													ALLOCSET_DEFAULT_SIZES);

	/* 在开始循环之前标记为空闲  */
	pgstat_report_activity(STATE_IDLE, NULL);

	/*
	 * 压入应用错误上下文回调。应用变更时将填写各        
	 * 字段   
	 */
	errcallback.callback = apply_error_callback;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;
	apply_error_context_stack = error_context_stack;

	/* 外层循环每轮等待迭代一次。 */
	for (;;)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			rc;
		int			len;
		char	   *buf = NULL;
		bool		endofstream = false;
		long		wait_time;

		CHECK_FOR_INTERRUPTS();

		MemoryContextSwitchTo(ApplyMessageContext);

		len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);

		if (len != 0)
		{
			/* 循环处理所有可用数据（不阻塞）。       */
			for (;;)
			{
				CHECK_FOR_INTERRUPTS();

				if (len == 0)
				{
					break;
				}
				else if (len < 0)
				{
					ereport(LOG,
							(errmsg("data stream from publisher has ended")));
					endofstream = true;
					break;
				}
				else
				{
					int			c;
					StringInfoData s;

					if (ConfigReloadPending)
					{
						ConfigReloadPending = false;
						ProcessConfigFile(PGC_SIGHUP);
					}

					/* 重置超时。*/
					last_recv_timestamp = GetCurrentTimestamp();
					ping_sent = false;

					/* 确保将数据读入我们的内存上下文中。     */
					MemoryContextSwitchTo(ApplyMessageContext);

					s.data = buf;
					s.len = len;
					s.cursor = 0;
					s.maxlen = -1;

					c = pq_getmsgbyte(&s);

					if (c == 'w')
					{
						XLogRecPtr	start_lsn;
						XLogRecPtr	end_lsn;
						TimestampTz send_time;

						start_lsn = pq_getmsgint64(&s);
						end_lsn = pq_getmsgint64(&s);
						send_time = pq_getmsgint64(&s);

						if (last_received < start_lsn)
							last_received = start_lsn;

						if (last_received < end_lsn)
							last_received = end_lsn;

						UpdateWorkerStats(last_received, send_time, false);

						apply_dispatch(&s);
					}
					else if (c == 'k')
					{
						XLogRecPtr	end_lsn;
						TimestampTz timestamp;
						bool		reply_requested;

						end_lsn = pq_getmsgint64(&s);
						timestamp = pq_getmsgint64(&s);
						reply_requested = pq_getmsgbyte(&s);

						if (last_received < end_lsn)
							last_received = end_lsn;

						send_feedback(last_received, reply_requested, false);
						UpdateWorkerStats(last_received, timestamp, true);
					}
					/* 其他消息类型被有意忽略            */

					MemoryContextReset(ApplyMessageContext);
				}

				len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);
			}
		}

		/* 确认目前所有写入  */
		send_feedback(last_received, false, false);

		if (!in_remote_transaction && !in_streamed_transaction)
		{
			/*
			 * 若一段时间未收到事务，队列中可能还有      
			 * 未消费的失效消息，现在就将其消费掉        
			 * 。 
			 */
			AcceptInvalidationMessages();
			maybe_reread_subscription();

			/* 处理任何表同步变更。             */
			process_syncing_tables(last_received);
		}

		/* 清理内存。     */
		MemoryContextResetAndDeleteChildren(ApplyMessageContext);
		MemoryContextSwitchTo(TopMemoryContext);

		/* 检查我们是否需要退出流式循环。*/
		if (endofstream)
			break;

		/*
		 * 等待更多数据或锁存器。若有未刷写完的事务， 
		 * 则会在 WalWriterDelay 后唤醒，看看它们是否已被      
		 * 刷写（那样就应发送一条反馈消息）。否则，      
		 * 除非得到数据或信号，否则唤醒并不紧迫       
		 * 。    
		 */
		if (!dlist_is_empty(&lsn_mapping))
			wait_time = WalWriterDelay;
		else
			wait_time = NAPTIME_PER_CYCLE;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							   fd, wait_time,
							   WAIT_EVENT_LOGICAL_APPLY_MAIN);

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

		if (rc & WL_TIMEOUT)
		{
			/*
			 * 我们没有收到任何新内容。若超过               
			 * wal_receiver_timeout / 2 仍无服务器的消息，则向  
			 * 服务器发送 ping。此外，若自上 
			 * 次发送更新已超过 wal_receiver_status_interval，   
			 * 则也要向主库发送状态更新，以报告         
			 * 应用 WAL 的进展。  
			 */
			bool		requestReply = false;

			/*
			 * 检查距上次从主库接收是否已达到配置的上限 
			 * 。              
			 */
			if (wal_receiver_timeout > 0)
			{
				TimestampTz now = GetCurrentTimestamp();
				TimestampTz timeout;

				timeout =
					TimestampTzPlusMilliseconds(last_recv_timestamp,
												wal_receiver_timeout);

				if (now >= timeout)
					ereport(ERROR,
							(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("terminating logical replication worker due to timeout")));

				/* 检查是否该发送 ping 了。     */
				if (!ping_sent)
				{
					timeout = TimestampTzPlusMilliseconds(last_recv_timestamp,
														  (wal_receiver_timeout / 2));
					if (now >= timeout)
					{
						requestReply = true;
						ping_sent = true;
					}
				}
			}

			send_feedback(last_received, requestReply, requestReply);

			/*
			 * 强制报告，以免长时空闲导致统计信息被   
			 * 任意推迟。统计信息只能在（隐式或显式）    
			 * 事务之外报告。这不应导致统计信息被长        
			 * 时间推迟，因为事务要么在提交时整体发送， 
			 * 要么以流式方式发送。流式事务会被溢写到   
			 * 磁盘并在提交时应用。            
			 */
			if (!IsTransactionState())
				pgstat_report_stat(true);
		}
	}

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
	apply_error_context_stack = error_context_stack;

	/* 完成   */
	walrcv_endstreaming(LogRepWorkerWalRcvConn, &tli);
}

/*
 * 向服务器发送备库状态更新消息。  
 *
 * 'recvpos' 是我们已接收到数据的最新 LSN；若需要发送响应 
 * 以避免超时，则设置 force。 
 */
static void
send_feedback(XLogRecPtr recvpos, bool force, bool requestReply)
{
	static StringInfo reply_message = NULL;
	static TimestampTz send_time = 0;

	static XLogRecPtr last_recvpos = InvalidXLogRecPtr;
	static XLogRecPtr last_writepos = InvalidXLogRecPtr;
	static XLogRecPtr last_flushpos = InvalidXLogRecPtr;

	XLogRecPtr	writepos;
	XLogRecPtr	flushpos;
	TimestampTz now;
	bool		have_pending_txes;

	/*
	 * 如果用户不希望向发布者报告状态，请务必在       
	 * 做任何事情之前先退出。         
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* 不传 recvpos 也是合法的   */
	if (recvpos < last_recvpos)
		recvpos = last_recvpos;

	get_flush_position(&writepos, &flushpos, &have_pending_txes);

	/*
	 * 没有待刷写的未决事务，我们可以报告最新              
	 * 的接收位置。这对同步复制很重要。        
	 */
	if (!have_pending_txes)
		flushpos = writepos = recvpos;

	if (writepos < last_writepos)
		writepos = last_writepos;

	if (flushpos < last_flushpos)
		flushpos = last_flushpos;

	now = GetCurrentTimestamp();

	/* 若已报告完所有内容就没问题了。   */
	if (!force &&
		writepos == last_writepos &&
		flushpos == last_flushpos &&
		!TimestampDifferenceExceeds(send_time, now,
									wal_receiver_status_interval * 1000))
		return;
	send_time = now;

	if (!reply_message)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(ApplyContext);

		reply_message = makeStringInfo();
		MemoryContextSwitchTo(oldctx);
	}
	else
		resetStringInfo(reply_message);

	pq_sendbyte(reply_message, 'r');
	pq_sendint64(reply_message, recvpos);	/* 写入*/
	pq_sendint64(reply_message, flushpos);	/* 刷写*/
	pq_sendint64(reply_message, writepos);	/* 应用*/
	pq_sendint64(reply_message, now);	/* sendTime */
	pq_sendbyte(reply_message, requestReply);	/* 请求回复   */

	elog(DEBUG2, "sending feedback (force %d) to recv %X/%X, write %X/%X, flush %X/%X",
		 force,
		 LSN_FORMAT_ARGS(recvpos),
		 LSN_FORMAT_ARGS(writepos),
		 LSN_FORMAT_ARGS(flushpos));

	walrcv_send(LogRepWorkerWalRcvConn,
				reply_message->data, reply_message->len);

	if (recvpos > last_recvpos)
		last_recvpos = recvpos;
	if (writepos > last_writepos)
		last_writepos = writepos;
	if (flushpos > last_flushpos)
		last_flushpos = flushpos;
}

/*
 * 因订阅参数变更而退出应用工作进程的例程。         
 */
static void
apply_worker_exit(void)
{
	if (am_parallel_apply_worker())
	{
		/*
		 * 不要停止并行应用工作进程，因领导者反正稍后   
		 * 会检测到订阅参数变更并重启逻辑复制。这也       
		 * 避免了领导者在尝试与已停止的并行应用工作    
		 * 进程通信时报错；当 disable_on_error 被设置时，这  
		 * 会意外禁用订阅                                           
		 * 。 
		 */
		return;
	}

	/*
	 * 重置此应用工作进程的上次启动时间，以便启动器  
	 * 在订阅仍活动时无需等待 wal_retrieve_retry_interval         
	 * 就会重启它；这也确保在订阅不活动时不会泄漏        
	 * 哈希表条目   
	 */
	if (!am_tablesync_worker())
		ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

	proc_exit(0);
}

/*
 * 若有需要则重读订阅信息。多数变更即退出。  
 */
void
maybe_reread_subscription(void)
{
	MemoryContext oldctx;
	Subscription *newsub;
	bool		started_tx = false;

	/* 当缓存状态有效时，此处无需做任何事。 */
	if (MySubscriptionValid)
		return;

	/* 此函数可能在事务内部或事务外部被调用。       */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/* 确保在永久上下文中分配。     */
	oldctx = MemoryContextSwitchTo(ApplyContext);

	newsub = GetSubscription(MyLogicalRepWorker->subid, true);

	/*
	 * 若订阅已被移除则退出。这通常不应发生，            
	 * 因为 DROP SUBSCRIPTION 会终止工作进程。  
	 */
	if (!newsub)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription \"%s\" will stop because the subscription was removed",
						MySubscription->name)));

		/* 确保移除工作进程启动时间不再有用的条目       */
		if (!am_tablesync_worker() && !am_parallel_apply_worker())
			ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);
		proc_exit(0);
	}

	/* 若订阅已被禁用则退出。      */
	if (!newsub->enabled)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription \"%s\" will stop because the subscription was disabled",
						MySubscription->name)));

		apply_worker_exit();
	}

	/* enabled 为真时绝不应出现 !slotname。       */
	Assert(newsub->slotname);

	/* two-phase 不应被更改       */
	Assert(newsub->twophasestate == MySubscription->twophasestate);

	/*
	 * 若任何影响远程连接的参数发生了变更则退出。      
	 * 启动器会启动一个新的工作进程，但注意并行应      
	 * 用工作进程在 streaming 选项的值从 'parallel' 改为      
	 * 其他值，或服务器决定不流式传输正在进行的         
	 * 事务则不会重启   
	 */
	if (strcmp(newsub->conninfo, MySubscription->conninfo) != 0 ||
		strcmp(newsub->name, MySubscription->name) != 0 ||
		strcmp(newsub->slotname, MySubscription->slotname) != 0 ||
		newsub->binary != MySubscription->binary ||
		newsub->stream != MySubscription->stream ||
		newsub->passwordrequired != MySubscription->passwordrequired ||
		strcmp(newsub->origin, MySubscription->origin) != 0 ||
		newsub->owner != MySubscription->owner ||
		!equal(newsub->publications, MySubscription->publications))
	{
		if (am_parallel_apply_worker())
			ereport(LOG,
					(errmsg("logical replication parallel apply worker for subscription \"%s\" will stop because of a parameter change",
							MySubscription->name)));
		else
			ereport(LOG,
					(errmsg("logical replication worker for subscription \"%s\" will restart because of a parameter change",
							MySubscription->name)));

		apply_worker_exit();
	}

	/* 同时检查其他也绝不该发生的变更。      */
	if (newsub->dbid != MySubscription->dbid)
	{
		elog(ERROR, "subscription %u changed unexpectedly",
			 MyLogicalRepWorker->subid);
	}

	/* 清理旧的订阅信息并切换到新的信息。*/
	FreeSubscription(MySubscription);
	MySubscription = newsub;

	MemoryContextSwitchTo(oldctx);

	/* 根据用户的意愿更改同步提交设置。         */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	if (started_tx)
		CommitTransactionCommand();

	MySubscriptionValid = true;
}

/*
 * 订阅 syscache 失效时的回调。            
 */
static void
subscription_change_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	MySubscriptionValid = false;
}

/*
 * subxact_info_write
 *	  存储顶层事务的子事务相关信息。               
 *
 * 对每个子事务，我们存储其首个变更在主文件中的     
 * 偏移量。文件总是被整体覆写。 
 *
 * XXX 我们应当只存储尚未中止的子事务。        
 */
static void
subxact_info_write(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	Size		len;
	BufFile    *fd;

	Assert(TransactionIdIsValid(xid));

	/* 构造该子事务的文件名 */
	subxact_filename(path, subid, xid);

	/* 若存在则删除子事务文件。 */
	if (subxact_data.nsubxacts == 0)
	{
		cleanup_subxact_info();
		BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, true);

		return;
	}

	/*
	 * 若子事务文件尚未创建则创建它，否则打开已有的   
	 * 文件。     
	 */
	fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path, O_RDWR,
							true);
	if (fd == NULL)
		fd = BufFileCreateFileSet(MyLogicalRepWorker->stream_fileset, path);

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* 写入子事务计数与子事务信息  */
	BufFileWrite(fd, &subxact_data.nsubxacts, sizeof(subxact_data.nsubxacts));
	BufFileWrite(fd, subxact_data.subxacts, len);

	BufFileClose(fd);

	/* 释放为子事务信息分配的内存    */
	cleanup_subxact_info();
}

/*
 * subxact_info_read
 *	  恢复流式事务的子事务相关信息。                
 *
 * 把子事务信息读入 subxact_data 结构中供稍后                   
 * 使用。  
 */
static void
subxact_info_read(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	Size		len;
	BufFile    *fd;
	MemoryContext oldctx;

	Assert(!subxact_data.subxacts);
	Assert(subxact_data.nsubxacts == 0);
	Assert(subxact_data.nsubxacts_max == 0);

	/*
	 * 若子事务文件不存在，则意味着没有任何子事务信息 
	 * 。  
	 */
	subxact_filename(path, subid, xid);
	fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path, O_RDONLY,
							true);
	if (fd == NULL)
		return;

	/* 读取子事务条目数量  */
	BufFileReadExact(fd, &subxact_data.nsubxacts, sizeof(subxact_data.nsubxacts));

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* 我们将最大值保持为 2 的幂*/
	subxact_data.nsubxacts_max = 1 << my_log2(subxact_data.nsubxacts);

	/*
	 * 在逻辑流式上下文中分配子事务信息。在整个流       
	 * 期间都需要这些信息，以便能向其中添加子事务       
	 * 信息。在流停止时，我们会将这些信息刷写到子        
	 * 事务文件中，并重置逻辑流式上下文。         
	 */
	oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
	subxact_data.subxacts = palloc(subxact_data.nsubxacts_max *
								   sizeof(SubXactInfo));
	MemoryContextSwitchTo(oldctx);

	if (len > 0)
		BufFileReadExact(fd, subxact_data.subxacts, len);

	BufFileClose(fd);
}

/*
 * subxact_info_add
 *	  添加子事务的信息（主文件中的偏移量）。 
 */
static void
subxact_info_add(TransactionId xid)
{
	SubXactInfo *subxacts = subxact_data.subxacts;
	int64		i;

	/* 我们必须拥有有效的顶层流 xid 和流 fd。      */
	Assert(TransactionIdIsValid(stream_xid));
	Assert(stream_fd != NULL);

	/*
	 * 若该 XID 与顶层事务匹配，我们就不想添加它。       
	 */
	if (stream_xid == xid)
		return;

	/*
	 * 多数情况下，我们检查的子事务与上次调用中已       
	 * 经见过，故务必忽略它（该变更稍后到来）。       
	 */
	if (subxact_data.subxact_last == xid)
		return;

	/* 好，记住我们正在处理该 XID。*/
	subxact_data.subxact_last = xid;

	/*
	 * 检查该事务是否已存在于子事务数组中。我们           
	 * 有意从尾部开始扫描数组，因为我们最有可能           
	 * 是在为最近的子事务添加变更。   
	 *
	 * XXX 我们能否依赖子事务的 XID 按序到达？这样      
	 * 便可以在此使用二分查找。     
	 */
	for (i = subxact_data.nsubxacts; i > 0; i--)
	{
		/* 已找到，故完成*/
		if (subxacts[i - 1].xid == xid)
			return;
	}

	/* 此为新子事务，需将其加入数组。             */
	if (subxact_data.nsubxacts == 0)
	{
		MemoryContext oldctx;

		subxact_data.nsubxacts_max = 128;

		/*
		 * 在每流上下文中为子事务分配此内存，参见   
		 * subxact_info_read.
		 */
		oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
		subxacts = palloc(subxact_data.nsubxacts_max * sizeof(SubXactInfo));
		MemoryContextSwitchTo(oldctx);
	}
	else if (subxact_data.nsubxacts == subxact_data.nsubxacts_max)
	{
		subxact_data.nsubxacts_max *= 2;
		subxacts = repalloc(subxacts,
							subxact_data.nsubxacts_max * sizeof(SubXactInfo));
	}

	subxacts[subxact_data.nsubxacts].xid = xid;

	/*
	 * 获取流文件的当前偏移量，将其存为该子事务的    
	 * 偏移量。 
	 */
	BufFileTell(stream_fd,
				&subxacts[subxact_data.nsubxacts].fileno,
				&subxacts[subxact_data.nsubxacts].offset);

	subxact_data.nsubxacts++;
	subxact_data.subxacts = subxacts;
}

/* 为包含子事务信息的文件构造文件名            */
static inline void
subxact_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.subxacts", subid, xid);
}

/* 为包含已序列化变更的文件构造文件名    */
static inline void
changes_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.changes", subid, xid);
}

/*
 * stream_cleanup_files
 *	  清理订阅 / 顶层事务的文件。                 
 *
 * 移除特定顶层事务的已序列化变更与子事务             
 * 信息文件。每个订阅对任一顶层事务都有一组       
 * 单独的文件。           
 */
void
stream_cleanup_files(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];

	/* 删除变更文件。    */
	changes_filename(path, subid, xid);
	BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, false);

	/* 删除子事务文件（若存在）。*/
	subxact_filename(path, subid, xid);
	BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, true);
}

/*
 * stream_open_file
 *	  打开一个文件，我们将用它为顶层事务序列化  
 * 变更。   
 *
 * 为某顶层事务的流式变更打开文件，该事务由           
 * stream_xid（全局变量）标识。若这是该事务的          
 * 第一批流式变更，则创建 BufFile，否则打开            
 * 已创建的文件。   
 */
static void
stream_open_file(Oid subid, TransactionId xid, bool first_segment)
{
	char		path[MAXPGPATH];
	MemoryContext oldcxt;

	Assert(OidIsValid(subid));
	Assert(TransactionIdIsValid(xid));
	Assert(stream_fd == NULL);


	changes_filename(path, subid, xid);
	elog(DEBUG1, "opening file \"%s\" for streamed changes", path);

	/*
	 * 在逻辑流式上下文下创建或打开这些 BufFile，以便      
	 * 这些文件保留至流停止。  
	 */
	oldcxt = MemoryContextSwitchTo(LogicalStreamingContext);

	/*
	 * 若这是首个流式段，则创建变更文件。            
	 * 否则，仅以追加模式打开文件写入。          
	 */
	if (first_segment)
		stream_fd = BufFileCreateFileSet(MyLogicalRepWorker->stream_fileset,
										 path);
	else
	{
		/*
		 * 打开文件并定位到文件末尾，因为我们总是      
		 * 追加变更文件。   
		 */
		stream_fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset,
									   path, O_RDWR, false);
		BufFileSeek(stream_fd, 0, 0, SEEK_END);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * stream_close_file
 *	  关闭当前打开的流式变更文件。          
 */
static void
stream_close_file(void)
{
	Assert(stream_fd != NULL);

	BufFileClose(stream_fd);

	stream_fd = NULL;
}

/*
 * stream_write_change
 *	  将当前顶层事务的一条变更序列化到文件。         
 *
 * 变更以简单格式序列化：长度（不含其自身）、        
 * 动作代码（标识消息类型）以及消息内容（不含    
 * 子事务的 TransactionId 值）。               
 */
static void
stream_write_change(char action, StringInfo s)
{
	int			len;

	Assert(stream_fd != NULL);

	/* 磁盘总大小，含动作类型字符在内           */
	len = (s->len - s->cursor) + sizeof(char);

	/* 先写入大小      */
	BufFileWrite(stream_fd, &len, sizeof(len));

	/* 然后写动作 */
	BufFileWrite(stream_fd, &action, sizeof(action));

	/* 最后写入缓冲区的剩余部分（XID 之后）         */
	len = (s->len - s->cursor);

	BufFileWrite(stream_fd, &s->data[s->cursor], len);
}

/*
 * stream_open_and_write_change
 *	  将给定事务的一条消息序列化到文件。     
 *
 * 此函数与 stream_write_change 类似，但它会在写入                
 * 消息前打开目标文件（若尚未打开），最后将其关闭。   
 *         
 */
static void
stream_open_and_write_change(TransactionId xid, char action, StringInfo s)
{
	Assert(!in_streamed_transaction);

	if (!stream_fd)
		stream_start_internal(xid, false);

	stream_write_change(action, s);
	stream_stop_internal(xid);
}

/*
 * 清理子事务的内存并重置相关变量。                
 */
static inline void
cleanup_subxact_info()
{
	if (subxact_data.subxacts)
		pfree(subxact_data.subxacts);

	subxact_data.subxacts = NULL;
	subxact_data.subxact_last = InvalidTransactionId;
	subxact_data.nsubxacts = 0;
	subxact_data.nsubxacts_max = 0;
}

/*
 * 为 two_phase 事务构造已准备事务的 GID。          
 *
 * 在给定缓冲区中返回 GID。    
 */
static void
TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid, int szgid)
{
	Assert(subid != InvalidRepOriginId);

	if (!TransactionIdIsValid(xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid two-phase transaction ID")));

	snprintf(gid, szgid, "pg_gid_%u_%u", subid, xid);
}

/*
 * 执行带错误处理的初始同步。如需要，则禁用           
 * 订阅。        
 *
 * 返回时在长生命周期上下文中分配槽名。注意，我们     
 * 不处理 FATAL 错误，这类错误多半源于系统资源问题且     
 * 不会重复。    
 */
static void
start_table_sync(XLogRecPtr *origin_startpos, char **myslotname)
{
	char	   *syncslotname = NULL;

	Assert(am_tablesync_worker());

	PG_TRY();
	{
		/* 调用初始同步 */
		syncslotname = LogicalRepSyncTableStart(origin_startpos);
	}
	PG_CATCH();
	{
		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * 报告工作进程在表同步期间失败。中止当前   
			 * 事务，以便统计消息在空闲状态下                  
			 * 发送。  
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid, false);

			PG_RE_THROW();
		}
	}
	PG_END_TRY();

	/* 长生命周期上下文中分配槽名  */
	*myslotname = MemoryContextStrdup(ApplyContext, syncslotname);
	pfree(syncslotname);
}

/*
 * 重置复制源状态  
 */
static void
replorigin_reset(int code, Datum arg)
{
	replorigin_session_origin = InvalidRepOriginId;
	replorigin_session_origin_lsn = InvalidXLogRecPtr;
	replorigin_session_origin_timestamp = 0;
}

/*
 * 运行带错误处理的应用循环。必要时禁用           
 * 订阅。    
 *
 * 注意，我们不处理 FATAL 错误，这类错误多半源    
 * 于系统资源问题且不会重复。         
 */
static void
start_apply(XLogRecPtr origin_startpos)
{
	PG_TRY();
	{
		LogicalRepApplyLoop(origin_startpos);
	}
	PG_CATCH();
	{
		/*
		 * 重置复制源状态，以免在应用失败时           
		 * 推进复制源进度。否则，应用失败的后果是   
		 * 事务丢失，因为服务器不会再次发送该事务。   
		 *        
		 */
		replorigin_reset(0, (Datum) 0);

		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * 报告工作进程在应用变更期间失败。中止    
			 * 当前事务，以便统计消息在空闲状态下        
			 * 发送。  
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid, !am_tablesync_worker());

			PG_RE_THROW();
		}
	}
	PG_END_TRY();
}

/*
 * 领导者应用工作进程与并行应用工作进程共用初始化   
 *
 * 初始化数据库连接、内存中订阅与必要的配置            
 * 选项。      
 */
void
InitializeApplyWorker(void)
{
	MemoryContext oldctx;

	/* 以 replica 会话复制角色运行。  */
	SetConfigOption("session_replication_role", "replica",
					PGC_SUSET, PGC_S_OVERRIDE);

	/* 连接到本数据库。 */
	BackgroundWorkerInitializeConnectionByOid(MyLogicalRepWorker->dbid,
											  MyLogicalRepWorker->userid,
											  0);

	/*
	 * 设始终安全搜索路径，使恶意用户无法重定向代码   
	 * （如 pg_index.indexprs）。
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/* 将订阅装载进永久内存上下文中。         */
	ApplyContext = AllocSetContextCreate(TopMemoryContext,
										 "ApplyContext",
										 ALLOCSET_DEFAULT_SIZES);
	StartTransactionCommand();
	oldctx = MemoryContextSwitchTo(ApplyContext);

	/*
	 * 锁定订阅以防其被并发删除，随后重新核实           
	 * 其是否存在。初始化完成后，若订阅被删除，则工作  
	 * 进程将被平稳终止。                             
	 */
	LockSharedObject(SubscriptionRelationId, MyLogicalRepWorker->subid, 0,
					 AccessShareLock);
	MySubscription = GetSubscription(MyLogicalRepWorker->subid, true);
	if (!MySubscription)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription %u will not start because the subscription was removed during startup",
						MyLogicalRepWorker->subid)));

		/* 确保移除工作进程启动时间不再有用的条目       */
		if (!am_tablesync_worker() && !am_parallel_apply_worker())
			ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);
		proc_exit(0);
	}

	MySubscriptionValid = true;
	MemoryContextSwitchTo(oldctx);

	if (!MySubscription->enabled)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription \"%s\" will not start because the subscription was disabled during startup",
						MySubscription->name)));

		apply_worker_exit();
	}

	/* 按用户的意愿设置同步提交。                 */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	/* 让我们及时获知订阅变更。         */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONOID,
								  subscription_change_cb,
								  (Datum) 0);

	if (am_tablesync_worker())
		ereport(LOG,
				(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has started",
						MySubscription->name,
						get_rel_name(MyLogicalRepWorker->relid))));
	else
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" has started",
						MySubscription->name)));

	CommitTransactionCommand();
}

/* 逻辑复制应用工作进程入口点      */
void
ApplyWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);
	char		originname[NAMEDATALEN];
	XLogRecPtr	origin_startpos = InvalidXLogRecPtr;
	char	   *myslotname = NULL;
	WalRcvStreamOptions options;
	int			server_version;

	InitializingApplyWorker = true;

	/* 附着到槽   */
	logicalrep_worker_attach(worker_slot);

	/* 设置信号处理    */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * 目前 walreceiver 进程不需要任何 ResourceOwner，但如果     
	 * 需要，可在此调用 CreateAuxProcessResourceOwner。   
	 */

	/* 统计信息初始化为合理值  */
	MyLogicalRepWorker->last_send_time = MyLogicalRepWorker->last_recv_time =
		MyLogicalRepWorker->reply_time = GetCurrentTimestamp();

	/* 装载 libpq 专用函数         */
	load_file("libpqwalreceiver", false);

	InitializeApplyWorker();

	/*
	 * 注册一个回调，在关闭期间中止任何未完成        
	 * 事务前重置复制源状态（参见 ShutdownPostgres()）。这    
	 * 可以避免对未完成事务推进复制源，否则该事务    
	 * 可能因此丢失，因为这样的事务不会被服务器再次   
	 * 发送。    
	 *
	 * 注意，即便是在设置复制源状态之后放置的 LOG 或      
	 * DEBUG 语句，也可能在提交当前应用操作之前就处理     
	 * 关闭信号。因此在此注册这样的回调很重要。    
	 */
	before_shmem_exit(replorigin_reset, (Datum) 0);

	InitializingApplyWorker = false;

	/* 连接到复制源并开始复制。             */
	elog(DEBUG1, "connecting to publisher using connection string \"%s\"",
		 MySubscription->conninfo);

	if (am_tablesync_worker())
	{
		start_table_sync(&origin_startpos, &myslotname);

		ReplicationOriginNameForLogicalRep(MySubscription->oid,
										   MyLogicalRepWorker->relid,
										   originname,
										   sizeof(originname));
		set_apply_error_context_origin(originname);
	}
	else
	{
		/* 即领导者应用工作进程  */
		RepOriginId originid;
		TimeLineID	startpointTLI;
		char	   *err;
		bool		must_use_password;

		myslotname = MySubscription->slotname;

		/*
		 * 若订阅已启用则本不应发生，但仍需防范         
		 * DDL 缺陷或手工改动系统表。（libpqwalreceiver 在槽为 
		 * NULL 会崩溃。）   
		 */
		if (!myslotname)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("subscription has no replication slot set")));

		/* 设置复制源跟踪。           */
		StartTransactionCommand();
		ReplicationOriginNameForLogicalRep(MySubscription->oid, InvalidOid,
										   originname, sizeof(originname));
		originid = replorigin_by_name(originname, true);
		if (!OidIsValid(originid))
			originid = replorigin_create(originname);
		replorigin_session_setup(originid, 0);
		replorigin_session_origin = originid;
		origin_startpos = replorigin_session_get_progress(false);

		/* 是否必须使用密码？         */
		must_use_password = MySubscription->passwordrequired &&
			!superuser_arg(MySubscription->owner);

		/* 注意 superuser_arg 调用可以访问 DB         */
		CommitTransactionCommand();

		LogRepWorkerWalRcvConn = walrcv_connect(MySubscription->conninfo, true,
												must_use_password,
												MySubscription->name, &err);
		if (LogRepWorkerWalRcvConn == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to the publisher: %s", err)));

		/*
		 * 我们其实并不使用 identify_system 的输出，但它会在 
		 * 上游做一些初始化，所以我们仍然调用它。        
		 */
		(void) walrcv_identify_system(LogRepWorkerWalRcvConn, &startpointTLI);

		set_apply_error_context_origin(originname);
	}

	/*
	 * 为 syscache 设置回调，以便知道订阅关系状态何时发  
	 * 生变化。                    
	 */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONRELMAP,
								  invalidate_syncing_table_states,
								  (Datum) 0);

	/* 构建逻辑复制流式选项。            */
	options.logical = true;
	options.startpoint = origin_startpos;
	options.slotname = myslotname;

	server_version = walrcv_server_version(LogRepWorkerWalRcvConn);
	options.proto.logical.proto_version =
		server_version >= 160000 ? LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM :
		server_version >= 150000 ? LOGICALREP_PROTO_TWOPHASE_VERSION_NUM :
		server_version >= 140000 ? LOGICALREP_PROTO_STREAM_VERSION_NUM :
		LOGICALREP_PROTO_VERSION_NUM;

	options.proto.logical.publication_names = MySubscription->publications;
	options.proto.logical.binary = MySubscription->binary;

	/*
	 * 为 streaming 选项分配合适的选项值：依据                
	 * 'streaming' 模式以及发布者对该模式的支持能力。       
	 */
	if (server_version >= 160000 &&
		MySubscription->stream == LOGICALREP_STREAM_PARALLEL)
	{
		options.proto.logical.streaming_str = "parallel";
		MyLogicalRepWorker->parallel_apply = true;
	}
	else if (server_version >= 140000 &&
			 MySubscription->stream != LOGICALREP_STREAM_OFF)
	{
		options.proto.logical.streaming_str = "on";
		MyLogicalRepWorker->parallel_apply = false;
	}
	else
	{
		options.proto.logical.streaming_str = NULL;
		MyLogicalRepWorker->parallel_apply = false;
	}

	options.proto.logical.twophase = false;
	options.proto.logical.origin = pstrdup(MySubscription->origin);

	if (!am_tablesync_worker())
	{
		/*
		 * 即使用户已要求 two_phase 模式，它仍会保持三态   
		 * PENDING，直到所有表同步都到达 READY 状态。        
		 * 此后才能变为 ENABLED。           
		 *
		 * 注意：若订阅没有表，则将状态保留为            
		 * PENDING，这样 ALTER SUBSCRIPTION ... REFRESH PUBLICATION        
		 * work.
		 */
		if (MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING &&
			AllTablesyncsReady())
		{
			/* 启用 two_phase 开始流式传输。 */
			options.proto.logical.twophase = true;
			walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);

			StartTransactionCommand();

			/*
			 * 更新 pg_subscription 可能涉及 TOAST 表访问，因此 
			 * 确保拥有有效快照。     
			 */
			PushActiveSnapshot(GetTransactionSnapshot());

			UpdateTwoPhaseState(MySubscription->oid, LOGICALREP_TWOPHASE_STATE_ENABLED);
			MySubscription->twophasestate = LOGICALREP_TWOPHASE_STATE_ENABLED;
			PopActiveSnapshot();
			CommitTransactionCommand();
		}
		else
		{
			walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);
		}

		ereport(DEBUG1,
				(errmsg_internal("logical replication apply worker for subscription \"%s\" two_phase is %s",
								 MySubscription->name,
								 MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_DISABLED ? "DISABLED" :
								 MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING ? "PENDING" :
								 MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED ? "ENABLED" :
								 "?")));
	}
	else
	{
		/* 开始常规逻辑流式复制。           */
		walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);
	}

	/* 运行主循环。 */
	start_apply(origin_startpos);

	proc_exit(0);
}

/*
 * 从错误中恢复后，在新事务中禁用订阅，然后       
 * 干净退出。  
 */
static void
DisableSubscriptionAndExit(void)
{
	/*
	 * 发出错误消息，并从错误状态恢复到空闲状态       
	 *      
	 */
	HOLD_INTERRUPTS();

	EmitErrorReport();
	AbortOutOfAnyTransaction();
	FlushErrorState();

	RESUME_INTERRUPTS();

	/* 报告工作进程在表同步或应用期间失败                   */
	pgstat_report_subscription_error(MyLogicalRepWorker->subid,
									 !am_tablesync_worker());

	/* 禁用订阅             */
	StartTransactionCommand();

	/*
	 * 更新 pg_subscription 可能涉及 TOAST 表访问，因此确保     
	 * 拥有有效快照。 
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	DisableSubscription(MySubscription->oid);
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* 确保移除工作进程启动时间不再有用的条目       */
	if (!am_tablesync_worker() && !am_parallel_apply_worker())
		ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

	/* 通知订阅已被禁用并退出                  */
	ereport(LOG,
			errmsg("subscription \"%s\" has been disabled because of an error",
				   MySubscription->name));

	proc_exit(0);
}

/*
 * 本进程是否为逻辑复制工作进程？   
 */
bool
IsLogicalWorker(void)
{
	return MyLogicalRepWorker != NULL;
}

/*
 * 本进程是否为逻辑复制并行应用工作进程？      
 */
bool
IsLogicalParallelApplyWorker(void)
{
	return IsLogicalWorker() && am_parallel_apply_worker();
}

/*
 * 若给定 LSN 与订阅的 skiplsn 所指定的 LSN 匹配，则开   
 * 始跳过该事务的变更。          
 */
static void
maybe_start_skipping_changes(XLogRecPtr finish_lsn)
{
	Assert(!is_skipping_changes());
	Assert(!in_remote_transaction);
	Assert(!in_streamed_transaction);

	/*
	 * 若未要求跳过该事务，则快速返回。本函数会     
	 * 为每个远端事务调用，我们假定跳过事务并         
	 * 不常用。                               
	 */
	if (likely(XLogRecPtrIsInvalid(MySubscription->skiplsn) ||
			   MySubscription->skiplsn != finish_lsn))
		return;

	/* 开始跳过该事务的全部变更           */
	skip_xact_finish_lsn = finish_lsn;

	ereport(LOG,
			errmsg("logical replication starts skipping transaction at LSN %X/%X",
				   LSN_FORMAT_ARGS(skip_xact_finish_lsn)));
}

/*
 * 若启用，重置 skip_xact_finish_lsn 以停止跳过变更。   
 */
static void
stop_skipping_changes(void)
{
	if (!is_skipping_changes())
		return;

	ereport(LOG,
			(errmsg("logical replication completed skipping transaction at LSN %X/%X",
					LSN_FORMAT_ARGS(skip_xact_finish_lsn))));

	/* 停止跳过变更    */
	skip_xact_finish_lsn = InvalidXLogRecPtr;
}

/*
 * 清除 pg_subscription 的 subskiplsn。    
 *
 * finish_lsn 是事务的结束 LSN，用于检查 subskiplsn 是否     
 * 与其匹配。若不匹配，我们在清除 subskiplsn 时会发出      
 * 警告，以便用户知悉诸如用户误设了错误的                    
 * subskiplsn 这类情形。     
 */
static void
clear_subscription_skip_lsn(XLogRecPtr finish_lsn)
{
	Relation	rel;
	Form_pg_subscription subform;
	HeapTuple	tup;
	XLogRecPtr	myskiplsn = MySubscription->skiplsn;
	bool		started_tx = false;

	if (likely(XLogRecPtrIsInvalid(myskiplsn)) || am_parallel_apply_worker())
		return;

	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/*
	 * 更新 pg_subscription 可能涉及 TOAST 表访问，因此确保     
	 * 拥有有效快照。 
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * 防止 pg_subscription 的 subskiplsn 在清除期间被             
	 * 并发更新。   
	 */
	LockSharedObject(SubscriptionRelationId, MySubscription->oid, 0,
					 AccessShareLock);

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	/* 获取既有元组。     */
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID,
							  ObjectIdGetDatum(MySubscription->oid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "subscription \"%s\" does not exist", MySubscription->name);

	subform = (Form_pg_subscription) GETSTRUCT(tup);

	/*
	 * 清除 subskiplsn。若用户在清除之前已改过 subskiplsn，    
	 * 则不会更新系统表，复制源状态也不会推进。      
	 * 因此最坏情况下，若服务器在发送刷盘位置的确认   
	 * 之前崩溃，事务会被再次发送，用户需要重新设置    
	 * subskiplsn。我们可以通过记录一条复制源 WAL 记录来   
	 * 推进复制源 LSN 来降低这种可能性，但无法推进      
	 * 复制源时间戳，为此做些什么似乎并不值得，     
	 * 因为这是一种非常罕见的                                    
	 * 情况。                      
	 */
	if (subform->subskiplsn == myskiplsn)
	{
		bool		nulls[Natts_pg_subscription];
		bool		replaces[Natts_pg_subscription];
		Datum		values[Natts_pg_subscription];

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(replaces));

		/* 重置 subskiplsn*/
		values[Anum_pg_subscription_subskiplsn - 1] = LSNGetDatum(InvalidXLogRecPtr);
		replaces[Anum_pg_subscription_subskiplsn - 1] = true;

		tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
								replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		if (myskiplsn != finish_lsn)
			ereport(WARNING,
					errmsg("skip-LSN of subscription \"%s\" cleared", MySubscription->name),
					errdetail("Remote transaction's finish WAL location (LSN) %X/%X did not match skip-LSN %X/%X.",
							  LSN_FORMAT_ARGS(finish_lsn),
							  LSN_FORMAT_ARGS(myskiplsn)));
	}

	heap_freetuple(tup);
	table_close(rel, NoLock);

	PopActiveSnapshot();

	if (started_tx)
		CommitTransactionCommand();
}

/* 错误回调，提供有关正在应用的变更的更多上下文信息*/
void
apply_error_callback(void *arg)
{
	ApplyErrorCallbackArg *errarg = &apply_error_callback_arg;

	if (apply_error_callback_arg.command == 0)
		return;

	Assert(errarg->origin_name);

	if (errarg->rel == NULL)
	{
		if (!TransactionIdIsValid(errarg->remote_xid))
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\"",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command));
		else if (XLogRecPtrIsInvalid(errarg->finish_lsn))
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" in transaction %u",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command),
					   errarg->remote_xid);
		else
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" in transaction %u, finished at %X/%X",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command),
					   errarg->remote_xid,
					   LSN_FORMAT_ARGS(errarg->finish_lsn));
	}
	else
	{
		if (errarg->remote_attnum < 0)
		{
			if (XLogRecPtrIsInvalid(errarg->finish_lsn))
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" in transaction %u",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->remote_xid);
			else
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" in transaction %u, finished at %X/%X",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->remote_xid,
						   LSN_FORMAT_ARGS(errarg->finish_lsn));
		}
		else
		{
			if (XLogRecPtrIsInvalid(errarg->finish_lsn))
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" column \"%s\" in transaction %u",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->rel->remoterel.attnames[errarg->remote_attnum],
						   errarg->remote_xid);
			else
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" column \"%s\" in transaction %u, finished at %X/%X",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->rel->remoterel.attnames[errarg->remote_attnum],
						   errarg->remote_xid,
						   LSN_FORMAT_ARGS(errarg->finish_lsn));
		}
	}
}

/* 设置应用错误回调的事务信息             */
static inline void
set_apply_error_context_xact(TransactionId xid, XLogRecPtr lsn)
{
	apply_error_callback_arg.remote_xid = xid;
	apply_error_callback_arg.finish_lsn = lsn;
}

/* 重置应用错误回调的全部信息       */
static inline void
reset_apply_error_context_info(void)
{
	apply_error_callback_arg.command = 0;
	apply_error_callback_arg.rel = NULL;
	apply_error_callback_arg.remote_attnum = -1;
	set_apply_error_context_xact(InvalidTransactionId, InvalidXLogRecPtr);
}

/*
 * 请求在当前事务提交时唤醒给定订阅 OID 的    
 * 各工作进程。                   
 *
 * 此举用于确保各工作进程尽快处理各类             
 * 变更。           
 */
void
LogicalRepWorkersWakeupAtCommit(Oid subid)
{
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	on_commit_wakeup_workers_subids =
		list_append_unique_oid(on_commit_wakeup_workers_subids, subid);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * 唤醒本事务中发生过变更的任何订阅的工作进程。      
 */
void
AtEOXact_LogicalRepWorkers(bool isCommit)
{
	if (isCommit && on_commit_wakeup_workers_subids != NIL)
	{
		ListCell   *lc;

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		foreach(lc, on_commit_wakeup_workers_subids)
		{
			Oid			subid = lfirst_oid(lc);
			List	   *workers;
			ListCell   *lc2;

			workers = logicalrep_workers_find(subid, true);
			foreach(lc2, workers)
			{
				LogicalRepWorker *worker = (LogicalRepWorker *) lfirst(lc2);

				logicalrep_worker_wakeup_ptr(worker);
			}
		}
		LWLockRelease(LogicalRepWorkerLock);
	}

	/* List 存储会在事务清理时被自动回收。                */
	on_commit_wakeup_workers_subids = NIL;
}

/*
 * 为错误上下文消息，在长生命周期上下文分配复制源名 
 */
void
set_apply_error_context_origin(char *originname)
{
	apply_error_callback_arg.origin_name = MemoryContextStrdup(ApplyContext,
															   originname);
}

/*
 * 返回给定事务要采取的行动。各项行动的      
 * 信息参见 TransApplyAction。                        
 *
 * *当领导者应用工作进程需要把事务的全部变更交给并行 
 * 应用工作进程时，*winfo 会被设为目标并行工作进程   
 * 信息。    
 */
static TransApplyAction
get_transaction_apply_action(TransactionId xid, ParallelApplyWorkerInfo **winfo)
{
	*winfo = NULL;

	if (am_parallel_apply_worker())
	{
		return TRANS_PARALLEL_APPLY;
	}

	/*
	 * 若我们用并行应用工作进程处理该事务，则要么    
	 * 把变更发送给并行工作进程，若该工作进程繁忙，     
	 * 则把变更序列化到文件，稍后再由并行工作进程   
	 * 加以处理。                  
	 */
	*winfo = pa_find_worker(xid);

	if (*winfo && (*winfo)->serialize_changes)
	{
		return TRANS_LEADER_PARTIAL_SERIALIZE;
	}
	else if (*winfo)
	{
		return TRANS_LEADER_SEND_TO_PARALLEL;
	}

	/*
	 * 若没有并行工作进程参与处理此事务，则要么直接 
	 * 应用该变更，要么将其序列化到文件，稍后         
	 * 会在事务结束消息得到处理时再                        
	 * 应用。 
	 */
	else if (in_streamed_transaction)
	{
		return TRANS_LEADER_SERIALIZE;
	}
	else
	{
		return TRANS_LEADER_APPLY;
	}
}
