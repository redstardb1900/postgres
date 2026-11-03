/*-------------------------------------------------------------------------
 *
 * pg_basebackup.c - receive a base backup using streaming replication protocol
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_basebackup.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "access/xlog_internal.h"
#include "backup/basebackup.h"
#include "bbstreamer.h"
#include "common/compression.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/recovery_gen.h"
#include "getopt_long.h"
#include "receivelog.h"
#include "streamutil.h"

#define ERRCODE_DATA_CORRUPTED	"XX001"

typedef struct TablespaceListCell
{
	struct TablespaceListCell *next;
	char		old_dir[MAXPGPATH];
	char		new_dir[MAXPGPATH];
} TablespaceListCell;

typedef struct TablespaceList
{
	TablespaceListCell *head;
	TablespaceListCell *tail;
} TablespaceList;

typedef struct ArchiveStreamState
{
	int			tablespacenum;
	pg_compress_specification *compress;
	bbstreamer *streamer;
	bbstreamer *manifest_inject_streamer;
	PQExpBuffer manifest_buffer;
	char		manifest_filename[MAXPGPATH];
	FILE	   *manifest_file;
} ArchiveStreamState;

typedef struct WriteTarState
{
	int			tablespacenum;
	bbstreamer *streamer;
} WriteTarState;

typedef struct WriteManifestState
{
	char		filename[MAXPGPATH];
	FILE	   *file;
} WriteManifestState;

typedef void (*WriteDataCallback) (size_t nbytes, char *buf,
								   void *callback_data);

/* * pg_xlog 在 v10 重命名为 pg_wal。应与
 * PQserverVersion() 比较。                                            */
#define MINIMUM_VERSION_FOR_PG_WAL	100000

/* * 临时复制槽 v10 起受支持。                          */
#define MINIMUM_VERSION_FOR_TEMP_SLOTS 100000

/* * 备份清单 v13 起受支持。                  */
#define MINIMUM_VERSION_FOR_MANIFESTS	130000

/*
 * Before v15, tar files received from the server will be improperly
 * terminated.
 */
#define MINIMUM_VERSION_FOR_TERMINATED_TARFILE 150000

/* * 包含 WAL 的不同方式      */
typedef enum
{
	NO_WAL,
	FETCH_WAL,
	STREAM_WAL
} IncludeWal;

/* * 执行压缩的不同位置               */
typedef enum
{
	COMPRESS_LOCATION_UNSPECIFIED,
	COMPRESS_LOCATION_CLIENT,
	COMPRESS_LOCATION_SERVER
} CompressionLocation;

/* 全局选项   */
static char *basedir = NULL;
static TablespaceList tablespace_dirs = {NULL, NULL};
static char *xlog_dir = NULL;
static char format = '\0';		/* p/t           */
static char *label = "pg_basebackup base backup";
static bool noclean = false;
static bool checksum_failure = false;
static bool showprogress = false;
static bool estimatesize = true;
static int	verbose = 0;
static IncludeWal includewal = STREAM_WAL;
static bool fastcheckpoint = false;
static bool writerecoveryconf = false;
static bool do_sync = true;
static int	standby_message_timeout = 10 * 1000;	/* 10s 默认       */
static pg_time_t last_progress_report = 0;
static int32 maxrate = 0;		/* 默认无限制     */
static char *replication_slot = NULL;
static bool temp_replication_slot = true;
static char *backup_target = NULL;
static bool create_slot = false;
static bool no_slot = false;
static bool verify_checksums = true;
static bool manifest = true;
static bool manifest_force_encode = false;
static char *manifest_checksums = NULL;

static bool success = false;
static bool made_new_pgdata = false;
static bool found_existing_pgdata = false;
static bool made_new_xlogdir = false;
static bool found_existing_xlogdir = false;
static bool made_tablespace_dirs = false;
static bool found_tablespace_dirs = false;

/* 进度指示        */
static uint64 totalsize_kb;
static uint64 totaldone;
static int	tablespacecount;
static char *progress_filename = NULL;

/* 与后台 WAL 接收进程通信的管道                */
#ifndef WIN32
static int	bgpipe[2] = {-1, -1};
#endif

/* 子进程句柄         */
static pid_t bgchild = -1;
static bool in_log_streamer = false;

/* 标志子进程是否意外退出                     */
static volatile sig_atomic_t bgchild_exited = false;

/* xlog 流结束位，未知为空                             */
static XLogRecPtr xlogendptr;

#ifndef WIN32
static int	has_xlogendptr = 0;
#else
static volatile LONG has_xlogendptr = 0;
#endif

/* 待生成配置文件内容                    */
static PQExpBuffer recoveryconfcontents = NULL;

/* 函数头        */
static void usage(void);
static void verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found);
static void progress_update_filename(const char *filename);
static void progress_report(int tablespacenum, bool force, bool finished);

static bbstreamer *CreateBackupStreamer(char *archive_name, char *spclocation,
										bbstreamer **manifest_inject_streamer_p,
										bool is_recovery_guc_supported,
										bool expect_unterminated_tarfile,
										pg_compress_specification *compress);
static void ReceiveArchiveStreamChunk(size_t r, char *copybuf,
									  void *callback_data);
static char GetCopyDataByte(size_t r, char *copybuf, size_t *cursor);
static char *GetCopyDataString(size_t r, char *copybuf, size_t *cursor);
static uint64 GetCopyDataUInt64(size_t r, char *copybuf, size_t *cursor);
static void GetCopyDataEnd(size_t r, char *copybuf, size_t cursor);
static void ReportCopyDataParseError(size_t r, char *copybuf);
static void ReceiveTarFile(PGconn *conn, char *archive_name, char *spclocation,
						   bool tablespacenum, pg_compress_specification *compress);
static void ReceiveTarCopyChunk(size_t r, char *copybuf, void *callback_data);
static void ReceiveBackupManifest(PGconn *conn);
static void ReceiveBackupManifestChunk(size_t r, char *copybuf,
									   void *callback_data);
static void ReceiveBackupManifestInMemory(PGconn *conn, PQExpBuffer buf);
static void ReceiveBackupManifestInMemoryChunk(size_t r, char *copybuf,
											   void *callback_data);
static void BaseBackup(char *compression_algorithm, char *compression_detail,
					   CompressionLocation compressloc,
					   pg_compress_specification *client_compress);

static bool reached_end_position(XLogRecPtr segendpos, uint32 timeline,
								 bool segment_finished);

static const char *get_tablespace_mapping(const char *dir);
static void tablespace_list_append(const char *arg);


static void
cleanup_directories_atexit(void)
{
	if (success || in_log_streamer)
		return;

	if (!noclean && !checksum_failure)
	{
		if (made_new_pgdata)
		{
			pg_log_info("removing data directory \"%s\"", basedir);
			if (!rmtree(basedir, true))
				pg_log_error("failed to remove data directory");
		}
		else if (found_existing_pgdata)
		{
			pg_log_info("removing contents of data directory \"%s\"", basedir);
			if (!rmtree(basedir, false))
				pg_log_error("failed to remove contents of data directory");
		}

		if (made_new_xlogdir)
		{
			pg_log_info("removing WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, true))
				pg_log_error("failed to remove WAL directory");
		}
		else if (found_existing_xlogdir)
		{
			pg_log_info("removing contents of WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, false))
				pg_log_error("failed to remove contents of WAL directory");
		}
	}
	else
	{
		if ((made_new_pgdata || found_existing_pgdata) && !checksum_failure)
			pg_log_info("data directory \"%s\" not removed at user's request", basedir);

		if (made_new_xlogdir || found_existing_xlogdir)
			pg_log_info("WAL directory \"%s\" not removed at user's request", xlog_dir);
	}

	if ((made_tablespace_dirs || found_tablespace_dirs) && !checksum_failure)
		pg_log_info("changes to tablespace directories will not be undone");
}

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

#ifndef WIN32
/* * bgchild 过早退出且 SIGCHLD 可中止，
 * 无需等备份完成。Windows 用后台线程。                                                                                                                                                                              */
static void
sigchld_handler(SIGNAL_ARGS)
{
	bgchild_exited = true;
}

/* * Windows 后台线程随进程终止。Unix 终止
 * 子进程以免继续流传输。                                                                                                          */
static void
kill_bgchild_atexit(void)
{
	if (bgchild > 0 && !bgchild_exited)
		kill(bgchild, SIGTERM);
}
#endif

/* * 拆分参数为 old_dir/new_dir，追加到
 * 表空间映射。                   */
static void
tablespace_list_append(const char *arg)
{
	TablespaceListCell *cell = (TablespaceListCell *) pg_malloc0(sizeof(TablespaceListCell));
	char	   *dst;
	char	   *dst_ptr;
	const char *arg_ptr;

	dst_ptr = dst = cell->old_dir;
	for (arg_ptr = arg; *arg_ptr; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
			pg_fatal("directory name too long");

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* 跳过反斜杠转义 =   */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*cell->new_dir)
				pg_fatal("multiple \"=\" signs in tablespace mapping");
			else
				dst = dst_ptr = cell->new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*cell->old_dir || !*cell->new_dir)
		pg_fatal("invalid tablespace mapping format \"%s\", must be \"OLDDIR=NEWDIR\"", arg);

	/* * 表空间均绝对目录，非绝对不匹配。不知
 * 远端 OS，允许任一规则下绝对路径。
 *
 * 几乎无混淆风险。Linux 用户不用反斜
 * 杠或驱动器路径。用了 -T 不生效无警告，
 * 等同指定不存在表空间。                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           */
	if (!is_nonwindows_absolute_path(cell->old_dir) &&
		!is_windows_absolute_path(cell->old_dir))
		pg_fatal("old directory is not an absolute path in tablespace mapping: %s",
				 cell->old_dir);

	if (!is_absolute_path(cell->new_dir))
		pg_fatal("new directory is not an absolute path in tablespace mapping: %s",
				 cell->new_dir);

	/* * 比较须用规范化路径。Windows 上尤需
 * 注意，路径可能非 Unix 斜杠。                                                                                                            */
	canonicalize_path(cell->old_dir);
	canonicalize_path(cell->new_dir);

	if (tablespace_dirs.tail)
		tablespace_dirs.tail->next = cell;
	else
		tablespace_dirs.head = cell;
	tablespace_dirs.tail = cell;
}


static void
usage(void)
{
	printf(_("%s takes a base backup of a running PostgreSQL server.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions controlling the output:\n"));
	printf(_("  -D, --pgdata=DIRECTORY receive base backup into directory\n"));
	printf(_("  -F, --format=p|t       output format (plain (default), tar)\n"));
	printf(_("  -r, --max-rate=RATE    maximum transfer rate to transfer data directory\n"
			 "                         (in kB/s, or use suffix \"k\" or \"M\")\n"));
	printf(_("  -R, --write-recovery-conf\n"
			 "                         write configuration for replication\n"));
	printf(_("  -t, --target=TARGET[:DETAIL]\n"
			 "                         backup target (if other than client)\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"
			 "                         relocate tablespace in OLDDIR to NEWDIR\n"));
	printf(_("      --waldir=WALDIR    location for the write-ahead log directory\n"));
	printf(_("  -X, --wal-method=none|fetch|stream\n"
			 "                         include required WAL files with specified method\n"));
	printf(_("  -z, --gzip             compress tar output\n"));
	printf(_("  -Z, --compress=[{client|server}-]METHOD[:DETAIL]\n"
			 "                         compress on client or server as specified\n"));
	printf(_("  -Z, --compress=none    do not compress tar output\n"));
	printf(_("\nGeneral options:\n"));
	printf(_("  -c, --checkpoint=fast|spread\n"
			 "                         set fast or spread checkpointing\n"));
	printf(_("  -C, --create-slot      create replication slot\n"));
	printf(_("  -l, --label=LABEL      set backup label\n"));
	printf(_("  -n, --no-clean         do not clean up after errors\n"));
	printf(_("  -N, --no-sync          do not wait for changes to be written safely to disk\n"));
	printf(_("  -P, --progress         show progress information\n"));
	printf(_("  -S, --slot=SLOTNAME    replication slot to use\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("      --manifest-checksums=SHA{224,256,384,512}|CRC32C|NONE\n"
			 "                         use algorithm for manifest checksums\n"));
	printf(_("      --manifest-force-encode\n"
			 "                         hex encode all file names in manifest\n"));
	printf(_("      --no-estimate-size do not estimate backup size in server side\n"));
	printf(_("      --no-manifest      suppress generation of backup manifest\n"));
	printf(_("      --no-slot          prevent creation of temporary replication slot\n"));
	printf(_("      --no-verify-checksums\n"
			 "                         do not verify checksums\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR   connection string\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -s, --status-interval=INTERVAL\n"
			 "                         time between status packets sent to server (in seconds)\n"));
	printf(_("  -U, --username=NAME    connect as specified database user\n"));
	printf(_("  -w, --no-password      never prompt for password\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


/* * 每次收数据时后台调用。
 * Unix 检查管道有无停止位置。
 * Windows 直接检查是否该停。                                                                                                                                                                                                 */
static bool
reached_end_position(XLogRecPtr segendpos, uint32 timeline,
					 bool segment_finished)
{
	if (!has_xlogendptr)
	{
#ifndef WIN32
		fd_set		fds;
		struct timeval tv = {0};
		int			r;

		/* * 尚无结束指针，查管道是否已发送。
 *                                        */
		FD_ZERO(&fds);
		FD_SET(bgpipe[0], &fds);

		r = select(bgpipe[0] + 1, &fds, NULL, NULL, &tv);
		if (r == 1)
		{
			char		xlogend[64] = {0};
			uint32		hi,
						lo;

			r = read(bgpipe[0], xlogend, sizeof(xlogend) - 1);
			if (r < 0)
				pg_fatal("could not read from ready pipe: %m");

			if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
				pg_fatal("could not parse write-ahead log location \"%s\"",
						 xlogend);
			xlogendptr = ((uint64) hi) << 32 | lo;
			has_xlogendptr = 1;

			/*
			 * Fall through to check if we've reached the point further
			 * already.
			 */
		}
		else
		{
			/* * 管道无数据=未知结束位，告知不停止。
 *                                                                      */
			return false;
		}
#else

		/* * win32 上 has_xlogendptr 主线程设，
 * 未设则返回等待。                                                                */
		return false;
#endif
	}

	/* * 有结束指针，比当前位置决定是否停止。
 *                                                               */
	if (segendpos >= xlogendptr)
		return true;

	/* * 有结束指针未到达，告知调用者继续流。
 *                                    */
	return false;
}

typedef struct
{
	PGconn	   *bgconn;
	XLogRecPtr	startptr;
	char		xlog[MAXPGPATH];	/* 目录或 tar，取决于模式        */
	char	   *sysidentifier;
	int			timeline;
	pg_compress_algorithm wal_compress_algorithm;
	int			wal_compress_level;
} logstreamer_param;

static int
LogStreamerMain(logstreamer_param *param)
{
	StreamCtl	stream = {0};

	in_log_streamer = true;

	stream.startpos = param->startptr;
	stream.timeline = param->timeline;
	stream.sysidentifier = param->sysidentifier;
	stream.stream_stop = reached_end_position;
#ifndef WIN32
	stream.stop_socket = bgpipe[0];
#else
	stream.stop_socket = PGINVALID_SOCKET;
#endif
	stream.standby_message_timeout = standby_message_timeout;
	stream.synchronous = false;
	/* fsync 在 pg_basebackup 末尾统一执行             */
	stream.do_sync = false;
	stream.mark_done = true;
	stream.partial_suffix = NULL;
	stream.replication_slot = replication_slot;
	if (format == 'p')
		stream.walmethod = CreateWalDirectoryMethod(param->xlog,
													PG_COMPRESSION_NONE, 0,
													stream.do_sync);
	else
		stream.walmethod = CreateWalTarMethod(param->xlog,
											  param->wal_compress_algorithm,
											  param->wal_compress_level,
											  stream.do_sync);

	if (!ReceiveXlogStream(param->bgconn, &stream))
	{
		/* * 错误已在函数内报告，告知父进程未正常
 * 关闭。                                                                                       */
#ifdef WIN32
		/* * 设 Unix SIGCHLD 同标志，通知主线
 * 程非正常退出。                                                              */
		bgchild_exited = true;
#endif
		return 1;
	}

	if (!stream.walmethod->ops->finish(stream.walmethod))
	{
		pg_log_error("could not finish writing WAL files: %m");
#ifdef WIN32
		bgchild_exited = true;
#endif
		return 1;
	}

	PQfinish(param->bgconn);

	stream.walmethod->ops->free(stream.walmethod);

	return 0;
}

/* * 启后台进程收 xlog。后台流独立连接，
 * 并行传输日志。                                                                                                                     */
static void
StartLogStreamer(char *startpos, uint32 timeline, char *sysidentifier,
				 pg_compress_algorithm wal_compress_algorithm,
				 int wal_compress_level)
{
	logstreamer_param *param;
	uint32		hi,
				lo;
	char		statusdir[MAXPGPATH];

	param = pg_malloc0(sizeof(logstreamer_param));
	param->timeline = timeline;
	param->sysidentifier = sysidentifier;
	param->wal_compress_algorithm = wal_compress_algorithm;
	param->wal_compress_level = wal_compress_level;

	/* 转换起始位置            */
	if (sscanf(startpos, "%X/%X", &hi, &lo) != 2)
		pg_fatal("could not parse write-ahead log location \"%s\"",
				 startpos);
	param->startptr = ((uint64) hi) << 32 | lo;
	/* 舍入到偶数段                 */
	param->startptr -= XLogSegmentOffset(param->startptr, WalSegSz);

#ifndef WIN32
	/* 创建后台管道         */
	if (pipe(bgpipe) < 0)
		pg_fatal("could not create pipe for background process: %m");
#endif

	/* 获取第二连接      */
	param->bgconn = GetConnection();
	if (!param->bgconn)
		/* 错误已在 GetConnection 写入                */
		exit(1);

	/* post-10 集簇 pg_xlog 重命名 pg_wal                */
	snprintf(param->xlog, sizeof(param->xlog), "%s/%s",
			 basedir,
			 PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
			 "pg_xlog" : "pg_wal");

	/* 临时复制槽仅 10+ 支持                                  */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_TEMP_SLOTS)
		temp_replication_slot = false;

	/* * 按需创建复制槽
 *                 */
	if (temp_replication_slot && !replication_slot)
		replication_slot = psprintf("pg_basebackup_%u",
									(unsigned int) PQbackendPID(param->bgconn));
	if (temp_replication_slot || create_slot)
	{
		if (!CreateReplicationSlot(param->bgconn, replication_slot, NULL,
								   temp_replication_slot, true, true, false, false))
			exit(1);

		if (verbose)
		{
			if (temp_replication_slot)
				pg_log_info("created temporary replication slot \"%s\"",
							replication_slot);
			else
				pg_log_info("created replication slot \"%s\"",
							replication_slot);
		}
	}

	if (format == 'p')
	{
		/* * 按目标服务器创建 pg_wal/archive_status
 * 或 pg_xlog/archive_status 及目录。                                                                                                                                                             */
		snprintf(statusdir, sizeof(statusdir), "%s/%s/archive_status",
				 basedir,
				 PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
				 "pg_xlog" : "pg_wal");

		if (pg_mkdir_p(statusdir, pg_dir_create_mode) != 0 && errno != EEXIST)
			pg_fatal("could not create directory \"%s\": %m", statusdir);
	}

	/* * 启动子进程流传输。Unix fork，
 * Windows 创线程。                                                          */
#ifndef WIN32
	bgchild = fork();
	if (bgchild == 0)
	{
		/* 子进程        */
		exit(LogStreamerMain(param));
	}
	else if (bgchild < 0)
		pg_fatal("could not create background process: %m");

	/* * 否则父进程中，一切正常。
 *                */
	atexit(kill_bgchild_atexit);
#else							/* WIN32 */
	bgchild = _beginthreadex(NULL, 0, (void *) LogStreamerMain, param, 0, NULL);
	if (bgchild == 0)
		pg_fatal("could not create background thread: %m");
#endif
}

/* * 验证目录存在且空。无则创，非空报错。
 *                                                                                                                      */
static void
verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found)
{
	switch (pg_check_dir(dirname))
	{
		case 0:

			/* * 不存在，创
 *                */
			if (pg_mkdir_p(dirname, pg_dir_create_mode) == -1)
				pg_fatal("could not create directory \"%s\": %m", dirname);
			if (created)
				*created = true;
			return;
		case 1:

			/* * 存在，空
 *       */
			if (found)
				*found = true;
			return;
		case 2:
		case 3:
		case 4:

			/* * 存在，非空
 *        */
			pg_fatal("directory \"%s\" exists but is not empty", dirname);
		case -1:

			/* * 访问问题
 *        */
			pg_fatal("could not access directory \"%s\": %m", dirname);
	}
}

/* * 更新当前文件名的回调。
 *
 * 其他代码勿改 progress_filename！                               */
static void
progress_update_filename(const char *filename)
{
	/* 不详细报告则无需维护此变量                         */
	if (showprogress && verbose)
	{
		free(progress_filename);
		if (filename)
			progress_filename = pg_strdup(filename);
		else
			progress_filename = NULL;
	}
}

/* * 基于全局变量打印进度。详细模式打印
 * 当前文件名。
 *
 * 最多每秒一次，force=true 除外。
 *
 * finished=true 最后一次，光标下行。                                                                                                                                                                        */
static void
progress_report(int tablespacenum, bool force, bool finished)
{
	int			percent;
	char		totaldone_str[32];
	char		totalsize_str[32];
	pg_time_t	now;

	if (!showprogress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !force && !finished)
		return;					/* 最多每秒一次  */

	last_progress_report = now;
	percent = totalsize_kb ? (int) ((totaldone / 1024) * 100 / totalsize_kb) : 0;

	/* * 避免超 100% 或总大小。接近结束时可能
 * 变化（含 WAL 时不准），但好过完成列
 * 超总计。                                                                                                                                                     */
	if (percent > 100)
		percent = 100;
	if (totaldone / 1024 > totalsize_kb)
		totalsize_kb = totaldone / 1024;

	snprintf(totaldone_str, sizeof(totaldone_str), UINT64_FORMAT,
			 totaldone / 1024);
	snprintf(totalsize_str, sizeof(totalsize_str), UINT64_FORMAT, totalsize_kb);

#define VERBOSE_FILENAME_LENGTH 35
	if (verbose)
	{
		if (!progress_filename)

			/* * 无文件名，清状态行（最后调用）。
 *                            */
			fprintf(stderr,
					ngettext("%*s/%s kB (100%%), %d/%d tablespace %*s",
							 "%*s/%s kB (100%%), %d/%d tablespaces %*s",
							 tablespacecount),
					(int) strlen(totalsize_str),
					totaldone_str, totalsize_str,
					tablespacenum, tablespacecount,
					VERBOSE_FILENAME_LENGTH + 5, "");
		else
		{
			bool		truncate = (strlen(progress_filename) > VERBOSE_FILENAME_LENGTH);

			fprintf(stderr,
					ngettext("%*s/%s kB (%d%%), %d/%d tablespace (%s%-*.*s)",
							 "%*s/%s kB (%d%%), %d/%d tablespaces (%s%-*.*s)",
							 tablespacecount),
					(int) strlen(totalsize_str),
					totaldone_str, totalsize_str, percent,
					tablespacenum, tablespacecount,
			/* 前导截断加 "..." 前缀                  */
					truncate ? "..." : "",
					truncate ? VERBOSE_FILENAME_LENGTH - 3 : VERBOSE_FILENAME_LENGTH,
					truncate ? VERBOSE_FILENAME_LENGTH - 3 : VERBOSE_FILENAME_LENGTH,
			/* 文件名太长则从开头截断               */
					truncate ? progress_filename + strlen(progress_filename) - VERBOSE_FILENAME_LENGTH + 3 : progress_filename);
		}
	}
	else
		fprintf(stderr,
				ngettext("%*s/%s kB (%d%%), %d/%d tablespace",
						 "%*s/%s kB (%d%%), %d/%d tablespaces",
						 tablespacecount),
				(int) strlen(totalsize_str),
				totaldone_str, totalsize_str, percent,
				tablespacenum, tablespacecount);

	/* * 终端输出且未完成保持同行。
 *                                       */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
}

static int32
parse_max_rate(char *src)
{
	double		result;
	char	   *after_num;
	char	   *suffix = NULL;

	errno = 0;
	result = strtod(src, &after_num);
	if (src == after_num)
		pg_fatal("transfer rate \"%s\" is not a valid value", src);
	if (errno != 0)
		pg_fatal("invalid transfer rate \"%s\": %m", src);

	if (result <= 0)
	{
		/* * 在此拒绝明显错误。
 *            */
		pg_fatal("transfer rate must be greater than zero");
	}

	/* * 跳过空白评后缀。无后缀=KB。
 *                                                        */
	while (*after_num != '\0' && isspace((unsigned char) *after_num))
		after_num++;

	if (*after_num != '\0')
	{
		suffix = after_num;
		if (*after_num == 'k')
		{
			/* KB 是期望单位             */
			after_num++;
		}
		else if (*after_num == 'M')
		{
			after_num++;
			result *= 1024.0;
		}
	}

	/* 其余仅可为空白                     */
	while (*after_num != '\0' && isspace((unsigned char) *after_num))
		after_num++;

	if (*after_num != '\0')
		pg_fatal("invalid --max-rate unit: \"%s\"", suffix);

	/* 有效整数   */
	if ((uint64) result != (uint64) ((uint32) result))
		pg_fatal("transfer rate \"%s\" exceeds integer range", src);

	/* * 服务器也会查范围，但避免无意义值发
 * 起连接。                                                     */
	if (result < MAX_RATE_LOWER || result > MAX_RATE_UPPER)
		pg_fatal("transfer rate \"%s\" is out of range", src);

	return (int32) result;
}

/* * -Z/--compress 基础解析。
 *
 * 不需全懂用户意图，但需知客户端/服
 * 务端压缩或未指定，算法名与详情分开。
 *
 * 如 --compress client-lz4:6 => (a)
 * 客户端 (b) lz4 (c) 6。前/后缀均可
 * 选。算法名必需，纯数字默认 gzip。
 *
 * 此阶段不验证。client-turkey:sandwich
 * => 算法 turkey，详情 sandwich。稍后
 * 确认合法性。                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             */
static void
backup_parse_compress_options(char *option, char **algorithm, char **detail,
							  CompressionLocation *locationres)
{
	/* * 剥 client-/server- 前缀定位置。
 *                                  */
	if (strncmp(option, "server-", 7) == 0)
	{
		*locationres = COMPRESS_LOCATION_SERVER;
		option += 7;
	}
	else if (strncmp(option, "client-", 7) == 0)
	{
		*locationres = COMPRESS_LOCATION_CLIENT;
		option += 7;
	}
	else
		*locationres = COMPRESS_LOCATION_UNSPECIFIED;

	/* 回退通用解析算法和详情                           */
	parse_compress_options(option, algorithm, detail);
}

/* * 读 COPY 数据流，每块调回调。
 *                                       */
static void
ReceiveCopyData(PGconn *conn, WriteDataCallback callback,
				void *callback_data)
{
	PGresult   *res;

	/* 获取 COPY 数据流     */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
		pg_fatal("could not get COPY data stream: %s",
				 PQerrorMessage(conn));
	PQclear(res);

	/* 循环处理直到完成     */
	while (1)
	{
		int			r;
		char	   *copybuf;

		r = PQgetCopyData(conn, &copybuf, 0);
		if (r == -1)
		{
			/* 块结束     */
			break;
		}
		else if (r == -2)
			pg_fatal("could not read COPY data: %s",
					 PQerrorMessage(conn));

		if (bgchild_exited)
			pg_fatal("background process terminated unexpectedly");

		(*callback) (r, copybuf, callback_data);

		PQfreemem(copybuf);
	}
}

/* * 依选项定如何处理归档：直接写、压缩
 * 或提取 tar 成员。此函数定
 * bbstreamer 类型，收数据时执行。                                                                                                                                                                                                                                                                                                                                      */
static bbstreamer *
CreateBackupStreamer(char *archive_name, char *spclocation,
					 bbstreamer **manifest_inject_streamer_p,
					 bool is_recovery_guc_supported,
					 bool expect_unterminated_tarfile,
					 pg_compress_specification *compress)
{
	bbstreamer *streamer = NULL;
	bbstreamer *manifest_inject_streamer = NULL;
	bool		inject_manifest;
	bool		is_tar,
				is_tar_gz,
				is_tar_lz4,
				is_tar_zstd,
				is_compressed_tar;
	bool		must_parse_archive;
	int			archive_name_len = strlen(archive_name);

	/* * 通常清单单独输出，写 stdout 则注入
 * 唯一 tar 文件。                                                                                                                  */
	inject_manifest = (format == 't' && strcmp(basedir, "-") == 0 && manifest);

	/* tar 归档？          */
	is_tar = (archive_name_len > 4 &&
			  strcmp(archive_name + archive_name_len - 4, ".tar") == 0);

	/* .tar.gz 归档？          */
	is_tar_gz = (archive_name_len > 7 &&
				 strcmp(archive_name + archive_name_len - 7, ".tar.gz") == 0);

	/* .tar.lz4 归档？          */
	is_tar_lz4 = (archive_name_len > 8 &&
				  strcmp(archive_name + archive_name_len - 8, ".tar.lz4") == 0);

	/* .tar.zst 归档？          */
	is_tar_zstd = (archive_name_len > 8 &&
				   strcmp(archive_name + archive_name_len - 8, ".tar.zst") == 0);

	/* 压缩 tar？                       */
	is_compressed_tar = is_tar_gz || is_tar_lz4 || is_tar_zstd;

	/* * 注入清单到压缩 tar 需解压-解析-重建
 * -重压，效率低。拒绝让用户改配置。                                                                                                                                                                                                                                                                                                                         */
	if (inject_manifest && is_compressed_tar)
	{
		pg_log_error("cannot inject manifest into a compressed tar file");
		pg_log_error_hint("Use client-side compression, send the output to a directory rather than standard output, or use %s.",
						  "--no-manifest");
		exit(1);
	}

	/* * 需解析归档若：(1) 提取 (2) 注入清
 * 单或恢复配置。仅支持 tar 解析。                                                                                                                */
	must_parse_archive = (format == 'p' || inject_manifest ||
						  (spclocation == NULL && writerecoveryconf));

	/* 目前仅支持 tar 归档解析                    */
	if (must_parse_archive && !is_tar && !is_compressed_tar)
	{
		pg_log_error("cannot parse archive \"%s\"", archive_name);
		pg_log_error_detail("Only tar archives can be parsed.");
		if (format == 'p')
			pg_log_error_detail("Plain format requires pg_basebackup to parse the archive.");
		if (inject_manifest)
			pg_log_error_detail("Using - as the output directory requires pg_basebackup to parse the archive.");
		if (writerecoveryconf)
			pg_log_error_detail("The -R option requires pg_basebackup to parse the archive.");
		exit(1);
	}

	if (format == 'p')
	{
		const char *directory;

		/* * 纯格式须提取。主表空间写基目录，
 * 其他写服务器位置（应用映射后）。
 *
 * 就地表空间 spclocation 相对路径，
 * 前加 basedir 转绝对。                                                                                                                                                                                                                                                                                         */
		if (spclocation == NULL)
			directory = basedir;
		else if (!is_absolute_path(spclocation))
			directory = psprintf("%s/%s", basedir, spclocation);
		else
			directory = get_tablespace_mapping(spclocation);
		streamer = bbstreamer_extractor_new(directory,
											get_tablespace_mapping,
											progress_update_filename);
	}
	else
	{
		FILE	   *archive_file;
		char		archive_filename[MAXPGPATH];

		/* * tar 格式只写不提取。通常写指定名，
 * basedir="-" 写 stdout。                                                                                                                                                            */
		if (strcmp(basedir, "-") == 0)
		{
			snprintf(archive_filename, sizeof(archive_filename), "-");
			archive_file = stdout;
		}
		else
		{
			snprintf(archive_filename, sizeof(archive_filename),
					 "%s/%s", basedir, archive_name);
			archive_file = NULL;
		}

		if (compress->algorithm == PG_COMPRESSION_NONE)
			streamer = bbstreamer_plain_writer_new(archive_filename,
												   archive_file);
		else if (compress->algorithm == PG_COMPRESSION_GZIP)
		{
			strlcat(archive_filename, ".gz", sizeof(archive_filename));
			streamer = bbstreamer_gzip_writer_new(archive_filename,
												  archive_file, compress);
		}
		else if (compress->algorithm == PG_COMPRESSION_LZ4)
		{
			strlcat(archive_filename, ".lz4", sizeof(archive_filename));
			streamer = bbstreamer_plain_writer_new(archive_filename,
												   archive_file);
			streamer = bbstreamer_lz4_compressor_new(streamer, compress);
		}
		else if (compress->algorithm == PG_COMPRESSION_ZSTD)
		{
			strlcat(archive_filename, ".zst", sizeof(archive_filename));
			streamer = bbstreamer_plain_writer_new(archive_filename,
												   archive_file);
			streamer = bbstreamer_zstd_compressor_new(streamer, compress);
		}
		else
		{
			Assert(false);		/* 不可达     */
		}

		/* * 需解析归档则也需重归档，tar 输出
 * 下解析只为注入。                                                                                                                                                       */
		if (must_parse_archive)
			streamer = bbstreamer_tar_archiver_new(streamer);
		progress_update_filename(archive_filename);
	}

	/* * 需注入清单在此完成，免 tar 细节。
 *                                                                                                                                                             */
	if (inject_manifest)
		manifest_inject_streamer = streamer;

	/* * 主表空间且需写恢复信息执行。
 *                                                                */
	if (spclocation == NULL && writerecoveryconf)
	{
		Assert(must_parse_archive);
		streamer = bbstreamer_recovery_injector_new(streamer,
													is_recovery_guc_supported,
													recoveryconfcontents);
	}

	/* * 涉及理解归档内容需解析。否则跳过，
 * 旧服务器未终止 tar 时加终止符。                                                                                                                                                                                                      */
	if (must_parse_archive)
		streamer = bbstreamer_tar_parser_new(streamer);
	else if (expect_unterminated_tarfile)
		streamer = bbstreamer_tar_terminator_new(streamer);

	/* * 用户请求服务器压缩且客户端提取时
 * 需解压。                                                                     */
	if (format == 'p')
	{
		if (is_tar_gz)
			streamer = bbstreamer_gzip_decompressor_new(streamer);
		else if (is_tar_lz4)
			streamer = bbstreamer_lz4_decompressor_new(streamer);
		else if (is_tar_zstd)
			streamer = bbstreamer_zstd_decompressor_new(streamer);
	}

	/* 返回结果        */
	*manifest_inject_streamer_p = manifest_inject_streamer;
	return streamer;
}

/* * 收服务器所有归档及清单（如存在），
 * 作为单 COPY 流。                                              */
static void
ReceiveArchiveStream(PGconn *conn, pg_compress_specification *compress)
{
	ArchiveStreamState state;

	/* 设初始状态       */
	memset(&state, 0, sizeof(state));
	state.tablespacenum = -1;
	state.compress = compress;

	/* 实际工作在 ReceiveArchiveStreamChunk 完成        */
	ReceiveCopyData(conn, ReceiveArchiveStreamChunk, &state);

	/* 清单已写文件则关闭                                */
	if (state.manifest_file !=NULL)
	{
		fclose(state.manifest_file);
		state.manifest_file = NULL;
	}

	/* * 缓冲的清单注入输出 tar 文件。
 *                                                           */
	if (state.manifest_inject_streamer != NULL &&
		state.manifest_buffer != NULL)
	{
		bbstreamer_inject_file(state.manifest_inject_streamer,
							   "backup_manifest",
							   state.manifest_buffer->data,
							   state.manifest_buffer->len);
		destroyPQExpBuffer(state.manifest_buffer);
		state.manifest_buffer = NULL;
	}

	/* 仍有归档处理中则结束                           */
	if (state.streamer != NULL)
	{
		bbstreamer_finalize(state.streamer);
		bbstreamer_free(state.streamer);
		state.streamer = NULL;
	}
}

/* * 收 COPY 流一个数据块，含所有归档
 * 和清单。                                                                  */
static void
ReceiveArchiveStreamChunk(size_t r, char *copybuf, void *callback_data)
{
	ArchiveStreamState *state = callback_data;
	size_t		cursor = 0;

	/* CopyData 消息以类型字节开始           */
	switch (GetCopyDataByte(r, copybuf, &cursor))
	{
		case 'n':
			{
				/* 新归档    */
				char	   *archive_name;
				char	   *spclocation;

				/* * 每表空间结束强制进度。新表空间始于
 * 前一个结束，首个除外。                                                                                        */
				if (++state->tablespacenum > 0)
					progress_report(state->tablespacenum, true, false);

				/* 健全检查  */
				if (state->manifest_buffer != NULL ||
					state->manifest_file !=NULL)
					pg_fatal("archives must precede manifest");

				/* 解析 CopyData 消息其余            */
				archive_name = GetCopyDataString(r, copybuf, &cursor);
				spclocation = GetCopyDataString(r, copybuf, &cursor);
				GetCopyDataEnd(r, copybuf, cursor);

				/* * 归档名健全：非空、非.始、无路径
 * 分隔。                                                                                                       */
				if (archive_name[0] == '\0' || archive_name[0] == '.' ||
					strchr(archive_name, '/') != NULL ||
					strchr(archive_name, '\\') != NULL)
					pg_fatal("invalid archive name: \"%s\"",
							 archive_name);

				/* * 空 spclocation 视为 NULL。数据目
 * 录自身有此情况，表空间归档无。                                                                                              */
				if (spclocation[0] == '\0')
					spclocation = NULL;

				/* 结束处理先前归档             */
				if (state->streamer != NULL)
				{
					bbstreamer_finalize(state->streamer);
					bbstreamer_free(state->streamer);
					state->streamer = NULL;
				}

				/* * 创建备份流处理器，除非有备份目标
 * 则由服务器定。                                                                                                           */
				if (backup_target == NULL)
				{
					/* * 恢复 GUC 已知支持，此协议仅 v15+。
 *                                                             */
					state->streamer =
						CreateBackupStreamer(archive_name,
											 spclocation,
											 &state->manifest_inject_streamer,
											 true, false,
											 state->compress);
				}
				break;
			}

		case 'd':
			{
				/* 归档或清单数据     */
				if (state->manifest_buffer != NULL)
				{
					/* 清单数据，缓冲内存      */
					appendPQExpBuffer(state->manifest_buffer, copybuf + 1,
									  r - 1);
				}
				else if (state->manifest_file !=NULL)
				{
					/* 清单数据，写磁盘      */
					if (fwrite(copybuf + 1, r - 1, 1,
							   state->manifest_file) != 1)
					{
						/* * fwrite 未设 errno 假设磁盘不足。
 *                                                                  */
						if (errno == 0)
							errno = ENOSPC;
						pg_fatal("could not write to file \"%s\": %m",
								 state->manifest_filename);
					}
				}
				else if (state->streamer != NULL)
				{
					/* 归档数据  */
					bbstreamer_content(state->streamer, NULL, copybuf + 1,
									   r - 1, BBSTREAMER_UNKNOWN);
				}
				else
					pg_fatal("unexpected payload data");
				break;
			}

		case 'p':
			{
				/* * 进度报告。
 *
 * 剩余应为 8 字节完成量。
 *                                                                         */
				totaldone = GetCopyDataUInt64(r, copybuf, &cursor);
				GetCopyDataEnd(r, copybuf, cursor);

				/* * 服务器不应频发进度，故每次强制
 * 更新。                                                                     */
				progress_report(state->tablespacenum, true, false);
				break;
			}

		case 'm':
			{
				/* * 清单数据将发送。此消息无额外负载。
 *                                                           */
				GetCopyDataEnd(r, copybuf, cursor);

				/* * 有备份目标清单由服务器处理，否则
 * 自行处理。                                                                                           */
				if (backup_target == NULL)
				{
					/* * 需注入清单缓冲内存，否则写临时
 * 文件。                                                                                                                    */
					if (state->manifest_inject_streamer != NULL)
						state->manifest_buffer = createPQExpBuffer();
					else
					{
						snprintf(state->manifest_filename,
								 sizeof(state->manifest_filename),
								 "%s/backup_manifest.tmp", basedir);
						state->manifest_file =
							fopen(state->manifest_filename, "wb");
						if (state->manifest_file == NULL)
							pg_fatal("could not create file \"%s\": %m",
									 state->manifest_filename);
					}
				}
				break;
			}

		default:
			ReportCopyDataParseError(r, copybuf);
			break;
	}
}

/* * 从 CopyData 取单字节。
 *
 * 无剩余则退出。
 *                 */
static char
GetCopyDataByte(size_t r, char *copybuf, size_t *cursor)
{
	if (*cursor >= r)
		ReportCopyDataParseError(r, copybuf);

	return copybuf[(*cursor)++];
}

/* * 从 CopyData 取 NUL 尾字符串。
 *
 * 找不到 NUL 尾退出。
 *                                      */
static char *
GetCopyDataString(size_t r, char *copybuf, size_t *cursor)
{
	size_t		startpos = *cursor;
	size_t		endpos = startpos;

	while (1)
	{
		if (endpos >= r)
			ReportCopyDataParseError(r, copybuf);
		if (copybuf[endpos] == '\0')
			break;
		++endpos;
	}

	*cursor = endpos + 1;
	return &copybuf[startpos];
}

/* * 从 CopyData 取 uint64。
 *
 * 不足 8 字节退出。
 *                                                          */
static uint64
GetCopyDataUInt64(size_t r, char *copybuf, size_t *cursor)
{
	uint64		result;

	if (*cursor + sizeof(uint64) > r)
		ReportCopyDataParseError(r, copybuf);
	memcpy(&result, &copybuf[*cursor], sizeof(uint64));
	*cursor += sizeof(uint64);
	return pg_ntoh64(result);
}

/* * 未完整解析消息退出。
 *                */
static void
GetCopyDataEnd(size_t r, char *copybuf, size_t cursor)
{
	if (r != cursor)
		ReportCopyDataParseError(r, copybuf);
}

/* * 报告 CopyData 解析失败后退出。
 *
 * 调试助：提示失败消息类型。不详但
 * 不必为不可能情形多写代码。                                                                                                                                                                                   */
static void
ReportCopyDataParseError(size_t r, char *copybuf)
{
	if (r == 0)
		pg_fatal("empty COPY message");
	else
		pg_fatal("malformed COPY message of type %d, length %zu",
				 copybuf[0], r);
}

/* * 收原始 tar 数据流式到目标。写
 * stdout 单 tar 同时也收注清单。                                                                                                                             */
static void
ReceiveTarFile(PGconn *conn, char *archive_name, char *spclocation,
			   bool tablespacenum, pg_compress_specification *compress)
{
	WriteTarState state;
	bbstreamer *manifest_inject_streamer;
	bool		is_recovery_guc_supported;
	bool		expect_unterminated_tarfile;

	/* COPY 数据全透传备份流处理器             */
	memset(&state, 0, sizeof(state));
	is_recovery_guc_supported =
		PQserverVersion(conn) >= MINIMUM_VERSION_FOR_RECOVERY_GUC;
	expect_unterminated_tarfile =
		PQserverVersion(conn) < MINIMUM_VERSION_FOR_TERMINATED_TARFILE;
	state.streamer = CreateBackupStreamer(archive_name, spclocation,
										  &manifest_inject_streamer,
										  is_recovery_guc_supported,
										  expect_unterminated_tarfile,
										  compress);
	state.tablespacenum = tablespacenum;
	ReceiveCopyData(conn, ReceiveTarCopyChunk, &state);
	progress_update_filename(NULL);

	/* * 是否需注清单由 CreateBackupStreamer
 * 定；需时 manifest_inject_streamer
 * 非 NULL。                                                                                                                                        */
	if (manifest_inject_streamer != NULL)
	{
		PQExpBufferData buf;

		/* 整份备份清单读入缓冲                  */
		initPQExpBuffer(&buf);
		ReceiveBackupManifestInMemory(conn, &buf);
		if (PQExpBufferDataBroken(buf))
			pg_fatal("out of memory");

		/* 注入输出 tar 文件            */
		bbstreamer_inject_file(manifest_inject_streamer, "backup_manifest",
							   buf.data, buf.len);

		/* 释放内存 */
		termPQExpBuffer(&buf);
	}

	/* 清理   */
	bbstreamer_finalize(state.streamer);
	bbstreamer_free(state.streamer);

	progress_report(tablespacenum, true, false);

	/* * 暂不同步 tar 文件，结束时统一。
 *                                       */
}

/* * 收一个 tar 格式数据块。
 *                     */
static void
ReceiveTarCopyChunk(size_t r, char *copybuf, void *callback_data)
{
	WriteTarState *state = callback_data;

	bbstreamer_content(state->streamer, NULL, copybuf, r, BBSTREAMER_UNKNOWN);

	totaldone += r;
	progress_report(state->tablespacenum, false, false);
}


/* * 检索表空间路径，依 -T 返重定位
 * 或原始。                                            */
static const char *
get_tablespace_mapping(const char *dir)
{
	TablespaceListCell *cell;
	char		canon_dir[MAXPGPATH];

	/* 规范化路径保持比较一致            */
	strlcpy(canon_dir, dir, sizeof(canon_dir));
	canonicalize_path(canon_dir);

	for (cell = tablespace_dirs.head; cell; cell = cell->next)
		if (strcmp(canon_dir, cell->old_dir) == 0)
			return cell->new_dir;

	return dir;
}

/* * 收备份清单写文件。
 *                                 */
static void
ReceiveBackupManifest(PGconn *conn)
{
	WriteManifestState state;

	snprintf(state.filename, sizeof(state.filename),
			 "%s/backup_manifest.tmp", basedir);
	state.file = fopen(state.filename, "wb");
	if (state.file == NULL)
		pg_fatal("could not create file \"%s\": %m", state.filename);

	ReceiveCopyData(conn, ReceiveBackupManifestChunk, &state);

	fclose(state.file);
}

/* * 收清单一块写文件。
 *                                              */
static void
ReceiveBackupManifestChunk(size_t r, char *copybuf, void *callback_data)
{
	WriteManifestState *state = callback_data;

	errno = 0;
	if (fwrite(copybuf, r, 1, state->file) != 1)
	{
		/* write 未设 errno 假设磁盘不足                      */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to file \"%s\": %m", state->filename);
	}
}

/* * 收备份清单写文件。
 *                                 */
static void
ReceiveBackupManifestInMemory(PGconn *conn, PQExpBuffer buf)
{
	ReceiveCopyData(conn, ReceiveBackupManifestInMemoryChunk, buf);
}

/* * 收清单一块写文件。
 *                                              */
static void
ReceiveBackupManifestInMemoryChunk(size_t r, char *copybuf,
								   void *callback_data)
{
	PQExpBuffer buf = callback_data;

	appendPQExpBuffer(buf, copybuf, r);
}

static void
BaseBackup(char *compression_algorithm, char *compression_detail,
		   CompressionLocation compressloc, pg_compress_specification *client_compress)
{
	PGresult   *res;
	char	   *sysidentifier;
	TimeLineID	latesttli;
	TimeLineID	starttli;
	char	   *basebkp;
	int			i;
	char		xlogstart[64];
	char		xlogend[64] = {0};
	int			minServerMajor,
				maxServerMajor;
	int			serverVersion,
				serverMajor;
	int			writing_to_stdout;
	bool		use_new_option_syntax = false;
	PQExpBufferData buf;

	Assert(conn != NULL);
	initPQExpBuffer(&buf);

	/* * 查服务器版本。BASE_BACKUP 9.1
 * 引入，不能用于更旧服务器。                                        */
	minServerMajor = 901;
	maxServerMajor = PG_VERSION_NUM / 100;
	serverVersion = PQserverVersion(conn);
	serverMajor = serverVersion / 100;
	if (serverMajor < minServerMajor || serverMajor > maxServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		pg_fatal("incompatible server version %s",
				 serverver ? serverver : "'unknown'");
	}
	if (serverMajor >= 1500)
		use_new_option_syntax = true;

	/* * 请求 WAL 流时查服务器是否够新。
 *                                            */
	if (includewal == STREAM_WAL && !CheckServerVersionForStreaming(conn))
	{
		/* * 错误已在 CheckServerVersionForStreaming
 * 写入，附加 -X none 提示。                                  */
		pg_log_error_hint("Use -X none or -X fetch to disable log streaming.");
		exit(1);
	}

	/* * 按需构建配置文件
 *                           */
	if (writerecoveryconf)
		recoveryconfcontents = GenerateRecoveryConfig(conn, replication_slot);

	/*
	 * 运行 IDENTIFY_SYSTEM 取时间线
	            */
	if (!RunIdentifySystem(conn, &sysidentifier, &latesttli, NULL, NULL))
		exit(1);

	/*
	 * 开始实际备份
	      */
	AppendStringCommandOption(&buf, use_new_option_syntax, "LABEL", label);
	if (estimatesize)
		AppendPlainCommandOption(&buf, use_new_option_syntax, "PROGRESS");
	if (includewal == FETCH_WAL)
		AppendPlainCommandOption(&buf, use_new_option_syntax, "WAL");
	if (fastcheckpoint)
	{
		if (use_new_option_syntax)
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "CHECKPOINT", "fast");
		else
			AppendPlainCommandOption(&buf, use_new_option_syntax, "FAST");
	}
	if (includewal != NO_WAL)
	{
		if (use_new_option_syntax)
			AppendIntegerCommandOption(&buf, use_new_option_syntax, "WAIT", 0);
		else
			AppendPlainCommandOption(&buf, use_new_option_syntax, "NOWAIT");
	}
	if (maxrate > 0)
		AppendIntegerCommandOption(&buf, use_new_option_syntax, "MAX_RATE",
								   maxrate);
	if (format == 't')
		AppendPlainCommandOption(&buf, use_new_option_syntax, "TABLESPACE_MAP");
	if (!verify_checksums)
	{
		if (use_new_option_syntax)
			AppendIntegerCommandOption(&buf, use_new_option_syntax,
									   "VERIFY_CHECKSUMS", 0);
		else
			AppendPlainCommandOption(&buf, use_new_option_syntax,
									 "NOVERIFY_CHECKSUMS");
	}

	if (manifest)
	{
		AppendStringCommandOption(&buf, use_new_option_syntax, "MANIFEST",
								  manifest_force_encode ? "force-encode" : "yes");
		if (manifest_checksums != NULL)
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "MANIFEST_CHECKSUMS", manifest_checksums);
	}

	if (backup_target != NULL)
	{
		char	   *colon;

		if (serverMajor < 1500)
			pg_fatal("backup targets are not supported by this server version");

		if (writerecoveryconf)
			pg_fatal("recovery configuration cannot be written when a backup target is used");

		AppendPlainCommandOption(&buf, use_new_option_syntax, "TABLESPACE_MAP");

		if ((colon = strchr(backup_target, ':')) == NULL)
		{
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "TARGET", backup_target);
		}
		else
		{
			char	   *target;

			target = pnstrdup(backup_target, colon - backup_target);
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "TARGET", target);
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "TARGET_DETAIL", colon + 1);
		}
	}
	else if (serverMajor >= 1500)
		AppendStringCommandOption(&buf, use_new_option_syntax,
								  "TARGET", "client");

	if (compressloc == COMPRESS_LOCATION_SERVER)
	{
		if (!use_new_option_syntax)
			pg_fatal("server does not support server-side compression");
		AppendStringCommandOption(&buf, use_new_option_syntax,
								  "COMPRESSION", compression_algorithm);
		if (compression_detail != NULL)
			AppendStringCommandOption(&buf, use_new_option_syntax,
									  "COMPRESSION_DETAIL",
									  compression_detail);
	}

	if (verbose)
		pg_log_info("initiating base backup, waiting for checkpoint to complete");

	if (showprogress && !verbose)
	{
		fprintf(stderr, _("waiting for checkpoint"));
		if (isatty(fileno(stderr)))
			fprintf(stderr, "\r");
		else
			fprintf(stderr, "\n");
	}

	if (use_new_option_syntax && buf.len > 0)
		basebkp = psprintf("BASE_BACKUP (%s)", buf.data);
	else
		basebkp = psprintf("BASE_BACKUP %s", buf.data);

	if (PQsendQuery(conn, basebkp) == 0)
		pg_fatal("could not send replication command \"%s\": %s",
				 "BASE_BACKUP", PQerrorMessage(conn));

	/*
	 * 获取起始 WAL 位置
	       */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not initiate base backup: %s",
				 PQerrorMessage(conn));
	if (PQntuples(res) != 1)
		pg_fatal("server returned unexpected response to BASE_BACKUP command; got %d rows and %d fields, expected %d rows and %d fields",
				 PQntuples(res), PQnfields(res), 1, 2);

	strlcpy(xlogstart, PQgetvalue(res, 0, 0), sizeof(xlogstart));

	if (verbose)
		pg_log_info("checkpoint completed");

	/* * 9.3+ 发起始 TLI。旧服务器假设与
 * IDENTIFY_SYSTEM 最新同。                                                                                  */
	if (PQnfields(res) >= 2)
		starttli = atoi(PQgetvalue(res, 0, 1));
	else
		starttli = latesttli;
	PQclear(res);

	if (verbose && includewal != NO_WAL)
		pg_log_info("write-ahead log start point: %s on timeline %u",
					xlogstart, starttli);

	/*
	 * 获取头部
	   */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not get backup header: %s",
				 PQerrorMessage(conn));
	if (PQntuples(res) < 1)
		pg_fatal("no data returned from server");

	/*
	 * 汇总总大小供进度
	                      */
	totalsize_kb = totaldone = 0;
	tablespacecount = PQntuples(res);
	for (i = 0; i < PQntuples(res); i++)
	{
		totalsize_kb += atol(PQgetvalue(res, i, 2));

		/* * 验证表空间目录空。跳首个（可重定
 * 位）。
 *
 * tar 和服务器存储目标跳过，因不
 * 写不创。                                                                                                                                                                                                                                                                                          */
		if (backup_target == NULL && format == 'p' && !PQgetisnull(res, i, 1))
		{
			char	   *path = PQgetvalue(res, i, 1);

			if (is_absolute_path(path))
				path = unconstify(char *, get_tablespace_mapping(path));
			else
			{
				/* 就地表空间，前加 basedir                    */
				path = psprintf("%s/%s", basedir, path);
			}

			verify_dir_is_empty_or_create(path, &made_tablespace_dirs, &found_tablespace_dirs);
		}
	}

	/* * 写 stdout 要求单表空间
 *                        */
	writing_to_stdout = format == 't' && basedir != NULL &&
		strcmp(basedir, "-") == 0;
	if (writing_to_stdout && PQntuples(res) > 1)
		pg_fatal("can only write single tablespace to stdout, database has %d",
				 PQntuples(res));

	/* * 流 WAL 时先启流会话再收数据。
 *                                                                  */
	if (includewal == STREAM_WAL)
	{
		pg_compress_algorithm wal_compress_algorithm;
		int			wal_compress_level;

		if (verbose)
			pg_log_info("starting background WAL receiver");

		if (client_compress->algorithm == PG_COMPRESSION_GZIP)
		{
			wal_compress_algorithm = PG_COMPRESSION_GZIP;
			wal_compress_level = client_compress->level;
		}
		else
		{
			wal_compress_algorithm = PG_COMPRESSION_NONE;
			wal_compress_level = 0;
		}

		StartLogStreamer(xlogstart, starttli, sysidentifier,
						 wal_compress_algorithm,
						 wal_compress_level);
	}

	if (serverMajor >= 1500)
	{
		/* 收含所有内容的单 tar 流             */
		ReceiveArchiveStream(conn, client_compress);
	}
	else
	{
		/* 依次收每表空间 tar 文件               */
		for (i = 0; i < PQntuples(res); i++)
		{
			char		archive_name[MAXPGPATH];
			char	   *spclocation;

			/* * 写 tar 时主数据目录 base.tar，
 * 其他 <oid>.tar。压缩时
 * CreateBackupStreamer 加扩展名。                                                                                                                                                                                                                                            */
			if (PQgetisnull(res, i, 0))
			{
				strlcpy(archive_name, "base.tar", sizeof(archive_name));
				spclocation = NULL;
			}
			else
			{
				snprintf(archive_name, sizeof(archive_name),
						 "%s.tar", PQgetvalue(res, i, 0));
				spclocation = PQgetvalue(res, i, 1);
			}

			ReceiveTarFile(conn, archive_name, spclocation, i,
						   client_compress);
		}

		/* * 收备份清单（如适用）。
 *
 * 写 stdout 时 ReceiveTarFile 已处
 * 理。此配置不支持多文件。
 *
 * 旧服务器不发清单，不尝试。                                                                                                                                                                                                                                        */
		if (!writing_to_stdout && manifest)
			ReceiveBackupManifest(conn);
	}

	if (showprogress)
	{
		progress_update_filename(NULL);
		progress_report(PQntuples(res), true, true);
	}

	PQclear(res);

	/*
	 * 获取停止位置
	    */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("backup failed: %s",
				 PQerrorMessage(conn));
	if (PQntuples(res) != 1)
		pg_fatal("no write-ahead log end position returned from server");
	strlcpy(xlogend, PQgetvalue(res, 0, 0), sizeof(xlogend));
	if (verbose && includewal != NO_WAL)
		pg_log_info("write-ahead log end point: %s", xlogend);
	PQclear(res);

	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		if (sqlstate &&
			strcmp(sqlstate, ERRCODE_DATA_CORRUPTED) == 0)
		{
			pg_log_error("checksum error occurred");
			checksum_failure = true;
		}
		else
		{
			pg_log_error("final receive failed: %s",
						 PQerrorMessage(conn));
		}
		exit(1);
	}

	if (bgchild > 0)
	{
#ifndef WIN32
		int			status;
		pid_t		r;
#else
		DWORD		status;

		/* * 取 bgchild 指针版，避 WIN64
 * 转换警告。                                                            */
		intptr_t	bgchild_handle = bgchild;
		uint32		hi,
					lo;
#endif

		if (verbose)
			pg_log_info("waiting for background process to finish streaming ...");

#ifndef WIN32
		if (write(bgpipe[1], xlogend, strlen(xlogend)) != strlen(xlogend))
			pg_fatal("could not send command to background pipe: %m");

		/* 仅等后台进程退出                     */
		r = waitpid(bgchild, &status, 0);
		if (r == (pid_t) -1)
			pg_fatal("could not wait for child process: %m");
		if (r != bgchild)
			pg_fatal("child %d died, expected %d", (int) r, (int) bgchild);
		if (status != 0)
			pg_fatal("%s", wait_result_to_str(status));
		/* 正常退出！               */
#else							/* WIN32 */

		/* * Windows 同进程直接存变量设标志。
 *                                                                                                                 */
		if (sscanf(xlogend, "%X/%X", &hi, &lo) != 2)
			pg_fatal("could not parse write-ahead log location \"%s\"",
					 xlogend);
		xlogendptr = ((uint64) hi) << 32 | lo;
		InterlockedIncrement(&has_xlogendptr);

		/* 先等线程退出                */
		if (WaitForSingleObjectEx((HANDLE) bgchild_handle, INFINITE, FALSE) !=
			WAIT_OBJECT_0)
		{
			_dosmaperr(GetLastError());
			pg_fatal("could not wait for child thread: %m");
		}
		if (GetExitCodeThread((HANDLE) bgchild_handle, &status) == 0)
		{
			_dosmaperr(GetLastError());
			pg_fatal("could not get child thread exit status: %m");
		}
		if (status != 0)
			pg_fatal("child thread exited with error %u",
					 (unsigned int) status);
		/* 正常退出                 */
#endif
	}

	/* 释放配置文件内容             */
	destroyPQExpBuffer(recoveryconfcontents);

	/* * COPY 数据结束，结果循环内已查。
 *                        */
	PQclear(res);
	PQfinish(conn);
	conn = NULL;

	/* * 备份完成持久化磁盘。tar 同步父目
 * 录（tar 未单独同步）。纯格式同步
 * 基目录含表空间。错非致命。
 *
 * 有备份目标跳过（不写本地）。                                                                                                                                                                                                                                                                       */
	if (do_sync && backup_target == NULL)
	{
		if (verbose)
			pg_log_info("syncing data to disk ...");
		if (format == 't')
		{
			if (strcmp(basedir, "-") != 0)
				(void) fsync_dir_recurse(basedir);
		}
		else
		{
			(void) fsync_pgdata(basedir, serverVersion);
		}
	}

	/* * 同步后持久重命名 backup_manifest
 * .tmp。此前崩溃无清单，降误认
 * 风险。                                                                                                                                                                                                                                                            */
	if (!writing_to_stdout && manifest && backup_target == NULL)
	{
		char		tmp_filename[MAXPGPATH];
		char		filename[MAXPGPATH];

		if (verbose)
			pg_log_info("renaming backup_manifest.tmp to backup_manifest");

		snprintf(tmp_filename, MAXPGPATH, "%s/backup_manifest.tmp", basedir);
		snprintf(filename, MAXPGPATH, "%s/backup_manifest", basedir);

		if (do_sync)
		{
			/* durable_rename 失败自写日志                           */
			if (durable_rename(tmp_filename, filename) != 0)
				exit(1);
		}
		else
		{
			if (rename(tmp_filename, filename) != 0)
				pg_fatal("could not rename file \"%s\" to \"%s\": %m",
						 tmp_filename, filename);
		}
	}

	if (verbose)
		pg_log_info("base backup completed");
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"pgdata", required_argument, NULL, 'D'},
		{"format", required_argument, NULL, 'F'},
		{"checkpoint", required_argument, NULL, 'c'},
		{"create-slot", no_argument, NULL, 'C'},
		{"max-rate", required_argument, NULL, 'r'},
		{"write-recovery-conf", no_argument, NULL, 'R'},
		{"slot", required_argument, NULL, 'S'},
		{"target", required_argument, NULL, 't'},
		{"tablespace-mapping", required_argument, NULL, 'T'},
		{"wal-method", required_argument, NULL, 'X'},
		{"gzip", no_argument, NULL, 'z'},
		{"compress", required_argument, NULL, 'Z'},
		{"label", required_argument, NULL, 'l'},
		{"no-clean", no_argument, NULL, 'n'},
		{"no-sync", no_argument, NULL, 'N'},
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"status-interval", required_argument, NULL, 's'},
		{"verbose", no_argument, NULL, 'v'},
		{"progress", no_argument, NULL, 'P'},
		{"waldir", required_argument, NULL, 1},
		{"no-slot", no_argument, NULL, 2},
		{"no-verify-checksums", no_argument, NULL, 3},
		{"no-estimate-size", no_argument, NULL, 4},
		{"no-manifest", no_argument, NULL, 5},
		{"manifest-force-encode", no_argument, NULL, 6},
		{"manifest-checksums", required_argument, NULL, 7},
		{NULL, 0, NULL, 0}
	};
	int			c;

	int			option_index;
	char	   *compression_algorithm = "none";
	char	   *compression_detail = NULL;
	CompressionLocation compressloc = COMPRESS_LOCATION_UNSPECIFIED;
	pg_compress_specification client_compress;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_basebackup"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0
				 || strcmp(argv[1], "--version") == 0)
		{
			puts("pg_basebackup (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	atexit(cleanup_directories_atexit);

	while ((c = getopt_long(argc, argv, "c:Cd:D:F:h:l:nNp:Pr:Rs:S:t:T:U:vwWX:zZ:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'c':
				if (pg_strcasecmp(optarg, "fast") == 0)
					fastcheckpoint = true;
				else if (pg_strcasecmp(optarg, "spread") == 0)
					fastcheckpoint = false;
				else
					pg_fatal("invalid checkpoint argument \"%s\", must be \"fast\" or \"spread\"",
							 optarg);
				break;
			case 'C':
				create_slot = true;
				break;
			case 'd':
				connection_string = pg_strdup(optarg);
				break;
			case 'D':
				basedir = pg_strdup(optarg);
				break;
			case 'F':
				if (strcmp(optarg, "p") == 0 || strcmp(optarg, "plain") == 0)
					format = 'p';
				else if (strcmp(optarg, "t") == 0 || strcmp(optarg, "tar") == 0)
					format = 't';
				else
					pg_fatal("invalid output format \"%s\", must be \"plain\" or \"tar\"",
							 optarg);
				break;
			case 'h':
				dbhost = pg_strdup(optarg);
				break;
			case 'l':
				label = pg_strdup(optarg);
				break;
			case 'n':
				noclean = true;
				break;
			case 'N':
				do_sync = false;
				break;
			case 'p':
				dbport = pg_strdup(optarg);
				break;
			case 'P':
				showprogress = true;
				break;
			case 'r':
				maxrate = parse_max_rate(optarg);
				break;
			case 'R':
				writerecoveryconf = true;
				break;
			case 's':
				if (!option_parse_int(optarg, "-s/--status-interval", 0,
									  INT_MAX / 1000,
									  &standby_message_timeout))
					exit(1);
				standby_message_timeout *= 1000;
				break;
			case 'S':

				/* * 指定复制槽名用永久槽。
 *                                          */
				replication_slot = pg_strdup(optarg);
				temp_replication_slot = false;
				break;
			case 't':
				backup_target = pg_strdup(optarg);
				break;
			case 'T':
				tablespace_list_append(optarg);
				break;
			case 'U':
				dbuser = pg_strdup(optarg);
				break;
			case 'v':
				verbose++;
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
			case 'X':
				if (strcmp(optarg, "n") == 0 ||
					strcmp(optarg, "none") == 0)
				{
					includewal = NO_WAL;
				}
				else if (strcmp(optarg, "f") == 0 ||
						 strcmp(optarg, "fetch") == 0)
				{
					includewal = FETCH_WAL;
				}
				else if (strcmp(optarg, "s") == 0 ||
						 strcmp(optarg, "stream") == 0)
				{
					includewal = STREAM_WAL;
				}
				else
					pg_fatal("invalid wal-method option \"%s\", must be \"fetch\", \"stream\", or \"none\"",
							 optarg);
				break;
			case 'z':
				compression_algorithm = "gzip";
				compression_detail = NULL;
				compressloc = COMPRESS_LOCATION_UNSPECIFIED;
				break;
			case 'Z':
				backup_parse_compress_options(optarg, &compression_algorithm,
											  &compression_detail, &compressloc);
				break;
			case 1:
				xlog_dir = pg_strdup(optarg);
				break;
			case 2:
				no_slot = true;
				break;
			case 3:
				verify_checksums = false;
				break;
			case 4:
				estimatesize = false;
				break;
			case 5:
				manifest = false;
				break;
			case 6:
				manifest_force_encode = true;
				break;
			case 7:
				manifest_checksums = pg_strdup(optarg);
				break;
			default:
				/* getopt_long 已投诉                   */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/* * 非选项参数？
 *         */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* * 设备份目标 client=省略。此逻辑使
 * backup_target==NULL 即本地存储。                                                                                                                            */
	if (backup_target != NULL && strcmp(backup_target, "client") == 0)
	{
		pg_free(backup_target);
		backup_target = NULL;
	}

	/* * --format 与 --target 互斥。无
 * --target 默认 tar。                       */
	if (backup_target != NULL && format != '\0')
	{
		pg_log_error("cannot specify both format and backup target");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (format == '\0')
		format = 'p';

	/* * 目录或备份目标须指定其一。
 *                              */
	if (basedir == NULL && backup_target == NULL)
	{
		pg_log_error("must specify output directory or backup target");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (basedir != NULL && backup_target != NULL)
	{
		pg_log_error("cannot specify both output directory and backup target");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* * 未指定压缩位默认客户端，除
 * --target 只服务器。                                                                                                                    */
	if (compressloc == COMPRESS_LOCATION_UNSPECIFIED)
	{
		if (backup_target == NULL)
			compressloc = COMPRESS_LOCATION_CLIENT;
		else
			compressloc = COMPRESS_LOCATION_SERVER;
	}

	/* * 客户端压缩需解析算法详情，服务
 * 端透传服务器决定。                                                                                                                                                                                                           */
	if (compressloc == COMPRESS_LOCATION_CLIENT)
	{
		pg_compress_algorithm alg;
		char	   *error_detail;

		if (!parse_compress_algorithm(compression_algorithm, &alg))
			pg_fatal("unrecognized compression algorithm: \"%s\"",
					 compression_algorithm);

		parse_compress_specification(alg, compression_detail, &client_compress);
		error_detail = validate_compress_specification(&client_compress);
		if (error_detail != NULL)
			pg_fatal("invalid compression specification: %s",
					 error_detail);
	}
	else
	{
		Assert(compressloc == COMPRESS_LOCATION_SERVER);
		client_compress.algorithm = PG_COMPRESSION_NONE;
		client_compress.options = 0;
	}

	/* * 备份不发客户端不能客户端压缩。
 *                                             */
	if (backup_target != NULL && compressloc == COMPRESS_LOCATION_CLIENT)
	{
		pg_log_error("client-side compression is not possible when a backup target is specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* * 客户端压缩仅 tar 有意义。
 *                                      */
	if (format == 'p' && compressloc == COMPRESS_LOCATION_CLIENT &&
		client_compress.algorithm != PG_COMPRESSION_NONE)
	{
		pg_log_error("only tar mode backups can be compressed");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * WAL 方法健全检查。
	     */
	if (backup_target != NULL && includewal == STREAM_WAL)
	{
		pg_log_error("WAL cannot be streamed when a backup target is specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (format == 't' && includewal == STREAM_WAL && strcmp(basedir, "-") == 0)
	{
		pg_log_error("cannot stream write-ahead logs in tar mode to stdout");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (replication_slot && includewal != STREAM_WAL)
	{
		pg_log_error("replication slots can only be used with WAL streaming");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * 复制槽选项健全检查。
	              */
	if (no_slot)
	{
		if (replication_slot)
		{
			pg_log_error("--no-slot cannot be used with slot name");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
		temp_replication_slot = false;
	}

	if (create_slot)
	{
		if (!replication_slot)
		{
			pg_log_error("%s needs a slot to be specified using --slot",
						 "--create-slot");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}

		if (no_slot)
		{
			pg_log_error("%s and %s are incompatible options",
						 "--create-slot", "--no-slot");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
	}

	/*
	 * WAL 目录健全检查。
	       */
	if (xlog_dir)
	{
		if (backup_target != NULL)
		{
			pg_log_error("WAL directory location cannot be specified along with a backup target");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
		if (format != 'p')
		{
			pg_log_error("WAL directory location can only be specified in plain mode");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}

		/* 清 xlog 目录名，检查绝对路径           */
		canonicalize_path(xlog_dir);
		if (!is_absolute_path(xlog_dir))
		{
			pg_log_error("WAL directory location must be an absolute path");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
	}

	/*
	 * 进度报告选项健全检查。
	             */
	if (showprogress && !estimatesize)
	{
		pg_log_error("%s and %s are incompatible options",
					 "--progress", "--no-estimate-size");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * 备份清单选项健全检查。
	          */
	if (!manifest && manifest_checksums != NULL)
	{
		pg_log_error("%s and %s are incompatible options",
					 "--no-manifest", "--manifest-checksums");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (!manifest && manifest_force_encode)
	{
		pg_log_error("%s and %s are incompatible options",
					 "--no-manifest", "--manifest-force-encode");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* 复制模式连服务器                 */
	conn = GetConnection();
	if (!conn)
	{
		/* 错误已在 GetConnection 写入                */
		exit(1);
	}
	atexit(disconnect_atexit);

#ifndef WIN32

	/* * 捕获 SIGCHLD 处理 WAL 流退出。
 * Windows 无 SIGCHLD，靠后台线程
 * 设变量。Win 流线程崩溃带下进程，
 * 无法捕获。UNIX 崩溃由信号处理
 * 器捕获。                                                                                                                                                                                                                                           */
	pqsignal(SIGCHLD, sigchld_handler);
#endif

	/* * 设 umask 使目录文件权限匹配源
 * 数据目录。
 *
 * pg_mode_mask 默认仅所有者，
 * GetConnection 中从服务端更新。                                                                                                                                                                                                                */
	umask(pg_mode_mask);

	/* 备份清单 v13 起支持                              */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_MANIFESTS)
		manifest = false;

	/* * 有输出目录验证或创建。tar "-"
 * 表 stdout，不做操作。                                                                                                                                  */
	if (basedir != NULL && (format == 'p' || strcmp(basedir, "-") != 0))
		verify_dir_is_empty_or_create(basedir, &made_new_pgdata, &found_existing_pgdata);

	/* 确定远端 xlog 段大小                 */
	if (!RetrieveWalSegSize(conn))
		exit(1);

	/* 需要时创 pg_wal 符号链接   */
	if (xlog_dir)
	{
		char	   *linkloc;

		verify_dir_is_empty_or_create(xlog_dir, &made_new_xlogdir, &found_existing_xlogdir);

		/* * 构造符号链接目标。pg_xlog post-10
 * 集簇已重命名 pg_wal。                                           */
		linkloc = psprintf("%s/%s", basedir,
						   PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
						   "pg_xlog" : "pg_wal");

		if (symlink(xlog_dir, linkloc) != 0)
			pg_fatal("could not create symbolic link \"%s\": %m", linkloc);
		free(linkloc);
	}

	BaseBackup(compression_algorithm, compression_detail, compressloc,
			   &client_compress);

	success = true;
	return 0;
}
