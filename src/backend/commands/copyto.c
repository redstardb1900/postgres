/*-------------------------------------------------------------------------
 *
 * copyto.c
 *		COPY <table> TO file/program/client
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyto.c
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
#include "commands/copy.h"
#include "commands/progress.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * 表示我们在底层需要关注的不同目标场景
 *
 */
typedef enum CopyDest
{
	COPY_FILE,					/* 到文件（或管道程序） */
	COPY_FRONTEND,				/* 到前端 */
	COPY_CALLBACK				/* 到回调函数 */
} CopyDest;

/*
 * 此结构包含整个 COPY TO 操作中使用的所有状态变量。 
 * 。       
 *
 * 多字节编码：所有受支持的客户端编码都是通过设置首   
 * 个字节的高位来编码多字节字符。字符的后续字节则可   
 * 以不设置高位。当在这种编码中扫描数据以查找与单 
 * 字节（即 ASCII）字符匹配的内容时，我们必须使用完整   
 * 的 pg_encoding_mblen() 机制跳过多字节字符，否则     
 * 可能在尾随字节上找到虚假匹配。在受支持的服    
 * 务器编码中，不可能出现虚假匹配，而对尾随字节做无 
 * 用的比较要比调用 pg_encoding_mblen() 来跳过它们更快。  
 *  encoding_embeds_ascii 在我们不得不采用困难方式时为          
 *  true。                           
 */
typedef struct CopyToStateData
{
	/* 低层状态数据 */
	CopyDest	copy_dest;		/* 复制源/目标的类型 */
	FILE	   *copy_file;		/* 当 copy_dest == COPY_FILE 时使用 */
	StringInfo	fe_msgbuf;		/* 在 COPY TO 期间对所有目标使用 */

	int			file_encoding;	/* 文件或远端的字符编码 */
	bool		need_transcoding;	/* 文件编码与服务器不同？ */
	bool		encoding_embeds_ascii;	/* ASCII 可以是非首字节？ */

	/* 来自 COPY 命令的参数 */
	Relation	rel;			/* 要复制到的关系 */
	QueryDesc  *queryDesc;		/* 要复制的可执行查询 */
	List	   *attnumlist;		/* 要复制的属性号整数列表 */
	char	   *filename;		/* 文件名，或为 STDOUT 时为 NULL */
	bool		is_program;		/* 'filename' 是否为要 popen 的程序？ */
	copy_data_dest_cb data_dest_cb; /* 用于写入数据的函数 */

	CopyFormatOptions opts;
	Node	   *whereClause;	/* WHERE condition (or NULL) */

	/*
		 * 工作状态
		 */
	MemoryContext copycontext;	/* 每次复制的执行上下文 */

	FmgrInfo   *out_functions;	/* 输出函数的查找信息 */
	MemoryContext rowcontext;	/* 每行评估上下文 */
	uint64		bytes_processed;	/* 已处理的字节数 */
} CopyToStateData;

/* COPY（查询）TO 的 DestReceiver */
typedef struct
{
	DestReceiver pub;			/* 公开的函数指针 */
	CopyToState cstate;			/* 命令的 CopyToStateData */
	uint64		processed;		/* 已处理的元组数 */
} DR_copy;

/* 注意：copyfromparse.c 中有此处的副本 */
static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* 非导出函数原型 */
static void EndCopy(CopyToState cstate);
static void ClosePipeToProgram(CopyToState cstate);
static void CopyOneRowTo(CopyToState cstate, TupleTableSlot *slot);
static void CopyAttributeOutText(CopyToState cstate, const char *string);
static void CopyAttributeOutCSV(CopyToState cstate, const char *string,
								bool use_quote, bool single_attr);

/* 低层通信函数 */
static void SendCopyBegin(CopyToState cstate);
static void SendCopyEnd(CopyToState cstate);
static void CopySendData(CopyToState cstate, const void *databuf, int datasize);
static void CopySendString(CopyToState cstate, const char *str);
static void CopySendChar(CopyToState cstate, char c);
static void CopySendEndOfRow(CopyToState cstate);
static void CopySendInt32(CopyToState cstate, int32 val);
static void CopySendInt16(CopyToState cstate, int16 val);


/*
 * 发送前端复制的开始/停止消息。这些在过去的协议重设计中
 * 已发生变化。
 */
static void
SendCopyBegin(CopyToState cstate)
{
	StringInfoData buf;
	int			natts = list_length(cstate->attnumlist);
	int16		format = (cstate->opts.binary ? 1 : 0);
	int			i;

	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, format);	/* 整体格式 */
	pq_sendint16(&buf, natts);
	for (i = 0; i < natts; i++)
		pq_sendint16(&buf, format); /* 每列的格式 */
	pq_endmessage(&buf);
	cstate->copy_dest = COPY_FRONTEND;
}

static void
SendCopyEnd(CopyToState cstate)
{
	/* 不应有任何未发送的数据 */
	Assert(cstate->fe_msgbuf->len == 0);
	/* 发送 Copy Done 消息 */
	pq_putemptymessage('c');
}

/*----------
 * CopySendData 向目标（文件或前端）发送输出数据
 * CopySendString 对以 null 终止的字符串执行相同操作
 * CopySendChar 对单个字符执行相同操作
 * CopySendEndOfRow 在每个数据行结束时执行适当的操作
 *	（数据实际上仅在 CopySendEndOfRow 时才会刷新）
 *
 * 注意：这些函数不应用任何数据转换
 *----------
 */
static void
CopySendData(CopyToState cstate, const void *databuf, int datasize)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, databuf, datasize);
}

static void
CopySendString(CopyToState cstate, const char *str)
{
	appendBinaryStringInfo(cstate->fe_msgbuf, str, strlen(str));
}

static void
CopySendChar(CopyToState cstate, char c)
{
	appendStringInfoCharMacro(cstate->fe_msgbuf, c);
}

static void
CopySendEndOfRow(CopyToState cstate)
{
	StringInfo	fe_msgbuf = cstate->fe_msgbuf;

	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			if (!cstate->opts.binary)
			{
				/* 默认行终止符取决于平台 */
#ifndef WIN32
				CopySendChar(cstate, '\n');
#else
				CopySendString(cstate, "\r\n");
#endif
			}

			if (fwrite(fe_msgbuf->data, fe_msgbuf->len, 1,
					   cstate->copy_file) != 1 ||
				ferror(cstate->copy_file))
			{
				if (cstate->is_program)
				{
					if (errno == EPIPE)
					{
						/*
							 * 管道将在事务结束时自动关闭，但我们可能
							 * 从子进程的退出码中获得比简单的
							 * "Broken Pipe" 更好的错误消息
							 *
							 */
						ClosePipeToProgram(cstate);

						/*
							 * 如果 ClosePipeToProgram() 未抛出错误，则
							 * 程序正常终止，但先关闭了管道。
							 * 恢复 errno 并抛出错误。
							 */
						errno = EPIPE;
					}
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write to COPY program: %m")));
				}
				else
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write to COPY file: %m")));
			}
			break;
		case COPY_FRONTEND:
			/* FE/BE 协议在所有平台上使用 \n 作为换行符 */
			if (!cstate->opts.binary)
				CopySendChar(cstate, '\n');

			/* 将累积的行作为一条 CopyData 消息输出 */
			(void) pq_putmessage('d', fe_msgbuf->data, fe_msgbuf->len);
			break;
		case COPY_CALLBACK:
			cstate->data_dest_cb(fe_msgbuf->data, fe_msgbuf->len);
			break;
	}

	/* 更新进度 */
	cstate->bytes_processed += fe_msgbuf->len;
	pgstat_progress_update_param(PROGRESS_COPY_BYTES_PROCESSED, cstate->bytes_processed);

	resetStringInfo(fe_msgbuf);
}

/*
 * 以下函数确实应用一些数据转换
 */

/*
 * CopySendInt32 以网络字节序发送 int32
 */
static inline void
CopySendInt32(CopyToState cstate, int32 val)
{
	uint32		buf;

	buf = pg_hton32((uint32) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * CopySendInt16 以网络字节序发送 int16
 */
static inline void
CopySendInt16(CopyToState cstate, int16 val)
{
	uint16		buf;

	buf = pg_hton16((uint16) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * 关闭到外部程序的管道，检查 pclose() 返回码。
 */
static void
ClosePipeToProgram(CopyToState cstate)
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
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("program \"%s\" failed",
						cstate->filename),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}
}

/*
 * 释放 COPY TO/FROM 的 cstate 中分配的资源。
 */
static void
EndCopy(CopyToState cstate)
{
	if (cstate->is_program)
	{
		ClosePipeToProgram(cstate);
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
 * 设置 CopyToState 以从表或查询中读取元组用于 COPY TO。
 *
 * 'rel': 要复制的关系
 * 'raw_query': 其结果要复制的查询
 * 'queryRelId': 要转换为查询的基关系 OID（用于 RLS）
 * 'filename': 要写入的服务器本地文件名，NULL 表示 STDOUT
 * 'is_program': 如果 'filename' 是要执行的程序则为 true
 * 'data_dest_cb': 处理输出数据的回调
 * 'attnamelist': char * 列表，要包含的列。NIL 表示选择所有列。
 * 'options': DefElem 列表。参见 gram.y 中的 copy_opt_item 以获取选项。
 *
 * 返回 CopyToState，传递给 DoCopyTo() 及相关函数。
 */
CopyToState
BeginCopyTo(ParseState *pstate,
			Relation rel,
			RawStmt *raw_query,
			Oid queryRelId,
			const char *filename,
			bool is_program,
			copy_data_dest_cb data_dest_cb,
			List *attnamelist,
			List *options)
{
	CopyToState cstate;
	bool		pipe = (filename == NULL && data_dest_cb == NULL);
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	MemoryContext oldcontext;
	const int	progress_cols[] = {
		PROGRESS_COPY_COMMAND,
		PROGRESS_COPY_TYPE
	};
	int64		progress_vals[] = {
		PROGRESS_COPY_COMMAND_TO,
		0
	};

	if (rel != NULL && rel->rd_rel->relkind != RELKIND_RELATION)
	{
		if (rel->rd_rel->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from view \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else if (rel->rd_rel->relkind == RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from materialized view \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from foreign table \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from sequence \"%s\"",
							RelationGetRelationName(rel))));
		else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from partitioned table \"%s\"",
							RelationGetRelationName(rel)),
					 errhint("Try the COPY (SELECT ...) TO variant.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from non-table relation \"%s\"",
							RelationGetRelationName(rel))));
	}


	/* 分配工作空间并将所有字段归零 */
	cstate = (CopyToStateData *) palloc0(sizeof(CopyToStateData));

	/*
	 * 我们在新的内存上下文中分配 cstate 使用的所有内容。
	 * 这避免了在查询中重复使用 COPY 时的内存泄漏。
	 */
	cstate->copycontext = AllocSetContextCreate(CurrentMemoryContext,
												"COPY",
												ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(cstate->copycontext);

	/* 从语句节点树中提取选项 */
	ProcessCopyOptions(pstate, &cstate->opts, false /* is_from */ , options);

	/* 处理源/目标关系或查询 */
	if (rel)
	{
		Assert(!raw_query);

		cstate->rel = rel;

		tupDesc = RelationGetDescr(cstate->rel);
	}
	else
	{
		List	   *rewritten;
		Query	   *query;
		PlannedStmt *plan;
		DestReceiver *dest;

		cstate->rel = NULL;

		/*
			 * 运行解析分析和重写。注意，这也会获取源表上
			 * 足够的锁。
			 */
		rewritten = pg_analyze_and_rewrite_fixedparams(raw_query,
													   pstate->p_sourcetext, NULL, 0,
													   NULL);

		/* 检查我们是否得到了可用的结果 */
		if (rewritten == NIL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DO INSTEAD NOTHING rules are not supported for COPY")));
		}
		else if (list_length(rewritten) > 1)
		{
			ListCell   *lc;

			/* examine queries to determine which error message to issue */
			foreach(lc, rewritten)
			{
				Query	   *q = lfirst_node(Query, lc);

				if (q->querySource == QSRC_QUAL_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional DO INSTEAD rules are not supported for COPY")));
				if (q->querySource == QSRC_NON_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("DO ALSO rules are not supported for COPY")));
			}

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multi-statement DO INSTEAD rules are not supported for COPY")));
		}

		query = linitial_node(Query, rewritten);

		/* The grammar allows SELECT INTO, but we don't support that */
		if (query->utilityStmt != NULL &&
			IsA(query->utilityStmt, CreateTableAsStmt))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT INTO) is not supported")));

		/* The only other utility command we could see is NOTIFY */
		if (query->utilityStmt != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY query must not be a utility command")));

		/*
			 * 同样，语法不强制要求 RETURNING 子句的存在，
			 * 但这里必须要求。
			 */
		if (query->commandType != CMD_SELECT &&
			query->returningList == NIL)
		{
			Assert(query->commandType == CMD_INSERT ||
				   query->commandType == CMD_UPDATE ||
				   query->commandType == CMD_DELETE);

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY query must have a RETURNING clause")));
		}

		/* 计划查询 */
		plan = pg_plan_query(query, pstate->p_sourcetext,
							 CURSOR_OPT_PARALLEL_OK, NULL);

		/*
			 * 当使用行级安全且用户使用 "COPY relation TO" 时，
			 * 我们必须将 "COPY relation TO" 转换为基于查询的 COPY
			 * （例如 "COPY (SELECT * FROM ONLY relation) TO"），以允许
			 * 重写器添加 RLS 子句。
			 *
			 * 当发生此情况时，传入最初找到的关系 relid
			 * （我们已经锁定它）。由于计划器会再次查找关系，
			 * 我们在这里双重检查以确保它找到的与我们已锁定的相同。
			 *
			 */
		if (queryRelId != InvalidOid)
		{
			/*
				 * 注意，RLS 涉及时可能有多个关系，虽然我们
				 * 需要的那个几乎肯定在第一个，但计划器不
				 * 保证这一点，所以检查整个列表确保找到
				 * 原始关系。
				 */
			if (!list_member_oid(plan->relationOids, queryRelId))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("relation referenced by COPY statement has changed")));
		}

		/*
			 * 使用带更新的命令 ID 的快照，以确保此查询能看到
			 * 之前执行的任何查询的结果。
			 */
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();

		/* 为 COPY OUT 创建目标接收器 */
		dest = CreateDestReceiver(DestCopyOut);
		((DR_copy *) dest)->cstate = cstate;

		/* 创建一个不要求输出的 QueryDesc */
		cstate->queryDesc = CreateQueryDesc(plan, pstate->p_sourcetext,
											GetActiveSnapshot(),
											InvalidSnapshot,
											dest, NULL, NULL, 0);

		/*
			 * 调用 ExecutorStart 以准备计划执行。
			 *
			 * ExecutorStart 为我们计算结果 tupdesc
			 */
		ExecutorStart(cstate->queryDesc, 0);

		tupDesc = cstate->queryDesc->tupDesc;
	}

	/* 生成或转换要处理的属性列表 */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);

	num_phys_attrs = tupDesc->natts;

	/* 将 FORCE_QUOTE 名称列表转换为每列标志，检查有效性 */
	cstate->opts.force_quote_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (cstate->opts.force_quote_all)
	{
		int			i;

		for (i = 0; i < num_phys_attrs; i++)
			cstate->opts.force_quote_flags[i] = true;
	}
	else if (cstate->opts.force_quote)
	{
		List	   *attnums;
		ListCell   *cur;

		attnums = CopyGetAttnums(tupDesc, cstate->rel, cstate->opts.force_quote);

		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("FORCE_QUOTE column \"%s\" not referenced by COPY",
								NameStr(attr->attname))));
			cstate->opts.force_quote_flags[attnum - 1] = true;
		}
	}

	/* 当未指定 ENCODING 选项时使用客户端编码。 */
	if (cstate->opts.file_encoding < 0)
		cstate->file_encoding = pg_get_client_encoding();
	else
		cstate->file_encoding = cstate->opts.file_encoding;

	/*
	 * 设置编码转换信息。即使文件和服务器编码相同，
	 * 我们也必须应用 pg_any_to_server() 来验证多字节编码中的数据。
	 *
	 */
	cstate->need_transcoding =
		(cstate->file_encoding != GetDatabaseEncoding() ||
		 pg_database_encoding_max_length() > 1);
	/* 参见上方的多字节编码注释 */
	cstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(cstate->file_encoding);

	cstate->copy_dest = COPY_FILE;	/* 默认 */

	if (data_dest_cb)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_CALLBACK;
		cstate->copy_dest = COPY_CALLBACK;
		cstate->data_dest_cb = data_dest_cb;
	}
	else if (pipe)
	{
		progress_vals[1] = PROGRESS_COPY_TYPE_PIPE;

		Assert(!is_program);	/* 语法不允许这种情况 */
		if (whereToSendOutput != DestRemote)
			cstate->copy_file = stdout;
	}
	else
	{
		cstate->filename = pstrdup(filename);
		cstate->is_program = is_program;

		if (is_program)
		{
			progress_vals[1] = PROGRESS_COPY_TYPE_PROGRAM;
			cstate->copy_file = OpenPipeStream(cstate->filename, PG_BINARY_W);
			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not execute command \"%s\": %m",
								cstate->filename)));
		}
		else
		{
			mode_t		oumask; /* 现有的 umask 值 */
			struct stat st;

			progress_vals[1] = PROGRESS_COPY_TYPE_FILE;

			/*
				 * 防止写入相对路径 ... 太容易因覆盖数据库文件
				 * 而自寻烦恼 ...
				 */
			if (!is_absolute_path(filename))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("relative path not allowed for COPY to file")));

			oumask = umask(S_IWGRP | S_IWOTH);
			PG_TRY();
			{
				cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_W);
			}
			PG_FINALLY();
			{
				umask(oumask);
			}
			PG_END_TRY();
			if (cstate->copy_file == NULL)
			{
				/* 复制 errno，因为 ereport 子函数可能会更改它 */
				int			save_errno = errno;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for writing: %m",
								cstate->filename),
						 (save_errno == ENOENT || save_errno == EACCES) ?
						 errhint("COPY TO instructs the PostgreSQL server process to write a file. "
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
		}
	}

	/* 初始化进度 */
	pgstat_progress_start_command(PROGRESS_COMMAND_COPY,
								  cstate->rel ? RelationGetRelid(cstate->rel) : InvalidOid);
	pgstat_progress_update_multi_param(2, progress_cols, progress_vals);

	cstate->bytes_processed = 0;

	MemoryContextSwitchTo(oldcontext);

	return cstate;
}

/*
 * 清理 COPY TO 的存储并释放资源。
 */
void
EndCopyTo(CopyToState cstate)
{
	if (cstate->queryDesc != NULL)
	{
		/* 关闭查询并释放资源。 */
		ExecutorFinish(cstate->queryDesc);
		ExecutorEnd(cstate->queryDesc);
		FreeQueryDesc(cstate->queryDesc);
		PopActiveSnapshot();
	}

	/* 清理存储 */
	EndCopy(cstate);
}

/*
 * 从关系或查询复制到文件。
 *
 * 返回已处理的行数。
 */
uint64
DoCopyTo(CopyToState cstate)
{
	bool		pipe = (cstate->filename == NULL && cstate->data_dest_cb == NULL);
	bool		fe_copy = (pipe && whereToSendOutput == DestRemote);
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	ListCell   *cur;
	uint64		processed;

	if (fe_copy)
		SendCopyBegin(cstate);

	if (cstate->rel)
		tupDesc = RelationGetDescr(cstate->rel);
	else
		tupDesc = cstate->queryDesc->tupDesc;
	num_phys_attrs = tupDesc->natts;
	cstate->opts.null_print_client = cstate->opts.null_print;	/* 默认 */

	/* 无论 copy_dest 为何，我们使用 fe_msgbuf 作为每行缓冲区 */
	cstate->fe_msgbuf = makeStringInfo();

	/* 获取我们需要处理的列的信息。 */
	cstate->out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Oid			out_func_oid;
		bool		isvarlena;
		Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

		if (cstate->opts.binary)
			getTypeBinaryOutputInfo(attr->atttypid,
									&out_func_oid,
									&isvarlena);
		else
			getTypeOutputInfo(attr->atttypid,
							  &out_func_oid,
							  &isvarlena);
		fmgr_info(out_func_oid, &cstate->out_functions[attnum - 1]);
	}

	/*
	 * 创建一个临时内存上下文，每行可以重置以回收 palloc 的内存。
	 * 这避免了数据类型输出例程内部的任何泄漏问题，且无论如何
	 * 应该比零售的 pfree 更快。（我们不需要像 CopyFrom 那样的
	 * 完整 econtext。）
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "COPY TO",
											   ALLOCSET_DEFAULT_SIZES);

	if (cstate->opts.binary)
	{
		/* 生成二进制复制的标头 */
		int32		tmp;

		/* 签名 */
		CopySendData(cstate, BinarySignature, 11);
		/* 标志字段 */
		tmp = 0;
		CopySendInt32(cstate, tmp);
		/* 无标头扩展 */
		tmp = 0;
		CopySendInt32(cstate, tmp);
	}
	else
	{
		/*
			 * 对于非二进制复制，我们需要将 null_print
			 * 转换为文件编码，因为它将通过 CopySendString 直接发送。
			 */
		if (cstate->need_transcoding)
			cstate->opts.null_print_client = pg_server_to_any(cstate->opts.null_print,
															  cstate->opts.null_print_len,
															  cstate->file_encoding);

		/* 如果已请求标头，则发送该行 */
		if (cstate->opts.header_line)
		{
			bool		hdr_delim = false;

			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				char	   *colname;

				if (hdr_delim)
					CopySendChar(cstate, cstate->opts.delim[0]);
				hdr_delim = true;

				colname = NameStr(TupleDescAttr(tupDesc, attnum - 1)->attname);

				if (cstate->opts.csv_mode)
					CopyAttributeOutCSV(cstate, colname, false,
										list_length(cstate->attnumlist) == 1);
				else
					CopyAttributeOutText(cstate, colname);
			}

			CopySendEndOfRow(cstate);
		}
	}

	if (cstate->rel)
	{
		TupleTableSlot *slot;
		TableScanDesc scandesc;

		scandesc = table_beginscan(cstate->rel, GetActiveSnapshot(), 0, NULL);
		slot = table_slot_create(cstate->rel, NULL);

		processed = 0;
		while (table_scan_getnextslot(scandesc, ForwardScanDirection, slot))
		{
			CHECK_FOR_INTERRUPTS();

			/* 解构元组 ... */
			slot_getallattrs(slot);

			/* 格式化并发送数据 */
			CopyOneRowTo(cstate, slot);

			/*
			 * 递增已处理的元组数，并报告进度。
			 *
			 */
			pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
										 ++processed);
		}

		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scandesc);
	}
	else
	{
		/* 运行计划 --- 目标接收器将发送元组 */
		ExecutorRun(cstate->queryDesc, ForwardScanDirection, 0, true);
		processed = ((DR_copy *) cstate->queryDesc->dest)->processed;
	}

	if (cstate->opts.binary)
	{
		/* 生成二进制复制的尾部 */
		CopySendInt16(cstate, -1);
		/* 需要刷新尾部数据 */
		CopySendEndOfRow(cstate);
	}

	MemoryContextDelete(cstate->rowcontext);

	if (fe_copy)
		SendCopyEnd(cstate);

	return processed;
}

/*
 * 在 DoCopyTo() 中发送一行。
 */
static void
CopyOneRowTo(CopyToState cstate, TupleTableSlot *slot)
{
	bool		need_delim = false;
	FmgrInfo   *out_functions = cstate->out_functions;
	MemoryContext oldcontext;
	ListCell   *cur;
	char	   *string;

	MemoryContextReset(cstate->rowcontext);
	oldcontext = MemoryContextSwitchTo(cstate->rowcontext);

	if (cstate->opts.binary)
	{
		/* 二进制每元组标头 */
		CopySendInt16(cstate, list_length(cstate->attnumlist));
	}

	/* 确保元组已完全解构 */
	slot_getallattrs(slot);

	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Datum		value = slot->tts_values[attnum - 1];
		bool		isnull = slot->tts_isnull[attnum - 1];

		if (!cstate->opts.binary)
		{
			if (need_delim)
				CopySendChar(cstate, cstate->opts.delim[0]);
			need_delim = true;
		}

		if (isnull)
		{
			if (!cstate->opts.binary)
				CopySendString(cstate, cstate->opts.null_print_client);
			else
				CopySendInt32(cstate, -1);
		}
		else
		{
			if (!cstate->opts.binary)
			{
				string = OutputFunctionCall(&out_functions[attnum - 1],
											value);
				if (cstate->opts.csv_mode)
					CopyAttributeOutCSV(cstate, string,
										cstate->opts.force_quote_flags[attnum - 1],
										list_length(cstate->attnumlist) == 1);
				else
					CopyAttributeOutText(cstate, string);
			}
			else
			{
				bytea	   *outputbytes;

				outputbytes = SendFunctionCall(&out_functions[attnum - 1],
											   value);
				CopySendInt32(cstate, VARSIZE(outputbytes) - VARHDRSZ);
				CopySendData(cstate, VARDATA(outputbytes),
							 VARSIZE(outputbytes) - VARHDRSZ);
			}
		}
	}

	CopySendEndOfRow(cstate);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 发送一个属性的文本表示，含转换和转义
 */
#define DUMPSOFAR() \
	do { \
		if (ptr > start) \
			CopySendData(cstate, start, ptr - start); \
	} while (0)

static void
CopyAttributeOutText(CopyToState cstate, const char *string)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = cstate->opts.delim[0];

	if (cstate->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), cstate->file_encoding);
	else
		ptr = string;

	/*
	 * 我们必须遍历字符串搜索控制字符和分隔符字符的实例。
	 * 但在大多数情况下，这些情况并不频繁。为了避免每次字符调用
	 * CopySendData 的开销，我们在一次调用中输出转义字符之间的所有
	 * 字符。循环不变式是从 "start" 到 "ptr" 的数据可以原样发送，
	 * 但尚未发送。
	 *
	 * 当编码是安全的时，我们可以跳过 pg_encoding_mblen() 的开销，
	 * 因为在有效的后端编码中，多字节字符的额外字节永远不会
	 * 看起来像 ASCII。此循环对性能非常关键，值得为它制作两份副本
	 * 以便将 IS_HIGHBIT_SET() 测试从正常安全编码路径中移除。
	 *
	 *
	 */
	if (cstate->encoding_embeds_ascii)
	{
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if ((unsigned char) c < (unsigned char) 0x20)
			{
				/*
					 * \r 和 \n 必须转义，其他属于传统做法。我们
					 * 更喜欢使用类 C 的记法输出这些字符，而不是
					 * 反斜杠加原始字符，因为这可以使转储文件
					 * 更能抵御 Microsoft 风格的数据破坏。
					 *
					 */
				switch (c)
				{
					case '\b':
						c = 'b';
						break;
					case '\f':
						c = 'f';
						break;
					case '\n':
						c = 'n';
						break;
					case '\r':
						c = 'r';
						break;
					case '\t':
						c = 't';
						break;
					case '\v':
						c = 'v';
						break;
					default:
						/* 如果是分隔符，必须加反斜杠 */
						if (c == delimc)
							break;
						/* 所有 ASCII 控制字符长度为 1 */
						ptr++;
						continue;	/* 转到循环末尾 */
				}
				/* 如果到了这里，我们需要转换控制字符 */
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				CopySendChar(cstate, c);
				start = ++ptr;	/* 不要在下一轮中包含此字符 */
			}
			else if (c == '\\' || c == delimc)
			{
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				start = ptr++;	/* 我们在下一轮中包含此字符 */
			}
			else if (IS_HIGHBIT_SET(c))
				ptr += pg_encoding_mblen(cstate->file_encoding, ptr);
			else
				ptr++;
		}
	}
	else
	{
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if ((unsigned char) c < (unsigned char) 0x20)
			{
				/*
					 * \r 和 \n 必须转义，其他属于传统做法。我们
					 * 更喜欢使用类 C 的记法输出这些字符，而不是
					 * 反斜杠加原始字符，因为这可以使转储文件
					 * 更能抵御 Microsoft 风格的数据破坏。
					 *
					 */
				switch (c)
				{
					case '\b':
						c = 'b';
						break;
					case '\f':
						c = 'f';
						break;
					case '\n':
						c = 'n';
						break;
					case '\r':
						c = 'r';
						break;
					case '\t':
						c = 't';
						break;
					case '\v':
						c = 'v';
						break;
					default:
						/* 如果是分隔符，必须加反斜杠 */
						if (c == delimc)
							break;
						/* 所有 ASCII 控制字符长度为 1 */
						ptr++;
						continue;	/* 转到循环末尾 */
				}
				/* 如果到了这里，我们需要转换控制字符 */
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				CopySendChar(cstate, c);
				start = ++ptr;	/* 不要在下一轮中包含此字符 */
			}
			else if (c == '\\' || c == delimc)
			{
				DUMPSOFAR();
				CopySendChar(cstate, '\\');
				start = ptr++;	/* 我们在下一轮中包含此字符 */
			}
			else
				ptr++;
		}
	}

	DUMPSOFAR();
}

/*
 * 发送一个属性的文本表示，含转换和
 * CSV 风格的转义
 */
static void
CopyAttributeOutCSV(CopyToState cstate, const char *string,
					bool use_quote, bool single_attr)
{
	const char *ptr;
	const char *start;
	char		c;
	char		delimc = cstate->opts.delim[0];
	char		quotec = cstate->opts.quote[0];
	char		escapec = cstate->opts.escape[0];

	/* 如果匹配 null_print，则强制引用（在转换之前！） */
	if (!use_quote && strcmp(string, cstate->opts.null_print) == 0)
		use_quote = true;

	if (cstate->need_transcoding)
		ptr = pg_server_to_any(string, strlen(string), cstate->file_encoding);
	else
		ptr = string;

	/*
	 * 进行初步扫描以确定是否需要引用
	 */
	if (!use_quote)
	{
		/*
		 * 因为 '\.' 可以是数据值，如果它单独出现在
		 * 一行上，则引用它以避免被解释为数据结束标记。
		 */
		if (single_attr && strcmp(ptr, "\\.") == 0)
			use_quote = true;
		else
		{
			const char *tptr = ptr;

			while ((c = *tptr) != '\0')
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
					tptr += pg_encoding_mblen(cstate->file_encoding, tptr);
				else
					tptr++;
			}
		}
	}

	if (use_quote)
	{
		CopySendChar(cstate, quotec);

		/*
		 * 我们采用与 CopyAttributeOutText 相同的优化策略
		 */
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if (c == quotec || c == escapec)
			{
				DUMPSOFAR();
				CopySendChar(cstate, escapec);
				start = ptr;	/* 我们在下一轮中包含此字符 */
			}
			if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
				ptr += pg_encoding_mblen(cstate->file_encoding, ptr);
			else
				ptr++;
		}
		DUMPSOFAR();

		CopySendChar(cstate, quotec);
	}
	else
	{
		/* 如果不需要引用，我们可以直接原样输出 */
		CopySendString(cstate, ptr);
	}
}

/*
 * copy_dest_startup --- 执行器启动
 */
static void
copy_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* 无操作 */
}

/*
 * copy_dest_receive --- 接收一个元组
 */
static bool
copy_dest_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_copy    *myState = (DR_copy *) self;
	CopyToState cstate = myState->cstate;

	/* 发送数据 */
	CopyOneRowTo(cstate, slot);

	/* 递增已处理的元组数，并报告进度 */
	pgstat_progress_update_param(PROGRESS_COPY_TUPLES_PROCESSED,
								 ++myState->processed);

	return true;
}

/*
 * copy_dest_shutdown --- 执行器结束
 */
static void
copy_dest_shutdown(DestReceiver *self)
{
	/* 无操作 */
}

/*
 * copy_dest_destroy --- 释放 DestReceiver 对象
 */
static void
copy_dest_destroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * CreateCopyDestReceiver -- 创建合适的 DestReceiver 对象
 */
DestReceiver *
CreateCopyDestReceiver(void)
{
	DR_copy    *self = (DR_copy *) palloc(sizeof(DR_copy));

	self->pub.receiveSlot = copy_dest_receive;
	self->pub.rStartup = copy_dest_startup;
	self->pub.rShutdown = copy_dest_shutdown;
	self->pub.rDestroy = copy_dest_destroy;
	self->pub.mydest = DestCopyOut;

	self->cstate = NULL;		/* 将在之后设置 */
	self->processed = 0;

	return (DestReceiver *) self;
}
