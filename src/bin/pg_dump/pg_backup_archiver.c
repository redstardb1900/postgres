/*-------------------------------------------------------------------------
 *
 * pg_backup_archiver.c
 *
 *	Private implementation of the archiver routines.
 *
 *	See the headers to pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *	Rights are granted to use this software in any way so long
 *	as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_archiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef WIN32
#include <io.h>
#endif

#include "common/string.h"
#include "compress_io.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-fs.h"
#include "parallel.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"

#define TEXT_DUMP_HEADER "--\n-- PostgreSQL database dump\n--\n\n"
#define TEXT_DUMPALL_HEADER "--\n-- PostgreSQL database cluster dump\n--\n\n"

/*
 * 跟踪并行恢复期间可处理的 TocEntry 的状态。
 * （过去这是一个列表，我们现在仍这样称呼它，但现在
 * 它实际上是一个数组，以便可以对其应用 qsort。）
 *
 * tes[] 的大小足够大，不会溢出。
 * 有效条目索引范围为 first_te 到 last_te（含）。
 * 我们定期对数组排序，将 dataLength 较大的条目
 * 放到前面；如果已知有效条目已排序，则 "sorted" 为 true。
 */
typedef struct _parallelReadyList
{
	TocEntry  **tes;			/* 准备好导出的 TocEntry */
	int			first_te;		/* tes[] 中第一个有效条目的索引 */
	int			last_te;		/* tes[] 中最后一个有效条目的索引 */
	bool		sorted;			/* 有效条目当前是否已排序？ */
} ParallelReadyList;


static ArchiveHandle *_allocAH(const char *FileSpec, const ArchiveFormat fmt,
							   const pg_compress_specification compression_spec,
							   bool dosync, ArchiveMode mode,
							   SetupWorkerPtrType setupWorkerPtr);
static void _getObjectDescription(PQExpBuffer buf, const TocEntry *te);
static void _printTocEntry(ArchiveHandle *AH, TocEntry *te, bool isData);
static void _doSetFixedOutputState(ArchiveHandle *AH);
static void _doSetSessionAuth(ArchiveHandle *AH, const char *user);
static void _reconnectToDB(ArchiveHandle *AH, const char *dbname);
static void _becomeUser(ArchiveHandle *AH, const char *user);
static void _becomeOwner(ArchiveHandle *AH, TocEntry *te);
static void _selectOutputSchema(ArchiveHandle *AH, const char *schemaName);
static void _selectTablespace(ArchiveHandle *AH, const char *tablespace);
static void _selectTableAccessMethod(ArchiveHandle *AH, const char *tableam);
static void processEncodingEntry(ArchiveHandle *AH, TocEntry *te);
static void processStdStringsEntry(ArchiveHandle *AH, TocEntry *te);
static void processSearchPathEntry(ArchiveHandle *AH, TocEntry *te);
static int	_tocEntryRequired(TocEntry *te, teSection curSection, ArchiveHandle *AH);
static RestorePass _tocEntryRestorePass(TocEntry *te);
static bool _tocEntryIsACL(TocEntry *te);
static void _disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te);
static void _enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te);
static bool is_load_via_partition_root(TocEntry *te);
static void buildTocEntryArrays(ArchiveHandle *AH);
static void _moveBefore(TocEntry *pos, TocEntry *te);
static int	_discoverArchiveFormat(ArchiveHandle *AH);

static int	RestoringToDB(ArchiveHandle *AH);
static void dump_lo_buf(ArchiveHandle *AH);
static void dumpTimestamp(ArchiveHandle *AH, const char *msg, time_t tim);
static void SetOutput(ArchiveHandle *AH, const char *filename,
					  const pg_compress_specification compression_spec);
static CompressFileHandle *SaveOutput(ArchiveHandle *AH);
static void RestoreOutput(ArchiveHandle *AH, CompressFileHandle *savedOutput);

static int	restore_toc_entry(ArchiveHandle *AH, TocEntry *te, bool is_parallel);
static void restore_toc_entries_prefork(ArchiveHandle *AH,
										TocEntry *pending_list);
static void restore_toc_entries_parallel(ArchiveHandle *AH,
										 ParallelState *pstate,
										 TocEntry *pending_list);
static void restore_toc_entries_postfork(ArchiveHandle *AH,
										 TocEntry *pending_list);
static void pending_list_header_init(TocEntry *l);
static void pending_list_append(TocEntry *l, TocEntry *te);
static void pending_list_remove(TocEntry *te);
static void ready_list_init(ParallelReadyList *ready_list, int tocCount);
static void ready_list_free(ParallelReadyList *ready_list);
static void ready_list_insert(ParallelReadyList *ready_list, TocEntry *te);
static void ready_list_remove(ParallelReadyList *ready_list, int i);
static void ready_list_sort(ParallelReadyList *ready_list);
static int	TocEntrySizeCompare(const void *p1, const void *p2);
static void move_to_ready_list(TocEntry *pending_list,
							   ParallelReadyList *ready_list,
							   RestorePass pass);
static TocEntry *pop_next_work_item(ParallelReadyList *ready_list,
									ParallelState *pstate);
static void mark_dump_job_done(ArchiveHandle *AH,
							   TocEntry *te,
							   int status,
							   void *callback_data);
static void mark_restore_job_done(ArchiveHandle *AH,
								  TocEntry *te,
								  int status,
								  void *callback_data);
static void fix_dependencies(ArchiveHandle *AH);
static bool has_lock_conflicts(TocEntry *te1, TocEntry *te2);
static void repoint_table_dependencies(ArchiveHandle *AH);
static void identify_locking_dependencies(ArchiveHandle *AH, TocEntry *te);
static void reduce_dependencies(ArchiveHandle *AH, TocEntry *te,
								ParallelReadyList *ready_list);
static void mark_create_done(ArchiveHandle *AH, TocEntry *te);
static void inhibit_data_for_failed_table(ArchiveHandle *AH, TocEntry *te);

static void StrictNamesCheck(RestoreOptions *ropt);


/*
 * 分配一个新的 DumpOptions 块，包含所有默认值。
 */
DumpOptions *
NewDumpOptions(void)
{
	DumpOptions *opts = (DumpOptions *) pg_malloc(sizeof(DumpOptions));

	InitDumpOptions(opts);
	return opts;
}

/*
 * 将 DumpOptions 结构体初始化为所有默认值
 */
void
InitDumpOptions(DumpOptions *opts)
{
	memset(opts, 0, sizeof(DumpOptions));
	/* 设置不应默认为零的字段 */
	opts->include_everything = true;
	opts->cparams.promptPassword = TRI_DEFAULT;
	opts->dumpSections = DUMP_UNSECTIONED;
}

/*
 * 创建一个新分配的 DumpOptions，其选项等同于
 * 给定 RestoreOptions 中找到的选项。
 */
DumpOptions *
dumpOptionsFromRestoreOptions(RestoreOptions *ropt)
{
	DumpOptions *dopt = NewDumpOptions();

	/* 这是 pg_dump.c 的 main() 末尾所做操作的反操作 */
	dopt->cparams.dbname = ropt->cparams.dbname ? pg_strdup(ropt->cparams.dbname) : NULL;
	dopt->cparams.pgport = ropt->cparams.pgport ? pg_strdup(ropt->cparams.pgport) : NULL;
	dopt->cparams.pghost = ropt->cparams.pghost ? pg_strdup(ropt->cparams.pghost) : NULL;
	dopt->cparams.username = ropt->cparams.username ? pg_strdup(ropt->cparams.username) : NULL;
	dopt->cparams.promptPassword = ropt->cparams.promptPassword;
	dopt->outputClean = ropt->dropSchema;
	dopt->dataOnly = ropt->dataOnly;
	dopt->schemaOnly = ropt->schemaOnly;
	dopt->if_exists = ropt->if_exists;
	dopt->column_inserts = ropt->column_inserts;
	dopt->dumpSections = ropt->dumpSections;
	dopt->aclsSkip = ropt->aclsSkip;
	dopt->outputSuperuser = ropt->superuser;
	dopt->outputCreateDB = ropt->createDB;
	dopt->outputNoOwner = ropt->noOwner;
	dopt->outputNoTableAm = ropt->noTableAm;
	dopt->outputNoTablespaces = ropt->noTablespace;
	dopt->disable_triggers = ropt->disable_triggers;
	dopt->use_setsessauth = ropt->use_setsessauth;
	dopt->disable_dollar_quoting = ropt->disable_dollar_quoting;
	dopt->dump_inserts = ropt->dump_inserts;
	dopt->no_comments = ropt->no_comments;
	dopt->no_publications = ropt->no_publications;
	dopt->no_security_labels = ropt->no_security_labels;
	dopt->no_subscriptions = ropt->no_subscriptions;
	dopt->lockWaitTimeout = ropt->lockWaitTimeout;
	dopt->include_everything = ropt->include_everything;
	dopt->enable_row_security = ropt->enable_row_security;
	dopt->sequence_data = ropt->sequence_data;
	dopt->restrict_key = ropt->restrict_key ? pg_strdup(ropt->restrict_key) : NULL;

	return dopt;
}


/*
 *	包装函数。
 *
 *	目标是使编写新格式和导出器尽可能简单，
 *	必要时以额外的函数调用等为代价。
 *
 */

/*
 * 导出工作进程的设置需要大量 pg_dump 内部知识，
 * 因此它在 pg_dump.c 中定义并传入 OpenArchive。恢复工作进程
 * 的设置不需要了解太多，因此在此定义。
 */
static void
setupRestoreWorker(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	AH->ReopenPtr(AH);
}


/* 创建新归档 */
/* 公共接口 */
Archive *
CreateArchive(const char *FileSpec, const ArchiveFormat fmt,
			  const pg_compress_specification compression_spec,
			  bool dosync, ArchiveMode mode,
			  SetupWorkerPtrType setupDumpWorker)

{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, compression_spec,
								 dosync, mode, setupDumpWorker);

	return (Archive *) AH;
}

/* 打开现有归档 */
/* 公共接口 */
Archive *
OpenArchive(const char *FileSpec, const ArchiveFormat fmt)
{
	ArchiveHandle *AH;
	pg_compress_specification compression_spec = {0};

	compression_spec.algorithm = PG_COMPRESSION_NONE;
	AH = _allocAH(FileSpec, fmt, compression_spec, true,
				  archModeRead, setupRestoreWorker);

	return (Archive *) AH;
}

/* 公共接口 */
void
CloseArchive(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	AH->ClosePtr(AH);

	/* 关闭输出 */
	errno = 0;
	if (!EndCompressFileHandle(AH->OF))
		pg_fatal("could not close output file: %m");
}

/* 公共接口 */
void
SetArchiveOptions(Archive *AH, DumpOptions *dopt, RestoreOptions *ropt)
{
	/* 调用者可以省略导出选项，此时我们会合成它们 */
	if (dopt == NULL && ropt != NULL)
		dopt = dumpOptionsFromRestoreOptions(ropt);

	/* 保存选项供后续访问 */
	AH->dopt = dopt;
	AH->ropt = ropt;
}

/* 公共接口 */
void
ProcessArchiveRestoreOptions(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	TocEntry   *te;
	teSection	curSection;

	/* 决定哪些 TOC 条目将被导出/恢复，并标记它们 */
	curSection = SECTION_PRE_DATA;
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		/*
			 * 写入归档时，我们也借此机会检查
			 * 生成的条目顺序是否合理且遵循
			 * 节划分。读取时不报错，因为有缺陷的
			 * 旧版 pg_dump 可能生成乱序的归档。
			 */
		if (AH->mode != archModeRead)
		{
			switch (te->section)
			{
				case SECTION_NONE:
					/* 可以出现在任何位置 */
					break;
				case SECTION_PRE_DATA:
					if (curSection != SECTION_PRE_DATA)
						pg_log_warning("archive items not in correct section order");
					break;
				case SECTION_DATA:
					if (curSection == SECTION_POST_DATA)
						pg_log_warning("archive items not in correct section order");
					break;
				case SECTION_POST_DATA:
					/* 无论我们在哪个节都可以 */
					break;
				default:
					pg_fatal("unexpected section code %d",
							 (int) te->section);
					break;
			}
		}

		if (te->section != SECTION_NONE)
			curSection = te->section;

		te->reqs = _tocEntryRequired(te, curSection, AH);
	}

	/* 执行严格名称检查 */
	if (ropt->strict_names)
		StrictNamesCheck(ropt);
}

/* 公共接口 */
void
RestoreArchive(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	bool		parallel_mode;
	TocEntry   *te;
	CompressFileHandle *sav;

	AH->stage = STAGE_INITIALIZING;

	/*
	 * 如果要进行并行恢复，则有一些限制。
	 */
	parallel_mode = (AH->public.numWorkers > 1 && ropt->useDB);
	if (parallel_mode)
	{
		/* 我们尚未使此功能适用于所有归档格式 */
		if (AH->ClonePtr == NULL || AH->ReopenPtr == NULL)
			pg_fatal("parallel restore is not supported with this archive file format");

		/* 如果归档以 OID 表示依赖关系，则无法使用 */
		if (AH->version < K_VERS_1_8)
			pg_fatal("parallel restore is not supported with archives made by pre-8.0 pg_dump");

		/*
		 * 如果无法重新打开输入文件，同样无法使用，
		 * 所以立即尝试。
		 */
		AH->ReopenPtr(AH);
	}

	/*
	 * 确保我们不需要未获得的（解）压缩支持
	 */
	if (AH->PrintTocDataPtr != NULL)
	{
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if (te->hadDumper && (te->reqs & REQ_DATA) != 0)
			{
				char	   *errmsg = supports_compression(AH->compression_spec);

				if (errmsg)
					pg_fatal("cannot restore from compressed archive (%s)",
							 errmsg);
				else
					break;
			}
		}
	}

	/*
	 * 准备索引数组，以便在整个恢复过程中都可以使用它们。
	 * 不过可能已经做过了。
	 */
	if (AH->tocsByDumpId == NULL)
		buildTocEntryArrays(AH);

	/*
	 * 如果使用数据库连接，则进行连接。
	 */
	if (ropt->useDB)
	{
		pg_log_info("connecting to database for restore");
		if (AH->version < K_VERS_1_3)
			pg_fatal("direct database connections are not supported in pre-1.3 archives");

		/*
			 * 我们不想猜测导出是否会成功恢复；
			 * 无论恢复目标的版本如何，都允许尝试。
			 *
			 */
		AHX->minRemoteVersion = 0;
		AHX->maxRemoteVersion = 9999999;

		ConnectDatabase(AHX, &ropt->cparams, false);

		/*
			 * 如果直接与数据库通信，不发送注释，因为
			 * 它们在显示错误时会混淆 SQL
			 */
		AH->noTocComments = 1;
	}

	/*
	 * 判断是否存在隐式的仅数据恢复。如果导出仅包含数据，
	 * 或用户使用目录列表排除了所有模式数据，
	 * 就会出现这种情况。我们只查找模式条目 — 如果找不到，
	 * 则设置 dataOnly 标志。
	 *
	 * 我们可以扫描所需的 TABLE 条目，但这与 dataOnly 不同。
	 * 目前来看似乎没有必要（2001-03-06）。
	 */
	if (!ropt->dataOnly)
	{
		int			impliedDataOnly = 1;

		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if ((te->reqs & REQ_SCHEMA) != 0)
			{					/* 这是模式条目，且被需要 */
				impliedDataOnly = 0;
				break;
			}
		}
		if (impliedDataOnly)
		{
			ropt->dataOnly = impliedDataOnly;
			pg_log_info("implied data-only restore");
		}
	}

	/*
	 * 如有必要，设置输出文件。
	 */
	sav = SaveOutput(AH);
	if (ropt->filename || ropt->compression_spec.algorithm != PG_COMPRESSION_NONE)
		SetOutput(AH, ropt->filename, ropt->compression_spec);

	ahprintf(AH, "--\n-- PostgreSQL database dump\n--\n\n");

	/*
	 * 如果生成纯文本输出，进入受限模式以阻止任何
	 * 意外的 psql 元命令。恶意数据源可能试图通过伪造的查询
	 * 响应注入各种内容。虽然我们无法阻止此类数据源在恢复时
	 * 影响目标数据库，但我们可以阻止 psql 元命令，
	 * 以便运行 psql 并处理导出输出的客户端机器
	 * 不受影响。
	 */
	if (ropt->restrict_key)
		ahprintf(AH, "\\restrict %s\n\n", ropt->restrict_key);

	if (AH->archiveRemoteVersion)
		ahprintf(AH, "-- Dumped from database version %s\n",
				 AH->archiveRemoteVersion);
	if (AH->archiveDumpVersion)
		ahprintf(AH, "-- Dumped by pg_dump version %s\n",
				 AH->archiveDumpVersion);

	ahprintf(AH, "\n");

	if (AH->public.verbose)
		dumpTimestamp(AH, "Started on", AH->createDate);

	if (ropt->single_txn)
	{
		if (AH->connection)
			StartTransaction(AHX);
		else
			ahprintf(AH, "BEGIN;\n\n");
	}

	/*
	 * 立即建立重要的参数值。
	 */
	_doSetFixedOutputState(AH);

	AH->stage = STAGE_PROCESSING;

	/*
	 * 开始时按逆序删除项目
	 */
	if (ropt->dropSchema)
	{
		for (te = AH->toc->prev; te != AH->toc; te = te->prev)
		{
			AH->currentTE = te;

			/*
				 * 在 createDB 模式下，仅对数据库整体发出 DROP。
				 * 对其他任何对象发出 DROP 都是错误的，
				 * 因为此时我们连接的是错误的数据库。
				 * （DATABASE PROPERTIES 条目（如果有）应被视为
				 * 与 DATABASE 条目相同。）
				 */
			if (ropt->createDB)
			{
				if (strcmp(te->desc, "DATABASE") != 0 &&
					strcmp(te->desc, "DATABASE PROPERTIES") != 0)
					continue;
			}

			/* 否则，删除所有被选中且有 dropStmt 的项目 */
			if (((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0) && te->dropStmt)
			{
				pg_log_info("dropping %s %s", te->desc, te->tag);
				/* 根据需要选择所有者和模式 */
				_becomeOwner(AH, te);
				_selectOutputSchema(AH, te->namespace);

				/*
					 * 现在发出 DROP 命令，如果对象有的话。注意我们
					 * 不一定是逐字发出；此时我们根据用户请求添加
					 * 适当的 IF EXISTS 子句。
					 */
				if (*te->dropStmt != '\0')
				{
					if (!ropt->if_exists ||
						strncmp(te->dropStmt, "--", 2) == 0)
					{
						/*
						 * 如果没有 --if-exists，或者只是注释（如
						 * public 模式的情况），则按原样打印 dropStmt。
						 *
						 */
						ahprintf(AH, "%s", te->dropStmt);
					}
					else
					{
						/*
						 * 注入适当的 "if exists" 拼写。对于
						 * 大对象，我们有一个单独的例程知道
						 * 如何做到，而不依赖于 te->dropStmt；使用该例程。
						 * 对于其他对象，我们需要解析命令。
						 *
						 */
						if (strncmp(te->desc, "BLOB", 4) == 0)
						{
							DropLOIfExists(AH, te->catalogId.oid);
						}
						else
						{
							char	   *dropStmt = pg_strdup(te->dropStmt);
							char	   *dropStmtOrig = dropStmt;
							PQExpBuffer ftStmt = createPQExpBuffer();

							/*
							 * 需要在 ALTER TABLE .. DROP 语句的
							 * ALTER TABLE 部分之后注入 IF EXISTS 子句
							 */
							if (strncmp(dropStmt, "ALTER TABLE", 11) == 0)
							{
								appendPQExpBufferStr(ftStmt,
													 "ALTER TABLE IF EXISTS");
								dropStmt = dropStmt + 11;
							}

							/*
							 * ALTER TABLE..ALTER COLUMN..DROP DEFAULT
							 * 不支持 IF EXISTS 子句，因此
							 * 我们简单地发出 DEFAULT 对象的原始命令
							 * （除了上面所做的调整）。
							 *
							 * 同样，不要处理 DATABASE PROPERTIES。
							 *
							 * 如果我们使用 CREATE OR REPLACE VIEW 作为
							 * 准删除 ON SELECT 规则的手段，那也应该
							 * 按原样发出。
							 *
							 * 对于其他对象类型，我们需要提取 DROP 的
							 * 第一部分，其中包含对象类型。大多数情况下
							 * 这与 te->desc 匹配，因此搜索它；
							 * 对于不同类型的 CONSTRAINT，我们知道
							 * 要搜索硬编码的 "DROP CONSTRAINT"。
							 *
							 */
							if (strcmp(te->desc, "DEFAULT") == 0 ||
								strcmp(te->desc, "DATABASE PROPERTIES") == 0 ||
								strncmp(dropStmt, "CREATE OR REPLACE VIEW", 22) == 0)
								appendPQExpBufferStr(ftStmt, dropStmt);
							else
							{
								char		buffer[40];
								char	   *mark;

								if (strcmp(te->desc, "CONSTRAINT") == 0 ||
									strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
									strcmp(te->desc, "FK CONSTRAINT") == 0)
									strcpy(buffer, "DROP CONSTRAINT");
								else
									snprintf(buffer, sizeof(buffer), "DROP %s",
											 te->desc);

								mark = strstr(dropStmt, buffer);

								if (mark)
								{
									*mark = '\0';
									appendPQExpBuffer(ftStmt, "%s%s IF EXISTS%s",
													  dropStmt, buffer,
													  mark + strlen(buffer));
								}
								else
								{
									/* 报错并输出未修改的命令 */
									pg_log_warning("could not find where to insert IF EXISTS in statement \"%s\"",
												   dropStmtOrig);
									appendPQExpBufferStr(ftStmt, dropStmt);
								}
							}

							ahprintf(AH, "%s", ftStmt->data);

							destroyPQExpBuffer(ftStmt);
							pg_free(dropStmtOrig);
						}
					}
				}
			}
		}

		/*
		 * _selectOutputSchema 可能设置了 currSchema 来反映
		 * 它发出的 "SET search_path" 命令的效果。然而，
		 * 到现在我们可能已经删除了该模式；或者它可能
		 * 一开始就不存在。在任何情况下 search_path 的有效值
		 * 都不会是我们想象的那样。强制重置 currSchema，
		 * 以便在需要时（创建模式后）重新建立
		 * search_path 设置。
		 *
		 * 如果我们将用户视为可导出的对象，
		 * 那么此处也需要重置 currUser。
		 */
		free(AH->currSchema);
		AH->currSchema = NULL;
	}

	if (parallel_mode)
	{
		/*
		 * 在并行模式下，将控制权交给并行恢复逻辑。
		 */
		ParallelState *pstate;
		TocEntry	pending_list;

		/* 归档格式模块可能需要为此进行一些设置 */
		if (AH->PrepParallelRestorePtr)
			AH->PrepParallelRestorePtr(AH);

		pending_list_header_init(&pending_list);

		/* 这运行 PRE_DATA 项目，然后断开与数据库的连接 */
		restore_toc_entries_prefork(AH, &pending_list);
		Assert(AH->connection == NULL);

		/* ParallelBackupStart() 将实际派生进程 */
		pstate = ParallelBackupStart(AH);
		restore_toc_entries_parallel(AH, pstate, &pending_list);
		ParallelBackupEnd(AH, pstate);

		/* 重新连接主进程，查看是否有遗漏 */
		restore_toc_entries_postfork(AH, &pending_list);
		Assert(AH->connection != NULL);
	}
	else
	{
		/*
		 * 在串行模式下，分三个阶段处理所有内容：常规项目，
		 * 然后是 ACL，然后是 post-ACL 项目。在某些情况下，
		 * 我们可能可以跳过一个或两个额外阶段，例如仅数据恢复。
		 */
		bool		haveACL = false;
		bool		havePostACL = false;

		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if ((te->reqs & (REQ_SCHEMA | REQ_DATA)) == 0)
				continue;		/* 如果不导出则忽略 */

			switch (_tocEntryRestorePass(te))
			{
				case RESTORE_PASS_MAIN:
					(void) restore_toc_entry(AH, te, false);
					break;
				case RESTORE_PASS_ACL:
					haveACL = true;
					break;
				case RESTORE_PASS_POST_ACL:
					havePostACL = true;
					break;
			}
		}

		if (haveACL)
		{
			for (te = AH->toc->next; te != AH->toc; te = te->next)
			{
				if ((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0 &&
					_tocEntryRestorePass(te) == RESTORE_PASS_ACL)
					(void) restore_toc_entry(AH, te, false);
			}
		}

		if (havePostACL)
		{
			for (te = AH->toc->next; te != AH->toc; te = te->next)
			{
				if ((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0 &&
					_tocEntryRestorePass(te) == RESTORE_PASS_POST_ACL)
					(void) restore_toc_entry(AH, te, false);
			}
		}
	}

	if (ropt->single_txn)
	{
		if (AH->connection)
			CommitTransaction(AHX);
		else
			ahprintf(AH, "COMMIT;\n\n");
	}

	if (AH->public.verbose)
		dumpTimestamp(AH, "Completed on", time(NULL));

	ahprintf(AH, "--\n-- PostgreSQL database dump complete\n--\n\n");

	/*
	 * 如果生成纯文本输出，在脚本最后
	 * 退出受限模式。这不是形式上的；特别是 pg_dumpall
	 * 在从一个数据库过渡到另一个数据库时需要这样做。
	 */
	if (ropt->restrict_key)
		ahprintf(AH, "\\unrestrict %s\n\n", ropt->restrict_key);

	/*
	 * 清理并完成。
	 */
	AH->stage = STAGE_FINALIZING;

	if (ropt->filename || ropt->compression_spec.algorithm != PG_COMPRESSION_NONE)
		RestoreOutput(AH, sav);

	if (ropt->useDB)
		DisconnectDatabase(&AH->public);
}

/*
 * 恢复单个 TOC 项目。用于并行和非并行恢复；
 * 如果我们在工作子进程中，is_parallel 为 true。
 *
 * 正常返回 0，但如果并行父进程需要进行相应
 * 状态更新，则返回 WORKER_CREATE_DONE 或 WORKER_INHIBIT_DATA。
 */
static int
restore_toc_entry(ArchiveHandle *AH, TocEntry *te, bool is_parallel)
{
	RestoreOptions *ropt = AH->public.ropt;
	int			status = WORKER_OK;
	int			reqs;
	bool		defnDumped;

	AH->currentTE = te;

	/* 将相关的导出警告输出到 stderr */
	if (!ropt->suppressDumpWarnings && strcmp(te->desc, "WARNING") == 0)
	{
		if (!ropt->dataOnly && te->defn != NULL && strlen(te->defn) != 0)
			pg_log_warning("warning from original dump file: %s", te->defn);
		else if (te->copyStmt != NULL && strlen(te->copyStmt) != 0)
			pg_log_warning("warning from original dump file: %s", te->copyStmt);
	}

	/* 计算我们需要从此条目获取什么（如果有的话） */
	reqs = te->reqs;

	defnDumped = false;

	/*
	 * 如果它有我们需要的模式组件，则处理它
	 */
	if ((reqs & REQ_SCHEMA) != 0)
	{
		/* 如果可用，在日志消息中显示命名空间 */
		if (te->namespace)
			pg_log_info("creating %s \"%s.%s\"",
						te->desc, te->namespace, te->tag);
		else
			pg_log_info("creating %s \"%s\"",
						te->desc, te->tag);

		_printTocEntry(AH, te, false);
		defnDumped = true;

		if (strcmp(te->desc, "TABLE") == 0)
		{
			if (AH->lastErrorTE == te)
			{
				/*
				 * 我们未能创建表。如果
				 * 给出了 --no-data-for-failed-tables，标记
				 * 相应的 TABLE DATA 为忽略。
				 *
				 * 在并行情况下，这必须在父进程中完成，因此我们
				 * 只设置返回值。
				 */
				if (ropt->noDataForFailedTables)
				{
					if (is_parallel)
						status = WORKER_INHIBIT_DATA;
					else
						inhibit_data_for_failed_table(AH, te);
				}
			}
			else
			{
				/*
				 * 我们成功创建了表。标记相应的
				 * TABLE DATA 为可能截断。
				 *
				 * 在并行情况下，这必须在父进程中完成，因此我们
				 * 只设置返回值。
				 */
				if (is_parallel)
					status = WORKER_CREATE_DONE;
				else
					mark_create_done(AH, te);
			}
		}

		/*
		 * 如果我们创建了数据库，则连接到它。此外，如果我们更改了
		 * 数据库属性，重新连接以确保相关的 GUC 设置
		 * 应用于我们的会话。
		 */
		if (strcmp(te->desc, "DATABASE") == 0 ||
			strcmp(te->desc, "DATABASE PROPERTIES") == 0)
		{
			pg_log_info("connecting to new database \"%s\"", te->tag);
			_reconnectToDB(AH, te->tag);
		}
	}

	/*
	 * 如果它有我们需要的数据组件，则处理它
	 */
	if ((reqs & REQ_DATA) != 0)
	{
		/*
		 * 如果此节点有真正的数据组件，hadDumper 将被设置。
		 * 否则，我们需要检查 defn 字段中需要在仅数据恢复中
		 * 执行的语句。
		 */
		if (te->hadDumper)
		{
			/*
			 * 如果我们可以输出数据，则恢复它。
			 */
			if (AH->PrintTocDataPtr != NULL)
			{
				_printTocEntry(AH, te, true);

				if (strcmp(te->desc, "BLOBS") == 0 ||
					strcmp(te->desc, "BLOB COMMENTS") == 0)
				{
					pg_log_info("processing %s", te->desc);

					_selectOutputSchema(AH, "pg_catalog");

					/* 将 BLOB COMMENTS 数据发送到 ExecuteSimpleCommands() */
					if (strcmp(te->desc, "BLOB COMMENTS") == 0)
						AH->outputKind = OUTPUT_OTHERDATA;

					AH->PrintTocDataPtr(AH, te);

					AH->outputKind = OUTPUT_SQLCMDS;
				}
				else
				{
					bool		use_truncate;

					_disableTriggersIfNecessary(AH, te);

					/* 根据需要选择所有者和模式 */
					_becomeOwner(AH, te);
					_selectOutputSchema(AH, te->namespace);

					pg_log_info("processing data for table \"%s.%s\"",
								te->namespace, te->tag);

					/*
					 * 在并行恢复中，如果我们在本次运行中更早地
					 * 创建了表（因此我们知道它是空的）且我们没有
					 * 恢复 load-via-partition-root 数据项，
					 * 则我们将 COPY 包装在事务中并在其前面加上
					 * TRUNCATE。如果 wal_level 设置为 minimal，
					 * 这可以防止 COPY 被记录到 WAL 中。
					 * 这获得了与在非并行恢复中使用
					 * single_txn 模式类似的速度提升。
					 *
					 * 对于 load-via-partition-root 情况，
					 * 我们绝不能这样做，因为某些数据可能跨分区
					 * 边界移动，有死锁和/或丢失先前加载数据的风险。
					 * （我们假设分区表的所有分区
					 * 将以相同方式处理。）
					 */
					use_truncate = is_parallel && te->created &&
						!is_load_via_partition_root(te);

					if (use_truncate)
					{
						/*
						 * 并行恢复始终直接与服务器通信，
						 * 因此无需查看是否应发出 BEGIN。
						 */
						StartTransaction(&AH->public);

						/*
						 * 使用 ONLY 发出 TRUNCATE，以便子表
						 * 不被清空。
						 */
						ahprintf(AH, "TRUNCATE TABLE ONLY %s;\n\n",
								 fmtQualifiedId(te->namespace, te->tag));
					}

					/*
					 * 如果有 COPY 语句，则使用它。
					 */
					if (te->copyStmt && strlen(te->copyStmt) > 0)
					{
						ahprintf(AH, "%s", te->copyStmt);
						AH->outputKind = OUTPUT_COPYDATA;
					}
					else
						AH->outputKind = OUTPUT_OTHERDATA;

					AH->PrintTocDataPtr(AH, te);

					/*
					 * 如有需要，终止 COPY。
					 */
					if (AH->outputKind == OUTPUT_COPYDATA &&
						RestoringToDB(AH))
						EndDBCopyMode(&AH->public, te->tag);
					AH->outputKind = OUTPUT_SQLCMDS;

					/* 关闭上面启动的事务 */
					if (use_truncate)
						CommitTransaction(&AH->public);

					_enableTriggersIfNecessary(AH, te);
				}
			}
		}
		else if (!defnDumped)
		{
			/* 如果尚未导出定义部分，现在执行 */
			pg_log_info("executing %s %s", te->desc, te->tag);
			_printTocEntry(AH, te, false);
		}
	}

	if (AH->public.n_errors > 0 && status == WORKER_OK)
		status = WORKER_IGNORED_ERRORS;

	return status;
}

/*
 * 分配一个新的 RestoreOptions 块。
 * 这主要是为了初始化它，但也为将来扩展做准备，
 */
RestoreOptions *
NewRestoreOptions(void)
{
	RestoreOptions *opts;

	opts = (RestoreOptions *) pg_malloc0(sizeof(RestoreOptions));

	/* 设置不应默认为零的字段 */
	opts->format = archUnknown;
	opts->cparams.promptPassword = TRI_DEFAULT;
	opts->dumpSections = DUMP_UNSECTIONED;
	opts->compression_spec.algorithm = PG_COMPRESSION_NONE;
	opts->compression_spec.level = 0;

	return opts;
}

static void
_disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* 此技巧仅在仅数据恢复时需要 */
	if (!ropt->dataOnly || !ropt->disable_triggers)
		return;

	pg_log_info("disabling triggers for %s", te->tag);

	/*
	 * 尽可能成为超级用户，因为只有他们才能
	 * 禁用约束触发器。如果未给出 -S，则假设初始
	 * 用户身份是超级用户。（XXX 成为表所有者会更好吗？）
	 *
	 */
	_becomeUser(AH, ropt->superuser);

	/*
	 * 禁用它们。
	 */
	ahprintf(AH, "ALTER TABLE %s DISABLE TRIGGER ALL;\n\n",
			 fmtQualifiedId(te->namespace, te->tag));
}

static void
_enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* 此技巧仅在仅数据恢复时需要 */
	if (!ropt->dataOnly || !ropt->disable_triggers)
		return;

	pg_log_info("enabling triggers for %s", te->tag);

	/*
	 * 尽可能成为超级用户，因为只有他们才能
	 * 禁用约束触发器。如果未给出 -S，则假设初始
	 * 用户身份是超级用户。（XXX 成为表所有者会更好吗？）
	 *
	 */
	_becomeUser(AH, ropt->superuser);

	/*
	 * 启用它们。
	 */
	ahprintf(AH, "ALTER TABLE %s ENABLE TRIGGER ALL;\n\n",
			 fmtQualifiedId(te->namespace, te->tag));
}

/*
 * 检测 TABLE DATA TOC 项目是否在执行 "load via partition
 * root"，即目标表是祖先分区而不是
 * TOC 项目名义上针对的表。
 *
 * 在较新的归档文件中，可以通过检查 te->defn 中的
 * 特殊注释来检测。在较旧的文件中，我们必须回退到查看
 * COPY 语句是否以命名表为目标还是以其他表为目标。
 * 这对于以 INSERT 命令导出的数据不起作用，因此在这种
 * 情况下可能给出假阴性；幸运的是，这是一个很少使用的选项。
 */
static bool
is_load_via_partition_root(TocEntry *te)
{
	if (te->defn &&
		strncmp(te->defn, "-- load via partition root ", 27) == 0)
		return true;
	if (te->copyStmt && *te->copyStmt)
	{
		PQExpBuffer copyStmt = createPQExpBuffer();
		bool		result;

		/*
		 * 构建 COPY 的初始部分，就像名义上的
		 * 目标表是实际目标时那样。如果我们看到其他任何内容，
		 * 那一定是 load-via-partition-root 情况。
		 */
		appendPQExpBuffer(copyStmt, "COPY %s ",
						  fmtQualifiedId(te->namespace, te->tag));
		result = strncmp(te->copyStmt, copyStmt->data, copyStmt->len) != 0;
		destroyPQExpBuffer(copyStmt);
		return result;
	}
	/* 假设不是 load-via-partition-root */
	return false;
}

/*
 * 这是一个属于导出器接口的例程，因此使用 'Archive*' 参数。
 */

/* 公共接口 */
void
WriteData(Archive *AHX, const void *data, size_t dLen)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (!AH->currToc)
		pg_fatal("internal error -- WriteData cannot be called outside the context of a DataDumper routine");

	AH->WriteDataPtr(AH, data, dLen);
}

/*
 * 创建一个新的 TOC 条目。TOC 最初设计为目录，但现在
 * 是所有元数据的存储库。但这个名字已经固定下来了。
 *
 * 新条目被添加到归档的 TOC 列表中。大多数调用者可以忽略
 * 返回值，因为不需要做其他事情，但少数需要进一步
 * 操作 TOC 条目。
 */

/* 公共接口 */
TocEntry *
ArchiveEntry(Archive *AHX, CatalogId catalogId, DumpId dumpId,
			 ArchiveOpts *opts)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	TocEntry   *newToc;

	newToc = (TocEntry *) pg_malloc0(sizeof(TocEntry));

	AH->tocCount++;
	if (dumpId > AH->maxDumpId)
		AH->maxDumpId = dumpId;

	newToc->prev = AH->toc->prev;
	newToc->next = AH->toc;
	AH->toc->prev->next = newToc;
	AH->toc->prev = newToc;

	newToc->catalogId = catalogId;
	newToc->dumpId = dumpId;
	newToc->section = opts->section;

	newToc->tag = pg_strdup(opts->tag);
	newToc->namespace = opts->namespace ? pg_strdup(opts->namespace) : NULL;
	newToc->tablespace = opts->tablespace ? pg_strdup(opts->tablespace) : NULL;
	newToc->tableam = opts->tableam ? pg_strdup(opts->tableam) : NULL;
	newToc->owner = opts->owner ? pg_strdup(opts->owner) : NULL;
	newToc->desc = pg_strdup(opts->description);
	newToc->defn = opts->createStmt ? pg_strdup(opts->createStmt) : NULL;
	newToc->dropStmt = opts->dropStmt ? pg_strdup(opts->dropStmt) : NULL;
	newToc->copyStmt = opts->copyStmt ? pg_strdup(opts->copyStmt) : NULL;

	if (opts->nDeps > 0)
	{
		newToc->dependencies = (DumpId *) pg_malloc(opts->nDeps * sizeof(DumpId));
		memcpy(newToc->dependencies, opts->deps, opts->nDeps * sizeof(DumpId));
		newToc->nDeps = opts->nDeps;
	}
	else
	{
		newToc->dependencies = NULL;
		newToc->nDeps = 0;
	}

	newToc->dataDumper = opts->dumpFn;
	newToc->dataDumperArg = opts->dumpArg;
	newToc->hadDumper = opts->dumpFn ? true : false;

	newToc->formatData = NULL;
	newToc->dataLength = 0;

	if (AH->ArchiveEntryPtr != NULL)
		AH->ArchiveEntryPtr(AH, newToc);

	return newToc;
}

/* 公共接口 */
void
PrintTOCSummary(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	TocEntry   *te;
	pg_compress_specification out_compression_spec = {0};
	teSection	curSection;
	CompressFileHandle *sav;
	const char *fmtName;
	char		stamp_str[64];

	/* TOC 始终不压缩 */
	out_compression_spec.algorithm = PG_COMPRESSION_NONE;

	sav = SaveOutput(AH);
	if (ropt->filename)
		SetOutput(AH, ropt->filename, out_compression_spec);

	if (strftime(stamp_str, sizeof(stamp_str), PGDUMP_STRFTIME_FMT,
				 localtime(&AH->createDate)) == 0)
		strcpy(stamp_str, "[unknown]");

	ahprintf(AH, ";\n; Archive created at %s\n", stamp_str);
	ahprintf(AH, ";     dbname: %s\n;     TOC Entries: %d\n;     Compression: %s\n",
			 sanitize_line(AH->archdbname, false),
			 AH->tocCount,
			 get_compress_algorithm_name(AH->compression_spec.algorithm));

	switch (AH->format)
	{
		case archCustom:
			fmtName = "CUSTOM";
			break;
		case archDirectory:
			fmtName = "DIRECTORY";
			break;
		case archTar:
			fmtName = "TAR";
			break;
		default:
			fmtName = "UNKNOWN";
	}

	ahprintf(AH, ";     Dump Version: %d.%d-%d\n",
			 ARCHIVE_MAJOR(AH->version), ARCHIVE_MINOR(AH->version), ARCHIVE_REV(AH->version));
	ahprintf(AH, ";     Format: %s\n", fmtName);
	ahprintf(AH, ";     Integer: %d bytes\n", (int) AH->intSize);
	ahprintf(AH, ";     Offset: %d bytes\n", (int) AH->offSize);
	if (AH->archiveRemoteVersion)
		ahprintf(AH, ";     Dumped from database version: %s\n",
				 AH->archiveRemoteVersion);
	if (AH->archiveDumpVersion)
		ahprintf(AH, ";     Dumped by pg_dump version: %s\n",
				 AH->archiveDumpVersion);

	ahprintf(AH, ";\n;\n; Selected TOC Entries:\n;\n");

	curSection = SECTION_PRE_DATA;
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		/* 此部分必须匹配 ProcessArchiveRestoreOptions 的标记逻辑 */
		if (te->section != SECTION_NONE)
			curSection = te->section;
		te->reqs = _tocEntryRequired(te, curSection, AH);
		/* 现在，是否应该打印它？ */
		if (ropt->verbose ||
			(te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0)
		{
			char	   *sanitized_name;
			char	   *sanitized_schema;
			char	   *sanitized_owner;

			/*
			 */
			sanitized_name = sanitize_line(te->tag, false);
			sanitized_schema = sanitize_line(te->namespace, true);
			sanitized_owner = sanitize_line(te->owner, false);

			ahprintf(AH, "%d; %u %u %s %s %s %s\n", te->dumpId,
					 te->catalogId.tableoid, te->catalogId.oid,
					 te->desc, sanitized_schema, sanitized_name,
					 sanitized_owner);

			free(sanitized_name);
			free(sanitized_schema);
			free(sanitized_owner);
		}
		if (ropt->verbose && te->nDeps > 0)
		{
			int			i;

			ahprintf(AH, ";\tdepends on:");
			for (i = 0; i < te->nDeps; i++)
				ahprintf(AH, " %d", te->dependencies[i]);
			ahprintf(AH, "\n");
		}
	}

	/* 执行严格名称检查 */
	if (ropt->strict_names)
		StrictNamesCheck(ropt);

	if (ropt->filename)
		RestoreOutput(AH, sav);
}

/***********
 * Large Object Archival
 ***********/

/* 由导出器调用以发出大对象开始信号 */
int
StartLO(Archive *AHX, Oid oid)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (!AH->StartLOPtr)
		pg_fatal("large-object output not supported in chosen format");

	AH->StartLOPtr(AH, AH->currToc, oid);

	return 1;
}

/* 由导出器调用以发出大对象结束信号 */
int
EndLO(Archive *AHX, Oid oid)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (AH->EndLOPtr)
		AH->EndLOPtr(AH, AH->currToc, oid);

	return 1;
}

/**********
 * Large Object Restoration
 **********/

/*
 * 由格式处理器在恢复任何大对象之前调用
 */
void
StartRestoreLOs(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	if (!ropt->single_txn)
	{
		if (AH->connection)
			StartTransaction(&AH->public);
		else
			ahprintf(AH, "BEGIN;\n\n");
	}

	AH->loCount = 0;
}

/*
 * 由格式处理器在恢复所有大对象之后调用
 */
void
EndRestoreLOs(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	if (!ropt->single_txn)
	{
		if (AH->connection)
			CommitTransaction(&AH->public);
		else
			ahprintf(AH, "COMMIT;\n\n");
	}

	pg_log_info(ngettext("restored %d large object",
						 "restored %d large objects",
						 AH->loCount),
				AH->loCount);
}


/*
 * 由格式处理器调用以启动大对象的恢复
 */
void
StartRestoreLO(ArchiveHandle *AH, Oid oid, bool drop)
{
	bool		old_lo_style = (AH->version < K_VERS_1_12);
	Oid			loOid;

	AH->loCount++;

	/* 初始化大对象缓冲区 */
	AH->lo_buf_used = 0;

	pg_log_info("restoring large object with OID %u", oid);

	/* 对于旧归档，我们必须在此处执行删除和创建逻辑 */
	if (old_lo_style && drop)
		DropLOIfExists(AH, oid);

	if (AH->connection)
	{
		if (old_lo_style)
		{
			loOid = lo_create(AH->connection, oid);
			if (loOid == 0 || loOid != oid)
				pg_fatal("could not create large object %u: %s",
						 oid, PQerrorMessage(AH->connection));
		}
		AH->loFd = lo_open(AH->connection, oid, INV_WRITE);
		if (AH->loFd == -1)
			pg_fatal("could not open large object %u: %s",
					 oid, PQerrorMessage(AH->connection));
	}
	else
	{
		if (old_lo_style)
			ahprintf(AH, "SELECT pg_catalog.lo_open(pg_catalog.lo_create('%u'), %d);\n",
					 oid, INV_WRITE);
		else
			ahprintf(AH, "SELECT pg_catalog.lo_open('%u', %d);\n",
					 oid, INV_WRITE);
	}

	AH->writingLO = true;
}

void
EndRestoreLO(ArchiveHandle *AH, Oid oid)
{
	if (AH->lo_buf_used > 0)
	{
		/* 写入大对象缓冲区中的剩余字节 */
		dump_lo_buf(AH);
	}

	AH->writingLO = false;

	if (AH->connection)
	{
		lo_close(AH->connection, AH->loFd);
		AH->loFd = -1;
	}
	else
	{
		ahprintf(AH, "SELECT pg_catalog.lo_close(0);\n\n");
	}
}

/***********
 * Sorting and Reordering
 ***********/

void
SortTocFromFile(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	FILE	   *fh;
	StringInfoData linebuf;

	/* 为 'wanted' 数组分配空间并初始化 */
	ropt->idWanted = (bool *) pg_malloc0(sizeof(bool) * AH->maxDumpId);

	/* 设置文件 */
	fh = fopen(ropt->tocFile, PG_BINARY_R);
	if (!fh)
		pg_fatal("could not open TOC file \"%s\": %m", ropt->tocFile);

	initStringInfo(&linebuf);

	while (pg_get_line_buf(fh, &linebuf))
	{
		char	   *cmnt;
		char	   *endptr;
		DumpId		id;
		TocEntry   *te;

		/* 如果有注释，截断该行 */
		cmnt = strchr(linebuf.data, ';');
		if (cmnt != NULL)
		{
			cmnt[0] = '\0';
			linebuf.len = cmnt - linebuf.data;
		}

		/* 如果全为空则忽略 */
		if (strspn(linebuf.data, " \t\r\n") == linebuf.len)
			continue;

		/* 获取 ID，检查是否有效且未见过 */
		id = strtol(linebuf.data, &endptr, 10);
		if (endptr == linebuf.data || id <= 0 || id > AH->maxDumpId ||
			ropt->idWanted[id - 1])
		{
			pg_log_warning("line ignored: %s", linebuf.data);
			continue;
		}

		/* 查找 TOC 条目 */
		te = getTocEntryByDumpId(AH, id);
		if (!te)
			pg_fatal("could not find entry for ID %d",
					 id);

		/* 标记为需要 */
		ropt->idWanted[id - 1] = true;

		/*
		 * 将每个选中的项目移到列表末尾，使它们
		 * 按所需顺序排列。任何不需要的项目将最终
		 * 出现在列表前面，这可能看起来不直观，但这是
		 * 我们需要的。在普通串行恢复中这没有区别，
		 * 但在并行恢复中，我们需要在开始检查可恢复项目之前
		 * 将未恢复项目的依赖标记为已满足。
		 * 否则，它们可能对可恢复项目实际恢复的顺序
		 * 产生意外的副作用。
		 *
		 */
		_moveBefore(AH->toc, te);
	}

	pg_free(linebuf.data);

	if (fclose(fh) != 0)
		pg_fatal("could not close TOC file: %m");
}

/**********************
 * 看起来像标准 IO 函数的便捷函数
 * 用于在导出模式下写入数据。
 **********************/

/* 公共接口 */
void
archputs(const char *s, Archive *AH)
{
	WriteData(AH, s, strlen(s));
}

/* 公共接口 */
int
archprintf(Archive *AH, const char *fmt,...)
{
	int			save_errno = errno;
	char	   *p;
	size_t		len = 128;		/* 缓冲区大小的初始假设 */
	size_t		cnt;

	for (;;)
	{
		va_list		args;

		/* 分配工作缓冲区。 */
		p = (char *) pg_malloc(len);

		/* 尝试格式化数据。 */
		errno = save_errno;
		va_start(args, fmt);
		cnt = pvsnprintf(p, len, fmt, args);
		va_end(args);

		if (cnt < len)
			break;				/* 成功 */

		/* 释放缓冲区并循环，尝试更大的长度。 */
		free(p);
		len = cnt;
	}

	WriteData(AH, p, cnt);
	free(p);
	return (int) cnt;
}


/*******************************
 * 以下内容应对归档器例程保持'私有'
 *******************************/

static void
SetOutput(ArchiveHandle *AH, const char *filename,
		  const pg_compress_specification compression_spec)
{
	CompressFileHandle *CFH;
	const char *mode;
	int			fn = -1;

	if (filename)
	{
		if (strcmp(filename, "-") == 0)
			fn = fileno(stdout);
	}
	else if (AH->FH)
		fn = fileno(AH->FH);
	else if (AH->fSpec)
	{
		filename = AH->fSpec;
	}
	else
		fn = fileno(stdout);

	if (AH->mode == archModeAppend)
		mode = PG_BINARY_A;
	else
		mode = PG_BINARY_W;

	CFH = InitCompressFileHandle(compression_spec);

	if (!CFH->open_func(filename, fn, mode, CFH))
	{
		if (filename)
			pg_fatal("could not open output file \"%s\": %m", filename);
		else
			pg_fatal("could not open output file: %m");
	}

	AH->OF = CFH;
}

static CompressFileHandle *
SaveOutput(ArchiveHandle *AH)
{
	return (CompressFileHandle *) AH->OF;
}

static void
RestoreOutput(ArchiveHandle *AH, CompressFileHandle *savedOutput)
{
	errno = 0;
	if (!EndCompressFileHandle(AH->OF))
		pg_fatal("could not close output file: %m");

	AH->OF = savedOutput;
}



/*
 *	将格式化文本打印到输出文件（通常是 stdout）。
 */
int
ahprintf(ArchiveHandle *AH, const char *fmt,...)
{
	int			save_errno = errno;
	char	   *p;
	size_t		len = 128;		/* 缓冲区大小的初始假设 */
	size_t		cnt;

	for (;;)
	{
		va_list		args;

		/* 分配工作缓冲区。 */
		p = (char *) pg_malloc(len);

		/* 尝试格式化数据。 */
		errno = save_errno;
		va_start(args, fmt);
		cnt = pvsnprintf(p, len, fmt, args);
		va_end(args);

		if (cnt < len)
			break;				/* 成功 */

		/* 释放缓冲区并循环，尝试更大的长度。 */
		free(p);
		len = cnt;
	}

	ahwrite(p, 1, cnt, AH);
	free(p);
	return (int) cnt;
}

/*
 * 单一位置存放'我们正在恢复到直接的数据库连接'的逻辑。
 */
static int
RestoringToDB(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	return (ropt && ropt->useDB && AH->connection);
}

/*
 * 在写入大对象时导出 LO 数据缓冲区的当前内容
 */
static void
dump_lo_buf(ArchiveHandle *AH)
{
	if (AH->connection)
	{
		int			res;

		res = lo_write(AH->connection, AH->loFd, AH->lo_buf, AH->lo_buf_used);
		pg_log_debug(ngettext("wrote %zu byte of large object data (result = %d)",
							  "wrote %zu bytes of large object data (result = %d)",
							  AH->lo_buf_used),
					 AH->lo_buf_used, res);
		/* 我们假设没有短写入，只有错误 */
		if (res != AH->lo_buf_used)
			warn_or_exit_horribly(AH, "could not write to large object: %s",
								  PQerrorMessage(AH->connection));
	}
	else
	{
		PQExpBuffer buf = createPQExpBuffer();

		appendByteaLiteralAHX(buf,
							  (const unsigned char *) AH->lo_buf,
							  AH->lo_buf_used,
							  AH);

		/* 技巧：关闭 writingLO 使 ahwrite 不会递归到这里 */
		AH->writingLO = false;
		ahprintf(AH, "SELECT pg_catalog.lowrite(0, %s);\n", buf->data);
		AH->writingLO = true;

		destroyPQExpBuffer(buf);
	}
	AH->lo_buf_used = 0;
}


/*
 *	将缓冲区写入输出文件（通常是 stdout）。这用于
 *	输出'恢复'脚本等。归档格式甚至可以
 *	创建自定义输出例程来'模拟'恢复，如果它
 *	想要生成脚本（参见 TAR 输出）。
 */
void
ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle *AH)
{
	int			bytes_written = 0;

	if (AH->writingLO)
	{
		size_t		remaining = size * nmemb;

		while (AH->lo_buf_used + remaining > AH->lo_buf_size)
		{
			size_t		avail = AH->lo_buf_size - AH->lo_buf_used;

			memcpy((char *) AH->lo_buf + AH->lo_buf_used, ptr, avail);
			ptr = (const void *) ((const char *) ptr + avail);
			remaining -= avail;
			AH->lo_buf_used += avail;
			dump_lo_buf(AH);
		}

		memcpy((char *) AH->lo_buf + AH->lo_buf_used, ptr, remaining);
		AH->lo_buf_used += remaining;

		bytes_written = size * nmemb;
	}
	else if (AH->CustomOutPtr)
		bytes_written = AH->CustomOutPtr(AH, ptr, size * nmemb);

	/*
	 * 如果我们正在进行恢复，且是直接到数据库，且已连接，
	 * 则将其发送到数据库。
	 */
	else if (RestoringToDB(AH))
		bytes_written = ExecuteSqlCommandBuf(&AH->public, (const char *) ptr, size * nmemb);
	else
	{
		CompressFileHandle *CFH = (CompressFileHandle *) AH->OF;

		CFH->write_func(ptr, size * nmemb, CFH);
		bytes_written = size * nmemb;
	}

	if (bytes_written != size * nmemb)
		WRITE_ERROR_EXIT;
}

/* 在某些错误情况下，我们可能决定继续... */
void
warn_or_exit_horribly(ArchiveHandle *AH, const char *fmt,...)
{
	va_list		ap;

	switch (AH->stage)
	{

		case STAGE_NONE:
			/* 不做特殊处理 */
			break;

		case STAGE_INITIALIZING:
			if (AH->stage != AH->lastErrorStage)
				pg_log_info("while INITIALIZING:");
			break;

		case STAGE_PROCESSING:
			if (AH->stage != AH->lastErrorStage)
				pg_log_info("while PROCESSING TOC:");
			break;

		case STAGE_FINALIZING:
			if (AH->stage != AH->lastErrorStage)
				pg_log_info("while FINALIZING:");
			break;
	}
	if (AH->currentTE != NULL && AH->currentTE != AH->lastErrorTE)
	{
		pg_log_info("from TOC entry %d; %u %u %s %s %s",
					AH->currentTE->dumpId,
					AH->currentTE->catalogId.tableoid,
					AH->currentTE->catalogId.oid,
					AH->currentTE->desc ? AH->currentTE->desc : "(no desc)",
					AH->currentTE->tag ? AH->currentTE->tag : "(no tag)",
					AH->currentTE->owner ? AH->currentTE->owner : "(no owner)");
	}
	AH->lastErrorStage = AH->stage;
	AH->lastErrorTE = AH->currentTE;

	va_start(ap, fmt);
	pg_log_generic_v(PG_LOG_ERROR, PG_LOG_PRIMARY, fmt, ap);
	va_end(ap);

	if (AH->public.exit_on_error)
		exit_nicely(1);
	else
		AH->public.n_errors++;
}

#ifdef NOT_USED

static void
_moveAfter(ArchiveHandle *AH, TocEntry *pos, TocEntry *te)
{
	/* 将 te 从链表中解除链接 */
	te->prev->next = te->next;
	te->next->prev = te->prev;

	/* 并将其插入到 "pos" 之后 */
	te->prev = pos;
	te->next = pos->next;
	pos->next->prev = te;
	pos->next = te;
}
#endif

static void
_moveBefore(TocEntry *pos, TocEntry *te)
{
	/* 将 te 从链表中解除链接 */
	te->prev->next = te->next;
	te->next->prev = te->prev;

	/* 并将其插入到 "pos" 之前 */
	te->prev = pos->prev;
	te->next = pos;
	pos->prev->next = te;
	pos->prev = te;
}

/*
 * 为 TOC 列表构建索引数组
 *
 * 这应该仅在我们创建或读入所有 TOC
 * 项目后调用。
 *
 * 数组按导出 ID 索引（因此条目零未使用）。注意
 * 数组条目仅延伸到 maxDumpId。我们可能看到
 * 超出此范围的依赖导出 ID（如果导出是部分的）；因此在尝试
 * 访问数组条目之前始终检查数组边界。
 */
static void
buildTocEntryArrays(ArchiveHandle *AH)
{
	DumpId		maxDumpId = AH->maxDumpId;
	TocEntry   *te;

	AH->tocsByDumpId = (TocEntry **) pg_malloc0((maxDumpId + 1) * sizeof(TocEntry *));
	AH->tableDataId = (DumpId *) pg_malloc0((maxDumpId + 1) * sizeof(DumpId));

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		/* 此检查纯属偏执，maxDumpId 应该是正确的 */
		if (te->dumpId <= 0 || te->dumpId > maxDumpId)
			pg_fatal("bad dumpId");

		/* tocsByDumpId 按导出 ID 索引所有 TOC */
		AH->tocsByDumpId[te->dumpId] = te;

		/*
		 * tableDataId 为每个具有 DATA 项目的 TABLE
		 * TOC 条目提供 TABLE DATA 项目的导出 ID。
		 * 我们通过反转 TABLE DATA 项目的依赖来计算此值，
		 * 知道 TABLE DATA 项目只有一个依赖，即 TABLE 项目。
		 */
		if (strcmp(te->desc, "TABLE DATA") == 0 && te->nDeps > 0)
		{
			DumpId		tableId = te->dependencies[0];

			/*
			 * TABLE 项目可能不在归档中，如果这是
			 * 仅数据导出；但其导出 ID 应小于其数据
			 * 项目的导出 ID，因此数组中应有其位置。
			 */
			if (tableId <= 0 || tableId > maxDumpId)
				pg_fatal("bad table dumpId for TABLE DATA item");

			AH->tableDataId[tableId] = te->dumpId;
		}
	}
}

TocEntry *
getTocEntryByDumpId(ArchiveHandle *AH, DumpId id)
{
	/* 如果尚未构建索引数组则构建 */
	if (AH->tocsByDumpId == NULL)
		buildTocEntryArrays(AH);

	if (id > 0 && id <= AH->maxDumpId)
		return AH->tocsByDumpId[id];

	return NULL;
}

int
TocIDRequired(ArchiveHandle *AH, DumpId id)
{
	TocEntry   *te = getTocEntryByDumpId(AH, id);

	if (!te)
		return 0;

	return te->reqs;
}

size_t
WriteOffset(ArchiveHandle *AH, pgoff_t o, int wasSet)
{
	int			off;

	/* 保存标志 */
	AH->WriteBytePtr(AH, wasSet);

	/* 先写出 pgoff_t 的最小字节，防止字节序不匹配 */
	for (off = 0; off < sizeof(pgoff_t); off++)
	{
		AH->WriteBytePtr(AH, o & 0xFF);
		o >>= 8;
	}
	return sizeof(pgoff_t) + 1;
}

int
ReadOffset(ArchiveHandle *AH, pgoff_t * o)
{
	int			i;
	int			off;
	int			offsetFlg;

	/* 初始化为零 */
	*o = 0;

	/* 检查旧版本 */
	if (AH->version < K_VERS_1_7)
	{
		/* 先前版本使用 WriteInt 写入偏移量 */
		i = ReadInt(AH);
		/* -1 表示未设置 */
		if (i < 0)
			return K_OFFSET_POS_NOT_SET;
		else if (i == 0)
			return K_OFFSET_NO_DATA;

		/* 转换为 pgoff_t，因为它是以 int 写入的。 */
		*o = (pgoff_t) i;
		return K_OFFSET_POS_SET;
	}

	/*
	 * 读取指示数据指针状态的标志。检查是否有效，
	 * 如果无效则终止。
	 *
	 * 过去通过负或零指针处理，现在使用
	 * 一个专门用于状态的额外字节。
	 */
	offsetFlg = AH->ReadBytePtr(AH) & 0xFF;

	switch (offsetFlg)
	{
		case K_OFFSET_POS_NOT_SET:
		case K_OFFSET_NO_DATA:
		case K_OFFSET_POS_SET:

			break;

		default:
			pg_fatal("unexpected data offset flag %d", offsetFlg);
	}

	/*
	 * 读取字节
	 */
	for (off = 0; off < AH->offSize; off++)
	{
		if (off < sizeof(pgoff_t))
			*o |= ((pgoff_t) (AH->ReadBytePtr(AH))) << (off * 8);
		else
		{
			if (AH->ReadBytePtr(AH) != 0)
				pg_fatal("file offset in dump file is too large");
		}
	}

	return offsetFlg;
}

size_t
WriteInt(ArchiveHandle *AH, int i)
{
	int			b;

	/*
	 * 这有点糟糕，但我不想让二进制格式
	 * 过于依赖表示形式，而且对此了解不多，我写出
	 * 一个符号字节。如果你更改此内容，不要忘记更改文件
	 * 版本号，并修改 ReadInt 以读取新格式以及
	 * 旧格式。
 */

	/* 符号字节 */
	if (i < 0)
	{
		AH->WriteBytePtr(AH, 1);
		i = -i;
	}
	else
		AH->WriteBytePtr(AH, 0);

	for (b = 0; b < AH->intSize; b++)
	{
		AH->WriteBytePtr(AH, i & 0xFF);
		i >>= 8;
	}

	return AH->intSize + 1;
}

int
ReadInt(ArchiveHandle *AH)
{
	int			res = 0;
	int			bv,
				b;
	int			sign = 0;		/* 默认正数 */
	int			bitShift = 0;

	if (AH->version > K_VERS_1_0)
		/* 读取符号字节 */
		sign = AH->ReadBytePtr(AH);

	for (b = 0; b < AH->intSize; b++)
	{
		bv = AH->ReadBytePtr(AH) & 0xFF;
		if (bv != 0)
			res = res + (bv << bitShift);
		bitShift += 8;
	}

	if (sign)
		res = -res;

	return res;
}

size_t
WriteStr(ArchiveHandle *AH, const char *c)
{
	size_t		res;

	if (c)
	{
		int			len = strlen(c);

		res = WriteInt(AH, len);
		AH->WriteBufPtr(AH, c, len);
		res += len;
	}
	else
		res = WriteInt(AH, -1);

	return res;
}

char *
ReadStr(ArchiveHandle *AH)
{
	char	   *buf;
	int			l;

	l = ReadInt(AH);
	if (l < 0)
		buf = NULL;
	else
	{
		buf = (char *) pg_malloc(l + 1);
		AH->ReadBufPtr(AH, (void *) buf, l);

		buf[l] = '\0';
	}

	return buf;
}

static bool
_fileExistsInDirectory(const char *dir, const char *filename)
{
	struct stat st;
	char		buf[MAXPGPATH];

	if (snprintf(buf, MAXPGPATH, "%s/%s", dir, filename) >= MAXPGPATH)
		pg_fatal("directory name too long: \"%s\"", dir);

	return (stat(buf, &st) == 0 && S_ISREG(st.st_mode));
}

static int
_discoverArchiveFormat(ArchiveHandle *AH)
{
	FILE	   *fh;
	char		sig[6];			/* 绰绰有余 */
	size_t		cnt;
	int			wantClose = 0;

	pg_log_debug("attempting to ascertain archive format");

	free(AH->lookahead);

	AH->readHeader = 0;
	AH->lookaheadSize = 512;
	AH->lookahead = pg_malloc0(512);
	AH->lookaheadLen = 0;
	AH->lookaheadPos = 0;

	if (AH->fSpec)
	{
		struct stat st;

		wantClose = 1;

		/*
		 * 检查指定的归档是否是目录。如果是，检查
		 * 其中是否有 "toc.dat"（或 "toc.dat.{gz,lz4,zst}"）文件。
		 */
		if (stat(AH->fSpec, &st) == 0 && S_ISDIR(st.st_mode))
		{
			AH->format = archDirectory;
			if (_fileExistsInDirectory(AH->fSpec, "toc.dat"))
				return AH->format;
#ifdef HAVE_LIBZ
			if (_fileExistsInDirectory(AH->fSpec, "toc.dat.gz"))
				return AH->format;
#endif
#ifdef USE_LZ4
			if (_fileExistsInDirectory(AH->fSpec, "toc.dat.lz4"))
				return AH->format;
#endif
#ifdef USE_ZSTD
			if (_fileExistsInDirectory(AH->fSpec, "toc.dat.zst"))
				return AH->format;
#endif
			pg_fatal("directory \"%s\" does not appear to be a valid archive (\"toc.dat\" does not exist)",
					 AH->fSpec);
			fh = NULL;			/* 让编译器安静 */
		}
		else
		{
			fh = fopen(AH->fSpec, PG_BINARY_R);
			if (!fh)
				pg_fatal("could not open input file \"%s\": %m", AH->fSpec);
		}
	}
	else
	{
		fh = stdin;
		if (!fh)
			pg_fatal("could not open input file: %m");
	}

	if ((cnt = fread(sig, 1, 5, fh)) != 5)
	{
		if (ferror(fh))
			pg_fatal("could not read input file: %m");
		else
			pg_fatal("input file is too short (read %lu, expected 5)",
					 (unsigned long) cnt);
	}

	/* 保存它，以防以后需要 */
	memcpy(&AH->lookahead[0], sig, 5);
	AH->lookaheadLen = 5;

	if (strncmp(sig, "PGDMP", 5) == 0)
	{
		/* 这是自定义格式，在此停止 */
		AH->format = archCustom;
		AH->readHeader = 1;
	}
	else
	{
		/*
		 * *也许* 我们有一个 tar 归档格式文件或文本导出 ... 所以，
		 * 读取第一个 512 字节头部...
		 */
		cnt = fread(&AH->lookahead[AH->lookaheadLen], 1, 512 - AH->lookaheadLen, fh);
		/* 读取失败在下面检查 */
		AH->lookaheadLen += cnt;

		if (AH->lookaheadLen >= strlen(TEXT_DUMPALL_HEADER) &&
			(strncmp(AH->lookahead, TEXT_DUMP_HEADER, strlen(TEXT_DUMP_HEADER)) == 0 ||
			 strncmp(AH->lookahead, TEXT_DUMPALL_HEADER, strlen(TEXT_DUMPALL_HEADER)) == 0))
		{
			/*
			 * 看起来可能是文本格式导出。建议他们
			 * 尝试使用 psql。
			 */
			pg_fatal("input file appears to be a text format dump. Please use psql.");
		}

		if (AH->lookaheadLen != 512)
		{
			if (feof(fh))
				pg_fatal("input file does not appear to be a valid archive (too short?)");
			else
				READ_ERROR_EXIT(fh);
		}

		if (!isValidTarHeader(AH->lookahead))
			pg_fatal("input file does not appear to be a valid archive");

		AH->format = archTar;
	}

	/* 如果我们打开了文件则关闭它 */
	if (wantClose)
	{
		if (fclose(fh) != 0)
			pg_fatal("could not close input file: %m");
		/* 忘记预读，因为重新打开后会重新读取头部 */
		AH->readHeader = 0;
		AH->lookaheadLen = 0;
	}

	return AH->format;
}


/*
 * 分配归档句柄
 */
static ArchiveHandle *
_allocAH(const char *FileSpec, const ArchiveFormat fmt,
		 const pg_compress_specification compression_spec,
		 bool dosync, ArchiveMode mode,
		 SetupWorkerPtrType setupWorkerPtr)
{
	ArchiveHandle *AH;
	CompressFileHandle *CFH;
	pg_compress_specification out_compress_spec = {0};

	pg_log_debug("allocating AH for %s, format %d",
				 FileSpec ? FileSpec : "(stdio)", fmt);

	AH = (ArchiveHandle *) pg_malloc0(sizeof(ArchiveHandle));

	AH->version = K_VERS_SELF;

	/* 初始化以实现向后兼容的字符串处理 */
	AH->public.encoding = 0;	/* PG_SQL_ASCII */
	AH->public.std_strings = false;

	/* SQL 错误处理 */
	AH->public.exit_on_error = true;
	AH->public.n_errors = 0;

	AH->archiveDumpVersion = PG_VERSION;

	AH->createDate = time(NULL);

	AH->intSize = sizeof(int);
	AH->offSize = sizeof(pgoff_t);
	if (FileSpec)
	{
		AH->fSpec = pg_strdup(FileSpec);

		/*
		 * 未使用；也许以后会用到...
		 *
		 * AH->workDir = pg_strdup(FileSpec); for(i=strlen(FileSpec) ; i > 0 ;
		 * i--) if (AH->workDir[i-1] == '/')
		 */
	}
	else
		AH->fSpec = NULL;

	AH->currUser = NULL;		/* 未知 */
	AH->currSchema = NULL;		/* 同上 */
	AH->currTablespace = NULL;	/* 同上 */
	AH->currTableAm = NULL;		/* 同上 */

	AH->toc = (TocEntry *) pg_malloc0(sizeof(TocEntry));

	AH->toc->next = AH->toc;
	AH->toc->prev = AH->toc;

	AH->mode = mode;
	AH->compression_spec = compression_spec;
	AH->dosync = dosync;

	memset(&(AH->sqlparse), 0, sizeof(AH->sqlparse));

	/* 为 AH 输出句柄打开无压缩的 stdout */
	out_compress_spec.algorithm = PG_COMPRESSION_NONE;
	CFH = InitCompressFileHandle(out_compress_spec);
	if (!CFH->open_func(NULL, fileno(stdout), PG_BINARY_A, CFH))
		pg_fatal("could not open stdout for appending: %m");
	AH->OF = CFH;

	/*
	 * 在 Windows 上，我们需要使用二进制模式来读/写非文本文件，
	 * 这包括所有归档格式以及压缩的纯文本。
	 * 如果这是我们要使用的，强制 stdin/stdout 进入二进制模式。
	 */
#ifdef WIN32
	if ((fmt != archNull || compression_spec.algorithm != PG_COMPRESSION_NONE) &&
		(AH->fSpec == NULL || strcmp(AH->fSpec, "") == 0))
	{
		if (mode == archModeWrite)
			_setmode(fileno(stdout), O_BINARY);
		else
			_setmode(fileno(stdin), O_BINARY);
	}
#endif

	AH->SetupWorkerPtr = setupWorkerPtr;

	if (fmt == archUnknown)
		AH->format = _discoverArchiveFormat(AH);
	else
		AH->format = fmt;

	switch (AH->format)
	{
		case archCustom:
			InitArchiveFmt_Custom(AH);
			break;

		case archNull:
			InitArchiveFmt_Null(AH);
			break;

		case archDirectory:
			InitArchiveFmt_Directory(AH);
			break;

		case archTar:
			InitArchiveFmt_Tar(AH);
			break;

		default:
			pg_fatal("unrecognized file format \"%d\"", fmt);
	}

	return AH;
}

/*
 * 写出所有数据（表和 LOs）
 */
void
WriteDataChunks(ArchiveHandle *AH, ParallelState *pstate)
{
	TocEntry   *te;

	if (pstate && pstate->numWorkers > 1)
	{
		/*
		 * 在并行模式下，此代码在领导进程中运行。我们
		 * 构建一个候选 TE 数组，然后按递减大小顺序
		 * 排序，然后将每个 TE 分派给数据传输工作进程。
		 * 通过首先导出较大的表，我们避免陷入只剩一个
		 * 大任务而失去并行性的情况。
		 */
		TocEntry  **tes;
		int			ntes;

		tes = (TocEntry **) pg_malloc(AH->tocCount * sizeof(TocEntry *));
		ntes = 0;
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			/* 仅考虑具有 dataDumper 函数的 TE ... */
			if (!te->dataDumper)
				continue;
			/* ... 并忽略未启用导出的 TE */
			if ((te->reqs & REQ_DATA) == 0)
				continue;

			tes[ntes++] = te;
		}

		if (ntes > 1)
			qsort(tes, ntes, sizeof(TocEntry *), TocEntrySizeCompare);

		for (int i = 0; i < ntes; i++)
			DispatchJobForTocEntry(AH, pstate, tes[i], ACT_DUMP,
								   mark_dump_job_done, NULL);

		pg_free(tes);

		/* 现在等待工作进程完成。 */
		WaitForWorkers(AH, pstate, WFW_ALL_IDLE);
	}
	else
	{
		/* 非并行模式：仅顺序导出所有候选 TE。 */
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			/* 必须具有与上面相同的过滤条件 */
			if (!te->dataDumper)
				continue;
			if ((te->reqs & REQ_DATA) == 0)
				continue;

			WriteDataChunksForTocEntry(AH, te);
		}
	}
}


/*
 * 在步骤并行导出后在领导进程中调用的
 * 回调函数。
 *
 * 除了检查工作进程失败外，我们不需要做任何事情。
 */
static void
mark_dump_job_done(ArchiveHandle *AH,
				   TocEntry *te,
				   int status,
				   void *callback_data)
{
	pg_log_info("finished item %d %s %s",
				te->dumpId, te->desc, te->tag);

	if (status != 0)
		pg_fatal("worker process failed: exit code %d",
				 status);
}


void
WriteDataChunksForTocEntry(ArchiveHandle *AH, TocEntry *te)
{
	StartDataPtrType startPtr;
	EndDataPtrType endPtr;

	AH->currToc = te;

	if (strcmp(te->desc, "BLOBS") == 0)
	{
		startPtr = AH->StartLOsPtr;
		endPtr = AH->EndLOsPtr;
	}
	else
	{
		startPtr = AH->StartDataPtr;
		endPtr = AH->EndDataPtr;
	}

	if (startPtr != NULL)
		(*startPtr) (AH, te);

	/*
	 * 用户提供的 DataDumper 例程需要调用 AH->WriteData
	 */
	te->dataDumper((Archive *) AH, te->dataDumperArg);

	if (endPtr != NULL)
		(*endPtr) (AH, te);

	AH->currToc = NULL;
}

void
WriteToc(ArchiveHandle *AH)
{
	TocEntry   *te;
	char		workbuf[32];
	int			tocCount;
	int			i;

	/* 统计实际将被导出的条目数 */
	tocCount = 0;
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if ((te->reqs & (REQ_SCHEMA | REQ_DATA | REQ_SPECIAL)) != 0)
			tocCount++;
	}

	/* printf("%d TOC Entries to save\n", tocCount); */

	WriteInt(AH, tocCount);

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if ((te->reqs & (REQ_SCHEMA | REQ_DATA | REQ_SPECIAL)) == 0)
			continue;

		WriteInt(AH, te->dumpId);
		WriteInt(AH, te->dataDumper ? 1 : 0);

		/* OID 由于历史原因以字符串形式记录 */
		sprintf(workbuf, "%u", te->catalogId.tableoid);
		WriteStr(AH, workbuf);
		sprintf(workbuf, "%u", te->catalogId.oid);
		WriteStr(AH, workbuf);

		WriteStr(AH, te->tag);
		WriteStr(AH, te->desc);
		WriteInt(AH, te->section);
		WriteStr(AH, te->defn);
		WriteStr(AH, te->dropStmt);
		WriteStr(AH, te->copyStmt);
		WriteStr(AH, te->namespace);
		WriteStr(AH, te->tablespace);
		WriteStr(AH, te->tableam);
		WriteStr(AH, te->owner);
		WriteStr(AH, "false");

		/* 导出依赖列表 */
		for (i = 0; i < te->nDeps; i++)
		{
			sprintf(workbuf, "%d", te->dependencies[i]);
			WriteStr(AH, workbuf);
		}
		WriteStr(AH, NULL);		/* 终止列表 */

		if (AH->WriteExtraTocPtr)
			AH->WriteExtraTocPtr(AH, te);
	}
}

void
ReadToc(ArchiveHandle *AH)
{
	int			i;
	char	   *tmp;
	DumpId	   *deps;
	int			depIdx;
	int			depSize;
	TocEntry   *te;
	bool		is_supported;

	AH->tocCount = ReadInt(AH);
	AH->maxDumpId = 0;

	for (i = 0; i < AH->tocCount; i++)
	{
		te = (TocEntry *) pg_malloc0(sizeof(TocEntry));
		te->dumpId = ReadInt(AH);

		if (te->dumpId > AH->maxDumpId)
			AH->maxDumpId = te->dumpId;

		/* 健全性检查 */
		if (te->dumpId <= 0)
			pg_fatal("entry ID %d out of range -- perhaps a corrupt TOC",
					 te->dumpId);

		te->hadDumper = ReadInt(AH);

		if (AH->version >= K_VERS_1_8)
		{
			tmp = ReadStr(AH);
			sscanf(tmp, "%u", &te->catalogId.tableoid);
			free(tmp);
		}
		else
			te->catalogId.tableoid = InvalidOid;
		tmp = ReadStr(AH);
		sscanf(tmp, "%u", &te->catalogId.oid);
		free(tmp);

		te->tag = ReadStr(AH);
		te->desc = ReadStr(AH);

		if (AH->version >= K_VERS_1_11)
		{
			te->section = ReadInt(AH);
		}
		else
		{
			/*
			 * 针对 pg_dump 尚未将条目分类到节的
			 * pre-8.4 归档的规则。此列表不需要覆盖
			 * 8.4 之后添加的条目类型。
			 */
			if (strcmp(te->desc, "COMMENT") == 0 ||
				strcmp(te->desc, "ACL") == 0 ||
				strcmp(te->desc, "ACL LANGUAGE") == 0)
				te->section = SECTION_NONE;
			else if (strcmp(te->desc, "TABLE DATA") == 0 ||
					 strcmp(te->desc, "BLOBS") == 0 ||
					 strcmp(te->desc, "BLOB COMMENTS") == 0)
				te->section = SECTION_DATA;
			else if (strcmp(te->desc, "CONSTRAINT") == 0 ||
					 strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
					 strcmp(te->desc, "FK CONSTRAINT") == 0 ||
					 strcmp(te->desc, "INDEX") == 0 ||
					 strcmp(te->desc, "RULE") == 0 ||
					 strcmp(te->desc, "TRIGGER") == 0)
				te->section = SECTION_POST_DATA;
			else
				te->section = SECTION_PRE_DATA;
		}

		te->defn = ReadStr(AH);
		te->dropStmt = ReadStr(AH);

		if (AH->version >= K_VERS_1_3)
			te->copyStmt = ReadStr(AH);

		if (AH->version >= K_VERS_1_6)
			te->namespace = ReadStr(AH);

		if (AH->version >= K_VERS_1_10)
			te->tablespace = ReadStr(AH);

		if (AH->version >= K_VERS_1_14)
			te->tableam = ReadStr(AH);

		te->owner = ReadStr(AH);
		is_supported = true;
		if (AH->version < K_VERS_1_9)
			is_supported = false;
		else
		{
			tmp = ReadStr(AH);

			if (strcmp(tmp, "true") == 0)
				is_supported = false;

			free(tmp);
		}

		if (!is_supported)
			pg_log_warning("restoring tables WITH OIDS is not supported anymore");

		/* 读取 TOC 条目依赖 */
		if (AH->version >= K_VERS_1_5)
		{
			depSize = 100;
			deps = (DumpId *) pg_malloc(sizeof(DumpId) * depSize);
			depIdx = 0;
			for (;;)
			{
				tmp = ReadStr(AH);
				if (!tmp)
					break;		/* 列表结束 */
				if (depIdx >= depSize)
				{
					depSize *= 2;
					deps = (DumpId *) pg_realloc(deps, sizeof(DumpId) * depSize);
				}
				sscanf(tmp, "%d", &deps[depIdx]);
				free(tmp);
				depIdx++;
			}

			if (depIdx > 0)		/* 我们有一个非空条目 */
			{
				deps = (DumpId *) pg_realloc(deps, sizeof(DumpId) * depIdx);
				te->dependencies = deps;
				te->nDeps = depIdx;
			}
			else
			{
				free(deps);
				te->dependencies = NULL;
				te->nDeps = 0;
			}
		}
		else
		{
			te->dependencies = NULL;
			te->nDeps = 0;
		}
		te->dataLength = 0;

		if (AH->ReadExtraTocPtr)
			AH->ReadExtraTocPtr(AH, te);

		pg_log_debug("read TOC entry %d (ID %d) for %s %s",
					 i, te->dumpId, te->desc, te->tag);

		/* 将完成的条目链接到 TOC 循环链表 */
		te->prev = AH->toc->prev;
		AH->toc->prev->next = te;
		AH->toc->prev = te;
		te->next = AH->toc;

		/* 某些项目在读取时立即进行特殊处理 */
		if (strcmp(te->desc, "ENCODING") == 0)
			processEncodingEntry(AH, te);
		else if (strcmp(te->desc, "STDSTRINGS") == 0)
			processStdStringsEntry(AH, te);
		else if (strcmp(te->desc, "SEARCHPATH") == 0)
			processSearchPathEntry(AH, te);
	}
}

static void
processEncodingEntry(ArchiveHandle *AH, TocEntry *te)
{
	/* te->defn 应具有 SET client_encoding = 'foo'; 的形式 */
	char	   *defn = pg_strdup(te->defn);
	char	   *ptr1;
	char	   *ptr2 = NULL;
	int			encoding;

	ptr1 = strchr(defn, '\'');
	if (ptr1)
		ptr2 = strchr(++ptr1, '\'');
	if (ptr2)
	{
		*ptr2 = '\0';
		encoding = pg_char_to_encoding(ptr1);
		if (encoding < 0)
			pg_fatal("unrecognized encoding \"%s\"",
					 ptr1);
		AH->public.encoding = encoding;
		setFmtEncoding(encoding);
	}
	else
		pg_fatal("invalid ENCODING item: %s",
				 te->defn);

	free(defn);
}

static void
processStdStringsEntry(ArchiveHandle *AH, TocEntry *te)
{
	/* te->defn 应具有 SET standard_conforming_strings = 'x'; 的形式 */
	char	   *ptr1;

	ptr1 = strchr(te->defn, '\'');
	if (ptr1 && strncmp(ptr1, "'on'", 4) == 0)
		AH->public.std_strings = true;
	else if (ptr1 && strncmp(ptr1, "'off'", 5) == 0)
		AH->public.std_strings = false;
	else
		pg_fatal("invalid STDSTRINGS item: %s",
				 te->defn);
}

static void
processSearchPathEntry(ArchiveHandle *AH, TocEntry *te)
{
	/*
	 * te->defn 应包含一个设置 search_path 的命令。我们只是
	 * 逐字复制以供以后使用。
	 */
	AH->public.searchpath = pg_strdup(te->defn);
}

static void
StrictNamesCheck(RestoreOptions *ropt)
{
	const char *missing_name;

	Assert(ropt->strict_names);

	if (ropt->schemaNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->schemaNames);
		if (missing_name != NULL)
			pg_fatal("schema \"%s\" not found", missing_name);
	}

	if (ropt->tableNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->tableNames);
		if (missing_name != NULL)
			pg_fatal("table \"%s\" not found", missing_name);
	}

	if (ropt->indexNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->indexNames);
		if (missing_name != NULL)
			pg_fatal("index \"%s\" not found", missing_name);
	}

	if (ropt->functionNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->functionNames);
		if (missing_name != NULL)
			pg_fatal("function \"%s\" not found", missing_name);
	}

	if (ropt->triggerNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->triggerNames);
		if (missing_name != NULL)
			pg_fatal("trigger \"%s\" not found", missing_name);
	}
}

/*
 * 确定我们是否要恢复此 TOC 条目。
 *
 * 如果要跳过条目，返回 0；如果要恢复此 TOC 条目的
 * 模式和/或数据部分，返回 REQ_SCHEMA 和 REQ_DATA 位的
 * 某种组合；如果是特殊条目，返回 REQ_SPECIAL。
 */
static int
_tocEntryRequired(TocEntry *te, teSection curSection, ArchiveHandle *AH)
{
	int			res = REQ_SCHEMA | REQ_DATA;
	RestoreOptions *ropt = AH->public.ropt;

	/* 这些项目有特殊处理 */
	if (strcmp(te->desc, "ENCODING") == 0 ||
		strcmp(te->desc, "STDSTRINGS") == 0 ||
		strcmp(te->desc, "SEARCHPATH") == 0)
		return REQ_SPECIAL;

	/*
	 * DATABASE 和 DATABASE PROPERTIES 也有特殊规则：
	 * 它们在 createDB 模式下恢复，否则不恢复，
	 * 独立于所有其他设置。
	 */
	if (strcmp(te->desc, "DATABASE") == 0 ||
		strcmp(te->desc, "DATABASE PROPERTIES") == 0)
	{
		if (ropt->createDB)
			return REQ_SCHEMA;
		else
			return 0;
	}

	/*
	 * 处理影响某些类别 TOC 条目的排除项。
	 */

	/* 如果是 ACL，可能忽略它 */
	if (ropt->aclsSkip && _tocEntryIsACL(te))
		return 0;

	/* 如果是注释，可能忽略它 */
	if (ropt->no_comments && strcmp(te->desc, "COMMENT") == 0)
		return 0;

	/*
	 * 如果是发布或订阅的注释，可能忽略它。
	 */
	if (strcmp(te->desc, "COMMENT") == 0)
	{
		if (ropt->no_publications &&
			strncmp(te->tag, "PUBLICATION", strlen("PUBLICATION")) == 0)
			return 0;

		if (ropt->no_subscriptions &&
			strncmp(te->tag, "SUBSCRIPTION", strlen("SUBSCRIPTION")) == 0)
			return 0;
	}

	/*
	 * 如果是发布或发布的一部分表，可能忽略
	 * 它。
	 */
	if (ropt->no_publications &&
		(strcmp(te->desc, "PUBLICATION") == 0 ||
		 strcmp(te->desc, "PUBLICATION TABLE") == 0 ||
		 strcmp(te->desc, "PUBLICATION TABLES IN SCHEMA") == 0))
		return 0;

	/* 如果是安全标签，可能忽略它 */
	if (ropt->no_security_labels && strcmp(te->desc, "SECURITY LABEL") == 0)
		return 0;

	/*
	 * 如果是发布或订阅的安全标签，可能
	 * 忽略它。
	 */
	if (strcmp(te->desc, "SECURITY LABEL") == 0)
	{
		if (ropt->no_publications &&
			strncmp(te->tag, "PUBLICATION", strlen("PUBLICATION")) == 0)
			return 0;

		if (ropt->no_subscriptions &&
			strncmp(te->tag, "SUBSCRIPTION", strlen("SUBSCRIPTION")) == 0)
			return 0;
	}

	/* 如果是订阅，可能忽略它 */
	if (ropt->no_subscriptions && strcmp(te->desc, "SUBSCRIPTION") == 0)
		return 0;

	/* 如果节不应被导出/恢复则忽略它 */
	switch (curSection)
	{
		case SECTION_PRE_DATA:
			if (!(ropt->dumpSections & DUMP_PRE_DATA))
				return 0;
			break;
		case SECTION_DATA:
			if (!(ropt->dumpSections & DUMP_DATA))
				return 0;
			break;
		case SECTION_POST_DATA:
			if (!(ropt->dumpSections & DUMP_POST_DATA))
				return 0;
			break;
		default:
			/* 实际上不应该到这里，但忽略它 */
			return 0;
	}

	/* 如果被 idWanted[] 拒绝则忽略（参见 SortTocFromFile） */
	if (ropt->idWanted && !ropt->idWanted[te->dumpId - 1])
		return 0;

	/*
	 * 检查选择性导出/恢复的选项。
	 */
	if (strcmp(te->desc, "ACL") == 0 ||
		strcmp(te->desc, "COMMENT") == 0 ||
		strcmp(te->desc, "SECURITY LABEL") == 0)
	{
		/* 数据库属性响应 createDB，而非选择性选项。 */
		if (strncmp(te->tag, "DATABASE ", 9) == 0)
		{
			if (!ropt->createDB)
				return 0;
		}
		else if (ropt->schemaNames.head != NULL ||
				 ropt->schemaExcludeNames.head != NULL ||
				 ropt->selTypes)
		{
			/*
			 * 在选择性导出/恢复中，我们只想在这些依赖的
			 * TOC 条目类型的父对象被恢复时才恢复它们。
			 * 没有选择性选项时，我们让归档中的所有内容
			 * 通过。注意可能存在没有父对象的此类条目，
			 * 例如内置对象的非默认 ACL。此外，我们使
			 * 按列的 ACL 额外依赖于表的 ACL（如果有的话）
			 * 以确保正确的恢复顺序，因此在此检查中应
			 * 忽略这些依赖。
			 *
			 * 此代码依赖于父对象已被标记，
			 * 应该如此；如果不是，可能是由于
			 * SortTocFromFile 重排，跳过依赖条目
			 * 似乎也是谨慎的。
			 *
			 * 理想情况下，我们也应该以这种方式处理表 CHECK 约束。
			 * 但很难判断它们的哪个依赖是应该咨询的。
			 *
			 */
			bool		dumpthis = false;

			for (int i = 0; i < te->nDeps; i++)
			{
				TocEntry   *pte = getTocEntryByDumpId(AH, te->dependencies[i]);

				if (!pte)
					continue;	/* 可能不应该发生 */
				if (strcmp(pte->desc, "ACL") == 0)
					continue;	/* 忽略对另一个 ACL 的依赖 */
				if (pte->reqs == 0)
					continue;	/* 此对象未被标记，所以忽略它 */
				/* 找到要导出的父对象，因此也要导出此对象 */
				dumpthis = true;
				break;
			}
			if (!dumpthis)
				return 0;
		}
	}
	else
	{
		/* 为独立的 TOC 条目应用选择性恢复规则。 */
		if (ropt->schemaNames.head != NULL)
		{
			/* 如果未指定命名空间，则表示全部。 */
			if (!te->namespace)
				return 0;
			if (!simple_string_list_member(&ropt->schemaNames, te->namespace))
				return 0;
		}

		if (ropt->schemaExcludeNames.head != NULL &&
			te->namespace &&
			simple_string_list_member(&ropt->schemaExcludeNames, te->namespace))
			return 0;

		if (ropt->selTypes)
		{
			if (strcmp(te->desc, "TABLE") == 0 ||
				strcmp(te->desc, "TABLE DATA") == 0 ||
				strcmp(te->desc, "VIEW") == 0 ||
				strcmp(te->desc, "FOREIGN TABLE") == 0 ||
				strcmp(te->desc, "MATERIALIZED VIEW") == 0 ||
				strcmp(te->desc, "MATERIALIZED VIEW DATA") == 0 ||
				strcmp(te->desc, "SEQUENCE") == 0 ||
				strcmp(te->desc, "SEQUENCE SET") == 0)
			{
				if (!ropt->selTable)
					return 0;
				if (ropt->tableNames.head != NULL &&
					!simple_string_list_member(&ropt->tableNames, te->tag))
					return 0;
			}
			else if (strcmp(te->desc, "INDEX") == 0)
			{
				if (!ropt->selIndex)
					return 0;
				if (ropt->indexNames.head != NULL &&
					!simple_string_list_member(&ropt->indexNames, te->tag))
					return 0;
			}
			else if (strcmp(te->desc, "FUNCTION") == 0 ||
					 strcmp(te->desc, "AGGREGATE") == 0 ||
					 strcmp(te->desc, "PROCEDURE") == 0)
			{
				if (!ropt->selFunction)
					return 0;
				if (ropt->functionNames.head != NULL &&
					!simple_string_list_member(&ropt->functionNames, te->tag))
					return 0;
			}
			else if (strcmp(te->desc, "TRIGGER") == 0)
			{
				if (!ropt->selTrigger)
					return 0;
				if (ropt->triggerNames.head != NULL &&
					!simple_string_list_member(&ropt->triggerNames, te->tag))
					return 0;
			}
			else
				return 0;
		}
	}

	/*
	 * 确定 TOC 条目是否包含模式和/或数据组件，
	 * 并屏蔽不适用的 REQ 位。如果有 dataDumper，
	 * 假设它同时包含模式和数据。否则可能只是模式，
	 * 但也有例外。
	 */
	if (!te->hadDumper)
	{
		/*
		 * 特殊情况：如果是 'SEQUENCE SET' 或任何与 LO 相关的，
		 * 则将其视为数据条目。我们不需要检查 BLOBS 条目
		 * 或旧式 BLOB COMMENTS，因为它们将具有 hadDumper
		 * = true ... 但我们需要检查新式 BLOB ACL、注释、
		 * 等。
		 */
		if (strcmp(te->desc, "SEQUENCE SET") == 0 ||
			strcmp(te->desc, "BLOB") == 0 ||
			(strcmp(te->desc, "ACL") == 0 &&
			 strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			(strcmp(te->desc, "COMMENT") == 0 &&
			 strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			(strcmp(te->desc, "SECURITY LABEL") == 0 &&
			 strncmp(te->tag, "LARGE OBJECT ", 13) == 0))
			res = res & REQ_DATA;
		else
			res = res & ~REQ_DATA;
	}

	/*
	 * 如果没有定义命令，就没有模式组件。将
	 * "load via partition root" 注释视为非模式。
	 */
	if (!te->defn || !te->defn[0] ||
		strncmp(te->defn, "-- load via partition root ", 27) == 0)
		res = res & ~REQ_SCHEMA;

	/*
	 * 特殊情况：<Init> 类型带 <Max OID> 标记；这是过时的，
	 * 我们始终忽略它。
	 */
	if ((strcmp(te->desc, "<Init>") == 0) && (strcmp(te->tag, "Max OID") == 0))
		return 0;

	/* 如果只需要模式则屏蔽 */
	if (ropt->schemaOnly)
	{
		/*
		 * sequence_data 选项为 SEQUENCE SET 覆盖 schemaOnly。
		 *
		 * 在 binary-upgrade 模式下，即使设置了 schemaOnly，
		 * 我们也不屏蔽大对象。（在 binary-upgrade 模式下只应
		 * 生成大对象定义、注释和其他元数据，而不是实际数据，
		 * 但这不需要我们在此关心。）
		 */
		if (!(ropt->sequence_data && strcmp(te->desc, "SEQUENCE SET") == 0) &&
			!(ropt->binary_upgrade &&
			  (strcmp(te->desc, "BLOB") == 0 ||
			   (strcmp(te->desc, "ACL") == 0 &&
				strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			   (strcmp(te->desc, "COMMENT") == 0 &&
				strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			   (strcmp(te->desc, "SECURITY LABEL") == 0 &&
				strncmp(te->tag, "LARGE OBJECT ", 13) == 0))))
			res = res & REQ_SCHEMA;
	}

	/* 如果只需要数据则屏蔽 */
	if (ropt->dataOnly)
		res = res & REQ_DATA;

	return res;
}

/*
 * 确定我们应该在哪个遍历中恢复此 TOC 条目。
 *
 * 参见 pg_backup_archiver.h 中 RestorePass 类型定义的注释。
 */
static RestorePass
_tocEntryRestorePass(TocEntry *te)
{
	/* "ACL LANGUAGE" 是仅在 PG 7.4 中输出的遗留内容 */
	if (strcmp(te->desc, "ACL") == 0 ||
		strcmp(te->desc, "ACL LANGUAGE") == 0 ||
		strcmp(te->desc, "DEFAULT ACL") == 0)
		return RESTORE_PASS_ACL;
	if (strcmp(te->desc, "EVENT TRIGGER") == 0 ||
		strcmp(te->desc, "MATERIALIZED VIEW DATA") == 0)
		return RESTORE_PASS_POST_ACL;

	/*
	 * 注释和安全标签需要与其父对象在同一遍历中发出。
	 * ACL 没有注释和安全标签，物化视图数据对象也没有，
	 * 但事件触发器有。
	 * （幸运的是，事件触发器没有 ACL，否则我们将需要
	 * 另一个奇怪的特殊情况。）
	 */
	if ((strcmp(te->desc, "COMMENT") == 0 ||
		 strcmp(te->desc, "SECURITY LABEL") == 0) &&
		strncmp(te->tag, "EVENT TRIGGER ", 14) == 0)
		return RESTORE_PASS_POST_ACL;

	/* 其他所有内容可以在主遍历中处理。 */
	return RESTORE_PASS_MAIN;
}

/*
 * 识别是 ACL 的 TOC 条目。
 *
 * 注意：似乎值得在此处复制一些代码，以避免硬编码
 * 假设这些与我们在 RESTORE_PASS_ACL 阶段恢复的条目
 * 完全相同。
 */
static bool
_tocEntryIsACL(TocEntry *te)
{
	/* "ACL LANGUAGE" 是仅在 PG 7.4 中输出的遗留内容 */
	if (strcmp(te->desc, "ACL") == 0 ||
		strcmp(te->desc, "ACL LANGUAGE") == 0 ||
		strcmp(te->desc, "DEFAULT ACL") == 0)
		return true;
	return false;
}

/*
 * 发出 SET 命令，用于我们希望在整个恢复脚本执行期间
 * 始终保持相同设置的参数。
 */
static void
_doSetFixedOutputState(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	/*
	 * 禁用超时以允许慢速命令、空闲并行工作进程等
	 */
	ahprintf(AH, "SET statement_timeout = 0;\n");
	ahprintf(AH, "SET lock_timeout = 0;\n");
	ahprintf(AH, "SET idle_in_transaction_session_timeout = 0;\n");

	/* 选择正确的字符集编码 */
	ahprintf(AH, "SET client_encoding = '%s';\n",
			 pg_encoding_to_char(AH->public.encoding));

	/* 选择正确的字符串字面量语法 */
	ahprintf(AH, "SET standard_conforming_strings = %s;\n",
			 AH->public.std_strings ? "on" : "off");

	/* 选择恢复期间要使用的角色 */
	if (ropt && ropt->use_role)
		ahprintf(AH, "SET ROLE %s;\n", fmtId(ropt->use_role));

	/* 选择导出时的 search_path */
	if (AH->public.searchpath)
		ahprintf(AH, "%s", AH->public.searchpath);

	/* 确保函数检查已禁用 */
	ahprintf(AH, "SET check_function_bodies = false;\n");

	/* 确保所有有效的 XML 数据将被接受 */
	ahprintf(AH, "SET xmloption = content;\n");

	/* 避免烦人的通知等 */
	ahprintf(AH, "SET client_min_messages = warning;\n");
	if (!AH->public.std_strings)
		ahprintf(AH, "SET escape_string_warning = off;\n");

	/* 调整行安全状态 */
	if (ropt && ropt->enable_row_security)
		ahprintf(AH, "SET row_security = on;\n");
	else
		ahprintf(AH, "SET row_security = off;\n");

	ahprintf(AH, "\n");
}

/*
 * 发出 SET SESSION AUTHORIZATION 命令。调用者负责
 * 在适当时更新状态。如果 user 为 NULL 或空字符串，
 * 将使用 DEFAULT 规范。
 */
static void
_doSetSessionAuth(ArchiveHandle *AH, const char *user)
{
	PQExpBuffer cmd = createPQExpBuffer();

	appendPQExpBufferStr(cmd, "SET SESSION AUTHORIZATION ");

	/*
	 * SQL 要求此处使用字符串字面量。不妨做得正确。
	 */
	if (user && *user)
		appendStringLiteralAHX(cmd, user, AH);
	else
		appendPQExpBufferStr(cmd, "DEFAULT");
	appendPQExpBufferChar(cmd, ';');

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, cmd->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			/* 不要使用 warn_or_exit_horribly... 使用 -O 来跳过此操作。 */
			pg_fatal("could not set session user to \"%s\": %s",
					 user, PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s\n\n", cmd->data);

	destroyPQExpBuffer(cmd);
}


/*
 * 发出连接到指定数据库的命令。
 *
 * 如果我们当前直接恢复到数据库，这将
 * 实际建立连接。否则将 \connect 放入
 * 脚本输出。
 */
static void
_reconnectToDB(ArchiveHandle *AH, const char *dbname)
{
	if (RestoringToDB(AH))
		ReconnectToServer(AH, dbname);
	else
	{
		PQExpBufferData connectbuf;
		RestoreOptions *ropt = AH->public.ropt;

		/*
		 * 我们必须暂时退出受限模式以执行 \connect 等。
		 * 在此行和后续 \restrict 之间添加的任何内容必须
		 * 小心避免任何可能的元命令注入向量。
		 */
		ahprintf(AH, "\\unrestrict %s\n", ropt->restrict_key);

		initPQExpBuffer(&connectbuf);
		appendPsqlMetaConnect(&connectbuf, dbname);
		ahprintf(AH, "%s", connectbuf.data);
		termPQExpBuffer(&connectbuf);

		ahprintf(AH, "\\restrict %s\n\n", ropt->restrict_key);
	}

	/*
	 * 注意：currUser 跟踪我们脚本中的虚拟会话用户。
	 * 它现在已有效重置为原始用户 ID。
	 */
	free(AH->currUser);
	AH->currUser = NULL;

	/* 也不要假设我们仍然知道输出模式、表空间等 */
	free(AH->currSchema);
	AH->currSchema = NULL;

	free(AH->currTableAm);
	AH->currTableAm = NULL;

	free(AH->currTablespace);
	AH->currTablespace = NULL;

	/* 重新建立固定状态 */
	_doSetFixedOutputState(AH);
}

/*
 * 成为指定用户，并更新状态以避免冗余命令
 *
 * NULL 或空参数表示恢复会话默认值
 */
static void
_becomeUser(ArchiveHandle *AH, const char *user)
{
	if (!user)
		user = "";				/* 避免空指针 */

	if (AH->currUser && strcmp(AH->currUser, user) == 0)
		return;					/* 无需执行任何操作 */

	_doSetSessionAuth(AH, user);

	/*
	 * 注意：currUser 跟踪我们脚本中的
	 * 虚拟会话用户
	 */
	free(AH->currUser);
	AH->currUser = pg_strdup(user);
}

/*
 * 成为给定 TOC 条目对象的所有者。如果
 * 不允许更改所有权，则不执行任何操作。
 */
static void
_becomeOwner(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->public.ropt;

	if (ropt && (ropt->noOwner || !ropt->use_setsessauth))
		return;

	_becomeUser(AH, te->owner);
}


/*
 * 发出命令，将指定的模式选择为目标数据库中的
 * 当前模式。
 */
static void
_selectOutputSchema(ArchiveHandle *AH, const char *schemaName)
{
	PQExpBuffer qry;

	/*
	 * 如果有 SEARCHPATH TOC 条目，我们应该保持
	 * 该 search_path 而不是切换到条目特定的路径。
	 * 否则，这是一个旧归档，除非我们按预期
	 * 设置 search_path，否则它将无法正确恢复。
	 */
	if (AH->public.searchpath)
		return;

	if (!schemaName || *schemaName == '\0' ||
		(AH->currSchema && strcmp(AH->currSchema, schemaName) == 0))
		return;					/* 无需执行任何操作 */

	qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "SET search_path = %s",
					  fmtId(schemaName));
	if (strcmp(schemaName, "pg_catalog") != 0)
		appendPQExpBufferStr(qry, ", pg_catalog");

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, qry->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH,
								  "could not set search_path to \"%s\": %s",
								  schemaName, PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s;\n\n", qry->data);

	free(AH->currSchema);
	AH->currSchema = pg_strdup(schemaName);

	destroyPQExpBuffer(qry);
}

/*
 * 发出命令，将指定的表空间选择为
 * 目标数据库中的当前表空间。
 */
static void
_selectTablespace(ArchiveHandle *AH, const char *tablespace)
{
	RestoreOptions *ropt = AH->public.ropt;
	PQExpBuffer qry;
	const char *want,
			   *have;

	/* 在 --no-tablespaces 模式下不执行任何操作 */
	if (ropt->noTablespace)
		return;

	have = AH->currTablespace;
	want = tablespace;

	/* 对于非表空间对象无需执行任何操作 */
	if (!want)
		return;

	if (have && strcmp(want, have) == 0)
		return;					/* 无需执行任何操作 */

	qry = createPQExpBuffer();

	if (strcmp(want, "") == 0)
	{
		/* 我们希望表空间为数据库的默认值 */
		appendPQExpBufferStr(qry, "SET default_tablespace = ''");
	}
	else
	{
		/* 我们希望使用显式表空间 */
		appendPQExpBuffer(qry, "SET default_tablespace = %s", fmtId(want));
	}

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, qry->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH,
								  "could not set default_tablespace to %s: %s",
								  fmtId(want), PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s;\n\n", qry->data);

	free(AH->currTablespace);
	AH->currTablespace = pg_strdup(want);

	destroyPQExpBuffer(qry);
}

/*
 * 为表设置正确的 default_table_access_method 值。
 */
static void
_selectTableAccessMethod(ArchiveHandle *AH, const char *tableam)
{
	RestoreOptions *ropt = AH->public.ropt;
	PQExpBuffer cmd;
	const char *want,
			   *have;

	/* 在 --no-table-access-method 模式下不执行任何操作 */
	if (ropt->noTableAm)
		return;

	have = AH->currTableAm;
	want = tableam;

	if (!want)
		return;

	if (have && strcmp(want, have) == 0)
		return;

	cmd = createPQExpBuffer();
	appendPQExpBuffer(cmd, "SET default_table_access_method = %s;", fmtId(want));

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, cmd->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH,
								  "could not set default_table_access_method: %s",
								  PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s\n\n", cmd->data);

	destroyPQExpBuffer(cmd);

	free(AH->currTableAm);
	AH->currTableAm = pg_strdup(want);
}

/*
 * 提取 TOC 条目的对象描述，并将其追加到 buf。
 *
 * 这用于 ALTER ... OWNER TO。
 *
 * 如果对象类型没有所有者，则不执行任何操作。
 */
static void
_getObjectDescription(PQExpBuffer buf, const TocEntry *te)
{
	const char *type = te->desc;

	/* 不需要特殊修饰的对象 */
	if (strcmp(type, "COLLATION") == 0 ||
		strcmp(type, "CONVERSION") == 0 ||
		strcmp(type, "DOMAIN") == 0 ||
		strcmp(type, "FOREIGN TABLE") == 0 ||
		strcmp(type, "MATERIALIZED VIEW") == 0 ||
		strcmp(type, "SEQUENCE") == 0 ||
		strcmp(type, "STATISTICS") == 0 ||
		strcmp(type, "TABLE") == 0 ||
		strcmp(type, "TEXT SEARCH DICTIONARY") == 0 ||
		strcmp(type, "TEXT SEARCH CONFIGURATION") == 0 ||
		strcmp(type, "TYPE") == 0 ||
		strcmp(type, "VIEW") == 0 ||
	/* 非模式指定的对象 */
		strcmp(type, "DATABASE") == 0 ||
		strcmp(type, "PROCEDURAL LANGUAGE") == 0 ||
		strcmp(type, "SCHEMA") == 0 ||
		strcmp(type, "EVENT TRIGGER") == 0 ||
		strcmp(type, "FOREIGN DATA WRAPPER") == 0 ||
		strcmp(type, "SERVER") == 0 ||
		strcmp(type, "PUBLICATION") == 0 ||
		strcmp(type, "SUBSCRIPTION") == 0)
	{
		appendPQExpBuffer(buf, "%s ", type);
		if (te->namespace && *te->namespace)
			appendPQExpBuffer(buf, "%s.", fmtId(te->namespace));
		appendPQExpBufferStr(buf, fmtId(te->tag));
	}
	/* 大对象只有一个名称，但它是数字，因此不能使用 fmtId */
	else if (strcmp(type, "BLOB") == 0)
	{
		appendPQExpBuffer(buf, "LARGE OBJECT %s", te->tag);
	}

	/*
	 * 这些对象类型需要额外的修饰。幸运的是，所需的
	 * 信息正好在 DROP 命令中。
	 */
	else if (strcmp(type, "AGGREGATE") == 0 ||
			 strcmp(type, "FUNCTION") == 0 ||
			 strcmp(type, "OPERATOR") == 0 ||
			 strcmp(type, "OPERATOR CLASS") == 0 ||
			 strcmp(type, "OPERATOR FAMILY") == 0 ||
			 strcmp(type, "PROCEDURE") == 0)
	{
		/* 去掉前面的 "DROP " 并创建可修改的副本 */
		char	   *first = pg_strdup(te->dropStmt + 5);
		char	   *last;

		/* 指向字符串中的最后一个字符 */
		last = first + strlen(first) - 1;

		/* 去掉末尾的 ';' 或 '\n' */
		while (last >= first && (*last == '\n' || *last == ';'))
			last--;
		*(last + 1) = '\0';

		appendPQExpBufferStr(buf, first);

		free(first);
		return;
	}
	/* 这些对象类型没有单独的所有者 */
	else if (strcmp(type, "CAST") == 0 ||
			 strcmp(type, "CHECK CONSTRAINT") == 0 ||
			 strcmp(type, "CONSTRAINT") == 0 ||
			 strcmp(type, "DATABASE PROPERTIES") == 0 ||
			 strcmp(type, "DEFAULT") == 0 ||
			 strcmp(type, "FK CONSTRAINT") == 0 ||
			 strcmp(type, "INDEX") == 0 ||
			 strcmp(type, "RULE") == 0 ||
			 strcmp(type, "TRIGGER") == 0 ||
			 strcmp(type, "ROW SECURITY") == 0 ||
			 strcmp(type, "POLICY") == 0 ||
			 strcmp(type, "USER MAPPING") == 0)
	{
		/* 不执行任何操作 */
	}
	else
		pg_fatal("don't know how to set owner for object type \"%s\"", type);
}

/*
 * 发出 SQL 命令以创建 TOC 条目表示的对象
 *
 * 这现在还包括发出 ALTER OWNER 命令以恢复
 * 对象的所有权（如果需要）。但请注意，对象的权限
 * 将保持默认值，直到匹配的 ACL TOC 条目被恢复。
 */
static void
_printTocEntry(ArchiveHandle *AH, TocEntry *te, bool isData)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* 根据需要选择所有者、模式、表空间和默认 AM */
	_becomeOwner(AH, te);
	_selectOutputSchema(AH, te->namespace);
	_selectTablespace(AH, te->tablespace);
	_selectTableAccessMethod(AH, te->tableam);

	/* 为项目输出头部注释 */
	if (!AH->noTocComments)
	{
		const char *pfx;
		char	   *sanitized_name;
		char	   *sanitized_schema;
		char	   *sanitized_owner;

		if (isData)
			pfx = "Data for ";
		else
			pfx = "";

		ahprintf(AH, "--\n");
		if (AH->public.verbose)
		{
			ahprintf(AH, "-- TOC entry %d (class %u OID %u)\n",
					 te->dumpId, te->catalogId.tableoid, te->catalogId.oid);
			if (te->nDeps > 0)
			{
				int			i;

				ahprintf(AH, "-- Dependencies:");
				for (i = 0; i < te->nDeps; i++)
					ahprintf(AH, " %d", te->dependencies[i]);
				ahprintf(AH, "\n");
			}
		}

		sanitized_name = sanitize_line(te->tag, false);
		sanitized_schema = sanitize_line(te->namespace, true);
		sanitized_owner = sanitize_line(ropt->noOwner ? NULL : te->owner, true);

		ahprintf(AH, "-- %sName: %s; Type: %s; Schema: %s; Owner: %s",
				 pfx, sanitized_name, te->desc, sanitized_schema,
				 sanitized_owner);

		free(sanitized_name);
		free(sanitized_schema);
		free(sanitized_owner);

		if (te->tablespace && strlen(te->tablespace) > 0 && !ropt->noTablespace)
		{
			char	   *sanitized_tablespace;

			sanitized_tablespace = sanitize_line(te->tablespace, false);
			ahprintf(AH, "; Tablespace: %s", sanitized_tablespace);
			free(sanitized_tablespace);
		}
		ahprintf(AH, "\n");

		if (AH->PrintExtraTocPtr != NULL)
			AH->PrintExtraTocPtr(AH, te);
		ahprintf(AH, "--\n\n");
	}

	/*
	 * 实际打印定义。
	 *
	 * 用于抑制旧版 pg_dump 放入 CREATE SCHEMA 的 AUTHORIZATION 子句的
	 * 真正粗糙的技巧。不要修改作为注释的模式 "public" 变体。
	 * 当选择了 --no-owner 模式时我们必须这样做。
	 * 这很丑陋，但我看不到其他好方法 ...
	 */
	if (ropt->noOwner &&
		strcmp(te->desc, "SCHEMA") == 0 && strncmp(te->defn, "--", 2) != 0)
	{
		ahprintf(AH, "CREATE SCHEMA %s;\n\n\n", fmtId(te->tag));
	}
	else
	{
		if (te->defn && strlen(te->defn) > 0)
			ahprintf(AH, "%s\n\n", te->defn);
	}

	/*
	 * 如果我们不使用 SET SESSION AUTH 来确定所有权，
	 * 我们必须改为发出 ALTER OWNER 命令。模式 "public" 是特殊的；
	 * 当导出发出注释代替创建它时，即使对所有其他对象使用 SET SESSION，
	 * 我们也使用 ALTER OWNER。我们假设任何没有 DROP 命令
	 * 的对象不是可单独拥有的对象。
	 */
	if (!ropt->noOwner &&
		(!ropt->use_setsessauth ||
		 (strcmp(te->desc, "SCHEMA") == 0 &&
		  strncmp(te->defn, "--", 2) == 0)) &&
		te->owner && strlen(te->owner) > 0 &&
		te->dropStmt && strlen(te->dropStmt) > 0)
	{
		PQExpBufferData temp;

		initPQExpBuffer(&temp);
		_getObjectDescription(&temp, te);

		/*
		 * 如果 _getObjectDescription() 没有填充缓冲区，
		 * 则没有所有者。
		 */
		if (temp.data[0])
			ahprintf(AH, "ALTER %s OWNER TO %s;\n\n", temp.data, fmtId(te->owner));
		termPQExpBuffer(&temp);
	}

	/*
	 * 如果它是 ACL 条目，可能包含 SET SESSION AUTHORIZATION
	 * 命令，因此我们不能再假设我们知道当前的身份验证设置。
	 */
	if (_tocEntryIsACL(te))
	{
		free(AH->currUser);
		AH->currUser = NULL;
	}
}

/*
 * 为自定义格式归档写入文件头
 */
void
WriteHead(ArchiveHandle *AH)
{
	struct tm	crtm;

	AH->WriteBufPtr(AH, "PGDMP", 5);	/* 魔术码 */
	AH->WriteBytePtr(AH, ARCHIVE_MAJOR(AH->version));
	AH->WriteBytePtr(AH, ARCHIVE_MINOR(AH->version));
	AH->WriteBytePtr(AH, ARCHIVE_REV(AH->version));
	AH->WriteBytePtr(AH, AH->intSize);
	AH->WriteBytePtr(AH, AH->offSize);
	AH->WriteBytePtr(AH, AH->format);
	AH->WriteBytePtr(AH, AH->compression_spec.algorithm);
	crtm = *localtime(&AH->createDate);
	WriteInt(AH, crtm.tm_sec);
	WriteInt(AH, crtm.tm_min);
	WriteInt(AH, crtm.tm_hour);
	WriteInt(AH, crtm.tm_mday);
	WriteInt(AH, crtm.tm_mon);
	WriteInt(AH, crtm.tm_year);
	WriteInt(AH, crtm.tm_isdst);
	WriteStr(AH, PQdb(AH->connection));
	WriteStr(AH, AH->public.remoteVersionStr);
	WriteStr(AH, PG_VERSION);
}

void
ReadHead(ArchiveHandle *AH)
{
	char	   *errmsg;
	char		vmaj,
				vmin,
				vrev;
	int			fmt;

	/*
	 * 如果我们尚未读取头部，则读取。
	 *
	 * 注意：此代码必须与 _discoverArchiveFormat() 一致。
	 * 也许能找到统一这些情况的方法？
	 */
	if (!AH->readHeader)
	{
		char		tmpMag[7];

		AH->ReadBufPtr(AH, tmpMag, 5);

		if (strncmp(tmpMag, "PGDMP", 5) != 0)
			pg_fatal("did not find magic string in file header");
	}

	vmaj = AH->ReadBytePtr(AH);
	vmin = AH->ReadBytePtr(AH);

	if (vmaj > 1 || (vmaj == 1 && vmin > 0))	/* 版本 > 1.0 */
		vrev = AH->ReadBytePtr(AH);
	else
		vrev = 0;

	AH->version = MAKE_ARCHIVE_VERSION(vmaj, vmin, vrev);

	if (AH->version < K_VERS_1_0 || AH->version > K_VERS_MAX)
		pg_fatal("unsupported version (%d.%d) in file header",
				 vmaj, vmin);

	AH->intSize = AH->ReadBytePtr(AH);
	if (AH->intSize > 32)
		pg_fatal("sanity check on integer size (%lu) failed",
				 (unsigned long) AH->intSize);

	if (AH->intSize > sizeof(int))
		pg_log_warning("archive was made on a machine with larger integers, some operations might fail");

	if (AH->version >= K_VERS_1_7)
		AH->offSize = AH->ReadBytePtr(AH);
	else
		AH->offSize = AH->intSize;

	fmt = AH->ReadBytePtr(AH);

	if (AH->format != fmt)
		pg_fatal("expected format (%d) differs from format found in file (%d)",
				 AH->format, fmt);

	if (AH->version >= K_VERS_1_15)
		AH->compression_spec.algorithm = AH->ReadBytePtr(AH);
	else if (AH->version >= K_VERS_1_2)
	{
		/* 根据级别猜测压缩方法 */
		if (AH->version < K_VERS_1_4)
			AH->compression_spec.level = AH->ReadBytePtr(AH);
		else
			AH->compression_spec.level = ReadInt(AH);

		if (AH->compression_spec.level != 0)
			AH->compression_spec.algorithm = PG_COMPRESSION_GZIP;
	}
	else
		AH->compression_spec.algorithm = PG_COMPRESSION_GZIP;

	errmsg = supports_compression(AH->compression_spec);
	if (errmsg)
	{
		pg_log_warning("archive is compressed, but this installation does not support compression (%s) -- no data will be available",
					   errmsg);
		pg_free(errmsg);
	}

	if (AH->version >= K_VERS_1_4)
	{
		struct tm	crtm;

		crtm.tm_sec = ReadInt(AH);
		crtm.tm_min = ReadInt(AH);
		crtm.tm_hour = ReadInt(AH);
		crtm.tm_mday = ReadInt(AH);
		crtm.tm_mon = ReadInt(AH);
		crtm.tm_year = ReadInt(AH);
		crtm.tm_isdst = ReadInt(AH);

		/*
		 * 较新版本的 glibc 如果 tm_isdst 与当前时区不一致，
		 * 例如 TZ=UTC 时 tm_isdst = 1，则 mktime() 会报告失败。
		 * 在不同时区设置下恢复归档时这是一个问题。
		 * 如果遇到失败，使用 tm_isdst 设置为 -1（"不知道"）再试一次。
		 *
		 * XXX 无论是否使用此技巧，当前时区与 pg_dump 时区不同时，
		 * 我们都会错误地重建 createDate。下次升级归档版本时，
		 * 我们应该刷新此表示并存储一个简单的
		 * 自纪元以来的秒数时间戳。
		 *
		 *
		 */
		AH->createDate = mktime(&crtm);
		if (AH->createDate == (time_t) -1)
		{
			crtm.tm_isdst = -1;
			AH->createDate = mktime(&crtm);
			if (AH->createDate == (time_t) -1)
				pg_log_warning("invalid creation date in header");
		}
	}

	if (AH->version >= K_VERS_1_4)
	{
		AH->archdbname = ReadStr(AH);
	}

	if (AH->version >= K_VERS_1_10)
	{
		AH->archiveRemoteVersion = ReadStr(AH);
		AH->archiveDumpVersion = ReadStr(AH);
	}
}


/*
 * checkSeek
 *	  检查 ftell/fseek 是否可以执行。
 */
bool
checkSeek(FILE *fp)
{
	pgoff_t		tpos;

	/* 检查 ftello 是否在此文件上工作 */
	tpos = ftello(fp);
	if (tpos < 0)
		return false;

	/*
	 * 也检查 fseeko(SEEK_SET) 是否工作。注意：我们过去尝试用
	 * fseeko(fp, 0, SEEK_CUR) 来测试。但某些平台将其视为
	 * 成功的无操作，即使在不可定位的文件上也是如此。
	 */
	if (fseeko(fp, tpos, SEEK_SET) != 0)
		return false;

	return true;
}


/*
 * dumpTimestamp — 输出时间戳
 */
static void
dumpTimestamp(ArchiveHandle *AH, const char *msg, time_t tim)
{
	char		buf[64];

	if (strftime(buf, sizeof(buf), PGDUMP_STRFTIME_FMT, localtime(&tim)) != 0)
		ahprintf(AH, "-- %s %s\n\n", msg, buf);
}

/*
 * 并行恢复的主引擎。
 *
 * 并行恢复分三个阶段完成。在第一阶段，我们将处理所有允许在
 * RESTORE_PASS_MAIN 遍历中处理的 SECTION_PRE_DATA TOC 条目。
 * （实际上，除了 ACL 之外的所有 PRE_DATA 项目。）
 * 我们现在无法处理的项目添加到 pending_list 供后续阶段处理。
 *
 */
static void
restore_toc_entries_prefork(ArchiveHandle *AH, TocEntry *pending_list)
{
	bool		skipped_some;
	TocEntry   *next_work_item;

	pg_log_debug("entering restore_toc_entries_prefork");

	/* 调整依赖信息 */
	fix_dependencies(AH);

	/*
	 * 在父进程中以单个连接完成所有早期工作。              
	 * 并行运行没什么意义，单连接实际反而更快，       
	 * 因为我们避免了所有连接与设置开销。此外，pre-9.2  
	 * 的 pg_dump 版本在显示 SECTION_PRE_DATA 条目的全部      
	 * 依赖方面做得不佳，所以我们不冒险                      
	 * 乱序处理它们。                        
	 *
	 * 我们无法立即处理的内容会被加入 pending_list。     
	 * 注意：我们尚未过滤掉不会恢复的条目。它们可能     
	 * 参与连接应恢复条目的依赖链，所以实际处理它们 
	 * 之前我们都把它们视为                                        
	 * live 
	 *
	 * 注意：从 9.2 起，应能保证所有 PRE_DATA 条目都出现在  
	 * DATA 条目之前，所有 DATA 条目在 POST_DATA 条目之前。   
	 * 但在较旧归档中不一定成立，而且无论如何，使用   
	 * 列表文件会破坏该顺序（见 SortTocFromFile）。故此     
	 * 此循环不能假设它成立。     
	 */
	AH->restorePass = RESTORE_PASS_MAIN;
	skipped_some = false;
	for (next_work_item = AH->toc->next; next_work_item != AH->toc; next_work_item = next_work_item->next)
	{
		bool		do_now = true;

		if (next_work_item->section != SECTION_PRE_DATA)
		{
			/* DATA 和 POST_DATA 项目暂时忽略 */
			if (next_work_item->section == SECTION_DATA ||
				next_work_item->section == SECTION_POST_DATA)
			{
				do_now = false;
				skipped_some = true;
			}
			else
			{
				/*
				 * SECTION_NONE 项目（如注释）如果我们仍在归档的
				 * PRE_DATA 部分，现在可以处理。一旦我们跳过了
				 * 任何项目，我们必须考虑注释的依赖是否满足，
				 * 因此现在跳过它。
				 */
				if (skipped_some)
					do_now = false;
			}
		}

		/*
		 * 也跳过需要强制到后续遍历的项目。在这种情况下
		 * 不需要设置 skipped_some，因为假设没有主遍历
		 * 项目会依赖于这些项目。
		 */
		if (_tocEntryRestorePass(next_work_item) != RESTORE_PASS_MAIN)
			do_now = false;

		if (do_now)
		{
			/* 好，恢复该项目并更新其依赖 */
			pg_log_info("processing item %d %s %s",
						next_work_item->dumpId,
						next_work_item->desc, next_work_item->tag);

			(void) restore_toc_entry(AH, next_work_item, false);

			/* 减少依赖，但不要将任何内容移到 ready_list */
			reduce_dependencies(AH, next_work_item, NULL);
		}
		else
		{
			/* 不，因此将其添加到 pending_list */
			pending_list_append(pending_list, next_work_item);
		}
	}

	/*
	 * 现在关闭父连接以准备并行步骤。我们这样做
	 * 主要是为了确保不超过指定的并行
	 * 连接数。
	 */
	DisconnectDatabase(&AH->public);

	/* 清除旧连接的任何临时状态 */
	free(AH->currUser);
	AH->currUser = NULL;
	free(AH->currSchema);
	AH->currSchema = NULL;
	free(AH->currTablespace);
	AH->currTablespace = NULL;
	free(AH->currTableAm);
	AH->currTableAm = NULL;
}

/*
 * 并行恢复的主引擎。
 *
 * 并行恢复分三个阶段完成。在第二阶段，
 * 我们通过将条目分派到并行工作子进程来处理它们
 * （Unix 上的进程，Windows 上的线程），每个子进程
 * 单独连接到数据库。尊重条目间的依赖关系，
 * 也尊重 RestorePass 多遍历结构。当我们不能再
 * 使任何条目准备好处理时，我们退出。正常情况下，
 * 应该没有剩余内容；但如果有，第三阶段将清理。
 */
static void
restore_toc_entries_parallel(ArchiveHandle *AH, ParallelState *pstate,
							 TocEntry *pending_list)
{
	ParallelReadyList ready_list;
	TocEntry   *next_work_item;

	pg_log_debug("entering restore_toc_entries_parallel");

	/* 设置 ready_list，为所有已知的 TocEntry 留出足够空间 */
	ready_list_init(&ready_list, AH->tocCount);

	/*
	 * pending_list 包含我们需要恢复的所有项目。
	 * 将所有可立即处理的项目移到 ready_list。
	 * 在此设置之后，pending_list 是所有需要完成但            
	 * 被一个或多个依赖阻塞的内容，而 ready_list        
	 * 包含没有剩余依赖且可以在当前恢复遍历中
	 * 处理的项目。
	 */
	AH->restorePass = RESTORE_PASS_MAIN;
	move_to_ready_list(pending_list, &ready_list, AH->restorePass);

	/*
	 * 主父循环
	 *
	 * 持续运行直到没有工作进程正在运行且没有剩余工作
	 * 需要完成。注意不变量：在循环顶部，应始终有
	 * 至少一个工作进程可用于分派作业。
	 */
	pg_log_info("entering main parallel loop");

	for (;;)
	{
		/* 查找准备好分发到工作进程的项目 */
		next_work_item = pop_next_work_item(&ready_list, pstate);
		if (next_work_item != NULL)
		{
			/* 如果不恢复，不要浪费时间启动工作进程 */
			if ((next_work_item->reqs & (REQ_SCHEMA | REQ_DATA)) == 0)
			{
				pg_log_info("skipping item %d %s %s",
							next_work_item->dumpId,
							next_work_item->desc, next_work_item->tag);
				/* 更新其依赖，就像已完成一样 */
				reduce_dependencies(AH, next_work_item, &ready_list);
				/* 循环查看是否有其他内容可以分发 */
				continue;
			}

			pg_log_info("launching item %d %s %s",
						next_work_item->dumpId,
						next_work_item->desc, next_work_item->tag);

			/* 分发到某个工作进程 */
			DispatchJobForTocEntry(AH, pstate, next_work_item, ACT_RESTORE,
								   mark_restore_job_done, &ready_list);
		}
		else if (IsEveryWorkerIdle(pstate))
		{
			/*
			 * 没有就绪项目且没有工作进程在运行，因此我们已完成
			 * 当前遍历，或者可能整个流程。
			 */
			if (AH->restorePass == RESTORE_PASS_LAST)
				break;			/* 无法再进行并行处理 */

			/* 前进到下一个恢复遍历 */
			AH->restorePass++;
			/* 这可能允许一些内容变为就绪 */
			move_to_ready_list(pending_list, &ready_list, AH->restorePass);
			/* 循环查看是否有内容现在已就绪 */
			continue;
		}
		else
		{
			/*
			 * 我们没有就绪项目，但至少有一个子进程在工作，
			 * 因此等待某个子作业完成。
			 */
		}

		/*
		 * 在分派另一个作业之前，检查是否有任何作业已完成。
		 * 我们应该在每次循环时检查，以便尽快减少依赖。
		 * 如果本轮无法分派任何作业，等待直到某个工作进程
		 * 完成（并希望解除某个挂起项目的阻塞）。如果我们确实分派了
		 * 某个作业，一旦至少有一个空闲工作进程就继续。
		 * 注意，在任何情况下，当我们返回循环顶部时，保证
		 * 至少有一个空闲工作进程。这确保我们不会在
		 * DispatchJobForTocEntry 中阻塞，那将是不希望的：
		 * 我们宁愿推迟分派，直到我们看到已完成作业
		 * 解除了哪些阻塞。
		 *
		 */
		WaitForWorkers(AH, pstate,
					   next_work_item ? WFW_ONE_IDLE : WFW_GOT_STATUS);
	}

	/* 现在 ready_list 中应该没有内容。 */
	Assert(ready_list.first_te > ready_list.last_te);

	ready_list_free(&ready_list);

	pg_log_info("finished main parallel loop");
}

/*
 * 并行恢复的主引擎。
 *
 * 并行恢复分三个阶段完成。在第三阶段，
 * 我们通过串行处理来清理任何剩余的 TOC 条目。
 * 此阶段通常应该没有工作，但如果由于循环依赖
 * 或类似情况卡住了，这至少提供了一些
 * 成功完成恢复的机会。
 */
static void
restore_toc_entries_postfork(ArchiveHandle *AH, TocEntry *pending_list)
{
	RestoreOptions *ropt = AH->public.ropt;
	TocEntry   *te;

	pg_log_debug("entering restore_toc_entries_postfork");

	/*
	 * 现在重新连接单个父连接。
	 */
	ConnectDatabase((Archive *) AH, &ropt->cparams, true);

	/* 重新建立固定状态 */
	_doSetFixedOutputState(AH);

	/*
	 * 确保没有因循环依赖或其他病理条件
	 * 而剩余的工作。如果有，在单个父连接中完成。
	 * 我们不担心 RestorePass 排序；很可能我们已经违反了。
	 *
	 */
	for (te = pending_list->pending_next; te != pending_list; te = te->pending_next)
	{
		pg_log_info("processing missed item %d %s %s",
					te->dumpId, te->desc, te->tag);
		(void) restore_toc_entry(AH, te, false);
	}
}

/*
 * 检查 te1 是否对 te2 也需要的项目有排他锁要求，
 * 无论 te2 的要求是否为排他锁。
 */
static bool
has_lock_conflicts(TocEntry *te1, TocEntry *te2)
{
	int			j,
				k;

	for (j = 0; j < te1->nLockDeps; j++)
	{
		for (k = 0; k < te2->nDeps; k++)
		{
			if (te1->lockDeps[j] == te2->dependencies[k])
				return true;
		}
	}
	return false;
}


/*
 * 初始化挂起项目列表的头部。
 *
 * 这是一个循环列表，以虚拟 TocEntry 作为头部，就像
 * 主 TOC 列表一样；但我们使用单独的列表链接，
 * 以便条目可以同时在主 TOC 列表和挂起列表中。
 */
static void
pending_list_header_init(TocEntry *l)
{
	l->pending_prev = l->pending_next = l;
}

/* 将 te 追加到以 l 为头的 pending-list 的末尾 */
static void
pending_list_append(TocEntry *l, TocEntry *te)
{
	te->pending_prev = l->pending_prev;
	l->pending_prev->pending_next = te;
	l->pending_prev = te;
	te->pending_next = l;
}

/* 将 te 从 pending-list 中移除 */
static void
pending_list_remove(TocEntry *te)
{
	te->pending_prev->pending_next = te->pending_next;
	te->pending_next->pending_prev = te->pending_prev;
	te->pending_prev = NULL;
	te->pending_next = NULL;
}


/*
 * 初始化 ready_list，为最多 tocCount 个条目留出空间。
 */
static void
ready_list_init(ParallelReadyList *ready_list, int tocCount)
{
	ready_list->tes = (TocEntry **)
		pg_malloc(tocCount * sizeof(TocEntry *));
	ready_list->first_te = 0;
	ready_list->last_te = -1;
	ready_list->sorted = false;
}

/*
 * 释放 ready_list 的存储空间。
 */
static void
ready_list_free(ParallelReadyList *ready_list)
{
	pg_free(ready_list->tes);
}

/* 将 te 添加到 ready_list */
static void
ready_list_insert(ParallelReadyList *ready_list, TocEntry *te)
{
	ready_list->tes[++ready_list->last_te] = te;
	/* 列表（可能）不再排序。 */
	ready_list->sorted = false;
}

/* 移除 ready_list 中的第 i 个条目 */
static void
ready_list_remove(ParallelReadyList *ready_list, int i)
{
	int			f = ready_list->first_te;

	Assert(i >= f && i <= ready_list->last_te);

	/*
	 * 在要移除的项目是第一个就绪条目的典型情况下，
	 * 我们只需要递增 first_te 来移除它。否则，移动
	 * 它之前的条目来压缩列表。（这保持了排序性，
	 * 如果有的话。）我们也可以移动 i 之后的条目，但
	 * 那些条目通常更多。
	 */
	if (i > f)
	{
		TocEntry  **first_te_ptr = &ready_list->tes[f];

		memmove(first_te_ptr + 1, first_te_ptr, (i - f) * sizeof(TocEntry *));
	}
	ready_list->first_te++;
}

/* 将 ready_list 排序为所需顺序 */
static void
ready_list_sort(ParallelReadyList *ready_list)
{
	if (!ready_list->sorted)
	{
		int			n = ready_list->last_te - ready_list->first_te + 1;

		if (n > 1)
			qsort(ready_list->tes + ready_list->first_te, n,
				  sizeof(TocEntry *),
				  TocEntrySizeCompare);
		ready_list->sorted = true;
	}
}

/* 按 dataLength 排序 TocEntry 的 qsort 比较器 */
static int
TocEntrySizeCompare(const void *p1, const void *p2)
{
	const TocEntry *te1 = *(const TocEntry *const *) p1;
	const TocEntry *te2 = *(const TocEntry *const *) p2;

	/* 按 dataLength 降序排序 */
	if (te1->dataLength > te2->dataLength)
		return -1;
	if (te1->dataLength < te2->dataLength)
		return 1;

	/* 对于相等的 dataLength，按 dumpId 排序以保持稳定性 */
	if (te1->dumpId < te2->dumpId)
		return -1;
	if (te1->dumpId > te2->dumpId)
		return 1;

	return 0;
}


/*
 * 将所有立即可用的项目从 pending_list 移到 ready_list。
 *
 * 如果项目没有剩余依赖且属于当前恢复遍历，
 * 则认为它们已就绪。（另见 reduce_dependencies，
 * 它一次一个地应用相同的逻辑。）
 */
static void
move_to_ready_list(TocEntry *pending_list,
				   ParallelReadyList *ready_list,
				   RestorePass pass)
{
	TocEntry   *te;
	TocEntry   *next_te;

	for (te = pending_list->pending_next; te != pending_list; te = next_te)
	{
		/* 在可能从列表中移除 te 之前必须保存列表链接 */
		next_te = te->pending_next;

		if (te->depCount == 0 &&
			_tocEntryRestorePass(te) == pass)
		{
			/* 从 pending_list 中移除 ... */
			pending_list_remove(te);
			/* ... 并添加到 ready_list */
			ready_list_insert(ready_list, te);
		}
	}
}

/*
 * 查找下一个能够现在运行的工作项目（如果有），
 * 并将其从 ready_list 中移除。
 *
 * 返回该项目，如果没有可运行的项目则返回 NULL。
 *
 * 要符合条件，项目必须没有剩余依赖，
 * 且没有与当前正在运行的项目不兼容的
 * 锁要求。ready_list 中的项目已知没有剩余依赖，
 * 但我们必须检查锁冲突。
 */
static TocEntry *
pop_next_work_item(ParallelReadyList *ready_list,
				   ParallelState *pstate)
{
	/*
	 * 对 ready_list 排序，以便我们首先处理较大的作业。
	 */
	ready_list_sort(ready_list);

	/*
	 * 搜索 ready_list 直到找到合适的项目。
	 */
	for (int i = ready_list->first_te; i <= ready_list->last_te; i++)
	{
		TocEntry   *te = ready_list->tes[i];
		bool		conflicts = false;

		/*
		 * 检查该项目是否需要对当前正在运行的项目
		 * 也需要锁的某个内容加排他锁，反之亦然。
		 * 如果是，我们不希望将它们安排在一起。
		 */
		for (int k = 0; k < pstate->numWorkers; k++)
		{
			TocEntry   *running_te = pstate->te[k];

			if (running_te == NULL)
				continue;
			if (has_lock_conflicts(te, running_te) ||
				has_lock_conflicts(running_te, te))
			{
				conflicts = true;
				break;
			}
		}

		if (conflicts)
			continue;

		/* 通过了所有测试，因此此项可以运行 */
		ready_list_remove(ready_list, i);
		return te;
	}

	pg_log_debug("no item ready");
	return NULL;
}


/*
 * 与其他项目并行恢复单个 TOC 项目
 *
 * 这在工作进程中运行，即在 Windows 上的线程中或在其他平台上在单独的进程中。
 * 一个工作进程在并行备份或恢复期间执行多个这样的工作项目。
 * 一旦我们在此终止并报告工作完成，领导进程将分配新的工作项目给我们。
 *
 */
int
parallel_restore(ArchiveHandle *AH, TocEntry *te)
{
	int			status;

	Assert(AH->connection != NULL);

	/* 只计数与此 TOC 条目相关的错误 */
	AH->public.n_errors = 0;

	/* 恢复 TOC 项目 */
	status = restore_toc_entry(AH, te, true);

	return status;
}


/*
 * 在步骤并行恢复后在领导进程中调用的
 * 回调函数。
 *
 * 更新状态并减少任何依赖项目的依赖计数。
 */
static void
mark_restore_job_done(ArchiveHandle *AH,
					  TocEntry *te,
					  int status,
					  void *callback_data)
{
	ParallelReadyList *ready_list = (ParallelReadyList *) callback_data;

	pg_log_info("finished item %d %s %s",
				te->dumpId, te->desc, te->tag);

	if (status == WORKER_CREATE_DONE)
		mark_create_done(AH, te);
	else if (status == WORKER_INHIBIT_DATA)
	{
		inhibit_data_for_failed_table(AH, te);
		AH->public.n_errors++;
	}
	else if (status == WORKER_IGNORED_ERRORS)
		AH->public.n_errors++;
	else if (status != 0)
		pg_fatal("worker process failed: exit code %d",
				 status);

	reduce_dependencies(AH, te, ready_list);
}


/*
 * 将依赖信息处理为对并行恢复有用的形式。
 *
 * 此函数负责修复一些缺失或设计不良的依赖，
 * 然后准备将在主并行恢复逻辑中使用的
 * 辅助数据结构，包括：
 * 1. 我们构建传入依赖导出 ID 的 revDeps[] 数组。
 * 2. 我们设置 depCount 字段，即每个 TOC 条目的
 *    尚未处理的依赖数。
 *
 * 我们还识别锁定依赖，以便避免尝试
 * 同时调度冲突的项目。
 */
static void
fix_dependencies(ArchiveHandle *AH)
{
	TocEntry   *te;
	int			i;

	/*
	 * 初始化 depCount/revDeps/nRevDeps 字段，并确保
	 * TOC 项目标记为不在任何并行处理列表中。
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		te->depCount = te->nDeps;
		te->revDeps = NULL;
		te->nRevDeps = 0;
		te->pending_prev = NULL;
		te->pending_next = NULL;
	}

	/*
	 * 显示为依赖表的 POST_DATA 项目需要重新指向
	 * 改为依赖该表的数据。这确保它们在
	 * 数据加载完成之前不会被调度。
	 */
	repoint_table_dependencies(AH);

	/*
	 * Pre-8.4 版本的 pg_dump 忽略了设置从
	 * BLOB COMMENTS 到 BLOBS 的依赖。应对。
	 * （我们假设此类文件中只有一个 BLOBS 和一个 BLOB COMMENTS。）
	 */
	if (AH->version < K_VERS_1_11)
	{
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if (strcmp(te->desc, "BLOB COMMENTS") == 0 && te->nDeps == 0)
			{
				TocEntry   *te2;

				for (te2 = AH->toc->next; te2 != AH->toc; te2 = te2->next)
				{
					if (strcmp(te2->desc, "BLOBS") == 0)
					{
						te->dependencies = (DumpId *) pg_malloc(sizeof(DumpId));
						te->dependencies[0] = te2->dumpId;
						te->nDeps++;
						te->depCount++;
						break;
					}
				}
				break;
			}
		}
	}

	/*
	 * 此时我们开始构建 revDeps 反向依赖数组，
	 * 因此所有依赖更改必须完成。
	 */

	/*
	 * 统计每个项目的传入依赖。此外，依赖列表
	 * 可能列出的项目根本不在归档中（在 9.2 及更高版本中
	 * 不应发生，但在较旧的归档中非常可能）。
	 * 从 depCounts 中减去此类项目。
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		for (i = 0; i < te->nDeps; i++)
		{
			DumpId		depid = te->dependencies[i];

			if (depid <= AH->maxDumpId && AH->tocsByDumpId[depid] != NULL)
				AH->tocsByDumpId[depid]->nRevDeps++;
			else
				te->depCount--;
		}
	}

	/*
	 * 为 revDeps[] 数组分配空间，并重置 nRevDeps
	 * 以便在下面将其用作计数器。
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->nRevDeps > 0)
			te->revDeps = (DumpId *) pg_malloc(te->nRevDeps * sizeof(DumpId));
		te->nRevDeps = 0;
	}

	/*
	 * 构建传入依赖导出 ID 的 revDeps[] 数组。
	 * 这必须与上面的循环一致。
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		for (i = 0; i < te->nDeps; i++)
		{
			DumpId		depid = te->dependencies[i];

			if (depid <= AH->maxDumpId && AH->tocsByDumpId[depid] != NULL)
			{
				TocEntry   *otherte = AH->tocsByDumpId[depid];

				otherte->revDeps[otherte->nRevDeps++] = te->dumpId;
			}
		}
	}

	/*
	 * 最后，计算锁定依赖。
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		te->lockDeps = NULL;
		te->nLockDeps = 0;
		identify_locking_dependencies(AH, te);
	}
}

/*
 * 将表项目上的依赖改为依赖表数据项目，
 * 但仅限于 POST_DATA 项目。
 *
 * 此外，对于任何具有此类依赖的项目，将其 dataLength
 * 设置为其依赖的表数据项目的最大 dataLength。
 * 这确保并行恢复将优先处理较大的作业（索引构建、
 * FK 约束检查等）而不是较小的作业，避免在恢复
 * 结束时只有一个活动作业在处理大表。
 */
static void
repoint_table_dependencies(ArchiveHandle *AH)
{
	TocEntry   *te;
	int			i;
	DumpId		olddep;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->section != SECTION_POST_DATA)
			continue;
		for (i = 0; i < te->nDeps; i++)
		{
			olddep = te->dependencies[i];
			if (olddep <= AH->maxDumpId &&
				AH->tableDataId[olddep] != 0)
			{
				DumpId		tabledataid = AH->tableDataId[olddep];
				TocEntry   *tabledatate = AH->tocsByDumpId[tabledataid];

				te->dependencies[i] = tabledataid;
				te->dataLength = Max(te->dataLength, tabledatate->dataLength);
				pg_log_debug("transferring dependency %d -> %d to %d",
							 te->dumpId, olddep, tabledataid);
			}
		}
	}
}

/*
 * 确定为了恢复给定 TOC 条目我们需要对哪些对象
 * 加排他锁（*除了*TOC 条目本身标识的那个）。
 * 将它们的导出 ID 记录在条目的 lockDeps[] 数组中。
 */
static void
identify_locking_dependencies(ArchiveHandle *AH, TocEntry *te)
{
	DumpId	   *lockids;
	int			nlockids;
	int			i;

	/*
	 * 我们只关心 POST_DATA 项目。PRE_DATA 项目不
	 * 并行运行，且 DATA 项目假设都是独立的。
	 */
	if (te->section != SECTION_POST_DATA)
		return;

	/* 如果完全没有依赖则快速退出 */
	if (te->nDeps == 0)
		return;

	/*
	 * 大多数 POST_DATA 项目是 ALTER TABLE 或类似的东西，
	 * 因此需要排他锁。然而，我们知道 CREATE INDEX
	 * 不需要。（也许有一天索引创建 CONSTRAINT 也会
	 * 归入此类 ... 但今天不是那天。）
	 */
	if (strcmp(te->desc, "INDEX") == 0)
		return;

	/*
	 * 我们假设该条目需要对其依赖中列出的每个
	 * TABLE 或 TABLE DATA 项目加排他锁。
	 * 最初所有这些都可能是 TABLE 项目，但
	 * repoint_table_dependencies 会将其重新指向
	 * TABLE DATA 项目（如果存在的话）（在仅模式导出中
	 * 可能不存在）。注意我们在此处理的所有条目都是
	 * POST_DATA；否则对表的依赖和对表数据的依赖之间
	 * 可能存在显著差异，因此需要更仔细的分析。
	 */
	lockids = (DumpId *) pg_malloc(te->nDeps * sizeof(DumpId));
	nlockids = 0;
	for (i = 0; i < te->nDeps; i++)
	{
		DumpId		depid = te->dependencies[i];

		if (depid <= AH->maxDumpId && AH->tocsByDumpId[depid] != NULL &&
			((strcmp(AH->tocsByDumpId[depid]->desc, "TABLE DATA") == 0) ||
			 strcmp(AH->tocsByDumpId[depid]->desc, "TABLE") == 0))
			lockids[nlockids++] = depid;
	}

	if (nlockids == 0)
	{
		free(lockids);
		return;
	}

	te->lockDeps = pg_realloc(lockids, nlockids * sizeof(DumpId));
	te->nLockDeps = nlockids;
}

/*
 * 从依赖它的项目的 depCounts 中移除指定的 TOC 条目，
 * 从而可能使它们变为可运行。任何变为就绪的挂起项目
 * 应移到 ready_list（如果提供了的话）。
 */
static void
reduce_dependencies(ArchiveHandle *AH, TocEntry *te,
					ParallelReadyList *ready_list)
{
	int			i;

	pg_log_debug("reducing dependencies for %d", te->dumpId);

	for (i = 0; i < te->nRevDeps; i++)
	{
		TocEntry   *otherte = AH->tocsByDumpId[te->revDeps[i]];

		Assert(otherte->depCount > 0);
		otherte->depCount--;

		/*
		 * 如果没有剩余依赖且它属于当前恢复遍历，
		 * 且它当前是挂起列表的成员（需要此检查以防止
		 * 在列表文件强制乱序恢复的某些情况下
		 * 重复恢复），则它已就绪。
		 * 然而，如果 ready_list == NULL，则调用者不想要
		 * 任何列表成员关系更改。
		 */
		if (otherte->depCount == 0 &&
			_tocEntryRestorePass(otherte) == AH->restorePass &&
			otherte->pending_prev != NULL &&
			ready_list != NULL)
		{
			/* 从 pending_list 中移除 ...  */
			pending_list_remove(otherte);
			/* ... 并添加到 ready_list */
			ready_list_insert(ready_list, otherte);
		}
	}
}

/*
 * 在与给定 TABLE 成员对应的 DATA 成员上
 * 设置已创建标志
 */
static void
mark_create_done(ArchiveHandle *AH, TocEntry *te)
{
	if (AH->tableDataId[te->dumpId] != 0)
	{
		TocEntry   *ted = AH->tocsByDumpId[AH->tableDataId[te->dumpId]];

		ted->created = true;
	}
}

/*
 * 将与给定 TABLE 成员对应的 DATA 成员
 * 标记为不需要
 */
static void
inhibit_data_for_failed_table(ArchiveHandle *AH, TocEntry *te)
{
	pg_log_info("table \"%s\" could not be created, will not restore its data",
				te->tag);

	if (AH->tableDataId[te->dumpId] != 0)
	{
		TocEntry   *ted = AH->tocsByDumpId[AH->tableDataId[te->dumpId]];

		ted->reqs = 0;
	}
}

/*
 * 并行恢复中使用的克隆和反克隆例程。
 *
 * 克隆的结构足以确保不同线程之间
 * 没有冲突，每个线程都有自己的克隆。
 */
ArchiveHandle *
CloneArchive(ArchiveHandle *AH)
{
	ArchiveHandle *clone;

	/* 制作"扁平"副本 */
	clone = (ArchiveHandle *) pg_malloc(sizeof(ArchiveHandle));
	memcpy(clone, AH, sizeof(ArchiveHandle));

	/* 处理格式无关的字段 */
	memset(&(clone->sqlparse), 0, sizeof(clone->sqlparse));

	/* 克隆体将有自己的连接，因此忽略连接状态 */
	clone->connection = NULL;
	clone->connCancel = NULL;
	clone->currUser = NULL;
	clone->currSchema = NULL;
	clone->currTableAm = NULL;
	clone->currTablespace = NULL;

	/* savedPassword 必须是本地的，以防连接时更改 */
	if (clone->savedPassword)
		clone->savedPassword = pg_strdup(clone->savedPassword);

	/* 克隆体也有自己的错误计数 */
	clone->public.n_errors = 0;

	/*
	 * 将我们的新克隆对象连接到数据库，使用与原始连接
	 * 相同的连接参数。
	 */
	ConnectDatabase((Archive *) clone, &clone->public.ropt->cparams, true);

	/* 重新建立固定状态 */
	if (AH->mode == archModeRead)
		_doSetFixedOutputState(clone);
	/* 在写入情况下，setupDumpWorker 将修复连接状态 */

	/* 让格式特定的代码也有机会 */
	clone->ClonePtr(clone);

	Assert(clone->connection != NULL);
	return clone;
}

/*
 * 释放克隆本地的存储空间。
 *
 * 注意：我们假设任何克隆本地的连接已关闭。
 */
void
DeCloneArchive(ArchiveHandle *AH)
{
	/* 不应有打开的数据库连接 */
	Assert(AH->connection == NULL);

	/* 清除格式特定的状态 */
	AH->DeClonePtr(AH);

	/* 清除 CloneArchive 分配的状态 */
	if (AH->sqlparse.curCmd)
		destroyPQExpBuffer(AH->sqlparse.curCmd);

	/* 清除任何连接本地状态 */
	free(AH->currUser);
	free(AH->currSchema);
	free(AH->currTablespace);
	free(AH->currTableAm);
	free(AH->savedPassword);

	free(AH);
}
