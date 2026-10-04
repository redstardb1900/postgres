/*-------------------------------------------------------------------------
 *
 * be-secure-common.c
 *
 * common implementation-independent SSL support code
 *
 * While be-secure.c contains the interfaces that the rest of the
 * communications code calls, this file contains support routines that are
 * used by the library-specific implementations such as be-secure-openssl.c.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure-common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "common/percentrepl.h"
#include "common/string.h"
#include "libpq/libpq.h"
#include "storage/fd.h"

/*
 * 运行 ssl_passphrase_command
 *
 * prompt 会替换 %p。is_server_start 决定错误消息的日志级别。
 *
 * 结果放进大小为 size 的缓冲区 buf。返回值是实际结果的长度。
 *
 *
 */
int
run_ssl_passphrase_command(const char *prompt, bool is_server_start, char *buf, int size)
{
	int			loglevel = is_server_start ? ERROR : LOG;
	char	   *command;
	FILE	   *fh;
	int			pclose_rc;
	size_t		len = 0;

	Assert(prompt);
	Assert(size > 0);
	buf[0] = '\0';

	command = replace_percent_placeholders(ssl_passphrase_command, "ssl_passphrase_command", "p", prompt);

	fh = OpenPipeStream(command, "r");
	if (fh == NULL)
	{
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not execute command \"%s\": %m",
						command)));
		goto error;
	}

	if (!fgets(buf, size, fh))
	{
		if (ferror(fh))
		{
			explicit_bzero(buf, size);
			ereport(loglevel,
					(errcode_for_file_access(),
					 errmsg("could not read from command \"%s\": %m",
							command)));
			goto error;
		}
	}

	pclose_rc = ClosePipeStream(fh);
	if (pclose_rc == -1)
	{
		explicit_bzero(buf, size);
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
		goto error;
	}
	else if (pclose_rc != 0)
	{
		explicit_bzero(buf, size);
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("command \"%s\" failed",
						command),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
		goto error;
	}

	/* 去掉末尾的换行和回车 */
	len = pg_strip_crlf(buf);

error:
	pfree(command);
	return len;
}


/*
 * 检查 SSL 密钥文件的权限。
 */
bool
check_ssl_key_file_permissions(const char *ssl_key_file, bool isServerStart)
{
	int			loglevel = isServerStart ? FATAL : LOG;
	struct stat buf;

	if (stat(ssl_key_file, &buf) != 0)
	{
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not access private key file \"%s\": %m",
						ssl_key_file)));
		return false;
	}

	/* 密钥文件必须是普通文件 */
	if (!S_ISREG(buf.st_mode))
	{
		ereport(loglevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("private key file \"%s\" is not a regular file",
						ssl_key_file)));
		return false;
	}

	/*
 * 拒绝加载不属于我们或 root 的密钥文件，且密钥文件不得
 * 有公共访问权限。若文件属于我们，需要模式为 0600 或
 * 更严。若属于 root，需要 0640 或更严，以便通过我们的 gid
 * 或允许我们读取全系统证书的附加 gid 来读取。
 *
 * 注意，src/interfaces/libpq/fe-secure-openssl.c 里有大致
 * 类似的检查，因此这里的改动也许也要同步到那里。不过环境
 * 不同；这段代码可以假定我们不是以 root 身份运行。
 *
 * 理想情况下 Windows 上也做类似的权限检查，但不清楚那该
 * 怎么做，因为 Unix 风格的权限也许不可用。
 *
 *
 *
 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	if (buf.st_uid != geteuid() && buf.st_uid != 0)
	{
		ereport(loglevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("private key file \"%s\" must be owned by the database user or root",
						ssl_key_file)));
		return false;
	}

	if ((buf.st_uid == geteuid() && buf.st_mode & (S_IRWXG | S_IRWXO)) ||
		(buf.st_uid == 0 && buf.st_mode & (S_IWGRP | S_IXGRP | S_IRWXO)))
	{
		ereport(loglevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("private key file \"%s\" has group or world access",
						ssl_key_file),
				 errdetail("File must have permissions u=rw (0600) or less if owned by the database user, or permissions u=rw,g=r (0640) or less if owned by root.")));
		return false;
	}
#endif

	return true;
}
