/*--------------------------------------------------------------------
 * guc_internal.h
 *
 * guc.c 与 guc-file.l 共用的声明
 * backend/utils/misc/guc-file.l
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/guc_internal.h
 *--------------------------------------------------------------------
 */
#ifndef GUC_INTERNAL_H
#define GUC_INTERNAL_H

#include "utils/guc.h"

extern int	guc_name_compare(const char *namea, const char *nameb);
extern ConfigVariable *ProcessConfigFileInternal(GucContext context,
												 bool applySettings, int elevel);
extern void record_config_file_error(const char *errmsg,
									 const char *config_file,
									 int lineno,
									 ConfigVariable **head_p,
									 ConfigVariable **tail_p);

#endif							/* GUC_INTERNAL_H */
