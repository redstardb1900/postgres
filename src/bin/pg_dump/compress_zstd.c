/*-------------------------------------------------------------------------
 *
 * compress_zstd.c
 *	 Routines for archivers to write a Zstd compressed data stream.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_zstd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include <unistd.h>

#include "pg_backup_utils.h"
#include "compress_zstd.h"

#ifndef USE_ZSTD

void
InitCompressorZstd(CompressorState *cs, const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "ZSTD");
}

void
InitCompressFileHandleZstd(CompressFileHandle *CFH, const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "ZSTD");
}

#else

#include <zstd.h>

typedef struct ZstdCompressorState
{
	/* 用于读取/写入压缩数据的普通文件 */
	FILE	   *fp;

	ZSTD_CStream *cstream;
	ZSTD_DStream *dstream;
	ZSTD_outBuffer output;
	ZSTD_inBuffer input;

	/* 指向静态字符串的指针，如 strerror() 返回的，供 Zstd_write() 使用 */
	const char *zstderror;
} ZstdCompressorState;

static ZSTD_CStream *_ZstdCStreamParams(pg_compress_specification compress);
static void EndCompressorZstd(ArchiveHandle *AH, CompressorState *cs);
static void WriteDataToArchiveZstd(ArchiveHandle *AH, CompressorState *cs,
								   const void *data, size_t dLen);
static void ReadDataFromArchiveZstd(ArchiveHandle *AH, CompressorState *cs);

static void
_Zstd_CCtx_setParam_or_die(ZSTD_CStream *cstream,
						   ZSTD_cParameter param, int value, char *paramname)
{
	size_t		res;

	res = ZSTD_CCtx_setParameter(cstream, param, value);
	if (ZSTD_isError(res))
		pg_fatal("could not set compression parameter \"%s\": %s",
				 paramname, ZSTD_getErrorName(res));
}

/* 返回根据参数设置相应参数的压缩流 */
static ZSTD_CStream *
_ZstdCStreamParams(pg_compress_specification compress)
{
	ZSTD_CStream *cstream;

	cstream = ZSTD_createCStream();
	if (cstream == NULL)
		pg_fatal("could not initialize compression library");

	_Zstd_CCtx_setParam_or_die(cstream, ZSTD_c_compressionLevel,
							   compress.level, "level");

	if (compress.options & PG_COMPRESSION_OPTION_LONG_DISTANCE)
		_Zstd_CCtx_setParam_or_die(cstream,
								   ZSTD_c_enableLongDistanceMatching,
								   compress.long_distance, "long");

	return cstream;
}

/* WriteDataToArchiveZstd 和 EndCompressorZstd 的辅助函数 */
static void
_ZstdWriteCommon(ArchiveHandle *AH, CompressorState *cs, bool flush)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;
	ZSTD_inBuffer *input = &zstdcs->input;
	ZSTD_outBuffer *output = &zstdcs->output;

	/* 当有输入时循环，或直到刷新完成 */
	while (input->pos != input->size || flush)
	{
		size_t		res;

		output->pos = 0;
		res = ZSTD_compressStream2(zstdcs->cstream, output,
								   input, flush ? ZSTD_e_end : ZSTD_e_continue);

		if (ZSTD_isError(res))
			pg_fatal("could not compress data: %s", ZSTD_getErrorName(res));

		/*
		 * 额外谨慎：避免零长度块，因为零长度块在自定义格式中是 EOF
		 * 标记。这不应发生，
		 * 但以防万一...
		 */
		if (output->pos > 0)
			cs->writeF(AH, output->dst, output->pos);

		if (res == 0)
			break;				/* 帧结束或所有输入已消费 */
	}
}

static void
EndCompressorZstd(ArchiveHandle *AH, CompressorState *cs)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;

	if (cs->readF != NULL)
	{
		Assert(zstdcs->cstream == NULL);
		ZSTD_freeDStream(zstdcs->dstream);
		pg_free(unconstify(void *, zstdcs->input.src));
	}
	else if (cs->writeF != NULL)
	{
		Assert(zstdcs->dstream == NULL);
		_ZstdWriteCommon(AH, cs, true);
		ZSTD_freeCStream(zstdcs->cstream);
	}

	/* 输出缓冲区可能在任一模式下分配 */
	pg_free(zstdcs->output.dst);
	pg_free(zstdcs);
}

static void
WriteDataToArchiveZstd(ArchiveHandle *AH, CompressorState *cs,
					   const void *data, size_t dLen)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;

	zstdcs->input.src = data;
	zstdcs->input.size = dLen;
	zstdcs->input.pos = 0;

	_ZstdWriteCommon(AH, cs, false);
}

static void
ReadDataFromArchiveZstd(ArchiveHandle *AH, CompressorState *cs)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) cs->private_data;
	ZSTD_outBuffer *output = &zstdcs->output;
	ZSTD_inBuffer *input = &zstdcs->input;
	size_t		input_allocated_size = ZSTD_DStreamInSize();
	size_t		res;

	for (;;)
	{
		size_t		cnt;

		/*
		 * 读取压缩数据。注意 readF 可以调整缓冲区大小；
		 * 新大小会被记录并在后续循环中使用。
		 */
		input->size = input_allocated_size;
		cnt = cs->readF(AH, (char **) unconstify(void **, &input->src), &input->size);

		/* 确保 readF 没有缩小缓冲区 */
		Assert(input->size >= input_allocated_size);
		input_allocated_size = input->size;
		input->size = cnt;
		input->pos = 0;

		if (cnt == 0)
			break;

		/* 现在解压 */
		while (input->pos < input->size)
		{
			output->pos = 0;
			res = ZSTD_decompressStream(zstdcs->dstream, output, input);
			if (ZSTD_isError(res))
				pg_fatal("could not decompress data: %s", ZSTD_getErrorName(res));

			/*
			 * 然后将解压后的数据写入输出句柄
			 */
			((char *) output->dst)[output->pos] = '\0';
			ahwrite(output->dst, 1, output->pos, AH);

			if (res == 0)
				break;			/* 帧结束 */
		}
	}
}

/* 支持 Zstd 压缩数据 I/O 的公共例程 */
void
InitCompressorZstd(CompressorState *cs,
				   const pg_compress_specification compression_spec)
{
	ZstdCompressorState *zstdcs;

	cs->readData = ReadDataFromArchiveZstd;
	cs->writeData = WriteDataToArchiveZstd;
	cs->end = EndCompressorZstd;

	cs->compression_spec = compression_spec;

	zstdcs = (ZstdCompressorState *) pg_malloc0(sizeof(*zstdcs));
	cs->private_data = zstdcs;

	/* 期望 readF/writeF 中有且仅有一个被指定 */
	Assert((cs->readF == NULL) != (cs->writeF == NULL));

	if (cs->readF != NULL)
	{
		zstdcs->dstream = ZSTD_createDStream();
		if (zstdcs->dstream == NULL)
			pg_fatal("could not initialize compression library");

		zstdcs->input.size = ZSTD_DStreamInSize();
		zstdcs->input.src = pg_malloc(zstdcs->input.size);

		/*
		 * output.size 是告知 zstd 可输出的缓冲区大小。
		 * 额外分配一个字节，使 ReadDataFromArchiveZstd() 能够以
		 * 空字符结尾的字符串调用 ahwrite()，这是 ExecuteSqlCommandBuf()
		 * 中的优化情形。
		 */
		zstdcs->output.size = ZSTD_DStreamOutSize();
		zstdcs->output.dst = pg_malloc(zstdcs->output.size + 1);
	}
	else if (cs->writeF != NULL)
	{
		zstdcs->cstream = _ZstdCStreamParams(cs->compression_spec);

		zstdcs->output.size = ZSTD_CStreamOutSize();
		zstdcs->output.dst = pg_malloc(zstdcs->output.size);
		zstdcs->output.pos = 0;
	}
}

/*
 * 压缩流 API
 */

static size_t
Zstd_read_internal(void *ptr, size_t size, CompressFileHandle *CFH, bool exit_on_error)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;
	ZSTD_inBuffer *input = &zstdcs->input;
	ZSTD_outBuffer *output = &zstdcs->output;
	size_t		input_allocated_size = ZSTD_DStreamInSize();
	size_t		res,
				cnt;

	/*
	 * 如果这是对读取函数的第一次调用，则初始化所需的
	 * 数据结构。
	 */
	if (zstdcs->dstream == NULL)
	{
		zstdcs->input.src = pg_malloc0(input_allocated_size);
		zstdcs->dstream = ZSTD_createDStream();
		if (zstdcs->dstream == NULL)
		{
			if (exit_on_error)
				pg_fatal("could not initialize compression library");
			return -1;
		}
	}

	output->size = size;
	output->dst = ptr;
	output->pos = 0;

	for (;;)
	{
		Assert(input->pos <= input->size);
		Assert(input->size <= input_allocated_size);

		/*
		 * 如果输入已完全消费，则从头开始
		 */
		if (input->pos == input->size)
		{
			/* input->size 是 "fread" 产生的数据大小 */
			input->size = 0;
			/* input->pos 是解压已消费的位置 */
			input->pos = 0;
		}

		/* 如果需要生成更多输入，则读取压缩数据 */
		if (input->pos == input->size)
		{
			cnt = fread(unconstify(void *, input->src), 1, input_allocated_size, zstdcs->fp);
			if (ferror(zstdcs->fp))
			{
				if (exit_on_error)
					pg_fatal("could not read from input file: %m");
				return -1;
			}

			input->size = cnt;

			Assert(cnt <= input_allocated_size);

			/* 如果没有更多输入可消费，则完成 */
			if (cnt == 0)
				break;
		}

		while (input->pos < input->size)
		{
			/* 现在解压 */
			res = ZSTD_decompressStream(zstdcs->dstream, output, input);

			if (ZSTD_isError(res))
			{
				if (exit_on_error)
					pg_fatal("could not decompress data: %s", ZSTD_getErrorName(res));
				return -1;
			}

			if (output->pos == output->size)
				break;			/* 输出空间已满 */

			if (res == 0)
				break;			/* 帧结束 */
		}

		if (output->pos == output->size)
			break;				/* 已读取所有能容纳的数据 */
	}

	return output->pos;
}

static void
Zstd_write(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;
	ZSTD_inBuffer *input = &zstdcs->input;
	ZSTD_outBuffer *output = &zstdcs->output;
	size_t		res,
				cnt;

	input->src = ptr;
	input->size = size;
	input->pos = 0;

	if (zstdcs->cstream == NULL)
	{
		zstdcs->output.size = ZSTD_CStreamOutSize();
		zstdcs->output.dst = pg_malloc0(zstdcs->output.size);
		zstdcs->cstream = _ZstdCStreamParams(CFH->compression_spec);
		if (zstdcs->cstream == NULL)
			pg_fatal("could not initialize compression library");
	}

	/* 消费所有输入，稍后刷新 */
	while (input->pos != input->size)
	{
		output->pos = 0;
		res = ZSTD_compressStream2(zstdcs->cstream, output, input, ZSTD_e_continue);
		if (ZSTD_isError(res))
			pg_fatal("could not write to file: %s", ZSTD_getErrorName(res));

		errno = 0;
		cnt = fwrite(output->dst, 1, output->pos, zstdcs->fp);
		if (cnt != output->pos)
		{
			errno = (errno) ? errno : ENOSPC;
			pg_fatal("could not write to file: %m");
		}
	}
}

static int
Zstd_getc(CompressFileHandle *CFH)
{
	unsigned char ret;

	if (CFH->read_func(&ret, 1, CFH) != 1)
		pg_fatal("could not read from input file: end of file");
	return ret;
}

static char *
Zstd_gets(char *buf, int len, CompressFileHandle *CFH)
{
	int			i;

	Assert(len > 0);

	/*
	 * 逐字节读取直到换行符或 EOF。仅用于读取 LO
	 * 列表，而且 I/O 本身是有缓冲的。
	 */
	for (i = 0; i < len - 1; ++i)
	{
		if (Zstd_read_internal(&buf[i], 1, CFH, false) != 1)
			break;
		if (buf[i] == '\n')
		{
			++i;
			break;
		}
	}
	buf[i] = '\0';
	return i > 0 ? buf : NULL;
}

static size_t
Zstd_read(void *ptr, size_t size, CompressFileHandle *CFH)
{
	return Zstd_read_internal(ptr, size, CFH, true);
}

static bool
Zstd_close(CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;
	bool		success = true;

	if (zstdcs->cstream)
	{
		size_t		res,
					cnt;
		ZSTD_inBuffer *input = &zstdcs->input;
		ZSTD_outBuffer *output = &zstdcs->output;

		/* 循环直到压缩缓冲区完全消费 */
		for (;;)
		{
			output->pos = 0;
			res = ZSTD_compressStream2(zstdcs->cstream, output, input, ZSTD_e_end);
			if (ZSTD_isError(res))
			{
				zstdcs->zstderror = ZSTD_getErrorName(res);
				success = false;
				break;
			}

			errno = 0;
			cnt = fwrite(output->dst, 1, output->pos, zstdcs->fp);
			if (cnt != output->pos)
			{
				errno = (errno) ? errno : ENOSPC;
				zstdcs->zstderror = strerror(errno);
				success = false;
				break;
			}

			if (res == 0)
				break;			/* 帧结束 */
		}

		ZSTD_freeCStream(zstdcs->cstream);
		pg_free(zstdcs->output.dst);
	}

	if (zstdcs->dstream)
	{
		ZSTD_freeDStream(zstdcs->dstream);
		pg_free(unconstify(void *, zstdcs->input.src));
	}

	errno = 0;
	if (fclose(zstdcs->fp) != 0)
	{
		zstdcs->zstderror = strerror(errno);
		success = false;
	}

	pg_free(zstdcs);
	CFH->private_data = NULL;
	return success;
}

static bool
Zstd_eof(CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;

	return feof(zstdcs->fp);
}

static bool
Zstd_open(const char *path, int fd, const char *mode,
		  CompressFileHandle *CFH)
{
	FILE	   *fp;
	ZstdCompressorState *zstdcs;

	/*
	 * 清除状态存储，避免在错误返回时 fd 指向非 NULL
	 * 内存。
	 */
	CFH->private_data = NULL;

	zstdcs = (ZstdCompressorState *) pg_malloc_extended(sizeof(*zstdcs),
														MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
	if (!zstdcs)
	{
		errno = ENOMEM;
		return false;
	}

	if (fd >= 0)
		fp = fdopen(dup(fd), mode);
	else
		fp = fopen(path, mode);

	if (fp == NULL)
	{
		pg_free(zstdcs);
		return false;
	}

	zstdcs->fp = fp;
	CFH->private_data = zstdcs;

	return true;
}

static bool
Zstd_open_write(const char *path, const char *mode, CompressFileHandle *CFH)
{
	char		fname[MAXPGPATH];

	sprintf(fname, "%s.zst", path);
	return CFH->open_func(fname, -1, mode, CFH);
}

static const char *
Zstd_get_error(CompressFileHandle *CFH)
{
	ZstdCompressorState *zstdcs = (ZstdCompressorState *) CFH->private_data;

	return zstdcs->zstderror;
}

void
InitCompressFileHandleZstd(CompressFileHandle *CFH,
						   const pg_compress_specification compression_spec)
{
	CFH->open_func = Zstd_open;
	CFH->open_write_func = Zstd_open_write;
	CFH->read_func = Zstd_read;
	CFH->write_func = Zstd_write;
	CFH->gets_func = Zstd_gets;
	CFH->getc_func = Zstd_getc;
	CFH->close_func = Zstd_close;
	CFH->eof_func = Zstd_eof;
	CFH->get_error_func = Zstd_get_error;

	CFH->compression_spec = compression_spec;

	CFH->private_data = NULL;
}

#endif							/* USE_ZSTD */
