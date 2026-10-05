/*
 * brin_tuple.c
 *		BRIN 索引中元组的方法实现。
 *
 * 预期用法是：本文件以外的代码只处理
 * BrinMemTuples，并通过本文件中的函数
 * 与磁盘上的表示互相转换。
 *
 * NOTES
 *
 * BRIN 元组类似于堆元组，但有几处关键差别。
 * 首先一个有趣的差别是元组头部简单得多，只
 * 含有总长度和一小块标志区域。另外，所存储的
 * 数据与关系元组描述符并不完全一致：对于描述符中的每个
 * 属性，索引元组携带任意多个
 * 值，具体取决于操作符类。
 *
 * 另外，索引关系的每一列都有两个空值位：一个
 * （hasnulls）表示该页范围内是否有任何元组的该列
 * 被置为空值；另一个（allnulls）表示该列的值是否
 * 全部为空值。若 allnulls 为真，则元组数据区完全不含
 * 该列的值；而若设置了 hasnulls，则包含这些值。
 * 注意空位掩码的大小可能与
 * datum 数组的大小不同。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_tuple.c
 */
#include "postgres.h"

#include "access/brin_tuple.h"
#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/toast_internals.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "utils/datum.h"
#include "utils/memutils.h"


/*
 * 启用对索引项解除 TOAST。在 VACUUM
 * 聪明到足以从头重建索引之前，这是需要的。
 */
#define TOAST_INDEX_HACK


static inline void brin_deconstruct_tuple(BrinDesc *brdesc,
										  char *tp, bits8 *nullbits, bool nulls,
										  Datum *values, bool *allnulls, bool *hasnulls);


/*
 * 返回用于 BRIN 元组磁盘存储的元组描述符。
 */
static TupleDesc
brtuple_disk_tupdesc(BrinDesc *brdesc)
{
	/* 我们把它们缓存在 BrinDesc 中 */
	if (brdesc->bd_disktdesc == NULL)
	{
		int			i;
		int			j;
		AttrNumber	attno = 1;
		TupleDesc	tupdesc;
		MemoryContext oldcxt;

		/* 确保它位于 bdesc 的上下文中 */
		oldcxt = MemoryContextSwitchTo(brdesc->bd_context);

		tupdesc = CreateTemplateTupleDesc(brdesc->bd_totalstored);

		for (i = 0; i < brdesc->bd_tupdesc->natts; i++)
		{
			for (j = 0; j < brdesc->bd_info[i]->oi_nstored; j++)
				TupleDescInitEntry(tupdesc, attno++, NULL,
								   brdesc->bd_info[i]->oi_typcache[j]->type_id,
								   -1, 0);
		}

		MemoryContextSwitchTo(oldcxt);

		brdesc->bd_disktdesc = tupdesc;
	}

	return brdesc->bd_disktdesc;
}

/*
 * 生成一个要插入 BRIN 索引的新磁盘元组。
 *
 * 若改动这里，请参见 brin_form_placeholder_tuple。
 */
BrinTuple *
brin_form_tuple(BrinDesc *brdesc, BlockNumber blkno, BrinMemTuple *tuple,
				Size *size)
{
	Datum	   *values;
	bool	   *nulls;
	bool		anynulls = false;
	BrinTuple  *rettuple;
	int			keyno;
	int			idxattno;
	uint16		phony_infomask = 0;
	bits8	   *phony_nullbitmap;
	Size		len,
				hoff,
				data_len;
	int			i;

#ifdef TOAST_INDEX_HACK
	Datum	   *untoasted_values;
	int			nuntoasted = 0;
#endif

	Assert(brdesc->bd_totalstored > 0);

	values = (Datum *) palloc(sizeof(Datum) * brdesc->bd_totalstored);
	nulls = (bool *) palloc0(sizeof(bool) * brdesc->bd_totalstored);
	phony_nullbitmap = (bits8 *)
		palloc(sizeof(bits8) * BITMAPLEN(brdesc->bd_totalstored));

#ifdef TOAST_INDEX_HACK
	untoasted_values = (Datum *) palloc(sizeof(Datum) * brdesc->bd_totalstored);
#endif

	/*
	 * 为 heap_fill_tuple 设置 values/nulls 数组
	 */
	idxattno = 0;
	for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
	{
		int			datumno;

		/*
		 * 当该列的任何行中都没有非空值时设置 "allnulls"；
		 * 此时没有数据可存。因此把该列全部数据元素的
		 * 可空位置位，然后就完成了。
		 */
		if (tuple->bt_columns[keyno].bv_allnulls)
		{
			for (datumno = 0;
				 datumno < brdesc->bd_info[keyno]->oi_nstored;
				 datumno++)
				nulls[idxattno++] = true;
			anynulls = true;
			continue;
		}

		/*
		 * 数据中存在一些空值时设置 "hasnulls" 位。
		 * 我们仍需存储一个真实值，但该位的存在
		 * 意味着我们需要空位掩码。
		 */
		if (tuple->bt_columns[keyno].bv_hasnulls)
			anynulls = true;

		/* 若需要，在形成磁盘元组之前先把值序列化。 */
		if (tuple->bt_columns[keyno].bv_serialize)
		{
			tuple->bt_columns[keyno].bv_serialize(brdesc,
												  tuple->bt_columns[keyno].bv_mem_value,
												  tuple->bt_columns[keyno].bv_values);
		}

		/*
		 * 现在取得每个已存储 datum 的值。注意有些值
		 * 可能已被 TOAST，而且无法依赖原始堆值
		 * 永远留在原地，因此必须对它们解除 TOAST。并尝试
		 * 压缩它们。
		 */
		for (datumno = 0;
			 datumno < brdesc->bd_info[keyno]->oi_nstored;
			 datumno++)
		{
			Datum		value = tuple->bt_columns[keyno].bv_values[datumno];

#ifdef TOAST_INDEX_HACK

			/* 必须看存储类型，而不是索引描述符。 */
			TypeCacheEntry *atttype = brdesc->bd_info[keyno]->oi_typcache[datumno];

			/* 结束时是否需要释放该值？ */
			bool		free_value = false;

			/* 对于非 varlena 类型，不需要做任何特殊处理 */
			if (atttype->typlen != -1)
			{
				values[idxattno++] = value;
				continue;
			}

			/*
			 * 若值不是 varlena 类型，则什么也不做。这里不必
			 * 关心 NULL 值，这要归功于上面的 bv_allnulls。
			 *
			 * 若值以 EXTERNAL 方式存储，就必须取出来，这样我们才不
			 * 依赖外部存储。
			 *
			 * XXX 这真的成立吗？会不会即使范围内有非 NULL 数据，
			 * 汇总仍为 NULL？例如退化的 bloom 过滤器
			 * 可能被丢弃，等等。
			 */
			if (VARATT_IS_EXTERNAL(DatumGetPointer(value)))
			{
				value = PointerGetDatum(detoast_external_attr((struct varlena *)
															  DatumGetPointer(value)));
				free_value = true;
			}

			/*
			 * 若值超过大小目标，且数据类型可压缩，
			 * 则尝试内联压缩它。
			 */
			if (!VARATT_IS_EXTENDED(DatumGetPointer(value)) &&
				VARSIZE(DatumGetPointer(value)) > TOAST_INDEX_TARGET &&
				(atttype->typstorage == TYPSTORAGE_EXTENDED ||
				 atttype->typstorage == TYPSTORAGE_MAIN))
			{
				Datum		cvalue;
				char		compression;
				Form_pg_attribute att = TupleDescAttr(brdesc->bd_tupdesc,
													  keyno);

				/*
				 * 若 BRIN 汇总与被索引属性使用同一数据
				 * 类型，且它有有效的压缩方法，就可以使用
				 * 同一压缩方法。否则就必须使用
				 * 默认方法。
				 */
				if (att->atttypid == atttype->type_id)
					compression = att->attcompression;
				else
					compression = InvalidCompressionMethod;

				cvalue = toast_compress_datum(value, compression);

				if (DatumGetPointer(cvalue) != NULL)
				{
					/* 压缩成功 */
					if (free_value)
						pfree(DatumGetPointer(value));

					value = cvalue;
					free_value = true;
				}
			}

			/*
			 * 若我们解除了 TOAST 或压缩了该值，就需要在
			 * 形成索引元组之后释放它。
			 */
			if (free_value)
				untoasted_values[nuntoasted++] = value;

#endif

			values[idxattno++] = value;
		}
	}

	/* 断言没有越出临时数组 */
	Assert(idxattno <= brdesc->bd_totalstored);

	/* 计算所需的总空间 */
	len = SizeOfBrinTuple;
	if (anynulls)
	{
		/*
		 * 磁盘上的 BRIN 索引元组需要双倍长度的位图；
		 * 前半存放 "allnulls" 位，后半存放
		 * "hasnulls"。
		 */
		len += BITMAPLEN(brdesc->bd_tupdesc->natts * 2);
	}

	len = hoff = MAXALIGN(len);

	data_len = heap_compute_data_size(brtuple_disk_tupdesc(brdesc),
									  values, nulls);
	len += data_len;

	len = MAXALIGN(len);

	rettuple = palloc0(len);
	rettuple->bt_blkno = blkno;
	rettuple->bt_info = hoff;

	/* 断言 hoff 能放进可用空间 */
	Assert((rettuple->bt_info & BRIN_OFFSET_MASK) == hoff);

	/*
	 * heap_fill_tuple 算出的 infomask 和空位掩码对我们
	 * 没有用。但该函数不接受空的 infomask；而且我们
	 * 需要传入有效的空位掩码，这样它才会正确地跳过
	 * 在数据区中输出空属性。
	 */
	heap_fill_tuple(brtuple_disk_tupdesc(brdesc),
					values,
					nulls,
					(char *) rettuple + hoff,
					data_len,
					&phony_infomask,
					phony_nullbitmap);

	/* 这些用完了 */
	pfree(values);
	pfree(nulls);
	pfree(phony_nullbitmap);

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < nuntoasted; i++)
		pfree(DatumGetPointer(untoasted_values[i]));
#endif

	/*
	 * 现在填入真正的空值位掩码。先填 allnulls。
	 */
	if (anynulls)
	{
		bits8	   *bitP;
		int			bitmask;

		rettuple->bt_info |= BRIN_NULLS_MASK;

		/*
		 * 注意本模块把空值位的含义反过来了：我们
		 * 为空属性存 1 而不是 0。因此也必须把
		 * brin_deconstruct_tuple 中 att_isnull 测试的含义反过来。
		 */
		bitP = ((bits8 *) ((char *) rettuple + SizeOfBrinTuple)) - 1;
		bitmask = HIGHBIT;
		for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
		{
			if (bitmask != HIGHBIT)
				bitmask <<= 1;
			else
			{
				bitP += 1;
				*bitP = 0x0;
				bitmask = 1;
			}

			if (!tuple->bt_columns[keyno].bv_allnulls)
				continue;

			*bitP |= bitmask;
		}
		/* 接着是 hasnulls 位 */
		for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
		{
			if (bitmask != HIGHBIT)
				bitmask <<= 1;
			else
			{
				bitP += 1;
				*bitP = 0x0;
				bitmask = 1;
			}

			if (!tuple->bt_columns[keyno].bv_hasnulls)
				continue;

			*bitP |= bitmask;
		}
	}

	if (tuple->bt_placeholder)
		rettuple->bt_info |= BRIN_PLACEHOLDER_MASK;

	if (tuple->bt_empty_range)
		rettuple->bt_info |= BRIN_EMPTY_RANGE_MASK;

	*size = len;
	return rettuple;
}

/*
 * 生成一个不含数据值、标记为占位的新磁盘元组。
 *
 * 这是 brin_form_tuple 的精简版。
 */
BrinTuple *
brin_form_placeholder_tuple(BrinDesc *brdesc, BlockNumber blkno, Size *size)
{
	Size		len;
	Size		hoff;
	BrinTuple  *rettuple;
	int			keyno;
	bits8	   *bitP;
	int			bitmask;

	/* 计算所需总空间：总是加上空值位 */
	len = SizeOfBrinTuple;
	len += BITMAPLEN(brdesc->bd_tupdesc->natts * 2);
	len = hoff = MAXALIGN(len);

	rettuple = palloc0(len);
	rettuple->bt_blkno = blkno;
	rettuple->bt_info = hoff;
	rettuple->bt_info |= BRIN_NULLS_MASK | BRIN_PLACEHOLDER_MASK | BRIN_EMPTY_RANGE_MASK;

	bitP = ((bits8 *) ((char *) rettuple + SizeOfBrinTuple)) - 1;
	bitmask = HIGHBIT;
	/* 把所有属性的 allnulls 设为真 */
	for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
	{
		if (bitmask != HIGHBIT)
			bitmask <<= 1;
		else
		{
			bitP += 1;
			*bitP = 0x0;
			bitmask = 1;
		}

		*bitP |= bitmask;
	}
	/* 不必设置 hasnulls */

	*size = len;
	return rettuple;
}

/*
 * 释放由 brin_form_tuple 创建的元组
 */
void
brin_free_tuple(BrinTuple *tuple)
{
	pfree(tuple);
}

/*
 * 给定大小为 len 的 brin 元组，创建它的一份副本。若 'dest' 不是
 * NULL，其大小为 destsz，可用作输出缓冲区；若待复制的元组
 * 放不下，则用 repalloc 扩大，并把大小
 * 更新为与之相符。这样在循环中处理许多 brin 元组时，
 * 可以避免 palloc/free 往返。
 */
BrinTuple *
brin_copy_tuple(BrinTuple *tuple, Size len, BrinTuple *dest, Size *destsz)
{
	if (!destsz || *destsz == 0)
		dest = palloc(len);
	else if (len > *destsz)
	{
		dest = repalloc(dest, len);
		*destsz = len;
	}

	memcpy(dest, tuple, len);

	return dest;
}

/*
 * 返回两个 BrinTuples 是否按位相同。
 */
bool
brin_tuples_equal(const BrinTuple *a, Size alen, const BrinTuple *b, Size blen)
{
	if (alen != blen)
		return false;
	if (memcmp(a, b, alen) != 0)
		return false;
	return true;
}

/*
 * 从头创建一个新的 BrinMemTuple，并把它初始化为
 * 空状态。
 *
 * 注意：我们不提供任何释放变形元组的手段，因此请务必
 * 使用临时内存上下文。
 */
BrinMemTuple *
brin_new_memtuple(BrinDesc *brdesc)
{
	BrinMemTuple *dtup;
	long		basesize;

	basesize = MAXALIGN(sizeof(BrinMemTuple) +
						sizeof(BrinValues) * brdesc->bd_tupdesc->natts);
	dtup = palloc0(basesize + sizeof(Datum) * brdesc->bd_totalstored);

	dtup->bt_values = palloc(sizeof(Datum) * brdesc->bd_totalstored);
	dtup->bt_allnulls = palloc(sizeof(bool) * brdesc->bd_tupdesc->natts);
	dtup->bt_hasnulls = palloc(sizeof(bool) * brdesc->bd_tupdesc->natts);

	dtup->bt_empty_range = true;

	dtup->bt_context = AllocSetContextCreate(CurrentMemoryContext,
											 "brin dtuple",
											 ALLOCSET_DEFAULT_SIZES);

	brin_memtuple_initialize(dtup, brdesc);

	return dtup;
}

/*
 * 把 BrinMemTuple 重置为初始状态。为了书写方便，
 * 我们返回同一个元组。
 */
BrinMemTuple *
brin_memtuple_initialize(BrinMemTuple *dtuple, BrinDesc *brdesc)
{
	int			i;
	char	   *currdatum;

	MemoryContextReset(dtuple->bt_context);

	currdatum = (char *) dtuple +
		MAXALIGN(sizeof(BrinMemTuple) +
				 sizeof(BrinValues) * brdesc->bd_tupdesc->natts);
	for (i = 0; i < brdesc->bd_tupdesc->natts; i++)
	{
		dtuple->bt_columns[i].bv_attno = i + 1;
		dtuple->bt_columns[i].bv_allnulls = true;
		dtuple->bt_columns[i].bv_hasnulls = false;
		dtuple->bt_columns[i].bv_values = (Datum *) currdatum;

		dtuple->bt_columns[i].bv_mem_value = PointerGetDatum(NULL);
		dtuple->bt_columns[i].bv_serialize = NULL;
		dtuple->bt_columns[i].bv_context = dtuple->bt_context;

		currdatum += sizeof(Datum) * brdesc->bd_info[i]->oi_nstored;
	}

	dtuple->bt_empty_range = true;

	return dtuple;
}

/*
 * 把 BrinTuple 转回 BrinMemTuple。这是
 * brin_form_tuple 的逆操作。
 *
 * 作为优化，调用者可以传入先前分配好的 'dMemtuple'。
 * 这样可以避免在这里分配；当本函数
 * 在循环中被调用许多次时，这很有用。调用者有责任
 * 保证给定的 BrinMemTuple 符合这里的需要。
 *
 * 注意这里不需要 "磁盘上的 tupdesc"；我们依靠自己的例程
 * 从磁盘格式解构该元组。
 */
BrinMemTuple *
brin_deform_tuple(BrinDesc *brdesc, BrinTuple *tuple, BrinMemTuple *dMemtuple)
{
	BrinMemTuple *dtup;
	Datum	   *values;
	bool	   *allnulls;
	bool	   *hasnulls;
	char	   *tp;
	bits8	   *nullbits;
	int			keyno;
	int			valueno;
	MemoryContext oldcxt;

	dtup = dMemtuple ? brin_memtuple_initialize(dMemtuple, brdesc) :
		brin_new_memtuple(brdesc);

	if (BrinTupleIsPlaceholder(tuple))
		dtup->bt_placeholder = true;

	/* 范围一开始是空的，取决于 BrinTuple */
	if (!BrinTupleIsEmptyRange(tuple))
		dtup->bt_empty_range = false;

	dtup->bt_blkno = tuple->bt_blkno;

	values = dtup->bt_values;
	allnulls = dtup->bt_allnulls;
	hasnulls = dtup->bt_hasnulls;

	tp = (char *) tuple + BrinTupleDataOffset(tuple);

	if (BrinTupleHasNulls(tuple))
		nullbits = (bits8 *) ((char *) tuple + SizeOfBrinTuple);
	else
		nullbits = NULL;
	brin_deconstruct_tuple(brdesc,
						   tp, nullbits, BrinTupleHasNulls(tuple),
						   values, allnulls, hasnulls);

	/*
	 * 逐一迭代，把每个值赋给每一列的 values 数组中
	 * 的对应项。这些复制发生在该元组的上下文中。
	 */
	oldcxt = MemoryContextSwitchTo(dtup->bt_context);
	for (valueno = 0, keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
	{
		int			i;

		if (allnulls[keyno])
		{
			valueno += brdesc->bd_info[keyno]->oi_nstored;
			continue;
		}

		/*
		 * 在某些情况下，我们希望跳过对值做 datumCopy，
		 * 只要调用者允许 ...
		 */
		for (i = 0; i < brdesc->bd_info[keyno]->oi_nstored; i++)
			dtup->bt_columns[keyno].bv_values[i] =
				datumCopy(values[valueno++],
						  brdesc->bd_info[keyno]->oi_typcache[i]->typbyval,
						  brdesc->bd_info[keyno]->oi_typcache[i]->typlen);

		dtup->bt_columns[keyno].bv_hasnulls = hasnulls[keyno];
		dtup->bt_columns[keyno].bv_allnulls = false;

		dtup->bt_columns[keyno].bv_mem_value = PointerGetDatum(NULL);
		dtup->bt_columns[keyno].bv_serialize = NULL;
		dtup->bt_columns[keyno].bv_context = dtup->bt_context;
	}

	MemoryContextSwitchTo(oldcxt);

	return dtup;
}

/*
 * brin_deconstruct_tuple
 *		从磁盘上的 BRIN 元组提取属性的核心。
 *
 * 其参数为：
 *	brdesc		所存储元组的 BRIN 描述符
 *	tp			指向元组数据区的指针
 *	nullbits	指向元组空位掩码的指针
 *	nulls		元组 infomask 中的 "has nulls" 位
 *	values		输出值，数组大小为 brdesc->bd_totalstored
 *	allnulls	输出 "allnulls"，大小为 brdesc->bd_tupdesc->natts
 *	hasnulls	输出 "hasnulls"，大小为 brdesc->bd_tupdesc->natts
 *
 * 输出数组必须由调用者分配。
 */
static inline void
brin_deconstruct_tuple(BrinDesc *brdesc,
					   char *tp, bits8 *nullbits, bool nulls,
					   Datum *values, bool *allnulls, bool *hasnulls)
{
	int			attnum;
	int			stored;
	TupleDesc	diskdsc;
	long		off;

	/*
	 * 先迭代到 natts，以取得每个属性的两个空值标志。
	 * 注意我们把 att_isnull 测试的含义反过来了，因为空值存
	 * 1（而不是像别处 att_isnull 约定那样，非空值存 1）。
	 * 参见 brin_form_tuple。
	 */
	for (attnum = 0; attnum < brdesc->bd_tupdesc->natts; attnum++)
	{
		/*
		 * "all nulls" 位表示该页范围内此列的全部值
		 * 都是空值。因此元组数据区中
		 * 没有这些值。
		 */
		allnulls[attnum] = nulls && !att_isnull(attnum, nullbits);

		/*
		 * "has nulls" 位表示有些元组含空值，但另一些
		 * 含非空值。因此我们知道该元组含有
		 * 此列的数据。
		 *
		 * hasnulls 位在同一位掩码中跟在 allnulls 位之后。
		 */
		hasnulls[attnum] =
			nulls && !att_isnull(brdesc->bd_tupdesc->natts + attnum, nullbits);
	}

	/*
	 * 迭代以取得每个属性所存储的值。注意由于我们
	 * 可能把属性条目复用于不止一列，这里无法缓存
	 * 偏移。
	 */
	diskdsc = brtuple_disk_tupdesc(brdesc);
	stored = 0;
	off = 0;
	for (attnum = 0; attnum < brdesc->bd_tupdesc->natts; attnum++)
	{
		int			datumno;

		if (allnulls[attnum])
		{
			stored += brdesc->bd_info[attnum]->oi_nstored;
			continue;
		}

		for (datumno = 0;
			 datumno < brdesc->bd_info[attnum]->oi_nstored;
			 datumno++)
		{
			Form_pg_attribute thisatt = TupleDescAttr(diskdsc, stored);

			if (thisatt->attlen == -1)
			{
				off = att_align_pointer(off, thisatt->attalign, -1,
										tp + off);
			}
			else
			{
				/* 不是 varlena，因此可以安全使用 att_align_nominal */
				off = att_align_nominal(off, thisatt->attalign);
			}

			values[stored++] = fetchatt(thisatt, tp + off);

			off = att_addlength_pointer(off, thisatt->attlen, tp + off);
		}
	}
}
