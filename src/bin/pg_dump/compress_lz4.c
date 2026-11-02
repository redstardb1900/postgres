/*-------------------------------------------------------------------------
 *
 * compress_lz4.c
 *	 归档器用于写入 LZ4 压缩数据流的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include <unistd.h>

#include "pg_backup_utils.h"

#include "compress_lz4.h"

#ifdef USE_LZ4
#include <lz4frame.h>

/*
 * LZ4F_HEADER_SIZE_MAX 首次出现在库的 v1.7.5 版本中。
 * 为安装较低版本的用户重新定义它。
 */
#ifndef LZ4F_HEADER_SIZE_MAX
#define LZ4F_HEADER_SIZE_MAX	32
#endif

/*---------------------------------
 * 两种压缩 API 共用
 *---------------------------------
 */

/*
 * 供 Compressor 和 Stream API 两者使用的（解）压缩状态。
 */
typedef struct LZ4State
{
	/*
	 * 由 Stream API 用于跟踪文件流。
	 */
	FILE	   *fp;

	LZ4F_preferences_t prefs;

	LZ4F_compressionContext_t ctx;
	LZ4F_decompressionContext_t dtx;

	/*
	 * 由 Stream API 的延迟初始化使用。
	 */
	bool		inited;

	/*
	 * 由 Stream API 用于区分压缩和
	 * 解压操作。
	 */
	bool		compressing;

	/*
	 * 由 Compressor API 用于标记压缩头部是否
	 * 在初始化后已写入。
	 */
	bool		needs_header_flush;

	size_t		buflen;
	char	   *buffer;

	/*
	 * 由 Stream API 用于存储调用者尚未
	 * 消费的已解压数据。
	 */
	size_t		overflowalloclen;
	size_t		overflowlen;
	char	   *overflowbuf;

	/*
	 * 由两种 API 用于跟踪缓冲区中存储的
	 * 压缩数据长度。
	 */
	size_t		compressedlen;

	/*
	 * 由两种 API 用于跟踪错误码。
	 */
	size_t		errcode;
} LZ4State;

/*
 * LZ4State_compression_init
 *		初始化压缩所需的 LZ4State 成员。
 *
 * 将 LZ4 帧头部写入缓冲区并跟踪其长度。此函数的使用者
 * 可以选择何时以及如何将头部写入文件流。
 *
 * 成功时返回 true。失败时返回 false，并将
 * 错误码存储在 state->errcode 中。
 */
static bool
LZ4State_compression_init(LZ4State *state)
{
	size_t		status;

	state->buflen = LZ4F_compressBound(DEFAULT_IO_BUFFER_SIZE, &state->prefs);

	/*
	 * LZ4F_compressBegin 需要一个大于或等于
	 * LZ4F_HEADER_SIZE_MAX 的缓冲区。验证是否满足该要求。
	 */
	if (state->buflen < LZ4F_HEADER_SIZE_MAX)
		state->buflen = LZ4F_HEADER_SIZE_MAX;

	status = LZ4F_createCompressionContext(&state->ctx, LZ4F_VERSION);
	if (LZ4F_isError(status))
	{
		state->errcode = status;
		return false;
	}

	state->buffer = pg_malloc(state->buflen);
	status = LZ4F_compressBegin(state->ctx,
								state->buffer, state->buflen,
								&state->prefs);
	if (LZ4F_isError(status))
	{
		state->errcode = status;
		return false;
	}

	state->compressedlen = status;

	return true;
}

/*----------------------
 * Compressor API
 *----------------------
 */

/* 支持 LZ4 压缩数据 I/O 的私有例程 */

static void
ReadDataFromArchiveLZ4(ArchiveHandle *AH, CompressorState *cs)
{
	size_t		r;
	size_t		readbuflen;
	char	   *outbuf;
	char	   *readbuf;
	LZ4F_decompressionContext_t ctx = NULL;
	LZ4F_decompressOptions_t dec_opt;
	LZ4F_errorCode_t status;

	memset(&dec_opt, 0, sizeof(dec_opt));
	status = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
	if (LZ4F_isError(status))
		pg_fatal("could not create LZ4 decompression context: %s",
				 LZ4F_getErrorName(status));

	outbuf = pg_malloc0(DEFAULT_IO_BUFFER_SIZE);
	readbuf = pg_malloc0(DEFAULT_IO_BUFFER_SIZE);
	readbuflen = DEFAULT_IO_BUFFER_SIZE;
	while ((r = cs->readF(AH, &readbuf, &readbuflen)) > 0)
	{
		char	   *readp;
		char	   *readend;

		/* 处理一个块 */
		readp = readbuf;
		readend = readbuf + r;
		while (readp < readend)
		{
			size_t		out_size = DEFAULT_IO_BUFFER_SIZE;
			size_t		read_size = readend - readp;

			memset(outbuf, 0, DEFAULT_IO_BUFFER_SIZE);
			status = LZ4F_decompress(ctx, outbuf, &out_size,
									 readp, &read_size, &dec_opt);
			if (LZ4F_isError(status))
				pg_fatal("could not decompress: %s",
						 LZ4F_getErrorName(status));

			ahwrite(outbuf, 1, out_size, AH);
			readp += read_size;
		}
	}

	pg_free(outbuf);
	pg_free(readbuf);

	status = LZ4F_freeDecompressionContext(ctx);
	if (LZ4F_isError(status))
		pg_fatal("could not free LZ4 decompression context: %s",
				 LZ4F_getErrorName(status));
}

static void
WriteDataToArchiveLZ4(ArchiveHandle *AH, CompressorState *cs,
					  const void *data, size_t dLen)
{
	LZ4State   *state = (LZ4State *) cs->private_data;
	size_t		remaining = dLen;
	size_t		status;
	size_t		chunk;

	/* 如果尚未写入，则写入头部。 */
	if (state->needs_header_flush)
	{
		cs->writeF(AH, state->buffer, state->compressedlen);
		state->needs_header_flush = false;
	}

	while (remaining > 0)
	{

		if (remaining > DEFAULT_IO_BUFFER_SIZE)
			chunk = DEFAULT_IO_BUFFER_SIZE;
		else
			chunk = remaining;

		remaining -= chunk;
		status = LZ4F_compressUpdate(state->ctx,
									 state->buffer, state->buflen,
									 data, chunk, NULL);

		if (LZ4F_isError(status))
			pg_fatal("could not compress data: %s",
					 LZ4F_getErrorName(status));

		cs->writeF(AH, state->buffer, status);

		data = ((char *) data) + chunk;
	}
}

static void
EndCompressorLZ4(ArchiveHandle *AH, CompressorState *cs)
{
	LZ4State   *state = (LZ4State *) cs->private_data;
	size_t		status;

	/* 无事可做 */
	if (!state)
		return;

	/*
	 * 如果尚未写入，则写入头部。调用者不需要在
	 * 关系中不包含任何数据时调用 writeData。因此可能
	 * 在尚未刷新头部的情况下到达此处。在
	 * 结束压缩之前执行此操作。
	 */
	if (state->needs_header_flush)
		cs->writeF(AH, state->buffer, state->compressedlen);

	status = LZ4F_compressEnd(state->ctx,
							  state->buffer, state->buflen,
							  NULL);
	if (LZ4F_isError(status))
		pg_fatal("could not end compression: %s",
				 LZ4F_getErrorName(status));

	cs->writeF(AH, state->buffer, status);

	status = LZ4F_freeCompressionContext(state->ctx);
	if (LZ4F_isError(status))
		pg_fatal("could not end compression: %s",
				 LZ4F_getErrorName(status));

	pg_free(state->buffer);
	pg_free(state);

	cs->private_data = NULL;
}

/*
 * 支持 LZ4 压缩数据 I/O 的公共例程
 */
void
InitCompressorLZ4(CompressorState *cs, const pg_compress_specification compression_spec)
{
	LZ4State   *state;

	cs->readData = ReadDataFromArchiveLZ4;
	cs->writeData = WriteDataToArchiveLZ4;
	cs->end = EndCompressorLZ4;

	cs->compression_spec = compression_spec;

	/*
	 * 读取操作可以访问整个输入。不需要在
	 * 调用之间携带状态。
	 */
	if (cs->readF)
		return;

	state = pg_malloc0(sizeof(*state));
	if (cs->compression_spec.level >= 0)
		state->prefs.compressionLevel = cs->compression_spec.level;

	if (!LZ4State_compression_init(state))
		pg_fatal("could not initialize LZ4 compression: %s",
				 LZ4F_getErrorName(state->errcode));

	/* 记住头部尚未写入。 */
	state->needs_header_flush = true;
	cs->private_data = state;
}

/*----------------------
 * Compress Stream API
 *----------------------
 */


/*
 * feof() 或 gzeof() 的 LZ4 等效实现。当溢出缓冲区
 * 中没有解压输出且后备文件的末尾
 * 已到达时返回 true。
 */
static bool
LZ4Stream_eof(CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;

	return state->overflowlen == 0 && feof(state->fp);
}

static const char *
LZ4Stream_get_error(CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	const char *errmsg;

	if (LZ4F_isError(state->errcode))
		errmsg = LZ4F_getErrorName(state->errcode);
	else
		errmsg = strerror(errno);

	return errmsg;
}

/*
 * 为后续调用初始化已分配的 LZ4State 结构。
 *
 * 创建压缩或解压所需的上下文。压缩数据时
 * （由 compressing=true 指示），它额外将
 * LZ4 头部写入输出流。
 *
 * 成功时返回 true。失败时返回 false，并将
 * 错误码存储在 state->errcode 中。
 */
static bool
LZ4Stream_init(LZ4State *state, int size, bool compressing)
{
	size_t		status;

	if (state->inited)
		return true;

	state->compressing = compressing;

	/* 压缩时，将 LZ4 头部写入输出流。 */
	if (state->compressing)
	{

		if (!LZ4State_compression_init(state))
			return false;

		errno = 0;
		if (fwrite(state->buffer, 1, state->compressedlen, state->fp) != state->compressedlen)
		{
			errno = (errno) ? errno : ENOSPC;
			return false;
		}
	}
	else
	{
		status = LZ4F_createDecompressionContext(&state->dtx, LZ4F_VERSION);
		if (LZ4F_isError(status))
		{
			state->errcode = status;
			return false;
		}

		state->buflen = Max(size, DEFAULT_IO_BUFFER_SIZE);
		state->buffer = pg_malloc(state->buflen);

		state->overflowalloclen = state->buflen;
		state->overflowbuf = pg_malloc(state->overflowalloclen);
		state->overflowlen = 0;
	}

	state->inited = true;
	return true;
}

/*
 * 将溢出缓冲区中已解压的内容读取到 'ptr' 中，最多
 * 'size' 字节（如果可用）。如果设置了 eol_flag，
 * 则在达到 'size' 字节之前遇到换行符时停止。
 *
 * 溢出缓冲区中任何未读的内容将移到开头。
 *
 * 返回从溢出缓冲区读取（并复制到 'ptr' 缓冲区）的
 * 字节数，如果溢出缓冲区为空则返回 0。
 */
static int
LZ4Stream_read_overflow(LZ4State *state, void *ptr, int size, bool eol_flag)
{
	char	   *p;
	int			readlen = 0;

	if (state->overflowlen == 0)
		return 0;

	if (state->overflowlen >= size)
		readlen = size;
	else
		readlen = state->overflowlen;

	if (eol_flag && (p = memchr(state->overflowbuf, '\n', readlen)))
		/* 包含行终止字符 */
		readlen = p - state->overflowbuf + 1;

	memcpy(ptr, state->overflowbuf, readlen);
	state->overflowlen -= readlen;

	if (state->overflowlen > 0)
		memmove(state->overflowbuf, state->overflowbuf + readlen, state->overflowlen);

	return readlen;
}

/*
 * 从 LZ4 压缩流中读取解压内容的主工作函数。
 *
 * 它将读取最多 'ptrsize' 的解压内容，如果设置了 eol_flag
 * 且先找到换行符则提前停止。通过 LZ4F API 读取
 * 压缩输入生成的解压输出可能超过 'ptrsize'。
 * 任何超出的解压内容存储在 LZ4State 的溢出缓冲区中。
 * 当然，调用此函数时，它首先尝试消费溢出缓冲区中
 * 已存在的解压内容，然后再解压新内容。
 *
 *
 *
 * 返回复制到 ptr 缓冲区中的解压数据字节数，
 * 出错时返回 -1。
 */
static int
LZ4Stream_read_internal(LZ4State *state, void *ptr, int ptrsize, bool eol_flag)
{
	int			dsize = 0;
	int			rsize;
	int			size = ptrsize;
	bool		eol_found = false;

	void	   *readbuf;

	/* 延迟初始化 */
	if (!LZ4Stream_init(state, size, false /* 解压 */ ))
	{
		pg_log_error("unable to initialize LZ4 library: %s",
					 LZ4F_getErrorName(state->errcode));
		return -1;
	}

	/* 对于零大小的输出缓冲区无需做任何工作 */
	if (size <= 0)
		return 0;

	/* 验证输出缓冲区是否有足够的空间 */
	if (size > state->buflen)
	{
		state->buflen = size;
		state->buffer = pg_realloc(state->buffer, size);
	}

	/* 如果有已解压的内容则使用它 */
	dsize = LZ4Stream_read_overflow(state, ptr, size, eol_flag);
	if (dsize == size || (eol_flag && memchr(ptr, '\n', dsize)))
		return dsize;

	readbuf = pg_malloc(size);

	do
	{
		char	   *rp;
		char	   *rend;

		rsize = fread(readbuf, 1, size, state->fp);
		if (rsize < size && !feof(state->fp))
		{
			pg_log_error("could not read from input file: %m");
			return -1;
		}

		rp = (char *) readbuf;
		rend = (char *) readbuf + rsize;

		while (rp < rend)
		{
			size_t		status;
			size_t		outlen = state->buflen;
			size_t		read_remain = rend - rp;

			memset(state->buffer, 0, outlen);
			status = LZ4F_decompress(state->dtx, state->buffer, &outlen,
									 rp, &read_remain, NULL);
			if (LZ4F_isError(status))
			{
				state->errcode = status;
				pg_log_error("could not read from input file: %s",
							 LZ4F_getErrorName(state->errcode));
				return -1;
			}

			rp += read_remain;

			/*
			 * 如果设置了 eol 标志，填充 ptr 中可用的空间，
			 * 如果已找到 eol 则跳过，如果输出缓冲区中存在
			 * eol 则填充到该位置
			 */
			if (outlen > 0 && dsize < size && eol_found == false)
			{
				char	   *p;
				size_t		lib = (!eol_flag) ? size - dsize : size - 1 - dsize;
				size_t		len = outlen < lib ? outlen : lib;

				if (eol_flag &&
					(p = memchr(state->buffer, '\n', outlen)) &&
					(size_t) (p - state->buffer + 1) <= len)
				{
					len = p - state->buffer + 1;
					eol_found = true;
				}

				memcpy((char *) ptr + dsize, state->buffer, len);
				dsize += len;

				/* 将未容纳的部分（如果有）移到缓冲区开头 */
				if (len < outlen)
					memmove(state->buffer, state->buffer + len, outlen - len);
				outlen -= len;
			}

			/* 如果有可用的输出，则保存它 */
			if (outlen > 0)
			{
				while (state->overflowlen + outlen > state->overflowalloclen)
				{
					state->overflowalloclen *= 2;
					state->overflowbuf = pg_realloc(state->overflowbuf,
													state->overflowalloclen);
				}

				memcpy(state->overflowbuf + state->overflowlen, state->buffer, outlen);
				state->overflowlen += outlen;
			}
		}
	} while (rsize == size && dsize < size && eol_found == false);

	pg_free(readbuf);

	return dsize;
}

/*
 * 将 ptr 中的 size 字节压缩并写入流。
 */
static void
LZ4Stream_write(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	size_t		status;
	int			remaining = size;

	/* 延迟初始化 */
	if (!LZ4Stream_init(state, size, true))
		pg_fatal("unable to initialize LZ4 library: %s",
				 LZ4F_getErrorName(state->errcode));

	while (remaining > 0)
	{
		int			chunk = Min(remaining, DEFAULT_IO_BUFFER_SIZE);

		remaining -= chunk;

		status = LZ4F_compressUpdate(state->ctx, state->buffer, state->buflen,
									 ptr, chunk, NULL);
		if (LZ4F_isError(status))
			pg_fatal("error during writing: %s", LZ4F_getErrorName(status));

		errno = 0;
		if (fwrite(state->buffer, 1, status, state->fp) != status)
		{
			errno = (errno) ? errno : ENOSPC;
			pg_fatal("error during writing: %m");
		}

		ptr = ((const char *) ptr) + chunk;
	}
}

/*
 * LZ4 压缩文件的 fread() 等效实现。
 */
static size_t
LZ4Stream_read(void *ptr, size_t size, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	int			ret;

	if ((ret = LZ4Stream_read_internal(state, ptr, size, false)) < 0)
		pg_fatal("could not read from input file: %s", LZ4Stream_get_error(CFH));

	return (size_t) ret;
}

/*
 * LZ4 压缩文件的 fgetc() 等效实现。
 */
static int
LZ4Stream_getc(CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	unsigned char c;

	if (LZ4Stream_read_internal(state, &c, 1, false) <= 0)
	{
		if (!LZ4Stream_eof(CFH))
			pg_fatal("could not read from input file: %s", LZ4Stream_get_error(CFH));
		else
			pg_fatal("could not read from input file: end of file");
	}

	return c;
}

/*
 * LZ4 压缩文件的 fgets() 等效实现。
 */
static char *
LZ4Stream_gets(char *ptr, int size, CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;
	int			ret;

	ret = LZ4Stream_read_internal(state, ptr, size - 1, true);

	/*
	 * LZ4Stream_read_internal 返回 0 或 -1 意味着
	 * 发生了 EOF 或错误，但 gets_func 定义为在
	 * 两种情况下都返回 NULL，因此我们在这里可以一视同仁。
	 */
	if (ret <= 0)
		return NULL;

	/*
	 * 我们的调用者期望返回的字符串以 NULL 结尾，并且我们知道
	 * ret 大于零。
	 */
	ptr[ret - 1] = '\0';

	return ptr;
}

/*
 * 完成流的（解）压缩。压缩时将写入 LZ4 API
 * 生成的任何剩余内容和/或尾部。
 */
static bool
LZ4Stream_close(CompressFileHandle *CFH)
{
	FILE	   *fp;
	LZ4State   *state = (LZ4State *) CFH->private_data;
	size_t		status;
	int			ret;

	fp = state->fp;
	if (state->inited)
	{
		if (state->compressing)
		{
			status = LZ4F_compressEnd(state->ctx, state->buffer, state->buflen, NULL);
			if (LZ4F_isError(status))
			{
				pg_log_error("could not end compression: %s",
							 LZ4F_getErrorName(status));
			}
			else
			{
				errno = 0;
				if (fwrite(state->buffer, 1, status, state->fp) != status)
				{
					errno = (errno) ? errno : ENOSPC;
					pg_log_error("could not write to output file: %m");
				}
			}

			status = LZ4F_freeCompressionContext(state->ctx);
			if (LZ4F_isError(status))
				pg_log_error("could not end compression: %s",
							 LZ4F_getErrorName(status));
		}
		else
		{
			status = LZ4F_freeDecompressionContext(state->dtx);
			if (LZ4F_isError(status))
				pg_log_error("could not end decompression: %s",
							 LZ4F_getErrorName(status));
			pg_free(state->overflowbuf);
		}

		pg_free(state->buffer);
	}

	pg_free(state);
	CFH->private_data = NULL;

	errno = 0;
	ret = fclose(fp);
	if (ret != 0)
	{
		pg_log_error("could not close file: %m");
		return false;
	}

	return true;
}

static bool
LZ4Stream_open(const char *path, int fd, const char *mode,
			   CompressFileHandle *CFH)
{
	LZ4State   *state = (LZ4State *) CFH->private_data;

	if (fd >= 0)
		state->fp = fdopen(dup(fd), mode);
	else
		state->fp = fopen(path, mode);
	if (state->fp == NULL)
	{
		state->errcode = errno;
		return false;
	}

	return true;
}

static bool
LZ4Stream_open_write(const char *path, const char *mode, CompressFileHandle *CFH)
{
	char	   *fname;
	int			save_errno;
	bool		ret;

	fname = psprintf("%s.lz4", path);
	ret = CFH->open_func(fname, -1, mode, CFH);

	save_errno = errno;
	pg_free(fname);
	errno = save_errno;

	return ret;
}

/*
 * 公共例程
 */
void
InitCompressFileHandleLZ4(CompressFileHandle *CFH,
						  const pg_compress_specification compression_spec)
{
	LZ4State   *state;

	CFH->open_func = LZ4Stream_open;
	CFH->open_write_func = LZ4Stream_open_write;
	CFH->read_func = LZ4Stream_read;
	CFH->write_func = LZ4Stream_write;
	CFH->gets_func = LZ4Stream_gets;
	CFH->getc_func = LZ4Stream_getc;
	CFH->eof_func = LZ4Stream_eof;
	CFH->close_func = LZ4Stream_close;
	CFH->get_error_func = LZ4Stream_get_error;

	CFH->compression_spec = compression_spec;
	state = pg_malloc0(sizeof(*state));
	if (CFH->compression_spec.level >= 0)
		state->prefs.compressionLevel = CFH->compression_spec.level;

	CFH->private_data = state;
}
#else							/* USE_LZ4 */
void
InitCompressorLZ4(CompressorState *cs,
				  const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "LZ4");
}

void
InitCompressFileHandleLZ4(CompressFileHandle *CFH,
						  const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "LZ4");
}
#endif							/* USE_LZ4 */
