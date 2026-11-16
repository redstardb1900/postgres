/*-------------------------------------------------------------------------
 *
 * cryptohashfuncs.c
 *	  加密哈希函数
 *
 * Portions Copyright (c) 2018-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/cryptohashfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/cryptohash.h"
#include "common/md5.h"
#include "common/sha2.h"
#include "utils/builtins.h"
#include "varatt.h"


/*
 * MD5
 */

/* MD5 哈希 16 字节(128位)；转十六进制则翻倍 */
#define MD5_HASH_LEN  32

/*
 * 对文本值创建 MD5 哈希，以十六进制串返回。
 */
Datum
md5_text(PG_FUNCTION_ARGS)
{
	text	   *in_text = PG_GETARG_TEXT_PP(0);
	size_t		len;
	char		hexsum[MD5_HASH_LEN + 1];
	const char *errstr = NULL;

	/* 利用 varlena 元数据计算缓冲区长度 */
	len = VARSIZE_ANY_EXHDR(in_text);

	/* 取哈希结果 */
	if (pg_md5_hash(VARDATA_ANY(in_text), len, hexsum, &errstr) == false)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not compute %s hash: %s", "MD5",
						errstr)));

	/* 转换为 text 并返回 */
	PG_RETURN_TEXT_P(cstring_to_text(hexsum));
}

/*
 * 对 bytea 值创建 MD5 哈希，以十六进制串返回。
 */
Datum
md5_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	size_t		len;
	char		hexsum[MD5_HASH_LEN + 1];
	const char *errstr = NULL;

	len = VARSIZE_ANY_EXHDR(in);
	if (pg_md5_hash(VARDATA_ANY(in), len, hexsum, &errstr) == false)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not compute %s hash: %s", "MD5",
						errstr)));

	PG_RETURN_TEXT_P(cstring_to_text(hexsum));
}

/*
 * 以给定 bytea 输入计算加密哈希的内部例程。
 */
static inline bytea *
cryptohash_internal(pg_cryptohash_type type, bytea *input)
{
	const uint8 *data;
	const char *typestr = NULL;
	int			digest_len = 0;
	size_t		len;
	pg_cryptohash_ctx *ctx;
	bytea	   *result;

	switch (type)
	{
		case PG_SHA224:
			typestr = "SHA224";
			digest_len = PG_SHA224_DIGEST_LENGTH;
			break;
		case PG_SHA256:
			typestr = "SHA256";
			digest_len = PG_SHA256_DIGEST_LENGTH;
			break;
		case PG_SHA384:
			typestr = "SHA384";
			digest_len = PG_SHA384_DIGEST_LENGTH;
			break;
		case PG_SHA512:
			typestr = "SHA512";
			digest_len = PG_SHA512_DIGEST_LENGTH;
			break;
		case PG_MD5:
		case PG_SHA1:
			elog(ERROR, "unsupported cryptohash type %d", type);
			break;
	}

	result = palloc0(digest_len + VARHDRSZ);
	len = VARSIZE_ANY_EXHDR(input);
	data = (unsigned char *) VARDATA_ANY(input);

	ctx = pg_cryptohash_create(type);
	if (pg_cryptohash_init(ctx) < 0)
		elog(ERROR, "could not initialize %s context: %s", typestr,
			 pg_cryptohash_error(ctx));
	if (pg_cryptohash_update(ctx, data, len) < 0)
		elog(ERROR, "could not update %s context: %s", typestr,
			 pg_cryptohash_error(ctx));
	if (pg_cryptohash_final(ctx, (unsigned char *) VARDATA(result),
							digest_len) < 0)
		elog(ERROR, "could not finalize %s context: %s", typestr,
			 pg_cryptohash_error(ctx));
	pg_cryptohash_free(ctx);

	SET_VARSIZE(result, digest_len + VARHDRSZ);

	return result;
}

/*
 * SHA-2 变体
 */

Datum
sha224_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *result = cryptohash_internal(PG_SHA224, PG_GETARG_BYTEA_PP(0));

	PG_RETURN_BYTEA_P(result);
}

Datum
sha256_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *result = cryptohash_internal(PG_SHA256, PG_GETARG_BYTEA_PP(0));

	PG_RETURN_BYTEA_P(result);
}

Datum
sha384_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *result = cryptohash_internal(PG_SHA384, PG_GETARG_BYTEA_PP(0));

	PG_RETURN_BYTEA_P(result);
}

Datum
sha512_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *result = cryptohash_internal(PG_SHA512, PG_GETARG_BYTEA_PP(0));

	PG_RETURN_BYTEA_P(result);
}
