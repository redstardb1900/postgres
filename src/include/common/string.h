/*
 *	string.h
 *		字符串处理辅助
 *
 *	Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/include/common/string.h
 */
#ifndef COMMON_STRING_H
#define COMMON_STRING_H

#include <signal.h>

struct StringInfoData;			/*  避免在此包含 stringinfo.h  */

typedef struct PromptInterruptContext
{
	/*  为免在此含 <setjmp.h>，jmpbuf 声明为 "void *"  */
	void	   *jmpbuf;			/* 略 */
	volatile sig_atomic_t *enabled; /*  启用中断时 longjmp 的标志  */
	bool		canceled;		/*  表示是否发生了取消  */
} PromptInterruptContext;

/* 略 */
extern bool pg_str_endswith(const char *str, const char *end);
extern int	strtoint(const char *pg_restrict str, char **pg_restrict endptr,
					 int base);
extern char *pg_clean_ascii(const char *str, int alloc_flags);
extern int	pg_strip_crlf(char *str);
extern bool pg_is_ascii(const char *str);

/* functions in src/common/pg_get_line.c */
extern char *pg_get_line(FILE *stream, PromptInterruptContext *prompt_ctx);
extern bool pg_get_line_buf(FILE *stream, struct StringInfoData *buf);
extern bool pg_get_line_append(FILE *stream, struct StringInfoData *buf,
							   PromptInterruptContext *prompt_ctx);

/* 略 */
extern char *simple_prompt(const char *prompt, bool echo);
extern char *simple_prompt_extended(const char *prompt, bool echo,
									PromptInterruptContext *prompt_ctx);

#endif							/* COMMON_STRING_H */
