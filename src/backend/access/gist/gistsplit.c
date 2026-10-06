/*-------------------------------------------------------------------------
 *
 * gistsplit.c
 *	  多列页分裂算法
 *
 * 本文件关注如何在多列 GiST 索引中做出良好的页分裂决策。
 * 操作符类专用的 picksplit 函数只能基于单一列给出答案。我们先对第 1 列
 * 运行 picksplit 函数；然后，若还有更多列，我们检查是否有元组就第 1 列
 * 的分裂而言是"无所谓"的
 * （即可以放到任一侧而不增加额外惩罚）。若有，
 * 我们尝试按下一列重新分配这些元组。
 * 重复直到用尽所有列。
 *
 *
 * gistSplitByKey() 是本文件的入口点。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistsplit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "utils/rel.h"

typedef struct
{
	OffsetNumber *entries;
	int			len;
	Datum	   *attr;
	bool	   *isnull;
	bool	   *dontcare;
} GistSplitUnion;


/*
 * 对 gsvp->entries[] 中列出的 itvec[] 条目形成子键的联合，
 * 忽略 gsvp->dontcare[] 中标记的任何元组。gistunionsubkey
 * 的子例程。
 */
static void
gistunionsubkeyvec(GISTSTATE *giststate, IndexTuple *itvec,
				   GistSplitUnion *gsvp)
{
	IndexTuple *cleanedItVec;
	int			i,
				cleanedLen = 0;

	cleanedItVec = (IndexTuple *) palloc(sizeof(IndexTuple) * gsvp->len);

	for (i = 0; i < gsvp->len; i++)
	{
		if (gsvp->dontcare && gsvp->dontcare[gsvp->entries[i]])
			continue;

		cleanedItVec[cleanedLen++] = itvec[gsvp->entries[i] - 1];
	}

	gistMakeUnionItVec(giststate, cleanedItVec, cleanedLen,
					   gsvp->attr, gsvp->isnull);

	pfree(cleanedItVec);
}

/*
 * 页分裂后重新计算左右两侧子键的联合，
 * 忽略 spl->spl_dontcare[] 中标记的任何元组。
 *
 * 注意：我们总是为所有索引 column 重新计算联合键。在某些情况下
 * 这对最左侧的列可能是重复工作，但假定"移动元组的惩罚为零"
 * 就意味着"联合键完全不变"是不安全的。惩罚函数并非
 * 百分之百准确。
 */
static void
gistunionsubkey(GISTSTATE *giststate, IndexTuple *itvec, GistSplitVector *spl)
{
	GistSplitUnion gsvp;

	gsvp.dontcare = spl->spl_dontcare;

	gsvp.entries = spl->splitVector.spl_left;
	gsvp.len = spl->splitVector.spl_nleft;
	gsvp.attr = spl->spl_lattr;
	gsvp.isnull = spl->spl_lisnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp);

	gsvp.entries = spl->splitVector.spl_right;
	gsvp.len = spl->splitVector.spl_nright;
	gsvp.attr = spl->spl_rattr;
	gsvp.isnull = spl->spl_risnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp);
}

/*
 * 查找"无所谓"元组，即可以零惩罚地移到分裂的另一侧，
 * 就 attno 列
 * 而言。
 *
 * 无所谓元组通过把 spl->spl_dontcare[] 中对应项设为
 * "true" 来标记。调用者必须已将该数组初始化
 * 为零。
 *
 * 返回找到的无所谓项数量。
 */
static int
findDontCares(Relation r, GISTSTATE *giststate, GISTENTRY *valvec,
			  GistSplitVector *spl, int attno)
{
	int			i;
	GISTENTRY	entry;
	int			NumDontCare = 0;

	/*
	 * 首先搜索左侧元组，看是否有加入右侧联合键时
	 * 惩罚为零的。
	 *
	 * attno 列已知全部非空（见 gistSplitByKey），因此不必
	 * 检查空值
	 */
	gistentryinit(entry, spl->splitVector.spl_rdatum, r, NULL,
				  (OffsetNumber) 0, false);
	for (i = 0; i < spl->splitVector.spl_nleft; i++)
	{
		int			j = spl->splitVector.spl_left[i];
		float		penalty = gistpenalty(giststate, attno, &entry, false,
										  &valvec[j], false);

		if (penalty == 0.0)
		{
			spl->spl_dontcare[j] = true;
			NumDontCare++;
		}
	}

	/* 对右侧元组则相反 */
	gistentryinit(entry, spl->splitVector.spl_ldatum, r, NULL,
				  (OffsetNumber) 0, false);
	for (i = 0; i < spl->splitVector.spl_nright; i++)
	{
		int			j = spl->splitVector.spl_right[i];
		float		penalty = gistpenalty(giststate, attno, &entry, false,
										  &valvec[j], false);

		if (penalty == 0.0)
		{
			spl->spl_dontcare[j] = true;
			NumDontCare++;
		}
	}

	return NumDontCare;
}

/*
 * 从长度为 *len 的元组索引数组 a[] 中移除标记为无所谓的元组。
 * 分别应用于 spl_left 与 spl_right
 * 数组。
 */
static void
removeDontCares(OffsetNumber *a, int *len, const bool *dontcare)
{
	int			origlen,
				newlen,
				i;
	OffsetNumber *curwpos;

	origlen = newlen = *len;
	curwpos = a;
	for (i = 0; i < origlen; i++)
	{
		OffsetNumber ai = a[i];

		if (dontcare[ai] == false)
		{
			/* 把项重新写回 a[] */
			*curwpos = ai;
			curwpos++;
		}
		else
			newlen--;
	}

	*len = newlen;
}

/*
 * 把单个无所谓元组放到分裂的左侧或右侧，
 * 取决于哪一侧把该元组并入先前计算的联合键时惩罚更小。
 * 我们只需考虑从 attno
 * 开始的列。
 */
static void
placeOne(Relation r, GISTSTATE *giststate, GistSplitVector *v,
		 IndexTuple itup, OffsetNumber off, int attno)
{
	GISTENTRY	identry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		toLeft = true;

	gistDeCompressAtt(giststate, r, itup, NULL, (OffsetNumber) 0,
					  identry, isnull);

	for (; attno < giststate->nonLeafTupdesc->natts; attno++)
	{
		float		lpenalty,
					rpenalty;
		GISTENTRY	entry;

		gistentryinit(entry, v->spl_lattr[attno], r, NULL, 0, false);
		lpenalty = gistpenalty(giststate, attno, &entry, v->spl_lisnull[attno],
							   identry + attno, isnull[attno]);
		gistentryinit(entry, v->spl_rattr[attno], r, NULL, 0, false);
		rpenalty = gistpenalty(giststate, attno, &entry, v->spl_risnull[attno],
							   identry + attno, isnull[attno]);

		if (lpenalty != rpenalty)
		{
			if (lpenalty > rpenalty)
				toLeft = false;
			break;
		}
	}

	if (toLeft)
		v->splitVector.spl_left[v->splitVector.spl_nleft++] = off;
	else
		v->splitVector.spl_right[v->splitVector.spl_nright++] = off;
}

#define SWAPVAR( s, d, t ) \
do {	\
	(t) = (s); \
	(s) = (d); \
	(d) = (t); \
} while(0)

/*
 * 当我们做了二次分裂但用户定义的 PickSplit
 * 方法不支持它时做清理（留下 spl_ldatum_exists 或 spl_rdatum_exists
 * 为真）。
 *
 * 我们考虑是否交换二次分裂的左右输出；
 * 若那样合并这些元组到先前选定集合的惩罚更小，
 * 则值得一做。
 *
 * 无论如何我们必须通过加入先前的联合键（oldL/oldR）来更新当前列的
 * 联合 datum，因为用户定义的
 * PickSplit 方法没有这样做。
 */
static void
supportSecondarySplit(Relation r, GISTSTATE *giststate, int attno,
					  GIST_SPLITVEC *sv, Datum oldL, Datum oldR)
{
	bool		leaveOnLeft = true,
				tmpBool;
	GISTENTRY	entryL,
				entryR,
				entrySL,
				entrySR;

	gistentryinit(entryL, oldL, r, NULL, 0, false);
	gistentryinit(entryR, oldR, r, NULL, 0, false);
	gistentryinit(entrySL, sv->spl_ldatum, r, NULL, 0, false);
	gistentryinit(entrySR, sv->spl_rdatum, r, NULL, 0, false);

	if (sv->spl_ldatum_exists && sv->spl_rdatum_exists)
	{
		float		penalty1,
					penalty2;

		penalty1 = gistpenalty(giststate, attno, &entryL, false, &entrySL, false) +
			gistpenalty(giststate, attno, &entryR, false, &entrySR, false);
		penalty2 = gistpenalty(giststate, attno, &entryL, false, &entrySR, false) +
			gistpenalty(giststate, attno, &entryR, false, &entrySL, false);

		if (penalty1 > penalty2)
			leaveOnLeft = false;
	}
	else
	{
		GISTENTRY  *entry1 = (sv->spl_ldatum_exists) ? &entryL : &entryR;
		float		penalty1,
					penalty2;

		/*
		 * 先前只定义了一个联合，因此我们仅按该侧最低惩罚
		 * 选择是否交换。只有在二次分裂碰巧使其列中、外层递归
		 * 已分配到一侧的元组全部为 NULLs 时才会到这里。
		 * （注意 gistSplitByKey 中的空值检查并不能阻止
		 * 这种情况，因为它们只检查外层递归级视为
		 * 无所谓的元组，而不检查用于确定向下传递的
		 * 左右联合键的那些
		 * 元组。）
		 */
		penalty1 = gistpenalty(giststate, attno, entry1, false, &entrySL, false);
		penalty2 = gistpenalty(giststate, attno, entry1, false, &entrySR, false);

		if (penalty1 < penalty2)
			leaveOnLeft = sv->spl_ldatum_exists;
		else
			leaveOnLeft = sv->spl_rdatum_exists;
	}

	if (leaveOnLeft == false)
	{
		/*
		 * 交换左右
		 */
		OffsetNumber *off,
					noff;
		Datum		datum;

		SWAPVAR(sv->spl_left, sv->spl_right, off);
		SWAPVAR(sv->spl_nleft, sv->spl_nright, noff);
		SWAPVAR(sv->spl_ldatum, sv->spl_rdatum, datum);
		gistentryinit(entrySL, sv->spl_ldatum, r, NULL, 0, false);
		gistentryinit(entrySR, sv->spl_rdatum, r, NULL, 0, false);
	}

	if (sv->spl_ldatum_exists)
		gistMakeUnionKey(giststate, attno, &entryL, false, &entrySL, false,
						 &sv->spl_ldatum, &tmpBool);

	if (sv->spl_rdatum_exists)
		gistMakeUnionKey(giststate, attno, &entryR, false, &entrySR, false,
						 &sv->spl_rdatum, &tmpBool);

	sv->spl_ldatum_exists = sv->spl_rdatum_exists = false;
}

/*
 * 平凡的 picksplit 实现。仅当
 * 用户定义的 picksplit 把所有键放到分裂同一侧时调用。
 * 那是用户定义 picksplit 的缺陷，但我们不想因此失败。
 */
static void
genericPickSplit(GISTSTATE *giststate, GistEntryVector *entryvec, GIST_SPLITVEC *v, int attno)
{
	OffsetNumber i,
				maxoff;
	int			nbytes;
	GistEntryVector *evec;

	maxoff = entryvec->n - 1;

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);

	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = v->spl_nright = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = i;
			v->spl_nleft++;
		}
		else
		{
			v->spl_right[v->spl_nright] = i;
			v->spl_nright++;
		}
	}

	/*
	 * 为每一侧形成联合 datum
	 */
	evec = palloc(sizeof(GISTENTRY) * entryvec->n + GEVHDRSZ);

	evec->n = v->spl_nleft;
	memcpy(evec->vector, entryvec->vector + FirstOffsetNumber,
		   sizeof(GISTENTRY) * evec->n);
	v->spl_ldatum = FunctionCall2Coll(&giststate->unionFn[attno],
									  giststate->supportCollation[attno],
									  PointerGetDatum(evec),
									  PointerGetDatum(&nbytes));

	evec->n = v->spl_nright;
	memcpy(evec->vector, entryvec->vector + FirstOffsetNumber + v->spl_nleft,
		   sizeof(GISTENTRY) * evec->n);
	v->spl_rdatum = FunctionCall2Coll(&giststate->unionFn[attno],
									  giststate->supportCollation[attno],
									  PointerGetDatum(evec),
									  PointerGetDatum(&nbytes));
}

/*
 * 对 attno 列调用用户 picksplit 方法，把元组分裂成
 * 两个向量。
 *
 * 若分裂已完成则返回 false（没有更多 index column，或
 * 无需再考虑它们，因为分裂已经最优）。
 *
 * 若 picksplit 结果退化（所有元组似乎都是无所谓的）则返回 true
 * 且 v->spl_dontcare = NULL，因此我们应忽略
 * 本列，改按后续列分裂。
 *
 * 若存在可按后续列重新安置的无所谓元组则返回 true
 * 且 v->spl_dontcare != NULL。无所谓
 * 元组已从分裂中移除，必须由调用者重新插入。
 * 分裂两侧各至少有一个非无所谓元组，
 * 且所有列的联合键已更新为只包含那些元组。
 *
 * 返回 true 意味着至少还有一列索引列。
 */
static bool
gistUserPicksplit(Relation r, GistEntryVector *entryvec, int attno, GistSplitVector *v,
				  IndexTuple *itup, int len, GISTSTATE *giststate)
{
	GIST_SPLITVEC *sv = &v->splitVector;

	/*
	 * 准备 spl_ldatum/spl_rdatum/spl_ldatum_exists/spl_rdatum_exists，以备
	 * 我们在做二次分裂（见 gist.h 中的注释）。
	 */
	sv->spl_ldatum_exists = !(v->spl_lisnull[attno]);
	sv->spl_rdatum_exists = !(v->spl_risnull[attno]);
	sv->spl_ldatum = v->spl_lattr[attno];
	sv->spl_rdatum = v->spl_rattr[attno];

	/*
	 * 让操作符类专用的 PickSplit 方法完成其工作。注意此时
	 * 我们知道 entryvec 中没有空键。
	 */
	FunctionCall2Coll(&giststate->picksplitFn[attno],
					  giststate->supportCollation[attno],
					  PointerGetDatum(entryvec),
					  PointerGetDatum(sv));

	if (sv->spl_nleft == 0 || sv->spl_nright == 0)
	{
		/*
		 * 用户定义的 picksplit 未能创建实际分裂，即它把
		 * 所有东西都放到了同一侧。抱怨但继续应付。
		 */
		ereport(DEBUG1,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("picksplit method for column %d of index \"%s\" failed",
						attno + 1, RelationGetRelationName(r)),
				 errhint("The index is not optimal. To optimize it, contact a developer, or try to use the column as the second one in the CREATE INDEX command.")));

		/*
		 * 重新初始化 GIST_SPLITVEC。虽然这些字段不被
		 * genericPickSplit() 使用，仍为后续处理设置它们
		 */
		sv->spl_ldatum_exists = !(v->spl_lisnull[attno]);
		sv->spl_rdatum_exists = !(v->spl_risnull[attno]);
		sv->spl_ldatum = v->spl_lattr[attno];
		sv->spl_rdatum = v->spl_rattr[attno];

		/* 做一次通用分裂 */
		genericPickSplit(giststate, entryvec, sv, attno);
	}
	else
	{
		/* 为兼容旧 picksplit API 的 hack */
		if (sv->spl_left[sv->spl_nleft - 1] == InvalidOffsetNumber)
			sv->spl_left[sv->spl_nleft - 1] = (OffsetNumber) (entryvec->n - 1);
		if (sv->spl_right[sv->spl_nright - 1] == InvalidOffsetNumber)
			sv->spl_right[sv->spl_nright - 1] = (OffsetNumber) (entryvec->n - 1);
	}

	/* 若 PickSplit 未处理二次分裂则清理 */
	if (sv->spl_ldatum_exists || sv->spl_rdatum_exists)
		supportSecondarySplit(r, giststate, attno, sv,
							  v->spl_lattr[attno], v->spl_rattr[attno]);

	/* 把 PickSplit 算出的联合 datum 写回 v 数组 */
	v->spl_lattr[attno] = sv->spl_ldatum;
	v->spl_rattr[attno] = sv->spl_rdatum;
	v->spl_lisnull[attno] = false;
	v->spl_risnull[attno] = false;

	/*
	 * 若还有索引列，则考虑能否用它们
	 * 改进分裂。
	 */
	v->spl_dontcare = NULL;

	if (attno + 1 < giststate->nonLeafTupdesc->natts)
	{
		int			NumDontCare;

		/*
		 * 快速检查左右联合键是否相等；
		 * 若是，则分裂肯定退化，因此告诉调用者
		 * 用下一列重新分裂。
		 */
		if (gistKeyIsEQ(giststate, attno, sv->spl_ldatum, sv->spl_rdatum))
			return true;

		/*
		 * 定位无所谓元组（若有）。若没有，则分裂已
		 * 最优，因此直接退出并返回 false。
		 */
		v->spl_dontcare = (bool *) palloc0(sizeof(bool) * (entryvec->n + 1));

		NumDontCare = findDontCares(r, giststate, entryvec->vector, v, attno);

		if (NumDontCare > 0)
		{
			/*
			 * 从 spl_left[] 与 spl_right[] 中移除无所谓项。
			 */
			removeDontCares(sv->spl_left, &sv->spl_nleft, v->spl_dontcare);
			removeDontCares(sv->spl_right, &sv->spl_nright, v->spl_dontcare);

			/*
			 * 若任一侧的全部元组都是无所谓的，则分裂
			 * 退化，最好忽略它并按下一列分裂。
			 * （我们曾尝试通过强制每侧一个随机元组当作
			 * 非无所谓来推进二次分裂，但那种技术似乎
			 * 不太可能给出更好结果。注意我们不想在
			 * 左侧或右侧主分裂侧为空时尝试二次分裂，
			 * 因为那样该侧没有联合键供
			 * PickSplit 函数尝试扩展，因而无法对其行为有良好
			 * 优劣度量。还要注意此检查
			 * 确保我们不能在下面 NumDontCare == 1 的特殊情况下
			 * 产生虚假的单侧
			 * 分裂。）
			 */
			if (sv->spl_nleft == 0 || sv->spl_nright == 0)
			{
				v->spl_dontcare = NULL;
				return true;
			}

			/*
			 * 重新计算联合键，只考虑非无所谓元组。
			 * 注意：这会为剩余索引列设置联合键，
			 * 这将使后续 gistUserPicksplit 调用把那些
			 * 值向下传给用户定义的 PickSplit 方法，并置
			 * spl_ldatum_exists/spl_rdatum_exists 为真。
			 */
			gistunionsubkey(giststate, itup, v);

			if (NumDontCare == 1)
			{
				/*
				 * 若只有一个无所谓元组，则无法对它做
				 * PickSplit，因此仅通过比较惩罚来选择送左还是
				 * 送右。我们反正需要
				 * gistunionsubkey 步骤，以便有合适的
				 * 联合键来计算惩罚。
				 */
				OffsetNumber toMove;

				/* 找到它 ... */
				for (toMove = FirstOffsetNumber; toMove < entryvec->n; toMove++)
				{
					if (v->spl_dontcare[toMove])
						break;
				}
				Assert(toMove < entryvec->n);

				/* ... 并把它赋给更便宜的一侧 */
				placeOne(r, giststate, v, itup[toMove - 1], toMove, attno + 1);

				/*
				 * 此时联合键是错的，但我们不在乎，
				 * 因为分裂已完成。gistSplitByKey 的最外层
				 * 递归级会在返回前修好。
				 */
			}
			else
				return true;
		}
	}

	return false;
}

/*
 * 简单地对半分裂页
 */
static void
gistSplitHalf(GIST_SPLITVEC *v, int len)
{
	int			i;

	v->spl_nright = v->spl_nleft = 0;
	v->spl_left = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
	for (i = 1; i <= len; i++)
		if (i < len / 2)
			v->spl_right[v->spl_nright++] = i;
		else
			v->spl_left[v->spl_nleft++] = i;

	/* 我们不必计算联合键，调用者已处理好 */
}

/*
 * gistSplitByKey：页分裂算法的主入口点
 *
 * r: 索引关系
 * page: 正被分裂的页
 * itup: 待处理的 IndexTuples 数组
 * len: 待处理的 IndexTuples 数量（至少为 2）
 * giststate: 关于索引的附加信息
 * v: 工作状态与输出区
 * attno: 我们正在处理的列（从零开始的下标）
 *
 * 外部调用者必须把 v->spl_lisnull 与 v->spl_risnull 数组初始化
 * 为全真。返回时，spl_left/spl_nleft 包含应去左侧的元组
 * 下标，spl_right/spl_nright 包含应去右侧的元组下标，
 * spl_lattr/spl_lisnull 包含左侧联合键值，而
 * spl_rattr/spl_risnull 包含右侧联合键值。本结构中的其他成员
 * 是本文件的工作区。
 *
 * 外部调用者必须传入 attno 为零。函数可在内部
 * 通过传入 attno+1 递归到下一列。
 */
void
gistSplitByKey(Relation r, Page page, IndexTuple *itup, int len,
			   GISTSTATE *giststate, GistSplitVector *v, int attno)
{
	GistEntryVector *entryvec;
	OffsetNumber *offNullTuples;
	int			nOffNullTuples = 0;
	int			i;

	/* 生成项数组，并识别带空键的元组 */
	/* 注意 entryvec->vector[0] 在本代码中未使用 */
	entryvec = palloc(GEVHDRSZ + (len + 1) * sizeof(GISTENTRY));
	entryvec->n = len + 1;
	offNullTuples = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));

	for (i = 1; i <= len; i++)
	{
		Datum		datum;
		bool		IsNull;

		datum = index_getattr(itup[i - 1], attno + 1, giststate->leafTupdesc,
							  &IsNull);
		gistdentryinit(giststate, attno, &(entryvec->vector[i]),
					   datum, r, page, i,
					   false, IsNull);
		if (IsNull)
			offNullTuples[nOffNullTuples++] = i;
	}

	if (nOffNullTuples == len)
	{
		/*
		 * 边界情况：attno 列中的所有键都为空，因此只需把
		 * 注意力转到下一列。若没有下一列，就
		 * 对半分裂页。
		 */
		v->spl_risnull[attno] = v->spl_lisnull[attno] = true;

		if (attno + 1 < giststate->nonLeafTupdesc->natts)
			gistSplitByKey(r, page, itup, len, giststate, v, attno + 1);
		else
			gistSplitHalf(&v->splitVector, len);
	}
	else if (nOffNullTuples > 0)
	{
		int			j = 0;

		/*
		 * 我们不想在一页上混放 NULL 与非 NULL 键，因此把
		 * 空值分到右页、非空分到左页。
		 */
		v->splitVector.spl_right = offNullTuples;
		v->splitVector.spl_nright = nOffNullTuples;
		v->spl_risnull[attno] = true;

		v->splitVector.spl_left = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
		v->splitVector.spl_nleft = 0;
		for (i = 1; i <= len; i++)
			if (j < v->splitVector.spl_nright && offNullTuples[j] == i)
				j++;
			else
				v->splitVector.spl_left[v->splitVector.spl_nleft++] = i;

		/* 计算联合键，除非外层递归级会处理 */
		if (attno == 0 && giststate->nonLeafTupdesc->natts == 1)
		{
			v->spl_dontcare = NULL;
			gistunionsubkey(giststate, itup, v);
		}
	}
	else
	{
		/*
		 * 所有键都非空，因此应用用户定义的 PickSplit 方法
		 */
		if (gistUserPicksplit(r, entryvec, attno, v, itup, len, giststate))
		{
			/*
			 * 按 attno 列分裂并非最优，因此考虑
			 * 按下一列重新分配无所谓元组
			 */
			Assert(attno + 1 < giststate->nonLeafTupdesc->natts);

			if (v->spl_dontcare == NULL)
			{
				/*
				 * 这次分裂实际上是退化的，因此完全忽略它
				 * 并仅按下一列分裂。
				 */
				gistSplitByKey(r, page, itup, len, giststate, v, attno + 1);
			}
			else
			{
				/*
				 * 形成一个仅含无所谓元组的数组，传给本函数
				 * 对下一列的递归调用。
				 */
				IndexTuple *newitup = (IndexTuple *) palloc(len * sizeof(IndexTuple));
				OffsetNumber *map = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
				int			newlen = 0;
				GIST_SPLITVEC backupSplit;

				for (i = 0; i < len; i++)
				{
					if (v->spl_dontcare[i + 1])
					{
						newitup[newlen] = itup[i];
						map[newlen] = i + 1;
						newlen++;
					}
				}

				Assert(newlen > 0);

				/*
				 * 备份 v->splitVector，因为递归
				 * 调用会用它自己的结果覆盖它。
				 */
				backupSplit = v->splitVector;
				backupSplit.spl_left = (OffsetNumber *) palloc(sizeof(OffsetNumber) * len);
				memcpy(backupSplit.spl_left, v->splitVector.spl_left, sizeof(OffsetNumber) * v->splitVector.spl_nleft);
				backupSplit.spl_right = (OffsetNumber *) palloc(sizeof(OffsetNumber) * len);
				memcpy(backupSplit.spl_right, v->splitVector.spl_right, sizeof(OffsetNumber) * v->splitVector.spl_nright);

				/* 递归决定如何分裂无所谓元组 */
				gistSplitByKey(r, page, newitup, newlen, giststate, v, attno + 1);

				/* 把子分裂结果与非无所谓元组合并 */
				for (i = 0; i < v->splitVector.spl_nleft; i++)
					backupSplit.spl_left[backupSplit.spl_nleft++] = map[v->splitVector.spl_left[i] - 1];
				for (i = 0; i < v->splitVector.spl_nright; i++)
					backupSplit.spl_right[backupSplit.spl_nright++] = map[v->splitVector.spl_right[i] - 1];

				v->splitVector = backupSplit;
			}
		}
	}

	/*
	 * 若我们处理的是多列索引，在递归结束时
	 * 为所有索引列重新计算左右联合 datum。这
	 * 确保我们在所有边界情况下交回正确的联合 datum，
	 * 包括我们一开始并未处理所有列时，或当
	 * 二次分裂把"无所谓"元组从一侧移到另一侧时
	 * （我们实在不应假定那没有改变联合 datum）。
	 *
	 * 注意：当我们处于内部递归（attno > 0）时，我们不必担心
	 * 返回的联合 datum 是否合理，因为
	 * 调用级并不关心。此外，在单列索引中，我们期望
	 * PickSplit（或上面的特殊情况）已产生正确的联合
	 * datum。
	 */
	if (attno == 0 && giststate->nonLeafTupdesc->natts > 1)
	{
		v->spl_dontcare = NULL;
		gistunionsubkey(giststate, itup, v);
	}
}
