/*-------------------------------------------------------------------------
 *
 * ginbulk.c
 *	  快速构建倒排索引的例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginbulk.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/gin_private.h"
#include "utils/datum.h"
#include "utils/memutils.h"


#define DEF_NENTRY	2048		/* GinEntryAccumulator 的分配单位 */
#define DEF_NPTR	5			/* ItemPointer 的初始分配单位 */


/* rbtree.c 的合并函数 */
static void
ginCombineData(RBTNode *existing, const RBTNode *newdata, void *arg)
{
	GinEntryAccumulator *eo = (GinEntryAccumulator *) existing;
	const GinEntryAccumulator *en = (const GinEntryAccumulator *) newdata;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	/*
	 * 注意：这段代码假定 newdata 只包含一个项指针。
	 */
	if (eo->count >= eo->maxcount)
	{
		if (eo->maxcount > INT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("posting list is too long"),
					 errhint("Reduce maintenance_work_mem.")));

		accum->allocatedMemory -= GetMemoryChunkSpace(eo->list);
		eo->maxcount *= 2;
		eo->list = (ItemPointerData *)
			repalloc_huge(eo->list, sizeof(ItemPointerData) * eo->maxcount);
		accum->allocatedMemory += GetMemoryChunkSpace(eo->list);
	}

	/* 若项指针未按序排列，稍后就需要排序 */
	if (eo->shouldSort == false)
	{
		int			res;

		res = ginCompareItemPointers(eo->list + eo->count - 1, en->list);
		Assert(res != 0);

		if (res > 0)
			eo->shouldSort = true;
	}

	eo->list[eo->count] = en->list[0];
	eo->count++;
}

/* rbtree.c 的比较函数 */
static int
cmpEntryAccumulator(const RBTNode *a, const RBTNode *b, void *arg)
{
	const GinEntryAccumulator *ea = (const GinEntryAccumulator *) a;
	const GinEntryAccumulator *eb = (const GinEntryAccumulator *) b;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	return ginCompareAttEntries(accum->ginstate,
								ea->attnum, ea->key, ea->category,
								eb->attnum, eb->key, eb->category);
}

/* rbtree.c 的分配函数 */
static RBTNode *
ginAllocEntryAccumulator(void *arg)
{
	BuildAccumulator *accum = (BuildAccumulator *) arg;
	GinEntryAccumulator *ea;

	/*
	 * 按相当大的块分配内存以降低开销。我们无需
	 * 逐个回收 RBTNodes，因此这样做没有额外代价。
	 */
	if (accum->entryallocator == NULL || accum->eas_used >= DEF_NENTRY)
	{
		accum->entryallocator = palloc(sizeof(GinEntryAccumulator) * DEF_NENTRY);
		accum->allocatedMemory += GetMemoryChunkSpace(accum->entryallocator);
		accum->eas_used = 0;
	}

	/* 从当前块中分配新的 RBTNode */
	ea = accum->entryallocator + accum->eas_used;
	accum->eas_used++;

	return (RBTNode *) ea;
}

void
ginInitBA(BuildAccumulator *accum)
{
	/* 这里有意不设置 accum->ginstate */
	accum->allocatedMemory = 0;
	accum->entryallocator = NULL;
	accum->eas_used = 0;
	accum->tree = rbt_create(sizeof(GinEntryAccumulator),
							 cmpEntryAccumulator,
							 ginCombineData,
							 ginAllocEntryAccumulator,
							 NULL,	/* 不需要 freefunc */
							 (void *) accum);
}

/*
 * 基本等同于 datumCopy()，但加以扩展，以便把
 * palloc 分配的空间计入 accum->allocatedMemory。
 */
static Datum
getDatumCopy(BuildAccumulator *accum, OffsetNumber attnum, Datum value)
{
	Form_pg_attribute att;
	Datum		res;

	att = TupleDescAttr(accum->ginstate->origTupdesc, attnum - 1);
	if (att->attbyval)
		res = value;
	else
	{
		res = datumCopy(value, false, att->attlen);
		accum->allocatedMemory += GetMemoryChunkSpace(DatumGetPointer(res));
	}
	return res;
}

/*
 * 从被索引值中查找或存入一个条目。
 */
static void
ginInsertBAEntry(BuildAccumulator *accum,
				 ItemPointer heapptr, OffsetNumber attnum,
				 Datum key, GinNullCategory category)
{
	GinEntryAccumulator eatmp;
	GinEntryAccumulator *ea;
	bool		isNew;

	/*
	 * 目前只填写会被 cmpEntryAccumulator 或
	 * ginCombineData 查看的那些 eatmp 字段。
	 */
	eatmp.attnum = attnum;
	eatmp.key = key;
	eatmp.category = category;
	/* 临时建立只含单个条目的项指针列表 */
	eatmp.list = heapptr;

	ea = (GinEntryAccumulator *) rbt_insert(accum->tree, (RBTNode *) &eatmp,
											&isNew);

	if (isNew)
	{
		/*
		 * 完成新树条目的初始化，包括为 datum（若非空）
		 * 和项指针制作永久副本。
		 */
		if (category == GIN_CAT_NORM_KEY)
			ea->key = getDatumCopy(accum, attnum, key);
		ea->maxcount = DEF_NPTR;
		ea->count = 1;
		ea->shouldSort = false;
		ea->list =
			(ItemPointerData *) palloc(sizeof(ItemPointerData) * DEF_NPTR);
		ea->list[0] = *heapptr;
		accum->allocatedMemory += GetMemoryChunkSpace(ea->list);
	}
	else
	{
		/*
		 * ginCombineData 已经完成了所需的全部工作。
		 */
	}
}

/*
 * 插入一个堆指针对应的全部条目。
 *
 * 这些条目被插入一棵平衡二叉树。你可能会认为
 * 插入顺序并不关键，但按已排序顺序插入实际上
 * 会造成大量再平衡操作，因而很慢。为避免这一点，
 * 我们尝试按某种顺序插入结点，使得若输入确实
 * 已排序，得到的树就会
 * 近乎平衡。
 *
 * 做法如下。首先，设想有一个数组，其大小是大于等于
 * 实际数组大小的最小 2 的幂。其次，把这个虚拟数组的
 * 中间条目插入树中；然后插入虚拟数组每一半的中间
 * 条目，再插入各四分之一的中间条目，
 * 依此类推。
 */
void
ginInsertBAEntries(BuildAccumulator *accum,
				   ItemPointer heapptr, OffsetNumber attnum,
				   Datum *entries, GinNullCategory *categories,
				   int32 nentries)
{
	uint32		step = nentries;

	if (nentries <= 0)
		return;

	Assert(ItemPointerIsValid(heapptr) && attnum >= FirstOffsetNumber);

	/*
	 * step 将包含不大于 nentries 的最大 2 的幂
	 */
	step |= (step >> 1);
	step |= (step >> 2);
	step |= (step >> 4);
	step |= (step >> 8);
	step |= (step >> 16);
	step >>= 1;
	step++;

	while (step > 0)
	{
		int			i;

		for (i = step - 1; i < nentries && i >= 0; i += step << 1 /* 乘以 2 */ )
			ginInsertBAEntry(accum, heapptr, attnum,
							 entries[i], categories[i]);

		step >>= 1;				/* 除以 2 */
	}
}

static int
qsortCompareItemPointers(const void *a, const void *b)
{
	int			res = ginCompareItemPointers((ItemPointer) a, (ItemPointer) b);

	/* 断言正在排序的项指针中没有相等项 */
	Assert(res != 0);
	return res;
}

/* 准备用 ginGetBAEntry 读出 rbtree 的内容 */
void
ginBeginBAScan(BuildAccumulator *accum)
{
	rbt_begin_iterate(accum->tree, LeftRightWalk, &accum->tree_walk);
}

/*
 * 按顺序从 BuildAccumulator 的 rbtree 中取出下一个条目。
 * 它由单个键 datum，以及该键出现于其中的一个或多个
 * 堆 TIDs 所组成的列表（数组）构成。该列表保证已排序。
 */
ItemPointerData *
ginGetBAEntry(BuildAccumulator *accum,
			  OffsetNumber *attnum, Datum *key, GinNullCategory *category,
			  uint32 *n)
{
	GinEntryAccumulator *entry;
	ItemPointerData *list;

	entry = (GinEntryAccumulator *) rbt_iterate(&accum->tree_walk);

	if (entry == NULL)
		return NULL;			/* 没有更多条目 */

	*attnum = entry->attnum;
	*key = entry->key;
	*category = entry->category;
	list = entry->list;
	*n = entry->count;

	Assert(list != NULL && entry->count > 0);

	if (entry->shouldSort && entry->count > 1)
		qsort(list, entry->count, sizeof(ItemPointerData),
			  qsortCompareItemPointers);

	return list;
}
