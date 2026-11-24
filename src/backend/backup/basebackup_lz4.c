/*-------------------------------------------------------------------------
 *
 * basebackup_lz4.c
 *	  实现 lz4 压缩的基备份 sink。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4frame.h>
#endif

#include "backup/basebackup_sink.h"

#ifdef USE_LZ4

typedef struct bbsink_lz4
{
	/* 各类 sink 的公共信息。 */
	bbsink		base;

	/* 压缩级别。 */
	int			compresslevel;

	LZ4F_compressionContext_t ctx;
	LZ4F_preferences_t prefs;

	/* 输出缓冲中已暂存的字节数。 */
	size_t		bytes_written;
} bbsink_lz4;

static void bbsink_lz4_begin_backup(bbsink *sink);
static void bbsink_lz4_begin_archive(bbsink *sink, const char *archive_name);
static void bbsink_lz4_archive_contents(bbsink *sink, size_t avail_in);
static void bbsink_lz4_manifest_contents(bbsink *sink, size_t len);
static void bbsink_lz4_end_archive(bbsink *sink);
static void bbsink_lz4_cleanup(bbsink *sink);

static const bbsink_ops bbsink_lz4_ops = {
	.begin_backup = bbsink_lz4_begin_backup,
	.begin_archive = bbsink_lz4_begin_archive,
	.archive_contents = bbsink_lz4_archive_contents,
	.end_archive = bbsink_lz4_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_lz4_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_lz4_cleanup
};
#endif

/*
 * 创建执行 lz4 压缩的新基备份 sink。
 */
bbsink *
bbsink_lz4_new(bbsink *next, pg_compress_specification *compress)
{
#ifndef USE_LZ4
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("lz4 compression is not supported by this build")));
	return NULL;				/* 让编译器安静 */
#else
	bbsink_lz4 *sink;
	int			compresslevel;

	Assert(next != NULL);

	compresslevel = compress->level;
	Assert(compresslevel >= 0 && compresslevel <= 12);

	sink = palloc0(sizeof(bbsink_lz4));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_lz4_ops;
	sink->base.bbs_next = next;
	sink->compresslevel = compresslevel;

	return &sink->base;
#endif
}

#ifdef USE_LZ4

/*
 * 开始备份.
 */
static void
bbsink_lz4_begin_backup(bbsink *sink)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	size_t		output_buffer_bound;
	LZ4F_preferences_t *prefs = &mysink->prefs;

	/* 初始化压缩器对象。 */
	memset(prefs, 0, sizeof(LZ4F_preferences_t));
	prefs->frameInfo.blockSizeID = LZ4F_max256KB;
	prefs->compressionLevel = mysink->compresslevel;

	/*
	 * 需要自有缓冲，因传给下一 sink 的数据不同于
	 * 传给我们的。
	 */
	mysink->base.bbs_buffer = palloc(mysink->base.bbs_buffer_length);

	/*
	 * 因 LZ4F_compressUpdate() 需要输出缓冲大小不小于
	 * LZ4F_compressBound()，须保证下一 sink 的
	 * bbs_buffer 长度能容纳压缩后的输入缓冲。
	 *
	 */
	output_buffer_bound = LZ4F_compressBound(mysink->base.bbs_buffer_length,
											 &mysink->prefs);

	/*
	 * 缓冲长度预期为 BLCKSZ 的倍数，故向上取整。
	 */
	output_buffer_bound = output_buffer_bound + BLCKSZ -
		(output_buffer_bound % BLCKSZ);

	bbsink_begin_backup(sink->bbs_next, sink->bbs_state, output_buffer_bound);
}

/*
 * 准备压缩下一归档。
 */
static void
bbsink_lz4_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	char	   *lz4_archive_name;
	LZ4F_errorCode_t ctxError;
	size_t		headerSize;

	ctxError = LZ4F_createCompressionContext(&mysink->ctx, LZ4F_VERSION);
	if (LZ4F_isError(ctxError))
		elog(ERROR, "could not create lz4 compression context: %s",
			 LZ4F_getErrorName(ctxError));

	/* 先把帧头写入目标缓冲。 */
	headerSize = LZ4F_compressBegin(mysink->ctx,
									mysink->base.bbs_next->bbs_buffer,
									mysink->base.bbs_next->bbs_buffer_length,
									&mysink->prefs);

	if (LZ4F_isError(headerSize))
		elog(ERROR, "could not write lz4 header: %s",
			 LZ4F_getErrorName(headerSize));

	/*
	 * 压缩数据须写在输出缓冲头之后。故须更新已写入
	 * 输出缓冲字节数的认识。
	 *
	 */
	mysink->bytes_written += headerSize;

	/* 归档名加 ".lz4"。 */
	lz4_archive_name = psprintf("%s.lz4", archive_name);
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, lz4_archive_name);
	pfree(lz4_archive_name);
}

/*
 * 把输入压缩到输出缓冲直至输入耗尽。每当输出缓冲
 * 低于输入缓冲的压缩上界，就调用下一 sink 的
 * archive_contents() 方法。
 *
	 * 注意因在压缩，常会耗尽输入而输出缓冲未满。此
	 * 时当前输入的压缩结果要等到本函数稍后调用，
	 * 甚至要到 bbsink_lz4_end_archive() 才发给
	 * 下一 bbsink。
	 *
 */
static void
bbsink_lz4_archive_contents(bbsink *sink, size_t avail_in)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	size_t		compressedSize;
	size_t		avail_in_bound;

	avail_in_bound = LZ4F_compressBound(avail_in, &mysink->prefs);

	/*
	 * 若可用字节数已低于 LZ4F_compressBound() 算出的值，
	 * 请下一 sink 处理数据以便我们
	 * 清空缓冲。
	 */
	if ((mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written) <
		avail_in_bound)
	{
		bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
		mysink->bytes_written = 0;
	}

	/*
	 * 压缩输入缓冲并写入输出缓冲。
	 */
	compressedSize = LZ4F_compressUpdate(mysink->ctx,
										 mysink->base.bbs_next->bbs_buffer + mysink->bytes_written,
										 mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written,
										 (uint8 *) mysink->base.bbs_buffer,
										 avail_in,
										 NULL);

	if (LZ4F_isError(compressedSize))
		elog(ERROR, "could not compress data: %s",
			 LZ4F_getErrorName(compressedSize));

	/*
	 * 更新已写入输出缓冲的字节数认识。
	 */
	mysink->bytes_written += compressedSize;
}

/*
 * lz4 内部缓冲可能还有数据；须冲刷并结束 lz4 帧，
 * 再作为归档内容转发给后继
 * sink。
 *
 * 然后可结束本归档处理。
 */
static void
bbsink_lz4_end_archive(bbsink *sink)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;
	size_t		compressedSize;
	size_t		lz4_footer_bound;

	lz4_footer_bound = LZ4F_compressBound(0, &mysink->prefs);

	Assert(mysink->base.bbs_next->bbs_buffer_length >= lz4_footer_bound);

	if ((mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written) <
		lz4_footer_bound)
	{
		bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
		mysink->bytes_written = 0;
	}

	compressedSize = LZ4F_compressEnd(mysink->ctx,
									  mysink->base.bbs_next->bbs_buffer + mysink->bytes_written,
									  mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written,
									  NULL);

	if (LZ4F_isError(compressedSize))
		elog(ERROR, "could not end lz4 compression: %s",
			 LZ4F_getErrorName(compressedSize));

	/* 更新已写入字节数的认识。 */
	mysink->bytes_written += compressedSize;

	/* 发送已累积的全部输出字节。 */
	bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
	mysink->bytes_written = 0;

	/* 释放资源。 */
	LZ4F_freeCompressionContext(mysink->ctx);
	mysink->ctx = NULL;

	/* 转发本归档已结束的信息。 */
	bbsink_forward_end_archive(sink);
}

/*
 * 清单内容不压缩，但须拷入后继 sink 缓冲，因我们
 * 有自己的缓冲。
 */
static void
bbsink_lz4_manifest_contents(bbsink *sink, size_t len)
{
	memcpy(sink->bbs_next->bbs_buffer, sink->bbs_buffer, len);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * 若备份失败，须按需调用 LZ4F_freeCompressionContext()
 * 释放压缩上下文以免泄漏。
 */
static void
bbsink_lz4_cleanup(bbsink *sink)
{
	bbsink_lz4 *mysink = (bbsink_lz4 *) sink;

	if (mysink->ctx)
	{
		LZ4F_freeCompressionContext(mysink->ctx);
		mysink->ctx = NULL;
	}
}

#endif
