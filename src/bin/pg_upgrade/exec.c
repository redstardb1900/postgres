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
			pg_usleep(1000000); /* 1 秒 */
			log = fopen(log_file, "a");
		}
	}
#endif

	if (log == NULL)
		pg_fatal("could not open log file \"%s\": %m", log_file);

#ifdef WIN32
	/* 是否在输出之前打印 "command:"？     */
	if (mainThreadId == GetCurrentThreadId())
		fprintf(log, "\n\n");
#endif
	fprintf(log, "command: %s\n", cmd);
#ifdef WIN32
	/* 是否在输出之后打印 "command:"？    */
	if (mainThreadId != GetCurrentThreadId())
		fprintf(log, "\n\n");
#endif

	/*
	 * 在 Windows 中，此时必须关闭日志文件，                    
	 * 以免命令运行时文件仍打开而造成共享冲突。   
	 */
	fclose(log);

#ifdef WIN32
	/* 见上文注释   */
	if (mainThreadId == GetCurrentThreadId())
#endif
	{
		fflush(NULL);
		result = system(cmd);
	}

	if (result != 0 && report_error)
	{
		/* 可能正处在进度状态行上，因此转到下一行        */
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

	/*
	 * 在 Windows 上不能这样做，因为那会使 "pg_ctl           
	 *  start" 的输出文件名保持打开，直到服务器停止，      
	 * 所以我们在上面做了 \n\n。我们为 "pg_ctl start"        
	 *  使用唯一文件名，服务器运行期间绝不复用，        
	 * 故无碍。也可记入第三文件，但徒增复杂性。      
	 */
	if ((log = fopen(log_file, "a")) == NULL)
		pg_fatal("could not write to log file \"%s\": %m", log_file);
	fprintf(log, "\n\n");
	fclose(log);
#endif

	return result == 0;
}


/*
 * pid_lock_file_exists()
 *
 * 检查 postmaster.pid 文件是否存在。   
 */
bool
pid_lock_file_exists(const char *datadir)
{
	char		path[MAXPGPATH];
	int			fd;

	snprintf(path, sizeof(path), "%s/postmaster.pid", datadir);

	if ((fd = open(path, O_RDONLY, 0)) < 0)
	{
		/* ENOTDIR 意味着稍后抛出更有用的错误       */
		if (errno != ENOENT && errno != ENOTDIR)
			pg_fatal("could not open file \"%s\" for reading: %s",
					 path, strerror(errno));

		return false;
	}

	close(fd);
	return true;
}


/*
 * verify_directories()
 *
 * 完成校验新旧服务器目录与可执行文件的           
 * 全部繁重工作。 
 *
 * 注：可能更新所有参数的值         
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
/*
 * win32_check_directory_write_permissions()
 *
 *	access() 在 WIN32 上无法检查目录权限，所以           
 *	须按需创建再删除文件来检查。     
 *		http://msdn.microsoft.com/en-us/library/1w06ktdy%28v=vs.80%29.aspx
 */
static int
win32_check_directory_write_permissions(void)
{
	int			fd;

	/*
	 * 我们打开一个反正通常都会创建的文件。即使在    
	 * 'check' 模式下也这样做，虽欠佳但已尽力。        
	 */
	if ((fd = open(GLOBALS_DUMP_FILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) < 0)
		return -1;
	close(fd);

	return unlink(GLOBALS_DUMP_FILE);
}
#endif


/*
 * check_single_dir()
 *
 *	检查 PGDATA 中单个目录是否存在，若缺失或           
 * 不可访问则失败。        
 */
static void
check_single_dir(const char *pg_data, const char *subdir)
{
	struct stat statBuf;
	char		subDirName[MAXPGPATH];

	snprintf(subDirName, sizeof(subDirName), "%s%s%s", pg_data,
	/* Win32 无法 stat() 带尾随斜杠的目录。       */
			 *subdir ? "/" : "",
			 subdir);

	if (stat(subDirName, &statBuf) != 0)
		report_status(PG_FATAL, "check for \"%s\" failed: %s",
					  subDirName, strerror(errno));
	else if (!S_ISDIR(statBuf.st_mode))
		report_status(PG_FATAL, "\"%s\" is not a directory",
					  subDirName);
}


/*
 * check_data_dir()
 *
 *	此函数校验给定的集簇目录 - 我们搜索少量            
 *	子目录，这些子目录应存在于有效的 $PGDATA             
 *	目录中。若任一子目录缺失（或无法访问），            
 *	则显示错误消息并 exit()           
 *
 */
static void
check_data_dir(ClusterInfo *cluster)
{
	const char *pg_data = cluster->pgdata;

	/* 获取集簇版本      */
	cluster->major_version = get_major_server_version(cluster);

	check_single_dir(pg_data, "");
	check_single_dir(pg_data, "base");
	check_single_dir(pg_data, "global");
	check_single_dir(pg_data, "pg_multixact");
	check_single_dir(pg_data, "pg_subtrans");
	check_single_dir(pg_data, "pg_tblspc");
	check_single_dir(pg_data, "pg_twophase");

	/* pg_xlog 在 v10 中已更名为 pg_wal    */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 906)
		check_single_dir(pg_data, "pg_xlog");
	else
		check_single_dir(pg_data, "pg_wal");

	/* pg_clog 在 v10 中已更名为 pg_xact    */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 906)
		check_single_dir(pg_data, "pg_clog");
	else
		check_single_dir(pg_data, "pg_xact");
}


/*
 * check_bin_dir()
 *
 *	此函数搜索预期在二进制目录中能找到的           
 *	可执行文件。若发现必需的可执行文件缺失        
 *	（或我们无法访问），则显示错误消息并             
 *	exit().
 *
 *	若 check_versions 为真，则将二进制文件的版本              
 *	与本 pg_upgrade 的版本比对。这用于检查目标               
 *	bindir.
 */
static void
check_bin_dir(ClusterInfo *cluster, bool check_versions)
{
	struct stat statBuf;

	/* 查bin目录 */
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

	/* pg_resetxlog 在版本 10 中已更名为 pg_resetwal      */
	if (GET_MAJOR_VERSION(cluster->bin_version) <= 906)
		check_exec(cluster->bindir, "pg_resetxlog", check_versions);
	else
		check_exec(cluster->bindir, "pg_resetwal", check_versions);

	if (cluster == &new_cluster)
	{
		/*
		 * 仅目标版本需要这些二进制文件。pg_dump 与          
		 * pg_dumpall 用于转储旧集簇，但必须是目标          
		 * 版本。      
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
