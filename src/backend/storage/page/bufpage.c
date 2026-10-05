/*-------------------------------------------------------------------------
 *
 * bufpage.c
 *	  POSTGRES 标准缓冲页代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/bufpage.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "pgstat.h"
#include "storage/checksum.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


/* GUC 变量 */
bool		ignore_checksum_failure = false;


/* ----------------------------------------------------------------
 *						页支持函数
 * ----------------------------------------------------------------
 */

/*
 * PageInit
 *		初始化页的内容。
 *		注意这里不算初始校验和；那要等到
 *		写出去的时候才做。
 */
void
PageInit(Page page, Size pageSize, Size specialSize)
{
	PageHeader	p = (PageHeader) page;

	specialSize = MAXALIGN(specialSize);

	Assert(pageSize == BLCKSZ);
	Assert(pageSize > specialSize + SizeOfPageHeaderData);

	/* 确保页的所有字段以及未使用空间都为零 */
	MemSet(p, 0, pageSize);

	p->pd_flags = 0;
	p->pd_lower = SizeOfPageHeaderData;
	p->pd_upper = pageSize - specialSize;
	p->pd_special = pageSize - specialSize;
	PageSetPageSizeAndVersion(page, pageSize, PG_PAGE_LAYOUT_VERSION);
	/* p->pd_prune_xid = InvalidTransactionId;		由上面的 MemSet 完成 */
}


/*
 * PageIsVerifiedExtended
 *		检查页面头以及校验和（若有）看起来是否有效。
 *
 * 刚从磁盘读入一页时调用。用意是
 * 在顺着伪造的行指针、检查无效事务标识符等等而失控之前，
 * 廉价地发现损坏的页。
 *
 * 这里也必须允许全零页。尽管
 * 有意把一页加入关系时*不会*调用本例程，
 * 表里仍可能出现全零页。
 * （例如：后端扩展了关系，却在写下
 * 关于新页的任何 WAL 记录之前崩溃。内核已经把
 * 全零页放进文件，重启后它仍会那样。）因此我们
 * 在这里允许全零页，并小心让页访问宏
 * 把这样的页当作空页、且没有空闲空间。最终，VACUUM
 * 会清理这样的页并使其可用。
 *
 * 若设置了标志 PIV_LOG_WARNING，则在发生
 * 校验和失败时记一条 WARNING。
 *
 * 若设置了标志 PIV_REPORT_STAT，则把校验和失败直接
 * 报告给 pgstat。
 */
bool
PageIsVerifiedExtended(Page page, BlockNumber blkno, int flags)
{
	PageHeader	p = (PageHeader) page;
	size_t	   *pagebytes;
	int			i;
	bool		checksum_failure = false;
	bool		header_sane = false;
	bool		all_zeroes = false;
	uint16		checksum = 0;

	/*
	 * 除非该页通过基本的非零检查，否则不校验页数据
	 */
	if (!PageIsNew(page))
	{
		if (DataChecksumsEnabled())
		{
			checksum = pg_checksum_page((char *) page, blkno);

			if (checksum != p->pd_checksum)
				checksum_failure = true;
		}

		/*
		 * 下面的检查并不能证明页面头正确，只说明
		 * 它看起来足够正常，可以放进缓冲池。之后使用
		 * 该块时仍可能暴露问题，所以我们提供
		 * 校验和选项。
		 */
		if ((p->pd_flags & ~PD_VALID_FLAG_BITS) == 0 &&
			p->pd_lower <= p->pd_upper &&
			p->pd_upper <= p->pd_special &&
			p->pd_special <= BLCKSZ &&
			p->pd_special == MAXALIGN(p->pd_special))
			header_sane = true;

		if (header_sane && !checksum_failure)
			return true;
	}

	/* 检查全零情形 */
	all_zeroes = true;
	pagebytes = (size_t *) page;
	for (i = 0; i < (BLCKSZ / sizeof(size_t)); i++)
	{
		if (pagebytes[i] != 0)
		{
			all_zeroes = false;
			break;
		}
	}

	if (all_zeroes)
		return true;

	/*
	 * 若校验和失败就报 WARNING，但要等我们先检查过
	 * 全零的情形。
	 */
	if (checksum_failure)
	{
		if ((flags & PIV_LOG_WARNING) != 0)
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("page verification failed, calculated checksum %u but expected %u",
							checksum, p->pd_checksum)));

		if ((flags & PIV_REPORT_STAT) != 0)
			pgstat_report_checksum_failure();

		if (header_sane && ignore_checksum_failure)
			return true;
	}

	return false;
}


/*
 *	PageAddItemExtended
 *
 *	向页中添加一项。返回值是它被插入处的偏移，
 *	若因任何原因没有插入该项则为 InvalidOffsetNumber。
 *	会发出 WARNING，说明拒绝的原因。
 *
 *	offsetNumber 必须是 InvalidOffsetNumber（表示寻找一个
 *	空闲行指针），或者是 FirstOffsetNumber 到已有最后一项
 *	再往后一位之间的值，表示使用那个特定的行指针。
 *
 *	若 offsetNumber 有效且设置了标志 PAI_OVERWRITE，我们只是把
 *	该项存到指定的 offsetNumber，它必须是
 *	当前未使用的行指针，或者已有最后一项再往后一位。
 *
 *	若 offsetNumber 有效且未设置标志 PAI_OVERWRITE，则把
 *	该项插入指定的 offsetNumber，并把数组中更靠后的已有项
 *	挪开以腾出位置。
 *
 *	若 offsetNumber 无效，则分配一个槽：找到第一个
 *	既未使用又已释放的槽。
 *
 *	若设置了标志 PAI_IS_HEAP，我们强制页上的行指针不能多于
 *	MaxHeapTuplesPerPage 个。
 *
 *	!!! 此处禁止 EREPORT(ERROR)（DISALLOWED）!!!
 */
OffsetNumber
PageAddItemExtended(Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags)
{
	PageHeader	phdr = (PageHeader) page;
	Size		alignedSize;
	int			lower;
	int			upper;
	ItemId		itemId;
	OffsetNumber limit;
	bool		needshuffle = false;

	/*
	 * 当心损坏的页指针
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	/*
	 * 选择放置新项的 offsetNumber
	 */
	limit = OffsetNumberNext(PageGetMaxOffsetNumber(page));

	/* 是否传入了 offsetNumber？ */
	if (OffsetNumberIsValid(offsetNumber))
	{
		/* 是的，检查它 */
		if ((flags & PAI_OVERWRITE) != 0)
		{
			if (offsetNumber < limit)
			{
				itemId = PageGetItemId(page, offsetNumber);
				if (ItemIdIsUsed(itemId) || ItemIdHasStorage(itemId))
				{
					elog(WARNING, "will not overwrite a used ItemId");
					return InvalidOffsetNumber;
				}
			}
		}
		else
		{
			if (offsetNumber < limit)
				needshuffle = true; /* 需要移动已有的 linp */
		}
	}
	else
	{
		/* 没有传入 offsetNumber，因此找一个空闲槽 */
		/* 若没有空闲槽，就放到 limit（第一个空位） */
		if (PageHasFreeLinePointers(page))
		{
			/*
			 * 扫描行指针数组，定位一个 "可回收"（未使用）的
			 * ItemId。
			 *
			 * 总是优先使用更靠前的项。PageTruncateLinePointerArray
			 * 只能截掉连成一片、出现在行指针数组末尾的
			 * 未使用项。
			 */
			for (offsetNumber = FirstOffsetNumber;
				 offsetNumber < limit;	/* limit 为 maxoff+1 */
				 offsetNumber++)
			{
				itemId = PageGetItemId(page, offsetNumber);

				/*
				 * 我们也检查没有存储，只是为了多加防备；
				 * 未使用的项绝不应该有存储。同时 Assert() 该
				 * 不变量也成立。
				 */
				Assert(ItemIdIsUsed(itemId) || !ItemIdHasStorage(itemId));

				if (!ItemIdIsUsed(itemId) && !ItemIdHasStorage(itemId))
					break;
			}
			if (offsetNumber >= limit)
			{
				/* 提示是错的，因此重置它 */
				PageClearHasFreeLinePointers(page);
			}
		}
		else
		{
			/* 若提示说没有空闲槽，就不必搜索 */
			offsetNumber = limit;
		}
	}

	/* 拒绝把项放到第一个未使用行指针之后 */
	if (offsetNumber > limit)
	{
		elog(WARNING, "specified item offset is too large");
		return InvalidOffsetNumber;
	}

	/* 若是堆，拒绝把项放到堆边界之外 */
	if ((flags & PAI_IS_HEAP) != 0 && offsetNumber > MaxHeapTuplesPerPage)
	{
		elog(WARNING, "can't put more than MaxHeapTuplesPerPage items in a heap page");
		return InvalidOffsetNumber;
	}

	/*
	 * 计算页的新下界与上界指针，看是否放得下。
	 *
	 * 注意：用有符号整数做算术，以免例如
	 * alignedSize > pd_upper 时出错。
	 */
	if (offsetNumber == limit || needshuffle)
		lower = phdr->pd_lower + sizeof(ItemIdData);
	else
		lower = phdr->pd_lower;

	alignedSize = MAXALIGN(size);

	upper = (int) phdr->pd_upper - (int) alignedSize;

	if (lower > upper)
		return InvalidOffsetNumber;

	/*
	 * 可以插入该项了。首先，必要时重排已有指针。
	 */
	itemId = PageGetItemId(page, offsetNumber);

	if (needshuffle)
		memmove(itemId + 1, itemId,
				(limit - offsetNumber) * sizeof(ItemIdData));

	/* 设置行指针 */
	ItemIdSetNormal(itemId, upper, size);

	/*
	 * 项通常不含未初始化字节。核心 bufpage 的使用者
	 * 都遵守这一点，但这不是必须的编码规则；新的索引访问方法可以
	 * 选择不遵守。不过，数据类型输入函数以及其他
	 * 合成 datum 的 C 语言函数应当初始化全部
	 * 字节；datumIsEqual() 依赖这一点。在这里测试，再加上
	 * printtup() 里的类似检查，有助于抓住这类错误。
	 *
	 * 经仅索引扫描取出的 "name" 类型值可能含有
	 * 未初始化字节；见 btrescan() 里的注释。Valgrind 会把这
	 * 报告为错误，但忽略它是安全的。
	 */
	VALGRIND_CHECK_MEM_IS_DEFINED(item, size);

	/* 把该项的数据复制到页上 */
	memcpy((char *) page + upper, item, size);

	/* 调整页面头 */
	phdr->pd_lower = (LocationIndex) lower;
	phdr->pd_upper = (LocationIndex) upper;

	return offsetNumber;
}


/*
 * PageGetTempPage
 *		在本地内存中取得临时页，供特殊处理使用。
 *		返回的页完全未初始化；调用者必须自己做。
 */
Page
PageGetTempPage(Page page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	return temp;
}

/*
 * PageGetTempPageCopy
 *		在本地内存中取得临时页，供特殊处理使用。
 *		该页通过复制给定页的内容来初始化。
 */
Page
PageGetTempPageCopy(Page page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	memcpy(temp, page, pageSize);

	return temp;
}

/*
 * PageGetTempPageCopySpecial
 *		在本地内存中取得临时页，供特殊处理使用。
 *		该页用与给定页相同的特殊空间大小做 PageInit，
 *		并从给定页复制特殊空间。
 */
Page
PageGetTempPageCopySpecial(Page page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	PageInit(temp, pageSize, PageGetSpecialSize(page));
	memcpy(PageGetSpecialPointer(temp),
		   PageGetSpecialPointer(page),
		   PageGetSpecialSize(page));

	return temp;
}

/*
 * PageRestoreTempPage
 *		特殊处理之后把临时页复制回永久页，
 *		并释放临时页。
 */
void
PageRestoreTempPage(Page tempPage, Page oldPage)
{
	Size		pageSize;

	pageSize = PageGetPageSize(tempPage);
	memcpy((char *) oldPage, (char *) tempPage, pageSize);

	pfree(tempPage);
}

/*
 * 供 PageRepairFragmentation 与 PageIndexMultiDelete 使用的元组碎片整理支持
 */
typedef struct itemIdCompactData
{
	uint16		offsetindex;	/* linp 数组下标 */
	int16		itemoff;		/* 项数据在页内的偏移 */
	uint16		alignedlen;		/* MAXALIGN（项数据长度） */
} itemIdCompactData;
typedef itemIdCompactData *itemIdCompact;

/*
 * 在移除或把一些行指针标成未使用之后，移动元组以
 * 消除被移除项造成的空隙，并把它们重新排成
 * 行指针逆序。
 *
 * 本函数常常相当热，因此值得采取措施
 * 让它尽可能高效。
 *
 * 若 'itemidbase' 数组按 itemoff 降序排列，调用者可把 'presorted'
 * 传为 true。这时我们可以直接 memmove()
 * 元组到页的末尾。这相当常见，因为这正是
 * 元组最初插入页时的顺序。当我们调用本
 * 函数对页内元组做碎片整理时，新加入页的行指针
 * 会保持这种预排序，因此命中这一情形
 * 对经常更新的表仍然非常常见。
 *
 * 当 'itemidbase' 数组并非预排序时，我们无法随意
 * memmove() 元组。那样做可能覆盖
 * 尚未移动的元组所占内存。这时，我们把所有
 * 需要移动的元组复制到临时缓冲区。然后可以
 * 直接从该临时缓冲区 memcpy() 回页内的正确
 * 位置。元组按与 'itemidbase' 数组相同的顺序
 * 复制回页，于是元组被重新排成行指针逆序。
 * 这会提高下次命中
 * 预排序情形的机会。
 *
 * 调用者必须保证 nitems > 0
 */
static void
compactify_tuples(itemIdCompact itemidbase, int nitems, Page page, bool presorted)
{
	PageHeader	phdr = (PageHeader) page;
	Offset		upper;
	Offset		copy_tail;
	Offset		copy_head;
	itemIdCompact itemidptr;
	int			i;

	/* 若 nitems == 0，其中的代码无法正确工作 */
	Assert(nitems > 0);

	if (presorted)
	{

#ifdef USE_ASSERT_CHECKING
		{
			/*
			 * 核对没有新的调用者错误地
			 * 传入值为 true 的 presorted。
			 */
			Offset		lastoff = phdr->pd_special;

			for (i = 0; i < nitems; i++)
			{
				itemidptr = &itemidbase[i];

				Assert(lastoff > itemidptr->itemoff);

				lastoff = itemidptr->itemoff;
			}
		}
#endif							/* USE_ASSERT_CHECKING（断言检查） */

		/*
		 * 'itemidbase' 已经是最优顺序，也就是较低的项
		 * 指针具有较高的偏移。这样我们就可以 memmove()
		 * 元组直到页的末尾，而不必担心
		 * 覆盖尚未移动的其他元组。
		 *
		 * 很有可能已经有元组就在页的
		 * 末尾，因为它们已经处在页内的正确位置，
		 * 我们可以直接跳过。我们先
		 * 做这一步……
		 */
		upper = phdr->pd_special;
		i = 0;
		do
		{
			itemidptr = &itemidbase[i];
			if (upper != itemidptr->itemoff + itemidptr->alignedlen)
				break;
			upper -= itemidptr->alignedlen;

			i++;
		} while (i < nitems);

		/*
		 * 既然已经找到第一个需要移动的元组，就可以
		 * 做元组压实。我们尽量减少
		 * memmove() 调用次数，只在出现空隙时才调用 memmove()。当
		 * 看到空隙时，就把空隙之后的所有元组往上移，直到
		 * 上一次移动操作的位置。
		 */
		copy_tail = copy_head = itemidptr->itemoff + itemidptr->alignedlen;
		for (; i < nitems; i++)
		{
			ItemId		lp;

			itemidptr = &itemidbase[i];
			lp = PageGetItemId(page, itemidptr->offsetindex + 1);

			if (copy_head != itemidptr->itemoff + itemidptr->alignedlen)
			{
				memmove((char *) page + upper,
						page + copy_head,
						copy_tail - copy_head);

				/*
				 * 我们已经移动了此前见过的所有元组，但不包括
				 * 当前元组，因此把 copy_tail 设为这个
				 * 元组的末尾，以便在循环的下一轮移动它。
				 */
				copy_tail = itemidptr->itemoff + itemidptr->alignedlen;
			}
			/* 把目标偏移按这个元组的长度下移 */
			upper -= itemidptr->alignedlen;
			/* 让 copy_head 指向这个元组的起点 */
			copy_head = itemidptr->itemoff;

			/* 更新行指针，使它引用新偏移 */
			lp->lp_off = upper;
		}

		/* 移动剩余的元组。 */
		memmove((char *) page + upper,
				page + copy_head,
				copy_tail - copy_head);
	}
	else
	{
		PGAlignedBlock scratch;
		char	   *scratchptr = scratch.data;

		/*
		 * 非预排序情形：itemidbase 数组中的元组可能是
		 * 任意顺序。因此，要把它们移到页末尾，
		 * 必须先为每个需要移动的元组做一份临时副本，然后
		 * 再按新偏移把它们复制回页内。
		 *
		 * 若很大比例的元组已被剪枝（>75%），我们就
		 * 逐个元组复制到临时缓冲区，否则就
		 * 对所有需要移动的元组做一次 memcpy()。
		 * 移除了这么多元组时，很可能有很多
		 * 空隙，页末尾也不太可能还留着很多不可移动的
		 * 元组。
		 */
		if (nitems < PageGetMaxOffsetNumber(page) / 4)
		{
			i = 0;
			do
			{
				itemidptr = &itemidbase[i];
				memcpy(scratchptr + itemidptr->itemoff, page + itemidptr->itemoff,
					   itemidptr->alignedlen);
				i++;
			} while (i < nitems);

			/* 为下面的压实代码做好准备 */
			i = 0;
			itemidptr = &itemidbase[0];
			upper = phdr->pd_special;
		}
		else
		{
			upper = phdr->pd_special;

			/*
			 * 很多元组很可能已经在正确位置。
			 * 不必把它们复制进临时缓冲区。我们改为
			 * 在 itemidbase 数组里向前跳到
			 * 确实需要从那里移动元组的位置，使下面的代码
			 * 不去动这些元组。
			 */
			i = 0;
			do
			{
				itemidptr = &itemidbase[i];
				if (upper != itemidptr->itemoff + itemidptr->alignedlen)
					break;
				upper -= itemidptr->alignedlen;

				i++;
			} while (i < nitems);

			/* 把所有需要移动的元组复制到临时缓冲区 */
			memcpy(scratchptr + phdr->pd_upper,
				   page + phdr->pd_upper,
				   upper - phdr->pd_upper);
		}

		/*
		 * 做元组压实。itemidptr 已经指向
		 * 我们将要移动的第一个元组。这里把相邻元组的
		 * 多次 memcpy 调用合并成一次。做法是
		 * 推迟 memcpy 调用，直到发现需要闭合的
		 * 空隙。
		 */
		copy_tail = copy_head = itemidptr->itemoff + itemidptr->alignedlen;
		for (; i < nitems; i++)
		{
			ItemId		lp;

			itemidptr = &itemidbase[i];
			lp = PageGetItemId(page, itemidptr->offsetindex + 1);

			/* 发现空隙时复制尚未写出的元组 */
			if (copy_head != itemidptr->itemoff + itemidptr->alignedlen)
			{
				memcpy((char *) page + upper,
					   scratchptr + copy_head,
					   copy_tail - copy_head);

				/*
				 * 我们已经复制了此前见过的所有元组，但不包括
				 * 当前元组，因此把 copy_tail 设为这个
				 * 元组的末尾。
				 */
				copy_tail = itemidptr->itemoff + itemidptr->alignedlen;
			}
			/* 把目标偏移按这个元组的长度下移 */
			upper -= itemidptr->alignedlen;
			/* 让 copy_head 指向这个元组的起点 */
			copy_head = itemidptr->itemoff;

			/* 更新行指针，使它引用新偏移 */
			lp->lp_off = upper;
		}

		/* 复制剩余的那一段 */
		memcpy((char *) page + upper,
			   scratchptr + copy_head,
			   copy_tail - copy_head);
	}

	phdr->pd_upper = upper;
}

/*
 * PageRepairFragmentation
 *
 * 剪枝之后释放堆页上的碎片空间。
 *
 * 本例程只适用于堆页，另见 PageIndexMultiDelete。
 *
 * 本例程从行指针数组末尾移除未使用的行指针。
 * 当剪枝移除死的堆内元组时可以这样做，
 * 尤其是原先存在每条含有多个元组的 HOT 链时。
 *
 * 调用者最好持有该页缓冲区的完全清理锁。作为副作用，
 * 页的 PD_HAS_FREE_LINES 提示位会按需要
 * 置位或清除。调用者也许还要考虑行指针数组
 * 在数组截断之后长度变短。
 */
void
PageRepairFragmentation(Page page)
{
	Offset		pd_lower = ((PageHeader) page)->pd_lower;
	Offset		pd_upper = ((PageHeader) page)->pd_upper;
	Offset		pd_special = ((PageHeader) page)->pd_special;
	Offset		last_offset;
	itemIdCompactData itemidbase[MaxHeapTuplesPerPage];
	itemIdCompact itemidptr;
	ItemId		lp;
	int			nline,
				nstorage,
				nunused;
	OffsetNumber finalusedlp = InvalidOffsetNumber;
	int			i;
	Size		totallen;
	bool		presorted = true;	/* 目前 */

	/*
	 * 这里比大多数地方更值得多加防备，
	 * 因为我们即将重排（通常是）共享
	 * 磁盘缓冲区里的数据。若不小心，损坏的指针、长度
	 * 等就会让我们破坏相邻的磁盘缓冲区，使数据
	 * 损失扩散。所以要检查每一项。
	 */
	if (pd_lower < SizeOfPageHeaderData ||
		pd_lower > pd_upper ||
		pd_upper > pd_special ||
		pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	/*
	 * 遍历行指针数组，收集关于活动项的数据。
	 */
	nline = PageGetMaxOffsetNumber(page);
	itemidptr = itemidbase;
	nunused = totallen = 0;
	last_offset = pd_special;
	for (i = FirstOffsetNumber; i <= nline; i++)
	{
		lp = PageGetItemId(page, i);
		if (ItemIdIsUsed(lp))
		{
			if (ItemIdHasStorage(lp))
			{
				itemidptr->offsetindex = i - 1;
				itemidptr->itemoff = ItemIdGetOffset(lp);

				if (last_offset > itemidptr->itemoff)
					last_offset = itemidptr->itemoff;
				else
					presorted = false;

				if (unlikely(itemidptr->itemoff < (int) pd_upper ||
							 itemidptr->itemoff >= (int) pd_special))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted line pointer: %u",
									itemidptr->itemoff)));
				itemidptr->alignedlen = MAXALIGN(ItemIdGetLength(lp));
				totallen += itemidptr->alignedlen;
				itemidptr++;
			}

			finalusedlp = i;	/* 可能是最后一个非 LP_UNUSED 项 */
		}
		else
		{
			/* 未使用项的 lp_len 应为 0，但还是确认一下 */
			Assert(!ItemIdHasStorage(lp));
			ItemIdSetUnused(lp);
			nunused++;
		}
	}

	nstorage = itemidptr - itemidbase;
	if (nstorage == 0)
	{
		/* 页完全是空的，因此快速重置它 */
		((PageHeader) page)->pd_upper = pd_special;
	}
	else
	{
		/* 需要用较费事的办法压实该页 */
		if (totallen > (Size) (pd_special - pd_lower))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted item lengths: total %u, available space %u",
							(unsigned int) totallen, pd_special - pd_lower)));

		compactify_tuples(itemidbase, nstorage, page, presorted);
	}

	if (finalusedlp != nline)
	{
		/* 最后一个行指针并不是最后一个已使用的行指针 */
		int			nunusedend = nline - finalusedlp;

		Assert(nunused >= nunusedend && nunusedend > 0);

		/* 从计数中去掉末尾未使用的行指针 */
		nunused -= nunusedend;
		/* 截断行指针数组 */
		((PageHeader) page)->pd_lower -= (sizeof(ItemIdData) * nunusedend);
	}

	/* 为 PageAddItemExtended 设置提示位 */
	if (nunused > 0)
		PageSetHasFreeLinePointers(page);
	else
		PageClearHasFreeLinePointers(page);
}

/*
 * PageTruncateLinePointerArray
 *
 * 移除行指针数组末尾未使用的行指针。
 *
 * 本例程只适用于堆页。VACUUM 在
 * 对堆的第二遍扫描中调用它。我们预期页上至少有一个 LP_UNUSED
 * 行指针（若 VACUUM 并未把该页上的 LP_DEAD 项
 * 刚刚设成 LP_UNUSED，就不该调用这里）。
 *
 * 我们避免把行指针数组截断到 0 项，必要时
 * 留下单独一个 LP_UNUSED 项。这有点
 * 随意，但避免留下 PageIsEmpty()
 * 的页似乎是个好主意。
 *
 * 调用者可以持有该页缓冲区的排他锁或完全清理锁。
 * 页的 PD_HAS_FREE_LINES 提示位会根据
 * 我们是否留下任何剩余的 LP_UNUSED 项来置位或清除。
 */
void
PageTruncateLinePointerArray(Page page)
{
	PageHeader	phdr = (PageHeader) page;
	bool		countdone = false,
				sethint = false;
	int			nunusedend = 0;

	/* 从后向前扫描行指针数组 */
	for (int i = PageGetMaxOffsetNumber(page); i >= FirstOffsetNumber; i--)
	{
		ItemId		lp = PageGetItemId(page, i);

		if (!countdone && i > FirstOffsetNumber)
		{
			/*
			 * 仍在确定数组末尾哪些行指针
			 * 将被截掉。要么再把一个行指针计为
			 * 可以安全截掉，要么发现再截
			 * 更多行指针并不安全（停止计数）。
			 */
			if (!ItemIdIsUsed(lp))
				nunusedend++;
			else
				countdone = true;
		}
		else
		{
			/*
			 * 一旦停止计数，我们仍要弄清
			 * 是否还有剩余的 LP_UNUSED 行指针，位于数组
			 * 更靠前的某处。
			 */
			if (!ItemIdIsUsed(lp))
			{
				/*
				 * 这是一个我们不会截掉的未使用行指针
				 * ——所以至少有一个。在页上设置提示。
				 */
				sethint = true;
				break;
			}
		}
	}

	if (nunusedend > 0)
	{
		phdr->pd_lower -= sizeof(ItemIdData) * nunusedend;

#ifdef CLOBBER_FREED_MEMORY
		memset((char *) page + phdr->pd_lower, 0x7F,
			   sizeof(ItemIdData) * nunusedend);
#endif
	}
	else
		Assert(sethint);

	/* 为 PageAddItemExtended 设置提示位 */
	if (sethint)
		PageSetHasFreeLinePointers(page);
	else
		PageClearHasFreeLinePointers(page);
}

/*
 * PageGetFreeSpace
 *		返回页上可分配空闲空间的大小，
 *		并减去一个新行指针所需的空间。
 *
 * 注意：这通常只应用于索引页。堆页请用
 * PageGetHeapFreeSpace。
 */
Size
PageGetFreeSpace(Page page)
{
	int			space;

	/*
	 * 这里用有符号算术，以便在 pd_lower >
	 * pd_upper 时行为仍然合理。
	 */
	space = (int) ((PageHeader) page)->pd_upper -
		(int) ((PageHeader) page)->pd_lower;

	if (space < (int) sizeof(ItemIdData))
		return 0;
	space -= sizeof(ItemIdData);

	return (Size) space;
}

/*
 * PageGetFreeSpaceForMultipleTuples
 *		返回页上可分配空闲空间的大小，
 *		并减去多个新行指针所需的空间。
 *
 * 注意：这通常只应用于索引页。堆页请用
 * PageGetHeapFreeSpace。
 */
Size
PageGetFreeSpaceForMultipleTuples(Page page, int ntups)
{
	int			space;

	/*
	 * 这里用有符号算术，以便在 pd_lower >
	 * pd_upper 时行为仍然合理。
	 */
	space = (int) ((PageHeader) page)->pd_upper -
		(int) ((PageHeader) page)->pd_lower;

	if (space < (int) (ntups * sizeof(ItemIdData)))
		return 0;
	space -= ntups * sizeof(ItemIdData);

	return (Size) space;
}

/*
 * PageGetExactFreeSpace
 *		返回页上可分配空闲空间的大小，
 *		不考虑添加或移除行指针。
 */
Size
PageGetExactFreeSpace(Page page)
{
	int			space;

	/*
	 * 这里用有符号算术，以便在 pd_lower >
	 * pd_upper 时行为仍然合理。
	 */
	space = (int) ((PageHeader) page)->pd_upper -
		(int) ((PageHeader) page)->pd_lower;

	if (space < 0)
		return 0;

	return (Size) space;
}


/*
 * PageGetHeapFreeSpace
 *		返回页上可分配空闲空间的大小，
 *		并减去一个新行指针所需的空间。
 *
 * 它与 PageGetFreeSpace 的差别是：若页上已有
 * MaxHeapTuplesPerPage 个行指针且没有空闲的，这里会返回
 * 零。我们用它来强制堆页上创建的行指针不超过
 * MaxHeapTuplesPerPage 个。（尽管
 * 反正也放不下更多元组，但若存在重定向
 * 或死亡行指针，行指针仍可能过多。
 * 为避免破坏把 MaxHeapTuplesPerPage 当作行指针个数硬上限的代码，
 * 我们做这项额外检查。）
 */
Size
PageGetHeapFreeSpace(Page page)
{
	Size		space;

	space = PageGetFreeSpace(page);
	if (space > 0)
	{
		OffsetNumber offnum,
					nline;

		/*
		 * 页上是否已经有 MaxHeapTuplesPerPage 个行指针？
		 */
		nline = PageGetMaxOffsetNumber(page);
		if (nline >= MaxHeapTuplesPerPage)
		{
			if (PageHasFreeLinePointers(page))
			{
				/*
				 * 因为这只是提示，我们必须确认确实
				 * 有一个空闲行指针
				 */
				for (offnum = FirstOffsetNumber; offnum <= nline; offnum = OffsetNumberNext(offnum))
				{
					ItemId		lp = PageGetItemId(page, offnum);

					if (!ItemIdIsUsed(lp))
						break;
				}

				if (offnum > nline)
				{
					/*
					 * 提示是错的，但这里无法清除它，因为我们
					 * 没有能力把该页标为脏页。
					 */
					space = 0;
				}
			}
			else
			{
				/*
				 * 尽管提示可能是错的，PageAddItem 仍会相信
				 * 它，所以我们也必须相信它。
				 */
				space = 0;
			}
		}
	}
	return space;
}


/*
 * PageIndexTupleDelete
 *
 * 本例程负责从索引页移除一个元组。
 *
 * 与堆页不同，我们把被移除元组的行指针压实掉。
 */
void
PageIndexTupleDelete(Page page, OffsetNumber offnum)
{
	PageHeader	phdr = (PageHeader) page;
	char	   *addr;
	ItemId		tup;
	Size		size;
	unsigned	offset;
	int			nbytes;
	int			offidx;
	int			nline;

	/*
	 * 与 PageRepairFragmentation 一样，这里多加防备是值得的。
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ ||
		phdr->pd_special != MAXALIGN(phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	nline = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > nline)
		elog(ERROR, "invalid index offnum: %u", offnum);

	/* 把偏移号改成偏移下标 */
	offidx = offnum - 1;

	tup = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tup));
	size = ItemIdGetLength(tup);
	offset = ItemIdGetOffset(tup);

	if (offset < phdr->pd_upper || (offset + size) > phdr->pd_special ||
		offset != MAXALIGN(offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted line pointer: offset = %u, size = %u",
						offset, (unsigned int) size)));

	/* 实际要删除的空间量 */
	size = MAXALIGN(size);

	/*
	 * 首先，我们要去掉该索引元组的 pd_linp 项。我们
	 * 把后续所有 linp 在数组里回填一个槽。我们不用
	 * PageGetItemId，因为我们操作的是 _array_，不是单个
	 * linp。
	 */
	nbytes = phdr->pd_lower -
		((char *) &phdr->pd_linp[offidx + 1] - (char *) phdr);

	if (nbytes > 0)
		memmove((char *) &(phdr->pd_linp[offidx]),
				(char *) &(phdr->pd_linp[offidx + 1]),
				nbytes);

	/*
	 * 现在把旧的上界（元组空间的起点）
	 * 与被删元组起点之间的所有内容前移，使页
	 * 中间空出空间。若我们刚删的元组
	 * 就在元组空间的起点，就不必做这次复制。
	 */

	/* 元组空间的起点 */
	addr = (char *) page + phdr->pd_upper;

	if (offset > phdr->pd_upper)
		memmove(addr + size, addr, offset - phdr->pd_upper);

	/* 调整空闲空间边界指针 */
	phdr->pd_upper += size;
	phdr->pd_lower -= sizeof(ItemIdData);

	/*
	 * 最后，需要调整剩余的 linp 项。
	 *
	 * 原先位于被删元组数据之前的内容都已
	 * 按被删元组的大小前移。
	 */
	if (!PageIsEmpty(page))
	{
		int			i;

		nline--;				/* 比开始时少了一个 */
		for (i = 1; i <= nline; i++)
		{
			ItemId		ii = PageGetItemId(page, i);

			Assert(ItemIdHasStorage(ii));
			if (ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size;
		}
	}
}


/*
 * PageIndexMultiDelete
 *
 * 本例程处理一次从索引页删除多个元组的情形。
 * 它比绕 PageIndexTupleDelete 循环一圈快得多……
 * 不过，调用者*必须*按项号顺序提供要删除的
 * 项号数组！
 */
void
PageIndexMultiDelete(Page page, OffsetNumber *itemnos, int nitems)
{
	PageHeader	phdr = (PageHeader) page;
	Offset		pd_lower = phdr->pd_lower;
	Offset		pd_upper = phdr->pd_upper;
	Offset		pd_special = phdr->pd_special;
	Offset		last_offset;
	itemIdCompactData itemidbase[MaxIndexTuplesPerPage];
	ItemIdData	newitemids[MaxIndexTuplesPerPage];
	itemIdCompact itemidptr;
	ItemId		lp;
	int			nline,
				nused;
	Size		totallen;
	Size		size;
	unsigned	offset;
	int			nextitm;
	OffsetNumber offnum;
	bool		presorted = true;	/* 目前 */

	Assert(nitems <= MaxIndexTuplesPerPage);

	/*
	 * 若要删除的项不多，逐个调用是最好的办法。
	 * PageIndexTupleDelete 最合适。按相反顺序删除各项，
	 * 这样就不必考虑为先前的删除
	 * 调整项号。
	 *
	 * TODO：在这里调这个魔数
	 */
	if (nitems <= 2)
	{
		while (--nitems >= 0)
			PageIndexTupleDelete(page, itemnos[nitems]);
		return;
	}

	/*
	 * 与 PageRepairFragmentation 一样，这里多加防备是值得的。
	 */
	if (pd_lower < SizeOfPageHeaderData ||
		pd_lower > pd_upper ||
		pd_upper > pd_special ||
		pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	/*
	 * 扫描行指针数组，只列出我们
	 * 要保留的那些。注意此时还不修改页，因为我们
	 * 仍在做有效性检查。
	 */
	nline = PageGetMaxOffsetNumber(page);
	itemidptr = itemidbase;
	totallen = 0;
	nused = 0;
	nextitm = 0;
	last_offset = pd_special;
	for (offnum = FirstOffsetNumber; offnum <= nline; offnum = OffsetNumberNext(offnum))
	{
		lp = PageGetItemId(page, offnum);
		Assert(ItemIdHasStorage(lp));
		size = ItemIdGetLength(lp);
		offset = ItemIdGetOffset(lp);
		if (offset < pd_upper ||
			(offset + size) > pd_special ||
			offset != MAXALIGN(offset))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted line pointer: offset = %u, size = %u",
							offset, (unsigned int) size)));

		if (nextitm < nitems && offnum == itemnos[nextitm])
		{
			/* 跳过要删除的项 */
			nextitm++;
		}
		else
		{
			itemidptr->offsetindex = nused; /* 它将要去的位置 */
			itemidptr->itemoff = offset;

			if (last_offset > itemidptr->itemoff)
				last_offset = itemidptr->itemoff;
			else
				presorted = false;

			itemidptr->alignedlen = MAXALIGN(size);
			totallen += itemidptr->alignedlen;
			newitemids[nused] = *lp;
			itemidptr++;
			nused++;
		}
	}

	/* 这会抓住无效或乱序的 itemnos[] */
	if (nextitm != nitems)
		elog(ERROR, "incorrect index offsets supplied");

	if (totallen > (Size) (pd_special - pd_lower))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted item lengths: total %u, available space %u",
						(unsigned int) totallen, pd_special - pd_lower)));

	/*
	 * 看起来没问题。用这份副本覆盖行指针，其中已经
	 * 去掉了所有未使用的项。
	 */
	memcpy(phdr->pd_linp, newitemids, nused * sizeof(ItemIdData));
	phdr->pd_lower = SizeOfPageHeaderData + nused * sizeof(ItemIdData);

	/* 并压实元组数据 */
	if (nused > 0)
		compactify_tuples(itemidbase, nused, page, presorted);
	else
		phdr->pd_upper = pd_special;
}


/*
 * PageIndexTupleDeleteNoCompact
 *
 * 从索引页移除指定元组，但把它的行指针
 * 设为 "unused"，而不是压实掉；例外是若它是
 * 页上最后一个行指针，则可以移除。
 *
 * 用于这样的索引访问方法：需要现存活动元组的 TIDs
 * 保持不变，并愿意改而允许未使用的行指针。
 */
void
PageIndexTupleDeleteNoCompact(Page page, OffsetNumber offnum)
{
	PageHeader	phdr = (PageHeader) page;
	char	   *addr;
	ItemId		tup;
	Size		size;
	unsigned	offset;
	int			nline;

	/*
	 * 与 PageRepairFragmentation 一样，这里多加防备是值得的。
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ ||
		phdr->pd_special != MAXALIGN(phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	nline = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > nline)
		elog(ERROR, "invalid index offnum: %u", offnum);

	tup = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tup));
	size = ItemIdGetLength(tup);
	offset = ItemIdGetOffset(tup);

	if (offset < phdr->pd_upper || (offset + size) > phdr->pd_special ||
		offset != MAXALIGN(offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted line pointer: offset = %u, size = %u",
						offset, (unsigned int) size)));

	/* 实际要删除的空间量 */
	size = MAXALIGN(size);

	/*
	 * 要么把行指针设为 "unused"，要么若它是最后一个就清掉
	 * 它。（注意：紧邻末尾之前的项可能已经
	 * 是未使用的，但即便如此我们也不费力去把它们压实掉。）
	 */
	if ((int) offnum < nline)
		ItemIdSetUnused(tup);
	else
	{
		phdr->pd_lower -= sizeof(ItemIdData);
		nline--;				/* 比开始时少了一个 */
	}

	/*
	 * 现在把旧的上界（元组空间的起点）
	 * 与被删元组起点之间的所有内容前移，使页
	 * 中间空出空间。若我们刚删的元组
	 * 就在元组空间的起点，就不必做这次复制。
	 */

	/* 元组空间的起点 */
	addr = (char *) page + phdr->pd_upper;

	if (offset > phdr->pd_upper)
		memmove(addr + size, addr, offset - phdr->pd_upper);

	/* 调整空闲空间边界指针 */
	phdr->pd_upper += size;

	/*
	 * 最后，需要调整剩余的 linp 项。
	 *
	 * 原先位于被删元组数据之前的内容都已
	 * 按被删元组的大小前移。
	 */
	if (!PageIsEmpty(page))
	{
		int			i;

		for (i = 1; i <= nline; i++)
		{
			ItemId		ii = PageGetItemId(page, i);

			if (ItemIdHasStorage(ii) && ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size;
		}
	}
}


/*
 * PageIndexTupleOverwrite
 *
 * 替换索引页上的指定元组。
 *
 * 新元组放在旧元组原来的位置，并按需要上移或下移
 * 其他元组的数据，以保持页是压实的。
 * 这比删除再重新插入该元组更好，因为它
 * 在元组大小不变时避免任何数据移动；而且
 * 即使大小变了，我们也不挪动行指针。
 * 不想在 LP_DEAD 位碰巧已置位时清除它的索引访问方法可以用它。
 * 原则上，也关心元组物理顺序
 * 及其逻辑/ItemId 顺序的索引访问方法，
 * 同样可以用它。
 *
 * 若新元组空间不足，返回 false。其他
 * 错误表示数据损坏问题，所以我们直接 elog。
 */
bool
PageIndexTupleOverwrite(Page page, OffsetNumber offnum,
						Item newtup, Size newsize)
{
	PageHeader	phdr = (PageHeader) page;
	ItemId		tupid;
	int			oldsize;
	unsigned	offset;
	Size		alignednewsize;
	int			size_diff;
	int			itemcount;

	/*
	 * 与 PageRepairFragmentation 一样，这里多加防备是值得的。
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ ||
		phdr->pd_special != MAXALIGN(phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	itemcount = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > itemcount)
		elog(ERROR, "invalid index offnum: %u", offnum);

	tupid = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tupid));
	oldsize = ItemIdGetLength(tupid);
	offset = ItemIdGetOffset(tupid);

	if (offset < phdr->pd_upper || (offset + oldsize) > phdr->pd_special ||
		offset != MAXALIGN(offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted line pointer: offset = %u, size = %u",
						offset, (unsigned int) oldsize)));

	/*
	 * 确定空间需求的实际变化，并检查页是否溢出。
	 */
	oldsize = MAXALIGN(oldsize);
	alignednewsize = MAXALIGN(newsize);
	if (alignednewsize > oldsize + (phdr->pd_upper - phdr->pd_lower))
		return false;

	/*
	 * 重新安置已有数据并更新行指针，除非新元组
	 * 与旧元组（对齐之后）大小相同，那时就
	 * 无事可做。注意要重新安置的是目标元组之前的数据，
	 * 不是之后的数据，因此把 size_diff
	 * 表示元组尺寸减少的量比较方便，使它成为
	 * 要加到 pd_upper 和受影响行指针上的增量。
	 */
	size_diff = oldsize - (int) alignednewsize;
	if (size_diff != 0)
	{
		char	   *addr = (char *) page + phdr->pd_upper;
		int			i;

		/* 重新安置目标元组之前的全部元组数据 */
		memmove(addr + size_diff, addr, offset - phdr->pd_upper);

		/* 调整空闲空间边界指针 */
		phdr->pd_upper += size_diff;

		/* 也调整受影响的行指针 */
		for (i = FirstOffsetNumber; i <= itemcount; i++)
		{
			ItemId		ii = PageGetItemId(page, i);

			/* 允许没有存储的项；目前只有 BRIN 需要这样 */
			if (ItemIdHasStorage(ii) && ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size_diff;
		}
	}

	/* 更新该项的元组长度，但不改变它的 lp_flags 字段 */
	tupid->lp_off = offset + size_diff;
	tupid->lp_len = newsize;

	/* 把新元组数据复制到页上 */
	memcpy(PageGetItem(page, tupid), newtup, newsize);

	return true;
}


/*
 * 为共享缓冲区中的页设置校验和。
 *
 * 若禁用了校验和，或该页尚未初始化，就直接返回
 * 输入。否则，在计算校验和之前必须复制该页，
 * 以免并发修改（例如设置提示位）
 * 使最终校验和无效。复制时带不带提示都没关系，
 * 只要写出的是有效页以及
 * 与之配套的校验和。
 *
 * 返回指向需要写出的、块大小数据的指针。使用
 * 静态分配的内存，因此调用者必须立刻写出
 * 返回的页，并且不得再次引用它。
 */
char *
PageSetChecksumCopy(Page page, BlockNumber blkno)
{
	static char *pageCopy = NULL;

	/* 若不需要校验和，就直接返回传入的数据 */
	if (PageIsNew(page) || !DataChecksumsEnabled())
		return (char *) page;

	/*
	 * 我们只分配一次副本空间，并在此后每次
	 * 调用中重复使用。这里用 palloc，而不是静态 char
	 * 数组，首先是为了给校验和代码足够的对齐，
	 * 其次是避免在从不调用这里的进程里浪费空间。
	 */
	if (pageCopy == NULL)
		pageCopy = MemoryContextAllocAligned(TopMemoryContext,
											 BLCKSZ,
											 PG_IO_ALIGN_SIZE,
											 0);

	memcpy(pageCopy, (char *) page, BLCKSZ);
	((PageHeader) pageCopy)->pd_checksum = pg_checksum_page(pageCopy, blkno);
	return pageCopy;
}

/*
 * 为私有内存中的页设置校验和。
 *
 * 只有在我们知道没有其他进程会修改
 * 该页缓冲区时才能使用。
 */
void
PageSetChecksumInplace(Page page, BlockNumber blkno)
{
	/* 若不需要校验和，就直接返回 */
	if (PageIsNew(page) || !DataChecksumsEnabled())
		return;

	((PageHeader) page)->pd_checksum = pg_checksum_page((char *) page, blkno);
}
