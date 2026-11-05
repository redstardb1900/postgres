/*
 *	check.c
 *
 *	server checks and output routines
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/check.c
 */

#include "postgres_fe.h"

#include "catalog/pg_authid_d.h"
#include "catalog/pg_collation.h"
#include "fe_utils/string_utils.h"
#include "mb/pg_wchar.h"
#include "pg_upgrade.h"

static void check_new_cluster_is_empty(void);
static void check_is_install_user(ClusterInfo *cluster);
static void check_proper_datallowconn(ClusterInfo *cluster);
static void check_for_prepared_transactions(ClusterInfo *cluster);
static void check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster);
static void check_for_user_defined_postfix_ops(ClusterInfo *cluster);
static void check_for_incompatible_polymorphics(ClusterInfo *cluster);
static void check_for_tables_with_oids(ClusterInfo *cluster);
static void check_for_not_null_inheritance(ClusterInfo *cluster);
static void check_for_composite_data_type_usage(ClusterInfo *cluster);
static void check_for_reg_data_type_usage(ClusterInfo *cluster);
static void check_for_aclitem_data_type_usage(ClusterInfo *cluster);
static void check_for_removed_data_type_usage(ClusterInfo *cluster,
											  const char *version,
											  const char *datatype);
static void check_for_jsonb_9_4_usage(ClusterInfo *cluster);
static void check_for_pg_role_prefix(ClusterInfo *cluster);
static void check_for_new_tablespace_dir(ClusterInfo *new_cluster);
static void check_for_user_defined_encoding_conversions(ClusterInfo *cluster);


/*
 * fix_path_separator
 * 非 Windows 下直接返回参数。      
 * Windows 下则把正斜杠转换为反斜杠，     
 * 使之适合于 RMDIR 和 DEL 等                    
 * 内建命令。    
 */
static char *
fix_path_separator(char *path)
{
#ifdef WIN32

	char	   *result;
	char	   *c;

	result = pg_strdup(path);

	for (c = result; *c != '\0'; c++)
		if (*c == '/')
			*c = '\\';

	return result;
#else

	return path;
#endif
}

void
output_check_banner(bool live_check)
{
	if (user_opts.check && live_check)
	{
		pg_log(PG_REPORT,
			   "Performing Consistency Checks on Old Live Server\n"
			   "------------------------------------------------");
	}
	else
	{
		pg_log(PG_REPORT,
			   "Performing Consistency Checks\n"
			   "-----------------------------");
	}
}


void
check_and_dump_old_cluster(bool live_check)
{
	/* -- 旧 -- */

	if (!live_check)
		start_postmaster(&old_cluster, true);

	/* 从旧集簇取 DB 和表列                                */
	get_db_and_rel_infos(&old_cluster);

	init_tablespaces();

	get_loadable_libraries();


	/*检查各故障情况
	 * 
	           */
	check_is_install_user(&old_cluster);
	check_proper_datallowconn(&old_cluster);
	check_for_prepared_transactions(&old_cluster);
	check_for_composite_data_type_usage(&old_cluster);
	check_for_reg_data_type_usage(&old_cluster);
	check_for_isn_and_int8_passing_mismatch(&old_cluster);

	/*
	 * PG 16 增大了 'aclitem' 类型的尺寸，这会破坏现有  
	 * 数据的磁盘格式。         
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1500)
		check_for_aclitem_data_type_usage(&old_cluster);

	/*PG 12 除类型 abstime、reltime、tinterval。
	 * 
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
	{
		check_for_removed_data_type_usage(&old_cluster, "12", "abstime");
		check_for_removed_data_type_usage(&old_cluster, "12", "reltime");
		check_for_removed_data_type_usage(&old_cluster, "12", "tinterval");
	}

	/*
	 * PG 14 更改了编码转换函数的函数签名。                   
	 * 旧版转换无法自动升级，因为编码转换所用的用 
	 * 户定义函数需要被修改，以使之匹配                   
	 * 新的签名。                               
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_user_defined_encoding_conversions(&old_cluster);

	/*
	 * PG 14 之前允许用户定义后缀运算符，现已不再   
	 * 支持。若适用，请验证不存在此类运算符。 
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_user_defined_postfix_ops(&old_cluster);

	/*
	 * PG 14 将多态函数从 anyarray 更改为         
	 * anycompatiblearray 
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_incompatible_polymorphics(&old_cluster);

	/*
	 * PG 12 之前允许表被声明为 WITH OIDS，这已不再被  
	 * 支持。若适用，请验证不存在此类表。      
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
		check_for_tables_with_oids(&old_cluster);

	/*
	 * PG 18 之前允许子表省略其父表列所具有的非空约束， 
	 * 但这类子表的模式恢复会失败，请验证不                
	 * 存在。
	 */
	check_for_not_null_inheritance(&old_cluster);

	/*
	 * PG 12 将 'sql_identifier' 类型的存储改为基于 name         
	 * 而非 varchar，这会破坏现有数据的磁盘格式。因此     
	 * 当它用于用户对象（表、索引等）时须阻止升级     
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
		old_11_check_for_sql_identifier_data_type_usage(&old_cluster);

	/*
	 * PG 10 之前允许含 'unknown' 类型列的表和非 WAL 记录的    
	 * 哈希索引
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
	{
		old_9_6_check_for_unknown_data_type_usage(&old_cluster);
		if (user_opts.check)
			old_9_6_invalidate_hash_indexes(&old_cluster, true);
	}

	/* 9.5 及下不应 pg_ 角色                           */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 905)
		check_for_pg_role_prefix(&old_cluster);

	if (GET_MAJOR_VERSION(old_cluster.major_version) == 904 &&
		old_cluster.controldata.cat_ver < JSONB_FORMAT_CHANGE_CAT_VER)
		check_for_jsonb_9_4_usage(&old_cluster);

	/* PG 9.4 前 line 内部格异                                */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 903)
		old_9_3_check_for_line_data_type_usage(&old_cluster);

	/*
	 * 虽非检查选项，但现在执行它，因为这是运行旧服    
	 * 务器的唯一时机。  
	 */
	if (!user_opts.check)
		generate_old_dump();

	if (!live_check)
		stop_postmaster(false);
}


void
check_new_cluster(void)
{
	get_db_and_rel_infos(&new_cluster);

	check_new_cluster_is_empty();

	check_loadable_libraries();

	switch (user_opts.transfer_mode)
	{
		case TRANSFER_MODE_CLONE:
			check_file_clone();
			break;
		case TRANSFER_MODE_COPY:
			break;
		case TRANSFER_MODE_LINK:
			check_hard_link();
			break;
	}

	check_is_install_user(&new_cluster);

	check_for_prepared_transactions(&new_cluster);

	check_for_new_tablespace_dir(&new_cluster);
}


void
report_clusters_compatible(void)
{
	if (user_opts.check)
	{
		pg_log(PG_REPORT, "\n*Clusters are compatible*");
		/* 停止新集簇   */
		stop_postmaster(false);

		cleanup_output_dirs();
		exit(0);
	}

	pg_log(PG_REPORT, "\n"
		   "If pg_upgrade fails after this point, you must re-initdb the\n"
		   "new cluster before continuing.");
}


void
issue_warnings_and_set_wal_level(void)
{
	/*
	 * 我们无条件地启动/停止新服务器，因为 pg_resetwal -o    
	 * 会将 wal_level 设为 'minimum'。若用户正按 rsync 说明       
	 * 升级备库，则需要 pg_upgrade 写入其最终 WAL 记录，   
	 * 以显示 wal_level 为 'replica'。      
	 */
	start_postmaster(&new_cluster, true);

	/* 为旧 < 10.0 重建哈希索引    */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
		old_9_6_invalidate_hash_indexes(&new_cluster, false);

	report_extension_updates(&new_cluster);

	stop_postmaster(false);
}


void
output_completion_banner(char *deletion_script_file_name)
{
	PQExpBufferData user_specification;

	initPQExpBuffer(&user_specification);
	if (os_info.user_specified)
	{
		appendPQExpBufferStr(&user_specification, "-U ");
		appendShellString(&user_specification, os_info.user);
		appendPQExpBufferChar(&user_specification, ' ');
	}

	pg_log(PG_REPORT,
		   "Optimizer statistics are not transferred by pg_upgrade.\n"
		   "Once you start the new server, consider running:\n"
		   "    %s/vacuumdb %s--all --analyze-in-stages", new_cluster.bindir, user_specification.data);

	if (deletion_script_file_name)
		pg_log(PG_REPORT,
			   "Running this script will delete the old cluster's data files:\n"
			   "    %s",
			   deletion_script_file_name);
	else
		pg_log(PG_REPORT,
			   "Could not create a script to delete the old cluster's data files\n"
			   "because user-defined tablespaces or the new cluster's data directory\n"
			   "exist in the old cluster directory.  The old cluster's contents must\n"
			   "be deleted manually.");

	termPQExpBuffer(&user_specification);
}


void
check_cluster_versions(void)
{
	prep_status("Checking cluster versions");

	/* 集簇版本此时应已获取                     */
	Assert(old_cluster.major_version != 0);
	Assert(new_cluster.major_version != 0);

	/*
	 * 对于 alpha/beta 升级，我们允许相同主版本间的   
	 * 升级  
	 */

	if (GET_MAJOR_VERSION(old_cluster.major_version) < 902)
		pg_fatal("This utility can only upgrade from PostgreSQL version %s and later.",
				 "9.2");

	/* 仅支持当前 PG 版本作为目标            */
	if (GET_MAJOR_VERSION(new_cluster.major_version) != GET_MAJOR_VERSION(PG_VERSION_NUM))
		pg_fatal("This utility can only upgrade to PostgreSQL version %s.",
				 PG_MAJORVERSION);

	/*
	 * 我们不能允许降级，因为使用的是目标 pg_dump，而
	 * pg_dump 无法操作更新的数据库版本，仅当前           
	 * 及旧版本。
	 */
	if (old_cluster.major_version > new_cluster.major_version)
		pg_fatal("This utility cannot be used to downgrade to older major PostgreSQL versions.");

	/* 确保二进制文件与指定数据目录相匹配   */
	if (GET_MAJOR_VERSION(old_cluster.major_version) !=
		GET_MAJOR_VERSION(old_cluster.bin_version))
		pg_fatal("Old cluster data and binary directories are from different major versions.");
	if (GET_MAJOR_VERSION(new_cluster.major_version) !=
		GET_MAJOR_VERSION(new_cluster.bin_version))
		pg_fatal("New cluster data and binary directories are from different major versions.");

	check_ok();
}


void
check_cluster_compatibility(bool live_check)
{
	/* 取/查服务器 pg_control 数据   */
	get_control_data(&old_cluster, live_check);
	get_control_data(&new_cluster, false);
	check_control_data(&old_cluster.controldata, &new_cluster.controldata);

	if (live_check && old_cluster.port == new_cluster.port)
		pg_fatal("When checking a live server, "
				 "the old and new port numbers must be different.");
}


static void
check_new_cluster_is_empty(void)
{
	int			dbnum;

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		int			relnum;
		RelInfoArr *rel_arr = &new_cluster.dbarr.dbs[dbnum].rel_arr;

		for (relnum = 0; relnum < rel_arr->nrels;
			 relnum++)
		{
			/* 应跳过 pg_largeobject 及其索引          */
			if (strcmp(rel_arr->rels[relnum].nspname, "pg_catalog") != 0)
				pg_fatal("New cluster database \"%s\" is not empty: found relation \"%s.%s\"",
						 new_cluster.dbarr.dbs[dbnum].db_name,
						 rel_arr->rels[relnum].nspname,
						 rel_arr->rels[relnum].relname);
		}
	}
}

/*
 * 上次运行 pg_upgrade 可能失败，且新集簇目录被      
 * 重建，但可能忘记删除新集簇的表空间目录。
 * 因此，检查确认新集簇的表空间目录尚不存在。 
 * 若已存在，则在恢复全局对象时会出错。      
 * 这样可在检查阶段而非                                  
 * 模式恢复阶段发现该失败。                             
 *                       
 */
static void
check_for_new_tablespace_dir(ClusterInfo *new_cluster)
{
	int			tblnum;
	char		new_tablespace_dir[MAXPGPATH];

	prep_status("Checking for new cluster tablespace directories");

	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		struct stat statbuf;

		snprintf(new_tablespace_dir, MAXPGPATH, "%s%s",
				 os_info.old_tablespaces[tblnum],
				 new_cluster->tablespace_suffix);

		if (stat(new_tablespace_dir, &statbuf) == 0 || errno != ENOENT)
			pg_fatal("new cluster tablespace directory already exists: \"%s\"",
					 new_tablespace_dir);
	}

	check_ok();
}

/*
 * create_script_for_old_cluster_deletion()
 *
 *	这对于删除表空间尤其有用。             
 */
void
create_script_for_old_cluster_deletion(char **deletion_script_file_name)
{
	FILE	   *script = NULL;
	int			tblnum;
	char		old_cluster_pgdata[MAXPGPATH],
				new_cluster_pgdata[MAXPGPATH];

	*deletion_script_file_name = psprintf("%sdelete_old_cluster.%s",
										  SCRIPT_PREFIX, SCRIPT_EXT);

	strlcpy(old_cluster_pgdata, old_cluster.pgdata, MAXPGPATH);
	canonicalize_path(old_cluster_pgdata);

	strlcpy(new_cluster_pgdata, new_cluster.pgdata, MAXPGPATH);
	canonicalize_path(new_cluster_pgdata);

	/* 有些人会把新数据目录放在旧目录之内。     */
	if (path_is_prefix_of_path(old_cluster_pgdata, new_cluster_pgdata))
	{
		pg_log(PG_WARNING,
			   "\nWARNING:  new data directory should not be inside the old data directory, i.e. %s", old_cluster_pgdata);

		/* 删除上次运行可能遗留的文件                  */
		unlink(*deletion_script_file_name);
		pg_free(*deletion_script_file_name);
		*deletion_script_file_name = NULL;
		return;
	}

	/*
	 * 有些用户（奇怪地）会在集簇数据目录内创    
	 * 建表空间。此时无法生成正常的旧集簇删除脚本。    
	 *      
	 */
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		char		old_tablespace_dir[MAXPGPATH];

		strlcpy(old_tablespace_dir, os_info.old_tablespaces[tblnum], MAXPGPATH);
		canonicalize_path(old_tablespace_dir);
		if (path_is_prefix_of_path(old_cluster_pgdata, old_tablespace_dir))
		{
			/* 复现日志里 CREATE TABLESPACE 产生的警告           */
			pg_log(PG_WARNING,
				   "\nWARNING:  user-defined tablespace locations should not be inside the data directory, i.e. %s", old_tablespace_dir);

			/* 删除上次运行可能遗留的文件                  */
			unlink(*deletion_script_file_name);
			pg_free(*deletion_script_file_name);
			*deletion_script_file_name = NULL;
			return;
		}
	}

	prep_status("Creating script to delete old cluster");

	if ((script = fopen_priv(*deletion_script_file_name, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %s",
				 *deletion_script_file_name, strerror(errno));

#ifndef WIN32
	/* 加 shebang 头    */
	fprintf(script, "#!/bin/sh\n\n");
#endif

	/* 删除旧集簇的默认表空间       */
	fprintf(script, RMDIR_CMD " %c%s%c\n", PATH_QUOTE,
			fix_path_separator(old_cluster.pgdata), PATH_QUOTE);

	/* 删除旧集簇的备用表空间          */
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		/*
		 * 旧集簇的每数据库目录是否与新版本专属         
		 * 表空间共用同一目录？         
		 */
		if (strlen(old_cluster.tablespace_suffix) == 0)
		{
			/* 删除每数据库的目录     */
			int			dbnum;

			fprintf(script, "\n");

			for (dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
				fprintf(script, RMDIR_CMD " %c%s%c%u%c\n", PATH_QUOTE,
						fix_path_separator(os_info.old_tablespaces[tblnum]),
						PATH_SEPARATOR, old_cluster.dbarr.dbs[dbnum].db_oid,
						PATH_QUOTE);
		}
		else
		{
			char	   *suffix_path = pg_strdup(old_cluster.tablespace_suffix);

			/*
			 * 直接删除表空间目录，它可能是 ".old"            
			 * 或版本专属子目录。        
			 */
			fprintf(script, RMDIR_CMD " %c%s%s%c\n", PATH_QUOTE,
					fix_path_separator(os_info.old_tablespaces[tblnum]),
					fix_path_separator(suffix_path), PATH_QUOTE);
			pfree(suffix_path);
		}
	}

	fclose(script);

#ifndef WIN32
	if (chmod(*deletion_script_file_name, S_IRWXU) != 0)
		pg_fatal("could not add execute permission to file \"%s\": %s",
				 *deletion_script_file_name, strerror(errno));
#endif

	check_ok();
}


/*
 *	check_is_install_user()
 *
 *	检查我们是否为安装用户，以及新集簇    
 *	无其他用户。 
 */
static void
check_is_install_user(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking database user is the install user");

	/* 不能用 pg_authid，因为仅超级用户可查看它  */
	res = executeQueryOrDie(conn,
							"SELECT rolsuper, oid "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname = current_user "
							"AND rolname !~ '^pg_'");

	/*
	 * 新集簇中仅允许安装用户（见下方注释），且保留   
	 *  pg_authid.oid，因此旧集簇中的该用户也必须是           
	 * 安装用户。 
	 */
	if (PQntuples(res) != 1 ||
		atooid(PQgetvalue(res, 0, 1)) != BOOTSTRAP_SUPERUSERID)
		pg_fatal("database user \"%s\" is not the install user",
				 os_info.user);

	PQclear(res);

	res = executeQueryOrDie(conn,
							"SELECT COUNT(*) "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname !~ '^pg_'");

	if (PQntuples(res) != 1)
		pg_fatal("could not determine the number of users");

	/*
	 * 只允许新集簇使用安装用户，因为其他已定义的        
	 * 用户可能与旧集簇中定义的用户匹配，并在         
	 * pg_dump 恢复期间出错。
	 */
	if (cluster == &new_cluster && strcmp(PQgetvalue(res, 0, 0), "1") != 0)
		pg_fatal("Only the install user can be defined in the new cluster.");

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


/*
 *	check_proper_datallowconn
 *
 *	确保所有非 template0 数据库都允许连接，               
 *	否则它们不会被恢复；并确保 template0 明确                
 *	不允许连接，否则 pg_dumpall --globals 恢复将失败。  
 */
static void
check_proper_datallowconn(ClusterInfo *cluster)
{
	int			dbnum;
	PGconn	   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking database connection settings");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "databases_with_datallowconn_false.txt");

	conn_template1 = connectToServer(cluster, "template1");

	/* 获取数据库名 */
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

		if (strcmp(datname, "template0") == 0)
		{
			/* 避免 pg_dumpall 尝试创建 template0 时恢复失败        */
			if (strcmp(datallowconn, "t") == 0)
				pg_fatal("template0 must not allow connections, "
						 "i.e. its pg_database.datallowconn must be false");
		}
		else
		{
			/*
			 * 避免 datallowconn == false 的库在恢复时被         
			 * 跳过 
			 */
			if (strcmp(datallowconn, "f") == 0)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %s",
							 output_path, strerror(errno));

				fprintf(script, "%s\n", datname);
			}
		}
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("All non-template0 databases must allow connections, i.e. their\n"
				 "pg_database.datallowconn must be true.  Your installation contains\n"
				 "non-template0 databases with their pg_database.datallowconn set to\n"
				 "false.  Consider allowing connection for all non-template0 databases\n"
				 "or drop the databases which do not allow connections.  A list of\n"
				 "databases with the problem is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}


/*
 *	check_for_prepared_transactions()
 *
 *	确保没有预备事务，因为存储格式                          
 *	可能已改变。 
 */
static void
check_for_prepared_transactions(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking for prepared transactions");

	res = executeQueryOrDie(conn,
							"SELECT * "
							"FROM pg_catalog.pg_prepared_xacts");

	if (PQntuples(res) != 0)
	{
		if (cluster == &old_cluster)
			pg_fatal("The source cluster contains prepared transactions");
		else
			pg_fatal("The target cluster contains prepared transactions");
	}

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


/*
 *	check_for_isn_and_int8_passing_mismatch()
 *
 *	contrib/isn 依赖 int8 数据类型，而在 8.4 中                  
 *	int8 现在可以按值传递。模式会转储 CREATE TYPE         
 *	PASSEDBYVALUE，故新旧必须匹配     
 */
static void
check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for contrib/isn with bigint-passing mismatch");

	if (old_cluster.controldata.float8_pass_by_value ==
		new_cluster.controldata.float8_pass_by_value)
	{
		/* 无差异   */
		check_ok();
		return;
	}

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "contrib_isn_and_int8_pass_by_value.txt");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_proname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* 查找来自 contrib/isn 的函数         */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, p.proname "
								"FROM	pg_catalog.pg_proc p, "
								"		pg_catalog.pg_namespace n "
								"WHERE	p.pronamespace = n.oid AND "
								"		p.probin = '$libdir/isn'");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_proname = PQfnumber(res, "proname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_proname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains \"contrib/isn\" functions which rely on the\n"
				 "bigint data type.  Your old and new clusters pass bigint values\n"
				 "differently so this cluster cannot currently be upgraded.  You can\n"
				 "manually dump databases in the old cluster that use \"contrib/isn\"\n"
				 "facilities, drop them, perform the upgrade, and then restore them.  A\n"
				 "list of the problem functions is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * 验证不存在用户定义的后缀运算符。    
 */
static void
check_for_user_defined_postfix_ops(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for user-defined postfix operators");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "postfix_ops.txt");

	/* 查找用户定义的后缀运算符    */
	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_oproid,
					i_oprnsp,
					i_oprname,
					i_typnsp,
					i_typname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/*
		 * 下面的查询将 FirstNormalObjectId 硬编码为               
		 *  16384，而非把该 C #define 插入查询，因为       
		 * 若该 #define 被更改，我们要用的截止值是版      
		 * 本 14 之前服务器的值，而非未来版本的值。      
		 */
		res = executeQueryOrDie(conn,
								"SELECT o.oid AS oproid, "
								"       n.nspname AS oprnsp, "
								"       o.oprname, "
								"       tn.nspname AS typnsp, "
								"       t.typname "
								"FROM pg_catalog.pg_operator o, "
								"     pg_catalog.pg_namespace n, "
								"     pg_catalog.pg_type t, "
								"     pg_catalog.pg_namespace tn "
								"WHERE o.oprnamespace = n.oid AND "
								"      o.oprleft = t.oid AND "
								"      t.typnamespace = tn.oid AND "
								"      o.oprright = 0 AND "
								"      o.oid >= 16384");
		ntups = PQntuples(res);
		i_oproid = PQfnumber(res, "oproid");
		i_oprnsp = PQfnumber(res, "oprnsp");
		i_oprname = PQfnumber(res, "oprname");
		i_typnsp = PQfnumber(res, "typnsp");
		i_typname = PQfnumber(res, "typname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			if (script == NULL &&
				(script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  (oid=%s) %s.%s (%s.%s, NONE)\n",
					PQgetvalue(res, rowno, i_oproid),
					PQgetvalue(res, rowno, i_oprnsp),
					PQgetvalue(res, rowno, i_oprname),
					PQgetvalue(res, rowno, i_typnsp),
					PQgetvalue(res, rowno, i_typname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains user-defined postfix operators, which are not\n"
				 "supported anymore.  Consider dropping the postfix operators and replacing\n"
				 "them with prefix operators or function calls.\n"
				 "A list of user-defined postfix operators is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 *	check_for_incompatible_polymorphics()
 *
 *	确保没有任何对象在使用带 anyarray/anyelement 
 *	的旧多态函数，而非新的 anycompatible 变体。      
 */
static void
check_for_incompatible_polymorphics(ClusterInfo *cluster)
{
	PGresult   *res;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];
	PQExpBufferData old_polymorphics;

	prep_status("Checking for incompatible polymorphic functions");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "incompatible_polymorphics.txt");

	/* 有问题的函数集合在不同版本间略有差异              */
	initPQExpBuffer(&old_polymorphics);

	appendPQExpBufferStr(&old_polymorphics,
						 "'array_append(anyarray,anyelement)'"
						 ", 'array_cat(anyarray,anyarray)'"
						 ", 'array_prepend(anyelement,anyarray)'");

	if (GET_MAJOR_VERSION(cluster->major_version) >= 903)
		appendPQExpBufferStr(&old_polymorphics,
							 ", 'array_remove(anyarray,anyelement)'"
							 ", 'array_replace(anyarray,anyelement,anyelement)'");

	if (GET_MAJOR_VERSION(cluster->major_version) >= 905)
		appendPQExpBufferStr(&old_polymorphics,
							 ", 'array_position(anyarray,anyelement)'"
							 ", 'array_position(anyarray,anyelement,integer)'"
							 ", 'array_positions(anyarray,anyelement)'"
							 ", 'width_bucket(anyelement,anyarray)'");

	for (int dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		bool		db_used = false;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);
		int			ntups;
		int			i_objkind,
					i_objname;

		/*
		 * 下面的查询将 FirstNormalObjectId 硬编码为               
		 *  16384，而非把该 C #define 插入查询，因为       
		 * 若该 #define 被更改，我们要用的截止值是版      
		 * 本 14 之前服务器的值，而非未来版本的值。      
		 */
		res = executeQueryOrDie(conn,
		/* 聚合转换函数             */
								"SELECT 'aggregate' AS objkind, p.oid::regprocedure::text AS objname "
								"FROM pg_proc AS p "
								"JOIN pg_aggregate AS a ON a.aggfnoid=p.oid "
								"JOIN pg_proc AS transfn ON transfn.oid=a.aggtransfn "
								"WHERE p.oid >= 16384 "
								"AND a.aggtransfn = ANY(ARRAY[%s]::regprocedure[]) "
								"AND a.aggtranstype = ANY(ARRAY['anyarray', 'anyelement']::regtype[]) "

		/* 聚合最终函数        */
								"UNION ALL "
								"SELECT 'aggregate' AS objkind, p.oid::regprocedure::text AS objname "
								"FROM pg_proc AS p "
								"JOIN pg_aggregate AS a ON a.aggfnoid=p.oid "
								"JOIN pg_proc AS finalfn ON finalfn.oid=a.aggfinalfn "
								"WHERE p.oid >= 16384 "
								"AND a.aggfinalfn = ANY(ARRAY[%s]::regprocedure[]) "
								"AND a.aggtranstype = ANY(ARRAY['anyarray', 'anyelement']::regtype[]) "

		/* 运算符 */
								"UNION ALL "
								"SELECT 'operator' AS objkind, op.oid::regoperator::text AS objname "
								"FROM pg_operator AS op "
								"WHERE op.oid >= 16384 "
								"AND oprcode = ANY(ARRAY[%s]::regprocedure[]) "
								"AND oprleft = ANY(ARRAY['anyarray', 'anyelement']::regtype[]);",
								old_polymorphics.data,
								old_polymorphics.data,
								old_polymorphics.data);

		ntups = PQntuples(res);

		i_objkind = PQfnumber(res, "objkind");
		i_objname = PQfnumber(res, "objname");

		for (int rowno = 0; rowno < ntups; rowno++)
		{
			if (script == NULL &&
				(script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}

			fprintf(script, "  %s: %s\n",
					PQgetvalue(res, rowno, i_objkind),
					PQgetvalue(res, rowno, i_objname));
		}

		PQclear(res);
		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains user-defined objects that refer to internal\n"
				 "polymorphic functions with arguments of type \"anyarray\" or \"anyelement\".\n"
				 "These user-defined objects must be dropped before upgrading and restored\n"
				 "afterwards, changing them to refer to the new corresponding functions with\n"
				 "arguments of type \"anycompatiblearray\" and \"anycompatible\".\n"
				 "A list of the problematic objects is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();

	termPQExpBuffer(&old_polymorphics);
}

/*
 * 验证没有表声明为 WITH OIDS。        
 */
static void
check_for_tables_with_oids(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for tables WITH OIDS");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_with_oids.txt");

	/* 查找声明为 WITH OIDS 的表   */
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

		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n "
								"WHERE	c.relnamespace = n.oid AND "
								"		c.relhasoids AND"
								"       n.nspname NOT IN ('pg_catalog')");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains tables declared WITH OIDS, which is not\n"
				 "supported anymore.  Consider removing the oid column using\n"
				 "    ALTER TABLE ... SET WITHOUT OIDS;\n"
				 "A list of tables with the problem is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_not_null_inheritance()
 *
 * 尝试创建缺少父表全部 NOT NULL 约束的子表会报错。    
 * 自 18 起这种情况不再可能发生，但此前存在              
 * 多种导致它的途径。检查待升级的集簇                   
 * 是否存在任何此类问题。                             
 */
static void
check_for_not_null_inheritance(ClusterInfo *cluster)
{
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];
	int			ntup;

	prep_status("Checking for not-null constraint inconsistencies");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "not_null_inconsistent_columns.txt");
	for (int dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			i_nspname,
					i_relname,
					i_attname;
		DbInfo	   *active_db = &old_cluster.dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(&old_cluster, active_db->db_name);

		res = executeQueryOrDie(conn,
								"SELECT nspname, cc.relname, ac.attname "
								"FROM pg_catalog.pg_inherits i, pg_catalog.pg_attribute ac, "
								"     pg_catalog.pg_attribute ap, pg_catalog.pg_class cc, "
								"     pg_catalog.pg_namespace nc "
								"WHERE cc.oid = ac.attrelid AND i.inhrelid = ac.attrelid "
								"      AND i.inhparent = ap.attrelid AND ac.attname = ap.attname "
								"      AND cc.relnamespace = nc.oid "
								"      AND ap.attnum > 0 and ap.attnotnull AND NOT ac.attnotnull");

		ntup = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (int i = 0; i < ntup; i++)
		{
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s", output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}

			fprintf(script, "  %s.%s.%s\n",
					PQgetvalue(res, i, i_nspname),
					PQgetvalue(res, i, i_relname),
					PQgetvalue(res, i, i_attname));
		}

		PQclear(res);
		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains inconsistent NOT NULL constraints.\n"
				 "If the parent column(s) are NOT NULL, then the child column must\n"
				 "also be marked NOT NULL, or the upgrade will fail.\n"
				 "You can fix this by running\n"
				 "    ALTER TABLE tablename ALTER column SET NOT NULL;\n"
				 "on each column listed in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_composite_data_type_usage()
 *	检查用户表中使用的系统定义复合类型。       
 *
 *	系统目录与 information_schema 视图的行类型 OID           
 *	跨主版本可能变化；与用户定义类型不同，           
 *	我们没有机制强制它们在新集簇中保持一致。    
 *	因此若有用户表用到，对 pg_upgrade 即成问题。         
 */
static void
check_for_composite_data_type_usage(ClusterInfo *cluster)
{
	bool		found;
	Oid			firstUserOid;
	char		output_path[MAXPGPATH];
	char	   *base_query;

	prep_status("Checking for system-defined composite types in user tables");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_composite.txt");

	/*
	 * 查找 initdb 期间创建 *或* 属于 information_schema          
	 * 的复合类型；若曾被删除后重新加载，                
	 * 这很重要。      
	 *
	 * 此处的截止 OID 应匹配源集簇的                     
	 * FirstNormalObjectId 值。我们将其硬编码，而非使用那个 
	 * C #define，因为若该 #define 被更改，                       
	 * 我们自身版本的值并非应选用之值。                       
	 * 最终或需按源版本测试。   
	 */
	firstUserOid = 16384;

	base_query = psprintf("SELECT t.oid FROM pg_catalog.pg_type t "
						  "LEFT JOIN pg_catalog.pg_namespace n ON t.typnamespace = n.oid "
						  " WHERE typtype = 'c' AND (t.oid < %u OR nspname = 'information_schema')",
						  firstUserOid);

	found = check_for_data_types_usage(cluster, base_query, output_path);

	free(base_query);

	if (found)
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains system-defined composite types in user tables.\n"
				 "These type OIDs are not stable across PostgreSQL versions,\n"
				 "so this cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_reg_data_type_usage()
 *	pg_upgrade 仅保留以下系统值：        
 *		pg_class.oid
 *		pg_type.oid
 *		pg_enum.oid
 *
 *	许多 reg* 数据类型引用了不会被保留的              
 *	系统目录信息，因此这些数据类型不能             
 *	用于升级的用户表。   
 */
static void
check_for_reg_data_type_usage(ClusterInfo *cluster)
{
	bool		found;
	char		output_path[MAXPGPATH];

	prep_status("Checking for reg* data types in user tables");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_reg.txt");

	/*
	 * 注意：旧服务器不会有所有这些 reg* 类型，因此必须
	 * 这样书写查询，而不能依赖到 regtype 的强制转换。     
	 */
	found = check_for_data_types_usage(cluster,
									   "SELECT oid FROM pg_catalog.pg_type t "
									   "WHERE t.typnamespace = "
									   "        (SELECT oid FROM pg_catalog.pg_namespace "
									   "         WHERE nspname = 'pg_catalog') "
									   "  AND t.typname IN ( "
	/* pg_class.oid 保留，因此 'regclass' 可用 */
									   "           'regcollation', "
									   "           'regconfig', "
									   "           'regdictionary', "
									   "           'regnamespace', "
									   "           'regoper', "
									   "           'regoperator', "
									   "           'regproc', "
									   "           'regprocedure' "
	/* pg_authid.oid 保留，因此 'regrole' 可用 */
	/* pg_type.oid 基本保留，因此 'regtype' 可用    */
									   "         )",
									   output_path);

	if (found)
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains one of the reg* data types in user tables.\n"
				 "These data types reference system OIDs that are not preserved by\n"
				 "pg_upgrade, so this cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_aclitem_data_type_usage
 *
 *	aclitem 在 16 中更改了存储格式，因此检查。 
 */
static void
check_for_aclitem_data_type_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for incompatible \"%s\" data type in user tables",
				"aclitem");

	snprintf(output_path, sizeof(output_path), "tables_using_aclitem.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.aclitem", output_path))
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains the \"aclitem\" data type in user tables.\n"
				 "The internal format of \"aclitem\" changed in PostgreSQL version 16\n"
				 "so this cluster cannot currently be upgraded.  You can drop the\n"
				 "problem columns and restart the upgrade.  A list of the problem\n"
				 "columns is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_removed_data_type_usage
 *
 *	检查已被移除的内核内置数据类型。调用者知道   
 *	确切列表。
 */
static void
check_for_removed_data_type_usage(ClusterInfo *cluster, const char *version,
								  const char *datatype)
{
	char		output_path[MAXPGPATH];
	char		typename[NAMEDATALEN];

	prep_status("Checking for removed \"%s\" data type in user tables",
				datatype);

	snprintf(output_path, sizeof(output_path), "tables_using_%s.txt",
			 datatype);
	snprintf(typename, sizeof(typename), "pg_catalog.%s", datatype);

	if (check_for_data_type_usage(cluster, typename, output_path))
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains the \"%s\" data type in user tables.\n"
				 "The \"%s\" type has been removed in PostgreSQL version %s,\n"
				 "so this cluster cannot currently be upgraded.  You can drop the\n"
				 "problem columns, or change them to another data type, and restart\n"
				 "the upgrade.  A list of the problem columns is in the file:\n"
				 "    %s", datatype, datatype, version, output_path);
	}
	else
		check_ok();
}


/*
 * check_for_jsonb_9_4_usage()
 *
 *	JSONB 在 9.4 beta 期间改变了存储格式，因此检查。  
 */
static void
check_for_jsonb_9_4_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for incompatible \"jsonb\" data type");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "tables_using_jsonb.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.jsonb", output_path))
	{
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains the \"jsonb\" data type in user tables.\n"
				 "The internal format of \"jsonb\" changed during 9.4 beta so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * check_for_pg_role_prefix()
 *
 *	早于 9.6 的版本不应有任何 pg_* 角色       
 */
static void
check_for_pg_role_prefix(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");
	int			ntups;
	int			i_roloid;
	int			i_rolname;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for roles starting with \"pg_\"");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "pg_role_prefix.txt");

	res = executeQueryOrDie(conn,
							"SELECT oid AS roloid, rolname "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname ~ '^pg_'");

	ntups = PQntuples(res);
	i_roloid = PQfnumber(res, "roloid");
	i_rolname = PQfnumber(res, "rolname");
	for (int rowno = 0; rowno < ntups; rowno++)
	{
		if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
			pg_fatal("could not open file \"%s\": %s",
					 output_path, strerror(errno));
		fprintf(script, "%s (oid=%s)\n",
				PQgetvalue(res, rowno, i_rolname),
				PQgetvalue(res, rowno, i_roloid));
	}

	PQclear(res);

	PQfinish(conn);

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains roles starting with \"pg_\".\n"
				 "\"pg_\" is a reserved prefix for system roles.  The cluster\n"
				 "cannot be upgraded until these roles are renamed.\n"
				 "A list of roles starting with \"pg_\" is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * 验证不存在用户定义的编码转换。          
 */
static void
check_for_user_defined_encoding_conversions(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for user-defined encoding conversions");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "encoding_conversions.txt");

	/* 查找用户定义的编码转换          */
	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_conoid,
					i_conname,
					i_nspname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/*
		 * 下面的查询把 FirstNormalObjectId 硬编码为 16384，      
		 * 而不是把该 C #define 插值到查询中，因为若该 
		 * #define 日后改变，我们要用的界限是 14 之前      
		 * 服务器所用的值，而非未来版本的值。             
		 */
		res = executeQueryOrDie(conn,
								"SELECT c.oid as conoid, c.conname, n.nspname "
								"FROM pg_catalog.pg_conversion c, "
								"     pg_catalog.pg_namespace n "
								"WHERE c.connamespace = n.oid AND "
								"      c.oid >= 16384");
		ntups = PQntuples(res);
		i_conoid = PQfnumber(res, "conoid");
		i_conname = PQfnumber(res, "conname");
		i_nspname = PQfnumber(res, "nspname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			if (script == NULL &&
				(script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s",
						 output_path, strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  (oid=%s) %s.%s\n",
					PQgetvalue(res, rowno, i_conoid),
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_conname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains user-defined encoding conversions.\n"
				 "The conversion function parameters changed in PostgreSQL version 14\n"
				 "so this cluster cannot currently be upgraded.  You can remove the\n"
				 "encoding conversions in the old cluster and restart the upgrade.\n"
				 "A list of user-defined encoding conversions is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}
