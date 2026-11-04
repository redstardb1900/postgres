/*
 *	option.c
 *
 *	options functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/option.c
 */

#include "postgres_fe.h"

#ifdef WIN32
#include <io.h>
#endif

#include "common/string.h"
#include "getopt_long.h"
#include "pg_upgrade.h"
#include "utils/pidfile.h"

static void usage(void);
static void check_required_directory(char **dirpath,
									 const char *envVarName, bool useCwd,
									 const char *cmdLineOption, const char *description,
									 bool missingOk);
#define FIX_DEFAULT_READ_ONLY "-c default_transaction_read_only=false"


UserOpts	user_opts;


/*解析命令行（argc, argv[]）加载结构
 * 
 *
 *	
                                 */
void
parseCommandLine(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"old-datadir", required_argument, NULL, 'd'},
		{"new-datadir", required_argument, NULL, 'D'},
		{"old-bindir", required_argument, NULL, 'b'},
		{"new-bindir", required_argument, NULL, 'B'},
		{"no-sync", no_argument, NULL, 'N'},
		{"old-options", required_argument, NULL, 'o'},
		{"new-options", required_argument, NULL, 'O'},
		{"old-port", required_argument, NULL, 'p'},
		{"new-port", required_argument, NULL, 'P'},

		{"username", required_argument, NULL, 'U'},
		{"check", no_argument, NULL, 'c'},
		{"link", no_argument, NULL, 'k'},
		{"retain", no_argument, NULL, 'r'},
		{"jobs", required_argument, NULL, 'j'},
		{"socketdir", required_argument, NULL, 's'},
		{"verbose", no_argument, NULL, 'v'},
		{"clone", no_argument, NULL, 1},
		{"copy", no_argument, NULL, 2},

		{NULL, 0, NULL, 0}
	};
	int			option;			/* 命令行选项     */
	int			optindex = 0;	/* getopt_long 所用  */
	int			os_user_effective_id;

	user_opts.do_sync = true;
	user_opts.transfer_mode = TRANSFER_MODE_COPY;

	os_info.progname = get_progname(argv[0]);

	/* 处理 libpq 环境变量；在此加载值供 usage() 输出     */
	old_cluster.port = getenv("PGPORTOLD") ? atoi(getenv("PGPORTOLD")) : DEF_PGUPORT;
	new_cluster.port = getenv("PGPORTNEW") ? atoi(getenv("PGPORTNEW")) : DEF_PGUPORT;

	os_user_effective_id = get_user_info(&os_info.user);
	/* 我们只覆盖数据库用户名；OS id 已在上面取得     */
	if (getenv("PGUSER"))
	{
		pg_free(os_info.user);
		/* 须保存该值，getenv() 指针不稳定        */
		os_info.user = pg_strdup(getenv("PGUSER"));
	}

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_upgrade (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/* 允许以 root 运行 help 和 version，故在此测试。    */
	if (os_user_effective_id == 0)
		pg_fatal("%s: cannot be run as root", os_info.progname);

	while ((option = getopt_long(argc, argv, "b:B:cd:D:j:kNo:O:p:P:rs:U:v",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'b':
				old_cluster.bindir = pg_strdup(optarg);
				break;

			case 'B':
				new_cluster.bindir = pg_strdup(optarg);
				break;

			case 'c':
				user_opts.check = true;
				break;

			case 'd':
				old_cluster.pgdata = pg_strdup(optarg);
				break;

			case 'D':
				new_cluster.pgdata = pg_strdup(optarg);
				break;

			case 'j':
				user_opts.jobs = atoi(optarg);
				break;

			case 'k':
				user_opts.transfer_mode = TRANSFER_MODE_LINK;
				break;

			case 'N':
				user_opts.do_sync = false;
				break;

			case 'o':
				/* 追加选项   */
				if (!old_cluster.pgopts)
					old_cluster.pgopts = pg_strdup(optarg);
				else
				{
					char	   *old_pgopts = old_cluster.pgopts;

					old_cluster.pgopts = psprintf("%s %s", old_pgopts, optarg);
					free(old_pgopts);
				}
				break;

			case 'O':
				/* 追加选项   */
				if (!new_cluster.pgopts)
					new_cluster.pgopts = pg_strdup(optarg);
				else
				{
					char	   *new_pgopts = new_cluster.pgopts;

					new_cluster.pgopts = psprintf("%s %s", new_pgopts, optarg);
					free(new_pgopts);
				}
				break;

			case 'p':
				if ((old_cluster.port = atoi(optarg)) <= 0)
					pg_fatal("invalid old port number");
				break;

			case 'P':
				if ((new_cluster.port = atoi(optarg)) <= 0)
					pg_fatal("invalid new port number");
				break;

			case 'r':
				log_opts.retain = true;
				break;

			case 's':
				user_opts.socketdir = pg_strdup(optarg);
				break;

			case 'U':
				pg_free(os_info.user);
				os_info.user = pg_strdup(optarg);
				os_info.user_specified = true;
				break;

			case 'v':
				log_opts.verbose = true;
				break;

			case 1:
				user_opts.transfer_mode = TRANSFER_MODE_CLONE;
				break;

			case 2:
				user_opts.transfer_mode = TRANSFER_MODE_COPY;
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						os_info.progname);
				exit(1);
		}
	}

	if (optind < argc)
		pg_fatal("too many command-line arguments (first is \"%s\")", argv[optind]);

	if (log_opts.verbose)
		pg_log(PG_REPORT, "Running in verbose mode");

	log_opts.isatty = isatty(fileno(stdout));

	/* 关闭只读模式；给 PGOPTIONS 加前缀？    */
	if (getenv("PGOPTIONS"))
	{
		char	   *pgoptions = psprintf("%s %s", FIX_DEFAULT_READ_ONLY,
										 getenv("PGOPTIONS"));

		setenv("PGOPTIONS", pgoptions, 1);
		pfree(pgoptions);
	}
	else
		setenv("PGOPTIONS", FIX_DEFAULT_READ_ONLY, 1);

	/* 若未设置则从环境变量取值   */
	check_required_directory(&old_cluster.bindir, "PGBINOLD", false,
							 "-b", _("old cluster binaries reside"), false);
	check_required_directory(&new_cluster.bindir, "PGBINNEW", false,
							 "-B", _("new cluster binaries reside"), true);
	check_required_directory(&old_cluster.pgdata, "PGDATAOLD", false,
							 "-d", _("old cluster data resides"), false);
	check_required_directory(&new_cluster.pgdata, "PGDATANEW", false,
							 "-D", _("new cluster data resides"), false);
	check_required_directory(&user_opts.socketdir, "PGSOCKETDIR", true,
							 "-s", _("sockets will be created"), false);

#ifdef WIN32

	/*
	 * 在 Windows 上，若 pg_upgrade 在新集簇目录内运行，    
	 * initdb --sync-only 会因 pg_upgrade_utility.log 报 "Permission    
	 * denied" 错误而失败，故在此检查。 
	 */
	{
		char		cwd[MAXPGPATH],
					new_cluster_pgdata[MAXPGPATH];

		strlcpy(new_cluster_pgdata, new_cluster.pgdata, MAXPGPATH);
		canonicalize_path(new_cluster_pgdata);

		if (!getcwd(cwd, MAXPGPATH))
			pg_fatal("could not determine current directory");
		canonicalize_path(cwd);
		if (path_is_prefix_of_path(new_cluster_pgdata, cwd))
			pg_fatal("cannot run pg_upgrade from inside the new cluster data directory on Windows");
	}
#endif
}


static void
usage(void)
{
	printf(_("pg_upgrade upgrades a PostgreSQL cluster to a different major version.\n\n"));
	printf(_("Usage:\n"));
	printf(_("  pg_upgrade [OPTION]...\n\n"));
	printf(_("Options:\n"));
	printf(_("  -b, --old-bindir=BINDIR       old cluster executable directory\n"));
	printf(_("  -B, --new-bindir=BINDIR       new cluster executable directory (default\n"
			 "                                same directory as pg_upgrade)\n"));
	printf(_("  -c, --check                   check clusters only, don't change any data\n"));
	printf(_("  -d, --old-datadir=DATADIR     old cluster data directory\n"));
	printf(_("  -D, --new-datadir=DATADIR     new cluster data directory\n"));
	printf(_("  -j, --jobs=NUM                number of simultaneous processes or threads to use\n"));
	printf(_("  -k, --link                    link instead of copying files to new cluster\n"));
	printf(_("  -N, --no-sync                 do not wait for changes to be written safely to disk\n"));
	printf(_("  -o, --old-options=OPTIONS     old cluster options to pass to the server\n"));
	printf(_("  -O, --new-options=OPTIONS     new cluster options to pass to the server\n"));
	printf(_("  -p, --old-port=PORT           old cluster port number (default %d)\n"), old_cluster.port);
	printf(_("  -P, --new-port=PORT           new cluster port number (default %d)\n"), new_cluster.port);
	printf(_("  -r, --retain                  retain SQL and log files after success\n"));
	printf(_("  -s, --socketdir=DIR           socket directory to use (default current dir.)\n"));
	printf(_("  -U, --username=NAME           cluster superuser (default \"%s\")\n"), os_info.user);
	printf(_("  -v, --verbose                 enable verbose internal logging\n"));
	printf(_("  -V, --version                 display version information, then exit\n"));
	printf(_("  --clone                       clone instead of copying files to new cluster\n"));
	printf(_("  --copy                        copy files to new cluster (default)\n"));
	printf(_("  -?, --help                    show this help, then exit\n"));
	printf(_("\n"
			 "Before running pg_upgrade you must:\n"
			 "  create a new database cluster (using the new version of initdb)\n"
			 "  shutdown the postmaster servicing the old cluster\n"
			 "  shutdown the postmaster servicing the new cluster\n"));
	printf(_("\n"
			 "When you run pg_upgrade, you must provide the following information:\n"
			 "  the data directory for the old cluster  (-d DATADIR)\n"
			 "  the data directory for the new cluster  (-D DATADIR)\n"
			 "  the \"bin\" directory for the old version (-b BINDIR)\n"
			 "  the \"bin\" directory for the new version (-B BINDIR)\n"));
	printf(_("\n"
			 "For example:\n"
			 "  pg_upgrade -d oldCluster/data -D newCluster/data -b oldCluster/bin -B newCluster/bin\n"
			 "or\n"));
#ifndef WIN32
	printf(_("  $ export PGDATAOLD=oldCluster/data\n"
			 "  $ export PGDATANEW=newCluster/data\n"
			 "  $ export PGBINOLD=oldCluster/bin\n"
			 "  $ export PGBINNEW=newCluster/bin\n"
			 "  $ pg_upgrade\n"));
#else
	printf(_("  C:\\> set PGDATAOLD=oldCluster/data\n"
			 "  C:\\> set PGDATANEW=newCluster/data\n"
			 "  C:\\> set PGBINOLD=oldCluster/bin\n"
			 "  C:\\> set PGBINNEW=newCluster/bin\n"
			 "  C:\\> pg_upgrade\n"));
#endif
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


/*
 * check_required_directory()
 *
 * 检查目录选项。     
 *	dirpath		  - 命令行提供的目录名，或 NULL                  
 *	envVarName	  - 若 dirpath 为 NULL，则取该环境变量的名称        
 *	useCwd		  - true 表示可默认用 CWD 
 *	cmdLineOption - 该目录对应的命令行选项         
 *	description   - 该目录选项的描述              
 *	missingOk	  - true 表示 dirpath 与 envVarName 都不存在也没关系 
 *
 * 如果用户未提供所需的目录名，我们用最后两个参数来构 
 * 造一条有意义的错误消息。             
 */
static void
check_required_directory(char **dirpath, const char *envVarName, bool useCwd,
						 const char *cmdLineOption, const char *description,
						 bool missingOk)
{
	if (*dirpath == NULL || strlen(*dirpath) == 0)
	{
		const char *envVar;

		if ((envVar = getenv(envVarName)) && strlen(envVar))
			*dirpath = pg_strdup(envVar);
		else if (useCwd)
		{
			char		cwd[MAXPGPATH];

			if (!getcwd(cwd, MAXPGPATH))
				pg_fatal("could not determine current directory");
			*dirpath = pg_strdup(cwd);
		}
		else if (missingOk)
			return;
		else
			pg_fatal("You must identify the directory where the %s.\n"
					 "Please use the %s command-line option or the %s environment variable.",
					 description, cmdLineOption, envVarName);
	}

	/*
	 * 清理该路径，尤其是去掉尾部路径分隔符，因为我们  
	 * 会向该路径追加内容来构造路径。        
	 */
	canonicalize_path(*dirpath);
}

/*
 * adjust_data_dir
 *
 * 若指定了仅配置目录，则通过查询运行中的服务器来  
 * 找到真实数据目录。检查有限，因为我们找不到      
 * postmaster.pid，无法检查服务器是否在运行。              
 *
 * 进入时，cluster->pgdata 已由命令行或环境变量设置，      
 * cluster->pgconfig 则未设置。我们会用修正值填充两者。  
 *        
 */
void
adjust_data_dir(ClusterInfo *cluster)
{
	char		filename[MAXPGPATH];
	char		cmd[MAXPGPATH],
				cmd_output[MAX_STRING];
	FILE	   *fp,
			   *output;
	int			rc;

	/* 初始假设配置目录与数据目录相同         */
	cluster->pgconfig = pg_strdup(cluster->pgdata);

	/* 若没有 postgresql.conf，则不可能是仅配置目录    */
	snprintf(filename, sizeof(filename), "%s/postgresql.conf", cluster->pgconfig);
	if ((fp = fopen(filename, "r")) == NULL)
		return;
	fclose(fp);

	/* 若 PG_VERSION 存在，则不是仅配置目录    */
	snprintf(filename, sizeof(filename), "%s/PG_VERSION", cluster->pgconfig);
	if ((fp = fopen(filename, "r")) != NULL)
	{
		fclose(fp);
		return;
	}

	/* 必是配置目录，因此要找出真实数据目录。           */

	if (cluster == &old_cluster)
		prep_status("Finding the real data directory for the source cluster");
	else
		prep_status("Finding the real data directory for the target cluster");

	/*
	 * 我们还没有数据目录，因此无法检查 PG 版本，所以  
	 * 这可能失败 --- 仅适用于 PG 9.2+。若此处失败，  
	 * pg_upgrade 反正也会失败，因为将找不到数据文件。    
	 */
	snprintf(cmd, sizeof(cmd), "\"%s/postgres\" -D \"%s\" -C data_directory",
			 cluster->bindir, cluster->pgconfig);
	fflush(NULL);

	if ((output = popen(cmd, "r")) == NULL ||
		fgets(cmd_output, sizeof(cmd_output), output) == NULL)
		pg_fatal("could not get data directory using %s: %s",
				 cmd, strerror(errno));

	rc = pclose(output);
	if (rc != 0)
		pg_fatal("could not get data directory using %s: %s",
				 cmd, wait_result_to_str(rc));

	/* 去除结尾的换行符和回车符       */
	(void) pg_strip_crlf(cmd_output);

	cluster->pgdata = pg_strdup(cmd_output);

	check_ok();
}


/*
 * get_sock_dir
 *
 * 确定该集簇要使用的套接字目录。如果正在进行实时 
 * 检查（仅限旧集簇），我们需要找出 postmaster 正在何处 
 * 监听；否则，我们会把套接字放到当前目录。            
 *           
 */
void
get_sock_dir(ClusterInfo *cluster, bool live_check)
{
#if !defined(WIN32)
	if (!live_check)
		cluster->sockdir = user_opts.socketdir;
	else
	{
		/*
		 * 若正在进行实时检查，我们将使用旧集簇的 Unix  
		 * 域套接字目录，以连接运行中的服务器。       
		 */
		unsigned short orig_port = cluster->port;
		char		filename[MAXPGPATH],
					line[MAXPGPATH];
		FILE	   *fp;
		int			lineno;

		snprintf(filename, sizeof(filename), "%s/postmaster.pid",
				 cluster->pgdata);
		if ((fp = fopen(filename, "r")) == NULL)
			pg_fatal("could not open file \"%s\": %s",
					 filename, strerror(errno));

		for (lineno = 1;
			 lineno <= Max(LOCK_FILE_LINE_PORT, LOCK_FILE_LINE_SOCKET_DIR);
			 lineno++)
		{
			if (fgets(line, sizeof(line), fp) == NULL)
				pg_fatal("could not read line %d from file \"%s\": %s",
						 lineno, filename, strerror(errno));

			/* 可能覆盖用户提供的值            */
			if (lineno == LOCK_FILE_LINE_PORT)
				sscanf(line, "%hu", &old_cluster.port);
			if (lineno == LOCK_FILE_LINE_SOCKET_DIR)
			{
				/* 去除结尾的换行符和回车符       */
				cluster->sockdir = pg_strdup(line);
				(void) pg_strip_crlf(cluster->sockdir);
			}
		}
		fclose(fp);

		/* 提示端口号已做校正    */
		if (orig_port != DEF_PGUPORT && old_cluster.port != orig_port)
			pg_log(PG_WARNING, "user-supplied old port number %hu corrected to %hu",
				   orig_port, cluster->port);
	}
#else							/* WIN32 */
	cluster->sockdir = NULL;
#endif
}
