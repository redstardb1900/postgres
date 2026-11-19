/*-------------------------------------------------------------------------
 *
 * amvalidate.h
 *	  支持索引访问方法 amvalidate 与 amadjustmembers
 *	  函数的例程。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * src/include/access/amvalidate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMVALIDATE_H
#define AMVALIDATE_H

#include "utils/catcache.h"


/* identify_opfamily_groups() 返回的结构体(列表中) */
typedef struct OpFamilyOpFuncGroup
{
	Oid			lefttype;		/* amoplefttype/amproclefttype */
	Oid			righttype;		/* amoprighttype/amprocrighttype */
	uint64		operatorset;	/* 这些类型的操作符位掩码 */
	uint64		functionset;	/* 这些类型的支持函数位掩码 */
} OpFamilyOpFuncGroup;


/* access/index/amvalidate.c 中的函数 */
extern List *identify_opfamily_groups(CatCList *oprlist, CatCList *proclist);
extern bool check_amproc_signature(Oid funcid, Oid restype, bool exact,
								   int minargs, int maxargs,...);
extern bool check_amoptsproc_signature(Oid funcid);
extern bool check_amop_signature(Oid opno, Oid restype,
								 Oid lefttype, Oid righttype);
extern Oid	opclass_for_family_datatype(Oid amoid, Oid opfamilyoid,
										Oid datatypeoid);
extern bool opfamily_can_sort_type(Oid opfamilyoid, Oid datatypeoid);

#endif							/* AMVALIDATE_H */
