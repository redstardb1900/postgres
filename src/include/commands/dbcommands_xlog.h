/*-------------------------------------------------------------------------
 *
 * dbcommands_xlog.h
 *	数据库资源管理器 XLOG 定义（创建/删除数据库）。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/dbcommands_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_XLOG_H
#define DBCOMMANDS_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/* 记录类型 */
#define XLOG_DBASE_CREATE_FILE_COPY		0x00
#define XLOG_DBASE_CREATE_WAL_LOG		0x10
#define XLOG_DBASE_DROP					0x20

/*
 * 整次 CREATE DATABASE 操作的单条 WAL 记录。用于
 * FILE_COPY 策略。
 */
typedef struct xl_dbase_create_file_copy_rec
{
	Oid			db_id;
	Oid			tablespace_id;
	Oid			src_db_id;
	Oid			src_tablespace_id;
} xl_dbase_create_file_copy_rec;

/*
 * CREATE DATABASE 操作开始时的 WAL 记录，当使用
 * WAL_LOG 策略时。各块随后将分别
 * 记录。
 */
typedef struct xl_dbase_create_wal_log_rec
{
	Oid			db_id;
	Oid			tablespace_id;
} xl_dbase_create_wal_log_rec;

typedef struct xl_dbase_drop_rec
{
	Oid			db_id;
	int			ntablespaces;	/* 表空间 ID 个数 */
	Oid			tablespace_ids[FLEXIBLE_ARRAY_MEMBER];
} xl_dbase_drop_rec;
#define MinSizeOfDbaseDropRec offsetof(xl_dbase_drop_rec, tablespace_ids)

extern void dbase_redo(XLogReaderState *record);
extern void dbase_desc(StringInfo buf, XLogReaderState *record);
extern const char *dbase_identify(uint8 info);

#endif							/* DBCOMMANDS_XLOG_H */
