/*-------------------------------------------------------------------------
 *
 * xlogutils.c
 *
 * PostgreSQL 预写式日志管理器实用例程
 *
 * 本文件包含供 XLOG 重放函数使用的支持例程。
 * 正常系统运行期间不会用到这里的任何代码。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/timeline.h"
#include "access/xlogrecovery.h"
#include "access/xlog_internal.h"
#include "access/xlogprefetcher.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/rel.h"


/* GUC 变量 */
bool		ignore_invalid_pages = false;

/*
 * 我们是否正在从 XLOG 做恢复？
 *
 * 该标志仅在启动进程中才会为真；应理解为
 * "本进程正在重放 WAL 记录"，而非"系统处于
 * 恢复模式"。主要应由那些在从 WAL redo 函数调用时
 * 需要表现不同的函数检查（例如跳过 WAL
 * 日志记录）。若要不论运行在哪个进程中都检查系统
 * 是否处于恢复，应使用 RecoveryInProgress()，但仅在共享
 * 内存启动与锁初始化之后。
 *
 * 该值由 xlog.c 与 xlogrecovery.c 更新，放在此处是因为
 * 它大多由 WAL redo 函数读取。
 */
bool		InRecovery = false;

/* 我们是否处于热备模式？仅在启动进程中有效，见 xlogutils.h */
HotStandbyState standbyState = STANDBY_DISABLED;

/*
 * 在 XLOG 重放期间，我们可能看到针对已不存在页面的
 * 增量更新的 XLOG 记录，因为其关系随后被删除或
 * 截断了。（注意：这只在 full_page_writes = OFF 时
 * 可能发生；当其为 ON 时，我们看到的对某页的首次引用
 * 应始终是整页重写而非增量更新。）与其简单
 * 忽略这类记录，我们记下被引用的页面，然后若在
 * 后续重放中实际未看到覆盖该页的删除或截断，
 * 再发出抱怨。
 */
typedef struct xl_invalid_page_key
{
	RelFileLocator locator;		/* 该关系 */
	ForkNumber	forkno;			/* 分支编号 */
	BlockNumber blkno;			/* 该页 */
} xl_invalid_page_key;

typedef struct xl_invalid_page
{
	xl_invalid_page_key key;	/* 哈希键……必须放在首位 */
	bool		present;		/* 页面存在但内容全为零 */
} xl_invalid_page;

static HTAB *invalid_page_tab = NULL;

static int	read_local_xlog_page_guts(XLogReaderState *state, XLogRecPtr targetPagePtr,
									  int reqLen, XLogRecPtr targetRecPtr,
									  char *cur_page, bool wait_for_wal);

/* 报告对无效页面的引用 */
static void
report_invalid_page(int elevel, RelFileLocator locator, ForkNumber forkno,
					BlockNumber blkno, bool present)
{
	char	   *path = relpathperm(locator, forkno);

	if (present)
		elog(elevel, "page %u of relation %s is uninitialized",
			 blkno, path);
	else
		elog(elevel, "page %u of relation %s does not exist",
			 blkno, path);
	pfree(path);
}

/* 记录对无效页面的引用 */
static void
log_invalid_page(RelFileLocator locator, ForkNumber forkno, BlockNumber blkno,
				 bool present)
{
	xl_invalid_page_key key;
	xl_invalid_page *hentry;
	bool		found;

	/*
	 * 一旦恢复已达到一致状态，无效页面表
	 * 应为空并保持为空。若在达到一致性之后
	 * 仍发现对无效页面的引用，立即 PANIC。这看似
	 * 激进，但总好过让无效引用滞留在
	 * 哈希表中直到恢复结束再 PANIC，而那
	 * 可能要到很久以后——若这是备库服务器。
	 */
	if (reachedConsistency)
	{
		report_invalid_page(WARNING, locator, forkno, blkno, present);
		elog(ignore_invalid_pages ? WARNING : PANIC,
			 "WAL contains references to invalid pages");
	}

	/*
	 * 以 DEBUG1 级别记录对无效页面的引用。这便于对
	 * 原因做一些追踪（注意 elog 上下文机制会告诉我们
	 * 一些关于产生该引用的 XLOG 记录的信息）。
	 */
	if (message_level_is_interesting(DEBUG1))
		report_invalid_page(DEBUG1, locator, forkno, blkno, present);

	if (invalid_page_tab == NULL)
	{
		/* 首次需要时创建哈希表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(xl_invalid_page_key);
		ctl.entrysize = sizeof(xl_invalid_page);

		invalid_page_tab = hash_create("XLOG invalid-page table",
									   100,
									   &ctl,
									   HASH_ELEM | HASH_BLOBS);
	}

	/* 目前假定 xl_invalid_page_key 不含填充字节 */
	key.locator = locator;
	key.forkno = forkno;
	key.blkno = blkno;
	hentry = (xl_invalid_page *)
		hash_search(invalid_page_tab, &key, HASH_ENTER, &found);

	if (!found)
	{
		/* hash_search 已填入键 */
		hentry->present = present;
	}
	else
	{
		/* 重复引用……保持 "present" 原样 */
	}
}

/* 忘记任何 >= minblkno 的无效页面，因为它们已被删除 */
static void
forget_invalid_pages(RelFileLocator locator, ForkNumber forkno,
					 BlockNumber minblkno)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;

	if (invalid_page_tab == NULL)
		return;					/* 无事可做 */

	hash_seq_init(&status, invalid_page_tab);

	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (RelFileLocatorEquals(hentry->key.locator, locator) &&
			hentry->key.forkno == forkno &&
			hentry->key.blkno >= minblkno)
		{
			if (message_level_is_interesting(DEBUG2))
			{
				char	   *path = relpathperm(hentry->key.locator, forkno);

				elog(DEBUG2, "page %u of relation %s has been dropped",
					 hentry->key.blkno, path);
				pfree(path);
			}

			if (hash_search(invalid_page_tab,
							&hentry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

/* 忘记整个数据库中的任何无效页面 */
static void
forget_invalid_pages_db(Oid dbid)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;

	if (invalid_page_tab == NULL)
		return;					/* 无事可做 */

	hash_seq_init(&status, invalid_page_tab);

	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (hentry->key.locator.dbOid == dbid)
		{
			if (message_level_is_interesting(DEBUG2))
			{
				char	   *path = relpathperm(hentry->key.locator, hentry->key.forkno);

				elog(DEBUG2, "page %u of relation %s has been dropped",
					 hentry->key.blkno, path);
				pfree(path);
			}

			if (hash_search(invalid_page_tab,
							&hentry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

/* 是否还有未解决的无效页面引用？ */
bool
XLogHaveInvalidPages(void)
{
	if (invalid_page_tab != NULL &&
		hash_get_num_entries(invalid_page_tab) > 0)
		return true;
	return false;
}

/* 对仍残留的无效页面条目发出抱怨 */
void
XLogCheckInvalidPages(void)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;
	bool		foundone = false;

	if (invalid_page_tab == NULL)
		return;					/* 无事可做 */

	hash_seq_init(&status, invalid_page_tab);

	/*
	 * 我们的策略是先为所有残留条目发出 WARNING 消息，
	 * 等把所有可用信息都转储完后再 PANIC。
	 */
	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		report_invalid_page(WARNING, hentry->key.locator, hentry->key.forkno,
							hentry->key.blkno, hentry->present);
		foundone = true;
	}

	if (foundone)
		elog(ignore_invalid_pages ? WARNING : PANIC,
			 "WAL contains references to invalid pages");

	hash_destroy(invalid_page_tab);
	invalid_page_tab = NULL;
}


/*
 * XLogReadBufferForRedo
 *		在 XLOG 重放期间读取一页
 *
 * 将 WAL 记录引用的块读入共享缓冲池，并
 * 判定需要对它重做哪些更改。若该 WAL
 * 记录包含该页的整页映像，则将其恢复。
 *
 * 将 'record.EndRecPtr' 与页面的 LSN 比较，以判定该记录
 * 是否已被重放过。'block_id' 是创建 WAL 记录时
 * 为该块注册的 ID 编号。
 *
 * 返回值之一如下：
 *
 *	BLK_NEEDS_REDO	- 需要应用来自该 WAL 记录的更改
 *	BLK_DONE		- 该块无需重放
 *	BLK_RESTORED	- 该块已从记录中附带的整页映像
 *					  恢复
 *	BLK_NOTFOUND	- 未找到该块（因为它被 WAL 流中
 *					  更晚的操作截断掉了）
 *
 * 返回时，缓冲区以排他模式锁定，并通过 *buf 返回。
 * 注意即使该块无需重放，也会锁定并返回
 * 缓冲区。（在单进程崩溃恢复期间获取缓冲区锁
 * 并非真正必要，但某些子例程如 MarkBufferDirty
 * 在我们未持锁时会抱怨。在热备模式下则
 * 肯定必要。）
 *
 * 注意：当 XLOG 中有带 BKPIMAGE_APPLY 标志的
 * 备份块可用时，我们会恢复它，即使数据库中的页面
 * 看起来更新。这是为了保护我们免受崩溃期间被部分或
 * 错误写入的数据库页面影响。我们假定 XLOG 数据一定
 * 是好的，因为它已通过 CRC 校验，而数据库页面可能
 * 并非如此。这会迫使我们重放 XLOG 中出现的对该页的所有
 * 后续修改，而不能可能地将它们当作已应用而
 * 忽略，但那不是很大的缺点。
 */
XLogRedoAction
XLogReadBufferForRedo(XLogReaderState *record, uint8 block_id,
					  Buffer *buf)
{
	return XLogReadBufferForRedoExtended(record, block_id, RBM_NORMAL,
										 false, buf);
}

/*
 * 固定并锁定 WAL 记录引用的缓冲区，以便
 * 重新初始化它。
 */
Buffer
XLogInitBufferForRedo(XLogReaderState *record, uint8 block_id)
{
	Buffer		buf;

	XLogReadBufferForRedoExtended(record, block_id, RBM_ZERO_AND_LOCK, false,
								  &buf);
	return buf;
}

/*
 * XLogReadBufferForRedoExtended
 *		类似 XLogReadBufferForRedo，但有额外选项。
 *
 * 在 RBM_ZERO_* 模式下，若页面不存在，则将关系
 * 用全零页扩展到被引用的块号。在
 * RBM_ZERO_AND_LOCK 与 RBM_ZERO_AND_CLEANUP_LOCK 模式下，返回值
 * 始终为 BLK_NEEDS_REDO。
 *
 * （RBM_ZERO_AND_CLEANUP_LOCK 模式与 get_cleanup_lock
 * 参数冗余。不要使用不一致的组合！）
 *
 * 若 'get_cleanup_lock' 为真，则对缓冲区获取"清理锁"，
 * 使用 LockBufferForCleanup()，而非普通排他锁。
 */
XLogRedoAction
XLogReadBufferForRedoExtended(XLogReaderState *record,
							  uint8 block_id,
							  ReadBufferMode mode, bool get_cleanup_lock,
							  Buffer *buf)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	Buffer		prefetch_buffer;
	Page		page;
	bool		zeromode;
	bool		willinit;

	if (!XLogRecGetBlockTagExtended(record, block_id, &rlocator, &forknum, &blkno,
									&prefetch_buffer))
	{
		/* 调用者指定了无效的 block_id */
		elog(PANIC, "failed to locate backup block with ID %d in WAL record",
			 block_id);
	}

	/*
	 * 确保若该块标有 WILL_INIT，则调用者将
	 * 初始化它。反之亦然。
	 */
	zeromode = (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK);
	willinit = (XLogRecGetBlock(record, block_id)->flags & BKPBLOCK_WILL_INIT) != 0;
	if (willinit && !zeromode)
		elog(PANIC, "block with WILL_INIT flag in WAL record must be zeroed by redo routine");
	if (!willinit && zeromode)
		elog(PANIC, "block to be initialized in redo routine must be marked with WILL_INIT flag in the WAL record");

	/* 若有整页映像且应被恢复，则恢复它。 */
	if (XLogRecBlockImageApply(record, block_id))
	{
		Assert(XLogRecHasBlockImage(record, block_id));
		*buf = XLogReadBufferExtended(rlocator, forknum, blkno,
									  get_cleanup_lock ? RBM_ZERO_AND_CLEANUP_LOCK : RBM_ZERO_AND_LOCK,
									  prefetch_buffer);
		page = BufferGetPage(*buf);
		if (!RestoreBlockImage(record, block_id, page))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg_internal("%s", record->errormsg_buf)));

		/*
		 * 该页可能尚未初始化。若是，则不能设置 LSN，因为
		 * 那会损坏该页。
		 */
		if (!PageIsNew(page))
		{
			PageSetLSN(page, lsn);
		}

		MarkBufferDirty(*buf);

		/*
		 * 在崩溃恢复结束时，未记录关系的 init 分支
		 * 会被复制，而不经过共享缓冲区。因此我们需要
		 * 强制 init 分支的磁盘状态始终与共享缓冲区中的
		 * 状态同步。
		 */
		if (forknum == INIT_FORKNUM)
			FlushOneBuffer(*buf);

		return BLK_RESTORED;
	}
	else
	{
		*buf = XLogReadBufferExtended(rlocator, forknum, blkno, mode, prefetch_buffer);
		if (BufferIsValid(*buf))
		{
			if (mode != RBM_ZERO_AND_LOCK && mode != RBM_ZERO_AND_CLEANUP_LOCK)
			{
				if (get_cleanup_lock)
					LockBufferForCleanup(*buf);
				else
					LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);
			}
			if (lsn <= PageGetLSN(BufferGetPage(*buf)))
				return BLK_DONE;
			else
				return BLK_NEEDS_REDO;
		}
		else
			return BLK_NOTFOUND;
	}
}

/*
 * XLogReadBufferExtended
 *		在 XLOG 重放期间读取一页
 *
 * 功能上可与 ReadBufferExtended 相比。对 "mode" 参数的
 * 行为有一些差异：
 *
 * 在 RBM_NORMAL 模式下，若页面不存在，或内容全为零，我们
 * 返回 InvalidBuffer。此时调用者应静默跳过对该页的
 * 更新。（在此情况下，我们预期该页随后被
 * 删除或截断。若在 WAL 序列更晚处看不到
 * 证据，我们将在 WAL 重放结束时抱怨。）
 *
 * 在 RBM_ZERO_* 模式下，若页面不存在，则将关系
 * 用全零页扩展到给定块号。
 *
 * 在 RBM_NORMAL_NO_LOG 模式下，若页面不存在则返回
 * InvalidBuffer，且不检查是否全为零。因此不会留下日志条目
 * 暗示该页随后应被删除或截断。
 *
 * 可选地，可用 recent_buffer 提示页面在缓冲池中的
 * 位置；它不必正确，但若正确则可避免
 * 一次缓冲区映射表探测。
 *
 * NB: redo 函数通常不应直接调用此函数。要获取待
 * 修改的页面，应改用 XLogReadBufferForRedoExtended。重要的是
 * WAL 记录修改的所有页面都要在 WAL 记录中注册，否则
 * 它们对需要知道哪些页面被修改的工具将不可见。
 */
Buffer
XLogReadBufferExtended(RelFileLocator rlocator, ForkNumber forknum,
					   BlockNumber blkno, ReadBufferMode mode,
					   Buffer recent_buffer)
{
	BlockNumber lastblock;
	Buffer		buffer;
	SMgrRelation smgr;

	Assert(blkno != P_NEW);

	/* 我们是否已有关于缓冲区可能位置的线索？ */
	if (BufferIsValid(recent_buffer) &&
		mode == RBM_NORMAL &&
		ReadRecentBuffer(rlocator, forknum, blkno, recent_buffer))
	{
		buffer = recent_buffer;
		goto recent_buffer_fast_path;
	}

	/* 在 smgr 层打开该关系 */
	smgr = smgropen(rlocator, InvalidBackendId);

	/*
	 * 若目标文件尚不存在则创建它。这让我们能应对
	 * 重放序列中包含对稍后被删除的关系的写入
	 * 的情况。（本例程原先的编码会改为抑制
	 * 这些写入，但那样若文件系统在崩溃期间丢失
	 * 一个 inode，就有丢失宝贵数据的风险。更好是写入数据
	 * 直到我们实际被告知删除该文件。）
	 */
	smgrcreate(smgr, forknum, true);

	lastblock = smgrnblocks(smgr, forknum);

	if (blkno < lastblock)
	{
		/* 页面在文件中存在 */
		buffer = ReadBufferWithoutRelcache(rlocator, forknum, blkno,
										   mode, NULL, true);
	}
	else
	{
		/* 嗯，页面在文件中不存在 */
		if (mode == RBM_NORMAL)
		{
			log_invalid_page(rlocator, forknum, blkno, false);
			return InvalidBuffer;
		}
		if (mode == RBM_NORMAL_NO_LOG)
			return InvalidBuffer;
		/* 可以扩展该文件 */
		/* 我们仅在恢复中这样做——无需关系扩展锁 */
		Assert(InRecovery);
		buffer = ExtendBufferedRelTo(BMR_SMGR(smgr, RELPERSISTENCE_PERMANENT),
									 forknum,
									 NULL,
									 EB_PERFORMING_RECOVERY |
									 EB_SKIP_EXTENSION_LOCK,
									 blkno + 1,
									 mode);
	}

recent_buffer_fast_path:
	if (mode == RBM_NORMAL)
	{
		/* 检查页面是否已初始化 */
		Page		page = (Page) BufferGetPage(buffer);

		/*
		 * 我们假定 PageIsNew 无需加锁即安全。在恢复期间，
		 * 不应有其他后端能同时
		 * 修改该缓冲区。
		 */
		if (PageIsNew(page))
		{
			ReleaseBuffer(buffer);
			log_invalid_page(rlocator, forknum, blkno, true);
			return InvalidBuffer;
		}
	}

	return buffer;
}

/*
 * CreateFakeRelcacheEntry 实际返回的结构，尽管声明的
 * 返回类型是 Relation。
 */
typedef struct
{
	RelationData reldata;		/* 注意：此项必须放在首位 */
	FormData_pg_class pgc;
} FakeRelCacheEntryData;

typedef FakeRelCacheEntryData *FakeRelCacheEntry;

/*
 * 为物理关系创建假的关系缓存条目
 *
 * 在 XLOG 重放中使用与主代码路径相同的函数往往很
 * 方便，但那些函数通常使用 relcache 条目。
 * XLOG 重放期间我们没有可用的关系缓存，但可用本
 * 函数改为创建假的 relcache 条目。仅初始化与
 * 物理存储相关的字段，如 rd_rel，因此该
 * 假条目仅可用于 ReadBuffer() 这类底层操作。
 *
 * 这也用于同步跳过 WAL 的文件。
 *
 * 调用者必须用 FreeFakeRelcacheEntry() 释放返回的条目。
 */
Relation
CreateFakeRelcacheEntry(RelFileLocator rlocator)
{
	FakeRelCacheEntry fakeentry;
	Relation	rel;

	/* 在一块中分配 Relation 结构及所有相关空间。 */
	fakeentry = palloc0(sizeof(FakeRelCacheEntryData));
	rel = (Relation) fakeentry;

	rel->rd_rel = &fakeentry->pgc;
	rel->rd_locator = rlocator;

	/*
	 * 在恢复期间或同步跳过 WAL 的文件时，我们绝不会
	 * 处理临时关系。
	 */
	rel->rd_backend = InvalidBackendId;

	/* 此处必须是永久表 */
	rel->rd_rel->relpersistence = RELPERSISTENCE_PERMANENT;

	/* 我们不知道关系名；改用 relfilenumber */
	sprintf(RelationGetRelationName(rel), "%u", rlocator.relNumber);

	/*
	 * 我们设置 lockRelId，以防有东西试图锁定该哑元
	 * 关系。注意这相当牵强，因为 relNumber 可能
	 * 不同于关系的 OID。不过这其实不应要紧。
	 * 在恢复中我们独自运行，不会有任何锁
	 * 冲突。在同步时，我们已持有 AccessExclusiveLock。
	 */
	rel->rd_lockInfo.lockRelId.dbId = rlocator.dbOid;
	rel->rd_lockInfo.lockRelId.relId = rlocator.relNumber;

	rel->rd_smgr = NULL;

	return rel;
}

/*
 * 释放假的关系缓存条目。
 */
void
FreeFakeRelcacheEntry(Relation fakerel)
{
	/* 确保 fakerel 不再被 SmgrRelation 引用 */
	if (fakerel->rd_smgr != NULL)
		smgrclearowner(&fakerel->rd_smgr, fakerel->rd_smgr);
	pfree(fakerel);
}

/*
 * 在 XLOG 重放期间删除关系
 *
 * 在关系即将被删除时调用；我们需要移除
 * 该关系上任何未决的"无效页面"记录。
 */
void
XLogDropRelation(RelFileLocator rlocator, ForkNumber forknum)
{
	forget_invalid_pages(rlocator, forknum, 0);
}

/*
 * 在 XLOG 重放期间删除整个数据库
 *
 * 同上，但是针对 DROP DATABASE，而非删除单个关系
 */
void
XLogDropDatabase(Oid dbid)
{
	/*
	 * 这过于粗暴，因为它也会关闭其他数据库的
	 * SMgrRelation 对象。DROP DATABASE 足够少见，
	 * 不值得仅为这一用途引入 smgrclose 变体。
	 * XXX: 或许我们更应让 smgr 条目悬空？
	 */
	smgrcloseall();

	forget_invalid_pages_db(dbid);
}

/*
 * 在 XLOG 重放期间截断关系
 *
 * 我们需要清理被丢弃页面上任何未决的"无效页面"记录。
 */
void
XLogTruncateRelation(RelFileLocator rlocator, ForkNumber forkNum,
					 BlockNumber nblocks)
{
	forget_invalid_pages(rlocator, forkNum, nblocks);
}

/*
 * 判定应从哪条时间线读取 xlog 页，并将
 * XLogReaderState 的 currTLI 设为该时间线 ID。
 *
 * 当我们可能在读取提升之前生成的 xlog 时，
 * xlogreader 会关心时间线——无论我们当前是处于
 * 恢复中的备库，还是已提升的主库正在读取提升前旧
 * 主库生成的 xlog。
 *
 * wantPage 必须设为待读页的起始地址，
 * wantLength 为将要读取的页内字节数，最多
 * XLOG_BLCKSZ。若未知待读量，则传入 XLOG_BLCKSZ。
 *
 * currTLI 参数应为系统范围的当前时间线。
 * 注意这可能不同于 state->currTLI，后者是调用者当前
 * 正在读取先前 xlog 记录所用的时间线。
 *
 * 当我们处于历史时间线上时，一旦到达包含时间线
 * 切换的 xlog 段的起始处，就急切地切换到新时间线的
 * xlog 段。服务器已将该段复制到新
 * 时间线，因此直到切换点的所有数据相同，但不
 * 保证旧段仍存在。它可能已被删除，或
 * 被加上 .partial 后缀重命名，因此即使 tliSwitchPoint
 * 说可以，我们也不一定能继续从旧 TLI 读取。
 *
 * 我们不能只在读取与上一页不同段上的页时
 * 才检查时间线。我们可能已从级联
 * 上游收到时间线切换，因此当前段突然结束
 * （可能被重命名为 .partial），我们必须切换到新段。
 * 甚至在读取一页的过程中也可能不得不丢弃缓存页并切换到新 TLI。
 *
 * 因此，调用者不可假定 currTLI 就是页的
 * xlp_tli 中的时间线；该页可能始于更旧的时间线，或者我们
 * 可能正在从已被复制到新时间线的段上的
 * 历史时间线数据读取。
 *
 * 若在恢复中执行，调用者还必须确保不读过当前重放
 * 位置（使用 GetXLogReplayRecPtr），以免
 * 未能注意到当前时间线已变为历史时间线。
 */
void
XLogReadDetermineTimeline(XLogReaderState *state, XLogRecPtr wantPage,
						  uint32 wantLength, TimeLineID currTLI)
{
	const XLogRecPtr lastReadPage = (state->seg.ws_segno *
									 state->segcxt.ws_segsize + state->segoff);

	Assert(wantPage != InvalidXLogRecPtr && wantPage % XLOG_BLCKSZ == 0);
	Assert(wantLength <= XLOG_BLCKSZ);
	Assert(state->readLen == 0 || state->readLen <= XLOG_BLCKSZ);
	Assert(currTLI != 0);

	/*
	 * 若所需页面当前已读入且有效，则无事可
	 * 做。
	 *
	 * 调用者应已确保先前未将 readOff
	 * 推进超过本时间线的有效上限，因此即使当前
	 * TLI 此后已变为历史时间线也无关紧要。
	 */
	if (lastReadPage == wantPage &&
		state->readLen != 0 &&
		lastReadPage + state->readLen >= wantPage + Min(wantLength, XLOG_BLCKSZ - 1))
		return;

	/*
	 * 若我们正在从当前时间线读取，它尚未变为历史
	 * 时间线，且我们正在读取的页在上次读取的页之后，则可再次
	 * 继续。（向后定位需要检查以确保
	 * 更旧的页不在先前的时间线上）。
	 *
	 * 自调用者取得该值以来 currTLI 可能已变为历史
	 * 时间线，但需要调用者不超过其查找时间线时
	 * 所见的刷盘上限。若 StartupXLOG() 并发地将其
	 * 重命名为 .partial，我们也无能为力。
	 */
	if (state->currTLI == currTLI && wantPage >= lastReadPage)
	{
		Assert(state->currTLIValidUntil == InvalidXLogRecPtr);
		return;
	}

	/*
	 * 若我们只是从先前已验证的历史
	 * 时间线读取页面，且我们正在读取的时间线在当前段
	 * 结束前都有效，则可继续读取。
	 */
	if (state->currTLIValidUntil != InvalidXLogRecPtr &&
		state->currTLI != currTLI &&
		state->currTLI != 0 &&
		((wantPage + wantLength) / state->segcxt.ws_segsize) <
		(state->currTLIValidUntil / state->segcxt.ws_segsize))
		return;

	/*
	 * 若到达此处，我们要么在为随机访问查找一页，
	 * 要么当前时间线刚变为历史时间线，要么正在从
	 * 包含时间线切换的新段读取。所有情况下我们都需要
	 * 判定该段上的最新时间线。
	 *
	 * 若它是当前时间线，我们可以继续从这里读取，除非
	 * 我们检测到使当前时间线变为历史的时间线切换。
	 * 若它是历史时间线，我们可以在最新时间线上读取
	 * 整个段，因为它也包含所有旧时间线的数据。因此只需
	 * 一次切换检查。
	 */
	{
		/*
		 * 我们需要重新读取时间线历史，以防它因提升或
		 * 从级联副本重放而已更改。
		 */
		List	   *timelineHistory = readTimeLineHistory(currTLI);
		XLogRecPtr	endOfSegment;

		endOfSegment = ((wantPage / state->segcxt.ws_segsize) + 1) *
			state->segcxt.ws_segsize - 1;
		Assert(wantPage / state->segcxt.ws_segsize ==
			   endOfSegment / state->segcxt.ws_segsize);

		/*
		 * 查找包含 wantPage 的段上最后一个 LSN 的
		 * 时间线。
		 */
		state->currTLI = tliOfPointInHistory(endOfSegment, timelineHistory);
		state->currTLIValidUntil = tliSwitchPoint(state->currTLI, timelineHistory,
												  &state->nextTLI);

		Assert(state->currTLIValidUntil == InvalidXLogRecPtr ||
			   wantPage + wantLength < state->currTLIValidUntil);

		list_free_deep(timelineHistory);

		elog(DEBUG3, "switched to timeline %u valid until %X/%X",
			 state->currTLI,
			 LSN_FORMAT_ARGS(state->currTLIValidUntil));
	}
}

/* 用于本地 pg_wal 文件的 XLogReaderRoutine->segment_open 回调 */
void
wal_segment_open(XLogReaderState *state, XLogSegNo nextSegNo,
				 TimeLineID *tli_p)
{
	TimeLineID	tli = *tli_p;
	char		path[MAXPGPATH];

	XLogFilePath(path, tli, nextSegNo, state->segcxt.ws_segsize);
	state->seg.ws_file = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (state->seg.ws_file >= 0)
		return;

	if (errno == ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						path)));
	else
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						path)));
}

/* 现成的 XLogReaderRoutine->segment_close 回调 */
void
wal_segment_close(XLogReaderState *state)
{
	close(state->seg.ws_file);
	/* 需要检查 errno 吗？ */
	state->seg.ws_file = -1;
}

/*
 * 用于读取本地 xlog 文件的 XLogReaderRoutine->page_read 回调
 *
 * 设为公开是因为它很可能对在 walsender
 * 之外编写另一种输出方法的人很有帮助，例如在 bgworker 中。
 *
 * TODO: walsender 有自己的版本，但它依赖
 * 每当 WAL 刷盘时设置 walsender 的锁存器。普通后端没有这类
 * 基础设施，因此目前我们不得不采用检查/睡眠/重复式的
 * 循环。
 */
int
read_local_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr,
					 int reqLen, XLogRecPtr targetRecPtr, char *cur_page)
{
	return read_local_xlog_page_guts(state, targetPagePtr, reqLen,
									 targetRecPtr, cur_page, true);
}

/*
 * 与 read_local_xlog_page 相同，但不等待未来的 WAL
 * 变为可用。
 */
int
read_local_xlog_page_no_wait(XLogReaderState *state, XLogRecPtr targetPagePtr,
							 int reqLen, XLogRecPtr targetRecPtr,
							 char *cur_page)
{
	return read_local_xlog_page_guts(state, targetPagePtr, reqLen,
									 targetRecPtr, cur_page, false);
}

/*
 * read_local_xlog_page 及其无等待版本的实现。
 */
static int
read_local_xlog_page_guts(XLogReaderState *state, XLogRecPtr targetPagePtr,
						  int reqLen, XLogRecPtr targetRecPtr,
						  char *cur_page, bool wait_for_wal)
{
	XLogRecPtr	read_upto,
				loc;
	TimeLineID	tli;
	int			count;
	WALReadError errinfo;
	TimeLineID	currTLI;

	loc = targetPagePtr + reqLen;

	/* 必要时循环等待 xlog 可用 */
	while (1)
	{
		/*
		 * 判定我们当前可读到的 xlog 上限，以及
		 * 最近的时间线是什么。
		 */
		if (!RecoveryInProgress())
			read_upto = GetFlushRecPtr(&currTLI);
		else
			read_upto = GetXLogReplayRecPtr(&currTLI);
		tli = currTLI;

		/*
		 * 检查应从哪条时间线获取该记录。
		 *
		 * 每次循环都必须做，因为若我们作为级联
		 * 备库处于恢复中，当前时间线可能已
		 * 变为历史时间线。我们不能依赖 RecoveryInProgress()，因为在
		 * 像这样的备库配置中
		 *
		 * A => B => C
		 *
		 * 若我们在 C 上做逻辑解码会话，而 B 被提升，我们的
		 * 时间线会改变，同时我们仍处于恢复中。
		 *
		 * 我们不能只继续从旧时间线读取，因为该时间线
		 * 中的最后一个 WAL 归档会被 StartupXLOG()
		 * 重命名为 .partial。
		 *
		 * 若那发生在调用者判定了 TLI 之后、我们
		 * 实际读取 xlog 页之前，我们仍可能尝试从
		 * 旧的（现已重命名的）段读取并失败。对此我们
		 * 无能为力，但它只会发生在我们是级联
		 * 备库的叶子、且其主库在我们解码期间被提升时，因此
		 * 一次性的 ERROR 还不算太糟。
		 */
		XLogReadDetermineTimeline(state, targetPagePtr, reqLen, tli);

		if (state->currTLI == currTLI)
		{

			if (loc <= read_upto)
				break;

			/* 若被要求，则不要等待未来的 WAL。 */
			if (!wait_for_wal)
			{
				ReadLocalXLogPageNoWaitPrivate *private_data;

				/*
				 * 通知 read_local_xlog_page_no_wait 的调用者
				 * 已到达 WAL 末尾。
				 */
				private_data = (ReadLocalXLogPageNoWaitPrivate *)
					state->private_data;
				private_data->end_of_wal = true;
				break;
			}

			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000L);
		}
		else
		{
			/*
			 * 我们处于历史时间线上，因此将读取限制到我们
			 * 移到下一条时间线的切换点。
			 *
			 * 我们不需要 GetFlushRecPtr 或 GetXLogReplayRecPtr。我们知道
			 * 新时间线，因此我们一定已接收过其
			 * 末尾。
			 */
			read_upto = state->currTLIValidUntil;

			/*
			 * 将 tli 设为我们想要的记录的 TLI 略有不妥；该
			 * 页若包含时间线切换，可能始于更旧的时间线，
			 * 因为它的 xlog 段是从先前
			 * 时间线复制来的。不过这相当无害，因为只要
			 * 时间线不倒退就无人在意。我们应
			 * 改为读取页头；FIXME 有朝一日。
			 */
			tli = state->currTLI;

			/* 在历史时间线上无需等待 */
			break;
		}
	}

	if (targetPagePtr + XLOG_BLCKSZ <= read_upto)
	{
		/*
		 * 有不止一个块可用；只读该块，若调用者
		 * 还需要更多再回来。
		 */
		count = XLOG_BLCKSZ;
	}
	else if (targetPagePtr + reqLen > read_upto)
	{
		/* 那里没有足够的数据 */
		return -1;
	}
	else
	{
		/* 有足够字节可满足请求 */
		count = read_upto - targetPagePtr;
	}

	/*
	 * 尽管我们刚把该页可有效读取的量判定为
	 * 'count'，仍读取整页。若页不完整，保证会
	 * 零填充到页边界。
	 */
	if (!WALRead(state, cur_page, targetPagePtr, XLOG_BLCKSZ, tli,
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/* 缓冲区中的有效字节数 */
	return count;
}

/*
 * 供后端专用的便利代码，用于处理 WALRead()
 * 遇到的读取错误。
 */
void
WALReadRaiseError(WALReadError *errinfo)
{
	WALOpenSegment *seg = &errinfo->wre_seg;
	char		fname[MAXFNAMELEN];

	XLogFileName(fname, seg->ws_tli, seg->ws_segno, wal_segment_size);

	if (errinfo->wre_read < 0)
	{
		errno = errinfo->wre_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from WAL segment %s, offset %d: %m",
						fname, errinfo->wre_off)));
	}
	else if (errinfo->wre_read == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read from WAL segment %s, offset %d: read %d of %d",
						fname, errinfo->wre_off, errinfo->wre_read,
						errinfo->wre_req)));
	}
}
