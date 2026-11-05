/*-------------------------------------------------------------------------
 *
 * tstoreReceiver.c
 *	  一种 DestReceiver 实现，将结果元组存入                
 *	  Tuplestore。
 *
 * 可选地，我们可以强制 detoast（但不解压）                  
 * 行外存储的 toast 值。这是为了支持 WITH HOLD 游标，      
 * 即使底层表被删也须保留数据。   
 *
 * 亦可选地，我们可在存储前应用元组转换映射。     
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/tstoreReceiver.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/tupconvert.h"
#include "executor/tstoreReceiver.h"


typedef struct
{
	DestReceiver pub;
	/* 参数：   */
	Tuplestorestate *tstore;	/* 数据存放位置    */
	MemoryContext cxt;			/* 包含 tstore 的上下文*/
	bool		detoast;		/* 是否要求 detoast？  */
	TupleDesc	target_tupdesc; /* 目标 tupdesc，无则为 NULL */
	const char *map_failure_msg;	/* tupdesc 映射失败消息      */
	/* 工作区  */
	Datum	   *outvalues;		/* 结果元组的值数组      */
	Datum	   *tofree;			/* 待 pfree 的临时值    */
	TupleConversionMap *tupmap; /* 转换映射（如需）  */
	TupleTableSlot *mapslot;	/* 映射元组的槽     */
} TStoreState;


static bool tstoreReceiveSlot_notoast(TupleTableSlot *slot, DestReceiver *self);
static bool tstoreReceiveSlot_detoast(TupleTableSlot *slot, DestReceiver *self);
static bool tstoreReceiveSlot_tupmap(TupleTableSlot *slot, DestReceiver *self);


/*
 * 准备接收来自执行器的元组。 
 */
static void
tstoreStartupReceiver(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	TStoreState *myState = (TStoreState *) self;
	bool		needtoast = false;
	int			natts = typeinfo->natts;
	int			i;

	/* 检查是否有列需要 detoast 处理。*/
	if (myState->detoast)
	{
		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(typeinfo, i);

			if (attr->attisdropped)
				continue;
			if (attr->attlen == -1)
			{
				needtoast = true;
				break;
			}
		}
	}

	/* 检查是否需要元组转换。   */
	if (myState->target_tupdesc)
		myState->tupmap = convert_tuples_by_position(typeinfo,
													 myState->target_tupdesc,
													 myState->map_failure_msg);
	else
		myState->tupmap = NULL;

	/* 设置合适的回调。    */
	if (needtoast)
	{
		Assert(!myState->tupmap);
		myState->pub.receiveSlot = tstoreReceiveSlot_detoast;
		/* 创建工作区  */
		myState->outvalues = (Datum *)
			MemoryContextAlloc(myState->cxt, natts * sizeof(Datum));
		myState->tofree = (Datum *)
			MemoryContextAlloc(myState->cxt, natts * sizeof(Datum));
		myState->mapslot = NULL;
	}
	else if (myState->tupmap)
	{
		myState->pub.receiveSlot = tstoreReceiveSlot_tupmap;
		myState->outvalues = NULL;
		myState->tofree = NULL;
		myState->mapslot = MakeSingleTupleTableSlot(myState->target_tupdesc,
													&TTSOpsVirtual);
	}
	else
	{
		myState->pub.receiveSlot = tstoreReceiveSlot_notoast;
		myState->outvalues = NULL;
		myState->tofree = NULL;
		myState->mapslot = NULL;
	}
}

/*
 * 从执行器接收一个元组并存入 tuplestore。            
 * 这是无需 detoast 也无需任何映射的简单情形。              
 */
static bool
tstoreReceiveSlot_notoast(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	tuplestore_puttupleslot(myState->tstore, slot);

	return true;
}

/*
 * 从执行器接收一个元组并存入 tuplestore。            
 * 这是需要对被 toast 的值做 detoast 的情形。          
 */
static bool
tstoreReceiveSlot_detoast(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	int			natts = typeinfo->natts;
	int			nfree;
	int			i;
	MemoryContext oldcxt;

	/* 确保元组已被完整解构。          */
	slot_getallattrs(slot);

	/*
	 * 取回所有行外的 datum。新的 datum 数组构建于          
	 * myState->outvalues[] 中（但可复用槽的 isnull 数组）。      
	 * 并记住取回的值，以便稍后释放。  
	 */
	nfree = 0;
	for (i = 0; i < natts; i++)
	{
		Datum		val = slot->tts_values[i];
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);

		if (!attr->attisdropped && attr->attlen == -1 && !slot->tts_isnull[i])
		{
			if (VARATT_IS_EXTERNAL(DatumGetPointer(val)))
			{
				val = PointerGetDatum(detoast_external_attr((struct varlena *)
															DatumGetPointer(val)));
				myState->tofree[nfree++] = val;
			}
		}

		myState->outvalues[i] = val;
	}

	/*
	 * 把修改后的元组推入 tuplestore。   
	 */
	oldcxt = MemoryContextSwitchTo(myState->cxt);
	tuplestore_putvalues(myState->tstore, typeinfo,
						 myState->outvalues, slot->tts_isnull);
	MemoryContextSwitchTo(oldcxt);

	/* 并释放所有临时 detoast 值。       */
	for (i = 0; i < nfree; i++)
		pfree(DatumGetPointer(myState->tofree[i]));

	return true;
}

/*
 * 从执行器接收一个元组并存入 tuplestore。            
 * 这是必须应用元组转换映射的情形。                
 */
static bool
tstoreReceiveSlot_tupmap(TupleTableSlot *slot, DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	execute_attr_map_slot(myState->tupmap->attrMap, slot, myState->mapslot);
	tuplestore_puttupleslot(myState->tstore, myState->mapslot);

	return true;
}

/*
 * 执行器运行结束时的清理 
 */
static void
tstoreShutdownReceiver(DestReceiver *self)
{
	TStoreState *myState = (TStoreState *) self;

	/* 若有工作区则释放 */
	if (myState->outvalues)
		pfree(myState->outvalues);
	myState->outvalues = NULL;
	if (myState->tofree)
		pfree(myState->tofree);
	myState->tofree = NULL;
	if (myState->tupmap)
		free_conversion_map(myState->tupmap);
	myState->tupmap = NULL;
	if (myState->mapslot)
		ExecDropSingleTupleTableSlot(myState->mapslot);
	myState->mapslot = NULL;
}

/*
 * 用完后销毁该接收器。    
 */
static void
tstoreDestroyReceiver(DestReceiver *self)
{
	pfree(self);
}

/*
 * 最初创建 DestReceiver 对象。    
 */
DestReceiver *
CreateTuplestoreDestReceiver(void)
{
	TStoreState *self = (TStoreState *) palloc0(sizeof(TStoreState));

	self->pub.receiveSlot = tstoreReceiveSlot_notoast;	/* 可能改变 */
	self->pub.rStartup = tstoreStartupReceiver;
	self->pub.rShutdown = tstoreShutdownReceiver;
	self->pub.rDestroy = tstoreDestroyReceiver;
	self->pub.mydest = DestTuplestore;

	/* 私有字段将由 SetTuplestoreDestReceiverParams 设置     */

	return (DestReceiver *) self;
}

/*
 * 为 TuplestoreDestReceiver 设置参数    
 *
 * tStore: 元组存储位置       
 * tContext: 含 tStore 的内存上下文   
 * detoast: 强制 detoast 所含数据？  
 * target_tupdesc: 若非 NULL，强制把元组转换为此行类型  
 * map_failure_msg: 映射到 target_tupdesc 失败时用的错误消息   
 *
 * 目前我们并不支持同时使用 detoast 与 target_tupdesc，    
 * 仅仅因为现有的调用方都不需要这种组合。    
 */
void
SetTuplestoreDestReceiverParams(DestReceiver *self,
								Tuplestorestate *tStore,
								MemoryContext tContext,
								bool detoast,
								TupleDesc target_tupdesc,
								const char *map_failure_msg)
{
	TStoreState *myState = (TStoreState *) self;

	Assert(!(detoast && target_tupdesc));

	Assert(myState->pub.mydest == DestTuplestore);
	myState->tstore = tStore;
	myState->cxt = tContext;
	myState->detoast = detoast;
	myState->target_tupdesc = target_tupdesc;
	myState->map_failure_msg = map_failure_msg;
}
