/*-------------------------------------------------------------------------
 *
 * basebackup_gzip.c
 *	  实现 gzip 压缩的基备份 sink。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_gzip.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "backup/basebackup_sink.h"

#ifdef HAVE_LIBZ
typedef struct bbsink_gzip
{
	/* 各类 sink 的公共信息。 */
	bbsink		base;

	/* 压缩级别。 */
	int			compresslevel;

	/* 压缩数据流。 */
	z_stream	zstream;

	/* 输出缓冲中已暂存的字节数。 */
	size_t		bytes_written;
} bbsink_gzip;

static void bbsink_gzip_begin_backup(bbsink *sink);
static void bbsink_gzip_begin_archive(bbsink *sink, const char *archive_name);
static void bbsink_gzip_archive_contents(bbsink *sink, size_t len);
static void bbsink_gzip_manifest_contents(bbsink *sink, size_t len);
static void bbsink_gzip_end_archive(bbsink *sink);
static void *gzip_palloc(void *opaque, unsigned items, unsigned size);
static void gzip_pfree(void *opaque, void *address);

static const bbsink_ops bbsink_gzip_ops = {
	.begin_backup = bbsink_gzip_begin_backup,
	.begin_archive = bbsink_gzip_begin_archive,
	.archive_contents = bbsink_gzip_archive_contents,
	.end_archive = bbsink_gzip_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_gzip_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};
#endif

/*
 * 创建执行 gzip 压缩的新基备份 sink。
 */
bbsink *
bbsink_gzip_new(bbsink *next, pg_compress_specification *compress)
{
#ifndef HAVE_LIBZ
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("gzip compression is not supported by this build")));
	return NULL;				/* 让编译器安静 */
#else
	bbsink_gzip *sink;
	int			compresslevel;

	Assert(next != NULL);

	compresslevel = compress->level;
	Assert((compresslevel >= 1 && compresslevel <= 9) ||
		   compresslevel == Z_DEFAULT_COMPRESSION);

	sink = palloc0(sizeof(bbsink_gzip));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_gzip_ops;
	sink->base.bbs_next = next;
	sink->compresslevel = compresslevel;

	return &sink->base;
#endif
}

#ifdef HAVE_LIBZ

/*
 * 开始备份.
 */
static void
bbsink_gzip_begin_backup(bbsink *sink)
{
	/*
	 * 需要自有缓冲，因传给下一 sink 的数据不同于
	 * 传给我们的。
	 */
	sink->bbs_buffer = palloc(sink->bbs_buffer_length);

	/*
	 * deflate() 不需要输出缓冲特定大小，故可与输入
	 * 缓冲同大。
	 */
	bbsink_begin_backup(sink->bbs_next, sink->bbs_state,
						sink->bbs_buffer_length);
}

/*
 * 准备压缩下一归档。
 */
static void
bbsink_gzip_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_gzip *mysink = (bbsink_gzip *) sink;
	char	   *gz_archive_name;
	z_stream   *zs = &mysink->zstream;

	/* 初始化压缩器对象。 */
	memset(zs, 0, sizeof(z_stream));
	zs->zalloc = gzip_palloc;
	zs->zfree = gzip_pfree;
	zs->next_out = (uint8 *) sink->bbs_next->bbs_buffer;
	zs->avail_out = sink->bbs_next->bbs_buffer_length;

	/*
	 * 须用 deflateInit2() 而非 deflateInit()，以便请求
	 * gzip 头而非 zlib 头。此外提供与直接调用
	 * deflateInit() 时相同的默认
	 * 值。
	 *
	 * 按 deflateInit2 文档，第三参须为 Z_DEFLATED；第四参
	 * 为 window bits，默认 15，加 16 则得 gzip 头而非 zlib
	 * 头；第五参控内存，默认 8；第六参默认亦为
	 * Z_DEFAULT_STRATEGY。
	 *
	 */
	if (deflateInit2(zs, mysink->compresslevel, Z_DEFLATED, 15 + 16, 8,
					 Z_DEFAULT_STRATEGY) != Z_OK)
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not initialize compression library"));

	/*
	 * 归档名加 ".gz"。注意 pg_basebackup -z 生成 ".tar.gz"
	 * 而非 ".tgz"，此处对齐。
	 */
	gz_archive_name = psprintf("%s.gz", archive_name);
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, gz_archive_name);
	pfree(gz_archive_name);
}

/*
 * 把输入压缩到输出缓冲直至输入耗尽。输出缓冲一满
 * 就调用下一 sink 的 archive_contents()
 * 方法。
 *
 * 注意因在压缩，常会耗尽输入而输出缓冲未满。此
 * 时当前输入的压缩结果要等到本函数稍后调用，
 * 甚至要到 bbsink_gzip_end_archive() 才发给
 * 下一 bbsink。
 * sink。
 */
static void
bbsink_gzip_archive_contents(bbsink *sink, size_t len)
{
	bbsink_gzip *mysink = (bbsink_gzip *) sink;
	z_stream   *zs = &mysink->zstream;

	/* 从输入缓冲压缩数据。 */
	zs->next_in = (uint8 *) mysink->base.bbs_buffer;
	zs->avail_in = len;

	while (zs->avail_in > 0)
	{
		int			res;

		/* 把输出写到输出缓冲未用部分。 */
		Assert(mysink->bytes_written < mysink->base.bbs_next->bbs_buffer_length);
		zs->next_out = (uint8 *)
			mysink->base.bbs_next->bbs_buffer + mysink->bytes_written;
		zs->avail_out =
			mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written;

		/*
		 * 尝试压缩。会按已耗输入更新 zs->next_in 与
		 * zs->avail_in，并按已产输出更新 zs->next_out 与
		 * zs->avail_out。
		 * 已写出。
		 *
		 * 按 zlib 文档，Z_STREAM_ERROR 仅在编程错误或
		 * 内存被破坏时出现；出于极度谨慎用 elog() 而非
		 * Assert()。
		 * 谨防万一。
		 */
		res = deflate(zs, Z_NO_FLUSH);
		if (res == Z_STREAM_ERROR)
			elog(ERROR, "could not compress data: %s", zs->msg);

		/* 更新已写入字节数的认识。 */
		mysink->bytes_written =
			mysink->base.bbs_next->bbs_buffer_length - zs->avail_out;

		/*
		 * 若输出缓冲已满，则该由下一 sink
		 * 处理内容。
		 */
		if (mysink->bytes_written >= mysink->base.bbs_next->bbs_buffer_length)
		{
			bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
			mysink->bytes_written = 0;
		}
	}
}

/*
 * zlib 内部缓冲可能还有数据；须冲刷并作为归档内容
 * 转发给后继 sink。
 *
 * 然后可结束本归档处理。
 */
static void
bbsink_gzip_end_archive(bbsink *sink)
{
	bbsink_gzip *mysink = (bbsink_gzip *) sink;
	z_stream   *zs = &mysink->zstream;

	/* 已无更多可用数据。 */
	zs->next_in = (uint8 *) mysink->base.bbs_buffer;
	zs->avail_in = 0;

	while (1)
	{
		int			res;

		/* 把输出写到输出缓冲未用部分。 */
		Assert(mysink->bytes_written < mysink->base.bbs_next->bbs_buffer_length);
		zs->next_out = (uint8 *)
			mysink->base.bbs_next->bbs_buffer + mysink->bytes_written;
		zs->avail_out =
			mysink->base.bbs_next->bbs_buffer_length - mysink->bytes_written;

		/*
		 * 如同 bbsink_gzip_archive_contents，但因无更多输入
		 * 传 Z_FINISH.
		 */
		res = deflate(zs, Z_FINISH);
		if (res == Z_STREAM_ERROR)
			elog(ERROR, "could not compress data: %s", zs->msg);

		/* 更新已写入字节数的认识。 */
		mysink->bytes_written =
			mysink->base.bbs_next->bbs_buffer_length - zs->avail_out;

		/*
		 * 显然输出缓冲无数据且 deflate() 也未能追加。
		 * 我们必定完成了。
		 */
		if (mysink->bytes_written == 0)
			break;

		/* 发送已累积的全部输出字节。 */
		bbsink_archive_contents(sink->bbs_next, mysink->bytes_written);
		mysink->bytes_written = 0;
	}

	/* 还必须转发本归档已结束的信息。 */
	bbsink_forward_end_archive(sink);
}

/*
 * 清单内容不压缩，但须拷入后继 sink 缓冲，因我们
 * 有自己的缓冲。
 */
static void
bbsink_gzip_manifest_contents(bbsink *sink, size_t len)
{
	memcpy(sink->bbs_next->bbs_buffer, sink->bbs_buffer, len);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * 包装函数，调整 palloc 签名以匹配 libz 所预期。
 *
 */
static void *
gzip_palloc(void *opaque, unsigned items, unsigned size)
{
	return palloc(items * size);
}

/*
 * 包装函数，调整 pfree 签名以匹配 libz 所预期。
 *
 */
static void
gzip_pfree(void *opaque, void *address)
{
	pfree(address);
}

#endif
