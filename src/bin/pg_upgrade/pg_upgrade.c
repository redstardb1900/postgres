/*
 *	pg_upgrade.c
 *
 *	main source file
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/pg_upgrade.c
 */

/*
 *	为简化升级，我们强制某些系统值在新旧集簇间保持
 *	一致：                              
 *
 *	我们控制 pg_class.oid（含 relfilenode）的全部赋值，使  
 *	toast oid 在新旧集簇间保持一致。这很重要，因为  
 *	toast oid 以 toast 指针的形式存储在用户表中。    
 *
 *	虽然 pg_class.oid 和 pg_class.relfilenode 在一个集簇中最初  
 *	相同，但可能因 CLUSTER、REINDEX 或 VACUUM FULL 而          
 *	产生分歧。我们控制 pg_class.relfilenode 的赋值，使文件   
 *	名在新旧集簇间匹配。           
 *
 *	我们控制 pg_tablespace.oid 的赋值，让 oid 在新旧集簇之间   
 *	相互匹配。                 
 *
 *	我们控制 pg_type.oid 的所有赋值，因这些 oid 存于用户  
 *	复合类型值中。         
 *
 *	我们控制 pg_enum.oid 的所有赋值，因这些 oid 作为枚举  
 *	值存储在用户表中。   
 *
 *	我们控制 pg_authid.oid 所有赋值是因历史原因（这些 oid 
 *	曾被存储在 pg_largeobject_metadata 中，现在通过 SQL 命令     
 *	复制），这一点将来可能会改变。                
 */



#include "postgres_fe.h"

#include <time.h>

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

#include "catalog/pg_class_d.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "common/restricted_token.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"

static void set_locale_and_encoding(void);
static void prepare_new_cluster(void);
static void prepare_new_globals(void);
static void create_new_objects(void);
static void copy_xact_xlog_xid(void);
static void set_frozenxids(bool minmxid_only);
static void make_outputdirs(char *pgdata);
static void setup(char *argv0, bool *live_check);

ClusterInfo old_cluster,
			new_cluster;
OSInfo		os_info;

char	   *output_files[] = {
	SERVER_LOG_FILE,
#ifdef WIN32
	/* pg_ctl start 唯文          */
	SERVER_START_LOG_FILE,
#endif
	UTILITY_LOG_FILE,
	INTERNAL_LOG_FILE,
	NULL
};


int
main(int argc, char **argv)
{
	char	   *deletion_script_file_name = NULL;
	bool		live_check = false;

	/*
	 * pg_upgrade 当前并不使用 common/logging.c，但因为可能    
	 * 调用它的通用代码，仍要初始化。      
	 */
	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_upgrade"));

	/* 设置默认限制权限掩码，直至读取新集簇权限为止  */
	umask(PG_MODE_MASK_OWNER);

	parseCommandLine(argc, argv);

	get_restricted_token();

	adjust_data_dir(&old_cluster);
	adjust_data_dir(&new_cluster);

	/*
	 * 根据 PGDATA 权限设置掩码，需以正确权限创建输出   
	 * 目录所必需的。                       
	 */
	if (!GetDataDirectoryCreatePerm(new_cluster.pgdata))
		pg_fatal("could not read permissions of directory \"%s\": %s",
				 new_cluster.pgdata, strerror(errno));

	umask(pg_mode_mask);

	/*
	 * 此操作需在 adjust_data_dir() 调整新集簇数据目录后  
	 * 进行。                    
	 */
	make_outputdirs(new_cluster.pgdata);

	setup(argv[0], &live_check);

	output_check_banner(live_check);

	check_cluster_versions();

	get_sock_dir(&old_cluster, live_check);
	get_sock_dir(&new_cluster, false);

	check_cluster_compatibility(live_check);

	check_and_dump_old_cluster(live_check);


	/* -- 新 -- */
	start_postmaster(&new_cluster, true);

	check_new_cluster();
	report_clusters_compatible();

	pg_log(PG_REPORT,
		   "\n"
		   "Performing Upgrade\n"
		   "------------------");

	set_locale_and_encoding();

	prepare_new_cluster();

	stop_postmaster(false);

	/*
	 * 对新集簇的破坏性更改    
	 */

	copy_xact_xlog_xid();

	/* 新用旧 xid                        */

	/* -- 新 -- */
	start_postmaster(&new_cluster, true);

	prepare_new_globals();

	create_new_objects();

	stop_postmaster(false);

	/*
	 * 大多数故障发生在 create_new_objects() 中，该函数此时 
	 * 已执行完毕。此处执行因它恰在链接之前，而链接   
	 * 会把新旧集簇数据文件链接起来，使旧集簇在新 
	 * 集簇启动后无法再安全启动。                           
	 */
	if (user_opts.transfer_mode == TRANSFER_MODE_LINK)
		disable_old_cluster();

	transfer_all_new_tablespaces(&old_cluster.dbarr, &new_cluster.dbarr,
								 old_cluster.pgdata, new_cluster.pgdata);

	/*
	 * 假设 OID 只用于系统表，则无需恢复 OID 计数器， 
	 * 因为我们没有从旧系统传输任何 OID，但为保险起见  
	 * 还是执行了。之所以在这里较晚执行，是因为无需让
	 * 模式加载使用新的 oid。                               
	 */
	prep_status("Setting next OID for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -o %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtoid,
			  new_cluster.pgdata);
	check_ok();

	if (user_opts.do_sync)
	{
		prep_status("Sync data directory to disk");
		exec_prog(UTILITY_LOG_FILE, NULL, true, true,
				  "\"%s/initdb\" --sync-only \"%s\"", new_cluster.bindir,
				  new_cluster.pgdata);
		check_ok();
	}

	create_script_for_old_cluster_deletion(&deletion_script_file_name);

	issue_warnings_and_set_wal_level();

	pg_log(PG_REPORT,
		   "\n"
		   "Upgrade Complete\n"
		   "----------------");

	output_completion_banner(deletion_script_file_name);

	pg_free(deletion_script_file_name);

	cleanup_output_dirs();

	return 0;
}

/*
 * 创建用于存放内部生成数据的输出目录集，并赋      
 * 予适当权限，在此过程中填充                            
 * log_opts。 
 */
static void
make_outputdirs(char *pgdata)
{
	FILE	   *fp;
	char	  **filename;
	time_t		run_time = time(NULL);
	char		filename_path[MAXPGPATH];
	char		timebuf[128];
	struct timeval time;
	time_t		tt;
	int			len;

	log_opts.rootdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.rootdir, MAXPGPATH, "%s/%s", pgdata, BASE_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/*输出目录/$timestamp/    */
	gettimeofday(&time, NULL);
	tt = (time_t) time.tv_sec;
	strftime(timebuf, sizeof(timebuf), "%Y%m%dT%H%M%S", localtime(&tt));
	/* 追毫秒           */
	snprintf(timebuf + strlen(timebuf), sizeof(timebuf) - strlen(timebuf),
			 ".%03d", (int) (time.tv_usec / 1000));
	log_opts.basedir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.basedir, MAXPGPATH, "%s/%s", log_opts.rootdir,
				   timebuf);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/*输出目录/$timestamp/导出/  */
	log_opts.dumpdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.dumpdir, MAXPGPATH, "%s/%s/%s", log_opts.rootdir,
				   timebuf, DUMP_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/*输出目录/$timestamp/日志/ */
	log_opts.logdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.logdir, MAXPGPATH, "%s/%s/%s", log_opts.rootdir,
				   timebuf, LOG_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/*
	 * 忽略根路径已存在的错误，因其在多次运行间保持 
	 * 不变。        
	 */
	if (mkdir(log_opts.rootdir, pg_dir_create_mode) < 0 && errno != EEXIST)
		pg_fatal("could not create directory \"%s\": %m", log_opts.rootdir);
	if (mkdir(log_opts.basedir, pg_dir_create_mode) < 0)
		pg_fatal("could not create directory \"%s\": %m", log_opts.basedir);
	if (mkdir(log_opts.dumpdir, pg_dir_create_mode) < 0)
		pg_fatal("could not create directory \"%s\": %m", log_opts.dumpdir);
	if (mkdir(log_opts.logdir, pg_dir_create_mode) < 0)
		pg_fatal("could not create directory \"%s\": %m", log_opts.logdir);

	len = snprintf(filename_path, sizeof(filename_path), "%s/%s",
				   log_opts.logdir, INTERNAL_LOG_FILE);
	if (len >= sizeof(filename_path))
		pg_fatal("directory path for new cluster is too long");

	if ((log_opts.internal = fopen_priv(filename_path, "a")) == NULL)
		pg_fatal("could not open log file \"%s\": %m", filename_path);

	/* 日志中标升级始              */
	for (filename = output_files; *filename != NULL; filename++)
	{
		len = snprintf(filename_path, sizeof(filename_path), "%s/%s",
					   log_opts.logdir, *filename);
		if (len >= sizeof(filename_path))
			pg_fatal("directory path for new cluster is too long");
		if ((fp = fopen_priv(filename_path, "a")) == NULL)
			pg_fatal("could not write to log file \"%s\": %m", filename_path);

		fprintf(fp,
				"-----------------------------------------------------------------\n"
				"  pg_upgrade run on %s"
				"-----------------------------------------------------------------\n\n",
				ctime(&run_time));
		fclose(fp);
	}
}


static void
setup(char *argv0, bool *live_check)
{
	/*
	 * 确保用户环境干净，否则连接其中一台（或两台）服
	 * 务器时，可能会使 libpq 产生混淆。        
	 */
	check_pghost_envvar();

	/*
	 * 若用户未用 -B 指定新二进制程序的目录，则默认    
	 * 使用当前所执行的 pg_upgrade 可执行文件所在的           
	 * 路径 
	 */
	if (!new_cluster.bindir)
	{
		char		exec_path[MAXPGPATH];

		if (find_my_exec(argv0, exec_path) < 0)
			pg_fatal("%s: could not find own program executable", argv0);
		/* 裁程序名留路径                    */
		*last_dir_separator(exec_path) = '\0';
		canonicalize_path(exec_path);
		new_cluster.bindir = pg_strdup(exec_path);
	}

	verify_directories();

	/* 除实检外不应运行 postmaster                       */
	if (pid_lock_file_exists(old_cluster.pgdata))
	{
		/*
		 * 若有 postmaster.pid 文件，就尝试启动服务器。若它 
		 * 能启动，说明 pid 文件过时，故停止服务器。若不 
		 * 能启动，则假定服务器在运行。若 pid 文件是崩溃 
		 * 遗留的，也让 WAL 中已提交的事务得到重放而不  
		 * 丢失，因 WAL 文件不会从旧服务器传到新服务器。 
		 * 我们稍后会检查服务器是否已                           
		 * 干净关闭。      
		 */
		if (start_postmaster(&old_cluster, false))
			stop_postmaster(false);
		else
		{
			if (!user_opts.check)
				pg_fatal("There seems to be a postmaster servicing the old cluster.\n"
						 "Please shutdown that postmaster and try again.");
			else
				*live_check = true;
		}
	}

	/* 新 postmaster 同理            */
	if (pid_lock_file_exists(new_cluster.pgdata))
	{
		if (start_postmaster(&new_cluster, false))
			stop_postmaster(false);
		else
			pg_fatal("There seems to be a postmaster servicing the new cluster.\n"
					 "Please shutdown that postmaster and try again.");
	}
}


/*
 * 将语言环境和编码信息复制到新集簇的 template0 中。  
 *
 * 我们需要复制 encoding、datlocprovider、datcollate、           
 * datctype 和 daticulocale。datcollversion 无需复制，template0    
 * 未设。 
 */
static void
set_locale_and_encoding(void)
{
	PGconn	   *conn_new_template1;
	char	   *datcollate_literal;
	char	   *datctype_literal;
	char	   *daticulocale_literal = NULL;
	DbLocaleInfo *locale = old_cluster.template0;

	prep_status("Setting locale and encoding for new cluster");

	/* 按新集簇转义字面                    */
	conn_new_template1 = connectToServer(&new_cluster, "template1");

	datcollate_literal = PQescapeLiteral(conn_new_template1,
										 locale->db_collate,
										 strlen(locale->db_collate));
	datctype_literal = PQescapeLiteral(conn_new_template1,
									   locale->db_ctype,
									   strlen(locale->db_ctype));

	if (locale->db_iculocale)
		daticulocale_literal = PQescapeLiteral(conn_new_template1,
											   locale->db_iculocale,
											   strlen(locale->db_iculocale));
	else
		daticulocale_literal = "NULL";

	/* 更新新集簇 template0       */
	if (GET_MAJOR_VERSION(new_cluster.major_version) >= 1500)
		PQclear(executeQueryOrDie(conn_new_template1,
								  "UPDATE pg_catalog.pg_database "
								  "  SET encoding = %d, "
								  "      datlocprovider = '%c', "
								  "      datcollate = %s, "
								  "      datctype = %s, "
								  "      daticulocale = %s "
								  "  WHERE datname = 'template0' ",
								  locale->db_encoding,
								  locale->db_collprovider,
								  datcollate_literal,
								  datctype_literal,
								  daticulocale_literal));
	else
		PQclear(executeQueryOrDie(conn_new_template1,
								  "UPDATE pg_catalog.pg_database "
								  "  SET encoding = %d, "
								  "      datcollate = %s, "
								  "      datctype = %s "
								  "  WHERE datname = 'template0' ",
								  locale->db_encoding,
								  datcollate_literal,
								  datctype_literal));

	PQfreemem(datcollate_literal);
	PQfreemem(datctype_literal);
	if (locale->db_iculocale)
		PQfreemem(daticulocale_literal);

	PQfinish(conn_new_template1);

	check_ok();
}


static void
prepare_new_cluster(void)
{
	/*
	 * 在加载模式后再冻结更合理，但那会使我们丢失加载
	 * 的 frozenxids。我们使用 --analyze，这样 autovacuum       
	 * 以后不会更新统计信息                         
	 */
	prep_status("Analyzing all rows in the new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/vacuumdb\" %s --all --analyze %s",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.verbose ? "--verbose" : "");
	check_ok();

	/*
	 * 我们在分析后冻结，这样 pg_statistic 也一并被冻结。   
	 * template0 不在此冻结，其数据行已由 initdb 冻结，而  
	 * 稍后设置 datfrozenxid/relfrozenxids/relminmxid 以匹配新的    
	 * xid 计数器 
	 */
	prep_status("Freezing all rows in the new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/vacuumdb\" %s --all --freeze %s",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.verbose ? "--verbose" : "");
	check_ok();
}


static void
prepare_new_globals(void)
{
	/*
	 * 恢复任何内容之前，先设置 initdb 建表的 frozenxids。 
	 */
	set_frozenxids(false);

	/*
	 * 恢复全局对象（角色和表空间）。      
	 */
	prep_status("Restoring global objects in the new cluster");

	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/psql\" " EXEC_PSQL_ARGS " %s -f \"%s/%s\"",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.dumpdir,
			  GLOBALS_DUMP_FILE);
	check_ok();
}


static void
create_new_objects(void)
{
	int			dbnum;

	prep_status_progress("Restoring database schemas in the new cluster");

	/*
	 * 我们无法与其他数据库并行处理 template1 数据库， 
	 * 因为当它被临时删除时，连接尝试将会失败。所以要 
	 * 在单独的不可并行步骤中处理它。    
	 */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char		sql_file_name[MAXPGPATH],
					log_file_name[MAXPGPATH];
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];
		const char *create_opts;

		/* 仅处 template1                    */
		if (strcmp(old_db->db_name, "template1") != 0)
			continue;

		pg_log(PG_STATUS, "%s", old_db->db_name);
		snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
		snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);

		/*
		 * template1 数据库已存在于目标安装中，所以要告诉 
		 * pg_restore 将其删除并重建；否则我们将无法传播它  
		 * 的数据库级属性。                   
		 */
		create_opts = "--clean --create";

		exec_prog(log_file_name,
				  NULL,
				  true,
				  true,
				  "\"%s/pg_restore\" %s %s --exit-on-error --verbose "
				  "--dbname postgres \"%s/%s\"",
				  new_cluster.bindir,
				  cluster_conn_opts(&new_cluster),
				  create_opts,
				  log_opts.dumpdir,
				  sql_file_name);

		break;					/* 处完 template1 即完             */
	}

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		char		sql_file_name[MAXPGPATH],
					log_file_name[MAXPGPATH];
		DbInfo	   *old_db = &old_cluster.dbarr.dbs[dbnum];
		const char *create_opts;

		/* 跳 template1               */
		if (strcmp(old_db->db_name, "template1") == 0)
			continue;

		pg_log(PG_STATUS, "%s", old_db->db_name);
		snprintf(sql_file_name, sizeof(sql_file_name), DB_DUMP_FILE_MASK, old_db->db_oid);
		snprintf(log_file_name, sizeof(log_file_name), DB_DUMP_LOG_FILE_MASK, old_db->db_oid);

		/*
		 * postgres 数据库已存在于目标安装中，所以要告诉    
		 * pg_restore 将其删除并重建；否则我们将无法传播它  
		 * 的数据库级属性。                
		 */
		if (strcmp(old_db->db_name, "postgres") == 0)
			create_opts = "--clean --create";
		else
			create_opts = "--create";

		parallel_exec_prog(log_file_name,
						   NULL,
						   "\"%s/pg_restore\" %s %s --exit-on-error --verbose "
						   "--dbname template1 \"%s/%s\"",
						   new_cluster.bindir,
						   cluster_conn_opts(&new_cluster),
						   create_opts,
						   log_opts.dumpdir,
						   sql_file_name);
	}

	/* 收子进程      */
	while (reap_child(true) == true)
		;

	end_progress_output();
	check_ok();

	/*
	 * 在 9.3 之前的集簇中，我们没有数据库或关系级的     
	 * minmxids，所以恢复模式后再设置。   
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 902)
		set_frozenxids(true);

	/* DB 中现有对象，更新 new_cluster 信息                    */
	get_db_and_rel_infos(&new_cluster);
}

/*
 * 从新集簇中删除给定子目录内容                 
 */
static void
remove_new_subdir(const char *subdir, bool rmtopdir)
{
	char		new_path[MAXPGPATH];

	prep_status("Deleting files from new %s", subdir);

	snprintf(new_path, sizeof(new_path), "%s/%s", new_cluster.pgdata, subdir);
	if (!rmtree(new_path, rmtopdir))
		pg_fatal("could not delete directory \"%s\"", new_path);

	check_ok();
}

/*
 * 从旧集簇复制文件到新集簇       
 */
static void
copy_subdir_files(const char *old_subdir, const char *new_subdir)
{
	char		old_path[MAXPGPATH];
	char		new_path[MAXPGPATH];

	remove_new_subdir(new_subdir, true);

	snprintf(old_path, sizeof(old_path), "%s/%s", old_cluster.pgdata, old_subdir);
	snprintf(new_path, sizeof(new_path), "%s/%s", new_cluster.pgdata, new_subdir);

	prep_status("Copying old %s to new server", old_subdir);

	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
#ifndef WIN32
			  "cp -Rf \"%s\" \"%s\"",
#else
	/* 标志：全部，无确认，静默，覆只读          */
			  "xcopy /e /y /q /r \"%s\" \"%s\\\"",
#endif
			  old_path, new_path);

	check_ok();
}

static void
copy_xact_xlog_xid(void)
{
	/*
	 * 把旧的提交日志复制到新数据目录。pg_clog 在      
	 * 10 后更名为 pg_xact。  
	 */
	copy_subdir_files(GET_MAJOR_VERSION(old_cluster.major_version) <= 906 ?
					  "pg_clog" : "pg_xact",
					  GET_MAJOR_VERSION(new_cluster.major_version) <= 906 ?
					  "pg_clog" : "pg_xact");

	prep_status("Setting oldest XID for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -u %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_oldstxid,
			  new_cluster.pgdata);
	check_ok();

	/* 设新下个事务 id 和 epoch                          */
	prep_status("Setting next transaction ID and epoch for new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -x %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtxid,
			  new_cluster.pgdata);
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -e %u \"%s\"",
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtepoch,
			  new_cluster.pgdata);
	/* 也须重提交时戳限                */
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/pg_resetwal\" -f -c %u,%u \"%s\"",
			  new_cluster.bindir,
			  old_cluster.controldata.chkpnt_nxtxid,
			  old_cluster.controldata.chkpnt_nxtxid,
			  new_cluster.pgdata);
	check_ok();

	/*
	 * 如果旧服务器早于 MULTIXACT_FORMATCHANGE_CAT_VER 这一       
	 * 变更（见 pg_upgrade.h）而新服务器晚于它，那么我  
	 * 们就不会复制 pg_multixact 文件，但仍需重置           
	 * pg_control，使新服务器不读取早于截止值的 multi。  
	 */
	if (old_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER &&
		new_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER)
	{
		copy_subdir_files("pg_multixact/offsets", "pg_multixact/offsets");
		copy_subdir_files("pg_multixact/members", "pg_multixact/members");

		prep_status("Setting next multixact ID and offset for new cluster");

		/*
		 * 我们保留全部文件和内容，因此必须同时保留"next" 
		 * 两个计数器和系统上现存的最旧 multi。  
		 */
		exec_prog(UTILITY_LOG_FILE, NULL, true, true,
				  "\"%s/pg_resetwal\" -O %u -m %u,%u \"%s\"",
				  new_cluster.bindir,
				  old_cluster.controldata.chkpnt_nxtmxoff,
				  old_cluster.controldata.chkpnt_nxtmulti,
				  old_cluster.controldata.chkpnt_oldstMulti,
				  new_cluster.pgdata);
		check_ok();
	}
	else if (new_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER)
	{
		/*
		 * 删除 initdb 创建的、已不匹配新 multi-xid 值的       
		 * offsets/0000 文件。"members" 从零开始，因此无需删  
		 * 除它。 
		 */
		remove_new_subdir("pg_multixact/offsets", false);

		prep_status("Setting oldest multixact ID in new cluster");

		/*
		 * 此情况下我们不保留文件，但重要的是让最旧的  
		 * multi 被设为旧系统用过的最新值，这样 multixact.c   
		 * 会对磁盘上可能存在的 multi 返回空集。我们把  
		 * next multi 设置为紧随其后的值；如果旧集簇的        
		 * next=MaxMultiXactId，则它可能回绕（即 0），但  
		 * multixact.c 完全可以应付这种情况。                     
		 */
		exec_prog(UTILITY_LOG_FILE, NULL, true, true,
				  "\"%s/pg_resetwal\" -m %u,%u \"%s\"",
				  new_cluster.bindir,
				  old_cluster.controldata.chkpnt_nxtmulti + 1,
				  old_cluster.controldata.chkpnt_nxtmulti,
				  new_cluster.pgdata);
		check_ok();
	}

	/* 重置新 WAL 归档                          */
	prep_status("Resetting WAL archives");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
	/* 用 timeline 1 匹 controldata 无 WAL 历                  */
			  "\"%s/pg_resetwal\" -l 00000001%s \"%s\"", new_cluster.bindir,
			  old_cluster.controldata.nextxlogfile + 8,
			  new_cluster.pgdata);
	check_ok();
}


/*
 *	set_frozenxids()
 *
 * 它在我们恢复任何内容之前在新集簇上被调用，   
 * minmxid_only = false。其目的是确保所有由 initdb 创建       
 * 的可 vacuum 表都拥有与旧集簇 xid/mxid 计数器相匹配       
 * 的 relfrozenxid/relminmxid。还会设置内建数据库的 datfrozenxid
 * /datminmxid 以匹配。    
 *
 * 我们稍后创建用户表时，其 relfrozenxid/relminmxid 字段将   
 * 由 binary-upgrade 恢复脚本正确地恢复。用户数据库的     
 * datfrozenxid/datminmxid 也是如此。然而，如果我们要从        
 * 9.3 之前的数据库升级，它并不存储每表或每数据库的  
 * minmxid，那么恢复脚本填充的 relminmxid/datminmxid 值就        
 * 是零。 
 *
 * 因此，源数据库为 9.3 之前的版本时，在一切恢复
 * 完毕后再调用，minmxid_only = true。此轮会用期望的 
 * minmxid 值初始化所有的表和数据库，既包括               
 * initdb 创建的也包括用户对象。frozenxid 值保持不变。      
 */
static void
set_frozenxids(bool minmxid_only)
{
	int			dbnum;
	PGconn	   *conn,
			   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;

	if (!minmxid_only)
		prep_status("Setting frozenxid and minmxid counters in new cluster");
	else
		prep_status("Setting minmxid counter in new cluster");

	conn_template1 = connectToServer(&new_cluster, "template1");

	if (!minmxid_only)
		/* 设 pg_database.datfrozenxid */
		PQclear(executeQueryOrDie(conn_template1,
								  "UPDATE pg_catalog.pg_database "
								  "SET	datfrozenxid = '%u'",
								  old_cluster.controldata.chkpnt_nxtxid));

	/* 设 pg_database.datminmxid */
	PQclear(executeQueryOrDie(conn_template1,
							  "UPDATE pg_catalog.pg_database "
							  "SET	datminmxid = '%u'",
							  old_cluster.controldata.chkpnt_nxtmulti));

	/* 取数据名       */
	dbres = executeQueryOrDie(conn_template1,
							  "SELECT	datname, datallowconn "
							  "FROM	pg_catalog.pg_database");

	i_datname = PQfnumber(dbres, "datname");
	i_datallowconn = PQfnumber(dbres, "datallowconn");

	ntups = PQntuples(dbres);
	for (dbnum = 0; dbnum < ntups; dbnum++)
	{
		char	   *datname = PQgetvalue(dbres, dbnum, i_datname);
		char	   *datallowconn = PQgetvalue(dbres, dbnum, i_datallowconn);

		/*
		 * 须更新 datallowconn = false 的数据库（如        
		 * template0），因为即使关闭了 autovacuum，它也会  
		 * 递增它们的 datfrozenxids、relfrozenxids 和 relminmxid，    
		 * 即便所有数据行都已冻结。为做到这一点，我们会
		 * 暂时改变 datallowconn。       
		 */
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(conn_template1,
									  "ALTER DATABASE %s ALLOW_CONNECTIONS = true",
									  quote_identifier(datname)));

		conn = connectToServer(&new_cluster, datname);

		if (!minmxid_only)
			/* 设 pg_class.relfrozenxid */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE	pg_catalog.pg_class "
									  "SET	relfrozenxid = '%u' "
			/* 仅堆/物化视图/TOAST 被 vacuum                 */
									  "WHERE	relkind IN ("
									  CppAsString2(RELKIND_RELATION) ", "
									  CppAsString2(RELKIND_MATVIEW) ", "
									  CppAsString2(RELKIND_TOASTVALUE) ")",
									  old_cluster.controldata.chkpnt_nxtxid));

		/* 设 pg_class.relminmxid */
		PQclear(executeQueryOrDie(conn,
								  "UPDATE	pg_catalog.pg_class "
								  "SET	relminmxid = '%u' "
		/* 仅堆/物化视图/TOAST 被 vacuum                 */
								  "WHERE	relkind IN ("
								  CppAsString2(RELKIND_RELATION) ", "
								  CppAsString2(RELKIND_MATVIEW) ", "
								  CppAsString2(RELKIND_TOASTVALUE) ")",
								  old_cluster.controldata.chkpnt_nxtmulti));
		PQfinish(conn);

		/*重置datallowconn 标志*/
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(conn_template1,
									  "ALTER DATABASE %s ALLOW_CONNECTIONS = false",
									  quote_identifier(datname)));
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	check_ok();
}
