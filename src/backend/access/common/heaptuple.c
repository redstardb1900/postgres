/*-------------------------------------------------------------------------
 *
 * heaptuple.c
 *	  本文件包含堆元组的访问例程与修改例程，
 *	  以及各种元组实用函数。
 *
 * 关于变长值以及这段代码的若干说明：
 *
 * 在 Postgres 8.3 之前，变长值始终带有 4 字节长度头，
 * 因此至少需要 4 字节对齐。这对短变长值浪费了空间，
 * 例如 CHAR(1) 占 5 字节，对齐时最多还可能再需要
 * 3 个额外的填充字节。
 *
 * 现在，短变长值（数据最多 126 字节）缩成 1 字节头，
 * 并且不再对齐。为了对不想处理它的类型相关函数
 * 隐藏这一差别，这样的 datum 被视为 "toasted"，并会
 * 由 pg_detoast_datum 展开回普通的 4 字节头格式。
 * （在性能关键的代码路径上可以使用 pg_detoast_datum_packed
 * 以及相应的访问宏来避免这笔开销。）注意，这一
 * 转换直接在 heap_form_tuple 中完成，并不调用
 * heaptoast.c。
 *
 * 这一改动会破坏任何假定如下的代码：放进元组但从未
 * 落盘的值不必做 detoast。希望这样的地方
 * 并不多。
 *
 * 变长值在 pg_type/pg_attribute 中的对齐仍是 INT（或 DOUBLE），因为
 * 那是未 toast 格式的正常需要。但对 1 字节头格式
 * 我们忽略该对齐。这意味着变长 datum 的实际起始位置
 * 会随它所采用的格式而变化。要判断存放的是什么，就得
 * 把对齐填充字节定为零。
 * （Postgres 实际上一直把它们清零，但现在这是必须的！）由于
 * 1 字节头变长值的第一个字节绝不可能为零，我们可以检查
 * 前一个 datum 之后的第一个字节，以判断它是填充字节还是
 * 1 字节头变长值的起始。
 *
 * 注意，以前可以信赖系统目录中第一个变长列就在
 * 该目录 C 结构体所给出的偏移处，现在这样做有风险：
 * 只有当前一个成员按字对齐、从而绝不会出现任何填充时，
 * 才是安全的。
 *
 * attstorage 为 PLAIN 的变长值我们不做紧缩，因为该数据类型
 * 并不预期要对这些值做 detoast。oidvector 与 int2vector
 * 尤其如此，它们用于系统目录，
 * 我们仍希望能经由 C 结构体偏移来引用它们。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/heaptuple.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heaptoast.h"
#include "access/sysattr.h"
#include "access/tupdesc_details.h"
#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"


/*
 * att 的数据类型是否允许打包成 1 字节头的变长格式？
 * 使用 TupleDescAttr() 并把 attstorage 赋为
 * TYPSTORAGE_PLAIN 的函数无法使用紧缩的变长头；调用
 * TupleDescInitEntry() 的函数使用 typeForm->typstorage（TYPSTORAGE_EXTENDED），
 * 可以使用紧缩的变长头，例如：
 *     CREATE TABLE test(a VARCHAR(10000) STORAGE PLAIN);
 *     INSERT INTO test VALUES (repeat('A',10));
 * 可以用 pageinspect 验证这一点。
 */
#define ATT_IS_PACKABLE(att) \
	((att)->attlen == -1 && (att)->attstorage != TYPSTORAGE_PLAIN)
/* 若已经知道是变长值，则用这个 */
#define VARLENA_ATT_IS_PACKABLE(att) \
	((att)->attstorage != TYPSTORAGE_PLAIN)

/*
 * 为按引用传递的缺少属性建立缓存，使其在
 * tupleDesc 销毁之后仍然有效。
 */

typedef struct
{
	int			len;
	Datum		value;
} missing_cache_key;

static HTAB *missing_cache = NULL;

static uint32
missing_hash(const void *key, Size keysize)
{
	const missing_cache_key *entry = (missing_cache_key *) key;

	return hash_bytes((const unsigned char *) entry->value, entry->len);
}

static int
missing_match(const void *key1, const void *key2, Size keysize)
{
	const missing_cache_key *entry1 = (missing_cache_key *) key1;
	const missing_cache_key *entry2 = (missing_cache_key *) key2;

	if (entry1->len != entry2->len)
		return entry1->len > entry2->len ? 1 : -1;

	return memcmp(DatumGetPointer(entry1->value),
				  DatumGetPointer(entry2->value),
				  entry1->len);
}

static void
init_missing_cache()
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = sizeof(missing_cache_key);
	hash_ctl.entrysize = sizeof(missing_cache_key);
	hash_ctl.hcxt = TopMemoryContext;
	hash_ctl.hash = missing_hash;
	hash_ctl.match = missing_match;
	missing_cache =
		hash_create("Missing Values Cache",
					32,
					&hash_ctl,
					HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION | HASH_COMPARE);
}

/* ----------------------------------------------------------------
 *						其他支持例程
 * ----------------------------------------------------------------
 */

/*
 * 返回某属性的缺少值；若没有则返回 NULL。
 */
Datum
getmissingattr(TupleDesc tupleDesc,
			   int attnum, bool *isnull)
{
	Form_pg_attribute att;

	Assert(attnum <= tupleDesc->natts);
	Assert(attnum > 0);

	att = TupleDescAttr(tupleDesc, attnum - 1);

	if (att->atthasmissing)
	{
		AttrMissing *attrmiss;

		Assert(tupleDesc->constr);
		Assert(tupleDesc->constr->missing);

		attrmiss = tupleDesc->constr->missing + (attnum - 1);

		if (attrmiss->am_present)
		{
			missing_cache_key key;
			missing_cache_key *entry;
			bool		found;
			MemoryContext oldctx;

			*isnull = false;

			/* 按值属性无需缓存 */
			if (att->attbyval)
				return attrmiss->am_value;

			/* 若需要则建立缓存 */
			if (missing_cache == NULL)
				init_missing_cache();

			/* 检查是否已有缓存项 */
			Assert(att->attlen > 0 || att->attlen == -1);
			if (att->attlen > 0)
				key.len = att->attlen;
			else
				key.len = VARSIZE_ANY(attrmiss->am_value);
			key.value = attrmiss->am_value;

			entry = hash_search(missing_cache, &key, HASH_ENTER, &found);

			if (!found)
			{
				/* 缓存未命中，因此需要一份非临时的 datum 副本 */
				oldctx = MemoryContextSwitchTo(TopMemoryContext);
				entry->value =
					datumCopy(attrmiss->am_value, false, att->attlen);
				MemoryContextSwitchTo(oldctx);
			}

			return entry->value;
		}
	}

	*isnull = true;
	return PointerGetDatum(NULL);
}

/*
 * heap_compute_data_size
 *		确定待构造元组的数据区域大小
 */
Size
heap_compute_data_size(TupleDesc tupleDesc,
					   Datum *values,
					   bool *isnull)
{
	Size		data_length = 0;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;

	for (i = 0; i < numberOfAttributes; i++)
	{
		Datum		val;
		Form_pg_attribute atti;

		if (isnull[i])
			continue;

		val = values[i];
		atti = TupleDescAttr(tupleDesc, i);

		if (ATT_IS_PACKABLE(atti) &&
			VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
		{
			/*
			 * 预期会转换成短变长头，因此
			 * 调整长度，并且不计入任何对齐
			 */
			data_length += VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val));
		}
		else if (atti->attlen == -1 &&
				 VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			/*
			 * 要把展开值展平，使构造出的
			 * 元组不依赖它
			 */
			data_length = att_align_nominal(data_length, atti->attalign);
			data_length += EOH_get_flat_size(DatumGetEOHP(val));
		}
		else
		{
			data_length = att_align_datum(data_length, atti->attalign,
										  atti->attlen, val);
			data_length = att_addlength_datum(data_length, atti->attlen,
											  val);
		}
	}

	return data_length;
}

/*
 * heap_fill_tuple 及其他构造元组的例程所用的逐属性辅助函数。
 *
 * 填入一个数据值，或在空值位图中置一位
 */
static inline void
fill_val(Form_pg_attribute att,
		 bits8 **bit,
		 int *bitmask,
		 char **dataP,
		 uint16 *infomask,
		 Datum datum,
		 bool isnull)
{
	Size		data_length;
	char	   *data = *dataP;

	/*
	 * 若正在构造空值位图，就在此为当前列的值
	 * 设置相应的位。
	 */
	if (bit != NULL)
	{
		if (*bitmask != HIGHBIT)
			*bitmask <<= 1;
		else
		{
			*bit += 1;
			**bit = 0x0;
			*bitmask = 1;
		}

		if (isnull)
		{
			*infomask |= HEAP_HASNULL;
			return;
		}

		**bit |= *bitmask;
	}

	/*
	 * XXX 我们对指针值本身使用 att_align 宏，而不是对
	 * 偏移使用。这有点像权宜之计。
	 */
	if (att->attbyval)
	{
		/* 按值传递 */
		data = (char *) att_align_nominal(data, att->attalign);
		store_att_byval(data, datum, att->attlen);
		data_length = att->attlen;
	}
	else if (att->attlen == -1)
	{
		/* 变长值 */
		Pointer		val = DatumGetPointer(datum);

		*infomask |= HEAP_HASVARWIDTH;
		if (VARATT_IS_EXTERNAL(val))
		{
			if (VARATT_IS_EXTERNAL_EXPANDED(val))
			{
				/*
				 * 要把展开值展平，使
				 * 构造出的元组不依赖它
				 */
				ExpandedObjectHeader *eoh = DatumGetEOHP(datum);

				data = (char *) att_align_nominal(data,
												  att->attalign);
				data_length = EOH_get_flat_size(eoh);
				EOH_flatten_into(eoh, data, data_length);
			}
			else
			{
				*infomask |= HEAP_HASEXTERNAL;
				/* 无需对齐，按定义它就是短的 */
				data_length = VARSIZE_EXTERNAL(val);
				memcpy(data, val, data_length);
			}
		}
		else if (VARATT_IS_SHORT(val))
		{
			/* 短变长值无需对齐 */
			data_length = VARSIZE_SHORT(val);
			memcpy(data, val, data_length);
		}
		else if (VARLENA_ATT_IS_PACKABLE(att) &&
				 VARATT_CAN_MAKE_SHORT(val))
		{
			/* 转换成短变长值 -- 无需对齐 */
			data_length = VARATT_CONVERTED_SHORT_SIZE(val);
			SET_VARSIZE_SHORT(data, data_length);
			memcpy(data + 1, VARDATA(val), data_length - 1);
		}
		else
		{
			/* 完整的 4 字节头变长值 */
			data = (char *) att_align_nominal(data,
											  att->attalign);
			data_length = VARSIZE(val);
			memcpy(data, val, data_length);
		}
	}
	else if (att->attlen == -2)
	{
		/* cstring ... 从不需要对齐 */
		*infomask |= HEAP_HASVARWIDTH;
		Assert(att->attalign == TYPALIGN_CHAR);
		data_length = strlen(DatumGetCString(datum)) + 1;
		memcpy(data, DatumGetPointer(datum), data_length);
	}
	else
	{
		/* 定长、按引用传递 */
		data = (char *) att_align_nominal(data, att->attalign);
		Assert(att->attlen > 0);
		data_length = att->attlen;
		memcpy(data, DatumGetPointer(datum), data_length);
	}

	data += data_length;
	*dataP = data;
}

/*
 * heap_fill_tuple
 *		从 values/isnull 数组装入元组的数据部分
 *
 * 同时填入空值位图（若有），并设置反映元组
 * 数据内容的 infomask 位。
 *
 * 注意：现在要求调用方预先把数据区域清零。
 */
void
heap_fill_tuple(TupleDesc tupleDesc,
				Datum *values, bool *isnull,
				char *data, Size data_size,
				uint16 *infomask, bits8 *bit)
{
	bits8	   *bitP;
	int			bitmask;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;

#ifdef USE_ASSERT_CHECKING
	char	   *start = data;
#endif

	if (bit != NULL)
	{
		bitP = &bit[-1];
		bitmask = HIGHBIT;
	}
	else
	{
		/* 只为让编译器保持安静 */
		bitP = NULL;
		bitmask = 0;
	}

	*infomask &= ~(HEAP_HASNULL | HEAP_HASVARWIDTH | HEAP_HASEXTERNAL);

	for (i = 0; i < numberOfAttributes; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupleDesc, i);

		fill_val(attr,
				 bitP ? &bitP : NULL,
				 &bitmask,
				 &data,
				 infomask,
				 values ? values[i] : PointerGetDatum(NULL),
				 isnull ? isnull[i] : true);
	}

	Assert((data - start) == data_size);
}


/* ----------------------------------------------------------------
 *						堆元组接口
 * ----------------------------------------------------------------
 */

/* ----------------
 *		heap_attisnull - 当元组属性不存在时返回 true
 * ----------------
 */
bool
heap_attisnull(HeapTuple tup, int attnum, TupleDesc tupleDesc)
{
	/*
	 * 对于预期不会有缺少值的关系，允许 tupledesc 为 NULL，
	 * 例如目录关系与索引。
	 */
	Assert(!tupleDesc || attnum <= tupleDesc->natts);
	if (attnum > (int) HeapTupleHeaderGetNatts(tup->t_data))
	{
		if (tupleDesc && TupleDescAttr(tupleDesc, attnum - 1)->atthasmissing)
			return false;
		else
			return true;
	}

	if (attnum > 0)
	{
		if (HeapTupleNoNulls(tup))
			return false;
		return att_isnull(attnum - 1, tup->t_data->t_bits);
	}

	switch (attnum)
	{
		case TableOidAttributeNumber:
		case SelfItemPointerAttributeNumber:
		case MinTransactionIdAttributeNumber:
		case MinCommandIdAttributeNumber:
		case MaxTransactionIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			/* 这些绝不会是空值 */
			break;

		default:
			elog(ERROR, "invalid attnum: %d", attnum);
	}

	return false;
}

/* ----------------
 *		nocachegetattr
 *
 *		只会被 fastgetattr() 宏调用，且是在无法使用
 *		cacheoffset、而值又非空的时候。
 *
 *		本函数把属性偏移缓存在属性描述符中。
 *
 *		另一种加速办法是把偏移与元组一起缓存，
 *		但除非真把这些偏移写进要落盘的元组、
 *		并承受相应的存储代价，否则似乎更难办到。
 *		真糟。
 *
 *		本方案会比那种做法略慢，但对命中大量
 *		元组的查询应当表现良好。偏移缓存一次之后，
 *		再用同一属性描述符检查其余所有元组
 *		就会快得多。-cim 5/4/91
 *
 *		注意：若要修改这段代码，请同时参见 heap_deform_tuple。
 *		另见 nocache_index_getattr，那是用于索引
 *		元组的同一套代码。
 * ----------------
 */
Datum
nocachegetattr(HeapTuple tup,
			   int attnum,
			   TupleDesc tupleDesc)
{
	HeapTupleHeader td = tup->t_data;
	char	   *tp;				/* 指向元组数据部分的指针 */
	bits8	   *bp = td->t_bits;	/* 指向元组中空值位图的指针 */
	bool		slow = false;	/* 是否必须遍历各属性？ */
	int			off;			/* 数据区内的当前偏移 */

	/* ----------------
	 *	 三种情形：
	 *
	 *	 1：没有空值，也没有变长属性。
	 *	 2：在 att 之后有空值或变长属性。
	 *	 3：在 att 之前有空值或变长属性。
	 * ----------------
	 */

	attnum--;

	if (!HeapTupleNoNulls(tup))
	{
		/*
		 * 元组中某处存在空值
		 *
		 * 检查前面是否有任何位为空值...
		 */
		int			byte = attnum >> 3;
		int			finalbit = attnum & 0x07;

		/* 检查最后一个字节中末位之前的空值 */
		if ((~bp[byte]) & ((1 << finalbit) - 1))
			slow = true;
		else
		{
			/* 检查任何更早字节中的空值 */
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

	tp = (char *) td + td->t_hoff;

	if (!slow)
	{
		Form_pg_attribute att;

		/*
		 * 若执行到这里，直到目标属性（含）都没有空值。
		 * 若已有缓存偏移，就可以使用它。
		 */
		att = TupleDescAttr(tupleDesc, attnum);
		if (att->attcacheoff >= 0)
			return fetchatt(att, tp + att->attcacheoff);

		/*
		 * 否则，检查直到目标（含）是否有非定长属性。
		 * 若没有，就可以低成本地为这些属性
		 * 初始化缓存偏移。
		 */
		if (HeapTupleHasVarWidth(tup))
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
		 * 若执行到这里，直到目标属性（含）为止，元组
		 * 都没有空值或变长属性，因此可以使用缓存偏移
		 * ……只不过现在还没有，否则也不会走到这里。由于
		 * 计算定宽列的偏移很便宜，我们借此机会
		 * 为前面*所有*定宽列初始化缓存偏移，
		 * 希望以后不必再进入本
		 * 例程。
		 */
		TupleDescAttr(tupleDesc, 0)->attcacheoff = 0;

		/* 慢路径上可能已经设置过一些偏移 */
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
		 * 现在已知必须小心地遍历该元组。不过仍
		 * 可能为下一次缓存一些偏移。
		 *
		 * 注意：这个循环有点微妙。对每个非空属性，
		 * 必须先计入该属性之前的对齐填充，
		 * 再按长度越过该属性。空值既没有
		 * 存储，也没有对齐填充。在遇到空值或变长属性之前，
		 * 都可以使用或设置 attcacheoff。
		 */
		off = 0;
		for (i = 0;; i++)		/* 循环在 "break" 处退出 */
		{
			Form_pg_attribute att = TupleDescAttr(tupleDesc, i);

			if (HeapTupleHasNulls(tup) && att_isnull(i, bp))
			{
				usecache = false;
				continue;		/* 这不可能是目标 att */
			}

			/* 若已知下一偏移，就可以跳过其余部分 */
			if (usecache && att->attcacheoff >= 0)
				off = att->attcacheoff;
			else if (att->attlen == -1)
			{
				/*
				 * 只有当偏移已经适当对齐，从而任何情况下
				 * 都不会有填充字节时，才能缓存变长属性的偏移：
				 * 这样，该偏移对已对齐和未对齐的值
				 * 都有效。
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
				/* 不是变长值，因此可以安全使用 att_align_nominal */
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

/* ----------------
 *		heap_getsysattr
 *
 *		取一个元组的系统属性值。
 *
 * 这是 heap_getattr 宏的支持例程。该宏
 * 已经确定 attnum 指向系统属性。
 * ----------------
 */
Datum
heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Datum		result;

	Assert(tup);

	/* 目前，系统属性读出来绝不会是 NULL。 */
	*isnull = false;

	switch (attnum)
	{
		case SelfItemPointerAttributeNumber:
			/* 按引用传递的数据类型 */
			result = PointerGetDatum(&(tup->t_self));
			break;
		case MinTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetRawXmin(tup->t_data));
			break;
		case MaxTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetRawXmax(tup->t_data));
			break;
		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:

			/*
			 * cmin 与 cmax 现在都是同一字段的别名，该字段
			 * 实际上也可以是组合命令标识符。XXX 或许应当
			 * 在可能时返回真正的 cmin 或 cmax，也就是当我们
			 * 处于发起事务之内时？
			 */
			result = CommandIdGetDatum(HeapTupleHeaderGetRawCommandId(tup->t_data));
			break;
		case TableOidAttributeNumber:
			result = ObjectIdGetDatum(tup->t_tableOid);
			break;
		default:
			elog(ERROR, "invalid attnum: %d", attnum);
			result = 0;			/* 让编译器保持安静 */
			break;
	}
	return result;
}

/* ----------------
 *		heap_copytuple
 *
 *		返回整个元组的一份副本
 *
 * HeapTuple 结构体、元组头和元组数据都在
 * 单独一个 palloc() 块中分配。
 * ----------------
 */
HeapTuple
heap_copytuple(HeapTuple tuple)
{
	HeapTuple	newTuple;

	if (!HeapTupleIsValid(tuple) || tuple->t_data == NULL)
		return NULL;

	newTuple = (HeapTuple) palloc(HEAPTUPLESIZE + tuple->t_len);
	newTuple->t_len = tuple->t_len;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;
	newTuple->t_data = (HeapTupleHeader) ((char *) newTuple + HEAPTUPLESIZE);
	memcpy((char *) newTuple->t_data, (char *) tuple->t_data, tuple->t_len);
	return newTuple;
}

/* ----------------
 *		heap_copytuple_with_tuple
 *
 *		把元组复制到调用方提供的 HeapTuple 管理结构体中
 *
 * 注意，调用本函数之后，"dest" HeapTuple 不会
 * 像 heap_copytuple() 那样作为单独一个 palloc() 块来分配。
 * ----------------
 */
void
heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest)
{
	if (!HeapTupleIsValid(src) || src->t_data == NULL)
	{
		dest->t_data = NULL;
		return;
	}

	dest->t_len = src->t_len;
	dest->t_self = src->t_self;
	dest->t_tableOid = src->t_tableOid;
	dest->t_data = (HeapTupleHeader) palloc(src->t_len);
	memcpy((char *) dest->t_data, (char *) src->t_data, src->t_len);
}

/*
 * 扩展属性数少于所需的元组。对 sourceTuple 中
 * 不存在的每个属性，若有缺少值，就使用该值。
 * 否则该属性将被设为 NULL。
 *
 * 源元组的属性数必须少于所需数量。
 *
 * targetHeapTuple 与 targetMinimalTuple 只能提供其中一个。
 * 另一个参数必须为 NULL。
 */
static void
expand_tuple(HeapTuple *targetHeapTuple,
			 MinimalTuple *targetMinimalTuple,
			 HeapTuple sourceTuple,
			 TupleDesc tupleDesc)
{
	AttrMissing *attrmiss = NULL;
	int			attnum;
	int			firstmissingnum;
	bool		hasNulls = HeapTupleHasNulls(sourceTuple);
	HeapTupleHeader targetTHeader;
	HeapTupleHeader sourceTHeader = sourceTuple->t_data;
	int			sourceNatts = HeapTupleHeaderGetNatts(sourceTHeader);
	int			natts = tupleDesc->natts;
	int			sourceNullLen;
	int			targetNullLen;
	Size		sourceDataLen = sourceTuple->t_len - sourceTHeader->t_hoff;
	Size		targetDataLen;
	Size		len;
	int			hoff;
	bits8	   *nullBits = NULL;
	int			bitMask = 0;
	char	   *targetData;
	uint16	   *infoMask;

	Assert((targetHeapTuple && !targetMinimalTuple)
		   || (!targetHeapTuple && targetMinimalTuple));

	Assert(sourceNatts < natts);

	sourceNullLen = (hasNulls ? BITMAPLEN(sourceNatts) : 0);

	targetDataLen = sourceDataLen;

	if (tupleDesc->constr &&
		tupleDesc->constr->missing)
	{
		/*
		 * 若有缺少值，就要把它们放进元组。
		 * 在此之前，必须为 values 数组
		 * 和变长数据计算额外长度。
		 */
		attrmiss = tupleDesc->constr->missing;

		/*
		 * 找出 attrmiss 中第一个在源元组里没有值的项。
		 * 在那之前的缺少项都可以忽略。
		 */
		for (firstmissingnum = sourceNatts;
			 firstmissingnum < natts;
			 firstmissingnum++)
		{
			if (attrmiss[firstmissingnum].am_present)
				break;
			else
				hasNulls = true;
		}

		/*
		 * 现在遍历缺少的属性。若有缺少值，就为它
		 * 留出空间。否则它将是 NULL。
		 */
		for (attnum = firstmissingnum;
			 attnum < natts;
			 attnum++)
		{
			if (attrmiss[attnum].am_present)
			{
				Form_pg_attribute att = TupleDescAttr(tupleDesc, attnum);

				targetDataLen = att_align_datum(targetDataLen,
												att->attalign,
												att->attlen,
												attrmiss[attnum].am_value);

				targetDataLen = att_addlength_pointer(targetDataLen,
													  att->attlen,
													  attrmiss[attnum].am_value);
			}
			else
			{
				/* 没有缺少值，因此它必定是空值 */
				hasNulls = true;
			}
		}
	}							/* 有缺少值时的分支到此结束 */
	else
	{
		/*
		 * 若完全没有缺少值，则必须允许 NULLS，
		 * 因为已知有些属性并不存在。
		 */
		hasNulls = true;
	}

	len = 0;

	if (hasNulls)
	{
		targetNullLen = BITMAPLEN(natts);
		len += targetNullLen;
	}
	else
		targetNullLen = 0;

	/*
	 * 分配所需空间并清零。注意元组本体与
	 * HeapTupleData 管理结构体分配在同一块中。
	 */
	if (targetHeapTuple)
	{
		len += offsetof(HeapTupleHeaderData, t_bits);
		hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */
		len += targetDataLen;

		*targetHeapTuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
		(*targetHeapTuple)->t_data
			= targetTHeader
			= (HeapTupleHeader) ((char *) *targetHeapTuple + HEAPTUPLESIZE);
		(*targetHeapTuple)->t_len = len;
		(*targetHeapTuple)->t_tableOid = sourceTuple->t_tableOid;
		(*targetHeapTuple)->t_self = sourceTuple->t_self;

		targetTHeader->t_infomask = sourceTHeader->t_infomask;
		targetTHeader->t_hoff = hoff;
		HeapTupleHeaderSetNatts(targetTHeader, natts);
		HeapTupleHeaderSetDatumLength(targetTHeader, len);
		HeapTupleHeaderSetTypeId(targetTHeader, tupleDesc->tdtypeid);
		HeapTupleHeaderSetTypMod(targetTHeader, tupleDesc->tdtypmod);
		/* 同时确保未经显式设置时 t_ctid 为无效 */
		ItemPointerSetInvalid(&(targetTHeader->t_ctid));
		if (targetNullLen > 0)
			nullBits = (bits8 *) ((char *) (*targetHeapTuple)->t_data
								  + offsetof(HeapTupleHeaderData, t_bits));
		targetData = (char *) (*targetHeapTuple)->t_data + hoff;
		infoMask = &(targetTHeader->t_infomask);
	}
	else
	{
		len += SizeofMinimalTupleHeader;
		hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */
		len += targetDataLen;

		*targetMinimalTuple = (MinimalTuple) palloc0(len);
		(*targetMinimalTuple)->t_len = len;
		(*targetMinimalTuple)->t_hoff = hoff + MINIMAL_TUPLE_OFFSET;
		(*targetMinimalTuple)->t_infomask = sourceTHeader->t_infomask;
		/* 同一宏对 MinimalTuples 同样适用 */
		HeapTupleHeaderSetNatts(*targetMinimalTuple, natts);
		if (targetNullLen > 0)
			nullBits = (bits8 *) ((char *) *targetMinimalTuple
								  + offsetof(MinimalTupleData, t_bits));
		targetData = (char *) *targetMinimalTuple + hoff;
		infoMask = &((*targetMinimalTuple)->t_infomask);
	}

	if (targetNullLen > 0)
	{
		if (sourceNullLen > 0)
		{
			/* 若位图原先已存在，则复制进来——各位都已设好 */
			memcpy(nullBits,
				   ((char *) sourceTHeader)
				   + offsetof(HeapTupleHeaderData, t_bits),
				   sourceNullLen);
			nullBits += sourceNullLen - 1;
		}
		else
		{
			sourceNullLen = BITMAPLEN(sourceNatts);
			/* 把所有已有属性设为 NOT NULL */
			memset(nullBits, 0xff, sourceNullLen);

			nullBits += sourceNullLen - 1;

			if (sourceNatts & 0x07)
			{
				/* 构造掩码（取反！） */
				bitMask = 0xff << (sourceNatts & 0x07);
				/* 完成 */
				*nullBits = ~bitMask;
			}
		}

		bitMask = (1 << ((sourceNatts - 1) & 0x07));
	}							/* 有空值位图时的分支到此结束 */

	memcpy(targetData,
		   ((char *) sourceTuple->t_data) + sourceTHeader->t_hoff,
		   sourceDataLen);

	targetData += sourceDataLen;

	/* 现在填入缺少的值 */
	for (attnum = sourceNatts; attnum < natts; attnum++)
	{

		Form_pg_attribute attr = TupleDescAttr(tupleDesc, attnum);

		if (attrmiss && attrmiss[attnum].am_present)
		{
			fill_val(attr,
					 nullBits ? &nullBits : NULL,
					 &bitMask,
					 &targetData,
					 infoMask,
					 attrmiss[attnum].am_value,
					 false);
		}
		else
		{
			fill_val(attr,
					 &nullBits,
					 &bitMask,
					 &targetData,
					 infoMask,
					 (Datum) 0,
					 true);
		}
	}							/* 遍历缺少属性的循环到此结束 */
}

/*
 * 为最小 HeapTuple 填入缺少的值
 */
MinimalTuple
minimal_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc)
{
	MinimalTuple minimalTuple;

	expand_tuple(NULL, &minimalTuple, sourceTuple, tupleDesc);
	return minimalTuple;
}

/*
 * 为普通 HeapTuple 填入缺少的值
 */
HeapTuple
heap_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc)
{
	HeapTuple	heapTuple;

	expand_tuple(&heapTuple, NULL, sourceTuple, tupleDesc);
	return heapTuple;
}

/* ----------------
 *		heap_copy_tuple_as_datum
 *
 *		把元组复制为复合类型 Datum
 * ----------------
 */
Datum
heap_copy_tuple_as_datum(HeapTuple tuple, TupleDesc tupleDesc)
{
	HeapTupleHeader td;

	/*
	 * 若元组含有任何外部 TOAST 指针，就必须内联
	 * 这些字段，以符合复合类型 Datum 的约定。
	 */
	if (HeapTupleHasExternal(tuple))
		return toast_flatten_tuple_to_datum(tuple->t_data,
											tuple->t_len,
											tupleDesc);

	/*
	 * 简单情形的快速路径：做一份 palloc 出来的副本，并填入
	 * 正确的复合 Datum 头字段（若给定元组来自磁盘
	 * 而非 heap_form_tuple，这些字段可能尚未设置）。
	 */
	td = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy((char *) td, (char *) tuple->t_data, tuple->t_len);

	HeapTupleHeaderSetDatumLength(td, tuple->t_len);
	HeapTupleHeaderSetTypeId(td, tupleDesc->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDesc->tdtypmod);

	return PointerGetDatum(td);
}

/*
 * heap_form_tuple
 *		根据给定的 values[] 与 isnull[] 数组构造元组，
 *		数组长度由 tupleDescriptor->natts 指出
 *
 * 结果分配在当前内存上下文中。
 */
HeapTuple
heap_form_tuple(TupleDesc tupleDescriptor,
				Datum *values,
				bool *isnull)
{
	HeapTuple	tuple;			/* 返回的元组 */
	HeapTupleHeader td;			/* 元组数据 */
	Size		len,
				data_len;
	int			hoff;
	bool		hasnull = false;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * 检查是否有空值
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	/*
	 * 确定所需的总空间
	 */
	len = offsetof(HeapTupleHeaderData, t_bits);

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */

	data_len = heap_compute_data_size(tupleDescriptor, values, isnull);

	len += data_len;

	/*
	 * 分配所需空间并清零。注意元组本体与
	 * HeapTupleData 管理结构体分配在同一块中。
	 */
	tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
	tuple->t_data = td = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	/*
	 * 然后填入信息。注意，即使这个元组可能永远不会
	 * 成为 Datum，我们也填写 Datum 字段。这样在需要时
	 * HeapTupleHeaderGetDatum 就能识别元组类型。
	 */
	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	HeapTupleHeaderSetDatumLength(td, len);
	HeapTupleHeaderSetTypeId(td, tupleDescriptor->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDescriptor->tdtypmod);
	/* 同时确保未经显式设置时 t_ctid 为无效 */
	ItemPointerSetInvalid(&(td->t_ctid));

	HeapTupleHeaderSetNatts(td, numberOfAttributes);
	td->t_hoff = hoff;

	heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) td + hoff,
					data_len,
					&td->t_infomask,
					(hasnull ? td->t_bits : NULL));

	return tuple;
}

/*
 * heap_modify_tuple
 *		由旧元组和一组替换值构成新元组。
 *
 * replValues、replIsnull 与 doReplace 数组的长度必须由
 * tupleDesc->natts 指出。新元组在 doReplace 为真的列上
 * 使用 replValues/replIsnull 的数据，在 doReplace 为假的
 * 列上则使用旧元组的数据。
 *
 * 结果分配在当前内存上下文中。
 */
HeapTuple
heap_modify_tuple(HeapTuple tuple,
				  TupleDesc tupleDesc,
				  Datum *replValues,
				  bool *replIsnull,
				  bool *doReplace)
{
	int			numberOfAttributes = tupleDesc->natts;
	int			attoff;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	newTuple;

	/*
	 * 按情况从元组或替换信息分配并填充
	 * values 与 isnull 数组。
	 *
	 * 注意：这里用 heap_deform_tuple() 还是只对未替换列
	 * 调用 heap_getattr()，尚可商榷。若替换的列很多、
	 * 未替换的列很少，后者可能更快。不过
	 * heap_deform_tuple 的代价只是 O(N)，而 heap_getattr 方式
	 * 在未替换列很多时会达到 O(N^2)，因此
	 * 更稳妥的是偏向线性代价。
	 */
	values = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	isnull = (bool *) palloc(numberOfAttributes * sizeof(bool));

	heap_deform_tuple(tuple, tupleDesc, values, isnull);

	for (attoff = 0; attoff < numberOfAttributes; attoff++)
	{
		if (doReplace[attoff])
		{
			values[attoff] = replValues[attoff];
			isnull[attoff] = replIsnull[attoff];
		}
	}

	/*
	 * 根据 values 与 isnull 数组创建新元组
	 */
	newTuple = heap_form_tuple(tupleDesc, values, isnull);

	pfree(values);
	pfree(isnull);

	/*
	 * 复制旧元组的标识信息：t_ctid、t_self
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
}

/*
 * heap_modify_tuple_by_cols
 *		由旧元组和一组替换值构成新元组。
 *
 * 与 heap_modify_tuple 类似，但不用布尔映射指定替换哪些
 * 列，而用目标列号数组。当要替换的列数
 * 固定时，这通常更方便。replCols、replValues 与 replIsnull
 * 数组长度必须为 nCols。目标列号
 * 从 1 开始编号。
 *
 * 结果分配在当前内存上下文中。
 */
HeapTuple
heap_modify_tuple_by_cols(HeapTuple tuple,
						  TupleDesc tupleDesc,
						  int nCols,
						  int *replCols,
						  Datum *replValues,
						  bool *replIsnull)
{
	int			numberOfAttributes = tupleDesc->natts;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	newTuple;
	int			i;

	/*
	 * 从元组分配并填充 values 与 isnull 数组，再用来自
	 * 输入数组的值替换选定的列。
	 */
	values = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	isnull = (bool *) palloc(numberOfAttributes * sizeof(bool));

	heap_deform_tuple(tuple, tupleDesc, values, isnull);

	for (i = 0; i < nCols; i++)
	{
		int			attnum = replCols[i];

		if (attnum <= 0 || attnum > numberOfAttributes)
			elog(ERROR, "invalid column number %d", attnum);
		values[attnum - 1] = replValues[i];
		isnull[attnum - 1] = replIsnull[i];
	}

	/*
	 * 根据 values 与 isnull 数组创建新元组
	 */
	newTuple = heap_form_tuple(tupleDesc, values, isnull);

	pfree(values);
	pfree(isnull);

	/*
	 * 复制旧元组的标识信息：t_ctid、t_self
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
}

/*
 * heap_deform_tuple
 *		给定一个元组，将其变形到 values/isnull 数组中；
 *		这是 heap_form_tuple 的逆操作。
 *
 *		values/isnull 数组的存储由调用方提供；
 *		其大小应按 tupleDesc->natts，而不是按
 *		HeapTupleHeaderGetNatts(tuple->t_data) 来确定。
 *
 *		注意，对于按引用传递的数据类型，放进 Datum 的
 *		指针会指向给定元组的内部。
 *
 *		当需要提取元组的全部或大部分字段时，
 *		本例程会明显快于围绕 heap_getattr 的循环；
 *		一旦涉及任何不可缓存的属性偏移，
 *		该循环就会变成 O(N^2)。
 */
void
heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
				  Datum *values, bool *isnull)
{
	HeapTupleHeader tup = tuple->t_data;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	int			tdesc_natts = tupleDesc->natts;
	int			natts;			/* 要提取的属性数 */
	int			attnum;
	char	   *tp;				/* 指向元组数据的指针 */
	uint32		off;			/* 元组数据中的偏移 */
	bits8	   *bp = tup->t_bits;	/* 指向元组中空值位图的指针 */
	bool		slow = false;	/* 能否使用或设置 attcacheoff？ */

	natts = HeapTupleHeaderGetNatts(tup);

	/*
	 * 在继承情形下，给定元组实际拥有的字段可能
	 * 比调用方预期的更多。不要越过调用方
	 * 数组的末尾。
	 */
	natts = Min(natts, tdesc_natts);

	tp = (char *) tup + tup->t_hoff;

	off = 0;

	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = TupleDescAttr(tupleDesc, attnum);

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* 此后无法再使用 attcacheoff */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else if (thisatt->attlen == -1)
		{
			/*
			 * 只有当偏移已经适当对齐，从而任何情况下
			 * 都不会有填充字节时，才能缓存变长属性的偏移：
			 * 这样，该偏移对已对齐和未对齐的值
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
			/* 不是变长值，因此可以安全使用 att_align_nominal */
			off = att_align_nominal(off, thisatt->attalign);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength_pointer(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* 此后无法再使用 attcacheoff */
	}

	/*
	 * 若元组没有 tupleDesc 所指出的全部属性，则按情况
	 * 把其余属性读成空值或缺少值。
	 */
	for (; attnum < tdesc_natts; attnum++)
		values[attnum] = getmissingattr(tupleDesc, attnum + 1, &isnull[attnum]);
}

/*
 * heap_freetuple - 释放堆元组
 */
void
heap_freetuple(HeapTuple htup)
{
	pfree(htup);
}


/*
 * heap_form_minimal_tuple
 *		根据给定的 values[] 与 isnull[] 数组构造 MinimalTuple，
 *		数组长度由 tupleDescriptor->natts 指出
 *
 * 这与 heap_form_tuple() 完全相同，只是结果是一个
 * 最小元组，没有 HeapTupleData 头，也没有
 * 系统列的空间。
 *
 * 结果分配在当前内存上下文中。
 */
MinimalTuple
heap_form_minimal_tuple(TupleDesc tupleDescriptor,
						Datum *values,
						bool *isnull)
{
	MinimalTuple tuple;			/* 返回的元组 */
	Size		len,
				data_len;
	int			hoff;
	bool		hasnull = false;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * 检查是否有空值
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	/*
	 * 确定所需的总空间
	 */
	len = SizeofMinimalTupleHeader;

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	hoff = len = MAXALIGN(len); /* 安全地对齐用户数据 */

	data_len = heap_compute_data_size(tupleDescriptor, values, isnull);

	len += data_len;

	/*
	 * 分配所需空间并清零。
	 */
	tuple = (MinimalTuple) palloc0(len);

	/*
	 * 然后填入信息。
	 */
	tuple->t_len = len;
	HeapTupleHeaderSetNatts(tuple, numberOfAttributes);
	tuple->t_hoff = hoff + MINIMAL_TUPLE_OFFSET;

	heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) tuple + hoff,
					data_len,
					&tuple->t_infomask,
					(hasnull ? tuple->t_bits : NULL));

	return tuple;
}

/*
 * heap_free_minimal_tuple - 释放最小元组
 */
void
heap_free_minimal_tuple(MinimalTuple mtup)
{
	pfree(mtup);
}

/*
 * heap_copy_minimal_tuple
 *		复制一个 MinimalTuple
 *
 * 结果分配在当前内存上下文中。
 */
MinimalTuple
heap_copy_minimal_tuple(MinimalTuple mtup)
{
	MinimalTuple result;

	result = (MinimalTuple) palloc(mtup->t_len);
	memcpy(result, mtup, mtup->t_len);
	return result;
}

/*
 * heap_tuple_from_minimal_tuple
 *		从 MinimalTuple 复制出一个 HeapTuple；
 *		系统列用零填充
 *
 * 结果分配在当前内存上下文中。
 * HeapTuple 结构体、元组头和元组数据都在
 * 单独一个 palloc() 块中分配。
 */
HeapTuple
heap_tuple_from_minimal_tuple(MinimalTuple mtup)
{
	HeapTuple	result;
	uint32		len = mtup->t_len + MINIMAL_TUPLE_OFFSET;

	result = (HeapTuple) palloc(HEAPTUPLESIZE + len);
	result->t_len = len;
	ItemPointerSetInvalid(&(result->t_self));
	result->t_tableOid = InvalidOid;
	result->t_data = (HeapTupleHeader) ((char *) result + HEAPTUPLESIZE);
	memcpy((char *) result->t_data + MINIMAL_TUPLE_OFFSET, mtup, mtup->t_len);
	memset(result->t_data, 0, offsetof(HeapTupleHeaderData, t_infomask2));
	return result;
}

/*
 * minimal_tuple_from_heap_tuple
 *		从 HeapTuple 复制出一个 MinimalTuple
 *
 * 结果分配在当前内存上下文中。
 */
MinimalTuple
minimal_tuple_from_heap_tuple(HeapTuple htup)
{
	MinimalTuple result;
	uint32		len;

	Assert(htup->t_len > MINIMAL_TUPLE_OFFSET);
	len = htup->t_len - MINIMAL_TUPLE_OFFSET;
	result = (MinimalTuple) palloc(len);
	memcpy(result, (char *) htup->t_data + MINIMAL_TUPLE_OFFSET, len);
	result->t_len = len;
	return result;
}

/*
 * 这主要是为了让 JIT 能够内联该定义，不过
 * 在调试时有时也有用。
 */
size_t
varsize_any(void *p)
{
	return VARSIZE_ANY(p);
}
