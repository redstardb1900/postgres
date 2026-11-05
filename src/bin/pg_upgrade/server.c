/*
 *	server.c
 *
 *	database server functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/server.c
 */

#include "postgres_fe.h"

#include "common/connect.h"
#include "fe_utils/string_utils.h"
#include "libpq/pqcomm.h"
#include "pg_upgrade.h"

static PGconn *get_db_conn(ClusterInfo *cluster, const char *db_name);


/*
 * connectToServer()
 *
 *	连接到指定服务器上的目标数据库。          
 *	若连接失败，此函数会记录错误消息并调用   
 *	exit() 终止程序。                       
 */
PGconn *
connectToServer(ClusterInfo *cluster, const char *db_name)
{
	PGconn	   *conn = get_db_conn(cluster, db_name);

	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(PG_REPORT, "%s", PQerrorMessage(conn));

		if (conn)
			PQfinish(conn);

		printf(_("Failure, exiting\n"));
		exit(1);
	}

	PQclear(executeQueryOrDie(conn, ALWAYS_SECURE_SEARCH_PATH_SQL));

	return conn;
}


/*
 * get_db_conn()
 *
 * 使用命名的数据库和集簇的标准参数获取数据库连接      
 *
 * 调用者必须检查连接是否失败  
 */
static PGconn *
get_db_conn(ClusterInfo *cluster, const char *db_name)
{
	PQExpBufferData conn_opts;
	PGconn	   *conn;

	/* 构建带引用的连接串                 */
	initPQExpBuffer(&conn_opts);
	appendPQExpBufferStr(&conn_opts, "dbname=");
	appendConnStrVal(&conn_opts, db_name);
	appendPQExpBufferStr(&conn_opts, " user=");
	appendConnStrVal(&conn_opts, os_info.user);
	appendPQExpBuffer(&conn_opts, " port=%d", cluster->port);
	if (cluster->sockdir)
	{
		appendPQExpBufferStr(&conn_opts, " host=");
		appendConnStrVal(&conn_opts, cluster->sockdir);
	}

	conn = PQconnectdb(conn_opts.data);
	termPQExpBuffer(&conn_opts);
	return conn;
}


/*
 * cluster_conn_opts()
 *
 * 返回在使用 psql、pg_dump 等工具时连接此集簇的标准     
 * 命令行选项。理想情况下它应与 get_db_conn() 所设置的 
 * 相匹配，但我们需要的这些工具对数据库名称选项的     
 * 处理并不一致，故我们将其省略。  
 *
 * 结果在下一次调用本函数前一直有效。  
 */
char *
cluster_conn_opts(ClusterInfo *cluster)
{
	static PQExpBuffer buf;

	if (buf == NULL)
		buf = createPQExpBuffer();
	else
		resetPQExpBuffer(buf);

	if (cluster->sockdir)
	{
		appendPQExpBufferStr(buf, "--host ");
		appendShellString(buf, cluster->sockdir);
		appendPQExpBufferChar(buf, ' ');
	}
	appendPQExpBuffer(buf, "--port %d --username ", cluster->port);
	appendShellString(buf, os_info.user);

	return buf->data;
}


/*
 * executeQueryOrDie()
 *
 *	根据给定参数格式化出查询字符串，然后执行得 
 *	到的查询。若查询失败，此函数会记录一条错误  
 *	消息并调用 exit() 终止程序。       
 */
PGresult *
executeQueryOrDie(PGconn *conn, const char *fmt,...)
{
	static char query[QUERY_ALLOC];
	va_list		args;
	PGresult   *result;
	ExecStatusType status;

	va_start(args, fmt);
	vsnprintf(query, sizeof(query), fmt, args);
	va_end(args);

	pg_log(PG_VERBOSE, "executing: %s", query);
	result = PQexec(conn, query);
	status = PQresultStatus(result);

	if ((status != PGRES_TUPLES_OK) && (status != PGRES_COMMAND_OK))
	{
		pg_log(PG_REPORT, "SQL command failed\n%s\n%s", query,
			   PQerrorMessage(conn));
		PQclear(result);
		PQfinish(conn);
		printf(_("Failure, exiting\n"));
		exit(1);
	}
	else
		return result;
}


/*
 * get_major_server_version()
 *
 * 获取给定 datadir 的版本号（无符号整数形式）。假定 
 * datadir 是一个有效 pgdata 目录的绝对路径。该版本号      
 * 通过读取 PG_VERSION 文件获得。     
 */
uint32
get_major_server_version(ClusterInfo *cluster)
{
	FILE	   *version_fd;
	char		ver_filename[MAXPGPATH];
	int			v1 = 0,
				v2 = 0;

	snprintf(ver_filename, sizeof(ver_filename), "%s/PG_VERSION",
			 cluster->pgdata);
	if ((version_fd = fopen(ver_filename, "r")) == NULL)
		pg_fatal("could not open version file \"%s\": %m", ver_filename);

	if (fscanf(version_fd, "%63s", cluster->major_version_str) == 0 ||
		sscanf(cluster->major_version_str, "%d.%d", &v1, &v2) < 1)
		pg_fatal("could not parse version file \"%s\"", ver_filename);

	fclose(version_fd);

	if (v1 < 10)
	{
		/* 旧格式如 9.6.1    */
		return v1 * 10000 + v2 * 100;
	}
	else
	{
		/* 新格式如 10.1    */
		return v1 * 10000;
	}
}


static void
stop_postmaster_atexit(void)
{
	stop_postmaster(true);
}


bool
start_postmaster(ClusterInfo *cluster, bool report_and_exit_on_error)
{
	char		cmd[MAXPGPATH * 4 + 1000];
	PGconn	   *conn;
	bool		pg_ctl_return = false;
	char		socket_string[MAXPGPATH + 200];

	static bool exit_hook_registered = false;

	if (!exit_hook_registered)
	{
		atexit(stop_postmaster_atexit);
		exit_hook_registered = true;
	}

	socket_string[0] = '\0';

#if !defined(WIN32)
	/* 禁 TCP/IP，限套接字访问                    */
	strcat(socket_string,
		   " -c listen_addresses='' -c unix_socket_permissions=0700");

	/* 有 sockdir？告知 postmaster      */
	if (cluster->sockdir)
		snprintf(socket_string + strlen(socket_string),
				 sizeof(socket_string) - strlen(socket_string),
				 " -c %s='%s'",
				 (GET_MAJOR_VERSION(cluster->major_version) <= 902) ?
				 "unix_socket_directory" : "unix_socket_directories",
				 cluster->sockdir);
#endif

	/*
	 * 用 -b 禁用 autovacuum。  
	 *
	 * 关闭持久性要求以提高对象创建速度，而且我们只修 
	 * 改新集簇，故只在其中使用。如果发生崩溃，新集  
	 * 簇反正必须重建。fsync=off 在 ext4 上是一个巨大的      
	 * 提升。   
	 */
	snprintf(cmd, sizeof(cmd),
			 "\"%s/pg_ctl\" -w -l \"%s/%s\" -D \"%s\" -o \"-p %d -b%s %s%s\" start",
			 cluster->bindir,
			 log_opts.logdir,
			 SERVER_LOG_FILE, cluster->pgconfig, cluster->port,
			 (cluster == &new_cluster) ?
			 " -c synchronous_commit=off -c fsync=off -c full_page_writes=off" : "",
			 cluster->pgopts ? cluster->pgopts : "", socket_string);

	/*
	 * 不要立即抛出错误，而是让连接操作抛出错误，因为  
	 * 它可能会提供失败的原因。     
	 */
	pg_ctl_return = exec_prog(SERVER_START_LOG_FILE,
	/* 如文件名异则双传            */
							  (strcmp(SERVER_LOG_FILE,
									  SERVER_START_LOG_FILE) != 0) ?
							  SERVER_LOG_FILE : NULL,
							  report_and_exit_on_error, false,
							  "%s", cmd);

	/* 在测服务器能否启？                                         */
	if (!pg_ctl_return && !report_and_exit_on_error)
		return false;

	/*
	 * 我们在此设置，以确保 atexit() 会关闭服务器，但仅在
	 * 成功启动了服务器的情况下才如此。我们在检查连  
	 * 接性之前就执行，以防服务器已启动但存在连接性 
	 * 故障。若 pg_ctl 未返回成功，下面就会退出。    
	 *
	 * 9.1 之前的服务器没有 PQping()，因此如果认证配置       
	 * 有误，可能就会让服务器一直运行，所以将来也许   
	 * 会在 pg_ctl 失败时更激进地关闭服务器，但现在   
	 * （2013-08-14）看来谨慎为妙。我们不想关闭一台         
	 * 可能是在升级期间被意外启动的                         
	 * 服务器。       
	 */
	if (pg_ctl_return)
		os_info.running_cluster = cluster;

	/*
	 * pg_ctl -w 失败可能是因为服务器无法启动，也可能是在
	 * _检查_服务器是否已启动时出现了连接问题。因此，  
	 * 即使 pg_ctl 失败了，我们仍会继续并测试连接，以   
	 * 便获得关于失败原因的连接信息。                       
	 */
	if ((conn = get_db_conn(cluster, "template1")) == NULL ||
		PQstatus(conn) != CONNECTION_OK)
	{
		pg_log(PG_REPORT, "\n%s", PQerrorMessage(conn));
		if (conn)
			PQfinish(conn);
		if (cluster == &old_cluster)
			pg_fatal("could not connect to source postmaster started with the command:\n"
					 "%s",
					 cmd);
		else
			pg_fatal("could not connect to target postmaster started with the command:\n"
					 "%s",
					 cmd);
	}
	PQfinish(conn);

	/*
	 * 若 pg_ctl 失败而连接未失败，且启用了   
	 * report_and_exit_on_error，则立即失败。这可能发生        
	 * 在服务器已运行时。    
	 */
	if (!pg_ctl_return)
	{
		if (cluster == &old_cluster)
			pg_fatal("pg_ctl failed to start the source server, or connection failed");
		else
			pg_fatal("pg_ctl failed to start the target server, or connection failed");
	}

	return true;
}


void
stop_postmaster(bool in_atexit)
{
	ClusterInfo *cluster;

	if (os_info.running_cluster == &old_cluster)
		cluster = &old_cluster;
	else if (os_info.running_cluster == &new_cluster)
		cluster = &new_cluster;
	else
		return;					/* 无运行集簇    */

	exec_prog(SERVER_STOP_LOG_FILE, NULL, !in_atexit, !in_atexit,
			  "\"%s/pg_ctl\" -w -D \"%s\" -o \"%s\" %s stop",
			  cluster->bindir, cluster->pgconfig,
			  cluster->pgopts ? cluster->pgopts : "",
			  in_atexit ? "-m fast" : "-m smart");

	os_info.running_cluster = NULL;
}


/*
 * check_pghost_envvar()
 *
 * 测试 PGHOST 是否未指向非本地服务器       
 */
void
check_pghost_envvar(void)
{
	PQconninfoOption *option;
	PQconninfoOption *start;

	/* 从 PQconndefaults 获 libpq 环境变                    */

	start = PQconndefaults();

	if (!start)
		pg_fatal("out of memory");

	for (option = start; option->keyword != NULL; option++)
	{
		if (option->envvar && (strcmp(option->envvar, "PGHOST") == 0 ||
							   strcmp(option->envvar, "PGHOSTADDR") == 0))
		{
			const char *value = getenv(option->envvar);

			if (value && strlen(value) > 0 &&
			/* 检查 'local' 主机值      */
				(strcmp(value, "localhost") != 0 && strcmp(value, "127.0.0.1") != 0 &&
				 strcmp(value, "::1") != 0 && !is_unixsock_path(value)))
				pg_fatal("libpq environment variable %s has a non-local server value: %s",
						 option->envvar, value);
		}
	}

	/* 释放 libpq 分配内存                          */
	PQconninfoFree(start);
}
