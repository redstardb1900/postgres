/*
 * simplehash.h
 *
 *	  When included this file generates a "templated" (by way of macros)
 *	  open-addressing hash table implementation specialized to user-defined
 *	  types.
 *
 *	  It's probably not worthwhile to generate such a specialized implementation
 *	  for hash tables that aren't performance or space sensitive.
 *
 *	  Compared to dynahash, simplehash has the following benefits:
 *
 *	  - Due to the "templated" code generation has known structure sizes and no
 *	    indirect function calls (which show up substantially in dynahash
 *	    profiles). These features considerably increase speed for small
 *	    entries.
 *	  - Open addressing has better CPU cache behavior than dynahash's chained
 *	    hashtables.
 *	  - The generated interface is type-safe and easier to use than dynahash,
 *	    though at the cost of more complex setup.
 *	  - Allocates memory in a MemoryContext or another allocator with a
 *	    malloc/free style interface (which isn't easily usable in a shared
 *	    memory context)
 *	  - Does not require the overhead of a separate memory context.
 *
 * Usage notes:
 *
 *	  To generate a hash-table and associated functions for a use case several
 *	  macros have to be #define'ed before this file is included.  Including
 *	  the file #undef's all those, so a new hash table can be generated
 *	  afterwards.
 *	  The relevant parameters are:
 *	  - SH_PREFIX - prefix for all symbol names generated. A prefix of 'foo'
 *		will result in hash table type 'foo_hash' and functions like
 *		'foo_insert'/'foo_lookup' and so forth.
 *	  - SH_ELEMENT_TYPE - type of the contained elements
 *	  - SH_KEY_TYPE - type of the hashtable's key
 *	  - SH_DECLARE - if defined function prototypes and type declarations are
 *		generated
 *	  - SH_DEFINE - if defined function definitions are generated
 *	  - SH_SCOPE - in which scope (e.g. extern, static inline) do function
 *		declarations reside
 *	  - SH_RAW_ALLOCATOR - if defined, memory contexts are not used; instead,
 *	    use this to allocate bytes. The allocator must zero the returned space.
 *	  - SH_USE_NONDEFAULT_ALLOCATOR - if defined no element allocator functions
 *		are defined, so you can supply your own
 *	  The following parameters are only relevant when SH_DEFINE is defined:
 *	  - SH_KEY - name of the element in SH_ELEMENT_TYPE containing the hash key
 *	  - SH_EQUAL(table, a, b) - compare two table keys
 *	  - SH_HASH_KEY(table, key) - generate hash for the key
 *	  - SH_STORE_HASH - if defined the hash is stored in the elements
 *	  - SH_GET_HASH(tb, a) - return the field to store the hash in
 *
 *	  The element type is required to contain a "status" member that can store
 *	  the range of values defined in the SH_STATUS enum.
 *
 *	  While SH_STORE_HASH (and subsequently SH_GET_HASH) are optional, because
 *	  the hash table implementation needs to compare hashes to move elements
 *	  (particularly when growing the hash), it's preferable, if possible, to
 *	  store the element's hash in the element's data type. If the hash is so
 *	  stored, the hash table will also compare hashes before calling SH_EQUAL
 *	  when comparing two keys.
 *
 *	  For convenience the hash table create functions accept a void pointer
 *	  that will be stored in the hash table type's member private_data. This
 *	  allows callbacks to reference caller provided data.
 *
 *	  For examples of usage look at tidbitmap.c (file local definition) and
 *	  execnodes.h/execGrouping.c (exposed declaration, file local
 *	  implementation).
 *
 * Hash table design:
 *
 *	  The hash table design chosen is a variant of linear open-addressing. The
 *	  reason for doing so is that linear addressing is CPU cache & pipeline
 *	  friendly. The biggest disadvantage of simple linear addressing schemes
 *	  are highly variable lookup times due to clustering, and deletions
 *	  leaving a lot of tombstones around.  To address these issues a variant
 *	  of "robin hood" hashing is employed.  Robin hood hashing optimizes
 *	  chaining lengths by moving elements close to their optimal bucket
 *	  ("rich" elements), out of the way if a to-be-inserted element is further
 *	  away from its optimal position (i.e. it's "poor").  While that can make
 *	  insertions slower, the average lookup performance is a lot better, and
 *	  higher fill factors can be used in a still performant manner.  To avoid
 *	  tombstones - which normally solve the issue that a deleted node's
 *	  presence is relevant to determine whether a lookup needs to continue
 *	  looking or is done - buckets following a deleted element are shifted
 *	  backwards, unless they're empty or already at their optimal position.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/lib/simplehash.h
 */

#include "port/pg_bitutils.h"

/* 辅助 */
#define SH_MAKE_PREFIX(a) CppConcat(a,_)
#define SH_MAKE_NAME(name) SH_MAKE_NAME_(SH_MAKE_PREFIX(SH_PREFIX),name)
#define SH_MAKE_NAME_(a,b) CppConcat(a,b)

/* 下列对象的名字宏： */

/* 类型声明 */
#define SH_TYPE SH_MAKE_NAME(hash)
#define SH_STATUS SH_MAKE_NAME(status)
#define SH_STATUS_EMPTY SH_MAKE_NAME(SH_EMPTY)
#define SH_STATUS_IN_USE SH_MAKE_NAME(SH_IN_USE)
#define SH_ITERATOR SH_MAKE_NAME(iterator)

/* 函数声明 */
#define SH_CREATE SH_MAKE_NAME(create)
#define SH_DESTROY SH_MAKE_NAME(destroy)
#define SH_RESET SH_MAKE_NAME(reset)
#define SH_INSERT SH_MAKE_NAME(insert)
#define SH_INSERT_HASH SH_MAKE_NAME(insert_hash)
#define SH_DELETE_ITEM SH_MAKE_NAME(delete_item)
#define SH_DELETE SH_MAKE_NAME(delete)
#define SH_LOOKUP SH_MAKE_NAME(lookup)
#define SH_LOOKUP_HASH SH_MAKE_NAME(lookup_hash)
#define SH_GROW SH_MAKE_NAME(grow)
#define SH_START_ITERATE SH_MAKE_NAME(start_iterate)
#define SH_START_ITERATE_AT SH_MAKE_NAME(start_iterate_at)
#define SH_ITERATE SH_MAKE_NAME(iterate)
#define SH_ALLOCATE SH_MAKE_NAME(allocate)
#define SH_FREE SH_MAKE_NAME(free)
#define SH_STAT SH_MAKE_NAME(stat)

/* 内部辅助函数（没有外部可见的原型） */
#define SH_COMPUTE_PARAMETERS SH_MAKE_NAME(compute_parameters)
#define SH_NEXT SH_MAKE_NAME(next)
#define SH_PREV SH_MAKE_NAME(prev)
#define SH_DISTANCE_FROM_OPTIMAL SH_MAKE_NAME(distance)
#define SH_INITIAL_BUCKET SH_MAKE_NAME(initial_bucket)
#define SH_ENTRY_HASH SH_MAKE_NAME(entry_hash)
#define SH_INSERT_HASH_INTERNAL SH_MAKE_NAME(insert_hash_internal)
#define SH_LOOKUP_HASH_INTERNAL SH_MAKE_NAME(lookup_hash_internal)

/* 生成使用该哈希表所需的前向声明 */
#ifdef SH_DECLARE

/* 类型定义 */
typedef struct SH_TYPE
{
	/*
 * 数据 / 桶数组的大小，用 64 位以便处理 UINT32_MAX
 * 大小的哈希表。注意元素个数上限更低
 * （SH_MAX_FILLFACTOR）。
 */
	uint64		size;

	/* 有多少元素具有有效内容 */
	uint32		members;

	/* 基于 size 的桶与大小计算掩码 */
	uint32		sizemask;

	/* 超过此界限后增长哈希表 */
	uint32		grow_threshold;

	/* 哈希桶 */
	SH_ELEMENT_TYPE *data;

#ifndef SH_RAW_ALLOCATOR
	/* 用于分配的内存上下文 */
	MemoryContext ctx;
#endif

	/* 用户定义的数据，供回调使用 */
	void	   *private_data;
}			SH_TYPE;

typedef enum SH_STATUS
{
	SH_STATUS_EMPTY = 0x00,
	SH_STATUS_IN_USE = 0x01
} SH_STATUS;

typedef struct SH_ITERATOR
{
	uint32		cur;			/* 当前元素 */
	uint32		end;
	bool		done;			/* 迭代器是否已耗尽？ */
}			SH_ITERATOR;

/* 外部可见的函数原型 */
#ifdef SH_RAW_ALLOCATOR
/* <prefix>_hash <prefix>_create(uint32 nelements, void *private_data) */
SH_SCOPE	SH_TYPE *SH_CREATE(uint32 nelements, void *private_data);
#else
/*
 * <prefix>_hash <prefix>_create(MemoryContext ctx, uint32 nelements,
 *								 void *private_data)
 */
SH_SCOPE	SH_TYPE *SH_CREATE(MemoryContext ctx, uint32 nelements,
							   void *private_data);
#endif

/* void <prefix>_destroy(<prefix>_hash *tb) */
SH_SCOPE void SH_DESTROY(SH_TYPE * tb);

/* void <prefix>_reset(<prefix>_hash *tb) */
SH_SCOPE void SH_RESET(SH_TYPE * tb);

/* void <prefix>_grow(<prefix>_hash *tb, uint64 newsize) */
SH_SCOPE void SH_GROW(SH_TYPE * tb, uint64 newsize);

/* <element> *<prefix>_insert(<prefix>_hash *tb, <key> key, bool *found) */
SH_SCOPE	SH_ELEMENT_TYPE *SH_INSERT(SH_TYPE * tb, SH_KEY_TYPE key, bool *found);

/*
 * <element> *<prefix>_insert_hash(<prefix>_hash *tb, <key> key, uint32 hash,
 * 								  bool *found)
 */
SH_SCOPE	SH_ELEMENT_TYPE *SH_INSERT_HASH(SH_TYPE * tb, SH_KEY_TYPE key,
											uint32 hash, bool *found);

/* <element> *<prefix>_lookup(<prefix>_hash *tb, <key> key) */
SH_SCOPE	SH_ELEMENT_TYPE *SH_LOOKUP(SH_TYPE * tb, SH_KEY_TYPE key);

/* <element> *<prefix>_lookup_hash(<prefix>_hash *tb, <key> key, uint32 hash) */
SH_SCOPE	SH_ELEMENT_TYPE *SH_LOOKUP_HASH(SH_TYPE * tb, SH_KEY_TYPE key,
											uint32 hash);

/* void <prefix>_delete_item(<prefix>_hash *tb, <element> *entry) */
SH_SCOPE void SH_DELETE_ITEM(SH_TYPE * tb, SH_ELEMENT_TYPE * entry);

/* bool <prefix>_delete(<prefix>_hash *tb, <key> key) */
SH_SCOPE bool SH_DELETE(SH_TYPE * tb, SH_KEY_TYPE key);

/* void <prefix>_start_iterate(<prefix>_hash *tb, <prefix>_iterator *iter) */
SH_SCOPE void SH_START_ITERATE(SH_TYPE * tb, SH_ITERATOR * iter);

/*
 * void <prefix>_start_iterate_at(<prefix>_hash *tb, <prefix>_iterator *iter,
 *								  uint32 at)
 */
SH_SCOPE void SH_START_ITERATE_AT(SH_TYPE * tb, SH_ITERATOR * iter, uint32 at);

/* <element> *<prefix>_iterate(<prefix>_hash *tb, <prefix>_iterator *iter) */
SH_SCOPE	SH_ELEMENT_TYPE *SH_ITERATE(SH_TYPE * tb, SH_ITERATOR * iter);

/* void <prefix>_stat(<prefix>_hash *tb */
SH_SCOPE void SH_STAT(SH_TYPE * tb);

#endif							/* SH_DECLARE */


/* 生成哈希表的实现 */
#ifdef SH_DEFINE

#ifndef SH_RAW_ALLOCATOR
#include "utils/memutils.h"
#endif

/* 数据数组最大尺寸，最多允许 PG_UINT32_MAX 个桶，含 0 */
#define SH_MAX_SIZE (((uint64) PG_UINT32_MAX) + 1)

/* 正常填充因子，除非已经接近上限 */
#ifndef SH_FILLFACTOR
#define SH_FILLFACTOR (0.9)
#endif
/* 若否则会报错，则提高填充因子 */
#define SH_MAX_FILLFACTOR (0.98)
/* 若实际位置与最优位置之差大于此值则增长 */
#ifndef SH_GROW_MAX_DIB
#define SH_GROW_MAX_DIB 25
#endif
/* 若插入时要移动的元素多于此值则增长 */
#ifndef SH_GROW_MAX_MOVE
#define SH_GROW_MAX_MOVE 150
#endif
#ifndef SH_GROW_MIN_FILLFACTOR
/* 但若低于此值，则不因 SH_GROW_MAX_* 而增长 */
#define SH_GROW_MIN_FILLFACTOR 0.1
#endif

#ifdef SH_STORE_HASH
#define SH_COMPARE_KEYS(tb, ahash, akey, b) (ahash == SH_GET_HASH(tb, b) && SH_EQUAL(tb, b->SH_KEY, akey))
#else
#define SH_COMPARE_KEYS(tb, ahash, akey, b) (SH_EQUAL(tb, b->SH_KEY, akey))
#endif

/*
 * 把随后的定义包进包含守卫，以免本头文件被包含
 * 多次时出现重复定义错误。文件其余部分故意没有
 * 包含守卫，因为它可以用不同参数被包含，以定义
 * 名字互不冲突的函数和类型。
 *
 */
#ifndef SIMPLEHASH_H
#define SIMPLEHASH_H

#ifdef FRONTEND
#define sh_error(...) pg_fatal(__VA_ARGS__)
#define sh_log(...) pg_log_info(__VA_ARGS__)
#else
#define sh_error(...) elog(ERROR, __VA_ARGS__)
#define sh_log(...) elog(LOG, __VA_ARGS__)
#endif

#endif

/*
 * 计算哈希表的尺寸参数。创建和增长哈希表时调用。
 *
 */
static inline void
SH_COMPUTE_PARAMETERS(SH_TYPE * tb, uint64 newsize)
{
	uint64		size;

	/* 支持大小为零的哈希会使事情变复杂 */
	size = Max(newsize, 2);

	/* 把大小向上取整到下一个 2 的幂，分桶就是这样工作的 */
	size = pg_nextpower2_64(size);
	Assert(size <= SH_MAX_SIZE);

	/*
 * 验证在本平台上分配 ->data 是可能的，且不会
 * 使 Size 溢出。
 */
	if (unlikely((((uint64) sizeof(SH_ELEMENT_TYPE)) * size) >= SIZE_MAX / 2))
		sh_error("hash table too large");

	/* 现在设置大小 */
	tb->size = size;
	tb->sizemask = (uint32) (size - 1);

	/*
 * 计算下一次需要再次增长哈希表的阈值。
 *
 */
	if (tb->size == SH_MAX_SIZE)
		tb->grow_threshold = ((double) tb->size) * SH_MAX_FILLFACTOR;
	else
		tb->grow_threshold = ((double) tb->size) * SH_FILLFACTOR;
}

/* 返回该哈希的最优桶 */
static inline uint32
SH_INITIAL_BUCKET(SH_TYPE * tb, uint32 hash)
{
	return hash & tb->sizemask;
}

/* 返回当前桶之后的下一个桶，并处理回绕 */
static inline uint32
SH_NEXT(SH_TYPE * tb, uint32 curelem, uint32 startelem)
{
	curelem = (curelem + 1) & tb->sizemask;

	Assert(curelem != startelem);

	return curelem;
}

/* 返回当前桶之前的上一个桶，并处理回绕 */
static inline uint32
SH_PREV(SH_TYPE * tb, uint32 curelem, uint32 startelem)
{
	curelem = (curelem - 1) & tb->sizemask;

	Assert(curelem != startelem);

	return curelem;
}

/* 返回桶与其最优位置之间的距离 */
static inline uint32
SH_DISTANCE_FROM_OPTIMAL(SH_TYPE * tb, uint32 optimal, uint32 bucket)
{
	if (optimal <= bucket)
		return bucket - optimal;
	else
		return (tb->size + bucket) - optimal;
}

static inline uint32
SH_ENTRY_HASH(SH_TYPE * tb, SH_ELEMENT_TYPE * entry)
{
#ifdef SH_STORE_HASH
	return SH_GET_HASH(tb, entry);
#else
	return SH_HASH_KEY(tb, entry->SH_KEY);
#endif
}

/* 默认的内存分配函数 */
static inline void *SH_ALLOCATE(SH_TYPE * type, Size size);
static inline void SH_FREE(SH_TYPE * type, void *pointer);

#ifndef SH_USE_NONDEFAULT_ALLOCATOR

/* 默认的内存分配函数 */
static inline void *
SH_ALLOCATE(SH_TYPE * type, Size size)
{
#ifdef SH_RAW_ALLOCATOR
	return SH_RAW_ALLOCATOR(size);
#else
	return MemoryContextAllocExtended(type->ctx, size,
									  MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
#endif
}

/* 默认的内存释放函数 */
static inline void
SH_FREE(SH_TYPE * type, void *pointer)
{
	pfree(pointer);
}

#endif

/*
 * 创建一张有足够空间存放 `nelements` 个不同成员的哈希表。
 * 哈希表的内存从传入的上下文分配。如果需要，元素数组
 * 可以用传入的分配器来分配；这有助于把元素数组放进
 * 共享内存，或放进比哈希表其余部分活得更久的上下文。
 * 元素数组以外的内存仍从传入的上下文分配。
 *
 *
 */
#ifdef SH_RAW_ALLOCATOR
SH_SCOPE	SH_TYPE *
SH_CREATE(uint32 nelements, void *private_data)
#else
SH_SCOPE	SH_TYPE *
SH_CREATE(MemoryContext ctx, uint32 nelements, void *private_data)
#endif
{
	SH_TYPE    *tb;
	uint64		size;

#ifdef SH_RAW_ALLOCATOR
	tb = (SH_TYPE *) SH_RAW_ALLOCATOR(sizeof(SH_TYPE));
#else
	tb = (SH_TYPE *) MemoryContextAllocZero(ctx, sizeof(SH_TYPE));
	tb->ctx = ctx;
#endif
	tb->private_data = private_data;

	/* 按填充因子放大 nelements，希望存下 nelements 个元素 */
	size = Min((double) SH_MAX_SIZE, ((double) nelements) / SH_FILLFACTOR);

	SH_COMPUTE_PARAMETERS(tb, size);

	tb->data = (SH_ELEMENT_TYPE *) SH_ALLOCATE(tb, sizeof(SH_ELEMENT_TYPE) * tb->size);

	return tb;
}

/* 销毁先前创建的哈希表 */
SH_SCOPE void
SH_DESTROY(SH_TYPE * tb)
{
	SH_FREE(tb, tb->data);
	pfree(tb);
}

/* 重置先前创建的哈希表的内容 */
SH_SCOPE void
SH_RESET(SH_TYPE * tb)
{
	memset(tb->data, 0, sizeof(SH_ELEMENT_TYPE) * tb->size);
	tb->members = 0;
}

/*
 * 把哈希表增长到至少 `newsize` 个桶。
 *
 * 通常这会在必要时由插入或删除自动调用。但若在某个
 * 时刻已知精确的输入大小，调到恰好那个大小在性能上
 * 可能更有利。
 */
SH_SCOPE void
SH_GROW(SH_TYPE * tb, uint64 newsize)
{
	uint64		oldsize = tb->size;
	SH_ELEMENT_TYPE *olddata = tb->data;
	SH_ELEMENT_TYPE *newdata;
	uint32		i;
	uint32		startelem = 0;
	uint32		copyelem;

	Assert(oldsize == pg_nextpower2_64(oldsize));
	Assert(oldsize != SH_MAX_SIZE);
	Assert(oldsize < newsize);

	/* 计算新表的参数 */
	SH_COMPUTE_PARAMETERS(tb, newsize);

	tb->data = (SH_ELEMENT_TYPE *) SH_ALLOCATE(tb, sizeof(SH_ELEMENT_TYPE) * tb->size);

	newdata = tb->data;

	/*
 * 把条目从旧数据复制到 newdata。理论上可以用 SH_INSERT
 * 来避免代码重复，但那比我们需要的更通用。我们既不希望
 * tb->members 增加，也不需要处理已删除元素，也不需要
 * 比较键。因此专门实现要快得多。调整大小可能既耗时又
 * 频繁，所以值得优化。
 *
 * 为了能简单地搬移条目，起点不能是第一个桶（即 olddata[0]），
 * 而要找到第一个要么为空、要么被处在最优位置的条目占据
 * 的桶。装载因子小于 1 的任何表里都必定存在这样的桶，
 * 因为并非所有桶都被占用，也就是说始终必须有一个空桶。
 * 从这样的桶开始，就能把条目搬到更大的表中，而不必处理冲突。
 *
 *
 *
 */

	/* 寻找哈希中第一个没有发生回绕的元素 */
	for (i = 0; i < oldsize; i++)
	{
		SH_ELEMENT_TYPE *oldentry = &olddata[i];
		uint32		hash;
		uint32		optimal;

		if (oldentry->status != SH_STATUS_IN_USE)
		{
			startelem = i;
			break;
		}

		hash = SH_ENTRY_HASH(tb, oldentry);
		optimal = SH_INITIAL_BUCKET(tb, hash);

		if (optimal == i)
		{
			startelem = i;
			break;
		}
	}

	/* 并复制旧表中的全部元素 */
	copyelem = startelem;
	for (i = 0; i < oldsize; i++)
	{
		SH_ELEMENT_TYPE *oldentry = &olddata[copyelem];

		if (oldentry->status == SH_STATUS_IN_USE)
		{
			uint32		hash;
			uint32		startelem2;
			uint32		curelem;
			SH_ELEMENT_TYPE *newentry;

			hash = SH_ENTRY_HASH(tb, oldentry);
			startelem2 = SH_INITIAL_BUCKET(tb, hash);
			curelem = startelem2;

			/* 找一个空元素来放入数据 */
			while (true)
			{
				newentry = &newdata[curelem];

				if (newentry->status == SH_STATUS_EMPTY)
				{
					break;
				}

				curelem = SH_NEXT(tb, curelem, startelem2);
			}

			/* 把条目复制到新槽 */
			memcpy(newentry, oldentry, sizeof(SH_ELEMENT_TYPE));
		}

		/* 这里无法使用 SH_NEXT，否则会用到新的大小 */
		copyelem++;
		if (copyelem >= oldsize)
		{
			copyelem = 0;
		}
	}

	SH_FREE(tb, olddata);
}

/*
 * 这是单独的 static inline 函数，因此即使 SH_SCOPE 为 extern，
 * 也能可靠地内联进它的包装函数。
 */
static inline SH_ELEMENT_TYPE *
SH_INSERT_HASH_INTERNAL(SH_TYPE * tb, SH_KEY_TYPE key, uint32 hash, bool *found)
{
	uint32		startelem;
	uint32		curelem;
	SH_ELEMENT_TYPE *data;
	uint32		insertdist;

restart:
	insertdist = 0;

	/*
 * 即使键实际上已经存在，我们也做增长检查，以免把检查
 * 放进循环里。这也让我们在调整大小之后不必重新寻找
 * 自己在哈希表中的位置。
 *
 * 注意：因 SH_GROW_MAX_DIB / SH_GROW_MAX_MOVE 而调整
 * 表大小时也会走到这里。
 */
	if (unlikely(tb->members >= tb->grow_threshold))
	{
		if (unlikely(tb->size == SH_MAX_SIZE))
			sh_error("hash table size exceeded");

		/*
 * 做优化时，把这些打印出来会非常有用。
 */
		/* SH_STAT(tb); */
		SH_GROW(tb, tb->size * 2);
		/* SH_STAT(tb); */
	}

	/* 执行插入，从最优位置开始搜索桶 */
	data = tb->data;
	startelem = SH_INITIAL_BUCKET(tb, hash);
	curelem = startelem;
	while (true)
	{
		uint32		curdist;
		uint32		curhash;
		uint32		curoptimal;
		SH_ELEMENT_TYPE *entry = &data[curelem];

		/* 任何空桶都可以直接使用 */
		if (entry->status == SH_STATUS_EMPTY)
		{
			tb->members++;
			entry->SH_KEY = key;
#ifdef SH_STORE_HASH
			SH_GET_HASH(tb, entry) = hash;
#endif
			entry->status = SH_STATUS_IN_USE;
			*found = false;
			return entry;
		}

		/*
 * 若桶非空，要么找到了匹配（此时已完成），要么必须决定
 * 是跳过还是移动发生冲突的条目。当冲突元素到其最优位置
 * 的距离小于待插入条目的距离时，把冲突条目（及其后继）
 * 向前移动一格。
 *
 */

		if (SH_COMPARE_KEYS(tb, hash, key, entry))
		{
			Assert(entry->status == SH_STATUS_IN_USE);
			*found = true;
			return entry;
		}

		curhash = SH_ENTRY_HASH(tb, entry);
		curoptimal = SH_INITIAL_BUCKET(tb, curhash);
		curdist = SH_DISTANCE_FROM_OPTIMAL(tb, curoptimal, curelem);

		if (insertdist > curdist)
		{
			SH_ELEMENT_TYPE *lastentry = entry;
			uint32		emptyelem = curelem;
			uint32		moveelem;
			int32		emptydist = 0;

			/* 寻找下一个空桶 */
			while (true)
			{
				SH_ELEMENT_TYPE *emptyentry;

				emptyelem = SH_NEXT(tb, emptyelem, startelem);
				emptyentry = &data[emptyelem];

				if (emptyentry->status == SH_STATUS_EMPTY)
				{
					lastentry = emptyentry;
					break;
				}

				/*
 * 为避免严重不平衡的哈希表带来负面影响，若冲突会需要
 * 我们移动大量条目，就增长哈希表。这种不平衡最可能的
 * 原因，是按哈希表顺序把一张（当前）很大的表填进一张
 * （当前）很小的表。若哈希表会因此变得太空，则不要增长，
 * 以免某些古怪边界情况下空间迅速膨胀。
 *
 *
 */
				if (unlikely(++emptydist > SH_GROW_MAX_MOVE) &&
					((double) tb->members / tb->size) >= SH_GROW_MIN_FILLFACTOR)
				{
					tb->grow_threshold = 0;
					goto restart;
				}
			}

			/* 从最后一个已占用元素开始向前移位 */

			/*
 * TODO: 很多情况下这可以优化成一次 memcpy，
 * 但在 ->data 末尾发生回绕时除外。不过到目前为止
 * 它还没在性能剖析里显现出来。
 */
			moveelem = emptyelem;
			while (moveelem != curelem)
			{
				SH_ELEMENT_TYPE *moveentry;

				moveelem = SH_PREV(tb, moveelem, startelem);
				moveentry = &data[moveelem];

				memcpy(lastentry, moveentry, sizeof(SH_ELEMENT_TYPE));
				lastentry = moveentry;
			}

			/* 并填上现在空出的位置 */
			tb->members++;

			entry->SH_KEY = key;
#ifdef SH_STORE_HASH
			SH_GET_HASH(tb, entry) = hash;
#endif
			entry->status = SH_STATUS_IN_USE;
			*found = false;
			return entry;
		}

		curelem = SH_NEXT(tb, curelem, startelem);
		insertdist++;

		/*
 * 为避免严重不平衡的哈希表带来负面影响，若冲突导致
 * 很长的连续段，就增长哈希表。这种不平衡最可能的原因，
 * 是按哈希表顺序把一张（当前）很大的表填进一张（当前）
 * 很小的表。若哈希表会因此变得太空，则不要增长，以免
 * 某些古怪边界情况下空间迅速膨胀。
 *
 */
		if (unlikely(insertdist > SH_GROW_MAX_DIB) &&
			((double) tb->members / tb->size) >= SH_GROW_MIN_FILLFACTOR)
		{
			tb->grow_threshold = 0;
			goto restart;
		}
	}
}

/*
 * 把键 key 插入哈希表。若键已存在则把 *found 设为 true，
 * 否则设为 false。两种情况下都返回哈希表条目。
 *
 */
SH_SCOPE	SH_ELEMENT_TYPE *
SH_INSERT(SH_TYPE * tb, SH_KEY_TYPE key, bool *found)
{
	uint32		hash = SH_HASH_KEY(tb, key);

	return SH_INSERT_HASH_INTERNAL(tb, key, hash, found);
}

/*
 * 用已经算好的哈希把键 key 插入哈希表。若键已存在则把
 * *found 设为 true，否则设为 false。两种情况下都返回
 * 哈希表条目。
 */
SH_SCOPE	SH_ELEMENT_TYPE *
SH_INSERT_HASH(SH_TYPE * tb, SH_KEY_TYPE key, uint32 hash, bool *found)
{
	return SH_INSERT_HASH_INTERNAL(tb, key, hash, found);
}

/*
 * 这是单独的 static inline 函数，因此即使 SH_SCOPE 为 extern，
 * 也能可靠地内联进它的包装函数。
 */
static inline SH_ELEMENT_TYPE *
SH_LOOKUP_HASH_INTERNAL(SH_TYPE * tb, SH_KEY_TYPE key, uint32 hash)
{
	const uint32 startelem = SH_INITIAL_BUCKET(tb, hash);
	uint32		curelem = startelem;

	while (true)
	{
		SH_ELEMENT_TYPE *entry = &tb->data[curelem];

		if (entry->status == SH_STATUS_EMPTY)
		{
			return NULL;
		}

		Assert(entry->status == SH_STATUS_IN_USE);

		if (SH_COMPARE_KEYS(tb, hash, key, entry))
			return entry;

		/*
 * TODO: 可以按距离停止搜索。若当前桶相对最优位置的距离
 * 小于我们已经跳过的距离，则条目不存在。也许只在定义了
 * SH_STORE_HASH 时才这样做，以免重新计算哈希？
 *
 */

		curelem = SH_NEXT(tb, curelem, startelem);
	}
}

/*
 * 在哈希表中查找条目。若键不存在则返回 NULL。
 */
SH_SCOPE	SH_ELEMENT_TYPE *
SH_LOOKUP(SH_TYPE * tb, SH_KEY_TYPE key)
{
	uint32		hash = SH_HASH_KEY(tb, key);

	return SH_LOOKUP_HASH_INTERNAL(tb, key, hash);
}

/*
 * 用已经算好的哈希在哈希表中查找条目。
 *
 * 若键不存在则返回 NULL。
 */
SH_SCOPE	SH_ELEMENT_TYPE *
SH_LOOKUP_HASH(SH_TYPE * tb, SH_KEY_TYPE key, uint32 hash)
{
	return SH_LOOKUP_HASH_INTERNAL(tb, key, hash);
}

/*
 * 按键从哈希表中删除条目。返回待删除的键是否存在。
 *
 */
SH_SCOPE bool
SH_DELETE(SH_TYPE * tb, SH_KEY_TYPE key)
{
	uint32		hash = SH_HASH_KEY(tb, key);
	uint32		startelem = SH_INITIAL_BUCKET(tb, hash);
	uint32		curelem = startelem;

	while (true)
	{
		SH_ELEMENT_TYPE *entry = &tb->data[curelem];

		if (entry->status == SH_STATUS_EMPTY)
			return false;

		if (entry->status == SH_STATUS_IN_USE &&
			SH_COMPARE_KEYS(tb, hash, key, entry))
		{
			SH_ELEMENT_TYPE *lastentry = entry;

			tb->members--;

			/*
 * 向后移动后续元素，直到遇到空元素，或遇到处在
 * 其最优位置的元素。
 *
 * 这听起来昂贵，但平均链长很短，而且否则删除
 * 就需要墓碑标记。
 */
			while (true)
			{
				SH_ELEMENT_TYPE *curentry;
				uint32		curhash;
				uint32		curoptimal;

				curelem = SH_NEXT(tb, curelem, startelem);
				curentry = &tb->data[curelem];

				if (curentry->status != SH_STATUS_IN_USE)
				{
					lastentry->status = SH_STATUS_EMPTY;
					break;
				}

				curhash = SH_ENTRY_HASH(tb, curentry);
				curoptimal = SH_INITIAL_BUCKET(tb, curhash);

				/* 当前位置已是最优位置，完成 */
				if (curoptimal == curelem)
				{
					lastentry->status = SH_STATUS_EMPTY;
					break;
				}

				/* 移位 */
				memcpy(lastentry, curentry, sizeof(SH_ELEMENT_TYPE));

				lastentry = curentry;
			}

			return true;
		}

		/* TODO: 若距离太大则 return false; */

		curelem = SH_NEXT(tb, curelem, startelem);
	}
}

/*
 * 按条目指针从哈希表中删除条目。
 */
SH_SCOPE void
SH_DELETE_ITEM(SH_TYPE * tb, SH_ELEMENT_TYPE * entry)
{
	SH_ELEMENT_TYPE *lastentry = entry;
	uint32		hash = SH_ENTRY_HASH(tb, entry);
	uint32		startelem = SH_INITIAL_BUCKET(tb, hash);
	uint32		curelem;

	/* 计算 'entry' 的下标 */
	curelem = entry - &tb->data[0];

	tb->members--;

	/*
 * 向后移动后续元素，直到遇到空元素或遇到处在其
 * 最优位置的元素。
 *
 * 这听起来昂贵，但平均链长很短，而且否则删除
 * 就需要墓碑标记。
 */
	while (true)
	{
		SH_ELEMENT_TYPE *curentry;
		uint32		curhash;
		uint32		curoptimal;

		curelem = SH_NEXT(tb, curelem, startelem);
		curentry = &tb->data[curelem];

		if (curentry->status != SH_STATUS_IN_USE)
		{
			lastentry->status = SH_STATUS_EMPTY;
			break;
		}

		curhash = SH_ENTRY_HASH(tb, curentry);
		curoptimal = SH_INITIAL_BUCKET(tb, curhash);

		/* 当前位置已是最优位置，完成 */
		if (curoptimal == curelem)
		{
			lastentry->status = SH_STATUS_EMPTY;
			break;
		}

		/* 移位 */
		memcpy(lastentry, curentry, sizeof(SH_ELEMENT_TYPE));

		lastentry = curentry;
	}
}

/*
 * 初始化迭代器。
 */
SH_SCOPE void
SH_START_ITERATE(SH_TYPE * tb, SH_ITERATOR * iter)
{
	uint64		startelem = PG_UINT64_MAX;

	/*
 * 寻找第一个空元素。由于支持在迭代期间删除，我们希望
 * 从无法受元素移位影响的元素处开始并结束。
 *
 */
	for (uint32 i = 0; i < tb->size; i++)
	{
		SH_ELEMENT_TYPE *entry = &tb->data[i];

		if (entry->status != SH_STATUS_IN_USE)
		{
			startelem = i;
			break;
		}
	}

	/* 此时应该已经找到一个空元素 */
	Assert(startelem < SH_MAX_SIZE);

	/*
 * 反向迭代，这样即使发生向后移位，当前元素
 * 也可以被删除。
 */
	iter->cur = startelem;
	iter->end = iter->cur;
	iter->done = false;
}

/*
 * 把迭代器初始化到指定桶。这其实只在调用者部分地遍历
 * 哈希空间、并且该迭代会基于已访问条目删除和插入元素时
 * 有用。若总是从同一位置开始并反复这样做，可能导致
 * 键空间不平衡。
 *
 */
SH_SCOPE void
SH_START_ITERATE_AT(SH_TYPE * tb, SH_ITERATOR * iter, uint32 at)
{
	/*
 * 反向迭代，这样即使发生向后移位，当前元素
 * 也可以被删除。
 */
	iter->cur = at & tb->sizemask;	/* 确保 at 落在有效范围内 */
	iter->end = iter->cur;
	iter->done = false;
}

/*
 * 遍历哈希表中的全部条目。返回下一个被占用的条目，
 * 若已结束则返回 NULL。
 *
 * 迭代期间可以删除哈希表中的当前条目，而不会导致元素
 * 被跳过或返回两次。此外也可以修改表的其余部分（即可以
 * 有插入或删除），但若这样做，既不保证每个节点至少被
 * 访问一次，也不保证一个节点至多被访问一次。
 *
 */
SH_SCOPE	SH_ELEMENT_TYPE *
SH_ITERATE(SH_TYPE * tb, SH_ITERATOR * iter)
{
	while (!iter->done)
	{
		SH_ELEMENT_TYPE *elem;

		elem = &tb->data[iter->cur];

		/* 反方向上的下一个元素 */
		iter->cur = (iter->cur - 1) & tb->sizemask;

		if ((iter->cur & tb->sizemask) == (iter->end & tb->sizemask))
			iter->done = true;
		if (elem->status == SH_STATUS_IN_USE)
		{
			return elem;
		}
	}

	return NULL;
}

/*
 * 报告哈希表状态的一些统计信息。仅用于调试或性能剖析。
 *
 */
SH_SCOPE void
SH_STAT(SH_TYPE * tb)
{
	uint32		max_chain_length = 0;
	uint32		total_chain_length = 0;
	double		avg_chain_length;
	double		fillfactor;
	uint32		i;

	uint32	   *collisions = (uint32 *) palloc0(tb->size * sizeof(uint32));
	uint32		total_collisions = 0;
	uint32		max_collisions = 0;
	double		avg_collisions;

	for (i = 0; i < tb->size; i++)
	{
		uint32		hash;
		uint32		optimal;
		uint32		dist;
		SH_ELEMENT_TYPE *elem;

		elem = &tb->data[i];

		if (elem->status != SH_STATUS_IN_USE)
			continue;

		hash = SH_ENTRY_HASH(tb, elem);
		optimal = SH_INITIAL_BUCKET(tb, hash);
		dist = SH_DISTANCE_FROM_OPTIMAL(tb, optimal, i);

		if (dist > max_chain_length)
			max_chain_length = dist;
		total_chain_length += dist;

		collisions[optimal]++;
	}

	for (i = 0; i < tb->size; i++)
	{
		uint32		curcoll = collisions[i];

		if (curcoll == 0)
			continue;

		/* 单独包含的一个元素不算冲突 */
		curcoll--;
		total_collisions += curcoll;
		if (curcoll > max_collisions)
			max_collisions = curcoll;
	}

	/* 大到值得释放，即使只是用于调试 */
	pfree(collisions);

	if (tb->members > 0)
	{
		fillfactor = tb->members / ((double) tb->size);
		avg_chain_length = ((double) total_chain_length) / tb->members;
		avg_collisions = ((double) total_collisions) / tb->members;
	}
	else
	{
		fillfactor = 0;
		avg_chain_length = 0;
		avg_collisions = 0;
	}

	sh_log("size: " UINT64_FORMAT ", members: %u, filled: %f, total chain: %u, max chain: %u, avg chain: %f, total_collisions: %u, max_collisions: %u, avg_collisions: %f",
		   tb->size, tb->members, fillfactor, total_chain_length, max_chain_length, avg_chain_length,
		   total_collisions, max_collisions, avg_collisions);
}

#endif							/* SH_DEFINE */


/* 取消定义外部参数，以便可以定义下一张哈希表 */
#undef SH_PREFIX
#undef SH_KEY_TYPE
#undef SH_KEY
#undef SH_ELEMENT_TYPE
#undef SH_HASH_KEY
#undef SH_SCOPE
#undef SH_DECLARE
#undef SH_DEFINE
#undef SH_GET_HASH
#undef SH_STORE_HASH
#undef SH_USE_NONDEFAULT_ALLOCATOR
#undef SH_EQUAL

/* 取消定义本地声明的宏 */
#undef SH_MAKE_PREFIX
#undef SH_MAKE_NAME
#undef SH_MAKE_NAME_
#undef SH_FILLFACTOR
#undef SH_MAX_FILLFACTOR
#undef SH_GROW_MAX_DIB
#undef SH_GROW_MAX_MOVE
#undef SH_GROW_MIN_FILLFACTOR
#undef SH_MAX_SIZE

/* 类型 */
#undef SH_TYPE
#undef SH_STATUS
#undef SH_STATUS_EMPTY
#undef SH_STATUS_IN_USE
#undef SH_ITERATOR

/* 外部函数名 */
#undef SH_CREATE
#undef SH_DESTROY
#undef SH_RESET
#undef SH_INSERT
#undef SH_INSERT_HASH
#undef SH_DELETE_ITEM
#undef SH_DELETE
#undef SH_LOOKUP
#undef SH_LOOKUP_HASH
#undef SH_GROW
#undef SH_START_ITERATE
#undef SH_START_ITERATE_AT
#undef SH_ITERATE
#undef SH_ALLOCATE
#undef SH_FREE
#undef SH_STAT

/* 内部函数名 */
#undef SH_COMPUTE_PARAMETERS
#undef SH_COMPARE_KEYS
#undef SH_INITIAL_BUCKET
#undef SH_NEXT
#undef SH_PREV
#undef SH_DISTANCE_FROM_OPTIMAL
#undef SH_ENTRY_HASH
#undef SH_INSERT_HASH_INTERNAL
#undef SH_LOOKUP_HASH_INTERNAL
