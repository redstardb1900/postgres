/*-------------------------------------------------------------------------
 *
 * help_config.h
 *		main.c 的 --help-config 选项接口
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *	  src/include/utils/help_config.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HELP_CONFIG_H
#define HELP_CONFIG_H 1

extern void GucInfoMain(void) pg_attribute_noreturn();

#endif
