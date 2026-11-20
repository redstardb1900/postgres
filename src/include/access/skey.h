/*-------------------------------------------------------------------------
 *
 * skey.h
 *	  POSTGRES 扫描键定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/skey.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SKEY_H
#define SKEY_H

#include "access/attnum.h"
#include "access/stratnum.h"
#include "fmgr.h"


/*
 * ScanKey 表示比较操作符作用于
 * 表或索引列与常量之间。当它属于 ScanKey
 * 数组时，各比较条件隐式 AND。若为二元
 * 操作符，索引列是操作符的左操作数。
 * （该结构也可支持一元可索引操作符；那时
 * sk_argument 将闲置。目前尚未实现。）
 *
 * 索引扫描时，须为操作符正确设置 sk_strategy
 * 与 sk_subtype。堆扫描中使用 ScanKey 时这些字段
 * 不用，可设为 InvalidStrategy/InvalidOid。
 *
 * 若操作符对排序规则敏感，还须正确设置
 * sk_collation。
 *
 * ScanKey 也可表示 ScalarArrayOpExpr，即条件
 * 列 op ANY(数组)。由 SK_SEARCHARRAY
 * 标志位标示。此时 sk_argument 不是操作符右端
 * 参数类型的值，而是此类值的数组，且按元素
 * 比较结果按 OR 合并。
 *
 * ScanKey 也可表示条件"列 IS NULL"或"列
 * IS NOT NULL"；分别由 SK_SEARCHNULL 与
 * SK_SEARCHNOTNULL 标志位标示。参数恒为 NULL，
 * 且 sk_strategy、sk_subtype、sk_collation、sk_func
 * 不用(除非索引 AM 设置)。
 *
 * SK_SEARCHARRAY、SK_SEARCHNULL 与 SK_SEARCHNOTNULL
 * 仅支持索引扫描而非堆扫描；且并非所有索引 AM
 * 都支持，仅设了 amsearcharray 或 amsearchnulls 者。
 *
 * ScanKey 也可表示排序操作符调用，即
 * 排序要求"ORDER BY 索引列 op 常量"。外表
 * 与比较操作符相同，只是操作符通常并不
 * 返回布尔值。对此类 ScanKey 置 SK_ORDER_BY。
 * 此处无法用 SK_SEARCHARRAY、SK_SEARCHNULL、SK_SEARCHNOTNULL。
 *
 * 注意：有些地方用 ScanKey 作为调用访问方法
 * 支持过程的便利表示。此时
 * sk_strategy/sk_subtype 无意义（但 sk_collation 可用）；且
 * sk_func 可能指向返回非布尔值的函数。
 */
typedef struct ScanKeyData
{
	int			sk_flags;		/* 标志，见下 */
	AttrNumber	sk_attno;		/* 表或索引列号 */
	StrategyNumber sk_strategy; /* 操作符策略号 */
	Oid			sk_subtype;		/* 策略子类型 */
	Oid			sk_collation;	/* 排序规则，若需要 */
	FmgrInfo	sk_func;		/* 待调函数的查找信息 */
	Datum		sk_argument;	/* 比较用数据 */
} ScanKeyData;

typedef ScanKeyData *ScanKey;

/*
 * 关于行比较：
 *
 * ScanKey 结构也支持行比较，即有序
 * 元组比较如 (x, y) > (c1, c2)，语义符合 SQL 规范
 * "x > c1 OR (x = c1 AND y > c2)"。注意目前仅
 * 为 btree 索引搜索实现，不用于堆扫描或其他索引
 * 类型。行比较由一个"头" ScanKey 项加
 * 另设的 ScanKey 数组表示，行比较每列一项。
 * 头项具有下列性质：
 *	sk_flags = SK_ROW_HEADER
 *	sk_attno = 行比较首列的索引列号
 *	sk_strategy = 行比较语义的 btree 策略码
 *			(即 < <= > 或 >=)
 *	sk_subtype、sk_collation、sk_func：不用
 *	sk_argument：指向附属 ScanKey 数组
 * 若头项属于按 attno 排序的 ScanKey 数组，则须
 * 按首列列号排序。
 *
 * 附属 ScanKey 数组按行比较的逻辑列序
 * 出现，可能与索引列序不同。数组
 * 元素类似普通 ScanKey 数组，但：
 *		sk_flags 须含 SK_ROW_MEMBER，末项还须 SK_ROW_END
 *				(因行头不含计数)
 *		sk_func 指向该操作符类的 btree 比较支持函数，
 *				而非操作符的实现函数。
 * 附属数组所有元素的 sk_strategy 必须相同，
 * 即与头项相同。
 * 此处无法用 SK_SEARCHARRAY、SK_SEARCHNULL、SK_SEARCHNOTNULL。
 */

/*
 * ScanKeyData sk_flags
 *
 * sk_flags 的 0-15 位保留给系统范围使用（这些位的
 * 符号应在此定义）。16-31 位保留给
 * 各索引 AM 内部使用。
 */
#define SK_ISNULL			0x0001	/* 参数为 NULL */
#define SK_UNARY			0x0002	/* 一元操作符(不支持!) */
#define SK_ROW_HEADER		0x0004	/* 行比较头（见上文） */
#define SK_ROW_MEMBER		0x0008	/* 行比较成员（见上文） */
#define SK_ROW_END			0x0010	/* 行比较末成员 */
#define SK_SEARCHARRAY		0x0020	/* scankey 表示 ScalarArrayOp */
#define SK_SEARCHNULL		0x0040	/* 表示"列 IS NULL" */
#define SK_SEARCHNOTNULL	0x0080	/* 表示"列 IS NOT NULL" */
#define SK_ORDER_BY			0x0100	/* scankey 用于 ORDER BY op */


/*
 * access/common/scankey.c 中的函数原型
 */
extern void ScanKeyInit(ScanKey entry,
						AttrNumber attributeNumber,
						StrategyNumber strategy,
						RegProcedure procedure,
						Datum argument);
extern void ScanKeyEntryInitialize(ScanKey entry,
								   int flags,
								   AttrNumber attributeNumber,
								   StrategyNumber strategy,
								   Oid subtype,
								   Oid collation,
								   RegProcedure procedure,
								   Datum argument);
extern void ScanKeyEntryInitializeWithInfo(ScanKey entry,
										   int flags,
										   AttrNumber attributeNumber,
										   StrategyNumber strategy,
										   Oid subtype,
										   Oid collation,
										   FmgrInfo *finfo,
										   Datum argument);

#endif							/* SKEY_H */
