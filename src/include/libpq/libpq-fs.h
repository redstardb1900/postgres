/*-------------------------------------------------------------------------
 *
 * libpq-fs.h
 *	  使用 Inversion 文件系统例程（即大对象）的定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/libpq-fs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_FS_H
#define LIBPQ_FS_H

/*
 * inversion（大对象）调用的读/写模式标志
 */

#define INV_WRITE		0x00020000
#define INV_READ		0x00040000

#endif							/* LIBPQ_FS_H */
