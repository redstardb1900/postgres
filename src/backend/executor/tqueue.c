/*-------------------------------------------------------------------------
 *
 * tqueue.c
 *	  在并行后端之间用 shm_mq 发送和接收元组。     
 *
 * 类型为 DestTupleQueue 的 DestReceiver 在底层是一个        
 * TQueueDestReceiver，把执行器的元组写入 shm_mq。  
 *
 * TupleQueueReader 从 shm_mq 读取元组并将其返回。           
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/tqueue.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "executor/tqueue.h"

/*
 * DestReceiver 对象的私有内容    
 *
 * queue 指向 DestReceiver 调用方提供的数据。        
 */
typedef struct TQueueDestReceiver
{
	DestReceiver pub;			/* 公共字段  */
	shm_mq_handle *queue;		/* 写入的 shm_mq  */
} TQueueDestReceiver;

/*
 * TupleQueueReader 对象的私有内容    
 *
 * queue 指向读取方调用者提供的数据。       
 *
 * "typedef struct TupleQueueReader TupleQueueReader" 见 tqueue.h  
 */
struct TupleQueueReader
{
	shm_mq_handle *queue;		/* 读取的 shm_mq       */
};

/*
 * 从查询接收元组，并把它发送到指定的 shm_mq。      
 *
 * 成功则返回 true；若 shm_mq 已分离则返回 false。 
 */
static bool
tqueueReceiveSlot(TupleTableSlot *slot, DestReceiver *self)
{
	TQueueDestReceiver *tqueue = (TQueueDestReceiver *) self;
	MinimalTuple tuple;
	shm_mq_result result;
	bool		should_free;

	/* 发送元组本身。  */
	tuple = ExecFetchSlotMinimalTuple(slot, &should_free);
	result = shm_mq_send(tqueue->queue, tuple->t_len, tuple, false, false);

	if (should_free)
		pfree(tuple);

	/* 检查是否失败 */
	if (result == SHM_MQ_DETACHED)
		return false;
	else if (result != SHM_MQ_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not send tuple to shared-memory queue")));

	return true;
}

/*
 * 准备接收来自执行器的元组。 
 */
static void
tqueueStartupReceiver(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* 无操作  */
}

/*
 * 在执行器运行结束时清理 
 */
static void
tqueueShutdownReceiver(DestReceiver *self)
{
	TQueueDestReceiver *tqueue = (TQueueDestReceiver *) self;

	if (tqueue->queue != NULL)
		shm_mq_detach(tqueue->queue);
	tqueue->queue = NULL;
}

/*
 * 用完后销毁接收器          
 */
static void
tqueueDestroyReceiver(DestReceiver *self)
{
	TQueueDestReceiver *tqueue = (TQueueDestReceiver *) self;

	/* 我们大概已从队列分离，但再确认一次        */
	if (tqueue->queue != NULL)
		shm_mq_detach(tqueue->queue);
	pfree(self);
}

/*
 * 创建把元组写入元组队列的 DestReceiver。      
 */
DestReceiver *
CreateTupleQueueDestReceiver(shm_mq_handle *handle)
{
	TQueueDestReceiver *self;

	self = (TQueueDestReceiver *) palloc0(sizeof(TQueueDestReceiver));

	self->pub.receiveSlot = tqueueReceiveSlot;
	self->pub.rStartup = tqueueStartupReceiver;
	self->pub.rShutdown = tqueueShutdownReceiver;
	self->pub.rDestroy = tqueueDestroyReceiver;
	self->pub.mydest = DestTupleQueue;
	self->queue = handle;

	return (DestReceiver *) self;
}

/*
 * 创建元组队列读取器 
 */
TupleQueueReader *
CreateTupleQueueReader(shm_mq_handle *handle)
{
	TupleQueueReader *reader = palloc0(sizeof(TupleQueueReader));

	reader->queue = handle;

	return reader;
}

/*
 * 销毁元组队列读取器  
 *
 * 注意：清理底层的 shm_mq 是调用方的责任。               
 * 此处不会访问它，因为它可能已被分离。 
 */
void
DestroyTupleQueueReader(TupleQueueReader *reader)
{
	pfree(reader);
}

/*
 * 从元组队列读取器获取元组。 
 *
 * 若无剩余元组，或 nowait = true 且无元组可返回，
 * 返回值为 NULL。*done 若不为 NULL，则在没有剩余      
 * 元组时被置为 true，否则被置为 false。                       
 *
 * 返回的元组（如有）要么在共享内存中，要么在私有     
 * 缓冲区中，不应被释放。在下次调用 TupleQueueReaderNext()
 * 后即失效。        
 *
 * 即使 shm_mq_receive() 返回 SHM_MQ_WOULD_BLOCK，此处           
 * 仍会累计来自半读取消息的字节，因此以 nowait = true  
 * 调用此函数仍有用，即使未返回内容。 
 */
MinimalTuple
TupleQueueReaderNext(TupleQueueReader *reader, bool nowait, bool *done)
{
	MinimalTuple tuple;
	shm_mq_result result;
	Size		nbytes;
	void	   *data;

	if (done != NULL)
		*done = false;

	/* 尝试读取一条消息。*/
	result = shm_mq_receive(reader->queue, &nbytes, &data, nowait);

	/* 队列已分离时，置 *done 并返回 NULL。 */
	if (result == SHM_MQ_DETACHED)
	{
		if (done != NULL)
			*done = true;
		return NULL;
	}

	/* 非阻塞模式下，尚无消息就绪则退出。     */
	if (result == SHM_MQ_WOULD_BLOCK)
		return NULL;
	Assert(result == SHM_MQ_SUCCESS);

	/*
	 * 直接返回指向队列内存的指针（前提是该内存      
	 * 已充分对齐）。 
	 */
	tuple = (MinimalTuple) data;
	Assert(tuple->t_len == nbytes);

	return tuple;
}
