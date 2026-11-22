/*-------------------------------------------------------------------------
 *
 * logicalproto.h
 *		逻辑复制协议
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/logicalproto.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICAL_PROTO_H
#define LOGICAL_PROTO_H

#include "access/xact.h"
#include "executor/tuptable.h"
#include "replication/reorderbuffer.h"
#include "utils/rel.h"

/*
 * Protocol capabilities
 *
 * LOGICALREP_PROTO_VERSION_NUM is our native protocol.
 * LOGICALREP_PROTO_MAX_VERSION_NUM is the greatest version we can support.
 * LOGICALREP_PROTO_MIN_VERSION_NUM is the oldest version we
 * have backwards compatibility for. The client requests protocol version at
 * connect time.
 *
 * LOGICALREP_PROTO_STREAM_VERSION_NUM is the minimum protocol version with
 * support for streaming large transactions. Introduced in PG14.
 *
 * LOGICALREP_PROTO_TWOPHASE_VERSION_NUM is the minimum protocol version with
 * support for two-phase commit decoding (at prepare time). Introduced in PG15.
 *
 * LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM is the minimum protocol version
 * where we support applying large streaming transactions in parallel.
 * Introduced in PG16.
 */
#define LOGICALREP_PROTO_MIN_VERSION_NUM 1
#define LOGICALREP_PROTO_VERSION_NUM 1
#define LOGICALREP_PROTO_STREAM_VERSION_NUM 2
#define LOGICALREP_PROTO_TWOPHASE_VERSION_NUM 3
#define LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM 4
#define LOGICALREP_PROTO_MAX_VERSION_NUM LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM

/*
 * Logical message types
 *
 * Used by logical replication wire protocol.
 *
 * Note: though this is an enum, the values are used to identify message types
 * in logical replication protocol, which uses a single byte to identify a
 * message type. Hence the values should be single-byte wide and preferably
 * human-readable characters.
 */
typedef enum LogicalRepMsgType
{
	LOGICAL_REP_MSG_BEGIN = 'B',
	LOGICAL_REP_MSG_COMMIT = 'C',
	LOGICAL_REP_MSG_ORIGIN = 'O',
	LOGICAL_REP_MSG_INSERT = 'I',
	LOGICAL_REP_MSG_UPDATE = 'U',
	LOGICAL_REP_MSG_DELETE = 'D',
	LOGICAL_REP_MSG_TRUNCATE = 'T',
	LOGICAL_REP_MSG_RELATION = 'R',
	LOGICAL_REP_MSG_TYPE = 'Y',
	LOGICAL_REP_MSG_MESSAGE = 'M',
	LOGICAL_REP_MSG_BEGIN_PREPARE = 'b',
	LOGICAL_REP_MSG_PREPARE = 'P',
	LOGICAL_REP_MSG_COMMIT_PREPARED = 'K',
	LOGICAL_REP_MSG_ROLLBACK_PREPARED = 'r',
	LOGICAL_REP_MSG_STREAM_START = 'S',
	LOGICAL_REP_MSG_STREAM_STOP = 'E',
	LOGICAL_REP_MSG_STREAM_COMMIT = 'c',
	LOGICAL_REP_MSG_STREAM_ABORT = 'A',
	LOGICAL_REP_MSG_STREAM_PREPARE = 'p'
} LogicalRepMsgType;

/*
 * This struct stores a tuple received via logical replication.
 * Keep in mind that the columns correspond to the *remote* table.
 */
typedef struct LogicalRepTupleData
{
	/*  每列一个 StringInfo 的数组;有些可能未用  */
	StringInfoData *colvalues;
	/*  每列空/未变/文本/二进制标记的数组  */
	char	   *colstatus;
	/* Length of above arrays */
	int			ncols;
} LogicalRepTupleData;

/*  LogicalRepTupleData.colstatus[colnum] 的可能取值  */
/*  这些值也用于线上协议  */
#define LOGICALREP_COLUMN_NULL		'n'
#define LOGICALREP_COLUMN_UNCHANGED	'u'
#define LOGICALREP_COLUMN_TEXT		't'
#define LOGICALREP_COLUMN_BINARY	'b' /* added in PG14 */

typedef uint32 LogicalRepRelId;

/*  关系信息  */
typedef struct LogicalRepRelation
{
	/*  来自远端的信息。  */
	LogicalRepRelId remoteid;	/*  关系的唯一 id  */
	char	   *nspname;		/*  模式名  */
	char	   *relname;		/*  关系名  */
	int			natts;			/*  列数  */
	char	  **attnames;		/*  列名  */
	Oid		   *atttyps;		/*  列类型  */
	char		replident;		/*  副本标识  */
	char		relkind;		/*  远端关系种类  */
	Bitmapset  *attkeys;		/*  键列位图  */
} LogicalRepRelation;

/* Type mapping info */
typedef struct LogicalRepTyp
{
	Oid			remoteid;		/*  远端类型的唯一 id  */
	char	   *nspname;		/*  远端类型的模式名  */
	char	   *typname;		/*  远端类型名  */
} LogicalRepTyp;

/*  事务信息  */
typedef struct LogicalRepBeginData
{
	XLogRecPtr	final_lsn;
	TimestampTz committime;
	TransactionId xid;
} LogicalRepBeginData;

typedef struct LogicalRepCommitData
{
	XLogRecPtr	commit_lsn;
	XLogRecPtr	end_lsn;
	TimestampTz committime;
} LogicalRepCommitData;

/*
 * Prepared transaction protocol information for begin_prepare, and prepare.
 */
typedef struct LogicalRepPreparedTxnData
{
	XLogRecPtr	prepare_lsn;
	XLogRecPtr	end_lsn;
	TimestampTz prepare_time;
	TransactionId xid;
	char		gid[GIDSIZE];
} LogicalRepPreparedTxnData;

/*
 * Prepared transaction protocol information for commit prepared.
 */
typedef struct LogicalRepCommitPreparedTxnData
{
	XLogRecPtr	commit_lsn;
	XLogRecPtr	end_lsn;
	TimestampTz commit_time;
	TransactionId xid;
	char		gid[GIDSIZE];
} LogicalRepCommitPreparedTxnData;

/*
 * Rollback Prepared transaction protocol information. The prepare information
 * prepare_end_lsn and prepare_time are used to check if the downstream has
 * received this prepared transaction in which case it can apply the rollback,
 * otherwise, it can skip the rollback operation. The gid alone is not
 * sufficient because the downstream node can have a prepared transaction with
 * same identifier.
 */
typedef struct LogicalRepRollbackPreparedTxnData
{
	XLogRecPtr	prepare_end_lsn;
	XLogRecPtr	rollback_end_lsn;
	TimestampTz prepare_time;
	TimestampTz rollback_time;
	TransactionId xid;
	char		gid[GIDSIZE];
} LogicalRepRollbackPreparedTxnData;

/*
 * Transaction protocol information for stream abort.
 */
typedef struct LogicalRepStreamAbortData
{
	TransactionId xid;
	TransactionId subxid;
	XLogRecPtr	abort_lsn;
	TimestampTz abort_time;
} LogicalRepStreamAbortData;

extern void logicalrep_write_begin(StringInfo out, ReorderBufferTXN *txn);
extern void logicalrep_read_begin(StringInfo in,
								  LogicalRepBeginData *begin_data);
extern void logicalrep_write_commit(StringInfo out, ReorderBufferTXN *txn,
									XLogRecPtr commit_lsn);
extern void logicalrep_read_commit(StringInfo in,
								   LogicalRepCommitData *commit_data);
extern void logicalrep_write_begin_prepare(StringInfo out, ReorderBufferTXN *txn);
extern void logicalrep_read_begin_prepare(StringInfo in,
										  LogicalRepPreparedTxnData *begin_data);
extern void logicalrep_write_prepare(StringInfo out, ReorderBufferTXN *txn,
									 XLogRecPtr prepare_lsn);
extern void logicalrep_read_prepare(StringInfo in,
									LogicalRepPreparedTxnData *prepare_data);
extern void logicalrep_write_commit_prepared(StringInfo out, ReorderBufferTXN *txn,
											 XLogRecPtr commit_lsn);
extern void logicalrep_read_commit_prepared(StringInfo in,
											LogicalRepCommitPreparedTxnData *prepare_data);
extern void logicalrep_write_rollback_prepared(StringInfo out, ReorderBufferTXN *txn,
											   XLogRecPtr prepare_end_lsn,
											   TimestampTz prepare_time);
extern void logicalrep_read_rollback_prepared(StringInfo in,
											  LogicalRepRollbackPreparedTxnData *rollback_data);
extern void logicalrep_write_stream_prepare(StringInfo out, ReorderBufferTXN *txn,
											XLogRecPtr prepare_lsn);
extern void logicalrep_read_stream_prepare(StringInfo in,
										   LogicalRepPreparedTxnData *prepare_data);

extern void logicalrep_write_origin(StringInfo out, const char *origin,
									XLogRecPtr origin_lsn);
extern char *logicalrep_read_origin(StringInfo in, XLogRecPtr *origin_lsn);
extern void logicalrep_write_insert(StringInfo out, TransactionId xid,
									Relation rel,
									TupleTableSlot *newslot,
									bool binary, Bitmapset *columns);
extern LogicalRepRelId logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup);
extern void logicalrep_write_update(StringInfo out, TransactionId xid,
									Relation rel,
									TupleTableSlot *oldslot,
									TupleTableSlot *newslot, bool binary, Bitmapset *columns);
extern LogicalRepRelId logicalrep_read_update(StringInfo in,
											  bool *has_oldtuple, LogicalRepTupleData *oldtup,
											  LogicalRepTupleData *newtup);
extern void logicalrep_write_delete(StringInfo out, TransactionId xid,
									Relation rel, TupleTableSlot *oldslot,
									bool binary, Bitmapset *columns);
extern LogicalRepRelId logicalrep_read_delete(StringInfo in,
											  LogicalRepTupleData *oldtup);
extern void logicalrep_write_truncate(StringInfo out, TransactionId xid,
									  int nrelids, Oid relids[],
									  bool cascade, bool restart_seqs);
extern List *logicalrep_read_truncate(StringInfo in,
									  bool *cascade, bool *restart_seqs);
extern void logicalrep_write_message(StringInfo out, TransactionId xid, XLogRecPtr lsn,
									 bool transactional, const char *prefix, Size sz, const char *message);
extern void logicalrep_write_rel(StringInfo out, TransactionId xid,
								 Relation rel, Bitmapset *columns);
extern LogicalRepRelation *logicalrep_read_rel(StringInfo in);
extern void logicalrep_write_typ(StringInfo out, TransactionId xid,
								 Oid typoid);
extern void logicalrep_read_typ(StringInfo in, LogicalRepTyp *ltyp);
extern void logicalrep_write_stream_start(StringInfo out, TransactionId xid,
										  bool first_segment);
extern TransactionId logicalrep_read_stream_start(StringInfo in,
												  bool *first_segment);
extern void logicalrep_write_stream_stop(StringInfo out);
extern void logicalrep_write_stream_commit(StringInfo out, ReorderBufferTXN *txn,
										   XLogRecPtr commit_lsn);
extern TransactionId logicalrep_read_stream_commit(StringInfo in,
												   LogicalRepCommitData *commit_data);
extern void logicalrep_write_stream_abort(StringInfo out, TransactionId xid,
										  TransactionId subxid,
										  XLogRecPtr abort_lsn,
										  TimestampTz abort_time,
										  bool write_abort_info);
extern void logicalrep_read_stream_abort(StringInfo in,
										 LogicalRepStreamAbortData *abort_data,
										 bool read_abort_info);
extern const char *logicalrep_message_type(LogicalRepMsgType action);

#endif							/* LOGICAL_PROTO_H */
