/*-------------------------------------------------------------------------
 *
 * hsearch.h
 *	  utils/hash/dynahash.c 的导出定义；详见其中说明
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/hsearch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HSEARCH_H
#define HSEARCH_H


/*
 * 哈希函数必须具有此签名。
 */
typedef uint32 (*HashValueFunc) (const void *key, Size keysize);

/*
 * 键比较函数必须具有此签名。比较函数匹配时返回
 * 零，不匹配返回非零。（该定义旨在允许直接把
 * memcmp() 与 strncmp() 用作键比较函数。）
 * 见上。
 */
typedef int (*HashCompareFunc) (const void *key1, const void *key2,
								Size keysize);

/*
 * 键拷贝函数必须具有此签名。返回值未使用。
 * 该定义旨在允许直接使用 memcpy() 与
 * strlcpy()。
 */
typedef void *(*HashCopyFunc) (void *dest, const void *src, Size keysize);

/*
 * 哈希表空间分配函数——设计为匹配 malloc()。
 * 注意：无释放函数 API；除非用默认分配器，
 * 无法销毁哈希表。
 */
typedef void *(*HashAllocFunc) (Size request);

/*
 * HASHELEMENT 是哈希表条目的私有部分。调用方
 * 数据跟在 HASHELEMENT 之后（MAXALIGN 对齐）。
 * 哈希键应位于调用方哈希条目数据结构的起始处。
 */
typedef struct HASHELEMENT
{
	struct HASHELEMENT *link;	/* 同桶下一条目的链接 */
	uint32		hashvalue;		/* 本条目的哈希函数结果 */
} HASHELEMENT;

/* 哈希表头结构是仅在 dynahash.c 内可知的不透明类型 */
typedef struct HASHHDR HASHHDR;

/* 哈希表控制结构是仅在 dynahash.c 内可知的不透明类型 */
typedef struct HTAB HTAB;

/* hash_create 的参数数据结构 */
/* 仅需设置 hash_flags 所指示的那些字段 */
typedef struct HASHCTL
{
	/* HASH_PARTITION 置位时用： */
	long		num_partitions; /* 分区数（须为 2 的幂） */
	/* HASH_SEGMENT 置位时用： */
	long		ssize;			/* 段大小 */
	/* HASH_DIRSIZE 置位时用： */
	long		dsize;			/* 初始目录大小 */
	long		max_dsize;		/* 目录大小受限时限制 dsize */
	/* HASH_ELEM 置位时用（现已必需）： */
	Size		keysize;		/* 哈希键长（字节） */
	Size		entrysize;		/* 用户元素总大小(字节) */
	/* HASH_FUNCTION 置位时用： */
	HashValueFunc hash;			/* 哈希函数 */
	/* HASH_COMPARE 置位时用： */
	HashCompareFunc match;		/* 键比较函数 */
	/* HASH_KEYCOPY 置位时用： */
	HashCopyFunc keycopy;		/* 键拷贝函数 */
	/* HASH_ALLOC 置位时用： */
	HashAllocFunc alloc;		/* 内存分配器 */
	/* HASH_CONTEXT 置位时用： */
	MemoryContext hcxt;			/* 用于分配的内存上下文 */
	/* HASH_SHARED_MEM 置位时用： */
	HASHHDR    *hctl;			/* 共享内存中头的位置 */
} HASHCTL;

/* hash_create 标志位；多数表示提供了哪些参数 */
#define HASH_PARTITION	0x0001	/* 哈希表配合分区锁使用 */
#define HASH_SEGMENT	0x0002	/* 设置段大小 */
#define HASH_DIRSIZE	0x0004	/* 设目录大小（初值与上限） */
#define HASH_ELEM		0x0008	/* 设 keysize/entrysize（现必需！） */
#define HASH_STRINGS	0x0010	/* 选用字符串键支持函数 */
#define HASH_BLOBS		0x0020	/* 选用二进制键支持函数 */
#define HASH_FUNCTION	0x0040	/* 设置用户定义哈希函数 */
#define HASH_COMPARE	0x0080	/* 设置用户定义比较函数 */
#define HASH_KEYCOPY	0x0100	/* 设置用户定义键拷贝函数 */
#define HASH_ALLOC		0x0200	/* 设内存分配器 */
#define HASH_CONTEXT	0x0400	/* 设内存分配上下文 */
#define HASH_SHARED_MEM 0x0800	/* 哈希表位于共享内存 */
#define HASH_ATTACH		0x1000	/* 不初始化 hctl */
#define HASH_FIXED_SIZE 0x2000	/* 初始大小为硬限制 */

/* 表示可扩展目录的 max_dsize 值 */
#define NO_MAX_DSIZE			(-1)

/* hash_search 操作 */
typedef enum
{
	HASH_FIND,
	HASH_ENTER,
	HASH_REMOVE,
	HASH_ENTER_NULL
} HASHACTION;

/* hash_seq 状态（调用方应视为不透明类型） */
typedef struct
{
	HTAB	   *hashp;
	uint32		curBucket;		/* 当前桶下标 */
	HASHELEMENT *curEntry;		/* 桶中当前条目 */
} HASH_SEQ_STATUS;

/*
 * dynahash.c 中函数的原型
 */
extern HTAB *hash_create(const char *tabname, long nelem,
						 const HASHCTL *info, int flags);
extern void hash_destroy(HTAB *hashp);
extern void hash_stats(const char *where, HTAB *hashp);
extern void *hash_search(HTAB *hashp, const void *keyPtr, HASHACTION action,
						 bool *foundPtr);
extern uint32 get_hash_value(HTAB *hashp, const void *keyPtr);
extern void *hash_search_with_hash_value(HTAB *hashp, const void *keyPtr,
										 uint32 hashvalue, HASHACTION action,
										 bool *foundPtr);
extern bool hash_update_hash_key(HTAB *hashp, void *existingEntry,
								 const void *newKeyPtr);
extern long hash_get_num_entries(HTAB *hashp);
extern void hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp);
extern void *hash_seq_search(HASH_SEQ_STATUS *status);
extern void hash_seq_term(HASH_SEQ_STATUS *status);
extern void hash_freeze(HTAB *hashp);
extern Size hash_estimate_size(long num_entries, Size entrysize);
extern long hash_select_dirsize(long num_entries);
extern Size hash_get_shared_size(HASHCTL *info, int flags);
extern void AtEOXact_HashTables(bool isCommit);
extern void AtEOSubXact_HashTables(bool isCommit, int nestDepth);

#endif							/* HSEARCH_H */
