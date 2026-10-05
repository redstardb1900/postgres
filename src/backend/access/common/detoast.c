/*-------------------------------------------------------------------------
 *
 * detoast.c
 *	  取回经过压缩或外部存储的变长属性。
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/common/detoast.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "common/int.h"
#include "common/pg_lzcompress.h"
#include "utils/expandeddatum.h"
#include "utils/rel.h"

static struct varlena *toast_fetch_datum(struct varlena *attr);
static struct varlena *toast_fetch_datum_slice(struct varlena *attr,
											   int32 sliceoffset,
											   int32 slicelength);
static struct varlena *toast_decompress_datum(struct varlena *attr);
static struct varlena *toast_decompress_datum_slice(struct varlena *attr, int32 slicelength);

/* ----------
 * detoast_external_attr -
 *
 *	从外部来源取回已 TOAST 的值的公开入口
 *	（结果仍可能是压缩格式）。
 *
 * 返回的 datum 在内部包含全部数据，也就是不依赖
 * 外部存储或内存，但仍可能是压缩的，或带有短头。
 * 注意：有些调用方假定若输入是
 * EXTERNAL datum，结果将是可用 pfree() 释放的块。
 * ----------
 */
struct varlena *
detoast_external_attr(struct varlena *attr)
{
	struct varlena *result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/*
		 * 这是外部存储的普通值
		 */
		result = toast_fetch_datum(attr);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		/*
		 * 这是间接指针 --- 解引用它
		 */
		struct varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);
		attr = (struct varlena *) redirect.pointer;

		/* 不允许嵌套的间接 Datum */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		/* 若值仍以其他方式存放在外部，则递归 */
		if (VARATT_IS_EXTERNAL(attr))
			return detoast_external_attr(attr);

		/*
		 * 复制到调用方的内存上下文，以防调用方试图
		 * 对结果做 pfree。
		 */
		result = (struct varlena *) palloc(VARSIZE_ANY(attr));
		memcpy(result, attr, VARSIZE_ANY(attr));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/*
		 * 这是展开对象指针 --- 取得扁平格式
		 */
		ExpandedObjectHeader *eoh;
		Size		resultsize;

		eoh = DatumGetEOHP(PointerGetDatum(attr));
		resultsize = EOH_get_flat_size(eoh);
		result = (struct varlena *) palloc(resultsize);
		EOH_flatten_into(eoh, (void *) result, resultsize);
	}
	else
	{
		/*
		 * 这是主元组里的普通值 --- 为什么会调用我？
		 */
		result = attr;
	}

	return result;
}


/* ----------
 * detoast_attr -
 *
 *	从压缩或外部存储取回已 TOAST 值的公开入口。
 *	结果始终是非扩展的 varlena 形式。
 *
 * 注意：有些调用方假定若输入是 EXTERNAL 或 COMPRESSED
 * datum，结果将是可用 pfree() 释放的块。
 * ----------
 */
struct varlena *
detoast_attr(struct varlena *attr)
{
	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/*
		 * 这是外部存储的 datum --- 从那里取回
		 */
		attr = toast_fetch_datum(attr);
		/* 若它是压缩的，就解压 */
		if (VARATT_IS_COMPRESSED(attr))
		{
			struct varlena *tmp = attr;

			attr = toast_decompress_datum(tmp);
			pfree(tmp);
		}
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		/*
		 * 这是间接指针 --- 解引用它
		 */
		struct varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);
		attr = (struct varlena *) redirect.pointer;

		/* 不允许嵌套的间接 Datum */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		/* 若值仍以其他方式处于扩展形式，则递归 */
		attr = detoast_attr(attr);

		/* 若并非如此，最好复制一份 */
		if (attr == (struct varlena *) redirect.pointer)
		{
			struct varlena *result;

			result = (struct varlena *) palloc(VARSIZE_ANY(attr));
			memcpy(result, attr, VARSIZE_ANY(attr));
			attr = result;
		}
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/*
		 * 这是展开对象指针 --- 取得扁平格式
		 */
		attr = detoast_external_attr(attr);
		/* 展平函数不允许产生压缩或短头输出 */
		Assert(!VARATT_IS_EXTENDED(attr));
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/*
		 * 这是主元组内部的压缩值
		 */
		attr = toast_decompress_datum(attr);
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * 这是短头 varlena --- 转换成 4 字节头格式
		 */
		Size		data_size = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
		Size		new_size = data_size + VARHDRSZ;
		struct varlena *new_attr;

		new_attr = (struct varlena *) palloc(new_size);
		SET_VARSIZE(new_attr, new_size);
		memcpy(VARDATA(new_attr), VARDATA_SHORT(attr), data_size);
		attr = new_attr;
	}

	return attr;
}


/* ----------
 * detoast_attr_slice -
 *
 *		从压缩或外部存储取回已 TOAST 值
 *		一部分的公开入口。
 *
 * sliceoffset 是起始位置（零或更大）
 * 若 slicelength < 0，则返回 sliceoffset 之后的全部内容
 * ----------
 */
struct varlena *
detoast_attr_slice(struct varlena *attr,
				   int32 sliceoffset, int32 slicelength)
{
	struct varlena *preslice;
	struct varlena *result;
	char	   *attrdata;
	int32		slicelimit;
	int32		attrsize;

	if (sliceoffset < 0)
		elog(ERROR, "invalid sliceoffset: %d", sliceoffset);

	/*
	 * 计算 slicelimit = 偏移 + 长度；若必须取回整个值则为 -1。
	 * 整数溢出时必须全部取回。
	 */
	if (slicelength < 0)
		slicelimit = -1;
	else if (pg_add_s32_overflow(sliceoffset, slicelength, &slicelimit))
		slicelength = slicelimit = -1;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* 未压缩外部 datum 的快速路径 */
		if (!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			return toast_fetch_datum_slice(attr, sliceoffset, slicelength);

		/*
		 * 对压缩值，需要取回足够的切片，以便至少解压
		 * 出所请求的部分（当请求的是前缀时）。
		 * 否则就取回全部切片。
		 */
		if (slicelimit >= 0)
		{
			int32		max_size = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);

			/*
			 * 确定给定长度的前缀（解压之后）最多需要
			 * 多少压缩数据。
			 *
			 * 至少目前，若是 LZ4 数据，就得整段取回，
			 * 因为似乎没有哪个 API 调用能确定需要多少
			 * 压缩数据，才能有把握解压出
			 * 所需的切片。
			 */
			if (VARATT_EXTERNAL_GET_COMPRESS_METHOD(toast_pointer) ==
				TOAST_PGLZ_COMPRESSION_ID)
				max_size = pglz_maximum_compressed_size(slicelimit, max_size);

			/*
			 * 取回足够的压缩切片（压缩标记会
			 * 自动设置）。
			 */
			preslice = toast_fetch_datum_slice(attr, 0, max_size);
		}
		else
			preslice = toast_fetch_datum(attr);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		struct varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);

		/* 不允许嵌套的间接 Datum */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(redirect.pointer));

		return detoast_attr_slice(redirect.pointer,
								  sliceoffset, slicelength);
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/* 交给 detoast_external_attr 去做扁平化 */
		preslice = detoast_external_attr(attr);
	}
	else
		preslice = attr;

	Assert(!VARATT_IS_EXTERNAL(preslice));

	if (VARATT_IS_COMPRESSED(preslice))
	{
		struct varlena *tmp = preslice;

		/* 解压到足以覆盖该切片和偏移 */
		if (slicelimit >= 0)
			preslice = toast_decompress_datum_slice(tmp, slicelimit);
		else
			preslice = toast_decompress_datum(tmp);

		if (tmp != attr)
			pfree(tmp);
	}

	if (VARATT_IS_SHORT(preslice))
	{
		attrdata = VARDATA_SHORT(preslice);
		attrsize = VARSIZE_SHORT(preslice) - VARHDRSZ_SHORT;
	}
	else
	{
		attrdata = VARDATA(preslice);
		attrsize = VARSIZE(preslice) - VARHDRSZ;
	}

	/* 对压缩情形和普通值做 datum 切片 */

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}
	else if (slicelength < 0 || slicelimit > attrsize)
		slicelength = attrsize - sliceoffset;

	result = (struct varlena *) palloc(slicelength + VARHDRSZ);
	SET_VARSIZE(result, slicelength + VARHDRSZ);

	memcpy(VARDATA(result), attrdata + sliceoffset, slicelength);

	if (preslice != attr)
		pfree(preslice);

	return result;
}

/* ----------
 * toast_fetch_datum -
 *
 *	根据保存在 toast 关系中的各个块，
 *	在内存里重组出 Datum
 * ----------
 */
static struct varlena *
toast_fetch_datum(struct varlena *attr)
{
	Relation	toastrel;
	struct varlena *result;
	struct varatt_external toast_pointer;
	int32		attrsize;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		elog(ERROR, "toast_fetch_datum shouldn't be called for non-ondisk datums");

	/* 必须复制才能访问已对齐的字段 */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	attrsize = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);

	result = (struct varlena *) palloc(attrsize + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
		SET_VARSIZE_COMPRESSED(result, attrsize + VARHDRSZ);
	else
		SET_VARSIZE(result, attrsize + VARHDRSZ);

	if (attrsize == 0)
		return result;			/* 按理不该发生，不过
								 * 以防万一。 */

	/*
	 * 打开 toast 关系及其索引
	 */
	toastrel = table_open(toast_pointer.va_toastrelid, AccessShareLock);

	/* 取回全部块 */
	table_relation_fetch_toast_slice(toastrel, toast_pointer.va_valueid,
									 attrsize, 0, attrsize, result);

	/* 关闭 toast 表 */
	table_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_fetch_datum_slice -
 *
 *	根据保存在 toast 关系中的各个块，重组出 Datum 的一段
 *	在内存里重组出 Datum
 *
 *	注意：本函数支持未压缩的外部 datum，
 *	也支持压缩的外部 datum（此时所请求的切片
 *	必须是前缀，即 sliceoffset 必须为 0）。
 * ----------
 */
static struct varlena *
toast_fetch_datum_slice(struct varlena *attr, int32 sliceoffset,
						int32 slicelength)
{
	Relation	toastrel;
	struct varlena *result;
	struct varatt_external toast_pointer;
	int32		attrsize;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		elog(ERROR, "toast_fetch_datum_slice shouldn't be called for non-ondisk datums");

	/* 必须复制才能访问已对齐的字段 */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
	 * 除非取的是前缀，否则去取压缩 datum 的切片毫无意义
	 * --- 这不是 lo_*，我们不能返回一份以后对 toast
	 * 仍有意义的压缩 datum。
	 */
	Assert(!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer) || 0 == sliceoffset);

	attrsize = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}

	/*
	 * 取压缩外部 datum 的前缀时，要计入 va_tcinfo
	 * 所需的空间。它以 int32 值的形式
	 * 存放在开头。
	 */
	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer) && slicelength > 0)
		slicelength = slicelength + sizeof(int32);

	/*
	 * 必要时调整长度请求。（注意：唯一的调用方
	 * detoast_attr_slice 会防止 sliceoffset + slicelength
	 * 溢出。）
	 */
	if (((sliceoffset + slicelength) > attrsize) || slicelength < 0)
		slicelength = attrsize - sliceoffset;

	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
		SET_VARSIZE_COMPRESSED(result, slicelength + VARHDRSZ);
	else
		SET_VARSIZE(result, slicelength + VARHDRSZ);

	if (slicelength == 0)
		return result;			/* 此时可以省下大量工作！ */

	/* 打开 toast 关系 */
	toastrel = table_open(toast_pointer.va_toastrelid, AccessShareLock);

	/* 取回全部块 */
	table_relation_fetch_toast_slice(toastrel, toast_pointer.va_valueid,
									 attrsize, sliceoffset, slicelength,
									 result);

	/* 关闭 toast 表 */
	table_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_decompress_datum -
 *
 * 解压 varlena datum 的压缩版本
 */
static struct varlena *
toast_decompress_datum(struct varlena *attr)
{
	ToastCompressionId cmid;

	Assert(VARATT_IS_COMPRESSED(attr));

	/*
	 * 取出存放在压缩头里的压缩方法 id，并用相应的
	 * 解压例程解压数据。
	 */
	cmid = TOAST_COMPRESS_METHOD(attr);
	switch (cmid)
	{
		case TOAST_PGLZ_COMPRESSION_ID:
			return pglz_decompress_datum(attr);
		case TOAST_LZ4_COMPRESSION_ID:
			return lz4_decompress_datum(attr);
		default:
			elog(ERROR, "invalid compression method id %d", cmid);
			return NULL;		/* 消除编译器告警 */
	}
}


/* ----------
 * toast_decompress_datum_slice -
 *
 * 解压 varlena datum 压缩版本的前部。
 * 偏移处理在 detoast_attr_slice 中完成。
 * 这里只从前部解压一个切片。
 */
static struct varlena *
toast_decompress_datum_slice(struct varlena *attr, int32 slicelength)
{
	ToastCompressionId cmid;

	Assert(VARATT_IS_COMPRESSED(attr));

	/*
	 * 有些调用方传入的 slicelength 可能大于实际
	 * 解压后的大小。若是这样，就按普通方式解压。这样
	 * 可以避免分配一个比所需更大的结果对象，而且可能
	 * 更快，也更稳妥。特别是，曾见过某些版本的 liblz4
	 * 在传入的输出大小大于数据真实解压大小时
	 * 给出错误结果。
	 */
	if ((uint32) slicelength >= TOAST_COMPRESS_EXTSIZE(attr))
		return toast_decompress_datum(attr);

	/*
	 * 取出存放在压缩头里的压缩方法 id，并用相应的
	 * 解压例程解压数据切片。
	 */
	cmid = TOAST_COMPRESS_METHOD(attr);
	switch (cmid)
	{
		case TOAST_PGLZ_COMPRESSION_ID:
			return pglz_decompress_datum_slice(attr, slicelength);
		case TOAST_LZ4_COMPRESSION_ID:
			return lz4_decompress_datum_slice(attr, slicelength);
		default:
			elog(ERROR, "invalid compression method id %d", cmid);
			return NULL;		/* 消除编译器告警 */
	}
}

/* ----------
 * toast_raw_datum_size -
 *
 *	返回 varlena datum 的原始（还原后）大小
 *	（含 VARHDRSZ 头）
 * ----------
 */
Size
toast_raw_datum_size(Datum value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/* va_rawsize 是原始 datum 的大小 -- 含头部 */
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = toast_pointer.va_rawsize;
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		struct varatt_indirect toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* 不允许嵌套的间接 Datum */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(toast_pointer.pointer));

		return toast_raw_datum_size(PointerGetDatum(toast_pointer.pointer));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		result = EOH_get_flat_size(DatumGetEOHP(value));
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/* 这里 va_rawsize 只是有效载荷大小 */
		result = VARDATA_COMPRESSED_GET_EXTSIZE(attr) + VARHDRSZ;
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * 必须把头长度规范化为 VARHDRSZ，否则本函数的
		 * 调用方会搞混。
		 */
		result = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT + VARHDRSZ;
	}
	else
	{
		/* 普通的未还原 datum */
		result = VARSIZE(attr);
	}
	return result;
}

/* ----------
 * toast_datum_size
 *
 *	返回 varlena datum 的物理存储大小（可能已压缩）
 * ----------
 */
Size
toast_datum_size(Datum value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/*
		 * 属性存放在外部 --- 无论是否压缩都返回 extsize。
		 * 我们不把 toast 指针的大小计算在内
		 * ……应该算吗？
		 */
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		struct varatt_indirect toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* 不允许嵌套的间接 Datum */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		return toast_datum_size(PointerGetDatum(toast_pointer.pointer));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		result = EOH_get_flat_size(DatumGetEOHP(value));
	}
	else if (VARATT_IS_SHORT(attr))
	{
		result = VARSIZE_SHORT(attr);
	}
	else
	{
		/*
		 * 属性以内联方式存放，无论是否压缩，两种情况下
		 * 都只计算 datum 的大小。
		 */
		result = VARSIZE(attr);
	}
	return result;
}
