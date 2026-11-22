/*-------------------------------------------------------------------------
 *
 * tqueue.h
 *	  Use shm_mq to send & receive tuples between parallel backends
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/tqueue.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TQUEUE_H
#define TQUEUE_H

#include "storage/shm_mq.h"
#include "tcop/dest.h"

/* Opaque struct, only known inside tqueue.c. */
typedef struct TupleQueueReader TupleQueueReader;

/*  用此向 shm_mq 发送元组。  */
extern DestReceiver *CreateTupleQueueDestReceiver(shm_mq_handle *handle);

/*  用这些从 shm_mq 接收元组。  */
extern TupleQueueReader *CreateTupleQueueReader(shm_mq_handle *handle);
extern void DestroyTupleQueueReader(TupleQueueReader *reader);
extern MinimalTuple TupleQueueReaderNext(TupleQueueReader *reader,
										 bool nowait, bool *done);

#endif							/* TQUEUE_H */
