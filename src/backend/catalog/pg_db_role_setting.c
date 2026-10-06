/*
 * pg_db_role_setting.c
 *		支持操作 pg_db_role_setting 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_db_role_setting.c
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_db_role_setting.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

void
AlterSetting(Oid databaseid, Oid roleid, VariableSetStmt *setstmt)
{
	char	   *valuestr;
	HeapTuple	tuple;
	Relation	rel;
	ScanKeyData scankey[2];
	SysScanDesc scan;

	valuestr = ExtractSetVariableArgs(setstmt);

	/* 获取旧元组（若有）。 */

	rel = table_open(DbRoleSettingRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey[0],
				Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(databaseid));
	ScanKeyInit(&scankey[1],
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(roleid));
	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 2, scankey);
	tuple = systable_getnext(scan);

	/*
	 * 有三种情形：
	 *
	 * - 在 RESET ALL 中，请求 GUC 重置设置数组，若还有剩余则更新
	 * 目录，否则删除它
	 *
	 * - 在其他命令中，若 pg_db_role_setting 中有元组则更新
	 * 它；若最终为空则删除它
	 *
	 * - 否则插入新的 pg_db_role_setting 元组，但仅当
	 * 命令不是 RESET 时
	 */
	if (setstmt->kind == VAR_RESET_ALL)
	{
		if (HeapTupleIsValid(tuple))
		{
			ArrayType  *new = NULL;
			Datum		datum;
			bool		isnull;

			datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
								 RelationGetDescr(rel), &isnull);

			if (!isnull)
				new = GUCArrayReset(DatumGetArrayTypeP(datum));

			if (new)
			{
				Datum		repl_val[Natts_pg_db_role_setting];
				bool		repl_null[Natts_pg_db_role_setting];
				bool		repl_repl[Natts_pg_db_role_setting];
				HeapTuple	newtuple;

				memset(repl_repl, false, sizeof(repl_repl));

				repl_val[Anum_pg_db_role_setting_setconfig - 1] =
					PointerGetDatum(new);
				repl_repl[Anum_pg_db_role_setting_setconfig - 1] = true;
				repl_null[Anum_pg_db_role_setting_setconfig - 1] = false;

				newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
											 repl_val, repl_null, repl_repl);
				CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
			}
			else
				CatalogTupleDelete(rel, &tuple->t_self);
		}
	}
	else if (HeapTupleIsValid(tuple))
	{
		Datum		repl_val[Natts_pg_db_role_setting];
		bool		repl_null[Natts_pg_db_role_setting];
		bool		repl_repl[Natts_pg_db_role_setting];
		HeapTuple	newtuple;
		Datum		datum;
		bool		isnull;
		ArrayType  *a;

		memset(repl_repl, false, sizeof(repl_repl));
		repl_repl[Anum_pg_db_role_setting_setconfig - 1] = true;
		repl_null[Anum_pg_db_role_setting_setconfig - 1] = false;

		/* 提取 setconfig 的旧值 */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		a = isnull ? NULL : DatumGetArrayTypeP(datum);

		/* 更新（RESET 情形下 valuestr 为 NULL） */
		if (valuestr)
			a = GUCArrayAdd(a, setstmt->name, valuestr);
		else
			a = GUCArrayDelete(a, setstmt->name);

		if (a)
		{
			repl_val[Anum_pg_db_role_setting_setconfig - 1] =
				PointerGetDatum(a);

			newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
										 repl_val, repl_null, repl_repl);
			CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
		}
		else
			CatalogTupleDelete(rel, &tuple->t_self);
	}
	else if (valuestr)
	{
		/* 非空 valuestr 表示不是 RESET，因此插入新元组 */
		HeapTuple	newtuple;
		Datum		values[Natts_pg_db_role_setting];
		bool		nulls[Natts_pg_db_role_setting];
		ArrayType  *a;

		memset(nulls, false, sizeof(nulls));

		a = GUCArrayAdd(NULL, setstmt->name, valuestr);

		values[Anum_pg_db_role_setting_setdatabase - 1] =
			ObjectIdGetDatum(databaseid);
		values[Anum_pg_db_role_setting_setrole - 1] = ObjectIdGetDatum(roleid);
		values[Anum_pg_db_role_setting_setconfig - 1] = PointerGetDatum(a);
		newtuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);

		CatalogTupleInsert(rel, newtuple);
	}

	InvokeObjectPostAlterHookArg(DbRoleSettingRelationId,
								 databaseid, 0, roleid, false);

	systable_endscan(scan);

	/* 关闭 pg_db_role_setting，但保持锁直到提交 */
	table_close(rel, NoLock);
}

/*
 * 从目录删除一些设置。这些可以针对特定
 * 数据库，或针对特定角色。（当然也可以两者都做，
 * 但对当前用途没有意义。）
 */
void
DropSetting(Oid databaseid, Oid roleid)
{
	Relation	relsetting;
	TableScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple	tup;
	int			numkeys = 0;

	relsetting = table_open(DbRoleSettingRelationId, RowExclusiveLock);

	if (OidIsValid(databaseid))
	{
		ScanKeyInit(&keys[numkeys],
					Anum_pg_db_role_setting_setdatabase,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(databaseid));
		numkeys++;
	}
	if (OidIsValid(roleid))
	{
		ScanKeyInit(&keys[numkeys],
					Anum_pg_db_role_setting_setrole,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(roleid));
		numkeys++;
	}

	scan = table_beginscan_catalog(relsetting, numkeys, keys);
	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		CatalogTupleDelete(relsetting, &tup->t_self);
	}
	table_endscan(scan);

	table_close(relsetting, RowExclusiveLock);
}

/*
 * 扫描 pg_db_role_setting 查找适用的设置，并加载到
 * 当前进程。
 *
 * relsetting 是已打开并加锁的 pg_db_role_setting。
 *
 * 注意：我们只考虑精确 databaseid/roleid 组合的设置。
 * 这可能需要调用多次，并把 InvalidOid 作为
 * databaseid/roleid 传入。
 */
void
ApplySetting(Snapshot snapshot, Oid databaseid, Oid roleid,
			 Relation relsetting, GucSource source)
{
	SysScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple	tup;

	ScanKeyInit(&keys[0],
				Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(databaseid));
	ScanKeyInit(&keys[1],
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(roleid));

	scan = systable_beginscan(relsetting, DbRoleSettingDatidRolidIndexId, true,
							  snapshot, 2, keys);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		datum;

		datum = heap_getattr(tup, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(relsetting), &isnull);
		if (!isnull)
		{
			ArrayType  *a = DatumGetArrayTypeP(datum);

			/*
			 * 我们在 SUSET 级别处理所有选项。假定
			 * 把选项插入 pg_db_role_setting 的权限已在
			 * 插入时检查过。
			 */
			ProcessGUCArray(a, PGC_SUSET, source, GUC_ACTION_SET);
		}
	}

	systable_endscan(scan);
}
