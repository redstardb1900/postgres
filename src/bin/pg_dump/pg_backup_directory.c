/*-------------------------------------------------------------------------
 *
 * pg_backup_directory.c
 *
 *	 目录格式导出是一个目录，其中包含一个 "toc.dat" 文件
 *	 用于 TOC，以及每个数据条目单独的 "<oid>.dat" 文件。
 *	 大对象存储在以 "blob_<oid>.dat" 命名的单独文件中，
 *	 并有一个名为 "blobs.toc" 的纯文本 TOC 文件。如果
 *	 使用压缩，每个数据文件会单独压缩，并在文件名
 *	 后添加 ".gz" 后缀。TOC 文件从未被 pg_dump 压缩，
 *	 不过也接受带 .gz 后缀的文件，以防用户手动
 *	 用 'gzip' 压缩它们。
 *
 *	 注意：此格式与 tar 格式中写入的 tar 文件
 *	 完全相同，只是我们不写 restore.sql 文件（TODO），
 *	 且 tar 格式不支持压缩。请保持这两种格式
 *	 同步。
 *
 *
 *	Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *	Portions Copyright (c) 2000, Philip Warner
 *
 *	Rights are granted to use this software in any way so long
 *	as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_directory.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>

#include "common/file_utils.h"
#include "compress_io.h"
#include "parallel.h"
#include "pg_backup_utils.h"

typedef struct
{
	/*
	 * 我们的归档位置。基本上就是用户指定的
	 * 备份文件，但这里它是一个目录。
	 */
	char	   *directory;

	CompressFileHandle *dataFH; /* 当前打开的数据文件 */
	CompressFileHandle *LOsTocFH;	/* blobs.toc 的文件句柄 */
	ParallelState *pstate;		/* 用于并行备份 / 恢复 */
} lclContext;

typedef struct
{
	char	   *filename;		/* 文件名，不包含目录（基本名称） */
} lclTocEntry;

/* 私有函数的原型 */
static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static int	_ReadByte(ArchiveHandle *AH);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _ReadBuf(ArchiveHandle *AH, void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _ReopenArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);

static void _WriteExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _ReadExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _PrintExtraToc(ArchiveHandle *AH, TocEntry *te);

static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);
static void _LoadLOs(ArchiveHandle *AH);

static void _PrepParallelRestore(ArchiveHandle *AH);
static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);

static int	_WorkerJobRestoreDirectory(ArchiveHandle *AH, TocEntry *te);
static int	_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te);

static void setFilePath(ArchiveHandle *AH, char *buf,
						const char *relativeFilename);

/*
 *	所有格式都需要的初始化例程。这是一个全局例程，
 *	应在 pg_backup_archiver.h 中声明。
 *
 *	其任务是创建任何额外的归档上下文（使用 AH->formatData），
 *	并初始化支持的函数指针。
 *
 *	它还应为读/写准备输入源，
 *	在读取模式下，应加载头部和 TOC。
 */
void
InitArchiveFmt_Directory(ArchiveHandle *AH)
{
	lclContext *ctx;

	/* 假设使用静态函数，这对每种格式都一样。 */
	AH->ArchiveEntryPtr = _ArchiveEntry;
	AH->StartDataPtr = _StartData;
	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->ReadBytePtr = _ReadByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ReadBufPtr = _ReadBuf;
	AH->ClosePtr = _CloseArchive;
	AH->ReopenPtr = _ReopenArchive;
	AH->PrintTocDataPtr = _PrintTocData;
	AH->ReadExtraTocPtr = _ReadExtraToc;
	AH->WriteExtraTocPtr = _WriteExtraToc;
	AH->PrintExtraTocPtr = _PrintExtraToc;

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;

	AH->PrepParallelRestorePtr = _PrepParallelRestore;
	AH->ClonePtr = _Clone;
	AH->DeClonePtr = _DeClone;

	AH->WorkerJobRestorePtr = _WorkerJobRestoreDirectory;
	AH->WorkerJobDumpPtr = _WorkerJobDumpDirectory;

	/* 设置我们的私有上下文 */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = (void *) ctx;

	ctx->dataFH = NULL;
	ctx->LOsTocFH = NULL;

	/* 初始化 LO 缓冲 */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) pg_malloc(LOBBUFSIZE);

	/*
	 * 现在打开 TOC 文件
	 */

	if (!AH->fSpec || strcmp(AH->fSpec, "") == 0)
		pg_fatal("no output directory specified");

	ctx->directory = AH->fSpec;

	if (AH->mode == archModeWrite)
	{
		struct stat st;
		bool		is_empty = false;

		/* 我们接受一个空的现有目录 */
		if (stat(ctx->directory, &st) == 0 && S_ISDIR(st.st_mode))
		{
			DIR		   *dir = opendir(ctx->directory);

			if (dir)
			{
				struct dirent *d;

				is_empty = true;
				while (errno = 0, (d = readdir(dir)))
				{
					if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0)
					{
						is_empty = false;
						break;
					}
				}

				if (errno)
					pg_fatal("could not read directory \"%s\": %m",
							 ctx->directory);

				if (closedir(dir))
					pg_fatal("could not close directory \"%s\": %m",
							 ctx->directory);
			}
		}

		if (!is_empty && mkdir(ctx->directory, 0700) < 0)
			pg_fatal("could not create directory \"%s\": %m",
					 ctx->directory);
	}
	else
	{							/* 读取模式 */
		char		fname[MAXPGPATH];
		CompressFileHandle *tocFH;

		setFilePath(AH, fname, "toc.dat");

		tocFH = InitDiscoverCompressFileHandle(fname, PG_BINARY_R);
		if (tocFH == NULL)
			pg_fatal("could not open input file \"%s\": %m", fname);

		ctx->dataFH = tocFH;

		/*
		 * 目录格式导出的 TOC 与 tar 格式
		 * 共享格式代码。
		 */
		AH->format = archTar;
		ReadHead(AH);
		AH->format = archDirectory;
		ReadToc(AH);

		/* 文件中没有其他内容，因此再次关闭它... */
		if (!EndCompressFileHandle(tocFH))
			pg_fatal("could not close TOC file: %m");
		ctx->dataFH = NULL;
	}
}

/*
 * 当 dumper 创建新的 TOC 条目时由 Archiver 调用。
 *
 * 我们为该条目确定文件名。
*/
static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx;
	char		fn[MAXPGPATH];

	tctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
	if (strcmp(te->desc, "BLOBS") == 0)
		tctx->filename = pg_strdup("blobs.toc");
	else if (te->dataDumper)
	{
		snprintf(fn, MAXPGPATH, "%d.dat", te->dumpId);
		tctx->filename = pg_strdup(fn);
	}
	else
		tctx->filename = NULL;

	te->formatData = (void *) tctx;
}

/*
 * 由 Archiver 调用以保存任何额外的格式相关 TOC 条目
 * 数据。
 *
 * 使用 Archiver 例程写入数据——它们是非端序的，并
 * 维护其他重要的文件信息。
 */
static void
_WriteExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	/*
	 * 可导出的对象设置了 tctx->filename，其他对象没有。
	 * （参见 _ArchiveEntry）。
	 */
	if (tctx->filename)
		WriteStr(AH, tctx->filename);
	else
		WriteStr(AH, "");
}

/*
 * 由 Archiver 调用以读取任何额外的格式相关 TOC 数据。
 *
 * 需要匹配 _WriteExtraToc 中定义的顺序，也应
 * 使用 Archiver 输入例程。
 */
static void
_ReadExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (tctx == NULL)
	{
		tctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
		te->formatData = (void *) tctx;
	}

	tctx->filename = ReadStr(AH);
	if (strlen(tctx->filename) == 0)
	{
		free(tctx->filename);
		tctx->filename = NULL;
	}
}

/*
 * 当恢复归档时由 Archiver 调用，输出一条
 * 包含 TOC 条目有用信息的注释。
 */
static void
_PrintExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (AH->public.verbose && tctx->filename)
		ahprintf(AH, "-- File: %s\n", tctx->filename);
}

/*
 * 当保存表数据（非模式）时由 archiver 调用。此例程
 * 应保存恢复归档时读取所需的格式特定
 * 信息。
 *
 * 它在 dumper 的 'DataDumper' 例程被调用之前调用。
 *
 * 我们创建数据文件以供写入。
 */
static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	setFilePath(AH, fname, tctx->filename);

	ctx->dataFH = InitCompressFileHandle(AH->compression_spec);

	if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * 当 dumper 调用 WriteData 时由 archiver 调用。此例程
 * 同时为 LO 和表数据调用；格式的责任是
 * 使用 StartLO/StartData 管理每种数据。
 *
 * 它应仅在 DataDumper 例程中调用。
 *
 * 我们将数据写入打开的数据文件。
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	if (dLen <= 0)
		return;
	CFH->write_func(data, dLen, CFH);
}

/*
 * 当 dumper 的 'DataDumper' 例程完成后
 * 由 archiver 调用。
 *
 * 我们关闭数据文件。
 */
static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	/* 关闭文件 */
	if (!EndCompressFileHandle(ctx->dataFH))
		pg_fatal("could not close data file: %m");

	ctx->dataFH = NULL;
}

/*
 * 打印给定文件的数据（也可以是 LO）
 */
static void
_PrintFileData(ArchiveHandle *AH, char *filename)
{
	size_t		cnt;
	char	   *buf;
	size_t		buflen;
	CompressFileHandle *CFH;

	if (!filename)
		return;

	CFH = InitDiscoverCompressFileHandle(filename, PG_BINARY_R);
	if (!CFH)
		pg_fatal("could not open input file \"%s\": %m", filename);

	buflen = DEFAULT_IO_BUFFER_SIZE;
	buf = pg_malloc(buflen);

	while ((cnt = CFH->read_func(buf, buflen, CFH)) > 0)
	{
		ahwrite(buf, 1, cnt, AH);
	}

	free(buf);
	if (!EndCompressFileHandle(CFH))
		pg_fatal("could not close data file \"%s\": %m", filename);
}

/*
 * 打印给定 TOC 条目的数据
*/
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (!tctx->filename)
		return;

	if (strcmp(te->desc, "BLOBS") == 0)
		_LoadLOs(AH);
	else
	{
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, tctx->filename);
		_PrintFileData(AH, fname);
	}
}

static void
_LoadLOs(ArchiveHandle *AH)
{
	Oid			oid;
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH;
	char		tocfname[MAXPGPATH];
	char		line[MAXPGPATH];

	StartRestoreLOs(AH);

	setFilePath(AH, tocfname, "blobs.toc");

	CFH = ctx->LOsTocFH = InitDiscoverCompressFileHandle(tocfname, PG_BINARY_R);

	if (ctx->LOsTocFH == NULL)
		pg_fatal("could not open large object TOC file \"%s\" for input: %m",
				 tocfname);

	/* 逐行读取 LOs TOC 文件，并处理每个 LO */
	while ((CFH->gets_func(line, MAXPGPATH, CFH)) != NULL)
	{
		char		lofname[MAXPGPATH + 1];
		char		path[MAXPGPATH];

		/* 不会溢出，因为 line 和 lofname 长度相同 */
		if (sscanf(line, "%u %" CppAsString2(MAXPGPATH) "s\n", &oid, lofname) != 2)
			pg_fatal("invalid line in large object TOC file \"%s\": \"%s\"",
					 tocfname, line);

		StartRestoreLO(AH, oid, AH->public.ropt->dropSchema);
		snprintf(path, MAXPGPATH, "%s/%s", ctx->directory, lofname);
		_PrintFileData(AH, path);
		EndRestoreLO(AH, oid);
	}
	if (!CFH->eof_func(CFH))
		pg_fatal("error reading large object TOC file \"%s\"",
				 tocfname);

	if (!EndCompressFileHandle(ctx->LOsTocFH))
		pg_fatal("could not close large object TOC file \"%s\": %m",
				 tocfname);

	ctx->LOsTocFH = NULL;

	EndRestoreLOs(AH);
}


/*
 * 向归档写入一个字节的数据。
 * 由 archiver 调用以进行整数和字节输出到归档。
 * 这些例程仅用于读取和写入头部和 TOC。
 */
static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	unsigned char c = (unsigned char) i;
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	CFH->write_func(&c, 1, CFH);
	return 1;
}

/*
 * 从归档读取一个字节的数据。
 * 由 archiver 调用以从归档读取字节和整数。
 * 这些例程仅用于读取和写入头部和 TOC。
 * EOF 应被视为致命错误。
 */
static int
_ReadByte(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	return CFH->getc_func(CFH);
}

/*
 * 向归档写入一个数据缓冲区。
 * 由 archiver 调用以将一块字节写入 TOC 或数据文件。
 */
static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	CFH->write_func(buf, len, CFH);
}

/*
 * 从归档读取一块字节。
 *
 * 由 archiver 调用以从归档读取一块字节
 */
static void
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	/*
	 * 我们不期望短读取，因此如果遇到就失败。read_func
	 * 已处理任何直接的 I/O 错误。
	 */
	if (CFH->read_func(buf, len, CFH) != len)
		pg_fatal("could not read from input file: end of file");
}

/*
 * 关闭归档。
 *
 * 写入归档时，这是实际启动
 * 将其保存到文件过程的例程。在此点之前不应写入数据，
 * 因为用户可以在创建 TOC 后对其进行排序。
 *
 * 如果要写入归档，此例程必须调用：
 *      WriteHead        保存归档头部
 *      WriteToc         保存 TOC 条目
 *      WriteDataChunks  保存所有数据和 LO。
 */
static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (AH->mode == archModeWrite)
	{
		CompressFileHandle *tocFH;
		pg_compress_specification compression_spec = {0};
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, "toc.dat");

		/* 这将为并行备份实际 fork 进程 */
		ctx->pstate = ParallelBackupStart(AH);

		/* TOC 总是以未压缩方式创建 */
		compression_spec.algorithm = PG_COMPRESSION_NONE;
		tocFH = InitCompressFileHandle(compression_spec);
		if (!tocFH->open_write_func(fname, PG_BINARY_W, tocFH))
			pg_fatal("could not open output file \"%s\": %m", fname);
		ctx->dataFH = tocFH;

		/*
		 * 在 toc.dat 文件的格式字段中写入 'tar'。目录
		 * 与 'tar' 兼容，因此没有必要为其使用不同的
		 * 格式代码。
		 */
		AH->format = archTar;
		WriteHead(AH);
		AH->format = archDirectory;
		WriteToc(AH);
		if (!EndCompressFileHandle(tocFH))
			pg_fatal("could not close TOC file: %m");
		WriteDataChunks(AH, ctx->pstate);

		ParallelBackupEnd(AH, ctx->pstate);

		/*
		 * 在目录模式下，无需单独同步所有条目。
		 * 只需递归遍历一次所有生成的文件。
		 */
		if (AH->dosync)
			fsync_dir_recurse(ctx->directory);
	}
	AH->FH = NULL;
}

/*
 * 重新打开归档的文件句柄。
 */
static void
_ReopenArchive(ArchiveHandle *AH)
{
	/*
	 * 我们的 TOC 在内存中，数据文件由每个子进程单独打开，
	 * 因为它们是分开的。我们通过简单地不做任何事情来
	 * 支持重新打开归档。
	 */
}

/*
 * LO 支持
 */

/*
 * 当开始保存所有 BLOB 数据（非模式）时由 archiver 调用。
 * 它在 dumper 的 DataDumper 例程之前调用。
 *
 * 我们在此处打开大对象 TOC 文件，以便可以为
 * 每个 LO 追加一行。
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	pg_compress_specification compression_spec = {0};
	char		fname[MAXPGPATH];

	setFilePath(AH, fname, "blobs.toc");

	/* LO TOC 文件从不压缩 */
	compression_spec.algorithm = PG_COMPRESSION_NONE;
	ctx->LOsTocFH = InitCompressFileHandle(compression_spec);
	if (!ctx->LOsTocFH->open_write_func(fname, "ab", ctx->LOsTocFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * 当我们要开始导出 LO 时由 archiver 调用。
 *
 * 我们创建一个文件来写入 LO。
 */
static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	snprintf(fname, MAXPGPATH, "%s/blob_%u.dat", ctx->directory, oid);

	ctx->dataFH = InitCompressFileHandle(AH->compression_spec);
	if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * 当 dumper 完成写入 LO 时由 archiver 调用。
 *
 * 我们关闭 LO 文件并为其在 LO TOC 文件中写入一个条目。
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->LOsTocFH;
	char		buf[50];
	int			len;

	/* 关闭 BLOB 数据文件本身 */
	if (!EndCompressFileHandle(ctx->dataFH))
		pg_fatal("could not close LO data file: %m");
	ctx->dataFH = NULL;

	/* 在 blobs.toc 中注册该 LO */
	len = snprintf(buf, sizeof(buf), "%u blob_%u.dat\n", oid, oid);
	CFH->write_func(buf, len, CFH);
}

/*
 * 当完成保存所有 BLOB 数据时由 archiver 调用。
 *
 * 我们关闭 LOs TOC 文件。
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (!EndCompressFileHandle(ctx->LOsTocFH))
		pg_fatal("could not close LOs TOC file: %m");
	ctx->LOsTocFH = NULL;
}

/*
 * 获取相对文件名并在其前面加上输出目录，将
 * 结果写入 buf。调用者需要确保 buf 的大小为 MAXPGPATH 字节。
 * 不能在函数内使用 static char[MAXPGPATH]，因为我们在
 * Windows 上是多线程运行的。
 */
static void
setFilePath(ArchiveHandle *AH, char *buf, const char *relativeFilename)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char	   *dname;

	dname = ctx->directory;

	if (strlen(dname) + 1 + strlen(relativeFilename) + 1 > MAXPGPATH)
		pg_fatal("file name too long: \"%s\"", dname);

	strcpy(buf, dname);
	strcat(buf, "/");
	strcat(buf, relativeFilename);
}

/*
 * 准备并行恢复。
 *
 * 这里需要做的主要事情是用适当的值填充 TABLE DATA
 * 和 BLOBS TOC 条目的 dataLength 字段，以指导
 * 恢复作业的排序。所述数据的来源依赖于格式，
 * 值的具体含义也是如此。
 *
 * 格式模块也可能选择在此处进行其他设置。
 */
static void
_PrepParallelRestore(ArchiveHandle *AH)
{
	TocEntry   *te;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		lclTocEntry *tctx = (lclTocEntry *) te->formatData;
		char		fname[MAXPGPATH];
		struct stat st;

		/*
		 * 可导出的对象设置了 tctx->filename，其他对象没有。
		 * （参见 _ArchiveEntry）。
		 */
		if (tctx->filename == NULL)
			continue;

		/* 我们可以忽略不需要恢复的条目 */
		if ((te->reqs & REQ_DATA) == 0)
			continue;

		/*
		 * 对文件执行 stat，如果成功，将其大小放入 dataLength。
		 * 使用压缩时，物理文件大小可能不是恢复文件
		 * 所需工作量的很好指标，但我们只需要一个
		 * 近似的指示。
		 */
		setFilePath(AH, fname, tctx->filename);

		if (stat(fname, &st) == 0)
			te->dataLength = st.st_size;
		else if (AH->compression_spec.algorithm != PG_COMPRESSION_NONE)
		{
			if (AH->compression_spec.algorithm == PG_COMPRESSION_GZIP)
				strlcat(fname, ".gz", sizeof(fname));
			else if (AH->compression_spec.algorithm == PG_COMPRESSION_LZ4)
				strlcat(fname, ".lz4", sizeof(fname));
			else if (AH->compression_spec.algorithm == PG_COMPRESSION_ZSTD)
				strlcat(fname, ".zst", sizeof(fname));

			if (stat(fname, &st) == 0)
				te->dataLength = st.st_size;
		}

		/*
		 * 如果这是 BLOBS 条目，我们 stat 的是 blobs.toc，
		 * 它很可能比实际的 blob 数据小得多。我们
		 * 无法廉价地估计小了多少，但幸运的是，
		 * 只要 LOs 被合理地尽早处理，就不太重要。
		 * 任意放大 1K 倍。
		 */
		if (strcmp(te->desc, "BLOBS") == 0)
			te->dataLength *= 1024;
	}
}

/*
 * 在并行恢复期间克隆格式特定字段。
 */
static void
_Clone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	AH->formatData = (lclContext *) pg_malloc(sizeof(lclContext));
	memcpy(AH->formatData, ctx, sizeof(lclContext));
	ctx = (lclContext *) AH->formatData;

	/*
	 * 注意：我们不会创建本地 lo_buf，因为我们期望
	 * 每个归档最多一个 BLOBS 条目，因此不可能并行处理。
	 * 同样，TOC 条目局部状态也不是问题，因为
	 * 任何 TOC 条目只由一个工作子进程处理。
	 */

	/*
	 * 我们也不复制 ParallelState 指针 (pstate)，只有
	 * 领导者进程才会写入它。
	 */
}

static void
_DeClone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	free(ctx);
}

/*
 * 此函数在目录格式归档的并行备份子进程中
 * 执行，并为一个 TOC 条目导出实际数据。
 */
static int
_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te)
{
	/*
	 * 此函数返回 void。我们要么失败并惨死，
	 * 要么成功... 失败将由父进程在子进程
	 * 意外死亡时检测到。
	 */
	WriteDataChunksForTocEntry(AH, te);

	return 0;
}

/*
 * 此函数在目录格式归档的并行恢复子进程中
 * 执行，并为一个 TOC 条目恢复实际数据。
 */
static int
_WorkerJobRestoreDirectory(ArchiveHandle *AH, TocEntry *te)
{
	return parallel_restore(AH, te);
}
