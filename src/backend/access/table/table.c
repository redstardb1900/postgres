/*-------------------------------------------------------------------------
 *
 * table.c
 *	  与表相关代码的通用例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/table/table.c
 *
 *
 * 说明（NOTES）
 *	  本文件包含实现表访问的 table_ 例程（相对
 *	  于索引等其他关系类型），这些例程独立于
 *	  各个表访问方法。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "storage/lmgr.h"

static inline void validate_relation_kind(Relation r);

/* ----------------
 *		table_open - 按关系 OID 打开表关系
 *
 *		这本质上是 relation_open，再检查该关系
 *		既不是索引也不是复合类型。（调用方还应
 *		在假定它拥有存储之前，检查它不是视图或
 *		外部表。）
 * ----------------
 */
Relation
table_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_open(relationId, lockmode);

	validate_relation_kind(r);

	return r;
}


/* ----------------
 *		try_table_open - 按关系 OID 打开表关系
 *
 *		与 table_open 相同，只是关系不存在时返回 NULL，
 *		而不是失败。
 * ----------------
 */
Relation
try_table_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	r = try_relation_open(relationId, lockmode);

	/* 表不存在则离开 */
	if (!r)
		return NULL;

	validate_relation_kind(r);

	return r;
}

/* ----------------
 *		table_openrv - 打开由 RangeVar 节点
 *		指定的表关系
 *
 *		同上，但关系由 RangeVar 指定。
 * ----------------
 */
Relation
table_openrv(const RangeVar *relation, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_openrv(relation, lockmode);

	validate_relation_kind(r);

	return r;
}

/* ----------------
 *		table_openrv_extended - 打开由 RangeVar 节点
 *		指定的表关系
 *
 *		同上，但找不到关系时可以选择返回 NULL，
 *		而不是失败。
 * ----------------
 */
Relation
table_openrv_extended(const RangeVar *relation, LOCKMODE lockmode,
					  bool missing_ok)
{
	Relation	r;

	r = relation_openrv_extended(relation, lockmode, missing_ok);

	if (r)
		validate_relation_kind(r);

	return r;
}

/* ----------------
 *		table_close - 关闭表
 *
 *		若 lockmode 不是 "NoLock"，则接着释放指定的锁。
 *
 *		注意，把锁保持到 relation_close 之后往往是合理的；
 *		那种情况下，锁会在事务结束时自动释放。
 *		----------------
 */
void
table_close(Relation relation, LOCKMODE lockmode)
{
	relation_close(relation, lockmode);
}

/* ----------------
 *		validate_relation_kind - 检查关系的种类
 *
 *		确保 relkind 不是索引或复合类型
 * ----------------
 */
static inline void
validate_relation_kind(Relation r)
{
	if (r->rd_rel->relkind == RELKIND_INDEX ||
		r->rd_rel->relkind == RELKIND_PARTITIONED_INDEX ||
		r->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot open relation \"%s\"",
						RelationGetRelationName(r)),
				 errdetail_relkind_not_supported(r->rd_rel->relkind)));
}
