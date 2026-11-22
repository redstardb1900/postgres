/*-------------------------------------------------------------------------
 *
 * dsa.h
 *	  动态共享内存区。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/utils/dsa.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSA_H
#define DSA_H

#include "port/atomics.h"
#include "storage/dsm.h"

/* 表示区域的不透明类型。 */
struct dsa_area;
typedef struct dsa_area dsa_area;

/*
 * 若本系统 size_t 仅用 32 位值，则用 32 位 DSA
 * 实现。这会把可创建的 DSA 量限制为远小于整个
 * 4GB 地址空间，因为 DSA 指针须编码段标识与
 * 偏移，但实践中这不应是显著限制。
 * 见上。
 *
 * 若本系统不支持 64 位原子操作，则因别无他法
 * 退回 32 位 dsa_pointer。
 *
 * 为测试可定义 USE_SMALL_DSA_POINTER，强制在本
 * 可支持 64 位 dsa_pointer 的系统上也用 32 位
 * 指针。
 */
#if SIZEOF_SIZE_T == 4 || !defined(PG_HAVE_ATOMIC_U64_SUPPORT) || \
	defined(USE_SMALL_DSA_POINTER)
#define SIZEOF_DSA_POINTER 4
#else
#define SIZEOF_DSA_POINTER 8
#endif

/*
 * 动态共享区所分配内存的"相对指针"类型。
 * dsa_pointer 值可与其他进程共享，但解引用前须
 * 转为后端本地指针。见 dsa_get_address。另有原子
 * 版本与相应宽度的原子操作。
 * 见上。
 */
#if SIZEOF_DSA_POINTER == 4
typedef uint32 dsa_pointer;
typedef pg_atomic_uint32 dsa_pointer_atomic;
#define dsa_pointer_atomic_init pg_atomic_init_u32
#define dsa_pointer_atomic_read pg_atomic_read_u32
#define dsa_pointer_atomic_write pg_atomic_write_u32
#define dsa_pointer_atomic_fetch_add pg_atomic_fetch_add_u32
#define dsa_pointer_atomic_compare_exchange pg_atomic_compare_exchange_u32
#define DSA_POINTER_FORMAT "%08x"
#else
typedef uint64 dsa_pointer;
typedef pg_atomic_uint64 dsa_pointer_atomic;
#define dsa_pointer_atomic_init pg_atomic_init_u64
#define dsa_pointer_atomic_read pg_atomic_read_u64
#define dsa_pointer_atomic_write pg_atomic_write_u64
#define dsa_pointer_atomic_fetch_add pg_atomic_fetch_add_u64
#define dsa_pointer_atomic_compare_exchange pg_atomic_compare_exchange_u64
#define DSA_POINTER_FORMAT "%016" INT64_MODIFIER "x"
#endif

/* dsa_allocate_extended 标志。 */
#define DSA_ALLOC_HUGE		0x01	/* 允许巨大分配（> 1 GB） */
#define DSA_ALLOC_NO_OOM	0x02	/* 内存不足时不失败 */
#define DSA_ALLOC_ZERO		0x04	/* 清零已分配内存 */

/* 表示分配失败的 dsa_pointer 哨兵值。 */
#define InvalidDsaPointer ((dsa_pointer) 0)

/* 检查 dsa_pointer 值是否有效。 */
#define DsaPointerIsValid(x) ((x) != InvalidDsaPointer)

/* 分配未初始化内存，内存不足时报错。 */
#define dsa_allocate(area, size) \
	dsa_allocate_extended(area, size, 0)

/* 分配零初始化内存，内存不足时报错。 */
#define dsa_allocate0(area, size) \
	dsa_allocate_extended(area, size, DSA_ALLOC_ZERO)

/*
 * dsa_area 句柄类型。dsa_handle 值可与其他进程
 * 共享以便它们附着。这提供了与其他进程共享已
 * 分配存储的途径。
 *
 * dsa_area 的句柄目前实现为支撑该动态存储区的
 * 首个 DSM 段的 dsm_handle，但客户代码不应假定
 * 如此。
 */
typedef dsm_handle dsa_handle;

/* 无效 dsa_handles 所用的哨兵值。 */
#define DSA_HANDLE_INVALID ((dsa_handle) DSM_HANDLE_INVALID)


extern dsa_area *dsa_create(int tranche_id);
extern dsa_area *dsa_create_in_place(void *place, size_t size,
									 int tranche_id, dsm_segment *segment);
extern dsa_area *dsa_attach(dsa_handle handle);
extern dsa_area *dsa_attach_in_place(void *place, dsm_segment *segment);
extern void dsa_release_in_place(void *place);
extern void dsa_on_dsm_detach_release_in_place(dsm_segment *, Datum);
extern void dsa_on_shmem_exit_release_in_place(int, Datum);
extern void dsa_pin_mapping(dsa_area *area);
extern void dsa_detach(dsa_area *area);
extern void dsa_pin(dsa_area *area);
extern void dsa_unpin(dsa_area *area);
extern void dsa_set_size_limit(dsa_area *area, size_t limit);
extern size_t dsa_minimum_size(void);
extern dsa_handle dsa_get_handle(dsa_area *area);
extern dsa_pointer dsa_allocate_extended(dsa_area *area, size_t size, int flags);
extern void dsa_free(dsa_area *area, dsa_pointer dp);
extern void *dsa_get_address(dsa_area *area, dsa_pointer dp);
extern void dsa_trim(dsa_area *area);
extern void dsa_dump(dsa_area *area);

#endif							/* DSA_H */
