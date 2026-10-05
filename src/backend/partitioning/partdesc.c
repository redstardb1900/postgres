/*-------------------------------------------------------------------------
 *
 * partdesc.c
 *		Support routines for manipulating partition descriptors
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/partitioning/partdesc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "storage/bufmgr.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

typedef struct PartitionDirectoryData
{
	MemoryContext pdir_mcxt;
	HTAB	   *pdir_hash;
	bool		omit_detached;
}			PartitionDirectoryData;

typedef struct PartitionDirectoryEntry
{
	Oid			reloid;
	Relation	rel;
	PartitionDesc pd;
} PartitionDirectoryEntry;

static PartitionDesc RelationBuildPartitionDesc(Relation rel,
												bool omit_detached);


/*
 * RelationGetPartitionDesc：若关系已分区，取得分区描述符。
 *
 * 关系缓存里保留两份分区描述符：rd_partdesc 包含全部分区
 * （甚至包括正在被并发标记为已分离的那些），而 rd_partdesc_nodetached
 * 会略去其中一些。我们保存后者对应的 pg_inherits.xmin 值，
 * 以判断每种情况下它能否有效复用，因为那
 * 取决于活动快照。
 *
 * 注意：我们安排分区描述符直到
 * 关系缓存项的引用计数降到零才释放（见 RelationClose、
 * RelationClearRelation 和 RelationBuildPartitionDesc 里的权宜做法）。因此，即使
 * 交出去的是指向关系缓存项内部的直接指针，调用方
 * 继续使用该指针也是安全的，只要 (a) 他们保持
 * 关系打开，并且 (b) 他们持有足够强的关系锁，以确保
 * 数据不会变陈旧。
 */
PartitionDesc
RelationGetPartitionDesc(Relation rel, bool omit_detached)
{
	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * 若关系缓存已有分区描述符，就用它。不过，只有在
	 * 要求包含全部分区（含已分离的）时才能这样做；
	 * 以及已知没有已分离分区时也可以。
	 *
	 * 若没有活动快照，已分离分区也不会被略去，
	 * 因此那种情况下也可以用缓存的描述符。
	 */
	if (likely(rel->rd_partdesc &&
			   (!rel->rd_partdesc->detached_exist || !omit_detached ||
				!ActiveSnapshotSet())))
		return rel->rd_partdesc;

	/*
	 * 若要求略去已分离分区，也许仍能用
	 * 一份缓存的描述符。判断依据是和该描述符一起保存的
	 * pg_inherits.xmin：若那个对当时活动快照尚未进行的 xmin，
	 * 对当前活动快照也尚未进行，就可以用它。
	 * 否则就从头
	 * 重新构造。
	 */
	if (omit_detached &&
		rel->rd_partdesc_nodetached &&
		ActiveSnapshotSet())
	{
		Snapshot	activesnap;

		Assert(TransactionIdIsValid(rel->rd_partdesc_nodetached_xmin));
		activesnap = GetActiveSnapshot();

		if (!XidInMVCCSnapshot(rel->rd_partdesc_nodetached_xmin, activesnap))
			return rel->rd_partdesc_nodetached;
	}

	return RelationBuildPartitionDesc(rel, omit_detached);
}

/*
 * RelationBuildPartitionDesc
 *		构造关系的分区描述符，并存入关系缓存项。
 *
 * 分区描述符结构复杂；为避免关系缓存项被刷掉时
 * 释放各个元素的复杂逻辑，我们给它
 * 自己的内存上下文，作为 CacheMemoryContext 的子上下文，这样可以
 * 单独删掉。为避免本函数中途出错时在该上下文里泄漏内存，
 * 这个上下文最初创建为
 * CurTransactionContext 的子上下文，只在最后、不可能再出错时
 * 才重新挂到 CacheMemoryContext 之下。另外，除了极短的代码段，
 * 我们不把这个上下文设为当前上下文，因为担心
 * 某些被调用者自己分配的内存会永久
 * 泄漏。
 *
 * 作为一种特殊情形，被要求略去
 * 正在分离的分区（并且确实含有这种分区）的描述符是临时的，
 * 不与关系缓存项关联。这种描述符只持续到
 * 请求它的 Portal 结束，因此我们为它们使用
 * 对应的内存上下文。
 */
static PartitionDesc
RelationBuildPartitionDesc(Relation rel, bool omit_detached)
{
	PartitionDesc partdesc;
	PartitionBoundInfo boundinfo = NULL;
	List	   *inhoids;
	PartitionBoundSpec **boundspecs = NULL;
	Oid		   *oids = NULL;
	bool	   *is_leaf = NULL;
	bool		detached_exist;
	bool		is_omit;
	TransactionId detached_xmin;
	ListCell   *cell;
	int			i,
				nparts;
	bool		retried = false;
	PartitionKey key = RelationGetPartitionKey(rel);
	MemoryContext new_pdcxt;
	MemoryContext oldcxt;
	int		   *mapping;

retry:

	/*
	 * 从 pg_inherits 取得分区 OID。这用单个快照
	 * 取出子关系列表，因此尽管可能有更多子关系正在被并发添加
	 * 或移除，本函数返回的结果在某个定义明确的时间点上
	 * 是准确的。
	 */
	detached_exist = false;
	detached_xmin = InvalidTransactionId;
	inhoids = find_inheritance_children_extended(RelationGetRelid(rel),
												 omit_detached, NoLock,
												 &detached_exist,
												 &detached_xmin);

	nparts = list_length(inhoids);

	/* 为 OIDs、叶子标志和边界说明分配工作数组。 */
	if (nparts > 0)
	{
		oids = (Oid *) palloc(nparts * sizeof(Oid));
		is_leaf = (bool *) palloc(nparts * sizeof(bool));
		boundspecs = palloc(nparts * sizeof(PartitionBoundSpec *));
	}

	/* 收集每个分区的边界说明节点。 */
	i = 0;
	foreach(cell, inhoids)
	{
		Oid			inhrelid = lfirst_oid(cell);
		HeapTuple	tuple;
		PartitionBoundSpec *boundspec = NULL;

		/* 为了速度，尝试从系统缓存取元组。 */
		tuple = SearchSysCache1(RELOID, inhrelid);
		if (HeapTupleIsValid(tuple))
		{
			Datum		datum;
			bool		isnull;

			datum = SysCacheGetAttr(RELOID, tuple,
									Anum_pg_class_relpartbound,
									&isnull);
			if (!isnull)
				boundspec = stringToNode(TextDatumGetCString(datum));
			ReleaseSysCache(tuple);
		}

		/*
		 * 这里可能有两个问题。首先，并发的 ATTACH
		 * PARTITION 可能正在添加新分区，但
		 * 系统缓存里还没有它，或者缓存副本尚未设置
		 * 它的 relpartbound。我们无法直接 AcceptInvalidationMessages()，
		 * 因为对方进程可能已经把自己从
		 * ProcArray 里摘掉，但还没把失效消息加进
		 * 共享队列。解决办法是直接读 pg_class
		 * 以取得所需元组。
		 *
		 * 若最近分离的分区也被删除，扫描就得不到元组。
		 * 那种情况下我们也重试，下次经过
		 * 这里时就不再看到那个分区。
		 *
		 * 另一个问题是 DETACH CONCURRENTLY 正在
		 * 移除分区，这分两步：先把它标记为
		 * 分离待定，提交，然后清除 relpartbound。若
		 * find_inheritance_children_extended 包含了那个分区，但我们
		 * 在下面看到 DETACH CONCURRENTLY 已为它重置了 relpartbound，
		 * 就会看到不一致的视图。（不一致是因为
		 * 下面的 table_open 会读取失效消息。）我们通过
		 * 重试 find_inheritance_children_extended() 来防范。
		 */
		if (boundspec == NULL)
		{
			Relation	pg_class;
			SysScanDesc scan;
			ScanKeyData key[1];

			pg_class = table_open(RelationRelationId, AccessShareLock);
			ScanKeyInit(&key[0],
						Anum_pg_class_oid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(inhrelid));
			scan = systable_beginscan(pg_class, ClassOidIndexId, true,
									  NULL, 1, key);

			/*
			 * 扫描可能得到一个元组（正常情形），也可能得到零
			 * 个元组，如果表在此期间已被删除。
			 */
			tuple = systable_getnext(scan);
			if (HeapTupleIsValid(tuple))
			{
				Datum		datum;
				bool		isnull;

				datum = heap_getattr(tuple, Anum_pg_class_relpartbound,
									 RelationGetDescr(pg_class), &isnull);
				if (!isnull)
					boundspec = stringToNode(TextDatumGetCString(datum));
			}
			systable_endscan(scan);
			table_close(pg_class, AccessShareLock);

			/*
			 * 若仍然得不到 relpartbound 值（要么因为
			 * boundspec 为空，要么因为没有元组），那一定
			 * 是因为 DETACH CONCURRENTLY。按上面的说明从顶部重新开始。
			 * 我们只做一次，有两个理由：第一，
			 * 同一时刻只有一个 DETACH CONCURRENTLY 会话能影响我们，
			 * 因为它们每一个都得等待本函数正在使用的快照；第二，
			 * 以免目录损坏时可能出现无限
			 * 循环。
			 *
			 * 注意当前内存上下文寿命足够短，因此
			 * 这里不必担心内存泄漏。
			 */
			if (!boundspec && !retried)
			{
				AcceptInvalidationMessages();
				retried = true;
				goto retry;
			}
		}

		/* 健全性检查。 */
		if (!boundspec)
			elog(ERROR, "missing relpartbound for relation %u", inhrelid);
		if (!IsA(boundspec, PartitionBoundSpec))
			elog(ERROR, "invalid relpartbound for relation %u", inhrelid);

		/*
		 * 若 PartitionBoundSpec 说这是默认分区，它的
		 * OID 应当匹配 pg_partitioned_table.partdefid；若不匹配，
		 * 目录就是损坏的。
		 */
		if (boundspec->is_default)
		{
			Oid			partdefid;

			partdefid = get_default_partition_oid(RelationGetRelid(rel));
			if (partdefid != inhrelid)
				elog(ERROR, "expected partdefid %u, but got %u",
					 inhrelid, partdefid);
		}

		/* 保存结果。 */
		oids[i] = inhrelid;
		is_leaf[i] = (get_rel_relkind(inhrelid) != RELKIND_PARTITIONED_TABLE);
		boundspecs[i] = boundspec;
		++i;
	}

	/*
	 * 在调用方的上下文里创建 PartitionBoundInfo 和映射。
	 * 这可能失败，但若失败我们还没有造成破坏。
	 */
	if (nparts > 0)
		boundinfo = partition_bounds_create(boundspecs, nparts, key, &mapping);

	/*
	 * 现在构造真正的关系缓存分区描述符，把全部
	 * 数据复制进一个新的小上下文。按上面的注释，在完成之前
	 * 我们不把它做成长寿命上下文。
	 */
	new_pdcxt = AllocSetContextCreate(CurTransactionContext,
									  "partition descriptor",
									  ALLOCSET_SMALL_SIZES);
	MemoryContextCopyAndSetIdentifier(new_pdcxt,
									  RelationGetRelationName(rel));

	partdesc = (PartitionDescData *)
		MemoryContextAllocZero(new_pdcxt, sizeof(PartitionDescData));
	partdesc->nparts = nparts;
	partdesc->detached_exist = detached_exist;
	/* 若没有分区，分区描述符的其余部分可以保持为零 */
	if (nparts > 0)
	{
		oldcxt = MemoryContextSwitchTo(new_pdcxt);
		partdesc->boundinfo = partition_bounds_copy(boundinfo, key);

		/* 初始化缓存成员，以加速 ExecFindPartition */
		partdesc->last_found_datum_index = -1;
		partdesc->last_found_part_index = -1;
		partdesc->last_found_count = 0;

		partdesc->oids = (Oid *) palloc(nparts * sizeof(Oid));
		partdesc->is_leaf = (bool *) palloc(nparts * sizeof(bool));

		/*
		 * 把原始数组里的 OIDs 按映射下标放进
		 * 结果数组。前者中 OID 的顺序由
		 * 取回它们的目录扫描定义，后者中的顺序则由
		 * 分区边界的规范化表示定义。
		 * 同时保存每个分区是否为叶子。
		 */
		for (i = 0; i < nparts; i++)
		{
			int			index = mapping[i];

			partdesc->oids[index] = oids[i];
			partdesc->is_leaf[index] = is_leaf[i];
		}
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 我们处理的是略去已分离分区的描述符，还是
	 * 包含它的那一份？
	 *
	 * 注意：若目录扫描发现某个分区已
	 * 分离，但说明这一点的 pg_inherit 元组对
	 * 活动快照不可见（那种情况下 find_inheritance_children_extended 不会设置
	 * detached_xmin），我们就认为没有可略去的
	 * 已分离分区。
	 */
	is_omit = omit_detached && detached_exist && ActiveSnapshotSet() &&
		TransactionIdIsValid(detached_xmin);

	/*
	 * 我们有了一份完全有效的分区描述符。重新挂接父上下文，使它拥有正确的
	 * 寿命。
	 */
	MemoryContextSetParent(new_pdcxt, CacheMemoryContext);

	/*
	 * 把它存进关系缓存。
	 *
	 * 但首先有个权宜之计：若这种描述符已有旧上下文，
	 * 它含有一份可能仍在某处被引用的旧分区描述符。
	 * 保留它，同时不泄漏，办法是
	 * 把它重新挂成新上下文的子上下文。最终它会
	 * 被 RelationClose 或 RelationClearRelation 丢掉。（常规分区描述符放在
	 * rd_pdcxt 里，不含已分离分区的那份放在
	 * rd_pddcxt 里。）
	 */
	if (is_omit)
	{
		if (rel->rd_pddcxt != NULL)
			MemoryContextSetParent(rel->rd_pddcxt, new_pdcxt);
		rel->rd_pddcxt = new_pdcxt;
		rel->rd_partdesc_nodetached = partdesc;

		/*
		 * 对排除已分离分区而构造的描述符，我们单独保存，
		 * 同时记录被略去的已分离
		 * 分区的 pg_inherits.xmin；这告诉以后可能使用
		 * 这种缓存描述符的人：只有在交叉确认该
		 * xmin 对它将要使用的快照确实可见之后，
		 * 才能使用。
		 */
		Assert(TransactionIdIsValid(detached_xmin));
		rel->rd_partdesc_nodetached_xmin = detached_xmin;
	}
	else
	{
		if (rel->rd_pdcxt != NULL)
			MemoryContextSetParent(rel->rd_pdcxt, new_pdcxt);
		rel->rd_pdcxt = new_pdcxt;
		rel->rd_partdesc = partdesc;
	}

	return partdesc;
}

/*
 * CreatePartitionDirectory
 *		创建一个新的分区目录对象。
 */
PartitionDirectory
CreatePartitionDirectory(MemoryContext mcxt, bool omit_detached)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
	PartitionDirectory pdir;
	HASHCTL		ctl;

	pdir = palloc(sizeof(PartitionDirectoryData));
	pdir->pdir_mcxt = mcxt;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(PartitionDirectoryEntry);
	ctl.hcxt = mcxt;

	pdir->pdir_hash = hash_create("partition directory", 256, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	pdir->omit_detached = omit_detached;

	MemoryContextSwitchTo(oldcontext);
	return pdir;
}

/*
 * PartitionDirectoryLookup
 *		在目录里查找关系的分区描述符。
 *
 * 本函数的目的是保证每次查找时，同一关系都得到同一个
 * PartitionDesc。面对并发 DDL，不同的 PartitionDescs 可能
 * 按对目录状态的不同视图来构造，但任何一个特定的 OID
 * 只要一直使用同一个
 * PartitionDirectory，就总会得到同一个
 * PartitionDesc。
 */
PartitionDesc
PartitionDirectoryLookup(PartitionDirectory pdir, Relation rel)
{
	PartitionDirectoryEntry *pde;
	Oid			relid = RelationGetRelid(rel);
	bool		found;

	pde = hash_search(pdir->pdir_hash, &relid, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * 必须保持对关系的引用计数，这样我们指向的
		 * PartitionDesc 才不会被销毁。
		 */
		RelationIncrementReferenceCount(rel);
		pde->rel = rel;
		pde->pd = RelationGetPartitionDesc(rel, pdir->omit_detached);
		Assert(pde->pd != NULL);
	}
	return pde->pd;
}

/*
 * DestroyPartitionDirectory
 *		销毁分区目录。
 *
 * 释放我们持有的引用计数。
 */
void
DestroyPartitionDirectory(PartitionDirectory pdir)
{
	HASH_SEQ_STATUS status;
	PartitionDirectoryEntry *pde;

	hash_seq_init(&status, pdir->pdir_hash);
	while ((pde = hash_seq_search(&status)) != NULL)
		RelationDecrementReferenceCount(pde->rel);
}

/*
 * get_default_oid_from_partdesc
 *
 * 给定分区描述符，若存在默认分区就返回它的 OID，
 * 否则返回 InvalidOid。
 */
Oid
get_default_oid_from_partdesc(PartitionDesc partdesc)
{
	if (partdesc && partdesc->boundinfo &&
		partition_bound_has_default(partdesc->boundinfo))
		return partdesc->oids[partdesc->boundinfo->default_index];

	return InvalidOid;
}
