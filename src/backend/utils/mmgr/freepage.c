/*-------------------------------------------------------------------------
 *
 * freepage.c
 *	  空闲内存页的管理。
 *
 * 本代码旨在为 PostgreSQL 专属编写的内存分配器提供
 * 基础设施。至少在动态共享内存的情况下，我们不能
 * 简单地使用 malloc()，甚至不能使用位于其上的
 * palloc() 之类相对轻量的封装，因为操作系统内建的
 * 任何分配器都无法处理相对指针。将来我们可能会
 * 发现其他一些情形，让我们觉得对自己内存管理有
 * 更大的控制权似乎是可取的。
 *
 * FreePageManager 负责跟踪从某个上层内存分配器的角度
 * 来看，哪些 4kB 内存页当前未被使用。
 * 与 palloc() 这类面向用户的分配器不同，
 * FreePageManager 只能以整页为单位分配和释放，
 * 且释放一次分配时必须知道其页数长度。
 *
 * 由于空闲页管理器只有固定数量的专用内存，而且
 * 没有底层分配器，它就利用交给它管理的空闲页来
 * 存储其簿记数据。它维护多条按跨度大小排序的页
 * 跨度空闲链表；每条空闲链表的头部存储在
 * FreePageManager 自身之中，且每个跨度的第一页包含
 * 指向下一跨度的相对指针。空闲链表管理细节见
 * FreePageManagerGetInternal。
 *
 *
 * 为避免内存碎片化，尽可能合并相邻的页跨度非常
 * 重要；否则，即使有足够的连续空间可用，大的分配
 * 请求也可能无法满足。因此，除空闲链表外，
 * 还维护一棵按页号排序的空闲页范围内存 btree。若
 * 被释放的范围在已空闲范围之前或之后，则扩展
 * 已有范围；若恰好桥接两个空闲范围之间的间隙，
 * 则两个已有范围会与新释放的范围合并，
 * 形成一个
 * 巨大的空闲页范围。
 *
 * 当只有一个空闲页范围时，btree 极简单，直接存
 * 在 FreePageManager 本体之内；否则，按需从受管理
 * 的区域中分配页。即使在内存碎片化非常严重的情
 * 况下，受管理的页也只有极小一部分被这棵 btree
 * 消耗掉。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/freepage.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"

#include "utils/freepage.h"
#include "utils/relptr.h"


/* 标识各种页类型的魔数 */
#define FREE_PAGE_SPAN_LEADER_MAGIC		0xea4020f0
#define FREE_PAGE_LEAF_MAGIC			0x98eae728
#define FREE_PAGE_INTERNAL_MAGIC		0x19aa32c9

/* 空闲页跨度的双向链表；存储于跨度的第一页。 */
struct FreePageSpanLeader
{
	int			magic;			/* 总为 FREE_PAGE_SPAN_LEADER_MAGIC */
	Size		npages;			/* 跨度中的页数 */
	RelptrFreePageSpanLeader prev;
	RelptrFreePageSpanLeader next;
};

/* btree 叶页与内页的公共头部。 */
typedef struct FreePageBtreeHeader
{
	int			magic;			/* FREE_PAGE_LEAF_MAGIC /
								 * FREE_PAGE_INTERNAL_MAGIC */
	Size		nused;			/* 已使用的项数 */
	RelptrFreePageBtree parent; /* 上行 */
} FreePageBtreeHeader;

/* 内部键；指向 btree 的下一层。 */
typedef struct FreePageBtreeInternalKey
{
	Size		first_page;		/* 子页上各键的下界 */
	RelptrFreePageBtree child;	/* 下行 */
} FreePageBtreeInternalKey;

/* 叶键；无负载。 */
typedef struct FreePageBtreeLeafKey
{
	Size		first_page;		/* 跨度首页 */
	Size		npages;			/* 跨度中的页数 */
} FreePageBtreeLeafKey;

/* 计算一页能容纳多少个键。 */
#define FPM_ITEMS_PER_INTERNAL_PAGE \
	((FPM_PAGE_SIZE - sizeof(FreePageBtreeHeader)) / \
		sizeof(FreePageBtreeInternalKey))
#define FPM_ITEMS_PER_LEAF_PAGE \
	((FPM_PAGE_SIZE - sizeof(FreePageBtreeHeader)) / \
		sizeof(FreePageBtreeLeafKey))

/* 任一类型的 btree 页 */
struct FreePageBtree
{
	FreePageBtreeHeader hdr;
	union
	{
		FreePageBtreeInternalKey internal_key[FPM_ITEMS_PER_INTERNAL_PAGE];
		FreePageBtreeLeafKey leaf_key[FPM_ITEMS_PER_LEAF_PAGE];
	}			u;
};

/* btree 搜索的结果 */
typedef struct FreePageBtreeSearchResult
{
	FreePageBtree *page;
	Size		index;
	bool		found;
	unsigned	split_pages;
} FreePageBtreeSearchResult;

/* 辅助函数 */
static void FreePageBtreeAdjustAncestorKeys(FreePageManager *fpm,
											FreePageBtree *btp);
static Size FreePageBtreeCleanup(FreePageManager *fpm);
static FreePageBtree *FreePageBtreeFindLeftSibling(char *base,
												   FreePageBtree *btp);
static FreePageBtree *FreePageBtreeFindRightSibling(char *base,
													FreePageBtree *btp);
static Size FreePageBtreeFirstKey(FreePageBtree *btp);
static FreePageBtree *FreePageBtreeGetRecycled(FreePageManager *fpm);
static void FreePageBtreeInsertInternal(char *base, FreePageBtree *btp,
										Size index, Size first_page, FreePageBtree *child);
static void FreePageBtreeInsertLeaf(FreePageBtree *btp, Size index,
									Size first_page, Size npages);
static void FreePageBtreeRecycle(FreePageManager *fpm, Size pageno);
static void FreePageBtreeRemove(FreePageManager *fpm, FreePageBtree *btp,
								Size index);
static void FreePageBtreeRemovePage(FreePageManager *fpm, FreePageBtree *btp);
static void FreePageBtreeSearch(FreePageManager *fpm, Size first_page,
								FreePageBtreeSearchResult *result);
static Size FreePageBtreeSearchInternal(FreePageBtree *btp, Size first_page);
static Size FreePageBtreeSearchLeaf(FreePageBtree *btp, Size first_page);
static FreePageBtree *FreePageBtreeSplitPage(FreePageManager *fpm,
											 FreePageBtree *btp);
static void FreePageBtreeUpdateParentPointers(char *base, FreePageBtree *btp);
static void FreePageManagerDumpBtree(FreePageManager *fpm, FreePageBtree *btp,
									 FreePageBtree *parent, int level, StringInfo buf);
static void FreePageManagerDumpSpans(FreePageManager *fpm,
									 FreePageSpanLeader *span, Size expected_pages,
									 StringInfo buf);
static bool FreePageManagerGetInternal(FreePageManager *fpm, Size npages,
									   Size *first_page);
static Size FreePageManagerPutInternal(FreePageManager *fpm, Size first_page,
									   Size npages, bool soft);
static void FreePagePopSpanLeader(FreePageManager *fpm, Size pageno);
static void FreePagePushSpanLeader(FreePageManager *fpm, Size first_page,
								   Size npages);
static Size FreePageManagerLargestContiguous(FreePageManager *fpm);
static void FreePageManagerUpdateLargest(FreePageManager *fpm);

#ifdef FPM_EXTRA_ASSERTS
static Size sum_free_pages(FreePageManager *fpm);
#endif

/*
 * 初始化新的空白空闲页管理器。
 *
 * 'fpm' 应指向调用方提供的内存，足以容纳
 * FreePageManager，在此初始化它。
 *
 * 'base' 是所有指针的相对基准地址。管理动态共享
 * 内存段时，它通常应为段的基地址。管理后端私有
 * 内存时，它可以是 NULL，或者在管理单块连续内存
 * 时为该内存区的起点。
 */
void
FreePageManagerInitialize(FreePageManager *fpm, char *base)
{
	Size		f;

	relptr_store(base, fpm->self, fpm);
	relptr_store(base, fpm->btree_root, (FreePageBtree *) NULL);
	relptr_store(base, fpm->btree_recycle, (FreePageSpanLeader *) NULL);
	fpm->btree_depth = 0;
	fpm->btree_recycle_count = 0;
	fpm->singleton_first_page = 0;
	fpm->singleton_npages = 0;
	fpm->contiguous_pages = 0;
	fpm->contiguous_pages_dirty = true;
#ifdef FPM_EXTRA_ASSERTS
	fpm->free_pages = 0;
#endif

	for (f = 0; f < FPM_NUM_FREELISTS; f++)
		relptr_store(base, fpm->freelist[f], (FreePageSpanLeader *) NULL);
}

/*
 * 从空闲页管理器分配给定长度的页跨度。
 * 返回值表明我们是否能够满足该请求；若为真，
 * 分配的第一页会存储在 *first_page 中。
 */
bool
FreePageManagerGet(FreePageManager *fpm, Size npages, Size *first_page)
{
	bool		result;
	Size		contiguous_pages;

	result = FreePageManagerGetInternal(fpm, npages, first_page);

	/*
	 * 这有点违反直觉，但分配页实际上可能创造出能让
	 * 更大范围出现的清理机会。我们可能从 btree 中取出
	 * 一个键，使 btree 回收链表头部的项得以插入；而若
	 * 其后还有更多项，其中之一可能使当前分离的两个
	 * 范围合并，形成比以往任何范围都更大的连续
	 * 页范围。也许值得尝试改进清理算法以避免这类
	 * 边界情形，但目前我们只是注意到该条件
	 * 并做相应
	 * 的报告。
	 */
	contiguous_pages = FreePageBtreeCleanup(fpm);
	if (fpm->contiguous_pages < contiguous_pages)
		fpm->contiguous_pages = contiguous_pages;

	/*
	 * FreePageManagerGetInternal可能已置contiguous_pages_dirty，
	 * 若如此则重算该值。
	 */
	FreePageManagerUpdateLargest(fpm);

#ifdef FPM_EXTRA_ASSERTS
	if (result)
	{
		Assert(fpm->free_pages >= npages);
		fpm->free_pages -= npages;
	}
	Assert(fpm->free_pages == sum_free_pages(fpm));
	Assert(fpm->contiguous_pages == FreePageManagerLargestContiguous(fpm));
#endif
	return result;
}

#ifdef FPM_EXTRA_ASSERTS
static void
sum_free_pages_recurse(FreePageManager *fpm, FreePageBtree *btp, Size *sum)
{
	char	   *base = fpm_segment_base(fpm);

	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC ||
		   btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	++*sum;
	if (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
	{
		Size		index;


		for (index = 0; index < btp->hdr.nused; ++index)
		{
			FreePageBtree *child;

			child = relptr_access(base, btp->u.internal_key[index].child);
			sum_free_pages_recurse(fpm, child, sum);
		}
	}
}
static Size
sum_free_pages(FreePageManager *fpm)
{
	FreePageSpanLeader *recycle;
	char	   *base = fpm_segment_base(fpm);
	Size		sum = 0;
	int			list;

	/* 通过扫描空闲链表统计跨度。 */
	for (list = 0; list < FPM_NUM_FREELISTS; ++list)
	{

		if (!relptr_is_null(fpm->freelist[list]))
		{
			FreePageSpanLeader *candidate =
				relptr_access(base, fpm->freelist[list]);

			do
			{
				sum += candidate->npages;
				candidate = relptr_access(base, candidate->next);
			} while (candidate != NULL);
		}
	}

	/* 统计 btree 内页。 */
	if (fpm->btree_depth > 0)
	{
		FreePageBtree *root = relptr_access(base, fpm->btree_root);

		sum_free_pages_recurse(fpm, root, &sum);
	}

	/* 统计回收链表。 */
	for (recycle = relptr_access(base, fpm->btree_recycle);
		 recycle != NULL;
		 recycle = relptr_access(base, recycle->next))
	{
		Assert(recycle->npages == 1);
		++sum;
	}

	return sum;
}
#endif

/*
 * 计算用户能够成功获取的最大页跨度大小。
 *
 */
static Size
FreePageManagerLargestContiguous(FreePageManager *fpm)
{
	char	   *base;
	Size		largest;

	base = fpm_segment_base(fpm);
	largest = 0;
	if (!relptr_is_null(fpm->freelist[FPM_NUM_FREELISTS - 1]))
	{
		FreePageSpanLeader *candidate;

		candidate = relptr_access(base, fpm->freelist[FPM_NUM_FREELISTS - 1]);
		do
		{
			if (candidate->npages > largest)
				largest = candidate->npages;
			candidate = relptr_access(base, candidate->next);
		} while (candidate != NULL);
	}
	else
	{
		Size		f = FPM_NUM_FREELISTS - 1;

		do
		{
			--f;
			if (!relptr_is_null(fpm->freelist[f]))
			{
				largest = f + 1;
				break;
			}
		} while (f > 0);
	}

	return largest;
}

/*
 * 若已标记为脏，则重新计算用户能够成功获取的
 * 最大页跨度大小。
 */
static void
FreePageManagerUpdateLargest(FreePageManager *fpm)
{
	if (!fpm->contiguous_pages_dirty)
		return;

	fpm->contiguous_pages = FreePageManagerLargestContiguous(fpm);
	fpm->contiguous_pages_dirty = false;
}

/*
 * 把一段页跨度移交给空闲页管理器。
 */
void
FreePageManagerPut(FreePageManager *fpm, Size first_page, Size npages)
{
	Size		contiguous_pages;

	Assert(npages > 0);

	/* 记录新页。 */
	contiguous_pages =
		FreePageManagerPutInternal(fpm, first_page, npages, false);

	/*
	 * 若我们插入页管理器的新范围与已有范围相邻，
	 * 则它可能开启了清理机会。
	 */
	if (contiguous_pages > npages)
	{
		Size		cleanup_contiguous_pages;

		cleanup_contiguous_pages = FreePageBtreeCleanup(fpm);
		if (cleanup_contiguous_pages > contiguous_pages)
			contiguous_pages = cleanup_contiguous_pages;
	}

	/* 看看现在是否有新的最大块。 */
	if (fpm->contiguous_pages < contiguous_pages)
		fpm->contiguous_pages = contiguous_pages;

	/*
	 * 先前调用 FreePageManagerPutInternal 时可能已设置
	 * contiguous_pages_dirty（若它需要分配内页），
	 * 必要时重算 contiguous_pages。
	 */
	FreePageManagerUpdateLargest(fpm);

#ifdef FPM_EXTRA_ASSERTS
	fpm->free_pages += npages;
	Assert(fpm->free_pages == sum_free_pages(fpm));
	Assert(fpm->contiguous_pages == FreePageManagerLargestContiguous(fpm));
#endif
}

/*
 * 生成空闲页管理器状态的调试转储。
 */
char *
FreePageManagerDump(FreePageManager *fpm)
{
	char	   *base = fpm_segment_base(fpm);
	StringInfoData buf;
	FreePageSpanLeader *recycle;
	bool		dumped_any_freelist = false;
	Size		f;

	/* 初始化输出缓冲。 */
	initStringInfo(&buf);

	/* 转储概况。 */
	appendStringInfo(&buf, "metadata: self %zu max contiguous pages = %zu\n",
					 relptr_offset(fpm->self), fpm->contiguous_pages);

	/* 转储btree */
	if (fpm->btree_depth > 0)
	{
		FreePageBtree *root;

		appendStringInfo(&buf, "btree depth %u:\n", fpm->btree_depth);
		root = relptr_access(base, fpm->btree_root);
		FreePageManagerDumpBtree(fpm, root, NULL, 0, &buf);
	}
	else if (fpm->singleton_npages > 0)
	{
		appendStringInfo(&buf, "singleton: %zu(%zu)\n",
						 fpm->singleton_first_page, fpm->singleton_npages);
	}

	/*转储btree回收链表。*/
	recycle = relptr_access(base, fpm->btree_recycle);
	if (recycle != NULL)
	{
		appendStringInfoString(&buf, "btree recycle:");
		FreePageManagerDumpSpans(fpm, recycle, 1, &buf);
	}

	/*转储空闲链表*/
	for (f = 0; f < FPM_NUM_FREELISTS; ++f)
	{
		FreePageSpanLeader *span;

		if (relptr_is_null(fpm->freelist[f]))
			continue;
		if (!dumped_any_freelist)
		{
			appendStringInfoString(&buf, "freelists:\n");
			dumped_any_freelist = true;
		}
		appendStringInfo(&buf, "  %zu:", f + 1);
		span = relptr_access(base, fpm->freelist[f]);
		FreePageManagerDumpSpans(fpm, span, f + 1, &buf);
	}

	/* 把结果交还调用方。 */
	return buf.data;
}


/*
 * 任何非根页中索引零处存储的 first_page 值，必须与
 * 其父页中指向该页的那个索引处存储的 first_page
 * 值一致。因此当 btree 页中索引零处存储的值变化时，
 * 我们必须沿树向上行走并调整祖先键，直到到达某个
 * 该键不再位于索引零处的祖先。本函数应当在更新
 * 目标页的第一个键之后调用；它会按需向上
 * 传播该变更。
 *
 * 我们在此假设：页上的第一个键尚未变化到需要
 * 改变其祖先页上各键排序的程度。因此，若我们在
 * 父页中搜索第一个大于等于当前页第一个键的键，
 * 那么指向本页的下行链接要么就是搜索返回的索引
 * （若第一个键变小了），要么比它小1（若第一个键
 * 变大了）。
 */
static void
FreePageBtreeAdjustAncestorKeys(FreePageManager *fpm, FreePageBtree *btp)
{
	char	   *base = fpm_segment_base(fpm);
	Size		first_page;
	FreePageBtree *parent;
	FreePageBtree *child;

	/* 这可能是叶页，也可能是内页。 */
	Assert(btp->hdr.nused > 0);
	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
	{
		Assert(btp->hdr.nused <= FPM_ITEMS_PER_LEAF_PAGE);
		first_page = btp->u.leaf_key[0].first_page;
	}
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		Assert(btp->hdr.nused <= FPM_ITEMS_PER_INTERNAL_PAGE);
		first_page = btp->u.internal_key[0].first_page;
	}
	child = btp;

	/* 循环，直到找到无需调整的祖先为止。 */
	for (;;)
	{
		Size		s;

		parent = relptr_access(base, child->hdr.parent);
		if (parent == NULL)
			break;
		s = FreePageBtreeSearchInternal(parent, first_page);

		/* 键位于索引 s 或 s-1 处；确定是哪一个。 */
		if (s >= parent->hdr.nused)
		{
			Assert(s == parent->hdr.nused);
			--s;
		}
		else
		{
			FreePageBtree *check;

			check = relptr_access(base, parent->u.internal_key[s].child);
			if (check != child)
			{
				Assert(s > 0);
				--s;
			}
		}

#ifdef USE_ASSERT_CHECKING
		/* 调试复核。 */
		{
			FreePageBtree *check;

			check = relptr_access(base, parent->u.internal_key[s].child);
			Assert(s < parent->hdr.nused);
			Assert(child == check);
		}
#endif

		/* 更新父键。 */
		parent->u.internal_key[s].first_page = first_page;

		/*
		 * 若是父页首键，则再上一层；否则结束。
		 *
		 */
		if (s > 0)
			break;
		child = parent;
	}
}

/*
 * 尝试从空闲页 btree 中回收空间。返回值为清理操作
 * 所创建的最大连续页范围。
 */
static Size
FreePageBtreeCleanup(FreePageManager *fpm)
{
	char	   *base = fpm_segment_base(fpm);
	Size		max_contiguous_pages = 0;

	/* 尝试缩小 btree 的深度。 */
	while (!relptr_is_null(fpm->btree_root))
	{
		FreePageBtree *root = relptr_access(base, fpm->btree_root);

		/* 若根页只含一个键，则深度减一。 */
		if (root->hdr.nused == 1)
		{
			/* 树的深度减一。 */
			Assert(fpm->btree_depth > 0);
			--fpm->btree_depth;
			if (root->hdr.magic == FREE_PAGE_LEAF_MAGIC)
			{
				/* 若根为叶页，则把唯一项转为单页范围。 */
				relptr_store(base, fpm->btree_root, (FreePageBtree *) NULL);
				fpm->singleton_first_page = root->u.leaf_key[0].first_page;
				fpm->singleton_npages = root->u.leaf_key[0].npages;
			}
			else
			{
				FreePageBtree *newroot;

				/* 若根为内页，则让唯一的子页成为根。 */
				Assert(root->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
				relptr_copy(fpm->btree_root, root->u.internal_key[0].child);
				newroot = relptr_access(base, fpm->btree_root);
				relptr_store(base, newroot->hdr.parent, (FreePageBtree *) NULL);
			}
			FreePageBtreeRecycle(fpm, fpm_pointer_to_page(base, root));
		}
		else if (root->hdr.nused == 2 &&
				 root->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		{
			Size		end_of_first;
			Size		start_of_second;

			end_of_first = root->u.leaf_key[0].first_page +
				root->u.leaf_key[0].npages;
			start_of_second = root->u.leaf_key[1].first_page;

			if (end_of_first + 1 == start_of_second)
			{
				Size		root_page = fpm_pointer_to_page(base, root);

				if (end_of_first == root_page)
				{
					FreePagePopSpanLeader(fpm, root->u.leaf_key[0].first_page);
					FreePagePopSpanLeader(fpm, root->u.leaf_key[1].first_page);
					fpm->singleton_first_page = root->u.leaf_key[0].first_page;
					fpm->singleton_npages = root->u.leaf_key[0].npages +
						root->u.leaf_key[1].npages + 1;
					fpm->btree_depth = 0;
					relptr_store(base, fpm->btree_root,
								 (FreePageBtree *) NULL);
					FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
										   fpm->singleton_npages);
					Assert(max_contiguous_pages == 0);
					max_contiguous_pages = fpm->singleton_npages;
				}
			}

			/* 无论成败，现在都该停止了。 */
			break;
		}
		else
		{
			/* 无事可做。停止。 */
			break;
		}
	}

	/*
	 * 尝试释放已回收的 btree 页。若释放该回收页会导致
	 * btree 页分裂，我们就跳过这步：我们试图回收的
	 * 页会被分裂消耗，那会
	 * 适得其反。
	 *
	 * 另外，目前我们也只尝试回收链表中的
	 * 第一页；本可以做得更激进，但不清楚这种
	 * 复杂度是否值得。
	 */
	while (fpm->btree_recycle_count > 0)
	{
		FreePageBtree *btp;
		Size		first_page;
		Size		contiguous_pages;

		btp = FreePageBtreeGetRecycled(fpm);
		first_page = fpm_pointer_to_page(base, btp);
		contiguous_pages = FreePageManagerPutInternal(fpm, first_page, 1, true);
		if (contiguous_pages == 0)
		{
			FreePageBtreeRecycle(fpm, first_page);
			break;
		}
		else
		{
			if (contiguous_pages > max_contiguous_pages)
				max_contiguous_pages = contiguous_pages;
		}
	}

	return max_contiguous_pages;
}

/*
 * 考虑把给定页与其左兄弟或右兄弟页合并，若该页
 * 相当空的话。
 */
static void
FreePageBtreeConsolidate(FreePageManager *fpm, FreePageBtree *btp)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtree *np;
	Size		max;

	/*
	 * 我们只尝试合并占用不足三分之一的页。本来可
	 * 以更激进一些，但那样可能冒风险：刚完成合并，
	 * 随后不久又要再次分裂。由于 btree 与受管理空间
	 * 相比非常小，我们的目标并非确保它始终恰好占据
	 * 绝对最少的页数，而是要在事态变得
	 * 过于糟糕之前
	 * 回收页。
	 */
	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		max = FPM_ITEMS_PER_LEAF_PAGE;
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		max = FPM_ITEMS_PER_INTERNAL_PAGE;
	}
	if (btp->hdr.nused >= max / 3)
		return;

	/*
	 * 若右兄弟的键能装进本页，则进行合并。
	 */
	np = FreePageBtreeFindRightSibling(base, btp);
	if (np != NULL && btp->hdr.nused + np->hdr.nused <= max)
	{
		if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		{
			memcpy(&btp->u.leaf_key[btp->hdr.nused], &np->u.leaf_key[0],
				   sizeof(FreePageBtreeLeafKey) * np->hdr.nused);
			btp->hdr.nused += np->hdr.nused;
		}
		else
		{
			memcpy(&btp->u.internal_key[btp->hdr.nused], &np->u.internal_key[0],
				   sizeof(FreePageBtreeInternalKey) * np->hdr.nused);
			btp->hdr.nused += np->hdr.nused;
			FreePageBtreeUpdateParentPointers(base, btp);
		}
		FreePageBtreeRemovePage(fpm, np);
		return;
	}

	/*
	 * 若我们的键能装进左兄弟的页，则进行合并。此时
	 * 我们把键移到另一页而非相反，以免必须调整
	 * 祖先键。
	 */
	np = FreePageBtreeFindLeftSibling(base, btp);
	if (np != NULL && btp->hdr.nused + np->hdr.nused <= max)
	{
		if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		{
			memcpy(&np->u.leaf_key[np->hdr.nused], &btp->u.leaf_key[0],
				   sizeof(FreePageBtreeLeafKey) * btp->hdr.nused);
			np->hdr.nused += btp->hdr.nused;
		}
		else
		{
			memcpy(&np->u.internal_key[np->hdr.nused], &btp->u.internal_key[0],
				   sizeof(FreePageBtreeInternalKey) * btp->hdr.nused);
			np->hdr.nused += btp->hdr.nused;
			FreePageBtreeUpdateParentPointers(base, np);
		}
		FreePageBtreeRemovePage(fpm, btp);
		return;
	}
}

/*
 * 查找给定页的左兄弟；即树中同一层上其键空间紧
 * 邻于我们的那一页。
 */
static FreePageBtree *
FreePageBtreeFindLeftSibling(char *base, FreePageBtree *btp)
{
	FreePageBtree *p = btp;
	int			levels = 0;

	/* 上移，直到能向左移。 */
	for (;;)
	{
		Size		first_page;
		Size		index;

		first_page = FreePageBtreeFirstKey(p);
		p = relptr_access(base, p->hdr.parent);

		if (p == NULL)
			return NULL;		/* 传入的已是最右页 */

		index = FreePageBtreeSearchInternal(p, first_page);
		if (index > 0)
		{
			Assert(p->u.internal_key[index].first_page == first_page);
			p = relptr_access(base, p->u.internal_key[index - 1].child);
			break;
		}
		Assert(index == 0);
		++levels;
	}

	/*向左下降。*/
	while (levels > 0)
	{
		Assert(p->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		p = relptr_access(base, p->u.internal_key[p->hdr.nused - 1].child);
		--levels;
	}
	Assert(p->hdr.magic == btp->hdr.magic);

	return p;
}

/*
 * 查找给定页的右兄弟；即树中同一层上其键空间紧
 * 随于我们的那一页。
 */
static FreePageBtree *
FreePageBtreeFindRightSibling(char *base, FreePageBtree *btp)
{
	FreePageBtree *p = btp;
	int			levels = 0;

	/* 上移，直到能向右移。 */
	for (;;)
	{
		Size		first_page;
		Size		index;

		first_page = FreePageBtreeFirstKey(p);
		p = relptr_access(base, p->hdr.parent);

		if (p == NULL)
			return NULL;		/* 传入的已是最右页 */

		index = FreePageBtreeSearchInternal(p, first_page);
		if (index < p->hdr.nused - 1)
		{
			Assert(p->u.internal_key[index].first_page == first_page);
			p = relptr_access(base, p->u.internal_key[index + 1].child);
			break;
		}
		Assert(index == p->hdr.nused - 1);
		++levels;
	}

	/*向左下降。*/
	while (levels > 0)
	{
		Assert(p->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		p = relptr_access(base, p->u.internal_key[0].child);
		--levels;
	}
	Assert(p->hdr.magic == btp->hdr.magic);

	return p;
}

/*
 * 获取btree页的首键。
 */
static Size
FreePageBtreeFirstKey(FreePageBtree *btp)
{
	Assert(btp->hdr.nused > 0);

	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		return btp->u.leaf_key[0].first_page;
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		return btp->u.internal_key[0].first_page;
	}
}

/*
 * 从 btree 回收链表中取一页用作 btree 页。
 */
static FreePageBtree *
FreePageBtreeGetRecycled(FreePageManager *fpm)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *victim = relptr_access(base, fpm->btree_recycle);
	FreePageSpanLeader *newhead;

	Assert(victim != NULL);
	newhead = relptr_access(base, victim->next);
	if (newhead != NULL)
		relptr_copy(newhead->prev, victim->prev);
	relptr_store(base, fpm->btree_recycle, newhead);
	Assert(fpm_pointer_is_page_aligned(base, victim));
	fpm->btree_recycle_count--;
	return (FreePageBtree *) victim;
}

/*
 * 向内页插入一项（必须有空间）。
 */
static void
FreePageBtreeInsertInternal(char *base, FreePageBtree *btp, Size index,
							Size first_page, FreePageBtree *child)
{
	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
	Assert(btp->hdr.nused < FPM_ITEMS_PER_INTERNAL_PAGE);
	Assert(index <= btp->hdr.nused);
	memmove(&btp->u.internal_key[index + 1], &btp->u.internal_key[index],
			sizeof(FreePageBtreeInternalKey) * (btp->hdr.nused - index));
	btp->u.internal_key[index].first_page = first_page;
	relptr_store(base, btp->u.internal_key[index].child, child);
	++btp->hdr.nused;
}

/*
 * 向叶页插入一项（必须有空间）。
 */
static void
FreePageBtreeInsertLeaf(FreePageBtree *btp, Size index, Size first_page,
						Size npages)
{
	Assert(btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	Assert(btp->hdr.nused < FPM_ITEMS_PER_LEAF_PAGE);
	Assert(index <= btp->hdr.nused);
	memmove(&btp->u.leaf_key[index + 1], &btp->u.leaf_key[index],
			sizeof(FreePageBtreeLeafKey) * (btp->hdr.nused - index));
	btp->u.leaf_key[index].first_page = first_page;
	btp->u.leaf_key[index].npages = npages;
	++btp->hdr.nused;
}

/*
 * 把一页放入 btree 回收链表。
 */
static void
FreePageBtreeRecycle(FreePageManager *fpm, Size pageno)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *head = relptr_access(base, fpm->btree_recycle);
	FreePageSpanLeader *span;

	span = (FreePageSpanLeader *) fpm_page_to_pointer(base, pageno);
	span->magic = FREE_PAGE_SPAN_LEADER_MAGIC;
	span->npages = 1;
	relptr_store(base, span->next, head);
	relptr_store(base, span->prev, (FreePageSpanLeader *) NULL);
	if (head != NULL)
		relptr_store(base, head->prev, span);
	relptr_store(base, fpm->btree_recycle, span);
	fpm->btree_recycle_count++;
}

/*
 * 从给定页上给定位置处删除 btree 中的一项。
 */
static void
FreePageBtreeRemove(FreePageManager *fpm, FreePageBtree *btp, Size index)
{
	Assert(btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	Assert(index < btp->hdr.nused);

	/* 删除最后一项时，将整页从 btree 中根除。 */
	if (btp->hdr.nused == 1)
	{
		FreePageBtreeRemovePage(fpm, btp);
		return;
	}

	/* 从页上物理移除该键。 */
	--btp->hdr.nused;
	if (index < btp->hdr.nused)
		memmove(&btp->u.leaf_key[index], &btp->u.leaf_key[index + 1],
				sizeof(FreePageBtreeLeafKey) * (btp->hdr.nused - index));

	/* 若刚移除的是第一个键，则调整祖先键。 */
	if (index == 0)
		FreePageBtreeAdjustAncestorKeys(fpm, btp);

	/* 考虑是否将本页与兄弟页合并。 */
	FreePageBtreeConsolidate(fpm, btp);
}

/*
 * 从 btree 中移除一页。调用方负责迁移走本页上仍
 * 需要的键。该页会被放入回收
 * 链表。
 */
static void
FreePageBtreeRemovePage(FreePageManager *fpm, FreePageBtree *btp)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtree *parent;
	Size		index;
	Size		first_page;

	for (;;)
	{
		/* 找到父页。 */
		parent = relptr_access(base, btp->hdr.parent);
		if (parent == NULL)
		{
			/* 我们正在移除根页。 */
			relptr_store(base, fpm->btree_root, (FreePageBtree *) NULL);
			fpm->btree_depth = 0;
			Assert(fpm->singleton_first_page == 0);
			Assert(fpm->singleton_npages == 0);
			return;
		}

		/*
		 * 若父页只含一项，我们也需要将其移除。
		 */
		if (parent->hdr.nused > 1)
			break;
		FreePageBtreeRecycle(fpm, fpm_pointer_to_page(base, btp));
		btp = parent;
	}

	/*找到并移除下行链接。*/
	first_page = FreePageBtreeFirstKey(btp);
	if (parent->hdr.magic == FREE_PAGE_LEAF_MAGIC)
	{
		index = FreePageBtreeSearchLeaf(parent, first_page);
		Assert(index < parent->hdr.nused);
		if (index < parent->hdr.nused - 1)
			memmove(&parent->u.leaf_key[index],
					&parent->u.leaf_key[index + 1],
					sizeof(FreePageBtreeLeafKey)
					* (parent->hdr.nused - index - 1));
	}
	else
	{
		index = FreePageBtreeSearchInternal(parent, first_page);
		Assert(index < parent->hdr.nused);
		if (index < parent->hdr.nused - 1)
			memmove(&parent->u.internal_key[index],
					&parent->u.internal_key[index + 1],
					sizeof(FreePageBtreeInternalKey)
					* (parent->hdr.nused - index - 1));
	}
	parent->hdr.nused--;
	Assert(parent->hdr.nused > 0);

	/* 回收该页。 */
	FreePageBtreeRecycle(fpm, fpm_pointer_to_page(base, btp));

	/* 按需调整祖先键。 */
	if (index == 0)
		FreePageBtreeAdjustAncestorKeys(fpm, parent);

	/* 考虑是否将父页与兄弟页合并。 */
	FreePageBtreeConsolidate(fpm, parent);
}

/*
 * 在 btree 中搜索给定首页对应的项，并用搜索结果
 * 初始化 *result。result->page 与 result->index 或给出
 * 精确匹配的位置，或给出应插入新键的位置。
 * result->found 在精确匹配时为真，否则为假。
 * result->split_pages 将包含执行分裂以插入键时
 * 所需的额外 btree 页数。除非上文另有说明，返回时
 * 结果对象中各字段的
 * 内容未定义。
 */
static void
FreePageBtreeSearch(FreePageManager *fpm, Size first_page,
					FreePageBtreeSearchResult *result)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtree *btp = relptr_access(base, fpm->btree_root);
	Size		index;

	result->split_pages = 1;

	/* 若 btree 为空，则无可查找之物。 */
	if (btp == NULL)
	{
		result->page = NULL;
		result->found = false;
		return;
	}

	/* 下行至叶页为止。 */
	while (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
	{
		FreePageBtree *child;
		bool		found_exact;

		index = FreePageBtreeSearchInternal(btp, first_page);
		found_exact = index < btp->hdr.nused &&
			btp->u.internal_key[index].first_page == first_page;

		/*
		 * 若找到精确匹配则直接下行。否则，尽量
		 * 向左下行入子页，以便能在其高端找到
		 * 插入点。
		 */
		if (!found_exact && index > 0)
			--index;

		/* 跟踪叶插入所需的分裂深度。 */
		if (btp->hdr.nused >= FPM_ITEMS_PER_INTERNAL_PAGE)
		{
			Assert(btp->hdr.nused == FPM_ITEMS_PER_INTERNAL_PAGE);
			result->split_pages++;
		}
		else
			result->split_pages = 0;

		/* 下行到相应的子页。 */
		Assert(index < btp->hdr.nused);
		child = relptr_access(base, btp->u.internal_key[index].child);
		Assert(relptr_access(base, child->hdr.parent) == btp);
		btp = child;
	}

	/* 跟踪叶插入所需的分裂深度。 */
	if (btp->hdr.nused >= FPM_ITEMS_PER_LEAF_PAGE)
	{
		Assert(btp->hdr.nused == FPM_ITEMS_PER_INTERNAL_PAGE);
		result->split_pages++;
	}
	else
		result->split_pages = 0;

	/* 搜索叶页。 */
	index = FreePageBtreeSearchLeaf(btp, first_page);

	/* 汇总结果。 */
	result->page = btp;
	result->index = index;
	result->found = index < btp->hdr.nused &&
		first_page == btp->u.leaf_key[index].first_page;
}

/*
 * 在内页中搜索第一个大于等于给定页号的键。返回
 * 该键的索引；若不存在，则返回比页上键数大1的
 * 值。
 */
static Size
FreePageBtreeSearchInternal(FreePageBtree *btp, Size first_page)
{
	Size		low = 0;
	Size		high = btp->hdr.nused;

	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
	Assert(high > 0 && high <= FPM_ITEMS_PER_INTERNAL_PAGE);

	while (low < high)
	{
		Size		mid = (low + high) / 2;
		Size		val = btp->u.internal_key[mid].first_page;

		if (first_page == val)
			return mid;
		else if (first_page < val)
			high = mid;
		else
			low = mid + 1;
	}

	return low;
}

/*
 * 在叶页中搜索第一个大于等于给定页号的键。返回
 * 该键的索引；若不存在，则返回比页上键数大1的
 * 值。
 */
static Size
FreePageBtreeSearchLeaf(FreePageBtree *btp, Size first_page)
{
	Size		low = 0;
	Size		high = btp->hdr.nused;

	Assert(btp->hdr.magic == FREE_PAGE_LEAF_MAGIC);
	Assert(high > 0 && high <= FPM_ITEMS_PER_LEAF_PAGE);

	while (low < high)
	{
		Size		mid = (low + high) / 2;
		Size		val = btp->u.leaf_key[mid].first_page;

		if (first_page == val)
			return mid;
		else if (first_page < val)
			high = mid;
		else
			low = mid + 1;
	}

	return low;
}

/*
 * 分配一个新的 btree 页，并把给定页上一半的键移
 * 到新页。调用方须确保 fpm->btree_recycle 中有可
 * 用的页。返回指向新页的指针，调用方必须向其
 * 添加下行链接。
 */
static FreePageBtree *
FreePageBtreeSplitPage(FreePageManager *fpm, FreePageBtree *btp)
{
	FreePageBtree *newsibling;

	newsibling = FreePageBtreeGetRecycled(fpm);
	newsibling->hdr.magic = btp->hdr.magic;
	newsibling->hdr.nused = btp->hdr.nused / 2;
	relptr_copy(newsibling->hdr.parent, btp->hdr.parent);
	btp->hdr.nused -= newsibling->hdr.nused;

	if (btp->hdr.magic == FREE_PAGE_LEAF_MAGIC)
		memcpy(&newsibling->u.leaf_key,
			   &btp->u.leaf_key[btp->hdr.nused],
			   sizeof(FreePageBtreeLeafKey) * newsibling->hdr.nused);
	else
	{
		Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
		memcpy(&newsibling->u.internal_key,
			   &btp->u.internal_key[btp->hdr.nused],
			   sizeof(FreePageBtreeInternalKey) * newsibling->hdr.nused);
		FreePageBtreeUpdateParentPointers(fpm_segment_base(fpm), newsibling);
	}

	return newsibling;
}

/*
 * 当内页被分裂或合并时，其子页的父指针必须更
 * 新。
 */
static void
FreePageBtreeUpdateParentPointers(char *base, FreePageBtree *btp)
{
	Size		i;

	Assert(btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC);
	for (i = 0; i < btp->hdr.nused; ++i)
	{
		FreePageBtree *child;

		child = relptr_access(base, btp->u.internal_key[i].child);
		relptr_store(base, child->hdr.parent, btp);
	}
}

/*
 * 调试转储 btree 数据。
 */
static void
FreePageManagerDumpBtree(FreePageManager *fpm, FreePageBtree *btp,
						 FreePageBtree *parent, int level, StringInfo buf)
{
	char	   *base = fpm_segment_base(fpm);
	Size		pageno = fpm_pointer_to_page(base, btp);
	Size		index;
	FreePageBtree *check_parent;

	check_stack_depth();
	check_parent = relptr_access(base, btp->hdr.parent);
	appendStringInfo(buf, "  %zu@%d %c", pageno, level,
					 btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC ? 'i' : 'l');
	if (parent != check_parent)
		appendStringInfo(buf, " [actual parent %zu, expected %zu]",
						 fpm_pointer_to_page(base, check_parent),
						 fpm_pointer_to_page(base, parent));
	appendStringInfoChar(buf, ':');
	for (index = 0; index < btp->hdr.nused; ++index)
	{
		if (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
			appendStringInfo(buf, " %zu->%zu",
							 btp->u.internal_key[index].first_page,
							 relptr_offset(btp->u.internal_key[index].child) / FPM_PAGE_SIZE);
		else
			appendStringInfo(buf, " %zu(%zu)",
							 btp->u.leaf_key[index].first_page,
							 btp->u.leaf_key[index].npages);
	}
	appendStringInfoChar(buf, '\n');

	if (btp->hdr.magic == FREE_PAGE_INTERNAL_MAGIC)
	{
		for (index = 0; index < btp->hdr.nused; ++index)
		{
			FreePageBtree *child;

			child = relptr_access(base, btp->u.internal_key[index].child);
			FreePageManagerDumpBtree(fpm, child, btp, level + 1, buf);
		}
	}
}

/*
 * 调试转储跨度数据。
 */
static void
FreePageManagerDumpSpans(FreePageManager *fpm, FreePageSpanLeader *span,
						 Size expected_pages, StringInfo buf)
{
	char	   *base = fpm_segment_base(fpm);

	while (span != NULL)
	{
		if (span->npages != expected_pages)
			appendStringInfo(buf, " %zu(%zu)", fpm_pointer_to_page(base, span),
							 span->npages);
		else
			appendStringInfo(buf, " %zu", fpm_pointer_to_page(base, span));
		span = relptr_access(base, span->next);
	}

	appendStringInfoChar(buf, '\n');
}

/*
 * 本函数从空闲页管理器中分配给定长度的
 * 页跨度。
 */
static bool
FreePageManagerGetInternal(FreePageManager *fpm, Size npages, Size *first_page)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *victim = NULL;
	FreePageSpanLeader *prev;
	FreePageSpanLeader *next;
	FreePageBtreeSearchResult result;
	Size		victim_page = 0;	/* 安抚编译器 */
	Size		f;

	/*
	 * 搜索空闲跨度。
	 *
	 * 当前我们在此使用简单的最佳适配策略，但如果
	 * 反复被要求分配只比现有空间略小的区块，就可能
	 * 导致内存碎片化。希望这种情况不太可能发生，
	 * 因为我们预计大多数请求是单页或超块大小区块
	 * ——但除非策略能预知未来分配模式，
	 * 否则没有策略能处处最优。
	 *
	 */
	for (f = Min(npages, FPM_NUM_FREELISTS) - 1; f < FPM_NUM_FREELISTS; ++f)
	{
		/*跳过空空闲链表*/
		if (relptr_is_null(fpm->freelist[f]))
			continue;

		/*
		 * 除最后一条外，所有空闲链表都只含单一尺寸的
		 * 项，故直接取第一个。但最后的空闲
		 * 链表装着对其他链表而言都过大的所有跨度，因此
		 * 需搜索该链表。
		 */
		if (f < FPM_NUM_FREELISTS - 1)
			victim = relptr_access(base, fpm->freelist[f]);
		else
		{
			FreePageSpanLeader *candidate;

			candidate = relptr_access(base, fpm->freelist[f]);
			do
			{
				if (candidate->npages >= npages && (victim == NULL ||
													victim->npages > candidate->npages))
				{
					victim = candidate;
					if (victim->npages == npages)
						break;
				}
				candidate = relptr_access(base, candidate->next);
			} while (candidate != NULL);
		}
		break;
	}

	/* 若未找到可分配的跨度，则返回失败。 */
	if (victim == NULL)
		return false;

	/*从空闲链表移除跨度*/
	Assert(victim->magic == FREE_PAGE_SPAN_LEADER_MAGIC);
	prev = relptr_access(base, victim->prev);
	next = relptr_access(base, victim->next);
	if (prev != NULL)
		relptr_copy(prev->next, victim->next);
	else
		relptr_copy(fpm->freelist[f], victim->next);
	if (next != NULL)
		relptr_copy(next->prev, victim->prev);
	victim_page = fpm_pointer_to_page(base, victim);

	/* 判断是否可能正使 contiguous_pages 失效。 */
	if (f == FPM_NUM_FREELISTS - 1 &&
		victim->npages == fpm->contiguous_pages)
	{
		/*
		 * 受害跨度来自超大空闲链表，且与最长跨度同大
		 * 小。可能存在也可能不存在另一个同大的跨度，
		 * 因此保险起见必须重新计算 contiguous_pages。
		 *
		 */
		fpm->contiguous_pages_dirty = true;
	}
	else if (f + 1 == fpm->contiguous_pages &&
			 relptr_is_null(fpm->freelist[f]))
	{
		/*
		 * 受害跨度来自定长空闲链表，且该链表正是与当
		 * 前最长跨度同大的链表，且移除受害跨度后链表
		 * 已空。因此毫无疑问必须重新计算
		 * contiguous_pages。
		 */
		fpm->contiguous_pages_dirty = true;
	}

	/*
	 * 若我们尚未初始化 btree，则受害跨度必定是存储在
	 * FreePageManager 自身之内。否则我们需要
	 * 更新 btree。
	 */
	if (relptr_is_null(fpm->btree_root))
	{
		Assert(victim_page == fpm->singleton_first_page);
		Assert(victim->npages == fpm->singleton_npages);
		Assert(victim->npages >= npages);
		fpm->singleton_first_page += npages;
		fpm->singleton_npages -= npages;
		if (fpm->singleton_npages > 0)
			FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
								   fpm->singleton_npages);
	}
	else
	{
		/*
		 * 若找到的跨度尺寸恰好合适，则把它从 btree
		 * 全移除。否则，调整 btree 项以反映该跨度中尚未
		 * 分配的部分，并把该部分放到相应的空闲链表上。
		 *
		 */
		FreePageBtreeSearch(fpm, victim_page, &result);
		Assert(result.found);
		if (victim->npages == npages)
			FreePageBtreeRemove(fpm, result.page, result.index);
		else
		{
			FreePageBtreeLeafKey *key;

			/* 调整 btree 以反映剩余页。 */
			Assert(victim->npages > npages);
			key = &result.page->u.leaf_key[result.index];
			Assert(key->npages == victim->npages);
			key->first_page += npages;
			key->npages -= npages;
			if (result.index == 0)
				FreePageBtreeAdjustAncestorKeys(fpm, result.page);

			/* 把未分配的页放回相应的空闲链表。 */
			FreePagePushSpanLeader(fpm, victim_page + npages,
								   victim->npages - npages);
		}
	}

	/* 结果交还调用方。 */
	*first_page = fpm_pointer_to_page(base, victim);
	return true;
}

/*
 * 把一段页范围放入 btree 与空闲链表，并与紧邻其前
 * 和/或其后的既有空闲跨度合并。若 'soft' 为真，
 * 则只有在不分配新 btree 页也能完成插入时才执行
 * 插入；若为假，则总是执行。若 soft 标志导致插入
 * 被跳过，则返回 0；否则返回插入所创建的连续跨
 * 度的大小。若我们能与相邻范围合并，该值可能大
 * 于 npages。
 */
static Size
FreePageManagerPutInternal(FreePageManager *fpm, Size first_page, Size npages,
						   bool soft)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageBtreeSearchResult result;
	FreePageBtreeLeafKey *prevkey = NULL;
	FreePageBtreeLeafKey *nextkey = NULL;
	FreePageBtree *np;
	Size		nindex;

	Assert(npages > 0);

	/* 无需初始化 btree 即可存储单个空闲跨度。 */
	if (fpm->btree_depth == 0)
	{
		if (fpm->singleton_npages == 0)
		{
			/* 还没有跨度；存下这一个。 */
			fpm->singleton_first_page = first_page;
			fpm->singleton_npages = npages;
			FreePagePushSpanLeader(fpm, first_page, npages);
			return fpm->singleton_npages;
		}
		else if (fpm->singleton_first_page + fpm->singleton_npages ==
				 first_page)
		{
			/* 新跨度紧随唯一的既有跨度。 */
			fpm->singleton_npages += npages;
			FreePagePopSpanLeader(fpm, fpm->singleton_first_page);
			FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
								   fpm->singleton_npages);
			return fpm->singleton_npages;
		}
		else if (first_page + npages == fpm->singleton_first_page)
		{
			/* 新跨度紧邻唯一的既有跨度之前。 */
			FreePagePopSpanLeader(fpm, fpm->singleton_first_page);
			fpm->singleton_first_page = first_page;
			fpm->singleton_npages += npages;
			FreePagePushSpanLeader(fpm, fpm->singleton_first_page,
								   fpm->singleton_npages);
			return fpm->singleton_npages;
		}
		else
		{
			/* 不相邻；我们需要初始化 btree。 */
			Size		root_page;
			FreePageBtree *root;

			if (!relptr_is_null(fpm->btree_recycle))
				root = FreePageBtreeGetRecycled(fpm);
			else if (soft)
				return 0;		/* soft 时不应分配。 */
			else if (FreePageManagerGetInternal(fpm, 1, &root_page))
				root = (FreePageBtree *) fpm_page_to_pointer(base, root_page);
			else
			{
				/* 我们最好能从既有范围中取得一页。 */
				elog(FATAL, "free page manager btree is corrupt");
			}

			/* 创建 btree 并把既有范围移入其中。 */
			root->hdr.magic = FREE_PAGE_LEAF_MAGIC;
			root->hdr.nused = 1;
			relptr_store(base, root->hdr.parent, (FreePageBtree *) NULL);
			root->u.leaf_key[0].first_page = fpm->singleton_first_page;
			root->u.leaf_key[0].npages = fpm->singleton_npages;
			relptr_store(base, fpm->btree_root, root);
			fpm->singleton_first_page = 0;
			fpm->singleton_npages = 0;
			fpm->btree_depth = 1;

			/*
			 * 边界情形：btree 根可能拿走了最后一个空闲
			 * 页。那样，唯一的btree项会覆盖一个零页跨
			 * 度，此为无效。用我们正要插入的项覆写它，
			 * 然后退出。
			 */
			if (root->u.leaf_key[0].npages == 0)
			{
				root->u.leaf_key[0].first_page = first_page;
				root->u.leaf_key[0].npages = npages;
				FreePagePushSpanLeader(fpm, first_page, npages);
				return npages;
			}

			/* 落入下方以插入新键。 */
		}
	}

	/* 搜索 btree。 */
	FreePageBtreeSearch(fpm, first_page, &result);
	Assert(!result.found);
	if (result.index > 0)
		prevkey = &result.page->u.leaf_key[result.index - 1];
	if (result.index < result.page->hdr.nused)
	{
		np = result.page;
		nindex = result.index;
		nextkey = &result.page->u.leaf_key[result.index];
	}
	else
	{
		np = FreePageBtreeFindRightSibling(base, result.page);
		nindex = 0;
		if (np != NULL)
			nextkey = &np->u.leaf_key[0];
	}

	/* 若可能，与前一项合并。 */
	if (prevkey != NULL && prevkey->first_page + prevkey->npages >= first_page)
	{
		bool		remove_next = false;
		Size		result;

		Assert(prevkey->first_page + prevkey->npages == first_page);
		prevkey->npages = (first_page - prevkey->first_page) + npages;

		/* 检查我们是否*还*能与后一项合并。 */
		if (nextkey != NULL &&
			prevkey->first_page + prevkey->npages >= nextkey->first_page)
		{
			Assert(prevkey->first_page + prevkey->npages ==
				   nextkey->first_page);
			prevkey->npages = (nextkey->first_page - prevkey->first_page)
				+ nextkey->npages;
			FreePagePopSpanLeader(fpm, nextkey->first_page);
			remove_next = true;
		}

		/*把跨度放到正确空闲链表并记下大小。*/
		FreePagePopSpanLeader(fpm, prevkey->first_page);
		FreePagePushSpanLeader(fpm, prevkey->first_page, prevkey->npages);
		result = prevkey->npages;

		/*
		 * 若与前一项和后一项都合并了，则必须移除
		 * 后一项。这步最后做，因为从 btree 中移除元素
		 * 可能使我们持有的指向当前
		 * 数据结构的指针失效。
		 *
		 * 注意：严格说此时 btree 处于无效状态，因为
		 * 我们已经更新了 prevkey，使其覆盖与 nextkey 相同
		 * 的键空间。不过 FreePageBtreeRemove() 不应会注意到。
		 */
		if (remove_next)
			FreePageBtreeRemove(fpm, np, nindex);

		return result;
	}

	/* 若可能，与后一项合并。 */
	if (nextkey != NULL && first_page + npages >= nextkey->first_page)
	{
		Size		newpages;

		/* 计算跨度新大小。 */
		Assert(first_page + npages == nextkey->first_page);
		newpages = (nextkey->first_page - first_page) + nextkey->npages;

		/* 放上正确空闲链表。 */
		FreePagePopSpanLeader(fpm, nextkey->first_page);
		FreePagePushSpanLeader(fpm, first_page, newpages);

		/* 原地更新键。 */
		nextkey->first_page = first_page;
		nextkey->npages = newpages;

		/* 若减小页上的第一个键，祖先可能需要调整。 */
		if (nindex == 0)
			FreePageBtreeAdjustAncestorKeys(fpm, np);

		return nextkey->npages;
	}

	/* 分裂叶页及其必要的若干祖先。 */
	if (result.split_pages > 0)
	{
		/*
		 * 注意：可考虑多种应对策略来避免分裂；比如
		 * 最明显的是，若 np != result.page，我们可以转而
		 * 以该页为目标。更复杂的腾挪策略也可能有；
		 * 从根本上说，除非每个叶页都已 100% 满，否则只要
		 * 足够努力就能把这键硬塞进去。这么努力通常
		 * 不太可能值得，但我们可能需要
		 * 付出不止零努力。目前我们只做简单的事，
		 * 即什么都不做。
		 */

		/* 若这是 soft 插入，现在该放弃了。 */
		if (soft)
			return 0;

		/* 检查是否需要分配更多 btree 页以进行分裂。 */
		if (result.split_pages > fpm->btree_recycle_count)
		{
			Size		pages_needed;
			Size		recycle_page;
			Size		i;

			/*
			 * 分配所需页数并逐个分裂。
			 * 这绝不该失败：如果我们闲散的空闲页跨度
			 * 已多到需要额外存储空间来记住它们，
			 * 那我们肯定也有足够空间扩展 btree——
			 * 相比受管理页数，它本就该只占用极少页。
			 * 若真失败了，那就说明
			 * 有东西严重出错了。
			 */
			pages_needed = result.split_pages - fpm->btree_recycle_count;
			for (i = 0; i < pages_needed; ++i)
			{
				if (!FreePageManagerGetInternal(fpm, 1, &recycle_page))
					elog(FATAL, "free page manager btree is corrupt");
				FreePageBtreeRecycle(fpm, recycle_page);
			}

			/*
			 * 分配页用于回收的动作可能已使此前 btree 研究
			 * 的结果失效，故重做一遍。（我们可以复核
			 * 先前不可行的各避免分裂策略如今是否可行，
			 * 但似乎不值得，所以我们不费心。若先前
			 * 不可能合并，现在也
			 * 不可能。）
			 */
			FreePageBtreeSearch(fpm, first_page, &result);

			/*
			 * 为构建btree而分配页的动作绝不该使页更满，
			 * 所以新分裂深度不应大于旧深度；而若我们
			 * 幸运地分配到一个腾出了所需更新页
			 * 上槽位的区块，深度甚至会
			 * 变小。
			 */
			Assert(result.split_pages <= fpm->btree_recycle_count);
		}

		/* 若仍需执行分裂，则执行之。 */
		if (result.split_pages > 0)
		{
			FreePageBtree *split_target = result.page;
			FreePageBtree *child = NULL;
			Size		key = first_page;

			for (;;)
			{
				FreePageBtree *newsibling;
				FreePageBtree *parent;

				/* 确定必须接收下行链接的父页。 */
				parent = relptr_access(base, split_target->hdr.parent);

				/* 分裂该页，下行链接未加。 */
				newsibling = FreePageBtreeSplitPage(fpm, split_target);

				/*
				 * 循环至此，我们总是携带着一项待插入
				 * 内容。第一遍时它是实际要插入的键；
				 * 后续遍次中，它是上轮分裂后需添加的
				 * 下行链接。既然刚分裂了该页，
				 * 两个结果页之中必定有一个
				 * 还有空间。
				 *
				 */
				if (child == NULL)
				{
					Size		index;
					FreePageBtree *insert_into;

					insert_into = key < newsibling->u.leaf_key[0].first_page ?
						split_target : newsibling;
					index = FreePageBtreeSearchLeaf(insert_into, key);
					FreePageBtreeInsertLeaf(insert_into, index, key, npages);
					if (index == 0 && insert_into == split_target)
						FreePageBtreeAdjustAncestorKeys(fpm, split_target);
				}
				else
				{
					Size		index;
					FreePageBtree *insert_into;

					insert_into =
						key < newsibling->u.internal_key[0].first_page ?
						split_target : newsibling;
					index = FreePageBtreeSearchInternal(insert_into, key);
					FreePageBtreeInsertInternal(base, insert_into, index,
												key, child);
					relptr_store(base, child->hdr.parent, insert_into);
					if (index == 0 && insert_into == split_target)
						FreePageBtreeAdjustAncestorKeys(fpm, split_target);
				}

				/* 若刚分裂的页没有父页，则分裂根页。 */
				if (parent == NULL)
				{
					FreePageBtree *newroot;

					newroot = FreePageBtreeGetRecycled(fpm);
					newroot->hdr.magic = FREE_PAGE_INTERNAL_MAGIC;
					newroot->hdr.nused = 2;
					relptr_store(base, newroot->hdr.parent,
								 (FreePageBtree *) NULL);
					newroot->u.internal_key[0].first_page =
						FreePageBtreeFirstKey(split_target);
					relptr_store(base, newroot->u.internal_key[0].child,
								 split_target);
					relptr_store(base, split_target->hdr.parent, newroot);
					newroot->u.internal_key[1].first_page =
						FreePageBtreeFirstKey(newsibling);
					relptr_store(base, newroot->u.internal_key[1].child,
								 newsibling);
					relptr_store(base, newsibling->hdr.parent, newroot);
					relptr_store(base, fpm->btree_root, newroot);
					fpm->btree_depth++;

					break;
				}

				/* 若父页未满，则插入下行链接。 */
				key = newsibling->u.internal_key[0].first_page;
				if (parent->hdr.nused < FPM_ITEMS_PER_INTERNAL_PAGE)
				{
					Size		index;

					index = FreePageBtreeSearchInternal(parent, key);
					FreePageBtreeInsertInternal(base, parent, index,
												key, newsibling);
					relptr_store(base, newsibling->hdr.parent, parent);
					if (index == 0)
						FreePageBtreeAdjustAncestorKeys(fpm, parent);
					break;
				}

				/* 父页也需要分裂，故继续循环。 */
				child = newsibling;
				split_target = parent;
			}

			/*
			 * 上面的循环已完成插入，故只需更新空闲链表
			 * 就大功告成了。
			 */
			FreePagePushSpanLeader(fpm, first_page, npages);

			return npages;
		}
	}

	/* 把键物理加入页中。 */
	Assert(result.page->hdr.nused < FPM_ITEMS_PER_LEAF_PAGE);
	FreePageBtreeInsertLeaf(result.page, result.index, first_page, npages);

	/* 若为页上新的第一个键，祖先可能需调整。 */
	if (result.index == 0)
		FreePageBtreeAdjustAncestorKeys(fpm, result.page);

	/* 放上空闲链表。 */
	FreePagePushSpanLeader(fpm, first_page, npages);

	return npages;
}

/*
 * 从包含它的链表中移除 FreePageSpanLeader，原因可能是
 * 我们正在改变该跨度的大小，或者正在分配它。
 */
static void
FreePagePopSpanLeader(FreePageManager *fpm, Size pageno)
{
	char	   *base = fpm_segment_base(fpm);
	FreePageSpanLeader *span;
	FreePageSpanLeader *next;
	FreePageSpanLeader *prev;

	span = (FreePageSpanLeader *) fpm_page_to_pointer(base, pageno);

	next = relptr_access(base, span->next);
	prev = relptr_access(base, span->prev);
	if (next != NULL)
		relptr_copy(next->prev, span->prev);
	if (prev != NULL)
		relptr_copy(prev->next, span->next);
	else
	{
		Size		f = Min(span->npages, FPM_NUM_FREELISTS) - 1;

		Assert(relptr_offset(fpm->freelist[f]) == pageno * FPM_PAGE_SIZE);
		relptr_copy(fpm->freelist[f], span->next);
	}
}

/*
 * 初始化新 FreePageSpanLeader 并放上相应的空闲链表。
 */
static void
FreePagePushSpanLeader(FreePageManager *fpm, Size first_page, Size npages)
{
	char	   *base = fpm_segment_base(fpm);
	Size		f = Min(npages, FPM_NUM_FREELISTS) - 1;
	FreePageSpanLeader *head = relptr_access(base, fpm->freelist[f]);
	FreePageSpanLeader *span;

	span = (FreePageSpanLeader *) fpm_page_to_pointer(base, first_page);
	span->magic = FREE_PAGE_SPAN_LEADER_MAGIC;
	span->npages = npages;
	relptr_store(base, span->next, head);
	relptr_store(base, span->prev, (FreePageSpanLeader *) NULL);
	if (head != NULL)
		relptr_store(base, head->prev, span);
	relptr_store(base, fpm->freelist[f], span);
}
