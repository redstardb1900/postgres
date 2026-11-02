/*-------------------------------------------------------------------------
 *
 * compress_io.c
 *	 归档器用于写入未压缩或压缩数据
 *	 流的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * 本文件包含两个用于处理压缩数据的 API。第一个
 * 提供更大的灵活性，使用回调函数从底层流
 * 读取/写入数据。第二个 API 是对 fopen 及其相关
 * 函数的封装，提供类似的接口，但抽象掉了
 * 可能的压缩。第二个 API 旨在让生成的文件
 * 可以使用外部压缩工具程序轻松
 * 操作。
 *
 * Compressor API
 * --------------
 *
 *	 写入归档的接口包含三个函数：
 *	 AllocateCompressor、writeData 和 EndCompressor。首先调用
 *	 AllocateCompressor，然后根据需要多次调用 writeData 写入
 *	 所有数据，最后调用 EndCompressor。每次 writeData 将
 *	 为每个压缩数据块调用提供给 AllocateCompressor 的
 *	 WriteFunc。
 *
 *	 读取归档的接口包含相同的三个函数：
 *	 AllocateCompressor、readData 和 EndCompressor。首先调用
 *	 AllocateCompressor，然后调用 readData 读取整个压缩流，
 *	 该函数会反复调用给定的 ReadFunc。ReadFunc
 *	 每次返回一个压缩数据块。然后 readData 将其解压
 *	 并将解压后的数据传递给 ahwrite()，直到 ReadFunc 返回 0
 *	 表示 EOF。压缩流和未压缩流的接口
 *	 相同。
 *
 * Compressed stream API
 * ----------------------
 *
 *	 压缩流 API 提供了一组函数指针，用于
 *	 打开、读取、写入并最终关闭文件。实现的
 *	 函数指针在相应的头文件中记录，并且对所有流
 *	 通用。它允许调用者对压缩流和
 *	 未压缩流使用相同的函数。
 *
 *	 接口包含三个函数：InitCompressFileHandle、
 *	 InitDiscoverCompressFileHandle 和 EndCompressFileHandle。如果
 *	 已知压缩方式，则从调用 InitCompressFileHandle 开始，
 *	 否则使用 InitDiscoverCompressFileHandle 来发现它。然后根据
 *	 读/写操作的需要调用函数指针。最后
 *	 调用 EndCompressFileHandle 结束流。
 *
 *	 InitDiscoverCompressFileHandle 尝试通过文件名后缀
 *	 推断压缩方式。如果后缀尚不可知，则尝试直接
 *	 打开该文件，如果失败，则尝试使用
 *	 压缩后缀（.gz、.lz4 和 .zst，按此顺序）打开同一文件。
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_io.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>

#include "compress_gzip.h"
#include "compress_io.h"
#include "compress_lz4.h"
#include "compress_none.h"
#include "compress_zstd.h"
#include "pg_backup_utils.h"

/*----------------------
 * 通用函数
 *----------------------
 */

/*
 * 检查 pg_dump/restore 中是否实现了
 * 指定压缩算法的支持。
 *
 * 成功时返回 NULL，否则返回一个已分配内存的字符串，
 * 调用者可在错误消息中使用。
 */
char *
supports_compression(const pg_compress_specification compression_spec)
{
	const pg_compress_algorithm algorithm = compression_spec.algorithm;
	bool		supported = false;

	if (algorithm == PG_COMPRESSION_NONE)
		supported = true;
#ifdef HAVE_LIBZ
	if (algorithm == PG_COMPRESSION_GZIP)
		supported = true;
#endif
#ifdef USE_LZ4
	if (algorithm == PG_COMPRESSION_LZ4)
		supported = true;
#endif
#ifdef USE_ZSTD
	if (algorithm == PG_COMPRESSION_ZSTD)
		supported = true;
#endif

	if (!supported)
		return psprintf(_("this build does not support compression with %s"),
						get_compress_algorithm_name(algorithm));

	return NULL;
}

/*----------------------
 * Compressor API
 *----------------------
 */

/*
 * 分配一个新的压缩器。
 */
CompressorState *
AllocateCompressor(const pg_compress_specification compression_spec,
				   ReadFunc readF, WriteFunc writeF)
{
	CompressorState *cs;

	cs = (CompressorState *) pg_malloc0(sizeof(CompressorState));
	cs->readF = readF;
	cs->writeF = writeF;

	if (compression_spec.algorithm == PG_COMPRESSION_NONE)
		InitCompressorNone(cs, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_GZIP)
		InitCompressorGzip(cs, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_LZ4)
		InitCompressorLZ4(cs, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_ZSTD)
		InitCompressorZstd(cs, compression_spec);

	return cs;
}

/*
 * 终止压缩库上下文并刷新其缓冲区。
 */
void
EndCompressor(ArchiveHandle *AH, CompressorState *cs)
{
	cs->end(AH, cs);
	pg_free(cs);
}

/*----------------------
 * 压缩流 API
 *----------------------
 */

/*
 * 私有例程
 */
static int
hasSuffix(const char *filename, const char *suffix)
{
	int			filenamelen = strlen(filename);
	int			suffixlen = strlen(suffix);

	if (filenamelen < suffixlen)
		return 0;

	return memcmp(&filename[filenamelen - suffixlen],
				  suffix,
				  suffixlen) == 0;
}

/* 不改变 errno 的 free()；在下面多处有用 */
static void
free_keep_errno(void *p)
{
	int			save_errno = errno;

	free(p);
	errno = save_errno;
}

/*
 * 公共接口
 */

/*
 * 为指定的压缩算法初始化压缩文件句柄。
 */
CompressFileHandle *
InitCompressFileHandle(const pg_compress_specification compression_spec)
{
	CompressFileHandle *CFH;

	CFH = pg_malloc0(sizeof(CompressFileHandle));

	if (compression_spec.algorithm == PG_COMPRESSION_NONE)
		InitCompressFileHandleNone(CFH, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_GZIP)
		InitCompressFileHandleGzip(CFH, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_LZ4)
		InitCompressFileHandleLZ4(CFH, compression_spec);
	else if (compression_spec.algorithm == PG_COMPRESSION_ZSTD)
		InitCompressFileHandleZstd(CFH, compression_spec);

	return CFH;
}

/*
 * 检查具有指定扩展名的压缩文件是否存在。
 *
 * 被测试文件的文件名存储在 fname 缓冲区中（已有的
 * 缓冲区被释放，新缓冲区通过指针分配并返回）。
 */
static bool
check_compressed_file(const char *path, char **fname, char *ext)
{
	free_keep_errno(*fname);
	*fname = psprintf("%s.%s", path, ext);
	return (access(*fname, F_OK) == 0);
}

/*
 * 打开文件以进行读取。'path' 是要打开的文件，'mode' 应为
 * "r" 或 "rb"。
 *
 * 如果 'path' 处的文件包含支持的压缩方法的后缀，
 * 目前包括 ".gz"、".lz4" 和 ".zst"，则将全程使用该压缩方式。
 * 否则，将通过迭代尝试来推断压缩方式：
 * 首先尝试直接打开 'path' 处的文件，然后追加已知的
 * 压缩后缀。因此，如果传入 "foo" 作为 'path'，将打开
 * "foo" 或 "foo.{gz,lz4,zst}"，按该顺序尝试。
 *
 * 失败时返回 NULL，并将错误码设置在 errno 中。
 */
CompressFileHandle *
InitDiscoverCompressFileHandle(const char *path, const char *mode)
{
	CompressFileHandle *CFH = NULL;
	struct stat st;
	char	   *fname;
	pg_compress_specification compression_spec = {0};

	compression_spec.algorithm = PG_COMPRESSION_NONE;

	Assert(strcmp(mode, PG_BINARY_R) == 0);

	fname = pg_strdup(path);

	if (hasSuffix(fname, ".gz"))
		compression_spec.algorithm = PG_COMPRESSION_GZIP;
	else if (hasSuffix(fname, ".lz4"))
		compression_spec.algorithm = PG_COMPRESSION_LZ4;
	else if (hasSuffix(fname, ".zst"))
		compression_spec.algorithm = PG_COMPRESSION_ZSTD;
	else
	{
		if (stat(path, &st) == 0)
			compression_spec.algorithm = PG_COMPRESSION_NONE;
		else if (check_compressed_file(path, &fname, "gz"))
			compression_spec.algorithm = PG_COMPRESSION_GZIP;
		else if (check_compressed_file(path, &fname, "lz4"))
			compression_spec.algorithm = PG_COMPRESSION_LZ4;
		else if (check_compressed_file(path, &fname, "zst"))
			compression_spec.algorithm = PG_COMPRESSION_ZSTD;
	}

	CFH = InitCompressFileHandle(compression_spec);
	errno = 0;
	if (!CFH->open_func(fname, -1, mode, CFH))
	{
		free_keep_errno(CFH);
		CFH = NULL;
	}
	free_keep_errno(fname);

	return CFH;
}

/*
 * 关闭打开的文件句柄并释放其内存。
 *
 * 失败时返回 false 并适当设置 errno。
 */
bool
EndCompressFileHandle(CompressFileHandle *CFH)
{
	bool		ret = false;

	errno = 0;
	if (CFH->private_data)
		ret = CFH->close_func(CFH);

	free_keep_errno(CFH);

	return ret;
}
