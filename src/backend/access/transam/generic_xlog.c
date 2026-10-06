/*-------------------------------------------------------------------------
 *
 * generic_xlog.c
 *	 通用 WAL 记录的实现。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/generic_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/generic_xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "utils/memutils.h"

/*-------------------------------------------------------------------------
 * 在内部，页面之间的增量由一组片段组成。每个
 * 片段表示页面某一区域中所做的更改。片段
 * 的构成如下：
 *
 * - 页面区域的偏移（OffsetNumber）
 * - 页面区域的长度（OffsetNumber）
 * - data - 放入该区域的数据（'length' 个字节）
 *
 * 页面中未更改的区域不出现在其增量中。因此，
 * 增量可以比完整页面映像更紧凑。但若两个片段
 * 之间未更改区域小于片段
 * 头部（offset+length），则对增量总体大小并无
 * 好处。因此，若它们之间未更改
 * 区域 <= MATCH_THRESHOLD 字节，我们会合并相邻片段。
 *
 * 我们不费心跨页面的 "lower" 与 "upper" 部分
 * 合并片段；pd_lower 与 pd_upper 彼此相距
 * MATCH_THRESHOLD 字节以内极少见，处理这种少见情况
 * 会不必要地使增量计算代码变复杂并变慢。
 * 因此，最坏情况下的增量大小包括两个片段头部加上
 * 一整页的数据。
 *-------------------------------------------------------------------------
 */
#define FRAGMENT_HEADER_SIZE	(2 * sizeof(OffsetNumber))
#define MATCH_THRESHOLD			FRAGMENT_HEADER_SIZE
#define MAX_DELTA_SIZE			(BLCKSZ + 2 * FRAGMENT_HEADER_SIZE)

/* 单页通用 WAL 数据的结构 */
typedef struct
{
	Buffer		buffer;			/* 已注册的缓冲区 */
	int			flags;			/* 该缓冲区的标志 */
	int			deltaLen;		/* delta 字段已占用的空间 */
	char	   *image;			/* 供修改的页面映像副本，不要
								 * 就地修改以便获得对齐的内存块 */
	char		delta[MAX_DELTA_SIZE];	/* 页面映像之间的增量 */
} PageData;

/*
 * 通用 WAL 记录构造的状态。必须分配在 I/O
 * 对齐的地址上。
 */
struct GenericXLogState
{
	/* 页面映像（正确对齐，必须在最前） */
	PGIOAlignedBlock images[MAX_GENERIC_XLOG_PAGES];
	/* 各页信息，见上文 */
	PageData	pages[MAX_GENERIC_XLOG_PAGES];
	bool		isLogged;
};

static void writeFragment(PageData *pageData, OffsetNumber offset,
						  OffsetNumber length, const char *data);
static void computeRegionDelta(PageData *pageData,
							   const char *curpage, const char *targetpage,
							   int targetStart, int targetEnd,
							   int validStart, int validEnd);
static void computeDelta(PageData *pageData, Page curpage, Page targetpage);
static void applyPageRedo(Page page, const char *delta, Size deltaSize);


/*
 * 将下一片段写入 pageData 的增量。
 *
 * 片段具有给定的偏移与长度，data 指向
 * 实际数据（长度为 length）。
 */
static void
writeFragment(PageData *pageData, OffsetNumber offset, OffsetNumber length,
			  const char *data)
{
	char	   *ptr = pageData->delta + pageData->deltaLen;

	/* 验证有足够空间 */
	Assert(pageData->deltaLen + sizeof(offset) +
		   sizeof(length) + length <= sizeof(pageData->delta));

	/* 写入片段数据 */
	memcpy(ptr, &offset, sizeof(offset));
	ptr += sizeof(offset);
	memcpy(ptr, &length, sizeof(length));
	ptr += sizeof(length);
	memcpy(ptr, data, length);
	ptr += length;

	pageData->deltaLen = ptr - pageData->delta;
}

/*
 * 计算将 curpage 某一区域变换为 targetpage 对应
 * 区域所需的 XLOG 片段，并追加到 pageData 的 delta
 * 字段。要变换的区域从 targetStart 到 targetEnd-1。
 * curpage 中 validStart 到 validEnd-1 范围之外的字节应视为
 * 无效，并总是用目标数据覆盖。
 *
 * 本函数是热点，因此数据匹配循环应尽可能
 * 严谨。
 */
static void
computeRegionDelta(PageData *pageData,
				   const char *curpage, const char *targetpage,
				   int targetStart, int targetEnd,
				   int validStart, int validEnd)
{
	int			i,
				loopEnd,
				fragmentBegin = -1,
				fragmentEnd = -1;

	/* 处理任何无效起始区域，将其纳入第一个片段 */
	if (validStart > targetStart)
	{
		fragmentBegin = targetStart;
		targetStart = validStart;
	}

	/* 无效结束区域将在主循环之后处理 */
	loopEnd = Min(targetEnd, validEnd);

	/* 检查所有可能匹配的字节 */
	i = targetStart;
	while (i < loopEnd)
	{
		if (curpage[i] != targetpage[i])
		{
			/* 遇到不匹配字节时，若不在片段中则开始新片段 */
			if (fragmentBegin < 0)
				fragmentBegin = i;
			/* 将不匹配数据终点标为不确定 */
			fragmentEnd = -1;
			/* 在紧循环中尽可能延伸该片段 */
			i++;
			while (i < loopEnd && curpage[i] != targetpage[i])
				i++;
			if (i >= loopEnd)
				break;
		}

		/* 找到匹配字节，故记住不匹配片段的终点 */
		fragmentEnd = i;

		/*
		 * 在紧循环中尽可能延伸匹配。（在典型
		 * 负载下，此内层循环占本函数运行时间的大部分。）
		 */
		i++;
		while (i < loopEnd && curpage[i] == targetpage[i])
			i++;

		/*
		 * 此时有几种可能情况：
		 *
		 * 1. 没有未写出的片段（fragmentBegin < 0）。无需
		 * 写出；且 fragmentEnd 是什么都无关紧要。
		 *
		 * 2. 找到超过 MATCH_THRESHOLD 个连续匹配字节。
		 * 写出未写出的片段，停在 fragmentEnd。
		 *
		 * 3. 匹配延伸到 loopEnd。此处什么也不做，退出
		 * 循环，然后在与无效结束区域（若有）合并后
		 * 写出未写出的片段。若未如此合并，fragmentEnd
		 * 决定最终 writeFragment 调用需要写出多少。
		 *
		 * 4. 在 loopEnd 之前找到不匹配字节。循环将重复
		 * 并进入上文的不匹配字节分支。因此在此情况下
		 * 同样，fragmentEnd 是什么都无关紧要。匹配的字节
		 * 将并入继续延伸的不匹配片段。
		 *
		 * 只有情况 3 到达循环底部时 fragmentEnd 才有
		 * 意义，因此上文无条件地赋值
		 * "fragmentEnd = i" 是可以的。
		 */
		if (fragmentBegin >= 0 && i - fragmentEnd > MATCH_THRESHOLD)
		{
			writeFragment(pageData, fragmentBegin,
						  fragmentEnd - fragmentBegin,
						  targetpage + fragmentBegin);
			fragmentBegin = -1;
			fragmentEnd = -1;	/* 并非真正必要 */
		}
	}

	/* 处理任何无效结束区域，将其纳入最终片段 */
	if (loopEnd < targetEnd)
	{
		if (fragmentBegin < 0)
			fragmentBegin = loopEnd;
		fragmentEnd = targetEnd;
	}

	/* 若有则写出最终片段 */
	if (fragmentBegin >= 0)
	{
		if (fragmentEnd < 0)
			fragmentEnd = targetEnd;
		writeFragment(pageData, fragmentBegin,
					  fragmentEnd - fragmentBegin,
					  targetpage + fragmentBegin);
	}
}

/*
 * 计算将 curpage 变换为 targetpage 所需的 XLOG 增量记录，
 * 并存入 pageData 的 delta 字段。
 */
static void
computeDelta(PageData *pageData, Page curpage, Page targetpage)
{
	int			targetLower = ((PageHeader) targetpage)->pd_lower,
				targetUpper = ((PageHeader) targetpage)->pd_upper,
				curLower = ((PageHeader) curpage)->pd_lower,
				curUpper = ((PageHeader) curpage)->pd_upper;

	pageData->deltaLen = 0;

	/* 计算页面下半部分的增量记录…… */
	computeRegionDelta(pageData, curpage, targetpage,
					   0, targetLower,
					   0, curLower);
	/* ……以及上半部分，忽略中间部分 */
	computeRegionDelta(pageData, curpage, targetpage,
					   targetUpper, BLCKSZ,
					   curUpper, BLCKSZ);

	/*
	 * 若启用了 xlog 调试，则检查所产生的增量。将增量
	 * 应用于 curpage 的结果应等价于 targetpage。
	 */
#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
	{
		PGAlignedBlock tmp;

		memcpy(tmp.data, curpage, BLCKSZ);
		applyPageRedo(tmp.data, pageData->delta, pageData->deltaLen);
		if (memcmp(tmp.data, targetpage, targetLower) != 0 ||
			memcmp(tmp.data + targetUpper, targetpage + targetUpper,
				   BLCKSZ - targetUpper) != 0)
			elog(ERROR, "result of generic xlog apply does not match");
	}
#endif
}

/*
 * 为对指定关系的修改开始新的通用 WAL 记录。
 */
GenericXLogState *
GenericXLogStart(Relation relation)
{
	GenericXLogState *state;
	int			i;

	state = (GenericXLogState *) palloc_aligned(sizeof(GenericXLogState),
												PG_IO_ALIGN_SIZE,
												0);
	state->isLogged = RelationNeedsWAL(relation);

	for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
	{
		state->pages[i].image = state->images[i].data;
		state->pages[i].buffer = InvalidBuffer;
	}

	return state;
}

/*
 * 为通用 WAL 记录注册新缓冲区。
 *
 * 返回 GenericXLogState 中该页映像的指针，调用方
 * 应修改该映像。
 *
 * 若缓冲区已注册，则仅返回其已有条目。
 * （此种情况下如何处理 flags 不太清楚，但
 * 目前我们保留原来的 flags。）
 */
Page
GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buffer, int flags)
{
	int			block_id;

	/* 在数组中搜索已有条目或第一个未用槽位 */
	for (block_id = 0; block_id < MAX_GENERIC_XLOG_PAGES; block_id++)
	{
		PageData   *page = &state->pages[block_id];

		if (BufferIsInvalid(page->buffer))
		{
			/* 空槽，故使用它（后面不可能再有匹配） */
			page->buffer = buffer;
			page->flags = flags;
			memcpy(page->image, BufferGetPage(buffer), BLCKSZ);
			return (Page) page->image;
		}
		else if (page->buffer == buffer)
		{
			/*
			 * 缓冲区已注册。仅返回映像，该映像
			 * 已准备好。
			 */
			return (Page) page->image;
		}
	}

	elog(ERROR, "maximum number %d of generic xlog buffers is exceeded",
		 MAX_GENERIC_XLOG_PAGES);
	/* 让编译器安静 */
	return NULL;
}

/*
 * 将 GenericXLogState 所表示的更改应用到实际缓冲区，
 * 并发出一条通用 WAL 记录。
 */
XLogRecPtr
GenericXLogFinish(GenericXLogState *state)
{
	XLogRecPtr	lsn;
	int			i;

	if (state->isLogged)
	{
		/* 已记录关系：在临界区内构造 xlog 记录。 */
		XLogBeginInsert();

		START_CRIT_SECTION();

		/*
		 * 必要时计算增量，将更改写入缓冲区，标记
		 * 缓冲区为脏，并注册更改。
		 */
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];
			Page		page;
			PageHeader	pageHeader;

			if (BufferIsInvalid(pageData->buffer))
				continue;

			page = BufferGetPage(pageData->buffer);
			pageHeader = (PageHeader) pageData->image;

			/*
			 * 在仍同时拥有未修改页面与新映像时计算
			 * 增量。若记录完整映像则不需要。
			 */
			if (!(pageData->flags & GENERIC_XLOG_FULL_IMAGE))
				computeDelta(pageData, page, (Page) pageData->image);

			/*
			 * 应用映像，注意将 pd_lower 与 pd_upper 之间的
			 * "空洞"清零，以免实际页面状态与重放
			 * 所产生的结果出现分歧。
			 */
			memcpy(page, pageData->image, pageHeader->pd_lower);
			memset(page + pageHeader->pd_lower, 0,
				   pageHeader->pd_upper - pageHeader->pd_lower);
			memcpy(page + pageHeader->pd_upper,
				   pageData->image + pageHeader->pd_upper,
				   BLCKSZ - pageHeader->pd_upper);

			MarkBufferDirty(pageData->buffer);

			if (pageData->flags & GENERIC_XLOG_FULL_IMAGE)
			{
				XLogRegisterBuffer(i, pageData->buffer,
								   REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			}
			else
			{
				XLogRegisterBuffer(i, pageData->buffer, REGBUF_STANDARD);
				XLogRegisterBufData(i, pageData->delta, pageData->deltaLen);
			}
		}

		/* 插入 xlog 记录 */
		lsn = XLogInsert(RM_GENERIC_ID, 0);

		/* 设置 LSN */
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];

			if (BufferIsInvalid(pageData->buffer))
				continue;
			PageSetLSN(BufferGetPage(pageData->buffer), lsn);
		}
		END_CRIT_SECTION();
	}
	else
	{
		/* 未记录关系：跳过与 xlog 相关的处理 */
		START_CRIT_SECTION();
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];

			if (BufferIsInvalid(pageData->buffer))
				continue;
			memcpy(BufferGetPage(pageData->buffer),
				   pageData->image,
				   BLCKSZ);
			/* 此种情况下我们不操心将"空洞"清零 */
			MarkBufferDirty(pageData->buffer);
		}
		END_CRIT_SECTION();
		/* 此种情况下没有可返回的 LSN */
		lsn = InvalidXLogRecPtr;
	}

	pfree(state);

	return lsn;
}

/*
 * 中止通用 WAL 记录的构造。不对缓冲区应用任何更改。
 *
 * 注意：若需要，由调用方负责释放缓冲区上的锁/钉住。
 */
void
GenericXLogAbort(GenericXLogState *state)
{
	pfree(state);
}

/*
 * 将增量应用到给定页面映像。
 */
static void
applyPageRedo(Page page, const char *delta, Size deltaSize)
{
	const char *ptr = delta;
	const char *end = delta + deltaSize;

	while (ptr < end)
	{
		OffsetNumber offset,
					length;

		memcpy(&offset, ptr, sizeof(offset));
		ptr += sizeof(offset);
		memcpy(&length, ptr, sizeof(length));
		ptr += sizeof(length);

		memcpy(page + offset, ptr, length);

		ptr += length;
	}
}

/*
 * 通用 WAL 记录的重做函数。
 */
void
generic_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffers[MAX_GENERIC_XLOG_PAGES];
	uint8		block_id;

	/* 保护 buffers[] 数组的有限大小 */
	Assert(XLogRecMaxBlockId(record) < MAX_GENERIC_XLOG_PAGES);

	/* 遍历各个块 */
	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		XLogRedoAction action;

		if (!XLogRecHasBlockRef(record, block_id))
		{
			buffers[block_id] = InvalidBuffer;
			continue;
		}

		action = XLogReadBufferForRedo(record, block_id, &buffers[block_id]);

		/* 若需要则对给定块应用重做 */
		if (action == BLK_NEEDS_REDO)
		{
			Page		page;
			PageHeader	pageHeader;
			char	   *blockDelta;
			Size		blockDeltaSize;

			page = BufferGetPage(buffers[block_id]);
			blockDelta = XLogRecGetBlockData(record, block_id, &blockDeltaSize);
			applyPageRedo(page, blockDelta, blockDeltaSize);

			/*
			 * 由于增量不包含 pd_lower 与 pd_upper 之间
			 * "空洞"中有何内容的信息，将其置零，
			 * 以确保产生与 GenericXLogFinish 应用已记录
			 * 动作时相同的页面状态。
			 */
			pageHeader = (PageHeader) page;
			memset(page + pageHeader->pd_lower, 0,
				   pageHeader->pd_upper - pageHeader->pd_lower);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffers[block_id]);
		}
	}

	/* 更改已完成：解锁并释放所有缓冲区 */
	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		if (BufferIsValid(buffers[block_id]))
			UnlockReleaseBuffer(buffers[block_id]);
	}
}

/*
 * 在对通用页面做一致性检查之前对其进行掩码处理。
 */
void
generic_mask(char *page, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(page);

	mask_unused_space(page);
}
