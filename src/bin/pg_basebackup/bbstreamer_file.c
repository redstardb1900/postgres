/*-------------------------------------------------------------------------
 *
 * bbstreamer_file.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
 * 创建仅将数据写入文件的 bbstreamer。     
 *
 * 调用者必须指定路径名，也可指定文件。                    
 * 无论哪种方式，路径名都用于错误报告。                     
 * 若 file 为 NULL，路径名还标识数据要写入的文件：         
 * 该文件打开写入，完成时关闭；否则，数据写入该处。  
 *       
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
		/* 若写入未设置 errno，则假定为磁盘空间不足  */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to file \"%s\": %m",
				 mystreamer->pathname);
	}
}

/*
 * 写入普通文件时，归档结束处理是在我们自己打开文件  
 * 时关闭它，而由调用者提供时则不关闭。      
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
 * 创建一个提取归档的 bbstreamer。    
 *
 * 归档中的所有路径名都是相对于 basepath 解析的。  
 *
 * 与 bbstreamer_plain_writer_new() 不同，这里无法对无类型块    
 * 做任何有用的事；我们需要遵循 bbstreamer.h 中描述的      
 * 规则的类型化块。若已有这些，就无需担心原始归档   
 * 格式；只需查看所提供的成员信息并写入相应文件即可。
 *                                              
 *
 * 'link_map' 是一个函数，应用于任何符号链接目标， 
 * 并应返回一个替换路径名以供其使用。如果为 NULL，    
 * 符号链接目标将不经过修改直接使用。             
 *              
 *
 * 'report_output_file' 是一个函数，每次我们打开新输出        
 * 文件时都会调用它。路径名作为参数传入。若为 NULL，
 * 则跳过该调用。     
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

			/* 拼接 basepath   */
			snprintf(mystreamer->filename, sizeof(mystreamer->filename),
					 "%s/%s", mystreamer->basepath, member->pathname);

			/* 移除尾部的斜杠。   */
			fnamelen = strlen(mystreamer->filename);
			if (mystreamer->filename[fnamelen - 1] == '/')
				mystreamer->filename[fnamelen - 1] = '\0';

			/* 按文件类型分发。     */
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

			/* 报告输出文件变动   */
			if (mystreamer->report_output_file)
				mystreamer->report_output_file(mystreamer->filename);
			break;

		case BBSTREAMER_MEMBER_CONTENTS:
			if (mystreamer->file == NULL)
				break;

			errno = 0;
			if (len > 0 && fwrite(data, len, 1, mystreamer->file) != 1)
			{
				/* 若写入未设置 errno，则假定为磁盘空间不足  */
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
			/* 不应发生。   */
			pg_fatal("unexpected state while extracting archive");
	}
}

/*
 * 我们应该容忍已存在的目录吗？       
 *
 * 流式传输 WAL 时，pg_wal（pre-9.6 集簇中为 pg_xlog）已         
 * 由 WAL 接收器进程创建。另外，指定 WAL 目录位置时，    
 * pg_wal（或 pg_xlog）已在实际备份开始之前作为符号链接  
 * 形式创建。因此只需忽略相关目录的创建失败。          
 *                        
 *
 * 若使用原位表空间，则 pg_tblspc 和子目录在我们到达这里
 * 时可能已存在。这种情况也要容忍。   
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
 * 很可能是 pg_tblspc 目录中指向表空间位置的链接。   
 * 应用命令行给出的任何表空间映射                     
 * （--tablespace-mapping）。（我们盲目地应用映射，不检查    
 * 该链接是否确在 pg_tblspc 中。我们不期望数据目录中  
 * 还有其他符号链接，但若有，可称为无文档的特性
 * 即你也可以映射它们。）               
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
 * 提取归档的流结束处理。                  
 *
 * 这里除了健全性检查外无事可做。  
 */
static void
bbstreamer_extractor_finalize(bbstreamer *streamer)
{
	bbstreamer_extractor *mystreamer PG_USED_FOR_ASSERTS_ONLY
	= (bbstreamer_extractor *) streamer;

	Assert(mystreamer->file == NULL);
}

/*
 * 释放内存
 */
static void
bbstreamer_extractor_free(bbstreamer *streamer)
{
	bbstreamer_extractor *mystreamer = (bbstreamer_extractor *) streamer;

	pfree(mystreamer->basepath);
	pfree(mystreamer);
}
