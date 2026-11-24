/*-------------------------------------------------------------------------
 *
 * backup_manifest.c
 *	  生成并发送备份清单的代码
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/backup_manifest.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/timeline.h"
#include "backup/backup_manifest.h"
#include "backup/basebackup_sink.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/json.h"

static void AppendStringToManifest(backup_manifest_info *manifest, const char *s);

/*
 * 用户是否需要备份清单？
 *
 * 最简单是始终持有 manifest_info 对象，这样多数地方
 * 就不必检查空指针。但若用户不要清单，则把
 * manifest->buffile 置为 NULL。
 */
static inline bool
IsManifestEnabled(backup_manifest_info *manifest)
{
	return (manifest->buffile != NULL);
}

/*
 * 向备份清单追加数据的便利宏。
 */
#define AppendToManifest(manifest, ...) \
	{ \
		char *_manifest_s = psprintf(__VA_ARGS__);	\
		AppendStringToManifest(manifest, _manifest_s);	\
		pfree(_manifest_s);	\
	}

/*
 * 初始化状态以便构造备份清单。
 *
 * 注意：虽数据文件校验和类型可配置，但清单自身的
 * 校验和始终用 SHA-256。见
 * SendBackupManifest.
 */
void
InitializeBackupManifest(backup_manifest_info *manifest,
						 backup_manifest_option want_manifest,
						 pg_checksum_type manifest_checksum_type)
{
	memset(manifest, 0, sizeof(backup_manifest_info));
	manifest->checksum_type = manifest_checksum_type;

	if (want_manifest == MANIFEST_OPTION_NO)
		manifest->buffile = NULL;
	else
	{
		manifest->buffile = BufFileCreateTemp(false);
		manifest->manifest_ctx = pg_cryptohash_create(PG_SHA256);
		if (pg_cryptohash_init(manifest->manifest_ctx) < 0)
			elog(ERROR, "failed to initialize checksum of backup manifest: %s",
				 pg_cryptohash_error(manifest->manifest_ctx));
	}

	manifest->manifest_size = UINT64CONST(0);
	manifest->force_encode = (want_manifest == MANIFEST_OPTION_FORCE_ENCODE);
	manifest->first_file = true;
	manifest->still_checksumming = true;

	if (want_manifest != MANIFEST_OPTION_NO)
		AppendToManifest(manifest,
						 "{ \"PostgreSQL-Backup-Manifest-Version\": 1,\n"
						 "\"Files\": [");
}

/*
 * 释放已构造备份清单所占用的资源。
 */
void
FreeBackupManifest(backup_manifest_info *manifest)
{
	pg_cryptohash_free(manifest->manifest_ctx);
	manifest->manifest_ctx = NULL;
}

/*
 * 为某文件向备份清单添加条目。
 */
void
AddFileToBackupManifest(backup_manifest_info *manifest, const char *spcoid,
						const char *pathname, size_t size, pg_time_t mtime,
						pg_checksum_context *checksum_ctx)
{
	char		pathbuf[MAXPGPATH];
	int			pathlen;
	StringInfoData buf;

	if (!IsManifestEnabled(manifest))
		return;

	/*
	 * 若文件属某表空间，传入本函数的路径名是相对
	 * 于容纳它的 tar 文件的。我们要的是相对数据目录的
	 * 路径名（忽略中间的符号链接
	 * 穿越）。
	 */
	if (spcoid != NULL)
	{
		snprintf(pathbuf, sizeof(pathbuf), "pg_tblspc/%s/%s", spcoid,
				 pathname);
		pathname = pathbuf;
	}

	/*
	 * 每个文件条目须与后续条目用逗号分隔，但首条前与
	 * 末条后皆无逗号。为此，向清单添加文件时先
	 * 结束最近追加的行（必要时加逗号），
	 * 但不结束为本文件新插入的那一行。
	 * 本文件行保持未结束。
	 */
	initStringInfo(&buf);
	if (manifest->first_file)
	{
		appendStringInfoChar(&buf, '\n');
		manifest->first_file = false;
	}
	else
		appendStringInfoString(&buf, ",\n");

	/*
	 * 把该文件相对路径写出到清单。清单始终以
	 * UTF-8 存储，故须对在该编码下不合法的路径
	 * 另行编码。
	 */
	pathlen = strlen(pathname);
	if (!manifest->force_encode &&
		pg_verify_mbstr(PG_UTF8, pathname, pathlen, true))
	{
		appendStringInfoString(&buf, "{ \"Path\": ");
		escape_json(&buf, pathname);
		appendStringInfoString(&buf, ", ");
	}
	else
	{
		appendStringInfoString(&buf, "{ \"Encoded-Path\": \"");
		enlargeStringInfo(&buf, 2 * pathlen);
		buf.len += hex_encode(pathname, pathlen,
							  &buf.data[buf.len]);
		appendStringInfoString(&buf, "\", ");
	}

	appendStringInfo(&buf, "\"Size\": %zu, ", size);

	/*
	 * 把最后修改时间转为字符串并追加到清单。因
	 * 时区选用不明且定义可能变更而致混淆，故用
	 * GMT。
	 *
	 */
	appendStringInfoString(&buf, "\"Last-Modified\": \"");
	enlargeStringInfo(&buf, 128);
	buf.len += pg_strftime(&buf.data[buf.len], 128, "%Y-%m-%d %H:%M:%S %Z",
						   pg_gmtime(&mtime));
	appendStringInfoChar(&buf, '"');

	/* 添加校验和信息。 */
	if (checksum_ctx->type != CHECKSUM_TYPE_NONE)
	{
		uint8		checksumbuf[PG_CHECKSUM_MAX_LENGTH];
		int			checksumlen;

		checksumlen = pg_checksum_final(checksum_ctx, checksumbuf);
		if (checksumlen < 0)
			elog(ERROR, "could not finalize checksum of file \"%s\"",
				 pathname);

		appendStringInfo(&buf,
						 ", \"Checksum-Algorithm\": \"%s\", \"Checksum\": \"",
						 pg_checksum_type_name(checksum_ctx->type));
		enlargeStringInfo(&buf, 2 * checksumlen);
		buf.len += hex_encode((char *) checksumbuf, checksumlen,
							  &buf.data[buf.len]);
		appendStringInfoChar(&buf, '"');
	}

	/* 结束该对象。 */
	appendStringInfoString(&buf, " }");

	/* 好，加入清单。 */
	AppendStringToManifest(manifest, buf.data);

	/* 避免泄漏内存。 */
	pfree(buf.data);
}

/*
 * 把恢复本备份时须重放的 WAL 相关信息
 * 加入备份清单。
 */
void
AddWALInfoToBackupManifest(backup_manifest_info *manifest, XLogRecPtr startptr,
						   TimeLineID starttli, XLogRecPtr endptr,
						   TimeLineID endtli)
{
	List	   *timelines;
	ListCell   *lc;
	bool		first_wal_range = true;
	bool		found_start_timeline = false;

	if (!IsManifestEnabled(manifest))
		return;

	/* 结束文件列表。 */
	AppendStringToManifest(manifest, "\n],\n");

	/* 读取结束时间线的时间线历史。 */
	timelines = readTimeLineHistory(endtli);

	/* 开始 LSN 范围列表。 */
	AppendStringToManifest(manifest, "\"WAL-Ranges\": [\n");

	foreach(lc, timelines)
	{
		TimeLineHistoryEntry *entry = lfirst(lc);
		XLogRecPtr	tl_beginptr;

		/*
		 * 我们只关心备份期间处于活动的时间线。
		 * 跳过在备份开始前已结束的。（注意：若
		 * entry->end 为 InvalidXLogRecPtr，表示时间线
		 * 未结束.)
		 */
		if (!XLogRecPtrIsInvalid(entry->end) && entry->end < startptr)
			continue;

		/*
		 * 因时间线历史文件先列较新再列较旧者，
		 * 我们遇到的第一条足够新的时间线应与备份的
		 * 结束时间线相符。
		 */
		if (first_wal_range && endtli != entry->tli)
			ereport(ERROR,
					errmsg("expected end timeline %u but found timeline %u",
						   starttli, entry->tli));

		/*
		 * 若本时间线条目与备份开始所在时间线匹配，
		 * 则须自备份起始 LSN 起检查 WAL。若条目指向
		 * 较新时间线，则须自该时间线起点起检查，故用
		 * 该时间线开始处的 LSN。
		 * 即其起点。
		 */
		if (starttli == entry->tli)
			tl_beginptr = startptr;
		else
		{
			tl_beginptr = entry->begin;

			/*
			 * 若到达无有效起始 LSN 的 TLI，则其后历史中
			 * 不应再有时间线，故此时应已到预期的起始
			 * TLI。若否，则说明严重出错。
			 *
			 */
			if (XLogRecPtrIsInvalid(entry->begin))
				ereport(ERROR,
						errmsg("expected start timeline %u but found timeline %u",
							   starttli, entry->tli));
		}

		AppendToManifest(manifest,
						 "%s{ \"Timeline\": %u, \"Start-LSN\": \"%X/%X\", \"End-LSN\": \"%X/%X\" }",
						 first_wal_range ? "" : ",\n",
						 entry->tli,
						 LSN_FORMAT_ARGS(tl_beginptr),
						 LSN_FORMAT_ARGS(endptr));

		if (starttli == entry->tli)
		{
			found_start_timeline = true;
			break;
		}

		endptr = entry->begin;
		first_wal_range = false;
	}

	/*
	 * 结束时间线之时间线历史的最后一条应为结束时间线
	 * 本身。验证我们观察到的正是如此。
	 */
	if (!found_start_timeline)
		ereport(ERROR,
				errmsg("start timeline %u not found in history of timeline %u",
					   starttli, endtli));

	/* 结束 WAL 范围列表。 */
	AppendStringToManifest(manifest, "\n],\n");
}

/*
 * 完成备份清单，并发送给客户端。
 */
void
SendBackupManifest(backup_manifest_info *manifest, bbsink *sink)
{
	uint8		checksumbuf[PG_SHA256_DIGEST_LENGTH];
	char		checksumstringbuf[PG_SHA256_DIGEST_STRING_LENGTH];
	size_t		manifest_bytes_done = 0;

	if (!IsManifestEnabled(manifest))
		return;

	/*
	 * 追加清单校验和，以便发现清单自身的
	 * 问题。
	 *
	 * 此处始终用 SHA-256，与文件校验和所选算法
	 * 无关。若日后要使清单文件校验和算法可变，
	 * 客户端须尽可能靠近清单文件开头就判明所用
	 * 算法，以免不得不把整份文件
	 * 再读
	 * 两次
	 */
	manifest->still_checksumming = false;
	if (pg_cryptohash_final(manifest->manifest_ctx, checksumbuf,
							sizeof(checksumbuf)) < 0)
		elog(ERROR, "failed to finalize checksum of backup manifest: %s",
			 pg_cryptohash_error(manifest->manifest_ctx));
	AppendStringToManifest(manifest, "\"Manifest-Checksum\": \"");

	hex_encode((char *) checksumbuf, sizeof checksumbuf, checksumstringbuf);
	checksumstringbuf[PG_SHA256_DIGEST_STRING_LENGTH - 1] = '\0';

	AppendStringToManifest(manifest, checksumstringbuf);
	AppendStringToManifest(manifest, "\"}\n");

	/*
	 * 清单文件数据已全部写完。回绕文件以便整份
	 * 再读回来。
	 */
	if (BufFileSeek(manifest->buffile, 0, 0, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind temporary file")));


	/*
	 * 发送备份清单。
	 */
	bbsink_begin_manifest(sink);
	while (manifest_bytes_done < manifest->manifest_size)
	{
		size_t		bytes_to_read;

		bytes_to_read = Min(sink->bbs_buffer_length,
							manifest->manifest_size - manifest_bytes_done);
		BufFileReadExact(manifest->buffile, sink->bbs_buffer, bytes_to_read);
		bbsink_manifest_contents(sink, bytes_to_read);
		manifest_bytes_done += bytes_to_read;
	}
	bbsink_end_manifest(sink);

	/* 释放资源 */
	BufFileClose(manifest->buffile);
}

/*
 * 向清单追加一个 cstring。
 */
static void
AppendStringToManifest(backup_manifest_info *manifest, const char *s)
{
	int			len = strlen(s);

	Assert(manifest != NULL);
	if (manifest->still_checksumming)
	{
		if (pg_cryptohash_update(manifest->manifest_ctx, (uint8 *) s, len) < 0)
			elog(ERROR, "failed to update checksum of backup manifest: %s",
				 pg_cryptohash_error(manifest->manifest_ctx));
	}
	BufFileWrite(manifest->buffile, s, len);
	manifest->manifest_size += len;
}
