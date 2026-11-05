/*-------------------------------------------------------------------------
 *
 * execJunk.c
 *	  junk 属性支撑代码。      
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execJunk.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"

/*-------------------------------------------------------------------------
 *		XXX 这部分代码应当重写，以利用         
 *			ExecProject() 与 ProjectionInfo 节点。   
 *			-cim 6/3/91
 *
 * 执行器内部的元组属性可以是普通属性，    
 * 也可以是 "junk" 属性。"junk" 属性绝不会               
 * 离开执行器，即它们永远不会被打印、返回      
 * 或存入磁盘。它们存在的唯一目的，是保存   
 * 一些仅对执行器有用的信息，主要是 "ctid" 这类       
 * 系统属性的值，或者无须输出的排序键                  
 * 列。    
 *
 * 总体思路如下：目标列表由一组包含表达式的          
 * TargetEntry 节点构成。每个 TargetEntry 都有一个            
 * 名为 'resjunk' 的字段。若该字段的值为 true，   
 * 则对应属性就是 "junk" 属性。        
 *
 * 初始化计划时，我们调用 ExecInitJunkFilter 创建过滤器。 
 *
 * 然后执行计划，将 resjunk 属性与其他属性同等对待。     
 *
 * 最后，在顶层得到元组时，我们可以调用         
 * ExecFindJunkAttribute/ExecGetJunkAttribute 来获取我们所           
 * 感兴趣的 junk 属性的值，并用 ExecFilterJunk 从元组          
 * 中移除所有 junk 属性。这个新的 "干净" 元组随后被   
 * 打印插入更新。
 *
 *-------------------------------------------------------------------------
 */

/*
 * ExecInitJunkFilter
 *
 * 初始化 Junk 过滤器。
 *
 * 传入的是源目标列表。输出元组描述符由非 junk 的 
 * tlist 条目构建而成。           
 * 还可传入可选的 resultSlot；否则我们自行创建一个。   
 */
JunkFilter *
ExecInitJunkFilter(List *targetList, TupleTableSlot *slot)
{
	JunkFilter *junkfilter;
	TupleDesc	cleanTupType;
	int			cleanLength;
	AttrNumber *cleanMap;

	/*
	 * 为清理后的元组计算其元组描述符。   
	 */
	cleanTupType = ExecCleanTypeFromTL(targetList);

	/*
	 * 使用给定的槽；若未给定，则新建一个槽。      
	 */
	if (slot)
		ExecSetSlotDescriptor(slot, cleanTupType);
	else
		slot = MakeSingleTupleTableSlot(cleanTupType, &TTSOpsVirtual);

	/*
	 * 现在计算原始元组的属性与 "干净" 元组的属性        
	 * 之间的映射关系。       
	 *
	 * "map" 是一个包含 "cleanLength" 个属性编号的数组，   
	 * 即 "干净" 元组的每个属性各占一项。该条目的值       
	 * 是 "原始" 元组中对应属性的属性号。            
	 * （零值表示输出属性为 NULL，但本例程并不使用       
	 * 该特性。）                       
	 */
	cleanLength = cleanTupType->natts;
	if (cleanLength > 0)
	{
		AttrNumber	cleanResno;
		ListCell   *t;

		cleanMap = (AttrNumber *) palloc(cleanLength * sizeof(AttrNumber));
		cleanResno = 0;
		foreach(t, targetList)
		{
			TargetEntry *tle = lfirst(t);

			if (!tle->resjunk)
			{
				cleanMap[cleanResno] = tle->resno;
				cleanResno++;
			}
		}
		Assert(cleanResno == cleanLength);
	}
	else
		cleanMap = NULL;

	/*
	 * 最后创建并初始化 JunkFilter 结构体。    
	 */
	junkfilter = makeNode(JunkFilter);

	junkfilter->jf_targetList = targetList;
	junkfilter->jf_cleanTupType = cleanTupType;
	junkfilter->jf_cleanMap = cleanMap;
	junkfilter->jf_resultSlot = slot;

	return junkfilter;
}

/*
 * ExecInitJunkFilterConversion
 *
 * 为行类型转换初始化一个 JunkFilter。 
 *
 * 此处直接给定目标 "干净" 元组描述符，而不是     
 * 从目标列表推断得出。目标描述符可以包含已删除  
 * 的列。假定调用方已检查目标列表中非 junk 列        
 * 与非删除列相匹配。                                              
 */
JunkFilter *
ExecInitJunkFilterConversion(List *targetList,
							 TupleDesc cleanTupType,
							 TupleTableSlot *slot)
{
	JunkFilter *junkfilter;
	int			cleanLength;
	AttrNumber *cleanMap;
	ListCell   *t;
	int			i;

	/*
	 * 使用给定的槽；若未给定，则新建一个槽。      
	 */
	if (slot)
		ExecSetSlotDescriptor(slot, cleanTupType);
	else
		slot = MakeSingleTupleTableSlot(cleanTupType, &TTSOpsVirtual);

	/*
	 * 计算原始元组的属性与 "干净" 元组的属性之间的     
	 * 映射关系。            
	 *
	 * "map" 是一个包含 "cleanLength" 个属性编号的数组，   
	 * 即 "干净" 元组的每个属性各占一项。该条目的值       
	 * 是 "原始" 元组中对应属性的属性号。            
	 * 对于已删除的属性，我们存入零值，以表明输出     
	 * 元组中需要 NULL。                   
	 */
	cleanLength = cleanTupType->natts;
	if (cleanLength > 0)
	{
		cleanMap = (AttrNumber *) palloc0(cleanLength * sizeof(AttrNumber));
		t = list_head(targetList);
		for (i = 0; i < cleanLength; i++)
		{
			if (TupleDescAttr(cleanTupType, i)->attisdropped)
				continue;		/* 该映射项已经为零  */
			for (;;)
			{
				TargetEntry *tle = lfirst(t);

				t = lnext(targetList, t);
				if (!tle->resjunk)
				{
					cleanMap[i] = tle->resno;
					break;
				}
			}
		}
	}
	else
		cleanMap = NULL;

	/*
	 * 最后创建并初始化 JunkFilter 结构体。    
	 */
	junkfilter = makeNode(JunkFilter);

	junkfilter->jf_targetList = targetList;
	junkfilter->jf_cleanTupType = cleanTupType;
	junkfilter->jf_cleanMap = cleanMap;
	junkfilter->jf_resultSlot = slot;

	return junkfilter;
}

/*
 * ExecFindJunkAttribute
 *
 * 在 junk 过滤器的目标列表中定位指定的 junk 属性，  
 * 并返回其 resno。找不到时返回 InvalidAttrNumber。  
 */
AttrNumber
ExecFindJunkAttribute(JunkFilter *junkfilter, const char *attrName)
{
	return ExecFindJunkAttributeInTlist(junkfilter->jf_targetList, attrName);
}

/*
 * ExecFindJunkAttributeInTlist
 *
 * 给定子计划的目标列表来查找 junk 属性（未必属于 
 * JunkFilter）。      
 */
AttrNumber
ExecFindJunkAttributeInTlist(List *targetlist, const char *attrName)
{
	ListCell   *t;

	foreach(t, targetlist)
	{
		TargetEntry *tle = lfirst(t);

		if (tle->resjunk && tle->resname &&
			(strcmp(tle->resname, attrName) == 0))
		{
			/* 找到了！  */
			return tle->resno;
		}
	}

	return InvalidAttrNumber;
}

/*
 * ExecFilterJunk
 *
 * 构建并返回一个移除了全部 junk 属性的槽。        
 */
TupleTableSlot *
ExecFilterJunk(JunkFilter *junkfilter, TupleTableSlot *slot)
{
	TupleTableSlot *resultSlot;
	AttrNumber *cleanMap;
	TupleDesc	cleanTupType;
	int			cleanLength;
	int			i;
	Datum	   *values;
	bool	   *isnull;
	Datum	   *old_values;
	bool	   *old_isnull;

	/*
	 * 提取旧元组的所有属性值。    
	 */
	slot_getallattrs(slot);
	old_values = slot->tts_values;
	old_isnull = slot->tts_isnull;

	/*
	 * 从 junk 过滤器取信息  
	 */
	cleanTupType = junkfilter->jf_cleanTupType;
	cleanLength = cleanTupType->natts;
	cleanMap = junkfilter->jf_cleanMap;
	resultSlot = junkfilter->jf_resultSlot;

	/*
	 * 准备构建一个虚拟结果元组。 
	 */
	ExecClearTuple(resultSlot);
	values = resultSlot->tts_values;
	isnull = resultSlot->tts_isnull;

	/*
	 * 把数据转置到新元组的相应字段中。   
	 */
	for (i = 0; i < cleanLength; i++)
	{
		int			j = cleanMap[i];

		if (j == 0)
		{
			values[i] = (Datum) 0;
			isnull[i] = true;
		}
		else
		{
			values[i] = old_values[j - 1];
			isnull[i] = old_isnull[j - 1];
		}
	}

	/*
	 * 再返回该虚拟元组。  
	 */
	return ExecStoreVirtualTuple(resultSlot);
}
