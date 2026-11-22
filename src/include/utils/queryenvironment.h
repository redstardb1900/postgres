/*-------------------------------------------------------------------------
 *
 * queryenvironment.h
 *	  变更查询环境并检索条目相关实际数据（若有）
 *	  的函数入口。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/queryenvironment.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUERYENVIRONMENT_H
#define QUERYENVIRONMENT_H

#include "access/tupdesc.h"


typedef enum EphemeralNameRelationType
{
	ENR_NAMED_TUPLESTORE		/* 具名 tuplestore 关系；如 deltas */
} EphemeralNameRelationType;

/*
 * 某些短暂具名关系必须匹配某关系（如触发器
 * 转移表），为正确处理缓存计划与 DDL，应携带
 * 该关系 OID。其他情况下 ENR 可能独立于系统
 * 目录中的任何关系，故需能直接存放 TupleDesc。
 * 两者从不同时需要。
 */
typedef struct EphemeralNamedRelationMetadataData
{
	char	   *name;			/* 用于标识关系的名称 */

	/* 下列两字段只用其一 */
	Oid			reliddesc;		/* 取 tupdesc 的关系 oid */
	TupleDesc	tupdesc;		/* 结果行的描述 */

	EphemeralNameRelationType enrtype;	/* 标识关系类型 */
	double		enrtuples;		/* 估计元组数 */
} EphemeralNamedRelationMetadataData;

typedef EphemeralNamedRelationMetadataData *EphemeralNamedRelationMetadata;

/*
 * 短暂具名关系数据；用于解析目录中没有的具名
 * 关系，如 AFTER 触发器中的转移表。
 */
typedef struct EphemeralNamedRelationData
{
	EphemeralNamedRelationMetadataData md;
	void	   *reldata;		/* 执行期访问数据的结构 */
} EphemeralNamedRelationData;

typedef EphemeralNamedRelationData *EphemeralNamedRelation;

/*
 * 在 queryenvironment.c 之外这是不透明结构。意
 * 图是能改实现或加新上下文特性，而不必改现有
 * 特性的既有用法代码。
 * 见上。
 */
typedef struct QueryEnvironment QueryEnvironment;


extern QueryEnvironment *create_queryEnv(void);
extern EphemeralNamedRelationMetadata get_visible_ENR_metadata(QueryEnvironment *queryEnv, const char *refname);
extern void register_ENR(QueryEnvironment *queryEnv, EphemeralNamedRelation enr);
extern void unregister_ENR(QueryEnvironment *queryEnv, const char *name);
extern EphemeralNamedRelation get_ENR(QueryEnvironment *queryEnv, const char *name);
extern TupleDesc ENRMetadataGetTupDesc(EphemeralNamedRelationMetadata enrmd);

#endif							/* QUERYENVIRONMENT_H */
