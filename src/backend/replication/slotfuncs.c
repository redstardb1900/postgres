/*-------------------------------------------------------------------------
 *
 * slotfuncs.c
 *	   复制槽的支持函数               
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/slotfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/slot.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/pg_lsn.h"
#include "utils/resowner.h"

/*
 * 用于以给定参数创建新物理复制槽的辅助函数。  
 * 注意该函数不会释放所创建的槽。                       
 *      
 *
 * 如果 restart_lsn 是有效值，我们不经 WAL 保留例程   
 * 就直接使用。调用者必须保证 WAL 可用。       
 */
static void
create_physical_replication_slot(char *name, bool immediately_reserve,
								 bool temporary, XLogRecPtr restart_lsn)
{
	Assert(!MyReplicationSlot);

	/* 获取复制槽，此操作会检查是否存在名称冲突    */
	ReplicationSlotCreate(name, false,
						  temporary ? RS_TEMPORARY : RS_PERSISTENT, false);

	if (immediately_reserve)
	{
		/* 按用户的要求保留 WAL         */
		if (XLogRecPtrIsInvalid(restart_lsn))
			ReplicationSlotReserveWal();
		else
			MyReplicationSlot->data.restart_lsn = restart_lsn;

		/* 把该槽写入磁盘   */
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
	}
}

/*
 * 创建新的物理（流复制）复制槽的                   
 * SQL 函数。    
 */
Datum
pg_create_physical_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		immediately_reserve = PG_GETARG_BOOL(1);
	bool		temporary = PG_GETARG_BOOL(2);
	Datum		values[2];
	bool		nulls[2];
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		result;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	CheckSlotPermissions();

	CheckSlotRequirements();

	create_physical_replication_slot(NameStr(*name),
									 immediately_reserve,
									 temporary,
									 InvalidXLogRecPtr);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	nulls[0] = false;

	if (immediately_reserve)
	{
		values[1] = LSNGetDatum(MyReplicationSlot->data.restart_lsn);
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}


/*
 * 用于以给定参数创建新逻辑复制槽的辅助函数。 
 * 注意该函数不会释放所创建的槽。                       
 *      
 *
 * 当 find_startpoint 为假时，不会设置槽的 confirmed_flush；     
 * 确保它被设为合理的值是调用者的责任。           
 */
static void
create_logical_replication_slot(char *name, char *plugin,
								bool temporary, bool two_phase,
								XLogRecPtr restart_lsn,
								bool find_startpoint)
{
	LogicalDecodingContext *ctx = NULL;

	Assert(!MyReplicationSlot);

	/*
	 * 获取逻辑解码槽，这一操作会检查名称冲突。           
	 * 初始将持久槽作为瞬态槽创建——这样我们能     
	 * 漂亮地处理初始化期间的错误，因为事务失败时它会  
	 * 被删除。最后我们会使其变为持久。临时槽则可以    
	 * 从一开始就作为临时槽创建，因为它们出错时同样     
	 * 会删除。  
	 */
	ReplicationSlotCreate(name, true,
						  temporary ? RS_TEMPORARY : RS_EPHEMERAL, two_phase);

	/*
	 * 创建逻辑解码上下文来寻找起点，或若我们不需要 
	 * 它，1) 提升槽的 restart_lsn 与 xmin 2) 检查插件健全性。 
	 *
	 * 注意：当 !find_startpoint 时这仍然重要，因为正是在  
	 * 此处，输出插件得到验证。           
	 */
	ctx = CreateInitDecodingContext(plugin, NIL,
									false,	/* 仅系统表即可  */
									restart_lsn,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

	/*
	 * 如果调用者需要我们确定解码起点，现在就来做。  
	 * 这可能耗时较久。
	 */
	if (find_startpoint)
		DecodingContextFindStartpoint(ctx);

	/* 不再需要解码上下文             */
	FreeDecodingContext(ctx);
}

/*
 * 创建新的逻辑复制槽的 SQL 函数。             
 */
Datum
pg_create_logical_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	Name		plugin = PG_GETARG_NAME(1);
	bool		temporary = PG_GETARG_BOOL(2);
	bool		two_phase = PG_GETARG_BOOL(3);
	Datum		result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[2];
	bool		nulls[2];

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	CheckSlotPermissions();

	CheckLogicalDecodingRequirements();

	create_logical_replication_slot(NameStr(*name),
									NameStr(*plugin),
									temporary,
									two_phase,
									InvalidXLogRecPtr,
									true);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	values[1] = LSNGetDatum(MyReplicationSlot->data.confirmed_flush);

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	/* 好，槽现已完全创建，必要时将其标记为持久槽*/
	if (!temporary)
		ReplicationSlotPersist();
	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}


/*
 * 用于删除复制槽的 SQL 函数。       
 */
Datum
pg_drop_replication_slot(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);

	CheckSlotPermissions();

	CheckSlotRequirements();

	ReplicationSlotDrop(NameStr(*name), true);

	PG_RETURN_VOID();
}

/*
 * pg_get_replication_slots —— 显示数据库集簇上当前  
 * 存在的所有复制槽的 SQL SRF。       
 */
Datum
pg_get_replication_slots(PG_FUNCTION_ARGS)
{
#define PG_GET_REPLICATION_SLOTS_COLS 15
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	XLogRecPtr	currlsn;
	int			slotno;

	/*
	 * 查看该函数的数据不需要任何特殊权限，因为其中 
	 * 不应有敏感信息。其中最关键的当属槽的名称，它   
	 * 不应包含任何特别敏感的内容。                    
	 */

	InitMaterializedSRF(fcinfo, 0);

	currlsn = GetXLogWriteRecPtr();

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (slotno = 0; slotno < max_replication_slots; slotno++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[slotno];
		ReplicationSlot slot_contents;
		Datum		values[PG_GET_REPLICATION_SLOTS_COLS];
		bool		nulls[PG_GET_REPLICATION_SLOTS_COLS];
		WALAvailability walstate;
		int			i;

		if (!slot->in_use)
			continue;

		/* 持有自旋锁时复制槽内容，之后再从容检查          */
		SpinLockAcquire(&slot->mutex);
		slot_contents = *slot;
		SpinLockRelease(&slot->mutex);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		i = 0;
		values[i++] = NameGetDatum(&slot_contents.data.name);

		if (slot_contents.data.database == InvalidOid)
			nulls[i++] = true;
		else
			values[i++] = NameGetDatum(&slot_contents.data.plugin);

		if (slot_contents.data.database == InvalidOid)
			values[i++] = CStringGetTextDatum("physical");
		else
			values[i++] = CStringGetTextDatum("logical");

		if (slot_contents.data.database == InvalidOid)
			nulls[i++] = true;
		else
			values[i++] = ObjectIdGetDatum(slot_contents.data.database);

		values[i++] = BoolGetDatum(slot_contents.data.persistency == RS_TEMPORARY);
		values[i++] = BoolGetDatum(slot_contents.active_pid != 0);

		if (slot_contents.active_pid != 0)
			values[i++] = Int32GetDatum(slot_contents.active_pid);
		else
			nulls[i++] = true;

		if (slot_contents.data.xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(slot_contents.data.xmin);
		else
			nulls[i++] = true;

		if (slot_contents.data.catalog_xmin != InvalidTransactionId)
			values[i++] = TransactionIdGetDatum(slot_contents.data.catalog_xmin);
		else
			nulls[i++] = true;

		if (slot_contents.data.restart_lsn != InvalidXLogRecPtr)
			values[i++] = LSNGetDatum(slot_contents.data.restart_lsn);
		else
			nulls[i++] = true;

		if (slot_contents.data.confirmed_flush != InvalidXLogRecPtr)
			values[i++] = LSNGetDatum(slot_contents.data.confirmed_flush);
		else
			nulls[i++] = true;

		/*
		 * 若该槽未失效，则从 restart_lsn 测试可用性。  
		 *             
		 */
		if (slot_contents.data.invalidated != RS_INVAL_NONE)
			walstate = WALAVAIL_REMOVED;
		else
			walstate = GetWALAvailability(slot_contents.data.restart_lsn);

		switch (walstate)
		{
			case WALAVAIL_INVALID_LSN:
				nulls[i++] = true;
				break;

			case WALAVAIL_RESERVED:
				values[i++] = CStringGetTextDatum("reserved");
				break;

			case WALAVAIL_EXTENDED:
				values[i++] = CStringGetTextDatum("extended");
				break;

			case WALAVAIL_UNRESERVED:
				values[i++] = CStringGetTextDatum("unreserved");
				break;

			case WALAVAIL_REMOVED:

				/*
				 * 若 restart_lsn 读取于很久前，该文件可能已    
				 * 被删了。不过 WAL 发送进程可能前进很远，   
				 * 以致在我们查看后跳到了另一文件。如果   
				 * checkpointer 已发信号令其终止，那肯定   
				 * 已丢失；但若进程仍存活，"unreserved"           
				 * 似乎更贴切。                                    
				 *
				 * 若改动，则为下方的 safe_wal_size 保存状态。  
				 */
				if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
				{
					int			pid;

					SpinLockAcquire(&slot->mutex);
					pid = slot->active_pid;
					slot_contents.data.restart_lsn = slot->data.restart_lsn;
					SpinLockRelease(&slot->mutex);
					if (pid != 0)
					{
						values[i++] = CStringGetTextDatum("unreserved");
						walstate = WALAVAIL_UNRESERVED;
						break;
					}
				}
				values[i++] = CStringGetTextDatum("lost");
				break;
		}

		/*
		 * safe_wal_size 只为尚未丢失的槽计算，并且仅当      
		 * 存在配置的最大尺寸时才会计算。 
		 */
		if (walstate == WALAVAIL_REMOVED || max_slot_wal_keep_size_mb < 0)
			nulls[i++] = true;
		else
		{
			XLogSegNo	targetSeg;
			uint64		slotKeepSegs;
			uint64		keepSegs;
			XLogSegNo	failSeg;
			XLogRecPtr	failLSN;

			XLByteToSeg(slot_contents.data.restart_lsn, targetSeg, wal_segment_size);

			/* 确定各槽合计能保留多少个段          */
			slotKeepSegs = XLogMBVarToSegs(max_slot_wal_keep_size_mb, wal_segment_size);
			/* wal_keep_size 同理    */
			keepSegs = XLogMBVarToSegs(wal_keep_size_mb, wal_segment_size);

			/* 若 currpos 到达 failLSN 就会丢失该段   */
			failSeg = targetSeg + Max(slotKeepSegs, keepSegs) + 1;
			XLogSegNoOffsetToRecPtr(failSeg, 0, wal_segment_size, failLSN);

			values[i++] = Int64GetDatum(failLSN - currlsn);
		}

		values[i++] = BoolGetDatum(slot_contents.data.two_phase);

		if (slot_contents.data.database == InvalidOid)
			nulls[i++] = true;
		else
		{
			if (slot_contents.data.invalidated != RS_INVAL_NONE)
				values[i++] = BoolGetDatum(true);
			else
				values[i++] = BoolGetDatum(false);
		}

		Assert(i == PG_GET_REPLICATION_SLOTS_COLS);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	LWLockRelease(ReplicationSlotControlLock);

	return (Datum) 0;
}

/*
 * 将我们的物理复制槽向前推进的辅助函数。           
 *
 * 要移到的 LSN 位置仅与槽的 restart_lsn 简单比较，          
 * 因为我们知道任何比它更旧的位置都会被后续的检查点
 * 所删除。
 */
static XLogRecPtr
pg_physical_replication_slot_advance(XLogRecPtr moveto)
{
	XLogRecPtr	startlsn = MyReplicationSlot->data.restart_lsn;
	XLogRecPtr	retlsn = startlsn;

	Assert(moveto != InvalidXLogRecPtr);

	if (startlsn < moveto)
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->data.restart_lsn = moveto;
		SpinLockRelease(&MyReplicationSlot->mutex);
		retlsn = moveto;

		/*
		 * 弄脏该槽使其在下次检查点时被写出。注意，       
		 * 已推进的 LSN 位置在发生崩溃时仍可能丢失，       
		 * 但这样能使数据在干净关闭后保持一致。           
		 */
		ReplicationSlotMarkDirty();
	}

	return retlsn;
}

/*
 * 将我们的逻辑复制槽向前推进的辅助函数。          
 *
 * 槽的 restart_lsn 被用作读取记录的起点，而                 
 * confirmed_flush 则被用作解码上下文的基点。        
 *
 * 我们无法只用 LogicalConfirmReceivedLocation 更新 confirmed_flush，
 * 因为我们需要消化 WAL 来推进 restart_lsn，从而允许回收 
 * WAL 并移除旧的系统表元组。由于解码以 fast_forward 模式   
 * 进行，反正不会生成变更。  
 */
static XLogRecPtr
pg_logical_replication_slot_advance(XLogRecPtr moveto)
{
	LogicalDecodingContext *ctx;
	ResourceOwner old_resowner = CurrentResourceOwner;
	XLogRecPtr	retlsn;

	Assert(moveto != InvalidXLogRecPtr);

	PG_TRY();
	{
		/*
		 * 以 fast_forward 模式创建解码上下文，传入 start_lsn    
		 * 作为 InvalidXLogRecPtr，这样我们就从本槽的          
		 * confirmed_flush 
		 */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									NIL,
									true,	/* fast_forward */
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

		/*
		 * 从槽的 restart_lsn 处开始读取，已知它指向           
		 * 有效记录。
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* 使非时间旅行条目失效    */
		InvalidateSystemCaches();

		/* 至少解码一条记录，直到记录用尽为止     */
		while (ctx->reader->EndRecPtr < moveto)
		{
			char	   *errm = NULL;
			XLogRecord *record;

			/*
			 * 读取记录。fast_forward 模式下不生成变更，      
			 * 但 snapbuilder/槽状态仍会正确更新。     
			 */
			record = XLogReadRecord(ctx->reader, &errm);
			if (errm)
				elog(ERROR, "could not find record while advancing replication slot: %s",
					 errm);

			/*
			 * 处理该记录。存储层级的变更在               
			 * fast_forward 模式下被忽略，但其他模块（如   
			 * snapbuilder）可能有紧要更新。  
			 */
			if (record)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			/* 到达所请求的目标后即停止            */
			if (moveto <= ctx->reader->EndRecPtr)
				break;

			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * 逻辑解码可能在事务管理期间破坏了                 
		 * CurrentResourceOwner，所以请恢复执行器的值。（这是 
		 * 一种蹩脚做法，但现在不值得清理。）
		 */
		CurrentResourceOwner = old_resowner;

		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr)
		{
			LogicalConfirmReceivedLocation(moveto);

			/*
			 * 若只有 confirmed_flush LSN 发生变化，上面的代码  
			 * 不会将槽标脏。WAL 发送进程接口           
			 * 上的调用者应当跟踪进度，无需写出。但 SQL    
			 * 接口用户无法指定自己的起点，对他们来说
			 * 跟踪自身进度更困难，因此我们应付出更多    
			 * 努力为他们保存进度。                            
			 *                            
			 *
			 * 弄脏该槽，使其在下次检查点时被写出。推进到
			 * 的 LSN 位置崩溃时仍可能丢失，但这能让数据在
			 * 干净关闭后保持一致。                   
			 */
			ReplicationSlotMarkDirty();
		}

		retlsn = MyReplicationSlot->data.confirmed_flush;

		/* 释放上下文，调用关闭回调 */
		FreeDecodingContext(ctx);

		InvalidateSystemCaches();
	}
	PG_CATCH();
	{
		/* 清空时间旅行条目     */
		InvalidateSystemCaches();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return retlsn;
}

/*
 * 用于移动复制槽中位置的 SQL 函数。            
 */
Datum
pg_replication_slot_advance(PG_FUNCTION_ARGS)
{
	Name		slotname = PG_GETARG_NAME(0);
	XLogRecPtr	moveto = PG_GETARG_LSN(1);
	XLogRecPtr	endlsn;
	XLogRecPtr	minlsn;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2];
	HeapTuple	tuple;
	Datum		result;

	Assert(!MyReplicationSlot);

	CheckSlotPermissions();

	if (XLogRecPtrIsInvalid(moveto))
		ereport(ERROR,
				(errmsg("invalid target WAL LSN")));

	/* 为我们的结果类型构建元组描述符*/
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * 无法把槽移过已刷写/已重放的位置，因此需将    
	 * 目标位置相应收紧。 
	 */
	if (!RecoveryInProgress())
		moveto = Min(moveto, GetFlushRecPtr(NULL));
	else
		moveto = Min(moveto, GetXLogReplayRecPtr(NULL));

	/* 获取槽，使我们"拥有"它*/
	ReplicationSlotAcquire(NameStr(*slotname), true);

	/* restart_lsn 从未被保留过的槽无法被推进                 */
	if (XLogRecPtrIsInvalid(MyReplicationSlot->data.restart_lsn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" cannot be advanced",
						NameStr(*slotname)),
				 errdetail("This slot has never previously reserved WAL, or it has been invalidated.")));

	/*
	 * 检查该槽是否没有向后移动。物理槽简单地只依赖    
	 * restart_lsn 作为最小点，而逻辑槽已确认消费到         
	 * confirmed_flush，这意味着两种情况下比该位置更旧   
	 * 的数据都已不再可用。           
	 */
	if (OidIsValid(MyReplicationSlot->data.database))
		minlsn = MyReplicationSlot->data.confirmed_flush;
	else
		minlsn = MyReplicationSlot->data.restart_lsn;

	if (moveto < minlsn)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot advance replication slot to %X/%X, minimum is %X/%X",
						LSN_FORMAT_ARGS(moveto), LSN_FORMAT_ARGS(minlsn))));

	/* 根据槽类型执行实际的槽更新               */
	if (OidIsValid(MyReplicationSlot->data.database))
		endlsn = pg_logical_replication_slot_advance(moveto);
	else
		endlsn = pg_physical_replication_slot_advance(moveto);

	values[0] = NameGetDatum(&MyReplicationSlot->data.name);
	nulls[0] = false;

	/*
	 * 重新计算所有槽中的最小 LSN 与 xmin，以针对可能已   
	 * 发生的推进。         
	 */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();

	ReplicationSlotRelease();

	/* 返回已到达的位置。  */
	values[1] = LSNGetDatum(endlsn);
	nulls[1] = false;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * 对复制槽执行复制的辅助函数。    
 */
static Datum
copy_replication_slot(FunctionCallInfo fcinfo, bool logical_slot)
{
	Name		src_name = PG_GETARG_NAME(0);
	Name		dst_name = PG_GETARG_NAME(1);
	ReplicationSlot *src = NULL;
	ReplicationSlot first_slot_contents;
	ReplicationSlot second_slot_contents;
	XLogRecPtr	src_restart_lsn;
	bool		src_islogical;
	bool		temporary;
	char	   *plugin;
	Datum		values[2];
	bool		nulls[2];
	Datum		result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	CheckSlotPermissions();

	if (logical_slot)
		CheckLogicalDecodingRequirements();
	else
		CheckSlotRequirements();

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	/*
	 * 我们需要防止源槽已保留的 WAL 被移除，                
	 * 但我们又不想把那个槽锁定太久，而且在此期间它   
	 * 可能会前进。因此先取得源槽的数据，并用其        
	 * restart_lsn 创建一个新槽。随后我们再次锁定源槽，   
	 * 并验证我们复制的数据（名称、类型）没有发生
	 * 不兼容的变化。一旦新槽被创建，就不会再发生       
	 * 讨厌的 WAL 移除——但在我们设法创建新槽之前，
	 * WAL 移除可能已经发生，所以我们在第二次锁定它     
	 * 时将新槽 restart_lsn 前进到源槽更新后的 restart_lsn。 
	 */
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(NameStr(s->data.name), NameStr(*src_name)) == 0)
		{
			/* 在持有自旋锁时复制槽内容          */
			SpinLockAcquire(&s->mutex);
			first_slot_contents = *s;
			SpinLockRelease(&s->mutex);
			src = s;
			break;
		}
	}

	LWLockRelease(ReplicationSlotControlLock);

	if (src == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist", NameStr(*src_name))));

	src_islogical = SlotIsLogical(&first_slot_contents);
	src_restart_lsn = first_slot_contents.data.restart_lsn;
	temporary = (first_slot_contents.data.persistency == RS_TEMPORARY);
	plugin = logical_slot ? NameStr(first_slot_contents.data.plugin) : NULL;

	/* 检查复制槽的类型       */
	if (src_islogical != logical_slot)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 src_islogical ?
				 errmsg("cannot copy physical replication slot \"%s\" as a logical replication slot",
						NameStr(*src_name)) :
				 errmsg("cannot copy logical replication slot \"%s\" as a physical replication slot",
						NameStr(*src_name))));

	/* 复制非保留槽并无意义               */
	if (XLogRecPtrIsInvalid(src_restart_lsn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot copy a replication slot that doesn't reserve WAL")));

	/* 无法复制已失效的复制槽           */
	if (first_slot_contents.data.invalidated != RS_INVAL_NONE)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot copy invalidated replication slot \"%s\"",
					   NameStr(*src_name)));

	/* 用可选参数覆盖这些参数        */
	if (PG_NARGS() >= 3)
		temporary = PG_GETARG_BOOL(2);
	if (PG_NARGS() >= 4)
	{
		Assert(logical_slot);
		plugin = NameStr(*(PG_GETARG_NAME(3)));
	}

	/* 创建新槽并获取它       */
	if (logical_slot)
	{
		/*
		 * 我们无法尝试读取 WAL，因为尚未保留它 ——     
		 * 因此给 find_startpoint 传入假值。confirmed_flush 将  
		 * 在下方通过复制源槽设置。   
		 */
		create_logical_replication_slot(NameStr(*dst_name),
										plugin,
										temporary,
										false,
										src_restart_lsn,
										false);
	}
	else
		create_physical_replication_slot(NameStr(*dst_name),
										 true,
										 temporary,
										 src_restart_lsn);

	/*
	 * 将目标槽更新为源槽的当前值；                       
	 * 再次检查源槽是否仍是我们先前看到的那个。    
	 */
	{
		TransactionId copy_effective_xmin;
		TransactionId copy_effective_catalog_xmin;
		TransactionId copy_xmin;
		TransactionId copy_catalog_xmin;
		XLogRecPtr	copy_restart_lsn;
		XLogRecPtr	copy_confirmed_flush;
		bool		copy_islogical;
		char	   *copy_name;

		/* 再次复制源槽的数据    */
		SpinLockAcquire(&src->mutex);
		second_slot_contents = *src;
		SpinLockRelease(&src->mutex);

		copy_effective_xmin = second_slot_contents.effective_xmin;
		copy_effective_catalog_xmin = second_slot_contents.effective_catalog_xmin;

		copy_xmin = second_slot_contents.data.xmin;
		copy_catalog_xmin = second_slot_contents.data.catalog_xmin;
		copy_restart_lsn = second_slot_contents.data.restart_lsn;
		copy_confirmed_flush = second_slot_contents.data.confirmed_flush;

		/* 供存在性检查  */
		copy_name = NameStr(second_slot_contents.data.name);
		copy_islogical = SlotIsLogical(&second_slot_contents);

		/*
		 * 检查源槽是否仍存在且有效。若发生以下情况，    
		 * 则视为无效：复制槽类型或名称已被改变，        
		 * 或者 restart_lsn 无效或已倒退。（                    
		 * 若源槽被删除并在安装期间从更旧的槽复制，    
		 * 则 restart_lsn 就可能倒退。）          
		 *
		 * 由于报错退出时会释放并删除目标槽，我们无需 
		 * 在此处释放它。         
		 */
		if (copy_restart_lsn < src_restart_lsn ||
			src_islogical != copy_islogical ||
			strcmp(copy_name, NameStr(*src_name)) != 0)
			ereport(ERROR,
					(errmsg("could not copy replication slot \"%s\"",
							NameStr(*src_name)),
					 errdetail("The source replication slot was modified incompatibly during the copy operation.")));

		/* 源槽必须具有一致的快照               */
		if (src_islogical && XLogRecPtrIsInvalid(copy_confirmed_flush))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot copy unfinished logical replication slot \"%s\"",
							NameStr(*src_name)),
					 errhint("Retry when the source replication slot's confirmed_flush_lsn is valid.")));

		/*
		 * 复制无效的槽并无意义。请注意，源槽有可能在 
		 * 我们创建新槽并复制源槽数据之后变得无效。    
		 * 这是因为 InvalidateObsoleteReplicationSlots()              
		 * 中的操作未与本函数串行化。尽管我们无法在     
		 * 此处检测到这种情况，被复制的槽也会在下一个   
		 * 检查点周期中变为无效。                     
		 */
		if (second_slot_contents.data.invalidated != RS_INVAL_NONE)
			ereport(ERROR,
					errmsg("cannot copy replication slot \"%s\"",
						   NameStr(*src_name)),
					errdetail("The source replication slot was invalidated during the copy operation."));

		/* 再次装回所复制的值 */
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->effective_xmin = copy_effective_xmin;
		MyReplicationSlot->effective_catalog_xmin = copy_effective_catalog_xmin;

		MyReplicationSlot->data.xmin = copy_xmin;
		MyReplicationSlot->data.catalog_xmin = copy_catalog_xmin;
		MyReplicationSlot->data.restart_lsn = copy_restart_lsn;
		MyReplicationSlot->data.confirmed_flush = copy_confirmed_flush;
		SpinLockRelease(&MyReplicationSlot->mutex);

		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
		ReplicationSlotSave();

#ifdef USE_ASSERT_CHECKING
		/* 检查 restart_lsn 是否可用         */
		{
			XLogSegNo	segno;

			XLByteToSeg(copy_restart_lsn, segno, wal_segment_size);
			Assert(XLogGetLastRemovedSegno() < segno);
		}
#endif
	}

	/* 目标槽已完全创建，必要时标记为持久槽  */
	if (logical_slot && !temporary)
		ReplicationSlotPersist();

	/* 全部完成。设置返回值。   */
	values[0] = NameGetDatum(dst_name);
	nulls[0] = false;
	if (!XLogRecPtrIsInvalid(MyReplicationSlot->data.confirmed_flush))
	{
		values[1] = LSNGetDatum(MyReplicationSlot->data.confirmed_flush);
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	ReplicationSlotRelease();

	PG_RETURN_DATUM(result);
}

/* 下面的包装都是为了安抚 opr_sanity     */
Datum
pg_copy_logical_replication_slot_a(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, true);
}

Datum
pg_copy_logical_replication_slot_b(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, true);
}

Datum
pg_copy_logical_replication_slot_c(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, true);
}

Datum
pg_copy_physical_replication_slot_a(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, false);
}

Datum
pg_copy_physical_replication_slot_b(PG_FUNCTION_ARGS)
{
	return copy_replication_slot(fcinfo, false);
}
