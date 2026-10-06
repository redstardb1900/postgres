/*-------------------------------------------------------------------------
 *
 * slru.c
 *		事务状态日志文件的简单 LRU 缓冲
 *
 * 我们使用简单的最近最少使用（LRU）策略管理一池页
 * 缓冲区。通常预期写流量主要落在最新页（以及页切换
 * 之后不久的前一页）。读流量可能会触及更大范围的
 * 页，但无论如何较少数量的页缓冲区就应足够。因此我们
 * 只用普通线性搜索查找缓冲区；不必上哈希表或更复杂的
 * 结构。管理算法就是标准 LRU，唯一例外是永远不会换出
 * 最新页（因为我们知道它迟早还会再被命中）。
 *
 * 我们用一把控制 LWLock 保护共享数据结构，再加上
 * 按缓冲区的 LWLocks 来同步每个缓冲区的 I/O。检查或修改任何
 * 共享状态时必须持有控制锁。正在读入或写出页缓冲区的
 * 进程不持有控制锁，只持有它所操作的那个缓冲区的
 * 按缓冲区锁。
 *
 * "持有控制锁"在除 SimpleLruReadPage_ReadOnly() 以外的
 * 所有情形下都意味着排他锁；其含义见 SlruRecentlyUsed() 的
 * 注释。
 *
 * 对某缓冲区启动 I/O 时，我们在释放控制锁之前先以排他方式
 * 获取按缓冲区锁。完成 I/O、重新获取控制锁并更新共享状态
 * 之后，才释放按缓冲区锁。（此处不可能死锁，因为若别人已在
 * 同一缓冲区上做 I/O，我们就绝不会再启动 I/O。）
 * 若要等待 I/O 完成：释放控制锁，以共享模式获取按缓冲区
 * 锁，立即释放按缓冲区锁，再重新获取控制锁，然后重新检查
 * 状态（因为在未持锁期间可能发生任意变化）。
 *
 * 与常规缓冲管理器一样，另一个进程可能把正在写出的
 * 页再次弄脏。处理方式是重新设置该页的 page_dirty
 * 标志。
 *
 *
 *
 *
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/slru.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/slru.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/shmem.h"

#define SlruFileName(ctl, path, seg) \
	snprintf(path, MAXPGPATH, "%s/%04X", (ctl)->Dir, seg)

/*
 * 在 SimpleLruWriteAll() 期间，通常只需写一两个物理文件，
 * 但每个文件可能要写若干页。通过让文件保持打开直到控制
 * 返回 SimpleLruWriteAll()，可以合并 I/O 请求。本数据结构
 * 记下哪些文件仍处于打开状态。
 */
#define MAX_WRITEALL_BUFFERS	16

typedef struct SlruWriteAllData
{
	int			num_files;		/* 实际打开的文件数 */
	int			fd[MAX_WRITEALL_BUFFERS];	/* 它们的 FD */
	int			segno[MAX_WRITEALL_BUFFERS];	/* 它们的日志段号 */
} SlruWriteAllData;

typedef struct SlruWriteAllData *SlruWriteAll;

/*
 * 填充描述段文件的文件标签。我们只用段号，因为可以为
 * clog、multixact 等分别提供同步处理函数，从而推导出
 * 其余所需信息。
 */
#define INIT_SLRUFILETAG(a,xx_handler,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = (xx_handler), \
	(a).segno = (xx_segno) \
)

/*
 * 把缓冲区槽标为"最近使用"。注意参数会被多次求值！
 *
 * 使用 if 测试的原因是：经常会连续多次访问同一页
 * （尤其是最新页）。通过抑制对 cur_lru_count 的无用递增，
 * 我们降低旧页计数"回卷"从而显得像最近使用的概率。
 *
 * 我们允许在 SimpleLruReadPage_ReadOnly() 内由多个进程
 * 并发执行本代码。只要 int 的读与写是原子的，就不至于
 * 把完全胡乱的值带进计算。不过，若某进程在执行本宏时
 * 被延迟，cur_lru_count 或个别 page_lru_count 项仍可能被
 * "复位"到低于应有的值。在 SlruSelectLRUPage() 中小心
 * 处理后，危害不大；绝对最坏的后果也只是换出页面的选择
 * 非最优。允许并发读取 SLRU 页的收益看起来值得这么做。
 *
 *
 *
 *
 */
#define SlruRecentlyUsed(shared, slotno)	\
	do { \
		int		new_lru_count = (shared)->cur_lru_count; \
		if (new_lru_count != (shared)->page_lru_count[slotno]) { \
			(shared)->cur_lru_count = ++new_lru_count; \
			(shared)->page_lru_count[slotno] = new_lru_count; \
		} \
	} while (0)

/* 供 SlruReportIOError 使用的已保存信息 */
typedef enum
{
	SLRU_OPEN_FAILED,
	SLRU_SEEK_FAILED,
	SLRU_READ_FAILED,
	SLRU_WRITE_FAILED,
	SLRU_FSYNC_FAILED,
	SLRU_CLOSE_FAILED
} SlruErrorCause;

static SlruErrorCause slru_errcause;
static int	slru_errno;


static void SimpleLruZeroLSNs(SlruCtl ctl, int slotno);
static void SimpleLruWaitIO(SlruCtl ctl, int slotno);
static void SlruInternalWritePage(SlruCtl ctl, int slotno, SlruWriteAll fdata);
static bool SlruPhysicalReadPage(SlruCtl ctl, int pageno, int slotno);
static bool SlruPhysicalWritePage(SlruCtl ctl, int pageno, int slotno,
								  SlruWriteAll fdata);
static void SlruReportIOError(SlruCtl ctl, int pageno, TransactionId xid);
static int	SlruSelectLRUPage(SlruCtl ctl, int pageno);

static bool SlruScanDirCbDeleteCutoff(SlruCtl ctl, char *filename,
									  int segpage, void *data);
static void SlruInternalDeleteSegment(SlruCtl ctl, int segno);

/*
 * 共享内存初始化
 */

Size
SimpleLruShmemSize(int nslots, int nlsns)
{
	Size		sz;

	/* 假定 nslots 不会大到有溢出风险 */
	sz = MAXALIGN(sizeof(SlruSharedData));
	sz += MAXALIGN(nslots * sizeof(char *));	/* page_buffer[] */
	sz += MAXALIGN(nslots * sizeof(SlruPageStatus));	/* page_status[] */
	sz += MAXALIGN(nslots * sizeof(bool));	/* page_dirty[] */
	sz += MAXALIGN(nslots * sizeof(int));	/* page_number[] */
	sz += MAXALIGN(nslots * sizeof(int));	/* page_lru_count[] */
	sz += MAXALIGN(nslots * sizeof(LWLockPadded));	/* buffer_locks[] */

	if (nlsns > 0)
		sz += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));	/* group_lsn[] */

	return BUFFERALIGN(sz) + BLCKSZ * nslots;
}

/*
 * 在共享内存中初始化或附着到一个简单 LRU 缓存。
 *
 * ctl: 本地（非共享）控制结构的地址。
 * name: SLRU 的名称。（用户可见，请慎重选取！）
 * nslots: 要使用的页槽数目。
 * nlsns: 每页的 LSN 组数（无关时设为零）。
 * ctllock: 用于控制对共享控制结构访问的 LWLock。
 * subdir: 相对于 PGDATA、将存放这些文件的子目录。
 * tranche_id: 该 SLRU 的按缓冲区 LWLocks 所用的 LWLock 批次 ID。
 * sync_handler: 用哪一套函数处理同步请求
 */
void
SimpleLruInit(SlruCtl ctl, const char *name, int nslots, int nlsns,
			  LWLock *ctllock, const char *subdir, int tranche_id,
			  SyncRequestHandler sync_handler)
{
	SlruShared	shared;
	bool		found;

	shared = (SlruShared) ShmemInitStruct(name,
										  SimpleLruShmemSize(nslots, nlsns),
										  &found);

	if (!IsUnderPostmaster)
	{
		/* 初始化锁与共享内存区域 */
		char	   *ptr;
		Size		offset;
		int			slotno;

		Assert(!found);

		memset(shared, 0, sizeof(SlruSharedData));

		shared->ControlLock = ctllock;

		shared->num_slots = nslots;
		shared->lsn_groups_per_page = nlsns;

		shared->cur_lru_count = 0;

		/* shared->latest_page_number 稍后设置 */

		shared->slru_stats_idx = pgstat_get_slru_index(name);

		ptr = (char *) shared;
		offset = MAXALIGN(sizeof(SlruSharedData));
		shared->page_buffer = (char **) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(char *));
		shared->page_status = (SlruPageStatus *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(SlruPageStatus));
		shared->page_dirty = (bool *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(bool));
		shared->page_number = (int *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(int));
		shared->page_lru_count = (int *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(int));

		/* 初始化 LWLocks */
		shared->buffer_locks = (LWLockPadded *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(LWLockPadded));

		if (nlsns > 0)
		{
			shared->group_lsn = (XLogRecPtr *) (ptr + offset);
			offset += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));
		}

		ptr += BUFFERALIGN(offset);
		for (slotno = 0; slotno < nslots; slotno++)
		{
			LWLockInitialize(&shared->buffer_locks[slotno].lock,
							 tranche_id);

			shared->page_buffer[slotno] = ptr;
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			shared->page_dirty[slotno] = false;
			shared->page_lru_count[slotno] = 0;
			ptr += BLCKSZ;
		}

		/* 应能装入估算的共享内存大小 */
		Assert(ptr - (char *) shared <= SimpleLruShmemSize(nslots, nlsns));
	}
	else
		Assert(found);

	/*
	 * 初始化非共享控制结构，包括目录路径。我们假定调用者
	 * 已设置 PagePrecedes。
	 */
	ctl->shared = shared;
	ctl->sync_handler = sync_handler;
	strlcpy(ctl->Dir, subdir, sizeof(ctl->Dir));
}

/*
 * 把一页初始化（或重新初始化）为零。
 *
 * 页实际上不会写出，只是在共享内存中建好。
 * 返回新页的槽号。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 */
int
SimpleLruZeroPage(SlruCtl ctl, int pageno)
{
	SlruShared	shared = ctl->shared;
	int			slotno;

	/* 为该页找一个合适的缓冲区槽 */
	slotno = SlruSelectLRUPage(ctl, pageno);
	Assert(shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
		   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno]) ||
		   shared->page_number[slotno] == pageno);

	/* 把该槽标为包含此页 */
	shared->page_number[slotno] = pageno;
	shared->page_status[slotno] = SLRU_PAGE_VALID;
	shared->page_dirty[slotno] = true;
	SlruRecentlyUsed(shared, slotno);

	/* 把缓冲区清零 */
	MemSet(shared->page_buffer[slotno], 0, BLCKSZ);

	/* 把此新页的 LSNs 设为零 */
	SimpleLruZeroLSNs(ctl, slotno);

	/* 假定此页现为最新活动页 */
	shared->latest_page_number = pageno;

	/* 更新已清零页的统计计数 */
	pgstat_count_slru_page_zeroed(shared->slru_stats_idx);

	return slotno;
}

/*
 * 把我们为该 slru 页存储的全部 LSNs 清零。
 *
 * 每次创建新页，以及每次从磁盘把一页读入已有缓冲区时，
 * 都应调用本函数。（这样的旧页不可能有有意义的 LSNs，
 * 因为我们当初写出该页之前早已刷写过它们。）
 *
 * 此处假定 InvalidXLogRecPtr 按位全为 0。
 *
 */
static void
SimpleLruZeroLSNs(SlruCtl ctl, int slotno)
{
	SlruShared	shared = ctl->shared;

	if (shared->lsn_groups_per_page > 0)
		MemSet(&shared->group_lsn[slotno * shared->lsn_groups_per_page], 0,
			   shared->lsn_groups_per_page * sizeof(XLogRecPtr));
}

/*
 * 等待页槽上任何活动 I/O 完成。（这并不保证返回前没有
 * 新的 I/O 被启动。事实上该槽甚至可能已不再包含同一页。）
 *
 * 进入时必须持有控制锁，退出时仍持有。
 *
 */
static void
SimpleLruWaitIO(SlruCtl ctl, int slotno)
{
	SlruShared	shared = ctl->shared;

	/* 见文件顶部的说明 */
	LWLockRelease(shared->ControlLock);
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED);
	LWLockRelease(&shared->buffer_locks[slotno].lock);
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	/*
	 * 若槽仍处于 io-in-progress 状态，则要么别人已在该槽上
	 * 启动了新的 I/O，要么先前的 I/O 失败且未复位页状态。
	 * 那本不该发生，但多花几个周期检查并恢复似乎值得。我们
	 * 可通过看缓冲区锁是否仍被持有来廉价地检测失败（假定
	 * 事务中止会释放该锁）。
	 *
	 */
	if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS ||
		shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS)
	{
		if (LWLockConditionalAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED))
		{
			/* 确实，I/O 一定已失败 */
			if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS)
				shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			else				/* write_in_progress */
			{
				shared->page_status[slotno] = SLRU_PAGE_VALID;
				shared->page_dirty[slotno] = true;
			}
			LWLockRelease(&shared->buffer_locks[slotno].lock);
		}
	}
}

/*
 * 在共享缓冲区中查找一页，必要时读入。
 * 页号必须对应已经初始化过的页。
 *
 * 若 write_ok 为真，则可以返回处于 WRITE_IN_PROGRESS
 * 状态的页；调用者有责任确保修改该页是安全的。若
 * write_ok 为假，则在页不再有活动 I/O 之前我们不会返回。
 *
 * 传入的 xid 仅用于错误报告；若无特定 xid 与该操作
 * 关联，可为 InvalidTransactionId。
 *
 * 返回值是当前持有该页的共享缓冲区槽号。
 * 会更新该缓冲区的 LRU 访问信息。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 *
 */
int
SimpleLruReadPage(SlruCtl ctl, int pageno, bool write_ok,
				  TransactionId xid)
{
	SlruShared	shared = ctl->shared;

	/* 外层循环处理必须等待别人 I/O 时的重启 */
	for (;;)
	{
		int			slotno;
		bool		ok;

		/* 看页是否已在内存中；否则挑选牺牲槽 */
		slotno = SlruSelectLRUPage(ctl, pageno);

		/* 我们是否在内存中找到了该页？ */
		if (shared->page_number[slotno] == pageno &&
			shared->page_status[slotno] != SLRU_PAGE_EMPTY)
		{
			/*
			 * 若页仍在读入，我们必须等待 I/O。同样，若页正在
			 * 写出且调用者说那不可接受，也要等待。
			 */
			if (shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS ||
				(shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS &&
				 !write_ok))
			{
				SimpleLruWaitIO(ctl, slotno);
				/* 现在必须从头重新检查状态 */
				continue;
			}
			/* 否则已可使用 */
			SlruRecentlyUsed(shared, slotno);

			/* 更新在 SLRU 中命中页的统计计数 */
			pgstat_count_slru_page_hit(shared->slru_stats_idx);

			return slotno;
		}

		/* 未找到匹配；断言我们选中了可释放的槽 */
		Assert(shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));

		/* 把该槽标为读忙 */
		shared->page_number[slotno] = pageno;
		shared->page_status[slotno] = SLRU_PAGE_READ_IN_PROGRESS;
		shared->page_dirty[slotno] = false;

		/* 获取按缓冲区锁（不会死锁，见顶部说明） */
		LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

		/* 做 I/O 期间释放控制锁 */
		LWLockRelease(shared->ControlLock);

		/* 执行读 */
		ok = SlruPhysicalReadPage(ctl, pageno, slotno);

		/* 把刚读入页的 LSNs 设为零 */
		SimpleLruZeroLSNs(ctl, slotno);

		/* 重新获取控制锁并更新页状态 */
		LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

		Assert(shared->page_number[slotno] == pageno &&
			   shared->page_status[slotno] == SLRU_PAGE_READ_IN_PROGRESS &&
			   !shared->page_dirty[slotno]);

		shared->page_status[slotno] = ok ? SLRU_PAGE_VALID : SLRU_PAGE_EMPTY;

		LWLockRelease(&shared->buffer_locks[slotno].lock);

		/* 现在可以在失败时 ereport 了 */
		if (!ok)
			SlruReportIOError(ctl, pageno, xid);

		SlruRecentlyUsed(shared, slotno);

		/* 更新未在 SLRU 中找到页的统计计数 */
		pgstat_count_slru_page_read(shared->slru_stats_idx);

		return slotno;
	}
}

/*
 * 在共享缓冲区中查找一页，必要时读入。
 * 页号必须对应已经初始化过的页。
 * 调用者只能打算对该页做只读访问。
 *
 * 传入的 xid 仅用于错误报告；若无特定 xid 与该操作
 * 关联，可为 InvalidTransactionId。
 *
 * 返回值是当前持有该页的共享缓冲区槽号。
 * 会更新该缓冲区的 LRU 访问信息。
 *
 * 进入时不得持有控制锁，但退出时将持有。
 * 锁是共享还是排他未指定。
 */
int
SimpleLruReadPage_ReadOnly(SlruCtl ctl, int pageno, TransactionId xid)
{
	SlruShared	shared = ctl->shared;
	int			slotno;

	/* 仅持共享锁时尝试查找该页 */
	LWLockAcquire(shared->ControlLock, LW_SHARED);

	/* 看页是否已在某缓冲区中 */
	for (slotno = 0; slotno < shared->num_slots; slotno++)
	{
		if (shared->page_number[slotno] == pageno &&
			shared->page_status[slotno] != SLRU_PAGE_EMPTY &&
			shared->page_status[slotno] != SLRU_PAGE_READ_IN_PROGRESS)
		{
			/* 见 SlruRecentlyUsed 宏的注释 */
			SlruRecentlyUsed(shared, slotno);

			/* 更新在 SLRU 中命中页的统计计数 */
			pgstat_count_slru_page_hit(shared->slru_stats_idx);

			return slotno;
		}
	}

	/* 没找到，切换为普通排他锁并做常规读 */
	LWLockRelease(shared->ControlLock);
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	return SimpleLruReadPage(ctl, pageno, true, xid);
}

/*
 * 必要时从共享缓冲区写出一页。
 * 若指定槽不是脏的则什么也不做。
 *
 * 注意：这里只尝试写一次。因此退出时页仍可能是脏的
 * （若写出期间别人再次弄脏了它）。不过，即使页已在
 * 写出中，我们*仍*会尝试重新写出；这是为检查点准备的。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 *
 */
static void
SlruInternalWritePage(SlruCtl ctl, int slotno, SlruWriteAll fdata)
{
	SlruShared	shared = ctl->shared;
	int			pageno = shared->page_number[slotno];
	bool		ok;

	/* 若写出正在进行，等待其完成 */
	while (shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS &&
		   shared->page_number[slotno] == pageno)
	{
		SimpleLruWaitIO(ctl, slotno);
	}

	/*
	 * 若页不是脏的，或缓冲区已不再包含我们被调用时所针对的
	 * 同一页，则什么也不做。
	 */
	if (!shared->page_dirty[slotno] ||
		shared->page_status[slotno] != SLRU_PAGE_VALID ||
		shared->page_number[slotno] != pageno)
		return;

	/*
	 * 把该槽标为写忙，并清除脏位。此后若有事务状态更新
	 * 触及此页，会再次把它标脏。
	 */
	shared->page_status[slotno] = SLRU_PAGE_WRITE_IN_PROGRESS;
	shared->page_dirty[slotno] = false;

	/* 获取按缓冲区锁（不会死锁，见顶部说明） */
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

	/* 做 I/O 期间释放控制锁 */
	LWLockRelease(shared->ControlLock);

	/* 执行写 */
	ok = SlruPhysicalWritePage(ctl, pageno, slotno, fdata);

	/* 若失败且我们处于刷写中，最好关闭这些文件 */
	if (!ok && fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
			CloseTransientFile(fdata->fd[i]);
	}

	/* 重新获取控制锁并更新页状态 */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	Assert(shared->page_number[slotno] == pageno &&
		   shared->page_status[slotno] == SLRU_PAGE_WRITE_IN_PROGRESS);

	/* 若写出失败，再次把页标脏 */
	if (!ok)
		shared->page_dirty[slotno] = true;

	shared->page_status[slotno] = SLRU_PAGE_VALID;

	LWLockRelease(&shared->buffer_locks[slotno].lock);

	/* 现在可以在失败时 ereport 了 */
	if (!ok)
		SlruReportIOError(ctl, pageno, InvalidTransactionId);

	/* 若属于检查点，把这计为写出的缓冲区。 */
	if (fdata)
		CheckpointStats.ckpt_bufs_written++;
}

/*
 * SlruInternalWritePage 的包装，供外部调用者使用。
 * 此处 fdata 总是传入 NULL。
 */
void
SimpleLruWritePage(SlruCtl ctl, int slotno)
{
	SlruInternalWritePage(ctl, slotno, NULL);
}

/*
 * 返回给定页是否在磁盘上存在。
 *
 * 返回假表示文件不存在，或者文件不够大、
 * 装不下给定页。
 */
bool
SimpleLruDoesPhysicalPageExist(SlruCtl ctl, int pageno)
{
	int			segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;
	bool		result;
	off_t		endpos;

	/* 更新已检查页的统计计数 */
	pgstat_count_slru_page_exists(ctl->shared->slru_stats_idx);

	SlruFileName(ctl, path, segno);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		/* 预期：文件不存在 */
		if (errno == ENOENT)
			return false;

		/* 按常规报告错误 */
		slru_errcause = SLRU_OPEN_FAILED;
		slru_errno = errno;
		SlruReportIOError(ctl, pageno, 0);
	}

	if ((endpos = lseek(fd, 0, SEEK_END)) < 0)
	{
		slru_errcause = SLRU_SEEK_FAILED;
		slru_errno = errno;
		SlruReportIOError(ctl, pageno, 0);
	}

	result = endpos >= (off_t) (offset + BLCKSZ);

	if (CloseTransientFile(fd) != 0)
	{
		slru_errcause = SLRU_CLOSE_FAILED;
		slru_errno = errno;
		return false;
	}

	return result;
}

/*
 * 把（先前已存在的）页物理读入缓冲区槽
 *
 * 失败时无法直接 ereport(ERROR)，因为调用者已在共享内存中
 * 放入必须撤销的状态。因此我们返回 false，并把足够信息
 * 保存在静态变量中，供 SlruReportIOError 报告。
 *
 * 目前假定不值得跨读/写操作保持文件指针打开。
 * 我们可以缓存一个虚拟文件指针……
 */
static bool
SlruPhysicalReadPage(SlruCtl ctl, int pageno, int slotno)
{
	SlruShared	shared = ctl->shared;
	int			segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	off_t		offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;

	SlruFileName(ctl, path, segno);

	/*
	 * 在崩溃并重启的情形下，我们可能收到要设置那些位已落在
	 * 已截断提交日志段中的事务的提交状态的命令（见
	 * SlruPhysicalWritePage 中的说明）。因此若处于 InRecovery，
	 * 允许文件不存在的情形，并改为返回全零。
	 *
	 */
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT || !InRecovery)
		{
			slru_errcause = SLRU_OPEN_FAILED;
			slru_errno = errno;
			return false;
		}

		ereport(LOG,
				(errmsg("file \"%s\" doesn't exist, reading as zeroes",
						path)));
		MemSet(shared->page_buffer[slotno], 0, BLCKSZ);
		return true;
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SLRU_READ);
	if (pg_pread(fd, shared->page_buffer[slotno], BLCKSZ, offset) != BLCKSZ)
	{
		pgstat_report_wait_end();
		slru_errcause = SLRU_READ_FAILED;
		slru_errno = errno;
		CloseTransientFile(fd);
		return false;
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
	{
		slru_errcause = SLRU_CLOSE_FAILED;
		slru_errno = errno;
		return false;
	}

	return true;
}

/*
 * 从缓冲区槽物理写出一页
 *
 * 失败时无法直接 ereport(ERROR)，因为调用者已在共享内存中
 * 放入必须撤销的状态。因此我们返回 false，并把足够信息
 * 保存在静态变量中，供 SlruReportIOError 报告。
 *
 * 目前假定不值得跨彼此独立的读/写操作保持文件指针
 * 打开。不过我们会在 SimpleLruWriteAll 期间做批处理。
 *
 * 独立写出时 fdata 为 NULL；在 SimpleLruWriteAll 期间则为
 * 打开文件信息的指针。
 *
 */
static bool
SlruPhysicalWritePage(SlruCtl ctl, int pageno, int slotno, SlruWriteAll fdata)
{
	SlruShared	shared = ctl->shared;
	int			segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	off_t		offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd = -1;

	/* 更新已写出页的统计计数 */
	pgstat_count_slru_page_written(shared->slru_stats_idx);

	/*
	 * 在适当时遵守先写 WAL 再写数据的规则，以免在关联的
	 * WAL 记录之前写出数据。这与主缓冲管理器中
	 * FlushBuffer() 所做的动作相同。
	 */
	if (shared->group_lsn != NULL)
	{
		/*
		 * 我们必须确定该页上最大的异步提交 LSN。这有点繁琐，
		 * 但既然整个函数本来就是慢路径，与其维护每页 LSN
		 * 变量（那会在事务提交路径上多一次比较），不如在这里
		 * 做。
		 *
		 */
		XLogRecPtr	max_lsn;
		int			lsnindex,
					lsnoff;

		lsnindex = slotno * shared->lsn_groups_per_page;
		max_lsn = shared->group_lsn[lsnindex++];
		for (lsnoff = 1; lsnoff < shared->lsn_groups_per_page; lsnoff++)
		{
			XLogRecPtr	this_lsn = shared->group_lsn[lsnindex++];

			if (max_lsn < this_lsn)
				max_lsn = this_lsn;
		}

		if (!XLogRecPtrIsInvalid(max_lsn))
		{
			/*
			 * 如上所述，此处无法接受 elog(ERROR)，因此若
			 * XLogFlush 失败，我们必须 PANIC。这限制不大，因为
			 * XLogFlush 几乎全在临界区里，不过还是明确一下。
			 *
			 */
			START_CRIT_SECTION();
			XLogFlush(max_lsn);
			END_CRIT_SECTION();
		}
	}

	/*
	 * 在 SimpleLruWriteAll 期间，我们可能已经打开了所需文件。
	 */
	if (fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
		{
			if (fdata->segno[i] == segno)
			{
				fd = fdata->fd[i];
				break;
			}
		}
	}

	if (fd < 0)
	{
		/*
		 * 若文件尚不存在，我们应创建它。写出并非段内第一页的
		 * 页时也可能需要创建；我们假定操作系统能应付。（注意：
		 * 看上去似乎只在对段的第一页调用 SimpleLruZeroPage
		 * 时创建文件就行。然而，若崩溃并重启后 REDO 逻辑选择从
		 * 早于最新检查点的检查点重放日志，则我们可能收到要设置
		 * 已从提交日志截断掉的事务的状态的命令。最简单的处理
		 * 方式是在此处以及 SlruPhysicalReadPage 中接受对不存在
		 * 文件的引用。）
		 *
		 * 注意：多个后端可能同时为本文件的不同页执行本代码。
		 * 因此不要使用 O_EXCL 或 O_TRUNC 或类似标志。
		 *
		 *
		 *
		 *
		 */
		SlruFileName(ctl, path, segno);
		fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
		if (fd < 0)
		{
			slru_errcause = SLRU_OPEN_FAILED;
			slru_errno = errno;
			return false;
		}

		if (fdata)
		{
			if (fdata->num_files < MAX_WRITEALL_BUFFERS)
			{
				fdata->fd[fdata->num_files] = fd;
				fdata->segno[fdata->num_files] = segno;
				fdata->num_files++;
			}
			else
			{
				/*
				 * 万一超过 MAX_WRITEALL_BUFFERS，则退回为
				 * 当作独立写出处理。
				 */
				fdata = NULL;
			}
		}
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SLRU_WRITE);
	if (pg_pwrite(fd, shared->page_buffer[slotno], BLCKSZ, offset) != BLCKSZ)
	{
		pgstat_report_wait_end();
		/* 若 write 未设置 errno，假定问题是磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		slru_errcause = SLRU_WRITE_FAILED;
		slru_errno = errno;
		if (!fdata)
			CloseTransientFile(fd);
		return false;
	}
	pgstat_report_wait_end();

	/* 为检查点进程排队一个同步请求。 */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
	{
		FileTag		tag;

		INIT_SLRUFILETAG(tag, ctl->sync_handler, segno);
		if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false))
		{
			/* 没有空间排队同步请求。同步地做。 */
			pgstat_report_wait_start(WAIT_EVENT_SLRU_SYNC);
			if (pg_fsync(fd) != 0)
			{
				pgstat_report_wait_end();
				slru_errcause = SLRU_FSYNC_FAILED;
				slru_errno = errno;
				CloseTransientFile(fd);
				return false;
			}
			pgstat_report_wait_end();
		}
	}

	/* 关闭文件，除非属于刷写请求的一部分。 */
	if (!fdata)
	{
		if (CloseTransientFile(fd) != 0)
		{
			slru_errcause = SLRU_CLOSE_FAILED;
			slru_errno = errno;
			return false;
		}
	}

	return true;
}

/*
 * 在 SlruPhysicalReadPage 或 SlruPhysicalWritePage 失败后发出错误消息。
 * 在清理共享内存状态之后调用本函数。
 */
static void
SlruReportIOError(SlruCtl ctl, int pageno, TransactionId xid)
{
	int			segno = pageno / SLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % SLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];

	SlruFileName(ctl, path, segno);
	errno = slru_errno;
	switch (slru_errcause)
	{
		case SLRU_OPEN_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("Could not open file \"%s\": %m.", path)));
			break;
		case SLRU_SEEK_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("Could not seek in file \"%s\" to offset %d: %m.",
							   path, offset)));
			break;
		case SLRU_READ_FAILED:
			if (errno)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not access status of transaction %u", xid),
						 errdetail("Could not read from file \"%s\" at offset %d: %m.",
								   path, offset)));
			else
				ereport(ERROR,
						(errmsg("could not access status of transaction %u", xid),
						 errdetail("Could not read from file \"%s\" at offset %d: read too few bytes.", path, offset)));
			break;
		case SLRU_WRITE_FAILED:
			if (errno)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not access status of transaction %u", xid),
						 errdetail("Could not write to file \"%s\" at offset %d: %m.",
								   path, offset)));
			else
				ereport(ERROR,
						(errmsg("could not access status of transaction %u", xid),
						 errdetail("Could not write to file \"%s\" at offset %d: wrote too few bytes.",
								   path, offset)));
			break;
		case SLRU_FSYNC_FAILED:
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("Could not fsync file \"%s\": %m.",
							   path)));
			break;
		case SLRU_CLOSE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("Could not close file \"%s\": %m.",
							   path)));
			break;
		default:
			/* 相信到不了这里 */
			elog(ERROR, "unrecognized SimpleLru error cause: %d",
				 (int) slru_errcause);
			break;
	}
}

/*
 * 当我们需要空闲槽时，选择要重用的槽。
 *
 * 传入目标页号，是因为要考虑在我们做 I/O 以腾出槽的
 * 同时，别的进程可能已把目标页读入。因此要检查或再检查
 * 是否已有槽持有目标页，若有则返回该槽。于是，返回的槽
 * *要么*已持有 pageno（可为除 EMPTY 外的任意状态），*要么*
 * 是可释放的槽（状态为 EMPTY 或 CLEAN）。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 *
 *
 */
static int
SlruSelectLRUPage(SlruCtl ctl, int pageno)
{
	SlruShared	shared = ctl->shared;

	/* 外层循环处理 I/O 后的重启 */
	for (;;)
	{
		int			slotno;
		int			cur_count;
		int			bestvalidslot = 0;	/* 让编译器安静 */
		int			best_valid_delta = -1;
		int			best_valid_page_number = 0; /* 让编译器安静 */
		int			bestinvalidslot = 0;	/* 让编译器安静 */
		int			best_invalid_delta = -1;
		int			best_invalid_page_number = 0;	/* 让编译器安静 */

		/* 看页是否已有分配的缓冲区 */
		for (slotno = 0; slotno < shared->num_slots; slotno++)
		{
			if (shared->page_number[slotno] == pageno &&
				shared->page_status[slotno] != SLRU_PAGE_EMPTY)
				return slotno;
		}

		/*
		 * 若找到任何 EMPTY 槽，就选它。否则挑选要替换的牺牲
		 * 页。通常取最近最少使用的有效页，但我们绝不会取
		 * 包含 latest_page_number 的槽，即使它看起来最近最少
		 * 使用。只有别无选择时我们才会选已在 I/O 忙的槽：读忙
		 * 槽在读完成后就不会再是最近最少使用，而等待写忙槽上的
		 * I/O 不如直接另选一槽。测试表明我们改选的槽常常是干净
		 * 的，从而可以马上开始读。
		 *
		 * 通常 page_lru_count 值各不相同，因而有明确的 LRU 页。
		 * 但因我们允许在 SimpleLruReadPage_ReadOnly() 内并发执行
		 * SlruRecentlyUsed()，多个页可能获得相同的 lru_count
		 * 值。那时我们以选择最靠后的页来打破平局。
		 *
		 * 注意下一行会强制把 cur_lru_count 推进到循环结束后
		 * 一定超过 page_lru_count 数组中任何值的值。这确保下一次
		 * 执行SlruRecentlyUsed时会把该页标为新近使用，即使它
		 * 当时持有当前计数器值。这让我们在多个页具有相同
		 * lru_count 时回到拥有良好数据的轨道上。
		 *
		 *
		 *
		 *
		 *
		 *
		 *
		 */
		cur_count = (shared->cur_lru_count)++;
		for (slotno = 0; slotno < shared->num_slots; slotno++)
		{
			int			this_delta;
			int			this_page_number;

			if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
				return slotno;
			this_delta = cur_count - shared->page_lru_count[slotno];
			if (this_delta < 0)
			{
				/*
				 * 清理共享更新可能导致 cur_count 递增"丢失"的
				 * 情况。我们回退页计数，而不是试图增大
				 * cur_count，以避免在计数已回卷时出现无限循环
				 * 或失败的任何疑问。
				 *
				 */
				shared->page_lru_count[slotno] = cur_count;
				this_delta = 0;
			}
			this_page_number = shared->page_number[slotno];
			if (this_page_number == shared->latest_page_number)
				continue;
			if (shared->page_status[slotno] == SLRU_PAGE_VALID)
			{
				if (this_delta > best_valid_delta ||
					(this_delta == best_valid_delta &&
					 ctl->PagePrecedes(this_page_number,
									   best_valid_page_number)))
				{
					bestvalidslot = slotno;
					best_valid_delta = this_delta;
					best_valid_page_number = this_page_number;
				}
			}
			else
			{
				if (this_delta > best_invalid_delta ||
					(this_delta == best_invalid_delta &&
					 ctl->PagePrecedes(this_page_number,
									   best_invalid_page_number)))
				{
					bestinvalidslot = slotno;
					best_invalid_delta = this_delta;
					best_invalid_page_number = this_page_number;
				}
			}
		}

		/*
		 * 若所有页（可能除最新页外）都在 I/O 忙，我们就得等待
		 * 某个 I/O 完成再重试。那种倒霉情形下，我们选择等待
		 * 最近最少使用槽上的 I/O，假定它很可能是所有进行中
		 * I/O 里最先启动的，因而可能最先结束。
		 *
		 */
		if (best_valid_delta < 0)
		{
			SimpleLruWaitIO(ctl, bestinvalidslot);
			continue;
		}

		/*
		 * 若选中的页是干净的，我们就搞定了。
		 */
		if (!shared->page_dirty[bestvalidslot])
			return bestvalidslot;

		/*
		 * 写出该页。
		 */
		SlruInternalWritePage(ctl, bestvalidslot, NULL);

		/*
		 * 现在回环并重试。这是处理诸如牺牲页在我们写出时又被
		 * 弄脏之类边角情形的最简单办法。
		 *
		 */
	}
}

/*
 * 在检查点或数据库关闭期间把脏页写出到磁盘。刷写推迟到
 * 下一次调用 ProcessSyncRequests()，不过我们会在这里对包含
 * 目录做 fsync，以确保新创建的目录项已落盘。
 *
 */
void
SimpleLruWriteAll(SlruCtl ctl, bool allow_redirtied)
{
	SlruShared	shared = ctl->shared;
	SlruWriteAllData fdata;
	int			slotno;
	int			pageno = 0;
	int			i;
	bool		ok;

	/* 更新刷写的统计计数 */
	pgstat_count_slru_flush(shared->slru_stats_idx);

	/*
	 * 查找并写出脏页
	 */
	fdata.num_files = 0;

	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	for (slotno = 0; slotno < shared->num_slots; slotno++)
	{
		SlruInternalWritePage(ctl, slotno, &fdata);

		/*
		 * 在某些地方（例如检查点），我们无法断言该槽现在是
		 * 干净的，因为另一个进程可能已经再次弄脏了它。那没
		 * 关系。
		 */
		Assert(allow_redirtied ||
			   shared->page_status[slotno] == SLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == SLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));
	}

	LWLockRelease(shared->ControlLock);

	/*
	 * 现在关闭所有曾打开的文件
	 */
	ok = true;
	for (i = 0; i < fdata.num_files; i++)
	{
		if (CloseTransientFile(fdata.fd[i]) != 0)
		{
			slru_errcause = SLRU_CLOSE_FAILED;
			slru_errno = errno;
			pageno = fdata.segno[i] * SLRU_PAGES_PER_SEGMENT;
			ok = false;
		}
	}
	if (!ok)
		SlruReportIOError(ctl, pageno, InvalidTransactionId);

	/* 确保新文件的目录项已落盘。 */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
		fsync_fname(ctl->Dir, true);
}

/*
 * 删除持有所传页号的段之前的所有段
 *
 * 所有 SLRUs 都会防止对本函数的并发调用，要么用 LWLock，
 * 要么仅在检查点中调用。互斥必须在计算 cutoffPage 之前
 * 开始。互斥必须在任何会允许其他后端向紧邻包含
 * cutoffPage 的段之前那个段写入新数据的限制更新之后结束。
 * 否则，当 SLRU 相当满时，SimpleLruTruncate() 可能在该段
 * 已累积新写入数据之后仍删除它。
 *
 */
void
SimpleLruTruncate(SlruCtl ctl, int cutoffPage)
{
	SlruShared	shared = ctl->shared;
	int			slotno;

	/* 更新截断的统计计数 */
	pgstat_count_slru_truncate(shared->slru_stats_idx);

	/*
	 * 扫描共享内存并移除截止页之前的任何页，以确保我们稍后
	 * 不会再写出它们。（由于通常在检查点中或紧随其后调用，
	 * 任何脏页本应已经刷写……这里只是格外小心。）
	 *
	 */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

restart:

	/*
	 * 持锁期间做一项重要安全检查：当前终点页不得符合
	 * 移除条件。
	 */
	if (ctl->PagePrecedes(shared->latest_page_number, cutoffPage))
	{
		LWLockRelease(shared->ControlLock);
		ereport(LOG,
				(errmsg("could not truncate directory \"%s\": apparent wraparound",
						ctl->Dir)));
		return;
	}

	for (slotno = 0; slotno < shared->num_slots; slotno++)
	{
		if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
			continue;
		if (!ctl->PagePrecedes(shared->page_number[slotno], cutoffPage))
			continue;

		/*
		 * 若页是干净的，只需把状态改为 EMPTY（预期情形）。
		 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno])
		{
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			continue;
		}

		/*
		 * 嗯，我们有（或可能有）作用于该页的 I/O 操作，因此必须
		 * 等待它们完成再重新开始。这与 SlruSelectLRUPage 中的
		 * 逻辑相同。（XXX 若页是脏的，不写就直接丢弃是否也行？
		 * SlruMayDeleteSegment() 使用更严格的判定，因此最终我们
		 * 可能并不删除此页；即使不删，也不会再有理由读其数据。
		 * 目前仍保持与原先相同的逻辑。）
		 *
		 *
		 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID)
			SlruInternalWritePage(ctl, slotno, NULL);
		else
			SimpleLruWaitIO(ctl, slotno);
		goto restart;
	}

	LWLockRelease(shared->ControlLock);

	/* 现在可以移除旧 segment(s) 了 */
	(void) SlruScanDirectory(ctl, SlruScanDirCbDeleteCutoff, &cutoffPage);
}

/*
 * 删除单个 SLRU 段。
 *
 * 注意：这不会触及 SLRU 缓冲区本身，调用者必须确保它们
 * 要么尚无法包含任何内容，要么已经被清理干净。
 */
static void
SlruInternalDeleteSegment(SlruCtl ctl, int segno)
{
	char		path[MAXPGPATH];

	/* 忘掉为该段排队的任何 fsync 请求。 */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
	{
		FileTag		tag;

		INIT_SLRUFILETAG(tag, ctl->sync_handler, segno);
		RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true);
	}

	/* 取消链接该文件。 */
	SlruFileName(ctl, path, segno);
	ereport(DEBUG2, (errmsg_internal("removing file \"%s\"", path)));
	unlink(path);
}

/*
 * 按段号删除单个 SLRU 段。
 */
void
SlruDeleteSegment(SlruCtl ctl, int segno)
{
	SlruShared	shared = ctl->shared;
	int			slotno;
	bool		did_write;

	/* 清理对该段可能存在的任何引用。 */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);
restart:
	did_write = false;
	for (slotno = 0; slotno < shared->num_slots; slotno++)
	{
		int			pagesegno = shared->page_number[slotno] / SLRU_PAGES_PER_SEGMENT;

		if (shared->page_status[slotno] == SLRU_PAGE_EMPTY)
			continue;

		/* 不是我们要找的段 */
		if (pagesegno != segno)
			continue;

		/* 若页是干净的，只需把状态改为 EMPTY（预期情形）。 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID &&
			!shared->page_dirty[slotno])
		{
			shared->page_status[slotno] = SLRU_PAGE_EMPTY;
			continue;
		}

		/* 与 SimpleLruTruncate() 相同的逻辑 */
		if (shared->page_status[slotno] == SLRU_PAGE_VALID)
			SlruInternalWritePage(ctl, slotno, NULL);
		else
			SimpleLruWaitIO(ctl, slotno);

		did_write = true;
	}

	/*
	 * 格外小心并重新检查。I/O 函数会释放控制锁，因此可能有
	 * 新页被读入。
	 */
	if (did_write)
		goto restart;

	SlruInternalDeleteSegment(ctl, segno);

	LWLockRelease(shared->ControlLock);
}

/*
 * 判定某段是否可以删除。
 *
 * segpage 是该段的第一页，cutoffPage 是 SLRU 中仍含有用数据的、
 * 按 PagePrecedes 顺序最老的页。由于每个核心 PagePrecedes
 * 回调都实现"回卷"，需检查该段的第一页与最后一页：
 *
 * first<cutoff  && last<cutoff:  是
 * first<cutoff  && last>=cutoff: 否；cutoff 落在本段内
 * first>=cutoff && last<cutoff:  否；回卷点落在本段内
 * first>=cutoff && last>=cutoff: 否；本段每一页都太新
 *
 */
static bool
SlruMayDeleteSegment(SlruCtl ctl, int segpage, int cutoffPage)
{
	int			seg_last_page = segpage + SLRU_PAGES_PER_SEGMENT - 1;

	Assert(segpage % SLRU_PAGES_PER_SEGMENT == 0);

	return (ctl->PagePrecedes(segpage, cutoffPage) &&
			ctl->PagePrecedes(seg_last_page, cutoffPage));
}

#ifdef USE_ASSERT_CHECKING
static void
SlruPagePrecedesTestOffset(SlruCtl ctl, int per_page, uint32 offset)
{
	TransactionId lhs,
				rhs;
	int			newestPage,
				oldestPage;
	TransactionId newestXact,
				oldestXact;

	/*
	 * 比较一对顺序未定义的 XID（见 RFC 1982），即位于 XID
	 * 空间"两端"的一对。TransactionIdPrecedes() 会把彼此都
	 * 视为先于对方。若 RHS 是 oldestXact，则 LHS 是我们不得
	 * 再分配的第一个 XID。
	 */
	lhs = per_page + offset;	/* 跳过第一页以避免非正规 XIDs */
	rhs = lhs + (1U << 31);
	Assert(TransactionIdPrecedes(lhs, rhs));
	Assert(TransactionIdPrecedes(rhs, lhs));
	Assert(!TransactionIdPrecedes(lhs - 1, rhs));
	Assert(TransactionIdPrecedes(rhs, lhs - 1));
	Assert(TransactionIdPrecedes(lhs + 1, rhs));
	Assert(!TransactionIdPrecedes(rhs, lhs + 1));
	Assert(!TransactionIdFollowsOrEquals(lhs, rhs));
	Assert(!TransactionIdFollowsOrEquals(rhs, lhs));
	Assert(!ctl->PagePrecedes(lhs / per_page, lhs / per_page));
	Assert(!ctl->PagePrecedes(lhs / per_page, rhs / per_page));
	Assert(!ctl->PagePrecedes(rhs / per_page, lhs / per_page));
	Assert(!ctl->PagePrecedes((lhs - per_page) / per_page, rhs / per_page));
	Assert(ctl->PagePrecedes(rhs / per_page, (lhs - 3 * per_page) / per_page));
	Assert(ctl->PagePrecedes(rhs / per_page, (lhs - 2 * per_page) / per_page));
	Assert(ctl->PagePrecedes(rhs / per_page, (lhs - 1 * per_page) / per_page)
		   || (1U << 31) % per_page != 0);	/* 见 CommitTsPagePrecedes() */
	Assert(ctl->PagePrecedes((lhs + 1 * per_page) / per_page, rhs / per_page)
		   || (1U << 31) % per_page != 0);
	Assert(ctl->PagePrecedes((lhs + 2 * per_page) / per_page, rhs / per_page));
	Assert(ctl->PagePrecedes((lhs + 3 * per_page) / per_page, rhs / per_page));
	Assert(!ctl->PagePrecedes(rhs / per_page, (lhs + per_page) / per_page));

	/*
	 * GetNewTransactionId() 已分配它能安全使用的最后一个 XID，
	 * 且该 XID 在第二段的*最后*一页。我们不得删除该段。
	 *
	 */
	newestPage = 2 * SLRU_PAGES_PER_SEGMENT - 1;
	newestXact = newestPage * per_page + offset;
	Assert(newestXact / per_page == newestPage);
	oldestXact = newestXact + 1;
	oldestXact -= 1U << 31;
	oldestPage = oldestXact / per_page;
	Assert(!SlruMayDeleteSegment(ctl,
								 (newestPage -
								  newestPage % SLRU_PAGES_PER_SEGMENT),
								 oldestPage));

	/*
	 * GetNewTransactionId() 已分配它能安全使用的最后一个 XID，
	 * 且该 XID 在第二段的 *FIRST* 页。我们不得删除该段。
	 *
	 */
	newestPage = SLRU_PAGES_PER_SEGMENT;
	newestXact = newestPage * per_page + offset;
	Assert(newestXact / per_page == newestPage);
	oldestXact = newestXact + 1;
	oldestXact -= 1U << 31;
	oldestPage = oldestXact / per_page;
	Assert(!SlruMayDeleteSegment(ctl,
								 (newestPage -
								  newestPage % SLRU_PAGES_PER_SEGMENT),
								 oldestPage));
}

/*
 * 对 PagePrecedes 函数做单元测试。
 *
 * 假定每个 >= FirstNormalTransactionId 的 uint32 都是有效键。
 * 假定每个值占用 SLRU 字节中一块连续、固定大小的区域。
 * （MultiXactMemberCtl 把标志与 XIDs 分开。NotifyCtl 有可变长
 * 条目、无键且无随机访问。这些单元测试不适用于它们。）
 *
 */
void
SlruPagePrecedesUnitTests(SlruCtl ctl, int per_page)
{
	/* 测试一页的首项、中间项与末项。 */
	SlruPagePrecedesTestOffset(ctl, per_page, 0);
	SlruPagePrecedesTestOffset(ctl, per_page, per_page / 2);
	SlruPagePrecedesTestOffset(ctl, per_page, per_page - 1);
}
#endif

/*
 * SlruScanDirectory 回调
 * 		若存在完全早于包含作为 "data" 传入的页的那个段的段，
 * 		本回调报告 true。
 */
bool
SlruScanDirCbReportPresence(SlruCtl ctl, char *filename, int segpage, void *data)
{
	int			cutoffPage = *(int *) data;

	if (SlruMayDeleteSegment(ctl, segpage, cutoffPage))
		return true;			/* 找到一个；不再迭代 */

	return false;				/* 继续 */
}

/*
 * SlruScanDirectory 回调。
 * 		本回调删除早于作为 "data" 传入的那个段的段。
 */
static bool
SlruScanDirCbDeleteCutoff(SlruCtl ctl, char *filename, int segpage, void *data)
{
	int			cutoffPage = *(int *) data;

	if (SlruMayDeleteSegment(ctl, segpage, cutoffPage))
		SlruInternalDeleteSegment(ctl, segpage / SLRU_PAGES_PER_SEGMENT);

	return false;				/* 继续 */
}

/*
 * SlruScanDirectory 回调。
 * 		本回调删除所有段。
 */
bool
SlruScanDirCbDeleteAll(SlruCtl ctl, char *filename, int segpage, void *data)
{
	SlruInternalDeleteSegment(ctl, segpage / SLRU_PAGES_PER_SEGMENT);

	return false;				/* 继续 */
}

/*
 * 扫描 SimpleLru 目录，并对其中找到的每个文件应用回调。
 *
 * 若回调返回 true，扫描停止。返回回调的最后返回值。
 *
 * 回调收到下列参数：1. 正被截断的 slru 的SlruCtl结构；
 * 2. 正在考虑的文件名；3. 该文件第一页的页号；4. 调用者
 * 给我们的不透明数据指针。
 *
 * 注意：扫描目录的顺序不作保证。
 *
 * 注意：不施加任何加锁。
 *
 *
 */
bool
SlruScanDirectory(SlruCtl ctl, SlruScanCallback callback, void *data)
{
	bool		retval = false;
	DIR		   *cldir;
	struct dirent *clde;
	int			segno;
	int			segpage;

	cldir = AllocateDir(ctl->Dir);
	while ((clde = ReadDir(cldir, ctl->Dir)) != NULL)
	{
		size_t		len;

		len = strlen(clde->d_name);

		if ((len == 4 || len == 5 || len == 6) &&
			strspn(clde->d_name, "0123456789ABCDEF") == len)
		{
			segno = (int) strtol(clde->d_name, NULL, 16);
			segpage = segno * SLRU_PAGES_PER_SEGMENT;

			elog(DEBUG2, "SlruScanDirectory invoking callback on %s/%s",
				 ctl->Dir, clde->d_name);
			retval = callback(ctl, clde->d_name, segpage, data);
			if (retval)
				break;
		}
	}
	FreeDir(cldir);

	return retval;
}

/*
 * 各个 SLRUs（clog、……）必须提供 sync.c 处理函数，以便它们能
 * 提供正确的 "SlruCtl"（否则我们不知道如何构造路径），但它们
 * 只需转发到这个执行 fsync 的公共实现。
 *
 */
int
SlruSyncFileTag(SlruCtl ctl, const FileTag *ftag, char *path)
{
	int			fd;
	int			save_errno;
	int			result;

	SlruFileName(ctl, path, ftag->segno);

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		return -1;

	pgstat_report_wait_start(WAIT_EVENT_SLRU_FLUSH_SYNC);
	result = pg_fsync(fd);
	pgstat_report_wait_end();
	save_errno = errno;

	CloseTransientFile(fd);

	errno = save_errno;
	return result;
}
