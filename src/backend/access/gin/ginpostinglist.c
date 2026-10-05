/*-------------------------------------------------------------------------
 *
 * ginpostinglist.c
 *	  处理倒排列表的例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginpostinglist.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"

#ifdef USE_ASSERT_CHECKING
#define CHECK_ENCODING_ROUNDTRIP
#endif

/*
 * 就编码而言，项指针表示为 64 位无符号
 * 整数。最低 11 位表示偏移号，接下来
 * 较低的 32 位是块号。于是剩下 21 位未使用，也就是
 * 只用了低 43 位。
 *
 * 11 位对偏移号已经够用，因为 MaxHeapTuplesPerPage <
 * 2^11（在所有受支持的块大小上）。我们省着用这些位，因为
 * 较小的整数在 varbyte 编码中占用更少字节，从而节省磁盘
 * 空间。（若将来的新表访问方法想使用偏移号的全部
 * 可能范围，就需要改这里。）
 *
 * 这些 43 位整数使用 varbyte 编码。每个字节中，
 * 低 7 位存放数据，最高位是延续位。
 * 延续位置位时，下一字节仍属于同一个
 * 整数，否则该字节就是这个整数的最后一字节。43 位
 * 在这种编码中最多需要 7 字节：
 *
 * 0XXXXXXX
 * 1XXXXXXX 0XXXXYYY
 * 1XXXXXXX 1XXXXYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 1YYYYYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 1YYYYYYY 1YYYYYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 1YYYYYYY 1YYYYYYY 1YYYYYYY 0uuuuuuY
 *
 * X = 用于偏移号的位
 * Y = 用于块号的位
 * u = 未使用的位
 *
 * 这些字节按小端序存储。
 *
 * 这种编码的一个重要性质是：从列表中移除一项
 * 绝不会使所得压缩倒排列表变大。证明：
 *
 * 移除一个数，其实是用二者之和替换这两个数。我们
 * 必须证明和的 varbyte 编码不会长于各加数的 varbyte
 * 编码。两数之和最多比较大的那个加数宽一位。
 * 把一个数加宽一位，其 varbyte 编码长度最多增加
 * 一字节。因此，和的 varbyte 编码
 * 最多比较大加数的 varbyte 编码长一字节。较小的
 * 加数至少占一字节，所以和占用的空间无法超过
 * 各加数，证毕。
 *
 * 这一性质大大简化了 VACUUM：它可以假定倒排
 * 列表在清理之后总能放进同一页。注意，即便
 * 从倒排列表中移除项时这一点成立，清理时也必须
 * 小心不要引起膨胀，例如把页上未压缩的项
 * 合并进压缩列表的时候。
 */

/*
 * 编码偏移号需要多少位？OffsetNumber 是 16 位
 * 整数，但一页上放不下那么多项。11 位应当绰绰
 * 有余。从 MaxHeapTuplesPerPage 推导并使用最少
 * 位数很诱人，但若 MaxHeapTuplesPerPage 改变，就需要改磁盘
 * 格式。最好留些余量。
 */
#define MaxHeapTuplesPerPageBits		11

/* 编码所支持的最大整数所需的最大字节数。 */
#define MaxBytesPerInteger				7

static inline uint64
itemptr_to_uint64(const ItemPointer iptr)
{
	uint64		val;

	Assert(ItemPointerIsValid(iptr));
	Assert(GinItemPointerGetOffsetNumber(iptr) < (1 << MaxHeapTuplesPerPageBits));

	val = GinItemPointerGetBlockNumber(iptr);
	val <<= MaxHeapTuplesPerPageBits;
	val |= GinItemPointerGetOffsetNumber(iptr);

	return val;
}

static inline void
uint64_to_itemptr(uint64 val, ItemPointer iptr)
{
	GinItemPointerSetOffsetNumber(iptr, val & ((1 << MaxHeapTuplesPerPageBits) - 1));
	val = val >> MaxHeapTuplesPerPageBits;
	GinItemPointerSetBlockNumber(iptr, val);

	Assert(ItemPointerIsValid(iptr));
}

/*
 * 把 val 以 varbyte 编码写入 *ptr。*ptr 递增到下一个整数。
 */
static void
encode_varbyte(uint64 val, unsigned char **ptr)
{
	unsigned char *p = *ptr;

	while (val > 0x7F)
	{
		*(p++) = 0x80 | (val & 0x7F);
		val >>= 7;
	}
	*(p++) = (unsigned char) val;

	*ptr = p;
}

/*
 * 解码 *ptr 处 varbyte 编码的整数。*ptr 递增到下一个整数。
 */
static uint64
decode_varbyte(unsigned char **ptr)
{
	uint64		val;
	unsigned char *p = *ptr;
	uint64		c;

	/* 第 1 字节 */
	c = *(p++);
	val = c & 0x7F;
	if (c & 0x80)
	{
		/* 第 2 字节 */
		c = *(p++);
		val |= (c & 0x7F) << 7;
		if (c & 0x80)
		{
			/* 第 3 字节 */
			c = *(p++);
			val |= (c & 0x7F) << 14;
			if (c & 0x80)
			{
				/* 第 4 字节 */
				c = *(p++);
				val |= (c & 0x7F) << 21;
				if (c & 0x80)
				{
					/* 第 5 字节 */
					c = *(p++);
					val |= (c & 0x7F) << 28;
					if (c & 0x80)
					{
						/* 第 6 字节 */
						c = *(p++);
						val |= (c & 0x7F) << 35;
						if (c & 0x80)
						{
							/* 第 7 字节，不应带延续位 */
							c = *(p++);
							val |= c << 42;
							Assert((c & 0x80) == 0);
						}
					}
				}
			}
		}
	}

	*ptr = p;

	return val;
}

/*
 * 编码一份倒排列表。
 *
 * 编码结果放在 palloc 得到的结构里返回，大小最多为
 * maxsize 字节。返回段中的项数
 * 经 *nwritten 返回。若它不等于 nipd，则并非所有项都放得进
 * maxsize，只编码了前 *nwritten 项。
 *
 * 返回结构的分配大小按短对齐，末尾的填充
 * 字节（若有）为零。
 */
GinPostingList *
ginCompressPostingList(const ItemPointer ipd, int nipd, int maxsize,
					   int *nwritten)
{
	uint64		prev;
	int			totalpacked = 0;
	int			maxbytes;
	GinPostingList *result;
	unsigned char *ptr;
	unsigned char *endptr;

	maxsize = SHORTALIGN_DOWN(maxsize);

	result = palloc(maxsize);

	maxbytes = maxsize - offsetof(GinPostingList, bytes);
	Assert(maxbytes > 0);

	/* 存储第一个特殊项 */
	result->first = ipd[0];

	prev = itemptr_to_uint64(&result->first);

	ptr = result->bytes;
	endptr = result->bytes + maxbytes;
	for (totalpacked = 1; totalpacked < nipd; totalpacked++)
	{
		uint64		val = itemptr_to_uint64(&ipd[totalpacked]);
		uint64		delta = val - prev;

		Assert(val > prev);

		if (endptr - ptr >= MaxBytesPerInteger)
			encode_varbyte(delta, &ptr);
		else
		{
			/*
			 * 剩余不足 7 字节。写出下一项之前，必须检查
			 * 它是否放得进这块空间。
			 */
			unsigned char buf[MaxBytesPerInteger];
			unsigned char *p = buf;

			encode_varbyte(delta, &p);
			if (p - buf > (endptr - ptr))
				break;			/* 输出已满 */

			memcpy(ptr, buf, p - buf);
			ptr += (p - buf);
		}
		prev = val;
	}
	result->nbytes = ptr - result->bytes;

	/*
	 * 若写入的字节数为奇数，把末尾的填充字节
	 * 清零。
	 */
	if (result->nbytes != SHORTALIGN(result->nbytes))
		result->bytes[result->nbytes] = 0;

	if (nwritten)
		*nwritten = totalpacked;

	Assert(SizeOfGinPostingList(result) <= maxsize);

	/*
	 * 检查编码后的段能否解码回原来的项。
	 */
#if defined (CHECK_ENCODING_ROUNDTRIP)
	{
		int			ndecoded;
		ItemPointer tmp = ginPostingListDecode(result, &ndecoded);

		Assert(ndecoded == totalpacked);
		Assert(memcmp(tmp, ipd, ndecoded * sizeof(ItemPointerData)) == 0);
		pfree(tmp);
	}
#endif

	return result;
}

/*
 * 把压缩倒排列表解码为项指针数组。
 * 项数经 *ndecoded 返回。
 */
ItemPointer
ginPostingListDecode(GinPostingList *plist, int *ndecoded_out)
{
	return ginPostingListDecodeAllSegments(plist,
										   SizeOfGinPostingList(plist),
										   ndecoded_out);
}

/*
 * 把多个倒排列表段解码为项指针数组。
 * 项数经 *ndecoded_out 返回。这些段
 * 一个接一个存放，总大小为 len 字节。
 */
ItemPointer
ginPostingListDecodeAllSegments(GinPostingList *segment, int len, int *ndecoded_out)
{
	ItemPointer result;
	int			nallocated;
	uint64		val;
	char	   *endseg = ((char *) segment) + len;
	int			ndecoded;
	unsigned char *ptr;
	unsigned char *endptr;

	/*
	 * 先估算数组的初始大小。
	 */
	nallocated = segment->nbytes * 2 + 1;
	result = palloc(nallocated * sizeof(ItemPointerData));

	ndecoded = 0;
	while ((char *) segment < endseg)
	{
		/* 必要时扩大输出数组 */
		if (ndecoded >= nallocated)
		{
			nallocated *= 2;
			result = repalloc(result, nallocated * sizeof(ItemPointerData));
		}

		/* 复制第一项 */
		Assert(OffsetNumberIsValid(ItemPointerGetOffsetNumber(&segment->first)));
		Assert(ndecoded == 0 || ginCompareItemPointers(&segment->first, &result[ndecoded - 1]) > 0);
		result[ndecoded] = segment->first;
		ndecoded++;

		val = itemptr_to_uint64(&segment->first);
		ptr = segment->bytes;
		endptr = segment->bytes + segment->nbytes;
		while (ptr < endptr)
		{
			/* 必要时扩大输出数组 */
			if (ndecoded >= nallocated)
			{
				nallocated *= 2;
				result = repalloc(result, nallocated * sizeof(ItemPointerData));
			}

			val += decode_varbyte(&ptr);

			uint64_to_itemptr(val, &result[ndecoded]);
			ndecoded++;
		}
		segment = GinNextPostingListSegment(segment);
	}

	if (ndecoded_out)
		*ndecoded_out = ndecoded;
	return result;
}

/*
 * 把一批倒排列表中的全部项指针加入 TIDBitmap。
 */
int
ginPostingListDecodeAllSegmentsToTbm(GinPostingList *ptr, int len,
									 TIDBitmap *tbm)
{
	int			ndecoded;
	ItemPointer items;

	items = ginPostingListDecodeAllSegments(ptr, len, &ndecoded);
	tbm_add_tuples(tbm, items, ndecoded, false);
	pfree(items);

	return ndecoded;
}

/*
 * 合并两个有序的项指针数组，并消除任何重复项。
 *
 * 返回 palloc 得到的数组，*nmerged 设为消除重复项之后
 * 结果中的项数。
 */
ItemPointer
ginMergeItemPointers(ItemPointerData *a, uint32 na,
					 ItemPointerData *b, uint32 nb,
					 int *nmerged)
{
	ItemPointerData *dst;

	dst = (ItemPointer) palloc((na + nb) * sizeof(ItemPointerData));

	/*
	 * 若参数数组互不重叠，就可以直接把它们彼此
	 * 拼接起来。
	 */
	if (na == 0 || nb == 0 || ginCompareItemPointers(&a[na - 1], &b[0]) < 0)
	{
		memcpy(dst, a, na * sizeof(ItemPointerData));
		memcpy(&dst[na], b, nb * sizeof(ItemPointerData));
		*nmerged = na + nb;
	}
	else if (ginCompareItemPointers(&b[nb - 1], &a[0]) < 0)
	{
		memcpy(dst, b, nb * sizeof(ItemPointerData));
		memcpy(&dst[nb], a, na * sizeof(ItemPointerData));
		*nmerged = na + nb;
	}
	else
	{
		ItemPointerData *dptr = dst;
		ItemPointerData *aptr = a;
		ItemPointerData *bptr = b;

		while (aptr - a < na && bptr - b < nb)
		{
			int			cmp = ginCompareItemPointers(aptr, bptr);

			if (cmp > 0)
				*dptr++ = *bptr++;
			else if (cmp == 0)
			{
				/* 相同项只保留一份 */
				*dptr++ = *bptr++;
				aptr++;
			}
			else
				*dptr++ = *aptr++;
		}

		while (aptr - a < na)
			*dptr++ = *aptr++;

		while (bptr - b < nb)
			*dptr++ = *bptr++;

		*nmerged = dptr - dst;
	}

	return dst;
}
