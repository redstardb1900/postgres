/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID 与 XID 变量支持例程
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/varsup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlogutils.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/syscache.h"


/* 每次 XLOG 写入预取（预分配）的 OIDs 数量 */
#define VAR_OID_PREFETCH		8192

/* 指向共享内存中"变量缓存"的指针（由 shmem.c 设置） */
VariableCache ShmemVariableCache = NULL;


/*
 * 为新事务或子事务分配下一个
 * FullTransactionId。
 *
 * 返回前还会把新 XIDs 存入
 * MyProc->xid/ProcGlobal->xids[]。
 *
 * 注意：调用本函数时，其实已处于有效事
 * 务中，因为现在要等到事务真正做事后才
 * 分配 XID。因此若要就XID 回卷
 * 发出警告，做一次数据库查找是安全的。
 */
FullTransactionId
GetNewTransactionId(bool isSubXact)
{
	FullTransactionId full_xid;
	TransactionId xid;

	/*
	 * 并行工作者在每次并行操作开始时同步事务状态，
	 * 因此之后不能再记入新的 XIDs。
	 */
	if (IsInParallelMode())
		elog(ERROR, "cannot assign TransactionIds during a parallel operation");

	/*
	 * 在 bootstrap 初始化期间，返回特殊的 bootstrap
	 * 事务 id。
	 */
	if (IsBootstrapProcessingMode())
	{
		Assert(!isSubXact);
		MyProc->xid = BootstrapTransactionId;
		ProcGlobal->xids[MyProc->pgxactoff] = BootstrapTransactionId;
		return FullTransactionIdFromEpochAndXid(0, BootstrapTransactionId);
	}

	/* 安全检查：在 HS 备库上绝不应该走到这里 */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign TransactionIds during recovery");

	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

	full_xid = ShmemVariableCache->nextXid;
	xid = XidFromFullTransactionId(full_xid);

	/*----------
	 * 检查再分配另一个 XID 是否安全。这用于防止因
	 * XID 回卷导致的灾难性数据丢失。基本规则是：
	 *
	 * 若已超过 xidVacLimit，开始设法强制自动清理周期。
	 * 若已超过 xidWarnLimit，开始发出警告。
	 * 若已超过 xidStopLimit，拒绝执行事务，除非
	 * 运行在单用户模式（给不知何故越过先前防线的
	 * DBA 留一条逃生通道）。
	 *
	 * 注意：这段逻辑同样出现在 GetNewMultiXactId 中。
	 *----------
	 */
	if (TransactionIdFollowsOrEquals(xid, ShmemVariableCache->xidVacLimit))
	{
		/*
		 * 为安全起见，发送信号、警告等时释放
		 * XidGenLock。与其说是为了在此情况下
		 * 保持并发，不如说是为了避免在
		 * get_database_name() 时出现死
		 * 锁的任何可能。首先，复制本路径所需的全部共享值。
		 */
		TransactionId xidWarnLimit = ShmemVariableCache->xidWarnLimit;
		TransactionId xidStopLimit = ShmemVariableCache->xidStopLimit;
		TransactionId xidWrapLimit = ShmemVariableCache->xidWrapLimit;
		Oid			oldest_datoid = ShmemVariableCache->oldestXidDB;

		LWLockRelease(XidGenLock);

		/*
		 * 为避免用信号淹没 postmaster，每 64K 次事务启动
		 * 才发出一次自动清理请求。这在真正出问题前
		 * 仍有足够多次机会。
		 */
		if (IsUnderPostmaster && (xid % 65536) == 0)
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

		if (IsUnderPostmaster &&
			TransactionIdFollowsOrEquals(xid, xidStopLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/* 即使该数据库已消失也要报错 */
			if (oldest_datname)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands to avoid wraparound data loss in database \"%s\"",
								oldest_datname),
						 errhint("Stop the postmaster and vacuum that database in single-user mode.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands to avoid wraparound data loss in database with OID %u",
								oldest_datoid),
						 errhint("Stop the postmaster and vacuum that database in single-user mode.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		}
		else if (TransactionIdFollowsOrEquals(xid, xidWarnLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/* 即使该数据库已消失也要抱怨 */
			if (oldest_datname)
				ereport(WARNING,
						(errmsg("database \"%s\" must be vacuumed within %u transactions",
								oldest_datname,
								xidWrapLimit - xid),
						 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(WARNING,
						(errmsg("database with OID %u must be vacuumed within %u transactions",
								oldest_datoid,
								xidWrapLimit - xid),
						 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		}

		/* 重新获取锁并从头开始 */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
		full_xid = ShmemVariableCache->nextXid;
		xid = XidFromFullTransactionId(full_xid);
	}

	/*
	 * 若正在分配提交日志新页的第一个 XID，则在返回前
	 * 先将该提交日志页清零。必须在持有 XidGenLock 时
	 * 完成，否则另一事务可能在我们清零页之前获取并提交
	 * 更大的 XID。幸好提交日志一页可容纳 32K 或更多事务，
	 * 因此不必经常这样做。
	 *
	 * 同时扩展 pg_subtrans 与 pg_commit_ts。
	 */
	ExtendCLOG(xid);
	ExtendCommitTs(xid);
	ExtendSUBTRANS(xid);

	/*
	 * 现在推进 nextXid 计数器。必须在成功完成
	 * ExtendCLOG() 之后才能推进——若该例程失败，我们希望
	 * 下一个进入的事务再试一次。在有足够 CLOG 空间之前
	 * 无法再分配更多 XIDs。
	 */
	FullTransactionIdAdvance(&ShmemVariableCache->nextXid);

	/*
	 * 必须在释放 XidGenLock 之前把新 XID 存入共享
	 * ProcArray。这确保每个比 latestCompletedXid 更老的
	 * 活动 XID 都出现在 ProcArray 中，这对正确跟踪
	 * OldestXmin 至关重要；参见 src/backend/access/transam/README。
	 *
	 * 注意：读取 ProcGlobal->xids/PGPROC->xid 的读者应小心
	 * 对每个进程只取一次该值，而不要假定可以多次读取并
	 * 每次得到相同答案。注意我们假定 TransactionId 与 int
	 * 的取存是原子的。
	 *
	 * 同样的说明适用于子事务 xid 计数与溢出字段。
	 *
	 * 使用写屏障可防止本函数中危险的代码重排；否则其他
	 * 后端例如可能同时检查我的
	 * subxids 信息，我们不希望它们看到无效的
	 * 中间状态，比如数组项尚未填入就已
	 * 递增 nxids。
	 *
	 * 读取 nxids 的其他进程应在读取 xids
	 * 元素之前先读 nxids，中间用 pg_read_barrier()，
	 * 以确保不会读到未初始化的数组元素；参见
	 * src/backend/storage/lmgr/README.barrier。
	 *
	 * 若 PGPROC 中已无空间容纳子事务 XID，则改为设置
	 * 缓存溢出标志。这迫使读者到
	 * pg_subtrans 中把子事务 XIDs 映射到顶层 XIDs。存在一个
	 * 竞态窗口：新 XID 在其父链接写入 pg_subtrans 之前
	 * 不会显示为正在运行。不过，那会在任何人可能有理由
	 * 查询该 XID 状态之前发生，因此似乎可以接受。
	 * （此窗口内取得的快照*会*包含父 XID，所以
	 * 稍后有人确实需要查询时会得到
	 * 正确答案。）
	 */
	if (!isSubXact)
	{
		Assert(ProcGlobal->subxidStates[MyProc->pgxactoff].count == 0);
		Assert(!ProcGlobal->subxidStates[MyProc->pgxactoff].overflowed);
		Assert(MyProc->subxidStatus.count == 0);
		Assert(!MyProc->subxidStatus.overflowed);

		/* LWLockRelease 充当屏障 */
		MyProc->xid = xid;
		ProcGlobal->xids[MyProc->pgxactoff] = xid;
	}
	else
	{
		XidCacheStatus *substat = &ProcGlobal->subxidStates[MyProc->pgxactoff];
		int			nxids = MyProc->subxidStatus.count;

		Assert(substat->count == MyProc->subxidStatus.count);
		Assert(substat->overflowed == MyProc->subxidStatus.overflowed);

		if (nxids < PGPROC_MAX_CACHED_SUBXIDS)
		{
			MyProc->subxids.xids[nxids] = xid;
			pg_write_barrier();
			MyProc->subxidStatus.count = substat->count = nxids + 1;
		}
		else
			MyProc->subxidStatus.overflowed = substat->overflowed = true;
	}

	LWLockRelease(XidGenLock);

	return full_xid;
}

/*
 * 读取 nextXid 但不分配它。
 */
FullTransactionId
ReadNextFullTransactionId(void)
{
	FullTransactionId fullXid;

	LWLockAcquire(XidGenLock, LW_SHARED);
	fullXid = ShmemVariableCache->nextXid;
	LWLockRelease(XidGenLock);

	return fullXid;
}

/*
 * 将 nextXid 推进到给定 xid 之后的值。纪元由推断得出。
 * 仅可在恢复期间或两阶段启动代码中调用。
 */
void
AdvanceNextFullTransactionIdPastXid(TransactionId xid)
{
	FullTransactionId newNextFullXid;
	TransactionId next_xid;
	uint32		epoch;

	/*
	 * 不持锁读取 nextXid 是安全
	 * 的，因为仅从启动进程或单进程模式调
	 * 用，意味着没有其他进程会修改它。
	 */
	Assert(AmStartupProcess() || !IsUnderPostmaster);

	/* 若此 xid 不够大、不足以推动指针，则快速返回。 */
	next_xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	if (!TransactionIdFollowsOrEquals(xid, next_xid))
		return;

	/*
	 * 计算给定 xid 之后的
	 * FullTransactionId。为此保留现有纪元
	 * ，但检测何时已回卷进入新纪元。这是必要的，因为
	 * WAL 记录与 2PC 状态目前包含 32 位
	 * xid。在这些情况下回卷逻辑是安全的，因为在 WAL
	 * 流的任意给定点，活动xid 的跨度都无法超过一个纪元。
	 */
	TransactionIdAdvance(xid);
	epoch = EpochFromFullTransactionId(ShmemVariableCache->nextXid);
	if (unlikely(xid < next_xid))
		++epoch;
	newNextFullXid = FullTransactionIdFromEpochAndXid(epoch, xid);

	/*
	 * 在有并发读者时，仍需
	 * 加锁才能修改该值。
	 */
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	ShmemVariableCache->nextXid = newNextFullXid;
	LWLockRelease(XidGenLock);
}

/*
 * 推进集簇范围内最老有效 clog 项的值。
 *
 * 推进 oldestClogXid 必须获取
 * XactTruncationLock。实际执行clog 截
 * 断时不必持锁，只有推进该界限时才需要，因为查找任意
 * xid 的代码须从测试 oldestClogXid 起持有
 * XactTruncationLock，直到完成 clog 查找。
 */
void
AdvanceOldestClogXid(TransactionId oldest_datfrozenxid)
{
	LWLockAcquire(XactTruncationLock, LW_EXCLUSIVE);
	if (TransactionIdPrecedes(ShmemVariableCache->oldestClogXid,
							  oldest_datfrozenxid))
	{
		ShmemVariableCache->oldestClogXid = oldest_datfrozenxid;
	}
	LWLockRelease(XactTruncationLock);
}

/*
 * 根据当前最老的 datfrozenxid（即我们集簇中任一数据库里
 * 可能存在的最老 XID）以及具有该值的（或某个）数据库的
 * OID，确定可安全分配的最后一个 XID。
 */
void
SetTransactionIdLimit(TransactionId oldest_datfrozenxid, Oid oldest_datoid)
{
	TransactionId xidVacLimit;
	TransactionId xidWarnLimit;
	TransactionId xidStopLimit;
	TransactionId xidWrapLimit;
	TransactionId curXid;

	Assert(TransactionIdIsNormal(oldest_datfrozenxid));

	/*
	 * 真正陷入严重麻烦的位置，是从可能仍
	 * 存在的最老 XIDs起绕环一半处。
	 * （此计算可能差一两个计数，因为特殊
	 * XID 会略微缩小环的大小。但下
	 * 面留了充足余量，因此无关紧要。）
	 */
	xidWrapLimit = oldest_datfrozenxid + (MaxTransactionId >> 1);
	if (xidWrapLimit < FirstNormalTransactionId)
		xidWrapLimit += FirstNormalTransactionId;

	/*
	 * 在交互模式下，一旦距数据丢失还剩 300 万事务，
	 * 我们就拒绝继续分配 XIDs。这给 DBA 在独立后
	 * 端中修修补补留下充足空间，同时相对于整个 XID
	 * 空间并不显著。（若
	 * wal_level!=minimal，VACUUM 在截
	 * 断时需要一个 XID。 DBA 可能下意识执行的 "
	 * VACUUM (ANALYZE)" 也会分配 XID。因
	 * 此最好确保还剩很多 XID……）另外，在默认
	 * BLCKSZ 下，这还留下两个完全空闲的段。若存在涉及页或段
	 * 算术的边界错误，空闲段可使这些错误在单用户模式之外不可达。
	 */
	xidStopLimit = xidWrapLimit - 3000000;
	if (xidStopLimit < FirstNormalTransactionId)
		xidStopLimit -= FirstNormalTransactionId;

	/*
	 * 一旦距数据丢失还剩 4000 万事务，
	 * 我们就开始大声抱怨。这个数字有些随意，
	 * 但如果油表只剩 2%，你难道不该找下一
	 * 站加油站吗？这个数字需要相当宽松，因
	 * 为有很多场景下大多数事务由不会理会警
	 * 告的自动客户端完成。 （不，我们不会
	 * 把它做成可配置的。若你懂到能配置它，
	 * 你也懂到不会一开始就陷入这种麻烦。）
	 */
	xidWarnLimit = xidWrapLimit - 40000000;
	if (xidWarnLimit < FirstNormalTransactionId)
		xidWarnLimit -= FirstNormalTransactionId;

	/*
	 * 一旦 oldest_datfrozenxid 超过
	 * autovacuum_freeze_max_age个事务那么老，我们就开始设法强制自动清理。
	 *
	 * 注意：guc.c 确保 autovacuum_freeze_max_age 处于
	 * 合理范围，从而使 xidVacLimit 远在 xidWarnLimit 之前。
	 *
	 * 注意：autovacuum_freeze_ma
	 * x_age 是 PGC_POSTMASTER
	 * 参数，这样我们就不必担心处理其值的即时变更。从
	 *  GUC 赋值钩子更新共享状态看起来并不现实
	 * （太多进程会尝试执行该钩子，导致竞态，以及那些
	 * 未连接共享内存的进程崩溃）。也许将来可以改进。
	 * 另见 SetMultiXactIdLimit。
	 */
	xidVacLimit = oldest_datfrozenxid + autovacuum_freeze_max_age;
	if (xidVacLimit < FirstNormalTransactionId)
		xidVacLimit += FirstNormalTransactionId;

	/* 只持锁足够长的时间以设置新的界限值 */
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	ShmemVariableCache->oldestXid = oldest_datfrozenxid;
	ShmemVariableCache->xidVacLimit = xidVacLimit;
	ShmemVariableCache->xidWarnLimit = xidWarnLimit;
	ShmemVariableCache->xidStopLimit = xidStopLimit;
	ShmemVariableCache->xidWrapLimit = xidWrapLimit;
	ShmemVariableCache->oldestXidDB = oldest_datoid;
	curXid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	LWLockRelease(XidGenLock);

	/* 记录该信息 */
	ereport(DEBUG1,
			(errmsg_internal("transaction ID wrap limit is %u, limited by database with OID %u",
							 xidWrapLimit, oldest_datoid)));

	/*
	 * 若已过强制自动清理点，立即发信号请求自
	 * 动清理。原因是自动清理每次调用只处理一
	 * 个数据库。清理完最老的数据库后会调用到
	 * 这里；若仍有任何老数据库，我们就通知
	 * postmaster 立即开始下一轮。
	 */
	if (TransactionIdFollowsOrEquals(curXid, xidVacLimit) &&
		IsUnderPostmaster && !InRecovery)
		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

	/* 若已过回卷警告点，立即给出警告 */
	if (TransactionIdFollowsOrEquals(curXid, xidWarnLimit) && !InRecovery)
	{
		char	   *oldest_datname;

		/*
		 * 我们可能在不处于事务中时被调用，例如在
		 * StartupXLOG() 期间。这种情况下无法做数据库访问，
		 * 因此只能报告最老数据库的 OID。
		 *
		 * 注意：get_database_name 也可能失败并返回 NULL，
		 * 例如因为数据库刚被删除。我们仍会警告，即使
		 * 警告此时可能已不必要。
		 */
		if (IsTransactionState())
			oldest_datname = get_database_name(oldest_datoid);
		else
			oldest_datname = NULL;

		if (oldest_datname)
			ereport(WARNING,
					(errmsg("database \"%s\" must be vacuumed within %u transactions",
							oldest_datname,
							xidWrapLimit - curXid),
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		else
			ereport(WARNING,
					(errmsg("database with OID %u must be vacuumed within %u transactions",
							oldest_datoid,
							xidWrapLimit - curXid),
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
	}
}


/*
 * ForceTransactionIdLimitUpdate -- XID 回卷界限数据是否需要更新？
 *
 * 我们主要检查 oldestXidDB 是否有效。
 * 考虑的情形包括该数据库已被删除，或该字段被
 * pg_resetwal 重置为零。无论哪种情况都
 * 应强制重新计算回卷界限。另外，若
 * oldestXid 已老到足以强制自动清理或其他动作，也
 * 要更新；这确保一旦开始产生额外开销，我们尽快更新状态。
 */
bool
ForceTransactionIdLimitUpdate(void)
{
	TransactionId nextXid;
	TransactionId xidVacLimit;
	TransactionId oldestXid;
	Oid			oldestXidDB;

	/* 加锁也许并非真正必要，但还是谨慎一些 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	nextXid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	xidVacLimit = ShmemVariableCache->xidVacLimit;
	oldestXid = ShmemVariableCache->oldestXid;
	oldestXidDB = ShmemVariableCache->oldestXidDB;
	LWLockRelease(XidGenLock);

	if (!TransactionIdIsNormal(oldestXid))
		return true;			/* 不应发生，但以防万一 */
	if (!TransactionIdIsValid(xidVacLimit))
		return true;			/* 这也不应再发生 */
	if (TransactionIdFollowsOrEquals(nextXid, xidVacLimit))
		return true;			/* 已过 xidVacLimit，不要推迟更新 */
	if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(oldestXidDB)))
		return true;			/* 可能发生，见上文注释 */
	return false;
}


/*
 * GetNewObjectId -- 分配一个新 OID
 *
 * OIDs 由集簇范围的计数器生成。由于它们只有 32 位
 * 宽，计数器最终会回卷，因此除非采取措施保证唯一性，否
 * 则假定它们唯一是不明智的。因此，一般不应直接使用本例
 * 程。唯一的直接调用者应是 catalog/
 * catalog.c 中的
 * GetNewOidWithIndex() 与 GetNewRelFileNumber()。
 */
Oid
GetNewObjectId(void)
{
	Oid			result;

	/* 安全检查：在 HS 备库上绝不应该走到这里 */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign OIDs during recovery");

	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	/*
	 * 检查 OIDs 计数器是否回卷。我们*绝不能*返回 0
	 * (InvalidOid)，且在正常运行中不得返回低于
	 * FirstNormalObjectId 的任何值，因为该范围保留给
	 * initdb（见 IsCatalogRelationOid()）。注意我们依赖无符号比较。
	 *
	 * 在 initdb 期间，OID 生成器从
	 * FirstGenbkiObjectId 开始，因此仅在
	 * bootstrap 或独立模式下低于该点时才回卷。正常
	 * postmaster 启动后第一次进入本例程时，计数器会被强制提升到
	 * FirstNormalObjectId。此机制使
	 * FirstGenbkiObjectId 与
	 * FirstNormalObjectId 之间的 OID可在 initdb 期间自动分配，同时确保它们永不与用户分配的 OID 冲突。
	 */
	if (ShmemVariableCache->nextOid < ((Oid) FirstNormalObjectId))
	{
		if (IsPostmasterEnvironment)
		{
			/* 正常模式下回卷，或 initdb 后首次分配 */
			ShmemVariableCache->nextOid = FirstNormalObjectId;
			ShmemVariableCache->oidCount = 0;
		}
		else
		{
			/* 可能在 bootstrap，故不强制完整范围 */
			if (ShmemVariableCache->nextOid < ((Oid) FirstGenbkiObjectId))
			{
				/* 独立模式下回卷（少见但可能） */
				ShmemVariableCache->nextOid = FirstNormalObjectId;
				ShmemVariableCache->oidCount = 0;
			}
		}
	}

	/* 若已记录供使用的 oid 用尽，则必须再记录更多 */
	if (ShmemVariableCache->oidCount == 0)
	{
		XLogPutNextOid(ShmemVariableCache->nextOid + VAR_OID_PREFETCH);
		ShmemVariableCache->oidCount = VAR_OID_PREFETCH;
	}

	result = ShmemVariableCache->nextOid;

	(ShmemVariableCache->nextOid)++;
	(ShmemVariableCache->oidCount)--;

	LWLockRelease(OidGenLock);

	return result;
}

/*
 * SetNextObjectId
 *
 * 仅可在 initdb 期间调用；
 * 将 OID 计数器推进到指定值。
 */
static void
SetNextObjectId(Oid nextOid)
{
	/* 安全检查：仅在 initdb 期间允许 */
	if (IsPostmasterEnvironment)
		elog(ERROR, "cannot advance OID counter anymore");

	/* 因此加锁只是形式上的；但还是要做 */
	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	if (ShmemVariableCache->nextOid > nextOid)
		elog(ERROR, "too late to advance OID counter to %u, it is now %u",
			 nextOid, ShmemVariableCache->nextOid);

	ShmemVariableCache->nextOid = nextOid;
	ShmemVariableCache->oidCount = 0;

	LWLockRelease(OidGenLock);
}

/*
 * StopGeneratingPinnedObjectIds
 *
 * 在 initdb 期间调用一次，强制把 OID 计数器提升到
 * FirstUnpinnedObjectId。这支持让 initdb 的 post-bootstrap
 * 处理尽早创建一些钉住对象。完成后它（通过
 * pg_stop_making_pinned_objects()）调用本函数，使之后创建的
 * 其余对象被视为未钉住。
 */
void
StopGeneratingPinnedObjectIds(void)
{
	SetNextObjectId(FirstUnpinnedObjectId);
}


#ifdef USE_ASSERT_CHECKING

/*
 * 断言 xid 位于 [oldestXid, nextXid] 之间，这是我们期望
 * 来自表等处的 XIDs 所处的范围。
 *
 * 由于本调用之后若不进一步防护，ShmemVariableCache->oldestXid
 * 可能立即改变，且回卷后的 xid 可能再次落入
 * 有效范围，此断言只能检测出肯定有问题的情况，
 * 而不能确立正确性。
 *
 * 故意不暴露返回值，以避免引入依赖返回值的
 * 代码。
 */
void
AssertTransactionIdInAllowableRange(TransactionId xid)
{
	TransactionId oldest_xid;
	TransactionId next_xid;

	Assert(TransactionIdIsValid(xid));

	/* 我们可能看到 bootstrap / 冻结 */
	if (!TransactionIdIsNormal(xid))
		return;

	/*
	 * 不能获取 XidGenLock，因为本函数可能在已持有
	 * XidGenLock 时被调用（或持有不允许嵌套
	 * XidGenLock 的其他锁）。但对我们的目的来说没问题
	 * ，因为我们已经依赖32 位读取是原子的。虽然
	 * nextXid 是 64 位，我们只看低 32 位，因此偏斜读取无妨。
	 *
	 * 不持锁访问它们，并不会增加落到 [oldest,
	 * next] 之外的危险。xid 须已在发起会话中由
	 * GetNewTransactionId()创建，那里的锁
	 * 与下方内存屏障配对。不过我们接受 xid可以 <=
	 * next_xid，而不只是 <，因为 xid 可能来自
	 * procarray，早于我们看到更新后的 nextXid 值。
	 */
	pg_memory_barrier();
	oldest_xid = ShmemVariableCache->oldestXid;
	next_xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);

	Assert(TransactionIdFollowsOrEquals(xid, oldest_xid) ||
		   TransactionIdPrecedesOrEquals(xid, next_xid));
}
#endif
