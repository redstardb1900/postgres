/*-------------------------------------------------------------------------
 *
 * execTuples.c
 *	  处理 TupleTableSlot 的例程。它们用于与元组相关联的
 *	  资源管理（例如释放磁盘缓冲内元组的缓冲区    
 *	  pin，或释放瞬态元组占用的内存）。槽还提供访问 
 *	  抽象层，让我们能实现"virtual"（虚拟）元组来降低    
 *	  数据拷贝的开销。                         
 *
 *	  处理元组类型信息的例程。当前，                    
 *	  一个元组的类型信息是 FormData_pg_attribute 数组。        
 *	  操纵元组的例程需要这些信息（getattribute、 
 *	  formtuple 等）。             
 *
 *
 *	 表例程的工作方式示例    
 *		假设有形如 SELECT emp.name FROM emp 的查询，并且查询   
 *		计划中只有一个 SeqScan 节点。 
 *
 *		ExecutorStart():  
 *		----------------
 *
 *		- ExecInitSeqScan() 调用 ExecInitScanTupleSlot() 来构造    
 *		  TupleTableSlot，用于存放访问方法返回的元组，并   
 *		  ExecInitResultTypeTL() 来定义节点的返回   
 *		  类型。ExecAssignScanProjectionInfo() 会在必要时创建  
 *		  另一个 TupleTableSlot，用于存放执行目标列表投影
 *		  所产生的元组。   
 *
 *		ExecutorRun() 期间
 *		----------------
 *		- SeqNext() 调用 ExecStoreBufferHeapTuple() 把访问方法  
 *		  返回的元组放入扫描元组槽中。             
 *
 *		- ExecSeqScan()（经由 ExecScan）在必要时调用            
 *		  ExecProject()，将投影结果放入结果元组槽。若无    
 *		  必要则直接返回 SeqNext() 返回的槽。                   
 *
 *		- ExecutePlan() 调用输出函数。     
 *
 *		执行器代码中需要留意的一个重要之处是：传递  
 *		的是指向含元组的槽的指针，而非元组本身。   
 *		这便于相关信息的传递（例如某个元组是否应被       
 *		pfree、哪个缓冲区包含该元组、元组的元组描述符     
 *		等）。它还让我们能在很多情况下避免物理地      
 *		构建投影元组。                                            
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execTuples.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

static TupleDesc ExecTypeFromTLInternal(List *targetList,
										bool skipjunk);
static pg_attribute_always_inline void slot_deform_heap_tuple(TupleTableSlot *slot, HeapTuple tuple, uint32 *offp,
															  int natts);
static inline void tts_buffer_heap_store_tuple(TupleTableSlot *slot,
											   HeapTuple tuple,
											   Buffer buffer,
											   bool transfer_pin);
static void tts_heap_store_tuple(TupleTableSlot *slot, HeapTuple tuple, bool shouldFree);


const TupleTableSlotOps TTSOpsVirtual;
const TupleTableSlotOps TTSOpsHeapTuple;
const TupleTableSlotOps TTSOpsMinimalTuple;
const TupleTableSlotOps TTSOpsBufferHeapTuple;


/*
 * 各 TupleTableSlotOps 的实现。
 */

/*
 * VirtualTupleTableSlot 的 TupleTableSlotOps 实现。      
 */
static void
tts_virtual_init(TupleTableSlot *slot)
{
}

static void
tts_virtual_release(TupleTableSlot *slot)
{
}

static void
tts_virtual_clear(TupleTableSlot *slot)
{
	if (unlikely(TTS_SHOULDFREE(slot)))
	{
		VirtualTupleTableSlot *vslot = (VirtualTupleTableSlot *) slot;

		pfree(vslot->data);
		vslot->data = NULL;

		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
}

/*
 * VirtualTupleTableSlot 总有完全填充的 tts_values、        
 * tts_isnull 数组。所以永远不应调用此函数。    
 */
static void
tts_virtual_getsomeattrs(TupleTableSlot *slot, int natts)
{
	elog(ERROR, "getsomeattrs is not required to be called on a virtual tuple table slot");
}

/*
 * VirtualTupleTableSlot 不提供系统属性（除按通用方式    
 * 已通用处理的除外，如 tableoid）。我们一般不应到 
 * 达这里，但仍会提供用户友好消息。   
 */
static Datum
tts_virtual_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	Assert(!TTS_EMPTY(slot));

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot retrieve a system column in this context")));

	return 0;					/* 消除编译器警告     */
}

/*
 * 要物化虚拟槽，所有非按值传递的 datum 都必须拷贝     
 * 到槽的内存上下文中。为此，先计算所需的大小，并   
 * 分配足够的内存来存储所有属性。这样做有利于           
 * 缓存命中率，但更重要的是只需内存的分配与       
 * 释放操作。         
 */
static void
tts_virtual_materialize(TupleTableSlot *slot)
{
	VirtualTupleTableSlot *vslot = (VirtualTupleTableSlot *) slot;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	Size		sz = 0;
	char	   *data;

	/* 已经物化         */
	if (TTS_SHOULDFREE(slot))
		return;

	/* 计算所需的内存大小     */
	for (int natt = 0; natt < desc->natts; natt++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;

		val = slot->tts_values[natt];

		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			/*
			 * 我们想要扁平化该展开值，使物化后的槽不     
			 * 再依赖它。           
			 */
			sz = att_align_nominal(sz, att->attalign);
			sz += EOH_get_flat_size(DatumGetEOHP(val));
		}
		else
		{
			sz = att_align_nominal(sz, att->attalign);
			sz = att_addlength_datum(sz, att->attlen, val);
		}
	}

	/* 全部按值传递*/
	if (sz == 0)
		return;

	/* 分配内存    */
	vslot->data = data = MemoryContextAlloc(slot->tts_mcxt, sz);
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	/* 然后把全部属性拷贝到预分配的空间中  */
	for (int natt = 0; natt < desc->natts; natt++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;

		val = slot->tts_values[natt];

		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			Size		data_length;

			/*
			 * 我们想要扁平化该展开值，使物化后的槽不     
			 * 再依赖它。           
			 */
			ExpandedObjectHeader *eoh = DatumGetEOHP(val);

			data = (char *) att_align_nominal(data,
											  att->attalign);
			data_length = EOH_get_flat_size(eoh);
			EOH_flatten_into(eoh, data, data_length);

			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
		else
		{
			Size		data_length = 0;

			data = (char *) att_align_nominal(data, att->attalign);
			data_length = att_addlength_datum(data_length, att->attlen, val);

			memcpy(data, DatumGetPointer(val), data_length);

			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
	}
}

static void
tts_virtual_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	TupleDesc	srcdesc = srcslot->tts_tupleDescriptor;

	Assert(srcdesc->natts <= dstslot->tts_tupleDescriptor->natts);

	tts_virtual_clear(dstslot);

	slot_getallattrs(srcslot);

	for (int natt = 0; natt < srcdesc->natts; natt++)
	{
		dstslot->tts_values[natt] = srcslot->tts_values[natt];
		dstslot->tts_isnull[natt] = srcslot->tts_isnull[natt];
	}

	dstslot->tts_nvalid = srcdesc->natts;
	dstslot->tts_flags &= ~TTS_FLAG_EMPTY;

	/* 确保存储不再依赖外部内存                */
	tts_virtual_materialize(dstslot);
}

static HeapTuple
tts_virtual_copy_heap_tuple(TupleTableSlot *slot)
{
	Assert(!TTS_EMPTY(slot));

	return heap_form_tuple(slot->tts_tupleDescriptor,
						   slot->tts_values,
						   slot->tts_isnull);
}

static MinimalTuple
tts_virtual_copy_minimal_tuple(TupleTableSlot *slot)
{
	Assert(!TTS_EMPTY(slot));

	return heap_form_minimal_tuple(slot->tts_tupleDescriptor,
								   slot->tts_values,
								   slot->tts_isnull);
}


/*
 * HeapTupleTableSlot 的 TupleTableSlotOps 实现。      
 */

static void
tts_heap_init(TupleTableSlot *slot)
{
}

static void
tts_heap_release(TupleTableSlot *slot)
{
}

static void
tts_heap_clear(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	/* 如果允许，就释放堆元组的内存。       */
	if (TTS_SHOULDFREE(slot))
	{
		heap_freetuple(hslot->tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	hslot->off = 0;
	hslot->tuple = NULL;
}

static void
tts_heap_getsomeattrs(TupleTableSlot *slot, int natts)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	slot_deform_heap_tuple(slot, hslot->tuple, &hslot->off, natts);
}

static Datum
tts_heap_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	/*
	 * 在某些代码路径下，可能带着未物化的槽到达此处  
	 * 此时便无法检索系统列。                    
	 */
	if (!hslot->tuple)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot retrieve a system column in this context")));

	return heap_getsysattr(hslot->tuple, attnum,
						   slot->tts_tupleDescriptor, isnull);
}

static void
tts_heap_materialize(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	MemoryContext oldContext;

	Assert(!TTS_EMPTY(slot));

	/* 如果槽的元组已物化，则无事可做。           */
	if (TTS_SHOULDFREE(slot))
		return;

	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);

	/*
	 * 必须从头开始反解析，否则 tts_values[] 的条目可能指向
	 * 未物化的元组（访问时它可能已经消失）。           
	 */
	slot->tts_nvalid = 0;
	hslot->off = 0;

	if (!hslot->tuple)
		hslot->tuple = heap_form_tuple(slot->tts_tupleDescriptor,
									   slot->tts_values,
									   slot->tts_isnull);
	else
	{
		/*
		 * 该槽所包含的元组并没有分配在给定槽的内存   
		 * 上下文中（否则它就会设置 TTS_FLAG_SHOULDFREE）。   
		 * 请把该元组拷贝到给定槽的内存上下文中。  
		 */
		hslot->tuple = heap_copytuple(hslot->tuple);
	}

	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	MemoryContextSwitchTo(oldContext);
}

static void
tts_heap_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	HeapTuple	tuple;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	tuple = ExecCopySlotHeapTuple(srcslot);
	MemoryContextSwitchTo(oldcontext);

	ExecStoreHeapTuple(tuple, dstslot, true);
}

static HeapTuple
tts_heap_get_heap_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));
	if (!hslot->tuple)
		tts_heap_materialize(slot);

	return hslot->tuple;
}

static HeapTuple
tts_heap_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));
	if (!hslot->tuple)
		tts_heap_materialize(slot);

	return heap_copytuple(hslot->tuple);
}

static MinimalTuple
tts_heap_copy_minimal_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	if (!hslot->tuple)
		tts_heap_materialize(slot);

	return minimal_tuple_from_heap_tuple(hslot->tuple);
}

static void
tts_heap_store_tuple(TupleTableSlot *slot, HeapTuple tuple, bool shouldFree)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	tts_heap_clear(slot);

	slot->tts_nvalid = 0;
	hslot->tuple = tuple;
	hslot->off = 0;
	slot->tts_flags &= ~(TTS_FLAG_EMPTY | TTS_FLAG_SHOULDFREE);
	slot->tts_tid = tuple->t_self;

	if (shouldFree)
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
}


/*
 * MinimalTupleTableSlot 的 TupleTableSlotOps 实现。      
 */

static void
tts_minimal_init(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	/*
	 * 初始化堆元组指针，以便像访问堆元组那样访问槽内
	 * 最小元组的属性。                           
	 */
	mslot->tuple = &mslot->minhdr;
}

static void
tts_minimal_release(TupleTableSlot *slot)
{
}

static void
tts_minimal_clear(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (TTS_SHOULDFREE(slot))
	{
		heap_free_minimal_tuple(mslot->mintuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	mslot->off = 0;
	mslot->mintuple = NULL;
}

static void
tts_minimal_getsomeattrs(TupleTableSlot *slot, int natts)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	slot_deform_heap_tuple(slot, mslot->tuple, &mslot->off, natts);
}

static Datum
tts_minimal_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	Assert(!TTS_EMPTY(slot));

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot retrieve a system column in this context")));

	return 0;					/* 消除编译器警告     */
}

static void
tts_minimal_materialize(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;
	MemoryContext oldContext;

	Assert(!TTS_EMPTY(slot));

	/* 如果槽的元组已物化，则无事可做。           */
	if (TTS_SHOULDFREE(slot))
		return;

	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);

	/*
	 * 必须从头开始反解析，否则 tts_values[] 的条目可能指向
	 * 未物化的元组（访问时它可能已经消失）。           
	 */
	slot->tts_nvalid = 0;
	mslot->off = 0;

	if (!mslot->mintuple)
	{
		mslot->mintuple = heap_form_minimal_tuple(slot->tts_tupleDescriptor,
												  slot->tts_values,
												  slot->tts_isnull);
	}
	else
	{
		/*
		 * 该槽所包含的最小元组并未分配在给定槽的       
		 * 内存上下文中（否则它就会设置          
		 * TTS_FLAG_SHOULDFREE）。请把最小元组拷贝到给定      
		 * 槽的内存上下文 
		 */
		mslot->mintuple = heap_copy_minimal_tuple(mslot->mintuple);
	}

	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	Assert(mslot->tuple == &mslot->minhdr);

	mslot->minhdr.t_len = mslot->mintuple->t_len + MINIMAL_TUPLE_OFFSET;
	mslot->minhdr.t_data = (HeapTupleHeader) ((char *) mslot->mintuple - MINIMAL_TUPLE_OFFSET);

	MemoryContextSwitchTo(oldContext);
}

static void
tts_minimal_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	MemoryContext oldcontext;
	MinimalTuple mintuple;

	oldcontext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	mintuple = ExecCopySlotMinimalTuple(srcslot);
	MemoryContextSwitchTo(oldcontext);

	ExecStoreMinimalTuple(mintuple, dstslot, true);
}

static MinimalTuple
tts_minimal_get_minimal_tuple(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (!mslot->mintuple)
		tts_minimal_materialize(slot);

	return mslot->mintuple;
}

static HeapTuple
tts_minimal_copy_heap_tuple(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (!mslot->mintuple)
		tts_minimal_materialize(slot);

	return heap_tuple_from_minimal_tuple(mslot->mintuple);
}

static MinimalTuple
tts_minimal_copy_minimal_tuple(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (!mslot->mintuple)
		tts_minimal_materialize(slot);

	return heap_copy_minimal_tuple(mslot->mintuple);
}

static void
tts_minimal_store_tuple(TupleTableSlot *slot, MinimalTuple mtup, bool shouldFree)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	tts_minimal_clear(slot);

	Assert(!TTS_SHOULDFREE(slot));
	Assert(TTS_EMPTY(slot));

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = 0;
	mslot->off = 0;

	mslot->mintuple = mtup;
	Assert(mslot->tuple == &mslot->minhdr);
	mslot->minhdr.t_len = mtup->t_len + MINIMAL_TUPLE_OFFSET;
	mslot->minhdr.t_data = (HeapTupleHeader) ((char *) mtup - MINIMAL_TUPLE_OFFSET);
	/* 由于不允许访问，所以无需设置 t_self 或 t_tableOid*/

	if (shouldFree)
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
}


/*
 * BufferHeapTupleTableSlot 的 TupleTableSlotOps 实现。      
 */

static void
tts_buffer_heap_init(TupleTableSlot *slot)
{
}

static void
tts_buffer_heap_release(TupleTableSlot *slot)
{
}

static void
tts_buffer_heap_clear(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	/*
	 * 如果允许，释放堆元组的内存。来自缓冲区的元组   
	 * 永远无法释放。但我们可能已物化过来自缓冲区的   
	 * 元组，可释放它。  
	 */
	if (TTS_SHOULDFREE(slot))
	{
		/* 物化元组时就应已解除对该缓冲区的 pin。           */
		Assert(!BufferIsValid(bslot->buffer));

		heap_freetuple(bslot->base.tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	if (BufferIsValid(bslot->buffer))
		ReleaseBuffer(bslot->buffer);

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	bslot->base.tuple = NULL;
	bslot->base.off = 0;
	bslot->buffer = InvalidBuffer;
}

static void
tts_buffer_heap_getsomeattrs(TupleTableSlot *slot, int natts)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	slot_deform_heap_tuple(slot, bslot->base.tuple, &bslot->base.off, natts);
}

static Datum
tts_buffer_heap_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	/*
	 * 在某些代码路径下，可能带着未物化的槽到达此处  
	 * 此时便无法检索系统列。                    
	 */
	if (!bslot->base.tuple)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot retrieve a system column in this context")));

	return heap_getsysattr(bslot->base.tuple, attnum,
						   slot->tts_tupleDescriptor, isnull);
}

static void
tts_buffer_heap_materialize(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	MemoryContext oldContext;

	Assert(!TTS_EMPTY(slot));

	/* 如果槽的元组已物化，则无事可做。           */
	if (TTS_SHOULDFREE(slot))
		return;

	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);

	/*
	 * 必须从头开始反解析，否则 tts_values[] 的条目可能指向
	 * 未物化的元组（访问时它可能已经消失）。           
	 */
	bslot->base.off = 0;
	slot->tts_nvalid = 0;

	if (!bslot->base.tuple)
	{
		/*
		 * 通常 BufferHeapTupleTableSlot 应带元组和缓冲区，   
		 * 与之关联，除非已物化（那会在上方返回）。
		 * 但有时允许把虚拟元组存入缓冲区槽中会很有  
		 * 用，此时该槽还需要能够被                
		 * 物化。      
		 */
		bslot->base.tuple = heap_form_tuple(slot->tts_tupleDescriptor,
											slot->tts_values,
											slot->tts_isnull);
	}
	else
	{
		bslot->base.tuple = heap_copytuple(bslot->base.tuple);

		/*
		 * 存储在 BufferHeapTupleTableSlot 中的堆元组应当有    
		 * 关联的缓冲区，除非它已物化或为虚拟元组。   
		 */
		if (likely(BufferIsValid(bslot->buffer)))
			ReleaseBuffer(bslot->buffer);
		bslot->buffer = InvalidBuffer;
	}

	/*
	 * 我们先释放缓冲区（如果有），然后才设置            
	 * TTS_FLAG_SHOULDFREE。这避免了出现一种会违反我们         
	 * 断言的瞬态状态：带 TTS_FLAG_SHOULDFREE 的槽不拥         
	 * 有缓冲区。万一上方的 ReleaseBuffer() 出错，我们会
	 * 实际泄漏被拷贝的元组，但这看起来基本无害。   
	 */
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	MemoryContextSwitchTo(oldContext);
}

static void
tts_buffer_heap_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	BufferHeapTupleTableSlot *bsrcslot = (BufferHeapTupleTableSlot *) srcslot;
	BufferHeapTupleTableSlot *bdstslot = (BufferHeapTupleTableSlot *) dstslot;

	/*
	 * 如果源槽是不同类型的槽，或者是已物化/虚拟的缓    
	 * 冲区槽，就为该元组创建一份新拷贝。否则就创建     
	 * 缓冲区内元组的一个新引用。     
	 */
	if (dstslot->tts_ops != srcslot->tts_ops ||
		TTS_SHOULDFREE(srcslot) ||
		!bsrcslot->base.tuple)
	{
		MemoryContext oldContext;

		ExecClearTuple(dstslot);
		dstslot->tts_flags &= ~TTS_FLAG_EMPTY;
		oldContext = MemoryContextSwitchTo(dstslot->tts_mcxt);
		bdstslot->base.tuple = ExecCopySlotHeapTuple(srcslot);
		dstslot->tts_flags |= TTS_FLAG_SHOULDFREE;
		MemoryContextSwitchTo(oldContext);
	}
	else
	{
		Assert(BufferIsValid(bsrcslot->buffer));

		tts_buffer_heap_store_tuple(dstslot, bsrcslot->base.tuple,
									bsrcslot->buffer, false);

		/*
		 * 源元组中 HeapTupleData 部分的寿命可能比目标槽  
		 * 更短命。因此要把该 HeapTuple 拷贝到本槽的          
		 * tupdata 中，那里保证有足够长的寿命（但它仍     
		 * 会指向缓冲区）。          
		 */
		memcpy(&bdstslot->base.tupdata, bdstslot->base.tuple, sizeof(HeapTupleData));
		bdstslot->base.tuple = &bdstslot->base.tupdata;
	}
}

static HeapTuple
tts_buffer_heap_get_heap_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tts_buffer_heap_materialize(slot);

	return bslot->base.tuple;
}

static HeapTuple
tts_buffer_heap_copy_heap_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tts_buffer_heap_materialize(slot);

	return heap_copytuple(bslot->base.tuple);
}

static MinimalTuple
tts_buffer_heap_copy_minimal_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tts_buffer_heap_materialize(slot);

	return minimal_tuple_from_heap_tuple(bslot->base.tuple);
}

static inline void
tts_buffer_heap_store_tuple(TupleTableSlot *slot, HeapTuple tuple,
							Buffer buffer, bool transfer_pin)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	if (TTS_SHOULDFREE(slot))
	{
		/* 已物化的槽不应有待释放的缓冲区        */
		Assert(!BufferIsValid(bslot->buffer));

		heap_freetuple(bslot->base.tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = 0;
	bslot->base.tuple = tuple;
	bslot->base.off = 0;
	slot->tts_tid = tuple->t_self;

	/*
	 * 如果元组位于磁盘页上，那么只要我们还持有指      
	 * 向它的指针，就要保持该页被 pin 住。假定调用    
	 * 者已持有这样的 pin。若 transfer_pin 为真则转移      
	 * 该 pin，否则重新 pin。
	 *
	 * 这段代码专门优化这样一种情形：槽之前持有的正 
	 * 是同一磁盘页上的元组：此时释放再重新获取        
	 * pin 属于浪费。在顺序扫描期间这种情况很常见，
	 * 所以值得为此费心。            
	 */
	if (bslot->buffer != buffer)
	{
		if (BufferIsValid(bslot->buffer))
			ReleaseBuffer(bslot->buffer);

		bslot->buffer = buffer;

		if (!transfer_pin && BufferIsValid(buffer))
			IncrBufferRefCount(buffer);
	}
	else if (transfer_pin && BufferIsValid(buffer))
	{
		/*
		 * 在 transfer_pin 模式下，调用者不会知道同一页优
		 * 化，因此我们得释放它的 pin。  
		 */
		ReleaseBuffer(buffer);
	}
}

/*
 * slot_deform_heap_tuple
 *		给定一个 TupleTableSlot，把槽的物理元组中的数据    
 *		提取到它的 Datum/isnull 数组中。数据一直提取到 
 *		第 natts 列（调用者必须确保这是合法的列号）。   
 *
 *		这本质上是 heap_deform_tuple 的一个增量版本：      
 *		每次调用时我们只提取到所需属性为止，而不会 
 *		重新计算先前已提取属性的信息。                  
 *		slot->tts_nvalid 是已经提取的属性数量。             
 *
 * 此函数被标记为始终内联，这样不同类型槽的不同        
 * offp 就会被优化掉。   
 */
static pg_attribute_always_inline void
slot_deform_heap_tuple(TupleTableSlot *slot, HeapTuple tuple, uint32 *offp,
					   int natts)
{
	TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
	Datum	   *values = slot->tts_values;
	bool	   *isnull = slot->tts_isnull;
	HeapTupleHeader tup = tuple->t_data;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	int			attnum;
	char	   *tp;				/* 元组数据指针*/
	uint32		off;			/* 元组数据内偏移*/
	bits8	   *bp = tup->t_bits;	/* 元组内 null 位图指针 */
	bool		slow;			/* 可否用 attcacheoff？    */

	/* 我们最多只能取出元组所拥有的属性。    */
	natts = Min(HeapTupleHeaderGetNatts(tuple->t_data), natts);

	/*
	 * 检查这是否为该元组首次调用，并初始化或恢复循环 
	 * 状态。  
	 */
	attnum = slot->tts_nvalid;
	if (attnum == 0)
	{
		/* 从第一个属性开始       */
		off = 0;
		slow = false;
	}
	else
	{
		/* 从上一次执行恢复状态        */
		off = *offp;
		slow = TTS_SLOW(slot);
	}

	tp = (char *) tup + tup->t_hoff;

	for (; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = TupleDescAttr(tupleDesc, attnum);

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* 无法再使用 attcacheoff   */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else if (thisatt->attlen == -1)
		{
			/*
			 * 只有当偏移已经恰当对齐时，我们才能缓存  
			 * varlena 属性的偏移量，这样任何情况下都不会  
			 * 有填充字节：此时偏移量对对齐或非对齐的值   
			 * 都有效。                  
			 */
			if (!slow &&
				off == att_align_nominal(off, thisatt->attalign))
				thisatt->attcacheoff = off;
			else
			{
				off = att_align_pointer(off, thisatt->attalign, -1,
										tp + off);
				slow = true;
			}
		}
		else
		{
			/* 非 varlena，可安全用 att_align_nominal  */
			off = att_align_nominal(off, thisatt->attalign);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength_pointer(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* 无法再使用 attcacheoff   */
	}

	/*
	 * 为下次执行保存状态  
	 */
	slot->tts_nvalid = attnum;
	*offp = off;
	if (slow)
		slot->tts_flags |= TTS_FLAG_SLOW;
	else
		slot->tts_flags &= ~TTS_FLAG_SLOW;
}


const TupleTableSlotOps TTSOpsVirtual = {
	.base_slot_size = sizeof(VirtualTupleTableSlot),
	.init = tts_virtual_init,
	.release = tts_virtual_release,
	.clear = tts_virtual_clear,
	.getsomeattrs = tts_virtual_getsomeattrs,
	.getsysattr = tts_virtual_getsysattr,
	.materialize = tts_virtual_materialize,
	.copyslot = tts_virtual_copyslot,

	/*
	 * 虚拟元组表槽无法"拥有"堆元组或者最小             
	 * 元组
	 */
	.get_heap_tuple = NULL,
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_virtual_copy_heap_tuple,
	.copy_minimal_tuple = tts_virtual_copy_minimal_tuple
};

const TupleTableSlotOps TTSOpsHeapTuple = {
	.base_slot_size = sizeof(HeapTupleTableSlot),
	.init = tts_heap_init,
	.release = tts_heap_release,
	.clear = tts_heap_clear,
	.getsomeattrs = tts_heap_getsomeattrs,
	.getsysattr = tts_heap_getsysattr,
	.materialize = tts_heap_materialize,
	.copyslot = tts_heap_copyslot,
	.get_heap_tuple = tts_heap_get_heap_tuple,

	/* 堆元组表槽无法"拥有"最小元组。           */
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_heap_copy_heap_tuple,
	.copy_minimal_tuple = tts_heap_copy_minimal_tuple
};

const TupleTableSlotOps TTSOpsMinimalTuple = {
	.base_slot_size = sizeof(MinimalTupleTableSlot),
	.init = tts_minimal_init,
	.release = tts_minimal_release,
	.clear = tts_minimal_clear,
	.getsomeattrs = tts_minimal_getsomeattrs,
	.getsysattr = tts_minimal_getsysattr,
	.materialize = tts_minimal_materialize,
	.copyslot = tts_minimal_copyslot,

	/* 最小元组表槽无法"拥有"堆元组。           */
	.get_heap_tuple = NULL,
	.get_minimal_tuple = tts_minimal_get_minimal_tuple,
	.copy_heap_tuple = tts_minimal_copy_heap_tuple,
	.copy_minimal_tuple = tts_minimal_copy_minimal_tuple
};

const TupleTableSlotOps TTSOpsBufferHeapTuple = {
	.base_slot_size = sizeof(BufferHeapTupleTableSlot),
	.init = tts_buffer_heap_init,
	.release = tts_buffer_heap_release,
	.clear = tts_buffer_heap_clear,
	.getsomeattrs = tts_buffer_heap_getsomeattrs,
	.getsysattr = tts_buffer_heap_getsysattr,
	.materialize = tts_buffer_heap_materialize,
	.copyslot = tts_buffer_heap_copyslot,
	.get_heap_tuple = tts_buffer_heap_get_heap_tuple,

	/* 缓冲区堆元组表槽无法"拥有"最小元组。         */
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_buffer_heap_copy_heap_tuple,
	.copy_minimal_tuple = tts_buffer_heap_copy_minimal_tuple
};


/* ----------------------------------------------------------------
 *				  元组表的创建/删除函数    
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		MakeTupleTableSlot
 *
 *		构建给定类型空槽的基础例程，其类型由
 *		TupleTableSlotType 指定。若给定了 tupleDesc，则槽的        
 *		描述符终身固定，可因此获得效率。如果那 
 *		不可取，传 NULL。
 * --------------------------------
 */
TupleTableSlot *
MakeTupleTableSlot(TupleDesc tupleDesc,
				   const TupleTableSlotOps *tts_ops)
{
	Size		basesz,
				allocsz;
	TupleTableSlot *slot;

	basesz = tts_ops->base_slot_size;

	/*
	 * 当指定了固定描述符时，我们可以通过一次性   
	 * 分配整个槽来减少开销。    
	 */
	if (tupleDesc)
		allocsz = MAXALIGN(basesz) +
			MAXALIGN(tupleDesc->natts * sizeof(Datum)) +
			MAXALIGN(tupleDesc->natts * sizeof(bool));
	else
		allocsz = basesz;

	slot = palloc0(allocsz);
	/* 出于优化目的使用 const，在分配时可以修改        */
	*((const TupleTableSlotOps **) &slot->tts_ops) = tts_ops;
	slot->type = T_TupleTableSlot;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	if (tupleDesc != NULL)
		slot->tts_flags |= TTS_FLAG_FIXED;
	slot->tts_tupleDescriptor = tupleDesc;
	slot->tts_mcxt = CurrentMemoryContext;
	slot->tts_nvalid = 0;

	if (tupleDesc != NULL)
	{
		slot->tts_values = (Datum *)
			(((char *) slot)
			 + MAXALIGN(basesz));
		slot->tts_isnull = (bool *)
			(((char *) slot)
			 + MAXALIGN(basesz)
			 + MAXALIGN(tupleDesc->natts * sizeof(Datum)));

		PinTupleDesc(tupleDesc);
	}

	/*
	 * 再允许特定于槽类型的初始化。  
	 */
	slot->tts_ops->init(slot);

	return slot;
}

/* --------------------------------
 *		ExecAllocTableSlot
 *
 *		在元组表（它只是一个 List）内创建元组表槽。        
 * --------------------------------
 */
TupleTableSlot *
ExecAllocTableSlot(List **tupleTable, TupleDesc desc,
				   const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot = MakeTupleTableSlot(desc, tts_ops);

	*tupleTable = lappend(*tupleTable, slot);

	return slot;
}

/* --------------------------------
 *		ExecResetTupleTable
 *
 *		此例程释放元组表的所有资源（缓冲区 pin、  
 *		tupdesc 引用计数），并可选地释放元组表数据
 *		结构所占用的内存。                
 *		预期该例程由 ExecEndPlan() 调用。                  
 * --------------------------------
 */
void
ExecResetTupleTable(List *tupleTable,	/* 元组表   */
					bool shouldFree)	/* 如果应释放内存则为真*/
{
	ListCell   *lc;

	foreach(lc, tupleTable)
	{
		TupleTableSlot *slot = lfirst_node(TupleTableSlot, lc);

		/* 总是释放资源并把槽重置为空              */
		ExecClearTuple(slot);
		slot->tts_ops->release(slot);
		if (slot->tts_tupleDescriptor)
		{
			ReleaseTupleDesc(slot->tts_tupleDescriptor);
			slot->tts_tupleDescriptor = NULL;
		}

		/* 如果 shouldFree 为真，释放槽本身占用的内存 */
		if (shouldFree)
		{
			if (!TTS_FIXED(slot))
			{
				if (slot->tts_values)
					pfree(slot->tts_values);
				if (slot->tts_isnull)
					pfree(slot->tts_isnull);
			}
			pfree(slot);
		}
	}

	/* 如果 shouldFree，释放 List 结构    */
	if (shouldFree)
		list_free(tupleTable);
}

/* --------------------------------
 *		MakeSingleTupleTableSlot
 *
 *		这是一个便利例程，用于那些需要独立                
 *		TupleTableSlot 而非来自主执行器元组表的情况。它        
 *		会构建一个给定类型的单槽，并将其初始化为使用     
 *		给定元组描述符  
 * --------------------------------
 */
TupleTableSlot *
MakeSingleTupleTableSlot(TupleDesc tupdesc,
						 const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot = MakeTupleTableSlot(tupdesc, tts_ops);

	return slot;
}

/* --------------------------------
 *		ExecDropSingleTupleTableSlot
 *
 *		释放由 MakeSingleTupleTableSlot 创建的                
 *		TupleTableSlot。切勿用于元组表列表内的槽！   
 * --------------------------------
 */
void
ExecDropSingleTupleTableSlot(TupleTableSlot *slot)
{
	/* 这应当与 ExecResetTupleTable 对单个槽的处理一致   */
	Assert(IsA(slot, TupleTableSlot));
	ExecClearTuple(slot);
	slot->tts_ops->release(slot);
	if (slot->tts_tupleDescriptor)
		ReleaseTupleDesc(slot->tts_tupleDescriptor);
	if (!TTS_FIXED(slot))
	{
		if (slot->tts_values)
			pfree(slot->tts_values);
		if (slot->tts_isnull)
			pfree(slot->tts_isnull);
	}
	pfree(slot);
}


/* ----------------------------------------------------------------
 *				  元组表槽访问函数           
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		ExecSetSlotDescriptor
 *
 *		此函数用于设置与该槽元组相关联的元组描述
 *		符。传入的描述符必须拥有至少与槽相同的       
 *		寿命。如果它是引用计数的描述符，那么只要          
 *		槽持有引用，它的引用计数就会被递增，直到         
 *		无引用。
 * --------------------------------
 */
void
ExecSetSlotDescriptor(TupleTableSlot *slot, /* 要修改的槽*/
					  TupleDesc tupdesc)	/* 新的元组描述符*/
{
	Assert(!TTS_FIXED(slot));

	/* 为安全起见，在修改前确保槽为空          */
	ExecClearTuple(slot);

	/*
	 * 释放任何旧的描述符。如果存在旧的 Datum/isnull 数   
	 * 组也一并释放（不费心检查能否重用）。      
	 */
	if (slot->tts_tupleDescriptor)
		ReleaseTupleDesc(slot->tts_tupleDescriptor);

	if (slot->tts_values)
		pfree(slot->tts_values);
	if (slot->tts_isnull)
		pfree(slot->tts_isnull);

	/*
	 * 安装新描述符；若它有引用计数，就递增其计数。
	 */
	slot->tts_tupleDescriptor = tupdesc;
	PinTupleDesc(tupdesc);

	/*
	 * 分配大小合适的 Datum/isnull 数组。它们必须与槽        
	 * 拥有相同的生命周期，所以要在槽自身的上下文中   
	 */
	slot->tts_values = (Datum *)
		MemoryContextAlloc(slot->tts_mcxt, tupdesc->natts * sizeof(Datum));
	slot->tts_isnull = (bool *)
		MemoryContextAlloc(slot->tts_mcxt, tupdesc->natts * sizeof(bool));
}

/* --------------------------------
 *		ExecStoreHeapTuple
 *
 *		此函数用于把一个即时构建的物理元组存入                   
 *		元组表的指定槽。
 *
 *		tuple:	待存元组  
 *		slot:	存放它的 TTSOpsHeapTuple 槽        
 *		shouldFree: 若 ExecClearTuple 用完后应调用 pfree()  
 *					时则为 true。
 *
 * 对于即时构建的元组，shouldFree 通常设置为 'true'。但       
 * 当被引用的元组保存在某个元组表槽中时，它          
 * 可以是 'false'。该槽属于更低层的执行器 Proc 节点。        
 * 在这种情况下，更低层的槽保留所有权，并有责       
 * 任最终释放该元组。使用这种方式时，我们必须          
 * 确保上层 Proc 节点对元组失去兴趣的时间要比             
 * 更低层节点更早！如果不确定，就用 heap_copytuple             
 * 拷贝低层元组，让上层表槽接管该拷贝！          
 *
 * 返回值就是传入的槽指针。            
 *
 * 如果无法保证目标槽是 TTSOpsHeapTuple 类型的                 
 * 用开销更大的 ExecForceStoreHeapTuple()。
 * --------------------------------
 */
TupleTableSlot *
ExecStoreHeapTuple(HeapTuple tuple,
				   TupleTableSlot *slot,
				   bool shouldFree)
{
	/*
	 * 例行检查 
	 */
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	if (unlikely(!TTS_IS_HEAPTUPLE(slot)))
		elog(ERROR, "trying to store a heap tuple into wrong type of slot");
	tts_heap_store_tuple(slot, tuple, shouldFree);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}

/* --------------------------------
 *		ExecStoreBufferHeapTuple
 *
 *		此函数用于把缓冲区中一个磁盘上的物理元组          
 *		存储到元组表的指定槽中。     
 *
 *		tuple:	待存元组  
 *		slot:	存放它的 TTSOpsBufferHeapTuple 槽        
 *		buffer: 磁盘页内元组用磁盘缓冲，否则 InvalidBuffer  
 *
 * 元组表代码会在缓冲区上获取一个 pin，并持有它直      
 * 到该槽被清空为止，元组便不会消失。    
 *
 * 返回值就是传入的槽指针。            
 *
 * 如果无法保证目标槽为 TTSOpsBufferHeapTuple 类型，             
 * 请用开销更大的 ExecForceStoreHeapTuple()。 
 * --------------------------------
 */
TupleTableSlot *
ExecStoreBufferHeapTuple(HeapTuple tuple,
						 TupleTableSlot *slot,
						 Buffer buffer)
{
	/*
	 * 例行检查 
	 */
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(BufferIsValid(buffer));

	if (unlikely(!TTS_IS_BUFFERTUPLE(slot)))
		elog(ERROR, "trying to store an on-disk heap tuple into wrong type of slot");
	tts_buffer_heap_store_tuple(slot, tuple, buffer, false);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}

/*
 * 与 ExecStoreBufferHeapTuple 类似，但转移调用者已                
 * 持有的现有 pin 给槽，即调用者无需且不得释放该 pin。   
 */
TupleTableSlot *
ExecStorePinnedBufferHeapTuple(HeapTuple tuple,
							   TupleTableSlot *slot,
							   Buffer buffer)
{
	/*
	 * 例行检查 
	 */
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(BufferIsValid(buffer));

	if (unlikely(!TTS_IS_BUFFERTUPLE(slot)))
		elog(ERROR, "trying to store an on-disk heap tuple into wrong type of slot");
	tts_buffer_heap_store_tuple(slot, tuple, buffer, true);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}

/*
 * 把最小元组存入 TTSOpsMinimalTuple 类型槽。   
 *
 * 如果无法保证目标槽为 TTSOpsMinimalTuple 类型的槽，       
 * 请用开销更大的 ExecForceStoreMinimalTuple()。 
 */
TupleTableSlot *
ExecStoreMinimalTuple(MinimalTuple mtup,
					  TupleTableSlot *slot,
					  bool shouldFree)
{
	/*
	 * 例行检查 
	 */
	Assert(mtup != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	if (unlikely(!TTS_IS_MINIMALTUPLE(slot)))
		elog(ERROR, "trying to store a minimal tuple into wrong type of slot");
	tts_minimal_store_tuple(slot, mtup, shouldFree);

	return slot;
}

/*
 * 把 HeapTuple 存入任何一种槽中，如有必要就         
 * 转换。 
 */
void
ExecForceStoreHeapTuple(HeapTuple tuple,
						TupleTableSlot *slot,
						bool shouldFree)
{
	if (TTS_IS_HEAPTUPLE(slot))
	{
		ExecStoreHeapTuple(tuple, slot, shouldFree);
	}
	else if (TTS_IS_BUFFERTUPLE(slot))
	{
		MemoryContext oldContext;
		BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

		ExecClearTuple(slot);
		slot->tts_flags &= ~TTS_FLAG_EMPTY;
		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		bslot->base.tuple = heap_copytuple(tuple);
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
		MemoryContextSwitchTo(oldContext);

		if (shouldFree)
			pfree(tuple);
	}
	else
	{
		ExecClearTuple(slot);
		heap_deform_tuple(tuple, slot->tts_tupleDescriptor,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		if (shouldFree)
		{
			ExecMaterializeSlot(slot);
			pfree(tuple);
		}
	}
}

/*
 * 把 MinimalTuple 存入任何一种槽中，如有必要就         
 * 转换。 
 */
void
ExecForceStoreMinimalTuple(MinimalTuple mtup,
						   TupleTableSlot *slot,
						   bool shouldFree)
{
	if (TTS_IS_MINIMALTUPLE(slot))
	{
		tts_minimal_store_tuple(slot, mtup, shouldFree);
	}
	else
	{
		HeapTupleData htup;

		ExecClearTuple(slot);

		htup.t_len = mtup->t_len + MINIMAL_TUPLE_OFFSET;
		htup.t_data = (HeapTupleHeader) ((char *) mtup - MINIMAL_TUPLE_OFFSET);
		heap_deform_tuple(&htup, slot->tts_tupleDescriptor,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		if (shouldFree)
		{
			ExecMaterializeSlot(slot);
			pfree(mtup);
		}
	}
}

/* --------------------------------
 *		ExecStoreVirtualTuple
 *			把槽标记为包含虚拟元组。      
 *
 * 向槽加载虚拟元组数据的协议是：              
 *		* 调用 ExecClearTuple 把槽标记为空。
 *		* 把数据存入 Datum/isnull 数组。  
 *		* 调用 ExecStoreVirtualTuple 标记槽有效。   
 * 这样做有点不干净，但省去了一轮数据拷贝。  
 * --------------------------------
 */
TupleTableSlot *
ExecStoreVirtualTuple(TupleTableSlot *slot)
{
	/*
	 * 例行检查 
	 */
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(TTS_EMPTY(slot));

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = slot->tts_tupleDescriptor->natts;

	return slot;
}

/* --------------------------------
 *		ExecStoreAllNullTuple
 *			设置槽让每一列都包含 NULL。            
 *
 * 乍一听这可能与 ExecClearTuple 很像，但其实完全不   
 * 同：槽最终是满的，而不是空的。        
 * --------------------------------
 */
TupleTableSlot *
ExecStoreAllNullTuple(TupleTableSlot *slot)
{
	/*
	 * 例行检查 
	 */
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	/* 清空所有旧内容  */
	ExecClearTuple(slot);

	/*
	 * 用 NULL 填充虚拟元组的所有列             
	 */
	MemSet(slot->tts_values, 0,
		   slot->tts_tupleDescriptor->natts * sizeof(Datum));
	memset(slot->tts_isnull, true,
		   slot->tts_tupleDescriptor->natts * sizeof(bool));

	return ExecStoreVirtualTuple(slot);
}

/*
 * 把 datum 形式的 HeapTuple 存入槽中。这总是要求      
 * 先反解析它再以虚拟形式存储。  
 *
 * 在该槽被物化之前，槽的内容都依赖于该                
 * datum 
 */
void
ExecStoreHeapTupleDatum(Datum data, TupleTableSlot *slot)
{
	HeapTupleData tuple = {0};
	HeapTupleHeader td;

	td = DatumGetHeapTupleHeader(data);

	tuple.t_len = HeapTupleHeaderGetDatumLength(td);
	tuple.t_self = td->t_ctid;
	tuple.t_data = td;

	ExecClearTuple(slot);

	heap_deform_tuple(&tuple, slot->tts_tupleDescriptor,
					  slot->tts_values, slot->tts_isnull);
	ExecStoreVirtualTuple(slot);
}

/*
 * ExecFetchSlotHeapTuple - 获取代表槽内容的 HeapTuple             
 *
 * 返回的 HeapTuple 会尽可能贴近地代表槽的             
 * 内容。
 *
 * 如果 materialize 为真，槽的内容将与底层存储脱离          
 * （即释放所有缓冲区 pin，并在槽的上下文中分配         
 * 内存）。                     
 *
 * 如果 shouldFree 非 NULL，那么当返回的元组是在调          
 * 用者的内存上下文中分配时，它将被设为真，且必须 
 * 由调用者释放（pfree() 或上下文重置）。   
 *
 * 注意：如果 materialize 为真，则允许修改返回的      
 * 元组。但这种修改是否也会影响槽的内容，取决           
 * 于槽的类型。虽然这并非最理想的行为，但所有   
 * 此类修改都在被逐步移除的过程中。                      
 */
HeapTuple
ExecFetchSlotHeapTuple(TupleTableSlot *slot, bool materialize, bool *shouldFree)
{
	/*
	 * 例行检查 
	 */
	Assert(slot != NULL);
	Assert(!TTS_EMPTY(slot));

	/* 若请求，则物化元组让槽"拥有"它。                 */
	if (materialize)
		slot->tts_ops->materialize(slot);

	if (slot->tts_ops->get_heap_tuple == NULL)
	{
		if (shouldFree)
			*shouldFree = true;
		return slot->tts_ops->copy_heap_tuple(slot);
	}
	else
	{
		if (shouldFree)
			*shouldFree = false;
		return slot->tts_ops->get_heap_tuple(slot);
	}
}

/* --------------------------------
 *		ExecFetchSlotMinimalTuple
 *			获取槽的最小物理元组。       
 *
 *		如果给定的元组表槽能够容纳最小元组（由非          
 *		NULL 的 get_minimal_tuple 回调来指示），该函数会返      
 *		回该回调返回的最小元组。它假定回调返回的      
 *		最小元组是由该槽"拥有"的，也就是说槽要        
 *		负责释放该元组占用的内存。因此它把 shouldFree         
 *		*设为 false，表明调用者不应释放该最小元组占用    
 *		的内存。在这种情况下，返回的最小元组应当被        
 *		视为只读。                         
 *
 *		如果不支持该回调，它就会调用 copy_minimal_tuple          
 *		回调，预期它会返回一个代表槽内容的最小元组     
 *		的拷贝。此时 *shouldFree 被设为 true，表明调用  
 *		者应当释放该最小元组占用的内存。在这种           
 *		情况下，返回的最小元组可能会被改                     
 *		写
 * --------------------------------
 */
MinimalTuple
ExecFetchSlotMinimalTuple(TupleTableSlot *slot,
						  bool *shouldFree)
{
	/*
	 * 例行检查 
	 */
	Assert(slot != NULL);
	Assert(!TTS_EMPTY(slot));

	if (slot->tts_ops->get_minimal_tuple)
	{
		if (shouldFree)
			*shouldFree = false;
		return slot->tts_ops->get_minimal_tuple(slot);
	}
	else
	{
		if (shouldFree)
			*shouldFree = true;
		return slot->tts_ops->copy_minimal_tuple(slot);
	}
}

/* --------------------------------
 *		ExecFetchSlotHeapTupleDatum
 *			以复合类型 Datum 形式获取槽的元组。
 *
 *		结果总是在调用者的内存上下文中重新 palloc。        
 * --------------------------------
 */
Datum
ExecFetchSlotHeapTupleDatum(TupleTableSlot *slot)
{
	HeapTuple	tup;
	TupleDesc	tupdesc;
	bool		shouldFree;
	Datum		ret;

	/* 以常规物理元组形式获取槽的内容        */
	tup = ExecFetchSlotHeapTuple(slot, false, &shouldFree);
	tupdesc = slot->tts_tupleDescriptor;

	/* 转换为 Datum 形式*/
	ret = heap_copy_tuple_as_datum(tup, tupdesc);

	if (shouldFree)
		pfree(tup);

	return ret;
}

/* ----------------------------------------------------------------
 *				便利的初始化例程           
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecInitResultTypeTL
 *
 *		用计划节点的目标列表初始化结果类型。   
 * ----------------
 */
void
ExecInitResultTypeTL(PlanState *planstate)
{
	TupleDesc	tupDesc = ExecTypeFromTL(planstate->plan->targetlist);

	planstate->ps_ResultTupleDesc = tupDesc;
}

/* --------------------------------
 *		ExecInit{Result,Scan,Extra}TupleSlot[TL]
 *
 *		这些是便利例程，用于初始化继承了相应状态的
 *		节点中的指定槽。ExecInitExtraTupleSlot 则用于初始    
 *		化特殊用途的槽。                       
 * --------------------------------
 */

/* ----------------
 *		ExecInitResultTupleSlotTL
 *
 *		使用先前由 ExecInitResultTypeTL() 计算得到的元组       
 *		描述符初始化结果元组槽。 
 * ----------------
 */
void
ExecInitResultSlot(PlanState *planstate, const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot;

	slot = ExecAllocTableSlot(&planstate->state->es_tupleTable,
							  planstate->ps_ResultTupleDesc, tts_ops);
	planstate->ps_ResultTupleSlot = slot;

	planstate->resultopsfixed = planstate->ps_ResultTupleDesc != NULL;
	planstate->resultops = tts_ops;
	planstate->resultopsset = true;
}

/* ----------------
 *		ExecInitResultTupleSlotTL
 *
 *		使用计划节点的目标列表来初始化结果元组槽。
 * ----------------
 */
void
ExecInitResultTupleSlotTL(PlanState *planstate,
						  const TupleTableSlotOps *tts_ops)
{
	ExecInitResultTypeTL(planstate);
	ExecInitResultSlot(planstate, tts_ops);
}

/* ----------------
 *		ExecInitScanTupleSlot
 * ----------------
 */
void
ExecInitScanTupleSlot(EState *estate, ScanState *scanstate,
					  TupleDesc tupledesc, const TupleTableSlotOps *tts_ops)
{
	scanstate->ss_ScanTupleSlot = ExecAllocTableSlot(&estate->es_tupleTable,
													 tupledesc, tts_ops);
	scanstate->ps.scandesc = tupledesc;
	scanstate->ps.scanopsfixed = tupledesc != NULL;
	scanstate->ps.scanops = tts_ops;
	scanstate->ps.scanopsset = true;
}

/* ----------------
 *		ExecInitExtraTupleSlot
 *
 * 返回一个新建的槽。如果 tupledesc 非 NULL，则该槽        
 * 将拥有作为其固定描述符的它。否则调用者     
 * 需要先用 ExecSetSlotDescriptor() 设置描述符。  
 * ----------------
 */
TupleTableSlot *
ExecInitExtraTupleSlot(EState *estate,
					   TupleDesc tupledesc,
					   const TupleTableSlotOps *tts_ops)
{
	return ExecAllocTableSlot(&estate->es_tupleTable, tupledesc, tts_ops);
}

/* ----------------
 *		ExecInitNullTupleSlot
 *
 * 构建一个包含给定类型的全 NULL 元组的槽。    
 * 在执行外连接时，它被用作输入元组的替            
 * 代品。  
 * ----------------
 */
TupleTableSlot *
ExecInitNullTupleSlot(EState *estate, TupleDesc tupType,
					  const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot = ExecInitExtraTupleSlot(estate, tupType, tts_ops);

	return ExecStoreAllNullTuple(slot);
}

/* ---------------------------------------------------------------
 *      用于设置/访问槽内属性的例程。         
 * ---------------------------------------------------------------
 */

/*
 * 为 TupleTableSlot 填充缺失值。       
 *
 * 只把它暴露出来是因为 JIT 编译的元组反解       
 * 析需要它。除此之外，本文件之外不应存在调用            
 * 者  
 */
void
slot_getmissingattrs(TupleTableSlot *slot, int startAttNum, int lastAttNum)
{
	AttrMissing *attrmiss = NULL;

	if (slot->tts_tupleDescriptor->constr)
		attrmiss = slot->tts_tupleDescriptor->constr->missing;

	if (!attrmiss)
	{
		/* 完全没有缺失值数组，所以直接把一切都填成 NULL  */
		memset(slot->tts_values + startAttNum, 0,
			   (lastAttNum - startAttNum) * sizeof(Datum));
		memset(slot->tts_isnull + startAttNum, 1,
			   (lastAttNum - startAttNum) * sizeof(bool));
	}
	else
	{
		int			missattnum;

		/* 如果有缺失值数组，就必须逐个处理它们             */
		for (missattnum = startAttNum;
			 missattnum < lastAttNum;
			 missattnum++)
		{
			slot->tts_values[missattnum] = attrmiss[missattnum].am_value;
			slot->tts_isnull[missattnum] = !attrmiss[missattnum].am_present;
		}
	}
}

/*
 * slot_getsomeattrs_int - slot_getsomeattrs() 的主力    
 */
void
slot_getsomeattrs_int(TupleTableSlot *slot, int attnum)
{
	/* 检查调用者的错误*/
	Assert(slot->tts_nvalid < attnum);	/* slot_getsomeattrs 已检查  */
	Assert(attnum > 0);

	if (unlikely(attnum > slot->tts_tupleDescriptor->natts))
		elog(ERROR, "invalid attribute number %d", attnum);

	/* 从底层元组中尽可能多地取出属性。                */
	slot->tts_ops->getsomeattrs(slot, attnum);

	/*
	 * 如果底层元组没有足够的属性，那么元组描述 
	 * 符就必须拥有缺失的那些属性。  
	 */
	if (unlikely(slot->tts_nvalid < attnum))
	{
		slot_getmissingattrs(slot, slot->tts_nvalid, attnum);
		slot->tts_nvalid = attnum;
	}
}

/* ----------------------------------------------------------------
 *		ExecTypeFromTL
 *
 *		为目标列表的结果元组生成一个元组描述符。     
 *		（要传解析/计划 tlist，而不是 ExprState tlist） 
 *		注意 resjunk 列（如果有）会包含在结果中。     
 *
 *		目前约有 4 个不同的地方会创建                  
 *		TupleDescriptor。它们应被合并，或者重写为  
 *		调用 BuildDesc()。            
 * ----------------------------------------------------------------
 */
TupleDesc
ExecTypeFromTL(List *targetList)
{
	return ExecTypeFromTLInternal(targetList, false);
}

/* ----------------------------------------------------------------
 *		ExecCleanTypeFromTL
 *
 *		与上面相同，但结果中会省略 resjunk 列。         
 * ----------------------------------------------------------------
 */
TupleDesc
ExecCleanTypeFromTL(List *targetList)
{
	return ExecTypeFromTLInternal(targetList, true);
}

static TupleDesc
ExecTypeFromTLInternal(List *targetList, bool skipjunk)
{
	TupleDesc	typeInfo;
	ListCell   *l;
	int			len;
	int			cur_resno = 1;

	if (skipjunk)
		len = ExecCleanTargetListLength(targetList);
	else
		len = ExecTargetListLength(targetList);
	typeInfo = CreateTemplateTupleDesc(len);

	foreach(l, targetList)
	{
		TargetEntry *tle = lfirst(l);

		if (skipjunk && tle->resjunk)
			continue;
		TupleDescInitEntry(typeInfo,
						   cur_resno,
						   tle->resname,
						   exprType((Node *) tle->expr),
						   exprTypmod((Node *) tle->expr),
						   0);
		TupleDescInitEntryCollation(typeInfo,
									cur_resno,
									exprCollation((Node *) tle->expr));
		cur_resno++;
	}

	return typeInfo;
}

/*
 * ExecTypeFromExprList - 从 Expr 列表构建元组描述符         
 *
 * 这大致类似于 ExecTypeFromTL，但我们处理的是裸表达式 
 * 而非 TargetEntry。tupledesc 的列上没有附加名称。       
 */
TupleDesc
ExecTypeFromExprList(List *exprList)
{
	TupleDesc	typeInfo;
	ListCell   *lc;
	int			cur_resno = 1;

	typeInfo = CreateTemplateTupleDesc(list_length(exprList));

	foreach(lc, exprList)
	{
		Node	   *e = lfirst(lc);

		TupleDescInitEntry(typeInfo,
						   cur_resno,
						   NULL,
						   exprType(e),
						   exprTypmod(e),
						   0);
		TupleDescInitEntryCollation(typeInfo,
									cur_resno,
									exprCollation(e));
		cur_resno++;
	}

	return typeInfo;
}

/*
 * ExecTypeSetColNames - 在 RECORD TupleDesc 中设置列名  
 *
 * 列名必须以别名列表（String 节点列表）的形式提供。
 */
void
ExecTypeSetColNames(TupleDesc typeInfo, List *namesList)
{
	int			colno = 0;
	ListCell   *lc;

	/* 只允许在尚未定型的 RECORD 类型中修改列名          */
	Assert(typeInfo->tdtypeid == RECORDOID);
	Assert(typeInfo->tdtypmod < 0);

	foreach(lc, namesList)
	{
		char	   *cname = strVal(lfirst(lc));
		Form_pg_attribute attr;

		/* 防止名称列表过长（这多半不可能发生）    */
		if (colno >= typeInfo->natts)
			break;
		attr = TupleDescAttr(typeInfo, colno);
		colno++;

		/*
		 * 对空别名或已删除的列不做任何事（这些情况
		 * 多半也不会出现在 RECORD 类型中） 
		 */
		if (cname[0] == '\0' || attr->attisdropped)
			continue;

		/* 现在赋予该列的名称*/
		namestrcpy(&(attr->attname), cname);
	}
}

/*
 * BlessTupleDesc - 让已完成的元组描述符可被 SRF 使用  
 *
 * 函数返回的行类型 Datum 必须包含有效的类型信息。       
 * 如果 tupdesc 来自 relcache 条目，这会"免费"发生；但    
 * 如果我们为瞬态 RECORD 类型手工构造了 tupdesc，         
 * 那么必须把该类型的存在告知 typcache.c。                  
 */
TupleDesc
BlessTupleDesc(TupleDesc tupdesc)
{
	if (tupdesc->tdtypeid == RECORDOID &&
		tupdesc->tdtypmod < 0)
		assign_record_type_typmod(tupdesc);

	return tupdesc;				/* 只是为了书写上的便利  */
}

/*
 * TupleDescGetAttInMetadata - 基于给定的 TupleDesc 构                
 * 建一个 AttInMetadata 结构。AttInMetadata 可配合 C 字符串       
 * 以生成格式正确的元组。  
 */
AttInMetadata *
TupleDescGetAttInMetadata(TupleDesc tupdesc)
{
	int			natts = tupdesc->natts;
	int			i;
	Oid			atttypeid;
	Oid			attinfuncid;
	FmgrInfo   *attinfuncinfo;
	Oid		   *attioparams;
	int32	   *atttypmods;
	AttInMetadata *attinmeta;

	attinmeta = (AttInMetadata *) palloc(sizeof(AttInMetadata));

	/* "定型"该 tupledesc，以便用它构造行类型 datum        */
	attinmeta->tupdesc = BlessTupleDesc(tupdesc);

	/*
	 * 收集以后为每个属性调用 "in" 函数所需的信息         
	 */
	attinfuncinfo = (FmgrInfo *) palloc0(natts * sizeof(FmgrInfo));
	attioparams = (Oid *) palloc0(natts * sizeof(Oid));
	atttypmods = (int32 *) palloc0(natts * sizeof(int32));

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/* 忽略已删除的属性  */
		if (!att->attisdropped)
		{
			atttypeid = att->atttypid;
			getTypeInputInfo(atttypeid, &attinfuncid, &attioparams[i]);
			fmgr_info(attinfuncid, &attinfuncinfo[i]);
			atttypmods[i] = att->atttypmod;
		}
	}
	attinmeta->attinfuncs = attinfuncinfo;
	attinmeta->attioparams = attioparams;
	attinmeta->atttypmods = atttypmods;

	return attinmeta;
}

/*
 * BuildTupleFromCStrings - 根据以 C 字符串形式给出的               
 * 用户数据构建 HeapTuple。values 是 C 字符串数组，每           
 * 个属性一个元素。NULL 串指针表示 NULL 字段。    
 */
HeapTuple
BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values)
{
	TupleDesc	tupdesc = attinmeta->tupdesc;
	int			natts = tupdesc->natts;
	Datum	   *dvalues;
	bool	   *nulls;
	int			i;
	HeapTuple	tuple;

	dvalues = (Datum *) palloc(natts * sizeof(Datum));
	nulls = (bool *) palloc(natts * sizeof(bool));

	/*
	 * 为支持域，即使对 NULL 也要为每个未删除属性调用    
	 * "in" 函数。     
	 */
	for (i = 0; i < natts; i++)
	{
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
		{
			/* 未删除的属性     */
			dvalues[i] = InputFunctionCall(&attinmeta->attinfuncs[i],
										   values[i],
										   attinmeta->attioparams[i],
										   attinmeta->atttypmods[i]);
			if (values[i] != NULL)
				nulls[i] = false;
			else
				nulls[i] = true;
		}
		else
		{
			/* 把已删除的属性设为 NULL 来处理   */
			dvalues[i] = (Datum) 0;
			nulls[i] = true;
		}
	}

	/*
	 * 构成元组
	 */
	tuple = heap_form_tuple(tupdesc, dvalues, nulls);

	/*
	 * 释放本地 palloc 的空间。XXX 最好也能 pfree 按引用    
	 * 传递的 datum 的值。                   
	 */
	pfree(dvalues);
	pfree(nulls);

	return tuple;
}

/*
 * HeapTupleHeaderGetDatum - 把 HeapTupleHeader 指针转换为 Datum。 
 *
 * 它*绝对不能*被用于磁盘上的元组；元组               
 * 应当是由 heap_form_tuple 或它的某个包装例程（例         
 * 如 BuildTupleFromCStrings）新近构建的。还要确保            
 * 其 tupledesc 行类型已"定型"。      
 *
 * 以前它是一个等价于 PointerGetDatum 的宏，依赖于         
 * 这样的事实：heap_form_tuple 会为复合 Datum 填好恰当     
 * 的元组头字段。然而我们现在要求复合 Datum 不            
 * 包含任何外部 TOAST 指针。我们不希望 heap_form_tuple          
 * 自身来强制这一点；更具体地说，该规则只适用于        
 * 真正的 Datum，而非 HeapTuple 结构。因此，                  
 * HeapTupleHeaderGetDatum 现在是一个函数，它会检测           
 * 是否存在外部 TOAST 的字段，若有则构建一个          
 * 字段已内联的新元组。仍需 heap_form_tuple 来插入 Datum     
 * 头字段，否则代码无法取得元组的 tupledesc。 
 *
 * 注意：若确实构建了新元组，它是在当前内存上下
 * 文中 palloc 的。要当心在初始 heap_form_tuple 等调用与     
 * HeapTuple(Header)GetDatum 调用间更改上下文的代码。 
 *
 * 对性能关键的调用者来说，采取额外步骤来确保       
 * heap_form_tuple 的输出中一开始就没有 TOAST 指针，   
 * 可能是值得的。然而 typcache 查找以                           
 * 及元组拆解/重组的开销很可能被 TOAST 解引用          
 * 的开销吞没，因此这类额外努力的好处很可能很      
 * 小。  
 *
 * XXX 更好的做法可能是创建包装函数，一步就从         
 * 字段值生成复合 Datum。然而，使用现有 API 的代码     
 * 多得让我们无法在短期内摆脱这个                       
 * hack              
 */
Datum
HeapTupleHeaderGetDatum(HeapTupleHeader tuple)
{
	Datum		result;
	TupleDesc	tupDesc;

	/* 如果元组中没有外部 TOAST 指针则无事可做      */
	if (!HeapTupleHeaderHasExternal(tuple))
		return PointerGetDatum(tuple);

	/* 使用 heap_form_tuple 保存的类型数据来查找行类型    */
	tupDesc = lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(tuple),
									 HeapTupleHeaderGetTypMod(tuple));

	/* 然后执行扁平化 */
	result = toast_flatten_tuple_to_datum(tuple,
										  HeapTupleHeaderGetDatumLength(tuple),
										  tupDesc);

	ReleaseTupleDesc(tupDesc);

	return result;
}


/*
 * 用于把元组发送到前端（或其他指定目的地）的函数，     
 * 发送方式如同它是一个 SELECT 结果。有些工具命令       
 * 需要直接投影到目的地，且不需要或不想拥有完整     
 * 的表函数能力。目前被 EXPLAIN 和 SHOW ALL 使用。     
 */
TupOutputState *
begin_tup_output_tupdesc(DestReceiver *dest,
						 TupleDesc tupdesc,
						 const TupleTableSlotOps *tts_ops)
{
	TupOutputState *tstate;

	tstate = (TupOutputState *) palloc(sizeof(TupOutputState));

	tstate->slot = MakeSingleTupleTableSlot(tupdesc, tts_ops);
	tstate->dest = dest;

	tstate->dest->rStartup(tstate->dest, (int) CMD_SELECT, tupdesc);

	return tstate;
}

/*
 * 写入单个元组  
 */
void
do_tup_output(TupOutputState *tstate, Datum *values, bool *isnull)
{
	TupleTableSlot *slot = tstate->slot;
	int			natts = slot->tts_tupleDescriptor->natts;

	/* 确保该槽已被清空    */
	ExecClearTuple(slot);

	/* 插入数据*/
	memcpy(slot->tts_values, values, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, isnull, natts * sizeof(bool));

	/* 把槽标记为包含虚拟元组       */
	ExecStoreVirtualTuple(slot);

	/* 把元组发送给接收方    */
	(void) tstate->dest->receiveSlot(slot, tstate->dest);

	/* 清理   */
	ExecClearTuple(slot);
}

/*
 * 写出一段文本，在换行符处断开           
 *
 * 只应与含单个 TEXT 属性的 tupdesc 一起使用。
 */
void
do_text_output_multiline(TupOutputState *tstate, const char *txt)
{
	Datum		values[1];
	bool		isnull[1] = {false};

	while (*txt)
	{
		const char *eol;
		int			len;

		eol = strchr(txt, '\n');
		if (eol)
		{
			len = eol - txt;
			eol++;
		}
		else
		{
			len = strlen(txt);
			eol = txt + len;
		}

		values[0] = PointerGetDatum(cstring_to_text_with_len(txt, len));
		do_tup_output(tstate, values, isnull);
		pfree(DatumGetPointer(values[0]));
		txt = eol;
	}
}

void
end_tup_output(TupOutputState *tstate)
{
	tstate->dest->rShutdown(tstate->dest);
	/* 注意销毁 dest 并非我们该做的事      */
	ExecDropSingleTupleTableSlot(tstate->slot);
	pfree(tstate);
}
