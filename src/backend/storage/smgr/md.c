/*-------------------------------------------------------------------------
 *
 * md.c
 *	  本代码管理驻留在磁盘上的关系。
 *
 * 至少，Berkeley 的人给这个文件起名时，心里想的就是这个。
 * 实际上，本代码提供的是从 smgr API 通向类 Unix
 * 文件系统 APIs 的接口，因此只要操作系统提供文件系统
 * 支持，任何类型的设备都能工作。这些位是落在旋转的锈片上，
 * 还是落在别的某种存储技术上，
 * 都无关紧要。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/md.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/xlog.h"
#include "access/xlogutils.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/md.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * 磁盘存储管理器在自己的描述符池里跟踪已打开的文件
 * 描述符。这样做是为了更容易支持那些超过操作系统
 * 文件大小限制（常常是 2GBytes）的关系。为此，
 * 我们把关系拆成称为"段"的文件，每一段都短于
 * 操作系统的文件大小限制。段大小由 RELSEG_SIZE
 * 这个配置常量设定，它定义在
 * pg_config.h 里。
 *
 * 在磁盘上，一个关系必须由连续编号的段
 * 文件按下列模式组成：
 *	-- 零个或多个完整段，每段恰好 RELSEG_SIZE 个块
 *	-- 恰好一个部分段，大小满足 0 <= size < RELSEG_SIZE 个块
 *	-- 可选地，任意多个大小为 0 个块的非活动段。
 * 完整段与部分段合称为"活动"段。
 * 非活动段是曾经含有数据、但目前因一次
 * mdtruncate() 而不再需要的段。把它们以大小为零留着，
 * 而不是 unlink 掉，原因是其他后端和/或检查点进程
 * 可能仍握着指向这些段的打开文件引用。若关系在
 * mdtruncate() 之后再次扩展，使一个已停用的段
 * 重新变为活动，这些文件引用必须仍然有效——
 * 否则数据可能被写到一份已经 unlink 的旧段文件
 * 副本上，而那份副本最终会
 * 消失。
 *
 * 文件描述符存放在按分叉划分的 md_seg_fds 数组中，这些数组位于
 * SMgrRelation 之内。数组长度存放在 md_num_open_segs 里。
 * 注意，某个分叉的 md_num_open_segs 取了某个具体值，并不
 * 必然表示该关系没有更多的段；我们可能
 * 只是还没打开下一段。（无论如何也无法把"所有
 * 段都在数组里"当作不变量，因为另一个后端
 * 可能在我们没看的时候扩展该关系。）不过我们不为
 * 非活动段保留表项；一旦发现一个部分
 * 段，就假定其后的任何段都是非活动的。
 *
 * 整个 MdfdVec 数组在 MdCxt 内存上下文里用 palloc 分配。
 */

typedef struct _MdfdVec
{
	File		mdfd_vfd;		/* fd.c 池中的 fd 号 */
	BlockNumber mdfd_segno;		/* 段号，从 0 起 */
} MdfdVec;

static MemoryContext MdCxt;		/* 所有 MdfdVec 对象所用的上下文 */


/* 填充一个描述 md.c 段文件的文件标签。 */
#define INIT_MD_FILETAG(a,xx_rlocator,xx_forknum,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = SYNC_HANDLER_MD, \
	(a).rlocator = (xx_rlocator), \
	(a).forknum = (xx_forknum), \
	(a).segno = (xx_segno) \
)


/*** mdopen 与 _mdfd_getseg 的行为 ***/
/* 段不存在时 ereport */
#define EXTENSION_FAIL				(1 << 0)
/* 段不存在时返回 NULL */
#define EXTENSION_RETURN_NULL		(1 << 1)
/* 按需要创建新段 */
#define EXTENSION_CREATE			(1 << 2)
/* 恢复期间若需要则创建新段 */
#define EXTENSION_CREATE_RECOVERY	(1 << 3)
/*
 * 允许打开前面还有短于 RELSEG_SIZE 的段的那些段，
 * 例如非活动段（见上文）。注意，此后 mdnblocks()
 * 及相关功能就不再成立——目前这是可以接受的，
 * 因为只有检查点进程需要这样做，而它从不使用
 * mdnblocks()。
 */
#define EXTENSION_DONT_CHECK_SIZE	(1 << 4)
/* 若段尚未打开，则不要尝试打开 */
#define EXTENSION_DONT_OPEN			(1 << 5)


/* 本地例程 */
static void mdunlinkfork(RelFileLocatorBackend rlocator, ForkNumber forknum,
						 bool isRedo);
static MdfdVec *mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior);
static void register_dirty_segment(SMgrRelation reln, ForkNumber forknum,
								   MdfdVec *seg);
static void register_unlink_segment(RelFileLocatorBackend rlocator, ForkNumber forknum,
									BlockNumber segno);
static void register_forget_request(RelFileLocatorBackend rlocator, ForkNumber forknum,
									BlockNumber segno);
static void _fdvec_resize(SMgrRelation reln,
						  ForkNumber forknum,
						  int nseg);
static char *_mdfd_segpath(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber segno);
static MdfdVec *_mdfd_openseg(SMgrRelation reln, ForkNumber forknum,
							  BlockNumber segno, int oflags);
static MdfdVec *_mdfd_getseg(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blkno, bool skipFsync, int behavior);
static BlockNumber _mdnblocks(SMgrRelation reln, ForkNumber forknum,
							  MdfdVec *seg);

static inline int
_mdfd_open_flags(void)
{
	int			flags = O_RDWR | PG_BINARY;

	if (io_direct_flags & IO_DIRECT_DATA)
		flags |= PG_O_DIRECT;

	return flags;
}

/*
 * mdinit() -- 初始化磁盘存储管理器的私有状态。
 */
void
mdinit(void)
{
	MdCxt = AllocSetContextCreate(TopMemoryContext,
								  "MdSmgr",
								  ALLOCSET_DEFAULT_SIZES);
}

/*
 * mdexists() -- 物理文件是否存在？
 *
 * 注意：对于仍残留、尚有待删除的文件，这里会返回 true
 */
bool
mdexists(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * 先把它关掉，以确保若该分叉在我们打开之后已被 unlink，
	 * 我们能够察觉。作为优化，恢复期间可以跳过这一步，
	 * 因为删除关系时恢复过程已经把它们关掉了。
	 */
	if (!InRecovery)
		mdclose(reln, forknum);

	return (mdopenfork(reln, forknum, EXTENSION_RETURN_NULL) != NULL);
}

/*
 * mdcreate() -- 在磁盘上创建一个新关系。
 *
 * 若 isRedo 为 true，关系已经存在也没有问题。
 */
void
mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	MdfdVec    *mdfd;
	char	   *path;
	File		fd;

	if (isRedo && reln->md_num_open_segs[forknum] > 0)
		return;					/* 已经创建并打开…… */

	Assert(reln->md_num_open_segs[forknum] == 0);

	/*
	 * 本数据库可能是第一次使用目标表空间，因此若需要
	 * 就创建每个数据库一个的子目录。
	 *
	 * XXX 这相当难看地破坏了模块分层，但检查放在这里
	 * 似乎最合适。也许 TablespaceCreateDbspace
	 * 应该放在这里，而不是 commands/tablespace.c？但那样就得
	 * 引入一大堆 smgr.c 同样不该知道的东西。
	 */
	TablespaceCreateDbspace(reln->smgr_rlocator.locator.spcOid,
							reln->smgr_rlocator.locator.dbOid,
							isRedo);

	path = relpath(reln->smgr_rlocator, forknum);

	fd = PathNameOpenFile(path, _mdfd_open_flags() | O_CREAT | O_EXCL);

	if (fd < 0)
	{
		int			save_errno = errno;

		if (isRedo)
			fd = PathNameOpenFile(path, _mdfd_open_flags());
		if (fd < 0)
		{
			/* 务必报告 create 报出的错误，而不是 open 的 */
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", path)));
		}
	}

	pfree(path);

	_fdvec_resize(reln, forknum, 1);
	mdfd = &reln->md_seg_fds[forknum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;

	if (!SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, mdfd);
}

/*
 * mdunlink() -- 删除一个关系。
 *
 * 注意，传进来的是 RelFileLocatorBackend——调用到这里时，
 * 散列表里已经不会再有 SMgrRelation 表项了。
 *
 * forknum 可以是要删除的某个分叉号，也可以是 InvalidForkNumber，
 * 表示删除所有分叉。
 *
 * 对于普通关系，我们不 unlink 该关系的第一段文件，
 * 只是把它截断到长度为零，并登记一个请求，在下一次
 * 检查点之后再 unlink。其余段则可以立刻
 * unlink。留下这个空文件，是为了防止该 relfilenumber
 * 被重新使用。这样防住的场景是：
 * 1. 我们删除一个关系（提交，并真正删掉它的文件）。
 * 2. 我们创建一个新关系，碰巧得到与刚删除的那个相同的 relfilenumber
 *	  （OIDs 必须已经回绕才会发生）。
 * 3. 在另一次检查点发生之前我们崩溃了。
 * 重放时，我们会先删掉文件再重建它；若文件内容由后续的 WAL
 * 记录重新填入，这没有问题。
 * 但若我们没有把插入记入 WAL，而是靠填完之后对该文件做
 * fsync（wal_level=minimal 时就是这样做的），文件内容
 * 就会永远丢失。把空文件留到下一次
 * 检查点之后，就能阻止在安全之前重新分配该 relfilenumber，
 * 因为分配 relfilenumber 时会跳过任何已存在的文件。
 *
 * 其余段若存在，先截断再 unlink。截断的原因
 * 是其他后端在 smgr 层可能仍握着这些文件的打开 FDs，
 * 于是内核还不能删掉文件。尽管如此，我们仍希望
 * 立刻收回磁盘空间。
 *
 * 不过临时关系不必走这套步骤，因为
 * 我们从不给临时关系写 WAL 记录，因此临时关系不会威胁
 * 接管了它的 relfilenumber 的普通关系的完好性。
 * 临时关系与普通关系的文件命名
 * 模式不同，这提供了额外的安全性。其他后端也不应当握着
 * 它们的打开 FDs。
 *
 * 做二进制升级时我们也不这样做。那种情况下没有重用
 * 风险，因为崩溃甚至一次简单的 ERROR 之后，
 * 升级即失败，整个数据库集簇必须从头重建。
 * 此外，必须立刻从磁盘上删掉这些文件，
 * 因为我们可能马上就要重用同一个 relfilenumber。
 *
 * 以上全部只适用于关系的主分叉；其他分叉可以
 * 立刻删除，因为它们并不用来防止
 * relfilenumber 被回收。而且我们并不仔细
 * 跟踪其他分叉是否已经创建，只是无条件地尝试
 * unlink 它们；因此绝不应当为 ENOENT 抱怨。
 *
 * 若 isRedo 为 true，关系已经不在了并不意外。
 * 而且应当立刻删除文件，而不是把请求
 * 排队留到以后，因为重做期间不可能再创建出一个
 * 冲突的关系。
 *
 * 注意：目前我们对 ENOENT 一概不警告。本来可以在
 * 主分叉且非 isRedo 的情况下警告，但似乎不值得费这个事。
 *
 * 注意：任何失败都应当报告为 WARNING 而不是 ERROR，因为
 * 调用到这里时，我们通常已经不在事务里了。
 */
void
mdunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	/* 现在按分叉分别处理 */
	if (forknum == InvalidForkNumber)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			mdunlinkfork(rlocator, forknum, isRedo);
	}
	else
		mdunlinkfork(rlocator, forknum, isRedo);
}

/*
 * 把文件截断以释放磁盘空间。
 */
static int
do_truncate(const char *path)
{
	int			save_errno;
	int			ret;

	ret = pg_truncate(path, 0);

	/* 在这里记一条警告，以免调用者重复记录。 */
	if (ret < 0 && errno != ENOENT)
	{
		save_errno = errno;
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m", path)));
		errno = save_errno;
	}

	return ret;
}

static void
mdunlinkfork(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	char	   *path;
	int			ret;
	int			save_errno;

	path = relpath(rlocator, forknum);

	/*
	 * 先截断再 unlink 第一段，或者只登记一个请求，
	 * 稍后再 unlink，如 mdunlink() 的注释所述。
	 */
	if (isRedo || IsBinaryUpgrade || forknum != MAIN_FORKNUM ||
		RelFileLocatorBackendIsTemp(rlocator))
	{
		if (!RelFileLocatorBackendIsTemp(rlocator))
		{
			/* 防止其他后端的 fd 继续占着磁盘空间 */
			ret = do_truncate(path);

			/* 忘掉第一段上任何待处理的同步请求 */
			save_errno = errno;
			register_forget_request(rlocator, forknum, 0 /* 第一段 */ );
			errno = save_errno;
		}
		else
			ret = 0;

		/* 接着 unlink 该文件，除非已经发现它不存在 */
		if (ret >= 0 || errno != ENOENT)
		{
			ret = unlink(path);
			if (ret < 0 && errno != ENOENT)
			{
				save_errno = errno;
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
				errno = save_errno;
			}
		}
	}
	else
	{
		/* 防止其他后端的 fd 继续占着磁盘空间 */
		ret = do_truncate(path);

		/* 登记请求，稍后再 unlink 第一段 */
		save_errno = errno;
		register_unlink_segment(rlocator, forknum, 0 /* 第一段 */ );
		errno = save_errno;
	}

	/*
	 * 删除其余任何段。
	 *
	 * 注意，因为我们一直循环到得到 ENOENT，活动段和
	 * 非活动段都会被正确地删掉。理想情况下应一直循环，
	 * 直到恰好得到这个 errno，但若问题出在整个目录，
	 * 那就有无限循环的风险（例如我们突然
	 * 读不了数据目录本身）。折中的做法是：
	 * 截断出现非 ENOENT 错误时继续，但任何
	 * unlink 错误都停下来。若确实是整个目录的问题，再多试
	 * 几次 unlink 反正也不会成功。
	 */
	if (ret >= 0 || errno != ENOENT)
	{
		char	   *segpath = (char *) palloc(strlen(path) + 12);
		BlockNumber segno;

		for (segno = 1;; segno++)
		{
			sprintf(segpath, "%s.%u", path, segno);

			if (!RelFileLocatorBackendIsTemp(rlocator))
			{
				/*
				 * 防止其他后端的 fd 继续占着磁盘
				 * 空间。不过若看到 ENOENT，就已经做完了。
				 */
				if (do_truncate(segpath) < 0 && errno == ENOENT)
					break;

				/*
				 * 在尝试 unlink 之前，忘掉这一段上任何
				 * 待处理的同步请求。
				 */
				register_forget_request(rlocator, forknum, segno);
			}

			if (unlink(segpath) < 0)
			{
				/* 最后一段之后出现 ENOENT 是预期的…… */
				if (errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m", segpath)));
				break;
			}
		}
		pfree(segpath);
	}

	pfree(path);
}

/*
 * mdextend() -- 给指定关系增加一个块。
 *
 * 语义与 mdwrite() 几乎相同：写到
 * 指定位置。不过本函数用于扩展关系的情形
 * （即 blocknum 位于当前 EOF 或超出 EOF）。
 * 注意，我们假定在当前 EOF 之后写一个块时，
 * 中间的文件空间会被填成零。
 */
void
mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void *buffer, bool skipFsync)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* 若本构建支持直接 I/O，缓冲区必须按 I/O 对齐。 */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	/* 这个断言平时开销太大…… */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum >= mdnblocks(reln, forknum));
#endif

	/*
	 * 若一个关系竟然长到 2^32-1 个块，就拒绝再扩展
	 * ——绝不能造出一个块号实际上就是
	 * InvalidBlockNumber 的块。（注意，由于 bufmgr.c 里上游的检查，
	 * 这个失败应当到不了。）
	 */
	if (blocknum == InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum),
						InvalidBlockNumber)));

	v = _mdfd_getseg(reln, forknum, blocknum, skipFsync, EXTENSION_CREATE);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	if ((nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_EXTEND)) != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not extend file \"%s\": %m",
							FilePathName(v->mdfd_vfd)),
					 errhint("Check free disk space.")));
		/* 短写：适当地报错 */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not extend file \"%s\": wrote only %d of %d bytes at block %u",
						FilePathName(v->mdfd_vfd),
						nbytes, BLCKSZ, blocknum),
				 errhint("Check free disk space.")));
	}

	if (!skipFsync && !SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, v);

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));
}

/*
 * mdzeroextend() -- 给指定关系增加新的填零块。
 *
 * 与 mdextend() 类似，只是关系可以一次扩展
 * 多个块，并且新增的块会被填成零。
 */
void
mdzeroextend(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber blocknum, int nblocks, bool skipFsync)
{
	MdfdVec    *v;
	BlockNumber curblocknum = blocknum;
	int			remblocks = nblocks;

	Assert(nblocks > 0);

	/* 这个断言平时开销太大…… */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum >= mdnblocks(reln, forknum));
#endif

	/*
	 * 若一个关系竟然长到 2^32-1 个块，就拒绝再扩展
	 * ——绝不能造出一个块号实际上是
	 * InvalidBlockNumber 或更大的块。
	 */
	if ((uint64) blocknum + nblocks >= (uint64) InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum),
						InvalidBlockNumber)));

	while (remblocks > 0)
	{
		BlockNumber segstartblock = curblocknum % ((BlockNumber) RELSEG_SIZE);
		off_t		seekpos = (off_t) BLCKSZ * segstartblock;
		int			numblocks;

		if (segstartblock + remblocks > RELSEG_SIZE)
			numblocks = RELSEG_SIZE - segstartblock;
		else
			numblocks = remblocks;

		v = _mdfd_getseg(reln, forknum, curblocknum, skipFsync, EXTENSION_CREATE);

		Assert(segstartblock < RELSEG_SIZE);
		Assert(segstartblock + numblocks <= RELSEG_SIZE);

		/*
		 * 若可用且有益，就经由 posix_fallocate()（通过
		 * FileFallocate()）来扩展关系。这常常比使用 write()
		 * 更高效，因为它通常不会让内核
		 * 为扩展出来的那些页分配页缓存空间。
		 *
		 * 不过小幅扩展不用 FileFallocate()，因为它会
		 * 破坏某些文件系统上的延迟分配。这个决定该放在
		 * 哪里还不清楚？目前先把界限定为
		 * 8，本地测试里 4 到 8 之间都可以。
		 */
		if (numblocks > 8)
		{
			int			ret;

			ret = FileFallocate(v->mdfd_vfd,
								seekpos, (off_t) BLCKSZ * numblocks,
								WAIT_EVENT_DATA_FILE_EXTEND);
			if (ret != 0)
			{
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not extend file \"%s\" with FileFallocate(): %m",
							   FilePathName(v->mdfd_vfd)),
						errhint("Check free disk space."));
			}
		}
		else
		{
			int			ret;

			/*
			 * 即便不想用 fallocate，扩展时仍可以
			 * 比逐个写每个 8kB 块更高效一些。
			 * pg_pwrite_zeros()（经由 FileZero()）使用 pg_pwritev_with_retry()
			 * 来避免多次写，也不必为扩展的
			 * 整个长度准备一块填零的缓冲区。
			 */
			ret = FileZero(v->mdfd_vfd,
						   seekpos, (off_t) BLCKSZ * numblocks,
						   WAIT_EVENT_DATA_FILE_EXTEND);
			if (ret < 0)
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not extend file \"%s\": %m",
							   FilePathName(v->mdfd_vfd)),
						errhint("Check free disk space."));
		}

		if (!skipFsync && !SmgrIsTemp(reln))
			register_dirty_segment(reln, forknum, v);

		Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));

		remblocks -= numblocks;
		curblocknum += numblocks;
	}
}

/*
 * mdopenfork() -- 打开指定关系的一个分叉。
 *
 * 有多个段时，注意我们只打开第一段。
 *
 * 若第一段不存在，则按 "behavior" 要么 ereport，要么返回 NULL。
 * 我们把 EXTENSION_CREATE 当成与 EXTENSION_FAIL 相同；
 * EXTENSION_CREATE 表示可以扩展一个已有关系，而不是
 * 凭空造出一个关系。
 */
static MdfdVec *
mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior)
{
	MdfdVec    *mdfd;
	char	   *path;
	File		fd;

	/* 已经打开则无需做事 */
	if (reln->md_num_open_segs[forknum] > 0)
		return &reln->md_seg_fds[forknum][0];

	path = relpath(reln->smgr_rlocator, forknum);

	fd = PathNameOpenFile(path, _mdfd_open_flags());

	if (fd < 0)
	{
		if ((behavior & EXTENSION_RETURN_NULL) &&
			FILE_POSSIBLY_DELETED(errno))
		{
			pfree(path);
			return NULL;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}

	pfree(path);

	_fdvec_resize(reln, forknum, 1);
	mdfd = &reln->md_seg_fds[forknum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;

	Assert(_mdnblocks(reln, forknum, mdfd) <= ((BlockNumber) RELSEG_SIZE));

	return mdfd;
}

/*
 * mdopen() -- 初始化新打开的关系。
 */
void
mdopen(SMgrRelation reln)
{
	/* 标记为未打开 */
	for (int forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		reln->md_num_open_segs[forknum] = 0;
}

/*
 * mdclose() -- 关闭指定关系，若它尚未关闭。
 */
void
mdclose(SMgrRelation reln, ForkNumber forknum)
{
	int			nopensegs = reln->md_num_open_segs[forknum];

	/* 已经关闭则无需做事 */
	if (nopensegs == 0)
		return;

	/* 从末尾开始关闭各段 */
	while (nopensegs > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][nopensegs - 1];

		FileClose(v->mdfd_vfd);
		_fdvec_resize(reln, forknum, nopensegs - 1);
		nopensegs--;
	}
}

/*
 * mdprefetch() -- 发起对关系指定块的异步读
 */
bool
mdprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
#ifdef USE_PREFETCH
	off_t		seekpos;
	MdfdVec    *v;

	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 InRecovery ? EXTENSION_RETURN_NULL : EXTENSION_FAIL);
	if (v == NULL)
		return false;

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	(void) FilePrefetch(v->mdfd_vfd, seekpos, BLCKSZ, WAIT_EVENT_DATA_FILE_PREFETCH);
#endif							/* USE_PREFETCH（预取） */

	return true;
}

/*
 * mdread() -- 从关系中读取指定块。
 */
void
mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
	   void *buffer)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* 若本构建支持直接 I/O，缓冲区必须按 I/O 对齐。 */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	TRACE_POSTGRESQL_SMGR_MD_READ_START(forknum, blocknum,
										reln->smgr_rlocator.locator.spcOid,
										reln->smgr_rlocator.locator.dbOid,
										reln->smgr_rlocator.locator.relNumber,
										reln->smgr_rlocator.backend);

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	nbytes = FileRead(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_READ);

	TRACE_POSTGRESQL_SMGR_MD_READ_DONE(forknum, blocknum,
									   reln->smgr_rlocator.locator.spcOid,
									   reln->smgr_rlocator.locator.dbOid,
									   reln->smgr_rlocator.locator.relNumber,
									   reln->smgr_rlocator.backend,
									   nbytes,
									   BLCKSZ);

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read block %u in file \"%s\": %m",
							blocknum, FilePathName(v->mdfd_vfd))));

		/*
		 * 短读：我们正处于 EOF 或已经越过 EOF，或者在
		 * EOF 处读到了一个不完整的块。通常这是错误；上层绝不应当
		 * 试图读取一个不存在的块。不过，若 zero_damaged_pages 为 ON，或
		 * 我们正处于 InRecovery，则应当改为返回零而不
		 * 抱怨。例如，这样就能处理试图更新一个
		 * 后来被截断掉的块的情形。
		 */
		if (zero_damaged_pages || InRecovery)
			MemSet(buffer, 0, BLCKSZ);
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read block %u in file \"%s\": read only %d of %d bytes",
							blocknum, FilePathName(v->mdfd_vfd),
							nbytes, BLCKSZ)));
	}
}

/*
 * mdwrite() -- 把提供的块写到适当的位置。
 *
 * 只用于更新关系中已经存在的块
 * （即当前 EOF 之前的那些块）。要扩展关系，
 * 请用 mdextend()。
 */
void
mdwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		const void *buffer, bool skipFsync)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* 若本构建支持直接 I/O，缓冲区必须按 I/O 对齐。 */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	/* 这个断言平时开销太大…… */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum < mdnblocks(reln, forknum));
#endif

	TRACE_POSTGRESQL_SMGR_MD_WRITE_START(forknum, blocknum,
										 reln->smgr_rlocator.locator.spcOid,
										 reln->smgr_rlocator.locator.dbOid,
										 reln->smgr_rlocator.locator.relNumber,
										 reln->smgr_rlocator.backend);

	v = _mdfd_getseg(reln, forknum, blocknum, skipFsync,
					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_WRITE);

	TRACE_POSTGRESQL_SMGR_MD_WRITE_DONE(forknum, blocknum,
										reln->smgr_rlocator.locator.spcOid,
										reln->smgr_rlocator.locator.dbOid,
										reln->smgr_rlocator.locator.relNumber,
										reln->smgr_rlocator.backend,
										nbytes,
										BLCKSZ);

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write block %u in file \"%s\": %m",
							blocknum, FilePathName(v->mdfd_vfd))));
		/* 短写：适当地报错 */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write block %u in file \"%s\": wrote only %d of %d bytes",
						blocknum,
						FilePathName(v->mdfd_vfd),
						nbytes, BLCKSZ),
				 errhint("Check free disk space.")));
	}

	if (!skipFsync && !SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, v);
}

/*
 * mdwriteback() -- 让内核把页写回存储。
 *
 * 这里接受一段块范围，因为一次刷出若干页
 * 比一页一页刷要高效得多。
 */
void
mdwriteback(SMgrRelation reln, ForkNumber forknum,
			BlockNumber blocknum, BlockNumber nblocks)
{
	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	/*
	 * 用尽可能少的请求发出刷写；不过必须在
	 * 段边界处拆开，因为那些其实是不同的文件。
	 */
	while (nblocks > 0)
	{
		BlockNumber nflush = nblocks;
		off_t		seekpos;
		MdfdVec    *v;
		int			segnum_start,
					segnum_end;

		v = _mdfd_getseg(reln, forknum, blocknum, true /* 未使用 */ ,
						 EXTENSION_DONT_OPEN);

		/*
		 * 我们可能正在刷写已经删除的关系的缓冲区，这
		 * 没关系，忽略这种情况即可。若段文件原先并未打开
		 * （例如来自最近一次 mdwrite()），我们就不想重新打开它，以免
		 * 与 PROCSIGNAL_BARRIER_SMGRRELEASE 发生竞争，结果留下
		 * 一个指向即将被 unlink 的文件的描述符。
		 */
		if (!v)
			return;

		/* 计算当前段内的偏移 */
		segnum_start = blocknum / RELSEG_SIZE;

		/* 计算当前段内期望写入的次数 */
		segnum_end = (blocknum + nblocks - 1) / RELSEG_SIZE;
		if (segnum_start != segnum_end)
			nflush = RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE));

		Assert(nflush >= 1);
		Assert(nflush <= nblocks);

		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

		FileWriteback(v->mdfd_vfd, seekpos, (off_t) BLCKSZ * nflush, WAIT_EVENT_DATA_FILE_FLUSH);

		nblocks -= nflush;
		blocknum += nflush;
	}
}

/*
 * mdnblocks() -- 取得关系中存放的块数。
 *
 * 重要的副作用：该关系的全部活动段都会被打开，
 * 并加入 md_seg_fds 数组。若还没调用过本例程，
 * 数组里就只有直到最近实际碰过的那一段为止的
 * 那些段。
 */
BlockNumber
mdnblocks(SMgrRelation reln, ForkNumber forknum)
{
	MdfdVec    *v;
	BlockNumber nblocks;
	BlockNumber segno;

	mdopenfork(reln, forknum, EXTENSION_FAIL);

	/* mdopen 已经打开了第一段 */
	Assert(reln->md_num_open_segs[forknum] > 0);

	/*
	 * 从最后打开的那些段开始，以免重复寻道。我们先前
	 * 已经确认这些段恰好长 RELSEG_SIZE，
	 * 每次再检查一遍并无用处。
	 *
	 * 注意：只有当另一个后端已经
	 * 截断了该关系时，这个假定才会错。我们依靠更高层代码
	 * 通过关闭再重新打开 md 的 fd 来处理那种
	 * 情形，这经由关系缓存刷新来完成。（由于检查点进程不参与
	 * 关系缓存刷新，它可能留有非活动段的段表项；
	 * 这没问题，因为检查点进程从不需要计算关系的
	 * 大小。）
	 */
	segno = reln->md_num_open_segs[forknum] - 1;
	v = &reln->md_seg_fds[forknum][segno];

	for (;;)
	{
		nblocks = _mdnblocks(reln, forknum, v);
		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");
		if (nblocks < ((BlockNumber) RELSEG_SIZE))
			return (segno * ((BlockNumber) RELSEG_SIZE)) + nblocks;

		/*
		 * 若该段恰好是 RELSEG_SIZE，就前进到下一段。
		 */
		segno++;

		/*
		 * 以前这里会传 O_CREAT，但那样有个缺点：它
		 * 可能把一个因某种操作系统意外而消失的段
		 * 重新创建出来。那种情况下，在这里创建该段会破坏
		 * _mdfd_getseg 察觉并报告错误的努力，
		 * 当有人去访问一个缺少的段时。
		 */
		v = _mdfd_openseg(reln, forknum, segno, 0);
		if (v == NULL)
			return segno * ((BlockNumber) RELSEG_SIZE);
	}
}

/*
 * mdtruncate() -- 把关系截断到指定的块数。
 *
 * 保证不分配内存，因此可以在临界区里使用。
 * 调用者必须已经调用 smgrnblocks() 取得 curnblk，同时持有
 * 足以防止关系大小变化的锁，并且其间没有对本关系使用任何 smgr
 * 函数，也没有处理过中断。这样我们才能确定
 * 已经打开了全部活动段，截断循环才会把它们
 * 全部处理到！
 */
void
mdtruncate(SMgrRelation reln, ForkNumber forknum,
		   BlockNumber curnblk, BlockNumber nblocks)
{
	BlockNumber priorblocks;
	int			curopensegs;

	if (nblocks > curnblk)
	{
		/* 无效的请求……但 InRecovery 时不报错 */
		if (InRecovery)
			return;
		ereport(ERROR,
				(errmsg("could not truncate file \"%s\" to %u blocks: it's only %u blocks now",
						relpath(reln->smgr_rlocator, forknum),
						nblocks, curnblk)));
	}
	if (nblocks == curnblk)
		return;					/* 无需做事 */

	/*
	 * 从最后一段开始截断各段。从末尾开始，出错时
	 * 管理 fd 数组的内存会更容易。
	 */
	curopensegs = reln->md_num_open_segs[forknum];
	while (curopensegs > 0)
	{
		MdfdVec    *v;

		priorblocks = (curopensegs - 1) * RELSEG_SIZE;

		v = &reln->md_seg_fds[forknum][curopensegs - 1];

		if (priorblocks > nblocks)
		{
			/*
			 * 这一段不再活动。我们截断文件，但
			 * 不删除它，原因见文件头注释。
			 */
			if (FileTruncate(v->mdfd_vfd, 0, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\": %m",
								FilePathName(v->mdfd_vfd))));

			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);

			/* 我们从不丢弃第一段 */
			Assert(v != &reln->md_seg_fds[forknum][0]);

			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, curopensegs - 1);
		}
		else if (priorblocks + ((BlockNumber) RELSEG_SIZE) > nblocks)
		{
			/*
			 * 这是我们想保留的最后一段。把文件截断到
			 * 正确的长度。注意：若 nblocks 恰好是 RELSEG_SIZE 的
			 * 整数倍 K，我们会把第 K+1 段截断到长度 0，但
			 * 仍然保留它。这符合文件头注释里给出的
			 * 不变量。
			 */
			BlockNumber lastsegblocks = nblocks - priorblocks;

			if (FileTruncate(v->mdfd_vfd, (off_t) lastsegblocks * BLCKSZ, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\" to %u blocks: %m",
								FilePathName(v->mdfd_vfd),
								nblocks)));
			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);
		}
		else
		{
			/*
			 * 我们仍然需要这一段，因此这一段以及任何
			 * 更早的段都没什么要做的。
			 */
			break;
		}
		curopensegs--;
	}
}

/*
 * mdimmedsync() -- 立即把关系同步到稳定存储。
 *
 * 注意，只同步已经发出的写；本例程对缓冲管理器内部
 * 可能存在的脏缓冲区一无所知。我们会同步
 * 活动段和非活动段；smgrDoPendingSyncs() 依赖这一点。
 * 考虑一个跳过 WAL 的关系。假设检查点同步了某段的
 * 若干块，然后 mdtruncate() 使该段变为非活动。若我们在
 * 下一次检查点同步这个新变为非活动的段之前崩溃，该
 * 段可能在恢复后幸存，从而把不想要的数据重新引入表中。
 */
void
mdimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	int			segno;
	int			min_inactive_seg;

	/*
	 * 注意：mdnblocks 确保我们已经打开了全部活动段，这样
	 * fsync 循环才会把它们全部处理到！
	 */
	mdnblocks(reln, forknum);

	min_inactive_seg = segno = reln->md_num_open_segs[forknum];

	/*
	 * 暂时打开非活动段，同步之后再关掉。fsync() 出错后
	 * 可能留下一些仍打开的非活动段，但那
	 * 无害。我们不费力去清理它们，以免再惹
	 * 更多麻烦。下一次 mdclose() 很快就会把它们关掉。
	 */
	while (_mdfd_openseg(reln, forknum, segno, 0) != NULL)
		segno++;

	while (segno > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][segno - 1];

		/*
		 * 经 mdimmedsync() 完成的 fsync 应当记在与
		 * 经 mdsyncfiletag() 完成的那些不同的 IOContext 里，以便区分
		 * 客户端后端无法避免的 fsync（例如索引构建期间
		 * 做的那些）和理想情况下本应由
		 * 检查点进程完成的那些。由于绕过缓冲
		 * 管理器的其他 IO 操作也可能记入这样的 IOContext，等到
		 * 那些也被跟踪之后，再来跟踪立即 fsync。
		 */
		if (FileSync(v->mdfd_vfd, WAIT_EVENT_DATA_FILE_IMMEDIATE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							FilePathName(v->mdfd_vfd))));

		/* 立即关闭非活动段 */
		if (segno > min_inactive_seg)
		{
			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, segno - 1);
		}

		segno--;
	}
}

/*
 * register_dirty_segment() -- 把关系的一段标为需要 fsync
 *
 * 若存在本地的待处理操作表，就在其中记一项，留给
 * ProcessSyncRequests 以后处理。否则，尝试把
 * fsync 请求交给检查点进程。若失败，就在返回前
 * 在本地做 fsync（我们希望这种情况不会频繁到
 * 成为性能问题）。
 */
static void
register_dirty_segment(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, reln->smgr_rlocator.locator, forknum, seg->mdfd_segno);

	/* 临时关系绝不应当做 fsync */
	Assert(!SmgrIsTemp(reln));

	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* retryOnError：出错时重试 */ ))
	{
		instr_time	io_start;

		ereport(DEBUG1,
				(errmsg_internal("could not forward fsync request because request queue is full")));

		io_start = pgstat_prepare_io_time();

		if (FileSync(seg->mdfd_vfd, WAIT_EVENT_DATA_FILE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							FilePathName(seg->mdfd_vfd))));

		/*
		 * 此刻我们无法知道当前 IOContext 是
		 * IOCONTEXT_NORMAL 还是 IOCONTEXT_[BULKREAD, BULKWRITE, VACUUM]，
		 * 因此把这次 fsync 计在 IOCONTEXT_NORMAL
		 * 这个 IOContext 里。这大概没问题，因为后端
		 * fsync 的次数并不能说明
		 * BufferAccessStrategy 是否有效。而且把在
		 * IOCONTEXT_NORMAL 和 IOCONTEXT_[BULKREAD, BULKWRITE, VACUUM] 中做的 fsync
		 * 都计在 IOCONTEXT_NORMAL 之下，在调查后端
		 * fsync 次数时可能更清楚。
		 */
		pgstat_count_io_op_time(IOOBJECT_RELATION, IOCONTEXT_NORMAL,
								IOOP_FSYNC, io_start, 1);
	}
}

/*
 * register_unlink_segment() -- 安排在下次检查点之后删除一个文件
 */
static void
register_unlink_segment(RelFileLocatorBackend rlocator, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rlocator.locator, forknum, segno);

	/* 绝不应当用于临时关系 */
	Assert(!RelFileLocatorBackendIsTemp(rlocator));

	RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST, true /* retryOnError：出错时重试 */ );
}

/*
 * register_forget_request() -- 忘掉关系某个分叉的一段上的任何 fsync
 */
static void
register_forget_request(RelFileLocatorBackend rlocator, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rlocator.locator, forknum, segno);

	RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true /* retryOnError：出错时重试 */ );
}

/*
 * ForgetDatabaseSyncRequests -- 忘掉某个数据库的任何 fsync 与 unlink
 */
void
ForgetDatabaseSyncRequests(Oid dbid)
{
	FileTag		tag;
	RelFileLocator rlocator;

	rlocator.dbOid = dbid;
	rlocator.spcOid = 0;
	rlocator.relNumber = 0;

	INIT_MD_FILETAG(tag, rlocator, InvalidForkNumber, InvalidBlockNumber);

	RegisterSyncRequest(&tag, SYNC_FILTER_REQUEST, true /* retryOnError：出错时重试 */ );
}

/*
 * DropRelationFiles -- 删除所有给定关系的文件
 */
void
DropRelationFiles(RelFileLocator *delrels, int ndelrels, bool isRedo)
{
	SMgrRelation *srels;
	int			i;

	srels = palloc(sizeof(SMgrRelation) * ndelrels);
	for (i = 0; i < ndelrels; i++)
	{
		SMgrRelation srel = smgropen(delrels[i], InvalidBackendId);

		if (isRedo)
		{
			ForkNumber	fork;

			for (fork = 0; fork <= MAX_FORKNUM; fork++)
				XLogDropRelation(delrels[i], fork);
		}
		srels[i] = srel;
	}

	smgrdounlinkall(srels, ndelrels, isRedo);

	for (i = 0; i < ndelrels; i++)
		smgrclose(srels[i]);
	pfree(srels);
}


/*
 * _fdvec_resize() -- 调整该分叉的打开段数组大小
 */
static void
_fdvec_resize(SMgrRelation reln,
			  ForkNumber forknum,
			  int nseg)
{
	if (nseg == 0)
	{
		if (reln->md_num_open_segs[forknum] > 0)
		{
			pfree(reln->md_seg_fds[forknum]);
			reln->md_seg_fds[forknum] = NULL;
		}
	}
	else if (reln->md_num_open_segs[forknum] == 0)
	{
		reln->md_seg_fds[forknum] =
			MemoryContextAlloc(MdCxt, sizeof(MdfdVec) * nseg);
	}
	else if (nseg > reln->md_num_open_segs[forknum])
	{
		/*
		 * 似乎不值得把代码写复杂来分摊
		 * repalloc() 调用。那些调用远快于 PathNameOpenFile() 或
		 * FileClose()，而且内存上下文内部有时会避免
		 * 真正重新分配。
		 */
		reln->md_seg_fds[forknum] =
			repalloc(reln->md_seg_fds[forknum],
					 sizeof(MdfdVec) * nseg);
	}
	else
	{
		/*
		 * 我们不把数组重新分配得更小，因为希望 mdtruncate()
		 * 能够承诺自己不分配内存，从而允许在
		 * 临界区里调用。这意味着数组里现在会浪费一点空间，
		 * 直到下次我们增加一段并
		 * 重新分配。
		 */
	}

	reln->md_num_open_segs[forknum] = nseg;
}

/*
 * 返回该关系指定段的文件名。返回的
 * 字符串用 palloc 分配。
 */
static char *
_mdfd_segpath(SMgrRelation reln, ForkNumber forknum, BlockNumber segno)
{
	char	   *path,
			   *fullpath;

	path = relpath(reln->smgr_rlocator, forknum);

	if (segno > 0)
	{
		fullpath = psprintf("%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;

	return fullpath;
}

/*
 * 打开该关系的指定段，
 * 并为其创建一个 MdfdVec 对象。失败时返回 NULL。
 */
static MdfdVec *
_mdfd_openseg(SMgrRelation reln, ForkNumber forknum, BlockNumber segno,
			  int oflags)
{
	MdfdVec    *v;
	File		fd;
	char	   *fullpath;

	fullpath = _mdfd_segpath(reln, forknum, segno);

	/* 打开文件 */
	fd = PathNameOpenFile(fullpath, _mdfd_open_flags() | oflags);

	pfree(fullpath);

	if (fd < 0)
		return NULL;

	/*
	 * 段总是从低到高依次打开，因此我们一定是
	 * 在末尾追加一个新段。
	 */
	Assert(segno == reln->md_num_open_segs[forknum]);

	_fdvec_resize(reln, forknum, segno + 1);

	/* 填好这个表项 */
	v = &reln->md_seg_fds[forknum][segno];
	v->mdfd_vfd = fd;
	v->mdfd_segno = segno;

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));

	/* 全部完成 */
	return v;
}

/*
 * _mdfd_getseg() -- 找到持有指定块的
 *					 那个关系段。
 *
 * 若该段不存在，我们按 "behavior" 来 ereport、返回 NULL 或创建
 * 该段。注意：skipFsync 只在
 * EXTENSION_CREATE 的情况下使用。
 */
static MdfdVec *
_mdfd_getseg(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
			 bool skipFsync, int behavior)
{
	MdfdVec    *v;
	BlockNumber targetseg;
	BlockNumber nextsegno;

	/* 必须指定某种处理不存在段的方式 */
	Assert(behavior &
		   (EXTENSION_FAIL | EXTENSION_CREATE | EXTENSION_RETURN_NULL |
			EXTENSION_DONT_OPEN));

	targetseg = blkno / ((BlockNumber) RELSEG_SIZE);

	/* 若是已经存在且已打开的段，就完成了 */
	if (targetseg < reln->md_num_open_segs[forknum])
	{
		v = &reln->md_seg_fds[forknum][targetseg];
		return v;
	}

	/* 调用者只想要我们已经打开的那段。 */
	if (behavior & EXTENSION_DONT_OPEN)
		return NULL;

	/*
	 * 目标段尚未打开。迭代最后打开的段与
	 * 目标段之间的所有段。这样，缺少的
	 * 段要么报错，要么被创建（取决于
	 * 'behavior'）。从最后打开的那段开始，若此前
	 * 一段都没打开过，就从第一段开始。
	 */
	if (reln->md_num_open_segs[forknum] > 0)
		v = &reln->md_seg_fds[forknum][reln->md_num_open_segs[forknum] - 1];
	else
	{
		v = mdopenfork(reln, forknum, behavior);
		if (!v)
			return NULL;		/* 若 behavior & EXTENSION_RETURN_NULL */
	}

	for (nextsegno = reln->md_num_open_segs[forknum];
		 nextsegno <= targetseg; nextsegno++)
	{
		BlockNumber nblocks = _mdnblocks(reln, forknum, v);
		int			flags = 0;

		Assert(nextsegno == v->mdfd_segno + 1);

		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");

		if ((behavior & EXTENSION_CREATE) ||
			(InRecovery && (behavior & EXTENSION_CREATE_RECOVERY)))
		{
			/*
			 * 通常只有在调用者授权时（也就是我们正在做
			 * mdextend()）才创建新段。但做 WAL
			 * 恢复时，无论如何都创建段；这样就能处理例如
			 * 重放一条写入某个关系高编号
			 * 段的 WAL 数据、而该关系后来已被删除的情形。我们希望
			 * 继续把这些段创建出来，以便把重放做完。
			 *
			 * 我们必须维持这样的不变量：最后一个
			 * 活动段之前的各段大小都是 RELSEG_SIZE；因此若正在
			 * 扩展，就在需要时用零把它们填满。（这只在
			 * 恢复中有影响，或者调用者不连续地扩展
			 * 关系时也有影响，散列索引里就会发生这种情况。）
			 */
			if (nblocks < ((BlockNumber) RELSEG_SIZE))
			{
				char	   *zerobuf = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE,
													 MCXT_ALLOC_ZERO);

				mdextend(reln, forknum,
						 nextsegno * ((BlockNumber) RELSEG_SIZE) - 1,
						 zerobuf, skipFsync);
				pfree(zerobuf);
			}
			flags = O_CREAT;
		}
		else if (!(behavior & EXTENSION_DONT_CHECK_SIZE) &&
				 nblocks < ((BlockNumber) RELSEG_SIZE))
		{
			/*
			 * 不在扩展时（或者没有显式把已截断的
			 * 段包括进来时），只有当前段恰好是
			 * RELSEG_SIZE 才打开下一段。否则（本分支）要么返回 NULL，
			 * 要么失败。
			 */
			if (behavior & EXTENSION_RETURN_NULL)
			{
				/*
				 * 有些调用者根据 errno 区分 _mdfd_getseg()
				 * 返回 NULL 的原因。这里并没有失败的
				 * 系统调用，因此显式把 errno 设为
				 * ENOENT，这似乎是最接近的解释。
				 */
				errno = ENOENT;
				return NULL;
			}

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): previous segment is only %u blocks",
							_mdfd_segpath(reln, forknum, nextsegno),
							blkno, nblocks)));
		}

		v = _mdfd_openseg(reln, forknum, nextsegno, flags);

		if (v == NULL)
		{
			if ((behavior & EXTENSION_RETURN_NULL) &&
				FILE_POSSIBLY_DELETED(errno))
				return NULL;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): %m",
							_mdfd_segpath(reln, forknum, nextsegno),
							blkno)));
		}
	}

	return v;
}

/*
 * 取得单个磁盘文件中现有的块数
 */
static BlockNumber
_mdnblocks(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	off_t		len;

	len = FileSize(seg->mdfd_vfd);
	if (len < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m",
						FilePathName(seg->mdfd_vfd))));
	/* 注意：这个计算会忽略 EOF 处的任何部分块 */
	return (BlockNumber) (len / BLCKSZ);
}

/*
 * 按给定的文件标签把文件同步到磁盘。把路径写进一个输出
 * 缓冲区，以便调用者在错误消息里使用。
 *
 * 成功返回 0，失败返回 -1，并设置 errno。
 */
int
mdsyncfiletag(const FileTag *ftag, char *path)
{
	SMgrRelation reln = smgropen(ftag->rlocator, InvalidBackendId);
	File		file;
	instr_time	io_start;
	bool		need_to_close;
	int			result,
				save_errno;

	/* 看文件是已经打开，还是需要打开。 */
	if (ftag->segno < reln->md_num_open_segs[ftag->forknum])
	{
		file = reln->md_seg_fds[ftag->forknum][ftag->segno].mdfd_vfd;
		strlcpy(path, FilePathName(file), MAXPGPATH);
		need_to_close = false;
	}
	else
	{
		char	   *p;

		p = _mdfd_segpath(reln, ftag->forknum, ftag->segno);
		strlcpy(path, p, MAXPGPATH);
		pfree(p);

		file = PathNameOpenFile(path, _mdfd_open_flags());
		if (file < 0)
			return -1;
		need_to_close = true;
	}

	io_start = pgstat_prepare_io_time();

	/* 同步该文件。 */
	result = FileSync(file, WAIT_EVENT_DATA_FILE_SYNC);
	save_errno = errno;

	if (need_to_close)
		FileClose(file);

	pgstat_count_io_op_time(IOOBJECT_RELATION, IOCONTEXT_NORMAL,
							IOOP_FSYNC, io_start, 1);

	errno = save_errno;
	return result;
}

/*
 * 按给定的文件标签 unlink 一个文件。把路径写进一个输出
 * 缓冲区，以便调用者在错误消息里使用。
 *
 * 成功返回 0，失败返回 -1，并设置 errno。
 */
int
mdunlinkfiletag(const FileTag *ftag, char *path)
{
	char	   *p;

	/* 计算路径。 */
	p = relpathperm(ftag->rlocator, MAIN_FORKNUM);
	strlcpy(path, p, MAXPGPATH);
	pfree(p);

	/* 尝试 unlink 该文件。 */
	return unlink(path);
}

/*
 * 处理 SYNC_FILTER_REQUEST 请求时，检查某个候选请求
 * 是否与给定标签匹配。会对所有待处理
 * 请求调用本函数，以决定是否忘掉它们。
 */
bool
mdfiletagmatches(const FileTag *ftag, const FileTag *candidate)
{
	/*
	 * 目前我们只用过滤请求作为一种手段，在删除数据库时丢掉
	 * 与给定数据库相关的全部已安排回调。
	 * 凡是数据库 OID 与 SYNC_FILTER_REQUEST 请求里的 ftag
	 * 相同的候选，我们都返回 true，于是它们被忘掉。
	 */
	return ftag->rlocator.dbOid == candidate->rlocator.dbOid;
}
