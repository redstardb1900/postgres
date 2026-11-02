/*-------------------------------------------------------------------------
 *
 * pg_backup_custom.c
 *
 *	Implements the custom output format.
 *
 *	The comments with the routines in this code are a good place to
 *	understand how to write a new format.
 *
 *	See the headers to pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	and any liability will be limited to the time taken to fix any
 *	related bug.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_custom.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/file_utils.h"
#include "compress_io.h"
#include "parallel.h"
#include "pg_backup_utils.h"

/*--------
 * Routines in the format interface
 *--------
 */

static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
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

static void _PrintData(ArchiveHandle *AH);
static void _skipData(ArchiveHandle *AH);
static void _skipLOs(ArchiveHandle *AH);

static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);
static void _LoadLOs(ArchiveHandle *AH, bool drop);

static void _PrepParallelRestore(ArchiveHandle *AH);
static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);

static int	_WorkerJobRestoreCustom(ArchiveHandle *AH, TocEntry *te);

typedef struct
{
	CompressorState *cs;
	int			hasSeek;
	/* lastFilePos 仅在读取时使用，如果 !hasSeek 可能无效 */
	pgoff_t		lastFilePos;	/* 上一个已读数据块之后的位置 */
} lclContext;

typedef struct
{
	int			dataState;
	pgoff_t		dataPos;		/* 仅在 dataState=K_OFFSET_POS_SET 时有效 */
} lclTocEntry;


/*------
 * Static declarations
 *------
 */
static void _readBlockHeader(ArchiveHandle *AH, int *type, int *id);
static pgoff_t _getFilePos(ArchiveHandle *AH, lclContext *ctx);

static void _CustomWriteFunc(ArchiveHandle *AH, const char *buf, size_t len);
static size_t _CustomReadFunc(ArchiveHandle *AH, char **buf, size_t *buflen);


/*
 *	所有格式必需执行的初始化例程。这是一个全局例程，
 *	应在 pg_backup_archiver.h 中声明。
 *
 *	其任务是创建任意的额外归档上下文（使用 AH->formatData），
 *	并初始化支持的函数指针。
 *
 *	它还应为其输入源做好读/写准备，
 *	在读模式连接的情况下，应加载文件头和 TOC。
 */
void
InitArchiveFmt_Custom(ArchiveHandle *AH)
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

	/* 自定义归档不支持并行导出，仅支持并行恢复 */
	AH->WorkerJobDumpPtr = NULL;
	AH->WorkerJobRestorePtr = _WorkerJobRestoreCustom;

	/* 设置私有区域。 */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = (void *) ctx;

	/* 初始化 LO 缓冲 */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) pg_malloc(LOBBUFSIZE);

	/*
	 * 现在打开文件
	 */
	if (AH->mode == archModeWrite)
	{
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			AH->FH = fopen(AH->fSpec, PG_BINARY_W);
			if (!AH->FH)
				pg_fatal("could not open output file \"%s\": %m", AH->fSpec);
		}
		else
		{
			AH->FH = stdout;
			if (!AH->FH)
				pg_fatal("could not open output file: %m");
		}

		ctx->hasSeek = checkSeek(AH->FH);
	}
	else
	{
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			AH->FH = fopen(AH->fSpec, PG_BINARY_R);
			if (!AH->FH)
				pg_fatal("could not open input file \"%s\": %m", AH->fSpec);
		}
		else
		{
			AH->FH = stdin;
			if (!AH->FH)
				pg_fatal("could not open input file: %m");
		}

		ctx->hasSeek = checkSeek(AH->FH);

		ReadHead(AH);
		ReadToc(AH);

		/*
		 * 记住第一个数据块的位置（即 TOC 之后的点），
		 * 以备我们需要搜索所需的数据块。
		 */
		ctx->lastFilePos = _getFilePos(AH, ctx);
	}
}

/*
 * 当导出器创建新的 TOC 条目时由归档器调用。
 *
 * 可选。
 *
 * 设置导出格式相关的 TOC 数据。
*/
static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx;

	ctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
	if (te->dataDumper)
		ctx->dataState = K_OFFSET_POS_NOT_SET;
	else
		ctx->dataState = K_OFFSET_NO_DATA;

	te->formatData = (void *) ctx;
}

/*
 * 由归档器调用以保存任意额外的格式相关 TOC 条目数据。
 *
 * 可选。
 *
 * 使用归档器例程来写入数据——它们是与字节序无关的，
 * 并能维护其他重要的文件信息。
 *
 */
static void
_WriteExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	WriteOffset(AH, ctx->dataPos, ctx->dataState);
}

/*
 * 由归档器调用以读取任意额外的格式相关 TOC 数据。
 *
 * 可选。
 *
 * 需要与 _WriteExtraToc 中定义的顺序匹配，
 * 并且也应使用归档器输入例程。
 */
static void
_ReadExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (ctx == NULL)
	{
		ctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
		te->formatData = (void *) ctx;
	}

	ctx->dataState = ReadOffset(AH, &(ctx->dataPos));

	/*
	 * 在 V1.7 (pg7.3) 之前，我们将数据大小导出为 int，
	 * 现在已不再导出。
	 */
	if (AH->version < K_VERS_1_7)
		ReadInt(AH);
}

/*
 * 由归档器在恢复归档时调用，以输出包含
 * TOC 条目有用信息的注释。
 *
 * 可选。
 */
static void
_PrintExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (AH->public.verbose)
		ahprintf(AH, "-- Data Pos: " INT64_FORMAT "\n",
				 (int64) ctx->dataPos);
}

/*
 * 归档器保存表数据（非模式）时调用。此例程应保存
 * 读取归档所需的所有格式特定信息。
 *
 * 在导出器的 'DataDumper' 例程被调用之前调用。
 *
 * 可选，但强烈建议实现。
 *
 *
 */
static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	tctx->dataPos = _getFilePos(AH, ctx);
	if (tctx->dataPos >= 0)
		tctx->dataState = K_OFFSET_POS_SET;

	_WriteByte(AH, BLK_DATA);	/* 块类型 */
	WriteInt(AH, te->dumpId);	/* 用于完整性检查 */

	ctx->cs = AllocateCompressor(AH->compression_spec,
								 NULL,
								 _CustomWriteFunc);
}

/*
 * 当导出器调用 WriteData 时由归档器调用。
 * 此例程针对 LO 和表数据都会被调用；
 * 格式负责使用 StartLO/StartData 管理各种数据。
 *
 * 只应在 DataDumper 例程内调用。
 *
 * 必需。
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressorState *cs = ctx->cs;

	if (dLen > 0)
		/* writeData() 内部抛出写入错误 */
		cs->writeData(AH, cs, data, dLen);
}

/*
 * 当导出器的 'DataDumper' 例程完成时由归档器调用。
 *
 * 必需。
 *
 */
static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	EndCompressor(AH, ctx->cs);
	ctx->cs = NULL;

	/* 发送结束标记 */
	WriteInt(AH, 0);
}

/*
 * 归档器开始保存所有 BLOB 数据（非模式）时调用。
 * 此例程应保存将 LOs 读回内存所需的所有格式特定信息。
 *
 * 在导出器的 DataDumper 例程之前调用。
 *
 * 可选，但强烈建议实现。
 *
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	tctx->dataPos = _getFilePos(AH, ctx);
	if (tctx->dataPos >= 0)
		tctx->dataState = K_OFFSET_POS_SET;

	_WriteByte(AH, BLK_BLOBS);	/* 块类型 */
	WriteInt(AH, te->dumpId);	/* 用于完整性检查 */
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

	if (oid == 0)
		pg_fatal("invalid OID for large object");

	WriteInt(AH, oid);

	ctx->cs = AllocateCompressor(AH->compression_spec,
								 NULL,
								 _CustomWriteFunc);
}

/*
 * 当导出器调用 EndLO 时由归档器调用。
 *
 * 可选。
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	EndCompressor(AH, ctx->cs);
	/* 发送结束标记 */
	WriteInt(AH, 0);
}

/*
 * 归档器完成保存所有 BLOB 数据时调用。
 *
 * 可选。
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	/* 写入一个伪造的零 OID 以标记 LOs 结束。 */
	WriteInt(AH, 0);
}

/*
 * 为给定的 TOC 条目打印数据
 */
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	int			blkType;
	int			id;

	if (tctx->dataState == K_OFFSET_NO_DATA)
		return;

	if (!ctx->hasSeek || tctx->dataState == K_OFFSET_POS_NOT_SET)
	{
		/*
		 * 我们无法直接寻道到所需的数据块。作为替代，
		 * 跳过块头直到找到我们想要的那个。记住被跳过块的位置，
		 * 这样如果我们后来决定需要读取某个块时，可以寻道到它。
		 *
		 * 当输入文件可寻道时，我们可以从上一个数据块
		 * 在之前迭代中扫描到的位置开始搜索。
		 *
		 *
		 */
		if (ctx->hasSeek)
		{
			if (fseeko(AH->FH, ctx->lastFilePos, SEEK_SET) != 0)
				pg_fatal("error during file seek: %m");
		}

		for (;;)
		{
			pgoff_t		thisBlkPos = _getFilePos(AH, ctx);

			_readBlockHeader(AH, &blkType, &id);

			if (blkType == EOF || id == te->dumpId)
				break;

			/* 记住块位置（如果获取到了） */
			if (thisBlkPos >= 0)
			{
				TocEntry   *otherte = getTocEntryByDumpId(AH, id);

				if (otherte && otherte->formatData)
				{
					lclTocEntry *othertctx = (lclTocEntry *) otherte->formatData;

					/*
					 * 注意：在 Windows 上，多个线程可能并发访问/更新
					 * 同一个 lclTocEntry，但只要我们在 dataState 之前
					 * 更新 dataPos，这应该是安全的。
					 * 理想情况下，我们会使用 pg_write_barrier() 来强制执行，
					 * 但前端代码中不存在所需的基础设施。
					 * 不过 Windows 仅在强存储序的机器上运行，所以目前应该没问题。
					 *
					 */
					if (othertctx->dataState == K_OFFSET_POS_NOT_SET)
					{
						othertctx->dataPos = thisBlkPos;
						othertctx->dataState = K_OFFSET_POS_SET;
					}
					else if (othertctx->dataPos != thisBlkPos ||
							 othertctx->dataState != K_OFFSET_POS_SET)
					{
						/* 完整性检查 */
						pg_log_warning("data block %d has wrong seek position",
									   id);
					}
				}
			}

			switch (blkType)
			{
				case BLK_DATA:
					_skipData(AH);
					break;

				case BLK_BLOBS:
					_skipLOs(AH);
					break;

				default:		/* 始终保留 default 分支 */
					pg_fatal("unrecognized data block type (%d) while searching archive",
							 blkType);
					break;
			}
		}
	}
	else
	{
		/* 我们可以直接寻道到所需位置。 */
		if (fseeko(AH->FH, tctx->dataPos, SEEK_SET) != 0)
			pg_fatal("error during file seek: %m");

		_readBlockHeader(AH, &blkType, &id);
	}

	/*
	 * 如果到达 EOF 而没有找到我们想要的块，那么
	 * 要么它不存在，要么它存在但我们无力寻道回去。
	 */
	if (blkType == EOF)
	{
		if (!ctx->hasSeek)
			pg_fatal("could not find block ID %d in archive -- "
					 "possibly due to out-of-order restore request, "
					 "which cannot be handled due to non-seekable input file",
					 te->dumpId);
		else
			pg_fatal("could not find block ID %d in archive -- "
					 "possibly corrupt archive",
					 te->dumpId);
	}

	/* 数据是否完好？ */
	if (id != te->dumpId)
		pg_fatal("found unexpected block ID (%d) when reading data -- expected %d",
				 id, te->dumpId);

	switch (blkType)
	{
		case BLK_DATA:
			_PrintData(AH);
			break;

		case BLK_BLOBS:
			_LoadLOs(AH, AH->public.ropt->dropSchema);
			break;

		default:				/* 始终保留 default 分支 */
			pg_fatal("unrecognized data block type %d while restoring archive",
					 blkType);
			break;
	}

	/*
	 * 如果输入文件可寻道但缺少数据偏移量，
	 * 更新我们关于未来搜索起始位置的知识。
	 * （注意我们没有更新当前 TE 的 dataState/dataPos。
	 * 我们可以但没必要，因为它不会再被访问。）
	 */
	if (ctx->hasSeek && tctx->dataState == K_OFFSET_POS_NOT_SET)
	{
		pgoff_t		curPos = _getFilePos(AH, ctx);

		if (curPos > ctx->lastFilePos)
			ctx->lastFilePos = curPos;
	}
}

/*
 * 从当前文件位置打印数据。
*/
static void
_PrintData(ArchiveHandle *AH)
{
	CompressorState *cs;

	cs = AllocateCompressor(AH->compression_spec,
							_CustomReadFunc, NULL);
	cs->readData(AH, cs);
	EndCompressor(AH, cs);
}

static void
_LoadLOs(ArchiveHandle *AH, bool drop)
{
	Oid			oid;

	StartRestoreLOs(AH);

	oid = ReadInt(AH);
	while (oid != 0)
	{
		StartRestoreLO(AH, oid, drop);
		_PrintData(AH);
		EndRestoreLO(AH, oid);
		oid = ReadInt(AH);
	}

	EndRestoreLOs(AH);
}

/*
 * 从当前文件位置跳过 LOs。
 * LOs 以数据块的形式顺序写入（见下文）。
 * 每个 LO 之前是其原始 OID。
 * 零 OID 表示 LOs 结束。
 */
static void
_skipLOs(ArchiveHandle *AH)
{
	Oid			oid;

	oid = ReadInt(AH);
	while (oid != 0)
	{
		_skipData(AH);
		oid = ReadInt(AH);
	}
}

/*
 * 从当前文件位置跳过数据。
 * 数据块格式为一个整数长度，后跟数据。
 * 零长度表示块结束。
*/
static void
_skipData(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		blkLen;
	char	   *buf = NULL;
	int			buflen = 0;

	blkLen = ReadInt(AH);
	while (blkLen != 0)
	{
		if (ctx->hasSeek)
		{
			if (fseeko(AH->FH, blkLen, SEEK_CUR) != 0)
				pg_fatal("error during file seek: %m");
		}
		else
		{
			if (blkLen > buflen)
			{
				free(buf);
				buf = (char *) pg_malloc(blkLen);
				buflen = blkLen;
			}
			if (fread(buf, 1, blkLen, AH->FH) != blkLen)
			{
				if (feof(AH->FH))
					pg_fatal("could not read from input file: end of file");
				else
					pg_fatal("could not read from input file: %m");
			}
		}

		blkLen = ReadInt(AH);
	}

	free(buf);
}

/*
 * 向归档写入一个字节数据。
 *
 * 必需。
 *
 * 由归档器调用，用于向归档输出整数和字节。
 */
static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	if (fputc(i, AH->FH) == EOF)
		WRITE_ERROR_EXIT;

	return 1;
}

/*
 * 从归档读取一个字节数据。
 *
 * 必需
 *
 * 由归档器调用，用于从归档读取字节和整数。
 * EOF 应被视为致命错误。
 */
static int
_ReadByte(ArchiveHandle *AH)
{
	int			res;

	res = getc(AH->FH);
	if (res == EOF)
		READ_ERROR_EXIT(AH->FH);
	return res;
}

/*
 * 向归档写入一个缓冲区的数据。
 *
 * 必需。
 *
 * 由归档器调用，用于向归档写入一个字节块。
 */
static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	if (fwrite(buf, 1, len, AH->FH) != len)
		WRITE_ERROR_EXIT;
}

/*
 * 从归档读取一个字节块。
 *
 * 必需。
 *
 * 由归档器调用，用于从归档读取一个字节块
 */
static void
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	if (fread(buf, 1, len, AH->FH) != len)
		READ_ERROR_EXIT(AH->FH);
}

/*
 * 关闭归档。
 *
 * 必需。
 *
 * 写入归档时，此例程实际启动将其保存到文件的过程。
 * 在此之前不应写入任何数据，因为用户可以在创建之后
 * 对 TOC 进行排序。
 *
 * 如果归档需要写入，此例程必须调用：
 *		WriteHead			以保存归档文件头
 *		WriteToc			以保存 TOC 条目
 *		WriteDataChunks		以保存所有数据和 LOs。
 *
 */
static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	pgoff_t		tpos;

	if (AH->mode == archModeWrite)
	{
		WriteHead(AH);
		/* 记住 TOC 的寻道位置以便后续使用 */
		tpos = ftello(AH->FH);
		if (tpos < 0 && ctx->hasSeek)
			pg_fatal("could not determine seek position in archive file: %m");
		WriteToc(AH);
		WriteDataChunks(AH, NULL);

		/*
		 * 如果可能的话，重新写入 TOC 以更新数据偏移量信息。
		 * 这不是必需的，因为 pg_restore 在大多数情况下都能应对；
		 * 但在某些情况下（特别是并行恢复），它可以显著加快 pg_restore 的速度。
		 *
		 */
		if (ctx->hasSeek &&
			fseeko(AH->FH, tpos, SEEK_SET) == 0)
			WriteToc(AH);
	}

	if (fclose(AH->FH) != 0)
		pg_fatal("could not close archive file: %m");

	/* 如果定义了输出文件则同步 */
	if (AH->dosync && AH->mode == archModeWrite && AH->fSpec)
		(void) fsync_fname(AH->fSpec, false);

	AH->FH = NULL;
}

/*
 * 重新打开归档的文件句柄。
 *
 * 我们关闭原始文件句柄，但在 Windows 上例外。
 * （不同之处在于，Windows 上这是在多线程上下文中使用的，
 * 我们不希望某个线程关闭父文件句柄。）
 */
static void
_ReopenArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	pgoff_t		tpos;

	if (AH->mode == archModeWrite)
		pg_fatal("can only reopen input archives");

	/*
	 * 以下两种情况是面向用户的错误，因为它们表示不支持
	 * （但并非无效）的用例。相应地措辞错误消息。
	 */
	if (AH->fSpec == NULL || strcmp(AH->fSpec, "") == 0)
		pg_fatal("parallel restore from standard input is not supported");
	if (!ctx->hasSeek)
		pg_fatal("parallel restore from non-seekable file is not supported");

	tpos = ftello(AH->FH);
	if (tpos < 0)
		pg_fatal("could not determine seek position in archive file: %m");

#ifndef WIN32
	if (fclose(AH->FH) != 0)
		pg_fatal("could not close archive file: %m");
#endif

	AH->FH = fopen(AH->fSpec, PG_BINARY_R);
	if (!AH->FH)
		pg_fatal("could not open input file \"%s\": %m", AH->fSpec);

	if (fseeko(AH->FH, tpos, SEEK_SET) != 0)
		pg_fatal("could not set seek position in archive file: %m");
}

/*
 * 为并行恢复做准备。
 *
 * 主要需要做的是为 TABLE DATA 和 BLOBS 的 TOC 条目
 * 填充 dataLength 字段，使用适当的值来指导恢复作业的排序。
 * 这些数据的来源取决于格式，值的具体含义也是如此。
 *
 * 格式模块也可以选择在此处进行其他设置。
 *
 */
static void
_PrepParallelRestore(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	TocEntry   *prev_te = NULL;
	lclTocEntry *prev_tctx = NULL;
	TocEntry   *te;

	/*
	 * 知道数据项是按照 TOC 顺序导出的，我们可以
	 * 将每个项的长度重建为与下一个数据项起始偏移量的差值。
	 *
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		lclTocEntry *tctx = (lclTocEntry *) te->formatData;

		/*
		 * 忽略没有已知数据偏移量的条目；如果在创建归档时
		 * 我们无法寻道重写 TOC，那么所有条目都将如此，
		 * 最终我们将没有任何大小估算值。
		 */
		if (tctx->dataState != K_OFFSET_POS_SET)
			continue;

		/* 计算上一个数据项的长度 */
		if (prev_te)
		{
			if (tctx->dataPos > prev_tctx->dataPos)
				prev_te->dataLength = tctx->dataPos - prev_tctx->dataPos;
		}

		prev_te = te;
		prev_tctx = tctx;
	}

	/* 如果可以寻道，我们可以确定最后一个项的长度 */
	if (prev_te && ctx->hasSeek)
	{
		pgoff_t		endpos;

		if (fseeko(AH->FH, 0, SEEK_END) != 0)
			pg_fatal("error during file seek: %m");
		endpos = ftello(AH->FH);
		if (endpos > prev_tctx->dataPos)
			prev_te->dataLength = endpos - prev_tctx->dataPos;
	}
}

/*
 * 并行恢复期间克隆格式特定的字段。
 */
static void
_Clone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	/*
	 * 每个线程必须有私有的 lclContext 工作状态。
	 */
	AH->formatData = (lclContext *) pg_malloc(sizeof(lclContext));
	memcpy(AH->formatData, ctx, sizeof(lclContext));
	ctx = (lclContext *) AH->formatData;

	/* 完整性检查，不应发生 */
	if (ctx->cs != NULL)
		pg_fatal("compressor active");

	/*
	 * 我们有意不克隆 TOC 条目本地状态：
	 * 跨线程共享数据块位置信息是有用的。
	 * 但 _PrintTocData 必须注意该状态上的操作顺序。
	 *
	 * 注意：我们不创建本地 lo_buf，
	 * 因为每个归档最多一个 BLOBS 条目，
	 * 因此不可能并行。
	 */
}

static void
_DeClone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	free(ctx);
}

/*
 * 此函数在并行恢复自定义格式归档的子进程中执行，
 * 为一个 TOC 条目恢复实际数据。
 */
static int
_WorkerJobRestoreCustom(ArchiveHandle *AH, TocEntry *te)
{
	return parallel_restore(AH, te);
}

/*--------------------------------------------------
 * END OF FORMAT CALLBACKS
 *--------------------------------------------------
 */

/*
 * 获取归档文件中当前的位置。
 *
 * 对于不可寻道的归档文件，我们可能无法获取文件位置。
 * 如果是这样，只需返回 -1。这在那种情况下不太重要，
 * 因为我们无论如何也无法重写 TOC 来填充数据块偏移量。
 *
 */
static pgoff_t
_getFilePos(ArchiveHandle *AH, lclContext *ctx)
{
	pgoff_t		pos;

	pos = ftello(AH->FH);
	if (pos < 0)
	{
		/* 如果我们发现可以寻道，不应该发生此情况。 */
		if (ctx->hasSeek)
			pg_fatal("could not determine seek position in archive file: %m");
	}
	return pos;
}

/*
 * 读取数据块头。格式在 V1.3 中发生了变化，
 * 因此为简单起见我们在此集中处理。如果位于 EOF，
 * 返回 *type = EOF。
 */
static void
_readBlockHeader(ArchiveHandle *AH, int *type, int *id)
{
	int			byt;

	/*
	 * 注意：如果是 pre-1.3 的输入文件且在 EOF 处，
	 * 我们会在 ReadInt 内部 pg_fatal() 而不是返回 EOF。
	 * 似乎不值得费力去更好地处理这种情况，
	 * 因为这类文件在实际环境中几乎不可能存在：
	 * 只有某些 7.1 开发版本的 pg_dump 才会生成这样的文件。
	 */
	if (AH->version < K_VERS_1_3)
		*type = BLK_DATA;
	else
	{
		byt = getc(AH->FH);
		*type = byt;
		if (byt == EOF)
		{
			*id = 0;			/* 不要返回未初始化的值 */
			return;
		}
	}

	*id = ReadInt(AH);
}

/*
 * writeData 的回调函数。向归档写入一个（压缩后的）数据块。
 *
 */
static void
_CustomWriteFunc(ArchiveHandle *AH, const char *buf, size_t len)
{
	/* 绝不写入 0 字节的块（这不应发生） */
	if (len > 0)
	{
		WriteInt(AH, len);
		_WriteBuf(AH, buf, len);
	}
}

/*
 * readData 的回调函数。为简单起见，
 * 我们一次读取一个压缩块。
 */
static size_t
_CustomReadFunc(ArchiveHandle *AH, char **buf, size_t *buflen)
{
	size_t		blkLen;

	/* 读取长度 */
	blkLen = ReadInt(AH);
	if (blkLen == 0)
		return 0;

	/* 如果调用者的缓冲区不够大，则分配更大的 */
	if (blkLen > *buflen)
	{
		free(*buf);
		*buf = (char *) pg_malloc(blkLen);
		*buflen = blkLen;
	}

	/* 读取错误时退出应用 */
	_ReadBuf(AH, *buf, blkLen);

	return blkLen;
}
