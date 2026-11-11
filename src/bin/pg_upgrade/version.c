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


/*
 * check_for_data_types_usage()
 *	检测是否存在依赖于给定类型的已存储列。             
 *
 * 如果有，则向给定文件写入报告并返回 true。   
 *
 * base_query 应当是一条 SELECT，产生名为 "oid" 的单      
 * 列，其中包含一个或多个已知在不同服务器版本间     
 * 磁盘表示不一致的类型的 pg_type OID。            
 *
 * 我们会在表、物化视图和索引中检查这些类型，但不检 
 * 查视图；视图不涉及存储。  
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

		/*
		 * 所关注的类型可能被包裹在域、数组、复合或
		 * 范围中，且这些容器类型可嵌套（其程度取决    
		 * 于服务器版本，但无关紧要）。要处理这些情  
		 * 况，我们需要一个递归的 CTE。                          
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

/*
 * check_for_data_type_usage()
 *	检测是否存在依赖于给定类型的已存储列。              
 *
 * 如果有，则向给定文件写入报告并返回 true。   
 *
 * type_name 应当是一个完全限定的类型名。这只是一
 * 个围绕 check_for_data_types_usage() 的简单包装，把    
 * 类型名转成 base_query  
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


/*
 * old_9_3_check_for_line_data_type_usage()
 *	9.3 -> 9.4
 *	在 9.4 中完整实现 'line' 数据类型，此前它默认返回   
 *	"not enabled"，并且只有在启用编译期开关时          
 *	才在功能上启用；9.4 起 "line" 的磁盘表示格式 
 *	不同了。          
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


/*
 * old_9_6_check_for_unknown_data_type_usage()
 *	9.6 -> 10
 *	不再允许创建带有 "unknown" 类型列的表或视图。      
 *	对于带有这类列的视图，我们并不提出抱怨，因为 
 *	它们应当在 DDL 转储和重载期间被静默地转换为     
 *	"text" 列；看起来不值得让用户手动去做这件事。   
 *	然而，如果有一个带这类列的表，那么 DDL 重载将
 *	会失败，因此我们应当预先检测到它，而不是在   
 *	升级中途失败。而若存在带这类列的物化视图，  
 *	那么 DDL 重载将静默地把它改为 "text"，而这将与   
 *	磁盘存储不匹配（类似 "cstring"）。所以*必须*拒绝它
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

/*
 * old_9_6_invalidate_hash_indexes()
 *	9.6 -> 10
 *	哈希索引二进制格式已从 9.6->10.0 改变 
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

/*
 * old_11_check_for_sql_identifier_data_type_usage()
 *	11 -> 12
 *	在 12 中，sql_identifier 数据类型从 name 切换成了         
 *	varchar，这确实会影响存储（name 是引用传递，但并   
 *	非 varlena）。这意味着列使用 sql_identifier 的用户表   
 *	损坏，因磁盘格式不同  
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


/*
 * report_extension_updates()
 *	报告那些应当被更新的扩展。  
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
