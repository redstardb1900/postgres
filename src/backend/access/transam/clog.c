/*-------------------------------------------------------------------------
 *
 * clog.c
 *		PostgreSQL 事务提交日志管理器
 *
 * 本模块取代旧的 "pg_log" 访问代码；后者把 pg_log
 * 基本上当作一个关系来对待，即走常规缓冲区
 * 管理器。问题在于，对于那些已足够老、永远不会再被
 * 查找的事务，没有好办法回收其存储空间。
 * 现在我们使用专用访问代码，使提交
 * 日志可以拆成相对较小、彼此独立的段。
 *
 * XLOG 交互：每当新的 CLOG 页被初始化为零时，本模块
 * 生成一条 XLOG 记录。对 CLOG 的其他写入来自 xact.c 中
 * 对事务提交或中止的记录，后者会为这些事件生成自己的
 * XLOG 记录，并在 redo 时重新执行状态更新；因此这里无需
 * 再写额外的 XLOG 项。对于同步
 * 事务提交，在我们被调用来记录提交之前，XLOG 已保证刷写到
 * XLOG 提交记录，因此 WAL 规则 "write xlog
 * before data" 会自动满足。然而，对于异步提交，我们
 * 必须跟踪影响每个 CLOG 页的最新 LSN，以便能把
 * XLOG 刷写到那么远并满足 WAL 规则。对于中止
 * （无论同步还是异步）我们不必担心这一点，因为崩溃后的假定
 * 本来就是这类事务已经失败。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/clog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "storage/proc.h"
#include "storage/sync.h"

/*
 * CLOG 页大小相关定义。页大小与 Postgres
 * 其他地方一样，均为 BLCKSZ。
 *
 * 注意：因为 TransactionIds 是 32 位并在 0xFFFFFFFF 回卷，
 * CLOG 页编号也会在 0xFFFFFFFF/CLOG_XACTS_PER_PAGE 处回卷，
 * 段编号在
 * 0xFFFFFFFF/CLOG_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT 处回卷。本模块无需
 * 显式关注这一事实，例外是在 TruncateCLOG 中比较段号
 * 与页号时（见 CLOGPagePrecedes）。
 */

/* 每个 xact 需要两位，因此一字节可装四个 xact */
#define CLOG_BITS_PER_XACT	2
#define CLOG_XACTS_PER_BYTE 4
#define CLOG_XACTS_PER_PAGE (BLCKSZ * CLOG_XACTS_PER_BYTE)
#define CLOG_XACT_BITMASK	((1 << CLOG_BITS_PER_XACT) - 1)

#define TransactionIdToPage(xid)	((xid) / (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToPgIndex(xid) ((xid) % (TransactionId) CLOG_XACTS_PER_PAGE)
#define TransactionIdToByte(xid)	(TransactionIdToPgIndex(xid) / CLOG_XACTS_PER_BYTE)
#define TransactionIdToBIndex(xid)	((xid) % (TransactionId) CLOG_XACTS_PER_BYTE)

/* 我们为每组事务存储最新的异步 LSN */
#define CLOG_XACTS_PER_LSN_GROUP	32	/* 保持为 2 的幂 */
#define CLOG_LSNS_PER_PAGE	(CLOG_XACTS_PER_PAGE / CLOG_XACTS_PER_LSN_GROUP)

#define GetLSNIndex(slotno, xid)	((slotno) * CLOG_LSNS_PER_PAGE + \
	((xid) % (TransactionId) CLOG_XACTS_PER_PAGE) / CLOG_XACTS_PER_LSN_GROUP)

/*
 * 子事务数目低于此阈值时，我们考虑应用 clog 分组
 * 更新优化。测试表明高于此值会
 * 损害性能。
 */
#define THRESHOLD_SUBTRANS_CLOG_OPT	5

/*
 * 指向 CLOG 控制用共享内存数据结构的链接
 */
static SlruCtlData XactCtlData;

#define XactCtl (&XactCtlData)


static int	ZeroCLOGPage(int pageno, bool writeXlog);
static bool CLOGPagePrecedes(int page1, int page2);
static void WriteZeroPageXlogRec(int pageno);
static void WriteTruncateXlogRec(int pageno, TransactionId oldestXact,
								 Oid oldestXactDb);
static void TransactionIdSetPageStatus(TransactionId xid, int nsubxids,
									   TransactionId *subxids, XidStatus status,
									   XLogRecPtr lsn, int pageno,
									   bool all_xact_same_page);
static void TransactionIdSetStatusBit(TransactionId xid, XidStatus status,
									  XLogRecPtr lsn, int slotno);
static void set_status_by_pages(int nsubxids, TransactionId *subxids,
								XidStatus status, XLogRecPtr lsn);
static bool TransactionGroupUpdateXidStatus(TransactionId xid,
											XidStatus status, XLogRecPtr lsn, int pageno);
static void TransactionIdSetPageStatusInternal(TransactionId xid, int nsubxids,
											   TransactionId *subxids, XidStatus status,
											   XLogRecPtr lsn, int pageno);


/*
 * TransactionIdSetTreeStatus
 *
 * 在提交日志中记录一个事务及其子事务树各项的
 * 最终状态。注意要保证这样做
 * 高效，并尽可能原子。
 *
 * xid 是要设置状态的单个 xid。这通常是
 * 顶层提交或中止时的顶层 transactionid。当
 * 记录事务中止时，也可以是子事务。
 *
 * subxids 是长度为 nsubxids 的 xid 数组，表示 xid 树中的
 * 子事务。各种情况下 nsubxids 可能为零。
 *
 * 记录异步提交时，lsn 必须是提交记录的 WAL 位置。
 * 对于同步提交可以是 InvalidXLogRecPtr，因为那种情况下
 * 调用方保证提交记录已刷写。中止情况下
 * 也应为 InvalidXLogRecPtr。
 *
 * 在提交情况下，原子性受限于所有 subxid 是否与 xid 在
 * 同一 CLOG 页上。若都在，则锁只会获取
 * 一次，并直接把状态设为已提交。否则
 * 我们必须
 *	 1. 把与主 xid 不在同一页上的所有
 *		subxid 设为子已提交
 *	 2. 原子地把主 xid 与同页上的 subxid 设为已提交
 *	 3. 再扫一遍第一批并把它们设为已提交
 * 注意就并发检查者而言，主事务
 * 提交作为整体仍然是原子的。
 *
 * 示例：
 *		TransactionId t 提交，并有子 xid t1、t2、t3、t4
 *		t 在页 p1 上，t1 也在 p1，t2 与 t3 在 p2，t4 在 p3
 *		1. 更新 pages2-3：
 *					page2: 将 t2、t3 设为子已提交
 *					page3: 将 t4 设为子已提交
 *		2. 更新 page1：
 *					page1: 将 t、t1 设为已提交
 *		3. 更新 pages2-3：
 *					page2: 将 t2、t3 设为已提交
 *					page3: 将 t4 设为已提交
 *
 * NB：这是低层例程，并非大多数用途的首选入口
 * 点；预期调用方是 transam.c 中的函数。
 *
 * XXX 考虑对尚不在缓存中但即将需要的页发出
 * POSIX_FADV_WILLNEED，并对暂不应
 * 被换出的页给出提示。
 */
void
TransactionIdSetTreeStatus(TransactionId xid, int nsubxids,
						   TransactionId *subxids, XidStatus status, XLogRecPtr lsn)
{
	int			pageno = TransactionIdToPage(xid);	/* 取父事务的页 */
	int			i;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED);

	/*
	 * 看看有多少 subxid（若有）与父事务在同一页上（若
	 * 有）。
	 */
	for (i = 0; i < nsubxids; i++)
	{
		if (TransactionIdToPage(subxids[i]) != pageno)
			break;
	}

	/*
	 * 所有项是否都落在单页上？
	 */
	if (i == nsubxids)
	{
		/*
		 * 在单次调用中设置父事务与全部子事务
		 */
		TransactionIdSetPageStatus(xid, nsubxids, subxids, status, lsn,
								   pageno, true);
	}
	else
	{
		int			nsubxids_on_first_page = i;

		/*
		 * 若这是提交，则我们关心正确完成（即
		 * 使用子已提交这一中间状态）。到此，我们知道
		 * 正在更新多于一页的 clog，因此必须先把*不*在
		 * 第一页上的项标为子已提交，然后
		 * 再回来把状态更新为完全已提交。
		 *
		 * 为避免两次触及第一页，跳过对该第一页上
		 * subxid 的子已提交标记。
		 */
		if (status == TRANSACTION_STATUS_COMMITTED)
			set_status_by_pages(nsubxids - nsubxids_on_first_page,
								subxids + nsubxids_on_first_page,
								TRANSACTION_STATUS_SUB_COMMITTED, lsn);

		/*
		 * 现在设置父事务以及与父事务同页的子事务（若
		 * 有）
		 */
		pageno = TransactionIdToPage(xid);
		TransactionIdSetPageStatus(xid, nsubxids_on_first_page, subxids, status,
								   lsn, pageno, false);

		/*
		 * 现在从第二页起，按每次一个 clog 页处理其余
		 * subxid，做法与上面相同。
		 */
		set_status_by_pages(nsubxids - nsubxids_on_first_page,
							subxids + nsubxids_on_first_page,
							status, lsn);
	}
}

/*
 * TransactionIdSetTreeStatus 的辅助函数：为一批事务设置状态，
 * 按所涉及的各个 CLOG 页分块。我们从不
 * 把整棵事务树传给本函数，只传与顶层事务 id
 * 不在同一页上的子事务。
 */
static void
set_status_by_pages(int nsubxids, TransactionId *subxids,
					XidStatus status, XLogRecPtr lsn)
{
	int			pageno = TransactionIdToPage(subxids[0]);
	int			offset = 0;
	int			i = 0;

	Assert(nsubxids > 0);		/* 否则上面取 pageno 不安全 */

	while (i < nsubxids)
	{
		int			num_on_page = 0;
		int			nextpageno;

		do
		{
			nextpageno = TransactionIdToPage(subxids[i]);
			if (nextpageno != pageno)
				break;
			num_on_page++;
			i++;
		} while (i < nsubxids);

		TransactionIdSetPageStatus(InvalidTransactionId,
								   num_on_page, subxids + offset,
								   status, lsn, pageno, false);
		offset = i;
		pageno = nextpageno;
	}
}

/*
 * 在提交日志中为一页上的全部项记录最终状态。
 * 仅在本页上原子。
 */
static void
TransactionIdSetPageStatus(TransactionId xid, int nsubxids,
						   TransactionId *subxids, XidStatus status,
						   XLogRecPtr lsn, int pageno,
						   bool all_xact_same_page)
{
	/* PGPROC 溢出时不能使用分组更新。 */
	StaticAssertDecl(THRESHOLD_SUBTRANS_CLOG_OPT <= PGPROC_MAX_CACHED_SUBXIDS,
					 "group clog threshold less than PGPROC cached subxids");

	/*
	 * 当 XactSLRULock 上有争用时，我们尝试把多次
	 * 更新分组；单个领导者进程将为多个后端执行
	 * 事务状态更新，从而减少获取
	 * XactSLRULock 的次数。
	 *
	 * 要使此优化安全，MyProc 中的 XID 与 subxid 必须与
	 * 我们要设置状态的那些相同。请检查
	 * 确是如此。
	 *
	 * 要使此优化高效，子 XIDs 不应太多，且
	 * 我们要调整 clog 的全部 XIDs 应在
	 * 同一页上。也请检查这些条件。
	 */
	if (all_xact_same_page && xid == MyProc->xid &&
		nsubxids <= THRESHOLD_SUBTRANS_CLOG_OPT &&
		nsubxids == MyProc->subxidStatus.count &&
		(nsubxids == 0 ||
		 memcmp(subxids, MyProc->subxids.xids,
				nsubxids * sizeof(TransactionId)) == 0))
	{
		/*
		 * 若能立即获取 XactSLRULock，则更新我们自己
		 * XID 的状态并释放锁。否则尝试使用分组 XID
		 * 更新。若那样也不行，则退回为等待
		 * 锁，仅为本事务执行更新。
		 */
		if (LWLockConditionalAcquire(XactSLRULock, LW_EXCLUSIVE))
		{
			/* 无需等待就拿到了锁！执行更新。 */
			TransactionIdSetPageStatusInternal(xid, nsubxids, subxids, status,
											   lsn, pageno);
			LWLockRelease(XactSLRULock);
			return;
		}
		else if (TransactionGroupUpdateXidStatus(xid, status, lsn, pageno))
		{
			/* 分组更新机制已完成工作。 */
			return;
		}

		/* Fall through only if update isn't done yet. */
	}

	/* 分组更新不适用，或无法接受此页号。 */
	LWLockAcquire(XactSLRULock, LW_EXCLUSIVE);
	TransactionIdSetPageStatusInternal(xid, nsubxids, subxids, status,
									   lsn, pageno);
	LWLockRelease(XactSLRULock);
}

/*
 * 在提交日志中记录事务项的最终状态
 *
 * 此处不加锁；调用方必须处理。
 */
static void
TransactionIdSetPageStatusInternal(TransactionId xid, int nsubxids,
								   TransactionId *subxids, XidStatus status,
								   XLogRecPtr lsn, int pageno)
{
	int			slotno;
	int			i;

	Assert(status == TRANSACTION_STATUS_COMMITTED ||
		   status == TRANSACTION_STATUS_ABORTED ||
		   (status == TRANSACTION_STATUS_SUB_COMMITTED && !TransactionIdIsValid(xid)));
	Assert(LWLockHeldByMeInMode(XactSLRULock, LW_EXCLUSIVE));

	/*
	 * 若我们在做异步提交（即 lsn 有效），则必须等待
	 * 该页槽上任何进行中的写出完成。否则我们的
	 * 更新可能随那次写出落到磁盘，这不行，因为在完成
	 * 适当的 WAL 刷写之前绝不能让它到达磁盘。
	 * 但当 lsn 无效时，在页处于 write-busy 时涂改它是
	 * 可以的，因为我们并不在意更新是否比我们以为的更早
	 * 到达磁盘。
	 */
	slotno = SimpleLruReadPage(XactCtl, pageno, XLogRecPtrIsInvalid(lsn), xid);

	/*
	 * 设置主事务 id（若有）。
	 *
	 * 若在本页写出过程中我们更新了页上多个 xid，
	 * 可能发现有些位落到了磁盘而另一些没有。
	 * 若我们在用顶层 xid 更新页上的提交，
	 * 那可能破坏原子性，因此我们先把 subxid 标为子已提交，再
	 * 标记顶层提交。
	 */
	if (TransactionIdIsValid(xid))
	{
		/* 若需要则先处理子事务 ... */
		if (status == TRANSACTION_STATUS_COMMITTED)
		{
			for (i = 0; i < nsubxids; i++)
			{
				Assert(XactCtl->shared->page_number[slotno] == TransactionIdToPage(subxids[i]));
				TransactionIdSetStatusBit(subxids[i],
										  TRANSACTION_STATUS_SUB_COMMITTED,
										  lsn, slotno);
			}
		}

		/* ... 然后是主事务 */
		TransactionIdSetStatusBit(xid, status, lsn, slotno);
	}

	/* 设置子事务 */
	for (i = 0; i < nsubxids; i++)
	{
		Assert(XactCtl->shared->page_number[slotno] == TransactionIdToPage(subxids[i]));
		TransactionIdSetStatusBit(subxids[i], status, lsn, slotno);
	}

	XactCtl->shared->page_dirty[slotno] = true;
}

/*
 * 当提交时无法立即以排他模式获取 XactSLRULock 时，
 * 把自己加入需要更新其 XIDs
 * 状态的进程列表。第一个把自己加入列表的进程将以
 * 排他模式获取 XactSLRULock，并代表所有组成员按要求
 * 设置事务状态。这在许多进程同时尝试提交时
 * 大大减少了围绕 XactSLRULock 的争用，
 * 因为锁不必在各个提交
 * 进程之间反复交接。
 *
 * 当事务状态已在 clog 中更新时返回 true；若因我们需要更新的
 * 页号与已在等待的那些进程不同而决定不应用该优化，则返回
 * false。
 */
static bool
TransactionGroupUpdateXidStatus(TransactionId xid, XidStatus status,
								XLogRecPtr lsn, int pageno)
{
	volatile PROC_HDR *procglobal = ProcGlobal;
	PGPROC	   *proc = MyProc;
	uint32		nextidx;
	uint32		wakeidx;

	/* 我们肯定有一个需要更新状态的 XID。 */
	Assert(TransactionIdIsValid(xid));

	/*
	 * 把自己加入需要分组 XID 状态
	 * 更新的进程列表。
	 */
	proc->clogGroupMember = true;
	proc->clogGroupMemberXid = xid;
	proc->clogGroupMemberXidStatus = status;
	proc->clogGroupMemberPage = pageno;
	proc->clogGroupMemberLsn = lsn;

	nextidx = pg_atomic_read_u32(&procglobal->clogGroupFirst);

	while (true)
	{
		/*
		 * 若我们需要更新当前事务状态的 clog 页与
		 * 分组领导者的 clog 页相同，则把本 proc 加入列表。
		 *
		 * 这里存在竞态：在做完下面的
		 * 检查之后、把本 proc 的 clog 更新加入某组之前，
		 * 分组领导者可能已完成该页的分组更新，并成为
		 * 另一组的领导者。这将导致
		 * 单个组内出现不同的 clog 页
		 * 更新。这不太常见，且仍能工作，只是可能
		 * 效率略低。
		 */
		if (nextidx != INVALID_PGPROCNO &&
			ProcGlobal->allProcs[nextidx].clogGroupMemberPage != proc->clogGroupMemberPage)
		{
			/*
			 * 确保本 proc 不是任何需要 XID 状态更新的
			 * clog 组的成员。
			 */
			proc->clogGroupMember = false;
			pg_atomic_write_u32(&proc->clogGroupNext, INVALID_PGPROCNO);
			return false;
		}

		pg_atomic_write_u32(&proc->clogGroupNext, nextidx);

		if (pg_atomic_compare_exchange_u32(&procglobal->clogGroupFirst,
										   &nextidx,
										   (uint32) proc->pgprocno))
			break;
	}

	/*
	 * 若列表非空，领导者将更新我们
	 * XID 的状态。不可能存在没有领导者的跟随者，因为
	 * 第一个把自己加入列表的进程总会使
	 * nextidx 为 INVALID_PGPROCNO。
	 */
	if (nextidx != INVALID_PGPROCNO)
	{
		int			extraWaits = 0;

		/* 睡眠直到领导者更新我们的 XID 状态。 */
		pgstat_report_wait_start(WAIT_EVENT_XACT_GROUP_UPDATE);
		for (;;)
		{
			/* 充当读屏障 */
			PGSemaphoreLock(proc->sem);
			if (!proc->clogGroupMember)
				break;
			extraWaits++;
		}
		pgstat_report_wait_end();

		Assert(pg_atomic_read_u32(&proc->clogGroupNext) == INVALID_PGPROCNO);

		/* 修正因吸收唤醒而偏高的信号量计数 */
		while (extraWaits-- > 0)
			PGSemaphoreUnlock(proc->sem);
		return true;
	}

	/* 我们是领导者。代表大家获取锁。 */
	LWLockAcquire(XactSLRULock, LW_EXCLUSIVE);

	/*
	 * 既然已拿到锁，清空等待分组 XID 状态更新的进程列表，
	 * 并保存指向列表头的指针。
	 * 试图一次弹出一个元素可能导致 ABA 问题。
	 */
	nextidx = pg_atomic_exchange_u32(&procglobal->clogGroupFirst,
									 INVALID_PGPROCNO);

	/* 记住列表头，以便在放下锁后执行唤醒。 */
	wakeidx = nextidx;

	/* 遍历列表并更新全部 XIDs 的状态。 */
	while (nextidx != INVALID_PGPROCNO)
	{
		PGPROC	   *nextproc = &ProcGlobal->allProcs[nextidx];

		/*
		 * 子 XIDs 多于 THRESHOLD_SUBTRANS_CLOG_OPT 的事务
		 * 不应使用分组 XID 状态更新机制。
		 */
		Assert(nextproc->subxidStatus.count <= THRESHOLD_SUBTRANS_CLOG_OPT);

		TransactionIdSetPageStatusInternal(nextproc->clogGroupMemberXid,
										   nextproc->subxidStatus.count,
										   nextproc->subxids.xids,
										   nextproc->clogGroupMemberXidStatus,
										   nextproc->clogGroupMemberLsn,
										   nextproc->clogGroupMemberPage);

		/* 移到列表中下一个 proc。 */
		nextidx = pg_atomic_read_u32(&nextproc->clogGroupNext);
	}

	/* 现在用完锁了。 */
	LWLockRelease(XactSLRULock);

	/*
	 * 既然已释放锁，回去唤醒所有人。我们
	 * 不在锁下做这件事，以便尽量缩短
	 * 持锁时间。
	 */
	while (wakeidx != INVALID_PGPROCNO)
	{
		PGPROC	   *wakeproc = &ProcGlobal->allProcs[wakeidx];

		wakeidx = pg_atomic_read_u32(&wakeproc->clogGroupNext);
		pg_atomic_write_u32(&wakeproc->clogGroupNext, INVALID_PGPROCNO);

		/* 确保先前写入在跟随者继续前均可见。 */
		pg_write_barrier();

		wakeproc->clogGroupMember = false;

		if (wakeproc != MyProc)
			PGSemaphoreUnlock(wakeproc->sem);
	}

	return true;
}

/*
 * 设置单个事务的提交状态。
 *
 * 调用时必须持有 XactSLRULock
 */
static void
TransactionIdSetStatusBit(TransactionId xid, XidStatus status, XLogRecPtr lsn, int slotno)
{
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	char	   *byteptr;
	char		byteval;
	char		curval;

	byteptr = XactCtl->shared->page_buffer[slotno] + byteno;
	curval = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	/*
	 * 在恢复期间重放事务时，我们仍需执行
	 * 子已提交然后提交这两个阶段。不过有些事务
	 * 已被正确标记，因此我们把那些当作空操作，
	 * 以便让后面的 Assert 尽可能严格。
	 */
	if (InRecovery && status == TRANSACTION_STATUS_SUB_COMMITTED &&
		curval == TRANSACTION_STATUS_COMMITTED)
		return;

	/*
	 * 当前状态变更应是从 0 或子已提交到目标状态，
	 * 或者在恢复期间重放变更时我们应已处于该状态。
	 */
	Assert(curval == 0 ||
		   (curval == TRANSACTION_STATUS_SUB_COMMITTED &&
			status != TRANSACTION_STATUS_IN_PROGRESS) ||
		   curval == status);

	/* 注意此处假定对 clog 页有排他访问 */
	byteval = *byteptr;
	byteval &= ~(((1 << CLOG_BITS_PER_XACT) - 1) << bshift);
	byteval |= (status << bshift);
	*byteptr = byteval;

	/*
	 * 若事务完成 LSN 更高，则更新组 LSN。
	 *
	 * 注意：在 InRecovery 处理期间提供的 lsn 将无效，
	 * 因此我们无需做任何特殊处理来避免恢复期间的
	 * LSN 更新。恢复完成后，下一次 clog 变更将正确设置
	 * LSN。
	 */
	if (!XLogRecPtrIsInvalid(lsn))
	{
		int			lsnindex = GetLSNIndex(slotno, xid);

		if (XactCtl->shared->group_lsn[lsnindex] < lsn)
			XactCtl->shared->group_lsn[lsnindex] = lsn;
	}
}

/*
 * 查询提交日志中某事务的状态。
 *
 * 除实际提交状态外，本函数还（写入 *lsn）
 * 返回一个足够靠后的 LSN，以保证若我们刷写到
 * 该 LSN，则已将该事务的提交记录刷写到磁盘。
 * 结果未必恰好是该事务提交
 * 记录的精确 LSN！例如，对很久以前的事务（其 clog 页
 * 已落到磁盘），我们将返回 InvalidXLogRecPtr。另外，因为
 * 我们把同一 clog 页上的事务分组以节省存储，我们可能
 * 返回落入同一组的稍后事务的 LSN。
 *
 * NB：这是低层例程，并非大多数用途的首选入口
 * 点；预期调用方是 transam.c 中的 TransactionLogFetch()。
 */
XidStatus
TransactionIdGetStatus(TransactionId xid, XLogRecPtr *lsn)
{
	int			pageno = TransactionIdToPage(xid);
	int			byteno = TransactionIdToByte(xid);
	int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
	int			slotno;
	int			lsnindex;
	char	   *byteptr;
	XidStatus	status;

	/* 锁由 SimpleLruReadPage_ReadOnly 获取 */

	slotno = SimpleLruReadPage_ReadOnly(XactCtl, pageno, xid);
	byteptr = XactCtl->shared->page_buffer[slotno] + byteno;

	status = (*byteptr >> bshift) & CLOG_XACT_BITMASK;

	lsnindex = GetLSNIndex(slotno, xid);
	*lsn = XactCtl->shared->group_lsn[lsnindex];

	LWLockRelease(XactSLRULock);

	return status;
}

/*
 * 共享 CLOG 缓冲区数目。
 *
 * 在较大的多处理器系统上，可能同时有许多 CLOG 页
 * 请求在途，若所需页不在内存中，可能导致对 CLOG
 * 页的磁盘访问。测试表明我们
 * 以 128 个 CLOG 缓冲区可获得最佳性能；再多
 * 并不能提升性能。
 *
 * 无条件把 CLOG 缓冲区数目保持为 128 似乎
 * 不是好主意，因为那会提高启动所需的共享内存
 * 下限，对运行非常小配置的人可能是
 * 问题。下列公式似乎代表合理的
 * 折中：shared_buffers 很低的人也会得到较少的
 * CLOG 缓冲区，其他人则得到 128。
 */
Size
CLOGShmemBuffers(void)
{
	return Min(128, Max(4, NBuffers / 512));
}

/*
 * 初始化 CLOG 的共享内存
 */
Size
CLOGShmemSize(void)
{
	return SimpleLruShmemSize(CLOGShmemBuffers(), CLOG_LSNS_PER_PAGE);
}

void
CLOGShmemInit(void)
{
	XactCtl->PagePrecedes = CLOGPagePrecedes;
	SimpleLruInit(XactCtl, "Xact", CLOGShmemBuffers(), CLOG_LSNS_PER_PAGE,
				  XactSLRULock, "pg_xact", LWTRANCHE_XACT_BUFFER,
				  SYNC_HANDLER_CLOG);
	SlruPagePrecedesUnitTests(XactCtl, CLOG_XACTS_PER_PAGE);
}

/*
 * 本函数必须在系统安装时调用一次。它创建
 * 初始 CLOG 段。（假定 CLOG 目录已由
 * initdb 创建，且 CLOGShmemInit 必须已经
 * 调用过。）
 */
void
BootStrapCLOG(void)
{
	int			slotno;

	LWLockAcquire(XactSLRULock, LW_EXCLUSIVE);

	/* 创建并清零提交日志的第一页 */
	slotno = ZeroCLOGPage(0, false);

	/* 确保已写出 */
	SimpleLruWritePage(XactCtl, slotno);
	Assert(!XactCtl->shared->page_dirty[slotno]);

	LWLockRelease(XactSLRULock);
}

/*
 * 把一页 CLOG 初始化（或重新初始化）为零。
 * 若 writeXlog 为 true，还发出一条说明我们做了此事的 XLOG 记录。
 *
 * 该页实际上并不写出，只是在共享内存中设置好。
 * 返回新页的槽号。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 */
static int
ZeroCLOGPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(XactCtl, pageno);

	if (writeXlog)
		WriteZeroPageXlogRec(pageno);

	return slotno;
}

/*
 * 本函数必须在 postmaster 或单机后端启动期间调用一次，
 * 且在 StartupXLOG 已初始化 ShmemVariableCache->nextXid 之后。
 */
void
StartupCLOG(void)
{
	TransactionId xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	int			pageno = TransactionIdToPage(xid);

	LWLockAcquire(XactSLRULock, LW_EXCLUSIVE);

	/*
	 * 初始化我们认为的最新页号。
	 */
	XactCtl->shared->latest_page_number = pageno;

	LWLockRelease(XactSLRULock);
}

/*
 * 本函数必须在启动/恢复结束时调用一次。
 */
void
TrimCLOG(void)
{
	TransactionId xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	int			pageno = TransactionIdToPage(xid);

	LWLockAcquire(XactSLRULock, LW_EXCLUSIVE);

	/*
	 * 把当前 clog 页的剩余部分清零。在正常
	 * 情况下它本应已是零，但至少在理论上
	 * 似乎可能：XLOG 重放会落在一个小于上一数据库生命周期
	 * 实际使用并标记过的最后 XID 的 nextXID
	 * 值上（因为子事务提交会写 clog
	 * 但不产生 WAL 项）。为安全起见还是清零。（我们不必担心
	 * 当前页之后的页，因为那些会在首次
	 * 使用时清零。同理，当
	 * nextXid 恰好在页边界上时无需做任何事；那种情况下
	 * "当前"页很可能尚不存在。）
	 */
	if (TransactionIdToPgIndex(xid) != 0)
	{
		int			byteno = TransactionIdToByte(xid);
		int			bshift = TransactionIdToBIndex(xid) * CLOG_BITS_PER_XACT;
		int			slotno;
		char	   *byteptr;

		slotno = SimpleLruReadPage(XactCtl, pageno, false, xid);
		byteptr = XactCtl->shared->page_buffer[slotno] + byteno;

		/* 清零当前字节中迄今未用的位置 */
		*byteptr &= (1 << bshift) - 1;
		/* 清零该页其余部分 */
		MemSet(byteptr + 1, 0, BLCKSZ - byteno - 1);

		XactCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(XactSLRULock);
}

/*
 * 执行检查点——无论是在关机期间还是联机执行
 */
void
CheckPointCLOG(void)
{
	/*
	 * 将脏 CLOG 页写出到磁盘。这可能导致同步请求
	 * 排队，稍后由 ProcessSyncRequests() 作为
	 * 检查点的一部分处理。
	 */
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_START(true);
	SimpleLruWriteAll(XactCtl, true);
	TRACE_POSTGRESQL_CLOG_CHECKPOINT_DONE(true);
}


/*
 * 确保 CLOG 有空间容纳新分配的 XID。
 *
 * NB：调用时持有 XidGenLock。我们希望它在多数时候非常快
 * ；即使不那么快，除非被迫
 * 写出脏 clog 或 xlog 页以腾出
 * 共享内存空间，否则也不需要实际 I/O。
 */
void
ExtendCLOG(TransactionId newestXact)
{
	int			pageno;

	/*
	 * 除页上第一个 XID 外无工作。但要注意：刚
	 * 回卷后，第零页的第一个 XID 是 FirstNormalTransactionId。
	 */
	if (TransactionIdToPgIndex(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);

	LWLockAcquire(XactSLRULock, LW_EXCLUSIVE);

	/* 清零该页并为此记一条 XLOG 项 */
	ZeroCLOGPage(pageno, true);

	LWLockRelease(XactSLRULock);
}


/*
 * 移除持有所传事务 ID 的那个段之前的全部 CLOG 段
 *
 * 在移除任何 CLOG 数据之前，必须把 XLOG 刷写到磁盘，以确保
 * 任何最近发出的 FREEZE_PAGE 记录已到达磁盘；否则
 * 崩溃并重启可能使我们留下引用已移除
 * CLOG 数据的未冻结元组。我们选择也发出一条特殊的 TRUNCATE XLOG 记录。
 * 从 XLOG 重放删除并非关键，因为这些文件完全可以
 * 稍后移除，但这样做可防止长时间运行的热
 * 备服务器获得不合理膨胀的 CLOG 目录。
 *
 * 由于 CLOG 段容纳大量事务，实际移除一段的机会
 * 相当少见，因此似乎最好在确认
 * 存在可移除段之前不要做 XLOG 刷写。
 */
void
TruncateCLOG(TransactionId oldestXact, Oid oldestxid_datoid)
{
	int			cutoffPage;

	/*
	 * 截断点是包含 oldestXact 的段的起始处。我们
	 * 把包含 oldestXact 的*页*传给 SimpleLruTruncate。
	 */
	cutoffPage = TransactionIdToPage(oldestXact);

	/* 检查是否有可移除的文件 */
	if (!SlruScanDirectory(XactCtl, SlruScanDirCbReportPresence, &cutoffPage))
		return;					/* 没什么可移除 */

	/*
	 * 在截断 clog 之前推进 oldestClogXid，以便并发的 xact 状态
	 * 查找能确保它们不尝试访问已被截断掉的 clog。
	 *
	 * 仅当我们确实会截断掉 clog
	 * 页时才有必要这样做。
	 */
	AdvanceOldestClogXid(oldestXact);

	/*
	 * 写 XLOG 记录并把 XLOG 刷写到磁盘。我们在此记录正在保留信息的
	 * 最老 xid，以便在崩溃时确保它始终
	 * 领先于 clog 截断，并使备库在下一检查点之前得知
	 * 新的有效 xid。
	 */
	WriteTruncateXlogRec(cutoffPage, oldestXact, oldestxid_datoid);

	/* 现在可以移除旧的 CLOG segment(s) */
	SimpleLruTruncate(XactCtl, cutoffPage);
}


/*
 * 判定某 CLOG 页号对截断而言是否"更老"。
 *
 * 这里需要用 TransactionIds 比较，以便在回卷 XID 算术下做对
 * 事情。然而 TransactionIdPrecedes()
 * 会对永久 xact ID 表现怪异。因此，把两者都偏移，使 xid1、
 * xid2，以及 xid2 + CLOG_XACTS_PER_PAGE - 1 都是普通 XIDs；此偏移
 * 与第 0 页以及第 0 页之前的那一页相关。
 *
 * 包含 oldestXact-2^31 的页是重要的边界情形。该
 * 页中等于或晚于 oldestXact-2^31 的部分可丢弃，
 * 但早于 oldestXact-2^31 的部分则不可。当 oldestXact-2^31 是
 * 某页与段的第一个 XID 时，整页与整段都
 * 可丢弃，我们可以截断该段。识别那种情形需要
 * 让本回调能拿到 oldestXact，而不只是包含 oldestXact 的
 * 页。收益少见且很小，因此我们
 * 不优化该边界情形。
 */
static bool
CLOGPagePrecedes(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * CLOG_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * CLOG_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + CLOG_XACTS_PER_PAGE - 1));
}


/*
 * 写一条 ZEROPAGE xlog 记录
 */
static void
WriteZeroPageXlogRec(int pageno)
{
	XLogBeginInsert();
	XLogRegisterData((char *) (&pageno), sizeof(int));
	(void) XLogInsert(RM_CLOG_ID, CLOG_ZEROPAGE);
}

/*
 * 写一条 TRUNCATE xlog 记录
 *
 * 返回前必须把 xlog 记录刷写到磁盘——见 TruncateCLOG()
 * 中的说明。
 */
static void
WriteTruncateXlogRec(int pageno, TransactionId oldestXact, Oid oldestXactDb)
{
	XLogRecPtr	recptr;
	xl_clog_truncate xlrec;

	xlrec.pageno = pageno;
	xlrec.oldestXact = oldestXact;
	xlrec.oldestXactDb = oldestXactDb;

	XLogBeginInsert();
	XLogRegisterData((char *) (&xlrec), sizeof(xl_clog_truncate));
	recptr = XLogInsert(RM_CLOG_ID, CLOG_TRUNCATE);
	XLogFlush(recptr);
}

/*
 * CLOG 资源管理器的例程
 */
void
clog_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* clog 记录不使用备份块 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == CLOG_ZEROPAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(XactSLRULock, LW_EXCLUSIVE);

		slotno = ZeroCLOGPage(pageno, false);
		SimpleLruWritePage(XactCtl, slotno);
		Assert(!XactCtl->shared->page_dirty[slotno]);

		LWLockRelease(XactSLRULock);
	}
	else if (info == CLOG_TRUNCATE)
	{
		xl_clog_truncate xlrec;

		memcpy(&xlrec, XLogRecGetData(record), sizeof(xl_clog_truncate));

		AdvanceOldestClogXid(xlrec.oldestXact);

		SimpleLruTruncate(XactCtl, xlrec.pageno);
	}
	else
		elog(PANIC, "clog_redo: unknown op code %u", info);
}

/*
 * sync.c 同步 clog 文件的入口点。
 */
int
clogsyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(XactCtl, ftag, path);
}
