/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/copy.c
 */
#include "postgres_fe.h"

#include <signal.h>
#include <sys/stat.h>
#ifndef WIN32
#include <unistd.h>				/* 用于 isatty */
#else
#include <io.h>					/* 我认为 */
#endif

#include "common.h"
#include "common/logging.h"
#include "copy.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "prompt.h"
#include "settings.h"
#include "stringutils.h"

/*
 * parse_slash_copy
 * -- 解析 \copy 命令行
 *
 * 文档记录的语法为：
 *  \copy 表名 [(列列表)] from|to 文件名 [选项]
 *  \copy ( 查询语句 ) to 文件名 [选项]
 *
 * 其中 'filename' 可为以下之一：
 *  '<文件路径>' | PROGRAM '<命令>' | stdin | stdout | pstdout | pstdout
 * 而 'query' 可为以下之一：
 *  SELECT | UPDATE | INSERT | DELETE
 *
 * 一个未记录的事实是，你仍然可以在表名前写 BINARY；
 * 这是 7.3 之前语法的遗留。选项语法在不同后端版本间有所不同，
 * 但我们通过直接原样传递文件名之后的内容来避免这些混乱。
 *
 * 表名可以用双引号括起来，并且可以包含模式部分。
 * 列名可以用双引号括起来。
 * 文件名可以像 SQL 字面量一样用单引号括起来。
 * 命令必须像 SQL 字面量一样用单引号括起来。
 *
 * 返回一个 malloc 分配的结构体，包含选项；解析错误时返回 NULL
 *
 */

struct copy_options
{
	char	   *before_tofrom;	/* COPY string before TO/FROM */
	char	   *after_tofrom;	/* COPY string after TO/FROM filename */
	char	   *file;			/* NULL = stdin/stdout */
	bool		program;		/* is 'file' a program to popen? */
	bool		psql_inout;		/* true = use psql stdin/stdout */
	bool		from;			/* true = FROM, false = TO */
};


static void
free_copy_options(struct copy_options *ptr)
{
	if (!ptr)
		return;
	free(ptr->before_tofrom);
	free(ptr->after_tofrom);
	free(ptr->file);
	free(ptr);
}


/* 将 "more" 拼接到 "var" 上，释放 *var 的原始值 */
static void
xstrcat(char **var, const char *more)
{
	char	   *newvar;

	newvar = psprintf("%s%s", *var, more);
	free(*var);
	*var = newvar;
}


static struct copy_options *
parse_slash_copy(const char *args)
{
	struct copy_options *result;
	char	   *token;
	const char *whitespace = " \t\n\r";
	char		nonstd_backslash = standard_strings() ? 0 : '\\';

	if (!args)
	{
		pg_log_error("\\copy: arguments required");
		return NULL;
	}

	result = pg_malloc0(sizeof(struct copy_options));

	result->before_tofrom = pg_strdup("");	/* 初始化用于追加 */

	token = strtokx(args, whitespace, ".,()", "\"",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	/* 放弃 7.3 语法支持时可移除此处 */
	if (pg_strcasecmp(token, "binary") == 0)
	{
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	/* 处理 COPY (查询) 的情况 */
	if (token[0] == '(')
	{
		int			parens = 1;

		while (parens > 0)
		{
			xstrcat(&result->before_tofrom, " ");
			xstrcat(&result->before_tofrom, token);
			token = strtokx(NULL, whitespace, "()", "\"'",
							nonstd_backslash, true, false, pset.encoding);
			if (!token)
				goto error;
			if (token[0] == '(')
				parens++;
			else if (token[0] == ')')
				parens--;
		}
	}

	xstrcat(&result->before_tofrom, " ");
	xstrcat(&result->before_tofrom, token);
	token = strtokx(NULL, whitespace, ".,()", "\"",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	/*
	 * strtokx() 不会返回以 '.' 开头的多字符 token，
	 * 因此这里不需要 strcmp()。下面的 '(' 等也同样。
	 */
	if (token[0] == '.')
	{
		/* 处理 schema . table */
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (token[0] == '(')
	{
		/* 处理带括号的列列表 */
		for (;;)
		{
			xstrcat(&result->before_tofrom, " ");
			xstrcat(&result->before_tofrom, token);
			token = strtokx(NULL, whitespace, "()", "\"",
							0, false, false, pset.encoding);
			if (!token)
				goto error;
			if (token[0] == ')')
				break;
		}
		xstrcat(&result->before_tofrom, " ");
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (pg_strcasecmp(token, "from") == 0)
		result->from = true;
	else if (pg_strcasecmp(token, "to") == 0)
		result->from = false;
	else
		goto error;

	/* { 'filename' | PROGRAM 'command' | STDIN | STDOUT | PSTDIN | PSTDOUT } */
	token = strtokx(NULL, whitespace, ";", "'",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	if (pg_strcasecmp(token, "program") == 0)
	{
		int			toklen;

		token = strtokx(NULL, whitespace, ";", "'",
						0, false, false, pset.encoding);
		if (!token)
			goto error;

		/*
			 * shell 命令必须用引号括起来。这不是万无一失的，但
			 * 能捕获大多数引号错误。
		 */
		toklen = strlen(token);
		if (token[0] != '\'' || toklen < 2 || token[toklen - 1] != '\'')
			goto error;

		strip_quotes(token, '\'', 0, pset.encoding);

		result->program = true;
		result->file = pg_strdup(token);
	}
	else if (pg_strcasecmp(token, "stdin") == 0 ||
			 pg_strcasecmp(token, "stdout") == 0)
	{
		result->file = NULL;
	}
	else if (pg_strcasecmp(token, "pstdin") == 0 ||
			 pg_strcasecmp(token, "pstdout") == 0)
	{
		result->psql_inout = true;
		result->file = NULL;
	}
	else
	{
		/* 文件名可以选择用引号括起来 */
		strip_quotes(token, '\'', 0, pset.encoding);
		result->file = pg_strdup(token);
		expand_tilde(&result->file);
	}

	/* 收集行的剩余部分（COPY 选项） */
	token = strtokx(NULL, "", NULL, NULL,
					0, false, false, pset.encoding);
	if (token)
		result->after_tofrom = pg_strdup(token);

	return result;

error:
	if (token)
		pg_log_error("\\copy: parse error at \"%s\"", token);
	else
		pg_log_error("\\copy: parse error at end of line");
	free_copy_options(result);

	return NULL;
}


/*
 * 执行 \copy 命令（前端复制）。我们必须打开一个文件（或执行
 * 一个命令），然后向后台提交一个 COPY 查询，并将数据从文件
 * 传入或将其响应路由到文件。
 */
bool
do_copy(const char *args)
{
	PQExpBufferData query;
	FILE	   *copystream;
	struct copy_options *options;
	bool		success;

	/* 解析选项 */
	options = parse_slash_copy(args);

	if (!options)
		return false;

	/* 准备读取或写入目标文件 */
	if (options->file && !options->program)
		canonicalize_path_enc(options->file, pset.encoding);

	if (options->from)
	{
		if (options->file)
		{
			if (options->program)
			{
				fflush(NULL);
				errno = 0;
				copystream = popen(options->file, PG_BINARY_R);
			}
			else
				copystream = fopen(options->file, PG_BINARY_R);
		}
		else if (!options->psql_inout)
			copystream = pset.cur_cmd_source;
		else
			copystream = stdin;
	}
	else
	{
		if (options->file)
		{
			if (options->program)
			{
				fflush(NULL);
				disable_sigpipe_trap();
				errno = 0;
				copystream = popen(options->file, PG_BINARY_W);
			}
			else
				copystream = fopen(options->file, PG_BINARY_W);
		}
		else if (!options->psql_inout)
			copystream = pset.queryFout;
		else
			copystream = stdout;
	}

	if (!copystream)
	{
		if (options->program)
			pg_log_error("could not execute command \"%s\": %m",
						 options->file);
		else
			pg_log_error("%s: %m",
						 options->file);
		free_copy_options(options);
		return false;
	}

	if (!options->program)
	{
		struct stat st;
		int			result;

		/* 确保指定的文件不是目录 */
		if ((result = fstat(fileno(copystream), &st)) < 0)
			pg_log_error("could not stat file \"%s\": %m",
						 options->file);

		if (result == 0 && S_ISDIR(st.st_mode))
			pg_log_error("%s: cannot copy from/to a directory",
						 options->file);

		if (result < 0 || S_ISDIR(st.st_mode))
		{
			fclose(copystream);
			free_copy_options(options);
			return false;
		}
	}

	/* 构建将发送到后台的命令 */
	initPQExpBuffer(&query);
	printfPQExpBuffer(&query, "COPY ");
	appendPQExpBufferStr(&query, options->before_tofrom);
	if (options->from)
		appendPQExpBufferStr(&query, " FROM STDIN ");
	else
		appendPQExpBufferStr(&query, " TO STDOUT ");
	if (options->after_tofrom)
		appendPQExpBufferStr(&query, options->after_tofrom);

	/* 像用户命令一样运行，但以 copystream 作为数据源/接收端 */
	pset.copyStream = copystream;
	success = SendQuery(query.data);
	pset.copyStream = NULL;
	termPQExpBuffer(&query);

	if (options->file != NULL)
	{
		if (options->program)
		{
			int			pclose_rc = pclose(copystream);

			if (pclose_rc != 0)
			{
				if (pclose_rc < 0)
					pg_log_error("could not close pipe to external command: %m");
				else
				{
					char	   *reason = wait_result_to_str(pclose_rc);

					pg_log_error("%s: %s", options->file,
								 reason ? reason : "");
					free(reason);
				}
				success = false;
			}
			SetShellResultVariables(pclose_rc);
			restore_sigpipe_trap();
		}
		else
		{
			if (fclose(copystream) != 0)
			{
				pg_log_error("%s: %m", options->file);
				success = false;
			}
		}
	}
	free_copy_options(options);
	return success;
}


/*
 * 处理 COPY IN/OUT 数据传输的函数。
 *
 * 如果你想在应用程序中使用 COPY TO STDOUT/FROM STDIN，
 * 这就是值得借鉴的代码 ;)
 */

/*
 * handleCopyOut
 * 接收 COPY ... TO STDOUT 命令的结果数据
 *
 * conn 应该是你刚刚对其发出 COPY TO 并收到
 * PGRES_COPY_OUT 结果的数据库连接。
 *
 * copystream 是数据要写入的文件流。
 * copystream 可以为 NULL，以丢弃数据而不写入任何地方。
 *
 * COPY 的最终状态返回到 *res 中（但请注意，
 * 如果不是成功结果，我们已经报告了错误）。
 *
 * 成功返回 true，失败返回 false。
 */
bool
handleCopyOut(PGconn *conn, FILE *copystream, PGresult **res)
{
	bool		OK = true;
	char	   *buf;
	int			ret;

	for (;;)
	{
		ret = PQgetCopyData(conn, &buf, 0);

		if (ret < 0)
			break;				/* 完成或服务器/连接错误 */

		if (buf)
		{
			if (OK && copystream && fwrite(buf, 1, ret, copystream) != ret)
			{
				pg_log_error("could not write COPY data: %m");
				/* 只抱怨一次，继续从服务器读取数据 */
				OK = false;
			}
			PQfreemem(buf);
		}
	}

	if (OK && copystream && fflush(copystream))
	{
		pg_log_error("could not write COPY data: %m");
		OK = false;
	}

	if (ret == -2)
	{
		pg_log_error("COPY data transfer failed: %s", PQerrorMessage(conn));
		OK = false;
	}

	/*
	 * 检查命令状态并返回正常的 libpq 状态。
	 *
	 * 如果由于某种原因 libpq 仍报告 PGRES_COPY_OUT 状态，我们
	 * 希望强制退出该状态，因为调用者无法区分这种情形与在包含
	 * 两个连续 COPY TO STDOUT 命令的命令字符串中到达下一个 COPY。
	 * 然而，libpq 没有提供这样做的 API，而且原则上，如果
	 * PQgetCopyData() 返回 -1 或 -2 但尚未在内部退出 COPY_OUT
	 * 状态，这本身就是 libpq 的 bug。所以我们在此忽略这种可能性。
	 *
	 *
	 */
	*res = PQgetResult(conn);
	if (PQresultStatus(*res) != PGRES_COMMAND_OK)
	{
		pg_log_info("%s", PQerrorMessage(conn));
		OK = false;
	}

	return OK;
}

/*
 * handleCopyIn
 * 发送数据以完成 COPY ... FROM STDIN 命令
 *
 * conn 应该是你刚刚对其发出 COPY FROM 并收到
 * PGRES_COPY_IN 结果的数据库连接。
 * copystream 是要从中读取数据的文件流。
 * isbinary 可以从 PQbinaryTuples() 设置。
 * COPY 的最终状态返回到 *res 中（但请注意，
 * 如果不是成功结果，我们已经报告了错误）。
 *
 * 成功返回 true，失败返回 false。
 */

/* COPY IN 的读取块大小 - 大小并不关键 */
#define COPYBUFSIZ 8192

bool
handleCopyIn(PGconn *conn, FILE *copystream, bool isbinary, PGresult **res)
{
	bool		OK;
	char		buf[COPYBUFSIZ];
	bool		showprompt;

	/*
	 * 建立用于退出等待输入状态的 longjmp 目标。
	 * （仅在 sigint_interrupt_enabled 为 TRUE 时生效。）
	 */
	if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
	{
		/* 通过 longjmp 到达此处 */

		/* 终止数据传输 */
		PQputCopyEnd(conn,
					 (PQprotocolVersion(conn) < 3) ? NULL :
					 _("canceled by user"));

		OK = false;
		goto copyin_cleanup;
	}

	/* 交互式输入时发出提示 */
	if (isatty(fileno(copystream)))
	{
		showprompt = true;
		if (!pset.quiet)
			puts(_("Enter data to be copied followed by a newline.\n"
				   "End with a backslash and a period on a line by itself, or an EOF signal."));
	}
	else
		showprompt = false;

	OK = true;

	if (isbinary)
	{
		/* 交互式输入可能不合适，但无论如何给出一次提示 */
		if (showprompt)
		{
			const char *prompt = get_prompt(PROMPT_COPY, NULL);

			fputs(prompt, stdout);
			fflush(stdout);
		}

		for (;;)
		{
			int			buflen;

			/* 等待输入时启用 longjmp */
			sigint_interrupt_enabled = true;

			buflen = fread(buf, 1, COPYBUFSIZ, copystream);

			sigint_interrupt_enabled = false;

			if (buflen <= 0)
				break;

			if (PQputCopyData(conn, buf, buflen) <= 0)
			{
				OK = false;
				break;
			}
		}
	}
	else
	{
		bool		copydone = false;
		int			buflen;
		bool		at_line_begin = true;

		/*
		 * 在文本模式下，我们必须一次读取一行输入，这样才能
		 * 在 EOF 标记 (\.) 处停止读取。我们不能读取超过
		 * EOF 标记的内容，因为如果数据内联在 SQL 脚本中，
		 * 我们会吞掉 EOF 标记之后的命令。
		 */
		buflen = 0;
		while (!copydone)
		{
			char	   *fgresult;

			if (at_line_begin && showprompt)
			{
				const char *prompt = get_prompt(PROMPT_COPY, NULL);

				fputs(prompt, stdout);
				fflush(stdout);
			}

			/* 等待输入时启用 longjmp */
			sigint_interrupt_enabled = true;

			fgresult = fgets(&buf[buflen], COPYBUFSIZ - buflen, copystream);

			sigint_interrupt_enabled = false;

			if (!fgresult)
				copydone = true;
			else
			{
				int			linelen;

				linelen = strlen(fgresult);
				buflen += linelen;

				/* 当前行已完成？ */
				if (buf[buflen - 1] == '\n')
				{
					/* 检查 EOF 标记，但不在不完整的行上 */
					if (at_line_begin)
					{
						/*
							 * 此代码错误地假定在带引号的 CSV 字符串中
							 * 单独一行上的 '\.' 会终止 \copy。
							 * https://www.postgresql.org/message-id/E1TdNVQ-0001ju-GO@wrigleys.postgresql.org
							 * https://www.postgresql.org/message-id/bfcd57e4-8f23-4c3e-a5db-2571d09208e2@beta.fastmail.com
						 */
						if ((linelen == 3 && memcmp(fgresult, "\\.\n", 3) == 0) ||
							(linelen == 4 && memcmp(fgresult, "\\.\r\n", 4) == 0))
						{
							copydone = true;
						}
					}

					if (copystream == pset.cur_cmd_source)
					{
						pset.lineno++;
						pset.stmt_lineno++;
					}
					at_line_begin = true;
				}
				else
					at_line_begin = false;
			}

			/*
			 * 如果缓冲区已满，或者我们已到达 EOF，则刷新它。
			 *
			 * 确保缓冲区中始终有四个额外字节的空间，加上 NUL
			 * 终止符。这样，EOF 标记就不会跨两次 fgets()
			 * 调用而拆分，从而简化了逻辑。
			 *
			 */
			if (buflen >= COPYBUFSIZ - 5 || (copydone && buflen > 0))
			{
				if (PQputCopyData(conn, buf, buflen) <= 0)
				{
					OK = false;
					break;
				}

				buflen = 0;
			}
		}
	}

	/* 检查读取错误 */
	if (ferror(copystream))
		OK = false;

	/*
	 * 终止数据传输。如果使用协议版本 2，我们无法发送错误消息。
	 * （libpq 不再支持协议版本 2，但保留版本检查以防在
	 * 运行时使用 pre-v14 libpq.so）
	 *
	 */
	if (PQputCopyEnd(conn,
					 (OK || PQprotocolVersion(conn) < 3) ? NULL :
					 _("aborted because of read failure")) <= 0)
		OK = false;

copyin_cleanup:

	/*
	 * 清除流上的 EOF 标志，以防复制由于 EOF 信号而结束。
	 * 这允许交互式 TTY 会话稍后执行另一个 COPY FROM
	 * STDIN。（在非 STDIN 情况下，我们即将关闭文件，
	 * 所以无关紧要。）虽然我们不会用 feof() 测试该标志，
	 * 但某些 fread() 实现在设置该标志后不会再读取更多数据。
	 * 这也会清除错误标志，但我们已经检查过了。
	 */
	clearerr(copystream);

	/*
	 * 检查命令状态并返回正常的 libpq 状态。
	 *
	 * 我们不希望在状态仍为 PGRES_COPY_IN 时返回：
	 * 调用者将无法区分这种情形与在包含两个连续
	 * COPY FROM STDIN 命令的命令字符串中到达下一个 COPY。
	 * 我们持续尝试 PQputCopyEnd()，希望它最终会生效。
	 * （实际上可能发生的是，在尝试刷新数据时，libpq
	 * 最终会意识到连接已丢失。但这没关系；它会让我们
	 * 退出 COPY_IN 状态，这正是我们需要的。）
	 *
	 */
	while (*res = PQgetResult(conn), PQresultStatus(*res) == PGRES_COPY_IN)
	{
		OK = false;
		PQclear(*res);
		/* 使用协议版本 2 时无法发送错误消息 */
		PQputCopyEnd(conn,
					 (PQprotocolVersion(conn) < 3) ? NULL :
					 _("trying to exit copy mode"));
	}
	if (PQresultStatus(*res) != PGRES_COMMAND_OK)
	{
		pg_log_info("%s", PQerrorMessage(conn));
		OK = false;
	}

	return OK;
}
