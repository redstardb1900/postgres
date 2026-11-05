/*-------------------------------------------------------------------------
 *
 * be-secure-common.c
 *
 * 与具体实现无关的公共 SSL 支持代码
 *
 * be-secure.c 含有其余通信代码所调用的接口，
 * 本文件则含有支持例程，供
 * be-secure-openssl.c 这类特定于库的实现使用。
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
 * Run ssl_passphrase_command
 *
 * %p 会被替换为 prompt。is_server_start 决定错误消息             
 * 的日志级别。
 *
 * 结果将放入大小为 size 的缓冲区 buf 中。返回值是       
 * 实际结果的长度。                 
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
	 * 拒绝加载非我们或 root 所有的密钥文件，要求      
	 * 密钥文件不允许公共访问。若文件属于我们，要求    
	 * 权限不高于 0600。若属于 root，则要求不高于 0640，   
	 * 以便通过我们的 gid 或可读取全系统证书的附加      
	 * gid 来读取。                           
	 *
	 * 注意，大致类似的检查也在             
	 * src/interfaces/libpq/fe-secure-openssl.c 中进行，                
	 * 因此此处的改动可能也需要在那里做。不过环境     
	 * 不同；此代码可假定我们不是 root。
	 *
	 * 理想情况下 Windows 上也会做类似的权限检查，但     
	 * 尚不清楚如何实现，因为 Unix 风格的权限也许无法   
	 * 使用。 
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
