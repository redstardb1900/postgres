/*-------------------------------------------------------------------------
 *
 * execGrouping.c
 *	  分组、哈希与聚合的执行器支撑例程。             
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execGrouping.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

static int	TupleHashTableMatch(struct tuplehash_hash *tb, const MinimalTuple tuple1, const MinimalTuple tuple2);
static inline uint32 TupleHashTableHash_internal(struct tuplehash_hash *tb,
												 const MinimalTuple tuple);
static inline TupleHashEntry LookupTupleHashEntry_internal(TupleHashTable hashtable,
														   TupleTableSlot *slot,
														   bool *isnew, uint32 hash);

/*
 * 为元组哈希表的代码生成定义参数。接口                  
 * *亦*在 execnodes.h 中声明（用于生成对外可见的类型         
 * ）。   
 */
#define SH_PREFIX tuplehash
#define SH_ELEMENT_TYPE TupleHashEntryData
#define SH_KEY_TYPE MinimalTuple
#define SH_KEY firstTuple
#define SH_HASH_KEY(tb, key) TupleHashTableHash_internal(tb, key)
#define SH_EQUAL(tb, a, b) TupleHashTableMatch(tb, a, b) == 0
#define SH_SCOPE extern
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"


/*****************************************************************************
 *		用于将元组归为一组的支撑例程   
 *****************************************************************************/

/*
 * execTuplesMatchPrepare
 *		构建可用 ExecQual() 求值的表达式，返回               
 *		ExprContext 的 inner/outer 元组是否为 NOT DISTINCT    
 */
ExprState *
execTuplesMatchPrepare(TupleDesc desc,
					   int numCols,
					   const AttrNumber *keyColIdx,
					   const Oid *eqOperators,
					   const Oid *collations,
					   PlanState *parent)
{
	Oid		   *eqFunctions = (Oid *) palloc(numCols * sizeof(Oid));
	int			i;
	ExprState  *expr;

	if (numCols == 0)
		return NULL;

	/* 查找相等性函数     */
	for (i = 0; i < numCols; i++)
		eqFunctions[i] = get_opcode(eqOperators[i]);

	/* 构建实际表达式   */
	expr = ExecBuildGroupingEqual(desc, desc, NULL, NULL,
								  numCols, keyColIdx, eqFunctions, collations,
								  parent);

	return expr;
}

/*
 * execTuplesHashPrepare
 *		查找 TupleHashTable 所需的相等与哈希函数。                
 *
 * 本函数与 execTuplesMatchPrepare 类似，但我们还需           
 * 要找出与相等算子关联的哈希函数。*eqFunctions 与        
 * *hashFunctions 接收 palloc 出的结果数组。
 *
 * 注意：我们假定给定的算子不是跨类型比较。            
 */
void
execTuplesHashPrepare(int numCols,
					  const Oid *eqOperators,
					  Oid **eqFuncOids,
					  FmgrInfo **hashFunctions)
{
	int			i;

	*eqFuncOids = (Oid *) palloc(numCols * sizeof(Oid));
	*hashFunctions = (FmgrInfo *) palloc(numCols * sizeof(FmgrInfo));

	for (i = 0; i < numCols; i++)
	{
		Oid			eq_opr = eqOperators[i];
		Oid			eq_function;
		Oid			left_hash_function;
		Oid			right_hash_function;

		eq_function = get_opcode(eq_opr);
		if (!get_op_hash_functions(eq_opr,
								   &left_hash_function, &right_hash_function))
			elog(ERROR, "could not find hash function for hash operator %u",
				 eq_opr);
		/* 此处不支持跨类型的情形          */
		Assert(left_hash_function == right_hash_function);
		(*eqFuncOids)[i] = eq_function;
		fmgr_info(right_hash_function, &(*hashFunctions)[i]);
	}
}


/*****************************************************************************
 *		用于全内存哈希表的支撑例程       
 *
 * 这些例程为元组归组构建哈希表（例如用于哈希       
 * 聚合）。对提交来的每个 not-distinct 元组集合各建一个   
 * 表项。 
 *****************************************************************************/

/*
 * 构建一个空的 TupleHashTable
 *
 *	numCols, keyColIdx：指定用作查找键的元组字段         
 *	eqfunctions：要使用的相等比较函数     
 *	hashfunctions：数据类型专用的哈希函数        
 *	nbuckets：对哈希表大小的初始估计
 *	additionalsize：->additional 中数据的大小   
 *	metacxt：长期分配用内存上下文，但非每表项数据         
 *	tablecxt：用于存储表项的内存上下文         
 *	tempcxt：用于执行哈希与比较函数的短命上下文            
 *
 * 函数数组可用 execTuplesHashPrepare() 构建。注意它们        
 * 不是跨类型函数，而是预期在两侧都看到表的数据   
 * 类型。     
 *
 * 注意 keyColIdx、eqfunctions 与 hashfunctions 必须分配在        
 * 能与哈希表存活一样久的存储中。        
 *
 * LookupTupleHashEntry、FindTupleHashEntry 及相关函数可能         
 * 会在 tempcxt 中泄漏内存。调用者有责任相当频繁地重置   
 * 上下文，通常每个元组一次。（我们这样做，而不是  
 * 在哈希表内部管理额外上下文，因为很多情况下调用     
 * 者反正可以指定一个它本来就要按元组重置的 tempcxt。）
 */
TupleHashTable
BuildTupleHashTableExt(PlanState *parent,
					   TupleDesc inputDesc,
					   int numCols, AttrNumber *keyColIdx,
					   const Oid *eqfuncoids,
					   FmgrInfo *hashfunctions,
					   Oid *collations,
					   long nbuckets, Size additionalsize,
					   MemoryContext metacxt,
					   MemoryContext tablecxt,
					   MemoryContext tempcxt,
					   bool use_variable_hash_iv)
{
	TupleHashTable hashtable;
	Size		entrysize = sizeof(TupleHashEntryData) + additionalsize;
	Size		hash_mem_limit;
	MemoryContext oldcontext;
	bool		allow_jit;

	Assert(nbuckets > 0);

	/* 将初始表大小请求限制为不超过 hash_mem        */
	hash_mem_limit = get_hash_memory_limit() / entrysize;
	if (nbuckets > hash_mem_limit)
		nbuckets = hash_mem_limit;

	oldcontext = MemoryContextSwitchTo(metacxt);

	hashtable = (TupleHashTable) palloc(sizeof(TupleHashTableData));

	hashtable->numCols = numCols;
	hashtable->keyColIdx = keyColIdx;
	hashtable->tab_hash_funcs = hashfunctions;
	hashtable->tab_collations = collations;
	hashtable->tablecxt = tablecxt;
	hashtable->tempcxt = tempcxt;
	hashtable->entrysize = entrysize;
	hashtable->tableslot = NULL;	/* 将在首次查找时进行  */
	hashtable->inputslot = NULL;
	hashtable->in_hash_funcs = NULL;
	hashtable->cur_eq_func = NULL;

	/*
	 * 如果使用并行，即使 leader 后端自己执行扫描，也不  
	 * 希望所有 worker 都以完全相同的方式创建哈希表。由于
	 * 哈希表按键序遍历，若所有进程都以相同方式      
	 * 操作，那么当表大小最初被                          
	 * 低估时，就容易产生"失衡"的                  
	 * 哈希表。   
	 */
	if (use_variable_hash_iv)
		hashtable->hash_iv = murmurhash32(ParallelWorkerNumber);
	else
		hashtable->hash_iv = 0;

	hashtable->hashtab = tuplehash_create(metacxt, nbuckets, hashtable);

	/*
	 * 复制输入元组描述符只是出于安全考虑 --- 我们假定
	 * 所有输入元组均具有等价描述符。 
	 */
	hashtable->tableslot = MakeSingleTupleTableSlot(CreateTupleDescCopy(inputDesc),
													&TTSOpsMinimalTuple);

	/*
	 * 如果使用旧的重置接口（即 BuildTupleHashTable 而非     
	 * BuildTupleHashTableExt），允许 JIT 会导致生成的函数       
	 * a) 存活得比查询更久，b) 在每次重置表时重新生成。  
	 * 因此在这种情况下要阻止 JIT 被使用，办法是不提供   
	 * 父节点（这会阻止访问 EState 中的                      
	 * JitContext）。          
	 */
	allow_jit = metacxt != tablecxt;

	/* 为所有列构建比较器      */
	hashtable->tab_eq_func = ExecBuildGroupingEqual(inputDesc, inputDesc,
													NULL, &TTSOpsMinimalTuple,
													numCols,
													keyColIdx, eqfuncoids, collations,
													allow_jit ? parent : NULL);

	/*
	 * 虽然不美观，但不关闭此上下文而依赖包含它的     
	 * 内存上下文被重置，这是可以的，因为  
	 * ExecBuildGroupingEqual() 只构建非常简单的、调用函数     
	 * 的表达式（即不会使用 RegisterExprContextCallback()）。   
	 */
	hashtable->exprcontext = CreateStandaloneExprContext();

	MemoryContextSwitchTo(oldcontext);

	return hashtable;
}

/*
 * BuildTupleHashTable 是 BuildTupleHashTableExt() 的        
 * 向后兼容包装，它在 tablecxt 中分配哈希表元数据。 
 * 注意：这样创建的哈希表无法配合 ResetTupleHashTable()      
 * 无泄漏地重置。      
 */
TupleHashTable
BuildTupleHashTable(PlanState *parent,
					TupleDesc inputDesc,
					int numCols, AttrNumber *keyColIdx,
					const Oid *eqfuncoids,
					FmgrInfo *hashfunctions,
					Oid *collations,
					long nbuckets, Size additionalsize,
					MemoryContext tablecxt,
					MemoryContext tempcxt,
					bool use_variable_hash_iv)
{
	return BuildTupleHashTableExt(parent,
								  inputDesc,
								  numCols, keyColIdx,
								  eqfuncoids,
								  hashfunctions,
								  collations,
								  nbuckets, additionalsize,
								  tablecxt,
								  tablecxt,
								  tempcxt,
								  use_variable_hash_iv);
}

/*
 * 将哈希表内容重置为空，同时保留所有非内容状态。注意
 * 传给 BuildTupleHashTableExt() 的 tablecxt 也应当被重置，     
 * 否则会产生泄漏。                     
 */
void
ResetTupleHashTable(TupleHashTable hashtable)
{
	tuplehash_reset(hashtable->hashtab);
}

/*
 * 为包含给定元组的元组组查找或创建哈希表表项。 
 * 它必须与哈希表项具有相同的类型。                       
 *
 * 如果 isnew 为 NULL，我们不会创建新表项；若找不到  
 * 则返回 NULL 
 *
 * 如果 hash 不是 NULL，我们把它设为计算出的哈希值。这 
 * 样即使没有返回表项，调用者能拿到哈希值。  
 *
 * 如果 isnew 不是 NULL，那么在没有已有表项匹配时会创
 * 建新表项。返回时，若表项是新建的，则 *isnew 为 
 * true；若已存在则为 false。新表项的 ->additional_data    
 * 已清零。
 */
TupleHashEntry
LookupTupleHashEntry(TupleHashTable hashtable, TupleTableSlot *slot,
					 bool *isnew, uint32 *hash)
{
	TupleHashEntry entry;
	MemoryContext oldContext;
	uint32		local_hash;

	/* 需要在短命上下文中运行哈希函数         */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	/* 设置哈希与匹配函数所需的数据     */
	hashtable->inputslot = slot;
	hashtable->in_hash_funcs = hashtable->tab_hash_funcs;
	hashtable->cur_eq_func = hashtable->tab_eq_func;

	local_hash = TupleHashTableHash_internal(hashtable->hashtab, NULL);
	entry = LookupTupleHashEntry_internal(hashtable, slot, isnew, local_hash);

	if (hash != NULL)
		*hash = local_hash;

	Assert(entry == NULL || entry->hash == local_hash);

	MemoryContextSwitchTo(oldContext);

	return entry;
}

/*
 * 计算一个元组的哈希值    
 */
uint32
TupleHashTableHash(TupleHashTable hashtable, TupleTableSlot *slot)
{
	MemoryContext oldContext;
	uint32		hash;

	hashtable->inputslot = slot;
	hashtable->in_hash_funcs = hashtable->tab_hash_funcs;

	/* 需要在短命上下文中运行哈希函数         */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	hash = TupleHashTableHash_internal(hashtable->hashtab, NULL);

	MemoryContextSwitchTo(oldContext);

	return hash;
}

/*
 * LookupTupleHashEntry 的变体，供已计算出哈希值的            
 * 调用者使用
 */
TupleHashEntry
LookupTupleHashEntryHash(TupleHashTable hashtable, TupleTableSlot *slot,
						 bool *isnew, uint32 hash)
{
	TupleHashEntry entry;
	MemoryContext oldContext;

	/* 需要在短命上下文中运行哈希函数         */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	/* 设置哈希与匹配函数所需的数据     */
	hashtable->inputslot = slot;
	hashtable->in_hash_funcs = hashtable->tab_hash_funcs;
	hashtable->cur_eq_func = hashtable->tab_eq_func;

	entry = LookupTupleHashEntry_internal(hashtable, slot, isnew, hash);
	Assert(entry == NULL || entry->hash == hash);

	MemoryContextSwitchTo(oldContext);

	return entry;
}

/*
 * 搜索与给定元组匹配的哈希表表项。若没有匹配， 
 * 则不会创建表项。本函数类似于 LookupTupleHashEntry     
 * 的不创建表项情形，但支持跨类型比较，此时    
 * 给定元组与表项的类型并不相同。调用者必须        
 * 提供用于输入元组的哈希函数，以及相等函数，      
 * 因为这些函数可能与表项自身的内部函数                
 * 并不相同。                               
 */
TupleHashEntry
FindTupleHashEntry(TupleHashTable hashtable, TupleTableSlot *slot,
				   ExprState *eqcomp,
				   FmgrInfo *hashfunctions)
{
	TupleHashEntry entry;
	MemoryContext oldContext;
	MinimalTuple key;

	/* 需要在短命上下文中运行哈希函数         */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	/* 设置哈希与匹配函数所需的数据     */
	hashtable->inputslot = slot;
	hashtable->in_hash_funcs = hashfunctions;
	hashtable->cur_eq_func = eqcomp;

	/* 搜索哈希表       */
	key = NULL;					/* 引用 inputslot 的标志  */
	entry = tuplehash_lookup(hashtable->hashtab, key);
	MemoryContextSwitchTo(oldContext);

	return entry;
}

/*
 * 如果元组为 NULL，则改用输入槽。此约定避免了把虚拟 
 * 输入元组实体化，除非它们确实需要被                      
 * 复制进表中。    
 *
 * 另外，调用者必须为哈希函数的运行选择合适的       
 * 内存上下文。（dynahash.c 不会改变 CurrentMemoryContext） 
 */
static uint32
TupleHashTableHash_internal(struct tuplehash_hash *tb,
							const MinimalTuple tuple)
{
	TupleHashTable hashtable = (TupleHashTable) tb->private_data;
	int			numCols = hashtable->numCols;
	AttrNumber *keyColIdx = hashtable->keyColIdx;
	uint32		hashkey = hashtable->hash_iv;
	TupleTableSlot *slot;
	FmgrInfo   *hashfunctions;
	int			i;

	if (tuple == NULL)
	{
		/* 为表处理当前输入元组                */
		slot = hashtable->inputslot;
		hashfunctions = hashtable->in_hash_funcs;
	}
	else
	{
		/*
		 * 处理已存储在表中的元组。        
		 *
		 * （由于 simplehash.h 的使用方式，加上哈希值       
		 * 存于表项中，实际不会发生）          
		 */
		slot = hashtable->tableslot;
		ExecStoreMinimalTuple(tuple, slot, false);
		hashfunctions = hashtable->tab_hash_funcs;
	}

	for (i = 0; i < numCols; i++)
	{
		AttrNumber	att = keyColIdx[i];
		Datum		attr;
		bool		isNull;

		/* 通过旋转合并相继的哈希键    */
		hashkey = pg_rotate_left32(hashkey, 1);

		attr = slot_getattr(slot, att, &isNull);

		if (!isNull)			/* 将空值视为哈希键 0       */
		{
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[i],
													hashtable->tab_collations[i],
													attr));
			hashkey ^= hkey;
		}
	}

	/*
	 * 上面这种哈希组合方式，无论哈希之间还是与 IV，  
	 * 都达不到良好的位扰动。IV 的目标正是实现该效果，
	 * 因此要对合并后的哈希值再执行一轮哈希，      
	 * 从而获得近乎完美的扰动。   
	 */
	return murmurhash32(hashkey);
}

/*
 * 完成 LookupTupleHashEntry 与 LookupTupleHashEntryHash 的              
 * 工作。这样可避免为 LookupTupleHashEntry 多次切换内存 
 * 上下文。         
 *
 * 注意：本函数可能切换内存上下文，也可能不切换。
 * 调用者应自行切回。
 */
static inline TupleHashEntry
LookupTupleHashEntry_internal(TupleHashTable hashtable, TupleTableSlot *slot,
							  bool *isnew, uint32 hash)
{
	TupleHashEntryData *entry;
	bool		found;
	MinimalTuple key;

	key = NULL;					/* 引用 inputslot 的标志  */

	if (isnew)
	{
		entry = tuplehash_insert_hash(hashtable->hashtab, key, hash, &found);

		if (found)
		{
			/* 找到已有表项       */
			*isnew = false;
		}
		else
		{
			/* 创建了新表项*/
			*isnew = true;
			/* 清零数据     */
			entry->additional = NULL;
			MemoryContextSwitchTo(hashtable->tablecxt);
			/* 把第一个元组复制到表上下文中  */
			entry->firstTuple = ExecCopySlotMinimalTuple(slot);
		}
	}
	else
	{
		entry = tuplehash_lookup_hash(hashtable->hashtab, key, hash);
	}

	return entry;
}

/*
 * 查看两个（假定哈希值相同的）元组是否匹配    
 */
static int
TupleHashTableMatch(struct tuplehash_hash *tb, const MinimalTuple tuple1, const MinimalTuple tuple2)
{
	TupleTableSlot *slot1;
	TupleTableSlot *slot2;
	TupleHashTable hashtable = (TupleHashTable) tb->private_data;
	ExprContext *econtext = hashtable->exprcontext;

	/*
	 * 我们假定 simplehash.h 调用本函数时，首参总是      
	 * 真正的表项，而第二个实参是 LookupTupleHashEntry 的   
	 * 哑 TupleHashEntryData。反方向本来也可以支持，但目前  
	 * 并无此需求。                                    
	 */
	Assert(tuple1 != NULL);
	slot1 = hashtable->tableslot;
	ExecStoreMinimalTuple(tuple1, slot1, false);
	Assert(tuple2 == NULL);
	slot2 = hashtable->inputslot;

	/* 对于跨类型比较，inputslot 必须在前         */
	econtext->ecxt_innertuple = slot2;
	econtext->ecxt_outertuple = slot1;
	return !ExecQualAndReset(hashtable->cur_eq_func, econtext);
}
