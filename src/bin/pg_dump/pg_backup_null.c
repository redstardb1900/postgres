/*-------------------------------------------------------------------------
 *
 * pg_backup_null.c
 *
 *	 实现一种永不保存的归档；pg_dump 使用它
 *	 来输出纯文本 SQL 脚本，而不是保存
 *	 真实的归档。
 *
 *	 更多详情请参见 pg_restore 的头文件。
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
 *		src/bin/pg_dump/pg_backup_null.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "fe_utils/string_utils.h"
#include "libpq/libpq-fs.h"
#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"

static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _WriteLOData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);
static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);


/*
 *	初始化器
 */
void
InitArchiveFmt_Null(ArchiveHandle *AH)
{
	/* 假设使用静态函数，这对每种格式都一样。 */
	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ClosePtr = _CloseArchive;
	AH->ReopenPtr = NULL;
	AH->PrintTocDataPtr = _PrintTocData;

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;
	AH->ClonePtr = NULL;
	AH->DeClonePtr = NULL;

	/* 初始化 LO 缓冲 */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) pg_malloc(LOBBUFSIZE);

	/*
	 * 现在阻止读取...
	 */
	if (AH->mode == archModeRead)
		pg_fatal("this format cannot be read");
}

/*
 * - 开始一个新的 TOC 条目
 */

/*
 * 由 dumper 通过 archiver 在数据导出例程中调用
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	/* 直接发送到输出，ahwrite() 已在失败时报告错误 */
	ahwrite(data, 1, dLen, AH);
}

/*
 * 由 dumper 通过 archiver 在数据导出例程中调用
 * 在发出 LO 时用此函数替代 _WriteData
 */
static void
_WriteLOData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	if (dLen > 0)
	{
		PQExpBuffer buf = createPQExpBuffer();

		appendByteaLiteralAHX(buf,
							  (const unsigned char *) data,
							  dLen,
							  AH);

		ahprintf(AH, "SELECT pg_catalog.lowrite(0, %s);\n", buf->data);

		destroyPQExpBuffer(buf);
	}
}

static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "\n\n");
}

/*
 * 当开始保存所有 BLOB 数据（非模式）时由 archiver 调用。
 * 此例程应保存恢复时读取 LO 所需的
 * 格式特定信息。
 *
 * 它在 dumper 的 DataDumper 例程之前调用。
 *
 * 可选，但强烈推荐。
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "BEGIN;\n\n");
}

/*
 * 当 dumper 调用 StartLO 时由 archiver 调用。
 *
 * 强制性。
 *
 * 必须保存传入的 OID 以便在恢复时检索。
 */
static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	bool		old_lo_style = (AH->version < K_VERS_1_12);

	if (oid == 0)
		pg_fatal("invalid OID for large object");

	/* 对于旧归档，我们必须在这里处理 drop 和 create 逻辑 */
	if (old_lo_style && AH->public.ropt->dropSchema)
		DropLOIfExists(AH, oid);

	if (old_lo_style)
		ahprintf(AH, "SELECT pg_catalog.lo_open(pg_catalog.lo_create('%u'), %d);\n",
				 oid, INV_WRITE);
	else
		ahprintf(AH, "SELECT pg_catalog.lo_open('%u', %d);\n",
				 oid, INV_WRITE);

	AH->WriteDataPtr = _WriteLOData;
}

/*
 * 当 dumper 调用 EndLO 时由 archiver 调用。
 *
 * 可选。
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	AH->WriteDataPtr = _WriteData;

	ahprintf(AH, "SELECT pg_catalog.lo_close(0);\n\n");
}

/*
 * 当完成保存所有 BLOB 数据时由 archiver 调用。
 *
 * 可选。
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "COMMIT;\n\n");
}

/*------
 * 作为 RestoreArchive 调用的一部分调用；对于 NULL 归档，
 * 这只是将给定 TOC 条目的数据发送到输出。
 *------
 */
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	if (te->dataDumper)
	{
		AH->currToc = te;

		if (strcmp(te->desc, "BLOBS") == 0)
			_StartLOs(AH, te);

		te->dataDumper((Archive *) AH, te->dataDumperArg);

		if (strcmp(te->desc, "BLOBS") == 0)
			_EndLOs(AH, te);

		AH->currToc = NULL;
	}
}

static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	/* 不做任何事 */
	return 0;
}

static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	/* 不做任何事 */
}

static void
_CloseArchive(ArchiveHandle *AH)
{
	/* 无事可做 */
}
