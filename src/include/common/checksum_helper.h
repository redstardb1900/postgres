/*-------------------------------------------------------------------------
 *
 * checksum_helper.h
 *	  用公共例程计算多种类型的校验和
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/include/common/checksum_helper.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CHECKSUM_HELPER_H
#define CHECKSUM_HELPER_H

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "port/pg_crc32c.h"

/*
 * 支持的校验和类型。使用这些函数的代码未必需要
 * 密码学强校验和；可能只需检测意外修改。故含
 * CRC-32C：比其他算法快得多。另一方面省略 MD5，
 * 因任何新需要密码学强校验和之处应选用更好的。
 * 略
 * 略
 */
typedef enum pg_checksum_type
{
	CHECKSUM_TYPE_NONE,
	CHECKSUM_TYPE_CRC32C,
	CHECKSUM_TYPE_SHA224,
	CHECKSUM_TYPE_SHA256,
	CHECKSUM_TYPE_SHA384,
	CHECKSUM_TYPE_SHA512
} pg_checksum_type;

/*
 * 这只是所有适用上下文类型的联合。
 */
typedef union pg_checksum_raw_context
{
	pg_crc32c	c_crc32c;
	pg_cryptohash_ctx *c_sha2;
} pg_checksum_raw_context;

/*
 * 本结构便于把校验和类型与上下文一起传递。
 * 略
 */
typedef struct pg_checksum_context
{
	pg_checksum_type type;
	pg_checksum_raw_context raw_context;
} pg_checksum_context;

/*
 * 本文件所支持任一校验和算法的最长可能输出。
 * 略
 */
#define PG_CHECKSUM_MAX_LENGTH		PG_SHA512_DIGEST_LENGTH

extern bool pg_checksum_parse_type(char *name, pg_checksum_type *);
extern char *pg_checksum_type_name(pg_checksum_type);

extern int	pg_checksum_init(pg_checksum_context *, pg_checksum_type);
extern int	pg_checksum_update(pg_checksum_context *, const uint8 *input,
							   size_t len);
extern int	pg_checksum_final(pg_checksum_context *, uint8 *output);

#endif
