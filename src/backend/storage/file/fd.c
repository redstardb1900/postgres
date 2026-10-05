/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  Virtual file descriptor code.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fd.c
 *
 * NOTES:
 *
 * This code manages a cache of 'virtual' file descriptors (VFDs).
 * The server opens many file descriptors for a variety of reasons,
 * including base tables, scratch files (e.g., sort and hash spool
 * files), and random calls to C library routines like system(3); it
 * is quite easy to exceed system limits on the number of open files a
 * single process can have.  (This is around 1024 on many modern
 * operating systems, but may be lower on others.)
 *
 * VFDs are managed as an LRU pool, with actual OS file descriptors
 * being opened and closed as needed.  Obviously, if a routine is
 * opened using these interfaces, all subsequent operations must also
 * be through these interfaces (the File type is not a real file
 * descriptor).
 *
 * For this scheme to work, most (if not all) routines throughout the
 * server should use these interfaces instead of calling the C library
 * routines (e.g., open(2) and fopen(3)) themselves.  Otherwise, we
 * may find ourselves short of real file descriptors anyway.
 *
 * INTERFACE ROUTINES
 *
 * PathNameOpenFile and OpenTemporaryFile are used to open virtual files.
 * A File opened with OpenTemporaryFile is automatically deleted when the
 * File is closed, either explicitly or implicitly at end of transaction or
 * process exit. PathNameOpenFile is intended for files that are held open
 * for a long time, like relation files. It is the caller's responsibility
 * to close them, there is no automatic mechanism in fd.c for that.
 *
 * PathName(Create|Open|Delete)Temporary(File|Dir) are used to manage
 * temporary files that have names so that they can be shared between
 * backends.  Such files are automatically closed and count against the
 * temporary file limit of the backend that creates them, but unlike anonymous
 * files they are not automatically deleted.  See sharedfileset.c for a shared
 * ownership mechanism that provides automatic cleanup for shared files when
 * the last of a group of backends detaches.
 *
 * AllocateFile, AllocateDir, OpenPipeStream and OpenTransientFile are
 * wrappers around fopen(3), opendir(3), popen(3) and open(2), respectively.
 * They behave like the corresponding native functions, except that the handle
 * is registered with the current subtransaction, and will be automatically
 * closed at abort. These are intended mainly for short operations like
 * reading a configuration file; there is a limit on the number of files that
 * can be opened using these functions at any one time.
 *
 * Finally, BasicOpenFile is just a thin wrapper around open() that can
 * release file descriptors in use by the virtual file descriptors if
 * necessary. There is no automatic cleanup of file descriptors returned by
 * BasicOpenFile, it is solely the caller's responsibility to close the file
 * descriptor by calling close(2).
 *
 * If a non-virtual file descriptor needs to be held open for any length of
 * time, report it to fd.c by calling AcquireExternalFD or ReserveExternalFD
 * (and eventually ReleaseExternalFD), so that we can take it into account
 * while deciding how many VFDs can be open.  This applies to FDs obtained
 * with BasicOpenFile as well as those obtained without use of any fd.c API.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <dirent.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/resource.h>		/* 供 getrlimit 使用 */
#include <sys/stat.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_tablespace.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "postmaster/startup.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/resowner_private.h"
#include "utils/varlena.h"

/* 若我们有 pg_flush_data 的实现，则定义 PG_FLUSH_DATA_WORKS */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif !defined(WIN32) && defined(MS_ASYNC)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif

/*
 * 我们必须留出一些文件描述符给 system()、动态加载器、
 * 以及其他不经 fd.c 就试图打开文件的代码。这就是
 * 留出的个数。（我们相当努力地防止 EMFILE
 * 错误，但绝无把握不会因为其他进程吃掉文件描述符
 * 而得到 ENFILE。因此不经 fd.c 就打开文件是坏主意。
 * 尽管如此，我们无法控制所有代码。）
 *
 * 因为这只是一个固定设置，我们实际上假定
 * 这类代码不会长期占用文件描述符；否则这份余量
 * 很可能不够。特别要注意，我们预期
 * 加载共享库不会使打开文件数永久增加。
 * （截至 2004 年 2 月，这在多数乃至
 * 所有平台上似乎都成立。）
 */
#define NUM_RESERVED_FDS		10

/*
 * 扣掉保留的之后，若可用文件描述符少于此数，就拒绝继续。
 * （这个值选来配合 ulimit -n 64，但再小很多就不行。
 * 注意，这个值保证 numExternalFDs 至少能到
 * 16；就目前而言，contrib/postgres_fdw 的回归测试
 * 除非它至少能增长到 14，否则不会通过。）
 */
#define FD_MINFREE				48

/*
 * 不少平台允许单个进程打开的文件数，远多于
 * 许多进程同时这么做时真正能支撑的数量。
 * 这个 GUC 参数让数据库管理员把 max_safe_fds 限制到
 * 低于 postmaster 初始探测所建议的值。
 */
int			max_files_per_process = 1000;

/*
 * fd.c 所知道的操作用以打开的文件描述符最大个数
 * （VFDs、AllocateFile 等，或外部文件描述符）。它被初始化为
 * 一个保守值，在引导或独立后端情形下会一直保持这样。
 * 在正常的 postmaster 运行中，postmaster
 * 在初始化后期调用 set_max_safe_fds() 来更新该值，
 * 然后由派生出的子进程继承。
 *
 * 注意：设置这个变量时已经考虑了 max_files_per_process，
 * 因此不必再单独测试它。
 */
int			max_safe_fds = FD_MINFREE;	/* 若未改变则用默认值 */

/* fsync() 失败后是否仍可安全地继续运行。 */
bool		data_sync_retry = false;

/* SyncDataDirectory() 应当如何完成其工作。 */
int			recovery_init_sync_method = RECOVERY_INIT_SYNC_METHOD_FSYNC;

/* 哪些种类的文件应当以 PG_O_DIRECT 打开。 */
int			io_direct_flags;

/* 调试用…… */

#ifdef FDDEBUG
#define DO_DB(A) \
	do { \
		int			_do_db_save_errno = errno; \
		A; \
		errno = _do_db_save_errno; \
	} while (0)
#else
#define DO_DB(A) \
	((void) 0)
#endif

#define VFD_CLOSED (-1)

#define FileIsValid(file) \
	((file) > 0 && (file) < (int) SizeVfdCache && VfdCache[file].fileName != NULL)

#define FileIsNotOpen(file) (VfdCache[file].fd == VFD_CLOSED)

/* 下面是 fdstate 里已分配的位： */
#define FD_DELETE_AT_CLOSE	(1 << 0)	/* 为真则关闭时删除 */
#define FD_CLOSE_AT_EOXACT	(1 << 1)	/* 为真则在事务结束时关闭 */
#define FD_TEMP_FILE_LIMIT	(1 << 2)	/* 为真则遵守 temp_file_limit */

typedef struct vfd
{
	int			fd;				/* 当前文件描述符，若没有则为 VFD_CLOSED */
	unsigned short fdstate;		/* VFD 状态的位标志 */
	ResourceOwner resowner;		/* 所有者，用于自动清理 */
	File		nextFree;		/* 若在空闲链表中，则指向下一个空闲 VFD */
	File		lruMoreRecently;	/* 按最近使用情况组织的双向链表 */
	File		lruLessRecently;
	off_t		fileSize;		/* 文件当前大小（若不是临时文件则为 0） */
	char	   *fileName;		/* 文件名，未使用的 VFD 则为 NULL */
	/* 注意：fileName 由 malloc 分配，关闭该 VFD 时必须 free */
	int			fileFlags;		/* 用于（重新）打开文件的 open(2) 标志 */
	mode_t		fileMode;		/* 传给 open(2) 的模式 */
} Vfd;

/*
 * 虚拟文件描述符数组的指针和大小。它按
 * 需要增长。File 值是这个数组的下标。
 * 注意 VfdCache[0] 不是可用的 VFD，只是链表头。
 */
static Vfd *VfdCache;
static Size SizeVfdCache = 0;

/*
 * 已知被 VFD 项占用的文件描述符个数。
 */
static int	nfile = 0;

/*
 * 标志：是否值得扫描 VfdCache，寻找要关闭的
 * 临时文件
 */
static bool have_xact_temporary_files = false;

/*
 * 跟踪所有临时文件的总大小。注意：在强制执行 temp_file_limit
 * 时，这个值无法溢出，因为限制无法超过
 * INT_MAX 千字节。不强制执行时，理论上它可能
 * 溢出，但我们不在乎。
 */
static uint64 temporary_files_size = 0;

/* 临时文件访问已初始化且尚未关闭？ */
#ifdef USE_ASSERT_CHECKING
static bool temporary_files_allowed = false;
#endif

/*
 * 用 AllocateFile、AllocateDir 和
 * OpenTransientFile 打开的操作系统句柄列表。
 */
typedef enum
{
	AllocateDescFile,
	AllocateDescPipe,
	AllocateDescDir,
	AllocateDescRawFD
} AllocateDescKind;

typedef struct
{
	AllocateDescKind kind;
	SubTransactionId create_subid;
	union
	{
		FILE	   *file;
		DIR		   *dir;
		int			fd;
	}			desc;
} AllocateDesc;

static int	numAllocatedDescs = 0;
static int	maxAllocatedDescs = 0;
static AllocateDesc *allocatedDescs = NULL;

/*
 * 报告给 Reserve/ReleaseExternalFD 的已打开外部文件描述符个数。
 */
static int	numExternalFDs = 0;

/*
 * 当前会话中打开的临时文件个数；
 * 用于生成临时文件名。
 */
static long tempFileCounter = 0;

/*
 * 临时表空间的 OIDs 数组。（有些项可能是 InvalidOid，
 * 表示应当使用当前数据库的默认表空间。）
 * 当 numTempTableSpaces 为 -1 时，当前
 * 事务里还没有设置它。
 */
static Oid *tempTableSpaces = NULL;
static int	numTempTableSpaces = -1;
static int	nextTempTableSpace = 0;


/*--------------------
 *
 * Private Routines
 *
 * Delete		   - delete a file from the Lru ring
 * LruDelete	   - remove a file from the Lru ring and close its FD
 * Insert		   - put a file at the front of the Lru ring
 * LruInsert	   - put a file at the front of the Lru ring and open it
 * ReleaseLruFile  - Release an fd by closing the last entry in the Lru ring
 * ReleaseLruFiles - Release fd(s) until we're under the max_safe_fds limit
 * AllocateVfd	   - grab a free (or new) file record (from VfdCache)
 * FreeVfd		   - free a file record
 *
 * The Least Recently Used ring is a doubly linked list that begins and
 * ends on element zero.  Element zero is special -- it doesn't represent
 * a file and its "fd" field always == VFD_CLOSED.  Element zero is just an
 * anchor that shows us the beginning/end of the ring.
 * Only VFD elements that are currently really open (have an FD assigned) are
 * in the Lru ring.  Elements that are "virtually" open can be recognized
 * by having a non-null fileName field.
 *
 * example:
 *
 *	   /--less----\				   /---------\
 *	   v		   \			  v			  \
 *	 #0 --more---> LeastRecentlyUsed --more-\ \
 *	  ^\									| |
 *	   \\less--> MostRecentlyUsedFile	<---/ |
 *		\more---/					 \--less--/
 *
 *--------------------
 */
static void Delete(File file);
static void LruDelete(File file);
static void Insert(File file);
static int	LruInsert(File file);
static bool ReleaseLruFile(void);
static void ReleaseLruFiles(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int	FileAccess(File file);
static File OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError);
static bool reserveAllocatedDesc(void);
static int	FreeDesc(AllocateDesc *desc);

static void BeforeShmemExit_Files(int code, Datum arg);
static void CleanupTempFiles(bool isCommit, bool isProcExit);
static void RemovePgTempRelationFiles(const char *tsdirname);
static void RemovePgTempRelationFilesInDbspace(const char *dbspacedirname);

static void walkdir(const char *path,
					void (*action) (const char *fname, bool isdir, int elevel),
					bool process_symlinks,
					int elevel);
#ifdef PG_FLUSH_DATA_WORKS
static void pre_sync_fname(const char *fname, bool isdir, int elevel);
#endif
static void datadir_fsync_fname(const char *fname, bool isdir, int elevel);
static void unlink_if_exists_fname(const char *fname, bool isdir, int elevel);

static int	fsync_parent_path(const char *fname, int elevel);


/*
 * pg_fsync --- 做带或不带写穿透的 fsync
 */
int
pg_fsync(int fd)
{
#if !defined(WIN32) && defined(USE_ASSERT_CHECKING)
	struct stat st;

	/*
	 * 某些操作系统的 fsync() 实现对其文件描述符
	 * 参数在打开时所使用的文件访问模式有要求，
	 * 而且这些要求取决于该文件描述符
	 * 是不是目录。
	 *
	 * 对任何最终可能交给 fsync() 的文件描述符，
	 * 我们都应当以与所有受支持系统上的
	 * fsync() 兼容的访问模式打开它，否则代码可能不可
	 * 移植，即使它在当前系统上运行正常。
	 *
	 * 这里我们断言：文件的描述符是以写
	 * 权限打开的（即不是 O_RDONLY），目录则没有写
	 * 权限（O_RDONLY）。注意，即使
	 * fsync() 被禁用，这个断言检查仍会进行。
	 *
	 * 若 fstat() 失败，则忽略它，让随后的 fsync() 去报错。
	 */
	if (fstat(fd, &st) == 0)
	{
		int			desc_flags = fcntl(fd, F_GETFL);

		desc_flags &= O_ACCMODE;

		if (S_ISDIR(st.st_mode))
			Assert(desc_flags == O_RDONLY);
		else
			Assert(desc_flags != O_RDONLY);
	}
	errno = 0;
#endif

	/* 若没有必要做 sync_method 测试，这个 #if 用来跳过它 */
#if defined(HAVE_FSYNC_WRITETHROUGH) && !defined(FSYNC_WRITETHROUGH_IS_FSYNC)
	if (sync_method == SYNC_METHOD_FSYNC_WRITETHROUGH)
		return pg_fsync_writethrough(fd);
	else
#endif
		return pg_fsync_no_writethrough(fd);
}


/*
 * pg_fsync_no_writethrough --- 与 fsync 相同，但在
 *	enableFsync 关闭时什么也不做
 */
int
pg_fsync_no_writethrough(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fsync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_fsync_writethrough 写穿透版
 */
int
pg_fsync_writethrough(int fd)
{
	if (enableFsync)
	{
#ifdef WIN32
		return _commit(fd);
#elif defined(F_FULLFSYNC)
		return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
		errno = ENOSYS;
		return -1;
#endif
	}
	else
		return 0;
}

/*
 * pg_fdatasync --- 与 fdatasync 相同，但在 enableFsync 关闭时什么也不做
 */
int
pg_fdatasync(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fdatasync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_flush_data --- 建议操作系统刷出所述的脏数据。
 *
 * 偏移为 0 且 nbytes 为 0 表示应当刷出整个文件
 */
void
pg_flush_data(int fd, off_t offset, off_t nbytes)
{
	/*
	 * 目前刷出文件主要是为了减轻稍后
	 * fsync()/fdatasync() 调用的影响。因此若禁用了 fsync，
	 * 就不要触发刷出。这个决定我们也许将来
	 * 会做成可配置的。
	 */
	if (!enableFsync)
		return;

	/*
	 * 当前平台上支持的各种替代实现我们都编译进来，
	 * 以便更容易发现可移植性问题。
	 */
#if defined(HAVE_SYNC_FILE_RANGE)
	{
		int			rc;
		static bool not_implemented_by_kernel = false;

		if (not_implemented_by_kernel)
			return;

retry:

		/*
		 * sync_file_range(SYNC_FILE_RANGE_WRITE) 目前是 Linux 特有的，
		 * 它告诉操作系统应当开始对指定块做回写，
		 * 但我们不想等待完成。注意，
		 * 若该范围内已有过多脏数据，这次调用可能阻塞。
		 * 在支持它的操作系统上这是首选方法，因为可用时它工作
		 * 可靠（与 msync() 相反），并且不会把
		 * 干净数据刷出去（不像 FADV_DONTNEED）。
		 */
		rc = sync_file_range(fd, offset, nbytes,
							 SYNC_FILE_RANGE_WRITE);
		if (rc != 0)
		{
			int			elevel;

			if (rc == EINTR)
				goto retry;

			/*
			 * 对没有 sync_file_range() 实现的系统，例如 Windows WSL，
			 * 只产生一次警告，然后抑制本进程
			 * 此后的所有尝试。
			 */
			if (errno == ENOSYS)
			{
				elevel = WARNING;
				not_implemented_by_kernel = true;
			}
			else
				elevel = data_sync_elevel(WARNING);

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
#if !defined(WIN32) && defined(MS_ASYNC)
	{
		void	   *p;
		static int	pagesize = 0;

		/*
		 * 在若干操作系统上，对 mmap 的文件做 msync(MS_ASYNC)
		 * 会触发回写。在 Linux 上只有指定 MS_SYNC 才会这样，但
		 * 那样回写是同步的。幸好常见的 Linux
		 * 系统都有 sync_file_range()。这比
		 * FADV_DONTNEED 更好，因为它不会把干净数据刷出去。
		 *
		 * 我们映射该文件（mmap()），让内核把内容同步回去
		 * （msync()），然后再去掉映射（munmap()）。
		 */

		/* 若要映射整个文件，mmap() 需要实际长度 */
		if (offset == 0 && nbytes == 0)
		{
			nbytes = lseek(fd, 0, SEEK_END);
			if (nbytes < 0)
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not determine dirty data size: %m")));
				return;
			}
		}

		/*
		 * 有些平台拒绝跨页不完整的 mmap() 尝试。为了处理
		 * 这一点，就把请求截断到页边界。若多出来的
		 * 字节没被刷出，反正这也只是一个提示。
		 */

		/* 页大小只取一次 */
		if (pagesize == 0)
			pagesize = sysconf(_SC_PAGESIZE);

		/* 把长度对齐到页大小，丢掉不足一页的部分 */
		if (pagesize > 0)
			nbytes = (nbytes / pagesize) * pagesize;

		/* 不足一页的请求是空操作 */
		if (nbytes <= 0)
			return;

		/*
		 * mmap 很可能失败，尤其在 32 位平台上，那里
		 * 地址空间可能根本不够。若如此，就静默地落到
		 * 下一种实现。
		 */
		if (nbytes <= (off_t) SSIZE_MAX)
			p = mmap(NULL, nbytes, PROT_READ, MAP_SHARED, fd, offset);
		else
			p = MAP_FAILED;

		if (p != MAP_FAILED)
		{
			int			rc;

			rc = msync(p, (size_t) nbytes, MS_ASYNC);
			if (rc != 0)
			{
				ereport(data_sync_elevel(WARNING),
						(errcode_for_file_access(),
						 errmsg("could not flush dirty data: %m")));
				/* NB: need to fall through to munmap()! */
			}

			rc = munmap(p, (size_t) nbytes);
			if (rc != 0)
			{
				/* 映射会留下来，因此这是 FATAL 错误 */
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not munmap() while flushing data: %m")));
			}

			return;
		}
	}
#endif
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	{
		int			rc;

		/*
		 * 通知内核，传入的范围不应当再被缓存。
		 * 这有一个我们想要的副作用：写出脏
		 * 数据；也有一个不想要的副作用：很可能丢掉有用的
		 * 干净缓存块。由于后一个原因，这是最不
		 * 优先的方法。
		 */

		rc = posix_fadvise(fd, offset, nbytes, POSIX_FADV_DONTNEED);

		if (rc != 0)
		{
			/* 不要报错，这只是性能上的优化 */
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
}

/*
 * 把一个已打开的文件截断到给定长度。
 */
static int
pg_ftruncate(int fd, off_t length)
{
	int			ret;

retry:
	ret = ftruncate(fd, length);

	if (ret == -1 && errno == EINTR)
		goto retry;

	return ret;
}

/*
 * 按名字把文件截断到给定长度。
 */
int
pg_truncate(const char *path, off_t length)
{
	int			ret;
#ifdef WIN32
	int			save_errno;
	int			fd;

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd >= 0)
	{
		ret = pg_ftruncate(fd, length);
		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
	}
	else
		ret = -1;
#else

retry:
	ret = truncate(path, length);

	if (ret == -1 && errno == EINTR)
		goto retry;
#endif

	return ret;
}

/*
 * fsync_fname -- 对文件或目录做 fsync，并正确处理错误。
 *
 * 尝试对文件或目录做 fsync。做后者时，忽略那些
 * 表示操作系统根本不允许或不需要对目录做 fsync 的错误。
 */
void
fsync_fname(const char *fname, bool isdir)
{
	fsync_fname_ext(fname, isdir, false, data_sync_elevel(ERROR));
}

/*
 * durable_rename -- rename(2) 的包装，发出持久性所需的 fsync。
 *
 * 本例程确保返回之后，重命名文件的效果
 * 在崩溃时仍然保持。本例程运行期间若崩溃，
 * 新文件的位置上只会留下原先已有的文件或已移动的文件；
 * 不会出现混合状态或被截断的文件。
 *
 * 做法是在 rename 之前对旧文件名和可能已存在的
 * 目标文件名做 fsync，之后再对目标文件和目录做 fsync。
 *
 * 注意，rename() 无法跨任意目录使用，因为它们
 * 可能不在同一个文件系统上。因此本例程不
 * 支持跨目录重命名。
 *
 * 按调用者指定的严重级别记录错误。
 *
 * 成功返回 0，否则返回 -1。注意返回时 errno
 * 无效。
 */
int
durable_rename(const char *oldfile, const char *newfile, int elevel)
{
	int			fd;

	/*
	 * 首先对旧路径和目标路径（若存在）做 fsync，以确保它们
	 * 在磁盘上真正持久。同步目标文件并非
	 * 严格必要，但它让崩溃推理更容易；
	 * 因为那样就能保证崩溃之后源文件或目标文件
	 * 至少有一个存在。
	 */
	if (fsync_fname_ext(oldfile, false, false, elevel) != 0)
		return -1;

	fd = OpenTransientFile(newfile, PG_BINARY | O_RDWR);
	if (fd < 0)
	{
		if (errno != ENOENT)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", newfile)));
			return -1;
		}
	}
	else
	{
		if (pg_fsync(fd) != 0)
		{
			int			save_errno;

			/* 出错时关闭文件，此时可能不在事务上下文里 */
			save_errno = errno;
			CloseTransientFile(fd);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", newfile)));
			return -1;
		}

		if (CloseTransientFile(fd) != 0)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", newfile)));
			return -1;
		}
	}

	/* 该做真正的操作了…… */
	if (rename(oldfile, newfile) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						oldfile, newfile)));
		return -1;
	}

	/*
	 * 为了保证重命名持久，用新名字对该文件
	 * 及其所在目录做 fsync。
	 */
	if (fsync_fname_ext(newfile, false, false, elevel) != 0)
		return -1;

	if (fsync_parent_path(newfile, elevel) != 0)
		return -1;

	return 0;
}

/*
 * durable_unlink -- 以持久的方式删除文件。
 *
 * 本例程确保返回之后，删除文件的效果
 * 在崩溃时仍然保持。本例程运行期间若崩溃，
 * 系统不会处于混合状态。
 *
 * 做法是在实际删除完成之后，对文件的父目录
 * 做 fsync。
 *
 * 按调用者指定的严重级别记录错误。
 *
 * 成功返回 0，否则返回 -1。注意返回时 errno
 * 无效。
 */
int
durable_unlink(const char *fname, int elevel)
{
	if (unlink(fname) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						fname)));
		return -1;
	}

	/*
	 * 为了保证删除文件是持久的，对其
	 * 父目录做 fsync。
	 */
	if (fsync_parent_path(fname, elevel) != 0)
		return -1;

	return 0;
}

/*
 * InitFileAccess --- 在后端启动期间初始化本模块。
 *
 * 这在正常后端或独立后端启动时调用。
 * postmaster 里不会调用它。
 *
 * 注意，这并不初始化临时文件访问，那是
 * 经由 InitTemporaryFileAccess() 单独初始化的。
 */
void
InitFileAccess(void)
{
	Assert(SizeVfdCache == 0);	/* 只调用我一次 */

	/* 初始化缓存头项 */
	VfdCache = (Vfd *) malloc(sizeof(Vfd));
	if (VfdCache == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	MemSet((char *) &(VfdCache[0]), 0, sizeof(Vfd));
	VfdCache->fd = VFD_CLOSED;

	SizeVfdCache = 1;
}

/*
 * InitTemporaryFileAccess --- 在启动期间初始化临时文件访问。
 *
 * 这在正常后端或独立后端启动时调用。
 * postmaster 里不会调用它。
 *
 * 它与 InitFileAccess() 分开，因为临时文件清理会
 * 引起 pgstat 报告。pgstat 在 before_shmem_exit() 期间关闭，
 * 我们的报告必须发生在那之前。低层文件访问应当
 * 可用得更久，因此临时文件处理的初始化与关闭
 * 是分开的。
 */
void
InitTemporaryFileAccess(void)
{
	Assert(SizeVfdCache != 0);	/* InitFileAccess() 必须已经运行过 */
	Assert(!temporary_files_allowed);	/* 只调用我一次 */

	/*
	 * 注册共享内存退出前的钩子，以确保在仍能
	 * 报告统计时丢掉临时文件。
	 */
	before_shmem_exit(BeforeShmemExit_Files, 0);

#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = true;
#endif
}

/*
 * count_usable_fds --- 统计系统允许我们打开多少文件描述符，
 *		并估计已经打开了多少。
 *
 * 若 usable_fds 达到 max_to_probe 就停止计数。注意：较小的
 * max_to_probe 可能导致低估 already_open；
 * 我们必须先填上已用文件描述符集合里的空隙，
 * already_open 的计算才会给出正确答案。
 * 实践中几十个 max_to_probe 就够。
 *
 * 我们假定 stderr（文件描述符 2）可以用来 dup。调用
 * 脚本理论上可以关掉它，但那真是个坏主意，
 * 因为那样就有丢失例如 libc 错误消息的风险。
 */
static void
count_usable_fds(int max_to_probe, int *usable_fds, int *already_open)
{
	int		   *fd;
	int			size;
	int			used = 0;
	int			highestfd = 0;
	int			j;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
	int			getrlimit_status;
#endif

	size = 1024;
	fd = (int *) palloc(size * sizeof(int));

#ifdef HAVE_GETRLIMIT
	getrlimit_status = getrlimit(RLIMIT_NOFILE, &rlim);
	if (getrlimit_status != 0)
		ereport(WARNING, (errmsg("getrlimit failed: %m")));
#endif							/* 具备 HAVE_GETRLIMIT */

	/* 一直 dup，直到失败或达到探测上限 */
	for (;;)
	{
		int			thisfd;

#ifdef HAVE_GETRLIMIT

		/*
		 * 不要超过 RLIMIT_NOFILE；在某些平台上会
		 * 引起烦人的内核日志
		 */
		if (getrlimit_status == 0 && highestfd >= rlim.rlim_cur - 1)
			break;
#endif

		thisfd = dup(2);
		if (thisfd < 0)
		{
			/* 预期是 EMFILE 或 ENFILE，否则就可疑 */
			if (errno != EMFILE && errno != ENFILE)
				elog(WARNING, "duplicating stderr file descriptor failed after %d successes: %m", used);
			break;
		}

		if (used >= size)
		{
			size *= 2;
			fd = (int *) repalloc(fd, size * sizeof(int));
		}
		fd[used++] = thisfd;

		if (highestfd < thisfd)
			highestfd = thisfd;

		if (used >= max_to_probe)
			break;
	}

	/* 释放我们打开的文件 */
	for (j = 0; j < used; j++)
		close(fd[j]);

	pfree(fd);

	/*
	 * 返回结果。usable_fds 就是成功 dup 的次数。我们
	 * 假定系统限制是 highestfd+1（记住 0 是合法的文件描述符
	 * 号），因此 already_open 是 highestfd+1 减去 usable_fds。
	 */
	*usable_fds = used;
	*already_open = highestfd + 1 - used;
}

/*
 * set_max_safe_fds
 *		确定 fd.c 被允许使用的文件描述符个数
 */
void
set_max_safe_fds(void)
{
	int			usable_fds;
	int			already_open;

	/*----------
	 * We want to set max_safe_fds to
	 *			MIN(usable_fds, max_files_per_process - already_open)
	 * less the slop factor for files that are opened without consulting
	 * fd.c.  This ensures that we won't exceed either max_files_per_process
	 * or the experimentally-determined EMFILE limit.
	 *----------
	 */
	count_usable_fds(max_files_per_process,
					 &usable_fds, &already_open);

	max_safe_fds = Min(usable_fds, max_files_per_process - already_open);

	/*
	 * 去掉为 system() 等保留的文件描述符。
	 */
	max_safe_fds -= NUM_RESERVED_FDS;

	/*
	 * 确保我们仍然够用。
	 */
	if (max_safe_fds < FD_MINFREE)
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient file descriptors available to start server process"),
				 errdetail("System allows %d, server needs at least %d.",
						   max_safe_fds + NUM_RESERVED_FDS,
						   FD_MINFREE + NUM_RESERVED_FDS)));

	elog(DEBUG2, "max_safe_fds = %d, usable_fds = %d, already_open = %d",
		 max_safe_fds, usable_fds, already_open);
}

/*
 * 用 BasicOpenFilePerm() 打开文件，并把默认文件模式传给
 * fileMode 参数。
 */
int
BasicOpenFile(const char *fileName, int fileFlags)
{
	return BasicOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * BasicOpenFilePerm --- 与 open(2) 相同，只是必要时可以释放其他文件描述符。
 *
 * 这是导出给那些确实想要普通内核文件描述符、
 * 但又必须能抵御文件描述符耗尽的地方使用的。一旦成功
 * 返回一个文件描述符，调用者就有责任确保
 * 它不会在 ereport() 时泄漏！多数用户不应当直接
 * 调用本例程，而应使用 VFD 抽象层，它
 * 既能防止描述符泄漏，也能管理
 * 需要打开超过一小段时间的文件。
 *
 * 理想情况下，这应当是后端里对 open() 的唯一直接调用。
 * 实践中，postmaster 会直接调用 open()，而且后端启动早期
 * 也有一些直接的 open() 调用。那些是可以的，因为
 * 那时本模块本来也没有任何已打开的文件可以关闭。
 */
int
BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

tryAgain:
#ifdef PG_O_DIRECT_USE_F_NOCACHE

	/*
	 * 我们定义用来代替 O_DIRECT、并用 F_NOCACHE 模拟它的那个值，
	 * 最好不要与任何标准标志冲突。
	 */
	StaticAssertStmt((PG_O_DIRECT &
					  (O_APPEND |
					   O_CLOEXEC |
					   O_CREAT |
					   O_DSYNC |
					   O_EXCL |
					   O_RDWR |
					   O_RDONLY |
					   O_SYNC |
					   O_TRUNC |
					   O_WRONLY)) == 0,
					 "PG_O_DIRECT value collides with standard flag");
	fd = open(fileName, fileFlags & ~PG_O_DIRECT, fileMode);
#else
	fd = open(fileName, fileFlags, fileMode);
#endif

	if (fd >= 0)
	{
#ifdef PG_O_DIRECT_USE_F_NOCACHE
		if (fileFlags & PG_O_DIRECT)
		{
			if (fcntl(fd, F_NOCACHE, 1) < 0)
			{
				int			save_errno = errno;

				close(fd);
				errno = save_errno;
				return -1;
			}
		}
#endif

		return fd;				/* 成功！ */
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto tryAgain;
		errno = save_errno;
	}

	return -1;					/* 失败 */
}

/*
 * AcquireExternalFD - 尝试保留一个外部文件描述符。
 *
 * 需要把文件描述符保持打开超过一小段时间、
 * 但又无法使用本模块提供的其他设施的调用者，
 * 应当使用这个。
 *
 * 它与底层的 ReserveExternalFD 函数的差别
 * 在于：若已经保留了过多外部文件描述符，
 * 它会报告失败（设置 errno 并返回假）。
 * 凡是要保留的文件描述符总数不可预测
 * 且并不小的代码，都应当使用它。
 */
bool
AcquireExternalFD(void)
{
	/*
	 * 我们不希望超过 max_safe_fds / 3 个文件描述符被
	 * 外部文件描述符消耗掉。
	 */
	if (numExternalFDs < max_safe_fds / 3)
	{
		ReserveExternalFD();
		return true;
	}
	errno = EMFILE;
	return false;
}

/*
 * ReserveExternalFD - 报告外部消耗了一个文件描述符。
 *
 * 需要把文件描述符保持打开超过一小段时间、
 * 但又无法使用本模块提供的其他设施的调用者，
 * 应当使用这个。它只跟踪该文件描述符的使用，并在需要时关闭
 * VFDs，以确保我们留有 NUM_RESERVED_FDS 个文件描述符可用。
 *
 * 只有在保留文件描述符失败会是
 * 致命的代码里才直接调用它；例如 WAL 写入代码就这样做，因为否则
 * 就是会话失败。而且，在可能
 * 每个进程消耗不止一个文件描述符的代码里这样做非常不明智。
 *
 * 注意：只要大家都守规矩，使 NUM_RESERVED_FDS 个文件描述符仍然
 * 可用，在实际打开文件描述符之前或
 * 之后调用这个都关系不大；但事先调用能降低
 * 并非人人守规矩时出现 EMFILE 失败的风险。无论如何，
 * 使外部文件描述符计数与现实保持同步完全是调用者的责任。
 */
void
ReserveExternalFD(void)
{
	/*
	 * 必要时释放 VFDs 以保持安全。因为我们在
	 * 增加 numExternalFDs 之前这样做，最终状态会如所期望，即
	 * nfile + numAllocatedDescs + numExternalFDs <= max_safe_fds。
	 */
	ReleaseLruFiles();

	numExternalFDs++;
}

/*
 * ReleaseExternalFD - 报告释放了一个外部文件描述符。
 *
 * 它保证不改变 errno，因此可以在失败路径上使用。
 */
void
ReleaseExternalFD(void)
{
	Assert(numExternalFDs > 0);
	numExternalFDs--;
}


#if defined(FDDEBUG)

static void
_dump_lru(void)
{
	int			mru = VfdCache[0].lruLessRecently;
	Vfd		   *vfdP = &VfdCache[mru];
	char		buf[2048];

	snprintf(buf, sizeof(buf), "LRU: MOST %d ", mru);
	while (mru != 0)
	{
		mru = vfdP->lruLessRecently;
		vfdP = &VfdCache[mru];
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d ", mru);
	}
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "LEAST");
	elog(LOG, "%s", buf);
}
#endif							/* 调试宏 FDDEBUG */

static void
Delete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Delete %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	VfdCache[vfdP->lruLessRecently].lruMoreRecently = vfdP->lruMoreRecently;
	VfdCache[vfdP->lruMoreRecently].lruLessRecently = vfdP->lruLessRecently;

	DO_DB(_dump_lru());
}

static void
LruDelete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruDelete %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	/*
	 * 关闭该文件。我们不预期这会失败；若失败了，
	 * 泄漏这个文件描述符也比弄乱内部状态要好。
	 */
	if (close(vfdP->fd) != 0)
		elog(vfdP->fdstate & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
			 "could not close file \"%s\": %m", vfdP->fileName);
	vfdP->fd = VFD_CLOSED;
	--nfile;

	/* 把该 vfd 记录从 LRU 环里删掉 */
	Delete(file);
}

static void
Insert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Insert %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	vfdP->lruMoreRecently = 0;
	vfdP->lruLessRecently = VfdCache[0].lruLessRecently;
	VfdCache[0].lruLessRecently = file;
	VfdCache[vfdP->lruLessRecently].lruMoreRecently = file;

	DO_DB(_dump_lru());
}

/* 成功返回 0，重新打开失败返回 -1（并设置 errno） */
static int
LruInsert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruInsert %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (FileIsNotOpen(file))
	{
		/* 关闭多余的内核文件描述符。 */
		ReleaseLruFiles();

		/*
		 * 打开仍可能因缺少文件描述符而失败，例如由于
		 * 整个系统的文件表已满。因此要准备好在必要时
		 * 再释放一个文件描述符……
		 */
		vfdP->fd = BasicOpenFilePerm(vfdP->fileName, vfdP->fileFlags,
									 vfdP->fileMode);
		if (vfdP->fd < 0)
		{
			DO_DB(elog(LOG, "re-open failed: %m"));
			return -1;
		}
		else
		{
			++nfile;
		}
	}

	/*
	 * 把它放到 Lru 环的头部
	 */

	Insert(file);

	return 0;
}

/*
 * 关闭最近最少使用的 VFD，从而释放一个内核文件描述符。
 */
static bool
ReleaseLruFile(void)
{
	DO_DB(elog(LOG, "ReleaseLruFile. Opened %d", nfile));

	if (nfile > 0)
	{
		/*
		 * 已经有打开的文件，因此环里应当至少有一个
		 * 正在使用的 vfd。
		 */
		Assert(VfdCache[0].lruMoreRecently != 0);
		LruDelete(VfdCache[0].lruMoreRecently);
		return true;			/* 释放了一个文件 */
	}
	return false;				/* 没有可释放的文件 */
}

/*
 * 按需要释放内核文件描述符，以落到 max_safe_fds 限制之下。
 * 调用这个之后，就可以尝试再打开一个文件。
 */
static void
ReleaseLruFiles(void)
{
	while (nfile + numAllocatedDescs + numExternalFDs >= max_safe_fds)
	{
		if (!ReleaseLruFile())
			break;
	}
}

static File
AllocateVfd(void)
{
	Index		i;
	File		file;

	DO_DB(elog(LOG, "AllocateVfd. Size %zu", SizeVfdCache));

	Assert(SizeVfdCache > 0);	/* 还没调用 InitFileAccess？ */

	if (VfdCache[0].nextFree == 0)
	{
		/*
		 * 空闲链表是空的，因此该增大
		 * 数组了。每次发生时我们选择把它加倍。不过，
		 * 从特别小的规模开始也没什么意义。
		 */
		Size		newCacheSize = SizeVfdCache * 2;
		Vfd		   *newVfdCache;

		if (newCacheSize < 32)
			newCacheSize = 32;

		/*
		 * 若 realloc 失败，小心不要覆盖 VfdCache 指针。
		 */
		newVfdCache = (Vfd *) realloc(VfdCache, sizeof(Vfd) * newCacheSize);
		if (newVfdCache == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		VfdCache = newVfdCache;

		/*
		 * 初始化新项并把它们链进空闲链表。
		 */
		for (i = SizeVfdCache; i < newCacheSize; i++)
		{
			MemSet((char *) &(VfdCache[i]), 0, sizeof(Vfd));
			VfdCache[i].nextFree = i + 1;
			VfdCache[i].fd = VFD_CLOSED;
		}
		VfdCache[newCacheSize - 1].nextFree = 0;
		VfdCache[0].nextFree = SizeVfdCache;

		/*
		 * 记录新的大小
		 */
		SizeVfdCache = newCacheSize;
	}

	file = VfdCache[0].nextFree;

	VfdCache[0].nextFree = VfdCache[file].nextFree;

	return file;
}

static void
FreeVfd(File file)
{
	Vfd		   *vfdP = &VfdCache[file];

	DO_DB(elog(LOG, "FreeVfd: %d (%s)",
			   file, vfdP->fileName ? vfdP->fileName : ""));

	if (vfdP->fileName != NULL)
	{
		free(vfdP->fileName);
		vfdP->fileName = NULL;
	}
	vfdP->fdstate = 0x0;

	vfdP->nextFree = VfdCache[0].nextFree;
	VfdCache[0].nextFree = file;
}

/* 成功返回 0，重新打开失败返回 -1（并设置 errno） */
static int
FileAccess(File file)
{
	int			returnValue;

	DO_DB(elog(LOG, "FileAccess %d (%s)",
			   file, VfdCache[file].fileName));

	/*
	 * 文件打开了吗？若没有，就打开它并放到 LRU
	 * 环的头部（必要时关闭最近最少使用的文件以取得文件描述符）。
	 */

	if (FileIsNotOpen(file))
	{
		returnValue = LruInsert(file);
		if (returnValue != 0)
			return returnValue;
	}
	else if (VfdCache[0].lruLessRecently != file)
	{
		/*
		 * 现在我们知道该文件已打开，而且它不是最近
		 * 访问的那个，因此需要把它移到 Lru 环的头部。
		 */

		Delete(file);
		Insert(file);
	}

	return 0;
}

/*
 * 每当删除一个临时文件时调用，以报告其大小。
 */
static void
ReportTemporaryFileUsage(const char *path, off_t size)
{
	pgstat_report_tempfile(size);

	if (log_temp_files >= 0)
	{
		if ((size / 1024) >= log_temp_files)
			ereport(LOG,
					(errmsg("temporary file: path \"%s\", size %lu",
							path, (unsigned long) size)));
	}
}

/*
 * 调用它来登记一个临时文件，以便自动关闭。
 * 打开该文件之前必须已经调用过
 * ResourceOwnerEnlargeFiles(CurrentResourceOwner)。
 */
static void
RegisterTemporaryFile(File file)
{
	ResourceOwnerRememberFile(CurrentResourceOwner, file);
	VfdCache[file].resowner = CurrentResourceOwner;

	/* 事务结束时关闭的后备机制。 */
	VfdCache[file].fdstate |= FD_CLOSE_AT_EOXACT;
	have_xact_temporary_files = true;
}

/*
 *	当我们收到某个关系的共享失效消息时调用。
 */
#ifdef NOT_USED
void
FileInvalidate(File file)
{
	Assert(FileIsValid(file));
	if (!FileIsNotOpen(file))
		LruDelete(file);
}
#endif

/*
 * 用 PathNameOpenFilePerm() 打开文件，并把默认文件模式传给
 * fileMode 参数。
 */
File
PathNameOpenFile(const char *fileName, int fileFlags)
{
	return PathNameOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * 在任意目录里打开一个文件。
 *
 * 注意：若传入的路径名是相对的（通常如此），
 * 它将相对于进程的工作目录来解释
 * （本代码运行时工作目录应当总是 $PGDATA）。
 */
File
PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	char	   *fnamecopy;
	File		file;
	Vfd		   *vfdP;

	DO_DB(elog(LOG, "PathNameOpenFilePerm: %s %x %o",
			   fileName, fileFlags, fileMode));

	/*
	 * 我们需要一份 malloc 出来的文件名副本；若没有空间就干净地失败。
	 */
	fnamecopy = strdup(fileName);
	if (fnamecopy == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	file = AllocateVfd();
	vfdP = &VfdCache[file];

	/* 关闭多余的内核文件描述符。 */
	ReleaseLruFiles();

	/*
	 * 由 VFDs 管理的描述符被隐式标为 O_CLOEXEC。
	 * 客户端不应当被预期知道当前哪些内核描述符
	 * 是打开的，因此让被执行的子程序继承它们
	 * 没有意义。
	 */
	fileFlags |= O_CLOEXEC;

	vfdP->fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (vfdP->fd < 0)
	{
		int			save_errno = errno;

		FreeVfd(file);
		free(fnamecopy);
		errno = save_errno;
		return -1;
	}
	++nfile;
	DO_DB(elog(LOG, "PathNameOpenFile: success %d",
			   vfdP->fd));

	vfdP->fileName = fnamecopy;
	/* 保存的标志已调整为适合重新打开文件 */
	vfdP->fileFlags = fileFlags & ~(O_CREAT | O_TRUNC | O_EXCL);
	vfdP->fileMode = fileMode;
	vfdP->fileSize = 0;
	vfdP->fdstate = 0x0;
	vfdP->resowner = NULL;

	Insert(file);

	return file;
}

/*
 * 创建目录 directory。必要时创建 basedir，它必须
 * 是其上一级目录。这是为了在其下创建目录之前按需创建顶层
 * 临时目录。
 * 若该目录已存在，则什么也不做。
 *
 * 在顶层临时目录里创建的目录应当以
 * PG_TEMP_FILE_PREFIX 开头，以便被识别为临时的，并在
 * 启动时由 RemovePgTempFiles() 删除。再往下的子目录
 * 不需要任何特定前缀。
*/
void
PathNameCreateTemporaryDir(const char *basedir, const char *directory)
{
	if (MakePGDirectory(directory) < 0)
	{
		if (errno == EEXIST)
			return;

		/*
		 * 失败了。先尝试创建 basedir，以防它缺少。容忍
		 * EEXIST，以消除另一个进程遵循同一
		 * 算法时的竞争。
		 */
		if (MakePGDirectory(basedir) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary directory \"%s\": %m",
							basedir)));

		/* 重试。 */
		if (MakePGDirectory(directory) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary subdirectory \"%s\": %m",
							directory)));
	}
}

/*
 * 若目录存在，则删除该目录及其全部内容。
 */
void
PathNameDeleteTemporaryDir(const char *dirname)
{
	struct stat statbuf;

	/* 静默忽略缺少的目录。 */
	if (stat(dirname, &statbuf) != 0 && errno == ENOENT)
		return;

	/*
	 * 目前 walkdir 没有办法让我们传入的函数
	 * 保持状态。也许它应当提供，这样我们就能告诉调用者
	 * 这次操作成功还是失败。由于这个操作
	 * 用在清理路径上，我们实际上不会有不同行为：
	 * 只会记录失败。
	 */
	walkdir(dirname, unlink_if_exists_fname, false, LOG);
}

/*
 * 打开一个关闭时就会消失的临时文件。
 *
 * 本例程负责生成合适的临时文件名。
 * 也不必传入 fileFlags 或 fileMode，因为对临时文件
 * 只有一种设置有意义。
 *
 * 除非 interXact 为真，该文件会被 CurrentResourceOwner
 * 记住，以确保不再需要时关闭并删除它，通常是在
 * 事务末尾。多数情况下，你不希望临时文件
 * 比创建它的事务活得更久，因此这里应当为假；但
 * 若你需要某种程度上临时的存储，这也许有用。无论哪种
 * 情况，当 File 被显式关闭时，该文件都会被删除。
 */
File
OpenTemporaryFile(bool interXact)
{
	File		file = 0;

	Assert(temporary_files_allowed);	/* 检查临时文件访问已经启用 */

	/*
	 * 若下面要登记这个 File，则在打开它之前，确保当前资源所有者
	 * 有容纳它的空间。
	 */
	if (!interXact)
		ResourceOwnerEnlargeFiles(CurrentResourceOwner);

	/*
	 * 若给了我们一些临时表空间，就尝试使用下一个。
	 * 若找不到给定的 tablespace，我们就静默地退回到
	 * 数据库的默认表空间。
	 *
	 * 但是：若该临时文件打算活过当前事务，
	 * 就强制把它放进数据库的默认表空间，这样它就不会
	 * 对可能的删除表空间尝试构成威胁。
	 */
	if (numTempTableSpaces > 0 && !interXact)
	{
		Oid			tblspcOid = GetNextTempTableSpace();

		if (OidIsValid(tblspcOid))
			file = OpenTemporaryFileInTablespace(tblspcOid, false);
	}

	/*
	 * 若不是，或者 tablespace 不好，就在数据库的默认
	 * 表空间里创建。到达这里之前通常应当已经设置 MyDatabaseTableSpace，
	 * 但万一没有，就退回到 pg_default 表空间。
	 */
	if (file <= 0)
		file = OpenTemporaryFileInTablespace(MyDatabaseTableSpace ?
											 MyDatabaseTableSpace :
											 DEFAULTTABLESPACE_OID,
											 true);

	/* 标记为关闭时删除，并计入临时文件大小限制 */
	VfdCache[file].fdstate |= FD_DELETE_AT_CLOSE | FD_TEMP_FILE_LIMIT;

	/* 向当前资源所有者登记它 */
	if (!interXact)
		RegisterTemporaryFile(file);

	return file;
}

/*
 * 返回给定表空间里临时目录的路径。
 */
void
TempTablespacePath(char *path, Oid tablespace)
{
	/*
	 * 识别该表空间的临时文件目录。
	 *
	 * 若有人试图指定 pg_global，则改用 pg_default。
	 */
	if (tablespace == InvalidOid ||
		tablespace == DEFAULTTABLESPACE_OID ||
		tablespace == GLOBALTABLESPACE_OID)
		snprintf(path, MAXPGPATH, "base/%s", PG_TEMP_FILES_DIR);
	else
	{
		/* 所有其他表空间都通过符号链接访问 */
		snprintf(path, MAXPGPATH, "pg_tblspc/%u/%s/%s",
				 tablespace, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
	}
}

/*
 * 在指定表空间里打开一个临时文件。
 * OpenTemporaryFile 的子例程，细节见该函数。
 */
static File
OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError)
{
	char		tempdirpath[MAXPGPATH];
	char		tempfilepath[MAXPGPATH];
	File		file;

	TempTablespacePath(tempdirpath, tblspcOid);

	/*
	 * 生成一个在当前
	 * 数据库实例内应当唯一的临时文件名。
	 */
	snprintf(tempfilepath, sizeof(tempfilepath), "%s/%s%d.%ld",
			 tempdirpath, PG_TEMP_FILE_PREFIX, MyProcPid, tempFileCounter++);

	/*
	 * 打开该文件。注意：我们不用 O_EXCL，以防有一个被遗弃的
	 * 临时文件可以重用。
	 */
	file = PathNameOpenFile(tempfilepath,
							O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		/*
		 * 若还没有人创建过该表空间的临时文件目录，
		 * 我们可能需要创建它。
		 *
		 * 不要检查 MakePGDirectory 的错误；若刚好有
		 * 别人做了同样的事，它可能失败。若它不行，
		 * 我们会在第二次创建尝试时失败。
		 */
		(void) MakePGDirectory(tempdirpath);

		file = PathNameOpenFile(tempfilepath,
								O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
		if (file <= 0 && rejectError)
			elog(ERROR, "could not create temporary file \"%s\": %m",
				 tempfilepath);
	}

	return file;
}


/*
 * 创建一个新文件。包含它的目录必须已存在。这样
 * 创建的文件受 temp_file_limit 约束，并在事务
 * 结束时自动关闭，但关闭时不会自动删除，
 * 因为它们打算在协作的后端之间共享。
 *
 * 若该文件在顶层临时目录里，其名字应当
 * 以 PG_TEMP_FILE_PREFIX 开头，以便被识别为临时的，
 * 并在启动时由 RemovePgTempFiles() 删除。或者，它可以
 * 位于用 PathNameCreateTemporaryDir() 创建的目录里，
 * 此时不需要该前缀。
 */
File
PathNameCreateTemporaryFile(const char *path, bool error_on_failure)
{
	File		file;

	Assert(temporary_files_allowed);	/* 检查临时文件访问已经启用 */

	ResourceOwnerEnlargeFiles(CurrentResourceOwner);

	/*
	 * 打开该文件。注意：我们不用 O_EXCL，以防有一个被遗弃的
	 * 临时文件可以重用。
	 */
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		if (error_on_failure)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create temporary file \"%s\": %m",
							path)));
		else
			return file;
	}

	/* 标记它以便计入 temp_file_limit。 */
	VfdCache[file].fdstate |= FD_TEMP_FILE_LIMIT;

	/* 登记它以便自动关闭。 */
	RegisterTemporaryFile(file);

	return file;
}

/*
 * 打开一个由 PathNameCreateTemporaryFile 创建的文件，可能是在
 * 另一个后端里。这样打开的文件不计入调用者的
 * temp_file_limit，在事务结束时自动关闭，
 * 但关闭时不删除。
 */
File
PathNameOpenTemporaryFile(const char *path, int mode)
{
	File		file;

	Assert(temporary_files_allowed);	/* 检查临时文件访问已经启用 */

	ResourceOwnerEnlargeFiles(CurrentResourceOwner);

	file = PathNameOpenFile(path, mode | PG_BINARY);

	/* 若没有这样的文件，我们就不报错。 */
	if (file <= 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open temporary file \"%s\": %m",
						path)));

	if (file > 0)
	{
		/* 登记它以便自动关闭。 */
		RegisterTemporaryFile(file);
	}

	return file;
}

/*
 * 按路径名删除文件。若文件存在则返回真，若不
 * 存在则返回假。
 */
bool
PathNameDeleteTemporaryFile(const char *path, bool error_on_failure)
{
	struct stat filestats;
	int			stat_errno;

	/* 取得最终大小，供 pgstat 报告。 */
	if (stat(path, &filestats) != 0)
		stat_errno = errno;
	else
		stat_errno = 0;

	/*
	 * 与 FileClose 的自动删除文件代码不同，我们容忍
	 * 不存在，以支持 BufFileDeleteFileSet，它在用完之前
	 * 不知道要删除多少段。
	 */
	if (stat_errno == ENOENT)
		return false;

	if (unlink(path) < 0)
	{
		if (errno != ENOENT)
			ereport(error_on_failure ? ERROR : LOG,
					(errcode_for_file_access(),
					 errmsg("could not unlink temporary file \"%s\": %m",
							path)));
		return false;
	}

	if (stat_errno == 0)
		ReportTemporaryFileUsage(path, filestats.st_size);
	else
	{
		errno = stat_errno;
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}

	return true;
}

/*
 * 用完后关闭文件
 */
void
FileClose(File file)
{
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileClose: %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (!FileIsNotOpen(file))
	{
		/* 关闭该文件 */
		if (close(vfdP->fd) != 0)
		{
			/*
			 * 关闭非临时文件失败时我们可能需要恐慌；
			 * 见 LruDelete。
			 */
			elog(vfdP->fdstate & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
				 "could not close file \"%s\": %m", vfdP->fileName);
		}

		--nfile;
		vfdP->fd = VFD_CLOSED;

		/* 把该文件从 lru 环里去掉 */
		Delete(file);
	}

	if (vfdP->fdstate & FD_TEMP_FILE_LIMIT)
	{
		/* 从当前用量里减去它的大小（出错时先做这个） */
		temporary_files_size -= vfdP->fileSize;
		vfdP->fileSize = 0;
	}

	/*
	 * 若它是临时文件就删除，并在需要时写一条日志
	 */
	if (vfdP->fdstate & FD_DELETE_AT_CLOSE)
	{
		struct stat filestats;
		int			stat_errno;

		/*
		 * 若我们得到错误，例如可能发生在 ereport/elog 调用内部，
		 * 事务中止期间会马上回到这里。重置该
		 * 标志，以确保我们没法陷入死循环。这段代码
		 * 的安排保证最坏后果是没能发出
		 * 日志消息，而不是没能尝试 unlink。
		 */
		vfdP->fdstate &= ~FD_DELETE_AT_CLOSE;


		/* 先尝试 stat() */
		if (stat(vfdP->fileName, &filestats))
			stat_errno = errno;
		else
			stat_errno = 0;

		/* 无论如何都做 unlink */
		if (unlink(vfdP->fileName))
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not delete file \"%s\": %m", vfdP->fileName)));

		/* 最后报告 stat 的结果 */
		if (stat_errno == 0)
			ReportTemporaryFileUsage(vfdP->fileName, filestats.st_size);
		else
		{
			errno = stat_errno;
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", vfdP->fileName)));
		}
	}

	/* 从资源所有者那里注销它 */
	if (vfdP->resowner)
		ResourceOwnerForgetFile(vfdP->resowner, file);

	/*
	 * 把该 Vfd 槽归还到空闲链表
	 */
	FreeVfd(file);
}

/*
 * FilePrefetch - 对文件的给定范围发起异步读取。
 *
 * 目前本函数的唯一实现是使用 posix_fadvise，
 * 这是完成此事的最简单的标准化接口。
 * 将来我们可以增加使用 libaio 的实现；但要注意，
 * 这个 API 不适合 libaio，后者想要提供一个缓冲区
 * 来读入。
 */
int
FilePrefetch(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_WILLNEED)
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FilePrefetch: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = posix_fadvise(VfdCache[file].fd, offset, amount,
							   POSIX_FADV_WILLNEED);
	pgstat_report_wait_end();

	if (returnCode == EINTR)
		goto retry;

	return returnCode;
#else
	Assert(FileIsValid(file));
	return 0;
#endif
}

void
FileWriteback(File file, off_t offset, off_t nbytes, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWriteback: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) nbytes));

	if (nbytes <= 0)
		return;

	if (VfdCache[file].fileFlags & PG_O_DIRECT)
		return;

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return;

	pgstat_report_wait_start(wait_event_info);
	pg_flush_data(VfdCache[file].fd, offset, nbytes);
	pgstat_report_wait_end();
}

int
FileRead(File file, void *buffer, size_t amount, off_t offset,
		 uint32 wait_event_info)
{
	int			returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileRead: %d (%s) " INT64_FORMAT " %zu %p",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   amount, buffer));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_pread(vfdP->fd, buffer, amount, offset);
	pgstat_report_wait_end();

	if (returnCode < 0)
	{
		/*
		 * Windows 可能会用尽内核缓冲区并返回系统资源
		 * 不足的错误。稍等再重试就能解决。
		 *
		 * 据说在某些 Unix 文件系统上也可能出现 EINTR，
		 * 那种情况下应当立即重试。
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* 若被中断，可以重试 */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

int
FileWrite(File file, const void *buffer, size_t amount, off_t offset,
		  uint32 wait_event_info)
{
	int			returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWrite: %d (%s) " INT64_FORMAT " %zu %p",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   amount, buffer));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

	/*
	 * 若正在强制 temp_file_limit 且这是临时文件，就检查
	 * 这次写是否会超出 temp_file_limit，若会则抛出错误。注意：在这里
	 * 抛出错误其实违反了模块边界；我们应当设置 errno
	 * 并返回 -1。然而那样就没法报告合适的错误
	 * 消息。目前所有调用者反正都会立刻
	 * 抛出错误，因此眼下这样做是安全的。
	 */
	if (temp_file_limit >= 0 && (vfdP->fdstate & FD_TEMP_FILE_LIMIT))
	{
		off_t		past_write = offset + amount;

		if (past_write > vfdP->fileSize)
		{
			uint64		newTotal = temporary_files_size;

			newTotal += past_write - vfdP->fileSize;
			if (newTotal > (uint64) temp_file_limit * (uint64) 1024)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
						 errmsg("temporary file size exceeds temp_file_limit (%dkB)",
								temp_file_limit)));
		}
	}

retry:
	errno = 0;
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_pwrite(VfdCache[file].fd, buffer, amount, offset);
	pgstat_report_wait_end();

	/* 若写操作没有设置 errno，就假定问题是磁盘空间不足 */
	if (returnCode != amount && errno == 0)
		errno = ENOSPC;

	if (returnCode >= 0)
	{
		/*
		 * 若是临时文件，则维护 fileSize 和 temporary_files_size。
		 */
		if (vfdP->fdstate & FD_TEMP_FILE_LIMIT)
		{
			off_t		past_write = offset + amount;

			if (past_write > vfdP->fileSize)
			{
				temporary_files_size += past_write - vfdP->fileSize;
				vfdP->fileSize = past_write;
			}
		}
	}
	else
	{
		/*
		 * 见 FileRead() 里的注释
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* 若被中断，可以重试 */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

int
FileSync(File file, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSync: %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_fsync(VfdCache[file].fd);
	pgstat_report_wait_end();

	return returnCode;
}

/*
 * 把文件的一个区域清零。
 *
 * 成功返回 0，否则返回 -1。后一种情况下 errno 被设为
 * 相应的错误。
 */
int
FileZero(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
	int			returnCode;
	ssize_t		written;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileZero: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	written = pg_pwrite_zeros(VfdCache[file].fd, amount, offset);
	pgstat_report_wait_end();

	if (written < 0)
		return -1;
	else if (written != amount)
	{
		/* 若 errno 未设置，就假定问题是磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		return -1;
	}

	return 0;
}

/*
 * 尝试用 posix_fallocate() 预留文件空间。若操作系统没有实现
 * posix_fallocate()，或它以 EINVAL / EOPNOTSUPP 失败，
 * 则改用 FileZero()。
 *
 * 注意，至少 glibc() 会在文件系统未实现时于用户空间实现 posix_fallocate()。
 * 不过并非所有环境
 * 都如此。
 *
 * 成功返回 0，否则返回 -1。后一种情况下 errno 被设为
 * 相应的错误。
 */
int
FileFallocate(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
#ifdef HAVE_POSIX_FALLOCATE
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileFallocate: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return -1;

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = posix_fallocate(VfdCache[file].fd, offset, amount);
	pgstat_report_wait_end();

	if (returnCode == 0)
		return 0;
	else if (returnCode == EINTR)
		goto retry;

	/* 为了与 %m 打印等兼容 */
	errno = returnCode;

	/*
	 * Return in cases of a "real" failure, if fallocate is not supported,
	 * fall through to the FileZero() backed implementation.
	 */
	if (returnCode != EINVAL && returnCode != EOPNOTSUPP)
		return -1;
#endif

	return FileZero(file, offset, amount, wait_event_info);
}

off_t
FileSize(File file)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSize %d (%s)",
			   file, VfdCache[file].fileName));

	if (FileIsNotOpen(file))
	{
		if (FileAccess(file) < 0)
			return (off_t) -1;
	}

	return lseek(VfdCache[file].fd, 0, SEEK_END);
}

int
FileTruncate(File file, off_t offset, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileTruncate %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_ftruncate(VfdCache[file].fd, offset);
	pgstat_report_wait_end();

	if (returnCode == 0 && VfdCache[file].fileSize > offset)
	{
		/* 若截断的是临时文件，则调整我们的状态 */
		Assert(VfdCache[file].fdstate & FD_TEMP_FILE_LIMIT);
		temporary_files_size -= VfdCache[file].fileSize - offset;
		VfdCache[file].fileSize = offset;
	}

	return returnCode;
}

/*
 * 返回与已打开文件关联的路径名。
 *
 * 返回的字符串指向一个内部缓冲区，在
 * 该文件关闭之前一直有效。
 */
char *
FilePathName(File file)
{
	Assert(FileIsValid(file));

	return VfdCache[file].fileName;
}

/*
 * 返回已打开文件的原始文件描述符。
 *
 * 返回的文件描述符在该文件关闭之前一直有效，但
 * 有很多事情会导致关闭。因此调用者应当
 * 小心，在用完返回的
 * 文件描述符之前不要做太多别的事。
 */
int
FileGetRawDesc(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fd;
}

/*
 * FileGetRawFlags - 返回 open(2) 时的文件标志
 */
int
FileGetRawFlags(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileFlags;
}

/*
 * FileGetRawMode - 返回传给 open(2) 的模式位掩码
 */
mode_t
FileGetRawMode(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileMode;
}

/*
 * 必要时并且可能的话，为 allocatedDescs[] 数组再腾出一个项。
 * 若有数组元素可用则返回真。
 */
static bool
reserveAllocatedDesc(void)
{
	AllocateDesc *newDescs;
	int			newMax;

	/* 若数组里已经有空闲槽，就快速返回。 */
	if (numAllocatedDescs < maxAllocatedDescs)
		return true;

	/*
	 * 若当前进程里还没创建该数组，就用
	 * FD_MINFREE / 3 个元素初始化它。在许多情形下，这反正就是
	 * 我们永远需要的那么多。我们不想立刻去看 max_safe_fds，
	 * 因为 set_max_safe_fds() 可能还没运行。
	 */
	if (allocatedDescs == NULL)
	{
		newMax = FD_MINFREE / 3;
		newDescs = (AllocateDesc *) malloc(newMax * sizeof(AllocateDesc));
		/* 已经内存不足？当作致命错误处理。 */
		if (newDescs == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/*
	 * 考虑把数组扩大到超出上面所用的初始分配。
	 * 到这时，max_safe_fds 应当已经准确知道。
	 *
	 * 我们不得让已分配的描述符占光所有可用文件描述符，实践中
	 * 最好为 VFD 留出合理数量的文件描述符。因此
	 * 把最大值设为 max_safe_fds / 3。（这肯定应当
	 * 至少和初始大小 FD_MINFREE / 3 一样大，所以这里并没有
	 * 收紧限制。）记住，外部文件描述符
	 * 还可以再消耗 max_safe_fds 的三分之一。
	 */
	newMax = max_safe_fds / 3;
	if (newMax > maxAllocatedDescs)
	{
		newDescs = (AllocateDesc *) realloc(allocatedDescs,
											newMax * sizeof(AllocateDesc));
		/* 把内存不足当作非致命错误处理。 */
		if (newDescs == NULL)
			return false;
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/* 无法再扩大 allocatedDescs[]。 */
	return false;
}

/*
 * 想使用 stdio（即 FILE*）的例程应当使用 AllocateFile，
 * 而不是普通的 fopen()。这样 fd.c 就能在必要时释放文件描述符
 * 来打开该文件。用完后调用 FreeFile，而不是 fclose。
 *
 * 注意，会打开任何可观时长的文件
 * 不应当用这种方式处理，因为它们无法与其他文件共享内核文件
 * 描述符；若有人锁住过多文件描述符，
 * 就有耗尽文件描述符的严重风险。本例程的多数调用者
 * 只是在读一个马上就会读完并关闭的配置文件。
 *
 * fd.c 会在事务提交或撤销时自动关闭所有用 AllocateFile
 * 打开的文件；这样，若调用 AllocateFile 的例程
 * 被 ereport(ERROR) 提前打断，就不会泄漏文件描述符。
 *
 * 理想情况下，这应当是后端里对 fopen() 的唯一直接调用。
 */
FILE *
AllocateFile(const char *name, const char *mode)
{
	FILE	   *file;

	DO_DB(elog(LOG, "AllocateFile: Allocated %d (%s)",
			   numAllocatedDescs, name));

	/* 我们还能再分配一个非虚拟文件描述符吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, name)));

	/* 关闭多余的内核文件描述符。 */
	ReleaseLruFiles();

TryAgain:
	if ((file = fopen(name, mode)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescFile;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * 用 OpenTransientFilePerm() 打开文件，并把默认文件模式传给
 * fileMode 参数。
 */
int
OpenTransientFile(const char *fileName, int fileFlags)
{
	return OpenTransientFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * 与 AllocateFile 类似，但返回像 open(2) 那样无缓冲的文件描述符
 */
int
OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

	DO_DB(elog(LOG, "OpenTransientFile: Allocated %d (%s)",
			   numAllocatedDescs, fileName));

	/* 我们还能再分配一个非虚拟文件描述符吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, fileName)));

	/* 关闭多余的内核文件描述符。 */
	ReleaseLruFiles();

	fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (fd >= 0)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescRawFD;
		desc->desc.fd = fd;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;

		return fd;
	}

	return -1;					/* 失败 */
}

/*
 * 想发起管道流的例程应当使用 OpenPipeStream，
 * 而不是普通的 popen()。这样 fd.c 就能在必要时释放文件描述符。
 * 用完后调用 ClosePipeStream，而不是 pclose。
 *
 * 本函数还确保 popen 出来的程序以默认的
 * SIGPIPE 处理来运行，而不是后端通常使用的 SIG_IGN 设置。
 * 这保证了对例如过早关闭读管道的期望响应。
 */
FILE *
OpenPipeStream(const char *command, const char *mode)
{
	FILE	   *file;
	int			save_errno;

	DO_DB(elog(LOG, "OpenPipeStream: Allocated %d (%s)",
			   numAllocatedDescs, command));

	/* 我们还能再分配一个非虚拟文件描述符吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to execute command \"%s\"",
						maxAllocatedDescs, command)));

	/* 关闭多余的内核文件描述符。 */
	ReleaseLruFiles();

TryAgain:
	fflush(NULL);
	pqsignal(SIGPIPE, SIG_DFL);
	errno = 0;
	file = popen(command, mode);
	save_errno = errno;
	pqsignal(SIGPIPE, SIG_IGN);
	errno = save_errno;
	if (file != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescPipe;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * 释放任意类型的 AllocateDesc。
 *
 * 参数必须指向 allocatedDescs[] 数组内部。
 */
static int
FreeDesc(AllocateDesc *desc)
{
	int			result;

	/* 关闭底层对象 */
	switch (desc->kind)
	{
		case AllocateDescFile:
			result = fclose(desc->desc.file);
			break;
		case AllocateDescPipe:
			result = pclose(desc->desc.file);
			break;
		case AllocateDescDir:
			result = closedir(desc->desc.dir);
			break;
		case AllocateDescRawFD:
			result = close(desc->desc.fd);
			break;
		default:
			elog(ERROR, "AllocateDesc kind not recognized");
			result = 0;			/* 消除编译器告警 */
			break;
	}

	/* 紧缩 allocatedDescs 数组里的存储 */
	numAllocatedDescs--;
	*desc = allocatedDescs[numAllocatedDescs];

	return result;
}

/*
 * 关闭 AllocateFile 返回的文件。
 *
 * 注意我们不检查 fclose 的返回值，由调用者
 * 来处理关闭错误。
 */
int
FreeFile(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "FreeFile: Allocated %d", numAllocatedDescs));

	/* 若该文件在已分配文件列表里，就把它去掉 */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescFile && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* 只有当有人传给我们一个不在 allocatedDescs 里的文件时才会到这里 */
	elog(WARNING, "file passed to FreeFile was not obtained from AllocateFile");

	return fclose(file);
}

/*
 * 关闭 OpenTransientFile 返回的文件。
 *
 * 注意我们不检查 close 的返回值，由调用者
 * 来处理关闭错误。
 */
int
CloseTransientFile(int fd)
{
	int			i;

	DO_DB(elog(LOG, "CloseTransientFile: Allocated %d", numAllocatedDescs));

	/* 若该文件描述符在已分配文件列表里，就把它去掉 */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescRawFD && desc->desc.fd == fd)
			return FreeDesc(desc);
	}

	/* 只有当有人传给我们一个不在 allocatedDescs 里的文件时才会到这里 */
	elog(WARNING, "fd passed to CloseTransientFile was not obtained from OpenTransientFile");

	return close(fd);
}

/*
 * 想使用 <dirent.h>（即 DIR*）的例程应当使用 AllocateDir，
 * 而不是普通的 opendir()。这样 fd.c 就能在必要时释放文件描述符
 * 来打开该目录，并在 elog 之后关闭它。
 * 用完后调用 FreeDir，而不是 closedir。
 *
 * 失败时返回 NULL，并设置 errno。注意，失败检测
 * 通常留给随后的 ReadDir 或 ReadDirExtended 调用；
 * 见 ReadDir 的注释。
 *
 * 理想情况下，这应当是后端里对 opendir() 的唯一直接调用。
 */
DIR *
AllocateDir(const char *dirname)
{
	DIR		   *dir;

	DO_DB(elog(LOG, "AllocateDir: Allocated %d (%s)",
			   numAllocatedDescs, dirname));

	/* 我们还能再分配一个非虚拟文件描述符吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open directory \"%s\"",
						maxAllocatedDescs, dirname)));

	/* 关闭多余的内核文件描述符。 */
	ReleaseLruFiles();

TryAgain:
	if ((dir = opendir(dirname)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescDir;
		desc->desc.dir = dir;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.dir;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * 读取用 AllocateDir 打开的目录，并用 ereport 报告任何错误。
 *
 * 这比原始的 readdir() 更容易使用，因为它处理了一些
 * 原本相当繁琐且容易出错的 errno 操作。另外，
 * 若你满意 AllocateDir 失败时的通用错误消息，
 * 就可以直接写成：
 *
 *		dir = AllocateDir(path);
 *		while ((dirent = ReadDir(dir, path)) != NULL)
 *			处理 dirent;
 *		FreeDir(dir)。
 *
 * 因为 dir 参数为 NULL 被当作表示 AllocateDir 失败。
 * （若使用这个简写，请确保在 AllocateDir 和 ReadDir 之间
 * 不要改变 errno。）
 *
 * 传给 AllocateDir 的路径名也必须传给本例程，
 * 但它只用于错误报告。
 */
struct dirent *
ReadDir(DIR *dir, const char *dirname)
{
	return ReadDirExtended(dir, dirname, ERROR);
}

/*
 * ReadDir 的另一个版本，允许调用者指定任何错误报告的
 * elevel（无论是报告 AllocateDir 的初始失败，
 * 还是随后的目录读取失败）。
 *
 * 若 elevel < ERROR，则任何错误之后都返回 NULL。按通常的编码
 * 模式，这会导致立刻退出循环，就好像
 * 该目录不再包含更多项。
 */
struct dirent *
ReadDirExtended(DIR *dir, const char *dirname, int elevel)
{
	struct dirent *dent;

	/* 若调用者没有给出，就为 AllocateDir 失败给出一条通用消息 */
	if (dir == NULL)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						dirname)));
		return NULL;
	}

	errno = 0;
	if ((dent = readdir(dir)) != NULL)
		return dent;

	if (errno)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m",
						dirname)));
	return NULL;
}

/*
 * 关闭用 AllocateDir 打开的目录。
 *
 * 返回 closedir 的返回值（若不是 0 则设置了 errno）。
 * 注意我们不检查返回值，由调用者
 * 在需要时处理关闭错误。
 *
 * 若 dir == NULL 则什么也不做；我们假定目录打开失败
 * 在需要时已经报告过了。
 */
int
FreeDir(DIR *dir)
{
	int			i;

	/* 若 AllocateDir 失败，则无事可做 */
	if (dir == NULL)
		return 0;

	DO_DB(elog(LOG, "FreeDir: Allocated %d", numAllocatedDescs));

	/* 若该目录在已分配目录列表里，就把它去掉 */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescDir && desc->desc.dir == dir)
			return FreeDesc(desc);
	}

	/* 只有当有人传给我们一个不在 allocatedDescs 里的目录时才会到这里 */
	elog(WARNING, "dir passed to FreeDir was not obtained from AllocateDir");

	return closedir(dir);
}


/*
 * 关闭 OpenPipeStream 返回的管道流。
 */
int
ClosePipeStream(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "ClosePipeStream: Allocated %d", numAllocatedDescs));

	/* 若该文件在已分配文件列表里，就把它去掉 */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescPipe && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* 只有当有人传给我们一个不在 allocatedDescs 里的文件时才会到这里 */
	elog(WARNING, "file passed to ClosePipeStream was not obtained from OpenPipeStream");

	return pclose(file);
}

/*
 * closeAllVfds
 *
 * 强制所有 VFDs 进入物理关闭状态，从而使正在使用的
 * 内核文件描述符尽可能少。VFDs 的逻辑状态
 * 没有变化。
 */
void
closeAllVfds(void)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));	/* 确保环没有损坏 */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if (!FileIsNotOpen(i))
				LruDelete(i);
		}
	}
}


/*
 * SetTempTablespaces
 *
 * 定义一个用于临时文件的表空间 OIDs 列表（实际上是数组）。
 * 这个列表会一直用到事务结束，
 * 除非在那之前再次调用本函数。调用者
 * 有责任使传入的数组有足够的寿命（通常
 * 它会分配在 TopTransactionContext 里）。
 *
 * 数组的某些项可能是 InvalidOid，表示应当使用当前
 * 数据库的默认表空间。
 */
void
SetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	Assert(numSpaces >= 0);
	tempTableSpaces = tableSpaces;
	numTempTableSpaces = numSpaces;

	/*
	 * 在列表里选一个随机起点。这是为了尽量减少
	 * 最可能共享同一临时表空间列表的后端之间的
	 * 冲突。注意，若我们在同一事务里创建多个临时文件，
	 * 就会沿列表循环前进。这
	 * 确保大型临时排序文件能很好地分布到所有
	 * 可用的表空间上。
	 */
	if (numSpaces > 1)
		nextTempTableSpace = pg_prng_uint64_range(&pg_global_prng_state,
												  0, numSpaces - 1);
	else
		nextTempTableSpace = 0;
}

/*
 * TempTablespacesAreSet
 *
 * 若当前事务里已经调用过 SetTempTablespaces，则返回真。
 * （这只是为了让 tablespaces.c 不必有自己的每事务
 * 状态。）
 */
bool
TempTablespacesAreSet(void)
{
	return (numTempTableSpaces >= 0);
}

/*
 * GetTempTablespaces
 *
 * 用应当用于临时文件的表空间 OIDs 填充一个数组。
 * （有些项可能是 InvalidOid，表示应当使用
 * 当前数据库的默认表空间。）最多填充 numSpaces
 * 项。
 * 返回复制到输出数组里的 OID 个数。
 */
int
GetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	int			i;

	Assert(TempTablespacesAreSet());
	for (i = 0; i < numTempTableSpaces && i < numSpaces; ++i)
		tableSpaces[i] = tempTableSpaces[i];

	return i;
}

/*
 * GetNextTempTableSpace
 *
 * 选择下一个要使用的临时表空间。结果为 InvalidOid 表示
 * 使用当前数据库的默认表空间。
 */
Oid
GetNextTempTableSpace(void)
{
	if (numTempTableSpaces > 0)
	{
		/* 推进 nextTempTableSpace 计数器，并在末尾回绕 */
		if (++nextTempTableSpace >= numTempTableSpaces)
			nextTempTableSpace = 0;
		return tempTableSpaces[nextTempTableSpace];
	}
	return InvalidOid;
}


/*
 * AtEOSubXact_Files
 *
 * 处理子事务的提交或中止。中止时，我们关闭该子事务
 * 可能打开的临时文件。提交时，我们把
 * 已打开的文件重新赋给父级子事务。
 */
void
AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
				  SubTransactionId parentSubid)
{
	Index		i;

	for (i = 0; i < numAllocatedDescs; i++)
	{
		if (allocatedDescs[i].create_subid == mySubid)
		{
			if (isCommit)
				allocatedDescs[i].create_subid = parentSubid;
			else
			{
				/* FreeDesc 之后必须重新检查该项（不漂亮） */
				FreeDesc(&allocatedDescs[i--]);
			}
		}
	}
}

/*
 * AtEOXact_Files
 *
 * 本例程在事务提交或中止期间调用。所有仍打开的
 * 每事务临时文件 VFDs 都会关闭，这也会导致
 * 底层文件被删除（尽管它们应当已经由
 * ResourceOwner 清理关闭了）。此外，所有已分配的 stdio 文件都会
 * 被关闭。我们还忘掉任何事务局部的临时表空间列表。
 *
 * isCommit 标志只用来决定是否对
 * 未关闭的文件发出警告。
 */
void
AtEOXact_Files(bool isCommit)
{
	CleanupTempFiles(isCommit, false);
	tempTableSpaces = NULL;
	numTempTableSpaces = -1;
}

/*
 * BeforeShmemExit_Files
 *
 * before_shmem_exit 钩子，在后端关闭期间清理临时文件。
 * 这里我们要清理包括 interXact 在内的全部临时文件。
 */
static void
BeforeShmemExit_Files(int code, Datum arg)
{
	CleanupTempFiles(false, true);

	/* 阻止再创建临时文件 */
#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = false;
#endif
}

/*
 * 关闭临时文件并删除其底层文件。
 *
 * isCommit：若为真，这是正常的事务提交，我们不
 * 预期还会剩下文件；若还有就警告。
 *
 * isProcExit：若为真，这是在后端进程
 * 退出时调用。若是这种情况，我们应当去掉所有临时文件；若
 * 不是，则我们是在为事务提交或中止而调用，
 * 应当只去掉事务局部的临时文件。无论哪种情况，
 * 也要清理已分配的 stdio 文件、目录和文件描述符。
 */
static void
CleanupTempFiles(bool isCommit, bool isProcExit)
{
	Index		i;

	/*
	 * 这里要小心：在 proc_exit 时我们需要额外清理，不只是
	 * xact_temporary 临时文件。
	 */
	if (isProcExit || have_xact_temporary_files)
	{
		Assert(FileIsNotOpen(0));	/* 确保环没有损坏 */
		for (i = 1; i < SizeVfdCache; i++)
		{
			unsigned short fdstate = VfdCache[i].fdstate;

			if (((fdstate & FD_DELETE_AT_CLOSE) || (fdstate & FD_CLOSE_AT_EOXACT)) &&
				VfdCache[i].fileName != NULL)
			{
				/*
				 * 若我们正在退出一个后端进程，就关闭
				 * 所有临时文件。否则只关闭当前事务
				 * 局部的临时文件。它们应当已经由
				 * ResourceOwner 机制关闭，因此这只是
				 * 一道调试用的交叉检查。
				 */
				if (isProcExit)
					FileClose(i);
				else if (fdstate & FD_CLOSE_AT_EOXACT)
				{
					elog(WARNING,
						 "temporary file %s not closed at end-of-transaction",
						 VfdCache[i].fileName);
					FileClose(i);
				}
			}
		}

		have_xact_temporary_files = false;
	}

	/* 若提交时仍有已分配的文件打开，就抱怨。 */
	if (isCommit && numAllocatedDescs > 0)
		elog(WARNING, "%d temporary files and directories not closed at end-of-transaction",
			 numAllocatedDescs);

	/* 清理已分配的 stdio 文件、目录和文件描述符。 */
	while (numAllocatedDescs > 0)
		FreeDesc(&allocatedDescs[0]);
}


/*
 * 去掉先前
 * postmaster 会话留下的临时文件和临时关系文件。
 *
 * 这应当在 postmaster 启动期间调用。它会强制
 * 去掉 OpenTemporaryFile 与 mdcreate 留下的
 * 临时关系文件和临时文件。
 *
 * 在后端崩溃后的重启循环里，当
 * remove_temp_files_after_crash 这个 GUC 启用时会调用本例程。
 * 查询使用临时文件时多次崩溃，可能导致无用的存储占用，
 * 只能靠服务重启来回收。反对启用它的理由是，
 * 有人可能想检查临时文件以便调试。不过这确实意味着
 * OpenTemporaryFile 最好能容忍与已有
 * 临时文件名的冲突。
 *
 * 注意：本函数及其子例程通常用 ereport(LOG)
 * 报告系统调用失败并继续。去掉临时文件并没有重要到
 * 我们做不到时就不应当启动数据库的程度。
 */
void
RemovePgTempFiles(void)
{
	char		temp_path[MAXPGPATH + 10 + sizeof(TABLESPACE_VERSION_DIRECTORY) + sizeof(PG_TEMP_FILES_DIR)];
	DIR		   *spc_dir;
	struct dirent *spc_de;

	/*
	 * 首先处理 pg_default（$PGDATA/base）里的临时文件
	 */
	snprintf(temp_path, sizeof(temp_path), "base/%s", PG_TEMP_FILES_DIR);
	RemovePgTempFilesInDir(temp_path, true, false);
	RemovePgTempRelationFiles("base");

	/*
	 * 遍历所有非默认表空间的临时目录。
	 */
	spc_dir = AllocateDir("pg_tblspc");

	while ((spc_de = ReadDirExtended(spc_dir, "pg_tblspc", LOG)) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "pg_tblspc/%s/%s/%s",
				 spc_de->d_name, TABLESPACE_VERSION_DIRECTORY, PG_TEMP_FILES_DIR);
		RemovePgTempFilesInDir(temp_path, true, false);

		snprintf(temp_path, sizeof(temp_path), "pg_tblspc/%s/%s",
				 spc_de->d_name, TABLESPACE_VERSION_DIRECTORY);
		RemovePgTempRelationFiles(temp_path);
	}

	FreeDir(spc_dir);

	/*
	 * 在 EXEC_BACKEND 情形下，DataDir 顶层也有一个 pgsql_tmp 目录。
	 * 不过这里不清理它，因为那样做
	 * 会造成竞争。它在 postmaster 启动更早的时候
	 * 单独完成。
	 */
}

/*
 * 为 RemovePgTempFiles 处理一个 pgsql_tmp 目录。
 *
 * 若 missing_ok 为真，指定的目录不存在也没关系。
 * 任何其他问题都会产生一条 LOG 消息。（在顶层 missing_ok 应当为真，
 * 因为 pgsql_tmp 目录直到需要时才创建。）
 *
 * 在顶层，应当以 unlink_all = false 调用，这样
 * 只会 unlink 匹配临时名前缀的文件。递归时
 * 会以 unlink_all = true 调用，以 unlink 顶层临时目录下的
 * 一切。
 *
 * （这两个标志可以合成一个，但分开
 * 似乎更清楚。）
 */
void
RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok, bool unlink_all)
{
	DIR		   *temp_dir;
	struct dirent *temp_de;
	char		rm_path[MAXPGPATH * 2];

	temp_dir = AllocateDir(tmpdirname);

	if (temp_dir == NULL && errno == ENOENT && missing_ok)
		return;

	while ((temp_de = ReadDirExtended(temp_dir, tmpdirname, LOG)) != NULL)
	{
		if (strcmp(temp_de->d_name, ".") == 0 ||
			strcmp(temp_de->d_name, "..") == 0)
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 tmpdirname, temp_de->d_name);

		if (unlink_all ||
			strncmp(temp_de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
		{
			PGFileType	type = get_dirent_type(rm_path, temp_de, false, LOG);

			if (type == PGFILETYPE_ERROR)
				continue;
			else if (type == PGFILETYPE_DIR)
			{
				/* 递归去掉内容，然后去掉目录本身 */
				RemovePgTempFilesInDir(rm_path, false, true);

				if (rmdir(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove directory \"%s\": %m",
									rm_path)));
			}
			else
			{
				if (unlink(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m",
									rm_path)));
			}
		}
		else
			ereport(LOG,
					(errmsg("unexpected file found in temporary-files directory: \"%s\"",
							rm_path)));
	}

	FreeDir(temp_dir);
}

/* 处理一个表空间目录，寻找每个数据库的子目录 */
static void
RemovePgTempRelationFiles(const char *tsdirname)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH * 2];

	ts_dir = AllocateDir(tsdirname);

	while ((de = ReadDirExtended(ts_dir, tsdirname, LOG)) != NULL)
	{
		/*
		 * 我们只关心每个数据库的目录，它们的
		 * 名字是数字。注意这段代码也会（正确地）忽略 .
		 * 和 ..。
		 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);
		RemovePgTempRelationFilesInDbspace(dbspace_path);
	}

	FreeDir(ts_dir);
}

/* 为 RemovePgTempRelationFiles 处理一个每数据库空间目录 */
static void
RemovePgTempRelationFilesInDbspace(const char *dbspacedirname)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH * 2];

	dbspace_dir = AllocateDir(dbspacedirname);

	while ((de = ReadDirExtended(dbspace_dir, dbspacedirname, LOG)) != NULL)
	{
		if (!looks_like_temp_rel_name(de->d_name))
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 dbspacedirname, de->d_name);

		if (unlink(rm_path) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							rm_path)));
	}

	FreeDir(dbspace_dir);
}

/* t<数字>_<数字>，或 t<数字>_<数字>_<forkname> */
bool
looks_like_temp_rel_name(const char *name)
{
	int			pos;
	int			savepos;

	/* 必须以 t 开头。 */
	if (name[0] != 't')
		return false;

	/* 随后是一串非空数字，然后是一个下划线。 */
	for (pos = 1; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (pos == 1 || name[pos] != '_')
		return false;

	/* 随后是另一串非空数字。 */
	for (savepos = ++pos; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (savepos == pos)
		return false;

	/* 我们可能有 _forkname，或 .segment，或两者都有。 */
	if (name[pos] == '_')
	{
		int			forkchar = forkname_chars(&name[pos + 1], NULL);

		if (forkchar <= 0)
			return false;
		pos += forkchar + 1;
	}
	if (name[pos] == '.')
	{
		int			segchar;

		for (segchar = 1; isdigit((unsigned char) name[pos + segchar]); ++segchar)
			;
		if (segchar <= 1)
			return false;
		pos += segchar;
	}

	/* 现在应当到了末尾。 */
	if (name[pos] != '\0')
		return false;
	return true;
}

#ifdef HAVE_SYNCFS
static void
do_syncfs(const char *path)
{
	int			fd;

	ereport_startup_progress("syncing data directory (syncfs), elapsed time: %ld.%02d s, current path: %s",
							 path);

	fd = OpenTransientFile(path, O_RDONLY);
	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
		return;
	}
	if (syncfs(fd) < 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not synchronize file system for file \"%s\": %m", path)));
	CloseTransientFile(fd);
}
#endif

/*
 * 根据 recovery_init_sync_method 的设置，对 PGDATA 及其全部内容
 * 递归发出 fsync，或对所有可能的文件系统发出 syncfs。
 *
 * 无论文件和目录在哪里，我们都对它们做 fsync，但只跟随
 * pg_wal 以及 pg_tblspc 紧下方的符号链接。
 * 其他符号链接被假定指向我们不负责做 fsync 的文件，
 * 而且我们可能根本没有写权限。
 *
 * 错误会被记录，但不视为致命；这只在
 * 数据库启动期间使用，用来处理数据目录上
 * 可能还有已发出但未同步的写。我们希望
 * 这类写在新的一次运行做任何事之前到达磁盘。
 * 然而，出错就中止会导致无害情形也无法启动，
 * 例如数据目录里的只读文件，
 * 那也不好。
 *
 * 注意，若我们先前因 fsync() 上的 PANIC 而崩溃，
 * 恢复期间会再次重写所有更改。
 *
 * 注意，我们假定一开始就已经 chdir 到 PGDATA。
 */
void
SyncDataDirectory(void)
{
	bool		xlog_is_symlink;

	/* 若禁用了 fsync，就可以整个跳过。 */
	if (!enableFsync)
		return;

	/*
	 * 若 pg_wal 是符号链接，我们需要单独递归进入它，
	 * 因为下面第一次 walkdir 会忽略它。
	 */
	xlog_is_symlink = false;

	{
		struct stat st;

		if (lstat("pg_wal", &st) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m",
							"pg_wal")));
		else if (S_ISLNK(st.st_mode))
			xlog_is_symlink = true;
	}

#ifdef HAVE_SYNCFS
	if (recovery_init_sync_method == RECOVERY_INIT_SYNC_METHOD_SYNCFS)
	{
		DIR		   *dir;
		struct dirent *de;

		/*
		 * 在 Linux 上，我们不必逐个打开每一个文件。
		 * 可以用 syncfs() 同步整个文件系统。我们只预期
		 * 文件系统边界出现在我们容忍符号链接的地方，即
		 * pg_wal 和表空间，因此我们对那些
		 * 目录分别调用 syncfs()。
		 */

		/* 准备报告经由 syncfs 同步数据目录的进度。 */
		begin_startup_progress_phase();

		/* 同步顶层 pgdata 目录。 */
		do_syncfs(".");
		/* 若配置了任何表空间，就逐个同步。 */
		dir = AllocateDir("pg_tblspc");
		while ((de = ReadDirExtended(dir, "pg_tblspc", LOG)))
		{
			char		path[MAXPGPATH];

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;

			snprintf(path, MAXPGPATH, "pg_tblspc/%s", de->d_name);
			do_syncfs(path);
		}
		FreeDir(dir);
		/* 若 pg_wal 是符号链接，也处理它。 */
		if (xlog_is_symlink)
			do_syncfs("pg_wal");
		return;
	}
#endif							/* 没有 HAVE_SYNCFS */

#ifdef PG_FLUSH_DATA_WORKS
	/* 准备报告 fsync 之前阶段的进度。 */
	begin_startup_progress_phase();

	/*
	 * 若可能，提示内核我们很快就要对数据
	 * 目录及其内容做 fsync。这一步的错误比平常
	 * 更不值得关心，因此只在 DEBUG1 记录它们。
	 */
	walkdir(".", pre_sync_fname, false, DEBUG1);
	if (xlog_is_symlink)
		walkdir("pg_wal", pre_sync_fname, false, DEBUG1);
	walkdir("pg_tblspc", pre_sync_fname, true, DEBUG1);
#endif

	/* 准备报告经由 fsync 同步数据目录的进度。 */
	begin_startup_progress_phase();

	/*
	 * 现在我们按同样的顺序做 fsync()。
	 *
	 * 主调用会忽略符号链接，因此除了在 pg_wal 是符号链接时
	 * 专门处理它之外，还必须单独访问 pg_tblspc，并令
	 * process_symlinks = true。注意，若 pg_tblspc 里有任何普通目录，
	 * 它们会被 fsync 两次。那不是预期情形，
	 * 所以我们不操心去优化它。
	 */
	walkdir(".", datadir_fsync_fname, false, LOG);
	if (xlog_is_symlink)
		walkdir("pg_wal", datadir_fsync_fname, false, LOG);
	walkdir("pg_tblspc", datadir_fsync_fname, true, LOG);
}

/*
 * walkdir：递归遍历一个目录，对每个
 * 普通文件和目录（包括指名的目录本身）施加动作。
 *
 * 若 process_symlinks 为真，动作和递归也施加于
 * 给定目录里符号链接所指向的普通文件和目录；
 * 否则忽略符号链接。子目录里的符号链接始终
 * 被忽略，也就是我们有意不把
 * process_symlinks 标志传给递归调用。
 *
 * 错误按 elevel 报告，它可以是 ERROR 或更低。
 *
 * 另见 file_utils.c 里的 walkdir，那是这段
 * 逻辑的前端版本。
 */
static void
walkdir(const char *path,
		void (*action) (const char *fname, bool isdir, int elevel),
		bool process_symlinks,
		int elevel)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(path);

	while ((de = ReadDirExtended(dir, path, elevel)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];

		CHECK_FOR_INTERRUPTS();

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		switch (get_dirent_type(subpath, de, process_symlinks, elevel))
		{
			case PGFILETYPE_REG:
				(*action) (subpath, false, elevel);
				break;
			case PGFILETYPE_DIR:
				walkdir(subpath, action, false, elevel);
				break;
			default:

				/*
				 * 错误已经由 get_dirent_type() 直接报告，
				 * 剩下的符号链接和未知文件类型都会
				 * 被忽略。
				 */
				break;
		}
	}

	FreeDir(dir);				/* 这里我们忽略任何错误 */

	/*
	 * 对目标目录本身做 fsync 很重要，因为对单个
	 * 文件做 fsync 并不保证该文件的目录项已经
	 * 同步。不过，若 AllocateDir 失败就跳过这一步；动作函数
	 * 对那种情况可能不够稳健。
	 */
	if (dir)
		(*action) (path, true, elevel);
}


/*
 * 提示操作系统应当准备好对该文件做 fsync()。
 *
 * 忽略打开不可读文件时的错误，并把其他错误按
 * 调用者指定的级别记录。
 */
#ifdef PG_FLUSH_DATA_WORKS

static void
pre_sync_fname(const char *fname, bool isdir, int elevel)
{
	int			fd;

	/* 不要尝试刷出目录，那多半只会失败 */
	if (isdir)
		return;

	ereport_startup_progress("syncing data directory (pre-fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	fd = OpenTransientFile(fname, O_RDONLY | PG_BINARY);

	if (fd < 0)
	{
		if (errno == EACCES)
			return;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return;
	}

	/*
	 * pg_flush_data() 忽略错误，这没问题，因为这只是
	 * 一个提示。
	 */
	pg_flush_data(fd, 0, 0);

	if (CloseTransientFile(fd) != 0)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
}

#endif							/* 具备 PG_FLUSH_DATA_WORKS */

static void
datadir_fsync_fname(const char *fname, bool isdir, int elevel)
{
	ereport_startup_progress("syncing data directory (fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	/*
	 * 我们想静默地忽略关于不可读文件的错误。把这个
	 * 意愿传给 fsync_fname_ext()。
	 */
	fsync_fname_ext(fname, isdir, true, elevel);
}

static void
unlink_if_exists_fname(const char *fname, bool isdir, int elevel)
{
	if (isdir)
	{
		if (rmdir(fname) != 0 && errno != ENOENT)
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m", fname)));
	}
	else
	{
		/* 用 PathNameDeleteTemporaryFile 来报告文件大小 */
		PathNameDeleteTemporaryFile(fname, false);
	}
}

/*
 * fsync_fname_ext -- 尝试对文件或目录做 fsync。
 *
 * 若 ignore_perm 为真，则忽略试图打开不可读
 * 文件时的错误。其他错误按调用者指定的级别记录。
 *
 * 成功返回 0，否则返回 -1。
 */
int
fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel)
{
	int			fd;
	int			flags;
	int			returncode;

	/*
	 * 有些操作系统需要以只读方式打开目录，而另一些
	 * 系统不允许我们对以只读方式打开的文件做 fsync；因此这里两种
	 * 情形都需要。使用 O_RDWR 会使我们无法 fsync 那些
	 * 我们的用户标识不能写的文件，但我们假定那是可以的。
	 */
	flags = PG_BINARY;
	if (!isdir)
		flags |= O_RDWR;
	else
		flags |= O_RDONLY;

	fd = OpenTransientFile(fname, flags);

	/*
	 * 有些操作系统根本不允许我们打开目录（Windows 返回
	 * EACCES），那种情况下就忽略错误。若需要，也静默地
	 * 忽略关于不可读文件的错误。其他的则记录日志。
	 */
	if (fd < 0 && isdir && (errno == EISDIR || errno == EACCES))
		return 0;
	else if (fd < 0 && ignore_perm && errno == EACCES)
		return 0;
	else if (fd < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return -1;
	}

	returncode = pg_fsync(fd);

	/*
	 * 有些 OSes 根本不允许我们对目录做 fsync，因此可以忽略
	 * 那些错误。其他任何情况都需要记日志。
	 */
	if (returncode != 0 && !(isdir && (errno == EBADF || errno == EINVAL)))
	{
		int			save_errno;

		/* 出错时关闭文件，此时可能不在事务上下文里 */
		save_errno = errno;
		(void) CloseTransientFile(fd);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", fname)));
		return -1;
	}

	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
		return -1;
	}

	return 0;
}

/*
 * fsync_parent_path -- 对文件或目录的父路径做 fsync。
 *
 * 这是为了在操作系统崩溃或断电时，使文件操作
 * 在磁盘上持久。
 */
static int
fsync_parent_path(const char *fname, int elevel)
{
	char		parentpath[MAXPGPATH];

	strlcpy(parentpath, fname, MAXPGPATH);
	get_parent_directory(parentpath);

	/*
	 * 若输入参数只是一个文件名，get_parent_directory() 返回空字符串
	 * （见 path.c 里的注释），因此把这种情况当作
	 * 当前目录来处理。
	 */
	if (strlen(parentpath) == 0)
		strlcpy(parentpath, ".", MAXPGPATH);

	if (fsync_fname_ext(parentpath, true, false, elevel) != 0)
		return -1;

	return 0;
}

/*
 * 创建一个 PostgreSQL 数据子目录。
 *
 * 数据目录本身及其多数子目录是在
 * initdb 时创建的，但我们确实有些场合要在
 * 后端里创建目录（例如 CREATE TABLESPACE）。在那些情况下，我们希望
 * 确保这些目录的创建方式一致。今天，这意味着
 * 确保创建出的目录有正确的权限，而这正是
 * pg_dir_create_mode 为我们跟踪的。
 *
 * 注意，我们还会根据自己所理解的正确
 * 权限来设置 umask()（见 file_perm.c）。
 *
 * 对于默认以外的权限，可以直接使用 mkdir()，但
 * 请务必仔细考虑这类情况：PostgreSQL 数据目录里权限不正确的
 * 子目录可能导致备份和其他
 * 进程失败。
 */
int
MakePGDirectory(const char *directoryName)
{
	return mkdir(directoryName, pg_dir_create_mode);
}

/*
 * 返回传入的错误级别；若 data_sync_retry 关闭，则返回 PANIC。
 *
 * 任何数据文件 fsync 失败都是立即恐慌的理由，除非
 * 启用了 data_sync_retry。数据可能已经写给操作系统，
 * 并且已经从我们的缓冲池里去掉，若我们运行在
 * 回写失败时会忘掉脏数据的操作系统上，
 * 数据可能只剩下一份副本：在 WAL 里。稍后再次
 * 尝试 fsync 可能会虚假地报告成功。因此我们不得允许
 * 再尝试任何检查点。理论上可以在已知不会在回写
 * 失败时丢掉脏的缓冲区数据的系统上启用 data_sync_retry
 * （很可能的结果是，在底层问题修好之前，检查点会继续
 * 失败）。
 *
 * 任何报告 fsync() 或相关函数失败的代码都应当
 * 用本函数过滤错误级别。
 */
int
data_sync_elevel(int elevel)
{
	return data_sync_retry ? elevel : PANIC;
}

bool
check_debug_io_direct(char **newval, void **extra, GucSource source)
{
	bool		result = true;
	int			flags;

#if PG_O_DIRECT == 0
	if (strcmp(*newval, "") != 0)
	{
		GUC_check_errdetail("debug_io_direct is not supported on this platform.");
		result = false;
	}
	flags = 0;
#else
	List	   *elemlist;
	ListCell   *l;
	char	   *rawstring;

	/* 需要一份可修改的字符串副本 */
	rawstring = pstrdup(*newval);

	if (!SplitGUCList(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("invalid list syntax in parameter \"%s\"",
							"debug_io_direct");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	flags = 0;
	foreach(l, elemlist)
	{
		char	   *item = (char *) lfirst(l);

		if (pg_strcasecmp(item, "data") == 0)
			flags |= IO_DIRECT_DATA;
		else if (pg_strcasecmp(item, "wal") == 0)
			flags |= IO_DIRECT_WAL;
		else if (pg_strcasecmp(item, "wal_init") == 0)
			flags |= IO_DIRECT_WAL_INIT;
		else
		{
			GUC_check_errdetail("invalid option \"%s\"", item);
			result = false;
			break;
		}
	}

	/*
	 * 可以把块大小配置得小于我们所假定的 I/O
	 * 对齐大小，这可能导致无效的 I/O 请求。
	 */
#if XLOG_BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & (IO_DIRECT_WAL | IO_DIRECT_WAL_INIT)))
	{
		GUC_check_errdetail("debug_io_direct is not supported for WAL because XLOG_BLCKSZ is too small");
		result = false;
	}
#endif
#if BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & IO_DIRECT_DATA))
	{
		GUC_check_errdetail("debug_io_direct is not supported for data because BLCKSZ is too small");
		result = false;
	}
#endif

	pfree(rawstring);
	list_free(elemlist);
#endif

	if (!result)
		return result;

	/* 把标志保存在 *extra 里，供 assign_debug_io_direct 使用 */
	*extra = guc_malloc(ERROR, sizeof(int));
	*((int *) *extra) = flags;

	return result;
}

extern void
assign_debug_io_direct(const char *newval, void *extra)
{
	int		   *flags = (int *) extra;

	io_direct_flags = *flags;
}
