/*-------------------------------------------------------------------------
 *
 * pg_constraint.c
 *	  支持操作 pg_constraint 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_constraint.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * CreateConstraintEntry
 *	创建一条约束表条目。
 *
 * 附属记录（例如实现该约束的触发器或索引）
 * *不*在此创建。但我们会从约束到它所依赖的对象
 * 建立依赖链接。
 *
 * 返回新约束的 OID。
 */
Oid
CreateConstraintEntry(const char *constraintName,
					  Oid constraintNamespace,
					  char constraintType,
					  bool isDeferrable,
					  bool isDeferred,
					  bool isValidated,
					  Oid parentConstrId,
					  Oid relId,
					  const int16 *constraintKey,
					  int constraintNKeys,
					  int constraintNTotalKeys,
					  Oid domainId,
					  Oid indexRelId,
					  Oid foreignRelId,
					  const int16 *foreignKey,
					  const Oid *pfEqOp,
					  const Oid *ppEqOp,
					  const Oid *ffEqOp,
					  int foreignNKeys,
					  char foreignUpdateType,
					  char foreignDeleteType,
					  const int16 *fkDeleteSetCols,
					  int numFkDeleteSetCols,
					  char foreignMatchType,
					  const Oid *exclOp,
					  Node *conExpr,
					  const char *conBin,
					  bool conIsLocal,
					  int conInhCount,
					  bool conNoInherit,
					  bool is_internal)
{
	Relation	conDesc;
	Oid			conOid;
	HeapTuple	tup;
	bool		nulls[Natts_pg_constraint];
	Datum		values[Natts_pg_constraint];
	ArrayType  *conkeyArray;
	ArrayType  *confkeyArray;
	ArrayType  *conpfeqopArray;
	ArrayType  *conppeqopArray;
	ArrayType  *conffeqopArray;
	ArrayType  *conexclopArray;
	ArrayType  *confdelsetcolsArray;
	NameData	cname;
	int			i;
	ObjectAddress conobject;
	ObjectAddresses *addrs_auto;
	ObjectAddresses *addrs_normal;

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	Assert(constraintName);
	namestrcpy(&cname, constraintName);

	/*
	 * 把 C 数组转换为 Postgres 数组。
	 */
	if (constraintNKeys > 0)
	{
		Datum	   *conkey;

		conkey = (Datum *) palloc(constraintNKeys * sizeof(Datum));
		for (i = 0; i < constraintNKeys; i++)
			conkey[i] = Int16GetDatum(constraintKey[i]);
		conkeyArray = construct_array_builtin(conkey, constraintNKeys, INT2OID);
	}
	else
		conkeyArray = NULL;

	if (foreignNKeys > 0)
	{
		Datum	   *fkdatums;
		int			nkeys = Max(foreignNKeys, numFkDeleteSetCols);

		fkdatums = (Datum *) palloc(nkeys * sizeof(Datum));
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = Int16GetDatum(foreignKey[i]);
		confkeyArray = construct_array_builtin(fkdatums, foreignNKeys, INT2OID);
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(pfEqOp[i]);
		conpfeqopArray = construct_array_builtin(fkdatums, foreignNKeys, OIDOID);
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(ppEqOp[i]);
		conppeqopArray = construct_array_builtin(fkdatums, foreignNKeys, OIDOID);
		for (i = 0; i < foreignNKeys; i++)
			fkdatums[i] = ObjectIdGetDatum(ffEqOp[i]);
		conffeqopArray = construct_array_builtin(fkdatums, foreignNKeys, OIDOID);

		if (numFkDeleteSetCols > 0)
		{
			for (i = 0; i < numFkDeleteSetCols; i++)
				fkdatums[i] = Int16GetDatum(fkDeleteSetCols[i]);
			confdelsetcolsArray = construct_array_builtin(fkdatums, numFkDeleteSetCols, INT2OID);
		}
		else
			confdelsetcolsArray = NULL;
	}
	else
	{
		confkeyArray = NULL;
		conpfeqopArray = NULL;
		conppeqopArray = NULL;
		conffeqopArray = NULL;
		confdelsetcolsArray = NULL;
	}

	if (exclOp != NULL)
	{
		Datum	   *opdatums;

		opdatums = (Datum *) palloc(constraintNKeys * sizeof(Datum));
		for (i = 0; i < constraintNKeys; i++)
			opdatums[i] = ObjectIdGetDatum(exclOp[i]);
		conexclopArray = construct_array_builtin(opdatums, constraintNKeys, OIDOID);
	}
	else
		conexclopArray = NULL;

	/* 初始化 nulls 与 values */
	for (i = 0; i < Natts_pg_constraint; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	conOid = GetNewOidWithIndex(conDesc, ConstraintOidIndexId,
								Anum_pg_constraint_oid);
	values[Anum_pg_constraint_oid - 1] = ObjectIdGetDatum(conOid);
	values[Anum_pg_constraint_conname - 1] = NameGetDatum(&cname);
	values[Anum_pg_constraint_connamespace - 1] = ObjectIdGetDatum(constraintNamespace);
	values[Anum_pg_constraint_contype - 1] = CharGetDatum(constraintType);
	values[Anum_pg_constraint_condeferrable - 1] = BoolGetDatum(isDeferrable);
	values[Anum_pg_constraint_condeferred - 1] = BoolGetDatum(isDeferred);
	values[Anum_pg_constraint_convalidated - 1] = BoolGetDatum(isValidated);
	values[Anum_pg_constraint_conrelid - 1] = ObjectIdGetDatum(relId);
	values[Anum_pg_constraint_contypid - 1] = ObjectIdGetDatum(domainId);
	values[Anum_pg_constraint_conindid - 1] = ObjectIdGetDatum(indexRelId);
	values[Anum_pg_constraint_conparentid - 1] = ObjectIdGetDatum(parentConstrId);
	values[Anum_pg_constraint_confrelid - 1] = ObjectIdGetDatum(foreignRelId);
	values[Anum_pg_constraint_confupdtype - 1] = CharGetDatum(foreignUpdateType);
	values[Anum_pg_constraint_confdeltype - 1] = CharGetDatum(foreignDeleteType);
	values[Anum_pg_constraint_confmatchtype - 1] = CharGetDatum(foreignMatchType);
	values[Anum_pg_constraint_conislocal - 1] = BoolGetDatum(conIsLocal);
	values[Anum_pg_constraint_coninhcount - 1] = Int16GetDatum(conInhCount);
	values[Anum_pg_constraint_connoinherit - 1] = BoolGetDatum(conNoInherit);

	if (conkeyArray)
		values[Anum_pg_constraint_conkey - 1] = PointerGetDatum(conkeyArray);
	else
		nulls[Anum_pg_constraint_conkey - 1] = true;

	if (confkeyArray)
		values[Anum_pg_constraint_confkey - 1] = PointerGetDatum(confkeyArray);
	else
		nulls[Anum_pg_constraint_confkey - 1] = true;

	if (conpfeqopArray)
		values[Anum_pg_constraint_conpfeqop - 1] = PointerGetDatum(conpfeqopArray);
	else
		nulls[Anum_pg_constraint_conpfeqop - 1] = true;

	if (conppeqopArray)
		values[Anum_pg_constraint_conppeqop - 1] = PointerGetDatum(conppeqopArray);
	else
		nulls[Anum_pg_constraint_conppeqop - 1] = true;

	if (conffeqopArray)
		values[Anum_pg_constraint_conffeqop - 1] = PointerGetDatum(conffeqopArray);
	else
		nulls[Anum_pg_constraint_conffeqop - 1] = true;

	if (confdelsetcolsArray)
		values[Anum_pg_constraint_confdelsetcols - 1] = PointerGetDatum(confdelsetcolsArray);
	else
		nulls[Anum_pg_constraint_confdelsetcols - 1] = true;

	if (conexclopArray)
		values[Anum_pg_constraint_conexclop - 1] = PointerGetDatum(conexclopArray);
	else
		nulls[Anum_pg_constraint_conexclop - 1] = true;

	if (conBin)
		values[Anum_pg_constraint_conbin - 1] = CStringGetTextDatum(conBin);
	else
		nulls[Anum_pg_constraint_conbin - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(conDesc), values, nulls);

	CatalogTupleInsert(conDesc, tup);

	ObjectAddressSet(conobject, ConstraintRelationId, conOid);

	table_close(conDesc, RowExclusiveLock);

	/* 处理自动依赖集合 */
	addrs_auto = new_object_addresses();

	if (OidIsValid(relId))
	{
		/*
		 * 注册从约束到所属关系的自动依赖，或若提及了
		 * 具体列则到这些列。
		 */
		ObjectAddress relobject;

		if (constraintNTotalKeys > 0)
		{
			for (i = 0; i < constraintNTotalKeys; i++)
			{
				ObjectAddressSubSet(relobject, RelationRelationId, relId,
									constraintKey[i]);
				add_exact_object_address(&relobject, addrs_auto);
			}
		}
		else
		{
			ObjectAddressSet(relobject, RelationRelationId, relId);
			add_exact_object_address(&relobject, addrs_auto);
		}
	}

	if (OidIsValid(domainId))
	{
		/*
		 * 注册从约束到所属域的自动依赖
		 */
		ObjectAddress domobject;

		ObjectAddressSet(domobject, TypeRelationId, domainId);
		add_exact_object_address(&domobject, addrs_auto);
	}

	record_object_address_dependencies(&conobject, addrs_auto,
									   DEPENDENCY_AUTO);
	free_object_addresses(addrs_auto);

	/* 处理普通依赖集合 */
	addrs_normal = new_object_addresses();

	if (OidIsValid(foreignRelId))
	{
		/*
		 * 注册从约束到外键关系的普通依赖，或若提及了
		 * 具体列则到这些列。
		 */
		ObjectAddress relobject;

		if (foreignNKeys > 0)
		{
			for (i = 0; i < foreignNKeys; i++)
			{
				ObjectAddressSubSet(relobject, RelationRelationId,
									foreignRelId, foreignKey[i]);
				add_exact_object_address(&relobject, addrs_normal);
			}
		}
		else
		{
			ObjectAddressSet(relobject, RelationRelationId, foreignRelId);
			add_exact_object_address(&relobject, addrs_normal);
		}
	}

	if (OidIsValid(indexRelId) && constraintType == CONSTRAINT_FOREIGN)
	{
		/*
		 * 注册对支持外键约束的唯一索引的
		 * 普通依赖。（注意：对与唯一或主键约束关联的
		 * 索引，依赖方向相反，且
		 * 不在此建立。）
		 */
		ObjectAddress relobject;

		ObjectAddressSet(relobject, RelationRelationId, indexRelId);
		add_exact_object_address(&relobject, addrs_normal);
	}

	if (foreignNKeys > 0)
	{
		/*
		 * 注册对支持外键约束的相等运算符的
		 * 普通依赖。若 PK 与 FK 类型相同，则
		 * 一列的三个运算符相同；否则它们
		 * 不同。
		 */
		ObjectAddress oprobject;

		oprobject.classId = OperatorRelationId;
		oprobject.objectSubId = 0;

		for (i = 0; i < foreignNKeys; i++)
		{
			oprobject.objectId = pfEqOp[i];
			add_exact_object_address(&oprobject, addrs_normal);
			if (ppEqOp[i] != pfEqOp[i])
			{
				oprobject.objectId = ppEqOp[i];
				add_exact_object_address(&oprobject, addrs_normal);
			}
			if (ffEqOp[i] != pfEqOp[i])
			{
				oprobject.objectId = ffEqOp[i];
				add_exact_object_address(&oprobject, addrs_normal);
			}
		}
	}

	record_object_address_dependencies(&conobject, addrs_normal,
									   DEPENDENCY_NORMAL);
	free_object_addresses(addrs_normal);

	/*
	 * 我们懒得注册对排他约束的排他运算符的
	 * 依赖。我们假定它们是支持该索引的操作符类的
	 * 成员，因此通过索引已有间接依赖。（对
	 * 跨类型运算符这会相当冒险，但排他运算符
	 * 绝不会是跨类型的。）
	 */

	if (conExpr != NULL)
	{
		/*
		 * 注册从约束到 CHECK 表达式中提及的对象的
		 * 依赖。
		 */
		recordDependencyOnSingleRelExpr(&conobject, conExpr, relId,
										DEPENDENCY_NORMAL,
										DEPENDENCY_NORMAL, false);
	}

	/* 新约束的创建后钩子 */
	InvokeObjectPostCreateHookArg(ConstraintRelationId, conOid, 0,
								  is_internal);

	return conOid;
}

/*
 * 测试给定名称当前是否被用作给定对象
 * （关系或域）的约束名。
 *
 * 这用于决定是否接受用户指定的约束名。
 * 它故意不同于 ChooseConstraintName 用于决定自动生成名是否
 * 可用的测试：这里，除非同一对象上已有相同约束名在用，
 * 否则我们都允许。
 *
 * 注意：调用方应对给定对象持有排他锁，否则
 * 此测试可能被并发添加愚弄。
 */
bool
ConstraintNameIsUsed(ConstraintCategory conCat, Oid objId,
					 const char *conname)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[3];

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum((conCat == CONSTRAINT_RELATION)
								 ? objId : InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum((conCat == CONSTRAINT_DOMAIN)
								 ? objId : InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	conscan = systable_beginscan(conDesc, ConstraintRelidTypidNameIndexId,
								 true, NULL, 3, skey);

	/* 最多只能有一行匹配 */
	found = (HeapTupleIsValid(systable_getnext(conscan)));

	systable_endscan(conscan);
	table_close(conDesc, AccessShareLock);

	return found;
}

/*
 * 给定命名空间中是否存在任何该名的约束？
 *
 * 这供希望匹配 ChooseConstraintName 规则的代码使用：
 * 我们应避免在命名空间内自动生成重复的
 * 约束名。
 */
bool
ConstraintNameExists(const char *conname, Oid namespaceid)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	ScanKeyInit(&skey[1],
				Anum_pg_constraint_connamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(namespaceid));

	conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
								 NULL, 2, skey);

	found = (HeapTupleIsValid(systable_getnext(conscan)));

	systable_endscan(conscan);
	table_close(conDesc, AccessShareLock);

	return found;
}

/*
 * 为新约束选择一个无冲突的名称。
 *
 * 此处目标是选择在指定命名空间内唯一的
 * 名称。Postgres 并不需要这一点，但 SQL
 * 规范需要，且有些应用依赖它。因此我们避免选择
 * 会如此冲突的默认名。
 *
 * name1、name2 与 label 的用法与 makeObjectName() 相同，
 * 只是 label 不能为 NULL；若需要在指定命名空间内
 * 生成唯一名，会在 label 后追加数字。
 *
 * 'others' 可以是当前命令内已选定（但尚未
 * 反映到目录中）的字符串名列表；我们也不会选择
 * 与其中之一重复的名称。
 *
 * 注意：理论上仍可能发生冲突，若有人
 * 并发选择了相同名称。实践中这不大可能成为
 * 问题，尤其是在对 name1 所标识的关系
 * 持有排他锁时。
 *
 * 返回 palloc 的字符串。
 */
char *
ChooseConstraintName(const char *name1, const char *name2,
					 const char *label, Oid namespaceid,
					 List *others)
{
	int			pass = 0;
	char	   *conname = NULL;
	char		modlabel[NAMEDATALEN];
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[2];
	bool		found;
	ListCell   *l;

	conDesc = table_open(ConstraintRelationId, AccessShareLock);

	/* 先尝试未修改的 label */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		conname = makeObjectName(name1, name2, modlabel);

		found = false;

		foreach(l, others)
		{
			if (strcmp((char *) lfirst(l), conname) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			ScanKeyInit(&skey[0],
						Anum_pg_constraint_conname,
						BTEqualStrategyNumber, F_NAMEEQ,
						CStringGetDatum(conname));

			ScanKeyInit(&skey[1],
						Anum_pg_constraint_connamespace,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(namespaceid));

			conscan = systable_beginscan(conDesc, ConstraintNameNspIndexId, true,
										 NULL, 2, skey);

			found = (HeapTupleIsValid(systable_getnext(conscan)));

			systable_endscan(conscan);
		}

		if (!found)
			break;

		/* 发现冲突，因此尝试新的名称分量 */
		pfree(conname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	table_close(conDesc, AccessShareLock);

	return conname;
}

/*
 * 删除单条约束记录。
 */
void
RemoveConstraintById(Oid conId)
{
	Relation	conDesc;
	HeapTuple	tup;
	Form_pg_constraint con;

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conId));
	if (!HeapTupleIsValid(tup)) /* 不应发生 */
		elog(ERROR, "cache lookup failed for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tup);

	/*
	 * 按约束所针对对象做特殊处理。
	 */
	if (OidIsValid(con->conrelid))
	{
		Relation	rel;

		/*
		 * 若约束针对关系，则打开并对其所针对的关系加
		 * 排他锁。
		 */
		rel = table_open(con->conrelid, AccessExclusiveLock);

		/*
		 * 若正在删除检查约束，我们需要更新 relchecks 计数。
		 * 此更新将迫使各后端在我们提交时重建
		 * relcache 条目。
		 */
		if (con->contype == CONSTRAINT_CHECK)
		{
			Relation	pgrel;
			HeapTuple	relTup;
			Form_pg_class classForm;

			pgrel = table_open(RelationRelationId, RowExclusiveLock);
			relTup = SearchSysCacheCopy1(RELOID,
										 ObjectIdGetDatum(con->conrelid));
			if (!HeapTupleIsValid(relTup))
				elog(ERROR, "cache lookup failed for relation %u",
					 con->conrelid);
			classForm = (Form_pg_class) GETSTRUCT(relTup);

			if (classForm->relchecks == 0)	/* 不应发生 */
				elog(ERROR, "relation \"%s\" has relchecks = 0",
					 RelationGetRelationName(rel));
			classForm->relchecks--;

			CatalogTupleUpdate(pgrel, &relTup->t_self, relTup);

			heap_freetuple(relTup);

			table_close(pgrel, RowExclusiveLock);
		}

		/* 保持对约束所属关系的锁直至事务结束 */
		table_close(rel, NoLock);
	}
	else if (OidIsValid(con->contypid))
	{
		/*
		 * XXX 目前，删除域约束时不做特殊处理
		 *
		 * 大概应对域类型有某种形式的加锁，
		 * 但我们目前没有这样的概念。
		 */
	}
	else
		elog(ERROR, "constraint %u is not of a known type", conId);

	/* 干掉约束本身 */
	CatalogTupleDelete(conDesc, &tup->t_self);

	/* 清理 */
	ReleaseSysCache(tup);
	table_close(conDesc, RowExclusiveLock);
}

/*
 * RenameConstraintById
 *		重命名约束。
 *
 * 注意：这并非打算作为用户暴露的函数；它不检查
 * 权限等。目前仅在重命名与约束关联的
 * 索引时调用，但做得稍通用一些，以期将来有
 * ALTER TABLE RENAME
 * CONSTRAINT。
 */
void
RenameConstraintById(Oid conId, const char *newname)
{
	Relation	conDesc;
	HeapTuple	tuple;
	Form_pg_constraint con;

	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(CONSTROID, ObjectIdGetDatum(conId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u", conId);
	con = (Form_pg_constraint) GETSTRUCT(tuple);

	/*
	 * 为用户友好起见，检查该名称是否已被使用。
	 */
	if (OidIsValid(con->conrelid) &&
		ConstraintNameIsUsed(CONSTRAINT_RELATION,
							 con->conrelid,
							 newname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for relation \"%s\" already exists",
						newname, get_rel_name(con->conrelid))));
	if (OidIsValid(con->contypid) &&
		ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
							 con->contypid,
							 newname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("constraint \"%s\" for domain %s already exists",
						newname, format_type_be(con->contypid))));

	/* 好，执行重命名 --- 元组是副本，可在上面涂改 */
	namestrcpy(&(con->conname), newname);

	CatalogTupleUpdate(conDesc, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, conId, 0);

	heap_freetuple(tuple);
	table_close(conDesc, RowExclusiveLock);
}

/*
 * AlterConstraintNamespaces
 *		查找属于指定对象的任何约束，
 *		并将它们移到指定的新命名空间。
 *
 * isType 指示所属对象是类型还是关系。
 */
void
AlterConstraintNamespaces(Oid ownerId, Oid oldNspId,
						  Oid newNspId, bool isType, ObjectAddresses *objsMoved)
{
	Relation	conRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	conRel = table_open(ConstraintRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(isType ? InvalidOid : ownerId));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(isType ? ownerId : InvalidOid));

	scan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(tup);
		ObjectAddress thisobj;

		ObjectAddressSet(thisobj, ConstraintRelationId, conform->oid);

		if (object_address_present(&thisobj, objsMoved))
			continue;

		/* 若对象已属于该命名空间则不更新 */
		if (conform->connamespace == oldNspId && oldNspId != newNspId)
		{
			tup = heap_copytuple(tup);
			conform = (Form_pg_constraint) GETSTRUCT(tup);

			conform->connamespace = newNspId;

			CatalogTupleUpdate(conRel, &tup->t_self, tup);

			/*
			 * 注意：目前，约束不会对自己的命名空间有
			 * 依赖，因此我们不需要做
			 * changeDependencyFor()。
			 */
		}

		InvokeObjectPostAlterHook(ConstraintRelationId, thisobj.objectId, 0);

		add_exact_object_address(&thisobj, objsMoved);
	}

	systable_endscan(scan);

	table_close(conRel, RowExclusiveLock);
}

/*
 * ConstraintSetParentConstraint
 *		将分区的约束设为其父约束的子约束，
 *		或者若 parentConstrId 为 InvalidOid 则移除该链接。
 *
 * 这会更新约束的 pg_constraint 行以显示其为继承而来，并
 * 添加 PARTITION 依赖以防止约束被单独删除。
 * 或者反过来做。
 */
void
ConstraintSetParentConstraint(Oid childConstrId,
							  Oid parentConstrId,
							  Oid childTableId)
{
	Relation	constrRel;
	Form_pg_constraint constrForm;
	HeapTuple	tuple,
				newtup;
	ObjectAddress depender;
	ObjectAddress referenced;

	constrRel = table_open(ConstraintRelationId, RowExclusiveLock);
	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(childConstrId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u", childConstrId);
	newtup = heap_copytuple(tuple);
	constrForm = (Form_pg_constraint) GETSTRUCT(newtup);
	if (OidIsValid(parentConstrId))
	{
		/* 不允许为已有父约束的约束设置父约束 */
		Assert(constrForm->coninhcount == 0);
		if (constrForm->conparentid != InvalidOid)
			elog(ERROR, "constraint %u already has a parent constraint",
				 childConstrId);

		constrForm->conislocal = false;
		constrForm->coninhcount++;
		if (constrForm->coninhcount < 0)
			ereport(ERROR,
					errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("too many inheritance parents"));
		constrForm->conparentid = parentConstrId;

		CatalogTupleUpdate(constrRel, &tuple->t_self, newtup);

		ObjectAddressSet(depender, ConstraintRelationId, childConstrId);

		ObjectAddressSet(referenced, ConstraintRelationId, parentConstrId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_PRI);

		ObjectAddressSet(referenced, RelationRelationId, childTableId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_SEC);
	}
	else
	{
		constrForm->coninhcount--;
		constrForm->conislocal = true;
		constrForm->conparentid = InvalidOid;

		/* 确保没有进一步的继承。 */
		Assert(constrForm->coninhcount == 0);

		CatalogTupleUpdate(constrRel, &tuple->t_self, newtup);

		deleteDependencyRecordsForClass(ConstraintRelationId, childConstrId,
										ConstraintRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(ConstraintRelationId, childConstrId,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);
	}

	ReleaseSysCache(tuple);
	table_close(constrRel, RowExclusiveLock);
}


/*
 * get_relation_constraint_oid
 *		在指定关系上查找指定名称的约束。
 *		返回约束的 OID。
 */
Oid
get_relation_constraint_oid(Oid relid, const char *conname, bool missing_ok)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[3];
	Oid			conOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* 最多只能有一行匹配 */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		conOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

	systable_endscan(scan);

	/* 若不存在此类约束，则报错 */
	if (!OidIsValid(conOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for table \"%s\" does not exist",
						conname, get_rel_name(relid))));

	table_close(pg_constraint, AccessShareLock);

	return conOid;
}

/*
 * get_relation_constraint_attnos
 *		在指定关系上查找指定名称的约束，
 *		并返回受约束的列。
 *
 * 返回受约束列的 attnos 的 Bitmapset，其中
 * attnos 按 FirstLowInvalidHeapAttributeNumber 偏移，以便能表示
 * 系统列。
 *
 * *constraintOid 设为约束的 OID，失败时为
 * InvalidOid。
 */
Bitmapset *
get_relation_constraint_attnos(Oid relid, const char *conname,
							   bool missing_ok, Oid *constraintOid)
{
	Bitmapset  *conattnos = NULL;
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[3];

	/* 设置 *constraintOid，以免抱怨未初始化变量 */
	*constraintOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* 最多只能有一行匹配 */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Datum		adatum;
		bool		isNull;

		*constraintOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

		/* 提取 conkey 数组，即受约束列的 attnums */
		adatum = heap_getattr(tuple, Anum_pg_constraint_conkey,
							  RelationGetDescr(pg_constraint), &isNull);
		if (!isNull)
		{
			ArrayType  *arr;
			int			numcols;
			int16	   *attnums;
			int			i;

			arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
			numcols = ARR_DIMS(arr)[0];
			if (ARR_NDIM(arr) != 1 ||
				numcols < 0 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != INT2OID)
				elog(ERROR, "conkey is not a 1-D smallint array");
			attnums = (int16 *) ARR_DATA_PTR(arr);

			/* 构造结果值 */
			for (i = 0; i < numcols; i++)
			{
				conattnos = bms_add_member(conattnos,
										   attnums[i] - FirstLowInvalidHeapAttributeNumber);
			}
		}
	}

	systable_endscan(scan);

	/* 若不存在此类约束，则报错 */
	if (!OidIsValid(*constraintOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for table \"%s\" does not exist",
						conname, get_rel_name(relid))));

	table_close(pg_constraint, AccessShareLock);

	return conattnos;
}

/*
 * 返回给定关系中由给定索引强制实施的约束的 OID；
 * 若未编目此类索引则返回 InvalidOid。
 *
 * 与 get_constraint_index 很像，本函数只关心
 * "拥有"给定索引的那一个约束。因此，忽略
 * 唯一、主键与排他以外类型的约束。
 */
Oid
get_relation_idx_constraint_oid(Oid relationId, Oid indexId)
{
	Relation	pg_constraint;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	Oid			constraintId = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relationId));
	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, &key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm;

		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* 见上文 */
		if (constrForm->contype != CONSTRAINT_PRIMARY &&
			constrForm->contype != CONSTRAINT_UNIQUE &&
			constrForm->contype != CONSTRAINT_EXCLUSION)
			continue;

		if (constrForm->conindid == indexId)
		{
			constraintId = constrForm->oid;
			break;
		}
	}
	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);
	return constraintId;
}

/*
 * get_domain_constraint_oid
 *		在指定域上查找指定名称的约束。
 *		返回约束的 OID。
 */
Oid
get_domain_constraint_oid(Oid typid, const char *conname, bool missing_ok)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[3];
	Oid			conOid = InvalidOid;

	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(conname));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* 最多只能有一行匹配 */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
		conOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

	systable_endscan(scan);

	/* 若不存在此类约束，则报错 */
	if (!OidIsValid(conOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" for domain %s does not exist",
						conname, format_type_be(typid))));

	table_close(pg_constraint, AccessShareLock);

	return conOid;
}

/*
 * get_primary_key_attnos
 *		识别关系主键中的列（若有）。
 *
 * 返回主键列的 attnos 的 Bitmapset，
 * 其中 attnos 按 FirstLowInvalidHeapAttributeNumber 偏移，以便能表示
 * 系统列。
 *
 * 若没有主键则返回 NULL。若主键约束可延迟且
 * deferrableOk 为 false，我们也返回 NULL。
 *
 * *constraintOid 设为主键约束的 OID，失败时为
 * InvalidOid。
 */
Bitmapset *
get_primary_key_attnos(Oid relid, bool deferrableOk, Oid *constraintOid)
{
	Bitmapset  *pkattnos = NULL;
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/* 设置 *constraintOid，以免抱怨未初始化变量 */
	*constraintOid = InvalidOid;

	/* 扫描 pg_constraint 查找目标关系的约束 */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
		Datum		adatum;
		bool		isNull;
		ArrayType  *arr;
		int16	   *attnums;
		int			numkeys;
		int			i;

		/* 跳过不是 PRIMARY KEY 的约束 */
		if (con->contype != CONSTRAINT_PRIMARY)
			continue;

		/*
		 * 若主键可延迟，但我们被指示忽略可延迟约束，
		 * 则可就此放弃搜索，因为一张表上只能有
		 * 一个主键。
		 */
		if (con->condeferrable && !deferrableOk)
			break;

		/* 提取 conkey 数组，即主键列的 attnums */
		adatum = heap_getattr(tuple, Anum_pg_constraint_conkey,
							  RelationGetDescr(pg_constraint), &isNull);
		if (isNull)
			elog(ERROR, "null conkey for constraint %u",
				 ((Form_pg_constraint) GETSTRUCT(tuple))->oid);
		arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
		numkeys = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numkeys < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != INT2OID)
			elog(ERROR, "conkey is not a 1-D smallint array");
		attnums = (int16 *) ARR_DATA_PTR(arr);

		/* 构造结果值 */
		for (i = 0; i < numkeys; i++)
		{
			pkattnos = bms_add_member(pkattnos,
									  attnums[i] - FirstLowInvalidHeapAttributeNumber);
		}
		*constraintOid = ((Form_pg_constraint) GETSTRUCT(tuple))->oid;

		/* 无需继续搜索 */
		break;
	}

	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);

	return pkattnos;
}

/*
 * 从外键约束的 pg_constraint 元组提取数据。
 *
 * 除第一个外，所有参数都是输出参数。除
 * numfks、conkey 与 confkey 外的所有输出参数，若调用方
 * 不需要可传 NULL。
 */
void
DeconstructFkConstraintRow(HeapTuple tuple, int *numfks,
						   AttrNumber *conkey, AttrNumber *confkey,
						   Oid *pf_eq_oprs, Oid *pp_eq_oprs, Oid *ff_eq_oprs,
						   int *num_fk_del_set_cols, AttrNumber *fk_del_set_cols)
{
	Datum		adatum;
	bool		isNull;
	ArrayType  *arr;
	int			numkeys;

	/*
	 * 我们期望这些数组为正确类型的一维数组；验证这一点。
	 * 我们不需要使用 deconstruct_array()，因为数组数据看起来就
	 * 像值的 C 数组。
	 */
	adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
									Anum_pg_constraint_conkey);
	arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != INT2OID)
		elog(ERROR, "conkey is not a 1-D smallint array");
	numkeys = ARR_DIMS(arr)[0];
	if (numkeys <= 0 || numkeys > INDEX_MAX_KEYS)
		elog(ERROR, "foreign key constraint cannot have %d columns", numkeys);
	memcpy(conkey, ARR_DATA_PTR(arr), numkeys * sizeof(int16));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* 释放去 toast 后的副本（若有） */

	adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
									Anum_pg_constraint_confkey);
	arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
	if (ARR_NDIM(arr) != 1 ||
		ARR_DIMS(arr)[0] != numkeys ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != INT2OID)
		elog(ERROR, "confkey is not a 1-D smallint array");
	memcpy(confkey, ARR_DATA_PTR(arr), numkeys * sizeof(int16));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* 释放去 toast 后的副本（若有） */

	if (pf_eq_oprs)
	{
		adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
										Anum_pg_constraint_conpfeqop);
		arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
		/* 若更改下面的测试，请参见 TryReuseForeignKey */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numkeys ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "conpfeqop is not a 1-D Oid array");
		memcpy(pf_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
		if ((Pointer) arr != DatumGetPointer(adatum))
			pfree(arr);			/* 释放去 toast 后的副本（若有） */
	}

	if (pp_eq_oprs)
	{
		adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
										Anum_pg_constraint_conppeqop);
		arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numkeys ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "conppeqop is not a 1-D Oid array");
		memcpy(pp_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
		if ((Pointer) arr != DatumGetPointer(adatum))
			pfree(arr);			/* 释放去 toast 后的副本（若有） */
	}

	if (ff_eq_oprs)
	{
		adatum = SysCacheGetAttrNotNull(CONSTROID, tuple,
										Anum_pg_constraint_conffeqop);
		arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numkeys ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "conffeqop is not a 1-D Oid array");
		memcpy(ff_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
		if ((Pointer) arr != DatumGetPointer(adatum))
			pfree(arr);			/* 释放去 toast 后的副本（若有） */
	}

	if (fk_del_set_cols)
	{
		adatum = SysCacheGetAttr(CONSTROID, tuple,
								 Anum_pg_constraint_confdelsetcols, &isNull);
		if (isNull)
		{
			*num_fk_del_set_cols = 0;
		}
		else
		{
			int			num_delete_cols;

			arr = DatumGetArrayTypeP(adatum);	/* 确保未被 toast */
			if (ARR_NDIM(arr) != 1 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != INT2OID)
				elog(ERROR, "confdelsetcols is not a 1-D smallint array");
			num_delete_cols = ARR_DIMS(arr)[0];
			memcpy(fk_del_set_cols, ARR_DATA_PTR(arr), num_delete_cols * sizeof(int16));
			if ((Pointer) arr != DatumGetPointer(adatum))
				pfree(arr);		/* 释放去 toast 后的副本（若有） */

			*num_fk_del_set_cols = num_delete_cols;
		}
	}

	*numfks = numkeys;
}

/*
 * 判定能否证明关系函数依赖于
 * 一组分组列。若能，返回 true，并把证明所需约束的
 * pg_constraint OID 加入 *constraintDeps 列表。
 *
 * grouping_columns 是分组表达式列表，其中感兴趣关系的
 * 列是带有所示 varno/varlevelsup 的 Vars。
 *
 * 目前我们只检查关系是否有作为 grouping_columns
 * 子集的主键。我们也可以使用普通唯一约束，
 * 若其所有列已知非空，但有个问题：我们需要
 * 能够把非空性表示为加入 *constraintDeps 的约束的一部分。
 * 待 pg_constraint 能表示非空约束时再
 * FIXME。
 */
bool
check_functional_grouping(Oid relid,
						  Index varno, Index varlevelsup,
						  List *grouping_columns,
						  List **constraintDeps)
{
	Bitmapset  *pkattnos;
	Bitmapset  *groupbyattnos;
	Oid			constraintOid;
	ListCell   *gl;

	/* 若关系没有主键，则无法证明函数依赖 */
	pkattnos = get_primary_key_attnos(relid, false, &constraintOid);
	if (pkattnos == NULL)
		return false;

	/* 识别出现在 grouping_columns 中的所有该关系的列 */
	groupbyattnos = NULL;
	foreach(gl, grouping_columns)
	{
		Var		   *gvar = (Var *) lfirst(gl);

		if (IsA(gvar, Var) &&
			gvar->varno == varno &&
			gvar->varlevelsup == varlevelsup)
			groupbyattnos = bms_add_member(groupbyattnos,
										   gvar->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	if (bms_is_subset(pkattnos, groupbyattnos))
	{
		/* 主键是 grouping_columns 的子集，因此我们成功 */
		*constraintDeps = lappend_oid(*constraintDeps, constraintOid);
		return true;
	}

	return false;
}
