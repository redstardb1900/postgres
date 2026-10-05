/*-------------------------------------------------------------------------
 *
 * dshash.c
 *	  由动态共享内存区支撑的并发哈希表。
 *
 * 这是开放哈希表，每个表项上挂一条
 * 链表。它支持动态调整大小，以免这些链表
 * 平均变得过长。目前只支持增大：
 * 哈希表从不缩小。
 *
 * 为应对并发，它有一组固定数量的分区，每个
 * 分区独立加锁。每个桶映射到一个分区；因此插入、
 * 查找和迭代通常只获取一把锁。于是，
 * 只要这些操作在锁分区这一层不发生冲突，
 * 就能得到良好的并发。不过，一旦开始调整大小，必须
 * 在短时间内同时取得全部分区锁。这
 * 预计只会发生少数几次，直到找到稳定的大小，
 * 因为增长是按几何级数进行的。
 *
 * 将来的版本也许会支持迭代器与增量调整大小；目前
 * 实现是最小化的。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/lib/dshash.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "lib/dshash.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"
#include "utils/memutils.h"

/*
 * 哈希表中的一项。这把用户的条目对象包在一个
 * 信封里，信封持有指回桶的指针以及指向桶中下
 * 一项的指针。
 */
struct dshash_table_item
{
	/* 同一桶中的下一项。 */
	dsa_pointer next;
	/* 已哈希的键，以免必须重新计算。 */
	dshash_hash hash;
	/* 用户的条目对象紧随其后。见 ENTRY_FROM_ITEM(item)。 */
};

/*
 * 为加锁目的的分区数。目前设为与 NUM_BUFFER_PARTITIONS
 * 匹配，依据是对缓冲池足够好的对任何其他目的也必须足够
 * 好。将来这可以变成运行时参数。
 *
 */
#define DSHASH_NUM_PARTITIONS_LOG2 7
#define DSHASH_NUM_PARTITIONS (1 << DSHASH_NUM_PARTITIONS_LOG2)

/* 用来识别我们哈希表的魔数。 */
#define DSHASH_MAGIC 0x75ff6a20

/*
 * 每个锁分区的跟踪信息。最初每个分区对应一个桶，但
 * 每次哈希表增长时，各分区覆盖的桶会分裂，覆盖的桶
 * 数加倍。
 *
 * 我们可能想在这里加填充，使每个分区在不同缓存行上，
 * 但那样会显著膨胀本结构。
 */
typedef struct dshash_partition
{
	LWLock		lock;			/* 保护本分区中的所有桶。 */
	size_t		count;			/* 本分区各桶中的项数 */
} dshash_partition;

/*
 * 哈希表的头对象。这将存在动态共享
 * 内存中。
 */
typedef struct dshash_table_control
{
	dshash_table_handle handle;
	uint32		magic;
	dshash_partition partitions[DSHASH_NUM_PARTITIONS];
	int			lwlock_tranche_id;

	/*
	 * 以下成员仅在持有全部分区锁时才写入。
	 * 持有任一分区锁时即可读取。
	 */

	/* 以 2 的幂表示的桶数（8 = 256 个桶）。 */
	size_t		size_log2;		/* log2(桶数) */
	dsa_pointer buckets;		/* 当前桶数组 */
} dshash_table_control;

/*
 * 动态哈希表的每后端状态。
 */
struct dshash_table
{
	dsa_area   *area;			/* 后备动态共享内存区。 */
	dshash_parameters params;	/* 参数。 */
	void	   *arg;			/* 用户提供的数据指针。 */
	dshash_table_control *control;	/* DSM 中的控制对象。 */
	dsa_pointer *buckets;		/* DSM 中的当前桶指针。 */
	size_t		size_log2;		/* log2(桶数) */
};

/* 给定指向项的指针，找出它持有的条目（用户数据）。 */
#define ENTRY_FROM_ITEM(item) \
	((char *)(item) + MAXALIGN(sizeof(dshash_table_item)))

/* 给定指向条目的指针，找出持有它的项。 */
#define ITEM_FROM_ENTRY(entry)											\
	((dshash_table_item *)((char *)(entry) -							\
							 MAXALIGN(sizeof(dshash_table_item))))

/* 已经发生了多少次调整大小操作（桶分裂）？ */
#define NUM_SPLITS(size_log2)					\
	(size_log2 - DSHASH_NUM_PARTITIONS_LOG2)

/* 给定大小下有多少个桶？ */
#define NUM_BUCKETS(size_log2)		\
	(((size_t) 1) << (size_log2))

/* 给定大小下每个分区有多少个桶？ */
#define BUCKETS_PER_PARTITION(size_log2)		\
	(((size_t) 1) << NUM_SPLITS(size_log2))

/* 需要增长前的最大条目数。一半 + 四分之一 = 75% 负载因子。 */
#define MAX_COUNT_PER_PARTITION(hash_table)				\
	(BUCKETS_PER_PARTITION(hash_table->size_log2) / 2 + \
	 BUCKETS_PER_PARTITION(hash_table->size_log2) / 4)

/* 根据哈希的最高位选择分区。 */
#define PARTITION_FOR_HASH(hash)										\
	(hash >> ((sizeof(dshash_hash) * CHAR_BIT) - DSHASH_NUM_PARTITIONS_LOG2))

/*
 * 找出给定哈希和表大小的桶下标。每次表大小加倍时，给定
 * 哈希值的适当桶会加倍，并可能加一，取决于新露出的位，
 * 从而所有桶都被分裂。
 *
 */
#define BUCKET_INDEX_FOR_HASH_AND_SIZE(hash, size_log2)		\
	(hash >> ((sizeof(dshash_hash) * CHAR_BIT) - (size_log2)))

/* 给定分区中第一个桶的下标。 */
#define BUCKET_INDEX_FOR_PARTITION(partition, size_log2)	\
	((partition) << NUM_SPLITS(size_log2))

/* 根据桶下标选择分区。 */
#define PARTITION_FOR_BUCKET_INDEX(bucket_idx, size_log2)				\
	((bucket_idx) >> NUM_SPLITS(size_log2))

/* 给定哈希值的活动桶的头（左值）。 */
#define BUCKET_FOR_HASH(hash_table, hash)								\
	(hash_table->buckets[												\
		BUCKET_INDEX_FOR_HASH_AND_SIZE(hash,							\
									   hash_table->size_log2)])

static void delete_item(dshash_table *hash_table,
						dshash_table_item *item);
static void resize(dshash_table *hash_table, size_t new_size_log2);
static inline void ensure_valid_bucket_pointers(dshash_table *hash_table);
static inline dshash_table_item *find_in_bucket(dshash_table *hash_table,
												const void *key,
												dsa_pointer item_pointer);
static void insert_item_into_bucket(dshash_table *hash_table,
									dsa_pointer item_pointer,
									dshash_table_item *item,
									dsa_pointer *bucket);
static dshash_table_item *insert_into_bucket(dshash_table *hash_table,
											 const void *key,
											 dsa_pointer *bucket);
static bool delete_key_from_bucket(dshash_table *hash_table,
								   const void *key,
								   dsa_pointer *bucket_head);
static bool delete_item_from_bucket(dshash_table *hash_table,
									dshash_table_item *item,
									dsa_pointer *bucket_head);
static inline dshash_hash hash_key(dshash_table *hash_table, const void *key);
static inline bool equal_keys(dshash_table *hash_table,
							  const void *a, const void *b);

#define PARTITION_LOCK(hash_table, i)			\
	(&(hash_table)->control->partitions[(i)].lock)

#define ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table) \
	Assert(!LWLockAnyHeldByMe(&(hash_table)->control->partitions[0].lock, \
		   DSHASH_NUM_PARTITIONS, sizeof(dshash_partition)))

/*
 * 用给定参数创建由给定动态共享区支持的新哈希表。返回的
 * 对象用当前 MemoryContext 分配在后端本地内存中。'arg'
 * 会传给比较函数和哈希函数。
 *
 */
dshash_table *
dshash_create(dsa_area *area, const dshash_parameters *params, void *arg)
{
	dshash_table *hash_table;
	dsa_pointer control;

	/* 分配表示哈希表的后端本地对象。 */
	hash_table = palloc(sizeof(dshash_table));

	/* 在共享内存中分配控制对象。 */
	control = dsa_allocate(area, sizeof(dshash_table_control));

	/* 设置本地和共享哈希表结构。 */
	hash_table->area = area;
	hash_table->params = *params;
	hash_table->arg = arg;
	hash_table->control = dsa_get_address(area, control);
	hash_table->control->handle = control;
	hash_table->control->magic = DSHASH_MAGIC;
	hash_table->control->lwlock_tranche_id = params->tranche_id;

	/* 设置锁分区数组。 */
	{
		dshash_partition *partitions = hash_table->control->partitions;
		int			tranche_id = hash_table->control->lwlock_tranche_id;
		int			i;

		for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
		{
			LWLockInitialize(&partitions[i].lock, tranche_id);
			partitions[i].count = 0;
		}
	}

	/*
	 * 设置初始桶数组。我们的初始大小与
	 * 分区数相同。
	 */
	hash_table->control->size_log2 = DSHASH_NUM_PARTITIONS_LOG2;
	hash_table->control->buckets =
		dsa_allocate_extended(area,
							  sizeof(dsa_pointer) * DSHASH_NUM_PARTITIONS,
							  DSA_ALLOC_NO_OOM | DSA_ALLOC_ZERO);
	if (!DsaPointerIsValid(hash_table->control->buckets))
	{
		dsa_free(area, control);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on DSA request of size %zu.",
						   sizeof(dsa_pointer) * DSHASH_NUM_PARTITIONS)));
	}
	hash_table->buckets = dsa_get_address(area,
										  hash_table->control->buckets);
	hash_table->size_log2 = hash_table->control->size_log2;

	return hash_table;
}

/*
 * 用句柄附着到已有哈希表。返回的对象用当前 MemoryContext
 * 分配在后端本地内存中。'arg' 会传给比较函数和哈希函数。
 *
 */
dshash_table *
dshash_attach(dsa_area *area, const dshash_parameters *params,
			  dshash_table_handle handle, void *arg)
{
	dshash_table *hash_table;
	dsa_pointer control;

	/* 分配表示哈希表的后端本地对象。 */
	hash_table = palloc(sizeof(dshash_table));

	/* 在共享内存中找到控制对象。 */
	control = handle;

	/* 设置本地哈希表结构。 */
	hash_table->area = area;
	hash_table->params = *params;
	hash_table->arg = arg;
	hash_table->control = dsa_get_address(area, control);
	Assert(hash_table->control->magic == DSHASH_MAGIC);

	/*
	 * 这些稍后会由 ensure_valid_bucket_pointers() 设为正确
	 * 值，那时我们将持有分区锁以与并发调整大小联锁。
 *
 */
	hash_table->buckets = NULL;
	hash_table->size_log2 = 0;

	return hash_table;
}

/*
 * 从哈希表分离。这释放与哈希表关联的后端本地资源，但
 * 哈希表会继续存在，直到它被（仍附着的后端）显式销毁，
 * 或支持它的区域被归还给操作系统。
 *
 */
void
dshash_detach(dshash_table *hash_table)
{
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	/* 哈希表可能已被销毁。只释放本地内存。 */
	pfree(hash_table);
}

/*
 * 销毁哈希表，把所有内存还给区域。调用者必须确定在调用
 * 本函数之前没有其他后端会试图访问该哈希表。其他后端必须
 * 显式调用 dshash_detach 以释放与哈希表关联的后端本地内存。
 * 调用 dshash_destroy 的后端不得调用 dshash_detach。
 *
 */
void
dshash_destroy(dshash_table *hash_table)
{
	size_t		size;
	size_t		i;

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ensure_valid_bucket_pointers(hash_table);

	/* 释放所有条目。 */
	size = NUM_BUCKETS(hash_table->size_log2);
	for (i = 0; i < size; ++i)
	{
		dsa_pointer item_pointer = hash_table->buckets[i];

		while (DsaPointerIsValid(item_pointer))
		{
			dshash_table_item *item;
			dsa_pointer next_item_pointer;

			item = dsa_get_address(hash_table->area, item_pointer);
			next_item_pointer = item->next;
			dsa_free(hash_table->area, item_pointer);
			item_pointer = next_item_pointer;
		}
	}

	/*
	 * 破坏控制块，以帮助捕获其他后端访问本哈希表
	 * 原先占用的内存的编程错误。
	 */
	hash_table->control->magic = 0;

	/* 释放活动表和控制对象。 */
	dsa_free(hash_table->area, hash_table->control->buckets);
	dsa_free(hash_table->area, hash_table->control->handle);

	pfree(hash_table);
}

/*
 * 取得可供其他进程用来附着到本哈希表的
 * 句柄。
 */
dshash_table_handle
dshash_get_hash_table_handle(dshash_table *hash_table)
{
	Assert(hash_table->control->magic == DSHASH_MAGIC);

	return hash_table->control->handle;
}

/*
 * 按键查找条目。若能找到具有给定键的条目则返回指向它的
 * 指针。若找不到键则返回 NULL。若返回非 NULL 值，则该条
 * 目被锁定，必须调用 dshash_release_lock 来释放。若在调用
 * dshash_release_lock 之前引发错误，锁会自动释放，但调用
 * 者必须注意确保条目不被留下损坏。锁模式按 'exclusive'
 * 为共享或排他。
 *
 * 调用者不得已经持有锁。
 *
 * 注意所持的锁实际上是 LWLock，因此从本函数返回时中断
 * 会被保持，直到调用 dshash_release_lock 才恢复。调用者
 * 尽快释放锁是很好的主意。
 *
 */
void *
dshash_find(dshash_table *hash_table, const void *key, bool exclusive)
{
	dshash_hash hash;
	size_t		partition;
	dshash_table_item *item;

	hash = hash_key(hash_table, key);
	partition = PARTITION_FOR_HASH(hash);

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	LWLockAcquire(PARTITION_LOCK(hash_table, partition),
				  exclusive ? LW_EXCLUSIVE : LW_SHARED);
	ensure_valid_bucket_pointers(hash_table);

	/* 搜索活动桶。 */
	item = find_in_bucket(hash_table, key, BUCKET_FOR_HASH(hash_table, hash));

	if (!item)
	{
		/* 未找到。 */
		LWLockRelease(PARTITION_LOCK(hash_table, partition));
		return NULL;
	}
	else
	{
		/* 调用者将通过调用 dshash_release_lock 释放锁。 */
		return ENTRY_FROM_ITEM(item);
	}
}

/*
 * 返回指向必须用 dshash_release_lock 释放的排他锁定项的
 * 指针。若在哈希表中找到该键，'found' 设为 true 并返回
 * 指向已有条目的指针。若未找到该键，'found' 设为 false，
 * 并返回指向新创建条目的指针。
 *
 * 上面关于 dshash_find() 的加锁和错误处理说明同样适用
 * 于此。
 *
 */
void *
dshash_find_or_insert(dshash_table *hash_table,
					  const void *key,
					  bool *found)
{
	dshash_hash hash;
	size_t		partition_index;
	dshash_partition *partition;
	dshash_table_item *item;

	hash = hash_key(hash_table, key);
	partition_index = PARTITION_FOR_HASH(hash);
	partition = &hash_table->control->partitions[partition_index];

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

restart:
	LWLockAcquire(PARTITION_LOCK(hash_table, partition_index),
				  LW_EXCLUSIVE);
	ensure_valid_bucket_pointers(hash_table);

	/* 搜索活动桶。 */
	item = find_in_bucket(hash_table, key, BUCKET_FOR_HASH(hash_table, hash));

	if (item)
		*found = true;
	else
	{
		*found = false;

		/* 检查我们是否太满。 */
		if (partition->count > MAX_COUNT_PER_PARTITION(hash_table))
		{
			/*
			 * 本分区保护的所有桶的负载因子（= 键 / 桶）
			 * > 0.75。大概整个哈希表普遍如此（不过我们
			 * 不试图直接跟踪以免在某种中心计数器上争用；
			 * 我们只假定本分区有代表性）。这是调整大小
			 * 的好时机。
			 *
			 * 先放弃我们已有的锁，因为调整大小需要按正确
			 * 顺序重新获取所有锁以避免死锁。
 *
 */
			LWLockRelease(PARTITION_LOCK(hash_table, partition_index));
			resize(hash_table, hash_table->size_log2 + 1);

			goto restart;
		}

		/* 终于可以尝试插入新项。 */
		item = insert_into_bucket(hash_table, key,
								  &BUCKET_FOR_HASH(hash_table, hash));
		item->hash = hash;
		/* 为负载因子知识调整每锁分区计数器。 */
		++partition->count;
	}

	/* 调用者必须用 dshash_release_lock 释放锁。 */
	return ENTRY_FROM_ITEM(item);
}

/*
 * 按键移除条目。若找到键且对应条目被移除则返回
 * true。
 *
 * 要删除你已有指针的条目，见
 * dshash_delete_entry。
 */
bool
dshash_delete_key(dshash_table *hash_table, const void *key)
{
	dshash_hash hash;
	size_t		partition;
	bool		found;

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	hash = hash_key(hash_table, key);
	partition = PARTITION_FOR_HASH(hash);

	LWLockAcquire(PARTITION_LOCK(hash_table, partition), LW_EXCLUSIVE);
	ensure_valid_bucket_pointers(hash_table);

	if (delete_key_from_bucket(hash_table, key,
							   &BUCKET_FOR_HASH(hash_table, hash)))
	{
		Assert(hash_table->control->partitions[partition].count > 0);
		found = true;
		--hash_table->control->partitions[partition].count;
	}
	else
		found = false;

	LWLockRelease(PARTITION_LOCK(hash_table, partition));

	return found;
}

/*
 * 移除一个条目。该条目必须已经排他锁定，并且必须
 * 由 dshash_find 或 dshash_find_or_insert 获得。注意本
 * 函数像 dshash_release_lock 一样释放锁。
 *
 * 要按键删除条目，见 dshash_delete_key。
 */
void
dshash_delete_entry(dshash_table *hash_table, void *entry)
{
	dshash_table_item *item = ITEM_FROM_ENTRY(entry);
	size_t		partition = PARTITION_FOR_HASH(item->hash);

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	Assert(LWLockHeldByMeInMode(PARTITION_LOCK(hash_table, partition),
								LW_EXCLUSIVE));

	delete_item(hash_table, item);
	LWLockRelease(PARTITION_LOCK(hash_table, partition));
}

/*
 * 解锁由 dshash_find 或 dshash_find_or_insert 锁定的条目。
 */
void
dshash_release_lock(dshash_table *hash_table, void *entry)
{
	dshash_table_item *item = ITEM_FROM_ENTRY(entry);
	size_t		partition_index = PARTITION_FOR_HASH(item->hash);

	Assert(hash_table->control->magic == DSHASH_MAGIC);

	LWLockRelease(PARTITION_LOCK(hash_table, partition_index));
}

/*
 * 转发给 memcmp 的比较函数。
 */
int
dshash_memcmp(const void *a, const void *b, size_t size, void *arg)
{
	return memcmp(a, b, size);
}

/*
 * 转发给 tag_hash 的哈希函数。
 */
dshash_hash
dshash_memhash(const void *v, size_t size, void *arg)
{
	return tag_hash(v, size);
}

/*
 * 顺序扫描 dshash 表并逐个返回所有元素，当所有元素
 * 都已返回时返回 NULL。
 *
 * 扫描结束时需要调用 dshash_seq_term。若 exclusive =
 * true，调用者可在扫描中途用 dshash_delete_current()
 * 删除返回的元素。
 */
void
dshash_seq_init(dshash_seq_status *status, dshash_table *hash_table,
				bool exclusive)
{
	status->hash_table = hash_table;
	status->curbucket = 0;
	status->nbuckets = 0;
	status->curitem = NULL;
	status->pnextitem = InvalidDsaPointer;
	status->curpartition = -1;
	status->exclusive = exclusive;
}

/*
 * 返回下一个元素。
 *
 * 返回的元素被锁定，调用者不得释放该锁。它由将来对
 * dshash_seq_next() 或 dshash_seq_term() 的调用释放。
 */
void *
dshash_seq_next(dshash_seq_status *status)
{
	dsa_pointer next_item_pointer;

	/*
	 * 尚未持有任何分区锁。需要确定哈希表的大小，
	 * 自我们上次查看以来它可能已被调整大小。由于
	 * 我们按分区顺序迭代，可以先无条件锁定分区 0。
	 *
	 * 一旦持有锁，直到扫描结束都不会发生调整大小。
	 * 因此我们不必反复调用 ensure_valid_bucket_pointers()。
 *
 */
	if (status->curpartition == -1)
	{
		Assert(status->curbucket == 0);
		ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(status->hash_table);

		status->curpartition = 0;

		LWLockAcquire(PARTITION_LOCK(status->hash_table,
									 status->curpartition),
					  status->exclusive ? LW_EXCLUSIVE : LW_SHARED);

		ensure_valid_bucket_pointers(status->hash_table);

		status->nbuckets =
			NUM_BUCKETS(status->hash_table->control->size_log2);
		next_item_pointer = status->hash_table->buckets[status->curbucket];
	}
	else
		next_item_pointer = status->pnextitem;

	Assert(LWLockHeldByMeInMode(PARTITION_LOCK(status->hash_table,
											   status->curpartition),
								status->exclusive ? LW_EXCLUSIVE : LW_SHARED));

	/* 若已完成当前桶则移到下一个桶 */
	while (!DsaPointerIsValid(next_item_pointer))
	{
		int			next_partition;

		if (++status->curbucket >= status->nbuckets)
		{
			/* 所有桶都已扫描完。结束。 */
			return NULL;
		}

		/* 检查是否移到下一个分区 */
		next_partition =
			PARTITION_FOR_BUCKET_INDEX(status->curbucket,
									   status->hash_table->size_log2);

		if (status->curpartition != next_partition)
		{
			/*
			 * 移到下一个分区。先锁下一个分区再释放
			 * 当前分区，不要反过来，以避免并发调整
			 * 大小。按与 resize() 相同的顺序取锁以避免
			 * 死锁。
			 */
			LWLockAcquire(PARTITION_LOCK(status->hash_table,
										 next_partition),
						  status->exclusive ? LW_EXCLUSIVE : LW_SHARED);
			LWLockRelease(PARTITION_LOCK(status->hash_table,
										 status->curpartition));
			status->curpartition = next_partition;
		}

		next_item_pointer = status->hash_table->buckets[status->curbucket];
	}

	status->curitem =
		dsa_get_address(status->hash_table->area, next_item_pointer);

	/*
	 * 调用者可能删除该项。为防删除，先存下一项。
	 *
	 */
	status->pnextitem = status->curitem->next;

	return ENTRY_FROM_ITEM(status->curitem);
}

/*
 * 终止顺序扫描并释放所有锁。
 *
 * 在完成或退出顺序扫描时需要调用。
 */
void
dshash_seq_term(dshash_seq_status *status)
{
	if (status->curpartition >= 0)
		LWLockRelease(PARTITION_LOCK(status->hash_table, status->curpartition));
}

/*
 * 移除顺序扫描的当前条目。
 */
void
dshash_delete_current(dshash_seq_status *status)
{
	dshash_table *hash_table = status->hash_table;
	dshash_table_item *item = status->curitem;
	size_t		partition PG_USED_FOR_ASSERTS_ONLY;

	partition = PARTITION_FOR_HASH(item->hash);

	Assert(status->exclusive);
	Assert(hash_table->control->magic == DSHASH_MAGIC);
	Assert(LWLockHeldByMeInMode(PARTITION_LOCK(hash_table, partition),
								LW_EXCLUSIVE));

	delete_item(hash_table, item);
}

/*
 * 把哈希表内部状态的调试信息打印到
 * stderr。调用者不得持有任何分区锁。
 */
void
dshash_dump(dshash_table *hash_table)
{
	size_t		i;
	size_t		j;

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
	{
		Assert(!LWLockHeldByMe(PARTITION_LOCK(hash_table, i)));
		LWLockAcquire(PARTITION_LOCK(hash_table, i), LW_SHARED);
	}

	ensure_valid_bucket_pointers(hash_table);

	fprintf(stderr,
			"hash table size = %zu\n", (size_t) 1 << hash_table->size_log2);
	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
	{
		dshash_partition *partition = &hash_table->control->partitions[i];
		size_t		begin = BUCKET_INDEX_FOR_PARTITION(i, hash_table->size_log2);
		size_t		end = BUCKET_INDEX_FOR_PARTITION(i + 1, hash_table->size_log2);

		fprintf(stderr, "  partition %zu\n", i);
		fprintf(stderr,
				"    active buckets (key count = %zu)\n", partition->count);

		for (j = begin; j < end; ++j)
		{
			size_t		count = 0;
			dsa_pointer bucket = hash_table->buckets[j];

			while (DsaPointerIsValid(bucket))
			{
				dshash_table_item *item;

				item = dsa_get_address(hash_table->area, bucket);

				bucket = item->next;
				++count;
			}
			fprintf(stderr, "      bucket %zu (key count = %zu)\n", j, count);
		}
	}

	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
		LWLockRelease(PARTITION_LOCK(hash_table, i));
}

/*
 * 删除我们持有指针的已锁定项。
 */
static void
delete_item(dshash_table *hash_table, dshash_table_item *item)
{
	size_t		hash = item->hash;
	size_t		partition = PARTITION_FOR_HASH(hash);

	Assert(LWLockHeldByMe(PARTITION_LOCK(hash_table, partition)));

	if (delete_item_from_bucket(hash_table, item,
								&BUCKET_FOR_HASH(hash_table, hash)))
	{
		Assert(hash_table->control->partitions[partition].count > 0);
		--hash_table->control->partitions[partition].count;
	}
	else
	{
		Assert(false);
	}
}

/*
 * 如有必要，把哈希表增长到所请求的桶数。所请求的
 * 大小必须是某个先前观察到的大小的两倍。
 *
 * 必须在未持有任何分区锁时调用。
 */
static void
resize(dshash_table *hash_table, size_t new_size_log2)
{
	dsa_pointer old_buckets;
	dsa_pointer new_buckets_shared;
	dsa_pointer *new_buckets;
	size_t		size;
	size_t		new_size = ((size_t) 1) << new_size_log2;
	size_t		i;

	/*
	 * 获取所有锁分区的锁。这很昂贵，但我们
	 * 不应必须做很多次。
	 */
	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
	{
		Assert(!LWLockHeldByMe(PARTITION_LOCK(hash_table, i)));

		LWLockAcquire(PARTITION_LOCK(hash_table, i), LW_EXCLUSIVE);
		if (i == 0 && hash_table->control->size_log2 >= new_size_log2)
		{
			/*
			 * 另一个后端已经增大了大小；我们可以避免
			 * 获取所有锁并提早返回。
			 */
			LWLockRelease(PARTITION_LOCK(hash_table, 0));
			return;
		}
	}

	Assert(new_size_log2 == hash_table->control->size_log2 + 1);

	/* 为新表分配空间。 */
	new_buckets_shared =
		dsa_allocate_extended(hash_table->area,
							  sizeof(dsa_pointer) * new_size,
							  DSA_ALLOC_HUGE | DSA_ALLOC_ZERO);
	new_buckets = dsa_get_address(hash_table->area, new_buckets_shared);

	/*
	 * 我们已经分配了新桶数组；现在剩下要做的就是
	 * 重新插入所有项，这相当于调整所有指针。
	 */
	size = ((size_t) 1) << hash_table->control->size_log2;
	for (i = 0; i < size; ++i)
	{
		dsa_pointer item_pointer = hash_table->buckets[i];

		while (DsaPointerIsValid(item_pointer))
		{
			dshash_table_item *item;
			dsa_pointer next_item_pointer;

			item = dsa_get_address(hash_table->area, item_pointer);
			next_item_pointer = item->next;
			insert_item_into_bucket(hash_table, item_pointer, item,
									&new_buckets[BUCKET_INDEX_FOR_HASH_AND_SIZE(item->hash,
																				new_size_log2)]);
			item_pointer = next_item_pointer;
		}
	}

	/* 把哈希表换到位并释放旧的。 */
	old_buckets = hash_table->control->buckets;
	hash_table->control->buckets = new_buckets_shared;
	hash_table->control->size_log2 = new_size_log2;
	hash_table->buckets = new_buckets;
	dsa_free(hash_table->area, old_buckets);

	/* 释放所有锁。 */
	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
		LWLockRelease(PARTITION_LOCK(hash_table, i));
}

/*
 * 确保我们的后端本地桶指针是最新的。调用者必须
 * 已锁定一个锁分区，这防止 resize() 并发运行。
 *
 */
static inline void
ensure_valid_bucket_pointers(dshash_table *hash_table)
{
	if (hash_table->size_log2 != hash_table->control->size_log2)
	{
		hash_table->buckets = dsa_get_address(hash_table->area,
											  hash_table->control->buckets);
		hash_table->size_log2 = hash_table->control->size_log2;
	}
}

/*
 * 用提供的比较函数扫描一个已锁定的桶以寻找匹配。
 */
static inline dshash_table_item *
find_in_bucket(dshash_table *hash_table, const void *key,
			   dsa_pointer item_pointer)
{
	while (DsaPointerIsValid(item_pointer))
	{
		dshash_table_item *item;

		item = dsa_get_address(hash_table->area, item_pointer);
		if (equal_keys(hash_table, key, ENTRY_FROM_ITEM(item)))
			return item;
		item_pointer = item->next;
	}
	return NULL;
}

/*
 * 把一个已分配的项插入桶中。
 */
static void
insert_item_into_bucket(dshash_table *hash_table,
						dsa_pointer item_pointer,
						dshash_table_item *item,
						dsa_pointer *bucket)
{
	Assert(item == dsa_get_address(hash_table->area, item_pointer));

	item->next = *bucket;
	*bucket = item_pointer;
}

/*
 * 为具有给定键的条目分配空间并把它插入
 * 提供的桶。
 */
static dshash_table_item *
insert_into_bucket(dshash_table *hash_table,
				   const void *key,
				   dsa_pointer *bucket)
{
	dsa_pointer item_pointer;
	dshash_table_item *item;

	item_pointer = dsa_allocate(hash_table->area,
								hash_table->params.entry_size +
								MAXALIGN(sizeof(dshash_table_item)));
	item = dsa_get_address(hash_table->area, item_pointer);
	memcpy(ENTRY_FROM_ITEM(item), key, hash_table->params.key_size);
	insert_item_into_bucket(hash_table, item_pointer, item, bucket);
	return item;
}

/*
 * 在桶中搜索匹配的键并删除它。
 */
static bool
delete_key_from_bucket(dshash_table *hash_table,
					   const void *key,
					   dsa_pointer *bucket_head)
{
	while (DsaPointerIsValid(*bucket_head))
	{
		dshash_table_item *item;

		item = dsa_get_address(hash_table->area, *bucket_head);

		if (equal_keys(hash_table, key, ENTRY_FROM_ITEM(item)))
		{
			dsa_pointer next;

			next = item->next;
			dsa_free(hash_table->area, *bucket_head);
			*bucket_head = next;

			return true;
		}
		bucket_head = &item->next;
	}
	return false;
}

/*
 * 从桶中删除指定项。
 */
static bool
delete_item_from_bucket(dshash_table *hash_table,
						dshash_table_item *item,
						dsa_pointer *bucket_head)
{
	while (DsaPointerIsValid(*bucket_head))
	{
		dshash_table_item *bucket_item;

		bucket_item = dsa_get_address(hash_table->area, *bucket_head);

		if (bucket_item == item)
		{
			dsa_pointer next;

			next = item->next;
			dsa_free(hash_table->area, *bucket_head);
			*bucket_head = next;
			return true;
		}
		bucket_head = &bucket_item->next;
	}
	return false;
}

/*
 * 计算键的哈希值。
 */
static inline dshash_hash
hash_key(dshash_table *hash_table, const void *key)
{
	return hash_table->params.hash_function(key,
											hash_table->params.key_size,
											hash_table->arg);
}

/*
 * 检查两个键是否比较相等。
 */
static inline bool
equal_keys(dshash_table *hash_table, const void *a, const void *b)
{
	return hash_table->params.compare_function(a, b,
											   hash_table->params.key_size,
											   hash_table->arg) == 0;
}
