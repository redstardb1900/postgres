/*-------------------------------------------------------------------------
 *
 * message.c
 *	  通用逻辑消息。    
 *
 * Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/message.c
 *
 * NOTES
 *
 * 通用逻辑消息允许把任意二进制数据块写入 XLOG 日志，  
 * 它们会被传递给逻辑解码插件。在正常的 XLOG 处理中    
 * 如同 NOOP。   
 *
 * 这些消息可以是事务性或者非事务性的。          
 * 事务性消息是当前事务的一部分，会以与 DML 操作         
 * 相同的方式发送给解码插件。               
 * 非事务性消息则会在逻辑解码从 XLOG 中                   
 * 读到它们时发送给插件。这也意味着，如果事务回滚， 
 * 事务性消息不会被送达，但非事务性消息却总是       
 * 会被送达。                                
 *
 * 每条消息都带有前缀，避免不同解码插件之间冲突。     
 * 插件作者必须格外小心地使用唯一的前缀，             
 * 例如，使用扩展的名称似乎是个不错的选择。          
 *
 * ---------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "utils/memutils.h"

/*
 * 将逻辑解码消息写入 XLog。      
 */
XLogRecPtr
LogLogicalMessage(const char *prefix, const char *message, size_t size,
				  bool transactional)
{
	xl_logical_message xlrec;

	/*
	 * 如果要发出事务性消息，则强制分配 xid。             
	 */
	if (transactional)
	{
		Assert(IsTransactionState());
		GetCurrentTransactionId();
	}

	xlrec.dbId = MyDatabaseId;
	xlrec.transactional = transactional;
	/* 末尾零至关重要；参见 logicalmsg_desc */
	xlrec.prefix_size = strlen(prefix) + 1;
	xlrec.message_size = size;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfLogicalMessage);
	XLogRegisterData(unconstify(char *, prefix), xlrec.prefix_size);
	XLogRegisterData(unconstify(char *, message), size);

	/* 允许按 origin 过滤*/
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	return XLogInsert(RM_LOGICALMSG_ID, XLOG_LOGICAL_MESSAGE);
}

/*
 * 重做对逻辑解码消息基本只是空操作。       
 */
void
logicalmsg_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info != XLOG_LOGICAL_MESSAGE)
		elog(PANIC, "logicalmsg_redo: unknown op code %u", info);

	/* 这只对逻辑解码有意义，参见 decode.c。          */
}
