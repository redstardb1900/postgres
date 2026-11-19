/*-------------------------------------------------------------------------
 *
 * hash_xlog.h
 *	  Postgres hash AM 实现的头文件
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hash_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_XLOG_H
#define HASH_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/* XLOG_HASH_SQUEEZE_PAGE 操作所需缓冲区数 */
#define HASH_XLOG_FREE_OVFL_BUFS	6

/*
 * hash 操作的 XLOG 记录
 */
#define XLOG_HASH_INIT_META_PAGE	0x00	/* 初始化元页 */
#define XLOG_HASH_INIT_BITMAP_PAGE	0x10	/* 初始化位图页 */
#define XLOG_HASH_INSERT		0x20	/* 无分裂添加索引元组 */
#define XLOG_HASH_ADD_OVFL_PAGE 0x30	/* 添加溢出页 */
#define XLOG_HASH_SPLIT_ALLOCATE_PAGE	0x40	/* 为分裂分配新页 */
#define XLOG_HASH_SPLIT_PAGE	0x50	/* 分裂页 */
#define XLOG_HASH_SPLIT_COMPLETE	0x60	/* 分裂操作完成 */
#define XLOG_HASH_MOVE_PAGE_CONTENTS	0x70	/* 从一页移除元组
												 * 并加到另一页*/
#define XLOG_HASH_SQUEEZE_PAGE	0x80	/* 把元组加到桶链先前
										 * 页之一并释放溢出
										 * 页*/
#define XLOG_HASH_DELETE		0x90	/* 从页删除索引元组 */
#define XLOG_HASH_SPLIT_CLEANUP 0xA0	/* 清除主桶页上的分裂清理
										 * 标志,在删除因分裂移走
										 * 的元组之后*/
#define XLOG_HASH_UPDATE_META_PAGE	0xB0	/* vacuum 后更新元页 */

#define XLOG_HASH_VACUUM_ONE_PAGE	0xC0	/* 从索引页移除死元组
											 * */

/*
 * xl_hash_split_allocate_page 标志值,有 8 位可用。
 */
#define XLH_SPLIT_META_UPDATE_MASKS		(1<<0)
#define XLH_SPLIT_META_UPDATE_SPLITPOINT		(1<<1)

/*
 * 关于简单(无分裂)插入需知这些。
 *
 * 本数据记录用于 XLOG_HASH_INSERT
 *
 * 备份块 0: 原页(数据含插入的元组)
 * 备份块 1: 元页(HashMetaPageData)
 */
typedef struct xl_hash_insert
{
	OffsetNumber offnum;
} xl_hash_insert;

#define SizeOfHashInsert	(offsetof(xl_hash_insert, offnum) + sizeof(OffsetNumber))

/*
 * 关于添加溢出页需知这些。
 *
 * 本数据记录用于 XLOG_HASH_ADD_OVFL_PAGE
 *
 * 备份块 0: 新分配的溢出页
 * 备份块 1: 桶链中新溢出页之前的页
 * 备份块 2: 位图页
 * 备份块 3: 新位图页
 * 备份块 4: 元页
 */
typedef struct xl_hash_add_ovfl_page
{
	uint16		bmsize;
	bool		bmpage_found;
} xl_hash_add_ovfl_page;

#define SizeOfHashAddOvflPage	\
	(offsetof(xl_hash_add_ovfl_page, bmpage_found) + sizeof(bool))

/*
 * 关于为分裂分配页需知这些。
 *
 * 本数据记录用于 XLOG_HASH_SPLIT_ALLOCATE_PAGE
 *
 * 备份块 0: 旧桶的页
 * 备份块 1: 新桶的页
 * 备份块 2: 元页
 */
typedef struct xl_hash_split_allocate_page
{
	uint32		new_bucket;
	uint16		old_bucket_flag;
	uint16		new_bucket_flag;
	uint8		flags;
} xl_hash_split_allocate_page;

#define SizeOfHashSplitAllocPage	\
	(offsetof(xl_hash_split_allocate_page, flags) + sizeof(uint8))

/*
 * 关于完成分裂操作需知这些。
 *
 * 本数据记录用于 XLOG_HASH_SPLIT_COMPLETE
 *
 * 备份块 0: 旧桶的页
 * 备份块 1: 新桶的页
 */
typedef struct xl_hash_split_complete
{
	uint16		old_bucket_flag;
	uint16		new_bucket_flag;
} xl_hash_split_complete;

#define SizeOfHashSplitComplete \
	(offsetof(xl_hash_split_complete, new_bucket_flag) + sizeof(uint16))

/*
 * 关于挤压操作所需的页内容移动
 * 需知这些。
 *
 * 本数据记录用于 XLOG_HASH_MOVE_PAGE_CONTENTS
 *
 * 备份块 0: 主桶页
 * 备份块 1: 含被移动元组的页
 * 备份块 2: 将从中移除元组的页
 */
typedef struct xl_hash_move_page_contents
{
	uint16		ntups;
	bool		is_prim_bucket_same_wrt;	/* 为真若元组移入页
											 * 同为
											 * 主桶页*/
} xl_hash_move_page_contents;

#define SizeOfHashMovePageContents	\
	(offsetof(xl_hash_move_page_contents, is_prim_bucket_same_wrt) + sizeof(bool))

/*
 * 关于挤压页操作需知这些。
 *
 * 本数据记录用于 XLOG_HASH_SQUEEZE_PAGE
 *
 * 备份块 0: 主桶页
 * 备份块 1: 含自已释放溢出页移来元组的页
 * 备份块 2: 已释放溢出页
 * 备份块 3: 已释放溢出页的前一页
 * 备份块 4: 已释放溢出页的后一页
 * 备份块 5: 含已释放溢出页信息的位图页
 * 备份块 6: 元页
 */
typedef struct xl_hash_squeeze_page
{
	BlockNumber prevblkno;
	BlockNumber nextblkno;
	uint16		ntups;
	bool		is_prim_bucket_same_wrt;	/* 为真若元组移入页
											 * 同为
											 * 主桶页*/
	bool		is_prev_bucket_same_wrt;	/* 为真若元组移入页
											 * 是已释放溢出
											 * 页之前的
											 * 页*/
} xl_hash_squeeze_page;

#define SizeOfHashSqueezePage	\
	(offsetof(xl_hash_squeeze_page, is_prev_bucket_same_wrt) + sizeof(bool))

/*
 * 关于从页删除索引元组需知这些。
 *
 * 本数据记录用于 XLOG_HASH_DELETE
 *
 * 备份块 0: 主桶页
 * 备份块 1: 从中删除元组的页
 */
typedef struct xl_hash_delete
{
	bool		clear_dead_marking; /* 为真若本操作清除
									 * LH_PAGE_HAS_DEAD_TUPLES flag */
	bool		is_primary_bucket_page; /* 为真若操作针对
										 * 主桶页*/
} xl_hash_delete;

#define SizeOfHashDelete	(offsetof(xl_hash_delete, is_primary_bucket_page) + sizeof(bool))

/*
 * 关于元页更新操作需知这些。
 *
 * 本数据记录用于 XLOG_HASH_UPDATE_META_PAGE
 *
 * 备份块 0: 元页
 */
typedef struct xl_hash_update_meta_page
{
	double		ntuples;
} xl_hash_update_meta_page;

#define SizeOfHashUpdateMetaPage	\
	(offsetof(xl_hash_update_meta_page, ntuples) + sizeof(double))

/*
 * 关于初始化元页需知这些。
 *
 * 本数据记录用于 XLOG_HASH_INIT_META_PAGE
 *
 * 备份块 0: 元页
 */
typedef struct xl_hash_init_meta_page
{
	double		num_tuples;
	RegProcedure procid;
	uint16		ffactor;
} xl_hash_init_meta_page;

#define SizeOfHashInitMetaPage		\
	(offsetof(xl_hash_init_meta_page, ffactor) + sizeof(uint16))

/*
 * 关于初始化位图页需知这些。
 *
 * 本数据记录用于 XLOG_HASH_INIT_BITMAP_PAGE
 *
 * 备份块 0: 位图页
 * 备份块 1: 元页
 */
typedef struct xl_hash_init_bitmap_page
{
	uint16		bmsize;
} xl_hash_init_bitmap_page;

#define SizeOfHashInitBitmapPage	\
	(offsetof(xl_hash_init_bitmap_page, bmsize) + sizeof(uint16))

/*
 * 索引元组删除并更新
 * 元页所需这些。
 *
 * 本数据记录用于 XLOG_HASH_VACUUM_ONE_PAGE
 *
 * 备份块 0: 主桶页
 * 备份块 1: 元页
 */
typedef struct xl_hash_vacuum_one_page
{
	TransactionId snapshotConflictHorizon;
	uint16		ntuples;
	bool		isCatalogRel;	/* 处理备库上逻辑解码时的
								 * 恢复冲突*/

	/* TARGET OFFSET NUMBERS */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} xl_hash_vacuum_one_page;

#define SizeOfHashVacuumOnePage offsetof(xl_hash_vacuum_one_page, offsets)

extern void hash_redo(XLogReaderState *record);
extern void hash_desc(StringInfo buf, XLogReaderState *record);
extern const char *hash_identify(uint8 info);
extern void hash_mask(char *pagedata, BlockNumber blkno);

#endif							/* HASH_XLOG_H */
