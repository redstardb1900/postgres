/*-------------------------------------------------------------------------
 *
 * walmethods.c - implementations of different ways to write received wal
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/walmethods.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_LZ4
#include <lz4frame.h>
#endif
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "pgtar.h"
#include "receivelog.h"
#include "streamutil.h"

/* .tar.gz zlib 缓冲区大小    */
#define ZLIB_OUT_SIZE 4096

/* .lz4 的 LZ4 输入块大小     */
#define LZ4_IN_SIZE  4096

/*-------------------------------------------------------------------------
 * WalDirectoryMethod - 将 WAL 写入看起来像 pg_wal 的目录
 *-------------------------------------------------------------------------
  */

static Walfile *dir_open_for_write(WalWriteMethod *wwmethod,
								   const char *pathname,
								   const char *temp_suffix,
								   size_t pad_to_size);
static int	dir_close(Walfile *f, WalCloseMethod method);
static bool dir_existsfile(WalWriteMethod *wwmethod, const char *pathname);
static ssize_t dir_get_file_size(WalWriteMethod *wwmethod,
								 const char *pathname);
static char *dir_get_file_name(WalWriteMethod *wwmethod,
							   const char *pathname, const char *temp_suffix);
static ssize_t dir_write(Walfile *f, const void *buf, size_t count);
static int	dir_sync(Walfile *f);
static bool dir_finish(WalWriteMethod *wwmethod);
static void dir_free(WalWriteMethod *wwmethod);

const WalWriteMethodOps WalDirectoryMethodOps = {
	.open_for_write = dir_open_for_write,
	.close = dir_close,
	.existsfile = dir_existsfile,
	.get_file_size = dir_get_file_size,
	.get_file_name = dir_get_file_name,
	.write = dir_write,
	.sync = dir_sync,
	.finish = dir_finish,
	.free = dir_free
};

/*
 * 此方法的全局静态数据
     */
typedef struct DirectoryMethodData
{
	WalWriteMethod base;
	char	   *basedir;
} DirectoryMethodData;

/*
 * 文件句柄     
 */
typedef struct DirectoryMethodFile
{
	Walfile		base;
	int			fd;
	char	   *fullpath;
	char	   *temp_suffix;
#ifdef HAVE_LIBZ
	gzFile		gzfp;
#endif
#ifdef USE_LZ4
	LZ4F_compressionContext_t ctx;
	size_t		lz4bufsize;
	void	   *lz4buf;
#endif
} DirectoryMethodFile;

#define clear_error(wwmethod) \
	((wwmethod)->lasterrstring = NULL, (wwmethod)->lasterrno = 0)

static char *
dir_get_file_name(WalWriteMethod *wwmethod,
				  const char *pathname, const char *temp_suffix)
{
	char	   *filename = pg_malloc0(MAXPGPATH * sizeof(char));

	snprintf(filename, MAXPGPATH, "%s%s%s",
			 pathname,
			 wwmethod->compression_algorithm == PG_COMPRESSION_GZIP ? ".gz" :
			 wwmethod->compression_algorithm == PG_COMPRESSION_LZ4 ? ".lz4" : "",
			 temp_suffix ? temp_suffix : "");

	return filename;
}

static Walfile *
dir_open_for_write(WalWriteMethod *wwmethod, const char *pathname,
				   const char *temp_suffix, size_t pad_to_size)
{
	DirectoryMethodData *dir_data = (DirectoryMethodData *) wwmethod;
	char		tmppath[MAXPGPATH];
	char	   *filename;
	int			fd;
	DirectoryMethodFile *f;
#ifdef HAVE_LIBZ
	gzFile		gzfp = NULL;
#endif
#ifdef USE_LZ4
	LZ4F_compressionContext_t ctx = NULL;
	size_t		lz4bufsize = 0;
	void	   *lz4buf = NULL;
#endif

	clear_error(wwmethod);

	filename = dir_get_file_name(wwmethod, pathname, temp_suffix);
	snprintf(tmppath, sizeof(tmppath), "%s/%s",
			 dir_data->basedir, filename);
	pg_free(filename);

	/*
	 * 为非压缩和压缩文件打开文件。跟踪文件描述符     
	 * 对于 dir_sync() 方法很重要，因为 gzflush() 不会        
	 * 执行任何 fsync() 系统调用使更改在磁盘上持久化。  
	 *      
	 */
	fd = open(tmppath, O_WRONLY | O_CREAT | PG_BINARY, pg_file_create_mode);
	if (fd < 0)
	{
		wwmethod->lasterrno = errno;
		return NULL;
	}

#ifdef HAVE_LIBZ
	if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		gzfp = gzdopen(fd, "wb");
		if (gzfp == NULL)
		{
			wwmethod->lasterrno = errno;
			close(fd);
			return NULL;
		}

		if (gzsetparams(gzfp, wwmethod->compression_level,
						Z_DEFAULT_STRATEGY) != Z_OK)
		{
			wwmethod->lasterrno = errno;
			gzclose(gzfp);
			return NULL;
		}
	}
#endif
#ifdef USE_LZ4
	if (wwmethod->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		size_t		ctx_out;
		size_t		header_size;
		LZ4F_preferences_t prefs;

		ctx_out = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
		if (LZ4F_isError(ctx_out))
		{
			wwmethod->lasterrstring = LZ4F_getErrorName(ctx_out);
			close(fd);
			return NULL;
		}

		lz4bufsize = LZ4F_compressBound(LZ4_IN_SIZE, NULL);
		lz4buf = pg_malloc0(lz4bufsize);

		/* 分配压缩级别，默认为 0           */
		memset(&prefs, 0, sizeof(prefs));
		prefs.compressionLevel = wwmethod->compression_level;

		/* 添加头部   */
		header_size = LZ4F_compressBegin(ctx, lz4buf, lz4bufsize, &prefs);
		if (LZ4F_isError(header_size))
		{
			wwmethod->lasterrstring = LZ4F_getErrorName(header_size);
			(void) LZ4F_freeCompressionContext(ctx);
			pg_free(lz4buf);
			close(fd);
			return NULL;
		}

		errno = 0;
		if (write(fd, lz4buf, header_size) != header_size)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			wwmethod->lasterrno = errno ? errno : ENOSPC;
			(void) LZ4F_freeCompressionContext(ctx);
			pg_free(lz4buf);
			close(fd);
			return NULL;
		}
	}
#endif

	/* 对非压缩文件进行预填充      */
	if (pad_to_size && wwmethod->compression_algorithm == PG_COMPRESSION_NONE)
	{
		ssize_t		rc;

		rc = pg_pwrite_zeros(fd, pad_to_size, 0);

		if (rc < 0)
		{
			wwmethod->lasterrno = errno;
			close(fd);
			return NULL;
		}

		/*
		 * pg_pwrite()（经 pg_pwrite_zeros() 调用）可能移动了文件
		 * 位置需重置（见 win32pwrite.c）。 
		 */
		if (lseek(fd, 0, SEEK_SET) != 0)
		{
			wwmethod->lasterrno = errno;
			close(fd);
			return NULL;
		}
	}

	/*
	 * fsync WAL 文件及所在目录，以确保文件             
	 * 被持久创建并清零（如果填充过）。这一点在    
	 * 使用同步模式时尤为重要，因为文件在原位被         
	 * 修改并就地 fsync，无目录 fsync。  
	 */
	if (wwmethod->sync)
	{
		if (fsync_fname(tmppath, false) != 0 ||
			fsync_parent_path(tmppath) != 0)
		{
			wwmethod->lasterrno = errno;
#ifdef HAVE_LIBZ
			if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
				gzclose(gzfp);
			else
#endif
#ifdef USE_LZ4
			if (wwmethod->compression_algorithm == PG_COMPRESSION_LZ4)
			{
				(void) LZ4F_compressEnd(ctx, lz4buf, lz4bufsize, NULL);
				(void) LZ4F_freeCompressionContext(ctx);
				pg_free(lz4buf);
				close(fd);
			}
			else
#endif
				close(fd);
			return NULL;
		}
	}

	f = pg_malloc0(sizeof(DirectoryMethodFile));
#ifdef HAVE_LIBZ
	if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
		f->gzfp = gzfp;
#endif
#ifdef USE_LZ4
	if (wwmethod->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		f->ctx = ctx;
		f->lz4buf = lz4buf;
		f->lz4bufsize = lz4bufsize;
	}
#endif

	f->base.wwmethod = wwmethod;
	f->base.currpos = 0;
	f->base.pathname = pg_strdup(pathname);
	f->fd = fd;
	f->fullpath = pg_strdup(tmppath);
	if (temp_suffix)
		f->temp_suffix = pg_strdup(temp_suffix);

	return &f->base;
}

static ssize_t
dir_write(Walfile *f, const void *buf, size_t count)
{
	ssize_t		r;
	DirectoryMethodFile *df = (DirectoryMethodFile *) f;

	Assert(f != NULL);
	clear_error(f->wwmethod);

#ifdef HAVE_LIBZ
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		errno = 0;
		r = (ssize_t) gzwrite(df->gzfp, buf, count);
		if (r != count)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			f->wwmethod->lasterrno = errno ? errno : ENOSPC;
		}
	}
	else
#endif
#ifdef USE_LZ4
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		size_t		chunk;
		size_t		remaining;
		const void *inbuf = buf;

		remaining = count;
		while (remaining > 0)
		{
			size_t		compressed;

			if (remaining > LZ4_IN_SIZE)
				chunk = LZ4_IN_SIZE;
			else
				chunk = remaining;

			remaining -= chunk;
			compressed = LZ4F_compressUpdate(df->ctx,
											 df->lz4buf, df->lz4bufsize,
											 inbuf, chunk,
											 NULL);

			if (LZ4F_isError(compressed))
			{
				f->wwmethod->lasterrstring = LZ4F_getErrorName(compressed);
				return -1;
			}

			errno = 0;
			if (write(df->fd, df->lz4buf, compressed) != compressed)
			{
				/* 若 write 未设置 errno，则假定为磁盘空间不足 */
				f->wwmethod->lasterrno = errno ? errno : ENOSPC;
				return -1;
			}

			inbuf = ((char *) inbuf) + chunk;
		}

		/* 我们的调用者跟踪未压缩的大小       */
		r = (ssize_t) count;
	}
	else
#endif
	{
		errno = 0;
		r = write(df->fd, buf, count);
		if (r != count)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			f->wwmethod->lasterrno = errno ? errno : ENOSPC;
		}
	}
	if (r > 0)
		df->base.currpos += r;
	return r;
}

static int
dir_close(Walfile *f, WalCloseMethod method)
{
	int			r;
	DirectoryMethodFile *df = (DirectoryMethodFile *) f;
	DirectoryMethodData *dir_data = (DirectoryMethodData *) f->wwmethod;
	char		tmppath[MAXPGPATH];
	char		tmppath2[MAXPGPATH];

	Assert(f != NULL);
	clear_error(f->wwmethod);

#ifdef HAVE_LIBZ
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		errno = 0;				/* 以防 gzclose() 不设置 errno */
		r = gzclose(df->gzfp);
	}
	else
#endif
#ifdef USE_LZ4
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		size_t		compressed;

		compressed = LZ4F_compressEnd(df->ctx,
									  df->lz4buf, df->lz4bufsize,
									  NULL);

		if (LZ4F_isError(compressed))
		{
			f->wwmethod->lasterrstring = LZ4F_getErrorName(compressed);
			return -1;
		}

		errno = 0;
		if (write(df->fd, df->lz4buf, compressed) != compressed)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			f->wwmethod->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}

		r = close(df->fd);
	}
	else
#endif
		r = close(df->fd);

	if (r == 0)
	{
		/* 构建到文件当前版本的路径          */
		if (method == CLOSE_NORMAL && df->temp_suffix)
		{
			char	   *filename;
			char	   *filename2;

			/*
			 * 若有临时前缀，正常操作是重命名该文件。  
			 *      
			 */
			filename = dir_get_file_name(f->wwmethod, df->base.pathname,
										 df->temp_suffix);
			snprintf(tmppath, sizeof(tmppath), "%s/%s",
					 dir_data->basedir, filename);
			pg_free(filename);

			/* 永久名称，因此不需要前缀      */
			filename2 = dir_get_file_name(f->wwmethod, df->base.pathname, NULL);
			snprintf(tmppath2, sizeof(tmppath2), "%s/%s",
					 dir_data->basedir, filename2);
			pg_free(filename2);
			if (f->wwmethod->sync)
				r = durable_rename(tmppath, tmppath2);
			else
			{
				if (rename(tmppath, tmppath2) != 0)
				{
					pg_log_error("could not rename file \"%s\" to \"%s\": %m",
								 tmppath, tmppath2);
					r = -1;
				}
			}
		}
		else if (method == CLOSE_UNLINK)
		{
			char	   *filename;

			/* 文件关闭后删除它         */
			filename = dir_get_file_name(f->wwmethod, df->base.pathname,
										 df->temp_suffix);
			snprintf(tmppath, sizeof(tmppath), "%s/%s",
					 dir_data->basedir, filename);
			pg_free(filename);
			r = unlink(tmppath);
		}
		else
		{
			/*
			 * 否则为 CLOSE_NORMAL 且无临时后缀，或
			 * 要么是 CLOSE_NO_RENAME。此时若请求同步模式，  
			 * 则 fsync 文件及所在目录。  
			 */
			if (f->wwmethod->sync)
			{
				r = fsync_fname(df->fullpath, false);
				if (r == 0)
					r = fsync_parent_path(df->fullpath);
			}
		}
	}

	if (r != 0)
		f->wwmethod->lasterrno = errno;

#ifdef USE_LZ4
	pg_free(df->lz4buf);
	/* 支持对 NULL 释放 */
	LZ4F_freeCompressionContext(df->ctx);
#endif

	pg_free(df->base.pathname);
	pg_free(df->fullpath);
	pg_free(df->temp_suffix);
	pg_free(df);

	return r;
}

static int
dir_sync(Walfile *f)
{
	int			r;

	Assert(f != NULL);
	clear_error(f->wwmethod);

	if (!f->wwmethod->sync)
		return 0;

#ifdef HAVE_LIBZ
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		if (gzflush(((DirectoryMethodFile *) f)->gzfp, Z_SYNC_FLUSH) != Z_OK)
		{
			f->wwmethod->lasterrno = errno;
			return -1;
		}
	}
#endif
#ifdef USE_LZ4
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_LZ4)
	{
		DirectoryMethodFile *df = (DirectoryMethodFile *) f;
		size_t		compressed;

		/* 刷新内部缓冲区      */
		compressed = LZ4F_flush(df->ctx, df->lz4buf, df->lz4bufsize, NULL);
		if (LZ4F_isError(compressed))
		{
			f->wwmethod->lasterrstring = LZ4F_getErrorName(compressed);
			return -1;
		}

		errno = 0;
		if (write(df->fd, df->lz4buf, compressed) != compressed)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			f->wwmethod->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}
	}
#endif

	r = fsync(((DirectoryMethodFile *) f)->fd);
	if (r < 0)
		f->wwmethod->lasterrno = errno;
	return r;
}

static ssize_t
dir_get_file_size(WalWriteMethod *wwmethod, const char *pathname)
{
	DirectoryMethodData *dir_data = (DirectoryMethodData *) wwmethod;
	struct stat statbuf;
	char		tmppath[MAXPGPATH];

	snprintf(tmppath, sizeof(tmppath), "%s/%s",
			 dir_data->basedir, pathname);

	if (stat(tmppath, &statbuf) != 0)
	{
		wwmethod->lasterrno = errno;
		return -1;
	}

	return statbuf.st_size;
}

static bool
dir_existsfile(WalWriteMethod *wwmethod, const char *pathname)
{
	DirectoryMethodData *dir_data = (DirectoryMethodData *) wwmethod;
	char		tmppath[MAXPGPATH];
	int			fd;

	clear_error(wwmethod);

	snprintf(tmppath, sizeof(tmppath), "%s/%s",
			 dir_data->basedir, pathname);

	fd = open(tmppath, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		return false;
	close(fd);
	return true;
}

static bool
dir_finish(WalWriteMethod *wwmethod)
{
	clear_error(wwmethod);

	if (wwmethod->sync)
	{
		DirectoryMethodData *dir_data = (DirectoryMethodData *) wwmethod;

		/*
		 * 文件在关闭时会被 fsync，但我们也需要在此       
		 * 处对目录项 fsync。     
		 */
		if (fsync_fname(dir_data->basedir, true) != 0)
		{
			wwmethod->lasterrno = errno;
			return false;
		}
	}
	return true;
}

static void
dir_free(WalWriteMethod *wwmethod)
{
	DirectoryMethodData *dir_data = (DirectoryMethodData *) wwmethod;

	pg_free(dir_data->basedir);
	pg_free(wwmethod);
}


WalWriteMethod *
CreateWalDirectoryMethod(const char *basedir,
						 pg_compress_algorithm compression_algorithm,
						 int compression_level, bool sync)
{
	DirectoryMethodData *wwmethod;

	wwmethod = pg_malloc0(sizeof(DirectoryMethodData));
	*((const WalWriteMethodOps **) &wwmethod->base.ops) =
		&WalDirectoryMethodOps;
	wwmethod->base.compression_algorithm = compression_algorithm;
	wwmethod->base.compression_level = compression_level;
	wwmethod->base.sync = sync;
	clear_error(&wwmethod->base);
	wwmethod->basedir = pg_strdup(basedir);

	return &wwmethod->base;
}


/*-------------------------------------------------------------------------
 * WalTarMethod - 将 WAL 写入包含 pg_wal 内容的 tar 文件
 *-------------------------------------------------------------------------
   */

static Walfile *tar_open_for_write(WalWriteMethod *wwmethod,
								   const char *pathname,
								   const char *temp_suffix,
								   size_t pad_to_size);
static int	tar_close(Walfile *f, WalCloseMethod method);
static bool tar_existsfile(WalWriteMethod *wwmethod, const char *pathname);
static ssize_t tar_get_file_size(WalWriteMethod *wwmethod,
								 const char *pathname);
static char *tar_get_file_name(WalWriteMethod *wwmethod,
							   const char *pathname, const char *temp_suffix);
static ssize_t tar_write(Walfile *f, const void *buf, size_t count);
static int	tar_sync(Walfile *f);
static bool tar_finish(WalWriteMethod *wwmethod);
static void tar_free(WalWriteMethod *wwmethod);

const WalWriteMethodOps WalTarMethodOps = {
	.open_for_write = tar_open_for_write,
	.close = tar_close,
	.existsfile = tar_existsfile,
	.get_file_size = tar_get_file_size,
	.get_file_name = tar_get_file_name,
	.write = tar_write,
	.sync = tar_sync,
	.finish = tar_finish,
	.free = tar_free
};

typedef struct TarMethodFile
{
	Walfile		base;
	pgoff_t		ofs_start;		/* 此文件的头部从哪里开始           */
	char		header[TAR_BLOCK_SIZE];
	size_t		pad_to_size;
} TarMethodFile;

typedef struct TarMethodData
{
	WalWriteMethod base;
	char	   *tarfilename;
	int			fd;
	TarMethodFile *currentfile;
#ifdef HAVE_LIBZ
	z_streamp	zp;
	void	   *zlibOut;
#endif
} TarMethodData;

#ifdef HAVE_LIBZ
static bool
tar_write_compressed_data(TarMethodData *tar_data, void *buf, size_t count,
						  bool flush)
{
	tar_data->zp->next_in = buf;
	tar_data->zp->avail_in = count;

	while (tar_data->zp->avail_in || flush)
	{
		int			r;

		r = deflate(tar_data->zp, flush ? Z_FINISH : Z_NO_FLUSH);
		if (r == Z_STREAM_ERROR)
		{
			tar_data->base.lasterrstring = _("could not compress data");
			return false;
		}

		if (tar_data->zp->avail_out < ZLIB_OUT_SIZE)
		{
			size_t		len = ZLIB_OUT_SIZE - tar_data->zp->avail_out;

			errno = 0;
			if (write(tar_data->fd, tar_data->zlibOut, len) != len)
			{
				/* 若 write 未设置 errno，则假定为磁盘空间不足 */
				tar_data->base.lasterrno = errno ? errno : ENOSPC;
				return false;
			}

			tar_data->zp->next_out = tar_data->zlibOut;
			tar_data->zp->avail_out = ZLIB_OUT_SIZE;
		}

		if (r == Z_STREAM_END)
			break;
	}

	if (flush)
	{
		/* 重置流以便写入        */
		if (deflateReset(tar_data->zp) != Z_OK)
		{
			tar_data->base.lasterrstring = _("could not reset compression stream");
			return false;
		}
	}

	return true;
}
#endif

static ssize_t
tar_write(Walfile *f, const void *buf, size_t count)
{
	TarMethodData *tar_data = (TarMethodData *) f->wwmethod;
	ssize_t		r;

	Assert(f != NULL);
	clear_error(f->wwmethod);

	/* tar 文件始终位于末尾                 */
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		r = write(tar_data->fd, buf, count);
		if (r != count)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			f->wwmethod->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}
		f->currpos += r;
		return r;
	}
#ifdef HAVE_LIBZ
	else if (f->wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		if (!tar_write_compressed_data(tar_data, unconstify(void *, buf),
									   count, false))
			return -1;
		f->currpos += count;
		return count;
	}
#endif
	else
	{
		/* 不可能发生 - 压缩已启用但未设置方法   */
		f->wwmethod->lasterrno = ENOSYS;
		return -1;
	}
}

static bool
tar_write_padding_data(TarMethodFile *f, size_t bytes)
{
	PGAlignedXLogBlock zerobuf;
	size_t		bytesleft = bytes;

	memset(zerobuf.data, 0, XLOG_BLCKSZ);
	while (bytesleft)
	{
		size_t		bytestowrite = Min(bytesleft, XLOG_BLCKSZ);
		ssize_t		r = tar_write(&f->base, zerobuf.data, bytestowrite);

		if (r < 0)
			return false;
		bytesleft -= r;
	}

	return true;
}

static char *
tar_get_file_name(WalWriteMethod *wwmethod, const char *pathname,
				  const char *temp_suffix)
{
	char	   *filename = pg_malloc0(MAXPGPATH * sizeof(char));

	snprintf(filename, MAXPGPATH, "%s%s",
			 pathname, temp_suffix ? temp_suffix : "");

	return filename;
}

static Walfile *
tar_open_for_write(WalWriteMethod *wwmethod, const char *pathname,
				   const char *temp_suffix, size_t pad_to_size)
{
	TarMethodData *tar_data = (TarMethodData *) wwmethod;
	char	   *tmppath;

	clear_error(wwmethod);

	if (tar_data->fd < 0)
	{
		/*
		 * 我们只在第一次尝试写入时才打开 tar 文件。
		 */
		tar_data->fd = open(tar_data->tarfilename,
							O_WRONLY | O_CREAT | PG_BINARY,
							pg_file_create_mode);
		if (tar_data->fd < 0)
		{
			wwmethod->lasterrno = errno;
			return NULL;
		}

#ifdef HAVE_LIBZ
		if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
		{
			tar_data->zp = (z_streamp) pg_malloc(sizeof(z_stream));
			tar_data->zp->zalloc = Z_NULL;
			tar_data->zp->zfree = Z_NULL;
			tar_data->zp->opaque = Z_NULL;
			tar_data->zp->next_out = tar_data->zlibOut;
			tar_data->zp->avail_out = ZLIB_OUT_SIZE;

			/*
			 * 初始化 deflation 库。对 windowBits 参数的            
			 * 默认值 15 加上魔法值 16，会使输出成为       
			 * gzip 而非 zlib。  
			 */
			if (deflateInit2(tar_data->zp, wwmethod->compression_level,
							 Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			{
				pg_free(tar_data->zp);
				tar_data->zp = NULL;
				wwmethod->lasterrstring =
					_("could not initialize compression library");
				return NULL;
			}
		}
#endif

		/* 没有 tar 头部本身，文件从常规文件开始            */
	}

	if (tar_data->currentfile != NULL)
	{
		wwmethod->lasterrstring =
			_("implementation error: tar files can't have more than one open file");
		return NULL;
	}

	tar_data->currentfile = pg_malloc0(sizeof(TarMethodFile));
	tar_data->currentfile->base.wwmethod = wwmethod;

	tmppath = tar_get_file_name(wwmethod, pathname, temp_suffix);

	/* 创建大小设为 0 的头部 - 我们将在关闭时填充大小      */
	if (tarCreateHeader(tar_data->currentfile->header, tmppath, NULL, 0, S_IRUSR | S_IWUSR, 0, 0, time(NULL)) != TAR_OK)
	{
		pg_free(tar_data->currentfile);
		pg_free(tmppath);
		tar_data->currentfile = NULL;
		wwmethod->lasterrstring = _("could not create tar header");
		return NULL;
	}

	pg_free(tmppath);

#ifdef HAVE_LIBZ
	if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* 刷新现有数据  */
		if (!tar_write_compressed_data(tar_data, NULL, 0, true))
			return NULL;

		/* 为头部关闭压缩           */
		if (deflateParams(tar_data->zp, 0, Z_DEFAULT_STRATEGY) != Z_OK)
		{
			wwmethod->lasterrstring =
				_("could not change compression parameters");
			return NULL;
		}
	}
#endif

	tar_data->currentfile->ofs_start = lseek(tar_data->fd, 0, SEEK_CUR);
	if (tar_data->currentfile->ofs_start == -1)
	{
		wwmethod->lasterrno = errno;
		pg_free(tar_data->currentfile);
		tar_data->currentfile = NULL;
		return NULL;
	}
	tar_data->currentfile->base.currpos = 0;

	if (wwmethod->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		if (write(tar_data->fd, tar_data->currentfile->header,
				  TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			wwmethod->lasterrno = errno ? errno : ENOSPC;
			pg_free(tar_data->currentfile);
			tar_data->currentfile = NULL;
			return NULL;
		}
	}
#ifdef HAVE_LIBZ
	else if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* 通过 zlib API 写入头部，但不压缩                */
		if (!tar_write_compressed_data(tar_data, tar_data->currentfile->header,
									   TAR_BLOCK_SIZE, true))
			return NULL;

		/* 为文件剩余部分重新启用压缩        */
		if (deflateParams(tar_data->zp, wwmethod->compression_level,
						  Z_DEFAULT_STRATEGY) != Z_OK)
		{
			wwmethod->lasterrstring = _("could not change compression parameters");
			return NULL;
		}
	}
#endif
	else
	{
		/* 不可达     */
		Assert(false);
	}

	tar_data->currentfile->base.pathname = pg_strdup(pathname);

	/*
	 * 未压缩文件在创建时填充，但压缩时无法这样做。     
	 *        
	 */
	if (pad_to_size)
	{
		tar_data->currentfile->pad_to_size = pad_to_size;
		if (wwmethod->compression_algorithm == PG_COMPRESSION_NONE)
		{
			/* 未压缩，立即填充 */
			if (!tar_write_padding_data(tar_data->currentfile, pad_to_size))
				return NULL;
			/* 回到开头       */
			if (lseek(tar_data->fd,
					  tar_data->currentfile->ofs_start + TAR_BLOCK_SIZE,
					  SEEK_SET) != tar_data->currentfile->ofs_start + TAR_BLOCK_SIZE)
			{
				wwmethod->lasterrno = errno;
				return NULL;
			}

			tar_data->currentfile->base.currpos = 0;
		}
	}

	return &tar_data->currentfile->base;
}

static ssize_t
tar_get_file_size(WalWriteMethod *wwmethod, const char *pathname)
{
	clear_error(wwmethod);

	/* 目前未使用，因此不支持    */
	wwmethod->lasterrno = ENOSYS;
	return -1;
}

static int
tar_sync(Walfile *f)
{
	TarMethodData *tar_data = (TarMethodData *) f->wwmethod;
	int			r;

	Assert(f != NULL);
	clear_error(f->wwmethod);

	if (!f->wwmethod->sync)
		return 0;

	/*
	 * 总是同步整个 tar 文件，因为我们能做的仅此而已。   
	 * 对压缩文件没有意义，因此直接忽略。
	 */
	if (f->wwmethod->compression_algorithm != PG_COMPRESSION_NONE)
		return 0;

	r = fsync(tar_data->fd);
	if (r < 0)
		f->wwmethod->lasterrno = errno;
	return r;
}

static int
tar_close(Walfile *f, WalCloseMethod method)
{
	ssize_t		filesize;
	int			padding;
	TarMethodData *tar_data = (TarMethodData *) f->wwmethod;
	TarMethodFile *tf = (TarMethodFile *) f;

	Assert(f != NULL);
	clear_error(f->wwmethod);

	if (method == CLOSE_UNLINK)
	{
		if (f->wwmethod->compression_algorithm != PG_COMPRESSION_NONE)
		{
			f->wwmethod->lasterrstring = _("unlink not supported with compression");
			return -1;
		}

		/*
		 * 删除我们刚写入 tar 的文件。我们的做法是    
		 * 将其截断到头部起始位置。这样做很安全，因为  
		 * 我们只写入最后一个文件。
		 */
		if (ftruncate(tar_data->fd, tf->ofs_start) != 0)
		{
			f->wwmethod->lasterrno = errno;
			return -1;
		}

		pg_free(tf->base.pathname);
		pg_free(tf);
		tar_data->currentfile = NULL;

		return 0;
	}

	/*
	 * 必要时用零填充文件本身。注意这与 tar 格式    
	 * 填充不同——这是我们在打开文件时                     
	 * 所要求的填充。        
	 */
	if (tf->pad_to_size)
	{
		if (f->wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
		{
			/*
			 * 压缩的 tar 文件在关闭时填充，因为我们直到最后才知道
			 * 压缩输出的大小。
			           */
			size_t		sizeleft = tf->pad_to_size - tf->base.currpos;

			if (sizeleft)
			{
				if (!tar_write_padding_data(tf, sizeleft))
					return -1;
			}
		}
		else
		{
			/*
			 * 未压缩 tar 文件在创建时已填充，因此只需调整
			 * 当前位置，如同已 seek 到末尾一样。
			 */
			tf->base.currpos = tf->pad_to_size;
		}
	}

	/*
	 * 获取文件大小，并将其填充到 tar 块大小的倍数。   
	 *      
	 */
	filesize = f->currpos;
	padding = tarPaddingBytesRequired(filesize);
	if (padding)
	{
		char		zerobuf[TAR_BLOCK_SIZE] = {0};

		if (tar_write(f, zerobuf, padding) != padding)
			return -1;
	}


#ifdef HAVE_LIBZ
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* 刷新当前缓冲区    */
		if (!tar_write_compressed_data(tar_data, NULL, 0, true))
			return -1;
	}
#endif

	/*
	 * 现在回头用正确的文件大小更新头部，并可能还
	 * 要重命名该文件。完成后我们会覆盖整个当前头部，  
	 * 包括校验和。                
	 */
	print_tar_number(&(tf->header[124]), 12, filesize);

	if (method == CLOSE_NORMAL)

		/*
		 * 如果没有临时文件名，就用原内容覆盖它，      
		 * 因为反正我们都要写入该缓冲区。
		 */
		strlcpy(&(tf->header[0]), tf->base.pathname, 100);

	print_tar_number(&(tf->header[148]), 8, tarChecksum(((TarMethodFile *) f)->header));
	if (lseek(tar_data->fd, tf->ofs_start, SEEK_SET) != ((TarMethodFile *) f)->ofs_start)
	{
		f->wwmethod->lasterrno = errno;
		return -1;
	}
	if (f->wwmethod->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		if (write(tar_data->fd, tf->header, TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE)
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			f->wwmethod->lasterrno = errno ? errno : ENOSPC;
			return -1;
		}
	}
#ifdef HAVE_LIBZ
	else if (f->wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		/* 关闭压缩         */
		if (deflateParams(tar_data->zp, 0, Z_DEFAULT_STRATEGY) != Z_OK)
		{
			f->wwmethod->lasterrstring = _("could not change compression parameters");
			return -1;
		}

		/* 覆盖头部，假设大小相同                        */
		if (!tar_write_compressed_data(tar_data, tar_data->currentfile->header,
									   TAR_BLOCK_SIZE, true))
			return -1;

		/* 重新开启压缩       */
		if (deflateParams(tar_data->zp, f->wwmethod->compression_level,
						  Z_DEFAULT_STRATEGY) != Z_OK)
		{
			f->wwmethod->lasterrstring = _("could not change compression parameters");
			return -1;
		}
	}
#endif
	else
	{
		/* 不可达     */
		Assert(false);
	}

	/* 将文件指针移回末尾，以便写入下一个文件         */
	if (lseek(tar_data->fd, 0, SEEK_END) < 0)
	{
		f->wwmethod->lasterrno = errno;
		return -1;
	}

	/* 关闭时总是 fsync，使填充也被 fsync      */
	if (tar_sync(f) < 0)
	{
		/* XXX 这似乎很不合理；为何仅此情况致命？   */
		pg_fatal("could not fsync file \"%s\": %s",
				 tf->base.pathname, GetLastWalMethodError(f->wwmethod));
	}

	/* 清理并完成   */
	pg_free(tf->base.pathname);
	pg_free(tf);
	tar_data->currentfile = NULL;

	return 0;
}

static bool
tar_existsfile(WalWriteMethod *wwmethod, const char *pathname)
{
	clear_error(wwmethod);
	/* 我们只处理新 tar 文件，因此不存在外部创建的内容 */
	return false;
}

static bool
tar_finish(WalWriteMethod *wwmethod)
{
	TarMethodData *tar_data = (TarMethodData *) wwmethod;
	char		zerobuf[1024] = {0};

	clear_error(wwmethod);

	if (tar_data->currentfile)
	{
		if (tar_close(&tar_data->currentfile->base, CLOSE_NORMAL) != 0)
			return false;
	}

	/* tar 文件始终以两个空块结束       */
	if (wwmethod->compression_algorithm == PG_COMPRESSION_NONE)
	{
		errno = 0;
		if (write(tar_data->fd, zerobuf, sizeof(zerobuf)) != sizeof(zerobuf))
		{
			/* 若 write 未设置 errno，则假定为磁盘空间不足 */
			wwmethod->lasterrno = errno ? errno : ENOSPC;
			return false;
		}
	}
#ifdef HAVE_LIBZ
	else if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
	{
		if (!tar_write_compressed_data(tar_data, zerobuf, sizeof(zerobuf),
									   false))
			return false;

		/* 同时刷新所有数据以确保 gzip 流已结束          */
		tar_data->zp->next_in = NULL;
		tar_data->zp->avail_in = 0;
		while (true)
		{
			int			r;

			r = deflate(tar_data->zp, Z_FINISH);

			if (r == Z_STREAM_ERROR)
			{
				wwmethod->lasterrstring = _("could not compress data");
				return false;
			}
			if (tar_data->zp->avail_out < ZLIB_OUT_SIZE)
			{
				size_t		len = ZLIB_OUT_SIZE - tar_data->zp->avail_out;

				errno = 0;
				if (write(tar_data->fd, tar_data->zlibOut, len) != len)
				{
					/*
					 * 若 write 未设置 errno，视为磁盘空间不足
					 *       
					 */
					wwmethod->lasterrno = errno ? errno : ENOSPC;
					return false;
				}
			}
			if (r == Z_STREAM_END)
				break;
		}

		if (deflateEnd(tar_data->zp) != Z_OK)
		{
			wwmethod->lasterrstring = _("could not close compression stream");
			return false;
		}
	}
#endif
	else
	{
		/* 不可达     */
		Assert(false);
	}

	/* 同样 sync 空块，因为它们在最后一个文件之后     */
	if (wwmethod->sync)
	{
		if (fsync(tar_data->fd) != 0)
		{
			wwmethod->lasterrno = errno;
			return false;
		}
	}

	if (close(tar_data->fd) != 0)
	{
		wwmethod->lasterrno = errno;
		return false;
	}

	tar_data->fd = -1;

	if (wwmethod->sync)
	{
		if (fsync_fname(tar_data->tarfilename, false) != 0 ||
			fsync_parent_path(tar_data->tarfilename) != 0)
		{
			wwmethod->lasterrno = errno;
			return false;
		}
	}

	return true;
}

static void
tar_free(WalWriteMethod *wwmethod)
{
	TarMethodData *tar_data = (TarMethodData *) wwmethod;

	pg_free(tar_data->tarfilename);
#ifdef HAVE_LIBZ
	if (wwmethod->compression_algorithm == PG_COMPRESSION_GZIP)
		pg_free(tar_data->zlibOut);
#endif
	pg_free(wwmethod);
}

/*
 * 参数 compression_algorithm 目前被忽略。它的存在是为了与    
 * CreateWalDirectoryMethod 对称，后者使用它来区分             
 * 不同的压缩方法。CreateWalTarMethod 及其系列函数               
 * 只处理 zlib 压缩。                  
 */
WalWriteMethod *
CreateWalTarMethod(const char *tarbase,
				   pg_compress_algorithm compression_algorithm,
				   int compression_level, bool sync)
{
	TarMethodData *wwmethod;
	const char *suffix = (compression_algorithm == PG_COMPRESSION_GZIP) ?
		".tar.gz" : ".tar";

	wwmethod = pg_malloc0(sizeof(TarMethodData));
	*((const WalWriteMethodOps **) &wwmethod->base.ops) =
		&WalTarMethodOps;
	wwmethod->base.compression_algorithm = compression_algorithm;
	wwmethod->base.compression_level = compression_level;
	wwmethod->base.sync = sync;
	clear_error(&wwmethod->base);

	wwmethod->tarfilename = pg_malloc0(strlen(tarbase) + strlen(suffix) + 1);
	sprintf(wwmethod->tarfilename, "%s%s", tarbase, suffix);
	wwmethod->fd = -1;
#ifdef HAVE_LIBZ
	if (compression_algorithm == PG_COMPRESSION_GZIP)
		wwmethod->zlibOut = (char *) pg_malloc(ZLIB_OUT_SIZE + 1);
#endif

	return &wwmethod->base;
}

const char *
GetLastWalMethodError(WalWriteMethod *wwmethod)
{
	if (wwmethod->lasterrstring)
		return wwmethod->lasterrstring;
	return strerror(wwmethod->lasterrno);
}
