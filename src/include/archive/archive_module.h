/*-------------------------------------------------------------------------
 *
 * archive_module.h
 *		归档模块的导出。
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * src/include/archive/archive_module.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _ARCHIVE_MODULE_H
#define _ARCHIVE_MODULE_H

/*
 * GUC archive_library 的值。
 */
extern PGDLLIMPORT char *XLogArchiveLibrary;

typedef struct ArchiveModuleState
{
	/*
	 * 归档模块可用的私有数据指针。可用于存放
	 * 将传给其各回调的模块状态。
	 * 。
	 */
	void	   *private_data;
} ArchiveModuleState;

/*
 * 归档模块回调
 *
 * 这些回调应由归档库定义，并经
 * _PG_archive_module_init() 返回。仅 ArchiveFileCB 必需。
 * 各回调用途详见
 * 归档模块文档。
 */
typedef void (*ArchiveStartupCB) (ArchiveModuleState *state);
typedef bool (*ArchiveCheckConfiguredCB) (ArchiveModuleState *state);
typedef bool (*ArchiveFileCB) (ArchiveModuleState *state, const char *file, const char *path);
typedef void (*ArchiveShutdownCB) (ArchiveModuleState *state);

typedef struct ArchiveModuleCallbacks
{
	ArchiveStartupCB startup_cb;
	ArchiveCheckConfiguredCB check_configured_cb;
	ArchiveFileCB archive_file_cb;
	ArchiveShutdownCB shutdown_cb;
} ArchiveModuleCallbacks;

/*
 * 加载归档库时查找的共享库符号
 * _PG_archive_module_init 类型。
 */
typedef const ArchiveModuleCallbacks *(*ArchiveModuleInit) (void);

extern PGDLLEXPORT const ArchiveModuleCallbacks *_PG_archive_module_init(void);

#endif							/* _ARCHIVE_MODULE_H */
