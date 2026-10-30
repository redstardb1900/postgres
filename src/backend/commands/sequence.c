/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  PostgreSQL 序列支持代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/sequence.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type.h"
#include "catalog/storage_xlog.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * 我们不想每次从序列中获取值时都记录 WAL，
 * 因此我们提前预记录少量获取。在崩溃时，
 * 我们可以丢失（跳过）与预记录数量相同的值。
 */
#define SEQ_LOG_VALS	32

/*
 * 序列缓冲区页面的"特殊区域"如下所示。
 */
#define SEQ_MAGIC	  0x1717

typedef struct sequence_magic
{
	uint32		magic;
} sequence_magic;

/*
 * 我们为当前会话中接触过的每个序列存储一个 SeqTable 条目。
 * 这是为了保持 nextval/currval 状态。
 * （我们不能依赖 relcache，因为它仅仅是一个缓存，
 * 可能会决定丢弃条目。）
 */
typedef struct SeqTableData
{
	Oid			relid;			/* 此序列的 pg_class OID（哈希键） */
	RelFileNumber filenumber;	/* 此序列最后一次看到的 relfilenumber */
	LocalTransactionId lxid;	/* 我们上次执行序列操作的事务 */
	bool		last_valid;		/* 我们是否有有效的 "last" 值？ */
	int64		last;			/* nextval 上次返回的值 */
	int64		cached;			/* 已为 nextval 缓存的最后一个值 */
	/* 如果 last != cached，则我们尚未用完所有缓存的值 */
	int64		increment;		/* 序列 increment 字段的副本 */
	/* 注意：increment 为零，直到我们首次调用 nextval_internal() */
} SeqTableData;

typedef SeqTableData *SeqTable;

static HTAB *seqhashtab = NULL; /* SeqTable 条目的哈希表 */

/*
 * last_used_seq 由 nextval() 更新，
 * 指向最后使用的序列。
 */
static SeqTableData *last_used_seq = NULL;

static void fill_seq_with_data(Relation rel, HeapTuple tuple);
static void fill_seq_fork_with_data(Relation rel, HeapTuple tuple, ForkNumber forkNum);
static Relation lock_and_open_sequence(SeqTable seq);
static void create_seq_hashtable(void);
static void init_sequence(Oid relid, SeqTable *p_elm, Relation *p_rel);
static Form_pg_sequence_data read_seq_tuple(Relation rel,
											Buffer *buf, HeapTuple seqdatatuple);
static void init_params(ParseState *pstate, List *options, bool for_identity,
						bool isInit,
						Form_pg_sequence seqform,
						Form_pg_sequence_data seqdataform,
						bool *need_seq_rewrite,
						List **owned_by);
static void do_setval(Oid relid, int64 next, bool iscalled);
static void process_owned_by(Relation seqrel, List *owned_by, bool for_identity);


/*
 * DefineSequence
 *				创建一个新的序列关系
 */
ObjectAddress
DefineSequence(ParseState *pstate, CreateSeqStmt *seq)
{
	FormData_pg_sequence seqform;
	FormData_pg_sequence_data seqdataform;
	bool		need_seq_rewrite;
	List	   *owned_by;
	CreateStmt *stmt = makeNode(CreateStmt);
	Oid			seqoid;
	ObjectAddress address;
	Relation	rel;
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Datum		value[SEQ_COL_LASTCOL];
	bool		null[SEQ_COL_LASTCOL];
	Datum		pgs_values[Natts_pg_sequence];
	bool		pgs_nulls[Natts_pg_sequence];
	int			i;

	/*
	 * 如果指定了 if_not_exists 且已存在同名关系，则退出。
	 * （注意：未指定 if_not_exists 时无需检查，因为
	 * DefineRelation 会自行报错。）
	 */
	if (seq->if_not_exists)
	{
		RangeVarGetAndCheckCreationNamespace(seq->sequence, NoLock, &seqoid);
		if (OidIsValid(seqoid))
		{
			/*
			 * 如果我们在扩展脚本中，要求预先存在的
			 * 对象是该扩展的成员，以避免安全风险。
			 */
			ObjectAddressSet(address, RelationRelationId, seqoid);
			checkMembershipInCurrentExtension(&address);

			/* 可以跳过 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists, skipping",
							seq->sequence->relname)));
			return InvalidObjectAddress;
		}
	}

	/* 检查并设置所有选项值 */
	init_params(pstate, seq->options, seq->for_identity, true,
				&seqform, &seqdataform,
				&need_seq_rewrite, &owned_by);

	/*
	 * 创建关系（并为元组填充 value[] 和 null[]）
	 */
	stmt->tableElts = NIL;
	for (i = SEQ_COL_FIRSTCOL; i <= SEQ_COL_LASTCOL; i++)
	{
		ColumnDef  *coldef = makeNode(ColumnDef);

		coldef->inhcount = 0;
		coldef->is_local = true;
		coldef->is_not_null = true;
		coldef->is_from_type = false;
		coldef->storage = 0;
		coldef->raw_default = NULL;
		coldef->cooked_default = NULL;
		coldef->collClause = NULL;
		coldef->collOid = InvalidOid;
		coldef->constraints = NIL;
		coldef->location = -1;

		null[i - 1] = false;

		switch (i)
		{
			case SEQ_COL_LASTVAL:
				coldef->typeName = makeTypeNameFromOid(INT8OID, -1);
				coldef->colname = "last_value";
				value[i - 1] = Int64GetDatumFast(seqdataform.last_value);
				break;
			case SEQ_COL_LOG:
				coldef->typeName = makeTypeNameFromOid(INT8OID, -1);
				coldef->colname = "log_cnt";
				value[i - 1] = Int64GetDatum((int64) 0);
				break;
			case SEQ_COL_CALLED:
				coldef->typeName = makeTypeNameFromOid(BOOLOID, -1);
				coldef->colname = "is_called";
				value[i - 1] = BoolGetDatum(false);
				break;
		}
		stmt->tableElts = lappend(stmt->tableElts, coldef);
	}

	stmt->relation = seq->sequence;
	stmt->inhRelations = NIL;
	stmt->constraints = NIL;
	stmt->options = NIL;
	stmt->oncommit = ONCOMMIT_NOOP;
	stmt->tablespacename = NULL;
	stmt->if_not_exists = seq->if_not_exists;

	address = DefineRelation(stmt, RELKIND_SEQUENCE, seq->ownerId, NULL, NULL);
	seqoid = address.objectId;
	Assert(seqoid != InvalidOid);

	rel = table_open(seqoid, AccessExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* 现在初始化序列的数据 */
	tuple = heap_form_tuple(tupDesc, value, null);
	fill_seq_with_data(rel, tuple);

	/* 如果指定了 OWNED BY，处理它 */
	if (owned_by)
		process_owned_by(rel, owned_by, seq->for_identity);

	table_close(rel, NoLock);

	/* 填充 pg_sequence */
	rel = table_open(SequenceRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	memset(pgs_nulls, 0, sizeof(pgs_nulls));

	pgs_values[Anum_pg_sequence_seqrelid - 1] = ObjectIdGetDatum(seqoid);
	pgs_values[Anum_pg_sequence_seqtypid - 1] = ObjectIdGetDatum(seqform.seqtypid);
	pgs_values[Anum_pg_sequence_seqstart - 1] = Int64GetDatumFast(seqform.seqstart);
	pgs_values[Anum_pg_sequence_seqincrement - 1] = Int64GetDatumFast(seqform.seqincrement);
	pgs_values[Anum_pg_sequence_seqmax - 1] = Int64GetDatumFast(seqform.seqmax);
	pgs_values[Anum_pg_sequence_seqmin - 1] = Int64GetDatumFast(seqform.seqmin);
	pgs_values[Anum_pg_sequence_seqcache - 1] = Int64GetDatumFast(seqform.seqcache);
	pgs_values[Anum_pg_sequence_seqcycle - 1] = BoolGetDatum(seqform.seqcycle);

	tuple = heap_form_tuple(tupDesc, pgs_values, pgs_nulls);
	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * 将序列重置为其初始值。
 *
 * 此更改以事务方式执行，因此如果当前事务失败，
 * 序列将恢复到其之前的状态。
 * 我们通过为序列创建全新的 relfilenumber 来实现这一点；因此
 * 这与 ALTER TABLE 的重写形式类似。
 *
 * 假定调用者已对序列获取了 AccessExclusiveLock，
 * 该锁必须持有到事务结束。调用者
 * 还负责权限检查。
 */
void
ResetSequence(Oid seq_relid)
{
	Relation	seq_rel;
	SeqTable	elm;
	Form_pg_sequence_data seq;
	Buffer		buf;
	HeapTupleData seqdatatuple;
	HeapTuple	tuple;
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	int64		startv;

	/*
	 * 读取旧序列。这比真正需要做的工作稍微多一些，
	 * 但简单明了，而且我们确实想要再次确认
	 * 它确实是序列。
	 */
	init_sequence(seq_relid, &elm, &seq_rel);
	(void) read_seq_tuple(seq_rel, &buf, &seqdatatuple);

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(seq_relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", seq_relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	startv = pgsform->seqstart;
	ReleaseSysCache(pgstuple);

	/*
	 * 复制现有的序列元组。
	 */
	tuple = heap_copytuple(&seqdatatuple);

	/* 现在我们已经完成了旧页面 */
	UnlockReleaseBuffer(buf);

	/*
	 * 修改复制后的元组以执行重启（与 AlterSequence 中的
	 * RESTART 操作对比）
	 */
	seq = (Form_pg_sequence_data) GETSTRUCT(tuple);
	seq->last_value = startv;
	seq->is_called = false;
	seq->log_cnt = 0;

	/*
	 * 为序列创建新的存储文件。
	 */
	RelationSetNewRelfilenumber(seq_rel, seq_rel->rd_rel->relpersistence);

	/*
	 * 确保序列的 relfrozenxid 为 0，因为序列不会包含
	 * 未冻结的 XID。relminmxid 同理，因为序列永远不会
	 * 包含 multixact。
	 */
	Assert(seq_rel->rd_rel->relfrozenxid == InvalidTransactionId);
	Assert(seq_rel->rd_rel->relminmxid == InvalidMultiXactId);

	/*
	 * 将修改后的元组插入新的存储文件。
	 */
	fill_seq_with_data(seq_rel, tuple);

	/* 清除本地缓存，这样我们就不会以为还有缓存的数字 */
	/* 注意：我们不改变 currval() 的状态 */
	elm->cached = elm->last;

	relation_close(seq_rel, NoLock);
}

/*
 * 用指定的元组初始化序列的关系
 *
 * 此函数通过根据需要写入主 fork 和 init fork
 * 来处理未记录日志的序列。
 */
static void
fill_seq_with_data(Relation rel, HeapTuple tuple)
{
	fill_seq_fork_with_data(rel, tuple, MAIN_FORKNUM);

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
	{
		SMgrRelation srel;

		srel = smgropen(rel->rd_locator, InvalidBackendId);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(&rel->rd_locator, INIT_FORKNUM);
		fill_seq_fork_with_data(rel, tuple, INIT_FORKNUM);
		FlushRelationBuffers(rel);
		smgrclose(srel);
	}
}

/*
 * 用指定的元组初始化序列的关系 fork
 */
static void
fill_seq_fork_with_data(Relation rel, HeapTuple tuple, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	sequence_magic *sm;
	OffsetNumber offnum;

	/* 用特殊魔术数字初始化关系的第一个页面 */

	buf = ExtendBufferedRel(BMR_REL(rel), forkNum, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(buf) == 0);

	page = BufferGetPage(buf);

	PageInit(page, BufferGetPageSize(buf), sizeof(sequence_magic));
	sm = (sequence_magic *) PageGetSpecialPointer(page);
	sm->magic = SEQ_MAGIC;

	/* 现在插入序列元组 */

	/*
	 * 由于 VACUUM 不处理序列，我们必须强制元组现在
	 * 就具有 xmin = FrozenTransactionId。否则经过 2G
	 * 事务后，它将对 SELECT 不可见。这样做是可以的，
	 * 因为如果当前事务中止，没有其他事务会检查
	 * 序列元组。
	 */
	HeapTupleHeaderSetXmin(tuple->t_data, FrozenTransactionId);
	HeapTupleHeaderSetXminFrozen(tuple->t_data);
	HeapTupleHeaderSetCmin(tuple->t_data, FirstCommandId);
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);
	tuple->t_data->t_infomask |= HEAP_XMAX_INVALID;
	ItemPointerSet(&tuple->t_data->t_ctid, 0, FirstOffsetNumber);

	/* 检查上面 nextval_internal() 等效调用的注释。 */
	if (RelationNeedsWAL(rel))
		GetTopTransactionId();

	START_CRIT_SECTION();

	MarkBufferDirty(buf);

	offnum = PageAddItem(page, (Item) tuple->t_data, tuple->t_len,
						 InvalidOffsetNumber, false, false);
	if (offnum != FirstOffsetNumber)
		elog(ERROR, "failed to add sequence tuple to page");

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel) || forkNum == INIT_FORKNUM)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);

		xlrec.locator = rel->rd_locator;

		XLogRegisterData((char *) &xlrec, sizeof(xl_seq_rec));
		XLogRegisterData((char *) tuple->t_data, tuple->t_len);

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
}

/*
 * AlterSequence
 *
 * 修改序列关系的定义
 */
ObjectAddress
AlterSequence(ParseState *pstate, AlterSeqStmt *stmt)
{
	Oid			relid;
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	HeapTupleData datatuple;
	Form_pg_sequence seqform;
	Form_pg_sequence_data newdataform;
	bool		need_seq_rewrite;
	List	   *owned_by;
	ObjectAddress address;
	Relation	rel;
	HeapTuple	seqtuple;
	HeapTuple	newdatatuple;

	/* 打开并锁定序列，并沿途检查所有权限。 */
	relid = RangeVarGetRelidExtended(stmt->sequence,
									 ShareRowExclusiveLock,
									 stmt->missing_ok ? RVR_MISSING_OK : 0,
									 RangeVarCallbackOwnsRelation,
									 NULL);
	if (relid == InvalidOid)
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->sequence->relname)));
		return InvalidObjectAddress;
	}

	init_sequence(relid, &elm, &seqrel);

	rel = table_open(SequenceRelationId, RowExclusiveLock);
	seqtuple = SearchSysCacheCopy1(SEQRELID,
								   ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(seqtuple))
		elog(ERROR, "cache lookup failed for sequence %u",
			 relid);

	seqform = (Form_pg_sequence) GETSTRUCT(seqtuple);

	/* 锁定页面缓冲区并将元组读入新的序列结构 */
	(void) read_seq_tuple(seqrel, &buf, &datatuple);

	/* 复制现有的序列数据元组，以便可以在本地修改 */
	newdatatuple = heap_copytuple(&datatuple);
	newdataform = (Form_pg_sequence_data) GETSTRUCT(newdatatuple);

	UnlockReleaseBuffer(buf);

	/* 检查并设置新值 */
	init_params(pstate, stmt->options, stmt->for_identity, false,
				seqform, newdataform,
				&need_seq_rewrite, &owned_by);

	/* 清除本地缓存，这样我们就不会以为还有缓存的数字 */
	/* 注意：我们不改变 currval() 的状态 */
	elm->cached = elm->last;

	/* 如果需要，重写序列关系本身 */
	if (need_seq_rewrite)
	{
		/* 检查上面 nextval_internal() 等效调用的注释。 */
		if (RelationNeedsWAL(seqrel))
			GetTopTransactionId();

		/*
		 * 为序列创建新的存储文件，使状态
		 * 更改具有事务性。
		 */
		RelationSetNewRelfilenumber(seqrel, seqrel->rd_rel->relpersistence);

		/*
		 * 确保序列的 relfrozenxid 为 0，因为序列不会包含
		 * 未冻结的 XID。relminmxid 同理，因为序列永远不会
		 * 包含 multixact。
		 */
		Assert(seqrel->rd_rel->relfrozenxid == InvalidTransactionId);
		Assert(seqrel->rd_rel->relminmxid == InvalidMultiXactId);

		/*
		 * 将修改后的元组插入新的存储文件。
		 */
		fill_seq_with_data(seqrel, newdatatuple);
	}

	/* 如果指定了 OWNED BY，处理它 */
	if (owned_by)
		process_owned_by(seqrel, owned_by, stmt->for_identity);

	/* 更新 pg_sequence 元组（某些情况下可以跳过...） */
	CatalogTupleUpdate(rel, &seqtuple->t_self, seqtuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	ObjectAddressSet(address, RelationRelationId, relid);

	table_close(rel, RowExclusiveLock);
	relation_close(seqrel, NoLock);

	return address;
}

void
SequenceChangePersistence(Oid relid, char newrelpersistence)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	HeapTupleData seqdatatuple;

	/*
	 * ALTER SEQUENCE 更早地获取此锁。如果我们正在为
	 * ALTER TABLE 处理已拥有的序列，现在获取锁。没有锁，
	 * 我们会丢弃在此函数的缓冲区解锁和此事务提交
	 * 之间的 nextval() 调用（其他会话中）的增量。
	 */
	LockRelationOid(relid, AccessExclusiveLock);
	init_sequence(relid, &elm, &seqrel);

	/* 检查上面 nextval_internal() 等效调用的注释。 */
	if (RelationNeedsWAL(seqrel))
		GetTopTransactionId();

	(void) read_seq_tuple(seqrel, &buf, &seqdatatuple);
	RelationSetNewRelfilenumber(seqrel, newrelpersistence);
	fill_seq_with_data(seqrel, &seqdatatuple);
	UnlockReleaseBuffer(buf);

	relation_close(seqrel, NoLock);
}

void
DeleteSequenceTuple(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;

	rel = table_open(SequenceRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);

	CatalogTupleDelete(rel, &tuple->t_self);

	ReleaseSysCache(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * 注意：带有文本参数的 nextval 不再作为 pg_proc
 * 条目导出，但我们保留它以便于移植可能直接
 * 调用了该函数的 C 代码。
 */
Datum
nextval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_PP(0);
	RangeVar   *sequence;
	Oid			relid;

	sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin));

	/*
	 * XXX：这在存在并发 DDL 时是不安全的，
	 * 但在这里获取锁比让 nextval_internal 去做更昂贵，
	 * 因为后者维护了一个缓存，
	 * 避免我们每个事务多次访问锁管理器。
	 * 尚不清楚性能惩罚在实践中是否重要，
	 * 但目前我们按此方式处理。
	 */
	relid = RangeVarGetRelid(sequence, NoLock, false);

	PG_RETURN_INT64(nextval_internal(relid, true));
}

Datum
nextval_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	PG_RETURN_INT64(nextval_internal(relid, true));
}

int64
nextval_internal(Oid relid, bool check_permissions)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	Page		page;
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	HeapTupleData seqdatatuple;
	Form_pg_sequence_data seq;
	int64		incby,
				maxv,
				minv,
				cache,
				log,
				fetch,
				last;
	int64		result,
				next,
				rescnt = 0;
	bool		cycle;
	bool		logit = false;

	/* 打开并锁定序列 */
	init_sequence(relid, &elm, &seqrel);

	if (check_permissions &&
		pg_class_aclcheck(elm->relid, GetUserId(),
						  ACL_USAGE | ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	/* 只读事务只能修改临时表序列 */
	if (!seqrel->rd_islocaltemp)
		PreventCommandIfReadOnly("nextval()");

	/*
	 * 在并行操作期间禁止此操作，因为要让它工作，
	 * 协作的后端需要共享后端本地的缓存序列信息。
	 * 目前，我们不支持这一点。
	 */
	PreventCommandIfParallelMode("nextval()");

	if (elm->last != elm->cached)	/* 一些数字被缓存了 */
	{
		Assert(elm->last_valid);
		Assert(elm->increment != 0);
		elm->last += elm->increment;
		relation_close(seqrel, NoLock);
		last_used_seq = elm;
		return elm->last;
	}

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	incby = pgsform->seqincrement;
	maxv = pgsform->seqmax;
	minv = pgsform->seqmin;
	cache = pgsform->seqcache;
	cycle = pgsform->seqcycle;
	ReleaseSysCache(pgstuple);

	/* 锁定页面缓冲区并读取元组 */
	seq = read_seq_tuple(seqrel, &buf, &seqdatatuple);
	page = BufferGetPage(buf);

	elm->increment = incby;
	last = next = result = seq->last_value;
	fetch = cache;
	log = seq->log_cnt;

	if (!seq->is_called)
	{
		rescnt++;				/* 如果未调用，则返回 last_value */
		fetch--;
	}

	/*
	 * 决定是否应发出 WAL 日志记录。如果是，强制提高
	 * 获取计数以抓取比我们实际需要缓存的更多 SEQ_LOG_VALS
	 * 个值。（这些值将可以无需记录日志地使用。）
	 *
	 * 如果这是某个检查点后的第一个 nextval，我们必须
	 * 强制写入一个新的 WAL 记录，否则从检查点开始的重放
	 * 将无法将序列推进到超过已记录的值。在这种情况下，
	 * 我们不妨多获取一些值。
	 */
	if (log < fetch || !seq->is_called)
	{
		/* 强制记录日志以满足本地对值的需求 */
		fetch = log = fetch + SEQ_LOG_VALS;
		logit = true;
	}
	else
	{
		XLogRecPtr	redoptr = GetRedoRecPtr();

		if (PageGetLSN(page) <= redoptr)
		{
			/* 序列的上次更新在检查点之前 */
			fetch = log = fetch + SEQ_LOG_VALS;
			logit = true;
		}
	}

	while (fetch)				/* 尝试获取缓存 [ + 日志] 数字 */
	{
		/*
		 * 检查递增序列的 MAXVALUE
		 * 和递减序列的 MINVALUE
		 */
		if (incby > 0)
		{
			/* 递增序列 */
			if ((maxv >= 0 && next > maxv - incby) ||
				(maxv < 0 && next + incby > maxv))
			{
				if (rescnt > 0)
					break;		/* 停止获取 */
				if (!cycle)
					ereport(ERROR,
							(errcode(ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED),
							 errmsg("nextval: reached maximum value of sequence \"%s\" (%lld)",
									RelationGetRelationName(seqrel),
									(long long) maxv)));
				next = minv;
			}
			else
				next += incby;
		}
		else
		{
			/* 递减序列 */
			if ((minv < 0 && next < minv - incby) ||
				(minv >= 0 && next + incby < minv))
			{
				if (rescnt > 0)
					break;		/* 停止获取 */
				if (!cycle)
					ereport(ERROR,
							(errcode(ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED),
							 errmsg("nextval: reached minimum value of sequence \"%s\" (%lld)",
									RelationGetRelationName(seqrel),
									(long long) minv)));
				next = maxv;
			}
			else
				next += incby;
		}
		fetch--;
		if (rescnt < cache)
		{
			log--;
			rescnt++;
			last = next;
			if (rescnt == 1)	/* 如果是第一个结果 - */
				result = next;	/* 这就是要返回的 */
		}
	}

	log -= fetch;				/* 调整任何未获取的数字 */
	Assert(log >= 0);

	/* 保存信息到本地缓存 */
	elm->last = result;			/* 最后返回的数字 */
	elm->cached = last;			/* 最后获取的数字 */
	elm->last_valid = true;

	last_used_seq = elm;

	/*
	 * 如果有东西需要写入 WAL 日志，则获取一个 XID，
	 * 以便此事务的提交将触发 WAL 刷新并等待同步复制。
	 * 只需确保顶层事务有一个 XID 即可，无需为子事务
	 * 分配 XID，它们已经会触发适当的等待。
	 * （必须在这里执行，这样我们就在临界区之外）
	 */
	if (logit && RelationNeedsWAL(seqrel))
		GetTopTransactionId();

	/* 准备更改磁盘（或者说，缓冲区内）的元组 */
	START_CRIT_SECTION();

	/*
	 * 我们必须在执行 XLogInsert() 之前将缓冲区标记为脏；
	 * 请参阅 SyncOneBuffer() 中的注释。
	 * 但我们暂不应用期望的更改。
	 * 这看起来违反了缓冲区更新协议，但实际上它是安全的，
	 * 因为我们持有缓冲区的排他锁。任何其他进程，
	 * 包括检查点，尝试检查缓冲区内容时将被阻塞，
	 * 直到我们释放锁，然后会看到我们下面安装的最终状态。
	 */
	MarkBufferDirty(buf);

	/* XLOG 相关 */
	if (logit && RelationNeedsWAL(seqrel))
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;

		/*
		 * 我们不记录元组的当前状态，而是记录"log"次更多获取
		 * 后的状态。这样我们可以跳过那么多未来的 WAL 记录，
		 * 代价是如果崩溃，我们会丢失这些
		 * 序列值。
		 */
		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);

		/* 设置将保存在 xlog 中的值 */
		seq->last_value = next;
		seq->is_called = true;
		seq->log_cnt = 0;

		xlrec.locator = seqrel->rd_locator;

		XLogRegisterData((char *) &xlrec, sizeof(xl_seq_rec));
		XLogRegisterData((char *) seqdatatuple.t_data, seqdatatuple.t_len);

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG);

		PageSetLSN(page, recptr);
	}

	/* 现在将序列元组更新为预期的最终状态 */
	seq->last_value = last;		/* 最后获取的数字 */
	seq->is_called = true;
	seq->log_cnt = log;			/* 已记录日志的数量 */

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);

	relation_close(seqrel, NoLock);

	return result;
}

Datum
currval_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	SeqTable	elm;
	Relation	seqrel;

	/* 打开并锁定序列 */
	init_sequence(relid, &elm, &seqrel);

	if (pg_class_aclcheck(elm->relid, GetUserId(),
						  ACL_SELECT | ACL_USAGE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	if (!elm->last_valid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("currval of sequence \"%s\" is not yet defined in this session",
						RelationGetRelationName(seqrel))));

	result = elm->last;

	relation_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}

Datum
lastval(PG_FUNCTION_ARGS)
{
	Relation	seqrel;
	int64		result;

	if (last_used_seq == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lastval is not yet defined in this session")));

	/* 自上次 nextval() 以来，可能有人已删除了该序列 */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(last_used_seq->relid)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lastval is not yet defined in this session")));

	seqrel = lock_and_open_sequence(last_used_seq);

	/* 必须已为此序列调用过 nextval() */
	Assert(last_used_seq->last_valid);

	if (pg_class_aclcheck(last_used_seq->relid, GetUserId(),
						  ACL_SELECT | ACL_USAGE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	result = last_used_seq->last;
	relation_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}

/*
 * 处理 SETVAL 的 2 参数和 3 参数形式的主要内部过程。
 *
 * 注意，3 参数版本（设置 is_called 标志）仅供 pg_dump
 * 使用，如果有多个用户连接到数据库并引用该序列，
 * 设置 is_called 标志可能不会生效
 * （如果 pg_dump 正在恢复它，这种情况不太可能）。
 *
 * 有必要提供 3 参数版本，以便 pg_dump 可以在仅数据恢复期间
 * 精确地恢复序列的状态 — 这是清除现有序列中
 * is_called 标志的
 * 唯一方法。
 */
static void
do_setval(Oid relid, int64 next, bool iscalled)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	HeapTupleData seqdatatuple;
	Form_pg_sequence_data seq;
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	int64		maxv,
				minv;

	/* 打开并锁定序列 */
	init_sequence(relid, &elm, &seqrel);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	maxv = pgsform->seqmax;
	minv = pgsform->seqmin;
	ReleaseSysCache(pgstuple);

	/* 只读事务只能修改临时表序列 */
	if (!seqrel->rd_islocaltemp)
		PreventCommandIfReadOnly("setval()");

	/*
	 * 在并行操作期间禁止此操作，因为要让它工作，
	 * 协作的后端需要共享后端本地的缓存序列信息。
	 * 目前，我们不支持这一点。
	 */
	PreventCommandIfParallelMode("setval()");

	/* 锁定页面缓冲区并读取元组 */
	seq = read_seq_tuple(seqrel, &buf, &seqdatatuple);

	if ((next < minv) || (next > maxv))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("setval: value %lld is out of bounds for sequence \"%s\" (%lld..%lld)",
						(long long) next, RelationGetRelationName(seqrel),
						(long long) minv, (long long) maxv)));

	/* 仅当 iscalled = true 时设置 currval() 状态 */
	if (iscalled)
	{
		elm->last = next;		/* 最后返回的数字 */
		elm->last_valid = true;
	}

	/* 无论如何，忘记任何将来缓存的数字 */
	elm->cached = elm->last;

	/* 检查上面 nextval_internal() 等效调用的注释。 */
	if (RelationNeedsWAL(seqrel))
		GetTopTransactionId();

	/* 准备更改磁盘（或者说，缓冲区内）的元组 */
	START_CRIT_SECTION();

	seq->last_value = next;		/* 最后获取的数字 */
	seq->is_called = iscalled;
	seq->log_cnt = 0;

	MarkBufferDirty(buf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(seqrel))
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		Page		page = BufferGetPage(buf);

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);

		xlrec.locator = seqrel->rd_locator;
		XLogRegisterData((char *) &xlrec, sizeof(xl_seq_rec));
		XLogRegisterData((char *) seqdatatuple.t_data, seqdatatuple.t_len);

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);

	relation_close(seqrel, NoLock);
}

/*
 * 实现 2 参数 setval 过程。
 * 请参阅 do_setval 中的讨论。
 */
Datum
setval_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);

	do_setval(relid, next, true);

	PG_RETURN_INT64(next);
}

/*
 * 实现 3 参数 setval 过程。
 * 请参阅 do_setval 中的讨论。
 */
Datum
setval3_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);
	bool		iscalled = PG_GETARG_BOOL(2);

	do_setval(relid, next, iscalled);

	PG_RETURN_INT64(next);
}


/*
 * 打开序列并在需要时获取锁
 *
 * 如果我们在此事务中尚未触碰该序列，
 * 则需要获取锁。我们安排让顶层事务拥有该锁，
 * 以便每个事务
 * 只需执行一次。
 */
static Relation
lock_and_open_sequence(SeqTable seq)
{
	LocalTransactionId thislxid = MyProc->lxid;

	/* 如果尚未在此事务中持有，则获取锁 */
	if (seq->lxid != thislxid)
	{
		ResourceOwner currentOwner;

		currentOwner = CurrentResourceOwner;
		CurrentResourceOwner = TopTransactionResourceOwner;

		LockRelationOid(seq->relid, RowExclusiveLock);

		CurrentResourceOwner = currentOwner;

		/* 标记我们在当前事务中持有锁 */
		seq->lxid = thislxid;
	}

	/* 我们现在知道我们持有锁，可以安全地打开关系 */
	return relation_open(seq->relid, NoLock);
}

/*
 * 创建用于存储序列数据的哈希表
 */
static void
create_seq_hashtable(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(SeqTableData);

	seqhashtab = hash_create("Sequence values", 16, &ctl,
							 HASH_ELEM | HASH_BLOBS);
}

/*
 * 给定关系 OID，打开并锁定序列。p_elm 和 p_rel 是
 * 输出参数。
 */
static void
init_sequence(Oid relid, SeqTable *p_elm, Relation *p_rel)
{
	SeqTable	elm;
	Relation	seqrel;
	bool		found;

	/* 查找或创建此序列的哈希表条目 */
	if (seqhashtab == NULL)
		create_seq_hashtable();

	elm = (SeqTable) hash_search(seqhashtab, &relid, HASH_ENTER, &found);

	/*
	 * 如果哈希表条目尚不存在，初始化它。
	 *
	 * 注意：seqhashtab 条目在后端的生命周期内保存
	 * （除非通过 DISCARD 显式丢弃）。如果序列本身被删除，
	 * 则条目变为浪费的内存，但它足够小，
	 * 应该不会有问题。
	 */
	if (!found)
	{
		/* relid 已填充 */
		elm->filenumber = InvalidRelFileNumber;
		elm->lxid = InvalidLocalTransactionId;
		elm->last_valid = false;
		elm->last = elm->cached = 0;
	}

	/*
	 * 打开序列关系。
	 */
	seqrel = lock_and_open_sequence(elm);

	if (seqrel->rd_rel->relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						RelationGetRelationName(seqrel))));

	/*
	 * 如果自上次查看以来序列已被事务性替换，
	 * 丢弃任何已缓存但尚未分发的值。但我们不会
	 * 触碰 currval() 状态。
	 */
	if (seqrel->rd_rel->relfilenode != elm->filenumber)
	{
		elm->filenumber = seqrel->rd_rel->relfilenode;
		elm->cached = elm->last;
	}

	/* 返回结果 */
	*p_elm = elm;
	*p_rel = seqrel;
}


/*
 * 给定已打开的序列关系，锁定页面缓冲区并查找元组
 *
 * *buf 接收已固定且排他锁定的缓冲区引用
 * *seqdatatuple 接收序列元组本身的引用
 *		（此参数应指向类型为 HeapTupleData 的本地变量）
 *
 * 函数返回值指向元组的数据负载
 */
static Form_pg_sequence_data
read_seq_tuple(Relation rel, Buffer *buf, HeapTuple seqdatatuple)
{
	Page		page;
	ItemId		lp;
	sequence_magic *sm;
	Form_pg_sequence_data seq;

	*buf = ReadBuffer(rel, 0);
	LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(*buf);
	sm = (sequence_magic *) PageGetSpecialPointer(page);

	if (sm->magic != SEQ_MAGIC)
		elog(ERROR, "bad magic number in sequence \"%s\": %08X",
			 RelationGetRelationName(rel), sm->magic);

	lp = PageGetItemId(page, FirstOffsetNumber);
	Assert(ItemIdIsNormal(lp));

	/* 注意：我们目前只设置 *seqdatatuple 的这两个字段 */
	seqdatatuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	seqdatatuple->t_len = ItemIdGetLength(lp);

	/*
	 * 以前版本的 Postgres 忽略了防止对序列执行 SELECT FOR UPDATE，
	 * 这会在序列元组的 xmax 中留下非冻结的 XID，
	 * 最终导致 clog 访问失败或更糟的问题。如果我们
	 * 发现这种情况，则清理它。我们将其视为提示位更新，
	 * 即不记录 WAL 日志，因为如果此更新丢失，
	 * 我们可以再次执行。
	 */
	Assert(!(seqdatatuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI));
	if (HeapTupleHeaderGetRawXmax(seqdatatuple->t_data) != InvalidTransactionId)
	{
		HeapTupleHeaderSetXmax(seqdatatuple->t_data, InvalidTransactionId);
		seqdatatuple->t_data->t_infomask &= ~HEAP_XMAX_COMMITTED;
		seqdatatuple->t_data->t_infomask |= HEAP_XMAX_INVALID;
		MarkBufferDirtyHint(*buf, true);
	}

	seq = (Form_pg_sequence_data) GETSTRUCT(seqdatatuple);

	return seq;
}

/*
 * init_params: 处理 CREATE 或 ALTER SEQUENCE 的选项列表，并
 * 将值存储到 seqform 的适当字段中（对于进入 pg_sequence
 * 系统目录的更改），以及 seqdataform 的字段中（对于序列
 * 关系本身的更改）。如果我们更改了任何需要重写序列关系的
 * 参数，将 *need_seq_rewrite 设置为 true（对 ALTER SEQUENCE
 * 有用）。同时将 *owned_by 设置为任何 OWNED BY 选项，
 * 或 NIL 如果没有。
 *
 * 如果 isInit 为 true，用默认值填充任何未指定的选项；
 * 否则，不更改未显式覆盖的现有选项。
 *
 * 注意：只要我们更改了影响未来序列值生成的参数，
 * 我们就会强制重写序列，即使 seqdataform 本身未被更改。
 * 这允许 ALTER SEQUENCE 以事务方式行为。目前，
 * 不会触发重写的唯一选项是 OWNED BY。
 * ALTER SEQUENCE OWNED BY 不重写序列是必要的，
 * 因为这会导致序列 relfilenumber 的不必要更改，
 * 从而破坏 pg_upgrade。
 */
static void
init_params(ParseState *pstate, List *options, bool for_identity,
			bool isInit,
			Form_pg_sequence seqform,
			Form_pg_sequence_data seqdataform,
			bool *need_seq_rewrite,
			List **owned_by)
{
	DefElem    *as_type = NULL;
	DefElem    *start_value = NULL;
	DefElem    *restart_value = NULL;
	DefElem    *increment_by = NULL;
	DefElem    *max_value = NULL;
	DefElem    *min_value = NULL;
	DefElem    *cache_value = NULL;
	DefElem    *is_cycled = NULL;
	ListCell   *option;
	bool		reset_max_value = false;
	bool		reset_min_value = false;

	*need_seq_rewrite = false;
	*owned_by = NIL;

	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_type)
				errorConflictingDefElem(defel, pstate);
			as_type = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "increment") == 0)
		{
			if (increment_by)
				errorConflictingDefElem(defel, pstate);
			increment_by = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "start") == 0)
		{
			if (start_value)
				errorConflictingDefElem(defel, pstate);
			start_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "restart") == 0)
		{
			if (restart_value)
				errorConflictingDefElem(defel, pstate);
			restart_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "maxvalue") == 0)
		{
			if (max_value)
				errorConflictingDefElem(defel, pstate);
			max_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "minvalue") == 0)
		{
			if (min_value)
				errorConflictingDefElem(defel, pstate);
			min_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "cache") == 0)
		{
			if (cache_value)
				errorConflictingDefElem(defel, pstate);
			cache_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "cycle") == 0)
		{
			if (is_cycled)
				errorConflictingDefElem(defel, pstate);
			is_cycled = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "owned_by") == 0)
		{
			if (*owned_by)
				errorConflictingDefElem(defel, pstate);
			*owned_by = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "sequence_name") == 0)
		{
			/*
			 * 解析器允许此选项，但它仅用于标识列，
			 * 在这种情况下它会在 parse_utilcmd.c 中被过滤掉。我们
			 * 只会在有人将其放入 CREATE SEQUENCE 时到达这里，
			 * 在这种情况下它是冗余的。（同样非标准的
			 * LOGGED 和 UNLOGGED 选项也是如此，但对于它们，
			 * 下面的默认错误似乎已经足够。）
			 */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid sequence option SEQUENCE NAME"),
					 parser_errposition(pstate, defel->location)));
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * 当 isInit 或更改任何会影响未来 nextval 分配的
	 * 参数时，我们必须重置 log_cnt。
	 */
	if (isInit)
		seqdataform->log_cnt = 0;

	/* AS 类型 */
	if (as_type != NULL)
	{
		Oid			newtypid = typenameTypeId(pstate, defGetTypeName(as_type));

		if (newtypid != INT2OID &&
			newtypid != INT4OID &&
			newtypid != INT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 for_identity
					 ? errmsg("identity column type must be smallint, integer, or bigint")
					 : errmsg("sequence type must be smallint, integer, or bigint")));

		if (!isInit)
		{
			/*
			 * 当更改类型且旧序列 min/max 值是旧类型的
			 * min/max 时，将序列 min/max 值调整为新类型的
			 * min/max。（否则，用户选择了显式的 min/max 值，
			 * 我们保持不变。）
			 */
			if ((seqform->seqtypid == INT2OID && seqform->seqmax == PG_INT16_MAX) ||
				(seqform->seqtypid == INT4OID && seqform->seqmax == PG_INT32_MAX) ||
				(seqform->seqtypid == INT8OID && seqform->seqmax == PG_INT64_MAX))
				reset_max_value = true;
			if ((seqform->seqtypid == INT2OID && seqform->seqmin == PG_INT16_MIN) ||
				(seqform->seqtypid == INT4OID && seqform->seqmin == PG_INT32_MIN) ||
				(seqform->seqtypid == INT8OID && seqform->seqmin == PG_INT64_MIN))
				reset_min_value = true;
		}

		seqform->seqtypid = newtypid;
	}
	else if (isInit)
	{
		seqform->seqtypid = INT8OID;
	}

	/* INCREMENT BY */
	if (increment_by != NULL)
	{
		seqform->seqincrement = defGetInt64(increment_by);
		if (seqform->seqincrement == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("INCREMENT must not be zero")));
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqform->seqincrement = 1;
	}

	/* CYCLE */
	if (is_cycled != NULL)
	{
		seqform->seqcycle = boolVal(is_cycled->arg);
		Assert(BoolIsValid(seqform->seqcycle));
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqform->seqcycle = false;
	}

	/* MAXVALUE（null 参数表示 NO MAXVALUE） */
	if (max_value != NULL && max_value->arg)
	{
		seqform->seqmax = defGetInt64(max_value);
		seqdataform->log_cnt = 0;
	}
	else if (isInit || max_value != NULL || reset_max_value)
	{
		if (seqform->seqincrement > 0 || reset_max_value)
		{
			/* 递增序列 */
			if (seqform->seqtypid == INT2OID)
				seqform->seqmax = PG_INT16_MAX;
			else if (seqform->seqtypid == INT4OID)
				seqform->seqmax = PG_INT32_MAX;
			else
				seqform->seqmax = PG_INT64_MAX;
		}
		else
			seqform->seqmax = -1;	/* 递减序列 */
		seqdataform->log_cnt = 0;
	}

	/* 验证最大值。无需检查 INT8，因为 seqmax 是 int64 */
	if ((seqform->seqtypid == INT2OID && (seqform->seqmax < PG_INT16_MIN || seqform->seqmax > PG_INT16_MAX))
		|| (seqform->seqtypid == INT4OID && (seqform->seqmax < PG_INT32_MIN || seqform->seqmax > PG_INT32_MAX)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("MAXVALUE (%lld) is out of range for sequence data type %s",
						(long long) seqform->seqmax,
						format_type_be(seqform->seqtypid))));

	/* MINVALUE（null 参数表示 NO MINVALUE） */
	if (min_value != NULL && min_value->arg)
	{
		seqform->seqmin = defGetInt64(min_value);
		seqdataform->log_cnt = 0;
	}
	else if (isInit || min_value != NULL || reset_min_value)
	{
		if (seqform->seqincrement < 0 || reset_min_value)
		{
			/* 递减序列 */
			if (seqform->seqtypid == INT2OID)
				seqform->seqmin = PG_INT16_MIN;
			else if (seqform->seqtypid == INT4OID)
				seqform->seqmin = PG_INT32_MIN;
			else
				seqform->seqmin = PG_INT64_MIN;
		}
		else
			seqform->seqmin = 1;	/* 递增序列 */
		seqdataform->log_cnt = 0;
	}

	/* 验证最小值。无需检查 INT8，因为 seqmin 是 int64 */
	if ((seqform->seqtypid == INT2OID && (seqform->seqmin < PG_INT16_MIN || seqform->seqmin > PG_INT16_MAX))
		|| (seqform->seqtypid == INT4OID && (seqform->seqmin < PG_INT32_MIN || seqform->seqmin > PG_INT32_MAX)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("MINVALUE (%lld) is out of range for sequence data type %s",
						(long long) seqform->seqmin,
						format_type_be(seqform->seqtypid))));

	/* 交叉检查 min/max */
	if (seqform->seqmin >= seqform->seqmax)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("MINVALUE (%lld) must be less than MAXVALUE (%lld)",
						(long long) seqform->seqmin,
						(long long) seqform->seqmax)));

	/* START WITH */
	if (start_value != NULL)
	{
		seqform->seqstart = defGetInt64(start_value);
	}
	else if (isInit)
	{
		if (seqform->seqincrement > 0)
			seqform->seqstart = seqform->seqmin;	/* 递增序列 */
		else
			seqform->seqstart = seqform->seqmax;	/* 递减序列 */
	}

	/* 交叉检查 START */
	if (seqform->seqstart < seqform->seqmin)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("START value (%lld) cannot be less than MINVALUE (%lld)",
						(long long) seqform->seqstart,
						(long long) seqform->seqmin)));
	if (seqform->seqstart > seqform->seqmax)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("START value (%lld) cannot be greater than MAXVALUE (%lld)",
						(long long) seqform->seqstart,
						(long long) seqform->seqmax)));

	/* RESTART [WITH] */
	if (restart_value != NULL)
	{
		if (restart_value->arg != NULL)
			seqdataform->last_value = defGetInt64(restart_value);
		else
			seqdataform->last_value = seqform->seqstart;
		seqdataform->is_called = false;
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqdataform->last_value = seqform->seqstart;
		seqdataform->is_called = false;
	}

	/* 交叉检查 RESTART（或当前值，如果更改 MIN/MAX） */
	if (seqdataform->last_value < seqform->seqmin)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("RESTART value (%lld) cannot be less than MINVALUE (%lld)",
						(long long) seqdataform->last_value,
						(long long) seqform->seqmin)));
	if (seqdataform->last_value > seqform->seqmax)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("RESTART value (%lld) cannot be greater than MAXVALUE (%lld)",
						(long long) seqdataform->last_value,
						(long long) seqform->seqmax)));

	/* CACHE */
	if (cache_value != NULL)
	{
		seqform->seqcache = defGetInt64(cache_value);
		if (seqform->seqcache <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("CACHE (%lld) must be greater than zero",
							(long long) seqform->seqcache)));
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqform->seqcache = 1;
	}
}

/*
 * 处理 CREATE/ALTER SEQUENCE 的 OWNED BY 选项
 *
 * 序列的所有权限已经检查，
 * 但如果我们正在建立新的 owned-by 依赖关系，
 * 我们必须确保引用表有相同的所有者和模式
 * 与序列相同。
 */
static void
process_owned_by(Relation seqrel, List *owned_by, bool for_identity)
{
	DependencyType deptype;
	int			nnames;
	Relation	tablerel;
	AttrNumber	attnum;

	deptype = for_identity ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO;

	nnames = list_length(owned_by);
	Assert(nnames > 0);
	if (nnames == 1)
	{
		/* 必须是 OWNED BY NONE */
		if (strcmp(strVal(linitial(owned_by)), "none") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid OWNED BY option"),
					 errhint("Specify OWNED BY table.column or OWNED BY NONE.")));
		tablerel = NULL;
		attnum = 0;
	}
	else
	{
		List	   *relname;
		char	   *attrname;
		RangeVar   *rel;

		/* 分离 relname 和 attr 名 */
		relname = list_copy_head(owned_by, nnames - 1);
		attrname = strVal(llast(owned_by));

		/* 打开并锁定 rel 以确保它不会在此期间消失 */
		rel = makeRangeVarFromNameList(relname);
		tablerel = relation_openrv(rel, AccessShareLock);

		/* 必须是普通表或外部表 */
		if (!(tablerel->rd_rel->relkind == RELKIND_RELATION ||
			  tablerel->rd_rel->relkind == RELKIND_FOREIGN_TABLE ||
			  tablerel->rd_rel->relkind == RELKIND_VIEW ||
			  tablerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("sequence cannot be owned by relation \"%s\"",
							RelationGetRelationName(tablerel)),
					 errdetail_relkind_not_supported(tablerel->rd_rel->relkind)));

		/* 我们要求相同的所有者和模式 */
		if (seqrel->rd_rel->relowner != tablerel->rd_rel->relowner)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("sequence must have same owner as table it is linked to")));
		if (RelationGetNamespace(seqrel) != RelationGetNamespace(tablerel))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("sequence must be in same schema as table it is linked to")));

		/* 现在，从系统缓存中获取属性号 */
		attnum = get_attnum(RelationGetRelid(tablerel), attrname);
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							attrname, RelationGetRelationName(tablerel))));
	}

	/*
	 * 捕获用户显式对标识序列执行 OWNED BY 的情况。
	 */
	if (deptype == DEPENDENCY_AUTO)
	{
		Oid			tableId;
		int32		colId;

		if (sequenceIsOwned(RelationGetRelid(seqrel), DEPENDENCY_INTERNAL, &tableId, &colId))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot change ownership of identity sequence"),
					 errdetail("Sequence \"%s\" is linked to table \"%s\".",
							   RelationGetRelationName(seqrel),
							   get_rel_name(tableId))));
	}

	/*
	 * 好的，我们准备更新 pg_depend。首先删除序列
	 * 的任何现有依赖关系，然后可选地添加新的。
	 */
	deleteDependencyRecordsForClass(RelationRelationId, RelationGetRelid(seqrel),
									RelationRelationId, deptype);

	if (tablerel)
	{
		ObjectAddress refobject,
					depobject;

		refobject.classId = RelationRelationId;
		refobject.objectId = RelationGetRelid(tablerel);
		refobject.objectSubId = attnum;
		depobject.classId = RelationRelationId;
		depobject.objectId = RelationGetRelid(seqrel);
		depobject.objectSubId = 0;
		recordDependencyOn(&depobject, &refobject, deptype);
	}

	/* 完成，但保持锁直到提交 */
	if (tablerel)
		relation_close(tablerel, NoLock);
}


/*
 * 以解析器创建的列表格式返回序列参数。
 */
List *
sequence_options(Oid relid)
{
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	List	   *options = NIL;

	pgstuple = SearchSysCache1(SEQRELID, relid);
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);

	/* 对 64 位整数使用 makeFloat()，如 gram.y 所做的。 */
	options = lappend(options,
					  makeDefElem("cache", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqcache)), -1));
	options = lappend(options,
					  makeDefElem("cycle", (Node *) makeBoolean(pgsform->seqcycle), -1));
	options = lappend(options,
					  makeDefElem("increment", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqincrement)), -1));
	options = lappend(options,
					  makeDefElem("maxvalue", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqmax)), -1));
	options = lappend(options,
					  makeDefElem("minvalue", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqmin)), -1));
	options = lappend(options,
					  makeDefElem("start", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqstart)), -1));

	ReleaseSysCache(pgstuple);

	return options;
}

/*
 * 返回序列参数（以前供 information schema 使用）
 */
Datum
pg_sequence_parameters(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		isnull[7];
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT | ACL_UPDATE | ACL_USAGE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						get_rel_name(relid))));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(isnull, 0, sizeof(isnull));

	pgstuple = SearchSysCache1(SEQRELID, relid);
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);

	values[0] = Int64GetDatum(pgsform->seqstart);
	values[1] = Int64GetDatum(pgsform->seqmin);
	values[2] = Int64GetDatum(pgsform->seqmax);
	values[3] = Int64GetDatum(pgsform->seqincrement);
	values[4] = BoolGetDatum(pgsform->seqcycle);
	values[5] = Int64GetDatum(pgsform->seqcache);
	values[6] = ObjectIdGetDatum(pgsform->seqtypid);

	ReleaseSysCache(pgstuple);

	return HeapTupleGetDatum(heap_form_tuple(tupdesc, values, isnull));
}

/*
 * 从序列返回最后一个值
 *
 * 注意：这与 lastval() 的含义完全不同。
 */
Datum
pg_sequence_last_value(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	SeqTable	elm;
	Relation	seqrel;
	bool		is_called = false;
	int64		result = 0;

	/* 打开并锁定序列 */
	init_sequence(relid, &elm, &seqrel);

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT | ACL_USAGE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	/*
	 * 对于其他会话的临时序列，我们返回 NULL。
	 * pg_sequences 系统视图已经过滤掉了它们，但这可以
	 * 在有人直接调用此函数时防止 ERROR。
	 *
	 * 同样，为了方便 pg_sequences 视图，对于备机上的
	 * 未记录日志序列，我们返回 NULL 而不是抛出错误。
	 */
	if (!RELATION_IS_OTHER_TEMP(seqrel) &&
		(RelationIsPermanent(seqrel) || !RecoveryInProgress()))
	{
		Buffer		buf;
		HeapTupleData seqtuple;
		Form_pg_sequence_data seq;

		seq = read_seq_tuple(seqrel, &buf, &seqtuple);

		is_called = seq->is_called;
		result = seq->last_value;

		UnlockReleaseBuffer(buf);
	}
	relation_close(seqrel, NoLock);

	if (is_called)
		PG_RETURN_INT64(result);
	else
		PG_RETURN_NULL();
}


void
seq_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	Buffer		buffer;
	Page		page;
	Page		localpage;
	char	   *item;
	Size		itemsz;
	xl_seq_rec *xlrec = (xl_seq_rec *) XLogRecGetData(record);
	sequence_magic *sm;

	if (info != XLOG_SEQ_LOG)
		elog(PANIC, "seq_redo: unknown op code %u", info);

	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	/*
	 * 我们始终重新初始化页面。但由于此 WAL 记录类型也
	 * 用于更新序列，可能有热备用后端同时检查页面；
	 * 因此我们不能短暂地破坏缓冲区。解决方案是在
	 * 本地工作区中构建正确的新页面内容，然后 memcpy
	 * 到缓冲区中。这样，即使是短暂的，只有
	 * 应该更改的字节会更改。我们必须 palloc
	 * 本地页面以确保对齐。
	 */
	localpage = (Page) palloc(BufferGetPageSize(buffer));

	PageInit(localpage, BufferGetPageSize(buffer), sizeof(sequence_magic));
	sm = (sequence_magic *) PageGetSpecialPointer(localpage);
	sm->magic = SEQ_MAGIC;

	item = (char *) xlrec + sizeof(xl_seq_rec);
	itemsz = XLogRecGetDataLen(record) - sizeof(xl_seq_rec);

	if (PageAddItem(localpage, (Item) item, itemsz,
					FirstOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(PANIC, "seq_redo: failed to add item to page");

	PageSetLSN(localpage, lsn);

	memcpy(page, localpage, BufferGetPageSize(buffer));
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	pfree(localpage);
}

/*
 * 刷新缓存的序列信息。
 */
void
ResetSequenceCaches(void)
{
	if (seqhashtab)
	{
		hash_destroy(seqhashtab);
		seqhashtab = NULL;
	}

	last_used_seq = NULL;
}

/*
 * 在对 Sequence 页面执行一致性检查之前将其遮罩。
 */
void
seq_mask(char *page, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(page);

	mask_unused_space(page);
}
