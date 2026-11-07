/*-------------------------------------------------------------------------
 *
 * pgoutput.c
 *		逻辑复制输出插件         
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/replication/pgoutput/pgoutput.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupconvert.h"
#include "catalog/partition.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_subscription.h"
#include "commands/defrem.h"
#include "commands/subscriptioncmds.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "replication/logical.h"
#include "replication/logicalproto.h"
#include "replication/origin.h"
#include "replication/pgoutput.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

static void pgoutput_startup(LogicalDecodingContext *ctx,
							 OutputPluginOptions *opt, bool is_init);
static void pgoutput_shutdown(LogicalDecodingContext *ctx);
static void pgoutput_begin_txn(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn);
static void pgoutput_commit_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pgoutput_change(LogicalDecodingContext *ctx,
							ReorderBufferTXN *txn, Relation relation,
							ReorderBufferChange *change);
static void pgoutput_truncate(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, int nrelations, Relation relations[],
							  ReorderBufferChange *change);
static void pgoutput_message(LogicalDecodingContext *ctx,
							 ReorderBufferTXN *txn, XLogRecPtr message_lsn,
							 bool transactional, const char *prefix,
							 Size sz, const char *message);
static bool pgoutput_origin_filter(LogicalDecodingContext *ctx,
								   RepOriginId origin_id);
static void pgoutput_begin_prepare_txn(LogicalDecodingContext *ctx,
									   ReorderBufferTXN *txn);
static void pgoutput_prepare_txn(LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn, XLogRecPtr prepare_lsn);
static void pgoutput_commit_prepared_txn(LogicalDecodingContext *ctx,
										 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pgoutput_rollback_prepared_txn(LogicalDecodingContext *ctx,
										   ReorderBufferTXN *txn,
										   XLogRecPtr prepare_end_lsn,
										   TimestampTz prepare_time);
static void pgoutput_stream_start(struct LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn);
static void pgoutput_stream_stop(struct LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn);
static void pgoutput_stream_abort(struct LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn,
								  XLogRecPtr abort_lsn);
static void pgoutput_stream_commit(struct LogicalDecodingContext *ctx,
								   ReorderBufferTXN *txn,
								   XLogRecPtr commit_lsn);
static void pgoutput_stream_prepare_txn(LogicalDecodingContext *ctx,
										ReorderBufferTXN *txn, XLogRecPtr prepare_lsn);

static bool publications_valid;
static bool in_streaming;

/*
 * 出版物数据的私有内存上下文，pgoutput     
 * 启动时创建于 PGOutputData->context；其父上下文被         
 * 专用 MemoryContextCallback 重置时置为 NULL             
 */
static MemoryContext pubctx = NULL;

static List *LoadPublications(List *pubnames);
static void publication_invalidation_cb(Datum arg, int cacheid,
										uint32 hashvalue);
static void send_relation_and_attrs(Relation relation, TransactionId xid,
									LogicalDecodingContext *ctx,
									Bitmapset *columns);
static void send_repl_origin(LogicalDecodingContext *ctx,
							 RepOriginId origin_id, XLogRecPtr origin_lsn,
							 bool send_origin);

/*
 * 行过滤仅用 3 种出版物动作（"insert"、"update"、             
 * "delete"）见 RelationSyncEntry.exprstate[] 
 */
enum RowFilterPubAction
{
	PUBACTION_INSERT,
	PUBACTION_UPDATE,
	PUBACTION_DELETE
};

#define NUM_ROWFILTER_PUBACTIONS (PUBACTION_DELETE+1)

/*
 * 映射中的条目，记录已发送过哪些关系模式        
 *
 * schema_sent 标志判断该关系（若设置了 publish_as_relid   
 * 还包括其祖先）的当前模式记录是否已发送给订阅者 
 * （已发送则无需再次发送）                                  
 *
 * 下游只在提交时更新模式缓存，而流式事务的提交顺 
 * 序可能与事务的发送顺序不同。此外，（子）事务   
 * 可能中止，所以需为每个（子）事务都发送模式     
 * ，以免中止时丢失模式信息。为此，我们为已发送过     
 * 模式的                                                                
 * 各事务维护相应的 xid 列表（streamed_txns）                     
 * 。        
 *
 * 分区时，'pubactions'不只考虑表自身出版物，也   
 * 考虑其所有祖先的出版物。                 
 */
typedef struct RelationSyncEntry
{
	Oid			relid;			/* 关系 OID   */

	bool		replicate_valid;	/* 条目的整体有效性标志  */

	bool		schema_sent;
	List	   *streamed_txns;	/* streamed toplevel transactions with this
								 * schema */

	/* 是否发布此关系？    */
	PublicationActions pubactions;

	/*
	 * 行过滤的 ExprState 数组。不同出版物动作并非总能  
	 * 把多个表达式合并为一个，因为 update 或 delete 要  
	 * 求表达式中的列属于复制标识索引，而 insert 没有这  
	 * 种限制，因此每个出版物动作各有一个                    
	 * ExprState。                                  
	 */
	ExprState  *exprstate[NUM_ROWFILTER_PUBACTIONS];
	EState	   *estate;			/* 行过滤所用的执行器状态  */
	TupleTableSlot *new_slot;	/* 存放新元组的槽      */
	TupleTableSlot *old_slot;	/* 存放旧元组的槽      */

	/*
	 * 要以哪个关系发布变更的 OID。若该出版物设置了     
	 * publish_via_partition_root，便可将其设为它的某     
	 * 个祖先，复制变更时将使用该祖先的模式           
	 * 。         
	 */
	Oid			publish_as_relid;

	/*
	 * 使用祖先模式复制时，用于把元组从分区的类型转换 
	 * 为祖先的类型；若 publish_as_relid 与 'relid' 相同，     
	 * 或因分区与祖先的 TupleDesc 完全一致而无须转换，  
	 * 则为 NULL                
	 */
	AttrMap    *attrmap;

	/*
	 * 出版物包含的列，若隐式包含全部列则为 NULL    
	 * 注意此位图中的 attnum 并未按 FirstLowInvalidHeap        
	 * AttributeNumber 移位                        
	 */
	Bitmapset  *columns;

	/*
	 * 私有上下文，存放此条目的附加数据——行过滤表达  
	 * 达式的状态、列列表等           
	 */
	MemoryContext entry_cxt;
} RelationSyncEntry;

/*
 * 维护一个事务级变量，跟踪该事务是否已发送 BEGIN。     
 * BEGIN 只在处理事务中的第一个变更时才发送。这样       
 * 可以跳过为空事务发送一对 BEGIN/COMMIT 消息，从             
 * 而节省网络带宽。                                      
 *
 * 已准备事务不使用该优化：因为若 WALSender 在某事务  
 * prepare 之后、同一事务 commit prepared 之前重启，我            
 * 们将无法判断是否因事务为空而跳过了发送 BEGIN/      
 * PREPARE。这是因为重启前的内存 txndata 信息已经          
 * 丢失。这会导致向下游发送一个虚假的                  
 * COMMIT PREPARED，下游没有对应的已准备事务，处        
 * 理它时便会出错。                                           
 * 。                                                                 
 *
 * XXX 也可修改协议、发送额外信息，让下游能检测   
 * 到相应 prepare 尚未发送。但为下游中的每个事务都增加   
 * 这种检查代价可能很高，所以我们或许希望        
 * 把它做成可选的。                                               
 * 。        
 *
 * 流式事务也无这一优化，因为它们可能包含已准备    
 * 事务。                              
 */
typedef struct PGOutputTxnData
{
	bool		sent_begin_txn; /* 指示是否已发送 BEGIN 的标志       */
} PGOutputTxnData;

/* 记住已发送了哪些关系模式的映射。     */
static HTAB *RelationSyncCache = NULL;

static void init_rel_sync_cache(MemoryContext cachectx);
static void cleanup_rel_sync_cache(TransactionId xid, bool is_commit);
static RelationSyncEntry *get_rel_sync_entry(PGOutputData *data,
											 Relation relation);
static void rel_sync_cache_relation_cb(Datum arg, Oid relid);
static void rel_sync_cache_publication_cb(Datum arg, int cacheid,
										  uint32 hashvalue);
static void set_schema_sent_in_streamed_txn(RelationSyncEntry *entry,
											TransactionId xid);
static bool get_schema_sent_in_streamed_txn(RelationSyncEntry *entry,
											TransactionId xid);
static void init_tuple_slot(PGOutputData *data, Relation relation,
							RelationSyncEntry *entry);
static void pgoutput_memory_context_reset(void *arg);

/* 行过滤例程     */
static EState *create_estate_for_relation(Relation rel);
static void pgoutput_row_filter_init(PGOutputData *data,
									 List *publications,
									 RelationSyncEntry *entry);
static bool pgoutput_row_filter_exec_expr(ExprState *state,
										  ExprContext *econtext);
static bool pgoutput_row_filter(Relation relation, TupleTableSlot *old_slot,
								TupleTableSlot **new_slot_ptr,
								RelationSyncEntry *entry,
								ReorderBufferChangeType *action);

/* 列列表例程      */
static void pgoutput_column_list_init(PGOutputData *data,
									  List *publications,
									  RelationSyncEntry *entry);

/*
 * 指定输出插件回调       
 */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	cb->startup_cb = pgoutput_startup;
	cb->begin_cb = pgoutput_begin_txn;
	cb->change_cb = pgoutput_change;
	cb->truncate_cb = pgoutput_truncate;
	cb->message_cb = pgoutput_message;
	cb->commit_cb = pgoutput_commit_txn;

	cb->begin_prepare_cb = pgoutput_begin_prepare_txn;
	cb->prepare_cb = pgoutput_prepare_txn;
	cb->commit_prepared_cb = pgoutput_commit_prepared_txn;
	cb->rollback_prepared_cb = pgoutput_rollback_prepared_txn;
	cb->filter_by_origin_cb = pgoutput_origin_filter;
	cb->shutdown_cb = pgoutput_shutdown;

	/* 事务流式传输    */
	cb->stream_start_cb = pgoutput_stream_start;
	cb->stream_stop_cb = pgoutput_stream_stop;
	cb->stream_abort_cb = pgoutput_stream_abort;
	cb->stream_commit_cb = pgoutput_stream_commit;
	cb->stream_change_cb = pgoutput_change;
	cb->stream_message_cb = pgoutput_message;
	cb->stream_truncate_cb = pgoutput_truncate;
	/* 事务流式传输——两阶段提交  */
	cb->stream_prepare_cb = pgoutput_stream_prepare_txn;
}

static void
parse_output_parameters(List *options, PGOutputData *data)
{
	ListCell   *lc;
	bool		protocol_version_given = false;
	bool		publication_names_given = false;
	bool		binary_option_given = false;
	bool		messages_option_given = false;
	bool		streaming_given = false;
	bool		two_phase_option_given = false;
	bool		origin_option_given = false;

	data->binary = false;
	data->streaming = LOGICALREP_STREAM_OFF;
	data->messages = false;
	data->two_phase = false;

	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		Assert(defel->arg == NULL || IsA(defel->arg, String));

		/* 检查每个参数，无论我们是否认识它 */
		if (strcmp(defel->defname, "proto_version") == 0)
		{
			unsigned long parsed;
			char	   *endptr;

			if (protocol_version_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			protocol_version_given = true;

			errno = 0;
			parsed = strtoul(strVal(defel->arg), &endptr, 10);
			if (errno != 0 || *endptr != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid proto_version")));

			if (parsed > PG_UINT32_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("proto_version \"%s\" out of range",
								strVal(defel->arg))));

			data->protocol_version = (uint32) parsed;
		}
		else if (strcmp(defel->defname, "publication_names") == 0)
		{
			if (publication_names_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			publication_names_given = true;

			if (!SplitIdentifierString(strVal(defel->arg), ',',
									   &data->publication_names))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("invalid publication_names syntax")));
		}
		else if (strcmp(defel->defname, "binary") == 0)
		{
			if (binary_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			binary_option_given = true;

			data->binary = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "messages") == 0)
		{
			if (messages_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			messages_option_given = true;

			data->messages = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "streaming") == 0)
		{
			if (streaming_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			streaming_given = true;

			data->streaming = defGetStreamingMode(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			if (two_phase_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			two_phase_option_given = true;

			data->two_phase = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "origin") == 0)
		{
			if (origin_option_given)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("conflicting or redundant options"));
			origin_option_given = true;

			data->origin = defGetString(defel);

			if (pg_strcasecmp(data->origin, LOGICALREP_ORIGIN_NONE) != 0 &&
				pg_strcasecmp(data->origin, LOGICALREP_ORIGIN_ANY) != 0)
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("unrecognized origin value: \"%s\"", data->origin));
		}
		else
			elog(ERROR, "unrecognized pgoutput option: %s", defel->defname);
	}
}

/*
 * PGOutputData->context 的上下文重置回调         
 */
static void
pgoutput_memory_context_reset(void *arg)
{
	if (RelationSyncCache)
	{
		hash_destroy(RelationSyncCache);
		RelationSyncCache = NULL;
	}

	/* 稳妥为上           */
	pubctx = NULL;
}

/*
 * 初始化此插件    
 */
static void
pgoutput_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				 bool is_init)
{
	PGOutputData *data = palloc0(sizeof(PGOutputData));
	static bool publication_callback_registered = false;
	MemoryContextCallback *mcallback;

	/* 为私有分配创建自己的内存上下文。   */
	data->context = AllocSetContextCreate(ctx->context,
										  "logical replication output context",
										  ALLOCSET_DEFAULT_SIZES);

	data->cachectx = AllocSetContextCreate(ctx->context,
										   "logical replication cache context",
										   ALLOCSET_DEFAULT_SIZES);

	Assert(pubctx == NULL);
	pubctx = AllocSetContextCreate(ctx->context,
								   "logical replication publication list context",
								   ALLOCSET_SMALL_SIZES);

	/*
	 * 即使通过 SQL 接口调用的逻辑解码最终出错，也要确  
	 * 保清理 RelationSyncCache             
	 */
	mcallback = palloc0(sizeof(MemoryContextCallback));
	mcallback->func = pgoutput_memory_context_reset;
	MemoryContextRegisterResetCallback(ctx->context, mcallback);

	ctx->output_plugin_private = data;

	/* 本插件使用二进制协议。 */
	opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;

	/*
	 * 这次是复制启动，而不是槽初始化。      
	 *
	 * 解析并校验客户端传入的选项。      
	 */
	if (!is_init)
	{
		/* 解析这些参数，若发现不认识的参数则报 ERROR*/
		parse_output_parameters(ctx->output_plugin_options, data);

		/* 检查是否支持所请求的协议   */
		if (data->protocol_version > LOGICALREP_PROTO_MAX_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but server only supports protocol %d or lower",
							data->protocol_version, LOGICALREP_PROTO_MAX_VERSION_NUM)));

		if (data->protocol_version < LOGICALREP_PROTO_MIN_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but server only supports protocol %d or higher",
							data->protocol_version, LOGICALREP_PROTO_MIN_VERSION_NUM)));

		if (data->publication_names == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("publication_names parameter missing")));

		/*
		 * 决定是否启用流式传输。默认禁用，仅更新解     
		 * 码上下文中的标志。否则仅当协议版本足够，且  
		 * 输出插件支持时，我们才允许启用                     
		 * 流式传输。               
		 */
		if (data->streaming == LOGICALREP_STREAM_OFF)
			ctx->streaming = false;
		else if (data->streaming == LOGICALREP_STREAM_ON &&
				 data->protocol_version < LOGICALREP_PROTO_STREAM_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support streaming, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_STREAM_VERSION_NUM)));
		else if (data->streaming == LOGICALREP_STREAM_PARALLEL &&
				 data->protocol_version < LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support parallel streaming, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM)));
		else if (!ctx->streaming)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("streaming requested, but not supported by output plugin")));

		/* 另外记住，我们当前未在流式传输任何事务。 */
		in_streaming = false;

		/*
		 * 这里只检查是否传入 two-phase 选项，稍后再决  
		 * 定是否启用。若上次已启用，则保持启用。但仅  
		 * 在协议版本足够、输出插件支持时才允许传入     
		 * 该选项。                                                   
		 * 。                                              
		 */
		if (!data->two_phase)
			ctx->twophase_opt_given = false;
		else if (data->protocol_version < LOGICALREP_PROTO_TWOPHASE_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support two-phase commit, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_TWOPHASE_VERSION_NUM)));
		else if (!ctx->twophase)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("two-phase commit requested, but not supported by output plugin")));
		else
			ctx->twophase_opt_given = true;

		/* 初始化出版物状态*/
		data->publications = NIL;
		publications_valid = false;

		/*
		 * 若本进程之前某次调用尚未注册过回调，则为     
		 * pg_publication 注册回调               
		 */
		if (!publication_callback_registered)
		{
			CacheRegisterSyscacheCallback(PUBLICATIONOID,
										  publication_invalidation_cb,
										  (Datum) 0);
			publication_callback_registered = true;
		}

		/* 初始化关系模式缓存。    */
		init_rel_sync_cache(CacheMemoryContext);
	}
	else
	{
		/*
		 * 槽初始化模式下，禁用流式传输与                  
		 * 已准备事务。  
		 */
		ctx->streaming = false;
		ctx->twophase = false;
	}
}

/*
 * BEGIN 回调   
 *
 * 此处不发送 BEGIN 消息，而是推迟到首个变更时再发  
 * 在逻辑复制中，常见场景是只复制一组表（而非所有     
 * 表），变更在未发布表上的事务将产生空事务。      
 * 这些空事务会向订阅者发送 BEGIN 与 COMMIT 消息，把        
 * 带宽浪费在对逻辑复制几乎/毫无用处的东西            
 * 上。                                                                  
 */
static void
pgoutput_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	PGOutputTxnData *txndata = MemoryContextAllocZero(ctx->context,
													  sizeof(PGOutputTxnData));

	txn->output_plugin_private = txndata;
}

/*
 * 发 BEGIN  
 *
 * 在处理事务的第一个变更时调用本函数。              
 */
static void
pgoutput_send_begin(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;

	Assert(txndata);
	Assert(!txndata->sent_begin_txn);

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_begin(ctx->out, txn);
	txndata->sent_begin_txn = true;

	send_repl_origin(ctx, txn->origin_id, txn->origin_lsn,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);
}

/*
 * COMMIT 回调  
 */
static void
pgoutput_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					XLogRecPtr commit_lsn)
{
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	bool		sent_begin_txn;

	Assert(txndata);

	/*
	 * 除非该事务的相关变更已发送到下游，否则无须     
	 * 发送提交消息。                                 
	 */
	sent_begin_txn = txndata->sent_begin_txn;
	OutputPluginUpdateProgress(ctx, !sent_begin_txn);
	pfree(txndata);
	txn->output_plugin_private = NULL;

	if (!sent_begin_txn)
	{
		elog(DEBUG1, "skipped replication of an empty transaction with XID: %u", txn->xid);
		return;
	}

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_commit(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * BEGIN PREPARE 回调  
 */
static void
pgoutput_begin_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_begin_prepare(ctx->out, txn);

	send_repl_origin(ctx, txn->origin_id, txn->origin_lsn,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);
}

/*
 * PREPARE 回调  
 */
static void
pgoutput_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr prepare_lsn)
{
	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_prepare(ctx->out, txn, prepare_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * COMMIT PREPARED 回调  
 */
static void
pgoutput_commit_prepared_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
							 XLogRecPtr commit_lsn)
{
	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_commit_prepared(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * ROLLBACK PREPARED 回调  
 */
static void
pgoutput_rollback_prepared_txn(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn,
							   XLogRecPtr prepare_end_lsn,
							   TimestampTz prepare_time)
{
	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_rollback_prepared(ctx->out, txn, prepare_end_lsn,
									   prepare_time);
	OutputPluginWrite(ctx, true);
}

/*
 * 若尚未写过，则写出该关系及其祖先（若有）的当前    
 * 模式   
 */
static void
maybe_send_schema(LogicalDecodingContext *ctx,
				  ReorderBufferChange *change,
				  Relation relation, RelationSyncEntry *relentry)
{
	bool		schema_sent;
	TransactionId xid = InvalidTransactionId;
	TransactionId topxid = InvalidTransactionId;

	/*
	 * 记住该变更所属（子）事务的 XID。无须关心它是不  
	 * 是顶层事务（该 XID 已在当前                             
	 * 流式块的开头发送过）。     
	 *
	 * 若不在流式块中，则直接使用 InvalidTransactionId，     
	 * 写入方法不会包含它。        
	 */
	if (in_streaming)
		xid = change->txn->xid;

	if (rbtxn_is_subtxn(change->txn))
		topxid = rbtxn_get_toptxn(change->txn)->xid;
	else
		topxid = xid;

	/*
	 * 要发送模式吗？流式事务需单独跟踪，因为它们 
	 * 稍后才被应用（常规事务到那时才见效果）且   
	 * 应用顺序目前无从知晓。XXX 这里有优化空间。        
	 * 当前，流式事务总是 
	 *
	 * 在第一次就发送模式，但大概可以通过检查           
	 * 'relentry->schema_sent' 标志来避免。不过在此之前，     
	 * 需要先研究其对流式与非流式事务相混合场景        
	 * 的影响。                                                          
	 * 。                                         
	 */
	if (in_streaming)
		schema_sent = get_schema_sent_in_streamed_txn(relentry, topxid);
	else
		schema_sent = relentry->schema_sent;

	/* 若已发送过模式，则无事可做。   */
	if (schema_sent)
		return;

	/*
	 * 发送模式。若变更将以祖先的模式而非关系自身的    
	 * 模式发布，则先发祖先模式，再发关系自身模式   
	 * （XXX——也许只发前者就够了？）                          
	 */
	if (relentry->publish_as_relid != RelationGetRelid(relation))
	{
		Relation	ancestor = RelationIdGetRelation(relentry->publish_as_relid);

		send_relation_and_attrs(ancestor, xid, ctx, relentry->columns);
		RelationClose(ancestor);
	}

	send_relation_and_attrs(relation, xid, ctx, relentry->columns);

	if (in_streaming)
		set_schema_sent_in_streamed_txn(relentry, topxid);
	else
		relentry->schema_sent = true;
}

/*
 * 发送关系    
 */
static void
send_relation_and_attrs(Relation relation, TransactionId xid,
						LogicalDecodingContext *ctx,
						Bitmapset *columns)
{
	TupleDesc	desc = RelationGetDescr(relation);
	int			i;

	/*
	 * 如有需要则写出类型信息。只有用户创建的类型才这  
	 * 样做。以 FirstGenbkiObjectId 为分界点，从而只把      
	 * 手工分配 OID 的对象视为"内置"，而不包括例如         
	 * information_schema 中定义的函数或类型。这一点很重     
	 * 要，因为只有手工分配的 OID 才有望跨主版本保持      
	 * 稳定。      
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || att->attgenerated)
			continue;

		if (att->atttypid < FirstGenbkiObjectId)
			continue;

		/* 若该属性不在列列表中，则跳过它              */
		if (columns != NULL && !bms_is_member(att->attnum, columns))
			continue;

		OutputPluginPrepareWrite(ctx, false);
		logicalrep_write_typ(ctx->out, xid, att->atttypid);
		OutputPluginWrite(ctx, false);
	}

	OutputPluginPrepareWrite(ctx, false);
	logicalrep_write_rel(ctx->out, xid, relation, columns);
	OutputPluginWrite(ctx, false);
}

/*
 * 为指定关系准备行过滤表达式求值所需的执行器            
 * 状态。          
 */
static EState *
create_estate_for_relation(Relation rel)
{
	EState	   *estate;
	RangeTblEntry *rte;
	List	   *perminfos = NIL;

	estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;

	addRTEPermissionInfo(&perminfos, rte);

	ExecInitRangeTable(estate, list_make1(rte), perminfos);

	estate->es_output_cid = GetCurrentCommandId(false);

	return estate;
}

/*
 * 行过滤求值。   
 *
 * 若行过滤求值结果为 NULL，则视为假，即该变更不会     
 * 被复制。     
 */
static bool
pgoutput_row_filter_exec_expr(ExprState *state, ExprContext *econtext)
{
	Datum		ret;
	bool		isnull;

	Assert(state != NULL);

	ret = ExecEvalExprSwitchContext(state, econtext, &isnull);

	elog(DEBUG3, "row filter evaluates to %s (isnull: %s)",
		 isnull ? "false" : DatumGetBool(ret) ? "true" : "false",
		 isnull ? "true" : "false");

	if (isnull)
		return false;

	return DatumGetBool(ret);
}

/*
 * 确保每条目的内存上下文存在。    
 */
static void
pgoutput_ensure_entry_cxt(PGOutputData *data, RelationSyncEntry *entry)
{
	Relation	relation;

	/* 该上下文可能已存在，此时直接返回。    */
	if (entry->entry_cxt)
		return;

	relation = RelationIdGetRelation(entry->publish_as_relid);

	entry->entry_cxt = AllocSetContextCreate(data->cachectx,
											 "entry private context",
											 ALLOCSET_SMALL_SIZES);

	MemoryContextCopyAndSetIdentifier(entry->entry_cxt,
									  RelationGetRelationName(relation));
}

/*
 * 初始化行过滤。     
 */
static void
pgoutput_row_filter_init(PGOutputData *data, List *publications,
						 RelationSyncEntry *entry)
{
	ListCell   *lc;
	List	   *rfnodes[] = {NIL, NIL, NIL};	/* 每动作一个   */
	bool		no_filter[] = {false, false, false};	/* 每动作一个   */
	MemoryContext oldctx;
	int			idx;
	bool		has_filter = true;
	Oid			schemaid = get_rel_namespace(entry->publish_as_relid);

	/*
	 * 查找此关系是否存在行过滤。若有，则准备所需的     
	 * ExprState 并缓存到 entry->exprstate 中。要构建表达       
	 * 式状态，需要确保以下几点：                    
	 *
	 * 必须检查所有给定的出版物-表映射。        
	 *
	 * 多个出版物可能为此关系设多份行过滤。行过  
	 * 滤取决于 DML 操作，行过滤会追加到多个列表（每   
	 * 种操作各有一个列表，行过滤会被追加进               
	 * 去。      
	 *
	 * FOR ALL TABLES 与 FOR TABLES IN SCHEMA 意味着"不使             
	 * 用行过滤表达式"，优先。 
	 */
	foreach(lc, publications)
	{
		Publication *pub = lfirst(lc);
		HeapTuple	rftuple = NULL;
		Datum		rfdatum = 0;
		bool		pub_no_filter = true;

		/*
		 * 若出版物是 FOR ALL TABLES，或出版物包含的 FOR         
		 * TABLES IN SCHEMA 且表属于所引模式，则视同没有 
		 * 行过滤（即使其他出版物                                 
		 * 有行过滤）。                             
		 */
		if (!pub->alltables &&
			!SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
								   ObjectIdGetDatum(schemaid),
								   ObjectIdGetDatum(pub->oid)))
		{
			/*
			 * 检查此出版物中是否存在行过滤。              
			 */
			rftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(entry->publish_as_relid),
									  ObjectIdGetDatum(pub->oid));

			if (HeapTupleIsValid(rftuple))
			{
				/* NULL 表示无过滤。   */
				rfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
										  Anum_pg_publication_rel_prqual,
										  &pub_no_filter);
			}
		}

		if (pub_no_filter)
		{
			if (rftuple)
				ReleaseSysCache(rftuple);

			no_filter[PUBACTION_INSERT] |= pub->pubactions.pubinsert;
			no_filter[PUBACTION_UPDATE] |= pub->pubactions.pubupdate;
			no_filter[PUBACTION_DELETE] |= pub->pubactions.pubdelete;

			/*
			 * 若此出版物已发布全部 DML 动作，则快速    
			 * 退出。   
			 */
			if (no_filter[PUBACTION_INSERT] &&
				no_filter[PUBACTION_UPDATE] &&
				no_filter[PUBACTION_DELETE])
			{
				has_filter = false;
				break;
			}

			/* 此出版物无需额外工作。看下一个。   */
			continue;
		}

		/* 构建各动作的行过滤列表。     */
		if (pub->pubactions.pubinsert && !no_filter[PUBACTION_INSERT])
			rfnodes[PUBACTION_INSERT] = lappend(rfnodes[PUBACTION_INSERT],
												TextDatumGetCString(rfdatum));
		if (pub->pubactions.pubupdate && !no_filter[PUBACTION_UPDATE])
			rfnodes[PUBACTION_UPDATE] = lappend(rfnodes[PUBACTION_UPDATE],
												TextDatumGetCString(rfdatum));
		if (pub->pubactions.pubdelete && !no_filter[PUBACTION_DELETE])
			rfnodes[PUBACTION_DELETE] = lappend(rfnodes[PUBACTION_DELETE],
												TextDatumGetCString(rfdatum));

		ReleaseSysCache(rftuple);
	}							/* 遍历所有订阅的出版物   */

	/* 清理行过滤      */
	for (idx = 0; idx < NUM_ROWFILTER_PUBACTIONS; idx++)
	{
		if (no_filter[idx])
		{
			list_free_deep(rfnodes[idx]);
			rfnodes[idx] = NIL;
		}
	}

	if (has_filter)
	{
		Relation	relation = RelationIdGetRelation(entry->publish_as_relid);

		pgoutput_ensure_entry_cxt(data, entry);

		/*
		 * 现在所有出版物动作的过滤条件都已明确。当出版 
		 * 物动作相同即合并。   
		 */
		oldctx = MemoryContextSwitchTo(entry->entry_cxt);
		entry->estate = create_estate_for_relation(relation);
		for (idx = 0; idx < NUM_ROWFILTER_PUBACTIONS; idx++)
		{
			List	   *filters = NIL;
			Expr	   *rfnode;

			if (rfnodes[idx] == NIL)
				continue;

			foreach(lc, rfnodes[idx])
				filters = lappend(filters, stringToNode((char *) lfirst(lc)));

			/* 合并行过滤并缓存 ExprState             */
			rfnode = make_orclause(filters);
			entry->exprstate[idx] = ExecPrepareExpr(rfnode, entry->estate);
		}						/* 对每个动作    */
		MemoryContextSwitchTo(oldctx);

		RelationClose(relation);
	}
}

/*
 * 初始化列列表。      
 */
static void
pgoutput_column_list_init(PGOutputData *data, List *publications,
						  RelationSyncEntry *entry)
{
	ListCell   *lc;
	bool		first = true;
	Relation	relation = RelationIdGetRelation(entry->publish_as_relid);

	/*
	 * 查找此关系是否存在列列表。若有，则                
	 * 用列列表构建位图。           
	 *
	 * 多个出版物可能为此关系提供多份列列表         
	 * 。      
	 *
	 * 注意不支持这种情况：合并出版物时同一表的列列    
	 * 表互不相同。参见 fetch_table_list 顶部注释。但人  
	 * 们之后可以修改出版物，所以仍需检查所有给定的出 
	 * 版物-表映射；只要某出版物的列列表不同，          
	 * 就报错。                                           
	 *
	 * FOR ALL TABLES 与 FOR TABLES IN SCHEMA 意为"不使用列列表"    
	 */
	foreach(lc, publications)
	{
		Publication *pub = lfirst(lc);
		HeapTuple	cftuple = NULL;
		Datum		cfdatum = 0;
		Bitmapset  *cols = NULL;

		/*
		 * 若出版物是 FOR ALL TABLES，则视同没有列列表（即    
		 * 使其他出版物有列列表）。                           
		 * 。   
		 */
		if (!pub->alltables)
		{
			bool		pub_no_list = true;

			/*
			 * 检查此出版物中是否存在列列表。               
			 *
			 * 注：无 pg_publication_rel 行，即为整个模式定义   
			 * 的出版物，因此不能有列列表，正如 FOR ALL   
			 * TABLES 出版物那样。              
			 */
			cftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(entry->publish_as_relid),
									  ObjectIdGetDatum(pub->oid));

			if (HeapTupleIsValid(cftuple))
			{
				/* 查找列列表属性。          */
				cfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, cftuple,
										  Anum_pg_publication_rel_prattrs,
										  &pub_no_list);

				/* 在每条目上下文中构建列列表位图。       */
				if (!pub_no_list)	/* 非空时     */
				{
					int			i;
					int			nliveatts = 0;
					TupleDesc	desc = RelationGetDescr(relation);

					pgoutput_ensure_entry_cxt(data, entry);

					cols = pub_collist_to_bitmapset(cols, cfdatum,
													entry->entry_cxt);

					/* 获取存活属性的数量。     */
					for (i = 0; i < desc->natts; i++)
					{
						Form_pg_attribute att = TupleDescAttr(desc, i);

						if (att->attisdropped || att->attgenerated)
							continue;

						nliveatts++;
					}

					/*
					 * 若列列表含表的全部列，则                 
					 * 设为 NULL    
					 */
					if (bms_num_members(cols) == nliveatts)
					{
						bms_free(cols);
						cols = NULL;
					}
				}

				ReleaseSysCache(cftuple);
			}
		}

		if (first)
		{
			entry->columns = cols;
			first = false;
		}
		else if (!bms_equal(entry->columns, cols))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   get_namespace_name(RelationGetNamespace(relation)),
						   RelationGetRelationName(relation)));
	}							/* 遍历所有订阅的出版物   */

	RelationClose(relation);
}

/*
 * 初始化用于存放新旧元组的槽，并构建将用于把该关     
 * 系的元组转换为祖先格式的映射。                            
 */
static void
init_tuple_slot(PGOutputData *data, Relation relation,
				RelationSyncEntry *entry)
{
	MemoryContext oldctx;
	TupleDesc	oldtupdesc;
	TupleDesc	newtupdesc;

	oldctx = MemoryContextSwitchTo(data->cachectx);

	/*
	 * 创建元组表槽。请创建 TupleDesc 的一份副本，因为它   
	 * 需与缓存存活一样久。    
	 */
	oldtupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));
	newtupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));

	entry->old_slot = MakeSingleTupleTableSlot(oldtupdesc, &TTSOpsHeapTuple);
	entry->new_slot = MakeSingleTupleTableSlot(newtupdesc, &TTSOpsHeapTuple);

	MemoryContextSwitchTo(oldctx);

	/*
	 * 缓存将用于把该关系元组转换为祖先格式的映射，   
	 * 如有需要。                  
	 */
	if (entry->publish_as_relid != RelationGetRelid(relation))
	{
		Relation	ancestor = RelationIdGetRelation(entry->publish_as_relid);
		TupleDesc	indesc = RelationGetDescr(relation);
		TupleDesc	outdesc = RelationGetDescr(ancestor);

		/* 映射必须活得与逻辑解码上下文一样久。 */
		oldctx = MemoryContextSwitchTo(data->cachectx);

		entry->attrmap = build_attrmap_by_name_if_req(indesc, outdesc, false);

		MemoryContextSwitchTo(oldctx);
		RelationClose(ancestor);
	}
}

/*
 * 若存在行过滤，则检查变更。         
 *
 * 若该变更将被复制则返回 true，否则 false。   
 *
 * 对 insert，评估新元组的行过滤。        
 * 对 delete，评估旧元组的行过滤。        
 * 对 update，评估旧、新元组的行过滤。          
 *
 * 对 update，若两次评估都为真，则允许发送                   
 * UPDATE；若两次评估都为假，则不复制该 UPDATE。若           
 * 只有一个元组匹配行过滤表达式，为避免任何数据不 
 * 一致，将按以下规则把 UPDATE 转换为 DELETE 或             
 * INSERT：       
 *
 * Case 1: old-row (no match)    new-row (no match)  -> (drop change)
 * Case 2: old-row (no match)    new row (match)     -> INSERT
 * Case 3: old-row (match)       new-row (no match)  -> DELETE
 * Case 4: old-row (match)       new row (match)     -> UPDATE
 *
 * 新的动作在 action 参数中更新。         
 *
 * 把 UPDATE 转换为 INSERT 时可能更新 new slot，因为原       
 * 始的新元组可能没有来自复制标识的列值                      
 * 。      
 *
 * 示例   
 * 假设旧元组满足行过滤而新元组不满足。由于旧元组      
 * 满足，初始表同步已复制此行（或用其他方法保证          
 * 数据一致性）。但 UPDATE 之后新元组不满足行过 
 * 滤，从数据一致性角度看，订阅者上应删除该行。应     
 * 将 UPDATE 转换为 DELETE 语句并发送给订阅者。把这      
 * 行留在订阅者上并不可取，因为它不反映发布者上行    
 * 过滤表达式所定义的内容。订阅者上的这一行很可能      
 * 不会再被复制修改。如果有人                                     
 * 插入一个使用相同旧标识符的新行，复制可能               
 * 因违反约束而停止                                                  
 * 。                                                                  
 *
 * 假设旧元组不匹配行过滤而新元组匹配。由于旧元组       
 * 不满足，初始表同步很可能没有复制此行。然而       
 * UPDATE 之后新元组满足行过滤，从数据一致性角度           
 * 看，该行应插入到订阅者上。否则，后续的 UPDATE        
 * 或 DELETE 语句将无效（匹配不到任何行——参见              
 * apply_handle_update_internal()）。故应把      
 * UPDATE 转换为 INSERT 语句并发送给订阅者。但这可             
 * 能让那些期望数据集满足提供方上行过滤表达式             
 * 的人感到意外。                                                       
 * 。      
 */
static bool
pgoutput_row_filter(Relation relation, TupleTableSlot *old_slot,
					TupleTableSlot **new_slot_ptr, RelationSyncEntry *entry,
					ReorderBufferChangeType *action)
{
	TupleDesc	desc;
	int			i;
	bool		old_matched,
				new_matched,
				result;
	TupleTableSlot *tmp_new_slot;
	TupleTableSlot *new_slot = *new_slot_ptr;
	ExprContext *ecxt;
	ExprState  *filter_exprstate;

	/*
	 * 需要此映射，以免依赖 ReorderBufferChangeType 枚举     
	 * 具有特定值。     
	 */
	static const int map_changetype_pubaction[] = {
		[REORDER_BUFFER_CHANGE_INSERT] = PUBACTION_INSERT,
		[REORDER_BUFFER_CHANGE_UPDATE] = PUBACTION_UPDATE,
		[REORDER_BUFFER_CHANGE_DELETE] = PUBACTION_DELETE
	};

	Assert(*action == REORDER_BUFFER_CHANGE_INSERT ||
		   *action == REORDER_BUFFER_CHANGE_UPDATE ||
		   *action == REORDER_BUFFER_CHANGE_DELETE);

	Assert(new_slot || old_slot);

	/* 获取对应的行过滤         */
	filter_exprstate = entry->exprstate[map_changetype_pubaction[*action]];

	/* 若无行过滤则直接返回     */
	if (!filter_exprstate)
		return true;

	elog(DEBUG3, "table \"%s.%s\" has row filter",
		 get_namespace_name(RelationGetNamespace(relation)),
		 RelationGetRelationName(relation));

	ResetPerTupleExprContext(entry->estate);

	ecxt = GetPerTupleExprContext(entry->estate);

	/*
	 * 在下列只有一个元组的场合，我们可以对该元组  
	 * 求值行过滤并返回。                       
	 *
	 * 对于插入，我们只有新元组。 
	 *
	 * 对于更新，若无任何复制标识列被修改且这些列   
	 * 都不含外部数据时，可能只有新元组；但我们仍      
	 * 需对新元组求值行过滤，因为这些列的现有值         
	 * 可能与过滤器不符。另外，用户可在行过滤中      
	 * 使用常量表达式，所以我们无论如何都要对新元组     
	 * 进行求值。                  
	 *
	 * 对于删除，我们只有旧元组。 
	 */
	if (!new_slot || !old_slot)
	{
		ecxt->ecxt_scantuple = new_slot ? new_slot : old_slot;
		result = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

		return result;
	}

	/*
	 * 仅对更新而言，新旧两个元组才都必须有效，且需    
	 * 要对照行过滤检查。       
	 */
	Assert(map_changetype_pubaction[*action] == PUBACTION_UPDATE);

	slot_getallattrs(new_slot);
	slot_getallattrs(old_slot);

	tmp_new_slot = NULL;
	desc = RelationGetDescr(relation);

	/*
	 * 新元组可能并不包含全部复制标识列，这种情况下     
	 * 需要从旧元组复制过来。                  
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		/*
		 * 若新元组或旧元组中的列为 null，则无需做任何事 
		 */
		if (new_slot->tts_isnull[i] || old_slot->tts_isnull[i])
			continue;

		/*
		 * 未修改的 toasted 复制标识列只记录于旧元组中。  
		 * 将其复制到新元组。已更改（或记入 WAL）的     
		 * toast 值总是在内存中组装并设为 VARTAG_INDIRECT。 
		 * 参见 ReorderBufferToastReplace。            
		 */
		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_ONDISK(new_slot->tts_values[i]) &&
			!VARATT_IS_EXTERNAL_ONDISK(old_slot->tts_values[i]))
		{
			if (!tmp_new_slot)
			{
				tmp_new_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
				ExecClearTuple(tmp_new_slot);

				memcpy(tmp_new_slot->tts_values, new_slot->tts_values,
					   desc->natts * sizeof(Datum));
				memcpy(tmp_new_slot->tts_isnull, new_slot->tts_isnull,
					   desc->natts * sizeof(bool));
			}

			tmp_new_slot->tts_values[i] = old_slot->tts_values[i];
			tmp_new_slot->tts_isnull[i] = old_slot->tts_isnull[i];
		}
	}

	ecxt->ecxt_scantuple = old_slot;
	old_matched = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

	if (tmp_new_slot)
	{
		ExecStoreVirtualTuple(tmp_new_slot);
		ecxt->ecxt_scantuple = tmp_new_slot;
	}
	else
		ecxt->ecxt_scantuple = new_slot;

	new_matched = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

	/*
	 * 情况 1：两元组均不匹配行过滤，则退出且不     
	 * 发送  
	 */
	if (!old_matched && !new_matched)
		return false;

	/*
	 * 情况 2：若旧元组不满足行过滤而新元组满足，     
	 * 则将 UPDATE 转换为 INSERT。            
	 *
	 * 此时应使用新转换出的元组，其中必须包含全部        
	 * 复制标识列的列值。这一点是必需的，以保证在      
	 * 向下游节点插入该元组时，我们拥有全部所需的  
	 * 列值。              
	 */
	if (!old_matched && new_matched)
	{
		*action = REORDER_BUFFER_CHANGE_INSERT;

		if (tmp_new_slot)
			*new_slot_ptr = tmp_new_slot;
	}

	/*
	 * 情况 3：若旧元组满足行过滤而新元组不满足，     
	 * 则将 UPDATE 转换为 DELETE。         
	 *
	 * 这种转换不需要另一个元组。将使用旧元组来执行    
	 * DELETE。          
	 */
	else if (old_matched && !new_matched)
		*action = REORDER_BUFFER_CHANGE_DELETE;

	/*
	 * 情况 4：两元组均匹配行过滤时无需做转换。      
	 * （*action 默认为 UPDATE）。     
	 */

	return true;
}

/*
 * 经网络发送解码 DML。    
 *
 * 流式与非流式模式下都会调用此函数。      
 */
static void
pgoutput_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				Relation relation, ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;
	TransactionId xid = InvalidTransactionId;
	Relation	ancestor = NULL;
	Relation	targetrel = relation;
	ReorderBufferChangeType action = change->action;
	TupleTableSlot *old_slot = NULL;
	TupleTableSlot *new_slot = NULL;

	if (!is_publishable_relation(relation))
		return;

	/*
	 * 在流式模式下记住该变更的 xid。流式模式下需要      
	 * 随每个变更发送 xid，让订阅者能建立关联，以便  
	 * 在中止时丢弃相应的变更                                
	 * 。     
	 */
	if (in_streaming)
		xid = change->txn->xid;

	relentry = get_rel_sync_entry(data, relation);

	/* 首先检查表过滤器     */
	switch (action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			if (!relentry->pubactions.pubinsert)
				return;
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			if (!relentry->pubactions.pubupdate)
				return;
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			if (!relentry->pubactions.pubdelete)
				return;

			/*
			 * 只有表未定义复制标识也允许删除时才有可能  
			 * 出现这种情况。DELETE 动作无法被发布，因此  
			 * 我们直接返回。                
			 */
			if (!change->data.tp.oldtuple)
			{
				elog(DEBUG1, "didn't send DELETE change because of missing oldtuple");
				return;
			}
			break;
		default:
			Assert(false);
	}

	/* 使用并重置自己的上下文，以免内存泄漏      */
	old = MemoryContextSwitchTo(data->context);

	/* 若经根发布，则切换关系。    */
	if (relentry->publish_as_relid != RelationGetRelid(relation))
	{
		Assert(relation->rd_rel->relispartition);
		ancestor = RelationIdGetRelation(relentry->publish_as_relid);
		targetrel = ancestor;
	}

	if (change->data.tp.oldtuple)
	{
		old_slot = relentry->old_slot;
		ExecStoreHeapTuple(&change->data.tp.oldtuple->tuple, old_slot, false);

		/* 按需转换元组。    */
		if (relentry->attrmap)
		{
			TupleTableSlot *slot = MakeTupleTableSlot(RelationGetDescr(targetrel),
													  &TTSOpsVirtual);

			old_slot = execute_attr_map_slot(relentry->attrmap, old_slot, slot);
		}
	}

	if (change->data.tp.newtuple)
	{
		new_slot = relentry->new_slot;
		ExecStoreHeapTuple(&change->data.tp.newtuple->tuple, new_slot, false);

		/* 按需转换元组。    */
		if (relentry->attrmap)
		{
			TupleTableSlot *slot = MakeTupleTableSlot(RelationGetDescr(targetrel),
													  &TTSOpsVirtual);

			new_slot = execute_attr_map_slot(relentry->attrmap, new_slot, slot);
		}
	}

	/*
	 * 行过滤检查  
	 *
	 * 根据对旧元组与新元组的行过滤结果，更新可能被     
	 * 转换为插入或删除。             
	 */
	if (!pgoutput_row_filter(targetrel, old_slot, &new_slot, relentry, &action))
		goto cleanup;

	/*
	 * 先发 BEGIN（若未发）  
	 *
	 * 我们在确保确实会发送该变更之后，才发送 BEGIN        
	 * 消息。这可避免为空事务发送一对 BEGIN/COMMIT           
	 * 消息。    
	 */
	if (txndata && !txndata->sent_begin_txn)
		pgoutput_send_begin(ctx, txn);

	/*
	 * 模式应当用原始关系来发送，因为它同时也会发送     
	 * 祖先的关系。      
	 */
	maybe_send_schema(ctx, change, relation, relentry);

	OutputPluginPrepareWrite(ctx, true);

	/* 发送数据  */
	switch (action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			logicalrep_write_insert(ctx->out, xid, targetrel, new_slot,
									data->binary, relentry->columns);
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			logicalrep_write_update(ctx->out, xid, targetrel, old_slot,
									new_slot, data->binary, relentry->columns);
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			logicalrep_write_delete(ctx->out, xid, targetrel, old_slot,
									data->binary, relentry->columns);
			break;
		default:
			Assert(false);
	}

	OutputPluginWrite(ctx, true);

cleanup:
	if (RelationIsValid(ancestor))
	{
		RelationClose(ancestor);
		ancestor = NULL;
	}

	/* 丢弃用于存放已转换元组的新槽位。                 */
	if (relentry->attrmap)
	{
		if (old_slot)
			ExecDropSingleTupleTableSlot(old_slot);

		if (new_slot)
			ExecDropSingleTupleTableSlot(new_slot);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

static void
pgoutput_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				  int nrelations, Relation relations[], ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;
	int			i;
	int			nrelids;
	Oid		   *relids;
	TransactionId xid = InvalidTransactionId;

	/* 在流式模式下记住该变更的 xid。参见 pgoutput_change。    */
	if (in_streaming)
		xid = change->txn->xid;

	old = MemoryContextSwitchTo(data->context);

	relids = palloc0(nrelations * sizeof(Oid));
	nrelids = 0;

	for (i = 0; i < nrelations; i++)
	{
		Relation	relation = relations[i];
		Oid			relid = RelationGetRelid(relation);

		if (!is_publishable_relation(relation))
			continue;

		relentry = get_rel_sync_entry(data, relation);

		if (!relentry->pubactions.pubtruncate)
			continue;

		/*
		 * 若出版物只想经由它来发送根表，则不发送      
		 * 分区。              
		 */
		if (relation->rd_rel->relispartition &&
			relentry->publish_as_relid != relid)
			continue;

		relids[nrelids++] = relid;

		/* 若尚未发送则发 BEGIN  */
		if (txndata && !txndata->sent_begin_txn)
			pgoutput_send_begin(ctx, txn);

		maybe_send_schema(ctx, change, relation, relentry);
	}

	if (nrelids > 0)
	{
		OutputPluginPrepareWrite(ctx, true);
		logicalrep_write_truncate(ctx->out,
								  xid,
								  nrelids,
								  relids,
								  change->data.truncate.cascade,
								  change->data.truncate.restart_seqs);
		OutputPluginWrite(ctx, true);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

static void
pgoutput_message(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 XLogRecPtr message_lsn, bool transactional, const char *prefix, Size sz,
				 const char *message)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	TransactionId xid = InvalidTransactionId;

	if (!data->messages)
		return;

	/*
	 * 在流式模式下记住该消息的 xid。参见      
	 * pgoutput_change 
	 */
	if (in_streaming)
		xid = txn->xid;

	/*
	 * 若尚未输出则输出 BEGIN。非事务性消息则跳过。      
	 */
	if (transactional)
	{
		PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;

		/* 若尚未发送则发 BEGIN  */
		if (txndata && !txndata->sent_begin_txn)
			pgoutput_send_begin(ctx, txn);
	}

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_message(ctx->out,
							 xid,
							 message_lsn,
							 transactional,
							 prefix,
							 sz,
							 message);
	OutputPluginWrite(ctx, true);
}

/*
 * 若数据与某个复制源相关联，而用户请求的是不带   
 * 复制源的变更，则返回 true，否则返回 false。      
 */
static bool
pgoutput_origin_filter(LogicalDecodingContext *ctx,
					   RepOriginId origin_id)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;

	if (data->origin && (pg_strcasecmp(data->origin, LOGICALREP_ORIGIN_NONE) == 0) &&
		origin_id != InvalidRepOriginId)
		return true;

	return false;
}

/*
 * 关闭输出插件。      
 *
 * 注意，无需清理 data->context、data->cachectx 与 pubctx，       
 * 因为它们都是 ctx->context 的子上下文，逻辑解码机制     
 * 会负责清理它们。      
 */
static void
pgoutput_shutdown(LogicalDecodingContext *ctx)
{
	pgoutput_memory_context_reset(NULL);
}

/*
 * 从出版物名称列表加载各出版物。        
 */
static List *
LoadPublications(List *pubnames)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, pubnames)
	{
		char	   *pubname = (char *) lfirst(lc);
		Publication *pub = GetPublicationByName(pubname, false);

		result = lappend(result, pub);
	}

	return result;
}

/*
 * 出版物 syscache 失效回调函数。   
 *
 * 在 pg_publication 失效时被调用。   
 */
static void
publication_invalidation_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	publications_valid = false;

	/*
	 * 同时使按关系缓存失效，以便下次检查过滤信息时，  
	 * 它会依据新的出版物设置完成更新。                
	 */
	rel_sync_cache_publication_cb(arg, cacheid, hashvalue);
}

/*
 * START STREAM 回调  
 */
static void
pgoutput_stream_start(struct LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;

	/* 无法嵌套事务的流式处理       */
	Assert(!in_streaming);

	/*
	 * 若我们已为本事务发送过第一个流，那么后续的    
	 * 流中就不再发送复制源 id。         
	 */
	if (rbtxn_is_streamed(txn))
		send_replication_origin = false;

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_stream_start(ctx->out, txn->xid, !rbtxn_is_streamed(txn));

	send_repl_origin(ctx, txn->origin_id, InvalidXLogRecPtr,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);

	/* 现在正流式传输事务的一个分块 */
	in_streaming = true;
}

/*
 * STOP STREAM 回调  
 */
static void
pgoutput_stream_stop(struct LogicalDecodingContext *ctx,
					 ReorderBufferTXN *txn)
{
	/* 此时应正在流式传输事务    */
	Assert(in_streaming);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_stop(ctx->out);
	OutputPluginWrite(ctx, true);

	/* 此时已停止流式传输事务     */
	in_streaming = false;
}

/*
 * 通知下游丢弃该流式事务（若其为顶层事务，则连   
 * 同其全部子事务一并丢弃）。               
 */
static void
pgoutput_stream_abort(struct LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn,
					  XLogRecPtr abort_lsn)
{
	ReorderBufferTXN *toptxn;
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	bool		write_abort_info = (data->streaming == LOGICALREP_STREAM_PARALLEL);

	/*
	 * 中止应发生在流式代码块之外，即使是流式事务   
	 * 也是如此。不过该事务仍必须被标记为流式的。    
	 */
	Assert(!in_streaming);

	/* 确定顶层事务                 */
	toptxn = rbtxn_get_toptxn(txn);

	Assert(rbtxn_is_streamed(toptxn));

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_abort(ctx->out, toptxn->xid, txn->xid, abort_lsn,
								  txn->xact_time.abort_time, write_abort_info);

	OutputPluginWrite(ctx, true);

	cleanup_rel_sync_cache(toptxn->xid, false);
}

/*
 * 通知下游应用该流式事务（连同其所有子事务       
 * 一并应用）。    
 */
static void
pgoutput_stream_commit(struct LogicalDecodingContext *ctx,
					   ReorderBufferTXN *txn,
					   XLogRecPtr commit_lsn)
{
	/*
	 * 提交应发生在流式代码块之外，即使是流式事务    
	 * 也是如此。不过该事务仍必须被标记为流式的。    
	 */
	Assert(!in_streaming);
	Assert(rbtxn_is_streamed(txn));

	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_commit(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);

	cleanup_rel_sync_cache(txn->xid, true);
}

/*
 * PREPARE 回调（流式两阶段提交用）。   
 *
 * 通知下游准备该事务。                   
 */
static void
pgoutput_stream_prepare_txn(LogicalDecodingContext *ctx,
							ReorderBufferTXN *txn,
							XLogRecPtr prepare_lsn)
{
	Assert(rbtxn_is_streamed(txn));

	OutputPluginUpdateProgress(ctx, false);
	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_prepare(ctx->out, txn, prepare_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * 为解码会话初始化关系模式同步缓存。              
 *
 * 哈希表在解码会话结束时销毁。虽然 relcache 失效   
 * 仍会存在且仍被调用，但它们只会看到该全局哈   
 * 希表变量为空，因而不采取任何行动。         
 */
static void
init_rel_sync_cache(MemoryContext cachectx)
{
	HASHCTL		ctl;
	static bool relation_callbacks_registered = false;

	/* 若哈希表已存在则无需做任何事 */
	if (RelationSyncCache != NULL)
		return;

	/* 为缓存新建一个哈希表      */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelationSyncEntry);
	ctl.hcxt = cachectx;

	RelationSyncCache = hash_create("logical replication output relation cache",
									128, &ctl,
									HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);

	Assert(RelationSyncCache != NULL);

	/* 若已注册回调，则无需再做任何事    */
	if (relation_callbacks_registered)
		return;

	/* relcache 刷写后，我们必须更新该关系的缓存条目。   */
	CacheRegisterRelcacheCallback(rel_sync_cache_relation_cb, (Datum) 0);

	/*
	 * pg_namespace 变更后刷写全部缓存条目，以防它是        
	 * 影响正被复制关系的模式重命名。       
	 */
	CacheRegisterSyscacheCallback(NAMESPACEOID,
								  rel_sync_cache_publication_cb,
								  (Datum) 0);

	/*
	 * 出版物变更后一律刷写全部缓存条目。（无需为    
	 * pg_publication 注册回调条目——publication_invalidation_cb    
	 * 会处理它。）    
	 */
	CacheRegisterSyscacheCallback(PUBLICATIONRELMAP,
								  rel_sync_cache_publication_cb,
								  (Datum) 0);
	CacheRegisterSyscacheCallback(PUBLICATIONNAMESPACEMAP,
								  rel_sync_cache_publication_cb,
								  (Datum) 0);

	relation_callbacks_registered = true;
}

/*
 * 我们预计流式事务的数量相对较少。           
 */
static bool
get_schema_sent_in_streamed_txn(RelationSyncEntry *entry, TransactionId xid)
{
	return list_member_xid(entry->streamed_txns, xid);
}

/*
 * 将已为其发送过该关系模式的 xid 记入 rel sync                
 * 条目。       
 */
static void
set_schema_sent_in_streamed_txn(RelationSyncEntry *entry, TransactionId xid)
{
	MemoryContext oldctx;

	oldctx = MemoryContextSwitchTo(CacheMemoryContext);

	entry->streamed_txns = lappend_xid(entry->streamed_txns, xid);

	MemoryContextSwitchTo(oldctx);
}

/*
 * 在关系模式缓存中查找或创建条目。  
 *
 * 此函数会查找给定关系直接或间接所属的出版物  
 * （间接所属指真正在出版物中的是该关系的祖先），     
 * 并利用下列信息填充找到的条目：要发布哪些操作、      
 * 以及发布时是否要使用祖先的模式                            
 * 来发布。    
 */
static RelationSyncEntry *
get_rel_sync_entry(PGOutputData *data, Relation relation)
{
	RelationSyncEntry *entry;
	bool		found;
	MemoryContext oldctx;
	Oid			relid = RelationGetRelid(relation);

	Assert(RelationSyncCache != NULL);

	/* 查找已缓存的关系信息，若无则创建 */
	entry = (RelationSyncEntry *) hash_search(RelationSyncCache,
											  &relid,
											  HASH_ENTER, &found);
	Assert(entry != NULL);

	/* 若是新条目则初始化   */
	if (!found)
	{
		entry->replicate_valid = false;
		entry->schema_sent = false;
		entry->streamed_txns = NIL;
		entry->pubactions.pubinsert = entry->pubactions.pubupdate =
			entry->pubactions.pubdelete = entry->pubactions.pubtruncate = false;
		entry->new_slot = NULL;
		entry->old_slot = NULL;
		memset(entry->exprstate, 0, sizeof(entry->exprstate));
		entry->entry_cxt = NULL;
		entry->publish_as_relid = InvalidOid;
		entry->columns = NULL;
		entry->attrmap = NULL;
	}

	/* 校验该条目    */
	if (!entry->replicate_valid)
	{
		Oid			schemaId = get_rel_namespace(relid);
		List	   *pubids = GetRelationPublications(relid);

		/*
		 * 构建缓存条目时不对命名空间系统表加锁，因为  
		 * 我们用历史快照构建，而此后的所有变更都会在    
		 * 解码 WAL 时被吸收。      
		 */
		List	   *schemaPubids = GetSchemaPublications(schemaId);
		ListCell   *lc;
		Oid			publish_as_relid = relid;
		int			publish_ancestor_level = 0;
		bool		am_partition = get_rel_relispartition(relid);
		char		relkind = get_rel_relkind(relid);
		List	   *rel_publications = NIL;

		/* 使用前按需重新加载出版物。   */
		if (!publications_valid)
		{
			Assert(pubctx);

			MemoryContextReset(pubctx);
			oldctx = MemoryContextSwitchTo(pubctx);

			data->publications = LoadPublications(data->publication_names);
			MemoryContextSwitchTo(oldctx);
			publications_valid = true;
		}

		/*
		 * 重置 schema_sent 状态，因为关系定义可能已     
		 * 改变。另外，若关系已从出版物中移除，则把    
		 * pubactions 重置为空。同时释放全部依赖先前定    
		 * 义的对象。    
		 */
		entry->schema_sent = false;
		list_free(entry->streamed_txns);
		entry->streamed_txns = NIL;
		bms_free(entry->columns);
		entry->columns = NULL;
		entry->pubactions.pubinsert = false;
		entry->pubactions.pubupdate = false;
		entry->pubactions.pubdelete = false;
		entry->pubactions.pubtruncate = false;

		/*
		 * 清理元组槽。（必要时会重新构建。）     
		 */
		if (entry->old_slot)
		{
			TupleDesc	desc = entry->old_slot->tts_tupleDescriptor;

			Assert(desc->tdrefcount == -1);

			ExecDropSingleTupleTableSlot(entry->old_slot);

			/*
			 * ExecDropSingleTupleTableSlot() 不会释放 TupleDesc，       
			 * 现在释放以免泄漏。  
			 */
			FreeTupleDesc(desc);
		}
		if (entry->new_slot)
		{
			TupleDesc	desc = entry->new_slot->tts_tupleDescriptor;

			Assert(desc->tdrefcount == -1);

			ExecDropSingleTupleTableSlot(entry->new_slot);

			/*
			 * ExecDropSingleTupleTableSlot() 不会释放 TupleDesc，       
			 * 现在释放以免泄漏。  
			 */
			FreeTupleDesc(desc);
		}

		entry->old_slot = NULL;
		entry->new_slot = NULL;

		if (entry->attrmap)
			free_attrmap(entry->attrmap);
		entry->attrmap = NULL;

		/*
		 * 行过滤缓存清理     
		 */
		if (entry->entry_cxt)
			MemoryContextDelete(entry->entry_cxt);

		entry->entry_cxt = NULL;
		entry->estate = NULL;
		memset(entry->exprstate, 0, sizeof(entry->exprstate));

		/*
		 * 构建出版物缓存。无法使用 relcache 提供的，       
		 * 因为 relcache 考虑给定关系所在的全部出版物，     
		 * 而这里只需考虑订阅者请求的那些出版物    
		 * 。       
		 */
		foreach(lc, data->publications)
		{
			Publication *pub = lfirst(lc);
			bool		publish = false;

			/*
			 * 此出版物中应以哪个 relid 来发布变更？           
			 * 将用所有出版物中最顶层的 relid。还记录    
			 * 此出版物的祖先层级。                
			 */
			Oid			pub_relid = relid;
			int			ancestor_level = 0;

			/*
			 * 若这是 FOR ALL TABLES 出版物，则选取分区根    
			 * 并相应地设置祖先层级。           
			 */
			if (pub->alltables)
			{
				publish = true;
				if (pub->pubviaroot && am_partition)
				{
					List	   *ancestors = get_partition_ancestors(relid);

					pub_relid = llast_oid(ancestors);
					ancestor_level = list_length(ancestors);
				}
			}

			if (!publish)
			{
				bool		ancestor_published = false;

				/*
				 * 对分区，检查其祖先是否被发布。     
				 * 若是，则记下经该出版物发布的最顶层祖   
				 * 先，它将作为发布该分区变更所用的关系   
				 * 。                                                   
				 */
				if (am_partition)
				{
					Oid			ancestor;
					int			level;
					List	   *ancestors = get_partition_ancestors(relid);

					ancestor = GetTopMostAncestorInPublication(pub->oid,
															   ancestors,
															   &level);

					if (ancestor != InvalidOid)
					{
						ancestor_published = true;
						if (pub->pubviaroot)
						{
							pub_relid = ancestor;
							ancestor_level = level;
						}
					}
				}

				if (list_member_oid(pubids, pub->oid) ||
					list_member_oid(schemaPubids, pub->oid) ||
					ancestor_published)
					publish = true;
			}

			/*
			 * 若要发布该关系，则确定要发布的动作，  
			 * 以及（若适用）列的列表。         
			 *
			 * 不要为分区表发布变更，因为发布其     
			 * 分区的变更已足够，除非因设置 pubviaroot 而    
			 * 不发布分区变更。                               
			 */
			if (publish &&
				(relkind != RELKIND_PARTITIONED_TABLE || pub->pubviaroot))
			{
				entry->pubactions.pubinsert |= pub->pubactions.pubinsert;
				entry->pubactions.pubupdate |= pub->pubactions.pubupdate;
				entry->pubactions.pubdelete |= pub->pubactions.pubdelete;
				entry->pubactions.pubtruncate |= pub->pubactions.pubtruncate;

				/*
				 * 我们要以所有出版物中最顶层的祖先发    
				 * 布变更。因此需检查已算得的层级是否高于  
				 * 新层级。若高于，则可忽略新值（因其为子  
				 * 级）。否则新值就是祖先，故保留            
				 * 它。                              
				 */
				if (publish_ancestor_level > ancestor_level)
					continue;

				/*
				 * 若在树中找到了更高处的祖先，则丢弃       
				 * 复制它所用的出版物列表，并改用新的        
				 * 祖先。        
				 */
				if (publish_ancestor_level < ancestor_level)
				{
					publish_as_relid = pub_relid;
					publish_ancestor_level = ancestor_level;

					/* 重置该关系的出版物列表            */
					rel_publications = NIL;
				}
				else
				{
					/* 祖先层级相同，OID 必须相同。     */
					Assert(publish_as_relid == pub_relid);
				}

				/* 跟踪该祖先的出版物。        */
				rel_publications = lappend(rel_publications, pub);
			}
		}

		entry->publish_as_relid = publish_as_relid;

		/*
		 * 初始化元组槽、映射和行过滤。它们仅在发布       
		 * 插入、更新或删除时才会用到。   
		 */
		if (entry->pubactions.pubinsert || entry->pubactions.pubupdate ||
			entry->pubactions.pubdelete)
		{
			/* 初始化元组槽与映射       */
			init_tuple_slot(data, relation, entry);

			/* 初始化行过滤        */
			pgoutput_row_filter_init(data, rel_publications, entry);

			/* 初始化列列表         */
			pgoutput_column_list_init(data, rel_publications, entry);
		}

		list_free(pubids);
		list_free(schemaPubids);
		list_free(rel_publications);

		entry->replicate_valid = true;
	}

	return entry;
}

/*
 * 清理流式事务列表并更新 schema_sent 标志。               
 *
 * 当流式事务提交或中止时，我们需要从模式缓存中  
 * 移除顶层 XID。若事务已中止，订阅者会直接丢弃   
 * 我们流式发送的模式记录，因此不需要                 
 * 再做别的事情。             
 *
 * 若事务已提交，订阅者会自行更新关系缓存——因此   
 * 相应地调整 schema_sent 标志。             
 */
static void
cleanup_rel_sync_cache(TransactionId xid, bool is_commit)
{
	HASH_SEQ_STATUS hash_seq;
	RelationSyncEntry *entry;
	ListCell   *lc;

	Assert(RelationSyncCache != NULL);

	hash_seq_init(&hash_seq, RelationSyncCache);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		/*
		 * 对于列表中已有已提交 xid 的条目，我们可以设置  
		 * schema_sent 标志，因为这确保订阅者已有对应     
		 * 的模式，无需再发送，除非该关系有任何           
		 * 失效。                          
		 */
		foreach(lc, entry->streamed_txns)
		{
			if (xid == lfirst_xid(lc))
			{
				if (is_commit)
					entry->schema_sent = true;

				entry->streamed_txns =
					foreach_delete_current(entry->streamed_txns, lc);
				break;
			}
		}
	}
}

/*
 * relcache 失效回调         
 */
static void
rel_sync_cache_relation_cb(Datum arg, Oid relid)
{
	RelationSyncEntry *entry;

	/*
	 * 若插件曾用于 SQL 接口，就会到达此处，因为   
	 * 解码结束时 RelSchemaSyncCache 已被销毁，而                
	 * 没有办法注销 relcache 失效回调。                
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * 在逻辑解码回调调用之外，没人会保留指向该哈   
	 * 希表条目的指针——但失效事件可能在回调期间      
	 * *到来，如果我们在回调中做任何 syscache 访问。   
	 * 因此我们必须将缓存条目标记为无效，且在此处       
	 * 不损坏其任何子结构。下次 get_rel_sync_entry() 调用会   
	 * 重建它。   
	 */
	if (OidIsValid(relid))
	{
		/*
		 * 收到不属于该表的关系的失效消息很正常。      
		 * 因此是否找得到我们都无所谓。              
		 */
		entry = (RelationSyncEntry *) hash_search(RelationSyncCache, &relid,
												  HASH_FIND, NULL);
		if (entry != NULL)
			entry->replicate_valid = false;
	}
	else
	{
		/* 整个缓存必须刷写。  */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, RelationSyncCache);
		while ((entry = (RelationSyncEntry *) hash_seq_search(&status)) != NULL)
		{
			entry->replicate_valid = false;
		}
	}
}

/*
 * 出版物关系/模式映射 syscache 失效回调            
 *
 * 本回调在 pg_publication、pg_publication_rel、            
 * pg_publication_namespace 与 pg_namespace  
 */
static void
rel_sync_cache_publication_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	RelationSyncEntry *entry;

	/*
	 * 若插件曾用于 SQL 接口，就会到达此处，因为   
	 * 解码结束时 RelSchemaSyncCache 已被销毁，而                
	 * 没有办法注销失效回调。                  
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * 我们无法简便地确定这次失效事件可能影响了哪些   
	 * 缓存条目，所以干脆统统标记为无效。      
	 */
	hash_seq_init(&status, RelationSyncCache);
	while ((entry = (RelationSyncEntry *) hash_seq_search(&status)) != NULL)
	{
		entry->replicate_valid = false;
	}
}

/* 发送复制源         */
static void
send_repl_origin(LogicalDecodingContext *ctx, RepOriginId origin_id,
				 XLogRecPtr origin_lsn, bool send_origin)
{
	if (send_origin)
	{
		char	   *origin;

		/*----------
		 * XXX: 这里想要哪种行为？     
		 *
		 * 备选：    
		 *  - 若找不到复制源名称，则不发送消息  
		 *    （即现行做法）  
		 *  - 抛出错误——这会破坏复制，不好      
		 *  - 发送特殊"unknown"复制源    
		 *----------
		 */
		if (replorigin_by_oid(origin_id, true, &origin))
		{
			/* 消息边界     */
			OutputPluginWrite(ctx, false);
			OutputPluginPrepareWrite(ctx, true);

			logicalrep_write_origin(ctx->out, origin, origin_lsn);
		}
	}
}
