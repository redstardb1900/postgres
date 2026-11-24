/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  拍摄基备份并流式传送到备库的代码
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "access/xlog_internal.h"
#include "access/xlogbackup.h"
#include "backup/backup_manifest.h"
#include "backup/basebackup.h"
#include "backup/basebackup_sink.h"
#include "backup/basebackup_target.h"
#include "commands/defrem.h"
#include "common/compression.h"
#include "common/file_perm.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtar.h"
#include "port.h"
#include "postmaster/syslogger.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/reinit.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

/*
 * 一条 CopyData 消息想发送多少数据？注意这也
 * 可能导致按此大小分块读取底层文件。
 *
 *
 * 注意：缓冲大小须为系统块大小的倍数；若系统
 * 块更大则改用之，而非我们偏好的大小。
 */
#define SINK_BUFFER_LENGTH			Max(32768, BLCKSZ)

typedef struct
{
	const char *label;
	bool		progress;
	bool		fastcheckpoint;
	bool		nowait;
	bool		includewal;
	uint32		maxrate;
	bool		sendtblspcmapfile;
	bool		send_to_client;
	bool		use_copytblspc;
	BaseBackupTargetHandle *target_handle;
	backup_manifest_option manifest;
	pg_compress_algorithm compression;
	pg_compress_specification compression_specification;
	pg_checksum_type manifest_checksum_type;
} basebackup_options;

static int64 sendTablespace(bbsink *sink, char *path, char *spcoid, bool sizeonly,
							struct backup_manifest_info *manifest);
static int64 sendDir(bbsink *sink, const char *path, int basepathlen, bool sizeonly,
					 List *tablespaces, bool sendtblspclinks,
					 backup_manifest_info *manifest, const char *spcoid);
static bool sendFile(bbsink *sink, const char *readfilename, const char *tarfilename,
					 struct stat *statbuf, bool missing_ok, Oid dboid,
					 backup_manifest_info *manifest, const char *spcoid);
static void sendFileWithContent(bbsink *sink, const char *filename,
								const char *content,
								backup_manifest_info *manifest);
static int64 _tarWriteHeader(bbsink *sink, const char *filename,
							 const char *linktarget, struct stat *statbuf,
							 bool sizeonly);
static void _tarWritePadding(bbsink *sink, int len);
static void convert_link_to_directory(const char *pathbuf, struct stat *statbuf);
static void perform_base_backup(basebackup_options *opt, bbsink *sink);
static void parse_basebackup_options(List *options, basebackup_options *opt);
static int	compareWalFileNames(const ListCell *a, const ListCell *b);
static bool is_checksummed_file(const char *fullpath, const char *filename);
static int	basebackup_read_file(int fd, char *buf, size_t nbytes, off_t offset,
								 const char *filename, bool partial_read_ok);

/* 当前进行中的备份是否在恢复模式下发起？ */
static bool backup_started_in_recovery = false;

/* 基备份期间校验和失败总数。 */
static long long int total_checksum_failures;

/* 不校验校验和。 */
static bool noverify_checksums = false;

/*
 * 排除列表元素定义，用于校验和验证或基备份路径。
 * name 是要检查排除的文件或路径名。若
 * match_prefix 为真，凡以该名为前缀的项皆排除。
 * 匹配该名前缀者皆排除。
 */
struct exclude_list_item
{
	const char *name;
	bool		match_prefix;
};

/*
 * 这些目录内容在服务器启动时被删或重建，故不纳入
 * 备份。目录本身保留并作空目录纳入以保留权限。
 * 保留为空以保留权限。
 *
 * 注意：本列表应与 pg_rewind 过滤列表保持同步，见
 * filemap.c.
 */
static const char *const excludeDirContents[] =
{
	/*
	 * 跳过临时统计文件。PG_STAT_TMP_DIR 必须跳过，因
	 * pg_stat_statements 等扩展在此存数据。
	 */
	PG_STAT_TMP_DIR,

	/*
	 * 一般不值得备份本目录内容，即便要恢复到
	 * 另一主库。详见 backup.sgml。
	 * 详见 backup.sgml。
	 */
	"pg_replslot",

	/* 启动时清空内容，见 dsm_cleanup_for_mmap()。 */
	PG_DYNSHMEM_DIR,

	/* 启动时清空内容，见 AsyncShmemInit()。 */
	"pg_notify",

	/*
	 * 旧内容会载入以供可能调试，但正常运行不需要，见
	 * SerialInit()。
	 */
	"pg_serial",

	/* 启动时清空内容，见 DeleteAllExportedSnapshotFiles()。 */
	"pg_snapshots",

	/* 启动时清零内容，见 StartupSUBTRANS()。 */
	"pg_subtrans",

	/* 列表终 */
	NULL
};

/*
 * 从备份中排除的文件列表。
 */
static const struct exclude_list_item excludeFiles[] =
{
	/* 跳过自动配置临时文件 */
	{PG_AUTOCONF_FILENAME ".tmp", false},

	/* 跳过当前日志临时文件 */
	{LOG_METAINFO_DATAFILE_TMP, false},

	/*
	 * 跳过关系缓存，因其在启动时重建。这包括
	 * 临时文件。
	 */
	{RELCACHE_INIT_FILENAME, true},

	/*
	 * 能做在线备份的运行中集簇不应有 backup_label 与
	 * tablespace_map，但以防万一仍排除它们。
	 */
	{BACKUP_LABEL_FILE, false},
	{TABLESPACE_MAP, false},

	/*
	 * 若有 backup_manifest，它属于启动本服务器的
	 * 备份；*不*适用于本次。若用户需要，我
	 * 们的 backup_manifest 会另行注入备份。
	 *
	 */
	{"backup_manifest", false},

	{"postmaster.pid", false},
	{"postmaster.opts", false},

	/* 列表终 */
	{NULL, false}
};

/*
 * 从校验和验证中排除的文件列表。
 *
 * 注意：应与 pg_checksums.c 所含保持同步。
 *
 */
static const struct exclude_list_item noChecksumFiles[] = {
	{"pg_control", false},
	{"pg_filenode.map", false},
	{"pg_internal.init", true},
	{"PG_VERSION", false},
#ifdef EXEC_BACKEND
	{"config_exec_params", true},
#endif
	{NULL, false}
};

/*
 * 实际为指定表空间做基备份。
 *
 * 拆出主要是为避免较笨 gcc 抱怨"变量可能被 longjmp
 * 破坏"。
 */
static void
perform_base_backup(basebackup_options *opt, bbsink *sink)
{
	bbsink_state state;
	XLogRecPtr	endptr;
	TimeLineID	endtli;
	backup_manifest_info manifest;
	BackupState *backup_state;
	StringInfo	tablespace_map;

	/* 就目前所知的初始备份状态。 */
	state.tablespaces = NIL;
	state.tablespace_num = 0;
	state.bytes_done = 0;
	state.bytes_total = 0;
	state.bytes_total_is_valid = false;

	/* 将用 BufFile，故需要 ResourceOwner */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "base backup");

	backup_started_in_recovery = RecoveryInProgress();

	InitializeBackupManifest(&manifest, opt->manifest,
							 opt->manifest_checksum_type);

	total_checksum_failures = 0;

	/* 分配备份相关变量。 */
	backup_state = (BackupState *) palloc0(sizeof(BackupState));
	tablespace_map = makeStringInfo();

	basebackup_progress_wait_checkpoint();
	do_pg_backup_start(opt->label, opt->fastcheckpoint, &state.tablespaces,
					   backup_state, tablespace_map);

	state.startptr = backup_state->startpoint;
	state.starttli = backup_state->starttli;

	/*
	 * 一旦调用了 do_pg_backup_start，须确保任何失败都会中止
	 * 备份，以免"泄漏"备份计数。故 do_pg_backup_start()
	 * 与 do_pg_backup_stop() 结束之间的*全部*功能都应在
	 * 错误清理块内！
	 */

	PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, BoolGetDatum(false));
	{
		ListCell   *lc;
		tablespaceinfo *newti;

		/* 在末尾为基目录加一个节点 */
		newti = palloc0(sizeof(tablespaceinfo));
		newti->size = -1;
		state.tablespaces = lappend(state.tablespaces, newti);

		/*
		 * 把各表空间大小相加，计算备份总大小
		 * 表空间
		 */
		if (opt->progress)
		{
			basebackup_progress_estimate_backup_size();

			foreach(lc, state.tablespaces)
			{
				tablespaceinfo *tmp = (tablespaceinfo *) lfirst(lc);

				if (tmp->path == NULL)
					tmp->size = sendDir(sink, ".", 1, true, state.tablespaces,
										true, NULL, NULL);
				else
					tmp->size = sendTablespace(sink, tmp->path, tmp->oid, true,
											   NULL);
				state.bytes_total += tmp->size;
			}
			state.bytes_total_is_valid = true;
		}

		/* 通知基备份 sink 备份开始 */
		bbsink_begin_backup(sink, &state, SINK_BUFFER_LENGTH);

		/* 逐个发送我们的表空间 */
		foreach(lc, state.tablespaces)
		{
			tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);

			if (ti->path == NULL)
			{
				struct stat statbuf;
				bool		sendtblspclinks = true;
				char	   *backup_label;

				bbsink_begin_archive(sink, "base.tar");

				/* 在主 tar 中先纳入 backup_label…… */
				backup_label = build_backup_content(backup_state, false);
				sendFileWithContent(sink, BACKUP_LABEL_FILE,
									backup_label, &manifest);
				pfree(backup_label);

				/* 然后是 tablespace_map（若需）…… */
				if (opt->sendtblspcmapfile)
				{
					sendFileWithContent(sink, TABLESPACE_MAP,
										tablespace_map->data, &manifest);
					sendtblspclinks = false;
				}

				/* 然后是大量文件…… */
				sendDir(sink, ".", 1, false, state.tablespaces,
						sendtblspclinks, &manifest, NULL);

				/* ……最后才是 pg_control。 */
				if (lstat(XLOG_CONTROL_FILE, &statbuf) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat file \"%s\": %m",
									XLOG_CONTROL_FILE)));
				sendFile(sink, XLOG_CONTROL_FILE, XLOG_CONTROL_FILE, &statbuf,
						 false, InvalidOid, &manifest, NULL);
			}
			else
			{
				char	   *archive_name = psprintf("%s.tar", ti->oid);

				bbsink_begin_archive(sink, archive_name);

				sendTablespace(sink, ti->path, ti->oid, false, &manifest);
			}

			/*
			 * 若含 WAL 且这是主数据目录，则不把它当表空间
			 * 结束。而是在下方纳入 xlog 后再停。这安全，
			 * 因主数据目录总是*最后*发送。
			 * 总是最后发。
			 */
			if (opt->includewal && ti->path == NULL)
			{
				Assert(lnext(state.tablespaces, lc) == NULL);
			}
			else
			{
				/* 正确终止 tar 文件。 */
				StaticAssertDecl(2 * TAR_BLOCK_SIZE <= BLCKSZ,
								 "BLCKSZ too small for 2 tar blocks");
				memset(sink->bbs_buffer, 0, 2 * TAR_BLOCK_SIZE);
				bbsink_archive_contents(sink, 2 * TAR_BLOCK_SIZE);

				/* 好，归档到此结束。 */
				bbsink_end_archive(sink);
			}
		}

		basebackup_progress_wait_wal_archive(&state);
		do_pg_backup_stop(backup_state, !opt->nowait);

		endptr = backup_state->stoppoint;
		endtli = backup_state->stoptli;

		/* 释放备份相关变量。 */
		pfree(tablespace_map->data);
		pfree(tablespace_map);
		pfree(backup_state);
	}
	PG_END_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, BoolGetDatum(false));


	if (opt->includewal)
	{
		/*
		 * 我们把最后一个 tar 开着，以便现在追加所需
		 * WAL 文件。
		 */
		char		pathbuf[MAXPGPATH];
		XLogSegNo	segno;
		XLogSegNo	startsegno;
		XLogSegNo	endsegno;
		struct stat statbuf;
		List	   *historyFileList = NIL;
		List	   *walFileList = NIL;
		char		firstoff[MAXFNAMELEN];
		char		lastoff[MAXFNAMELEN];
		DIR		   *dir;
		struct dirent *de;
		ListCell   *lc;
		TimeLineID	tli;

		basebackup_progress_transfer_wal();

		/*
		 * 此处不想操心时间线，故扫描 pg_wal，纳入
		 * startptr 与 endptr 间全部 WAL，不论戳记时间线。
		 * 若有不属于本服务器历史的虚假 WAL，也会纳入。
		 * 正常不应有；即便有，纳入也几乎无害。
		 * 正常不应有；即便有，
		 * 纳入也几乎无害。
		 * 纳入之。
		 */
		XLByteToSeg(state.startptr, startsegno, wal_segment_size);
		XLogFileName(firstoff, state.starttli, startsegno, wal_segment_size);
		XLByteToPrevSeg(endptr, endsegno, wal_segment_size);
		XLogFileName(lastoff, endtli, endsegno, wal_segment_size);

		dir = AllocateDir("pg_wal");
		while ((de = ReadDir(dir, "pg_wal")) != NULL)
		{
			/* 看起来像 WAL 段且在范围内吗？ */
			if (IsXLogFileName(de->d_name) &&
				strcmp(de->d_name + 8, firstoff + 8) >= 0 &&
				strcmp(de->d_name + 8, lastoff + 8) <= 0)
			{
				walFileList = lappend(walFileList, pstrdup(de->d_name));
			}
			/* 看起来像时间线历史文件吗？ */
			else if (IsTLHistoryFileName(de->d_name))
			{
				historyFileList = lappend(historyFileList, pstrdup(de->d_name));
			}
		}
		FreeDir(dir);

		/*
		 * 继续前先检查我们需要的 WAL 段皆未被
		 * 删除。
		 */
		CheckXLogRemoved(startsegno, state.starttli);

		/*
		 * 对 WAL 文件名排序。想从最旧到最新发送，以减
		 * 少文件在我们有机会发送前就被回收的
		 * 几率。
		 */
		list_sort(walFileList, compareWalFileNames);

		/*
		 * pg_wal 中至少须有一个 xlog 文件，因我们在做
		 * 含 xlog 的备份。
		 */
		if (walFileList == NIL)
			ereport(ERROR,
					(errmsg("could not find any WAL files")));

		/*
		 * 健全检查：首末段应覆盖 startptr 与
		 * endptr，中间无缺口。
		 */
		XLogFromFileName((char *) linitial(walFileList),
						 &tli, &segno, wal_segment_size);
		if (segno != startsegno)
		{
			char		startfname[MAXFNAMELEN];

			XLogFileName(startfname, state.starttli, startsegno,
						 wal_segment_size);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", startfname)));
		}
		foreach(lc, walFileList)
		{
			char	   *walFileName = (char *) lfirst(lc);
			XLogSegNo	currsegno = segno;
			XLogSegNo	nextsegno = segno + 1;

			XLogFromFileName(walFileName, &tli, &segno, wal_segment_size);
			if (!(nextsegno == segno || currsegno == segno))
			{
				char		nextfname[MAXFNAMELEN];

				XLogFileName(nextfname, tli, nextsegno, wal_segment_size);
				ereport(ERROR,
						(errmsg("could not find WAL file \"%s\"", nextfname)));
			}
		}
		if (segno != endsegno)
		{
			char		endfname[MAXFNAMELEN];

			XLogFileName(endfname, endtli, endsegno, wal_segment_size);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", endfname)));
		}

		/* 好，所需皆备。发送 WAL 文件。 */
		foreach(lc, walFileList)
		{
			char	   *walFileName = (char *) lfirst(lc);
			int			fd;
			size_t		cnt;
			pgoff_t		len = 0;

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", walFileName);
			XLogFromFileName(walFileName, &tli, &segno, wal_segment_size);

			fd = OpenTransientFile(pathbuf, O_RDONLY | PG_BINARY);
			if (fd < 0)
			{
				int			save_errno = errno;

				/*
				 * 最可能是文件已被检查点删除，故检查之
				 * 以得到更好的错误消息。
				 *
				 */
				CheckXLogRemoved(segno, tli);

				errno = save_errno;
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", pathbuf)));
			}

			if (fstat(fd, &statbuf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								pathbuf)));
			if (statbuf.st_size != wal_segment_size)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("unexpected WAL file size \"%s\"", walFileName)));
			}

			/* 发送 WAL 文件本身 */
			_tarWriteHeader(sink, pathbuf, NULL, &statbuf, false);

			while ((cnt = basebackup_read_file(fd, sink->bbs_buffer,
											   Min(sink->bbs_buffer_length,
												   wal_segment_size - len),
											   len, pathbuf, true)) > 0)
			{
				CheckXLogRemoved(segno, tli);
				bbsink_archive_contents(sink, cnt);

				len += cnt;

				if (len == wal_segment_size)
					break;
			}

			if (len != wal_segment_size)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("unexpected WAL file size \"%s\"", walFileName)));
			}

			/*
			 * wal_segment_size 是 TAR_BLOCK_SIZE 的倍数，故无需
			 * 填充。
			 */
			Assert(wal_segment_size % TAR_BLOCK_SIZE == 0);

			CloseTransientFile(fd);

			/*
			 * 标为已归档，否则新节点提升后文件可能再次
			 * 被归档。与 walreceiver.c 在完整段后总做
			 * XLogArchiveForceDone() 一致。
			 * 完整段后。
			 */
			StatusFilePath(pathbuf, walFileName, ".done");
			sendFileWithContent(sink, pathbuf, "", &manifest);
		}

		/*
		 * 也发送时间线历史文件。恢复只需最新时间线
		 * 历史文件，且仅当含检查点的首个 WAL 段恰有
		 * 时间线切换，或从备库做基备份且期间目标时间线
		 * 变化时才真正需要。但它们很小且对调试极有用，
		 * 故总是全部纳入为好。
		 * 故总是全部纳入。
		 * 总是全部纳入。
		 */
		foreach(lc, historyFileList)
		{
			char	   *fname = lfirst(lc);

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", fname);

			if (lstat(pathbuf, &statbuf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathbuf)));

			sendFile(sink, pathbuf, pathbuf, &statbuf, false, InvalidOid,
					 &manifest, NULL);

			/* 无条件标文件为已归档 */
			StatusFilePath(pathbuf, fname, ".done");
			sendFileWithContent(sink, pathbuf, "", &manifest);
		}

		/* 正确终止 tar 文件。 */
		StaticAssertStmt(2 * TAR_BLOCK_SIZE <= BLCKSZ,
						 "BLCKSZ too small for 2 tar blocks");
		memset(sink->bbs_buffer, 0, 2 * TAR_BLOCK_SIZE);
		bbsink_archive_contents(sink, 2 * TAR_BLOCK_SIZE);

		/* 好，归档到此结束。 */
		bbsink_end_archive(sink);
	}

	AddWALInfoToBackupManifest(&manifest, state.startptr, state.starttli,
							   endptr, endtli);

	SendBackupManifest(&manifest, sink);

	bbsink_end_backup(sink, endptr, endtli);

	if (total_checksum_failures)
	{
		if (total_checksum_failures > 1)
			ereport(WARNING,
					(errmsg_plural("%lld total checksum verification failure",
								   "%lld total checksum verification failures",
								   total_checksum_failures,
								   total_checksum_failures)));

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("checksum verification failure during base backup")));
	}

	/*
	 * 确保在资源所有者之前释放清单，因清单使用的
	 * cryptohash 上下文可能依赖资源所有者（如
	 * OpenSSL).
	 */
	FreeBackupManifest(&manifest);

	/* 清理我们创建的资源所有者 */
	WalSndResourceCleanup(true);

	basebackup_progress_done();
}

/*
 * list_sort 比较函数：比较 WAL 段文件名的 log/seg 部分，
 * 忽略时间线部分。
 */
static int
compareWalFileNames(const ListCell *a, const ListCell *b)
{
	char	   *fna = (char *) lfirst(a);
	char	   *fnb = (char *) lfirst(b);

	return strcmp(fna + 8, fnb + 8);
}

/*
 * 解析解析器下传的基备份选项
 */
static void
parse_basebackup_options(List *options, basebackup_options *opt)
{
	ListCell   *lopt;
	bool		o_label = false;
	bool		o_progress = false;
	bool		o_checkpoint = false;
	bool		o_nowait = false;
	bool		o_wal = false;
	bool		o_maxrate = false;
	bool		o_tablespace_map = false;
	bool		o_noverify_checksums = false;
	bool		o_manifest = false;
	bool		o_manifest_checksums = false;
	bool		o_target = false;
	bool		o_target_detail = false;
	char	   *target_str = NULL;
	char	   *target_detail_str = NULL;
	bool		o_compression = false;
	bool		o_compression_detail = false;
	char	   *compression_detail_str = NULL;

	MemSet(opt, 0, sizeof(*opt));
	opt->manifest = MANIFEST_OPTION_NO;
	opt->manifest_checksum_type = CHECKSUM_TYPE_CRC32C;
	opt->compression = PG_COMPRESSION_NONE;
	opt->compression_specification.algorithm = PG_COMPRESSION_NONE;

	foreach(lopt, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lopt);

		if (strcmp(defel->defname, "label") == 0)
		{
			if (o_label)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->label = defGetString(defel);
			o_label = true;
		}
		else if (strcmp(defel->defname, "progress") == 0)
		{
			if (o_progress)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->progress = defGetBoolean(defel);
			o_progress = true;
		}
		else if (strcmp(defel->defname, "checkpoint") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_checkpoint)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (pg_strcasecmp(optval, "fast") == 0)
				opt->fastcheckpoint = true;
			else if (pg_strcasecmp(optval, "spread") == 0)
				opt->fastcheckpoint = false;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized checkpoint type: \"%s\"",
								optval)));
			o_checkpoint = true;
		}
		else if (strcmp(defel->defname, "wait") == 0)
		{
			if (o_nowait)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->nowait = !defGetBoolean(defel);
			o_nowait = true;
		}
		else if (strcmp(defel->defname, "wal") == 0)
		{
			if (o_wal)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->includewal = defGetBoolean(defel);
			o_wal = true;
		}
		else if (strcmp(defel->defname, "max_rate") == 0)
		{
			int64		maxrate;

			if (o_maxrate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));

			maxrate = defGetInt64(defel);
			if (maxrate < MAX_RATE_LOWER || maxrate > MAX_RATE_UPPER)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("%d is outside the valid range for parameter \"%s\" (%d .. %d)",
								(int) maxrate, "MAX_RATE", MAX_RATE_LOWER, MAX_RATE_UPPER)));

			opt->maxrate = (uint32) maxrate;
			o_maxrate = true;
		}
		else if (strcmp(defel->defname, "tablespace_map") == 0)
		{
			if (o_tablespace_map)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->sendtblspcmapfile = defGetBoolean(defel);
			o_tablespace_map = true;
		}
		else if (strcmp(defel->defname, "verify_checksums") == 0)
		{
			if (o_noverify_checksums)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			noverify_checksums = !defGetBoolean(defel);
			o_noverify_checksums = true;
		}
		else if (strcmp(defel->defname, "manifest") == 0)
		{
			char	   *optval = defGetString(defel);
			bool		manifest_bool;

			if (o_manifest)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (parse_bool(optval, &manifest_bool))
			{
				if (manifest_bool)
					opt->manifest = MANIFEST_OPTION_YES;
				else
					opt->manifest = MANIFEST_OPTION_NO;
			}
			else if (pg_strcasecmp(optval, "force-encode") == 0)
				opt->manifest = MANIFEST_OPTION_FORCE_ENCODE;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized manifest option: \"%s\"",
								optval)));
			o_manifest = true;
		}
		else if (strcmp(defel->defname, "manifest_checksums") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_manifest_checksums)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (!pg_checksum_parse_type(optval,
										&opt->manifest_checksum_type))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized checksum algorithm: \"%s\"",
								optval)));
			o_manifest_checksums = true;
		}
		else if (strcmp(defel->defname, "target") == 0)
		{
			if (o_target)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			target_str = defGetString(defel);
			o_target = true;
		}
		else if (strcmp(defel->defname, "target_detail") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_target_detail)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			target_detail_str = optval;
			o_target_detail = true;
		}
		else if (strcmp(defel->defname, "compression") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_compression)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (!parse_compress_algorithm(optval, &opt->compression))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized compression algorithm: \"%s\"",
								optval)));
			o_compression = true;
		}
		else if (strcmp(defel->defname, "compression_detail") == 0)
		{
			if (o_compression_detail)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			compression_detail_str = defGetString(defel);
			o_compression_detail = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized base backup option: \"%s\"",
							defel->defname)));
	}

	if (opt->label == NULL)
		opt->label = "base backup";
	if (opt->manifest == MANIFEST_OPTION_NO)
	{
		if (o_manifest_checksums)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("manifest checksums require a backup manifest")));
		opt->manifest_checksum_type = CHECKSUM_TYPE_NONE;
	}

	if (target_str == NULL)
	{
		if (target_detail_str != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("target detail cannot be used without target")));
		opt->use_copytblspc = true;
		opt->send_to_client = true;
	}
	else if (strcmp(target_str, "client") == 0)
	{
		if (target_detail_str != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("target \"%s\" does not accept a target detail",
							target_str)));
		opt->send_to_client = true;
	}
	else
		opt->target_handle =
			BaseBackupGetTargetHandle(target_str, target_detail_str);

	if (o_compression_detail && !o_compression)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("compression detail cannot be specified unless compression is enabled")));

	if (o_compression)
	{
		char	   *error_detail;

		parse_compress_specification(opt->compression, compression_detail_str,
									 &opt->compression_specification);
		error_detail =
			validate_compress_specification(&opt->compression_specification);
		if (error_detail != NULL)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid compression specification: %s",
						   error_detail));
	}
}


/*
 * SendBaseBackup() - 发送完整基备份。
 *
 * 本函数会像 pg_backup_start() 一样使系统进入备份模式，
 * 以便即便直接读文件系统、绕过缓冲缓存，备份仍
 * 一致。
 */
void
SendBaseBackup(BaseBackupCmd *cmd)
{
	basebackup_options opt;
	bbsink	   *sink;
	SessionBackupState status = get_backup_status();

	if (status == SESSION_BACKUP_RUNNING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a backup is already in progress in this session")));

	parse_basebackup_options(cmd->options, &opt);

	WalSndSetState(WALSNDSTATE_BACKUP);

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "sending backup \"%s\"",
				 opt.label);
		set_ps_display(activitymsg);
	}

	/*
	 * 若 target 明确为 'client'，则设置为向客户端流式传送
	 * 备份；否则发往别处，不应发给客户端。
	 * BaseBackupGetSink 负责建立 sink，把备份数据送到
	 * 该去的地方。
	 */
	sink = bbsink_copystream_new(opt.send_to_client);
	if (opt.target_handle != NULL)
		sink = BaseBackupGetSink(opt.target_handle, sink);

	/* 若客户端请求，则设置网络限速 */
	if (opt.maxrate > 0)
		sink = bbsink_throttle_new(sink, opt.maxrate);

	/* 若客户端请求，则设置服务器端压缩 */
	if (opt.compression == PG_COMPRESSION_GZIP)
		sink = bbsink_gzip_new(sink, &opt.compression_specification);
	else if (opt.compression == PG_COMPRESSION_LZ4)
		sink = bbsink_lz4_new(sink, &opt.compression_specification);
	else if (opt.compression == PG_COMPRESSION_ZSTD)
		sink = bbsink_zstd_new(sink, &opt.compression_specification);

	/* 设置进度报告。 */
	sink = bbsink_progress_new(sink, opt.progress);

	/*
	 * 执行基备份，但确保即便出错也清理 bbsink。
	 * 出错亦然。
	 */
	PG_TRY();
	{
		perform_base_backup(&opt, sink);
	}
	PG_FINALLY();
	{
		bbsink_cleanup(sink);
	}
	PG_END_TRY();
}

/*
 * 在输出 tar 流中注入给定名与内容的文件。
 */
static void
sendFileWithContent(bbsink *sink, const char *filename, const char *content,
					backup_manifest_info *manifest)
{
	struct stat statbuf;
	int			bytes_done = 0,
				len;
	pg_checksum_context checksum_ctx;

	if (pg_checksum_init(&checksum_ctx, manifest->checksum_type) < 0)
		elog(ERROR, "could not initialize checksum of file \"%s\"",
			 filename);

	len = strlen(content);

	/*
	 * 为正在注入 tar 的 backup_label 构造 stat 结构。
	 * tar中.
	 */
	/* Windows 没有 uid 与 gid 的概念 */
#ifdef WIN32
	statbuf.st_uid = 0;
	statbuf.st_gid = 0;
#else
	statbuf.st_uid = geteuid();
	statbuf.st_gid = getegid();
#endif
	statbuf.st_mtime = time(NULL);
	statbuf.st_mode = pg_file_create_mode;
	statbuf.st_size = len;

	_tarWriteHeader(sink, filename, NULL, &statbuf, false);

	if (pg_checksum_update(&checksum_ctx, (uint8 *) content, len) < 0)
		elog(ERROR, "could not update checksum of file \"%s\"",
			 filename);

	while (bytes_done < len)
	{
		size_t		remaining = len - bytes_done;
		size_t		nbytes = Min(sink->bbs_buffer_length, remaining);

		memcpy(sink->bbs_buffer, content, nbytes);
		bbsink_archive_contents(sink, nbytes);
		bytes_done += nbytes;
		content += nbytes;
	}

	_tarWritePadding(sink, len);

	AddFileToBackupManifest(manifest, NULL, filename, len,
							(pg_time_t) statbuf.st_mtime, &checksum_ctx);
}

/*
 * 把 'path' 指向的表空间目录纳入输出 tar 流。若
 * sizeonly 为真，只计算总长度并返回，不实际发送。
 * 不实际发送任何内容。
 *
 * 仅用于发送辅助表空间，非 PGDATA。
 */
static int64
sendTablespace(bbsink *sink, char *path, char *spcoid, bool sizeonly,
			   backup_manifest_info *manifest)
{
	int64		size;
	char		pathbuf[MAXPGPATH];
	struct stat statbuf;

	/*
	 * path 指向表空间位置，但我们只想纳入其中属于
	 * 我们的版本目录。
	 */
	snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path,
			 TABLESPACE_VERSION_DIRECTORY);

	/*
	 * 在 tar 中存目录项以便权限正确。
	 *
	 */
	if (lstat(pathbuf, &statbuf) != 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file or directory \"%s\": %m",
							pathbuf)));

		/* 扫描时表空间消失不算错误。 */
		return 0;
	}

	size = _tarWriteHeader(sink, TABLESPACE_VERSION_DIRECTORY, NULL, &statbuf,
						   sizeonly);

	/* 发送表空间版本目录中的全部文件 */
	size += sendDir(sink, pathbuf, strlen(path), sizeonly, NIL, true, manifest,
					spcoid);

	return size;
}

/*
 * 把给定目录中全部文件纳入输出 tar 流。若 sizeonly
 * 为真，只计算总长度并返回，不实际
 * 发送任何内容。
 *
 * 省略表空间列表中的目录，以免 PGDATA 内创建的
 * 表空间被备份两次。
 *
 * 若 sendtblspclinks 为真，须在 tar 纳入
 * 符号链接信息；否则可跳过，因其会在
 * tablespace_map 中单独发送。
 */
static int64
sendDir(bbsink *sink, const char *path, int basepathlen, bool sizeonly,
		List *tablespaces, bool sendtblspclinks, backup_manifest_info *manifest,
		const char *spcoid)
{
	DIR		   *dir;
	struct dirent *de;
	char		pathbuf[MAXPGPATH * 2];
	struct stat statbuf;
	int64		size = 0;
	const char *lastDir;		/* 从父路径拆出末级目录. */
	bool		isDbDir = false;	/* 本目录是否含关系？ */

	/*
	 * 判断当前路径是否为可含关系的数据库目录。
	 *
	 *
	 * 先找出父路径与当前路径之间分隔符的位置。
	 *
	 */
	lastDir = last_dir_separator(path);

	/* 本路径看起来像数据库路径（全数字）吗？ */
	if (lastDir != NULL &&
		strspn(lastDir + 1, "0123456789") == strlen(lastDir + 1))
	{
		/* 含父目录的那部分路径。 */
		int			parentPathLen = lastDir - path;

		/*
		 * 若父路径是 $PGDATA/base 或表空间版本路径，则标
		 * 本路径为数据库目录。
		 */
		if (strncmp(path, "./base", parentPathLen) == 0 ||
			(parentPathLen >= (sizeof(TABLESPACE_VERSION_DIRECTORY) - 1) &&
			 strncmp(lastDir - (sizeof(TABLESPACE_VERSION_DIRECTORY) - 1),
					 TABLESPACE_VERSION_DIRECTORY,
					 sizeof(TABLESPACE_VERSION_DIRECTORY) - 1) == 0))
			isDbDir = true;
	}

	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		int			excludeIdx;
		bool		excludeFound;
		ForkNumber	relForkNum; /* 关系文件的 fork 类型 */
		int			relnumchars;	/* 文件名中作为
									 * relnumber */

		/* 跳过特殊内容 */
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		/* 跳过临时文件 */
		if (strncmp(de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
			continue;

		/* 跳过 macOS 文件 */
		if (strcmp(de->d_name, ".DS_Store") == 0)
			continue;

		/*
		 * 检查 postmaster 是否发信号让我们退出；若是则报错
		 * 中止。上层错误处理会为我们调用
		 * do_pg_abort_backup()。还要检查：若备份在恢复中
		 * 开始，服务器是否已被提升。
		 * do_pg_backup_stop() 也会查，但早停备份好过撑到
		 * 末尾再失败。
		 */
		CHECK_FOR_INTERRUPTS();
		if (RecoveryInProgress() != backup_started_in_recovery)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the standby was promoted during online backup"),
					 errhint("This means that the backup being taken is corrupt "
							 "and should not be used. "
							 "Try taking another online backup.")));

		/* 扫描应排除的文件 */
		excludeFound = false;
		for (excludeIdx = 0; excludeFiles[excludeIdx].name != NULL; excludeIdx++)
		{
			int			cmplen = strlen(excludeFiles[excludeIdx].name);

			if (!excludeFiles[excludeIdx].match_prefix)
				cmplen++;
			if (strncmp(de->d_name, excludeFiles[excludeIdx].name, cmplen) == 0)
			{
				elog(DEBUG1, "file \"%s\" excluded from backup", de->d_name);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			continue;

		/* 排除非日志表除 init fork 外的全部 fork */
		if (isDbDir &&
			parse_filename_for_nontemp_relation(de->d_name, &relnumchars,
												&relForkNum))
		{
			/* 永不排除 init fork */
			if (relForkNum != INIT_FORKNUM)
			{
				char		initForkFile[MAXPGPATH];
				char		relNumber[OIDCHARS + 1];

				/*
				 * 若为其他 fork，检查是否有相同 RelFileNumber
				 * 的 init fork。若有，则该文件可排除。
				 *
				 */
				memcpy(relNumber, de->d_name, relnumchars);
				relNumber[relnumchars] = '\0';
				snprintf(initForkFile, sizeof(initForkFile), "%s/%s_init",
						 path, relNumber);

				if (lstat(initForkFile, &statbuf) == 0)
				{
					elog(DEBUG2,
						 "unlogged relation file \"%s\" excluded from backup",
						 de->d_name);

					continue;
				}
			}
		}

		/* 排除临时关系 */
		if (isDbDir && looks_like_temp_rel_name(de->d_name))
		{
			elog(DEBUG2,
				 "temporary relation file \"%s\" excluded from backup",
				 de->d_name);

			continue;
		}

		snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, de->d_name);

		/* 跳过 pg_control，留到最后备份 */
		if (strcmp(pathbuf, "./global/pg_control") == 0)
			continue;

		if (lstat(pathbuf, &statbuf) != 0)
		{
			if (errno != ENOENT)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file or directory \"%s\": %m",
								pathbuf)));

			/* 扫描时文件消失不算错误。 */
			continue;
		}

		/* 扫描内容应排除的目录 */
		excludeFound = false;
		for (excludeIdx = 0; excludeDirContents[excludeIdx] != NULL; excludeIdx++)
		{
			if (strcmp(de->d_name, excludeDirContents[excludeIdx]) == 0)
			{
				elog(DEBUG1, "contents of directory \"%s\" excluded from backup", de->d_name);
				convert_link_to_directory(pathbuf, &statbuf);
				size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL,
										&statbuf, sizeonly);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			continue;

		/*
		 * 可跳过 pg_wal，WAL 段反正要从 WAL 归档取。但仍
		 * 把它作为空目录纳入，以便
		 * 权限正确。
		 */
		if (strcmp(pathbuf, "./pg_wal") == 0)
		{
			/* 若 pg_wal 是符号链接，仍写为目录 */
			convert_link_to_directory(pathbuf, &statbuf);
			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL,
									&statbuf, sizeonly);

			/*
			 * 也发送 archive_status 目录（取巧复用上面的
			 * statbuf……）。
			 */
			size += _tarWriteHeader(sink, "./pg_wal/archive_status", NULL,
									&statbuf, sizeonly);

			continue;			/* 不递归进入 pg_wal */
		}

		/* 仅允许 pg_tblspc 中的符号链接 */
		if (strcmp(path, "./pg_tblspc") == 0 && S_ISLNK(statbuf.st_mode))
		{
			char		linkpath[MAXPGPATH];
			int			rllen;

			rllen = readlink(pathbuf, linkpath, sizeof(linkpath));
			if (rllen < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read symbolic link \"%s\": %m",
								pathbuf)));
			if (rllen >= sizeof(linkpath))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("symbolic link \"%s\" target is too long",
								pathbuf)));
			linkpath[rllen] = '\0';

			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, linkpath,
									&statbuf, sizeonly);
		}
		else if (S_ISDIR(statbuf.st_mode))
		{
			bool		skip_this_dir = false;
			ListCell   *lc;

			/*
			 * 在 tar 中存目录项以便取得
			 * 正确权限。
			 */
			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL, &statbuf,
									sizeonly);

			/*
			 * 对目录递归调用自身，除非它恰是 PGDATA 内的
			 * 独立表空间。
			 */
			foreach(lc, tablespaces)
			{
				tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);

				/*
				 * ti->rpath 是表空间在 PGDATA 内相对路径，或在
				 * 表空间已正确放在别处时为
				 * NULL.
				 *
				 * 比较时跳过 pathbuf 前导的 "./"。
				 */
				if (ti->rpath && strcmp(ti->rpath, pathbuf + 2) == 0)
				{
					skip_this_dir = true;
					break;
				}
			}

			/*
			 * 若不需要，跳过发送 pg_tblspc 内的目录。
			 */
			if (strcmp(pathbuf, "./pg_tblspc") == 0 && !sendtblspclinks)
				skip_this_dir = true;

			if (!skip_this_dir)
				size += sendDir(sink, pathbuf, basepathlen, sizeonly, tablespaces,
								sendtblspclinks, manifest, spcoid);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			bool		sent = false;

			if (!sizeonly)
				sent = sendFile(sink, pathbuf, pathbuf + basepathlen + 1, &statbuf,
								true, isDbDir ? atooid(lastDir + 1) : InvalidOid,
								manifest, spcoid);

			if (sent || sizeonly)
			{
				/* 加算. */
				size += statbuf.st_size;

				/* 填充到 tar 块大小的倍数。 */
				size += tarPaddingBytesRequired(statbuf.st_size);

				/* 该文件头的大小。 */
				size += TAR_BLOCK_SIZE;
			}
		}
		else
			ereport(WARNING,
					(errmsg("skipping special file \"%s\"", pathbuf)));
	}
	FreeDir(dir);
	return size;
}

/*
 * 检查文件是否应做校验和验证。
 * 只对常规表空间(含 global/default)中文件
 * 验证校验和，且其中有些文件被显式
 * 排除。
 */
static bool
is_checksummed_file(const char *fullpath, const char *filename)
{
	/* 检查文件是否在表空间中 */
	if (strncmp(fullpath, "./global/", 9) == 0 ||
		strncmp(fullpath, "./base/", 7) == 0 ||
		strncmp(fullpath, "/", 1) == 0)
	{
		int			excludeIdx;

		/* 对照 noChecksumFiles 跳过列表比较 */
		for (excludeIdx = 0; noChecksumFiles[excludeIdx].name != NULL; excludeIdx++)
		{
			int			cmplen = strlen(noChecksumFiles[excludeIdx].name);

			if (!noChecksumFiles[excludeIdx].match_prefix)
				cmplen++;
			if (strncmp(filename, noChecksumFiles[excludeIdx].name,
						cmplen) == 0)
				return false;
		}

		return true;
	}
	else
		return false;
}

/*
 * 给定成员，写 TAR 头并发送文件。
 *
 * 若 missing_ok 为真，文件未找到也不抛错。
 *
 * 若 dboid 不是 InvalidOid，则检测到的校验和失败会
 * 报告给累计统计系统。
 *
 * 成功发送返回真；若 missing_ok 且文件不存在则
 * 返回假。
 */
static bool
sendFile(bbsink *sink, const char *readfilename, const char *tarfilename,
		 struct stat *statbuf, bool missing_ok, Oid dboid,
		 backup_manifest_info *manifest, const char *spcoid)
{
	int			fd;
	BlockNumber blkno = 0;
	bool		block_retry = false;
	uint16		checksum;
	int			checksum_failures = 0;
	off_t		cnt;
	int			i;
	pgoff_t		len = 0;
	char	   *page;
	PageHeader	phdr;
	int			segmentno = 0;
	char	   *segmentpath;
	bool		verify_checksum = false;
	pg_checksum_context checksum_ctx;

	if (pg_checksum_init(&checksum_ctx, manifest->checksum_type) < 0)
		elog(ERROR, "could not initialize checksum of file \"%s\"",
			 readfilename);

	fd = OpenTransientFile(readfilename, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT && missing_ok)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", readfilename)));
	}

	_tarWriteHeader(sink, tarfilename, NULL, statbuf, false);

	if (!noverify_checksums && DataChecksumsEnabled())
	{
		char	   *filename;

		/*
		 * 取文件名（不含路径）。因 last_dir_separator()
		 * 含最后的目录分隔符，通过递增指针
		 * 去掉它。
		 */
		filename = last_dir_separator(readfilename) + 1;

		if (is_checksummed_file(readfilename, filename))
		{
			verify_checksum = true;

			/*
			 * 在段边界（"."）处截断以取得段号，以便混入
			 * 校验和。
			 */
			segmentpath = strstr(filename, ".");
			if (segmentpath != NULL)
			{
				segmentno = atoi(segmentpath + 1);
				if (segmentno == 0)
					ereport(ERROR,
							(errmsg("invalid segment number %d in file \"%s\"",
									segmentno, filename)));
			}
		}
	}

	/*
	 * 循环读取直至达到调用方预期的数据量。文件可能在
	 * 发送时被扩展而更长，但对基备份可忽略这类扩展
	 * 数据，将由 WAL 恢复。
	 *
	 */
	while (len < statbuf->st_size)
	{
		size_t		remaining = statbuf->st_size - len;

		/* 尝试再读一些数据。 */
		cnt = basebackup_read_file(fd, sink->bbs_buffer,
								   Min(sink->bbs_buffer_length, remaining),
								   len, readfilename, true);

		/*
		 * 校验和按块级验证，故在确认 TAR_SEND_SIZE/buf
		 * 可被 BLCKSZ 整除且读到其倍数后，按
		 * BLCKSZ 块迭代缓冲。
		 * BLCKSZ 字节
		 */
		Assert((sink->bbs_buffer_length % BLCKSZ) == 0);

		if (verify_checksum && (cnt % BLCKSZ != 0))
		{
			ereport(WARNING,
					(errmsg("could not verify checksum in file \"%s\", block "
							"%u: read buffer size %d and page size %d "
							"differ",
							readfilename, blkno, (int) cnt, BLCKSZ)));
			verify_checksum = false;
		}

		if (verify_checksum)
		{
			for (i = 0; i < cnt / BLCKSZ; i++)
			{
				page = sink->bbs_buffer + BLCKSZ * i;

				/*
				 * 只检查自备份起未改的页。否则可能只写
				 * 一半致校验和无效。但重放 WAL 会在此
				 * 种情况下恢复正确页。我们也跳过全新
				 * 页，因为它们尚无校验和。
				 *
				 *
				 */
				if (!PageIsNew(page) && PageGetLSN(page) < sink->bbs_state->startptr)
				{
					checksum = pg_checksum_page((char *) page, blkno + segmentno * RELSEG_SIZE);
					phdr = (PageHeader) page;
					if (phdr->pd_checksum != checksum)
					{
						/*
						 * 首次失败则重试；或更新前读了
						 * 前 4K 页而看似撕裂。若重试前并
						 * 发写完成，页 LSN 更新，我们就会
						 * 意识到应忽略本块。
						 *
						 *
						 *
						 *
						 * 但无保证：撕裂写或任意久才完
						 * 成。多次重试难根治，只降几率
						 * 真修似需互锁，以便等到确定无
						 * 对该块之写。目前没有，只好如此
						 * 并指望最好结果。
						 *
						 *
						 *
						 *
						 *
						 */
						if (block_retry == false)
						{
							int			reread_cnt;

							/* 重读失败的块 */
							reread_cnt =
								basebackup_read_file(fd,
													 sink->bbs_buffer + BLCKSZ * i,
													 BLCKSZ, len + BLCKSZ * i,
													 readfilename,
													 false);
							if (reread_cnt == 0)
							{
								/*
								 * 若遇 EOF 即并发截断，故像
								 * 初始 fread() 返回 0 一样跳出，
								 * 落到同一处理代码。（须先
								 * 修正 cnt。）
								 *
								 *
								 */
								cnt = BLCKSZ * i;
								break;
							}

							/* 设标志以便知道已尝试重试 */
							block_retry = true;

							/* 重置循环以再次验证该块 */
							i--;
							continue;
						}

						checksum_failures++;

						if (checksum_failures <= 5)
							ereport(WARNING,
									(errmsg("checksum verification failed in "
											"file \"%s\", block %u: calculated "
											"%X but expected %X",
											readfilename, blkno, checksum,
											phdr->pd_checksum)));
						if (checksum_failures == 5)
							ereport(WARNING,
									(errmsg("further checksum verification "
											"failures in file \"%s\" will not "
											"be reported", readfilename)));
					}
				}
				block_retry = false;
				blkno++;
			}
		}

		/*
		 * 若碰到文件结束，则发生了并发截断。
		 * 这不是错误，因 WAL 重放会把事情理顺。
		 *
		 */
		if (cnt == 0)
			break;

		/* 归档刚读到的数据。 */
		bbsink_archive_contents(sink, cnt);

		/* 也送入校验和机制。 */
		if (pg_checksum_update(&checksum_ctx,
							   (uint8 *) sink->bbs_buffer, cnt) < 0)
			elog(ERROR, "could not update checksum of base backup");

		len += cnt;
	}

	/* 若发送时文件被截断，则用零填充 */
	while (len < statbuf->st_size)
	{
		size_t		remaining = statbuf->st_size - len;
		size_t		nbytes = Min(sink->bbs_buffer_length, remaining);

		MemSet(sink->bbs_buffer, 0, nbytes);
		if (pg_checksum_update(&checksum_ctx,
							   (uint8 *) sink->bbs_buffer,
							   nbytes) < 0)
			elog(ERROR, "could not update checksum of base backup");
		bbsink_archive_contents(sink, nbytes);
		len += nbytes;
	}

	/*
	 * 按 tar 格式要求填充到块边界。（这小片数据多半
	 * 不值得限速，也不做校验和，因其并非文件
	 * 实际部分。）
	 */
	_tarWritePadding(sink, len);

	CloseTransientFile(fd);

	if (checksum_failures > 1)
	{
		ereport(WARNING,
				(errmsg_plural("file \"%s\" has a total of %d checksum verification failure",
							   "file \"%s\" has a total of %d checksum verification failures",
							   checksum_failures,
							   readfilename, checksum_failures)));

		pgstat_report_checksum_failures_in_db(dboid, checksum_failures);
	}

	total_checksum_failures += checksum_failures;

	AddFileToBackupManifest(manifest, spcoid, tarfilename, statbuf->st_size,
							(pg_time_t) statbuf->st_mtime, &checksum_ctx);

	return true;
}

static int64
_tarWriteHeader(bbsink *sink, const char *filename, const char *linktarget,
				struct stat *statbuf, bool sizeonly)
{
	enum tarError rc;

	if (!sizeonly)
	{
		/*
		 * 截至撰写时，最小支持块大小为 1kB，即
		 * TAR_BLOCK_SIZE 的两倍。因缓冲大小须为 BLCKSZ
		 * 倍数，可假定缓冲大到能装下完整 tar 块。我们
		 * 用这些断言再确认一次。
		 * 用这些断言。
		 */
		StaticAssertDecl(TAR_BLOCK_SIZE <= BLCKSZ,
						 "BLCKSZ too small for tar block");
		Assert(sink->bbs_buffer_length >= TAR_BLOCK_SIZE);

		rc = tarCreateHeader(sink->bbs_buffer, filename, linktarget,
							 statbuf->st_size, statbuf->st_mode,
							 statbuf->st_uid, statbuf->st_gid,
							 statbuf->st_mtime);

		switch (rc)
		{
			case TAR_OK:
				break;
			case TAR_NAME_TOO_LONG:
				ereport(ERROR,
						(errmsg("file name too long for tar format: \"%s\"",
								filename)));
				break;
			case TAR_SYMLINK_TOO_LONG:
				ereport(ERROR,
						(errmsg("symbolic link target too long for tar format: "
								"file name \"%s\", target \"%s\"",
								filename, linktarget)));
				break;
			default:
				elog(ERROR, "unrecognized tar error: %d", rc);
		}

		bbsink_archive_contents(sink, TAR_BLOCK_SIZE);
	}

	return TAR_BLOCK_SIZE;
}

/*
 * 用零字节填充到 TAR_BLOCK_SIZE 的倍数。
 */
static void
_tarWritePadding(bbsink *sink, int len)
{
	int			pad = tarPaddingBytesRequired(len);

	/*
	 * 与 _tarWriteHeader 一样，可假定缓冲足够大，无需
	 * 分多块做。
	 */
	Assert(sink->bbs_buffer_length >= TAR_BLOCK_SIZE);
	Assert(pad <= TAR_BLOCK_SIZE);

	if (pad > 0)
	{
		MemSet(sink->bbs_buffer, 0, pad);
		bbsink_archive_contents(sink, pad);
	}
}

/*
 * 若 statbuf 中条目是链接，则调整 statbuf 使它看起来像
 * 目录，从而按目录写出。
 */
static void
convert_link_to_directory(const char *pathbuf, struct stat *statbuf)
{
	/* 若是符号链接，仍写为目录 */
	if (S_ISLNK(statbuf->st_mode))
		statbuf->st_mode = S_IFDIR | pg_dir_create_mode;
}

/*
 * 从文件读一些数据，设置等待事件并报告遇到的任何
 * 错误。
 *
 * 若 partial_read_ok 为假，则读到的字节数不等于请求数
 * 时也报错。
 *
 * 返回读到的字节数。
 */
static int
basebackup_read_file(int fd, char *buf, size_t nbytes, off_t offset,
					 const char *filename, bool partial_read_ok)
{
	int			rc;

	pgstat_report_wait_start(WAIT_EVENT_BASEBACKUP_READ);
	rc = pg_pread(fd, buf, nbytes, offset);
	pgstat_report_wait_end();

	if (rc < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));
	if (!partial_read_ok && rc > 0 && rc != nbytes)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": read %d of %zu",
						filename, rc, nbytes)));

	return rc;
}
