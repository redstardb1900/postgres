/*-------------------------------------------------------------------------
 *
 * partition.c
 *		  与分区相关的数据结构和函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		  src/backend/catalog/partition.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/attmap.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_partitioned_table.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "partitioning/partbounds.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid	get_partition_parent_worker(Relation inhRel, Oid relid,
										bool *detach_pending);
static void get_partition_ancestors_worker(Relation inhRel, Oid relid,
										   List **ancestors);

/*
 * get_partition_parent
 *		获取给定关系的直接父关系
 *
 * 通过扫描 pg_inherits 返回分区的继承父关系
 *
 * 若分区正处于分离过程中，则报错，
 * 除非 even_if_detached 传入为 true。
 *
 * 注意：本函数假定作为参数传入 OID 的关系
 * 恰好有一个父关系，因此仅应在已知该关系
 * 是分区时调用。
 */
Oid
get_partition_parent(Oid relid, bool even_if_detached)
{
	Relation	catalogRelation;
	Oid			result;
	bool		detach_pending;

	catalogRelation = table_open(InheritsRelationId, AccessShareLock);

	result = get_partition_parent_worker(catalogRelation, relid,
										 &detach_pending);

	if (!OidIsValid(result))
		elog(ERROR, "could not find tuple for parent of relation %u", relid);

	if (detach_pending && !even_if_detached)
		elog(ERROR, "relation %u has no parent because it's being detached",
			 relid);

	table_close(catalogRelation, AccessShareLock);

	return result;
}

/*
 * get_partition_parent_worker
 *		扫描 pg_inherits 关系，返回给定
 *		关系的父关系 OID
 *
 * 若分区正在被分离，则将 *detach_pending 置为 true（但仍返回
 * 原来的父关系。）
 */
static Oid
get_partition_parent_worker(Relation inhRel, Oid relid, bool *detach_pending)
{
	SysScanDesc scan;
	ScanKeyData key[2];
	Oid			result = InvalidOid;
	HeapTuple	tuple;

	*detach_pending = false;

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_inherits_inhseqno,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(1));

	scan = systable_beginscan(inhRel, InheritsRelidSeqnoIndexId, true,
							  NULL, 2, key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_inherits form = (Form_pg_inherits) GETSTRUCT(tuple);

		/* 告知调用方分区正在被分离 */
		if (form->inhdetachpending)
			*detach_pending = true;
		result = form->inhparent;
	}

	systable_endscan(scan);

	return result;
}

/*
 * get_partition_ancestors
 *		获取给定关系的祖先
 *
 * 返回给定关系的祖先列表。
 *
 * 注意：本函数假定作为参数传入 OID 的关系
 * 以及每个祖先都恰好有一个父关系，因此仅应
 * 在已知该关系是分区时调用。
 */
List *
get_partition_ancestors(Oid relid)
{
	List	   *result = NIL;
	Relation	inhRel;

	inhRel = table_open(InheritsRelationId, AccessShareLock);

	get_partition_ancestors_worker(inhRel, relid, &result);

	table_close(inhRel, AccessShareLock);

	return result;
}

/*
 * get_partition_ancestors_worker
 *		get_partition_ancestors 的递归工作函数
 */
static void
get_partition_ancestors_worker(Relation inhRel, Oid relid, List **ancestors)
{
	Oid			parentOid;
	bool		detach_pending;

	/*
	 * 递归在最顶层结束，即没有父关系时结束；分区
	 * 正在被分离时也结束。
	 */
	parentOid = get_partition_parent_worker(inhRel, relid, &detach_pending);
	if (parentOid == InvalidOid || detach_pending)
		return;

	*ancestors = lappend_oid(*ancestors, parentOid);
	get_partition_ancestors_worker(inhRel, parentOid, ancestors);
}

/*
 * index_get_partition
 *		返回给定分区上、作为给定索引子索引的
 *		那个索引的 OID；若不存在则返回 InvalidOid。
 */
Oid
index_get_partition(Relation partition, Oid indexId)
{
	List	   *idxlist = RelationGetIndexList(partition);
	ListCell   *l;

	foreach(l, idxlist)
	{
		Oid			partIdx = lfirst_oid(l);
		HeapTuple	tup;
		Form_pg_class classForm;
		bool		ispartition;

		tup = SearchSysCache1(RELOID, ObjectIdGetDatum(partIdx));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for relation %u", partIdx);
		classForm = (Form_pg_class) GETSTRUCT(tup);
		ispartition = classForm->relispartition;
		ReleaseSysCache(tup);
		if (!ispartition)
			continue;
		if (get_partition_parent(partIdx, false) == indexId)
		{
			list_free(idxlist);
			return partIdx;
		}
	}

	list_free(idxlist);
	return InvalidOid;
}

/*
 * map_partition_varattnos - 将 'expr' 中所有 Vars（其
 * varno 为 'fromrel_varno'）的 varattnos 从 'from_rel' 的 attnums
 * 映射到 'to_rel' 的 attnums；两者都可以是叶子分区或分区
 * 表，但必须属于同一分区层次结构。
 *
 * 需要这样做是因为：尽管层次结构中所有关系都必须有
 * 相同的列名，且类型也必须相同，但 attnums
 * 可能不同。
 *
 * 注意：这对任意节点树都适用，因此参数和结果
 * 其实应声明为 "Node *"。但绝大多数调用方
 * 处理的是 List，在内部做转换更省事。
 */
List *
map_partition_varattnos(List *expr, int fromrel_varno,
						Relation to_rel, Relation from_rel)
{
	if (expr != NIL)
	{
		AttrMap    *part_attmap;
		bool		found_whole_row;

		part_attmap = build_attrmap_by_name(RelationGetDescr(to_rel),
											RelationGetDescr(from_rel),
											false);
		expr = (List *) map_variable_attnos((Node *) expr,
											fromrel_varno, 0,
											part_attmap,
											RelationGetForm(to_rel)->reltype,
											&found_whole_row);
		/* 由于我们提供了 to_rowtype，可以忽略 found_whole_row。 */
	}

	return expr;
}

/*
 * 检查 'attnums' 中是否有任一属性是 rel 的分区键属性
 *
 * 若发现 'attnums' 中任一属性被某个分区键表达式引用，
 * 则设置 *used_in_expr。一列可能既被直接使用
 * 也作为表达式的一部分；若出现这种情况，*used_in_expr 可能
 * 最终为 true 或 false。对当前用途这没关系，
 * 因为 *used_in_expr 仅用于调整错误消息
 * 文本。
 */
bool
has_partition_attrs(Relation rel, Bitmapset *attnums, bool *used_in_expr)
{
	PartitionKey key;
	int			partnatts;
	List	   *partexprs;
	ListCell   *partexprs_item;
	int			i;

	if (attnums == NULL || rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return false;

	key = RelationGetPartitionKey(rel);
	partnatts = get_partition_natts(key);
	partexprs = get_partition_exprs(key);

	partexprs_item = list_head(partexprs);
	for (i = 0; i < partnatts; i++)
	{
		AttrNumber	partattno = get_partition_col_attnum(key, i);

		if (partattno != 0)
		{
			if (bms_is_member(partattno - FirstLowInvalidHeapAttributeNumber,
							  attnums))
			{
				if (used_in_expr)
					*used_in_expr = false;
				return true;
			}
		}
		else
		{
			/* 任意表达式 */
			Node	   *expr = (Node *) lfirst(partexprs_item);
			Bitmapset  *expr_attrs = NULL;

			/* 找出所有被引用的属性 */
			pull_varattnos(expr, 1, &expr_attrs);
			partexprs_item = lnext(partexprs, partexprs_item);

			if (bms_overlap(attnums, expr_attrs))
			{
				if (used_in_expr)
					*used_in_expr = true;
				return true;
			}
		}
	}

	return false;
}

/*
 * get_default_partition_oid
 *
 * 给定关系 OID，返回默认分区的 OID（若存在）。
 * 为效率起见，尽可能使用
 * get_default_oid_from_partdesc。
 */
Oid
get_default_partition_oid(Oid parentId)
{
	HeapTuple	tuple;
	Oid			defaultPartId = InvalidOid;

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(parentId));

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_partitioned_table part_table_form;

		part_table_form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
		defaultPartId = part_table_form->partdefid;
		ReleaseSysCache(tuple);
	}

	return defaultPartId;
}

/*
 * update_default_partition_oid
 *
 * 用新的默认分区 OID 更新 pg_partitioned_table.partdefid。
 */
void
update_default_partition_oid(Oid parentId, Oid defaultPartId)
{
	HeapTuple	tuple;
	Relation	pg_partitioned_table;
	Form_pg_partitioned_table part_table_form;

	pg_partitioned_table = table_open(PartitionedRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(parentId));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 parentId);

	part_table_form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
	part_table_form->partdefid = defaultPartId;
	CatalogTupleUpdate(pg_partitioned_table, &tuple->t_self, tuple);

	heap_freetuple(tuple);
	table_close(pg_partitioned_table, RowExclusiveLock);
}

/*
 * get_proposed_default_constraint
 *
 * 本函数返回 new_part_constraints 的否定形式；在加入
 * 该 new_part_constraints 所属分区之后，它将成为默认
 * 分区约束的组成部分。
 */
List *
get_proposed_default_constraint(List *new_part_constraints)
{
	Expr	   *defPartConstraint;

	defPartConstraint = make_ands_explicit(new_part_constraints);

	/*
	 * 通过对给定分区约束取反，导出默认分区的
	 * 分区约束。分区约束从不会求值为
	 * NULL，因此这样取反是安全的。
	 */
	defPartConstraint = makeBoolExpr(NOT_EXPR,
									 list_make1(defPartConstraint),
									 -1);

	/* 化简，使取反后的表达式成为规范形式 */
	defPartConstraint =
		(Expr *) eval_const_expressions(NULL,
										(Node *) defPartConstraint);
	defPartConstraint = canonicalize_qual(defPartConstraint, true);

	return make_ands_implicit(defPartConstraint);
}
