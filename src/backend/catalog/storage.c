/*-------------------------------------------------------------------------
 *
 * storage.c
 *	  为关系创建与销毁物理存储的代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/storage.c
 *
 * NOTES
 *	  这些代码中有一部分曾位于 storage/smgr/smgr.c，函数
 *	  名仍反映那一点。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* GUC 变量 */
int			wal_skip_threshold = 2048;	/* 以千字节计 */

/*
 * 我们维护当前事务中已创建或删除的所有关系
 * （以 RelFileLocator 值表示）的列表。当
 * 创建关系时，我们立即创建物理文件，但
 * 记住它，以便若当前事务中止，我们可以再次
 * 删除该文件。相反，删除请求并不
 * 立即执行，而只是记入列表。当且仅当
 * 事务提交时，我们才能删除物理文件。
 *
 * 为处理子事务，每个条目都标有其事务
 * 嵌套级别。在子事务提交时，我们把子事务的
 * 条目重新归属到父嵌套级别。在子事务中止时，我们可以
 * 立即对当前嵌套级别的所有条目执行
 * 中止时动作。
 *
 * 注意：该列表保存在 TopMemoryContext 中，以确保它不会
 * 不合时宜地消失。把它放在 TopTransactionContext 中大概也可以，
 * 但我比较谨慎。
 */

typedef struct PendingRelDelete
{
	RelFileLocator rlocator;	/* 可能需要删除的关系 */
	BackendId	backend;		/* 若非临时关系则为 InvalidBackendId */
	bool		atCommit;		/* T=提交时删除；F=中止时删除 */
	int			nestLevel;		/* 请求的事务嵌套级别 */
	struct PendingRelDelete *next;	/* 链表链接 */
} PendingRelDelete;

typedef struct PendingRelSync
{
	RelFileLocator rlocator;
	bool		is_truncated;	/* 该文件是否经历过截断？ */
} PendingRelSync;

static PendingRelDelete *pendingDeletes = NULL; /* 链表头 */
static HTAB *pendingSyncHash = NULL;


/*
 * AddPendingSync
 *		排队一个提交时的 fsync。
 */
static void
AddPendingSync(const RelFileLocator *rlocator)
{
	PendingRelSync *pending;
	bool		found;

	/* 若尚未创建则创建哈希 */
	if (!pendingSyncHash)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(PendingRelSync);
		ctl.hcxt = TopTransactionContext;
		pendingSyncHash = hash_create("pending sync hash", 16, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	pending = hash_search(pendingSyncHash, rlocator, HASH_ENTER, &found);
	Assert(!found);
	pending->is_truncated = false;
}

/*
 * RelationCreateStorage
 *		为关系创建物理存储。
 *
 * 为关系创建底层磁盘文件存储。这只
 * 创建主分支；额外分支由需要它们的模块
 * 惰性创建。
 *
 * 本函数是事务性的。创建会被记入 WAL，若事务
 * 随后中止，存储将被销毁。不想在中止时
 * 销毁存储的调用方可传入
 * register_delete = false。
 */
SMgrRelation
RelationCreateStorage(RelFileLocator rlocator, char relpersistence,
					  bool register_delete)
{
	SMgrRelation srel;
	BackendId	backend;
	bool		needs_wal;

	Assert(!IsInParallelMode());	/* 无法更新 pendingSyncHash */

	switch (relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			backend = BackendIdForTempRelations();
			needs_wal = false;
			break;
		case RELPERSISTENCE_UNLOGGED:
			backend = InvalidBackendId;
			needs_wal = false;
			break;
		case RELPERSISTENCE_PERMANENT:
			backend = InvalidBackendId;
			needs_wal = true;
			break;
		default:
			elog(ERROR, "invalid relpersistence: %c", relpersistence);
			return NULL;		/* 安抚编译器 */
	}

	srel = smgropen(rlocator, backend);
	smgrcreate(srel, MAIN_FORKNUM, false);

	if (needs_wal)
		log_smgrcreate(&srel->smgr_rlocator.locator, MAIN_FORKNUM);

	/*
	 * 若被要求如此，则把该关系加入中止时
	 * 要删除的列表。
	 */
	if (register_delete)
	{
		PendingRelDelete *pending;

		pending = (PendingRelDelete *)
			MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
		pending->rlocator = rlocator;
		pending->backend = backend;
		pending->atCommit = false;	/* 中止时删除 */
		pending->nestLevel = GetCurrentTransactionNestLevel();
		pending->next = pendingDeletes;
		pendingDeletes = pending;
	}

	if (relpersistence == RELPERSISTENCE_PERMANENT && !XLogIsNeeded())
	{
		Assert(backend == InvalidBackendId);
		AddPendingSync(&rlocator);
	}

	return srel;
}

/*
 * 对 XLOG_SMGR_CREATE 记录执行 XLogInsert 到 WAL。
 */
void
log_smgrcreate(const RelFileLocator *rlocator, ForkNumber forkNum)
{
	xl_smgr_create xlrec;

	/*
	 * 做一条报告文件创建的 XLOG 条目。
	 */
	xlrec.rlocator = *rlocator;
	xlrec.forkNum = forkNum;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogInsert(RM_SMGR_ID, XLOG_SMGR_CREATE | XLR_SPECIAL_REL_UPDATE);
}

/*
 * RelationDropStorage
 *		安排在事务提交时解除链接物理存储。
 */
void
RelationDropStorage(Relation rel)
{
	PendingRelDelete *pending;

	/* 把该关系加入提交时要删除的列表 */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->rlocator = rel->rd_locator;
	pending->backend = rel->rd_backend;
	pending->atCommit = true;	/* 提交时删除 */
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;

	/*
	 * 注意：若该关系是在本事务中创建的，它现在会
	 * 在待删除列表中出现两次，一次 atCommit 为 true，
	 * 一次 atCommit 为 false。因此，无论哪种情况它都会在事务
	 * 结束时被物理删除（另一条目会被
	 * smgrDoPendingDeletes 忽略，因此不会出错）。我们也可以改为移除
	 * 已有列表条目并立即删除物理文件，但
	 * 目前我保持逻辑简单。
	 */

	RelationCloseSmgr(rel);
}

/*
 * RelationPreserveStorage
 *		将关系标记为终究不要删除。
 *
 * 我们需要本函数，因为关系映射更改与整个事务的提交是
 * 分开提交的，因此在映射更新完成之后，事务仍可能
 * 中止。
 * 当新的物理关系被安装进映射时，它会被安排为
 * 中止时删除，于是我们会删除它，从而陷入麻烦。
 * 关系映射器通过在其提交过程中告诉我们终究不要删除
 * 此类关系来修复这一点。
 *
 * 我们还用它在 ALTER TABLE 期间复用索引的旧构建，这次
 * 是移除提交时删除的条目。
 *
 * 若该关系不在安排删除者之列，则为空操作。
 */
void
RelationPreserveStorage(RelFileLocator rlocator, bool atCommit)
{
	PendingRelDelete *pending;
	PendingRelDelete *prev;
	PendingRelDelete *next;

	prev = NULL;
	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		if (RelFileLocatorEquals(rlocator, pending->rlocator)
			&& pending->atCommit == atCommit)
		{
			/* 解除链接并删除列表条目 */
			if (prev)
				prev->next = next;
			else
				pendingDeletes = next;
			pfree(pending);
			/* prev 不变 */
		}
		else
		{
			/* 无关条目，不要碰它 */
			prev = pending;
		}
	}
}

/*
 * RelationTruncate
 *		把关系物理截断到指定块数。
 *
 * 这包括丢掉将被丢弃的那些块的任何
 * 缓冲区。
 */
void
RelationTruncate(Relation rel, BlockNumber nblocks)
{
	bool		fsm;
	bool		vm;
	bool		need_fsm_vacuum = false;
	ForkNumber	forks[MAX_FORKNUM];
	BlockNumber old_blocks[MAX_FORKNUM];
	BlockNumber blocks[MAX_FORKNUM];
	int			nforks = 0;
	SMgrRelation reln;

	/*
	 * 确保 smgr_targblock 等没有指向超过新末尾的某处。
	 * （注：在此循环之后不要依赖这个 reln 指针。）
	 */
	reln = RelationGetSmgr(rel);
	reln->smgr_targblock = InvalidBlockNumber;
	for (int i = 0; i <= MAX_FORKNUM; ++i)
		reln->smgr_cached_nblocks[i] = InvalidBlockNumber;

	/* 为截断关系的 MAIN 分支做准备 */
	forks[nforks] = MAIN_FORKNUM;
	old_blocks[nforks] = smgrnblocks(reln, MAIN_FORKNUM);
	blocks[nforks] = nblocks;
	nforks++;

	/* 若 FSM 存在，为其截断做准备 */
	fsm = smgrexists(RelationGetSmgr(rel), FSM_FORKNUM);
	if (fsm)
	{
		blocks[nforks] = FreeSpaceMapPrepareTruncateRel(rel, nblocks);
		if (BlockNumberIsValid(blocks[nforks]))
		{
			forks[nforks] = FSM_FORKNUM;
			old_blocks[nforks] = smgrnblocks(reln, FSM_FORKNUM);
			nforks++;
			need_fsm_vacuum = true;
		}
	}

	/* 若可见性映射也存在，为其截断做准备 */
	vm = smgrexists(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM);
	if (vm)
	{
		blocks[nforks] = visibilitymap_prepare_truncate(rel, nblocks);
		if (BlockNumberIsValid(blocks[nforks]))
		{
			forks[nforks] = VISIBILITYMAP_FORKNUM;
			old_blocks[nforks] = smgrnblocks(reln, VISIBILITYMAP_FORKNUM);
			nforks++;
		}
	}

	RelationPreTruncate(rel);

	/*
	 * 后续代码可能以两种独立方式与并发检查点
	 * 交互。
	 *
	 * 首先，截断操作可能丢弃检查点本会刷出的
	 * 缓冲区。若如此，则必须在检查点记录被写出
	 * 之前，文件实际上已在磁盘上被截断。否则，若
	 * 从该检查点开始重放，待截断的块可能仍存在于
	 * 磁盘上，但内容比预期更旧，从而导致重放失败。
	 * 块在磁盘上完全不存在是可以的，但不能有错误
	 * 内容。因此，在本代码执行期间我们需要
	 * 设置
	 * DELAY_CHKPT_COMPLETE。
	 *
	 * 其次，下面调用 smgrtruncate() 进而会调用
	 * RegisterSyncRequest()。我们需要该调用创建的同步
	 * 请求在检查点完成之前被处理。CheckPointGuts() 会
	 * 调用 ProcessSyncRequests()，但若我们在那之后才注册
	 * 同步请求，则截断的 WAL 记录可能最终排在检查点
	 * 记录之前，而实际同步却要等到下一个检查点才发生。
	 * 为防止这一点，我们需要在此设置
	 * DELAY_CHKPT_START。这样，若 XLOG_SMGR_TRUNCATE
	 * 先于并发检查点的 redo 指针，我们就保证相应的
	 * 同步请求会在检查点完成之前被
	 * 处理。
	 */
	Assert((MyProc->delayChkptFlags & (DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE)) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE;

	/*
	 * 我们先把截断记入 WAL，然后在临界区中执行
	 * 截断。截断会丢弃缓冲区（即使是脏的），然后
	 * 截断磁盘文件。所有这些工作都必须在锁被释放之前
	 * 完成，否则磁盘上缺少近期更改的旧版页面会再次
	 * 可访问。若我们 panic，会在崩溃恢复中重试整个
	 * 操作，但即便如此也不能放弃，因为我们不希望备库的
	 * 关系大小发散并破坏下游的重放或可见性不变量。
	 * 临界区也会抑制
	 * 中断。
	 *
	 * （若更改本代码，另见 pg_visibilitymap.c。）
	 */
	START_CRIT_SECTION();

	if (RelationNeedsWAL(rel))
	{
		/*
		 * 做一条报告文件截断的 XLOG 条目。
		 */
		XLogRecPtr	lsn;
		xl_smgr_truncate xlrec;

		xlrec.blkno = nblocks;
		xlrec.rlocator = rel->rd_locator;
		xlrec.flags = SMGR_TRUNCATE_ALL;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));

		lsn = XLogInsert(RM_SMGR_ID,
						 XLOG_SMGR_TRUNCATE | XLR_SPECIAL_REL_UPDATE);

		/*
		 * 刷出，因为否则主关系的截断可能在 WAL 记录以及
		 * FSM 或可见性映射的截断之前落到磁盘。若我们在该
		 * 窗口期间崩溃，会留下已截断的堆，但 FSM 或可见性
		 * 映射仍包含不存在的堆页面的条目，且备库也
		 * 永远不会重放该
		 * 截断。
		 */
		XLogFlush(lsn);
	}

	/*
	 * 这会首先从缓冲池中移除截断完成后不应再存在的
	 * 任何缓冲区，然后截断磁盘上相应的
	 * 文件。
	 */
	smgrtruncate2(RelationGetSmgr(rel), forks, nforks, old_blocks, blocks);

	END_CRIT_SECTION();

	/* 关键工作都做完了，现在检查点可以了。 */
	MyProc->delayChkptFlags &= ~(DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE);

	/*
	 * 更新上层 FSM 页面以反映截断。这很重要，因为刚被
	 * 截断的页面很可能被标为全部空闲，会被优先
	 * 选中。
	 *
	 * NB: 没有必要延迟检查点直到这做完。
	 * 因为 FSM 不记 WAL，我们反正得为崩溃后的损坏
	 * 可能做好准备。
	 */
	if (need_fsm_vacuum)
		FreeSpaceMapVacuumRange(rel, nblocks, InvalidBlockNumber);
}

/*
 * RelationPreTruncate
 *		在物理截断之前执行与 AM 无关的工作。
 *
 * 若访问方法的 relation_nontransactional_truncate 不调用
 * RelationTruncate()，则必须在减小表大小之前调用本函数。
 */
void
RelationPreTruncate(Relation rel)
{
	PendingRelSync *pending;

	if (!pendingSyncHash)
		return;

	pending = hash_search(pendingSyncHash,
						  &(RelationGetSmgr(rel)->smgr_rlocator.locator),
						  HASH_FIND, NULL);
	if (pending)
		pending->is_truncated = true;
}

/*
 * 逐块复制一个分支的数据。
 *
 * 注意这需要共享缓冲区中没有脏数据。若
 * 可能有，调用方需要用例如 FlushRelationBuffers(rel)
 * 刷出它们。
 *
 * 另请注意这经常通过如下说法调用：
 *		RelationCopyStorage(RelationGetSmgr(rel), ...);
 * 这之所以安全，只因为我们在此只执行 smgr 与 WAL 操作。
 * 若我们调用别的任何东西，relcache 刷出可能使我们的 SMgrRelation
 * 参数变成悬空指针。
 */
void
RelationCopyStorage(SMgrRelation src, SMgrRelation dst,
					ForkNumber forkNum, char relpersistence)
{
	PGIOAlignedBlock buf;
	Page		page;
	bool		use_wal;
	bool		copying_initfork;
	BlockNumber nblocks;
	BlockNumber blkno;

	page = (Page) buf.data;

	/*
	 * 未记日志关系的 init 分支在许多方面必须与普通关系
	 * 同样对待，更改需要记入 WAL，并且
	 * 需要同步到磁盘。
	 */
	copying_initfork = relpersistence == RELPERSISTENCE_UNLOGGED &&
		forkNum == INIT_FORKNUM;

	/*
	 * 当且仅当启用了 WAL 归档/流式传输并且它是永久关系时，
	 * 我们才需要把复制的数据记入 WAL。这与
	 * "RelationNeedsWAL(rel) || copying_initfork" 给出相同答案，因为我们知道
	 * 当前操作创建了新的关系存储。
	 */
	use_wal = XLogIsNeeded() &&
		(relpersistence == RELPERSISTENCE_PERMANENT || copying_initfork);

	nblocks = smgrnblocks(src, forkNum);

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		/* 若在复制数据期间收到取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		smgrread(src, forkNum, blkno, buf.data);

		if (!PageIsVerifiedExtended(page, blkno,
									PIV_LOG_WARNING | PIV_REPORT_STAT))
		{
			/*
			 * 为谨慎起见，在调用 ereport 机制之前先捕获文件
			 * 路径。这防范例如由 errcontext 回调引起的
			 * relcache 刷出的可能。（errcontext 回调本不该冒这种险，但
			 * 人们曾忘记那条
			 * 规则。）
			 */
			char	   *relpath = relpathbackend(src->smgr_rlocator.locator,
												 src->smgr_rlocator.backend,
												 forkNum);

			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid page in block %u of relation %s",
							blkno, relpath)));
		}

		/*
		 * 把复制的页记入 WAL。遗憾的是我们不知道这是哪种
		 * 页，因此必须记录包括任何未用空间在内的
		 * 整页。
		 */
		if (use_wal)
			log_newpage(&dst->smgr_rlocator.locator, forkNum, blkno, page, false);

		PageSetChecksumInplace(page, blkno);

		/*
		 * 现在写出该页。我们说 skipFsync = true，因为
		 * 无需让 smgr 为这次写出安排 fsync；我们会在
		 * 下面自己做。
		 */
		smgrextend(dst, forkNum, blkno, buf.data, true);
	}

	/*
	 * 当我们把关系页记入 WAL 时，仍必须对它们做 fsync。
	 * 原因是由于我们在共享缓冲区之外复制，复制期间发生的
	 * CHECKPOINT 无法把先前写出的数据刷到磁盘（实际上它甚至
	 * 不知道新关系存在）。之后的崩溃会从检查点重放 WAL，
	 * 因此不会重放我们更早的 WAL 条目。若我们不在此对这些
	 * 页做 fsync，崩溃发生时它们可能仍不在
	 * 磁盘上。
	 */
	if (use_wal || copying_initfork)
		smgrimmedsync(dst, forkNum);
}

/*
 * RelFileLocatorSkippingWAL
 *		检查 BM_PERMANENT 的 relfilelocator 是否使用 WAL。
 *
 * 对某些关系的更改不得写 WAL；见
 * src/backend/access/transam/README 中的 "Skipping WAL for
 * New RelFileLocator"。虽然从 Relation 可以高效得知，但本函数
 * 面向没有 Relation 访问的代码路径。
 */
bool
RelFileLocatorSkippingWAL(RelFileLocator rlocator)
{
	if (!pendingSyncHash ||
		hash_search(pendingSyncHash, &rlocator, HASH_FIND, NULL) == NULL)
		return false;

	return true;
}

/*
 * EstimatePendingSyncsSpace
 *		估计把同步传递给并行工作进程所需的空间。
 */
Size
EstimatePendingSyncsSpace(void)
{
	long		entries;

	entries = pendingSyncHash ? hash_get_num_entries(pendingSyncHash) : 0;
	return mul_size(1 + entries, sizeof(RelFileLocator));
}

/*
 * SerializePendingSyncs
 *		为并行工作进程序列化同步。
 */
void
SerializePendingSyncs(Size maxSize, char *startAddress)
{
	HTAB	   *tmphash;
	HASHCTL		ctl;
	HASH_SEQ_STATUS scan;
	PendingRelSync *sync;
	PendingRelDelete *delete;
	RelFileLocator *src;
	RelFileLocator *dest = (RelFileLocator *) startAddress;

	if (!pendingSyncHash)
		goto terminate;

	/* 创建临时哈希以收集活动的 relfilelocator */
	ctl.keysize = sizeof(RelFileLocator);
	ctl.entrysize = sizeof(RelFileLocator);
	ctl.hcxt = CurrentMemoryContext;
	tmphash = hash_create("tmp relfilelocators",
						  hash_get_num_entries(pendingSyncHash), &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* 从待处理同步收集所有 rlocator */
	hash_seq_init(&scan, pendingSyncHash);
	while ((sync = (PendingRelSync *) hash_seq_search(&scan)))
		(void) hash_search(tmphash, &sync->rlocator, HASH_ENTER, NULL);

	/* 移除已删除的 rnode */
	for (delete = pendingDeletes; delete != NULL; delete = delete->next)
		if (delete->atCommit)
			(void) hash_search(tmphash, &delete->rlocator,
							   HASH_REMOVE, NULL);

	hash_seq_init(&scan, tmphash);
	while ((src = (RelFileLocator *) hash_seq_search(&scan)))
		*dest++ = *src;

	hash_destroy(tmphash);

terminate:
	MemSet(dest, 0, sizeof(RelFileLocator));
}

/*
 * RestorePendingSyncs
 *		在并行工作进程内恢复同步。
 *
 * RelationNeedsWAL() 与 RelFileLocatorSkippingWAL() 必须向并行工作进程
 * 给出正确答案。只有 smgrDoPendingSyncs() 在事务结束时读取
 * is_truncated 字段。因此，不要恢复它。
 */
void
RestorePendingSyncs(char *startAddress)
{
	RelFileLocator *rlocator;

	Assert(pendingSyncHash == NULL);
	for (rlocator = (RelFileLocator *) startAddress; rlocator->relNumber != 0;
		 rlocator++)
		AddPendingSync(rlocator);
}

/*
 *	smgrDoPendingDeletes() -- 在事务结束时处理关系删除。
 *
 * 在中止子事务时也会运行；我们希望立即清理失败的
 * 子事务。
 *
 * 注：有可能被要求移除在任何分支中都没有物理存储的
 * 关系。特别地，有可能我们在清理一个旧的临时关系，而
 * RemovePgTempFiles 已经恢复了其物理
 * 存储。
 */
void
smgrDoPendingDeletes(bool isCommit)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;
	PendingRelDelete *prev;
	PendingRelDelete *next;
	int			nrels = 0,
				maxrels = 0;
	SMgrRelation *srels = NULL;

	prev = NULL;
	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		if (pending->nestLevel < nestLevel)
		{
			/* 外层条目尚不应处理 */
			prev = pending;
		}
		else
		{
			/* 先解除链接列表条目，以免失败时重试 */
			if (prev)
				prev->next = next;
			else
				pendingDeletes = next;
			/* 若需要则执行删除 */
			if (pending->atCommit == isCommit)
			{
				SMgrRelation srel;

				srel = smgropen(pending->rlocator, pending->backend);

				/* 分配初始数组，或在需要时扩展它 */
				if (maxrels == 0)
				{
					maxrels = 8;
					srels = palloc(sizeof(SMgrRelation) * maxrels);
				}
				else if (maxrels <= nrels)
				{
					maxrels *= 2;
					srels = repalloc(srels, sizeof(SMgrRelation) * maxrels);
				}

				srels[nrels++] = srel;
			}
			/* 必须显式释放列表条目 */
			pfree(pending);
			/* prev 不变 */
		}
	}

	if (nrels > 0)
	{
		smgrdounlinkall(srels, nrels, false);

		for (int i = 0; i < nrels; i++)
			smgrclose(srels[i]);

		pfree(srels);
	}
}

/*
 *	smgrDoPendingSyncs() -- 在事务结束时处理关系同步。
 */
void
smgrDoPendingSyncs(bool isCommit, bool isParallelWorker)
{
	PendingRelDelete *pending;
	int			nrels = 0,
				maxrels = 0;
	SMgrRelation *srels = NULL;
	HASH_SEQ_STATUS scan;
	PendingRelSync *pendingsync;

	Assert(GetCurrentTransactionNestLevel() == 1);

	if (!pendingSyncHash)
		return;					/* 没有关系需要同步 */

	/* 中止 -- 直接丢弃所有待处理同步 */
	if (!isCommit)
	{
		pendingSyncHash = NULL;
		return;
	}

	AssertPendingSyncs_RelationCache();

	/* 并行工作进程 -- 直接丢弃所有待处理同步 */
	if (isParallelWorker)
	{
		pendingSyncHash = NULL;
		return;
	}

	/* 跳过同步那些 smgrDoPendingDeletes() 将删除的节点。 */
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
		if (pending->atCommit)
			(void) hash_search(pendingSyncHash, &pending->rlocator,
							   HASH_REMOVE, NULL);

	hash_seq_init(&scan, pendingSyncHash);
	while ((pendingsync = (PendingRelSync *) hash_seq_search(&scan)))
	{
		ForkNumber	fork;
		BlockNumber nblocks[MAX_FORKNUM + 1];
		uint64		total_blocks = 0;
		SMgrRelation srel;

		srel = smgropen(pendingsync->rlocator, InvalidBackendId);

		/*
		 * 我们对较小的关系发出 newpage WAL 记录。
		 *
		 * 小的 WAL 记录有机会与其他后端的 WAL 记录一起发出。
		 * 对小于某个阈值的文件，我们发出 WAL 记录而不是同步，
		 * 期望更快提交。该阈值由 GUC wal_skip_threshold
		 * 定义。
		 */
		if (!pendingsync->is_truncated)
		{
			for (fork = 0; fork <= MAX_FORKNUM; fork++)
			{
				if (smgrexists(srel, fork))
				{
					BlockNumber n = smgrnblocks(srel, fork);

					/* 我们不应为未记日志关系走到这里 */
					Assert(fork != INIT_FORKNUM);
					nblocks[fork] = n;
					total_blocks += n;
				}
				else
					nblocks[fork] = InvalidBlockNumber;
			}
		}

		/*
		 * 同步文件或为其内容发出 WAL 记录。
		 *
		 * 虽然若文件足够小我们会发出 WAL 记录，但若文件
		 * 经历过截断，则不论大小都要做文件同步。这是因为若
		 * 过去更长的文件已被刷出，而我们省略了对该文件的
		 * 同步写出并改为发出 WAL，则崩溃恢复后文件后面会
		 * 跟着尾部垃圾块。你可能认为若当前主分支比以往更长
		 * 就可以选择 WAL，但存在主分支比以往更长而
		 * FSM 分支变短的
		 * 情形。
		 */
		if (pendingsync->is_truncated ||
			total_blocks >= wal_skip_threshold * (uint64) 1024 / BLCKSZ)
		{
			/* 分配初始数组，或在需要时扩展它 */
			if (maxrels == 0)
			{
				maxrels = 8;
				srels = palloc(sizeof(SMgrRelation) * maxrels);
			}
			else if (maxrels <= nrels)
			{
				maxrels *= 2;
				srels = repalloc(srels, sizeof(SMgrRelation) * maxrels);
			}

			srels[nrels++] = srel;
		}
		else
		{
			/* 为所有块发出 WAL 记录。该文件足够小。 */
			for (fork = 0; fork <= MAX_FORKNUM; fork++)
			{
				int			n = nblocks[fork];
				Relation	rel;

				if (!BlockNumberIsValid(n))
					continue;

				/*
				 * 为整个文件发出 WAL。遗憾的是我们不知道这是哪种
				 * 页，因此必须记录包括任何未用空间在内的
				 * 整页。ReadBufferExtended() 会计入一些
				 * pgstat 事件；遗憾的是，我们丢弃它们。
				 */
				rel = CreateFakeRelcacheEntry(srel->smgr_rlocator.locator);
				log_newpage_range(rel, fork, 0, n, false);
				FreeFakeRelcacheEntry(rel);
			}
		}
	}

	pendingSyncHash = NULL;

	if (nrels > 0)
	{
		smgrdosyncall(srels, nrels);
		pfree(srels);
	}
}

/*
 * smgrGetPendingDeletes() -- 取得要删除的非临时关系列表。
 *
 * 返回值是安排终止的关系数量。
 * *ptr 被设为指向新 palloc 的 RelFileLocators 数组。
 * 若没有要删除的关系，*ptr 被设为 NULL。
 *
 * 返回列表只包含非临时关系。这是可以的，因为该列表仅用于
 * 临时关系无关紧要的上下文：我们要么在写两阶段状态文件
 * （而碰过临时表的事务不能被准备），要么在写
 * xlog（而若我们重启，所有临时文件反正会被清除，
 * 因此无需 redo 也
 * 去做）。
 *
 * 注意该列表不包括上层事务安排终止的
 * 任何内容。
 */
int
smgrGetPendingDeletes(bool forCommit, RelFileLocator **ptr)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	int			nrels;
	RelFileLocator *rptr;
	PendingRelDelete *pending;

	nrels = 0;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit
			&& pending->backend == InvalidBackendId)
			nrels++;
	}
	if (nrels == 0)
	{
		*ptr = NULL;
		return 0;
	}
	rptr = (RelFileLocator *) palloc(nrels * sizeof(RelFileLocator));
	*ptr = rptr;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit
			&& pending->backend == InvalidBackendId)
		{
			*rptr = pending->rlocator;
			rptr++;
		}
	}
	return nrels;
}

/*
 *	PostPrepare_smgr -- 在成功的 PREPARE 之后清理
 *
 * 我们在此要做的是丢掉关于待处理关系删除的内存中状态。
 * 这一切都已记录在 2PC 状态文件中，
 * 不再是 smgr 该操心的事。
 */
void
PostPrepare_smgr(void)
{
	PendingRelDelete *pending;
	PendingRelDelete *next;

	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		pendingDeletes = next;
		/* 必须显式释放列表条目 */
		pfree(pending);
	}
}


/*
 * AtSubCommit_smgr() --- 处理子事务提交。
 *
 * 把待删除列表中的所有项重新归属到父事务。
 */
void
AtSubCommit_smgr(void)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel)
			pending->nestLevel = nestLevel - 1;
	}
}

/*
 * AtSubAbort_smgr() --- 处理子事务中止。
 *
 * 删除已创建的关系并忘记已删除的关系。
 * 我们可以立即执行这些操作，因为我们知道此
 * 子事务不会提交。
 */
void
AtSubAbort_smgr(void)
{
	smgrDoPendingDeletes(false);
}

void
smgr_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* 备份块不用于 smgr 记录 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(record);
		SMgrRelation reln;

		reln = smgropen(xlrec->rlocator, InvalidBackendId);
		smgrcreate(reln, xlrec->forkNum, true);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);
		SMgrRelation reln;
		Relation	rel;
		ForkNumber	forks[MAX_FORKNUM];
		BlockNumber blocks[MAX_FORKNUM];
		BlockNumber old_blocks[MAX_FORKNUM];
		int			nforks = 0;
		bool		need_fsm_vacuum = false;

		reln = smgropen(xlrec->rlocator, InvalidBackendId);

		/*
		 * 若关系不存在则强制创建（这表明它在 WAL 序列中稍后
		 * 某处被删除了）。与 XLogReadBufferForRedo 一样，我们宁愿
		 * 重建该关系并尽可能重放日志，直到看到
		 * 删除。
		 */
		smgrcreate(reln, MAIN_FORKNUM, true);

		/*
		 * 在执行截断之前，把最小恢复点更新到覆盖本
		 * WAL 记录。关系一旦被截断就没有回头路。
		 * 缓冲管理器对关系文件的正常更新强制 WAL 优先规则，
		 * 因此最小恢复点总是在数据文件中相应更改刷到磁盘之前
		 * 被更新。我们在此必须手动做同样的
		 * 事。
		 *
		 * 在截断之前做这件事意味着若截断因某种原因失败，
		 * 即使重启后你也无法启动系统，直到你修好底层状况使
		 * 截断能够成功。或者，我们可以在截断之后更新最小
		 * 恢复点，但那会留下一个小窗口，其中 WAL 优先规则
		 * 可能被
		 * 违反。
		 */
		XLogFlush(lsn);

		/* 为截断 MAIN 分支做准备 */
		if ((xlrec->flags & SMGR_TRUNCATE_HEAP) != 0)
		{
			forks[nforks] = MAIN_FORKNUM;
			old_blocks[nforks] = smgrnblocks(reln, MAIN_FORKNUM);
			blocks[nforks] = xlrec->blkno;
			nforks++;

			/* 也告知 xlogutils.c */
			XLogTruncateRelation(xlrec->rlocator, MAIN_FORKNUM, xlrec->blkno);
		}

		/* 也为截断 FSM 与 VM 做准备 */
		rel = CreateFakeRelcacheEntry(xlrec->rlocator);

		if ((xlrec->flags & SMGR_TRUNCATE_FSM) != 0 &&
			smgrexists(reln, FSM_FORKNUM))
		{
			blocks[nforks] = FreeSpaceMapPrepareTruncateRel(rel, xlrec->blkno);
			if (BlockNumberIsValid(blocks[nforks]))
			{
				forks[nforks] = FSM_FORKNUM;
				old_blocks[nforks] = smgrnblocks(reln, FSM_FORKNUM);
				nforks++;
				need_fsm_vacuum = true;
			}
		}
		if ((xlrec->flags & SMGR_TRUNCATE_VM) != 0 &&
			smgrexists(reln, VISIBILITYMAP_FORKNUM))
		{
			blocks[nforks] = visibilitymap_prepare_truncate(rel, xlrec->blkno);
			if (BlockNumberIsValid(blocks[nforks]))
			{
				forks[nforks] = VISIBILITYMAP_FORKNUM;
				old_blocks[nforks] = smgrnblocks(reln, VISIBILITYMAP_FORKNUM);
				nforks++;
			}
		}

		/* 做截断关系分支的实际工作 */
		if (nforks > 0)
		{
			START_CRIT_SECTION();
			smgrtruncate2(reln, forks, nforks, old_blocks, blocks);
			END_CRIT_SECTION();
		}

		/*
		 * 更新上层 FSM 页面以反映截断。这很重要，因为刚被
		 * 截断的页面很可能被标为全部空闲，会被
		 * 优先选中。
		 */
		if (need_fsm_vacuum)
			FreeSpaceMapVacuumRange(rel, xlrec->blkno,
									InvalidBlockNumber);

		FreeFakeRelcacheEntry(rel);
	}
	else
		elog(PANIC, "smgr_redo: unknown op code %u", info);
}
