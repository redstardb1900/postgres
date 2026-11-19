/*-------------------------------------------------------------------------
 *
 * heapam_xlog.h
 *	  POSTGRES 堆访问 XLOG 定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_XLOG_H
#define HEAPAM_XLOG_H

#include "access/htup.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"


/*
 * heapam.c WAL 操作的 WAL 记录定义
 *
 * XLOG 允许在日志记录 xl_info 字段高 4 位存放
 * 一些信息。3 位用作操作码，1 位用作初始化位。
 */
#define XLOG_HEAP_INSERT		0x00
#define XLOG_HEAP_DELETE		0x10
#define XLOG_HEAP_UPDATE		0x20
#define XLOG_HEAP_TRUNCATE		0x30
#define XLOG_HEAP_HOT_UPDATE	0x40
#define XLOG_HEAP_CONFIRM		0x50
#define XLOG_HEAP_LOCK			0x60
#define XLOG_HEAP_INPLACE		0x70

#define XLOG_HEAP_OPMASK		0x70
/*
 * 在 INSERT、UPDATE、HOT_UPDATE 或 MULTI_INSERT 中向新页
 * 插入首项时，我们可(也确实)在 redo 恢复整页。
 */
#define XLOG_HEAP_INIT_PAGE		0x80
/*
 * 操作码已用完，故 heapam.c 现有第二个 RmgrId。这些操作码
 * 与 RM_HEAP2_ID 关联，但逻辑上与上面关联 RM_HEAP_ID 的
 * 那些并无不同。XLOG_HEAP_OPMASK 对它们也
 * 适用。
 */
#define XLOG_HEAP2_REWRITE		0x00
#define XLOG_HEAP2_PRUNE		0x10
#define XLOG_HEAP2_VACUUM		0x20
#define XLOG_HEAP2_FREEZE_PAGE	0x30
#define XLOG_HEAP2_VISIBLE		0x40
#define XLOG_HEAP2_MULTI_INSERT 0x50
#define XLOG_HEAP2_LOCK_UPDATED 0x60
#define XLOG_HEAP2_NEW_CID		0x70

/*
 * xl_heap_insert/xl_heap_multi_insert 标志值，可用 8 位。
 */
/* PD_ALL_VISIBLE 已清除 */
#define XLH_INSERT_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_INSERT_LAST_IN_MULTI				(1<<1)
#define XLH_INSERT_IS_SPECULATIVE				(1<<2)
#define XLH_INSERT_CONTAINS_NEW_TUPLE			(1<<3)
#define XLH_INSERT_ON_TOAST_RELATION			(1<<4)

/* all_frozen_set 必然蕴含 all_visible_set */
#define XLH_INSERT_ALL_FROZEN_SET				(1<<5)

/*
 * xl_heap_update 标志值，可用 8 位。
 */
/* PD_ALL_VISIBLE 已清除 */
#define XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED		(1<<0)
/* PD_ALL_VISIBLE 已在第二个页面清除 */
#define XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED		(1<<1)
#define XLH_UPDATE_CONTAINS_OLD_TUPLE			(1<<2)
#define XLH_UPDATE_CONTAINS_OLD_KEY				(1<<3)
#define XLH_UPDATE_CONTAINS_NEW_TUPLE			(1<<4)
#define XLH_UPDATE_PREFIX_FROM_OLD				(1<<5)
#define XLH_UPDATE_SUFFIX_FROM_OLD				(1<<6)

/* 用于检查是否记录了任何形式旧元组的便利宏 */
#define XLH_UPDATE_CONTAINS_OLD						\
	(XLH_UPDATE_CONTAINS_OLD_TUPLE | XLH_UPDATE_CONTAINS_OLD_KEY)

/*
 * xl_heap_delete 标志值，可用 8 位。
 */
/* PD_ALL_VISIBLE 已清除 */
#define XLH_DELETE_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_DELETE_CONTAINS_OLD_TUPLE			(1<<1)
#define XLH_DELETE_CONTAINS_OLD_KEY				(1<<2)
#define XLH_DELETE_IS_SUPER						(1<<3)
#define XLH_DELETE_IS_PARTITION_MOVE			(1<<4)

/* 用于检查是否记录了任何形式旧元组的便利宏 */
#define XLH_DELETE_CONTAINS_OLD						\
	(XLH_DELETE_CONTAINS_OLD_TUPLE | XLH_DELETE_CONTAINS_OLD_KEY)

/* 这是我们需要了解的删除信息 */
typedef struct xl_heap_delete
{
	TransactionId xmax;			/* 被删除元组的 xmax */
	OffsetNumber offnum;		/* 被删元组偏移量 */
	uint8		infobits_set;	/* infomask 位 */
	uint8		flags;
} xl_heap_delete;

#define SizeOfHeapDelete	(offsetof(xl_heap_delete, flags) + sizeof(uint8))

/*
 * xl_heap_truncate 标志值，可用 8 位。
 */
#define XLH_TRUNCATE_CASCADE					(1<<0)
#define XLH_TRUNCATE_RESTART_SEQS				(1<<1)

/*
 * 对 truncate，我们把所有被截断的 relid 列为数组，后跟
 * 需重启的所有序列 relid(如有)。
 * 所有关系始终在同一数据库内，故只列一次 dbid。
 */
typedef struct xl_heap_truncate
{
	Oid			dbId;
	uint32		nrelids;
	uint8		flags;
	Oid			relids[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_truncate;

#define SizeOfHeapTruncate	(offsetof(xl_heap_truncate, relids))

/*
 * 我们不在 WAL 中存储插入或更新元组的整个固定部分
 * 即 HeapTupleHeaderData；重建 WAL 记录中别处已有的字段
 * 可省几个字节，或者有些字段干脆无需重建。
 * 以下是我们必须存储的字段。
 */
typedef struct xl_heap_header
{
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
} xl_heap_header;

#define SizeOfHeapHeader	(offsetof(xl_heap_header, t_hoff) + sizeof(uint8))

/* 这是我们需要了解的插入信息 */
typedef struct xl_heap_insert
{
	OffsetNumber offnum;		/* 被插元组偏移量 */
	uint8		flags;

	/* 备份块0 含 xl_heap_header 与元组数据 */
} xl_heap_insert;

#define SizeOfHeapInsert	(offsetof(xl_heap_insert, flags) + sizeof(uint8))

/*
 * 这是需要了解的多元组插入信息。
 *
 * 记录的主数据由此 xl_heap_multi_insert 头构成。
 * 若整页被重新初始化，则省略 'offsets' 数组
 * (XLOG_HEAP_INIT_PAGE).
 *
 * 块 0 的数据部分中有一个 xl_multi_insert_tuple 结构，
 * 其后是各元组的元组数据。另有填充，用于对齐各
 * xl_multi_insert_tuple 结构。
 */
typedef struct xl_heap_multi_insert
{
	uint8		flags;
	uint16		ntuples;
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_multi_insert;

#define SizeOfHeapMultiInsert	offsetof(xl_heap_multi_insert, offsets)

typedef struct xl_multi_insert_tuple
{
	uint16		datalen;		/* 其后元组数据的大小 */
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
	/* 元组数据紧随结构末尾 */
} xl_multi_insert_tuple;

#define SizeOfMultiInsertTuple	(offsetof(xl_multi_insert_tuple, t_hoff) + sizeof(uint8))

/*
 * 这是我们需要了解的 update|hot_update 信息
 *
 * 备份块 0：新页
 *
 * 若设 XLH_UPDATE_PREFIX_FROM_OLD 或 XLH_UPDATE_SUFFIX_FROM_OLD 标志，
 * 则前缀和/或后缀居先，为一个或两个 uint16。
 *
 * 其后是 xl_heap_header 和新元组数据。新元组
 * 数据不含前缀和后缀，它们在重放时从
 * 旧元组复制。
 *
 * 若给定 XLH_UPDATE_CONTAINS_NEW_TUPLE 标志，则元组数据
 * 即使取了整页映像也包含在内。
 *
 * 备份块 1：旧页（若不同）。（无数据，仅引用该块）
 */
typedef struct xl_heap_update
{
	TransactionId old_xmax;		/* 旧元组的 xmax */
	OffsetNumber old_offnum;	/* 旧元组偏移号 */
	uint8		old_infobits_set;	/* 旧元组上待设 infomask 位 */
	uint8		flags;
	TransactionId new_xmax;		/* 新元组的 xmax */
	OffsetNumber new_offnum;	/* 新元组偏移号 */

	/*
	 * 若设 XLH_UPDATE_CONTAINS_OLD_TUPLE 或 XLH_UPDATE_CONTAINS_OLD_KEY
	 * 标志，其后是旧元组的 xl_heap_header 和元组数据。
	 */
} xl_heap_update;

#define SizeOfHeapUpdate	(offsetof(xl_heap_update, new_offnum) + sizeof(OffsetNumber))

/*
 * 这是需要了解的页面剪枝信息（含 VACUUM 期间和
 * 机会性剪枝期间）
 *
 * 记录固定部分之后的 OffsetNumber 数组包含：
 *	* 每个重定向项：项偏移号，然后是重定向目标偏移号
 *	* 每个现已死亡的项：项偏移号
 *	* 每个现未使用的项：项偏移号
 * 故 OffsetNumber 总数为 2*nredirected+ndead+nunused。
 * 注意 nunused 并未显式存储，但可参照
 * 记录总长度求得。
 *
 * 获取完全清理锁。
 */
typedef struct xl_heap_prune
{
	TransactionId snapshotConflictHorizon;
	uint16		nredirected;
	uint16		ndead;
	bool		isCatalogRel;	/* 用于处理备库上逻辑解码期间的
								 * 恢复冲突*/
	/* 偏移号位于块引用 0 中 */
} xl_heap_prune;

#define SizeOfHeapPrune (offsetof(xl_heap_prune, isCatalogRel) + sizeof(bool))

/*
 * 清理页记录与剪枝记录类似，但只能把已是 LP_DEAD
 * 的项标记为 LP_UNUSED（在 VACUUM 第二遍堆扫描期间）
 *
 * 只获取普通排他锁。
 */
typedef struct xl_heap_vacuum
{
	uint16		nunused;
	/* 偏移号位于块引用 0 中 */
} xl_heap_vacuum;

#define SizeOfHeapVacuum (offsetof(xl_heap_vacuum, nunused) + sizeof(uint16))

/* infobits_set 的标志 */
#define XLHL_XMAX_IS_MULTI		0x01
#define XLHL_XMAX_LOCK_ONLY		0x02
#define XLHL_XMAX_EXCL_LOCK		0x04
#define XLHL_XMAX_KEYSHR_LOCK	0x08
#define XLHL_KEYS_UPDATED		0x10

/* xl_heap_lock / xl_heap_lock_updated 标志字段的标志位 */
#define XLH_LOCK_ALL_FROZEN_CLEARED		0x01

/* 这是我们需要了解的锁信息 */
typedef struct xl_heap_lock
{
	TransactionId xmax;			/* 可能是 MultiXactId */
	OffsetNumber offnum;		/* 页上被锁元组偏移号 */
	uint8		infobits_set;	/* 待设 infomask 和 infomask2 位 */
	uint8		flags;			/* XLH_LOCK_* 标志位 */
} xl_heap_lock;

#define SizeOfHeapLock	(offsetof(xl_heap_lock, flags) + sizeof(uint8))

/* 这是锁定某行已更新版本所需了解的信息 */
typedef struct xl_heap_lock_updated
{
	TransactionId xmax;
	OffsetNumber offnum;
	uint8		infobits_set;
	uint8		flags;
} xl_heap_lock_updated;

#define SizeOfHeapLockUpdated	(offsetof(xl_heap_lock_updated, flags) + sizeof(uint8))

/* 这是确认推测插入所需了解的信息 */
typedef struct xl_heap_confirm
{
	OffsetNumber offnum;		/* 已确认元组的页内偏移 */
} xl_heap_confirm;

#define SizeOfHeapConfirm	(offsetof(xl_heap_confirm, offnum) + sizeof(OffsetNumber))

/* 这是原地更新所需了解的信息 */
typedef struct xl_heap_inplace
{
	OffsetNumber offnum;		/* 已更新元组的页内偏移 */
} xl_heap_inplace;

#define SizeOfHeapInplace	(offsetof(xl_heap_inplace, offnum) + sizeof(OffsetNumber))

/*
 * 本结构表示'冻结计划'，描述如何冻结由一个或多个
 * 堆元组组成的一组（见于 xl_heap_freeze_page 记录）
 */
/* 原0x01=XLH_FREEZE_XMIN */
#define		XLH_FREEZE_XVAC		0x02
#define		XLH_INVALID_XVAC	0x04

typedef struct xl_heap_freeze_plan
{
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* 此计划对应的单个页偏移号数组的长度 */
	uint16		ntuples;
} xl_heap_freeze_plan;

/*
 * 这是 VACUUM 期间冻结某块所需了解的信息
 *
 * 备份块 0 的数据含 xl_heap_freeze_plan 结构体数组，共
 * nplans 个元素，其后接一个或多个页偏移号数组。
 * 每个这样的页偏移号数组对应单个冻结计划，
 * REDO 例程据该冻结计划冻结对应的堆元组。
 */
typedef struct xl_heap_freeze_page
{
	TransactionId snapshotConflictHorizon;
	uint16		nplans;
	bool		isCatalogRel;	/* 用于处理备库上逻辑解码期间的
								 * 恢复冲突*/

	/*
	 * 块 0 的载荷中：冻结计划与偏移号数组
	 */
} xl_heap_freeze_page;

#define SizeOfHeapFreezePage	(offsetof(xl_heap_freeze_page, isCatalogRel) + sizeof(bool))

/*
 * 这是设置可见性映射位所需了解的信息
 *
 * 备份块0:可见性映射缓冲区
 * 备份块1:堆缓冲区
 */
typedef struct xl_heap_visible
{
	TransactionId snapshotConflictHorizon;
	uint8		flags;
} xl_heap_visible;

#define SizeOfHeapVisible (offsetof(xl_heap_visible, flags) + sizeof(uint8))

typedef struct xl_heap_new_cid
{
	/*
	 * 存储顶层 xid，这样就不必合并来自不同事务的
	 * cid
	 */
	TransactionId top_xid;
	CommandId	cmin;
	CommandId	cmax;
	CommandId	combocid;		/* 仅用于调试 */

	/*
	 * 存储 relfilelocator/ctid 对，以便于查找。
	 */
	RelFileLocator target_locator;
	ItemPointerData target_tid;
} xl_heap_new_cid;

#define SizeOfHeapNewCid (offsetof(xl_heap_new_cid, target_tid) + sizeof(ItemPointerData))

/* 逻辑重写 xlog 记录头 */
typedef struct xl_heap_rewrite_mapping
{
	TransactionId mapped_xid;	/* 可能需要看到该行的 xid */
	Oid			mapped_db;		/* DbOid，共享关系为 InvalidOid */
	Oid			mapped_rel;		/* 被映射关系的 Oid */
	off_t		offset;			/* 目前已写入的位置 */
	uint32		num_mappings;	/* 内存中的映射数量 */
	XLogRecPtr	start_lsn;		/* 重写开始时的插入 LSN */
} xl_heap_rewrite_mapping;

extern void HeapTupleHeaderAdvanceConflictHorizon(HeapTupleHeader tuple,
												  TransactionId *snapshotConflictHorizon);

extern void heap_redo(XLogReaderState *record);
extern void heap_desc(StringInfo buf, XLogReaderState *record);
extern const char *heap_identify(uint8 info);
extern void heap_mask(char *pagedata, BlockNumber blkno);
extern void heap2_redo(XLogReaderState *record);
extern void heap2_desc(StringInfo buf, XLogReaderState *record);
extern const char *heap2_identify(uint8 info);
extern void heap_xlog_logical_rewrite(XLogReaderState *r);

extern XLogRecPtr log_heap_visible(Relation rel, Buffer heap_buffer,
								   Buffer vm_buffer,
								   TransactionId snapshotConflictHorizon,
								   uint8 vmflags);

#endif							/* HEAPAM_XLOG_H */
