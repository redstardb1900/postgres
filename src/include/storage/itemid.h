/*-------------------------------------------------------------------------
 *
 * itemid.h
 *	  标准 POSTGRES 缓冲页项标识/行指针定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ITEMID_H
#define ITEMID_H

/*
 * 缓冲区页上的行指针。见缓冲区页定义与注释
 * 以了解行指针如何使用。
 *
 * 有时行指针虽"在用"但并无关联的
 * 页上存储。按约定无存储的行指针 lp_len == 0，
 * 与其 lp_flags 状态无关。
 */
typedef struct ItemIdData
{
	unsigned	lp_off:15,		/* 至元组的偏移（自页首） */
				lp_flags:2,		/* 行指针状态，见下 */
				lp_len:15;		/* 元组字节长度 */
} ItemIdData;

typedef ItemIdData *ItemId;

/*
 * lp_flags 有以下可能状态。UNUSED 行指针可
 * 立即重用，其他状态则否。
 */
#define LP_UNUSED		0		/* 未用（应总有 lp_len=0） */
#define LP_NORMAL		1		/* 在用（应总有 lp_len>0） */
#define LP_REDIRECT		2		/* HOT 重定向（应有 lp_len=0） */
#define LP_DEAD			3		/* 已死，可有可无存储 */

/*
 * 项偏移与长度由这些类型表示，当其
 * 未实际存放在 ItemIdData 中时。
 */
typedef uint16 ItemOffset;
typedef uint16 ItemLength;


/* ----------------
 *		支持宏
 * ----------------
 */

/*
 *		ItemIdGetLength
 */
#define ItemIdGetLength(itemId) \
   ((itemId)->lp_len)

/*
 *		ItemIdGetOffset
 */
#define ItemIdGetOffset(itemId) \
   ((itemId)->lp_off)

/*
 *		ItemIdGetFlags
 */
#define ItemIdGetFlags(itemId) \
   ((itemId)->lp_flags)

/*
 *		ItemIdGetRedirect
 * REDIRECT 指针中，lp_off 存放下一行指针的偏移号
 */
#define ItemIdGetRedirect(itemId) \
   ((itemId)->lp_off)

/*
 * ItemIdIsValid
 *		项标识有效则为真。
 *		检测很弱，大概仅对 Asserts 有用。
 */
#define ItemIdIsValid(itemId)	PointerIsValid(itemId)

/*
 * ItemIdIsUsed
 *		项标识在用则为真。
 */
#define ItemIdIsUsed(itemId) \
	((itemId)->lp_flags != LP_UNUSED)

/*
 * ItemIdIsNormal
 *		当且仅当项标识处于 NORMAL 为真。
 */
#define ItemIdIsNormal(itemId) \
	((itemId)->lp_flags == LP_NORMAL)

/*
 * ItemIdIsRedirected
 *		当且仅当项标识处于 REDIRECT 为真。
 */
#define ItemIdIsRedirected(itemId) \
	((itemId)->lp_flags == LP_REDIRECT)

/*
 * ItemIdIsDead
 *		当且仅当项标识处于 DEAD 为真。
 */
#define ItemIdIsDead(itemId) \
	((itemId)->lp_flags == LP_DEAD)

/*
 * ItemIdHasStorage
 *		当且仅当项标识有关联存储为真。
 */
#define ItemIdHasStorage(itemId) \
	((itemId)->lp_len != 0)

/*
 * ItemIdSetUnused
 *		把项标识设为 UNUSED，无存储。
 *		当心 itemId 被多次求值！
 */
#define ItemIdSetUnused(itemId) \
( \
	(itemId)->lp_flags = LP_UNUSED, \
	(itemId)->lp_off = 0, \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdSetNormal
 *		把项标识设为 NORMAL，带指定存储。
 *		当心 itemId 被多次求值！
 */
#define ItemIdSetNormal(itemId, off, len) \
( \
	(itemId)->lp_flags = LP_NORMAL, \
	(itemId)->lp_off = (off), \
	(itemId)->lp_len = (len) \
)

/*
 * ItemIdSetRedirect
 *		把项标识设为 REDIRECT，带指定链接。
 *		当心 itemId 被多次求值！
 */
#define ItemIdSetRedirect(itemId, link) \
( \
	(itemId)->lp_flags = LP_REDIRECT, \
	(itemId)->lp_off = (link), \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdSetDead
 *		把项标识设为 DEAD，无存储。
 *		当心 itemId 被多次求值！
 */
#define ItemIdSetDead(itemId) \
( \
	(itemId)->lp_flags = LP_DEAD, \
	(itemId)->lp_off = 0, \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdMarkDead
 *		把项标识设为 DEAD，保留其现有存储。
 *
 * 注意：在索引中，这被当作提示位机制来用；
 * 我们相信多处理器可并行做此并得到
 * 相同结果。
 */
#define ItemIdMarkDead(itemId) \
( \
	(itemId)->lp_flags = LP_DEAD \
)

#endif							/* ITEMID_H */
