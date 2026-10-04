/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Functions for dealing with encrypted passwords stored in
 *	  pg_authid.rolpassword.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/libpq/crypt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "catalog/pg_authid.h"
#include "common/md5.h"
#include "common/scram-common.h"
#include "libpq/crypt.h"
#include "libpq/scram.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/*
 * 取出用户已存储的口令，供认证使用。
 *
 * 出错时返回 NULL，并把一段 palloc 分配的原因说明存入
 * *logdetail，供 postmaster 日志使用。错误原因 *不应* 发给
 * 客户端，以免泄露用户信息！
 */
char *
get_role_password(const char *role, const char **logdetail)
{
	TimestampTz vuntil = 0;
	HeapTuple	roleTup;
	Datum		datum;
	bool		isnull;
	char	   *shadow_pass;

	/* 从 pg_authid 取得角色信息 */
	roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
	if (!HeapTupleIsValid(roleTup))
	{
		*logdetail = psprintf(_("Role \"%s\" does not exist."),
							  role);
		return NULL;			/* 没有这个用户 */
	}

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolpassword, &isnull);
	if (isnull)
	{
		ReleaseSysCache(roleTup);
		*logdetail = psprintf(_("User \"%s\" has no password assigned."),
							  role);
		return NULL;			/* 用户没有口令 */
	}
	shadow_pass = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(AUTHNAME, roleTup,
							Anum_pg_authid_rolvaliduntil, &isnull);
	if (!isnull)
		vuntil = DatumGetTimestampTz(datum);

	ReleaseSysCache(roleTup);

	/*
 * 口令正确，但还要确认没有超过 rolvaliduntil
 */
	if (!isnull && vuntil < GetCurrentTimestamp())
	{
		*logdetail = psprintf(_("User \"%s\" has an expired password."),
							  role);
		return NULL;
	}

	return shadow_pass;
}

/*
 * 'shadow_pass' 是哪一种口令类型？
 */
PasswordType
get_password_type(const char *shadow_pass)
{
	char	   *encoded_salt;
	int			iterations;
	int			key_length = 0;
	pg_cryptohash_type hash_type;
	uint8		stored_key[SCRAM_MAX_KEY_LEN];
	uint8		server_key[SCRAM_MAX_KEY_LEN];

	if (strncmp(shadow_pass, "md5", 3) == 0 &&
		strlen(shadow_pass) == MD5_PASSWD_LEN &&
		strspn(shadow_pass + 3, MD5_PASSWD_CHARSET) == MD5_PASSWD_LEN - 3)
		return PASSWORD_TYPE_MD5;
	if (parse_scram_secret(shadow_pass, &iterations, &hash_type, &key_length,
						   &encoded_salt, stored_key, server_key))
		return PASSWORD_TYPE_SCRAM_SHA_256;
	return PASSWORD_TYPE_PLAINTEXT;
}

/*
 * 给定用户提供的口令，把它转换成 'target_type' 那种秘密。
 *
 * 若口令已经是加密形式，我们无法反推哈希，因此无论请求
 * 的类型是什么，都按原样存储。
 *
 */
char *
encrypt_password(PasswordType target_type, const char *role,
				 const char *password)
{
	PasswordType guessed_type = get_password_type(password);
	char	   *encrypted_password;
	const char *errstr = NULL;

	if (guessed_type != PASSWORD_TYPE_PLAINTEXT)
	{
		/*
 * 无法把已经加密的口令从一种格式转换成另一种，因此按原样
 * 返回。
 */
		return pstrdup(password);
	}

	switch (target_type)
	{
		case PASSWORD_TYPE_MD5:
			encrypted_password = palloc(MD5_PASSWD_LEN + 1);

			if (!pg_md5_encrypt(password, role, strlen(role),
								encrypted_password, &errstr))
				elog(ERROR, "password encryption failed: %s", errstr);
			return encrypted_password;

		case PASSWORD_TYPE_SCRAM_SHA_256:
			return pg_be_scram_build_secret(password);

		case PASSWORD_TYPE_PLAINTEXT:
			elog(ERROR, "cannot encrypt password with 'plaintext'");
	}

	/*
 * 这不该发生，因为上面的 switch 语句应该处理源类型和目标
 * 口令类型的每一种组合。
 */
	elog(ERROR, "cannot encrypt password to requested type");
	return NULL;				/* 让编译器安静 */
}

/*
 * 检查 MD5 认证应答，返回 STATUS_OK 或 STATUS_ERROR。
 *
 * 'shadow_pass' 是用户的正确口令或口令哈希，即存储在
 * pg_authid.rolpassword 中的值。
 * 'client_pass' 是远程用户对 MD5 质询给出的应答。
 * 'md5_salt' 是 MD5 认证质询使用的盐。
 *
 * 出错时，把一段字符串存入 *logdetail，它会写入 postmaster
 * 日志（但不发给客户端）。
 */
int
md5_crypt_verify(const char *role, const char *shadow_pass,
				 const char *client_pass,
				 const char *md5_salt, int md5_salt_len,
				 const char **logdetail)
{
	int			retval;
	char		crypt_pwd[MD5_PASSWD_LEN + 1];
	const char *errstr = NULL;

	Assert(md5_salt_len > 0);

	if (get_password_type(shadow_pass) != PASSWORD_TYPE_MD5)
	{
		/* 不兼容的口令哈希格式。 */
		*logdetail = psprintf(_("User \"%s\" has a password that cannot be used with MD5 authentication."),
							  role);
		return STATUS_ERROR;
	}

	/*
 * 计算 MD5 质询的正确答案。
 */
	/* 存储的口令已经加密，只做加盐 */
	if (!pg_md5_encrypt(shadow_pass + strlen("md5"),
						md5_salt, md5_salt_len,
						crypt_pwd, &errstr))
	{
		*logdetail = errstr;
		return STATUS_ERROR;
	}

	if (strcmp(client_pass, crypt_pwd) == 0)
		retval = STATUS_OK;
	else
	{
		*logdetail = psprintf(_("Password does not match for user \"%s\"."),
							  role);
		retval = STATUS_ERROR;
	}

	return retval;
}

/*
 * 检查给定用户的给定口令，返回 STATUS_OK 或 STATUS_ERROR。
 *
 * 'shadow_pass' 是用户的正确口令哈希，即存储在
 * pg_authid.rolpassword 中的值。
 * 'client_pass' 是远程用户给出的口令。
 *
 * 出错时，把一段字符串存入 *logdetail，它会写入 postmaster
 * 日志（但不发给客户端）。
 */
int
plain_crypt_verify(const char *role, const char *shadow_pass,
				   const char *client_pass,
				   const char **logdetail)
{
	char		crypt_client_pass[MD5_PASSWD_LEN + 1];
	const char *errstr = NULL;

	/*
 * 客户端以明文发送了口令。若我们存的是 MD5 哈希，就把客户端
 * 发来的口令做哈希，再比较哈希。否则直接比较明文口令。
 *
 */
	switch (get_password_type(shadow_pass))
	{
		case PASSWORD_TYPE_SCRAM_SHA_256:
			if (scram_verify_plain_password(role,
											client_pass,
											shadow_pass))
			{
				return STATUS_OK;
			}
			else
			{
				*logdetail = psprintf(_("Password does not match for user \"%s\"."),
									  role);
				return STATUS_ERROR;
			}
			break;

		case PASSWORD_TYPE_MD5:
			if (!pg_md5_encrypt(client_pass,
								role,
								strlen(role),
								crypt_client_pass,
								&errstr))
			{
				*logdetail = errstr;
				return STATUS_ERROR;
			}
			if (strcmp(crypt_client_pass, shadow_pass) == 0)
				return STATUS_OK;
			else
			{
				*logdetail = psprintf(_("Password does not match for user \"%s\"."),
									  role);
				return STATUS_ERROR;
			}
			break;

		case PASSWORD_TYPE_PLAINTEXT:

			/*
 * 我们从不存储明文口令，因此这不该发生。
 *
 */
			break;
	}

	/*
 * 这不该发生。纯 "password" 认证对任何已存储的口令哈希
 * 都是可能的。
 */
	*logdetail = psprintf(_("Password of user \"%s\" is in unrecognized format."),
						  role);
	return STATUS_ERROR;
}
