/*-------------------------------------------------------------------------
 *
 * spgxlog.h
 *	  SP-GiST 访问方法的 xlog 声明。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGXLOG_H
#define SPGXLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/* SPGiST 的 XLOG 记录类型 */
 /* #define XLOG_SPGIST_CREATE_INDEX       0x00 */	/* not used anymore */
#define XLOG_SPGIST_ADD_LEAF		0x10
#define XLOG_SPGIST_MOVE_LEAFS		0x20
#define XLOG_SPGIST_ADD_NODE		0x30
#define XLOG_SPGIST_SPLIT_TUPLE		0x40
#define XLOG_SPGIST_PICKSPLIT		0x50
#define XLOG_SPGIST_VACUUM_LEAF		0x60
#define XLOG_SPGIST_VACUUM_ROOT		0x70
#define XLOG_SPGIST_VACUUM_REDIRECT 0x80

/*
 * 某些 redo 函数需要 SpGistState，尽管其中仅少数字段
 * 须有效。spgxlogState 在 xlog 记录中携带所需信息。
 * (更多说明见 spgxlog.c 的 fillFakeState。)
 */
typedef struct spgxlogState
{
	TransactionId myXid;
	bool		isBuild;
} spgxlogState;

/*
 * 备份块 0: 叶元组的目标页
 * 备份块 1: 父页(若有)
 */
typedef struct spgxlogAddLeaf
{
	bool		newPage;		/* 初化目标页?*/
	bool		storesNulls;	/* 页是否在空值树中? */
	OffsetNumber offnumLeaf;	/* 叶元组放置处之偏移 */
	OffsetNumber offnumHeadLeaf;	/* 链中头元组偏移，若有 */

	OffsetNumber offnumParent;	/* 父下行链接所在，若有 */
	uint16		nodeI;

	/* 后跟新叶元组(未对齐!) */
} spgxlogAddLeaf;

/*
 * 备份块 0: 源叶页
 * 备份块 1: 目标叶页
 * 备份块 2: 父页
 */
typedef struct spgxlogMoveLeafs
{
	uint16		nMoves;			/* 从源页移出的元组数 */
	bool		newPage;		/* 初化目标页?*/
	bool		replaceDead;	/* 是否在替换 DEAD 源元组? */
	bool		storesNulls;	/* 各页是否在空值树中? */

	/* 父下行链接所在 */
	OffsetNumber offnumParent;
	uint16		nodeI;

	spgxlogState stateSrc;

	/*----------
	 * 后跟数据:
	 *		已删元组编号数组，长度 nMoves
	 *		已插入元组编号数组，长度 nMoves + 1 或 1
	 *		叶元组列表，长度 nMoves + 1 或 1 (未对齐!)
	 *
	 * 注意: 若 replaceDead 为真则数据中仅一个已插入元组
	 * 编号与仅一个叶元组，因我们不从源复制
	 * 那条死元组
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogMoveLeafs;

#define SizeOfSpgxlogMoveLeafs	offsetof(spgxlogMoveLeafs, offsets)

/*
 * 备份块 0: 原页
 * 备份块 1: 新元组去处，若非同一位置
 * 备份块 2: 父下行链接所在，若已更新且不同于
 *				 旧页与新页
 */
typedef struct spgxlogAddNode
{
	/*
	 * 原内元组在原页上的偏移(位于备份
	 * 块 0).
	 */
	OffsetNumber offnum;

	/*
	 * 新元组在新页上的偏移（位于备份块 1）。无效，
	 * 若我们在原页上覆写了旧元组）。
	 */
	OffsetNumber offnumNew;
	bool		newPage;		/* 初化新页? */

	/*----
	 * 父下行链接在哪? parentBlk 指出它所在页，
	 * offnumParent 是页内偏移。parentBlk 的可能取值
	 * 为:
	 *
	 * 0: 父 == 原页
	 * 1: 父 == 新页
	 * 2: 父 == 其他页 (块引用 2)
	 * -1: 父未更新
	 *----
	 */
	int8		parentBlk;
	OffsetNumber offnumParent;	/* 父页内的偏移 */

	uint16		nodeI;

	spgxlogState stateSrc;

	/*
	 * 后跟已更新内元组(未对齐!)
	 */
} spgxlogAddNode;

/*
 * 备份块 0: 前缀元组去处
 * 备份块 1: 后缀元组去处(若不同页)
 */
typedef struct spgxlogSplitTuple
{
	/* 前缀元组去处 */
	OffsetNumber offnumPrefix;

	/* 后缀元组去处 */
	OffsetNumber offnumPostfix;
	bool		newPage;		/* 需要初始化该页? */
	bool		postfixBlkSame; /* 后缀元组是否放在与前缀
								 * 同页?*/

	/*
	 * 后跟新前缀内元组，再跟新后缀内元组(二者均
	 * 未对齐!)
	 */
} spgxlogSplitTuple;

/*
 * rdata 数组中的缓冲区引用为:
 * 备份块 0: Src 页(仅当非根)
 * 备份块 1: Dest 页(若使用)
 * 备份块 2: Inner 页
 * 备份块 3: 父页(若有，且不同于 Inner)
 */
typedef struct spgxlogPickSplit
{
	bool		isRootSplit;

	uint16		nDelete;		/* 自Src删除个数 */
	uint16		nInsert;		/* Src和/或Dest上插入个数 */
	bool		initSrc;		/* 再初化Src页? */
	bool		initDest;		/* 再初化Dest页? */

	/* 新内元组放置处 */
	OffsetNumber offnumInner;
	bool		initInner;		/* 再初化Inner页? */

	bool		storesNulls;	/* 各页是否在空值树中? */

	/* 父下行链接所在，若有 */
	bool		innerIsParent;	/* 父是否与内页相同? */
	OffsetNumber offnumParent;
	uint16		nodeI;

	spgxlogState stateSrc;

	/*----------
	 * 后跟数据:
	 *		已删元组编号数组，长度 nDelete
	 *		已插入元组编号数组，长度 nInsert
	 *		已插入元组的页选择字节数组，长度 nInsert
	 *		新内元组 (未对齐!)
	 *		叶元组列表，长度 nInsert (未对齐!)
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogPickSplit;

#define SizeOfSpgxlogPickSplit offsetof(spgxlogPickSplit, offsets)

typedef struct spgxlogVacuumLeaf
{
	uint16		nDead;			/* 将变为 DEAD 的元组数 */
	uint16		nPlaceholder;	/* 将变为 PLACEHOLDER 的元组数 */
	uint16		nMove;			/* 要移动的元组数 */
	uint16		nChain;			/* 要重链的元组数 */

	spgxlogState stateSrc;

	/*----------
	 * 后跟数据:
	 *		变为 DEAD 的元组编号
	 *		变为 PLACEHOLDER 的元组编号
	 *		移出源元组编号(并以 PLACEHOLDER 替换)
	 *		移入目标元组编号(替换原有内容)
	 *		要更新 nextOffset 链接的元组编号
	 *		要插入 nextOffset 链接的元组编号
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumLeaf;

#define SizeOfSpgxlogVacuumLeaf offsetof(spgxlogVacuumLeaf, offsets)

typedef struct spgxlogVacuumRoot
{
	/* 当根页亦为叶时清理该根页 */
	uint16		nDelete;		/* 要删除的元组数 */

	spgxlogState stateSrc;

	/* 后跟待删元组的偏移 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumRoot;

#define SizeOfSpgxlogVacuumRoot offsetof(spgxlogVacuumRoot, offsets)

typedef struct spgxlogVacuumRedirect
{
	uint16		nToPlaceholder; /* 将重定向改为占位的数目 */
	OffsetNumber firstPlaceholder;	/* 要移除的首个占位元组 */
	TransactionId snapshotConflictHorizon;	/* 被移除重定向中最新 XID */
	bool		isCatalogRel;	/* 处理备库上逻辑解码时的
								 * 恢复冲突*/

	/* 后跟将改为占位的重定向元组偏移 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumRedirect;

#define SizeOfSpgxlogVacuumRedirect offsetof(spgxlogVacuumRedirect, offsets)

extern void spg_redo(XLogReaderState *record);
extern void spg_desc(StringInfo buf, XLogReaderState *record);
extern const char *spg_identify(uint8 info);
extern void spg_xlog_startup(void);
extern void spg_xlog_cleanup(void);
extern void spg_mask(char *pagedata, BlockNumber blkno);

#endif							/* SPGXLOG_H */
