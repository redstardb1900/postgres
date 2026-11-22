/*-------------------------------------------------------------------------
 *
 * worker_internal.h
 *	  Internal headers shared by logical replication workers.
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * src/include/replication/worker_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WORKER_INTERNAL_H
#define WORKER_INTERNAL_H

#include <signal.h>

#include "access/xlogdefs.h"
#include "catalog/pg_subscription.h"
#include "datatype/timestamp.h"
#include "miscadmin.h"
#include "replication/logicalrelation.h"
#include "storage/buffile.h"
#include "storage/fileset.h"
#include "storage/lock.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"


typedef struct LogicalRepWorker
{
	/*  本工作进程启动时刻。  */
	TimestampTz launch_time;

	/*  指示本槽已用或空闲。  */
	bool		in_use;

	/*  槽被新工作进程占用时递增。  */
	uint16		generation;

	/*  指向进程数组.未运行则为 NULL.  */
	PGPROC	   *proc;

	/* 要连接的数据库 id. */
	Oid			dbid;

	/*  连接所用用户(与订阅所有者相同)。  */
	Oid			userid;

	/* 本工作进程的订阅 id。 */
	Oid			subid;

	/*  用于初始表同步。  */
	Oid			relid;
	char		relstate;
	XLogRecPtr	relstate_lsn;
	slock_t		relmutex;

	/*
	 * Used to create the changes and subxact files for the streaming
	 * transactions.  Upon the arrival of the first streaming transaction or
	 * when the first-time leader apply worker times out while sending changes
	 * to the parallel apply worker, the fileset will be initialized, and it
	 * will be deleted when the worker exits.  Under this, separate buffiles
	 * would be created for each transaction which will be deleted after the
	 * transaction is finished.
	 */
	FileSet    *stream_fileset;

	/*
	 * 若本槽用于并行应用则为主应用工作进程 PID,
	 * 否则为 InvalidPid。
	 */
	pid_t		leader_pid;

	/*  指示是否可并行执行应用。  */
	bool		parallel_apply;

	/* Stats. */
	XLogRecPtr	last_lsn;
	TimestampTz last_send_time;
	TimestampTz last_recv_time;
	XLogRecPtr	reply_lsn;
	TimestampTz reply_time;
} LogicalRepWorker;

/*
 * State of the transaction in parallel apply worker.
 *
 * The enum values must have the same order as the transaction state
 * transitions.
 */
typedef enum ParallelTransState
{
	PARALLEL_TRANS_UNKNOWN,
	PARALLEL_TRANS_STARTED,
	PARALLEL_TRANS_FINISHED
} ParallelTransState;

/*
 * State of fileset used to communicate changes from leader to parallel
 * apply worker.
 *
 * FS_EMPTY indicates an initial state where the leader doesn't need to use
 * the file to communicate with the parallel apply worker.
 *
 * FS_SERIALIZE_IN_PROGRESS indicates that the leader is serializing changes
 * to the file.
 *
 * FS_SERIALIZE_DONE indicates that the leader has serialized all changes to
 * the file.
 *
 * FS_READY indicates that it is now ok for a parallel apply worker to
 * read the file.
 */
typedef enum PartialFileSetState
{
	FS_EMPTY,
	FS_SERIALIZE_IN_PROGRESS,
	FS_SERIALIZE_DONE,
	FS_READY
} PartialFileSetState;

/*
 * Struct for sharing information between leader apply worker and parallel
 * apply workers.
 */
typedef struct ParallelApplyWorkerShared
{
	slock_t		mutex;

	TransactionId xid;

	/*
	 * State used to ensure commit ordering.
	 *
	 * The parallel apply worker will set it to PARALLEL_TRANS_FINISHED after
	 * handling the transaction finish commands while the apply leader will
	 * wait for it to become PARALLEL_TRANS_FINISHED before proceeding in
	 * transaction finish commands (e.g. STREAM_COMMIT/STREAM_PREPARE/
	 * STREAM_ABORT).
	 */
	ParallelTransState xact_state;

	/*  来自对应 LogicalRepWorker 槽的信息。  */
	uint16		logicalrep_worker_generation;
	int			logicalrep_worker_slot_no;

	/*
	 * Indicates whether there are pending streaming blocks in the queue. The
	 * parallel apply worker will check it before starting to wait.
	 */
	pg_atomic_uint32 pending_stream_count;

	/*
	 * XactLastCommitEnd from the parallel apply worker. This is required by
	 * the leader worker so it can update the lsn_mappings.
	 */
	XLogRecPtr	last_commit_end;

	/*
	 * After entering PARTIAL_SERIALIZE mode, the leader apply worker will
	 * serialize changes to the file, and share the fileset with the parallel
	 * apply worker when processing the transaction finish command. Then the
	 * parallel apply worker will apply all the spooled messages.
	 *
	 * FileSet is used here instead of SharedFileSet because we need it to
	 * survive after releasing the shared memory so that the leader apply
	 * worker can re-use the same fileset for the next streaming transaction.
	 */
	PartialFileSetState fileset_state;
	FileSet		fileset;
} ParallelApplyWorkerShared;

/*
 * 用于管理并行应用工作进程的信息。
 */
typedef struct ParallelApplyWorkerInfo
{
	/*
	 * This queue is used to send changes from the leader apply worker to the
	 * parallel apply worker.
	 */
	shm_mq_handle *mq_handle;

	/*
	 * This queue is used to transfer error messages from the parallel apply
	 * worker to the leader apply worker.
	 */
	shm_mq_handle *error_mq_handle;

	dsm_segment *dsm_seg;

	/*
	 * Indicates whether the leader apply worker needs to serialize the
	 * remaining changes to a file due to timeout when attempting to send data
	 * to the parallel apply worker via shared memory.
	 */
	bool		serialize_changes;

	/*
	 * 若工作进程正用于处理并行应用
	 * 事务则为真。假表示可复用。
	 */
	bool		in_use;

	ParallelApplyWorkerShared *shared;
} ParallelApplyWorkerInfo;

/*  应用工作进程主内存上下文.工作进程存活期间常驻.  */
extern PGDLLIMPORT MemoryContext ApplyContext;

extern PGDLLIMPORT MemoryContext ApplyMessageContext;

extern PGDLLIMPORT ErrorContextCallback *apply_error_context_stack;

extern PGDLLIMPORT ParallelApplyWorkerShared *MyParallelShared;

/*  libpqreceiver 连接  */
extern PGDLLIMPORT struct WalReceiverConn *LogRepWorkerWalRcvConn;

/*  工作进程与订阅对象。  */
extern PGDLLIMPORT Subscription *MySubscription;
extern PGDLLIMPORT LogicalRepWorker *MyLogicalRepWorker;

extern PGDLLIMPORT bool in_remote_transaction;

extern PGDLLIMPORT bool InitializingApplyWorker;

extern void logicalrep_worker_attach(int slot);
extern LogicalRepWorker *logicalrep_worker_find(Oid subid, Oid relid,
												bool only_running);
extern List *logicalrep_workers_find(Oid subid, bool only_running);
extern bool logicalrep_worker_launch(Oid dbid, Oid subid, const char *subname,
									 Oid userid, Oid relid,
									 dsm_handle subworker_dsm);
extern void logicalrep_worker_stop(Oid subid, Oid relid);
extern void logicalrep_pa_worker_stop(ParallelApplyWorkerInfo *winfo);
extern void logicalrep_worker_wakeup(Oid subid, Oid relid);
extern void logicalrep_worker_wakeup_ptr(LogicalRepWorker *worker);

extern int	logicalrep_sync_worker_count(Oid subid);

extern void ReplicationOriginNameForLogicalRep(Oid suboid, Oid relid,
											   char *originname, Size szoriginname);
extern char *LogicalRepSyncTableStart(XLogRecPtr *origin_startpos);

extern bool AllTablesyncsReady(void);
extern void UpdateTwoPhaseState(Oid suboid, char new_state);

extern void process_syncing_tables(XLogRecPtr current_lsn);
extern void invalidate_syncing_table_states(Datum arg, int cacheid,
											uint32 hashvalue);

extern void stream_start_internal(TransactionId xid, bool first_segment);
extern void stream_stop_internal(TransactionId xid);

/*  应用全部假脱机消息的公共流式函数  */
extern void apply_spooled_messages(FileSet *stream_fileset, TransactionId xid,
								   XLogRecPtr lsn);

extern void apply_dispatch(StringInfo s);

extern void maybe_reread_subscription(void);

extern void stream_cleanup_files(Oid subid, TransactionId xid);

extern void InitializeApplyWorker(void);

extern void store_flush_position(XLogRecPtr remote_lsn, XLogRecPtr local_lsn);

/*  应用错误回调函数  */
extern void apply_error_callback(void *arg);
extern void set_apply_error_context_origin(char *originname);

/*  并行应用工作进程的设置与交互  */
extern void pa_allocate_worker(TransactionId xid);
extern ParallelApplyWorkerInfo *pa_find_worker(TransactionId xid);
extern void pa_detach_all_error_mq(void);

extern bool pa_send_data(ParallelApplyWorkerInfo *winfo, Size nbytes,
						 const void *data);
extern void pa_switch_to_partial_serialize(ParallelApplyWorkerInfo *winfo,
										   bool stream_locked);

extern void pa_set_xact_state(ParallelApplyWorkerShared *wshared,
							  ParallelTransState xact_state);
extern void pa_set_stream_apply_worker(ParallelApplyWorkerInfo *winfo);

extern void pa_start_subtrans(TransactionId current_xid,
							  TransactionId top_xid);
extern void pa_reset_subtrans(void);
extern void pa_stream_abort(LogicalRepStreamAbortData *abort_data);
extern void pa_set_fileset_state(ParallelApplyWorkerShared *wshared,
								 PartialFileSetState fileset_state);

extern void pa_lock_stream(TransactionId xid, LOCKMODE lockmode);
extern void pa_unlock_stream(TransactionId xid, LOCKMODE lockmode);

extern void pa_lock_transaction(TransactionId xid, LOCKMODE lockmode);
extern void pa_unlock_transaction(TransactionId xid, LOCKMODE lockmode);

extern void pa_decr_and_wait_stream_block(void);

extern void pa_xact_finish(ParallelApplyWorkerInfo *winfo,
						   XLogRecPtr remote_lsn);

#define isParallelApplyWorker(worker) ((worker)->leader_pid != InvalidPid)

static inline bool
am_tablesync_worker(void)
{
	return OidIsValid(MyLogicalRepWorker->relid);
}

static inline bool
am_leader_apply_worker(void)
{
	return (!am_tablesync_worker() &&
			!isParallelApplyWorker(MyLogicalRepWorker));
}

static inline bool
am_parallel_apply_worker(void)
{
	return isParallelApplyWorker(MyLogicalRepWorker);
}

#endif							/* WORKER_INTERNAL_H */
