/*-------------------------------------------------------------------------
 *
 * hashjoin.h
 *	  internal structures for hash joins
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/hashjoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHJOIN_H
#define HASHJOIN_H

#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/barrier.h"
#include "storage/buffile.h"
#include "storage/lwlock.h"

/* ----------------------------------------------------------------
 *				hash-join hash table structures
 *
 * Each active hashjoin has a HashJoinTable structure, which is
 * palloc'd in the executor's per-query context.  Other storage needed for
 * each hashjoin is kept in child contexts, three for each hashjoin:
 *   - HashTableContext (hashCxt): the parent hash table storage context
 *   - HashSpillContext (spillCxt): storage for temp files buffers
 *   - HashBatchContext (batchCxt): storage for a batch in serial hash join
 *
 * The hashtable contexts are made children of the per-query context, ensuring
 * that they will be discarded at end of statement even if the join is
 * aborted early by an error.  (Likewise, any temporary files we make will
 * be cleaned up by the virtual file manager in event of an error.)
 *
 * Storage that should live through the entire join is allocated from the
 * "hashCxt" (mainly the hashtable's metadata). Also, the "hashCxt" context is
 * the parent of "spillCxt" and "batchCxt". It makes it easy and fast to
 * release the storage when we don't need it anymore.
 *
 * Data associated with temp files is allocated in the "spillCxt" context
 * which lives for the duration of the entire join as batch files'
 * creation and usage may span batch execution. These files are
 * explicitly destroyed by calling BufFileClose() when the code is done
 * with them. The aim of this context is to help accounting for the
 * memory allocated for temp files and their buffers.
 *
 * Finally, data used only during a single batch's execution is allocated
 * in the "batchCxt". By resetting the batchCxt at the end of each batch,
 * we free all the per-batch storage reliably and without tedium.
 *
 * During first scan of inner relation, we get its tuples from executor.
 * If nbatch > 1 then tuples that don't belong in first batch get saved
 * into inner-batch temp files. The same statements apply for the
 * first scan of the outer relation, except we write tuples to outer-batch
 * temp files.  After finishing the first scan, we do the following for
 * each remaining batch:
 *	1. Read tuples from inner batch file, load into hash buckets.
 *	2. Read tuples from outer batch file, match to hash buckets and output.
 *
 * It is possible to increase nbatch on the fly if the in-memory hash table
 * gets too big.  The hash-value-to-batch computation is arranged so that this
 * can only cause a tuple to go into a later batch than previously thought,
 * never into an earlier batch.  When we increase nbatch, we rescan the hash
 * table and dump out any tuples that are now of a later batch to the correct
 * inner batch file.  Subsequently, while reading either inner or outer batch
 * files, we might find tuples that no longer belong to the current batch;
 * if so, we just dump them out to the correct batch file.
 * ----------------------------------------------------------------
 */

/* these are in nodes/execnodes.h: */
/* typedef struct HashJoinTupleData *HashJoinTuple; */
/* typedef struct HashJoinTableData *HashJoinTable; */

typedef struct HashJoinTupleData
{
	/*  链到同桶中下一元组  */
	union
	{
		struct HashJoinTupleData *unshared;
		dsa_pointer shared;
	}			next;
	uint32		hashvalue;		/* tuple's hash code */
	/*  元组数据,MinimalTuple 格式,跟在 MAXALIGN 边界上  */
}			HashJoinTupleData;

#define HJTUPLE_OVERHEAD  MAXALIGN(sizeof(HashJoinTupleData))
#define HJTUPLE_MINTUPLE(hjtup)  \
	((MinimalTuple) ((char *) (hjtup) + HJTUPLE_OVERHEAD))

/*
 * If the outer relation's distribution is sufficiently nonuniform, we attempt
 * to optimize the join by treating the hash values corresponding to the outer
 * relation's MCVs specially.  Inner relation tuples matching these hash
 * values go into the "skew" hashtable instead of the main hashtable, and
 * outer relation tuples with these hash values are matched against that
 * table instead of the main one.  Thus, tuples with these hash values are
 * effectively handled as part of the first batch and will never go to disk.
 * The skew hashtable is limited to SKEW_HASH_MEM_PERCENT of the total memory
 * allowed for the join; while building the hashtables, we decrease the number
 * of MCVs being specially treated if needed to stay under this limit.
 *
 * Note: you might wonder why we look at the outer relation stats for this,
 * rather than the inner.  One reason is that the outer relation is typically
 * bigger, so we get more I/O savings by optimizing for its most common values.
 * Also, for similarly-sized relations, the planner prefers to put the more
 * uniformly distributed relation on the inside, so we're more likely to find
 * interesting skew in the outer relation.
 */
typedef struct HashSkewBucket
{
	uint32		hashvalue;		/*  常见哈希值  */
	HashJoinTuple tuples;		/*  内关系元组的链表  */
} HashSkewBucket;

#define SKEW_BUCKET_OVERHEAD  MAXALIGN(sizeof(HashSkewBucket))
#define INVALID_SKEW_BUCKET_NO	(-1)
#define SKEW_HASH_MEM_PERCENT  2
#define SKEW_MIN_OUTER_FRACTION  0.01

/*
 * To reduce palloc overhead, the HashJoinTuples for the current batch are
 * packed in 32kB buffers instead of pallocing each tuple individually.
 */
typedef struct HashMemoryChunkData
{
	int			ntuples;		/*  本块中存储的元组数  */
	size_t		maxlen;			/*  块的元组缓冲大小  */
	size_t		used;			/*  已使用的缓冲字节数  */

	/*  指向下一块的指针(链表)  */
	union
	{
		struct HashMemoryChunkData *unshared;
		dsa_pointer shared;
	}			next;

	/*
	 * The chunk's tuple buffer starts after the HashMemoryChunkData struct,
	 * at offset HASH_CHUNK_HEADER_SIZE (which must be maxaligned).  Note that
	 * that offset is not included in "maxlen" or "used".
	 */
}			HashMemoryChunkData;

typedef struct HashMemoryChunkData *HashMemoryChunk;

#define HASH_CHUNK_SIZE			(32 * 1024L)
#define HASH_CHUNK_HEADER_SIZE	MAXALIGN(sizeof(HashMemoryChunkData))
#define HASH_CHUNK_DATA(hc)		(((char *) (hc)) + HASH_CHUNK_HEADER_SIZE)
/*  超过 HASH_CHUNK_THRESHOLD 字节的元组放入自有块  */
#define HASH_CHUNK_THRESHOLD	(HASH_CHUNK_SIZE / 4)

/*
 * For each batch of a Parallel Hash Join, we have a ParallelHashJoinBatch
 * object in shared memory to coordinate access to it.  Since they are
 * followed by variable-sized objects, they are arranged in contiguous memory
 * but not accessed directly as an array.
 */
typedef struct ParallelHashJoinBatch
{
	dsa_pointer buckets;		/*  哈希表桶数组  */
	Barrier		batch_barrier;	/*  连接本批的同步  */

	dsa_pointer chunks;			/*  已装入的元组块  */
	size_t		size;			/*  内存中桶+块的大小  */
	size_t		estimated_size; /*  写出时桶+块的大小  */
	size_t		ntuples;		/*  已装入元组数  */
	size_t		old_ntuples;	/*  重分区前的元组数  */
	bool		space_exhausted;
	bool		skip_unmatched; /*  是否放弃未匹配扫描  */

	/*
	 * Variable-sized SharedTuplestore objects follow this struct in memory.
	 * See the accessor macros below.
	 */
} ParallelHashJoinBatch;

/*  ParallelHashJoinBatch 后内批 tuplestore 的访问器。  */
#define ParallelHashJoinBatchInner(batch)							\
	((SharedTuplestore *)											\
	 ((char *) (batch) + MAXALIGN(sizeof(ParallelHashJoinBatch))))

/*  ParallelHashJoinBatch 后外批 tuplestore 的访问器。  */
#define ParallelHashJoinBatchOuter(batch, nparticipants) \
	((SharedTuplestore *)												\
	 ((char *) ParallelHashJoinBatchInner(batch) +						\
	  MAXALIGN(sts_estimate(nparticipants))))

/*  ParallelHashJoinBatch 与 tuplestore 的总大小。  */
#define EstimateParallelHashJoinBatch(hashtable)						\
	(MAXALIGN(sizeof(ParallelHashJoinBatch)) +							\
	 MAXALIGN(sts_estimate((hashtable)->parallel_state->nparticipants)) * 2)

/*  给定基址访问第 n 个 ParallelHashJoinBatch。  */
#define NthParallelHashJoinBatch(base, n)								\
	((ParallelHashJoinBatch *)											\
	 ((char *) (base) +													\
	  EstimateParallelHashJoinBatch(hashtable) *  (n)))

/*
 * Each backend requires a small amount of per-batch state to interact with
 * each ParallelHashJoinBatch.
 */
typedef struct ParallelHashJoinBatchAccessor
{
	ParallelHashJoinBatch *shared;	/* pointer to shared state */

	/*  每后端部分计数器以降低争用。  */
	size_t		preallocated;	/*  本后端预分配的空间  */
	size_t		ntuples;		/*  元组数  */
	size_t		size;			/* size of partition in memory */
	size_t		estimated_size; /* size of partition on disk */
	size_t		old_ntuples;	/*  重分区前有多少元组?  */
	bool		at_least_one_chunk; /*  本后端是否已分配块?  */
	bool		outer_eof;		/*  本进程是否已到批末?  */
	bool		done;			/*  记住批已完成的标志  */
	SharedTuplestoreAccessor *inner_tuples;
	SharedTuplestoreAccessor *outer_tuples;
} ParallelHashJoinBatchAccessor;

/*
 * While hashing the inner relation, any participant might determine that it's
 * time to increase the number of buckets to reduce the load factor or batches
 * to reduce the memory size.  This is indicated by setting the growth flag to
 * these values.
 */
typedef enum ParallelHashGrowth
{
	/*  当前维度已足够。  */
	PHJ_GROWTH_OK,
	/*  负载因子过高,需要加桶。  */
	PHJ_GROWTH_NEED_MORE_BUCKETS,
	/*  内存预算将耗尽,需要重分区。  */
	PHJ_GROWTH_NEED_MORE_BATCHES,
	/*  上次重分区无助,故不再尝试。  */
	PHJ_GROWTH_DISABLED
} ParallelHashGrowth;

/*
 * The shared state used to coordinate a Parallel Hash Join.  This is stored
 * in the DSM segment.
 */
typedef struct ParallelHashJoinState
{
	dsa_pointer batches;		/*  ParallelHashJoinBatch 数组  */
	dsa_pointer old_batches;	/*  重分区时的上一代  */
	int			nbatch;			/*  当前批次数  */
	int			old_nbatch;		/*  先前批次数  */
	int			nbuckets;		/*  桶数  */
	ParallelHashGrowth growth;	/*  控制批/桶增长  */
	dsa_pointer chunk_work_queue;	/* chunk work queue */
	int			nparticipants;
	size_t		space_allowed;
	size_t		total_tuples;	/*  内元组总数  */
	LWLock		lock;			/* lock protecting the above */

	Barrier		build_barrier;	/*  构建阶段的同步  */
	Barrier		grow_batches_barrier;
	Barrier		grow_buckets_barrier;
	pg_atomic_uint32 distributor;	/*  负载均衡计数器  */

	SharedFileSet fileset;		/*  共享临时文件空间  */
} ParallelHashJoinState;

/*  构建批的阶段,由 build_barrier 使用。  */
#define PHJ_BUILD_ELECT					0
#define PHJ_BUILD_ALLOCATE				1
#define PHJ_BUILD_HASH_INNER			2
#define PHJ_BUILD_HASH_OUTER			3
#define PHJ_BUILD_RUN					4
#define PHJ_BUILD_FREE					5

/*  探测各批的阶段,由 batch_barrier 使用。  */
#define PHJ_BATCH_ELECT					0
#define PHJ_BATCH_ALLOCATE				1
#define PHJ_BATCH_LOAD					2
#define PHJ_BATCH_PROBE					3
#define PHJ_BATCH_SCAN					4
#define PHJ_BATCH_FREE					5

/*  哈希时批增长阶段,供 grow_batches_barrier。  */
#define PHJ_GROW_BATCHES_ELECT			0
#define PHJ_GROW_BATCHES_REALLOCATE		1
#define PHJ_GROW_BATCHES_REPARTITION	2
#define PHJ_GROW_BATCHES_DECIDE			3
#define PHJ_GROW_BATCHES_FINISH			4
#define PHJ_GROW_BATCHES_PHASE(n)		((n) % 5)	/*  循环阶段  */

/*  哈希时桶增长阶段,供 grow_buckets_barrier。  */
#define PHJ_GROW_BUCKETS_ELECT			0
#define PHJ_GROW_BUCKETS_REALLOCATE		1
#define PHJ_GROW_BUCKETS_REINSERT		2
#define PHJ_GROW_BUCKETS_PHASE(n)		((n) % 3)	/*  循环阶段  */

typedef struct HashJoinTableData
{
	int			nbuckets;		/*  内存哈希表中的桶数  */
	int			log2_nbuckets;	/*  其 log2(nbuckets 须为 2 的幂)  */

	int			nbuckets_original;	/*  开始第一次哈希时的桶数  */
	int			nbuckets_optimal;	/*  最优桶数(每批)  */
	int			log2_nbuckets_optimal;	/* log2(nbuckets_optimal) */

	/*  buckets[i] 是第 i 个内存桶中元组链表头  */
	union
	{
		/*  非共享数组是每批存储,元组亦然  */
		struct HashJoinTupleData **unshared;
		/*  共享数组在每查询 DSA 区,元组亦然  */
		dsa_pointer_atomic *shared;
	}			buckets;

	bool		keepNulls;		/* true to store unmatchable NULL tuples */

	bool		skewEnabled;	/*  是否使用偏斜优化?  */
	HashSkewBucket **skewBucket;	/*  偏斜桶的哈希表  */
	int			skewBucketLen;	/* size of skewBucket array (a power of 2!) */
	int			nSkewBuckets;	/*  活动偏斜桶数  */
	int		   *skewBucketNums; /*  活动偏斜桶的数组下标  */

	int			nbatch;			/*  批次数  */
	int			curbatch;		/*  当前批号;首遍时为 0  */

	int			nbatch_original;	/*  开始内扫描时的 nbatch  */
	int			nbatch_outstart;	/*  开始外扫描时的 nbatch  */

	bool		growEnabled;	/*  关闭 nbatch 增长的标志  */

	double		totalTuples;	/*  从内计划获得的元组数  */
	double		partialTuples;	/*  我从内计划获得的元组数  */
	double		skewTuples;		/*  插入偏斜元组的个数  */

	/*
	 * These arrays are allocated for the life of the hash join, but only if
	 * nbatch > 1.  A file is opened only when we first write a tuple into it
	 * (otherwise its pointer remains NULL).  Note that the zero'th array
	 * elements never get used, since we will process rather than dump out any
	 * tuples of batch zero.
	 */
	BufFile   **innerBatchFile; /* buffered virtual temp file per batch */
	BufFile   **outerBatchFile; /* buffered virtual temp file per batch */

	/*
	 * Info about the datatype-specific hash functions for the datatypes being
	 * hashed. These are arrays of the same length as the number of hash join
	 * clauses (hash keys).
	 */
	FmgrInfo   *outer_hashfunctions;	/*  哈希函数的查找数据  */
	FmgrInfo   *inner_hashfunctions;	/*  哈希函数的查找数据  */
	bool	   *hashStrict;		/* is each hash join operator strict? */
	Oid		   *collations;

	Size		spaceUsed;		/*  元组当前占用的内存空间  */
	Size		spaceAllowed;	/*  空间使用上限  */
	Size		spacePeak;		/* peak space used */
	Size		spaceUsedSkew;	/*  偏斜哈希表当前空间用量  */
	Size		spaceAllowedSkew;	/*  偏斜哈希表上限  */

	MemoryContext hashCxt;		/* context for whole-hash-join storage */
	MemoryContext batchCxt;		/*  仅本批的存储上下文  */
	MemoryContext spillCxt;		/* context for spilling to temp files */

	/*  用于密集分配元组(到链接块中)  */
	HashMemoryChunk chunks;		/*  整批一个列表  */

	/*  并行哈希的共享与私有状态。  */
	HashMemoryChunk current_chunk;	/*  本后端当前块  */
	dsa_area   *area;			/*  从中分配内存的 DSA 区  */
	ParallelHashJoinState *parallel_state;
	ParallelHashJoinBatchAccessor *batches;
	dsa_pointer current_chunk_shared;
} HashJoinTableData;

#endif							/* HASHJOIN_H */
