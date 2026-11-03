/*
 *	util.c
 *
 *	utility functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/util.c
 */

#include "postgres_fe.h"

#include <signal.h>

#include "common/username.h"
#include "pg_upgrade.h"

LogOpts		log_opts;

static void pg_log_v(eLogType type, const char *fmt, va_list ap) pg_attribute_printf(2, 0);


/*显示操作结果（ok、failed 等）。功能与 pg_log() 相同，保留以区分操作结果与其他消息。
 * 
 *
 *	
 *
 *	
 *	
 *	
                                                                                                                              */
void
report_status(eLogType type, const char *fmt,...)
{
	va_list		args;

	va_start(args, fmt);
	pg_log_v(type, fmt, args);
	va_end(args);
}


void
end_progress_output(void)
{
	/*tty 输出擦进度行前内容，tty/verbose 缩进使 report_status() 对齐。
	 * 
	 * 
	 * 
	                                                              */
	if (log_opts.isatty)
	{
		printf("\r");
		pg_log(PG_REPORT_NONL, "%-*s", MESSAGE_WIDTH, "");
	}
	else if (log_opts.verbose)
		pg_log(PG_REPORT_NONL, "%-*s", MESSAGE_WIDTH, "");
}

/*退出时移除内部日志。
 * 
                                       */
void
cleanup_output_dirs(void)
{
	fclose(log_opts.internal);

	/* 移除转储和日志？   */
	if (log_opts.retain)
		return;

	/*尝试两次。第二次等文件在 Windows 上完成 unlink。
	 * 
	 * 
	                      */
	if (!rmtree(log_opts.basedir, true))
		rmtree(log_opts.basedir, true);

	/* 仅 pg_upgrade_output.d 空时移除     */
	switch (pg_check_dir(log_opts.rootdir))
	{
		case 0:					/* 不存在    */
		case 3:					/* 存在且含挂载点             */
			Assert(false);
			break;

		case 1:					/* 存在且空     */
		case 2:					/* 存在且仅点文件              */

			/*尝试两次。第二次等文件在 Windows 上完成 unlink。
			 * 
			 * 
			                      */
			if (!rmtree(log_opts.rootdir, true))
				rmtree(log_opts.rootdir, true);
			break;

		case 4:					/* 存在 */

			/*保留根目录含过去日志活动。
			 * 
			 * 
			                         */
			break;

		default:
			/* 其他失败，仅报告          */
			pg_log(PG_WARNING, "could not access directory \"%s\": %m",
				   log_opts.rootdir);
			break;
	}
}

/*显示即将开始操作的描述。填充消息到 MESSAGE_WIDTH 使 ok/failed 对齐。（过长截断勿冗长。）
 * 
 *
 *	
 *	
 *	
 *	
 *
 *	
 *	
 *	
 *	
 *	
 *	
                                                                                                                                                                                                                                                                                                                                                                                   */
void
prep_status(const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/* trim strings */
	pg_log(PG_REPORT_NONL, "%-*s", MESSAGE_WIDTH, message);
}

/*类似 prep_status() 但用于较长操作。当前处理项通过 pg_log(PG_STATUS) 展示。
 * 
 *
 * 
 * 
 * 
 *
 * 
 * 
 * 
 * 
 * 
                                                                                                                                                                                                                                                                                           */
void
prep_status_progress(const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/*tty/verbose 追加换行。pg_log_v() 将进度项放下一行。
	 * 
	 * 
	                                                          */
	if (log_opts.isatty || log_opts.verbose)
		pg_log(PG_REPORT, "%-*s", MESSAGE_WIDTH, message);
	else
		pg_log(PG_REPORT_NONL, "%-*s", MESSAGE_WIDTH, message);
}

static void
pg_log_v(eLogType type, const char *fmt, va_list ap)
{
	char		message[QUERY_ALLOC];

	/* 入消息不应以换行结尾；在此加。                */
	Assert(fmt);
	Assert(fmt[0] == '\0' || fmt[strlen(fmt) - 1] != '\n');

	vsnprintf(message, sizeof(message), _(fmt), ap);

	/* PG_VERBOSE 和 PG_STATUS 仅 verbose 输出              */
	/* log_opts.internal fopen() 可败故检                      */
	if (((type != PG_VERBOSE && type != PG_STATUS) || log_opts.verbose) &&
		log_opts.internal != NULL)
	{
		if (type == PG_STATUS)
			/* 状态消息有两前导空格                    */
			fprintf(log_opts.internal, "  %s\n", message);
		else if (type == PG_REPORT_NONL)
			fprintf(log_opts.internal, "%s", message);
		else
			fprintf(log_opts.internal, "%s\n", message);
		fflush(log_opts.internal);
	}

	switch (type)
	{
		case PG_VERBOSE:
			if (log_opts.verbose)
				printf("%s\n", message);
			break;

		case PG_STATUS:

			/*终端输出加两空格不要换行（\r 使下条同行）。左截断适配 MESSAGE_WIDTH。非交互仅 verbose 展示，否则输出过大。
			 * 
			 * 
			 * 
			 * 
			 *
			 * 
			 * 
			 * 
			                                                                                                                                                                                                                    */
			if (log_opts.isatty)
			{
				bool		itfits = (strlen(message) <= MESSAGE_WIDTH - 2);

				/* 前导截前缀 "..."                         */
				printf("  %s%-*.*s\r",
					   itfits ? "" : "...",
					   MESSAGE_WIDTH - 2, MESSAGE_WIDTH - 2,
					   itfits ? message :
					   message + strlen(message) - MESSAGE_WIDTH + 3 + 2);
			}
			else if (log_opts.verbose)
				printf("  %s\n", message);
			break;

		case PG_REPORT_NONL:
			/* prep_status 及关联用                          */
			printf("%s", message);
			break;

		case PG_REPORT:
		case PG_WARNING:
			printf("%s\n", message);
			break;

		case PG_FATAL:
			/* 中断状态输出额外换行                         */
			printf("\n%s\n", message);
			printf(_("Failure, exiting\n"));
			exit(1);
			break;

			/* 无 default：望收略况警告                 */
	}
	fflush(stdout);
}


void
pg_log(eLogType type, const char *fmt,...)
{
	va_list		args;

	va_start(args, fmt);
	pg_log_v(type, fmt, args);
	va_end(args);
}


void
pg_fatal(const char *fmt,...)
{
	va_list		args;

	va_start(args, fmt);
	pg_log_v(PG_FATAL, fmt, args);
	va_end(args);
	/* 一切好  */
	printf(_("Failure, exiting\n"));
	exit(1);
}


void
check_ok(void)
{
	/* all seems well */
	report_status(PG_REPORT, "ok");
}


/*制作副本
 * 
 *	
 *
 * 
 * 
                                                                                                                                                                     */
char *
quote_identifier(const char *s)
{
	char	   *result = pg_malloc(strlen(s) * 2 + 3);
	char	   *r = result;

	*r++ = '"';
	while (*s)
	{
		if (*s == '"')
			*r++ = *s;
		*r++ = *s;
		s++;
	}
	*r++ = '"';
	*r++ = '\0';

	return result;
}


/*
 * get_user_info()
 */
int
get_user_info(char **user_name_p)
{
	int			user_id;
	const char *user_name;
	char	   *errstr;

#ifndef WIN32
	user_id = geteuid();
#else
	user_id = 1;
#endif

	user_name = get_user_name(&errstr);
	if (!user_name)
		pg_fatal("%s", errstr);

	/* make a copy */
	*user_name_p = pg_strdup(user_name);

	return user_id;
}


/*
 *	str2uint()
 *
 *	convert string to oid
 */
unsigned int
str2uint(const char *str)
{
	return strtoul(str, NULL, 10);
}
