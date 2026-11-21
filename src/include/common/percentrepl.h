/*-------------------------------------------------------------------------
 *
 * percentrepl.h
 *	  替换串中百分号占位符的公共例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/percentrepl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PERCENTREPL_H
#define PERCENTREPL_H

extern char *replace_percent_placeholders(const char *instr, const char *param_name, const char *letters,...);

#endif							/* PERCENTREPL_H */
