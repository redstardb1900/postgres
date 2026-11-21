/*
 *	fe_memutils.h
 *		前端代码的内存管理支持
 *
 *	Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 *	src/include/common/fe_memutils.h
 */
#ifndef FE_MEMUTILS_H
#define FE_MEMUTILS_H

/*
 * pg_malloc_extended 与 palloc_extended 的标志，故意与
 * 后端标志同名。
 */
#define MCXT_ALLOC_HUGE			0x01	/* 略
										 * 实际未用 */
#define MCXT_ALLOC_NO_OOM		0x02	/*  内存不足时不失败  */
#define MCXT_ALLOC_ZERO			0x04	/* 略 */

/*
 * "安全"内存分配函数——失败时 exit(1)
 * （除 pg_malloc_extended+MCXT_ALLOC_NO_OOM）
 */
extern char *pg_strdup(const char *in);
extern void *pg_malloc(size_t size);
extern void *pg_malloc0(size_t size);
extern void *pg_malloc_extended(size_t size, int flags);
extern void *pg_realloc(void *ptr, size_t size);
extern void pg_free(void *ptr);

/*
 * 更易记、类型更安全的变体
 */

/*
 * 为类型 "type" 的一个对象分配空间
 */
#define pg_malloc_object(type) ((type *) pg_malloc(sizeof(type)))
#define pg_malloc0_object(type) ((type *) pg_malloc0(sizeof(type)))

/*
 * 为 "count" 个 "type" 对象分配空间
 */
#define pg_malloc_array(type, count) ((type *) pg_malloc(sizeof(type) * (count)))
#define pg_malloc0_array(type, count) ((type *) pg_malloc0(sizeof(type) * (count)))

/*
 * 把 "pointer" 所指分配改为可容纳 "count" 个
 * "type" 对象
 */
#define pg_realloc_array(pointer, type, count) ((type *) pg_realloc(pointer, sizeof(type) * (count)))

/*  等价函数，故意与后端函数同名  */
extern char *pstrdup(const char *in);
extern char *pnstrdup(const char *in, Size size);
extern void *palloc(Size size);
extern void *palloc0(Size size);
extern void *palloc_extended(Size size, int flags);
extern void *repalloc(void *pointer, Size size);
extern void pfree(void *pointer);

#define palloc_object(type) ((type *) palloc(sizeof(type)))
#define palloc0_object(type) ((type *) palloc0(sizeof(type)))
#define palloc_array(type, count) ((type *) palloc(sizeof(type) * (count)))
#define palloc0_array(type, count) ((type *) palloc0(sizeof(type) * (count)))
#define repalloc_array(pointer, type, count) ((type *) repalloc(pointer, sizeof(type) * (count)))

/*  格式化写入 palloc 缓冲——在 psprintf.c  */
extern char *psprintf(const char *fmt,...) pg_attribute_printf(1, 2);
extern size_t pvsnprintf(char *buf, size_t len, const char *fmt, va_list args) pg_attribute_printf(3, 0);

#endif							/* FE_MEMUTILS_H */
