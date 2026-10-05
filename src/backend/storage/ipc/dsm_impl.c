/*-------------------------------------------------------------------------
 *
 * dsm_impl.c
 *	  manage dynamic shared memory segments
 *
 * This file provides low-level APIs for creating and destroying shared
 * memory segments using several different possible techniques.  We refer
 * to these segments as dynamic because they can be created, altered, and
 * destroyed at any point during the server life cycle.  This is unlike
 * the main shared memory segment, of which there is always exactly one
 * and which is always mapped at a fixed address in every PostgreSQL
 * background process.
 *
 * Because not all systems provide the same primitives in this area, nor
 * do all primitives behave the same way on all systems, we provide
 * several implementations of this facility.  Many systems implement
 * POSIX shared memory (shm_open etc.), which is well-suited to our needs
 * in this area, with the exception that shared memory identifiers live
 * in a flat system-wide namespace, raising the uncomfortable prospect of
 * name collisions with other processes (including other copies of
 * PostgreSQL) running on the same system.  Some systems only support
 * the older System V shared memory interface (shmget etc.) which is
 * also usable; however, the default allocation limits are often quite
 * small, and the namespace is even more restricted.
 *
 * We also provide an mmap-based shared memory implementation.  This may
 * be useful on systems that provide shared memory via a special-purpose
 * filesystem; by opting for this implementation, the user can even
 * control precisely where their shared memory segments are placed.  It
 * can also be used as a fallback for systems where shm_open and shmget
 * are not available or can't be used for some reason.  Of course,
 * mapping a file residing on an actual spinning disk is a fairly poor
 * approximation for shared memory because writeback may hurt performance
 * substantially, but there should be few systems where we must make do
 * with such poor tools.
 *
 * As ever, Windows requires its own implementation.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_impl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#endif

#include "common/file_perm.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "postmaster/postmaster.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#ifdef USE_DSM_POSIX
static bool dsm_impl_posix(dsm_op op, dsm_handle handle, Size request_size,
						   void **impl_private, void **mapped_address,
						   Size *mapped_size, int elevel);
static int	dsm_impl_posix_resize(int fd, off_t size);
#endif
#ifdef USE_DSM_SYSV
static bool dsm_impl_sysv(dsm_op op, dsm_handle handle, Size request_size,
						  void **impl_private, void **mapped_address,
						  Size *mapped_size, int elevel);
#endif
#ifdef USE_DSM_WINDOWS
static bool dsm_impl_windows(dsm_op op, dsm_handle handle, Size request_size,
							 void **impl_private, void **mapped_address,
							 Size *mapped_size, int elevel);
#endif
#ifdef USE_DSM_MMAP
static bool dsm_impl_mmap(dsm_op op, dsm_handle handle, Size request_size,
						  void **impl_private, void **mapped_address,
						  Size *mapped_size, int elevel);
#endif
static int	errcode_for_dynamic_shared_memory(void);

const struct config_enum_entry dynamic_shared_memory_options[] = {
#ifdef USE_DSM_POSIX
	{"posix", DSM_IMPL_POSIX, false},
#endif
#ifdef USE_DSM_SYSV
	{"sysv", DSM_IMPL_SYSV, false},
#endif
#ifdef USE_DSM_WINDOWS
	{"windows", DSM_IMPL_WINDOWS, false},
#endif
#ifdef USE_DSM_MMAP
	{"mmap", DSM_IMPL_MMAP, false},
#endif
	{NULL, 0, false}
};

/* 实现选择器。 */
int			dynamic_shared_memory_type = DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE;

/* 主区域里为动态共享内存段保留的空间量。 */
int			min_dynamic_shared_memory;

/* 用于填零的缓冲区大小。 */
#define ZBUFFER_SIZE				8192

#define SEGMENT_NAME_PREFIX			"Global/PostgreSQL"

/*------
 * Perform a low-level shared memory operation in a platform-specific way,
 * as dictated by the selected implementation.  Each implementation is
 * required to implement the following primitives.
 *
 * DSM_OP_CREATE.  Create a segment whose size is the request_size and
 * map it.
 *
 * DSM_OP_ATTACH.  Map the segment, whose size must be the request_size.
 *
 * DSM_OP_DETACH.  Unmap the segment.
 *
 * DSM_OP_DESTROY.  Unmap the segment, if it is mapped.  Destroy the
 * segment.
 *
 * Arguments:
 *	 op: The operation to be performed.
 *	 handle: The handle of an existing object, or for DSM_OP_CREATE, the
 *	   a new handle the caller wants created.
 *	 request_size: For DSM_OP_CREATE, the requested size.  Otherwise, 0.
 *	 impl_private: Private, implementation-specific data.  Will be a pointer
 *	   to NULL for the first operation on a shared memory segment within this
 *	   backend; thereafter, it will point to the value to which it was set
 *	   on the previous call.
 *	 mapped_address: Pointer to start of current mapping; pointer to NULL
 *	   if none.  Updated with new mapping address.
 *	 mapped_size: Pointer to size of current mapping; pointer to 0 if none.
 *	   Updated with new mapped size.
 *	 elevel: Level at which to log errors.
 *
 * Return value: true on success, false on failure.  When false is returned,
 * a message should first be logged at the specified elevel, except in the
 * case where DSM_OP_CREATE experiences a name collision, which should
 * silently return false.
 *-----
 */
bool
dsm_impl_op(dsm_op op, dsm_handle handle, Size request_size,
			void **impl_private, void **mapped_address, Size *mapped_size,
			int elevel)
{
	Assert(op == DSM_OP_CREATE || request_size == 0);
	Assert((op != DSM_OP_CREATE && op != DSM_OP_ATTACH) ||
		   (*mapped_address == NULL && *mapped_size == 0));

	switch (dynamic_shared_memory_type)
	{
#ifdef USE_DSM_POSIX
		case DSM_IMPL_POSIX:
			return dsm_impl_posix(op, handle, request_size, impl_private,
								  mapped_address, mapped_size, elevel);
#endif
#ifdef USE_DSM_SYSV
		case DSM_IMPL_SYSV:
			return dsm_impl_sysv(op, handle, request_size, impl_private,
								 mapped_address, mapped_size, elevel);
#endif
#ifdef USE_DSM_WINDOWS
		case DSM_IMPL_WINDOWS:
			return dsm_impl_windows(op, handle, request_size, impl_private,
									mapped_address, mapped_size, elevel);
#endif
#ifdef USE_DSM_MMAP
		case DSM_IMPL_MMAP:
			return dsm_impl_mmap(op, handle, request_size, impl_private,
								 mapped_address, mapped_size, elevel);
#endif
		default:
			elog(ERROR, "unexpected dynamic shared memory type: %d",
				 dynamic_shared_memory_type);
			return false;
	}
}

#ifdef USE_DSM_POSIX
/*
 * 支持 POSIX 共享内存的操作系统原语。
 *
 * POSIX 共享内存段用 shm_open() 创建并挂接，
 * 用 shm_unlink() 解除；调整大小或映射等其他操作
 * 则当作这些共享内存段是文件来做。
 *
 * 事实上，在某些平台上它们也许就是这样实现的。虽然
 * POSIX 共享内存段似乎打算存在于扁平名字空间里，
 * 有些操作系统却可能把它们实现成文件，甚至把对 /xyz
 * 的请求当成在根目录里按该名字创建文件的请求。
 * 使用这种有缺陷平台的用户应当另选一种
 * 共享内存实现。
 */
static bool
dsm_impl_posix(dsm_op op, dsm_handle handle, Size request_size,
			   void **impl_private, void **mapped_address, Size *mapped_size,
			   int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;

	snprintf(name, 64, "/PostgreSQL.%u", handle);

	/* 处理拆除情形。 */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		if (*mapped_address != NULL
			&& munmap(*mapped_address, *mapped_size) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not unmap shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
		if (op == DSM_OP_DESTROY && shm_unlink(name) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not remove shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		return true;
	}

	/*
	 * 创建新段，或打开已有段以便挂接。
	 *
	 * 尽管返回前会关闭该文件描述符，使用
	 * ReserveExternalFD/ReleaseExternalFD 以降低 EMFILE
	 * 失败的概率看来仍是可取的。我们不会长时间持有该
	 * 文件描述符，这一点说明应当用 ReserveExternalFD 而不是 AcquireExternalFD。
	 */
	ReserveExternalFD();

	flags = O_RDWR | (op == DSM_OP_CREATE ? O_CREAT | O_EXCL : 0);
	if ((fd = shm_open(name, flags, PG_FILE_MODE_OWNER)) == -1)
	{
		ReleaseExternalFD();
		if (op == DSM_OP_ATTACH || errno != EEXIST)
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not open shared memory segment \"%s\": %m",
							name)));
		return false;
	}

	/*
	 * 若是在挂接该段，就确定当前大小；若是在
	 * 创建该段，就把大小设成所请求的值。
	 */
	if (op == DSM_OP_ATTACH)
	{
		struct stat st;

		if (fstat(fd, &st) != 0)
		{
			int			save_errno;

			/* 回退已经做过的部分。 */
			save_errno = errno;
			close(fd);
			ReleaseExternalFD();
			errno = save_errno;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not stat shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		request_size = st.st_size;
	}
	else if (dsm_impl_posix_resize(fd, request_size) != 0)
	{
		int			save_errno;

		/* 回退已经做过的部分。 */
		save_errno = errno;
		close(fd);
		ReleaseExternalFD();
		shm_unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
						name, request_size)));
		return false;
	}

	/* 映射它。 */
	address = mmap(NULL, request_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		/* 回退已经做过的部分。 */
		save_errno = errno;
		close(fd);
		ReleaseExternalFD();
		if (op == DSM_OP_CREATE)
			shm_unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	*mapped_address = address;
	*mapped_size = request_size;
	close(fd);
	ReleaseExternalFD();

	return true;
}

/*
 * 设置与文件描述符关联的虚拟内存区域的大小。
 * 如有必要，还要确保操作系统真正分配了虚拟内存，
 * 以免以后出现讨厌的意外。
 *
 * 若截断或分配失败则返回非零，并设置 errno。
 */
static int
dsm_impl_posix_resize(int fd, off_t size)
{
	int			rc;
	int			save_errno;
	sigset_t	save_sigmask;

	/*
	 * 阻塞所有可阻塞的信号，但 SIGQUIT 除外。posix_fallocate() 可能
	 * 运行相当长的时间，而且是要么全成要么全败的操作。若允许
	 * SIGUSR1 反复中断我们（例如由于恢复冲突），
	 * 重试循环可能永远成功不了。
	 */
	if (IsUnderPostmaster)
		sigprocmask(SIG_SETMASK, &BlockSig, &save_sigmask);

	pgstat_report_wait_start(WAIT_EVENT_DSM_ALLOCATE);
#if defined(HAVE_POSIX_FALLOCATE) && defined(__linux__)

	/*
	 * 在 Linux 上，shm_open 的文件描述符由 tmpfs 文件支撑。
	 * 若使用 ftruncate，文件里会有一个洞。访问由洞支撑的
	 * 内存会让 tmpfs 分配页面；若没有更多 tmpfs 空间，
	 * 就会因 SIGBUS 失败。所以我们在这里让 tmpfs 分配页面，
	 * 以便现在就能以 ENOSPC 得体地失败，
	 * 而不是以后冒 SIGBUS 的风险。
	 *
	 * 我们仍用传统的 EINTR 重试循环来处理 SIGCONT。
	 * posix_fallocate() 不会自动重新开始，而且我们不希望
	 * 你挂上调试器时这件事失败。
	 */
	do
	{
		rc = posix_fallocate(fd, 0, size);
	} while (rc == EINTR);

	/*
	 * 调用方预期 errno 会被设置，但 posix_fallocate() 并不设置
	 * 它，而是直接返回错误号。所以要设置 errno，即便
	 * 我们也会返回 rc 来表示成功或失败。
	 */
	errno = rc;
#else
	/* 把文件扩展到所请求的大小。 */
	do
	{
		rc = ftruncate(fd, size);
	} while (rc < 0 && errno == EINTR);
#endif
	pgstat_report_wait_end();

	if (IsUnderPostmaster)
	{
		save_errno = errno;
		sigprocmask(SIG_SETMASK, &save_sigmask, NULL);
		errno = save_errno;
	}

	return rc;
}

#endif							/* 条件编译宏 USE_DSM_POSIX */

#ifdef USE_DSM_SYSV
/*
 * 支持 System V 共享内存的操作系统原语。
 *
 * System V 共享内存段用 shmget()、shmat()、
 * shmdt() 和 shmctl() 来操作。System V 共享内存的
 * 默认分配限额通常相当低，POSIX 设施也许更可取；
 * 但那些设施并非到处都支持。
 */
static bool
dsm_impl_sysv(dsm_op op, dsm_handle handle, Size request_size,
			  void **impl_private, void **mapped_address, Size *mapped_size,
			  int elevel)
{
	key_t		key;
	int			ident;
	char	   *address;
	char		name[64];
	int		   *ident_cache;

	/*
	 * POSIX 共享内存和基于 mmap 的共享内存用名字标识段。
	 * 为避免错误信息无谓地不一致，我们用句柄作为
	 * 名字。
	 */
	snprintf(name, 64, "%u", handle);

	/*
	 * System V 共享内存的名字空间非常受限。名字的类型
	 * 是 key_t，预期为某种整数类型，但不一定和
	 * dsm_handle 相同。我们用 dsm_handle 在各进程之间
	 * 标识共享内存段，这看起来像个问题，其实并不是。
	 * 若 dsm_handle 比 key_t 更大，下面的强制转换可能
	 * 截掉用户所给句柄里的若干位，但每次使用该句柄时
	 * 都会以完全相同的方式截掉完全相同的那些位，
	 * 而这才是真正要紧的全部。反过来，若 dsm_handle
	 * 比 key_t 更小，我们就用不完可用的键空间，
	 * 不过那也
	 * 没什么大不了。
	 *
	 * 我们确实要保证键不为负，因为那可能
	 * 不可移植。
	 */
	key = (key_t) handle;
	if (key < 1)				/* 若类型为无符号，避免编译器告警 */
		key = -key;

	/*
	 * 有一个特殊键 IPC_PRIVATE 不能使用。若创建操作中
	 * 碰巧得到该值，就假装它已存在，让调用方重试。
	 * 若在别处碰上它，说明调用方传入的句柄并不对应
	 * 我们曾经创建过的任何东西，
	 * 这不应当发生。
	 */
	if (key == IPC_PRIVATE)
	{
		if (op != DSM_OP_CREATE)
			elog(DEBUG4, "System V shared memory key may not be IPC_PRIVATE");
		errno = EEXIST;
		return false;
	}

	/*
	 * 对共享内存段做任何事之前，必须用 shmget() 把
	 * 共享内存键映射成共享内存标识。为避免反复查找，
	 * 我们用 impl_private 存放该键。
	 */
	if (*impl_private != NULL)
	{
		ident_cache = *impl_private;
		ident = *ident_cache;
	}
	else
	{
		int			flags = IPCProtection;
		size_t		segsize;

		/*
		 * 先把内存分配好 BEFORE 再取得资源，以免分配失败时
		 * 泄漏该资源。
		 */
		ident_cache = MemoryContextAlloc(TopMemoryContext, sizeof(int));

		/*
		 * 用 shmget 寻找已有段时，必须把大小作为 0 传入。
		 * 传入大于实际大小的非零大小会得到
		 * EINVAL。
		 */
		segsize = 0;

		if (op == DSM_OP_CREATE)
		{
			flags |= IPC_CREAT | IPC_EXCL;
			segsize = request_size;
		}

		if ((ident = shmget(key, segsize, flags)) == -1)
		{
			if (op == DSM_OP_ATTACH || errno != EEXIST)
			{
				int			save_errno = errno;

				pfree(ident_cache);
				errno = save_errno;
				ereport(elevel,
						(errcode_for_dynamic_shared_memory(),
						 errmsg("could not get shared memory segment: %m")));
			}
			return false;
		}

		*ident_cache = ident;
		*impl_private = ident_cache;
	}

	/* 处理拆除情形。 */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		pfree(ident_cache);
		*impl_private = NULL;
		if (*mapped_address != NULL && shmdt(*mapped_address) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not unmap shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
		if (op == DSM_OP_DESTROY && shmctl(ident, IPC_RMID, NULL) < 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not remove shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		return true;
	}

	/* 若是在挂接它，必须用 IPC_STAT 来确定大小。 */
	if (op == DSM_OP_ATTACH)
	{
		struct shmid_ds shm;

		if (shmctl(ident, IPC_STAT, &shm) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not stat shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		request_size = shm.shm_segsz;
	}

	/* 映射它。 */
	address = shmat(ident, NULL, PG_SHMAT_FLAGS);
	if (address == (void *) -1)
	{
		int			save_errno;

		/* 回退已经做过的部分。 */
		save_errno = errno;
		if (op == DSM_OP_CREATE)
			shmctl(ident, IPC_RMID, NULL);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	*mapped_address = address;
	*mapped_size = request_size;

	return true;
}
#endif

#ifdef USE_DSM_WINDOWS
/*
 * 支持 Windows 共享内存的操作系统原语。
 *
 * Windows 的共享内存用文件映射实现，底层可以是
 * 物理文件，也可以是系统分页文件。当前实现使用
 * 系统分页文件，因为物理文件在性能等方面的影响
 * 尚不清楚，而且主共享内存在 Windows 上也以
 * 类似方式使用。
 *
 * 内存映射对象是内核对象：当指向它们的最后一次引用
 * 消失时总会被删除，要么通过 CloseHandle 显式删除，
 * 要么在持有该引用的进程退出时删除。
 */
static bool
dsm_impl_windows(dsm_op op, dsm_handle handle, Size request_size,
				 void **impl_private, void **mapped_address,
				 Size *mapped_size, int elevel)
{
	char	   *address;
	HANDLE		hmap;
	char		name[64];
	MEMORY_BASIC_INFORMATION info;

	/*
	 * 把共享内存段放在 Global\ 名字空间里，可以让任何
	 * 会话中运行的任何进程访问该文件映射对象，只要
	 * 调用方拥有所需的访问权限。但为避免主共享内存里
	 * 遇到过的问题，我们采用与主共享内存相似的命名
	 * 约定。等 GetSharedMemName 里提到的问题解决后，
	 * 这里可以再改。
	 */
	snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);

	/*
	 * 处理拆除情形。Windows 在没有剩余引用时会自动销毁对象，
	 * 因此可以把它当成与脱离相同。
	 */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		if (*mapped_address != NULL
			&& UnmapViewOfFile(*mapped_address) == 0)
		{
			_dosmaperr(GetLastError());
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not unmap shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		if (*impl_private != NULL
			&& CloseHandle(*impl_private) == 0)
		{
			_dosmaperr(GetLastError());
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not remove shared memory segment \"%s\": %m",
							name)));
			return false;
		}

		*impl_private = NULL;
		*mapped_address = NULL;
		*mapped_size = 0;
		return true;
	}

	/* 创建新段，或打开已有段以便挂接。 */
	if (op == DSM_OP_CREATE)
	{
		DWORD		size_high;
		DWORD		size_low;
		DWORD		errcode;

		/* 移位量大于等于类型宽度时，行为未定义。 */
#ifdef _WIN64
		size_high = request_size >> 32;
#else
		size_high = 0;
#endif
		size_low = (DWORD) request_size;

		/* CreateFileMapping 成功时未必会清掉错误码 */
		SetLastError(0);

		hmap = CreateFileMapping(INVALID_HANDLE_VALUE,	/* 使用分页文件 */
								 NULL,	/* 默认安全属性 */
								 PAGE_READWRITE,	/* 内存可读可写 */
								 size_high, /* 大小的高 32 位 */
								 size_low,	/* 大小的低 32 位 */
								 name);

		errcode = GetLastError();
		if (errcode == ERROR_ALREADY_EXISTS || errcode == ERROR_ACCESS_DENIED)
		{
			/*
			 * 在 Windows 上，当该段已存在时，会返回已有段的
			 * 句柄。返回之前必须把它关掉。不过，若已有段是
			 * 由服务创建的，则会返回 ERROR_ACCESS_DENIED。
			 * 这里不做 _dosmaperr，
			 * 因此 errno 不会被修改。
			 */
			if (hmap)
				CloseHandle(hmap);
			return false;
		}

		if (!hmap)
		{
			_dosmaperr(errcode);
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not create shared memory segment \"%s\": %m",
							name)));
			return false;
		}
	}
	else
	{
		hmap = OpenFileMapping(FILE_MAP_WRITE | FILE_MAP_READ,
							   FALSE,	/* 不继承该名字 */
							   name);	/* 映射对象的名字 */
		if (!hmap)
		{
			_dosmaperr(GetLastError());
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not open shared memory segment \"%s\": %m",
							name)));
			return false;
		}
	}

	/* 映射它。 */
	address = MapViewOfFile(hmap, FILE_MAP_WRITE | FILE_MAP_READ,
							0, 0, 0);
	if (!address)
	{
		int			save_errno;

		_dosmaperr(GetLastError());
		/* 回退已经做过的部分。 */
		save_errno = errno;
		CloseHandle(hmap);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/*
	 * VirtualQuery 给出的大小以 page_size 为单位，在 Windows 上是 4K。
	 * 只有挂接时才需要大小，但创建新段时也取一下更好，
	 * 这样 DSM_OP_CREATE 与 DSM_OP_ATTACH 两边的大小
	 * 保持一致。
	 */
	if (VirtualQuery(address, &info, sizeof(info)) == 0)
	{
		int			save_errno;

		_dosmaperr(GetLastError());
		/* 回退已经做过的部分。 */
		save_errno = errno;
		UnmapViewOfFile(address);
		CloseHandle(hmap);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not stat shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	*mapped_address = address;
	*mapped_size = info.RegionSize;
	*impl_private = hmap;

	return true;
}
#endif

#ifdef USE_DSM_MMAP
/*
 * 支持基于 mmap 的共享内存的操作系统原语。
 *
 * 把这叫做共享内存有点名不副实，因为我们真正做的
 * 是创建一批文件并把它们映射进自己的地址空间。
 * 操作系统可能觉得有义务把内容同步到磁盘，即便
 * 并没有页面被换出，这对我们没有好处。若有内存盘，
 * 用户可以把 pg_dynshmem 目录挪到内存盘上，以避开
 * 这个问题。
 */
static bool
dsm_impl_mmap(dsm_op op, dsm_handle handle, Size request_size,
			  void **impl_private, void **mapped_address, Size *mapped_size,
			  int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;

	snprintf(name, 64, PG_DYNSHMEM_DIR "/" PG_DYNSHMEM_MMAP_FILE_PREFIX "%u",
			 handle);

	/* 处理拆除情形。 */
	if (op == DSM_OP_DETACH || op == DSM_OP_DESTROY)
	{
		if (*mapped_address != NULL
			&& munmap(*mapped_address, *mapped_size) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not unmap shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		*mapped_address = NULL;
		*mapped_size = 0;
		if (op == DSM_OP_DESTROY && unlink(name) != 0)
		{
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not remove shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		return true;
	}

	/* 创建新段，或打开已有段以便挂接。 */
	flags = O_RDWR | (op == DSM_OP_CREATE ? O_CREAT | O_EXCL : 0);
	if ((fd = OpenTransientFile(name, flags)) == -1)
	{
		if (op == DSM_OP_ATTACH || errno != EEXIST)
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not open shared memory segment \"%s\": %m",
							name)));
		return false;
	}

	/*
	 * 若是在挂接该段，就确定当前大小；若是在
	 * 创建该段，就把大小设成所请求的值。
	 */
	if (op == DSM_OP_ATTACH)
	{
		struct stat st;

		if (fstat(fd, &st) != 0)
		{
			int			save_errno;

			/* 回退已经做过的部分。 */
			save_errno = errno;
			CloseTransientFile(fd);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not stat shared memory segment \"%s\": %m",
							name)));
			return false;
		}
		request_size = st.st_size;
	}
	else
	{
		/*
		 * 分配一块全为零的缓冲区。
		 *
		 * 注意：用 palloc 分配 zbuffer，而不是只用局部字符数组，
		 * 以保证它对齐得还算好；把数据送到内核时
		 * 这也许能省几个周期。
		 */
		char	   *zbuffer = (char *) palloc0(ZBUFFER_SIZE);
		Size		remaining = request_size;
		bool		success = true;

		/*
		 * 把文件填零。必须用这种笨办法，才能确保文件空间
		 * 真的都已分配，这样以后访问内存映射时才不会
		 * 段错误。这相当
		 * 低效。
		 */
		while (success && remaining > 0)
		{
			Size		goal = remaining;

			if (goal > ZBUFFER_SIZE)
				goal = ZBUFFER_SIZE;
			pgstat_report_wait_start(WAIT_EVENT_DSM_FILL_ZERO_WRITE);
			if (write(fd, zbuffer, goal) == goal)
				remaining -= goal;
			else
				success = false;
			pgstat_report_wait_end();
		}

		if (!success)
		{
			int			save_errno;

			/* 回退已经做过的部分。 */
			save_errno = errno;
			CloseTransientFile(fd);
			unlink(name);
			errno = save_errno ? save_errno : ENOSPC;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
							name, request_size)));
			return false;
		}
	}

	/* 映射它。 */
	address = mmap(NULL, request_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		/* 回退已经做过的部分。 */
		save_errno = errno;
		CloseTransientFile(fd);
		if (op == DSM_OP_CREATE)
			unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	*mapped_address = address;
	*mapped_size = request_size;

	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	return true;
}
#endif

/*
 * 当一段即使没有任何后端挂接也要保留时，必须执行的
 * 特定实现动作。
 *
 * 除 Windows 外，我们完全不必做什么。但 Windows 会在
 * 没有剩余引用时自动清理段，因此我们把段句柄复制到
 * postmaster 进程里。postmaster 不必为接收该句柄做任何事；
 * Windows 会自动把它传过去。
 */
void
dsm_impl_pin_segment(dsm_handle handle, void *impl_private,
					 void **impl_private_pm_handle)
{
	switch (dynamic_shared_memory_type)
	{
#ifdef USE_DSM_WINDOWS
		case DSM_IMPL_WINDOWS:
			if (IsUnderPostmaster)
			{
				HANDLE		hmap;

				if (!DuplicateHandle(GetCurrentProcess(), impl_private,
									 PostmasterHandle, &hmap, 0, FALSE,
									 DUPLICATE_SAME_ACCESS))
				{
					char		name[64];

					snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);
					_dosmaperr(GetLastError());
					ereport(ERROR,
							(errcode_for_dynamic_shared_memory(),
							 errmsg("could not duplicate handle for \"%s\": %m",
									name)));
				}

				/*
				 * 这里记住我们在 postmaster 进程里创建的句柄。
				 * 这个句柄实际上除了 postmaster 之外任何进程都
				 * 用不了，但那没关系。我们只是握着它，以便
				 * 该段被解除钉住时，dsm_impl_unpin_segment
				 * 可以把它关掉。
				 */
				*impl_private_pm_handle = hmap;
			}
			break;
#endif
		default:
			break;
	}
}

/*
 * 当一段不再需要保留、从而会在所有后端都脱离之后
 * 被清理时，必须执行的特定实现动作如下。
 * 除 Windows 外，我们完全不必做什么。
 *
 * 对 Windows，我们关掉 dsm_impl_pin_segment
 * 在 postmaster 进程空间里创建的
 * 那个额外句柄。
 */
void
dsm_impl_unpin_segment(dsm_handle handle, void **impl_private)
{
	switch (dynamic_shared_memory_type)
	{
#ifdef USE_DSM_WINDOWS
		case DSM_IMPL_WINDOWS:
			if (IsUnderPostmaster)
			{
				if (*impl_private &&
					!DuplicateHandle(PostmasterHandle, *impl_private,
									 NULL, NULL, 0, FALSE,
									 DUPLICATE_CLOSE_SOURCE))
				{
					char		name[64];

					snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);
					_dosmaperr(GetLastError());
					ereport(ERROR,
							(errcode_for_dynamic_shared_memory(),
							 errmsg("could not duplicate handle for \"%s\": %m",
									name)));
				}

				*impl_private = NULL;
			}
			break;
#endif
		default:
			break;
	}
}

static int
errcode_for_dynamic_shared_memory(void)
{
	if (errno == EFBIG || errno == ENOMEM)
		return errcode(ERRCODE_OUT_OF_MEMORY);
	else
		return errcode_for_file_access();
}
