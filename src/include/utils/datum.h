/*-------------------------------------------------------------------------
 *
 * datum.h
 *	  POSTGRES Datum（抽象数据类型）操作例程。
 *
 * 这些例程由调用方先前已取得的 typbyval 与
 * typlen 信息驱动（按 Datum 的数据类型）。如
 * 此是因多数情况下调用方可只查一次信息，再用于
 * 许多逐 Datum 操作。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/datum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATUM_H
#define DATUM_H

/*
 * datumGetSize - 求 Datum 的"真实"长度
 */
extern Size datumGetSize(Datum value, bool typByVal, int typLen);

/*
 * datumCopy - 拷贝非 NULL Datum。
 *
 * 若数据类型按引用传递，用 palloc() 取得内存。
 */
extern Datum datumCopy(Datum value, bool typByVal, int typLen);

/*
 * datumTransfer - 把非 NULL Datum 转入当前内存上下文。
 *
 * 与 datumCopy() 在可读写扩展对象的处理上不同。
 */
extern Datum datumTransfer(Datum value, bool typByVal, int typLen);

/*
 * datumIsEqual
 * 两同类型 Datum 相等则返回 true，否则 false。
 *
 * XXX：限制见代码中的注释！
 */
extern bool datumIsEqual(Datum value1, Datum value2,
						 bool typByVal, int typLen);

/*
 * datum_image_eq
 *
 * 按字节映像比较两 Datum 内容是否相同。相等返回
 * true，否则 false。
 */
extern bool datum_image_eq(Datum value1, Datum value2,
						   bool typByVal, int typLen);

/*
 * datum_image_hash
 *
 * 基于位模式而非逻辑值，为 value 生成哈希
 * 值。
 */
extern uint32 datum_image_hash(Datum value, bool typByVal, int typLen);

/*
 * 序列化并恢复 Datum，以便传送给并行工作者。
 * 见上
 */
extern Size datumEstimateSpace(Datum value, bool isnull, bool typByVal,
							   int typLen);
extern void datumSerialize(Datum value, bool isnull, bool typByVal,
						   int typLen, char **start_address);
extern Datum datumRestore(char **start_address, bool *isnull);

#endif							/* DATUM_H */
