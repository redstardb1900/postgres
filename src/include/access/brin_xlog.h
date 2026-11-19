/*-------------------------------------------------------------------------
 *
 * brin_xlog.h
 *	  POSTGRES BRIN 访问 XLOG 定义.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/brin_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BRIN_XLOG_H
#define BRIN_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"


/*
 * BRIN WAL 操作的 WAL 记录定义
 *
 * XLOG 允许把部分信息存在日志记录 xl_info
 * 字段的高 4 位。
 */
#define XLOG_BRIN_CREATE_INDEX		0x00
#define XLOG_BRIN_INSERT			0x10
#define XLOG_BRIN_UPDATE			0x20
#define XLOG_BRIN_SAMEPAGE_UPDATE	0x30
#define XLOG_BRIN_REVMAP_EXTEND		0x40
#define XLOG_BRIN_DESUMMARIZE		0x50

#define XLOG_BRIN_OPMASK			0x70
/*
 * 当我们在新页插入首项时，整页恢复于
 * redo.
 */
#define XLOG_BRIN_INIT_PAGE		0x80

/*
 * 关于 BRIN 索引创建需知这些。
 *
 * 备份块 0: 元页
 */
typedef struct xl_brin_createidx
{
	BlockNumber pagesPerRange;
	uint16		version;
} xl_brin_createidx;
#define SizeOfBrinCreateIdx (offsetof(xl_brin_createidx, version) + sizeof(uint16))

/*
 * 关于 BRIN 元组插入需知这些
 *
 * 备份块 0: 主页，块数据为新的 BrinTuple。
 * 备份块1: 范围映射页
 */
typedef struct xl_brin_insert
{
	BlockNumber heapBlk;

	/* 更新范围映射所需的额外信息 */
	BlockNumber pagesPerRange;

	/* 主页中插入该元组的偏移号。 */
	OffsetNumber offnum;
} xl_brin_insert;

#define SizeOfBrinInsert	(offsetof(xl_brin_insert, offnum) + sizeof(OffsetNumber))

/*
 * 跨页更新与插入相同，但还存放关于旧
 * 元组的信息。
 *
 * 如 xl_brin_insert：
 * 备份块 0: 新页，块数据含新的 BrinTuple。
 * 备份块1: 范围映射页
 *
 * 此外还有：
 * 备份块 2: 旧页
 */
typedef struct xl_brin_update
{
	/* 旧页上旧元组的偏移号 */
	OffsetNumber oldOffnum;

	xl_brin_insert insert;
} xl_brin_update;

#define SizeOfBrinUpdate	(offsetof(xl_brin_update, insert) + SizeOfBrinInsert)

/*
 * 关于 BRIN 元组同页更新需知这些
 *
 * 备份块 0: 已更新页，以新 BrinTuple 为块数据
 */
typedef struct xl_brin_samepage_update
{
	OffsetNumber offnum;
} xl_brin_samepage_update;

#define SizeOfBrinSamepageUpdate		(sizeof(OffsetNumber))

/*
 * 关于范围映射扩展需知这些
 *
 * 备份块 0: 元页
 * 备份块 1: 新范围映射页
 */
typedef struct xl_brin_revmap_extend
{
	/*
	 * XXX: 这其实多余——块号已作为一部分存于
	 * 备份块 1。
	 */
	BlockNumber targetBlk;
} xl_brin_revmap_extend;

#define SizeOfBrinRevmapExtend	(offsetof(xl_brin_revmap_extend, targetBlk) + \
								 sizeof(BlockNumber))

/*
 * 关于范围解除汇总需知这些
 *
 * 备份块 0:范围映射页
 * 备份块 1: 常规页
 */
typedef struct xl_brin_desummarize
{
	BlockNumber pagesPerRange;
	/* 要设为无效的页号位置 */
	BlockNumber heapBlk;
	/* 常规索引页中要删除项的偏移 */
	OffsetNumber regOffset;
} xl_brin_desummarize;

#define SizeOfBrinDesummarize	(offsetof(xl_brin_desummarize, regOffset) + \
								 sizeof(OffsetNumber))


extern void brin_redo(XLogReaderState *record);
extern void brin_desc(StringInfo buf, XLogReaderState *record);
extern const char *brin_identify(uint8 info);
extern void brin_mask(char *pagedata, BlockNumber blkno);

#endif							/* BRIN_XLOG_H */
