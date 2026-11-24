/*-------------------------------------------------------------------------
 *
 * sysv_shmem.c
 *	  用 SysV 设施实现共享内存
 *
 * 这些例程曾是 SysV 共享内存功能上相当薄的
 * 一层。加入匿名共享内存逻辑后变厚了些。我们仍
 * 要求有 SysV 共享内存块存在，因 mmap 的共享内存
 * 无法得知有多少进程附着，而联锁需要该信息。
 * 有多少进程附着，联锁需要该信息。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/sysv_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "portability/mem.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "utils/guc_hooks.h"
#include "utils/pidfile.h"


/*
 * 自 PostgreSQL 9.3 起，通常只分配很少量的
 * System V 共享内存，且仅用于提供保护数据目录的
 * 联锁。真正的共享内存块用 mmap() 分配。这绕开
 * 许多系统对可分配 System V 共享内存量限制极低
 * 的问题。哪怕只有几兆字节上限，也足以运行许多
 * 份 PostgreSQL 而无需调整系统设置。
 *
 *
 * 我们假定没人会在古老到不支持匿名共享内存的
 * 系统上跑 PostgreSQL 9.3 或更新版，例如 Linux
 * 2.4 之前。若假定不成立，或须在此加编译与/或
 * 运行时测试，仅当运行中的内核支持时才做。
 *
 *
 * 但在 EXEC_BACKEND 情形必须始终禁用此逻辑，并
 * 回退到用 System V 共享内存分配整段的旧方法，因
 * exec() 之后无法把匿名 mmap 段附着到进程。既然
 * EXEC_BACKEND 仅供开发者使用，这不该是大问题。
 * 因此，下文 EXEC_BACKEND 情形我们不操心支持
 * 匿名共享内存。
 *
 * 自 PostgreSQL 12 起，即便非 EXEC_BACKEND 构建，
 * 若 shared_memory_type 设为 sysv（非默认），也可
 * 用大型 System V 共享内存区。
 */


typedef key_t IpcMemoryKey;		/* 传给 shmget(2) 的共享内存键 */
typedef int IpcMemoryId;		/* shmget(2) 返回的共享内存 ID */

/*
 * 给定 IpcMemoryId 与本 PostgreSQL 进程有何关系？
 *
 * 若把不同数据目录的未附着段与其他 SHMSTATE_FOREIGN
 * 情形区分，或可回收它们。那样会少访问键空间，更难
 * 检出 SHMSTATE_ATTACHED 键。也会让并发分析更复杂：
 * 不同数据目录的 postmaster 可能同时试图回收同一键。
 * 某些情形会更久浪费键，但避免替代方案的问题足以
 * 证明这点损失合理。
 *
 */
typedef enum
{
	SHMSTATE_ANALYSIS_FAILURE,	/* 分析该 ID 时意外失败 */
	SHMSTATE_ATTACHED,			/* 与 DataDir 相关，有附着 PID */
	SHMSTATE_ENOENT,			/* 无该 ID 的段 */
	SHMSTATE_FOREIGN,			/* 存在，但与 DataDir 无关 */
	SHMSTATE_UNATTACHED			/* 与 DataDir 相关，无附着 PID */
} IpcMemoryState;


unsigned long UsedShmemSegID = 0;
void	   *UsedShmemSegAddr = NULL;

static Size AnonymousShmemSize;
static void *AnonymousShmem = NULL;

static void *InternalIpcMemoryCreate(IpcMemoryKey memKey, Size size);
static void IpcMemoryDetach(int status, Datum shmaddr);
static void IpcMemoryDelete(int status, Datum shmId);
static IpcMemoryState PGSharedMemoryAttach(IpcMemoryId shmId,
										   void *attachAt,
										   PGShmemHeader **addr);


/*
 *	InternalIpcMemoryCreate(memKey, size)
 *
 * 尝试用指定键创建新共享内存段。
 * 若该段已存在则失败（返回 NULL）。成功则把段
 * 附着到当前进程并返回附着地址。
 * 成功时向 on_shmem_exit 注册回调，以便在调用
 * on_shmem_exit 时脱离并删除该段。
 *
 * 若失败且失败码不是与已有段冲突，则打印错误
 * 并中止。其他类型错误不可恢复。
 */
static void *
InternalIpcMemoryCreate(IpcMemoryKey memKey, Size size)
{
	IpcMemoryId shmid;
	void	   *requestedAddress = NULL;
	void	   *memAddress;

	/*
	 * 通常把 requestedAddress = NULL 传给 shmat()，让
	 * 系统选择段映射位置。但在 EXEC_BACKEND 构建中，
	 * postmaster 所选地址可能因地址空间布局差异而不适
	 * 用于 backend。作为颇为笨拙的变通，允许用户通过
	 * 环境变量 PG_SHMEM_ADDR 指定地址。（若这不只是
	 * 调试用途，我们大概会提供更干净、文档更好的
	 * 设置方式，例如 GUC。）
	 *
	 */
#ifdef EXEC_BACKEND
	{
		char	   *pg_shmem_addr = getenv("PG_SHMEM_ADDR");

		if (pg_shmem_addr)
			requestedAddress = (void *) strtoul(pg_shmem_addr, NULL, 0);
		else
		{
#if defined(__darwin__) && SIZEOF_VOID_P == 8
			/*
			 * 提供据信可避免当前 macOS 发行版 ASLR
			 * 问题的默认值。
			 */
			requestedAddress = (void *) 0x80000000000;
#endif
		}
	}
#endif

	shmid = shmget(memKey, size, IPC_CREAT | IPC_EXCL | IPCProtection);

	if (shmid < 0)
	{
		int			shmget_errno = errno;

		/*
		 * 若错误表示与已有段冲突则安静失败。
		 * 既说了 IPC_EXCL，本预期 EEXIST，但或许会得
		 * 到权限违规？此外，若旧段已排定销毁但尚未
		 * 消失，也可能出现 EIDRM。
		 */
		if (shmget_errno == EEXIST || shmget_errno == EACCES
#ifdef EIDRM
			|| shmget_errno == EIDRM
#endif
			)
			return NULL;

		/*
		 * 某些 BSD 衍生内核在已有段但小于 "size" 时返回
		 * EINVAL 而非 EEXIST（错误测试顺序欠妥所致）。为
		 * 在此情形区分冲突与非法大小，用 size = 0
		 * 再试一次。这些内核在已有段情形下不对照 SHMMIN
		 * 测 size，故若确有此段，第二次不会再得 EINVAL。
		 *
		 *
		 */
		if (shmget_errno == EINVAL)
		{
			shmid = shmget(memKey, 0, IPC_CREAT | IPC_EXCL | IPCProtection);

			if (shmid < 0)
			{
				/* 同上，若核实为冲突则安静失败 */
				if (errno == EEXIST || errno == EACCES
#ifdef EIDRM
					|| errno == EIDRM
#endif
					)
					return NULL;
				/* 否则落入报告原始错误 */
			}
			else
			{
				/*
				 * 多数平台因 SHMMIN 大于零而到不了这里。
				 * 但若确实创建了零大小段，则释放它再落入
				 * 报告原始错误。
				 *
				 */
				if (shmctl(shmid, IPC_RMID, NULL) < 0)
					elog(LOG, "shmctl(%d, %d, 0) failed: %m",
						 (int) shmid, IPC_RMID);
			}
		}

		/*
		 * 否则抱怨并中止。
		 *
		 * 注意：此时 EINVAL 应表示违反了 SHMMIN 或 SHMMAX。
		 * SHMALL 违规可能报 ENOMEM（BSD）或 ENOSPC（Linux）；
		 * 单一 Unix 规范未说明该用哪个。
		 * SHMMNI 违规按规范是 ENOSPC。单纯内存不足是
		 * ENOMEM。
		 */
		errno = shmget_errno;
		ereport(FATAL,
				(errmsg("could not create shared memory segment: %m"),
				 errdetail("Failed system call was shmget(key=%lu, size=%zu, 0%o).",
						   (unsigned long) memKey, size,
						   IPC_CREAT | IPC_EXCL | IPCProtection),
				 (shmget_errno == EINVAL) ?
				 errhint("This error usually means that PostgreSQL's request for a shared memory "
						 "segment exceeded your kernel's SHMMAX parameter, or possibly that "
						 "it is less than "
						 "your kernel's SHMMIN parameter.\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.") : 0,
				 (shmget_errno == ENOMEM) ?
				 errhint("This error usually means that PostgreSQL's request for a shared "
						 "memory segment exceeded your kernel's SHMALL parameter.  You might need "
						 "to reconfigure the kernel with larger SHMALL.\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.") : 0,
				 (shmget_errno == ENOSPC) ?
				 errhint("This error does *not* mean that you have run out of disk space.  "
						 "It occurs either if all available shared memory IDs have been taken, "
						 "in which case you need to raise the SHMMNI parameter in your kernel, "
						 "or because the system's overall limit for shared memory has been "
						 "reached.\n"
						 "The PostgreSQL documentation contains more information about shared "
						 "memory configuration.") : 0));
	}

	/* 注册退出例程以删除新段 */
	on_shmem_exit(IpcMemoryDelete, Int32GetDatum(shmid));

	/* 好，应能附着到该段 */
	memAddress = shmat(shmid, requestedAddress, PG_SHMAT_FLAGS);

	if (memAddress == (void *) -1)
		elog(FATAL, "shmat(id=%d, addr=%p, flags=0x%x) failed: %m",
			 shmid, requestedAddress, PG_SHMAT_FLAGS);

	/* 注册退出例程以在删除前脱离新段 */
	on_shmem_exit(IpcMemoryDetach, PointerGetDatum(memAddress));

	/*
	 * 把共享内存键与 ID 存入数据目录锁文件。格式尽量
	 * 始终同长（锁文件尾部垃圾无害，或迷惑人）。
	 *
	 */
	{
		char		line[64];

		sprintf(line, "%9lu %9lu",
				(unsigned long) memKey, (unsigned long) shmid);
		AddToDataDirLockFile(LOCK_FILE_LINE_SHMEM_KEY, line);
	}

	return memAddress;
}

/****************************************************************************/
/*	IpcMemoryDetach(status, shmaddr)	removes a shared memory segment		*/
/*										from process' address space			*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDetach(int status, Datum shmaddr)
{
	/* 脱离 System V 共享内存块。 */
	if (shmdt((void *) DatumGetPointer(shmaddr)) < 0)
		elog(LOG, "shmdt(%p) failed: %m", DatumGetPointer(shmaddr));
}

/****************************************************************************/
/*	IpcMemoryDelete(status, shmId)		deletes a shared memory segment		*/
/*	(called as an on_shmem_exit callback, hence funny argument list)		*/
/****************************************************************************/
static void
IpcMemoryDelete(int status, Datum shmId)
{
	if (shmctl(DatumGetInt32(shmId), IPC_RMID, NULL) < 0)
		elog(LOG, "shmctl(%d, %d, 0) failed: %m",
			 DatumGetInt32(shmId), IPC_RMID);
}

/*
 * PGSharedMemoryIsInUse
 *
 * 先前存在的共享内存段是否仍存在且在使用？
 *
 * 用意是检出先前 postmaster 已崩溃、但仍有子
 * backend 在跑的情形。因此我们只关心与目标
 * DataDir 关联的共享内存段。这很重要，因共享内存
 * 段 ID 偶然匹配相当常见。
 *
 */
bool
PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
	PGShmemHeader *memAddress;
	IpcMemoryState state;

	state = PGSharedMemoryAttach((IpcMemoryId) id2, NULL, &memAddress);
	if (memAddress && shmdt((void *) memAddress) < 0)
		elog(LOG, "shmdt(%p) failed: %m", memAddress);
	switch (state)
	{
		case SHMSTATE_ENOENT:
		case SHMSTATE_FOREIGN:
		case SHMSTATE_UNATTACHED:
			return false;
		case SHMSTATE_ANALYSIS_FAILURE:
		case SHMSTATE_ATTACHED:
			return true;
	}
	return true;
}

/*
 * 测试 id 为 shmId 的段；见 IpcMemoryState 处注释。
 *
 * 若段存在，则尝试附着，若 attachAt 非 NULL 则用它
 * 但尽可能传 NULL 更好。
 *
 * *addr 在附着成功时设为段内存地址，否则为 NULL。
 */
static IpcMemoryState
PGSharedMemoryAttach(IpcMemoryId shmId,
					 void *attachAt,
					 PGShmemHeader **addr)
{
	struct shmid_ds shmStat;
	struct stat statbuf;
	PGShmemHeader *hdr;

	*addr = NULL;

	/*
	 * 先尝试 stat 该 shm 段 ID，看它是否根本存在。
	 */
	if (shmctl(shmId, IPC_STAT, &shmStat) < 0)
	{
		/*
		 * EINVAL 在 shmctl 手册中有多种可能原因，但我们
		 * 假定它一定表示段已不存在。
		 * exists.
		 */
		if (errno == EINVAL)
			return SHMSTATE_ENOENT;

		/*
		 * EACCES 表示无读权限，意味着不是 Postgres 共享内存
		 * 段（或至少与我们的数据目录无关）。
		 *
		 */
		if (errno == EACCES)
			return SHMSTATE_FOREIGN;

		/*
		 * 某些 Linux 内核版本（截至 2007 年 7 月全部）
		 * 有时在应为 EINVAL 时返回 EIDRM。Linux 内核无
		 * 可证返回 EIDRM 的内部状态，故假定该平台上
		 * EIDRM 等价于 EINVAL。
		 *
		 */
#ifdef HAVE_LINUX_EIDRM_BUG
		if (errno == EIDRM)
			return SHMSTATE_ENOENT;
#endif

		/*
		 * 否则最好假定该段仍在使用。唯一可能情形是非
		 * Linux、假定合规范的 EIDRM，表示段已
		 * IPC_RMID 但仍有进程附着。
		 *
		 */
		return SHMSTATE_ANALYSIS_FAILURE;
	}

	/*
	 * 尝试附着该段，看是否匹配我们的数据目录。
	 * 这可避免同一 userid 下跑多个 postmaster 的机器上
	 * 出现重复共享内存键冲突。
	 *
	 * 从 PGSharedMemoryCreate 调用时，此 stat 重复；
	 * 但非高频路径，不值得优化。）
	 *
	 */
	if (stat(DataDir, &statbuf) < 0)
		return SHMSTATE_ANALYSIS_FAILURE;	/* 无法 stat；保守处理 */

	hdr = (PGShmemHeader *) shmat(shmId, attachAt, PG_SHMAT_FLAGS);
	if (hdr == (PGShmemHeader *) -1)
	{
		/*
		 * 附着失败。关心的情形与上面 shmctl() 调用相同。
		 * 尤其注意：拥有方 postmaster 可能在 shmctl() 与
		 * shmat() 之间已终止并移除了该段。
		 *
		 *
		 * 若 attachAt 非 NULL，EINVAL 或反映该地址有问题
		 * 而非段已消失，故探测冲突段时最好传 NULL。
		 *
		 */
		if (errno == EINVAL)
			return SHMSTATE_ENOENT; /* 段已消失 */
		if (errno == EACCES)
			return SHMSTATE_FOREIGN;	/* 必为非 Postgres */
#ifdef HAVE_LINUX_EIDRM_BUG
		if (errno == EIDRM)
			return SHMSTATE_ENOENT; /* 段已消失 */
#endif
		/* 否则保守处理。 */
		return SHMSTATE_ANALYSIS_FAILURE;
	}
	*addr = hdr;

	if (hdr->magic != PGShmemMagic ||
		hdr->device != statbuf.st_dev ||
		hdr->inode != statbuf.st_ino)
	{
		/*
		 * 要么不是 Postgres 段，要么不是我的数据
		 * directory.
		 */
		return SHMSTATE_FOREIGN;
	}

	/*
	 * 它确实匹配我们的数据目录，故再测是否仍有进程
	 * 附着。（我们现在附着了，但 shm_nattch 结果来自
	 * 附着之前。）
	 */
	return shmStat.shm_nattch == 0 ? SHMSTATE_UNATTACHED : SHMSTATE_ATTACHED;
}

/*
 * 确定要使用的大页大小，并计算相关 mmap 标志。
 *
 * 某些 Linux 内核版本有 bug：mmap() 在请求不是大页
 * 大小倍数时失败。无该 bug 的版本则默默把请求向上
 * 取整到下一大大页倍数——然后给出不同大小时
 * munmap() 会失败。
 * 因此必须把请求向上取整为实际大页大小的倍数以免
 * 麻烦。
 *
 * 自己做向上取整也可利用额外内存，而非浪费。
 * 当前只增加共享内存头中记录的可用空间，使额外
 * 部分可用于例如更多锁表项等用途。将来对非常大的
 * 或可考虑更侵入的策略，例如增大 shared_buffers 来吸收
 * 额外空间。
 *
 *
 * 把（真实、假定或配置提供的）页大小返回到
 * *hugepagesize，若调用方请求则把大页相关 mmap
 * 标志返回到 *mmap_flags。若不支持大页，则
 * *hugepagesize 与 *mmap_flags 设为 0。
 */
void
GetHugePageSize(Size *hugepagesize, int *mmap_flags)
{
#ifdef MAP_HUGETLB

	Size		default_hugepagesize = 0;
	Size		hugepagesize_local = 0;
	int			mmap_flags_local = 0;

	/*
	 * 查明默认大页大小的系统相关代码。
	 *
	 * 在 Linux 上读 /proc/meminfo，查找形如 "Hugepagesize:
	 * nnnn kB" 的行。忽略任何失败，回退到预设默认。
	 */
#ifdef __linux__

	{
		FILE	   *fp = AllocateFile("/proc/meminfo", "r");
		char		buf[128];
		unsigned int sz;
		char		ch;

		if (fp)
		{
			while (fgets(buf, sizeof(buf), fp))
			{
				if (sscanf(buf, "Hugepagesize: %u %c", &sz, &ch) == 2)
				{
					if (ch == 'k')
					{
						default_hugepagesize = sz * (Size) 1024;
						break;
					}
					/* 若需要也可接受 kB 以外的单位 */
				}
			}
			FreeFile(fp);
		}
	}
#endif							/* __linux__ */

	if (huge_page_size != 0)
	{
		/* 若显式请求了大页大小，则用之。 */
		hugepagesize_local = (Size) huge_page_size * 1024;
	}
	else if (default_hugepagesize != 0)
	{
		/* 否则若有系统默认则用之。 */
		hugepagesize_local = default_hugepagesize;
	}
	else
	{
		/*
		 * 若未能查明系统默认大页大小，或未显式请求
		 * 大页大小，则假定为 2MB。实际更小时没问题。若
		 * 更大，可能因未对齐导致 mmap()/munmap() 失败；但
		 * 撰写时尚无非 Linux 系统对此挑剔的报告。
		 *
		 *
		 */
		hugepagesize_local = 2 * 1024 * 1024;
	}

	mmap_flags_local = MAP_HUGETLB;

	/*
	 * 在足够新的 Linux 上，若必要也含显式页大小。
	 * necessary.
	 */
#if defined(MAP_HUGE_MASK) && defined(MAP_HUGE_SHIFT)
	if (hugepagesize_local != default_hugepagesize)
	{
		int			shift = pg_ceil_log2_64(hugepagesize_local);

		mmap_flags_local |= (shift & MAP_HUGE_MASK) << MAP_HUGE_SHIFT;
	}
#endif

	/* 赋以所得结果 */
	if (mmap_flags)
		*mmap_flags = mmap_flags_local;
	if (hugepagesize)
		*hugepagesize = hugepagesize_local;

#else

	if (hugepagesize)
		*hugepagesize = 0;
	if (mmap_flags)
		*mmap_flags = 0;

#endif							/* MAP_HUGETLB */
}

/*
 * huge_page_size 的 GUC check_hook
 */
bool
check_huge_page_size(int *newval, void **extra, GucSource source)
{
#if !(defined(MAP_HUGE_MASK) && defined(MAP_HUGE_SHIFT))
	/* 目前仅足够新的 Linux。见 GetHugePageSize()。 */
	if (*newval != 0)
	{
		GUC_check_errdetail("huge_page_size must be 0 on this platform.");
		return false;
	}
#endif
	return true;
}

/*
 * 创建匿名 mmap() 的共享内存段。
 *
 * 在 *size 中传入请求大小。若最终分配大于请求，本函数
 * 会把 *size 改为实际分配大小。
 *
 */
static void *
CreateAnonymousSegment(Size *size)
{
	Size		allocsize = *size;
	void	   *ptr = MAP_FAILED;
	int			mmap_errno = 0;

#ifndef MAP_HUGETLB
	/* PGSharedMemoryCreate 本应已处理此情形 */
	Assert(huge_pages != HUGE_PAGES_ON);
#else
	if (huge_pages == HUGE_PAGES_ON || huge_pages == HUGE_PAGES_TRY)
	{
		/*
		 * 把请求大小向上取整到合适的较大值。
		 */
		Size		hugepagesize;
		int			mmap_flags;

		GetHugePageSize(&hugepagesize, &mmap_flags);

		if (allocsize % hugepagesize != 0)
			allocsize += hugepagesize - (allocsize % hugepagesize);

		ptr = mmap(NULL, allocsize, PROT_READ | PROT_WRITE,
				   PG_MMAP_FLAGS | mmap_flags, -1, 0);
		mmap_errno = errno;
		if (huge_pages == HUGE_PAGES_TRY && ptr == MAP_FAILED)
			elog(DEBUG1, "mmap(%zu) with MAP_HUGETLB failed, huge pages disabled: %m",
				 allocsize);
	}
#endif

	if (ptr == MAP_FAILED && huge_pages != HUGE_PAGES_ON)
	{
		/*
		 * 回退到非大页时用原始大小，非取整值。
		 *
		 */
		allocsize = *size;
		ptr = mmap(NULL, allocsize, PROT_READ | PROT_WRITE,
				   PG_MMAP_FLAGS, -1, 0);
		mmap_errno = errno;
	}

	if (ptr == MAP_FAILED)
	{
		errno = mmap_errno;
		ereport(FATAL,
				(errmsg("could not map anonymous shared memory: %m"),
				 (mmap_errno == ENOMEM) ?
				 errhint("This error usually means that PostgreSQL's request "
						 "for a shared memory segment exceeded available memory, "
						 "swap space, or huge pages. To reduce the request size "
						 "(currently %zu bytes), reduce PostgreSQL's shared "
						 "memory usage, perhaps by reducing shared_buffers or "
						 "max_connections.",
						 allocsize) : 0));
	}

	*size = allocsize;
	return ptr;
}

/*
 * AnonymousShmemDetach --- 脱离匿名 mmap 块
 * 注：作为 on_shmem_exit 回调，故参数列表古怪
 */
static void
AnonymousShmemDetach(int status, Datum arg)
{
	/* 若有则释放匿名共享内存块。 */
	if (AnonymousShmem != NULL)
	{
		if (munmap(AnonymousShmem, AnonymousShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 AnonymousShmem, AnonymousShmemSize);
		AnonymousShmem = NULL;
	}
}

/*
 * PGSharedMemoryCreate
 *
 * 创建给定大小的共享内存段并初始化其标准头。
 * 并注册 on_shmem_exit 回调以释放存储。
 *
 *
 * 若发现与本 DataDir 相关的已死 Postgres 段则回收，
 * 但与外来共享内存段冲突时不失败。用意是检出并
 * 重用可能由崩溃的 postmaster 或 backend 分配的键。
 *
 */
PGShmemHeader *
PGSharedMemoryCreate(Size size,
					 PGShmemHeader **shim)
{
	IpcMemoryKey NextShmemSegID;
	void	   *memAddress;
	PGShmemHeader *hdr;
	struct stat statbuf;
	Size		sysvsize;

	/*
	 * 用数据目录的 ID 信息（inode 与设备号）来识别
	 * 与本数据目录关联的共享内存段，并作为搜索空闲
	 * 共享内存键的种子。
	 */
	if (stat(DataDir, &statbuf) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not stat data directory \"%s\": %m",
						DataDir)));

	/* 若要求大页但完全无法支持则抱怨 */
#if !defined(MAP_HUGETLB)
	if (huge_pages == HUGE_PAGES_ON)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("huge pages not supported on this platform")));
#endif

	/* 目前 SysV 内存不支持大页 */
	if (huge_pages == HUGE_PAGES_ON && shared_memory_type != SHMEM_TYPE_MMAP)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("huge pages not supported with the current shared_memory_type setting")));

	/* 有头空间？ */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	if (shared_memory_type == SHMEM_TYPE_MMAP)
	{
		AnonymousShmem = CreateAnonymousSegment(&size);
		AnonymousShmemSize = size;

		/* 注册退出例程以取消映射匿名段 */
		on_shmem_exit(AnonymousShmemDetach, (Datum) 0);

		/* 现在只需分配最小尺寸的 SysV 共享内存块。 */
		sysvsize = sizeof(PGShmemHeader);
	}
	else
		sysvsize = size;

	/*
	 * 循环直至找到空闲 IPC 键。信任 CreateDataDirLockFile()
	 * 确保每个数据目录最多一个 postmaster 能同时进入此
	 * 循环。（CreateDataDirLockFile() 并未完全保证如此，
	 * 但宁可修它也不在此凑合。）
	 */
	NextShmemSegID = statbuf.st_ino;

	for (;;)
	{
		IpcMemoryId shmid;
		PGShmemHeader *oldhdr;
		IpcMemoryState state;

		/* 尝试创建新段 */
		memAddress = InternalIpcMemoryCreate(NextShmemSegID, sysvsize);
		if (memAddress)
			break;				/* 创建并附着成功 */

		/* 检查共享内存，可能移除并重建 */

		/*
		 * shmget() 失败通常是 EACCES，故为 SHMSTATE_FOREIGN。
		 * ENOENT（窄可能）意味着 SHMSTATE_ENOENT，但可安全
		 * 把 SHMSTATE_ENOENT 当作 SHMSTATE_FOREIGN。
		 */
		shmid = shmget(NextShmemSegID, sizeof(PGShmemHeader), 0);
		if (shmid < 0)
		{
			oldhdr = NULL;
			state = SHMSTATE_FOREIGN;
		}
		else
			state = PGSharedMemoryAttach(shmid, NULL, &oldhdr);

		switch (state)
		{
			case SHMSTATE_ANALYSIS_FAILURE:
			case SHMSTATE_ATTACHED:
				ereport(FATAL,
						(errcode(ERRCODE_LOCK_FILE_EXISTS),
						 errmsg("pre-existing shared memory block (key %lu, ID %lu) is still in use",
								(unsigned long) NextShmemSegID,
								(unsigned long) shmid),
						 errhint("Terminate any old server processes associated with data directory \"%s\".",
								 DataDir)));
				break;
			case SHMSTATE_ENOENT:

				/*
				 * 出乎意料，自上次 InternalIpcMemoryCreate()
				 * 以来另有进程已删除。片刻前本会看见
				 * SHMSTATE_FOREIGN。再试同一 ID。
				 */
				elog(LOG,
					 "shared memory block (key %lu, ID %lu) deleted during startup",
					 (unsigned long) NextShmemSegID,
					 (unsigned long) shmid);
				break;
			case SHMSTATE_FOREIGN:
				NextShmemSegID++;
				break;
			case SHMSTATE_UNATTACHED:

				/*
				 * 该段属 DataDir，曾用它的进程都已死或脱离。
				 * 尽可能干掉它及关联动态共享内存段。不该
				 * 失败；若失败则假定属别人，试下一候选。
				 * 否则再试创建。若其他进程抢先用同一键
				 * 创建，则可能失败，那时试下一键。
				 *
				 *
				 *
				 */
				if (oldhdr->dsm_control != 0)
					dsm_cleanup_using_control_segment(oldhdr->dsm_control);
				if (shmctl(shmid, IPC_RMID, NULL) < 0)
					NextShmemSegID++;
				break;
		}

		if (oldhdr && shmdt((void *) oldhdr) < 0)
			elog(LOG, "shmdt(%p) failed: %m", oldhdr);
	}

	/* 初始化新段。 */
	hdr = (PGShmemHeader *) memAddress;
	hdr->creatorPID = getpid();
	hdr->magic = PGShmemMagic;
	hdr->dsm_control = 0;

	/* 也填入数据目录 ID 信息 */
	hdr->device = statbuf.st_dev;
	hdr->inode = statbuf.st_ino;

	/*
	 * 初始化该段的空间分配状态。
	 */
	hdr->totalsize = size;
	hdr->freeoffset = MAXALIGN(sizeof(PGShmemHeader));
	*shim = hdr;

	/* 保存信息以备将来使用 */
	UsedShmemSegAddr = memAddress;
	UsedShmemSegID = (unsigned long) NextShmemSegID;

	/*
	 * 若此处 AnonymousShmem 为 NULL，则未用匿名共享
	 * 内存，应返回指向 System V 共享内存块的指针。
	 * 否则 System V 块只是垫片，须返回真正块指针。
	 *
	 */
	if (AnonymousShmem == NULL)
		return hdr;
	memcpy(AnonymousShmem, hdr, sizeof(PGShmemHeader));
	return (PGShmemHeader *) AnonymousShmem;
}

#ifdef EXEC_BACKEND

/*
 * PGSharedMemoryReAttach
 *
 * 在 postmaster 子进程启动时调用，以重新附着已有共享内存
 * 段。仅 EXEC_BACKEND 情形需要；否则 postmaster 子进程经
 * fork() 继承共享内存段附着。
 *
 *
 * UsedShmemSegID 与 UsedShmemSegAddr 是本例程隐式参数。
 * 调用方须已把它们恢复为 postmaster 的值。
 *
 */
void
PGSharedMemoryReAttach(void)
{
	IpcMemoryId shmid;
	PGShmemHeader *hdr;
	IpcMemoryState state;
	void	   *origUsedShmemSegAddr = UsedShmemSegAddr;

	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

#ifdef __CYGWIN__
	/* cygipc（目前）似乎在 exec 时不脱离。 */
	PGSharedMemoryDetach();
	UsedShmemSegAddr = origUsedShmemSegAddr;
#endif

	elog(DEBUG3, "attaching to %p", UsedShmemSegAddr);
	shmid = shmget(UsedShmemSegID, sizeof(PGShmemHeader), 0);
	if (shmid < 0)
		state = SHMSTATE_FOREIGN;
	else
		state = PGSharedMemoryAttach(shmid, UsedShmemSegAddr, &hdr);
	if (state != SHMSTATE_ATTACHED)
		elog(FATAL, "could not reattach to shared memory (key=%d, addr=%p): %m",
			 (int) UsedShmemSegID, UsedShmemSegAddr);
	if (hdr != origUsedShmemSegAddr)
		elog(FATAL, "reattaching to shared memory returned unexpected address (got %p, expected %p)",
			 hdr, origUsedShmemSegAddr);
	dsm_set_control_handle(hdr->dsm_control);

	UsedShmemSegAddr = hdr;		/* 大概多余 */
}

/*
 * PGSharedMemoryNoReAttach
 *
 * 在 postmaster 子进程启动时，当选择 *不* 重新附着
 * 已有共享内存段时调用。须清理以使状态正确。非
 * EXEC_BACKEND 情形也不使用。
 *
 *
 * 子进程启动逻辑此后或会调用 PGSharedMemoryDetach；
 * 确保若调用则是空操作。
 *
 * UsedShmemSegID 与 UsedShmemSegAddr 是本例程隐式参数。
 * 调用方须已把它们恢复为 postmaster 的值。
 *
 */
void
PGSharedMemoryNoReAttach(void)
{
	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

#ifdef __CYGWIN__
	/* cygipc（目前）似乎在 exec 时不脱离。 */
	PGSharedMemoryDetach();
#endif

	/* 为整洁起见，复位 UsedShmemSegAddr 以示未附着。 */
	UsedShmemSegAddr = NULL;
	/* UsedShmemSegID 同样处理。 */
	UsedShmemSegID = 0;
}

#endif							/* EXEC_BACKEND */

/*
 * PGSharedMemoryDetach
 *
 * 若仍附着则脱离共享内存段。本意不是由原先创建
 * 该段的进程显式调用（它会注册 on_shmem_exit 回调）。
 * 而是给继承了附着并想去掉它的子进程用。
 *
 *
 *
 * UsedShmemSegID 与 UsedShmemSegAddr 是本例程隐式参数，
 * 另有 AnonymousShmem 与 AnonymousShmemSize。
 */
void
PGSharedMemoryDetach(void)
{
	if (UsedShmemSegAddr != NULL)
	{
		if ((shmdt(UsedShmemSegAddr) < 0)
#if defined(EXEC_BACKEND) && defined(__CYGWIN__)
		/* cygipc 执行缺陷的变通 */
			&& shmdt(NULL) < 0
#endif
			)
			elog(LOG, "shmdt(%p) failed: %m", UsedShmemSegAddr);
		UsedShmemSegAddr = NULL;
	}

	if (AnonymousShmem != NULL)
	{
		if (munmap(AnonymousShmem, AnonymousShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 AnonymousShmem, AnonymousShmemSize);
		AnonymousShmem = NULL;
	}
}
