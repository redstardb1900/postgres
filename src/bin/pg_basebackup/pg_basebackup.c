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

/*
 * pg_xlog 在版本 10 中已被重命名为 pg_wal。这个版本号   
 * 应与 PQserverVersion() 进行比较。  
 */
#define MINIMUM_VERSION_FOR_PG_WAL	100000

/*
 * 从版本 10 开始支持临时复制槽。               
 */
#define MINIMUM_VERSION_FOR_TEMP_SLOTS 100000

/*
 * 从版本 13 开始支持备份清单。       
 */
#define MINIMUM_VERSION_FOR_MANIFESTS	130000

/*
 * v15 之前，服务器发来的 tar 文件将无法正确        
 * 终止。  
 */
#define MINIMUM_VERSION_FOR_TERMINATED_TARFILE 150000

/*
 * 包含 WAL 的不同方式   
 */
typedef enum
{
	NO_WAL,
	FETCH_WAL,
	STREAM_WAL
} IncludeWal;

/*
 * 执行压缩的不同位置            
 */
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
static char format = '\0';		/* p(纯)/t(tar) */
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
/*
 * 如果 bgchild 提前退出并发出 SIGCHLD 信号，我们可以中止  
 * 处理，不必等到备份完成之后才报错。在 Windows 上，     
 * 我们使用后台线程，它无需信号处理程序即可           
 * 进行通信。                       
 */
static void
sigchld_handler(SIGNAL_ARGS)
{
	bgchild_exited = true;
}

/*
 * 在 Windows 上，后台线程会随进程一起消亡。            
 * 但在 Unix 上，如果启动了子进程，就要杀掉它，    
 * 以免它继续运行、流式传输数据。
 */
static void
kill_bgchild_atexit(void)
{
	if (bgchild > 0 && !bgchild_exited)
		kill(bgchild, SIGTERM);
}
#endif

/*
 * 把参数拆分为 old_dir 和 new_dir 并追加到表空间映射列表
 * 。  
 */
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

	/*
	 * 所有表空间都以绝对目录创建，在此指定非绝对路径 
	 * 将永远无法匹配，可能让用户困惑。因不知远端  
	 * 是 Windows 还是别的系统，且可能与本地不同，因此   
	 * 我们允许任何在任一套规则之下都可能为             
	 * 绝对路径的路径。                    
	 *
	 * （这里几乎没有混淆风险，因为完全在 Linux 上     
	 * 工作的人不太可能拥有以反斜杠或盘符样式开头    
	 * 的相对路径。但如果确实这样做了，还错误地
	 * 认为相对路径在这里可接受，那么我们将默默地    
	 * 无法就其错误发出警告，而 -T 选项将不会被应用，  
	 * 这与他们为不存在的表空间指定 -T 的                     
	 * 情况一样。）                                
	 */
	if (!is_nonwindows_absolute_path(cell->old_dir) &&
		!is_windows_absolute_path(cell->old_dir))
		pg_fatal("old directory is not an absolute path in tablespace mapping: %s",
				 cell->old_dir);

	if (!is_absolute_path(cell->new_dir))
		pg_fatal("new directory is not an absolute path in tablespace mapping: %s",
				 cell->new_dir);

	/*
	 * 对这些值的比较应使用同样规范化路径值。  
	 * 这一点在 Windows 上尤其敏感，因为路径值未必         
	 * 使用 Unix 风格的斜杠。                         
	 */
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


/*
 * 在后台进程中，每次收到数据时都会被调用。 
 * 在 Unix 上，我们检查管道有无数据（意味着
 * 我们已有了停止位置），如果有，则检查是否到了
 * 停止时间。    
 * 在 Windows 上，我们在单进程内，因此只需检查是否  
 * 该停止。 
 */
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

		/*
		 * 还没有结束指针 - 检查管道看它是否已被发送    
		 * 过来了。  
		 */
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
			/*
			 * 管道上没有数据意味着还不知道结束位置  
			 *  - 所以就回答现在还不是停止的时候。  
			 */
			return false;
		}
#else

		/*
		 * 在 win32 上，has_xlogendptr 由主线程设置，所以如果  
		 * 在这里尚未设置，就回去等它出现。     
		 */
		return false;
#endif
	}

	/*
	 * 此时我们已有结束指针，把它与当前位置相比，   
	 * 以判断是否到了停止的时候。     
	 */
	if (segendpos >= xlogendptr)
		return true;

	/*
	 * 已有结束指针但尚未到达 - 因此告诉调用者继续     
	 * 流式传输。
	 */
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
		/*
		 * 任何错误都已在函数进程中被报告，但需告诉父    
		 * 进程我们并未以体面的方式关闭                      
		 * 。 
		 */
#ifdef WIN32
		/*
		 * 为了向主线程通知非正常退出，我们设置在 Unix 上 
		 * 用于发出 SIGCHLD 信号的同一个标志。
		 */
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

/*
 * 在备份期间启动用于接收 xlog 的后台进程。        
 * 后台流会使用自己的数据库连接，因此我们可以与  
 * 备份并行地流式传输日志文件。      
 */
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

	/*
	 * 如果请求了则创建复制槽   
	 */
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
		/*
		 * 根据目标服务器，创建 pg_wal/archive_status 或        
		 * pg_xlog/archive_status（也就是 pg_wal 或 pg_xlog），      
		 * 以便我们可以写入 basedir/pg_wal 或 basedir/pg_xlog，    
		 * 因目录项可能后到  
		 */
		snprintf(statusdir, sizeof(statusdir), "%s/%s/archive_status",
				 basedir,
				 PQserverVersion(conn) < MINIMUM_VERSION_FOR_PG_WAL ?
				 "pg_xlog" : "pg_wal");

		if (pg_mkdir_p(statusdir, pg_dir_create_mode) != 0 && errno != EEXIST)
			pg_fatal("could not create directory \"%s\": %m", statusdir);
	}

	/*
	 * 启动子进程并告诉它开始流式传输。在 Unix 上，这是 
	 * fork()。Windows 上则创建线程。   
	 */
#ifndef WIN32
	bgchild = fork();
	if (bgchild == 0)
	{
		/* 子进程        */
		exit(LogStreamerMain(param));
	}
	else if (bgchild < 0)
		pg_fatal("could not create background process: %m");

	/*
	 * 否则我们处于父进程中，一切正常。  
	 */
	atexit(kill_bgchild_atexit);
#else							/* WIN32 */
	bgchild = _beginthreadex(NULL, 0, (void *) LogStreamerMain, param, 0, NULL);
	if (bgchild == 0)
		pg_fatal("could not create background thread: %m");
#endif
}

/*
 * 验证给定的目录存在且为空。如果目录不存在，    
 * 则会创建它。如果目录存在但不为空，则会给出   
 * 错误并结束进程。       
 */
static void
verify_dir_is_empty_or_create(char *dirname, bool *created, bool *found)
{
	switch (pg_check_dir(dirname))
	{
		case 0:

			/*
			 * 不存在，因此创建 
			 */
			if (pg_mkdir_p(dirname, pg_dir_create_mode) == -1)
				pg_fatal("could not create directory \"%s\": %m", dirname);
			if (created)
				*created = true;
			return;
		case 1:

			/*
			 * 存在且空 
			 */
			if (found)
				*found = true;
			return;
		case 2:
		case 3:
		case 4:

			/*
			 * 存在但非空  
			 */
			pg_fatal("directory \"%s\" exists but is not empty", dirname);
		case -1:

			/*
			 * 访问出错  
			 */
			pg_fatal("could not access directory \"%s\": %m", dirname);
	}
}

/*
 * 更新我们当前文件名记录的回调函数。   
 *
 * 其他代码不得修改 progress_filename！ 
 */
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

/*
 * 根据全局变量输出进度报告。如果启用了详细输出，还
 * 会打印当前的文件名。               
 *
 * 进度报告最多每秒写入一次，除非 force 参数被设置为 
 * true。                  
 *
 * 如果 finished 被设为 true，这就是最后一次进度报告。光
 * 标将移到下一行。  
 */
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

	/*
	 * 避免溢出超过 100% 或总大小。临近备份结束，总计   
	 * 大小数字可能变化（若含 WAL，估计值永远不准），  
	 * 但这总比 done 列大于                                            
	 * 总计的情况要好。                 
	 */
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

			/*
			 * 未给出文件名，故清除状态行（供末次调用 
			 * ）  
			 */
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

	/*
	 * 如果是输出到终端且尚未完成，则仍留在同一行    
	 * 。 
	 */
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
		/*
		 * 在此拒绝明显错误的值。  
		 */
		pg_fatal("transfer rate must be greater than zero");
	}

	/*
	 * 跳过可能存在的空白后解析后缀。没有后缀表示  
	 * 千字节。           
	 */
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

	/*
	 * 服务器端也会检查范围，但若传入值毫无意义，  
	 * 就应避免连接服务器。               
	 */
	if (result < MAX_RATE_LOWER || result > MAX_RATE_UPPER)
		pg_fatal("transfer rate \"%s\" is out of range", src);

	return (int32) result;
}

/*
 * 对 -Z/--compress 所指定值的基本解析。     
 *
 * 我们不关心确切了解用户想要何种行为，但确实需要
 * 知道用户请求的是客户端压缩、服务端压缩还是未指定
 * 并且需要把压缩算法的名称与细节字符串分开。     
 * 例如，如果用户写入 --compress client-lz4:6，我们希望    
 *
 * 将其拆分为 (a) 客户端压缩、(b) 算法 "lz4"、(c) 细节 
 * "6"。不过要注意，client/server 前缀是可选的，细节   
 * 也是如此。算法名称是必需的，除非整个字符串是     
 * 一个整数，此时我们假定算法为 "gzip"，并把该         
 * 整数用作                                                         
 * 细节。                                   
 *
 * 在此阶段我们不关心验证问题，因此如果用户写入      
 * --compress client-turkey:sandwich，则所请求的算法是          
 * "turkey"，细节字符串是 "sandwich"。是否合法将于以后     
 * 阶段厘清。  
 */
static void
backup_parse_compress_options(char *option, char **algorithm, char **detail,
							  CompressionLocation *locationres)
{
	/*
	 * 剥掉任何 "client-" 或 "server-" 前缀，并计算其位置。   
	 */
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

/*
 * 读取 COPY 数据，对每个数据块调用所提供的回调函  
 * 数。
 */
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

/*
 * 根据用户所选选项，决定如何处理服务器发来的归档  
 * 。结果可能被直接写入文件，也可能先压缩，也可能    
 * 解压 tar 文件并逐个写入其成员。这个函数并不直接   
 * 做这些事，但它会算出我们需要创建何种 bbstreamer，  
 * 以便等到之后，也就是真正收到数据的时候，           
 * 能让正确的事情得以发生                                    
 * 。      
 */
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

	/*
	 * 通常，备份清单会作为单独文件输出，但当我们往
	 * stdout 写 tar 文件时没有该选项，只能放进我们    
	 * 仅有的那个 tar 文件中。        
	 */
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

	/*
	 * 将清单注入压缩过的 tar 文件是可行的，只要先解压  
	 * 它、解析 tarfile、生成新 tar 文件再重新压缩，但只 
	 * 为了注入清单而这样反复压缩和解压，效率似乎太    
	 * 低了，多半不是用户想要的。因此，我们改为拒绝该 
	 * 请求，并让用户指定更合理的                               
	 * 做法。                            
	 */
	if (inject_manifest && is_compressed_tar)
	{
		pg_log_error("cannot inject manifest into a compressed tar file");
		pg_log_error_hint("Use client-side compression, send the output to a directory rather than standard output, or use %s.",
						  "--no-manifest");
		exit(1);
	}

	/*
	 * 如果 (1) 应当解压该归档，或 (2) 需要向其中注入      
	 * backup_manifest 或恢复配置，就必须解析它。不过我们 
	 * 只知道如何解析 tar 归档。                 
	 */
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

		/*
		 * 纯格式下必须解压归档。主表空间的数据将被写入 
		 * 基础目录，而其他表空间的数据则将被写入它们在
		 * 服务器上的所在目录，在应用用户指定的表空间 
		 * 映射之后。                                                    
		 *          
		 *
		 * 对于原地表空间，spclocation 将会是一个相对   
		 * 路径。只需在其前面加上 basedir 即变为绝对路径。 
		 *         
		 */
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

		/*
		 * tar 格式下直接写出归档而不解压。通常把它写  
		 * 到调用者提供的归档名，但当基础目录为 "-" 时，
		 * 这意味着我们需要写入                                   
		 * 标准输出。 
		 */
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

		/*
		 * 如果因任何原因要解析归档，就得重新归档，   
		 * 因为如果输出格式是 tar，解析归档的唯一目的     
		 * 就是能够向其中注入内容。                           
		 *         
		 */
		if (must_parse_archive)
			streamer = bbstreamer_tar_archiver_new(streamer);
		progress_update_filename(archive_filename);
	}

	/*
	 * 如果需把备份清单注入结果中，就应当在这里做，  
	 * 这样文件内容可以被直接注入，而无需关心 tar 格式的
	 * 细节。                                            
	 */
	if (inject_manifest)
		manifest_inject_streamer = streamer;

	/*
	 * 如果这是主表空间且需写入恢复信息，则安排进行 
	 * 相应的处理。              
	 */
	if (spclocation == NULL && writerecoveryconf)
	{
		Assert(must_parse_archive);
		streamer = bbstreamer_recovery_injector_new(streamer,
													is_recovery_guc_supported,
													recoveryconfcontents);
	}

	/*
	 * 如果我们所做的任何事都涉及理解归档内容，就需     
	 * 要解析它。若不需要，可跳过解析，但旧版服务器  
	 * 发送的 tar 文件终止不正确，若面对这类服务器，    
	 * 就需要在这里补上终止符。                                 
	 */
	if (must_parse_archive)
		streamer = bbstreamer_tar_parser_new(streamer);
	else if (expect_unterminated_tarfile)
		streamer = bbstreamer_tar_terminator_new(streamer);

	/*
	 * 若用户请求了服务器端压缩的归档，又要在客户 
	 * 进行解压，那么我们就需要解压该归档。     
	 */
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

/*
 * 以单个 COPY 流接收服务器要发送的全部归档 - 以及   
 * 备份清单（如果存在）。             
 */
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

	/*
	 * 如果为把清单注入输出 tar 文件而缓冲了它，就   
	 * 在此执行该操作。    
	 */
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

/*
 * 接收服务器作为 COPY 流的一部分而发送的一块数据，
 * 该流包含所有归档和清单。               
 */
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

				/*
				 * 每个表空间结束时都强制输出进度报告。新  
				 * 表空间在前一个结束时开始，但第一个        
				 * 表空间除外。             
				 */
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

				/*
				 * 对归档名的基本检查：不能为空，不能以  
				 * 点号开头，也不能包含                        
				 * 路径分隔符。       
				 */
				if (archive_name[0] == '\0' || archive_name[0] == '.' ||
					strchr(archive_name, '/') != NULL ||
					strchr(archive_name, '\\') != NULL)
					pg_fatal("invalid archive name: \"%s\"",
							 archive_name);

				/*
				 * 空 spclocation 会被当作 NULL。这种情况预期 
				 * 出现在数据目录本身，而不会出现在对应  
				 * 的表空间归档上。                    
				 */
				if (spclocation[0] == '\0')
					spclocation = NULL;

				/* 结束处理先前归档             */
				if (state->streamer != NULL)
				{
					bbstreamer_finalize(state->streamer);
					bbstreamer_free(state->streamer);
					state->streamer = NULL;
				}

				/*
				 * 创建适当备份流，除非指定了备份目标。
				 * 此时，把备份放到该放的地方是服务器的   
				 * 职责。                                 
				 */
				if (backup_target == NULL)
				{
					/*
					 * 我们知道恢复 GUCs 是受支持的，因为      
					 * 该协议只能用于 v15+。     
					 */
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
						/*
						 * 如果 fwrite() 未设置 errno，就假定  
						 * 问题是磁盘空间已耗尽。       
						 */
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
				/*
				 * 进度报告。 
				 *
				 * 其余部分应当是已完成字节数的              
				 * 8 字节计数值。     
				 */
				totaldone = GetCopyDataUInt64(r, copybuf, &cursor);
				GetCopyDataEnd(r, copybuf, cursor);

				/*
				 * 服务器发送进度报告消息不应太频繁，故
				 * 每收到一条我们就强制更新一次。         
				 */
				progress_report(state->tablespacenum, true, false);
				break;
			}

		case 'm':
			{
				/*
				 * 接下来是清单数据。此消息不应带有    
				 * 更多的有效负载数据。            
				 */
				GetCopyDataEnd(r, copybuf, cursor);

				/*
				 * 如果指定了备份目标，清单放在哪就归服务  
				 * 器管；否则，我们就得自行                       
				 * 处理它。 
				 */
				if (backup_target == NULL)
				{
					/*
					 * 如果应把清单注入归档，就准备缓存进    
					 * 内存；否则，就准备把它写到一个   
					 * 临时文件中。                      
					 */
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

/*
 * 从 CopyData 消息获取单个字节。  
 *
 * 若无剩余则退出。
 */
static char
GetCopyDataByte(size_t r, char *copybuf, size_t *cursor)
{
	if (*cursor >= r)
		ReportCopyDataParseError(r, copybuf);

	return copybuf[(*cursor)++];
}

/*
 * 从 CopyData 消息获取 NUL 结尾的字符串。 
 *
 * 如果找不到终止 NUL 则退出。          
 */
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

/*
 * 从 CopyData 消息中获取无符号 64 位整数。  
 *
 * 如果剩余的字节不足 8 个则退出。        
 */
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

/*
 * 如果未能解析整个消息则退出。    
 */
static void
GetCopyDataEnd(size_t r, char *copybuf, size_t cursor)
{
	if (r != cursor)
		ReportCopyDataParseError(r, copybuf);
}

/*
 * 报告解析来自服务器的 CopyData 消息失败。然后退出。
 *
 * 作为调试辅助，我们尝试给出关于是哪类消息引发     
 * 该失败的提示。也许这不够详细，但为这种不该发生  
 * 的情况再投入更多代码是否值得，并不              
 * 清楚。         
 */
static void
ReportCopyDataParseError(size_t r, char *copybuf)
{
	if (r == 0)
		pg_fatal("empty COPY message");
	else
		pg_fatal("malformed COPY message of type %d, length %zu",
				 copybuf[0], r);
}

/*
 * 从服务器接收原始 tar 数据，并将其流式传输到合适  
 * 的位置。若正把单个 tar 文件写入标准输出，还要   
 * 接收备份清单并将其注入该 tar 文件。          
 */
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

	/*
	 * 是否需要在此时把备份清单注入到输出之中，由      
	 * CreateBackupStreamer 决定；如果需要，                       
	 * manifest_inject_streamer 将不为 NULL；否则它将为            
	 * NULL。 
	 */
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

	/*
	 * 暂不同步生成的 tar 文件；所有文件最后统一完成   
	 * 。     
	 */
}

/*
 * 从服务器接收一块 tar 格式的数据。      
 */
static void
ReceiveTarCopyChunk(size_t r, char *copybuf, void *callback_data)
{
	WriteTarState *state = callback_data;

	bbstreamer_content(state->streamer, NULL, copybuf, r, BBSTREAMER_UNKNOWN);

	totaldone += r;
	progress_report(state->tablespacenum, false, false);
}


/*
 * 获取表空间路径：如果传入 -T 则是重定位后的路径，     
 * 否则是原路径。
 */
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

/*
 * 接收备份清单文件并将其写出到文件中。      
 */
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

/*
 * 接收备份清单文件的一块数据并将其写出到文件中。    
 */
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

/*
 * 接收备份清单文件并将其写出到文件中。      
 */
static void
ReceiveBackupManifestInMemory(PGconn *conn, PQExpBuffer buf)
{
	ReceiveCopyData(conn, ReceiveBackupManifestInMemoryChunk, buf);
}

/*
 * 接收备份清单文件的一块数据并将其写出到文件中。    
 */
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

	/*
	 * 检查服务器版本。BASE_BACKUP 命令在 9.1 中引入。        
	 * 无法用于 9.1 之前的服务器。 
	 */
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

	/*
	 * 如果请求了 WAL 流式传输，还要检查服务器版本   
	 * 是否够新。 
	 */
	if (includewal == STREAM_WAL && !CheckServerVersionForStreaming(conn))
	{
		/*
		 * 错误消息已在 CheckServerVersionForStreaming() 中写        
		 * 出，并加上 -X none 提示。  
		 */
		pg_log_error_hint("Use -X none or -X fetch to disable log streaming.");
		exit(1);
	}

	/*
	 * 如果请求了则构建配置文件的内容    
	 */
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

	/*
	 * 9.3 及以后版本会发送起点的 TLI。对更旧的服务器，  
	 * 假定与 IDENTIFY_SYSTEM 所报最新时间线一致  
	 * 。             
	 */
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

		/*
		 * 验证表空间目录为空。第一个无需检查，因为  
		 * 它可被重定位，而且在开始做事之前反正都会       
		 * 得到检查。       
		 *
		 * 注意，tar 格式备份以及服务器存到目标处的备份 
		 * 会跳过此步，因为那样我们不会向这些目录        
		 * 写入任何内容，因此不应                               
		 * 创建它们。 
		 */
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

	/*
	 * 写 stdout 时要求只有单个表空间          
	 */
	writing_to_stdout = format == 't' && basedir != NULL &&
		strcmp(basedir, "-") == 0;
	if (writing_to_stdout && PQntuples(res) > 1)
		pg_fatal("can only write single tablespace to stdout, database has %d",
				 PQntuples(res));

	/*
	 * 若正在流式传输 WAL，需在开始接收实际数据块之前
	 * 先启动流式传输会话。   
	 */
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

			/*
			 * 若写成 tar 文件，主数据目录的文件命名   
			 * 为 base.tar，其他表空间为                              
			 *  <tablespaceoid>.tar。CreateBackupStreamer() 会          
			 * 在 pg_basebackup 压缩时给归档名添加     
			 * 扩展名，具体取决于                              
			 * 压缩类型。  
			 */
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

		/*
		 * 现在接收备份清单（如合适）。  
		 *
		 * 若把 tar 文件写入 stdout，ReceiveTarFile 已           
		 * 经处理过备份清单并把它含入输出 tar 文件。这种  
		 * 配置不允许写入多个文件                                
		 * 。   
		 *
		 * 若服务器较旧，就不会发送备份清单，所以  
		 * 不要尝试接收清单。           
		 */
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

		/*
		 * 获取 bgchild 的指针大小版本，避免在 WIN64 上    
		 * 因转换成不同大小而告警。 
		 */
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

		/*
		 * 在 Windows 上，由于我们在同一进程中，可以直接    
		 * 把值存入变量，然后设置表明值已存在的         
		 * 标志。  
		 */
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

	/*
	 * 复制数据已结束。最终结果已在循环内检查过了。
	 */
	PQclear(res);
	PQfinish(conn);
	conn = NULL;

	/*
	 * 备份完成后将数据持久化到磁盘。tar 格式需同步     
	 * 父目录及其全部内容，因为每个 tar 文件完成后没有   
	 * 被同步。纯格式下，基础目录所有数据都会同步， 
	 * 并考虑到所有表空间。错误不被视为致命            
	 * 错误。                       
	 *
	 * 但若存在备份目标，我们不向本地写任何内容， 
	 * 因此在这种情况下跳过这一步。 
	 */
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

	/*
	 * 数据同步到磁盘后，若写过 backup_manifest.tmp，  
	 * 则对它执行持久化的重命名，将其改名为 backup_       
	 * manifest。这样，在我们到达这一步之前发生的故障或系
	 * 统崩溃都会使我们没有 backup_manifest 文件，从而降      
	 * 低遗留目录被误认为有效备份的几率。 
	 */
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

				/*
				 * 指定复制槽名称时，使用永久槽            
				 * 。  
				 */
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

	/*
	 * 有非选项参数吗？ 
	 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * 把备份目标设为 'client' 等同于省略该选项。这样      
	 * 我们在别处就可以假定：当且仅当 backup_target == NULL
	 * 时备份正被存储在本地。                         
	 */
	if (backup_target != NULL && strcmp(backup_target, "client") == 0)
	{
		pg_free(backup_target);
		backup_target = NULL;
	}

	/*
	 * --format 不能与 --target 同用；无 --target 时默认为 tar   
	 * 。 
	 */
	if (backup_target != NULL && format != '\0')
	{
		pg_log_error("cannot specify both format and backup target");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	if (format == '\0')
		format = 'p';

	/*
	 * 应指定目录或备份目标之一，但不能两者都指定    
	 */
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

	/*
	 * 如果用户未指定在哪里执行备份压缩，则默认客户
	 * 端，除非用户指定了 --target，此时服务器             
	 * 是唯一选择。                 
	 */
	if (compressloc == COMPRESS_LOCATION_UNSPECIFIED)
	{
		if (backup_target == NULL)
			compressloc = COMPRESS_LOCATION_CLIENT;
		else
			compressloc = COMPRESS_LOCATION_SERVER;
	}

	/*
	 * 如果我们所做的任何压缩在客户端进行，就必须尝     
	 * 试解析压缩算法和细节；若全在服务器端，那么        
	 * 我们就把所请求的内容原样传递，让服务器自行决定 
	 * 要做什么。                                
	 */
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

	/*
	 * 如果备份未发送到客户端，就不能进行客户端         
	 * 压缩。     
	 */
	if (backup_target != NULL && compressloc == COMPRESS_LOCATION_CLIENT)
	{
		pg_log_error("client-side compression is not possible when a backup target is specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * 除非使用 tar 格式，否则客户端压缩没有意义。         
	 */
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

	/*
	 * 捕获 SIGCHLD，以便能够处理 WAL 流进程的退出。          
	 * Windows 上没有 SIGCHLD，在那里依靠后台线程         
	 * 在意外但正常的退出时设置信号变量。如果 WAL          
	 * 流线程在 Windows 上崩溃，由于它是一个线程，          
	 * 会拖垮整个进程，那种情况下没有东西可以捕获。  
	 * UNIX 上的崩溃由信号处理程序捕获。    
	 */
	pqsignal(SIGCHLD, sigchld_handler);
#endif

	/*
	 * 设置 umask，让新建的目录/文件与源数据目录   
	 * 中的目录/文件具有相同的权限。                   
	 *
	 * pg_mode_mask 默认为仅属主；随后它在                  
	 * GetConnection() 中更新，从服务器端经                 
	 * RetrieveDataDirCreatePerm()后，调用 SetDataDirectoryCreatePerm()。
	 */
	umask(pg_mode_mask);

	/* 备份清单 v13 起支持                              */
	if (PQserverVersion(conn) < MINIMUM_VERSION_FOR_MANIFESTS)
		manifest = false;

	/*
	 * 如果指定了输出目录，则验证它存在或创建它。注    
	 * 意对于 tar 备份，输出目录为 "-" 意味着我们正在写入 
	 * 标准输出，此时不做任何事情。    
	 */
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

		/*
		 * 构造符号链接必须指向的位置的名称。pg_xlog 在    
		 * 10 后的集簇中更名为 pg_wal。 
		 */
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
