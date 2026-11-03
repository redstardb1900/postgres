/*-------------------------------------------------------------------------
 *
 * bbstreamer_file.c
 *
 * 部分版权 (c) 1996-2023, PostgreSQL 全球开发组
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_file.c
 *-------------------------------------------------------------------------
                */

#include "postgres_fe.h"

#include <unistd.h>

#include "bbstreamer.h"
#include "common/logging.h"
#include "common/file_perm.h"
#include "common/string.h"

typedef struct bbstreamer_plain_writer
{
	bbstreamer	base;
	char	   *pathname;
	FILE	   *file;
	bool		should_close_file;
} bbstreamer_plain_writer;

typedef struct bbstreamer_extractor
{
	bbstreamer	base;
	char	   *basepath;
	const char *(*link_map) (const char *);
	void		(*report_output_file) (const char *);
	char		filename[MAXPGPATH];
	FILE	   *file;
} bbstreamer_extractor;

static void bbstreamer_plain_writer_content(bbstreamer *streamer,
											bbstreamer_member *member,
											const char *data, int len,
											bbstreamer_archive_context context);
static void bbstreamer_plain_writer_finalize(bbstreamer *streamer);
static void bbstreamer_plain_writer_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_plain_writer_ops = {
	.content = bbstreamer_plain_writer_content,
	.finalize = bbstreamer_plain_writer_finalize,
	.free = bbstreamer_plain_writer_free
};

static void bbstreamer_extractor_content(bbstreamer *streamer,
										 bbstreamer_member *member,
										 const char *data, int len,
										 bbstreamer_archive_context context);
static void bbstreamer_extractor_finalize(bbstreamer *streamer);
static void bbstreamer_extractor_free(bbstreamer *streamer);
static void extract_directory(const char *filename, mode_t mode);
static void extract_link(const char *filename, const char *linktarget);
static FILE *create_file_for_extract(const char *filename, mode_t mode);

const bbstreamer_ops bbstreamer_extractor_ops = {
	.content = bbstreamer_extractor_content,
	.finalize = bbstreamer_extractor_finalize,
	.free = bbstreamer_extractor_free
};

/*
 * 创建一个仅将数据写入文件的 bbstreamer。
 *
 * 调用者必须指定路径名，也可以指定文件。路径名在两种情况下都用于错误报告。
 * 如果 file 为 NULL，则路径名同时也标识数据应写入的文件：该文件将被打开、
 * 写入并在完成时关闭。如果 file 不为 NULL，数据将写入那里。
                      */
bbstreamer *
bbstreamer_plain_writer_new(char *pathname, FILE *file)
{
	bbstreamer_plain_writer *streamer;

	streamer = palloc0(sizeof(bbstreamer_plain_writer));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_plain_writer_ops;

	streamer->pathname = pstrdup(pathname);
	streamer->file = file;

	if (file == NULL)
	{
		streamer->file = fopen(pathname, "wb");
		if (streamer->file == NULL)
			pg_fatal("could not create file \"%s\": %m", pathname);
		streamer->should_close_file = true;
	}

	return &streamer->base;
}

/*
 * 将归档内容写入文件。
 */
static void
bbstreamer_plain_writer_content(bbstreamer *streamer,
								bbstreamer_member *member, const char *data,
								int len, bbstreamer_archive_context context)
{
	bbstreamer_plain_writer *mystreamer;

	mystreamer = (bbstreamer_plain_writer *) streamer;

	if (len == 0)
		return;

	errno = 0;
	if (fwrite(data, len, 1, mystreamer->file) != 1)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to file \"%s\": %m",
				 mystreamer->pathname);
	}
}

/*
 * End-of-archive processing when writing to a plain file consists of closing
 * the file if we opened it, but not if the caller provided it.
 */
static void
bbstreamer_plain_writer_finalize(bbstreamer *streamer)
{
	bbstreamer_plain_writer *mystreamer;

	mystreamer = (bbstreamer_plain_writer *) streamer;

	if (mystreamer->should_close_file && fclose(mystreamer->file) != 0)
		pg_fatal("could not close file \"%s\": %m",
				 mystreamer->pathname);

	mystreamer->file = NULL;
	mystreamer->should_close_file = false;
}

/*
 * 释放与此 bbstreamer 关联的内存。
   */
static void
bbstreamer_plain_writer_free(bbstreamer *streamer)
{
	bbstreamer_plain_writer *mystreamer;

	mystreamer = (bbstreamer_plain_writer *) streamer;

	Assert(!mystreamer->should_close_file);
	Assert(mystreamer->base.bbs_next == NULL);

	pfree(mystreamer->pathname);
	pfree(mystreamer);
}

/*
 * 创建一个解压归档文件的 bbstreamer。
 *
 * 归档中的所有路径名都是相对于 basepath 解析的。
 *
 * 与 bbstreamer_plain_writer_new() 不同，这里无法对无类型块做任何有用的
 * 事；我们需要遵循 bbstreamer.h 中描述的规则的类型化块。假设有了这些，
 * 我们就不需要担心原始归档格式；只需查看提供的成员信息并写入相应文件即可。
 *
 * 'link_map' 是一个函数，它将应用于任何符号链接的目标，并应返回一个
 * 替换路径名以供使用。如果为 NULL，符号链接目标将不经过修改直接使用。
 *
 * 'report_output_file' 是一个函数，每次打开新的输出文件时都会调用它。
 * 该文件的路径名作为参数传入。如果为 NULL，则跳过该调用。
                                                                                                */
bbstreamer *
bbstreamer_extractor_new(const char *basepath,
						 const char *(*link_map) (const char *),
						 void (*report_output_file) (const char *))
{
	bbstreamer_extractor *streamer;

	streamer = palloc0(sizeof(bbstreamer_extractor));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_extractor_ops;
	streamer->basepath = pstrdup(basepath);
	streamer->link_map = link_map;
	streamer->report_output_file = report_output_file;

	return &streamer->base;
}

/*
 * 将归档内容解压到文件系统。
     */
static void
bbstreamer_extractor_content(bbstreamer *streamer, bbstreamer_member *member,
							 const char *data, int len,
							 bbstreamer_archive_context context)
{
	bbstreamer_extractor *mystreamer = (bbstreamer_extractor *) streamer;
	int			fnamelen;

	Assert(member != NULL || context == BBSTREAMER_ARCHIVE_TRAILER);
	Assert(context != BBSTREAMER_UNKNOWN);

	switch (context)
	{
		case BBSTREAMER_MEMBER_HEADER:
			Assert(mystreamer->file == NULL);

			/* Prepend basepath. */
			snprintf(mystreamer->filename, sizeof(mystreamer->filename),
					 "%s/%s", mystreamer->basepath, member->pathname);

			/* 移除尾部的斜杠。   */
			fnamelen = strlen(mystreamer->filename);
			if (mystreamer->filename[fnamelen - 1] == '/')
				mystreamer->filename[fnamelen - 1] = '\0';

			/* Dispatch based on file type. */
			if (member->is_directory)
				extract_directory(mystreamer->filename, member->mode);
			else if (member->is_link)
			{
				const char *linktarget = member->linktarget;

				if (mystreamer->link_map)
					linktarget = mystreamer->link_map(linktarget);
				extract_link(mystreamer->filename, linktarget);
			}
			else
				mystreamer->file =
					create_file_for_extract(mystreamer->filename,
											member->mode);

			/* Report output file change. */
			if (mystreamer->report_output_file)
				mystreamer->report_output_file(mystreamer->filename);
			break;

		case BBSTREAMER_MEMBER_CONTENTS:
			if (mystreamer->file == NULL)
				break;

			errno = 0;
			if (len > 0 && fwrite(data, len, 1, mystreamer->file) != 1)
			{
				/* if write didn't set errno, assume problem is no disk space */
				if (errno == 0)
					errno = ENOSPC;
				pg_fatal("could not write to file \"%s\": %m",
						 mystreamer->filename);
			}
			break;

		case BBSTREAMER_MEMBER_TRAILER:
			if (mystreamer->file == NULL)
				break;
			fclose(mystreamer->file);
			mystreamer->file = NULL;
			break;

		case BBSTREAMER_ARCHIVE_TRAILER:
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while extracting archive");
	}
}

/*
 * 我们应该容忍已存在的目录吗？
 *
 * 在流式传输 WAL 时，pg_wal（或 pre-9.6 集群的 pg_xlog）将由 WAL 接收器
 * 进程创建。此外，当指定了 WAL 目录位置时，pg_wal（或 pg_xlog）在开始实际
 * 备份之前已经作为符号链接创建。因此只需忽略相关目录的创建失败。
 *
 * 如果使用了原位表空间，pg_tblspc 和子目录在到达这里时可能已存在。
 * 所以也容忍这种情况。
                                                             */
static bool
should_allow_existing_directory(const char *pathname)
{
	const char *filename = last_dir_separator(pathname) + 1;

	if (strcmp(filename, "pg_wal") == 0 ||
		strcmp(filename, "pg_xlog") == 0 ||
		strcmp(filename, "archive_status") == 0 ||
		strcmp(filename, "pg_tblspc") == 0)
		return true;

	if (strspn(filename, "0123456789") == strlen(filename))
	{
		const char *pg_tblspc = strstr(pathname, "/pg_tblspc/");

		return pg_tblspc != NULL && pg_tblspc + 11 == filename;
	}

	return false;
}

/*
 * 创建目录。
     */
static void
extract_directory(const char *filename, mode_t mode)
{
	if (mkdir(filename, pg_dir_create_mode) != 0 &&
		(errno != EEXIST || !should_allow_existing_directory(filename)))
		pg_fatal("could not create directory \"%s\": %m",
				 filename);

#ifndef WIN32
	if (chmod(filename, mode))
		pg_fatal("could not set permissions on directory \"%s\": %m",
				 filename);
#endif
}

/*
 * 创建符号链接。
 *
 * 很可能是 pg_tblspc 目录中的一个链接，指向表空间的位置。应用命令行给出的
 * 任何表空间映射（--tablespace-mapping）。（我们盲目地应用映射，不检查链接
 * 是否确实在 pg_tblspc 中。我们不期望数据目录中有其他符号链接，但如果有，
 * 你可以称其为无文档功能，表示你也可以映射它们。）
                                */
static void
extract_link(const char *filename, const char *linktarget)
{
	if (symlink(linktarget, filename) != 0)
		pg_fatal("could not create symbolic link from \"%s\" to \"%s\": %m",
				 filename, linktarget);
}

/*
 * 创建常规文件。
 *
 * 返回结果句柄，以便将内容写入文件。
                   */
static FILE *
create_file_for_extract(const char *filename, mode_t mode)
{
	FILE	   *file;

	file = fopen(filename, "wb");
	if (file == NULL)
		pg_fatal("could not create file \"%s\": %m", filename);

#ifndef WIN32
	if (chmod(filename, mode))
		pg_fatal("could not set permissions on file \"%s\": %m",
				 filename);
#endif

	return file;
}

/*
 * End-of-stream processing for extracting an archive.
 *
 * There's nothing to do here but sanity checking.
 */
static void
bbstreamer_extractor_finalize(bbstreamer *streamer)
{
	bbstreamer_extractor *mystreamer PG_USED_FOR_ASSERTS_ONLY
	= (bbstreamer_extractor *) streamer;

	Assert(mystreamer->file == NULL);
}

/*
 * Free memory.
 */
static void
bbstreamer_extractor_free(bbstreamer *streamer)
{
	bbstreamer_extractor *mystreamer = (bbstreamer_extractor *) streamer;

	pfree(mystreamer->basepath);
	pfree(mystreamer);
}
