/*-------------------------------------------------------------------------
 *
 * local_source.c
 *	  Functions for using a local data directory as the source.
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <unistd.h>

#include "datapagemap.h"
#include "file_ops.h"
#include "filemap.h"
#include "pg_rewind.h"
#include "rewind_source.h"

typedef struct
{
	rewind_source common;		/* common interface functions */

	const char *datadir;		/* 指向源数据目录的路径    */
} local_source;

static void local_traverse_files(rewind_source *source,
								 process_file_callback_t callback);
static char *local_fetch_file(rewind_source *source, const char *path,
							  size_t *filesize);
static void local_queue_fetch_file(rewind_source *source, const char *path,
								   size_t len);
static void local_queue_fetch_range(rewind_source *source, const char *path,
									off_t off, size_t len);
static void local_finish_fetch(rewind_source *source);
static void local_destroy(rewind_source *source);

rewind_source *
init_local_source(const char *datadir)
{
	local_source *src;

	src = pg_malloc0(sizeof(local_source));

	src->common.traverse_files = local_traverse_files;
	src->common.fetch_file = local_fetch_file;
	src->common.queue_fetch_file = local_queue_fetch_file;
	src->common.queue_fetch_range = local_queue_fetch_range;
	src->common.finish_fetch = local_finish_fetch;
	src->common.get_current_wal_insert_lsn = NULL;
	src->common.destroy = local_destroy;

	src->datadir = datadir;

	return &src->common;
}

static void
local_traverse_files(rewind_source *source, process_file_callback_t callback)
{
	traverse_datadir(((local_source *) source)->datadir, callback);
}

static char *
local_fetch_file(rewind_source *source, const char *path, size_t *filesize)
{
	return slurpFile(((local_source *) source)->datadir, path, filesize);
}

/*
 * 将文件从源复制到目标。
 *
 * 'len' 是文件的预期长度。
                                                  */
static void
local_queue_fetch_file(rewind_source *source, const char *path, size_t len)
{
	const char *datadir = ((local_source *) source)->datadir;
	PGIOAlignedBlock buf;
	char		srcpath[MAXPGPATH];
	int			srcfd;
	size_t		written_len;

	snprintf(srcpath, sizeof(srcpath), "%s/%s", datadir, path);

	/* 打开源文件进行读取                    */
	srcfd = open(srcpath, O_RDONLY | PG_BINARY, 0);
	if (srcfd < 0)
		pg_fatal("could not open source file \"%s\": %m",
				 srcpath);

	/* 截断并打开目标文件进行写入                                 */
	open_target_file(path, true);

	written_len = 0;
	for (;;)
	{
		ssize_t		read_len;

		read_len = read(srcfd, buf.data, sizeof(buf));

		if (read_len < 0)
			pg_fatal("could not read file \"%s\": %m", srcpath);
		else if (read_len == 0)
			break;				/* EOF reached */

		write_target_range(buf.data, written_len, read_len);
		written_len += read_len;
	}

	/*
	 * 本地源在回绕期间不应变化，                           
	 * 因此检查文件大小是否与预期一致。                
	 */
	if (written_len != len)
		pg_fatal("size of source file \"%s\" changed concurrently: %d bytes expected, %d copied",
				 srcpath, (int) len, (int) written_len);

	if (close(srcfd) != 0)
		pg_fatal("could not close file \"%s\": %m", srcpath);
}

/*
 * 将文件从源复制到目标，从 'off' 开始，共
 * 'len' 字节。
                                   */
static void
local_queue_fetch_range(rewind_source *source, const char *path, off_t off,
						size_t len)
{
	const char *datadir = ((local_source *) source)->datadir;
	PGIOAlignedBlock buf;
	char		srcpath[MAXPGPATH];
	int			srcfd;
	off_t		begin = off;
	off_t		end = off + len;

	snprintf(srcpath, sizeof(srcpath), "%s/%s", datadir, path);

	srcfd = open(srcpath, O_RDONLY | PG_BINARY, 0);
	if (srcfd < 0)
		pg_fatal("could not open source file \"%s\": %m",
				 srcpath);

	if (lseek(srcfd, begin, SEEK_SET) == -1)
		pg_fatal("could not seek in source file: %m");

	open_target_file(path, false);

	while (end - begin > 0)
	{
		ssize_t		readlen;
		size_t		thislen;

		if (end - begin > sizeof(buf))
			thislen = sizeof(buf);
		else
			thislen = end - begin;

		readlen = read(srcfd, buf.data, thislen);

		if (readlen < 0)
			pg_fatal("could not read file \"%s\": %m", srcpath);
		else if (readlen == 0)
			pg_fatal("unexpected EOF while reading file \"%s\"", srcpath);

		write_target_range(buf.data, begin, readlen);
		begin += readlen;
	}

	if (close(srcfd) != 0)
		pg_fatal("could not close file \"%s\": %m", srcpath);
}

static void
local_finish_fetch(rewind_source *source)
{
	/*
	 * 无需操作，local_queue_fetch_range() 立即复制
	 * 范围。
	                             */
}

static void
local_destroy(rewind_source *source)
{
	pfree(source);
}
