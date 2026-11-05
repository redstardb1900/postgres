/*-------------------------------------------------------------------------
 *
 * filemap.c
 *	  A data structure for keeping track of files that have changed.
 *
 * This source file contains the logic to decide what to do with different
 * kinds of files, and the data structure to support it.  Before modifying
 * anything, pg_rewind collects information about all the files and their
 * attributes in the target and source data directories.  It also scans the
 * WAL log in the target, and collects information about data blocks that
 * were changed.  All this information is stored in a hash table, using the
 * file path relative to the root of the data directory as the key.
 *
 * After collecting all the information required, the decide_file_actions()
 * function scans the hash table and decides what action needs to be taken
 * for each file.  Finally, it sorts the array to the final order that the
 * actions should be executed in.
 *
 * Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>

#include "catalog/pg_tablespace_d.h"
#include "common/hashfn.h"
#include "common/string.h"
#include "datapagemap.h"
#include "filemap.h"
#include "pg_rewind.h"
#include "storage/fd.h"

/*
 * 定义哈希表，用于存储在源和目标系统中出现的文件
 * 信息。
                                                                                       */
static uint32 hash_string_pointer(const char *s);
#define SH_PREFIX				filehash
#define SH_ELEMENT_TYPE			file_entry_t
#define SH_KEY_TYPE				const char *
#define SH_KEY					path
#define SH_HASH_KEY(tb, key)	hash_string_pointer(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_SCOPE				static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

#define FILEHASH_INITIAL_SIZE	1000

static filehash_hash *filehash;

static bool isRelDataFile(const char *path);
static char *datasegpath(RelFileLocator rlocator, ForkNumber forknum,
						 BlockNumber segno);

static file_entry_t *insert_filehash_entry(const char *path);
static file_entry_t *lookup_filehash_entry(const char *path);

/*
 * 单独的哈希表，跟踪不可删除的 WAL 文件。
                                                 */
typedef struct keepwal_entry
{
	const char *path;
	uint32		status;
} keepwal_entry;

#define SH_PREFIX				keepwal
#define SH_ELEMENT_TYPE			keepwal_entry
#define SH_KEY_TYPE				const char *
#define SH_KEY					path
#define SH_HASH_KEY(tb, key)	hash_string_pointer(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_SCOPE				static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

#define KEEPWAL_INITIAL_SIZE	1000


static keepwal_hash *keepwal = NULL;
static bool keepwal_entry_exists(const char *path);

static int	final_filemap_cmp(const void *a, const void *b);

static bool check_file_excluded(const char *path, bool is_source);

/*
 * 排除列表元素定义，用于在回绕时排除内容。
 * "name" 是要检查排除的文件或路径名称。如果
 * "match_prefix" 为 true，则匹配前缀的所有
 * 内容都会被排除。
                                                                                                                                                         */
struct exclude_list_item
{
	const char *name;
	bool		match_prefix;
};

/*
 * 这些目录的内容在服务器启动期间被移除或重建，
 * 因此不包含在 pg_rewind 处理的数据中。
 *
 * 注意：这些列表应与 basebackup.c 保持同步。
 * 某些值由于定义在后端专用头文件中而被硬编码，
 * 与 basebackup.c 使用的不同。此列表按最佳
 * 努力维护。
                                                                                                                                                                                                                                                        */
static const char *excludeDirContents[] =
{
	/*
	 * 跳过临时统计文件。PG_STAT_TMP_DIR 须跳过，因      
	 * 扩展如 pg_stat_statements 在其中存储数据。       
	 */
	"pg_stat_tmp",				/* defined as PG_STAT_TMP_DIR */

	/*
	 * 通常备份此目录的内容没有意义，即使意图是恢复 
	 * 到另一个主库。详情参见 backup.sgml。                       
	 * 了解更多。                 
	 */
	"pg_replslot",

	/* 启动时移除的内容，参见 dsm_cleanup_for_mmap()。                      */
	"pg_dynshmem",				/* defined as PG_DYNSHMEM_DIR */

	/* 启动时移除的内容，参见 AsyncShmemInit()。                      */
	"pg_notify",

	/*
	 * 启动时加载旧内容以作调试，但正常运行不需要，     
	 * 参见 SerialInit()。             
	 */
	"pg_serial",

	/* 启动时移除的内容，参见 DeleteAllExportedSnapshotFiles()。                      */
	"pg_snapshots",

	/* 启动时清零的内容，参见 StartupSUBTRANS()。                     */
	"pg_subtrans",

	/* end of list */
	NULL
};

/*
 * 文件映射处理中排除的文件列表。匹配前缀的
 * 文件会被排除。
                                                                */
static const struct exclude_list_item excludeFiles[] =
{
	/* 跳过自动配置临时文件。                    */
	{"postgresql.auto.conf.tmp", false},	/* defined as PG_AUTOCONF_FILENAME */

	/* 跳过当前日志文件临时文件                         */
	{"current_logfiles.tmp", false},	/* 定义为 
										 * LOG_METAINFO_DATAFILE_TMP */

	/* 跳过关系缓存，因为在启动时会重建                                     */
	{"pg_internal.init", true}, /* defined as RELCACHE_INIT_FILENAME */

	/*
	 * 若 backup_label 或 tablespace_map 文件存在，表明             
	 * 曾恢复失败，该数据库集簇可能无法回绕，但仍排除  
	 * 找到的这类文件。      
	 */
	{"backup_label", false},	/* defined as BACKUP_LABEL_FILE */
	{"tablespace_map", false},	/* defined as TABLESPACE_MAP */

	/*
	 * 若 backup_manifest 存在，它属于用来启动此服务器       
	 * 的备份。它对此备份是*不*正确的。            
	 * backup_manifest 会在用户需要时单独注入备份             
	 * 。
	 */
	{"backup_manifest", false},

	{"postmaster.pid", false},
	{"postmaster.opts", false},

	/* end of list */
	{NULL, false}
};

/*
 * 初始化文件映射的哈希表。
                                */
void
filehash_init(void)
{
	filehash = filehash_create(FILEHASH_INITIAL_SIZE, NULL);
}

/* 查找 'path' 的条目，不存在则创建                                             */
static file_entry_t *
insert_filehash_entry(const char *path)
{
	file_entry_t *entry;
	bool		found;

	entry = filehash_insert(filehash, path, &found);
	if (!found)
	{
		entry->path = pg_strdup(path);
		entry->isrelfile = isRelDataFile(path);

		entry->target_exists = false;
		entry->target_type = FILE_TYPE_UNDEFINED;
		entry->target_size = 0;
		entry->target_link_target = NULL;
		entry->target_pages_to_overwrite.bitmap = NULL;
		entry->target_pages_to_overwrite.bitmapsize = 0;

		entry->source_exists = false;
		entry->source_type = FILE_TYPE_UNDEFINED;
		entry->source_size = 0;
		entry->source_link_target = NULL;

		entry->action = FILE_ACTION_UNDECIDED;
	}

	return entry;
}

static file_entry_t *
lookup_filehash_entry(const char *path)
{
	return filehash_lookup(filehash, path);
}

/*
 * 初始化哈希表，存储必须保留的 WAL 文件名。
                                            */
void
keepwal_init(void)
{
	/* 凭空设定的初始哈希大小                          */
	keepwal = keepwal_create(KEEPWAL_INITIAL_SIZE, NULL);
}

/* 标记给定文件以防止其被移除                              */
void
keepwal_add_entry(const char *path)
{
	keepwal_entry *entry;
	bool		found;

	/* 仅应在 keepwal 已初始化时调用                            */
	Assert(keepwal != NULL);

	entry = keepwal_insert(keepwal, path, &found);

	if (!found)
		entry->path = pg_strdup(path);
}

/* 若文件被标记为不可移除则返回 true，否则返回 false                                      */
static bool
keepwal_entry_exists(const char *path)
{
	return keepwal_lookup(keepwal, path) != NULL;
}

/*
 * 处理源文件列表的回调。
 *
 * 对源服务器中的每个文件调用一次。记录文件的
 * 类型和大小，以便 decide_file_action() 后续
 * 决定如何处理。
                                                                                                                                     */
void
process_source_file(const char *path, file_type_t type, size_t size,
					const char *link_target)
{
	file_entry_t *entry;

	/*
	 * 假设 pg_wal 是目录，即使它实际是符号链接。          
	 * 不想干扰链接本身，也不抱怨源和目标不一致。   
	 * （反之亦然）。                             
	 */
	if (strcmp(path, "pg_wal") == 0 && type == FILE_TYPE_SYMLINK)
		type = FILE_TYPE_DIRECTORY;

	/*
	 * 健全检查：像数据文件的文件名须为普通          
	 * 文件      
	 */
	if (type != FILE_TYPE_REGULAR && isRelDataFile(path))
		pg_fatal("data file \"%s\" in source is not a regular file", path);

	/* 记住此源文件                    */
	entry = insert_filehash_entry(path);
	if (entry->source_exists)
		pg_fatal("duplicate source file \"%s\"", path);
	entry->source_exists = true;
	entry->source_type = type;
	entry->source_size = size;
	entry->source_link_target = link_target ? pg_strdup(link_target) : NULL;
}

/*
 * 处理目标文件列表的回调。
 *
 * 记录文件的类型和大小，类似 process_source_file()。
                                                                */
void
process_target_file(const char *path, file_type_t type, size_t size,
					const char *link_target)
{
	file_entry_t *entry;

	/*
	 * 此处不应用任何排除过滤。好处是处理源文件          
	 * 时从源数据文件夹过滤掉的所有路径，也会从目标     
	 * 数据文件夹中移除。                             
	 */

	/*
	 * 与 process_source_file 类似，假设 pg_wal 始终为目录。      
	 */
	if (strcmp(path, "pg_wal") == 0 && type == FILE_TYPE_SYMLINK)
		type = FILE_TYPE_DIRECTORY;

	/* 记住此目标文件                   */
	entry = insert_filehash_entry(path);
	if (entry->target_exists)
		pg_fatal("duplicate source file \"%s\"", path);
	entry->target_exists = true;
	entry->target_type = type;
	entry->target_size = size;
	entry->target_link_target = link_target ? pg_strdup(link_target) : NULL;
}

/*
 * 在读取目标 WAL 时，为目标系统中已更改的                
 * 每个块调用此回调。它决定目标 relfile 中给定的      
 * 'blkno' 是否需要从源覆盖，如果需要则记录到               
 * 'target_pages_to_overwrite' 位图中。                
 *
 * 注意：两个系统上的所有文件必须已添加到哈希表     
 * 中！     
 */
void
process_target_wal_block_change(ForkNumber forknum, RelFileLocator rlocator,
								BlockNumber blkno)
{
	char	   *path;
	file_entry_t *entry;
	BlockNumber blkno_inseg;
	int			segno;

	segno = blkno / RELSEG_SIZE;
	blkno_inseg = blkno % RELSEG_SIZE;

	path = datasegpath(rlocator, forknum, segno);
	entry = lookup_filehash_entry(path);
	pfree(path);

	/*
	 * 若块在两个系统中均存在，记住它。否则可以安全  
	 * 忽略。            
	 *
	 * 若块超出源系统的 EOF，或文件在源中根本不      
	 * 存在，我们总会从目标中截断/移除。同理，           
	 * 若它在目标中已不存在，我们                          
	 * 将用源系统的"尾部"来复制。                              
	 *                
	 *
	 * 可能为任一系统中都不再存在的文件找到 WAL。这  
	 * 意味着该关系稍后在目标中被删除，之后独立地在 
	 * 源中也被删除；或者它在目标中被创建并删除且从未
	 * 在源中存在。无论哪种情况，都可安全忽略。        
	 *                                                 
	 */
	if (entry)
	{
		Assert(entry->isrelfile);

		if (entry->target_exists)
		{
			if (entry->target_type != FILE_TYPE_REGULAR)
				pg_fatal("unexpected page modification for non-regular file \"%s\"",
						 entry->path);

			if (entry->source_exists)
			{
				off_t		end_offset;

				end_offset = (blkno_inseg + 1) * BLCKSZ;
				if (end_offset <= entry->source_size && end_offset <= entry->target_size)
					datapagemap_add(&entry->target_pages_to_overwrite, blkno_inseg);
			}
		}
	}
}

/*
 * 这是 pg_rewind 可以跳过复制的文件路径吗？
                                */
static bool
check_file_excluded(const char *path, bool is_source)
{
	char		localpath[MAXPGPATH];
	int			excludeIdx;
	const char *filename;

	/*
	 * 跳过所有临时文件，.../pgsql_tmp/...和.../pgsql_tmp.* 
	 */
	if (strstr(path, "/" PG_TEMP_FILE_PREFIX) != NULL ||
		strstr(path, "/" PG_TEMP_FILES_DIR "/") != NULL)
	{
		return true;
	}

	/* 检查单个文件...                 */
	for (excludeIdx = 0; excludeFiles[excludeIdx].name != NULL; excludeIdx++)
	{
		int			cmplen = strlen(excludeFiles[excludeIdx].name);

		filename = last_dir_separator(path);
		if (filename == NULL)
			filename = path;
		else
			filename++;

		if (!excludeFiles[excludeIdx].match_prefix)
			cmplen++;
		if (strncmp(filename, excludeFiles[excludeIdx].name, cmplen) == 0)
		{
			if (is_source)
				pg_log_debug("entry \"%s\" excluded from source file list",
							 path);
			else
				pg_log_debug("entry \"%s\" excluded from target file list",
							 path);
			return true;
		}
	}

	/*
	 * ... 并检查一些目录。注意这也包含目录本身           
	 * 内部的任何内容。          
	 */
	for (excludeIdx = 0; excludeDirContents[excludeIdx] != NULL; excludeIdx++)
	{
		snprintf(localpath, sizeof(localpath), "%s/",
				 excludeDirContents[excludeIdx]);
		if (strstr(path, localpath) == path)
		{
			if (is_source)
				pg_log_debug("entry \"%s\" excluded from source file list",
							 path);
			else
				pg_log_debug("entry \"%s\" excluded from target file list",
							 path);
			return true;
		}
	}

	return false;
}

static const char *
action_to_str(file_action_t action)
{
	switch (action)
	{
		case FILE_ACTION_NONE:
			return "NONE";
		case FILE_ACTION_COPY:
			return "COPY";
		case FILE_ACTION_TRUNCATE:
			return "TRUNCATE";
		case FILE_ACTION_COPY_TAIL:
			return "COPY_TAIL";
		case FILE_ACTION_CREATE:
			return "CREATE";
		case FILE_ACTION_REMOVE:
			return "REMOVE";

		default:
			return "unknown";
	}
}

/*
 * 计算进度报告所需的总计。
                                      */
void
calculate_totals(filemap_t *filemap)
{
	file_entry_t *entry;
	int			i;

	filemap->total_size = 0;
	filemap->fetch_size = 0;

	for (i = 0; i < filemap->nentries; i++)
	{
		entry = filemap->entries[i];

		if (entry->source_type != FILE_TYPE_REGULAR)
			continue;

		filemap->total_size += entry->source_size;

		if (entry->action == FILE_ACTION_COPY)
		{
			filemap->fetch_size += entry->source_size;
			continue;
		}

		if (entry->action == FILE_ACTION_COPY_TAIL)
			filemap->fetch_size += (entry->source_size - entry->target_size);

		if (entry->target_pages_to_overwrite.bitmapsize > 0)
		{
			datapagemap_iterator_t *iter;
			BlockNumber blk;

			iter = datapagemap_iterate(&entry->target_pages_to_overwrite);
			while (datapagemap_next(iter, &blk))
				filemap->fetch_size += BLCKSZ;

			pg_free(iter);
		}
	}
}

void
print_filemap(filemap_t *filemap)
{
	file_entry_t *entry;
	int			i;

	for (i = 0; i < filemap->nentries; i++)
	{
		entry = filemap->entries[i];
		if (entry->action != FILE_ACTION_NONE ||
			entry->target_pages_to_overwrite.bitmapsize > 0)
		{
			pg_log_debug("%s (%s)", entry->path,
						 action_to_str(entry->action));

			if (entry->target_pages_to_overwrite.bitmapsize > 0)
				datapagemap_print(&entry->target_pages_to_overwrite);
		}
	}
	fflush(stdout);
}

/*
 * 它看起来像关系数据文件吗？
 *
 * 出于我们的目的，仅主分支文件被视为关系文件。    
 * 其他分支始终完整复制，因为我们无法可靠跟踪         
 * 它们的更改，因为 WAL 仅包含主分支的块                     
 * 引用。         
 */
static bool
isRelDataFile(const char *path)
{
	RelFileLocator rlocator;
	unsigned int segNo;
	int			nmatch;
	bool		matched;

	/*----
	 * 关系数据文件可位于以下目录之一：               
	 *
	 * global/
	 *		共享关系    
	 *
	 * base/<db oid>/
	 *		常规关系，默认表空间       
	 *
	 * pg_tblspc/<tblspc oid>/<tblspc version>/
	 *		非默认表空间内（目录名称                      
	 *		取决于版本） 
	 *
	 * 关系数据文件本身的文件名形如：               
	 *
	 * <oid>.<段号>        
	 *
	 *----
	 */
	rlocator.spcOid = InvalidOid;
	rlocator.dbOid = InvalidOid;
	rlocator.relNumber = InvalidRelFileNumber;
	segNo = 0;
	matched = false;

	nmatch = sscanf(path, "global/%u.%u", &rlocator.relNumber, &segNo);
	if (nmatch == 1 || nmatch == 2)
	{
		rlocator.spcOid = GLOBALTABLESPACE_OID;
		rlocator.dbOid = 0;
		matched = true;
	}
	else
	{
		nmatch = sscanf(path, "base/%u/%u.%u",
						&rlocator.dbOid, &rlocator.relNumber, &segNo);
		if (nmatch == 2 || nmatch == 3)
		{
			rlocator.spcOid = DEFAULTTABLESPACE_OID;
			matched = true;
		}
		else
		{
			nmatch = sscanf(path, "pg_tblspc/%u/" TABLESPACE_VERSION_DIRECTORY "/%u/%u.%u",
							&rlocator.spcOid, &rlocator.dbOid, &rlocator.relNumber,
							&segNo);
			if (nmatch == 3 || nmatch == 4)
				matched = true;
		}
	}

	/*
	 * 上述 sscanf 测试可能匹配末尾多余字符的文件。      
	 * 为排除此类情况，交叉检查：用从文件名提取的   
	 * RelFileLocator 信息调用 GetRelationPath，确              
	 * 认生成相同的文件名。             
	 */
	if (matched)
	{
		char	   *check_path = datasegpath(rlocator, MAIN_FORKNUM, segNo);

		if (strcmp(check_path, path) != 0)
			matched = false;

		pfree(check_path);
	}

	return matched;
}

/*
 * 创建关系文件和段路径的辅助函数。
 *
 * 返回的路径由 palloc 分配
                                                                  */
static char *
datasegpath(RelFileLocator rlocator, ForkNumber forknum, BlockNumber segno)
{
	char	   *path;
	char	   *segpath;

	path = relpathperm(rlocator, forknum);
	if (segno > 0)
	{
		segpath = psprintf("%s.%u", path, segno);
		pfree(path);
		return segpath;
	}
	else
		return path;
}

/*
 * 最后阶段，对文件映射排序，使删除操作在最后。
 * 从磁盘空间使用角度看，先删除更好，但目前安全
 * 优先。若整个目录被删除，其中的文件和子目录需
 * 先被删除。创建时需先创建父目录再创建其中的
 * 文件和目录。为此，file_action_t 枚举的排序
 * 使得只需先按它排序。此外，REMOVE 条目按路径
 * 反向排序，使 "foo/bar" 子目录在 "foo" 之前
 * 被删除。
                                                                                                                                                                                                                                                                                                                                                                                              */
static int
final_filemap_cmp(const void *a, const void *b)
{
	file_entry_t *fa = *((file_entry_t **) a);
	file_entry_t *fb = *((file_entry_t **) b);

	if (fa->action > fb->action)
		return 1;
	if (fa->action < fb->action)
		return -1;

	if (fa->action == FILE_ACTION_REMOVE)
		return strcmp(fb->path, fa->path);
	else
		return strcmp(fa->path, fb->path);
}

/*
 * 决定对文件执行的操作。
                              */
static file_action_t
decide_file_action(file_entry_t *entry)
{
	const char *path = entry->path;

	/*
	 * 不要触碰控制文件。它在复制所有其他文件之        
	 * 后特殊处理。  
	 */
	if (strcmp(path, "global/pg_control") == 0)
		return FILE_ACTION_NONE;

	/* 跳过 macOS 系统文件           */
	if (strstr(path, ".DS_Store") != NULL)
		return FILE_ACTION_NONE;

	/*
	 * 移除目标中所有匹配排除过滤的文件。           
	 */
	if (check_file_excluded(path, true))
	{
		if (entry->target_exists)
			return FILE_ACTION_REMOVE;
		else
			return FILE_ACTION_NONE;
	}

	/*
	 * 处理文件在某一系统中缺失的情况。               
	 */
	if (!entry->target_exists && entry->source_exists)
	{
		/*
		 * 源有目标无的文件，完整复制。（若为关系          
		 * 数据文件，回绕后的 WAL 重放会重新创建它         
		 * 但现在复制它也无害。）                 
		 */
		switch (entry->source_type)
		{
			case FILE_TYPE_DIRECTORY:
			case FILE_TYPE_SYMLINK:
				return FILE_ACTION_CREATE;
			case FILE_TYPE_REGULAR:
				return FILE_ACTION_COPY;
			case FILE_TYPE_UNDEFINED:
				pg_fatal("unknown file type for \"%s\"", entry->path);
				break;
		}
	}
	else if (entry->target_exists && !entry->source_exists)
	{
		/*
		 * 对于目标存在而源不存在的文件，查keepwal哈希 
		 * 表；其中列出的任何文件都不得移除。              
		 */
		if (keepwal_entry_exists(path))
		{
			pg_log_debug("Not removing file \"%s\" because it is required for recovery", path);
			return FILE_ACTION_NONE;
		}
		return FILE_ACTION_REMOVE;
	}
	else if (!entry->target_exists && !entry->source_exists)
	{
		/*
		 * 在两个服务器中都不存在。为什么会有条目？！ 
		 *              
		 */
		Assert(false);
		return FILE_ACTION_NONE;
	}

	/*
	 * 否则，文件在两个系统中都存在
	 */
	Assert(entry->target_exists && entry->source_exists);

	if (entry->source_type != entry->target_type)
	{
		/* 但是不同类型的对象。奇怪..                                 */
		pg_fatal("file \"%s\" is of different type in source and target", entry->path);
	}

	/*
	 * PG_VERSION 文件在两个系统应相同，但为防万一， 
	 * 避免覆盖它们。         
	 */
	if (pg_str_endswith(entry->path, "PG_VERSION"))
		return FILE_ACTION_NONE;

	switch (entry->source_type)
	{
		case FILE_TYPE_DIRECTORY:
			return FILE_ACTION_NONE;

		case FILE_TYPE_SYMLINK:

			/*
			 * XXX：是否应检查它指向相同的目标？     
			 */
			return FILE_ACTION_NONE;

		case FILE_TYPE_REGULAR:
			if (!entry->isrelfile)
			{
				/*
				 * 没有特殊处理的非数据文件。完整复制。 
				 *                      
				 */
				return FILE_ACTION_COPY;
			}
			else
			{
				/*
				 * 该数据文件在两个系统中都存在。
				 *
				 * 若目标端更大，我们可以截断它。源端     
				 * 也会有这次截断的 WAL 记录，WAL 重放    
				 * 最终也将截断目标，但                            
				 * 我们不妨现在就做。         
				 *
				 * 如果目标端更小，则意味着它曾在目标端  
				 * 被截断，或在源端变大，或兼有。如果它
				 * 在目标端被截断，则需从源系统复制缺失  
				 * 的尾部。如果它是在源系统中变大，源系统  
				 * 中会有新块的 WAL 记录，因此本不必       
				 * 在此复制它们。但我们不知道遇到的是     
				 * 哪种情形，而复制缺失的块并无害处，   
				 * 所以现在就把它                                   
				 * 复制过来。          
				 *
				 * 如果大小相同，这里什么都不做。目标端     
				 * 被修改的块将基于解析目标系统的 WAL 来   
				 * 复制，而源端修改过的块将在回绕后、重放  
				 * 源系统的 WAL 时得到更                           
				 * 新。   
				 */
				if (entry->target_size < entry->source_size)
					return FILE_ACTION_COPY_TAIL;
				else if (entry->target_size > entry->source_size)
					return FILE_ACTION_TRUNCATE;
				else
					return FILE_ACTION_NONE;
			}
			break;

		case FILE_TYPE_UNDEFINED:
			pg_fatal("unknown file type for \"%s\"", path);
			break;
	}

	/* 不可达         */
	pg_fatal("could not decide what to do with file \"%s\"", path);
}

/*
 * 决定对每个文件的处理。
 *
 * 返回 'filemap'，条目按其操作应执行的                   
 * 顺序排列。    
 */
filemap_t *
decide_file_actions(void)
{
	int			i;
	filehash_iterator it;
	file_entry_t *entry;
	filemap_t  *filemap;

	filehash_start_iterate(filehash, &it);
	while ((entry = filehash_iterate(filehash, &it)) != NULL)
	{
		entry->action = decide_file_action(entry);
	}

	/*
	 * 将哈希表转为数组，并按操作应执行的              
	 * 顺序排序。             
	 */
	filemap = pg_malloc(offsetof(filemap_t, entries) +
						filehash->members * sizeof(file_entry_t *));
	filemap->nentries = filehash->members;
	filehash_start_iterate(filehash, &it);
	i = 0;
	while ((entry = filehash_iterate(filehash, &it)) != NULL)
	{
		filemap->entries[i++] = entry;
	}

	qsort(&filemap->entries, filemap->nentries, sizeof(file_entry_t *),
		  final_filemap_cmp);

	return filemap;
}


/*
 * 文件映射哈希表的辅助函数。
                           */
static uint32
hash_string_pointer(const char *s)
{
	unsigned char *ss = (unsigned char *) s;

	return hash_bytes(ss, strlen(s));
}
