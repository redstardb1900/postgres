/*-------------------------------------------------------------------------
 *
 * dsm.c
 *	  管理动态共享内存段
 *
 * 本文件提供一组服务，使使用动态
 * 共享内存段编程更加方便。与
 * dsm_impl.h 与 dsm_impl.c 提供的低级
 * 设施不同，用本模块创建的映射与段
 * 会自动清理。映射会在创建它们时所处的
 * 资源所有者清理时移除；若使用了 dsm_pin_mapping()，
 * 则映射具有会话寿命。当不再有
 * 任何映射时段会被移除，无论如何也会在 postmaster
 * 关闭时移除。在硬性的 postmaster
 * 崩溃之后，若段仍存在，将在下一次 postmaster 启动时
 * 移除。
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>

#include "common/pg_prng.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pg_shmem.h"
#include "utils/freepage.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"

#define PG_DYNSHMEM_CONTROL_MAGIC		0x9a503d32

#define PG_DYNSHMEM_FIXED_SLOTS			64
#define PG_DYNSHMEM_SLOTS_PER_BACKEND	5

#define INVALID_CONTROL_SLOT		((uint32) -1)

/* 脱离回调的后端本地跟踪。 */
typedef struct dsm_segment_detach_callback
{
	on_dsm_detach_callback function;
	Datum		arg;
	slist_node	node;
} dsm_segment_detach_callback;

/* 一个动态共享内存段的后端本地状态。 */
struct dsm_segment
{
	dlist_node	node;			/* dsm_segment_list 中的链表结点。 */
	ResourceOwner resowner;		/* 资源所有者。 */
	dsm_handle	handle;			/* 段名。 */
	uint32		control_slot;	/* 控制段中的槽。 */
	void	   *impl_private;	/* 特定实现的私有数据。 */
	void	   *mapped_address; /* 映射地址；若未映射则为 NULL。 */
	Size		mapped_size;	/* 我们这块映射的大小。 */
	slist_head	on_detach;		/* 脱离时的回调。 */
};

/* 一个动态共享内存段的共享内存状态。 */
typedef struct dsm_control_item
{
	dsm_handle	handle;
	uint32		refcnt;			/* 2 及以上表示活动，1 表示垂死，0 表示已消失 */
	size_t		first_page;
	size_t		npages;
	void	   *impl_private_pm_handle; /* 仅 Windows 需要 */
	bool		pinned;
} dsm_control_item;

/* 动态共享内存控制段的布局。 */
typedef struct dsm_control_header
{
	uint32		magic;
	uint32		nitems;
	uint32		maxitems;
	dsm_control_item item[FLEXIBLE_ARRAY_MEMBER];
} dsm_control_header;

static void dsm_cleanup_for_mmap(void);
static void dsm_postmaster_shutdown(int code, Datum arg);
static dsm_segment *dsm_create_descriptor(void);
static bool dsm_control_segment_sane(dsm_control_header *control,
									 Size mapped_size);
static uint64 dsm_control_bytes_needed(uint32 nitems);
static inline dsm_handle make_main_region_dsm_handle(int slot);
static inline bool is_main_region_dsm_handle(dsm_handle handle);

/* 此后端是否已经初始化动态共享内存系统？ */
static bool dsm_init_done = false;

/* 主共享内存区中预分配的动态共享内存空间。 */
static void *dsm_main_space_begin = NULL;

/*
 * 本后端使用的动态共享内存段列表。
 *
 * 进程退出时，我们必须把每个已挂接段的
 * 引用计数减一；这份列表使找出所有这样的
 * 段成为可能。
 *
 * 这份列表在 postmaster 里应当始终为空。我们也许可以
 * 允许 postmaster 在开始启动子进程之前映射动态共享内存段，
 * 只要每个进程在启动时调整控制段里那些段的
 * 引用计数，但看不出对这种机制有明显的需要，
 * 而且在 EXEC_BACKEND 情形下处理起来也会很复杂。一旦
 * postmaster 开始派生子进程，还有一个额外问题：
 * 每次新映射都需要更新控制段，
 * 而更新需要上锁，
 * postmaster 不得卷入其中。
 */
static dlist_head dsm_segment_list = DLIST_STATIC_INIT(dsm_segment_list);

/*
 * 控制段信息。
 *
 * 与普通共享内存段不同，控制段不按引用计数；
 * 它的寿命与 postmaster 的整个生命周期相同。为简单起见，
 * 它也没有 dsm_segment 对象。
 */
static dsm_handle dsm_control_handle;
static dsm_control_header *dsm_control;
static Size dsm_control_mapped_size = 0;
static void *dsm_control_impl_private = NULL;

/*
 * 启动动态共享内存系统。
 *
 * 每个数据目录的生命周期里只在 postmaster
 * 启动时调用一次。
 */
void
dsm_postmaster_startup(PGShmemHeader *shim)
{
	void	   *dsm_control_address = NULL;
	uint32		maxitems;
	Size		segsize;

	Assert(!IsUnderPostmaster);

	/*
	 * 若使用 mmap 实现，清掉任何残留。
	 * Windows 上不需要清理，POSIX 与 System V 共享内存
	 * 则在启动更早的时候，通过直接调用
	 * dsm_cleanup_using_control_segment 完成清理。
	 */
	if (dynamic_shared_memory_type == DSM_IMPL_MMAP)
		dsm_cleanup_for_mmap();

	/* 确定新控制段的大小。 */
	maxitems = PG_DYNSHMEM_FIXED_SLOTS
		+ PG_DYNSHMEM_SLOTS_PER_BACKEND * MaxBackends;
	elog(DEBUG2, "dynamic shared memory system will support %u segments",
		 maxitems);
	segsize = dsm_control_bytes_needed(maxitems);

	/*
	 * 循环直到为新控制段找到一个未使用的标识。我们
	 * 有时用 DSM_HANDLE_INVALID 作为哨兵，表示没有
	 * 控制段，因此要避免为真实句柄生成该值。
	 */
	for (;;)
	{
		Assert(dsm_control_address == NULL);
		Assert(dsm_control_mapped_size == 0);
		/* 只使用偶数 */
		dsm_control_handle = pg_prng_uint32(&pg_global_prng_state) << 1;
		if (dsm_control_handle == DSM_HANDLE_INVALID)
			continue;
		if (dsm_impl_op(DSM_OP_CREATE, dsm_control_handle, segsize,
						&dsm_control_impl_private, &dsm_control_address,
						&dsm_control_mapped_size, ERROR))
			break;
	}
	dsm_control = dsm_control_address;
	on_shmem_exit(dsm_postmaster_shutdown, PointerGetDatum(shim));
	elog(DEBUG2,
		 "created dynamic shared memory control segment %u (%zu bytes)",
		 dsm_control_handle, segsize);
	shim->dsm_control = dsm_control_handle;

	/* 初始化控制段。 */
	dsm_control->magic = PG_DYNSHMEM_CONTROL_MAGIC;
	dsm_control->nitems = 0;
	dsm_control->maxitems = maxitems;
}

/*
 * 判断上一次 postmaster 调用留下的控制段是否仍在。
 * 若仍在，去掉它所引用的动态共享内存段，
 * 然后再去掉控制段本身。
 */
void
dsm_cleanup_using_control_segment(dsm_handle old_control_handle)
{
	void	   *mapped_address = NULL;
	void	   *junk_mapped_address = NULL;
	void	   *impl_private = NULL;
	void	   *junk_impl_private = NULL;
	Size		mapped_size = 0;
	Size		junk_mapped_size = 0;
	uint32		nitems;
	uint32		i;
	dsm_control_header *old_control;

	/*
	 * 尝试挂接该段。若失败，多半只是操作系统已重启，
	 * 该段不再存在，或是无关进程用过同一个 shm 标识。
	 * 因此不必声张，
	 * 悄悄返回即可。
	 */
	if (!dsm_impl_op(DSM_OP_ATTACH, old_control_handle, 0, &impl_private,
					 &mapped_address, &mapped_size, DEBUG1))
		return;

	/*
	 * 我们设法重新挂上了它，但内容未必健全。若不健全，
	 * 最终还是忽略该段。
	 */
	old_control = (dsm_control_header *) mapped_address;
	if (!dsm_control_segment_sane(old_control, mapped_size))
	{
		dsm_impl_op(DSM_OP_DETACH, old_control_handle, 0, &impl_private,
					&mapped_address, &mapped_size, LOG);
		return;
	}

	/*
	 * 好，控制段看起来基本有效，因此可以用它取得
	 * 需要去掉的段的列表。
	 */
	nitems = old_control->nitems;
	for (i = 0; i < nitems; ++i)
	{
		dsm_handle	handle;
		uint32		refcnt;

		/* 若引用计数为 0，该槽实际上未使用。 */
		refcnt = old_control->item[i].refcnt;
		if (refcnt == 0)
			continue;

		/* 若它使用的是主共享内存区，就无事可做。 */
		handle = old_control->item[i].handle;
		if (is_main_region_dsm_handle(handle))
			continue;

		/* 记录调试信息。 */
		elog(DEBUG2, "cleaning up orphaned dynamic shared memory with ID %u (reference count %u)",
			 handle, refcnt);

		/* 销毁被引用的段。 */
		dsm_impl_op(DSM_OP_DESTROY, handle, 0, &junk_impl_private,
					&junk_mapped_address, &junk_mapped_size, LOG);
	}

	/* 把旧的控制段也销毁。 */
	elog(DEBUG2,
		 "cleaning up dynamic shared memory control segment with ID %u",
		 old_control_handle);
	dsm_impl_op(DSM_OP_DESTROY, old_control_handle, 0, &impl_private,
				&mapped_address, &mapped_size, LOG);
}

/*
 * 使用 mmap 共享内存实现时，所谓共享内存段甚至可能
 * 挺过操作系统重启。但究竟哪些能留下来并没有保证：
 * 有的段可能还在，有的可能没有，有的内容可能已经
 * 过时。尤其是控制段可能过时，因此不能靠它来判断
 * 该去掉什么。不过，既然我们知道用作共享内存的文件
 * 放在哪个目录里，只要扫描该目录，
 * 把不该留在那里的
 * 东西全部清掉即可。
 */
static void
dsm_cleanup_for_mmap(void)
{
	DIR		   *dir;
	struct dirent *dent;

	/* 扫描该目录，寻找名字格式正确的项。 */
	dir = AllocateDir(PG_DYNSHMEM_DIR);

	while ((dent = ReadDir(dir, PG_DYNSHMEM_DIR)) != NULL)
	{
		if (strncmp(dent->d_name, PG_DYNSHMEM_MMAP_FILE_PREFIX,
					strlen(PG_DYNSHMEM_MMAP_FILE_PREFIX)) == 0)
		{
			char		buf[MAXPGPATH + sizeof(PG_DYNSHMEM_DIR)];

			snprintf(buf, sizeof(buf), PG_DYNSHMEM_DIR "/%s", dent->d_name);

			elog(DEBUG2, "removing file \"%s\"", buf);

			/* 找到了匹配的文件，于是把它去掉。 */
			if (unlink(buf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", buf)));
		}
	}

	/* 清理完成。 */
	FreeDir(dir);
}

/*
 * 关闭时，我们遍历控制段，去掉所有剩余的
 * 动态共享内存段。这里避免抛出错误；
 * postmaster 反正正在关闭，而这只是非关键的
 * 资源清理。
 */
static void
dsm_postmaster_shutdown(int code, Datum arg)
{
	uint32		nitems;
	uint32		i;
	void	   *dsm_control_address;
	void	   *junk_mapped_address = NULL;
	void	   *junk_impl_private = NULL;
	Size		junk_mapped_size = 0;
	PGShmemHeader *shim = (PGShmemHeader *) DatumGetPointer(arg);

	/*
	 * 若某个其他后端不干净地退出，它可能在临终时
	 * 弄坏了控制段。这种情况下我们发出警告并忽略
	 * 控制段的内容。这可能留下散落的共享内存段，
	 * 但元数据已经没了，
	 * 我们也没有多少办法。
	 */
	nitems = dsm_control->nitems;
	if (!dsm_control_segment_sane(dsm_control, dsm_control_mapped_size))
	{
		ereport(LOG,
				(errmsg("dynamic shared memory control segment is corrupt")));
		return;
	}

	/* 去掉任何剩余的段。 */
	for (i = 0; i < nitems; ++i)
	{
		dsm_handle	handle;

		/* 若引用计数为 0，该槽实际上未使用。 */
		if (dsm_control->item[i].refcnt == 0)
			continue;

		handle = dsm_control->item[i].handle;
		if (is_main_region_dsm_handle(handle))
			continue;

		/* 记录调试信息。 */
		elog(DEBUG2, "cleaning up orphaned dynamic shared memory with ID %u",
			 handle);

		/* 销毁该段。 */
		dsm_impl_op(DSM_OP_DESTROY, handle, 0, &junk_impl_private,
					&junk_mapped_address, &junk_mapped_size, LOG);
	}

	/* 去掉控制段本身。 */
	elog(DEBUG2,
		 "cleaning up dynamic shared memory control segment with ID %u",
		 dsm_control_handle);
	dsm_control_address = dsm_control;
	dsm_impl_op(DSM_OP_DESTROY, dsm_control_handle, 0,
				&dsm_control_impl_private, &dsm_control_address,
				&dsm_control_mapped_size, LOG);
	dsm_control = dsm_control_address;
	shim->dsm_control = 0;
}

/*
 * 让此后端准备好使用动态共享内存。在 EXEC_BACKEND 下，
 * 我们必须重读状态文件并映射控制段；其他情况下，
 * 会继承 postmaster 的映射和全局变量。
 */
static void
dsm_backend_startup(void)
{
#ifdef EXEC_BACKEND
	if (IsUnderPostmaster)
	{
		void	   *control_address = NULL;

		/* 挂接控制段。 */
		Assert(dsm_control_handle != 0);
		dsm_impl_op(DSM_OP_ATTACH, dsm_control_handle, 0,
					&dsm_control_impl_private, &control_address,
					&dsm_control_mapped_size, ERROR);
		dsm_control = control_address;
		/* 若控制段看起来不健全，就有严重问题。 */
		if (!dsm_control_segment_sane(dsm_control, dsm_control_mapped_size))
		{
			dsm_impl_op(DSM_OP_DETACH, dsm_control_handle, 0,
						&dsm_control_impl_private, &control_address,
						&dsm_control_mapped_size, WARNING);
			ereport(FATAL,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("dynamic shared memory control segment is not valid")));
		}
	}
#endif

	dsm_init_done = true;
}

#ifdef EXEC_BACKEND
/*
 * 在 EXEC_BACKEND 下运行时，主共享内存段重新挂接时
 * 会在这里得到回调，以便记下从中取出的控制
 * 句柄。
 */
void
dsm_set_control_handle(dsm_handle h)
{
	Assert(dsm_control_handle == 0 && h != 0);
	dsm_control_handle = h;
}
#endif

/*
 * 在主共享内存段里为动态共享内存段预留一些空间。
 */
size_t
dsm_estimate_size(void)
{
	return 1024 * 1024 * (size_t) min_dynamic_shared_memory;
}

/*
 * 初始化主共享内存段里给动态共享内存段用的空间。
 */
void
dsm_shmem_init(void)
{
	size_t		size = dsm_estimate_size();
	bool		found;

	if (size == 0)
		return;

	dsm_main_space_begin = ShmemInitStruct("Preallocated DSM", size, &found);
	if (!found)
	{
		FreePageManager *fpm = (FreePageManager *) dsm_main_space_begin;
		size_t		first_page = 0;
		size_t		pages;

		/* 为 FreePageManager 预留空间。 */
		while (first_page * FPM_PAGE_SIZE < sizeof(FreePageManager))
			++first_page;

		/* 初始化它，并把其余空间都交给它。 */
		FreePageManagerInitialize(fpm, dsm_main_space_begin);
		pages = (size / FPM_PAGE_SIZE) - first_page;
		FreePageManagerPut(fpm, first_page, pages);
	}
}

/*
 * 创建一段新的动态共享内存。
 *
 * 若 CurrentResourceOwner 非 NULL，新段与它关联，
 * 并且必须在资源所有者释放之前脱离，否则会记一条
 * 警告。若 CurrentResourceOwner 为 NULL，该段一直保持
 * 挂接，直到显式脱离或会话结束。以 NULL 的
 * CurrentResourceOwner 创建，等价于以非 NULL 的
 * CurrentResourceOwner 创建后再调用 dsm_pin_mapping。
 */
dsm_segment *
dsm_create(Size size, int flags)
{
	dsm_segment *seg;
	uint32		i;
	uint32		nitems;
	size_t		npages = 0;
	size_t		first_page = 0;
	FreePageManager *dsm_main_space_fpm = dsm_main_space_begin;
	bool		using_main_dsm_region = false;

	/*
	 * 在 postmaster 中不安全。允许单用户模式使用 dsm 看似
	 * 没有意义，但否则有些子系统就得为单用户模式准备
	 * 专门的代码路径。
	 */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	if (!dsm_init_done)
		dsm_backend_startup();

	/* 创建一个新的段描述符。 */
	seg = dsm_create_descriptor();

	/*
	 * 若已配置，在尝试从主共享内存区分配时锁住
	 * 控制段。
	 */
	if (dsm_main_space_fpm)
	{
		npages = size / FPM_PAGE_SIZE;
		if (size % FPM_PAGE_SIZE > 0)
			++npages;

		LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
		if (FreePageManagerGet(dsm_main_space_fpm, npages, &first_page))
		{
			/* 我们可以从主共享内存段里切出一块。 */
			seg->mapped_address = (char *) dsm_main_space_begin +
				first_page * FPM_PAGE_SIZE;
			seg->mapped_size = npages * FPM_PAGE_SIZE;
			using_main_dsm_region = true;
			/* 句柄留到下面再选。 */
		}
	}

	if (!using_main_dsm_region)
	{
		/*
		 * 我们得创建一段新的内存。循环直到找到一个
		 * 未使用的段标识。
		 */
		if (dsm_main_space_fpm)
			LWLockRelease(DynamicSharedMemoryControlLock);
		for (;;)
		{
			Assert(seg->mapped_address == NULL && seg->mapped_size == 0);
			/* 只使用偶数 */
			seg->handle = pg_prng_uint32(&pg_global_prng_state) << 1;
			if (seg->handle == DSM_HANDLE_INVALID)	/* 保留哨兵值 */
				continue;
			if (dsm_impl_op(DSM_OP_CREATE, seg->handle, size, &seg->impl_private,
							&seg->mapped_address, &seg->mapped_size, ERROR))
				break;
		}
		LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	}

	/* 在控制段里搜索一个未使用的槽。 */
	nitems = dsm_control->nitems;
	for (i = 0; i < nitems; ++i)
	{
		if (dsm_control->item[i].refcnt == 0)
		{
			if (using_main_dsm_region)
			{
				seg->handle = make_main_region_dsm_handle(i);
				dsm_control->item[i].first_page = first_page;
				dsm_control->item[i].npages = npages;
			}
			else
				Assert(!is_main_region_dsm_handle(seg->handle));
			dsm_control->item[i].handle = seg->handle;
			/* refcnt 为 1 会触发销毁，所以从 2 开始 */
			dsm_control->item[i].refcnt = 2;
			dsm_control->item[i].impl_private_pm_handle = NULL;
			dsm_control->item[i].pinned = false;
			seg->control_slot = i;
			LWLockRelease(DynamicSharedMemoryControlLock);
			return seg;
		}
	}

	/* 确认我们还能再支持一份映射。 */
	if (nitems >= dsm_control->maxitems)
	{
		if (using_main_dsm_region)
			FreePageManagerPut(dsm_main_space_fpm, first_page, npages);
		LWLockRelease(DynamicSharedMemoryControlLock);
		if (!using_main_dsm_region)
			dsm_impl_op(DSM_OP_DESTROY, seg->handle, 0, &seg->impl_private,
						&seg->mapped_address, &seg->mapped_size, WARNING);
		if (seg->resowner != NULL)
			ResourceOwnerForgetDSM(seg->resowner, seg);
		dlist_delete(&seg->node);
		pfree(seg);

		if ((flags & DSM_CREATE_NULL_IF_MAXSEGMENTS) != 0)
			return NULL;
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("too many dynamic shared memory segments")));
	}

	/* 把句柄放进一个新的数组槽。 */
	if (using_main_dsm_region)
	{
		seg->handle = make_main_region_dsm_handle(nitems);
		dsm_control->item[i].first_page = first_page;
		dsm_control->item[i].npages = npages;
	}
	dsm_control->item[nitems].handle = seg->handle;
	/* refcnt 为 1 会触发销毁，所以从 2 开始 */
	dsm_control->item[nitems].refcnt = 2;
	dsm_control->item[nitems].impl_private_pm_handle = NULL;
	dsm_control->item[nitems].pinned = false;
	seg->control_slot = nitems;
	dsm_control->nitems++;
	LWLockRelease(DynamicSharedMemoryControlLock);

	return seg;
}

/*
 * 挂接一段动态共享内存。
 *
 * 关于预期用法的说明，见 dsm_segment_handle()
 * 的注释。
 *
 * 若系统不知道该段，本函数返回 NULL。
 * 若我们被请求挂接该段，但在真正挂上之前
 * 其他人都已脱离（导致该段被销毁），
 * 就会发生这种情况。
 *
 * 若 CurrentResourceOwner 非 NULL，挂接的段就与它关联，
 * 并且必须在资源所有者释放之前脱离，否则会记一条警告。
 * 否则该段一直保持挂接，直到显式脱离或会话结束。
 * 见 dsm_create() 顶部的说明。
 */
dsm_segment *
dsm_attach(dsm_handle h)
{
	dsm_segment *seg;
	dlist_iter	iter;
	uint32		i;
	uint32		nitems;

	/* 在 postmaster 中不安全（在独立后端里也没有意义）。 */
	Assert(IsUnderPostmaster);

	if (!dsm_init_done)
		dsm_backend_startup();

	/*
	 * 这只是调试用的交叉检查，本来可以整个省掉，
	 * 或只放进启用断言的构建里。但已挂接段的列表
	 * 通常应当很短，所以目前
	 * 总是把它包括进来。
	 *
	 * 若你撞上这个错误，多半想在调用 dsm_attach()
	 * 创建新映射之前，先用 dsm_find_mapping()
	 * 找一份已有的映射。
	 */
	dlist_foreach(iter, &dsm_segment_list)
	{
		seg = dlist_container(dsm_segment, node, iter.cur);
		if (seg->handle == h)
			elog(ERROR, "can't attach the same segment more than once");
	}

	/* 创建一个新的段描述符。 */
	seg = dsm_create_descriptor();
	seg->handle = h;

	/* 增加该段在共享内存中的引用计数。 */
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	nitems = dsm_control->nitems;
	for (i = 0; i < nitems; ++i)
	{
		/*
		 * 若引用计数为 0，该槽实际上未使用。若引用计数
		 * 为 1，槽仍在使用，但该段正在消失；即便句柄匹配，
		 * 另一个槽也可能碰巧已经开始使用
		 * 同一个句柄值，所以我们必须
		 * 继续搜索。
		 */
		if (dsm_control->item[i].refcnt <= 1)
			continue;

		/* 若句柄不匹配，就不是我们要的槽。 */
		if (dsm_control->item[i].handle != seg->handle)
			continue;

		/* 否则我们就找到了匹配项。 */
		dsm_control->item[i].refcnt++;
		seg->control_slot = i;
		if (is_main_region_dsm_handle(seg->handle))
		{
			seg->mapped_address = (char *) dsm_main_space_begin +
				dsm_control->item[i].first_page * FPM_PAGE_SIZE;
			seg->mapped_size = dsm_control->item[i].npages * FPM_PAGE_SIZE;
		}
		break;
	}
	LWLockRelease(DynamicSharedMemoryControlLock);

	/*
	 * 若在控制段里找不到要找的句柄，多半意味着包括
	 * 最初创建者在内、所有映射过它的人都在我们到达
	 * 这里之前死掉了。
	 * 如何处理由调用方决定。
	 */
	if (seg->control_slot == INVALID_CONTROL_SLOT)
	{
		dsm_detach(seg);
		return NULL;
	}

	/* 真正尝试映射该段的地方在这里。 */
	if (!is_main_region_dsm_handle(seg->handle))
		dsm_impl_op(DSM_OP_ATTACH, seg->handle, 0, &seg->impl_private,
					&seg->mapped_address, &seg->mapped_size, ERROR);

	return seg;
}

/*
 * 后端关闭时，脱离仍然挂接的段。
 * （这与 dsm_detach_all 类似，只是没有理由在退出前
 * 解除控制段的映射，所以我们不这么做。）
 */
void
dsm_backend_shutdown(void)
{
	while (!dlist_is_empty(&dsm_segment_list))
	{
		dsm_segment *seg;

		seg = dlist_head_element(dsm_segment, node, &dsm_segment_list);
		dsm_detach(seg);
	}
}

/*
 * 脱离所有共享内存段，包括控制段。那些可能继承了
 * 映射、但并不打算连接到动态共享内存的进程，
 * 应当连同 PGSharedMemoryDetach
 * 一起调用本函数。
 */
void
dsm_detach_all(void)
{
	void	   *control_address = dsm_control;

	while (!dlist_is_empty(&dsm_segment_list))
	{
		dsm_segment *seg;

		seg = dlist_head_element(dsm_segment, node, &dsm_segment_list);
		dsm_detach(seg);
	}

	if (control_address != NULL)
		dsm_impl_op(DSM_OP_DETACH, dsm_control_handle, 0,
					&dsm_control_impl_private, &control_address,
					&dsm_control_mapped_size, ERROR);
}

/*
 * 脱离共享内存段；若去掉的是最后一次引用，就销毁该段。
 * 本函数绝不应当失败。
 *
 * 它常常在中止事务时被调用，再产生错误也没有用处。
 * 未能解除映射或销毁该段并不是彻底的灾难：
 * 那意味着资源泄漏，但不一定
 * 妨碍后续操作。
 */
void
dsm_detach(dsm_segment *seg)
{
	/*
	 * 调用已登记的回调。万一其中某个回调再抛出错误
	 * 把我们带回这里，就在调用之前先弹出该回调，
	 * 以免错误无限递归。在非错误代码路径上运行各个
	 * 回调时不允许中断，以免被语句超时或类似情况打断，
	 * 从而留下未完成的
	 * 清理工作。
	 */
	HOLD_INTERRUPTS();
	while (!slist_is_empty(&seg->on_detach))
	{
		slist_node *node;
		dsm_segment_detach_callback *cb;
		on_dsm_detach_callback function;
		Datum		arg;

		node = slist_pop_head_node(&seg->on_detach);
		cb = slist_container(dsm_segment_detach_callback, node, node);
		function = cb->function;
		arg = cb->arg;
		pfree(cb);

		function(seg, arg);
	}
	RESUME_INTERRUPTS();

	/*
	 * 若存在映射，就尝试去掉它。通常会有，但若创建或
	 * 挂接做到一半失败了，也许没有。我们在减少引用计数
	 * 之前去掉映射，这样看到引用计数为零的进程可以确信
	 * 已经没有残留映射。即便这一步失败，我们也假装
	 * 它成功了，因为重试很可能
	 * 以同样的方式失败。
	 */
	if (seg->mapped_address != NULL)
	{
		if (!is_main_region_dsm_handle(seg->handle))
			dsm_impl_op(DSM_OP_DETACH, seg->handle, 0, &seg->impl_private,
						&seg->mapped_address, &seg->mapped_size, WARNING);
		seg->impl_private = NULL;
		seg->mapped_address = NULL;
		seg->mapped_size = 0;
	}

	/* 若先前增加过引用计数，就把它减下来。 */
	if (seg->control_slot != INVALID_CONTROL_SLOT)
	{
		uint32		refcnt;
		uint32		control_slot = seg->control_slot;

		LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
		Assert(dsm_control->item[control_slot].handle == seg->handle);
		Assert(dsm_control->item[control_slot].refcnt > 1);
		refcnt = --dsm_control->item[control_slot].refcnt;
		seg->control_slot = INVALID_CONTROL_SLOT;
		LWLockRelease(DynamicSharedMemoryControlLock);

		/* 若新的引用计数为 1，尝试销毁该段。 */
		if (refcnt == 1)
		{
			/* 被钉住的段绝不应当降到 1。 */
			Assert(!dsm_control->item[control_slot].pinned);

			/*
			 * 若我们在这里没能销毁该段，或在做完之前被杀掉，
			 * 引用计数会停在 1。这意味着别人都无法再挂接该段。
			 * postmaster 关闭时，或在硬杀掉之后启动新的
			 * postmaster 时，会再做一次尝试，把该段
			 * 去掉。
			 * 这里主要担心在完成去掉之前被信号杀掉。
			 *
			 * 那种情况下，必须确保该段仍然会被去掉。
			 * 若我们因为别的原因真的没能去掉该段，
			 * postmaster 未必比我们更幸运。
			 * 不过对此我们也没有多少办法，
			 * 只能接受
			 * 这个结果。
			 */
			if (is_main_region_dsm_handle(seg->handle) ||
				dsm_impl_op(DSM_OP_DESTROY, seg->handle, 0, &seg->impl_private,
							&seg->mapped_address, &seg->mapped_size, WARNING))
			{
				LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
				if (is_main_region_dsm_handle(seg->handle))
					FreePageManagerPut((FreePageManager *) dsm_main_space_begin,
									   dsm_control->item[control_slot].first_page,
									   dsm_control->item[control_slot].npages);
				Assert(dsm_control->item[control_slot].handle == seg->handle);
				Assert(dsm_control->item[control_slot].refcnt == 1);
				dsm_control->item[control_slot].refcnt = 0;
				LWLockRelease(DynamicSharedMemoryControlLock);
			}
		}
	}

	/* 清理我们剩余的后端私有数据结构。 */
	if (seg->resowner != NULL)
		ResourceOwnerForgetDSM(seg->resowner, seg);
	dlist_delete(&seg->node);
	pfree(seg);
}

/*
 * 把动态共享内存映射保持到会话结束。
 *
 * 默认情况下，映射归当前资源所有者拥有。
 * 这通常意味着它们只存在于
 * 当前查询期间。
 */
void
dsm_pin_mapping(dsm_segment *seg)
{
	if (seg->resowner != NULL)
	{
		ResourceOwnerForgetDSM(seg->resowner, seg);
		seg->resowner = NULL;
	}
}

/*
 * 安排在清理时去掉一段动态共享内存映射。
 *
 * dsm_pin_mapping() 可以把映射保留到进程的整个
 * 生命期；本函数撤销那个决定，使该段归当前资源
 * 所有者拥有。若即将做的操作会使该段对此后端以后的
 * 使用失效，
 * 这可能有用。
 */
void
dsm_unpin_mapping(dsm_segment *seg)
{
	Assert(seg->resowner == NULL);
	ResourceOwnerEnlargeDSMs(CurrentResourceOwner);
	seg->resowner = CurrentResourceOwner;
	ResourceOwnerRememberDSM(seg->resowner, seg);
}

/*
 * 把一段动态共享内存一直保留到 postmaster 关闭，或直到
 * 调用 dsm_unpin_segment。
 *
 * 每个段调用本函数不应超过一次，除非在两次调用之间
 * 用 dsm_unpin_segment 显式解除钉住。
 *
 * 注意：本函数并不让当前进程无限期保持该段已映射。
 * 若想要那种行为，每个需要保留该映射的进程
 * 都应当自行调用
 * dsm_pin_mapping()。
 */
void
dsm_pin_segment(dsm_segment *seg)
{
	void	   *handle = NULL;

	/*
	 * 增加该段在共享内存中的引用计数。这样，即便没有任何
	 * 会话挂接在这段上，它也会保留到 postmaster 关闭，
	 * 或直到有人显式调用
	 * 解除钉住。
	 */
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	if (dsm_control->item[seg->control_slot].pinned)
		elog(ERROR, "cannot pin a segment that is already pinned");
	if (!is_main_region_dsm_handle(seg->handle))
		dsm_impl_pin_segment(seg->handle, seg->impl_private, &handle);
	dsm_control->item[seg->control_slot].pinned = true;
	dsm_control->item[seg->control_slot].refcnt++;
	dsm_control->item[seg->control_slot].impl_private_pm_handle = handle;
	LWLockRelease(DynamicSharedMemoryControlLock);
}

/*
 * 解除先前用 dsm_pin_segment 钉住的一段动态共享内存。
 * 除非先前曾对该段调用过 dsm_pin_segment，
 * 否则不应调用本函数。
 *
 * 参数是 dsm_handle 而不是 dsm_segment，以便解除钉住
 * 一段尚未挂接的段。这很有用，例如当对一个
 * 共享内存段的引用存放在另一个共享内存段里时。
 * 你可能想在销毁实施引用的那段之前，
 * 先解除被引用段的钉住。
 */
void
dsm_unpin_segment(dsm_handle handle)
{
	uint32		control_slot = INVALID_CONTROL_SLOT;
	bool		destroy = false;
	uint32		i;

	/* 按给定句柄找到控制槽。 */
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	for (i = 0; i < dsm_control->nitems; ++i)
	{
		/* 跳过未使用的槽，以及正在并发消失的段。 */
		if (dsm_control->item[i].refcnt <= 1)
			continue;

		/* 若已找到我们的句柄，就可以停止搜索。 */
		if (dsm_control->item[i].handle == handle)
		{
			control_slot = i;
			break;
		}
	}

	/*
	 * 我们肯定应当已经找到该槽，而且它不应当已经
	 * 处于正在消失的过程中，因为本函数只应当对
	 * 已钉住的段调用。
	 */
	if (control_slot == INVALID_CONTROL_SLOT)
		elog(ERROR, "cannot unpin unknown segment handle");
	if (!dsm_control->item[control_slot].pinned)
		elog(ERROR, "cannot unpin a segment that is not pinned");
	Assert(dsm_control->item[control_slot].refcnt > 1);

	/*
	 * 允许特定实现的代码运行。必须在放开锁之前做，
	 * 因为 impl_private_pm_handle 可能被
	 * dsm_impl_unpin_segment 修改。
	 */
	if (!is_main_region_dsm_handle(handle))
		dsm_impl_unpin_segment(handle,
							   &dsm_control->item[control_slot].impl_private_pm_handle);

	/* 注意：1 表示没有引用（0 表示未使用的槽）。 */
	if (--dsm_control->item[control_slot].refcnt == 1)
		destroy = true;
	dsm_control->item[control_slot].pinned = false;

	/* 现在可以放开锁。 */
	LWLockRelease(DynamicSharedMemoryControlLock);

	/* 若那是最后一次引用，就清理资源。 */
	if (destroy)
	{
		void	   *junk_impl_private = NULL;
		void	   *junk_mapped_address = NULL;
		Size		junk_mapped_size = 0;

		/*
		 * 关于这种情况下错误处理如何工作，见
		 * dsm_detach 里的注释。注意，若走到这里，当前进程
		 * 肯定没有映射该段，因为若映射了，即便放开钉住
		 * 所持的引用计数，计数也仍然会大于 1。既然这个
		 * 句柄不可能有 dsm_segment，这里把映射大小、映射
		 * 地址和私有数据都作为 NULL
		 * 传入
		 * 就是可以的。
		 */
		if (is_main_region_dsm_handle(handle) ||
			dsm_impl_op(DSM_OP_DESTROY, handle, 0, &junk_impl_private,
						&junk_mapped_address, &junk_mapped_size, WARNING))
		{
			LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
			if (is_main_region_dsm_handle(handle))
				FreePageManagerPut((FreePageManager *) dsm_main_space_begin,
								   dsm_control->item[control_slot].first_page,
								   dsm_control->item[control_slot].npages);
			Assert(dsm_control->item[control_slot].handle == handle);
			Assert(dsm_control->item[control_slot].refcnt == 1);
			dsm_control->item[control_slot].refcnt = 0;
			LWLockRelease(DynamicSharedMemoryControlLock);
		}
	}
}

/*
 * 若已有共享内存段的映射，就找到它。
 */
dsm_segment *
dsm_find_mapping(dsm_handle handle)
{
	dlist_iter	iter;
	dsm_segment *seg;

	dlist_foreach(iter, &dsm_segment_list)
	{
		seg = dlist_container(dsm_segment, node, iter.cur);
		if (seg->handle == handle)
			return seg;
	}

	return NULL;
}

/*
 * 取得动态共享内存段被映射到的地址。
 */
void *
dsm_segment_address(dsm_segment *seg)
{
	Assert(seg->mapped_address != NULL);
	return seg->mapped_address;
}

/*
 * 取得映射的大小。
 */
Size
dsm_segment_map_length(dsm_segment *seg)
{
	Assert(seg->mapped_address != NULL);
	return seg->mapped_size;
}

/*
 * 取得一份映射的句柄。
 *
 * 要让两个后端经动态共享内存通信，其中一个应先
 * 调用 dsm_create() 建立新的共享内存映射。该进程
 * 接着应调用 dsm_segment_handle() 取得该映射的
 * 句柄，并用某种办法把句柄传给协调后端
 * （例如 bgw_main_arg，或经主共享内存段）。
 * 收到句柄的一方一旦持有它，就应调用
 * dsm_attach()。
 */
dsm_handle
dsm_segment_handle(dsm_segment *seg)
{
	return seg->handle;
}

/*
 * 为一段动态共享内存登记脱离回调。
 */
void
on_dsm_detach(dsm_segment *seg, on_dsm_detach_callback function, Datum arg)
{
	dsm_segment_detach_callback *cb;

	cb = MemoryContextAlloc(TopMemoryContext,
							sizeof(dsm_segment_detach_callback));
	cb->function = function;
	cb->arg = arg;
	slist_push_head(&seg->on_detach, &cb->node);
}

/*
 * 为一段动态共享内存注销脱离回调。
 */
void
cancel_on_dsm_detach(dsm_segment *seg, on_dsm_detach_callback function,
					 Datum arg)
{
	slist_mutable_iter iter;

	slist_foreach_modify(iter, &seg->on_detach)
	{
		dsm_segment_detach_callback *cb;

		cb = slist_container(dsm_segment_detach_callback, node, iter.cur);
		if (cb->function == function && cb->arg == arg)
		{
			slist_delete_current(&iter);
			pfree(cb);
			break;
		}
	}
}

/*
 * 丢掉所有已登记的脱离回调，而不执行它们。
 */
void
reset_on_dsm_detach(void)
{
	dlist_iter	iter;

	dlist_foreach(iter, &dsm_segment_list)
	{
		dsm_segment *seg = dlist_container(dsm_segment, node, iter.cur);

		/* 逐个丢掉显式的脱离动作。 */
		while (!slist_is_empty(&seg->on_detach))
		{
			slist_node *node;
			dsm_segment_detach_callback *cb;

			node = slist_pop_head_node(&seg->on_detach);
			cb = slist_container(dsm_segment_detach_callback, node, node);
			pfree(cb);
		}

		/*
		 * 减少引用计数是一种隐式的脱离动作；
		 * 也要确保我们不那么做。
		 */
		seg->control_slot = INVALID_CONTROL_SLOT;
	}
}

/*
 * 创建一个段描述符。
 */
static dsm_segment *
dsm_create_descriptor(void)
{
	dsm_segment *seg;

	if (CurrentResourceOwner)
		ResourceOwnerEnlargeDSMs(CurrentResourceOwner);

	seg = MemoryContextAlloc(TopMemoryContext, sizeof(dsm_segment));
	dlist_push_head(&dsm_segment_list, &seg->node);

	/* seg->handle 必须由调用方初始化 */
	seg->control_slot = INVALID_CONTROL_SLOT;
	seg->impl_private = NULL;
	seg->mapped_address = NULL;
	seg->mapped_size = 0;

	seg->resowner = CurrentResourceOwner;
	if (CurrentResourceOwner)
		ResourceOwnerRememberDSM(CurrentResourceOwner, seg);

	slist_init(&seg->on_detach);

	return seg;
}

/*
 * 对控制段做健全性检查。
 *
 * 这里的目标不是查出控制段上一切可能的问题；
 * 信息不够。目标是确保有人可以遍历段中的项，
 * 而不会越过映射末尾导致崩溃。我们也检查魔数，
 * 因为若魔数乱了，这甚至可能
 * 根本不是
 * 我们的段。
 */
static bool
dsm_control_segment_sane(dsm_control_header *control, Size mapped_size)
{
	if (mapped_size < offsetof(dsm_control_header, item))
		return false;			/* 映射长度太短，读不了头部。 */
	if (control->magic != PG_DYNSHMEM_CONTROL_MAGIC)
		return false;			/* 魔数不匹配。 */
	if (dsm_control_bytes_needed(control->maxitems) > mapped_size)
		return false;			/* 最大项数放不进映射。 */
	if (control->nitems > control->maxitems)
		return false;			/* 过满。 */
	return true;
}

/*
 * 计算存放给定数量的项
 * 所需的控制段字节数。
 */
static uint64
dsm_control_bytes_needed(uint32 nitems)
{
	return offsetof(dsm_control_header, item)
		+ sizeof(dsm_control_item) * (uint64) nitems;
}

static inline dsm_handle
make_main_region_dsm_handle(int slot)
{
	dsm_handle	handle;

	/*
	 * 我们得造一个句柄，不与 dsm_impl_op() 创建的任何
	 * 已有额外段相撞，因此把它做成奇数。它也不得与
	 * 主区域中的其他伪段相撞，所以若干位里会纳入槽号。
	 * 我们还希望尽量避免新创建的句柄和最近销毁的句柄
	 * 被混淆，因此其余的位
	 * 做成随机数。
	 */
	handle = 1;
	handle |= slot << 1;
	handle |= pg_prng_uint32(&pg_global_prng_state) << (pg_leftmost_one_pos32(dsm_control->maxitems) + 1);
	return handle;
}

static inline bool
is_main_region_dsm_handle(dsm_handle handle)
{
	return handle & 1;
}
