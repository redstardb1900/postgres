/*-------------------------------------------------------------------------
 *
 * partcache.h
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/partcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARTCACHE_H
#define PARTCACHE_H

#include "access/attnum.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "partitioning/partdefs.h"
#include "utils/relcache.h"

/*
 * 关系分区键的信息
 */
typedef struct PartitionKeyData
{
	PartitionStrategy strategy; /* 分区策略 */
	int16		partnatts;		/* 分区键中的列数 */
	AttrNumber *partattrs;		/* 分区键中列的属性号，
								 * 表达式则为 0 */
	List	   *partexprs;		/* 分区键中的表达式列表，
								 * 每个零值 partattrs 一项 */

	Oid		   *partopfamily;	/* 操作符族的 OIDs */
	Oid		   *partopcintype;	/* opclass 声明输入数据类型的 OIDs */
	FmgrInfo   *partsupfunc;	/* 支持函数的查找信息 */

	/* 每属性的分区 collation */
	Oid		   *partcollation;

	/* 每属性的类型信息 */
	Oid		   *parttypid;
	int32	   *parttypmod;
	int16	   *parttyplen;
	bool	   *parttypbyval;
	char	   *parttypalign;
	Oid		   *parttypcoll;
}			PartitionKeyData;


extern PartitionKey RelationGetPartitionKey(Relation rel);
extern List *RelationGetPartitionQual(Relation rel);
extern Expr *get_partition_qual_relid(Oid relid);

/*
 * PartitionKey 查询函数
 */
static inline int
get_partition_strategy(PartitionKey key)
{
	return key->strategy;
}

static inline int
get_partition_natts(PartitionKey key)
{
	return key->partnatts;
}

static inline List *
get_partition_exprs(PartitionKey key)
{
	return key->partexprs;
}

/*
 * PartitionKey 查询函数 - 单列
 */
static inline int16
get_partition_col_attnum(PartitionKey key, int col)
{
	return key->partattrs[col];
}

static inline Oid
get_partition_col_typid(PartitionKey key, int col)
{
	return key->parttypid[col];
}

static inline int32
get_partition_col_typmod(PartitionKey key, int col)
{
	return key->parttypmod[col];
}

static inline Oid
get_partition_col_collation(PartitionKey key, int col)
{
	return key->partcollation[col];
}

#endif							/* PARTCACHE_H */
