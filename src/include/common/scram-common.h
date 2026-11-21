/*-------------------------------------------------------------------------
 *
 * scram-common.h
 *		SCRAM 认证所用辅助函数的声明
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/scram-common.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SCRAM_COMMON_H
#define SCRAM_COMMON_H

#include "common/cryptohash.h"
#include "common/sha2.h"

/*  按 IANA 的 SCRAM 机制名  */
#define SCRAM_SHA_256_NAME "SCRAM-SHA-256"
#define SCRAM_SHA_256_PLUS_NAME "SCRAM-SHA-256-PLUS"	/*  带信道绑定  */

/* SCRAM 密钥长（客户/服务器） */
#define SCRAM_SHA_256_KEY_LEN				PG_SHA256_DIGEST_LENGTH

/*
 * SCRAM 例程内部缓冲大小，应为所支持散列方法中
 * SCRAM_SHA_*_KEY_LEN 的最大值。
 */
#define SCRAM_MAX_KEY_LEN					SCRAM_SHA_256_KEY_LEN

/*
 * 认证交换中生成的随机 nonce 大小。这是"原始"
 * 字节数，线上实际发送的 nonce 仅用 ASCII 可打印
 * 字符编码。
 */
#define SCRAM_RAW_NONCE_LEN			18

/*
 * 生成新秘密时盐的长度（字节）。（存盘与线上以
 * Base64 编码发送。）16 字节是 RFC 7677 示例所用。
 * 略
 */
#define SCRAM_DEFAULT_SALT_LEN		16

/*
 * 生成秘密时的默认迭代次数。按 RFC 7677 应至少
 * 4096。
 */
#define SCRAM_SHA_256_DEFAULT_ITERATIONS	4096

extern int	scram_SaltedPassword(const char *password,
								 pg_cryptohash_type hash_type, int key_length,
								 const char *salt, int saltlen, int iterations,
								 uint8 *result, const char **errstr);
extern int	scram_H(const uint8 *input, pg_cryptohash_type hash_type,
					int key_length, uint8 *result,
					const char **errstr);
extern int	scram_ClientKey(const uint8 *salted_password,
							pg_cryptohash_type hash_type, int key_length,
							uint8 *result, const char **errstr);
extern int	scram_ServerKey(const uint8 *salted_password,
							pg_cryptohash_type hash_type, int key_length,
							uint8 *result, const char **errstr);

extern char *scram_build_secret(pg_cryptohash_type hash_type, int key_length,
								const char *salt, int saltlen, int iterations,
								const char *password, const char **errstr);

#endif							/* SCRAM_COMMON_H */
