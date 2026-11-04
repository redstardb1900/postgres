/*
 *	relfilenumber.c
 *
 *	relfilenumber functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/relfilenumber.c
 */

#include "postgres_fe.h"

#include <sys/stat.h>

#include "access/transam.h"
#include "catalog/pg_class_d.h"
#include "pg_upgrade.h"

static void transfer_single_new_db(FileNameMap *maps, int size, char *old_tablespace);
static void transfer_relfile(FileNameMap *map, const char *type_suffix, bool vm_must_add_frozenbit);


/*
 * transfer_all_new_tablespaces()
 *
 * 负责升级所有数据库。它会调用例程来生成映射，                    
 * 物理链接这些数据库。
 */
void
transfer_all_new_tablespaces(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
							 char *old_pgdata, char *new_pgdata)
{
	switch (user_opts.transfer_mode)
	{
		case TRANSFER_MODE_CLONE:
			prep_status_progress("Cloning user relation files");
			break;
		case TRANSFER_MODE_COPY:
			prep_status_progress("Copying user relation files");
			break;
		case TRANSFER_MODE_LINK:
			prep_status_progress("Linking user relation files");
			break;
	}

	/*按表空间传输较复杂因单数据库可用多表空间。非并行模式传 NULL 匹配所有。并行模式传默认和用户表空间并行操作。
	 * 
	 * 
	 * 
	 * 
	 * 
	                                                                                                                                                               */
	if (user_opts.jobs <= 1)
		parallel_transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata,
									  new_pgdata, NULL);
	else
	{
		int			tblnum;

		/* 传输默认表空间       */
		parallel_transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata,
									  new_pgdata, old_pgdata);

		for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
			parallel_transfer_all_new_dbs(old_db_arr,
										  new_db_arr,
										  old_pgdata,
										  new_pgdata,
										  os_info.old_tablespaces[tblnum]);
		/*收全部子进程 */
		while (reap_child(true) == true)
			;
	}

	end_progress_output();
	check_ok();
}


/*升级所有数据库：生成映射并物理链接。
 * 
 *
 * 
 * 
                                                                                     */
void
transfer_all_new_dbs(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
					 char *old_pgdata, char *new_pgdata, char *old_tablespace)
{
	int			old_dbnum,
				new_dbnum;

	/* 扫描旧集簇 DB 传输文件                         */
	for (old_dbnum = new_dbnum = 0;
		 old_dbnum < old_db_arr->ndbs;
		 old_dbnum++, new_dbnum++)
	{
		DbInfo	   *old_db = &old_db_arr->dbs[old_dbnum],
				   *new_db = NULL;
		FileNameMap *mappings;
		int			n_maps;

		/*跳过仅新集簇有的数据库（如 postgres），因用户可能已删除。
		 * 
		 * 
		 * 
		                                                                                        */
		for (; new_dbnum < new_db_arr->ndbs; new_dbnum++)
		{
			new_db = &new_db_arr->dbs[new_dbnum];
			if (strcmp(old_db->db_name, new_db->db_name) == 0)
				break;
		}

		if (new_dbnum >= new_db_arr->ndbs)
			pg_fatal("old database \"%s\" not found in the new cluster",
					 old_db->db_name);

		mappings = gen_db_file_maps(old_db, new_db, &n_maps, old_pgdata,
									new_pgdata);
		if (n_maps)
		{
			transfer_single_new_db(mappings, n_maps, old_tablespace);
		}
		/* 即使 n_maps==0 也分配内存           */
		pg_free(mappings);
	}
}

/*为映射数组创建链接。
 * 
 *
 * 
                                            */
static void
transfer_single_new_db(FileNameMap *maps, int size, char *old_tablespace)
{
	int			mapnum;
	bool		vm_must_add_frozenbit = false;

	/*需重写可见性映射？
	 * 
	          */
	if (old_cluster.controldata.cat_ver < VISIBILITY_MAP_FROZEN_BIT_CAT_VER &&
		new_cluster.controldata.cat_ver >= VISIBILITY_MAP_FROZEN_BIT_CAT_VER)
		vm_must_add_frozenbit = true;

	for (mapnum = 0; mapnum < size; mapnum++)
	{
		if (old_tablespace == NULL ||
			strcmp(maps[mapnum].old_tablespace, old_tablespace) == 0)
		{
			/* 传输主文件       */
			transfer_relfile(&maps[mapnum], "", vm_must_add_frozenbit);

			/*复制/链接任何 fsm 和 vm 文件
			 * 
			         */
			transfer_relfile(&maps[mapnum], "_fsm", vm_must_add_frozenbit);
			transfer_relfile(&maps[mapnum], "_vm", vm_must_add_frozenbit);
		}
	}
}


/*从旧集簇复制或链接文件到新集簇。vm_must_add_frozenbit 为 true 时，vm 叉即使链接模式也重写。
 * 
 *
 * 
 * 
 * 
                                              */
static void
transfer_relfile(FileNameMap *map, const char *type_suffix, bool vm_must_add_frozenbit)
{
	char		old_file[MAXPGPATH];
	char		new_file[MAXPGPATH];
	int			segno;
	char		extent_suffix[65];
	struct stat statbuf;

	/*复制/链接段文件。PG 将大文件分成 1GB 段，首段无后缀，后续为 .1, .2, .3。
	 * 
	 * 
	 * 
	                                                                                                                */
	for (segno = 0;; segno++)
	{
		if (segno == 0)
			extent_suffix[0] = '\0';
		else
			snprintf(extent_suffix, sizeof(extent_suffix), ".%d", segno);

		snprintf(old_file, sizeof(old_file), "%s%s/%u/%u%s%s",
				 map->old_tablespace,
				 map->old_tablespace_suffix,
				 map->db_oid,
				 map->relfilenumber,
				 type_suffix,
				 extent_suffix);
		snprintf(new_file, sizeof(new_file), "%s%s/%u/%u%s%s",
				 map->new_tablespace,
				 map->new_tablespace_suffix,
				 map->db_oid,
				 map->relfilenumber,
				 type_suffix,
				 extent_suffix);

		/* 是段/fsm/vm？                  */
		if (type_suffix[0] != '\0' || segno != 0)
		{
			/*文件打开失败？*/
			if (stat(old_file, &statbuf) != 0)
			{
				/* 文件不存在？返回                     */
				if (errno == ENOENT)
					return;
				else
					pg_fatal("error while checking for file existence \"%s.%s\" (\"%s\" to \"%s\"): %s",
							 map->nspname, map->relname, old_file, new_file,
							 strerror(errno));
			}

			/* 文件空则返回            */
			if (statbuf.st_size == 0)
				return;
		}

		unlink(new_file);

		/* 复制耗时，给进度反馈                        */
		pg_log(PG_STATUS, "%s", old_file);

		if (vm_must_add_frozenbit && strcmp(type_suffix, "_vm") == 0)
		{
			/* 需重写 VM 格式                   */
			pg_log(PG_VERBOSE, "rewriting \"%s\" to \"%s\"",
				   old_file, new_file);
			rewriteVisibilityMap(old_file, new_file, map->nspname, map->relname);
		}
		else
			switch (user_opts.transfer_mode)
			{
				case TRANSFER_MODE_CLONE:
					pg_log(PG_VERBOSE, "cloning \"%s\" to \"%s\"",
						   old_file, new_file);
					cloneFile(old_file, new_file, map->nspname, map->relname);
					break;
				case TRANSFER_MODE_COPY:
					pg_log(PG_VERBOSE, "copying \"%s\" to \"%s\"",
						   old_file, new_file);
					copyFile(old_file, new_file, map->nspname, map->relname);
					break;
				case TRANSFER_MODE_LINK:
					pg_log(PG_VERBOSE, "linking \"%s\" to \"%s\"",
						   old_file, new_file);
					linkFile(old_file, new_file, map->nspname, map->relname);
			}
	}
}
