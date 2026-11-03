/*
 *	version.c
 *
 *	Postgres-version-specific routines
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/version.c
 */

#include "postgres_fe.h"

#include "catalog/pg_class_d.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"


/*检测是否有存储列依赖给定类型。如有则写报告并返回 true。base_query 产生单列 "oid"，包含跨版本磁盘表示不一致类型的 OID。检查表、物化视图和索引（视图无存储）。
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
check_for_data_types_usage(ClusterInfo *cluster,
						   const char *base_query,
						   const char *output_path)
{
	bool		found = false;
	FILE	   *script = NULL;
	int			dbnum;

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);
		PQExpBufferData querybuf;
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;

		/*类型可能包装在域、数组、复合或范围中并嵌套。用递归 CTE 处理。
		 * 
		 * 
		 * 
		 * 
		                                                                                                                                                                    */
		initPQExpBuffer(&querybuf);
		appendPQExpBuffer(&querybuf,
						  "WITH RECURSIVE oids AS ( "
		/* 从 base_query 类型开始                   */
						  "	%s "
						  "	UNION ALL "
						  "	SELECT * FROM ( "
		/* 内 WITH 因 CTE 仅引用一次                      */
						  "		WITH x AS (SELECT oid FROM oids) "
		/* 所选类型的域                  */
						  "			SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typbasetype = x.oid AND typtype = 'd' "
						  "			UNION ALL "
		/* 所选类型的数组                */
						  "			SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typelem = x.oid AND typtype = 'b' "
						  "			UNION ALL "
		/* 含所选类型的复合                            */
						  "			SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_class c, pg_catalog.pg_attribute a, x "
						  "			WHERE t.typtype = 'c' AND "
						  "				  t.oid = c.reltype AND "
						  "				  c.oid = a.attrelid AND "
						  "				  NOT a.attisdropped AND "
						  "				  a.atttypid = x.oid "
						  "			UNION ALL "
		/* 含所选类型的范围                   */
						  "			SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_range r, x "
						  "			WHERE t.typtype = 'r' AND r.rngtypid = t.oid AND r.rngsubtype = x.oid"
						  "	) foo "
						  ") "
		/* 查找这些类型的存储列               */
						  "SELECT n.nspname, c.relname, a.attname "
						  "FROM	pg_catalog.pg_class c, "
						  "		pg_catalog.pg_namespace n, "
						  "		pg_catalog.pg_attribute a "
						  "WHERE	c.oid = a.attrelid AND "
						  "		NOT a.attisdropped AND "
						  "		a.atttypid IN (SELECT oid FROM oids) AND "
						  "		c.relkind IN ("
						  CppAsString2(RELKIND_RELATION) ", "
						  CppAsString2(RELKIND_MATVIEW) ", "
						  CppAsString2(RELKIND_INDEX) ") AND "
						  "		c.relnamespace = n.oid AND "
		/* 排除孤立临时表                 */
						  "		n.nspname !~ '^pg_temp_' AND "
						  "		n.nspname !~ '^pg_toast_temp_' AND "
		/* 排除系统目录           */
						  "		n.nspname NOT IN ('pg_catalog', 'information_schema')",
						  base_query);

		res = executeQueryOrDie(conn, "%s", querybuf.data);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s", output_path,
						 strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname),
					PQgetvalue(res, rowno, i_attname));
		}

		PQclear(res);

		termPQExpBuffer(&querybuf);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	return found;
}

/*包装函数：将 type_name（完全限定）转为 base_query 查 check_for_data_types_usage。
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
check_for_data_type_usage(ClusterInfo *cluster,
						  const char *type_name,
						  const char *output_path)
{
	bool		found;
	char	   *base_query;

	base_query = psprintf("SELECT '%s'::pg_catalog.regtype AS oid",
						  type_name);

	found = check_for_data_types_usage(cluster, base_query, output_path);

	free(base_query);

	return found;
}


/*9.3->9.4: 完全实现 line 类型，此前仅编译期启用，磁盘格式不同。
 * 
 *	
 *	
 *	
 *	
 *	
                                                                                                                                                                                        */
void
old_9_3_check_for_line_data_type_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for incompatible \"line\" data type");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_line.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.line", output_path))
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains the \"line\" data type in user tables.\n"
				 "This data type changed its internal and input/output format\n"
				 "between your old and new versions so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}


/*9.6->10: 不再允许含 unknown 列的表/视图。视图静默转 text 无需处理；含 unknown 列的表 DDL 重载失败需预检；物化视图更严重，改 text 与磁盘不匹配，必须拒绝。
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
void
old_9_6_check_for_unknown_data_type_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for invalid \"unknown\" user columns");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_unknown.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.unknown", output_path))
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains the \"unknown\" data type in user tables.\n"
				 "This data type is no longer allowed in tables, so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*9.6->10: 哈希索引二进制格式变更
 * 
 *	
 *	
                                                    */
void
old_9_6_invalidate_hash_indexes(ClusterInfo *cluster, bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char	   *output_path = "reindex_hash.sql";

	prep_status("Checking for hash indexes");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* 查找哈希索引*/
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_index i, "
								"		pg_catalog.pg_am a, "
								"		pg_catalog.pg_namespace n "
								"WHERE	i.indexrelid = c.oid AND "
								"		c.relam = a.oid AND "
								"		c.relnamespace = n.oid AND "
								"		a.amname = 'hash'"
			);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %s", output_path,
							 strerror(errno));
				if (!db_used)
				{
					PQExpBufferData connectbuf;

					initPQExpBuffer(&connectbuf);
					appendPsqlMetaConnect(&connectbuf, active_db->db_name);
					fputs(connectbuf.data, script);
					termPQExpBuffer(&connectbuf);
					db_used = true;
				}
				fprintf(script, "REINDEX INDEX %s.%s;\n",
						quote_identifier(PQgetvalue(res, rowno, i_nspname)),
						quote_identifier(PQgetvalue(res, rowno, i_relname)));
			}
		}

		PQclear(res);

		if (!check_mode && db_used)
		{
			/* 标记哈希索引无效     */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE pg_catalog.pg_index i "
									  "SET	indisvalid = false "
									  "FROM	pg_catalog.pg_class c, "
									  "		pg_catalog.pg_am a, "
									  "		pg_catalog.pg_namespace n "
									  "WHERE	i.indexrelid = c.oid AND "
									  "		c.relam = a.oid AND "
									  "		c.relnamespace = n.oid AND "
									  "		a.amname = 'hash'"));
		}

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  After upgrading, you will be given\n"
				   "REINDEX instructions.");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  The file\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will recreate all invalid\n"
				   "indexes; until then, none of these indexes will be used.",
				   output_path);
	}
	else
		check_ok();
}

/*11->12: sql_identifier 从 name 改 varchar，影响存储（name 引用传递非 varlena），磁盘格式不同致用户表损坏。
 * 
 *	
 *	
 *	
 *	
 *	
                                                                                                                                                                    */
void
old_11_check_for_sql_identifier_data_type_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for invalid \"sql_identifier\" user columns");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_sql_identifier.txt");

	if (check_for_data_type_usage(cluster, "information_schema.sql_identifier",
								  output_path))
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains the \"sql_identifier\" data type in user tables.\n"
				 "The on-disk format for this data type has changed, so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}


/*报告需更新的扩展
 * 
 *	
                                            */
void
report_extension_updates(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	char	   *output_path = "update_extensions.sql";

	prep_status("Checking for extension updates");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_name;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* 查找需更新扩展           */
		res = executeQueryOrDie(conn,
								"SELECT name "
								"FROM pg_available_extensions "
								"WHERE installed_version != default_version"
			);

		ntups = PQntuples(res);
		i_name = PQfnumber(res, "name");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s", output_path,
						 strerror(errno));
			if (!db_used)
			{
				PQExpBufferData connectbuf;

				initPQExpBuffer(&connectbuf);
				appendPsqlMetaConnect(&connectbuf, active_db->db_name);
				fputs(connectbuf.data, script);
				termPQExpBuffer(&connectbuf);
				db_used = true;
			}
			fprintf(script, "ALTER EXTENSION %s UPDATE;\n",
					quote_identifier(PQgetvalue(res, rowno, i_name)));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		report_status(PG_REPORT, "notice");
		pg_log(PG_REPORT, "\n"
			   "Your installation contains extensions that should be updated\n"
			   "with the ALTER EXTENSION command.  The file\n"
			   "    %s\n"
			   "when executed by psql by the database superuser will update\n"
			   "these extensions.",
			   output_path);
	}
	else
		check_ok();
}
