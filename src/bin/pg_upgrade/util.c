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


/*
 * report_status()
 *
 *	显示一次操作的结果（ok、failed、错误消息等）。  
 *
 *	这在功能上已与 pg_log() 无差别，但我们仍保留它，
 *	以在操作结果与其他消息之间                         
 *	保持记法上的区别。
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
	/*
	 * 对于输出到 tty 的情况，擦除进度行先前的内容。当处
	 * 于 tty 或详细模式时，缩进使 report_status() 输出能
	 * 对齐 
	 */
	if (log_opts.isatty)
	{
		printf("\r");
		pg_log(PG_REPORT_NONL, "%-*s", MESSAGE_WIDTH, "");
	}
	else if (log_opts.verbose)
		pg_log(PG_REPORT_NONL, "%-*s", MESSAGE_WIDTH, "");
}

/*
 * 删除内部生成的所有日志。仅在退出时使用一次。  
 */
void
cleanup_output_dirs(void)
{
	fclose(log_opts.internal);

	/* 移除转储和日志？   */
	if (log_opts.retain)
		return;

	/*
	 * 尝试两次。在 Windows 上，第二次可能要等文件    
	 * 完成解除链接。
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

			/*
			 * 尝试两次。第二次可能在 Windows 上等待文件 
			 * 完成解除链接。      
			 */
			if (!rmtree(log_opts.rootdir, true))
				rmtree(log_opts.rootdir, true);
			break;

		case 4:					/* 存在 */

			/*
			 * 保留根目录，因为它包含一些过去的日志
			 * 活动。
			 */
			break;

		default:
			/* 其他失败，仅报告          */
			pg_log(PG_WARNING, "could not access directory \"%s\": %m",
				   log_opts.rootdir);
			break;
	}
}

/*
 * prep_status
 *
 *	显示一条消息，描述我们即将开始的操作。我们会把
 *	消息填充到 MESSAGE_WIDTH 个字符，以便所有的"ok"        
 *	和 "failed" 指示符都整齐排列。（过长的消息会被  
 *	截断，所以不要太过冗长。）      
 *
 *	典型的序列看起来像这样：    
 *		prep_status("about to flarb the next %d files", fileCount);
 *		if ((message = flarbFiles(fileCount)) == NULL)
 *		  report_status(PG_REPORT, "ok");
 *		else
 *		  pg_log(PG_FATAL, "failed: %s", message);
 */
void
prep_status(const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/* 修剪串    */
	pg_log(PG_REPORT_NONL, "%-*s", MESSAGE_WIDTH, message);
}

/*
 * prep_status_progress
 *
 *   类似 prep_status()，但用于可能耗时较长的操作。    
 *   关于当前正在处理哪个条目的详细信息可以通过      
 *   pg_log(PG_STATUS, ...) 显示。典型的序列看起来如下：    
 *
 *   prep_status_progress("copying files");
 *   for (...)
 *     pg_log(PG_STATUS, "%s", filename);
 *   end_progress_output();
 *   report_status(PG_REPORT, "ok");
 */
void
prep_status_progress(const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/*
	 * 如果输出到 tty 或处于详细模式，则追加换行符。    
	 * pg_log_v() 会把各个进度项放到下一行。   
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

			/*
			 * 输出到终端时，我们加两个前导空格且不加 
			 * 换行；而是追加 \r，使下一条消息输出在同一 
			 * 行上。左截断以适配 MESSAGE_WIDTH（这两 
			 * 个空格也算在内）。                         
			 *
			 * 对非交互式输出，仅在启用了详细模式时才显
			 * 示进度；否则的话，默认情况下输出会大到 
			 * 不合理。     
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

			/* 无 default：希望对省略情形给出警告  */
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
	/* NOTREACHED */
	printf(_("Failure, exiting\n"));
	exit(1);
}


void
check_ok(void)
{
	/* 一切正常   */
	report_status(PG_REPORT, "ok");
}


/*
 * quote_identifier()
 *		恰当地为 SQL 标识符加双引号 
 *
 * 结果应使用 pg_free 释放，但多数调用者不这么做，   
 * 因为内存泄漏对本程序无关紧要。    
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
 * 获用户信息
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

	/* 复制一份*/
	*user_name_p = pg_strdup(user_name);

	return user_id;
}


/*
 *	str2uint()
 *
 *	字符串转 oid     
 */
unsigned int
str2uint(const char *str)
{
	return strtoul(str, NULL, 10);
}
