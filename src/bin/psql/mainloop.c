/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/mainloop.c
 */
#include "postgres_fe.h"

#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "input.h"
#include "mainloop.h"
#include "mb/pg_wchar.h"
#include "prompt.h"
#include "settings.h"

/* 用于 flex 词法分析器的回调函数 */
const PsqlScanCallbacks psqlscan_callbacks = {
	psql_get_variable,
};


/*
 * 主处理循环，用于读取输入行
 * 并将其发送到后台。
 *
 * 此循环是可重入的。可由读取文件输入的
 * \i 命令调用。
 */
int
MainLoop(FILE *source)
{
	PsqlScanState scan_state;	/* 词法分析器工作状态 */
	ConditionalStack cond_stack;	/* \if 状态栈 */
	volatile PQExpBuffer query_buf; /* 正在累积的查询缓冲区 */
	volatile PQExpBuffer previous_buf;	/* 如果新缓冲区中没有任何内容，
										 * 则将此用于 \e 等
										 * */
	PQExpBuffer history_buf;	/* 多行命令的先前行，尚未保存到
								 * readline 历史记录中 */
	char	   *line;			/* 当前输入行 */
	int			added_nl_pos;
	bool		success;
	bool		line_saved_in_history;
	volatile int successResult = EXIT_SUCCESS;
	volatile backslashResult slashCmdStatus = PSQL_CMD_UNKNOWN;
	volatile promptStatus_t prompt_status = PROMPT_READY;
	volatile bool need_redisplay = false;
	volatile int count_eof = 0;
	volatile bool die_on_error = false;
	FILE	   *prev_cmd_source;
	bool		prev_cmd_interactive;
	uint64		prev_lineno;

	/* 保存先前的命令源 */
	prev_cmd_source = pset.cur_cmd_source;
	prev_cmd_interactive = pset.cur_cmd_interactive;
	prev_lineno = pset.lineno;
	/* pset.stmt_lineno 不需要保存和恢复 */

	/* 建立新源 */
	pset.cur_cmd_source = source;
	pset.cur_cmd_interactive = ((source == stdin) && !pset.notty);
	pset.lineno = 0;
	pset.stmt_lineno = 1;

	/* 创建工作状态 */
	scan_state = psql_scan_create(&psqlscan_callbacks);
	cond_stack = conditional_stack_create();
	psql_scan_set_passthrough(scan_state, (void *) cond_stack);

	query_buf = createPQExpBuffer();
	previous_buf = createPQExpBuffer();
	history_buf = createPQExpBuffer();
	if (PQExpBufferBroken(query_buf) ||
		PQExpBufferBroken(previous_buf) ||
		PQExpBufferBroken(history_buf))
		pg_fatal("out of memory");

	/* 获取查询并执行的主循环 */
	while (successResult == EXIT_SUCCESS)
	{
		/*
		 * 清理前一次 Control-C 之后的状态
		 */
		if (cancel_pressed)
		{
			if (!pset.cur_cmd_interactive)
			{
				/*
				 * 如果用 Ctrl-C 停止了脚本，会到达此处。
				 */
				successResult = EXIT_USER;
				break;
			}

			cancel_pressed = false;
		}

		/*
		 * 建立用于退出等待输入状态的 longjmp 目标。为安全起见，
		 * 我们必须在每次循环中重新设置，因为 jmpbuf 可能在
		 * 命令执行期间发生变化。
		 */
		if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
		{
			/* 通过 longjmp 到达此处 */

			/* 重置解析状态 */
			psql_scan_finish(scan_state);
			psql_scan_reset(scan_state);
			resetPQExpBuffer(query_buf);
			resetPQExpBuffer(history_buf);
			count_eof = 0;
			slashCmdStatus = PSQL_CMD_UNKNOWN;
			prompt_status = PROMPT_READY;
			need_redisplay = false;
			pset.stmt_lineno = 1;
			cancel_pressed = false;

			if (pset.cur_cmd_interactive)
			{
				putc('\n', stdout);

				/*
					 * 如果交互用户正在 \if 块中，Ctrl-C 将退出
					 * 最内层的 \if。
				 */
				if (!conditional_stack_empty(cond_stack))
				{
					pg_log_error("\\if: escaped");
					conditional_stack_pop(cond_stack);
				}
			}
			else
			{
				successResult = EXIT_USER;
				break;
			}
		}

		fflush(stdout);

		/*
			 * 获取另一行
		 */
		if (pset.cur_cmd_interactive)
		{
			/* 可能需要重置提示符，例如在 \r 命令之后 */
			if (query_buf->len == 0)
				prompt_status = PROMPT_READY;
			/* 如果查询缓冲区来自 \e，则用提示符重新显示 */
			if (need_redisplay)
			{
				if (query_buf->len > 0)
				{
					fputs(get_prompt(PROMPT_READY, cond_stack), stdout);
					fputs(query_buf->data, stdout);
					fflush(stdout);
				}
				need_redisplay = false;
			}
			/* 现在可以获取一行 */
			line = gets_interactive(get_prompt(prompt_status, cond_stack),
									query_buf);
		}
		else
		{
			line = gets_fromFile(source);
			if (!line && ferror(source))
				successResult = EXIT_FAILURE;
		}

		/*
			 * query_buf 保存已累积的查询。line 是 malloc 分配的
			 * 新输入行（注意在循环之前必须释放！）
		 */

		/* 没有更多输入。退出，或 \i 完成 */
		if (line == NULL)
		{
			if (pset.cur_cmd_interactive)
			{
				/* 这尝试模仿 bash 的 IGNOREEOF 功能 */
				count_eof++;

				if (count_eof < pset.ignoreeof)
				{
					if (!pset.quiet)
						printf(_("Use \"\\q\" to leave %s.\n"), pset.progname);
					continue;
				}

				puts(pset.quiet ? "" : "\\q");
			}
			break;
		}

		count_eof = 0;

		pset.lineno++;

		/* 忽略 UTF-8 Unicode 字节序标记 */
		if (pset.lineno == 1 && pset.encoding == PG_UTF8 && strncmp(line, "\xef\xbb\xbf", 3) == 0)
			memmove(line, line + 3, strlen(line + 3) + 1);

		/* 检测将自定义格式转储作为 SQL 脚本运行的尝试 */
		if (pset.lineno == 1 && !pset.cur_cmd_interactive &&
			strncmp(line, "PGDMP", 5) == 0)
		{
			free(line);
			puts(_("The input is a PostgreSQL custom-format dump.\n"
				   "Use the pg_restore command-line client to restore this dump to a database.\n"));
			fflush(stdout);
			successResult = EXIT_FAILURE;
			break;
		}

		/* 不再进一步处理空行，除非在字面量中 */
		if (line[0] == '\0' && !psql_scan_in_quote(scan_state))
		{
			free(line);
			continue;
		}

		/* 仅在交互模式下识别 help、quit、exit */
		if (pset.cur_cmd_interactive)
		{
			char	   *first_word = line;
			char	   *rest_of_line = NULL;
			bool		found_help = false;
			bool		found_exit_or_quit = false;
			bool		found_q = false;

			/*
				 * 辅助词 help/exit/quit 必须在前面没有空白字符，
				 * 且后面只有空白字符，可选带一个分号。
				 * 这可以防止以缩进方式使用这些词（例如作为标识符）
				 * 时触发辅助行为。
			 */
			if (pg_strncasecmp(first_word, "help", 4) == 0)
			{
				rest_of_line = first_word + 4;
				found_help = true;
			}
			else if (pg_strncasecmp(first_word, "exit", 4) == 0 ||
					 pg_strncasecmp(first_word, "quit", 4) == 0)
			{
				rest_of_line = first_word + 4;
				found_exit_or_quit = true;
			}
			else if (strncmp(first_word, "\\q", 2) == 0)
			{
				rest_of_line = first_word + 2;
				found_q = true;
			}

			/*
				 * 如果找到了命令词，检查行的其余部分是否仅包含
				 * 空白字符外加一个可能的分号。如果不是，则完全
				 * 忽略该命令词。这些命令仅用于与其他 SQL 客户端
				 * 兼容，并没有文档记录。
			 */
			if (rest_of_line != NULL)
			{
				/*
					 * 忽略，除非行的其余部分是空白字符，外加一个可能
					 * 的分号
				 */
				while (isspace((unsigned char) *rest_of_line))
					++rest_of_line;
				if (*rest_of_line == ';')
					++rest_of_line;
				while (isspace((unsigned char) *rest_of_line))
					++rest_of_line;
				if (*rest_of_line != '\0')
				{
					found_help = false;
					found_exit_or_quit = false;
				}
			}

			/*
				 * "help" 仅在查询缓冲区为空时才被视为命令，但即使
				 * 不为空我们也发出一条单行消息以帮助困惑的用户。
				 * 在这种情况下的文本仍然会被添加到查询缓冲区中。
				 *
			 */
			if (found_help)
			{
				if (query_buf->len != 0)
#ifndef WIN32
					puts(_("Use \\? for help or press control-C to clear the input buffer."));
#else
					puts(_("Use \\? for help."));
#endif
				else
				{
					puts(_("You are using psql, the command-line interface to PostgreSQL."));
					printf(_("Type:  \\copyright for distribution terms\n"
							 "       \\h for help with SQL commands\n"
							 "       \\? for help with psql commands\n"
							 "       \\g or terminate with semicolon to execute query\n"
							 "       \\q to quit\n"));
					free(line);
					fflush(stdout);
					continue;
				}
			}

			/*
				 * "quit" 和 "exit" 仅在查询缓冲区为空时才被视为
				 * 命令，但即使不为空我们也发出一条单行消息以帮助
				 * 困惑的用户。在这种情况下的文本仍然会被添加到
				 * 查询缓冲区中。
			 */
			if (found_exit_or_quit)
			{
				if (query_buf->len != 0)
				{
					if (prompt_status == PROMPT_READY ||
						prompt_status == PROMPT_CONTINUE ||
						prompt_status == PROMPT_PAREN)
						puts(_("Use \\q to quit."));
					else
#ifndef WIN32
						puts(_("Use control-D to quit."));
#else
						puts(_("Use control-C to quit."));
#endif
				}
				else
				{
					/* 退出应用 */
					free(line);
					fflush(stdout);
					successResult = EXIT_SUCCESS;
					break;
				}
			}

			/*
				 * 如果在 "\q" 不活跃的地方输入了 "\q"，
				 * 则提供提示。该文本仍然会被添加到查询缓冲区中。
			 */
			if (found_q && query_buf->len != 0 &&
				prompt_status != PROMPT_READY &&
				prompt_status != PROMPT_CONTINUE &&
				prompt_status != PROMPT_PAREN)
#ifndef WIN32
				puts(_("Use control-D to quit."));
#else
				puts(_("Use control-C to quit."));
#endif
		}

		/* 如果设置了标志则回显，除非在交互模式下 */
		if (pset.echo == PSQL_ECHO_ALL && !pset.cur_cmd_interactive)
		{
			puts(line);
			fflush(stdout);
		}

		/* 在源行之间将换行符插入查询缓冲区 */
		if (query_buf->len > 0)
		{
			appendPQExpBufferChar(query_buf, '\n');
			added_nl_pos = query_buf->len;
		}
		else
			added_nl_pos = -1;	/* 标记我们没有添加 */

		/* 设置此项要到下一行才会生效 */
		die_on_error = pset.on_error_stop;

		/*
			 * 解析行，查找命令分隔符。
		 */
		psql_scan_setup(scan_state, line, strlen(line),
						pset.encoding, standard_strings());
		success = true;
		line_saved_in_history = false;

		while (success || !die_on_error)
		{
			PsqlScanResult scan_result;
			promptStatus_t prompt_tmp = prompt_status;
			size_t		pos_in_query;
			char	   *tmp_line;

			pos_in_query = query_buf->len;
			scan_result = psql_scan(scan_state, query_buf, &prompt_tmp);
			prompt_status = prompt_tmp;

			if (PQExpBufferBroken(query_buf))
				pg_fatal("out of memory");

			/*
				 * 对最后一次 psql_scan() 调用添加到查询缓冲区的
				 * 每个换行符增加语句行号计数器。只有在导航到
				 * readline 历史中包含换行符的语句时，才会添加
				 * 这些换行符。
			 */
			tmp_line = query_buf->data + pos_in_query;
			while (*tmp_line != '\0')
			{
				if (*(tmp_line++) == '\n')
					pset.stmt_lineno++;
			}

			if (scan_result == PSCAN_EOL)
				pset.stmt_lineno++;

			/*
				 * 如果找到分号则发送命令，或者如果到达行尾且我们处于
				 * 单行模式，也发送命令。
			 */
			if (scan_result == PSCAN_SEMICOLON ||
				(scan_result == PSCAN_EOL && pset.singleline))
			{
				/*
					 * 将行保存到历史记录中。我们使用 history_buf 将
					 * 多行查询累积为单个历史条目。请注意，历史记录
					 * 累积基于输入行，因此查询是否因 \if 而被忽略
					 * 并不重要。
				 */
				if (pset.cur_cmd_interactive && !line_saved_in_history)
				{
					pg_append_history(line, history_buf);
					pg_send_history(history_buf);
					line_saved_in_history = true;
				}

				/* 执行查询，除非我们在不活跃的 \if 分支中 */
				if (conditional_active(cond_stack))
				{
					success = SendQuery(query_buf->data);
					slashCmdStatus = success ? PSQL_CMD_SEND : PSQL_CMD_ERROR;
					pset.stmt_lineno = 1;

					/* 通过指针交换将查询转移到 previous_buf */
					{
						PQExpBuffer swap_buf = previous_buf;

						previous_buf = query_buf;
						query_buf = swap_buf;
					}
					resetPQExpBuffer(query_buf);

					added_nl_pos = -1;
					/* 此处不需要执行 psql_scan_reset() */
				}
				else
				{
					/* 如果是交互模式，警告查询未执行 */
					if (pset.cur_cmd_interactive)
						pg_log_error("query ignored; use \\endif or Ctrl-C to exit current \\if block");
					/* 为循环检查目的伪造一个 OK 结果 */
					success = true;
					slashCmdStatus = PSQL_CMD_SEND;
					pset.stmt_lineno = 1;
					/* 注意 query_buf 不会改变状态 */
				}
			}
			else if (scan_result == PSCAN_BACKSLASH)
			{
				/* 处理反斜杠命令 */

				/*
					 * 如果我们向 query_buf 添加了一个换行符，且词法
					 * 分析器没有向 query_buf 中插入任何其他内容，
					 * 则再次去除该换行符。这样可以避免在行仅包含
					 * 反斜杠命令时对 query_buf 的任何更改。此外，
					 * 在这种情况下，我们将之前的行作为单独的
					 * 历史条目强制输出；我们希望尽可能避免 SQL
					 * 和反斜杠命令混在历史记录中。
				 */
				if (query_buf->len == added_nl_pos)
				{
					query_buf->data[--query_buf->len] = '\0';
					pg_send_history(history_buf);
				}
				added_nl_pos = -1;

				/* 将反斜杠命令保存到历史记录中 */
				if (pset.cur_cmd_interactive && !line_saved_in_history)
				{
					pg_append_history(line, history_buf);
					pg_send_history(history_buf);
					line_saved_in_history = true;
				}

				/* 执行反斜杠命令 */
				slashCmdStatus = HandleSlashCmds(scan_state,
												 cond_stack,
												 query_buf,
												 previous_buf);

				success = slashCmdStatus != PSQL_CMD_ERROR;

				/*
					 * 反斜杠命令后重置 stmt_lineno 并不总是合适，
					 * 但这是我们历史沿用的做法，投诉也很少。
					 *
				 */
				pset.stmt_lineno = 1;

				if (slashCmdStatus == PSQL_CMD_SEND)
				{
					/* 在不活跃分支中不应出现 */
					Assert(conditional_active(cond_stack));

					success = SendQuery(query_buf->data);

					/* 通过指针交换将查询转移到 previous_buf */
					{
						PQExpBuffer swap_buf = previous_buf;

						previous_buf = query_buf;
						query_buf = swap_buf;
					}
					resetPQExpBuffer(query_buf);

					/* 强制发送后刷新任何括号嵌套信息 */
					psql_scan_reset(scan_state);
				}
				else if (slashCmdStatus == PSQL_CMD_NEWEDIT)
				{
					/* 在不活跃分支中不应出现 */
					Assert(conditional_active(cond_stack));
					/* 确保编辑返回的内容以换行符结尾 */
					if (query_buf->len > 0 &&
						query_buf->data[query_buf->len - 1] != '\n')
						appendPQExpBufferChar(query_buf, '\n');
					/* 将 query_buf 作为新输入重新扫描 */
					psql_scan_finish(scan_state);
					free(line);
					line = pg_strdup(query_buf->data);
					resetPQExpBuffer(query_buf);
					/* 重置解析状态，因为我们要重新扫描整行 */
					psql_scan_reset(scan_state);
					psql_scan_setup(scan_state, line, strlen(line),
									pset.encoding, standard_strings());
					line_saved_in_history = false;
					prompt_status = PROMPT_READY;
					/* 解析完现有内容后我们将需要重新显示 */
					need_redisplay = true;
				}
				else if (slashCmdStatus == PSQL_CMD_TERMINATE)
					break;
			}

			/* 如果词法分析器到达 EOL 则退出循环 */
			if (scan_result == PSCAN_INCOMPLETE ||
				scan_result == PSCAN_EOL)
				break;
		}

		/*
			 * 如果尚未完成，将行添加到待处理的历史记录中。然后，
			 * 如果查询缓冲区仍为空，刷新任何未发送的历史条目。
			 * 这意味着查询之前的空行（仅包含空白字符和可能的
			 * 双短划线注释）将被记录为单独的历史条目，
			 * 而不是该查询的一部分。
		 */
		if (pset.cur_cmd_interactive)
		{
			if (!line_saved_in_history)
				pg_append_history(line, history_buf);
			if (query_buf->len == 0)
				pg_send_history(history_buf);
		}

		psql_scan_finish(scan_state);
		free(line);

		if (slashCmdStatus == PSQL_CMD_TERMINATE)
		{
			successResult = EXIT_SUCCESS;
			break;
		}

		if (!pset.cur_cmd_interactive)
		{
			if (!success && die_on_error)
				successResult = EXIT_USER;
			/* 我们是否丢失了数据库连接？ */
			else if (!pset.db)
				successResult = EXIT_BADCONN;
		}
	}							/* while !endoffile/session */

	/*
	 * 如果在文件末尾有一个不以分号结尾的查询，我们处理它，
	 * 除非输入源是交互式的 --- 在这种情况下，似乎最好继续
	 * 退出。如果是错误退出，也跳过。
	 */
	if (query_buf->len > 0 && !pset.cur_cmd_interactive &&
		successResult == EXIT_SUCCESS)
	{
		/* 将查询保存到历史记录中 */
		/* 目前不需要，因为我们不在交互模式下使用此块 */
#ifdef NOT_USED
		if (pset.cur_cmd_interactive)
			pg_send_history(history_buf);
#endif

		/* 执行查询，除非我们在不活跃的 \if 分支中 */
		if (conditional_active(cond_stack))
		{
			success = SendQuery(query_buf->data);
		}
		else
		{
			if (pset.cur_cmd_interactive)
				pg_log_error("query ignored; use \\endif or Ctrl-C to exit current \\if block");
			success = true;
		}

		if (!success && die_on_error)
			successResult = EXIT_USER;
		else if (pset.db == NULL)
			successResult = EXIT_BADCONN;
	}

	/*
	 * 检查不平衡的 \if-\endif，除非用户明确退出或脚本
	 * 正因错误而退出
	 */
	if (slashCmdStatus != PSQL_CMD_TERMINATE &&
		successResult != EXIT_USER &&
		!conditional_stack_empty(cond_stack))
	{
		pg_log_error("reached EOF without finding closing \\endif(s)");
		if (die_on_error && !pset.cur_cmd_interactive)
			successResult = EXIT_USER;
	}

	/*
	 * 让我们确保 SIGINT 处理器在我们退出此例程后
	 * 不会尝试使用 sigint_interrupt_jmp。如果存在外层
	 * MainLoop 实例，它将在其循环顶部将
	 * sigint_interrupt_jmp 重置为指向自身，
	 * 在任何进一步的交互式输入发生之前。
	 */
	sigint_interrupt_enabled = false;

	destroyPQExpBuffer(query_buf);
	destroyPQExpBuffer(previous_buf);
	destroyPQExpBuffer(history_buf);

	psql_scan_destroy(scan_state);
	conditional_stack_destroy(cond_stack);

	pset.cur_cmd_source = prev_cmd_source;
	pset.cur_cmd_interactive = prev_cmd_interactive;
	pset.lineno = prev_lineno;

	return successResult;
}								/* MainLoop() */
