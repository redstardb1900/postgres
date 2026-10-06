/*-------------------------------------------------------------------------
 *
 * gistutil.c
 *	  postgres GiST 索引访问方法的实用例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gist/gistutil.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/gist_private.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/pg_opclass.h"
#include "common/pg_prng.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 把 itup 向量写入页，不控制空闲空间。
 */
void
gistfillbuffer(Page page, IndexTuple *itup, int len, OffsetNumber off)
{
	int			i;

	if (off == InvalidOffsetNumber)
		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

	for (i = 0; i < len; i++)
	{
		Size		sz = IndexTupleSize(itup[i]);
		OffsetNumber l;

		l = PageAddItem(page, (Item) itup[i], sz, off, false, false);
		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to GiST index page, item %d out of %d, size %d bytes",
				 i, len, (int) sz);
		off++;
	}
}

/*
 * 检查页上是否有容纳 itup 向量的空间
 */
bool
gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete, Size freespace)
{
	unsigned int size = freespace,
				deleted = 0;
	int			i;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	if (todelete != InvalidOffsetNumber)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, todelete));

		deleted = IndexTupleSize(itup) + sizeof(ItemIdData);
	}

	return (PageGetFreeSpace(page) + deleted < size);
}

bool
gistfitpage(IndexTuple *itvec, int len)
{
	int			i;
	Size		size = 0;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	/* TODO: 考虑 fillfactor */
	return (size <= GiSTPageSize);
}

/*
 * 把缓冲区读入 itup 向量
 */
IndexTuple *
gistextractpage(Page page, int *len /* 输出 */ )
{
	OffsetNumber i,
				maxoff;
	IndexTuple *itvec;

	maxoff = PageGetMaxOffsetNumber(page);
	*len = maxoff;
	itvec = palloc(sizeof(IndexTuple) * maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		itvec[i - FirstOffsetNumber] = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));

	return itvec;
}

/*
 * 把两个向量合并为一个
 */
IndexTuple *
gistjoinvector(IndexTuple *itvec, int *len, IndexTuple *additvec, int addlen)
{
	itvec = (IndexTuple *) repalloc(itvec, sizeof(IndexTuple) * ((*len) + addlen));
	memmove(&itvec[*len], additvec, sizeof(IndexTuple) * addlen);
	*len += addlen;
	return itvec;
}

/*
 * 构造扁平的 IndexTuple 向量
 */

IndexTupleData *
gistfillitupvec(IndexTuple *vec, int veclen, int *memlen)
{
	char	   *ptr,
			   *ret;
	int			i;

	*memlen = 0;

	for (i = 0; i < veclen; i++)
		*memlen += IndexTupleSize(vec[i]);

	ptr = ret = palloc(*memlen);

	for (i = 0; i < veclen; i++)
	{
		memcpy(ptr, vec[i], IndexTupleSize(vec[i]));
		ptr += IndexTupleSize(vec[i]);
	}

	return (IndexTupleData *) ret;
}

/*
 * 对 IndexTuple 向量中的键做联合（每个索引列一个联合 datum）。
 * 联合后的 Datum 返回到 attr/isnull 数组中。
 * 结果 Datum 未经压缩。
 */
void
gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
				   Datum *attr, bool *isnull)
{
	int			i;
	GistEntryVector *evec;
	int			attrsize = 0;	/* 消除编译器警告 */

	evec = (GistEntryVector *) palloc((len + 2) * sizeof(GISTENTRY) + GEVHDRSZ);

	for (i = 0; i < giststate->nonLeafTupdesc->natts; i++)
	{
		int			j;

		/* 收集该列的非空 datum */
		evec->n = 0;
		for (j = 0; j < len; j++)
		{
			Datum		datum;
			bool		IsNull;

			datum = index_getattr(itvec[j], i + 1, giststate->leafTupdesc,
								  &IsNull);
			if (IsNull)
				continue;

			gistdentryinit(giststate, i,
						   evec->vector + evec->n,
						   datum,
						   NULL, NULL, (OffsetNumber) 0,
						   false, IsNull);
			evec->n++;
		}

		/* 若该列全为 NULLs，则联合结果为 NULL */
		if (evec->n == 0)
		{
			attr[i] = (Datum) 0;
			isnull[i] = true;
		}
		else
		{
			if (evec->n == 1)
			{
				/* unionFn 可能期望至少两个输入 */
				evec->n = 2;
				evec->vector[1] = evec->vector[0];
			}

			/* 做联合并存入 attr 数组 */
			attr[i] = FunctionCall2Coll(&giststate->unionFn[i],
										giststate->supportCollation[i],
										PointerGetDatum(evec),
										PointerGetDatum(&attrsize));

			isnull[i] = false;
		}
	}
}

/*
 * 返回一个 IndexTuple，其中包含对指定 IndexTuple 向量应用
 * "union" 方法的结果。
 */
IndexTuple
gistunion(Relation r, IndexTuple *itvec, int len, GISTSTATE *giststate)
{
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	gistMakeUnionItVec(giststate, itvec, len, attr, isnull);

	return gistFormTuple(giststate, r, attr, isnull, false);
}

/*
 * 对两个键做联合
 */
void
gistMakeUnionKey(GISTSTATE *giststate, int attno,
				 GISTENTRY *entry1, bool isnull1,
				 GISTENTRY *entry2, bool isnull2,
				 Datum *dst, bool *dstisnull)
{
	/* 我们需要恰好能容纳 2 个元素的 GistEntryVector */
	union
	{
		GistEntryVector gev;
		char		padding[2 * sizeof(GISTENTRY) + GEVHDRSZ];
	}			storage;
	GistEntryVector *evec = &storage.gev;
	int			dstsize = 0;	/* 消除编译器警告 */

	evec->n = 2;

	if (isnull1 && isnull2)
	{
		*dstisnull = true;
		*dst = (Datum) 0;
	}
	else
	{
		if (isnull1 == false && isnull2 == false)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry2;
		}
		else if (isnull1 == false)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry1;
		}
		else
		{
			evec->vector[0] = *entry2;
			evec->vector[1] = *entry2;
		}

		*dstisnull = false;
		*dst = FunctionCall2Coll(&giststate->unionFn[attno],
								 giststate->supportCollation[attno],
								 PointerGetDatum(evec),
								 PointerGetDatum(&dstsize));
	}
}

bool
gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b)
{
	bool		result = false; /* 消除编译器警告 */

	FunctionCall3Coll(&giststate->equalFn[attno],
					  giststate->supportCollation[attno],
					  a, b,
					  PointerGetDatum(&result));
	return result;
}

/*
 * 解压缩元组中的全部键
 */
void
gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
				  OffsetNumber o, GISTENTRY *attdata, bool *isnull)
{
	int			i;

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		Datum		datum;

		datum = index_getattr(tuple, i + 1, giststate->leafTupdesc, &isnull[i]);
		gistdentryinit(giststate, i, &attdata[i],
					   datum, r, p, o,
					   false, isnull[i]);
	}
}

/*
 * 形成 oldtup 与 addtup 的联合；若联合等于 oldtup 则返回 NULL
 */
IndexTuple
gistgetadjusted(Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE *giststate)
{
	bool		neednew = false;
	GISTENTRY	oldentries[INDEX_MAX_KEYS],
				addentries[INDEX_MAX_KEYS];
	bool		oldisnull[INDEX_MAX_KEYS],
				addisnull[INDEX_MAX_KEYS];
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	newtup = NULL;
	int			i;

	gistDeCompressAtt(giststate, r, oldtup, NULL,
					  (OffsetNumber) 0, oldentries, oldisnull);

	gistDeCompressAtt(giststate, r, addtup, NULL,
					  (OffsetNumber) 0, addentries, addisnull);

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		gistMakeUnionKey(giststate, i,
						 oldentries + i, oldisnull[i],
						 addentries + i, addisnull[i],
						 attr + i, isnull + i);

		if (neednew)
			/* 已需要新键，因此可跳过检查 */
			continue;

		if (isnull[i])
			/* 键的联合为 NULL，当且仅当两个键都为 NULL */
			continue;

		if (!addisnull[i])
		{
			if (oldisnull[i] ||
				!gistKeyIsEQ(giststate, i, oldentries[i].key, attr[i]))
				neednew = true;
		}
	}

	if (neednew)
	{
		/* 需要更新键 */
		newtup = gistFormTuple(giststate, r, attr, isnull, false);
		newtup->t_tid = oldtup->t_tid;
	}

	return newtup;
}

/*
 * 在上层索引页中搜索对插入 "it" 所含新索引键
 * 惩罚最低的条目。
 *
 * 返回要插入到的页条目索引。
 */
OffsetNumber
gistchoose(Relation r, Page p, IndexTuple it,	/* it 具有压缩后的条目 */
		   GISTSTATE *giststate)
{
	OffsetNumber result;
	OffsetNumber maxoff;
	OffsetNumber i;
	float		best_penalty[INDEX_MAX_KEYS];
	GISTENTRY	entry,
				identry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			keep_current_best;

	Assert(!GistPageIsLeaf(p));

	gistDeCompressAtt(giststate, r,
					  it, NULL, (OffsetNumber) 0,
					  identry, isnull);

	/* 若页为空则返回 FirstOffsetNumber（不应发生） */
	result = FirstOffsetNumber;

	/*
	 * 索引可能有多列，且每列都有一个惩罚值。
	 * 索引定义中出现较早的列，其惩罚严格地比
	 * 出现较晚的列的惩罚更重要。
	 *
	 * best_penalty[j] 是迄今为止在第 j 列见过的最佳
	 * 惩罚，尚未检查第 j 列时为 -1。第一个 -1
	 * 右侧的
	 * 数组元素未定义。
	 */
	best_penalty[0] = -1;

	/*
	 * 若找到与当前最佳元组同样好的元组，两者
	 * 都可用。当插入大量相同或相近键的元组时，
	 * 尽可能沿同一路径下降更有利于缓存。
	 * 另一方面，若分裂后全部插入都落到同一叶子页，
	 * 我们就永远不会向分裂的另一半插入任何内容，最终
	 * 只用到可用空间的 50%。均匀分配插入会
	 * 带来更好的空间利用率，但会损害插入期间的
	 * 缓存局部性。为兼得两者，当找到与先前最佳
	 * 同样好的元组时，随机选择是坚持旧最佳，
	 * 还是改用新的。一旦决定坚持旧最佳，对随后
	 * 找到的任何同等好的元组都继续坚持。这偏袒
	 * 偏移量较低的元组，但仍允许部分插入进入
	 * 其他同等好的子树。
	 *
	 * keep_current_best 为 -1 表示尚未就是否保留当前
	 * 最佳元组做过随机选择。若已做过并决定保留，
	 * keep_current_best 为 1；若已决定替换，则为
	 * 0。（一旦完成替换，该状态会重置为 -1，
	 * 但有时会在实际找到替换的最佳元组
	 * 之前就
	 * 做出选择。）
	 */
	keep_current_best = -1;

	/*
	 * 遍历页上的元组。
	 */
	maxoff = PageGetMaxOffsetNumber(p);
	Assert(maxoff >= FirstOffsetNumber);

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));
		bool		zero_penalty;
		int			j;

		zero_penalty = true;

		/* 遍历索引属性。 */
		for (j = 0; j < IndexRelationGetNumberOfKeyAttributes(r); j++)
		{
			Datum		datum;
			float		usize;
			bool		IsNull;

			/* 计算该列的惩罚。 */
			datum = index_getattr(itup, j + 1, giststate->leafTupdesc,
								  &IsNull);
			gistdentryinit(giststate, j, &entry, datum, r, p, i,
						   false, IsNull);
			usize = gistpenalty(giststate, j, &entry, IsNull,
								&identry[j], isnull[j]);
			if (usize > 0)
				zero_penalty = false;

			if (best_penalty[j] < 0 || usize < best_penalty[j])
			{
				/*
				 * 该列出现新的最佳惩罚。暂定选择此元组
				 * 作为目标，并记下最佳惩罚。然后将下一列的
				 * 惩罚重置为 "unknown"（并间接地，对其右侧
				 * 所有列同样处理）。这将迫使我们在后续
				 * 循环迭代中采用此元组的惩罚值作为
				 * 其余各列的最佳值。
				 */
				result = i;
				best_penalty[j] = usize;

				if (j < IndexRelationGetNumberOfKeyAttributes(r) - 1)
					best_penalty[j + 1] = -1;

				/* 有了新最佳，因此重置是否保留的决定 */
				keep_current_best = -1;
			}
			else if (best_penalty[j] == usize)
			{
				/*
				 * 当前元组在该列上与迄今最佳元组
				 * 同样好。本循环的下一次迭代
				 * 将比较下一列。
				 */
			}
			else
			{
				/*
				 * 当前元组在该列上比迄今最佳
				 * 元组更差。跳过其余列，继续
				 * 处理下一个元组（若有）。
				 */
				zero_penalty = false;	/* 使外层循环不会退出 */
				break;
			}
		}

		/*
		 * 若已循环过最后一列，且未更新 "result"，
		 * 则此元组与先前最佳元组同样好。
		 */
		if (j == IndexRelationGetNumberOfKeyAttributes(r) && result != i)
		{
			if (keep_current_best == -1)
			{
				/* 尚未为该旧最佳做过随机选择 */
				keep_current_best = pg_prng_bool(&pg_global_prng_state) ? 1 : 0;
			}
			if (keep_current_best == 0)
			{
				/* 我们选择使用新元组 */
				result = i;
				/* 若还有更多同样好的，再次选择 */
				keep_current_best = -1;
			}
		}

		/*
		 * 若找到各列惩罚均为零的元组，且我们
		 * 已决定不再搜索惩罚相等的另一元组，
		 * 则无需再检查其余元组；直接跳出
		 * 循环并返回它。
		 */
		if (zero_penalty)
		{
			if (keep_current_best == -1)
			{
				/* 尚未为该旧最佳做过随机选择 */
				keep_current_best = pg_prng_bool(&pg_global_prng_state) ? 1 : 0;
			}
			if (keep_current_best == 1)
				break;
		}
	}

	return result;
}

/*
 * 用键的解压缩版本初始化 GiST 条目
 */
void
gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
			   Datum k, Relation r, Page pg, OffsetNumber o,
			   bool l, bool isNull)
{
	if (!isNull)
	{
		GISTENTRY  *dep;

		gistentryinit(*e, k, r, pg, o, l);

		/* 操作符类中可能没有 decompress 函数 */
		if (!OidIsValid(giststate->decompressFn[nkey].fn_oid))
			return;

		dep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1Coll(&giststate->decompressFn[nkey],
											  giststate->supportCollation[nkey],
											  PointerGetDatum(e)));
		/* decompressFn 可能只是返回给定指针 */
		if (dep != e)
			gistentryinit(*e, dep->key, dep->rel, dep->page, dep->offset,
						  dep->leafkey);
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, l);
}

IndexTuple
gistFormTuple(GISTSTATE *giststate, Relation r,
			  Datum *attdata, bool *isnull, bool isleaf)
{
	Datum		compatt[INDEX_MAX_KEYS];
	IndexTuple	res;

	gistCompressValues(giststate, r, attdata, isnull, isleaf, compatt);

	res = index_form_tuple(isleaf ? giststate->leafTupdesc :
						   giststate->nonLeafTupdesc,
						   compatt, isnull);

	/*
	 * 内部页上元组的偏移号未使用。出于历史
	 * 原因，将其设为 0xffff。
	 */
	ItemPointerSetOffsetNumber(&(res->t_tid), 0xffff);
	return res;
}

void
gistCompressValues(GISTSTATE *giststate, Relation r,
				   Datum *attdata, bool *isnull, bool isleaf, Datum *compatt)
{
	int			i;

	/*
	 * 对每个属性调用 compress 方法。
	 */
	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		if (isnull[i])
			compatt[i] = (Datum) 0;
		else
		{
			GISTENTRY	centry;
			GISTENTRY  *cep;

			gistentryinit(centry, attdata[i], r, NULL, (OffsetNumber) 0,
						  isleaf);
			/* 操作符类中可能没有 compress 函数 */
			if (OidIsValid(giststate->compressFn[i].fn_oid))
				cep = (GISTENTRY *)
					DatumGetPointer(FunctionCall1Coll(&giststate->compressFn[i],
													  giststate->supportCollation[i],
													  PointerGetDatum(&centry)));
			else
				cep = &centry;
			compatt[i] = cep->key;
		}
	}

	if (isleaf)
	{
		/*
		 * 若有被 INCLUDE 的属性，则放入它们。
		 */
		for (; i < r->rd_att->natts; i++)
		{
			if (isnull[i])
				compatt[i] = (Datum) 0;
			else
				compatt[i] = attdata[i];
		}
	}
}

/*
 * 用取回的值在 key 字段中初始化 GiST 条目
 */
static Datum
gistFetchAtt(GISTSTATE *giststate, int nkey, Datum k, Relation r)
{
	GISTENTRY	fentry;
	GISTENTRY  *fep;

	gistentryinit(fentry, k, r, NULL, (OffsetNumber) 0, false);

	fep = (GISTENTRY *)
		DatumGetPointer(FunctionCall1Coll(&giststate->fetchFn[nkey],
										  giststate->supportCollation[nkey],
										  PointerGetDatum(&fentry)));

	/* fetchFn 设置了 'key'，将其返回给调用者 */
	return fep->key;
}

/*
 * 取回元组中的全部键。
 * 返回包含原先被索引数据的新 HeapTuple。
 */
HeapTuple
gistFetchTuple(GISTSTATE *giststate, Relation r, IndexTuple tuple)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(giststate->tempCxt);
	Datum		fetchatt[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			i;

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		Datum		datum;

		datum = index_getattr(tuple, i + 1, giststate->leafTupdesc, &isnull[i]);

		if (giststate->fetchFn[i].fn_oid != InvalidOid)
		{
			if (!isnull[i])
				fetchatt[i] = gistFetchAtt(giststate, i, datum, r);
			else
				fetchatt[i] = (Datum) 0;
		}
		else if (giststate->compressFn[i].fn_oid == InvalidOid)
		{
			/*
			 * 若操作符类未提供可能改变原始值的
			 * compress 方法，则 att 必然以原始形式存储。
			 */
			if (!isnull[i])
				fetchatt[i] = datum;
			else
				fetchatt[i] = (Datum) 0;
		}
		else
		{
			/*
			 * 该列不支持仅索引扫描。由于规划器
			 * 仍选择了仅索引扫描，说明它对该列
			 * 不感兴趣，我们可以用 NULL 替换它。
			 */
			isnull[i] = true;
			fetchatt[i] = (Datum) 0;
		}
	}

	/*
	 * 获取每个被 INCLUDE 的属性。
	 */
	for (; i < r->rd_att->natts; i++)
	{
		fetchatt[i] = index_getattr(tuple, i + 1, giststate->leafTupdesc,
									&isnull[i]);
	}
	MemoryContextSwitchTo(oldcxt);

	return heap_form_tuple(giststate->fetchTupdesc, fetchatt, isnull);
}

float
gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *orig, bool isNullOrig,
			GISTENTRY *add, bool isNullAdd)
{
	float		penalty = 0.0;

	if (giststate->penaltyFn[attno].fn_strict == false ||
		(isNullOrig == false && isNullAdd == false))
	{
		FunctionCall3Coll(&giststate->penaltyFn[attno],
						  giststate->supportCollation[attno],
						  PointerGetDatum(orig),
						  PointerGetDatum(add),
						  PointerGetDatum(&penalty));
		/* 禁止负的或 NaN 惩罚 */
		if (isnan(penalty) || penalty < 0.0)
			penalty = 0.0;
	}
	else if (isNullOrig && isNullAdd)
		penalty = 0.0;
	else
	{
		/* 尽量避免混用空与非空值 */
		penalty = get_float4_infinity();
	}

	return penalty;
}

/*
 * 初始化新的索引页
 */
void
gistinitpage(Page page, uint32 f)
{
	GISTPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(GISTPageOpaqueData));

	opaque = GistPageGetOpaque(page);
	opaque->rightlink = InvalidBlockNumber;
	opaque->flags = f;
	opaque->gist_page_id = GIST_PAGE_ID;
}

/*
 * 初始化新的索引缓冲区
 */
void
GISTInitBuffer(Buffer b, uint32 f)
{
	Page		page;

	page = BufferGetPage(b);
	gistinitpage(page, f);
}

/*
 * 验证刚读入的页看起来是否合理。
 */
void
gistcheckpage(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer 会验证每个新读入的页都通过
	 * PageHeaderIsValid，意味着它要么包含合理健全的
	 * 页头，要么全为零。不过我们必须防范
	 * 全零的情况。
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * 另外检查特殊区域看起来是否合理。
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GISTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));
}


/*
 * 分配新页（通过回收，或通过扩展索引文件）
 *
 * 返回的缓冲区已被钉住并独占锁定
 *
 * 调用者负责通过调用 GISTInitBuffer 初始化该页
 */
Buffer
gistNewBuffer(Relation r, Relation heaprel)
{
	Buffer		buffer;

	/* 首先尝试从 FSM 获取一页 */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(r);

		if (blkno == InvalidBlockNumber)
			break;				/* FSM 中已无剩余 */

		buffer = ReadBuffer(r, blkno);

		/*
		 * 必须防范别人已经回收了该页的可能；
		 * 若如此，缓冲区可能已被锁定。
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			/*
			 * 若页从未初始化，则可使用。
			 */
			if (PageIsNew(page))
				return buffer;

			gistcheckpage(r, buffer);

			/*
			 * 否则，若已删除且过旧、不再有任何
			 * 进程对其感兴趣，则回收它。
			 */
			if (gistPageRecyclable(page))
			{
				/*
				 * 若正在为热备生成 WAL，则创建一条
				 * WAL 记录，以便在备库上运行的查询快照
				 * 早于该页的 deleteXid 时，能与这些
				 * 查询产生冲突。
				 */
				if (XLogStandbyInfoActive() && RelationNeedsWAL(r))
					gistXLogPageReuse(r, heaprel, blkno, GistPageGetDeleteXid(page));

				return buffer;
			}

			LockBuffer(buffer, GIST_UNLOCK);
		}

		/* 无法使用，因此释放缓冲区并重试 */
		ReleaseBuffer(buffer);
	}

	/* 必须扩展文件 */
	buffer = ExtendBufferedRel(BMR_REL(r), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);

	return buffer;
}

/* 此页现在是否可回收？ */
bool
gistPageRecyclable(Page page)
{
	if (PageIsNew(page))
		return true;
	if (GistPageIsDeleted(page))
	{
		/*
		 * 页已被删除，但何时删除的？若刚删除，扫描
		 * 可能已见过指向它的下行指针，并将稍后读该页。
		 * 只要这种情况仍可能发生，就必须把已删除页保留为
		 * 墓碑。
		 *
		 * 为此检查删除 XID 是否仍可能对任何人可见。
		 * 若否，则仍在进行的扫描不可能见过其
		 * 下行指针，我们就可以回收它。
		 */
		FullTransactionId deletexid_full = GistPageGetDeleteXid(page);

		return GlobalVisCheckRemovableFullXid(NULL, deletexid_full);
	}
	return false;
}

bytea *
gistoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(GiSTOptions, fillfactor)},
		{"buffering", RELOPT_TYPE_ENUM, offsetof(GiSTOptions, buffering_mode)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_GIST,
									  sizeof(GiSTOptions),
									  tab, lengthof(tab));
}

/*
 *	gistproperty() -- 检查索引的布尔属性。
 *
 * 对多数 AM 而言这是可选的，但对 GiST 是必需的，因为核心
 * 属性代码不支持 AMPROP_DISTANCE_ORDERABLE。我们也在此处理
 * AMPROP_RETURNABLE，以免为调用 gistcanreturn 而打开关系。
 */
bool
gistproperty(Oid index_oid, int attno,
			 IndexAMProperty prop, const char *propname,
			 bool *res, bool *isnull)
{
	Oid			opclass,
				opfamily,
				opcintype;
	int16		procno;

	/* 仅回答列级查询 */
	if (attno == 0)
		return false;

	/*
	 * 目前，GiST 按距离有序的扫描需要操作符类中存在
	 * 使用默认类型的 distance 函数（即装入 relcache
	 * 条目的那个，见 initGISTstate）。因此我们假定：若
	 * 存在这样的函数，就有其存在的理由（而不是翻遍
	 * 操作符族的全部操作符去找有序的那个）。
	 *
	 * 本质上同一段代码也可测试是否支持返回列数据，
	 * 因为若操作符类提供了 fetch 过程则为真。
	 */

	switch (prop)
	{
		case AMPROP_DISTANCE_ORDERABLE:
			procno = GIST_DISTANCE_PROC;
			break;
		case AMPROP_RETURNABLE:
			procno = GIST_FETCH_PROC;
			break;
		default:
			return false;
	}

	/* 首先需要知道该列的操作符类。 */
	opclass = get_index_column_opclass(index_oid, attno);
	if (!OidIsValid(opclass))
	{
		*isnull = true;
		return true;
	}

	/* 现在查找操作符类所属族与输入数据类型。 */
	if (!get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype))
	{
		*isnull = true;
		return true;
	}

	/* 现在可以检查该函数是否已提供。 */

	*res = SearchSysCacheExists4(AMPROCNUM,
								 ObjectIdGetDatum(opfamily),
								 ObjectIdGetDatum(opcintype),
								 ObjectIdGetDatum(opcintype),
								 Int16GetDatum(procno));

	/*
	 * 特例：即便没有 fetch 函数，若操作符类没有
	 * compress 函数，AMPROP_RETURNABLE 仍为真。
	 */
	if (prop == AMPROP_RETURNABLE && !*res)
	{
		*res = !SearchSysCacheExists4(AMPROCNUM,
									  ObjectIdGetDatum(opfamily),
									  ObjectIdGetDatum(opcintype),
									  ObjectIdGetDatum(opcintype),
									  Int16GetDatum(GIST_COMPRESS_PROC));
	}

	*isnull = false;

	return true;
}

/*
 * 有些索引不做 WAL 记录，但仍需要 LSNs 来检测并发页
 * 分裂。本函数为此提供一个假的 LSN
 * 序列。
 */
XLogRecPtr
gistGetFakeLSN(Relation rel)
{
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
	{
		/*
		 * 临时关系仅在本会话中可访问，因此简单的
		 * 后端本地计数器即可。
		 */
		static XLogRecPtr counter = FirstNormalUnloggedLSN;

		return counter++;
	}
	else if (RelationIsPermanent(rel))
	{
		/*
		 * 该关系的 WAL 记录将在提交后开始，因此其 LSNs
		 * 必须是小于下次提交处 LSN 的互异编号。
		 * 若自上次调用后 insert-LSN 未前进，则发出
		 * 一条哑 WAL 记录。
		 */
		static XLogRecPtr lastlsn = InvalidXLogRecPtr;
		XLogRecPtr	currlsn = GetXLogInsertRecPtr();

		/* 不应为做 WAL 记录的关系调用 */
		Assert(!RelationNeedsWAL(rel));

		/* 若已有互异的 LSN，则无需实际记录 */
		if (!XLogRecPtrIsInvalid(lastlsn) && lastlsn == currlsn)
			currlsn = gistXLogAssignLSN();

		lastlsn = currlsn;
		return currlsn;
	}
	else
	{
		/*
		 * 未记录关系可从其他后端访问，并能在（干净的）
		 * 重启后存活。GetFakeLSNForUnloggedRel() 为我们处理。
		 */
		Assert(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED);
		return GetFakeLSNForUnloggedRel();
	}
}
