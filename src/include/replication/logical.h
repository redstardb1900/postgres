/*-------------------------------------------------------------------------
 * logical.h
 *	   PostgreSQL 逻辑解码协调
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICAL_H
#define LOGICAL_H

#include "access/xlog.h"
#include "access/xlogreader.h"
#include "replication/output_plugin.h"
#include "replication/slot.h"

struct LogicalDecodingContext;

typedef void (*LogicalOutputPluginWriterWrite) (struct LogicalDecodingContext *lr,
												XLogRecPtr Ptr,
												TransactionId xid,
												bool last_write
);

typedef LogicalOutputPluginWriterWrite LogicalOutputPluginWriterPrepareWrite;

typedef void (*LogicalOutputPluginWriterUpdateProgress) (struct LogicalDecodingContext *lr,
														 XLogRecPtr Ptr,
														 TransactionId xid,
														 bool skipped_xact
);

typedef struct LogicalDecodingContext
{
	/* memory context this is all allocated in */
	MemoryContext context;

	/*  关联的复制槽  */
	ReplicationSlot *slot;

	/*  解码基础设施组件  */
	XLogReaderState *reader;
	struct ReorderBuffer *reorder;
	struct SnapBuild *snapshot_builder;

	/*
	 * Marks the logical decoding context as fast forward decoding one. Such a
	 * context does not have plugin loaded so most of the following properties
	 * are unused.
	 */
	bool		fast_forward;

	OutputPluginCallbacks callbacks;
	OutputPluginOptions options;

	/*
	 * 用户指定的选项
	 */
	List	   *output_plugin_options;

	/*
	 * 用户提供的写出/流出数据回调。
	 */
	LogicalOutputPluginWriterPrepareWrite prepare_write;
	LogicalOutputPluginWriterWrite write;
	LogicalOutputPluginWriterUpdateProgress update_progress;

	/*
	 * Output buffer.
	 */
	StringInfo	out;

	/*
	 * 输出插件的私有数据指针。
	 */
	void	   *output_plugin_private;

	/*
	 * 数据写出器的私有数据指针。
	 */
	void	   *output_writer_private;

	/*
	 * 输出插件是否支持流式且已启用?
	 */
	bool		streaming;

	/*
	 * 输出插件是否支持两阶段解码且已启用?
	 */
	bool		twophase;

	/*
	 * Is two-phase option given by output plugin?
	 *
	 * This flag indicates that the plugin passed in the two-phase option as
	 * part of the START_STREAMING command. We can't rely solely on the
	 * twophase flag which only tells whether the plugin provided all the
	 * necessary two-phase callbacks.
	 */
	bool		twophase_opt_given;

	/*
	 * 写出状态。
	 */
	bool		accept_writes;
	bool		prepared_write;
	XLogRecPtr	write_location;
	TransactionId write_xid;
	/*  是否正在处理事务的结束 LSN?  */
	bool		end_xact;

	/*
	 * True if the logical decoding context being used for the creation
	 * of a logical replication slot.
	 */
	bool		in_create;
} LogicalDecodingContext;


extern void CheckLogicalDecodingRequirements(void);

extern LogicalDecodingContext *CreateInitDecodingContext(const char *plugin,
														 List *output_plugin_options,
														 bool need_full_snapshot,
														 XLogRecPtr restart_lsn,
														 XLogReaderRoutine *xl_routine,
														 LogicalOutputPluginWriterPrepareWrite prepare_write,
														 LogicalOutputPluginWriterWrite do_write,
														 LogicalOutputPluginWriterUpdateProgress update_progress);
extern LogicalDecodingContext *CreateDecodingContext(XLogRecPtr start_lsn,
													 List *output_plugin_options,
													 bool fast_forward,
													 XLogReaderRoutine *xl_routine,
													 LogicalOutputPluginWriterPrepareWrite prepare_write,
													 LogicalOutputPluginWriterWrite do_write,
													 LogicalOutputPluginWriterUpdateProgress update_progress);
extern void DecodingContextFindStartpoint(LogicalDecodingContext *ctx);
extern bool DecodingContextReady(LogicalDecodingContext *ctx);
extern void FreeDecodingContext(LogicalDecodingContext *ctx);

extern void LogicalIncreaseXminForSlot(XLogRecPtr current_lsn,
									   TransactionId xmin);
extern void LogicalIncreaseRestartDecodingForSlot(XLogRecPtr current_lsn,
												  XLogRecPtr restart_lsn);
extern void LogicalConfirmReceivedLocation(XLogRecPtr lsn);

extern bool filter_prepare_cb_wrapper(LogicalDecodingContext *ctx,
									  TransactionId xid, const char *gid);
extern bool filter_by_origin_cb_wrapper(LogicalDecodingContext *ctx, RepOriginId origin_id);
extern void ResetLogicalStreamingState(void);
extern void UpdateDecodingStats(LogicalDecodingContext *ctx);

#endif
