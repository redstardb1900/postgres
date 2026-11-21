/*-------------------------------------------------------------------------
 *
 * shell_archive.h
 *		经 shell 归档的导出。
 *
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * src/include/archive/shell_archive.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SHELL_ARCHIVE_H
#define _SHELL_ARCHIVE_H

#include "archive/archive_module.h"

/*
 * 经 shell 命令归档的逻辑在核心服务器内，
 * 无需经共享库加载，故有特殊的
 * 初始化函数。
 */
extern const ArchiveModuleCallbacks *shell_archive_init(void);

#endif							/* _SHELL_ARCHIVE_H */
