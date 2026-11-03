/*
 *	exec.c
 *
 *	execution functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/exec.c
 */

#include "postgres_fe.h"

#include <fcntl.h>

#include "common/string.h"
#include "pg_upgrade.h"

static void check_data_dir(ClusterInfo *cluster);
static void check_bin_dir(ClusterInfo *cluster, bool check_versions);
static void get_bin_version(ClusterInfo *cluster);
static void check_exec(const char *dir, const char *program, bool check_version);

#ifdef WIN32
static int	win32_check_directory_write_permissions(void);
#endif


/*获取集簇二进制的主版本。
 * 
 *
 *	
                        */
static void
get_bin_version(ClusterInfo *cluster)
{
	char		cmd[MAXPGPATH],
				cmd_output[MAX_STRING];
	FILE	   *output;
	int			rc;
	int			v1 = 0,
				v2 = 0;

	snprintf(cmd, sizeof(cmd), "\"%s/pg_ctl\" --version", cluster->bindir);
	fflush(NULL);

	if ((output = popen(cmd, "r")) == NULL ||
		fgets(cmd_output, sizeof(cmd_output), output) == NULL)
		pg_fatal("could not get pg_ctl version data using %s: %s",
				 cmd, strerror(errno));

	rc = pclose(output);
	if (rc != 0)
		pg_fatal("could not get pg_ctl version data using %s: %s",
				 cmd, wait_result_to_str(rc));

	if (sscanf(cmd_output, "%*s %*s %d.%d", &v1, &v2) < 1)
		pg_fatal("could not get pg_ctl version output from %s", cmd);

	if (v1 < 10)
	{
		/* 旧格式如 9.6.1    */
		cluster->bin_version = v1 * 10000 + v2 * 100;
	}
	else
	{
		/* 新格式如 10.1    */
		cluster->bin_version = v1 * 10000;
	}
}


/*执行外部程序（stdout/stderr 重定向），格式化命令并记录到日志。成功返回 true。失败可写错误消息，可选退出。须先在 Win 主线程调用。
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
bool
exec_prog(const char *log_filename, const char *opt_log_file,
		  bool report_error, bool exit_on_error, const char *fmt,...)
{
	int			result = 0;
	int			written;
	char		log_file[MAXPGPATH];

#define MAXCMDLEN (2 * MAXPGPATH)
	char		cmd[MAXCMDLEN];
	FILE	   *log;
	va_list		ap;

#ifdef WIN32
	static DWORD mainThreadId = 0;

	/* 假设主线先调                                    */
	if (mainThreadId == 0)
		mainThreadId = GetCurrentThreadId();
#endif

	snprintf(log_file, MAXPGPATH, "%s/%s", log_opts.logdir, log_filename);

	written = 0;
	va_start(ap, fmt);
	written += vsnprintf(cmd + written, MAXCMDLEN - written, fmt, ap);
	va_end(ap);
	if (written >= MAXCMDLEN)
		pg_fatal("command too long");
	written += snprintf(cmd + written, MAXCMDLEN - written,
						" >> \"%s\" 2>&1", log_file);
	if (written >= MAXCMDLEN)
		pg_fatal("command too long");

	pg_log(PG_VERBOSE, "%s", cmd);

#ifdef WIN32

	/*Win: 非主线程创建子进程写同日志文件，系统报使用中错。解决：睡眠 100ms，或子进程后写日志。fflush/systen 两次/预创建均无效。
	 * 
	 * 
	 * 
	 * 
	 * 
	 * 
	 * 
	                                                                                                                                                                                                                                                                      */
	if (mainThreadId != GetCurrentThreadId())
	{
		fflush(NULL);
		result = system(cmd);
	}
#endif

	log = fopen(log_file, "a");

#ifdef WIN32
	{
		/*pg_ctl -w stop 可能因 pid 文件移除而报停，但 start 可能仍在关、stdout/-l 仍开。尝试多次打开日志。
		 * 
		 * 
		 * 
		 * 
		 * 
		                                                                                                                                                                              */
		int			iter;

		for (iter = 0; iter < 4 && log == NULL; iter++)
		{
			pg_usleep(1000000); /* 1 sec */
			log = fopen(log_file, "a");
		}
	}
#endif

	if (log == NULL)
		pg_fatal("could not open log file \"%s\": %m", log_file);

#ifdef WIN32
	/* 在命令输出后打 "command:" ？          */
	if (mainThreadId == GetCurrentThreadId())
		fprintf(log, "\n\n");
#endif
	fprintf(log, "command: %s\n", cmd);
#ifdef WIN32
	/* Are we printing "command:" after its output? */
	if (mainThreadId != GetCurrentThreadId())
		fprintf(log, "\n\n");
#endif

	/*见上注释
	 * 
	 * 
	                                                                                                                           */
	fclose(log);

#ifdef WIN32
	/* see comment above */
	if (mainThreadId == GetCurrentThreadId())
#endif
	{
		fflush(NULL);
		result = system(cmd);
	}

	if (result != 0 && report_error)
	{
		/* we might be in on a progress status line, so go to the next line */
		report_status(PG_REPORT, "\n*failure*");
		fflush(stdout);

		pg_log(PG_VERBOSE, "There were problems executing \"%s\"", cmd);
		if (opt_log_file)
			pg_log(exit_on_error ? PG_FATAL : PG_REPORT,
				   "Consult the last few lines of \"%s\" or \"%s\" for\n"
				   "the probable cause of the failure.",
				   log_file, opt_log_file);
		else
			pg_log(exit_on_error ? PG_FATAL : PG_REPORT,
				   "Consult the last few lines of \"%s\" for\n"
				   "the probable cause of the failure.",
				   log_file);
	}

#ifndef WIN32

	/*检查 postmaster.pid 文件存在。
	 * 
	 * 
	 * 
	 * 
	 * 
	                                                                                                                                                                                                                                                                                                                */
	if ((log = fopen(log_file, "a")) == NULL)
		pg_fatal("could not write to log file \"%s\": %m", log_file);
	fprintf(log, "\n\n");
	fclose(log);
#endif

	return result == 0;
}


/*ENOTDIR 则稍后抛更有用错误
 * 
 *
 * 
                                  */
bool
pid_lock_file_exists(const char *datadir)
{
	char		path[MAXPGPATH];
	int			fd;

	snprintf(path, sizeof(path), "%s/postmaster.pid", datadir);

	if ((fd = open(path, O_RDONLY, 0)) < 0)
	{
		/* ENOTDIR means we will throw a more useful error later */
		if (errno != ENOENT && errno != ENOTDIR)
			pg_fatal("could not open file \"%s\" for reading: %s",
					 path, strerror(errno));

		return false;
	}

	close(fd);
	return true;
}


/*access() 在 WIN32 不能查目录权限，须创建删文件检查。
 * 
 *
 * 
 * 
 *
 * 
                                                                                   */
void
verify_directories(void)
{
#ifndef WIN32
	if (access(".", R_OK | W_OK | X_OK) != 0)
#else
	if (win32_check_directory_write_permissions() != 0)
#endif
		pg_fatal("You must have read and write access in the current directory.");

	check_bin_dir(&old_cluster, false);
	check_data_dir(&old_cluster);
	check_bin_dir(&new_cluster, true);
	check_data_dir(&new_cluster);
}


#ifdef WIN32
/*打开通常创建的文件。即使 check 模式也做，不理想但最可行。
 * 
 *
 *	
 *	
 *	
                                                                                                                                            */
static int
win32_check_directory_write_permissions(void)
{
	int			fd;

	/*检查 PGDATA 单个目录存在性，缺失或不可访则失败。
	 * 
	 * 
	                                                                */
	if ((fd = open(GLOBALS_DUMP_FILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) < 0)
		return -1;
	close(fd);

	return unlink(GLOBALS_DUMP_FILE);
}
#endif


/*Win32 不能 stat() 尾斜杠目录。
 * 
 *
 *	
 * 
                                                                                */
static void
check_single_dir(const char *pg_data, const char *subdir)
{
	struct stat statBuf;
	char		subDirName[MAXPGPATH];

	snprintf(subDirName, sizeof(subDirName), "%s%s%s", pg_data,
	/* Win32 can't stat() a directory with a trailing slash. */
			 *subdir ? "/" : "",
			 subdir);

	if (stat(subDirName, &statBuf) != 0)
		report_status(PG_FATAL, "check for \"%s\" failed: %s",
					  subDirName, strerror(errno));
	else if (!S_ISDIR(statBuf.st_mode))
		report_status(PG_FATAL, "\"%s\" is not a directory",
					  subDirName);
}


/*获取集簇版本
 * 
 *
 *	
 *	
 *	
 *	
 *
                                                                                                                                                                                                                                                           */
static void
check_data_dir(ClusterInfo *cluster)
{
	const char *pg_data = cluster->pgdata;

	/* get the cluster version */
	cluster->major_version = get_major_server_version(cluster);

	check_single_dir(pg_data, "");
	check_single_dir(pg_data, "base");
	check_single_dir(pg_data, "global");
	check_single_dir(pg_data, "pg_multixact");
	check_single_dir(pg_data, "pg_subtrans");
	check_single_dir(pg_data, "pg_tblspc");
	check_single_dir(pg_data, "pg_twophase");

	/* pg_clog 在 v10 更名 pg_xact            */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 906)
		check_single_dir(pg_data, "pg_xlog");
	else
		check_single_dir(pg_data, "pg_wal");

	/* pg_clog has been renamed to pg_xact in v10 */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 906)
		check_single_dir(pg_data, "pg_clog");
	else
		check_single_dir(pg_data, "pg_xact");
}


/*检查 bindir
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
check_bin_dir(ClusterInfo *cluster, bool check_versions)
{
	struct stat statBuf;

	/* check bindir */
	if (stat(cluster->bindir, &statBuf) != 0)
		report_status(PG_FATAL, "check for \"%s\" failed: %s",
					  cluster->bindir, strerror(errno));
	else if (!S_ISDIR(statBuf.st_mode))
		report_status(PG_FATAL, "\"%s\" is not a directory",
					  cluster->bindir);

	check_exec(cluster->bindir, "postgres", check_versions);
	check_exec(cluster->bindir, "pg_controldata", check_versions);
	check_exec(cluster->bindir, "pg_ctl", check_versions);

	/*pg_resetxlog 在 v10 更名为 pg_resetwal
	 * 
	 * 
	 * 
	                                                                                                                             */
	get_bin_version(cluster);

	/* pg_resetxlog has been renamed to pg_resetwal in version 10 */
	if (GET_MAJOR_VERSION(cluster->bin_version) <= 906)
		check_exec(cluster->bindir, "pg_resetxlog", check_versions);
	else
		check_exec(cluster->bindir, "pg_resetwal", check_versions);

	if (cluster == &new_cluster)
	{
		/*
		 * These binaries are only needed for the target version. pg_dump and
		 * pg_dumpall are used to dump the old cluster, but must be of the
		 * target version.
		 */
		check_exec(cluster->bindir, "initdb", check_versions);
		check_exec(cluster->bindir, "pg_dump", check_versions);
		check_exec(cluster->bindir, "pg_dumpall", check_versions);
		check_exec(cluster->bindir, "pg_restore", check_versions);
		check_exec(cluster->bindir, "psql", check_versions);
		check_exec(cluster->bindir, "vacuumdb", check_versions);
	}
}

static void
check_exec(const char *dir, const char *program, bool check_version)
{
	char		path[MAXPGPATH];
	char		line[MAXPGPATH];
	char		cmd[MAXPGPATH];
	char		versionstr[128];

	snprintf(path, sizeof(path), "%s/%s", dir, program);

	if (validate_exec(path) != 0)
		pg_fatal("check for \"%s\" failed: %m", path);

	snprintf(cmd, sizeof(cmd), "\"%s\" -V", path);

	if (!pipe_read_line(cmd, line, sizeof(line)))
		pg_fatal("check for \"%s\" failed: cannot execute",
				 path);

	if (check_version)
	{
		pg_strip_crlf(line);

		snprintf(versionstr, sizeof(versionstr), "%s (PostgreSQL) " PG_VERSION, program);

		if (strcmp(line, versionstr) != 0)
			pg_fatal("check for \"%s\" failed: incorrect version: found \"%s\", expected \"%s\"",
					 path, line, versionstr);
	}
}
