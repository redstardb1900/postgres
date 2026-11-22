/*
 * reorderbuffer.h
 *	  PostgreSQL logical replay/reorder buffer management.
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * src/include/replication/reorderbuffer.h
 */
#ifndef REORDERBUFFER_H
#define REORDERBUFFER_H

#include "access/htup_details.h"
#include "lib/ilist.h"
#include "storage/sinval.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
#include "utils/timestamp.h"

/*  GUC 变量  */
extern PGDLLIMPORT int logical_decoding_work_mem;
extern PGDLLIMPORT int debug_logical_replication_streaming;

/*  debug_logical_replication_streaming 的可能取值  */
typedef enum
{
	DEBUG_LOGICAL_REP_STREAMING_BUFFERED,
	DEBUG_LOGICAL_REP_STREAMING_IMMEDIATE
}			DebugLogicalRepStreamingMode;

/*  单个元组,存于一块内存  */
typedef struct ReorderBufferTupleBuf
{
	/*  预分配列表中的位置  */
	slist_node	node;

	/*  元组头,逻辑解码用户关心的部分  */
	HeapTupleData tuple;

	/*  元组缓冲预分配大小,不同于元组大小  */
	Size		alloc_tuple_size;

	/* actual tuple data follows */
} ReorderBufferTupleBuf;

/* pointer to the data stored in a TupleBuf */
#define ReorderBufferTupleBufData(p) \
	((HeapTupleHeader) MAXALIGN(((char *) p) + sizeof(ReorderBufferTupleBuf)))

/*
 * Types of the change passed to a 'change' callback.
 *
 * For efficiency and simplicity reasons we want to keep Snapshots, CommandIds
 * and ComboCids in the same list with the user visible INSERT/UPDATE/DELETE
 * changes. Users of the decoding facilities will never see changes with
 * *_INTERNAL_* actions.
 *
 * The INTERNAL_SPEC_INSERT and INTERNAL_SPEC_CONFIRM, and INTERNAL_SPEC_ABORT
 * changes concern "speculative insertions", their confirmation, and abort
 * respectively.  They're used by INSERT .. ON CONFLICT .. UPDATE.  Users of
 * logical decoding don't have to care about these.
 */
typedef enum ReorderBufferChangeType
{
	REORDER_BUFFER_CHANGE_INSERT,
	REORDER_BUFFER_CHANGE_UPDATE,
	REORDER_BUFFER_CHANGE_DELETE,
	REORDER_BUFFER_CHANGE_MESSAGE,
	REORDER_BUFFER_CHANGE_INVALIDATION,
	REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT,
	REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID,
	REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID,
	REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT,
	REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM,
	REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT,
	REORDER_BUFFER_CHANGE_TRUNCATE
} ReorderBufferChangeType;

/*  前向声明  */
struct ReorderBufferTXN;

/*
 * a single 'change', can be an insert (with one tuple), an update (old, new),
 * or a delete (old).
 *
 * The same struct is also used internally for other purposes but that should
 * never be visible outside reorderbuffer.c.
 */
typedef struct ReorderBufferChange
{
	XLogRecPtr	lsn;

	/*  变更类型。  */
	ReorderBufferChangeType action;

	/*  本变更所属事务。  */
	struct ReorderBufferTXN *txn;

	RepOriginId origin_id;

	/*
	 * Context data for the change. Which part of the union is valid depends
	 * on action.
	 */
	union
	{
		/* Old, new tuples when action == *_INSERT|UPDATE|DELETE */
		struct
		{
			/*  已变更的关系  */
			RelFileLocator rlocator;

			/*  此前重组的 toast 块已不再需要  */
			bool		clear_toast_afterwards;

			/* valid for DELETE || UPDATE */
			ReorderBufferTupleBuf *oldtuple;
			/* valid for INSERT || UPDATE */
			ReorderBufferTupleBuf *newtuple;
		}			tp;

		/*
		 * Truncate data for REORDER_BUFFER_CHANGE_TRUNCATE representing one
		 * set of relations to be truncated.
		 */
		struct
		{
			Size		nrelids;
			bool		cascade;
			bool		restart_seqs;
			Oid		   *relids;
		}			truncate;

		/*  任意数据的消息。  */
		struct
		{
			char	   *prefix;
			Size		message_size;
			char	   *message;
		}			msg;

		/*  新快照,当 action == *_INTERNAL_SNAPSHOT 时设  */
		Snapshot	snapshot;

		/*
		 * New command id for existing snapshot in a catalog changing tx. Set
		 * when action == *_INTERNAL_COMMAND_ID.
		 */
		CommandId	command_id;

		/*
		 * New cid mapping for catalog changing transaction, set when action
		 * == *_INTERNAL_TUPLECID.
		 */
		struct
		{
			RelFileLocator locator;
			ItemPointerData tid;
			CommandId	cmin;
			CommandId	cmax;
			CommandId	combocid;
		}			tuplecid;

		/*  失效。  */
		struct
		{
			uint32		ninvalidations; /*  消息数  */
			SharedInvalidationMessage *invalidations;	/*  失效消息  */
		}			inval;
	}			data;

	/*
	 * While in use this is how a change is linked into a transactions,
	 * otherwise it's the preallocated list.
	 */
	dlist_node	node;
} ReorderBufferChange;

/* ReorderBufferTXN txn_flags */
#define RBTXN_HAS_CATALOG_CHANGES 		0x0001
#define RBTXN_IS_SUBXACT          		0x0002
#define RBTXN_IS_SERIALIZED       		0x0004
#define RBTXN_IS_SERIALIZED_CLEAR 		0x0008
#define RBTXN_IS_STREAMED         		0x0010
#define RBTXN_HAS_PARTIAL_CHANGE  		0x0020
#define RBTXN_PREPARE             		0x0040
#define RBTXN_SKIPPED_PREPARE	  		0x0080
#define RBTXN_HAS_STREAMABLE_CHANGE		0x0100
#define RBTXN_DISTR_INVAL_OVERFLOWED	0x0200

/*  事务是否有目录变更?  */
#define rbtxn_has_catalog_changes(txn) \
( \
	 ((txn)->txn_flags & RBTXN_HAS_CATALOG_CHANGES) != 0 \
)

/*  事务是否已知为子事务?  */
#define rbtxn_is_known_subxact(txn) \
( \
	((txn)->txn_flags & RBTXN_IS_SUBXACT) != 0 \
)

/*  本事务是否已溢写到磁盘?  */
#define rbtxn_is_serialized(txn) \
( \
	((txn)->txn_flags & RBTXN_IS_SERIALIZED) != 0 \
)

/*  本事务是否曾溢写到磁盘?  */
#define rbtxn_is_serialized_clear(txn) \
( \
	((txn)->txn_flags & RBTXN_IS_SERIALIZED_CLEAR) != 0 \
)

/*  本事务是否含部分变更?  */
#define rbtxn_has_partial_change(txn) \
( \
	((txn)->txn_flags & RBTXN_HAS_PARTIAL_CHANGE) != 0 \
)

/*  本事务是否含可流式变更?  */
#define rbtxn_has_streamable_change(txn) \
( \
	((txn)->txn_flags & RBTXN_HAS_STREAMABLE_CHANGE) != 0 \
)

/*
 * Has this transaction been streamed to downstream?
 *
 * (It's not possible to deduce this from nentries and nentries_mem for
 * various reasons. For example, all changes may be in subtransactions in
 * which case we'd have nentries==0 for the toplevel one, which would say
 * nothing about the streaming. So we maintain this flag, but only for the
 * toplevel transaction.)
 */
#define rbtxn_is_streamed(txn) \
( \
	((txn)->txn_flags & RBTXN_IS_STREAMED) != 0 \
)

/*  本事务是否已预备?  */
#define rbtxn_prepared(txn) \
( \
	((txn)->txn_flags & RBTXN_PREPARE) != 0 \
)

/*  是否跳过了本事务的预备?  */
#define rbtxn_skip_prepared(txn) \
( \
	((txn)->txn_flags & RBTXN_SKIPPED_PREPARE) != 0 \
)

/*  分布式失效消息数组是否溢出?  */
#define rbtxn_distr_inval_overflowed(txn) \
( \
	((txn)->txn_flags & RBTXN_DISTR_INVAL_OVERFLOWED) != 0 \
)

/*  是否为顶层事务?  */
#define rbtxn_is_toptxn(txn) \
( \
	(txn)->toptxn == NULL \
)

/*  是否为子事务?  */
#define rbtxn_is_subtxn(txn) \
( \
	(txn)->toptxn != NULL \
)

/*  取本(子)事务的顶层事务。  */
#define rbtxn_get_toptxn(txn) \
( \
	rbtxn_is_subtxn(txn) ? (txn)->toptxn : (txn) \
)

typedef struct ReorderBufferTXN
{
	/*  见上  */
	bits32		txn_flags;

	/*  事务的 xid,可为顶层或子 xid。  */
	TransactionId xid;

	/*  顶层事务 xid(若已知)  */
	TransactionId toplevel_xid;

	/*
	 * Global transaction id required for identification of prepared
	 * transactions.
	 */
	char	   *gid;

	/*
	 * LSN of the first data carrying, WAL record with knowledge about this
	 * xid. This is allowed to *not* be first record adorned with this xid, if
	 * the previous records aren't relevant for logical decoding.
	 */
	XLogRecPtr	first_lsn;

	/* ----
	 * LSN of the record that lead to this xact to be prepared or committed or
	 * aborted. This can be a
	 * * plain commit record
	 * * plain commit record, of a parent transaction
	 * * prepared transaction
	 * * prepared transaction commit
	 * * plain abort record
	 * * prepared transaction abort
	 *
	 * This can also become set to earlier values than transaction end when
	 * a transaction is spilled to disk; specifically it's set to the LSN of
	 * the latest change written to disk so far.
	 * ----
	 */
	XLogRecPtr	final_lsn;

	/*
	 * 指向提交记录末尾 + 1 的 LSN。
	 */
	XLogRecPtr	end_lsn;

	/*  本子事务的顶层事务(顶层则为 NULL)。  */
	struct ReorderBufferTXN *toptxn;

	/*
	 * LSN of the last lsn at which snapshot information reside, so we can
	 * restart decoding from there and fully recover this transaction from
	 * WAL.
	 */
	XLogRecPtr	restart_decoding_lsn;

	/*  导致本事务的变更之源  */
	RepOriginId origin_id;
	XLogRecPtr	origin_lsn;

	/*
	 * Commit or Prepare time, only known when we read the actual commit or
	 * prepare record.
	 */
	union
	{
		TimestampTz commit_time;
		TimestampTz prepare_time;
		TimestampTz abort_time;
	}			xact_time;

	/*
	 * The base snapshot is used to decode all changes until either this
	 * transaction modifies the catalog, or another catalog-modifying
	 * transaction commits.
	 */
	Snapshot	base_snapshot;
	XLogRecPtr	base_snapshot_lsn;
	dlist_node	base_snapshot_node; /* link in txns_by_base_snapshot_lsn */

	/*
	 * Snapshot/CID from the previous streaming run. Only valid for already
	 * streamed transactions (NULL/InvalidCommandId otherwise).
	 */
	Snapshot	snapshot_now;
	CommandId	command_id;

	/*
	 * How many ReorderBufferChange's do we have in this txn.
	 *
	 * Changes in subtransactions are *not* included but tracked separately.
	 */
	uint64		nentries;

	/*
	 * How many of the above entries are stored in memory in contrast to being
	 * spilled to disk.
	 */
	uint64		nentries_mem;

	/*
	 * List of ReorderBufferChange structs, including new Snapshots, new
	 * CommandIds and command invalidation messages.
	 */
	dlist_head	changes;

	/*
	 * List of (relation, ctid) => (cmin, cmax) mappings for catalog tuples.
	 * Those are always assigned to the toplevel transaction. (Keep track of
	 * #entries to create a hash of the right size)
	 */
	dlist_head	tuplecids;
	uint64		ntuplecids;

	/*
	 * 按需构建的哈希,用于查找上述值。
	 */
	HTAB	   *tuplecid_hash;

	/*
	 * Hash containing (potentially partial) toast entries. NULL if no toast
	 * tuples have been found for the current change.
	 */
	HTAB	   *toast_hash;

	/*
	 * non-hierarchical list of subtransactions that are *not* aborted. Only
	 * used in toplevel transactions.
	 */
	dlist_head	subtxns;
	uint32		nsubtxns;

	/*
	 * Stored cache invalidations. This is not a linked list because we get
	 * all the invalidations at once.
	 */
	uint32		ninvalidations;
	SharedInvalidationMessage *invalidations;

	/* ---
	 * Position in one of three lists:
	 * * list of subtransactions if we are *known* to be subxact
	 * * list of toplevel xacts (can be an as-yet unknown subxact)
	 * * list of preallocated ReorderBufferTXNs (if unused)
	 * ---
	 */
	dlist_node	node;

	/*
	 * 目录修改事务列表中的节点
	 */
	dlist_node	catchange_node;

	/*
	 * Size of this transaction (changes currently in memory, in bytes).
	 */
	Size		size;

	/*  含全部子事务的顶层事务大小。  */
	Size		total_size;

	/*  若已检测到并发中止则忽略后续变更。  */
	bool		concurrent_abort;

	/*
	 * 输出插件的私有数据指针。
	 */
	void	   *output_plugin_private;

	/*
	 * Stores cache invalidation messages distributed by other transactions.
	 */
	uint32		ninvalidations_distributed;
	SharedInvalidationMessage *invalidations_distributed;
} ReorderBufferTXN;

/* so we can define the callbacks used inside struct ReorderBuffer itself */
typedef struct ReorderBuffer ReorderBuffer;

/*  变更回调签名  */
typedef void (*ReorderBufferApplyChangeCB) (ReorderBuffer *rb,
											ReorderBufferTXN *txn,
											Relation relation,
											ReorderBufferChange *change);

/*  截断回调签名  */
typedef void (*ReorderBufferApplyTruncateCB) (ReorderBuffer *rb,
											  ReorderBufferTXN *txn,
											  int nrelations,
											  Relation relations[],
											  ReorderBufferChange *change);

/*  开始回调签名  */
typedef void (*ReorderBufferBeginCB) (ReorderBuffer *rb,
									  ReorderBufferTXN *txn);

/*  提交回调签名  */
typedef void (*ReorderBufferCommitCB) (ReorderBuffer *rb,
									   ReorderBufferTXN *txn,
									   XLogRecPtr commit_lsn);

/*  消息回调签名  */
typedef void (*ReorderBufferMessageCB) (ReorderBuffer *rb,
										ReorderBufferTXN *txn,
										XLogRecPtr message_lsn,
										bool transactional,
										const char *prefix, Size sz,
										const char *message);

/*  开始预备回调签名  */
typedef void (*ReorderBufferBeginPrepareCB) (ReorderBuffer *rb,
											 ReorderBufferTXN *txn);

/*  预备回调签名  */
typedef void (*ReorderBufferPrepareCB) (ReorderBuffer *rb,
										ReorderBufferTXN *txn,
										XLogRecPtr prepare_lsn);

/*  提交已预备回调签名  */
typedef void (*ReorderBufferCommitPreparedCB) (ReorderBuffer *rb,
											   ReorderBufferTXN *txn,
											   XLogRecPtr commit_lsn);

/*  回滚已预备回调签名  */
typedef void (*ReorderBufferRollbackPreparedCB) (ReorderBuffer *rb,
												 ReorderBufferTXN *txn,
												 XLogRecPtr prepare_end_lsn,
												 TimestampTz prepare_time);

/*  开始流式事务回调签名  */
typedef void (*ReorderBufferStreamStartCB) (
											ReorderBuffer *rb,
											ReorderBufferTXN *txn,
											XLogRecPtr first_lsn);

/*  停止流式事务回调签名  */
typedef void (*ReorderBufferStreamStopCB) (
										   ReorderBuffer *rb,
										   ReorderBufferTXN *txn,
										   XLogRecPtr last_lsn);

/*  丢弃已流式事务回调签名  */
typedef void (*ReorderBufferStreamAbortCB) (
											ReorderBuffer *rb,
											ReorderBufferTXN *txn,
											XLogRecPtr abort_lsn);

/*  预备已流式事务回调签名  */
typedef void (*ReorderBufferStreamPrepareCB) (
											  ReorderBuffer *rb,
											  ReorderBufferTXN *txn,
											  XLogRecPtr prepare_lsn);

/*  提交已流式事务回调签名  */
typedef void (*ReorderBufferStreamCommitCB) (
											 ReorderBuffer *rb,
											 ReorderBufferTXN *txn,
											 XLogRecPtr commit_lsn);

/*  流式变更回调签名  */
typedef void (*ReorderBufferStreamChangeCB) (
											 ReorderBuffer *rb,
											 ReorderBufferTXN *txn,
											 Relation relation,
											 ReorderBufferChange *change);

/*  流式消息回调签名  */
typedef void (*ReorderBufferStreamMessageCB) (
											  ReorderBuffer *rb,
											  ReorderBufferTXN *txn,
											  XLogRecPtr message_lsn,
											  bool transactional,
											  const char *prefix, Size sz,
											  const char *message);

/*  流式截断回调签名  */
typedef void (*ReorderBufferStreamTruncateCB) (
											   ReorderBuffer *rb,
											   ReorderBufferTXN *txn,
											   int nrelations,
											   Relation relations[],
											   ReorderBufferChange *change);

/*  更新事务进度回调签名  */
typedef void (*ReorderBufferUpdateProgressTxnCB) (
												  ReorderBuffer *rb,
												  ReorderBufferTXN *txn,
												  XLogRecPtr lsn);

struct ReorderBuffer
{
	/*
	 * xid => ReorderBufferTXN 查找表
	 */
	HTAB	   *by_txn;

	/*
	 * Transactions that could be a toplevel xact, ordered by LSN of the first
	 * record bearing that xid.
	 */
	dlist_head	toplevel_by_lsn;

	/*
	 * Transactions and subtransactions that have a base snapshot, ordered by
	 * LSN of the record which caused us to first obtain the base snapshot.
	 * This is not the same as toplevel_by_lsn, because we only set the base
	 * snapshot on the first logical-decoding-relevant record (eg. heap
	 * writes), whereas the initial LSN could be set by other operations.
	 */
	dlist_head	txns_by_base_snapshot_lsn;

	/*
	 * Transactions and subtransactions that have modified system catalogs.
	 */
	dclist_head catchange_txns;

	/*
	 * one-entry sized cache for by_txn. Very frequently the same txn gets
	 * looked up over and over again.
	 */
	TransactionId by_txn_last_xid;
	ReorderBufferTXN *by_txn_last_txn;

	/*
	 * 事务提交时调用的回调。
	 */
	ReorderBufferBeginCB begin;
	ReorderBufferApplyChangeCB apply_change;
	ReorderBufferApplyTruncateCB apply_truncate;
	ReorderBufferCommitCB commit;
	ReorderBufferMessageCB message;

	/*
	 * Callbacks to be called when streaming a transaction at prepare time.
	 */
	ReorderBufferBeginCB begin_prepare;
	ReorderBufferPrepareCB prepare;
	ReorderBufferCommitPreparedCB commit_prepared;
	ReorderBufferRollbackPreparedCB rollback_prepared;

	/*
	 * 流式事务时调用的回调。
	 */
	ReorderBufferStreamStartCB stream_start;
	ReorderBufferStreamStopCB stream_stop;
	ReorderBufferStreamAbortCB stream_abort;
	ReorderBufferStreamPrepareCB stream_prepare;
	ReorderBufferStreamCommitCB stream_commit;
	ReorderBufferStreamChangeCB stream_change;
	ReorderBufferStreamMessageCB stream_message;
	ReorderBufferStreamTruncateCB stream_truncate;

	/*
	 * Callback to be called when updating progress during sending data of a
	 * transaction (and its subtransactions) to the output plugin.
	 */
	ReorderBufferUpdateProgressTxnCB update_progress_txn;

	/*
	 * 原样传给回调的指针。
	 */
	void	   *private_data;

	/*
	 * Saved output plugin option
	 */
	bool		output_rewrites;

	/*
	 * 私有内存上下文.
	 */
	MemoryContext context;

	/*
	 * 特定类型对象的内存上下文
	 */
	MemoryContext change_context;
	MemoryContext txn_context;
	MemoryContext tup_context;

	XLogRecPtr	current_restart_decoding_lsn;

	/*  磁盘<->内存转换缓冲  */
	char	   *outbuf;
	Size		outbufsize;

	/*  内存记账  */
	Size		size;

	/*
	 * Statistics about transactions spilled to disk.
	 *
	 * A single transaction may be spilled repeatedly, which is why we keep
	 * two different counters. For spilling, the transaction counter includes
	 * both toplevel transactions and subtransactions.
	 */
	int64		spillTxns;		/*  溢写到磁盘的事务数  */
	int64		spillCount;		/*  溢写磁盘调用计数  */
	int64		spillBytes;		/*  溢写到磁盘的数据量  */

	/*  流式到解码输出插件的事务统计  */
	int64		streamTxns;		/*  已流式事务数  */
	int64		streamCount;	/*  流式调用计数  */
	int64		streamBytes;	/*  已解码数据量  */

	/*
	 * Statistics about all the transactions sent to the decoding output
	 * plugin
	 */
	int64		totalTxns;		/*  已发送事务总数  */
	int64		totalBytes;		/*  已解码数据总量  */
};


extern ReorderBuffer *ReorderBufferAllocate(void);
extern void ReorderBufferFree(ReorderBuffer *rb);

extern ReorderBufferTupleBuf *ReorderBufferGetTupleBuf(ReorderBuffer *rb,
													   Size tuple_len);
extern void ReorderBufferReturnTupleBuf(ReorderBuffer *rb,
										ReorderBufferTupleBuf *tuple);
extern ReorderBufferChange *ReorderBufferGetChange(ReorderBuffer *rb);
extern void ReorderBufferReturnChange(ReorderBuffer *rb,
									  ReorderBufferChange *change, bool upd_mem);

extern Oid *ReorderBufferGetRelids(ReorderBuffer *rb, int nrelids);
extern void ReorderBufferReturnRelids(ReorderBuffer *rb, Oid *relids);

extern void ReorderBufferQueueChange(ReorderBuffer *rb, TransactionId xid,
									 XLogRecPtr lsn, ReorderBufferChange *change,
									 bool toast_insert);
extern void ReorderBufferQueueMessage(ReorderBuffer *rb, TransactionId xid,
									  Snapshot snap, XLogRecPtr lsn,
									  bool transactional, const char *prefix,
									  Size message_size, const char *message);
extern void ReorderBufferCommit(ReorderBuffer *rb, TransactionId xid,
								XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
								TimestampTz commit_time, RepOriginId origin_id, XLogRecPtr origin_lsn);
extern void ReorderBufferFinishPrepared(ReorderBuffer *rb, TransactionId xid,
										XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
										XLogRecPtr two_phase_at,
										TimestampTz commit_time,
										RepOriginId origin_id, XLogRecPtr origin_lsn,
										char *gid, bool is_commit);
extern void ReorderBufferAssignChild(ReorderBuffer *rb, TransactionId xid,
									 TransactionId subxid, XLogRecPtr lsn);
extern void ReorderBufferCommitChild(ReorderBuffer *rb, TransactionId xid,
									 TransactionId subxid, XLogRecPtr commit_lsn,
									 XLogRecPtr end_lsn);
extern void ReorderBufferAbort(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn,
							   TimestampTz abort_time);
extern void ReorderBufferAbortOld(ReorderBuffer *rb, TransactionId oldestRunningXid);
extern void ReorderBufferForget(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn);
extern void ReorderBufferInvalidate(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn);

extern void ReorderBufferSetBaseSnapshot(ReorderBuffer *rb, TransactionId xid,
										 XLogRecPtr lsn, Snapshot snap);
extern void ReorderBufferAddSnapshot(ReorderBuffer *rb, TransactionId xid,
									 XLogRecPtr lsn, Snapshot snap);
extern void ReorderBufferAddNewCommandId(ReorderBuffer *rb, TransactionId xid,
										 XLogRecPtr lsn, CommandId cid);
extern void ReorderBufferAddNewTupleCids(ReorderBuffer *rb, TransactionId xid,
										 XLogRecPtr lsn, RelFileLocator locator,
										 ItemPointerData tid,
										 CommandId cmin, CommandId cmax, CommandId combocid);
extern void ReorderBufferAddInvalidations(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn,
										  Size nmsgs, SharedInvalidationMessage *msgs);
extern void ReorderBufferAddDistributedInvalidations(ReorderBuffer *rb, TransactionId xid,
													 XLogRecPtr lsn, Size nmsgs,
													 SharedInvalidationMessage *msgs);
extern void ReorderBufferImmediateInvalidation(ReorderBuffer *rb, uint32 ninvalidations,
											   SharedInvalidationMessage *invalidations);
extern void ReorderBufferProcessXid(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn);

extern void ReorderBufferXidSetCatalogChanges(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn);
extern bool ReorderBufferXidHasCatalogChanges(ReorderBuffer *rb, TransactionId xid);
extern bool ReorderBufferXidHasBaseSnapshot(ReorderBuffer *rb, TransactionId xid);

extern bool ReorderBufferRememberPrepareInfo(ReorderBuffer *rb, TransactionId xid,
											 XLogRecPtr prepare_lsn, XLogRecPtr end_lsn,
											 TimestampTz prepare_time,
											 RepOriginId origin_id, XLogRecPtr origin_lsn);
extern void ReorderBufferSkipPrepare(ReorderBuffer *rb, TransactionId xid);
extern void ReorderBufferPrepare(ReorderBuffer *rb, TransactionId xid, char *gid);
extern ReorderBufferTXN *ReorderBufferGetOldestTXN(ReorderBuffer *rb);
extern TransactionId ReorderBufferGetOldestXmin(ReorderBuffer *rb);
extern TransactionId *ReorderBufferGetCatalogChangesXacts(ReorderBuffer *rb);

extern void ReorderBufferSetRestartPoint(ReorderBuffer *rb, XLogRecPtr ptr);

extern uint32 ReorderBufferGetInvalidations(ReorderBuffer *rb,
											TransactionId xid,
											SharedInvalidationMessage **msgs);

extern void StartupReorderBuffer(void);

#endif
