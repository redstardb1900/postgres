/*-------------------------------------------------------------------------
 *
 * bbstreamer_gzip.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
 * 创建一个用 gzip 压缩数据并写入文件的 bbstreamer。        
 *              
 *
 * 与 bbstreamer_plain_writer_new 一样，pathname 总是用于        
 * 错误报告；若 file 为 NULL，它也会被打开并关闭，从而 
 * 使数据可以写入该处。               
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
	return NULL;				/* 让编译器安静  */
#endif
}

#ifdef HAVE_LIBZ
/*
 * 将归档内容写入 gzip 文件  
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
		/* 若写入未设置 errno，则假定为磁盘空间不足  */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to compressed file \"%s\": %s",
				 mystreamer->pathname, get_gz_error(mystreamer->gzfile));
	}
}

/*
 * 写入 gzip 文件时的归档结束处理仅包含调用             
 * gzclose。      
 *
 * 是我们打开文件还是由调用者打开，并没有区别，     
 * 因为 libz 无法避免关闭底层文件句柄。但请注意，     
 * bbstreamer_gzip_writer_new() 使用了 dup() 来解决此问题，      
 * 因此从调用者的角度来看，其行为与                        
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
 * 创建新的基础备份流处理器，用于对 gzip 压缩块执   
 * 行解压。      
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
	 * 数据压缩使用 deflateInit2 初始化，以请求 gzip           
	 * 头部。类似地，我们用 inflateInit2 来初始化数据 
	 * 解压。     
	 *
	 * 根据 inflateInit2 的文档，第二个参数是            
	 * "windowBits"，其值必须大于等于压缩数据时所提供的   
	 * 值，因此为安全起见我们使用了最大可能值。    
	 *                           
	 */
	if (inflateInit2(zs, 15 + 16) != Z_OK)
		pg_fatal("could not initialize compression library");

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "gzip");
	return NULL;				/* 让编译器安静  */
#endif
}

#ifdef HAVE_LIBZ
/*
 * 将输入数据解压到输出缓冲区，直到输入耗尽。     
 * 每当输出缓冲区满时，将解压后的数据传递给下一个   
 * 流处理器。      
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
		 * 此调用解压从 zs->next_in 开始的数据，并更新     
		 * zs->next_in 和 zs->avail_in。输出数据从 zs->next_out      
		 * 开始，并相应更新 zs->next_out 和 zs->avail_out。
		 *             
		 */
		res = inflate(zs, Z_NO_FLUSH);

		if (res == Z_STREAM_ERROR)
			pg_log_error("could not decompress data: %s", zs->msg);

		mystreamer->bytes_written =
			mystreamer->base.bbs_buffer.maxlen - zs->avail_out;

		/* 输出缓冲区满则将数据传给下一流处理器   */
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
	 * 到达流的末尾时，如果输出缓冲区中还有未决数据，  
	 * 必须转发给下一个流处理器
	 */
	bbstreamer_content(mystreamer->base.bbs_next, NULL,
					   mystreamer->base.bbs_buffer.data,
					   mystreamer->base.bbs_buffer.maxlen,
					   BBSTREAMER_UNKNOWN);

	bbstreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * 释放内存
 */
static void
bbstreamer_gzip_decompressor_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}

/*
 * 包装函数，用于调整 palloc 签名以匹配 libz 的要求。 
 *         
 */
static void *
gzip_palloc(void *opaque, unsigned items, unsigned size)
{
	return palloc(items * size);
}

/*
 * 包装函数，用于调整 pfree 签名以匹配 libz 的要求。 
 *         
 */
static void
gzip_pfree(void *opaque, void *address)
{
	pfree(address);
}
#endif
