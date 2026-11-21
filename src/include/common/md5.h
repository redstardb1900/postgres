/*-------------------------------------------------------------------------
 *
 * md5.h
 *	  与 MD5 相关的常量与公共工具。
 *
 * 前后端处理 MD5 加密口令时都需要这些定义。
 * 略
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/md5.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_MD5_H
#define PG_MD5_H

/*  MD5 计算结果的大小  */
#define MD5_DIGEST_LENGTH 16
/*  MD5 的块大小  */
#define MD5_BLOCK_SIZE	64

/* 略 */
#define MD5_PASSWD_CHARSET	"0123456789abcdef"
#define MD5_PASSWD_LEN	35

/*  各 MD5 实现共用的工具，见 md5_common.c  */
extern bool pg_md5_hash(const void *buff, size_t len, char *hexsum,
						const char **errstr);
extern bool pg_md5_binary(const void *buff, size_t len, void *outbuf,
						  const char **errstr);
extern bool pg_md5_encrypt(const char *passwd, const char *salt,
						   size_t salt_len, char *buf,
						   const char **errstr);

#endif							/* PG_MD5_H */
