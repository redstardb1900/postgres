/*-------------------------------------------------------------------------
 *
 * heap.c
 *	  创建与销毁 POSTGRES 堆关系的代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/heap.c
 *
 *
 * INTERFACE ROUTINES
 *		heap_create()			- 创建未编目的堆关系
 *		heap_create_with_catalog() - 创建已编目的关系
 *		heap_drop_with_catalog() - 从目录中移除指定名称的关系
 *
 * NOTES
 *	  本代码取自 access/heap/create.c，其中包含
 *	  旧的 heap_create_with_catalog、amcreate 和 amdestroy。
 *	  那些例程很快就会通过函数管理器
 *	  调用这些例程，
 *	  就像命名糟糕的 "NewXXX" 例程那样。
 *	  "New" 例程很快就会彻底消失，一了百了！
 *		-cim 1/13/91
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* 可能由 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_heap_pg_class_oid = InvalidOid;
Oid			binary_upgrade_next_toast_pg_class_oid = InvalidOid;
RelFileNumber binary_upgrade_next_heap_pg_class_relfilenumber = InvalidRelFileNumber;
RelFileNumber binary_upgrade_next_toast_pg_class_relfilenumber = InvalidRelFileNumber;

static void AddNewRelationTuple(Relation pg_class_desc,
								Relation new_rel_desc,
								Oid new_rel_oid,
								Oid new_type_oid,
								Oid reloftype,
								Oid relowner,
								char relkind,
								TransactionId relfrozenxid,
								TransactionId relminmxid,
								Datum relacl,
								Datum reloptions);
static ObjectAddress AddNewRelationType(const char *typeName,
										Oid typeNamespace,
										Oid new_rel_oid,
										char new_rel_kind,
										Oid ownerid,
										Oid new_row_type,
										Oid new_array_type);
static void RelationRemoveInheritance(Oid relid);
static Oid	StoreRelCheck(Relation rel, const char *ccname, Node *expr,
						  bool is_validated, bool is_local, int inhcount,
						  bool is_no_inherit, bool is_internal);
static void StoreConstraints(Relation rel, List *cooked_constraints,
							 bool is_internal);
static bool MergeWithExistingConstraint(Relation rel, const char *ccname, Node *expr,
										bool allow_merge, bool is_local,
										bool is_initially_valid,
										bool is_no_inherit);
static void SetRelationNumChecks(Relation rel, int numchecks);
static Node *cookConstraint(ParseState *pstate,
							Node *raw_constraint,
							char *relname);


/* ----------------------------------------------------------------
 *				XXX UGLY HARD CODED BADNESS FOLLOWS XXX
 *
 *		这些都应该移到 lib/catalog 模块的某处，
 *		如果不是先被消灭的话。
 * ----------------------------------------------------------------
 */


/*
 * 注意：
 *		将来系统是否应对这些属性做特殊处理？
 *		优点：	在 ATTRIBUTE 关系中占用的空间少得多。
 *		缺点：  特殊情况会遍布各处。
 */

/*
 * 下面的初始化器不含尾部变长字段，
 * 但这没问题 — 反正我们从不引用结构中
 * 固定大小部分以外的任何内容。可默认为
 * 零的字段也不再提及。
 */

static const FormData_pg_attribute a1 = {
	.attname = {"ctid"},
	.atttypid = TIDOID,
	.attlen = sizeof(ItemPointerData),
	.attnum = SelfItemPointerAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = false,
	.attalign = TYPALIGN_SHORT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a2 = {
	.attname = {"xmin"},
	.atttypid = XIDOID,
	.attlen = sizeof(TransactionId),
	.attnum = MinTransactionIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a3 = {
	.attname = {"cmin"},
	.atttypid = CIDOID,
	.attlen = sizeof(CommandId),
	.attnum = MinCommandIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a4 = {
	.attname = {"xmax"},
	.atttypid = XIDOID,
	.attlen = sizeof(TransactionId),
	.attnum = MaxTransactionIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a5 = {
	.attname = {"cmax"},
	.atttypid = CIDOID,
	.attlen = sizeof(CommandId),
	.attnum = MaxCommandIdAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

/*
 * 我们决定把此属性叫做 "tableoid" 而不是
 * "classoid"，依据是将来某一 class/type 可能
 * 对应多张表。无论如何 table 仍是
 * SQL 中使用的词。
 */
static const FormData_pg_attribute a6 = {
	.attname = {"tableoid"},
	.atttypid = OIDOID,
	.attlen = sizeof(Oid),
	.attnum = TableOidAttributeNumber,
	.attcacheoff = -1,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute *SysAtt[] = {&a1, &a2, &a3, &a4, &a5, &a6};

/*
 * 本函数返回系统属性的 Form_pg_attribute 指针。
 * 注意若给出的 attno 无效我们会 elog，而这
 * 只应在上游出问题时发生。
 */
const FormData_pg_attribute *
SystemAttributeDefinition(AttrNumber attno)
{
	if (attno >= 0 || attno < -(int) lengthof(SysAtt))
		elog(ERROR, "invalid system attribute number %d", attno);
	return SysAtt[-attno - 1];
}

/*
 * 若给定名称是系统属性名，返回原型定义的
 * Form_pg_attribute 指针。否则返回 NULL。
 */
const FormData_pg_attribute *
SystemAttributeByName(const char *attname)
{
	int			j;

	for (j = 0; j < (int) lengthof(SysAtt); j++)
	{
		const FormData_pg_attribute *att = SysAtt[j];

		if (strcmp(NameStr(att->attname), attname) == 0)
			return att;
	}

	return NULL;
}


/* ----------------------------------------------------------------
 *				XXX END OF UGLY HARD CODED BADNESS XXX
 * ---------------------------------------------------------------- */


/* ----------------------------------------------------------------
 *		heap_create		- 创建未编目的堆关系
 *
 *		注意 API 变更：调用方现在必须始终提供要用于
 *		该关系的 OID。relfilenumber 可以（并且在
 *		最简单的情况下就是）保持未指定。
 *
 *		create_storage 表示是否创建存储。
 *		然而，即便 create_storage 为 true，若 relkind
 *		是没有存储的那种，也不会创建存储。
 *
 *		rel->rd_rel 由 RelationBuildLocalRelation 初始化，
 *		返回时大多为零。
 * ----------------------------------------------------------------
 */
Relation
heap_create(const char *relname,
			Oid relnamespace,
			Oid reltablespace,
			Oid relid,
			RelFileNumber relfilenumber,
			Oid accessmtd,
			TupleDesc tupDesc,
			char relkind,
			char relpersistence,
			bool shared_relation,
			bool mapped_relation,
			bool allow_system_table_mods,
			TransactionId *relfrozenxid,
			MultiXactId *relminmxid,
			bool create_storage)
{
	Relation	rel;

	/* 调用方必须已为该关系提供 OID。 */
	Assert(OidIsValid(relid));

	/*
	 * 不允许直接在 pg_catalog 中创建关系，尽管把
	 * 用户定义关系移到那里是允许的。包含 pg_catalog
	 * 的搜索路径语义目前太令人困惑。
	 *
	 * 但即便 allow_system_table_mods = off，也允许在
	 * pg_catalog 中的关系上创建索引，上层已保证那是
	 * 用户定义关系，不是系统关系。
	 */
	if (!allow_system_table_mods &&
		((IsCatalogNamespace(relnamespace) && relkind != RELKIND_INDEX) ||
		 IsToastNamespace(relnamespace)) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create \"%s.%s\"",
						get_namespace_name(relnamespace), relname),
				 errdetail("System catalog modifications are currently disallowed.")));

	*relfrozenxid = InvalidTransactionId;
	*relminmxid = InvalidMultiXactId;

	/*
	 * 若该关系种类不支持表空间，则强制 reltablespace
	 * 为零。这主要是为了干净。
	 */
	if (!RELKIND_HAS_TABLESPACE(relkind))
		reltablespace = InvalidOid;

	/* 对没有物理存储的 relkind 不创建存储。 */
	if (!RELKIND_HAS_STORAGE(relkind))
		create_storage = false;
	else
	{
		/*
		 * 若调用方未指定 relfilenumber，则用与 relid
		 * 相同的 oid 创建存储。
		 */
		if (!RelFileNumberIsValid(relfilenumber))
			relfilenumber = relid;
	}

	/*
	 * 绝不允许 pg_class 条目在 reltablespace 中显式指定
	 * 数据库的默认表空间；改为强制为零。这样可
	 * 确保若数据库被克隆到不同的默认
	 * 表空间，pg_class 条目仍与 CREATE DATABASE
	 * 放置物理复制关系的位置相符。
	 *
	 * 是的，这有点 hack。
	 */
	if (reltablespace == MyDatabaseTableSpace)
		reltablespace = InvalidOid;

	/*
	 * 构造关系缓存条目。
	 */
	rel = RelationBuildLocalRelation(relname,
									 relnamespace,
									 tupDesc,
									 relid,
									 accessmtd,
									 relfilenumber,
									 reltablespace,
									 shared_relation,
									 mapped_relation,
									 relpersistence,
									 relkind);

	/*
	 * 若需要，让存储管理器创建该关系的磁盘文件。
	 *
	 * 对于表，AM 回调会同时创建主分支和 init 分支。
	 * 对于其他，只创建主分支；其余分支将按
	 * 需创建。
	 */
	if (create_storage)
	{
		if (RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
			table_relation_set_new_filelocator(rel, &rel->rd_locator,
											   relpersistence,
											   relfrozenxid, relminmxid);
		else if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
			RelationCreateStorage(rel->rd_locator, relpersistence, true);
		else
			Assert(false);
	}

	/*
	 * 若指定了表空间，移除该表空间通常由
	 * 物理文件的存在来保护；但对没有文件的
	 * 关系，添加一条 pg_shdepend 条目来说明这一点。
	 */
	if (!create_storage && reltablespace != InvalidOid)
		recordDependencyOnTablespace(RelationRelationId, relid,
									 reltablespace);

	/* 确保事务中止时统计信息会被删除 */
	pgstat_create_relation(rel);

	return rel;
}

/* ----------------------------------------------------------------
 *		heap_create_with_catalog		- 创建已编目的关系
 *
 *		分多个步骤完成：
 *
 *		1) 用 CheckAttributeNamesTypes() 确认元组
 *		   描述符含有有效的属性名和类型集合
 *
 *		2) 打开 pg_class，并由 get_relname_relid()
 *		   执行扫描，确保尚不存在同名
 *		   关系。
 *
 *		3) 调用 heap_create() 在磁盘上创建新关系。
 *
 *		4) 调用 TypeCreate() 定义与新关系
 *		   对应的新类型。
 *
 *		5) 调用 AddNewRelationTuple() 在 pg_class
 *		   中登记该关系。
 *
 *		6) 调用 AddNewAttributeTuples() 在 pg_attribute
 *		   中登记新关系的模式。
 *
 *		7) 调用 StoreConstraints ()		- vadim 08/22/97
 *
 *		8) 关闭各关系，并返回新关系的
 *		   oid。
 *
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		CheckAttributeNamesTypes
 *
 *		用来确认元组描述符含有有效的
 *		属性名和数据类型集合。出问题只是
 *		生成 ereport(ERROR)，从而中止当前事务。
 *
 *		relkind 是要创建的关系的 relkind。
 *		flags 控制允许哪些数据类型，参见 CheckAttributeType。
 * --------------------------------
 */
void
CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind,
						 int flags)
{
	int			i;
	int			j;
	int			natts = tupdesc->natts;

	/* 列数合理性检查 */
	if (natts < 0 || natts > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * 首先检查是否与系统属性名冲突
	 *
	 * 对视图或类型关系跳过此项，因为它们没有
	 * 系统属性。
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE)
	{
		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (SystemAttributeByName(NameStr(attr->attname)) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" conflicts with a system column name",
								NameStr(attr->attname))));
		}
	}

	/*
	 * 接着检查重复的属性名
	 */
	for (i = 1; i < natts; i++)
	{
		for (j = 0; j < i; j++)
		{
			if (strcmp(NameStr(TupleDescAttr(tupdesc, j)->attname),
					   NameStr(TupleDescAttr(tupdesc, i)->attname)) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" specified more than once",
								NameStr(TupleDescAttr(tupdesc, j)->attname))));
		}
	}

	/*
	 * 接着检查属性类型
	 */
	for (i = 0; i < natts; i++)
	{
		CheckAttributeType(NameStr(TupleDescAttr(tupdesc, i)->attname),
						   TupleDescAttr(tupdesc, i)->atttypid,
						   TupleDescAttr(tupdesc, i)->attcollation,
						   NIL, /* 假定我们正在创建新的行类型 */
						   flags);
	}
}

/* --------------------------------
 *		CheckAttributeType
 *
 *		验证提议的属性数据类型是否合法。
 *		这主要是因为目录中有些类型（以及伪类型）
 *		我们不支持作为真实元组的元素。
 *		我们也检查表列所需的其他一些性质。
 *
 * 若属性是提议添加到已有表或
 * 复合类型上的，将行类型 OID 作为单元素列表传入
 * containing_rowtypes。检查即将创建的行类型时，
 * 传入 NIL 即可，因为不可能递归引用
 * 尚不存在的行类型。
 *
 * flags 是控制允许哪些数据类型的位掩码。大体上
 * 伪类型不允许作为属性类型，但也有一些
 * 例外：ANYARRAYOID、RECORDOID 和 RECORDARRAYOID 在
 * 某些情况下可以允许。（这能工作是因为那些类型类的值在
 * 某种程度上可自我识别。然而，RECORDOID 和 RECORDARRAYOID
 * 只在会话内能可靠识别，因为身份信息
 * 可能使用仅本地分配的 typmod。期望
 * 调用方知道这些情况是否安全。）
 *
 * flags 也可控制错误消息的措辞。若
 * 指定了 CHKATYPE_IS_PARTKEY，"attname" 应是分区键
 * 列号的文本，而不是真正的列名。
 * --------------------------------
 */
void
CheckAttributeType(const char *attname,
				   Oid atttypid, Oid attcollation,
				   List *containing_rowtypes,
				   int flags)
{
	char		att_typtype = get_typtype(atttypid);
	Oid			att_typelem;

	/* 本函数会递归，因此可能把栈撑爆 */
	check_stack_depth();

	if (att_typtype == TYPTYPE_PSEUDO)
	{
		/*
		 * 我们禁止伪类型列，例外是当调用方
		 * 说 ANYARRAY、RECORD 和 RECORD[] 可以时。
		 *
		 * 对 RECORD 和 RECORD[] 不必担心递归包含，
		 * 因为 (a) 不应允许命名复合类型
		 * 包含它们，且 (b) 两个"匿名"记录类型不可能
		 * 被视为同一类型，因此无限递归并
		 * 不可能。
		 */
		if (!((atttypid == ANYARRAYOID && (flags & CHKATYPE_ANYARRAY)) ||
			  (atttypid == RECORDOID && (flags & CHKATYPE_ANYRECORD)) ||
			  (atttypid == RECORDARRAYOID && (flags & CHKATYPE_ANYRECORD))))
		{
			if (flags & CHKATYPE_IS_PARTKEY)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				/* translator: first %s is an integer not a name */
						 errmsg("partition key column %s has pseudo-type %s",
								attname, format_type_be(atttypid))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("column \"%s\" has pseudo-type %s",
								attname, format_type_be(atttypid))));
		}
	}
	else if (att_typtype == TYPTYPE_DOMAIN)
	{
		/*
		 * 若是域，递归检查其基类型。
		 */
		CheckAttributeType(attname, getBaseType(atttypid), attcollation,
						   containing_rowtypes,
						   flags);
	}
	else if (att_typtype == TYPTYPE_COMPOSITE)
	{
		/*
		 * 对复合类型，递归进入其属性。
		 */
		Relation	relation;
		TupleDesc	tupdesc;
		int			i;

		/*
		 * 检查自包含。最终我们或许能允许
		 * 这一点（若如此，直接返回而不抱怨），但尚不清楚
		 * 还要在多少其他地方加上反递归防护之后，
		 * 才可以安全地允许表包含其自身的行类型。
		 */
		if (list_member_oid(containing_rowtypes, atttypid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("composite type %s cannot be made a member of itself",
							format_type_be(atttypid))));

		containing_rowtypes = lappend_oid(containing_rowtypes, atttypid);

		relation = relation_open(get_typ_typrelid(atttypid), AccessShareLock);

		tupdesc = RelationGetDescr(relation);

		for (i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				continue;
			CheckAttributeType(NameStr(attr->attname),
							   attr->atttypid, attr->attcollation,
							   containing_rowtypes,
							   flags & ~CHKATYPE_IS_PARTKEY);
		}

		relation_close(relation, AccessShareLock);

		containing_rowtypes = list_delete_last(containing_rowtypes);
	}
	else if (att_typtype == TYPTYPE_RANGE)
	{
		/*
		 * 若是范围，递归检查其子类型。
		 */
		CheckAttributeType(attname, get_range_subtype(atttypid),
						   get_range_collation(atttypid),
						   containing_rowtypes,
						   flags);
	}
	else if (OidIsValid((att_typelem = get_element_type(atttypid))))
	{
		/*
		 * 也必须递归进入数组类型，以防它们是复合的。
		 */
		CheckAttributeType(attname, att_typelem, attcollation,
						   containing_rowtypes,
						   flags);
	}

	/*
	 * 这按 SQL 标准未必严格无效，但相当
	 * 无用，而且无法转储，因此必须禁止。
	 */
	if (!OidIsValid(attcollation) && type_is_collatable(atttypid))
	{
		if (flags & CHKATYPE_IS_PARTKEY)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
			/* translator: first %s is an integer not a name */
					 errmsg("no collation was derived for partition key column %s with collatable type %s",
							attname, format_type_be(atttypid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("no collation was derived for column \"%s\" with collatable type %s",
							attname, format_type_be(atttypid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

/*
 * InsertPgAttributeTuples
 *		在 pg_attribute 中构造并插入一组元组。
 *
 * 调用方已打开并锁住 pg_attribute。tupdesc 含有要
 * 插入的属性。attcacheoff 总是初始化为 -1。attoptions
 * 提供 attoptions 字段的值，元素个数必须与
 * tupdesc 相同，或为 NULL。pg_attribute 的其他变长
 * 字段总是初始化为 null。
 *
 * indstate 是 CatalogTupleInsertWithInfo 的索引状态。可以
 * 传 NULL，此时我们会获取必要信息。（插入
 * 多个属性时不要这样做，因为会稍微更
 * 昂贵。）
 *
 * new_rel_oid 是赋给所插入属性的关系 OID。
 * 若设为 InvalidOid，则改用 tupdesc 中的关系 OID。
 */
void
InsertPgAttributeTuples(Relation pg_attribute_rel,
						TupleDesc tupdesc,
						Oid new_rel_oid,
						Datum *attoptions,
						CatalogIndexState indstate)
{
	TupleTableSlot **slot;
	TupleDesc	td;
	int			nslots;
	int			natts = 0;
	int			slotCount = 0;
	bool		close_index = false;

	td = RelationGetDescr(pg_attribute_rel);

	/* 初始化要使用的槽位数 */
	nslots = Min(tupdesc->natts,
				 (MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_attribute)));
	slot = palloc(sizeof(TupleTableSlot *) * nslots);
	for (int i = 0; i < nslots; i++)
		slot[i] = MakeSingleTupleTableSlot(td, &TTSOpsHeapTuple);

	while (natts < tupdesc->natts)
	{
		Form_pg_attribute attrs = TupleDescAttr(tupdesc, natts);

		ExecClearTuple(slot[slotCount]);

		memset(slot[slotCount]->tts_isnull, false,
			   slot[slotCount]->tts_tupleDescriptor->natts * sizeof(bool));

		if (new_rel_oid != InvalidOid)
			slot[slotCount]->tts_values[Anum_pg_attribute_attrelid - 1] = ObjectIdGetDatum(new_rel_oid);
		else
			slot[slotCount]->tts_values[Anum_pg_attribute_attrelid - 1] = ObjectIdGetDatum(attrs->attrelid);

		slot[slotCount]->tts_values[Anum_pg_attribute_attname - 1] = NameGetDatum(&attrs->attname);
		slot[slotCount]->tts_values[Anum_pg_attribute_atttypid - 1] = ObjectIdGetDatum(attrs->atttypid);
		slot[slotCount]->tts_values[Anum_pg_attribute_attlen - 1] = Int16GetDatum(attrs->attlen);
		slot[slotCount]->tts_values[Anum_pg_attribute_attnum - 1] = Int16GetDatum(attrs->attnum);
		slot[slotCount]->tts_values[Anum_pg_attribute_attcacheoff - 1] = Int32GetDatum(-1);
		slot[slotCount]->tts_values[Anum_pg_attribute_atttypmod - 1] = Int32GetDatum(attrs->atttypmod);
		slot[slotCount]->tts_values[Anum_pg_attribute_attndims - 1] = Int16GetDatum(attrs->attndims);
		slot[slotCount]->tts_values[Anum_pg_attribute_attbyval - 1] = BoolGetDatum(attrs->attbyval);
		slot[slotCount]->tts_values[Anum_pg_attribute_attalign - 1] = CharGetDatum(attrs->attalign);
		slot[slotCount]->tts_values[Anum_pg_attribute_attstorage - 1] = CharGetDatum(attrs->attstorage);
		slot[slotCount]->tts_values[Anum_pg_attribute_attcompression - 1] = CharGetDatum(attrs->attcompression);
		slot[slotCount]->tts_values[Anum_pg_attribute_attnotnull - 1] = BoolGetDatum(attrs->attnotnull);
		slot[slotCount]->tts_values[Anum_pg_attribute_atthasdef - 1] = BoolGetDatum(attrs->atthasdef);
		slot[slotCount]->tts_values[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(attrs->atthasmissing);
		slot[slotCount]->tts_values[Anum_pg_attribute_attidentity - 1] = CharGetDatum(attrs->attidentity);
		slot[slotCount]->tts_values[Anum_pg_attribute_attgenerated - 1] = CharGetDatum(attrs->attgenerated);
		slot[slotCount]->tts_values[Anum_pg_attribute_attisdropped - 1] = BoolGetDatum(attrs->attisdropped);
		slot[slotCount]->tts_values[Anum_pg_attribute_attislocal - 1] = BoolGetDatum(attrs->attislocal);
		slot[slotCount]->tts_values[Anum_pg_attribute_attinhcount - 1] = Int16GetDatum(attrs->attinhcount);
		slot[slotCount]->tts_values[Anum_pg_attribute_attstattarget - 1] = Int16GetDatum(attrs->attstattarget);
		slot[slotCount]->tts_values[Anum_pg_attribute_attcollation - 1] = ObjectIdGetDatum(attrs->attcollation);
		if (attoptions && attoptions[natts] != (Datum) 0)
			slot[slotCount]->tts_values[Anum_pg_attribute_attoptions - 1] = attoptions[natts];
		else
			slot[slotCount]->tts_isnull[Anum_pg_attribute_attoptions - 1] = true;

		/* 以空权限和空选项开始 */
		slot[slotCount]->tts_isnull[Anum_pg_attribute_attacl - 1] = true;
		slot[slotCount]->tts_isnull[Anum_pg_attribute_attfdwoptions - 1] = true;
		slot[slotCount]->tts_isnull[Anum_pg_attribute_attmissingval - 1] = true;

		ExecStoreVirtualTuple(slot[slotCount]);
		slotCount++;

		/*
		 * 若槽已满或已处理到末尾，则插入
		 * 一批元组。
		 */
		if (slotCount == nslots || natts == tupdesc->natts - 1)
		{
			/* 仅在确知需要时才获取索引信息 */
			if (!indstate)
			{
				indstate = CatalogOpenIndexes(pg_attribute_rel);
				close_index = true;
			}

			/* 插入新元组并更新索引 */
			CatalogTuplesMultiInsertWithInfo(pg_attribute_rel, slot, slotCount,
											 indstate);
			slotCount = 0;
		}

		natts++;
	}

	if (close_index)
		CatalogCloseIndexes(indstate);
	for (int i = 0; i < nslots; i++)
		ExecDropSingleTupleTableSlot(slot[i]);
	pfree(slot);
}

/* --------------------------------
 *		AddNewAttributeTuples
 *
 *		通过向 pg_attribute 添加元组来登记
 *		新关系的模式。
 * --------------------------------
 */
static void
AddNewAttributeTuples(Oid new_rel_oid,
					  TupleDesc tupdesc,
					  char relkind)
{
	Relation	rel;
	CatalogIndexState indstate;
	int			natts = tupdesc->natts;
	ObjectAddress myself,
				referenced;

	/*
	 * 打开 pg_attribute 及其索引。
	 */
	rel = table_open(AttributeRelationId, RowExclusiveLock);

	indstate = CatalogOpenIndexes(rel);

	/* 将统计详情级别设为合理默认值 */
	for (int i = 0; i < natts; i++)
		tupdesc->attrs[i].attstattarget = -1;
	InsertPgAttributeTuples(rel, tupdesc, new_rel_oid, NULL, indstate);

	/* 添加对其数据类型和排序规则的依赖 */
	for (int i = 0; i < natts; i++)
	{
		/* 添加依赖信息 */
		ObjectAddressSubSet(myself, RelationRelationId, new_rel_oid, i + 1);
		ObjectAddressSet(referenced, TypeRelationId,
						 tupdesc->attrs[i].atttypid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		/* 默认排序规则已被钉住，不必记录 */
		if (OidIsValid(tupdesc->attrs[i].attcollation) &&
			tupdesc->attrs[i].attcollation != DEFAULT_COLLATION_OID)
		{
			ObjectAddressSet(referenced, CollationRelationId,
							 tupdesc->attrs[i].attcollation);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * 接下来添加系统属性。若 rel 没有 OIDs 则跳过 OID。对
	 * 视图或类型关系全部跳过。这里不费心建立
	 * 数据类型依赖，因为这些类型想来都被钉住了。
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE)
	{
		TupleDesc	td;

		td = CreateTupleDesc(lengthof(SysAtt), (FormData_pg_attribute **) &SysAtt);

		InsertPgAttributeTuples(rel, td, new_rel_oid, NULL, indstate);
		FreeTupleDesc(td);
	}

	/*
	 * 收尾
	 */
	CatalogCloseIndexes(indstate);

	table_close(rel, RowExclusiveLock);
}

/* --------------------------------
 *		InsertPgClassTuple
 *
 *		在 pg_class 中构造并插入一个新元组。
 *
 * 调用方已打开并锁住 pg_class。
 * 元组数据取自 new_rel_desc->rd_rel，缓存的 reldesc 中
 * 不存在的变宽字段除外。
 * relacl 和 reloptions 以 Datum 形式传入（以免在
 * heap.h 中引用这些数据类型）。传 (Datum) 0 可将它们
 * 设为 NULL。
 * --------------------------------
 */
void
InsertPgClassTuple(Relation pg_class_desc,
				   Relation new_rel_desc,
				   Oid new_rel_oid,
				   Datum relacl,
				   Datum reloptions)
{
	Form_pg_class rd_rel = new_rel_desc->rd_rel;
	Datum		values[Natts_pg_class];
	bool		nulls[Natts_pg_class];
	HeapTuple	tup;

	/* 这稍显繁琐，但比我们以前的做法干净得多…… */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_class_oid - 1] = ObjectIdGetDatum(new_rel_oid);
	values[Anum_pg_class_relname - 1] = NameGetDatum(&rd_rel->relname);
	values[Anum_pg_class_relnamespace - 1] = ObjectIdGetDatum(rd_rel->relnamespace);
	values[Anum_pg_class_reltype - 1] = ObjectIdGetDatum(rd_rel->reltype);
	values[Anum_pg_class_reloftype - 1] = ObjectIdGetDatum(rd_rel->reloftype);
	values[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(rd_rel->relowner);
	values[Anum_pg_class_relam - 1] = ObjectIdGetDatum(rd_rel->relam);
	values[Anum_pg_class_relfilenode - 1] = ObjectIdGetDatum(rd_rel->relfilenode);
	values[Anum_pg_class_reltablespace - 1] = ObjectIdGetDatum(rd_rel->reltablespace);
	values[Anum_pg_class_relpages - 1] = Int32GetDatum(rd_rel->relpages);
	values[Anum_pg_class_reltuples - 1] = Float4GetDatum(rd_rel->reltuples);
	values[Anum_pg_class_relallvisible - 1] = Int32GetDatum(rd_rel->relallvisible);
	values[Anum_pg_class_reltoastrelid - 1] = ObjectIdGetDatum(rd_rel->reltoastrelid);
	values[Anum_pg_class_relhasindex - 1] = BoolGetDatum(rd_rel->relhasindex);
	values[Anum_pg_class_relisshared - 1] = BoolGetDatum(rd_rel->relisshared);
	values[Anum_pg_class_relpersistence - 1] = CharGetDatum(rd_rel->relpersistence);
	values[Anum_pg_class_relkind - 1] = CharGetDatum(rd_rel->relkind);
	values[Anum_pg_class_relnatts - 1] = Int16GetDatum(rd_rel->relnatts);
	values[Anum_pg_class_relchecks - 1] = Int16GetDatum(rd_rel->relchecks);
	values[Anum_pg_class_relhasrules - 1] = BoolGetDatum(rd_rel->relhasrules);
	values[Anum_pg_class_relhastriggers - 1] = BoolGetDatum(rd_rel->relhastriggers);
	values[Anum_pg_class_relrowsecurity - 1] = BoolGetDatum(rd_rel->relrowsecurity);
	values[Anum_pg_class_relforcerowsecurity - 1] = BoolGetDatum(rd_rel->relforcerowsecurity);
	values[Anum_pg_class_relhassubclass - 1] = BoolGetDatum(rd_rel->relhassubclass);
	values[Anum_pg_class_relispopulated - 1] = BoolGetDatum(rd_rel->relispopulated);
	values[Anum_pg_class_relreplident - 1] = CharGetDatum(rd_rel->relreplident);
	values[Anum_pg_class_relispartition - 1] = BoolGetDatum(rd_rel->relispartition);
	values[Anum_pg_class_relrewrite - 1] = ObjectIdGetDatum(rd_rel->relrewrite);
	values[Anum_pg_class_relfrozenxid - 1] = TransactionIdGetDatum(rd_rel->relfrozenxid);
	values[Anum_pg_class_relminmxid - 1] = MultiXactIdGetDatum(rd_rel->relminmxid);
	if (relacl != (Datum) 0)
		values[Anum_pg_class_relacl - 1] = relacl;
	else
		nulls[Anum_pg_class_relacl - 1] = true;
	if (reloptions != (Datum) 0)
		values[Anum_pg_class_reloptions - 1] = reloptions;
	else
		nulls[Anum_pg_class_reloptions - 1] = true;

	/* relpartbound 如有必要通过更新本元组来设置 */
	nulls[Anum_pg_class_relpartbound - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(pg_class_desc), values, nulls);

	/* 最后插入新元组、更新索引，并收尾 */
	CatalogTupleInsert(pg_class_desc, tup);

	heap_freetuple(tup);
}

/* --------------------------------
 *		AddNewRelationTuple
 *
 *		通过向 pg_class 添加元组，在目录中
 *		登记新关系。
 * --------------------------------
 */
static void
AddNewRelationTuple(Relation pg_class_desc,
					Relation new_rel_desc,
					Oid new_rel_oid,
					Oid new_type_oid,
					Oid reloftype,
					Oid relowner,
					char relkind,
					TransactionId relfrozenxid,
					TransactionId relminmxid,
					Datum relacl,
					Datum reloptions)
{
	Form_pg_class new_rel_reltup;

	/*
	 * 首先更新我们未编目关系的关系描述符中的
	 * 部分信息。
	 */
	new_rel_reltup = new_rel_desc->rd_rel;

	/* 该关系为空 */
	new_rel_reltup->relpages = 0;
	new_rel_reltup->reltuples = -1;
	new_rel_reltup->relallvisible = 0;

	/* 序列总是有已知大小 */
	if (relkind == RELKIND_SEQUENCE)
	{
		new_rel_reltup->relpages = 1;
		new_rel_reltup->reltuples = 1;
	}

	new_rel_reltup->relfrozenxid = relfrozenxid;
	new_rel_reltup->relminmxid = relminmxid;
	new_rel_reltup->relowner = relowner;
	new_rel_reltup->reltype = new_type_oid;
	new_rel_reltup->reloftype = reloftype;

	/* relispartition 总是稍后通过更新本元组来设置 */
	new_rel_reltup->relispartition = false;

	/* 即便 reltype 为零，也为 rd_att 填入合理的类型 ID */
	new_rel_desc->rd_att->tdtypeid = new_type_oid ? new_type_oid : RECORDOID;
	new_rel_desc->rd_att->tdtypmod = -1;

	/* 现在构造并插入元组 */
	InsertPgClassTuple(pg_class_desc, new_rel_desc, new_rel_oid,
					   relacl, reloptions);
}


/* --------------------------------
 *		AddNewRelationType -
 *
 *		定义与新关系对应的复合类型
 * --------------------------------
 */
static ObjectAddress
AddNewRelationType(const char *typeName,
				   Oid typeNamespace,
				   Oid new_rel_oid,
				   char new_rel_kind,
				   Oid ownerid,
				   Oid new_row_type,
				   Oid new_array_type)
{
	return
		TypeCreate(new_row_type,	/* 可选的预先指定 OID */
				   typeName,	/* 类型名 */
				   typeNamespace,	/* 类型命名空间 */
				   new_rel_oid, /* 关系 oid */
				   new_rel_kind,	/* 关系种类 */
				   ownerid,		/* 所有者 ID */
				   -1,			/* 内部大小（varlena） */
				   TYPTYPE_COMPOSITE,	/* 类型类别（复合） */
				   TYPCATEGORY_COMPOSITE,	/* 类型分类（同上） */
				   false,		/* 复合类型从不是首选类型 */
				   DEFAULT_TYPDELIM,	/* 默认数组分隔符 */
				   F_RECORD_IN, /* 输入过程 */
				   F_RECORD_OUT,	/* 输出过程 */
				   F_RECORD_RECV,	/* 接收过程 */
				   F_RECORD_SEND,	/* 发送过程 */
				   InvalidOid,	/* typmodin 过程 — 无 */
				   InvalidOid,	/* typmodout 过程 — 无 */
				   InvalidOid,	/* analyze 过程 — 默认 */
				   InvalidOid,	/* 下标过程 — 无 */
				   InvalidOid,	/* 数组元素类型 — 无关 */
				   false,		/* 这不是数组类型 */
				   new_array_type,	/* 数组类型（若有） */
				   InvalidOid,	/* 域基类型 — 无关 */
				   NULL,		/* 默认值 — 无 */
				   NULL,		/* 默认二进制表示 */
				   false,		/* 按引用传递 */
				   TYPALIGN_DOUBLE, /* 对齐 — 必须是最大的！ */
				   TYPSTORAGE_EXTENDED, /* 完全可 TOASTable */
				   -1,			/* typmod */
				   0,			/* typBaseType 的数组维数 */
				   false,		/* 类型 NOT NULL */
				   InvalidOid); /* 行类型从没有排序规则 */
}

/* --------------------------------
 *		heap_create_with_catalog
 *
 *		创建新的已编目关系。参见上面的注释。
 *
 * 参数：
 *	relname: 赋予新关系的名称
 *	relnamespace: 它所在命名空间的 OID
 *	reltablespace: 它所在表空间的 OID
 *	relid: 赋给新关系的 OID，或 InvalidOid 以选择新 OID
 *	reltypeid: 赋给关系行类型的 OID，或 InvalidOid 以选择一个
 *	reloftypeid: 若是类型表，为底层类型的 OID；否则 InvalidOid
 *	ownerid: 新关系所有者的 OID
 *	accessmtd: 新关系访问方法的 OID
 *	tupdesc: 元组描述符（列定义的来源）
 *	cooked_constraints: 预先烹调的 CHECK 约束和默认值列表
 *	relkind: 新关系的 relkind
 *	relpersistence: 关系的持久性状态（永久、临时或未日志）
 *	shared_relation: 若要成为共享关系则为 true
 *	mapped_relation: 若关系将使用 relfilenumber 映射则为 true
 *	oncommit: ON COMMIT 标记（仅当它是临时表时才有意义）
 *	reloptions: Datum 形式的 reloptions，若无则为 (Datum) 0
 *	use_user_acl: 若应查找用户定义的默认权限则为 true；
 *		若为 false，relacl 总是设为 NULL
 *	allow_system_table_mods: true 表示允许在系统命名空间中创建
 *	is_internal: 这是否是系统生成的目录？
 *
 * 输出参数：
 *	typaddress: 若非空，取得新 pg_type 条目的对象地址
 *	（若 relkind 是不会得到 pg_type 条目的那种，则必须为 null）
 *
 * 返回新关系的 OID
 * --------------------------------
 */
Oid
heap_create_with_catalog(const char *relname,
						 Oid relnamespace,
						 Oid reltablespace,
						 Oid relid,
						 Oid reltypeid,
						 Oid reloftypeid,
						 Oid ownerid,
						 Oid accessmtd,
						 TupleDesc tupdesc,
						 List *cooked_constraints,
						 char relkind,
						 char relpersistence,
						 bool shared_relation,
						 bool mapped_relation,
						 OnCommitAction oncommit,
						 Datum reloptions,
						 bool use_user_acl,
						 bool allow_system_table_mods,
						 bool is_internal,
						 Oid relrewrite,
						 ObjectAddress *typaddress)
{
	Relation	pg_class_desc;
	Relation	new_rel_desc;
	Acl		   *relacl;
	Oid			existing_relid;
	Oid			old_type_oid;
	Oid			new_type_oid;

	/* 默认设为 InvalidOid，除非被 binary-upgrade 覆盖 */
	RelFileNumber relfilenumber = InvalidRelFileNumber;
	TransactionId relfrozenxid;
	MultiXactId relminmxid;

	pg_class_desc = table_open(RelationRelationId, RowExclusiveLock);

	/*
	 * 合理性检查
	 */
	Assert(IsNormalProcessingMode() || IsBootstrapProcessingMode());

	/*
	 * 按所需 relkind 验证提议的 tupdesc。若
	 * allow_system_table_mods 打开，允许使用 ANYARRAY；这是为了
	 * 允许创建 pg_statistic 并在 VACUUM FULL 期间克隆它的 hack。
	 */
	CheckAttributeNamesTypes(tupdesc, relkind,
							 allow_system_table_mods ? CHKATYPE_ANYARRAY : 0);

	/*
	 * 若关系已存在，稍后反正也会失败。但
	 * 在这里捕获可以发出更友好的错误消息。
	 */
	existing_relid = get_relname_relid(relname, relnamespace);
	if (existing_relid != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists", relname)));

	/*
	 * 既然还要创建行类型，也要检查是否与
	 * 已有类型名冲突。若有一个且它是
	 * 自动生成的数组，我们可以把它改名让开；否则至少
	 * 能给出好的错误消息。
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(relname),
								   ObjectIdGetDatum(relnamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, relname, relnamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", relname),
					 errhint("A relation has an associated type of the same name, "
							 "so you must use a name that doesn't conflict "
							 "with any existing type.")));
	}

	/*
	 * 共享关系必须位于 pg_global（最后一道检查）
	 */
	if (shared_relation && reltablespace != GLOBALTABLESPACE_OID)
		elog(ERROR, "shared relations must be placed in pg_global tablespace");

	/*
	 * 为关系分配 OID，除非已被告知使用哪个。
	 *
	 * 该 OID 也将是 relfilenumber，因此确保它不与
	 * pg_class OIDs 或已有物理文件冲突。
	 */
	if (!OidIsValid(relid))
	{
		/* 对 pg_class.oid 和 relfilenumber 使用 binary-upgrade 覆盖值 */
		if (IsBinaryUpgrade)
		{
			/*
			 * 此处不支持索引；它们使用
			 * binary_upgrade_next_index_pg_class_oid。
			 */
			Assert(relkind != RELKIND_INDEX);
			Assert(relkind != RELKIND_PARTITIONED_INDEX);

			if (relkind == RELKIND_TOASTVALUE)
			{
				/* 可能没有 TOAST 表，因此必须测试。 */
				if (OidIsValid(binary_upgrade_next_toast_pg_class_oid))
				{
					relid = binary_upgrade_next_toast_pg_class_oid;
					binary_upgrade_next_toast_pg_class_oid = InvalidOid;

					if (!RelFileNumberIsValid(binary_upgrade_next_toast_pg_class_relfilenumber))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("toast relfilenumber value not set when in binary upgrade mode")));

					relfilenumber = binary_upgrade_next_toast_pg_class_relfilenumber;
					binary_upgrade_next_toast_pg_class_relfilenumber = InvalidRelFileNumber;
				}
			}
			else
			{
				if (!OidIsValid(binary_upgrade_next_heap_pg_class_oid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("pg_class heap OID value not set when in binary upgrade mode")));

				relid = binary_upgrade_next_heap_pg_class_oid;
				binary_upgrade_next_heap_pg_class_oid = InvalidOid;

				if (RELKIND_HAS_STORAGE(relkind))
				{
					if (!RelFileNumberIsValid(binary_upgrade_next_heap_pg_class_relfilenumber))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("relfilenumber value not set when in binary upgrade mode")));

					relfilenumber = binary_upgrade_next_heap_pg_class_relfilenumber;
					binary_upgrade_next_heap_pg_class_relfilenumber = InvalidRelFileNumber;
				}
			}
		}

		if (!OidIsValid(relid))
			relid = GetNewRelFileNumber(reltablespace, pg_class_desc,
										relpersistence);
	}

	/*
	 * 其他会话的目录扫描在我们提交前找不到它。因此，
	 * 持有 AccessExclusiveLock 无妨。在此获取，以免调用方
	 * 无意中在锁模式或获取时机上不一致。
	 */
	LockRelationOid(relid, AccessExclusiveLock);

	/*
	 * 确定关系的初始权限。
	 */
	if (use_user_acl)
	{
		switch (relkind)
		{
			case RELKIND_RELATION:
			case RELKIND_VIEW:
			case RELKIND_MATVIEW:
			case RELKIND_FOREIGN_TABLE:
			case RELKIND_PARTITIONED_TABLE:
				relacl = get_user_default_acl(OBJECT_TABLE, ownerid,
											  relnamespace);
				break;
			case RELKIND_SEQUENCE:
				relacl = get_user_default_acl(OBJECT_SEQUENCE, ownerid,
											  relnamespace);
				break;
			default:
				relacl = NULL;
				break;
		}
	}
	else
		relacl = NULL;

	/*
	 * 创建关系缓存条目（此刻大多是哑的）以及物理
	 * 磁盘文件。（若我们在更下面失败，由 smgr 负责
	 * 再次移除磁盘文件。）
	 *
	 * 注意：即便对 binary upgrade，传入 create_storage = true 也是正确的。
	 * 这里创建的存储稍后会被替换，但我们
	 * 需要其间磁盘上有点东西。
	 */
	new_rel_desc = heap_create(relname,
							   relnamespace,
							   reltablespace,
							   relid,
							   relfilenumber,
							   accessmtd,
							   tupdesc,
							   relkind,
							   relpersistence,
							   shared_relation,
							   mapped_relation,
							   allow_system_table_mods,
							   &relfrozenxid,
							   &relminmxid,
							   true);

	Assert(relid == RelationGetRelid(new_rel_desc));

	new_rel_desc->rd_rel->relrewrite = relrewrite;

	/*
	 * 决定是否为关系的行类型创建 pg_type 条目。
	 * 除了把使用关系本身当作实现细节的情况外都
	 * 会制作这些类型：TOAST 表、序列和索引。
	 */
	if (!(relkind == RELKIND_SEQUENCE ||
		  relkind == RELKIND_TOASTVALUE ||
		  relkind == RELKIND_INDEX ||
		  relkind == RELKIND_PARTITIONED_INDEX))
	{
		Oid			new_array_oid;
		ObjectAddress new_type_addr;
		char	   *relarrayname;

		/*
		 * 我们也会在复合类型之上做数组。很大程度上出于
		 * 历史原因，数组类型的 OID 先分配。
		 */
		new_array_oid = AssignTypeArrayOid();

		/*
		 * 为复合类型制作 pg_type 条目。复合类型的
		 * OID 可由调用方预先选定，但若 reltypeid
		 * 为 InvalidOid，我们将为它生成新 OID。
		 *
		 * 注意：此处可能遇到唯一索引失败，如果有人
		 * 在并行创建同一类型名，但上面我们检查重复名时
		 * 他们尚未提交。
		 */
		new_type_addr = AddNewRelationType(relname,
										   relnamespace,
										   relid,
										   relkind,
										   ownerid,
										   reltypeid,
										   new_array_oid);
		new_type_oid = new_type_addr.objectId;
		if (typaddress)
			*typaddress = new_type_addr;

		/* 现在创建数组类型。 */
		relarrayname = makeArrayTypeName(relname, relnamespace);

		TypeCreate(new_array_oid,	/* 强制类型 OID 为此值 */
				   relarrayname,	/* 数组类型名 */
				   relnamespace,	/* 与父对象同一命名空间 */
				   InvalidOid,	/* 非复合，无 relationOid */
				   0,			/* relkind，此处同样不适用 */
				   ownerid,		/* 所有者 ID */
				   -1,			/* 内部大小（varlena） */
				   TYPTYPE_BASE,	/* 非复合 — typelem 为 */
				   TYPCATEGORY_ARRAY,	/* 类型分类（数组） */
				   false,		/* 数组类型从不是首选类型 */
				   DEFAULT_TYPDELIM,	/* 默认数组分隔符 */
				   F_ARRAY_IN,	/* 数组输入过程 */
				   F_ARRAY_OUT, /* 数组输出过程 */
				   F_ARRAY_RECV,	/* 数组 recv（二进制）过程 */
				   F_ARRAY_SEND,	/* 数组 send（二进制）过程 */
				   InvalidOid,	/* typmodin 过程 — 无 */
				   InvalidOid,	/* typmodout 过程 — 无 */
				   F_ARRAY_TYPANALYZE,	/* 数组 analyze 过程 */
				   F_ARRAY_SUBSCRIPT_HANDLER,	/* 数组下标过程 */
				   new_type_oid,	/* 数组元素类型 — 该行类型 */
				   true,		/* 是的，这是数组类型 */
				   InvalidOid,	/* 它没有数组类型 */
				   InvalidOid,	/* 域基类型 — 无关 */
				   NULL,		/* 默认值 — 无 */
				   NULL,		/* 默认二进制表示 */
				   false,		/* 按引用传递 */
				   TYPALIGN_DOUBLE, /* 对齐 — 必须是最大的！ */
				   TYPSTORAGE_EXTENDED, /* 完全可 TOASTable */
				   -1,			/* typmod */
				   0,			/* typBaseType 的数组维数 */
				   false,		/* 类型 NOT NULL */
				   InvalidOid); /* 行类型从没有排序规则 */

		pfree(relarrayname);
	}
	else
	{
		/* 调用方不应期望会创建类型。 */
		Assert(reltypeid == InvalidOid);
		Assert(typaddress == NULL);

		new_type_oid = InvalidOid;
	}

	/*
	 * 现在为该关系在 pg_class 中创建条目。
	 *
	 * 注意：此处可能遇到唯一索引失败，如果有人在
	 * 并行创建同一关系名，但上面我们检查重复名时
	 * 他们尚未提交。
	 */
	AddNewRelationTuple(pg_class_desc,
						new_rel_desc,
						relid,
						new_type_oid,
						reloftypeid,
						ownerid,
						relkind,
						relfrozenxid,
						relminmxid,
						PointerGetDatum(relacl),
						reloptions);

	/*
	 * 现在为新关系中的属性向 pg_attribute 添加元组。
	 */
	AddNewAttributeTuples(relid, new_rel_desc->rd_att, relkind);

	/*
	 * 建立依赖链接，以便其命名空间被删除时强制
	 * 删除该关系。也为所有者建立依赖链接，以及
	 * 默认 ACL 中提及的任何角色的依赖。
	 *
	 * 对复合类型，这些依赖跟踪在 pg_type
	 * 条目上，因此这里不必记录。同样，TOAST 表不
	 * 需要命名空间依赖（它们活在被钉住的命名空间中），也不需要
	 * 所有者依赖（它们通过父表间接依赖），也不
	 * 应有任何 ACL 条目。扩展依赖同样
	 * 适用。
	 *
	 * 另外，引导模式跳过此项，因为引导期间
	 * 我们不建立依赖。
	 */
	if (relkind != RELKIND_COMPOSITE_TYPE &&
		relkind != RELKIND_TOASTVALUE &&
		!IsBootstrapProcessingMode())
	{
		ObjectAddress myself,
					referenced;
		ObjectAddresses *addrs;

		ObjectAddressSet(myself, RelationRelationId, relid);

		recordDependencyOnOwner(RelationRelationId, relid, ownerid);

		recordDependencyOnNewAcl(RelationRelationId, relid, 0, ownerid, relacl);

		recordDependencyOnCurrentExtension(&myself, false);

		addrs = new_object_addresses();

		ObjectAddressSet(referenced, NamespaceRelationId, relnamespace);
		add_exact_object_address(&referenced, addrs);

		if (reloftypeid)
		{
			ObjectAddressSet(referenced, TypeRelationId, reloftypeid);
			add_exact_object_address(&referenced, addrs);
		}

		/*
		 * 建立依赖链接，以便其访问方法被删除时强制
		 * 删除该关系。
		 *
		 * 不必为 TOAST 表添加显式依赖，因为
		 * 主表依赖它。
		 */
		if (RELKIND_HAS_TABLE_AM(relkind) && relkind != RELKIND_TOASTVALUE)
		{
			ObjectAddressSet(referenced, AccessMethodRelationId, accessmtd);
			add_exact_object_address(&referenced, addrs);
		}

		record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
		free_object_addresses(addrs);
	}

	/* 新关系的创建后钩子 */
	InvokeObjectPostCreateHookArg(RelationRelationId, relid, 0, is_internal);

	/*
	 * 存储任何提供的约束和默认值。
	 *
	 * 注意：这可能做 CommandCounterIncrement 并重建关系缓存
	 * 条目，因此此时关系必须有效且自洽。
	 * 特别是，任何地方都还没有约束和默认值。
	 */
	StoreConstraints(new_rel_desc, cooked_constraints, is_internal);

	/*
	 * 若有特殊的 on-commit 动作，记住它
	 */
	if (oncommit != ONCOMMIT_NOOP)
		register_on_commit_action(relid, oncommit);

	/*
	 * 好，关系已编目，因此关闭我们的关系并返回
	 * 新创建关系的 OID。
	 */
	table_close(new_rel_desc, NoLock);	/* 直到事务结束才解锁 */
	table_close(pg_class_desc, RowExclusiveLock);

	return relid;
}

/*
 *		RelationRemoveInheritance
 *
 * 从前，本例程检查子关系，若找到任何则
 * 中止删除。现在我们依赖依赖机制
 * 来检查或删除子关系。等到我们到这里时，
 * 已没有子女，只需移除把本关系链接到
 * 其 parent(s) 的任何 pg_inherits 行。
 */
static void
RelationRemoveInheritance(Oid relid)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);

	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(catalogRelation, &tuple->t_self);

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);
}

/*
 *		DeleteRelationTuple
 *
 * 移除给定 relid 的 pg_class 行。
 *
 * 注意：关系删除和索引删除共享此例程。不
 * 打算在任何其他地方使用。
 */
void
DeleteRelationTuple(Oid relid)
{
	Relation	pg_class_desc;
	HeapTuple	tup;

	/* 在 pg_class 关系上获取适当的锁 */
	pg_class_desc = table_open(RelationRelationId, RowExclusiveLock);

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	/* 从 pg_class 删除该关系元组，并收尾 */
	CatalogTupleDelete(pg_class_desc, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(pg_class_desc, RowExclusiveLock);
}

/*
 *		DeleteAttributeTuples
 *
 * 移除给定 relid 的 pg_attribute 行。
 *
 * 注意：关系删除和索引删除共享此例程。不
 * 打算在任何其他地方使用。
 */
void
DeleteAttributeTuples(Oid relid)
{
	Relation	attrel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	atttup;

	/* 在 pg_attribute 关系上获取适当的锁 */
	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* 用索引只扫描目标关系的属性 */
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(attrel, AttributeRelidNumIndexId, true,
							  NULL, 1, key);

	/* 删除所有匹配的元组 */
	while ((atttup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(attrel, &atttup->t_self);

	/* 扫描结束后收尾 */
	systable_endscan(scan);
	table_close(attrel, RowExclusiveLock);
}

/*
 *		DeleteSystemAttributeTuples
 *
 * 移除给定 relid 的系统列的 pg_attribute 行。
 *
 * 注意：这仅在把表转换成视图时使用。视图没有
 * 系统列，因此应从 pg_attribute 中移除它们。
 */
void
DeleteSystemAttributeTuples(Oid relid)
{
	Relation	attrel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	atttup;

	/* 在 pg_attribute 关系上获取适当的锁 */
	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* 用索引只扫描目标关系的系统属性 */
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_attribute_attnum,
				BTLessEqualStrategyNumber, F_INT2LE,
				Int16GetDatum(0));

	scan = systable_beginscan(attrel, AttributeRelidNumIndexId, true,
							  NULL, 2, key);

	/* 删除所有匹配的元组 */
	while ((atttup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(attrel, &atttup->t_self);

	/* 扫描结束后收尾 */
	systable_endscan(scan);
	table_close(attrel, RowExclusiveLock);
}

/*
 *		RemoveAttributeById
 *
 * 这是 ALTER TABLE DROP COLUMN 的核心：实际在 pg_attribute 中
 * 把该属性标记为已删除。我们也移除它的 pg_statistic 条目。
 * （其余所需事项，例如去掉任何 pg_attrdef 条目，
 * 由 dependency.c 处理。）
 */
void
RemoveAttributeById(Oid relid, AttrNumber attnum)
{
	Relation	rel;
	Relation	attr_rel;
	HeapTuple	tuple;
	Form_pg_attribute attStruct;
	char		newattname[NAMEDATALEN];

	/*
	 * 在目标表上获取排他锁，直到事务结束
	 * 我们都不会释放。（在直接删除本列的简单情况下，
	 * ATExecDropColumn 已经这样做了……但当从删除
	 * 其他对象级联而来时，我们可能没有任何锁。）
	 */
	rel = relation_open(relid, AccessExclusiveLock);

	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy2(ATTNUM,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tuple))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	attStruct = (Form_pg_attribute) GETSTRUCT(tuple);

	if (attnum < 0)
	{
		/* 系统属性（多半是 OID）……直接删除该行 */

		CatalogTupleDelete(attr_rel, &tuple->t_self);
	}
	else
	{
		/* 删除用户属性要困难得多 */

		/* 将属性标记为已删除 */
		attStruct->attisdropped = true;

		/*
		 * 将类型 OID 设为无效。已删除属性的类型链接
		 * 不可依赖（属性一旦删除，类型也可能
		 * 被删）。幸好我们不需要类型行 — 真正
		 * 必不可少的信息只是类型的 typlen 和 typalign，
		 * 它们保存在属性的 attlen 和 attalign 中。我们在此把
		 * atttypid 设为零，以便抓住错误地
		 * 期望它有效的代码。
		 */
		attStruct->atttypid = InvalidOid;

		/* 去掉该列可能具有的任何 NOT NULL 约束 */
		attStruct->attnotnull = false;

		/* 我们不再想为它保留统计信息 */
		attStruct->attstattarget = 0;

		/* 取消此标志，以免有人去查找生成表达式 */
		attStruct->attgenerated = '\0';

		/*
		 * 把列名改成不大可能冲突的名字
		 */
		snprintf(newattname, sizeof(newattname),
				 "........pg.dropped.%d........", attnum);
		namestrcpy(&(attStruct->attname), newattname);

		/* 清除缺失值（若有） */
		if (attStruct->atthasmissing)
		{
			Datum		valuesAtt[Natts_pg_attribute] = {0};
			bool		nullsAtt[Natts_pg_attribute] = {0};
			bool		replacesAtt[Natts_pg_attribute] = {0};

			/* 更新元组 — 设置 atthasmissing 和 attmissingval */
			valuesAtt[Anum_pg_attribute_atthasmissing - 1] =
				BoolGetDatum(false);
			replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
			valuesAtt[Anum_pg_attribute_attmissingval - 1] = (Datum) 0;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = true;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

			tuple = heap_modify_tuple(tuple, RelationGetDescr(attr_rel),
									  valuesAtt, nullsAtt, replacesAtt);
		}

		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);
	}

	/*
	 * 因为更新 pg_attribute 行会触发目标关系的
	 * 关系缓存刷新，我们不必再做别的来通知其他
	 * 后端这一变更。
	 */

	table_close(attr_rel, RowExclusiveLock);

	if (attnum > 0)
		RemoveStatistics(relid, attnum);

	relation_close(rel, NoLock);
}

/*
 * heap_drop_with_catalog	- 从目录中移除指定关系
 *
 * 注意本例程不负责删除通过依赖链接到
 * pg_class 条目的对象（例如索引和
 * 约束）。那些由 dependency.c 中的依赖追踪逻辑
 * 在控制到达这里之前删除。因此一般而言，本例程
 * 绝不应被直接调用；应通过 performDeletion()。
 */
void
heap_drop_with_catalog(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;
	Oid			parentOid = InvalidOid,
				defaultPartOid = InvalidOid;

	/*
	 * 要安全地删除分区，必须在其父表上获取排他锁，
	 * 因为另一后端可能正要在父
	 * 表上执行查询。若它依赖先前缓存的分区描述符，则可能
	 * 试图把刚删除的关系当作其分区访问。我们
	 * 因此必须取足够强的表锁，以阻止该表上的所有查询
	 * 继续，直到我们提交并发出
	 * 共享缓存失效通知，使它们更新其分区
	 * 描述符。
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	if (((Form_pg_class) GETSTRUCT(tuple))->relispartition)
	{
		/*
		 * 若分区正在被分离，我们必须锁住父表，
		 * 因为可能仍有查询的分区
		 * 描述符包含本分区。
		 */
		parentOid = get_partition_parent(relid, true);
		LockRelationOid(parentOid, AccessExclusiveLock);

		/*
		 * 若这不是默认分区，删除它会改变
		 * 默认分区的分区约束，因此必须锁住它。
		 */
		defaultPartOid = get_default_partition_oid(parentOid);
		if (OidIsValid(defaultPartOid) && relid != defaultPartOid)
			LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	ReleaseSysCache(tuple);

	/*
	 * 打开并锁住该关系。
	 */
	rel = relation_open(relid, AccessExclusiveLock);

	/*
	 * 不能再有*别人*碰这个关系，但我们
	 * 自己的会话中仍可能有打开的查询或游标，或挂起的
	 * 触发器事件。
	 */
	CheckTableNotInUse(rel, "DROP TABLE");

	/*
	 * 这实际上删除表中的所有行，并且可能在
	 * 可串行化事务中完成。那种情况下我们必须从每个
	 * 持有该表谓词锁的事务向本事务记录
	 * 读写冲突。
	 */
	CheckTableForSerializableConflictIn(rel);

	/*
	 * 先删除 pg_foreign_table 元组。
	 */
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		Relation	ftrel;
		HeapTuple	fttuple;

		ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

		fttuple = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(fttuple))
			elog(ERROR, "cache lookup failed for foreign table %u", relid);

		CatalogTupleDelete(ftrel, &fttuple->t_self);

		ReleaseSysCache(fttuple);
		table_close(ftrel, RowExclusiveLock);
	}

	/*
	 * 若是分区表，删除 pg_partitioned_table 元组。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		RemovePartitionKeyByRelId(relid);

	/*
	 * 若被删除的关系就是默认分区本身，
	 * 使 pg_partitioned_table 中的其条目失效。
	 */
	if (relid == defaultPartOid)
		update_default_partition_oid(parentOid, InvalidOid);

	/*
	 * 安排在提交时解除该关系物理文件的链接。
	 */
	if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		RelationDropStorage(rel);

	/* 确保事务提交时统计信息会被删除 */
	pgstat_drop_relation(rel);

	/*
	 * 关闭关系缓存条目，但*保留*关系上的
	 * AccessExclusiveLock 直到事务提交。这确保没有别人会试图
	 * 对这个注定删除的关系做任何事。
	 */
	relation_close(rel, NoLock);

	/*
	 * 移除任何关联的关系同步状态。
	 */
	RemoveSubscriptionRel(InvalidOid, relid);

	/*
	 * 忘掉该关系的任何 ON COMMIT 动作
	 */
	remove_on_commit_action(relid);

	/*
	 * 从关系缓存中刷掉该关系。我们想在开始
	 * 移除目录条目之前做这件事，以确保不会在中途
	 * 发生关系缓存条目重建。（这其实不该
	 * 要紧，因为这里不做 CommandCounterIncrement，但还是
	 * 求个稳。）
	 */
	RelationForgetRelation(relid);

	/*
	 * 移除继承信息
	 */
	RelationRemoveInheritance(relid);

	/*
	 * 删除统计信息
	 */
	RemoveStatistics(relid, 0);

	/*
	 * 删除属性元组
	 */
	DeleteAttributeTuples(relid);

	/*
	 * 删除关系元组
	 */
	DeleteRelationTuple(relid);

	if (OidIsValid(parentOid))
	{
		/*
		 * 若这不是默认分区，默认分区的分区约束
		 * 已改变，以包含先前由被删除分区覆盖的
		 * 那部分键空间。
		 */
		if (OidIsValid(defaultPartOid) && relid != defaultPartOid)
			CacheInvalidateRelcacheByRelid(defaultPartOid);

		/*
		 * 使父表的关系缓存失效，使该分区不再
		 * 包含在其分区描述符中。
		 */
		CacheInvalidateRelcacheByRelid(parentOid);
		/* 保留该锁 */
	}
}


/*
 * RelationClearMissing
 *
 * 对当前已设置的所有属性，将 atthasmissing 和 attmissingval
 * 设为 false/null。若表被重写（例如由 VACUUM FULL 或 CLUSTER）
 * 且我们知道不会剩下属性不齐全的行，
 * 就可以安全且有用地这样做。
 *
 * 调用方必须对该关系持有 AccessExclusive 锁。
 */
void
RelationClearMissing(Relation rel)
{
	Relation	attr_rel;
	Oid			relid = RelationGetRelid(rel);
	int			natts = RelationGetNumberOfAttributes(rel);
	int			attnum;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Form_pg_attribute attrtuple;
	HeapTuple	tuple,
				newtuple;

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	repl_val[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(false);
	repl_null[Anum_pg_attribute_attmissingval - 1] = true;

	repl_repl[Anum_pg_attribute_atthasmissing - 1] = true;
	repl_repl[Anum_pg_attribute_attmissingval - 1] = true;


	/* 在 pg_attribute 上获取锁 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	/* 处理每个非系统属性，包括任何已删除列 */
	for (attnum = 1; attnum <= natts; attnum++)
	{
		tuple = SearchSysCache2(ATTNUM,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));
		if (!HeapTupleIsValid(tuple))	/* 不应发生 */
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, relid);

		attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

		/* 忽略所有 atthasmissing 不为 true 的属性 */
		if (attrtuple->atthasmissing)
		{
			newtuple = heap_modify_tuple(tuple, RelationGetDescr(attr_rel),
										 repl_val, repl_null, repl_repl);

			CatalogTupleUpdate(attr_rel, &newtuple->t_self, newtuple);

			heap_freetuple(newtuple);
		}

		ReleaseSysCache(tuple);
	}

	/*
	 * 我们对 pg_attribute 行的更新会强制关系缓存重建，因此
	 * 这里不必再做别的。
	 */
	table_close(attr_rel, RowExclusiveLock);
}

/*
 * StoreAttrMissingVal
 *
 * 设置单个属性的缺失值。
 */
void
StoreAttrMissingVal(Relation rel, AttrNumber attnum, Datum missingval)
{
	Datum		valuesAtt[Natts_pg_attribute] = {0};
	bool		nullsAtt[Natts_pg_attribute] = {0};
	bool		replacesAtt[Natts_pg_attribute] = {0};
	Relation	attrrel;
	Form_pg_attribute attStruct;
	HeapTuple	atttup,
				newtup;

	/* 这仅支持普通表 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	/* 取出 pg_attribute 行 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);

	atttup = SearchSysCache2(ATTNUM,
							 ObjectIdGetDatum(RelationGetRelid(rel)),
							 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttup))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);

	/* 构造仅含该值的单元素数组 */
	missingval = PointerGetDatum(construct_array(&missingval,
												 1,
												 attStruct->atttypid,
												 attStruct->attlen,
												 attStruct->attbyval,
												 attStruct->attalign));

	/* 更新 pg_attribute 行 */
	valuesAtt[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(true);
	replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;

	valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
	replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

	newtup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
							   valuesAtt, nullsAtt, replacesAtt);
	CatalogTupleUpdate(attrrel, &newtup->t_self, newtup);

	/* 收尾 */
	ReleaseSysCache(atttup);
	table_close(attrrel, RowExclusiveLock);
}

/*
 * SetAttrMissing
 *
 * 设置单个属性的缺失值。这只应由
 * binary upgrade 使用。对拥有该属性的关系取
 * AccessExclusive 锁。
 */
void
SetAttrMissing(Oid relid, char *attname, char *value)
{
	Datum		valuesAtt[Natts_pg_attribute] = {0};
	bool		nullsAtt[Natts_pg_attribute] = {0};
	bool		replacesAtt[Natts_pg_attribute] = {0};
	Datum		missingval;
	Form_pg_attribute attStruct;
	Relation	attrrel,
				tablerel;
	HeapTuple	atttup,
				newtup;

	/* 锁住所属性所属的表 */
	tablerel = table_open(relid, AccessExclusiveLock);

	/* 除非是普通表，否则什么也不做 */
	if (tablerel->rd_rel->relkind != RELKIND_RELATION)
	{
		table_close(tablerel, AccessExclusiveLock);
		return;
	}

	/* 锁住属性行并取得数据 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup failed for attribute %s of relation %u",
			 attname, relid);
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);

	/* 从值字符串得到一个数组值 */
	missingval = OidFunctionCall3(F_ARRAY_IN,
								  CStringGetDatum(value),
								  ObjectIdGetDatum(attStruct->atttypid),
								  Int32GetDatum(attStruct->atttypmod));

	/* 更新元组 — 设置 atthasmissing 和 attmissingval */
	valuesAtt[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(true);
	replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
	valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
	replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

	newtup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
							   valuesAtt, nullsAtt, replacesAtt);
	CatalogTupleUpdate(attrrel, &newtup->t_self, newtup);

	/* 收尾 */
	ReleaseSysCache(atttup);
	table_close(attrrel, RowExclusiveLock);
	table_close(tablerel, AccessExclusiveLock);
}

/*
 * 为给定关系存储 CHECK 约束表达式。
 *
 * 调用方负责更新该关系 pg_class 条目中的
 * 约束计数。
 *
 * 返回新约束的 OID。
 */
static Oid
StoreRelCheck(Relation rel, const char *ccname, Node *expr,
			  bool is_validated, bool is_local, int inhcount,
			  bool is_no_inherit, bool is_internal)
{
	char	   *ccbin;
	List	   *varList;
	int			keycount;
	int16	   *attNos;
	Oid			constrOid;

	/*
	 * 将表达式展平为字符串形式以便存储。
	 */
	ccbin = nodeToString(expr);

	/*
	 * 找出 expr 中用到的 rel 的列
	 *
	 * 注意：这里用 pull_var_clause 没问题，只因为 CHECK 约束中
	 * 不允许子查询；它不会去检查
	 * 子查询的内容。
	 */
	varList = pull_var_clause(expr, 0);
	keycount = list_length(varList);

	if (keycount > 0)
	{
		ListCell   *vl;
		int			i = 0;

		attNos = (int16 *) palloc(keycount * sizeof(int16));
		foreach(vl, varList)
		{
			Var		   *var = (Var *) lfirst(vl);
			int			j;

			for (j = 0; j < i; j++)
				if (attNos[j] == var->varattno)
					break;
			if (j == i)
				attNos[i++] = var->varattno;
		}
		keycount = i;
	}
	else
		attNos = NULL;

	/*
	 * 分区表本身不含任何行，因此 NO INHERIT
	 * 约束毫无意义。
	 */
	if (is_no_inherit &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot add NO INHERIT constraint to partitioned table \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * 创建 CHECK 约束
	 */
	constrOid =
		CreateConstraintEntry(ccname,	/* 约束名 */
							  RelationGetNamespace(rel),	/* 命名空间 */
							  CONSTRAINT_CHECK, /* 约束类型 */
							  false,	/* 是否可延迟 */
							  false,	/* 是否已延迟 */
							  is_validated,
							  InvalidOid,	/* 无父约束 */
							  RelationGetRelid(rel),	/* 关系 */
							  attNos,	/* 约束中的属性 */
							  keycount, /* 约束中的键属性数 */
							  keycount, /* 约束中的总属性数 */
							  InvalidOid,	/* 不是域约束 */
							  InvalidOid,	/* 无关联索引 */
							  InvalidOid,	/* 外键字段 */
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
							  NULL, /* 不是排他约束 */
							  expr, /* CHECK 约束的树形式 */
							  ccbin,	/* CHECK 约束的二进制形式 */
							  is_local, /* conislocal */
							  inhcount, /* coninhcount */
							  is_no_inherit,	/* connoinherit */
							  is_internal); /* 是否内部构造？ */

	pfree(ccbin);

	return constrOid;
}

/*
 * 存储默认值和约束（作为 CookedConstraint 列表传入）。
 *
 * 每个 CookedConstraint 结构被修改以存储新目录元组 OID。
 *
 * 注意：只有预先烹调的表达式会以这种方式传入，也就是
 * 从已有关系继承的表达式。新解析的
 * 表达式可以稍后通过直接调用 StoreAttrDefault
 * 和 StoreRelCheck 添加（见 AddRelationNewConstraints()）。
 */
static void
StoreConstraints(Relation rel, List *cooked_constraints, bool is_internal)
{
	int			numchecks = 0;
	ListCell   *lc;

	if (cooked_constraints == NIL)
		return;					/* 无事可做 */

	/*
	 * 除非本关系刚创建的 pg_attribute 元组变为可见，
	 * 否则约束表达式的反解析会失败。因此，递增
	 * 命令计数器。CAUTION：这将导致关系缓存条目重建。
	 */
	CommandCounterIncrement();

	foreach(lc, cooked_constraints)
	{
		CookedConstraint *con = (CookedConstraint *) lfirst(lc);

		switch (con->contype)
		{
			case CONSTR_DEFAULT:
				con->conoid = StoreAttrDefault(rel, con->attnum, con->expr,
											   is_internal, false);
				break;
			case CONSTR_CHECK:
				con->conoid =
					StoreRelCheck(rel, con->name, con->expr,
								  !con->skip_validation, con->is_local,
								  con->inhcount, con->is_no_inherit,
								  is_internal);
				numchecks++;
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	if (numchecks > 0)
		SetRelationNumChecks(rel, numchecks);
}

/*
 * AddRelationNewConstraints
 *
 * 向已有关系添加新的列默认表达式和/或
 * 约束检查表达式。在 DefineRelation 中为效率定义为
 * 两者都做，当然你也可以只传入空列表来只做
 * 其中一项。
 *
 * rel: 要修改的关系
 * newColDefaults: RawColumnDefault 结构列表
 * newConstraints: Constraint 节点列表
 * allow_merge: 若 CHECK 约束可与已有的合并则为 true
 * is_local: 若定义为本地则为 true，若继承则为 false
 * is_internal: 若是某内部过程的结果而非用户请求则为 true
 *
 * newColDefaults 中的所有条目都会被处理。newConstraints 中的条目
 * 仅当它们是 CONSTR_CHECK 类型时才处理。
 *
 * 返回 CookedConstraint 节点列表，显示添加到关系的
 * 默认值和约束表达式的烹调后形式。
 *
 * 注意：调用方应以某种自冲突锁模式打开 rel，
 * 并持有该锁直到事务结束；正常情况下那会
 * 是 AccessExclusiveLock，但若调用方知道该约束已由
 * 其他方式强制执行，可以是 ShareUpdateExclusiveLock。另外，我们
 * 假定调用方已在必要时做了 CommandCounterIncrement，使
 * 该关系的目录元组可见。
 */
List *
AddRelationNewConstraints(Relation rel,
						  List *newColDefaults,
						  List *newConstraints,
						  bool allow_merge,
						  bool is_local,
						  bool is_internal,
						  const char *queryString)
{
	List	   *cookedConstraints = NIL;
	TupleDesc	tupleDesc;
	TupleConstr *oldconstr;
	int			numoldchecks;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	int			numchecks;
	List	   *checknames;
	ListCell   *cell;
	Node	   *expr;
	CookedConstraint *cooked;

	/*
	 * 获取已有约束的信息。
	 */
	tupleDesc = RelationGetDescr(rel);
	oldconstr = tupleDesc->constr;
	if (oldconstr)
		numoldchecks = oldconstr->num_check;
	else
		numoldchecks = 0;

	/*
	 * 创建哑 ParseState，并把目标关系作为其唯一
	 * 范围表条目插入。transformExpr 需要 ParseState。
	 */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	nsitem = addRangeTableEntryForRelation(pstate,
										   rel,
										   AccessShareLock,
										   NULL,
										   false,
										   true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/*
	 * 处理列默认表达式。
	 */
	foreach(cell, newColDefaults)
	{
		RawColumnDefault *colDef = (RawColumnDefault *) lfirst(cell);
		Form_pg_attribute atp = TupleDescAttr(rel->rd_att, colDef->attnum - 1);
		Oid			defOid;

		expr = cookDefault(pstate, colDef->raw_default,
						   atp->atttypid, atp->atttypmod,
						   NameStr(atp->attname),
						   atp->attgenerated);

		/*
		 * 若表达式只是 NULL 常量，我们不费心做
		 * 显式的 pg_attrdef 条目，因为默认行为
		 * 与之等价。这适用于列默认值，但不适用于
		 * 生成表达式。
		 *
		 * 注意此测试一个非显而易见的性质：若列是
		 * 域类型，我们得到的不是裸 null Const 而是
		 * CoerceToDomain 表达式，因此不会丢弃该默认值。这
		 * 很关键，因为需要保留列默认值以
		 * 覆盖域可能具有的任何默认值。
		 */
		if (expr == NULL ||
			(!colDef->generated &&
			 IsA(expr, Const) &&
			 castNode(Const, expr)->constisnull))
			continue;

		defOid = StoreAttrDefault(rel, colDef->attnum, expr, is_internal,
								  false);

		cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
		cooked->contype = CONSTR_DEFAULT;
		cooked->conoid = defOid;
		cooked->name = NULL;
		cooked->attnum = colDef->attnum;
		cooked->expr = expr;
		cooked->skip_validation = false;
		cooked->is_local = is_local;
		cooked->inhcount = is_local ? 0 : 1;
		cooked->is_no_inherit = false;
		cookedConstraints = lappend(cookedConstraints, cooked);
	}

	/*
	 * 处理约束表达式。
	 */
	numchecks = numoldchecks;
	checknames = NIL;
	foreach(cell, newConstraints)
	{
		Constraint *cdef = (Constraint *) lfirst(cell);
		char	   *ccname;
		Oid			constrOid;

		if (cdef->contype != CONSTR_CHECK)
			continue;

		if (cdef->raw_expr != NULL)
		{
			Assert(cdef->cooked_expr == NULL);

			/*
			 * 将原始解析树变换为可执行表达式，并验证
			 * 它作为 CHECK 约束有效。
			 */
			expr = cookConstraint(pstate, cdef->raw_expr,
								  RelationGetRelationName(rel));
		}
		else
		{
			Assert(cdef->cooked_expr != NULL);

			/*
			 * 这里假定解析器只会把有效的 CHECK
			 * 表达式传给我们，因此不做特别检查。
			 */
			expr = stringToNode(cdef->cooked_expr);
		}

		/*
		 * 检查名称唯一性，若未给出则生成名称。
		 */
		if (cdef->conname != NULL)
		{
			ListCell   *cell2;

			ccname = cdef->conname;
			/* 对照其他新约束检查 */
			/* 需要，因为循环中不做 CommandCounterIncrement */
			foreach(cell2, checknames)
			{
				if (strcmp((char *) lfirst(cell2), ccname) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("check constraint \"%s\" already exists",
									ccname)));
			}

			/* 保存名称供后续检查 */
			checknames = lappend(checknames, ccname);

			/*
			 * 对照预先存在的约束检查。若允许与
			 * 已有约束合并，这里就没有更多事可做。
			 * （我们从结果中省略重复约束，这正是
			 * ATAddCheckConstraint 想要的。）
			 */
			if (MergeWithExistingConstraint(rel, ccname, expr,
											allow_merge, is_local,
											cdef->initially_valid,
											cdef->is_no_inherit))
				continue;
		}
		else
		{
			/*
			 * 生成名称时，我们想为列约束创建 "tab_col_check"，
			 * 为表约束创建 "tab_check"。我们
			 * 不再有关于约束短语句法位置的任何信息，
			 * 因此通过看表达式是否引用
			 * 多于一列来近似。（若用户
			 * 按规则来，结果是一样的……）
			 *
			 * 注意：pull_var_clause() 不进入子链接，但我们
			 * 上面已排除那些；而且反正这只需要是
			 * 近似答案。
			 */
			List	   *vars;
			char	   *colname;

			vars = pull_var_clause(expr, 0);

			/* 消除重复 */
			vars = list_union(NIL, vars);

			if (list_length(vars) == 1)
				colname = get_attname(RelationGetRelid(rel),
									  ((Var *) linitial(vars))->varattno,
									  true);
			else
				colname = NULL;

			ccname = ChooseConstraintName(RelationGetRelationName(rel),
										  colname,
										  "check",
										  RelationGetNamespace(rel),
										  checknames);

			/* 保存名称供后续检查 */
			checknames = lappend(checknames, ccname);
		}

		/*
		 * 好，存储它。
		 */
		constrOid =
			StoreRelCheck(rel, ccname, expr, cdef->initially_valid, is_local,
						  is_local ? 0 : 1, cdef->is_no_inherit, is_internal);

		numchecks++;

		cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
		cooked->contype = CONSTR_CHECK;
		cooked->conoid = constrOid;
		cooked->name = ccname;
		cooked->attnum = 0;
		cooked->expr = expr;
		cooked->skip_validation = cdef->skip_validation;
		cooked->is_local = is_local;
		cooked->inhcount = is_local ? 0 : 1;
		cooked->is_no_inherit = cdef->is_no_inherit;
		cookedConstraints = lappend(cookedConstraints, cooked);
	}

	/*
	 * 更新该关系 pg_class 元组中的约束计数。即便
	 * 没有变化我们也这样做，以确保为 pg_class 元组发出
	 * SI 更新消息，从而强制其他
	 * 后端重建该关系的关系缓存条目。（若我们添加了
	 * 默认值但没有约束，这至关重要。）
	 */
	SetRelationNumChecks(rel, numchecks);

	return cookedConstraints;
}

/*
 * 检查是否有与提议的新 CHECK 约束冲突的
 * 预先存在的 CHECK 约束，并按需调整其
 * conislocal/coninhcount 设置或报错。
 *
 * 若已合并（约束是重复的）返回 true，若它
 * 有迄今唯一的名称则返回 false，冲突则报错。
 *
 * XXX 若你改这段代码，也请看 MergeConstraintsIntoExisting。
 */
static bool
MergeWithExistingConstraint(Relation rel, const char *ccname, Node *expr,
							bool allow_merge, bool is_local,
							bool is_initially_valid,
							bool is_no_inherit)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[3];
	HeapTuple	tup;

	/* 搜索同名且同一关系的 pg_constraint 条目 */
	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	found = false;

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(ccname));

	conscan = systable_beginscan(conDesc, ConstraintRelidTypidNameIndexId, true,
								 NULL, 3, skey);

	/* 最多只能有一行匹配 */
	if (HeapTupleIsValid(tup = systable_getnext(conscan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

		/* 找到了。若不是相同的 CHECK 约束则冲突 */
		if (con->contype == CONSTRAINT_CHECK)
		{
			Datum		val;
			bool		isnull;

			val = fastgetattr(tup,
							  Anum_pg_constraint_conbin,
							  conDesc->rd_att, &isnull);
			if (isnull)
				elog(ERROR, "null conbin for rel %s",
					 RelationGetRelationName(rel));
			if (equal(expr, stringToNode(TextDatumGetCString(val))))
				found = true;
		}

		/*
		 * 若已有约束纯粹是继承来的（无本地
		 * 定义），则把添加本地约束解释为合法
		 * 合并。这允许对父表和子
		 * 表以任一顺序给出 ALTER ADD CONSTRAINT 而得到相同终态。但若
		 * 关系是分区，所有继承约束总是
		 * 非本地的，包括那些被合并的。
		 */
		if (is_local && !con->conislocal && !rel->rd_rel->relispartition)
			allow_merge = true;

		if (!found || !allow_merge)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for relation \"%s\" already exists",
							ccname, RelationGetRelationName(rel))));

		/* 若子约束是 "no inherit" 则无法合并 */
		if (con->connoinherit)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with non-inherited constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/*
		 * 不得把已有的继承约束改为 "no inherit"
		 * 状态。因为继承约束应能
		 * 传播到更下层的子女。
		 */
		if (con->coninhcount > 0 && is_no_inherit)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with inherited constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/*
		 * 若子约束是 "not valid" 则无法与
		 * 有效的父约束合并。
		 */
		if (is_initially_valid && !con->convalidated)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with NOT VALID constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/* 可以更新该元组 */
		ereport(NOTICE,
				(errmsg("merging constraint \"%s\" with inherited definition",
						ccname)));

		tup = heap_copytuple(tup);
		con = (Form_pg_constraint) GETSTRUCT(tup);

		/*
		 * 对分区而言，继承约束必须只被继承
		 * 一次，因为它无法有多个父，且从不
		 * 被视为本地。
		 */
		if (rel->rd_rel->relispartition)
		{
			con->coninhcount = 1;
			con->conislocal = false;
		}
		else
		{
			if (is_local)
				con->conislocal = true;
			else
				con->coninhcount++;

			if (con->coninhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
		}

		if (is_no_inherit)
		{
			Assert(is_local);
			con->connoinherit = true;
		}

		CatalogTupleUpdate(conDesc, &tup->t_self, tup);
	}

	systable_endscan(conscan);
	table_close(conDesc, RowExclusiveLock);

	return found;
}

/*
 * 更新该关系 pg_class 元组中的约束计数。
 *
 * 调用方最好对该关系持有排他锁。
 *
 * 一个重要副作用是将为 pg_class 元组发出 SI 更新
 * 消息，从而强制其他后端重建其
 * 该关系的关系缓存条目。另外，本后端将在下次
 * CommandCounterIncrement 时重建自己的关系缓存条目。
 */
static void
SetRelationNumChecks(Relation rel, int numchecks)
{
	Relation	relrel;
	HeapTuple	reltup;
	Form_pg_class relStruct;

	relrel = table_open(RelationRelationId, RowExclusiveLock);
	reltup = SearchSysCacheCopy1(RELOID,
								 ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));
	relStruct = (Form_pg_class) GETSTRUCT(reltup);

	if (relStruct->relchecks != numchecks)
	{
		relStruct->relchecks = numchecks;

		CatalogTupleUpdate(relrel, &reltup->t_self, reltup);
	}
	else
	{
		/* 跳过磁盘更新，但仍强制关系缓存失效 */
		CacheInvalidateRelcache(rel);
	}

	heap_freetuple(reltup);
	table_close(relrel, RowExclusiveLock);
}

/*
 * 检查对生成列的引用
 */
static bool
check_nested_generated_walker(Node *node, void *context)
{
	ParseState *pstate = context;

	if (node == NULL)
		return false;
	else if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Oid			relid;
		AttrNumber	attnum;

		relid = rt_fetch(var->varno, pstate->p_rtable)->relid;
		if (!OidIsValid(relid))
			return false;		/* XXX 我们难道不该报错吗？ */

		attnum = var->varattno;

		if (attnum > 0 && get_attgenerated(relid, attnum))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot use generated column \"%s\" in column generation expression",
							get_attname(relid, attnum, false)),
					 errdetail("A generated column cannot reference another generated column."),
					 parser_errposition(pstate, var->location)));
		/* 整行 Var 必然是自引用的，因此禁止 */
		if (attnum == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot use whole-row variable in column generation expression"),
					 errdetail("This would cause the generated column to depend on its own value."),
					 parser_errposition(pstate, var->location)));
		/* 系统列已在解析器中检查过 */

		return false;
	}
	else
		return expression_tree_walker(node, check_nested_generated_walker,
									  (void *) context);
}

static void
check_nested_generated(ParseState *pstate, Node *node)
{
	check_nested_generated_walker(node, pstate);
}

/*
 * 取原始默认值并转换成可供存储的
 * 烹调后格式。
 *
 * 解析状态应设置为能识别表达式中可能出现的
 * 任何 vars。（尽管我们打算拒绝 vars，给出
 * 正确错误消息比 "unknown var" 更友好。）
 *
 * 若 atttypid 不是 InvalidOid，将表达式强制为指定
 * 类型（以及 typmod atttypmod）。仅在此情况下需要 attname：
 * 它用于错误消息（若有）。
 */
Node *
cookDefault(ParseState *pstate,
			Node *raw_default,
			Oid atttypid,
			int32 atttypmod,
			const char *attname,
			char attgenerated)
{
	Node	   *expr;

	Assert(raw_default != NULL);

	/*
	 * 将原始解析树变换为可执行表达式。
	 */
	expr = transformExpr(pstate, raw_default, attgenerated ? EXPR_KIND_GENERATED_COLUMN : EXPR_KIND_COLUMN_DEFAULT);

	if (attgenerated)
	{
		/* 禁止引用其他生成列 */
		check_nested_generated(pstate, expr);

		/* 禁止可变函数 */
		if (contain_mutable_functions_after_planning((Expr *) expr))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("generation expression is not immutable")));
	}
	else
	{
		/*
		 * 对默认表达式，transformExpr() 应已拒绝
		 * 列引用。
		 */
		Assert(!contain_var_clause(expr));
	}

	/*
	 * 若给定，将表达式强制为正确的类型和 typmod。这
	 * 应与解析器对非默认表达式的处理一致 —
	 * 见 transformAssignedExpr()。
	 */
	if (OidIsValid(atttypid))
	{
		Oid			type_id = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, type_id,
									 atttypid, atttypmod,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST,
									 -1);
		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is of type %s"
							" but default expression is of type %s",
							attname,
							format_type_be(atttypid),
							format_type_be(type_id)),
					 errhint("You will need to rewrite or cast the expression.")));
	}

	/*
	 * 最后，处理完成表达式中的排序规则。
	 */
	assign_expr_collations(pstate, expr);

	return expr;
}

/*
 * 取原始 CHECK 约束表达式并转换成可供存储的
 * 烹调后格式。
 *
 * 解析状态必须设置为能识别表达式中可能出现的
 * 任何 vars。
 */
static Node *
cookConstraint(ParseState *pstate,
			   Node *raw_constraint,
			   char *relname)
{
	Node	   *expr;

	/*
	 * 将原始解析树变换为可执行表达式。
	 */
	expr = transformExpr(pstate, raw_constraint, EXPR_KIND_CHECK_CONSTRAINT);

	/*
	 * 确保它产生布尔结果。
	 */
	expr = coerce_to_boolean(pstate, expr, "CHECK");

	/*
	 * 处理排序规则。
	 */
	assign_expr_collations(pstate, expr);

	/*
	 * 确保未引用外部关系（既然 add_missing_from 已成历史，
	 * 这大概是死代码了）。
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("only table \"%s\" can be referenced in check constraint",
						relname)));

	return expr;
}

/*
 * CopyStatistics --- 把 pg_statistic 中的条目从一个关系复制到另一个
 */
void
CopyStatistics(Oid fromrelid, Oid torelid)
{
	HeapTuple	tup;
	SysScanDesc scan;
	ScanKeyData key[1];
	Relation	statrel;
	CatalogIndexState indstate = NULL;

	statrel = table_open(StatisticRelationId, RowExclusiveLock);

	/* 现在搜索统计记录 */
	ScanKeyInit(&key[0],
				Anum_pg_statistic_starelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fromrelid));

	scan = systable_beginscan(statrel, StatisticRelidAttnumInhIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_statistic statform;

		/* 做一份可修改的副本 */
		tup = heap_copytuple(tup);
		statform = (Form_pg_statistic) GETSTRUCT(tup);

		/* 更新元组副本并插入它 */
		statform->starelid = torelid;

		/* 确知需要时才获取索引信息 */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(statrel);

		CatalogTupleInsertWithInfo(statrel, tup, indstate);

		heap_freetuple(tup);
	}

	systable_endscan(scan);

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);
	table_close(statrel, RowExclusiveLock);
}

/*
 * RemoveStatistics --- 移除某关系或列在 pg_statistic 中的条目
 *
 * 若 attnum 为零，移除该关系的全部条目；否则只移除
	 * 该列的那一条（或那些）。
 */
void
RemoveStatistics(Oid relid, AttrNumber attnum)
{
	Relation	pgstatistic;
	SysScanDesc scan;
	ScanKeyData key[2];
	int			nkeys;
	HeapTuple	tuple;

	pgstatistic = table_open(StatisticRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_statistic_starelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	if (attnum == 0)
		nkeys = 1;
	else
	{
		ScanKeyInit(&key[1],
					Anum_pg_statistic_staattnum,
					BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(attnum));
		nkeys = 2;
	}

	scan = systable_beginscan(pgstatistic, StatisticRelidAttnumInhIndexId, true,
							  NULL, nkeys, key);

	/* 即便 attnum != 0 也必须循环，以防有继承的统计 */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(pgstatistic, &tuple->t_self);

	systable_endscan(scan);

	table_close(pgstatistic, RowExclusiveLock);
}


/*
 * RelationTruncateIndexes - 把与堆关系关联的所有索引
 * 截断为零元组。
 *
 * 本例程将截断然后重建指定关系上的
 * 索引。调用方必须对 rel 持有排他锁。
 */
static void
RelationTruncateIndexes(Relation heapRelation)
{
	ListCell   *indlist;

	/* 请关系缓存生成该关系的索引列表 */
	foreach(indlist, RelationGetIndexList(heapRelation))
	{
		Oid			indexId = lfirst_oid(indlist);
		Relation	currentIndex;
		IndexInfo  *indexInfo;

		/* 打开索引关系；使用排他锁，以求保险 */
		currentIndex = index_open(indexId, AccessExclusiveLock);

		/*
		 * 获取 index_build 所需信息。既然我们知道没有
		 * 实际需要建索引的元组，可以使用哑 IndexInfo。
		 * 这样构造稍便宜些，但真正目的是避免
		 * 可能在索引表达式或谓词中运行用户定义代码。
		 * 我们可能在 ON COMMIT 处理期间被调用，
		 * 那时我们不想运行任何这类代码。
		 */
		indexInfo = BuildDummyIndexInfo(currentIndex);

		/*
		 * 现在截断实际文件（并丢弃缓冲区）。
		 */
		RelationTruncate(currentIndex, 0);

		/* 初始化索引并重建 */
		/* 注意：我们不需要重新建立 pkey 设置 */
		index_build(heapRelation, currentIndex, indexInfo, true, false);

		/* 这个索引处理完毕 */
		index_close(currentIndex, NoLock);
	}
}

/*
 *	 heap_truncate
 *
 *	 本例程删除指定各关系中的全部数据。
 *
 * 这不是事务安全的！commands/tablecmds.c 中还有另一个
 * 事务安全的实现。我们现在只把它用于
 * 临时表的 ON COMMIT 截断，那时这无关紧要。
 */
void
heap_truncate(List *relids)
{
	List	   *relations = NIL;
	ListCell   *cell;

	/* 打开关系以便处理，并对每个获取排他访问 */
	foreach(cell, relids)
	{
		Oid			rid = lfirst_oid(cell);
		Relation	rel;

		rel = table_open(rid, AccessExclusiveLock);
		relations = lappend(relations, rel);
	}

	/* 不允许截断被外键引用的表 */
	heap_truncate_check_FKs(relations, true);

	/* 可以执行了 */
	foreach(cell, relations)
	{
		Relation	rel = lfirst(cell);

		/* 截断该关系 */
		heap_truncate_one_rel(rel);

		/* 关闭该关系，但直到提交都保留其上的排他锁 */
		table_close(rel, NoLock);
	}
}

/*
 *	 heap_truncate_one_rel
 *
 *	 本例程删除指定关系中的全部数据。
 *
 * 这不是事务安全的，因为截断立即完成
 * 且稍后无法回滚。调用方负责已
 * 检查权限等，并且必须已获得 AccessExclusiveLock。
 */
void
heap_truncate_one_rel(Relation rel)
{
	Oid			toastrelid;

	/*
	 * 截断该关系。分区表没有存储，因此对它们
	 * 这里无事可做。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* 截断底层关系 */
	table_relation_nontransactional_truncate(rel);

	/* 若关系有索引，也截断索引 */
	RelationTruncateIndexes(rel);

	/* 若有 TOAST 表，也截断它 */
	toastrelid = rel->rd_rel->reltoastrelid;
	if (OidIsValid(toastrelid))
	{
		Relation	toastrel = table_open(toastrelid, AccessExclusiveLock);

		table_relation_nontransactional_truncate(toastrel);
		RelationTruncateIndexes(toastrel);
		/* 保留该锁…… */
		table_close(toastrel, NoLock);
	}
}

/*
 * heap_truncate_check_FKs
 *		检查是否有外键引用即将被截断的
 *		关系列表，若有则报错
 *
 * 我们禁止这类外键（自引用的除外），因为 TRUNCATE 的
 * 全部意义就是不去扫描将被丢掉的各个行。
 *
 * 拆出来以便两种截断实现都能共享。
 * 调用方应已对关系持有适当的锁。
 *
 * tempTables 仅用于选择适当的错误消息。
 */
void
heap_truncate_check_FKs(List *relations, bool tempTables)
{
	List	   *oids = NIL;
	List	   *dependents;
	ListCell   *cell;

	/*
	 * 构造感兴趣关系的 OIDs 列表。
	 *
	 * 若关系没有触发器，则它既不能有外键，也不能被
	 * 另一表的外键引用，因此可以忽略。对
	 * 分区表，外键没有触发器，因此必须仍然
	 * 包含它们。
	 */
	foreach(cell, relations)
	{
		Relation	rel = lfirst(cell);

		if (rel->rd_rel->relhastriggers ||
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			oids = lappend_oid(oids, RelationGetRelid(rel));
	}

	/*
	 * 快路径：若没有关系有触发器，也就都没有外键。
	 */
	if (oids == NIL)
		return;

	/*
	 * 否则必须扫描 pg_constraint。我们对所有关系做一次
	 * 扫描；若什么也没找到，那就都好。
	 */
	dependents = heap_truncate_find_FKs(oids);
	if (dependents == NIL)
		return;

	/*
	 * 否则我们按每个关系重复扫描一次，以识别要抱怨的
	 * 某一对关系。这相当慢，但
	 * 失败路径上性能不太要紧。这样做的原因是
	 * 确保产生的消息不依赖于
	 * pg_constraint 中碰巧的行位置。
	 */
	foreach(cell, oids)
	{
		Oid			relid = lfirst_oid(cell);
		ListCell   *cell2;

		dependents = heap_truncate_find_FKs(list_make1_oid(relid));

		foreach(cell2, dependents)
		{
			Oid			relid2 = lfirst_oid(cell2);

			if (!list_member_oid(oids, relid2))
			{
				char	   *relname = get_rel_name(relid);
				char	   *relname2 = get_rel_name(relid2);

				if (tempTables)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unsupported ON COMMIT and foreign key combination"),
							 errdetail("Table \"%s\" references \"%s\", but they do not have the same ON COMMIT setting.",
									   relname2, relname)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot truncate a table referenced in a foreign key constraint"),
							 errdetail("Table \"%s\" references \"%s\".",
									   relname2, relname),
							 errhint("Truncate table \"%s\" at the same time, "
									 "or use TRUNCATE ... CASCADE.",
									 relname2)));
			}
		}
	}
}

/*
 * heap_truncate_find_FKs
 *		找出拥有引用给定任一关系的外键的关系
 *
 * 输入和结果都是关系 OIDs 列表。结果不含
 * 重复，*不*包含输入列表中已有的任何关系，
 * 并按 OID 排序。（最后这一性质主要是为了
 * 保证回归测试中行为一致；我们不希望
 * 行为随 pg_constraint 中碰巧的行位置而变。）
 *
 * 注意：调用方应对 relationIds 中提及的所有关系已持有
 * 适当的锁。由于添加或删除外键需要对
 * 双方关系持有排他锁，这确保答案是稳定的。
 */
List *
heap_truncate_find_FKs(List *relationIds)
{
	List	   *result = NIL;
	List	   *oids;
	List	   *parent_cons;
	ListCell   *cell;
	ScanKeyData key;
	Relation	fkeyRel;
	SysScanDesc fkeyScan;
	HeapTuple	tuple;
	bool		restart;

	oids = list_copy(relationIds);

	/*
	 * 必须扫描 pg_constraint。眼下是顺序扫描，因为
	 * confrelid 上没有可用索引。
	 */
	fkeyRel = table_open(ConstraintRelationId, AccessShareLock);

restart:
	restart = false;
	parent_cons = NIL;

	fkeyScan = systable_beginscan(fkeyRel, InvalidOid, false,
								  NULL, 0, NULL);

	while (HeapTupleIsValid(tuple = systable_getnext(fkeyScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		/* 不是外键 */
		if (con->contype != CONSTRAINT_FOREIGN)
			continue;

		/* 未引用我们表列表中的某一个 */
		if (!list_member_oid(oids, con->confrelid))
			continue;

		/*
		 * 若此约束有我们尚未见过的父约束，
		 * 为下面的第二轮循环跟踪它。跟踪父
		 * 约束使我们能爬到顶层约束，并
		 * 查找引用该分区表的所有可能关系。
		 */
		if (OidIsValid(con->conparentid) &&
			!list_member_oid(parent_cons, con->conparentid))
			parent_cons = lappend_oid(parent_cons, con->conparentid);

		/*
		 * 把引用方加入结果，除非已在输入列表中。
		 * （先别担心重复：下面会处理）。
		 */
		if (!list_member_oid(relationIds, con->conrelid))
			result = lappend_oid(result, con->conrelid);
	}

	systable_endscan(fkeyScan);

	/*
	 * 处理我们发现的每个父约束，把它们引用的关系
	 * 列表加入 oids 列表。若确实加入了任何新的这类
	 * 关系，重做上面的第一轮循环。另外，若看到父
	 * 约束自己还有父，也加入，以便我们在单次
	 * 额外遍历中处理所有关系。
	 */
	foreach(cell, parent_cons)
	{
		Oid			parent = lfirst_oid(cell);

		ScanKeyInit(&key,
					Anum_pg_constraint_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(parent));

		fkeyScan = systable_beginscan(fkeyRel, ConstraintOidIndexId,
									  true, NULL, 1, &key);

		tuple = systable_getnext(fkeyScan);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

			/*
			 * pg_constraint 行对分区层次总是
			 * 这样出现：在约束的每一侧，每个分区
			 * 各有一行，指向另一侧的最顶层
			 * 表。
			 *
			 * 由于这种安排，我们可以通过把所有带有效
			 * conparentid 的行加入 'parent_cons'、把所有
			 * conparentid 为零的行加入 'oids' 列表，来正确
			 * 捕获所有相关关系。若有任何 oid 被加入
			 * 'oids'，通过设置 'restart' 重做上面的第一轮循环。
			 */
			if (OidIsValid(con->conparentid))
				parent_cons = list_append_unique_oid(parent_cons,
													 con->conparentid);
			else if (!list_member_oid(oids, con->confrelid))
			{
				oids = lappend_oid(oids, con->confrelid);
				restart = true;
			}
		}

		systable_endscan(fkeyScan);
	}

	list_free(parent_cons);
	if (restart)
		goto restart;

	table_close(fkeyRel, AccessShareLock);
	list_free(oids);

	/* 现在对结果列表排序并去重 */
	list_sort(result, list_oid_cmp);
	list_deduplicate_oid(result);

	return result;
}

/*
 * StorePartitionKey
 *		把分区键 rel 的信息存入目录
 */
void
StorePartitionKey(Relation rel,
				  char strategy,
				  int16 partnatts,
				  AttrNumber *partattrs,
				  List *partexprs,
				  Oid *partopclass,
				  Oid *partcollation)
{
	int			i;
	int2vector *partattrs_vec;
	oidvector  *partopclass_vec;
	oidvector  *partcollation_vec;
	Datum		partexprDatum;
	Relation	pg_partitioned_table;
	HeapTuple	tuple;
	Datum		values[Natts_pg_partitioned_table];
	bool		nulls[Natts_pg_partitioned_table] = {0};
	ObjectAddress myself;
	ObjectAddress referenced;
	ObjectAddresses *addrs;

	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/* 把分区属性编号、操作符类 OIDs 复制到数组中 */
	partattrs_vec = buildint2vector(partattrs, partnatts);
	partopclass_vec = buildoidvector(partopclass, partnatts);
	partcollation_vec = buildoidvector(partcollation, partnatts);

	/* 把表达式（若有）转换为 text datum */
	if (partexprs)
	{
		char	   *exprString;

		exprString = nodeToString(partexprs);
		partexprDatum = CStringGetTextDatum(exprString);
		pfree(exprString);
	}
	else
		partexprDatum = (Datum) 0;

	pg_partitioned_table = table_open(PartitionedRelationId, RowExclusiveLock);

	/* 只有这个字段可能为 NULL */
	if (!partexprDatum)
		nulls[Anum_pg_partitioned_table_partexprs - 1] = true;

	values[Anum_pg_partitioned_table_partrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_partitioned_table_partstrat - 1] = CharGetDatum(strategy);
	values[Anum_pg_partitioned_table_partnatts - 1] = Int16GetDatum(partnatts);
	values[Anum_pg_partitioned_table_partdefid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_partitioned_table_partattrs - 1] = PointerGetDatum(partattrs_vec);
	values[Anum_pg_partitioned_table_partclass - 1] = PointerGetDatum(partopclass_vec);
	values[Anum_pg_partitioned_table_partcollation - 1] = PointerGetDatum(partcollation_vec);
	values[Anum_pg_partitioned_table_partexprs - 1] = partexprDatum;

	tuple = heap_form_tuple(RelationGetDescr(pg_partitioned_table), values, nulls);

	CatalogTupleInsert(pg_partitioned_table, tuple);
	table_close(pg_partitioned_table, RowExclusiveLock);

	/* 将本关系标记为依赖于以下几项 */
	addrs = new_object_addresses();
	ObjectAddressSet(myself, RelationRelationId, RelationGetRelid(rel));

	/* 每个键列的操作符类和排序规则 */
	for (i = 0; i < partnatts; i++)
	{
		ObjectAddressSet(referenced, OperatorClassRelationId, partopclass[i]);
		add_exact_object_address(&referenced, addrs);

		/* 默认排序规则已被钉住，不必记录 */
		if (OidIsValid(partcollation[i]) &&
			partcollation[i] != DEFAULT_COLLATION_OID)
		{
			ObjectAddressSet(referenced, CollationRelationId, partcollation[i]);
			add_exact_object_address(&referenced, addrs);
		}
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/*
	 * 分区列被做成内部依赖于该表，
	 * 因为不删除整张表就无法删除其中任何一列。
	 * （ATExecDropColumn 独立地强制这一点，但并非万无一失，
	 * 因此我们也需要依赖。）
	 */
	for (i = 0; i < partnatts; i++)
	{
		if (partattrs[i] == 0)
			continue;			/* 此处忽略表达式 */

		ObjectAddressSubSet(referenced, RelationRelationId,
							RelationGetRelid(rel), partattrs[i]);
		recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
	}

	/*
	 * 也考虑分区表达式中提及的任何东西。外部
	 * 引用（例如函数）得到 NORMAL 依赖。表达式中
	 * 提及的表列与普通分区列同样处理，
	 * 即它们变成内部依赖于整张表。
	 */
	if (partexprs)
		recordDependencyOnSingleRelExpr(&myself,
										(Node *) partexprs,
										RelationGetRelid(rel),
										DEPENDENCY_NORMAL,
										DEPENDENCY_INTERNAL,
										true /* 反转自身依赖 */ );

	/*
	 * 我们必须使关系缓存失效，以便下一次
	 * CommandCounterIncrement() 会使用刚创建的目录条目中的
	 * 信息重建它。
	 */
	CacheInvalidateRelcache(rel);
}

/*
 *	RemovePartitionKeyByRelId
 *		移除某关系的 pg_partitioned_table 条目
 */
void
RemovePartitionKeyByRelId(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;

	rel = table_open(PartitionedRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 relid);

	CatalogTupleDelete(rel, &tuple->t_self);

	ReleaseSysCache(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * StorePartitionBound
 *		更新 rel 的 pg_class 元组以存储分区边界，并将
 *		relispartition 设为 true
 *
 * 若这是默认分区，也更新 pg_partitioned_table 中的
 * 默认分区 OID。
 *
 * 另外，使父表的关系缓存失效，以便下次重建会把
 * 新分区的信息加载进其分区描述符。若有
 * 默认分区，也必须使其关系缓存条目失效。
 */
void
StorePartitionBound(Relation rel, Relation parent, PartitionBoundSpec *bound)
{
	Relation	classRel;
	HeapTuple	tuple,
				newtuple;
	Datum		new_val[Natts_pg_class];
	bool		new_null[Natts_pg_class],
				new_repl[Natts_pg_class];
	Oid			defaultPartOid;

	/* 更新 pg_class 元组 */
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

#ifdef USE_ASSERT_CHECKING
	{
		Form_pg_class classForm;
		bool		isnull;

		classForm = (Form_pg_class) GETSTRUCT(tuple);
		Assert(!classForm->relispartition);
		(void) SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relpartbound,
							   &isnull);
		Assert(isnull);
	}
#endif

	/* 填入 relpartbound 值 */
	memset(new_val, 0, sizeof(new_val));
	memset(new_null, false, sizeof(new_null));
	memset(new_repl, false, sizeof(new_repl));
	new_val[Anum_pg_class_relpartbound - 1] = CStringGetTextDatum(nodeToString(bound));
	new_null[Anum_pg_class_relpartbound - 1] = false;
	new_repl[Anum_pg_class_relpartbound - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classRel),
								 new_val, new_null, new_repl);
	/* 同时设置该标志 */
	((Form_pg_class) GETSTRUCT(newtuple))->relispartition = true;

	/*
	 * 我们已经检查过没有继承子女，但重置
	 * relhassubclass，以防它被留下。
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION && rel->rd_rel->relhassubclass)
		((Form_pg_class) GETSTRUCT(newtuple))->relhassubclass = false;

	CatalogTupleUpdate(classRel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	table_close(classRel, RowExclusiveLock);

	/*
	 * 若我们正在为默认分区存储边界，也更新
	 * pg_partitioned_table。
	 */
	if (bound->is_default)
		update_default_partition_oid(RelationGetRelid(parent),
									 RelationGetRelid(rel));

	/* 使这些更新可见 */
	CommandCounterIncrement();

	/*
	 * 默认分区的分区约束依赖于
	 * 其他每个分区的分区边界，因此每次添加或
	 * 移除分区时我们都必须使该分区的
	 * 关系缓存条目失效。
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(parent, true));
	if (OidIsValid(defaultPartOid))
		CacheInvalidateRelcacheByRelid(defaultPartOid);

	CacheInvalidateRelcache(parent);
}
