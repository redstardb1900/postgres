/*-------------------------------------------------------------------------
 * message.h
 *	   自 replication/logical/message.c 导出
 *
 * Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 * src/include/replication/message.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_LOGICAL_MESSAGE_H
#define PG_LOGICAL_MESSAGE_H

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "access/xlogreader.h"

/*
 * 通用逻辑解码消息 wal 记录。
 */
typedef struct xl_logical_message
{
	Oid			dbId;			/* database Oid emitted from */
	bool		transactional;	/*  消息是否事务性?  */
	Size		prefix_size;	/*  前缀长度  */
	Size		message_size;	/*  消息大小  */
	/*  载荷,含长度为 prefix_size 的空终止前缀  */
	char		message[FLEXIBLE_ARRAY_MEMBER];
} xl_logical_message;

#define SizeOfLogicalMessage	(offsetof(xl_logical_message, message))

extern XLogRecPtr LogLogicalMessage(const char *prefix, const char *message,
									size_t size, bool transactional);

/* RMGR API */
#define XLOG_LOGICAL_MESSAGE	0x00
extern void logicalmsg_redo(XLogReaderState *record);
extern void logicalmsg_desc(StringInfo buf, XLogReaderState *record);
extern const char *logicalmsg_identify(uint8 info);

#endif							/* PG_LOGICAL_MESSAGE_H */
