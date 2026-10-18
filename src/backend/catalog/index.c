/*-------------------------------------------------------------------------
 *
 * index.c
 *	  创建与销毁 POSTGRES 索引关系的代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/index.c
 *
 *
 * INTERFACE ROUTINES
 *		index_create()			- 创建已编目的索引关系
 *		index_drop()			- 从目录中移除索引关系
 *		BuildIndexInfo()		- 准备插入索引元组
 *		FormIndexDatum()		- 为一个索引元组构造数据值向量
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/event_trigger.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "pgstat.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

/* 可能由 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_index_pg_class_oid = InvalidOid;
RelFileNumber binary_upgrade_next_index_pg_class_relfilenumber =
InvalidRelFileNumber;

/*
 * 在重建系统目录索引时所使用变量的无指针表示形式；
 * 我们用它把这些值传播给并行工作进程。
 */
typedef struct
{
	Oid			currentlyReindexedHeap;
	Oid			currentlyReindexedIndex;
	int			numPendingReindexedIndexes;
	Oid			pendingReindexedIndexes[FLEXIBLE_ARRAY_MEMBER];
} SerializedReindexState;

/* 非导出函数的原型 */
static bool relationHasPrimaryKey(Relation rel);
static TupleDesc ConstructTupleDescriptor(Relation heapRelation,
										  IndexInfo *indexInfo,
										  List *indexColNames,
										  Oid accessMethodObjectId,
										  Oid *collationObjectId,
										  Oid *classObjectId);
static void InitializeAttributeOids(Relation indexRelation,
									int numatts, Oid indexoid);
static void AppendAttributeTuples(Relation indexRelation, Datum *attopts);
static void UpdateIndexRelation(Oid indexoid, Oid heapoid,
								Oid parentIndexId,
								IndexInfo *indexInfo,
								Oid *collationOids,
								Oid *classOids,
								int16 *coloptions,
								bool primary,
								bool isexclusion,
								bool immediate,
								bool isvalid,
								bool isready);
static void index_update_stats(Relation rel,
							   bool hasindex,
							   double reltuples);
static void IndexCheckExclusion(Relation heapRelation,
								Relation indexRelation,
								IndexInfo *indexInfo);
static bool validate_index_callback(ItemPointer itemptr, void *opaque);
static bool ReindexIsCurrentlyProcessingIndex(Oid indexOid);
static void SetReindexProcessing(Oid heapOid, Oid indexOid);
static void ResetReindexProcessing(void);
static void SetReindexPending(List *indexes);
static void RemoveReindexPending(Oid indexOid);


/*
 * relationHasPrimaryKey
 *		查看一个现有关系是否具有主键。
 *
 * 调用方必须已对该关系持有适当的锁。
 *
 * 注意：我们在此有意不检查 indisvalid；这是因为本函数用于强制执行
 * 至多只能有一个 indisprimary 索引的规则，而且我们希望即使该索引
 * 无效时该规则也成立。
 */
static bool
relationHasPrimaryKey(Relation rel)
{
	bool		result = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;

	/*
	 * 从关系缓存获取该表的索引 OID 列表，并逐个在
	 * pg_index 系统缓存中查找，直到找到被标记为主键的那个
	 * （希望这样的索引不超过一个）。
	 */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))	/* 不应发生 */
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		result = ((Form_pg_index) GETSTRUCT(indexTuple))->indisprimary;
		ReleaseSysCache(indexTuple);
		if (result)
			break;
	}

	list_free(indexoidlist);

	return result;
}

/*
 * index_check_primary_key
 *		在创建 PRIMARY KEY 索引之前应用所需的特殊检查
 *
 * 这段处理过去位于 DefineIndex() 中，但现在已经拆分出来，
 * 以便在 ALTER TABLE ADD PRIMARY KEY USING INDEX 期间也能应用。
 *
 * 我们检查是否已存在主键，索引的所有列是否都是简单列引用
 * （而非表达式），以及所有这些列是否都标记为 NOT NULL。
 * 若不是，则失败。
 *
 * 过去我们在此通过执行自己的局部 ALTER TABLE 命令，自动将未标记的列改为
 * NOT NULL。但如果我们正在执行某个 ALTER TABLE 子命令，这种做法效果并不
 * 好：各操作整体上未必能按正确的顺序执行。现在我们期望解析器在尝试
 * 创建主键索引之前，
 * 已经插入了所有必需的
 * ALTER TABLE SET NOT NULL 操作。
 *
 * 调用方最好至少对该表持有 ShareLock，否则
 * 非空检查就不可信。
 */
void
index_check_primary_key(Relation heapRel,
						IndexInfo *indexInfo,
						bool is_alter_table,
						IndexStmt *stmt)
{
	int			i;

	/*
	 * 如果是 ALTER TABLE 或 CREATE TABLE .. PARTITION OF，检查是否已经存在
	 * PRIMARY KEY。对于普通关系的 CREATE TABLE，我们相信解析器
	 * 已拒绝了多个 pkey 子句；而 CREATE INDEX 没有办法声明 PRIMARY KEY，
	 * 因此那也不成问题。
	 */
	if ((is_alter_table || heapRel->rd_rel->relispartition) &&
		relationHasPrimaryKey(heapRel))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("multiple primary keys for table \"%s\" are not allowed",
						RelationGetRelationName(heapRel))));
	}

	/*
	 * 使用 NULLS NOT DISTINCT 创建的索引不能用于主键约束。
	 * 虽然没有直接的语法能走到这里，
	 * 但可以先创建一个单独的索引，再通过 ALTER TABLE ..
	 * USING INDEX 将其附加过来。
	 */
	if (indexInfo->ii_NullsNotDistinct)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("primary keys cannot use NULLS NOT DISTINCT indexes")));
	}

	/*
	 * 检查主键中的所有属性是否都标记为非空。
	 * （我们并不真的期望看到这种情况；那意味着解析器出了问题，
	 * 但检查一下似乎是明智的。）
	 */
	for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
	{
		AttrNumber	attnum = indexInfo->ii_IndexAttrNumbers[i];
		HeapTuple	atttuple;
		Form_pg_attribute attform;

		if (attnum == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("primary keys cannot be expressions")));

		/* 系统属性永远不为空，因此无需检查 */
		if (attnum < 0)
			continue;

		atttuple = SearchSysCache2(ATTNUM,
								   ObjectIdGetDatum(RelationGetRelid(heapRel)),
								   Int16GetDatum(attnum));
		if (!HeapTupleIsValid(atttuple))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, RelationGetRelid(heapRel));
		attform = (Form_pg_attribute) GETSTRUCT(atttuple);

		if (!attform->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("primary key column \"%s\" is not marked NOT NULL",
							NameStr(attform->attname))));

		ReleaseSysCache(atttuple);
	}
}

/*
 *		ConstructTupleDescriptor
 *
 * 为新索引构建索引元组描述符
 */
static TupleDesc
ConstructTupleDescriptor(Relation heapRelation,
						 IndexInfo *indexInfo,
						 List *indexColNames,
						 Oid accessMethodObjectId,
						 Oid *collationObjectId,
						 Oid *classObjectId)
{
	int			numatts = indexInfo->ii_NumIndexAttrs;
	int			numkeyatts = indexInfo->ii_NumIndexKeyAttrs;
	ListCell   *colnames_item = list_head(indexColNames);
	ListCell   *indexpr_item = list_head(indexInfo->ii_Expressions);
	IndexAmRoutine *amroutine;
	TupleDesc	heapTupDesc;
	TupleDesc	indexTupDesc;
	int			natts;			/* 堆关系中的属性数 --- 用于错误检查 */
	int			i;

	/* 我们需要访问索引访问方法的 API 结构体 */
	amroutine = GetIndexAmRoutineByAmId(accessMethodObjectId, false);

	/* ... 以及表的元组描述符 */
	heapTupDesc = RelationGetDescr(heapRelation);
	natts = RelationGetForm(heapRelation)->relnatts;

	/*
	 * 分配新的元组描述符
	 */
	indexTupDesc = CreateTemplateTupleDesc(numatts);

	/*
	 * 填写 pg_attribute 行。
	 */
	for (i = 0; i < numatts; i++)
	{
		AttrNumber	atnum = indexInfo->ii_IndexAttrNumbers[i];
		Form_pg_attribute to = TupleDescAttr(indexTupDesc, i);
		HeapTuple	tuple;
		Form_pg_type typeTup;
		Form_pg_opclass opclassTup;
		Oid			keyType;

		MemSet(to, 0, ATTRIBUTE_FIXED_PART_SIZE);
		to->attnum = i + 1;
		to->attstattarget = -1;
		to->attcacheoff = -1;
		to->attislocal = true;
		to->attcollation = (i < numkeyatts) ?
			collationObjectId[i] : InvalidOid;

		/*
		 * 按照调用方指定的那样设置属性名。
		 */
		if (colnames_item == NULL)	/* 不应发生 */
			elog(ERROR, "too few entries in colnames list");
		namestrcpy(&to->attname, (const char *) lfirst(colnames_item));
		colnames_item = lnext(indexColNames, colnames_item);

		/*
		 * 对于简单索引列，我们从父关系复制某些 pg_attribute 字段。
		 * 对于表达式，
		 * 我们必须查看表达式求值的结果。
		 */
		if (atnum != 0)
		{
			/* 简单索引列 */
			const FormData_pg_attribute *from;

			Assert(atnum > 0);	/* 本应在上面被检查出来 */

			if (atnum > natts)	/* 安全性检查 */
				elog(ERROR, "invalid column number %d", atnum);
			from = TupleDescAttr(heapTupDesc,
								 AttrNumberGetAttrOffset(atnum));

			to->atttypid = from->atttypid;
			to->attlen = from->attlen;
			to->attndims = from->attndims;
			to->atttypmod = from->atttypmod;
			to->attbyval = from->attbyval;
			to->attalign = from->attalign;
			to->attstorage = from->attstorage;
			to->attcompression = from->attcompression;
		}
		else
		{
			/* 表达式索引 */
			Node	   *indexkey;

			if (indexpr_item == NULL)	/* 不应发生 */
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexInfo->ii_Expressions, indexpr_item);

			/*
			 * 在 pg_type 中查找表达式的类型，以获得类型长度等信息。
			 */
			keyType = exprType(indexkey);
			tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(keyType));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for type %u", keyType);
			typeTup = (Form_pg_type) GETSTRUCT(tuple);

			/*
			 * 赋予部分属性值，其余的留待以后设置。
			 */
			to->atttypid = keyType;
			to->attlen = typeTup->typlen;
			to->atttypmod = exprTypmod(indexkey);
			to->attbyval = typeTup->typbyval;
			to->attalign = typeTup->typalign;
			to->attstorage = typeTup->typstorage;

			/*
			 * 对于表达式列，将 attcompression 设为无效，因为
			 * 不存在可供复制其值的表列。无论何时我们真正需要
			 * 压缩某个值，
			 * 都将使用那一刻 default_toast_compression 的
			 * 当前取值。
			 */
			to->attcompression = InvalidCompressionMethod;

			ReleaseSysCache(tuple);

			/*
			 * 确保表达式产生的类型可以安全地存储在索引中。
			 * 我们需要这层防御，因为我们要为 "record" 这类伪类型
			 * 建立索引操作符类，而实际存储的类型最好必须是安全的；
			 * 例如具名复合类型可以，匿名 record 类型则不行。
			 * 该测试与判断表列是否为安全类型
			 * 所用的测试相同（这就是为什么对非表达式的情况
			 * 我们无需检查的原因）。
			 */
			CheckAttributeType(NameStr(to->attname),
							   to->atttypid, to->attcollation,
							   NIL, 0);
		}

		/*
		 * 我们还没有索引的正确关系 OID，
		 * 所以暂时将其设为无效。
		 * InitializeAttributeOids() 稍后会修正它。
		 */
		to->attrelid = InvalidOid;

		/*
		 * 检查操作符类和索引访问方法，
		 * 看二者是否提供了键类型（从而覆盖属性类型）。
		 * 操作符类（如果存在）优先。
		 */
		keyType = amroutine->amkeytype;

		if (i < indexInfo->ii_NumIndexKeyAttrs)
		{
			tuple = SearchSysCache1(CLAOID, ObjectIdGetDatum(classObjectId[i]));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for opclass %u",
					 classObjectId[i]);
			opclassTup = (Form_pg_opclass) GETSTRUCT(tuple);
			if (OidIsValid(opclassTup->opckeytype))
				keyType = opclassTup->opckeytype;

			/*
			 * 如果 keytype 指定为 ANYELEMENT，而 opcintype 是 ANYARRAY，
			 * 那么属性类型必须是数组（否则它不会与此操作符类匹配）；
			 * 使用其元素类型。
			 *
			 * 我们在此也可以允许 ANYCOMPATIBLE/ANYCOMPATIBLEARRAY，但
			 * 看来没有这样做的必要；没有理由把操作符类声明为
			 * 接受 ANYCOMPATIBLEARRAY 而不是 ANYARRAY。
			 */
			if (keyType == ANYELEMENTOID && opclassTup->opcintype == ANYARRAYOID)
			{
				keyType = get_base_element_type(to->atttypid);
				if (!OidIsValid(keyType))
					elog(ERROR, "could not get element type of array type %u",
						 to->atttypid);
			}

			ReleaseSysCache(tuple);
		}

		/*
		 * 如果指定了与堆值不同的键类型，则更新索引 tupdesc 中
		 * 与类型相关的字段。
		 */
		if (OidIsValid(keyType) && keyType != to->atttypid)
		{
			tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(keyType));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for type %u", keyType);
			typeTup = (Form_pg_type) GETSTRUCT(tuple);

			to->atttypid = keyType;
			to->atttypmod = -1;
			to->attlen = typeTup->typlen;
			to->attbyval = typeTup->typbyval;
			to->attalign = typeTup->typalign;
			to->attstorage = typeTup->typstorage;
			/* 同上，在这种情况下使用默认压缩方法 */
			to->attcompression = InvalidCompressionMethod;

			ReleaseSysCache(tuple);
		}
	}

	pfree(amroutine);

	return indexTupDesc;
}

/* ----------------------------------------------------------------
 *		InitializeAttributeOids
 * ----------------------------------------------------------------
 */
static void
InitializeAttributeOids(Relation indexRelation,
						int numatts,
						Oid indexoid)
{
	TupleDesc	tupleDescriptor;
	int			i;

	tupleDescriptor = RelationGetDescr(indexRelation);

	for (i = 0; i < numatts; i += 1)
		TupleDescAttr(tupleDescriptor, i)->attrelid = indexoid;
}

/* ----------------------------------------------------------------
 *		AppendAttributeTuples
 * ----------------------------------------------------------------
 */
static void
AppendAttributeTuples(Relation indexRelation, Datum *attopts)
{
	Relation	pg_attribute;
	CatalogIndexState indstate;
	TupleDesc	indexTupDesc;

	/*
	 * 打开属性关系及其索引。
	 */
	pg_attribute = table_open(AttributeRelationId, RowExclusiveLock);

	indstate = CatalogOpenIndexes(pg_attribute);

	/*
	 * 把新索引的 tupdesc 中的数据插入 pg_attribute。
	 */
	indexTupDesc = RelationGetDescr(indexRelation);

	InsertPgAttributeTuples(pg_attribute, indexTupDesc, InvalidOid, attopts, indstate);

	CatalogCloseIndexes(indstate);

	table_close(pg_attribute, RowExclusiveLock);
}

/* ----------------------------------------------------------------
 *		UpdateIndexRelation
 *
 * 构造并在 pg_index 目录中插入一个新条目
 * ----------------------------------------------------------------
 */
static void
UpdateIndexRelation(Oid indexoid,
					Oid heapoid,
					Oid parentIndexId,
					IndexInfo *indexInfo,
					Oid *collationOids,
					Oid *classOids,
					int16 *coloptions,
					bool primary,
					bool isexclusion,
					bool immediate,
					bool isvalid,
					bool isready)
{
	int2vector *indkey;
	oidvector  *indcollation;
	oidvector  *indclass;
	int2vector *indoption;
	Datum		exprsDatum;
	Datum		predDatum;
	Datum		values[Natts_pg_index];
	bool		nulls[Natts_pg_index] = {0};
	Relation	pg_index;
	HeapTuple	tuple;
	int			i;

	/*
	 * 将索引键、操作符类和 indoption 信息复制到数组中
	 * （是否应该让调用方一开始就以这种形式传递它们？）
	 */
	indkey = buildint2vector(NULL, indexInfo->ii_NumIndexAttrs);
	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		indkey->values[i] = indexInfo->ii_IndexAttrNumbers[i];
	indcollation = buildoidvector(collationOids, indexInfo->ii_NumIndexKeyAttrs);
	indclass = buildoidvector(classOids, indexInfo->ii_NumIndexKeyAttrs);
	indoption = buildint2vector(coloptions, indexInfo->ii_NumIndexKeyAttrs);

	/*
	 * 将索引表达式（如果有）转换为文本数据值。
	 */
	if (indexInfo->ii_Expressions != NIL)
	{
		char	   *exprsString;

		exprsString = nodeToString(indexInfo->ii_Expressions);
		exprsDatum = CStringGetTextDatum(exprsString);
		pfree(exprsString);
	}
	else
		exprsDatum = (Datum) 0;

	/*
	 * 将索引谓词（如果有）转换为文本数据值。注意我们为存储
	 * 把隐式 AND 格式转换为普通的显式 AND 格式。
	 */
	if (indexInfo->ii_Predicate != NIL)
	{
		char	   *predString;

		predString = nodeToString(make_ands_explicit(indexInfo->ii_Predicate));
		predDatum = CStringGetTextDatum(predString);
		pfree(predString);
	}
	else
		predDatum = (Datum) 0;


	/*
	 * 打开系统目录索引关系。
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	/*
	 * 构造一个 pg_index 元组。
	 */
	values[Anum_pg_index_indexrelid - 1] = ObjectIdGetDatum(indexoid);
	values[Anum_pg_index_indrelid - 1] = ObjectIdGetDatum(heapoid);
	values[Anum_pg_index_indnatts - 1] = Int16GetDatum(indexInfo->ii_NumIndexAttrs);
	values[Anum_pg_index_indnkeyatts - 1] = Int16GetDatum(indexInfo->ii_NumIndexKeyAttrs);
	values[Anum_pg_index_indisunique - 1] = BoolGetDatum(indexInfo->ii_Unique);
	values[Anum_pg_index_indnullsnotdistinct - 1] = BoolGetDatum(indexInfo->ii_NullsNotDistinct);
	values[Anum_pg_index_indisprimary - 1] = BoolGetDatum(primary);
	values[Anum_pg_index_indisexclusion - 1] = BoolGetDatum(isexclusion);
	values[Anum_pg_index_indimmediate - 1] = BoolGetDatum(immediate);
	values[Anum_pg_index_indisclustered - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indisvalid - 1] = BoolGetDatum(isvalid);
	values[Anum_pg_index_indcheckxmin - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indisready - 1] = BoolGetDatum(isready);
	values[Anum_pg_index_indislive - 1] = BoolGetDatum(true);
	values[Anum_pg_index_indisreplident - 1] = BoolGetDatum(false);
	values[Anum_pg_index_indkey - 1] = PointerGetDatum(indkey);
	values[Anum_pg_index_indcollation - 1] = PointerGetDatum(indcollation);
	values[Anum_pg_index_indclass - 1] = PointerGetDatum(indclass);
	values[Anum_pg_index_indoption - 1] = PointerGetDatum(indoption);
	values[Anum_pg_index_indexprs - 1] = exprsDatum;
	if (exprsDatum == (Datum) 0)
		nulls[Anum_pg_index_indexprs - 1] = true;
	values[Anum_pg_index_indpred - 1] = predDatum;
	if (predDatum == (Datum) 0)
		nulls[Anum_pg_index_indpred - 1] = true;

	tuple = heap_form_tuple(RelationGetDescr(pg_index), values, nulls);

	/*
	 * 把该元组插入 pg_index 目录。
	 */
	CatalogTupleInsert(pg_index, tuple);

	/*
	 * 关闭该关系并释放该元组。
	 */
	table_close(pg_index, RowExclusiveLock);
	heap_freetuple(tuple);
}


/*
 * index_create
 *
 * heapRelation: 要在其上建立索引的表（调用方已持有适当的锁）
 * indexRelationName: 顾名思义
 * indexRelationId: 通常传入 InvalidOid，让本例程
 *		为索引生成一个 OID。在引导期间，可以传入非零值
 *		以指定一个预先选定的 OID。
 * parentIndexRelid: 若创建索引分区，则为父索引的
 *		OID；否则为 InvalidOid。
 * parentConstraintId: 若在分区上创建约束，则为父表中
 *		该约束的 OID；否则为 InvalidOid。
 * relFileNumber: 通常传入 InvalidRelFileNumber 以获得新的存储。
 *		也可以传入非零值以附加一个已有的有效构建。
 * indexInfo: 执行器用于向索引插入的相同信息
 * indexColNames: 索引要使用的列名（char * 的 List）
 * accessMethodObjectId: 要使用的索引 AM 的 OID
 * tableSpaceId: 要使用的表空间的 OID
 * collationObjectId: 排序规则 OID 数组，每个索引列一个
 * classObjectId: 索引操作符类 OID 数组，每个索引列一个
 * coloptions: 每个索引列的 indoption 设置数组
 * reloptions: AM 特定的选项
 * flags: 位掩码，可以包含以下各位的任意组合：
 *		INDEX_CREATE_IS_PRIMARY
 *			该索引是一个主键
 *		INDEX_CREATE_ADD_CONSTRAINT:
 *			同时调用 index_constraint_create
 *		INDEX_CREATE_SKIP_BUILD:
 *			暂时跳过 index_build() 步骤；调用方稍后必须执行它
 *			（通常通过 reindex_index()）
 *		INDEX_CREATE_CONCURRENT:
 *			不对表加防止写入者的锁。该索引将被
 *			标记为 "无效"，调用方必须采取额外的步骤
 *			来修正它。
 *		INDEX_CREATE_IF_NOT_EXISTS:
 *			如果已存在同名的关系，
 *			则不抛出错误。
 *		INDEX_CREATE_PARTITIONED:
 *			创建分区索引（表必须已分区）
 * constr_flags: 传给 index_constraint_create 的标志
 *		（仅在设置了 INDEX_CREATE_ADD_CONSTRAINT 时）
 * allow_system_table_mods: 允许该表成为系统目录
 * is_internal: 若为 true，则为新索引调用创建后钩子
 * constraintId: 若不为 NULL，则接收所创建约束的 OID
 *
 * 返回所创建索引的 OID。
 */
Oid
index_create(Relation heapRelation,
			 const char *indexRelationName,
			 Oid indexRelationId,
			 Oid parentIndexRelid,
			 Oid parentConstraintId,
			 RelFileNumber relFileNumber,
			 IndexInfo *indexInfo,
			 List *indexColNames,
			 Oid accessMethodObjectId,
			 Oid tableSpaceId,
			 Oid *collationObjectId,
			 Oid *classObjectId,
			 int16 *coloptions,
			 Datum reloptions,
			 bits16 flags,
			 bits16 constr_flags,
			 bool allow_system_table_mods,
			 bool is_internal,
			 Oid *constraintId)
{
	Oid			heapRelationId = RelationGetRelid(heapRelation);
	Relation	pg_class;
	Relation	indexRelation;
	TupleDesc	indexTupDesc;
	bool		shared_relation;
	bool		mapped_relation;
	bool		is_exclusion;
	Oid			namespaceId;
	int			i;
	char		relpersistence;
	bool		isprimary = (flags & INDEX_CREATE_IS_PRIMARY) != 0;
	bool		invalid = (flags & INDEX_CREATE_INVALID) != 0;
	bool		concurrent = (flags & INDEX_CREATE_CONCURRENT) != 0;
	bool		partitioned = (flags & INDEX_CREATE_PARTITIONED) != 0;
	char		relkind;
	TransactionId relfrozenxid;
	MultiXactId relminmxid;
	bool		create_storage = !RelFileNumberIsValid(relFileNumber);

	/* 约束标志只有在请求约束时才能设置 */
	Assert((constr_flags == 0) ||
		   ((flags & INDEX_CREATE_ADD_CONSTRAINT) != 0));
	/* 分区索引绝不能单独被"构建" */
	Assert(!partitioned || (flags & INDEX_CREATE_SKIP_BUILD));

	relkind = partitioned ? RELKIND_PARTITIONED_INDEX : RELKIND_INDEX;
	is_exclusion = (indexInfo->ii_ExclusionOps != NULL);

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	/*
	 * 该索引将与其父表处于同一命名空间中，并且当且仅当父表
	 * 跨数据库共享时它才跨数据库共享。同样，当且仅当父表
	 * 使用 relfilenumber 映射时它才使用该映射；
	 * 并且它继承父表的 relpersistence。
	 */
	namespaceId = RelationGetNamespace(heapRelation);
	shared_relation = heapRelation->rd_rel->relisshared;
	mapped_relation = RelationIsMapped(heapRelation);
	relpersistence = heapRelation->rd_rel->relpersistence;

	/*
	 * 检查参数。
	 */
	if (indexInfo->ii_NumIndexAttrs < 1)
		elog(ERROR, "must index at least one column");

	if (!allow_system_table_mods &&
		IsSystemRelation(heapRelation) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user-defined indexes on system catalog tables are not supported")));

	/*
	 * Btree 的 text_pattern_ops 使用 text_eq 作为相等运算符，只要
	 * 排序规则是确定性的就没有问题；此时 text_eq 归结为按位相等，
	 * 因此它与该操作符类中的其他运算符和函数在语义上兼容。
	 * 但在非确定性排序规则下，text_eq 可能产生
	 * 与索引实际行为（由操作符类的比较函数决定）
	 * 不兼容的结果。
	 * 我们通过拒绝创建同时使用该操作符类和非确定性
	 * 排序规则的索引来防止这类问题。
	 *
	 * 同样的情况适用于 varchar_pattern_ops 和 bpchar_pattern_ops。
	 * 如果我们发现更多这种情况，也许会决定创建一种真正的机制，
	 * 把操作符类标记为与非确定性不兼容；但目前这个小 hack
	 * 已经足够。
	 *
	 * 另一种解决方案是使用一个特殊运算符（而非 text_eq）
	 * 作为相等操作符类成员；但这并不可取，因为那会
	 * 阻止许多如今工作正常的查询使用索引。
	 */
	for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
	{
		Oid			collation = collationObjectId[i];
		Oid			opclass = classObjectId[i];

		if (collation)
		{
			if ((opclass == TEXT_BTREE_PATTERN_OPS_OID ||
				 opclass == VARCHAR_BTREE_PATTERN_OPS_OID ||
				 opclass == BPCHAR_BTREE_PATTERN_OPS_OID) &&
				!get_collation_isdeterministic(collation))
			{
				HeapTuple	classtup;

				classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
				if (!HeapTupleIsValid(classtup))
					elog(ERROR, "cache lookup failed for operator class %u", opclass);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("nondeterministic collations are not supported for operator class \"%s\"",
								NameStr(((Form_pg_opclass) GETSTRUCT(classtup))->opcname))));
				ReleaseSysCache(classtup);
			}
		}
	}

	/*
	 * 在系统目录上并发建立索引是不安全的，因为我们在目录中
	 * 往往在提交之前就释放了锁。
	 */
	if (concurrent &&
		IsCatalogRelation(heapRelation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent index creation on system catalog tables is not supported")));

	/*
	 * 目前不支持这种情况。用 CREATE INDEX 在语法上无法要求它，
	 * 但 REINDEX 可能会出现这种情况。
	 */
	if (concurrent && is_exclusion)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent index creation for exclusion constraints is not supported")));

	/*
	 * 我们不允许在 initdb 之后为共享关系创建索引（因为
	 * 没有办法在其他数据库的 pg_class 中建立相应条目）。
	 */
	if (shared_relation && !IsBootstrapProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("shared indexes cannot be created after initdb")));

	/*
	 * 共享关系还必须位于 pg_global 中（最后的检查）
	 */
	if (shared_relation && tableSpaceId != GLOBALTABLESPACE_OID)
		elog(ERROR, "shared relations must be placed in pg_global tablespace");

	/*
	 * 检查名称是否重复（既针对索引本身，
	 * 也针对其关联的约束（如果有））。
	 * 这类情况反正会在相关目录的唯一索引上失败，
	 * 但我们更愿意给出更友好的错误消息。
	 */
	if (get_relname_relid(indexRelationName, namespaceId))
	{
		if ((flags & INDEX_CREATE_IF_NOT_EXISTS) != 0)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists, skipping",
							indexRelationName)));
			table_close(pg_class, RowExclusiveLock);
			return InvalidOid;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						indexRelationName)));
	}

	if ((flags & INDEX_CREATE_ADD_CONSTRAINT) != 0 &&
		ConstraintNameIsUsed(CONSTRAINT_RELATION, heapRelationId,
							 indexRelationName))
	{
		/*
		 * INDEX_CREATE_IF_NOT_EXISTS 在此不适用，因为
		 * 冲突的约束并不是一个索引。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for relation \"%s\" already exists",
						indexRelationName, RelationGetRelationName(heapRelation))));
	}

	/*
	 * 为索引元组构造元组描述符。
	 */
	indexTupDesc = ConstructTupleDescriptor(heapRelation,
											indexInfo,
											indexColNames,
											accessMethodObjectId,
											collationObjectId,
											classObjectId);

	/*
	 * 为索引分配一个 OID，除非我们被告知要使用某个特定值。
	 *
	 * 该 OID 同时也将是 relfilenumber，因此要确保它既不与
	 * pg_class 中的 OID 冲突，也不与现有物理文件冲突。
	 */
	if (!OidIsValid(indexRelationId))
	{
		/* 对 pg_class.oid 和 relfilenumber 使用二进制升级覆盖值 */
		if (IsBinaryUpgrade)
		{
			if (!OidIsValid(binary_upgrade_next_index_pg_class_oid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("pg_class index OID value not set when in binary upgrade mode")));

			indexRelationId = binary_upgrade_next_index_pg_class_oid;
			binary_upgrade_next_index_pg_class_oid = InvalidOid;

			/* 覆盖索引的 relfilenumber */
			if ((relkind == RELKIND_INDEX) &&
				(!RelFileNumberIsValid(binary_upgrade_next_index_pg_class_relfilenumber)))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("index relfilenumber value not set when in binary upgrade mode")));
			relFileNumber = binary_upgrade_next_index_pg_class_relfilenumber;
			binary_upgrade_next_index_pg_class_relfilenumber = InvalidRelFileNumber;

			/*
			 * 注意，对于二进制升级我们希望 create_storage = true。
			 * 我们在此创建的存储稍后会被替换，
			 * 但在此期间我们需要磁盘上有东西存在。
			 */
			Assert(create_storage);
		}
		else
		{
			indexRelationId =
				GetNewRelFileNumber(tableSpaceId, pg_class, relpersistence);
		}
	}

	/*
	 * 创建索引关系的关系缓存条目，必要时还包括物理磁盘文件。
	 * （如果我们后面失败了，若有磁盘文件，
	 * 则由 smgr 负责再次删除它。）
	 */
	indexRelation = heap_create(indexRelationName,
								namespaceId,
								tableSpaceId,
								indexRelationId,
								relFileNumber,
								accessMethodObjectId,
								indexTupDesc,
								relkind,
								relpersistence,
								shared_relation,
								mapped_relation,
								allow_system_table_mods,
								&relfrozenxid,
								&relminmxid,
								create_storage);

	Assert(relfrozenxid == InvalidTransactionId);
	Assert(relminmxid == InvalidMultiXactId);
	Assert(indexRelationId == RelationGetRelid(indexRelation));

	/*
	 * 获取它的排他锁。尽管在我们提交之前其他事务看不到它，
	 * 但这可以防止诸如 CLUSTER 之类的场景中锁管理器
	 * 报出可能死锁的抱怨。
	 */
	LockRelation(indexRelation, AccessExclusiveLock);

	/*
	 * 填写索引的 pg_class 条目中未被 heap_create
	 * 正确设置的字段。
	 *
	 * XXX 应该有更干净的办法来创建已编目的索引
	 */
	indexRelation->rd_rel->relowner = heapRelation->rd_rel->relowner;
	indexRelation->rd_rel->relam = accessMethodObjectId;
	indexRelation->rd_rel->relispartition = OidIsValid(parentIndexRelid);

	/*
	 * 存储索引的 pg_class 条目。
	 */
	InsertPgClassTuple(pg_class, indexRelation,
					   RelationGetRelid(indexRelation),
					   (Datum) 0,
					   reloptions);

	/* pg_class 处理完毕 */
	table_close(pg_class, RowExclusiveLock);

	/*
	 * 现在更新索引关系的元组描述符中所有属性元组形式的
	 * 对象标识。
	 */
	InitializeAttributeOids(indexRelation,
							indexInfo->ii_NumIndexAttrs,
							indexRelationId);

	/*
	 * 为该索引追加 ATTRIBUTE 元组。
	 */
	AppendAttributeTuples(indexRelation, indexInfo->ii_OpclassOptions);

	/* ----------------
	 *	  更新 pg_index
	 *	  （追加 INDEX 元组）
	 *
	 *	  注意，这会把 "predicate" 的一种表示形式存起来。
	 *	  （或者，也可以定义一条规则来维护该谓词）--Nels, Feb '92
	 * ----------------
	 */
	UpdateIndexRelation(indexRelationId, heapRelationId, parentIndexRelid,
						indexInfo,
						collationObjectId, classObjectId, coloptions,
						isprimary, is_exclusion,
						(constr_flags & INDEX_CONSTR_CREATE_DEFERRABLE) == 0,
						!concurrent && !invalid,
						!concurrent);

	/*
	 * 在索引的堆关系上注册关系缓存失效，
	 * 以维护其索引列表的一致性。
	 */
	CacheInvalidateRelcache(heapRelation);

	/* 必要时更新 pg_inherits 和父表的 relhassubclass */
	if (OidIsValid(parentIndexRelid))
	{
		StoreSingleInheritance(indexRelationId, parentIndexRelid, 1);
		LockRelationOid(parentIndexRelid, ShareUpdateExclusiveLock);
		SetRelationHasSubclass(parentIndexRelid, true);
	}

	/*
	 * 为该索引注册约束和依赖。
	 *
	 * 如果索引来自 CONSTRAINT 子句，则构造一个 pg_constraint 条目。
	 * 该索引将链接到该约束，而该约束又链接到该表。
	 * 如果不是 CONSTRAINT，我们就需要在表上直接
	 * 建立一个依赖。
	 *
	 * 我们不需要对命名空间建立依赖，因为通过我们的父表
	 * 会存在一个间接依赖。
	 *
	 * 在引导期间我们无法注册任何依赖，
	 * 并且我们也不尝试创建约束。
	 */
	if (!IsBootstrapProcessingMode())
	{
		ObjectAddress myself,
					referenced;
		ObjectAddresses *addrs;

		ObjectAddressSet(myself, RelationRelationId, indexRelationId);

		if ((flags & INDEX_CREATE_ADD_CONSTRAINT) != 0)
		{
			char		constraintType;
			ObjectAddress localaddr;

			if (isprimary)
				constraintType = CONSTRAINT_PRIMARY;
			else if (indexInfo->ii_Unique)
				constraintType = CONSTRAINT_UNIQUE;
			else if (is_exclusion)
				constraintType = CONSTRAINT_EXCLUSION;
			else
			{
				elog(ERROR, "constraint must be PRIMARY, UNIQUE or EXCLUDE");
				constraintType = 0; /* 让编译器保持安静 */
			}

			localaddr = index_constraint_create(heapRelation,
												indexRelationId,
												parentConstraintId,
												indexInfo,
												indexRelationName,
												constraintType,
												constr_flags,
												allow_system_table_mods,
												is_internal);
			if (constraintId)
				*constraintId = localaddr.objectId;
		}
		else
		{
			bool		have_simple_col = false;

			addrs = new_object_addresses();

			/* 为被直接引用的列创建自动依赖 */
			for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
			{
				if (indexInfo->ii_IndexAttrNumbers[i] != 0)
				{
					ObjectAddressSubSet(referenced, RelationRelationId,
										heapRelationId,
										indexInfo->ii_IndexAttrNumbers[i]);
					add_exact_object_address(&referenced, addrs);
					have_simple_col = true;
				}
			}

			/*
			 * 如果没有被直接引用的列，则让索引对整个表
			 * 建立一个自动依赖。在大多数情况下这将是多余的，
			 * 但如果索引表达式和谓词中不包含 Var 或只包含整行 Var，
			 * 情况可能就不是这样。
			 */
			if (!have_simple_col)
			{
				ObjectAddressSet(referenced, RelationRelationId,
								 heapRelationId);
				add_exact_object_address(&referenced, addrs);
			}

			record_object_address_dependencies(&myself, addrs, DEPENDENCY_AUTO);
			free_object_addresses(addrs);
		}

		/*
		 * 如果这是一个索引分区，则对父索引和表都创建分区依赖。
		 * （注意：这些依赖必须是*加上*而不是取代
		 * 所有其他依赖。否则在 DETACH PARTITION 之后
		 * 我们会缺少一些依赖。）
		 */
		if (OidIsValid(parentIndexRelid))
		{
			ObjectAddressSet(referenced, RelationRelationId, parentIndexRelid);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_PRI);

			ObjectAddressSet(referenced, RelationRelationId, heapRelationId);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_SEC);
		}

		/* 普通依赖的占位符 */
		addrs = new_object_addresses();

		/* 存储对排序规则的依赖 */

		/* 默认排序规则是被固定的，因此不必费心记录它 */
		for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
		{
			if (OidIsValid(collationObjectId[i]) &&
				collationObjectId[i] != DEFAULT_COLLATION_OID)
			{
				ObjectAddressSet(referenced, CollationRelationId,
								 collationObjectId[i]);
				add_exact_object_address(&referenced, addrs);
			}
		}

		/* 存储对操作符类的依赖 */
		for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
		{
			ObjectAddressSet(referenced, OperatorClassRelationId, classObjectId[i]);
			add_exact_object_address(&referenced, addrs);
		}

		record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
		free_object_addresses(addrs);

		/* 存储对索引表达式中提到的任何内容的依赖 */
		if (indexInfo->ii_Expressions)
		{
			recordDependencyOnSingleRelExpr(&myself,
											(Node *) indexInfo->ii_Expressions,
											heapRelationId,
											DEPENDENCY_NORMAL,
											DEPENDENCY_AUTO, false);
		}

		/* 存储对谓词中提到的任何内容的依赖 */
		if (indexInfo->ii_Predicate)
		{
			recordDependencyOnSingleRelExpr(&myself,
											(Node *) indexInfo->ii_Predicate,
											heapRelationId,
											DEPENDENCY_NORMAL,
											DEPENDENCY_AUTO, false);
		}
	}
	else
	{
		/* 引导模式 --- 断言我们没有被要求提供约束支持 */
		Assert((flags & INDEX_CREATE_ADD_CONSTRAINT) == 0);
	}

	/* 针对新索引的创建后钩子 */
	InvokeObjectPostCreateHookArg(RelationRelationId,
								  indexRelationId, 0, is_internal);

	/*
	 * 推进命令计数器，以便我们能够看到该索引新录入的
	 * 目录元组。
	 */
	CommandCounterIncrement();

	/*
	 * 在引导模式下，我们必须用来自目录的信息
	 * 填写索引策略结构。如果我们不在引导过程中，
	 * 那么由于 CommandCounterIncrement 期间的 sinval 更新，
	 * 关系缓存条目已经重建。
	 */
	if (IsBootstrapProcessingMode())
		RelationInitIndexAccessInfo(indexRelation);
	else
		Assert(indexRelation->rd_indexcxt != NULL);

	indexRelation->rd_index->indnkeyatts = indexInfo->ii_NumIndexKeyAttrs;

	/* 验证操作符类特定的选项 */
	if (indexInfo->ii_OpclassOptions)
		for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
			(void) index_opclass_options(indexRelation, i + 1,
										 indexInfo->ii_OpclassOptions[i],
										 true);

	/*
	 * 如果这是引导（initdb）期间，那么我们实际上还不填写该索引。
	 * 稍后我们还会创建更多的索引和类型，
	 * 所以我们把填写它们的工作推迟到引导即将结束之前。
	 * 同样，如果调用方指定了跳过构建，
	 * 那么填写索引的工作也推迟到以后（ALTER TABLE 在某些情况下
	 * 可以借此节省工作量）。否则，我们就调用 AM
	 * 构造该索引的例程。
	 */
	if (IsBootstrapProcessingMode())
	{
		index_register(heapRelationId, indexRelationId, indexInfo);
	}
	else if ((flags & INDEX_CREATE_SKIP_BUILD) != 0)
	{
		/*
		 * 调用方负责稍后填写该索引。
		 * 不过，我们最好确保堆关系
		 * 被正确地标记为具有索引。
		 */
		index_update_stats(heapRelation,
						   true,
						   -1.0);
		/* 让上面的更新可见 */
		CommandCounterIncrement();
	}
	else
	{
		index_build(heapRelation, indexRelation, indexInfo, false, true);
	}

	/*
	 * 关闭索引；但我们获取的锁要保持到事务结束。
	 * 关闭堆是调用方的责任。
	 */
	index_close(indexRelation, NoLock);

	return indexRelationId;
}

/*
 * index_concurrently_create_copy
 *
 * 根据调用方提供的索引定义，并发地创建一个基于它的索引。
 * 该索引被插入目录中，稍后还需要进行构建。本函数在并发
 * 重建索引处理期间被调用。
 *
 * "tablespaceOid" 是该索引要使用的表空间。
 */
Oid
index_concurrently_create_copy(Relation heapRelation, Oid oldIndexId,
							   Oid tablespaceOid, const char *newName)
{
	Relation	indexRelation;
	IndexInfo  *oldInfo,
			   *newInfo;
	Oid			newIndexId = InvalidOid;
	HeapTuple	indexTuple,
				classTuple;
	Datum		indclassDatum,
				colOptionDatum,
				optionDatum;
	oidvector  *indclass;
	int2vector *indcoloptions;
	bool		isnull;
	List	   *indexColNames = NIL;
	List	   *indexExprs = NIL;
	List	   *indexPreds = NIL;

	indexRelation = index_open(oldIndexId, RowExclusiveLock);

	/* 新索引需要来自旧索引的一些信息 */
	oldInfo = BuildIndexInfo(indexRelation);

	/*
	 * 不支持对带有排他约束的索引
	 * 进行并发构建。
	 */
	if (oldInfo->ii_ExclusionOps != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("concurrent index creation for exclusion constraints is not supported")));

	/* 从索引信息中获取类和列选项 ID 的数组 */
	indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(oldIndexId));
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "cache lookup failed for index %u", oldIndexId);
	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
										   Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	colOptionDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
											Anum_pg_index_indoption);
	indcoloptions = (int2vector *) DatumGetPointer(colOptionDatum);

	/* 获取索引的选项（如果有） */
	classTuple = SearchSysCache1(RELOID, oldIndexId);
	if (!HeapTupleIsValid(classTuple))
		elog(ERROR, "cache lookup failed for relation %u", oldIndexId);
	optionDatum = SysCacheGetAttr(RELOID, classTuple,
								  Anum_pg_class_reloptions, &isnull);

	/*
	 * 直接从目录获取表达式和谓词的列表。
	 * 这不能依赖旧索引的 IndexInfo 中的信息，
	 * 因为它们已经为规划器被展平。
	 */
	if (oldInfo->ii_Expressions != NIL)
	{
		Datum		exprDatum;
		char	   *exprString;

		exprDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
										   Anum_pg_index_indexprs);
		exprString = TextDatumGetCString(exprDatum);
		indexExprs = (List *) stringToNode(exprString);
		pfree(exprString);
	}
	if (oldInfo->ii_Predicate != NIL)
	{
		Datum		predDatum;
		char	   *predString;

		predDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
										   Anum_pg_index_indpred);
		predString = TextDatumGetCString(predDatum);
		indexPreds = (List *) stringToNode(predString);

		/* 同样转换为隐式 AND 格式 */
		indexPreds = make_ands_implicit((Expr *) indexPreds);
		pfree(predString);
	}

	/*
	 * 为新索引构建索引信息。注意，
	 * 带有排他约束的索引不支持重建，
	 * 因此无需填写所有 ii_Exclusion* 字段。
	 */
	newInfo = makeIndexInfo(oldInfo->ii_NumIndexAttrs,
							oldInfo->ii_NumIndexKeyAttrs,
							oldInfo->ii_Am,
							indexExprs,
							indexPreds,
							oldInfo->ii_Unique,
							oldInfo->ii_NullsNotDistinct,
							false,	/* 尚未准备好接受插入 */
							true,
							indexRelation->rd_indam->amsummarizing);

	/*
	 * 从新索引信息中提取列名列表和列号。
	 * 所有这些信息都将用于
	 * 索引的创建。
	 */
	for (int i = 0; i < oldInfo->ii_NumIndexAttrs; i++)
	{
		TupleDesc	indexTupDesc = RelationGetDescr(indexRelation);
		Form_pg_attribute att = TupleDescAttr(indexTupDesc, i);

		indexColNames = lappend(indexColNames, NameStr(att->attname));
		newInfo->ii_IndexAttrNumbers[i] = oldInfo->ii_IndexAttrNumbers[i];
	}

	/* 提取每个属性的操作符类参数（如果有） */
	if (oldInfo->ii_OpclassOptions != NULL)
	{
		newInfo->ii_OpclassOptions = palloc0(sizeof(Datum) *
											 newInfo->ii_NumIndexAttrs);
		for (int i = 0; i < newInfo->ii_NumIndexAttrs; i++)
			newInfo->ii_OpclassOptions[i] = get_attoptions(oldIndexId, i + 1);
	}

	/*
	 * 现在创建新索引。
	 *
	 * 对于分区索引，我们稍后调整分区依赖，以确保任何时候
	 * 状态都是一致的。这就是这里不设置 parentIndexRelid
	 * 的原因。
	 */
	newIndexId = index_create(heapRelation,
							  newName,
							  InvalidOid,	/* indexRelationId */
							  InvalidOid,	/* parentIndexRelid */
							  InvalidOid,	/* parentConstraintId */
							  InvalidRelFileNumber, /* relFileNumber */
							  newInfo,
							  indexColNames,
							  indexRelation->rd_rel->relam,
							  tablespaceOid,
							  indexRelation->rd_indcollation,
							  indclass->values,
							  indcoloptions->values,
							  optionDatum,
							  INDEX_CREATE_SKIP_BUILD | INDEX_CREATE_CONCURRENT,
							  0,
							  true, /* 允许该表成为系统目录？ */
							  false,	/* is_internal（是否为内部？） */
							  NULL);

	/* 关闭使用过的关系并进行清理 */
	index_close(indexRelation, NoLock);
	ReleaseSysCache(indexTuple);
	ReleaseSysCache(classTuple);

	return newIndexId;
}

/*
 * index_concurrently_build
 *
 * 为并发操作构建索引。执行本操作时会获取低级锁，它们只用于
 * 防止模式变更，但需要保持到执行本操作的事务结束。
 * 'indexOid' 指的是先前处理中已作为其中一部分
 * 创建的索引关系 OID，
 * 'heapOid' 指的是其父堆关系。
 */
void
index_concurrently_build(Oid heapRelationId,
						 Oid indexRelationId)
{
	Relation	heapRel;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	Relation	indexRelation;
	IndexInfo  *indexInfo;

	/* 这最好能确保有一个活跃的快照 */
	Assert(ActiveSnapshotSet());

	/* 打开并锁定父堆关系 */
	heapRel = table_open(heapRelationId, ShareUpdateExclusiveLock);

	/*
	 * 切换到表所有者的 userid，使任何索引函数都以该用户身份运行。
	 * 同时限制安全敏感操作，并使 GUC 变量的更改
	 * 仅限于本命令。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	indexRelation = index_open(indexRelationId, RowExclusiveLock);

	/*
	 * 我们必须重新构建 IndexInfo 结构，
	 * 因为它在该并发索引于目录层创建的
	 * 事务提交时丢失了。
	 */
	indexInfo = BuildIndexInfo(indexRelation);
	Assert(!indexInfo->ii_ReadyForInserts);
	indexInfo->ii_Concurrent = true;
	indexInfo->ii_BrokenHotChain = false;

	/* 现在构建索引 */
	index_build(heapRel, indexRelation, indexInfo, false, true);

	/* 回滚索引函数执行的任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* 关闭这两个关系，但保留锁 */
	table_close(heapRel, NoLock);
	index_close(indexRelation, NoLock);

	/*
	 * 更新 pg_index 行，将索引标记为可以接受插入。一旦我们
	 * 提交该事务，任何打开该表的新事务在插入和非 HOT 更新时
	 * 都必须向该索引插入新条目。
	 */
	index_set_state_flags(indexRelationId, INDEX_CREATE_SET_READY);
}

/*
 * index_concurrently_swap
 *
 * 把旧索引的名称、依赖和约束交换到新索引上，同时把旧索引
 * 标记为无效、新索引标记为有效。
 */
void
index_concurrently_swap(Oid newIndexId, Oid oldIndexId, const char *oldName)
{
	Relation	pg_class,
				pg_index,
				pg_constraint,
				pg_trigger;
	Relation	oldClassRel,
				newClassRel;
	HeapTuple	oldClassTuple,
				newClassTuple;
	Form_pg_class oldClassForm,
				newClassForm;
	HeapTuple	oldIndexTuple,
				newIndexTuple;
	Form_pg_index oldIndexForm,
				newIndexForm;
	bool		isPartition;
	Oid			indexConstraintOid;
	List	   *constraintOids = NIL;
	ListCell   *lc;

	/*
	 * 在交换新旧索引之前，对它们获取必要的锁。
	 */
	oldClassRel = relation_open(oldIndexId, ShareUpdateExclusiveLock);
	newClassRel = relation_open(newIndexId, ShareUpdateExclusiveLock);

	/* 现在交换这些索引的名称和依赖 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	oldClassTuple = SearchSysCacheCopy1(RELOID,
										ObjectIdGetDatum(oldIndexId));
	if (!HeapTupleIsValid(oldClassTuple))
		elog(ERROR, "could not find tuple for relation %u", oldIndexId);
	newClassTuple = SearchSysCacheCopy1(RELOID,
										ObjectIdGetDatum(newIndexId));
	if (!HeapTupleIsValid(newClassTuple))
		elog(ERROR, "could not find tuple for relation %u", newIndexId);

	oldClassForm = (Form_pg_class) GETSTRUCT(oldClassTuple);
	newClassForm = (Form_pg_class) GETSTRUCT(newClassTuple);

	/* 交换名称 */
	namestrcpy(&newClassForm->relname, NameStr(oldClassForm->relname));
	namestrcpy(&oldClassForm->relname, oldName);

	/* 交换分区标志以正确跟踪继承关系 */
	isPartition = newClassForm->relispartition;
	newClassForm->relispartition = oldClassForm->relispartition;
	oldClassForm->relispartition = isPartition;

	CatalogTupleUpdate(pg_class, &oldClassTuple->t_self, oldClassTuple);
	CatalogTupleUpdate(pg_class, &newClassTuple->t_self, newClassTuple);

	heap_freetuple(oldClassTuple);
	heap_freetuple(newClassTuple);

	/* 现在交换索引信息 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	oldIndexTuple = SearchSysCacheCopy1(INDEXRELID,
										ObjectIdGetDatum(oldIndexId));
	if (!HeapTupleIsValid(oldIndexTuple))
		elog(ERROR, "could not find tuple for relation %u", oldIndexId);
	newIndexTuple = SearchSysCacheCopy1(INDEXRELID,
										ObjectIdGetDatum(newIndexId));
	if (!HeapTupleIsValid(newIndexTuple))
		elog(ERROR, "could not find tuple for relation %u", newIndexId);

	oldIndexForm = (Form_pg_index) GETSTRUCT(oldIndexTuple);
	newIndexForm = (Form_pg_index) GETSTRUCT(newIndexTuple);

	/*
	 * 从旧索引复制约束标志。这是安全的，因为旧索引
	 * 保证了唯一性。
	 */
	newIndexForm->indisprimary = oldIndexForm->indisprimary;
	oldIndexForm->indisprimary = false;
	newIndexForm->indisexclusion = oldIndexForm->indisexclusion;
	oldIndexForm->indisexclusion = false;
	newIndexForm->indimmediate = oldIndexForm->indimmediate;
	oldIndexForm->indimmediate = true;

	/* 在新索引中保留 indisreplident */
	newIndexForm->indisreplident = oldIndexForm->indisreplident;

	/* 在新索引中保留 indisclustered */
	newIndexForm->indisclustered = oldIndexForm->indisclustered;

	/*
	 * 把新索引标记为有效，并效仿 index_set_state_flags() 的做法
	 * 把旧索引标记为无效。
	 */
	newIndexForm->indisvalid = true;
	oldIndexForm->indisvalid = false;
	oldIndexForm->indisclustered = false;
	oldIndexForm->indisreplident = false;

	CatalogTupleUpdate(pg_index, &oldIndexTuple->t_self, oldIndexTuple);
	CatalogTupleUpdate(pg_index, &newIndexTuple->t_self, newIndexTuple);

	heap_freetuple(oldIndexTuple);
	heap_freetuple(newIndexTuple);

	/*
	 * 把约束和触发器转移到新索引上
	 */

	constraintOids = get_index_ref_constraints(oldIndexId);

	indexConstraintOid = get_index_constraint(oldIndexId);

	if (OidIsValid(indexConstraintOid))
		constraintOids = lappend_oid(constraintOids, indexConstraintOid);

	pg_constraint = table_open(ConstraintRelationId, RowExclusiveLock);
	pg_trigger = table_open(TriggerRelationId, RowExclusiveLock);

	foreach(lc, constraintOids)
	{
		HeapTuple	constraintTuple,
					triggerTuple;
		Form_pg_constraint conForm;
		ScanKeyData key[1];
		SysScanDesc scan;
		Oid			constraintOid = lfirst_oid(lc);

		/* 把约束从旧索引移到新索引 */
		constraintTuple = SearchSysCacheCopy1(CONSTROID,
											  ObjectIdGetDatum(constraintOid));
		if (!HeapTupleIsValid(constraintTuple))
			elog(ERROR, "could not find tuple for constraint %u", constraintOid);

		conForm = ((Form_pg_constraint) GETSTRUCT(constraintTuple));

		if (conForm->conindid == oldIndexId)
		{
			conForm->conindid = newIndexId;

			CatalogTupleUpdate(pg_constraint, &constraintTuple->t_self, constraintTuple);
		}

		heap_freetuple(constraintTuple);

		/* 查找触发器记录 */
		ScanKeyInit(&key[0],
					Anum_pg_trigger_tgconstraint,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(constraintOid));

		scan = systable_beginscan(pg_trigger, TriggerConstraintIndexId, true,
								  NULL, 1, key);

		while (HeapTupleIsValid((triggerTuple = systable_getnext(scan))))
		{
			Form_pg_trigger tgForm = (Form_pg_trigger) GETSTRUCT(triggerTuple);

			if (tgForm->tgconstrindid != oldIndexId)
				continue;

			/* 制作一份可修改的副本 */
			triggerTuple = heap_copytuple(triggerTuple);
			tgForm = (Form_pg_trigger) GETSTRUCT(triggerTuple);

			tgForm->tgconstrindid = newIndexId;

			CatalogTupleUpdate(pg_trigger, &triggerTuple->t_self, triggerTuple);

			heap_freetuple(triggerTuple);
		}

		systable_endscan(scan);
	}

	/*
	 * 移动注释（如果有）
	 */
	{
		Relation	description;
		ScanKeyData skey[3];
		SysScanDesc sd;
		HeapTuple	tuple;
		Datum		values[Natts_pg_description] = {0};
		bool		nulls[Natts_pg_description] = {0};
		bool		replaces[Natts_pg_description] = {0};

		values[Anum_pg_description_objoid - 1] = ObjectIdGetDatum(newIndexId);
		replaces[Anum_pg_description_objoid - 1] = true;

		ScanKeyInit(&skey[0],
					Anum_pg_description_objoid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(oldIndexId));
		ScanKeyInit(&skey[1],
					Anum_pg_description_classoid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationRelationId));
		ScanKeyInit(&skey[2],
					Anum_pg_description_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(0));

		description = table_open(DescriptionRelationId, RowExclusiveLock);

		sd = systable_beginscan(description, DescriptionObjIndexId, true,
								NULL, 3, skey);

		while ((tuple = systable_getnext(sd)) != NULL)
		{
			tuple = heap_modify_tuple(tuple, RelationGetDescr(description),
									  values, nulls, replaces);
			CatalogTupleUpdate(description, &tuple->t_self, tuple);

			break;				/* 假定至多只有一个匹配 */
		}

		systable_endscan(sd);
		table_close(description, NoLock);
	}

	/*
	 * 与父索引交换继承关系
	 */
	if (get_rel_relispartition(oldIndexId))
	{
		List	   *ancestors = get_partition_ancestors(oldIndexId);
		Oid			parentIndexRelid = linitial_oid(ancestors);

		DeleteInheritsTuple(oldIndexId, parentIndexRelid, false, NULL);
		StoreSingleInheritance(newIndexId, parentIndexRelid, 1);

		list_free(ancestors);
	}

	/*
	 * 把旧索引的所有依赖以及指向它的依赖交换到新索引上，反之亦然。
	 * 注意，调用 CommandCounterIncrement() 会导致 pg_depend 中出现
	 * 重复条目，因此不应这样做。
	 */
	changeDependenciesOf(RelationRelationId, newIndexId, oldIndexId);
	changeDependenciesOn(RelationRelationId, newIndexId, oldIndexId);

	changeDependenciesOf(RelationRelationId, oldIndexId, newIndexId);
	changeDependenciesOn(RelationRelationId, oldIndexId, newIndexId);

	/* 把统计信息从旧索引复制到新索引 */
	pgstat_copy_relation_stats(newClassRel, oldClassRel);

	/* 把 pg_statistic 的数据从旧索引复制到新索引 */
	CopyStatistics(oldIndexId, newIndexId);

	/* 为每个索引属性复制 pg_attribute.attstattarget */
	{
		HeapTuple	attrTuple;
		Relation	pg_attribute;
		SysScanDesc scan;
		ScanKeyData key[1];

		pg_attribute = table_open(AttributeRelationId, RowExclusiveLock);
		ScanKeyInit(&key[0],
					Anum_pg_attribute_attrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(newIndexId));
		scan = systable_beginscan(pg_attribute, AttributeRelidNumIndexId,
								  true, NULL, 1, key);

		while (HeapTupleIsValid((attrTuple = systable_getnext(scan))))
		{
			Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attrTuple);
			Datum		repl_val[Natts_pg_attribute];
			bool		repl_null[Natts_pg_attribute];
			bool		repl_repl[Natts_pg_attribute];
			int			attstattarget;
			HeapTuple	newTuple;

			/* 忽略已删除的列 */
			if (att->attisdropped)
				continue;

			/*
			 * 从旧索引获取 attstattarget 并刷新新值。
			 */
			attstattarget = get_attstattarget(oldIndexId, att->attnum);

			/* 如果两者相同则无需刷新 */
			if (attstattarget == att->attstattarget)
				continue;

			memset(repl_val, 0, sizeof(repl_val));
			memset(repl_null, false, sizeof(repl_null));
			memset(repl_repl, false, sizeof(repl_repl));

			repl_repl[Anum_pg_attribute_attstattarget - 1] = true;
			repl_val[Anum_pg_attribute_attstattarget - 1] = Int16GetDatum(attstattarget);

			newTuple = heap_modify_tuple(attrTuple,
										 RelationGetDescr(pg_attribute),
										 repl_val, repl_null, repl_repl);
			CatalogTupleUpdate(pg_attribute, &newTuple->t_self, newTuple);

			heap_freetuple(newTuple);
		}

		systable_endscan(scan);
		table_close(pg_attribute, RowExclusiveLock);
	}

	/* 关闭关系 */
	table_close(pg_class, RowExclusiveLock);
	table_close(pg_index, RowExclusiveLock);
	table_close(pg_constraint, RowExclusiveLock);
	table_close(pg_trigger, RowExclusiveLock);

	/* 先前获取的锁要保持到事务结束 */
	relation_close(oldClassRel, NoLock);
	relation_close(newClassRel, NoLock);
}

/*
 * index_concurrently_set_dead
 *
 * 在实际删除索引之前，执行 DROP INDEX CONCURRENTLY 或
 * REINDEX CONCURRENTLY 的最后失效阶段。调用本函数之后，
 * 所有后端都会把该索引视为已死。此处获取的低级锁将保持到
 * 调用本函数的事务结束。
 */
void
index_concurrently_set_dead(Oid heapId, Oid indexId)
{
	Relation	userHeapRelation;
	Relation	userIndexRelation;

	/*
	 * 不会再有事务在该索引上获取谓词锁，
	 * 而且我们即将停止向该索引插入，
	 * 这可能显示出与现有谓词锁的冲突，
	 * 所以现在是把它们转移到堆关系上的时机。
	 */
	userHeapRelation = table_open(heapId, ShareUpdateExclusiveLock);
	userIndexRelation = index_open(indexId, ShareUpdateExclusiveLock);
	TransferPredicateLocksToHeapRelation(userIndexRelation);

	/*
	 * 现在我们确信没有人会使用该索引进行查询；他们最多只是
	 * 为了更新它而把它打开。所以现在我们可以取消 indisready 和
	 * indislive，然后等待直到再也没有任何人能够使用它。
	 */
	index_set_state_flags(indexId, INDEX_DROP_SET_DEAD);

	/*
	 * 使该表的关系缓存失效，以便本次提交之后所有会话
	 * 都会刷新该表的索引列表。仅忘记索引的
	 * 关系缓存条目是不够的。
	 */
	CacheInvalidateRelcache(userHeapRelation);

	/*
	 * 再次关闭关系，但仍持有会话锁。
	 */
	table_close(userHeapRelation, NoLock);
	index_close(userIndexRelation, NoLock);
}

/*
 * index_constraint_create
 *
 * 建立与索引相关联的约束。返回新约束的
 * 地址。
 *
 * heapRelation: 拥有该索引的表（调用方必须已持有适当的锁）
 * indexRelationId: 索引的 OID
 * parentConstraintId: 若约束在分区上，则为父表中
 *		该约束的 OID。
 * indexInfo: 执行器用于向索引插入的相同信息
 * constraintName: 顾名思义（一般应与索引的名称一致）
 * constraintType: CONSTRAINT_PRIMARY、CONSTRAINT_UNIQUE 或
 *		CONSTRAINT_EXCLUSION 之一
 * flags: 位掩码，可以包含以下各位的任意组合：
 *		INDEX_CONSTR_CREATE_MARK_AS_PRIMARY: 索引是 PRIMARY KEY
 *		INDEX_CONSTR_CREATE_DEFERRABLE: 约束是 DEFERRABLE
 *		INDEX_CONSTR_CREATE_INIT_DEFERRED: 约束是 INITIALLY DEFERRED
 *		INDEX_CONSTR_CREATE_UPDATE_INDEX: 更新 pg_index 行
 *		INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS: 移除索引对表的列
 *			现有的依赖
 * allow_system_table_mods: 允许该表成为系统目录
 * is_internal: 该索引是因内部过程而构造的
 */
ObjectAddress
index_constraint_create(Relation heapRelation,
						Oid indexRelationId,
						Oid parentConstraintId,
						IndexInfo *indexInfo,
						const char *constraintName,
						char constraintType,
						bits16 constr_flags,
						bool allow_system_table_mods,
						bool is_internal)
{
	Oid			namespaceId = RelationGetNamespace(heapRelation);
	ObjectAddress myself,
				idxaddr;
	Oid			conOid;
	bool		deferrable;
	bool		initdeferred;
	bool		mark_as_primary;
	bool		islocal;
	bool		noinherit;
	int			inhcount;

	deferrable = (constr_flags & INDEX_CONSTR_CREATE_DEFERRABLE) != 0;
	initdeferred = (constr_flags & INDEX_CONSTR_CREATE_INIT_DEFERRED) != 0;
	mark_as_primary = (constr_flags & INDEX_CONSTR_CREATE_MARK_AS_PRIMARY) != 0;

	/* 引导期间约束创建支持不可用 */
	Assert(!IsBootstrapProcessingMode());

	/* 强制执行系统表限制 */
	if (!allow_system_table_mods &&
		IsSystemRelation(heapRelation) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user-defined indexes on system catalog tables are not supported")));

	/* 主键/唯一约束不应含有任何表达式 */
	if (indexInfo->ii_Expressions &&
		constraintType != CONSTRAINT_EXCLUSION)
		elog(ERROR, "constraints cannot have index expressions");

	/*
	 * 如果我们是为一个已存在的索引制造约束，我们需要清除该索引
	 * 现有的自动依赖（即那些 index_create() 在不调用本函数时
	 * 本应建立的依赖）。
	 *
	 * 注意：如果索引含有任何表达式或谓词，
	 * 这段代码未必会做正确的事，但我们绝不会把这样的索引
	 * 变成 UNIQUE 或 PRIMARY KEY 约束。
	 */
	if (constr_flags & INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS)
		deleteDependencyRecordsForClass(RelationRelationId, indexRelationId,
										RelationRelationId, DEPENDENCY_AUTO);

	if (OidIsValid(parentConstraintId))
	{
		islocal = false;
		inhcount = 1;
		noinherit = false;
	}
	else
	{
		islocal = true;
		inhcount = 0;
		noinherit = true;
	}

	/*
	 * 构造一个 pg_constraint 条目。
	 */
	conOid = CreateConstraintEntry(constraintName,
								   namespaceId,
								   constraintType,
								   deferrable,
								   initdeferred,
								   true,
								   parentConstraintId,
								   RelationGetRelid(heapRelation),
								   indexInfo->ii_IndexAttrNumbers,
								   indexInfo->ii_NumIndexKeyAttrs,
								   indexInfo->ii_NumIndexAttrs,
								   InvalidOid,	/* 没有域 */
								   indexRelationId, /* 索引的 OID */
								   InvalidOid,	/* 没有外键 */
								   NULL,
								   NULL,
								   NULL,
								   NULL,
								   0,
								   ' ',
								   ' ',
								   NULL,
								   0,
								   ' ',
								   indexInfo->ii_ExclusionOps,
								   NULL,	/* 没有检查约束 */
								   NULL,
								   islocal,
								   inhcount,
								   noinherit,
								   is_internal);

	/*
	 * 把该索引注册为在内部依赖于该约束。
	 *
	 * 注意，该约束对表有依赖，因此我们不需要（也不希望）
	 * 索引到表之间存在任何直接依赖。
	 */
	ObjectAddressSet(myself, ConstraintRelationId, conOid);
	ObjectAddressSet(idxaddr, RelationRelationId, indexRelationId);
	recordDependencyOn(&idxaddr, &myself, DEPENDENCY_INTERNAL);

	/*
	 * 此外，如果这是分区上的约束，还要让它对父约束和表
	 * 都建立分区类型的依赖。
	 */
	if (OidIsValid(parentConstraintId))
	{
		ObjectAddress referenced;

		ObjectAddressSet(referenced, ConstraintRelationId, parentConstraintId);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_PRI);
		ObjectAddressSet(referenced, RelationRelationId,
						 RelationGetRelid(heapRelation));
		recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_SEC);
	}

	/*
	 * 如果该约束是可延迟的，则创建延迟唯一性检查触发器。
	 * （CreateTrigger 会给该触发器一个
	 * 对约束的内部依赖。）
	 */
	if (deferrable)
	{
		CreateTrigStmt *trigger = makeNode(CreateTrigStmt);

		trigger->replace = false;
		trigger->isconstraint = true;
		trigger->trigname = (constraintType == CONSTRAINT_PRIMARY) ?
			"PK_ConstraintTrigger" :
			"Unique_ConstraintTrigger";
		trigger->relation = NULL;
		trigger->funcname = SystemFuncName("unique_key_recheck");
		trigger->args = NIL;
		trigger->row = true;
		trigger->timing = TRIGGER_TYPE_AFTER;
		trigger->events = TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE;
		trigger->columns = NIL;
		trigger->whenClause = NULL;
		trigger->transitionRels = NIL;
		trigger->deferrable = true;
		trigger->initdeferred = initdeferred;
		trigger->constrrel = NULL;

		(void) CreateTrigger(trigger, NULL, RelationGetRelid(heapRelation),
							 InvalidOid, conOid, indexRelationId, InvalidOid,
							 InvalidOid, NULL, true, false);
	}

	/*
	 * 如果需要，在 pg_index 中把该索引标记为主键
	 * 和/或延迟约束。
	 *
	 * 注意：把已有索引变成约束时，调用方必须持有可防止
	 * 并发表更新的表锁；否则，表的并发读者有可能
	 * 完全看不到该索引。
	 */
	if ((constr_flags & INDEX_CONSTR_CREATE_UPDATE_INDEX) &&
		(mark_as_primary || deferrable))
	{
		Relation	pg_index;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;
		bool		dirty = false;
		bool		marked_as_primary = false;

		pg_index = table_open(IndexRelationId, RowExclusiveLock);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(indexRelationId));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexRelationId);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		if (mark_as_primary && !indexForm->indisprimary)
		{
			indexForm->indisprimary = true;
			dirty = true;
			marked_as_primary = true;
		}

		if (deferrable && indexForm->indimmediate)
		{
			indexForm->indimmediate = false;
			dirty = true;
		}

		if (dirty)
		{
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

			/*
			 * 当我们把已有索引标记为主键时，强制对其父表
			 * 进行关系缓存刷新，使所有会话都意识到
			 * 该表现在有了主键。这一点很重要，因为它会影响
			 * 某些复制行为。
			 */
			if (marked_as_primary)
				CacheInvalidateRelcache(heapRelation);

			InvokeObjectPostAlterHookArg(IndexRelationId, indexRelationId, 0,
										 InvalidOid, is_internal);
		}

		heap_freetuple(indexTuple);
		table_close(pg_index, RowExclusiveLock);
	}

	return myself;
}

/*
 *		index_drop
 *
 * 注意：现在本例程应当只能通过 performDeletion() 调用，
 * 否则关联的依赖不会被清理。
 *
 * 如果 concurrent 为 true，则执行 DROP INDEX CONCURRENTLY。
 * 如果 concurrent 为 false 但 concurrent_lock_mode 为 true，
 * 则执行普通的 DROP INDEX，但为 CONCURRENTLY 处理获取一个锁。
 * 这是 REINDEX CONCURRENTLY 的一部分。
 */
void
index_drop(Oid indexId, bool concurrent, bool concurrent_lock_mode)
{
	Oid			heapId;
	Relation	userHeapRelation;
	Relation	userIndexRelation;
	Relation	indexRelation;
	HeapTuple	tuple;
	bool		hasexprs;
	LockRelId	heaprelid,
				indexrelid;
	LOCKTAG		heaplocktag;
	LOCKMODE	lockmode;

	/*
	 * 临时关系使用非并发的 DROP。其他后端无法访问
	 * 临时关系，因此获取更强的锁并无坏处
	 * （见 RemoveRelations 中的注释），
	 * 而且非并发的 DROP 效率更高。
	 */
	Assert(get_rel_persistence(indexId) != RELPERSISTENCE_TEMP ||
		   (!concurrent && !concurrent_lock_mode));

	/*
	 * 要安全地删除索引，我们必须获取其父表的排他锁。
	 * 仅对索引加排他锁是不够的，因为另一个后端可能正要
	 * 在父表上执行查询。如果它依赖于先前缓存的索引 OID 列表，
	 * 它就可能试图访问刚被删除的索引。
	 * 因此我们必须获取一个足够强的表锁，
	 * 以阻止所有对该表的查询进行下去，
	 * 直到我们提交并发出共享缓存失效通知，
	 * 使它们更新各自的索引列表。
	 *
	 * 在并发情况下，我们通过分多步禁用索引的使用并等待
	 * 所有可能正在使用该索引的事务结束来避免这一要求，
	 * 因此我们不需要对父表加排他锁。相反，我们获取
	 * ShareUpdateExclusiveLock，以确保两个会话不会对同一个索引
	 * 同时执行 CREATE/DROP INDEX CONCURRENTLY。（一旦我们确定
	 * 没有其他人在使用该索引，
	 * 下面就会在该索引上获取 AccessExclusiveLock。）
	 */
	heapId = IndexGetRelation(indexId, false);
	lockmode = (concurrent || concurrent_lock_mode) ? ShareUpdateExclusiveLock : AccessExclusiveLock;
	userHeapRelation = table_open(heapId, lockmode);
	userIndexRelation = index_open(indexId, lockmode);

	/*
	 * 我们自己的会话中可能仍有使用它的未关闭查询，
	 * 上述加锁无法防止这一点，因此要显式检查。
	 */
	CheckTableNotInUse(userIndexRelation, "DROP INDEX");

	/*
	 * Drop Index Concurrently 大体上是 Create Index Concurrently 的
	 * 逆过程。
	 *
	 * 首先我们取消 indisvalid，使之后开始的查询不再使用该索引
	 * 来回答查询。我们必须保持 indisready = true，
	 * 以便仍在扫描该索引的事务能够继续看到有效的索引内容。
	 * 例如，如果它们使用的是 READ COMMITTED 模式，
	 * 而另一个事务做了更改并提交，
	 * 它们就需要在索引中看到那些新元组。
	 *
	 * 在所有可能已经使用该索引进行查询的事务都结束之后，
	 * 我们可以取消 indisready 和 indislive，
	 * 然后等待直到再没有任何人可能触及它。
	 * （注意：我们需要 indislive，因为该状态必须区别于
	 * CREATE INDEX CONCURRENTLY 期间的初始状态，
	 * 在那种状态下 indislive 为真，而 indisready 和 indisvalid 为假。
	 * 这是因为在那种状态下，事务必须检查该索引以做出 HOT 安全性决策；
	 * 而在本状态下，我们根本不希望它们打开它。）
	 *
	 * 由于该索引上所有的谓词锁都即将失效，我们必须
	 * 把它们提升为堆上的谓词锁。在非并发情况下，
	 * 我们现在就可以这样做。在并发情况下则要棘手一些。
	 * 谓词锁必须在该索引上没有正在进行的索引扫描、
	 * 且随后也不会再开始扫描时才能转移，
	 * 这样该索引上就不会产生新的谓词锁。此外，
	 * 它们必须在堆插入停止维护该索引之前转移，
	 * 否则在堆关系上的锁就位、能够根据堆元组插入
	 * 检测冲突之前，与索引间隙上的谓词锁
	 * 的冲突可能被漏掉。
	 */
	if (concurrent)
	{
		/*
		 * 我们必须提交事务，以使第一次 pg_index 状态更新
		 * 对其他会话可见。如果 DROP 机制已经执行了
		 * 任何其他操作（删除其他对象、pg_depend 条目等），
		 * 提交将使那些操作成为永久性的，
		 * 这将使我们在后续序列中途失败时
		 * 留下不一致的目录状态。
		 * 由于 DROP INDEX CONCURRENTLY 限制为只能删除一个
		 * 没有任何依赖的索引，我们应当在任何操作被执行之前
		 * 到达这里——但让我们检查一下以确认。我们可以通过检查
		 * 是否未分配任何 XID 来验证当前事务
		 * 尚未执行任何事务性更新。
		 */
		if (GetTopTransactionIdIfAny() != InvalidTransactionId)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DROP INDEX CONCURRENTLY must be first action in transaction")));

		/*
		 * 通过更新其 pg_index 条目把索引标记为无效
		 */
		index_set_state_flags(indexId, INDEX_DROP_CLEAR_VALID);

		/*
		 * 使该表的关系缓存失效，以便本次提交之后
		 * 所有会话都会刷新可能引用该索引的
		 * 所有缓存计划。
		 */
		CacheInvalidateRelcache(userHeapRelation);

		/* 保存 lockrelid 和 locktag 以供下面使用，然后关闭但保留锁 */
		heaprelid = userHeapRelation->rd_lockInfo.lockRelId;
		SET_LOCKTAG_RELATION(heaplocktag, heaprelid.dbId, heaprelid.relId);
		indexrelid = userIndexRelation->rd_lockInfo.lockRelId;

		table_close(userHeapRelation, NoLock);
		index_close(userIndexRelation, NoLock);

		/*
		 * 我们必须提交当前事务，以使 indisvalid 的更新
		 * 对其他事务可见；然后启动另一个事务。注意，
		 * 先前构建的数据结构在提交中丢失。
		 * 越过此处之后我们保留的唯一数据是关系 ID。
		 *
		 * 提交之前，对表获取一个会话级的锁，以确保在我们完成之前
		 * 该表和该索引都不会被删除。即使有人正在等待访问，
		 * 这也不会造成阻塞，因为我们在自己的事务中
		 * 已经持有相同的锁。
		 */
		LockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);
		LockRelationIdForSession(&indexrelid, ShareUpdateExclusiveLock);

		PopActiveSnapshot();
		CommitTransactionCommand();
		StartTransactionCommand();

		/*
		 * 现在我们必须等待，直到没有任何运行中的事务
		 * 可能正在使用该索引进行查询。
		 * 这里使用 AccessExclusiveLock 来检查
		 * 持有该表上任何种类锁的运行中事务。
		 * 注意我们无需担心在此之后为读取而打开该表的事务；
		 * 它们打开该关系时会看到该索引是无效的。
		 *
		 * 注意：我们在此使用实际的锁获取，而不是仅仅检查 ProcArray
		 * 然后睡眠，原因是：如果相关事务之一因试图获取
		 * 我们表上的排他锁而被阻塞，就可能出现死锁。
		 * 锁代码会检测死锁
		 * 并正确地报错。
		 *
		 * 注意：我们在此无条件地通过 WaitForLockers() 报告进度，
		 * 尽管它只会在我们被 REINDEX CONCURRENTLY 调用时才会被使用，
		 * 而被 DROP INDEX CONCURRENTLY 调用时则不会。
		 */
		WaitForLockers(heaplocktag, AccessExclusiveLock, true);

		/* 完成索引的失效并将其标记为已死 */
		index_concurrently_set_dead(heapId, indexId);

		/*
		 * 同样，提交事务以使 pg_index 的更新
		 * 对其他会话可见。
		 */
		CommitTransactionCommand();
		StartTransactionCommand();

		/*
		 * 等待所有看到旧索引状态的事务都结束。
		 * 关于进度报告见上文。
		 */
		WaitForLockers(heaplocktag, AccessExclusiveLock, true);

		/*
		 * 重新打开关系，以便我们完成操作。
		 *
		 * 此时不应有任何东西在访问该索引，
		 * 但我们不要心存侥幸，
		 * 在物理删除之前先在该索引上获取 AccessExclusiveLock。
		 */
		userHeapRelation = table_open(heapId, ShareUpdateExclusiveLock);
		userIndexRelation = index_open(indexId, AccessExclusiveLock);
	}
	else
	{
		/* 非并发，因此只需转移谓词锁即可 */
		TransferPredicateLocksToHeapRelation(userIndexRelation);
	}

	/*
	 * 安排物理删除这些文件（如果有）
	 */
	if (RELKIND_HAS_STORAGE(userIndexRelation->rd_rel->relkind))
		RelationDropStorage(userIndexRelation);

	/* 确保事务提交时统计信息也被删除 */
	pgstat_drop_relation(userIndexRelation);

	/*
	 * 关闭并刷新索引的关系缓存条目，
	 * 以确保在我们删除目录条目时关系缓存不会试图重建它。
	 * 不过我们保留该锁。
	 */
	index_close(userIndexRelation, NoLock);

	RelationForgetRelation(indexId);

	/*
	 * 修正 INDEX 关系，并检查是否为表达式索引
	 */
	indexRelation = table_open(IndexRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);

	hasexprs = !heap_attisnull(tuple, Anum_pg_index_indexprs,
							   RelationGetDescr(indexRelation));

	CatalogTupleDelete(indexRelation, &tuple->t_self);

	ReleaseSysCache(tuple);
	table_close(indexRelation, RowExclusiveLock);

	/*
	 * 如果它有任何表达式列，我们可能已经存储了
	 * 关于它们的统计信息。
	 */
	if (hasexprs)
		RemoveStatistics(indexId, 0);

	/*
	 * 修正 ATTRIBUTE 关系
	 */
	DeleteAttributeTuples(indexId);

	/*
	 * 修正 RELATION 关系
	 */
	DeleteRelationTuple(indexId);

	/*
	 * 修正 INHERITS 关系
	 */
	DeleteInheritsTuple(indexId, InvalidOid, false, NULL);

	/*
	 * 我们现在懒得去计算 relhasindex 的新的正确值
	 * （如有必要，下一次 VACUUM 会修正它）。
	 * 因此无需更新所属关系的 pg_class 元组。
	 * 但我们必须在所属关系上发出共享缓存失效通知，
	 * 以确保其他后端更新其关系缓存中的索引列表。
	 * （在并发情况下，这是多余但无害的。）
	 */
	CacheInvalidateRelcache(userHeapRelation);

	/*
	 * 关闭所属关系，但保留锁
	 */
	table_close(userHeapRelation, NoLock);

	/*
	 * 离开之前释放会话锁。
	 */
	if (concurrent)
	{
		UnlockRelationIdForSession(&heaprelid, ShareUpdateExclusiveLock);
		UnlockRelationIdForSession(&indexrelid, ShareUpdateExclusiveLock);
	}
}

/* ----------------------------------------------------------------
 *						index_build 支持
 * ----------------------------------------------------------------
 */

/* ----------------
 *		BuildIndexInfo
 *			构造一个打开索引的 IndexInfo 记录
 *
 * IndexInfo 存储 FormIndexDatum 所需的关于索引的信息，
 * FormIndexDatum 既用于 index_build()，也用于以后单个索引元组的插入。
 * 通常我们每条命令只为索引构建一次 IndexInfo，
 * 然后（可能）用于多个元组。
 * ----------------
 */
IndexInfo *
BuildIndexInfo(Relation index)
{
	IndexInfo  *ii;
	Form_pg_index indexStruct = index->rd_index;
	int			i;
	int			numAtts;

	/* 检查键的数量，并把属性号复制到 IndexInfo 中 */
	numAtts = indexStruct->indnatts;
	if (numAtts < 1 || numAtts > INDEX_MAX_KEYS)
		elog(ERROR, "invalid indnatts %d for index %u",
			 numAtts, RelationGetRelid(index));

	/*
	 * 创建该节点，获取表达式索引所需的表达式
	 * 以及索引谓词（如果有）。
	 */
	ii = makeIndexInfo(indexStruct->indnatts,
					   indexStruct->indnkeyatts,
					   index->rd_rel->relam,
					   RelationGetIndexExpressions(index),
					   RelationGetIndexPredicate(index),
					   indexStruct->indisunique,
					   indexStruct->indnullsnotdistinct,
					   indexStruct->indisready,
					   false,
					   index->rd_indam->amsummarizing);

	/* 填写属性号 */
	for (i = 0; i < numAtts; i++)
		ii->ii_IndexAttrNumbers[i] = indexStruct->indkey.values[i];

	/* 获取排他约束信息（如果有） */
	if (indexStruct->indisexclusion)
	{
		RelationGetExclusionInfo(index,
								 &ii->ii_ExclusionOps,
								 &ii->ii_ExclusionProcs,
								 &ii->ii_ExclusionStrats);
	}

	ii->ii_OpclassOptions = RelationGetIndexRawAttOptions(index);

	return ii;
}

/* ----------------
 *		BuildDummyIndexInfo
 *			构造一个打开索引的哑 IndexInfo 记录
 *
 * 它与真正的 BuildIndexInfo 的区别在于，它绝不会运行
 * 索引表达式或谓词中可能存在的任何用户定义代码。
 * 我们不返回真正的索引表达式，而是返回具有正确
 * 类型/typmod/排序规则的空常量。谓词和排他子句被直接忽略。
 * 这对于截断索引的用途来说已经足够，
 * 因为我们无需真正对表达式或谓词求值；
 * 对这些数据唯一可能做的处理就是构造一个
 * 描述该索引行类型的 tupdesc。
 * ----------------
 */
IndexInfo *
BuildDummyIndexInfo(Relation index)
{
	IndexInfo  *ii;
	Form_pg_index indexStruct = index->rd_index;
	int			i;
	int			numAtts;

	/* 检查键的数量，并把属性号复制到 IndexInfo 中 */
	numAtts = indexStruct->indnatts;
	if (numAtts < 1 || numAtts > INDEX_MAX_KEYS)
		elog(ERROR, "invalid indnatts %d for index %u",
			 numAtts, RelationGetRelid(index));

	/*
	 * 创建该节点，使用哑索引表达式，
	 * 并假装不存在谓词。
	 */
	ii = makeIndexInfo(indexStruct->indnatts,
					   indexStruct->indnkeyatts,
					   index->rd_rel->relam,
					   RelationGetDummyIndexExpressions(index),
					   NIL,
					   indexStruct->indisunique,
					   indexStruct->indnullsnotdistinct,
					   indexStruct->indisready,
					   false,
					   index->rd_indam->amsummarizing);

	/* 填写属性号 */
	for (i = 0; i < numAtts; i++)
		ii->ii_IndexAttrNumbers[i] = indexStruct->indkey.values[i];

	/* 我们忽略排他约束（如果有） */

	return ii;
}

/*
 * CompareIndexInfo
 *		返回两个索引（位于不同的表中）的属性是否表明它们
 *		具有"相同"的定义。
 *
 * 注意：单独传递排序规则和操作符族是一种权宜做法。把它们
 * 加入 IndexInfo 可能让此处及其他地方的代码写得更好。
 *
 * 使用 build_attrmap_by_name(index2, index1) 来构建 attmap。
 */
bool
CompareIndexInfo(IndexInfo *info1, IndexInfo *info2,
				 Oid *collations1, Oid *collations2,
				 Oid *opfamilies1, Oid *opfamilies2,
				 AttrMap *attmap)
{
	int			i;

	if (info1->ii_Unique != info2->ii_Unique)
		return false;

	if (info1->ii_NullsNotDistinct != info2->ii_NullsNotDistinct)
		return false;

	/* 只有当两个索引使用相同的访问方法时它们才等价 */
	if (info1->ii_Am != info2->ii_Am)
		return false;

	/* 且属性数量相同 */
	if (info1->ii_NumIndexAttrs != info2->ii_NumIndexAttrs)
		return false;

	/* 且键属性数量相同 */
	if (info1->ii_NumIndexKeyAttrs != info2->ii_NumIndexKeyAttrs)
		return false;

	/*
	 * 并且各列通过属性映射匹配
	 * （实际的属性号可能不同！）注意这会检查
	 * 表达式形式的索引列是否出现在相同的位置上。
	 * 接下来我们将比较表达式本身。
	 */
	for (i = 0; i < info1->ii_NumIndexAttrs; i++)
	{
		if (attmap->maplen < info2->ii_IndexAttrNumbers[i])
			elog(ERROR, "incorrect attribute map");

		/* 目前忽略表达式（但检查其排序规则/操作符族） */
		if (!(info1->ii_IndexAttrNumbers[i] == InvalidAttrNumber &&
			  info2->ii_IndexAttrNumbers[i] == InvalidAttrNumber))
		{
			/* 如果只有一个索引在该列上有表达式，则失败 */
			if (info1->ii_IndexAttrNumbers[i] == InvalidAttrNumber ||
				info2->ii_IndexAttrNumbers[i] == InvalidAttrNumber)
				return false;

			/* 两者都是列，所以检查映射后是否匹配 */
			if (attmap->attnums[info2->ii_IndexAttrNumbers[i] - 1] !=
				info1->ii_IndexAttrNumbers[i])
				return false;
		}

		/* 排序规则和操作符族对包含列无效 */
		if (i >= info1->ii_NumIndexKeyAttrs)
			continue;

		if (collations1[i] != collations2[i])
			return false;
		if (opfamilies1[i] != opfamilies2[i])
			return false;
	}

	/*
	 * 对于表达式索引：要么两者都是表达式索引，要么都不是；
	 * 如果是，则确保表达式匹配。
	 */
	if ((info1->ii_Expressions != NIL) != (info2->ii_Expressions != NIL))
		return false;
	if (info1->ii_Expressions != NIL)
	{
		bool		found_whole_row;
		Node	   *mapped;

		mapped = map_variable_attnos((Node *) info2->ii_Expressions,
									 1, 0, attmap,
									 InvalidOid, &found_whole_row);
		if (found_whole_row)
		{
			/*
			 * 我们本可以在此抛出一个错误，
			 * 但这似乎超出了本例程的职责范围。
			 */
			return false;
		}

		if (!equal(info1->ii_Expressions, mapped))
			return false;
	}

	/* 部分索引的谓词（如果存在）必须完全相同 */
	if ((info1->ii_Predicate == NULL) != (info2->ii_Predicate == NULL))
		return false;
	if (info1->ii_Predicate != NULL)
	{
		bool		found_whole_row;
		Node	   *mapped;

		mapped = map_variable_attnos((Node *) info2->ii_Predicate,
									 1, 0, attmap,
									 InvalidOid, &found_whole_row);
		if (found_whole_row)
		{
			/*
			 * 我们本可以在此抛出一个错误，
			 * 但这似乎超出了本例程的职责范围。
			 */
			return false;
		}
		if (!equal(info1->ii_Predicate, mapped))
			return false;
	}

	/* 目前不支持比较排他约束索引。 */
	if (info1->ii_ExclusionOps != NULL || info2->ii_ExclusionOps != NULL)
		return false;

	return true;
}

/* ----------------
 *		BuildSpeculativeIndexInfo
 *			向 IndexInfo 记录添加额外状态
 *
 * 对于唯一索引，我们通常不希望向 IndexInfo 添加用于检查唯一性的
 * 信息，因为 B-Tree AM 直接处理该工作。然而，在 speculative 插入
 * 的情况下，需要额外的支持。
 *
 * 在此而非 BuildIndexInfo() 中做这段处理，
 * 是为了不在常见的非 speculative 场景中付出该开销。
 * ----------------
 */
void
BuildSpeculativeIndexInfo(Relation index, IndexInfo *ii)
{
	int			indnkeyatts;
	int			i;

	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);

	/*
	 * 获取用于检查唯一索引的信息。
	 */
	Assert(ii->ii_Unique);

	if (index->rd_rel->relam != BTREE_AM_OID)
		elog(ERROR, "unexpected non-btree speculative unique index");

	ii->ii_UniqueOps = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	ii->ii_UniqueProcs = (Oid *) palloc(sizeof(Oid) * indnkeyatts);
	ii->ii_UniqueStrats = (uint16 *) palloc(sizeof(uint16) * indnkeyatts);

	/*
	 * 我们必须查找运算符的策略号。这也对
	 * 该运算符确实与该索引匹配进行了交叉检查。
	 */
	/* 我们还需要函数 OID 和策略号 */
	for (i = 0; i < indnkeyatts; i++)
	{
		ii->ii_UniqueStrats[i] = BTEqualStrategyNumber;
		ii->ii_UniqueOps[i] =
			get_opfamily_member(index->rd_opfamily[i],
								index->rd_opcintype[i],
								index->rd_opcintype[i],
								ii->ii_UniqueStrats[i]);
		if (!OidIsValid(ii->ii_UniqueOps[i]))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 ii->ii_UniqueStrats[i], index->rd_opcintype[i],
				 index->rd_opcintype[i], index->rd_opfamily[i]);
		ii->ii_UniqueProcs[i] = get_opcode(ii->ii_UniqueOps[i]);
	}
}

/* ----------------
 *		FormIndexDatum
 *			为新的索引元组构造 values[] 和 isnull[] 数组。
 *
 *	indexInfo		关于索引的信息
 *	slot			我们必须为之准备索引条目的堆元组
 *	estate			用于对索引表达式求值的执行器状态
 *	values			索引 Datum 的数组（输出区）
 *	isnull			空值指示器的数组（输出区）
 *
 * 当不存在索引表达式时，estate 可以是 NULL。否则，
 * 必须提供它，*并且*其逐元组表达式上下文的 ecxt_scantuple 插槽
 * 必须指向传入的堆元组。
 *
 * 注意我们在此并不实际调用 index_form_tuple()；我们只是准备
 * 其输入数组 values[] 和 isnull[]。这是因为索引 AM
 * 可能希望在存储之前修改数据。
 * ----------------
 */
void
FormIndexDatum(IndexInfo *indexInfo,
			   TupleTableSlot *slot,
			   EState *estate,
			   Datum *values,
			   bool *isnull)
{
	ListCell   *indexpr_item;
	int			i;

	if (indexInfo->ii_Expressions != NIL &&
		indexInfo->ii_ExpressionsState == NIL)
	{
		/* 第一次通过时，建立表达式求值状态 */
		indexInfo->ii_ExpressionsState =
			ExecPrepareExprList(indexInfo->ii_Expressions, estate);
		/* 检查调用方是否已正确建立上下文 */
		Assert(GetPerTupleExprContext(estate)->ecxt_scantuple == slot);
	}
	indexpr_item = list_head(indexInfo->ii_ExpressionsState);

	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
	{
		int			keycol = indexInfo->ii_IndexAttrNumbers[i];
		Datum		iDatum;
		bool		isNull;

		if (keycol < 0)
			iDatum = slot_getsysattr(slot, keycol, &isNull);
		else if (keycol != 0)
		{
			/*
			 * 普通索引列；
			 * 直接从堆元组获取我们需要的值。
			 */
			iDatum = slot_getattr(slot, keycol, &isNull);
		}
		else
		{
			/*
			 * 索引表达式 --- 需要对它求值。
			 */
			if (indexpr_item == NULL)
				elog(ERROR, "wrong number of index expressions");
			iDatum = ExecEvalExprSwitchContext((ExprState *) lfirst(indexpr_item),
											   GetPerTupleExprContext(estate),
											   &isNull);
			indexpr_item = lnext(indexInfo->ii_ExpressionsState, indexpr_item);
		}
		values[i] = iDatum;
		isnull[i] = isNull;
	}

	if (indexpr_item != NULL)
		elog(ERROR, "wrong number of index expressions");
}


/*
 * index_update_stats --- 在 CREATE INDEX 或 REINDEX 之后更新 pg_class 条目
 *
 * 本例程在 CREATE INDEX 或 REINDEX 之后更新索引或其父关系
 * 的 pg_class 行。它相当古怪的 API 旨在确保我们能用一次更新
 * 完成所有必需的工作。
 *
 * hasindex: 把 relhasindex 设为该值
 * reltuples: 若 >= 0，则把 reltuples 设为该值；否则不更改
 *
 * 若 reltuples >= 0，relpages 和 relallvisible 也会被更新
 * （使用 RelationGetNumberOfBlocks() 和 visibilitymap_count()）。
 *
 * 注意：本操作的一个重要副作用是，一条 SI 失效消息会发给所有后端
 * --- 包括我自己 --- 使关系缓存条目被刷新或用新数据更新。
 * 即使我们发现 pg_class 行无需更改，这也必须发生。
 * 在更新堆条目时，这确保其他后端得知新索引的存在。
 * 在更新索引时这很重要，
 * 因为某些索引 AM 期望在 REINDEX 之后发生
 * 关系缓存刷新。
 */
static void
index_update_stats(Relation rel,
				   bool hasindex,
				   double reltuples)
{
	bool		update_stats;
	BlockNumber relpages = 0;	/* 让编译器保持安静 */
	BlockNumber relallvisible = 0;
	Oid			relid = RelationGetRelid(rel);
	Relation	pg_class;
	ScanKeyData key[1];
	HeapTuple	tuple;
	void	   *state;
	Form_pg_class rd_rel;
	bool		dirty;

	/*
	 * 作为一个特殊的 hack，如果我们处理的是空表
	 * 而现有的 reltuples 为 -1，我们就不去动它。
	 * 这确保作为 CREATE TABLE 一部分创建索引不会使该表
	 * 过早地看起来像是已经被 VACUUM 处理过。
	 * 我们修改的 rd_rel 可能与 rel->rd_rel 不同，
	 * 例如由于并发 GRANT 的提交，但更改 reltuples 的命令
	 * 所获取的锁与我们的锁冲突。（即使某个命令在较弱的锁下
	 * 更改了 reltuples，这也只影响空表的统计信息。）
	 */
	if (reltuples == 0 && rel->rd_rel->reltuples < 0)
		reltuples = -1;

	update_stats = reltuples >= 0;

	/*
	 * 在 systable_inplace_update_begin() 锁定 pg_class 缓冲区之前，
	 * 先完成 I/O 和可见性映射缓冲区的锁处理。
	 * 我们修改的 rd_rel 可能与 rel->rd_rel 不同，
	 * 例如由于并发 GRANT 的提交，但没有任何命令会把 relkind
	 * 从非索引改为索引。（即使有，relallvisible 也不会破坏功能。）
	 */
	if (update_stats)
	{
		relpages = RelationGetNumberOfBlocks(rel);

		if (rel->rd_rel->relkind != RELKIND_INDEX)
			visibilitymap_count(rel, &relallvisible, NULL);
	}

	/*
	 * 我们总是使用非事务性的、原地覆盖更新
	 * 来更新 pg_class 行。这样做有以下几个原因：
	 *
	 * 1. 在引导模式下，我们别无选择 --- UPDATE 行不通。
	 *
	 * 2. 我们可能正在重建 pg_class 自身的索引，这种情况下我们无法移动
	 * 它的 pg_class 行，因为 CatalogTupleInsert/CatalogTupleUpdate
	 * 可能尚不了解所有索引（见 reindex_relation）。
	 *
	 * 3. 因为我们执行 CREATE INDEX 时只对父关系持有共享锁（以允许并发
	 * 创建索引），普通更新可能与大约同时提交的另一个 CREATE INDEX
	 * 发生元组并发更新失败。
	 * 我们通过让它们都
	 * 进行非事务性更新来避免这一点
	 * （我们假定它们都想把 pg_class 行改成同样的内容，
	 * 所以谁先谁后并不重要）。
	 *
	 * 即使我们的事务在提交之前仍可能失败，
	 * 使用非事务性更新也是安全的。
	 * 即使没有任何索引，把 relhasindex 设为 true 也是安全的
	 * （VACUUM 最终会修正它）。当然，新的 relpages
	 * 和 reltuples 计数
	 * 无论如何都是正确的。不过，如果调用方没有提供更新的 reltuples
	 * 计数，我们就不想更改 relpages（或 relallvisible），
	 * 因为那会搞乱真正重要的 reltuples/relpages 比率。
	 */

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	systable_inplace_update_begin(pg_class, ClassOidIndexId, true, NULL,
								  1, key, &tuple, &state);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for relation %u", relid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* 这是否应当是一个更全面的测试？ */
	Assert(rd_rel->relkind != RELKIND_PARTITIONED_INDEX);

	/* 对复制出来的元组应用必需的更新（如果有） */

	dirty = false;
	if (rd_rel->relhasindex != hasindex)
	{
		rd_rel->relhasindex = hasindex;
		dirty = true;
	}

	if (update_stats)
	{
		if (rd_rel->relpages != (int32) relpages)
		{
			rd_rel->relpages = (int32) relpages;
			dirty = true;
		}
		if (rd_rel->reltuples != (float4) reltuples)
		{
			rd_rel->reltuples = (float4) reltuples;
			dirty = true;
		}
		if (rd_rel->relallvisible != (int32) relallvisible)
		{
			rd_rel->relallvisible = (int32) relallvisible;
			dirty = true;
		}
	}

	/*
	 * 如果有任何更改，就把元组写出
	 */
	if (dirty)
	{
		systable_inplace_update_finish(state, tuple);
		/* 上面会发送一条缓存失效消息 */
	}
	else
	{
		systable_inplace_update_cancel(state);
		/* 无需更改元组，但仍强制关系缓存失效 */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);

	table_close(pg_class, RowExclusiveLock);
}


/*
 * index_build - 调用访问方法特定的索引构建例程
 *
 * 进入时，索引的目录条目有效，
 * 其物理磁盘文件已创建但为空。
 * 我们调用 AM 特定的构建例程来填写索引内容。然后我们使用
 * ambuild 返回的统计数据以及调用方传递的数据，按需更新索引
 * 和堆关系的 pg_class 条目。
 *
 * isreindex 表示我们正在重新创建一个先前存在的索引。
 * parallel 表示并行化是否可能有用。
 *
 * 注意：在 Postgres 8.2 之前，传入的堆和索引 Relation
 * 会由本例程自动关闭。现在不再是这样了。
 * 调用方打开了它们，就该由调用方关闭它们。
 */
void
index_build(Relation heapRelation,
			Relation indexRelation,
			IndexInfo *indexInfo,
			bool isreindex,
			bool parallel)
{
	IndexBuildResult *stats;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	/*
	 * 健全性检查
	 */
	Assert(RelationIsValid(indexRelation));
	Assert(PointerIsValid(indexRelation->rd_indam));
	Assert(PointerIsValid(indexRelation->rd_indam->ambuild));
	Assert(PointerIsValid(indexRelation->rd_indam->ambuildempty));

	/*
	 * 确定并行 CREATE INDEX 的工作进程细节。目前
	 * 只有 btree 支持并行构建。
	 *
	 * 注意规划器为我们考虑了并行安全性。
	 */
	if (parallel && IsNormalProcessingMode() &&
		indexRelation->rd_rel->relam == BTREE_AM_OID)
		indexInfo->ii_ParallelWorkers =
			plan_create_index_workers(RelationGetRelid(heapRelation),
									  RelationGetRelid(indexRelation));

	if (indexInfo->ii_ParallelWorkers == 0)
		ereport(DEBUG1,
				(errmsg_internal("building index \"%s\" on table \"%s\" serially",
								 RelationGetRelationName(indexRelation),
								 RelationGetRelationName(heapRelation))));
	else
		ereport(DEBUG1,
				(errmsg_internal("building index \"%s\" on table \"%s\" with request for %d parallel workers",
								 RelationGetRelationName(indexRelation),
								 RelationGetRelationName(heapRelation),
								 indexInfo->ii_ParallelWorkers)));

	/*
	 * 切换到表所有者的 userid，使任何索引函数都以该用户身份运行。
	 * 同时限制安全敏感操作，并使 GUC 变量的更改
	 * 仅限于本命令。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRelation->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/* 设置初始进度报告状态 */
	{
		const int	progress_index[] = {
			PROGRESS_CREATEIDX_PHASE,
			PROGRESS_CREATEIDX_SUBPHASE,
			PROGRESS_CREATEIDX_TUPLES_DONE,
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE,
			PROGRESS_SCAN_BLOCKS_TOTAL
		};
		const int64 progress_vals[] = {
			PROGRESS_CREATEIDX_PHASE_BUILD,
			PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE,
			0, 0, 0, 0
		};

		pgstat_progress_update_multi_param(6, progress_index, progress_vals);
	}

	/*
	 * 调用访问方法的构建例程
	 */
	stats = indexRelation->rd_indam->ambuild(heapRelation, indexRelation,
											 indexInfo);
	Assert(PointerIsValid(stats));

	/*
	 * 如果这是一个非日志索引，我们可能需要为它写出一个 init fork
	 * ——但必须先检查是否已存在。例如，如果一个非日志关系
	 * 在创建它的事务中被截断，或在随后的事务中被截断两次，
	 * relfilenumber 不会改变，
	 * 这里就无需做任何事情。
	 */
	if (indexRelation->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
		!smgrexists(RelationGetSmgr(indexRelation), INIT_FORKNUM))
	{
		smgrcreate(RelationGetSmgr(indexRelation), INIT_FORKNUM, false);
		log_smgrcreate(&indexRelation->rd_locator, INIT_FORKNUM);
		indexRelation->rd_indam->ambuildempty(indexRelation);
	}

	/*
	 * 如果我们发现任何可能损坏的 HOT 链，就把索引标记为在当前事务
	 * 降到事件视野之下之前不可使用。有关讨论见
	 * src/backend/access/heap/README.HOT。如果堆关系启用了早期
	 * 修剪/清理，也要设置此项。尽管根据实际的清理活动
	 * 和其他活跃事务，索引可能更早就可以安全使用，
	 * 但对此的判断会复杂得多，
	 * 并且需要某种形式的阻塞，所以为了简单快速，
	 * 我们只使用当前事务。
	 *
	 * 然而，在重建已有索引的索引时，我们在此什么也不该做。
	 * 任何相对于该索引而言损坏的 HOT 链都必定早于该索引最初的
	 * 创建，因此无需更改索引的可用性视野。
	 * 此外，在重建 pg_index 自身时我们*不得*尝试更改
	 * 索引的 pg_index 条目，而这一优化
	 * 恰好很好地防止了这一点。
	 * 重建索引所需的更复杂规则
	 * 在本函数返回之后单独处理。
	 *
	 * 在并发索引构建期间我们也不需要设置 indcheckxmin，
	 * 因为在所有关心损坏的 HOT 链或早期修剪/清理的事务消失之前，
	 * 我们不会把 indisvalid 设为真。
	 *
	 * 因此，这条代码路径只能在非并发的 CREATE INDEX 期间走到。
	 * 所以 heap_update 会设置 pg_index 元组的 xmin 这一点
	 * 并不重要，因为该元组本来就是在当前事务中创建的。
	 * 这也意味着我们无需担心该元组的任何并发读者；
	 * 还没有其他事务能看到它。
	 */
	if ((indexInfo->ii_BrokenHotChain || EarlyPruningEnabled(heapRelation)) &&
		!isreindex &&
		!indexInfo->ii_Concurrent)
	{
		Oid			indexId = RelationGetRelid(indexRelation);
		Relation	pg_index;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;

		pg_index = table_open(IndexRelationId, RowExclusiveLock);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(indexId));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexId);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		/* 如果是新索引，indcheckxmin 不应被设置 ... */
		Assert(!indexForm->indcheckxmin);

		indexForm->indcheckxmin = true;
		CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

		heap_freetuple(indexTuple);
		table_close(pg_index, RowExclusiveLock);
	}

	/*
	 * 更新堆和索引的 pg_class 行
	 */
	index_update_stats(heapRelation,
					   true,
					   stats->heap_tuples);

	index_update_stats(indexRelation,
					   false,
					   stats->index_tuples);

	/* 使更新后的目录行版本可见 */
	CommandCounterIncrement();

	/*
	 * 如果这是为排他约束服务的，则对堆再扫描一遍
	 * 以验证该约束得到满足。我们必须等到索引完全有效
	 * 之后才能这样做。（不过损坏的 HOT 链应该没有关系；
	 * 见 IndexCheckExclusion 的注释。）
	 */
	if (indexInfo->ii_ExclusionOps != NULL)
		IndexCheckExclusion(heapRelation, indexRelation, indexInfo);

	/* 回滚索引函数执行的任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);
}

/*
 * IndexCheckExclusion - 验证新的排他约束得到满足
 *
 * 在创建排他约束时，我们先正常构建索引，然后重新扫描堆
 * 以检查冲突。我们假定只需验证根据最新快照仍然存活的元组，
 * 并且即使存在损坏的 HOT 链，这些元组也被正确地索引了。
 * 这应当是没问题的，因为我们至少对该表
 * 持有 ShareLock，
 * 这意味着其他事务不可能有未提交的更新。
 * （注意：这对于系统目录未必行得通，因为许多操作会提前
 * 释放系统目录上的写锁。）
 */
static void
IndexCheckExclusion(Relation heapRelation,
					Relation indexRelation,
					IndexInfo *indexInfo)
{
	TableScanDesc scan;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;

	/*
	 * 如果我们正在重建目标索引，则把它标记为不再处于重建状态，
	 * 以防止我们试图用该索引进行探测时 index_beginscan 中的 Assert。
	 * 这是没问题的，因为该索引现在已经完全有效。
	 */
	if (ReindexIsCurrentlyProcessingIndex(RelationGetRelid(indexRelation)))
		ResetReindexProcessing();

	/*
	 * 需要一个 EState 来对索引表达式和部分索引谓词求值。
	 * 还需要一个插槽来保存当前元组。
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);

	/* 让 econtext 的扫描元组成为被测试的元组 */
	econtext->ecxt_scantuple = slot;

	/* 为谓词建立执行状态（如果有）。 */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * 扫描基础关系中所有存活的元组。
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan_strat(heapRelation,	/* 关系 */
								 snapshot,	/* 快照 */
								 0, /* 键的数量 */
								 NULL,	/* 扫描键 */
								 true,	/* 缓冲区访问策略 OK */
								 true); /* 允许使用 syncscan（同步扫描） */

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * 在部分索引中，忽略不满足谓词的元组。
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * 提取索引列的值，包括计算表达式。
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * 检查该元组没有任何冲突。
		 */
		check_exclusion_constraint(heapRelation,
								   indexRelation, indexInfo,
								   &(slot->tts_tid), values, isnull,
								   estate, true);

		MemoryContextReset(econtext->ecxt_per_tuple_memory);
	}

	table_endscan(scan);
	UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* 这些可能一直指向现已消失的 estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}


/*
 * validate_index - 并发索引构建的支持代码
 *
 * 我们通过以下方式做并发索引构建：先经由 index_create() 插入索引的
 * 目录条目，并把它标记为非 indisready 且非 indisvalid。
 * 然后提交我们的事务并开始一个新的，
 * 接着等待所有可能修改过该表的事务终止。
 * 现在我们知道，任何随后启动的事务都会看到该索引并
 * 遵守它对 HOT 更新的约束；所以尽管现有的 HOT 链
 * 相对于该索引可能已损坏，
 * 但当前存活的元组不会被做出不兼容的 HOT 更新。
 * 现在我们通过 index_build() 正常构建索引，同时持有一个允许并发
 * 插入/更新/删除的弱锁。此外，我们只为扫描开始时有效的元组
 * 建立索引（见 table_index_build_scan），而正常构建会注意
 * 把最近死亡的元组也包括进来。这是没问题的，因为在我们标记
 * 该索引有效之前，可能会看到那些元组的所有事务都会消失。
 * 这样做的原因是为了避免因并发 UPDATE 而导致的虚假唯一索引
 * 失败（如果使用 HeapTupleSatisfiesVacuum，
 * 当我们扫过同一行的
 * 不同版本时，可能会把它们都视为有效）。这样我们得到的索引
 * 不包含在我们构建索引期间加入到表中的任何元组。
 *
 * 接下来，我们把索引标记为 "indisready"（但仍不是 "indisvalid"），
 * 提交第二个事务并开始第三个。
 * 同样我们等待所有可能修改过该表的
 * 事务终止。现在我们知道，任何随后启动的事务都会看到该索引并
 * 把它们的新元组插入其中。
 * 然后我们取一个新的参考快照
 * 并把它传给 validate_index()。任何根据该快照有效、但不在索引中
 * 的元组，都必须被加入到索引中。
 * （在该快照之后提交为存活的元组，将由其发起事务插入到索引中。
 * 在该快照之前提交为死亡的元组无需被索引，因为我们会在标记索引
 * 有效之前等待所有可能关心它们的事务结束。）
 *
 * validate_index() 的工作方式是：先用一个 bulkdelete 回调收集索引中
 * 当前所有的 TID，该回调只存储 TID，绝不会说 "删除它"。
 * （这应当比普通索引扫描更快；而且并非所有索引 AM 都支持
 * 全索引扫描。）然后我们对 TID 排序，
 * 最后扫描表，与 TID 列表做
 * "归并连接"，看哪些元组在索引中缺失。这样我们将确保所有
 * 根据参考快照有效的元组都在索引中。
 *
 * 以这种方式构建唯一索引很棘手：
 * 我们可能试图插入一个已经死亡或
 * 正在被删除的元组，而且我们不得对同一行的更新版本出现唯一性
 * 失败。我们可以尝试检查该元组是否已经死亡，并告诉 index_insert()
 * 不做唯一性检查，但这仍然留下
 * 与进行中更新的竞态条件。为此，
 * 我们期望索引 AM 在宣布唯一性错误之前，重新检查待插入元组的存活性。
 *
 * 完成 validate_index() 之后，
 * 我们等待参考快照时存活的所有事务
 * 都消失；这有必要确保
 * 没有事务还持有比参考快照更旧的事务快照
 * （因而可能看到我们未索引的元组）。然后我们把索引标记为
 * "indisvalid" 并提交。随后的事务将能够用它进行查询。
 *
 * 做两次全表扫描是一种粗暴的策略。
 * 我们可以设法更聪明些，例如把
 * 新元组存入表中的特殊区域
 * （或许通过设置 use_fsm 让表只追加）。
 * 然而那会带来更多锁方面的问题。
 */
void
validate_index(Oid heapId, Oid indexId, Snapshot snapshot)
{
	Relation	heapRelation,
				indexRelation;
	IndexInfo  *indexInfo;
	IndexVacuumInfo ivinfo;
	ValidateIndexState state;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	{
		const int	progress_index[] = {
			PROGRESS_CREATEIDX_PHASE,
			PROGRESS_CREATEIDX_TUPLES_DONE,
			PROGRESS_CREATEIDX_TUPLES_TOTAL,
			PROGRESS_SCAN_BLOCKS_DONE,
			PROGRESS_SCAN_BLOCKS_TOTAL
		};
		const int64 progress_vals[] = {
			PROGRESS_CREATEIDX_PHASE_VALIDATE_IDXSCAN,
			0, 0, 0, 0
		};

		pgstat_progress_update_multi_param(5, progress_index, progress_vals);
	}

	/* 打开并锁定父堆关系 */
	heapRelation = table_open(heapId, ShareUpdateExclusiveLock);

	/*
	 * 切换到表所有者的 userid，使任何索引函数都以该用户身份运行。
	 * 同时限制安全敏感操作，并使 GUC 变量的更改
	 * 仅限于本命令。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRelation->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	indexRelation = index_open(indexId, RowExclusiveLock);

	/*
	 * 获取 index_insert 所需的信息。
	 * （你可能认为这应当从 DefineIndex 传入，
	 * 但它的副本早已因为是在先前的事务中构建的而丢失了。）
	 */
	indexInfo = BuildIndexInfo(indexRelation);

	/* 只是出于一致性而把构建标记为并发 */
	indexInfo->ii_Concurrent = true;

	/*
	 * 扫描索引并把所有 TID 收集到一个 tuplesort 对象中。
	 */
	ivinfo.index = indexRelation;
	ivinfo.heaprel = heapRelation;
	ivinfo.analyze_only = false;
	ivinfo.report_progress = true;
	ivinfo.estimated_count = true;
	ivinfo.message_level = DEBUG2;
	ivinfo.num_heap_tuples = heapRelation->rd_rel->reltuples;
	ivinfo.strategy = NULL;

	/*
	 * 把 TID 编码为 int8 值来排序，而不是直接对项指针排序。
	 * 这样可以明显更快，主要是因为 TID
	 * 在所有平台上都是
	 * 按引用传递的类型，而 int8 在大多数平台上按值传递。
	 */
	state.tuplesort = tuplesort_begin_datum(INT8OID, Int8LessOperator,
											InvalidOid, false,
											maintenance_work_mem,
											NULL, TUPLESORT_NONE);
	state.htups = state.itups = state.tups_inserted = 0;

	/* ambulkdelete 更新进度指标 */
	(void) index_bulk_delete(&ivinfo, NULL,
							 validate_index_callback, (void *) &state);

	/* 执行排序 */
	{
		const int	progress_index[] = {
			PROGRESS_CREATEIDX_PHASE,
			PROGRESS_SCAN_BLOCKS_DONE,
			PROGRESS_SCAN_BLOCKS_TOTAL
		};
		const int64 progress_vals[] = {
			PROGRESS_CREATEIDX_PHASE_VALIDATE_SORT,
			0, 0
		};

		pgstat_progress_update_multi_param(3, progress_index, progress_vals);
	}
	tuplesort_performsort(state.tuplesort);

	/*
	 * 现在扫描堆并将其与索引"归并"
	 */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE,
								 PROGRESS_CREATEIDX_PHASE_VALIDATE_TABLESCAN);
	table_index_validate_scan(heapRelation,
							  indexRelation,
							  indexInfo,
							  snapshot,
							  &state);

	/* tuplesort 对象使用完毕 */
	tuplesort_end(state.tuplesort);

	elog(DEBUG2,
		 "validate_index found %.0f heap tuples, %.0f index tuples; inserted %.0f missing tuples",
		 state.htups, state.itups, state.tups_inserted);

	/* 回滚索引函数执行的任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* 关闭关系，但保留锁 */
	index_close(indexRelation, NoLock);
	table_close(heapRelation, NoLock);
}

/*
 * validate_index_callback - 用于收集索引 TID 的 bulkdelete 回调
 */
static bool
validate_index_callback(ItemPointer itemptr, void *opaque)
{
	ValidateIndexState *state = (ValidateIndexState *) opaque;
	int64		encoded = itemptr_encode(itemptr);

	tuplesort_putdatum(state->tuplesort, Int64GetDatum(encoded), false);
	state->itups += 1;
	return false;				/* 从不实际删除任何东西 */
}

/*
 * index_set_state_flags - 调整 pg_index 状态标志
 *
 * 在 CREATE/DROP INDEX CONCURRENTLY 期间使用本函数来调整
 * 表示索引状态的 pg_index 标志。
 *
 * 注意 CatalogTupleUpdate() 会为该元组发送缓存失效消息，
 * 因此我们一旦提交，其他会话就会听到这次更新。
 */
void
index_set_state_flags(Oid indexId, IndexStateFlagsAction action)
{
	Relation	pg_index;
	HeapTuple	indexTuple;
	Form_pg_index indexForm;

	/* 打开 pg_index 并获取该索引元组的一份可写副本 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);

	indexTuple = SearchSysCacheCopy1(INDEXRELID,
									 ObjectIdGetDatum(indexId));
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "cache lookup failed for index %u", indexId);
	indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

	/* 在副本上执行所要求的状态更改 */
	switch (action)
	{
		case INDEX_CREATE_SET_READY:
			/* 在 CREATE INDEX CONCURRENTLY 序列中设置 indisready */
			Assert(indexForm->indislive);
			Assert(!indexForm->indisready);
			Assert(!indexForm->indisvalid);
			indexForm->indisready = true;
			break;
		case INDEX_CREATE_SET_VALID:
			/* 在 CREATE INDEX CONCURRENTLY 序列中设置 indisvalid */
			Assert(indexForm->indislive);
			Assert(indexForm->indisready);
			Assert(!indexForm->indisvalid);
			indexForm->indisvalid = true;
			break;
		case INDEX_DROP_CLEAR_VALID:

			/*
			 * 在 DROP INDEX CONCURRENTLY 序列中清除 indisvalid
			 *
			 * 如果 indisready == true，我们保留它的设置，这样该索引仍然
			 * 会被活跃事务维护。我们只需确保 indisvalid 为假。
			 * （不过我们不断言二者初始都为真，
			 * 因为我们希望能够重试
			 * 中途失败的 DROP INDEX CONCURRENTLY。）
			 *
			 * 注意：CLUSTER 的逻辑假定任何无效索引上都不会设置
			 * indisclustered，因此也要清除该标志。为了整洁，
			 * 也清除 indisreplident。
			 */
			indexForm->indisvalid = false;
			indexForm->indisclustered = false;
			indexForm->indisreplident = false;
			break;
		case INDEX_DROP_SET_DEAD:

			/*
			 * 在 DROP INDEX CONCURRENTLY 中清除 indisready/indislive
			 *
			 * 我们同时清除 indisready 和 indislive，因为我们不仅
			 * 要停止更新，
			 * 还要阻止会话触及该索引。
			 */
			Assert(!indexForm->indisvalid);
			Assert(!indexForm->indisclustered);
			Assert(!indexForm->indisreplident);
			indexForm->indisready = false;
			indexForm->indislive = false;
			break;
	}

	/* ... 并更新它 */
	CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

	table_close(pg_index, RowExclusiveLock);
}


/*
 * IndexGetRelation：给定一个索引的关系 OID，获取该索引所
 * 基于的关系的 OID。使用系统缓存。
 */
Oid
IndexGetRelation(Oid indexId, bool missing_ok)
{
	HeapTuple	tuple;
	Form_pg_index index;
	Oid			result;

	tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexId));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return InvalidOid;
		elog(ERROR, "cache lookup failed for index %u", indexId);
	}
	index = (Form_pg_index) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);

	result = index->indrelid;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * reindex_index - 本例程用于重新创建单个索引
 */
void
reindex_index(Oid indexId, bool skip_constraint_checks, char persistence,
			  ReindexParams *params)
{
	Relation	iRel,
				heapRelation;
	Oid			heapId;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	IndexInfo  *indexInfo;
	volatile bool skipped_constraint = false;
	PGRUsage	ru0;
	bool		progress = ((params->options & REINDEXOPT_REPORT_PROGRESS) != 0);
	bool		set_tablespace = false;

	pg_rusage_init(&ru0);

	/*
	 * 打开并锁定父堆关系。ShareLock 就足够了，因为我们只需
	 * 确保没有模式或数据变更在进行。
	 */
	heapId = IndexGetRelation(indexId,
							  (params->options & REINDEXOPT_MISSING_OK) != 0);
	/* 如果关系不存在，离开 */
	if (!OidIsValid(heapId))
		return;

	if ((params->options & REINDEXOPT_MISSING_OK) != 0)
		heapRelation = try_table_open(heapId, ShareLock);
	else
		heapRelation = table_open(heapId, ShareLock);

	/* 如果关系已消失，离开 */
	if (!heapRelation)
		return;

	/*
	 * 切换到表所有者的 userid，使任何索引函数都以该用户身份运行。
	 * 同时限制安全敏感操作，并使 GUC 变量的更改
	 * 仅限于本命令。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(heapRelation->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	if (progress)
	{
		const int	progress_cols[] = {
			PROGRESS_CREATEIDX_COMMAND,
			PROGRESS_CREATEIDX_INDEX_OID
		};
		const int64 progress_vals[] = {
			PROGRESS_CREATEIDX_COMMAND_REINDEX,
			indexId
		};

		pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX,
									  heapId);
		pgstat_progress_update_multi_param(2, progress_cols, progress_vals);
	}

	/*
	 * 打开目标索引关系并对它加排他锁，以确保
	 * 没有其他人正在触及这个特定的索引。
	 */
	if ((params->options & REINDEXOPT_MISSING_OK) != 0)
		iRel = try_index_open(indexId, AccessExclusiveLock);
	else
		iRel = index_open(indexId, AccessExclusiveLock);

	/* 如果索引关系已消失，离开 */
	if (!iRel)
	{
		/* 回滚任何 GUC 更改 */
		AtEOXact_GUC(false, save_nestlevel);

		/* 恢复 userid 和安全上下文 */
		SetUserIdAndSecContext(save_userid, save_sec_context);

		/* 关闭父堆关系，但保留锁 */
		table_close(heapRelation, NoLock);
		return;
	}

	if (progress)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_ACCESS_METHOD_OID,
									 iRel->rd_rel->relam);

	/*
	 * 分区索引绝不应当在这里被处理，
	 * 因为它们没有物理存储。
	 */
	if (iRel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		elog(ERROR, "cannot reindex partitioned index \"%s.%s\"",
			 get_namespace_name(RelationGetNamespace(iRel)),
			 RelationGetRelationName(iRel));

	/*
	 * 不允许重建其他后端的临时表 ... 它们的本地缓冲区
	 * 管理器应付不了。
	 */
	if (RELATION_IS_OTHER_TEMP(iRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex temporary tables of other sessions")));

	/*
	 * 不允许重建 TOAST 表上的无效索引。这是失败的
	 * REINDEX CONCURRENTLY 留下的残余，如果重建了它，
	 * 就再也没办法删除它了。
	 */
	if (IsToastNamespace(RelationGetNamespace(iRel)) &&
		!get_index_isvalid(indexId))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot reindex invalid index on TOAST table")));

	/*
	 * 即使启用了 allow_system_table_mods，系统关系也不能被移动，
	 * 以与并发情况保持一致：在并发情况下，
	 * 一个关系的所有索引
	 * 都会被依次处理，其中包括 toast 关系的索引。
	 *
	 * 注意，本检查不属于 CheckRelationTableSpaceMove()，
	 * 因为后者用于可能级联到 toast 关系的
	 * ALTER TABLE SET TABLESPACE。
	 */
	if (OidIsValid(params->tablespaceOid) &&
		IsSystemRelation(iRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move system relation \"%s\"",
						RelationGetRelationName(iRel))));

	/* 检查是否需要更改该索引的表空间 */
	if (OidIsValid(params->tablespaceOid) &&
		CheckRelationTableSpaceMove(iRel, params->tablespaceOid))
		set_tablespace = true;

	/*
	 * 还要检查当前事务中对该索引的活跃使用；
	 * 我们不想在一个打开的索引扫描之下重建它。
	 */
	CheckTableNotInUse(iRel, "REINDEX INDEX");

	/* 设置新的表空间（如果要求） */
	if (set_tablespace)
	{
		/* 更新它的 pg_class 行 */
		SetRelationTableSpace(iRel, params->tablespaceOid, InvalidOid);

		/*
		 * 安排在事务提交时解除链接旧索引的存储。
		 */
		RelationDropStorage(iRel);
		RelationAssumeNewRelfilelocator(iRel);

		/* 确保 reltablespace 的更改可见 */
		CommandCounterIncrement();
	}

	/*
	 * 该索引上所有的谓词锁都即将失效。把它们提升为
	 * 堆上的关系锁。
	 */
	TransferPredicateLocksToHeapRelation(iRel);

	/* 获取 index_build 所需的信息 */
	indexInfo = BuildIndexInfo(iRel);

	/* 如果要求，跳过唯一性/排他约束检查 */
	if (skip_constraint_checks)
	{
		if (indexInfo->ii_Unique || indexInfo->ii_ExclusionOps != NULL)
			skipped_constraint = true;
		indexInfo->ii_Unique = false;
		indexInfo->ii_ExclusionOps = NULL;
		indexInfo->ii_ExclusionProcs = NULL;
		indexInfo->ii_ExclusionStrats = NULL;
	}

	/* 在重建期间禁用目标索引的使用 */
	SetReindexProcessing(heapId, indexId);

	/* 为该索引创建一个新的物理关系 */
	RelationSetNewRelfilenumber(iRel, persistence);

	/* 初始化索引并重建 */
	/* 注意：我们无需重新建立 pkey 设置 */
	index_build(heapRelation, iRel, indexInfo, true, true);

	/* 重新允许使用目标索引 */
	ResetReindexProcessing();

	/*
	 * 如果该索引被标记为无效/未就绪/已死（即它来自失败的
	 * CREATE INDEX CONCURRENTLY，或 DROP INDEX CONCURRENTLY 中途失败），
	 * 并且我们没有跳过唯一性检查，那么现在可以把它标记为有效。
	 * 这使得 REINDEX 可以用来清理这类情况。
	 *
	 * 我们还可以重置 indcheckxmin，因为我们现在已经做了一次
	 * 非并发的索引构建，*除了*
	 * index_build 发现某些仍然损坏的 HOT
	 * 链的情况。如果确实如此，而我们又不必更改其他任何标志，
	 * 我们就干脆不动 indcheckxmin（注意 index_build 不会更改它，
	 * 因为这是一次重建）。这样做是可以的也是可取的，因为不更新该
	 * 元组使索引的可用性视野（记录为该元组的 xmin 值）保持原样。
	 *
	 * 但是，如果该索引是无效/未就绪/已死的并且存在损坏的 HOT 链，
	 * 我们最好强制把 indcheckxmin 设为真，
	 * 因为 HOT 链不会与索引冲突
	 * 这一通常的论断，对无效索引而言是可疑的。
	 * （如果索引是已死的，
	 * 冲突肯定是可能的。其他情况下大概不该发生，但我们还是保守些。）
	 * 在这种情况下推进可用性视野是恰当的。
	 *
	 * 在此避免不必要更新的另一个原因是：
	 * 在重建 pg_index 自身时，我们不得尝试更新其中的元组。
	 * pg_index 的索引应当总是处于这些
	 * 标志的干净状态，所以那不会发生。
	 *
	 * 如果堆关系启用了早期修剪/清理，
	 * 则每次构建或重建都必须把
	 * 可用性视野推进到当前事务。
	 * pg_index 在这方面没问题，因为
	 * 目录表不受早期清理的影响。
	 */
	if (!skipped_constraint)
	{
		Relation	pg_index;
		HeapTuple	indexTuple;
		Form_pg_index indexForm;
		bool		index_bad;
		bool		early_pruning_enabled = EarlyPruningEnabled(heapRelation);

		pg_index = table_open(IndexRelationId, RowExclusiveLock);

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(indexId));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexId);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		index_bad = (!indexForm->indisvalid ||
					 !indexForm->indisready ||
					 !indexForm->indislive);
		if (index_bad ||
			(indexForm->indcheckxmin && !indexInfo->ii_BrokenHotChain) ||
			early_pruning_enabled)
		{
			if (!indexInfo->ii_BrokenHotChain && !early_pruning_enabled)
				indexForm->indcheckxmin = false;
			else if (index_bad || early_pruning_enabled)
				indexForm->indcheckxmin = true;
			indexForm->indisvalid = true;
			indexForm->indisready = true;
			indexForm->indislive = true;
			CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

			/*
			 * 使该表的关系缓存失效，以便我们提交之后
			 * 所有会话都会刷新该表的索引列表。
			 * 这确保了
			 * 如果有人在这次更新期间错过了 pg_index 行，
			 * 他们在对该表做任何更新之前会先刷新自己的列表。
			 */
			CacheInvalidateRelcache(heapRelation);
		}

		table_close(pg_index, RowExclusiveLock);
	}

	/* 记录我们做了什么 */
	if ((params->options & REINDEXOPT_VERBOSE) != 0)
		ereport(INFO,
				(errmsg("index \"%s\" was reindexed",
						get_rel_name(indexId)),
				 errdetail_internal("%s",
									pg_rusage_show(&ru0))));

	/* 回滚索引函数执行的任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* 关闭关系，但保留锁 */
	index_close(iRel, NoLock);
	table_close(heapRelation, NoLock);

	if (progress)
		pgstat_progress_end_command();
}

/*
 * reindex_relation - 本例程用于重新创建一个关系的
 * 所有索引（如果有 toast 关系，也可以选择连它一起重建）。
 *
 * "flags" 是一个位掩码，可以包含以下各位的任意组合：
 *
 * REINDEX_REL_PROCESS_TOAST：若为 true，也处理 toast 表（如果有）。
 *
 * REINDEX_REL_SUPPRESS_INDEX_USE：若为 true，则该关系刚刚被
 * VACUUM FULL 或 CLUSTER 之类的操作完全重建，
 * 因此其索引与它不一致。
 * 如果该关系是我们重建索引期间可能查阅的系统目录，这就麻烦了。
 * 为处理这种情况，我们把所有索引标记为待重建，
 * 这样在重建之前
 * 它们不会被信任。调用方必须*在*没有通过 CommandCounterIncrement
 * 让重建后的表可见的情况下调用我们；我们会在收集索引列表之后
 * 做 CCI。（这样我们在收集列表期间仍能使用目录索引。）
 *
 * REINDEX_REL_CHECK_CONSTRAINTS：若为 true，重新检查唯一和排他
 * 约束条件，否则不检查。
 * 为避免死锁，对系统目录的 VACUUM FULL 或
 * CLUSTER 必须省略此标志。如果怀疑约束不一致，应当使用 REINDEX
 * 来重建索引。为了最佳性能，
 * 其他调用方只应在以可能改变约束有效性
 * 的方式转换了数据之后，才包含该标志。
 *
 * REINDEX_REL_FORCE_INDEXES_UNLOGGED：若为 true，把重建索引的
 * 持久性设为非日志。
 *
 * REINDEX_REL_FORCE_INDEXES_PERMANENT：若为 true，把重建索引的
 * 持久性设为永久。
 *
 * 若有任何索引被重建则返回 true（相关时包括 toast 表的索引）。
 * 注意每次索引重建之后都会发生一次 CommandCounterIncrement。
 */
bool
reindex_relation(Oid relid, int flags, ReindexParams *params)
{
	Relation	rel;
	Oid			toast_relid;
	List	   *indexIds;
	char		persistence;
	bool		result;
	ListCell   *indexId;
	int			i;

	/*
	 * 打开并锁定该关系。ShareLock 就足够了，因为我们只需
	 * 防止其中的模式和数据变更。这里使用的锁级别
	 * 应当与 ReindexTable() 一致。
	 */
	if ((params->options & REINDEXOPT_MISSING_OK) != 0)
		rel = try_table_open(relid, ShareLock);
	else
		rel = table_open(relid, ShareLock);

	/* 如果关系已消失，离开 */
	if (!rel)
		return false;

	/*
	 * 分区表绝不应当在这里被处理，
	 * 因为它们没有物理存储。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		elog(ERROR, "cannot reindex partitioned table \"%s.%s\"",
			 get_namespace_name(RelationGetNamespace(rel)),
			 RelationGetRelationName(rel));

	toast_relid = rel->rd_rel->reltoastrelid;

	/*
	 * 获取该关系的索引 OID 列表。
	 * （如果我们忽略系统索引，
	 * 我们信任关系缓存能用顺序扫描来获取它。）
	 */
	indexIds = RelationGetIndexList(rel);

	if (flags & REINDEX_REL_SUPPRESS_INDEX_USE)
	{
		/* 在它们被重建之前禁用所有索引的使用 */
		SetReindexPending(indexIds);

		/*
		 * 让新的堆内容可见 ---
		 * 现在事情可能不一致了！
		 */
		CommandCounterIncrement();
	}

	/*
	 * 计算索引的持久性：与所属关系的相同，
	 * 除非调用方另有指定。
	 */
	if (flags & REINDEX_REL_FORCE_INDEXES_UNLOGGED)
		persistence = RELPERSISTENCE_UNLOGGED;
	else if (flags & REINDEX_REL_FORCE_INDEXES_PERMANENT)
		persistence = RELPERSISTENCE_PERMANENT;
	else
		persistence = rel->rd_rel->relpersistence;

	/* 重建所有索引。 */
	i = 1;
	foreach(indexId, indexIds)
	{
		Oid			indexOid = lfirst_oid(indexId);
		Oid			indexNamespaceId = get_rel_namespace(indexOid);

		/*
		 * 跳过 TOAST 表上的任何无效索引。它们只可能是失败的
		 * REINDEX CONCURRENTLY 留下的重复残余，如果重建了它们，
		 * 就再也没办法删除它们了。
		 */
		if (IsToastNamespace(indexNamespaceId) &&
			!get_index_isvalid(indexOid))
		{
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot reindex invalid index \"%s.%s\" on TOAST table, skipping",
							get_namespace_name(indexNamespaceId),
							get_rel_name(indexOid))));

			/*
			 * 把这个无效的 toast 索引从待重建索引列表中移除，
			 * 因为此处它因 reindex_index() 中会发生的硬失败
			 * 而被跳过，即使我们试图处理它。
			 */
			if (flags & REINDEX_REL_SUPPRESS_INDEX_USE)
				RemoveReindexPending(indexOid);
			continue;
		}

		reindex_index(indexOid, !(flags & REINDEX_REL_CHECK_CONSTRAINTS),
					  persistence, params);

		CommandCounterIncrement();

		/* 索引不应再出现在待处理列表中 */
		Assert(!ReindexIsProcessingIndex(indexOid));

		/* 设置索引重建计数 */
		pgstat_progress_update_param(PROGRESS_CLUSTER_INDEX_REBUILD_COUNT,
									 i);
		i++;
	}

	/*
	 * 关闭关系，但继续持有该锁。
	 */
	table_close(rel, NoLock);

	result = (indexIds != NIL);

	/*
	 * 如果该关系有辅助 toast 关系，在我们仍持有主表
	 * 锁期间也重建它。
	 */
	if ((flags & REINDEX_REL_PROCESS_TOAST) && OidIsValid(toast_relid))
	{
		/*
		 * 注意，如果 toast 关系缺失，这应当失败，
		 * 所以重置 REINDEXOPT_MISSING_OK。即使为父关系设置了新的
		 * 表空间，其 toast 表上的索引也不会被移动。
		 * 这一规则通过把 tablespaceOid 设为 InvalidOid 来强制执行。
		 */
		ReindexParams newparams = *params;

		newparams.options &= ~(REINDEXOPT_MISSING_OK);
		newparams.tablespaceOid = InvalidOid;
		result |= reindex_relation(toast_relid, flags, &newparams);
	}

	return result;
}


/* ----------------------------------------------------------------
 *		系统索引重建支持
 *
 * 当我们忙于重建系统目录索引时，这段代码提供支持，
 * 防止目录查找使用该索引。
 * 我们还用它来捕捉重建这些索引期间
 * 对用户索引的使用企图。该信息被传播给并行工作进程；
 * 在并行操作期间试图更改它是不被允许的。
 * ----------------------------------------------------------------
 */

static Oid	currentlyReindexedHeap = InvalidOid;
static Oid	currentlyReindexedIndex = InvalidOid;
static List *pendingReindexedIndexes = NIL;
static int	reindexingNestLevel = 0;

/*
 * ReindexIsProcessingHeap
 *		若 OID 指定的堆当前正在被重建则为真。
 */
bool
ReindexIsProcessingHeap(Oid heapOid)
{
	return heapOid == currentlyReindexedHeap;
}

/*
 * ReindexIsCurrentlyProcessingIndex
 *		若 OID 指定的索引当前正在被重建则为真。
 */
static bool
ReindexIsCurrentlyProcessingIndex(Oid indexOid)
{
	return indexOid == currentlyReindexedIndex;
}

/*
 * ReindexIsProcessingIndex
 *		若 OID 指定的索引当前正在被重建，或者因等待重建
 *		而应被视为无效，则为真。
 */
bool
ReindexIsProcessingIndex(Oid indexOid)
{
	return indexOid == currentlyReindexedIndex ||
		list_member_oid(pendingReindexedIndexes, indexOid);
}

/*
 * SetReindexProcessing
 *		设置标志以表示指定的堆/索引正在被重建。
 */
static void
SetReindexProcessing(Oid heapOid, Oid indexOid)
{
	Assert(OidIsValid(heapOid) && OidIsValid(indexOid));
	/* 重建索引不可重入。 */
	if (OidIsValid(currentlyReindexedHeap))
		elog(ERROR, "cannot reindex while reindexing");
	currentlyReindexedHeap = heapOid;
	currentlyReindexedIndex = indexOid;
	/* 索引不再处于"待重建"状态。 */
	RemoveReindexPending(indexOid);
	/* 这可能已经被设置，但万一没有，现在设置它。 */
	reindexingNestLevel = GetCurrentTransactionNestLevel();
}

/*
 * ResetReindexProcessing
 *		清除重建索引状态。
 */
static void
ResetReindexProcessing(void)
{
	currentlyReindexedHeap = InvalidOid;
	currentlyReindexedIndex = InvalidOid;
	/* reindexingNestLevel 保持设置直到（子）事务结束 */
}

/*
 * SetReindexPending
 *		把给定的索引标记为待重建。
 *
 * 注意：我们假定当前内存上下文在整个过程中保持有效。
 */
static void
SetReindexPending(List *indexes)
{
	/* 重建索引不可重入。 */
	if (pendingReindexedIndexes)
		elog(ERROR, "cannot reindex while reindexing");
	if (IsInParallelMode())
		elog(ERROR, "cannot modify reindex state during a parallel operation");
	pendingReindexedIndexes = list_copy(indexes);
	reindexingNestLevel = GetCurrentTransactionNestLevel();
}

/*
 * RemoveReindexPending
 *		把给定的索引从待处理列表中移除。
 */
static void
RemoveReindexPending(Oid indexOid)
{
	if (IsInParallelMode())
		elog(ERROR, "cannot modify reindex state during a parallel operation");
	pendingReindexedIndexes = list_delete_oid(pendingReindexedIndexes,
											  indexOid);
}

/*
 * ResetReindexState
 *		在（子）事务中止期间清除所有重建索引状态。
 */
void
ResetReindexState(int nestLevel)
{
	/*
	 * 因为重建索引不可重入，
	 * 我们无需处理嵌套的重建状态。
	 * 我们只需避免在 REINDEX 内部子事务失败时搞乱外层状态。
	 * 所以把当前嵌套级别与重建操作的嵌套级别相比就足够了。
	 */
	if (reindexingNestLevel >= nestLevel)
	{
		currentlyReindexedHeap = InvalidOid;
		currentlyReindexedIndex = InvalidOid;

		/*
		 * 我们无需尝试释放 pendingReindexedIndexes 的内容；
		 * 该列表应当位于一个事务生存期的上下文中，因此它会
		 * 自动消失。
		 */
		pendingReindexedIndexes = NIL;

		reindexingNestLevel = 0;
	}
}

/*
 * EstimateReindexStateSpace
 *		估算把重建状态传给并行工作进程所需的空间。
 */
Size
EstimateReindexStateSpace(void)
{
	return offsetof(SerializedReindexState, pendingReindexedIndexes)
		+ mul_size(sizeof(Oid), list_length(pendingReindexedIndexes));
}

/*
 * SerializeReindexState
 *		为并行工作进程序列化重建状态。
 */
void
SerializeReindexState(Size maxsize, char *start_address)
{
	SerializedReindexState *sistate = (SerializedReindexState *) start_address;
	int			c = 0;
	ListCell   *lc;

	sistate->currentlyReindexedHeap = currentlyReindexedHeap;
	sistate->currentlyReindexedIndex = currentlyReindexedIndex;
	sistate->numPendingReindexedIndexes = list_length(pendingReindexedIndexes);
	foreach(lc, pendingReindexedIndexes)
		sistate->pendingReindexedIndexes[c++] = lfirst_oid(lc);
}

/*
 * RestoreReindexState
 *		在并行工作进程中恢复重建状态。
 */
void
RestoreReindexState(void *reindexstate)
{
	SerializedReindexState *sistate = (SerializedReindexState *) reindexstate;
	int			c = 0;
	MemoryContext oldcontext;

	currentlyReindexedHeap = sistate->currentlyReindexedHeap;
	currentlyReindexedIndex = sistate->currentlyReindexedIndex;

	Assert(pendingReindexedIndexes == NIL);
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	for (c = 0; c < sistate->numPendingReindexedIndexes; ++c)
		pendingReindexedIndexes =
			lappend_oid(pendingReindexedIndexes,
						sistate->pendingReindexedIndexes[c]);
	MemoryContextSwitchTo(oldcontext);

	/* 注意该工作进程有它自己的事务嵌套级别 */
	reindexingNestLevel = GetCurrentTransactionNestLevel();
}
