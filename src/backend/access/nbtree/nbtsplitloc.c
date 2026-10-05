/*-------------------------------------------------------------------------
 *
 * nbtsplitloc.c
 *	  为 Postgres B-树实现选择分裂点的代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtsplitloc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "storage/lmgr.h"

typedef enum
{
	/* 在物化分裂点列表中搜索所用的策略 */
	SPLIT_DEFAULT,				/* 给截断一些权重 */
	SPLIT_MANY_DUPLICATES,		/* 寻找区分度刚好够的点 */
	SPLIT_SINGLE_VALUE			/* 让左页几乎保持满 */
} FindSplitStrat;

typedef struct
{
	/* 分裂后所留空闲空间的详情 */
	int16		curdelta;		/* 当前 leftfree/rightfree 差值 */
	int16		leftfree;		/* 分裂后左页剩余空间 */
	int16		rightfree;		/* 分裂后右页剩余空间 */

	/* 标识分裂点的字段（由 _bt_findsplitloc 返回） */
	OffsetNumber firstrightoff; /* 右页上的第一个 origpage 项 */
	bool		newitemonleft;	/* 新项放在左侧，还是右侧？ */
} SplitPoint;

typedef struct
{
	/* _bt_recsplitloc 的上下文数据 */
	Relation	rel;			/* 索引关系 */
	Page		origpage;		/* 正在分裂的页 */
	IndexTuple	newitem;		/* 新项（页分裂的原因） */
	Size		newitemsz;		/* newitem 的大小（含行指针） */
	bool		is_leaf;		/* 若正在分裂叶子页则为 T */
	bool		is_rightmost;	/* 若正在分裂该层最右页则为 T */
	OffsetNumber newitemoff;	/* 新项将要插入的位置 */
	int			leftspace;		/* 左页上可供项使用的空间 */
	int			rightspace;		/* 右页上可供项使用的空间 */
	int			olddataitemstotal;	/* 旧项占用的空间 */
	Size		minfirstrightsz;	/* 最小的 firstright 大小 */

	/* 候选分裂点数据 */
	int			maxsplits;		/* 分裂点数量上限 */
	int			nsplits;		/* 当前分裂点数 */
	SplitPoint *splits;			/* 该页的全部候选分裂点 */
	int			interval;		/* 当前可接受分裂点的范围 */
} FindSplitData;

static void _bt_recsplitloc(FindSplitData *state,
							OffsetNumber firstrightoff, bool newitemonleft,
							int olddataitemstoleft,
							Size firstrightofforigpagetuplesz);
static void _bt_deltasortsplits(FindSplitData *state, double fillfactormult,
								bool usemult);
static int	_bt_splitcmp(const void *arg1, const void *arg2);
static bool _bt_afternewitemoff(FindSplitData *state, OffsetNumber maxoff,
								int leaffillfactor, bool *usemult);
static bool _bt_adjacenthtid(ItemPointer lowhtid, ItemPointer highhtid);
static OffsetNumber _bt_bestsplitloc(FindSplitData *state, int perfectpenalty,
									 bool *newitemonleft, FindSplitStrat strategy);
static int	_bt_defaultinterval(FindSplitData *state);
static int	_bt_strategy(FindSplitData *state, SplitPoint *leftpage,
						 SplitPoint *rightpage, FindSplitStrat *strategy);
static void _bt_interval_edges(FindSplitData *state,
							   SplitPoint **leftinterval, SplitPoint **rightinterval);
static inline int _bt_split_penalty(FindSplitData *state, SplitPoint *split);
static inline IndexTuple _bt_split_lastleft(FindSplitData *state,
											SplitPoint *split);
static inline IndexTuple _bt_split_firstright(FindSplitData *state,
											  SplitPoint *split);


/*
 *	_bt_findsplitloc() -- 为页寻找合适的分裂位置。
 *
 * 这里的主要目标是均衡每一张分裂页上
 * 将有的空闲空间，*在计入即将插入的元组之后*。（若没有
 * 计入它，我们可能会发现它需要进入的那一页
 * 空间不够！）
 *
 * 若该页是其所在层的最右页，我们则改为设法安排
 * 让左半分裂页保持 fillfactor% 满。这样，当我们
 * 连续插入递增的键（例如序列、时间戳
 * 等）时，最终得到的树，其各页大约是 fillfactor% 满，
 * 而不是没有这一特例时会得到的 50% 满。
 * 这与 nbtsort.c 为新建的树生成的结果相同。注意
 * 叶子页与非叶页使用不同的 fillfactor。还要注意，
 * 另有若干进一步的特例，其中 fillfactor 并不
 * 按通常方式应用。
 *
 * 传入的是新元组的预定插入位置，表示为
 * 它必须排在其前面的那个元组的偏移号（这可以是
 * maxoff+1，若该元组要放在末尾）。新元组本身也会
 * 传入，因为需要据此权衡后缀截断会有
 * 多有效。实现从一小份候选分裂点中挑出使
 * 后缀截断最为有效的那个，这些候选点
 * 让分裂两侧分到大致相同份额的
 * 空闲空间。相对于均衡空闲空间，后缀截断是次要的，
 * 大量重复的情形除外。
 * 注意，这里总是假定调用者接着会执行截断，
 * 即使对经 pg_upgrade 升级的索引其实并非如此
 * （!heapkeyspace 索引）。关于后缀截断的更多信息，
 * 见 nbtree/README。
 *
 * 我们返回应当进入右页的第一个已有元组的
 * 下标（称为 firstrightoff），外加一个布尔值，
 * 表明新元组放在左页还是右页。可以把
 * 返回状态看成两个相邻数据项 _between_ 的一个点，
 * 这两个数据项（lastleft 与 firstright）位于想象版本的
 * 已经包含 newitem 的 origpage 上。该布尔值用于
 * 消除 firstrightoff == newitemoff 时的歧义（也就是
 * 有时需要判断该分裂的 firstright 元组究竟是
 * newitem，还是 origpage 上偏移 firstrightoff 处的元组）。
 */
OffsetNumber
_bt_findsplitloc(Relation rel,
				 Page origpage,
				 OffsetNumber newitemoff,
				 Size newitemsz,
				 IndexTuple newitem,
				 bool *newitemonleft)
{
	BTPageOpaque opaque;
	int			leftspace,
				rightspace,
				olddataitemstotal,
				olddataitemstoleft,
				perfectpenalty,
				leaffillfactor;
	FindSplitData state;
	FindSplitStrat strategy;
	ItemId		itemid;
	OffsetNumber offnum,
				maxoff,
				firstrightoff;
	double		fillfactormult;
	bool		usemult;
	SplitPoint	leftpage,
				rightpage;

	opaque = BTPageGetOpaque(origpage);
	maxoff = PageGetMaxOffsetNumber(origpage);

	/* B-树页上扣除固定开销后可用的空闲空间总量 */
	leftspace = rightspace =
		PageGetPageSize(origpage) - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));

	/* 右页将拥有与旧页相同的高键 */
	if (!P_RIGHTMOST(opaque))
	{
		itemid = PageGetItemId(origpage, P_HIKEY);
		rightspace -= (int) (MAXALIGN(ItemIdGetLength(itemid)) +
							 sizeof(ItemIdData));
	}

	/* 在真正扫描数据项之前，先累计它们占用的总空间 */
	olddataitemstotal = rightspace - (int) PageGetExactFreeSpace(origpage);
	leaffillfactor = BTGetFillFactor(rel);

	/* 传入的 newitemsz 已 MAXALIGNED，但不包含行指针 */
	newitemsz += sizeof(ItemIdData);
	state.rel = rel;
	state.origpage = origpage;
	state.newitem = newitem;
	state.newitemsz = newitemsz;
	state.is_leaf = P_ISLEAF(opaque);
	state.is_rightmost = P_RIGHTMOST(opaque);
	state.leftspace = leftspace;
	state.rightspace = rightspace;
	state.olddataitemstotal = olddataitemstotal;
	state.minfirstrightsz = SIZE_MAX;
	state.newitemoff = newitemoff;

	/* newitem 无法成为倒排列表项 */
	Assert(!BTreeTupleIsPosting(newitem));

	/*
	 * nsplits 绝不应超过 maxoff，因为候选分裂点至多与
	 * 各项 _between_ 的点一样多；只要
	 * 把新项想象成已经在原页上（最终的
	 * 分裂点数可能略少，因为并不是元组之间的每个点
	 * 都合法）。
	 */
	state.maxsplits = maxoff;
	state.splits = palloc(sizeof(SplitPoint) * state.maxsplits);
	state.nsplits = 0;

	/*
	 * 扫描数据项，并计算在每个可能位置
	 * 做一次分裂时的空间占用
	 */
	olddataitemstoleft = 0;

	for (offnum = P_FIRSTDATAKEY(opaque);
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		Size		itemsz;

		itemid = PageGetItemId(origpage, offnum);
		itemsz = MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);

		/*
		 * 当项偏移号不是 newitemoff 时，分裂的任一侧
		 * 都不会是 newitem。记录原页上前一个数据项之后、
		 * 当前原页数据项之前的一次分裂。
		 * （没有前一项时 _bt_recsplitloc() 会拒绝该分裂，
		 * 我们依赖这一点。）
		 */
		if (offnum < newitemoff)
			_bt_recsplitloc(&state, offnum, false, olddataitemstoleft, itemsz);
		else if (offnum > newitemoff)
			_bt_recsplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
		else
		{
			/*
			 * 记录一次分裂：在所有 "offnum < newitemoff" 的原页
			 * 数据项之后、newitem 之前
			 */
			_bt_recsplitloc(&state, offnum, false, olddataitemstoleft, itemsz);

			/*
			 * 记录一次分裂：在 newitem 之后、偏移 newitemoff/
			 * 当前偏移处的原页数据项之前
			 */
			_bt_recsplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
		}

		olddataitemstoleft += itemsz;
	}

	/*
	 * 记录一次分裂：在所有原页数据项之后、newitem 之前。
	 * （不过仅当 newitem 有可能单独落在新的
	 * 右页上时才记录。）
	 */
	Assert(olddataitemstoleft == olddataitemstotal);
	if (newitemoff > maxoff)
		_bt_recsplitloc(&state, newitemoff, false, olddataitemstotal, 0);

	/*
	 * 我认为不可能找不到可行的分裂，但
	 * 以防万一 ...
	 */
	if (state.nsplits == 0)
		elog(ERROR, "could not find a feasible split point for index \"%s\"",
			 RelationGetRelationName(rel));

	/*
	 * 开始在合法分裂点列表中搜索分裂点。首先
	 * 优先考虑均衡分裂两半各自可用的空闲空间
	 * （一开始使用默认策略），并在适当处应用
	 * 最右页优化以及 "在新项之后分裂" 优化。
	 * 当原分裂点/空间最优分裂点附近
	 * 存在大量重复时，可能需要另外两种
	 * 回退策略之一。
	 *
	 * 默认策略在决定叶子页上的分裂点时，会给后缀截断
	 * 一些权重。它试图选出这样的分裂点：某个区分属性
	 * 在分裂左侧的新高键中更早出现，以便最大化
	 * 可以截掉的尾部属性个数。只有那些意味着两侧
	 * 空闲空间平衡可接受的候选分裂点才会被
	 * 考虑。
	 * 见 _bt_defaultinterval()。
	 */
	if (!state.is_leaf)
	{
		/* fillfactormult 只用于最右页 */
		usemult = state.is_rightmost;
		fillfactormult = BTREE_NONLEAF_FILLFACTOR / 100.0;
	}
	else if (state.is_rightmost)
	{
		/* 最右叶子页 -- fillfactormult 总会用到 */
		usemult = true;
		fillfactormult = leaffillfactor / 100.0;
	}
	else if (_bt_afternewitemoff(&state, maxoff, leaffillfactor, &usemult))
	{
		/*
		 * 新项插在叶子页上一处局部分组的最右位置 -- 应用
		 * "在新项之后分裂" 优化：要么套用叶子页 fillfactor
		 * 乘数，要么选出精确分裂点，使 newitem 成为 lastleft。
		 * （usemult 已为我们设好。）
		 */
		if (usemult)
		{
			/* fillfactormult 应按叶子页 fillfactor 来设置 */
			fillfactormult = leaffillfactor / 100.0;
		}
		else
		{
			/* 寻找 newitemoff 之后的精确分裂点 */
			for (int i = 0; i < state.nsplits; i++)
			{
				SplitPoint *split = state.splits + i;

				if (split->newitemonleft &&
					newitemoff == split->firstrightoff)
				{
					pfree(state.splits);
					*newitemonleft = true;
					return newitemoff;
				}
			}

			/*
			 * 在 newitemoff 之后无法合法分裂；继续做这次分裂，
			 * 但不使用 fillfactor 乘数。这是防御性处理，
			 * 实践中应当永远不会用到。
			 */
			fillfactormult = 0.50;
		}
	}
	else
	{
		/* 其他叶子页。50:50 页分裂。 */
		usemult = false;
		/* fillfactormult 并未使用，但求整齐 */
		fillfactormult = 0.50;
	}

	/*
	 * 在 delta/fillfactormult 排序打乱原来的先后顺序之前，
	 * 先保存该页最左与最右的分裂
	 */
	leftpage = state.splits[0];
	rightpage = state.splits[state.nsplits - 1];

	/* 按 fillfactormult 给分裂点算出 delta，并按 delta 排序 */
	_bt_deltasortsplits(&state, fillfactormult, usemult);

	/* 为默认策略确定分裂区间 */
	state.interval = _bt_defaultinterval(&state);

	/*
	 * 判断默认策略/分裂区间能否产生
	 * 足够有区分度的分裂，或者是否应当更换策略。
	 * 其他策略会改变被视为可接受的分裂点
	 * 范围（分裂区间），并可能改变
	 * fillfactormult，以便妥善处理含有大量
	 * 重复的页。
	 *
	 * 传入整页的低分裂与高分裂（实际上，它们针对的是
	 * 已经包含 newitem 的该页想象版本）。当初始分裂区间
	 * 包住全是重复的分裂点时会用到它们，此时还需要考虑
	 * 是否还有可能避免
	 * 追加堆 TID。
	 */
	perfectpenalty = _bt_strategy(&state, &leftpage, &rightpage, &strategy);

	if (strategy == SPLIT_DEFAULT)
	{
		/*
		 * 默认策略成功了（对内部页总是成功）。
		 * 原来的分裂区间仍然有效。
		 */
	}

	/*
	 * 多重复策略用于这种情形：否则就会追加堆 TID，
	 * 但该页又并非完全充满逻辑重复。
	 *
	 * 分裂区间被放宽，以包含全部合法候选分裂点。
	 * 整页分裂区间中的不同值可能少到只有两个，
	 * 不过页上大部分值也可能各自唯一。最终分裂点
	 * 要么紧挨在那组重复元组的左侧，
	 * 要么紧挨在其右侧。这一组包住首个/delta 最优分裂点
	 * （完美惩罚已设置成：会选出 delta 最低、
	 * 且能避免追加堆 TID 的那个分裂点）。
	 * 最大化可以截掉的属性个数，
	 * 并不是多重复策略的目标。
	 * 这一策略并不追求把更多属性截掉。
	 *
	 * 当无法避免追加堆 TID 时，使用单值策略。
	 * 它安排让左页非常满。当相同属性值的元组
	 * 跨越许多页时，这样可以最大化空间利用率。
	 * 新插入的重复往往具有更大的堆 TID 值，因此我们会
	 * 始终向右侧分裂。
	 * （单值策略虽然无害，但对 !heapkeyspace 索引而言
	 * 并不是特别有用。）
	 */
	else if (strategy == SPLIT_MANY_DUPLICATES)
	{
		Assert(state.is_leaf);
		/* 不应试图截掉额外的用户属性 */
		Assert(perfectpenalty ==
			   IndexRelationGetNumberOfKeyAttributes(state.rel));
		/* 不必重新排序分裂点 -- fillfactormult/delta 没有变化 */
		state.interval = state.nsplits;
	}
	else if (strategy == SPLIT_SINGLE_VALUE)
	{
		Assert(state.is_leaf);
		/* 在靠近页尾处分裂 */
		usemult = true;
		fillfactormult = BTREE_SINGLEVAL_FILLFACTOR / 100.0;
		/* 用新的 delta 重新排序分裂点 */
		_bt_deltasortsplits(&state, fillfactormult, usemult);
		/* 追加堆 TID 不可避免，因此区间取 1 即可 */
		state.interval = 1;
	}

	/*
	 * 在可接受的分裂点中（使用最终分裂区间）搜索
	 * 惩罚最低、因而预期能最大化扇出的那一项。
	 * 会为我们设置 *newitemonleft。
	 */
	firstrightoff = _bt_bestsplitloc(&state, perfectpenalty, newitemonleft,
									 strategy);
	pfree(state.splits);

	return firstrightoff;
}

/*
 * 子例程：把页上两个元组之间的某个特定点记下来
 * （其中之一可能是新项）。也就是把 firstrightoff 与 newitemonleft
 * 的组合写入 *state，供稍后分析。这里也便于检查
 * 该分裂是否合法（若不合法，就不会被记下）。
 *
 * firstrightoff 是原页上进入右页的第一个项的偏移，
 * firstrightofforigpagetuplesz 是该元组的大小。
 * firstrightoff 可以大于最大偏移，这表示所有旧项
 * 都进入左页，只有新项进入右页。
 * 那种情况下我们实际上并不使用 firstrightofforigpagetuplesz
 * （实际上，对 firstright 元组恰好是 newitem 的 _any_ 分裂，
 * 我们都不用它）。
 *
 * olddataitemstoleft 是此处在合法时被记下的分裂点左侧
 * 全部旧项的总大小。不应包含 newitemsz，
 * 因为那部分在这里处理。
 */
static void
_bt_recsplitloc(FindSplitData *state,
				OffsetNumber firstrightoff,
				bool newitemonleft,
				int olddataitemstoleft,
				Size firstrightofforigpagetuplesz)
{
	int16		leftfree,
				rightfree;
	Size		firstrightsz;
	Size		postingsz = 0;
	bool		newitemisfirstright;

	/* 新项会成为该分裂点的 firstright 元组吗？ */
	newitemisfirstright = (firstrightoff == state->newitemoff &&
						   !newitemonleft);

	if (newitemisfirstright)
		firstrightsz = state->newitemsz;
	else
	{
		firstrightsz = firstrightofforigpagetuplesz;

		/*
		 * 当 firstright 元组是倒排列表元组时，计算后缀截断省下的空间，
		 * 不过仅当该元组超过 64 字节（含行指针开销，阈值任意）时
		 * 才计算。这样可以避免在倒排列表必定很小的情况下
		 * 去访问该元组（如果它真有倒排列表的话，
		 * 那个列表也一定很短）。
		 *
		 * 注意：firstright 元组就是 newitem 时不这样做，
		 * 因为 newitem 无法带有倒排列表。
		 */
		if (state->is_leaf && firstrightsz > 64)
		{
			ItemId		itemid;
			IndexTuple	newhighkey;

			itemid = PageGetItemId(state->origpage, firstrightoff);
			newhighkey = (IndexTuple) PageGetItem(state->origpage, itemid);

			if (BTreeTupleIsPosting(newhighkey))
				postingsz = IndexTupleSize(newhighkey) -
					BTreeTupleGetPostingOffset(newhighkey);
		}
	}

	/* 计入所有旧元组 */
	leftfree = state->leftspace - olddataitemstoleft;
	rightfree = state->rightspace -
		(state->olddataitemstotal - olddataitemstoleft);

	/*
	 * 右页上的第一项会成为左页的高键；因此它
	 * 既占用左侧空间，也占用右侧空间（我们无法假定
	 * 后缀截断会让它变小）。当索引带有 INCLUDE 属性时，
	 * 左页高键中的那些属性会被截掉，使该页
	 * 略多一点空闲空间。不过这不应影响我们找到
	 * 合法分裂位置的能力，因为我们在左半空闲空间上
	 * 宁可估计得更悲观。此外，即使非 TID 属性
	 * 发生了后缀截断，新高键往往甚至不会
	 * 比它所依据的 firstright 元组更小，
	 * 连单独一个 MAXALIGN() 对齐单位
	 * 都常常省不下来。
	 *
	 * 若我们处在叶层，则假定在叶层分裂时，后缀截断
	 * 无法避免把堆 TID 加进左半的新高键。
	 * 实践中新高键常常更小，很少更大，
	 * 但我们保守地假定最坏情况。我们确实会费心
	 * 减去倒排列表的开销，不过仅当看起来
	 * 会带来明显差别时才这样做。
	 * （倒排列表是截断通常会使最终高键远小于
	 * firstright 的唯一情形，因此在那里更精确一些
	 * 会明显改善空闲空间的平衡。）
	 */
	if (state->is_leaf)
		leftfree -= (int16) (firstrightsz +
							 MAXALIGN(sizeof(ItemPointerData)) -
							 postingsz);
	else
		leftfree -= (int16) firstrightsz;

	/* 计入新项 */
	if (newitemonleft)
		leftfree -= (int16) state->newitemsz;
	else
		rightfree -= (int16) state->newitemsz;

	/*
	 * 若我们不在叶层，就可以丢弃最终落在右页上的
	 * 第一项的键数据。
	 */
	if (!state->is_leaf)
		rightfree += (int16) firstrightsz -
			(int16) (MAXALIGN(sizeof(IndexTupleData)) + sizeof(ItemIdData));

	/* 若合法则记录该分裂 */
	if (leftfree >= 0 && rightfree >= 0)
	{
		Assert(state->nsplits < state->maxsplits);

		/* 在合法分裂中确定最小的 firstright 元组大小 */
		state->minfirstrightsz = Min(state->minfirstrightsz, firstrightsz);

		state->splits[state->nsplits].curdelta = 0;
		state->splits[state->nsplits].leftfree = leftfree;
		state->splits[state->nsplits].rightfree = rightfree;
		state->splits[state->nsplits].firstrightoff = firstrightoff;
		state->splits[state->nsplits].newitemonleft = newitemonleft;
		state->nsplits++;
	}
}

/*
 * 子例程：按当前 fillfactor 给物化的候选分裂点数组
 * 指定空间 delta，并据此 fillfactor 对数组排序
 */
static void
_bt_deltasortsplits(FindSplitData *state, double fillfactormult,
					bool usemult)
{
	for (int i = 0; i < state->nsplits; i++)
	{
		SplitPoint *split = state->splits + i;
		int16		delta;

		if (usemult)
			delta = fillfactormult * split->leftfree -
				(1.0 - fillfactormult) * split->rightfree;
		else
			delta = split->leftfree - split->rightfree;

		if (delta < 0)
			delta = -delta;

		/* 保存 delta */
		split->curdelta = delta;
	}

	qsort(state->splits, state->nsplits, sizeof(SplitPoint), _bt_splitcmp);
}

/*
 * _bt_deltasortsplits() 使用的 qsort 风格比较函数
 */
static int
_bt_splitcmp(const void *arg1, const void *arg2)
{
	SplitPoint *split1 = (SplitPoint *) arg1;
	SplitPoint *split2 = (SplitPoint *) arg2;

	if (split1->curdelta > split2->curdelta)
		return 1;
	if (split1->curdelta < split2->curdelta)
		return -1;

	return 0;
}

/*
 * 子例程：判断非最右叶子页是否应当
 * 紧接在新到元组于原页上本应占据的偏移之后分裂
 * （或者当新项位于原页右侧时，是否应套用叶子页 fillfactor）。
 * 当复合索引上存在局部单调递增的插入模式，
 * 前导属性值形成局部分组，并且我们预计
 * 不久的将来还会插入同一分组、也就是当前分组
 * （新项所属的分组）时，这样做是合适的。
 * 可以把这看成最右叶子页分裂时套用叶子页 fillfactor
 * 的一种变体，因为受益的情形会随着时间
 * 收敛到把叶子页填满到 leaffillfactor%
 * 的程度。
 *
 * 我们可能会在某个 "最重要列" 元组分组的最右页上
 * 留下额外空闲空间，如果该分组最终并没有后续插入
 * 来用掉这些空间。这种效应是自我限制的；
 * 以后若有一个分组成为受影响分组的 "右侧最近" 分组，
 * 它通常会把多出来的空闲空间
 * 好好利用起来。
 *
 * 本例程返回 true 时，调用者采用该优化，不过具体动作
 * 因调用者而异。若这里同时把 *usemult 设为 true，
 * 调用者按标准方式使用原来的叶子页 fillfactor，
 * 而不是直接拿新项偏移来用。否则，调用者通过
 * 找出使新元组成为该分裂 lastleft 元组的
 * 合法分裂点来应用优化。
 */
static bool
_bt_afternewitemoff(FindSplitData *state, OffsetNumber maxoff,
					int leaffillfactor, bool *usemult)
{
	int16		nkeyatts;
	ItemId		itemid;
	IndexTuple	tup;
	int			keepnatts;

	Assert(state->is_leaf && !state->is_rightmost);

	nkeyatts = IndexRelationGetNumberOfKeyAttributes(state->rel);

	/* 这里不考虑单键索引 */
	if (nkeyatts == 1)
		return false;

	/* 当新项是第一项时，从不推断为升序插入模式 */
	if (state->newitemoff == P_FIRSTKEY)
		return false;

	/*
	 * 只在元组等宽的页上应用该优化，因为有序键
	 * 很可能是定宽的。直接检测新元组是不是变宽也许也行，
	 * 但那样就无法把该优化用到带有
	 * numeric_ops 属性的索引上。
	 *
	 * 当新项与遍历该页时观察到的最小项同宽，
	 * 并且其他非枢轴元组也必须同宽时，就断定该页元组等宽。
	 * （注意，可能已被截断的现有高键并不计入
	 * olddataitemstotal，并且必须从 maxoff 中
	 * 把它减掉。）
	 */
	if (state->newitemsz != state->minfirstrightsz)
		return false;
	if (state->newitemsz * (maxoff - 1) != state->olddataitemstotal)
		return false;

	/*
	 * 当元组宽于由两个非 NULL int8/int64 属性
	 * （或四个非 NULL int4/int32 属性）组成的元组时，
	 * 避免应用该优化
	 */
	if (state->newitemsz >
		MAXALIGN(sizeof(IndexTupleData) + sizeof(int64) * 2) +
		sizeof(ItemIdData))
		return false;

	/*
	 * 要应用该优化，至少第一个属性的值必须等于
	 * 前一个元组中的对应值。新项也无法成为
	 * 重复项。
	 *
	 * 处理新项位于现有页上全部项右侧的情形。
	 * 这本身就暗示单调递增插入，因此这里
	 * 不应用 "堆 TID 邻接" 测试。
	 */
	if (state->newitemoff > maxoff)
	{
		itemid = PageGetItemId(state->origpage, maxoff);
		tup = (IndexTuple) PageGetItem(state->origpage, itemid);
		keepnatts = _bt_keep_natts_fast(state->rel, tup, state->newitem);

		if (keepnatts > 1 && keepnatts <= nkeyatts)
		{
			*usemult = true;
			return true;
		}

		return false;
	}

	/*
	 * "前导列基数低、后缀列基数高" 的索引若带有随机插入模式
	 * （例如带布尔列的索引，如
	 * '(book_is_in_print, book_isbn)' 上的索引），会让我们面临
	 * 持续误用该优化的风险。我们愿意
	 * 接受非常偶尔的误用，只要出错的情形
	 * 少见并且自我限制。
	 *
	 * 堆 TID 邻接强烈暗示紧挨左侧的项是刚刚插入的，
	 * 这就限制了该优化被用得过宽。此外，这里触发的
	 * 所有不适当情形，平均仍会
	 * 在页的中部分裂。
	 */
	itemid = PageGetItemId(state->origpage, OffsetNumberPrev(state->newitemoff));
	tup = (IndexTuple) PageGetItem(state->origpage, itemid);
	/* 先做更便宜的测试 */
	if (BTreeTupleIsPosting(tup) ||
		!_bt_adjacenthtid(&tup->t_tid, &state->newitem->t_tid))
		return false;
	/* 同时检查与最右项情形相同的条件 */
	keepnatts = _bt_keep_natts_fast(state->rel, tup, state->newitem);

	if (keepnatts > 1 && keepnatts <= nkeyatts)
	{
		double		interp = (double) state->newitemoff / ((double) maxoff + 1);
		double		leaffillfactormult = (double) leaffillfactor / 100.0;

		/*
		 * 若在新项之后分裂会得到一个比叶子页 fillfactor 分裂
		 * 所用位置更靠右的分裂点，就不要让调用者那样做 --
		 * 改为让调用者套用叶子页 fillfactor
		 */
		*usemult = interp > leaffillfactormult;

		return true;
	}

	return false;
}

/*
 * 子例程：判断两个堆 TID 是否 "相邻"。
 *
 * 相邻意味着高 TID 非常可能是在低 TID 之后
 * 立即插入堆关系的，大概就在当前
 * 事务期间。
 */
static bool
_bt_adjacenthtid(ItemPointer lowhtid, ItemPointer highhtid)
{
	BlockNumber lowblk,
				highblk;

	lowblk = ItemPointerGetBlockNumber(lowhtid);
	highblk = ItemPointerGetBlockNumber(highhtid);

	/* 堆块号相同时，乐观地假定它们相邻 */
	if (lowblk == highblk)
		return true;

	/* 堆块号刚好大 1 时，第二个偏移应为 FirstOffsetNumber */
	if (lowblk + 1 == highblk &&
		ItemPointerGetOffsetNumber(highhtid) == FirstOffsetNumber)
		return true;

	return false;
}

/*
 * 子例程，用来在候选分裂点中找出 "最佳" 分裂点。
 * 最佳分裂点是落在当前/最终分裂区间内、
 * 惩罚最低的分裂点。惩罚是抽象分数，
 * 定义随分裂的是叶子页还是内部页而变化。
 * 详见 _bt_split_penalty()
 * 中的说明。
 *
 * 假定 "perfectpenalty" 是各候选分裂点里可能的最低惩罚。
 * 于是当继续算下去显然无法改进我们的选择时，
 * 就可以提早返回，不必为每个候选分裂白费循环去算
 * 第一个不同的属性（也适用于我们只想要
 * 区分度刚好够的分裂点、而不希望分裂
 * 比必要的更不平衡的情形）。
 *
 * 我们返回应当放到右页上的第一个已有元组的下标，
 * 外加一个布尔值，表示新项是否位于分裂点左侧。
 */
static OffsetNumber
_bt_bestsplitloc(FindSplitData *state, int perfectpenalty,
				 bool *newitemonleft, FindSplitStrat strategy)
{
	int			bestpenalty,
				lowsplit;
	int			highsplit = Min(state->interval, state->nsplits);
	SplitPoint *final;

	bestpenalty = INT_MAX;
	lowsplit = 0;
	for (int i = lowsplit; i < highsplit; i++)
	{
		int			penalty;

		penalty = _bt_split_penalty(state, state->splits + i);

		if (penalty < bestpenalty)
		{
			bestpenalty = penalty;
			lowsplit = i;
		}

		if (penalty <= perfectpenalty)
			break;
	}

	final = &state->splits[lowsplit];

	/*
	 * 存在一种风险："多重复" 策略会一而再地做错事，
	 * 当一大组重复的右侧有单调递减的插入时就是如此。
	 * 反复分裂可能留下一连串右半页，
	 * 其上的空闲空间永远无法再被使用。
	 * 这种情况必须避免。
	 *
	 * 考虑一个例子：单个整数属性且为 NULLS FIRST 的索引，
	 * 其最左页几乎被 NULLs 填满。单调递减的
	 * 整数插入可能让同一张最左页在同一点反复分裂。
	 * 每一次分裂的新高键都来自紧挨这一大组 NULLs
	 * 右侧当前最小的那个值，而该值永远高于
	 * 今后全部整数插入，于是今后全部整数插入
	 * 都会被引导到同一张
	 * 最左页。
	 */
	if (strategy == SPLIT_MANY_DUPLICATES && !state->is_rightmost &&
		!final->newitemonleft && final->firstrightoff >= state->newitemoff &&
		final->firstrightoff < state->newitemoff + 9)
	{
		/*
		 * 当新项刚好在本应采用的 "多重复" 分裂点右侧时，
		 * 通过执行 50:50 分裂来避免该问题。
		 * （注意，用来判断插入是否 "刚好在分裂点右侧"
		 * 的测试是保守的。）
		 */
		final = &state->splits[0];
	}

	*newitemonleft = final->newitemonleft;
	return final->firstrightoff;
}

#define LEAF_SPLIT_DISTANCE			0.050
#define INTERNAL_SPLIT_DISTANCE		0.075

/*
 * 返回默认策略使用的分裂区间。这是对
 * 进一步考虑的候选分裂点数量的限制。
 * 只有全体候选分裂点中的一部分（位于
 * 现已排序的 splits 数组开头的那些）落在分裂区间内。
 * 分裂区间在 _bt_bestsplitloc() 内应用。
 *
 * 分裂区间表示可接受的分裂点范围 -- 也就是
 * leftfree 与 rightfree 平衡程度可接受的那些。最终
 * 选定的分裂点，是该分裂区间内 "惩罚" 最低的分裂点
 * （除非我们更换整个策略，那种情况下区间也会变 --
 * 见 _bt_strategy()）。
 *
 * "Prefix B-Trees" 论文把叶子分裂的分裂区间称为 sigma l，
 * 把内部（"branch"）分裂的称为 sigma b。很难为分裂区间的
 * 大小给出理论上的理由，不过显然，较小的分裂区间
 * 能使 L+1 层上的元组平均小得多，
 * 又不会明显影响 L 层的空间利用率。
 * （注意，若后缀截断学会在最后一个
 * 属性/datum "within" 截断元组，例如 text 这类数据类型，
 * 我们计算分裂区间的方式可能就需要改变；论文里大体就是
 * 这么假定它工作的。）
 */
static int
_bt_defaultinterval(FindSplitData *state)
{
	SplitPoint *spaceoptimal;
	int16		tolerance,
				lowleftfree,
				lowrightfree,
				highleftfree,
				highrightfree;

	/*
	 * 确定我们愿意容忍的、偏高和偏低的 leftfree 与
	 * rightfree 值。注意，在叶子页上所有非枢轴元组
	 * （数据项）宽度均匀的常见情形下，最终分裂区间大约是
	 * nsplits 的 10%。分裂内部页时
	 * 我们稍微更激进一些。
	 */
	if (state->is_leaf)
		tolerance = state->olddataitemstotal * LEAF_SPLIT_DISTANCE;
	else
		tolerance = state->olddataitemstotal * INTERNAL_SPLIT_DISTANCE;

	/* 第一个候选分裂点是最均衡的 */
	spaceoptimal = state->splits;
	lowleftfree = spaceoptimal->leftfree - tolerance;
	lowrightfree = spaceoptimal->rightfree - tolerance;
	highleftfree = spaceoptimal->leftfree + tolerance;
	highrightfree = spaceoptimal->rightfree + tolerance;

	/*
	 * 从紧接 'spaceoptimal' 之后的分裂开始遍历分裂点。
	 * 找出第一个把空闲空间分得过于不均、以至于把它纳入
	 * 分裂区间会不可接受的分裂点。
	 */
	for (int i = 1; i < state->nsplits; i++)
	{
		SplitPoint *split = state->splits + i;

		/* 这里无法使用 curdelta，因为它的值常常经过加权 */
		if (split->leftfree < lowleftfree || split->rightfree < lowrightfree ||
			split->leftfree > highleftfree || split->rightfree > highrightfree)
			return i;
	}

	return state->nsplits;
}

/*
 * 子例程：决定分裂应当使用默认策略/初始
 * 分裂区间，还是应当用其他策略
 * 来结束该页的分裂（这只对叶子页有可能）。
 *
 * 调用者根据这里对 *strategy 的设置，采用其他策略
 * （或坚持默认策略）。返回值是 "perfect penalty"，
 * 它作为最终约束传给 _bt_bestsplitloc()，限制调用者在使用
 * 多重复策略时，为避免追加堆 TID 愿意走多远
 * （这也省下 _bt_bestsplitloc() 的无用循环）。
 */
static int
_bt_strategy(FindSplitData *state, SplitPoint *leftpage,
			 SplitPoint *rightpage, FindSplitStrat *strategy)
{
	IndexTuple	leftmost,
				rightmost;
	SplitPoint *leftinterval,
			   *rightinterval;
	int			perfectpenalty;
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(state->rel);

	/* 暂时假定不会使用其他策略 */
	*strategy = SPLIT_DEFAULT;

	/*
	 * 在内部页上，用整页（实际上是包含 newitem 的
	 * 整页想象版本）上观察到的最小 firstright 项大小
	 * 作为完美惩罚。在多数或全部分裂（不只是区间内的
	 * 分裂）的 firstright 元组大小都相同这一常见情形下，
	 * 这能省下一些循环。
	 */
	if (!state->is_leaf)
		return state->minfirstrightsz;

	/*
	 * 使用当前分裂区间中最左分裂与最右分裂上的
	 * 最左元组与最右元组
	 */
	_bt_interval_edges(state, &leftinterval, &rightinterval);
	leftmost = _bt_split_lastleft(state, leftinterval);
	rightmost = _bt_split_firstright(state, rightinterval);

	/*
	 * 若初始分裂区间能产生至少一个在新高键中
	 * 避免追加堆 TID 的分裂点，就完成了。用默认策略和
	 * 初始分裂区间结束这次分裂。
	 */
	perfectpenalty = _bt_keep_natts_fast(state->rel, leftmost, rightmost);
	if (perfectpenalty <= indnkeyatts)
		return perfectpenalty;

	/*
	 * 当即使初始/默认分裂区间的 "完美" 惩罚也表明该区间
	 * 连一个能避免追加堆 TID 的分裂都不包含时，
	 * 算出调用者应如何结束分裂。
	 *
	 * 用最左分裂的 lastleft 元组和最右分裂的
	 * firstright 元组来评估每一种可能的分裂。
	 */
	leftmost = _bt_split_lastleft(state, leftpage);
	rightmost = _bt_split_firstright(state, rightpage);

	/*
	 * 若该页（包括新项）有许多重复，但并非完全充满重复，
	 * 就会执行多重复策略分裂。
	 * 若该页完全充满重复，就会执行
	 * 单值策略分裂。
	 */
	perfectpenalty = _bt_keep_natts_fast(state->rel, leftmost, rightmost);
	if (perfectpenalty <= indnkeyatts)
	{
		*strategy = SPLIT_MANY_DUPLICATES;

		/*
		 * 多重复策略应当在包住 delta 最优分裂点的
		 * 那组重复的任一侧分裂。返回
		 * indnkeyatts，而不是真正的完美惩罚，以便
		 * 做到这一点。（若这里返回 perfectpenalty，低基数
		 * 复合索引就可能持续出现不平衡的分裂。）
		 *
		 * 注意，在少数看起来分裂点紧右侧存在
		 * 不断变小的插入的情形下，调用者不会真的做
		 * 多重复分裂。这必须发生在最终决定之前，
		 * 就在 _bt_bestsplitloc() 内部。
		 */
		return indnkeyatts;
	}

	/*
	 * 单值策略只适合堆 TIDs 不断增大的情形；否则继续
	 * 采用原来的默认策略分裂，以避免病态的性能。
	 * 用页高键推断这是否为存放同一重复值的那些页中
	 * 的最右页。略微偏离顺序的堆 TIDs 插入
	 * 仍应能使用单值策略，因为对同一重复值的
	 * 并发插入者来说，这种情况
	 * 属于预期之中。
	 */
	else if (state->is_rightmost)
		*strategy = SPLIT_SINGLE_VALUE;
	else
	{
		ItemId		itemid;
		IndexTuple	hikey;

		itemid = PageGetItemId(state->origpage, P_HIKEY);
		hikey = (IndexTuple) PageGetItem(state->origpage, itemid);
		perfectpenalty = _bt_keep_natts_fast(state->rel, hikey,
											 state->newitem);
		if (perfectpenalty <= indnkeyatts)
			*strategy = SPLIT_SINGLE_VALUE;
		else
		{
			/*
			 * 让调用者改用默认策略来结束分裂，因为该页看起来
			 * 并不是它所充满的那个重复值
			 * 在各页之中的最右页
			 */
		}
	}

	return perfectpenalty;
}

/*
 * 子例程：定位当前/默认分裂区间的最左与最右分裂。
 * 注意，当且仅当区间里只有一次分裂时，
 * 二者就是同一次分裂。
 */
static void
_bt_interval_edges(FindSplitData *state, SplitPoint **leftinterval,
				   SplitPoint **rightinterval)
{
	int			highsplit = Min(state->interval, state->nsplits);
	SplitPoint *deltaoptimal;

	deltaoptimal = state->splits;
	*leftinterval = NULL;
	*rightinterval = NULL;

	/*
	 * delta 是到最优分裂点的绝对距离，因此最左分裂点
	 * 和最右分裂点通常都位于
	 * 数组的末尾
	 */
	for (int i = highsplit - 1; i >= 0; i--)
	{
		SplitPoint *distant = state->splits + i;

		if (distant->firstrightoff < deltaoptimal->firstrightoff)
		{
			if (*leftinterval == NULL)
				*leftinterval = distant;
		}
		else if (distant->firstrightoff > deltaoptimal->firstrightoff)
		{
			if (*rightinterval == NULL)
				*rightinterval = distant;
		}
		else if (!distant->newitemonleft && deltaoptimal->newitemonleft)
		{
			/*
			 * "即将进入的元组将成为 firstright"（distant）位于
			 * "即将进入的元组将成为 lastleft"（delta 最优）的左侧
			 */
			Assert(distant->firstrightoff == state->newitemoff);
			if (*leftinterval == NULL)
				*leftinterval = distant;
		}
		else if (distant->newitemonleft && !deltaoptimal->newitemonleft)
		{
			/*
			 * "即将进入的元组将成为 lastleft"（distant）位于
			 * "即将进入的元组将成为 firstright"（delta 最优）的右侧
			 */
			Assert(distant->firstrightoff == state->newitemoff);
			if (*rightinterval == NULL)
				*rightinterval = distant;
		}
		else
		{
			/* 初始分裂区间里只有一次或两次分裂 */
			Assert(distant == deltaoptimal);
			if (*leftinterval == NULL)
				*leftinterval = distant;
			if (*rightinterval == NULL)
				*rightinterval = distant;
		}

		if (*leftinterval && *rightinterval)
			return;
	}

	Assert(false);
}

/*
 * 子例程：求调用者候选分裂点的惩罚。
 *
 * 在叶子页上，惩罚是区分分裂两侧的属性号。
 * 它是左页新高键必须包含的最后一个属性。
 * 当截断期间需要追加堆 TID 时，它可以大于
 * 键属性的个数。
 *
 * 在内部页上，惩罚就是该分裂的 firstright 元组大小
 * （含行指针开销）。这个元组将成为
 * 左页的新高键。
 */
static inline int
_bt_split_penalty(FindSplitData *state, SplitPoint *split)
{
	IndexTuple	lastleft;
	IndexTuple	firstright;

	if (!state->is_leaf)
	{
		ItemId		itemid;

		if (!split->newitemonleft &&
			split->firstrightoff == state->newitemoff)
			return state->newitemsz;

		itemid = PageGetItemId(state->origpage, split->firstrightoff);

		return MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);
	}

	lastleft = _bt_split_lastleft(state, split);
	firstright = _bt_split_firstright(state, split);

	return _bt_keep_natts_fast(state->rel, lastleft, firstright);
}

/*
 * 子例程：取得某分裂点的 lastleft IndexTuple
 */
static inline IndexTuple
_bt_split_lastleft(FindSplitData *state, SplitPoint *split)
{
	ItemId		itemid;

	if (split->newitemonleft && split->firstrightoff == state->newitemoff)
		return state->newitem;

	itemid = PageGetItemId(state->origpage,
						   OffsetNumberPrev(split->firstrightoff));
	return (IndexTuple) PageGetItem(state->origpage, itemid);
}

/*
 * 子例程：取得某分裂点的 firstright IndexTuple
 */
static inline IndexTuple
_bt_split_firstright(FindSplitData *state, SplitPoint *split)
{
	ItemId		itemid;

	if (!split->newitemonleft && split->firstrightoff == state->newitemoff)
		return state->newitem;

	itemid = PageGetItemId(state->origpage, split->firstrightoff);
	return (IndexTuple) PageGetItem(state->origpage, itemid);
}
