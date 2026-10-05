/*-------------------------------------------------------------------------
 *
 * ginutil.c
 *	  Postgres 倒排索引访问方法的实用例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginutil.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/reloptions.h"
#include "access/xloginsert.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/index_selfuncs.h"
#include "utils/typcache.h"


/*
 * GIN 处理函数：返回带有访问方法参数
 * 和回调的 IndexAmRoutine。
 */
Datum
ginhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = GINNProcs;
	amroutine->amoptsprocnum = GIN_OPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = true;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = true;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = ginbuild;
	amroutine->ambuildempty = ginbuildempty;
	amroutine->aminsert = gininsert;
	amroutine->ambulkdelete = ginbulkdelete;
	amroutine->amvacuumcleanup = ginvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = gincostestimate;
	amroutine->amoptions = ginoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = ginvalidate;
	amroutine->amadjustmembers = ginadjustmembers;
	amroutine->ambeginscan = ginbeginscan;
	amroutine->amrescan = ginrescan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = gingetbitmap;
	amroutine->amendscan = ginendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * initGinState：填写空的 GinState 结构以描述该索引
 *
 * 注意：各种附属数据在 CurrentMemoryContext 中分配。
 */
void
initGinState(GinState *state, Relation index)
{
	TupleDesc	origTupdesc = RelationGetDescr(index);
	int			i;

	MemSet(state, 0, sizeof(GinState));

	state->index = index;
	state->oneCol = (origTupdesc->natts == 1);
	state->origTupdesc = origTupdesc;

	for (i = 0; i < origTupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(origTupdesc, i);

		if (state->oneCol)
			state->tupdesc[i] = state->origTupdesc;
		else
		{
			state->tupdesc[i] = CreateTemplateTupleDesc(2);

			TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 1, NULL,
							   INT2OID, -1, 0);
			TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 2, NULL,
							   attr->atttypid,
							   attr->atttypmod,
							   attr->attndims);
			TupleDescInitEntryCollation(state->tupdesc[i], (AttrNumber) 2,
										attr->attcollation);
		}

		/*
		 * 若操作符类定义中未指定比较支持函数，则查找
		 * 索引键类型的默认 btree 比较器。
		 */
		if (index_getprocid(index, i + 1, GIN_COMPARE_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->compareFn[i]),
						   index_getprocinfo(index, i + 1, GIN_COMPARE_PROC),
						   CurrentMemoryContext);
		}
		else
		{
			TypeCacheEntry *typentry;

			typentry = lookup_type_cache(attr->atttypid,
										 TYPECACHE_CMP_PROC_FINFO);
			if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify a comparison function for type %s",
								format_type_be(attr->atttypid))));
			fmgr_info_copy(&(state->compareFn[i]),
						   &(typentry->cmp_proc_finfo),
						   CurrentMemoryContext);
		}

		/* 操作符类必须始终提供提取支持函数 */
		fmgr_info_copy(&(state->extractValueFn[i]),
					   index_getprocinfo(index, i + 1, GIN_EXTRACTVALUE_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(state->extractQueryFn[i]),
					   index_getprocinfo(index, i + 1, GIN_EXTRACTQUERY_PROC),
					   CurrentMemoryContext);

		/*
		 * 检查操作符类是否具备做三值逻辑或二值逻辑
		 * 一致性检查的能力。
		 */
		if (index_getprocid(index, i + 1, GIN_TRICONSISTENT_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->triConsistentFn[i]),
						   index_getprocinfo(index, i + 1, GIN_TRICONSISTENT_PROC),
						   CurrentMemoryContext);
		}

		if (index_getprocid(index, i + 1, GIN_CONSISTENT_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->consistentFn[i]),
						   index_getprocinfo(index, i + 1, GIN_CONSISTENT_PROC),
						   CurrentMemoryContext);
		}

		if (state->consistentFn[i].fn_oid == InvalidOid &&
			state->triConsistentFn[i].fn_oid == InvalidOid)
		{
			elog(ERROR, "missing GIN support function (%d or %d) for attribute %d of index \"%s\"",
				 GIN_CONSISTENT_PROC, GIN_TRICONSISTENT_PROC,
				 i + 1, RelationGetRelationName(index));
		}

		/*
		 * 检查操作符类是否具备做部分匹配的能力。
		 */
		if (index_getprocid(index, i + 1, GIN_COMPARE_PARTIAL_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->comparePartialFn[i]),
						   index_getprocinfo(index, i + 1, GIN_COMPARE_PARTIAL_PROC),
						   CurrentMemoryContext);
			state->canPartialMatch[i] = true;
		}
		else
		{
			state->canPartialMatch[i] = false;
		}

		/*
		 * 若索引列指定了排序规则，比较时应当遵守它。
		 * 不过，不支持排序规则的被索引数据类型，也可能拥有
		 * 支持排序规则的存储类型（例如 hstore
		 * 使用 text 索引条目）。若没有索引排序规则，
		 * 则指定默认排序规则，以防支持函数需要
		 * 排序规则。支持函数若不关心排序规则，
		 * 这样做也无害，因此无条件执行。（也可以改为
		 * 调用 get_typcollation，但那似乎代价过高、
		 * 多此一举 --- 不会出现 GIN 存储类型
		 * 使用非默认排序规则的情形。）
		 */
		if (OidIsValid(index->rd_indcollation[i]))
			state->supportCollation[i] = index->rd_indcollation[i];
		else
			state->supportCollation[i] = DEFAULT_COLLATION_OID;
	}
}

/*
 * 从 GIN 元组中提取所存条目的属性（列）号
 */
OffsetNumber
gintuple_get_attrnum(GinState *ginstate, IndexTuple tuple)
{
	OffsetNumber colN;

	if (ginstate->oneCol)
	{
		/* 列号并未显式存储 */
		colN = FirstOffsetNumber;
	}
	else
	{
		Datum		res;
		bool		isnull;

		/*
		 * 第一个属性始终是 int16，因此可以安全地使用任意
		 * 元组描述符来取得元组的第一个属性
		 */
		res = index_getattr(tuple, FirstOffsetNumber, ginstate->tupdesc[0],
							&isnull);
		Assert(!isnull);

		colN = DatumGetUInt16(res);
		Assert(colN >= FirstOffsetNumber && colN <= ginstate->origTupdesc->natts);
	}

	return colN;
}

/*
 * 从 GIN 元组中提取所存 datum（以及可能的空类别）
 */
Datum
gintuple_get_key(GinState *ginstate, IndexTuple tuple,
				 GinNullCategory *category)
{
	Datum		res;
	bool		isnull;

	if (ginstate->oneCol)
	{
		/*
		 * 单列索引不在元组里存储属性号
		 */
		res = index_getattr(tuple, FirstOffsetNumber, ginstate->origTupdesc,
							&isnull);
	}
	else
	{
		/*
		 * 由于 datum 类型取决于它来自哪一索引列，这里
		 * 必须小心地使用正确的元组描述符。
		 */
		OffsetNumber colN = gintuple_get_attrnum(ginstate, tuple);

		res = index_getattr(tuple, OffsetNumberNext(FirstOffsetNumber),
							ginstate->tupdesc[colN - 1],
							&isnull);
	}

	if (isnull)
		*category = GinGetNullCategory(tuple, ginstate);
	else
		*category = GIN_CAT_NORM_KEY;

	return res;
}

/*
 * 分配一个新页（回收已有页，或扩展索引文件）
 * 返回的缓冲区已经钉住并排他锁定
 * 调用者负责调用 GinInitBuffer 来初始化该页
 */
Buffer
GinNewBuffer(Relation index)
{
	Buffer		buffer;

	/* 首先，尝试从 FSM 取得一页 */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (blkno == InvalidBlockNumber)
			break;

		buffer = ReadBuffer(index, blkno);

		/*
		 * 必须防备别人已经
		 * 回收了这一页；若是如此，缓冲区可能已被锁定。
		 */
		if (ConditionalLockBuffer(buffer))
		{
			if (GinPageIsRecyclable(BufferGetPage(buffer)))
				return buffer;	/* 可以使用 */

			LockBuffer(buffer, GIN_UNLOCK);
		}

		/* 无法使用，于是释放缓冲区并重试 */
		ReleaseBuffer(buffer);
	}

	/* 必须扩展该文件 */
	buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);

	return buffer;
}

void
GinInitPage(Page page, uint32 f, Size pageSize)
{
	GinPageOpaque opaque;

	PageInit(page, pageSize, sizeof(GinPageOpaqueData));

	opaque = GinPageGetOpaque(page);
	opaque->flags = f;
	opaque->rightlink = InvalidBlockNumber;
}

void
GinInitBuffer(Buffer b, uint32 f)
{
	GinInitPage(BufferGetPage(b), f, BufferGetPageSize(b));
}

void
GinInitMetabuffer(Buffer b)
{
	GinMetaPageData *metadata;
	Page		page = BufferGetPage(b);

	GinInitPage(page, GIN_META, BufferGetPageSize(b));

	metadata = GinPageGetMeta(page);

	metadata->head = metadata->tail = InvalidBlockNumber;
	metadata->tailFreeSize = 0;
	metadata->nPendingPages = 0;
	metadata->nPendingHeapTuples = 0;
	metadata->nTotalPages = 0;
	metadata->nEntryPages = 0;
	metadata->nDataPages = 0;
	metadata->nEntries = 0;
	metadata->ginVersion = GIN_CURRENT_VERSION;

	/*
	 * 把 pd_lower 设到元数据末尾之后。这必不可少，
	 * 因为若不这样做，xlog.c 压缩该页时
	 * 元数据就会丢失。
	 */
	((PageHeader) page)->pd_lower =
		((char *) metadata + sizeof(GinMetaPageData)) - (char *) page;
}

/*
 * 比较同一索引列的两个键
 */
int
ginCompareEntries(GinState *ginstate, OffsetNumber attnum,
				  Datum a, GinNullCategory categorya,
				  Datum b, GinNullCategory categoryb)
{
	/* 若空类别不同，则先按空类别排序 */
	if (categorya != categoryb)
		return (categorya < categoryb) ? -1 : 1;

	/* 同一类别中的所有空项都相等 */
	if (categorya != GIN_CAT_NORM_KEY)
		return 0;

	/* 两者都非空，因此可以安全调用 compareFn */
	return DatumGetInt32(FunctionCall2Coll(&ginstate->compareFn[attnum - 1],
										   ginstate->supportCollation[attnum - 1],
										   a, b));
}

/*
 * 比较可能属于不同索引列的两个键
 */
int
ginCompareAttEntries(GinState *ginstate,
					 OffsetNumber attnuma, Datum a, GinNullCategory categorya,
					 OffsetNumber attnumb, Datum b, GinNullCategory categoryb)
{
	/* 属性号是第一排序键 */
	if (attnuma != attnumb)
		return (attnuma < attnumb) ? -1 : 1;

	return ginCompareEntries(ginstate, attnuma, a, categorya, b, categoryb);
}


/*
 * 为 ginExtractEntries 里的键 datum 排序提供支持
 *
 * 注意：这里只需考虑空键与非空键；
 * ginExtractEntries 生成的占位空值从不超过一个，
 * 因此不必对它们排序。
 */
typedef struct
{
	Datum		datum;
	bool		isnull;
} keyEntryData;

typedef struct
{
	FmgrInfo   *cmpDatumFunc;
	Oid			collation;
	bool		haveDups;
} cmpEntriesArg;

static int
cmpEntries(const void *a, const void *b, void *arg)
{
	const keyEntryData *aa = (const keyEntryData *) a;
	const keyEntryData *bb = (const keyEntryData *) b;
	cmpEntriesArg *data = (cmpEntriesArg *) arg;
	int			res;

	if (aa->isnull)
	{
		if (bb->isnull)
			res = 0;			/* NULL 等于 NULL */
		else
			res = 1;			/* NULL ">" 非 NULL */
	}
	else if (bb->isnull)
		res = -1;				/* 非 NULL "<" NULL */
	else
		res = DatumGetInt32(FunctionCall2Coll(data->cmpDatumFunc,
											  data->collation,
											  aa->datum, bb->datum));

	/*
	 * 检测是否有重复。若存在相等的键，qsort 必定会
	 * 在某处比较它们，否则无法知道其中一个应排在
	 * 另一个之前还是之后。
	 */
	if (res == 0)
		data->haveDups = true;

	return res;
}


/*
 * 从可索引项中提取索引键值
 *
 * 所得键值已排序，并去掉任何重复。
 * 这样可避免生成冗余的索引条目。
 */
Datum *
ginExtractEntries(GinState *ginstate, OffsetNumber attnum,
				  Datum value, bool isNull,
				  int32 *nentries, GinNullCategory **categories)
{
	Datum	   *entries;
	bool	   *nullFlags;
	int32		i;

	/*
	 * 对空项不调用 extractValueFn，改为生成一个
	 * 占位。
	 */
	if (isNull)
	{
		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		entries[0] = (Datum) 0;
		*categories = (GinNullCategory *) palloc(sizeof(GinNullCategory));
		(*categories)[0] = GIN_CAT_NULL_ITEM;
		return entries;
	}

	/* 于是调用操作符类的 extractValueFn */
	nullFlags = NULL;			/* 以防 extractValue 未设置它 */
	entries = (Datum *)
		DatumGetPointer(FunctionCall3Coll(&ginstate->extractValueFn[attnum - 1],
										  ginstate->supportCollation[attnum - 1],
										  value,
										  PointerGetDatum(nentries),
										  PointerGetDatum(&nullFlags)));

	/*
	 * 若该项不含任何键，则生成一个占位。
	 */
	if (entries == NULL || *nentries <= 0)
	{
		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		entries[0] = (Datum) 0;
		*categories = (GinNullCategory *) palloc(sizeof(GinNullCategory));
		(*categories)[0] = GIN_CAT_EMPTY_ITEM;
		return entries;
	}

	/*
	 * 若 extractValueFn 没有创建 nullFlags 数组，则创建一个，
	 * 并假定全部都非空。
	 */
	if (nullFlags == NULL)
		nullFlags = (bool *) palloc0(*nentries * sizeof(bool));

	/*
	 * 若键不止一个，则排序并去重。
	 *
	 * XXX 这里用 qsort 在写法上很别扭，开销也
	 * 相当大。键的个数较少时，改用简单的
	 * 插入排序很可能更好。
	 */
	if (*nentries > 1)
	{
		keyEntryData *keydata;
		cmpEntriesArg arg;

		keydata = (keyEntryData *) palloc(*nentries * sizeof(keyEntryData));
		for (i = 0; i < *nentries; i++)
		{
			keydata[i].datum = entries[i];
			keydata[i].isnull = nullFlags[i];
		}

		arg.cmpDatumFunc = &ginstate->compareFn[attnum - 1];
		arg.collation = ginstate->supportCollation[attnum - 1];
		arg.haveDups = false;
		qsort_arg(keydata, *nentries, sizeof(keyEntryData),
				  cmpEntries, &arg);

		if (arg.haveDups)
		{
			/* 有重复，必须去掉它们 */
			int32		j;

			entries[0] = keydata[0].datum;
			nullFlags[0] = keydata[0].isnull;
			j = 1;
			for (i = 1; i < *nentries; i++)
			{
				if (cmpEntries(&keydata[i - 1], &keydata[i], &arg) != 0)
				{
					entries[j] = keydata[i].datum;
					nullFlags[j] = keydata[i].isnull;
					j++;
				}
			}
			*nentries = j;
		}
		else
		{
			/* 容易，没有重复 */
			for (i = 0; i < *nentries; i++)
			{
				entries[i] = keydata[i].datum;
				nullFlags[i] = keydata[i].isnull;
			}
		}

		pfree(keydata);
	}

	/*
	 * 由 nullFlags 建立 GinNullCategory 表示。
	 */
	*categories = (GinNullCategory *) palloc0(*nentries * sizeof(GinNullCategory));
	for (i = 0; i < *nentries; i++)
		(*categories)[i] = (nullFlags[i] ? GIN_CAT_NULL_KEY : GIN_CAT_NORM_KEY);

	return entries;
}

bytea *
ginoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fastupdate", RELOPT_TYPE_BOOL, offsetof(GinOptions, useFastUpdate)},
		{"gin_pending_list_limit", RELOPT_TYPE_INT, offsetof(GinOptions,
															 pendingListCleanupSize)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_GIN,
									  sizeof(GinOptions),
									  tab, lengthof(tab));
}

/*
 * 把索引的统计数据取入 *stats
 *
 * 注意：结果中 nPendingPages 可以相信是最新的，
 * ginVersion 也是；其余字段则是最近一次 VACUUM 时的值。
 */
void
ginGetStats(Relation index, GinStatsData *stats)
{
	Buffer		metabuffer;
	Page		metapage;
	GinMetaPageData *metadata;

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	LockBuffer(metabuffer, GIN_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = GinPageGetMeta(metapage);

	stats->nPendingPages = metadata->nPendingPages;
	stats->nTotalPages = metadata->nTotalPages;
	stats->nEntryPages = metadata->nEntryPages;
	stats->nDataPages = metadata->nDataPages;
	stats->nEntries = metadata->nEntries;
	stats->ginVersion = metadata->ginVersion;

	UnlockReleaseBuffer(metabuffer);
}

/*
 * 把给定统计信息写入索引的元页
 *
 * 注意：nPendingPages 和 ginVersion *不会*被复制过去
 */
void
ginUpdateStats(Relation index, const GinStatsData *stats, bool is_build)
{
	Buffer		metabuffer;
	Page		metapage;
	GinMetaPageData *metadata;

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	LockBuffer(metabuffer, GIN_EXCLUSIVE);
	metapage = BufferGetPage(metabuffer);
	metadata = GinPageGetMeta(metapage);

	START_CRIT_SECTION();

	metadata->nTotalPages = stats->nTotalPages;
	metadata->nEntryPages = stats->nEntryPages;
	metadata->nDataPages = stats->nDataPages;
	metadata->nEntries = stats->nEntries;

	/*
	 * 把 pd_lower 设到元数据末尾之后。这必不可少，
	 * 因为若不这样做，xlog.c 压缩
	 * 该页时元数据就会丢失。（必须在这里设置，因为 v11 之前的 PG 并未
	 * 正确设置元页的 pd_lower，所以经 pg_upgraded 升级的索引可能
	 * 含有错误的值。）
	 */
	((PageHeader) metapage)->pd_lower =
		((char *) metadata + sizeof(GinMetaPageData)) - (char *) metapage;

	MarkBufferDirty(metabuffer);

	if (RelationNeedsWAL(index) && !is_build)
	{
		XLogRecPtr	recptr;
		ginxlogUpdateMeta data;

		data.locator = index->rd_locator;
		data.ntuples = 0;
		data.newRightlink = data.prevTail = InvalidBlockNumber;
		memcpy(&data.metadata, metadata, sizeof(GinMetaPageData));

		XLogBeginInsert();
		XLogRegisterData((char *) &data, sizeof(ginxlogUpdateMeta));
		XLogRegisterBuffer(0, metabuffer, REGBUF_WILL_INIT | REGBUF_STANDARD);

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_UPDATE_META_PAGE);
		PageSetLSN(metapage, recptr);
	}

	UnlockReleaseBuffer(metabuffer);

	END_CRIT_SECTION();
}
