/*-------------------------------------------------------------------------
 *
 * commit_ts.c
 *		PostgreSQL 提交时间戳管理器
 *
 * 本模块类似 pg_xact，为每个事务存储
 * 提交时间戳。
 *
 * XLOG 交互：每当新的 CommitTs 页被初始化为零时，本模块
 * 生成一条 XLOG 记录。对 CommitTS 的其他写入来自 xact.c 中
 * 对事务提交的记录，后者会为这些事件生成自己的 XLOG 记录，
 * 并在 redo 时重新执行状态更新；因此这里无需再写额外的
 * XLOG 项。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/commit_ts.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/commit_ts.h"
#include "access/htup_details.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

/*
 * CommitTs 页大小相关定义。页大小与 Postgres
 * 其他地方一样，均为 BLCKSZ。
 *
 * 注意：因为 TransactionIds 是 32 位并在 0xFFFFFFFF 回卷，
 * CommitTs 页编号也会在
 * 0xFFFFFFFF/COMMIT_TS_XACTS_PER_PAGE 处回卷，段编号在
 * 0xFFFFFFFF/COMMIT_TS_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT 处回卷。
 * 除了在 TruncateCommitTs 中比较段号与页号时（见
 * CommitTsPagePrecedes）外，本模块无需显式关注这一事实。
 */

/*
 * 每个 xact 需要 8+2 字节。注意扩大此结构可能意味着
 * 最大可能文件名超过 5 个字符；参见
 * SlruScanDirectory。
 */
typedef struct CommitTimestampEntry
{
	TimestampTz time;
	RepOriginId nodeid;
} CommitTimestampEntry;

#define SizeOfCommitTimestampEntry (offsetof(CommitTimestampEntry, nodeid) + \
									sizeof(RepOriginId))

#define COMMIT_TS_XACTS_PER_PAGE \
	(BLCKSZ / SizeOfCommitTimestampEntry)

#define TransactionIdToCTsPage(xid) \
	((xid) / (TransactionId) COMMIT_TS_XACTS_PER_PAGE)
#define TransactionIdToCTsEntry(xid)	\
	((xid) % (TransactionId) COMMIT_TS_XACTS_PER_PAGE)

/*
 * 指向 CommitTs 控制用共享内存数据结构的链接
 */
static SlruCtlData CommitTsCtlData;

#define CommitTsCtl (&CommitTsCtlData)

/*
 * 我们在共享内存中缓存最近设置的值。
 *
 * 这里也适合存放激活状态。我们把它与
 * GUC 分开，以便备库可以在主库已激活该
 * 模块时独立于 GUC 的值激活本模块。
 *
 * 由 CommitTsLock 保护。有些地
 * 方在未获取锁时使用
 * commitTsActive；出现这种情况时，注释会说明理由。
 */
typedef struct CommitTimestampShared
{
	TransactionId xidLastCommit;
	CommitTimestampEntry dataLastCommit;
	bool		commitTsActive;
} CommitTimestampShared;

static CommitTimestampShared *commitTsShared;


/* GUC 变量 */
bool		track_commit_timestamp;

static void SetXidCommitTsInPage(TransactionId xid, int nsubxids,
								 TransactionId *subxids, TimestampTz ts,
								 RepOriginId nodeid, int pageno);
static void TransactionIdSetCommitTs(TransactionId xid, TimestampTz ts,
									 RepOriginId nodeid, int slotno);
static void error_commit_ts_disabled(void);
static int	ZeroCommitTsPage(int pageno, bool writeXlog);
static bool CommitTsPagePrecedes(int page1, int page2);
static void ActivateCommitTs(void);
static void DeactivateCommitTs(void);
static void WriteZeroPageXlogRec(int pageno);
static void WriteTruncateXlogRec(int pageno, TransactionId oldestXid);

/*
 * TransactionTreeSetCommitTsData
 *
 * 尽可能高效地在提交日志中记录一个事务及其子事务树
 * 各项的最终提交时间戳。
 *
 * xid 是顶层事务 id。
 *
 * subxids 是长度为 nsubxids 的 xid 数组，表示 xid 树中的
 * 子事务。各种情况下 nsubxids 可能为零。
 * 仅跟踪父 xid 的提交时间戳还不够，原因是
 * 子事务 SLRU 跨崩溃后不会保持有效（它不是
 * 永久的），因此需要在此保留关于它们的信息。若将来
 * 子事务实现改变，我们可能希望重新考虑为每个
 * subxid 存储时间戳信息的决定。
 */
void
TransactionTreeSetCommitTsData(TransactionId xid, int nsubxids,
							   TransactionId *subxids, TimestampTz timestamp,
							   RepOriginId nodeid)
{
	int			i;
	TransactionId headxid;
	TransactionId newestXact;

	/*
	 * 若模块未激活则为空操作。
	 *
	 * 此处无锁读取没问题，因为在备库
	 * 上（标志可在飞行中改变的唯一地
	 * 方）本例程仅由恢复进程调用，而
	 * 它也是唯一能改变该标志的进程。
	 */
	if (!commitTsShared->commitTsActive)
		return;

	/*
	 * 算出本批中最新的 Xid：若有子 xid 则为最后一个
	 * subxid，否则为父 xid。
	 */
	if (nsubxids > 0)
		newestXact = subxids[nsubxids - 1];
	else
		newestXact = xid;

	/*
	 * 我们把要设置时间戳的 xid 按所属同一
	 * SLRU 页分组；每组的第一个元素是其
	 * head。第一组以主 XID 为 head；后续各
	 * 组用不在上一页上的第一个 subxid 作为
	 * head。这样每个 SLRU 页只需加锁/修改一次。
	 */
	headxid = xid;
	i = 0;
	for (;;)
	{
		int			pageno = TransactionIdToCTsPage(headxid);
		int			j;

		for (j = i; j < nsubxids; j++)
		{
			if (TransactionIdToCTsPage(subxids[j]) != pageno)
				break;
		}
		/* subxids[i..j] 与 head 在同一页上 */

		SetXidCommitTsInPage(headxid, j - i, subxids + i, timestamp, nodeid,
							 pageno);

		/* 若已写出全部 subxids，则完成。 */
		if (j >= nsubxids)
			break;

		/*
		 * 设置新的 head，并跳过它以及我们刚写出的
		 * subxids。
		 */
		headxid = subxids[j];
		i = j + 1;
	}

	/* 更新共享内存中的缓存值 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	commitTsShared->xidLastCommit = xid;
	commitTsShared->dataLastCommit.time = timestamp;
	commitTsShared->dataLastCommit.nodeid = nodeid;

	/* 必要时向前推进我们的终点 */
	if (TransactionIdPrecedes(ShmemVariableCache->newestCommitTsXid, newestXact))
		ShmemVariableCache->newestCommitTsXid = newestXact;
	LWLockRelease(CommitTsLock);
}

/*
 * 在提交日志中为一页上的全部项记录提交时间戳。
 * 仅在本页上原子。
 */
static void
SetXidCommitTsInPage(TransactionId xid, int nsubxids,
					 TransactionId *subxids, TimestampTz ts,
					 RepOriginId nodeid, int pageno)
{
	int			slotno;
	int			i;

	LWLockAcquire(CommitTsSLRULock, LW_EXCLUSIVE);

	slotno = SimpleLruReadPage(CommitTsCtl, pageno, true, xid);

	TransactionIdSetCommitTs(xid, ts, nodeid, slotno);
	for (i = 0; i < nsubxids; i++)
		TransactionIdSetCommitTs(subxids[i], ts, nodeid, slotno);

	CommitTsCtl->shared->page_dirty[slotno] = true;

	LWLockRelease(CommitTsSLRULock);
}

/*
 * 设置单个事务的提交时间戳。
 *
 * 调用时必须持有 CommitTsSLRULock
 */
static void
TransactionIdSetCommitTs(TransactionId xid, TimestampTz ts,
						 RepOriginId nodeid, int slotno)
{
	int			entryno = TransactionIdToCTsEntry(xid);
	CommitTimestampEntry entry;

	Assert(TransactionIdIsNormal(xid));

	entry.time = ts;
	entry.nodeid = nodeid;

	memcpy(CommitTsCtl->shared->page_buffer[slotno] +
		   SizeOfCommitTimestampEntry * entryno,
		   &entry, SizeOfCommitTimestampEntry);
}

/*
 * 查询某事务的提交时间戳。
 *
 * 返回值表示是否为给定 xid 找到了提交时间戳
 * 记录。时间戳值返回在 *ts 中（不得为
 * null），若 *nodeid 非null，则把
 * 该 Xid 的源节点返回在 *nodeid 中。
 */
bool
TransactionIdGetCommitTsData(TransactionId xid, TimestampTz *ts,
							 RepOriginId *nodeid)
{
	int			pageno = TransactionIdToCTsPage(xid);
	int			entryno = TransactionIdToCTsEntry(xid);
	int			slotno;
	CommitTimestampEntry entry;
	TransactionId oldestCommitTsXid;
	TransactionId newestCommitTsXid;

	if (!TransactionIdIsValid(xid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot retrieve commit timestamp for transaction %u", xid)));
	else if (!TransactionIdIsNormal(xid))
	{
		/* 冻结与 bootstrap xid 总是在很久以前就已提交 */
		*ts = 0;
		if (nodeid)
			*nodeid = 0;
		return false;
	}

	LWLockAcquire(CommitTsLock, LW_SHARED);

	/* 模块未启用则报错 */
	if (!commitTsShared->commitTsActive)
		error_commit_ts_disabled();

	/*
	 * 若请求的是缓存值，则返回它。否则继续向下
	 * 从 SLRU 读取。
	 */
	if (commitTsShared->xidLastCommit == xid)
	{
		*ts = commitTsShared->dataLastCommit.time;
		if (nodeid)
			*nodeid = commitTsShared->dataLastCommit.nodeid;

		LWLockRelease(CommitTsLock);
		return *ts != 0;
	}

	oldestCommitTsXid = ShmemVariableCache->oldestCommitTsXid;
	newestCommitTsXid = ShmemVariableCache->newestCommitTsXid;
	/* 二者要么都无效，要么都有效 */
	Assert(TransactionIdIsValid(oldestCommitTsXid) == TransactionIdIsValid(newestCommitTsXid));
	LWLockRelease(CommitTsLock);

	/*
	 * 若请求值在我们的有效范围之外，则返回空。
	 */
	if (!TransactionIdIsValid(oldestCommitTsXid) ||
		TransactionIdPrecedes(xid, oldestCommitTsXid) ||
		TransactionIdPrecedes(newestCommitTsXid, xid))
	{
		*ts = 0;
		if (nodeid)
			*nodeid = InvalidRepOriginId;
		return false;
	}

	/* 锁由 SimpleLruReadPage_ReadOnly 获取 */
	slotno = SimpleLruReadPage_ReadOnly(CommitTsCtl, pageno, xid);
	memcpy(&entry,
		   CommitTsCtl->shared->page_buffer[slotno] +
		   SizeOfCommitTimestampEntry * entryno,
		   SizeOfCommitTimestampEntry);

	*ts = entry.time;
	if (nodeid)
		*nodeid = entry.nodeid;

	LWLockRelease(CommitTsSLRULock);
	return *ts != 0;
}

/*
 * 返回最近提交事务的 Xid。
 * （就本模块而言如此；由调用
 * 者确保该值对其用途有用。）
 *
 * ts 与 nodeid 填入对
 * 应数据；若不需要可传NULL。
 */
TransactionId
GetLatestCommitTsData(TimestampTz *ts, RepOriginId *nodeid)
{
	TransactionId xid;

	LWLockAcquire(CommitTsLock, LW_SHARED);

	/* 模块未启用则报错 */
	if (!commitTsShared->commitTsActive)
		error_commit_ts_disabled();

	xid = commitTsShared->xidLastCommit;
	if (ts)
		*ts = commitTsShared->dataLastCommit.time;
	if (nodeid)
		*nodeid = commitTsShared->dataLastCommit.nodeid;
	LWLockRelease(CommitTsLock);

	return xid;
}

static void
error_commit_ts_disabled(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("could not get commit timestamp data"),
			 RecoveryInProgress() ?
			 errhint("Make sure the configuration parameter \"%s\" is set on the primary server.",
					 "track_commit_timestamp") :
			 errhint("Make sure the configuration parameter \"%s\" is set.",
					 "track_commit_timestamp")));
}

/*
 * 可供 SQL 调用的包装，用于取得事务的提交时间
 */
Datum
pg_xact_commit_timestamp(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	TimestampTz ts;
	bool		found;

	found = TransactionIdGetCommitTsData(xid, &ts, NULL);

	if (!found)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(ts);
}


/*
 * pg_last_committed_xact
 *
 * 可供 SQL 调用的包装，用于取得最近提交事务的
 * 若干信息：事务 ID、时间戳与复制
 * 源。
 */
Datum
pg_last_committed_xact(PG_FUNCTION_ARGS)
{
	TransactionId xid;
	RepOriginId nodeid;
	TimestampTz ts;
	Datum		values[3];
	bool		nulls[3];
	TupleDesc	tupdesc;
	HeapTuple	htup;

	/* 并用我们的数据构造一个元组 */
	xid = GetLatestCommitTsData(&ts, &nodeid);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (!TransactionIdIsNormal(xid))
	{
		memset(nulls, true, sizeof(nulls));
	}
	else
	{
		values[0] = TransactionIdGetDatum(xid);
		nulls[0] = false;

		values[1] = TimestampTzGetDatum(ts);
		nulls[1] = false;

		values[2] = ObjectIdGetDatum((Oid) nodeid);
		nulls[2] = false;
	}

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * pg_xact_commit_timestamp_origin
 *
 * 可供 SQL 调用的包装，用于取得给定事务的
 * 提交时间戳与复制源。
 */
Datum
pg_xact_commit_timestamp_origin(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	RepOriginId nodeid;
	TimestampTz ts;
	Datum		values[2];
	bool		nulls[2];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	bool		found;

	found = TransactionIdGetCommitTsData(xid, &ts, &nodeid);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (!found)
	{
		memset(nulls, true, sizeof(nulls));
	}
	else
	{
		values[0] = TimestampTzGetDatum(ts);
		nulls[0] = false;

		values[1] = ObjectIdGetDatum((Oid) nodeid);
		nulls[1] = false;
	}

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * 共享 CommitTS 缓冲区数量。
 *
 * 我们使用与 CLOG 缓冲区数量非常相似的逻辑（只是随
 * 共享缓冲区增长快一倍，且最大值也高一倍）；参见
 * CLOGShmemBuffers 中的注释。
 */
Size
CommitTsShmemBuffers(void)
{
	return Min(256, Max(4, NBuffers / 256));
}

/*
 * CommitTs 的共享内存规模估算
 */
Size
CommitTsShmemSize(void)
{
	return SimpleLruShmemSize(CommitTsShmemBuffers(), 0) +
		sizeof(CommitTimestampShared);
}

/*
 * 在系统启动时初始化 CommitTs（postmaster 启动或独立
 * 后端）
 */
void
CommitTsShmemInit(void)
{
	bool		found;

	CommitTsCtl->PagePrecedes = CommitTsPagePrecedes;
	SimpleLruInit(CommitTsCtl, "CommitTs", CommitTsShmemBuffers(), 0,
				  CommitTsSLRULock, "pg_commit_ts",
				  LWTRANCHE_COMMITTS_BUFFER,
				  SYNC_HANDLER_COMMIT_TS);
	SlruPagePrecedesUnitTests(CommitTsCtl, COMMIT_TS_XACTS_PER_PAGE);

	commitTsShared = ShmemInitStruct("CommitTs shared",
									 sizeof(CommitTimestampShared),
									 &found);

	if (!IsUnderPostmaster)
	{
		Assert(!found);

		commitTsShared->xidLastCommit = InvalidTransactionId;
		TIMESTAMP_NOBEGIN(commitTsShared->dataLastCommit.time);
		commitTsShared->dataLastCommit.nodeid = InvalidRepOriginId;
		commitTsShared->commitTsActive = false;
	}
	else
		Assert(found);
}

/*
 * 本函数必须在系统安装时调用一次。
 *
 * （假定 CommitTs 目录已由 initdb 创建，且
 * CommitTsShmemInit 已被调用。）
 */
void
BootStrapCommitTs(void)
{
	/*
	 * 目前这里无事可做，与多数其他 SLRU 模块不同；段
	 * 在服务器以本模块启用状态启动时创建。参见
	 * ActivateCommitTs。
	 */
}

/*
 * 将一页 CommitTs 初始化（或重新初始化）为零。
 * 若 writeXlog 为真，还发出一条 XLOG 记录说明我们做了此事。
 *
 * 页实际上不会写出，只是在共享内存中建好。
 * 返回新页的槽号。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 */
static int
ZeroCommitTsPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(CommitTsCtl, pageno);

	if (writeXlog)
		WriteZeroPageXlogRec(pageno);

	return slotno;
}

/*
 * 必须在 postmaster 或独立后端启动期间调用一次，
 * 且在 StartupXLOG 已初始化 ShmemVariableCache->nextXid 之后。
 */
void
StartupCommitTs(void)
{
	ActivateCommitTs();
}

/*
 * 必须在 postmaster 或独立后端启动期间调用一次，
 * 且在恢复完成之后。
 */
void
CompleteCommitTsInitialization(void)
{
	/*
	 * 若功能未启用，则彻底关闭它
	 * 。这也会移除任何残留数据。
	 *
	 * 反过来，若功能已启用则激活本模块。这对主
	 * 库与备库都是必要的，因为激活取决于恢复
	 * 开始时或重放
	 * XLOG_PARAMETER_CHANGE 时的控制文件内容。
	 */
	if (!track_commit_timestamp)
		DeactivateCommitTs();
	else
		ActivateCommitTs();
}

/*
 * 在恢复期间收到 XLOG_PARAMETER_CHANGE
 * XLog 记录时激活或停用 CommitTs。
 */
void
CommitTsParameterChange(bool newvalue, bool oldvalue)
{
	/*
	 * 若本服务器上 commit_ts 模块已禁
	 * 用，而我们从主库得知那里已启用，则激活它以
	 * 便能重放将来涉及它的WAL 记录；同时在
	 * pg_control 上标记为活动。若旧值
	 * 已经设置，我们已做过此事，故不再做任何事。
	 *
	 * 若主库上模块已禁用，则这里也禁
	 * 用，除非本机本地已启用该模块。
	 *
	 * 注意这只在恢复进程中运行
	 * ，因此无锁读取没问题。
	 */
	if (newvalue)
	{
		if (!commitTsShared->commitTsActive)
			ActivateCommitTs();
	}
	else if (commitTsShared->commitTsActive)
		DeactivateCommitTs();
}

/*
 * 在必要时激活本模块。
 *		这必须发生在 postmaster 或独立后端启动期间，
 *		或在 WAL 重放中主库的 track_commit_timestamp 设置
 *		发生变更时。
 *
 * 此 SLRU 需要单独的激活/停用函数，原因是它可在启动期间
 * 启用/禁用，且主库上的激活/停用会通过重放传播到备库。
 * 其他 SLRUs 没有这一属性，它们只需在正常
 * 启动时初始化即可。
 *
 * 本函数负责创建当前活动段（若尚不存在）。原因是
 * 服务器可能已在本模块禁用状态下运行了一段时间，
 * 因而可能跳过了
 * 正常的创建点。
 */
static void
ActivateCommitTs(void)
{
	TransactionId xid;
	int			pageno;

	/*
	 * 在 bootstrap 期间不应登记提交时间戳，因此此种情况下
	 * 跳过激活。
	 */
	if (IsBootstrapProcessingMode())
		return;

	/* 若已做过，则无事可做 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (commitTsShared->commitTsActive)
	{
		LWLockRelease(CommitTsLock);
		return;
	}
	LWLockRelease(CommitTsLock);

	xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	pageno = TransactionIdToCTsPage(xid);

	/*
	 * 重新初始化我们对最新页号的认识。
	 */
	LWLockAcquire(CommitTsSLRULock, LW_EXCLUSIVE);
	CommitTsCtl->shared->latest_page_number = pageno;
	LWLockRelease(CommitTsSLRULock);

	/*
	 * 若 CommitTs 已启用，但在上一轮服务器运行中未启用，
	 * 需要把最老与最新值设为下一个 Xid；这样我们就不会
	 * 试图读取可能尚未设置的数据。
	 *
	 * XXX 若服务器先以 commitTs 启用启动，再以 commitTs
	 * 禁用启动，然后再次以启用重启，这是否有问题？
	 * 看起来没有，因为恢复结束时应有检查点把该值设为
	 * InvalidTransactionId；因此任何在没有 CommitTs
	 * 值的情况下注入新事务的机会，都会发生在
	 * oldestCommitTsXid 被临时设为
	 * Invalid 之后。
	 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (ShmemVariableCache->oldestCommitTsXid == InvalidTransactionId)
	{
		ShmemVariableCache->oldestCommitTsXid =
			ShmemVariableCache->newestCommitTsXid = ReadNextTransactionId();
	}
	LWLockRelease(CommitTsLock);

	/* 必要时创建当前段文件 */
	if (!SimpleLruDoesPhysicalPageExist(CommitTsCtl, pageno))
	{
		int			slotno;

		LWLockAcquire(CommitTsSLRULock, LW_EXCLUSIVE);
		slotno = ZeroCommitTsPage(pageno, false);
		SimpleLruWritePage(CommitTsCtl, slotno);
		Assert(!CommitTsCtl->shared->page_dirty[slotno]);
		LWLockRelease(CommitTsSLRULock);
	}

	/* 更改共享内存中的激活状态。 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	commitTsShared->commitTsActive = true;
	LWLockRelease(CommitTsLock);
}

/*
 * 停用本模块。
 *
 * 在关闭 track_commit_timestamp 参数时必须调用。
 * 这发生在 postmaster 或独立后端启动期间，或在 WAL
 * 重放期间。
 *
 * 将 CommitTs 重置为无效状态，以确保我们不会交回
 * 可能无效的数据；同时移除旧数据的段。
 */
static void
DeactivateCommitTs(void)
{
	/*
	 * 清理共享内存中的状态。
	 *
	 * 我们重置 commitTsShared 记录中的一切，以防止用户在
	 * 主库上反复激活该模块时，在备库上拿到关于最近提交
	 * 事务的令人困惑的数据。
	 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);

	commitTsShared->commitTsActive = false;
	commitTsShared->xidLastCommit = InvalidTransactionId;
	TIMESTAMP_NOBEGIN(commitTsShared->dataLastCommit.time);
	commitTsShared->dataLastCommit.nodeid = InvalidRepOriginId;

	ShmemVariableCache->oldestCommitTsXid = InvalidTransactionId;
	ShmemVariableCache->newestCommitTsXid = InvalidTransactionId;

	LWLockRelease(CommitTsLock);

	/*
	 * 移除*全部*文件。这是必要的，以
	 * 免留下残留文件；若此功能在禁用运
	 * 行一段时间后再启用，文件序列中可
	 * 能出现缺口。（我们大概可以容忍乱
	 * 序文件，因为回卷时反正会被覆盖
	 * ，但看起来还是整齐一些更好。）
	 */
	LWLockAcquire(CommitTsSLRULock, LW_EXCLUSIVE);
	(void) SlruScanDirectory(CommitTsCtl, SlruScanDirCbDeleteAll, NULL);
	LWLockRelease(CommitTsSLRULock);
}

/*
 * 执行检查点——无论是在关机期间还是联机执行
 */
void
CheckPointCommitTs(void)
{
	/*
	 * 将脏 CommitTs 页写出到磁盘。这可能导致同步请求
	 * 排队，稍后由 ProcessSyncRequests() 作为检查点的一部分
	 * 处理。
	 */
	SimpleLruWriteAll(CommitTsCtl, true);
}

/*
 * 确保 CommitTs 有空间容纳新分配的 XID。
 *
 * NB：调用时持有 XidGenLock。我们希
 * 望它在多数时候非常快；即使不那么快，除非被迫
 * 写出脏 CommitTs 或 xlog 页以
 * 腾出共享内存空间，否则也不需要实际 I/O。
 *
 * NB：当前实现依赖
 * track_commit_timestamp 为PGC_POSTMASTER。
 */
void
ExtendCommitTs(TransactionId newestXact)
{
	int			pageno;

	/*
	 * 若模块未启用则无事可做。注意此处对标志做无锁读取，
	 * 这没问题，因为本例程仅由 GetNewTransactionId 调用，
	 * 而后者从不在备库上调用。
	 */
	Assert(!InRecovery);
	if (!commitTsShared->commitTsActive)
		return;

	/*
	 * 除页上第一个 XID 外无工作。但要注意：刚回卷后，
	 * 第零页的第一个 XID 是 FirstNormalTransactionId。
	 */
	if (TransactionIdToCTsEntry(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToCTsPage(newestXact);

	LWLockAcquire(CommitTsSLRULock, LW_EXCLUSIVE);

	/* 清零该页并为此记一条 XLOG 项 */
	ZeroCommitTsPage(pageno, !InRecovery);

	LWLockRelease(CommitTsSLRULock);
}

/*
 * 移除持有所传事务 ID 的那个段之前的全部
 * CommitTs 段。
 *
 * 注意这里无需刷写 XLOG。
 */
void
TruncateCommitTs(TransactionId oldestXact)
{
	int			cutoffPage;

	/*
	 * 截断点是包含 oldestXact 的段的起始处。我们
	 * 把包含 oldestXact 的*页*传给 SimpleLruTruncate。
	 */
	cutoffPage = TransactionIdToCTsPage(oldestXact);

	/* 检查是否有可移除的文件 */
	if (!SlruScanDirectory(CommitTsCtl, SlruScanDirCbReportPresence,
						   &cutoffPage))
		return;					/* 没什么可移除 */

	/* 写 XLOG 记录 */
	WriteTruncateXlogRec(cutoffPage, oldestXact);

	/* 现在可以移除旧的 CommitTs segment(s) */
	SimpleLruTruncate(CommitTsCtl, cutoffPage);
}

/*
 * 设置可查询提交时间戳的界限值。
 */
void
SetCommitTsLimit(TransactionId oldestXact, TransactionId newestXact)
{
	/*
	 * 小心不要覆盖那些更靠"未来"或表示
	 * committs 已禁用的值。
	 */
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (ShmemVariableCache->oldestCommitTsXid != InvalidTransactionId)
	{
		if (TransactionIdPrecedes(ShmemVariableCache->oldestCommitTsXid, oldestXact))
			ShmemVariableCache->oldestCommitTsXid = oldestXact;
		if (TransactionIdPrecedes(newestXact, ShmemVariableCache->newestCommitTsXid))
			ShmemVariableCache->newestCommitTsXid = newestXact;
	}
	else
	{
		Assert(ShmemVariableCache->newestCommitTsXid == InvalidTransactionId);
		ShmemVariableCache->oldestCommitTsXid = oldestXact;
		ShmemVariableCache->newestCommitTsXid = newestXact;
	}
	LWLockRelease(CommitTsLock);
}

/*
 * 向前推进可查询的最老 commitTS 值
 */
void
AdvanceOldestCommitTsXid(TransactionId oldestXact)
{
	LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
	if (ShmemVariableCache->oldestCommitTsXid != InvalidTransactionId &&
		TransactionIdPrecedes(ShmemVariableCache->oldestCommitTsXid, oldestXact))
		ShmemVariableCache->oldestCommitTsXid = oldestXact;
	LWLockRelease(CommitTsLock);
}


/*
 * 判定某 commitTS 页号对截断而言是否"更老"。
 * 类似于 CLOGPagePrecedes()。
 *
 * 在默认 BLCKSZ 下，(1 << 31) % COMMIT_TS_XACTS_PER_PAGE == 128。这
 * 引入了与 CLOG 及其他 SLRUs 在 (1 <<
 * 31) % per_page == 0 时的差异。本函数从不精确测试
 * TransactionIdPrecedes(x-2^31, x)。当系统到达 xidStopLimit 时，
 * oldestXact 与最新已分配 XID 之间页边界的计数有两种可能，
 * 取决于 oldestXact 是否在其页的前
 * 128 个项之内。由于本函数不知道 oldestXact 在
 * page2 内的位置，它对实际上可丢弃的一页返回 false。
 * 这是 CLOGPagePrecedes() 无法识别的截断机会的更宽
 * （但仍可忽略）版本。
 *
 * 为便于演算示例，用十进制为项编号，使得
 * page1==1 的项范围为 1.0 到 1.999。令 N+0.15 为 2^31 个项
 * 将跨越的页数（N 为整数）。若 oldestXact=N+2.1，
 * 则最后一次安全的 XID 分配使 newestXact=1.95。我们保留第 2 页，
 * 因为 entry=2.85 是切换项是否先于
 * oldestXact 页最后一项的边界。虽然在
 * oldestXact=N+2.1 时可丢弃第 2 页，但在 oldestXact=N+2.9 时它将是宝贵的。
 */
static bool
CommitTsPagePrecedes(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * COMMIT_TS_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * COMMIT_TS_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + COMMIT_TS_XACTS_PER_PAGE - 1));
}


/*
 * 写一条 ZEROPAGE xlog 记录
 */
static void
WriteZeroPageXlogRec(int pageno)
{
	XLogBeginInsert();
	XLogRegisterData((char *) (&pageno), sizeof(int));
	(void) XLogInsert(RM_COMMIT_TS_ID, COMMIT_TS_ZEROPAGE);
}

/*
 * 写一条 TRUNCATE xlog 记录
 */
static void
WriteTruncateXlogRec(int pageno, TransactionId oldestXid)
{
	xl_commit_ts_truncate xlrec;

	xlrec.pageno = pageno;
	xlrec.oldestXid = oldestXid;

	XLogBeginInsert();
	XLogRegisterData((char *) (&xlrec), SizeOfCommitTsTruncate);
	(void) XLogInsert(RM_COMMIT_TS_ID, COMMIT_TS_TRUNCATE);
}

/*
 * CommitTS 资源管理器的例程
 */
void
commit_ts_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* commit_ts 记录不使用备份块 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == COMMIT_TS_ZEROPAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(CommitTsSLRULock, LW_EXCLUSIVE);

		slotno = ZeroCommitTsPage(pageno, false);
		SimpleLruWritePage(CommitTsCtl, slotno);
		Assert(!CommitTsCtl->shared->page_dirty[slotno]);

		LWLockRelease(CommitTsSLRULock);
	}
	else if (info == COMMIT_TS_TRUNCATE)
	{
		xl_commit_ts_truncate *trunc = (xl_commit_ts_truncate *) XLogRecGetData(record);

		AdvanceOldestCommitTsXid(trunc->oldestXid);

		/*
		 * 在 XLOG 重放期间，latest_page_number 尚未设置；插入一个
		 * 合适的值以绕过 SimpleLruTruncate 中的健全性测试。
		 */
		CommitTsCtl->shared->latest_page_number = trunc->pageno;

		SimpleLruTruncate(CommitTsCtl, trunc->pageno);
	}
	else
		elog(PANIC, "commit_ts_redo: unknown op code %u", info);
}

/*
 * sync.c 同步 commit_ts 文件的入口点。
 */
int
committssyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(CommitTsCtl, ftag, path);
}
