/*
 *	function.c
 *
 *	server-side function support
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/function.c
 */

#include "postgres_fe.h"

#include "access/transam.h"
#include "catalog/pg_language_d.h"
#include "pg_upgrade.h"

/*库名指针的 qsort 比较函数。先按名称长度排序，同长者按字母序，再按数据库数组下标。这确保如 hstore_plpython 排在 hstore 和 plpython 之后；否则转换模块可能无法通过 LOAD 测试。（后端本应处理此情况，但目前尚不能；即便将来能，保持可预期的探测顺序仍是好主意。）
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
static int
library_name_compare(const void *p1, const void *p2)
{
	const char *str1 = ((const LibraryInfo *) p1)->name;
	const char *str2 = ((const LibraryInfo *) p2)->name;
	int			slen1 = strlen(str1);
	int			slen2 = strlen(str2);
	int			cmp = strcmp(str1, str2);

	if (slen1 != slen2)
		return slen1 - slen2;
	if (cmp != 0)
		return cmp;
	else
		return ((const LibraryInfo *) p1)->dbnum -
			((const LibraryInfo *) p2)->dbnum;
}


/*获取所有含 C 语言函数的旧库名。稍后检查它们是否都存在于新安装中。
 * 
 * 
 * 
 * 
                                                             */
void
get_loadable_libraries(void)
{
	PGresult  **ress;
	int			totaltups;
	int			dbnum;

	ress = (PGresult **) pg_malloc(old_cluster.dbarr.ndbs * sizeof(PGresult *));
	totaltups = 0;

	/* 每库去重取库名                                       */
	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		/*获取此库中所有含非内建 C 函数的库
		 * 
		                    */
		ress[dbnum] = executeQueryOrDie(conn,
										"SELECT DISTINCT probin "
										"FROM pg_catalog.pg_proc "
										"WHERE prolang = %u AND "
										"probin IS NOT NULL AND "
										"oid >= %u;",
										ClanguageId,
										FirstNormalObjectId);
		totaltups += PQntuples(ress[dbnum]);

		PQfinish(conn);
	}

	os_info.libraries = (LibraryInfo *) pg_malloc(totaltups * sizeof(LibraryInfo));
	totaltups = 0;

	for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		PGresult   *res = ress[dbnum];
		int			ntups;
		int			rowno;

		ntups = PQntuples(res);
		for (rowno = 0; rowno < ntups; rowno++)
		{
			char	   *lib = PQgetvalue(res, rowno, 0);

			os_info.libraries[totaltups].name = pg_strdup(lib);
			os_info.libraries[totaltups].dbnum = dbnum;

			totaltups++;
		}
		PQclear(res);
	}

	pg_free(ress);

	os_info.num_libraries = totaltups;
}


/*检查新集簇是否包含所有必需库。实际逐个尝试 LOAD，从而同时测试兼容性与存在性。
 * 
 * 
 * 
 * 
 * 
                                                                     */
void
check_loadable_libraries(void)
{
	PGconn	   *conn = connectToServer(&new_cluster, "template1");
	int			libnum;
	int			was_load_failure = false;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for presence of required libraries");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir, "loadable_libraries.txt");

	/*现在将库名排序。避免重复探测同一库，并确保以一致顺序探测各库；这在库间存在依赖时对可复现行为很重要。
	 * 
	 * 
	 * 
	 * 
	                                                                                        */
	qsort(os_info.libraries, os_info.num_libraries,
		  sizeof(LibraryInfo), library_name_compare);

	for (libnum = 0; libnum < os_info.num_libraries; libnum++)
	{
		char	   *lib = os_info.libraries[libnum].name;
		int			llen = strlen(lib);
		char		cmd[7 + 2 * MAXPGPATH + 1];
		PGresult   *res;

		/* 库名变了？探测                   */
		if (libnum == 0 || strcmp(lib, os_info.libraries[libnum - 1].name) != 0)
		{
			strcpy(cmd, "LOAD '");
			PQescapeStringConn(conn, cmd + strlen(cmd), lib, llen, NULL);
			strcat(cmd, "'");

			res = PQexec(conn, cmd);

			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				was_load_failure = true;

				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %s",
							 output_path, strerror(errno));
				fprintf(script, _("could not load library \"%s\": %s"),
						lib,
						PQerrorMessage(conn));
			}
			else
				was_load_failure = false;

			PQclear(res);
		}

		if (was_load_failure)
			fprintf(script, _("In database: %s\n"),
					old_cluster.dbarr.dbs[os_info.libraries[libnum].dbnum].db_name);
	}

	PQfinish(conn);

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation references loadable libraries that are missing from the\n"
				 "new installation.  You can add these libraries to the new installation,\n"
				 "or remove the functions using them from the old installation.  A list of\n"
				 "problem libraries is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}
