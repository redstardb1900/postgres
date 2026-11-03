/*
 *	pg_upgrade.c
 *
 *	main source file
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/pg_upgrade.c
 */

/*简化升级：强制新旧集簇间系统值一致。控 pg_class.oid 和 relfilenode 赋值使 toast oid 一致（因 toast 指针存于用户表）。虽 oid 和 relfilenode 初始同但 CLUSTER/REINDEX/VACUUM FULL 可使其不同，控 relfilenode 分配使文件名匹配。控 pg_tablespace.oid 使 oid 匹配。控 pg_type.oid 因存于用户复合类型。控 pg_enum.oid 因存于用户枚举值。控 pg_authid.oid（历史原因，oid 曾存 pg_largeobject_metadata，今用 SQL 复制），未来可改。
 *	
 *	
 *
 *	
 *	
 *	
 *
 *	
 *	
 *	
 *	
 *
 *	
 *	
 *
 *	
 *	
 *
 *	
 *	
 *
 *	
 *	
 *	
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

	/*pg_upgrade 当前未用 common/logging.c，但初始化因可能调用使用它的通用代码。
	 * 
	 * 
	                       */
	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_upgrade"));

	/* 设默限掩码至读新集簇权                                   */
	umask(PG_MODE_MASK_OWNER);

	parseCommandLine(argc, argv);

	get_restricted_token();

	adjust_data_dir(&old_cluster);
	adjust_data_dir(&new_cluster);

	/*据 PGDATA 权限设掩码，需正确创建输出目录。
	 * 
	 * 
	                                                      */
	if (!GetDataDirectoryCreatePerm(new_cluster.pgdata))
		pg_fatal("could not read permissions of directory \"%s\": %s",
				 new_cluster.pgdata, strerror(errno));

	umask(pg_mode_mask);

	/*在 adjust_data_dir() 调整新集簇数据目录后执行。
	 * 
	 * 
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

	/*对新集簇的破坏性更改
	 * 
	     */

	copy_xact_xlog_xid();

	/* 新用旧 xid                        */

	/* -- 新 -- */
	start_postmaster(&new_cluster, true);

	prepare_new_globals();

	create_new_objects();

	stop_postmaster(false);

	/*多数故障在 create_new_objects() 已完成。在此执行因恰在链接前，链接将新旧数据文件关联，新集簇启动后旧集簇不可安全启动。
	 * 
	 * 
	 * 
	 * 
	                                                                                                      */
	if (user_opts.transfer_mode == TRANSFER_MODE_LINK)
		disable_old_cluster();

	transfer_all_new_tablespaces(&old_cluster.dbarr, &new_cluster.dbarr,
								 old_cluster.pgdata, new_cluster.pgdata);

	/*假设 OID 仅系统表用，无需恢复 OID 计数器因未传输任何 OID，但为安全仍做。较晚执行因不需模式加载用新 oid。
	 * 
	 * 
	 * 
	 * 
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

/*创建输出目录集并分配适当权限，填充 log_opts。
 * 
 * 
 * 
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

	/* BASE_OUTPUTDIR/$timestamp/ */
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

	/* BASE_OUTPUTDIR/$timestamp/dump/ */
	log_opts.dumpdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.dumpdir, MAXPGPATH, "%s/%s/%s", log_opts.rootdir,
				   timebuf, DUMP_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/* BASE_OUTPUTDIR/$timestamp/log/ */
	log_opts.logdir = (char *) pg_malloc0(MAXPGPATH);
	len = snprintf(log_opts.logdir, MAXPGPATH, "%s/%s/%s", log_opts.rootdir,
				   timebuf, LOG_OUTPUTDIR);
	if (len >= MAXPGPATH)
		pg_fatal("directory path for new cluster is too long");

	/*忽略根路径已存在错误，因跨运行保持。
	 * 
	 * 
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
	/*确保用户环境干净，否则连接服务器时混淆 libpq。
	 * 
	 * 
	                                                          */
	check_pghost_envvar();

	/*用户未指定 -B 新二进制目录则用当前 pg_upgrade 路径。
	 * 
	 * 
	 * 
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
		/*有 postmaster.pid 则尝试启动。启动则 pid 过时故停止。未启动则假设在运行。pid 文件来自崩溃则此可重放 WAL 中的已提交事务（WAL 文件不传至新集簇），避免丢失。后检查干净关闭。
		 * 
		 * 
		 * 
		 * 
		 * 
		 * 
		 * 
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


/*将语言环境和编码复制到新集簇 template0。需复制 encoding、datlocprovider、datcollate、datctype、daticulocale。无需 datcollversion（从未为 template0 设）。
 * 
 *
 * 
 * 
 * 
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
	/*加载模式后冻结更合理但会丢失加载恢复的 frozenxids。用 --analyze 免 autovacuum 后改统计。
	 * 
	 * 
	 * 
	                                                                            */
	prep_status("Analyzing all rows in the new cluster");
	exec_prog(UTILITY_LOG_FILE, NULL, true, true,
			  "\"%s/vacuumdb\" %s --all --analyze %s",
			  new_cluster.bindir, cluster_conn_opts(&new_cluster),
			  log_opts.verbose ? "--verbose" : "");
	check_ok();

	/*分析后冻结使 pg_statistic 也被冻。template0 不在此冻但行已由 initdb 冻，后设 datfrozenxid/relfrozenxids/relminmxid 匹配新 xid 计数器。
	 * 
	 * 
	 * 
	 * 
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
	/*恢复前先设 initdb 建表的 frozenxids。
	 * 
	                       */
	set_frozenxids(false);

	/*恢复全局对象（角色和表空间）。
	 * 
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

	/*不能与其他数据库同时处理 template1 因被临时删除时连接失败。故在单独非并行过程中处理。
	 * 
	 * 
	 * 
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

		/*template1 已存在于目标安装，告诉 pg_restore 删并重建否则无法传播数据库级属性。
		 * 
		 * 
		 * 
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

		/*postgres 已存在于目标安装，告诉 pg_restore 删并重建否则无法传播数据库级属性。
		 * 
		 * 
		 * 
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

	/*9.3 前集簇无数据库或关系级 minmxids，恢复模式后设之。
	 * 
	 * 
	                                             */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 902)
		set_frozenxids(true);

	/* DB 中现有对象，更新 new_cluster 信息                    */
	get_db_and_rel_infos(&new_cluster);
}

/*从新集簇中删除给定子目录内容
 * 
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

/*从旧集簇复制文件到新集簇
 * 
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
	/*复制旧提交日志到新数据目录。pg_clog 在 post-10 集簇更名 pg_xact。
	 * 
	 * 
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

	/*若旧服务 MULTIXACT_FORMATCHANGE_CAT_VER 前新后，不复制 pg_multixact 文件但重置 pg_control 防新服务读截止前多事务。
	 * 
	 * 
	 * 
	 * 
	                                                                                                                               */
	if (old_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER &&
		new_cluster.controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER)
	{
		copy_subdir_files("pg_multixact/offsets", "pg_multixact/offsets");
		copy_subdir_files("pg_multixact/members", "pg_multixact/members");

		prep_status("Setting next multixact ID and offset for new cluster");

		/*保留所有文件和内容故保 "next" 计数器和系统最旧 multi。
		 * 
		 * 
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
		/*删 initdb 创建的 offsets/0000 因不匹配新 multi-xid。members 从零始无需删。
		 * 
		 * 
		 * 
		                                                 */
		remove_new_subdir("pg_multixact/offsets", false);

		prep_status("Setting oldest multixact ID in new cluster");

		/*此时不保留文件，但重要是最旧 multi 设为旧系统最新值使 multixact.c 对可能存在的事务返空集。设 next multi 为后续值；旧集簇 next=MaxMultiXactId 时可回绕（0）但 multixact.c 可处理。
		 * 
		 * 
		 * 
		 * 
		 * 
		 * 
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


/*恢复前在新集簇调用 (minmxid_only=false)。确保 initdb 建的可 vacuum 表的 relfrozenxid/relminmxid 匹配旧集簇计数器，并初始化内建数据库的 datfrozenxid/datminmxid。用户表创建时字段由 binary-upgrade 恢复脚本正确恢复，用户数据库 datfrozenxid/datminmxid 同理。9.3 前源不存每个表/DB minmxid，恢复填充值为零。故 9.3 前源恢复后第二次调用 (minmxid_only=true)，初始化所有表和数据库的 minmxid。frozenxid 不变。
 *	
 *
 * 
 * 
 * 
 * 
 * 
 *
 * 
 * 
 * 
 * 
 * 
 * 
 *
 * 
 * 
 * 
 * 
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

		/*须更新 datallowconn=false 数据库（如 template0）因 autovacuum 即使关闭也递增其冻结 xid。临时改 datallowconn 以启用。
		 * 
		 * 
		 * 
		 * 
		 * 
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

		/* Reset datallowconn flag */
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(conn_template1,
									  "ALTER DATABASE %s ALLOW_CONNECTIONS = false",
									  quote_identifier(datname)));
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	check_ok();
}
