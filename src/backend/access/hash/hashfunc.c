/*-------------------------------------------------------------------------
 *
 * hashfunc.c
 *	  hash 访问方法的支持函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashfunc.c
 *
 * 说明（NOTES）
 *	  这些函数存放在 pg_amproc 中。对为 hash 索引
 *	  定义的每个操作符类，它们计算参数的哈希值。
 *
 *	  更多哈希函数出现在各种专用数据类型的
 *	  /utils/adt/ 文件中。
 *
 *	  预期哈希函数 32 位结果的每一位都与其他位
 *	  同样随机；做不到这一点很可能导致哈希连接等
 *	  性能变差。多数情况下哈希函数应使用 hash_any()
 *	  或其变体 hash_uint32()。
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_collation.h"
#include "common/hashfn.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/pg_locale.h"
#include "varatt.h"

/*
 * 特定数据类型的哈希函数。
 *
 * 它们同时支持 hash 索引和哈希连接。
 *
 * 注意：其中一些也被 catcache 操作使用，与
 * hash 索引没有直接关系。另外，公共的 hash_any
 * 例程也被 dynahash 表使用。
 */

/* 注意：同时用于 "char" 和布尔数据类型 */
Datum
hashchar(PG_FUNCTION_ARGS)
{
	return hash_uint32((int32) PG_GETARG_CHAR(0));
}

Datum
hashcharextended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((int32) PG_GETARG_CHAR(0), PG_GETARG_INT64(1));
}

Datum
hashint2(PG_FUNCTION_ARGS)
{
	return hash_uint32((int32) PG_GETARG_INT16(0));
}

Datum
hashint2extended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((int32) PG_GETARG_INT16(0), PG_GETARG_INT64(1));
}

Datum
hashint4(PG_FUNCTION_ARGS)
{
	return hash_uint32(PG_GETARG_INT32(0));
}

Datum
hashint4extended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended(PG_GETARG_INT32(0), PG_GETARG_INT64(1));
}

Datum
hashint8(PG_FUNCTION_ARGS)
{
	/*
	 * 这里的想法是产生与 hashint4 和 hashint2 对逻辑上
	 * 相等的输入所产生的值兼容的哈希值；这是为了支持
	 * 这些输入类型之间的跨类型哈希连接所必需的。
	 * 由于这三种类型都是有符号的，符号为正时可以把
	 * int8 值的高半部分异或进去，符号为负时则异或
	 * 高半部分的补码。
	 */
	int64		val = PG_GETARG_INT64(0);
	uint32		lohalf = (uint32) val;
	uint32		hihalf = (uint32) (val >> 32);

	lohalf ^= (val >= 0) ? hihalf : ~hihalf;

	return hash_uint32(lohalf);
}

Datum
hashint8extended(PG_FUNCTION_ARGS)
{
	/* 与 hashint8 相同的做法 */
	int64		val = PG_GETARG_INT64(0);
	uint32		lohalf = (uint32) val;
	uint32		hihalf = (uint32) (val >> 32);

	lohalf ^= (val >= 0) ? hihalf : ~hihalf;

	return hash_uint32_extended(lohalf, PG_GETARG_INT64(1));
}

Datum
hashoid(PG_FUNCTION_ARGS)
{
	return hash_uint32((uint32) PG_GETARG_OID(0));
}

Datum
hashoidextended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((uint32) PG_GETARG_OID(0), PG_GETARG_INT64(1));
}

Datum
hashenum(PG_FUNCTION_ARGS)
{
	return hash_uint32((uint32) PG_GETARG_OID(0));
}

Datum
hashenumextended(PG_FUNCTION_ARGS)
{
	return hash_uint32_extended((uint32) PG_GETARG_OID(0), PG_GETARG_INT64(1));
}

Datum
hashfloat4(PG_FUNCTION_ARGS)
{
	float4		key = PG_GETARG_FLOAT4(0);
	float8		key8;

	/*
	 * 在 IEEE 浮点机器上，负零与零的位模式不同，
	 * 但比较时应相等。我们必须保证它们具有相同的
	 * 哈希值，最可靠的做法如下：
	 */
	if (key == (float4) 0)
		PG_RETURN_UINT32(0);

	/*
	 * 为支持 float8 与 float4 的跨类型哈希，我们希望返回
	 * 与 hashfloat8 对相等的 float8 值所产生的相同哈希值。
	 * 因此把该值拓宽为 float8 再做哈希。（必须这样做，
	 * 而不能让 hashfloat8 试图把值收窄为 float4；那样
	 * 可能因溢出而失败。）
	 */
	key8 = key;

	/*
	 * 类似地，NaNs 可以有不同的位模式，但它们比较时
	 * 都应相等。出于向后兼容，我们强制它们具有标准
	 * float8 NaN 的哈希值。（也许会以为可以把 key
	 * 换成 float4 NaN 再拓宽；但在某些旧平台上，
	 * 那样会产生不同的位模式。）
	 */
	if (isnan(key8))
		key8 = get_float8_nan();

	return hash_any((unsigned char *) &key8, sizeof(key8));
}

Datum
hashfloat4extended(PG_FUNCTION_ARGS)
{
	float4		key = PG_GETARG_FLOAT4(0);
	uint64		seed = PG_GETARG_INT64(1);
	float8		key8;

	/* 与 hashfloat4 相同的做法 */
	if (key == (float4) 0)
		PG_RETURN_UINT64(seed);
	key8 = key;
	if (isnan(key8))
		key8 = get_float8_nan();

	return hash_any_extended((unsigned char *) &key8, sizeof(key8), seed);
}

Datum
hashfloat8(PG_FUNCTION_ARGS)
{
	float8		key = PG_GETARG_FLOAT8(0);

	/*
	 * 在 IEEE 浮点机器上，负零与零的位模式不同，
	 * 但比较时应相等。我们必须保证它们具有相同的
	 * 哈希值，最可靠的做法如下：
	 */
	if (key == (float8) 0)
		PG_RETURN_UINT32(0);

	/*
	 * 类似地，NaNs 可以有不同的位模式，但它们比较时
	 * 都应相等。出于向后兼容，我们强制它们具有标准
	 * NaN 的哈希值。
	 */
	if (isnan(key))
		key = get_float8_nan();

	return hash_any((unsigned char *) &key, sizeof(key));
}

Datum
hashfloat8extended(PG_FUNCTION_ARGS)
{
	float8		key = PG_GETARG_FLOAT8(0);
	uint64		seed = PG_GETARG_INT64(1);

	/* 与 hashfloat8 相同的做法 */
	if (key == (float8) 0)
		PG_RETURN_UINT64(seed);
	if (isnan(key))
		key = get_float8_nan();

	return hash_any_extended((unsigned char *) &key, sizeof(key), seed);
}

Datum
hashoidvector(PG_FUNCTION_ARGS)
{
	oidvector  *key = (oidvector *) PG_GETARG_POINTER(0);

	return hash_any((unsigned char *) key->values, key->dim1 * sizeof(Oid));
}

Datum
hashoidvectorextended(PG_FUNCTION_ARGS)
{
	oidvector  *key = (oidvector *) PG_GETARG_POINTER(0);

	return hash_any_extended((unsigned char *) key->values,
							 key->dim1 * sizeof(Oid),
							 PG_GETARG_INT64(1));
}

Datum
hashname(PG_FUNCTION_ARGS)
{
	char	   *key = NameStr(*PG_GETARG_NAME(0));

	return hash_any((unsigned char *) key, strlen(key));
}

Datum
hashnameextended(PG_FUNCTION_ARGS)
{
	char	   *key = NameStr(*PG_GETARG_NAME(0));

	return hash_any_extended((unsigned char *) key, strlen(key),
							 PG_GETARG_INT64(1));
}

Datum
hashtext(PG_FUNCTION_ARGS)
{
	text	   *key = PG_GETARG_TEXT_PP(0);
	Oid			collid = PG_GET_COLLATION();
	pg_locale_t mylocale = 0;
	Datum		result;

	if (!collid)
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string hashing"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));

	if (!lc_collate_is_c(collid))
		mylocale = pg_newlocale_from_collation(collid);

	if (pg_locale_deterministic(mylocale))
	{
		result = hash_any((unsigned char *) VARDATA_ANY(key),
						  VARSIZE_ANY_EXHDR(key));
	}
	else
	{
		Size		bsize,
					rsize;
		char	   *buf;
		const char *keydata = VARDATA_ANY(key);
		size_t		keylen = VARSIZE_ANY_EXHDR(key);


		bsize = pg_strnxfrm(NULL, 0, keydata, keylen, mylocale);
		buf = palloc(bsize + 1);

		rsize = pg_strnxfrm(buf, bsize + 1, keydata, keylen, mylocale);

		/* 第二次调用返回的值可能比第一次更小 */
		if (rsize > bsize)
			elog(ERROR, "pg_strnxfrm() returned unexpected result");

		/*
		 * 原则上没有理由把结尾的 NUL 字符
		 * 纳入哈希，但以前就是这样做的，该行为必须
		 * 保留。
		 */
		result = hash_any((uint8_t *) buf, bsize + 1);

		pfree(buf);
	}

	/* 避免已 TOAST 输入泄漏内存 */
	PG_FREE_IF_COPY(key, 0);

	return result;
}

Datum
hashtextextended(PG_FUNCTION_ARGS)
{
	text	   *key = PG_GETARG_TEXT_PP(0);
	Oid			collid = PG_GET_COLLATION();
	pg_locale_t mylocale = 0;
	Datum		result;

	if (!collid)
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string hashing"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));

	if (!lc_collate_is_c(collid))
		mylocale = pg_newlocale_from_collation(collid);

	if (pg_locale_deterministic(mylocale))
	{
		result = hash_any_extended((unsigned char *) VARDATA_ANY(key),
								   VARSIZE_ANY_EXHDR(key),
								   PG_GETARG_INT64(1));
	}
	else
	{
		Size		bsize,
					rsize;
		char	   *buf;
		const char *keydata = VARDATA_ANY(key);
		size_t		keylen = VARSIZE_ANY_EXHDR(key);

		bsize = pg_strnxfrm(NULL, 0, keydata, keylen, mylocale);
		buf = palloc(bsize + 1);

		rsize = pg_strnxfrm(buf, bsize + 1, keydata, keylen, mylocale);

		/* 第二次调用返回的值可能比第一次更小 */
		if (rsize > bsize)
			elog(ERROR, "pg_strnxfrm() returned unexpected result");

		/*
		 * 原则上没有理由把结尾的 NUL 字符
		 * 纳入哈希，但以前就是这样做的，该行为必须
		 * 保留。
		 */
		result = hash_any_extended((uint8_t *) buf, bsize + 1,
								   PG_GETARG_INT64(1));

		pfree(buf);
	}

	PG_FREE_IF_COPY(key, 0);

	return result;
}

/*
 * hashvarlena() 可用于任何没有非有效位的 varlena 数据类型，
 * 即不同的位模式永远不会比较为相等。
 */
Datum
hashvarlena(PG_FUNCTION_ARGS)
{
	struct varlena *key = PG_GETARG_VARLENA_PP(0);
	Datum		result;

	result = hash_any((unsigned char *) VARDATA_ANY(key),
					  VARSIZE_ANY_EXHDR(key));

	/* 避免已 TOAST 输入泄漏内存 */
	PG_FREE_IF_COPY(key, 0);

	return result;
}

Datum
hashvarlenaextended(PG_FUNCTION_ARGS)
{
	struct varlena *key = PG_GETARG_VARLENA_PP(0);
	Datum		result;

	result = hash_any_extended((unsigned char *) VARDATA_ANY(key),
							   VARSIZE_ANY_EXHDR(key),
							   PG_GETARG_INT64(1));

	PG_FREE_IF_COPY(key, 0);

	return result;
}
