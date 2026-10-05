/*-------------------------------------------------------------------------
 *
 * indextuple.c
 *	   本文件包含索引元组的访问例程与修改例程，
 *	   以及各种元组实用函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/indextuple.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/itup.h"
#include "access/toast_internals.h"

/*
 * 启用索引项的还原。在 VACUUM 清理足以
 * 从头重建索引之前，仍需要这样做。
 */
#define TOAST_INDEX_HACK

/* ----------------------------------------------------------------
 *				  index_ 元组接口例程
 * ----------------------------------------------------------------
 */

 /* ----------------
  *		index_form_tuple
  *
  *		与 index_form_tuple_context 相同，但把返回的元组分配在
  *		CurrentMemoryContext 中。
  * ----------------
  */
IndexTuple
index_form_tuple(TupleDesc tupleDescriptor,
				 Datum *values,
				 bool *isnull)
{
	return index_form_tuple_context(tupleDescriptor, values, isnull,
									CurrentMemoryContext);
}

/* ----------------
 *		index_form_tuple_context
 *
 *		这里不应泄漏任何内存，否则诸如
 *		tuplesort_putindextuplevalues() 这样的调用方会非常不满。
 *
 *		只要调用方不传入以 EXTERNAL 方式存储的值，
 *		这里就不应访问外部表。
 *
 *		在给定的 'context' 中分配返回的元组。
 * ----------------
 */
IndexTuple
index_form_tuple_context(TupleDesc tupleDescriptor,
						 Datum *values,
						 bool *isnull,
						 MemoryContext context)
{
	char	   *tp;				/* 元组指针 */
	IndexTuple	tuple;			/* 返回的元组 */
	Size		size,
				data_size,
				hoff;
	int			i;
	unsigned short infomask = 0;
	bool		hasnull = false;
	uint16		tupmask = 0;
	int			numberOfAttributes = tupleDescriptor->natts;

#ifdef TOAST_INDEX_HACK
	Datum		untoasted_values[INDEX_MAX_KEYS];
	bool		untoasted_free[INDEX_MAX_KEYS];
#endif

	if (numberOfAttributes > INDEX_MAX_KEYS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of index columns (%d) exceeds limit (%d)",
						numberOfAttributes, INDEX_MAX_KEYS)));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDescriptor, i);

		untoasted_values[i] = values[i];
		untoasted_free[i] = false;

		/* 值为 NULL 或不是 varlena 类型时什么都不做 */
		if (isnull[i] || att->attlen != -1)
			continue;

		/*
		 * 若值以 EXTERNAL 方式存储，必须取回它，这样才不依赖
		 * 外部存储。将来应当改进这一点。
		 */
		if (VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
		{
			untoasted_values[i] =
				PointerGetDatum(detoast_external_attr((struct varlena *)
													  DatumGetPointer(values[i])));
			untoasted_free[i] = true;
		}

		/*
		 * 若值超过目标大小，且数据类型可压缩，
		 * 就尝试就地压缩。
		 */
		if (!VARATT_IS_EXTENDED(DatumGetPointer(untoasted_values[i])) &&
			VARSIZE(DatumGetPointer(untoasted_values[i])) > TOAST_INDEX_TARGET &&
			(att->attstorage == TYPSTORAGE_EXTENDED ||
			 att->attstorage == TYPSTORAGE_MAIN))
		{
			Datum		cvalue;

			cvalue = toast_compress_datum(untoasted_values[i],
										  att->attcompression);

			if (DatumGetPointer(cvalue) != NULL)
			{
				/* 压缩成功 */
				if (untoasted_free[i])
					pfree(DatumGetPointer(untoasted_values[i]));
				untoasted_values[i] = cvalue;
				untoasted_free[i] = true;
			}
		}
	}
#endif

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	if (hasnull)
		infomask |= INDEX_NULL_MASK;

	hoff = IndexInfoFindDataOffset(infomask);
#ifdef TOAST_INDEX_HACK
	data_size = heap_compute_data_size(tupleDescriptor,
									   untoasted_values, isnull);
#else
	data_size = heap_compute_data_size(tupleDescriptor,
									   values, isnull);
#endif
	size = hoff + data_size;
	size = MAXALIGN(size);		/* 取得保守一些 */

	tp = (char *) MemoryContextAllocZero(context, size);
	tuple = (IndexTuple) tp;

	heap_fill_tuple(tupleDescriptor,
#ifdef TOAST_INDEX_HACK
					untoasted_values,
#else
					values,
#endif
					isnull,
					(char *) tp + hoff,
					data_size,
					&tupmask,
					(hasnull ? (bits8 *) tp + sizeof(IndexTupleData) : NULL));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (untoasted_free[i])
			pfree(DatumGetPointer(untoasted_values[i]));
	}
#endif

	/*
	 * 这样做是因为 heap_fill_tuple 要初始化一个供
	 * HeapTuples 使用的 "tupmask"，而我们要的是索引元组 infomask。唯一
	 * 有关的信息是 "has variable attributes" 字段。上面已经
	 * 设置过 hasnull 位。
	 */
	if (tupmask & HEAP_HASVARWIDTH)
		infomask |= INDEX_VAR_MASK;

	/* 同时断言外部属性已被去掉 */
#ifdef TOAST_INDEX_HACK
	Assert((tupmask & HEAP_HASEXTERNAL) == 0);
#endif

	/*
	 * 这里确认大小放得进为它保留的字段，
	 * 该字段位于 t_info 中。
	 */
	if ((size & INDEX_SIZE_MASK) != size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row requires %zu bytes, maximum size is %zu",
						size, (Size) INDEX_SIZE_MASK)));

	infomask |= size;

	/*
	 * 初始化元数据
	 */
	tuple->t_info = infomask;
	return tuple;
}

/* ----------------
 *		nocache_index_getattr
 *
 *		由 index_getattr() 宏调用，而且只在无法使用
 *		cacheoffset 且该值非空的情况下。
 *
 *		它把属性偏移缓存在属性描述符中。
 *
 *		另一种加速办法是把偏移随元组一起缓存，但那似乎更难，
 *		除非真把这些偏移写进送往磁盘的元组，并承担那份
 *		存储开销。
 *		真难看。
 *
 *		本方案会比那种略慢一些，但对命中大量元组的查询
 *		应当表现良好。把偏移缓存一次之后，再用同一个
 *		属性描述符检查其余所有元组，
 *		就会快得多。-cim 5/4/91
 * ----------------
 */
Datum
nocache_index_getattr(IndexTuple tup,
					  int attnum,
					  TupleDesc tupleDesc)
{
	char	   *tp;				/* 指向元组数据部分的指针 */
	bits8	   *bp = NULL;		/* 指向元组中空值位图的指针 */
	bool		slow = false;	/* 是否必须逐个属性遍历？ */
	int			data_off;		/* 元组数据偏移 */
	int			off;			/* 数据内的当前偏移 */

	/* ----------------
	 *	 三种情形：
	 *
	 *	 1：没有空值，也没有变长属性。
	 *	 2：在 att 之后（AFTER）有空值或变长。
	 *	 3：在 att 之前（BEFORE）有空值或变长。
	 * ----------------
	 */

	data_off = IndexInfoFindDataOffset(tup->t_info);

	attnum--;

	if (IndexTupleHasNulls(tup))
	{
		/*
		 * 元组中某处有空值
		 *
		 * 检查目标 att 是否为空
		 */

		/* XXX 这里 "知道" t_bits 紧跟在固定元组头之后！ */
		bp = (bits8 *) ((char *) tup + sizeof(IndexTupleData));

		/*
		 * 现在检查前面是否有任何位为空...
		 */
		{
			int			byte = attnum >> 3;
			int			finalbit = attnum & 0x07;

			/* 检查最后一个字节的最终位 "before" 是否有空值 */
			if ((~bp[byte]) & ((1 << finalbit) - 1))
				slow = true;
			else
			{
				/* 检查任何 "earlier" 字节中是否有空值 */
				int			i;

				for (i = 0; i < byte; i++)
				{
					if (bp[i] != 0xFF)
					{
						slow = true;
						break;
					}
				}
			}
		}
	}

	tp = (char *) tup + data_off;

	if (!slow)
	{
		Form_pg_attribute att;

		/*
		 * 若走到这里，直到目标属性（含）都没有空值。
		 * 若已有缓存的偏移，就可以用它。
		 */
		att = TupleDescAttr(tupleDesc, attnum);
		if (att->attcacheoff >= 0)
			return fetchatt(att, tp + att->attcacheoff);

		/*
		 * 否则，检查直到目标（含）是否有非定长属性。
		 * 如果没有，就可以低成本地初始化这些属性
		 * 已缓存的偏移。
		 */
		if (IndexTupleHasVarwidths(tup))
		{
			int			j;

			for (j = 0; j <= attnum; j++)
			{
				if (TupleDescAttr(tupleDesc, j)->attlen <= 0)
				{
					slow = true;
					break;
				}
			}
		}
	}

	if (!slow)
	{
		int			natts = tupleDesc->natts;
		int			j = 1;

		/*
		 * 若走到这里，直到目标属性（含）的元组都没有空值或变长，
		 * 因此可以用已缓存的偏移
		 * ……只不过还没有这份偏移，否则也不会走到这里。既然
		 * 计算定长列的偏移并不贵，就趁机
		 * 为前面*所有*前导定长列初始化已缓存的偏移，
		 * 希望以后不必再进入这个
		 * 例程。
		 */
		TupleDescAttr(tupleDesc, 0)->attcacheoff = 0;

		/* 慢路径上先前可能已经设置过一些偏移 */
		while (j < natts && TupleDescAttr(tupleDesc, j)->attcacheoff > 0)
			j++;

		off = TupleDescAttr(tupleDesc, j - 1)->attcacheoff +
			TupleDescAttr(tupleDesc, j - 1)->attlen;

		for (; j < natts; j++)
		{
			Form_pg_attribute att = TupleDescAttr(tupleDesc, j);

			if (att->attlen <= 0)
				break;

			off = att_align_nominal(off, att->attalign);

			att->attcacheoff = off;

			off += att->attlen;
		}

		Assert(j > attnum);

		off = TupleDescAttr(tupleDesc, attnum)->attcacheoff;
	}
	else
	{
		bool		usecache = true;
		int			i;

		/*
		 * 现在我们知道必须 CAREFULLY 地遍历这个元组。但下次
		 * 仍可能缓存一些偏移。
		 *
		 * 注意：这个循环有点绕。对每个非空属性，先计入
		 * 该属性之前的对齐填充，再按其长度跨过它。
		 * 空值既没有存储，也没有对齐填充。在遇到空值
		 * 或变长属性之前，都可以使用或设置
		 * attcacheoff。
		 */
		off = 0;
		for (i = 0;; i++)		/* 循环在 "break" 处退出 */
		{
			Form_pg_attribute att = TupleDescAttr(tupleDesc, i);

			if (IndexTupleHasNulls(tup) && att_isnull(i, bp))
			{
				usecache = false;
				continue;		/* 这无法成为目标 att */
			}

			/* 若已知下一偏移，就可以跳过其余部分 */
			if (usecache && att->attcacheoff >= 0)
				off = att->attcacheoff;
			else if (att->attlen == -1)
			{
				/*
				 * 只有当偏移已经适当对齐，因而任何情况下
				 * 都不会出现填充字节时，才能缓存 varlena
				 * 属性的偏移：此时该偏移对对齐或未对齐的
				 * 值都有效。
				 */
				if (usecache &&
					off == att_align_nominal(off, att->attalign))
					att->attcacheoff = off;
				else
				{
					off = att_align_pointer(off, att->attalign, -1,
											tp + off);
					usecache = false;
				}
			}
			else
			{
				/* 不是 varlena，因此可以安全使用 att_align_nominal */
				off = att_align_nominal(off, att->attalign);

				if (usecache)
					att->attcacheoff = off;
			}

			if (i == attnum)
				break;

			off = att_addlength_pointer(off, att->attlen, tp + off);

			if (usecache && att->attlen <= 0)
				usecache = false;
		}
	}

	return fetchatt(TupleDescAttr(tupleDesc, attnum), tp + off);
}

/*
 * 把索引元组转换成 Datum/isnull 数组。
 *
 * 调用方必须为输出数组分配足够的存储。
 * （INDEX_MAX_KEYS 项应该就够了。）
 *
 * 这与 heap_deform_tuple() 几乎相同，不过针对的是 IndexTuples。
 * 一处差别是：元组绝不应该有任何缺少的列。
 */
void
index_deform_tuple(IndexTuple tup, TupleDesc tupleDescriptor,
				   Datum *values, bool *isnull)
{
	char	   *tp;				/* 指向元组数据的指针 */
	bits8	   *bp;				/* 指向元组中空值位图的指针 */

	/* XXX 这里 "知道" t_bits 紧跟在固定元组头之后！ */
	bp = (bits8 *) ((char *) tup + sizeof(IndexTupleData));

	tp = (char *) tup + IndexInfoFindDataOffset(tup->t_info);

	index_deform_tuple_internal(tupleDescriptor, values, isnull,
								tp, bp, IndexTupleHasNulls(tup));
}

/*
 * 把索引元组转换成 Datum/isnull 数组，
 * 不假定索引元组头有任何特定布局。
 *
 * 调用方必须提供数据区指针、空值位图指针
 * （若 !hasnulls 则可以为 NULL）以及 hasnulls 标志。
 */
void
index_deform_tuple_internal(TupleDesc tupleDescriptor,
							Datum *values, bool *isnull,
							char *tp, bits8 *bp, int hasnulls)
{
	int			natts = tupleDescriptor->natts; /* 要抽出的属性数 */
	int			attnum;
	int			off = 0;		/* 元组数据中的偏移 */
	bool		slow = false;	/* 能否使用或设置 attcacheoff？ */

	/* 断言，以保护分配了定长数组的调用方 */
	Assert(natts <= INDEX_MAX_KEYS);

	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = TupleDescAttr(tupleDescriptor, attnum);

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* 此后不能再用 attcacheoff */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else if (thisatt->attlen == -1)
		{
			/*
			 * 只有当偏移已经适当对齐，因而任何情况下
			 * 都不会出现填充字节，才能缓存该 varlena 属性的偏移：
			 * 这样该偏移对对齐或未对齐的值
			 * 都有效。
			 */
			if (!slow &&
				off == att_align_nominal(off, thisatt->attalign))
				thisatt->attcacheoff = off;
			else
			{
				off = att_align_pointer(off, thisatt->attalign, -1,
										tp + off);
				slow = true;
			}
		}
		else
		{
			/* 不是 varlena，因此可以安全使用 att_align_nominal */
			off = att_align_nominal(off, thisatt->attalign);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength_pointer(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* 此后不能再用 attcacheoff */
	}
}

/*
 * 创建索引元组的一份 palloc 副本。
 */
IndexTuple
CopyIndexTuple(IndexTuple source)
{
	IndexTuple	result;
	Size		size;

	size = IndexTupleSize(source);
	result = (IndexTuple) palloc(size);
	memcpy(result, source, size);
	return result;
}

/*
 * 创建索引元组的一份 palloc 副本，只留下前
 * leavenatts 个属性。
 *
 * 截断保证得到的索引元组不会比原来更大。可以拿这份
 * IndexTuple 配原来的元组描述符使用，但调用方必须避免
 * 真正访问返回元组里已被截断的属性！实践中，这意味着
 * 调用 index_getattr() 必须特别小心，而且被截断的元组
 * 只应由处于调用方直接控制之下的代码
 * 访问。
 * 调用方须自行守住这条边界。
 *
 * 持有缓冲锁时调用本函数是安全的，因为它从不
 * 做外部表访问。万一索引元组将来可能包含
 * EXTERNAL TOAST 值，就得重新审视
 * 这一点。
 */
IndexTuple
index_truncate_tuple(TupleDesc sourceDescriptor, IndexTuple source,
					 int leavenatts)
{
	TupleDesc	truncdesc;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	truncated;

	Assert(leavenatts <= sourceDescriptor->natts);

	/* 简单情形：实际上不需要截断 */
	if (leavenatts == sourceDescriptor->natts)
		return CopyIndexTuple(source);

	/* 创建一份可供临时改写的临时描述符 */
	truncdesc = palloc(TupleDescSize(sourceDescriptor));
	TupleDescCopy(truncdesc, sourceDescriptor);
	truncdesc->natts = leavenatts;

	/* 拆开再组装一份属性更少的元组副本 */
	index_deform_tuple(source, truncdesc, values, isnull);
	truncated = index_form_tuple(truncdesc, values, isnull);
	truncated->t_tid = source->t_tid;
	Assert(IndexTupleSize(truncated) <= IndexTupleSize(source));

	/*
	 * 这里无法泄漏内存。TupleDescCopy() 不分配任何内部
	 * 结构，因此普通的 pfree() 就能清掉全部分配的内存
	 */
	pfree(truncdesc);

	return truncated;
}
