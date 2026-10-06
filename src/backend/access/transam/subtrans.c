/*-------------------------------------------------------------------------
 *
 * subtrans.c
 *		PostgreSQL 子事务日志管理器
 *
 * pg_subtrans 管理器类似 pg_xact，为每个事务存储其父
 * 事务 Id。它是嵌套事务实现的基础组成部分。主事务的父
 * 为 InvalidTransactionId，每个子事务则指向其直接父事务。
 * 可以方便地从子到父遍历该树，但不能
 * 反向遍历。
 *
 * 本代码基于 xact.c，但对稳健性的要求
 * 与 pg_xact 完全不同，因为我们只需记住
 * 当前仍打开事务的 pg_subtrans 信息。因此，无需
 * 在崩溃与重启后保留这些数据。
 *
 * 由于不关心跨崩溃保留数据，故无 XLOG 交互。
 * 数据库启动时，我们只需将当前活动的
 * SUBTRANS 页强制清零即可。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/subtrans.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slru.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "pg_trace.h"
#include "utils/snapmgr.h"


/*
 * SubTrans 页大小相关定义。页大小与 Postgres
 * 其他地方一样，均为 BLCKSZ。
 *
 * 注意：由于 TransactionIds 为 32 位并在 0xFFFFFFFF 处回卷，
 * SubTrans 页编号也会在
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE 处回卷，段编号在
 * 0xFFFFFFFF/SUBTRANS_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT 处回卷。本模块无需
 * 显式处理该事实，仅在 TruncateSUBTRANS 中比较段
 * 与页号时（见 SubTransPagePrecedes），以及在 StartupSUBTRANS
 * 中清零时需要注意。
 */

/* 每个事务需要四个字节 */
#define SUBTRANS_XACTS_PER_PAGE (BLCKSZ / sizeof(TransactionId))

#define TransactionIdToPage(xid) ((xid) / (TransactionId) SUBTRANS_XACTS_PER_PAGE)
#define TransactionIdToEntry(xid) ((xid) % (TransactionId) SUBTRANS_XACTS_PER_PAGE)


/*
 * 指向 SUBTRANS 控制用共享内存数据结构的链接
 */
static SlruCtlData SubTransCtlData;

#define SubTransCtl  (&SubTransCtlData)


static int	ZeroSUBTRANSPage(int pageno);
static bool SubTransPagePrecedes(int page1, int page2);


/*
 * 在子事务日志中记录子事务的父事务。
 */
void
SubTransSetParent(TransactionId xid, TransactionId parent)
{
	int			pageno = TransactionIdToPage(xid);
	int			entryno = TransactionIdToEntry(xid);
	int			slotno;
	TransactionId *ptr;

	Assert(TransactionIdIsValid(parent));
	Assert(TransactionIdFollows(xid, parent));

	LWLockAcquire(SubtransSLRULock, LW_EXCLUSIVE);

	slotno = SimpleLruReadPage(SubTransCtl, pageno, true, xid);
	ptr = (TransactionId *) SubTransCtl->shared->page_buffer[slotno];
	ptr += entryno;

	/*
	 * 有可能多次尝试设置父 xid，但我们
	 * 绝不应把一个有效 xid 改成另一个有效
	 * xid，否则会破坏数据结构。
	 */
	if (*ptr != parent)
	{
		Assert(*ptr == InvalidTransactionId);
		*ptr = parent;
		SubTransCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(SubtransSLRULock);
}

/*
 * 查询子事务日志中某事务的父事务。
 */
TransactionId
SubTransGetParent(TransactionId xid)
{
	int			pageno = TransactionIdToPage(xid);
	int			entryno = TransactionIdToEntry(xid);
	int			slotno;
	TransactionId *ptr;
	TransactionId parent;

	/* 无法查询可能已不存在的内容 */
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	/* Bootstrap 与冻结 XIDs 没有父事务 */
	if (!TransactionIdIsNormal(xid))
		return InvalidTransactionId;

	/* 锁由 SimpleLruReadPage_ReadOnly 获取 */

	slotno = SimpleLruReadPage_ReadOnly(SubTransCtl, pageno, xid);
	ptr = (TransactionId *) SubTransCtl->shared->page_buffer[slotno];
	ptr += entryno;

	parent = *ptr;

	LWLockRelease(SubtransSLRULock);

	return parent;
}

/*
 * SubTransGetTopmostTransaction
 *
 * 返回给定事务 id 的最顶层事务。
 *
 * 由于无法回溯到比 TransactionXmin 更早的记录，本函数
 * 有可能"撒谎"，返回某个中间子事务 ID
 * 而非真正的最顶层父 ID。这是可以接受的，因为实践中
 * 我们只关心检测最顶层父事务是否仍在运行，
 * 或是否出现在当前快照的仍在运行事务列表中。
 * 因此，任何早于 TransactionXmin 的 XID 彼此等价。
 */
TransactionId
SubTransGetTopmostTransaction(TransactionId xid)
{
	TransactionId parentXid = xid,
				previousXid = xid;

	/* 无法查询可能已不存在的内容 */
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	while (TransactionIdIsValid(parentXid))
	{
		previousXid = parentXid;
		if (TransactionIdPrecedes(parentXid, TransactionXmin))
			break;
		parentXid = SubTransGetParent(parentXid);

		/*
		 * 按约定父 xid 先分配，因此应始终
		 * 先于子 xid。其他情况表明数据结构已损坏，
		 * 可能导致无限循环，故退出。
		 */
		if (!TransactionIdPrecedes(parentXid, previousXid))
			elog(ERROR, "pg_subtrans contains invalid entry: xid %u points to parent xid %u",
				 previousXid, parentXid);
	}

	Assert(TransactionIdIsValid(previousXid));

	return previousXid;
}


/*
 * 初始化 SUBTRANS 的共享内存
 */
Size
SUBTRANSShmemSize(void)
{
	return SimpleLruShmemSize(NUM_SUBTRANS_BUFFERS, 0);
}

void
SUBTRANSShmemInit(void)
{
	SubTransCtl->PagePrecedes = SubTransPagePrecedes;
	SimpleLruInit(SubTransCtl, "Subtrans", NUM_SUBTRANS_BUFFERS, 0,
				  SubtransSLRULock, "pg_subtrans",
				  LWTRANCHE_SUBTRANS_BUFFER, SYNC_HANDLER_NONE);
	SlruPagePrecedesUnitTests(SubTransCtl, SUBTRANS_XACTS_PER_PAGE);
}

/*
 * 本函数必须在系统安装时调用一次。它创建
 * 初始 SUBTRANS 段。（假定 SUBTRANS 目录已由
 * initdb shell 脚本创建，且 SUBTRANSShmemInit
 * 已被调用。）
 *
 * 注意：其实不必现在就创建初始段，
 * 因为 slru.c 会在首次写入时创建。但我们仍
 * 这样做，以确保目录设置正确。
 */
void
BootStrapSUBTRANS(void)
{
	int			slotno;

	LWLockAcquire(SubtransSLRULock, LW_EXCLUSIVE);

	/* 创建并清零子事务日志的第一页 */
	slotno = ZeroSUBTRANSPage(0);

	/* 确保已写出 */
	SimpleLruWritePage(SubTransCtl, slotno);
	Assert(!SubTransCtl->shared->page_dirty[slotno]);

	LWLockRelease(SubtransSLRULock);
}

/*
 * 将一页 SUBTRANS 初始化（或重新初始化）为零。
 *
 * 页实际上不会写出，只是在共享内存中建好。
 * 返回新页的槽号。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 */
static int
ZeroSUBTRANSPage(int pageno)
{
	return SimpleLruZeroPage(SubTransCtl, pageno);
}

/*
 * 必须在 postmaster 或独立后端启动期间调用一次，
 * 且在 StartupXLOG 已初始化 ShmemVariableCache->nextXid 之后。
 *
 * oldestActiveXID 是任一预备事务中最老的 XID；若无则
 * 为 nextXid。
 */
void
StartupSUBTRANS(TransactionId oldestActiveXID)
{
	FullTransactionId nextXid;
	int			startPage;
	int			endPage;

	/*
	 * 由于我们不期望 pg_subtrans 在崩溃后仍然有效，
	 * 启动时将当前活动 page（页）清零。
	 * 每当进入新页时，ExtendSUBTRANS 同样会清零
	 * 新页，而不理会磁盘上原先的内容。
	 */
	LWLockAcquire(SubtransSLRULock, LW_EXCLUSIVE);

	startPage = TransactionIdToPage(oldestActiveXID);
	nextXid = ShmemVariableCache->nextXid;
	endPage = TransactionIdToPage(XidFromFullTransactionId(nextXid));

	while (startPage != endPage)
	{
		(void) ZeroSUBTRANSPage(startPage);
		startPage++;
		/* 必须考虑回卷 */
		if (startPage > TransactionIdToPage(MaxTransactionId))
			startPage = 0;
	}
	(void) ZeroSUBTRANSPage(startPage);

	LWLockRelease(SubtransSLRULock);
}

/*
 * 执行检查点 —— 无论是关闭期间还是联机进行
 */
void
CheckPointSUBTRANS(void)
{
	/*
	 * 将脏的 SUBTRANS 页写出到磁盘
	 *
	 * 从正确性角度看这并非必要。我们这样做
	 * 只是为了提高由检查点进程（而非后端）
	 * 写出脏页的几率。
	 */
	TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_START(true);
	SimpleLruWriteAll(SubTransCtl, true);
	TRACE_POSTGRESQL_SUBTRANS_CHECKPOINT_DONE(true);
}


/*
 * 确保 SUBTRANS 能为新分配的 XID 留出空间。
 *
 * 注意：调用时持有 XidGenLock。我们希望它在多数情况下
 * 非常快；即使不够快，除非被迫写出脏的
 * 子事务页以为共享内存腾出空间，否则
 * 也不会发生实际 I/O。
 */
void
ExtendSUBTRANS(TransactionId newestXact)
{
	int			pageno;

	/*
	 * 仅在页的第一个 XID 时才有工作。但要注意：刚发生
	 * 回卷后，第零页的第一个 XID 是 FirstNormalTransactionId。
	 */
	if (TransactionIdToEntry(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPage(newestXact);

	LWLockAcquire(SubtransSLRULock, LW_EXCLUSIVE);

	/* 清零该页 */
	ZeroSUBTRANSPage(pageno);

	LWLockRelease(SubtransSLRULock);
}


/*
 * 删除持有所传事务 ID 的段之前的所有 SUBTRANS 段
 *
 * oldestXact 是任一运行中事务的最老 TransactionXmin。本
 * 函数仅在检查点期间调用。
 */
void
TruncateSUBTRANS(TransactionId oldestXact)
{
	int			cutoffPage;

	/*
	 * 截断点是包含 oldestXact 的段起点。我们
	 * 把包含 oldestXact 的*页*传给 SimpleLruTruncate。我们先
	 * 回退一个事务，以免在罕见情况下 oldestXact
	 * 恰好是某页第一项且 oldestXact == next XID 时，传入尚未
	 * 创建的截断页。若不减一，就会触发
	 * SimpleLruTruncate 的回卷检测。
	 */
	TransactionIdRetreat(oldestXact);
	cutoffPage = TransactionIdToPage(oldestXact);

	SimpleLruTruncate(SubTransCtl, cutoffPage);
}


/*
 * 判断某一 SUBTRANS 页号在截断意义上是否"更旧"。
 * 与 CLOGPagePrecedes() 类似。
 */
static bool
SubTransPagePrecedes(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * SUBTRANS_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * SUBTRANS_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + SUBTRANS_XACTS_PER_PAGE - 1));
}
