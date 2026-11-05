/*-------------------------------------------------------------------------
 *
 * execAsync.c
 *	  异步执行的支撑例程。             
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execAsync.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execAsync.h"
#include "executor/executor.h"
#include "executor/nodeAppend.h"
#include "executor/nodeForeignscan.h"

/*
 * 以异步方式向指定的支持异步的节点请求元组。   
 */
void
ExecAsyncRequest(AsyncRequest *areq)
{
	if (areq->requestee->chgParam != NULL)	/* 有变化吗？    */
		ExecReScan(areq->requestee);	/* 交给 ReScan 处理   */

	/* 必须提供我们自己的统计信息支持*/
	if (areq->requestee->instrument)
		InstrStartNode(areq->requestee->instrument);

	switch (nodeTag(areq->requestee))
	{
		case T_ForeignScanState:
			ExecAsyncForeignScanRequest(areq);
			break;
		default:
			/* 若节点不支持异步，则调用方出了错。  */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestee));
	}

	ExecAsyncResponse(areq);

	/* 必须提供我们自己的统计信息支持*/
	if (areq->requestee->instrument)
		InstrStopNode(areq->requestee->instrument,
					  TupIsNull(areq->result) ? 0.0 : 1.0);
}

/*
 * 让异步节点有机会配置它想等待的文件描述符事件，     
 * 我们期望节点类型特定的回调函数按如下形式              
 * 进行单次调用：                    
 *
 * AddWaitEventToSet(set, WL_SOCKET_READABLE, fd, NULL, areq);
 */
void
ExecAsyncConfigureWait(AsyncRequest *areq)
{
	/* 必须提供我们自己的统计信息支持*/
	if (areq->requestee->instrument)
		InstrStartNode(areq->requestee->instrument);

	switch (nodeTag(areq->requestee))
	{
		case T_ForeignScanState:
			ExecAsyncForeignScanConfigureWait(areq);
			break;
		default:
			/* 若节点不支持异步，则调用方出了错。  */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestee));
	}

	/* 必须提供我们自己的统计信息支持*/
	if (areq->requestee->instrument)
		InstrStopNode(areq->requestee->instrument, 0.0);
}

/*
 * 当相关事件发生时，回调该异步节点。                
 */
void
ExecAsyncNotify(AsyncRequest *areq)
{
	/* 必须提供我们自己的统计信息支持*/
	if (areq->requestee->instrument)
		InstrStartNode(areq->requestee->instrument);

	switch (nodeTag(areq->requestee))
	{
		case T_ForeignScanState:
			ExecAsyncForeignScanNotify(areq);
			break;
		default:
			/* 若节点不支持异步，则调用方出了错。  */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestee));
	}

	ExecAsyncResponse(areq);

	/* 必须提供我们自己的统计信息支持*/
	if (areq->requestee->instrument)
		InstrStopNode(areq->requestee->instrument,
					  TupIsNull(areq->result) ? 0.0 : 1.0);
}

/*
 * 当异步节点产生了结果时，回调请求方节点。            
 */
void
ExecAsyncResponse(AsyncRequest *areq)
{
	switch (nodeTag(areq->requestor))
	{
		case T_AppendState:
			ExecAsyncAppendResponse(areq);
			break;
		default:
			/* 若节点不支持异步，则调用方出了错。  */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(areq->requestor));
	}
}

/*
 * 被请求节点应当调用此函数，把元组交付给它的       
 * 请求方节点。被请求节点可以在其 ExecAsyncRequest 或         
 * ExecAsyncNotify 回调。   
 */
void
ExecAsyncRequestDone(AsyncRequest *areq, TupleTableSlot *result)
{
	areq->request_complete = true;
	areq->result = result;
}

/*
 * 被请求节点应调用此函数表明自己正在等待回调。       
 * 被请求节点可以在其 ExecAsyncRequest 或                           
 * ExecAsyncNotify 回调。   
 */
void
ExecAsyncRequestPending(AsyncRequest *areq)
{
	areq->callback_pending = true;
	areq->request_complete = false;
	areq->result = NULL;
}
