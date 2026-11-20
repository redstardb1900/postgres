/*-------------------------------------------------------------------------
 *
 * buffile.h
 *	  大型缓冲临时文件的管理。
 *
 * BufFile 例程提供 stdio 的部分替代，位于
 * fd.c 所管虚拟文件描述符之上。目前仅支持
 * 对虚拟文件的缓冲访问，不含 stdio 的格式化
 * 功能。这已够眼前所需，但设施集
 * 必要时可扩充。
 *
 * BufFile 也支持处理超出 OS 文件大小限制及/或
 * int 所能表示最大偏移的临时文件。
 * 把这部分拆成可单独访问的模块或许更好，
 * 但目前无需不带缓冲访问的超大临时
 * 文件.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buffile.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BUFFILE_H
#define BUFFILE_H

#include "storage/fileset.h"

/* BufFile 是不透明类型，细节在 buffile.c 外不可知。 */

typedef struct BufFile BufFile;

/*
 * buffile.c 中函数原型
 */

extern BufFile *BufFileCreateTemp(bool interXact);
extern void BufFileClose(BufFile *file);
extern pg_nodiscard size_t BufFileRead(BufFile *file, void *ptr, size_t size);
extern void BufFileReadExact(BufFile *file, void *ptr, size_t size);
extern size_t BufFileReadMaybeEOF(BufFile *file, void *ptr, size_t size, bool eofOK);
extern void BufFileWrite(BufFile *file, const void *ptr, size_t size);
extern int	BufFileSeek(BufFile *file, int fileno, off_t offset, int whence);
extern void BufFileTell(BufFile *file, int *fileno, off_t *offset);
extern int	BufFileSeekBlock(BufFile *file, long blknum);
extern int64 BufFileSize(BufFile *file);
extern long BufFileAppend(BufFile *target, BufFile *source);

extern BufFile *BufFileCreateFileSet(FileSet *fileset, const char *name);
extern void BufFileExportFileSet(BufFile *file);
extern BufFile *BufFileOpenFileSet(FileSet *fileset, const char *name,
								   int mode, bool missing_ok);
extern void BufFileDeleteFileSet(FileSet *fileset, const char *name,
								 bool missing_ok);
extern void BufFileTruncateFileSet(BufFile *file, int fileno, off_t offset);

#endif							/* BUFFILE_H */
