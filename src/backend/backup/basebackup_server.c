/*-------------------------------------------------------------------------
 *
 * basebackup_server.c
 *	  在服务器上存储基备份归档
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_server.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "backup/basebackup.h"
#include "backup/basebackup_sink.h"
#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

typedef struct bbsink_server
{
	/* 各类 sink 的公共信息。 */
	bbsink		base;

	/* 存放备份的目录。 */
	char	   *pathname;

	/* 当前打开文件（无则 0）。 */
	File		file;

	/* 当前文件位置。 */
	off_t		filepos;
} bbsink_server;

static void bbsink_server_begin_archive(bbsink *sink,
										const char *archive_name);
static void bbsink_server_archive_contents(bbsink *sink, size_t len);
static void bbsink_server_end_archive(bbsink *sink);
static void bbsink_server_begin_manifest(bbsink *sink);
static void bbsink_server_manifest_contents(bbsink *sink, size_t len);
static void bbsink_server_end_manifest(bbsink *sink);

static const bbsink_ops bbsink_server_ops = {
	.begin_backup = bbsink_forward_begin_backup,
	.begin_archive = bbsink_server_begin_archive,
	.archive_contents = bbsink_server_archive_contents,
	.end_archive = bbsink_server_end_archive,
	.begin_manifest = bbsink_server_begin_manifest,
	.manifest_contents = bbsink_server_manifest_contents,
	.end_manifest = bbsink_server_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

/*
 * 新建 'server' bbsink。
 */
bbsink *
bbsink_server_new(bbsink *next, char *pathname)
{
	bbsink_server *sink = palloc0(sizeof(bbsink_server));

	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_server_ops;
	sink->pathname = pathname;
	sink->base.bbs_next = next;

	/* 仅有复制权限在此情形下不够。 */
	StartTransactionCommand();
	if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create backup stored on server"),
				 errdetail("Only roles with privileges of the \"%s\" role may create a backup stored on the server.",
						   "pg_write_server_files")));
	CommitTransactionCommand();

	/*
	 * 不宜把备份放在正在备份的同一目录。若允许
	 * 相对路径，很容易误发生，故不许。用户仍可把
	 * $PGDATA 绝对路径写入路径名达到同样效果，
	 * 但那多半是有意的坏决定而非意外。
	 *
	 *
	 */
	if (!is_absolute_path(pathname))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("relative path not allowed for backup stored on server")));

	switch (pg_check_dir(pathname))
	{
		case 0:

			/*
			 * 不存在，故用与数据目录新子目录相同权限
			 * 创建它。
			 */
			if (MakePGDirectory(pathname) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create directory \"%s\": %m", pathname)));
			break;

		case 1:
			/* 存在且空. */
			break;

		case 2:
		case 3:
		case 4:
			/* 存在但非空。 */
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FILE),
					 errmsg("directory \"%s\" exists but is not empty",
							pathname)));
			break;

		default:
			/* 访问问题. */
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access directory \"%s\": %m",
							pathname)));
	}

	return &sink->base;
}

/*
 * 为本归档打开正确的输出文件。
 */
static void
bbsink_server_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	char	   *filename;

	Assert(mysink->file == 0);
	Assert(mysink->filepos == 0);

	filename = psprintf("%s/%s", mysink->pathname, archive_name);

	mysink->file = PathNameOpenFile(filename,
									O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (mysink->file <= 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", filename)));

	pfree(filename);

	bbsink_forward_begin_archive(sink, archive_name);
}

/*
 * 把数据写入输出文件。
 */
static void
bbsink_server_archive_contents(bbsink *sink, size_t len)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	int			nbytes;

	nbytes = FileWrite(mysink->file, mysink->base.bbs_buffer, len,
					   mysink->filepos, WAIT_EVENT_BASEBACKUP_WRITE);

	if (nbytes != len)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							FilePathName(mysink->file)),
					 errhint("Check free disk space.")));
		/* 短写：按情形报错 */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write file \"%s\": wrote only %d of %d bytes at offset %u",
						FilePathName(mysink->file),
						nbytes, (int) len, (unsigned) mysink->filepos),
				 errhint("Check free disk space.")));
	}

	mysink->filepos += nbytes;

	bbsink_forward_archive_contents(sink, len);
}

/*
 * fsync 并关闭当前输出文件。
 */
static void
bbsink_server_end_archive(bbsink *sink)
{
	bbsink_server *mysink = (bbsink_server *) sink;

	/*
	 * 有意不用 data_sync_elevel，因不能仅因无法保证
	 * 备份已落盘就让服务器 PANIC。此时跑恢复也
	 * 无济于事。
	 *
	 */
	if (FileSync(mysink->file, WAIT_EVENT_BASEBACKUP_SYNC) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						FilePathName(mysink->file))));


	/* 本文件现已处理完毕。 */
	FileClose(mysink->file);
	mysink->file = 0;
	mysink->filepos = 0;

	bbsink_forward_end_archive(sink);
}

/*
 * 打开将写入清单的输出文件。
 *
 * 与 pg_basebackup 一样，先用临时名写清单，fsync
 * 后再改名到位。这样若清单以正确名存在，用户可
 * 确信备份已完成。
 *
 */
static void
bbsink_server_begin_manifest(bbsink *sink)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	char	   *tmp_filename;

	Assert(mysink->file == 0);

	tmp_filename = psprintf("%s/backup_manifest.tmp", mysink->pathname);

	mysink->file = PathNameOpenFile(tmp_filename,
									O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (mysink->file <= 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmp_filename)));

	pfree(tmp_filename);

	bbsink_forward_begin_manifest(sink);
}

/*
 * 清单数据的每一块用 CopyData 消息发送。
 */
static void
bbsink_server_manifest_contents(bbsink *sink, size_t len)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	int			nbytes;

	nbytes = FileWrite(mysink->file, mysink->base.bbs_buffer, len,
					   mysink->filepos, WAIT_EVENT_BASEBACKUP_WRITE);

	if (nbytes != len)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							FilePathName(mysink->file)),
					 errhint("Check free disk space.")));
		/* 短写：按情形报错 */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write file \"%s\": wrote only %d of %d bytes at offset %u",
						FilePathName(mysink->file),
						nbytes, (int) len, (unsigned) mysink->filepos),
				 errhint("Check free disk space.")));
	}

	mysink->filepos += nbytes;

	bbsink_forward_manifest_contents(sink, len);
}

/*
 * fsync 备份清单、关闭文件，再改名到位。
 */
static void
bbsink_server_end_manifest(bbsink *sink)
{
	bbsink_server *mysink = (bbsink_server *) sink;
	char	   *tmp_filename;
	char	   *filename;

	/* 本文件现已处理完毕。 */
	FileClose(mysink->file);
	mysink->file = 0;

	/*
	 * 改名到位。这也会 fsync 临时文件，故此处不必再做。
	 * 此处不用 data_sync_elevel，理由同
	 * bbsink_server_end_archive。
	 */
	tmp_filename = psprintf("%s/backup_manifest.tmp", mysink->pathname);
	filename = psprintf("%s/backup_manifest", mysink->pathname);
	durable_rename(tmp_filename, filename, ERROR);
	pfree(filename);
	pfree(tmp_filename);

	bbsink_forward_end_manifest(sink);
}
