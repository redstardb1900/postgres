/*-------------------------------------------------------------------------
 *
 * 文件与目录权限定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/file_perm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILE_PERM_H
#define FILE_PERM_H

#include <sys/stat.h>

/*
 * 仅允许属主读写目录与文件的数据目录权限
 * 模式掩码。
 *
 * 此为默认。
 */
#define PG_MODE_MASK_OWNER		    (S_IRWXG | S_IRWXO)

/*
 * 还允许组读/执行的数据目录权限模式掩码。
 */
#define PG_MODE_MASK_GROUP			(S_IWGRP | S_IRWXO)

/*  创建目录的默认模式  */
#define PG_DIR_MODE_OWNER			S_IRWXU

/*  允许组读/执行的创建目录模式  */
#define PG_DIR_MODE_GROUP			(S_IRWXU | S_IRGRP | S_IXGRP)

/*  创建文件的默认模式  */
#define PG_FILE_MODE_OWNER		    (S_IRUSR | S_IWUSR)

/*  允许组读的创建文件模式  */
#define PG_FILE_MODE_GROUP			(S_IRUSR | S_IWUSR | S_IRGRP)

/*  在数据目录中创建目录与文件的模式  */
extern PGDLLIMPORT int pg_dir_create_mode;
extern PGDLLIMPORT int pg_file_create_mode;

/* umask() 用的模式掩码 */
extern PGDLLIMPORT int pg_mode_mask;

/*  按给定模式设置权限与掩码  */
extern void SetDataDirectoryCreatePerm(int dataDirMode);

/*  按数据目录模式设置权限与掩码  */
extern bool GetDataDirectoryCreatePerm(const char *dataDir);

#endif							/* FILE_PERM_H */
