/*-------------------------------------------------------------------------
 *
 * datum.c
 *	  POSTGRES Datum（抽象数据类型）操作例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/datum.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * 在实现这些例程时，我们做如下假设：
 *
 * A) 若类型为 "byVal"，则全部信息都存放在
 * Datum 本身（即不涉及任何指针！）。此时，
 * 类型的长度总是大于零，并且不超过
 * "sizeof(Datum)"
 *
 * B) 若某类型不为 "byVal" 且长度固定（typlen > 0），
 * 那么 "Datum" 总包含一个指向字节流的指针。
 * 有效字节的数目总是等于 typlen。
 *
 * C) 若某类型不为 "byVal" 且 typlen == -1，
 * 此时 "Datum" 总指向 "struct varlena"。
 * 该 varlena 结构中存有关于此类型当前实例
 * 实际长度及其取值的信息。
 *
 * D) 若某类型不为 "byVal" 且 typlen == -2，
 * 此时 "Datum" 总指向 NUL 结尾的 C 字符串。
 *
 * 注意我们对 "toasted" datum 不作特殊处理；因此
 * 被复制或比较的是压缩后的数据或 toast 引用。
 * 不过，对展开对象的 datumCopy() 是个例外，
 * 因为多数调用方期望 datumCopy() 返回简单连续（
 * pfree 的）结果。另见 datumTransfer()。
 */

#include "postgres.h"

#include "access/detoast.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"


/*-------------------------------------------------------------------------
 * datumGetSize
 *
 * 求 datum 的"真实"大小，需给定 datum 值、
 * 是否"按值传递"及声明的类型长度。
 * （对于 TOAST 指针 datum，这是指针 datum 的大小。）
 *
 * 这实质上是 att_addlength_datum() 宏的脱离内联版本，
 * 该宏在 access/tupmacs.h 中，我们多做些错误检查。
 *-------------------------------------------------------------------------
 */
Size
datumGetSize(Datum value, bool typByVal, int typLen)
{
	Size		size;

	if (typByVal)
	{
		/* 按值传递的类型总是定长的 */
		Assert(typLen > 0 && typLen <= sizeof(Datum));
		size = (Size) typLen;
	}
	else
	{
		if (typLen > 0)
		{
			/* 定长传引用类型 */
			size = (Size) typLen;
		}
		else if (typLen == -1)
		{
			/* varlena 数据类型 */
			struct varlena *s = (struct varlena *) DatumGetPointer(value);

			if (!PointerIsValid(s))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("invalid Datum pointer")));

			size = (Size) VARSIZE_ANY(s);
		}
		else if (typLen == -2)
		{
			/* cstring 数据类型 */
			char	   *s = (char *) DatumGetPointer(value);

			if (!PointerIsValid(s))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("invalid Datum pointer")));

			size = (Size) (strlen(s) + 1);
		}
		else
		{
			elog(ERROR, "invalid typLen: %d", typLen);
			size = 0;			/* 让编译器安静 */
		}
	}

	return size;
}

/*-------------------------------------------------------------------------
 * datumCopy
 *
 * 复制非 NULL datum。
 *
 * 若数据类型为传引用，则用 palloc() 获取内存。
 *
 * 若该值是展开对象的引用，我们会将其展平进内存，
 * 内存由 palloc() 提供。必须复制：本函数主要用途
 * 之一是从即将销毁的临时上下文复制出 datum，
 * 而展开对象多半位于同样会消失的子上下文中。
 * 此外，许多调用方都假定
 * 结果为单个可 pfree 的块。
 *-------------------------------------------------------------------------
 */
Datum
datumCopy(Datum value, bool typByVal, int typLen)
{
	Datum		res;

	if (typByVal)
		res = value;
	else if (typLen == -1)
	{
		/* varlena 数据类型 */
		struct varlena *vl = (struct varlena *) DatumGetPointer(value);

		if (VARATT_IS_EXTERNAL_EXPANDED(vl))
		{
			/* 展平到调用方的内存上下文 */
			ExpandedObjectHeader *eoh = DatumGetEOHP(value);
			Size		resultsize;
			char	   *resultptr;

			resultsize = EOH_get_flat_size(eoh);
			resultptr = (char *) palloc(resultsize);
			EOH_flatten_into(eoh, (void *) resultptr, resultsize);
			res = PointerGetDatum(resultptr);
		}
		else
		{
			/* 否则，原样复制 varlena datum */
			Size		realSize;
			char	   *resultptr;

			realSize = (Size) VARSIZE_ANY(vl);
			resultptr = (char *) palloc(realSize);
			memcpy(resultptr, vl, realSize);
			res = PointerGetDatum(resultptr);
		}
	}
	else
	{
		/* 传引用且非 varlena，故未被 TOAST */
		Size		realSize;
		char	   *resultptr;

		realSize = datumGetSize(value, typByVal, typLen);

		resultptr = (char *) palloc(realSize);
		memcpy(resultptr, DatumGetPointer(value), realSize);
		res = PointerGetDatum(resultptr);
	}
	return res;
}

/*-------------------------------------------------------------------------
 * datumTransfer
 *
 * 把非 NULL datum 转移到当前内存上下文。
 *
 * 此函数与 datumCopy() 等价，除非 datum 是展开对象的
 * 读写指针。那种情况下我们仅把该对象重新挂接
 * 到当前上下文，并返回其标准读写指针（以防传入的
 * 是寿命较短的临时指针）。
 *-------------------------------------------------------------------------
 */
Datum
datumTransfer(Datum value, bool typByVal, int typLen)
{
	if (!typByVal && typLen == -1 &&
		VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(value)))
		value = TransferExpandedObject(value, CurrentMemoryContext);
	else
		value = datumCopy(value, typByVal, typLen);
	return value;
}

/*-------------------------------------------------------------------------
 * datumIsEqual
 *
 * 两 datum 相等则返回 true，否则 false
 *
 * 注意！
 * 我们只是逐字节地比较两个值。
 * 若同一值有不同表示，本例程将返回 false
 * （例如反码算术中
 * 零的表示就属于这种情况）。
 * 另外，若任一 datum 已被 "toasted"，答案多半
 * 不合你意。
 *
 * 不要试图在处理 "toasted" datum 方面把本函数做得比
 * 现在更聪明，因为有些调用方可能正处于事务
 * 中止的上下文中工作。
 *-------------------------------------------------------------------------
 */
bool
datumIsEqual(Datum value1, Datum value2, bool typByVal, int typLen)
{
	bool		res;

	if (typByVal)
	{
		/*
		 * 直接比较两个 datum。注意：只比较 "len" 个字节是
		 * 不行的，因为我们不知道这些字节在 "Datum" 内部
		 * 是如何对齐的。我们改而假定任何给定类型对
		 * 如何填充 Datum 中的多余位都是一致的。
		 */
		res = (value1 == value2);
	}
	else
	{
		Size		size1,
					size2;
		char	   *s1,
				   *s2;

		/*
		 * 比较各 datum 中所存指针指向的字节。
		 */
		size1 = datumGetSize(value1, typByVal, typLen);
		size2 = datumGetSize(value2, typByVal, typLen);
		if (size1 != size2)
			return false;
		s1 = (char *) DatumGetPointer(value1);
		s2 = (char *) DatumGetPointer(value2);
		res = (memcmp(s1, s2, size1) == 0);
	}
	return res;
}

/*-------------------------------------------------------------------------
 * datum_image_eq
 *
 * 基于字节映像比较两个 datum 的内容是否完全相同。若
 * 相等则返回 true，否则返回 false。
 *-------------------------------------------------------------------------
 */
bool
datum_image_eq(Datum value1, Datum value2, bool typByVal, int typLen)
{
	Size		len1,
				len2;
	bool		result = true;

	if (typByVal)
	{
		result = (value1 == value2);
	}
	else if (typLen > 0)
	{
		result = (memcmp(DatumGetPointer(value1),
						 DatumGetPointer(value2),
						 typLen) == 0);
	}
	else if (typLen == -1)
	{
		len1 = toast_raw_datum_size(value1);
		len2 = toast_raw_datum_size(value2);
		/* 长度不匹配则无需去 TOAST。 */
		if (len1 != len2)
			result = false;
		else
		{
			struct varlena *arg1val;
			struct varlena *arg2val;

			arg1val = PG_DETOAST_DATUM_PACKED(value1);
			arg2val = PG_DETOAST_DATUM_PACKED(value2);

			result = (memcmp(VARDATA_ANY(arg1val),
							 VARDATA_ANY(arg2val),
							 len1 - VARHDRSZ) == 0);

			/* 仅释放此处生成的副本内存。 */
			if ((Pointer) arg1val != (Pointer) value1)
				pfree(arg1val);
			if ((Pointer) arg2val != (Pointer) value2)
				pfree(arg2val);
		}
	}
	else if (typLen == -2)
	{
		char	   *s1,
				   *s2;

		/* 比较 cstring datum */
		s1 = DatumGetCString(value1);
		s2 = DatumGetCString(value2);
		len1 = strlen(s1) + 1;
		len2 = strlen(s2) + 1;
		if (len1 != len2)
			return false;
		result = (memcmp(s1, s2, len1) == 0);
	}
	else
		elog(ERROR, "unexpected typLen: %d", typLen);

	return result;
}

/*-------------------------------------------------------------------------
 * datum_image_hash
 *
 * 基于 'value' 的二进制表示生成哈希值。多数
 * 用例应使用 Datum 类型专有的哈希函数，不过某些
 * 边角情况需要基于实际位而非逻辑值来生成
 * 哈希值。
 *-------------------------------------------------------------------------
 */
uint32
datum_image_hash(Datum value, bool typByVal, int typLen)
{
	Size		len;
	uint32		result;

	if (typByVal)
		result = hash_bytes((unsigned char *) &value, sizeof(Datum));
	else if (typLen > 0)
		result = hash_bytes((unsigned char *) DatumGetPointer(value), typLen);
	else if (typLen == -1)
	{
		struct varlena *val;

		len = toast_raw_datum_size(value);

		val = PG_DETOAST_DATUM_PACKED(value);

		result = hash_bytes((unsigned char *) VARDATA_ANY(val), len - VARHDRSZ);

		/* 仅释放此处生成的副本内存。 */
		if ((Pointer) val != (Pointer) value)
			pfree(val);
	}
	else if (typLen == -2)
	{
		char	   *s;

		s = DatumGetCString(value);
		len = strlen(s) + 1;

		result = hash_bytes((unsigned char *) s, len);
	}
	else
	{
		elog(ERROR, "unexpected typLen: %d", typLen);
		result = 0;				/* 让编译器安静 */
	}

	return result;
}

/*-------------------------------------------------------------------------
 * btequalimage
 *
 * 通用的 "equalimage" 支持函数。
 *
 * B-Tree 算子类的相等函数若在任何情况下都能安全地
 * 用 datum_image_eq() 替代，可用它作为其 "equalimage" 支持
 * 函数。
 *
 * 较早的次要版本曾把本函数错误地关联到
 * interval_ops。检测到该情况时撤回去重支持，而无需
 * initdb。
 *-------------------------------------------------------------------------
 */
Datum
btequalimage(PG_FUNCTION_ARGS)
{
	Oid			opcintype = PG_GETARG_OID(0);

	PG_RETURN_BOOL(opcintype != INTERVALOID);
}

/*-------------------------------------------------------------------------
 * datumEstimateSpace
 *
 * 计算 datumSerialize 对某个 Datum 所需的
 * 空间量。
 *-------------------------------------------------------------------------
 */
Size
datumEstimateSpace(Datum value, bool isnull, bool typByVal, int typLen)
{
	Size		sz = sizeof(int);

	if (!isnull)
	{
		/* 无需使用 add_size，不会溢出 */
		if (typByVal)
			sz += sizeof(Datum);
		else if (typLen == -1 &&
				 VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(value)))
		{
			/* 展开对象需要展平，见下方注释 */
			sz += EOH_get_flat_size(DatumGetEOHP(value));
		}
		else
			sz += datumGetSize(value, typByVal, typLen);
	}

	return sz;
}

/*-------------------------------------------------------------------------
 * datumSerialize
 *
 * 将可能为 NULL 的 datum 序列化到调用方存储。
 *
 * 注意："expanded" 对象会被展平以产生自包含的表示，
 * 而其他种类的 toast 指针则原样转移。
 * 这是因为本函数的预期用途是把值传递给
 * 同一数据库服务器内的另一进程。对方进程无法
 * 访问本进程内存中的 "expanded" 对象，但假定它能
 * 解引用与本进程相同的那些 TOAST 指针。
 *
 * 格式如下：首先写入一个 4 字节头部字，它可能是
 * 传引用 datum 的长度，传值 datum 为 -1，
 * NULL 则为 -2。若值为 NULL，则不再写入任何
 * 内容。若为传值，则其后跟 sizeof(Datum) 个
 * 字节。否则，其后跟头部字所指示的字节数，
 * 调用方负责确保有足够的存储空间来容纳将要写入
 * 的字节数；可使用 datumEstimateSpace() 来了解
 * 具体需要多少字节。
 * *start_address 更新为指向紧随已写
 * 的字节。
 *-------------------------------------------------------------------------
 */
void
datumSerialize(Datum value, bool isnull, bool typByVal, int typLen,
			   char **start_address)
{
	ExpandedObjectHeader *eoh = NULL;
	int			header;

	/* 写头部字。 */
	if (isnull)
		header = -2;
	else if (typByVal)
		header = -1;
	else if (typLen == -1 &&
			 VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(value)))
	{
		eoh = DatumGetEOHP(value);
		header = EOH_get_flat_size(eoh);
	}
	else
		header = datumGetSize(value, typByVal, typLen);
	memcpy(*start_address, &header, sizeof(int));
	*start_address += sizeof(int);

	/* 非 NULL 则写载荷字节。 */
	if (!isnull)
	{
		if (typByVal)
		{
			memcpy(*start_address, &value, sizeof(Datum));
			*start_address += sizeof(Datum);
		}
		else if (eoh)
		{
			char	   *tmp;

			/*
			 * EOH_flatten_into 期望目标地址按最大对齐，
			 * 因此不能直接存入 *start_address。
			 */
			tmp = (char *) palloc(header);
			EOH_flatten_into(eoh, (void *) tmp, header);
			memcpy(*start_address, tmp, header);
			*start_address += header;

			/*收尾。*/
			pfree(tmp);
		}
		else
		{
			memcpy(*start_address, DatumGetPointer(value), header);
			*start_address += header;
		}
	}
}

/*-------------------------------------------------------------------------
 * datumRestore
 *
 * 恢复由 datumSerialize 序列化的可能为 NULL 的 datum。
 * *start_address 会按已消耗的字节数相应更新。
 *-------------------------------------------------------------------------
 */
Datum
datumRestore(char **start_address, bool *isnull)
{
	int			header;
	void	   *d;

	/* 读头部字。 */
	memcpy(&header, *start_address, sizeof(int));
	*start_address += sizeof(int);

	/* datum 为 NULL 则到此为止。 */
	if (header == -2)
	{
		*isnull = true;
		return (Datum) 0;
	}

	/* OK，datum 非空。 */
	*isnull = false;

	/* datum 为传值时其后为 sizeof(Datum) 个字节。 */
	if (header == -1)
	{
		Datum		val;

		memcpy(&val, *start_address, sizeof(Datum));
		*start_address += sizeof(Datum);
		return val;
	}

	/* 传引用情形；复制指定数量的字节。 */
	Assert(header > 0);
	d = palloc(header);
	memcpy(d, *start_address, header);
	*start_address += header;
	return PointerGetDatum(d);
}
