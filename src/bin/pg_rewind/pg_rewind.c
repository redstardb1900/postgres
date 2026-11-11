/*-------------------------------------------------------------------------
 *
 * pg_rewind.c
 *	  Synchronizes a PostgreSQL data directory to a new timeline
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "access/timeline.h"
#include "access/xlog_internal.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "common/file_perm.h"
#include "common/restricted_token.h"
#include "common/string.h"
#include "fe_utils/recovery_gen.h"
#include "fe_utils/string_utils.h"
#include "file_ops.h"
#include "filemap.h"
#include "getopt_long.h"
#include "pg_rewind.h"
#include "rewind_source.h"
#include "storage/bufpage.h"

static void usage(const char *progname);

static void perform_rewind(filemap_t *filemap, rewind_source *source,
						   XLogRecPtr chkptrec,
						   TimeLineID chkpttli,
						   XLogRecPtr chkptredo);

static void createBackupLabel(XLogRecPtr startpoint, TimeLineID starttli,
							  XLogRecPtr checkpointloc);

static void digestControlFile(ControlFileData *ControlFile,
							  const char *content, size_t size);
static void getRestoreCommand(const char *argv0);
static void sanityChecks(void);
static TimeLineHistoryEntry *getTimelineHistory(TimeLineID tli, bool is_source,
												int *nentries);
static void findCommonAncestorTimeline(TimeLineHistoryEntry *a_history,
									   int a_nentries,
									   TimeLineHistoryEntry *b_history,
									   int b_nentries,
									   XLogRecPtr *recptr, int *tliIndex);
static void ensureCleanShutdown(const char *argv0);
static void disconnect_atexit(void);

static ControlFileData ControlFile_target;
static ControlFileData ControlFile_source;
static ControlFileData ControlFile_source_after;

const char *progname;
int			WalSegSz;

/* 配置选项                  */
char	   *datadir_target = NULL;
char	   *datadir_source = NULL;
char	   *connstr_source = NULL;
char	   *restore_command = NULL;
char	   *config_file = NULL;

static bool debug = false;
bool		showprogress = false;
bool		dry_run = false;
bool		do_sync = true;
bool		restore_wal = false;

/* 目标历史           */
TimeLineHistoryEntry *targetHistory;
int			targetNentries;

/* 进度计数器             */
uint64		fetch_size;
uint64		fetch_done;

static PGconn *conn;
static rewind_source *source;

static void
usage(const char *progname)
{
	printf(_("%s resynchronizes a PostgreSQL cluster with another copy of the cluster.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -c, --restore-target-wal       use restore_command in target configuration to\n"
			 "                                 retrieve WAL files from archives\n"));
	printf(_("  -D, --target-pgdata=DIRECTORY  existing data directory to modify\n"));
	printf(_("      --source-pgdata=DIRECTORY  source data directory to synchronize with\n"));
	printf(_("      --source-server=CONNSTR    source server to synchronize with\n"));
	printf(_("  -n, --dry-run                  stop before modifying anything\n"));
	printf(_("  -N, --no-sync                  do not wait for changes to be written\n"
			 "                                 safely to disk\n"));
	printf(_("  -P, --progress                 write progress messages\n"));
	printf(_("  -R, --write-recovery-conf      write configuration for replication\n"
			 "                                 (requires --source-server)\n"));
	printf(_("      --config-file=FILENAME     use specified main server configuration\n"
			 "                                 file when running target cluster\n"));
	printf(_("      --debug                    write a lot of debug messages\n"));
	printf(_("      --no-ensure-shutdown       do not automatically fix unclean shutdown\n"));
	printf(_("  -V, --version                  output version information, then exit\n"));
	printf(_("  -?, --help                     show this help, then exit\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"target-pgdata", required_argument, NULL, 'D'},
		{"write-recovery-conf", no_argument, NULL, 'R'},
		{"source-pgdata", required_argument, NULL, 1},
		{"source-server", required_argument, NULL, 2},
		{"no-ensure-shutdown", no_argument, NULL, 4},
		{"config-file", required_argument, NULL, 5},
		{"version", no_argument, NULL, 'V'},
		{"restore-target-wal", no_argument, NULL, 'c'},
		{"dry-run", no_argument, NULL, 'n'},
		{"no-sync", no_argument, NULL, 'N'},
		{"progress", no_argument, NULL, 'P'},
		{"debug", no_argument, NULL, 3},
		{NULL, 0, NULL, 0}
	};
	int			option_index;
	int			c;
	XLogRecPtr	divergerec;
	int			lastcommontliIndex;
	XLogRecPtr	chkptrec;
	TimeLineID	chkpttli;
	XLogRecPtr	chkptredo;
	TimeLineID	source_tli;
	TimeLineID	target_tli;
	XLogRecPtr	target_wal_endrec;
	size_t		size;
	char	   *buffer;
	bool		no_ensure_shutdown = false;
	bool		rewind_needed;
	bool		writerecoveryconf = false;
	filemap_t  *filemap;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_rewind"));
	progname = get_progname(argv[0]);

	/* 处理命令行参数                        */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_rewind (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "cD:nNPR", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'c':
				restore_wal = true;
				break;

			case 'P':
				showprogress = true;
				break;

			case 'n':
				dry_run = true;
				break;

			case 'N':
				do_sync = false;
				break;

			case 'R':
				writerecoveryconf = true;
				break;

			case 3:
				debug = true;
				pg_logging_increase_verbosity();
				break;

			case 'D':			/* -D or --target-pgdata */
				datadir_target = pg_strdup(optarg);
				break;

			case 1:				/* --source-pgdata */
				datadir_source = pg_strdup(optarg);
				break;

			case 2:				/* --source-server */
				connstr_source = pg_strdup(optarg);
				break;

			case 4:
				no_ensure_shutdown = true;
				break;

			case 5:
				config_file = pg_strdup(optarg);
				break;

			default:
				/* getopt_long 已发出投诉             */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (datadir_source == NULL && connstr_source == NULL)
	{
		pg_log_error("no source specified (--source-pgdata or --source-server)");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (datadir_source != NULL && connstr_source != NULL)
	{
		pg_log_error("only one of --source-pgdata or --source-server can be specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (datadir_target == NULL)
	{
		pg_log_error("no target data directory specified (--target-pgdata)");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (writerecoveryconf && connstr_source == NULL)
	{
		pg_log_error("no source server information (--source-server) specified for --write-recovery-conf");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * 禁止 pg_rewind 以 root 身份运行                           
	 * ，避免覆盖数据目录文件的所有权。只需检查         
	 * root —— 任何其他用户都无足够权限修改                
	 * 目录文件。    
	 */
#ifndef WIN32
	if (geteuid() == 0)
	{
		pg_log_error("cannot be executed by \"root\"");
		pg_log_error_hint("You must run %s as the PostgreSQL superuser.",
						  progname);
		exit(1);
	}
#endif

	get_restricted_token();

	/* 根据 PGDATA 权限设置掩码                     */
	if (!GetDataDirectoryCreatePerm(datadir_target))
		pg_fatal("could not read permissions of directory \"%s\": %m",
				 datadir_target);

	umask(pg_mode_mask);

	getRestoreCommand(argv[0]);

	atexit(disconnect_atexit);

	/*
	 * 好，选项就绪，准备开始。先连接远程服务器。        
	 * 远程服。  
	 */
	if (connstr_source)
	{
		conn = PQconnectdb(connstr_source);

		if (PQstatus(conn) == CONNECTION_BAD)
			pg_fatal("%s", PQerrorMessage(conn));

		if (showprogress)
			pg_log_info("connected to server");

		source = init_libpq_source(conn);
	}
	else
		source = init_local_source(datadir_source);

	/*
	 * 检查目标实例的状态。          
	 *
	 * 若目标实例未干净关闭，则以单用户模式启停目标  
	 * 集簇一次，强制恢复完成，确保集簇可被 pg_rewind 使  
	 * 用。注意，如果指定了 no_ensure_shutdown，pg_rewind    
	 * 会忽略这一步，用户需自行确保目标集簇处于           
	 * 干净                                                               
	 * 状态
	 */
	buffer = slurpFile(datadir_target, "global/pg_control", &size);
	digestControlFile(&ControlFile_target, buffer, size);
	pg_free(buffer);

	if (!no_ensure_shutdown &&
		ControlFile_target.state != DB_SHUTDOWNED &&
		ControlFile_target.state != DB_SHUTDOWNED_IN_RECOVERY)
	{
		ensureCleanShutdown(argv[0]);

		buffer = slurpFile(datadir_target, "global/pg_control", &size);
		digestControlFile(&ControlFile_target, buffer, size);
		pg_free(buffer);
	}

	buffer = source->fetch_file(source, "global/pg_control", &size);
	digestControlFile(&ControlFile_source, buffer, size);
	pg_free(buffer);

	sanityChecks();

	/*
	 * 通常，TLI 可以在最新的检查点记录中找到。但如果  
	 * 源刚被提升（或其备机跟随的主刚被提升），      
	 * 且提升所请求的检查点尚未完成，则最新时间线在 
	 * minRecoveryPoint 中。所以我们检查哪个更晚：是            
	 * minRecoveryPoint 的 TLI 还是最新检查点的 TLI，       
	 * 取较晚的那个。                     
	 */
	source_tli = Max(ControlFile_source.minRecoveryPointTLI,
					 ControlFile_source.checkPointCopy.ThisTimeLineID);

	/* 目标同理。                     */
	target_tli = Max(ControlFile_target.minRecoveryPointTLI,
					 ControlFile_target.checkPointCopy.ThisTimeLineID);

	/*
	 * 查找两集簇间的公共祖先时间线。          
	 *
	 * 若两集簇已在同一时间线，则无需做任何事。         
	 * 。
	 */
	if (target_tli == source_tli)
	{
		pg_log_info("source and target cluster are on the same timeline");
		rewind_needed = false;
		target_wal_endrec = 0;
	}
	else
	{
		XLogRecPtr	chkptendrec;
		TimeLineHistoryEntry *sourceHistory;
		int			sourceNentries;

		/*
		 * 获取源和目标的 timeline 历史，定位分岔点。       
		 * 分岔点。        
		 */
		sourceHistory = getTimelineHistory(source_tli, true, &sourceNentries);
		targetHistory = getTimelineHistory(target_tli, false, &targetNentries);

		findCommonAncestorTimeline(sourceHistory, sourceNentries,
								   targetHistory, targetNentries,
								   &divergerec, &lastcommontliIndex);

		pg_log_info("servers diverged at WAL location %X/%X on timeline %u",
					LSN_FORMAT_ARGS(divergerec),
					targetHistory[lastcommontliIndex].tli);

		/*
		 * 不再需要源 timeline 历史。目标历史仍由 parsexlog.c  
		 * 读取目标 WAL 时所用。                                      
		 */
		pfree(sourceHistory);


		/*
		 * 确定目标侧的 WAL 结束位置。 
		 *
		 * WAL 止于最后关机检查点，或备机的     
		 * minRecoveryPoint。非干净关闭回绕需                       
		 * 重放至第一个无效记录，同崩溃恢复。）        
		 * 重放至首个无效记录，如崩溃恢复。）                
		 */

		/* 读取目标检查点记录以查看其结束位置。                                             */
		chkptendrec = readOneRecord(datadir_target,
									ControlFile_target.checkPoint,
									targetNentries - 1,
									restore_command);

		if (ControlFile_target.minRecoveryPoint > chkptendrec)
		{
			target_wal_endrec = ControlFile_target.minRecoveryPoint;
		}
		else
		{
			target_wal_endrec = chkptendrec;
		}

		/*
		 * 检查目标实际上是否是源的直系祖先。          
		 * 若是这样，目标中就没有需要回绕的                   
		 * 分岔历史。                    
		 */
		if (target_wal_endrec > divergerec)
		{
			rewind_needed = true;
		}
		else
		{
			/* 最后一个公共检查点记录必须是目标 WAL 的一部分                                    */
			Assert(target_wal_endrec == divergerec);

			rewind_needed = false;
		}
	}

	if (!rewind_needed)
	{
		pg_log_info("no rewind required");
		if (writerecoveryconf && !dry_run)
			WriteRecoveryConfig(conn, datadir_target,
								GenerateRecoveryConfig(conn, NULL));
		exit(0);
	}

	/* 初始化跟踪受保护不被删除的 WAL 文件的哈希表                                          */
	keepwal_init();

	findLastCheckpoint(datadir_target, divergerec, lastcommontliIndex,
					   &chkptrec, &chkpttli, &chkptredo, restore_command);
	pg_log_info("rewinding from last common checkpoint at %X/%X on timeline %u",
				LSN_FORMAT_ARGS(chkptrec), chkpttli);

	/* 初始化跟踪每个文件状态的哈希表                                            */
	filehash_init();

	/*
	 * 收集两个数据目录中所有文件的信息。              
	 */
	if (showprogress)
		pg_log_info("reading source file list");
	source->traverse_files(source, &process_source_file);

	if (showprogress)
		pg_log_info("reading target file list");
	traverse_datadir(datadir_target, &process_target_file);

	/*
	 * 从分岔点之前最后一个检查点开始读取目标 WAL，     
	 * 以提取分岔之后目标集簇上被修改的所有              
	 * 页面。
	 */
	if (showprogress)
		pg_log_info("reading WAL in target");
	extractPageMap(datadir_target, chkptrec, lastcommontliIndex,
				   target_wal_endrec, restore_command);

	/*
	 * 已从两系统收集所有所需信息。决定每个文件       
	 * 做何操作。           
	 */
	filemap = decide_file_actions();
	if (showprogress)
		calculate_totals(filemap);

	/* 即使在详细模式下这也过于啰嗦                            */
	if (debug)
		print_filemap(filemap);

	/*
	 * 好，准备开始复制数据。            
	 */
	if (showprogress)
	{
		pg_log_info("need to copy %lu MB (total source directory size is %lu MB)",
					(unsigned long) (filemap->fetch_size / (1024 * 1024)),
					(unsigned long) (filemap->total_size / (1024 * 1024)));

		fetch_size = filemap->fetch_size;
		fetch_done = 0;
	}

	/*
	 * 已从双方系统收集所有信息，即将修改目标目录。  
	 * 标目录。                                             
	 *
	 * 这是不归点。一旦开始复制，便无回头路！             
	 * 不归路！    
	 */
	perform_rewind(filemap, source, chkptrec, chkpttli, chkptredo);

	if (showprogress)
		pg_log_info("syncing target data directory");
	sync_target_dir();

	/* 同时更新备机配置（如果请求的话）。                                    */
	if (writerecoveryconf && !dry_run)
		WriteRecoveryConfig(conn, datadir_target,
							GenerateRecoveryConfig(conn, NULL));

	/* 不再需要源连接                                  */
	source->destroy(source);
	if (conn)
	{
		PQfinish(conn);
		conn = NULL;
	}

	pg_log_info("Done!");

	return 0;
}

/*
 * 执行回绕。    
 *
 * 我们已经从目标和源收集了所有需要的           
 * 信息。             
 */
static void
perform_rewind(filemap_t *filemap, rewind_source *source,
			   XLogRecPtr chkptrec,
			   TimeLineID chkpttli,
			   XLogRecPtr chkptredo)
{
	XLogRecPtr	endrec;
	TimeLineID	endtli;
	ControlFileData ControlFile_new;
	size_t		size;
	char	   *buffer;

	/*
	 * 执行文件映射中的操作，按需从源获取数据。      
	 * 获数据。     
	 */
	for (int i = 0; i < filemap->nentries; i++)
	{
		file_entry_t *entry = filemap->entries[i];

		/*
		 * 若为关系文件，复制其修改的数据块。  
		 *
		 * 此操作独立于任何其他变更。  
		 */
		if (entry->target_pages_to_overwrite.bitmapsize > 0)
		{
			datapagemap_iterator_t *iter;
			BlockNumber blkno;
			off_t		offset;

			iter = datapagemap_iterate(&entry->target_pages_to_overwrite);
			while (datapagemap_next(iter, &blkno))
			{
				offset = blkno * BLCKSZ;
				source->queue_fetch_range(source, entry->path, offset, BLCKSZ);
			}
			pg_free(iter);
		}

		switch (entry->action)
		{
			case FILE_ACTION_NONE:
				/* 无需其他操作             */
				break;

			case FILE_ACTION_COPY:
				source->queue_fetch_file(source, entry->path, entry->source_size);
				break;

			case FILE_ACTION_TRUNCATE:
				truncate_target_file(entry->path, entry->source_size);
				break;

			case FILE_ACTION_COPY_TAIL:
				source->queue_fetch_range(source, entry->path,
										  entry->target_size,
										  entry->source_size - entry->target_size);
				break;

			case FILE_ACTION_REMOVE:
				remove_target(entry);
				break;

			case FILE_ACTION_CREATE:
				create_target(entry);
				break;

			case FILE_ACTION_UNDECIDED:
				pg_fatal("no action decided for file \"%s\"", entry->path);
				break;
		}
	}

	/* 完成上面排队的所有剩余范围获取。                                              */
	source->finish_fetch(source);

	close_target_file();

	progress_report(true);

	/*
	 * 最后从源获取控制文件，确保 minRecoveryPoint 最新。
	 * 最新。                      
	 */
	buffer = source->fetch_file(source, "global/pg_control", &size);
	digestControlFile(&ControlFile_source_after, buffer, size);
	pg_free(buffer);

	/*
	 * 健全检查：本地源的控制文件不应在启动后改变。    
	 * 化。                            
	 *
	 * XXX：假定未变，但实际呢？libpq 源能并发修改，    
	 * 为何不能处理本地数据目录的并发修改？               
	 * 录？                  
	 */
	if (datadir_source &&
		memcmp(&ControlFile_source, &ControlFile_source_after,
			   sizeof(ControlFileData)) != 0)
	{
		pg_fatal("source system was modified while pg_rewind was running");
	}

	if (showprogress)
		pg_log_info("creating backup label and updating control file");

	/*
	 * 创建备份标签，告知WAL重放起点。通常为源            
	 * 与目标的最后公共检查点。若源为备机，可能最后  
	 * 检查点在备机重启点之后。即源已应用检查点但未  
	 * 重启点。务必从重启点的 redo 点开始。                   
	 * 重启点。务必从重启点的 redo 点开始。                   
	 * 重启点。务必从重启点的 redo 点开始。                
	 * redo 点开始。                                   
	 *
	 * 用旧源控制文件。服务器可能已在复制期间完成      
	 * 重启点，但须从复制开始时的 redo 点出发。           
	 * 重启点，但须从复制开始时的 redo 点出发。              
	 */
	if (ControlFile_source.checkPointCopy.redo < chkptredo)
	{
		chkptredo = ControlFile_source.checkPointCopy.redo;
		chkpttli = ControlFile_source.checkPointCopy.ThisTimeLineID;
		chkptrec = ControlFile_source.checkPoint;
	}
	createBackupLabel(chkptredo, chkpttli, chkptrec);

	/*
	 * 更新目标控制文件，告知目标需重放多少 WAL       
	 * (minRecoveryPoint)。             
	 */
	if (connstr_source)
	{
		/*
		 * 源为活动服务器。如同在线备份，重要的是  
		 * 恢复全部 WAL，即我们复制文件                         
		 * 期间的 WAL。   
		 */
		if (ControlFile_source_after.state == DB_IN_ARCHIVE_RECOVERY)
		{
			/*
			 * 源是备机服务器。须重放到其          
			 * minRecoveryPt。 
			 */
			endrec = ControlFile_source_after.minRecoveryPoint;
			endtli = ControlFile_source_after.minRecoveryPointTLI;
		}
		else
		{
			/*
			 * 源是生产（非备机）服务器。须重放到最后     
			 * WAL 插入位置。          
			 */
			if (ControlFile_source_after.state != DB_IN_PRODUCTION)
				pg_fatal("source system was in unexpected state at end of rewind");

			endrec = source->get_current_wal_insert_lsn(source);
			endtli = Max(ControlFile_source_after.checkPointCopy.ThisTimeLineID,
						 ControlFile_source_after.minRecoveryPointTLI);
		}
	}
	else
	{
		/*
		 * 源是本地数据目录。已干净关闭，须重放到最后  
		 * 关机检查点。                                   
		 */
		endrec = ControlFile_source_after.checkPoint;
		endtli = ControlFile_source_after.checkPointCopy.ThisTimeLineID;
	}

	memcpy(&ControlFile_new, &ControlFile_source_after, sizeof(ControlFileData));
	ControlFile_new.minRecoveryPoint = endrec;
	ControlFile_new.minRecoveryPointTLI = endtli;
	ControlFile_new.state = DB_IN_ARCHIVE_RECOVERY;
	if (!dry_run)
		update_controlfile(datadir_target, &ControlFile_new, do_sync);
}

static void
sanityChecks(void)
{
	/* TODO 检查两个集簇中都没有 backup_label                              */

	/* 检查 system_identifier 是否匹配     */
	if (ControlFile_target.system_identifier != ControlFile_source.system_identifier)
		pg_fatal("source and target clusters are from different systems");

	/* 检查版本          */
	if (ControlFile_target.pg_control_version != PG_CONTROL_VERSION ||
		ControlFile_source.pg_control_version != PG_CONTROL_VERSION ||
		ControlFile_target.catalog_version_no != CATALOG_VERSION_NO ||
		ControlFile_source.catalog_version_no != CATALOG_VERSION_NO)
	{
		pg_fatal("clusters are not compatible with this version of pg_rewind");
	}

	/*
	 * 目标集簇需启用 checksum 或提示位WAL 记录，以          
	 * 防提示位导致数据损坏。                                  
	 */
	if (ControlFile_target.data_checksum_version != PG_DATA_CHECKSUM_VERSION &&
		!ControlFile_target.wal_log_hints)
	{
		pg_fatal("target server needs to use either data checksums or \"wal_log_hints = on\"");
	}

	/*
	 * 目标集簇最好别运行。这并不能防止有人          
	 * 并发启动集簇。此外这可能比必要的更严格；          
	 * 只要目标节点当前未在运行，未干净关闭也          
	 * 可以。                                          
	 */
	if (ControlFile_target.state != DB_SHUTDOWNED &&
		ControlFile_target.state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("target server must be shut down cleanly");

	/*
	 * 当源为数据目录时，还要求源服务器已关闭。这  
	 * 一限制并没有什么很强的理由，                      
	 * 但安全总比遗憾好。            
	 */
	if (datadir_source &&
		ControlFile_source.state != DB_SHUTDOWNED &&
		ControlFile_source.state != DB_SHUTDOWNED_IN_RECOVERY)
		pg_fatal("source data directory must be shut down cleanly");
}

/*
 * 根据 fetch_size 和 fetch_done 变量打印进度报告。
 *
 * 进度报告最多每秒写入一次，但最后一条进度报告
 * 总是会被打印。
 *
 * 如果 finished 设为 true，这是最后一条进度报告。
 * 光标移至下一行。
                                                                                                                                                                                 */
void
progress_report(bool finished)
{
	static pg_time_t last_progress_report = 0;
	int			percent;
	char		fetch_done_str[32];
	char		fetch_size_str[32];
	pg_time_t	now;

	if (!showprogress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !finished)
		return;					/* Max once per second */

	last_progress_report = now;
	percent = fetch_size ? (int) ((fetch_done) * 100 / fetch_size) : 0;

	/*
	 * 避免溢出超过 100% 或完整大小。这可能导致总量数   
	 * 值随着我们接近备份末尾而变化（若包含 WAL，估计  
	 * 总是不准确的），但这总比让                                
	 * 已完成列大于总量要好。        
	 */
	if (percent > 100)
		percent = 100;
	if (fetch_done > fetch_size)
		fetch_size = fetch_done;

	snprintf(fetch_done_str, sizeof(fetch_done_str), UINT64_FORMAT,
			 fetch_done / 1024);
	snprintf(fetch_size_str, sizeof(fetch_size_str), UINT64_FORMAT,
			 fetch_size / 1024);

	fprintf(stderr, _("%*s/%s kB (%d%%) copied"),
			(int) strlen(fetch_size_str), fetch_done_str, fetch_size_str,
			percent);

	/*
	 * 若输出到终端且未完成，保持在同行。                
	 * 。 
	 */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
}

/*
 * 从两个 WAL 位置取最小值，假设 InvalidXLogRecPtr
 * 表示无穷大（如 src/include/access/timeline.h 所述）。
 * 此例程仅用于比较与历史文件相关的 WAL 位置。
                                                                                                        */
static XLogRecPtr
MinXLogRecPtr(XLogRecPtr a, XLogRecPtr b)
{
	if (XLogRecPtrIsInvalid(a))
		return b;
	else if (XLogRecPtrIsInvalid(b))
		return a;
	else
		return Min(a, b);
}

/*
 * 获取源或目标系统的时间线历史。
                                            */
static TimeLineHistoryEntry *
getTimelineHistory(TimeLineID tli, bool is_source, int *nentries)
{
	TimeLineHistoryEntry *history;

	/*
	 * Timeline 1 无历史文件，无需检查，伪造一起止条目。 
	 * 条目。                                               
	 */
	if (tli == 1)
	{
		history = (TimeLineHistoryEntry *) pg_malloc(sizeof(TimeLineHistoryEntry));
		history->tli = tli;
		history->begin = history->end = InvalidXLogRecPtr;
		*nentries = 1;
	}
	else
	{
		char		path[MAXPGPATH];
		char	   *histfile;

		TLHistoryFilePath(path, tli);

		/* 从适当的源获取历史文件                              */
		if (is_source)
			histfile = source->fetch_file(source, path, NULL);
		else
			histfile = slurpFile(datadir_target, path, NULL);

		history = rewind_parseTimeLineHistory(histfile, tli, nentries);
		pg_free(histfile);
	}

	/* 在调试模式下，打印读取的内容                        */
	if (debug)
	{
		int			i;

		if (is_source)
			pg_log_debug("Source timeline history:");
		else
			pg_log_debug("Target timeline history:");

		for (i = 0; i < *nentries; i++)
		{
			TimeLineHistoryEntry *entry;

			entry = &history[i];
			pg_log_debug("%u: %X/%X - %X/%X", entry->tli,
						 LSN_FORMAT_ARGS(entry->begin),
						 LSN_FORMAT_ARGS(entry->end));
		}
	}

	return history;
}

/*
 * 确定两个集簇时间线历史中最后一条公共时间线的 TLI。
 * *tliIndex 设为数组中最后公共时间线的索引，*recptr
 * 设为时间线历史分叉的位置（即两个集簇中不相同的
 * 第一个 WAL 记录）。
                                                                                                                                                                                                  */
static void
findCommonAncestorTimeline(TimeLineHistoryEntry *a_history, int a_nentries,
						   TimeLineHistoryEntry *b_history, int b_nentries,
						   XLogRecPtr *recptr, int *tliIndex)
{
	int			i,
				n;

	/*
	 * 向前追溯至 timeline 分岔。源和目标可用相同          
	 * timeline 号，但因各自恢复获取的历史文件不同， 
	 * 起始位置可不同。需检查新 timeline 起始位置，     
	 * 若不匹配则下移一条额外 timeline 条目。                 
	 * 若不匹配则下移一条额外 timeline 条目。                  
	 * 不匹配则多移一 timeline 条目。                               
	 */
	n = Min(a_nentries, b_nentries);
	for (i = 0; i < n; i++)
	{
		if (a_history[i].tli != b_history[i].tli ||
			a_history[i].begin != b_history[i].begin)
			break;
	}

	if (i > 0)
	{
		i--;
		*recptr = MinXLogRecPtr(a_history[i].end, b_history[i].end);
		*tliIndex = i;
		return;
	}
	else
	{
		pg_fatal("could not find common ancestor of the source and target cluster's timelines");
	}
}


/*
 * 创建 backup_label 文件，强制从最后一个公共
 * 检查点开始恢复。
                                                   */
static void
createBackupLabel(XLogRecPtr startpoint, TimeLineID starttli, XLogRecPtr checkpointloc)
{
	XLogSegNo	startsegno;
	time_t		stamp_time;
	char		strfbuf[128];
	char		xlogfilename[MAXFNAMELEN];
	struct tm  *tmp;
	char		buf[1000];
	int			len;

	XLByteToSeg(startpoint, startsegno, WalSegSz);
	XLogFileName(xlogfilename, starttli, startsegno, WalSegSz);

	/*
	 * Construct backup label file
	 */
	stamp_time = time(NULL);
	tmp = localtime(&stamp_time);
	strftime(strfbuf, sizeof(strfbuf), "%Y-%m-%d %H:%M:%S %Z", tmp);

	len = snprintf(buf, sizeof(buf),
				   "START WAL LOCATION: %X/%X (file %s)\n"
				   "CHECKPOINT LOCATION: %X/%X\n"
				   "BACKUP METHOD: pg_rewind\n"
				   "BACKUP FROM: standby\n"
				   "START TIME: %s\n",
	/* 省略 LABEL: 行      */
				   LSN_FORMAT_ARGS(startpoint), xlogfilename,
				   LSN_FORMAT_ARGS(checkpointloc),
				   strfbuf);
	if (len >= sizeof(buf))
		pg_fatal("backup label buffer too small");	/* shouldn't happen */

	/* TODO：如果存在旧文件，将其移开。                          */
	open_target_file("backup_label", true); /* BACKUP_LABEL_FILE */
	write_target_range(buf, 0, len);
	close_target_file();
}

/*
 * 检查控制文件的 CRC
               */
static void
checkControlFile(ControlFileData *ControlFile)
{
	pg_crc32c	crc;

	/* 计算 CRC        */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (char *) ControlFile, offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	/* 然后简单比较                */
	if (!EQ_CRC32C(crc, ControlFile->crc))
		pg_fatal("unexpected control file CRC");
}

/*
 * 验证缓冲区 'content' 中的控制文件内容，并将其
 * 复制到 *ControlFile。
                                     */
static void
digestControlFile(ControlFileData *ControlFile, const char *content,
				  size_t size)
{
	if (size != PG_CONTROL_FILE_SIZE)
		pg_fatal("unexpected control file size %d, expected %d",
				 (int) size, PG_CONTROL_FILE_SIZE);

	memcpy(ControlFile, content, sizeof(ControlFileData));

	/* 设置并验证 WalSegSz            */
	WalSegSz = ControlFile->xlog_seg_size;

	if (!IsValidWalSegSize(WalSegSz))
		pg_fatal(ngettext("WAL segment size must be a power of two between 1 MB and 1 GB, but the control file specifies %d byte",
						  "WAL segment size must be a power of two between 1 MB and 1 GB, but the control file specifies %d bytes",
						  WalSegSz),
				 WalSegSz);

	/* 对控制文件的额外检查                        */
	checkControlFile(ControlFile);
}

/*
 * 从目标集簇获取 GUC 参数 restore_command 的值。         
 *
 * 这使用基于 "postgres -C" 的逻辑从集簇获取该值。   
 *         
 */
static void
getRestoreCommand(const char *argv0)
{
	int			rc;
	char		postgres_exec_path[MAXPGPATH],
				cmd_output[MAXPGPATH];
	PQExpBuffer postgres_cmd;

	if (!restore_wal)
		return;

	/* 查找 postgres 可执行文件        */
	rc = find_other_exec(argv0, "postgres",
						 PG_BACKEND_VERSIONSTR,
						 postgres_exec_path);

	if (rc < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (rc == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 "postgres", progname, full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 "postgres", full_path, progname);
	}

	/*
	 * 构造命令以获取 GUC restore_command 值（若设）  
	 * 。                     
	 */
	postgres_cmd = createPQExpBuffer();

	/* postgres 的路径，适当引用                 */
	appendShellString(postgres_cmd, postgres_exec_path);

	/* 添加 -D 开关，带适当引用的数据目录                                */
	appendPQExpBufferStr(postgres_cmd, " -D ");
	appendShellString(postgres_cmd, datadir_target);

	/* 仅在请求时添加自定义配置文件                                  */
	if (config_file != NULL)
	{
		appendPQExpBufferStr(postgres_cmd, " -c config_file=");
		appendShellString(postgres_cmd, config_file);
	}

	/* 添加 -C 开关，用于 restore_command        */
	appendPQExpBufferStr(postgres_cmd, " -C restore_command");

	if (!pipe_read_line(postgres_cmd->data, cmd_output, sizeof(cmd_output)))
		exit(1);

	(void) pg_strip_crlf(cmd_output);

	if (strcmp(cmd_output, "") == 0)
		pg_fatal("restore_command is not set in the target cluster");

	restore_command = pg_strdup(cmd_output);

	pg_log_debug("using for rewind restore_command = \'%s\'",
				 restore_command);

	destroyPQExpBuffer(postgres_cmd);
}


/*
 * 通过启动单用户模式的 postgres 进行崩溃恢复，
 * 确保目标实例干净关闭。
                                                               */
static void
ensureCleanShutdown(const char *argv0)
{
	int			ret;
#define MAXCMDLEN (2 * MAXPGPATH)
	char		exec_path[MAXPGPATH];
	PQExpBuffer postgres_cmd;

	/* 定位 postgres 二进制文件      */
	if ((ret = find_other_exec(argv0, "postgres",
							   PG_BACKEND_VERSIONSTR,
							   exec_path)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 "postgres", progname, full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 "postgres", full_path, progname);
	}

	pg_log_info("executing \"%s\" for target server to complete crash recovery",
				exec_path);

	/*
	 * 如需跳过则跳过，但仅在确认 postgres 存在后。    
	 * 。      
	 */
	if (dry_run)
		return;

	/*
	 * 最后以单用户模式运行 postgres。这里不需要使用   
	 * fsync。这使恢复更快，且目标数据目录最后无论如何   
	 * 都会被同步。          
	 */
	postgres_cmd = createPQExpBuffer();

	/* postgres 的路径，适当引用                 */
	appendShellString(postgres_cmd, exec_path);

	/* 添加选项集，带适当引用的数据目录                                       */
	appendPQExpBufferStr(postgres_cmd, " --single -F -D ");
	appendShellString(postgres_cmd, datadir_target);

	/* 仅在请求时添加自定义配置文件                                  */
	if (config_file != NULL)
	{
		appendPQExpBufferStr(postgres_cmd, " -c config_file=");
		appendShellString(postgres_cmd, config_file);
	}

	/* 以数据库名结束，带适当引用的重定向                                                */
	appendPQExpBufferStr(postgres_cmd, " template1 < ");
	appendShellString(postgres_cmd, DEVNULL);

	fflush(NULL);
	if (system(postgres_cmd->data) != 0)
	{
		pg_log_error("postgres single-user mode in target cluster failed");
		pg_log_error_detail("Command was: %s", postgres_cmd->data);
		exit(1);
	}

	destroyPQExpBuffer(postgres_cmd);
}

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}
