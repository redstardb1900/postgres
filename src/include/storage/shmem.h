/*-------------------------------------------------------------------------
 *
 * shmem.h
 *	  共享内存管理结构。
 *
 * 历史说明：
 * 很久以前，Postgres 共享内存区允许在各进程映射到
 * 不同地址，共享内存"指针"以相对共享内存区起始的
 * 偏移传递。如今已非如此：每个进程必须把共享内存
 * 区映射到同一地址。这意味着共享内存指针可在
 * 不同进程之间
 * 直接传递。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHMEM_H
#define SHMEM_H

#include "utils/hsearch.h"


/* shmem.c */
extern void InitShmemAccess(void *seghdr);
extern void InitShmemAllocation(void);
extern void *ShmemAlloc(Size size);
extern void *ShmemAllocNoError(Size size);
extern void *ShmemAllocUnlocked(Size size);
extern bool ShmemAddrIsValid(const void *addr);
extern void InitShmemIndex(void);
extern HTAB *ShmemInitHash(const char *name, long init_size, long max_size,
						   HASHCTL *infoP, int hash_flags);
extern void *ShmemInitStruct(const char *name, Size size, bool *foundPtr);
extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);

/* ipci.c */
extern void RequestAddinShmemSpace(Size size);

/* shmem 索引表的大小常量 */
 /* 数据结构字符串名最大长度 */
#define SHMEM_INDEX_KEYSIZE		 (48)
 /* shmem 索引表的估计大小（非硬限制） */
#define SHMEM_INDEX_SIZE		 (64)

/* 这是 shmem 索引表中的哈希桶 */
typedef struct
{
	char		key[SHMEM_INDEX_KEYSIZE];	/* 名称 */
	void	   *location;		/* 共享内存位置 */
	Size		size;			/* 为该结构请求的字节数 */
	Size		allocated_size; /* 实际分配的字节数 */
} ShmemIndexEnt;

#endif							/* SHMEM_H */
