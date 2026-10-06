/*-------------------------------------------------------------------------
 *
 * pg_inherits.c
 *	  支持操作 pg_inherits 关系的例程
 *
 * 注意：目前本模块主要包含查询函数；实际的
 * pg_inherits 条目创建与删除大多在 tablecmds.c 中完成。
 * 或许有朝一日那些代码应移到这里，但必须先把它与
 * pg_depend 更新等其他事情拆解开。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_inherits.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * find_all_inheritors 所用哈希表的条目。见下文。
 */
typedef struct SeenRelsEntry
{
	Oid			rel_id;			/* 关系 oid */
	int			list_index;		/* 它在输出 list(s) 中的位置 */
} SeenRelsEntry;

/*
 * find_inheritance_children
 *
 * 返回一个 list，其中包含所有
 * *直接* 继承自 OID 为 'parentrelId' 的关系的关系 OIDs。
 *
 * 对每个子关系取得指定类型的锁（但不锁
 * 给定关系；调用方应已锁住它）。若 lockmode 为 NoLock
 * 则不取得任何锁，但调用方须警惕与可能的子关系
 * DROPs 之间的竞态。
 *
 * 标为正在分离的分区会被省略；详见
 * find_inheritance_children_extended。
 */
List *
find_inheritance_children(Oid parentrelId, LOCKMODE lockmode)
{
	return find_inheritance_children_extended(parentrelId, true, lockmode,
											  NULL, NULL);
}

/*
 * find_inheritance_children_extended
 *
 * 与 find_inheritance_children 相同，但对已分离
 * 分区提供更多选项。
 *
 * 若分区的 pg_inherits 行标为 "detach pending"，
 * 则将 *detached_exist（若非 null）置为 true。
 *
 * 若 omit_detached 为 true，且存在活动快照（与扫描
 * pg_inherits 所用的目录快照不同！），并且该快照可见某条标为
 * "detach pending" 的 pg_inherits 元组，则将该分区
 * 从输出列表中省略。这使分区是否可见取决于
 * 将那些分区标为已分离的事务对活动快照而言是否
 * 已提交。此外，*detached_xmin（若非 null）
 * 被设为已分离分区那一行的 xmin。
 */
List *
find_inheritance_children_extended(Oid parentrelId, bool omit_detached,
								   LOCKMODE lockmode, bool *detached_exist,
								   TransactionId *detached_xmin)
{
	List	   *list = NIL;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	inheritsTuple;
	Oid			inhrelid;
	Oid		   *oidarr;
	int			maxoids,
				numoids,
				i;

	/*
	 * 若 pg_class 显示该关系从未有过
	 * 子类，则可跳过扫描。
	 */
	if (!has_subclass(parentrelId))
		return NIL;

	/*
	 * 扫描 pg_inherits 并构建子类 OIDs 的工作数组。
	 */
	maxoids = 32;
	oidarr = (Oid *) palloc(maxoids * sizeof(Oid));
	numoids = 0;

	relation = table_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parentrelId));

	scan = systable_beginscan(relation, InheritsParentIndexId, true,
							  NULL, 1, key);

	while ((inheritsTuple = systable_getnext(scan)) != NULL)
	{
		/*
		 * 处理并发正在分离的分区。当我们看到
		 * 标为 "detach pending" 的分区时，若调用方请求省略，且该元组的 xmin
		 * 对活动快照而言看起来不在进行中，则从返回的
		 * 可见分区集合中省略它。（若没有
		 * 设置活动快照，意味着我们不是在运行用户查询，因此
		 * 那种情况下总是包含已分离分区是可以的；若
		 * xmin 对活动快照仍在运行，则该分区
		 * 尚未分离，因此我们包含它。）
		 *
		 * 这种变通的原因是：我们希望在 REPEATABLE READ 或
		 * SERIALIZABLE 事务的 RI 查询中避免将该分区视为存活：
		 * 这类查询使用的快照与常规（用户）查询所用的
		 * 不同。
		 */
		if (((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhdetachpending)
		{
			if (detached_exist)
				*detached_exist = true;

			if (omit_detached && ActiveSnapshotSet())
			{
				TransactionId xmin;
				Snapshot	snap;

				xmin = HeapTupleHeaderGetXmin(inheritsTuple->t_data);
				snap = GetActiveSnapshot();

				if (!XidInMVCCSnapshot(xmin, snap))
				{
					if (detached_xmin)
					{
						/*
						 * 不应出现两个已分离分区（见
						 * MarkInheritDetached 中的检查），但若出现，
						 * 则跟踪两者中较新的那个。务必警告
						 * 用户，以便其清理。由于这只是对可能
						 * 已损坏目录的交叉检查，我们不把它做成
						 * 完整的
						 * 错误消息。
						 */
						if (*detached_xmin != InvalidTransactionId)
						{
							elog(WARNING, "more than one partition pending detach found for table with OID %u",
								 parentrelId);
							if (TransactionIdFollows(xmin, *detached_xmin))
								*detached_xmin = xmin;
						}
						else
							*detached_xmin = xmin;
					}

					/* 不要把该分区加入输出列表 */
					continue;
				}
			}
		}

		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrelid;
		if (numoids >= maxoids)
		{
			maxoids *= 2;
			oidarr = (Oid *) repalloc(oidarr, maxoids * sizeof(Oid));
		}
		oidarr[numoids++] = inhrelid;
	}

	systable_endscan(scan);

	table_close(relation, AccessShareLock);

	/*
	 * 若找到多个子关系，按 OID 排序。这确保
	 * 行为相当一致，而不受索引扫描偶然性影响。
	 * 这一点很重要，因为我们需要确保所有后端
	 * 以相同顺序锁住子关系，以避免不必要的死锁。
	 */
	if (numoids > 1)
		qsort(oidarr, numoids, sizeof(Oid), oid_cmp);

	/*
	 * 取得锁并构建结果列表。
	 */
	for (i = 0; i < numoids; i++)
	{
		inhrelid = oidarr[i];

		if (lockmode != NoLock)
		{
			/* 取得锁以与并发删除同步 */
			LockRelationOid(inhrelid, lockmode);

			/*
			 * 现在我们已持有锁，再复查该关系
			 * 是否确实存在。若不存在，则假定在我们等待加锁期间
			 * 它已被删除，并忽略它。
			 */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(inhrelid)))
			{
				/* 释放无用的锁 */
				UnlockRelationOid(inhrelid, lockmode);
				/* 并忽略此关系 */
				continue;
			}
		}

		list = lappend_oid(list, inhrelid);
	}

	pfree(oidarr);

	return list;
}


/*
 * find_all_inheritors -
 *		返回一个关系 OIDs 列表，包括给定关系以及
 *		所有直接或间接继承自它的关系。
 *		可选地，还返回在以给定关系为根的
 *		继承树中为每个此类关系找到的
 *		父关系数量。
 *
 * 对所有子关系取得指定类型的锁（但不锁
 * 给定关系；调用方应已锁住它）。若 lockmode 为 NoLock
 * 则不取得任何锁，但调用方须警惕与可能的子关系
 * DROPs 之间的竞态。
 *
 * NB - 目前没有调用方关心并发正在分离的
 * 子关系，因此没有提供包含它们的机制。
 */
List *
find_all_inheritors(Oid parentrelId, LOCKMODE lockmode, List **numparents)
{
	/* 用于 O(1) rel_oid -> rel_numparents 单元查找的哈希表 */
	HTAB	   *seen_rels;
	HASHCTL		ctl;
	List	   *rels_list,
			   *rel_numparents;
	ListCell   *l;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(SeenRelsEntry);
	ctl.hcxt = CurrentMemoryContext;

	seen_rels = hash_create("find_all_inheritors temporary table",
							32, /* 从小开始并扩展 */
							&ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * 我们构建一个列表，以给定关系开始，并加入所有直接和
	 * 间接子关系。我们可以用同一个列表既作为
	 * 已找到关系的记录，也作为尚待扫描以寻找更多
	 * 子关系的议程。这有点巧妙，但可行，因为 foreach() 宏
	 * 直到循环底部才取下一个列表元素。注意
	 * 我们不能保持指向输出列表的指针；但一个索引就
	 * 足够了。
	 */
	rels_list = list_make1_oid(parentrelId);
	rel_numparents = list_make1_int(0);

	foreach(l, rels_list)
	{
		Oid			currentrel = lfirst_oid(l);
		List	   *currentchildren;
		ListCell   *lc;

		/* 获取此关系的直接子关系 */
		currentchildren = find_inheritance_children(currentrel, lockmode);

		/*
		 * 仅把尚未见过的子关系加入队列。这避免
		 * 在从同一父关系有多条继承路径时产生
		 * 重复条目。（这也能防止我们陷入无限
		 * 循环，尽管理论上继承图中
		 * 不可能有环。）
		 */
		foreach(lc, currentchildren)
		{
			Oid			child_oid = lfirst_oid(lc);
			bool		found;
			SeenRelsEntry *hash_entry;

			hash_entry = hash_search(seen_rels, &child_oid, HASH_ENTER, &found);
			if (found)
			{
				/* 若该关系已在其中，则增加父关系计数 */
				ListCell   *numparents_cell;

				numparents_cell = list_nth_cell(rel_numparents,
												hash_entry->list_index);
				lfirst_int(numparents_cell)++;
			}
			else
			{
				/* 若不在其中，则加入。最初预期有 1 个父关系。 */
				hash_entry->list_index = list_length(rels_list);
				rels_list = lappend_oid(rels_list, child_oid);
				rel_numparents = lappend_int(rel_numparents, 1);
			}
		}
	}

	if (numparents)
		*numparents = rel_numparents;
	else
		list_free(rel_numparents);

	hash_destroy(seen_rels);

	return rels_list;
}


/*
 * has_subclass - 此关系是否有任何子关系？
 *
 * 在当前实现中，has_subclass 返回的是某个
 * 类 *可能* 有子类。若某类曾有子类但后来被删除，
 * 它不会返回正确结果。
 * 这是因为删除子类时不会立即更新
 * pg_class 中的 relhassubclass，主要是出于并发考虑。
 *
 * 目前 has_subclass 仅用作效率技巧以跳过
 * 不必要的继承搜索，因此这样可以。注意对
 * 无子表执行 ANALYZE 会清理过时的 relhassubclass 标志。
 *
 * 尽管这实际上不触及 pg_inherits，但把它放在这里
 * 似乎合理，因为它通常与这里的其他例程一起使用。
 */
bool
has_subclass(Oid relationId)
{
	HeapTuple	tuple;
	bool		result;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);

	result = ((Form_pg_class) GETSTRUCT(tuple))->relhassubclass;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * has_superclass - 此关系是否继承自另一个关系？
 *
 * 与 has_subclass 不同，这一点可以信赖以给出准确答案。
 * 不过，调用方必须持有对给定关系的锁，使其
 * 不能被并发加入或移出继承层次。
 */
bool
has_superclass(Oid relationId)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData skey;
	bool		result;

	catalog = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&skey, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relationId));
	scan = systable_beginscan(catalog, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &skey);
	result = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	return result;
}

/*
 * 给定两个类型 OIDs，判断第一个是否为继承自
 * 第二个的复合类型（类类型）。
 *
 * 这本质上是在问第一种类型是否保证可强制转换
 * 为第二种。因此，我们允许第一种类型是覆盖某个
 * 继承自第二种的复合类型的域；那不会造成困难。
 * 但第二种类型无法是域。
 */
bool
typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId)
{
	bool		result = false;
	Oid			subclassRelid;
	Oid			superclassRelid;
	Relation	inhrel;
	List	   *visited,
			   *queue;
	ListCell   *queue_item;

	/* 我们需要使用相关联的关系 OIDs 来工作 */
	subclassRelid = typeOrDomainTypeRelid(subclassTypeId);
	if (subclassRelid == InvalidOid)
		return false;			/* 不是复合类型，也不是其上的域 */
	superclassRelid = typeidTypeRelid(superclassTypeId);
	if (superclassRelid == InvalidOid)
		return false;			/* 不是复合类型 */

	/* 若超类没有子类则不必搜索 */
	if (!has_subclass(superclassRelid))
		return false;

	/*
	 * 从关系自身开始搜索，因此把它的 relid 加入队列。
	 */
	queue = list_make1_oid(subclassRelid);
	visited = NIL;

	inhrel = table_open(InheritsRelationId, AccessShareLock);

	/*
	 * 用队列对从所给 relid 直到根的继承图做
	 * 广度优先遍历。注意我们在循环内追加到队列
	 * --- 这没问题，因为 foreach() 宏直到下一次循环迭代开始时
	 * 才推进 queue_item。
	 */
	foreach(queue_item, queue)
	{
		Oid			this_relid = lfirst_oid(queue_item);
		ScanKeyData skey;
		SysScanDesc inhscan;
		HeapTuple	inhtup;

		/*
		 * 若我们已经见过此 relid，则跳过。这在
		 * 多重继承场景中避免额外工作，也在
		 * pg_inherits 存在环时保护我们免于无限循环（尽管
		 * 理论上不应发生）。
		 */
		if (list_member_oid(visited, this_relid))
			continue;

		/*
		 * 好，这是尚未见过的 relid。把它加入
		 * 已访问 OIDs 列表，然后找出此 relid 继承自的
		 * 所有类型并把它们加入队列。
		 */
		visited = lappend_oid(visited, this_relid);

		ScanKeyInit(&skey,
					Anum_pg_inherits_inhrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(this_relid));

		inhscan = systable_beginscan(inhrel, InheritsRelidSeqnoIndexId, true,
									 NULL, 1, &skey);

		while ((inhtup = systable_getnext(inhscan)) != NULL)
		{
			Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inhtup);
			Oid			inhparent = inh->inhparent;

			/* 若这就是目标超类，则完成 */
			if (inhparent == superclassRelid)
			{
				result = true;
				break;
			}

			/* 否则加入队列 */
			queue = lappend_oid(queue, inhparent);
		}

		systable_endscan(inhscan);

		if (result)
			break;
	}

	/* 清理 ... */
	table_close(inhrel, AccessShareLock);

	list_free(visited);
	list_free(queue);

	return result;
}

/*
 * 用给定数据创建单条 pg_inherits 行
 */
void
StoreSingleInheritance(Oid relationId, Oid parentOid, int32 seqNumber)
{
	Datum		values[Natts_pg_inherits];
	bool		nulls[Natts_pg_inherits];
	HeapTuple	tuple;
	Relation	inhRelation;

	inhRelation = table_open(InheritsRelationId, RowExclusiveLock);

	/*
	 * 创建 pg_inherits 条目
	 */
	values[Anum_pg_inherits_inhrelid - 1] = ObjectIdGetDatum(relationId);
	values[Anum_pg_inherits_inhparent - 1] = ObjectIdGetDatum(parentOid);
	values[Anum_pg_inherits_inhseqno - 1] = Int32GetDatum(seqNumber);
	values[Anum_pg_inherits_inhdetachpending - 1] = BoolGetDatum(false);

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(RelationGetDescr(inhRelation), values, nulls);

	CatalogTupleInsert(inhRelation, tuple);

	heap_freetuple(tuple);

	table_close(inhRelation, RowExclusiveLock);
}

/*
 * DeleteInheritsTuple
 *
 * 删除具有给定 inhrelid 的 pg_inherits 元组。inhparent 可给
 * InvalidOid，此时删除所有匹配 inhrelid 的元组；
 * 否则仅删除具有指定 inhparent 的元组。
 *
 * expect_detach_pending 是 inhdetachpending 标志的期望状态。
 * 若目录行与该状态不匹配，则报错。
 *
 * childname 是分区名（若为表）；对普通继承或处理
 * 其他关系种类时传 NULL。
 *
 * 返回是否至少删除了一行。
 */
bool
DeleteInheritsTuple(Oid inhrelid, Oid inhparent, bool expect_detach_pending,
					const char *childname)
{
	bool		found = false;
	Relation	catalogRelation;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	inheritsTuple;

	/*
	 * 按 inhrelid 查找 pg_inherits 条目。
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(inhrelid));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Oid			parent;

		/* 若给定了 inhparent 则比较，并做实际删除。 */
		parent = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhparent;
		if (!OidIsValid(inhparent) || parent == inhparent)
		{
			bool		detach_pending;

			detach_pending =
				((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhdetachpending;

			/*
			 * 按状态报错。这应只对
			 * 分区发生，但我们无法交叉验证。
			 */
			if (detach_pending && !expect_detach_pending)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot detach partition \"%s\"",
								childname ? childname : "unknown relation"),
						 errdetail("The partition is being detached concurrently or has an unfinished detach."),
						 errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation.")));
			if (!detach_pending && expect_detach_pending)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot complete detaching partition \"%s\"",
								childname ? childname : "unknown relation"),
						 errdetail("There's no pending concurrent detach.")));

			CatalogTupleDelete(catalogRelation, &inheritsTuple->t_self);
			found = true;
		}
	}

	/* 完成 */
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	return found;
}

/*
 * 返回分区的 pg_inherits 元组是否设置了 "detach
 * pending" 标志。
 */
bool
PartitionHasPendingDetach(Oid partoid)
{
	Relation	catalogRelation;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	inheritsTuple;

	/* 我们没有很好的方法验证它确实是分区 */

	/*
	 * 按 inhrelid 查找 pg_inherits 条目。（应只有一个。）
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partoid));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		bool		detached;

		detached =
			((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhdetachpending;

		/* 完成 */
		systable_endscan(scan);
		table_close(catalogRelation, RowExclusiveLock);

		return detached;
	}

	elog(ERROR, "relation %u is not a partition", partoid);
	return false;				/* 让编译器安静 */
}
