/* -------------------------------------------------------------------------
 *
 * pgstat_shmem.c
 *	  共享内存中统计条目的存储。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_shmem.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgstat.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"


#define PGSTAT_ENTRY_REF_HASH_SIZE	128

/* 用于查找键对应 PgStat_EntryRef 的哈希表条目 */
typedef struct PgStat_EntryRefHashEntry
{
	PgStat_HashKey key;			/* 哈希 */
	char		status;			/* 供 simplehash 用 */
	PgStat_EntryRef *entry_ref;
} PgStat_EntryRefHashEntry;


/* 指向共享统计条目的引用 */
#define SH_PREFIX pgstat_entry_ref_hash
#define SH_ELEMENT_TYPE PgStat_EntryRefHashEntry
#define SH_KEY_TYPE PgStat_HashKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) \
	pgstat_hash_hash_key(&key, sizeof(PgStat_HashKey), NULL)
#define SH_EQUAL(tb, a, b) \
	pgstat_cmp_hash_key(&a, &b, sizeof(PgStat_HashKey), NULL) == 0
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"


static void pgstat_drop_database_and_contents(Oid dboid);

static void pgstat_free_entry(PgStatShared_HashEntry *shent, dshash_seq_status *hstat);

static void pgstat_release_entry_ref(PgStat_HashKey key, PgStat_EntryRef *entry_ref, bool discard_pending);
static bool pgstat_need_entry_refs_gc(void);
static void pgstat_gc_entry_refs(void);
static void pgstat_release_all_entry_refs(bool discard_pending);
typedef bool (*ReleaseMatchCB) (PgStat_EntryRefHashEntry *, Datum data);
static void pgstat_release_matching_entry_refs(bool discard_pending, ReleaseMatchCB match, Datum match_data);

static void pgstat_setup_memcxt(void);


/* 共享哈希的参数 */
static const dshash_parameters dsh_params = {
	sizeof(PgStat_HashKey),
	sizeof(PgStatShared_HashEntry),
	pgstat_cmp_hash_key,
	pgstat_hash_hash_key,
	LWTRANCHE_PGSTATS_HASH
};


/*
 * 后端本地的共享统计条目引用。若有待写入的
 * 更新，则 PgStat_EntryRef 会被加入 pgStatPending 列表
 * 
 *
 * 某统计条目被删除时，各后端都需先释放自己
 * 的引用，这块内存才能被释放。为此
 * pgStatLocal.shmem->gc_request_count 会递增，各后端会定期
 * 将其与自己的 pgStatSharedRefAge 副本进行比较。
 */
static pgstat_entry_ref_hash_hash *pgStatEntryRefHash = NULL;
static int	pgStatSharedRefAge = 0; /* pgStatShmLookupCache 缓存年龄 */

/*
 * 包含 pgStatEntryRefHash 哈希表与
 * pgStatSharedRef 条目的内存上下文。分开保存，以便更易
 * 追踪 / 归属内存用量。
 */
static MemoryContext pgStatSharedRefContext = NULL;
static MemoryContext pgStatEntryRefHashContext = NULL;


/* ------------------------------------------------------------
 * 以下为 postmaster 调用的公共函数
 * ------------------------------------------------------------
 */

/*
 * 存放在共享统计哈希表中的统计所用的共享
 * 内存分配的大小。该分配将作为主共享内存
 * 的一部分完成，而非动态共享内存，初始化点为
 * postmaster
 */
static Size
pgstat_dsa_init_size(void)
{
	Size		sz;

	/*
	 * dshash 头部 / 初始桶数组必须能放进"普通"共
	 * 享内存，但最好无需立即用到
	 * dsm 段。256kB 的大小效果不错，
	 * 且相对于其他固定大小的共享内存分配
	 * 而言并不失衡。注：进一步避免 DSMs 可配置
	 * min_dynamic_shared_memory
	 */
	sz = 256 * 1024;
	Assert(dsa_minimum_size() <= sz);
	return MAXALIGN(sz);
}

/*
 * 计算累计统计所需的共享内存空间
 */
Size
StatsShmemSize(void)
{
	Size		sz;

	sz = MAXALIGN(sizeof(PgStat_ShmemControl));
	sz = add_size(sz, pgstat_dsa_init_size());

	return sz;
}

/*
 * 启动期间初始化累计统计系统
 */
void
StatsShmemInit(void)
{
	bool		found;
	Size		sz;

	sz = StatsShmemSize();
	pgStatLocal.shmem = (PgStat_ShmemControl *)
		ShmemInitStruct("Shared Memory Stats", sz, &found);

	if (!IsUnderPostmaster)
	{
		dsa_area   *dsa;
		dshash_table *dsh;
		PgStat_ShmemControl *ctl = pgStatLocal.shmem;
		char	   *p = (char *) ctl;

		Assert(!found);

		/* pgStatLocal.shmem 本身的分配 */
		p += MAXALIGN(sizeof(PgStat_ShmemControl));

		/*
		 * 在普通共享内存中创建一个 dsa 分配。这是
		 * 必需的，因为 postmaster 无法使用 dsm 段。这也
		 * 带来一点效率收益。
		 */
		ctl->raw_dsa_area = p;
		p += MAXALIGN(pgstat_dsa_init_size());
		dsa = dsa_create_in_place(ctl->raw_dsa_area,
								  pgstat_dsa_init_size(),
								  LWTRANCHE_PGSTATS_DSA, 0);
		dsa_pin(dsa);

		/*
		 * 为确保 dshash 创建于"普通"共享内存中，先暂时
		 * 把 dsa 限制为 dsa 的初始大小。
		 */
		dsa_set_size_limit(dsa, pgstat_dsa_init_size());

		/*
		 * 限制就位后，创建 dshash 表。XXX：若能有
		 * dshash_create_in_place() 就好了。
		 */
		dsh = dshash_create(dsa, &dsh_params, 0);
		ctl->hash_handle = dshash_get_hash_table_handle(dsh);

		/* 解除上述限制 */
		dsa_set_size_limit(dsa, -1);

		/*
		 * postmaster 不会再访问这些，因此释放本地的
		 * dsa/dshash 引用。
		 */
		dshash_detach(dsh);
		dsa_detach(dsa);

		pg_atomic_init_u64(&ctl->gc_request_count, 1);


		/* 初始化固定编号统计 */
		LWLockInitialize(&ctl->archiver.lock, LWTRANCHE_PGSTATS_DATA);
		LWLockInitialize(&ctl->bgwriter.lock, LWTRANCHE_PGSTATS_DATA);
		LWLockInitialize(&ctl->checkpointer.lock, LWTRANCHE_PGSTATS_DATA);
		LWLockInitialize(&ctl->slru.lock, LWTRANCHE_PGSTATS_DATA);
		LWLockInitialize(&ctl->wal.lock, LWTRANCHE_PGSTATS_DATA);

		for (int i = 0; i < BACKEND_NUM_TYPES; i++)
			LWLockInitialize(&ctl->io.locks[i],
							 LWTRANCHE_PGSTATS_DATA);
	}
	else
	{
		Assert(found);
	}
}

void
pgstat_attach_shmem(void)
{
	MemoryContext oldcontext;

	Assert(pgStatLocal.dsa == NULL);

	/* 统计共享内存与后端生命周期共存 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	pgStatLocal.dsa = dsa_attach_in_place(pgStatLocal.shmem->raw_dsa_area,
										  NULL);
	dsa_pin_mapping(pgStatLocal.dsa);

	pgStatLocal.shared_hash = dshash_attach(pgStatLocal.dsa, &dsh_params,
											pgStatLocal.shmem->hash_handle, 0);

	MemoryContextSwitchTo(oldcontext);
}

void
pgstat_detach_shmem(void)
{
	Assert(pgStatLocal.dsa);

	/* 不应留下对共享统计的引用 */
	pgstat_release_all_entry_refs(false);

	dshash_detach(pgStatLocal.shared_hash);
	pgStatLocal.shared_hash = NULL;

	dsa_detach(pgStatLocal.dsa);

	/*
	 * dsa_detach() 不会递减 DSA 引用计数，因为没有段
	 * 被提供给 dsa_attach_in_place()，导致没有注册任何清理
	 * 回调。因此现在手动释放它。
	 */
	dsa_release_in_place(pgStatLocal.shmem->raw_dsa_area);

	pgStatLocal.dsa = NULL;
}


/* ------------------------------------------------------------
 * 共享内存统计条目的维护
 * ------------------------------------------------------------
 */

/*
 * 初始化新建的条目。
 *
 * 分配失败时返回 NULL，以便调用方能够
 * 采取清理动作，因为已初始化的条目已插入共享
 * 哈希表。
 */
PgStatShared_Common *
pgstat_init_entry(PgStat_Kind kind,
				  PgStatShared_HashEntry *shhashent)
{
	/* 新建统计条目。 */
	dsa_pointer chunk;
	PgStatShared_Common *shheader;

	/*
	 * 将引用计数初始化为 1，标记有效 / 未删除。该条目
	 * 在初始化完成前不能被释放，因为只要我们还持
	 * 有 dshash 分区锁就找不到它。若需要更长寿命的
	 * 引用，调用方需要进一步增加它。
	 */
	pg_atomic_init_u32(&shhashent->refcount, 1);

	/*
	 * 将"generation"初始化为 0，即新建。
	 */
	pg_atomic_init_u32(&shhashent->generation, 0);
	shhashent->dropped = false;

	chunk = dsa_allocate_extended(pgStatLocal.dsa,
								  pgstat_get_kind_info(kind)->shared_size,
								  DSA_ALLOC_ZERO | DSA_ALLOC_NO_OOM);
	if (chunk == InvalidDsaPointer)
		return NULL;

	shheader = dsa_get_address(pgStatLocal.dsa, chunk);
	shheader->magic = 0xdeadbeef;

	/* 从哈希条目链接新条目。 */
	shhashent->body = chunk;

	LWLockInitialize(&shheader->lock, LWTRANCHE_PGSTATS_DATA);

	return shheader;
}

static PgStatShared_Common *
pgstat_reinit_entry(PgStat_Kind kind, PgStatShared_HashEntry *shhashent)
{
	PgStatShared_Common *shheader;

	shheader = dsa_get_address(pgStatLocal.dsa, shhashent->body);

	/* 标记为不再已删除 */
	pg_atomic_fetch_add_u32(&shhashent->refcount, 1);

	/*
	 * 递增"generation"，让任何持有本地引用的后端知道
	 * 它们所指向的内容已过时。
	 */
	pg_atomic_fetch_add_u32(&shhashent->generation, 1);
	shhashent->dropped = false;

	/* 重初始化内容 */
	Assert(shheader->magic == 0xdeadbeef);
	memset(pgstat_get_entry_data(kind, shheader), 0,
		   pgstat_get_entry_len(kind));

	return shheader;
}

static void
pgstat_setup_shared_refs(void)
{
	if (likely(pgStatEntryRefHash != NULL))
		return;

	pgStatEntryRefHash =
		pgstat_entry_ref_hash_create(pgStatEntryRefHashContext,
									 PGSTAT_ENTRY_REF_HASH_SIZE, NULL);
	pgStatSharedRefAge = pg_atomic_read_u64(&pgStatLocal.shmem->gc_request_count);
	Assert(pgStatSharedRefAge != 0);
}

/*
 * pgstat_get_entry_ref() 的辅助函数。
 */
static void
pgstat_acquire_entry_ref(PgStat_EntryRef *entry_ref,
						 PgStatShared_HashEntry *shhashent,
						 PgStatShared_Common *shheader)
{
	Assert(shheader->magic == 0xdeadbeef);
	Assert(pg_atomic_read_u32(&shhashent->refcount) > 0);

	pg_atomic_fetch_add_u32(&shhashent->refcount, 1);

	dshash_release_lock(pgStatLocal.shared_hash, shhashent);

	entry_ref->shared_stats = shheader;
	entry_ref->shared_entry = shhashent;
	entry_ref->generation = pg_atomic_read_u32(&shhashent->generation);
}

/*
 * pgstat_get_entry_ref() 的辅助函数。
 */
static bool
pgstat_get_entry_ref_cached(PgStat_HashKey key, PgStat_EntryRef **entry_ref_p)
{
	bool		found;
	PgStat_EntryRefHashEntry *cache_entry;

	/*
	 * 我们立即插入缓存条目，因为这避免了 1) 多次
	 * 缓存未命中时的哈希表查找 2) 在递增
	 * PgStatShared_Common->refcount 之后就得处理内存不足错误。
	 */

	cache_entry = pgstat_entry_ref_hash_insert(pgStatEntryRefHash, key, &found);

	if (!found || !cache_entry->entry_ref)
	{
		PgStat_EntryRef *entry_ref;

		cache_entry->entry_ref = entry_ref =
			MemoryContextAlloc(pgStatSharedRefContext,
							   sizeof(PgStat_EntryRef));
		entry_ref->shared_stats = NULL;
		entry_ref->shared_entry = NULL;
		entry_ref->pending = NULL;

		found = false;
	}
	else if (cache_entry->entry_ref->shared_stats == NULL)
	{
		Assert(cache_entry->entry_ref->pending == NULL);
		found = false;
	}
	else
	{
		PgStat_EntryRef *entry_ref PG_USED_FOR_ASSERTS_ONLY;

		entry_ref = cache_entry->entry_ref;
		Assert(entry_ref->shared_entry != NULL);
		Assert(entry_ref->shared_stats != NULL);

		Assert(entry_ref->shared_stats->magic == 0xdeadbeef);
		/* 至少应持有我们的引用 */
		Assert(pg_atomic_read_u32(&entry_ref->shared_entry->refcount) > 0);
	}

	*entry_ref_p = cache_entry->entry_ref;
	return found;
}

/*
 * 获取共享统计引用。若 create 为真，则共享统计对象
 * 不存在时创建它。
 *
 * 当 create 为真且 created_entry 非 NULL 时，若条目是新
 * 创建的，就设为真，否则为假。
 */
PgStat_EntryRef *
pgstat_get_entry_ref(PgStat_Kind kind, Oid dboid, Oid objoid, bool create,
					 bool *created_entry)
{
	PgStat_HashKey key;
	PgStatShared_HashEntry *shhashent;
	PgStatShared_Common *shheader = NULL;
	PgStat_EntryRef *entry_ref;

	/* 清零填充 */
	memset(&key, 0, sizeof(struct PgStat_HashKey));

	key.kind = kind;
	key.dboid = dboid;
	key.objoid = objoid;

	/*
	 * 仅当我们可能创建条目时传入 created_entry 才有意义
	 * 
	 */
	Assert(create || created_entry == NULL);
	pgstat_assert_is_up();
	Assert(pgStatLocal.shared_hash != NULL);
	Assert(!pgStatLocal.shmem->is_shutdown);

	pgstat_setup_memcxt();
	pgstat_setup_shared_refs();

	if (created_entry != NULL)
		*created_entry = false;

	/*
	 * 检查是否有其他后端删除了因有人持有其引用而
	 * 无法删除的统计。若有，则检查本后端的引用。
	 * 这种情况预计不会经常发生。这项检查的位置
	 * 有点随机，但这是一条被相对频繁调用的路径，
	 * 较优。
	 */
	if (pgstat_need_entry_refs_gc())
		pgstat_gc_entry_refs();

	/*
	 * 先检查本地内存中的查找缓存哈希表。若在此
	 * 找到匹配，就能避免加锁 / 引发争用。
	 */
	if (pgstat_get_entry_ref_cached(key, &entry_ref))
		return entry_ref;

	Assert(entry_ref != NULL);

	/*
	 * 先在哈希表中做一次查找——条目很可能已经
	 * 存在，这样我们只需要共享锁。
	 */
	shhashent = dshash_find(pgStatLocal.shared_hash, &key, false);

	if (create && !shhashent)
	{
		bool		shfound;

		/*
		 * 有可能自上述查找以来，某个后端已经创建
		 * 了该条目。若是这样，则走与在
		 * dshash_find() 调用前已创建时相同的路径。
		 */
		shhashent = dshash_find_or_insert(pgStatLocal.shared_hash, &key, &shfound);
		if (!shfound)
		{
			shheader = pgstat_init_entry(kind, shhashent);
			if (shheader == NULL)
			{
				/*
				 * 新条目分配失败，请在放弃前清理共享
				 * 哈希表。
				 */
				dshash_delete_entry(pgStatLocal.shared_hash, shhashent);

				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory"),
						 errdetail("Failed while allocating entry %d/%u/%u.",
								   key.kind, key.dboid, key.objoid)));
			}
			pgstat_acquire_entry_ref(entry_ref, shhashent, shheader);

			if (created_entry != NULL)
				*created_entry = true;

			return entry_ref;
		}
	}

	if (!shhashent)
	{
		/*
		 * 如果不是创建，则再次删除引用。极有可能
		 * 这只是一次统计查找——为指向虚无的共享引
		 * 用浪费内存……
		 */
		pgstat_release_entry_ref(key, entry_ref, false);

		return NULL;
	}
	else
	{
		/*
		 * 到达此处，要么是 dshash_find() 匹配到，要么是
		 * dshash_find_or_insert() 发现了并发插入的条目。
		 */

		if (shhashent->dropped && create)
		{
			/*
			 * 存在一些正当情形，旧的统计条目在被复用时
			 * 可能尚未被删除。最明显的例子是复制槽统
			 * 计，其中的新槽在删除后马上就能用同一
			 * 索引创建。但 oid 回绕也能导致其他情形。
			 * 我们只需将统计重置为其初始状态，同时递增
			 * 共享条目中的"generation"，供任何残留的
			 * 本地引用使用。
			 */
			shheader = pgstat_reinit_entry(kind, shhashent);
			pgstat_acquire_entry_ref(entry_ref, shhashent, shheader);

			if (created_entry != NULL)
				*created_entry = true;

			return entry_ref;
		}
		else if (shhashent->dropped)
		{
			dshash_release_lock(pgStatLocal.shared_hash, shhashent);
			pgstat_release_entry_ref(key, entry_ref, false);

			return NULL;
		}
		else
		{
			shheader = dsa_get_address(pgStatLocal.dsa, shhashent->body);
			pgstat_acquire_entry_ref(entry_ref, shhashent, shheader);

			return entry_ref;
		}
	}
}

static void
pgstat_release_entry_ref(PgStat_HashKey key, PgStat_EntryRef *entry_ref,
						 bool discard_pending)
{
	if (entry_ref && entry_ref->pending)
	{
		if (discard_pending)
			pgstat_delete_pending_entry(entry_ref);
		else
			elog(ERROR, "releasing ref with pending data");
	}

	if (entry_ref && entry_ref->shared_stats)
	{
		Assert(entry_ref->shared_stats->magic == 0xdeadbeef);
		Assert(entry_ref->pending == NULL);

		/*
		 * 这不会与另一个查找该统计条目并递增引用计
		 * 数的后端竞态，因为创建对已删除条目的
		 * 更多引用并不"合法"。
		 */
		if (pg_atomic_fetch_sub_u32(&entry_ref->shared_entry->refcount, 1) == 1)
		{
			PgStatShared_HashEntry *shent;

			/*
			 * 我们是此条目最后一个引用者，可尝试删除
			 * 它。
			 */

			/* 仅已删除条目能达到 0 引用计数 */
			Assert(entry_ref->shared_entry->dropped);

			shent = dshash_find(pgStatLocal.shared_hash,
								&entry_ref->shared_entry->key,
								true);
			if (!shent)
				elog(ERROR, "could not find just referenced shared stats entry");

			/*
			 * 此条目在尝试释放期间可能已被重新初始化，
			 * 因此请仔细确认它在持有其共享条目锁期间未
			 * 被复用。
			 */
			if (pg_atomic_read_u32(&entry_ref->shared_entry->generation) ==
				entry_ref->generation)
			{
				/* "generation"相同，可接受此次移除 */
				Assert(pg_atomic_read_u32(&entry_ref->shared_entry->refcount) == 0);
				Assert(entry_ref->shared_entry == shent);
				pgstat_free_entry(shent, NULL);
			}
			else
			{
				/*
				 * 共享统计条目已被重新初始化，请勿删除
				 * 其共享条目，只释放其锁。
				 */
				dshash_release_lock(pgStatLocal.shared_hash, shent);
			}
		}
	}

	if (!pgstat_entry_ref_hash_delete(pgStatEntryRefHash, key))
		elog(ERROR, "entry ref vanished before deletion");

	if (entry_ref)
		pfree(entry_ref);
}

bool
pgstat_lock_entry(PgStat_EntryRef *entry_ref, bool nowait)
{
	LWLock	   *lock = &entry_ref->shared_stats->lock;

	if (nowait)
		return LWLockConditionalAcquire(lock, LW_EXCLUSIVE);

	LWLockAcquire(lock, LW_EXCLUSIVE);
	return true;
}

/*
 * 与 pgstat_lock_entry() 分开，因为大多数调用方需要
 * 独占锁。
 */
bool
pgstat_lock_entry_shared(PgStat_EntryRef *entry_ref, bool nowait)
{
	LWLock	   *lock = &entry_ref->shared_stats->lock;

	if (nowait)
		return LWLockConditionalAcquire(lock, LW_SHARED);

	LWLockAcquire(lock, LW_SHARED);
	return true;
}

void
pgstat_unlock_entry(PgStat_EntryRef *entry_ref)
{
	LWLockRelease(&entry_ref->shared_stats->lock);
}

/*
 * 获取并锁定共享统计的辅助函数。
 */
PgStat_EntryRef *
pgstat_get_entry_ref_locked(PgStat_Kind kind, Oid dboid, Oid objoid,
							bool nowait)
{
	PgStat_EntryRef *entry_ref;

	/* 查找与本地条目对应的共享哈希表统计条目 */
	entry_ref = pgstat_get_entry_ref(kind, dboid, objoid, true, NULL);

	/* 锁定共享条目以保护其内容，失败则跳过 */
	if (!pgstat_lock_entry(entry_ref, nowait))
		return NULL;

	return entry_ref;
}

void
pgstat_request_entry_refs_gc(void)
{
	pg_atomic_fetch_add_u64(&pgStatLocal.shmem->gc_request_count, 1);
}

static bool
pgstat_need_entry_refs_gc(void)
{
	uint64		curage;

	if (!pgStatEntryRefHash)
		return false;

	/* 在创建 pgStatEntryRefHash 时就应已初始化 */
	Assert(pgStatSharedRefAge != 0);

	curage = pg_atomic_read_u64(&pgStatLocal.shmem->gc_request_count);

	return pgStatSharedRefAge != curage;
}

static void
pgstat_gc_entry_refs(void)
{
	pgstat_entry_ref_hash_iterator i;
	PgStat_EntryRefHashEntry *ent;
	uint64		curage;

	curage = pg_atomic_read_u64(&pgStatLocal.shmem->gc_request_count);
	Assert(curage != 0);

	/*
	 * 某些条目已被删除或重新初始化。请使其缓存
	 * 指针失效。
	 */
	pgstat_entry_ref_hash_start_iterate(pgStatEntryRefHash, &i);
	while ((ent = pgstat_entry_ref_hash_iterate(pgStatEntryRefHash, &i)) != NULL)
	{
		PgStat_EntryRef *entry_ref = ent->entry_ref;

		Assert(!entry_ref->shared_stats ||
			   entry_ref->shared_stats->magic == 0xdeadbeef);

		/*
		 * "generation" 检查条目被重新初始化的情形，而
		 * "dropped" 则对应这些条目被删除的情形。
		 */
		if (!entry_ref->shared_entry->dropped &&
			pg_atomic_read_u32(&entry_ref->shared_entry->generation) ==
			entry_ref->generation)
			continue;

		/* 有待写入数据的共享引用无法 gc */
		if (entry_ref->pending != NULL)
			continue;

		pgstat_release_entry_ref(ent->key, entry_ref, false);
	}

	pgStatSharedRefAge = curage;
}

static void
pgstat_release_matching_entry_refs(bool discard_pending, ReleaseMatchCB match,
								   Datum match_data)
{
	pgstat_entry_ref_hash_iterator i;
	PgStat_EntryRefHashEntry *ent;

	if (pgStatEntryRefHash == NULL)
		return;

	pgstat_entry_ref_hash_start_iterate(pgStatEntryRefHash, &i);

	while ((ent = pgstat_entry_ref_hash_iterate(pgStatEntryRefHash, &i))
		   != NULL)
	{
		Assert(ent->entry_ref != NULL);

		if (match && !match(ent, match_data))
			continue;

		pgstat_release_entry_ref(ent->key, ent->entry_ref, discard_pending);
	}
}

/*
 * 释放对共享统计条目的所有本地引用。
 *
 * 进程退出时无法在仍持有统计条目引用的情况
 * 下退出，否则共享统计条目将永远无法释放。
 */
static void
pgstat_release_all_entry_refs(bool discard_pending)
{
	if (pgStatEntryRefHash == NULL)
		return;

	pgstat_release_matching_entry_refs(discard_pending, NULL, 0);
	Assert(pgStatEntryRefHash->members == 0);
	pgstat_entry_ref_hash_destroy(pgStatEntryRefHash);
	pgStatEntryRefHash = NULL;
}

static bool
match_db(PgStat_EntryRefHashEntry *ent, Datum match_data)
{
	Oid			dboid = DatumGetObjectId(match_data);

	return ent->key.dboid == dboid;
}

static void
pgstat_release_db_entry_refs(Oid dboid)
{
	pgstat_release_matching_entry_refs( /* 丢弃待写入 = */ true,
									   match_db,
									   ObjectIdGetDatum(dboid));
}


/* ------------------------------------------------------------
 * 统计条目的删除与重置
 * ------------------------------------------------------------
 */

static void
pgstat_free_entry(PgStatShared_HashEntry *shent, dshash_seq_status *hstat)
{
	dsa_pointer pdsa;

	/*
	 * 在删除条目前先取 dsa 指针——这样就能在
	 * 释放锁之后释放内存。
	 */
	pdsa = shent->body;

	if (!hstat)
		dshash_delete_entry(pgStatLocal.shared_hash, shent);
	else
		dshash_delete_current(hstat);

	dsa_free(pgStatLocal.dsa, pdsa);
}

/*
 * pgstat_drop_database_and_contents() 和
 * pgstat_drop_entry() 二者共用的辅助函数。若 hstat
 * 非空则用 dshash_delete_current() 删除共享条目，否则用
 * dshash_delete_entry()。需先加锁。
 */
static bool
pgstat_drop_entry_internal(PgStatShared_HashEntry *shent,
						   dshash_seq_status *hstat)
{
	Assert(shent->body != InvalidDsaPointer);

	/* 此时应已释放本地引用 */
	if (pgStatEntryRefHash)
		Assert(!pgstat_entry_ref_hash_lookup(pgStatEntryRefHash, shent->key));

	/*
	 * 发出条目已被删除的信号——这最终会让其他
	 * 后端释放各自的引用。
	 */
	if (shent->dropped)
		elog(ERROR,
			 "trying to drop stats entry already dropped: kind=%s dboid=%u objoid=%u refcount=%u generation=%u",
			 pgstat_get_kind_info(shent->key.kind)->name,
			 shent->key.dboid, shent->key.objoid,
			 pg_atomic_read_u32(&shent->refcount),
			 pg_atomic_read_u32(&shent->generation));
	shent->dropped = true;

	/* 释放标记条目为未删除的引用计数 */
	if (pg_atomic_sub_fetch_u32(&shent->refcount, 1) == 0)
	{
		pgstat_free_entry(shent, hstat);
		return true;
	}
	else
	{
		if (!hstat)
			dshash_release_lock(pgStatLocal.shared_hash, shent);
		return false;
	}
}

/*
 * 删除该数据库及其内部所有对象的统计。
 */
static void
pgstat_drop_database_and_contents(Oid dboid)
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;
	uint64		not_freed_count = 0;

	Assert(OidIsValid(dboid));

	Assert(pgStatLocal.shared_hash != NULL);

	/*
	 * 本后端很可能就是唯一持有对这些待删除条目
	 * 的引用的后端。请确保我们没有把它推迟到
	 * 以后再清理。
	 *
	 * 将这一步与下面的 dshash 迭代分开来做，避免
	 * 在持有共享哈希表分区锁的情况下执行。
	 */
	pgstat_release_db_entry_refs(dboid);

	/* 部分 dshash 条目将被移除，取独占锁。 */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, true);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		if (p->dropped)
			continue;

		if (p->key.dboid != dboid)
			continue;

		if (!pgstat_drop_entry_internal(p, &hstat))
		{
			/*
			 * 即使是已删除数据库的统计，也可能正被
			 * 访问（例如 pg_stat_database 的数据库统计）。
			 */
			not_freed_count++;
		}
	}
	dshash_seq_term(&hstat);

	/*
	 * 如果部分统计数据无法释放，则发信号让引用
	 * 持有者对其缓存的 pgStatShmLookupCache 运行垃圾回收。
	 */
	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

/*
 * 删单个统计条目。
 *
 * 若被删除对象的统计条目无法释放，本例程返
 * 回假，否则返回真。
 *
 * 本函数的调用方应当在统计条目无法释放时调
 * 用 pgstat_request_entry_refs_gc()，以确保该条目的
 * 内存稍后能被调用 pgstat_gc_entry_refs()
 * 的后端回收。
 */
bool
pgstat_drop_entry(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	PgStat_HashKey key;
	PgStatShared_HashEntry *shent;
	bool		freed = true;

	/* 清零填充 */
	memset(&key, 0, sizeof(struct PgStat_HashKey));

	key.kind = kind;
	key.dboid = dboid;
	key.objoid = objoid;

	/* 删除本地引用 */
	if (pgStatEntryRefHash)
	{
		PgStat_EntryRefHashEntry *lohashent =
			pgstat_entry_ref_hash_lookup(pgStatEntryRefHash, key);

		if (lohashent)
			pgstat_release_entry_ref(lohashent->key, lohashent->entry_ref,
									 true);
	}

	/* 把共享哈希表条目标记为已删除，尽量删除 */
	shent = dshash_find(pgStatLocal.shared_hash, &key, true);
	if (shent)
	{
		freed = pgstat_drop_entry_internal(shent, NULL);

		/*
		 * 数据库统计包含其他统计。删除数据库时也
		 * 应一并删除它们。XXX：或许这应该用更有原则
		 * 的方式完成？但尚不清楚那会是什么样子，
		 * 而且至今这是唯一的情形……
		 */
		if (key.kind == PGSTAT_KIND_DATABASE)
			pgstat_drop_database_and_contents(key.dboid);
	}

	return freed;
}

void
pgstat_drop_all_entries(void)
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *ps;
	uint64		not_freed_count = 0;

	dshash_seq_init(&hstat, pgStatLocal.shared_hash, true);
	while ((ps = dshash_seq_next(&hstat)) != NULL)
	{
		if (ps->dropped)
			continue;

		if (!pgstat_drop_entry_internal(ps, &hstat))
			not_freed_count++;
	}
	dshash_seq_term(&hstat);

	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

static void
shared_stat_reset_contents(PgStat_Kind kind, PgStatShared_Common *header,
						   TimestampTz ts)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

	memset(pgstat_get_entry_data(kind, header), 0,
		   pgstat_get_entry_len(kind));

	if (kind_info->reset_timestamp_cb)
		kind_info->reset_timestamp_cb(header, ts);
}

/*
 * 重置单个可变编号统计条目。
 */
void
pgstat_reset_entry(PgStat_Kind kind, Oid dboid, Oid objoid, TimestampTz ts)
{
	PgStat_EntryRef *entry_ref;

	Assert(!pgstat_get_kind_info(kind)->fixed_amount);

	entry_ref = pgstat_get_entry_ref(kind, dboid, objoid, false, NULL);
	if (!entry_ref || entry_ref->shared_entry->dropped)
		return;

	(void) pgstat_lock_entry(entry_ref, false);
	shared_stat_reset_contents(kind, entry_ref->shared_stats, ts);
	pgstat_unlock_entry(entry_ref);
}

/*
 * 遍历统计的共享哈希表，若提供的 do_reset()
 * 函数认可，则重置统计。
 */
void
pgstat_reset_matching_entries(bool (*do_reset) (PgStatShared_HashEntry *, Datum),
							  Datum match_data, TimestampTz ts)
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;

	/* dshash 条目未被修改，取共享锁 */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		PgStatShared_Common *header;

		if (p->dropped)
			continue;

		if (!do_reset(p, match_data))
			continue;

		header = dsa_get_address(pgStatLocal.dsa, p->body);

		LWLockAcquire(&header->lock, LW_EXCLUSIVE);

		shared_stat_reset_contents(p->key.kind, header, ts);

		LWLockRelease(&header->lock);
	}
	dshash_seq_term(&hstat);
}

static bool
match_kind(PgStatShared_HashEntry *p, Datum match_data)
{
	return p->key.kind == DatumGetInt32(match_data);
}

void
pgstat_reset_entries_of_kind(PgStat_Kind kind, TimestampTz ts)
{
	pgstat_reset_matching_entries(match_kind, Int32GetDatum(kind), ts);
}

static void
pgstat_setup_memcxt(void)
{
	if (unlikely(!pgStatSharedRefContext))
		pgStatSharedRefContext =
			AllocSetContextCreate(TopMemoryContext,
								  "PgStat Shared Ref",
								  ALLOCSET_SMALL_SIZES);
	if (unlikely(!pgStatEntryRefHashContext))
		pgStatEntryRefHashContext =
			AllocSetContextCreate(TopMemoryContext,
								  "PgStat Shared Ref Hash",
								  ALLOCSET_SMALL_SIZES);
}
