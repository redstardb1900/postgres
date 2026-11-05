/*
 *	info.c
 *
 *	information support functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/info.c
 */

#include "postgres_fe.h"

#include "access/transam.h"
#include "catalog/pg_class_d.h"
#include "pg_upgrade.h"

static void create_rel_filename_map(const char *old_data, const char *new_data,
									const DbInfo *old_db, const DbInfo *new_db,
									const RelInfo *old_rel, const RelInfo *new_rel,
									FileNameMap *map);
static void report_unmatched_relation(const RelInfo *rel, const DbInfo *db,
									  bool is_new_db);
static void free_db_and_rel_infos(DbInfoArr *db_arr);
static void get_template0_info(ClusterInfo *cluster);
static void get_db_infos(ClusterInfo *cluster);
static void get_rel_infos(ClusterInfo *cluster, DbInfo *dbinfo);
static void free_rel_infos(RelInfoArr *rel_arr);
static void print_db_infos(DbInfoArr *db_arr);
static void print_rel_infos(RelInfoArr *rel_arr);


/*
 * gen_db_file_maps()
 *
 * 生成从 "old_db" 到 "new_db" 的数据库映射。  
 *
 * 返回 malloc 分配的映射数组。数组长度通过       
 * *nmaps 返回。        
 */
FileNameMap *
gen_db_file_maps(DbInfo *old_db, DbInfo *new_db,
				 int *nmaps,
				 const char *old_pgdata, const char *new_pgdata)
{
	FileNameMap *maps;
	int			old_relnum,
				new_relnum;
	int			num_maps = 0;
	bool		all_matched = true;

	/* 映射不超旧关系数                                          */
	maps = (FileNameMap *) pg_malloc(sizeof(FileNameMap) *
									 old_db->rel_arr.nrels);

	/*
	 * 每个 RelInfo 数组都应已按 OID 排序。我们会扫描        
	 * 这些数组并进行匹配。如果不能匹配所有项，将会  
	 * 中止，但先尽量多地打印不匹配信息。 
	 */
	old_relnum = new_relnum = 0;
	while (old_relnum < old_db->rel_arr.nrels ||
		   new_relnum < new_db->rel_arr.nrels)
	{
		RelInfo    *old_rel = (old_relnum < old_db->rel_arr.nrels) ?
			&old_db->rel_arr.rels[old_relnum] : NULL;
		RelInfo    *new_rel = (new_relnum < new_db->rel_arr.nrels) ?
			&new_db->rel_arr.rels[new_relnum] : NULL;

		/* 处理数组耗尽                            */
		if (!new_rel)
		{
			/*
			 * old_rel 未匹配。这不应发生，因若旧关系有   
			 * TOAST 表，就强制新关系也有 TOAST 表。      
			 */
			report_unmatched_relation(old_rel, old_db, false);
			all_matched = false;
			old_relnum++;
			continue;
		}
		if (!old_rel)
		{
			/*
			 * new_rel 未匹配。这实际也不应发生，但如果它是 
			 * 一个 TOAST 表，则可忽略它并继续，      
			 * 并假定新服务器创建了一个不需要的           
			 * TOAST 表。       
			 */
			if (strcmp(new_rel->nspname, "pg_toast") != 0)
			{
				report_unmatched_relation(new_rel, new_db, true);
				all_matched = false;
			}
			new_relnum++;
			continue;
		}

		/* 检查 OID 不匹配     */
		if (old_rel->reloid < new_rel->reloid)
		{
			/* old_rel 不匹（见上）              */
			report_unmatched_relation(old_rel, old_db, false);
			all_matched = false;
			old_relnum++;
			continue;
		}
		else if (old_rel->reloid > new_rel->reloid)
		{
			/* new_rel 不匹（见上）              */
			if (strcmp(new_rel->nspname, "pg_toast") != 0)
			{
				report_unmatched_relation(new_rel, new_db, true);
				all_matched = false;
			}
			new_relnum++;
			continue;
		}

		/*
		 * 验证相同 OID 的关系是否同名。命名空间名称应  
		 * 当总是匹配，但 TOAST 表（及其索引）的 relname  
		 * 可能不匹配。                     
		 */
		if (strcmp(old_rel->nspname, new_rel->nspname) != 0 ||
			strcmp(old_rel->relname, new_rel->relname) != 0)
		{
			pg_log(PG_WARNING, "Relation names for OID %u in database \"%s\" do not match: "
				   "old name \"%s.%s\", new name \"%s.%s\"",
				   old_rel->reloid, old_db->db_name,
				   old_rel->nspname, old_rel->relname,
				   new_rel->nspname, new_rel->relname);
			all_matched = false;
			old_relnum++;
			new_relnum++;
			continue;
		}

		/* 创映射条               */
		create_rel_filename_map(old_pgdata, new_pgdata, old_db, new_db,
								old_rel, new_rel, maps + num_maps);
		num_maps++;
		old_relnum++;
		new_relnum++;
	}

	if (!all_matched)
		pg_fatal("Failed to match up old and new tables in database \"%s\"",
				 old_db->db_name);

	*nmaps = num_maps;
	return maps;
}


/*
 * create_rel_filename_map()
 *
 * 填充文件节点映射结构并在 "map" 中返回。 
 */
static void
create_rel_filename_map(const char *old_data, const char *new_data,
						const DbInfo *old_db, const DbInfo *new_db,
						const RelInfo *old_rel, const RelInfo *new_rel,
						FileNameMap *map)
{
	/* 新旧表空间不匹则分处                               */
	if (strlen(old_rel->tablespace) == 0)
	{
		/*
		 * 该关系属于默认表空间，因此其 relfile 应当存在  
		 * 于数据目录中。         
		 */
		map->old_tablespace = old_data;
		map->old_tablespace_suffix = "/base";
	}
	else
	{
		/* 关属某表空间，用表空间位                             */
		map->old_tablespace = old_rel->tablespace;
		map->old_tablespace_suffix = old_cluster.tablespace_suffix;
	}

	/* 新表空间同处              */
	if (strlen(new_rel->tablespace) == 0)
	{
		map->new_tablespace = new_data;
		map->new_tablespace_suffix = "/base";
	}
	else
	{
		map->new_tablespace = new_rel->tablespace;
		map->new_tablespace_suffix = new_cluster.tablespace_suffix;
	}

	/* db oid 和 relfilenumber 在新旧集簇间不变                   */
	map->db_oid = old_db->db_oid;
	map->relfilenumber = old_rel->relfilenumber;

	/* 仅日志和错报用，新旧同                                */
	map->nspname = old_rel->nspname;
	map->relname = old_rel->relname;
}


/*
 * 对我们无法匹配到另一数据库的关系发出抱怨，   
 * 并尽可能标识出它。   
 */
static void
report_unmatched_relation(const RelInfo *rel, const DbInfo *db, bool is_new_db)
{
	Oid			reloid = rel->reloid;	/* 下面可改 rel          */
	char		reldesc[1000];
	int			i;

	snprintf(reldesc, sizeof(reldesc), "\"%s.%s\"",
			 rel->nspname, rel->relname);
	if (rel->indtable)
	{
		for (i = 0; i < db->rel_arr.nrels; i++)
		{
			const RelInfo *hrel = &db->rel_arr.rels[i];

			if (hrel->reloid == rel->indtable)
			{
				snprintf(reldesc + strlen(reldesc),
						 sizeof(reldesc) - strlen(reldesc),
						 _(" which is an index on \"%s.%s\""),
						 hrel->nspname, hrel->relname);
				/* 转而关注索引的表以做 toast 检查      */
				rel = hrel;
				break;
			}
		}
		if (i >= db->rel_arr.nrels)
			snprintf(reldesc + strlen(reldesc),
					 sizeof(reldesc) - strlen(reldesc),
					 _(" which is an index on OID %u"), rel->indtable);
	}
	if (rel->toastheap)
	{
		for (i = 0; i < db->rel_arr.nrels; i++)
		{
			const RelInfo *brel = &db->rel_arr.rels[i];

			if (brel->reloid == rel->toastheap)
			{
				snprintf(reldesc + strlen(reldesc),
						 sizeof(reldesc) - strlen(reldesc),
						 _(" which is the TOAST table for \"%s.%s\""),
						 brel->nspname, brel->relname);
				break;
			}
		}
		if (i >= db->rel_arr.nrels)
			snprintf(reldesc + strlen(reldesc),
					 sizeof(reldesc) - strlen(reldesc),
					 _(" which is the TOAST table for OID %u"), rel->toastheap);
	}

	if (is_new_db)
		pg_log(PG_WARNING, "No match found in old cluster for new relation with OID %u in database \"%s\": %s",
			   reloid, db->db_name, reldesc);
	else
		pg_log(PG_WARNING, "No match found in new cluster for old relation with OID %u in database \"%s\": %s",
			   reloid, db->db_name, reldesc);
}

/*
 * get_db_and_rel_infos()
 *
 * 较上层的例程，为运行于给定 "port" 的               
 * 数据库生成 dbinfo。假定服务器已运行。        
 */
void
get_db_and_rel_infos(ClusterInfo *cluster)
{
	int			dbnum;

	if (cluster->dbarr.dbs != NULL)
		free_db_and_rel_infos(&cluster->dbarr);

	get_template0_info(cluster);
	get_db_infos(cluster);

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
		get_rel_infos(cluster, &cluster->dbarr.dbs[dbnum]);

	if (cluster == &old_cluster)
		pg_log(PG_VERBOSE, "\nsource databases:");
	else
		pg_log(PG_VERBOSE, "\ntarget databases:");

	if (log_opts.verbose)
		print_db_infos(&cluster->dbarr);
}


/*
 * 获取 template0 的信息，它将从旧集簇复制                     
 * 到新集簇。    
 */
static void
get_template0_info(ClusterInfo *cluster)
{
	PGconn	   *conn = connectToServer(cluster, "template1");
	DbLocaleInfo *locale;
	PGresult   *dbres;
	int			i_datencoding;
	int			i_datlocprovider;
	int			i_datcollate;
	int			i_datctype;
	int			i_daticulocale;

	if (GET_MAJOR_VERSION(cluster->major_version) >= 1500)
		dbres = executeQueryOrDie(conn,
								  "SELECT encoding, datlocprovider, "
								  "       datcollate, datctype, daticulocale "
								  "FROM	pg_catalog.pg_database "
								  "WHERE datname='template0'");
	else
		dbres = executeQueryOrDie(conn,
								  "SELECT encoding, 'c' AS datlocprovider, "
								  "       datcollate, datctype, NULL AS daticulocale "
								  "FROM	pg_catalog.pg_database "
								  "WHERE datname='template0'");


	if (PQntuples(dbres) != 1)
		pg_fatal("template0 not found");

	locale = pg_malloc(sizeof(DbLocaleInfo));

	i_datencoding = PQfnumber(dbres, "encoding");
	i_datlocprovider = PQfnumber(dbres, "datlocprovider");
	i_datcollate = PQfnumber(dbres, "datcollate");
	i_datctype = PQfnumber(dbres, "datctype");
	i_daticulocale = PQfnumber(dbres, "daticulocale");

	locale->db_encoding = atoi(PQgetvalue(dbres, 0, i_datencoding));
	locale->db_collprovider = PQgetvalue(dbres, 0, i_datlocprovider)[0];
	locale->db_collate = pg_strdup(PQgetvalue(dbres, 0, i_datcollate));
	locale->db_ctype = pg_strdup(PQgetvalue(dbres, 0, i_datctype));
	if (PQgetisnull(dbres, 0, i_daticulocale))
		locale->db_iculocale = NULL;
	else
		locale->db_iculocale = pg_strdup(PQgetvalue(dbres, 0, i_daticulocale));

	cluster->template0 = locale;

	PQclear(dbres);
	PQfinish(conn);
}


/*
 * get_db_infos()
 *
 * 扫描 pg_database 系统目录并填充所有用户   
 * 数据库 
 */
static void
get_db_infos(ClusterInfo *cluster)
{
	PGconn	   *conn = connectToServer(cluster, "template1");
	PGresult   *res;
	int			ntups;
	int			tupnum;
	DbInfo	   *dbinfos;
	int			i_datname,
				i_oid,
				i_spclocation;
	char		query[QUERY_ALLOC];

	snprintf(query, sizeof(query),
			 "SELECT d.oid, d.datname, d.encoding, d.datcollate, d.datctype, ");
	if (GET_MAJOR_VERSION(cluster->major_version) < 1500)
		snprintf(query + strlen(query), sizeof(query) - strlen(query),
				 "'c' AS datlocprovider, NULL AS daticulocale, ");
	else
		snprintf(query + strlen(query), sizeof(query) - strlen(query),
				 "datlocprovider, daticulocale, ");
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "pg_catalog.pg_tablespace_location(t.oid) AS spclocation "
			 "FROM pg_catalog.pg_database d "
			 " LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 " ON d.dattablespace = t.oid "
			 "WHERE d.datallowconn = true "
			 "ORDER BY 1");

	res = executeQueryOrDie(conn, "%s", query);

	i_oid = PQfnumber(res, "oid");
	i_datname = PQfnumber(res, "datname");
	i_spclocation = PQfnumber(res, "spclocation");

	ntups = PQntuples(res);
	dbinfos = (DbInfo *) pg_malloc(sizeof(DbInfo) * ntups);

	for (tupnum = 0; tupnum < ntups; tupnum++)
	{
		dbinfos[tupnum].db_oid = atooid(PQgetvalue(res, tupnum, i_oid));
		dbinfos[tupnum].db_name = pg_strdup(PQgetvalue(res, tupnum, i_datname));
		snprintf(dbinfos[tupnum].db_tablespace, sizeof(dbinfos[tupnum].db_tablespace), "%s",
				 PQgetvalue(res, tupnum, i_spclocation));
	}
	PQclear(res);

	PQfinish(conn);

	cluster->dbarr.dbs = dbinfos;
	cluster->dbarr.ndbs = ntups;
}


/*
 * get_rel_infos()
 *
 * 获取 "dbinfo" 所引用数据库中全部用户表                 
 * 和索引的 relinfo。 
 *
 * 注意：假定得到的 RelInfo 数组按 OID 排序。         
 * 这使后续处理能够高效地匹配新旧数据库。                  
 */
static void
get_rel_infos(ClusterInfo *cluster, DbInfo *dbinfo)
{
	PGconn	   *conn = connectToServer(cluster,
									   dbinfo->db_name);
	PGresult   *res;
	RelInfo    *relinfos;
	int			ntups;
	int			relnum;
	int			num_rels = 0;
	char	   *nspname = NULL;
	char	   *relname = NULL;
	char	   *tablespace = NULL;
	int			i_spclocation,
				i_nspname,
				i_relname,
				i_reloid,
				i_indtable,
				i_toastheap,
				i_relfilenumber,
				i_reltablespace;
	char		query[QUERY_ALLOC];
	char	   *last_namespace = NULL,
			   *last_tablespace = NULL;

	query[0] = '\0';			/* 初始化查询串为空         */

	/*
	 * 创建 CTE 收集普通用户表和 matview 的 OIDs，             
	 * 但不包括 toast 表和索引。我们假定 OID                    
	 * >= FirstNormalObjectId 的关系属于用户。（这            
	 * 多半与命名空间名称排除冗余，但稳妥起见。）  
	 *
	 * pg_largeobject 包含未出现在 pg_dump 输出中              
	 * 的用户数据，因此必须复制该系统表。最简单的       
	 * 即把它当作用户表。    
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "WITH regular_heap (reloid, indtable, toastheap) AS ( "
			 "  SELECT c.oid, 0::oid, 0::oid "
			 "  FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n "
			 "         ON c.relnamespace = n.oid "
			 "  WHERE relkind IN (" CppAsString2(RELKIND_RELATION) ", "
			 CppAsString2(RELKIND_MATVIEW) ") AND "
	/* 排除可能存在的孤立临时表  */
			 "    ((n.nspname !~ '^pg_temp_' AND "
			 "      n.nspname !~ '^pg_toast_temp_' AND "
			 "      n.nspname NOT IN ('pg_catalog', 'information_schema', "
			 "                        'binary_upgrade', 'pg_toast') AND "
			 "      c.oid >= %u::pg_catalog.oid) OR "
			 "     (n.nspname = 'pg_catalog' AND "
			 "      relname IN ('pg_largeobject') ))), ",
			 FirstNormalObjectId);

	/*
	 * 添加一个 CTE，收集属于 regular_heap CTE 所选表的       
	 * toast 表的 OID。（由于上述命名空间名称规则不    
	 * 适用于 toast 表，所以必须单独进行此项操作。）    
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "  toast_heap (reloid, indtable, toastheap) AS ( "
			 "  SELECT c.reltoastrelid, 0::oid, c.oid "
			 "  FROM regular_heap JOIN pg_catalog.pg_class c "
			 "      ON regular_heap.reloid = c.oid "
			 "  WHERE c.reltoastrelid != 0), ");

	/*
	 * 添加一个 CTE，收集前面所选全部表的有效            
	 * 索引 OIDs。无效索引可忽略，pg_dump 也如此。         
	 * 在 9.2 中必须测试 indisready，对更早/更晚                
	 * 无害。
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "  all_index (reloid, indtable, toastheap) AS ( "
			 "  SELECT indexrelid, indrelid, 0::oid "
			 "  FROM pg_catalog.pg_index "
			 "  WHERE indisvalid AND indisready "
			 "    AND indrelid IN "
			 "        (SELECT reloid FROM regular_heap "
			 "         UNION ALL "
			 "         SELECT reloid FROM toast_heap)) ");

	/*
	 * 现在可以写出为每个堆和索引关系取回我们              
	 * 所需数据的查询。确保结果按 OID 排序。       
	 */
	snprintf(query + strlen(query), sizeof(query) - strlen(query),
			 "SELECT all_rels.*, n.nspname, c.relname, "
			 "  c.relfilenode, c.reltablespace, "
			 "  pg_catalog.pg_tablespace_location(t.oid) AS spclocation "
			 "FROM (SELECT * FROM regular_heap "
			 "      UNION ALL "
			 "      SELECT * FROM toast_heap "
			 "      UNION ALL "
			 "      SELECT * FROM all_index) all_rels "
			 "  JOIN pg_catalog.pg_class c "
			 "      ON all_rels.reloid = c.oid "
			 "  JOIN pg_catalog.pg_namespace n "
			 "     ON c.relnamespace = n.oid "
			 "  LEFT OUTER JOIN pg_catalog.pg_tablespace t "
			 "     ON c.reltablespace = t.oid "
			 "ORDER BY 1;");

	res = executeQueryOrDie(conn, "%s", query);

	ntups = PQntuples(res);

	relinfos = (RelInfo *) pg_malloc(sizeof(RelInfo) * ntups);

	i_reloid = PQfnumber(res, "reloid");
	i_indtable = PQfnumber(res, "indtable");
	i_toastheap = PQfnumber(res, "toastheap");
	i_nspname = PQfnumber(res, "nspname");
	i_relname = PQfnumber(res, "relname");
	i_relfilenumber = PQfnumber(res, "relfilenode");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_spclocation = PQfnumber(res, "spclocation");

	for (relnum = 0; relnum < ntups; relnum++)
	{
		RelInfo    *curr = &relinfos[num_rels++];

		curr->reloid = atooid(PQgetvalue(res, relnum, i_reloid));
		curr->indtable = atooid(PQgetvalue(res, relnum, i_indtable));
		curr->toastheap = atooid(PQgetvalue(res, relnum, i_toastheap));

		nspname = PQgetvalue(res, relnum, i_nspname);
		curr->nsp_alloc = false;

		/*
		 * 许多命名空间与表空间字符串相同，因此我们     
		 * 尽量复用已分配的字符串指针，以减少                
		 * 内存消耗。    
		 */
		/* 能否复用先前的字符串分配？      */
		if (last_namespace && strcmp(nspname, last_namespace) == 0)
			curr->nspname = last_namespace;
		else
		{
			last_namespace = curr->nspname = pg_strdup(nspname);
			curr->nsp_alloc = true;
		}

		relname = PQgetvalue(res, relnum, i_relname);
		curr->relname = pg_strdup(relname);

		curr->relfilenumber = atooid(PQgetvalue(res, relnum, i_relfilenumber));
		curr->tblsp_alloc = false;

		/* 表空间 oid 非默认？         */
		if (atooid(PQgetvalue(res, relnum, i_reltablespace)) != 0)
		{
			/*
			 * 表空间位置可能为 ""，意即集簇的默认     
			 * 位置，即 pg_default 或 pg_global。       
			 */
			tablespace = PQgetvalue(res, relnum, i_spclocation);

			/* 能否复用先前的字符串分配？      */
			if (last_tablespace && strcmp(tablespace, last_tablespace) == 0)
				curr->tablespace = last_tablespace;
			else
			{
				last_tablespace = curr->tablespace = pg_strdup(tablespace);
				curr->tblsp_alloc = true;
			}
		}
		else
			/* reltablespace oid 为零表示数据库表空间。         */
			curr->tablespace = dbinfo->db_tablespace;
	}
	PQclear(res);

	PQfinish(conn);

	dbinfo->rel_arr.rels = relinfos;
	dbinfo->rel_arr.nrels = num_rels;
}


static void
free_db_and_rel_infos(DbInfoArr *db_arr)
{
	int			dbnum;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
	{
		free_rel_infos(&db_arr->dbs[dbnum].rel_arr);
		pg_free(db_arr->dbs[dbnum].db_name);
	}
	pg_free(db_arr->dbs);
	db_arr->dbs = NULL;
	db_arr->ndbs = 0;
}


static void
free_rel_infos(RelInfoArr *rel_arr)
{
	int			relnum;

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
	{
		if (rel_arr->rels[relnum].nsp_alloc)
			pg_free(rel_arr->rels[relnum].nspname);
		pg_free(rel_arr->rels[relnum].relname);
		if (rel_arr->rels[relnum].tblsp_alloc)
			pg_free(rel_arr->rels[relnum].tablespace);
	}
	pg_free(rel_arr->rels);
	rel_arr->nrels = 0;
}


static void
print_db_infos(DbInfoArr *db_arr)
{
	int			dbnum;

	for (dbnum = 0; dbnum < db_arr->ndbs; dbnum++)
	{
		pg_log(PG_VERBOSE, "Database: %s", db_arr->dbs[dbnum].db_name);
		print_rel_infos(&db_arr->dbs[dbnum].rel_arr);
	}
}


static void
print_rel_infos(RelInfoArr *rel_arr)
{
	int			relnum;

	for (relnum = 0; relnum < rel_arr->nrels; relnum++)
		pg_log(PG_VERBOSE, "relname: %s.%s: reloid: %u reltblspace: %s",
			   rel_arr->rels[relnum].nspname,
			   rel_arr->rels[relnum].relname,
			   rel_arr->rels[relnum].reloid,
			   rel_arr->rels[relnum].tablespace);
}
