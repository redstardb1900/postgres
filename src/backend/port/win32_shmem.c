/*-------------------------------------------------------------------------
 *
 * win32_shmem.c
 *	  用 win32 设施实现共享内存
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "utils/guc_hooks.h"


/*
 * 进程生命早期，Windows 会异步为进程的"默认线程池"
 * 创建线程
 * (https://docs.microsoft.com/en-us/windows/desktop/ProcThread/thread-pools).
 * 偶尔，线程创建会在其后分配栈：
 * PGSharedMemoryReAttach() 已释放 UsedShmemSegAddr，且尚未
 * 把共享内存映射到 UsedShmemSegAddr。若分配器偏爱刚释放
 * 区域来放新线程栈，映射就会失败。我们在某些 Windows
 * Server 2016 配置上观察到此类失败。为给系统另一
 * 偏爱区域，在保留或释放共享内存前，先保留并释放
 * 额外保护区域。想法是：若分配器某次先给出 REGION1
 * 页再给 REGION2 页，则两区皆空时也会如此。Windows
 * Server 2016 表现如此；行为不同的系统对保护
 * UsedShmemSegAddr 的需求更小。保护区域至少
 * 须够一个线程栈。不过十倍于栈仍不到 32 位
 * 地址空间的 2%，相对 64 位地址空间可忽略。
 *
 *
 */
#define PROTECTIVE_REGION_SIZE (10 * WIN32_STACK_RLIMIT)
void	   *ShmemProtectiveRegion = NULL;

HANDLE		UsedShmemSegID = INVALID_HANDLE_VALUE;
void	   *UsedShmemSegAddr = NULL;
static Size UsedShmemSegSize = 0;

static bool EnableLockPagesPrivilege(int elevel);
static void pgwin32_SharedMemoryDelete(int status, Datum shmId);

/*
 * 生成共享内存段名。展开数据目录以生成对该数据
 * 目录唯一的标识。再把反斜杠换成正斜杠，因全局
 * 对象名不允许反斜杠。
 *
 * 把共享内存段放在 Global\ 命名空间（需 NT2 TSE 或
 * 2000，但我们因其他原因也只支持这些），以确保不能
 * 在不同会话对同一数据目录开两个 postmaster。
 *
 * XXX: junction 会怎样？只是有人故意搞破坏，
 *		且这仍好于从前，但将来或许要对
 *		此做些处理。
 */
static char *
GetSharedMemName(void)
{
	char	   *retptr;
	DWORD		bufsize;
	DWORD		r;
	char	   *cp;

	bufsize = GetFullPathName(DataDir, 0, NULL, NULL);
	if (bufsize == 0)
		elog(FATAL, "could not get size for full pathname of datadir %s: error code %lu",
			 DataDir, GetLastError());

	retptr = malloc(bufsize + 18);	/* Global\PostgreSQL: 占 18 */
	if (retptr == NULL)
		elog(FATAL, "could not allocate memory for shared memory name");

	strcpy(retptr, "Global\\PostgreSQL:");
	r = GetFullPathName(DataDir, bufsize, retptr + 18, NULL);
	if (r == 0 || r > bufsize)
		elog(FATAL, "could not generate full pathname for datadir %s: error code %lu",
			 DataDir, GetLastError());

	/*
	 * XXX: 故意在此覆盖 Global\ 部分。原先不是此
	 * 做法，但放进真正的 Global\ 命名空间在很多
	 * 情况下会权限错误，故暂留默认命名空间。
	 *
	 */
	for (cp = retptr; *cp; cp++)
		if (*cp == '\\')
			*cp = '/';

	return retptr;
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
	char	   *szShareMem;
	HANDLE		hmap;

	szShareMem = GetSharedMemName();

	hmap = OpenFileMapping(FILE_MAP_READ, FALSE, szShareMem);

	free(szShareMem);

	if (hmap == NULL)
		return false;

	CloseHandle(hmap);
	return true;
}

/*
 * EnableLockPagesPrivilege
 *
 * 尝试获取 SeLockMemoryPrivilege 以便使用大页。
 */
static bool
EnableLockPagesPrivilege(int elevel)
{
	HANDLE		hToken;
	TOKEN_PRIVILEGES tp;
	LUID		luid;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
	{
		ereport(elevel,
				(errmsg("could not enable user right \"%s\": error code %lu",

		/*
		 * translator: This is a term from Windows and should be translated to
		 * match the Windows localization.
		 */
						_("Lock pages in memory"),
						GetLastError()),
				 errdetail("Failed system call was %s.", "OpenProcessToken")));
		return FALSE;
	}

	if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &luid))
	{
		ereport(elevel,
				(errmsg("could not enable user right \"%s\": error code %lu", _("Lock pages in memory"), GetLastError()),
				 errdetail("Failed system call was %s.", "LookupPrivilegeValue")));
		CloseHandle(hToken);
		return FALSE;
	}
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL))
	{
		ereport(elevel,
				(errmsg("could not enable user right \"%s\": error code %lu", _("Lock pages in memory"), GetLastError()),
				 errdetail("Failed system call was %s.", "AdjustTokenPrivileges")));
		CloseHandle(hToken);
		return FALSE;
	}

	if (GetLastError() != ERROR_SUCCESS)
	{
		if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
			ereport(elevel,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("could not enable user right \"%s\"", _("Lock pages in memory")),
					 errhint("Assign user right \"%s\" to the Windows user account which runs PostgreSQL.",
							 _("Lock pages in memory"))));
		else
			ereport(elevel,
					(errmsg("could not enable user right \"%s\": error code %lu", _("Lock pages in memory"), GetLastError()),
					 errdetail("Failed system call was %s.", "AdjustTokenPrivileges")));
		CloseHandle(hToken);
		return FALSE;
	}

	CloseHandle(hToken);

	return TRUE;
}

/*
 * PGSharedMemoryCreate
 *
 * 创建给定大小的共享内存段并初始化其
 * 标准头。
 */
PGShmemHeader *
PGSharedMemoryCreate(Size size,
					 PGShmemHeader **shim)
{
	void	   *memAddress;
	PGShmemHeader *hdr;
	HANDLE		hmap,
				hmap2;
	char	   *szShareMem;
	int			i;
	DWORD		size_high;
	DWORD		size_low;
	SIZE_T		largePageSize = 0;
	Size		orig_size = size;
	DWORD		flProtect = PAGE_READWRITE;
	DWORD		desiredAccess;

	ShmemProtectiveRegion = VirtualAlloc(NULL, PROTECTIVE_REGION_SIZE,
										 MEM_RESERVE, PAGE_NOACCESS);
	if (ShmemProtectiveRegion == NULL)
		elog(FATAL, "could not reserve memory region: error code %lu",
			 GetLastError());

	/* 有头空间？ */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

	szShareMem = GetSharedMemName();

	UsedShmemSegAddr = NULL;

	if (huge_pages == HUGE_PAGES_ON || huge_pages == HUGE_PAGES_TRY)
	{
		/* 处理器支持大页吗？ */
		largePageSize = GetLargePageMinimum();
		if (largePageSize == 0)
		{
			ereport(huge_pages == HUGE_PAGES_ON ? FATAL : DEBUG1,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("the processor does not support large pages")));
			ereport(DEBUG1,
					(errmsg_internal("disabling huge pages")));
		}
		else if (!EnableLockPagesPrivilege(huge_pages == HUGE_PAGES_ON ? FATAL : DEBUG1))
		{
			ereport(DEBUG1,
					(errmsg_internal("disabling huge pages")));
		}
		else
		{
			/* 大页可用且特权已启用，故打开 */
			flProtect = PAGE_READWRITE | SEC_COMMIT | SEC_LARGE_PAGES;

			/* 按需向上取整大小。 */
			if (size % largePageSize != 0)
				size += largePageSize - (size % largePageSize);
		}
	}

retry:
#ifdef _WIN64
	size_high = size >> 32;
#else
	size_high = 0;
#endif
	size_low = (DWORD) size;

	/*
	 * 回收共享内存段时，从全局命名空间消失可能要
	 * 一小会儿。故睡眠一秒后重试，并重试 10 次。
	 * 注：1 秒间隔与 10 次重试都完全任意。
	 *
	 */
	for (i = 0; i < 10; i++)
	{
		/*
		 * 以防 CreateFileMapping() 成功时未把错误码设为 0
		 *
		 */
		SetLastError(0);

		hmap = CreateFileMapping(INVALID_HANDLE_VALUE,	/* 用页面文件 */
								 NULL,	/* 默认安全属性 */
								 flProtect,
								 size_high, /* 大小高 32 位	*/
								 size_low,	/* 大小低 32 位 */
								 szShareMem);

		if (!hmap)
		{
			if (GetLastError() == ERROR_NO_SYSTEM_RESOURCES &&
				huge_pages == HUGE_PAGES_TRY &&
				(flProtect & SEC_LARGE_PAGES) != 0)
			{
				elog(DEBUG1, "CreateFileMapping(%zu) with SEC_LARGE_PAGES failed, "
					 "huge pages disabled",
					 size);

				/*
				 * 回退到非大页时，用原始大小而非
				 * 向上取整后的值。
				 */
				size = orig_size;
				flProtect = PAGE_READWRITE;
				goto retry;
			}
			else
				ereport(FATAL,
						(errmsg("could not create shared memory segment: error code %lu", GetLastError()),
						 errdetail("Failed system call was CreateFileMapping(size=%zu, name=%s).",
								   size, szShareMem)));
		}

		/*
		 * 若段已存在，CreateFileMapping() 会返回已有
		 * 段的句柄并设置 ERROR_ALREADY_EXISTS。
		 */
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(hmap);	/* 关闭句柄，因拿到了指向先前段
								 * 的有效句柄。 */
			hmap = NULL;
			Sleep(1000);
			continue;
		}
		break;
	}

	/*
	 * 若循环最后一次仍返回 ERROR_ALREADY_EXISTS，则此
	 * 共享内存段存在，并假定属于别人。
	 */
	if (!hmap)
		ereport(FATAL,
				(errmsg("pre-existing shared memory block is still in use"),
				 errhint("Check if there are any old server processes still running, and terminate them.")));

	free(szShareMem);

	/*
	 * 使句柄可继承
	 */
	if (!DuplicateHandle(GetCurrentProcess(), hmap, GetCurrentProcess(), &hmap2, 0, TRUE, DUPLICATE_SAME_ACCESS))
		ereport(FATAL,
				(errmsg("could not create shared memory segment: error code %lu", GetLastError()),
				 errdetail("Failed system call was DuplicateHandle.")));

	/*
	 * 关闭旧的不可继承句柄。若失败我们并不在意
	 *
	 */
	if (!CloseHandle(hmap))
		elog(LOG, "could not close handle to shared memory: error code %lu", GetLastError());

	desiredAccess = FILE_MAP_WRITE | FILE_MAP_READ;

#ifdef FILE_MAP_LARGE_PAGES
	/* 若需要则设大页。 */
	if ((flProtect & SEC_LARGE_PAGES) != 0)
		desiredAccess |= FILE_MAP_LARGE_PAGES;
#endif

	/*
	 * 取得新共享内存段指针。一次映射整段，
	 * 并让系统决定初始地址。
	 */
	memAddress = MapViewOfFileEx(hmap2, desiredAccess, 0, 0, 0, NULL);
	if (!memAddress)
		ereport(FATAL,
				(errmsg("could not create shared memory segment: error code %lu", GetLastError()),
				 errdetail("Failed system call was MapViewOfFileEx.")));



	/*
	 * 好，已创建新段。标记为本进程所创。赋值顺序
	 * 关键，以免其他 Postgres 进程看见头有效但
	 * 属于无效 PID！
	 */
	hdr = (PGShmemHeader *) memAddress;
	hdr->creatorPID = getpid();
	hdr->magic = PGShmemMagic;

	/*
	 * 初始化该段的空间分配状态。
	 */
	hdr->totalsize = size;
	hdr->freeoffset = MAXALIGN(sizeof(PGShmemHeader));
	hdr->dsm_control = 0;

	/* 保存信息以备将来使用 */
	UsedShmemSegAddr = memAddress;
	UsedShmemSegSize = size;
	UsedShmemSegID = hmap2;

	/* 注册退出例程以删除新段 */
	on_shmem_exit(pgwin32_SharedMemoryDelete, PointerGetDatum(hmap2));

	*shim = hdr;
	return hdr;
}

/*
 * PGSharedMemoryReAttach
 *
 * 在 postmaster 子进程启动时调用，以用从 postmaster
 * 继承的句柄重新附着已有共享内存段。
 *
 *
 * ShmemProtectiveRegion、UsedShmemSegID 与 UsedShmemSegAddr
 * 是本例程隐式参数。调用方须已把它们恢复为
 * postmaster 的值。
 */
void
PGSharedMemoryReAttach(void)
{
	PGShmemHeader *hdr;
	void	   *origUsedShmemSegAddr = UsedShmemSegAddr;

	Assert(ShmemProtectiveRegion != NULL);
	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

	/*
	 * 释放 postmaster 所做的内存区域保留
	 */
	if (VirtualFree(ShmemProtectiveRegion, 0, MEM_RELEASE) == 0)
		elog(FATAL, "failed to release reserved memory region (addr=%p): error code %lu",
			 ShmemProtectiveRegion, GetLastError());
	if (VirtualFree(UsedShmemSegAddr, 0, MEM_RELEASE) == 0)
		elog(FATAL, "failed to release reserved memory region (addr=%p): error code %lu",
			 UsedShmemSegAddr, GetLastError());

	hdr = (PGShmemHeader *) MapViewOfFileEx(UsedShmemSegID, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0, UsedShmemSegAddr);
	if (!hdr)
		elog(FATAL, "could not reattach to shared memory (key=%p, addr=%p): error code %lu",
			 UsedShmemSegID, UsedShmemSegAddr, GetLastError());
	if (hdr != origUsedShmemSegAddr)
		elog(FATAL, "reattaching to shared memory returned unexpected address (got %p, expected %p)",
			 hdr, origUsedShmemSegAddr);
	if (hdr->magic != PGShmemMagic)
		elog(FATAL, "reattaching to shared memory returned non-PostgreSQL memory");
	dsm_set_control_handle(hdr->dsm_control);

	UsedShmemSegAddr = hdr;		/* 大概多余 */
}

/*
 * PGSharedMemoryNoReAttach
 *
 * 在 postmaster 子进程启动时，当我们选择
 * *不*重新附着已有共享内存段时调用。须清理
 * 以使状态正确。
 *
 * 子进程启动逻辑此后或会调用 PGSharedMemoryDetach；
 * 确保若调用则是空操作。
 *
 * ShmemProtectiveRegion、UsedShmemSegID 与 UsedShmemSegAddr
 * 是本例程隐式参数。调用方须已把它们恢复为
 * postmaster 的值。
 */
void
PGSharedMemoryNoReAttach(void)
{
	Assert(ShmemProtectiveRegion != NULL);
	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

	/*
	 * Windows 下我们尚未映射该段，故不必取消映射。
	 * 只需复位 UsedShmemSegAddr 以示未附着。
	 */
	UsedShmemSegAddr = NULL;

	/*
	 * 必须关闭继承的共享内存段句柄，否则 Windows 会
	 * 认为本进程存在即不能释放该段。现在可用
	 * PGSharedMemoryDetach 来做。
	 */
	PGSharedMemoryDetach();
}

/*
 * PGSharedMemoryDetach
 *
 * 若仍附着则脱离共享内存段。本意不是由原先创建该
 * 段的进程显式调用（它会注册 on_shmem_exit 回调
 * 来做）。而是给继承了附着并想去掉它的子进程用。
 *
 *
 *
 * ShmemProtectiveRegion、UsedShmemSegID 与 UsedShmemSegAddr
 * 是本例程隐式参数。
 */
void
PGSharedMemoryDetach(void)
{
	/*
	 * 释放保护区域可解放少量无关紧要的地址空间，
	 * 但保持整洁。
	 */
	if (ShmemProtectiveRegion != NULL)
	{
		if (VirtualFree(ShmemProtectiveRegion, 0, MEM_RELEASE) == 0)
			elog(LOG, "failed to release reserved memory region (addr=%p): error code %lu",
				 ShmemProtectiveRegion, GetLastError());

		ShmemProtectiveRegion = NULL;
	}

	/* 若已映射则取消映射 */
	if (UsedShmemSegAddr != NULL)
	{
		if (!UnmapViewOfFile(UsedShmemSegAddr))
			elog(LOG, "could not unmap view of shared memory: error code %lu",
				 GetLastError());

		UsedShmemSegAddr = NULL;
	}

	/* 若有共享内存句柄则关闭之 */
	if (UsedShmemSegID != INVALID_HANDLE_VALUE)
	{
		if (!CloseHandle(UsedShmemSegID))
			elog(LOG, "could not close handle to shared memory: error code %lu",
				 GetLastError());

		UsedShmemSegID = INVALID_HANDLE_VALUE;
	}
}


/*
 * pgwin32_SharedMemoryDelete
 *
 * 脱离并删除共享内存段
 * 注：作为 on_shmem_exit 回调，故参数列表古怪
 */
static void
pgwin32_SharedMemoryDelete(int status, Datum shmId)
{
	Assert(DatumGetPointer(shmId) == UsedShmemSegID);
	PGSharedMemoryDetach();
}

/*
 * pgwin32_ReserveSharedMemoryRegion(hChild)
 *
 * 为子进程保留将用于共享内存的内存区域。在子进程
 * 启动前调用，以确保内存可用。
 *
 *
 * 子进程一旦启动，DLL 加载顺序不同或线程调度
 * 不同可能分配与我们共享内存所需地址冲突的
 * 内存。在子进程启动前保留共享内存区域，仅在
 * 试图访问共享内存前才释放，可迫使这些分配
 *
	 * 使用不冲突的其他地址范围。
 *
 * 注意！本函数在 postmaster 中执行，因此不应
 * 用 elog(FATAL)，以免弄垮 postmaster。
 */
int
pgwin32_ReserveSharedMemoryRegion(HANDLE hChild)
{
	void	   *address;

	Assert(ShmemProtectiveRegion != NULL);
	Assert(UsedShmemSegAddr != NULL);
	Assert(UsedShmemSegSize != 0);

	/* ShmemProtectiveRegion */
	address = VirtualAllocEx(hChild, ShmemProtectiveRegion,
							 PROTECTIVE_REGION_SIZE,
							 MEM_RESERVE, PAGE_NOACCESS);
	if (address == NULL)
	{
		/* 不用 FATAL，因我们在 postmaster 中运行 */
		elog(LOG, "could not reserve shared memory region (addr=%p) for child %p: error code %lu",
			 ShmemProtectiveRegion, hChild, GetLastError());
		return false;
	}
	if (address != ShmemProtectiveRegion)
	{
		/*
		 * 不应发生——理论上若分配粒度导致奇怪效果
		 * 也可能，故仍检查以防万一。
		 *
		 * 不用 FATAL，因我们在 postmaster 中运行。
		 */
		elog(LOG, "reserved shared memory region got incorrect address %p, expected %p",
			 address, ShmemProtectiveRegion);
		return false;
	}

	/* UsedShmemSegAddr */
	address = VirtualAllocEx(hChild, UsedShmemSegAddr, UsedShmemSegSize,
							 MEM_RESERVE, PAGE_READWRITE);
	if (address == NULL)
	{
		elog(LOG, "could not reserve shared memory region (addr=%p) for child %p: error code %lu",
			 UsedShmemSegAddr, hChild, GetLastError());
		return false;
	}
	if (address != UsedShmemSegAddr)
	{
		elog(LOG, "reserved shared memory region got incorrect address %p, expected %p",
			 address, UsedShmemSegAddr);
		return false;
	}

	return true;
}

/*
 * 为与 sysv_shmem.c 一致而提供，对 Windows 不提供
 * 任何有用信息。要取得大页大小，请改用
 * GetLargePageMinimum()。
 */
void
GetHugePageSize(Size *hugepagesize, int *mmap_flags)
{
	if (hugepagesize)
		*hugepagesize = 0;
	if (mmap_flags)
		*mmap_flags = 0;
}

/*
 * huge_page_size 的 GUC check_hook
 */
bool
check_huge_page_size(int *newval, void **extra, GucSource source)
{
	if (*newval != 0)
	{
		GUC_check_errdetail("huge_page_size must be 0 on this platform.");
		return false;
	}
	return true;
}
