/*-------------------------------------------------------------------------
 *
 * pg_enum.c
 *	  支持操作 pg_enum 关系的例程
 *
 * Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_enum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/value.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/* 可能由 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_pg_enum_oid = InvalidOid;

/*
 * 当前事务中由 AddEnumLabel 创建的枚举值 OIDs 的哈希表。
 * 在事务提交之前我们禁止使用这些值；
 * 否则它们可能进入我们无法清理的索引，
 * 然后若事务回滚就会留下损坏的索引。
 * （见 enum.c 中 check_safe_enum_use() 的注释。）EnumValuesCreate
 * 创建的值 *不* 进入该表；我们假定那些是在
 * CREATE TYPE 期间创建的，因此除非枚举类型本身消失，
 * 它们不会消失。
 */
static HTAB *uncommitted_enums = NULL;

static void RenumberEnumType(Relation pg_enum, HeapTuple *existing, int nelems);
static int	sort_order_cmp(const void *p1, const void *p2);


/*
 * EnumValuesCreate
 *		为所提供的每个枚举值在 pg_enum 中创建一条条目。
 *
 * vals 是 String 值的列表。
 */
void
EnumValuesCreate(Oid enumTypeOid, List *vals)
{
	Relation	pg_enum;
	Oid		   *oids;
	int			elemno,
				num_elems;
	ListCell   *lc;
	int			slotCount = 0;
	int			nslots;
	CatalogIndexState indstate;
	TupleTableSlot **slot;

	num_elems = list_length(vals);

	/*
	 * 我们不费心检查值列表是否有重复 --- 若有，
	 * 你会得到不太友好的唯一索引冲突。大概
	 * 不值得再费劲。
	 */

	pg_enum = table_open(EnumRelationId, RowExclusiveLock);

	/*
	 * 为枚举成员分配 OIDs。
	 *
	 * 虽然这种方法不能绝对保证不产生
	 * 重复 OIDs（因为我们在分配下一个之前尚未把每个 oid 写入表），
	 * 只有在我们完成之前 OID 计数器整整绕一圈时才会出问题。
	 * 这似乎不太可能。
	 */
	oids = (Oid *) palloc(num_elems * sizeof(Oid));

	for (elemno = 0; elemno < num_elems; elemno++)
	{
		/*
		 * 我们给所有新枚举标签分配偶数 OIDs。这
		 * 告诉比较函数这些 OIDs 处于正确的排序
		 * 顺序，可以直接比较。
		 */
		Oid			new_oid;

		do
		{
			new_oid = GetNewOidWithIndex(pg_enum, EnumOidIndexId,
										 Anum_pg_enum_oid);
		} while (new_oid & 1);
		oids[elemno] = new_oid;
	}

	/* 对它们排序，以防 OID 计数器从高绕到低 */
	qsort(oids, num_elems, sizeof(Oid), oid_cmp);

	/* 并创建条目 */
	indstate = CatalogOpenIndexes(pg_enum);

	/* 分配要使用的槽并初始化它们 */
	nslots = Min(num_elems,
				 MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_enum));
	slot = palloc(sizeof(TupleTableSlot *) * nslots);
	for (int i = 0; i < nslots; i++)
		slot[i] = MakeSingleTupleTableSlot(RelationGetDescr(pg_enum),
										   &TTSOpsHeapTuple);

	elemno = 0;
	foreach(lc, vals)
	{
		char	   *lab = strVal(lfirst(lc));
		Name		enumlabel = palloc0(NAMEDATALEN);

		/*
		 * 标签存储在 name 字段中，以便于系统缓存查找，因此
		 * 检查长度以确保在范围内。
		 */
		if (strlen(lab) > (NAMEDATALEN - 1))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("invalid enum label \"%s\"", lab),
					 errdetail("Labels must be %d bytes or less.",
							   NAMEDATALEN - 1)));

		ExecClearTuple(slot[slotCount]);

		memset(slot[slotCount]->tts_isnull, false,
			   slot[slotCount]->tts_tupleDescriptor->natts * sizeof(bool));

		slot[slotCount]->tts_values[Anum_pg_enum_oid - 1] = ObjectIdGetDatum(oids[elemno]);
		slot[slotCount]->tts_values[Anum_pg_enum_enumtypid - 1] = ObjectIdGetDatum(enumTypeOid);
		slot[slotCount]->tts_values[Anum_pg_enum_enumsortorder - 1] = Float4GetDatum(elemno + 1);

		namestrcpy(enumlabel, lab);
		slot[slotCount]->tts_values[Anum_pg_enum_enumlabel - 1] = NameGetDatum(enumlabel);

		ExecStoreVirtualTuple(slot[slotCount]);
		slotCount++;

		/* 若槽已满，则插入一批元组 */
		if (slotCount == nslots)
		{
			CatalogTuplesMultiInsertWithInfo(pg_enum, slot, slotCount,
											 indstate);
			slotCount = 0;
		}

		elemno++;
	}

	/* 插入缓冲区中剩余的任何元组 */
	if (slotCount > 0)
		CatalogTuplesMultiInsertWithInfo(pg_enum, slot, slotCount,
										 indstate);

	/* 清理 */
	pfree(oids);
	for (int i = 0; i < nslots; i++)
		ExecDropSingleTupleTableSlot(slot[i]);
	CatalogCloseIndexes(indstate);
	table_close(pg_enum, RowExclusiveLock);
}


/*
 * EnumValuesDelete
 *		移除指定枚举类型的所有 pg_enum 条目。
 */
void
EnumValuesDelete(Oid enumTypeOid)
{
	Relation	pg_enum;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;

	pg_enum = table_open(EnumRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumTypeOid));

	scan = systable_beginscan(pg_enum, EnumTypIdLabelIndexId, true,
							  NULL, 1, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		CatalogTupleDelete(pg_enum, &tup->t_self);
	}

	systable_endscan(scan);

	table_close(pg_enum, RowExclusiveLock);
}

/*
 * 为本事务初始化未提交枚举表。
 */
static void
init_uncommitted_enums(void)
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(Oid);
	hash_ctl.hcxt = TopTransactionContext;
	uncommitted_enums = hash_create("Uncommitted enums",
									32,
									&hash_ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * AddEnumLabel
 *		向枚举集合添加新标签。默认放在
 *		末尾，但用户可以选择把它放在任何
 *		已有成员之前或之后。
 */
void
AddEnumLabel(Oid enumTypeOid,
			 const char *newVal,
			 const char *neighbor,
			 bool newValIsAfter,
			 bool skipIfExists)
{
	Relation	pg_enum;
	Oid			newOid;
	Datum		values[Natts_pg_enum];
	bool		nulls[Natts_pg_enum];
	NameData	enumlabel;
	HeapTuple	enum_tup;
	float4		newelemorder;
	HeapTuple  *existing;
	CatCList   *list;
	int			nelems;
	int			i;

	/* 检查新标签长度是否可以 */
	if (strlen(newVal) > (NAMEDATALEN - 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid enum label \"%s\"", newVal),
				 errdetail("Labels must be %d bytes or less.",
						   NAMEDATALEN - 1)));

	/*
	 * 在枚举类型上取得一把锁，直到提交才释放。
	 * 这确保两个后端不会并发修改同一
	 * 枚举类型。没有它，我们无法确保通过系统缓存看到
	 * 枚举成员的一致视图。注意这并不阻止
	 * 其他后端检查该类型；见
	 * RenumberEnumType 的注释。
	 */
	LockDatabaseObject(TypeRelationId, enumTypeOid, 0, ExclusiveLock);

	/*
	 * 检查标签是否已在使用。pg_enum 上的唯一索引
	 * 反正也会抓住这一点，但我们更喜欢更友好的错误消息，而且
	 * 此外我们需要检查以支持 IF NOT EXISTS。
	 */
	enum_tup = SearchSysCache2(ENUMTYPOIDNAME,
							   ObjectIdGetDatum(enumTypeOid),
							   CStringGetDatum(newVal));
	if (HeapTupleIsValid(enum_tup))
	{
		ReleaseSysCache(enum_tup);
		if (skipIfExists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("enum label \"%s\" already exists, skipping",
							newVal)));
			return;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("enum label \"%s\" already exists",
							newVal)));
	}

	pg_enum = table_open(EnumRelationId, RowExclusiveLock);

	/* 若必须对已有成员重新编号，则从这里重新开始 */
restart:

	/* 获取枚举的已有成员列表 */
	list = SearchSysCacheList1(ENUMTYPOIDNAME,
							   ObjectIdGetDatum(enumTypeOid));
	nelems = list->n_members;

	/* 按 enumsortorder 对已有成员排序 */
	existing = (HeapTuple *) palloc(nelems * sizeof(HeapTuple));
	for (i = 0; i < nelems; i++)
		existing[i] = &(list->members[i]->tuple);

	qsort(existing, nelems, sizeof(HeapTuple), sort_order_cmp);

	if (neighbor == NULL)
	{
		/*
		 * 把新标签放在列表末尾。无需更改
		 * 已有元组。
		 */
		if (nelems > 0)
		{
			Form_pg_enum en = (Form_pg_enum) GETSTRUCT(existing[nelems - 1]);

			newelemorder = en->enumsortorder + 1;
		}
		else
			newelemorder = 1;
	}
	else
	{
		/* 指定了 BEFORE 或 AFTER */
		int			nbr_index;
		int			other_nbr_index;
		Form_pg_enum nbr_en;
		Form_pg_enum other_nbr_en;

		/* 定位邻居元素 */
		for (nbr_index = 0; nbr_index < nelems; nbr_index++)
		{
			Form_pg_enum en = (Form_pg_enum) GETSTRUCT(existing[nbr_index]);

			if (strcmp(NameStr(en->enumlabel), neighbor) == 0)
				break;
		}
		if (nbr_index >= nelems)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" is not an existing enum label",
							neighbor)));
		nbr_en = (Form_pg_enum) GETSTRUCT(existing[nbr_index]);

		/*
		 * 尝试分配合适的 enumsortorder 值：比最小成员
		 * 小一，比最大成员大一，或介于两个
		 * 已有成员的中间。
		 *
		 * 在"中间"情形下，由于 float4 精度有限，
		 * 我们可能算出一个实际上等于某一侧邻居的值。
		 * 那种情况下我们对已有成员重新编号
		 * 并重试。
		 */
		if (newValIsAfter)
			other_nbr_index = nbr_index + 1;
		else
			other_nbr_index = nbr_index - 1;

		if (other_nbr_index < 0)
			newelemorder = nbr_en->enumsortorder - 1;
		else if (other_nbr_index >= nelems)
			newelemorder = nbr_en->enumsortorder + 1;
		else
		{
			/*
			 * 这里算出的中点值必须舍入到 float4
			 * 精度，否则我们与相邻值的相等比较
			 * 就没有意义。对非 C 标准兼容编译器强制这一点的
			 * 最可移植方式是把它存入
			 * volatile 变量。
			 */
			volatile float4 midpoint;

			other_nbr_en = (Form_pg_enum) GETSTRUCT(existing[other_nbr_index]);
			midpoint = (nbr_en->enumsortorder +
						other_nbr_en->enumsortorder) / 2;

			if (midpoint == nbr_en->enumsortorder ||
				midpoint == other_nbr_en->enumsortorder)
			{
				RenumberEnumType(pg_enum, existing, nelems);
				/* 清理并重新开始 */
				pfree(existing);
				ReleaseCatCacheList(list);
				goto restart;
			}

			newelemorder = midpoint;
		}
	}

	/* 为新标签获取新 OID */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_pg_enum_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_enum OID value not set when in binary upgrade mode")));

		/*
		 * 若提供了 binary-upgrade 覆盖，则用于 pg_enum.oid。在
		 * 二进制升级期间，所有 pg_enum.oid 都这样设置，以保证
		 * 一致。
		 */
		if (neighbor != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ALTER TYPE ADD BEFORE/AFTER is incompatible with binary upgrade")));

		newOid = binary_upgrade_next_pg_enum_oid;
		binary_upgrade_next_pg_enum_oid = InvalidOid;
	}
	else
	{
		/*
		 * 普通情形：我们需要为该值分配新的 Oid。
		 *
		 * 若安全，我们希望给新元素偶数 Oid，
		 * 也就是说它与枚举中所有预先存在的偶数
		 * Oid 比较正确。否则，必须给它奇数 Oid。
		 */
		for (;;)
		{
			bool		sorts_ok;

			/* 获取新 OID（与所有已有 pg_enum 元组不同） */
			newOid = GetNewOidWithIndex(pg_enum, EnumOidIndexId,
										Anum_pg_enum_oid);

			/*
			 * 检测它相对于枚举中已有偶数编号
			 * 标签的排序是否正确。我们可以忽略已有的
			 * 奇数 Oid 标签，因为涉及它们之一的比较
			 * 反正不会走快速路径。
			 */
			sorts_ok = true;
			for (i = 0; i < nelems; i++)
			{
				HeapTuple	exists_tup = existing[i];
				Form_pg_enum exists_en = (Form_pg_enum) GETSTRUCT(exists_tup);
				Oid			exists_oid = exists_en->oid;

				if (exists_oid & 1)
					continue;	/* 忽略奇数 Oid */

				if (exists_en->enumsortorder < newelemorder)
				{
					/* 应排在前面 */
					if (exists_oid >= newOid)
					{
						sorts_ok = false;
						break;
					}
				}
				else
				{
					/* 应排在后面 */
					if (exists_oid <= newOid)
					{
						sorts_ok = false;
						break;
					}
				}
			}

			if (sorts_ok)
			{
				/* 若是偶数且排序正确，则完成。 */
				if ((newOid & 1) == 0)
					break;

				/*
				 * 若是奇数且排序正确，则循环回去再取一个 OID 并
				 * 重试。大概下一个可用的偶数 OID 也会
				 * 排序正确，因此值得再试。
				 */
			}
			else
			{
				/*
				 * 若是奇数且排序不正确，则完成。
				 * （大概下一个可用的偶数 OID 也会
				 * 排序不正确，因此不必再试。）
				 */
				if (newOid & 1)
					break;

				/*
				 * 若是偶数且排序不正确，则循环回去再取
				 * 一个 OID 并重试。（我们必须拒绝这种情况。）
				 */
			}
		}
	}

	/* 完成关于已有成员的信息 */
	pfree(existing);
	ReleaseCatCacheList(list);

	/* 创建新的 pg_enum 条目 */
	memset(nulls, false, sizeof(nulls));
	values[Anum_pg_enum_oid - 1] = ObjectIdGetDatum(newOid);
	values[Anum_pg_enum_enumtypid - 1] = ObjectIdGetDatum(enumTypeOid);
	values[Anum_pg_enum_enumsortorder - 1] = Float4GetDatum(newelemorder);
	namestrcpy(&enumlabel, newVal);
	values[Anum_pg_enum_enumlabel - 1] = NameGetDatum(&enumlabel);
	enum_tup = heap_form_tuple(RelationGetDescr(pg_enum), values, nulls);
	CatalogTupleInsert(pg_enum, enum_tup);
	heap_freetuple(enum_tup);

	table_close(pg_enum, RowExclusiveLock);

	/* 若本事务尚未建立未提交枚举表则建立之 */
	if (uncommitted_enums == NULL)
		init_uncommitted_enums();

	/* 把新值加入表 */
	(void) hash_search(uncommitted_enums, &newOid, HASH_ENTER, NULL);
}


/*
 * RenameEnumLabel
 *		重命名枚举集合中的标签。
 */
void
RenameEnumLabel(Oid enumTypeOid,
				const char *oldVal,
				const char *newVal)
{
	Relation	pg_enum;
	HeapTuple	enum_tup;
	Form_pg_enum en;
	CatCList   *list;
	int			nelems;
	HeapTuple	old_tup;
	bool		found_new;
	int			i;

	/* 检查新标签长度是否可以 */
	if (strlen(newVal) > (NAMEDATALEN - 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid enum label \"%s\"", newVal),
				 errdetail("Labels must be %d bytes or less.",
						   NAMEDATALEN - 1)));

	/*
	 * 在枚举类型上取得一把锁，直到提交才释放。
	 * 这确保两个后端不会并发修改同一
	 * 枚举类型。由于我们不改变类型的排序顺序，这
	 * 大概并非真正必要，但似乎没有理由不加锁
	 * 以确保。
	 */
	LockDatabaseObject(TypeRelationId, enumTypeOid, 0, ExclusiveLock);

	pg_enum = table_open(EnumRelationId, RowExclusiveLock);

	/* 获取枚举的已有成员列表 */
	list = SearchSysCacheList1(ENUMTYPOIDNAME,
							   ObjectIdGetDatum(enumTypeOid));
	nelems = list->n_members;

	/*
	 * 定位要重命名的元素，并检查新标签是否已在
	 * 使用。（pg_enum 上的唯一索引反正也会抓住这一点，但我们
	 * 更喜欢更友好的错误消息。）
	 */
	old_tup = NULL;
	found_new = false;
	for (i = 0; i < nelems; i++)
	{
		enum_tup = &(list->members[i]->tuple);
		en = (Form_pg_enum) GETSTRUCT(enum_tup);
		if (strcmp(NameStr(en->enumlabel), oldVal) == 0)
			old_tup = enum_tup;
		if (strcmp(NameStr(en->enumlabel), newVal) == 0)
			found_new = true;
	}
	if (!old_tup)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not an existing enum label",
						oldVal)));
	if (found_new)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("enum label \"%s\" already exists",
						newVal)));

	/* 好，制作旧元组的可写副本 */
	enum_tup = heap_copytuple(old_tup);
	en = (Form_pg_enum) GETSTRUCT(enum_tup);

	ReleaseCatCacheList(list);

	/* 更新 pg_enum 条目 */
	namestrcpy(&en->enumlabel, newVal);
	CatalogTupleUpdate(pg_enum, &enum_tup->t_self, enum_tup);
	heap_freetuple(enum_tup);

	table_close(pg_enum, RowExclusiveLock);
}


/*
 * 测试给定枚举值是否在未提交枚举表中。
 */
bool
EnumUncommitted(Oid enum_id)
{
	bool		found;

	/* 若尚未建立未提交表，则所有值都安全 */
	if (uncommitted_enums == NULL)
		return false;

	/* 否则，它是否在表中？ */
	(void) hash_search(uncommitted_enums, &enum_id, HASH_FIND, &found);
	return found;
}


/*
 * 顶层事务结束后清理枚举相关状态。
 */
void
AtEOXact_Enum(void)
{
	/*
	 * 重置未提交表，因为我们所有的枚举值现在都已提交。
	 * TopTransactionContext 被释放时内存会自动消失；
	 * 清除我们的指针就足够了。
	 */
	uncommitted_enums = NULL;
}


/*
 * RenumberEnumType
 *		将已有枚举元素重新编号为排序位置 1..n。
 *
 * 除非绝对必要我们避免这样做；在大多数安装中
 * 这永远不会发生。原因是更新已有 pg_enum
 * 条目会给并发读取 pg_enum 的其他后端带来风险。
 * 尽管系统目录扫描现在使用 MVCC 语义，
 * 系统缓存机制可能在不同快照下读取不同的
 * pg_enum 条目，因此若发生并发重新编号，其他后端可能对正确
 * 顺序感到困惑。
 *
 * 因此我们做如下选择：
 *
 * 1. 任何关心 enumsortorder 值的代码 MUST 用
 * 单个 MVCC 快照读取所有相关 pg_enum 条目，或者
 * 在枚举类型上加锁以防止并发执行
 * AddEnumLabel()。
 *
 * 2. 不检查 enumsortorder 的代码可以使用系统缓存
 * （例如，enum_in 和 enum_out 就是这样做的）。
 */
static void
RenumberEnumType(Relation pg_enum, HeapTuple *existing, int nelems)
{
	int			i;

	/*
	 * 我们应只需增大已有元素的 enumsortorders，
	 * 从不减小它们。因此从末尾向前工作，以避免
	 * 不想要的唯一性冲突。
	 */
	for (i = nelems - 1; i >= 0; i--)
	{
		HeapTuple	newtup;
		Form_pg_enum en;
		float4		newsortorder;

		newtup = heap_copytuple(existing[i]);
		en = (Form_pg_enum) GETSTRUCT(newtup);

		newsortorder = i + 1;
		if (en->enumsortorder != newsortorder)
		{
			en->enumsortorder = newsortorder;

			CatalogTupleUpdate(pg_enum, &newtup->t_self, newtup);
		}

		heap_freetuple(newtup);
	}

	/* 使更新可见 */
	CommandCounterIncrement();
}


/* 按排序顺序比较元组的 qsort 比较函数 */
static int
sort_order_cmp(const void *p1, const void *p2)
{
	HeapTuple	v1 = *((const HeapTuple *) p1);
	HeapTuple	v2 = *((const HeapTuple *) p2);
	Form_pg_enum en1 = (Form_pg_enum) GETSTRUCT(v1);
	Form_pg_enum en2 = (Form_pg_enum) GETSTRUCT(v2);

	if (en1->enumsortorder < en2->enumsortorder)
		return -1;
	else if (en1->enumsortorder > en2->enumsortorder)
		return 1;
	else
		return 0;
}

Size
EstimateUncommittedEnumsSpace(void)
{
	size_t		entries;

	if (uncommitted_enums)
		entries = hash_get_num_entries(uncommitted_enums);
	else
		entries = 0;

	/* 为终止符加一。 */
	return sizeof(Oid) * (entries + 1);
}

void
SerializeUncommittedEnums(void *space, Size size)
{
	Oid		   *serialized = (Oid *) space;

	/*
	 * 确保自调用方预留空间以来哈希表的大小
	 * 没有变化。
	 */
	Assert(size == EstimateUncommittedEnumsSpace());

	/* 若有哈希表，则写出其中的所有值。 */
	if (uncommitted_enums)
	{
		HASH_SEQ_STATUS status;
		Oid		   *value;

		hash_seq_init(&status, uncommitted_enums);
		while ((value = (Oid *) hash_seq_search(&status)))
			*serialized++ = *value;
	}

	/* 写出终止符。 */
	*serialized = InvalidOid;

	/*
	 * 确保我们实际使用的空间量与估计的
	 * 相符。
	 */
	Assert((char *) (serialized + 1) == ((char *) space) + size);
}

void
RestoreUncommittedEnums(void *space)
{
	Oid		   *serialized = (Oid *) space;

	Assert(!uncommitted_enums);

	/*
	 * 作为特例，若列表为空则甚至不必
	 * 创建哈希表。这是通常情况，因为预期枚举
	 * 变更很少见。
	 */
	if (!OidIsValid(*serialized))
		return;

	/* 把所有值读入新的哈希表。 */
	init_uncommitted_enums();
	do
	{
		hash_search(uncommitted_enums, serialized++, HASH_ENTER, NULL);
	} while (OidIsValid(*serialized));
}
