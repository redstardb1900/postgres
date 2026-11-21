/*-------------------------------------------------------------------------
 *
 * saslprep.h
 *	  用于 SCRAM 认证的 SASLprep 规范化
 *
 * 这些定义供前端与后端代码共用。
 *
 * Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * src/include/common/saslprep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SASLPREP_H
#define SASLPREP_H

/*
 * pg_saslprep() 的返回码。
 */
typedef enum
{
	SASLPREP_SUCCESS = 0,
	SASLPREP_OOM = -1,			/*  内存不足（仅前端）  */
	SASLPREP_INVALID_UTF8 = -2, /*  输入不是合法 UTF-8 串  */
	SASLPREP_PROHIBITED = -3	/*  输出会含禁用字符  */
} pg_saslprep_rc;

extern pg_saslprep_rc pg_saslprep(const char *input, char **output);

#endif							/* SASLPREP_H */
