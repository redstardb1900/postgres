/*-------------------------------------------------------------------------
 *
 * sync.c
 *	  文件同步管理代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/sync/sync.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/commit_ts.h"
#include "access/clog.h"
#include "access/multixact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "postmaster/bgwriter.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/md.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"

/*
 * 在某些情形下（目前是独立后端和检查点进程），
 * 我们跟踪待处理的 fsync 操作：需要记住自上次检查点以来
 * 写过的全部关系段，以便在完成下一次检查点之前
 * 把它们 fsync 到磁盘。这张散列表
 * 记住这些待处理操作。我们用散列表，主要是当作
 * 合并重复请求的方便办法。
 *
 * 我们用类似的机制记住下次检查点之后可以
 * 删除的、不再需要的文件，但用的是链表而不是
 * 散列表，因为预计不会有重复请求。
 *
 * 这些机制只用于非临时关系；我们从不对
 * 临时关系做 fsync，也不需要推迟删除它们（见
 * mdunlink 里的注释）。
 *
 * （普通后端不在本地跟踪待处理操作，而是把它们
 * 转发给检查点进程。）
 */
typedef uint16 CycleCtr;		/* 可以是任何方便的整数宽度 */

typedef struct
{
	FileTag		tag;			/* 标识处理程序和文件 */
	CycleCtr	cycle_ctr;		/* 最旧请求的 sync_cycle_ctr */
	bool		canceled;		/* 若我们 "最近" 取消过，则 canceled 为 true */
} PendingFsyncEntry;

typedef struct
{
	FileTag		tag;			/* 标识处理程序和文件 */
	CycleCtr	cycle_ctr;		/* 发出请求时的 checkpoint_cycle_ctr */
	bool		canceled;		/* 若请求已被取消则为 true */
} PendingUnlinkEntry;

static HTAB *pendingOps = NULL;
static List *pendingUnlinks = NIL;
static MemoryContext pendingOpsCxt; /* 供上面这些使用的上下文 */

static CycleCtr sync_cycle_ctr = 0;
static CycleCtr checkpoint_cycle_ctr = 0;

/* 调用 AbsorbSyncRequests 的间隔 */
#define FSYNCS_PER_ABSORB		10
#define UNLINKS_PER_ABSORB		10

/*
 * 处理同步请求和 unlink 请求的函数指针。
 */
typedef struct SyncOps
{
	int			(*sync_syncfiletag) (const FileTag *ftag, char *path);
	int			(*sync_unlinkfiletag) (const FileTag *ftag, char *path);
	bool		(*sync_filetagmatches) (const FileTag *ftag,
										const FileTag *candidate);
} SyncOps;

/*
 * 这些下标必须与 SyncRequestHandler 枚举的取值对应。
 */
static const SyncOps syncsw[] = {
	/* 磁盘 */
	[SYNC_HANDLER_MD] = {
		.sync_syncfiletag = mdsyncfiletag,
		.sync_unlinkfiletag = mdunlinkfiletag,
		.sync_filetagmatches = mdfiletagmatches
	},
	/* pg_xact */
	[SYNC_HANDLER_CLOG] = {
		.sync_syncfiletag = clogsyncfiletag
	},
	/* pg_commit_ts */
	[SYNC_HANDLER_COMMIT_TS] = {
		.sync_syncfiletag = committssyncfiletag
	},
	/* pg_multixact/offsets 目录 */
	[SYNC_HANDLER_MULTIXACT_OFFSET] = {
		.sync_syncfiletag = multixactoffsetssyncfiletag
	},
	/* pg_multixact/members 目录 */
	[SYNC_HANDLER_MULTIXACT_MEMBER] = {
		.sync_syncfiletag = multixactmemberssyncfiletag
	}
};

/*
 * 初始化文件同步跟踪所用的数据结构。
 */
void
InitSync(void)
{
	/*
	 * 若需要，就创建待处理操作散列表。目前，仅当我们是独立后端
	 * （不在 postmaster 之下），或我们是检查点进程
	 * 这种辅助进程时，才需要它。
	 */
	if (!IsUnderPostmaster || AmCheckpointerProcess())
	{
		HASHCTL		hash_ctl;

		/*
		 * XXX: 检查点进程吸收 fsync 请求时，需要往待处理操作表
		 * 里添加表项。这件事是在临界区内做的，
		 * 通常并不允许，但这里破例。这意味着
		 * 吸收 fsync 请求时理论上可能内存不足，
		 * 从而导致 PANIC。
		 * 好在散列表很小，所以实践中
		 * 不太可能发生。
		 */
		pendingOpsCxt = AllocSetContextCreate(TopMemoryContext,
											  "Pending ops context",
											  ALLOCSET_DEFAULT_SIZES);
		MemoryContextAllowInCriticalSection(pendingOpsCxt, true);

		hash_ctl.keysize = sizeof(FileTag);
		hash_ctl.entrysize = sizeof(PendingFsyncEntry);
		hash_ctl.hcxt = pendingOpsCxt;
		pendingOps = hash_create("Pending Ops Table",
								 100L,
								 &hash_ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		pendingUnlinks = NIL;
	}
}

/*
 * SyncPreCheckpoint() -- 做检查点之前的工作
 *
 * 为区分本检查点开始之前到达的 unlink 请求
 * 和检查点进行期间到达的那些，我们使用一个周期
 * 计数器，与 fsync 请求所用的类似。该周期
 * 计数器在这里递增。
 *
 * 必须在确定检查点 REDO 点 *之前* 调用。
 * 这样才能保证不会过早删除文件。由于这里会调用
 * AbsorbSyncRequests()，而它会分配内存，因此无法
 * 在临界区内调用。
 *
 * 注意，切勿在这里做任何依赖于如下假定的事情：
 * 这次检查点将会完成。
 */
void
SyncPreCheckpoint(void)
{
	/*
	 * DROP TABLESPACE 之类的操作假定下一次检查点会
	 * 处理最近转发来的全部 unlink 请求，但若在推进周期计数器
	 * 之前没有吸收它们，就要等到以后的某次检查点
	 * 才会处理。下面这次吸收保证：凡是在检查点开始前转发的
	 * unlink 请求，都会在当前这次
	 * 检查点中得到处理。
	 */
	AbsorbSyncRequests();

	/*
	 * 此点之后到达的任何 unlink 请求都会被赋上下一个
	 * 周期计数器，要到下一次检查点才会 unlink。
	 */
	checkpoint_cycle_ctr++;
}

/*
 * SyncPostCheckpoint() -- 做检查点之后的工作
 *
 * 删除现在可以安全删掉的任何残留文件。
 */
void
SyncPostCheckpoint(void)
{
	int			absorb_counter;
	ListCell   *lc;

	absorb_counter = UNLINKS_PER_ABSORB;
	foreach(lc, pendingUnlinks)
	{
		PendingUnlinkEntry *entry = (PendingUnlinkEntry *) lfirst(lc);
		char		path[MAXPGPATH];

		/* 跳过任何已取消的表项 */
		if (entry->canceled)
			continue;

		/*
		 * 新表项追加在末尾，因此若这个表项是新的，就说明
		 * 已经走到了旧表项的尽头。
		 *
		 * 注意：若连续失败的检查点次数刚好，这里可能会被
		 * cycle_ctr 回绕误导。不过唯一的
		 * 后果是 unlink 会再推迟一个检查点，
		 * 这完全可以容忍。
		 */
		if (entry->cycle_ctr == checkpoint_cycle_ctr)
			break;

		/* unlink 该文件 */
		if (syncsw[entry->tag.handler].sync_unlinkfiletag(&entry->tag,
														  path) < 0)
		{
			/*
			 * 这里有竞争：删除数据库的同时，我们正在处理
			 * 待处理的 unlink 请求。若 DROP DATABASE
			 * 比我们更早删掉该文件，这里就会得到 ENOENT。
			 * rmtree() 也必须忽略 ENOENT 错误，以便应对
			 * 我们先删掉该文件的可能。
			 */
			if (errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}

		/* 把该链表表项标为已取消，以防万一 */
		entry->canceled = true;

		/*
		 * 与 ProcessSyncRequests 中一样，有很多删除要做时，我们不想
		 * 长时间停止吸收 fsync 请求。
		 * 在循环的这一点可以安全调用 AbsorbSyncRequests()。
		 */
		if (--absorb_counter <= 0)
		{
			AbsorbSyncRequests();
			absorb_counter = UNLINKS_PER_ABSORB;
		}
	}

	/*
	 * 若已走到链表末尾，就可以直接拆掉整条链表
	 * （记得 pfree 全部 PendingUnlinkEntry 对象）。否则，
	 * 必须保留 "lc" 及其之后的表项。
	 */
	if (lc == NULL)
	{
		list_free_deep(pendingUnlinks);
		pendingUnlinks = NIL;
	}
	else
	{
		int			ntodelete = list_cell_number(pendingUnlinks, lc);

		for (int i = 0; i < ntodelete; i++)
			pfree(list_nth(pendingUnlinks, i));

		pendingUnlinks = list_delete_first_n(pendingUnlinks, ntodelete);
	}
}

/*
 *	ProcessSyncRequests() -- 处理排队的 fsync 请求。
 */
void
ProcessSyncRequests(void)
{
	static bool sync_in_progress = false;

	HASH_SEQ_STATUS hstat;
	PendingFsyncEntry *entry;
	int			absorb_counter;

	/* 同步耗时统计 */
	int			processed = 0;
	instr_time	sync_start,
				sync_end,
				sync_diff;
	uint64		elapsed;
	uint64		longest = 0;
	uint64		total_elapsed = 0;

	/*
	 * 本函数只在检查点期间调用，而检查点只应当发生在
	 * 已经创建了 pendingOps 的进程中。
	 */
	if (!pendingOps)
		elog(ERROR, "cannot sync without a pendingOps table");

	/*
	 * 若我们处于检查点进程中，这次同步最好包含截至此时
	 * 各后端已经排入队列的全部 fsync 请求。窗口最窄的
	 * 竞争是：某个必须为检查点写出并 fsync 的缓冲区，可能刚好在
	 * BufferSync() 访问它之前被某个后端写出。我们知道该后端会在
	 * 清除该缓冲区的脏位之前先排入一条 fsync 请求，因此只要在
	 * 做完 BufferSync() 之后再执行一次 Absorb，
	 * 就是安全的。
	 */
	AbsorbSyncRequests();

	/*
	 * 为避免过量 fsync（最坏情况下，检查点也许永远不会
	 * 终止），我们要忽略在此之后才进入
	 * 散列表的 fsync 请求——它们应当留到下一次
	 * 再处理。我们用 sync_cycle_ctr 把旧表项和新表项
	 * 区分开：新表项的 cycle_ctr 等于递增之后的
	 * sync_cycle_ctr。
	 *
	 * 正常情况下，此时表中已有的全部表项，其
	 * cycle_ctr 都恰好等于 sync_cycle_ctr 当前
	 * （即将变旧）的值。然而，若我们在 fsync
	 * 循环中途失败，等回到这里重试时，可能还留着
	 * 更旧的 cycle_ctr。检查点反复失败
	 * 最终会把计数器回绕到这种程度：旧表项
	 * 看起来像新的，于是我们跳过它，可能让一次
	 * 本不该成功的检查点成功。为防止回绕，每当
	 * 上一次 ProcessSyncRequests() 未能完成时，就遍历
	 * 整张表，并强制令 cycle_ctr = sync_cycle_ctr。
	 *
	 * 不要想把这个循环并进主循环，问题恰恰在于
	 * 那个循环可能在访问完所有表项之前就失败。
	 * 从性能上看这也无关紧要，因为在正常运转的系统里
	 * 根本不会走到这条路径。
	 */
	if (sync_in_progress)
	{
		/* 上次尝试失败，因此更新任何陈旧的 cycle_ctr 值 */
		hash_seq_init(&hstat, pendingOps);
		while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			entry->cycle_ctr = sync_cycle_ctr;
		}
	}

	/* 推进计数器，使新的散列表表项可以区分 */
	sync_cycle_ctr++;

	/* 设置标志，以便在未到达循环末尾时察觉失败 */
	sync_in_progress = true;

	/* 现在扫描散列表，找出要处理的 fsync 请求 */
	absorb_counter = FSYNCS_PER_ABSORB;
	hash_seq_init(&hstat, pendingOps);
	while ((entry = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
	{
		int			failures;

		/*
		 * 若表项是新的，这次就不处理；它是新的。
		 * 注意 "continue" 会绕过循环底部的
		 * hash-remove 调用。
		 */
		if (entry->cycle_ctr == sync_cycle_ctr)
			continue;

		/* 否则断言我们没有漏掉它 */
		Assert((CycleCtr) (entry->cycle_ctr + 1) == sync_cycle_ctr);

		/*
		 * 若关闭了 fsync，就完全不必费心去打开文件。
		 * （把检查推迟到这里，这样在运行期间修改 fsync
		 * 时，行为仍然合理。）
		 */
		if (enableFsync)
		{
			/*
			 * 若在检查点进程中，我们希望时常吸收待处理请求，
			 * 以免 fsync 请求队列溢出。新加入的表项
			 * 会不会被 hash_seq_search 访问到，并无规定，
			 * 但我们并不在意，因为反正
			 * 也不需要处理它们。
			 */
			if (--absorb_counter <= 0)
			{
				AbsorbSyncRequests();
				absorb_counter = FSYNCS_PER_ABSORB;
			}

			/*
			 * fsync 表里可能含有对某些段的 fsync 请求，等我们处理到
			 * 它们时，这些段可能已被删除（unlink）。我们并不是
			 * 仅仅指望 ENOENT（Windows 上则为 EACCES）错误可以
			 * 被忽略；出错时的做法是先吸收待处理请求，然后
			 * 重试。由于 mdunlink() 在真正 unlink 之前会先排队一条
			 * "cancel" 消息，若确实属于这种情况，吸收之后该
			 * fsync 请求保证已被标为 canceled。
			 * DROP DATABASE 同样必须在开始删除之前告诉
			 * 我们忘掉相关的 fsync 请求。
			 */
			for (failures = 0; !entry->canceled; failures++)
			{
				char		path[MAXPGPATH];

				INSTR_TIME_SET_CURRENT(sync_start);
				if (syncsw[entry->tag.handler].sync_syncfiletag(&entry->tag,
																path) == 0)
				{
					/* 成功；更新有关同步耗时的统计 */
					INSTR_TIME_SET_CURRENT(sync_end);
					sync_diff = sync_end;
					INSTR_TIME_SUBTRACT(sync_diff, sync_start);
					elapsed = INSTR_TIME_GET_MICROSEC(sync_diff);
					if (elapsed > longest)
						longest = elapsed;
					total_elapsed += elapsed;
					processed++;

					if (log_checkpoints)
						elog(DEBUG1, "checkpoint sync: number=%d file=%s time=%.3f ms",
							 processed,
							 path,
							 (double) elapsed / 1000);

					break;		/* 跳出重试循环 */
				}

				/*
				 * 自 fsync 请求记入以来，该关系可能已被删除
				 * 或截断。因此允许 ENOENT，但仅当
				 * 我们还没有在这个文件上
				 * 失败过。
				 */
				if (!FILE_POSSIBLY_DELETED(errno) || failures > 0)
					ereport(data_sync_elevel(ERROR),
							(errcode_for_file_access(),
							 errmsg("could not fsync file \"%s\": %m",
									path)));
				else
					ereport(DEBUG1,
							(errcode_for_file_access(),
							 errmsg_internal("could not fsync file \"%s\" but retrying: %m",
											 path)));

				/*
				 * 吸收新到的请求，并检查该关系分叉
				 * 是否已有取消到达。
				 */
				AbsorbSyncRequests();
				absorb_counter = FSYNCS_PER_ABSORB; /* 不妨如此... */
			}					/* 重试循环结束 */
		}

		/* 这个表项已处理完，删掉它 */
		if (hash_search(pendingOps, &entry->tag, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "pendingOps corrupted");
	}							/* 遍历散列表表项的循环结束 */

	/* 返回同步性能指标，供检查点结束时报告 */
	CheckpointStats.ckpt_sync_rels = processed;
	CheckpointStats.ckpt_longest_sync = longest;
	CheckpointStats.ckpt_agg_sync_time = total_elapsed;

	/* 标记 ProcessSyncRequests 成功完成 */
	sync_in_progress = false;
}

/*
 * RememberSyncRequest() -- 检查点进程一侧的同步请求回调
 *
 * 我们把 fsync 请求放进本地散列表，以便在
 * 检查点进程的下一次检查点期间执行。不过 UNLINK 请求进入
 * 另一条链表，因为它们要分开处理。
 *
 * 支持哪些同步请求类型，详见 sync.h。
 */
void
RememberSyncRequest(const FileTag *ftag, SyncRequestType type)
{
	Assert(pendingOps);

	if (type == SYNC_FORGET_REQUEST)
	{
		PendingFsyncEntry *entry;

		/* 取消先前登记的请求 */
		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  ftag,
												  HASH_FIND,
												  NULL);
		if (entry != NULL)
			entry->canceled = true;
	}
	else if (type == SYNC_FILTER_REQUEST)
	{
		HASH_SEQ_STATUS hstat;
		PendingFsyncEntry *pfe;
		ListCell   *cell;

		/* 取消匹配的 fsync 请求 */
		hash_seq_init(&hstat, pendingOps);
		while ((pfe = (PendingFsyncEntry *) hash_seq_search(&hstat)) != NULL)
		{
			if (pfe->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &pfe->tag))
				pfe->canceled = true;
		}

		/* 取消匹配的 unlink 请求 */
		foreach(cell, pendingUnlinks)
		{
			PendingUnlinkEntry *pue = (PendingUnlinkEntry *) lfirst(cell);

			if (pue->tag.handler == ftag->handler &&
				syncsw[ftag->handler].sync_filetagmatches(ftag, &pue->tag))
				pue->canceled = true;
		}
	}
	else if (type == SYNC_UNLINK_REQUEST)
	{
		/* unlink 请求：放入链表 */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingUnlinkEntry *entry;

		entry = palloc(sizeof(PendingUnlinkEntry));
		entry->tag = *ftag;
		entry->cycle_ctr = checkpoint_cycle_ctr;
		entry->canceled = false;

		pendingUnlinks = lappend(pendingUnlinks, entry);

		MemoryContextSwitchTo(oldcxt);
	}
	else
	{
		/* 正常情形：登记一条对该段做 fsync 的请求 */
		MemoryContext oldcxt = MemoryContextSwitchTo(pendingOpsCxt);
		PendingFsyncEntry *entry;
		bool		found;

		Assert(type == SYNC_REQUEST);

		entry = (PendingFsyncEntry *) hash_search(pendingOps,
												  ftag,
												  HASH_ENTER,
												  &found);
		/* 若是新表项，或先前已取消，则初始化它 */
		if (!found || entry->canceled)
		{
			entry->cycle_ctr = sync_cycle_ctr;
			entry->canceled = false;
		}

		/*
		 * NB: 若表项已存在就不改 cycle_ctr，这是有意为之。
		 * cycle_ctr 必须代表该表项中可能存在的最早那次
		 * fsync 请求。
		 */

		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * 在本地登记同步请求，或者转发给检查点进程。
 *
 * 若 retryOnError 为 true，队列没有空位时就继续尝试。
 * 成功则返回 true；没有空位则返回 false。
 */
bool
RegisterSyncRequest(const FileTag *ftag, SyncRequestType type,
					bool retryOnError)
{
	bool		ret;

	if (pendingOps != NULL)
	{
		/* 独立后端或启动进程：fsync 状态在本地 */
		RememberSyncRequest(ftag, type);
		return true;
	}

	for (;;)
	{
		/*
		 * 把这件事通知检查点进程。若在 retryOnError 模式下未能把消息
		 * 排入队列，就得睡眠后重试...很难看，但
		 * 希望不会经常发生。
		 *
		 * XXX 这个循环里是否应当 CHECK_FOR_INTERRUPTS？若在
		 * SYNC_UNLINK_REQUEST 时带着错误逃出，会让不再使用的
		 * 文件仍然留在磁盘上，那就糟了，因此
		 * 我倾向于假定检查点进程总会很快把队列
		 * 排空。
		 */
		ret = ForwardSyncRequest(ftag, type);

		/*
		 * 若请求已成功排入队列，或者失败且被告知
		 * 出错时不要重试，就 break。
		 */
		if (ret || (!ret && !retryOnError))
			break;

		WaitLatch(NULL, WL_EXIT_ON_PM_DEATH | WL_TIMEOUT, 10,
				  WAIT_EVENT_REGISTER_SYNC_REQUEST);
	}

	return ret;
}
