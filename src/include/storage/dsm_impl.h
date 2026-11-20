/*-------------------------------------------------------------------------
 *
 * dsm_impl.h
 *	  动态共享内存底层原语。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm_impl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSM_IMPL_H
#define DSM_IMPL_H

/* 动态共享内存实现。 */
#define DSM_IMPL_POSIX			1
#define DSM_IMPL_SYSV			2
#define DSM_IMPL_WINDOWS		3
#define DSM_IMPL_MMAP			4

/*
 * 确定本平台将支持哪些动态共享内存实现，
 * 以及以哪一个为默认。
 */
#ifdef WIN32
#define USE_DSM_WINDOWS
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_WINDOWS
#else
#ifdef HAVE_SHM_OPEN
#define USE_DSM_POSIX
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_POSIX
#endif
#define USE_DSM_SYSV
#ifndef DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE
#define DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE		DSM_IMPL_SYSV
#endif
#define USE_DSM_MMAP
#endif

/* GUC. */
extern PGDLLIMPORT int dynamic_shared_memory_type;
extern PGDLLIMPORT int min_dynamic_shared_memory;

/*
 * 磁盘状态所用目录。
 *
 * 所有实现用于崩溃恢复，mmap 实现还
 * 用于存储。
 */
#define PG_DYNSHMEM_DIR					"pg_dynshmem"
#define PG_DYNSHMEM_MMAP_FILE_PREFIX	"mmap."

/* 动态共享内存段的"名称"。 */
typedef uint32 dsm_handle;

/* 无效 DSM 句柄所用哨兵值。 */
#define DSM_HANDLE_INVALID ((dsm_handle) 0)

/* 我们所知的全部共享内存操作。 */
typedef enum
{
	DSM_OP_CREATE,
	DSM_OP_ATTACH,
	DSM_OP_DETACH,
	DSM_OP_DESTROY
} dsm_op;

/* 创建、附着、脱离、调整大小或销毁一段。 */
extern bool dsm_impl_op(dsm_op op, dsm_handle handle, Size request_size,
						void **impl_private, void **mapped_address, Size *mapped_size,
						int elevel);

/* 关停前保持该段所需的实现相关动作。 */
extern void dsm_impl_pin_segment(dsm_handle handle, void *impl_private,
								 void **impl_private_pm_handle);
extern void dsm_impl_unpin_segment(dsm_handle handle, void **impl_private);

#endif							/* DSM_IMPL_H */
