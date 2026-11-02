/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/prompt.c
 */
#include "postgres_fe.h"

#ifdef WIN32
#include <io.h>
#include <win32.h>
#endif

#include "common.h"
#include "common/string.h"
#include "input.h"
#include "libpq/pqcomm.h"
#include "prompt.h"
#include "settings.h"

/*--------------------------
 * get_prompt
 *
 * 通过在 pset.vars "PROMPT1|2|3" 中插值某些 tcsh 风格的转义序列
 * 构建一个静态分配的提示符。
 * （可能不完全多字节安全）
 *
 * 定义的插值如下：
 * %M - 数据库服务器"主机名.域名"，AF_UNIX 套接字为"[local]"
 *		非默认时为 "[local:/目录/名]"
 * %m - 同 %M，但仅显示主机名（第一个点之前），或始终为"[local]"
 * %p - 后端进程 PID
 * %> - 数据库服务器端口号
 * %n - 数据库用户名
 * %/ - 当前数据库
 * %~ - 同 %/，但数据库名等于用户名时显示"~"
 * %w - 与最近一次 PROMPT1 输出相同宽度的空白
 * %# - 超级用户时为"#"，否则为">"
 * %R - 在 prompt1 中通常为 =，单行模式时为 ^，
 *			或者会话未连接到数据库时为 !；
 *		在 prompt2 中为 -、*、'、" 或 $；
 *		在 prompt3 中为空
 * %x - 事务状态：空、*、!、? （未知或无连接）
 * %l - 当前语句中的行号，从 1 开始。
 * %? - 上次查询的错误代码（尚未实现）
 * %% - 百分号
 *
 * %[0-9]		   - 指定十进制编码的字符
 * %0[0-7]		   - 指定八进制编码的字符
 * %0x[0-9A-Fa-f]  - 指定十六进制编码的字符
 *
 * %`command`	   - 在 /bin/sh 中执行命令的结果，去除
 *					 尾随换行符。
 * %:name:		   - psql 变量 "name" 的值
 * （这些将不会被重新扫描更多转义序列！）
 *
 * %[ ... %]	   - 告诉 readline 包含的文本是不可见的
 *
 * 如果应用范围的提示符不知何故变为 NULL，返回的字符串将为空
 * （不是 NULL！）。
 *--------------------------
 */

char *
get_prompt(promptStatus_t status, ConditionalStack cstack)
{
#define MAX_PROMPT_SIZE 256
	static char destination[MAX_PROMPT_SIZE + 1];
	char		buf[MAX_PROMPT_SIZE + 1];
	bool		esc = false;
	const char *p;
	const char *prompt_string = "? ";
	static size_t last_prompt1_width = 0;

	switch (status)
	{
		case PROMPT_READY:
			prompt_string = pset.prompt1;
			break;

		case PROMPT_CONTINUE:
		case PROMPT_SINGLEQUOTE:
		case PROMPT_DOUBLEQUOTE:
		case PROMPT_DOLLARQUOTE:
		case PROMPT_COMMENT:
		case PROMPT_PAREN:
			prompt_string = pset.prompt2;
			break;

		case PROMPT_COPY:
			prompt_string = pset.prompt3;
			break;
	}

	destination[0] = '\0';

	for (p = prompt_string;
		 *p && strlen(destination) < sizeof(destination) - 1;
		 p++)
	{
		memset(buf, 0, sizeof(buf));
		if (esc)
		{
			switch (*p)
			{
					/* 当前数据库 */
				case '/':
					if (pset.db)
						strlcpy(buf, PQdb(pset.db), sizeof(buf));
					break;
				case '~':
					if (pset.db)
					{
						const char *var;

						if (strcmp(PQdb(pset.db), PQuser(pset.db)) == 0 ||
							((var = getenv("PGDATABASE")) && strcmp(var, PQdb(pset.db)) == 0))
							strlcpy(buf, "~", sizeof(buf));
						else
							strlcpy(buf, PQdb(pset.db), sizeof(buf));
					}
					break;

					/* 与最近一次 PROMPT1 相同宽度的空白 */
				case 'w':
					if (pset.db)
						memset(buf, ' ',
							   Min(last_prompt1_width, sizeof(buf) - 1));
					break;

					/* 数据库服务器主机名（长/短） */
				case 'M':
				case 'm':
					if (pset.db)
					{
						const char *host = PQhost(pset.db);

						/* INET 套接字 */
						if (host && host[0] && !is_unixsock_path(host))
						{
							strlcpy(buf, host, sizeof(buf));
							if (*p == 'm')
								buf[strcspn(buf, ".")] = '\0';
						}
						/* UNIX 套接字 */
						else
						{
							if (!host
								|| strcmp(host, DEFAULT_PGSOCKET_DIR) == 0
								|| *p == 'm')
								strlcpy(buf, "[local]", sizeof(buf));
							else
								snprintf(buf, sizeof(buf), "[local:%s]", host);
						}
					}
					break;
					/* 数据库服务器端口号 */
				case '>':
					if (pset.db && PQport(pset.db))
						strlcpy(buf, PQport(pset.db), sizeof(buf));
					break;
					/* 数据库用户名 */
				case 'n':
					if (pset.db)
						strlcpy(buf, session_username(), sizeof(buf));
					break;
					/* 后端进程 PID */
				case 'p':
					if (pset.db)
					{
						int			pid = PQbackendPID(pset.db);

						if (pid)
							snprintf(buf, sizeof(buf), "%d", pid);
					}
					break;

				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					*buf = (char) strtol(p, unconstify(char **, &p), 8);
					--p;
					break;
				case 'R':
					switch (status)
					{
						case PROMPT_READY:
							if (cstack != NULL && !conditional_active(cstack))
								buf[0] = '@';
							else if (!pset.db)
								buf[0] = '!';
							else if (!pset.singleline)
								buf[0] = '=';
							else
								buf[0] = '^';
							break;
						case PROMPT_CONTINUE:
							buf[0] = '-';
							break;
						case PROMPT_SINGLEQUOTE:
							buf[0] = '\'';
							break;
						case PROMPT_DOUBLEQUOTE:
							buf[0] = '"';
							break;
						case PROMPT_DOLLARQUOTE:
							buf[0] = '$';
							break;
						case PROMPT_COMMENT:
							buf[0] = '*';
							break;
						case PROMPT_PAREN:
							buf[0] = '(';
							break;
						default:
							buf[0] = '\0';
							break;
					}
					break;

				case 'x':
					if (!pset.db)
						buf[0] = '?';
					else
						switch (PQtransactionStatus(pset.db))
						{
							case PQTRANS_IDLE:
								buf[0] = '\0';
								break;
							case PQTRANS_ACTIVE:
							case PQTRANS_INTRANS:
								buf[0] = '*';
								break;
							case PQTRANS_INERROR:
								buf[0] = '!';
								break;
							default:
								buf[0] = '?';
								break;
						}
					break;

				case 'l':
					snprintf(buf, sizeof(buf), UINT64_FORMAT, pset.stmt_lineno);
					break;

				case '?':
					/* 尚未实现 */
					break;

				case '#':
					if (is_superuser())
						buf[0] = '#';
					else
						buf[0] = '>';
					break;

					/* 执行命令 */
				case '`':
					{
						int			cmdend = strcspn(p + 1, "`");
						char	   *file = pnstrdup(p + 1, cmdend);
						FILE	   *fd;

						fflush(NULL);
						fd = popen(file, "r");
						if (fd)
						{
							if (fgets(buf, sizeof(buf), fd) == NULL)
								buf[0] = '\0';
							pclose(fd);
						}

						/* 去除尾随换行符和回车符 */
						(void) pg_strip_crlf(buf);

						free(file);
						p += cmdend + 1;
						break;
					}

					/* 插值变量 */
				case ':':
					{
						int			nameend = strcspn(p + 1, ":");
						char	   *name = pnstrdup(p + 1, nameend);
						const char *val;

						val = GetVariable(pset.vars, name);
						if (val)
							strlcpy(buf, val, sizeof(buf));
						free(name);
						p += nameend + 1;
						break;
					}

				case '[':
				case ']':
#if defined(USE_READLINE) && defined(RL_PROMPT_START_IGNORE)

					/*
					 * readline >=4.0 未文档化功能：提示符字符串中的
					 * 非打印字符必须标记，以便在编辑时正确显示行。
					 *
					 */
					buf[0] = (*p == '[') ? RL_PROMPT_START_IGNORE : RL_PROMPT_END_IGNORE;
					buf[1] = '\0';
#endif							/* USE_READLINE */
					break;

				default:
					buf[0] = *p;
					buf[1] = '\0';
					break;
			}
			esc = false;
		}
		else if (*p == '%')
			esc = true;
		else
		{
			buf[0] = *p;
			buf[1] = '\0';
			esc = false;
		}

		if (!esc)
			strlcat(destination, buf, sizeof(destination));
	}

	/* 计算 PROMPT1 的可见宽度，用于 PROMPT2 的 %w */
	if (prompt_string == pset.prompt1)
	{
		char	   *p = destination;
		char	   *end = p + strlen(p);
		bool		visible = true;

		last_prompt1_width = 0;
		while (*p)
		{
#if defined(USE_READLINE) && defined(RL_PROMPT_START_IGNORE)
			if (*p == RL_PROMPT_START_IGNORE)
			{
				visible = false;
				++p;
			}
			else if (*p == RL_PROMPT_END_IGNORE)
			{
				visible = true;
				++p;
			}
			else
#endif
			{
				int			chlen,
							chwidth;

				chlen = PQmblen(p, pset.encoding);
				if (p + chlen > end)
					break;		/* 无效字符串 */

				if (visible)
				{
					chwidth = PQdsplen(p, pset.encoding);

					if (*p == '\n')
						last_prompt1_width = 0;
					else if (chwidth > 0)
						last_prompt1_width += chwidth;
				}

				p += chlen;
			}
		}
	}

	return destination;
}
