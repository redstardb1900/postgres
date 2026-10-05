/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  存储管理器开关的公共接口例程。
 *
 *	  POSTGRES 里的全部文件系统操作都经由这些
 *	  例程分派。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"


/*
 * 这个函数指针结构定义了 smgr.c 与
 * 任何一个具体存储管理器模块之间的 API。注意，smgr 子函数
 * 通常应当经由 elog(ERROR) 报告问题。例外是
 * smgr_unlink 应当使用 elog(WARNING)，而不是报错退出，
 * 因为我们通常在提交后/中止后的清理阶段 unlink 关系，
 * 那时再抛出错误已经太晚。另外，有些通常算错误的情况
 * 在 bootstrap 和/或 WAL
 * 恢复期间应当被允许——详见 md.c 里的注释。
 */
typedef struct f_smgr
{
	void		(*smgr_init) (void);	/* 可以为 NULL */
	void		(*smgr_shutdown) (void);	/* 可以为 NULL */
	void		(*smgr_open) (SMgrRelation reln);
	void		(*smgr_close) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_create) (SMgrRelation reln, ForkNumber forknum,
								bool isRedo);
	bool		(*smgr_exists) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_unlink) (RelFileLocatorBackend rlocator, ForkNumber forknum,
								bool isRedo);
	void		(*smgr_extend) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, const void *buffer, bool skipFsync);
	void		(*smgr_zeroextend) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, int nblocks, bool skipFsync);
	bool		(*smgr_prefetch) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber blocknum);
	void		(*smgr_read) (SMgrRelation reln, ForkNumber forknum,
							  BlockNumber blocknum, void *buffer);
	void		(*smgr_write) (SMgrRelation reln, ForkNumber forknum,
							   BlockNumber blocknum, const void *buffer, bool skipFsync);
	void		(*smgr_writeback) (SMgrRelation reln, ForkNumber forknum,
								   BlockNumber blocknum, BlockNumber nblocks);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_truncate) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber old_blocks, BlockNumber nblocks);
	void		(*smgr_immedsync) (SMgrRelation reln, ForkNumber forknum);
} f_smgr;

static const f_smgr smgrsw[] = {
	/* 磁盘 */
	{
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink,
		.smgr_extend = mdextend,
		.smgr_zeroextend = mdzeroextend,
		.smgr_prefetch = mdprefetch,
		.smgr_read = mdread,
		.smgr_write = mdwrite,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
	}
};

static const int NSmgr = lengthof(smgrsw);

/*
 * 每个后端都有一张散列表，存放全部现存的 SMgrRelation 对象。
 * 此外，"无主"的 SMgrRelation 对象被串在一条链表里。
 */
static HTAB *SMgrRelationHash = NULL;

static dlist_head unowned_relns;

/* 本地函数原型 */
static void smgrshutdown(int code, Datum arg);


/*
 * smgrinit()、smgrshutdown() -- 初始化或关闭存储
 *								 管理器。
 *
 * 注意：smgrinit 在后端启动时调用（普通或独立
 * 运行），*不是*在 postmaster 启动期间。因此，这里创建的
 * 或在 smgrshutdown 里销毁的任何资源都是后端本地的。
 */
void
smgrinit(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
			smgrsw[i].smgr_init();
	}

	/* 登记关闭过程 */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * 后端关闭期间做 smgr 清理的 on_proc_exit 钩子
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
			smgrsw[i].smgr_shutdown();
	}
}

/*
 * smgropen() -- 返回一个 SMgrRelation 对象，必要时创建它。
 *
 * 这里并不试图真正打开底层文件。
 */
SMgrRelation
smgropen(RelFileLocator rlocator, BackendId backend)
{
	RelFileLocatorBackend brlocator;
	SMgrRelation reln;
	bool		found;

	if (SMgrRelationHash == NULL)
	{
		/* 第一次进入：初始化散列表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocatorBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_BLOBS);
		dlist_init(&unowned_relns);
	}

	/* 查找或创建一个表项 */
	brlocator.locator = rlocator;
	brlocator.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &brlocator,
									  HASH_ENTER, &found);

	/* 若先前不存在，则初始化它 */
	if (!found)
	{
		/* hash_search 已经填好了查找键 */
		reln->smgr_owner = NULL;
		reln->smgr_targblock = InvalidBlockNumber;
		for (int i = 0; i <= MAX_FORKNUM; ++i)
			reln->smgr_cached_nblocks[i] = InvalidBlockNumber;
		reln->smgr_which = 0;	/* 目前只有 md.c */

		/* 特定实现的初始化 */
		smgrsw[reln->smgr_which].smgr_open(reln);

		/* 它还没有所有者 */
		dlist_push_tail(&unowned_relns, &reln->node);
	}

	return reln;
}

/*
 * smgrsetowner() -- 建立对一个 SMgrRelation 对象的长期引用
 *
 * 同一时刻只能有一个所有者；这已经够用，因为目前
 * 仅有的这类所有者都在关系缓存里。
 */
void
smgrsetowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* 这里不支持"放弃"一个 SMgrRelation 的所有权，请用 smgrclearowner */
	Assert(owner != NULL);

	/*
	 * 首先，卸下任何旧的所有者。（通常不应当有，但在
	 * swap_relation_files() 期间，取决于处理顺序，这似乎
	 * 可能发生。那种情况下提前关掉旧的
	 * 关系缓存项是可以的。）
	 *
	 * 若没有旧的所有者，则该 reln 应当在无主
	 * 链表里，我们需要把它移出去。
	 */
	if (reln->smgr_owner)
		*(reln->smgr_owner) = NULL;
	else
		dlist_delete(&reln->node);

	/* 现在建立所有权关系。 */
	reln->smgr_owner = owner;
	*owner = reln;
}

/*
 * smgrclearowner() -- 去掉对一个 SMgrRelation 对象的长期引用，
 *					   若存在的话
 */
void
smgrclearowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* 若该 SMgrRelation 对象并不由这个所有者拥有，则什么都不做 */
	if (reln->smgr_owner != owner)
		return;

	/* 清掉所有者的引用 */
	*owner = NULL;

	/* 清掉我们指向所有者的引用 */
	reln->smgr_owner = NULL;

	/* 加入无主关系链表 */
	dlist_push_tail(&unowned_relns, &reln->node);
}

/*
 * smgrexists() -- 某个分叉的底层文件是否存在？
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	return smgrsw[reln->smgr_which].smgr_exists(reln, forknum);
}

/*
 * smgrclose() -- 关闭并删除一个 SMgrRelation 对象。
 */
void
smgrclose(SMgrRelation reln)
{
	SMgrRelation *owner;
	ForkNumber	forknum;

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);

	owner = reln->smgr_owner;

	if (!owner)
		dlist_delete(&reln->node);

	if (hash_search(SMgrRelationHash,
					&(reln->smgr_rlocator),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	/*
	 * 卸下所有者指针，若有的话。我们最后才做这一步，因为万一
	 * 上面极小概率地失败了，这个 SMgrRelation 对象仍会存在。
	 */
	if (owner)
		*owner = NULL;
}

/*
 * smgrrelease() -- 释放本对象使用的全部资源。
 *
 * 对象本身仍然有效。
 */
void
smgrrelease(SMgrRelation reln)
{
	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	}
	reln->smgr_targblock = InvalidBlockNumber;
}

/*
 * smgrreleaseall() -- 释放所有对象使用的资源。
 *
 * 为 PROCSIGNAL_BARRIER_SMGRRELEASE 而调用。
 */
void
smgrreleaseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* 若散列表尚未建立，则没什么要做的 */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrrelease(reln);
}

/*
 * smgrcloseall() -- 关闭全部现存的 SMgrRelation 对象。
 */
void
smgrcloseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* 若散列表尚未建立，则没什么要做的 */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrclose(reln);
}

/*
 * smgrcloserellocator() -- 关闭给定 RelFileLocator 的 SMgrRelation 对象，
 *							若存在的话。
 *
 * 效果与 smgrclose(smgropen(rlocator)) 相同，但避免了
 * 在原本没有表项时白白创建一个散列表项
 * 再把它丢掉。
 */
void
smgrcloserellocator(RelFileLocatorBackend rlocator)
{
	SMgrRelation reln;

	/* 若散列表尚未建立，则没什么要做的 */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &rlocator,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrclose(reln);
}

/*
 * smgrcreate() -- 创建一个新关系。
 *
 * 给定一个已经创建（但想必尚未使用）的 SMgrRelation，
 * 让该分叉的底层磁盘文件或其他存储
 * 被创建出来。
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	smgrsw[reln->smgr_which].smgr_create(reln, forknum, isRedo);
}

/*
 * smgrdosyncall() -- 立即同步所有给定关系的全部分叉
 *
 * 所有给定关系的全部分叉都被同步到存储上。
 *
 * 这等价于对每个 smgr 关系调用 FlushRelationBuffers()，
 * 再对每个关系的每个分叉调用 smgrimmedsync()，但它
 * 明显更快，因此能用时应当优先使用。
 */
void
smgrdosyncall(SMgrRelation *rels, int nrels)
{
	int			i = 0;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	FlushRelationsAllBuffers(rels, nrels);

	/*
	 * 同步物理文件（file(s)）。
	 */
	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		{
			if (smgrsw[which].smgr_exists(rels[i], forknum))
				smgrsw[which].smgr_immedsync(rels[i], forknum);
		}
	}
}

/*
 * smgrdounlinkall() -- 立即 unlink 所有给定关系的全部分叉
 *
 * 所有给定关系的全部分叉都从存储上删掉。事务性操作期间
 * 不应当使用本函数，因为它无法
 * 撤销。
 *
 * 若 isRedo 为 true，底层文件（file(s)）已经
 * 不在了也没有问题。
 */
void
smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo)
{
	int			i = 0;
	RelFileLocatorBackend *rlocators;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	/*
	 * 丢掉这些关系上任何残留的缓冲区。bufmgr 会直接
	 * 丢弃它们，而不费力把内容写出去。
	 */
	DropRelationsAllBuffers(rels, nrels);

	/*
	 * 创建一个包含全部待删除关系的数组，并在此过程中
	 * 在 smgr 层关掉每个关系的分叉
	 */
	rlocators = palloc(sizeof(RelFileLocatorBackend) * nrels);
	for (i = 0; i < nrels; i++)
	{
		RelFileLocatorBackend rlocator = rels[i]->smgr_rlocator;
		int			which = rels[i]->smgr_which;

		rlocators[i] = rlocator;

		/* 在 smgr 层关闭各分叉 */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_close(rels[i], forknum);
	}

	/*
	 * 发送一条共享缓存失效消息，迫使其他后端关掉它们可能仍握着的、
	 * 指向这些关系的悬空 smgr 引用。应当在
	 * 真正开始 unlink 之前做这件事，以免这一步做到一半失败。
	 * 注意，sinval 消息最终也会
	 * 回到本后端，从而兜底确保我们
	 * 关掉了自己的 smgr 关系。
	 */
	for (i = 0; i < nrels; i++)
		CacheInvalidateSmgr(rlocators[i]);

	/*
	 * 删除物理文件（file(s)）。
	 *
	 * 注意：smgr_unlink 必须把删除失败当作 WARNING，而不是
	 * ERROR，因为我们已经决定提交或中止当前
	 * 事务。
	 */

	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_unlink(rlocators[i], forknum, isRedo);
	}

	pfree(rlocators);
}


/*
 * smgrextend() -- 给文件增加一个新块。
 *
 * 语义与 smgrwrite() 几乎相同：写到
 * 指定位置。不过本函数用于扩展关系的情形
 * （即 blocknum 位于当前 EOF 或超出 EOF）。
 * 注意，我们假定在当前 EOF 之后写一个块时，
 * 中间的文件空间会被填成零。
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void *buffer, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_extend(reln, forknum, blocknum,
										 buffer, skipFsync);

	/*
	 * 通常我们预期这会使 nblocks 增加一，但若缓存的
	 * 值与预期不符，就把它作废，让下次调用去问
	 * 内核。
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + 1;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
}

/*
 * smgrzeroextend() -- 给文件增加新的填零块。
 *
 * 与 smgrextend() 类似，只是关系可以一次
 * 扩展多个块，并且新增的块会被填成
 * 零。
 */
void
smgrzeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_zeroextend(reln, forknum, blocknum,
											 nblocks, skipFsync);

	/*
	 * 通常我们预期这会使分叉大小增加 nblocks，但若
	 * 缓存的值与预期不符，就把它作废，让下次调用
	 * 去问内核。
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + nblocks;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
}

/*
 * smgrprefetch() -- 发起对关系指定块的异步读。
 *
 * 仅在恢复中，本函数可以返回 false，表示文件
 * 不存在（想必已被后来的 WAL
 * 记录删除）。
 */
bool
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	return smgrsw[reln->smgr_which].smgr_prefetch(reln, forknum, blocknum);
}

/*
 * smgrread() -- 从关系中把特定块读进提供的
 *				 缓冲区。
 *
 * 缓冲管理器调用本例程，以便
 * 在共享缓冲区里实例化页。所有存储管理器
 * 都按 POSTGRES 期望的格式返回页。
 */
void
smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 void *buffer)
{
	smgrsw[reln->smgr_which].smgr_read(reln, forknum, blocknum, buffer);
}

/*
 * smgrwrite() -- 把提供的缓冲区写出去。
 *
 * 只用于更新关系中已经存在的块
 * （即当前 EOF 之前的那些块）。要扩展关系，
 * 请用 smgrextend()。
 *
 * 这不是同步写——返回时块不一定
 * 已经在磁盘上，只是交给了内核。不过，
 * 会设法在下一次检查点之前对该写做 fsync。
 *
 * skipFsync 表示调用者会另行安排对该关系做
 * fsync，因此我们不必费心。临时关系同样
 * 不需要 fsync。
 */
void
smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  const void *buffer, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_write(reln, forknum, blocknum,
										buffer, skipFsync);
}


/*
 * smgrwriteback() -- 触发内核把所提供的那段
 *					   块写回。
 */
void
smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	smgrsw[reln->smgr_which].smgr_writeback(reln, forknum, blocknum,
											nblocks);
}

/*
 * smgrnblocks() -- 计算所提供关系中的
 *					块数。
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	/* 若块数已有缓存值，则检查并返回。 */
	result = smgrnblocks_cached(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	result = smgrsw[reln->smgr_which].smgr_nblocks(reln, forknum);

	reln->smgr_cached_nblocks[forknum] = result;

	return result;
}

/*
 * smgrnblocks_cached() -- 取得所提供
 *						   关系中缓存的块数。
 *
 * 不在恢复中，以及该关系
 * 分叉大小未被缓存时，返回 InvalidBlockNumber。
 */
BlockNumber
smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * 目前，由于文件大小的变化缺少共享失效机制，本函数
	 * 只在恢复中使用缓存值。别处的代码会读取 smgr_cached_nblocks，
	 * 并自行应对过期数据。
	 */
	if (InRecovery && reln->smgr_cached_nblocks[forknum] != InvalidBlockNumber)
		return reln->smgr_cached_nblocks[forknum];

	return InvalidBlockNumber;
}

/*
 * smgrtruncate() -- 把所提供关系的给定分叉截断到
 *					 各自指定的块数
 *
 * 为外部调用者提供的、与 smgrtruncate2() 兼容的旧版本。
 * PostgreSQL 核心代码并不使用这个版本，也不能在
 * 临界区里使用。
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks,
			 BlockNumber *nblocks)
{
	BlockNumber old_nblocks[MAX_FORKNUM + 1];

	for (int i = 0; i < nforks; ++i)
		old_nblocks[i] = smgrnblocks(reln, forknum[i]);

	smgrtruncate2(reln, forknum, nforks, old_nblocks, nblocks);
}

/*
 * smgrtruncate2() -- 把所提供关系的给定分叉截断到
 *					  各自指定的块数
 *
 * 截断立即完成，因此无法回滚。
 *
 * 调用者必须持有该关系上的 AccessExclusiveLock，以确保
 * 其他后端先收到本函数发出的 smgr 失效事件，
 * 然后才会再次访问该关系的任何分叉。分叉的当前大小
 * 应当放在 old_nblocks 里提供。本函数通常应当
 * 在临界区里调用，但当前大小必须在
 * 临界区之外检查，并且其间不得处理中断，也不得调用与
 * 本关系相关的 smgr 函数。
 */
void
smgrtruncate2(SMgrRelation reln, ForkNumber *forknum, int nforks,
			  BlockNumber *old_nblocks, BlockNumber *nblocks)
{
	int			i;

	/*
	 * 丢掉即将被删除的那些块上的任何缓冲区。bufmgr 会直接
	 * 丢弃它们，而不费力把内容写出去。
	 */
	DropRelationBuffers(reln, forknum, nforks, nblocks);

	/*
	 * 发送一条共享缓存失效消息，迫使其他后端关掉它们可能仍握着的、
	 * 指向本关系的 smgr 引用。这很有用，因为它们
	 * 可能握着指向已被删除的段的打开文件指针，并且/或者
	 * smgr_targblock 变量指向新的关系末尾之后。（失效
	 * 消息也会回到我们自己的后端，导致一次
	 * 大概并无必要的本地 smgr 刷新。但我们并不期望这
	 * 是一条性能关键的路径。）与 unlink 代码一样，我们希望
	 * 在开始改动磁盘上的东西之前，确保消息已经发出。
	 */
	CacheInvalidateSmgr(reln->smgr_rlocator);

	/* 执行截断 */
	for (i = 0; i < nforks; i++)
	{
		/* 若遇到错误，就使缓存的大小失效。 */
		reln->smgr_cached_nblocks[forknum[i]] = InvalidBlockNumber;

		smgrsw[reln->smgr_which].smgr_truncate(reln, forknum[i],
											   old_nblocks[i], nblocks[i]);

		/*
		 * 不妨更新本地的 smgr_cached_nblocks 值。本函数发出的
		 * smgr 缓存失效消息会使其他
		 * 后端作废它们的 smgr_fsm_nblocks 与
		 * smgr_vm_nblocks 副本，并在下一条命令边界处也作废这些值。
		 * 但这些本地更新保证在那之前它们不至于完全错误。
		 */
		reln->smgr_cached_nblocks[forknum[i]] = nblocks[i];
	}
}

/*
 * smgrimmedsync() -- 把指定关系强制写入稳定存储。
 *
 * 同步地把此前对指定关系的全部写
 * 压到磁盘上。
 *
 * 这对构建全新的关系（例如新的
 * 索引）很有用。可以不把索引构建的各步
 * 逐步记入 WAL，而只是用 smgrwrite
 * 或 smgrextend 把完成的索引页写到磁盘，然后在
 * 提交事务之前 fsync 完成的索引文件。
 * （就崩溃恢复而言这已经足够，因为它实际上相当于对
 * 完成的索引强制做了一次检查点。但若希望
 * 把 WAL 日志用于 PITR 或复制，这 *并不* 足够：那种情况下
 * 我们还必须写 WAL 记录。）
 *
 * 前面的写应当指定 skipFsync = true，以免
 * 重复 fsync。
 *
 * 注意，若该关系有可能存在脏缓冲区，
 * 需要先做 FlushRelationBuffers()；
 * 否则这次同步就没什么意义。
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	smgrsw[reln->smgr_which].smgr_immedsync(reln, forknum);
}

/*
 * AtEOXact_SMgr
 *
 * 本例程在事务提交或中止时调用（它并不特别
 * 在意是哪一种）。所有临时的 SMgrRelation 对象都会被关闭。
 *
 * 这是一种折中：既希望临时的 SMgrRelations
 * 存活一段时间（以分摊对多个块做盲写的代价），
 * 又不能让它们永远活着（因为我们很可能握着底层文件的
 * 一个内核文件描述符，若文件被删除，需要确保
 * 它在合理的时间内被关掉）。
 */
void
AtEOXact_SMgr(void)
{
	dlist_mutable_iter iter;

	/*
	 * 清掉所有无主的 SMgrRelations。我们依靠 smgrclose() 把每一个
	 * 从链表里去掉。
	 */
	dlist_foreach_modify(iter, &unowned_relns)
	{
		SMgrRelation rel = dlist_container(SMgrRelationData, node,
										   iter.cur);

		Assert(rel->smgr_owner == NULL);

		smgrclose(rel);
	}
}

/*
 * 当 ProcSignalBarrier 命令我们释放全部打开的文件时，
 * 调用本例程。
 */
bool
ProcessBarrierSmgrRelease(void)
{
	smgrreleaseall();
	return true;
}
