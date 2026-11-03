/*-------------------------------------------------------------------------
 *
 * file_ops.c
 *	  Helper functions for operating on files.
 *
 * Most of the functions in this file are helper functions for writing to
 * the target data directory. The functions check the --dry-run flag, and
 * do nothing if it's enabled. You should avoid accessing the target files
 * directly but if you do, make sure you honor the --dry-run mode!
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "file_ops.h"
#include "filemap.h"
#include "pg_rewind.h"

/*
 * 当前打开的目标文件。
                  */
static int	dstfd = -1;
static char dstpath[MAXPGPATH] = "";

static void create_target_dir(const char *path);
static void remove_target_dir(const char *path);
static void create_target_symlink(const char *path, const char *link);
static void remove_target_symlink(const char *path);

static void recurse_dir(const char *datadir, const char *parentpath,
						process_file_callback_t callback);

/*
 * 打开目标文件用于写入。如果 'trunc' 为 true 且
 * 文件已存在，则将其截断。
                                                           */
void
open_target_file(const char *path, bool trunc)
{
	int			mode;

	if (dry_run)
		return;

	if (dstfd != -1 && !trunc &&
		strcmp(path, &dstpath[strlen(datadir_target) + 1]) == 0)
		return;					/* already open */

	close_target_file();

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);

	mode = O_WRONLY | O_CREAT | PG_BINARY;
	if (trunc)
		mode |= O_TRUNC;
	dstfd = open(dstpath, mode, pg_file_create_mode);
	if (dstfd < 0)
		pg_fatal("could not open target file \"%s\": %m",
				 dstpath);
}

/*
 * 关闭目标文件（如果已打开）。
                   */
void
close_target_file(void)
{
	if (dstfd == -1)
		return;

	if (close(dstfd) != 0)
		pg_fatal("could not close target file \"%s\": %m",
				 dstpath);

	dstfd = -1;
}

void
write_target_range(char *buf, off_t begin, size_t size)
{
	size_t		writeleft;
	char	   *p;

	/* 更新进度报告                 */
	fetch_done += size;
	progress_report(false);

	if (dry_run)
		return;

	if (lseek(dstfd, begin, SEEK_SET) == -1)
		pg_fatal("could not seek in target file \"%s\": %m",
				 dstpath);

	writeleft = size;
	p = buf;
	while (writeleft > 0)
	{
		ssize_t		writelen;

		errno = 0;
		writelen = write(dstfd, p, writeleft);
		if (writelen < 0)
		{
			/* 如果 write 未设置 errno，假设问题是无磁盘空间                              */
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("could not write file \"%s\": %m",
					 dstpath);
		}

		p += writelen;
		writeleft -= writelen;
	}

	/* 保持文件打开，以防需要向其中复制更多块                                           */
}


void
remove_target(file_entry_t *entry)
{
	Assert(entry->action == FILE_ACTION_REMOVE);
	Assert(entry->target_exists);

	switch (entry->target_type)
	{
		case FILE_TYPE_DIRECTORY:
			remove_target_dir(entry->path);
			break;

		case FILE_TYPE_REGULAR:
			remove_target_file(entry->path, false);
			break;

		case FILE_TYPE_SYMLINK:
			remove_target_symlink(entry->path);
			break;

		case FILE_TYPE_UNDEFINED:
			pg_fatal("undefined file type for \"%s\"", entry->path);
			break;
	}
}

void
create_target(file_entry_t *entry)
{
	Assert(entry->action == FILE_ACTION_CREATE);
	Assert(!entry->target_exists);

	switch (entry->source_type)
	{
		case FILE_TYPE_DIRECTORY:
			create_target_dir(entry->path);
			break;

		case FILE_TYPE_SYMLINK:
			create_target_symlink(entry->path, entry->source_link_target);
			break;

		case FILE_TYPE_REGULAR:
			/* 不可能发生。普通文件由 open_target_file 创建。   */
			pg_fatal("invalid action (CREATE) for regular file");
			break;

		case FILE_TYPE_UNDEFINED:
			pg_fatal("undefined file type for \"%s\"", entry->path);
			break;
	}
}

/*
 * 从目标数据目录移除文件。若 missing_ok 为 true，
 * 目标文件不存在是允许的。
                                                                  */
void
remove_target_file(const char *path, bool missing_ok)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (unlink(dstpath) != 0)
	{
		if (errno == ENOENT && missing_ok)
			return;

		pg_fatal("could not remove file \"%s\": %m",
				 dstpath);
	}
}

void
truncate_target_file(const char *path, off_t newsize)
{
	char		dstpath[MAXPGPATH];
	int			fd;

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);

	fd = open(dstpath, O_WRONLY, pg_file_create_mode);
	if (fd < 0)
		pg_fatal("could not open file \"%s\" for truncation: %m",
				 dstpath);

	if (ftruncate(fd, newsize) != 0)
		pg_fatal("could not truncate file \"%s\" to %u: %m",
				 dstpath, (unsigned int) newsize);

	close(fd);
}

static void
create_target_dir(const char *path)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (mkdir(dstpath, pg_dir_create_mode) != 0)
		pg_fatal("could not create directory \"%s\": %m",
				 dstpath);
}

static void
remove_target_dir(const char *path)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (rmdir(dstpath) != 0)
		pg_fatal("could not remove directory \"%s\": %m",
				 dstpath);
}

static void
create_target_symlink(const char *path, const char *link)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (symlink(link, dstpath) != 0)
		pg_fatal("could not create symbolic link at \"%s\": %m",
				 dstpath);
}

static void
remove_target_symlink(const char *path)
{
	char		dstpath[MAXPGPATH];

	if (dry_run)
		return;

	snprintf(dstpath, sizeof(dstpath), "%s/%s", datadir_target, path);
	if (unlink(dstpath) != 0)
		pg_fatal("could not remove symbolic link \"%s\": %m",
				 dstpath);
}

/*
 * 同步目标数据目录，确保修改安全写入磁盘。
 *
 * 出于性能考虑，对整个数据目录执行一次此操作。
 * 在 pg_rewind 运行结束时，内核很可能已将大多数
 * 脏缓冲区刷新到磁盘。此外，fsync_pgdata 使用
 * 两遍方法（第一遍仅启动写回），通常能显著
 * 减少总体 IO 量。
                                                                                                                                                                                                                                                                                    */
void
sync_target_dir(void)
{
	if (!do_sync || dry_run)
		return;

	fsync_pgdata(datadir_target, PG_VERSION_NUM);
}


/*
 * 将文件读入内存。要读取的文件是 <datadir>/<path>。
 * 文件内容以 malloc 分配的缓冲区返回，*filesize
 * 设为文件长度。
 *
 * 返回的缓冲区始终以零结尾；实际大小为
 * *filesize + 1。读取文本文件时很方便。也可用于
 * 读取二进制文件，此时可忽略零终止符。
                                                                                                                                                                                                                                                                                            */
char *
slurpFile(const char *datadir, const char *path, size_t *filesize)
{
	int			fd;
	char	   *buffer;
	struct stat statbuf;
	char		fullpath[MAXPGPATH];
	int			len;
	int			r;

	snprintf(fullpath, sizeof(fullpath), "%s/%s", datadir, path);

	if ((fd = open(fullpath, O_RDONLY | PG_BINARY, 0)) == -1)
		pg_fatal("could not open file \"%s\" for reading: %m",
				 fullpath);

	if (fstat(fd, &statbuf) < 0)
		pg_fatal("could not open file \"%s\" for reading: %m",
				 fullpath);

	len = statbuf.st_size;

	buffer = pg_malloc(len + 1);

	r = read(fd, buffer, len);
	if (r != len)
	{
		if (r < 0)
			pg_fatal("could not read file \"%s\": %m",
					 fullpath);
		else
			pg_fatal("could not read file \"%s\": read %d of %zu",
					 fullpath, r, (Size) len);
	}
	close(fd);

	/* 对缓冲区进行零终止。                 */
	buffer[len] = '\0';

	if (filesize)
		*filesize = len;
	return buffer;
}

/*
 * 遍历数据目录中的所有文件，对每个文件调用
 * 'callback'。
                                                  */
void
traverse_datadir(const char *datadir, process_file_callback_t callback)
{
	recurse_dir(datadir, NULL, callback);
}

/*
 * traverse_datadir 的递归部分
 *
 * parentpath 是当前子目录相对于 datadir 的路径，
 * 顶层时为 NULL。
                                                             */
static void
recurse_dir(const char *datadir, const char *parentpath,
			process_file_callback_t callback)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fullparentpath[MAXPGPATH];

	if (parentpath)
		snprintf(fullparentpath, MAXPGPATH, "%s/%s", datadir, parentpath);
	else
		snprintf(fullparentpath, MAXPGPATH, "%s", datadir);

	xldir = opendir(fullparentpath);
	if (xldir == NULL)
		pg_fatal("could not open directory \"%s\": %m",
				 fullparentpath);

	while (errno = 0, (xlde = readdir(xldir)) != NULL)
	{
		struct stat fst;
		char		fullpath[MAXPGPATH * 2];
		char		path[MAXPGPATH * 2];

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fullpath, sizeof(fullpath), "%s/%s", fullparentpath, xlde->d_name);

		if (lstat(fullpath, &fst) < 0)
		{
			if (errno == ENOENT)
			{
				/*
				 * 文件消失。若新主在跑且刚移除，没问题。 
				 * 则正常。若是数据文件，移除应有WAL记录。 
				 * 若是其他类型，本就无关紧要。              
				 * 若非数据文件，本不可能有影响。    
				 *
				 * TODO: 但处理目标目录时应报错！            
				 */
			}
			else
				pg_fatal("could not stat file \"%s\": %m",
						 fullpath);
		}

		if (parentpath)
			snprintf(path, sizeof(path), "%s/%s", parentpath, xlde->d_name);
		else
			snprintf(path, sizeof(path), "%s", xlde->d_name);

		if (S_ISREG(fst.st_mode))
			callback(path, FILE_TYPE_REGULAR, fst.st_size, NULL);
		else if (S_ISDIR(fst.st_mode))
		{
			callback(path, FILE_TYPE_DIRECTORY, 0, NULL);
			/* 递归处理子目录                          */
			recurse_dir(datadir, path, callback);
		}
		else if (S_ISLNK(fst.st_mode))
		{
			char		link_target[MAXPGPATH];
			int			len;

			len = readlink(fullpath, link_target, sizeof(link_target));
			if (len < 0)
				pg_fatal("could not read symbolic link \"%s\": %m",
						 fullpath);
			if (len >= sizeof(link_target))
				pg_fatal("symbolic link \"%s\" target is too long",
						 fullpath);
			link_target[len] = '\0';

			callback(path, FILE_TYPE_SYMLINK, 0, link_target);

			/*
			 * 若是 pg_tblspc 内符号链接，                            
			 * 需递归处理所有表空间。                           
			 * pg_wal 符号链接跟随，余忽略。         
			 */
			if ((parentpath && strcmp(parentpath, "pg_tblspc") == 0) ||
				strcmp(path, "pg_wal") == 0)
				recurse_dir(datadir, path, callback);
		}
	}

	if (errno)
		pg_fatal("could not read directory \"%s\": %m",
				 fullparentpath);

	if (closedir(xldir))
		pg_fatal("could not close directory \"%s\": %m",
				 fullparentpath);
}
