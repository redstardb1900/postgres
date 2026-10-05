/*-------------------------------------------------------------------------
 *
 * rewriteheap.c
 *	  重写表所用的支持函数。
 *
 * 这些函数提供一套手段，可以完整重写一个堆，同时
 * 保留可见性信息与更新链。
 *
 * 接口（INTERFACE）
 *
 * 调用方负责创建新堆、完成全部系统目录
 * 变更、提供要写入新堆的元组，以及
 * 重建索引。调用方必须对目标表持有 AccessExclusiveLock，
 * 因为我们假定没有别人正在向它写入。
 *
 * 使用方法：
 *
 * begin_heap_rewrite
 * while (取下一个元组)
 * {
 *	   if (元组已死)
 *		   rewrite_heap_dead_tuple
 *	   else
 *	   {
 *		   // 若需要，在此做任何变换
 *		   rewrite_heap_tuple
 *	   }
 * }
 * end_heap_rewrite
 *
 * 在调用 end_heap_rewrite 之前，不应依赖
 * 新关系的内容。
 *
 *
 * 实现（IMPLEMENTATION）
 *
 * 若不是必须维护把同一行各更新版本链接起来的 ctid 链，
 * 这件事本来相当简单。
 * 新存入的元组其 tid 与原来的不同，
 * 若只是把 t_ctid 字段复制到新表，链接就会
 * 是错的。当必须复制一个（多半是最近已死或
 * 删除进行中的）元组，且其 ctid 并不指向自身时，我们
 * 必须改填正确的 ctid。
 *
 * 对每一处从 A 指向 B 的 ctid 引用，我们可能先遇到 A，
 * 也可能先遇到 B。（注意，链中间的元组既是 A 也是 B，
 * 只是分属不同的配对。）
 *
 * 若先遇到 A，就把该元组存入 unresolved_tups
 * 哈希表。稍后遇到 B 时，把 A 从哈希表中移除，
 * 把 ctid 改成指向 B 的新位置，并把 A 和 B 都插入
 * 新堆。
 *
 * 若先遇到 B，就可以立刻把 B 插入新堆。
 * 然后往 old_new_tid_map 哈希表加一条，记下 B 的
 * 原 tid（在旧堆中）和新 tid（在新堆中）。
 * 稍后遇到 A 时，从表中取得 B 的新位置，
 * 就可以立刻写入带有正确 ctid 的 A。
 *
 * 一旦遇到配对中较晚的那个元组，哈希表条目就可以移除。
 * 这有助于压低内存用量。到结束时，
 * 两张表通常都是空的；每一对的 A 和 B
 * 我们都应该遇到过。不过按 HeapTupleSatisfiesVacuum，A 可能是 RECENTLY_DEAD，而 B
 * 则完全是 DEAD，这是按 HeapTupleSatisfiesVacuum 得出的，因为该项测试
 * 用 OldestXmin 判断死亡并不精确。在这种情况下我们可能
 * 先遇到 B 并跳过它，稍后才找到 A。于是 A 会被加入
 * unresolved_tups，并一直留到重写结束。由于
 * 这种情形非常少见，我们不担心其内存用量。
 *
 * 使用内存中的哈希表意味着，对表里每一条仍然活着的
 * 更新链，从找到引用的一端开始，
 * 直到找到另一端，都要占用一些内存。实践中这不应当成为问题。
 * 但若你在大表上做一次不带 WHERE 子句的 UPDATE，
 * 然后在同一事务里运行 CLUSTER，
 * 就可能内存不足。似乎不值得为此增加
 * 溢出到磁盘的支持，因为正常情况下表里不该有那么多 RECENTLY_DEAD 元组。
 * 此外，在典型情形中，
 * 对一个不变的键列做 CLUSTERing 时，某一元组的所有版本
 * 反正会一起出现，因此内存峰值只
 * 与单行的 RECENTLY_DEAD 版本数成正比，而不是
 * 与整张表成正比。注意，若 CLUSTER 做到一半失败，
 * 旧表仍然有效，所以失败并非灾难性的。
 *
 * 不能用普通的 heap_insert 函数往新
 * 堆里插入，因为 heap_insert 会覆盖可见性信息。
 * 我们改用专用的 raw_heap_insert 函数，它
 * 针对批量插入大量元组做了优化，并且知道我们
 * 对该堆拥有排他访问。raw_heap_insert 在
 * 本地存储中构建新页。当一页已满，或过程结束时，
 * 我们把它作为单条记录插入 WAL，然后直接经 smgr
 * 写到磁盘。不过请注意，发往新
 * 堆的 TOAST 表的任何数据仍走正常的 bufmgr。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/rewriteheap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/heaptoast.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "common/file_utils.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/slot.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * 与一次重写操作相关联的状态。它对
 * 重写功能的使用者而言是不透明的。
 */
typedef struct RewriteStateData
{
	Relation	rs_old_rel;		/* 源堆 */
	Relation	rs_new_rel;		/* 目标堆 */
	Page		rs_buffer;		/* 当前正在构建的页 */
	BlockNumber rs_blockno;		/* 该页将放入的块 */
	bool		rs_buffer_valid;	/* 若缓冲区中有任何元组则为 T */
	bool		rs_logical_rewrite; /* 是否需要做逻辑重写 */
	TransactionId rs_oldest_xmin;	/* 调用方用来判定
									 * 元组可见性的最旧 xmin */
	TransactionId rs_freeze_xid;	/* 将用作冻结截止
									 * 点的事务 ID（Xid） */
	TransactionId rs_logical_xmin;	/* 将用作截止点的事务 ID（Xid），
									 * 供逻辑重写使用 */
	MultiXactId rs_cutoff_multi;	/* 将用作截止点的 MultiXactId，
									 * 针对多事务 */
	MemoryContext rs_cxt;		/* 供哈希表、其中的条目以及
								 * 其中的元组使用 */
	XLogRecPtr	rs_begin_lsn;	/* 开始重写时的 XLogInsertLsn */
	HTAB	   *rs_unresolved_tups; /* 尚未配对的 A 元组 */
	HTAB	   *rs_old_new_tid_map; /* 尚未配对的 B 元组 */
	HTAB	   *rs_logical_mappings;	/* 逻辑重映射文件 */
	uint32		rs_num_rewrite_mappings;	/* 内存中的映射条数 */
}			RewriteStateData;

/*
 * 哈希表的查找键是元组 TID 与 xmin（两者都必须检查，
 * 以免死元组造成假匹配）。注意这个结构里
 * 可能有填充空隙；为使哈希表正确工作，必须把它清零。
 * 正确工作。
 */
typedef struct
{
	TransactionId xmin;			/* 元组的 xmin */
	ItemPointerData tid;		/* 元组在旧堆中的位置 */
} TidHashKey;

/*
 * 哈希表的条目结构
 */
typedef struct
{
	TidHashKey	key;			/* B 元组预期的 xmin/旧位置 */
	ItemPointerData old_tid;	/* A 在旧堆中的位置 */
	HeapTuple	tuple;			/* A 的元组内容 */
} UnresolvedTupData;

typedef UnresolvedTupData *UnresolvedTup;

typedef struct
{
	TidHashKey	key;			/* B 元组实际的 xmin/旧位置 */
	ItemPointerData new_tid;	/* 我们把它放在新堆中的位置 */
} OldToNewMappingData;

typedef OldToNewMappingData *OldToNewMapping;

/*
 * 某个 xid 的内存数据，该 xid 可能需要把逻辑重映射条目
 * 记入日志。
 */
typedef struct RewriteMappingFile
{
	TransactionId xid;			/* 可能需要看到该行的 xid */
	int			vfd;			/* 映射文件的 fd */
	off_t		off;			/* 目前已经写到的位置 */
	dclist_head mappings;		/* 内存中的映射列表 */
	char		path[MAXPGPATH];	/* 路径，用于错误信息 */
} RewriteMappingFile;

/*
 * 单条内存中的逻辑重写映射，挂在
 * RewriteMappingFile->mappings.
 */
typedef struct RewriteMappingDataEntry
{
	LogicalRewriteMappingData map;	/* 元组旧位置与新位置之间的
									 * 映射 */
	dlist_node	node;
} RewriteMappingDataEntry;


/* 内部函数的原型 */
static void raw_heap_insert(RewriteState state, HeapTuple tup);

/* 内部逻辑重映射函数的原型 */
static void logical_begin_heap_rewrite(RewriteState state);
static void logical_rewrite_heap_tuple(RewriteState state, ItemPointerData old_tid, HeapTuple new_tuple);
static void logical_end_heap_rewrite(RewriteState state);


/*
 * 开始重写一张表
 *
 * old_heap		从中读取元组的、已锁定的旧堆关系
 * new_heap		要插入元组的、已锁定的新堆关系
 * oldest_xmin	调用方用来判定哪些元组已死的 xid
 * freeze_xid	此 xid 之前的元组将被冻结
 * cutoff_multi	此多事务之前的多事务将被移除
 *
 * 返回一个不透明的 RewriteState，分配在当前内存上下文中，
 * 供随后调用其他函数时使用。
 */
RewriteState
begin_heap_rewrite(Relation old_heap, Relation new_heap, TransactionId oldest_xmin,
				   TransactionId freeze_xid, MultiXactId cutoff_multi)
{
	RewriteState state;
	MemoryContext rw_cxt;
	MemoryContext old_cxt;
	HASHCTL		hash_ctl;

	/*
	 * 为便于清理，单独建一个上下文，其中将包含
	 * RewriteState 结构本身以及全部附属数据。
	 */
	rw_cxt = AllocSetContextCreate(CurrentMemoryContext,
								   "Table rewrite",
								   ALLOCSET_DEFAULT_SIZES);
	old_cxt = MemoryContextSwitchTo(rw_cxt);

	/* 创建并填写状态结构 */
	state = palloc0(sizeof(RewriteStateData));

	state->rs_old_rel = old_heap;
	state->rs_new_rel = new_heap;
	state->rs_buffer = (Page) palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);
	/* new_heap 不必为空，只要已锁定即可 */
	state->rs_blockno = RelationGetNumberOfBlocks(new_heap);
	state->rs_buffer_valid = false;
	state->rs_oldest_xmin = oldest_xmin;
	state->rs_freeze_xid = freeze_xid;
	state->rs_cutoff_multi = cutoff_multi;
	state->rs_cxt = rw_cxt;

	/* 初始化用于跟踪更新链的哈希表 */
	hash_ctl.keysize = sizeof(TidHashKey);
	hash_ctl.entrysize = sizeof(UnresolvedTupData);
	hash_ctl.hcxt = state->rs_cxt;

	state->rs_unresolved_tups =
		hash_create("Rewrite / Unresolved ctids",
					128,		/* 任意的初始大小 */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	hash_ctl.entrysize = sizeof(OldToNewMappingData);

	state->rs_old_new_tid_map =
		hash_create("Rewrite / Old to new tid map",
					128,		/* 任意的初始大小 */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	MemoryContextSwitchTo(old_cxt);

	logical_begin_heap_rewrite(state);

	return state;
}

/*
 * 结束一次重写。
 *
 * state 以及其他任何资源都被释放。
 */
void
end_heap_rewrite(RewriteState state)
{
	HASH_SEQ_STATUS seq_status;
	UnresolvedTup unresolved;

	/*
	 * 写出 UnresolvedTups 表中剩余的元组。若还有剩余，
	 * 它们其实应当已经死亡，但我们宁可保守处理。
	 */
	hash_seq_init(&seq_status, state->rs_unresolved_tups);

	while ((unresolved = hash_seq_search(&seq_status)) != NULL)
	{
		ItemPointerSetInvalid(&unresolved->tuple->t_data->t_ctid);
		raw_heap_insert(state, unresolved->tuple);
	}

	/* 若还有最后一页，把它写出去 */
	if (state->rs_buffer_valid)
	{
		if (RelationNeedsWAL(state->rs_new_rel))
			log_newpage(&state->rs_new_rel->rd_locator,
						MAIN_FORKNUM,
						state->rs_blockno,
						state->rs_buffer,
						true);

		PageSetChecksumInplace(state->rs_buffer, state->rs_blockno);

		smgrextend(RelationGetSmgr(state->rs_new_rel), MAIN_FORKNUM,
				   state->rs_blockno, state->rs_buffer, true);
	}

	/*
	 * 即便这些关系页已经记入 WAL，我们仍必须对它们 fsync。原因
	 * 与 storage.c 中 RelationCopyStorage() 相同：我们写的
	 * 数据不在共享缓冲池里，因此重写期间发生的 CHECKPOINT
	 * 不会对属于这次 rewriteheap 操作的数据做 fsync，那些数据写在检查点
	 * 之前。
	 */
	if (RelationNeedsWAL(state->rs_new_rel))
		smgrimmedsync(RelationGetSmgr(state->rs_new_rel), MAIN_FORKNUM);

	logical_end_heap_rewrite(state);

	/* 删除该上下文就会释放一切 */
	MemoryContextDelete(state->rs_cxt);
}

/*
 * 把一个元组加入新堆。
 *
 * 可见性信息从原始元组复制而来，只是
 * 我们会"冻结"非常旧的元组。注意，由于我们会就地改写 new_tuple，
 * 它最好是临时存储，而不是指向原始元组的指针。
 *
 * state		由 begin_heap_rewrite 返回的不透明状态
 * old_tuple	旧堆中的原始元组
 * new_tuple	要插入新堆的、重写后的新元组
 */
void
rewrite_heap_tuple(RewriteState state,
				   HeapTuple old_tuple, HeapTuple new_tuple)
{
	MemoryContext old_cxt;
	ItemPointerData old_tid;
	TidHashKey	hashkey;
	bool		found;
	bool		free_new;

	old_cxt = MemoryContextSwitchTo(state->rs_cxt);

	/*
	 * 把原始元组的可见性信息复制到 new_tuple。
	 *
	 * XXX 以后是否还需要复制一些 t_infomask2 位？目前
	 * 我们有意清除 HOT 状态位。
	 */
	memcpy(&new_tuple->t_data->t_choice.t_heap,
		   &old_tuple->t_data->t_choice.t_heap,
		   sizeof(HeapTupleFields));

	new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	new_tuple->t_data->t_infomask |=
		old_tuple->t_data->t_infomask & HEAP_XACT_MASK;

	/*
	 * 既然手里正拿着这个元组，不妨把任何
	 * 符合条件的 xmin 或 xmax 冻结，以便将来省下 VACUUM 的工作。
	 */
	heap_freeze_tuple(new_tuple->t_data,
					  state->rs_old_rel->rd_rel->relfrozenxid,
					  state->rs_old_rel->rd_rel->relminmxid,
					  state->rs_freeze_xid,
					  state->rs_cutoff_multi);

	/*
	 * 无效的 ctid 表示 ctid 应当指向元组自身。若该元组
	 * 属于一条更新链，我们稍后会改写它。
	 */
	ItemPointerSetInvalid(&new_tuple->t_data->t_ctid);

	/*
	 * 若元组已被更新，就查旧到新的映射哈希表。
	 */
	if (!((old_tuple->t_data->t_infomask & HEAP_XMAX_INVALID) ||
		  HeapTupleHeaderIsOnlyLocked(old_tuple->t_data)) &&
		!HeapTupleHeaderIndicatesMovedPartitions(old_tuple->t_data) &&
		!(ItemPointerEquals(&(old_tuple->t_self),
							&(old_tuple->t_data->t_ctid))))
	{
		OldToNewMapping mapping;

		memset(&hashkey, 0, sizeof(hashkey));
		hashkey.xmin = HeapTupleHeaderGetUpdateXid(old_tuple->t_data);
		hashkey.tid = old_tuple->t_data->t_ctid;

		mapping = (OldToNewMapping)
			hash_search(state->rs_old_new_tid_map, &hashkey,
						HASH_FIND, NULL);

		if (mapping != NULL)
		{
			/*
			 * t_ctid 指向的元组我们已经复制过，因此可以把
			 * 这个元组的 ctid 设为指向新位置，并且
			 * 立刻插入它。
			 */
			new_tuple->t_data->t_ctid = mapping->new_tid;

			/* 这条映射条目不再需要 */
			hash_search(state->rs_old_new_tid_map, &hashkey,
						HASH_REMOVE, &found);
			Assert(found);
		}
		else
		{
			/*
			 * 还没见过 t_ctid 指向的元组。把这个
			 * 元组暂存进 unresolved_tups，稍后再写。
			 */
			UnresolvedTup unresolved;

			unresolved = hash_search(state->rs_unresolved_tups, &hashkey,
									 HASH_ENTER, &found);
			Assert(!found);

			unresolved->old_tid = old_tuple->t_self;
			unresolved->tuple = heap_copytuple(new_tuple);

			/*
			 * 现在不能再做什么，因为我们还不知道这个
			 * 元组将被写到哪里。
			 */
			MemoryContextSwitchTo(old_cxt);
			return;
		}
	}

	/*
	 * 现在写出该元组，然后检查它是否是某个新的或已知配对中的 B 元组。
	 * 解析一个已知配对后，我们就能够
	 * 写出该配对的 A 元组，接着还得检查它是否
	 * 又解析了别的配对。因此这里需要一个循环。
	 */
	old_tid = old_tuple->t_self;
	free_new = false;

	for (;;)
	{
		ItemPointerData new_tid;

		/* 插入该元组，并查明它在 new_heap 中的位置 */
		raw_heap_insert(state, new_tuple);
		new_tid = new_tuple->t_self;

		logical_rewrite_heap_tuple(state, old_tid, new_tuple);

		/*
		 * 若该元组是某行的更新版本，且前一版本
		 * 还不会是 DEAD，则要么解析前一
		 * 版本（若它正在 rs_unresolved_tups 中等待），要么在
		 * rs_old_new_tid_map 中加一条（以便我们见到它时再解析）。前一个
		 * 元组的 xmax 等于这一个的 xmin，因此当且仅当 xmin 不早于 OldestXmin 时，它才是
		 * RECENTLY_DEAD。
		 */
		if ((new_tuple->t_data->t_infomask & HEAP_UPDATED) &&
			!TransactionIdPrecedes(HeapTupleHeaderGetXmin(new_tuple->t_data),
								   state->rs_oldest_xmin))
		{
			/*
			 * 好，这是更新配对中的 B。看看我们是否已经见过 A。
			 */
			UnresolvedTup unresolved;

			memset(&hashkey, 0, sizeof(hashkey));
			hashkey.xmin = HeapTupleHeaderGetXmin(new_tuple->t_data);
			hashkey.tid = old_tid;

			unresolved = hash_search(state->rs_unresolved_tups, &hashkey,
									 HASH_FIND, NULL);

			if (unresolved != NULL)
			{
				/*
				 * 前一个元组我们已经见过并记住了。现在
				 * 既然知道所插入的、被其 t_ctid 指向的元组
				 * 位于何处，就修正它的 t_ctid 并插入新堆。
				 */
				if (free_new)
					heap_freetuple(new_tuple);
				new_tuple = unresolved->tuple;
				free_new = true;
				old_tid = unresolved->old_tid;
				new_tuple->t_data->t_ctid = new_tid;

				/*
				 * 这条哈希条目不再需要，但先不要释放它的
				 * 元组。
				 */
				hash_search(state->rs_unresolved_tups, &hashkey,
							HASH_REMOVE, &found);
				Assert(found);

				/* 循环回去，插入链中的前一个元组 */
				continue;
			}
			else
			{
				/*
				 * 记住这个元组的新 tid。找到链中前一个元组时，我们用它来设置
				 * ctid。
				 */
				OldToNewMapping mapping;

				mapping = hash_search(state->rs_old_new_tid_map, &hashkey,
									  HASH_ENTER, &found);
				Assert(!found);

				mapping->new_tid = new_tid;
			}
		}

		/* 这一（串）元组暂时处理完了 */
		if (free_new)
			heap_freetuple(new_tuple);
		break;
	}

	MemoryContextSwitchTo(old_cxt);
}

/*
 * 向正在进行的重写登记一个死元组。死元组不会
 * 被复制到新表，但我们仍记下它们，以便
 * 更早释放一些资源。
 *
 * 若从 unresolved_tups 表中移除了一个元组，则返回真。
 * 这表示该元组原先被认为"最近已死"，
 * 现在已知确实已死，不会被写到输出中。
 */
bool
rewrite_heap_dead_tuple(RewriteState state, HeapTuple old_tuple)
{
	/*
	 * 若我们已经见过更新链中更早的、指向
	 * 这个元组的那个元组，就忘掉那个更早的元组。它其实
	 * 同样已死，只是 HeapTupleSatisfiesVacuum 里简单的 xmax < OldestXmin 测试
	 * 不足以发现这一点。这种情况出现在
	 * 元组的 xmin 大于 xmax 的时候，听起来
	 * 违反直觉，但完全有效。
	 *
	 * 我们不费力去检测相反的情形，
	 * 即先遇到死元组，然后才遇到指向它的最近已死元组。
	 * 若发生那种情况，结束时 UnresolvedTups 哈希表里会有一些无法配对的条目。
	 * 反正也可能发生，
	 * 因为清理可能在我们之前就已经移除了链中的死元组。
	 * 赶在我们之前。
	 */
	UnresolvedTup unresolved;
	TidHashKey	hashkey;
	bool		found;

	memset(&hashkey, 0, sizeof(hashkey));
	hashkey.xmin = HeapTupleHeaderGetXmin(old_tuple->t_data);
	hashkey.tid = old_tuple->t_self;

	unresolved = hash_search(state->rs_unresolved_tups, &hashkey,
							 HASH_FIND, NULL);

	if (unresolved != NULL)
	{
		/* 需要释放其中包含的元组以及哈希表条目 */
		heap_freetuple(unresolved->tuple);
		hash_search(state->rs_unresolved_tups, &hashkey,
					HASH_REMOVE, &found);
		Assert(found);
		return true;
	}

	return false;
}

/*
 * 把元组插入新关系。这必须跟上 heap_insert
 * 及其附属函数！
 *
 * 元组的 t_self 被设为该元组的新 TID。若进入时元组的 t_ctid
 * 无效，则同样替换为新 TID（只改
 * 插入的数据，不改调用方的副本）。
 */
static void
raw_heap_insert(RewriteState state, HeapTuple tup)
{
	Page		page = state->rs_buffer;
	Size		pageFreeSpace,
				saveFreeSpace;
	Size		len;
	OffsetNumber newoff;
	HeapTuple	heaptup;

	/*
	 * 若新元组大到无法存储，或含有来自其他关系的、已经做过 TOAST 的
	 * 行外属性，就调用 TOAST 处理。
	 *
	 * 注意：从此往下，heaptup 是我们实际要存入
	 * 该关系的数据；tup 是调用方原来的、未做 TOAST 的数据。
	 */
	if (state->rs_new_rel->rd_rel->relkind == RELKIND_TOASTVALUE)
	{
		/* TOAST 表的条目绝不应当被递归地做 TOAST */
		Assert(!HeapTupleHasExternal(tup));
		heaptup = tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
	{
		int			options = HEAP_INSERT_SKIP_FSM;

		/*
		 * 为 VACUUM FULL / CLUSTER 重写堆时，确保
		 * TOAST 表的数据不会被逻辑解码。主堆以
		 * XLOG 整页镜像（FPI）记录的形式记入 WAL，而那些记录不会被逻辑解码。
		 */
		options |= HEAP_INSERT_NO_LOGICAL;

		heaptup = heap_toast_insert_or_update(state->rs_new_rel, tup, NULL,
											  options);
	}
	else
		heaptup = tup;

	len = MAXALIGN(heaptup->t_len); /* 采取保守做法 */

	/*
	 * 若会因元组过大而失败，就立刻失败
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						len, MaxHeapTupleSize)));

	/* 按 fillfactor 选项计算期望的额外空闲空间 */
	saveFreeSpace = RelationGetTargetPageFreeSpace(state->rs_new_rel,
												   HEAP_DEFAULT_FILLFACTOR);

	/* 现在可以检查是否已经有足够的空闲空间。 */
	if (state->rs_buffer_valid)
	{
		pageFreeSpace = PageGetHeapFreeSpace(page);

		if (len + saveFreeSpace > pageFreeSpace)
		{
			/*
			 * 放不下，因此把现有页写出去。该页总是
			 * 含有一个元组。因此与 RelationGetBufferForTuple() 不同，
			 * 无条件地强制 saveFreeSpace。
			 */

			/* XLOG 相关处理 */
			if (RelationNeedsWAL(state->rs_new_rel))
				log_newpage(&state->rs_new_rel->rd_locator,
							MAIN_FORKNUM,
							state->rs_blockno,
							page,
							true);

			/*
			 * 现在写这一页。我们令 skipFsync = true，因为没有
			 * 必要让 smgr 为这次写入安排 fsync；我们会在
			 * end_heap_rewrite 里自己做。
			 */
			PageSetChecksumInplace(page, state->rs_blockno);

			smgrextend(RelationGetSmgr(state->rs_new_rel), MAIN_FORKNUM,
					   state->rs_blockno, page, true);

			state->rs_blockno++;
			state->rs_buffer_valid = false;
		}
	}

	if (!state->rs_buffer_valid)
	{
		/* 初始化一个新的空页 */
		PageInit(page, BLCKSZ, 0);
		state->rs_buffer_valid = true;
	}

	/* 现在可以把元组插入该页 */
	newoff = PageAddItem(page, (Item) heaptup->t_data, heaptup->t_len,
						 InvalidOffsetNumber, false, true);
	if (newoff == InvalidOffsetNumber)
		elog(ERROR, "failed to add tuple");

	/* 把调用方的 t_self 更新为它实际被存放的位置 */
	ItemPointerSet(&(tup->t_self), state->rs_blockno, newoff);

	/*
	 * 若调用方没有提供有效的 CTID，也把正确位置写入所存元组的 CTID。
	 * 调用方没有提供有效 CTID 时才这样做。
	 */
	if (!ItemPointerIsValid(&tup->t_data->t_ctid))
	{
		ItemId		newitemid;
		HeapTupleHeader onpage_tup;

		newitemid = PageGetItemId(page, newoff);
		onpage_tup = (HeapTupleHeader) PageGetItem(page, newitemid);

		onpage_tup->t_ctid = tup->t_self;
	}

	/* 若 heaptup 是私有副本，就释放它。 */
	if (heaptup != tup)
		heap_freetuple(heaptup);
}

/* ------------------------------------------------------------------------
 * 逻辑重写支持
 *
 * 做逻辑解码时——它依赖系统目录元组的 cmin/cmax，
 * 经由 xl_heap_new_cid 记录——堆重写必须记下足够的
 * 信息，让执行解码的后端更新其内部映射，
 * 使 (relfilelocator,ctid) => (cmin, cmax) 对重写后的堆仍然正确。
 *
 * 为此，每当发现一个在系统目录关系中被修改过的元组，
 * 且落在任一解码复制槽的 xmin 视界之内，我们就记录一条从旧位置到新位置的映射。
 * 从旧位置到新位置。
 *
 * 为处理会中止的重写，映射文件名包含
 * 执行重写的事务的 xid，读入之前可以据此
 * 加以检查。
 *
 * 为了效率，我们不会立刻把每一条映射都溢出写到
 * 磁盘，而是攒够若干条再成批写，
 * 或者在调用了 end_heap_rewrite() 时再写。
 *
 * 崩溃安全：本模块偏离了通常的 WAL 写法，
 * 因为它无法依赖检查点把全部缓冲区刷出，从而
 * 等待缓冲区上的排他锁。通常，覆盖缓冲区修改的 XLogInsert()
 * 是在被修改的 buffer(s) 正被
 * 排他锁定时执行的，从而保证 WAL 记录和
 * 被修改的堆分别落在检查点的两侧。但因为我们记录的
 * 映射文件不在 shared_buffers 中，那种互锁不起作用。
 *
 * 因此我们只是在 XLogInsert() 之前就把映射文件写到磁盘。
 * *先*写盘。这样可以保证，要么 XLogInsert()
 * 被插在检查点的重做指针之后，要么检查点（经由
 * CheckPointLogicalRewriteHeap()）已经把（部分）映射文件刷到
 * 磁盘。尚未刷出的尾部仍可能
 * 损坏，解决办法是在 xl_heap_rewrite_mapping 记录中带上当前偏移，
 * 并在重放时把映射文件截断到该偏移。
 * 每次重写结束时，把生成的全部映射文件同步到磁盘。
 * 同步到磁盘。
 *
 * 注意，若我们只关心崩溃安全，就完全不必
 * 处理 WAL 日志——重写结束时一次 fsync()
 * 就足以保证崩溃安全。任何尚未安全刷到
 * 磁盘的映射，必定属于一个已中止（显式中止或因崩溃而中止）的事务，
 * 并且会因文件名中的 xid 要经过
 * TransactionDidCommit() 检查而被忽略。但我们希望经由物理复制支持备库，
 * 既为了可用性，也为了在那里做逻辑解码。
 * 在备库上做。
 * ------------------------------------------------------------------------
 */

/*
 * 若有必要，为在重写期间记录逻辑映射做准备。
 * 若检测到不需要记录任何东西，就阻止
 * 各个逻辑重写函数再做进一步动作。
 */
static void
logical_begin_heap_rewrite(RewriteState state)
{
	HASHCTL		hash_ctl;
	TransactionId logical_xmin;

	/*
	 * 只有当重写后的表在逻辑解码期间可以被访问时，才需要持久化这些映射；
	 * 否则就可以跳过任何
	 * 额外工作。
	 */
	state->rs_logical_rewrite =
		RelationIsAccessibleInLogicalDecoding(state->rs_old_rel);

	if (!state->rs_logical_rewrite)
		return;

	ProcArrayGetReplicationSlotXmin(NULL, &logical_xmin);

	/*
	 * 若没有正在进行的逻辑复制槽，就什么都不必做，
	 * 相关行还不可能已有重映射。该关系上的
	 * 锁保护我们免于竞态。
	 */
	if (logical_xmin == InvalidTransactionId)
	{
		state->rs_logical_rewrite = false;
		return;
	}

	state->rs_logical_xmin = logical_xmin;
	state->rs_begin_lsn = GetXLogInsertRecPtr();
	state->rs_num_rewrite_mappings = 0;

	hash_ctl.keysize = sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(RewriteMappingFile);
	hash_ctl.hcxt = state->rs_cxt;

	state->rs_logical_mappings =
		hash_create("Logical rewrite mapping",
					128,		/* 任意的初始大小 */
					&hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * 把全部逻辑内存映射刷到磁盘，但先不要 fsync。
 */
static void
logical_heap_rewrite_flush_mappings(RewriteState state)
{
	HASH_SEQ_STATUS seq_status;
	RewriteMappingFile *src;
	dlist_mutable_iter iter;

	Assert(state->rs_logical_rewrite);

	/* 没有正在进行的逻辑重写，不必遍历映射 */
	if (state->rs_num_rewrite_mappings == 0)
		return;

	elog(DEBUG1, "flushing %u logical rewrite mapping entries",
		 state->rs_num_rewrite_mappings);

	hash_seq_init(&seq_status, state->rs_logical_mappings);
	while ((src = (RewriteMappingFile *) hash_seq_search(&seq_status)) != NULL)
	{
		char	   *waldata;
		char	   *waldata_start;
		xl_heap_rewrite_mapping xlrec;
		Oid			dboid;
		uint32		len;
		int			written;
		uint32		num_mappings = dclist_count(&src->mappings);

		/* 这个文件没有任何新映射 */
		if (num_mappings == 0)
			continue;

		if (state->rs_old_rel->rd_rel->relisshared)
			dboid = InvalidOid;
		else
			dboid = MyDatabaseId;

		xlrec.num_mappings = num_mappings;
		xlrec.mapped_rel = RelationGetRelid(state->rs_old_rel);
		xlrec.mapped_xid = src->xid;
		xlrec.mapped_db = dboid;
		xlrec.offset = src->off;
		xlrec.start_lsn = state->rs_begin_lsn;

		/* 把所有映射连续写出 */
		len = num_mappings * sizeof(LogicalRewriteMappingData);
		waldata_start = waldata = palloc(len);

		/*
		 * 收集需要写出的数据，但先不要修改磁盘上的数据
		 */
		dclist_foreach_modify(iter, &src->mappings)
		{
			RewriteMappingDataEntry *pmap;

			pmap = dclist_container(RewriteMappingDataEntry, node, iter.cur);

			memcpy(waldata, &pmap->map, sizeof(pmap->map));
			waldata += sizeof(pmap->map);

			/* 从列表中移除并释放 */
			dclist_delete_from(&src->mappings, &pmap->node);
			pfree(pmap);

			/* 更新簿记 */
			state->rs_num_rewrite_mappings--;
		}

		Assert(dclist_count(&src->mappings) == 0);
		Assert(waldata == waldata_start + len);

		/*
		 * 注意，这里我们偏离了通常的 WAL 编码做法，
		 * 理由见上面的"逻辑重写支持"注释。
		 */
		written = FileWrite(src->vfd, waldata_start, len, src->off,
							WAIT_EVENT_LOGICAL_REWRITE_WRITE);
		if (written != len)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\", wrote %d of %d: %m", src->path,
							written, len)));
		src->off += len;

		XLogBeginInsert();
		XLogRegisterData((char *) (&xlrec), sizeof(xlrec));
		XLogRegisterData(waldata_start, len);

		/* 写 xlog 记录 */
		XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_REWRITE);

		pfree(waldata_start);
	}
	Assert(state->rs_num_rewrite_mappings == 0);
}

/*
 * end_heap_rewrite() 中的逻辑重映射部分。
 */
static void
logical_end_heap_rewrite(RewriteState state)
{
	HASH_SEQ_STATUS seq_status;
	RewriteMappingFile *src;

	/* 完成，没有正在进行的逻辑重写 */
	if (!state->rs_logical_rewrite)
		return;

	/* 把剩余的内存条目写出 */
	if (state->rs_num_rewrite_mappings > 0)
		logical_heap_rewrite_flush_mappings(state);

	/* 遍历我们已经写下的全部映射，并对文件做 fsync。 */
	hash_seq_init(&seq_status, state->rs_logical_mappings);
	while ((src = (RewriteMappingFile *) hash_seq_search(&seq_status)) != NULL)
	{
		if (FileSync(src->vfd, WAIT_EVENT_LOGICAL_REWRITE_SYNC) != 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", src->path)));
		FileClose(src->vfd);
	}
	/* 其余的由内存上下文清理来处理 */
}

/*
 * 为 'xid' 记录一条（旧->新）映射。
 */
static void
logical_rewrite_log_mapping(RewriteState state, TransactionId xid,
							LogicalRewriteMappingData *map)
{
	RewriteMappingFile *src;
	RewriteMappingDataEntry *pmap;
	Oid			relid;
	bool		found;

	relid = RelationGetRelid(state->rs_old_rel);

	/* 查找这个被映射 xid 的已有映射 */
	src = hash_search(state->rs_logical_mappings, &xid,
					  HASH_ENTER, &found);

	/*
	 * 还没有必要为这个 xid 做任何映射，创建
	 * 按 xid 划分的数据结构。
	 */
	if (!found)
	{
		char		path[MAXPGPATH];
		Oid			dboid;

		if (state->rs_old_rel->rd_rel->relisshared)
			dboid = InvalidOid;
		else
			dboid = MyDatabaseId;

		snprintf(path, MAXPGPATH,
				 "pg_logical/mappings/" LOGICAL_REWRITE_FORMAT,
				 dboid, relid,
				 LSN_FORMAT_ARGS(state->rs_begin_lsn),
				 xid, GetCurrentTransactionId());

		dclist_init(&src->mappings);
		src->off = 0;
		memcpy(src->path, path, sizeof(path));
		src->vfd = PathNameOpenFile(path,
									O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
		if (src->vfd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", path)));
	}

	pmap = MemoryContextAlloc(state->rs_cxt,
							  sizeof(RewriteMappingDataEntry));
	memcpy(&pmap->map, map, sizeof(LogicalRewriteMappingData));
	dclist_push_tail(&src->mappings, &pmap->node);
	state->rs_num_rewrite_mappings++;

	/*
	 * 当所有映射文件里的内存条目过多时，就把缓冲区写出。
	 * 映射文件。
	 */
	if (state->rs_num_rewrite_mappings >= 1000 /* 任意的数目 */ )
		logical_heap_rewrite_flush_mappings(state);
}

/*
 * 若有必要，对由 rewrite_heap_tuple() 从 old_tid 映射到
 * new_tuple->t_self 的元组做逻辑重映射。
 */
static void
logical_rewrite_heap_tuple(RewriteState state, ItemPointerData old_tid,
						   HeapTuple new_tuple)
{
	ItemPointerData new_tid = new_tuple->t_self;
	TransactionId cutoff = state->rs_logical_xmin;
	TransactionId xmin;
	TransactionId xmax;
	bool		do_log_xmin = false;
	bool		do_log_xmax = false;
	LogicalRewriteMappingData map;

	/* 没有正在进行的逻辑重写，不必记录任何东西 */
	if (!state->rs_logical_rewrite)
		return;

	xmin = HeapTupleHeaderGetXmin(new_tuple->t_data);
	/* 使用 *GetUpdateXid 以正确处理多事务 */
	xmax = HeapTupleHeaderGetUpdateXid(new_tuple->t_data);

	/*
	 * 当且仅当该元组是最近创建的，才记下这条映射。
	 */
	if (TransactionIdIsNormal(xmin) && !TransactionIdPrecedes(xmin, cutoff))
		do_log_xmin = true;

	if (!TransactionIdIsNormal(xmax))
	{
		/*
		 * 没有设置 xmax，不可能有任何永久性的，因此这项检查
		 * 就足够了
		 */
	}
	else if (HEAP_XMAX_IS_LOCKED_ONLY(new_tuple->t_data->t_infomask))
	{
		/* 仅仅被锁定，我们不关心 */
	}
	else if (!TransactionIdPrecedes(xmax, cutoff))
	{
		/* 元组最近被删除了，记下来 */
		do_log_xmax = true;
	}

	/* 若两者都不需要记录，就结束 */
	if (!do_log_xmin && !do_log_xmax)
		return;

	/* 填写映射信息 */
	map.old_locator = state->rs_old_rel->rd_locator;
	map.old_tid = old_tid;
	map.new_locator = state->rs_new_rel->rd_locator;
	map.new_tid = new_tid;

	/* ---
	 * 现在为受到影响的各个 xid 持久化映射。若 xmin 与 xmax
	 * 不是同一事务，两者都要记录，
	 * 因为映射文件是按"受影响"的 xid 分开的。
	 * 我们并不费太多力气去检测 xmin 与 xmax
	 * 是否其实是同一事务，只检查 xid 是否相同，
	 * 不考虑子事务。多记一些相对
	 * 无害，而且我们永远无法把检查做完整，因为子事务
	 * 数据在重启时会被丢掉。
	 * ---
	 */
	if (do_log_xmin)
		logical_rewrite_log_mapping(state, xmin, &map);
	/* 除非会冗余，否则单独为 xmax 记录映射 */
	if (do_log_xmax && !TransactionIdEquals(xmin, xmax))
		logical_rewrite_log_mapping(state, xmax, &map);
}

/*
 * 重放 XLOG_HEAP2_REWRITE 记录
 */
void
heap_xlog_logical_rewrite(XLogReaderState *r)
{
	char		path[MAXPGPATH];
	int			fd;
	xl_heap_rewrite_mapping *xlrec;
	uint32		len;
	char	   *data;

	xlrec = (xl_heap_rewrite_mapping *) XLogRecGetData(r);

	snprintf(path, MAXPGPATH,
			 "pg_logical/mappings/" LOGICAL_REWRITE_FORMAT,
			 xlrec->mapped_db, xlrec->mapped_rel,
			 LSN_FORMAT_ARGS(xlrec->start_lsn),
			 xlrec->mapped_xid, XLogRecGetXid(r));

	fd = OpenTransientFile(path,
						   O_CREAT | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	/*
	 * 截断所有不能保证已被安全 fsync 过的数据（由
	 * 先前的记录或上一个检查点保证）。
	 */
	pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_TRUNCATE);
	if (ftruncate(fd, xlrec->offset) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\" to %u: %m",
						path, (uint32) xlrec->offset)));
	pgstat_report_wait_end();

	data = XLogRecGetData(r) + sizeof(*xlrec);

	len = xlrec->num_mappings * sizeof(LogicalRewriteMappingData);

	/* （再次）写出映射文件的尾部 */
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_MAPPING_WRITE);
	if (pg_pwrite(fd, data, len, xlrec->offset) != len)
	{
		/* 若写入没有设置 errno，就假定问题是没有磁盘空间 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", path)));
	}
	pgstat_report_wait_end();

	/*
	 * 现在对先前写下的全部数据做 fsync。我们可以改进，只对
	 * 一个文件的最后一次写入这样做，但所需的簿记
	 * 似乎不值得为此费事。
	 */
	pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_MAPPING_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}

/* ---
 * 为逻辑重写映射执行一次检查点
 *
 * 这完成两件事：
 * 1) 按逻辑重启 LSN 移除所有不再需要的映射
 * 2) 把剩余映射全部刷到磁盘，这样检查点之后的重放
 *	  只需要处理在检查点开始之后才写出的那部分映射。
 *	  检查点开始之后的部分。
 * ---
 */
void
CheckPointLogicalRewriteHeap(void)
{
	XLogRecPtr	cutoff;
	XLogRecPtr	redo;
	DIR		   *mappings_dir;
	struct dirent *mapping_de;
	char		path[MAXPGPATH + 20];

	/*
	 * 我们从上次重做指针的最小值开始。不会有新的解码
	 * 复制槽在那之前启动，因此这是移除操作的安全上界。
	 */
	redo = GetRedoRecPtr();

	/* 现在检查现有复制槽的重启指针 */
	cutoff = ReplicationSlotsComputeLogicalRestartLSN();

	/* 不要早于重启 LSN 开始 */
	if (cutoff != InvalidXLogRecPtr && redo < cutoff)
		cutoff = redo;

	mappings_dir = AllocateDir("pg_logical/mappings");
	while ((mapping_de = ReadDir(mappings_dir, "pg_logical/mappings")) != NULL)
	{
		Oid			dboid;
		Oid			relid;
		XLogRecPtr	lsn;
		TransactionId rewrite_xid;
		TransactionId create_xid;
		uint32		hi,
					lo;
		PGFileType	de_type;

		if (strcmp(mapping_de->d_name, ".") == 0 ||
			strcmp(mapping_de->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "pg_logical/mappings/%s", mapping_de->d_name);
		de_type = get_dirent_type(path, mapping_de, false, DEBUG1);

		if (de_type != PGFILETYPE_ERROR && de_type != PGFILETYPE_REG)
			continue;

		/* 跳过无法属于我们的文件。 */
		if (strncmp(mapping_de->d_name, "map-", 4) != 0)
			continue;

		if (sscanf(mapping_de->d_name, LOGICAL_REWRITE_FORMAT,
				   &dboid, &relid, &hi, &lo, &rewrite_xid, &create_xid) != 6)
			elog(ERROR, "could not parse filename \"%s\"", mapping_de->d_name);

		lsn = ((uint64) hi) << 32 | lo;

		if (lsn < cutoff || cutoff == InvalidXLogRecPtr)
		{
			elog(DEBUG1, "removing logical rewrite file \"%s\"", path);
			if (unlink(path) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}
		else
		{
			/* 在某些操作系统上，对文件做 fsync 需要 O_RDWR */
			int			fd = OpenTransientFile(path, O_RDWR | PG_BINARY);

			/*
			 * 该文件无法因并发而消失，因为本函数
			 * 是唯一移除逻辑映射的函数，并且同一时刻只能有一个
			 * 检查点在进行。
			 */
			if (fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", path)));

			/*
			 * 我们可以设法避免对那些要么没有
			 * 改变、要么只是在检查点开始之后才创建的文件做 fsync，
			 * 但目前认为不值得花这个力气。
			 */
			pgstat_report_wait_start(WAIT_EVENT_LOGICAL_REWRITE_CHECKPOINT_SYNC);
			if (pg_fsync(fd) != 0)
				ereport(data_sync_elevel(ERROR),
						(errcode_for_file_access(),
						 errmsg("could not fsync file \"%s\": %m", path)));
			pgstat_report_wait_end();

			if (CloseTransientFile(fd) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not close file \"%s\": %m", path)));
		}
	}
	FreeDir(mappings_dir);

	/* 把目录项持久化到磁盘 */
	fsync_fname("pg_logical/mappings", true);
}
