/*-------------------------------------------------------------------------
 *
 * tupconvert.h
 *	  元组转换支持。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupconvert.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPCONVERT_H
#define TUPCONVERT_H

#include "access/attmap.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"


typedef struct TupleConversionMap
{
	TupleDesc	indesc;			/* 源行类型的 tupdesc */
	TupleDesc	outdesc;		/* 结果行类型的 tupdesc */
	AttrMap    *attrMap;		/* 输入字段下标，或 0 表示 NULL */
	Datum	   *invalues;		/* 用于拆解源的工作区 */
	bool	   *inisnull;
	Datum	   *outvalues;		/* 用于构造结果的工作区 */
	bool	   *outisnull;
} TupleConversionMap;


extern TupleConversionMap *convert_tuples_by_position(TupleDesc indesc,
													  TupleDesc outdesc,
													  const char *msg);

extern TupleConversionMap *convert_tuples_by_name(TupleDesc indesc,
												  TupleDesc outdesc);
extern TupleConversionMap *convert_tuples_by_name_attrmap(TupleDesc indesc,
														  TupleDesc outdesc,
														  AttrMap *attrMap);

extern HeapTuple execute_attr_map_tuple(HeapTuple tuple, TupleConversionMap *map);
extern TupleTableSlot *execute_attr_map_slot(AttrMap *attrMap,
											 TupleTableSlot *in_slot,
											 TupleTableSlot *out_slot);
extern Bitmapset *execute_attr_map_cols(AttrMap *attrMap, Bitmapset *in_cols);

extern void free_conversion_map(TupleConversionMap *map);

#endif							/* TUPCONVERT_H */
