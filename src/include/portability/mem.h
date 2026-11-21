/*-------------------------------------------------------------------------
 *
 * mem.h
 *	  各类内存操作的可移植定义
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * src/include/portability/mem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEM_H
#define MEM_H

#define IPCProtection	(0600)	/*  仅用户访问/修改  */

#ifdef SHM_SHARE_MMU			/*  Solaris 亲密共享内存  */
#define PG_SHMAT_FLAGS			SHM_SHARE_MMU
#else
#define PG_SHMAT_FLAGS			0
#endif

/*  Linux 偏好 MAP_ANONYMOUS，其他系统该标志叫 MAP_ANON。  */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS			MAP_ANON
#endif

/*  BSD 派生系统有 MAP_HASSEMAPHORE，Linux 上不存在（也不需要）。  */
#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE		0
#endif

/*
 * BSD 派生系统用 MAP_NOSYNC 防止脏 mmap(2)
 * 页被无谓刷到磁盘。
 */
#ifndef MAP_NOSYNC
#define MAP_NOSYNC			0
#endif

#define PG_MMAP_FLAGS			(MAP_SHARED|MAP_ANONYMOUS|MAP_HASSEMAPHORE)

/*  某些很老的系统不定义 MAP_FAILED。  */
#ifndef MAP_FAILED
#define MAP_FAILED ((void *) -1)
#endif

#endif							/* MEM_H */
