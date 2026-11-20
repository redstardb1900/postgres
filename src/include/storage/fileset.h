/*-------------------------------------------------------------------------
 *
 * fileset.h
 *	  具名临时文件的管理。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fileset.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FILESET_H
#define FILESET_H

#include "storage/fd.h"

/*
 * 一组临时文件。
 */
typedef struct FileSet
{
	pid_t		creator_pid;	/* 创建进程的 PID */
	uint32		number;			/* 每 PID 标识 */
	int			ntablespaces;	/* 所用表空间数目 */
	Oid			tablespaces[8]; /* 所用表空间的 OIDs。假定很少
								 * 有比这更多的临时
								 * 表空间。 */
} FileSet;

extern void FileSetInit(FileSet *fileset);
extern File FileSetCreate(FileSet *fileset, const char *name);
extern File FileSetOpen(FileSet *fileset, const char *name,
						int mode);
extern bool FileSetDelete(FileSet *fileset, const char *name,
						  bool error_on_failure);
extern void FileSetDeleteAll(FileSet *fileset);

#endif
