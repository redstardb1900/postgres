/*--------------------------------------------------------------------------
 * ginxlog.h
 *	  postgres 倒排索引 xlog 实现的头文件。
 *
 *	Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *	src/include/access/ginxlog.h
 *--------------------------------------------------------------------------
 */
#ifndef GINXLOG_H
#define GINXLOG_H

#include "access/ginblock.h"
#include "access/itup.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

#define XLOG_GIN_CREATE_PTREE  0x10

typedef struct ginxlogCreatePostingTree
{
	uint32		size;
	/* 随后是压缩的倒排列表 */
} ginxlogCreatePostingTree;

/*
 * 插入记录的格式随页类型而异。
 * ginxlogInsert 是所有变体的公共部分。
 *
 * 备份块 0: 目标页
 * 备份块 1: 左子页，若本插入结束未完成的分裂
 */

#define XLOG_GIN_INSERT  0x20

typedef struct
{
	uint16		flags;			/* GIN_INSERT_ISLEAF and/or GIN_INSERT_ISDATA */

	/*
	 * FOLLOWS:
	 *
	 * 1. 若非叶子页，则为左、右子页块号，本次插入
	 * 所结束之分裂的两侧，形如 BlockIdData[2]（慎勿在
	 * 本结构添加会使它们失去 16 位对齐的字段）
	 *
	 * 2. ginxlogInsertEntry 或 ginxlogRecompressDataLeaf 结构，
	 * 视树类型
	 *
	 * 注意：下列结构追加到 ginxlogInsert 后仅
	 * 为 16 位对齐！添加字段时请慎勿要求
	 * 更严的对齐。
	 */
} ginxlogInsert;

typedef struct
{
	OffsetNumber offset;
	bool		isDelete;
	IndexTupleData tuple;		/* 可变长度 */
} ginxlogInsertEntry;


typedef struct
{
	uint16		nactions;

	/* 随后是可变数量的 'actions' */
} ginxlogRecompressDataLeaf;

/*
 * 注意：此结构目前未在代码中使用，仅用作
 * 文档。WAL 记录格式如这里所述，但代码
 * 经 Pointer 直接访问并用 memcpy 读写它们。
 */
typedef struct
{
	uint8		segno;			/* 本动作所作用的段 */
	char		type;			/* 动作类型(见下) */

	/*
	 * 随后是动作专用数据。对 INSERT 与 REPLACE，那是
	 * 一个 GinPostingList 结构。对 ADDITEMS，先是项数的
	 * uint16，再跟各项本身（ItemPointers）。DELETE 动作
	 * 无更多数据。
	 */
}			ginxlogSegmentAction;

/* 动作类型 */
#define GIN_SEGMENT_UNMODIFIED	0	/* 无动作(不用于 WAL 记录) */
#define GIN_SEGMENT_DELETE		1	/* 整段被移除 */
#define GIN_SEGMENT_INSERT		2	/* 整段被添加 */
#define GIN_SEGMENT_REPLACE		3	/* 一段被替换 */
#define GIN_SEGMENT_ADDITEMS	4	/* 项被加入现有段 */

typedef struct
{
	OffsetNumber offset;
	PostingItem newitem;
} ginxlogInsertDataInternal;

/*
 * 备份块 0: 新左页（=原页，若非根分裂）
 * 备份块 1: 新右页
 * 备份块 2: 原页 / 新根页，若根分裂
 * 备份块 3: 左子页，若本插入完成更早的分裂
 */
#define XLOG_GIN_SPLIT	0x30

typedef struct ginxlogSplit
{
	RelFileLocator locator;
	BlockNumber rrlink;			/* 右链，或根之块号若根
								 * 分裂*/
	BlockNumber leftChildBlkno; /* 在非叶分裂时有效 */
	BlockNumber rightChildBlkno;
	uint16		flags;			/* 见下 */
} ginxlogSplit;

/*
 * ginxlogInsert 与 ginxlogSplit 记录所用标志
 */
#define GIN_INSERT_ISDATA	0x01	/* 插入与分裂记录皆用 */
#define GIN_INSERT_ISLEAF	0x02	/* ditto */
#define GIN_SPLIT_ROOT		0x04	/* 仅用于分裂记录 */

/*
 * Vacuum 在有修改时只把整页记入 WAL。这
 * 在功能上等同于 XLOG_FPI 记录，但为调试而
 * 单独保留。（检视 WAL 流时，GIN vacuum 记录
 * 被标为此类而非堆记录，更易看清状况。）本
 * 机制目前仅用于条目树叶子页。
 */
#define XLOG_GIN_VACUUM_PAGE	0x40

/*
 * 倒排树叶子页清理记 WAL，如同插入引起的
 * 重压缩。
 */
#define XLOG_GIN_VACUUM_DATA_LEAF_PAGE	0x90

typedef struct ginxlogVacuumDataLeafPage
{
	ginxlogRecompressDataLeaf data;
} ginxlogVacuumDataLeafPage;

/*
 * 备份块 0: 被删页
 * 备份块 1: 父页
 * 备份块 2: 左兄弟
 */
#define XLOG_GIN_DELETE_PAGE	0x50

typedef struct ginxlogDeletePage
{
	OffsetNumber parentOffset;
	BlockNumber rightLink;
	TransactionId deleteXid;	/* 扫描中还能看见本页的最后 Xid */
} ginxlogDeletePage;

#define XLOG_GIN_UPDATE_META_PAGE 0x60

/*
 * 备份块 0: 元页
 * 备份块 1: 尾页
 */
typedef struct ginxlogUpdateMeta
{
	RelFileLocator locator;
	GinMetaPageData metadata;
	BlockNumber prevTail;
	BlockNumber newRightlink;
	int32		ntuples;		/* 若 ntuples>0 则 metadata.tail 被
								 * 那许多元组更新；否则插入新子
								 * 列表已插入*/
	/* 随后是插入元组的数组 */
} ginxlogUpdateMeta;

#define XLOG_GIN_INSERT_LISTPAGE  0x70

typedef struct ginxlogInsertListPage
{
	BlockNumber rightlink;
	int32		ntuples;
	/* 随后是插入元组的数组 */
} ginxlogInsertListPage;

/*
 * 备份块 0: 元页
 * 备份块 1 至 (ndeleted + 1): 被删页
 */

#define XLOG_GIN_DELETE_LISTPAGE  0x80

/*
 * 删除列表页的 WAL 记录必须含有指向所有
 * 被删页的块引用，故一次记录能删的页数受
 * XLR_MAX_BLOCK_ID 限制。（block_id 0 用于元
 * 页。）
 */
#define GIN_NDELETE_AT_ONCE Min(16, XLR_MAX_BLOCK_ID - 1)
typedef struct ginxlogDeleteListPages
{
	GinMetaPageData metadata;
	int32		ndeleted;
} ginxlogDeleteListPages;

extern void gin_redo(XLogReaderState *record);
extern void gin_desc(StringInfo buf, XLogReaderState *record);
extern const char *gin_identify(uint8 info);
extern void gin_xlog_startup(void);
extern void gin_xlog_cleanup(void);
extern void gin_mask(char *pagedata, BlockNumber blkno);

#endif							/* GINXLOG_H */
