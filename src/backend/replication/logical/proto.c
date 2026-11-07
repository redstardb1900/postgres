/*-------------------------------------------------------------------------
 *
 * proto.c
 *		逻辑复制协议函数              
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/replication/logical/proto.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "replication/logicalproto.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * 协议消息标志。  
 */
#define LOGICALREP_IS_REPLICA_IDENTITY 1

#define MESSAGE_TRANSACTIONAL (1<<0)
#define TRUNCATE_CASCADE		(1<<0)
#define TRUNCATE_RESTART_SEQS	(1<<1)

static void logicalrep_write_attrs(StringInfo out, Relation rel,
								   Bitmapset *columns);
static void logicalrep_write_tuple(StringInfo out, Relation rel,
								   TupleTableSlot *slot,
								   bool binary, Bitmapset *columns);
static void logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel);
static void logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple);

static void logicalrep_write_namespace(StringInfo out, Oid nspid);
static const char *logicalrep_read_namespace(StringInfo in);

/*
 * 检查某列是否被列清单覆盖。       
 *
 * 需要小心处理 NULL，它被视为列清单，覆盖                 
 * 所有列。
 */
static bool
column_in_column_list(int attnum, Bitmapset *columns)
{
	return (columns == NULL || bms_is_member(attnum, columns));
}


/*
 * 将 BEGIN 写入输出流。     
 */
void
logicalrep_write_begin(StringInfo out, ReorderBufferTXN *txn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_BEGIN);

	/* 固定字段 */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);
}

/*
 * 从流中读取事务 BEGIN。         
 */
void
logicalrep_read_begin(StringInfo in, LogicalRepBeginData *begin_data)
{
	/* 读取字段*/
	begin_data->final_lsn = pq_getmsgint64(in);
	if (begin_data->final_lsn == InvalidXLogRecPtr)
		elog(ERROR, "final_lsn not set in begin message");
	begin_data->committime = pq_getmsgint64(in);
	begin_data->xid = pq_getmsgint(in, 4);
}


/*
 * 将 COMMIT 写入输出流。     
 */
void
logicalrep_write_commit(StringInfo out, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_COMMIT);

	/* 发送 flags 字段（暂未使用） */
	pq_sendbyte(out, flags);

	/* 发送字段*/
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
}

/*
 * 从流中读取事务 COMMIT。         
 */
void
logicalrep_read_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
	/* 读取暂未使用的 flags */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit message", flags);

	/* 读取字段*/
	commit_data->commit_lsn = pq_getmsgint64(in);
	commit_data->end_lsn = pq_getmsgint64(in);
	commit_data->committime = pq_getmsgint64(in);
}

/*
 * 将 BEGIN PREPARE 写入输出流。     
 */
void
logicalrep_write_begin_prepare(StringInfo out, ReorderBufferTXN *txn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_BEGIN_PREPARE);

	/* 固定字段 */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.prepare_time);
	pq_sendint32(out, txn->xid);

	/* 写 gid  */
	pq_sendstring(out, txn->gid);
}

/*
 * 从流中读取事务 BEGIN PREPARE。         
 */
void
logicalrep_read_begin_prepare(StringInfo in, LogicalRepPreparedTxnData *begin_data)
{
	/* 读取字段*/
	begin_data->prepare_lsn = pq_getmsgint64(in);
	if (begin_data->prepare_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_lsn not set in begin prepare message");
	begin_data->end_lsn = pq_getmsgint64(in);
	if (begin_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn not set in begin prepare message");
	begin_data->prepare_time = pq_getmsgint64(in);
	begin_data->xid = pq_getmsgint(in, 4);

	/* 读取 gid（复制到预分配的缓冲区） */
	strlcpy(begin_data->gid, pq_getmsgstring(in), sizeof(begin_data->gid));
}

/*
 * 核心功能：供 logicalrep_write_prepare 与        
 * logicalrep_write_stream_prepare 
 */
static void
logicalrep_write_prepare_common(StringInfo out, LogicalRepMsgType type,
								ReorderBufferTXN *txn, XLogRecPtr prepare_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, type);

	/*
	 * 这应当只发生在两阶段提交事务中，此时我们预期
	 * 会有一个有效的 GID。             
	 */
	Assert(txn->gid != NULL);
	Assert(rbtxn_prepared(txn));
	Assert(TransactionIdIsValid(txn->xid));

	/* 发送 flags 字段  */
	pq_sendbyte(out, flags);

	/* 发送字段*/
	pq_sendint64(out, prepare_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.prepare_time);
	pq_sendint32(out, txn->xid);

	/* 写 gid  */
	pq_sendstring(out, txn->gid);
}

/*
 * 将 PREPARE 写入输出流。     
 */
void
logicalrep_write_prepare(StringInfo out, ReorderBufferTXN *txn,
						 XLogRecPtr prepare_lsn)
{
	logicalrep_write_prepare_common(out, LOGICAL_REP_MSG_PREPARE,
									txn, prepare_lsn);
}

/*
 * 核心功能：供 logicalrep_read_prepare 与        
 * logicalrep_read_stream_prepare 
 */
static void
logicalrep_read_prepare_common(StringInfo in, char *msgtype,
							   LogicalRepPreparedTxnData *prepare_data)
{
	/* 读 flags  */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in %s message", flags, msgtype);

	/* 读取字段*/
	prepare_data->prepare_lsn = pq_getmsgint64(in);
	if (prepare_data->prepare_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_lsn is not set in %s message", msgtype);
	prepare_data->end_lsn = pq_getmsgint64(in);
	if (prepare_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn is not set in %s message", msgtype);
	prepare_data->prepare_time = pq_getmsgint64(in);
	prepare_data->xid = pq_getmsgint(in, 4);
	if (prepare_data->xid == InvalidTransactionId)
		elog(ERROR, "invalid two-phase transaction ID in %s message", msgtype);

	/* 读取 gid（复制到预分配的缓冲区） */
	strlcpy(prepare_data->gid, pq_getmsgstring(in), sizeof(prepare_data->gid));
}

/*
 * 从流中读取事务 PREPARE。         
 */
void
logicalrep_read_prepare(StringInfo in, LogicalRepPreparedTxnData *prepare_data)
{
	logicalrep_read_prepare_common(in, "prepare", prepare_data);
}

/*
 * 将 COMMIT PREPARED 写入输出流。     
 */
void
logicalrep_write_commit_prepared(StringInfo out, ReorderBufferTXN *txn,
								 XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_COMMIT_PREPARED);

	/*
	 * 这应当只发生在两阶段提交事务中，此时我们预期
	 * 会有一个有效的 GID。             
	 */
	Assert(txn->gid != NULL);

	/* 发送 flags 字段  */
	pq_sendbyte(out, flags);

	/* 发送字段*/
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);

	/* 写 gid  */
	pq_sendstring(out, txn->gid);
}

/*
 * 从流中读取事务 COMMIT PREPARED。         
 */
void
logicalrep_read_commit_prepared(StringInfo in, LogicalRepCommitPreparedTxnData *prepare_data)
{
	/* 读 flags  */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit prepared message", flags);

	/* 读取字段*/
	prepare_data->commit_lsn = pq_getmsgint64(in);
	if (prepare_data->commit_lsn == InvalidXLogRecPtr)
		elog(ERROR, "commit_lsn is not set in commit prepared message");
	prepare_data->end_lsn = pq_getmsgint64(in);
	if (prepare_data->end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "end_lsn is not set in commit prepared message");
	prepare_data->commit_time = pq_getmsgint64(in);
	prepare_data->xid = pq_getmsgint(in, 4);

	/* 读取 gid（复制到预分配的缓冲区） */
	strlcpy(prepare_data->gid, pq_getmsgstring(in), sizeof(prepare_data->gid));
}

/*
 * 将 ROLLBACK PREPARED 写入输出流。     
 */
void
logicalrep_write_rollback_prepared(StringInfo out, ReorderBufferTXN *txn,
								   XLogRecPtr prepare_end_lsn,
								   TimestampTz prepare_time)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_ROLLBACK_PREPARED);

	/*
	 * 这应当只发生在两阶段提交事务中，此时我们预期
	 * 会有一个有效的 GID。             
	 */
	Assert(txn->gid != NULL);

	/* 发送 flags 字段  */
	pq_sendbyte(out, flags);

	/* 发送字段*/
	pq_sendint64(out, prepare_end_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, prepare_time);
	pq_sendint64(out, txn->xact_time.commit_time);
	pq_sendint32(out, txn->xid);

	/* 写 gid  */
	pq_sendstring(out, txn->gid);
}

/*
 * 从流中读取事务 ROLLBACK PREPARED。         
 */
void
logicalrep_read_rollback_prepared(StringInfo in,
								  LogicalRepRollbackPreparedTxnData *rollback_data)
{
	/* 读 flags  */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in rollback prepared message", flags);

	/* 读取字段*/
	rollback_data->prepare_end_lsn = pq_getmsgint64(in);
	if (rollback_data->prepare_end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "prepare_end_lsn is not set in rollback prepared message");
	rollback_data->rollback_end_lsn = pq_getmsgint64(in);
	if (rollback_data->rollback_end_lsn == InvalidXLogRecPtr)
		elog(ERROR, "rollback_end_lsn is not set in rollback prepared message");
	rollback_data->prepare_time = pq_getmsgint64(in);
	rollback_data->rollback_time = pq_getmsgint64(in);
	rollback_data->xid = pq_getmsgint(in, 4);

	/* 读取 gid（复制到预分配的缓冲区） */
	strlcpy(rollback_data->gid, pq_getmsgstring(in), sizeof(rollback_data->gid));
}

/*
 * 将 STREAM PREPARE 写入输出流。     
 */
void
logicalrep_write_stream_prepare(StringInfo out,
								ReorderBufferTXN *txn,
								XLogRecPtr prepare_lsn)
{
	logicalrep_write_prepare_common(out, LOGICAL_REP_MSG_STREAM_PREPARE,
									txn, prepare_lsn);
}

/*
 * 从流中读取 STREAM PREPARE。   
 */
void
logicalrep_read_stream_prepare(StringInfo in, LogicalRepPreparedTxnData *prepare_data)
{
	logicalrep_read_prepare_common(in, "stream prepare", prepare_data);
}

/*
 * 将 ORIGIN 写入输出流。     
 */
void
logicalrep_write_origin(StringInfo out, const char *origin,
						XLogRecPtr origin_lsn)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_ORIGIN);

	/* 固定字段 */
	pq_sendint64(out, origin_lsn);

	/* 源字符串  */
	pq_sendstring(out, origin);
}

/*
 * 从输出流中读取 ORIGIN。    
 */
char *
logicalrep_read_origin(StringInfo in, XLogRecPtr *origin_lsn)
{
	/* 固定字段 */
	*origin_lsn = pq_getmsgint64(in);

	/* 返回 origin */
	return pstrdup(pq_getmsgstring(in));
}

/*
 * 将 INSERT 写入输出流。     
 */
void
logicalrep_write_insert(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *newslot, bool binary, Bitmapset *columns)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_INSERT);

	/* 事务 ID（若无效，则不在流式传输）   */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* 用 Oid 作为关系标识符  */
	pq_sendint32(out, RelationGetRelid(rel));

	pq_sendbyte(out, 'N');		/* 后跟新元组   */
	logicalrep_write_tuple(out, rel, newslot, binary, columns);
}

/*
 * 从流读取 INSERT。  
 *
 * 填充新元组。  
 */
LogicalRepRelId
logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* 读取关系 ID      */
	relid = pq_getmsgint(in, 4);

	action = pq_getmsgbyte(in);
	if (action != 'N')
		elog(ERROR, "expected new tuple but got %d",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * 将 UPDATE 写入输出流。     
 */
void
logicalrep_write_update(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *oldslot, TupleTableSlot *newslot,
						bool binary, Bitmapset *columns)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_UPDATE);

	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	/* 事务 ID（若无效，则不在流式传输）   */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* 用 Oid 作为关系标识符  */
	pq_sendint32(out, RelationGetRelid(rel));

	if (oldslot != NULL)
	{
		if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
			pq_sendbyte(out, 'O');	/* 后跟旧元组   */
		else
			pq_sendbyte(out, 'K');	/* 后跟旧键    */
		logicalrep_write_tuple(out, rel, oldslot, binary, columns);
	}

	pq_sendbyte(out, 'N');		/* 后跟新元组   */
	logicalrep_write_tuple(out, rel, newslot, binary, columns);
}

/*
 * 从流读取 UPDATE。  
 */
LogicalRepRelId
logicalrep_read_update(StringInfo in, bool *has_oldtuple,
					   LogicalRepTupleData *oldtup,
					   LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* 读取关系 ID      */
	relid = pq_getmsgint(in, 4);

	/* 读取并校验动作  */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O' && action != 'N')
		elog(ERROR, "expected action 'N', 'O' or 'K', got %c",
			 action);

	/* 检查旧元组     */
	if (action == 'K' || action == 'O')
	{
		logicalrep_read_tuple(in, oldtup);
		*has_oldtuple = true;

		action = pq_getmsgbyte(in);
	}
	else
		*has_oldtuple = false;

	/* 检查新元组      */
	if (action != 'N')
		elog(ERROR, "expected action 'N', got %c",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * 将 DELETE 写入输出流。     
 */
void
logicalrep_write_delete(StringInfo out, TransactionId xid, Relation rel,
						TupleTableSlot *oldslot, bool binary,
						Bitmapset *columns)
{
	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	pq_sendbyte(out, LOGICAL_REP_MSG_DELETE);

	/* 事务 ID（若无效，则不在流式传输）   */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* 用 Oid 作为关系标识符  */
	pq_sendint32(out, RelationGetRelid(rel));

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		pq_sendbyte(out, 'O');	/* 后跟旧元组   */
	else
		pq_sendbyte(out, 'K');	/* 后跟旧键    */

	logicalrep_write_tuple(out, rel, oldslot, binary, columns);
}

/*
 * 从流读取 DELETE。  
 *
 * 填充旧元组。  
 */
LogicalRepRelId
logicalrep_read_delete(StringInfo in, LogicalRepTupleData *oldtup)
{
	char		action;
	LogicalRepRelId relid;

	/* 读取关系 ID      */
	relid = pq_getmsgint(in, 4);

	/* 读取并校验动作  */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O')
		elog(ERROR, "expected action 'O' or 'K', got %c", action);

	logicalrep_read_tuple(in, oldtup);

	return relid;
}

/*
 * 将 TRUNCATE 写入输出流。     
 */
void
logicalrep_write_truncate(StringInfo out,
						  TransactionId xid,
						  int nrelids,
						  Oid relids[],
						  bool cascade, bool restart_seqs)
{
	int			i;
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_TRUNCATE);

	/* 事务 ID（若无效，则不在流式传输）   */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	pq_sendint32(out, nrelids);

	/* 编码并发送截断标志    */
	if (cascade)
		flags |= TRUNCATE_CASCADE;
	if (restart_seqs)
		flags |= TRUNCATE_RESTART_SEQS;
	pq_sendint8(out, flags);

	for (i = 0; i < nrelids; i++)
		pq_sendint32(out, relids[i]);
}

/*
 * 从流读取 TRUNCATE。  
 */
List *
logicalrep_read_truncate(StringInfo in,
						 bool *cascade, bool *restart_seqs)
{
	int			i;
	int			nrelids;
	List	   *relids = NIL;
	uint8		flags;

	nrelids = pq_getmsgint(in, 4);

	/* 读取并解码截断标志    */
	flags = pq_getmsgint(in, 1);
	*cascade = (flags & TRUNCATE_CASCADE) > 0;
	*restart_seqs = (flags & TRUNCATE_RESTART_SEQS) > 0;

	for (i = 0; i < nrelids; i++)
		relids = lappend_oid(relids, pq_getmsgint(in, 4));

	return relids;
}

/*
 * 将 MESSAGE 写入流  
 */
void
logicalrep_write_message(StringInfo out, TransactionId xid, XLogRecPtr lsn,
						 bool transactional, const char *prefix, Size sz,
						 const char *message)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_MESSAGE);

	/* 编码并发送消息标志   */
	if (transactional)
		flags |= MESSAGE_TRANSACTIONAL;

	/* 事务 ID（若无效，则不在流式传输）   */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	pq_sendint8(out, flags);
	pq_sendint64(out, lsn);
	pq_sendstring(out, prefix);
	pq_sendint32(out, sz);
	pq_sendbytes(out, message, sz);
}

/*
 * 将关系描述写入输出流。               
 */
void
logicalrep_write_rel(StringInfo out, TransactionId xid, Relation rel,
					 Bitmapset *columns)
{
	char	   *relname;

	pq_sendbyte(out, LOGICAL_REP_MSG_RELATION);

	/* 事务 ID（若无效，则不在流式传输）   */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	/* 用 Oid 作为关系标识符  */
	pq_sendint32(out, RelationGetRelid(rel));

	/* 发送关系限定名        */
	logicalrep_write_namespace(out, RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);
	pq_sendstring(out, relname);

	/* 发送复制标识    */
	pq_sendbyte(out, rel->rd_rel->relreplident);

	/* 发送属性信息      */
	logicalrep_write_attrs(out, rel, columns);
}

/*
 * 从流中读取关系信息并以 LogicalRepRelation 返回。      
 */
LogicalRepRelation *
logicalrep_read_rel(StringInfo in)
{
	LogicalRepRelation *rel = palloc(sizeof(LogicalRepRelation));

	rel->remoteid = pq_getmsgint(in, 4);

	/* 从流中读取关系名       */
	rel->nspname = pstrdup(logicalrep_read_namespace(in));
	rel->relname = pstrdup(pq_getmsgstring(in));

	/* 读取复制标识。      */
	rel->replident = pq_getmsgbyte(in);

	/* 获取属性描述        */
	logicalrep_read_attrs(in, rel);

	return rel;
}

/*
 * 将类型信息写入输出流。    
 *
 * 此函数总是写入基类型信息。        
 */
void
logicalrep_write_typ(StringInfo out, TransactionId xid, Oid typoid)
{
	Oid			basetypoid = getBaseType(typoid);
	HeapTuple	tup;
	Form_pg_type typtup;

	pq_sendbyte(out, LOGICAL_REP_MSG_TYPE);

	/* 事务 ID（若无效，则不在流式传输）   */
	if (TransactionIdIsValid(xid))
		pq_sendint32(out, xid);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(basetypoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", basetypoid);
	typtup = (Form_pg_type) GETSTRUCT(tup);

	/* 用 Oid 作为关系标识符  */
	pq_sendint32(out, typoid);

	/* 发送类型限定名    */
	logicalrep_write_namespace(out, typtup->typnamespace);
	pq_sendstring(out, NameStr(typtup->typname));

	ReleaseSysCache(tup);
}

/*
 * 从输出流中读取类型信息。  
 */
void
logicalrep_read_typ(StringInfo in, LogicalRepTyp *ltyp)
{
	ltyp->remoteid = pq_getmsgint(in, 4);

	/* 从流中读取类型名   */
	ltyp->nspname = pstrdup(logicalrep_read_namespace(in));
	ltyp->typname = pstrdup(pq_getmsgstring(in));
}

/*
 * 以尽可能高效的格式将元组写入输出流。                   
 */
static void
logicalrep_write_tuple(StringInfo out, Relation rel, TupleTableSlot *slot,
					   bool binary, Bitmapset *columns)
{
	TupleDesc	desc;
	Datum	   *values;
	bool	   *isnull;
	int			i;
	uint16		nliveatts = 0;

	desc = RelationGetDescr(rel);

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || att->attgenerated)
			continue;

		if (!column_in_column_list(att->attnum, columns))
			continue;

		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	slot_getallattrs(slot);
	values = slot->tts_values;
	isnull = slot->tts_isnull;

	/* 写入这些值  */
	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typtup;
		Form_pg_type typclass;
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || att->attgenerated)
			continue;

		if (!column_in_column_list(att->attnum, columns))
			continue;

		if (isnull[i])
		{
			pq_sendbyte(out, LOGICALREP_COLUMN_NULL);
			continue;
		}

		if (att->attlen == -1 && VARATT_IS_EXTERNAL_ONDISK(values[i]))
		{
			/*
			 * 未变更的 TOAST 值。（注意，我们并不承诺检测  
			 * 出一般的未变更数据；这只是为避免不必要     
			 * 地发送大值。）               
			 */
			pq_sendbyte(out, LOGICALREP_COLUMN_UNCHANGED);
			continue;
		}

		typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(typtup))
			elog(ERROR, "cache lookup failed for type %u", att->atttypid);
		typclass = (Form_pg_type) GETSTRUCT(typtup);

		/*
		 * 若被要求且类型发送函数合适，则二进制发送。 
		 */
		if (binary && OidIsValid(typclass->typsend))
		{
			bytea	   *outputbytes;
			int			len;

			pq_sendbyte(out, LOGICALREP_COLUMN_BINARY);
			outputbytes = OidSendFunctionCall(typclass->typsend, values[i]);
			len = VARSIZE(outputbytes) - VARHDRSZ;
			pq_sendint(out, len, 4);	/* 长度 */
			pq_sendbytes(out, VARDATA(outputbytes), len);	/* 值  */
			pfree(outputbytes);
		}
		else
		{
			char	   *outputstr;

			pq_sendbyte(out, LOGICALREP_COLUMN_TEXT);
			outputstr = OidOutputFunctionCall(typclass->typoutput, values[i]);
			pq_sendcountedtext(out, outputstr, strlen(outputstr), false);
			pfree(outputstr);
		}

		ReleaseSysCache(typtup);
	}
}

/*
 * 从流中读取逻辑复制格式的元组。        
 */
static void
logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple)
{
	int			i;
	int			natts;

	/* 获取属性数量       */
	natts = pq_getmsgint(in, 2);

	/* 为各列值分配空间；将未使用的 StringInfoData 清零      */
	tuple->colvalues = (StringInfoData *) palloc0(natts * sizeof(StringInfoData));
	tuple->colstatus = (char *) palloc(natts * sizeof(char));
	tuple->ncols = natts;

	/* 读取数据  */
	for (i = 0; i < natts; i++)
	{
		char		kind;
		int			len;
		StringInfo	value = &tuple->colvalues[i];

		kind = pq_getmsgbyte(in);
		tuple->colstatus[i] = kind;

		switch (kind)
		{
			case LOGICALREP_COLUMN_NULL:
				/* 无需更多处理 */
				break;
			case LOGICALREP_COLUMN_UNCHANGED:
				/* 我们不接收未变更列的值                 */
				break;
			case LOGICALREP_COLUMN_TEXT:
			case LOGICALREP_COLUMN_BINARY:
				len = pq_getmsgint(in, 4);	/* 读取长度*/

				/* 及数据*/
				value->data = palloc(len + 1);
				pq_copymsgbytes(in, value->data, len);

				/*
				 * 对 LOGICALREP_COLUMN_BINARY 并非严格必需，只是
				 * 按 StringInfo 惯例。
				 */
				value->data[len] = '\0';

				/* 使 StringInfo 完全有效 */
				value->len = len;
				value->cursor = 0;
				value->maxlen = len;
				break;
			default:
				elog(ERROR, "unrecognized data representation type '%c'", kind);
		}
	}
}

/*
 * 将关系属性元数据写入流。            
 */
static void
logicalrep_write_attrs(StringInfo out, Relation rel, Bitmapset *columns)
{
	TupleDesc	desc;
	int			i;
	uint16		nliveatts = 0;
	Bitmapset  *idattrs = NULL;
	bool		replidentfull;

	desc = RelationGetDescr(rel);

	/* 发送有效属性的数量    */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || att->attgenerated)
			continue;

		if (!column_in_column_list(att->attnum, columns))
			continue;

		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	/* 获取 REPLICATION IDENTITY 属性的位图     */
	replidentfull = (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL);
	if (!replidentfull)
		idattrs = RelationGetIdentityKeyBitmap(rel);

	/* 发送这些属性  */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		uint8		flags = 0;

		if (att->attisdropped || att->attgenerated)
			continue;

		if (!column_in_column_list(att->attnum, columns))
			continue;

		/* REPLICA IDENTITY FULL 表示所有列均随键一起发送。    */
		if (replidentfull ||
			bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			flags |= LOGICALREP_IS_REPLICA_IDENTITY;

		pq_sendbyte(out, flags);

		/* 属性名      */
		pq_sendstring(out, NameStr(att->attname));

		/* 属性类型 ID   */
		pq_sendint32(out, (int) att->atttypid);

		/* 属性模式   */
		pq_sendint32(out, att->atttypmod);
	}

	bms_free(idattrs);
}

/*
 * 从流中读取关系属性元数据。          
 */
static void
logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel)
{
	int			i;
	int			natts;
	char	  **attnames;
	Oid		   *atttyps;
	Bitmapset  *attkeys = NULL;

	natts = pq_getmsgint(in, 2);
	attnames = palloc(natts * sizeof(char *));
	atttyps = palloc(natts * sizeof(Oid));

	/* 读取这些属性  */
	for (i = 0; i < natts; i++)
	{
		uint8		flags;

		/* 检查是否为复制标识列    */
		flags = pq_getmsgbyte(in);
		if (flags & LOGICALREP_IS_REPLICA_IDENTITY)
			attkeys = bms_add_member(attkeys, i);

		/* 属性名      */
		attnames[i] = pstrdup(pq_getmsgstring(in));

		/* 属性类型 ID   */
		atttyps[i] = (Oid) pq_getmsgint(in, 4);

		/* 我们暂时忽略属性模式   */
		(void) pq_getmsgint(in, 4);
	}

	rel->attnames = attnames;
	rel->atttyps = atttyps;
	rel->attkeys = attkeys;
	rel->natts = natts;
}

/*
 * 写入命名空间名，pg_catalog 则写空串（以节省空间）。 
 */
static void
logicalrep_write_namespace(StringInfo out, Oid nspid)
{
	if (nspid == PG_CATALOG_NAMESPACE)
		pq_sendbyte(out, '\0');
	else
	{
		char	   *nspname = get_namespace_name(nspid);

		if (nspname == NULL)
			elog(ERROR, "cache lookup failed for namespace %u",
				 nspid);

		pq_sendstring(out, nspname);
	}
}

/*
 * 读取命名空间名，空串按 pg_catalog 处理。            
 */
static const char *
logicalrep_read_namespace(StringInfo in)
{
	const char *nspname = pq_getmsgstring(in);

	if (nspname[0] == '\0')
		nspname = "pg_catalog";

	return nspname;
}

/*
 * 将 start stream 消息的信息写入输出流。                      
 */
void
logicalrep_write_stream_start(StringInfo out,
							  TransactionId xid, bool first_segment)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_START);

	Assert(TransactionIdIsValid(xid));

	/* 事务 ID（即将开始流式传输，因此必须有效）*/
	pq_sendint32(out, xid);

	/* 若这是该 xid 的首个流式传输分段则为 1  */
	pq_sendbyte(out, first_segment ? 1 : 0);
}

/*
 * 从输出流中读取 start stream 消息的信息。                  
 */
TransactionId
logicalrep_read_stream_start(StringInfo in, bool *first_segment)
{
	TransactionId xid;

	Assert(first_segment);

	xid = pq_getmsgint(in, 4);
	*first_segment = (pq_getmsgbyte(in) == 1);

	return xid;
}

/*
 * 将 stop stream 消息写入输出流。           
 */
void
logicalrep_write_stream_stop(StringInfo out)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_STOP);
}

/*
 * 将 STREAM COMMIT 写入输出流。     
 */
void
logicalrep_write_stream_commit(StringInfo out, ReorderBufferTXN *txn,
							   XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_COMMIT);

	Assert(TransactionIdIsValid(txn->xid));

	/* 事务 ID      */
	pq_sendint32(out, txn->xid);

	/* 发送 flags 字段（暂未使用） */
	pq_sendbyte(out, flags);

	/* 发送字段*/
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->xact_time.commit_time);
}

/*
 * 从输出流中读取 STREAM COMMIT。    
 */
TransactionId
logicalrep_read_stream_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
	TransactionId xid;
	uint8		flags;

	xid = pq_getmsgint(in, 4);

	/* 读取暂未使用的 flags */
	flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit message", flags);

	/* 读取字段*/
	commit_data->commit_lsn = pq_getmsgint64(in);
	commit_data->end_lsn = pq_getmsgint64(in);
	commit_data->committime = pq_getmsgint64(in);

	return xid;
}

/*
 * 将 STREAM ABORT 写入输出流。注意：对顶层事务              
 * 中止，xid 与 subxid 将相同。     
 *
 * 若 write_abort_info 为真，发送 abort_lsn、abort_time 字段， 
 * 否则不发。 
 */
void
logicalrep_write_stream_abort(StringInfo out, TransactionId xid,
							  TransactionId subxid, XLogRecPtr abort_lsn,
							  TimestampTz abort_time, bool write_abort_info)
{
	pq_sendbyte(out, LOGICAL_REP_MSG_STREAM_ABORT);

	Assert(TransactionIdIsValid(xid) && TransactionIdIsValid(subxid));

	/* 事务 ID      */
	pq_sendint32(out, xid);
	pq_sendint32(out, subxid);

	if (write_abort_info)
	{
		pq_sendint64(out, abort_lsn);
		pq_sendint64(out, abort_time);
	}
}

/*
 * 从输出流中读取 STREAM ABORT。    
 *
 * 若 read_abort_info 为真，读取 abort_lsn、abort_time 字段， 
 * 否则不读。 
 */
void
logicalrep_read_stream_abort(StringInfo in,
							 LogicalRepStreamAbortData *abort_data,
							 bool read_abort_info)
{
	Assert(abort_data);

	abort_data->xid = pq_getmsgint(in, 4);
	abort_data->subxid = pq_getmsgint(in, 4);

	if (read_abort_info)
	{
		abort_data->abort_lsn = pq_getmsgint64(in);
		abort_data->abort_time = pq_getmsgint64(in);
	}
	else
	{
		abort_data->abort_lsn = InvalidXLogRecPtr;
		abort_data->abort_time = 0;
	}
}

/*
 * 取得 LogicalRepMsgType 的字符串。  
 */
const char *
logicalrep_message_type(LogicalRepMsgType action)
{
	static char	err_unknown[20];

	switch (action)
	{
		case LOGICAL_REP_MSG_BEGIN:
			return "BEGIN";
		case LOGICAL_REP_MSG_COMMIT:
			return "COMMIT";
		case LOGICAL_REP_MSG_ORIGIN:
			return "ORIGIN";
		case LOGICAL_REP_MSG_INSERT:
			return "INSERT";
		case LOGICAL_REP_MSG_UPDATE:
			return "UPDATE";
		case LOGICAL_REP_MSG_DELETE:
			return "DELETE";
		case LOGICAL_REP_MSG_TRUNCATE:
			return "TRUNCATE";
		case LOGICAL_REP_MSG_RELATION:
			return "RELATION";
		case LOGICAL_REP_MSG_TYPE:
			return "TYPE";
		case LOGICAL_REP_MSG_MESSAGE:
			return "MESSAGE";
		case LOGICAL_REP_MSG_BEGIN_PREPARE:
			return "BEGIN PREPARE";
		case LOGICAL_REP_MSG_PREPARE:
			return "PREPARE";
		case LOGICAL_REP_MSG_COMMIT_PREPARED:
			return "COMMIT PREPARED";
		case LOGICAL_REP_MSG_ROLLBACK_PREPARED:
			return "ROLLBACK PREPARED";
		case LOGICAL_REP_MSG_STREAM_START:
			return "STREAM START";
		case LOGICAL_REP_MSG_STREAM_STOP:
			return "STREAM STOP";
		case LOGICAL_REP_MSG_STREAM_COMMIT:
			return "STREAM COMMIT";
		case LOGICAL_REP_MSG_STREAM_ABORT:
			return "STREAM ABORT";
		case LOGICAL_REP_MSG_STREAM_PREPARE:
			return "STREAM PREPARE";
	}

	/*
	 * 此消息为应用逻辑消息时引发错误提供上下文。  
	 * 因此这里无法抛出错误。请返回未知的指示值，    
	 * 以便仍能报告原始错误。                            
	 */
	snprintf(err_unknown, sizeof(err_unknown), "??? (%d)", action);

	return err_unknown;
}
