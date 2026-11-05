/*-------------------------------------------------------------------------
 *
 * pg_backup_tar.c
 *
 *	This file is copied from the 'files' format file, but dumps data into
 *	one temp file then sends it to the output TAR archive.
 *
 *	The tar format also includes a 'restore.sql' script which is there for
 *	the benefit of humans. This script is never used by pg_restore.
 *
 *	NOTE: If you untar the created 'tar' file, the resulting files are
 *	compatible with the 'directory' format. Please keep the two formats in
 *	sync.
 *
 *	See the headers to pg_backup_directory & pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_tar.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#include "common/file_utils.h"
#include "fe_utils/string_utils.h"
#include "pg_backup_archiver.h"
#include "pg_backup_tar.h"
#include "pg_backup_utils.h"
#include "pgtar.h"

static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static int	_ReadByte(ArchiveHandle *AH);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _ReadBuf(ArchiveHandle *AH, void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);
static void _WriteExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _ReadExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _PrintExtraToc(ArchiveHandle *AH, TocEntry *te);

static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);

#define K_STD_BUF_SIZE 1024


typedef struct
{
	FILE	   *nFH;
	FILE	   *tarFH;
	FILE	   *tmpFH;
	char	   *targetFile;
	char		mode;
	pgoff_t		pos;
	pgoff_t		fileLen;
	ArchiveHandle *AH;
} TAR_MEMBER;

typedef struct
{
	int			hasSeek;
	pgoff_t		filePos;
	TAR_MEMBER *loToc;
	FILE	   *tarFH;
	pgoff_t		tarFHpos;
	pgoff_t		tarNextMember;
	TAR_MEMBER *FH;
	int			isSpecialScript;
	TAR_MEMBER *scriptTH;
} lclContext;

typedef struct
{
	TAR_MEMBER *TH;
	char	   *filename;
} lclTocEntry;

static void _LoadLOs(ArchiveHandle *AH);

static TAR_MEMBER *tarOpen(ArchiveHandle *AH, const char *filename, char mode);
static void tarClose(ArchiveHandle *AH, TAR_MEMBER *th);

#ifdef __NOT_USED__
static char *tarGets(char *buf, size_t len, TAR_MEMBER *th);
#endif
static int	tarPrintf(TAR_MEMBER *th, const char *fmt,...) pg_attribute_printf(2, 3);

static void _tarAddFile(ArchiveHandle *AH, TAR_MEMBER *th);
static TAR_MEMBER *_tarPositionTo(ArchiveHandle *AH, const char *filename);
static size_t tarRead(void *buf, size_t len, TAR_MEMBER *th);
static size_t tarWrite(const void *buf, size_t len, TAR_MEMBER *th);
static void _tarWriteHeader(TAR_MEMBER *th);
static int	_tarGetHeader(ArchiveHandle *AH, TAR_MEMBER *th);
static size_t _tarReadRaw(ArchiveHandle *AH, void *buf, size_t len, TAR_MEMBER *th, FILE *fh);

static size_t _scriptOut(ArchiveHandle *AH, const void *buf, size_t len);

/*
 *	初始化器
 */
void
InitArchiveFmt_Tar(ArchiveHandle *AH)
{
	lclContext *ctx;

	/* 假定为静态函数，可以针对每种格式复制。 */
	AH->ArchiveEntryPtr = _ArchiveEntry;
	AH->StartDataPtr = _StartData;
	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->ReadBytePtr = _ReadByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ReadBufPtr = _ReadBuf;
	AH->ClosePtr = _CloseArchive;
	AH->ReopenPtr = NULL;
	AH->PrintTocDataPtr = _PrintTocData;
	AH->ReadExtraTocPtr = _ReadExtraToc;
	AH->WriteExtraTocPtr = _WriteExtraToc;
	AH->PrintExtraTocPtr = _PrintExtraToc;

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;
	AH->ClonePtr = NULL;
	AH->DeClonePtr = NULL;

	AH->WorkerJobDumpPtr = NULL;
	AH->WorkerJobRestorePtr = NULL;

	/*
	 * 设置用于压缩数据的一些特殊上下文。
	 */
	ctx = pg_malloc0_object(lclContext);
	AH->formatData = (void *) ctx;
	ctx->filePos = 0;
	ctx->isSpecialScript = 0;

	/* 初始化 LO 缓冲 */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) pg_malloc(LOBBUFSIZE);

	/*
	 * 现在打开 tar 文件，如果是读模式则加载 TOC。
	 */
	if (AH->mode == archModeWrite)
	{
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			ctx->tarFH = fopen(AH->fSpec, PG_BINARY_W);
			if (ctx->tarFH == NULL)
				pg_fatal("could not open TOC file \"%s\" for output: %m",
						 AH->fSpec);
		}
		else
		{
			ctx->tarFH = stdout;
			if (ctx->tarFH == NULL)
				pg_fatal("could not open TOC file for output: %m");
		}

		ctx->tarFHpos = 0;

		/*
		 * 设为无缓冲，因为我们会 dup() 它，而缓冲区会相互干扰
		 *
		 */
		/* setvbuf(ctx->tarFH, NULL, _IONBF, 0); */

		ctx->hasSeek = checkSeek(ctx->tarFH);

		/*
		 * 我们不支持压缩，因为读取回文件是不可能的，
		 * 由于 gzdopen 使用缓冲 IO 会完全破坏文件定位。
		 *
		 */
		if (AH->compression_spec.algorithm != PG_COMPRESSION_NONE)
			pg_fatal("compression is not supported by tar archive format");
	}
	else
	{							/* 读模式 */
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			ctx->tarFH = fopen(AH->fSpec, PG_BINARY_R);
			if (ctx->tarFH == NULL)
				pg_fatal("could not open TOC file \"%s\" for input: %m",
						 AH->fSpec);
		}
		else
		{
			ctx->tarFH = stdin;
			if (ctx->tarFH == NULL)
				pg_fatal("could not open TOC file for input: %m");
		}

		/*
		 * 设为无缓冲，因为我们会 dup() 它，而缓冲区会相互干扰
		 *
		 */
		/* setvbuf(ctx->tarFH, NULL, _IONBF, 0); */

		ctx->tarFHpos = 0;

		ctx->hasSeek = checkSeek(ctx->tarFH);

		ctx->FH = (void *) tarOpen(AH, "toc.dat", 'r');
		ReadHead(AH);
		ReadToc(AH);
		tarClose(AH, ctx->FH);	/* 文件中没有其他内容了... */
	}
}

/*
 * - 开始一个新的 TOC 条目
 *	 设置输出文件名。
 */
static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx;
	char		fn[K_STD_BUF_SIZE];

	ctx = pg_malloc0_object(lclTocEntry);
	if (te->dataDumper != NULL)
	{
		snprintf(fn, sizeof(fn), "%d.dat", te->dumpId);
		ctx->filename = pg_strdup(fn);
	}
	else
	{
		ctx->filename = NULL;
		ctx->TH = NULL;
	}
	te->formatData = (void *) ctx;
}

static void
_WriteExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (ctx->filename)
		WriteStr(AH, ctx->filename);
	else
		WriteStr(AH, "");
}

static void
_ReadExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (ctx == NULL)
	{
		ctx = pg_malloc0_object(lclTocEntry);
		te->formatData = (void *) ctx;
	}

	ctx->filename = ReadStr(AH);
	if (strlen(ctx->filename) == 0)
	{
		free(ctx->filename);
		ctx->filename = NULL;
	}
	ctx->TH = NULL;
}

static void
_PrintExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (AH->public.verbose && ctx->filename != NULL)
		ahprintf(AH, "-- File: %s\n", ctx->filename);
}

static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	tctx->TH = tarOpen(AH, tctx->filename, 'w');
}

static TAR_MEMBER *
tarOpen(ArchiveHandle *AH, const char *filename, char mode)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	TAR_MEMBER *tm;

	if (mode == 'r')
	{
		tm = _tarPositionTo(AH, filename);
		if (!tm)				/* 未找到 */
		{
			if (filename)
			{
				/*
				 * 找不到请求的文件。将来：执行 SEEK(0) 并重试。
				 *
				 */
				pg_fatal("could not find file \"%s\" in archive", filename);
			}
			else
			{
				/* 任何文件都可以，没有剩余了，因此返回 NULL */
				return NULL;
			}
		}

		if (AH->compression_spec.algorithm == PG_COMPRESSION_NONE)
			tm->nFH = ctx->tarFH;
		else
			pg_fatal("compression is not supported by tar archive format");
	}
	else
	{
		int			old_umask;

		tm = pg_malloc0_object(TAR_MEMBER);

		/*
		 * POSIX 不要求但允许 tmpfile() 限制文件权限。
		 * 假定写入数据后发生操作系统崩溃，
		 * 文件系统可能保留数据但忘记 tmpfile() 的 unlink()。
		 * 如果是这样，文件模式可保护所写数据的机密性。
		 */
		old_umask = umask(S_IRWXG | S_IRWXO);

#ifndef WIN32
		tm->tmpFH = tmpfile();
#else

		/*
		 * 在 WIN32 上，tmpfile() 在根目录生成文件名，
		 * 在某些系统上需要管理员权限。循环
		 * 直到找到一个可以创建的唯一文件名。
		 */
		while (1)
		{
			char	   *name;
			int			fd;

			name = _tempnam(NULL, "pg_temp_");
			if (name == NULL)
				break;
			fd = open(name, O_RDWR | O_CREAT | O_EXCL | O_BINARY |
					  O_TEMPORARY, S_IRUSR | S_IWUSR);
			free(name);

			if (fd != -1)		/* 创建了文件 */
			{
				tm->tmpFH = fdopen(fd, "w+b");
				break;
			}
			else if (errno != EEXIST)	/* 不是文件已存在的错误 */
				break;
		}
#endif

		if (tm->tmpFH == NULL)
			pg_fatal("could not generate temporary file name: %m");

		umask(old_umask);

		if (AH->compression_spec.algorithm == PG_COMPRESSION_NONE)
			tm->nFH = tm->tmpFH;
		else
			pg_fatal("compression is not supported by tar archive format");

		tm->AH = AH;
		tm->targetFile = pg_strdup(filename);
	}

	tm->mode = mode;
	tm->tarFH = ctx->tarFH;

	return tm;
}

static void
tarClose(ArchiveHandle *AH, TAR_MEMBER *th)
{
	if (AH->compression_spec.algorithm != PG_COMPRESSION_NONE)
		pg_fatal("compression is not supported by tar archive format");

	if (th->mode == 'w')
		_tarAddFile(AH, th);	/* 这将关闭临时文件 */

	/*
	 * 否则，对于正常读取无需做任何事，
	 * 因为我们不 dup() 正常文件句柄，也不使用临时文件。
	 */

	free(th->targetFile);

	th->nFH = NULL;
}

#ifdef __NOT_USED__
static char *
tarGets(char *buf, size_t len, TAR_MEMBER *th)
{
	char	   *s;
	size_t		cnt = 0;
	char		c = ' ';
	int			eof = 0;

	/* 不能读取超过逻辑 EOF 的内容 */
	if (len > (th->fileLen - th->pos))
		len = th->fileLen - th->pos;

	while (cnt < len && c != '\n')
	{
		if (_tarReadRaw(th->AH, &c, 1, th, NULL) <= 0)
		{
			eof = 1;
			break;
		}
		buf[cnt++] = c;
	}

	if (eof && cnt == 0)
		s = NULL;
	else
	{
		buf[cnt++] = '\0';
		s = buf;
	}

	if (s)
	{
		len = strlen(s);
		th->pos += len;
	}

	return s;
}
#endif

/*
 * 从归档中读取字节。这是用于 tar 文件上所有读取的
 * 低级读取例程。
 */
static size_t
_tarReadRaw(ArchiveHandle *AH, void *buf, size_t len, TAR_MEMBER *th, FILE *fh)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		avail;
	size_t		used = 0;
	size_t		res = 0;

	Assert(th || fh);

	avail = AH->lookaheadLen - AH->lookaheadPos;
	if (avail > 0)
	{
		/* 我们有一些预读字节可用 */
		if (avail >= len)		/* 直接使用预读缓冲区 */
			used = len;
		else
			used = avail;

		/* 复制并调整缓冲区位置 */
		memcpy(buf, AH->lookahead + AH->lookaheadPos, used);
		AH->lookaheadPos += used;

		/* 调整所需长度 */
		len -= used;
	}

	/* 如果 len > 0，读取文件 */
	if (len > 0)
	{
		if (fh)
		{
			res = fread(&((char *) buf)[used], 1, len, fh);
			if (res != len && !feof(fh))
				READ_ERROR_EXIT(fh);
		}
		else if (th)
		{
			res = fread(&((char *) buf)[used], 1, len, th->nFH);
			if (res != len && !feof(th->nFH))
				READ_ERROR_EXIT(th->nFH);
		}
	}

	ctx->tarFHpos += res + used;

	return (res + used);
}

static size_t
tarRead(void *buf, size_t len, TAR_MEMBER *th)
{
	size_t		res;

	if (th->pos + len > th->fileLen)
		len = th->fileLen - th->pos;

	if (len <= 0)
		return 0;

	res = _tarReadRaw(th->AH, buf, len, th, NULL);

	th->pos += res;

	return res;
}

static size_t
tarWrite(const void *buf, size_t len, TAR_MEMBER *th)
{
	size_t		res;

	res = fwrite(buf, 1, len, th->nFH);

	th->pos += res;
	return res;
}

static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclTocEntry *tctx = (lclTocEntry *) AH->currToc->formatData;

	if (tarWrite(data, dLen, tctx->TH) != dLen)
		WRITE_ERROR_EXIT;
}

static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	/* 关闭文件 */
	tarClose(AH, tctx->TH);
	tctx->TH = NULL;
}

/*
 * 为给定文件打印数据
 */
static void
_PrintFileData(ArchiveHandle *AH, char *filename)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		buf[4096];
	size_t		cnt;
	TAR_MEMBER *th;

	if (!filename)
		return;

	th = tarOpen(AH, filename, 'r');
	ctx->FH = th;

	while ((cnt = tarRead(buf, 4095, th)) > 0)
	{
		buf[cnt] = '\0';
		ahwrite(buf, 1, cnt, AH);
	}

	tarClose(AH, th);
}


/*
 * 为给定 TOC 条目打印数据
*/
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	int			pos1;

	if (!tctx->filename)
		return;

	/*
	 * 如果我们正在写入特殊的 restore.sql 脚本，发出一条适当的
	 * 命令以包含来自相应文件的每个表的数据。
	 *
	 * 在 COPY 情况下这有些取巧，因为常规的 COPY 命令
	 * 在我们获得控制权之前就已经打印了。
	 */
	if (ctx->isSpecialScript)
	{
		if (te->copyStmt)
		{
			/* 中止 COPY FROM stdin */
			ahprintf(AH, "\\.\n");

			/*
			 * COPY 语句应看起来像 "COPY ... FROM stdin;\n"，
			 * 参见 dumpTableData()。
			 */
			pos1 = (int) strlen(te->copyStmt) - 13;
			if (pos1 < 6 || strncmp(te->copyStmt, "COPY ", 5) != 0 ||
				strcmp(te->copyStmt + pos1, " FROM stdin;\n") != 0)
				pg_fatal("unexpected COPY statement syntax: \"%s\"",
						 te->copyStmt);

			/* 发出除 FROM 部分之外的所有内容... */
			ahwrite(te->copyStmt, 1, pos1, AH);
			/* ...并插入修改后的 FROM */
			ahprintf(AH, " FROM '$$PATH$$/%s';\n\n", tctx->filename);
		}
		else
		{
			/* --inserts 模式，无需担心，只需包含数据文件 */
			ahprintf(AH, "\\i $$PATH$$/%s\n\n", tctx->filename);
		}

		return;
	}

	if (strcmp(te->desc, "BLOBS") == 0)
		_LoadLOs(AH);
	else
		_PrintFileData(AH, tctx->filename);
}

static void
_LoadLOs(ArchiveHandle *AH)
{
	Oid			oid;
	lclContext *ctx = (lclContext *) AH->formatData;
	TAR_MEMBER *th;
	size_t		cnt;
	bool		foundLO = false;
	char		buf[4096];

	StartRestoreLOs(AH);

	th = tarOpen(AH, NULL, 'r');	/* 打开下一个文件 */
	while (th != NULL)
	{
		ctx->FH = th;

		if (strncmp(th->targetFile, "blob_", 5) == 0)
		{
			oid = atooid(&th->targetFile[5]);
			if (oid != 0)
			{
				pg_log_info("restoring large object with OID %u", oid);

				StartRestoreLO(AH, oid, AH->public.ropt->dropSchema);

				while ((cnt = tarRead(buf, 4095, th)) > 0)
				{
					buf[cnt] = '\0';
					ahwrite(buf, 1, cnt, AH);
				}
				EndRestoreLO(AH, oid);
				foundLO = true;
			}
			tarClose(AH, th);
		}
		else
		{
			tarClose(AH, th);

			/*
			 * 一旦找到第一个 LO，就在第一个非 LO 条目处停止
			 * （这将是 'blobs.toc'）。如果没有 LO，
			 * 此代码会耗尽归档的其余部分...
			 * 但此函数在那情况下根本不应被调用。
			 */
			if (foundLO)
				break;
		}

		th = tarOpen(AH, NULL, 'r');
	}
	EndRestoreLOs(AH);
}


static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		b = i;			/* 避免字节序问题 */

	if (tarWrite(&b, 1, ctx->FH) != 1)
		WRITE_ERROR_EXIT;

	ctx->filePos += 1;
	return 1;
}

static int
_ReadByte(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		res;
	unsigned char c;

	res = tarRead(&c, 1, ctx->FH);
	if (res != 1)
		/* 读取错误时我们已经退出，此处一定是 EOF */
		pg_fatal("could not read from input file: end of file");
	ctx->filePos += 1;
	return c;
}

static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (tarWrite(buf, len, ctx->FH) != len)
		WRITE_ERROR_EXIT;

	ctx->filePos += len;
}

static void
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (tarRead(buf, len, ctx->FH) != len)
		/* 读取错误时我们已经退出，此处一定是 EOF */
		pg_fatal("could not read from input file: end of file");

	ctx->filePos += len;
}

static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	TAR_MEMBER *th;
	RestoreOptions *ropt;
	RestoreOptions *savRopt;
	DumpOptions *savDopt;
	int			savVerbose,
				i;

	if (AH->mode == archModeWrite)
	{
		/*
		 * 先将文件头和 TOC 写入归档
		 */
		th = tarOpen(AH, "toc.dat", 'w');
		ctx->FH = th;
		WriteHead(AH);
		WriteToc(AH);
		tarClose(AH, th);		/* 不再需要 */

		/*
		 * 现在发送数据（表和 LOs）
		 */
		WriteDataChunks(AH, NULL);

		/*
		 * 现在此格式想要追加一个脚本，该脚本在文件
		 * 被提取后能够执行完整恢复。
		 */
		th = tarOpen(AH, "restore.sql", 'w');

		tarPrintf(th, "--\n"
				  "-- NOTE:\n"
				  "--\n"
				  "-- File paths need to be edited. Search for $$PATH$$ and\n"
				  "-- replace it with the path to the directory containing\n"
				  "-- the extracted data files.\n"
				  "--\n");

		AH->CustomOutPtr = _scriptOut;

		ctx->isSpecialScript = 1;
		ctx->scriptTH = th;

		ropt = NewRestoreOptions();
		memcpy(ropt, AH->public.ropt, sizeof(RestoreOptions));
		ropt->filename = NULL;
		ropt->dropSchema = 1;
		ropt->superuser = NULL;
		ropt->suppressDumpWarnings = true;

		savDopt = AH->public.dopt;
		savRopt = AH->public.ropt;

		SetArchiveOptions((Archive *) AH, NULL, ropt);

		savVerbose = AH->public.verbose;
		AH->public.verbose = 0;

		RestoreArchive((Archive *) AH);

		SetArchiveOptions((Archive *) AH, savDopt, savRopt);

		AH->public.verbose = savVerbose;

		tarClose(AH, th);

		ctx->isSpecialScript = 0;

		/*
		 * tar 文件的 EOF 标记是两个 NULL 块。
		 */
		for (i = 0; i < TAR_BLOCK_SIZE * 2; i++)
		{
			if (fputc(0, ctx->tarFH) == EOF)
				WRITE_ERROR_EXIT;
		}

		/* 如果定义了输出文件则同步 */
		if (AH->dosync && AH->fSpec)
			(void) fsync_fname(AH->fSpec, false);
	}

	AH->FH = NULL;
}

static size_t
_scriptOut(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	return tarWrite(buf, len, ctx->scriptTH);
}

/*
 * 大对象支持
 */

/*
 * 归档器开始保存所有 BLOB 数据（非模式）时调用。      
 * 此例程应保存将 LOs 读回内存所需的格式特定信息。   
 *                                  
 *
 * 它恰好在导出器的 DataDumper 例程之前调用。  
 *
 * 可选，但强烈建议实现。  
 *
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[K_STD_BUF_SIZE];

	sprintf(fname, "blobs.toc");
	ctx->loToc = tarOpen(AH, fname, 'w');
}

/*
 * 当导出器调用 StartLO 时由归档器调用。
 *
 * 必需。
 *
 * 必须保存传入的 OID 以便在恢复时检索。
 */
static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	char		fname[255];

	if (oid == 0)
		pg_fatal("invalid OID for large object (%u)", oid);

	if (AH->compression_spec.algorithm != PG_COMPRESSION_NONE)
		pg_fatal("compression is not supported by tar archive format");

	sprintf(fname, "blob_%u.dat", oid);

	tarPrintf(ctx->loToc, "%u %s\n", oid, fname);

	tctx->TH = tarOpen(AH, fname, 'w');
}

/*
 * 当导出器调用 EndLO 时由归档器调用。
 *
 * 可选。
 *
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	tarClose(AH, tctx->TH);
}

/*
 * 归档器完成保存所有 BLOB 数据时调用。
 *
 * 可选。
 *
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	/* 写入一个伪造的零 OID 以标记 LOs 结束。 */
	/* WriteInt(AH, 0); */

	tarClose(AH, ctx->loToc);
}



/*------------
 * TAR 支持
 *------------
 */

static int
tarPrintf(TAR_MEMBER *th, const char *fmt,...)
{
	int			save_errno = errno;
	char	   *p;
	size_t		len = 128;		/* 初始缓冲区大小假设 */
	size_t		cnt;

	for (;;)
	{
		va_list		args;

		/* 分配工作缓冲区。 */
		p = (char *) pg_malloc(len);

		/* 尝试格式化数据。 */
		errno = save_errno;
		va_start(args, fmt);
		cnt = pvsnprintf(p, len, fmt, args);
		va_end(args);

		if (cnt < len)
			break;				/* 成功 */

		/* 释放缓冲区并循环重试更大的 len。 */
		free(p);
		len = cnt;
	}

	cnt = tarWrite(p, cnt, th);
	free(p);
	return (int) cnt;
}

bool
isValidTarHeader(char *header)
{
	int			sum;
	int			chk = tarChecksum(header);

	sum = read_tar_number(&header[148], 8);

	if (sum != chk)
		return false;

	/* POSIX tar 格式 */
	if (memcmp(&header[257], "ustar\0", 6) == 0 &&
		memcmp(&header[263], "00", 2) == 0)
		return true;
	/* GNU tar 格式 */
	if (memcmp(&header[257], "ustar  \0", 8) == 0)
		return true;
	/* 由 pre-9.3 pg_dump 写入的非完全 POSIX 格式 */
	if (memcmp(&header[257], "ustar00\0", 8) == 0)
		return true;

	return false;
}

/* 给定成员，写入 TAR 文件头并复制文件 */
static void
_tarAddFile(ArchiveHandle *AH, TAR_MEMBER *th)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	FILE	   *tmp = th->tmpFH;	/* 方便使用的引用 */
	char		buf[32768];
	size_t		cnt;
	pgoff_t		len = 0;
	size_t		res;
	size_t		i,
				pad;

	/*
	 * 查找文件长度并回到开始位置。
	 */
	if (fseeko(tmp, 0, SEEK_END) != 0)
		pg_fatal("error during file seek: %m");
	th->fileLen = ftello(tmp);
	if (th->fileLen < 0)
		pg_fatal("could not determine seek position in archive file: %m");
	if (fseeko(tmp, 0, SEEK_SET) != 0)
		pg_fatal("error during file seek: %m");

	_tarWriteHeader(th);

	while ((cnt = fread(buf, 1, sizeof(buf), tmp)) > 0)
	{
		if ((res = fwrite(buf, 1, cnt, th->tarFH)) != cnt)
			WRITE_ERROR_EXIT;
		len += res;
	}
	if (!feof(tmp))
		READ_ERROR_EXIT(tmp);

	if (fclose(tmp) != 0)		/* 这*应该*会删除它... */
		pg_fatal("could not close temporary file: %m");

	if (len != th->fileLen)
		pg_fatal("actual file length (%lld) does not match expected (%lld)",
				 (long long) len, (long long) th->fileLen);

	pad = tarPaddingBytesRequired(len);
	for (i = 0; i < pad; i++)
	{
		if (fputc('\0', th->tarFH) == EOF)
			WRITE_ERROR_EXIT;
	}

	ctx->tarFHpos += len + pad;
}

/* 在归档中定位文件，读取文件头并定位到数据 */
static TAR_MEMBER *
_tarPositionTo(ArchiveHandle *AH, const char *filename)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	TAR_MEMBER *th = pg_malloc0_object(TAR_MEMBER);
	char		c;
	char		header[TAR_BLOCK_SIZE];
	size_t		i,
				len,
				blks;
	int			id;

	th->AH = AH;

	/* 如果有当前文件，跳转到其末尾 */
	if (ctx->tarFHpos != 0)
	{
		pg_log_debug("moving from position %lld to next member at file position %lld",
					 (long long) ctx->tarFHpos, (long long) ctx->tarNextMember);

		while (ctx->tarFHpos < ctx->tarNextMember)
			_tarReadRaw(AH, &c, 1, NULL, ctx->tarFH);
	}

	pg_log_debug("now at file position %lld", (long long) ctx->tarFHpos);

	/* 我们位于文件开头，或下一个成员处 */

	/* 获取文件头 */
	if (!_tarGetHeader(AH, th))
	{
		if (filename)
			pg_fatal("could not find header for file \"%s\" in tar archive", filename);
		else
		{
			/*
			 * 我们只是在扫描归档寻找下一个文件，
			 * 因此返回 null
			 */
			free(th);
			return NULL;
		}
	}

	while (filename != NULL && strcmp(th->targetFile, filename) != 0)
	{
		pg_log_debug("skipping tar member %s", th->targetFile);

		id = atoi(th->targetFile);
		if ((TocIDRequired(AH, id) & REQ_DATA) != 0)
			pg_fatal("restoring data out of order is not supported in this archive format: "
					 "\"%s\" is required, but comes before \"%s\" in the archive file.",
					 th->targetFile, filename);

		/* 文件头不匹配，因此读取到下一个文件头 */
		len = th->fileLen;
		len += tarPaddingBytesRequired(th->fileLen);
		blks = len / TAR_BLOCK_SIZE;	/* tar 块数 */

		for (i = 0; i < blks; i++)
			_tarReadRaw(AH, &header[0], TAR_BLOCK_SIZE, NULL, ctx->tarFH);

		if (!_tarGetHeader(AH, th))
			pg_fatal("could not find header for file \"%s\" in tar archive", filename);
	}

	ctx->tarNextMember = ctx->tarFHpos + th->fileLen
		+ tarPaddingBytesRequired(th->fileLen);
	th->pos = 0;

	return th;
}

/* 读取并验证文件头 */
static int
_tarGetHeader(ArchiveHandle *AH, TAR_MEMBER *th)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		h[TAR_BLOCK_SIZE];
	char		tag[100 + 1];
	int			sum,
				chk;
	pgoff_t		len;
	pgoff_t		hPos;
	bool		gotBlock = false;

	while (!gotBlock)
	{
		/* 保存位置以供报告 */
		hPos = ctx->tarFHpos;

		/* 读取下一个 tar 块，返回 EOF，数据不足则退出 */
		len = _tarReadRaw(AH, h, TAR_BLOCK_SIZE, NULL, ctx->tarFH);
		if (len == 0)			/* EOF */
			return 0;

		if (len != TAR_BLOCK_SIZE)
			pg_fatal(ngettext("incomplete tar header found (%lu byte)",
							  "incomplete tar header found (%lu bytes)",
							  len),
					 (unsigned long) len);

		/* 计算校验和 */
		chk = tarChecksum(h);
		sum = read_tar_number(&h[148], 8);

		/*
		 * 如果校验和失败，检查是否为空块。如果是，
		 * 静默继续到下一个块。
		 */
		if (chk == sum)
			gotBlock = true;
		else
		{
			int			i;

			for (i = 0; i < TAR_BLOCK_SIZE; i++)
			{
				if (h[i] != 0)
				{
					gotBlock = true;
					break;
				}
			}
		}
	}

	/* 名称字段为 100 字节，可能不是 null 结尾的 */
	strlcpy(tag, &h[0], 100 + 1);

	len = read_tar_number(&h[124], 12);

	pg_log_debug("TOC Entry %s at %llu (length %llu, checksum %d)",
				 tag, (unsigned long long) hPos, (unsigned long long) len, sum);

	if (chk != sum)
		pg_fatal("corrupt tar header found in %s (expected %d, computed %d) file position %llu",
				 tag, sum, chk, (unsigned long long) ftello(ctx->tarFH));

	th->targetFile = pg_strdup(tag);
	th->fileLen = len;

	return 1;
}


static void
_tarWriteHeader(TAR_MEMBER *th)
{
	char		h[TAR_BLOCK_SIZE];

	tarCreateHeader(h, th->targetFile, NULL, th->fileLen,
					0600, 04000, 02000, time(NULL));

	/* 现在写入完成的文件头。 */
	if (fwrite(h, 1, TAR_BLOCK_SIZE, th->tarFH) != TAR_BLOCK_SIZE)
		WRITE_ERROR_EXIT;
}
