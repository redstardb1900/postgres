/*-------------------------------------------------------------------------
 *
 * pg_shmem.h
 *	  共享内存支持的平台无关 API。
 *
 * 每个移植应支持语义大致类似 SysV 的共享内存；
 * 尤其是内存块并非匿名而有 ID，且我们必须能判断
 * 指定 ID 的块上是否还有
 * 进程附着。
 *
 * 为简化 SysV 实现，假定 ID 由两个 unsigned long
 * 组成（SysV 术语中的 key 与 ID）。其他平台若只需
 * 一个 ID 号，可忽略
 * 第二个值。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pg_shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SHMEM_H
#define PG_SHMEM_H

#include "storage/dsm_impl.h"

typedef struct PGShmemHeader	/* 所有 Postgres 共享内存标准头 */
{
	int32		magic;			/* 标识 Postgres 段的幻数 */
#define PGShmemMagic  679834894
	pid_t		creatorPID;		/* 创建进程的 PID（设置但未读） */
	Size		totalsize;		/* 段的总大小 */
	Size		freeoffset;		/* 空闲空间起始偏移 */
	dsm_handle	dsm_control;	/* 动态共享内存控制段的 ID */
	void	   *index;			/* ShmemIndex 表指针 */
#ifndef WIN32					/* Windows 无有用的 inode#s */
	dev_t		device;			/* 数据目录所在设备 */
	ino_t		inode;			/* 数据目录的 inode 号 */
#endif
} PGShmemHeader;

/* GUC 变量 */
extern PGDLLIMPORT int shared_memory_type;
extern PGDLLIMPORT int huge_pages;
extern PGDLLIMPORT int huge_page_size;

/* huge_pages 的可能值 */
typedef enum
{
	HUGE_PAGES_OFF,
	HUGE_PAGES_ON,
	HUGE_PAGES_TRY
}			HugePagesType;

/* shared_memory_type 的可能值 */
typedef enum
{
	SHMEM_TYPE_WINDOWS,
	SHMEM_TYPE_SYSV,
	SHMEM_TYPE_MMAP
}			PGShmemType;

#ifndef WIN32
extern PGDLLIMPORT unsigned long UsedShmemSegID;
#else
extern PGDLLIMPORT HANDLE UsedShmemSegID;
extern PGDLLIMPORT void *ShmemProtectiveRegion;
#endif
extern PGDLLIMPORT void *UsedShmemSegAddr;

#if !defined(WIN32) && !defined(EXEC_BACKEND)
#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_MMAP
#elif !defined(WIN32)
#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_SYSV
#else
#define DEFAULT_SHARED_MEMORY_TYPE SHMEM_TYPE_WINDOWS
#endif

#ifdef EXEC_BACKEND
extern void PGSharedMemoryReAttach(void);
extern void PGSharedMemoryNoReAttach(void);
#endif

extern PGShmemHeader *PGSharedMemoryCreate(Size size,
										   PGShmemHeader **shim);
extern bool PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2);
extern void PGSharedMemoryDetach(void);
extern void GetHugePageSize(Size *hugepagesize, int *mmap_flags);

#endif							/* PG_SHMEM_H */
