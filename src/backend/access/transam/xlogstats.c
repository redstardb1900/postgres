/*-------------------------------------------------------------------------
 *
 * xlogstats.c
 *		WAL 统计相关函数
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogstats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogreader.h"
#include "access/xlogstats.h"

/*
 * 计算一条记录的大小，拆成非 FPI 与 FPI 两部分。
 */
void
XLogRecGetLen(XLogReaderState *record, uint32 *rec_len,
			  uint32 *fpi_len)
{
	int			block_id;

	/*
	 * 计算该记录中的 FPI 数据量。
	 *
	 * XXX: 我们窥视 xlogreader 私有的已解码备份块，读取
	 * 表示 FPI 数据长度的 bimg_len。
	 */
	*fpi_len = 0;
	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		if (XLogRecHasBlockImage(record, block_id))
			*fpi_len += XLogRecGetBlock(record, block_id)->bimg_len;
	}

	/*
	 * 记录长度取为总长度减去所有
	 * 块映像的长度。
	 */
	*rec_len = XLogRecGetTotalLen(record) - *fpi_len;
}

/*
 * 为给定记录保存按资源管理器与按记录的统计。
 */
void
XLogRecStoreStats(XLogStats *stats, XLogReaderState *record)
{
	RmgrId		rmid;
	uint8		recid;
	uint32		rec_len;
	uint32		fpi_len;

	Assert(stats != NULL && record != NULL);

	stats->count++;

	rmid = XLogRecGetRmid(record);

	XLogRecGetLen(record, &rec_len, &fpi_len);

	/* 更新按资源管理器的统计 */

	stats->rmgr_stats[rmid].count++;
	stats->rmgr_stats[rmid].rec_len += rec_len;
	stats->rmgr_stats[rmid].fpi_len += fpi_len;

	/*
	 * 更新按记录的统计；记录由 RmgrId 与 xl_info
	 * 字段中属于该资源管理器域的四位组合来标识
	 * （因而每个 RmgrId 有十六个可能的
	 * 表项）。
	 */

	recid = XLogRecGetInfo(record) >> 4;

	/*
	 * XACT 记录需要另行处理。这类记录用上述四位中的
	 * 第一位作为可选标志变量，后三位作为操作码。我们
	 * 从 xl_info 中滤出操作码，并用它作为该记录的
	 * 标识符。
	 */
	if (rmid == RM_XACT_ID)
		recid &= 0x07;

	stats->record_stats[rmid][recid].count++;
	stats->record_stats[rmid][recid].rec_len += rec_len;
	stats->record_stats[rmid][recid].fpi_len += fpi_len;
}
