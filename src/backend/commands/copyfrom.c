/*-------------------------------------------------------------------------
 *
 * copyfrom.c
 *		COPY 表 FROM 文件/程序/客户端
 *
 * 本文件包含高效装载元组进表所需的例程。这包括  
 * 查找正确的分区、触发触发器、调用表 AM 函数来        
 * 插入数据并更新索引。从输入文件或客户端读取        
 * 数据并将其解析为 Datum 的工作由 copyfromparse.c          
 * 负责处理。               
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyfrom.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/progress.h"
#include "commands/trigger.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "executor/tuptable.h"
#include "foreign/fdwapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * 每个 CopyMultiInsertBuffer 不超过此数量的元组
 *
 * 注意：不要设置得太大，否则我们可能在 CopyMultiInsertInfo 的
 * multiInsertBuffers 列表中存储这么多 CopyMultiInsertBuffer 项。
 * 增大此值可能导致在向具有大量分区的分区表进行复制时，
 * 内存需求呈二次增长。
 *
 */
#define MAX_BUFFERED_TUPLES		1000

/*
 * 如果存储的元组的输入大小达到或超过此字节数，则刷新缓冲区。
 *
 */
#define MAX_BUFFERED_BYTES		65535

/* 刷新后将缓冲区列表剪裁回此数量 */
#define MAX_PARTITION_BUFFERS	32

/* 存储 CopyFrom 中与单个关系相关的多重插入数据。 */
typedef struct CopyMultiInsertBuffer
{
	TupleTableSlot *slots[MAX_BUFFERED_TUPLES]; /* 存储元组的数组 */
	ResultRelInfo *resultRelInfo;	/* 'relid' 的 ResultRelInfo */
	BulkInsertState bistate;	/* 如果是普通表，则为此 rel 的
									 * BulkInsertState；若为外部表则为 NULL */
	int			nused;			/* 包含元组的 'slots' 数量 */
	uint64		linenos[MAX_BUFFERED_TUPLES];	/* 复制流中元组的行号
						 */
} CopyMultiInsertBuffer;

/*
 * 存储一个或多个 CopyMultiInsertBuffer 以及其中存储的元组
 * 大小和数量的详细信息。这允许在向分区表进行 COPY 时
 * 同时存在多个缓冲区。
 */
typedef struct CopyMultiInsertInfo
{
	List	   *multiInsertBuffers; /* 已跟踪的 CopyMultiInsertBuffer 列表 */
	int			bufferedTuples; /* 所有缓冲区中缓冲的元组数 */
	int			bufferedBytes;	/* 所有缓冲元组的字节数 */
	CopyFromState cstate;		/* 此 CopyMultiInsertInfo 的复制状态 */
	EState	   *estate;			/* 用于 COPY 的执行器状态 */
	CommandId	mycid;			/* 用于 COPY 的命令 ID */
	int			ti_options;		/* 表插入选项 */
} CopyMultiInsertInfo;


/* 非导出函数原型 */
static char *limit_printout_length(const char *str);

static void ClosePipeFromProgram(CopyFromState cstate);

/*
 * COPY FROM 的错误上下文回调
 *
 * 错误上下文的参数必须是 CopyFromState。
 */
void
CopyFromErrorCallback(void *arg)
{
	CopyFromState cstate = (CopyFromState) arg;

	if (cstate->relname_only)
	{
		errcontext("COPY %s",
				   cstate->cur_relname);
		return;
	}
	if (cstate->opts.binary)
	{
		/* 无法有用地显示数据 */
		if (cstate->cur_attname)
			errcontext("COPY %s, line %llu, column %s",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno,
					   cstate->cur_attname);
		else
			errcontext("COPY %s, line %llu",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno);
	}
	else
	{
		if (cstate->cur_attname && cstate->cur_attval)
		{
			/* 错误与特定列相关 */
			char	   *attval;

			attval = limit_printout_length(cstate->cur_attval);
			errcontext("COPY %s, line %llu, column %s: \"%s\"",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno,
					   cstate->cur_attname,
					   attval);
			pfree(attval);
		}
		else if (cstate->cur_attname)
		{
			/* 错误与特定列相关，值为 NULL */
			errcontext("COPY %s, line %llu, column %s: null input",
					   cstate->cur_relname,
					   (unsigned long long) cstate->cur_lineno,
					   cstate->cur_attname);
		}
		else
		{
			/*
				 * 错误与特定行相关。
				 *
				 * 如果 line_buf 仍包含正确的行，则打印它。
				 */
			if (cstate->line_buf_valid)
			{
				char	   *lineval;

				lineval = limit_printout_length(cstate->line_buf.data);
				errcontext("COPY %s, line %llu: \"%s\"",
						   cstate->cur_relname,
						   (unsigned long long) cstate->cur_lineno, lineval);
				pfree(lineval);
			}
			else
			{
				errcontext("COPY %s, line %llu",
						   cstate->cur_relname,
						   (unsigned long long) cstate->cur_lineno);
			}
		}
	}
}

/*
 * 确保我们不会在消息中打印过多的 COPY 数据。
 *
 * 返回输入的 pstrdup 副本。
 */
static char *
limit_printout_length(const char *str)
{
#define MAX_COPY_DATA_DISPLAY 100

	int			slen = strlen(str);
	int			len;
	char	   *res;

	/* 如果确定没问题，走快速路径 */
	if (slen <= MAX_COPY_DATA_DISPLAY)
		return pstrdup(str);

	/* 应用编码相关的截断 */
	len = pg_mbcliplen(str, slen, MAX_COPY_DATA_DISPLAY);

	/*
	 * 截断并添加 "..." 以表示我们已截断输入。
	 */
	res = (char *) palloc(len + 4);
	memcpy(res, str, len);
	strcpy(res + len, "...");

	return res;
}

/*
 * 为此 ResultRelInfo 分配内存并初始化新的 CopyMultiInsertBuffer。
 *
 */
static CopyMultiInsertBuffer *
CopyMultiInsertBufferInit(ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer;

	buffer = (CopyMultiInsertBuffer *) palloc(sizeof(CopyMultiInsertBuffer));
	memset(buffer->slots, 0, sizeof(TupleTableSlot *) * MAX_BUFFERED_TUPLES);
	buffer->resultRelInfo = rri;
	buffer->bistate = (rri->ri_FdwRoutine == NULL) ? GetBulkInsertState() : NULL;
	buffer->nused = 0;

	return buffer;
}

/*
 * 为此 ResultRelInfo 创建新的缓冲区。
 */
static inline void
CopyMultiInsertInfoSetupBuffer(CopyMultiInsertInfo *miinfo,
							   ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer;

	buffer = CopyMultiInsertBufferInit(rri);

	/* 设置反向链接，以便我们可以轻松再次找到此缓冲区 */
	rri->ri_CopyMultiInsertBuffer = buffer;
	/* 记录我们正在跟踪此缓冲区 */
	miinfo->multiInsertBuffers = lappend(miinfo->multiInsertBuffers, buffer);
}

/*
 * 初始化已分配的 CopyMultiInsertInfo。
 *
 * 如果 rri 是非分区表，则为该表设置一个 CopyMultiInsertBuffer。
 *
 */
static void
CopyMultiInsertInfoInit(CopyMultiInsertInfo *miinfo, ResultRelInfo *rri,
						CopyFromState cstate, EState *estate, CommandId mycid,
						int ti_options)
{
	miinfo->multiInsertBuffers = NIL;
	miinfo->bufferedTuples = 0;
	miinfo->bufferedBytes = 0;
	miinfo->cstate = cstate;
	miinfo->estate = estate;
	miinfo->mycid = mycid;
	miinfo->ti_options = ti_options;

	/*
	 * 仅在不处理分区表时设置缓冲区。分区表的缓冲区
	 * 将在我们首次需要向它们发送元组时再设置。
	 *
	 */
	if (rri->ri_RelationDesc->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		CopyMultiInsertInfoSetupBuffer(miinfo, rri);
}

/*
 * 如果缓冲区已满，则返回 true
 */
static inline bool
CopyMultiInsertInfoIsFull(CopyMultiInsertInfo *miinfo)
{
	if (miinfo->bufferedTuples >= MAX_BUFFERED_TUPLES ||
		miinfo->bufferedBytes >= MAX_BUFFERED_BYTES)
		return true;
	return false;
}

/*
 * 如果没有缓冲的元组，则返回 true
 */
static inline bool
CopyMultiInsertInfoIsEmpty(CopyMultiInsertInfo *miinfo)
{
	return miinfo->bufferedTuples == 0;
}

/*
 * 将 'buffer' 中存储的元组写入表中。
 */
static inline void
CopyMultiInsertBufferFlush(CopyMultiInsertInfo *miinfo,
						   CopyMultiInsertBuffer *buffer,
						   int64 *processed)
{
	CopyFromState cstate = miinfo->cstate;
	EState	   *estate = miinfo->estate;
	int			nused = buffer->nused;
	ResultRelInfo *resultRelInfo = buffer->resultRelInfo;
	TupleTableSlot **slots = buffer->slots;
	int			i;

	if (resultRelInfo->ri_FdwRoutine)
	{
		int			batch_size = resultRelInfo->ri_BatchSize;
		int			sent = 0;

		Assert(buffer->bistate == NULL);

		/* 确保 FDW 支持批处理且已启用 */
		Assert(resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert);
		Assert(batch_size > 1);

		/*
		 * 如果下面的某个操作失败，我们会截断除关系名以外
		 * 的错误上下文信息。
		 */
		Assert(!cstate->relname_only);
		cstate->relname_only = true;

		while (sent < nused)
		{
			int			size = (batch_size < nused - sent) ? batch_size : (nused - sent);
			int			inserted = size;
			TupleTableSlot **rslots;

			/* 插入到外部表：让 FDW 来做 */
			rslots =
				resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert(estate,
																	 resultRelInfo,
																	 &slots[sent],
																	 NULL,
																	 &inserted);

			sent += size;

			/* 如果没有插入的行，无需做任何操作 */
			if (inserted <= 0)
				continue;

			/* 外部表上的触发器不应有过渡表 */
			Assert(resultRelInfo->ri_TrigDesc == NULL ||
				   resultRelInfo->ri_TrigDesc->trig_insert_new_table == false);

			/* 运行 AFTER ROW INSERT 触发器 */
			if (resultRelInfo->ri_TrigDesc != NULL &&
				resultRelInfo->ri_TrigDesc->trig_insert_after_row)
			{
				Oid			relid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

				for (i = 0; i < inserted; i++)
				{
					TupleTableSlot *slot = rslots[i];

					/*
					 * AFTER ROW 触发器可能引用 tableoid 列，
					 * 所以在评估之前（重新）初始化 tts_tableOid。
					 */
					slot->tts_tableOid = relid;

					ExecARInsertTriggers(estate, resultRelInfo,
										 slot, NIL,
										 cstate->transition_capture);
				}
			}

			/* 更新 COPY 命令的行计数器和进度 */
			*processed += inserted;
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 *processed);
		}

		for (i = 0; i < nused; i++)
			ExecClearTuple(slots[i]);

		/* 重置 relname_only */
		cstate->relname_only = false;
	}
	else
	{
		CommandId	mycid = miinfo->mycid;
		int			ti_options = miinfo->ti_options;
		bool		line_buf_valid = cstate->line_buf_valid;
		uint64		save_cur_lineno = cstate->cur_lineno;
		MemoryContext oldcontext;

		Assert(buffer->bistate != NULL);

		/*
		 * 如果下面的某个操作失败，正确打印错误上下文信息。
		 *
		 */
		cstate->line_buf_valid = false;

		/*
		 * table_multi_insert 可能会泄漏内存，所以在调用之前
		 * 切换到短生命内存上下文。
		 */
		oldcontext = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		table_multi_insert(resultRelInfo->ri_RelationDesc,
						   slots,
						   nused,
						   mycid,
						   ti_options,
						   buffer->bistate);
		MemoryContextSwitchTo(oldcontext);

		for (i = 0; i < nused; i++)
		{
			/*
			 * 如果有任何索引，则为所有插入的元组更新它们，
			 * 并运行 AFTER ROW INSERT 触发器。
			 */
			if (resultRelInfo->ri_NumIndices > 0)
			{
				List	   *recheckIndexes;

				cstate->cur_lineno = buffer->linenos[i];
				recheckIndexes =
					ExecInsertIndexTuples(resultRelInfo,
										  buffer->slots[i], estate, false,
										  false, NULL, NIL, false);
				ExecARInsertTriggers(estate, resultRelInfo,
									 slots[i], recheckIndexes,
									 cstate->transition_capture);
				list_free(recheckIndexes);
			}

			/*
			 * 没有索引，但检查是否仍然需要运行
			 * AFTER ROW INSERT 触发器。
			 */
			else if (resultRelInfo->ri_TrigDesc != NULL &&
					 (resultRelInfo->ri_TrigDesc->trig_insert_after_row ||
					  resultRelInfo->ri_TrigDesc->trig_insert_new_table))
			{
				cstate->cur_lineno = buffer->linenos[i];
				ExecARInsertTriggers(estate, resultRelInfo,
									 slots[i], NIL,
									 cstate->transition_capture);
			}

			ExecClearTuple(slots[i]);
		}

		/* 更新 COPY 命令的行计数器和进度 */
		*processed += nused;
		pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
									 *processed);

		/* 将 cur_lineno 和 line_buf_valid 重置为原来的值 */
		cstate->line_buf_valid = line_buf_valid;
		cstate->cur_lineno = save_cur_lineno;
	}

	/* 标记所有 slot 已释放 */
	buffer->nused = 0;
}

/*
 * 释放已用的 slot 并释放此缓冲区的成员。
 *
 * 清理前必须先刷新缓冲区。
 */
static inline void
CopyMultiInsertBufferCleanup(CopyMultiInsertInfo *miinfo,
							 CopyMultiInsertBuffer *buffer)
{
	ResultRelInfo *resultRelInfo = buffer->resultRelInfo;
	int			i;

	/* 确保缓冲区已刷新 */
	Assert(buffer->nused == 0);

	/* 移除指向我们自己的反向链接 */
	resultRelInfo->ri_CopyMultiInsertBuffer = NULL;

	if (resultRelInfo->ri_FdwRoutine == NULL)
	{
		Assert(buffer->bistate != NULL);
		FreeBulkInsertState(buffer->bistate);
	}
	else
		Assert(buffer->bistate == NULL);

	/* 由于我们仅在需要时创建 slot，只释放非空的。 */
	for (i = 0; i < MAX_BUFFERED_TUPLES && buffer->slots[i] != NULL; i++)
		ExecDropSingleTupleTableSlot(buffer->slots[i]);

	if (resultRelInfo->ri_FdwRoutine == NULL)
		table_finish_bulk_insert(resultRelInfo->ri_RelationDesc,
								 miinfo->ti_options);

	pfree(buffer);
}

/*
 * 将所有缓冲区中的所有存储的元组写入表中。
 *
 * 刷新后，我们还通过优先移除最早创建的缓冲区来
 * 将已跟踪的缓冲区列表剪裁到指定大小。
 *
 * 调用者应将当前正在使用的 ResultRelInfo 传递为 'curr_rri'。
 * 在清理旧缓冲区时，我们永远不会移除 'curr_rri' 的缓冲区。
 *
 */
static inline void
CopyMultiInsertInfoFlush(CopyMultiInsertInfo *miinfo, ResultRelInfo *curr_rri,
						 int64 *processed)
{
	ListCell   *lc;

	foreach(lc, miinfo->multiInsertBuffers)
	{
		CopyMultiInsertBuffer *buffer = (CopyMultiInsertBuffer *) lfirst(lc);

		CopyMultiInsertBufferFlush(miinfo, buffer, processed);
	}

	miinfo->bufferedTuples = 0;
	miinfo->bufferedBytes = 0;

	/*
	 * 如果已跟踪的缓冲区列表超过限制，则剪裁它。我们
	 * 从最先创建的缓冲区开始移除。这些旧缓冲区比刚创建的
	 * 更不可能被需要。
	 *
	 */
	while (list_length(miinfo->multiInsertBuffers) > MAX_PARTITION_BUFFERS)
	{
		CopyMultiInsertBuffer *buffer;

		buffer = (CopyMultiInsertBuffer *) linitial(miinfo->multiInsertBuffers);

		/*
		 * 我们永远不想移除当前正在使用的缓冲区，
		 * 所以如果碰到它，则将其移到列表末尾。
		 */
		if (buffer->resultRelInfo == curr_rri)
		{
			miinfo->multiInsertBuffers = list_delete_first(miinfo->multiInsertBuffers);
			miinfo->multiInsertBuffers = lappend(miinfo->multiInsertBuffers, buffer);
			buffer = (CopyMultiInsertBuffer *) linitial(miinfo->multiInsertBuffers);
		}

		CopyMultiInsertBufferCleanup(miinfo, buffer);
		miinfo->multiInsertBuffers = list_delete_first(miinfo->multiInsertBuffers);
	}
}

/*
 * 清理已分配的缓冲区并释放内存
 */
static inline void
CopyMultiInsertInfoCleanup(CopyMultiInsertInfo *miinfo)
{
	ListCell   *lc;

	foreach(lc, miinfo->multiInsertBuffers)
		CopyMultiInsertBufferCleanup(miinfo, lfirst(lc));

	list_free(miinfo->multiInsertBuffers);
}

/*
 * 获取下一个元组应存储在其中的 TupleTableSlot。
 *
 * 调用者必须确保缓冲区未满。
 *
 * 注意：'miinfo' 未使用，但为了与此区域中的其他函数保持一致而包含。
 *
 */
static inline TupleTableSlot *
CopyMultiInsertInfoNextFreeSlot(CopyMultiInsertInfo *miinfo,
								ResultRelInfo *rri)
{
	CopyMultiInsertBuffer *buffer = rri->ri_CopyMultiInsertBuffer;
	int			nused = buffer->nused;

	Assert(buffer != NULL);
	Assert(nused < MAX_BUFFERED_TUPLES);

	if (buffer->slots[nused] == NULL)
		buffer->slots[nused] = table_slot_create(rri->ri_RelationDesc, NULL);
	return buffer->slots[nused];
}

/*
 * 记录之前由 CopyMultiInsertInfoNextFreeSlot 预留的
 * TupleTableSlot 已被消费。
 */
static inline void
CopyMultiInsertInfoStore(CopyMultiInsertInfo *miinfo, ResultRelInfo *rri,
						 TupleTableSlot *slot, int tuplen, uint64 lineno)
{
	CopyMultiInsertBuffer *buffer = rri->ri_CopyMultiInsertBuffer;

	Assert(buffer != NULL);
	Assert(slot == buffer->slots[buffer->nused]);

	/* 存储行号，以便之后能正确报告任何错误 */
	buffer->linenos[buffer->nused] = lineno;

	/* 记录此 slot 已被使用 */
	buffer->nused++;

	/* 更新存储的元组数量和其大小 */
	miinfo->bufferedTuples++;
	miinfo->bufferedBytes += tuplen;
}

/*
 * 从文件复制到关系。
 */
uint64
CopyFrom(CopyFromState cstate)
{
	ResultRelInfo *resultRelInfo;
	ResultRelInfo *target_resultRelInfo;
	ResultRelInfo *prevResultRelInfo = NULL;
	EState	   *estate = CreateExecutorState(); /* 用于 ExecConstraints() */
	ModifyTableState *mtstate;
	ExprContext *econtext;
	TupleTableSlot *singleslot = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;

	PartitionTupleRouting *proute = NULL;
	ErrorContextCallback errcallback;
	CommandId	mycid = GetCurrentCommandId(true);
	int			ti_options = 0; /* 从插入的默认选项开始 */
	BulkInsertState bistate = NULL;
	CopyInsertMethod insertMethod;
	CopyMultiInsertInfo multiInsertInfo = {0};	/* 让编译器保持安静 */
	int64		processed = 0;
	int64		excluded = 0;
	bool		has_before_insert_row_trig;
	bool		has_instead_insert_row_trig;
	bool		leafpart_use_multi_insert = false;

	Assert(cstate->rel);
	Assert(list_length(cstate->range_table) == 1);

	/*
	 * 目标必须是普通、外部或分区关系，或者具有 INSTEAD OF INSERT
	 * 行触发器。（目前，此类触发器仅允许在视图上，
	 * 所以我们仅在视图场景中提示它们。）
	 */
	if (cstate->rel->rd_rel->relkind != RELKIND_RELATION &&
		cstate->rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		cstate->rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		!(cstate->rel->trigdesc &&
		  cstate->rel->trigdesc->trig_insert_instead_row))
	{
		if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to view \"%s\"",
							RelationGetRelationName(cstate->rel)),
					 errhint("To enable copying to a view, provide an INSTEAD OF INSERT trigger.")));
		else if (cstate->rel->rd_rel->relkind == RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to materialized view \"%s\"",
							RelationGetRelationName(cstate->rel))));
		else if (cstate->rel->rd_rel->relkind == RELKIND_SEQUENCE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to sequence \"%s\"",
							RelationGetRelationName(cstate->rel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy to non-table relation \"%s\"",
							RelationGetRelationName(cstate->rel))));
	}

	/*
	 * 如果目标文件是事务内新创建的，我们假定检查 FSM
	 * 中的空闲空间是浪费时间。这可能是错误的，但几乎不可能。
	 *
	 */
	if (RELKIND_HAS_STORAGE(cstate->rel->rd_rel->relkind) &&
		(cstate->rel->rd_createSubid != InvalidSubTransactionId ||
		 cstate->rel->rd_firstRelfilelocatorSubid != InvalidSubTransactionId))
		ti_options |= TABLE_INSERT_SKIP_FSM;

	/*
	 * 如果在此子事务或其已提交的子事务中创建了新的关系存储，
	 * 且我们不会在之后的扫描或命令中看到这些行，则进行优化。
	 * 子事务测试确保如果此子事务中止，则冻结行在事务清理后不会可见。
	 * 注意，精确确定是哪个子事务创建它的更强测试对此优化的
	 * 正确性至关重要。对更早扫描或命令的测试允许假阴性。
	 * FREEZE 会导致其他会话看到在 MVCC 下不会看到的行，
	 * 假阴性仅将该异常扩展到当前会话。
	 *
	 *
	 */
	if (cstate->opts.freeze)
	{
		/*
		 * 我们目前不允许在分区表上使用 COPY FREEZE。
		 * 原因是我们尚未打开分区来确定是否可以应用此优化。
		 * 我们可以在这里全部打开它们，但对于小型复制来说，
		 * 这可能是相当昂贵的开销。无论如何，我们可能最终只将
		 * 元组路由到少数分区。似乎更好的做法是对分区表
		 * 直接抛出 ERROR。
		 *
		 */
		if (cstate->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot perform COPY FREEZE on a partitioned table")));
		}

		/*
		 * 为了 FirstXactSnapshot 的好处，容忍一次注册。
		 * 带有扫描的查询通常会创建至少两次注册，不过
		 * 依赖此点是脆弱的，就像忽略 ActiveSnapshot 一样。
		 * 清除 CatalogSnapshot 以避免计算其注册。我们
		 * 仍会检测到正在进行的目录扫描，每个扫描会单独
		 * 注册其使用的快照。
		 */
		InvalidateCatalogSnapshot();
		if (!ThereAreNoPriorRegisteredSnapshots() || !ThereAreNoReadyPortals())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("cannot perform COPY FREEZE because of prior transaction activity")));

		if (cstate->rel->rd_createSubid != GetCurrentSubTransactionId() &&
			cstate->rel->rd_newRelfilelocatorSubid != GetCurrentSubTransactionId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot perform COPY FREEZE because the table was not created or truncated in the current subtransaction")));

		ti_options |= TABLE_INSERT_FROZEN;
	}

	/*
	 * 我们需要 ResultRelInfo 以便可以使用普通执行器的
	 * 索引项创建机制。（这里曾有大量代码基本上复制了
	 * execUtils.c ...）
	 */
	ExecInitRangeTable(estate, cstate->range_table, cstate->rteperminfos);
	resultRelInfo = target_resultRelInfo = makeNode(ResultRelInfo);
	ExecInitResultRelation(estate, resultRelInfo, 1);

	/* 验证命名的关系是 INSERT 的有效目标 */
	CheckValidResultRel(resultRelInfo, CMD_INSERT);

	ExecOpenIndices(resultRelInfo, false);

	/*
	 * 设置 ModifyTableState 以便我们可以让 FDW 为
	 * 外部表结果关系自行初始化。
	 */
	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = CMD_INSERT;
	mtstate->mt_nrels = 1;
	mtstate->resultRelInfo = resultRelInfo;
	mtstate->rootResultRelInfo = resultRelInfo;

	if (resultRelInfo->ri_FdwRoutine != NULL &&
		resultRelInfo->ri_FdwRoutine->BeginForeignInsert != NULL)
		resultRelInfo->ri_FdwRoutine->BeginForeignInsert(mtstate,
														 resultRelInfo);

	/*
	 * 此外，如果命名关系是外部表，则确定 FDW 是否
	 * 支持批量插入并确定批量大小（FDW 可能支持批处理，
	 * 但可能对服务器/表禁用）。
	 *
	 * 如果 FDW 不支持批处理，则将批量大小设置为 1。
	 */
	if (resultRelInfo->ri_FdwRoutine != NULL &&
		resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize &&
		resultRelInfo->ri_FdwRoutine->ExecForeignBatchInsert)
		resultRelInfo->ri_BatchSize =
			resultRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize(resultRelInfo);
	else
		resultRelInfo->ri_BatchSize = 1;

	Assert(resultRelInfo->ri_BatchSize >= 1);

	/* 准备捕获 AFTER 触发器。 */
	AfterTriggerBeginQuery();

	/*
	 * 如果命名关系上有任何带过渡表的触发器，
	 * 我们需要准备捕获过渡元组。
	 *
	 * 因为分区元组路由需要知道过渡捕获是否活动，
	 * 我们也在 mtstate 中设置它，该值会传递给下面的
	 * ExecFindPartition()。
	 */
	cstate->transition_capture = mtstate->mt_transition_capture =
		MakeTransitionCaptureState(cstate->rel->trigdesc,
								   RelationGetRelid(cstate->rel),
								   CMD_INSERT);

	/*
	 * 如果命名关系是分区表，则初始化 CopyFrom 元组路由的状态。
	 *
	 */
	if (cstate->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		proute = ExecSetupPartitionTupleRouting(estate, cstate->rel);

	if (cstate->whereClause)
		cstate->qualexpr = ExecInitQual(castNode(List, cstate->whereClause),
										&mtstate->ps);

	/*
	 * 通常，准备一批元组并在一次
	 * table_multi_insert()/ExecForeignBatchInsert() 调用中插入它们
	 * 比为每个元组单独调用 table_tuple_insert()/ExecForeignInsert()
	 * 更有效率。但是，有一些原因可能使我们无法这样做。
	 * 这些原因在下面解释。
	 *
	 */
	if (resultRelInfo->ri_TrigDesc != NULL &&
		(resultRelInfo->ri_TrigDesc->trig_insert_before_row ||
		 resultRelInfo->ri_TrigDesc->trig_insert_instead_row))
	{
		/*
		 * 当表上有任何 BEFORE/INSTEAD OF 触发器时，无法支持
		 * 多重插入。这类触发器可能查询我们正在插入的表，
		 * 如果已处理并准备插入的元组不在，则会产生不同的行为。
		 *
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (resultRelInfo->ri_FdwRoutine != NULL &&
			 resultRelInfo->ri_BatchSize == 1)
	{
		/*
		 * 如果 FDW 不支持批处理，或对服务器或外部表禁用，
		 * 则无法支持向外部表进行多重插入。
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (proute != NULL && resultRelInfo->ri_TrigDesc != NULL &&
			 resultRelInfo->ri_TrigDesc->trig_insert_new_table)
	{
		/*
		 * 对于分区表，当存在任何语句级插入触发器时， 
		 * 我们无法支持多重插入。未来或许可以允许带    
		 * 有这类触发器的分区表，但目前                        
		 * CopyMultiInsertInfoFlush 期望任何 AFTER ROW INSERT 与         
		 * 语句级 INSERT 触发器在同一关系上。          
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (cstate->volatile_defexprs)
	{
		/*
		 * 如果表中有任何易变的默认表达式，则无法
		 * 支持多重插入。与上述触发器场景类似，
		 * 此类表达式可能查询我们正在插入的表。
		 *
		 * 注意：分区是否有易变的默认表达式无关紧要，
		 * 因为我们使用 COPY 命令目标的默认值。
		 *
		 */
		insertMethod = CIM_SINGLE;
	}
	else if (contain_volatile_functions(cstate->whereClause))
	{
		/*
		 * 如果 WHERE 子句中有任何易变函数表达式，则
		 * 无法支持多重插入。与上述触发器场景类似，
		 * 此类表达式可能查询我们正在插入的表。
		 *
		 * 注意：whereClause 已在 DoCopy() 中预处理，所以
		 * 直接使用 contain_volatile_functions() 是可以的。
		 */
		insertMethod = CIM_SINGLE;
	}
	else
	{
		/*
		 * 对于分区表，我们可能仍能执行批量插入。   
		 * 不过其可能性取决于分区上有哪些触发器类型。  
		 * 如果分区是无法使用批处理的外部表，或者它     
		 * 拥有任何 before row insert 或 insert instead 触发器      
		 * （与我们上面对父表的检查相同），就必须       
		 * 禁用批量插入。由于分区的                       
		 * resultRelInfos 仅在我们实际需要向其中插入             
		 * 第一个元组时才会初始化，因此必须使用         
		 * 中间插入方法 CIM_MULTI_CONDITIONAL，以标记我们
		 * 必须在之后确定能否对正在插入的分区使用批
		 * 量插入。  
		 */
		if (proute)
			insertMethod = CIM_MULTI_CONDITIONAL;
		else
			insertMethod = CIM_MULTI;

		CopyMultiInsertInfoInit(&multiInsertInfo, resultRelInfo, cstate,
								estate, mycid, ti_options);
	}

	/*
	 * 如果不使用批处理模式（该模式会按需分配 slot），
	 * 则也设置一个元组 slot。向分区表插入时，即使我们
	 * 可能使用批量插入，也需要一个以便以根分区的形式读取元组。
	 *
	 */
	if (insertMethod == CIM_SINGLE || insertMethod == CIM_MULTI_CONDITIONAL)
	{
		singleslot = table_slot_create(resultRelInfo->ri_RelationDesc,
									   &estate->es_tupleTable);
		bistate = GetBulkInsertState();
	}

	has_before_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
								  resultRelInfo->ri_TrigDesc->trig_insert_before_row);

	has_instead_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
								   resultRelInfo->ri_TrigDesc->trig_insert_instead_row);

	/*
	 * 检查 BEFORE STATEMENT 插入触发器。是否应当对 COPY 执行此操作
	 * 存在争议，因为它并不是真正的 "INSERT" 语句。
	 * 但是，执行这些触发器可以与我们在 COPY 上已触发的
	 * EACH ROW 触发器保持一致性。
	 */
	ExecBSInsertTriggers(estate, resultRelInfo);

	econtext = GetPerTupleExprContext(estate);

	/* 设置回调以标识错误行号 */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = (void *) cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (;;)
	{
		TupleTableSlot *myslot;
		bool		skip_tuple;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 重置每元组的 exprcontext。我们在每个元组后执行此操作，
		 * 以清理表达式评估等。
		 */
		ResetPerTupleExprContext(estate);

		/* 选择 slot 以（初始）加载行 */
		if (insertMethod == CIM_SINGLE || proute)
		{
			myslot = singleslot;
			Assert(myslot != NULL);
		}
		else
		{
			Assert(resultRelInfo == target_resultRelInfo);
			Assert(insertMethod == CIM_MULTI);

			myslot = CopyMultiInsertInfoNextFreeSlot(&multiInsertInfo,
													 resultRelInfo);
		}

		/*
		 * 在调用 NextCopyFrom 之前切换到每元组上下文，NextCopyFrom
		 * 会评估默认表达式等，需要每元组上下文。
		 */
		MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		ExecClearTuple(myslot);

		/* 直接将 values/nulls 数组存储在 slot 中 */
		if (!NextCopyFrom(cstate, econtext, myslot->tts_values, myslot->tts_isnull))
			break;

		ExecStoreVirtualTuple(myslot);

		/*
		 * 约束和 where 子句可能引用 tableoid 列，
		 * 所以在评估它们之前（重新）初始化 tts_tableOid。
		 */
		myslot->tts_tableOid = RelationGetRelid(target_resultRelInfo->ri_RelationDesc);

		/* 触发器等需要在查询上下文中调用。        */
		MemoryContextSwitchTo(oldcontext);

		if (cstate->whereClause)
		{
			econtext->ecxt_scantuple = myslot;
			/* 跳过不匹配 COPY WHERE 子句的项 */
			if (!ExecQual(cstate->qualexpr, econtext))
			{
				/*
				 * 报告此元组已被 WHERE 子句过滤。
				 *
				 */
				pgstat_progress_update_param(PROGRESS_COPY_TUPLES_EXCLUDED,
											 ++excluded);
				continue;
			}
		}

		/* 确定要将元组插入到哪个分区 */
		if (proute)
		{
			TupleConversionMap *map;

			/*
			 * 尝试找到适合此元组的分区。
			 * 如果找不到或找到的分区不适合 INSERT，
			 * ExecFindPartition() 将抛出错误。
			 */
			resultRelInfo = ExecFindPartition(mtstate, target_resultRelInfo,
											  proute, myslot, estate);

			if (prevResultRelInfo != resultRelInfo)
			{
				/* 确定此分区上存在哪些触发器 */
				has_before_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
											  resultRelInfo->ri_TrigDesc->trig_insert_before_row);

				has_instead_insert_row_trig = (resultRelInfo->ri_TrigDesc &&
											   resultRelInfo->ri_TrigDesc->trig_insert_instead_row);

				/*
				 * 当分区具有 BEFORE/INSTEAD OF 触发器时，
				 * 或分区是不能使用批处理的外部表时，
				 * 禁用多重插入。
				 */
				leafpart_use_multi_insert = insertMethod == CIM_MULTI_CONDITIONAL &&
					!has_before_insert_row_trig &&
					!has_instead_insert_row_trig &&
					(resultRelInfo->ri_FdwRoutine == NULL ||
					 resultRelInfo->ri_BatchSize > 1);

				/* 设置此分区要使用的多重插入缓冲区。 */
				if (leafpart_use_multi_insert)
				{
					if (resultRelInfo->ri_CopyMultiInsertBuffer == NULL)
						CopyMultiInsertInfoSetupBuffer(&multiInsertInfo,
													   resultRelInfo);
				}
				else if (insertMethod == CIM_MULTI_CONDITIONAL &&
						 !CopyMultiInsertInfoIsEmpty(&multiInsertInfo))
				{
					/*
					 * 如果此分区不能使用批处理，则刷新待处理的
					 * 插入，以便触发器等能看到行。
					 */
					CopyMultiInsertInfoFlush(&multiInsertInfo,
											 resultRelInfo,
											 &processed);
				}

				if (bistate != NULL)
					ReleaseBulkInsertStatePin(bistate);
				prevResultRelInfo = resultRelInfo;
			}

			/*
			 * 如果我们正在捕获过渡元组，可能需要从
			 * 分区行类型转换到根行类型。但如果分区上没有
			 * 可以更改元组的 BEFORE 触发器，
			 * 我们可以只记住原始未转换的元组，以避免
			 * 不必要的往返转换。
			 */
			if (cstate->transition_capture != NULL)
				cstate->transition_capture->tcs_original_insert_tuple =
					!has_before_insert_row_trig ? myslot : NULL;

			/*
			 * 我们可能需要从根行类型转换到分区行类型。
			 *
			 */
			map = ExecGetRootToChildMap(resultRelInfo, estate);
			if (insertMethod == CIM_SINGLE || !leafpart_use_multi_insert)
			{
				/* 非批量插入 */
				if (map != NULL)
				{
					TupleTableSlot *new_slot;

					new_slot = resultRelInfo->ri_PartitionTupleSlot;
					myslot = execute_attr_map_slot(map->attrMap, myslot, new_slot);
				}
			}
			else
			{
				/*
				 * 准备将元组排队，以便之后批量插入到
				 * 当前分区。
				 */
				TupleTableSlot *batchslot;

				/* 分区表没有其他可用的路径 */
				Assert(insertMethod == CIM_MULTI_CONDITIONAL);

				batchslot = CopyMultiInsertInfoNextFreeSlot(&multiInsertInfo,
															resultRelInfo);

				if (map != NULL)
					myslot = execute_attr_map_slot(map->attrMap, myslot,
												   batchslot);
				else
				{
					/*
					 * 这看起来比实际更昂贵（相信我，我
					 * 优化过它。两次。）。输入是虚拟形式，
					 * 我们将在下面实体化 slot，对于大多数
					 * slot 类型来说，拷贝会执行实体化操作
					 * 后续无论如何都会需要的工作。
					 */
					ExecCopySlot(batchslot, myslot);
					myslot = batchslot;
				}
			}

			/* 确保触发器等看到正确的关系 */
			myslot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
		}

		skip_tuple = false;

		/* BEFORE ROW INSERT 触发器 */
		if (has_before_insert_row_trig)
		{
			if (!ExecBRInsertTriggers(estate, resultRelInfo, myslot))
				skip_tuple = true;	/* "不做任何事" */
		}

		if (!skip_tuple)
		{
			/*
			 * 如果存在 INSTEAD OF INSERT ROW 触发器，让它处理
			 * 元组。否则，继续将元组插入到表或外部表中。
			 *
			 */
			if (has_instead_insert_row_trig)
			{
				ExecIRInsertTriggers(estate, resultRelInfo, myslot);
			}
			else
			{
				/* 计算存储的生成列 */
				if (resultRelInfo->ri_RelationDesc->rd_att->constr &&
					resultRelInfo->ri_RelationDesc->rd_att->constr->has_generated_stored)
					ExecComputeStoredGenerated(resultRelInfo, estate, myslot,
											   CMD_INSERT);

				/*
				 * 如果目标是普通表，检查元组的约束。
				 *
				 */
				if (resultRelInfo->ri_FdwRoutine == NULL &&
					resultRelInfo->ri_RelationDesc->rd_att->constr)
					ExecConstraints(resultRelInfo, myslot, estate);

				/*
				 * 同时检查元组是否符合分区约束（如果有的话）；
				 * 但如果我们通过元组路由到达此处，
				 * 且分区上没有定义 BR 触发器，则无需检查。
				 *
				 */
				if (resultRelInfo->ri_RelationDesc->rd_rel->relispartition &&
					(proute == NULL || has_before_insert_row_trig))
					ExecPartitionCheck(resultRelInfo, myslot, estate, true);

				/* 当启用时，将 slot 存储在多重插入缓冲区中。 */
				if (insertMethod == CIM_MULTI || leafpart_use_multi_insert)
				{
					/*
					 * slot 之前可能指向每元组上下文。
					 * 对于批处理，它需要更长的生命周期。
					 */
					ExecMaterializeSlot(myslot);

					/* 将此元组添加到元组缓冲区 */
					CopyMultiInsertInfoStore(&multiInsertInfo,
											 resultRelInfo, myslot,
											 cstate->line_buf.len,
											 cstate->cur_lineno);

					/*
					 * 如果已排队足够多的插入，则
					 * 将所有缓冲区刷新到其表中。
					 */
					if (CopyMultiInsertInfoIsFull(&multiInsertInfo))
						CopyMultiInsertInfoFlush(&multiInsertInfo,
												 resultRelInfo,
												 &processed);

					/*
					 * 我们延迟更新 COPY 命令的行计数器和
					 * 进度，直到将缓冲区中存储的元组写入
					 * 表中之后，与单个插入模式一样。
					 * 参见 CopyMultiInsertBufferFlush()。
					 */
					continue;	/* 请继续处理下一个元组 */
				}
				else
				{
					List	   *recheckIndexes = NIL;

					/* 好的，存储元组 */
					if (resultRelInfo->ri_FdwRoutine != NULL)
					{
						myslot = resultRelInfo->ri_FdwRoutine->ExecForeignInsert(estate,
																				 resultRelInfo,
																				 myslot,
																				 NULL);

						if (myslot == NULL) /* "不做任何事" */
							continue;	/* 请继续处理下一个元组 */

						/*
						 * AFTER ROW 触发器可能引用 tableoid
						 * 列，所以在评估它们之前
						 * （重新）初始化 tts_tableOid。
						 */
						myslot->tts_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
					}
					else
					{
						/* 好的，存储元组并为其创建索引项 */
						table_tuple_insert(resultRelInfo->ri_RelationDesc,
										   myslot, mycid, ti_options, bistate);

						if (resultRelInfo->ri_NumIndices > 0)
							recheckIndexes = ExecInsertIndexTuples(resultRelInfo,
																   myslot,
																   estate,
																   false,
																   false,
																   NULL,
																   NIL,
																   false);
					}

					/* AFTER ROW INSERT 触发器 */
					ExecARInsertTriggers(estate, resultRelInfo, myslot,
										 recheckIndexes, cstate->transition_capture);

					list_free(recheckIndexes);
				}
			}

			/*
			 * 我们仅计数未被 BEFORE INSERT 触发器或 FDW 截断的元组；
			 * 这与 nodeModifyTable.c 用于计数 INSERT 命令插入的元组的
			 * 定义相同。同时更新 COPY 命令的进度。
			 *
			 */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 ++processed);
		}
	}

	/* 刷新任何剩余的缓冲元组 */
	if (insertMethod != CIM_SINGLE)
	{
		if (!CopyMultiInsertInfoIsEmpty(&multiInsertInfo))
			CopyMultiInsertInfoFlush(&multiInsertInfo, NULL, &processed);
	}

	/* 完成，清理 */
	error_context_stack = errcallback.previous;

	if (bistate != NULL)
		FreeBulkInsertState(bistate);

	MemoryContextSwitchTo(oldcontext);

	/* 执行 AFTER STATEMENT 插入触发器 */
	ExecASInsertTriggers(estate, target_resultRelInfo, cstate->transition_capture);

	/* 处理排队的 AFTER 触发器 */
	AfterTriggerEndQuery(estate);

	ExecResetTupleTable(estate->es_tupleTable, false);

	/* 允许 FDW 关闭 */
	if (target_resultRelInfo->ri_FdwRoutine != NULL &&
		target_resultRelInfo->ri_FdwRoutine->EndForeignInsert != NULL)
		target_resultRelInfo->ri_FdwRoutine->EndForeignInsert(estate,
															  target_resultRelInfo);

	/* 拆除多重插入缓冲区数据 */
	if (insertMethod != CIM_SINGLE)
		CopyMultiInsertInfoCleanup(&multiInsertInfo);

	/* 关闭所有分区表、叶分区及其索引 */
	if (proute)
		ExecCleanupTupleRouting(mtstate, proute);

	/* 关闭结果关系，包括任何触发器目标关系 */
	ExecCloseResultRelations(estate);
	ExecCloseRangeTableRelations(estate);

	FreeExecutorState(estate);

	return processed;
}

/*
 * 设置从文件读取元组用于 COPY FROM。
 *
 * 'rel': 用作元组的模板
 * 'whereClause': 来自 COPY FROM 命令的 WHERE 子句
 * 'filename': 要读取的服务器本地文件名，NULL 表示 STDIN
 * 'is_program': 如果 'filename' 是要执行的程序则为 true
 * 'data_source_cb': 提供输入数据的回调
 * 'attnamelist': char * 列表，要包含的列。NIL 表示选择所有列。
 * 'options': DefElem 列表。参见 gram.y 中的 copy_opt_item 以获取选项。
 *
 * 返回 CopyFromState，传递给 NextCopyFrom 及相关函数。
 */
CopyFromState
BeginCopyFrom(ParseState *pstate,
			  Relation rel,
			  Node *whereClause,
			  const char *filename,
			  bool is_program,
			  copy_data_source_cb data_source_cb,
			  List *attnamelist,
			  List *options)
{
	CopyFromState cstate;
	bool		pipe = (filename == NULL);
	TupleDesc	tupDesc;
	AttrNumber	num_phys_attrs,
				num_defaults;
	FmgrInfo   *in_functions;
	Oid		   *typioparams;
	Oid			in_func_oid;
	int		   *defmap;
	ExprState **defexprs;
	MemoryContext oldcontext;
	bool		volatile_defexprs;
	const int	progress_cols[] = {
		PROGRESS_COPY_COMMAND,
		PROGRESS_COPY_TYPE,
		PROGRESS_COPY_BYTES_TOTAL
	};
	int64		progress_vals[] = {
		PROGRESS_COPY_COMMAND_FROM,
		0,
		0
	};

	/* 分配工作空间并将所有字段归零 */
	cstate = (CopyFromStateData *) palloc0(sizeof(CopyFromStateData));

	/*
	 * 我们在新的内存上下文中分配 cstate 使用的所有内容。
	 * 这避免了在查询中重复使用 COPY 时的内存泄漏。
	 */
	cstate->copycontext = AllocSetContextCreate(CurrentMemoryContext,
												"COPY",
												ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/* 从语句节点树中提取选项 */
	ProcessCopyOptions(pstate, &cstate->opts, true /* is_from */ , options);

	/* 处理目标关系 */
	cstate->rel = rel;

	tupDesc = RelationGetDescr(cstate->rel);

	/* 处理通用选项或初始化 */

	/* 生成或转换要处理的属性列表 */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* 将 FORCE_NOT_NULL 名称列表转换为每列标志，检查有效性 */
	cstate->opts.force_notnull_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_notnull)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_notnull);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("FORCE_NOT_NULL column \"%s\" not referenced by COPY",
								NameStr(attr->attname))));
			cstate->opts.force_notnull_flags[attnum - 1] = true;
		}
	}

	/* 将 FORCE_NULL 名称列表转换为每列标志，检查有效性 */
	cstate->opts.force_null_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_null)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_null);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("FORCE_NULL column \"%s\" not referenced by COPY",
								NameStr(attr->attname))));
			cstate->opts.force_null_flags[attnum - 1] = true;
		}
	}

	/* 将 convert_selectively 名称列表转换为每列标志 */
	if (cstate->opts.convert_selectively)
	{
		List	   *attnums;
		ListCell   *cur;

		cstate->convert_select_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.convert_select);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg_internal("selected column \"%s\" not referenced by COPY",
										 NameStr(attr->attname))));
			cstate->convert_select_flags[attnum - 1] = true;
		}
	}

	/* 当未指定 ENCODING 选项时使用客户端编码。 */
	if (cstate->opts.file_encoding < 0)
		cstate->file_encoding = pg_get_client_encoding();
	else
		cstate->file_encoding = cstate->opts.file_encoding;

	/*
	 * 查找编码转换函数。
	 */
	if (cstate->file_encoding == GetDatabaseEncoding() ||
		cstate->file_encoding == PG_SQL_ASCII ||
		GetDatabaseEncoding() == PG_SQL_ASCII)
	{
		cstate->need_transcoding = false;
	}
	else
	{
		cstate->need_transcoding = true;
		cstate->conversion_proc = FindDefaultConversionProc(cstate->file_encoding,
															GetDatabaseEncoding());
		if (!OidIsValid(cstate->conversion_proc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
							pg_encoding_to_char(cstate->file_encoding),
							pg_encoding_to_char(GetDatabaseEncoding()))));
	}

	cstate->copy_src = COPY_FILE;	/* 默认 */

	cstate->whereClause = whereClause;

	/* 初始化状态变量 */
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno = 0;
	cstate->cur_attname = NULL;
	cstate->cur_attval = NULL;
	cstate->relname_only = false;

	/*
	 * 为输入管道分配缓冲区。
	 *
	 * attribute_buf 和 raw_buf 在文本和二进制模式下都使用，
	 * 但 input_buf 和 line_buf 仅在文本模式下使用。
	 */
	cstate->raw_buf = palloc(RAW_BUF_SIZE + 1);
	cstate->raw_buf_index = cstate->raw_buf_len = 0;
	cstate->raw_reached_eof = false;

	if (!cstate->opts.binary)
	{
		/*
		 * 如果需要编码转换，我们需要另一个缓冲区来
		 * 存储转换后的输入数据。否则，我们可以直接将
		 * input_buf 指向与 raw_buf 相同的缓冲区。
		 */
		if (cstate->need_transcoding)
		{
			cstate->input_buf = (char *) palloc(INPUT_BUF_SIZE + 1);
			cstate->input_buf_index = cstate->input_buf_len = 0;
		}
		else
			cstate->input_buf = cstate->raw_buf;
		cstate->input_reached_eof = false;

		initStringInfo(&cstate->line_buf);
	}

	initStringInfo(&cstate->attribute_buf);

	/* 分配范围表和 rteperminfos，我们在 CopyFrom 中会需要它们。 */
	if (pstate)
	{
		cstate->range_table = pstate->p_rtable;
		cstate->rteperminfos = pstate->p_rteperminfos;
	}

	tupDesc = RelationGetDescr(cstate->rel);
	num_phys_attrs = tupDesc->natts;
	num_defaults = 0;
	volatile_defexprs = false;

	/*
	 * 获取关系中每个属性所需的目录信息，包括输入函数、
	 * 元素类型（传递给输入函数）和默认值及约束信息。
	 * （使用哪个输入函数取决于文本/二进制格式选择。）
	 *
	 */
	in_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	for (int attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(tupDesc, attnum - 1);

		/* 我们不需要已删除属性的信息 */
		if (att->attisdropped)
			continue;

		/* 获取输入函数和 typioparam 信息 */
		if (cstate->opts.binary)
			getTypeBinaryInputInfo(att->atttypid,
								   &in_func_oid, &typioparams[attnum - 1]);
		else
			getTypeInputInfo(att->atttypid,
							 &in_func_oid, &typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &in_functions[attnum - 1]);

		/* 如果有可用的默认值信息，则获取 */
		defexprs[attnum - 1] = NULL;

		/*
		 * 我们仅需要未出现在列列表中的列的默认值，
		 * 除非给定了 DEFAULT 选项。我们永远不需要
		 * 生成列的默认值。
		 */
		if ((cstate->opts.default_print != NULL ||
			 !list_member_int(cstate->attnumlist, attnum)) &&
			!att->attgenerated)
		{
			Expr	   *defexpr = (Expr *) build_column_default(cstate->rel,
																attnum);

			if (defexpr != NULL)
			{
				/* 将表达式通过计划器运行 */
				defexpr = expression_planner(defexpr);

				/* 在 copycontext 中初始化可执行表达式 */
				defexprs[attnum - 1] = ExecInitExpr(defexpr, NULL);

				/* 如果不是从输入复制的 */
				/* 如果存在默认值，则使用 */
				if (!list_member_int(cstate->attnumlist, attnum))
				{
					defmap[num_defaults] = attnum - 1;
					num_defaults++;
				}

				/*
				 * 如果默认表达式查看正在加载的表，
				 * 则在使用多重插入时可能产生错误结果。
				 * 由于数据库访问可以是动态的，这很难精确测试，
				 * 所以我们使用更宽泛的测试：默认表达式是否是易变的。
				 * 我们允许一个特例：当默认表达式是序列的
				 * nextval() 时，在这个特定场景下已知其与多重插入
				 * 优化兼容。因此我们使用这个特例函数检查器
				 * 而不是 contain_volatile_functions() 的标准检查。
				 * 另外注意，我们已经通过 expression_planner() 运行了表达式。
				 *
				 *
				 *
				 */
				if (!volatile_defexprs)
					volatile_defexprs = contain_volatile_functions_not_nextval((Node *) defexpr);
			}
		}
	}

	cstate->defaults = (bool *) palloc0(tupDesc->natts * sizeof(bool));

	/* 初始化进度 */
	pgstat_progress_start_command(PROGRESS_COMMAND_COPY,
								  cstate->rel ? RelationGetRelid(cstate->rel) : InvalidOid);
	cstate->bytes_processed = 0;

	/* 我们将这些变量保留在 cstate 中。 */
	cstate->in_functions = in_functions;
	cstate->typioparams = typioparams;
	cstate->defmap = defmap;
	cstate->defexprs = defexprs;
	cstate->volatile_defexprs = volatile_defexprs;
	cstate->num_defaults = num_defaults;
	cstate->is_program = is_program;

	if (data_source_cb)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_CALLBACK;
		cstate->copy_src = COPY_CALLBACK;
		cstate->data_source_cb = data_source_cb;
	}
	else if (pipe)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_PIPE;
		Assert(!is_program);	/* 语法不允许这种情况 */
		if (whereToSendOutput == DestRemote)
			ReceiveCopyBegin(cstate);
		else
			cstate->copy_file = stdin;
	}
	else
	{
		cstate->filename = pstrdup(filename);

		if (cstate->is_program)
		{
			progress_vals[1] = PROGRESS_COPY_TYPE_PROGRAM;
			cstate->copy_file = OpenPipeStream(cstate->filename, PG_BINARY_R);
			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not execute command \"%s\": %m",
								cstate->filename)));
		}
		else
		{
			struct stat st;

			progress_vals[1] = PROGRESS_COPY_TYPE_FILE;
			cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_R);
			if (cstate->copy_file == NULL)
			{
				/* 复制 errno，因为 ereport 子函数可能会更改它 */
				int			save_errno = errno;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for reading: %m",
								cstate->filename),
						 (save_errno == ENOENT || save_errno == EACCES) ?
						 errhint("COPY FROM instructs the PostgreSQL server process to read a file. "
								 "You may want a client-side facility such as psql's \\copy.") : 0));
			}

			if (fstat(fileno(cstate->copy_file), &st))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								cstate->filename)));

			if (S_ISDIR(st.st_mode))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", cstate->filename)));

			progress_vals[2] = st.st_size;
		}
	}

	pgstat_progress_update_multi_param(3, progress_cols, progress_vals);

	if (cstate->opts.binary)
	{
		/* 读取并验证二进制标头 */
		ReceiveCopyBinaryHeader(cstate);
	}

	/* 为 CopyReadAttributes 结果创建工作空间 */
	if (!cstate->opts.binary)
	{
		AttrNumber	attr_count = list_length(cstate->attnumlist);

		cstate->max_fields = attr_count;
		cstate->raw_fields = (char **) palloc(attr_count * sizeof(char *));
	}

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * 清理 COPY FROM 的存储并释放资源。
 */
void
EndCopyFrom(CopyFromState cstate)
{
	/* 除内存外没有 COPY FROM 相关的资源。 */
	if (cstate->is_program)
	{
		ClosePipeFromProgram(cstate);
	}
	else
	{
		if (cstate->filename != NULL && FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m",
							cstate->filename)));
	}

	pgstat_progress_end_command();

	MemoryContextDelete(cstate->copycontext);
	pfree(cstate);
}

/*
 * 关闭来自外部程序的管道，检查 pclose() 返回码。
 */
static void
ClosePipeFromProgram(CopyFromState cstate)
{
	int			pclose_rc;

	Assert(cstate->is_program);

	pclose_rc = ClosePipeStream(cstate->copy_file);
	if (pclose_rc == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
	else if (pclose_rc != 0)
	{
		/*
		 * 如果我们在到达 EOF 之前结束了 COPY FROM PROGRAM，
		 * 则被调用的程序因 SIGPIPE 失败是可以预期的，
		 * 我们不应将其报告为错误。否则，SIGPIPE 表示有问题。
		 *
		 */
		if (!cstate->raw_reached_eof &&
			wait_result_is_signal(pclose_rc, SIGPIPE))
			return;

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("program \"%s\" failed",
						cstate->filename),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}
}
