/*-------------------------------------------------------------------------
 *
 * tupconvert.c
 *	  元组转换支持。
 *
 * 这些函数提供逻辑上等价、但列的顺序可能不同、
 * 或已删除列的集合可能不同的行类型之间的
 * 转换。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/tupconvert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupconvert.h"
#include "executor/tuptable.h"


/*
 * 转换准备例程有如下公共 API：
 *
 * 准备例程借助 attmap.c 检查给定的源元组描述符与
 * 目标元组描述符在逻辑上是否兼容。若不兼容，就抛出
 * 错误。若兼容，则当它们在物理上也兼容时（即无需
 * 转换）返回 NULL，否则返回一份可供 execute_attr_map_tuple
 * 或 execute_attr_map_slot 用来执行转换的 TupleConversionMap。
 *
 * 若需要 TupleConversionMap，则在调用方的内存上下文中
 * 用 palloc 分配。另外，该映射会引用给定的元组描述符，
 * 因此它们的寿命必须不短于该映射的需要。
 *
 * 若抛出兼容性错误，调用方必须提供一条合适的主错误消息。
 * 推荐的做法是对该字符串使用 gettext_noop()，使它可以翻译，
 * 但除非错误真的被抛出，否则不会实际
 * 翻译。
 *
 *
 * 实现说明：
 *
 * TupleConversionMap 的关键组成部分是 attrMap[] 数组，
 * 每个输出列对应一项。该项含有对应输入列从 1 开始的下标，
 * 或者为零，以强制一个 NULL 值（用于
 * 已删除的输出列）。TupleConversionMap 还包含工作区
 * 数组。
 */


/*
 * 为元组转换做准备，按位置匹配输入列与输出列。
 * （输入和输出中的已删除列都忽略。）
 */
TupleConversionMap *
convert_tuples_by_position(TupleDesc indesc,
						   TupleDesc outdesc,
						   const char *msg)
{
	TupleConversionMap *map;
	int			n;
	AttrMap    *attrMap;

	/* 验证兼容性并准备属性号映射 */
	attrMap = build_attrmap_by_position(indesc, outdesc, msg);

	if (attrMap == NULL)
	{
		/* 不需要运行时转换 */
		return NULL;
	}

	/* 准备映射结构 */
	map = (TupleConversionMap *) palloc(sizeof(TupleConversionMap));
	map->indesc = indesc;
	map->outdesc = outdesc;
	map->attrMap = attrMap;
	/* 为 Datum 数组预分配工作区 */
	n = outdesc->natts + 1;		/* 为 NULL 加 1 */
	map->outvalues = (Datum *) palloc(n * sizeof(Datum));
	map->outisnull = (bool *) palloc(n * sizeof(bool));
	n = indesc->natts + 1;		/* 为 NULL 加 1 */
	map->invalues = (Datum *) palloc(n * sizeof(Datum));
	map->inisnull = (bool *) palloc(n * sizeof(bool));
	map->invalues[0] = (Datum) 0;	/* 设置 NULL 项 */
	map->inisnull[0] = true;

	return map;
}

/*
 * 为元组转换做准备，按名称匹配输入列与输出列。
 * （输入和输出中的已删除列都忽略。）这是为
 * 由继承相关联的行类型准备的，因此类型与 typmod
 * 都应当精确匹配。除非两种行类型都是命名的复合类型，
 * 否则错误消息会不太有帮助。
 */
TupleConversionMap *
convert_tuples_by_name(TupleDesc indesc,
					   TupleDesc outdesc)
{
	AttrMap    *attrMap;

	/* 验证兼容性并准备属性号映射 */
	attrMap = build_attrmap_by_name_if_req(indesc, outdesc, false);

	if (attrMap == NULL)
	{
		/* 不需要运行时转换 */
		return NULL;
	}

	return convert_tuples_by_name_attrmap(indesc, outdesc, attrMap);
}

/*
 * 用给定的 AttrMap，为输入与输出 TupleDescs
 * 建立元组转换。
 */
TupleConversionMap *
convert_tuples_by_name_attrmap(TupleDesc indesc,
							   TupleDesc outdesc,
							   AttrMap *attrMap)
{
	int			n = outdesc->natts;
	TupleConversionMap *map;

	Assert(attrMap != NULL);

	/* 准备映射结构 */
	map = (TupleConversionMap *) palloc(sizeof(TupleConversionMap));
	map->indesc = indesc;
	map->outdesc = outdesc;
	map->attrMap = attrMap;
	/* 为 Datum 数组预分配工作区 */
	map->outvalues = (Datum *) palloc(n * sizeof(Datum));
	map->outisnull = (bool *) palloc(n * sizeof(bool));
	n = indesc->natts + 1;		/* 为 NULL 加 1 */
	map->invalues = (Datum *) palloc(n * sizeof(Datum));
	map->inisnull = (bool *) palloc(n * sizeof(bool));
	map->invalues[0] = (Datum) 0;	/* 设置 NULL 项 */
	map->inisnull[0] = true;

	return map;
}

/*
 * 按该映射执行元组转换。
 */
HeapTuple
execute_attr_map_tuple(HeapTuple tuple, TupleConversionMap *map)
{
	AttrMap    *attrMap = map->attrMap;
	Datum	   *invalues = map->invalues;
	bool	   *inisnull = map->inisnull;
	Datum	   *outvalues = map->outvalues;
	bool	   *outisnull = map->outisnull;
	int			i;

	/*
	 * 抽出旧元组的全部值，并把数组偏移，使
	 * invalues[0] 留作 NULL，invalues[1] 为第一个源属性；
	 * 这与 attrMap 中的编号约定完全一致。
	 */
	heap_deform_tuple(tuple, map->indesc, invalues + 1, inisnull + 1);

	/*
	 * 转置到新元组的适当字段中。
	 */
	Assert(attrMap->maplen == map->outdesc->natts);
	for (i = 0; i < attrMap->maplen; i++)
	{
		int			j = attrMap->attnums[i];

		outvalues[i] = invalues[j];
		outisnull[i] = inisnull[j];
	}

	/*
	 * 现在构造新元组。
	 */
	return heap_form_tuple(map->outdesc, outvalues, outisnull);
}

/*
 * 按该映射转换一个元组槽。
 */
TupleTableSlot *
execute_attr_map_slot(AttrMap *attrMap,
					  TupleTableSlot *in_slot,
					  TupleTableSlot *out_slot)
{
	Datum	   *invalues;
	bool	   *inisnull;
	Datum	   *outvalues;
	bool	   *outisnull;
	int			outnatts;
	int			i;

	/* 健全性检查 */
	Assert(in_slot->tts_tupleDescriptor != NULL &&
		   out_slot->tts_tupleDescriptor != NULL);
	Assert(in_slot->tts_values != NULL && out_slot->tts_values != NULL);

	outnatts = out_slot->tts_tupleDescriptor->natts;

	/* 抽出输入槽的全部值。 */
	slot_getallattrs(in_slot);

	/* 做映射之前，清除输出槽中的旧内容 */
	ExecClearTuple(out_slot);

	invalues = in_slot->tts_values;
	inisnull = in_slot->tts_isnull;
	outvalues = out_slot->tts_values;
	outisnull = out_slot->tts_isnull;

	/* 转置到输出槽的适当字段中。 */
	for (i = 0; i < outnatts; i++)
	{
		int			j = attrMap->attnums[i] - 1;

		/* attrMap->attnums[i] == 0 表示这是一个 NULL Datum。 */
		if (j == -1)
		{
			outvalues[i] = (Datum) 0;
			outisnull[i] = true;
		}
		else
		{
			outvalues[i] = invalues[j];
			outisnull[i] = inisnull[j];
		}
	}

	ExecStoreVirtualTuple(out_slot);

	return out_slot;
}

/*
 * 按该映射转换列的位图。
 *
 * 输入与输出位图都按 FirstLowInvalidHeapAttributeNumber
 * 做了偏移，以便容纳系统列，如同 RangeTblEntry 中的
 * 列位图。
 */
Bitmapset *
execute_attr_map_cols(AttrMap *attrMap, Bitmapset *in_cols)
{
	Bitmapset  *out_cols;
	int			out_attnum;

	/* 常见平凡情形的快速路径 */
	if (in_cols == NULL)
		return NULL;

	/*
	 * 对每个输出列，检查它对应哪个输入列。
	 */
	out_cols = NULL;

	for (out_attnum = FirstLowInvalidHeapAttributeNumber;
		 out_attnum <= attrMap->maplen;
		 out_attnum++)
	{
		int			in_attnum;

		if (out_attnum < 0)
		{
			/* 系统列。无需映射。 */
			in_attnum = out_attnum;
		}
		else if (out_attnum == 0)
			continue;
		else
		{
			/* 普通用户列 */
			in_attnum = attrMap->attnums[out_attnum - 1];

			if (in_attnum == 0)
				continue;
		}

		if (bms_is_member(in_attnum - FirstLowInvalidHeapAttributeNumber, in_cols))
			out_cols = bms_add_member(out_cols, out_attnum - FirstLowInvalidHeapAttributeNumber);
	}

	return out_cols;
}

/*
 * 释放 TupleConversionMap 结构。
 */
void
free_conversion_map(TupleConversionMap *map)
{
	/* indesc 与 outdesc 不由我们释放 */
	free_attrmap(map->attrMap);
	pfree(map->invalues);
	pfree(map->inisnull);
	pfree(map->outvalues);
	pfree(map->outisnull);
	pfree(map);
}
