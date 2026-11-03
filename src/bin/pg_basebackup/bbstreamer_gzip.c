/*-------------------------------------------------------------------------
 *
 * bbstreamer_gzip.c
 *
 * 部分版权 (c) 1996-2023, PostgreSQL 全球开发组
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_gzip.c
 *-------------------------------------------------------------------------
                */

#include "postgres_fe.h"

#include <unistd.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "bbstreamer.h"
#include "common/logging.h"
#include "common/file_perm.h"
#include "common/string.h"

#ifdef HAVE_LIBZ
typedef struct bbstreamer_gzip_writer
{
	bbstreamer	base;
	char	   *pathname;
	gzFile		gzfile;
} bbstreamer_gzip_writer;

typedef struct bbstreamer_gzip_decompressor
{
	bbstreamer	base;
	z_stream	zstream;
	size_t		bytes_written;
} bbstreamer_gzip_decompressor;

static void bbstreamer_gzip_writer_content(bbstreamer *streamer,
										   bbstreamer_member *member,
										   const char *data, int len,
										   bbstreamer_archive_context context);
static void bbstreamer_gzip_writer_finalize(bbstreamer *streamer);
static void bbstreamer_gzip_writer_free(bbstreamer *streamer);
static const char *get_gz_error(gzFile gzf);

const bbstreamer_ops bbstreamer_gzip_writer_ops = {
	.content = bbstreamer_gzip_writer_content,
	.finalize = bbstreamer_gzip_writer_finalize,
	.free = bbstreamer_gzip_writer_free
};

static void bbstreamer_gzip_decompressor_content(bbstreamer *streamer,
												 bbstreamer_member *member,
												 const char *data, int len,
												 bbstreamer_archive_context context);
static void bbstreamer_gzip_decompressor_finalize(bbstreamer *streamer);
static void bbstreamer_gzip_decompressor_free(bbstreamer *streamer);
static void *gzip_palloc(void *opaque, unsigned items, unsigned size);
static void gzip_pfree(void *opaque, void *address);

const bbstreamer_ops bbstreamer_gzip_decompressor_ops = {
	.content = bbstreamer_gzip_decompressor_content,
	.finalize = bbstreamer_gzip_decompressor_finalize,
	.free = bbstreamer_gzip_decompressor_free
};
#endif

/*
 * 创建一个使用 gzip 压缩数据并将其写入文件的 bbstreamer。
 *
 * 与 bbstreamer_plain_writer_new 的情况一样，pathname 始终用于错误报告目的；
 * 如果 file 为 NULL，它也会打开并关闭以便数据可以写入那里。
                                    */
bbstreamer *
bbstreamer_gzip_writer_new(char *pathname, FILE *file,
						   pg_compress_specification *compress)
{
#ifdef HAVE_LIBZ
	bbstreamer_gzip_writer *streamer;

	streamer = palloc0(sizeof(bbstreamer_gzip_writer));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_gzip_writer_ops;

	streamer->pathname = pstrdup(pathname);

	if (file == NULL)
	{
		streamer->gzfile = gzopen(pathname, "wb");
		if (streamer->gzfile == NULL)
			pg_fatal("could not create compressed file \"%s\": %m",
					 pathname);
	}
	else
	{
		int			fd = dup(fileno(file));

		if (fd < 0)
			pg_fatal("could not duplicate stdout: %m");

		streamer->gzfile = gzdopen(fd, "wb");
		if (streamer->gzfile == NULL)
			pg_fatal("could not open output file: %m");
	}

	if (gzsetparams(streamer->gzfile, compress->level, Z_DEFAULT_STRATEGY) != Z_OK)
		pg_fatal("could not set compression level %d: %s",
				 compress->level, get_gz_error(streamer->gzfile));

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "gzip");
	return NULL;				/* keep compiler quiet */
#endif
}

#ifdef HAVE_LIBZ
/*
 * Write archive content to gzip file.
 */
static void
bbstreamer_gzip_writer_content(bbstreamer *streamer,
							   bbstreamer_member *member, const char *data,
							   int len, bbstreamer_archive_context context)
{
	bbstreamer_gzip_writer *mystreamer;

	mystreamer = (bbstreamer_gzip_writer *) streamer;

	if (len == 0)
		return;

	errno = 0;
	if (gzwrite(mystreamer->gzfile, data, len) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to compressed file \"%s\": %s",
				 mystreamer->pathname, get_gz_error(mystreamer->gzfile));
	}
}

/*
 * 当写入 gzip 文件时，归档结束处理仅包含调用 gzclose。
 *
 * 是我们打开了文件还是调用者打开了文件没有区别，因为 libz 无法避免
 * 关闭底层文件句柄。但请注意，bbstreamer_gzip_writer_new() 使用 dup()
 * 来解决这个问题，以便从调用者角度看行为与
 * bbstreamer_plain_writer 相同。
                                                                                    */
static void
bbstreamer_gzip_writer_finalize(bbstreamer *streamer)
{
	bbstreamer_gzip_writer *mystreamer;

	mystreamer = (bbstreamer_gzip_writer *) streamer;

	errno = 0;					/* 以防 gzclose() 不设置 errno */
	if (gzclose(mystreamer->gzfile) != 0)
		pg_fatal("could not close compressed file \"%s\": %m",
				 mystreamer->pathname);

	mystreamer->gzfile = NULL;
}

/*
 * 释放与此 bbstreamer 关联的内存。
   */
static void
bbstreamer_gzip_writer_free(bbstreamer *streamer)
{
	bbstreamer_gzip_writer *mystreamer;

	mystreamer = (bbstreamer_gzip_writer *) streamer;

	Assert(mystreamer->base.bbs_next == NULL);
	Assert(mystreamer->gzfile == NULL);

	pfree(mystreamer->pathname);
	pfree(mystreamer);
}

/*
 * libz 错误报告的辅助函数。
       */
static const char *
get_gz_error(gzFile gzf)
{
	int			errnum;
	const char *errmsg;

	errmsg = gzerror(gzf, &errnum);
	if (errnum == Z_ERRNO)
		return strerror(errno);
	else
		return errmsg;
}
#endif

/*
 * 创建一个新的基础备份流，执行 gzip 压缩块的解压。
                       */
bbstreamer *
bbstreamer_gzip_decompressor_new(bbstreamer *next)
{
#ifdef HAVE_LIBZ
	bbstreamer_gzip_decompressor *streamer;
	z_stream   *zs;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_gzip_decompressor));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_gzip_decompressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);

	/* 初始化用于解压的内部流状态            */
	zs = &streamer->zstream;
	zs->zalloc = gzip_palloc;
	zs->zfree = gzip_pfree;
	zs->next_out = (uint8 *) streamer->base.bbs_buffer.data;
	zs->avail_out = streamer->base.bbs_buffer.maxlen;

	/*
	 * 数据压缩使用 deflateInit2 初始化以请求 gzip 头。类似地，
	 * 我们使用 inflateInit2 来初始化数据解压。
	 *
	 * 根据 inflateInit2 的文档，第二个参数是 "windowBits"，
	 * 其值必须大于或等于压缩数据时提供的值，因此我们
	 * 为安全起见使用最大可能值。
	                                                                             */
	if (inflateInit2(zs, 15 + 16) != Z_OK)
		pg_fatal("could not initialize compression library");

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "gzip");
	return NULL;				/* keep compiler quiet */
#endif
}

#ifdef HAVE_LIBZ
/*
 * 将输入数据解压到输出缓冲区，直到输入数据耗尽。
 * 每当输出缓冲区满时，将解压后的数据传递给下一个流处理器。
             */
static void
bbstreamer_gzip_decompressor_content(bbstreamer *streamer,
									 bbstreamer_member *member,
									 const char *data, int len,
									 bbstreamer_archive_context context)
{
	bbstreamer_gzip_decompressor *mystreamer;
	z_stream   *zs;

	mystreamer = (bbstreamer_gzip_decompressor *) streamer;

	zs = &mystreamer->zstream;
	zs->next_in = (uint8 *) data;
	zs->avail_in = len;

	/* 处理当前块           */
	while (zs->avail_in > 0)
	{
		int			res;

		Assert(mystreamer->bytes_written < mystreamer->base.bbs_buffer.maxlen);

		zs->next_out = (uint8 *)
			mystreamer->base.bbs_buffer.data + mystreamer->bytes_written;
		zs->avail_out =
			mystreamer->base.bbs_buffer.maxlen - mystreamer->bytes_written;

		/*
		 * 此调用从 zs->next_in 开始解压数据，并更新 zs->next_in
		 * 和 zs->avail_in。它从 zs->next_out 开始生成输出数据，并相应地
		 * 更新 zs->next_out 和 zs->avail_out。
		                    */
		res = inflate(zs, Z_NO_FLUSH);

		if (res == Z_STREAM_ERROR)
			pg_log_error("could not decompress data: %s", zs->msg);

		mystreamer->bytes_written =
			mystreamer->base.bbs_buffer.maxlen - zs->avail_out;

		/* If output buffer is full then pass data to next streamer */
		if (mystreamer->bytes_written >= mystreamer->base.bbs_buffer.maxlen)
		{
			bbstreamer_content(mystreamer->base.bbs_next, member,
							   mystreamer->base.bbs_buffer.data,
							   mystreamer->base.bbs_buffer.maxlen, context);
			mystreamer->bytes_written = 0;
		}
	}
}

/*
 * 流结束处理。
        */
static void
bbstreamer_gzip_decompressor_finalize(bbstreamer *streamer)
{
	bbstreamer_gzip_decompressor *mystreamer;

	mystreamer = (bbstreamer_gzip_decompressor *) streamer;

	/*
	 * End of the stream, if there is some pending data in output buffers then
	 * we must forward it to next streamer.
	 */
	bbstreamer_content(mystreamer->base.bbs_next, NULL,
					   mystreamer->base.bbs_buffer.data,
					   mystreamer->base.bbs_buffer.maxlen,
					   BBSTREAMER_UNKNOWN);

	bbstreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * Free memory.
 */
static void
bbstreamer_gzip_decompressor_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}

/*
 * 包装函数，用于调整 palloc 的签名以匹配 libz 的期望。
           */
static void *
gzip_palloc(void *opaque, unsigned items, unsigned size)
{
	return palloc(items * size);
}

/*
 * 包装函数，用于调整 pfree 的签名以匹配 libz 的期望。
           */
static void
gzip_pfree(void *opaque, void *address)
{
	pfree(address);
}
#endif
