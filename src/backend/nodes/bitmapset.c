/*-------------------------------------------------------------------------
 *
 * bitmapset.c
 *	  PostgreSQL generic bitmap set package
 *
 * A bitmap set can represent any set of nonnegative integers, although
 * it is mainly intended for sets where the maximum value is not large,
 * say at most a few hundred.  By convention, we always represent the
 * empty set by a NULL pointer.
 *
 *
 * Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/nodes/bitmapset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"
#include "port/pg_bitutils.h"


#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

#define BITMAPSET_SIZE(nwords)	\
	(offsetof(Bitmapset, words) + (nwords) * sizeof(bitmapword))

/*----------
 * This is a well-known cute trick for isolating the rightmost one-bit
 * in a word.  It assumes two's complement arithmetic.  Consider any
 * nonzero value, and focus attention on the rightmost one.  The value is
 * then something like
 *				xxxxxx10000
 * where x's are unspecified bits.  The two's complement negative is formed
 * by inverting all the bits and adding one.  Inversion gives
 *				yyyyyy01111
 * where each y is the inverse of the corresponding x.  Incrementing gives
 *				yyyyyy10000
 * and then ANDing with the original value gives
 *				00000010000
 * This works for all cases except original value = zero, where of course
 * we get zero.
 *----------
 */
#define RIGHTMOST_ONE(x) ((signedbitmapword) (x) & -((signedbitmapword) (x)))

#define HAS_MULTIPLE_ONES(x)	((bitmapword) RIGHTMOST_ONE(x) != (x))

/* 按位图字长选择合适的位操作函数 */
#if BITS_PER_BITMAPWORD == 32
#define bmw_leftmost_one_pos(w)		pg_leftmost_one_pos32(w)
#define bmw_rightmost_one_pos(w)	pg_rightmost_one_pos32(w)
#define bmw_popcount(w)				pg_popcount32(w)
#elif BITS_PER_BITMAPWORD == 64
#define bmw_leftmost_one_pos(w)		pg_leftmost_one_pos64(w)
#define bmw_rightmost_one_pos(w)	pg_rightmost_one_pos64(w)
#define bmw_popcount(w)				pg_popcount64(w)
#else
#error "invalid BITS_PER_BITMAPWORD"
#endif

static bool bms_is_empty_internal(const Bitmapset *a);


/*
 * bms_copy - 制作位图集的 palloc 副本
 */
Bitmapset *
bms_copy(const Bitmapset *a)
{
	Bitmapset  *result;
	size_t		size;

	if (a == NULL)
		return NULL;
	size = BITMAPSET_SIZE(a->nwords);
	result = (Bitmapset *) palloc(size);
	memcpy(result, a, size);
	return result;
}

/*
 * bms_equal - 两个位图集是否相等？
 *
 * 这是逻辑相等，而不是物理相等；特别是，NULL 指针会
 * 被报告为与一个不含成员的 palloc 值相等。
 */
bool
bms_equal(const Bitmapset *a, const Bitmapset *b)
{
	const Bitmapset *shorter;
	const Bitmapset *longer;
	int			shortlen;
	int			longlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
	{
		if (b == NULL)
			return true;
		return false;
	}
	else if (b == NULL)
		return false;
	/* 区分较短与较长的输入 */
	if (a->nwords <= b->nwords)
	{
		shorter = a;
		longer = b;
	}
	else
	{
		shorter = b;
		longer = a;
	}
	/* 然后处理 */
	shortlen = shorter->nwords;
	for (i = 0; i < shortlen; i++)
	{
		if (shorter->words[i] != longer->words[i])
			return false;
	}
	longlen = longer->nwords;
	for (; i < longlen; i++)
	{
		if (longer->words[i] != 0)
			return false;
	}
	return true;
}

/*
 * bms_compare - 供 qsort 使用的位图集比较器
 *
 * 这保证：当且仅当 bms_equal 会判定它们相等时，才报告
 * 相等。否则，由只在其中一个值中置位的最高编号位
 * 决定结果。（按此规则，例如
 * {6} 大于 {5}，这看起来合理。）
 */
int
bms_compare(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return (b == NULL) ? 0 : -1;
	else if (b == NULL)
		return +1;
	/* 处理一个输入比另一个更长的情况 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = shortlen; i < a->nwords; i++)
	{
		if (a->words[i] != 0)
			return +1;
	}
	for (i = shortlen; i < b->nwords; i++)
	{
		if (b->words[i] != 0)
			return -1;
	}
	/* 处理公共的字 */
	i = shortlen;
	while (--i >= 0)
	{
		bitmapword	aw = a->words[i];
		bitmapword	bw = b->words[i];

		if (aw != bw)
			return (aw > bw) ? +1 : -1;
	}
	return 0;
}

/*
 * bms_make_singleton - 构建只含单个成员的位图集
 */
Bitmapset *
bms_make_singleton(int x)
{
	Bitmapset  *result;
	int			wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	result = (Bitmapset *) palloc0(BITMAPSET_SIZE(wordnum + 1));
	result->type = T_Bitmapset;
	result->nwords = wordnum + 1;
	result->words[wordnum] = ((bitmapword) 1 << bitnum);
	return result;
}

/*
 * bms_free - 释放一个位图集
 *
 * 与 pfree 相同，但允许 NULL 输入
 */
void
bms_free(Bitmapset *a)
{
	if (a)
		pfree(a);
}


/*
 * 这些操作都会做出一份新 palloc 的结果，
 * 而不改动其输入
 */


/*
 * bms_union - 集合并
 */
Bitmapset *
bms_union(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return bms_copy(a);
	/* 区分较短与较长的输入；复制较长的那个 */
	if (a->nwords <= b->nwords)
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = bms_copy(a);
		other = b;
	}
	/* 并把较短的输入并入结果 */
	otherlen = other->nwords;
	for (i = 0; i < otherlen; i++)
		result->words[i] |= other->words[i];
	return result;
}

/*
 * bms_intersect - 集合交
 */
Bitmapset *
bms_intersect(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			resultlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL || b == NULL)
		return NULL;
	/* 区分较短与较长的输入；复制较短的那个 */
	if (a->nwords <= b->nwords)
	{
		result = bms_copy(a);
		other = b;
	}
	else
	{
		result = bms_copy(b);
		other = a;
	}
	/* 并把较长的输入与结果求交 */
	resultlen = result->nwords;
	for (i = 0; i < resultlen; i++)
		result->words[i] &= other->words[i];
	/* 若算出空结果，必须返回 NULL */
	if (bms_is_empty_internal(result))
	{
		pfree(result);
		return NULL;
	}
	return result;
}

/*
 * bms_difference - 集合差（即 A 去掉 B 的成员）
 */
Bitmapset *
bms_difference(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	int			shortlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return NULL;
	if (b == NULL)
		return bms_copy(a);

	/*
	 * 在 PostgreSQL 的用法中，空结果非常常见，因此
	 * 值得先调用 bms_nonempty_difference() 做优化。这样
	 * 比起事后再检查，能省掉一轮 palloc/pfree。
	 */
	if (!bms_nonempty_difference(a, b))
		return NULL;

	/* 复制左侧输入 */
	result = bms_copy(a);
	/* 并从结果中去掉 b 的位 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
		result->words[i] &= ~b->words[i];
	/* 不必检查空结果，因为上面已经处理过该情况 */
	return result;
}

/*
 * bms_is_subset - A 是否为 B 的子集？
 */
bool
bms_is_subset(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			longlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return true;			/* 空集是任何集合的子集 */
	if (b == NULL)
		return false;
	/* 检查公共字 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return false;
	}
	/* 检查多余的字 */
	if (a->nwords > b->nwords)
	{
		longlen = a->nwords;
		for (; i < longlen; i++)
		{
			if (a->words[i] != 0)
				return false;
		}
	}
	return true;
}

/*
 * bms_subset_compare - 比较 A 与 B 的相等/子集关系
 *
 * 这比双向调用 bms_is_subset 更高效。
 */
BMS_Comparison
bms_subset_compare(const Bitmapset *a, const Bitmapset *b)
{
	BMS_Comparison result;
	int			shortlen;
	int			longlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
	{
		if (b == NULL)
			return BMS_EQUAL;
		return BMS_SUBSET1;
	}
	if (b == NULL)
		return BMS_SUBSET2;
	/* 检查公共字 */
	result = BMS_EQUAL;			/* 到目前为止的状态 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		bitmapword	aword = a->words[i];
		bitmapword	bword = b->words[i];

		if ((aword & ~bword) != 0)
		{
			/* a 不是 b 的子集 */
			if (result == BMS_SUBSET1)
				return BMS_DIFFERENT;
			result = BMS_SUBSET2;
		}
		if ((bword & ~aword) != 0)
		{
			/* b 不是 a 的子集 */
			if (result == BMS_SUBSET2)
				return BMS_DIFFERENT;
			result = BMS_SUBSET1;
		}
	}
	/* 检查多余的字 */
	if (a->nwords > b->nwords)
	{
		longlen = a->nwords;
		for (; i < longlen; i++)
		{
			if (a->words[i] != 0)
			{
				/* a 不是 b 的子集 */
				if (result == BMS_SUBSET1)
					return BMS_DIFFERENT;
				result = BMS_SUBSET2;
			}
		}
	}
	else if (a->nwords < b->nwords)
	{
		longlen = b->nwords;
		for (; i < longlen; i++)
		{
			if (b->words[i] != 0)
			{
				/* b 不是 a 的子集 */
				if (result == BMS_SUBSET2)
					return BMS_DIFFERENT;
				result = BMS_SUBSET1;
			}
		}
	}
	return result;
}

/*
 * bms_is_member - X 是否为 A 的成员？
 */
bool
bms_is_member(int x, const Bitmapset *a)
{
	int			wordnum,
				bitnum;

	/* XXX：对 x<0 直接返回 false 是否更好？ */
	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return false;
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	if (wordnum >= a->nwords)
		return false;
	if ((a->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
		return true;
	return false;
}

/*
 * bms_member_index
 *		确定成员 x 在位图中从 0 开始的下标
 *
 * 当 x 不是成员时返回 (-1)。
 */
int
bms_member_index(Bitmapset *a, int x)
{
	int			i;
	int			bitnum;
	int			wordnum;
	int			result = 0;
	bitmapword	mask;

	/* 若不是位图成员则返回 -1 */
	if (!bms_is_member(x, a))
		return -1;

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* 统计前面各字中的位数 */
	for (i = 0; i < wordnum; i++)
	{
		bitmapword	w = a->words[i];

		/* 零字中的位不必计数 */
		if (w != 0)
			result += bmw_popcount(w);
	}

	/*
	 * 再加上最后一个字的位，但只计该项之前的位。可以
	 * 先套用掩码，然后再用 popcount。要得到
	 * 从 0 开始的下标，我们只想统计前面的位，不计该项
	 * 本身，因此减去 1。
	 */
	mask = ((bitmapword) 1 << bitnum) - 1;
	result += bmw_popcount(a->words[wordnum] & mask);

	return result;
}

/*
 * bms_overlap - 两个集合是否重叠（即交集非空）？
 */
bool
bms_overlap(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL || b == NULL)
		return false;
	/* 检查公共的字 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		if ((a->words[i] & b->words[i]) != 0)
			return true;
	}
	return false;
}

/*
 * bms_overlap_list - 一个集合是否与整数列表重叠？
 */
bool
bms_overlap_list(const Bitmapset *a, const List *b)
{
	ListCell   *lc;
	int			wordnum,
				bitnum;

	if (a == NULL || b == NIL)
		return false;

	foreach(lc, b)
	{
		int			x = lfirst_int(lc);

		if (x < 0)
			elog(ERROR, "negative bitmapset member not allowed");
		wordnum = WORDNUM(x);
		bitnum = BITNUM(x);
		if (wordnum < a->nwords)
			if ((a->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
				return true;
	}

	return false;
}

/*
 * bms_nonempty_difference - 两个集合的差是否非空？
 *
 * 即 'a' 中是否有成员未同时置位于 'b'。
 */
bool
bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return false;
	if (b == NULL)
		return true;
	/* 检查公共的字 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return true;
	}
	/* 检查 a 中多余的字 */
	for (; i < a->nwords; i++)
	{
		if (a->words[i] != 0)
			return true;
	}
	return false;
}

/*
 * bms_singleton_member - 返回集合中唯一的整数成员
 *
 * 若 |a| 不是 1 则报错。
 */
int
bms_singleton_member(const Bitmapset *a)
{
	int			result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		elog(ERROR, "bitmapset is empty");
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				elog(ERROR, "bitmapset has multiple members");
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	}
	if (result < 0)
		elog(ERROR, "bitmapset is empty");
	return result;
}

/*
 * bms_get_singleton_member
 *
 * 测试给定集合是否为单元素集合。
 * 若是，把 *member 设为该唯一成员的值，并返回 true。
 * 若否，返回 false，且不修改 *member。
 *
 * 若我们不关心区分空集
 * 与多成员集合，这比先调用 bms_membership() 再调用
 * bms_singleton_member() 更方便也更快。
 */
bool
bms_get_singleton_member(const Bitmapset *a, int *member)
{
	int			result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return false;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				return false;
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	}
	if (result < 0)
		return false;
	*member = result;
	return true;
}

/*
 * bms_num_members - 统计集合的成员数
 */
int
bms_num_members(const Bitmapset *a)
{
	int			result = 0;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return 0;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		/* 零字中的位不必计数 */
		if (w != 0)
			result += bmw_popcount(w);
	}
	return result;
}

/*
 * bms_membership - 集合有零个、一个还是多个成员？
 *
 * 这比用 bms_num_members() 做精确计数更快。
 */
BMS_Membership
bms_membership(const Bitmapset *a)
{
	BMS_Membership result = BMS_EMPTY_SET;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return BMS_EMPTY_SET;
	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result != BMS_EMPTY_SET || HAS_MULTIPLE_ONES(w))
				return BMS_MULTIPLE;
			result = BMS_SINGLETON;
		}
	}
	return result;
}

/*
 * bms_is_empty_internal - 集合是否为空？
 *
 * 现在只在本地使用，用来发现某个函数
 * 算出了我们必须丢掉的空集。因此可以
 * 假定输入不是 NULL。
 */
static bool
bms_is_empty_internal(const Bitmapset *a)
{
	int			nwords;
	int			wordnum;

	nwords = a->nwords;
	for (wordnum = 0; wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
			return false;
	}
	return true;
}


/*
 * 这些操作都会 "回收" 其非 const 输入，即要么
 * 返回修改后的输入，要么在无法容纳结果时将其 pfree。
 *
 * 一般应按如下风格使用
 *
 *		foo = bms_add_member(foo, x);
 */


/*
 * bms_add_member - 向集合加入指定成员
 *
 * 输入集合会被修改或回收！
 */
Bitmapset *
bms_add_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return bms_make_singleton(x);
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* 必要时扩大集合 */
	if (wordnum >= a->nwords)
	{
		int			oldnwords = a->nwords;
		int			i;

		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(wordnum + 1));
		a->nwords = wordnum + 1;
		/* 把扩大出来的部分清零 */
		for (i = oldnwords; i < a->nwords; i++)
			a->words[i] = 0;
	}

	a->words[wordnum] |= ((bitmapword) 1 << bitnum);
	return a;
}

/*
 * bms_del_member - 从集合中去掉指定成员
 *
 * 若 x 当前不是集合成员，也不报错
 *
 * 输入集合会被就地修改！
 */
Bitmapset *
bms_del_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	if (x < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	if (a == NULL)
		return NULL;
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	if (wordnum < a->nwords)
		a->words[wordnum] &= ~((bitmapword) 1 << bitnum);
	/* 若算出空结果，必须返回 NULL */
	if (bms_is_empty_internal(a))
	{
		pfree(a);
		return NULL;
	}
	return a;
}

/*
 * bms_add_members - 类似 bms_union，但左侧输入会被回收
 */
Bitmapset *
bms_add_members(Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return a;
	/* 区分较短与较长的输入；必要时复制较长的那个 */
	if (a->nwords < b->nwords)
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	/* 并把较短的输入并入结果 */
	otherlen = other->nwords;
	for (i = 0; i < otherlen; i++)
		result->words[i] |= other->words[i];
	if (result != a)
		pfree(a);
	return result;
}

/*
 * bms_add_range
 *		把 'lower' 到 'upper' 范围内的成员加入集合。
 *
 * 这也可以在循环里调用 bms_add_member 完成，不过
 * 范围较大时用本函数更快，因为我们在
 * 位图字一级而不是位一级工作。
 */
Bitmapset *
bms_add_range(Bitmapset *a, int lower, int upper)
{
	int			lwordnum,
				lbitnum,
				uwordnum,
				ushiftbits,
				wordnum;

	/* 若无需做任何事，则不再进一步检查 */
	if (upper < lower)
		return a;

	if (lower < 0)
		elog(ERROR, "negative bitmapset member not allowed");
	uwordnum = WORDNUM(upper);

	if (a == NULL)
	{
		a = (Bitmapset *) palloc0(BITMAPSET_SIZE(uwordnum + 1));
		a->type = T_Bitmapset;
		a->nwords = uwordnum + 1;
	}
	else if (uwordnum >= a->nwords)
	{
		int			oldnwords = a->nwords;
		int			i;

		/* 确保有足够的字来存放上界位 */
		a = (Bitmapset *) repalloc(a, BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = uwordnum + 1;
		/* 把扩大出来的部分清零 */
		for (i = oldnwords; i < a->nwords; i++)
			a->words[i] = 0;
	}

	wordnum = lwordnum = WORDNUM(lower);

	lbitnum = BITNUM(lower);
	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(upper) + 1);

	/*
	 * 当 lwordnum 与 uwordnum 相同时的特殊情况，必须对
	 * 该字同时做上界与下界掩码。
	 */
	if (lwordnum == uwordnum)
	{
		a->words[lwordnum] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1)
			& (~(bitmapword) 0) >> ushiftbits;
	}
	else
	{
		/* 打开 lbitnum 及其左侧所有位 */
		a->words[wordnum++] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1);

		/* 打开所有中间字的全部位 */
		while (wordnum < uwordnum)
			a->words[wordnum++] = ~(bitmapword) 0;

		/* 打开 upper 的那一位及其右侧所有位。 */
		a->words[uwordnum] |= (~(bitmapword) 0) >> ushiftbits;
	}

	return a;
}

/*
 * bms_int_members - 类似 bms_intersect，但左侧输入会被回收
 */
Bitmapset *
bms_int_members(Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return NULL;
	if (b == NULL)
	{
		pfree(a);
		return NULL;
	}
	/* 把 b 交入 a；从不需要复制 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
		a->words[i] &= b->words[i];
	for (; i < a->nwords; i++)
		a->words[i] = 0;
	/* 若算出空结果，必须返回 NULL */
	if (bms_is_empty_internal(a))
	{
		pfree(a);
		return NULL;
	}
	return a;
}

/*
 * bms_del_members - 类似 bms_difference，但左侧输入会被回收
 */
Bitmapset *
bms_del_members(Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return NULL;
	if (b == NULL)
		return a;
	/* 从 a 中去掉 b 的位；从不需要复制 */
	shortlen = Min(a->nwords, b->nwords);
	for (i = 0; i < shortlen; i++)
		a->words[i] &= ~b->words[i];
	/* 若算出空结果，必须返回 NULL */
	if (bms_is_empty_internal(a))
	{
		pfree(a);
		return NULL;
	}
	return a;
}

/*
 * bms_join - 类似 bms_union，但两个输入都会被回收
 */
Bitmapset *
bms_join(Bitmapset *a, Bitmapset *b)
{
	Bitmapset  *result;
	Bitmapset  *other;
	int			otherlen;
	int			i;

	/* 处理任一输入为 NULL 的情况 */
	if (a == NULL)
		return b;
	if (b == NULL)
		return a;
	/* 区分较短与较长的输入；用较长的那个作为结果 */
	if (a->nwords < b->nwords)
	{
		result = b;
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	/* 并把较短的输入并入结果 */
	otherlen = other->nwords;
	for (i = 0; i < otherlen; i++)
		result->words[i] |= other->words[i];
	if (other != result)		/* 纯粹是防御性检查 */
		pfree(other);
	return result;
}

/*
 * bms_next_member - 找出集合的下一个成员
 *
 * 返回大于 "prevbit" 的最小成员，若没有则返回 -2。
 * "prevbit" 不得小于 -1，否则行为难以预料。
 *
 * 这是为遍历集合成员提供的支持。
 * 典型写法是
 *
 *			x = -1;
 *			while ((x = bms_next_member(inputset, x)) >= 0)
 *				处理成员 x;
 *
 * 注意，没有更多成员时我们返回 -2，而不是你
 * 可能预期的 -1。理由是便于区分
 * 循环尚未开始的状态（x == -1）与循环已结束的状态（x == -2）。
 * 对简单循环用法没有区别，但复杂的迭代逻辑
 * 也许需要这种能力。
 */
int
bms_next_member(const Bitmapset *a, int prevbit)
{
	int			nwords;
	int			wordnum;
	bitmapword	mask;

	if (a == NULL)
		return -2;
	nwords = a->nwords;
	prevbit++;
	mask = (~(bitmapword) 0) << BITNUM(prevbit);
	for (wordnum = WORDNUM(prevbit); wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		/* 忽略 prevbit 之前的位 */
		w &= mask;

		if (w != 0)
		{
			int			result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
			return result;
		}

		/* 在后续字中考虑所有位 */
		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * bms_prev_member - 找出集合的前一个成员
 *
 * 返回小于 "prevbit" 的最大成员，若没有则返回 -2。
 * "prevbit" 不得比 Bitmapset 按当前大小所能
 * 置位的最高位再高一位。
 *
 * 为便于在初始循环中找到最高置位，可以传入特殊的
 * prevbit 值 -1，让函数找出集合中
 * 取值最大的成员。
 *
 * 这是为反向遍历集合成员提供的支持。
 * 典型写法是
 *
 *			x = -1;
 *			while ((x = bms_prev_member(inputset, x)) >= 0)
 *				处理成员 x;
 *
 * 注意，没有更多成员时我们返回 -2，而不是你
 * 可能预期的 -1。理由是便于区分
 * 循环尚未开始的状态（x == -1）与循环已结束的状态（x == -2）。
 * 对简单循环用法没有区别，但复杂的迭代逻辑
 * 也许需要这种能力。
 */

int
bms_prev_member(const Bitmapset *a, int prevbit)
{
	int			wordnum;
	int			ushiftbits;
	bitmapword	mask;

	/*
	 * 若集合为 NULL，或右侧已没有更多位，则
	 * 无事可做。
	 */
	if (a == NULL || prevbit == 0)
		return -2;

	/* 把 -1 变换为我们可能置过的最高位 */
	if (prevbit == -1)
		prevbit = a->nwords * BITS_PER_BITMAPWORD - 1;
	else
		prevbit--;

	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(prevbit) + 1);
	mask = (~(bitmapword) 0) >> ushiftbits;
	for (wordnum = WORDNUM(prevbit); wordnum >= 0; wordnum--)
	{
		bitmapword	w = a->words[wordnum];

		/* 屏蔽掉 prevbit 左侧的位 */
		w &= mask;

		if (w != 0)
		{
			int			result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_leftmost_one_pos(w);
			return result;
		}

		/* 在后续字中考虑所有位 */
		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * bms_hash_value - 计算 Bitmapset 的哈希键
 *
 * 注意：必须保证任意两个 bms_equal() 的位图集
 * 哈希到同一值；实践中这意味着尾部全零
 * 字不得影响结果。因此在调用
 * hash_any() 之前先去掉那些字。
 */
uint32
bms_hash_value(const Bitmapset *a)
{
	int			lastword;

	if (a == NULL)
		return 0;				/* 所有空集的哈希值都是 0 */
	for (lastword = a->nwords; --lastword >= 0;)
	{
		if (a->words[lastword] != 0)
			break;
	}
	if (lastword < 0)
		return 0;				/* 所有空集的哈希值都是 0 */
	return DatumGetUInt32(hash_any((const unsigned char *) a->words,
								   (lastword + 1) * sizeof(bitmapword)));
}

/*
 * bitmap_hash - 键为（指向）Bitmapset 的指针时所用的哈希函数
 *
 * 注意：别忘了把 bitmap_match 指定为匹配函数！
 */
uint32
bitmap_hash(const void *key, Size keysize)
{
	Assert(keysize == sizeof(Bitmapset *));
	return bms_hash_value(*((const Bitmapset *const *) key));
}

/*
 * bitmap_match - 与 bitmap_hash 配合使用的匹配函数
 */
int
bitmap_match(const void *key1, const void *key2, Size keysize)
{
	Assert(keysize == sizeof(Bitmapset *));
	return !bms_equal(*((const Bitmapset *const *) key1),
					  *((const Bitmapset *const *) key2));
}
