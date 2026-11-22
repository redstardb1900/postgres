/*-------------------------------------------------------------------------
 *
 * arrayaccess.h
 *	  逐元素访问 Postgres 数组的声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/arrayaccess.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ARRAYACCESS_H
#define ARRAYACCESS_H

#include "access/tupmacs.h"
#include "utils/array.h"


/*
 * 遍历扁平或扩展数组元素的函数。需要状态结构
 * "array_iter iter"。
 *
 * 用 array_iter_setup(&iter, arrayptr) 准备，并用
 * array_iter_next(...) 取下一元素到
 * datumvar/isnullvar。
 * "index" 须为零基元素号；由调用方提供，因调用方
 * 通常本就在计数。尽管如此，这些函数只能顺序取元
 * 素。
 */

typedef struct array_iter
{
	/* datumptr 是否为 NULL 表明是扁平还是扩展数组 */

	/* 有扩展数组时使用的字段 */
	Datum	   *datumptr;		/* 指向 Datum 数组 */
	bool	   *isnullptr;		/* 指向 isnull 数组 */

	/* 有扁平数组时使用的字段 */
	char	   *dataptr;		/* 数据区中的当前位置 */
	bits8	   *bitmapptr;		/* 空值位图当前字节，或 NULL */
	int			bitmask;		/* 空值位图中当前位的掩码 */
} array_iter;


static inline void
array_iter_setup(array_iter *it, AnyArrayType *a)
{
	if (VARATT_IS_EXPANDED_HEADER(a))
	{
		if (a->xpn.dvalues)
		{
			it->datumptr = a->xpn.dvalues;
			it->isnullptr = a->xpn.dnulls;
			/* 须填满所有字段以防编译器警告 */
			it->dataptr = NULL;
			it->bitmapptr = NULL;
		}
		else
		{
			/* 处理嵌在扩展 datum 中的扁平数组 */
			it->datumptr = NULL;
			it->isnullptr = NULL;
			it->dataptr = ARR_DATA_PTR(a->xpn.fvalue);
			it->bitmapptr = ARR_NULLBITMAP(a->xpn.fvalue);
		}
	}
	else
	{
		it->datumptr = NULL;
		it->isnullptr = NULL;
		it->dataptr = ARR_DATA_PTR((ArrayType *) a);
		it->bitmapptr = ARR_NULLBITMAP((ArrayType *) a);
	}
	it->bitmask = 1;
}

static inline Datum
array_iter_next(array_iter *it, bool *isnull, int i,
				int elmlen, bool elmbyval, char elmalign)
{
	Datum		ret;

	if (it->datumptr)
	{
		ret = it->datumptr[i];
		*isnull = it->isnullptr ? it->isnullptr[i] : false;
	}
	else
	{
		if (it->bitmapptr && (*(it->bitmapptr) & it->bitmask) == 0)
		{
			*isnull = true;
			ret = (Datum) 0;
		}
		else
		{
			*isnull = false;
			ret = fetch_att(it->dataptr, elmbyval, elmlen);
			it->dataptr = att_addlength_pointer(it->dataptr, elmlen,
												it->dataptr);
			it->dataptr = (char *) att_align_nominal(it->dataptr, elmalign);
		}
		it->bitmask <<= 1;
		if (it->bitmask == 0x100)
		{
			if (it->bitmapptr)
				it->bitmapptr++;
			it->bitmask = 1;
		}
	}

	return ret;
}

#endif							/* ARRAYACCESS_H */
