/*-------------------------------------------------------------------------
 *
 * dsm.h
 *	  管理动态共享内存段。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/dsm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSM_H
#define DSM_H

#include "storage/dsm_impl.h"

typedef struct dsm_segment dsm_segment;

#define DSM_CREATE_NULL_IF_MAXSEGMENTS			0x0001

/* 启动与关闭函数。 */
struct PGShmemHeader;			/* 避免包含 pg_shmem.h */
extern void dsm_cleanup_using_control_segment(dsm_handle old_control_handle);
extern void dsm_postmaster_startup(struct PGShmemHeader *);
extern void dsm_backend_shutdown(void);
extern void dsm_detach_all(void);

extern size_t dsm_estimate_size(void);
extern void dsm_shmem_init(void);

#ifdef EXEC_BACKEND
extern void dsm_set_control_handle(dsm_handle h);
#endif

/* 创建或移除映射的函数。 */
extern dsm_segment *dsm_create(Size size, int flags);
extern dsm_segment *dsm_attach(dsm_handle h);
extern void dsm_detach(dsm_segment *seg);

/* 资源管理函数。 */
extern void dsm_pin_mapping(dsm_segment *seg);
extern void dsm_unpin_mapping(dsm_segment *seg);
extern void dsm_pin_segment(dsm_segment *seg);
extern void dsm_unpin_segment(dsm_handle handle);
extern dsm_segment *dsm_find_mapping(dsm_handle handle);

/* 信息函数。 */
extern void *dsm_segment_address(dsm_segment *seg);
extern Size dsm_segment_map_length(dsm_segment *seg);
extern dsm_handle dsm_segment_handle(dsm_segment *seg);

/* 清理钩子. */
typedef void (*on_dsm_detach_callback) (dsm_segment *, Datum arg);
extern void on_dsm_detach(dsm_segment *seg,
						  on_dsm_detach_callback function, Datum arg);
extern void cancel_on_dsm_detach(dsm_segment *seg,
								 on_dsm_detach_callback function, Datum arg);
extern void reset_on_dsm_detach(void);

#endif							/* DSM_H */
