/*-------------------------------------------------------------------------
 *
 * logicalfuncs.c
 *
 *	   通过 SQL 使用逻辑解码和管理                        
 *	   逻辑复制槽的支持函数。 
 *
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/logicalfuncs.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/regproc.h"
#include "utils/resowner.h"

/* 用于写出数据的私有数据 */
typedef struct DecodingOutputState
{
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	bool		binary_output;
	int64		returned_rows;
} DecodingOutputState;

/*
 * 为输出插件写入做准备。  
 */
static void
LogicalOutputPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
						  bool last_write)
{
	resetStringInfo(ctx->out);
}

/*
 * 执行输出插件写入元组存储。     
 */
static void
LogicalOutputWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
				   bool last_write)
{
	Datum		values[3];
	bool		nulls[3];
	DecodingOutputState *p;

	/* SQL Datum 的长度只能是有限值……   */
	if (ctx->out->len > MaxAllocSize - VARHDRSZ)
		elog(ERROR, "too much output for sql interface");

	p = (DecodingOutputState *) ctx->output_writer_private;

	memset(nulls, 0, sizeof(nulls));
	values[0] = LSNGetDatum(lsn);
	values[1] = TransactionIdGetDatum(xid);

	/*
	 * 当写出文本输出时，断言 ctx->out 处于数据库编     
	 * 码。 
	 */
	if (!p->binary_output)
		Assert(pg_verify_mbstr(GetDatabaseEncoding(),
							   ctx->out->data, ctx->out->len,
							   false));

	/* 有点恶心，但 cstring_to_text_with_len对 bytea 完全适用*/
	values[2] = PointerGetDatum(cstring_to_text_with_len(ctx->out->data, ctx->out->len));

	tuplestore_putvalues(p->tupstore, p->tupdesc, values, nulls);
	p->returned_rows++;
}

/*
 * 供各种可由 SQL 调用的逻辑解码函数使用的辅助函数。 
 */
static Datum
pg_logical_slot_get_changes_guts(FunctionCallInfo fcinfo, bool confirm, bool binary)
{
	Name		name;
	XLogRecPtr	upto_lsn;
	int32		upto_nchanges;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	XLogRecPtr	end_of_wal;
	LogicalDecodingContext *ctx;
	ResourceOwner old_resowner = CurrentResourceOwner;
	ArrayType  *arr;
	Size		ndim;
	List	   *options = NIL;
	DecodingOutputState *p;

	CheckSlotPermissions();

	CheckLogicalDecodingRequirements();

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("slot name must not be null")));
	name = PG_GETARG_NAME(0);

	if (PG_ARGISNULL(1))
		upto_lsn = InvalidXLogRecPtr;
	else
		upto_lsn = PG_GETARG_LSN(1);

	if (PG_ARGISNULL(2))
		upto_nchanges = InvalidXLogRecPtr;
	else
		upto_nchanges = PG_GETARG_INT32(2);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("options array must not be null")));
	arr = PG_GETARG_ARRAYTYPE_P(3);

	/* 输出写往的状态    */
	p = palloc0(sizeof(DecodingOutputState));

	p->binary_output = binary;

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* 解构选项数组        */
	ndim = ARR_NDIM(arr);
	if (ndim > 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("array must be one-dimensional")));
	}
	else if (array_contains_nulls(arr))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("array must not contain nulls")));
	}
	else if (ndim == 1)
	{
		int			nelems;
		Datum	   *datum_opts;
		int			i;

		Assert(ARR_ELEMTYPE(arr) == TEXTOID);

		deconstruct_array_builtin(arr, TEXTOID, &datum_opts, NULL, &nelems);

		if (nelems % 2 != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array must have even number of elements")));

		for (i = 0; i < nelems; i += 2)
		{
			char	   *name = TextDatumGetCString(datum_opts[i]);
			char	   *opt = TextDatumGetCString(datum_opts[i + 1]);

			options = lappend(options, makeDefElem(name, (Node *) makeString(opt), -1));
		}
	}

	InitMaterializedSRF(fcinfo, 0);
	p->tupstore = rsinfo->setResult;
	p->tupdesc = rsinfo->setDesc;

	/*
	 * 计算当前的 WAL 末尾。  
	 */
	if (!RecoveryInProgress())
		end_of_wal = GetFlushRecPtr(NULL);
	else
		end_of_wal = GetXLogReplayRecPtr(NULL);

	ReplicationSlotAcquire(NameStr(*name), true);

	PG_TRY();
	{
		/* 从槽 confirmed_flush重新开始*/
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									options,
									false,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									LogicalOutputPrepareWrite,
									LogicalOutputWrite, NULL);

		MemoryContextSwitchTo(oldcontext);

		/*
		 * 检查输出插件是否写文本输出，如果正是我们   
		 * 所需的。 
		 */
		if (!binary &&
			ctx->options.output_type !=OUTPUT_PLUGIN_TEXTUAL_OUTPUT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("logical decoding output plugin \"%s\" produces binary output, but function \"%s\" expects textual data",
							NameStr(MyReplicationSlot->data.plugin),
							format_procedure(fcinfo->flinfo->fn_oid))));

		ctx->output_writer_private = p;

		/*
		 * WAL 的解码必须从 restart_lsn 开始，这样在槽的      
		 * confirmed_flush 之后提交的事务才能全部累积到  
		 * 重排序缓冲区中。         
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* 使非时间旅行条目失效    */
		InvalidateSystemCaches();

		/* 解码直到记录耗尽           */
		while (ctx->reader->EndRecPtr < end_of_wal)
		{
			XLogRecord *record;
			char	   *errm = NULL;

			record = XLogReadRecord(ctx->reader, &errm);
			if (errm)
				elog(ERROR, "could not find record for logical decoding: %s", errm);

			/*
			 * 上面的 {begin_txn,change,commit_txn}_wrapper 回调函     
			 * 数会把描述信息存入元组存储。
			 */
			if (record != NULL)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			/* 检查限制 */
			if (upto_lsn != InvalidXLogRecPtr &&
				upto_lsn <= ctx->reader->EndRecPtr)
				break;
			if (upto_nchanges != 0 &&
				upto_nchanges <= p->returned_rows)
				break;
			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * 逻辑解码在事务管理期间可能已经破坏了           
		 * CurrentResourceOwner，因此需要恢复执行器的值。       
		 * （是权宜之计，但现在不值得清理。）
		 */
		CurrentResourceOwner = old_resowner;

		/*
		 * 下次从上次停下的地方开始。（猎魔之事，家族
		 * 生意..）
		 */
		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr && confirm)
		{
			LogicalConfirmReceivedLocation(ctx->reader->EndRecPtr);

			/*
			 * 如果只有 confirmed_flush_lsn 发生变化，槽不会被  
			 * 上面的代码标记为脏。WAL 发送进程接口  
			 * 上的调用者应自行跟踪进度，无需将其写出。  
			 * 但 SQL 接口用户无法指定自己的起始位置， 
			 * 而且对他们来说进度更难跟踪，所以             
			 * 我们应当更加努力地为他们保存                
			 * 进度。                  
			 *
			 * 将槽标记为脏，让它在下一个检查点写出。 
			 * 如文档所述，崩溃时我们仍会丢失它的位置，但
			 * 这总好过即使在干净重启后也总是丢失位置。 
			 */
			ReplicationSlotMarkDirty();
		}

		/* 释放上下文，调用关闭回调 */
		FreeDecodingContext(ctx);

		ReplicationSlotRelease();
		InvalidateSystemCaches();
	}
	PG_CATCH();
	{
		/* 清空时间旅行条目     */
		InvalidateSystemCaches();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return (Datum) 0;
}

/*
 * 以文本形式返回变更流并消费数据的 SQL 函数。      
 */
Datum
pg_logical_slot_get_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, true, false);
}

/*
 * 以文本形式返回变更流且只向前窥视的 SQL 函数。   
 */
Datum
pg_logical_slot_peek_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, false, false);
}

/*
 * 以二进制形式返回变更流并消费数据的 SQL 函数。     
 */
Datum
pg_logical_slot_get_binary_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, true, true);
}

/*
 * 以二进制形式返回变更流且只向前窥视的 SQL 函数。  
 */
Datum
pg_logical_slot_peek_binary_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, false, true);
}


/*
 * 用于将逻辑解码消息写入 WAL 的 SQL 函数。    
 */
Datum
pg_logical_emit_message_bytea(PG_FUNCTION_ARGS)
{
	bool		transactional = PG_GETARG_BOOL(0);
	char	   *prefix = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bytea	   *data = PG_GETARG_BYTEA_PP(2);
	XLogRecPtr	lsn;

	lsn = LogLogicalMessage(prefix, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data),
							transactional);
	PG_RETURN_LSN(lsn);
}

Datum
pg_logical_emit_message_text(PG_FUNCTION_ARGS)
{
	/* bytea 与 text 兼容         */
	return pg_logical_emit_message_bytea(fcinfo);
}
