/*-------------------------------------------------------------------------
 *
 * compress_gzip.c
 *	 Routines for archivers to read or write a gzip compressed data stream.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_gzip.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include <unistd.h>

#include "compress_gzip.h"
#include "pg_backup_utils.h"

#ifdef HAVE_LIBZ
#include "zlib.h"

/*
 * 我们不使用 gzgetc() 宏，因为 zlib 的配置逻辑
 * 不足以保证该宏对结构体字段布局的理解
 * 与库本身一致；示例参见
 * https://gnats.netbsd.org/cgi-bin/query-pr-single.pl?number=59711
 * 因此，我们 #undef 该宏并回退到底层函数。
 */
#undef gzgetc

/*----------------------
 * Compressor API
 *----------------------
 */
typedef struct GzipCompressorState
{
	z_streamp	zp;

	void	   *outbuf;
	size_t		outsize;
} GzipCompressorState;

/* 支持 gzip 压缩数据 I/O 的私有例程 */
static void DeflateCompressorInit(CompressorState *cs);
static void DeflateCompressorEnd(ArchiveHandle *AH, CompressorState *cs);
static void DeflateCompressorCommon(ArchiveHandle *AH, CompressorState *cs,
									bool flush);
static void EndCompressorGzip(ArchiveHandle *AH, CompressorState *cs);
static void WriteDataToArchiveGzip(ArchiveHandle *AH, CompressorState *cs,
								   const void *data, size_t dLen);
static void ReadDataFromArchiveGzip(ArchiveHandle *AH, CompressorState *cs);

static void
DeflateCompressorInit(CompressorState *cs)
{
	GzipCompressorState *gzipcs;
	z_streamp	zp;

	gzipcs = (GzipCompressorState *) pg_malloc0(sizeof(GzipCompressorState));
	zp = gzipcs->zp = (z_streamp) pg_malloc(sizeof(z_stream));
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	/*
	 * outsize 是告知 zlib 可输出的缓冲区大小。
	 * 我们实际多分配一个字节，因为某些例程需要在
	 * zlib 输出的末尾追加一个尾随零字节。
	 */
	gzipcs->outsize = DEFAULT_IO_BUFFER_SIZE;
	gzipcs->outbuf = pg_malloc(gzipcs->outsize + 1);

	/* -Z 0 使用 "None" 压缩器 -- 而非无压缩级别的 zlib */
	Assert(cs->compression_spec.level != 0);

	if (deflateInit(zp, cs->compression_spec.level) != Z_OK)
		pg_fatal("could not initialize compression library: %s", zp->msg);

	/* 谨慎起见 —— 可能在 Start 之后直接调用 End，中间没有 Write */
	zp->next_out = gzipcs->outbuf;
	zp->avail_out = gzipcs->outsize;

	/* 记录 gzipcs */
	cs->private_data = gzipcs;
}

static void
DeflateCompressorEnd(ArchiveHandle *AH, CompressorState *cs)
{
	GzipCompressorState *gzipcs = (GzipCompressorState *) cs->private_data;
	z_streamp	zp;

	zp = gzipcs->zp;
	zp->next_in = NULL;
	zp->avail_in = 0;

	/* 刷新 zlib 缓冲区中的剩余数据 */
	DeflateCompressorCommon(AH, cs, true);

	if (deflateEnd(zp) != Z_OK)
		pg_fatal("could not close compression stream: %s", zp->msg);

	pg_free(gzipcs->outbuf);
	pg_free(gzipcs->zp);
	pg_free(gzipcs);
	cs->private_data = NULL;
}

static void
DeflateCompressorCommon(ArchiveHandle *AH, CompressorState *cs, bool flush)
{
	GzipCompressorState *gzipcs = (GzipCompressorState *) cs->private_data;
	z_streamp	zp = gzipcs->zp;
	void	   *out = gzipcs->outbuf;
	int			res = Z_OK;

	while (gzipcs->zp->avail_in != 0 || flush)
	{
		res = deflate(zp, flush ? Z_FINISH : Z_NO_FLUSH);
		if (res == Z_STREAM_ERROR)
			pg_fatal("could not compress data: %s", zp->msg);
		if ((flush && (zp->avail_out < gzipcs->outsize))
			|| (zp->avail_out == 0)
			|| (zp->avail_in != 0)
			)
		{
			/*
			 * 额外谨慎：避免零长度块，因为零长度块在自定义格式中是 EOF
			 * 标记。这不应发生，
			 * 但以防万一...
			 */
			if (zp->avail_out < gzipcs->outsize)
			{
				/*
				 * 任何写入函数都应自行进行错误检查，但为确保安全，
				 * 我们在此也进行检查...
				 */
				size_t		len = gzipcs->outsize - zp->avail_out;

				cs->writeF(AH, (char *) out, len);
			}
			zp->next_out = out;
			zp->avail_out = gzipcs->outsize;
		}

		if (res == Z_STREAM_END)
			break;
	}
}

static void
EndCompressorGzip(ArchiveHandle *AH, CompressorState *cs)
{
	/* 如果 deflation 已初始化，则完成它 */
	if (cs->private_data)
		DeflateCompressorEnd(AH, cs);
}

static void
WriteDataToArchiveGzip(ArchiveHandle *AH, CompressorState *cs,
					   const void *data, size_t dLen)
{
	GzipCompressorState *gzipcs = (GzipCompressorState *) cs->private_data;

	gzipcs->zp->next_in = (void *) unconstify(void *, data);
	gzipcs->zp->avail_in = dLen;
	DeflateCompressorCommon(AH, cs, false);
}

static void
ReadDataFromArchiveGzip(ArchiveHandle *AH, CompressorState *cs)
{
	z_streamp	zp;
	char	   *out;
	int			res = Z_OK;
	size_t		cnt;
	char	   *buf;
	size_t		buflen;

	zp = (z_streamp) pg_malloc(sizeof(z_stream));
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	buflen = DEFAULT_IO_BUFFER_SIZE;
	buf = pg_malloc(buflen);

	out = pg_malloc(DEFAULT_IO_BUFFER_SIZE + 1);

	if (inflateInit(zp) != Z_OK)
		pg_fatal("could not initialize compression library: %s",
				 zp->msg);

	/* zlib 没有最小块大小要求 */
	while ((cnt = cs->readF(AH, &buf, &buflen)))
	{
		zp->next_in = (void *) buf;
		zp->avail_in = cnt;

		while (zp->avail_in > 0)
		{
			zp->next_out = (void *) out;
			zp->avail_out = DEFAULT_IO_BUFFER_SIZE;

			res = inflate(zp, 0);
			if (res != Z_OK && res != Z_STREAM_END)
				pg_fatal("could not uncompress data: %s", zp->msg);

			out[DEFAULT_IO_BUFFER_SIZE - zp->avail_out] = '\0';
			ahwrite(out, 1, DEFAULT_IO_BUFFER_SIZE - zp->avail_out, AH);
		}
	}

	zp->next_in = NULL;
	zp->avail_in = 0;
	while (res != Z_STREAM_END)
	{
		zp->next_out = (void *) out;
		zp->avail_out = DEFAULT_IO_BUFFER_SIZE;
		res = inflate(zp, 0);
		if (res != Z_OK && res != Z_STREAM_END)
			pg_fatal("could not uncompress data: %s", zp->msg);

		out[DEFAULT_IO_BUFFER_SIZE - zp->avail_out] = '\0';
		ahwrite(out, 1, DEFAULT_IO_BUFFER_SIZE - zp->avail_out, AH);
	}

	if (inflateEnd(zp) != Z_OK)
		pg_fatal("could not close compression library: %s", zp->msg);

	free(buf);
	free(out);
	free(zp);
}

/* 支持 gzip 压缩数据 I/O 的公共例程 */
void
InitCompressorGzip(CompressorState *cs,
				   const pg_compress_specification compression_spec)
{
	cs->readData = ReadDataFromArchiveGzip;
	cs->writeData = WriteDataToArchiveGzip;
	cs->end = EndCompressorGzip;

	cs->compression_spec = compression_spec;

	/*
	 * 如果调用者定义了写入函数，则准备必要的状态。
	 * 注意：如果数据为空，End 可能直接在 Init 之后立即调用，
	 * 而无需调用 Write。
	 */
	if (cs->writeF)
		DeflateCompressorInit(cs);
}


/*----------------------
 * Compress File API
 *----------------------
 */

static size_t
Gzip_read(void *ptr, size_t size, CompressFileHandle *CFH)
{
	gzFile		gzfp = (gzFile) CFH->private_data;
	int			gzret;

	/* 读取零字节必须为空操作 */
	if (size == 0)
		return 0;

	gzret = gzread(gzfp, ptr, size);

	/*
	 * gzread 在 EOF 和某些错误条件下返回零，在其他错误条件下返回
	 * 小于零的值，因此返回零时需要检查是否为
	 * EOF。
	 */
	if (gzret <= 0)
	{
		int			errnum;
		const char *errmsg;

		if (gzret == 0 && gzeof(gzfp))
			return 0;

		errmsg = gzerror(gzfp, &errnum);

		pg_fatal("could not read from input file: %s",
				 errnum == Z_ERRNO ? strerror(errno) : errmsg);
	}

	return (size_t) gzret;
}

static void
Gzip_write(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	gzFile		gzfp = (gzFile) CFH->private_data;
	int			errnum;
	const char *errmsg;

	if (gzwrite(gzfp, ptr, size) != size)
	{
		errmsg = gzerror(gzfp, &errnum);
		pg_fatal("could not write to file: %s",
				 errnum == Z_ERRNO ? strerror(errno) : errmsg);
	}
}

static int
Gzip_getc(CompressFileHandle *CFH)
{
	gzFile		gzfp = (gzFile) CFH->private_data;
	int			ret;

	errno = 0;
	ret = gzgetc(gzfp);
	if (ret == EOF)
	{
		if (!gzeof(gzfp))
			pg_fatal("could not read from input file: %s", strerror(errno));
		else
			pg_fatal("could not read from input file: end of file");
	}

	return ret;
}

static char *
Gzip_gets(char *ptr, int size, CompressFileHandle *CFH)
{
	gzFile		gzfp = (gzFile) CFH->private_data;

	return gzgets(gzfp, ptr, size);
}

static bool
Gzip_close(CompressFileHandle *CFH)
{
	gzFile		gzfp = (gzFile) CFH->private_data;

	CFH->private_data = NULL;

	return gzclose(gzfp) == Z_OK;
}

static bool
Gzip_eof(CompressFileHandle *CFH)
{
	gzFile		gzfp = (gzFile) CFH->private_data;

	return gzeof(gzfp) == 1;
}

static const char *
Gzip_get_error(CompressFileHandle *CFH)
{
	gzFile		gzfp = (gzFile) CFH->private_data;
	const char *errmsg;
	int			errnum;

	errmsg = gzerror(gzfp, &errnum);
	if (errnum == Z_ERRNO)
		errmsg = strerror(errno);

	return errmsg;
}

static bool
Gzip_open(const char *path, int fd, const char *mode, CompressFileHandle *CFH)
{
	gzFile		gzfp;
	char		mode_compression[32];

	if (CFH->compression_spec.level != Z_DEFAULT_COMPRESSION)
	{
		/*
		 * 用户指定了压缩级别，因此告知 zlib 使用它
		 */
		snprintf(mode_compression, sizeof(mode_compression), "%s%d",
				 mode, CFH->compression_spec.level);
	}
	else
		strcpy(mode_compression, mode);

	if (fd >= 0)
		gzfp = gzdopen(dup(fd), mode_compression);
	else
		gzfp = gzopen(path, mode_compression);

	if (gzfp == NULL)
		return false;

	CFH->private_data = gzfp;

	return true;
}

static bool
Gzip_open_write(const char *path, const char *mode, CompressFileHandle *CFH)
{
	char	   *fname;
	bool		ret;
	int			save_errno;

	fname = psprintf("%s.gz", path);
	ret = CFH->open_func(fname, -1, mode, CFH);

	save_errno = errno;
	pg_free(fname);
	errno = save_errno;

	return ret;
}

void
InitCompressFileHandleGzip(CompressFileHandle *CFH,
						   const pg_compress_specification compression_spec)
{
	CFH->open_func = Gzip_open;
	CFH->open_write_func = Gzip_open_write;
	CFH->read_func = Gzip_read;
	CFH->write_func = Gzip_write;
	CFH->gets_func = Gzip_gets;
	CFH->getc_func = Gzip_getc;
	CFH->close_func = Gzip_close;
	CFH->eof_func = Gzip_eof;
	CFH->get_error_func = Gzip_get_error;

	CFH->compression_spec = compression_spec;

	CFH->private_data = NULL;
}
#else							/* HAVE_LIBZ */
void
InitCompressorGzip(CompressorState *cs,
				   const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "gzip");
}

void
InitCompressFileHandleGzip(CompressFileHandle *CFH,
						   const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "gzip");
}
#endif							/* HAVE_LIBZ */
