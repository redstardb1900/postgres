/*-------------------------------------------------------------------------
 *
 * partcache.c
 *		用于操作分区信息（缓存于 relcache）的支持
 *		例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/utils/cache/partcache.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_partitioned_table.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "partitioning/partbounds.h"
#include "rewrite/rewriteHandler.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static void RelationBuildPartitionKey(Relation relation);
static List *generate_partition_qual(Relation rel);

/*
 * RelationGetPartitionKey -- 若关系已分区则获取分区键
 *
 * 注意：分区关系创建之后，分区键不允许再更改。
 * RelationClearRelation 明白这一点，只要关系保持打开，
 * 就会在 relcache 重建期间保留 rd_partkey。因此，即使
 * 我们交回的是指向 relcache 条目内部的直接指针，只要
 * 调用者保持关系打开，就可以安全地继续使用
 * 该指针。
 */
PartitionKey
RelationGetPartitionKey(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return NULL;

	if (unlikely(rel->rd_partkey == NULL))
		RelationBuildPartitionKey(rel);

	return rel->rd_partkey;
}

/*
 * RelationBuildPartitionKey
 *		构建关系的分区键数据，并挂接到 relcache
 *
 * 分区键数据是一个复杂结构；为免在 relcache 条目被刷
 * 写时需要释放其中各个元素而编写复杂逻辑，我们为它
 * 提供专属的内存上下文（CacheMemoryContext 的子上
 * 下文），该上下文可被单独轻松删除。为避免本函数中
 * 途出错时在该上下文中泄漏内存，此上下文最初创建为
 *  CurTransactionContext 的子上下文，直到最后不可能再
 * 出错时才重新挂到 CacheMemoryContext 之下。另外，我
 * 们不把这个上下文设为当前上下文，除非在极短的代
 * 码段中进行，因为担心被调用者自行分配的内存会永
 * 久泄漏。
 */
static void
RelationBuildPartitionKey(Relation relation)
{
	Form_pg_partitioned_table form;
	HeapTuple	tuple;
	bool		isnull;
	int			i;
	PartitionKey key;
	AttrNumber *attrs;
	oidvector  *opclass;
	oidvector  *collation;
	ListCell   *partexprs_item;
	Datum		datum;
	MemoryContext partkeycxt,
				oldcxt;
	int16		procnum;

	tuple = SearchSysCache1(PARTRELID,
							ObjectIdGetDatum(RelationGetRelid(relation)));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 RelationGetRelid(relation));

	partkeycxt = AllocSetContextCreate(CurTransactionContext,
									   "partition key",
									   ALLOCSET_SMALL_SIZES);
	MemoryContextCopyAndSetIdentifier(partkeycxt,
									  RelationGetRelationName(relation));

	key = (PartitionKey) MemoryContextAllocZero(partkeycxt,
												sizeof(PartitionKeyData));

	/* 定长属性 */
	form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
	key->strategy = form->partstrat;
	key->partnatts = form->partnatts;

	/* 校验分区策略代码 */
	if (key->strategy != PARTITION_STRATEGY_LIST &&
		key->strategy != PARTITION_STRATEGY_RANGE &&
		key->strategy != PARTITION_STRATEGY_HASH)
		elog(ERROR, "invalid partition strategy \"%c\"", key->strategy);

	/*
	 * 可以相信首个变长属性会映射到目录 C 结构体
	 * 的相应字段，因为之前所有属性均非空且定
	 * 长。
	 */
	attrs = form->partattrs.values;

	/* 但要用麻烦的方式检索后续的变长属性 */
	/* 算子类 */
	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partclass);
	opclass = (oidvector *) DatumGetPointer(datum);

	/* Collation */
	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partcollation);
	collation = (oidvector *) DatumGetPointer(datum);

	/* 表达式 */
	datum = SysCacheGetAttr(PARTRELID, tuple,
							Anum_pg_partitioned_table_partexprs, &isnull);
	if (!isnull)
	{
		char	   *exprString;
		Node	   *expr;

		exprString = TextDatumGetCString(datum);
		expr = stringToNode(exprString);
		pfree(exprString);

		/*
		 * 先对表达式做常量简化，规划器会比较它们与相似
		 * 处理的 qual 子句操作数，没有这一步就检测不到有
		 * 效匹配；顺便修复 opfuncids。
		 * 不必费心 canonicalize_qual()，因为分区
		 * 表达式应当已是规范形式（即无需 OR 合并或
		 * 常量
		 * 消除）。
		 */
		expr = eval_const_expressions(NULL, expr);
		fix_opfuncids(expr);

		oldcxt = MemoryContextSwitchTo(partkeycxt);
		key->partexprs = (List *) copyObject(expr);
		MemoryContextSwitchTo(oldcxt);
	}

	/* 在 partkeycxt 中分配下面要填充的各个数组 */
	oldcxt = MemoryContextSwitchTo(partkeycxt);
	key->partattrs = (AttrNumber *) palloc0(key->partnatts * sizeof(AttrNumber));
	key->partopfamily = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->partopcintype = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->partsupfunc = (FmgrInfo *) palloc0(key->partnatts * sizeof(FmgrInfo));

	key->partcollation = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->parttypid = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	key->parttypmod = (int32 *) palloc0(key->partnatts * sizeof(int32));
	key->parttyplen = (int16 *) palloc0(key->partnatts * sizeof(int16));
	key->parttypbyval = (bool *) palloc0(key->partnatts * sizeof(bool));
	key->parttypalign = (char *) palloc0(key->partnatts * sizeof(char));
	key->parttypcoll = (Oid *) palloc0(key->partnatts * sizeof(Oid));
	MemoryContextSwitchTo(oldcxt);

	/* 确定要查找的支持函数号 */
	procnum = (key->strategy == PARTITION_STRATEGY_HASH) ?
		HASHEXTENDED_PROC : BTORDER_PROC;

	/* 复制 partattrs 并填写其他属性信息 */
	memcpy(key->partattrs, attrs, key->partnatts * sizeof(int16));
	partexprs_item = list_head(key->partexprs);
	for (i = 0; i < key->partnatts; i++)
	{
		AttrNumber	attno = key->partattrs[i];
		HeapTuple	opclasstup;
		Form_pg_opclass opclassform;
		Oid			funcid;

		/* 收集算子族信息 */
		opclasstup = SearchSysCache1(CLAOID,
									 ObjectIdGetDatum(opclass->values[i]));
		if (!HeapTupleIsValid(opclasstup))
			elog(ERROR, "cache lookup failed for opclass %u", opclass->values[i]);

		opclassform = (Form_pg_opclass) GETSTRUCT(opclasstup);
		key->partopfamily[i] = opclassform->opcfamily;
		key->partopcintype[i] = opclassform->opcintype;

		/* 为指定算子族与数据类型获取支持函数 */
		funcid = get_opfamily_proc(opclassform->opcfamily,
								   opclassform->opcintype,
								   opclassform->opcintype,
								   procnum);
		if (!OidIsValid(funcid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator class \"%s\" of access method %s is missing support function %d for type %s",
							NameStr(opclassform->opcname),
							(key->strategy == PARTITION_STRATEGY_HASH) ?
							"hash" : "btree",
							procnum,
							format_type_be(opclassform->opcintype))));

		fmgr_info_cxt(funcid, &key->partsupfunc[i], partkeycxt);

		/* Collation */
		key->partcollation[i] = collation->values[i];

		/* 收集类型信息 */
		if (attno != 0)
		{
			Form_pg_attribute att = TupleDescAttr(relation->rd_att, attno - 1);

			key->parttypid[i] = att->atttypid;
			key->parttypmod[i] = att->atttypmod;
			key->parttypcoll[i] = att->attcollation;
		}
		else
		{
			if (partexprs_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");

			key->parttypid[i] = exprType(lfirst(partexprs_item));
			key->parttypmod[i] = exprTypmod(lfirst(partexprs_item));
			key->parttypcoll[i] = exprCollation(lfirst(partexprs_item));

			partexprs_item = lnext(key->partexprs, partexprs_item);
		}
		get_typlenbyvalalign(key->parttypid[i],
							 &key->parttyplen[i],
							 &key->parttypbyval[i],
							 &key->parttypalign[i]);

		ReleaseSysCache(opclasstup);
	}

	ReleaseSysCache(tuple);

	/* 断言在下面的赋值期间不会泄漏任何旧数据 */
	Assert(relation->rd_partkeycxt == NULL);
	Assert(relation->rd_partkey == NULL);

	/*
	 * 成功 --- 重新挂接我们的上下文，并让 relcache 指向
	 * 新构建的键。
	 */
	MemoryContextSetParent(partkeycxt, CacheMemoryContext);
	relation->rd_partkeycxt = partkeycxt;
	relation->rd_partkey = key;
}

/*
 * RelationGetPartitionQual
 *
 * 返回分区约束条件列表
 */
List *
RelationGetPartitionQual(Relation rel)
{
	/*快速退出*/
	if (!rel->rd_rel->relispartition)
		return NIL;

	return generate_partition_qual(rel);
}

/*
 * get_partition_qual_relid
 *
 * 返回描述传入关系之分区约束条件的表达式
 * 树。
 *
 * 若找不到关系、或非分区、或无分区约束，则返回
 *  NULL。我们必须防范前两种情况，因为本函数支持的
 *  SQL 函数可能被传入任意 OID。最后一种情况即使在
 *  relispartition 为真时也会发生，即当默认分区是唯一
 * 的分区时。
 */
Expr *
get_partition_qual_relid(Oid relid)
{
	Expr	   *result = NULL;

	/* 仅当该关系存在且是分区时才执行。 */
	if (get_rel_relispartition(relid))
	{
		Relation	rel = relation_open(relid, AccessShareLock);
		List	   *and_args;

		and_args = generate_partition_qual(rel);

		/* 把隐式 AND 列表格式转换为布尔表达式 */
		if (and_args == NIL)
			result = NULL;
		else if (list_length(and_args) > 1)
			result = makeBoolExpr(AND_EXPR, and_args, -1);
		else
			result = linitial(and_args);

		/* 保持锁，让调用者能安全地反解析该关系。 */
		relation_close(rel, NoLock);
	}

	return result;
}

/*
 * generate_partition_qual
 *
 * 从关系的分区边界表达式生成分区谓词。若无谓词，
 * 本函数返回 NIL 列表。
 *
 * 我们会在 relcache 条目中缓存一份结果副本，先用调用
 * 者上下文构建。这样可避免把任何数据泄漏到
 * 长寿命的缓存上下文中，尤其当我们在中途失败时。
 */
static List *
generate_partition_qual(Relation rel)
{
	HeapTuple	tuple;
	MemoryContext oldcxt;
	Datum		boundDatum;
	bool		isnull;
	List	   *my_qual = NIL,
			   *result = NIL;
	Oid			parentrelid;
	Relation	parent;

	/* 防范因分区树过深导致的栈溢出 */
	check_stack_depth();

	/* 若已缓存结果，则只返回一份副本 */
	if (rel->rd_partcheckvalid)
		return copyObject(rel->rd_partcheck);

	/*
	 * 至少以 AccessShareLock 锁定父表。即便该分区已被
	 * 部分分离，也必须这么做，因为与分离并发的某事务
	 * 可能仍在试图使用包含它的分区描述
	 * 符。
	 */
	parentrelid = get_partition_parent(RelationGetRelid(rel), true);
	parent = relation_open(parentrelid, AccessShareLock);

	/* 取 pg_class.relpartbound */
	tuple = SearchSysCache1(RELOID, RelationGetRelid(rel));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

	boundDatum = SysCacheGetAttr(RELOID, tuple,
								 Anum_pg_class_relpartbound,
								 &isnull);
	if (!isnull)
	{
		PartitionBoundSpec *bound;

		bound = castNode(PartitionBoundSpec,
						 stringToNode(TextDatumGetCString(boundDatum)));

		my_qual = get_qual_from_partbound(parent, bound);
	}

	ReleaseSysCache(tuple);

	/* 把父方约束加入列表（如有） */
	if (parent->rd_rel->relispartition)
		result = list_concat(generate_partition_qual(parent), my_qual);
	else
		result = my_qual;

	/*
	 * 把 Var 改为使用分区的 attnos 而非父方的。这一步要
	 * 在拼接父方约束之后再做，因为我们希望其中的每个
	 *  Var 都携带本关系的 attnos。此处可安全假定 varno
	 * =1。
	 */
	result = map_partition_varattnos(result, 1, rel, parent);

	/* 断言在下面的赋值期间不会泄漏任何旧数据 */
	Assert(rel->rd_partcheckcxt == NULL);
	Assert(rel->rd_partcheck == NIL);

	/*
	 * relcache 中保存一份副本。操作顺序很关键，为避免
	 * 内存泄漏并确保即便 copyObject 中途失败也不会留下
	 * 损坏的 relcache 条目。
	 *
	 * 如果 partcheck 列表为 NIL（这完全可能），那么我们
	 * 无需为它创建上下文。
	 */
	if (result != NIL)
	{
		rel->rd_partcheckcxt = AllocSetContextCreate(CacheMemoryContext,
													 "partition constraint",
													 ALLOCSET_SMALL_SIZES);
		MemoryContextCopyAndSetIdentifier(rel->rd_partcheckcxt,
										  RelationGetRelationName(rel));
		oldcxt = MemoryContextSwitchTo(rel->rd_partcheckcxt);
		rel->rd_partcheck = copyObject(result);
		MemoryContextSwitchTo(oldcxt);
	}
	else
		rel->rd_partcheck = NIL;
	rel->rd_partcheckvalid = true;

	/* 保持父表锁定直至提交 */
	relation_close(parent, NoLock);

	/* 把工作副本返回给调用者 */
	return result;
}
