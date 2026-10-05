/*-------------------------------------------------------------------------
 *
 * tidbitmap.c
 *	  PostgreSQL 元组标识符（TID）位图包
 *
 * 本模块提供的位图数据结构在精神上
 * 与 Bitmapsets 相似，但专门用来存放
 * 元组标识符（TIDs），也就是 ItemPointers 的集合。尤其照顾了
 * 把 ItemPointer 分成 BlockNumber 与 OffsetNumber 这一点。
 * 另外，由于希望用这种数据结构在
 * 内存中存放非常大的元组集合，我们支持 "有损" 存储：此时
 * 不再记住一页上各个元组的偏移，而只记住
 * 某一页需要被访问这一事实。
 *
 * "有损" 存储对每个磁盘页使用一位，因此按标准 8K
 * BLCKSZ，大约 1Mb 内存就能表示 64Gb 磁盘空间中的
 * 全部页。推动这种规模的表的人应当有
 * 几 Mb 富余，因此我们不操心再提供第二级
 * 有损。理论上某个时候可以退回到页范围，
 * 但目前那看起来是无用的复杂性。
 *
 * 我们也支持候选匹配，或者说复查。这
 * 意味着我们知道搜索只需访问一页上的部分元组，
 * 但不能确定这些元组全都是真正的匹配。
 * 因此最终的堆扫描必须只对这些元组复查限定条件，
 * 而不是像有损位图那样对页上全部元组复查限定条件。
 * 把 TID 插入位图时可以指定复查，
 * 并且在把有损页与非有损页做 AND 时，
 * 内部也可能产生复查。
 *
 *
 * Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/nodes/tidbitmap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "common/hashfn.h"
#include "nodes/bitmapset.h"
#include "nodes/tidbitmap.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"

/*
 * 每页的最大元组数并不大（8K 页通常是 256，
 * 32K 页是 1024）。因此没必要把
 * 每页位图做成可变大小。我们直接规定大小
 * 就是这样：
 */
#define MAX_TUPLES_PER_PAGE  MaxHeapTuplesPerPage

/*
 * 当我们不得不改用有损存储时，使用每个页一位的数据结构，
 * 其中页号 DIV
 * PAGES_PER_CHUNK 相同的所有页被聚成一个组块。当某个组块存在
 * 且对给定页置了位时，页表中不得再有该页的
 * 按页条目。
 *
 * 实际上我们把精确页和有损组块存在同一个哈希
 * 表里，使用相同的数据结构。（这是因为哈希表的内存
 * 管理不容易、也不高效地把空间
 * 方便地从一个哈希表转到另一个。）因此最好让
 * PAGES_PER_CHUNK 与 MAX_TUPLES_PER_PAGE 相同，或至少不要
 * 差太多。但我们也希望 PAGES_PER_CHUNK 是 2 的幂，以
 * 避免昂贵的整数取余。因此这样定义：
 */
#define PAGES_PER_CHUNK  (BLCKSZ / 32)

/* 我们使用 nodes/bitmapset.h 中的 BITS_PER_BITMAPWORD 和 typedef bitmapword */

#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

/* 精确页的活动字数： */
#define WORDS_PER_PAGE	((MAX_TUPLES_PER_PAGE - 1) / BITS_PER_BITMAPWORD + 1)
/* 有损组块的活动字数： */
#define WORDS_PER_CHUNK  ((PAGES_PER_CHUNK - 1) / BITS_PER_BITMAPWORD + 1)

/*
 * 哈希表项用这个数据结构表示。对于
 * 精确页，blockno 是页号，位图的第 k 位
 * 表示元组偏移 k+1。对于有损组块，blockno 是组块中的第一
 * 页（它必须是 PAGES_PER_CHUNK 的倍数），
 * 第 k 位表示页 blockno+k。注意，若组块范围内
 * 任一页使用有损存储，就无法对该组块的第一页
 * 做精确存储，因为同一个
 * 哈希表项必须同时承担两种用途。
 *
 * recheck 只用于精确页——它表示虽然
 * 只需要检查所说的那些元组，但对每个元组仍必须检查完整的索引限定条件
 * （即这些是候选匹配）。
 */
typedef struct PagetableEntry
{
	BlockNumber blockno;		/* 页号（哈希表键） */
	char		status;			/* 哈希项状态 */
	bool		ischunk;		/* T = 有损存储，F = 精确 */
	bool		recheck;		/* 这些元组是否应当重新检查？ */
	bitmapword	words[Max(WORDS_PER_PAGE, WORDS_PER_CHUNK)];
} PagetableEntry;

/*
 * 存放页表项数组。
 */
typedef struct PTEntryArray
{
	pg_atomic_uint32 refcount;	/* 已挂接的迭代器数 */
	PagetableEntry ptentry[FLEXIBLE_ARRAY_MEMBER];
} PTEntryArray;

/*
 * 我们希望在不必要时避免创建哈希表的开销，它
 * 相对较大。尤其是在嵌套循环连接内侧使用位图
 * 扫描时：这种情况下位图很可能只
 * 活到积累一个条目。因此我们等到需要两个页表条目时
 * 才创建真正的哈希表。当
 * 只需要一个页表条目时，把它存在
 * TIDBitMap 的固定字段里。（注意：若位图后来
 * 又缩回零页或一页，我们并不丢掉哈希表。
 * 因此即使 nentries 为零或一，状态也可以是 TBM_HASH。）
 */
typedef enum
{
	TBM_EMPTY,					/* 没有哈希表，nentries == 0 */
	TBM_ONE_PAGE,				/* entry1 存放那唯一的条目 */
	TBM_HASH					/* pagetable 有效，entry1 无效 */
} TBMStatus;

/*
 * TBM 当前的迭代状态。
 */
typedef enum
{
	TBM_NOT_ITERATING,			/* 尚未转换成页数组和组块数组 */
	TBM_ITERATING_PRIVATE,		/* 已转换成本地页数组和组块数组 */
	TBM_ITERATING_SHARED		/* 已转换成共享页数组和组块数组 */
} TBMIteratingState;

/*
 * 整个 TIDBitMap 的表示如下：
 */
struct TIDBitmap
{
	NodeTag		type;			/* 使它成为有效的 Node */
	MemoryContext mcxt;			/* 包含我的内存上下文 */
	TBMStatus	status;			/* 见上面的代码 */
	struct pagetable_hash *pagetable;	/* PagetableEntry 的哈希表 */
	int			nentries;		/* 页表中的条目数 */
	int			maxentries;		/* 为满足 maxbytes 而对同样项设的上限 */
	int			npages;			/* 页表中的精确条目数 */
	int			nchunks;		/* 页表中的有损条目数 */
	TBMIteratingState iterating;	/* 调用过 tbm_begin_iterate 吗？ */
	uint32		lossify_start;	/* 开始把哈希表变成有损的偏移 */
	PagetableEntry entry1;		/* 当 status == TBM_ONE_PAGE 时使用 */
	/* 当 iterating 为 true 时这些有效： */
	PagetableEntry **spages;	/* 排好序的精确页列表，或 NULL */
	PagetableEntry **schunks;	/* 排好序的有损组块列表，或 NULL */
	dsa_pointer dsapagetable;	/* 指向元素数组的 dsa_pointer */
	dsa_pointer dsapagetableold;	/* 指向旧元素数组的 dsa_pointer */
	dsa_pointer ptpages;		/* 指向页数组的 dsa_pointer */
	dsa_pointer ptchunks;		/* 指向组块数组的 dsa_pointer */
	dsa_area   *dsa;			/* 对每个查询的 dsa 区域的引用 */
};

/*
 * 按排序顺序遍历位图时，用 TBMIterator 来
 * 跟踪进度。可以有多个迭代器同时扫描同一
 * 位图。注意，一旦创建了任何迭代器，
 * 位图就变为只读。
 */
struct TBMIterator
{
	TIDBitmap  *tbm;			/* 我们正在遍历的 TIDBitmap */
	int			spageptr;		/* 下一个 spages 下标 */
	int			schunkptr;		/* 下一个 schunks 下标 */
	int			schunkbit;		/* 当前 schunk 中下一个要检查的位 */
	TBMIterateResult output;	/* 必须放在最后（因为大小可变） */
};

/*
 * 存放迭代器的共享成员，以便多个进程
 * 可以联合迭代。
 */
typedef struct TBMSharedIteratorState
{
	int			nentries;		/* 页表中的条目数 */
	int			maxentries;		/* 为满足 maxbytes 而对同样项设的上限 */
	int			npages;			/* 页表中的精确条目数 */
	int			nchunks;		/* 页表中的有损条目数 */
	dsa_pointer pagetable;		/* 指向页表数据头的 dsa 指针 */
	dsa_pointer spages;			/* 指向页数组的 dsa 指针 */
	dsa_pointer schunks;		/* 指向组块数组的 dsa 指针 */
	LWLock		lock;			/* 保护下面这些成员的锁 */
	int			spageptr;		/* 下一个 spages 下标 */
	int			schunkptr;		/* 下一个 schunks 下标 */
	int			schunkbit;		/* 当前 schunk 中下一个要检查的位 */
} TBMSharedIteratorState;

/*
 * 页表迭代数组。
 */
typedef struct PTIterationArray
{
	pg_atomic_uint32 refcount;	/* 已挂接的迭代器数 */
	int			index[FLEXIBLE_ARRAY_MEMBER];	/* 下标数组 */
} PTIterationArray;

/*
 * 与 TBMIterator 相同，但用于联合迭代，因此它
 * 还持有对共享状态的引用。
 */
struct TBMSharedIterator
{
	TBMSharedIteratorState *state;	/* 共享状态 */
	PTEntryArray *ptbase;		/* 页表元素数组 */
	PTIterationArray *ptpages;	/* 排好序的精确页下标列表 */
	PTIterationArray *ptchunks; /* 排好序的有损页下标列表 */
	TBMIterateResult output;	/* 必须放在最后（因为大小可变） */
};

/* 本地函数原型 */
static void tbm_union_page(TIDBitmap *a, const PagetableEntry *bpage);
static bool tbm_intersect_page(TIDBitmap *a, PagetableEntry *apage,
							   const TIDBitmap *b);
static const PagetableEntry *tbm_find_pageentry(const TIDBitmap *tbm,
												BlockNumber pageno);
static PagetableEntry *tbm_get_pageentry(TIDBitmap *tbm, BlockNumber pageno);
static bool tbm_page_is_lossy(const TIDBitmap *tbm, BlockNumber pageno);
static void tbm_mark_page_lossy(TIDBitmap *tbm, BlockNumber pageno);
static void tbm_lossify(TIDBitmap *tbm);
static int	tbm_comparator(const void *left, const void *right);
static int	tbm_shared_comparator(const void *left, const void *right,
								  void *arg);

/* 定义把块号映射到 PagetableEntry 的哈希表 */
#define SH_USE_NONDEFAULT_ALLOCATOR
#define SH_PREFIX pagetable
#define SH_ELEMENT_TYPE PagetableEntry
#define SH_KEY_TYPE BlockNumber
#define SH_KEY blockno
#define SH_HASH_KEY(tb, key) murmurhash32(key)
#define SH_EQUAL(tb, a, b) a == b
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"


/*
 * tbm_create - 创建一个最初为空的位图
 *
 * 该位图将活在本次调用时 CurrentMemoryContext
 * 所在的内存上下文中。它的总内存消耗将限制在（大约）maxbytes。
 * 若传给本函数的 DSA 不是 NULL，
 * 则底层页表元素的存储内存将
 * 从该 DSA 分配。
 */
TIDBitmap *
tbm_create(long maxbytes, dsa_area *dsa)
{
	TIDBitmap  *tbm;

	/* 创建 TIDBitmap 结构体并把所有字段清零 */
	tbm = makeNode(TIDBitmap);

	tbm->mcxt = CurrentMemoryContext;
	tbm->status = TBM_EMPTY;

	tbm->maxentries = (int) tbm_calculate_entries(maxbytes);
	tbm->lossify_start = 0;
	tbm->dsa = dsa;
	tbm->dsapagetable = InvalidDsaPointer;
	tbm->dsapagetableold = InvalidDsaPointer;
	tbm->ptpages = InvalidDsaPointer;
	tbm->ptchunks = InvalidDsaPointer;

	return tbm;
}

/*
 * 真正创建哈希表。因为这相当
 * 昂贵，我们等到必须时才做。
 */
static void
tbm_create_pagetable(TIDBitmap *tbm)
{
	Assert(tbm->status != TBM_HASH);
	Assert(tbm->pagetable == NULL);

	tbm->pagetable = pagetable_create(tbm->mcxt, 128, tbm);

	/* 若 entry1 有效，把它推进哈希表 */
	if (tbm->status == TBM_ONE_PAGE)
	{
		PagetableEntry *page;
		bool		found;
		char		oldstatus;

		page = pagetable_insert(tbm->pagetable,
								tbm->entry1.blockno,
								&found);
		Assert(!found);
		oldstatus = page->status;
		memcpy(page, &tbm->entry1, sizeof(PagetableEntry));
		page->status = oldstatus;
	}

	tbm->status = TBM_HASH;
}

/*
 * tbm_free - 释放一个 TIDBitmap
 */
void
tbm_free(TIDBitmap *tbm)
{
	if (tbm->pagetable)
		pagetable_destroy(tbm->pagetable);
	if (tbm->spages)
		pfree(tbm->spages);
	if (tbm->schunks)
		pfree(tbm->schunks);
	pfree(tbm);
}

/*
 * tbm_free_shared_area - 释放共享状态
 *
 * 释放共享迭代器状态；若没有任何共享迭代器引用，
 * 也释放共享页表和迭代器数组的内存，即 refcount
 * 变为 0。
 */
void
tbm_free_shared_area(dsa_area *dsa, dsa_pointer dp)
{
	TBMSharedIteratorState *istate = dsa_get_address(dsa, dp);
	PTEntryArray *ptbase;
	PTIterationArray *ptpages;
	PTIterationArray *ptchunks;

	if (DsaPointerIsValid(istate->pagetable))
	{
		ptbase = dsa_get_address(dsa, istate->pagetable);
		if (pg_atomic_sub_fetch_u32(&ptbase->refcount, 1) == 0)
			dsa_free(dsa, istate->pagetable);
	}
	if (DsaPointerIsValid(istate->spages))
	{
		ptpages = dsa_get_address(dsa, istate->spages);
		if (pg_atomic_sub_fetch_u32(&ptpages->refcount, 1) == 0)
			dsa_free(dsa, istate->spages);
	}
	if (DsaPointerIsValid(istate->schunks))
	{
		ptchunks = dsa_get_address(dsa, istate->schunks);
		if (pg_atomic_sub_fetch_u32(&ptchunks->refcount, 1) == 0)
			dsa_free(dsa, istate->schunks);
	}

	dsa_free(dsa, dp);
}

/*
 * tbm_add_tuples - 向 TIDBitmap 加入一些元组 ID
 *
 * 若 recheck 为 true，则当这些元组中的任何一个被报告出来时，
 * 会在 TBMIterateResult 中设置重新检查标志。
 */
void
tbm_add_tuples(TIDBitmap *tbm, const ItemPointer tids, int ntids,
			   bool recheck)
{
	BlockNumber currblk = InvalidBlockNumber;
	PagetableEntry *page = NULL;	/* 仅当 currblk 有效时才有效 */
	int			i;

	Assert(tbm->iterating == TBM_NOT_ITERATING);
	for (i = 0; i < ntids; i++)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(tids + i);
		OffsetNumber off = ItemPointerGetOffsetNumber(tids + i);
		int			wordnum,
					bitnum;

		/* 安全检查，确保不会越出位数组边界 */
		if (off < 1 || off > MAX_TUPLES_PER_PAGE)
			elog(ERROR, "tuple offset out of range: %u", off);

		/*
		 * 除非已经查过，否则查找目标页。当输入包含
		 * 同一页上连续的元组时，这能省下一些周期，而这种情况
		 * 常见到值得在这里多做一次测试。
		 */
		if (blk != currblk)
		{
			if (tbm_page_is_lossy(tbm, blk))
				page = NULL;	/* 记住该页是有损的 */
			else
				page = tbm_get_pageentry(tbm, blk);
			currblk = blk;
		}

		if (page == NULL)
			continue;			/* 整页已经被标过 */

		if (page->ischunk)
		{
			/* 该页是有损组块头，为自己置位 */
			wordnum = bitnum = 0;
		}
		else
		{
			/* 该页是精确的，因此为单个元组置位 */
			wordnum = WORDNUM(off - 1);
			bitnum = BITNUM(off - 1);
		}
		page->words[wordnum] |= ((bitmapword) 1 << bitnum);
		page->recheck |= recheck;

		if (tbm->nentries > tbm->maxentries)
		{
			tbm_lossify(tbm);
			/* 该页可能已被转换成有损，因此强制重新查找 */
			currblk = InvalidBlockNumber;
		}
	}
}

/*
 * tbm_add_page - 向 TIDBitmap 加入整页
 *
 * 这会使扫描 TIDBitmap 时报告整页
 * （并带上重新检查标志）。
 */
void
tbm_add_page(TIDBitmap *tbm, BlockNumber pageno)
{
	/* 把该页记入位图，若已存在则标为有损 */
	tbm_mark_page_lossy(tbm, pageno);
	/* 若超过了内存限制，再把更多页变成有损 */
	if (tbm->nentries > tbm->maxentries)
		tbm_lossify(tbm);
}

/*
 * tbm_union - 集合并
 *
 * a 被就地修改，b 不变
 */
void
tbm_union(TIDBitmap *a, const TIDBitmap *b)
{
	Assert(!a->iterating);
	/* 若 b 为空则无事可做 */
	if (b->nentries == 0)
		return;
	/* 扫描 b 中的组块和页，并入 a */
	if (b->status == TBM_ONE_PAGE)
		tbm_union_page(a, &b->entry1);
	else
	{
		pagetable_iterator i;
		PagetableEntry *bpage;

		Assert(b->status == TBM_HASH);
		pagetable_start_iterate(b->pagetable, &i);
		while ((bpage = pagetable_iterate(b->pagetable, &i)) != NULL)
			tbm_union_page(a, bpage);
	}
}

/* 在并操作期间处理 b 的一页 */
static void
tbm_union_page(TIDBitmap *a, const PagetableEntry *bpage)
{
	PagetableEntry *apage;
	int			wordnum;

	if (bpage->ischunk)
	{
		/* 扫描 b 的组块，把每个所指的页在 a 中标为有损 */
		for (wordnum = 0; wordnum < WORDS_PER_CHUNK; wordnum++)
		{
			bitmapword	w = bpage->words[wordnum];

			if (w != 0)
			{
				BlockNumber pg;

				pg = bpage->blockno + (wordnum * BITS_PER_BITMAPWORD);
				while (w != 0)
				{
					if (w & 1)
						tbm_mark_page_lossy(a, pg);
					pg++;
					w >>= 1;
				}
			}
		}
	}
	else if (tbm_page_is_lossy(a, bpage->blockno))
	{
		/* 该页在 a 中已经是有损的，无事可做 */
		return;
	}
	else
	{
		apage = tbm_get_pageentry(a, bpage->blockno);
		if (apage->ischunk)
		{
			/* 该页是有损组块头，为自己置位 */
			apage->words[0] |= ((bitmapword) 1 << 0);
		}
		else
		{
			/* 两页都是精确的，在位一级合并 */
			for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
				apage->words[wordnum] |= bpage->words[wordnum];
			apage->recheck |= bpage->recheck;
		}
	}

	if (a->nentries > a->maxentries)
		tbm_lossify(a);
}

/*
 * tbm_intersect - 集合交
 *
 * a 被就地修改，b 不变
 */
void
tbm_intersect(TIDBitmap *a, const TIDBitmap *b)
{
	Assert(!a->iterating);
	/* 若 a 为空则无事可做 */
	if (a->nentries == 0)
		return;
	/* 扫描 a 中的组块和页，尝试与 b 匹配 */
	if (a->status == TBM_ONE_PAGE)
	{
		if (tbm_intersect_page(a, &a->entry1, b))
		{
			/* 该页现在为空，从 a 中去掉 */
			Assert(!a->entry1.ischunk);
			a->npages--;
			a->nentries--;
			Assert(a->nentries == 0);
			a->status = TBM_EMPTY;
		}
	}
	else
	{
		pagetable_iterator i;
		PagetableEntry *apage;

		Assert(a->status == TBM_HASH);
		pagetable_start_iterate(a->pagetable, &i);
		while ((apage = pagetable_iterate(a->pagetable, &i)) != NULL)
		{
			if (tbm_intersect_page(a, apage, b))
			{
				/* 页或组块现在为空，从 a 中去掉 */
				if (apage->ischunk)
					a->nchunks--;
				else
					a->npages--;
				a->nentries--;
				if (!pagetable_delete(a->pagetable, apage->blockno))
					elog(ERROR, "hash table corrupted");
			}
		}
	}
}

/*
 * 在交操作期间处理 a 的一页
 *
 * 若 apage 现在为空、应从 a 中删除，则返回 true
 */
static bool
tbm_intersect_page(TIDBitmap *a, PagetableEntry *apage, const TIDBitmap *b)
{
	const PagetableEntry *bpage;
	int			wordnum;

	if (apage->ischunk)
	{
		/* 扫描组块中的每一位，尝试清除 */
		bool		candelete = true;

		for (wordnum = 0; wordnum < WORDS_PER_CHUNK; wordnum++)
		{
			bitmapword	w = apage->words[wordnum];

			if (w != 0)
			{
				bitmapword	neww = w;
				BlockNumber pg;
				int			bitnum;

				pg = apage->blockno + (wordnum * BITS_PER_BITMAPWORD);
				bitnum = 0;
				while (w != 0)
				{
					if (w & 1)
					{
						if (!tbm_page_is_lossy(b, pg) &&
							tbm_find_pageentry(b, pg) == NULL)
						{
							/* 该页根本不在 b 中，丢掉有损位 */
							neww &= ~((bitmapword) 1 << bitnum);
						}
					}
					pg++;
					bitnum++;
					w >>= 1;
				}
				apage->words[wordnum] = neww;
				if (neww != 0)
					candelete = false;
			}
		}
		return candelete;
	}
	else if (tbm_page_is_lossy(b, apage->blockno))
	{
		/*
		 * 'a' 中的某些元组也许不满足 'b' 的限定条件，但
		 * 因为页 'b' 是有损的，我们不知道是哪些。因此
		 * 我们把 'a' 标为需要重新检查，表示最多只有
		 * 'a' 中置位的那些元组是匹配。
		 */
		apage->recheck = true;
		return false;
	}
	else
	{
		bool		candelete = true;

		bpage = tbm_find_pageentry(b, apage->blockno);
		if (bpage != NULL)
		{
			/* 两页都是精确的，在位一级合并 */
			Assert(!bpage->ischunk);
			for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
			{
				apage->words[wordnum] &= bpage->words[wordnum];
				if (apage->words[wordnum] != 0)
					candelete = false;
			}
			apage->recheck |= bpage->recheck;
		}
		/* 若没有匹配的 b 页，可以直接删除 a 页 */
		return candelete;
	}
}

/*
 * tbm_is_empty - TIDBitmap 是否完全为空？
 */
bool
tbm_is_empty(const TIDBitmap *tbm)
{
	return (tbm->nentries == 0);
}

/*
 * tbm_begin_iterate - 准备遍历一个 TIDBitmap
 *
 * TBMIterator 结构体在调用方的内存上下文中创建。
 * 要干净地结束迭代，请调用 tbm_end_iterate；不过
 * 只让内存上下文被释放也可以。调用方有责任
 * 在 TIDBitmap
 * 被释放之后，不再触碰该 TBMIterator。
 *
 * 注意：在此之后，不再允许修改位图的
 * 内容。不过你可以多次调用本函数来反复
 * 扫描这些内容，包括并行扫描。
 */
TBMIterator *
tbm_begin_iterate(TIDBitmap *tbm)
{
	TBMIterator *iterator;

	Assert(tbm->iterating != TBM_ITERATING_SHARED);

	/*
	 * 创建 TBMIterator 结构体，并留出足够的尾部空间，以满足
	 * TBMIterateResult 子结构体的需要。
	 */
	iterator = (TBMIterator *) palloc(sizeof(TBMIterator) +
									  MAX_TUPLES_PER_PAGE * sizeof(OffsetNumber));
	iterator->tbm = tbm;

	/*
	 * 初始化迭代指针。
	 */
	iterator->spageptr = 0;
	iterator->schunkptr = 0;
	iterator->schunkbit = 0;

	/*
	 * 若我们有哈希表，就创建并填好排好序的页列表，除非
	 * 先前的迭代器已经做过。注意这些列表
	 * 挂在位图上而不是迭代器上，因此可以被多个
	 * 迭代器使用。
	 */
	if (tbm->status == TBM_HASH && tbm->iterating == TBM_NOT_ITERATING)
	{
		pagetable_iterator i;
		PagetableEntry *page;
		int			npages;
		int			nchunks;

		if (!tbm->spages && tbm->npages > 0)
			tbm->spages = (PagetableEntry **)
				MemoryContextAlloc(tbm->mcxt,
								   tbm->npages * sizeof(PagetableEntry *));
		if (!tbm->schunks && tbm->nchunks > 0)
			tbm->schunks = (PagetableEntry **)
				MemoryContextAlloc(tbm->mcxt,
								   tbm->nchunks * sizeof(PagetableEntry *));

		npages = nchunks = 0;
		pagetable_start_iterate(tbm->pagetable, &i);
		while ((page = pagetable_iterate(tbm->pagetable, &i)) != NULL)
		{
			if (page->ischunk)
				tbm->schunks[nchunks++] = page;
			else
				tbm->spages[npages++] = page;
		}
		Assert(npages == tbm->npages);
		Assert(nchunks == tbm->nchunks);
		if (npages > 1)
			qsort(tbm->spages, npages, sizeof(PagetableEntry *),
				  tbm_comparator);
		if (nchunks > 1)
			qsort(tbm->schunks, nchunks, sizeof(PagetableEntry *),
				  tbm_comparator);
	}

	tbm->iterating = TBM_ITERATING_PRIVATE;

	return iterator;
}

/*
 * tbm_prepare_shared_iterate - 为 TIDBitmap 准备共享迭代状态。
 *
 * 所需的共享状态将从传给
 * tbm_create 的 DSA 中分配，以便多个进程可以挂接并联合迭代。
 *
 * 这会把页表哈希转换成页数组和组块数组，其元素是
 * 页表数组中的下标。
 */
dsa_pointer
tbm_prepare_shared_iterate(TIDBitmap *tbm)
{
	dsa_pointer dp;
	TBMSharedIteratorState *istate;
	PTEntryArray *ptbase = NULL;
	PTIterationArray *ptpages = NULL;
	PTIterationArray *ptchunks = NULL;

	Assert(tbm->dsa != NULL);
	Assert(tbm->iterating != TBM_ITERATING_PRIVATE);

	/*
	 * 从 DSA 分配 TBMSharedIteratorState，以存放共享成员和
	 * 锁，多个工作进程的共享迭代也会用到它。
	 */
	dp = dsa_allocate0(tbm->dsa, sizeof(TBMSharedIteratorState));
	istate = dsa_get_address(tbm->dsa, dp);

	/*
	 * 若我们尚未在迭代，就创建并填好排好序的页列表。
	 * （若已经在迭代，排好序的页列表已经存在 TIDBitmap 中，
	 * 我们可以直接复用。）
	 */
	if (tbm->iterating == TBM_NOT_ITERATING)
	{
		pagetable_iterator i;
		PagetableEntry *page;
		int			idx;
		int			npages;
		int			nchunks;

		/*
		 * 从 DSA 分配页数组和组块数组的内存，以便在
		 * 多个进程之间共享。
		 */
		if (tbm->npages)
		{
			tbm->ptpages = dsa_allocate(tbm->dsa, sizeof(PTIterationArray) +
										tbm->npages * sizeof(int));
			ptpages = dsa_get_address(tbm->dsa, tbm->ptpages);
			pg_atomic_init_u32(&ptpages->refcount, 0);
		}
		if (tbm->nchunks)
		{
			tbm->ptchunks = dsa_allocate(tbm->dsa, sizeof(PTIterationArray) +
										 tbm->nchunks * sizeof(int));
			ptchunks = dsa_get_address(tbm->dsa, tbm->ptchunks);
			pg_atomic_init_u32(&ptchunks->refcount, 0);
		}

		/*
		 * 若 TBM 状态是 TBM_HASH，则遍历页表并
		 * 把它转换成页数组和组块数组。若它处于
		 * TBM_ONE_PAGE 模式，则直接从 DSA
		 * 为一个条目分配空间。
		 */
		npages = nchunks = 0;
		if (tbm->status == TBM_HASH)
		{
			ptbase = dsa_get_address(tbm->dsa, tbm->dsapagetable);

			pagetable_start_iterate(tbm->pagetable, &i);
			while ((page = pagetable_iterate(tbm->pagetable, &i)) != NULL)
			{
				idx = page - ptbase->ptentry;
				if (page->ischunk)
					ptchunks->index[nchunks++] = idx;
				else
					ptpages->index[npages++] = idx;
			}

			Assert(npages == tbm->npages);
			Assert(nchunks == tbm->nchunks);
		}
		else if (tbm->status == TBM_ONE_PAGE)
		{
			/*
			 * 在单页模式下为一个页表项分配空间，
			 * 初始化它，并把它的下标（即 0）直接存进
			 * 页数组。
			 */
			tbm->dsapagetable = dsa_allocate(tbm->dsa, sizeof(PTEntryArray) +
											 sizeof(PagetableEntry));
			ptbase = dsa_get_address(tbm->dsa, tbm->dsapagetable);
			memcpy(ptbase->ptentry, &tbm->entry1, sizeof(PagetableEntry));
			ptpages->index[0] = 0;
		}

		if (ptbase != NULL)
			pg_atomic_init_u32(&ptbase->refcount, 0);
		if (npages > 1)
			qsort_arg(ptpages->index, npages, sizeof(int),
					  tbm_shared_comparator, ptbase->ptentry);
		if (nchunks > 1)
			qsort_arg(ptchunks->index, nchunks, sizeof(int),
					  tbm_shared_comparator, ptbase->ptentry);
	}

	/*
	 * 把 TBM 成员存进共享状态，以便在
	 * 多个进程之间共享。
	 */
	istate->nentries = tbm->nentries;
	istate->maxentries = tbm->maxentries;
	istate->npages = tbm->npages;
	istate->nchunks = tbm->nchunks;
	istate->pagetable = tbm->dsapagetable;
	istate->spages = tbm->ptpages;
	istate->schunks = tbm->ptchunks;

	ptbase = dsa_get_address(tbm->dsa, tbm->dsapagetable);
	ptpages = dsa_get_address(tbm->dsa, tbm->ptpages);
	ptchunks = dsa_get_address(tbm->dsa, tbm->ptchunks);

	/*
	 * 对每个引用页表和迭代器数组的共享迭代器，
	 * 把引用计数加 1，这样在释放共享迭代器时，
	 * 要等到引用计数变为 0 才释放页表和迭代器数组。
	 */
	if (ptbase != NULL)
		pg_atomic_add_fetch_u32(&ptbase->refcount, 1);
	if (ptpages != NULL)
		pg_atomic_add_fetch_u32(&ptpages->refcount, 1);
	if (ptchunks != NULL)
		pg_atomic_add_fetch_u32(&ptchunks->refcount, 1);

	/* 初始化迭代器锁 */
	LWLockInitialize(&istate->lock, LWTRANCHE_SHARED_TIDBITMAP);

	/* 初始化共享迭代器状态 */
	istate->schunkbit = 0;
	istate->schunkptr = 0;
	istate->spageptr = 0;

	tbm->iterating = TBM_ITERATING_SHARED;

	return dp;
}

/*
 * tbm_extract_page_tuple - 从一页中取出元组偏移
 *
 * 取出的偏移存入 TBMIterateResult。
 */
static inline int
tbm_extract_page_tuple(PagetableEntry *page, TBMIterateResult *output)
{
	int			wordnum;
	int			ntuples = 0;

	for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
	{
		bitmapword	w = page->words[wordnum];

		if (w != 0)
		{
			int			off = wordnum * BITS_PER_BITMAPWORD + 1;

			while (w != 0)
			{
				if (w & 1)
					output->offsets[ntuples++] = (OffsetNumber) off;
				off++;
				w >>= 1;
			}
		}
	}

	return ntuples;
}

/*
 *	tbm_advance_schunkbit - 推进 schunkbit
 */
static inline void
tbm_advance_schunkbit(PagetableEntry *chunk, int *schunkbitp)
{
	int			schunkbit = *schunkbitp;

	while (schunkbit < PAGES_PER_CHUNK)
	{
		int			wordnum = WORDNUM(schunkbit);
		int			bitnum = BITNUM(schunkbit);

		if ((chunk->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
			break;
		schunkbit++;
	}

	*schunkbitp = schunkbit;
}

/*
 * tbm_iterate - 扫描 TIDBitmap 的下一页
 *
 * 返回表示一页的 TBMIterateResult，若没有
 * 更多页可扫描则返回 NULL。页保证按数字
 * 顺序交付。若 result->ntuples < 0，则位图是 "有损" 的，未能
 * 记住本页要查看的确切元组——调用方必须
 * 检查该页上的所有元组，并检查它们是否满足预期
 * 条件。若 result->recheck 为 true，则只需检查所指明的元组，
 * 但条件无论如何都必须重新检查。（为便于
 * 测试，当 ntuples < 0 时 recheck 总是被设为 true。）
 */
TBMIterateResult *
tbm_iterate(TBMIterator *iterator)
{
	TIDBitmap  *tbm = iterator->tbm;
	TBMIterateResult *output = &(iterator->output);

	Assert(tbm->iterating == TBM_ITERATING_PRIVATE);

	/*
	 * 若还剩有损组块页，确保我们已经把 schunkptr/
	 * schunkbit 推进到下一个置位。
	 */
	while (iterator->schunkptr < tbm->nchunks)
	{
		PagetableEntry *chunk = tbm->schunks[iterator->schunkptr];
		int			schunkbit = iterator->schunkbit;

		tbm_advance_schunkbit(chunk, &schunkbit);
		if (schunkbit < PAGES_PER_CHUNK)
		{
			iterator->schunkbit = schunkbit;
			break;
		}
		/* 前进到下一个组块 */
		iterator->schunkptr++;
		iterator->schunkbit = 0;
	}

	/*
	 * 若组块数据和按页数据都还剩，必须输出数字上
	 * 更靠前的那一页。
	 */
	if (iterator->schunkptr < tbm->nchunks)
	{
		PagetableEntry *chunk = tbm->schunks[iterator->schunkptr];
		BlockNumber chunk_blockno;

		chunk_blockno = chunk->blockno + iterator->schunkbit;
		if (iterator->spageptr >= tbm->npages ||
			chunk_blockno < tbm->spages[iterator->spageptr]->blockno)
		{
			/* 从组块返回一个有损页指示 */
			output->blockno = chunk_blockno;
			output->ntuples = -1;
			output->recheck = true;
			iterator->schunkbit++;
			return output;
		}
	}

	if (iterator->spageptr < tbm->npages)
	{
		PagetableEntry *page;
		int			ntuples;

		/* 在 TBM_ONE_PAGE 状态下，我们不分配 spages[] 数组 */
		if (tbm->status == TBM_ONE_PAGE)
			page = &tbm->entry1;
		else
			page = tbm->spages[iterator->spageptr];

		/* 扫描位图以取出各个偏移号 */
		ntuples = tbm_extract_page_tuple(page, output);
		output->blockno = page->blockno;
		output->ntuples = ntuples;
		output->recheck = page->recheck;
		iterator->spageptr++;
		return output;
	}

	/* 位图中没有更多内容 */
	return NULL;
}

/*
 *	tbm_shared_iterate - 扫描 TIDBitmap 的下一页
 *
 *	同上，但这将使用在多个进程之间共享的
 *	迭代器来迭代。在访问共享成员之前，
 *	我们需要获取迭代器 LWLock。
 */
TBMIterateResult *
tbm_shared_iterate(TBMSharedIterator *iterator)
{
	TBMIterateResult *output = &iterator->output;
	TBMSharedIteratorState *istate = iterator->state;
	PagetableEntry *ptbase = NULL;
	int		   *idxpages = NULL;
	int		   *idxchunks = NULL;

	if (iterator->ptbase != NULL)
		ptbase = iterator->ptbase->ptentry;
	if (iterator->ptpages != NULL)
		idxpages = iterator->ptpages->index;
	if (iterator->ptchunks != NULL)
		idxchunks = iterator->ptchunks->index;

	/* 在访问共享成员之前获取 LWLock */
	LWLockAcquire(&istate->lock, LW_EXCLUSIVE);

	/*
	 * 若还剩有损组块页，确保我们已经把 schunkptr/
	 * schunkbit 推进到下一个置位。
	 */
	while (istate->schunkptr < istate->nchunks)
	{
		PagetableEntry *chunk = &ptbase[idxchunks[istate->schunkptr]];
		int			schunkbit = istate->schunkbit;

		tbm_advance_schunkbit(chunk, &schunkbit);
		if (schunkbit < PAGES_PER_CHUNK)
		{
			istate->schunkbit = schunkbit;
			break;
		}
		/* 前进到下一个组块 */
		istate->schunkptr++;
		istate->schunkbit = 0;
	}

	/*
	 * 若组块数据和按页数据都还剩，必须输出数字上
	 * 更靠前的那一页。
	 */
	if (istate->schunkptr < istate->nchunks)
	{
		PagetableEntry *chunk = &ptbase[idxchunks[istate->schunkptr]];
		BlockNumber chunk_blockno;

		chunk_blockno = chunk->blockno + istate->schunkbit;

		if (istate->spageptr >= istate->npages ||
			chunk_blockno < ptbase[idxpages[istate->spageptr]].blockno)
		{
			/* 从组块返回一个有损页指示 */
			output->blockno = chunk_blockno;
			output->ntuples = -1;
			output->recheck = true;
			istate->schunkbit++;

			LWLockRelease(&istate->lock);
			return output;
		}
	}

	if (istate->spageptr < istate->npages)
	{
		PagetableEntry *page = &ptbase[idxpages[istate->spageptr]];
		int			ntuples;

		/* 扫描位图以取出各个偏移号 */
		ntuples = tbm_extract_page_tuple(page, output);
		output->blockno = page->blockno;
		output->ntuples = ntuples;
		output->recheck = page->recheck;
		istate->spageptr++;

		LWLockRelease(&istate->lock);

		return output;
	}

	LWLockRelease(&istate->lock);

	/* 位图中没有更多内容 */
	return NULL;
}

/*
 * tbm_end_iterate - 结束对 TIDBitmap 的一次迭代
 *
 * 目前这只是一次 pfree，但将来也许会做更多。（例如，
 * 统计打开的迭代器会有用，并在不再有迭代器时
 * 允许位图回到读/写状态。）
 */
void
tbm_end_iterate(TBMIterator *iterator)
{
	pfree(iterator);
}

/*
 * tbm_end_shared_iterate - 结束对 TIDBitmap 的一次共享迭代
 *
 * 这不释放与迭代器关联的任何共享状态，
 * 只释放我们后端私有的状态。
 */
void
tbm_end_shared_iterate(TBMSharedIterator *iterator)
{
	pfree(iterator);
}

/*
 * tbm_find_pageentry - 为 pageno 查找 PagetableEntry
 *
 * 若该 pageno 没有非有损条目，则返回 NULL。
 */
static const PagetableEntry *
tbm_find_pageentry(const TIDBitmap *tbm, BlockNumber pageno)
{
	const PagetableEntry *page;

	if (tbm->nentries == 0)		/* 以防页表不存在 */
		return NULL;

	if (tbm->status == TBM_ONE_PAGE)
	{
		page = &tbm->entry1;
		if (page->blockno != pageno)
			return NULL;
		Assert(!page->ischunk);
		return page;
	}

	page = pagetable_lookup(tbm->pagetable, pageno);
	if (page == NULL)
		return NULL;
	if (page->ischunk)
		return NULL;			/* 不想要有损组块头 */
	return page;
}

/*
 * tbm_get_pageentry - 为 pageno 查找或创建 PagetableEntry
 *
 * 若是新建的，该条目标为精确（非组块）条目。
 *
 * 这可能使表超过期望的内存大小。若如此，
 * 由调用方在下一个安全点调用 tbm_lossify()。
 */
static PagetableEntry *
tbm_get_pageentry(TIDBitmap *tbm, BlockNumber pageno)
{
	PagetableEntry *page;
	bool		found;

	if (tbm->status == TBM_EMPTY)
	{
		/* 使用固定槽位 */
		page = &tbm->entry1;
		found = false;
		tbm->status = TBM_ONE_PAGE;
	}
	else
	{
		if (tbm->status == TBM_ONE_PAGE)
		{
			page = &tbm->entry1;
			if (page->blockno == pageno)
				return page;
			/* 该从单页切换到哈希表了 */
			tbm_create_pagetable(tbm);
		}

		/* 查找或创建一个条目 */
		page = pagetable_insert(tbm->pagetable, pageno, &found);
	}

	/* 若先前不存在则初始化 */
	if (!found)
	{
		char		oldstatus = page->status;

		MemSet(page, 0, sizeof(PagetableEntry));
		page->status = oldstatus;
		page->blockno = pageno;
		/* 也必须计入 */
		tbm->nentries++;
		tbm->npages++;
	}

	return page;
}

/*
 * tbm_page_is_lossy - 该页是否被标为有损存储？
 */
static bool
tbm_page_is_lossy(const TIDBitmap *tbm, BlockNumber pageno)
{
	PagetableEntry *page;
	BlockNumber chunk_pageno;
	int			bitno;

	/* 若没有有损组块，可以跳过查找 */
	if (tbm->nchunks == 0)
		return false;
	Assert(tbm->status == TBM_HASH);

	bitno = pageno % PAGES_PER_CHUNK;
	chunk_pageno = pageno - bitno;

	page = pagetable_lookup(tbm->pagetable, chunk_pageno);

	if (page != NULL && page->ischunk)
	{
		int			wordnum = WORDNUM(bitno);
		int			bitnum = BITNUM(bitno);

		if ((page->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
			return true;
	}
	return false;
}

/*
 * tbm_mark_page_lossy - 把该页号标为有损存储
 *
 * 这可能使表超过期望的内存大小。若如此，
 * 由调用方在下一个安全点调用 tbm_lossify()。
 */
static void
tbm_mark_page_lossy(TIDBitmap *tbm, BlockNumber pageno)
{
	PagetableEntry *page;
	bool		found;
	BlockNumber chunk_pageno;
	int			bitno;
	int			wordnum;
	int			bitnum;

	/* 一旦位图变成有损，我们就强制它进入哈希表模式 */
	if (tbm->status != TBM_HASH)
		tbm_create_pagetable(tbm);

	bitno = pageno % PAGES_PER_CHUNK;
	chunk_pageno = pageno - bitno;

	/*
	 * 去掉该页现存的任何非有损条目。不过若该页自己就是
	 * 组块头，则跳过这一步，并在下面处理该情形。
	 */
	if (bitno != 0)
	{
		if (pagetable_delete(tbm->pagetable, pageno))
		{
			/* 它存在，因此调整计数 */
			tbm->nentries--;
			tbm->npages--;		/* 假定它必定曾经是非有损的 */
		}
	}

	/* 查找或创建组块头页的条目 */
	page = pagetable_insert(tbm->pagetable, chunk_pageno, &found);

	/* 若先前不存在则初始化 */
	if (!found)
	{
		char		oldstatus = page->status;

		MemSet(page, 0, sizeof(PagetableEntry));
		page->status = oldstatus;
		page->blockno = chunk_pageno;
		page->ischunk = true;
		/* 也必须计入 */
		tbm->nentries++;
		tbm->nchunks++;
	}
	else if (!page->ischunk)
	{
		char		oldstatus = page->status;

		/* 组块头页原先是非有损的，把它变成有损 */
		MemSet(page, 0, sizeof(PagetableEntry));
		page->status = oldstatus;
		page->blockno = chunk_pageno;
		page->ischunk = true;
		/* 我们假定它有某些元组 bit(s) 被置位，因此标为有损 */
		page->words[0] = ((bitmapword) 1 << 0);
		/* 调整计数 */
		tbm->nchunks++;
		tbm->npages--;
	}

	/* 现在设置原来目标页的那一位 */
	wordnum = WORDNUM(bitno);
	bitnum = BITNUM(bitno);
	page->words[wordnum] |= ((bitmapword) 1 << bitnum);
}

/*
 * tbm_lossify - 丢掉一些信息，以便回到内存限制以内
 */
static void
tbm_lossify(TIDBitmap *tbm)
{
	pagetable_iterator i;
	PagetableEntry *page;

	/*
	 * XXX 实现实在笨：这只是按基本上
	 * 随机的顺序把页变成有损。我们其实应当注意
	 * 每一页置了多少位。
	 *
	 * 因为一旦 nentries 超过 maxentries 就会调用我们，应当把
	 * nentries 压到明显低于 maxentries，否则
	 * 很快又得再来一次。我们的目标是 maxentries/2。
	 */
	Assert(tbm->iterating == TBM_NOT_ITERATING);
	Assert(tbm->status == TBM_HASH);

	pagetable_start_iterate_at(tbm->pagetable, &i, tbm->lossify_start);
	while ((page = pagetable_iterate(tbm->pagetable, &i)) != NULL)
	{
		if (page->ischunk)
			continue;			/* 已经是组块头 */

		/*
		 * 若该页会变成组块头，把它变成有损也省不下任何东西，
		 * 因此跳过它。
		 */
		if ((page->blockno % PAGES_PER_CHUNK) == 0)
			continue;

		/* 脏活由这里来做…… */
		tbm_mark_page_lossy(tbm, page->blockno);

		if (tbm->nentries <= tbm->maxentries / 2)
		{
			/*
			 * 我们已经腾出足够空间。记住下一轮从哪里开始
			 * 变成有损，以便均匀地遍历哈希表。
			 */
			tbm->lossify_start = i.cur;
			break;
		}

		/*
		 * 注意：tbm_mark_page_lossy 可能已向
		 * 哈希表插入了一个有损组块，也可能删除了非有损组块。我们仍可
		 * 继续同一次哈希表扫描，因为漏访一个
		 * 元素或访问到新插入的元素并不会致命。
		 */
	}

	/*
	 * 位图很大而 work_mem 很小时，我们可能无法把条目数
	 * 压到 maxentries 以下。若发生这种情况，又会无用地
	 * 一遍遍调用 tbm_lossify。为防止这变成
	 * 性能陷阱，把 maxentries 强制提升到至少为当前
	 * 条目数的两倍。（本质上，这么做是承认无法
	 * 装进 work_mem。）注意，若我们提前跳出循环，这个测试不会触发；
	 * 若没有提前跳出，则当前条目数
	 * 已经无法再减少了。
	 */
	if (tbm->nentries > tbm->maxentries / 2)
		tbm->maxentries = Min(tbm->nentries, (INT_MAX - 1) / 2) * 2;
}

/*
 * 处理 PagetableEntry 指针的 qsort 比较器。
 */
static int
tbm_comparator(const void *left, const void *right)
{
	BlockNumber l = (*((PagetableEntry *const *) left))->blockno;
	BlockNumber r = (*((PagetableEntry *const *) right))->blockno;

	if (l < r)
		return -1;
	else if (l > r)
		return 1;
	return 0;
}

/*
 * 同上，但这会得到 PagetableEntry 数组中的下标。因此
 * 比较 blockno 之前，需要用该下标取得实际的
 * PagetableEntry。
 */
static int
tbm_shared_comparator(const void *left, const void *right, void *arg)
{
	PagetableEntry *base = (PagetableEntry *) arg;
	PagetableEntry *lpage = &base[*(int *) left];
	PagetableEntry *rpage = &base[*(int *) right];

	if (lpage->blockno < rpage->blockno)
		return -1;
	else if (lpage->blockno > rpage->blockno)
		return 1;
	return 0;
}

/*
 *	tbm_attach_shared_iterate
 *
 *	分配一个后端私有迭代器，并把共享迭代器状态
 *	挂上去，以便多个进程可以联合迭代。
 *
 *	我们还会把 DSA 指针转换成局部指针，并存进
 *	我们的私有迭代器。
 */
TBMSharedIterator *
tbm_attach_shared_iterate(dsa_area *dsa, dsa_pointer dp)
{
	TBMSharedIterator *iterator;
	TBMSharedIteratorState *istate;

	/*
	 * 创建 TBMSharedIterator 结构体，并留出足够的尾部空间，
	 * 以满足 TBMIterateResult 子结构体的需要。
	 */
	iterator = (TBMSharedIterator *) palloc0(sizeof(TBMSharedIterator) +
											 MAX_TUPLES_PER_PAGE * sizeof(OffsetNumber));

	istate = (TBMSharedIteratorState *) dsa_get_address(dsa, dp);

	iterator->state = istate;

	iterator->ptbase = dsa_get_address(dsa, istate->pagetable);

	if (istate->npages)
		iterator->ptpages = dsa_get_address(dsa, istate->spages);
	if (istate->nchunks)
		iterator->ptchunks = dsa_get_address(dsa, istate->schunks);

	return iterator;
}

/*
 * pagetable_allocate
 *
 * 为哈希表元素分配内存的回调函数。
 * 为哈希表元素分配内存，若有 DSA 则使用它。
 */
static inline void *
pagetable_allocate(pagetable_hash *pagetable, Size size)
{
	TIDBitmap  *tbm = (TIDBitmap *) pagetable->private_data;
	PTEntryArray *ptbase;

	if (tbm->dsa == NULL)
		return MemoryContextAllocExtended(pagetable->ctx, size,
										  MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

	/*
	 * 在分配新内存之前，把 dsapagetable 引用存进 dsapagetableold，
	 * 以便 pagetable_free 可以释放旧条目。
	 */
	tbm->dsapagetableold = tbm->dsapagetable;
	tbm->dsapagetable = dsa_allocate_extended(tbm->dsa,
											  sizeof(PTEntryArray) + size,
											  DSA_ALLOC_HUGE | DSA_ALLOC_ZERO);
	ptbase = dsa_get_address(tbm->dsa, tbm->dsapagetable);

	return ptbase->ptentry;
}

/*
 * pagetable_free
 *
 * 释放哈希表元素的回调函数。
 */
static inline void
pagetable_free(pagetable_hash *pagetable, void *pointer)
{
	TIDBitmap  *tbm = (TIDBitmap *) pagetable->private_data;

	/* 若没有 DSA，则 pfree 输入指针 */
	if (tbm->dsa == NULL)
		pfree(pointer);
	else if (DsaPointerIsValid(tbm->dsapagetableold))
	{
		dsa_free(tbm->dsa, tbm->dsapagetableold);
		tbm->dsapagetableold = InvalidDsaPointer;
	}
}

/*
 * tbm_calculate_entries
 *
 * 估计在 maxbytes 之内可以有多少哈希表项。
 */
long
tbm_calculate_entries(double maxbytes)
{
	long		nbuckets;

	/*
	 * 估计在 maxbytes 之内可以有多少哈希表项。这里把
	 * 哈希开销估计为 sizeof(PagetableEntry)，对我们的目的
	 * 已经够好。另外为迭代读出期间创建的数组，
	 * 每个条目再计一个额外的 Pointer。
	 */
	nbuckets = maxbytes /
		(sizeof(PagetableEntry) + sizeof(Pointer) + sizeof(Pointer));
	nbuckets = Min(nbuckets, INT_MAX - 1);	/* 安全上限 */
	nbuckets = Max(nbuckets, 16);	/* 健全性上限 */

	return nbuckets;
}
