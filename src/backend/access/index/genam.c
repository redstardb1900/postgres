/*-------------------------------------------------------------------------
 *
 * genam.c
 *	  通用索引访问方法例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/genam.c
 *
 * 说明（NOTES）
 *	  许多旧的访问方法例程已改成
 *	  宏并移到 genam.h -cim 4/30/91
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "catalog/index.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* ----------------------------------------------------------------
 *		通用访问方法例程
 *
 *		所有索引访问方法使用相同的扫描结构。
 *		不过我们不知道各个 AM 如何加锁，因此这里
 *		不做任何加锁处理。
 *
 *		意图是 AM 实现者定义一个 beginscan 例程，
 *		它调用 RelationGetIndexScan 填好扫描，然后再做
 *		它所要的那种加锁。
 *
 *		扫描结束时，AM 的 endscan 例程撤销加锁，
 *		但 *不* 调用 IndexScanEnd --- 更高层的 index_endscan
 *		例程才做这件事。（不能在 AM 里做，因为 index_endscan
 *		在调用 AM 之后仍需触碰 IndexScanDesc。）
 *
 *		因此 AM 无权选择是否调用
 *		RelationGetIndexScan；其 beginscan 例程必须返回
 *		由 RelationGetIndexScan 构造的对象。这有点难看，但
 *		现在不值得清理。
 * ----------------------------------------------------------------
 */

/* ----------------
 *	RelationGetIndexScan -- 创建并填写 IndexScanDesc。
 *
 *		本例程创建索引扫描结构并设置其初始
 *		内容。
 *
 *		参数：
 *				indexRelation -- 供扫描的索引关系。
 *				nkeys -- 扫描键数目（索引限定条件）。
 *				norderbys -- 索引 order-by 操作符数目。
 *
 *		返回：
 *				已初始化的 IndexScanDesc。
 * ----------------
 */
IndexScanDesc
RelationGetIndexScan(Relation indexRelation, int nkeys, int norderbys)
{
	IndexScanDesc scan;

	scan = (IndexScanDesc) palloc(sizeof(IndexScanDescData));

	scan->heapRelation = NULL;	/* 稍后可能设置 */
	scan->xs_heapfetch = NULL;
	scan->indexRelation = indexRelation;
	scan->xs_snapshot = InvalidSnapshot;	/* 调用方必须初始化此项 */
	scan->numberOfKeys = nkeys;
	scan->numberOfOrderBys = norderbys;

	/*
	 * 这里分配键的工作空间，但要到 amrescan 才会填入。
	 */
	if (nkeys > 0)
		scan->keyData = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->keyData = NULL;
	if (norderbys > 0)
		scan->orderByData = (ScanKey) palloc(sizeof(ScanKeyData) * norderbys);
	else
		scan->orderByData = NULL;

	scan->xs_want_itup = false; /* 稍后可能设置 */

	/*
	 * 恢复期间我们忽略已杀死的元组，也不费心去杀死
	 * 它们。这样做是因为主库上的 xmin 很容易
	 * 晚于备库上的 xmin，于是主库认为已杀死的元组
	 * 在备库上应当可见。因此，为使恢复期间查询的
	 * MVCC 正确，必须忽略这些提示并检查全部元组。
	 * 在恢复期间启动的事务中运行时，*不要* 把
	 * ignore_killed_tuples 设为真。索引 AM 不应改动
	 * xactStartedInRecovery。
	 */
	scan->kill_prior_tuple = false;
	scan->xactStartedInRecovery = TransactionStartedDuringRecovery();
	scan->ignore_killed_tuples = !scan->xactStartedInRecovery;

	scan->opaque = NULL;

	scan->xs_itup = NULL;
	scan->xs_itupdesc = NULL;
	scan->xs_hitup = NULL;
	scan->xs_hitupdesc = NULL;

	return scan;
}

/* ----------------
 *	IndexScanEnd -- 结束一次索引扫描。
 *
 *		本例程只释放由 RelationGetIndexScan()
 *		取得的存储。任何 AM 级资源都假定
 *		已由该 AM 的 endscan 例程
 *		释放。
 *
 *	返回：
 *		无。
 * ----------------
 */
void
IndexScanEnd(IndexScanDesc scan)
{
	if (scan->keyData != NULL)
		pfree(scan->keyData);
	if (scan->orderByData != NULL)
		pfree(scan->orderByData);

	pfree(scan);
}

/*
 * BuildIndexValueDescription
 *
 * 构造描述索引项内容的字符串，形式为
 * "(key_name, ...)=(key_value, ...)"。目前用于
 * 构造唯一约束与排他约束的错误消息，
 * 因此只检查并打印索引的键列。
 *
 * 注意：若用户无权查看涉及的全部
 * 列，则返回 NULL。返回部分键看来
 * 不大有用，而且我们无法知道用户提供了哪些
 * 列（与 ExecBuildSlotValueDescription 不同）。
 *
 * 传入的 values/nulls 数组是索引 AM 的 "原始" 输入，
 * 例如 FormIndexDatum 的结果 --- 这不一定是索引里
 * 实际存放的内容，而是用户所感知的存储内容。
 *
 * 注意：若此处有任何改动，请检查
 * execMain.c 中的 ExecBuildSlotPartitionKeyDescription() 是否需要类似
 * 改动。
 */
char *
BuildIndexValueDescription(Relation indexRelation,
						   Datum *values, bool *isnull)
{
	StringInfoData buf;
	Form_pg_index idxrec;
	int			indnkeyatts;
	int			i;
	int			keyno;
	Oid			indexrelid = RelationGetRelid(indexRelation);
	Oid			indrelid;
	AclResult	aclresult;

	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(indexRelation);

	/*
	 * 检查权限：若用户无权查看全部键列，
	 * 则返回 NULL，以免泄露数据。
	 *
	 * 先检查该关系是否启用了 RLS。若是，返回 NULL，
	 * 以免泄露数据。
	 *
	 * 接下来需要检查表级 SELECT 访问，若那里没有访问权，
	 * 再检查列级权限。
	 */
	idxrec = indexRelation->rd_index;
	indrelid = idxrec->indrelid;
	Assert(indexrelid == idxrec->indexrelid);

	/* RLS 检查：若启用了 RLS，则什么都不返回。 */
	if (check_enable_rls(indrelid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	/* 若用户拥有表级 SELECT，则已足够 */
	aclresult = pg_class_aclcheck(indrelid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/*
		 * 没有表级访问权，因此逐个检查索引中的列，
		 * 确保用户对它们全部拥有 SELECT 权限。
		 */
		for (keyno = 0; keyno < indnkeyatts; keyno++)
		{
			AttrNumber	attnum = idxrec->indkey.values[keyno];

			/*
			 * 注意：若 attnum == InvalidAttrNumber，则这是基于
			 * 表达式的索引，我们不返回细节，而不是试图
			 * 弄清表达式包含哪些列 column(s)，以及用户
			 * 是否对它们拥有 SELECT 权限。
			 */
			if (attnum == InvalidAttrNumber ||
				pg_attribute_aclcheck(indrelid, attnum, GetUserId(),
									  ACL_SELECT) != ACLCHECK_OK)
			{
				/* 没有访问权，清理并返回 */
				return NULL;
			}
		}
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "(%s)=(",
					 pg_get_indexdef_columns(indexrelid, true));

	for (i = 0; i < indnkeyatts; i++)
	{
		char	   *val;

		if (isnull[i])
			val = "null";
		else
		{
			Oid			foutoid;
			bool		typisvarlena;

			/*
			 * 提供的数据不一定是索引中存储的类型，
			 * 而是索引操作符类的输入类型。因此查看
			 * rd_opcintype，而不是索引 tupdesc。
			 *
			 * 注意：对输入类型为伪类型的操作符类，这有点不稳，
			 * 例如 ANYARRAY 或 RECORD。目前与这些伪类型关联的
			 * typoutput 函数可以正常工作，但将来也许得
			 * 再加把劲。
			 */
			getTypeOutputInfo(indexRelation->rd_opcintype[i],
							  &foutoid, &typisvarlena);
			val = OidOutputFunctionCall(foutoid, values[i]);
		}

		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, val);
	}

	appendStringInfoChar(&buf, ')');

	return buf.data;
}

/*
 * 用与 AM 无关的方法，从即将删除的索引元组所指向的
 * 表项取得 snapshotConflictHorizon。
 *
 * 这是 table_index_delete_tuples() 的垫片，供那些只需
 * 咨询 tableam 以取得 snapshotConflictHorizon 值、且只
 * 预期删除已知可删的索引元组（通常因为已设置
 * LP_DEAD 位）的索引 AM 使用。若删除 WAL record
 * 不需要 snapshotConflictHorizon 值，则完全可以
 * 跳过此处的调用。
 *
 * 我们假定调用方索引 AM 使用标准 IndexTuple 表示，
 * 表 TIDs 存在 t_tid 字段中。我们还预期（并断言）
 * 页上 'itemnos' 偏移对应的行指针已标记为
 * LP_DEAD。
 */
TransactionId
index_compute_xid_horizon_for_tuples(Relation irel,
									 Relation hrel,
									 Buffer ibuf,
									 OffsetNumber *itemnos,
									 int nitems)
{
	TM_IndexDeleteOp delstate;
	TransactionId snapshotConflictHorizon = InvalidTransactionId;
	Page		ipage = BufferGetPage(ibuf);
	IndexTuple	itup;

	Assert(nitems > 0);

	delstate.irel = irel;
	delstate.iblknum = BufferGetBlockNumber(ibuf);
	delstate.bottomup = false;
	delstate.bottomupfreespace = 0;
	delstate.ndeltids = 0;
	delstate.deltids = palloc(nitems * sizeof(TM_IndexDelete));
	delstate.status = palloc(nitems * sizeof(TM_IndexStatus));

	/* 辨明即将删除的索引元组指向什么 */
	for (int i = 0; i < nitems; i++)
	{
		OffsetNumber offnum = itemnos[i];
		ItemId		iitemid;

		iitemid = PageGetItemId(ipage, offnum);
		itup = (IndexTuple) PageGetItem(ipage, iitemid);

		Assert(ItemIdIsDead(iitemid));

		ItemPointerCopy(&itup->t_tid, &delstate.deltids[i].tid);
		delstate.deltids[i].id = delstate.ndeltids;
		delstate.status[i].idxoffnum = offnum;
		delstate.status[i].knowndeletable = true;	/* 已标记 LP_DEAD */
		delstate.status[i].promising = false;	/* 未使用 */
		delstate.status[i].freespace = 0;	/* 未使用 */

		delstate.ndeltids++;
	}

	/* 确定实际的 xid 界限 */
	snapshotConflictHorizon = table_index_delete_tuples(hrel, &delstate);

	/* 断言 tableam 同意所有项都可删除 */
	Assert(delstate.ndeltids == nitems);

	pfree(delstate.deltids);
	pfree(delstate.status);

	return snapshotConflictHorizon;
}


/* ----------------------------------------------------------------
 *		系统目录的堆扫描或索引扫描访问
 *
 *		这些函数支持通常使用索引、
 *		但在系统索引不可用时必须能切换到堆扫描的
 *		系统目录访问。
 *
 *		指定的扫描键必须与指名的索引兼容。
 *		一般这意味着它们必须约束索引的全部列，
 *		或 N 列索引的前 K 列。
 *
 *		这些例程其实也能用于非系统表，
 *		但只有在已知有索引可配合给定扫描键时才有用；
 *		因此实践中只适合系统目录上
 *		预先确定类型的扫描。
 * ----------------------------------------------------------------
 */

/*
 * systable_beginscan --- 为堆扫描或索引扫描做准备
 *
 *	rel: 要扫描的目录，已打开并适当加锁
 *	indexId: 有条件使用的索引 OID
 *	indexOK: 若为假，则强制堆扫描（见下面的说明）
 *	snapshot: 使用的时间限定（NULL 表示最近的目录快照）
 *	nkeys, key: 扫描键
 *
 * 扫描键中的属性号应按堆的情形设置。
 * 若选择走索引，则把它们重设为 1..n，以引用索引
 * 列。注意这意味着每个索引列必须有一条 scankey 限定！
 * 正常走索引的情形由 Asserts 检查，但若走
 * heapscan 路径则不会检查。
 *
 * 本例程检查索引扫描是否安全的常规情形，
 * 但调用方可以做额外检查，必要时传入 indexOK=false。
 * 标准情形下 indexOK 可以就是常量 TRUE。
 */
SysScanDesc
systable_beginscan(Relation heapRelation,
				   Oid indexId,
				   bool indexOK,
				   Snapshot snapshot,
				   int nkeys, ScanKey key)
{
	SysScanDesc sysscan;
	Relation	irel;

	if (indexOK &&
		!IgnoreSystemIndexes &&
		!ReindexIsProcessingIndex(indexId))
		irel = index_open(indexId, AccessShareLock);
	else
		irel = NULL;

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));

	sysscan->heap_rel = heapRelation;
	sysscan->irel = irel;
	sysscan->slot = table_slot_create(heapRelation, NULL);

	if (snapshot == NULL)
	{
		Oid			relid = RelationGetRelid(heapRelation);

		snapshot = RegisterSnapshot(GetCatalogSnapshot(relid));
		sysscan->snapshot = snapshot;
	}
	else
	{
		/* 调用方负责任何快照。 */
		sysscan->snapshot = NULL;
	}

	if (irel)
	{
		int			i;

		/* 把属性号改成索引列号。 */
		for (i = 0; i < nkeys; i++)
		{
			int			j;

			for (j = 0; j < IndexRelationGetNumberOfAttributes(irel); j++)
			{
				if (key[i].sk_attno == irel->rd_index->indkey.values[j])
				{
					key[i].sk_attno = j + 1;
					break;
				}
			}
			if (j == IndexRelationGetNumberOfAttributes(irel))
				elog(ERROR, "column is not in index");
		}

		sysscan->iscan = index_beginscan(heapRelation, irel,
										 snapshot, nkeys, 0);
		index_rescan(sysscan->iscan, key, nkeys, NULL, 0);
		sysscan->scan = NULL;
	}
	else
	{
		/*
		 * 被迫对目录做堆扫描时，不允许同步扫描。
		 * 多数情况下想要的行靠近前面，因此
		 * 同步扫描不可预测的起点是严重缺点；
		 * 也没有补偿性的好处，因为这类扫描
		 * 不太可能并行发生。
		 */
		sysscan->scan = table_beginscan_strat(heapRelation, snapshot,
											  nkeys, key,
											  true, false);
		sysscan->iscan = NULL;
	}

	/*
	 * 若设置了 CheckXidAlive，则置标志表示系统表扫描
	 * 正在进行。这些变量的声明处见 xact.c 中的
	 * 详细注释。
	 */
	if (TransactionIdIsValid(CheckXidAlive))
		bsysscan = true;

	return sysscan;
}

/*
 * HandleConcurrentAbort - 处理 CheckXidAlive 的并发中止。
 *
 * 若 CheckXidAlive 已中止则报错。不能直接使用
 * TransactionIdDidAbort，因为崩溃之后这样的事务可能
 * 尚未被标记为已中止。该变量的声明处
 * 见 xact.c 中的详细注释。
 */
static inline void
HandleConcurrentAbort()
{
	if (TransactionIdIsValid(CheckXidAlive) &&
		!TransactionIdIsInProgress(CheckXidAlive) &&
		!TransactionIdDidCommit(CheckXidAlive))
		ereport(ERROR,
				(errcode(ERRCODE_TRANSACTION_ROLLBACK),
				 errmsg("transaction aborted during system catalog scan")));
}

/*
 * systable_getnext --- 在堆扫描或索引扫描中取下一个元组
 *
 * 若没有更多元组则返回 NULL。
 *
 * 注意：返回的元组是磁盘缓冲区中数据的引用；
 * 不得修改它，并且应假定在下一次 getnext() 或 endscan()
 * 调用之后就不能再访问。
 *
 * XXX：至少可选地提供基于槽的接口，
 * 大概是有意义的。
 */
HeapTuple
systable_getnext(SysScanDesc sysscan)
{
	HeapTuple	htup = NULL;

	if (sysscan->irel)
	{
		if (index_getnext_slot(sysscan->iscan, ForwardScanDirection, sysscan->slot))
		{
			bool		shouldFree;

			htup = ExecFetchSlotHeapTuple(sysscan->slot, false, &shouldFree);
			Assert(!shouldFree);

			/*
			 * 目前不需要为任何系统目录扫描
			 * 支持有损索引操作符。可以在这里做，用扫描
			 * 键驱动操作符调用，只要在 systable_beginscan()
			 * 期间保存堆的 attnums；这是可行的，
			 * 因为我们仍然不需要支持表达式上的
			 * 索引。
			 */
			if (sysscan->iscan->xs_recheck)
				elog(ERROR, "system catalog scans with lossy index conditions are not implemented");
		}
	}
	else
	{
		if (table_scan_getnextslot(sysscan->scan, ForwardScanDirection, sysscan->slot))
		{
			bool		shouldFree;

			htup = ExecFetchSlotHeapTuple(sysscan->slot, false, &shouldFree);
			Assert(!shouldFree);
		}
	}

	/*
	 * 在事务的逻辑流式传输期间取出目录元组时，
	 * 处理并发中止。
	 */
	HandleConcurrentAbort();

	return htup;
}

/*
 * systable_recheck_tuple --- 复查最近取出的元组的可见性
 *
 * 具体而言，判断该元组对此刻开始的目录扫描
 * 是否可见。我们不处理非 MVCC 扫描快照的情形，
 * 因为还没有调用方需要。
 *
 * 这可用于测试：在我们等待获取其上的锁时，
 * 对象是否已被删除。
 *
 * 注意：其实 *不需要* 把元组传进来，但这是
 * 很好的交叉检查，确认调用方关心的是正确的元组。
 */
bool
systable_recheck_tuple(SysScanDesc sysscan, HeapTuple tup)
{
	Snapshot	freshsnap;
	bool		result;

	Assert(tup == ExecFetchSlotHeapTuple(sysscan->slot, false, NULL));

	/*
	 * 相信 table_tuple_satisfies_snapshot() 及其子例程
	 * （常见的是 LockBuffer() 与 HeapTupleSatisfiesMVCC()）自身不会获取快照，
	 * 因此不必登记该快照。那些设施层次太低，
	 * 不该去扫描表。
	 */
	freshsnap = GetCatalogSnapshot(RelationGetRelid(sysscan->heap_rel));

	result = table_tuple_satisfies_snapshot(sysscan->heap_rel,
											sysscan->slot,
											freshsnap);

	/*
	 * 在事务的逻辑流式传输期间取出目录元组时，
	 * 处理并发中止。
	 */
	HandleConcurrentAbort();

	return result;
}

/*
 * systable_endscan --- 关闭扫描，释放资源
 *
 * 注意：关闭堆关系仍由调用方负责。
 */
void
systable_endscan(SysScanDesc sysscan)
{
	if (sysscan->slot)
	{
		ExecDropSingleTupleTableSlot(sysscan->slot);
		sysscan->slot = NULL;
	}

	if (sysscan->irel)
	{
		index_endscan(sysscan->iscan);
		index_close(sysscan->irel, AccessShareLock);
	}
	else
		table_endscan(sysscan->scan);

	if (sysscan->snapshot)
		UnregisterSnapshot(sysscan->snapshot);

	/*
	 * 在系统表扫描结束时复位 bsysscan 标志。这些变量的声明处
	 * 见 xact.c 中的详细注释。
	 */
	if (TransactionIdIsValid(CheckXidAlive))
		bsysscan = false;

	pfree(sysscan);
}


/*
 * systable_beginscan_ordered --- 为有序目录扫描做准备
 *
 * 这些例程的 API 与 systable_beginscan 等基本相同，
 * 只是保证按索引顺序返回多个匹配元组。
 * 另外，很大程度上出于历史原因，要使用的索引
 * 由调用方打开并加锁，而不是在这里。
 *
 * 目前这里不支持非基于索引的扫描。（原则上
 * 可以做堆扫描再排序，但使用处大概不需要
 * 在目录索引损坏时仍能工作。）
 * 因此眼下这些函数只是
 * index_beginscan/index_getnext_slot
 * 最薄的包装。它们存在的主要原因，是把目录扫描中
 * 将来可能对有损操作符的支持集中起来。
 */
SysScanDesc
systable_beginscan_ordered(Relation heapRelation,
						   Relation indexRelation,
						   Snapshot snapshot,
						   int nkeys, ScanKey key)
{
	SysScanDesc sysscan;
	int			i;

	/* REINDEX 在这里大概可以是硬错误 ... */
	if (ReindexIsProcessingIndex(RelationGetRelid(indexRelation)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access index \"%s\" while it is being reindexed",
						RelationGetRelationName(indexRelation))));
	/* ... 但对违反 IgnoreSystemIndexes 只发警告 */
	if (IgnoreSystemIndexes)
		elog(WARNING, "using index \"%s\" despite IgnoreSystemIndexes",
			 RelationGetRelationName(indexRelation));

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));

	sysscan->heap_rel = heapRelation;
	sysscan->irel = indexRelation;
	sysscan->slot = table_slot_create(heapRelation, NULL);

	if (snapshot == NULL)
	{
		Oid			relid = RelationGetRelid(heapRelation);

		snapshot = RegisterSnapshot(GetCatalogSnapshot(relid));
		sysscan->snapshot = snapshot;
	}
	else
	{
		/* 调用方负责任何快照。 */
		sysscan->snapshot = NULL;
	}

	/* 把属性号改成索引列号。 */
	for (i = 0; i < nkeys; i++)
	{
		int			j;

		for (j = 0; j < IndexRelationGetNumberOfAttributes(indexRelation); j++)
		{
			if (key[i].sk_attno == indexRelation->rd_index->indkey.values[j])
			{
				key[i].sk_attno = j + 1;
				break;
			}
		}
		if (j == IndexRelationGetNumberOfAttributes(indexRelation))
			elog(ERROR, "column is not in index");
	}

	sysscan->iscan = index_beginscan(heapRelation, indexRelation,
									 snapshot, nkeys, 0);
	index_rescan(sysscan->iscan, key, nkeys, NULL, 0);
	sysscan->scan = NULL;

	/*
	 * 若设置了 CheckXidAlive，则置标志表示系统表扫描
	 * 正在进行。这些变量的声明处见 xact.c 中的
	 * 详细注释。
	 */
	if (TransactionIdIsValid(CheckXidAlive))
		bsysscan = true;

	return sysscan;
}

/*
 * systable_getnext_ordered --- 在有序目录扫描中取下一个元组
 */
HeapTuple
systable_getnext_ordered(SysScanDesc sysscan, ScanDirection direction)
{
	HeapTuple	htup = NULL;

	Assert(sysscan->irel);
	if (index_getnext_slot(sysscan->iscan, direction, sysscan->slot))
		htup = ExecFetchSlotHeapTuple(sysscan->slot, false, NULL);

	/* 见 systable_getnext 中的说明 */
	if (htup && sysscan->iscan->xs_recheck)
		elog(ERROR, "system catalog scans with lossy index conditions are not implemented");

	/*
	 * 在事务的逻辑流式传输期间取出目录元组时，
	 * 处理并发中止。
	 */
	HandleConcurrentAbort();

	return htup;
}

/*
 * systable_endscan_ordered --- 关闭扫描，释放资源
 */
void
systable_endscan_ordered(SysScanDesc sysscan)
{
	if (sysscan->slot)
	{
		ExecDropSingleTupleTableSlot(sysscan->slot);
		sysscan->slot = NULL;
	}

	Assert(sysscan->irel);
	index_endscan(sysscan->iscan);
	if (sysscan->snapshot)
		UnregisterSnapshot(sysscan->snapshot);

	/*
	 * 在系统表扫描结束时复位 bsysscan 标志。这些变量的声明处
	 * 见 xact.c 中的详细注释。
	 */
	if (TransactionIdIsValid(CheckXidAlive))
		bsysscan = false;

	pfree(sysscan);
}

/*
 * systable_inplace_update_begin --- 原地更新一行（覆盖它）
 *
 * 覆盖既破坏 MVCC，也破坏事务安全性，因此本函数在 Postgres 中的
 * 用途极其有限。尽管如此，我们还是找到一些地方使用它。
 * 读者与写者对接受原地更新的表有何预期，见 README.tuplock
 * 中 "Locking to write inplace-updated tables" 一节及后续各节。
 * 标准流程：
 *
 * ... [任何不需要 oldtup 的慢速准备] ...
 * systable_inplace_update_begin([...], &tup, &inplace_state);
 * if (!HeapTupleIsValid(tup))
 *	elog(ERROR, [...]);
 * ... [缓冲区已排他锁定；修改 "tup"] ...
 * if (dirty)
 *	systable_inplace_update_finish(inplace_state, tup);
 * else
 *	systable_inplace_update_cancel(inplace_state);
 *
 * 前几个参数重复 systable_beginscan() 的参数列表。
 * "oldtupcopy" 是输出参数，若键不再能找到活元组，则赋为 NULL。
 * （在 PROC_IN_VACUUM 中，这是低概率的瞬时状况。）
 * 若 "oldtupcopy" 得到非 NULL，必须把输出参数 "state" 传给
 * systable_inplace_update_finish() 或
 * systable_inplace_update_cancel()。
 */
void
systable_inplace_update_begin(Relation relation,
							  Oid indexId,
							  bool indexOK,
							  Snapshot snapshot,
							  int nkeys, const ScanKeyData *key,
							  HeapTuple *oldtupcopy,
							  void **state)
{
	ScanKey		mutable_key = palloc(sizeof(ScanKeyData) * nkeys);
	int			retries = 0;
	SysScanDesc scan;
	HeapTuple	oldtup;
	BufferHeapTupleTableSlot *bslot;

	/*
	 * 目前不允许并行更新。与普通更新不同，
	 * 这里绝不应该创建组合 CID，因此也许可以放宽
	 * 这一限制，但需要更多思考和测试。况且
	 * 也不清楚那样是否有用。
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

	/*
	 * 为对称起见接受快照参数，但本函数会按需要推进其快照，
	 * 以到达已更新元组链的尾部。
	 */
	Assert(snapshot == NULL);

	Assert(IsInplaceUpdateRelation(relation) || !IsSystemRelation(relation));

	/* 循环以取得非更新元组的排他锁定缓冲区。 */
	do
	{
		TupleTableSlot *slot;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 以最高速度发出 heap_update（例如 GRANT）的进程
		 * 可能把我们逼到这个错误。恶意的表所有者有更强的办法
		 * 损坏自己的表，所以这算小事。
		 */
		if (retries++ > 10000)
			elog(ERROR, "giving up after too many tries to overwrite row");

		memcpy(mutable_key, key, sizeof(ScanKeyData) * nkeys);
		scan = systable_beginscan(relation, indexId, indexOK, snapshot,
								  nkeys, mutable_key);
		oldtup = systable_getnext(scan);
		if (!HeapTupleIsValid(oldtup))
		{
			systable_endscan(scan);
			*oldtupcopy = NULL;
			return;
		}

		slot = scan->slot;
		Assert(TTS_IS_BUFFERTUPLE(slot));
		bslot = (BufferHeapTupleTableSlot *) slot;
	} while (!heap_inplace_lock(scan->heap_rel,
								bslot->base.tuple, bslot->buffer,
								(void (*) (void *)) systable_endscan, scan));

	*oldtupcopy = heap_copytuple(oldtup);
	*state = scan;
}

/*
 * systable_inplace_update_finish --- 原地更新的第二阶段
 *
 * 元组无法改变大小，因此其头部字段和空值位图
 * （若有）也不会改变。
 */
void
systable_inplace_update_finish(void *state, HeapTuple tuple)
{
	SysScanDesc scan = (SysScanDesc) state;
	Relation	relation = scan->heap_rel;
	TupleTableSlot *slot = scan->slot;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	HeapTuple	oldtup = bslot->base.tuple;
	Buffer		buffer = bslot->buffer;

	heap_inplace_update_and_unlock(relation, oldtup, tuple, buffer);
	systable_endscan(scan);
}

/*
 * systable_inplace_update_cancel --- 放弃原地更新
 *
 * 这是不做空操作更新的替代办法。
 */
void
systable_inplace_update_cancel(void *state)
{
	SysScanDesc scan = (SysScanDesc) state;
	Relation	relation = scan->heap_rel;
	TupleTableSlot *slot = scan->slot;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	HeapTuple	oldtup = bslot->base.tuple;
	Buffer		buffer = bslot->buffer;

	heap_inplace_unlock(relation, oldtup, buffer);
	systable_endscan(scan);
}
