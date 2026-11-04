/*-------------------------------------------------------------------------
 *
 * bbstreamer_lz4.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_lz4.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#ifdef USE_LZ4
#include <lz4frame.h>
#endif

#include "bbstreamer.h"
#include "common/logging.h"
#include "common/file_perm.h"
#include "common/string.h"

#ifdef USE_LZ4
typedef struct bbstreamer_lz4_frame
{
	bbstreamer	base;

	LZ4F_compressionContext_t cctx;
	LZ4F_decompressionContext_t dctx;
	LZ4F_preferences_t prefs;

	size_t		bytes_written;
	bool		header_written;
} bbstreamer_lz4_frame;

static void bbstreamer_lz4_compressor_content(bbstreamer *streamer,
											  bbstreamer_member *member,
											  const char *data, int len,
											  bbstreamer_archive_context context);
static void bbstreamer_lz4_compressor_finalize(bbstreamer *streamer);
static void bbstreamer_lz4_compressor_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_lz4_compressor_ops = {
	.content = bbstreamer_lz4_compressor_content,
	.finalize = bbstreamer_lz4_compressor_finalize,
	.free = bbstreamer_lz4_compressor_free
};

static void bbstreamer_lz4_decompressor_content(bbstreamer *streamer,
												bbstreamer_member *member,
												const char *data, int len,
												bbstreamer_archive_context context);
static void bbstreamer_lz4_decompressor_finalize(bbstreamer *streamer);
static void bbstreamer_lz4_decompressor_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_lz4_decompressor_ops = {
	.content = bbstreamer_lz4_decompressor_content,
	.finalize = bbstreamer_lz4_decompressor_finalize,
	.free = bbstreamer_lz4_decompressor_free
};
#endif

/*
 * 创建一个新的基础备份流，执行 lz4 压缩 tar             
 * 块。 
 */
bbstreamer *
bbstreamer_lz4_compressor_new(bbstreamer *next, pg_compress_specification *compress)
{
#ifdef USE_LZ4
	bbstreamer_lz4_frame *streamer;
	LZ4F_errorCode_t ctxError;
	LZ4F_preferences_t *prefs;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_lz4_frame));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_lz4_compressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	streamer->header_written = false;

	/* 初始化流压缩偏好设置            */
	prefs = &streamer->prefs;
	memset(prefs, 0, sizeof(LZ4F_preferences_t));
	prefs->frameInfo.blockSizeID = LZ4F_max256KB;
	prefs->compressionLevel = compress->level;

	ctxError = LZ4F_createCompressionContext(&streamer->cctx, LZ4F_VERSION);
	if (LZ4F_isError(ctxError))
		pg_log_error("could not create lz4 compression context: %s",
					 LZ4F_getErrorName(ctxError));

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "LZ4");
	return NULL;				/* 让编译器安静  */
#endif
}

#ifdef USE_LZ4
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
bbstreamer_lz4_compressor_content(bbstreamer *streamer,
								  bbstreamer_member *member,
								  const char *data, int len,
								  bbstreamer_archive_context context)
{
	bbstreamer_lz4_frame *mystreamer;
	uint8	   *next_in,
			   *next_out;
	size_t		out_bound,
				compressed_size,
				avail_out;

	mystreamer = (bbstreamer_lz4_frame *) streamer;
	next_in = (uint8 *) data;

	/* 在处理第一个输入块之前写入头。         */
	if (!mystreamer->header_written)
	{
		compressed_size = LZ4F_compressBegin(mystreamer->cctx,
											 (uint8 *) mystreamer->base.bbs_buffer.data,
											 mystreamer->base.bbs_buffer.maxlen,
											 &mystreamer->prefs);

		if (LZ4F_isError(compressed_size))
			pg_log_error("could not write lz4 header: %s",
						 LZ4F_getErrorName(compressed_size));

		mystreamer->bytes_written += compressed_size;
		mystreamer->header_written = true;
	}

	/*
	 * 根据写入输出缓冲区的字节数，                        
	 * 更新其偏移量和容量。 
	 */
	next_out = (uint8 *) mystreamer->base.bbs_buffer.data + mystreamer->bytes_written;
	avail_out = mystreamer->base.bbs_buffer.maxlen - mystreamer->bytes_written;

	/*
	 * 计算压缩界限，确保输出缓冲区有足够的容量
	 * 以便 LZ4F_compressUpdate 成功。如果需要，将内容转发给下一个
	 * 流处理器并清空缓冲区。
	                          */
	out_bound = LZ4F_compressBound(len, &mystreamer->prefs);
	if (avail_out < out_bound)
	{
		bbstreamer_content(mystreamer->base.bbs_next, member,
						   mystreamer->base.bbs_buffer.data,
						   mystreamer->bytes_written,
						   context);

		/* 若不足输出界限则扩大缓冲区。     */
		if (mystreamer->base.bbs_buffer.maxlen < out_bound)
			enlargeStringInfo(&mystreamer->base.bbs_buffer, out_bound);

		avail_out = mystreamer->base.bbs_buffer.maxlen;
		mystreamer->bytes_written = 0;
		next_out = (uint8 *) mystreamer->base.bbs_buffer.data;
	}

	/*
	 * 此调用压缩从 next_in 开始的数据，并生成从          
	 * next_out 开始的输出。它期望调用者通过                   
	 * 参数 len 和 avail_out 提供输入缓冲区大小和输出        
	 * 缓冲区容量。
	 *
	 * 它返回已压缩到输出缓冲区的字节数。        
	 */
	compressed_size = LZ4F_compressUpdate(mystreamer->cctx,
										  next_out, avail_out,
										  next_in, len, NULL);

	if (LZ4F_isError(compressed_size))
		pg_log_error("could not compress data: %s",
					 LZ4F_getErrorName(compressed_size));

	mystreamer->bytes_written += compressed_size;
}

/*
 * 流结束处理。
        */
static void
bbstreamer_lz4_compressor_finalize(bbstreamer *streamer)
{
	bbstreamer_lz4_frame *mystreamer;
	uint8	   *next_out;
	size_t		footer_bound,
				compressed_size,
				avail_out;

	mystreamer = (bbstreamer_lz4_frame *) streamer;

	/* 计算尾部界限并更新输出缓冲区。           */
	footer_bound = LZ4F_compressBound(0, &mystreamer->prefs);
	if ((mystreamer->base.bbs_buffer.maxlen - mystreamer->bytes_written) <
		footer_bound)
	{
		bbstreamer_content(mystreamer->base.bbs_next, NULL,
						   mystreamer->base.bbs_buffer.data,
						   mystreamer->bytes_written,
						   BBSTREAMER_UNKNOWN);

		/* 若不足帧尾界限则扩大缓冲区。        */
		if (mystreamer->base.bbs_buffer.maxlen < footer_bound)
			enlargeStringInfo(&mystreamer->base.bbs_buffer, footer_bound);

		avail_out = mystreamer->base.bbs_buffer.maxlen;
		mystreamer->bytes_written = 0;
		next_out = (uint8 *) mystreamer->base.bbs_buffer.data;
	}
	else
	{
		next_out = (uint8 *) mystreamer->base.bbs_buffer.data + mystreamer->bytes_written;
		avail_out = mystreamer->base.bbs_buffer.maxlen - mystreamer->bytes_written;
	}

	/*
	 * 完成帧的处理，并刷新压缩上下文中剩余的全部    
	 * 数据  
	 */
	compressed_size = LZ4F_compressEnd(mystreamer->cctx,
									   next_out, avail_out, NULL);

	if (LZ4F_isError(compressed_size))
		pg_log_error("could not end lz4 compression: %s",
					 LZ4F_getErrorName(compressed_size));

	mystreamer->bytes_written += compressed_size;

	bbstreamer_content(mystreamer->base.bbs_next, NULL,
					   mystreamer->base.bbs_buffer.data,
					   mystreamer->bytes_written,
					   BBSTREAMER_UNKNOWN);

	bbstreamer_finalize(mystreamer->base.bbs_next);
}

/*
 * 释放内存
 */
static void
bbstreamer_lz4_compressor_free(bbstreamer *streamer)
{
	bbstreamer_lz4_frame *mystreamer;

	mystreamer = (bbstreamer_lz4_frame *) streamer;
	bbstreamer_free(streamer->bbs_next);
	LZ4F_freeCompressionContext(mystreamer->cctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif

/*
 * 创建一个新的基础备份流，执行 lz4 压缩块的解压。
 *                   
 */
bbstreamer *
bbstreamer_lz4_decompressor_new(bbstreamer *next)
{
#ifdef USE_LZ4
	bbstreamer_lz4_frame *streamer;
	LZ4F_errorCode_t ctxError;

	Assert(next != NULL);

	streamer = palloc0(sizeof(bbstreamer_lz4_frame));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_lz4_decompressor_ops;

	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);

	/* 初始化用于解压的内部流状态            */
	ctxError = LZ4F_createDecompressionContext(&streamer->dctx, LZ4F_VERSION);
	if (LZ4F_isError(ctxError))
		pg_fatal("could not initialize compression library: %s",
				 LZ4F_getErrorName(ctxError));

	return &streamer->base;
#else
	pg_fatal("this build does not support compression with %s", "LZ4");
	return NULL;				/* 让编译器安静  */
#endif
}

#ifdef USE_LZ4
/*
 * 将输入数据解压到输出缓冲区，直到输入耗尽。     
 * 每当输出缓冲区已满时，将解压后的数据传递给         
 * 下一个流处理器
 */
static void
bbstreamer_lz4_decompressor_content(bbstreamer *streamer,
									bbstreamer_member *member,
									const char *data, int len,
									bbstreamer_archive_context context)
{
	bbstreamer_lz4_frame *mystreamer;
	uint8	   *next_in,
			   *next_out;
	size_t		avail_in,
				avail_out;

	mystreamer = (bbstreamer_lz4_frame *) streamer;
	next_in = (uint8 *) data;
	next_out = (uint8 *) mystreamer->base.bbs_buffer.data + mystreamer->bytes_written;
	avail_in = len;
	avail_out = mystreamer->base.bbs_buffer.maxlen - mystreamer->bytes_written;

	while (avail_in > 0)
	{
		size_t		ret,
					read_size,
					out_size;

		read_size = avail_in;
		out_size = avail_out;

		/*
		 * 此调用从 next_in 开始解压数据，并从 next_out        
		 * 开始生成输出数据。它期望调用者通过           
		 * 参数 len 和 avail_out 提供输入缓冲区大小             
		 * 和输出缓冲区的总容量。                        
		 *              
		 *
		 * 根据 LZ4 的文档，参数 read_size 和 out_size           
		 * 表现为双重参数。返回时，从输入缓冲区消耗的    
		 * 字节数将写回 read_size，解压到输出缓冲区的     
		 * 字节数将写回 out_size。                                    
		 *                          
		 */
		ret = LZ4F_decompress(mystreamer->dctx,
							  next_out, &out_size,
							  next_in, &read_size, NULL);

		if (LZ4F_isError(ret))
			pg_log_error("could not decompress data: %s",
						 LZ4F_getErrorName(ret));

		/* 根据已消耗的字节数更新输入缓冲区      */
		avail_in -= read_size;
		next_in += read_size;

		mystreamer->bytes_written += out_size;

		/*
		 * 若输出缓冲区已满，则转发给下一个流处理器，   
		 * 并更新该缓冲区。     
		 */
		if (mystreamer->bytes_written >= mystreamer->base.bbs_buffer.maxlen)
		{
			bbstreamer_content(mystreamer->base.bbs_next, member,
							   mystreamer->base.bbs_buffer.data,
							   mystreamer->base.bbs_buffer.maxlen,
							   context);

			avail_out = mystreamer->base.bbs_buffer.maxlen;
			mystreamer->bytes_written = 0;
			next_out = (uint8 *) mystreamer->base.bbs_buffer.data;
		}
		else
		{
			avail_out = mystreamer->base.bbs_buffer.maxlen - mystreamer->bytes_written;
			next_out += mystreamer->bytes_written;
		}
	}
}

/*
 * 流结束处理。
        */
static void
bbstreamer_lz4_decompressor_finalize(bbstreamer *streamer)
{
	bbstreamer_lz4_frame *mystreamer;

	mystreamer = (bbstreamer_lz4_frame *) streamer;

	/*
	 * 流结束时，若输出缓冲区中仍有待发数据，则须将其  
	 * 转发给下一个流处理器。   
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
bbstreamer_lz4_decompressor_free(bbstreamer *streamer)
{
	bbstreamer_lz4_frame *mystreamer;

	mystreamer = (bbstreamer_lz4_frame *) streamer;
	bbstreamer_free(streamer->bbs_next);
	LZ4F_freeDecompressionContext(mystreamer->dctx);
	pfree(streamer->bbs_buffer.data);
	pfree(streamer);
}
#endif
