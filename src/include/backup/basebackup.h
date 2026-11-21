/*-------------------------------------------------------------------------
 *
 * basebackup.h
 *	  自 basebackup.c 的导出。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BASEBACKUP_H
#define _BASEBACKUP_H

#include "nodes/replnodes.h"

/*
 * BASE_BACKUP 命令中 MAX_RATE 选项的最小与最大值。
 */
#define MAX_RATE_LOWER	32
#define MAX_RATE_UPPER	1048576

/*
 * 表空间信息
 *
 * 某些用法下 path 可为 NULL，表示 PGDATA 目录本身。
 */
typedef struct
{
	char	   *oid;			/*  表空间 OID，十进制字符串  */
	char	   *path;			/*  表空间目录的完整路径  */
	char	   *rpath;			/*  PGDATA 内为相对路径，否则
								 * NULL */
	int64		size;			/*  已发送总大小；未知 -1  */
} tablespaceinfo;

extern void SendBaseBackup(BaseBackupCmd *cmd);

#endif							/* _BASEBACKUP_H */
