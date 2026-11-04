/*-------------------------------------------------------------------------
 *
 * bbstreamer_zstd.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_zstd.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "bbstreamer.h"
#include "common/logging.h"

#ifdef USE_ZSTD

typedef struct bbstreamer_zstd_frame
{
	bbstreamer	base;

	ZSTD_CCtx  *cctx;
	ZSTD_DCtx  *dctx;
	ZSTD_outBuffer zstd_outBuf;
} bbstreamer_zstd_frame;

static void bbstreamer_zstd_compressor_content(bbstreamer *streamer,
											   bbstreamer_member *member,
											   const char *data, int len,
											   bbstreamer_archive_context context);
static void bbstreamer_zstd_compressor_finalize(bbstreamer *streamer);
static void bbstreamer_zstd_compressor_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_zstd_compressor_ops = {
	.content = bbstreamer_zstd_compressor_content,
	.finalize = bbstreamer_zstd_compressor_finalize,
	.free = bbstreamer_zstd_compressor_free
};

static void bbstreamer_zstd_decompressor_content(bbstreamer *streamer,
												 bbstreamer_member *member,
												 const char *data, int len,
												 bbstreamer_archive_context context);
static void bbstreamer_zstd_decompressor_finalize(bbstreamer *streamer);
static void bbstreamer_zstd_decompressor_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_zstd_decompressor_ops = {
	.content = bbstreamer_zstd_decompressor_content,
	.finalize = bbstreamer_zstd_decompressor_finalize,
	.free = bbstreamer_zstd_decompressor_free
};
#endif

/*
 * 创建一个新的基础备份流，执行 zstd 压缩 tar             
 * 块。 
 */
bbstreamer *
bbstreamer_zstd_compressor_new(bbstreamer *next, pg_compress_specification *compress)
{
#ifdef USE_ZSTD
	bbstreamer_zstd_frame *streamer;
	size_t		ret;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_zstd_frame));

	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_zstd_compressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	enlargeStringInfo(&streamer->base.bbs_buffer, ZSTD_DStreamOutSize());

	streamer->cctx = ZSTD_createCCtx();
	if (!streamer->cctx)
		pg_fatal("could not create zstd compression context");

	/* 设置压缩级别    */
	ret = ZSTD_CCtx_setParameter(streamer->cctx, ZSTD_c_compressionLevel,
								 compress->level);
	if (ZSTD_isError(ret))
		pg_fatal("could not set zstd compression level to %d: %s",
				 compress->level, ZSTD_getErrorName(ret));

	/* 指定时设置工作者数量 */
	if ((compress->options & PG_COMPRESSION_OPTION_WORKERS) != 0)
	{
		/*
		 * 在较旧版本的 libzstd 中，此选项不存在，       
		 * 尝试设置它会失败。对于新版本，如果编译时    
		 * 没有线程支持，同样会失败。
		 */
		ret = ZSTD_CCtx_setParameter(streamer->cctx, ZSTD_c_nbWorkers,
									 compress->workers);
		if (ZSTD_isError(ret))
			pg_fatal("could not set compression worker count to %d: %s",
					 compress->workers, ZSTD_getErrorName(ret));
	}

	if ((compress->options & PG_COMPRESSION_OPTION_LONG_DISTANCE) != 0)
	{
		ret = ZSTD_CCtx_setParameter(streamer->cctx,
									 ZSTD_c_enableLongDistanceMatching,
									 compress->long_distance);
		if (ZSTD_isError(ret))
		{
			pg_log_error("could not enable long-distance mode: %s",
						 ZSTD_getErrorName(ret));
			exit(1);
		}
	}

	/* 初始化 ZSTD 输出缓冲区。  */
	streamer->zstd_outBuf.dst = streamer->base.bbs_buffer.data;
	streamer->zstd_outBuf.size = streamer->base.bbs_buffer.maxlen;
	streamer->zstd_outBuf.pos = 0;

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "ZSTD");
	return NULL;				/* 让编译器安静  */
#endif
}

#ifdef USE_ZSTD
/*
 * 压缩输入数据至输出缓冲区。  
 *
 * 每次调用时根据输入数据长度计算压缩界限，      
 * 确保输出缓冲区有足够容量容纳压缩后的数据。  
 * 如果输出缓冲区容量不足以容纳压缩界限，    
 * 则将输出缓冲区内容转发给下一个流处理器，      
 * 并清空缓冲区。                                  
 */
static void
bbstreamer_zstd_compressor_content(bbstreamer *streamer,
								   bbstreamer_member *member,
								   const char *data, int len,
								   bbstreamer_archive_context context)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;
	ZSTD_inBuffer inBuf = {data, len, 0};

	while (inBuf.pos < inBuf.size)
	{
		size_t		yet_to_flush;
		size_t		max_needed = ZSTD_compressBound(inBuf.size - inBuf.pos);

		/*
		 * 如果输出缓冲区剩余空间不足，则将压缩后的字节
		 * 发送给下一个流处理器，并清空缓冲区。
		 */
		if (mystreamer->zstd_outBuf.size - mystreamer->zstd_outBuf.pos <
			max_needed)
		{
			bbstreamer_content(mystreamer->base.bbs_next, member,
							   mystreamer->zstd_outBuf.dst,
							   mystreamer->zstd_outBuf.pos,
							   context);

			/* 重置 ZSTD 输出缓冲区   */
			mystreamer->zstd_outBuf.dst = mystreamer->base.bbs_buffer.data;
			mystreamer->zstd_outBuf.size = mystreamer->base.bbs_buffer.maxlen;
			mystreamer->zstd_outBuf.pos = 0;
		}

		yet_to_flush =
			ZSTD_compressStream2(mystreamer->cctx, &mystreamer->zstd_outBuf,
								 &inBuf, ZSTD_e_continue);

		if (ZSTD_isError(yet_to_flush))
			pg_log_error("could not compress data: %s",
						 ZSTD_getErrorName(yet_to_flush));
	}
}

/*
 * 流结束处理。
        */
static void
bbstreamer_zstd_compressor_finalize(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;
	size_t		yet_to_flush;

	do
	{
		ZSTD_inBuffer in = {NULL, 0, 0};
		size_t		max_needed = ZSTD_compressBound(0);

		/*
		 * 如果输出缓冲区剩余空间不足，则将压缩后的字节
		 * 发送给下一个流处理器，并清空缓冲区。
		 */
		if (mystreamer->zstd_outBuf.size - mystreamer->zstd_outBuf.pos <
			max_needed)
		{
			bbstreamer_content(mystreamer->base.bbs_next, NULL,
							   mystreamer->zstd_outBuf.dst,
							   mystreamer->zstd_outBuf.pos,
							   BBSTREAMER_UNKNOWN);

			/* 重置 ZSTD 输出缓冲区   */
			mystreamer->zstd_outBuf.dst = mystreamer->base.bbs_buffer.data;
			mystreamer->zstd_outBuf.size = mystreamer->base.bbs_buffer.maxlen;
			mystreamer->zstd_outBuf.pos = 0;
		}

		yet_to_flush = ZSTD_compressStream2(mystreamer->cctx,
											&mystreamer->zstd_outBuf,
											&in, ZSTD_e_end);

		if (ZSTD_isError(yet_to_flush))
			pg_log_error("could not compress data: %s",
						 ZSTD_getErrorName(yet_to_flush));

	} while (yet_to_flush > 0);

	/* 确保把所有剩余字节传给下一个流处理器。   */
	if (mystreamer->zstd_outBuf.pos > 0)
		bbstreamer_content(mystreamer->base.bbs_next, NULL,
						   mystreamer->zstd_outBuf.dst,
						   mystreamer->zstd_outBuf.pos,
						   BBSTREAMER_UNKNOWN);

	bbstreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * 释放内存
 */
static void
bbstreamer_zstd_compressor_free(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;

	bbstreamer_free(streamer->bbs_next);
	ZSTD_freeCCtx(mystreamer->cctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif

/*
 * 创建一个新的基础备份流，执行 zstd 压缩块的解压。
 *                   
 */
bbstreamer *
bbstreamer_zstd_decompressor_new(bbstreamer *next)
{
#ifdef USE_ZSTD
	bbstreamer_zstd_frame *streamer;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_zstd_frame));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_zstd_decompressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	enlargeStringInfo(&streamer->base.bbs_buffer, ZSTD_DStreamOutSize());

	streamer->dctx = ZSTD_createDCtx();
	if (!streamer->dctx)
		pg_fatal("could not create zstd decompression context");

	/* 初始化 ZSTD 输出缓冲区。  */
	streamer->zstd_outBuf.dst = streamer->base.bbs_buffer.data;
	streamer->zstd_outBuf.size = streamer->base.bbs_buffer.maxlen;
	streamer->zstd_outBuf.pos = 0;

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "ZSTD");
	return NULL;				/* 让编译器安静  */
#endif
}

#ifdef USE_ZSTD
/*
 * 将输入数据解压到输出缓冲区，直到输入耗尽。     
 * 每当输出缓冲区已满时，将解压后的数据传递给         
 * 下一个流处理器
 */
static void
bbstreamer_zstd_decompressor_content(bbstreamer *streamer,
									 bbstreamer_member *member,
									 const char *data, int len,
									 bbstreamer_archive_context context)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;
	ZSTD_inBuffer inBuf = {data, len, 0};

	while (inBuf.pos < inBuf.size)
	{
		size_t		ret;

		/*
		 * 若输出缓冲区已满，则转发给下一个流处理器，   
		 * 并更新该缓冲区。     
		 */
		if (mystreamer->zstd_outBuf.pos >= mystreamer->zstd_outBuf.size)
		{
			bbstreamer_content(mystreamer->base.bbs_next, member,
							   mystreamer->zstd_outBuf.dst,
							   mystreamer->zstd_outBuf.pos,
							   context);

			/* 重置 ZSTD 输出缓冲区   */
			mystreamer->zstd_outBuf.dst = mystreamer->base.bbs_buffer.data;
			mystreamer->zstd_outBuf.size = mystreamer->base.bbs_buffer.maxlen;
			mystreamer->zstd_outBuf.pos = 0;
		}

		ret = ZSTD_decompressStream(mystreamer->dctx,
									&mystreamer->zstd_outBuf, &inBuf);

		if (ZSTD_isError(ret))
			pg_log_error("could not decompress data: %s",
						 ZSTD_getErrorName(ret));
	}
}

/*
 * 流结束处理。
        */
static void
bbstreamer_zstd_decompressor_finalize(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;

	/*
	 * 流结束时，若输出缓冲区中仍有待发数据，则须将其  
	 * 转发给下一个流处理器。   
	 */
	if (mystreamer->zstd_outBuf.pos > 0)
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
bbstreamer_zstd_decompressor_free(bbstreamer *streamer)
{
	bbstreamer_zstd_frame *mystreamer = (bbstreamer_zstd_frame *) streamer;

	bbstreamer_free(streamer->bbs_next);
	ZSTD_freeDCtx(mystreamer->dctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif
