/*-------------------------------------------------------------------------
 *
 * nodeMemoize.c
 *	  处理参数化节点结果缓存的例程。                 
 *
 * Portions Copyright (c) 2021-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMemoize.c
 *
 * Memoize 节点旨在置于计划树中参数化节点之上，         
 * 以便缓存这些节点的结果。其意图在于：                 
 * 当节点已见过某参数值后再次扫描时，                      
 * 可以直接从缓存取元组，而无需重新扫描内节点。     
 * 查询规划器若认为对曾见值的重扫足够常见、            
 * 值得为此多加一个节点，就可能选用                         
 * Memoize。                            
 *
 * 我们采用的缓存方法是哈希表。当缓存填满时，            
 * 我们从不将元组溢写到磁盘，而是改为淘汰                
 * 缓存中最近最少使用的条目。我们将新条目                
 * 与待查条目总是压入双向链表尾部，以此记住         
 * 最近最少使用的条目。旧条目在此 LRU 链上总会           
 * 浮向链顶。  
 *
 * 有时调用者不会把扫描运行到底。例如半连接            
 * 只需运行到找到匹配元组为止，一旦找到，               
 * 连接操作符就会跳到下一个外元组，且在本次扫           
 * 描中不再执行内侧。因此我们必须跟踪缓存条              
 * 目何时完整；默认情况下，我们知道当扫描中             
 * 已无元组可读时它就完整了。不过在某些情况             
 * 下，无需扫尽所有元组就能把缓存条目标记为           
 * 完整。一种情况是唯一连接，此时连接操作符知            
 * 道对任一给定外元组至多只有一个匹配。为了          
 * 支持这类情况，我们允许为缓存设置 "singlerow"               
 * 选项：在读到子节点的第一个元组后，缓存条目            
 * 即告完整。  
 *
 * 在为某组参数填充缓存时，可能出现无法腾出            
 * 足够内存来存放更多元组的情况。若真如此，              
 * 则此时我们应已淘汰了其他所有缓存条目。当          
 * 再缓存一个元组会超出内存预算时，我们必须             
 * 释放正在填充的条目，并把状态机转入                      
 * MEMO_CACHE_BYPASS_MODE。这意味着本次扫描                       
 * 不再尝试为这个特定扫描缓存更多元组，因为              
 * 我们没有那么多内存。状态机会在下次重扫时          
 * 再次重置。若缓存下一组参数元组所需的内存     
 * 较少，那便可能让我们重新开始向缓存放入有               
 * 用条目。    
 *
 *
 * 接口例程      
 *		ExecMemoize 查找缓存，未命中则执行子计划   
 *		ExecInitMemoize 初始化节点及子节点    
 *		ExecEndMemoize 关闭节点及子节点     
 *		ExecReScanMemoize 重扫 memoize 节点    
 *
 *		ExecMemoizeEstimate 估算并行计划所需的 DSM 空间       
 *		ExecMemoizeInitializeDSM 为并行计划初始化 DSM    
 *		ExecMemoizeInitializeWorker 在并行 worker 中附着 DSM 信息
 *		ExecMemoizeRetrieveInstrumentation 从 worker 获取统计信息  
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/nodeMemoize.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

/* ExecMemoize 状态机的各状态       */
#define MEMO_CACHE_LOOKUP			1	/* 尝试执行缓存查找          */
#define MEMO_CACHE_FETCH_NEXT_TUPLE	2	/* 从缓存获取下一个元组   */
#define MEMO_FILLING_CACHE			3	/* 读外侧节点以填充缓存*/
#define MEMO_CACHE_BYPASS_MODE		4	/* Bypass mode.  Just read from our
										 * subplan without caching anything */
#define MEMO_END_OF_SCAN			5	/* 准备好重扫  */


/* 用于内存统计的辅助宏      */
#define EMPTY_ENTRY_MEMORY_BYTES(e)		(sizeof(MemoizeEntry) + \
										 sizeof(MemoizeKey) + \
										 (e)->key->params->t_len);
#define CACHE_TUPLE_BYTES(t)			(sizeof(MemoizeTuple) + \
										 (t)->mintuple->t_len)

 /* MemoizeTuple 存储单个被缓存的元组      */
typedef struct MemoizeTuple
{
	MinimalTuple mintuple;		/* 缓存元组 */
	struct MemoizeTuple *next;	/* The next tuple with the same parameter
								 * values or NULL if it's the last one */
} MemoizeTuple;

/*
 * MemoizeKey
 * 缓存条目的哈希表键，兼作 LRU 链表链接       
 */
typedef struct MemoizeKey
{
	MinimalTuple params;
	dlist_node	lru_node;		/* LRU 链表 next/prev 键指针       */
} MemoizeKey;

/*
 * MemoizeEntry
 *		缓存哈希表所存储的数据结构         
 */
typedef struct MemoizeEntry
{
	MemoizeKey *key;			/* 哈希表查找用的哈希键  */
	MemoizeTuple *tuplehead;	/* Pointer to the first tuple or NULL if no
								 * tuples are cached for this entry */
	uint32		hash;			/* 哈希值(已缓存)*/
	char		status;			/* 哈希状态*/
	bool		complete;		/* 我们是否已把外侧计划读完？   */
} MemoizeEntry;


#define SH_PREFIX memoize
#define SH_ELEMENT_TYPE MemoizeEntry
#define SH_KEY_TYPE MemoizeKey *
#define SH_SCOPE static inline
#define SH_DECLARE
#include "lib/simplehash.h"

static uint32 MemoizeHash_hash(struct memoize_hash *tb,
							   const MemoizeKey *key);
static bool MemoizeHash_equal(struct memoize_hash *tb,
							  const MemoizeKey *key1,
							  const MemoizeKey *key2);

#define SH_PREFIX memoize
#define SH_ELEMENT_TYPE MemoizeEntry
#define SH_KEY_TYPE MemoizeKey *
#define SH_KEY key
#define SH_HASH_KEY(tb, key) MemoizeHash_hash(tb, key)
#define SH_EQUAL(tb, a, b) MemoizeHash_equal(tb, a, b)
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * MemoizeHash_hash
 *		simplehash 的哈希函数。'key' 在此不用，因为要求     
 *		所有表查找都先在 MemoizeState 的 probeslot 中填入   
 *		待查找的键值。                         
 */
static uint32
MemoizeHash_hash(struct memoize_hash *tb, const MemoizeKey *key)
{
	MemoizeState *mstate = (MemoizeState *) tb->private_data;
	ExprContext *econtext = mstate->ss.ps.ps_ExprContext;
	MemoryContext oldcontext;
	TupleTableSlot *pslot = mstate->probeslot;
	uint32		hashkey = 0;
	int			numkeys = mstate->nkeys;

	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	if (mstate->binary_mode)
	{
		for (int i = 0; i < numkeys; i++)
		{
			/* 通过轮转合并相继的哈希键    */
			hashkey = pg_rotate_left32(hashkey, 1);

			if (!pslot->tts_isnull[i])	/* 把 NULL 当作哈希键 0 处理*/
			{
				FormData_pg_attribute *attr;
				uint32		hkey;

				attr = &pslot->tts_tupleDescriptor->attrs[i];

				hkey = datum_image_hash(pslot->tts_values[i], attr->attbyval, attr->attlen);

				hashkey ^= hkey;
			}
		}
	}
	else
	{
		FmgrInfo   *hashfunctions = mstate->hashfunctions;
		Oid		   *collations = mstate->collations;

		for (int i = 0; i < numkeys; i++)
		{
			/* 通过轮转合并相继的哈希键    */
			hashkey = pg_rotate_left32(hashkey, 1);

			if (!pslot->tts_isnull[i])	/* 把 NULL 当作哈希键 0 处理*/
			{
				uint32		hkey;

				hkey = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[i],
														collations[i], pslot->tts_values[i]));
				hashkey ^= hkey;
			}
		}
	}

	MemoryContextSwitchTo(oldcontext);
	return murmurhash32(hashkey);
}

/*
 * MemoizeHash_equal
 *		在哈希表查找期间用于确认哈希值匹配的相等函  
 *		数。'key2' 从不被使用。MemoizeState 的 probeslot       
 *		总会以被查找对象的详情来填充。                        
 */
static bool
MemoizeHash_equal(struct memoize_hash *tb, const MemoizeKey *key1,
				  const MemoizeKey *key2)
{
	MemoizeState *mstate = (MemoizeState *) tb->private_data;
	ExprContext *econtext = mstate->ss.ps.ps_ExprContext;
	TupleTableSlot *tslot = mstate->tableslot;
	TupleTableSlot *pslot = mstate->probeslot;

	/* probeslot 应已由 prepare_probe_slot() 准备好                  */
	ExecStoreMinimalTuple(key1->params, tslot, false);

	if (mstate->binary_mode)
	{
		MemoryContext oldcontext;
		int			numkeys = mstate->nkeys;
		bool		match = true;

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		slot_getallattrs(tslot);
		slot_getallattrs(pslot);

		for (int i = 0; i < numkeys; i++)
		{
			FormData_pg_attribute *attr;

			if (tslot->tts_isnull[i] != pslot->tts_isnull[i])
			{
				match = false;
				break;
			}

			/* 均为 NULL？相等     */
			if (tslot->tts_isnull[i])
				continue;

			/* 对这两个 datum 执行二进制比较    */
			attr = &tslot->tts_tupleDescriptor->attrs[i];
			if (!datum_image_eq(tslot->tts_values[i], pslot->tts_values[i],
								attr->attbyval, attr->attlen))
			{
				match = false;
				break;
			}
		}

		MemoryContextSwitchTo(oldcontext);
		return match;
	}
	else
	{
		econtext->ecxt_innertuple = tslot;
		econtext->ecxt_outertuple = pslot;
		return ExecQual(mstate->cache_eq_expr, econtext);
	}
}

/*
 * 把哈希表初始化为空。     
 */
static void
build_hash_table(MemoizeState *mstate, uint32 size)
{
	/* 当未传入有效大小时，先猜测一个合适的值。   */
	if (size == 0)
		size = 1024;

	/* memoize_create 会把该大小转换为 2 的幂     */
	mstate->hashtable = memoize_create(mstate->tableContext, size, mstate);
}

/*
 * prepare_probe_slot
 *		用 'key' 中所存元组的值来填充 mstate 的 probeslot。 
 *		若 'key' 为 NULL，则通过求值 mstate 的 param_exprs           
 *		来执行填充。   
 */
static inline void
prepare_probe_slot(MemoizeState *mstate, MemoizeKey *key)
{
	TupleTableSlot *pslot = mstate->probeslot;
	TupleTableSlot *tslot = mstate->tableslot;
	int			numKeys = mstate->nkeys;

	ExecClearTuple(pslot);

	if (key == NULL)
	{
		ExprContext *econtext = mstate->ss.ps.ps_ExprContext;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/* 依据当前参数值设置 probeslot 的各值                  */
		for (int i = 0; i < numKeys; i++)
			pslot->tts_values[i] = ExecEvalExpr(mstate->param_exprs[i],
												econtext,
												&pslot->tts_isnull[i]);

		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/* 处理该键的 MinimalTuple 并把值存入 probeslot           */
		ExecStoreMinimalTuple(key->params, tslot, false);
		slot_getallattrs(tslot);
		memcpy(pslot->tts_values, tslot->tts_values, sizeof(Datum) * numKeys);
		memcpy(pslot->tts_isnull, tslot->tts_isnull, sizeof(bool) * numKeys);
	}

	ExecStoreVirtualTuple(pslot);
}

/*
 * entry_purge_tuples
 *		从 'entry' 所指的缓存条目中移除所有元组。          
 *		这会留下一个空缓存条目。同时更新内存统计，    
 *		以反映元组的移除。       
 */
static inline void
entry_purge_tuples(MemoizeState *mstate, MemoizeEntry *entry)
{
	MemoizeTuple *tuple = entry->tuplehead;
	uint64		freed_mem = 0;

	while (tuple != NULL)
	{
		MemoizeTuple *next = tuple->next;

		freed_mem += CACHE_TUPLE_BYTES(tuple);

		/* 释放该元组占用的内存  */
		pfree(tuple->mintuple);
		pfree(tuple);

		tuple = next;
	}

	entry->complete = false;
	entry->tuplehead = NULL;

	/* 更新内存统计           */
	mstate->mem_used -= freed_mem;
}

/*
 * remove_cache_entry
 *		从缓存中移除 'entry' 并释放其占用的内存。
 */
static void
remove_cache_entry(MemoizeState *mstate, MemoizeEntry *entry)
{
	MemoizeKey *key = entry->key;

	dlist_delete(&entry->key->lru_node);

	/* 移除该条目中的全部元组        */
	entry_purge_tuples(mstate, entry);

	/*
	 * 更新内存统计。entry_purge_tuples 应已扣减过         
	 * 每个缓存元组所占用的内存。这里我们只需更新       
	 * 条目本身占用的数量。      
	 */
	mstate->mem_used -= EMPTY_ENTRY_MEMORY_BYTES(entry);

	/* 从缓存中移除该条目     */
	memoize_delete_item(mstate->hashtable, entry);

	pfree(key->params);
	pfree(key);
}

/*
 * cache_purge_all
 *		移除缓存内全部条目。 
 */
static void
cache_purge_all(MemoizeState *mstate)
{
	uint64		evictions = mstate->hashtable->members;
	PlanState  *pstate = (PlanState *) mstate;

	/*
	 * 移除全部条目最高效的方式，很可能就是直接重置    
	 * 缓存的内存上下文，然后重建一个全新哈希表。        
	 * 这样就免去了逐个移除条目并 pfree 各缓存元组          
	 */
	MemoryContextReset(mstate->tableContext);

	/* 让哈希表恢复为原来的大小                   */
	build_hash_table(mstate, ((Memoize *) pstate->plan)->est_entries);

	/* 重置 LRU 链表  */
	dlist_init(&mstate->lru_list);
	mstate->last_tuple = NULL;
	mstate->entry = NULL;

	mstate->mem_used = 0;

	/* XXX 是否该新增手段来跟踪这些清除？      */
	mstate->stats.cache_evictions += evictions; /* 更新统计 */
}

/*
 * cache_reduce_memory
 *		从缓存中淘汰较旧、最近最少使用的条目，以使    
 *		内存消耗重新降回 MemoizeState 的 mem_limit      
 *		之下。                
 *
 * 'specialkey' 若不为 NULL，则当该键所属的条目被               
 * 从缓存中移除时，函数会返回 false。   
 */
static bool
cache_reduce_memory(MemoizeState *mstate, MemoizeKey *specialkey)
{
	bool		specialkey_intact = true;	/* 暂时  */
	dlist_mutable_iter iter;
	uint64		evictions = 0;

	/* 更新内存峰值用量 */
	if (mstate->mem_used > mstate->stats.mem_peak)
		mstate->stats.mem_peak = mstate->mem_used;

	/* 预期只有在内存超预算时才会被调用                  */
	Assert(mstate->mem_used > mstate->mem_limit);

	/* 从 LRU 链表头部开始执行淘汰流程。                  */
	dlist_foreach_modify(iter, &mstate->lru_list)
	{
		MemoizeKey *key = dlist_container(MemoizeKey, lru_node, iter.cur);
		MemoizeEntry *entry;

		/*
		 * 填充哈希探测槽，为此 LRU 条目的查找做准           
		 * 备。
		 */
		prepare_probe_slot(mstate, key);

		/*
		 * 理想情况下 LRU 链表指针应直接存放在条目本      
		 * 身中而不是键中。遗憾的是我们做不到这一点，
		 * 因为 simplehash.h 的代码可能调整表大小并为条目    
		 * 分配新内存，这会使那些指针指向旧的桶。不过 
		 * 用键来存放这些是可行的，因为键只被条目中的    
		 * 一个指针所引用，而该指针当然会随哈希表调整   
		 * 大小而一起移动。既然这里我们只持有指向键的    
		 * 指针，所以必须执行一次哈希表查找，才能找到
		 * 该键所属的条目。               
		 */
		entry = memoize_lookup(mstate->hashtable, NULL);

		/*
		 * 健全性检查：确认找到的是该 LRU 链               
		 * 表项所属的条目。行为异常的哈希或相等函数  
		 * 可能导致找不到条目或找错条目。        
		 */
		if (unlikely(entry == NULL || entry->key != key))
			elog(ERROR, "could not find memoization table entry");

		/*
		 * 若在缓存正被填充新元组时被调用去释放内存 
		 * 则最好多加小心，因为我们可能最终释放掉       
		 * 'specialkey' 所属的条目。通常调用者会把         
		 * 'specialkey' 作为正在被填充的缓存条目的键传入， 
		 * 因此我们必须把 'specialkey_intact' 置为        
		 * false，以告知调用者 specialkey                           
		 * 条目已被移除。  
		 */
		if (key == specialkey)
			specialkey_intact = false;

		/*
		 * 最后移除该条目。这也会把它从 LRU 链表移除。    
		 */
		remove_cache_entry(mstate, entry);

		evictions++;

		/* 若已腾出足够内存则退出 */
		if (mstate->mem_used <= mstate->mem_limit)
			break;
	}

	mstate->stats.cache_evictions += evictions; /* 更新统计 */

	return specialkey_intact;
}

/*
 * cache_lookup
 *		执行一次查找，看是否已基于扫描的当前参数缓    
 *		存过元组。若找到已有条目，就把它移到 LRU 链        
 *		表尾部，置 *found 为 true 然后返回它。若没          
 *		有找到条目，则创建一个新条目并加到 LRU 链表       
 *		尾部。我们还会更新缓存内存统计，并在超出内       
 *		存预算时移除较旧的条目。若成功腾出足够内存，    
 *		则返回新条目，否则返回 NULL。           
 *
 * *found 为 true 时调用者可认定绝不会返回 NULL。   
 */
static MemoizeEntry *
cache_lookup(MemoizeState *mstate, bool *found)
{
	MemoizeKey *key;
	MemoizeEntry *entry;
	MemoryContext oldcontext;

	/* 用当前扫描参数准备探测槽                    */
	prepare_probe_slot(mstate, NULL);

	/*
	 * 把新条目加入缓存。无需传入有效键，因为哈希       
	 * 函数使用的是上面已填充的 mstate probeslot。        
	 */
	entry = memoize_insert(mstate->hashtable, NULL, found);

	if (*found)
	{
		/*
		 * 把已有条目移到 LRU 链表尾部，以标记它是         
		 * 最近使用的条目。
		 */
		dlist_move_tail(&mstate->lru_list, &entry->key->lru_node);

		return entry;
	}

	oldcontext = MemoryContextSwitchTo(mstate->tableContext);

	/* 分配一个新键 */
	entry->key = key = (MemoizeKey *) palloc(sizeof(MemoizeKey));
	key->params = ExecCopySlotMinimalTuple(mstate->probeslot);

	/* 更新缓存的总内存占用量         */
	mstate->mem_used += EMPTY_ENTRY_MEMORY_BYTES(entry);

	/* 初始化该条目    */
	entry->complete = false;
	entry->tuplehead = NULL;

	/*
	 * 由于这是最近使用的条目，故将其压入 LRU 链表      
	 * 的末端。        
	 */
	dlist_push_tail(&mstate->lru_list, &entry->key->lru_node);

	mstate->last_tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * 如果我们已超出内存预算，那么将在缓存中腾出       
	 * 空间。 
	 */
	if (mstate->mem_used > mstate->mem_limit)
	{
		/*
		 * 尝试腾出一些内存。这里失败的可能性极小，因  
		 * 为我们刚添加的条目尚未包含任何元组，而且我 
		 * 们可以移除任何其他条目来减少内存                 
		 * 消耗。          
		 */
		if (unlikely(!cache_reduce_memory(mstate, key)))
			return NULL;

		/*
		 * 从缓存中移除条目的过程可能已使 simplehash.h        
		 * 中的代码把元素挪到哈希表中更早的桶里。若是   
		 * 这样，就需再次执行查找来找到该条目。       
		 * 幸运的是，我们可以检测这种情况是否发生，
		 * 方法是看该条目是否仍在使用中，且键指针与我 
		 * 们预期的键相符。         
		 */
		if (entry->status != memoize_SH_IN_USE || entry->key != key)
		{
			/*
			 * 我们需要重新填充 probeslot，因为上述缓存淘     
			 * 汰期间执行的查找已存入了其他键。          
			 */
			prepare_probe_slot(mstate, key);

			/* 重新找到刚添加的条目*/
			entry = memoize_lookup(mstate->hashtable, NULL);
			Assert(entry != NULL);
		}
	}

	return entry;
}

/*
 * cache_store_tuple
 *		把 'slot' 中存储的元组加入 mstate 的当前缓存条目。
 *		该缓存条目必须已由 cache_lookup() 创建。            
 *		mstate 的 last_tuple 字段必须指向 mstate->entry 的元组   
 *		列表末尾。
 */
static bool
cache_store_tuple(MemoizeState *mstate, TupleTableSlot *slot)
{
	MemoizeTuple *tuple;
	MemoizeEntry *entry = mstate->entry;
	MemoryContext oldcontext;

	Assert(slot != NULL);
	Assert(entry != NULL);

	oldcontext = MemoryContextSwitchTo(mstate->tableContext);

	tuple = (MemoizeTuple *) palloc(sizeof(MemoizeTuple));
	tuple->mintuple = ExecCopySlotMinimalTuple(slot);
	tuple->next = NULL;

	/* 统计我们刚刚消耗的内存       */
	mstate->mem_used += CACHE_TUPLE_BYTES(tuple);

	if (entry->tuplehead == NULL)
	{
		/*
		 * 这是该条目的第一个元组，因此只需让列表头部指 
		 * 向它
		 */
		entry->tuplehead = tuple;
	}
	else
	{
		/* 把该元组压入列表尾部            */
		mstate->last_tuple->next = tuple;
	}

	mstate->last_tuple = tuple;
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 如果我们已超出内存预算，就在缓存中腾出一些空 
	 * 间。
	 */
	if (mstate->mem_used > mstate->mem_limit)
	{
		MemoizeKey *key = entry->key;

		if (!cache_reduce_memory(mstate, key))
			return false;

		/*
		 * 从缓存中移除条目的过程可能已使 simplehash.h        
		 * 中的代码把元素挪到哈希表中更早的桶里。若是   
		 * 这样，就需再次执行查找来找到该条目。       
		 * 幸运的是，我们可以检测这种情况是否发生，
		 * 方法是看该条目是否仍在使用中，且键指针与我 
		 * 们预期的键相符。         
		 */
		if (entry->status != memoize_SH_IN_USE || entry->key != key)
		{
			/*
			 * 我们需要重新填充 probeslot，因为上述缓存淘     
			 * 汰期间执行的查找已存入了其他键。          
			 */
			prepare_probe_slot(mstate, key);

			/* 再找该条目   */
			mstate->entry = entry = memoize_lookup(mstate->hashtable, NULL);
			Assert(entry != NULL);
		}
	}

	return true;
}

static TupleTableSlot *
ExecMemoize(PlanState *pstate)
{
	MemoizeState *node = castNode(MemoizeState, pstate);
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	PlanState  *outerNode;
	TupleTableSlot *slot;

	CHECK_FOR_INTERRUPTS();

	/*
	 * 重置每元组内存上下文，以释放上一个元组周期 
	 * 中分配的表达式求值存储。          
	 */
	ResetExprContext(econtext);

	switch (node->mstatus)
	{
		case MEMO_CACHE_LOOKUP:
			{
				MemoizeEntry *entry;
				TupleTableSlot *outerslot;
				bool		found;

				Assert(node->entry == NULL);

				/*
				 * 仅扫描首次调用会进入此状态。在此我们 
				 * 要看之前是否见过当前参数，以及是否已缓  
				 * 存外侧计划为这些参数将返回的               
				 * 完整                                                     
				 * 记录集。     
				 *
				 * 当找到有效缓存条目时，返回其中第一个  
				 * 元组。若未找到，则创建缓存条目，           
				 * 然后从外侧扫描获取元组。若在那里找到    
				 * 就会尝试缓存它。         
				 */

				/* 看看是否已为当前参数缓存了任何内容         */
				entry = cache_lookup(node, &found);

				if (found && entry->complete)
				{
					node->stats.cache_hits += 1;	/* 更新统计 */

					/*
					 * 设置 last_tuple 和 entry，使状态   
					 * MEMO_CACHE_FETCH_NEXT_TUPLE 能轻松找到这些   
					 * 参数的下一个元组。
					 */
					node->last_tuple = entry->tuplehead;
					node->entry = entry;

					/* 若有则取第一个缓存元组             */
					if (entry->tuplehead)
					{
						node->mstatus = MEMO_CACHE_FETCH_NEXT_TUPLE;

						slot = node->ss.ps.ps_ResultTupleSlot;
						ExecStoreMinimalTuple(entry->tuplehead->mintuple,
											  slot, false);

						return slot;
					}

					/* 该缓存条目中没有任何元组。*/
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				/* 处理缓存缺失*/
				node->stats.cache_misses += 1;	/* 更新统计 */

				if (found)
				{
					/*
					 * 此时会找到缓存条目，但针对该条目的 
					 * 扫描并未运行到底。我们只需移除    
					 * 元组并重新开始。从上次中断处继续
					 * 处理虽然诱人，但无法保证外侧节点仍 
					 * 会按照与上次扫描相同的顺序来产出  
					 * 这些元组。           
					 */
					entry_purge_tuples(node, entry);
				}

				/* 扫描外侧节点找可缓存的元组  */
				outerNode = outerPlanState(node);
				outerslot = ExecProcNode(outerNode);
				if (TupIsNull(outerslot))
				{
					/*
					 * cache_lookup 可能因腾不出足够缓存空间而 
					 * 返回 NULL，因此这里不要做任何假定它     
					 * 已成功的操作。这里无需进入旁路模式， 
					 * 因为我们正把 mstatus 置为扫描结         
					 * 束  
					 */
					if (likely(entry))
						entry->complete = true;

					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				node->entry = entry;

				/*
				 * 若创建条目失败，或向条目存元组失败， 
				 * 则进入旁路模式。                     
				 */
				if (unlikely(entry == NULL ||
							 !cache_store_tuple(node, outerslot)))
				{
					node->stats.cache_overflows += 1;	/* 更新统计 */

					node->mstatus = MEMO_CACHE_BYPASS_MODE;

					/*
					 * 无需清空 last_tuple，因为在扫描结束前我  
					 * 们会保持旁路模式。    
					 */
				}
				else
				{
					/*
					 * 若预期此扫描只返回单行，那么可以     
					 * 标记不再期待更多行。这样，即使扫描 
					 * 尚未真正执行完毕，缓存查找也能正常  
					 * 工作。              
					 */
					entry->complete = node->singlerow;
					node->mstatus = MEMO_FILLING_CACHE;
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecCopySlot(slot, outerslot);
				return slot;
			}

		case MEMO_CACHE_FETCH_NEXT_TUPLE:
			{
				/* 若这些未被设置，就不应处于此状态   */
				Assert(node->entry != NULL);
				Assert(node->last_tuple != NULL);

				/* 跳到下一个待输出的元组*/
				node->last_tuple = node->last_tuple->next;

				/* 缓存中没有更多元组 */
				if (node->last_tuple == NULL)
				{
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecStoreMinimalTuple(node->last_tuple->mintuple, slot,
									  false);

				return slot;
			}

		case MEMO_FILLING_CACHE:
			{
				TupleTableSlot *outerslot;
				MemoizeEntry *entry = node->entry;

				/* entry 应已由 MEMO_CACHE_LOOKUP 设置好             */
				Assert(entry != NULL);

				/*
				 * 处于 MEMO_FILLING_CACHE 状态时，我们刚遭遇 
				 * 了缓存未命中，正用当前扫描元组填充缓  
				 * 存。      
				 */
				outerNode = outerPlanState(node);
				outerslot = ExecProcNode(outerNode);
				if (TupIsNull(outerslot))
				{
					/* 已无元组，标记为完整。    */
					entry->complete = true;
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				/*
				 * 校验规划器是否正确设置了 singlerow 标志。   
				 * 只有当每个条目至多能返回 1 行时才应设
				 * 置它。    
				 */
				if (unlikely(entry->complete))
					elog(ERROR, "cache entry already complete");

				/* 把该元组记录到当前缓存条目中  */
				if (unlikely(!cache_store_tuple(node, outerslot)))
				{
					/* 没能存入？处理溢出         */
					node->stats.cache_overflows += 1;	/* 更新统计 */

					node->mstatus = MEMO_CACHE_BYPASS_MODE;

					/*
					 * 无需清空 entry 或 last_tuple，因为在扫描结
					 * 束前我们会保持旁路模式。     
					 */
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecCopySlot(slot, outerslot);
				return slot;
			}

		case MEMO_CACHE_BYPASS_MODE:
			{
				TupleTableSlot *outerslot;

				/*
				 * 处于旁路模式时，只是继续读取元组而不     
				 * 进行缓存。需要等到下一次重扫之后才能   
				 * 退出此模式。        
				 */
				outerNode = outerPlanState(node);
				outerslot = ExecProcNode(outerNode);
				if (TupIsNull(outerslot))
				{
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecCopySlot(slot, outerslot);
				return slot;
			}

		case MEMO_END_OF_SCAN:

			/*
			 * 本次扫描我们已经返回过 NULL，但以防有什么
			 * 东西误再次调用我们。      
			 */
			return NULL;

		default:
			elog(ERROR, "unrecognized memoize state: %d",
				 (int) node->mstatus);
			return NULL;
	}							/* switch */
}

MemoizeState *
ExecInitMemoize(Memoize *node, EState *estate, int eflags)
{
	MemoizeState *mstate = makeNode(MemoizeState);
	Plan	   *outerNode;
	int			i;
	int			nkeys;
	Oid		   *eqfuncoids;

	/* 检查不支持的标志    */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	mstate->ss.ps.plan = (Plan *) node;
	mstate->ss.ps.state = estate;
	mstate->ss.ps.ExecProcNode = ExecMemoize;

	/*
	 * 杂项初始化             
	 *
	 * 为节点创建表达式上下文 
	 */
	ExecAssignExprContext(estate, &mstate->ss.ps);

	outerNode = outerPlan(node);
	outerPlanState(mstate) = ExecInitNode(outerNode, estate, eflags);

	/*
	 * 初始化返回槽和类型。无需初始化投影信息，          
	 * 因为该节点不做投影。           
	 */
	ExecInitResultTupleSlotTL(&mstate->ss.ps, &TTSOpsMinimalTuple);
	mstate->ss.ps.ps_ProjInfo = NULL;

	/*
	 * 初始化扫描槽和类型。
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &mstate->ss, &TTSOpsMinimalTuple);

	/*
	 * 把状态机设为查找缓存。在缓存了内容之前我      
	 * 们不会找到任何东西，但这省去了创建首个条         
	 * 目特例。
	 */
	mstate->mstatus = MEMO_CACHE_LOOKUP;

	mstate->nkeys = nkeys = node->numKeys;
	mstate->hashkeydesc = ExecTypeFromExprList(node->param_exprs);
	mstate->tableslot = MakeSingleTupleTableSlot(mstate->hashkeydesc,
												 &TTSOpsMinimalTuple);
	mstate->probeslot = MakeSingleTupleTableSlot(mstate->hashkeydesc,
												 &TTSOpsVirtual);

	mstate->param_exprs = (ExprState **) palloc(nkeys * sizeof(ExprState *));
	mstate->collations = node->collations;	/* Just point directly to the plan
											 * data */
	mstate->hashfunctions = (FmgrInfo *) palloc(nkeys * sizeof(FmgrInfo));

	eqfuncoids = palloc(nkeys * sizeof(Oid));

	for (i = 0; i < nkeys; i++)
	{
		Oid			hashop = node->hashOperators[i];
		Oid			left_hashfn;
		Oid			right_hashfn;
		Expr	   *param_expr = (Expr *) list_nth(node->param_exprs, i);

		if (!get_op_hash_functions(hashop, &left_hashfn, &right_hashfn))
			elog(ERROR, "could not find hash function for hash operator %u",
				 hashop);

		fmgr_info(left_hashfn, &mstate->hashfunctions[i]);

		mstate->param_exprs[i] = ExecInitExpr(param_expr, (PlanState *) mstate);
		eqfuncoids[i] = get_opcode(hashop);
	}

	mstate->cache_eq_expr = ExecBuildParamSetEqual(mstate->hashkeydesc,
												   &TTSOpsMinimalTuple,
												   &TTSOpsVirtual,
												   eqfuncoids,
												   node->collations,
												   node->param_exprs,
												   (PlanState *) mstate);

	pfree(eqfuncoids);
	mstate->mem_used = 0;

	/* 把缓存消耗的总内存限制为此值           */
	mstate->mem_limit = get_hash_memory_limit();

	/* 专用于缓存的内存上下文        */
	mstate->tableContext = AllocSetContextCreate(CurrentMemoryContext,
												 "MemoizeHashTable",
												 ALLOCSET_DEFAULT_SIZES);

	dlist_init(&mstate->lru_list);
	mstate->last_tuple = NULL;
	mstate->entry = NULL;

	/*
	 * 标记取到某缓存条目的第一条记录后，我们是否    
	 * 可以认定该条目已完成。有些调用者在得到第    
	 * 一个匹配后就不会再调用我们。例如执行唯一连       
	 * 接的连接操作符，在得到第一个匹配的内元组后
	 * 便能跳到下一个外元组。此时缓存条目在取到          
	 * 第一个元组后即完成，因此可如此标记。    
	 */
	mstate->singlerow = node->singlerow;
	mstate->keyparamids = node->keyparamids;

	/*
	 * 记录缓存键应逐位比较，还是用该类型的哈希        
	 * 相等操作符做逻辑比较。      
	 */
	mstate->binary_mode = node->binary_mode;

	/* 统计计数器清零        */
	memset(&mstate->stats, 0, sizeof(MemoizeInstrumentation));

	/* 分配并设置实际的缓存       */
	build_hash_table(mstate, node->est_entries);

	return mstate;
}

void
ExecEndMemoize(MemoizeState *node)
{
#ifdef USE_ASSERT_CHECKING
	/* 在断言构建中校验内存统计代码是否正确。        */
	{
		int			count;
		uint64		mem = 0;
		memoize_iterator i;
		MemoizeEntry *entry;

		memoize_start_iterate(node->hashtable, &i);

		count = 0;
		while ((entry = memoize_iterate(node->hashtable, &i)) != NULL)
		{
			MemoizeTuple *tuple = entry->tuplehead;

			mem += EMPTY_ENTRY_MEMORY_BYTES(entry);
			while (tuple != NULL)
			{
				mem += CACHE_TUPLE_BYTES(tuple);
				tuple = tuple->next;
			}
			count++;
		}

		Assert(count == node->hashtable->members);
		Assert(mem == node->mem_used);
	}
#endif

	/*
	 * 结束并行 worker 时，把该 worker 收集的统计信息     
	 * 复制回共享内存，以便主进程能取用它们并在          
	 * EXPLAIN ANALYZE 中报告。         
	 */
	if (node->shared_info != NULL && IsParallelWorker())
	{
		MemoizeInstrumentation *si;

		/* 让 mem_peak 可供 EXPLAIN 使用  */
		if (node->stats.mem_peak == 0)
			node->stats.mem_peak = node->mem_used;

		Assert(ParallelWorkerNumber <= node->shared_info->num_workers);
		si = &node->shared_info->sinstrument[ParallelWorkerNumber];
		memcpy(si, &node->stats, sizeof(MemoizeInstrumentation));
	}

	/* 移除缓存上下文    */
	MemoryContextDelete(node->tableContext);

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* 必须丢弃缓存结果元组指针    */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * 释放上下文 
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * 关闭子计划      
	 */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanMemoize(MemoizeState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/* 标记必须为一组新参数查找缓存                     */
	node->mstatus = MEMO_CACHE_LOOKUP;

	/* 将上次扫描所用的指针置空    */
	node->entry = NULL;
	node->last_tuple = NULL;

	/*
	 * 如果子节点的 chgParam 非空，则计划会在第一次     
	 * 调用时重扫。 
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/*
	 * 如果变化的参数不属于缓存键的一部分，则清空整个
	 * 缓存。 
	 */
	if (bms_nonempty_difference(outerPlan->chgParam, node->keyparamids))
		cache_purge_all(node);
}

/*
 * ExecEstimateCacheEntryOverheadBytes
 *		供查询规划器使用，帮助它估算在缓存中存            
 *		储单个条目所需的内存量。          
 */
double
ExecEstimateCacheEntryOverheadBytes(double ntuples)
{
	return sizeof(MemoizeEntry) + sizeof(MemoizeKey) + sizeof(MemoizeTuple) *
		ntuples;
}

/* ----------------------------------------------------------------
 *						并行查询支持    
 * ----------------------------------------------------------------
 */

 /* ----------------------------------------------------------------
  *		ExecMemoizeEstimate
  *
  *		估算传播 memoize 统计信息所需的空间。     
  * ----------------------------------------------------------------
  */
void
ExecMemoizeEstimate(MemoizeState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 若未启用统计或没有 worker 则无需此项 */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(MemoizeInstrumentation));
	size = add_size(size, offsetof(SharedMemoizeInfo, sinstrument));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecMemoizeInitializeDSM
 *
 *		为 memoize 统计初始化 DSM 空间。   
 * ----------------------------------------------------------------
 */
void
ExecMemoizeInitializeDSM(MemoizeState *node, ParallelContext *pcxt)
{
	Size		size;

	/* 若未启用统计或没有 worker 则无需此项 */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedMemoizeInfo, sinstrument)
		+ pcxt->nworkers * sizeof(MemoizeInstrumentation);
	node->shared_info = shm_toc_allocate(pcxt->toc, size);
	/* 确保所有未填充的槽都包含零       */
	memset(node->shared_info, 0, size);
	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id,
				   node->shared_info);
}

/* ----------------------------------------------------------------
 *		ExecMemoizeInitializeWorker
 *
 *		把 worker 附着到 memoize 统计 DSM 空间。 
 * ----------------------------------------------------------------
 */
void
ExecMemoizeInitializeWorker(MemoizeState *node, ParallelWorkerContext *pwcxt)
{
	node->shared_info =
		shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, true);
}

/* ----------------------------------------------------------------
 *		ExecMemoizeRetrieveInstrumentation
 *
 *		把 memoize 统计信息从 DSM 传到私有内存。  
 * ----------------------------------------------------------------
 */
void
ExecMemoizeRetrieveInstrumentation(MemoizeState *node)
{
	Size		size;
	SharedMemoizeInfo *si;

	if (node->shared_info == NULL)
		return;

	size = offsetof(SharedMemoizeInfo, sinstrument)
		+ node->shared_info->num_workers * sizeof(MemoizeInstrumentation);
	si = palloc(size);
	memcpy(si, node->shared_info, size);
	node->shared_info = si;
}
