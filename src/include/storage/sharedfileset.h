/*-------------------------------------------------------------------------
 *
 * sharedfileset.h
 *	  共享临时文件管理。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sharedfileset.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SHAREDFILESET_H
#define SHAREDFILESET_H

#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/fileset.h"
#include "storage/spin.h"

/*
 * 一组可被多个后端共享的临时文件。
 */
typedef struct SharedFileSet
{
	FileSet		fs;
	slock_t		mutex;			/* 保护引用计数的互斥锁 */
	int			refcnt;			/* 已附着的后端数目 */
} SharedFileSet;

extern void SharedFileSetInit(SharedFileSet *fileset, dsm_segment *seg);
extern void SharedFileSetAttach(SharedFileSet *fileset, dsm_segment *seg);
extern void SharedFileSetDeleteAll(SharedFileSet *fileset);

#endif
