/*-------------------------------------------------------------------------
 *
 * shm_toc.h
 *	  共享内存段内容表。
 *
 * 旨在提供一种简单办法，把一块共享内存（多半是
 * 经 dsm_create 分配的动态共享内存）分成若干区域，
 * 并跟踪这些区域的地址或其中关键数据结构。
 * 无意扩展到大量键，那样用会很差；若需要大量
 * 指针，把它们存在段内其他数据结构中，内容表里
 * 只放指向该
 * 数据结构本身的
 * 指针。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shm_toc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHM_TOC_H
#define SHM_TOC_H

#include "storage/shmem.h"		/* for add_size() */

/* shm_toc 是仅 shm_toc.c 内可知的不透明类型 */
typedef struct shm_toc shm_toc;

extern shm_toc *shm_toc_create(uint64 magic, void *address, Size nbytes);
extern shm_toc *shm_toc_attach(uint64 magic, void *address);
extern void *shm_toc_allocate(shm_toc *toc, Size nbytes);
extern Size shm_toc_freespace(shm_toc *toc);
extern void shm_toc_insert(shm_toc *toc, uint64 key, void *address);
extern void *shm_toc_lookup(shm_toc *toc, uint64 key, bool noError);

/*
 * 用于估算存放 TOC 及其从属对象需要多大一块共享
 * 内存。注意：我们并不真正支持大量键，但把
 * number_of_keys 声明为 Size
 * 仍方便。
 */
typedef struct
{
	Size		space_for_chunks;
	Size		number_of_keys;
} shm_toc_estimator;

#define shm_toc_initialize_estimator(e) \
	((e)->space_for_chunks = 0, (e)->number_of_keys = 0)
#define shm_toc_estimate_chunk(e, sz) \
	((e)->space_for_chunks = add_size((e)->space_for_chunks, BUFFERALIGN(sz)))
#define shm_toc_estimate_keys(e, cnt) \
	((e)->number_of_keys = add_size((e)->number_of_keys, cnt))

extern Size shm_toc_estimate(shm_toc_estimator *e);

#endif							/* SHM_TOC_H */
