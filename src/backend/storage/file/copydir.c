/*-------------------------------------------------------------------------
 *
 * copydir.c
 *	  copies a directory
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	While "xcopy /e /i /q" works fine for copying directories, on Windows XP
 *	it requires a Window handle which prevents it from working when invoked
 *	as a service.
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/copydir.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/copydir.h"
#include "storage/fd.h"

/*
 * copydir：复制一个目录。
 *
 * 若 recurse 为假，则忽略子目录。凡不是
 * 目录或普通文件的都忽略。
 */
void
copydir(const char *fromdir, const char *todir, bool recurse)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfile[MAXPGPATH * 2];
	char		tofile[MAXPGPATH * 2];

	if (MakePGDirectory(todir) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", todir)));

	xldir = AllocateDir(fromdir);

	while ((xlde = ReadDir(xldir, fromdir)) != NULL)
	{
		PGFileType	xlde_type;

		/* 若在复制目录期间收到取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fromfile, sizeof(fromfile), "%s/%s", fromdir, xlde->d_name);
		snprintf(tofile, sizeof(tofile), "%s/%s", todir, xlde->d_name);

		xlde_type = get_dirent_type(fromfile, xlde, false, ERROR);

		if (xlde_type == PGFILETYPE_DIR)
		{
			/* 递归处理子目录 */
			if (recurse)
				copydir(fromfile, tofile, true);
		}
		else if (xlde_type == PGFILETYPE_REG)
			copy_file(fromfile, tofile);
	}
	FreeDir(xldir);

	/*
	 * 这里要谨慎，对所有文件做 fsync，以确保复制真正完成。
	 * 但若禁用了 fsync，就到此为止。
	 */
	if (!enableFsync)
		return;

	xldir = AllocateDir(todir);

	while ((xlde = ReadDir(xldir, todir)) != NULL)
	{
		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(tofile, sizeof(tofile), "%s/%s", todir, xlde->d_name);

		/*
		 * 这里不需要同步子目录，因为递归的
		 * copydir 会在返回前做这件事
		 */
		if (get_dirent_type(tofile, xlde, false, ERROR) == PGFILETYPE_REG)
			fsync_fname(tofile, false);
	}
	FreeDir(xldir);

	/*
	 * 对目标目录本身做 fsync 很重要，因为对单个
	 * 文件做 fsync 并不保证该文件的目录项已经
	 * 同步。较新的 ext4 把这个窗口拉得宽得多，但
	 * 对 ext3 和过去的其他文件系统也一直如此。
	 */
	fsync_fname(todir, true);
}

/*
 * 复制一个文件
 */
void
copy_file(const char *fromfile, const char *tofile)
{
	char	   *buffer;
	int			srcfd;
	int			dstfd;
	int			nbytes;
	off_t		offset;
	off_t		flush_offset;

	/* 复制缓冲区的大小（读请求和写请求） */
#define COPY_BUF_SIZE (8 * BLCKSZ)

	/*
	 * 数据刷出请求的大小。在多数平台上大约每
	 * 1MB 做一次似乎有好处。但 macOS，至少在 APFS 的早期版本上，
	 * 对小的 mmap/msync 请求非常不友好，因此那里只
	 * 每 32MB 做一次。
	 */
#if defined(__darwin__)
#define FLUSH_DISTANCE (32 * 1024 * 1024)
#else
#define FLUSH_DISTANCE (1024 * 1024)
#endif

	/* 用 palloc 以确保得到最大对齐的缓冲区 */
	buffer = palloc(COPY_BUF_SIZE);

	/*
	 * 打开这些文件
	 */
	srcfd = OpenTransientFile(fromfile, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fromfile)));

	dstfd = OpenTransientFile(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tofile)));

	/*
	 * 进行数据复制。
	 */
	flush_offset = 0;
	for (offset = 0;; offset += nbytes)
	{
		/* 若在复制文件期间收到取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 文件稍后会 fsync，但在复制期间每隔一段时间刷出，
		 * 以免冲击缓存，并希望内核在 fsync 到来之前
		 * 就开始把它们写出去。
		 */
		if (offset - flush_offset >= FLUSH_DISTANCE)
		{
			pg_flush_data(dstfd, flush_offset, offset - flush_offset);
			flush_offset = offset;
		}

		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_READ);
		nbytes = read(srcfd, buffer, COPY_BUF_SIZE);
		pgstat_report_wait_end();
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", fromfile)));
		if (nbytes == 0)
			break;
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_WRITE);
		if ((int) write(dstfd, buffer, nbytes) != nbytes)
		{
			/* 若写操作没有设置 errno，就假定问题是磁盘空间不足 */
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tofile)));
		}
		pgstat_report_wait_end();
	}

	if (offset > flush_offset)
		pg_flush_data(dstfd, flush_offset, offset - flush_offset);

	if (CloseTransientFile(dstfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tofile)));

	if (CloseTransientFile(srcfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fromfile)));

	pfree(buffer);
}
