/*-------------------------------------------------------------------------
 *
 * crypt.h
 *	  libpq/crypt.c 的接口
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/crypt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CRYPT_H
#define PG_CRYPT_H

#include "datatype/timestamp.h"

/*
 * 口令哈希或秘密的类型。
 *
 * 用户可以在 CREATE/ALTER USER 命令中传入明文口令。
 * 存盘之前它们会被加密成 MD5 或 SCRAM-SHA-256 格式，
 * 因此 pg_authid.rolpassword 中只应出现 MD5 和
 * SCRAM-SHA-256 口令。它们也是 password_encryption
 * GUC 的允许取值。
 */
typedef enum PasswordType
{
	PASSWORD_TYPE_PLAINTEXT = 0,
	PASSWORD_TYPE_MD5,
	PASSWORD_TYPE_SCRAM_SHA_256
} PasswordType;

extern PasswordType get_password_type(const char *shadow_pass);
extern char *encrypt_password(PasswordType target_type, const char *role,
							  const char *password);

extern char *get_role_password(const char *role, const char **logdetail);

extern int	md5_crypt_verify(const char *role, const char *shadow_pass,
							 const char *client_pass, const char *md5_salt,
							 int md5_salt_len, const char **logdetail);
extern int	plain_crypt_verify(const char *role, const char *shadow_pass,
							   const char *client_pass,
							   const char **logdetail);

#endif
