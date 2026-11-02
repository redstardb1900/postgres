/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/input.c
 */
#include "postgres_fe.h"

#ifndef WIN32
#include <unistd.h>
#endif
#include <fcntl.h>
#include <limits.h>

#include "common.h"
#include "common/logging.h"
#include "input.h"
#include "settings.h"
#include "tab-complete.h"

#ifndef WIN32
#define PSQLHISTORY ".psql_history"
#else
#define PSQLHISTORY "psql_history"
#endif

/* 关闭 readline 和历史记录的运行时选项 */
/* （当然没有用于执行此操作的运行时命令 :) */
#ifdef USE_READLINE
static bool useReadline;
static bool useHistory;

static char *psql_history;

static int	history_lines_added;


/*
 *	通过将 '\n' 映射到 NL_IN_HISTORY 来保留已保存查询中的换行符
 *
 *	假设 NL_IN_HISTORY 永远不会被用户输入，
 *	也不会出现在多字节字符串中。0x00 不能由 readline
 *	例程正确处理，因此不能用于此目的。
 *	
 */
#define NL_IN_HISTORY	0x01
#endif

static void finishInput(void);


/*
 * gets_interactive()
 *
 * 获取一行交互式输入，如果需要则使用 readline。
 *
 * prompt: 要使用的提示字符串
 * query_buf: 包含当前命令中已读取行的缓冲区
 * （query_buf 不会在这里修改，但可能会被 tab 补全查询）
 *
 * 结果是 malloc'd 的字符串。
 *
 * 调用者在调用前*必须*已设置 sigint_interrupt_jmp。
 */
char *
gets_interactive(const char *prompt, PQExpBuffer query_buf)
{
#ifdef USE_READLINE
	if (useReadline)
	{
		char	   *result;

		/*
		 * 某些版本的 readline 不会注意到在未主动读取输入时到达的
		 * SIGWINCH 信号。最简单的修复方法是始终重新读取终端大小。
		 * 这会在 readline() 启用 libreadline 信号处理程序之前
		 * 留下一个 SIGWINCH 可能被错过的窗口，但该窗口可能足够短
		 * 可以忽略。
		 */
#ifdef HAVE_RL_RESET_SCREEN_SIZE
		rl_reset_screen_size();
#endif

		/* 使当前 query_buf 可用于 tab 补全回调 */
		tab_completion_query_buf = query_buf;

		/* 启用 SIGINT 以 longjmp 到 sigint_interrupt_jmp */
		sigint_interrupt_enabled = true;

		/* 在某些平台上，readline 声明为 readline(char *) */
		result = readline((char *) prompt);

		/* 再次禁用 SIGINT */
		sigint_interrupt_enabled = false;

		/* 纯粹的整洁主义 */
		tab_completion_query_buf = NULL;

		return result;
	}
#endif

	fputs(prompt, stdout);
	fflush(stdout);
	return gets_fromFile(stdin);
}


/*
 * 将行追加到历史缓冲区，确保末尾有 '\n'
 */
void
pg_append_history(const char *s, PQExpBuffer history_buf)
{
#ifdef USE_READLINE
	if (useHistory && s)
	{
		appendPQExpBufferStr(history_buf, s);
		if (!s[0] || s[strlen(s) - 1] != '\n')
			appendPQExpBufferChar(history_buf, '\n');
	}
#endif
}


/*
 * 将累积的历史条目发送到 readline 的历史机制，
 * 然后将缓冲区重置为空。
 *
 * 注意：如果 history_buf 为空，我们不会写入任何内容，因此对此函数
 * 的额外调用不会有影响。在执行任何操作之前，必须至少有
 * pg_append_history 添加的一行。
 */
void
pg_send_history(PQExpBuffer history_buf)
{
#ifdef USE_READLINE
	static char *prev_hist = NULL;

	char	   *s = history_buf->data;
	int			i;

	/* 修剪尾随的 \n（可以在 history_buf 上涂写） */
	for (i = strlen(s) - 1; i >= 0 && s[i] == '\n'; i--)
		;
	s[i + 1] = '\0';

	if (useHistory && s[0])
	{
		if (((pset.histcontrol & hctl_ignorespace) &&
			 s[0] == ' ') ||
			((pset.histcontrol & hctl_ignoredups) &&
			 prev_hist && strcmp(s, prev_hist) == 0))
		{
			/* 就历史记录而言，忽略此行 */
		}
		else
		{
			/* 保存上一行以进行 ignoredups 处理 */
			free(prev_hist);
			prev_hist = pg_strdup(s);
			/* 并将其发送到 readline */
			add_history(s);
			/* 计算添加到历史记录的行数以备后用 */
			history_lines_added++;
		}
	}

	resetPQExpBuffer(history_buf);
#endif
}


/*
 * gets_fromFile
 *
 * 从文件（可以是 stdin）获取一行非交互式输入。
 * 结果是 malloc'd 的字符串，或在 EOF 或输入错误时为 NULL。
 *
 * 调用者在调用前*必须*已设置 sigint_interrupt_jmp。
 *
 * 注意：我们为每次调用重用静态 PQExpBuffer。这是为了避免
 * 被 SIGINT 中断时泄漏内存。
 */
char *
gets_fromFile(FILE *source)
{
	static PQExpBuffer buffer = NULL;

	char		line[1024];

	if (buffer == NULL)			/* 第一次进入？ */
		buffer = createPQExpBuffer();
	else
		resetPQExpBuffer(buffer);

	for (;;)
	{
		char	   *result;

		/* 启用 SIGINT 以 longjmp 到 sigint_interrupt_jmp */
		sigint_interrupt_enabled = true;

		/* 获取一些数据 */
		result = fgets(line, sizeof(line), source);

		/* 再次禁用 SIGINT */
		sigint_interrupt_enabled = false;

		/* EOF 或错误？ */
		if (result == NULL)
		{
			if (ferror(source))
			{
				pg_log_error("could not read from input file: %m");
				return NULL;
			}
			break;
		}

		appendPQExpBufferStr(buffer, line);

		if (PQExpBufferBroken(buffer))
		{
			pg_log_error("out of memory");
			return NULL;
		}

		/* 行尾？ */
		if (buffer->len > 0 && buffer->data[buffer->len - 1] == '\n')
		{
			buffer->data[buffer->len - 1] = '\0';
			return pg_strdup(buffer->data);
		}
	}

	if (buffer->len > 0)		/* 读取一些缓冲区内容后的 EOF */
		return pg_strdup(buffer->data);

	/* EOF，因此返回 null */
	return NULL;
}


#ifdef USE_READLINE

/*
 * 按顺序遍历历史列表每个元素的宏
 *
 * 你可能认为这足够简单，但 libedit 以其独特的方式成功地破坏了它：
 * 在 libreadline 中我们必须使用 next_history() 从最旧到最新遍历，
 * 但在 libedit 中我们必须使用 previous_history()。
 * 为了检测该做什么，我们进行 previous_history() 的试探调用：
 * 如果失败，则要么使用 next_history()，要么有零个或一个历史条目
 * 以至于朝哪个方向前进无关紧要。
 *
 * 如果那还不够令人作呕：下面的代码不像看起来那么显而易见。
 * 在某些 libedit 版本中，history_set_pos(0) 会失败，直到至少
 * 调用过一次 add_history()。对于 printHistory() 或 encode_history()
 * 这不是问题，它们在发生之前无法被调用。在 decode_history() 中
 * 情况并非如此，实际发生的是我们一开始位于最新条目上，
 * previous_history() 失败，我们使用 next_history() 遍历所有条目。
 * 因此当使用这样的 libedit 版本时，decode_history() 循环
 * 以错误的顺序遍历条目，如果在某些 add_history() 调用发生之前
 * 还有另一次使用 BEGIN_ITERATE_HISTORY() 的尝试，它将无法工作。
 * 幸运的是我们不在乎这两种情况。
 *
 * 使用模式如下：
 *
 *		BEGIN_ITERATE_HISTORY(varname);
 *		{
 *			引用 varname->line 的循环体；
 *		}
 *		END_ITERATE_HISTORY();
 *
 */
#define BEGIN_ITERATE_HISTORY(VARNAME) \
	do { \
		HIST_ENTRY *VARNAME; \
		bool		use_prev_; \
		\
		history_set_pos(0); \
		use_prev_ = (previous_history() != NULL); \
		history_set_pos(0); \
		for (VARNAME = current_history(); VARNAME != NULL; \
			 VARNAME = use_prev_ ? previous_history() : next_history()) \
		{ \
			(void) 0

#define END_ITERATE_HISTORY() \
		} \
	} while(0)


/*
 * 将换行符转换为 NL_IN_HISTORY，以便安全保存到 readline 历史文件中
 */
static void
encode_history(void)
{
	BEGIN_ITERATE_HISTORY(cur_hist);
	{
		char	   *cur_ptr;

		/* 某些平台将 HIST_ENTRY.line 声明为 const char * */
		for (cur_ptr = (char *) cur_hist->line; *cur_ptr; cur_ptr++)
		{
			if (*cur_ptr == '\n')
				*cur_ptr = NL_IN_HISTORY;
		}
	}
	END_ITERATE_HISTORY();
}

/*
 * 反转上述编码
 */
static void
decode_history(void)
{
	BEGIN_ITERATE_HISTORY(cur_hist);
	{
		char	   *cur_ptr;

		/* 某些平台将 HIST_ENTRY.line 声明为 const char * */
		for (cur_ptr = (char *) cur_hist->line; *cur_ptr; cur_ptr++)
		{
			if (*cur_ptr == NL_IN_HISTORY)
				*cur_ptr = '\n';
		}
	}
	END_ITERATE_HISTORY();
}
#endif							/* USE_READLINE */


/*
 * 将输入相关的启动内容放在此处。保持这种抽象是好的。
 *
 * 目前唯一的"标志"是 1 表示使用 readline 和历史记录。
 *
 */
void
initializeInput(int flags)
{
#ifdef USE_READLINE
	if (flags & 1)
	{
		const char *histfile;
		char		home[MAXPGPATH];

		useReadline = true;

		/* 为 Readline 的全局变量设置适当的值 */
		initialize_readline();

#ifdef HAVE_RL_VARIABLE_BIND
		/* 将 comment-begin 设置为对 SQL 有用的值 */
		(void) rl_variable_bind("comment-begin", "-- ");
#endif

		/* 这会读取 ~/.inputrc，所以在 rl_variable_bind 之后执行 */
		rl_initialize();

		useHistory = true;
		using_history();
		history_lines_added = 0;

		histfile = GetVariable(pset.vars, "HISTFILE");

		if (histfile == NULL)
		{
			char	   *envhist;

			envhist = getenv("PSQL_HISTORY");
			if (envhist != NULL && strlen(envhist) > 0)
				histfile = envhist;
		}

		if (histfile == NULL)
		{
			if (get_home_path(home))
				psql_history = psprintf("%s/%s", home, PSQLHISTORY);
		}
		else
		{
			psql_history = pg_strdup(histfile);
			expand_tilde(&psql_history);
		}

		if (psql_history)
		{
			read_history(psql_history);
			decode_history();
		}
	}
#endif

	atexit(finishInput);
}


/*
 * 此函数在 psql 退出时保存 readline 历史记录。
 *
 * fname: 历史文件的路径名。（实际上应该是 "const char *"，
 * 但某些古老版本的 readline 省略了 const 修饰。）
 *
 * max_lines: 如果 >= 0，将历史文件限制为该数量的条目。
 */
#ifdef USE_READLINE
static bool
saveHistory(char *fname, int max_lines)
{
	int			errnum;

	/*
	 * 当 HISTFILE 设置为 /dev/null 时抑制写尝试可能看起来像是
	 * 微不足道的优化，但在 macOS 等系统上是必要的，因为
	 * write_history 会尝试 chmod 目标文件而失败。
	 *
	 */
	if (strcmp(fname, DEVNULL) != 0)
	{
		/*
		 * 编码 \n，因为否则 readline 会将多行历史条目重新加载为
		 * 单独的行。（libedit 实际上不需要这个，但无论如何我们
		 * 都这样做，因为很难判断我们正在使用哪个实现。）
		 *
		 */
		encode_history();

		/*
		 * 在较新版本的 libreadline 上，根据需要截断历史文件，
		 * 然后追加我们添加的内容。这避免了覆盖其他并发会话的
		 * 历史记录（尽管当两个会话大约同时退出时仍存在竞争条件）。
		 * 如果我们没有这些函数，退回到 write_history()。
		 *
		 */
#if defined(HAVE_HISTORY_TRUNCATE_FILE) && defined(HAVE_APPEND_HISTORY)
		{
			int			nlines;
			int			fd;

			/* 如果需要，截断以前的条目 */
			if (max_lines >= 0)
			{
				nlines = Max(max_lines - history_lines_added, 0);
				(void) history_truncate_file(fname, nlines);
			}
			/* append_history 在文件尚不存在时失败 :-( */
			fd = open(fname, O_CREAT | O_WRONLY | PG_BINARY, 0600);
			if (fd >= 0)
				close(fd);
			/* 追加适当数量的行 */
			if (max_lines >= 0)
				nlines = Min(max_lines, history_lines_added);
			else
				nlines = history_lines_added;
			errnum = append_history(nlines, fname);
			if (errnum == 0)
				return true;
		}
#else							/* 没有 append 支持 */
		{
			/* 截断已有的记录…… */
			if (max_lines >= 0)
				stifle_history(max_lines);
			/* ……并覆盖文件。并发会话不幸了。 */
			errnum = write_history(fname);
			if (errnum == 0)
				return true;
		}
#endif

		pg_log_error("could not save history to file \"%s\": %m", fname);
	}
	return false;
}
#endif



/*
 * 将历史记录打印到指定文件，如果 fname 为 NULL 则打印到控制台
 * （psql \s 命令）
 *
 * 我们曾经使用 saveHistory() 来实现此目的，但它不允许使用
 * 分页器；此外 libedit 的实现行为不兼容（偏好编码其输出），
 * 并且当目标文件指定为 /dev/tty 时可能完全失败。
 *
 */
bool
printHistory(const char *fname, unsigned short int pager)
{
#ifdef USE_READLINE
	FILE	   *output;
	bool		is_pager;

	if (!useHistory)
		return false;

	if (fname == NULL)
	{
		/* 打印到控制台时，如果启用了分页器则使用 */
		output = PageOutput(INT_MAX, pager ? &(pset.popt.topt) : NULL);
		is_pager = true;
	}
	else
	{
		output = fopen(fname, "w");
		if (output == NULL)
		{
			pg_log_error("could not save history to file \"%s\": %m", fname);
			return false;
		}
		is_pager = false;
	}

	BEGIN_ITERATE_HISTORY(cur_hist);
	{
		fprintf(output, "%s\n", cur_hist->line);
	}
	END_ITERATE_HISTORY();

	if (is_pager)
		ClosePager(output);
	else
		fclose(output);

	return true;
#else
	pg_log_error("history is not supported by this installation");
	return false;
#endif
}


static void
finishInput(void)
{
#ifdef USE_READLINE
	if (useHistory && psql_history)
	{
		(void) saveHistory(psql_history, pset.histsize);
		free(psql_history);
		psql_history = NULL;
	}
#endif
}
