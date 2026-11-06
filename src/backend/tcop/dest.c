/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  通信目的地支持                 
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/dest.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 接口例程      
 *		BeginCommand - 在命令开始时初始化目的地          
 *		CreateDestReceiver - 为目的地创建元组接收器对象     
 *		EndCommand - 在命令结束时清理目的地         
 *		NullCommand - 告知目的地已识别出空查询串            
 *		ReadyForQuery - 告知目的地已准备好新查询       
 *
 *	 NOTES
 *		这些例程在查询返回元组的前后做相应的 
 *		工作，让后端与"目的地"门户保持同步。     
 *		                                   
 */

#include "postgres.h"

#include "access/printsimple.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/matview.h"
#include "executor/functions.h"
#include "executor/tqueue.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/portal.h"


/* ----------------
 *		dummy DestReceiver 函数   
 * ----------------
 */
static bool
donothingReceive(TupleTableSlot *slot, DestReceiver *self)
{
	return true;
}

static void
donothingStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

static void
donothingCleanup(DestReceiver *self)
{
	/* shutdown 与 destroy 两个方法都用它         */
}

/* ----------------
 *		无本地状态目的地类型的静态 DestReceiver 结构体   
 * ----------------
 */
static const DestReceiver donothingDR = {
	donothingReceive, donothingStartup, donothingCleanup, donothingCleanup,
	DestNone
};

static const DestReceiver debugtupDR = {
	debugtup, debugStartup, donothingCleanup, donothingCleanup,
	DestDebug
};

static const DestReceiver printsimpleDR = {
	printsimple, printsimple_startup, donothingCleanup, donothingCleanup,
	DestRemoteSimple
};

static const DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
	DestSPI
};

/*
 * DestNone 可用的全局接收器。     
 *
 * 把 const 限定去掉是安全的：对 none 接收器的任何修改     
 * 都会是 bug（这样更容易抓住它）。       
 */
DestReceiver *None_Receiver = (DestReceiver *) &donothingDR;

/* ----------------
 *		BeginCommand - 在命令开始时初始化目的地          
 * ----------------
 */
void
BeginCommand(CommandTag commandTag, CommandDest dest)
{
	/* 目前无事可做       */
}

/* ----------------
 *		CreateDestReceiver - 为目的地返回合适的接收器函数集    
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest)
{
	/*
	 * 把 const 限定去掉是安全的：none 接收器任何修改   
	 * 都会是 bug（这样更容易抓住它）。                
	 */

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			return printtup_create_DR(dest);

		case DestRemoteSimple:
			return unconstify(DestReceiver *, &printsimpleDR);

		case DestNone:
			return unconstify(DestReceiver *, &donothingDR);

		case DestDebug:
			return unconstify(DestReceiver *, &debugtupDR);

		case DestSPI:
			return unconstify(DestReceiver *, &spi_printtupDR);

		case DestTuplestore:
			return CreateTuplestoreDestReceiver();

		case DestIntoRel:
			return CreateIntoRelDestReceiver(NULL);

		case DestCopyOut:
			return CreateCopyDestReceiver();

		case DestSQLFunction:
			return CreateSQLFunctionDestReceiver();

		case DestTransientRel:
			return CreateTransientRelDestReceiver(InvalidOid);

		case DestTupleQueue:
			return CreateTupleQueueDestReceiver(NULL);
	}

	/* 不应到达此处    */
	pg_unreachable();
}

/* ----------------
 *		EndCommand - 在命令结束时清理目的地         
 * ----------------
 */
void
EndCommand(const QueryCompletion *qc, CommandDest dest, bool force_undecorated_output)
{
	char		completionTag[COMPLETION_TAG_BUFSIZE];
	Size		len;

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			len = BuildQueryCompletionString(completionTag, qc,
											 force_undecorated_output);
			pq_putmessage('C', completionTag, len + 1);

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
			break;
	}
}

/* ----------------
 *		EndReplicationCommand - EndCommand 的精简版本         
 *
 *		供复制命令使用。        
 * ----------------
 */
void
EndReplicationCommand(const char *commandTag)
{
	pq_putmessage('C', commandTag, strlen(commandTag) + 1);
}

/* ----------------
 *		NullCommand - 告知目的地已识别出空查询串            
 *
 *		这保证扩展查询协议中 Execute 消息的响应有一个   
 *		可识别的结束。                                
 * ----------------
 */
void
NullCommand(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			/* 告知前端我们见到了空查询串       */
			pq_putemptymessage('I');
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
			break;
	}
}

/* ----------------
 *		ReadyForQuery - 告知目的地已准备好新查询       
 *
 *		发送 ReadyForQuery 消息，让前端能分辨何时        
 *		处理完了一个查询串。        
 *		3.0 及以上版本中，它还携带事务状态指示。            
 *
 *		注意：在此冲刷 stdio 缓冲区后，其余大多数地方  
 *		都可免了这一步，从而减少单独发送的包数。          
 * ----------------
 */
void
ReadyForQuery(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:
			{
				StringInfoData buf;

				pq_beginmessage(&buf, 'Z');
				pq_sendbyte(&buf, TransactionBlockStatusCode());
				pq_endmessage(&buf);
			}
			/* 每轮结束时无论如何都冲刷输出*/
			pq_flush();
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
			break;
	}
}
