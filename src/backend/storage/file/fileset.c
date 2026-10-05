/*-------------------------------------------------------------------------
 *
 * fileset.c
 *	  Management of named temporary files.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fileset.c
 *
 * FileSets provide a temporary namespace (think directory) so that files can
 * be discovered by name.
 *
 * FileSets can be used by backends when the temporary files need to be
 * opened/closed multiple times and the underlying files need to survive across
 * transactions.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/fileset.h"
#include "utils/builtins.h"

static void FileSetPath(char *path, FileSet *fileset, Oid tablespace);
static void FilePath(char *path, FileSet *fileset, const char *name);
static Oid	ChooseTablespace(const FileSet *fileset, const char *name);

/*
 * 为临时文件初始化一块空间。这个 API 既可用于共享
 * 文件集，也可用于仅由单个后端使用的临时文件，但这些文件得多次
 * 打开和关闭，而且底层文件得跨事务
 * 存活。
 *
 * 预期调用者会用
 * FileSetDelete/FileSetDeleteAll 显式去掉这些文件。
 *
 * 文件会分布到 temp_tablespaces
 * 所配置的表空间上。
 *
 * 在内部，这个集合是一个或多个目录，最终
 * 会被删除。
 */
void
FileSetInit(FileSet *fileset)
{
	static uint32 counter = 0;

	fileset->creator_pid = MyProcPid;
	fileset->number = counter;
	counter = (counter + 1) % INT_MAX;

	/* 记下表空间 OIDs，使所有后端对它们达成一致。 */
	PrepareTempTablespaces();
	fileset->ntablespaces =
		GetTempTablespaces(&fileset->tablespaces[0],
						   lengthof(fileset->tablespaces));
	if (fileset->ntablespaces == 0)
	{
		/* 若该 GUC 为空，则使用当前数据库的默认表空间 */
		fileset->tablespaces[0] = MyDatabaseTableSpace;
		fileset->ntablespaces = 1;
	}
	else
	{
		int			i;

		/*
		 * InvalidOid 项表示使用当前数据库的默认表空间。
		 * 现在就替换它，以确保该 FileSet 的所有使用者
		 * 对做法达成一致。
		 */
		for (i = 0; i < fileset->ntablespaces; i++)
		{
			if (fileset->tablespaces[i] == InvalidOid)
				fileset->tablespaces[i] = MyDatabaseTableSpace;
		}
	}
}

/*
 * 在给定集合里创建一个新文件。
 */
File
FileSetCreate(FileSet *fileset, const char *name)
{
	char		path[MAXPGPATH];
	File		file;

	FilePath(path, fileset, name);
	file = PathNameCreateTemporaryFile(path, false);

	/* 若失败了，看看是否需要按需创建目录。 */
	if (file <= 0)
	{
		char		tempdirpath[MAXPGPATH];
		char		filesetpath[MAXPGPATH];
		Oid			tablespace = ChooseTablespace(fileset, name);

		TempTablespacePath(tempdirpath, tablespace);
		FileSetPath(filesetpath, fileset, tablespace);
		PathNameCreateTemporaryDir(tempdirpath, filesetpath);
		file = PathNameCreateTemporaryFile(path, true);
	}

	return file;
}

/*
 * 打开一个用 FileSetCreate() 创建的文件。 */
File
FileSetOpen(FileSet *fileset, const char *name, int mode)
{
	char		path[MAXPGPATH];
	File		file;

	FilePath(path, fileset, name);
	file = PathNameOpenTemporaryFile(path, mode);

	return file;
}

/*
 * 删除一个用 FileSetCreate() 创建的文件。
 *
 * 若文件存在则返回真，若不存在则返回假。
 */
bool
FileSetDelete(FileSet *fileset, const char *name,
			  bool error_on_failure)
{
	char		path[MAXPGPATH];

	FilePath(path, fileset, name);

	return PathNameDeleteTemporaryFile(path, error_on_failure);
}

/*
 * 删除集合里的所有文件。
 */
void
FileSetDeleteAll(FileSet *fileset)
{
	char		dirpath[MAXPGPATH];
	int			i;

	/*
	 * 删除我们在每个表空间里创建的目录。这里不因失败而中断，
	 * 因为我们在错误清理路径上使用它，但发生 IO 错误时可以产生 LOG
	 * 消息。
	 */
	for (i = 0; i < fileset->ntablespaces; ++i)
	{
		FileSetPath(dirpath, fileset, fileset->tablespaces[i]);
		PathNameDeleteTemporaryDir(dirpath);
	}
}

/*
 * 构造在给定表空间里支撑 FileSet 的文件所在目录的
 * 路径。
 */
static void
FileSetPath(char *path, FileSet *fileset, Oid tablespace)
{
	char		tempdirpath[MAXPGPATH];

	TempTablespacePath(tempdirpath, tablespace);
	snprintf(path, MAXPGPATH, "%s/%s%lu.%u.fileset",
			 tempdirpath, PG_TEMP_FILE_PREFIX,
			 (unsigned long) fileset->creator_pid, fileset->number);
}

/*
 * 排序必须确定给定临时文件属于哪个表空间。
 */
static Oid
ChooseTablespace(const FileSet *fileset, const char *name)
{
	uint32		hash = hash_any((const unsigned char *) name, strlen(name));

	return fileset->tablespaces[hash % fileset->ntablespaces];
}

/*
 * 计算 FileSet 里一个文件的完整路径。
 */
static void
FilePath(char *path, FileSet *fileset, const char *name)
{
	char		dirpath[MAXPGPATH];

	FileSetPath(dirpath, fileset, ChooseTablespace(fileset, name));
	snprintf(path, MAXPGPATH, "%s/%s", dirpath, name);
}
