/*-------------------------------------------------------------------------
 *
 * toast_compression.c
 *	  TOAST 压缩函数。
 *
 * Copyright (c) 2021-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_compression.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "common/pg_lzcompress.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "varatt.h"

/* GUC 参数 */
int			default_toast_compression = TOAST_PGLZ_COMPRESSION;

#define NO_LZ4_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method lz4 not supported"), \
			 errdetail("This functionality requires the server to be built with lz4 support.")))

/*
 * 用 PGLZ 压缩一个 varlena。
 *
 * 返回压缩后的 varlena；若压缩失败则返回 NULL。
 */
struct varlena *
pglz_compress_datum(const struct varlena *value)
{
	int32		valsize,
				len;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * 若值的大小超出允许压缩的范围，就没必要
	 * 白做一次 palloc。
	 */
	if (valsize < PGLZ_strategy_default->min_input_size ||
		valsize > PGLZ_strategy_default->max_input_size)
		return NULL;

	/*
	 * 算出 pglz 输出的最大可能大小，加上 varlena 开销
	 * 所需的字节，然后分配那么多空间。
	 */
	tmp = (struct varlena *) palloc(PGLZ_MAX_OUTPUT(valsize) +
									VARHDRSZ_COMPRESSED);

	len = pglz_compress(VARDATA_ANY(value),
						valsize,
						(char *) tmp + VARHDRSZ_COMPRESSED,
						NULL);
	if (len < 0)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
}

/*
 * 解压一个用 PGLZ 压缩过的 varlena。
 */
struct varlena *
pglz_decompress_datum(const struct varlena *value)
{
	struct varlena *result;
	int32		rawsize;

	/* 为未压缩数据分配内存 */
	result = (struct varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* 解压数据 */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  VARDATA_COMPRESSED_GET_EXTSIZE(value), true);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * 解压一个用 PGLZ 压缩过的 varlena 的一部分。
 */
struct varlena *
pglz_decompress_datum_slice(const struct varlena *value,
							int32 slicelength)
{
	struct varlena *result;
	int32		rawsize;

	/* 为未压缩数据分配内存 */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* 解压数据 */
	rawsize = pglz_decompress((char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  slicelength, false);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * 用 LZ4 压缩一个 varlena。
 *
 * 返回压缩后的 varlena；若压缩失败则返回 NULL。
 */
struct varlena *
lz4_compress_datum(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return NULL;				/* 消除编译器告警 */
#else
	int32		valsize;
	int32		len;
	int32		max_size;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * 算出 LZ4 输出的最大可能大小，加上 varlena 开销
	 * 所需的字节，然后分配那么多空间。
	 */
	max_size = LZ4_compressBound(valsize);
	tmp = (struct varlena *) palloc(max_size + VARHDRSZ_COMPRESSED);

	len = LZ4_compress_default(VARDATA_ANY(value),
							   (char *) tmp + VARHDRSZ_COMPRESSED,
							   valsize, max_size);
	if (len <= 0)
		elog(ERROR, "lz4 compression failed");

	/* 数据不可压缩，于是释放内存并返回 NULL */
	if (len > valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
#endif
}

/*
 * 解压一个用 LZ4 压缩过的 varlena。
 */
struct varlena *
lz4_decompress_datum(const struct varlena *value)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return NULL;				/* 消除编译器告警 */
#else
	int32		rawsize;
	struct varlena *result;

	/* 为未压缩数据分配内存 */
	result = (struct varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* 解压数据 */
	rawsize = LZ4_decompress_safe((char *) value + VARHDRSZ_COMPRESSED,
								  VARDATA(result),
								  VARSIZE(value) - VARHDRSZ_COMPRESSED,
								  VARDATA_COMPRESSED_GET_EXTSIZE(value));
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));


	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * 解压一个用 LZ4 压缩过的 varlena 的一部分。
 */
struct varlena *
lz4_decompress_datum_slice(const struct varlena *value, int32 slicelength)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return NULL;				/* 消除编译器告警 */
#else
	int32		rawsize;
	struct varlena *result;

	/* 1.8.3 之前不支持切片解压 */
	if (LZ4_versionNumber() < 10803)
		return lz4_decompress_datum(value);

	/* 为未压缩数据分配内存 */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	/* 解压数据 */
	rawsize = LZ4_decompress_safe_partial((char *) value + VARHDRSZ_COMPRESSED,
										  VARDATA(result),
										  VARSIZE(value) - VARHDRSZ_COMPRESSED,
										  slicelength,
										  slicelength);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * 从 varlena 中取出压缩 ID。
 *
 * 若该 varlena 未压缩，则返回 TOAST_INVALID_COMPRESSION_ID。
 */
ToastCompressionId
toast_get_compression_id(struct varlena *attr)
{
	ToastCompressionId cmid = TOAST_INVALID_COMPRESSION_ID;

	/*
	 * 若它存放在外部，就从外部 TOAST 指针中取出
	 * 压缩方法 id。若是内联压缩，则从
	 * TOAST 压缩头中取出。
	 */
	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			cmid = VARATT_EXTERNAL_GET_COMPRESS_METHOD(toast_pointer);
	}
	else if (VARATT_IS_COMPRESSED(attr))
		cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(attr);

	return cmid;
}

/*
 * CompressionNameToMethod - 由压缩名取得压缩方法
 *
 * 在可用的内建方法中搜索。若在内建方法中找不到
 * 该压缩方法，则返回 InvalidCompressionMethod。
 */
char
CompressionNameToMethod(const char *compression)
{
	if (strcmp(compression, "pglz") == 0)
		return TOAST_PGLZ_COMPRESSION;
	else if (strcmp(compression, "lz4") == 0)
	{
#ifndef USE_LZ4
		NO_LZ4_SUPPORT();
#endif
		return TOAST_LZ4_COMPRESSION;
	}

	return InvalidCompressionMethod;
}

/*
 * GetCompressionMethodName - 取得压缩方法名
 */
const char *
GetCompressionMethodName(char method)
{
	switch (method)
	{
		case TOAST_PGLZ_COMPRESSION:
			return "pglz";
		case TOAST_LZ4_COMPRESSION:
			return "lz4";
		default:
			elog(ERROR, "invalid compression method %c", method);
			return NULL;		/* 消除编译器告警 */
	}
}
