/*-------------------------------------------------------------------------
 *
 * basebackup_zstd.c
 *	  实现 zstd 压缩的基备份 sink。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_zstd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "backup/basebackup_sink.h"

#ifdef USE_ZSTD

typedef struct bbsink_zstd
{
	/* 各类 sink 的公共信息。 */
	bbsink		base;

	/* 压缩选项 */
	pg_compress_specification *compress;

	ZSTD_CCtx  *cctx;
	ZSTD_outBuffer zstd_outBuf;
} bbsink_zstd;

static void bbsink_zstd_begin_backup(bbsink *sink);
static void bbsink_zstd_begin_archive(bbsink *sink, const char *archive_name);
static void bbsink_zstd_archive_contents(bbsink *sink, size_t avail_in);
static void bbsink_zstd_manifest_contents(bbsink *sink, size_t len);
static void bbsink_zstd_end_archive(bbsink *sink);
static void bbsink_zstd_cleanup(bbsink *sink);
static void bbsink_zstd_end_backup(bbsink *sink, XLogRecPtr endptr,
								   TimeLineID endtli);

static const bbsink_ops bbsink_zstd_ops = {
	.begin_backup = bbsink_zstd_begin_backup,
	.begin_archive = bbsink_zstd_begin_archive,
	.archive_contents = bbsink_zstd_archive_contents,
	.end_archive = bbsink_zstd_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_zstd_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_zstd_end_backup,
	.cleanup = bbsink_zstd_cleanup
};
#endif

/*
 * 创建执行 zstd 压缩的新基备份 sink。
 */
bbsink *
bbsink_zstd_new(bbsink *next, pg_compress_specification *compress)
{
#ifndef USE_ZSTD
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("zstd compression is not supported by this build")));
	return NULL;				/* 让编译器安静 */
#else
	bbsink_zstd *sink;

	Assert(next != NULL);

	sink = palloc0(sizeof(bbsink_zstd));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_zstd_ops;
	sink->base.bbs_next = next;
	sink->compress = compress;

	return &sink->base;
#endif
}

#ifdef USE_ZSTD

/*
 * 开始备份.
 */
static void
bbsink_zstd_begin_backup(bbsink *sink)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	size_t		output_buffer_bound;
	size_t		ret;
	pg_compress_specification *compress = mysink->compress;

	mysink->cctx = ZSTD_createCCtx();
	if (!mysink->cctx)
		elog(ERROR, "could not create zstd compression context");

	ret = ZSTD_CCtx_setParameter(mysink->cctx, ZSTD_c_compressionLevel,
								 compress->level);
	if (ZSTD_isError(ret))
		elog(ERROR, "could not set zstd compression level to %d: %s",
			 compress->level, ZSTD_getErrorName(ret));

	if ((compress->options & PG_COMPRESSION_OPTION_WORKERS) != 0)
	{
		/*
		 * 较旧 libzstd 无此选项，试图设置会
		 * 失败。较新版本若编译时无线程支持也同样。
		 * 无线程支持时亦然。
		 */
		ret = ZSTD_CCtx_setParameter(mysink->cctx, ZSTD_c_nbWorkers,
									 compress->workers);
		if (ZSTD_isError(ret))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not set compression worker count to %d: %s",
						   compress->workers, ZSTD_getErrorName(ret)));
	}

	if ((compress->options & PG_COMPRESSION_OPTION_LONG_DISTANCE) != 0)
	{
		ret = ZSTD_CCtx_setParameter(mysink->cctx,
									 ZSTD_c_enableLongDistanceMatching,
									 compress->long_distance);
		if (ZSTD_isError(ret))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("could not enable long-distance mode: %s",
						   ZSTD_getErrorName(ret)));
	}

	/*
	 * 需要自有缓冲，因传给下一 sink 的数据不同于
	 * 传给我们的。
	 */
	mysink->base.bbs_buffer = palloc(mysink->base.bbs_buffer_length);

	/*
	 * 确保下一 sink 的 bbs_buffer 足够大以容纳
	 * 压缩后的输入缓冲。
	 */
	output_buffer_bound = ZSTD_compressBound(mysink->base.bbs_buffer_length);

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
bbsink_zstd_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	char	   *zstd_archive_name;

	/*
	 * 每个归档开始时重置状态以开始新的
	 * 压缩操作。参数会保留，因我们用
	 * ZSTD_reset_session_only 重置。
	 */
	ZSTD_CCtx_reset(mysink->cctx, ZSTD_reset_session_only);

	mysink->zstd_outBuf.dst = mysink->base.bbs_next->bbs_buffer;
	mysink->zstd_outBuf.size = mysink->base.bbs_next->bbs_buffer_length;
	mysink->zstd_outBuf.pos = 0;

	/* 归档名加 ".zst"。 */
	zstd_archive_name = psprintf("%s.zst", archive_name);
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, zstd_archive_name);
	pfree(zstd_archive_name);
}

/*
 * 把输入压缩到输出缓冲直至输入耗尽。每当输出缓冲
 * 低于输入缓冲的压缩上界，就调用下一 sink 的
 * archive_contents() 方法。
 *
 * 注意因在压缩，常会耗尽输入而输出缓冲未满。此
 * 时当前输入的压缩结果要等到本函数稍后调用，
 * 甚至也许要等到 bbsink_zstd_end_archive()
 * 被调用时才发给下一 bbsink。
 * 才调用。
 */
static void
bbsink_zstd_archive_contents(bbsink *sink, size_t len)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	ZSTD_inBuffer inBuf = {mysink->base.bbs_buffer, len, 0};

	while (inBuf.pos < inBuf.size)
	{
		size_t		yet_to_flush;
		size_t		max_needed = ZSTD_compressBound(inBuf.size - inBuf.pos);

		/*
		 * 若输出缓冲剩余空间不足，把输出缓冲发给下一
		 * sink 并重置。
		 */
		if (mysink->zstd_outBuf.size - mysink->zstd_outBuf.pos < max_needed)
		{
			bbsink_archive_contents(mysink->base.bbs_next,
									mysink->zstd_outBuf.pos);
			mysink->zstd_outBuf.dst = mysink->base.bbs_next->bbs_buffer;
			mysink->zstd_outBuf.size =
				mysink->base.bbs_next->bbs_buffer_length;
			mysink->zstd_outBuf.pos = 0;
		}

		yet_to_flush = ZSTD_compressStream2(mysink->cctx, &mysink->zstd_outBuf,
											&inBuf, ZSTD_e_continue);

		if (ZSTD_isError(yet_to_flush))
			elog(ERROR,
				 "could not compress data: %s",
				 ZSTD_getErrorName(yet_to_flush));
	}
}

/*
 * zstd 内部缓冲可能还有数据；须冲刷并结束 zstd 帧，
 * 再冲刷出来、结束帧并转发给后继 sink 作
 * 归档内容。
 *
 * 然后可结束本归档处理。
 */
static void
bbsink_zstd_end_archive(bbsink *sink)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;
	size_t		yet_to_flush;

	do
	{
		ZSTD_inBuffer in = {NULL, 0, 0};
		size_t		max_needed = ZSTD_compressBound(0);

		/*
		 * 若输出缓冲剩余空间不足，把输出缓冲发给下一
		 * sink 并重置。
		 */
		if (mysink->zstd_outBuf.size - mysink->zstd_outBuf.pos < max_needed)
		{
			bbsink_archive_contents(mysink->base.bbs_next,
									mysink->zstd_outBuf.pos);
			mysink->zstd_outBuf.dst = mysink->base.bbs_next->bbs_buffer;
			mysink->zstd_outBuf.size =
				mysink->base.bbs_next->bbs_buffer_length;
			mysink->zstd_outBuf.pos = 0;
		}

		yet_to_flush = ZSTD_compressStream2(mysink->cctx,
											&mysink->zstd_outBuf,
											&in, ZSTD_e_end);

		if (ZSTD_isError(yet_to_flush))
			elog(ERROR, "could not compress data: %s",
				 ZSTD_getErrorName(yet_to_flush));

	} while (yet_to_flush > 0);

	/* 确保把剩余字节传给下一 sink。 */
	if (mysink->zstd_outBuf.pos > 0)
		bbsink_archive_contents(mysink->base.bbs_next,
								mysink->zstd_outBuf.pos);

	/* 转发本归档已结束的信息。 */
	bbsink_forward_end_archive(sink);
}

/*
 * 释放资源与上下文。
 */
static void
bbsink_zstd_end_backup(bbsink *sink, XLogRecPtr endptr,
					   TimeLineID endtli)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;

	/* 释放上下文。 */
	if (mysink->cctx)
	{
		ZSTD_freeCCtx(mysink->cctx);
		mysink->cctx = NULL;
	}

	bbsink_forward_end_backup(sink, endptr, endtli);
}

/*
 * 清单内容不压缩，但须拷入后继 sink 缓冲，因我们
 * 有自己的缓冲。
 */
static void
bbsink_zstd_manifest_contents(bbsink *sink, size_t len)
{
	memcpy(sink->bbs_next->bbs_buffer, sink->bbs_buffer, len);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * 若备份失败，须释放任何已分配的压缩上下文，以免
 * 泄漏内存。
 */
static void
bbsink_zstd_cleanup(bbsink *sink)
{
	bbsink_zstd *mysink = (bbsink_zstd *) sink;

	/* 若尚未释放则释放上下文。 */
	if (mysink->cctx)
	{
		ZSTD_freeCCtx(mysink->cctx);
		mysink->cctx = NULL;
	}
}

#endif
