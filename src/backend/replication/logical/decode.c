/* -------------------------------------------------------------------------
 *
 * decode.c
 *		本模块用 xlogreader.h 的 API 读取 WAL 记录并将其解码， 
 *		以便进行逻辑解码：把信息传递给            
 *		reorderbuffer 模块（包含实际更改），并传递给    
 *		snapbuild 模块以构建匹配的系统表快照（从而能够   
 *		正确解码 reorderbuffer 中的更改）。     
 *
 * NOTE:
 *		本模块基本上负责处理所有低层 xlog 事务，  
 *		为 reorderbuffer.c 与 snapbuild.c 服务。有少量泄漏，即 
 *		用特定记录的结构体来传递数据；它们只是恰好    
 *		以方便的格式包含了数量正合适的数据。    
 *		此处不存在、也不应存在太多针对记录             
 *		内容的智能处理，只需将它们转换为更易用的格   
 *		式。 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/decode.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/xlogutils.h"
#include "catalog/pg_control.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "replication/origin.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "storage/standby.h"

/* 各条记录（组）的处理函数*/
static void DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeTruncate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);

static void DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						 xl_xact_parsed_commit *parsed, TransactionId xid,
						 bool two_phase);
static void DecodeAbort(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						xl_xact_parsed_abort *parsed, TransactionId xid,
						bool two_phase);
static void DecodePrepare(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						  xl_xact_parsed_prepare *parsed);


/* 解码元组的公共函数      */
static void DecodeXLogTuple(char *data, Size len, ReorderBufferTupleBuf *tuple);

/* 用于解码事务的辅助函数          */
static inline bool FilterPrepare(LogicalDecodingContext *ctx,
								 TransactionId xid, const char *gid);
static bool DecodeTXNNeedSkip(LogicalDecodingContext *ctx,
							  XLogRecordBuffer *buf, Oid txn_dbid,
							  RepOriginId origin_id);

/*
 * 对 XLogReadRecord() 读出的每条记录，执行所需的动作来   
 * 解码它，所用的输出插件已设置于逻辑解码上下文     
 * 中。  
 *
 * NB: 每条记录的 xid 都需要由 reorderbuffer 来处理（记录  
 * 内容里的 xid 与此规则无关）。这意味着对于原本不会   
 * 经过 reorderbuffer 的记录，必须调用                   
 * ReorderBufferProcessXid()。我们并不想默认对每种                
 * 记录类型都调用它，因为例如空事务                     
 * 在不存在既有状态时能得到更高效的处理                 
 * 。            
 *
 * 我们还支持快进掠过记录的能力，可完全跳过某些     
 * 记录类型——详见各记录类型的细节。               
 */
void
LogicalDecodingProcessRecord(LogicalDecodingContext *ctx, XLogReaderState *record)
{
	XLogRecordBuffer buf;
	TransactionId txid;
	RmgrData	rmgr;

	buf.origptr = ctx->reader->ReadRecPtr;
	buf.endptr = ctx->reader->EndRecPtr;
	buf.record = record;

	txid = XLogRecGetTopXid(record);

	/*
	 * 若顶层 xid 有效，则需将子事务指派给顶层事务。  
	 * 所有记录都需这样做，因此我们把该操作放在      
	 * switch 之前。  
	 */
	if (TransactionIdIsValid(txid))
	{
		ReorderBufferAssignChild(ctx->reorder,
								 txid,
								 XLogRecGetXid(record),
								 buf.origptr);
	}

	rmgr = GetRmgr(XLogRecGetRmid(record));

	if (rmgr.rm_decode != NULL)
		rmgr.rm_decode(ctx, &buf);
	else
	{
		/* 仅处理 xid 即告完成   */
		ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(record),
								buf.origptr);
	}
}

/*
 * 为 LogicalDecodingProcessRecord() 处理 rmgr XLOG_ID记录。
 */
void
xlog_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	uint8		info = XLogRecGetInfo(buf->record) & ~XLR_INFO_MASK;

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(buf->record),
							buf->origptr);

	switch (info)
	{
			/* 这同样用于 END_OF_RECOVERY 检查点        */
		case XLOG_CHECKPOINT_SHUTDOWN:
		case XLOG_END_OF_RECOVERY:
			SnapBuildSerializationPoint(builder, buf->origptr);

			break;
		case XLOG_CHECKPOINT_ONLINE:

			/*
			 * 此点附近已记录 RUNNING_XACTS 记录，可从那里    
			 * 重新开始。        
			 */
			break;
		case XLOG_PARAMETER_CHANGE:
			{
				xl_parameter_change *xlrec =
					(xl_parameter_change *) XLogRecGetData(buf->record);

				/*
				 * 若主库 wal_level 降到低于 logical，我们  
				 * 希望防止现有逻辑复制槽被使用。备库上 
				 * 的既有逻辑复制槽会在本 WAL 记录重放时 
				 * 失效；此外，wal_level 不够时创建复制槽      
				 * 也会失败；但所有这些操作并非同步进行，
				 * 因此在 wal_level 正在降低期间，某个逻辑  
				 * 复制槽可能乘虚而入。故需要这一额外    
				 * 检查。              
				 */
				if (xlrec->wal_level < WAL_LEVEL_LOGICAL)
				{
					/*
					 * 这只可能发生在备库，因为主库在存有 
					 * 既有逻辑复制槽时不会允许在把 wal_level   
					 * 改到低于 logical 后重启。     
					 */
					Assert(RecoveryInProgress());
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("logical decoding on standby requires wal_level >= logical on the primary")));
				}
				break;
			}
		case XLOG_NOOP:
		case XLOG_NEXTOID:
		case XLOG_SWITCH:
		case XLOG_BACKUP_END:
		case XLOG_RESTORE_POINT:
		case XLOG_FPW_CHANGE:
		case XLOG_FPI_FOR_HINT:
		case XLOG_FPI:
		case XLOG_OVERWRITE_CONTRECORD:
			break;
		default:
			elog(ERROR, "unexpected RM_XLOG_ID record type: %u", info);
	}
}

/*
 * 为 LogicalDecodingProcessRecord() 处理 rmgr XACT_ID记录。
 */
void
xact_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	ReorderBuffer *reorder = ctx->reorder;
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & XLOG_XACT_OPMASK;

	/*
	 * 若快照尚未完整构建，就无法解码任何东西，因此  
	 * 退出。
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_XACT_COMMIT:
		case XLOG_XACT_COMMIT_PREPARED:
			{
				xl_xact_commit *xlrec;
				xl_xact_parsed_commit parsed;
				TransactionId xid;
				bool		two_phase = false;

				xlrec = (xl_xact_commit *) XLogRecGetData(r);
				ParseCommitRecord(XLogRecGetInfo(buf->record), xlrec, &parsed);

				if (!TransactionIdIsValid(parsed.twophase_xid))
					xid = XLogRecGetXid(r);
				else
					xid = parsed.twophase_xid;

				/*
				 * 我们希望以两阶段方式处理事务，条件是 
				 * 输出插件支持两阶段提交，且不在 prepare  
				 * 阶段过滤掉该事务。                    
				 */
				if (info == XLOG_XACT_COMMIT_PREPARED)
					two_phase = !(FilterPrepare(ctx, xid,
												parsed.twophase_gid));

				DecodeCommit(ctx, buf, &parsed, xid, two_phase);
				break;
			}
		case XLOG_XACT_ABORT:
		case XLOG_XACT_ABORT_PREPARED:
			{
				xl_xact_abort *xlrec;
				xl_xact_parsed_abort parsed;
				TransactionId xid;
				bool		two_phase = false;

				xlrec = (xl_xact_abort *) XLogRecGetData(r);
				ParseAbortRecord(XLogRecGetInfo(buf->record), xlrec, &parsed);

				if (!TransactionIdIsValid(parsed.twophase_xid))
					xid = XLogRecGetXid(r);
				else
					xid = parsed.twophase_xid;

				/*
				 * 我们希望以两阶段方式处理事务，条件是 
				 * 输出插件支持两阶段提交，且不在 prepare  
				 * 阶段过滤掉该事务。                    
				 */
				if (info == XLOG_XACT_ABORT_PREPARED)
					two_phase = !(FilterPrepare(ctx, xid,
												parsed.twophase_gid));

				DecodeAbort(ctx, buf, &parsed, xid, two_phase);
				break;
			}
		case XLOG_XACT_ASSIGNMENT:

			/*
			 * 处理每条记录时，我们会按需把子事务指派给
			 * 顶层事务。因此这里无需做任何事情。参见      
			 * LogicalDecodingProcessRecord 
			 */
			break;
		case XLOG_XACT_INVALIDATIONS:
			{
				TransactionId xid;
				xl_xact_invals *invals;

				xid = XLogRecGetXid(r);
				invals = (xl_xact_invals *) XLogRecGetData(r);

				/*
				 * 为无 xid 事务执行失效处理，否则将其  
				 * 累积起来，以便可在提交                          
				 * 时处理。    
				 */
				if (TransactionIdIsValid(xid))
				{
					if (!ctx->fast_forward)
						ReorderBufferAddInvalidations(reorder, xid,
													  buf->origptr,
													  invals->nmsgs,
													  invals->msgs);
					ReorderBufferXidSetCatalogChanges(ctx->reorder, xid,
													  buf->origptr);
				}
				else if ((!ctx->fast_forward))
					ReorderBufferImmediateInvalidation(ctx->reorder,
													   invals->nmsgs,
													   invals->msgs);
			}
			break;
		case XLOG_XACT_PREPARE:
			{
				xl_xact_parsed_prepare parsed;
				xl_xact_prepare *xlrec;

				/* 好，解析 */
				xlrec = (xl_xact_prepare *) XLogRecGetData(r);
				ParsePrepareRecord(XLogRecGetInfo(buf->record),
								   xlrec, &parsed);

				/*
				 * 我们希望以两阶段方式处理事务，条件是 
				 * 输出插件支持两阶段提交，且不在 prepare  
				 * 阶段过滤掉该事务。                    
				 */
				if (FilterPrepare(ctx, parsed.twophase_xid,
								  parsed.twophase_gid))
				{
					ReorderBufferProcessXid(reorder, parsed.twophase_xid,
											buf->origptr);
					break;
				}

				/*
				 * 注意：若已准备事务独占锁定了 [user] 系  
				 * 统表，则解码 prepare 可能阻塞直至主事务    
				 * 提交，因为它需要锁定这些系统                
				 * 表。                  
				 *
				 * XXX 现在，若 prepare 事务正等待其被          
				 * 逻辑复制用于分布式 2PC，甚至可能死锁。  
				 * 要避免这点，可禁止预备已独占锁定了
				 * [user] 系统表事务；但就目前而言，我们  
				 * 要求用户不要去做                                
				 * 这种操作。   
				 */
				DecodePrepare(ctx, buf, &parsed);
				break;
			}
		default:
			elog(ERROR, "unexpected RM_XACT_ID record type: %u", info);
	}
}

/*
 * 为 LogicalDecodingProcessRecord() 处理 rmgr STANDBY_ID记录。
 */
void
standby_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & ~XLR_INFO_MASK;

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(r), buf->origptr);

	switch (info)
	{
		case XLOG_RUNNING_XACTS:
			{
				xl_running_xacts *running = (xl_running_xacts *) XLogRecGetData(r);

				SnapBuildProcessRunningXacts(builder, buf->origptr, running);

				/*
				 * 中止我们跟踪的、比记录的 oldestRunningXid 
				 * 更旧的事务。这里是最合适的位置，因为与 
				 * 关闭或恢复结束检查点不同，我们拥有关于  
				 * 所有正在运行事务的信息，其中含            
				 * 已准备的事务；而关闭检查点仅知道没有
				 * 非准备的事务                                       
				 * 正在进行。              
				 */
				ReorderBufferAbortOld(ctx->reorder, running->oldestRunningXid);
			}
			break;
		case XLOG_STANDBY_LOCK:
			break;
		case XLOG_INVALIDATIONS:

			/*
			 * 我们已经在命令层级通过 XLOG_XACT_INVALIDATIONS   
			 * 处理失效消息，所以这里无需做任何事情。      
			 */
			break;
		default:
			elog(ERROR, "unexpected RM_STANDBY_ID record type: %u", info);
	}
}

/*
 * 为 LogicalDecodingProcessRecord() 处理 rmgr HEAP2_ID记录。
 */
void
heap2_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_HEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * 若我们还没有快照，或者只是在快进，那么解码数据
	 * 变更就没有意义。但在快进模式下构建基础快照        
	 * 至关重要（如 SnapBuildProcessChange() 所 
	 * 做的那样），因为我们需要快照的 xmin 来确定         
	 * 复制槽的候选 catalog_xmin。参见                            
	 * SnapBuildProcessRunningXacts() 
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_HEAP2_MULTI_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeMultiInsert(ctx, buf);
			break;
		case XLOG_HEAP2_NEW_CID:
			if (!ctx->fast_forward)
			{
				xl_heap_new_cid *xlrec;

				xlrec = (xl_heap_new_cid *) XLogRecGetData(buf->record);
				SnapBuildProcessNewCid(builder, xid, buf->origptr, xlrec);

				break;
			}
		case XLOG_HEAP2_REWRITE:

			/*
			 * 虽然这些记录的存在只是为了服务逻辑解码，   
			 * 但所有工作都作为崩溃或归档恢复的一部分 
			 * 发生，所以这里无需做任何事情。  
			 */
			break;

			/*
			 * 这里其余的一切都只是我们不感兴趣的底层物理
			 * 内容。     
			 */
		case XLOG_HEAP2_FREEZE_PAGE:
		case XLOG_HEAP2_PRUNE:
		case XLOG_HEAP2_VACUUM:
		case XLOG_HEAP2_VISIBLE:
		case XLOG_HEAP2_LOCK_UPDATED:
			break;
		default:
			elog(ERROR, "unexpected RM_HEAP2_ID record type: %u", info);
	}
}

/*
 * 为 LogicalDecodingProcessRecord() 处理 rmgr HEAP_ID记录。
 */
void
heap_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_HEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * 若我们还没有快照，或者只是在快进，那么解码数据
	 * 变更就没有意义。但在快进模式下构建基础快照        
	 * 至关重要（如 SnapBuildProcessChange() 所 
	 * 做的那样），因为我们需要快照的 xmin 来确定         
	 * 复制槽的候选 catalog_xmin。参见                            
	 * SnapBuildProcessRunningXacts() 
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_HEAP_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeInsert(ctx, buf);
			break;

			/*
			 * 把 HOT 更新当作普通更新来处理。          
			 * 能否在本地将其做成 HOT 更新这一事实并无  
			 * 有用信息；且 WAL 布局兼容。   
			 */
		case XLOG_HEAP_HOT_UPDATE:
		case XLOG_HEAP_UPDATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeUpdate(ctx, buf);
			break;

		case XLOG_HEAP_DELETE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeDelete(ctx, buf);
			break;

		case XLOG_HEAP_TRUNCATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeTruncate(ctx, buf);
			break;

		case XLOG_HEAP_INPLACE:

			/*
			 * 就地更新只会在系统表元组上执行，且按定义 
			 * 改变元组可见性。由于不解码系统表元组，  
			 * 所以对该记录的内容并不                       
			 * 感兴趣。      
			 *
			 * 就地更新可由带 XID 的事务（例如在 CREATE         
			 * INDEX CONCURRENTLY 中）或无 XID 的事务（例
			 * 如 VACUUM）。在前一种情形，提交记录将包含  
			 * 缓存失效消息，故在此把该事务标记为    
			 * 正在修改系统表。目前这是多余的，因为 
			 * 提交也会做同样的事情，但一旦我们支持解码  
			 * 进行中的关系，这就会变得重要。                  
			 */
			if (!TransactionIdIsValid(xid))
				break;

			(void) SnapBuildProcessChange(builder, xid, buf->origptr);
			ReorderBufferXidSetCatalogChanges(ctx->reorder, xid, buf->origptr);
			break;

		case XLOG_HEAP_CONFIRM:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeSpecConfirm(ctx, buf);
			break;

		case XLOG_HEAP_LOCK:
			/* 我们目前不关心行级锁              */
			break;

		default:
			elog(ERROR, "unexpected RM_HEAP_ID record type: %u", info);
			break;
	}
}

/*
 * 询问输出插件我们是否想跳过此 PREPARE，而把    
 * 该事务以后作为普通提交发送。 
 */
static inline bool
FilterPrepare(LogicalDecodingContext *ctx, TransactionId xid,
			  const char *gid)
{
	/*
	 * 若尚未启用在 PREPARE 时解码两阶段事务的功能，  
	 * 则跳过。此时所有两阶段事务都被视为已过滤， 
	 * 并会在 COMMIT PREPARED 时作为普通事务                   
	 * 应用。
	 */
	if (!ctx->twophase)
		return true;

	/*
	 * filter_prepare 回调是可选的。若未提供，则所有   
	 * 已准备的事务都会通过。       
	 */
	if (ctx->callbacks.filter_prepare_cb == NULL)
		return false;

	return filter_prepare_cb_wrapper(ctx, xid, gid);
}

static inline bool
FilterByOrigin(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	if (ctx->callbacks.filter_by_origin_cb == NULL)
		return false;

	return filter_by_origin_cb_wrapper(ctx, origin_id);
}

/*
 * 为 LogicalDecodingProcessRecord() 处理 rmgr LOGICALMSG_ID记录。
 */
void
logicalmsg_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogReaderState *r = buf->record;
	TransactionId xid = XLogRecGetXid(r);
	uint8		info = XLogRecGetInfo(r) & ~XLR_INFO_MASK;
	RepOriginId origin_id = XLogRecGetOrigin(r);
	Snapshot	snapshot = NULL;
	xl_logical_message *message;

	if (info != XLOG_LOGICAL_MESSAGE)
		elog(ERROR, "unexpected RM_LOGICALMSG_ID record type: %u", info);

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(r), buf->origptr);

	/*
	 * 若我们还没有快照，或者只是在快进，那么解码      
	 * 消息就没有意义。   
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT ||
		ctx->fast_forward)
		return;

	message = (xl_logical_message *) XLogRecGetData(r);

	if (message->dbId != ctx->slot->data.database ||
		FilterByOrigin(ctx, origin_id))
		return;

	if (message->transactional &&
		!SnapBuildProcessChange(builder, xid, buf->origptr))
		return;
	else if (!message->transactional &&
			 (SnapBuildCurrentState(builder) != SNAPBUILD_CONSISTENT ||
			  SnapBuildXactNeedsSkip(builder, buf->origptr)))
		return;

	/*
	 * 若这是非事务性变更，获取我们预期使用的快照。    
	 * 只有当快照一致且该变更不应被跳过时，我们     
	 * 才会到达此处。             
	 *
	 * 对于事务性变更，我们不需要快照，将使用        
	 * ReorderBuffer 维护的常规快照。我们只需留为 NULL。    
	 */
	if (!message->transactional)
		snapshot = SnapBuildGetOrBuildSnapshot(builder);

	ReorderBufferQueueMessage(ctx->reorder, xid, snapshot, buf->endptr,
							  message->transactional,
							  message->message, /* first part of message is
												 * prefix */
							  message->message_size,
							  message->message + message->prefix_size);
}

/*
 * 在多种不同形式的提交记录之间统一处理提交记         
 * 录。  
 *
 * 'two_phase' 表示调用方想要分两个阶段处理该事务，       
 * 即若尚未处理 prepare 则先处理它，然后再处理      
 * commit_prepared 
 */
static void
DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			 xl_xact_parsed_commit *parsed, TransactionId xid,
			 bool two_phase)
{
	XLogRecPtr	origin_lsn = InvalidXLogRecPtr;
	TimestampTz commit_time = parsed->xact_time;
	RepOriginId origin_id = XLogRecGetOrigin(buf->record);
	int			i;

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		origin_lsn = parsed->origin_lsn;
		commit_time = parsed->origin_timestamp;
	}

	SnapBuildCommitTxn(ctx->snapshot_builder, buf->origptr, xid,
					   parsed->nsubxacts, parsed->subxacts,
					   parsed->xinfo);

	/* ----
	 * 检查我们是否对该特定事务感兴趣，若不感兴趣，    
	 * 则告诉 reorderbuffer 忘掉这些（子）事务的           
	 * 内容 
	 *
	 * 我们无法在这里只调用 ReorderBufferAbort()，因为           
	 * 需要执行该事务的失效处理。若只是跳过该事务， 
	 * 那么当前并不需要这一步：我们目前只在启动期间   
	 * 这样做，为的是到达客户端需要的第一条事务。既     
	 * 然我们在开始读取 WAL 之前就已重置系统表缓存，   
	 * 且尚未触及任何系统表，所以不存在任何可失效的      
	 * 东西。但如果这次提交被"遗忘"是因为它发生在另   
	 * 一个数据库中，那么失效就可能很重要，因为它们   
	 * 可能涉及共享系统表，且可能向相关 syscaches 载入  
	 * 数据。 
	 * ---
	 */
	if (DecodeTXNNeedSkip(ctx, buf, parsed->dbId, origin_id))
	{
		for (i = 0; i < parsed->nsubxacts; i++)
		{
			ReorderBufferForget(ctx->reorder, parsed->subxacts[i], buf->origptr);
		}
		ReorderBufferForget(ctx->reorder, xid, buf->origptr);

		return;
	}

	/* 把存留的子事务告知 reorderbuffer                  */
	for (i = 0; i < parsed->nsubxacts; i++)
	{
		ReorderBufferCommitChild(ctx->reorder, xid, parsed->subxacts[i],
								 buf->origptr, buf->endptr);
	}

	/*
	 * 若事务数据已解码完毕，则发送最终提交记录；
	 * 否则处理整个事务。                        
	 */
	if (two_phase)
	{
		ReorderBufferFinishPrepared(ctx->reorder, xid, buf->origptr, buf->endptr,
									SnapBuildGetTwoPhaseAt(ctx->snapshot_builder),
									commit_time, origin_id, origin_lsn,
									parsed->twophase_gid, true);
	}
	else
	{
		ReorderBufferCommit(ctx->reorder, xid, buf->origptr, buf->endptr,
							commit_time, origin_id, origin_lsn);
	}

	/*
	 * 在事务准备/提交/中止时更新解码统计。         
	 * 此外，当把变更溢写到磁盘或流式发送时我们也会   
	 * 发送这些统计，以免在解码被中断时丢失它们。尚    
	 * 不清楚比这更频繁或更不频繁地发送是否更好。
	 */
	UpdateDecodingStats(ctx);
}

/*
 * 解码 PREPARE 记录。逻辑与 DecodeCommit 类似。
 *
 * 注意：即便已检测到并发中止，我们也不跳过          
 * prepare，因为我们完全可能在此前已经发送了某些变更，   
 * 然后才发现中止；那种情况下我们需要在订阅者上中止    
 * 这些变更。为中止这类变更，我们确实会发送 prepare，     
 * 然后再发送 rollback prepared，这与发布者一侧实际发        
 * 生的情况一致。当然，我们可以发明一种新的中止 API，  
 * 在这类情形下发送 abort 并跳过发送 prepare 和 rollback     
 * prepared，但那样做并不那么直接，因为到那时我们    
 * 已经流式发送过该事务，那种情况下会在遇到 rollback     
 * 时得到处理。并发中止并非无法优化，但会因处理不 
 * 同情形而引入设计复杂度，故暂且搁置，因其似不值得。
 */
static void
DecodePrepare(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			  xl_xact_parsed_prepare *parsed)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogRecPtr	origin_lsn = parsed->origin_lsn;
	TimestampTz prepare_time = parsed->xact_time;
	RepOriginId origin_id = XLogRecGetOrigin(buf->record);
	int			i;
	TransactionId xid = parsed->twophase_xid;

	if (parsed->origin_timestamp != 0)
		prepare_time = parsed->origin_timestamp;

	/*
	 * 记住某事务的 prepare 信息，以便日后在需要时用于 
	 * commit prepared。参见 ReorderBufferFinishPrepared。      
	 */
	if (!ReorderBufferRememberPrepareInfo(ctx->reorder, xid, buf->origptr,
										  buf->endptr, prepare_time, origin_id,
										  origin_lsn))
		return;

	/* 在达到一致状态之前，我们无法开始流式发送。*/
	if (SnapBuildCurrentState(builder) < SNAPBUILD_CONSISTENT)
	{
		ReorderBufferSkipPrepare(ctx->reorder, xid);
		return;
	}

	/*
	 * 检查我们是否需要处理该事务。参见      
	 * DecodeTXNNeedSkip，了解我们有时想跳过该事务的        
	 * 原因。   
	 *
	 * 我们无法像在 DecodeCommit 中那样调用                       
	 * ReorderBufferForget，因为该事务尚未提交；在提交     
	 * 之前移除它可能导致 restart_lsn 计算错误。   
	 * 参见 SnapBuildProcessRunningXacts。但若其中存在  
	 * 缓存失效消息，则需处理，理由见 DecodeCommit 
	 * 中所述。 
	 */
	if (DecodeTXNNeedSkip(ctx, buf, parsed->dbId, origin_id))
	{
		ReorderBufferSkipPrepare(ctx->reorder, xid);
		ReorderBufferInvalidate(ctx->reorder, xid, buf->origptr);
		return;
	}

	/* 把存留的子事务告知 reorderbuffer。                */
	for (i = 0; i < parsed->nsubxacts; i++)
	{
		ReorderBufferCommitChild(ctx->reorder, xid, parsed->subxacts[i],
								 buf->origptr, buf->endptr);
	}

	/* 按顺序重放所有事务 + 子事务的动作             */
	ReorderBufferPrepare(ctx->reorder, xid, parsed->twophase_gid);

	/*
	 * 在事务准备/提交/中止时更新解码统计。         
	 * 此外，当把变更溢写到磁盘或流式发送时我们也会   
	 * 发送这些统计，以免在解码被中断时丢失它们。尚    
	 * 不清楚比这更频繁或更不频繁地发送是否更好。
	 */
	UpdateDecodingStats(ctx);
}


/*
 * 从各种形式的中止记录中取出数据，传给 snapbuild.c 与
 * reorderbuffer.c。              
 *
 * 'two_phase' 表示要完成已准备的事务。     
 */
static void
DecodeAbort(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			xl_xact_parsed_abort *parsed, TransactionId xid,
			bool two_phase)
{
	int			i;
	XLogRecPtr	origin_lsn = InvalidXLogRecPtr;
	TimestampTz abort_time = parsed->xact_time;
	RepOriginId origin_id = XLogRecGetOrigin(buf->record);
	bool		skip_xact;

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		origin_lsn = parsed->origin_lsn;
		abort_time = parsed->origin_timestamp;
	}

	/*
	 * 检查我们是否需要处理该事务。参见      
	 * DecodeTXNNeedSkip，了解我们有时想跳过该事务的        
	 * 原因。   
	 */
	skip_xact = DecodeTXNNeedSkip(ctx, buf, parsed->dbId, origin_id);

	/*
	 * 为已准备的事务发送最终的回滚记录，除非我们需 
	 * 要跳过它。对非两阶段事务，直接忘掉该事务。  
	 */
	if (two_phase && !skip_xact)
	{
		ReorderBufferFinishPrepared(ctx->reorder, xid, buf->origptr, buf->endptr,
									InvalidXLogRecPtr,
									abort_time, origin_id, origin_lsn,
									parsed->twophase_gid, false);
	}
	else
	{
		for (i = 0; i < parsed->nsubxacts; i++)
		{
			ReorderBufferAbort(ctx->reorder, parsed->subxacts[i],
							   buf->record->EndRecPtr, abort_time);
		}

		ReorderBufferAbort(ctx->reorder, xid, buf->record->EndRecPtr,
						   abort_time);
	}

	/* 更新解码统计        */
	UpdateDecodingStats(ctx);
}

/*
 * 解析 XLOG_HEAP_INSERT（非 MULTI_INSERT！）进元组缓冲。
 *
 * 删除可能包含新元组。    
 */
static void
DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	Size		datalen;
	char	   *tupledata;
	Size		tuplelen;
	XLogReaderState *r = buf->record;
	xl_heap_insert *xlrec;
	ReorderBufferChange *change;
	RelFileLocator target_locator;

	xlrec = (xl_heap_insert *) XLogRecGetData(r);

	/*
	 * 忽略没有新元组的插入记录（当 raw_heap_insert 将 
	 * TOAST 记录标为 HEAP_INSERT_NO_LOGICAL 时确实会发生）。
	 */
	if (!(xlrec->flags & XLH_INSERT_CONTAINS_NEW_TUPLE))
		return;

	/* 仅关注我们的数据库     */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* 输出插件不查找该 origin，无需入队               */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	if (!(xlrec->flags & XLH_INSERT_IS_SPECULATIVE))
		change->action = REORDER_BUFFER_CHANGE_INSERT;
	else
		change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	tupledata = XLogRecGetBlockData(r, 0, &datalen);
	tuplelen = datalen - SizeOfHeapHeader;

	change->data.tp.newtuple =
		ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

	DecodeXLogTuple(tupledata, datalen, change->data.tp.newtuple);

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change,
							 xlrec->flags & XLH_INSERT_ON_TOAST_RELATION);
}

/*
 * 解析 XLOG_HEAP_UPDATE 与 XLOG_HEAP_HOT_UPDATE（两者在记录中      
 * 布局相同），从WAL 读入元组缓冲。
 *
 * 更新操作可能同时包含新元组和旧的主键。        
 */
static void
DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_update *xlrec;
	ReorderBufferChange *change;
	char	   *data;
	RelFileLocator target_locator;

	xlrec = (xl_heap_update *) XLogRecGetData(r);

	/* 仅关注我们的数据库     */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* 输出插件不查找该 origin，无需入队               */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_UPDATE;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	if (xlrec->flags & XLH_UPDATE_CONTAINS_NEW_TUPLE)
	{
		Size		datalen;
		Size		tuplelen;

		data = XLogRecGetBlockData(r, 0, &datalen);

		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.newtuple =
			ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple(data, datalen, change->data.tp.newtuple);
	}

	if (xlrec->flags & XLH_UPDATE_CONTAINS_OLD)
	{
		Size		datalen;
		Size		tuplelen;

		/* 注意，记录中剩余的数据未对齐       */
		data = XLogRecGetData(r) + SizeOfHeapUpdate;
		datalen = XLogRecGetDataLen(r) - SizeOfHeapUpdate;
		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.oldtuple =
			ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple(data, datalen, change->data.tp.oldtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change, false);
}

/*
 * 从 WAL 解析 XLOG_HEAP_DELETE 进元组缓冲区。 
 *
 * 删除操作可能包含旧的主键。          
 */
static void
DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_delete *xlrec;
	ReorderBufferChange *change;
	RelFileLocator target_locator;

	xlrec = (xl_heap_delete *) XLogRecGetData(r);

	/* 仅关注我们的数据库     */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* 输出插件不查找该 origin，无需入队               */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);

	if (xlrec->flags & XLH_DELETE_IS_SUPER)
		change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT;
	else
		change->action = REORDER_BUFFER_CHANGE_DELETE;

	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	/* 已存储旧主键     */
	if (xlrec->flags & XLH_DELETE_CONTAINS_OLD)
	{
		Size		datalen = XLogRecGetDataLen(r) - SizeOfHeapDelete;
		Size		tuplelen = datalen - SizeOfHeapHeader;

		Assert(XLogRecGetDataLen(r) > (SizeOfHeapDelete + SizeOfHeapHeader));

		change->data.tp.oldtuple =
			ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple((char *) xlrec + SizeOfHeapDelete,
						datalen, change->data.tp.oldtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change, false);
}

/*
 * 从 WAL 解析 XLOG_HEAP_TRUNCATE
 */
static void
DecodeTruncate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_truncate *xlrec;
	ReorderBufferChange *change;

	xlrec = (xl_heap_truncate *) XLogRecGetData(r);

	/* 仅关注我们的数据库     */
	if (xlrec->dbId != ctx->slot->data.database)
		return;

	/* 输出插件不查找该 origin，无需入队               */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_TRUNCATE;
	change->origin_id = XLogRecGetOrigin(r);
	if (xlrec->flags & XLH_TRUNCATE_CASCADE)
		change->data.truncate.cascade = true;
	if (xlrec->flags & XLH_TRUNCATE_RESTART_SEQS)
		change->data.truncate.restart_seqs = true;
	change->data.truncate.nrelids = xlrec->nrelids;
	change->data.truncate.relids = ReorderBufferGetRelids(ctx->reorder,
														  xlrec->nrelids);
	memcpy(change->data.truncate.relids, xlrec->relids,
		   xlrec->nrelids * sizeof(Oid));
	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r),
							 buf->origptr, change, false);
}

/*
 * 把 XLOG_HEAP2_MULTI_INSERT_insert 解码进多个元组缓冲区。 
 *
 * 目前 MULTI_INSERT 总是包含完整元组。            
 */
static void
DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_multi_insert *xlrec;
	int			i;
	char	   *data;
	char	   *tupledata;
	Size		tuplelen;
	RelFileLocator rlocator;

	xlrec = (xl_heap_multi_insert *) XLogRecGetData(r);

	/*
	 * 忽略没有新元组的插入记录。multi_insert 针对的 
	 * 是系统表或非持久关系时就会发生这种情况。      
	 */
	if (!(xlrec->flags & XLH_INSERT_CONTAINS_NEW_TUPLE))
		return;

	/* 仅关注我们的数据库     */
	XLogRecGetBlockTag(r, 0, &rlocator, NULL, NULL);
	if (rlocator.dbOid != ctx->slot->data.database)
		return;

	/* 输出插件不查找该 origin，无需入队               */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	/*
	 * 我们知道这个 multi_insert 不是针对系统表的，因此即使
	 * 对它做了整页写入，该块也总会有数据。    
	 */
	tupledata = XLogRecGetBlockData(r, 0, &tuplelen);
	Assert(tupledata != NULL);

	data = tupledata;
	for (i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange *change;
		xl_multi_insert_tuple *xlhdr;
		int			datalen;
		ReorderBufferTupleBuf *tuple;
		HeapTupleHeader header;

		change = ReorderBufferGetChange(ctx->reorder);
		change->action = REORDER_BUFFER_CHANGE_INSERT;
		change->origin_id = XLogRecGetOrigin(r);

		memcpy(&change->data.tp.rlocator, &rlocator, sizeof(RelFileLocator));

		xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(data);
		data = ((char *) xlhdr) + SizeOfMultiInsertTuple;
		datalen = xlhdr->datalen;

		change->data.tp.newtuple =
			ReorderBufferGetTupleBuf(ctx->reorder, datalen);

		tuple = change->data.tp.newtuple;
		header = tuple->tuple.t_data;

		/* 非磁盘式元组     */
		ItemPointerSetInvalid(&tuple->tuple.t_self);

		/*
		 * 只有在重组各事务后我们才能弄清这一点。       
		 */
		tuple->tuple.t_tableOid = InvalidOid;

		tuple->tuple.t_len = datalen + SizeofHeapTupleHeader;

		memset(header, 0, SizeofHeapTupleHeader);

		memcpy((char *) tuple->tuple.t_data + SizeofHeapTupleHeader,
			   (char *) data,
			   datalen);
		header->t_infomask = xlhdr->t_infomask;
		header->t_infomask2 = xlhdr->t_infomask2;
		header->t_hoff = xlhdr->t_hoff;

		/*
		 * toast 重组状态的重置要等到 heap_multi_insert() 调用 
		 * 所发出的最后一条 xl_multi_insert_tuple 记录末行之 
		 * 后  
		 */
		if (xlrec->flags & XLH_INSERT_LAST_IN_MULTI &&
			(i + 1) == xlrec->ntuples)
			change->data.tp.clear_toast_afterwards = true;
		else
			change->data.tp.clear_toast_afterwards = false;

		ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r),
								 buf->origptr, change, false);

		/* 移到下一个 xl_multi_insert_tuple 条目 */
		data += datalen;
	}
	Assert(data == tupledata + tuplelen);
}

/*
 * 把 XLOG_HEAP_CONFIRM 从 WAL 解析成一条确认变更。
 *
 * 这相当简单，全部状态实质上已由推测插入设置       
 * 完毕。             
 */
static void
DecodeSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	ReorderBufferChange *change;
	RelFileLocator target_locator;

	/* 仅关注我们的数据库     */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* 输出插件不查找该 origin，无需入队               */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change, false);
}


/*
 * 读取 heap_insert、heap_update 与 heap_delete 写入 WAL 的 HeapTuple 
 * （不含 heap_multi_insert）进 tuplebuf。  
 *
 * 记录中 'len' 大小与 'data' 指针需在外部计算，  
 * 因为它们是随记录而异的。         
 */
static void
DecodeXLogTuple(char *data, Size len, ReorderBufferTupleBuf *tuple)
{
	xl_heap_header xlhdr;
	int			datalen = len - SizeOfHeapHeader;
	HeapTupleHeader header;

	Assert(datalen >= 0);

	tuple->tuple.t_len = datalen + SizeofHeapTupleHeader;
	header = tuple->tuple.t_data;

	/* 非磁盘式元组     */
	ItemPointerSetInvalid(&tuple->tuple.t_self);

	/* 只有在重组各事务后我们才能弄清这一点。       */
	tuple->tuple.t_tableOid = InvalidOid;

	/* 数据存储未对齐，复制到对齐存储中    */
	memcpy((char *) &xlhdr,
		   data,
		   SizeOfHeapHeader);

	memset(header, 0, SizeofHeapTupleHeader);

	memcpy(((char *) tuple->tuple.t_data) + SizeofHeapTupleHeader,
		   data + SizeOfHeapHeader,
		   datalen);

	header->t_infomask = xlhdr.t_infomask;
	header->t_infomask2 = xlhdr.t_infomask2;
	header->t_hoff = xlhdr.t_hoff;
}

/*
 * 检查我们是否对该特定事务感兴趣。             
 *
 * 我们可能对该事务不感兴趣的原因有               
 * 如下：   
 * 1) 我们可能不想解码截至此 LSN 为止的事务。      
 *	  原因可能是我们之前已经解码过它，现在只是      
 *	  重新启动；或者是尚未组装出一致的快照。           
 * 2) 该事务发生在另一个数据库中。   
 * 3) 输出插件对该 origin 不感兴趣。         
 * 4) 我们正在执行快进    
 */
static bool
DecodeTXNNeedSkip(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
				  Oid txn_dbid, RepOriginId origin_id)
{
	return (SnapBuildXactNeedsSkip(ctx->snapshot_builder, buf->origptr) ||
			(txn_dbid != InvalidOid && txn_dbid != ctx->slot->data.database) ||
			ctx->fast_forward || FilterByOrigin(ctx, origin_id));
}
