/*-------------------------------------------------------------------------
 *
 * htup.h
 *	POSTGRES 堆元组定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_H
#define HTUP_H

#include "storage/itemptr.h"

/* htup_details.h 中所定义结构的类型定义与前向声明 */

typedef struct HeapTupleHeaderData HeapTupleHeaderData;

typedef HeapTupleHeaderData *HeapTupleHeader;

typedef struct MinimalTupleData MinimalTupleData;

typedef MinimalTupleData *MinimalTuple;


/*
 * HeapTupleData 是指向元组的内存数据结构。
 *
 * 该数据结构有若干种用法：
 *
 * * 指向磁盘缓冲区内元组的指针：t_data 直接指向
 *	缓冲区（代码最好已钉住它，但这点
 *	HeapTupleData 本身不体现）。
 *
 * * 指向空：t_data 为 NULL。某些函数用它作
 *	失败指示。
 *
 * * palloc 分配元组的一部分：HeapTupleData 本身与元组
 *	构成单个 palloc 分配块。t_data 指向
 *	紧接 HeapTupleData 结构之后的位置（偏移 HEAPTUPLESIZE）。
 *	这是 heap_form_tuple 及相关例程的输出格式。
 *
 * * 单独分配的元组：t_data 指向 palloc 分配的内存块，
 *	该块与 HeapTupleData 不相邻。（此情形已弃用，因其
 *	难与情形 #1 区分。仅应在代码确知情形 #1
 *	绝不适用的有限场合使用。）
 *
 * * 单独分配的最小元组：t_data 指向 MinimalTuple 起点
 *	之前 MINIMAL_TUPLE_OFFSET 字节处。与上一情形相同，
 *	此情形也无法靠检查与情形 #1 区分；建立或销毁
 *	此表示的代码须清楚自己在做什么。
 *
 * t_len 应始终有效，指向空的情形除外。
 * t_self 和 t_tableOid 在 HeapTupleData 指向
 * 磁盘缓冲区，或表示磁盘上元组的副本时应有效。
 * 手工构造的元组中应显式将其置为无效。
 */
typedef struct HeapTupleData
{
	uint32		t_len;			/* *t_data 的长度 */
	ItemPointerData t_self;		/* SelfItemPointer */
	Oid			t_tableOid;		/* 元组所属的表 */
#define FIELDNO_HEAPTUPLEDATA_DATA 3
	HeapTupleHeader t_data;		/* -> 元组头与数据 */
} HeapTupleData;

typedef HeapTupleData *HeapTuple;

#define HEAPTUPLESIZE	MAXALIGN(sizeof(HeapTupleData))

/*
 * 与 HeapTuple 指针配合使用的访问宏。
 */
#define HeapTupleIsValid(tuple) PointerIsValid(tuple)

/* utils/time/combocid.c 中实现的 HeapTupleHeader 函数 */
extern CommandId HeapTupleHeaderGetCmin(HeapTupleHeader tup);
extern CommandId HeapTupleHeaderGetCmax(HeapTupleHeader tup);
extern void HeapTupleHeaderAdjustCmax(HeapTupleHeader tup,
									  CommandId *cmax, bool *iscombo);

/* heapam.c 中 HeapTupleHeader 访问函数的原型 */
extern TransactionId HeapTupleGetUpdateXid(HeapTupleHeader tuple);

#endif							/* HTUP_H */
