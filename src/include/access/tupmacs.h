/*-------------------------------------------------------------------------
 *
 * tupmacs.h
 *	  索引元组与堆元组共用的元组宏。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupmacs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPMACS_H
#define TUPMACS_H

#include "catalog/pg_type_d.h"	/* 供 TYPALIGN 宏用 */


/*
 * 检查元组的空值位图，以判定该属性是否为空。
 * 注意空值位图中 0 表示空，而 1 表示
 * 非空。
 */
static inline bool
att_isnull(int ATT, const bits8 *BITS)
{
	return !(BITS[ATT >> 3] & (1 << (ATT & 0x07)));
}

#ifndef FRONTEND
/*
 * 给定 Form_pg_attribute 与指向元组数据区的指针，
 * 返回正确的值或指针。
 *
 * 所有情况均返回 Datum。若属性 "byval" 为假，
 * 则返回传入的、指向元组数据区的同一指针。
 * 否则返回从数据区取出的正确字节数，
 * 并扩展为 Datum 形式。
 *
 * 在 Datum 为 8 字节的机器上，支持取 8 字节按值
 * 属性；否则仅支持 1、2、4 字节的值。
 *
 * 注意 T 必须已正确对齐，本操作才能正确工作。
 */
#define fetchatt(A,T) fetch_att(T, (A)->attbyval, (A)->attlen)

/*
 * 同上，但依据 byval/len 参数而非 Form_pg_attribute。
 */
static inline Datum
fetch_att(const void *T, bool attbyval, int attlen)
{
	if (attbyval)
	{
		switch (attlen)
		{
			case sizeof(char):
				return CharGetDatum(*((const char *) T));
			case sizeof(int16):
				return Int16GetDatum(*((const int16 *) T));
			case sizeof(int32):
				return Int32GetDatum(*((const int32 *) T));
#if SIZEOF_DATUM == 8
			case sizeof(Datum):
				return *((const Datum *) T);
#endif
			default:
				elog(ERROR, "unsupported byval length: %d", attlen);
				return 0;
		}
	}
	else
		return PointerGetDatum(T);
}
#endif							/* FRONTEND */

/*
 * att_align_datum 按对齐要求 attalign 与 typlen
 * attlen，对给定偏移做对齐。attdatum 是拟打入
 * 元组的 Datum 变量（仅当处理变长类型时才
 * 访问）。注意这假定 Datum 将按原样存储；
 * 打算把非短变长值转为短格式的调用方，
 * 须自行计入该差异。
 */
#define att_align_datum(cur_offset, attalign, attlen, attdatum) \
( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * att_align_pointer 做与 att_align_datum 相同的
 * 计算，但用于遍历元组。attptr 是当前实际数据
 * 指针；访问变长字段时须"窥视"以判断所见是
 * 填充字节，还是单字节头 datum 的首字节。
 * （零字节必为填充字节，或已正确对齐的 4 字节
 * 长度字之首字节；两种情形均可安全对齐。非零
 * 字节必为单字节长度字，或已正确对齐的 4 字节
 * 长度字之首字节；两种情形都无需对齐。）
 *
 * 注意：有的调用方给 cur_offset 传入 "char *"
 * 指针。这有点取巧，但只要 uintptr_t 宽度正确
 * 即可。
 */
#define att_align_pointer(cur_offset, attalign, attlen, attptr) \
( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * att_align_nominal 按对齐要求 attalign 对给定
 * 偏移做对齐，不考虑已打包变长值的任何因素。
 * 直接使用本宏主要有三种情形：
 *	* 已知该属性不是变长（attlen != -1）；
 *	  此时比上述宏更便宜，效果却一样好。
 *	* 需要抽象地估算对齐填充代价，即不参照
 *	  真实元组。必须按最坏情形假定所有
 *	  变长值均已对齐。
 *	* 在数组与多范围中，无条件对齐变长值（XXX 这
 *	  大概应再审视）。
 *
 * 按希望接近实际出现频率的次序，检测各
 * attalign 情形。
 */
#define att_align_nominal(cur_offset, attalign) \
( \
	((attalign) == TYPALIGN_INT) ? INTALIGN(cur_offset) : \
	 (((attalign) == TYPALIGN_CHAR) ? (uintptr_t) (cur_offset) : \
	  (((attalign) == TYPALIGN_DOUBLE) ? DOUBLEALIGN(cur_offset) : \
	   ( \
			AssertMacro((attalign) == TYPALIGN_SHORT), \
			SHORTALIGN(cur_offset) \
	   ))) \
)

/*
 * att_addlength_datum 把给定偏移加上存放该
 * Datum 变量所需空间。仅当处理变长属性时才
 * 访问 attdatum。
 */
#define att_addlength_datum(cur_offset, attlen, attdatum) \
	att_addlength_pointer(cur_offset, attlen, DatumGetPointer(attdatum))

/*
 * att_addlength_pointer 做与 att_addlength_datum
 * 相同的计算，但用于遍历元组。attptr 指向元组内
 * 字段。
 *
 * 注意：有的调用方给 cur_offset 传入 "char *"
 * 指针。这其实完全没问题，但大概应连同
 * att_align_pointer 同样做法清理。
 */
#define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
	((attlen) > 0) ? \
	( \
		(cur_offset) + (attlen) \
	) \
	: (((attlen) == -1) ? \
	( \
		(cur_offset) + VARSIZE_ANY(attptr) \
	) \
	: \
	( \
		AssertMacro((attlen) == -2), \
		(cur_offset) + (strlen((char *) (attptr)) + 1) \
	)) \
)

#ifndef FRONTEND
/*
 * store_att_byval 是 fetch_att 的部分逆操作：把
 * 给定 Datum 值存入指定地址处的元组数据区。
 * 但它只处理按值情形，因典型用法下调用方本就
 * 须区分按值与按引用，故一揽子函数
 * 反而不便。
 */
static inline void
store_att_byval(void *T, Datum newdatum, int attlen)
{
	switch (attlen)
	{
		case sizeof(char):
			*(char *) T = DatumGetChar(newdatum);
			break;
		case sizeof(int16):
			*(int16 *) T = DatumGetInt16(newdatum);
			break;
		case sizeof(int32):
			*(int32 *) T = DatumGetInt32(newdatum);
			break;
#if SIZEOF_DATUM == 8
		case sizeof(Datum):
			*(Datum *) T = newdatum;
			break;
#endif
		default:
			elog(ERROR, "unsupported byval length: %d", attlen);
	}
}
#endif							/* FRONTEND */

#endif							/* TUPMACS_H */
