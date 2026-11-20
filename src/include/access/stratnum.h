/*-------------------------------------------------------------------------
 *
 * stratnum.h
 *	  POSTGRES 策略号定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/stratnum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRATNUM_H
#define STRATNUM_H

/*
 * 策略号标识特定操作符相对于特定操作符类所具有的
 * 语义。有些情况下，策略子类型（一个 OID）用作
 * 进一步信息。
 */
typedef uint16 StrategyNumber;

#define InvalidStrategy ((StrategyNumber) 0)

/*
 * 用于 B-tree 索引的策略号。
 */
#define BTLessStrategyNumber			1
#define BTLessEqualStrategyNumber		2
#define BTEqualStrategyNumber			3
#define BTGreaterEqualStrategyNumber	4
#define BTGreaterStrategyNumber			5

#define BTMaxStrategyNumber				5

/*
 *	哈希索引的策略号。仅有一种有效策略用于
 *	散列：相等。
 */
#define HTEqualStrategyNumber			1

#define HTMaxStrategyNumber				1

/*
 * GiST、SP-GiST 与 BRIN 某些操作符类共用的策略号。
 *
 * 这些中前几个来自 R-Tree 索引方法（故有其名）；
 * 其余已随时间按需加入。
 */
#define RTLeftStrategyNumber			1	/* 为 << */
#define RTOverLeftStrategyNumber		2	/* 为 &< */
#define RTOverlapStrategyNumber			3	/* 为 && */
#define RTOverRightStrategyNumber		4	/* 为 &> */
#define RTRightStrategyNumber			5	/* 为 >> */
#define RTSameStrategyNumber			6	/* 为 ~= */
#define RTContainsStrategyNumber		7	/* 为 @> */
#define RTContainedByStrategyNumber		8	/* 为 <@ */
#define RTOverBelowStrategyNumber		9	/* 为 &<| */
#define RTBelowStrategyNumber			10	/* 为 <<| */
#define RTAboveStrategyNumber			11	/* 为 |>> */
#define RTOverAboveStrategyNumber		12	/* 为 |&> */
#define RTOldContainsStrategyNumber		13	/* 为 @> 的旧拼写 */
#define RTOldContainedByStrategyNumber	14	/* 为 <@ 的旧拼写 */
#define RTKNNSearchStrategyNumber		15	/* 为 <-> (距离) */
#define RTContainsElemStrategyNumber	16	/* 为范围类型 @> elem */
#define RTAdjacentStrategyNumber		17	/* 为 -|- */
#define RTEqualStrategyNumber			18	/* 为 = */
#define RTNotEqualStrategyNumber		19	/* 为 != */
#define RTLessStrategyNumber			20	/* 为 < */
#define RTLessEqualStrategyNumber		21	/* 为 <= */
#define RTGreaterStrategyNumber			22	/* 为 > */
#define RTGreaterEqualStrategyNumber	23	/* 为 >= */
#define RTSubStrategyNumber				24	/* 为 inet >> */
#define RTSubEqualStrategyNumber		25	/* 为 inet <<= */
#define RTSuperStrategyNumber			26	/* 为 inet << */
#define RTSuperEqualStrategyNumber		27	/* 为 inet >>= */
#define RTPrefixStrategyNumber			28	/* 为 text ^@ */
#define RTOldBelowStrategyNumber		29	/* 为 <<| 的旧拼写 */
#define RTOldAboveStrategyNumber		30	/* 为 |>> 的旧拼写 */

#define RTMaxStrategyNumber				30


#endif							/* STRATNUM_H */
