/*-------------------------------------------------------------------------
 *
 * dshash.h
 *	  Concurrent hash tables backed by dynamic shared memory areas.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/lib/dshash.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DSHASH_H
#define DSHASH_H

#include "utils/dsa.h"

/* 表示哈希表的不透明类型。 */
struct dshash_table;
typedef struct dshash_table dshash_table;

/* 可与其他进程共享的 dshash_table 句柄。 */
typedef dsa_pointer dshash_table_handle;

/* 用于无效 dshash_table 句柄的哨兵值。 */
#define DSHASH_HANDLE_INVALID ((dshash_table_handle) InvalidDsaPointer)

/* 哈希值的类型。 */
typedef uint32 dshash_hash;

/* 比较键的函数类型。 */
typedef int (*dshash_compare_function) (const void *a, const void *b,
										size_t size, void *arg);

/* 为键计算哈希值的函数类型。 */
typedef dshash_hash (*dshash_hash_function) (const void *v, size_t size,
											 void *arg);

/*
 * 创建哈希表或附着到已有哈希表所需的参数集合。附着到
 * 已有哈希表时，成员 tranche_id 与 tranche_name 不必初始化。
 *
 * 即使在附着时也必须提供比较函数和哈希函数，因为一般
 * 无法在各后端之间安全共享函数指针。应提供带 arg 的
 * 变体或不带 arg 的变体之一；另一组函数指针应为 NULL。
 * 若提供带 arg 的变体，则传给 create 与 attach 的用户
 * 数据指针会被传给哈希函数和比较函数。
 *
 *
 */
typedef struct dshash_parameters
{
	size_t		key_size;		/* 键的大小（条目起始若干字节） */
	size_t		entry_size;		/* 条目的总大小 */
	dshash_compare_function compare_function;	/* 比较函数 */
	dshash_hash_function hash_function; /* 哈希函数 */
	int			tranche_id;		/* 这些锁使用的 tranche_id */
} dshash_parameters;

/* 仅供 dshash.c 使用的私有类型的前向声明。 */
struct dshash_table_item;
typedef struct dshash_table_item dshash_table_item;

/*
 * 顺序扫描状态。暴露细节是为了让使用者知道存储
 * 大小，但调用者应把它视为不透明类型。
 */
typedef struct dshash_seq_status
{
	dshash_table *hash_table;	/* 正在操作的 dshash 表 */
	int			curbucket;		/* 当前所在的桶号 */
	int			nbuckets;		/* dshash 中的桶总数 */
	dshash_table_item *curitem; /* 当前所在的项 */
	dsa_pointer pnextitem;		/* 指向下一项的 dsa 指针 */
	int			curpartition;	/* 当前所在的分区号 */
	bool		exclusive;		/* 加锁模式 */
} dshash_seq_status;

/* 创建、共享和销毁哈希表。 */
extern dshash_table *dshash_create(dsa_area *area,
								   const dshash_parameters *params,
								   void *arg);
extern dshash_table *dshash_attach(dsa_area *area,
								   const dshash_parameters *params,
								   dshash_table_handle handle,
								   void *arg);
extern void dshash_detach(dshash_table *hash_table);
extern dshash_table_handle dshash_get_hash_table_handle(dshash_table *hash_table);
extern void dshash_destroy(dshash_table *hash_table);

/* 查找、创建和删除条目。 */
extern void *dshash_find(dshash_table *hash_table,
						 const void *key, bool exclusive);
extern void *dshash_find_or_insert(dshash_table *hash_table,
								   const void *key, bool *found);
extern bool dshash_delete_key(dshash_table *hash_table, const void *key);
extern void dshash_delete_entry(dshash_table *hash_table, void *entry);
extern void dshash_release_lock(dshash_table *hash_table, void *entry);

/* 顺序扫描支持 */
extern void dshash_seq_init(dshash_seq_status *status, dshash_table *hash_table,
							bool exclusive);
extern void *dshash_seq_next(dshash_seq_status *status);
extern void dshash_seq_term(dshash_seq_status *status);
extern void dshash_delete_current(dshash_seq_status *status);

/* 包装 memcmp 与 tag_hash 的便利哈希和比较函数。 */
extern int	dshash_memcmp(const void *a, const void *b, size_t size, void *arg);
extern dshash_hash dshash_memhash(const void *v, size_t size, void *arg);

/* 调试支持。 */
extern void dshash_dump(dshash_table *hash_table);

#endif							/* DSHASH_H */
