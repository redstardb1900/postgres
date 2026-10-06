/*-------------------------------------------------------------------------
 *
 * pg_depend.c
 *	  支持操作 pg_depend 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_depend.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


static bool isObjectPinned(const ObjectAddress *object);


/*
 * 通过各自的 objectAddress 记录两个对象之间的依赖。
 * 第一个参数是依赖方对象，第二个是它所
 * 引用的对象。
 *
 * 这只是在 pg_depend 中创建一条条目，不做其他处理。
 */
void
recordDependencyOn(const ObjectAddress *depender,
				   const ObjectAddress *referenced,
				   DependencyType behavior)
{
	recordMultipleDependencies(depender, referenced, 1, behavior);
}

/*
 * 为单个依赖方对象记录多条（同种）依赖。
 * 这比逐条记录开销稍小。
 */
void
recordMultipleDependencies(const ObjectAddress *depender,
						   const ObjectAddress *referenced,
						   int nreferenced,
						   DependencyType behavior)
{
	Relation	dependDesc;
	CatalogIndexState indstate;
	TupleTableSlot **slot;
	int			i,
				max_slots,
				slot_init_count,
				slot_stored_count;

	if (nreferenced <= 0)
		return;					/* 无事可做 */

	/*
	 * 引导期间什么也不做，因为 pg_depend 可能尚不存在。
	 *
	 * 引导期间创建的对象大多被钉住，而未钉住的
	 * 少数彼此没有依赖，因此反正
	 * 也无需制作 pg_depend 条目。
	 */
	if (IsBootstrapProcessingMode())
		return;

	dependDesc = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * 分配将要用的槽，但推迟昂贵的初始化，直到
	 * 知道它们会被使用。
	 */
	max_slots = Min(nreferenced,
					MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_depend));
	slot = palloc(sizeof(TupleTableSlot *) * max_slots);

	/* 除非需要做更新，否则不要打开索引 */
	indstate = NULL;

	/* 当前存放着元组的槽数 */
	slot_stored_count = 0;
	/* 当前已初始化的槽数 */
	slot_init_count = 0;
	for (i = 0; i < nreferenced; i++, referenced++)
	{
		/*
		 * 若被引用对象被系统钉住，就没有真正必要
		 * 记录对它的依赖。这能节省 pg_depend 中大量
		 * 空间，因此值得花时间检查。
		 */
		if (isObjectPinned(referenced))
			continue;

		if (slot_init_count < max_slots)
		{
			slot[slot_stored_count] = MakeSingleTupleTableSlot(RelationGetDescr(dependDesc),
															   &TTSOpsHeapTuple);
			slot_init_count++;
		}

		ExecClearTuple(slot[slot_stored_count]);

		/*
		 * 记录依赖。注意我们不费心检查重复
		 * 依赖；有它们也无害。
		 */
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refclassid - 1] = ObjectIdGetDatum(referenced->classId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refobjid - 1] = ObjectIdGetDatum(referenced->objectId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_refobjsubid - 1] = Int32GetDatum(referenced->objectSubId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_deptype - 1] = CharGetDatum((char) behavior);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_classid - 1] = ObjectIdGetDatum(depender->classId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_objid - 1] = ObjectIdGetDatum(depender->objectId);
		slot[slot_stored_count]->tts_values[Anum_pg_depend_objsubid - 1] = Int32GetDatum(depender->objectSubId);

		memset(slot[slot_stored_count]->tts_isnull, false,
			   slot[slot_stored_count]->tts_tupleDescriptor->natts * sizeof(bool));

		ExecStoreVirtualTuple(slot[slot_stored_count]);
		slot_stored_count++;

		/* 若槽已满，则插入一批元组 */
		if (slot_stored_count == max_slots)
		{
			/* 仅在知道需要时才取索引信息 */
			if (indstate == NULL)
				indstate = CatalogOpenIndexes(dependDesc);

			CatalogTuplesMultiInsertWithInfo(dependDesc, slot, slot_stored_count,
											 indstate);
			slot_stored_count = 0;
		}
	}

	/* 插入缓冲区中剩余的任何元组 */
	if (slot_stored_count > 0)
	{
		/* 仅在知道需要时才取索引信息 */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(dependDesc);

		CatalogTuplesMultiInsertWithInfo(dependDesc, slot, slot_stored_count,
										 indstate);
	}

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);

	table_close(dependDesc, RowExclusiveLock);

	/* 只丢弃已使用的槽数 */
	for (i = 0; i < slot_init_count; i++)
		ExecDropSingleTupleTableSlot(slot[i]);
	pfree(slot);
}

/*
 * 若正在执行 CREATE EXTENSION 操作，则将给定对象
 * 标记为该扩展的成员，或检查它已经是成员。
 * 否则什么也不做。
 *
 * 在创建任何可能成为扩展成员的用户可定义
 * 对象类型时，必须调用本函数。
 *
 * 若对象已经存在则 isReplace 必须为真，若是新创建的
 * 则为假。前一种情形我们坚持它已经是当前扩展的
 * 成员。后一种情形可跳过检查它是否已是
 * 任何扩展的成员。
 *
 * Note: isReplace = true 通常用于在 CREATE OR REPLACE
 * 及类似命令中更新对象。我们曾允许目标对象
 * 尚不是扩展成员，而是默默把它吸收进
 * 当前扩展。然而这既容易出错
 * （扩展可能意外覆盖独立对象），也是
 * 安全隐患（因为对象会保留其先前的所有权）。
 */
void
recordDependencyOnCurrentExtension(const ObjectAddress *object,
								   bool isReplace)
{
	/* 只有完整对象才能成为扩展成员 */
	Assert(object->objectSubId == 0);

	if (creating_extension)
	{
		ObjectAddress extension;

		/* 仅当 isReplace 时才需要检查已有成员关系 */
		if (isReplace)
		{
			Oid			oldext;

			/*
			 * 旁注：这些目录查找仅因对象是已存在的
			 * 对象才安全。在非 isReplace 情形下，调用方
			 * 多半尚未做 CommandCounterIncrement，
			 * 因而新对象尚不可见。
			 */
			oldext = getExtensionOfObject(object->classId, object->objectId);
			if (OidIsValid(oldext))
			{
				/* 若已是本扩展成员，则无事可做 */
				if (oldext == CurrentExtensionObject)
					return;
				/* 已是其他某扩展的成员，因此拒绝 */
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("%s is already a member of extension \"%s\"",
								getObjectDescription(object, false),
								get_extension_name(oldext))));
			}
			/* 这是独立对象，因此拒绝 */
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is not a member of extension \"%s\"",
							getObjectDescription(object, false),
							get_extension_name(CurrentExtensionObject)),
					 errdetail("An extension is not allowed to replace an object that it does not own.")));
		}

		/* 好，把它记录为 CurrentExtensionObject 的成员 */
		extension.classId = ExtensionRelationId;
		extension.objectId = CurrentExtensionObject;
		extension.objectSubId = 0;

		recordDependencyOn(object, &extension, DEPENDENCY_EXTENSION);
	}
}

/*
 * 若正在执行 CREATE EXTENSION 操作，检查给定对象
 * 是否为该扩展的成员；若不是则抛错。
 * 否则什么也不做。
 *
 * 每当 CREATE IF NOT EXISTS 操作（针对可成为
 * 扩展成员的对象类型）发现所需名称的对象
 * 已存在时，必须调用本函数。扩展使用
 * IF NOT EXISTS 是不安全的，除非冲突对象已经是扩展
 * 成员；否则怀有敌意的用户可能用任意属性的
 * 对象顶替。
 */
void
checkMembershipInCurrentExtension(const ObjectAddress *object)
{
	/*
	 * 这实际上与 recordDependencyOnCurrentExtension 中测试的
	 * 条件相同；但我们想发出措辞不同的
	 * 错误，而且在这些情况下调用
	 * recordDependencyOnCurrentExtension 会相当令人困惑。
	 */

	/* 只有完整对象才能成为扩展成员 */
	Assert(object->objectSubId == 0);

	if (creating_extension)
	{
		Oid			oldext;

		oldext = getExtensionOfObject(object->classId, object->objectId);
		/* 若已是本扩展成员，则 OK */
		if (oldext == CurrentExtensionObject)
			return;
		/* 否则抱怨 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s is not a member of extension \"%s\"",
						getObjectDescription(object, false),
						get_extension_name(CurrentExtensionObject)),
				 errdetail("An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.")));
	}
}

/*
 * deleteDependencyRecordsFor -- 删除 depender 为给定
 * classId/objectId 的所有记录。返回所删记录数。
 *
 * 在重新定义已有对象时使用。指向该对象的
 * 链接不变，而从它出发的链接将被重建
 * （可能与之前有些不同）。
 *
 * 若 skipExtensionDeps 为真，我们不删除表明给定对象
 * 是扩展成员的任何依赖。这避免了需要大量
 * 额外逻辑去取回并重建该依赖。
 */
long
deleteDependencyRecordsFor(Oid classId, Oid objectId,
						   bool skipExtensionDeps)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		if (skipExtensionDeps &&
			((Form_pg_depend) GETSTRUCT(tup))->deptype == DEPENDENCY_EXTENSION)
			continue;

		CatalogTupleDelete(depRel, &tup->t_self);
		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * deleteDependencyRecordsForClass -- 删除 depender 为给定
 * classId/objectId、dependee 为给定 classId，且 deptype 匹配的所有记录。
 * 返回所删记录数。
 *
 * 这是 deleteDependencyRecordsFor 的变体，适用于撤销由
 * 依赖记录表达的对象属性（例如
 * 扩展成员关系）。
 */
long
deleteDependencyRecordsForClass(Oid classId, Oid objectId,
								Oid refclassId, char deptype)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refclassId && depform->deptype == deptype)
		{
			CatalogTupleDelete(depRel, &tup->t_self);
			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * deleteDependencyRecordsForSpecific -- 删除 depender 为给定
 * classId/objectId、dependee 为给定 classId/objectId，且 deptype 匹配的所有记录。
 * 返回所删记录数。
 */
long
deleteDependencyRecordsForSpecific(Oid classId, Oid objectId, char deptype,
								   Oid refclassId, Oid refobjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refclassId &&
			depform->refobjid == refobjectId &&
			depform->deptype == deptype)
		{
			CatalogTupleDelete(depRel, &tup->t_self);
			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * 调整 dependency record(s)，使其指向同类型的另一对象
 *
 * classId/objectId 指定引用方对象。
 * refClassId/oldRefObjectId 指定旧的被引用对象。
 * newRefObjectId 是新的被引用对象（必须属于 class refClassId）。
 *
 * 注意没有 objsubid 参数。若存在子对象引用，
 * 它们都会被重新调整。另外，我们预期处理的是
 * NORMAL 依赖：若必须把对钉住对象的（隐式）
 * 依赖替换为对未钉住对象的显式依赖，
 * 新的依赖将是 NORMAL。
 *
 * 返回所更新的记录数 —— 零表示有问题。
 */
long
changeDependencyFor(Oid classId, Oid objectId,
					Oid refClassId, Oid oldRefObjectId,
					Oid newRefObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress objAddr;
	ObjectAddress depAddr;
	bool		oldIsPinned;
	bool		newIsPinned;

	/*
	 * 检查 oldRefObjectId 或 newRefObjectId 是否被钉住。
	 * 钉住对象不应有任何指向它们的依赖条目，
	 * 因此在这些情形下我们应添加或删除 pg_depend 条目，或
	 * 什么也不做，而不是像正常情形那样更新条目。
	 */
	objAddr.classId = refClassId;
	objAddr.objectId = oldRefObjectId;
	objAddr.objectSubId = 0;

	oldIsPinned = isObjectPinned(&objAddr);

	objAddr.objectId = newRefObjectId;

	newIsPinned = isObjectPinned(&objAddr);

	if (oldIsPinned)
	{
		/*
		 * 若两者都被钉住，我们无需做任何事。然而返回 1 而非 0，
		 * 否则调用方会认为这是错误情形。
		 */
		if (newIsPinned)
			return 1;

		/*
		 * 没有旧的依赖记录，但应插入一条新的。
		 * 假定需要的是普通依赖。
		 */
		depAddr.classId = classId;
		depAddr.objectId = objectId;
		depAddr.objectSubId = 0;
		recordDependencyOn(&depAddr, &objAddr, DEPENDENCY_NORMAL);

		return 1;
	}

	depRel = table_open(DependRelationId, RowExclusiveLock);

	/* 应存在 dependency record(s)，因此搜索。 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == refClassId &&
			depform->refobjid == oldRefObjectId)
		{
			if (newIsPinned)
				CatalogTupleDelete(depRel, &tup->t_self);
			else
			{
				/* 制作可修改的副本 */
				tup = heap_copytuple(tup);
				depform = (Form_pg_depend) GETSTRUCT(tup);

				depform->refobjid = newRefObjectId;

				CatalogTupleUpdate(depRel, &tup->t_self, tup);

				heap_freetuple(tup);
			}

			count++;
		}
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * 调整所有依赖记录，使其来自同类型的另一对象
 *
 * classId/oldObjectId 指定旧的引用方对象。
 * newObjectId 是新的引用方对象（必须属于 class classId）。
 *
 * 返回所更新的记录数。
 */
long
changeDependenciesOf(Oid classId, Oid oldObjectId,
					 Oid newObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldObjectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform;

		/* 制作可修改的副本 */
		tup = heap_copytuple(tup);
		depform = (Form_pg_depend) GETSTRUCT(tup);

		depform->objid = newObjectId;

		CatalogTupleUpdate(depRel, &tup->t_self, tup);

		heap_freetuple(tup);

		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * 调整所有依赖记录，使其指向同类型的另一对象
 *
 * refClassId/oldRefObjectId 指定旧的被引用对象。
 * newRefObjectId 是新的被引用对象（必须属于 class refClassId）。
 *
 * 返回所更新的记录数。
 */
long
changeDependenciesOn(Oid refClassId, Oid oldRefObjectId,
					 Oid newRefObjectId)
{
	long		count = 0;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress objAddr;
	bool		newIsPinned;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * 若 oldRefObjectId 被钉住，就不会有指向它的依赖条目
	 * —— 那种情形我们无法处理。（以当前用法，不值得
	 * 花代码去修；这只意味着你不能把东西重命名出
	 * pg_catalog，而那本来多半也是糟糕的举动。）
	 */
	objAddr.classId = refClassId;
	objAddr.objectId = oldRefObjectId;
	objAddr.objectSubId = 0;

	if (isObjectPinned(&objAddr))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot remove dependency on %s because it is a system object",
						getObjectDescription(&objAddr, false))));

	/*
	 * 不过我们可以处理添加对钉住对象的依赖，因为
	 * 那只意味着删除依赖条目。
	 */
	objAddr.objectId = newRefObjectId;

	newIsPinned = isObjectPinned(&objAddr);

	/* 现在搜索依赖记录 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(refClassId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oldRefObjectId));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		if (newIsPinned)
			CatalogTupleDelete(depRel, &tup->t_self);
		else
		{
			Form_pg_depend depform;

			/* 制作可修改的副本 */
			tup = heap_copytuple(tup);
			depform = (Form_pg_depend) GETSTRUCT(tup);

			depform->refobjid = newRefObjectId;

			CatalogTupleUpdate(depRel, &tup->t_self, tup);

			heap_freetuple(tup);
		}

		count++;
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	return count;
}

/*
 * isObjectPinned()
 *
 * 测试某对象是否为基本数据库功能所必需。
 *
 * 传入的 subId（若有）被忽略；我们假定只有完整对象
 * 会被钉住（且这意味着钉住其组成部分）。
 */
static bool
isObjectPinned(const ObjectAddress *object)
{
	return IsPinnedObject(object->classId, object->objectId);
}


/*
 * 对 pg_depend 的各种专用查找与操作。
 */


/*
 * 查找包含指定对象的扩展（若有）
 *
 * 返回扩展的 OID；若对象不属于任何扩展则返回
 * InvalidOid。
 *
 * 扩展成员关系由从对象到扩展的 EXTENSION
 * 依赖标记。注意若 pg_depend 含有从本对象到
 * 多个扩展的链接，结果将不确定……但那
 * 绝不应该发生。
 */
Oid
getExtensionOfObject(Oid classId, Oid objectId)
{
	Oid			result = InvalidOid;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == ExtensionRelationId &&
			depform->deptype == DEPENDENCY_EXTENSION)
		{
			result = depform->refobjid;
			break;				/* 无需继续扫描 */
		}
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return result;
}

/*
 * 返回给定对象以 DEPENDENCY_AUTO_EXTENSION 模式所依赖的
 * 扩展列表（可能为 NIL）。
 */
List *
getAutoExtensionsOfObject(Oid classId, Oid objectId)
{
	List	   *result = NIL;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == ExtensionRelationId &&
			depform->deptype == DEPENDENCY_AUTO_EXTENSION)
			result = lappend_oid(result, depform->refobjid);
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return result;
}

/*
 * 检测序列是否被标记为某列"拥有"
 *
 * 拥有标记是从序列到该列的 AUTO 或 INTERNAL
 * 依赖。若找到，把拥有列的身份存入
 * *tableId 与 *colId 并返回 true；否则返回 false。
 *
 * Note: 若有多条这样的 pg_depend 条目，则输出参数会得到
 * 其中随机的一条。不过这不应当
 * 发生。
 */
bool
sequenceIsOwned(Oid seqId, char deptype, Oid *tableId, int32 *colId)
{
	bool		ret = false;
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(seqId));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == RelationRelationId &&
			depform->deptype == deptype)
		{
			*tableId = depform->refobjid;
			*colId = depform->refobjsubid;
			ret = true;
			break;				/* 无需继续扫描 */
		}
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return ret;
}

/*
 * 收集由指定关系（及若指定则由列）拥有的所有
 * 序列的 OIDs 列表。若 deptype 非零，则只查找具有
 * 指定依赖类型的序列。
 */
static List *
getOwnedSequences_internal(Oid relid, AttrNumber attnum, char deptype)
{
	List	   *result = NIL;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	if (attnum)
		ScanKeyInit(&key[2],
					Anum_pg_depend_refobjsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, attnum ? 3 : 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * 我们假定序列对列的任何 auto 或 internal 依赖
		 * 都是我们所要找的。（需要 relkind 测试是因为
		 * 索引也可能对列有 auto 依赖。）
		 */
		if (deprec->classid == RelationRelationId &&
			deprec->objsubid == 0 &&
			deprec->refobjsubid != 0 &&
			(deprec->deptype == DEPENDENCY_AUTO || deprec->deptype == DEPENDENCY_INTERNAL) &&
			get_rel_relkind(deprec->objid) == RELKIND_SEQUENCE)
		{
			if (!deptype || deprec->deptype == deptype)
				result = lappend_oid(result, deprec->objid);
		}
	}

	systable_endscan(scan);

	table_close(depRel, AccessShareLock);

	return result;
}

/*
 * 收集由指定关系拥有（identity 或 serial）的所有序列
 * 的 OIDs 列表。
 */
List *
getOwnedSequences(Oid relid)
{
	return getOwnedSequences_internal(relid, 0, 0);
}

/*
 * 取得所拥有的 identity 序列；若不是恰好一条则报错。
 */
Oid
getIdentitySequence(Oid relid, AttrNumber attnum, bool missing_ok)
{
	List	   *seqlist = getOwnedSequences_internal(relid, attnum, DEPENDENCY_INTERNAL);

	if (list_length(seqlist) > 1)
		elog(ERROR, "more than one owned sequence found");
	else if (seqlist == NIL)
	{
		if (missing_ok)
			return InvalidOid;
		else
			elog(ERROR, "no owned sequence found");
	}

	return linitial_oid(seqlist);
}

/*
 * get_index_constraint
 *		给定索引的 OID，返回拥有它的唯一、
 *		主键或排斥约束的 OID；若没有拥有
 *		约束则返回 InvalidOid。
 */
Oid
get_index_constraint(Oid indexId)
{
	Oid			constraintId = InvalidOid;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* 在依赖表中搜索该索引 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(indexId));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * 我们假定对约束的任何 internal 依赖都是我们
		 * 所要找的。
		 */
		if (deprec->refclassid == ConstraintRelationId &&
			deprec->refobjsubid == 0 &&
			deprec->deptype == DEPENDENCY_INTERNAL)
		{
			constraintId = deprec->refobjid;
			break;
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return constraintId;
}

/*
 * get_index_ref_constraints
 *		给定索引的 OID，返回引用该索引的所有
 *		外键约束的 OID。
 */
List *
get_index_ref_constraints(Oid indexId)
{
	List	   *result = NIL;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* 在依赖表中搜索该索引 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(indexId));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * 我们假定来自约束的任何 normal 依赖都是我们
		 * 所要找的。
		 */
		if (deprec->classid == ConstraintRelationId &&
			deprec->objsubid == 0 &&
			deprec->deptype == DEPENDENCY_NORMAL)
		{
			result = lappend_oid(result, deprec->objid);
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return result;
}
