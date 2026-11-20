/*-------------------------------------------------------------------------
 *
 * shm_mq.h
 *	  单读者、单写者共享内存消息队列。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shm_mq.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHM_MQ_H
#define SHM_MQ_H

#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/proc.h"

/* 队列本身在共享内存。 */
struct shm_mq;
typedef struct shm_mq shm_mq;

/* 后端私有状态。 */
struct shm_mq_handle;
typedef struct shm_mq_handle shm_mq_handle;

/* 跨越多个位置的单次写入的描述符。 */
typedef struct
{
	const char *data;
	Size		len;
} shm_mq_iovec;

/* 发送或接收操作的可能结果。 */
typedef enum
{
	SHM_MQ_SUCCESS,				/* 已发送或接收消息。 */
	SHM_MQ_WOULD_BLOCK,			/* 未完成；稍后重试。 */
	SHM_MQ_DETACHED				/* 对方进程已脱离队列。 */
} shm_mq_result;

/*
 * 创建队列并设置发送者与接收者的原语。
 *
 * 读写任何消息前必须设置发送者与接收者，
 * 但不必由同一进程设置。每个必须设置
 * 恰好一次。
 */
extern shm_mq *shm_mq_create(void *address, Size size);
extern void shm_mq_set_receiver(shm_mq *mq, PGPROC *);
extern void shm_mq_set_sender(shm_mq *mq, PGPROC *);

/* 发送者与接收者的访问方法。 */
extern PGPROC *shm_mq_get_receiver(shm_mq *);
extern PGPROC *shm_mq_get_sender(shm_mq *);

/* 建立后端局部队列状态。 */
extern shm_mq_handle *shm_mq_attach(shm_mq *mq, dsm_segment *seg,
									BackgroundWorkerHandle *handle);

/* 关联 worker-handle 与 shm_mq。 */
extern void shm_mq_set_handle(shm_mq_handle *, BackgroundWorkerHandle *);

/* 断开连接，释放 handle 资源。 */
extern void shm_mq_detach(shm_mq_handle *mqh);

/* 从 handle 取 shm_mq。 */
extern shm_mq *shm_mq_get_queue(shm_mq_handle *mqh);

/* 发送或接收消息。 */
extern shm_mq_result shm_mq_send(shm_mq_handle *mqh,
								 Size nbytes, const void *data, bool nowait,
								 bool force_flush);
extern shm_mq_result shm_mq_sendv(shm_mq_handle *mqh, shm_mq_iovec *iov,
								  int iovcnt, bool nowait, bool force_flush);
extern shm_mq_result shm_mq_receive(shm_mq_handle *mqh,
									Size *nbytesp, void **datap, bool nowait);

/* 等待对方附着到队列。 */
extern shm_mq_result shm_mq_wait_for_attach(shm_mq_handle *mqh);

/* 可能的最小队列。 */
extern PGDLLIMPORT const Size shm_mq_minimum_size;

#endif							/* SHM_MQ_H */
