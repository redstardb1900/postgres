/*--------------------------------------------------------------------
 * conffiles.h
 *
 * 与配置文件相关的实用工具。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/conffiles.h
 *
 *--------------------------------------------------------------------
 */
#ifndef CONFFILES_H
#define CONFFILES_H

/*  配置文件的递归嵌套深度  */
#define CONF_FILE_START_DEPTH	0
#define CONF_FILE_MAX_DEPTH		10

extern char *AbsoluteConfigLocation(const char *location,
									const char *calling_file);
extern char **GetConfFilesInDir(const char *includedir,
								const char *calling_file,
								int elevel, int *num_filenames,
								char **err_msg);

#endif							/* CONFFILES_H */
