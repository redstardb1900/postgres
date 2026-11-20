/*-------------------------------------------------------------------------
 *
 * attmap.h
 *	  PostgreSQL 属性映射的定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/attmap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ATTMAP_H
#define ATTMAP_H

#include "access/attnum.h"
#include "access/tupdesc.h"

/*
 * 属性映射结构
 *
 * 本结构在一对关系之间映射属性号，二者称为
 * 'input' 与 'output'（最常见为继承父表与子
 * 关系），其共有列可能具有不同的属性号。
 * 此种差异可能因列在两关系中次序不同，
 * 或两关系有已删除列位于
 * 不同位置。
 *
 * 'maplen' 设为 'output' 关系的属性个数，
 * 计入其任何已删除属性，并将对应的
 * 'attnums' 数组元素设为 0。
 */
typedef struct AttrMap
{
	AttrNumber *attnums;
	int			maplen;
} AttrMap;

extern AttrMap *make_attrmap(int maplen);
extern void free_attrmap(AttrMap *map);

/* 构建映射的转换例程 */
extern AttrMap *build_attrmap_by_name(TupleDesc indesc,
									  TupleDesc outdesc,
									  bool missing_ok);
extern AttrMap *build_attrmap_by_name_if_req(TupleDesc indesc,
											 TupleDesc outdesc,
											 bool missing_ok);
extern AttrMap *build_attrmap_by_position(TupleDesc indesc,
										  TupleDesc outdesc,
										  const char *msg);

#endif							/* ATTMAP_H */
