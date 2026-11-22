/*--------------------------------------------------------------------
 * execParallel.h
 *		POSTGRES parallel execution interface
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/executor/execParallel.h
 *--------------------------------------------------------------------
 */

#ifndef EXECPARALLEL_H
#define EXECPARALLEL_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "utils/dsa.h"

typedef struct SharedExecutorInstrumentation SharedExecutorInstrumentation;

typedef struct ParallelExecutorInfo
{
	PlanState  *planstate;		/*  我们并行运行的计划子树  */
	ParallelContext *pcxt;		/* parallel context we're using */
	BufferUsage *buffer_usage;	/*  指向 DSM 中 bufusage 区  */
	WalUsage   *wal_usage;		/* walusage area in DSM */
	SharedExecutorInstrumentation *instrumentation; /*  可选  */
	struct SharedJitInstrumentation *jit_instrumentation;	/*  可选  */
	dsa_area   *area;			/*  指向 DSM 中 DSA 区  */
	dsa_pointer param_exec;		/*  序列化的 PARAM_EXEC 参数  */
	bool		finished;		/* set true by ExecParallelFinish */
	/*  这两个数组有 pcxt->nworkers_launched 项:  */
	shm_mq_handle **tqueue;		/* tuple queues for worker output */
	struct TupleQueueReader **reader;	/*  元组读/写支持  */
} ParallelExecutorInfo;

extern ParallelExecutorInfo *ExecInitParallelPlan(PlanState *planstate,
												  EState *estate, Bitmapset *sendParams, int nworkers,
												  int64 tuples_needed);
extern void ExecParallelCreateReaders(ParallelExecutorInfo *pei);
extern void ExecParallelFinish(ParallelExecutorInfo *pei);
extern void ExecParallelCleanup(ParallelExecutorInfo *pei);
extern void ExecParallelReinitialize(PlanState *planstate,
									 ParallelExecutorInfo *pei, Bitmapset *sendParams);

extern void ParallelQueryMain(dsm_segment *seg, shm_toc *toc);

#endif							/* EXECPARALLEL_H */
