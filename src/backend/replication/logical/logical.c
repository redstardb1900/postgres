/*-------------------------------------------------------------------------
 * logical.c
 *	   PostgreSQL 逻辑解码协同           
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/logical.c
 *
 * NOTES
 *	  本文件协调逻辑解码的各个组成模块之间的交互，
 *	  主要是通过提供所谓的                              
 *	  LogicalDecodingContext。目标是把逻辑解码的大部分         
 *	  内部复杂性封装起来，使用方只需少量代码即可   
 *	  创建并消费变更流。内置使用方是 walsender 和 SQL SRF 
 *	  接口，但也可以在不改动核心代码的情况下增加更     
 *	  多使用方，例如在                                               
 *	  bgworker。
 *
 *	  其思路是：使用方提供三个回调，一个用于读取 WAL，
 *	  一个用于准备数据写入，最后一个则负责实际写入。  
 *	  具体实现取决于使用方的类型。参见            
 *	  logicalfuncs.c，其中有相当简单的使用方示例，以及       
 *	  一个 WAL 读取回调的示例实现，该回调适用于        
 *	  简单使用方  
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog_internal.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/origin.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/* errcontext 回调所用数据*/
typedef struct LogicalErrorCallbackState
{
	LogicalDecodingContext *ctx;
	const char *callback_name;
	XLogRecPtr	report_location;
} LogicalErrorCallbackState;

/* 输出插件回调的包装器          */
static void output_plugin_error_callback(void *arg);
static void startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
							   bool is_init);
static void shutdown_cb_wrapper(LogicalDecodingContext *ctx);
static void begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn);
static void commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							  XLogRecPtr commit_lsn);
static void begin_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn);
static void prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr prepare_lsn);
static void commit_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									   XLogRecPtr commit_lsn);
static void rollback_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
										 XLogRecPtr prepare_end_lsn, TimestampTz prepare_time);
static void change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							  Relation relation, ReorderBufferChange *change);
static void truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
								int nrelations, Relation relations[], ReorderBufferChange *change);
static void message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr message_lsn, bool transactional,
							   const char *prefix, Size message_size, const char *message);

/* 流式回调        */
static void stream_start_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									XLogRecPtr first_lsn);
static void stream_stop_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
								   XLogRecPtr last_lsn);
static void stream_abort_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									XLogRecPtr abort_lsn);
static void stream_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									  XLogRecPtr prepare_lsn);
static void stream_commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									 XLogRecPtr commit_lsn);
static void stream_change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									 Relation relation, ReorderBufferChange *change);
static void stream_message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									  XLogRecPtr message_lsn, bool transactional,
									  const char *prefix, Size message_size, const char *message);
static void stream_truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									   int nrelations, Relation relations[], ReorderBufferChange *change);

/* 更新事务进度的回调       */
static void update_progress_txn_cb_wrapper(ReorderBuffer *cache,
										   ReorderBufferTXN *txn,
										   XLogRecPtr lsn);

static void LoadOutputPlugin(OutputPluginCallbacks *callbacks, const char *plugin);

/*
 * 确保当前设置与环境具备执行逻辑                            
 * 解码。
 */
void
CheckLogicalDecodingRequirements(void)
{
	CheckSlotRequirements();

	/*
	 * 注意：新增一项要求很可能意味着 RestoreSlotFromDisk() 
	 * 也需同样检查。
	 */

	if (wal_level < WAL_LEVEL_LOGICAL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires wal_level >= logical")));

	if (MyDatabaseId == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires a database connection")));

	if (RecoveryInProgress())
	{
		/*
		 * 此检查可能存在竞态条件，但每当    
		 * XLOG_PARAMETER_CHANGE 表明 wal_level 已改变时，我们  
		 * 都会验证不存在已有的逻辑复制槽。另外，为避免 
		 * 创建新复制槽时的竞态，      
		 * CheckLogicalDecodingRequirements() 在创建复制槽之前      
		 * 调用一次，并在逻辑解码最初启动时再调用一次。
		 */
		if (GetActiveWalLevelOnStandby() < WAL_LEVEL_LOGICAL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical decoding on standby requires wal_level >= logical on the primary")));
	}
}

/*
 * 辅助函数，供 CreateInitDecodingContext() 与 
 * CreateDecodingContext() 执行公共任务。   
 */
static LogicalDecodingContext *
StartupDecodingContext(List *output_plugin_options,
					   XLogRecPtr start_lsn,
					   TransactionId xmin_horizon,
					   bool need_full_snapshot,
					   bool fast_forward,
					   bool in_create,
					   XLogReaderRoutine *xl_routine,
					   LogicalOutputPluginWriterPrepareWrite prepare_write,
					   LogicalOutputPluginWriterWrite do_write,
					   LogicalOutputPluginWriterUpdateProgress update_progress)
{
	ReplicationSlot *slot;
	MemoryContext context,
				old_context;
	LogicalDecodingContext *ctx;

	/* 较短行……  */
	slot = MyReplicationSlot;

	context = AllocSetContextCreate(CurrentMemoryContext,
									"Logical decoding context",
									ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(context);
	ctx = palloc0(sizeof(LogicalDecodingContext));

	ctx->context = context;

	/*
	 * （重新）加载输出插件，以发现已移除的失效插件  
	 * 。 
	 */
	if (!fast_forward)
		LoadOutputPlugin(&ctx->callbacks, NameStr(slot->data.plugin));

	/*
	 * 既然复制槽的 xmin 已设置，我们就可以把自己登记为
	 * 逻辑解码后端，在计算 xmin 视界时无需被逐个检查， 
	 * 因为 xmin 的强制执行依赖                               
	 * 复制槽。      
	 *
	 * 只有处于事务之外时才能这样做（即通过 walsender 流   
	 * 式发送变更的场景），否则先前已设置好的   
	 * 快照/xid 最终会被忽略。这并不是一个特别          
	 * 麻烦的限制，因为 SQL 接口反正也无法用于流式  
	 * 发送。        
	 */
	if (!IsTransactionOrTransactionBlock())
	{
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_IN_LOGICAL_DECODING;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	ctx->slot = slot;

	ctx->reader = XLogReaderAllocate(wal_segment_size, NULL, xl_routine, ctx);
	if (!ctx->reader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	ctx->reorder = ReorderBufferAllocate();
	ctx->snapshot_builder =
		AllocateSnapshotBuilder(ctx->reorder, xmin_horizon, start_lsn,
								need_full_snapshot, slot->data.two_phase_at);

	ctx->reorder->private_data = ctx;

	/* 包装输出插件回调，以便添加错误上下文信息          */
	ctx->reorder->begin = begin_cb_wrapper;
	ctx->reorder->apply_change = change_cb_wrapper;
	ctx->reorder->apply_truncate = truncate_cb_wrapper;
	ctx->reorder->commit = commit_cb_wrapper;
	ctx->reorder->message = message_cb_wrapper;

	/*
	 * 为支持流式，我们需要 start/stop/abort/commit/change  
	 * 回调。message/truncate 回调可选，类似常规输出插件，  
	 * 一样。但我们会在至少一个方法被启用时启用流式，
	 * 从而能轻松识别出缺少的                               
	 * 方法  
	 *
	 * 我们在此决定，但稍后才在包装器中检查。  
	 */
	ctx->streaming = (ctx->callbacks.stream_start_cb != NULL) ||
		(ctx->callbacks.stream_stop_cb != NULL) ||
		(ctx->callbacks.stream_abort_cb != NULL) ||
		(ctx->callbacks.stream_commit_cb != NULL) ||
		(ctx->callbacks.stream_change_cb != NULL) ||
		(ctx->callbacks.stream_message_cb != NULL) ||
		(ctx->callbacks.stream_truncate_cb != NULL);

	/*
	 * 流式回调       
	 *
	 * stream_message 与 stream_truncate 回调是可选的，缺少时      
	 * 不会以 ERROR 失败，包装器只会什么都不做。我们    
	 * 必须为 ReorderBuffer 回调设置某种值，否则来自那里    
	 * 的调用会崩溃（我们不想把检查挪到那里）。   
	 */
	ctx->reorder->stream_start = stream_start_cb_wrapper;
	ctx->reorder->stream_stop = stream_stop_cb_wrapper;
	ctx->reorder->stream_abort = stream_abort_cb_wrapper;
	ctx->reorder->stream_prepare = stream_prepare_cb_wrapper;
	ctx->reorder->stream_commit = stream_commit_cb_wrapper;
	ctx->reorder->stream_change = stream_change_cb_wrapper;
	ctx->reorder->stream_message = stream_message_cb_wrapper;
	ctx->reorder->stream_truncate = stream_truncate_cb_wrapper;


	/*
	 * 为支持两阶段逻辑解码，我们需要    
	 * begin_prepare/prepare/commit-prepare/abort-prepare 回调。     
	 * filter_prepare 回调可选。但只要启用其中一个方法 
	 * 我们就启用两阶段逻辑解码，以便能轻松识别出缺少  
	 * 的方法。                        
	 *
	 * 我们在此决定，但稍后才在包装器中检查。  
	 */
	ctx->twophase = (ctx->callbacks.begin_prepare_cb != NULL) ||
		(ctx->callbacks.prepare_cb != NULL) ||
		(ctx->callbacks.commit_prepared_cb != NULL) ||
		(ctx->callbacks.rollback_prepared_cb != NULL) ||
		(ctx->callbacks.stream_prepare_cb != NULL) ||
		(ctx->callbacks.filter_prepare_cb != NULL);

	/*
	 * 在 PREPARE 阶段支持解码的回调。   
	 */
	ctx->reorder->begin_prepare = begin_prepare_cb_wrapper;
	ctx->reorder->prepare = prepare_cb_wrapper;
	ctx->reorder->commit_prepared = commit_prepared_cb_wrapper;
	ctx->reorder->rollback_prepared = rollback_prepared_cb_wrapper;

	/*
	 * 在向输出插件发送事务（含子事务）数据期间  
	 * 支持更新进度。                                      
	 */
	ctx->reorder->update_progress_txn = update_progress_txn_cb_wrapper;

	ctx->out = makeStringInfo();
	ctx->prepare_write = prepare_write;
	ctx->write = do_write;
	ctx->update_progress = update_progress;

	ctx->output_plugin_options = output_plugin_options;

	ctx->fast_forward = fast_forward;

	ctx->in_create = in_create;

	MemoryContextSwitchTo(old_context);

	return ctx;
}

/*
 * 为新的逻辑复制槽新建解码上下文。      
 *
 * plugin -- 包含输出插件的名称           
 * output_plugin_options -- 包含传给输出插件的选项           
 * need_full_snapshot -- 为真时必须获取能读取所有表的      
 *		快照；为假时，可接受只能读取系统表的快照。 
 * restart_lsn -- 若传入无效值，本例程有责任为复制槽设    
 *		置一个合适的 restart_lsn 来把 WAL 标记为已保留。       
 *		否则，我们把解码设置为从给定 LSN 开始，不预先 
 *		WAL 标记为保留。在这种场景下，需由调用者保证  
 *		WAL 依然可用。                            
 * xl_routine -- 底层 XLogReader 的 XLogReaderRoutine    
 * prepare_write, do_write, update_progress --
 *		执行与具体用例相关的实际工作的回调。      
 *
 * 必须在存活期至少与解码上下文一样长的内存上下        
 * 文中调用，因为解码上下文内还会创建更多内存上     
 * 下文。 
 *
 * 调用输出插件的启动函数后，返回一个已初始化的解    
 * 码上下文。  
 */
LogicalDecodingContext *
CreateInitDecodingContext(const char *plugin,
						  List *output_plugin_options,
						  bool need_full_snapshot,
						  XLogRecPtr restart_lsn,
						  XLogReaderRoutine *xl_routine,
						  LogicalOutputPluginWriterPrepareWrite prepare_write,
						  LogicalOutputPluginWriterWrite do_write,
						  LogicalOutputPluginWriterUpdateProgress update_progress)
{
	TransactionId xmin_horizon = InvalidTransactionId;
	ReplicationSlot *slot;
	NameData	plugin_name;
	LogicalDecodingContext *ctx;
	MemoryContext old_context;

	/*
	 * 在备库上，创建复制槽时也需要此项检查。         
	 * 请参见该函数内的注释。  
	 */
	CheckLogicalDecodingRequirements();

	/* 较短行……  */
	slot = MyReplicationSlot;

	/* 先做一些不太可能被违反的健全性检查       */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	if (plugin == NULL)
		elog(ERROR, "cannot initialize logical decoding without a specified plugin");

	/* 确保传入的复制槽合适。这些是面向用户的错误。   */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot use physical replication slot for logical decoding")));

	if (slot->data.database != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" was not created in this database",
						NameStr(slot->data.name))));

	if (IsTransactionState() &&
		GetTopTransactionIdIfAny() != InvalidTransactionId)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot create logical replication slot in transaction that has performed writes")));

	/*
	 * 将输出插件名登记到槽上。需用互斥锁避免         
	 * 并发读部分拷贝的字符串。但不想在持有自旋锁期间  
	 * 出现复杂代码，因此把 namestrcpy() 放到外面执行。     
	 */
	namestrcpy(&plugin_name, plugin);
	SpinLockAcquire(&slot->mutex);
	slot->data.plugin = plugin_name;
	SpinLockRelease(&slot->mutex);

	if (XLogRecPtrIsInvalid(restart_lsn))
		ReplicationSlotReserveWal();
	else
	{
		SpinLockAcquire(&slot->mutex);
		slot->data.restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);
	}

	/* ----
	 * 这里有点棘手：我们需要确定一个用于开始解码的     
	 * 安全 xmin 视界，以免从一条引用了已被清理或修剪    
	 * 过行的运行中事务记录启动解码。  
	 * GetOldestSafeDecodingTransactionId() 会返回这样的值，          
	 * 但若无进一步联锁，其返回值可能立即过                
	 * 期  
	 *
	 * 因此我们必须获取 ProcArrayLock 以阻止其他后端计算   
	 * 出新的 xmin 视界，拿到安全解码 xid，并把新的限制   
	 * 告知复制槽机制。一旦完成，                     
	 * 就能释放 ProcArrayLock，因为复制槽机制现在  
	 * 已在防止清理了。  
	 *
	 * 注意，若要导出数据快照，就必须暂时预留数据（而 
	 * 不仅系统表）的 xmin。否则此处创建的初始数据快照 
	 * 并不保证有效。此后数据 xmin 就不再需要管理，     
	 * 而应重新计算全局 xmin。由于我们能够接受在崩溃后
	 * 损失被钉住的 xmin——反正不会再导出快照——我们 
	 * 就可以只设置复制槽的                                        
	 * effective_xmin 即可。             
	 * ReplicationSlotRelease 之后会再次将其重置。      
	 *
	 * ----
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	xmin_horizon = GetOldestSafeDecodingTransactionId(!need_full_snapshot);

	SpinLockAcquire(&slot->mutex);
	slot->effective_catalog_xmin = xmin_horizon;
	slot->data.catalog_xmin = xmin_horizon;
	if (need_full_snapshot)
		slot->effective_xmin = xmin_horizon;
	SpinLockRelease(&slot->mutex);

	ReplicationSlotsComputeRequiredXmin(true);

	LWLockRelease(ProcArrayLock);

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();

	ctx = StartupDecodingContext(NIL, restart_lsn, xmin_horizon,
								 need_full_snapshot, false, true,
								 xl_routine, prepare_write, do_write,
								 update_progress);

	/* 调用输出插件初始化回调          */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, true);
	MemoryContextSwitchTo(old_context);

	/*
	 * 当 two_phase 在创建槽时启用，或在流式启动时     
	 * 给出 two_phase 选项时，我们允许解码已准备的事务， 
	 * 前提是插件支持全部两阶段                              
	 * 回调。               
	 */
	ctx->twophase &= slot->data.two_phase;

	ctx->reorder->output_rewrites = ctx->options.receive_rewrites;

	return ctx;
}

/*
 * 为先前已被使用过的逻辑复制槽创建一个新的解码上     
 * 下文。    
 *
 * start_lsn
 *		开始解码的 LSN。若为 InvalidXLogRecPtr，则从复制槽  
 *		的 confirmed_flush 重新开始；否则从指定位置开始（但
 *		若更旧则前移到 confirmed_flush，参见                      
 *		下文）。     
 *
 * output_plugin_options
 *		传给输出插件的选项。      
 *
 * fast_forward
 *		绕过逻辑变更的生成。           
 *
 * xl_routine
 *		底层 xlogreader 所用的 XLogReaderRoutine  
 *
 * prepare_write, do_write, update_progress
 *		必须填充的回调，用于执行与用例相关的实际       
 *		工作。   
 *
 * 必须在存活期至少与解码上下文一样长的内存上下        
 * 文中调用，因为其中还会创建更多内存上                 
 * 下文。 
 *
 * 调用输出插件的启动函数后，返回一个已初始化的解    
 * 码上下文。  
 */
LogicalDecodingContext *
CreateDecodingContext(XLogRecPtr start_lsn,
					  List *output_plugin_options,
					  bool fast_forward,
					  XLogReaderRoutine *xl_routine,
					  LogicalOutputPluginWriterPrepareWrite prepare_write,
					  LogicalOutputPluginWriterWrite do_write,
					  LogicalOutputPluginWriterUpdateProgress update_progress)
{
	LogicalDecodingContext *ctx;
	ReplicationSlot *slot;
	MemoryContext old_context;

	/* 较短行……  */
	slot = MyReplicationSlot;

	/* 先做一些不太可能被违反的健全性检查       */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	/* 确保传入的复制槽合适，这些是面向用户的错误     */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot use physical replication slot for logical decoding")));

	if (slot->data.database != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" was not created in this database",
						NameStr(slot->data.name))));

	/*
	 * 检查复制槽是否已因 max_slot_wal_keep_size 而失效。        
	 * 避免在此 errmsg 中使用"cannot get changes"措辞，   
	 * 因为在从 pg_logical_slot_get_changes_guts() 调用时，           
	 * 会令人困惑地暗示无变更  
	 */
	if (MyReplicationSlot->data.invalidated == RS_INVAL_WAL_REMOVED)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("can no longer get changes from replication slot \"%s\"",
						NameStr(MyReplicationSlot->data.name)),
				 errdetail("This slot has been invalidated because it exceeded the maximum reserved size.")));

	if (MyReplicationSlot->data.invalidated != RS_INVAL_NONE)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("can no longer get changes from replication slot \"%s\"",
						NameStr(MyReplicationSlot->data.name)),
				 errdetail("This slot has been invalidated because it was conflicting with recovery.")));

	Assert(MyReplicationSlot->data.invalidated == RS_INVAL_NONE);
	Assert(MyReplicationSlot->data.restart_lsn != InvalidXLogRecPtr);

	if (start_lsn == InvalidXLogRecPtr)
	{
		/* 从上次位置继续       */
		start_lsn = slot->data.confirmed_flush;
	}
	else if (start_lsn < slot->data.confirmed_flush)
	{
		/*
		 * 看似应当报错，但其实客户端确认一个无须为 
		 * 之做任何事的 LSN 相当常见——相关 xlog 记录没有 
		 * 产生对逻辑解码有意义的内容，因而没有持久    
		 * 保存它。客户端必须能够这样做，才           
		 * 能支持同步复                                                 
		 * 制。      
		 *
		 * 从与请求不同的 LSN 开始可能无法捕获某些类型    
		 * 的客户端错误；因此客户端可能希望检查      
		 * confirmed_flush_lsn 是否符合其预期。 
		 */
		elog(LOG, "%X/%X has been already streamed, forwarding to %X/%X",
			 LSN_FORMAT_ARGS(start_lsn),
			 LSN_FORMAT_ARGS(slot->data.confirmed_flush));

		start_lsn = slot->data.confirmed_flush;
	}

	ctx = StartupDecodingContext(output_plugin_options,
								 start_lsn, InvalidTransactionId, false,
								 fast_forward, false, xl_routine, prepare_write,
								 do_write, update_progress);

	/* 调用输出插件初始化回调          */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, false);
	MemoryContextSwitchTo(old_context);

	/*
	 * 当 two_phase 在创建槽时启用，或在流式启动时     
	 * 给出 two_phase 选项时，我们允许解码已准备的事务， 
	 * 前提是插件支持全部两阶段                              
	 * 回调。               
	 */
	ctx->twophase &= (slot->data.two_phase || ctx->twophase_opt_given);

	/* 若未标记则标记复制槽允许 two_phase 解码       */
	if (ctx->twophase && !slot->data.two_phase)
	{
		SpinLockAcquire(&slot->mutex);
		slot->data.two_phase = true;
		slot->data.two_phase_at = start_lsn;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
		SnapBuildSetTwoPhaseAt(ctx->snapshot_builder, start_lsn);
	}

	ctx->reorder->output_rewrites = ctx->options.receive_rewrites;

	ereport(LOG,
			(errmsg("starting logical decoding for slot \"%s\"",
					NameStr(slot->data.name)),
			 errdetail("Streaming transactions committing after %X/%X, reading WAL from %X/%X.",
					   LSN_FORMAT_ARGS(slot->data.confirmed_flush),
					   LSN_FORMAT_ARGS(slot->data.restart_lsn))));

	return ctx;
}

/*
 * 如果已构建出一致的初始解码快照，则返回 true。     
 */
bool
DecodingContextReady(LogicalDecodingContext *ctx)
{
	return SnapBuildCurrentState(ctx->snapshot_builder) == SNAPBUILD_CONSISTENT;
}

/*
 * 从解码槽读取，直到它已就绪可以开始提取变更。         
 */
void
DecodingContextFindStartpoint(LogicalDecodingContext *ctx)
{
	ReplicationSlot *slot = ctx->slot;

	/* 初始化从哪里开始读取 WAL。       */
	XLogBeginRead(ctx->reader, slot->data.restart_lsn);

	elog(DEBUG1, "searching for logical decoding starting point, starting at %X/%X",
		 LSN_FORMAT_ARGS(slot->data.restart_lsn));

	/* 等待一个一致的起点。       */
	for (;;)
	{
		XLogRecord *record;
		char	   *err = NULL;

		/* read_page 回调会等待新 WAL         */
		record = XLogReadRecord(ctx->reader, &err);
		if (err)
			elog(ERROR, "could not find logical decoding starting point: %s", err);
		if (!record)
			elog(ERROR, "could not find logical decoding starting point");

		LogicalDecodingProcessRecord(ctx, ctx->reader);

		/* 只持续到找到一致的地点为止       */
		if (DecodingContextReady(ctx))
			break;

		CHECK_FOR_INTERRUPTS();
	}

	SpinLockAcquire(&slot->mutex);
	slot->data.confirmed_flush = ctx->reader->EndRecPtr;
	if (slot->data.two_phase)
		slot->data.two_phase_at = ctx->reader->EndRecPtr;
	SpinLockRelease(&slot->mutex);
}

/*
 * 释放先前分配的解码上下文，必要时调用             
 * 关闭回调。       
 */
void
FreeDecodingContext(LogicalDecodingContext *ctx)
{
	if (ctx->callbacks.shutdown_cb != NULL)
		shutdown_cb_wrapper(ctx);

	ReorderBufferFree(ctx->reorder);
	FreeSnapshotBuilder(ctx->snapshot_builder);
	XLogReaderFree(ctx->reader);
	MemoryContextDelete(ctx->context);
}

/*
 * 使用上下文的输出例程准备一次写入。
 */
void
OutputPluginPrepareWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->accept_writes)
		elog(ERROR, "writes are only accepted in commit, begin and change callbacks");

	ctx->prepare_write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = true;
}

/*
 * 使用上下文的输出例程执行一次写入。
 */
void
OutputPluginWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->prepared_write)
		elog(ERROR, "OutputPluginPrepareWrite needs to be called before OutputPluginWrite");

	ctx->write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = false;
}

/*
 * 更新进度跟踪（若支持）。    
 */
void
OutputPluginUpdateProgress(struct LogicalDecodingContext *ctx,
						   bool skipped_xact)
{
	if (!ctx->update_progress)
		return;

	ctx->update_progress(ctx, ctx->write_location, ctx->write_xid,
						 skipped_xact);
}

/*
 * 加载输出插件，查找其输出插件初始化函数，并检查    
 * 它是否提供所需的回调。       
 */
static void
LoadOutputPlugin(OutputPluginCallbacks *callbacks, const char *plugin)
{
	LogicalOutputPluginInit plugin_init;

	plugin_init = (LogicalOutputPluginInit)
		load_external_function(plugin, "_PG_output_plugin_init", false, NULL);

	if (plugin_init == NULL)
		elog(ERROR, "output plugins have to declare the _PG_output_plugin_init symbol");

	/* 请输出插件填充回调结构体              */
	plugin_init(callbacks);

	if (callbacks->begin_cb == NULL)
		elog(ERROR, "output plugins have to register a begin callback");
	if (callbacks->change_cb == NULL)
		elog(ERROR, "output plugins have to register a change callback");
	if (callbacks->commit_cb == NULL)
		elog(ERROR, "output plugins have to register a commit callback");
}

static void
output_plugin_error_callback(void *arg)
{
	LogicalErrorCallbackState *state = (LogicalErrorCallbackState *) arg;

	/* 并非所有回调都有关联的 LSN     */
	if (state->report_location != InvalidXLogRecPtr)
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback, associated LSN %X/%X",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name,
				   LSN_FORMAT_ARGS(state->report_location));
	else
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name);
}

static void
startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "startup";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* 执行实际工作：调用回调 */
	ctx->callbacks.startup_cb(ctx, opt, is_init);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
shutdown_cb_wrapper(LogicalDecodingContext *ctx)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "shutdown";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* 执行实际工作：调用回调 */
	ctx->callbacks.shutdown_cb(ctx);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}


/*
 * ReorderBuffer 的回调，补充一些信息后调用                       
 * output_plugin.h 插件  
 */
static void
begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "begin";
	state.report_location = txn->first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->first_lsn;
	ctx->end_xact = false;

	/* 执行实际工作：调用回调 */
	ctx->callbacks.begin_cb(ctx, txn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "commit";
	state.report_location = txn->final_lsn; /* 提交记录的开头      */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* 指向记录的末尾           */
	ctx->end_xact = true;

	/* 执行实际工作：调用回调 */
	ctx->callbacks.commit_cb(ctx, txn, commit_lsn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

/*
 * begin_prepare 的功能与 begin 非常相似，区别在于         
 * 它会带有 gid（全局事务标识符）信息供插件使用。        
 * 本来我们也考虑过扩展现有的 begin，但那会破坏复制    
 * 协议，而且这样看起来更                                        
 * 整洁  
 */
static void
begin_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 只有在支持两阶段提交时才应当调用此函数             */
	Assert(ctx->twophase);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "begin_prepare";
	state.report_location = txn->first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->first_lsn;
	ctx->end_xact = false;

	/*
	 * 如果插件支持两阶段提交，则 begin_prepare 回调是        
	 * 必需的
	 */
	if (ctx->callbacks.begin_prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"begin_prepare_cb")));

	/* 执行实际工作：调用回调 */
	ctx->callbacks.begin_prepare_cb(ctx, txn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr prepare_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 只有在支持两阶段提交时才应当调用此函数             */
	Assert(ctx->twophase);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "prepare";
	state.report_location = txn->final_lsn; /* prepare 记录的开头     */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* 指向记录的末尾           */
	ctx->end_xact = true;

	/*
	 * 如果插件支持两阶段提交，则 prepare 回调是        
	 * 必需的
	 */
	if (ctx->callbacks.prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"prepare_cb")));

	/* 执行实际工作：调用回调 */
	ctx->callbacks.prepare_cb(ctx, txn, prepare_lsn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
commit_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						   XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 只有在支持两阶段提交时才应当调用此函数             */
	Assert(ctx->twophase);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "commit_prepared";
	state.report_location = txn->final_lsn; /* 提交记录的开头      */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* 指向记录的末尾           */
	ctx->end_xact = true;

	/*
	 * 如果插件支持两阶段提交，则 commit_prepared 回调       
	 * 是必需的
	 */
	if (ctx->callbacks.commit_prepared_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"commit_prepared_cb")));

	/* 执行实际工作：调用回调 */
	ctx->callbacks.commit_prepared_cb(ctx, txn, commit_lsn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
rollback_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							 XLogRecPtr prepare_end_lsn,
							 TimestampTz prepare_time)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 只有在支持两阶段提交时才应当调用此函数             */
	Assert(ctx->twophase);

	/* 把回调 + 信息压入错误上下文栈      */
	state.ctx = ctx;
	state.callback_name = "rollback_prepared";
	state.report_location = txn->final_lsn; /* 提交记录的开头      */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* 指向记录的末尾           */
	ctx->end_xact = true;

	/*
	 * 若插件支持两阶段提交，则回滚已准备事务回调        
	 * 必需。   
	 */
	if (ctx->callbacks.rollback_prepared_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"rollback_prepared_cb")));

	/* 执行实际工作：调用回调 */
	ctx->callbacks.rollback_prepared_cb(ctx, txn, prepare_end_lsn,
										prepare_time);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  Relation relation, ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "change";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * 上报本次变更的 lsn，使客户端的应答能够给出最新    
	 * 结果。这永远不够（也不应够！）确认已收到    
	 * 该事务，但或许能让另一事务的提交经由一条消息   
	 * 获得确认。                         
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.change_cb(ctx, txn, relation, change);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
					int nrelations, Relation relations[], ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	if (!ctx->callbacks.truncate_cb)
		return;

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "truncate";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * 上报本次变更的 lsn，使客户端的应答能够给出最新    
	 * 结果。这永远不够（也不应够！）确认已收到    
	 * 该事务，但或许能让另一事务的提交经由一条消息   
	 * 获得确认。                         
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.truncate_cb(ctx, txn, nrelations, relations, change);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

bool
filter_prepare_cb_wrapper(LogicalDecodingContext *ctx, TransactionId xid,
						  const char *gid)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;
	bool		ret;

	Assert(!ctx->fast_forward);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "filter_prepare";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* 执行实际工作：调用回调 */
	ret = ctx->callbacks.filter_prepare_cb(ctx, xid, gid);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;

	return ret;
}

bool
filter_by_origin_cb_wrapper(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;
	bool		ret;

	Assert(!ctx->fast_forward);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "filter_by_origin";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* 执行实际工作：调用回调 */
	ret = ctx->callbacks.filter_by_origin_cb(ctx, origin_id);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;

	return ret;
}

static void
message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr message_lsn, bool transactional,
				   const char *prefix, Size message_size, const char *message)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	if (ctx->callbacks.message_cb == NULL)
		return;

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "message";
	state.report_location = message_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn != NULL ? txn->xid : InvalidTransactionId;
	ctx->write_location = message_lsn;
	ctx->end_xact = false;

	/* 执行实际工作：调用回调 */
	ctx->callbacks.message_cb(ctx, txn, message_lsn, transactional, prefix,
							  message_size, message);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_start_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						XLogRecPtr first_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 仅当支持流式传输时才应当调用此函数。        */
	Assert(ctx->streaming);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_start";
	state.report_location = first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * 上报本消息的 lsn，使客户端应答能给出最新   
	 * 结果。这永远不够（也不应够！）确认已收到       
	 * 该事务，但或许能让另一事务的提交仅需一条   
	 * 消息即可获得确认。                           
	 */
	ctx->write_location = first_lsn;

	ctx->end_xact = false;

	/* 流式模式下必须提供 stream_start_cb    */
	if (ctx->callbacks.stream_start_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_start_cb")));

	ctx->callbacks.stream_start_cb(ctx, txn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_stop_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
					   XLogRecPtr last_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 仅当支持流式传输时才应当调用此函数。        */
	Assert(ctx->streaming);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_stop";
	state.report_location = last_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * 上报本消息的 lsn，使客户端应答能给出最新   
	 * 结果。这永远不够（也不应够！）确认已收到       
	 * 该事务，但或许能让另一事务的提交仅需一条   
	 * 消息即可获得确认。                           
	 */
	ctx->write_location = last_lsn;

	ctx->end_xact = false;

	/* 流式模式下必须提供 stream_stop_cb    */
	if (ctx->callbacks.stream_stop_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_stop_cb")));

	ctx->callbacks.stream_stop_cb(ctx, txn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_abort_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						XLogRecPtr abort_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 仅当支持流式传输时才应当调用此函数。        */
	Assert(ctx->streaming);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_abort";
	state.report_location = abort_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = abort_lsn;
	ctx->end_xact = true;

	/* 流式模式下必须提供 stream_abort_cb    */
	if (ctx->callbacks.stream_abort_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_abort_cb")));

	ctx->callbacks.stream_abort_cb(ctx, txn, abort_lsn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						  XLogRecPtr prepare_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/*
	 * 仅当支持流式传输与两阶段提交时才应当调用此函   
	 * 数。        
	 */
	Assert(ctx->streaming);
	Assert(ctx->twophase);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_prepare";
	state.report_location = txn->final_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn;
	ctx->end_xact = true;

	/* 在流式模式与两阶段提交下必须提供 stream_prepare_cb      */
	if (ctx->callbacks.stream_prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming at prepare time requires a %s callback",
						"stream_prepare_cb")));

	ctx->callbacks.stream_prepare_cb(ctx, txn, prepare_lsn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						 XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 仅当支持流式传输时才应当调用此函数。        */
	Assert(ctx->streaming);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_commit";
	state.report_location = txn->final_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn;
	ctx->end_xact = true;

	/* 流式模式下必须提供 stream_commit_cb    */
	if (ctx->callbacks.stream_commit_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_commit_cb")));

	ctx->callbacks.stream_commit_cb(ctx, txn, commit_lsn);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						 Relation relation, ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 仅当支持流式传输时才应当调用此函数。        */
	Assert(ctx->streaming);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_change";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * 上报本次变更的 lsn，使客户端的应答能够给出最新    
	 * 结果。这永远不够（也不应够！）确认已收到    
	 * 该事务，但或许能让另一事务的提交经由一条消息   
	 * 获得确认。                         
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	/* 流式模式下必须提供 stream_change_cb    */
	if (ctx->callbacks.stream_change_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_change_cb")));

	ctx->callbacks.stream_change_cb(ctx, txn, relation, change);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						  XLogRecPtr message_lsn, bool transactional,
						  const char *prefix, Size message_size, const char *message)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 仅当支持流式传输时才应当调用此函数。        */
	Assert(ctx->streaming);

	/* 此回调可选           */
	if (ctx->callbacks.stream_message_cb == NULL)
		return;

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_message";
	state.report_location = message_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn != NULL ? txn->xid : InvalidTransactionId;
	ctx->write_location = message_lsn;
	ctx->end_xact = false;

	/* 执行实际工作：调用回调 */
	ctx->callbacks.stream_message_cb(ctx, txn, message_lsn, transactional, prefix,
									 message_size, message);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
stream_truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						   int nrelations, Relation relations[],
						   ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 仅当支持流式传输时才应当调用此函数。        */
	Assert(ctx->streaming);

	/* 此回调可选           */
	if (!ctx->callbacks.stream_truncate_cb)
		return;

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "stream_truncate";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * 上报本次变更的 lsn，使客户端的应答能够给出最新    
	 * 结果。这永远不够（也不应够！）确认已收到    
	 * 该事务，但或许能让另一事务的提交经由一条消息   
	 * 获得确认。                         
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.stream_truncate_cb(ctx, txn, nrelations, relations, change);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

static void
update_progress_txn_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* 将回调+信息压入错误上下文栈        */
	state.ctx = ctx;
	state.callback_name = "update_progress_txn";
	state.report_location = lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* 设输出状态  */
	ctx->accept_writes = false;
	ctx->write_xid = txn->xid;

	/*
	 * 上报本次变更的 lsn，使客户端的应答能够给出最新    
	 * 结果。这永远不够（也不应够！）确认已收到    
	 * 该事务，但或许能让另一事务的提交经由一条消息   
	 * 获得确认。                         
	 */
	ctx->write_location = lsn;

	ctx->end_xact = false;

	OutputPluginUpdateProgress(ctx, false);

	/* 弹出错误上下文栈    */
	error_context_stack = errcallback.previous;
}

/*
 * 为当前复制槽中的历史快照设置所需的系统表 xmin          
 * 视界。        
 *
 * 注意：多数情况下我们无法立即用该 xmin 提升 xmin        
 * 视界：需要等到客户端通过 LogicalConfirmReceivedLocation()      
 * 确认已接收 current_lsn。                              
 */
void
LogicalIncreaseXminForSlot(XLogRecPtr current_lsn, TransactionId xmin)
{
	bool		updated_xmin = false;
	ReplicationSlot *slot;
	bool		got_new_xmin = false;

	slot = MyReplicationSlot;

	Assert(slot != NULL);

	SpinLockAcquire(&slot->mutex);

	/*
	 * 若已有更新的 xmin 则不覆盖，槽中重启解码时就可能 
	 * 发生这种情况。      
	 */
	if (TransactionIdPrecedesOrEquals(xmin, slot->data.catalog_xmin))
	{
	}

	/*
	 * 若客户端已确认到此 lsn，则可直接将其标记为      
	 * 已接受。在槽中重启解码时可能发生这种情况      
	 * 。  
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;

		/* 候选值可直接使用           */
		updated_xmin = true;
	}

	/*
	 * 仅在前一值已被应用时才增加，否则若接收方确认  
	 * 过慢，就可能永远无法更新。                     
	 */
	else if (slot->candidate_xmin_lsn == InvalidXLogRecPtr)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;

		/*
		 * 释放自旋锁后，以适当的日志级别记录新      
		 * xmin。  
		 */
		got_new_xmin = true;
	}
	SpinLockRelease(&slot->mutex);

	if (got_new_xmin)
		elog(DEBUG1, "got new catalog xmin %u at %X/%X", xmin,
			 LSN_FORMAT_ARGS(current_lsn));

	/* 候选值相对当前刷写位置已有效，直接应用      */
	if (updated_xmin)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * 标记我们需要读取的最小 LSN（restart_lsn），       
 * 以便重放 current_lsn 时未提交的全部事务。 
 *
 * 与 LogicalIncreaseXminForSlot 一样，该值仅在客户端       
 * 确认已接收 current_lsn 后才生效。       
 */
void
LogicalIncreaseRestartDecodingForSlot(XLogRecPtr current_lsn, XLogRecPtr restart_lsn)
{
	bool		updated_lsn = false;
	ReplicationSlot *slot;

	slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(restart_lsn != InvalidXLogRecPtr);
	Assert(current_lsn != InvalidXLogRecPtr);

	SpinLockAcquire(&slot->mutex);

	/* 若已有更新的 restart_lsn 则不覆盖 */
	if (restart_lsn <= slot->data.restart_lsn)
	{
		SpinLockRelease(&slot->mutex);
	}

	/*
	 * 我们可能已刷写至足够远处，可直接接受此 lsn，     
	 * 此时无需检查已有的候选 LSN。                          
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		/* 候选值可直接使用           */
		updated_lsn = true;
	}

	/*
	 * 仅在前一值已被应用时才增加，否则若接收方确认  
	 * 过慢，就可能永远无法更新。此处漏掉的值只会在   
	 * 重连后造成一些额外工作。                            
	 */
	else if (slot->candidate_restart_valid == InvalidXLogRecPtr)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		elog(DEBUG1, "got new restart lsn %X/%X at %X/%X",
			 LSN_FORMAT_ARGS(restart_lsn),
			 LSN_FORMAT_ARGS(current_lsn));
	}
	else
	{
		XLogRecPtr	candidate_restart_lsn;
		XLogRecPtr	candidate_restart_valid;
		XLogRecPtr	confirmed_flush;

		candidate_restart_lsn = slot->candidate_restart_lsn;
		candidate_restart_valid = slot->candidate_restart_valid;
		confirmed_flush = slot->data.confirmed_flush;
		SpinLockRelease(&slot->mutex);

		elog(DEBUG1, "failed to increase restart lsn: proposed %X/%X, after %X/%X, current candidate %X/%X, current after %X/%X, flushed up to %X/%X",
			 LSN_FORMAT_ARGS(restart_lsn),
			 LSN_FORMAT_ARGS(current_lsn),
			 LSN_FORMAT_ARGS(candidate_restart_lsn),
			 LSN_FORMAT_ARGS(candidate_restart_valid),
			 LSN_FORMAT_ARGS(confirmed_flush));
	}

	/* 候选值相对当前刷写位置已全部有效，直接应用     */
	if (updated_lsn)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * 处理消费者确认已接收直到 lsn 的全部变更。            
 */
void
LogicalConfirmReceivedLocation(XLogRecPtr lsn)
{
	Assert(lsn != InvalidXLogRecPtr);

	/* 首先对 candidate_lsn 做无锁检查。    */
	if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr ||
		MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr)
	{
		bool		updated_xmin = false;
		bool		updated_restart = false;

		SpinLockAcquire(&MyReplicationSlot->mutex);

		/*
		 * 防止 confirmed_flush 后移，因为这可能重复复制        
		 * 已经复制过的变更，从而造成数据重复问题。    
		 * 。     
		 *
		 * 当客户端确认一个无需为其做任何事、因而未     
		 * 存储的 LSN 时就可能发生这种情况。重启后，    
		 * 客户端可能将先前持久存储的 LSN 作为确认 
		 * 发来，但我们需要忽略这样的 LSN。类似的情形    
		 * 处理见 CreateDecodingContext。                      
		 */
		if (lsn > MyReplicationSlot->data.confirmed_flush)
			MyReplicationSlot->data.confirmed_flush = lsn;

		/* 若已越过提升 xmin 所需的位置，则进行提升   */
		if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_xmin_lsn <= lsn)
		{
			/*
			 * 我们必须在修改内存值*之前*把变更后的 xmin  
			 * 写入磁盘，否则崩溃后将不知道某些系统表    
			 * 元组可能已被移除。                              
			 *
			 * 即先写入 ->xmin，新状态同步到磁盘后才  
			 * 更新 ->effective_xmin。崩溃后 ->effective_xmin          
			 * 会被设为 ->xmin。                  
			 */
			if (TransactionIdIsValid(MyReplicationSlot->candidate_catalog_xmin) &&
				MyReplicationSlot->data.catalog_xmin != MyReplicationSlot->candidate_catalog_xmin)
			{
				MyReplicationSlot->data.catalog_xmin = MyReplicationSlot->candidate_catalog_xmin;
				MyReplicationSlot->candidate_catalog_xmin = InvalidTransactionId;
				MyReplicationSlot->candidate_xmin_lsn = InvalidXLogRecPtr;
				updated_xmin = true;
			}
		}

		if (MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_restart_valid <= lsn)
		{
			Assert(MyReplicationSlot->candidate_restart_lsn != InvalidXLogRecPtr);

			MyReplicationSlot->data.restart_lsn = MyReplicationSlot->candidate_restart_lsn;
			MyReplicationSlot->candidate_restart_lsn = InvalidXLogRecPtr;
			MyReplicationSlot->candidate_restart_valid = InvalidXLogRecPtr;
			updated_restart = true;
		}

		SpinLockRelease(&MyReplicationSlot->mutex);

		/*
		 * 首先把新的 xmin 和 restart_lsn 写入磁盘，这样崩溃  
		 * 后我们才知道状况。即使这样做了，检查点进程   
		 * 仍可能在共享内存中看到更新后的 restart_lsn 值； 
		 * 随后，还没把该值写入磁盘就可能发生崩溃。   
		 * 因此检查点进程仍需特别努力，把 restart_lsn   
		 * 所需的 WAL 段写入磁盘予以保留。详情见         
		 * CreateCheckPoint() 与 CreateRestartPoint()。          
		 */
		if (updated_xmin || updated_restart)
		{
			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
			elog(DEBUG1, "updated xmin: %u restart: %u", updated_xmin, updated_restart);
		}

		/*
		 * 既然新的 xmin 已安全落盘，就可以让全局值      
		 * 前进。我们不取 ProcArrayLock 或类似的锁，因为  
		 * 这里只推进 xmin，即使并发计算漏掉了它，        
		 * 也无大碍。          
		 */
		if (updated_xmin)
		{
			SpinLockAcquire(&MyReplicationSlot->mutex);
			MyReplicationSlot->effective_catalog_xmin = MyReplicationSlot->data.catalog_xmin;
			SpinLockRelease(&MyReplicationSlot->mutex);

			ReplicationSlotsComputeRequiredXmin(false);
			ReplicationSlotsComputeRequiredLSN();
		}
	}
	else
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);

		/*
		 * 防止 confirmed_flush 后移。详情见                       
		 * 上文注释。 
		 */
		if (lsn > MyReplicationSlot->data.confirmed_flush)
			MyReplicationSlot->data.confirmed_flush = lsn;

		SpinLockRelease(&MyReplicationSlot->mutex);
	}
}

/*
 * 在（子）事务中止时清除逻辑流式状态。      
 */
void
ResetLogicalStreamingState(void)
{
	CheckXidAlive = InvalidTransactionId;
	bsysscan = false;
}

/*
 * 上报槽统计信息。
 */
void
UpdateDecodingStats(LogicalDecodingContext *ctx)
{
	ReorderBuffer *rb = ctx->reorder;
	PgStat_StatReplSlotEntry repSlotStat;

	/* 若没有待发送的复制统计，则无需做任何事。     */
	if (rb->spillBytes <= 0 && rb->streamBytes <= 0 && rb->totalBytes <= 0)
		return;

	elog(DEBUG2, "UpdateDecodingStats: updating stats %p %lld %lld %lld %lld %lld %lld %lld %lld",
		 rb,
		 (long long) rb->spillTxns,
		 (long long) rb->spillCount,
		 (long long) rb->spillBytes,
		 (long long) rb->streamTxns,
		 (long long) rb->streamCount,
		 (long long) rb->streamBytes,
		 (long long) rb->totalTxns,
		 (long long) rb->totalBytes);

	repSlotStat.spill_txns = rb->spillTxns;
	repSlotStat.spill_count = rb->spillCount;
	repSlotStat.spill_bytes = rb->spillBytes;
	repSlotStat.stream_txns = rb->streamTxns;
	repSlotStat.stream_count = rb->streamCount;
	repSlotStat.stream_bytes = rb->streamBytes;
	repSlotStat.total_txns = rb->totalTxns;
	repSlotStat.total_bytes = rb->totalBytes;

	pgstat_report_replslot(ctx->slot, &repSlotStat);

	rb->spillTxns = 0;
	rb->spillCount = 0;
	rb->spillBytes = 0;
	rb->streamTxns = 0;
	rb->streamCount = 0;
	rb->streamBytes = 0;
	rb->totalTxns = 0;
	rb->totalBytes = 0;
}
