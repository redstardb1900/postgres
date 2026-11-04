/*
 *	tablespace.c
 *
 *	tablespace functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/tablespace.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

static void get_tablespace_paths(void);
static void set_tablespace_directory_suffix(ClusterInfo *cluster);


void
init_tablespaces(void)
{
	get_tablespace_paths();

	set_tablespace_directory_suffix(&old_cluster);
	set_tablespace_directory_suffix(&new_cluster);

	if (os_info.num_old_tablespaces > 0 &&
		strcmp(old_cluster.tablespace_suffix, new_cluster.tablespace_suffix) == 0)
		pg_fatal("Cannot upgrade to/from the same system catalog version when\n"
				 "using tablespaces.");
}


/*扫描 pg_tablespace，返回所有表空间路径的 malloc 数组。由调用者负责释放数组。
 * 
 * 
 * 
 * 
                                            */
static void
get_tablespace_paths(void)
{
	PGconn	   *conn = connectToServer(&old_cluster, "template1");
	PGresult   *res;
	int			tblnum;
	int			i_spclocation;
	char		query[QUERY_ALLOC];

	snprintf(query, sizeof(query),
			 "SELECT pg_catalog.pg_tablespace_location(oid) AS spclocation "
			 "FROM	pg_catalog.pg_tablespace "
			 "WHERE	spcname != 'pg_default' AND "
			 "		spcname != 'pg_global'");

	res = executeQueryOrDie(conn, "%s", query);

	if ((os_info.num_old_tablespaces = PQntuples(res)) != 0)
		os_info.old_tablespaces =
			(char **) pg_malloc(os_info.num_old_tablespaces * sizeof(char *));
	else
		os_info.old_tablespaces = NULL;

	i_spclocation = PQfnumber(res, "spclocation");

	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		struct stat statBuf;

		os_info.old_tablespaces[tblnum] = pg_strdup(PQgetvalue(res, tblnum, i_spclocation));

		/*检查表空间路径存在且为目录。实际仅检查不存在表空间目录中的表/索引；位于不存在表空间的数据库已在后端报错。当含用户表空间的数据目录在 pg_upgrade 准备中被移动且符号链接未更新时，会出现不存在的表空间目录。
		 * 
		 * 
		 * 
		 * 
		 * 
		 * 
		 * 
		                                                                                                               */
		if (stat(os_info.old_tablespaces[tblnum], &statBuf) != 0)
		{
			if (errno == ENOENT)
				report_status(PG_FATAL,
							  "tablespace directory \"%s\" does not exist",
							  os_info.old_tablespaces[tblnum]);
			else
				report_status(PG_FATAL,
							  "could not stat tablespace directory \"%s\": %s",
							  os_info.old_tablespaces[tblnum], strerror(errno));
		}
		if (!S_ISDIR(statBuf.st_mode))
			report_status(PG_FATAL,
						  "tablespace path \"%s\" is not a directory",
						  os_info.old_tablespaces[tblnum]);
	}

	PQclear(res);

	PQfinish(conn);
}


static void
set_tablespace_directory_suffix(ClusterInfo *cluster)
{
	/* 该集簇有版本特定的子目录             */

	/* 前置斜杠为开始新目录                        */
	cluster->tablespace_suffix = psprintf("/PG_%s_%d",
										  cluster->major_version_str,
										  cluster->controldata.cat_ver);
}
