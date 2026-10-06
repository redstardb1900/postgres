/*-------------------------------------------------------------------------
 *
 * spgutils.c
 *	  SP-GiST 的各类支持函数
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgutils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/spgist_private.h"
#include "access/toast_compression.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_amop.h"
#include "commands/vacuum.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * SP-GiST 处理函数：返回带有访问方法参数与回调的
 * IndexAmRoutine。
 */
Datum
spghandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = SPGISTNProc;
	amroutine->amoptsprocnum = SPGIST_OPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = true;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = true;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_COND_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = spgbuild;
	amroutine->ambuildempty = spgbuildempty;
	amroutine->aminsert = spginsert;
	amroutine->ambulkdelete = spgbulkdelete;
	amroutine->amvacuumcleanup = spgvacuumcleanup;
	amroutine->amcanreturn = spgcanreturn;
	amroutine->amcostestimate = spgcostestimate;
	amroutine->amoptions = spgoptions;
	amroutine->amproperty = spgproperty;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = spgvalidate;
	amroutine->amadjustmembers = spgadjustmembers;
	amroutine->ambeginscan = spgbeginscan;
	amroutine->amrescan = spgrescan;
	amroutine->amgettuple = spggettuple;
	amroutine->amgetbitmap = spggetbitmap;
	amroutine->amendscan = spgendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * GetIndexInputType
 *		确定索引列的名义输入数据类型
 *
 * 我们将 "名义" 输入类型定义为关联操作类的 opcintype，
 * 若其为多态类型，则取作为索引输入的堆列或
 * 表达式的基类型。优先使用 opcintype 的原因是：
 * 非多态操作类通常不希望听到二进制兼容的输入类型。
 * 例如，若 text 操作类用于 varchar 堆列，我们希望报告
 * "text" 而非 "varchar"。同样，操作类也不希望听到
 * 域类型，因此若确实查阅实际输入类型，我们会确保
 * 展平域。
 *
 * 将来或许应放到别处，但尚不清楚其他索引访问方法
 * 是否用得上。
 */
static Oid
GetIndexInputType(Relation index, AttrNumber indexcol)
{
	Oid			opcintype;
	AttrNumber	heapcol;
	List	   *indexprs;
	ListCell   *indexpr_item;

	Assert(index->rd_index != NULL);
	Assert(indexcol > 0 && indexcol <= index->rd_index->indnkeyatts);
	opcintype = index->rd_opcintype[indexcol - 1];
	if (!IsPolymorphicType(opcintype))
		return opcintype;
	heapcol = index->rd_index->indkey.values[indexcol - 1];
	if (heapcol != 0)			/* 简单索引列？ */
		return getBaseType(get_atttype(index->rd_index->indrelid, heapcol));

	/*
	 * 若索引表达式已缓存，则跳过调用
	 * RelationGetIndexExpressions，因为它会做一次不必要的拷贝。
	 * 我们不会修改这些树，也不会在完成前做任何会使
	 * relcache 项失效的事。
	 */
	if (index->rd_indexprs)
		indexprs = index->rd_indexprs;
	else
		indexprs = RelationGetIndexExpressions(index);
	indexpr_item = list_head(indexprs);
	for (int i = 1; i <= index->rd_index->indnkeyatts; i++)
	{
		if (index->rd_index->indkey.values[i - 1] == 0)
		{
			/* 表达式列 */
			if (indexpr_item == NULL)
				elog(ERROR, "wrong number of index expressions");
			if (i == indexcol)
				return getBaseType(exprType((Node *) lfirst(indexpr_item)));
			indexpr_item = lnext(indexprs, indexpr_item);
		}
	}
	elog(ERROR, "wrong number of index expressions");
	return InvalidOid;			/* 让编译器安静 */
}

/* 用指定数据类型的信息填充 SpGistTypeDesc 结构 */
static void
fillTypeDesc(SpGistTypeDesc *desc, Oid type)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	desc->type = type;
	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", type);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	desc->attlen = typtup->typlen;
	desc->attbyval = typtup->typbyval;
	desc->attalign = typtup->typalign;
	desc->attstorage = typtup->typstorage;
	ReleaseSysCache(tp);
}

/*
 * 获取索引的 AM 特有信息本地缓存，必要时
 * 进行初始化
 */
SpGistCache *
spgGetCache(Relation index)
{
	SpGistCache *cache;

	if (index->rd_amcache == NULL)
	{
		Oid			atttype;
		spgConfigIn in;
		FmgrInfo   *procinfo;

		cache = MemoryContextAllocZero(index->rd_indexcxt,
									   sizeof(SpGistCache));

		/* SPGiST 必须有一列键列，也可有 INCLUDE 列 */
		Assert(IndexRelationGetNumberOfKeyAttributes(index) == 1);
		Assert(IndexRelationGetNumberOfAttributes(index) <= INDEX_MAX_KEYS);

		/*
		 * 取得键列的实际（嗯，名义）数据类型。我们
		 * 把它传给操作类的配置函数，以使多态
		 * 操作类成为可能。
		 */
		atttype = GetIndexInputType(index, spgKeyColumn + 1);

		/* 调用配置函数以获取操作类的配置信息 */
		in.attType = atttype;

		procinfo = index_getprocinfo(index, 1, SPGIST_CONFIG_PROC);
		FunctionCall2Coll(procinfo,
						  index->rd_indcollation[spgKeyColumn],
						  PointerGetDatum(&in),
						  PointerGetDatum(&cache->config));

		/*
		 * 若未指定 leafType，则使用已声明的索引列类型，
		 * 该类型由 index.c 从操作类的 opcintype 导出。
		 * （尽管现在 spgvalidate.c 会在二者不同时发出警告，
		 * 旧的用户定义操作类可能未正确设置 STORAGE 参数，
		 * 因此若给出了 leafType 则相信它。）
		 */
		if (!OidIsValid(cache->config.leafType))
		{
			cache->config.leafType =
				TupleDescAttr(RelationGetDescr(index), spgKeyColumn)->atttypid;

			/*
			 * 若索引列类型可二进制强制转换为 atttype（例如
			 * 它是 atttype 上的域），则将其视为普通 atttype，
			 * 以免误以为需要压缩。
			 */
			if (cache->config.leafType != atttype &&
				IsBinaryCoercible(cache->config.leafType, atttype))
				cache->config.leafType = atttype;
		}

		/* 获取各相关数据类型所需的信息 */
		fillTypeDesc(&cache->attType, atttype);

		if (cache->config.leafType != atttype)
		{
			if (!OidIsValid(index_getprocid(index, 1, SPGIST_COMPRESS_PROC)))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("compress method must be defined when leaf type is different from input type")));

			fillTypeDesc(&cache->attLeafType, cache->config.leafType);
		}
		else
		{
			/* 在此常见情况下节省查找 */
			cache->attLeafType = cache->attType;
		}

		fillTypeDesc(&cache->attPrefixType, cache->config.prefixType);
		fillTypeDesc(&cache->attLabelType, cache->config.labelType);

		/*
		 * 最后，若为真实索引（而非分区索引），从元页
		 * 读取 lastUsedPages 数据
		 */
		if (index->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			Buffer		metabuffer;
			SpGistMetaPageData *metadata;

			metabuffer = ReadBuffer(index, SPGIST_METAPAGE_BLKNO);
			LockBuffer(metabuffer, BUFFER_LOCK_SHARE);

			metadata = SpGistPageGetMeta(BufferGetPage(metabuffer));

			if (metadata->magicNumber != SPGIST_MAGIC_NUMBER)
				elog(ERROR, "index \"%s\" is not an SP-GiST index",
					 RelationGetRelationName(index));

			cache->lastUsedPages = metadata->lastUsedPages;

			UnlockReleaseBuffer(metabuffer);
		}

		index->rd_amcache = (void *) cache;
	}
	else
	{
		/* 假定它是最新的 */
		cache = (SpGistCache *) index->rd_amcache;
	}

	return cache;
}

/*
 * 为叶子元组或仅索引扫描结果元组计算元组描述符。
 *
 * 在许多情况下可直接使用 relcache 的 tupdesc，且就任何
 * INCLUDE 列而言始终没问题。但键列条目在第一种情况下须
 * 匹配 leafType，在第二种情况下须匹配 attType。虽然
 * relcache 的 tupdesc *应当* 显示 leafType，但对遗留的
 * 用户定义操作类未必成立，因为在 v14 之前它们不允许在
 * CREATE OPCLASS 中声明真实存储类型。此外，attType
 * 也可能与 relcache 中的不同。
 *
 * 本函数返回指向 relcache tupdesc 的指针（若合适），
 * 或返回经调整以匹配指定键列类型的 palloc 拷贝。通过
 * 要求调用方传入 SpGistTypeDesc 而非仅 OID，此处可避免
 * 任何目录查找。
 */
TupleDesc
getSpGistTupleDesc(Relation index, SpGistTypeDesc *keyType)
{
	TupleDesc	outTupDesc;
	Form_pg_attribute att;

	if (keyType->type ==
		TupleDescAttr(RelationGetDescr(index), spgKeyColumn)->atttypid)
		outTupDesc = RelationGetDescr(index);
	else
	{
		outTupDesc = CreateTupleDescCopy(RelationGetDescr(index));
		att = TupleDescAttr(outTupDesc, spgKeyColumn);
		/* 更新该列的类型相关字段即可 */
		att->atttypid = keyType->type;
		att->atttypmod = -1;
		att->attlen = keyType->attlen;
		att->attbyval = keyType->attbyval;
		att->attalign = keyType->attalign;
		att->attstorage = keyType->attstorage;
		/* 我们不必费心使这些字段有效： */
		att->attcompression = InvalidCompressionMethod;
		att->attcollation = InvalidOid;
		/* 若改了 typlen，最好重置后续偏移 */
		for (int i = spgFirstIncludeColumn; i < outTupDesc->natts; i++)
			TupleDescAttr(outTupDesc, i)->attcacheoff = -1;
	}
	return outTupDesc;
}

/* 为给定索引初始化 SpGistState */
void
initSpGistState(SpGistState *state, Relation index)
{
	SpGistCache *cache;

	state->index = index;

	/* 获取关于索引的已缓存静态信息 */
	cache = spgGetCache(index);

	state->config = cache->config;
	state->attType = cache->attType;
	state->attLeafType = cache->attLeafType;
	state->attPrefixType = cache->attPrefixType;
	state->attLabelType = cache->attLabelType;

	/* 确保我们有用于叶子元组的有效描述符 */
	state->leafTupDesc = getSpGistTupleDesc(state->index, &state->attLeafType);

	/* 为构造死元组准备工作区 */
	state->deadTupleStorage = palloc0(SGDTSIZE);

	/*
	 * 设置重定向元组中使用的视界 XID。若有则用我们自己的
	 * XID，否则用 InvalidTransactionId。后一种情况可能发生在
	 * VACUUM 或 REINDEX CONCURRENTLY 中，这两种情况都不宜
	 * 强制分配 XID。VACUUM 反正不会创建重定向
	 * 元组，但 REINDEX CONCURRENTLY 会。幸好 REINDEX
	 * CONCURRENTLY 直到结束才把索引标为有效，因此不会有
	 * 并发扫描 "在途" 飞向它所插入的重定向元组。而且它
	 * 也会一直锁住 VACUUM 到结束。因此 VACUUM 立即
	 * 使含无效 xid 的重定向元组
	 * 过期是可以的。
	 */
	state->myXid = GetTopTransactionIdIfAny();

	/* 假定不在索引构建中（spgbuild 会覆盖） */
	state->isBuild = false;
}

/*
 * 分配新页（通过回收，或通过扩展索引文件）。
 *
 * 返回的缓冲区已钉住并独占锁定。
 * 调用方负责通过调用 SpGistInitBuffer 初始化该页。
 */
Buffer
SpGistNewBuffer(Relation index)
{
	Buffer		buffer;

	/* 首先尝试从 FSM 获取一页 */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (blkno == InvalidBlockNumber)
			break;				/* FSM 中没有已知空闲页 */

		/*
		 * 固定页本不应出现在 FSM 中，但以防其中有一页，
		 * 忽略它。
		 */
		if (SpGistBlockIsFixed(blkno))
			continue;

		buffer = ReadBuffer(index, blkno);

		/*
		 * 必须防范他人已回收该页的可能；若如此，缓冲区
		 * 可能已被锁定。
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			if (PageIsNew(page))
				return buffer;	/* 若从未初始化则可使用 */

			if (SpGistPageIsDeleted(page) || PageIsEmpty(page))
				return buffer;	/* 可以使用 */

			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		}

		/* 不能用，释放缓冲区再试 */
		ReleaseBuffer(buffer);
	}

	buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);

	return buffer;
}

/*
 * 若可能，用本地缓存更新索引元页的 lastUsedPages 信息
 *
 * 更新元页对索引工作并非关键，因此
 * 1 使用 ConditionalLockBuffer 以提高并发
 * 2 不对 metabuffer 变更记 WAL，以减少 WAL 流量
 */
void
SpGistUpdateMetaPage(Relation index)
{
	SpGistCache *cache = (SpGistCache *) index->rd_amcache;

	if (cache != NULL)
	{
		Buffer		metabuffer;

		metabuffer = ReadBuffer(index, SPGIST_METAPAGE_BLKNO);

		if (ConditionalLockBuffer(metabuffer))
		{
			Page		metapage = BufferGetPage(metabuffer);
			SpGistMetaPageData *metadata = SpGistPageGetMeta(metapage);

			metadata->lastUsedPages = cache->lastUsedPages;

			/*
			 * 将 pd_lower 设为刚好越过元数据末尾。这很
			 * 关键，因为若不这样做，在 xlog.c 压缩页面时
			 * 元数据会丢失。（必须在此设置，因为
			 * v11 之前的 PG 未正确设置元页的 pd_lower，
			 * 因此 pg_upgraded 索引可能含有错误的
			 * 值。）
			 */
			((PageHeader) metapage)->pd_lower =
				((char *) metadata + sizeof(SpGistMetaPageData)) - (char *) metapage;

			MarkBufferDirty(metabuffer);
			UnlockReleaseBuffer(metabuffer);
		}
		else
		{
			ReleaseBuffer(metabuffer);
		}
	}
}

/* 按 flags 选择 lastUsedPages 缓存中正确元素的宏 */
/* 用 SPGIST_CACHED_PAGES 掩码 flags 只是出于偏执 */
#define GET_LUP(c, f)  (&(c)->lastUsedPages.cachedPage[((unsigned int) (f)) % SPGIST_CACHED_PAGES])

/*
 * 按 flags 指定的类型与奇偶分配并初始化新缓冲区。
 * 返回的缓冲区已钉住并独占锁定。
 *
 * 请求内部页时，若得到奇偶不符的页，
 * 则释放缓冲区再试。我们会得到不同的页，
 * 因为 GetFreeIndexPage 已在 FSM 中将该页标为已用。该页
 * 会记入我们本地的 lastUsedPages 缓存，因此本会话稍后
 * 还有希望用到它；否则我们依赖 VACUUM
 * 最终把该页重新放回 FSM，使其可供回收。
 * 注意此处不会将该页标脏，因此除非很快被使用，
 * 缓冲区会被丢弃，页在磁盘上
 * 保持原样。
 *
 * 向调用方返回缓冲区时，该页*不会*记入
 * lastUsedPages 缓存；我们期望调用方在占用所需空间后再
 * 记入。因为调用方用掉一些空间后，该页的剩余空间可能
 * 少于已缓存的条目，我们宁可不要冲掉旧的
 * 缓存项。
 */
static Buffer
allocNewBuffer(Relation index, int flags)
{
	SpGistCache *cache = spgGetCache(index);
	uint16		pageflags = 0;

	if (GBUF_REQ_LEAF(flags))
		pageflags |= SPGIST_LEAF;
	if (GBUF_REQ_NULLS(flags))
		pageflags |= SPGIST_NULLS;

	for (;;)
	{
		Buffer		buffer;

		buffer = SpGistNewBuffer(index);
		SpGistInitBuffer(buffer, pageflags);

		if (pageflags & SPGIST_LEAF)
		{
			/* 叶子页没有奇偶问题，直接用它 */
			return buffer;
		}
		else
		{
			BlockNumber blkno = BufferGetBlockNumber(buffer);
			int			blkFlags = GBUF_INNER_PARITY(blkno);

			if ((flags & GBUF_PARITY_MASK) == blkFlags)
			{
				/* 页奇偶正确，使用它 */
				return buffer;
			}
			else
			{
				/* 页奇偶错误，记入缓存再试 */
				if (pageflags & SPGIST_NULLS)
					blkFlags |= GBUF_NULLS;
				cache->lastUsedPages.cachedPage[blkFlags].blkno = blkno;
				cache->lastUsedPages.cachedPage[blkFlags].freeSpace =
					PageGetExactFreeSpace(BufferGetPage(buffer));
				UnlockReleaseBuffer(buffer);
			}
		}
	}
}

/*
 * 获取 flags 指定类型与奇偶、且至少有 needSpace 所示
 * 空闲空间的缓冲区。我们用 lastUsedPages
 * 缓存，尽可能分配先前请求过的同一缓冲区。
 * 返回的缓冲区已钉住并独占锁定。
 *
 * 若页在此初始化则 *isNew 设为 true，若原本已有效则
 * 设为 false。
 */
Buffer
SpGistGetBuffer(Relation index, int flags, int needSpace, bool *isNew)
{
	SpGistCache *cache = spgGetCache(index);
	SpGistLastUsedPage *lup;

	/* 即便空页也无法满足需求则退出 */
	if (needSpace > SPGIST_PAGE_CAPACITY)
		elog(ERROR, "desired SPGiST tuple size is too big");

	/*
	 * 若可能，增大空间请求以计入关系的
	 * fillfactor。这确保向页添加无关元组时，
	 * 我们仍尝试保留 100-fillfactor% 的空间，供添加与页上
	 * 已有元组相关的元组。但 fillfactor 不得使
	 * 原本合法的请求出错。
	 */
	needSpace += SpGistGetTargetPageFreeSpace(index);
	needSpace = Min(needSpace, SPGIST_PAGE_CAPACITY);

	/* 获取此 flags 设置对应的缓存项 */
	lup = GET_LUP(cache, flags);

	/* 若无缓存则直接交给 allocNewBuffer */
	if (lup->blkno == InvalidBlockNumber)
	{
		*isNew = true;
		return allocNewBuffer(index, flags);
	}

	/* 固定页绝不应当在缓存中 */
	Assert(!SpGistBlockIsFixed(lup->blkno));

	/* 若缓存的 freeSpace 不够，不必查看该页 */
	if (lup->freeSpace >= needSpace)
	{
		Buffer		buffer;
		Page		page;

		buffer = ReadBuffer(index, lup->blkno);

		if (!ConditionalLockBuffer(buffer))
		{
			/*
			 * 缓冲区被另一进程锁定，因此返回新缓冲区
			 */
			ReleaseBuffer(buffer);
			*isNew = true;
			return allocNewBuffer(index, flags);
		}

		page = BufferGetPage(buffer);

		if (PageIsNew(page) || SpGistPageIsDeleted(page) || PageIsEmpty(page))
		{
			/* 可以初始化该页 */
			uint16		pageflags = 0;

			if (GBUF_REQ_LEAF(flags))
				pageflags |= SPGIST_LEAF;
			if (GBUF_REQ_NULLS(flags))
				pageflags |= SPGIST_NULLS;
			SpGistInitBuffer(buffer, pageflags);
			lup->freeSpace = PageGetExactFreeSpace(page) - needSpace;
			*isNew = true;
			return buffer;
		}

		/*
		 * 检查页类型正确且有足够空间。必须
		 * 重新检查，因为我们的缓存不一定是最新的。
		 */
		if ((GBUF_REQ_LEAF(flags) ? SpGistPageIsLeaf(page) : !SpGistPageIsLeaf(page)) &&
			(GBUF_REQ_NULLS(flags) ? SpGistPageStoresNulls(page) : !SpGistPageStoresNulls(page)))
		{
			int			freeSpace = PageGetExactFreeSpace(page);

			if (freeSpace >= needSpace)
			{
				/* 成功，更新空闲空间信息并返回缓冲区 */
				lup->freeSpace = freeSpace - needSpace;
				*isNew = false;
				return buffer;
			}
		}

		/*
		 * 回退到分配新缓冲区
		 */
		UnlockReleaseBuffer(buffer);
	}

	/* 缓存未成功，因此返回新缓冲区 */
	*isNew = true;
	return allocNewBuffer(index, flags);
}

/*
 * 修改页完成后更新 lastUsedPages 缓存。
 *
 * 若相应缓存项已含本页（其 freeSpace 很可能过时），或本页
 * 空间多于我们已缓存的，则更新该
 * 缓存项。
 */
void
SpGistSetLastUsedPage(Relation index, Buffer buffer)
{
	SpGistCache *cache = spgGetCache(index);
	SpGistLastUsedPage *lup;
	int			freeSpace;
	Page		page = BufferGetPage(buffer);
	BlockNumber blkno = BufferGetBlockNumber(buffer);
	int			flags;

	/* 不过绝不要把固定页（根页）放入缓存 */
	if (SpGistBlockIsFixed(blkno))
		return;

	if (SpGistPageIsLeaf(page))
		flags = GBUF_LEAF;
	else
		flags = GBUF_INNER_PARITY(blkno);
	if (SpGistPageStoresNulls(page))
		flags |= GBUF_NULLS;

	lup = GET_LUP(cache, flags);

	freeSpace = PageGetExactFreeSpace(page);
	if (lup->blkno == InvalidBlockNumber || lup->blkno == blkno ||
		lup->freeSpace < freeSpace)
	{
		lup->blkno = blkno;
		lup->freeSpace = freeSpace;
	}
}

/*
 * 将 SPGiST 页初始化为空，并带指定 flags
 */
void
SpGistInitPage(Page page, uint16 f)
{
	SpGistPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(SpGistPageOpaqueData));
	opaque = SpGistPageGetOpaque(page);
	opaque->flags = f;
	opaque->spgist_page_id = SPGIST_PAGE_ID;
}

/*
 * 将缓冲区的页初始化为空，并带指定 flags
 */
void
SpGistInitBuffer(Buffer b, uint16 f)
{
	Assert(BufferGetPageSize(b) == BLCKSZ);
	SpGistInitPage(BufferGetPage(b), f);
}

/*
 * 初始化元数据页
 */
void
SpGistInitMetapage(Page page)
{
	SpGistMetaPageData *metadata;
	int			i;

	SpGistInitPage(page, SPGIST_META);
	metadata = SpGistPageGetMeta(page);
	memset(metadata, 0, sizeof(SpGistMetaPageData));
	metadata->magicNumber = SPGIST_MAGIC_NUMBER;

	/* 将最近使用页缓存初始化为空 */
	for (i = 0; i < SPGIST_CACHED_PAGES; i++)
		metadata->lastUsedPages.cachedPage[i].blkno = InvalidBlockNumber;

	/*
	 * 将 pd_lower 设为刚好越过元数据末尾。这很关键，
	 * 因为若不这样做，在 xlog.c 压缩页面时元数据会
	 * 丢失。
	 */
	((PageHeader) page)->pd_lower =
		((char *) metadata + sizeof(SpGistMetaPageData)) - (char *) page;
}

/*
 * SPGiST 的 reloptions 处理
 */
bytea *
spgoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(SpGistOptions, fillfactor)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_SPGIST,
									  sizeof(SpGistOptions),
									  tab, lengthof(tab));
}

/*
 * 计算在内部元组中存储指定类型非空 datum 所需空间
 * （即作为前缀或节点标签）。
 * 注意结果已上取整到 MAXALIGN 边界。
 * 此处遵循按值传递类型仅以其 Datum 表示存储的约定
 * （参见 memcpyInnerDatum）。
 */
unsigned int
SpGistGetInnerTypeSize(SpGistTypeDesc *att, Datum datum)
{
	unsigned int size;

	if (att->attbyval)
		size = sizeof(Datum);
	else if (att->attlen > 0)
		size = att->attlen;
	else
		size = VARSIZE_ANY(datum);

	return MAXALIGN(size);
}

/*
 * 在内部元组情形下，将给定非空 datum 拷贝到 *target
 */
static void
memcpyInnerDatum(void *target, SpGistTypeDesc *att, Datum datum)
{
	unsigned int size;

	if (att->attbyval)
	{
		memcpy(target, &datum, sizeof(Datum));
	}
	else
	{
		size = (att->attlen > 0) ? att->attlen : VARSIZE_ANY(datum);
		memcpy(target, DatumGetPointer(datum), size);
	}
}

/*
 * 计算保存给定数据的叶子元组所需空间。
 *
 * 这必须与 spgFormLeafTuple 中计算大小的部分一致。
 */
Size
SpGistGetLeafTupleSize(TupleDesc tupleDescriptor,
					   Datum *datums, bool *isnulls)
{
	Size		size;
	Size		data_size;
	bool		needs_null_mask = false;
	int			natts = tupleDescriptor->natts;

	/*
	 * 决定是否需要空值位图。
	 *
	 * 若只有键属性（natts == 1），为与 v14 之前叶子元组布局
	 * 兼容，从不使用位图。否则，任一属性为空时
	 * 需要位图。
	 */
	if (natts > 1)
	{
		for (int i = 0; i < natts; i++)
		{
			if (isnulls[i])
			{
				needs_null_mask = true;
				break;
			}
		}
	}

	/*
	 * 计算数据部分大小；与堆元组相同。
	 */
	data_size = heap_compute_data_size(tupleDescriptor, datums, isnulls);

	/*
	 * 计算总大小。
	 */
	size = SGLTHDRSZ(needs_null_mask);
	size += data_size;
	size = MAXALIGN(size);

	/*
	 * 确保稍后可用死元组替换该元组。当存在任何非空属性时
	 * 此测试并非必要，但为安全起见仍做。
	 */
	if (size < SGDTSIZE)
		size = SGDTSIZE;

	return size;
}

/*
 * 构造包含给定堆 TID 与 datum 值的叶子元组
 */
SpGistLeafTuple
spgFormLeafTuple(SpGistState *state, ItemPointer heapPtr,
				 Datum *datums, bool *isnulls)
{
	SpGistLeafTuple tup;
	TupleDesc	tupleDescriptor = state->leafTupDesc;
	Size		size;
	Size		hoff;
	Size		data_size;
	bool		needs_null_mask = false;
	int			natts = tupleDescriptor->natts;
	char	   *tp;				/* 指向元组数据 */
	uint16		tupmask = 0;	/* 未使用的 heap_fill_tuple 输出 */

	/*
	 * 决定是否需要空值位图。
	 *
	 * 若只有键属性（natts == 1），为与 v14 之前叶子元组布局
	 * 兼容，从不使用位图。否则，任一属性为空时
	 * 需要位图。
	 */
	if (natts > 1)
	{
		for (int i = 0; i < natts; i++)
		{
			if (isnulls[i])
			{
				needs_null_mask = true;
				break;
			}
		}
	}

	/*
	 * 计算数据部分大小；与堆元组相同。
	 */
	data_size = heap_compute_data_size(tupleDescriptor, datums, isnulls);

	/*
	 * 计算总大小。
	 */
	hoff = SGLTHDRSZ(needs_null_mask);
	size = hoff + data_size;
	size = MAXALIGN(size);

	/*
	 * 确保稍后可用死元组替换该元组。当存在任何非空属性时
	 * 此测试并非必要，但为安全起见仍做。
	 */
	if (size < SGDTSIZE)
		size = SGDTSIZE;

	/* 好，形成元组 */
	tup = (SpGistLeafTuple) palloc0(size);

	tup->size = size;
	SGLT_SET_NEXTOFFSET(tup, InvalidOffsetNumber);
	tup->heapPtr = *heapPtr;

	tp = (char *) tup + hoff;

	if (needs_null_mask)
	{
		bits8	   *bp;			/* 指向元组中的空值位图 */

		/* 在 SpGistLeafTuple 头中设置空值掩码存在位 */
		SGLT_SET_HASNULLMASK(tup, true);
		/* 填充数据区与空值掩码 */
		bp = (bits8 *) ((char *) tup + sizeof(SpGistLeafTupleData));
		heap_fill_tuple(tupleDescriptor, datums, isnulls, tp, data_size,
						&tupmask, bp);
	}
	else if (natts > 1 || !isnulls[spgKeyColumn])
	{
		/* 仅填充数据区 */
		heap_fill_tuple(tupleDescriptor, datums, isnulls, tp, data_size,
						&tupmask, (bits8 *) NULL);
	}
	/* 否则没有数据，也没有位图可填 */

	return tup;
}

/*
 * 构造包含给定标签的节点（将放入内部元组）
 *
 * 注意此处仅将节点的下行链路设为无效。调用方稍后会
 * 填入。
 */
SpGistNodeTuple
spgFormNodeTuple(SpGistState *state, Datum label, bool isnull)
{
	SpGistNodeTuple tup;
	unsigned int size;
	unsigned short infomask = 0;

	/* 计算所需空间（注意结果已 maxaligned） */
	size = SGNTHDRSZ;
	if (!isnull)
		size += SpGistGetInnerTypeSize(&state->attLabelType, label);

	/*
	 * 在此确保大小能放入 t_info 中为其预留的
	 * 字段。
	 */
	if ((size & INDEX_SIZE_MASK) != size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row requires %zu bytes, maximum size is %zu",
						(Size) size, (Size) INDEX_SIZE_MASK)));

	tup = (SpGistNodeTuple) palloc0(size);

	if (isnull)
		infomask |= INDEX_NULL_MASK;
	/* 我们不费心设置 INDEX_VAR_MASK 位 */
	infomask |= size;
	tup->t_info = infomask;

	/* TID 字段稍后填入 */
	ItemPointerSetInvalid(&tup->t_tid);

	if (!isnull)
		memcpyInnerDatum(SGNTDATAPTR(tup), &state->attLabelType, label);

	return tup;
}

/*
 * 构造包含给定前缀与节点数组的内部元组
 */
SpGistInnerTuple
spgFormInnerTuple(SpGistState *state, bool hasPrefix, Datum prefix,
				  int nNodes, SpGistNodeTuple *nodes)
{
	SpGistInnerTuple tup;
	unsigned int size;
	unsigned int prefixSize;
	int			i;
	char	   *ptr;

	/* 计算所需大小 */
	if (hasPrefix)
		prefixSize = SpGistGetInnerTypeSize(&state->attPrefixType, prefix);
	else
		prefixSize = 0;

	size = SGITHDRSZ + prefixSize;

	/* 注意：我们依赖节点元组大小已经 maxaligned */
	for (i = 0; i < nNodes; i++)
		size += IndexTupleSize(nodes[i]);

	/*
	 * 确保稍后可用死元组替换该元组。按当前
	 * 元组布局此测试并非必要，但为安全起见仍做。
	 */
	if (size < SGDTSIZE)
		size = SGDTSIZE;

	/*
	 * 内部元组应小到足以装入一页
	 */
	if (size > SPGIST_PAGE_CAPACITY - sizeof(ItemIdData))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("SP-GiST inner tuple size %zu exceeds maximum %zu",
						(Size) size,
						SPGIST_PAGE_CAPACITY - sizeof(ItemIdData)),
				 errhint("Values larger than a buffer page cannot be indexed.")));

	/*
	 * 检查头字段是否溢出——若上面已成功则大概不会失败，
	 * 但还是偏执一点
	 */
	if (size > SGITMAXSIZE ||
		prefixSize > SGITMAXPREFIXSIZE ||
		nNodes > SGITMAXNNODES)
		elog(ERROR, "SPGiST inner tuple header field is too small");

	/* 好，形成元组 */
	tup = (SpGistInnerTuple) palloc0(size);

	tup->nNodes = nNodes;
	tup->prefixSize = prefixSize;
	tup->size = size;

	if (hasPrefix)
		memcpyInnerDatum(SGITDATAPTR(tup), &state->attPrefixType, prefix);

	ptr = (char *) SGITNODEPTR(tup);

	for (i = 0; i < nNodes; i++)
	{
		SpGistNodeTuple node = nodes[i];

		memcpy(ptr, node, IndexTupleSize(node));
		ptr += IndexTupleSize(node);
	}

	return tup;
}

/*
 * 构造 "死" 元组以替换正被删除的元组。
 *
 * 状态可以是 SPGIST_REDIRECT、SPGIST_DEAD 或 SPGIST_PLACEHOLDER。
 * 对 REDIRECT 元组，必须提供指针（blkno+offset），且
 * xid 字段会自动填入。
 *
 * 此函数在临界区中调用，因此不用 palloc；元组
 * 建在预分配存储中。在以不同参数再次调用前应先
 * 拷贝它。
 */
SpGistDeadTuple
spgFormDeadTuple(SpGistState *state, int tupstate,
				 BlockNumber blkno, OffsetNumber offnum)
{
	SpGistDeadTuple tuple = (SpGistDeadTuple) state->deadTupleStorage;

	tuple->tupstate = tupstate;
	tuple->size = SGDTSIZE;
	SGLT_SET_NEXTOFFSET(tuple, InvalidOffsetNumber);

	if (tupstate == SPGIST_REDIRECT)
	{
		ItemPointerSet(&tuple->pointer, blkno, offnum);
		tuple->xid = state->myXid;
	}
	else
	{
		ItemPointerSetInvalid(&tuple->pointer);
		tuple->xid = InvalidTransactionId;
	}

	return tuple;
}

/*
 * 将 SPGiST 叶子元组转换为 Datum/isnull 数组。
 *
 * 调用方必须为输出数组分配足够存储。
 * （INDEX_MAX_KEYS 个条目应足够。）
 */
void
spgDeformLeafTuple(SpGistLeafTuple tup, TupleDesc tupleDescriptor,
				   Datum *datums, bool *isnulls, bool keyColumnIsNull)
{
	bool		hasNullsMask = SGLT_GET_HASNULLMASK(tup);
	char	   *tp;				/* 指向元组数据 */
	bits8	   *bp;				/* 指向元组中的空值位图 */

	if (keyColumnIsNull && tupleDescriptor->natts == 1)
	{
		/*
		 * 平凡情形：只有键属性且我们在空值
		 * 树中。元组头中的 hasNullsMask 位不应设置
		 * （因而不能使用 index_deform_tuple_internal），但
		 * 结果仍为 NULL。
		 *
		 * 注意：当前这是死代码，因为在只有键属性时无处调用
		 * 本函数。但我们仍应覆盖该情形。
		 */
		Assert(!hasNullsMask);

		datums[spgKeyColumn] = (Datum) 0;
		isnulls[spgKeyColumn] = true;
		return;
	}

	tp = (char *) tup + SGLTHDRSZ(hasNullsMask);
	bp = (bits8 *) ((char *) tup + sizeof(SpGistLeafTupleData));

	index_deform_tuple_internal(tupleDescriptor,
								datums, isnulls,
								tp, bp, hasNullsMask);

	/*
	 * 元组中键列的 isnull 值应与调用方传入的
	 * keyColumnIsNull 标志一致。
	 */
	Assert(keyColumnIsNull == isnulls[spgKeyColumn]);
}

/*
 * 提取 innerTuple 内各节点的标签 datum
 *
 * 若标签 datum 均为 NULLs 则返回 NULL
 */
Datum *
spgExtractNodeLabels(SpGistState *state, SpGistInnerTuple innerTuple)
{
	Datum	   *nodeLabels;
	int			i;
	SpGistNodeTuple node;

	/* 所有标签必须全为 NULL，或全非 NULL。 */
	node = SGITNODEPTR(innerTuple);
	if (IndexTupleHasNulls(node))
	{
		SGITITERATE(innerTuple, i, node)
		{
			if (!IndexTupleHasNulls(node))
				elog(ERROR, "some but not all node labels are null in SPGiST inner tuple");
		}
		/* 它们全为空，因此直接返回 NULL */
		return NULL;
	}
	else
	{
		nodeLabels = (Datum *) palloc(sizeof(Datum) * innerTuple->nNodes);
		SGITITERATE(innerTuple, i, node)
		{
			if (IndexTupleHasNulls(node))
				elog(ERROR, "some but not all node labels are null in SPGiST inner tuple");
			nodeLabels[i] = SGNTDATUM(node, state);
		}
		return nodeLabels;
	}
}

/*
 * 向页添加新项，若可能则替换 PLACEHOLDER 项。
 * 返回插入位置，失败时返回 InvalidOffsetNumber。
 *
 * 若 startOffset 非 NULL，则从 *startOffset 开始搜索占位项，
 * 并更新为下一搜索位置。这只是为重复插入做的
 * 优化。
 *
 * 若 errorOK 为 false，空间不足时抛错，而非返回
 * InvalidOffsetNumber。
 */
OffsetNumber
SpGistPageAddNewItem(SpGistState *state, Page page, Item item, Size size,
					 OffsetNumber *startOffset, bool errorOK)
{
	SpGistPageOpaque opaque = SpGistPageGetOpaque(page);
	OffsetNumber i,
				maxoff,
				offnum;

	if (opaque->nPlaceholder > 0 &&
		PageGetExactFreeSpace(page) + SGDTSIZE >= MAXALIGN(size))
	{
		/* 尝试替换占位项 */
		maxoff = PageGetMaxOffsetNumber(page);
		offnum = InvalidOffsetNumber;

		for (;;)
		{
			if (startOffset && *startOffset != InvalidOffsetNumber)
				i = *startOffset;
			else
				i = FirstOffsetNumber;
			for (; i <= maxoff; i++)
			{
				SpGistDeadTuple it = (SpGistDeadTuple) PageGetItem(page,
																   PageGetItemId(page, i));

				if (it->tupstate == SPGIST_PLACEHOLDER)
				{
					offnum = i;
					break;
				}
			}

			/* 若找到占位项则完成 */
			if (offnum != InvalidOffsetNumber)
				break;

			if (startOffset && *startOffset != InvalidOffsetNumber)
			{
				/* 提示无效，从头重新搜索 */
				*startOffset = InvalidOffsetNumber;
				continue;
			}

			/* 嗯，没找到占位项？ */
			opaque->nPlaceholder = 0;
			break;
		}

		if (offnum != InvalidOffsetNumber)
		{
			/* 替换占位元组 */
			PageIndexTupleDelete(page, offnum);

			offnum = PageAddItem(page, item, size, offnum, false, false);

			/*
			 * 按函数顶部的大小检查本不应失败，但仍测试。若
			 * 确实失败，必须 PANIC，因为我们已经删除了占位
			 * 元组，没有别的办法阻止损坏落到
			 * 磁盘。
			 */
			if (offnum != InvalidOffsetNumber)
			{
				Assert(opaque->nPlaceholder > 0);
				opaque->nPlaceholder--;
				if (startOffset)
					*startOffset = offnum + 1;
			}
			else
				elog(PANIC, "failed to add item of size %zu to SPGiST index page",
					 size);

			return offnum;
		}
	}

	/* 未能替换占位项，因此直接加到页上 */
	offnum = PageAddItem(page, item, size,
						 InvalidOffsetNumber, false, false);

	if (offnum == InvalidOffsetNumber && !errorOK)
		elog(ERROR, "failed to add item of size %zu to SPGiST index page",
			 size);

	return offnum;
}

/*
 *	spgproperty() -- 检查索引的布尔属性。
 *
 * 对大多数 AM 这是可选的，但对 SP-GiST 是必需的，因为核心
 * 属性代码不支持 AMPROP_DISTANCE_ORDERABLE。
 */
bool
spgproperty(Oid index_oid, int attno,
			IndexAMProperty prop, const char *propname,
			bool *res, bool *isnull)
{
	Oid			opclass,
				opfamily,
				opcintype;
	CatCList   *catlist;
	int			i;

	/* 只回答列级查询 */
	if (attno == 0)
		return false;

	switch (prop)
	{
		case AMPROP_DISTANCE_ORDERABLE:
			break;
		default:
			return false;
	}

	/*
	 * 当前，SP-GiST 按距离排序的扫描需要操作类中存在默认
	 * 类型的距离操作符。因此我们假定若存在这样的操作符，
	 * 则自有其用途。
	 */

	/* 首先需要知道该列的操作类。 */
	opclass = get_index_column_opclass(index_oid, attno);
	if (!OidIsValid(opclass))
	{
		*isnull = true;
		return true;
	}

	/* 现在查找操作类族与输入数据类型。 */
	if (!get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype))
	{
		*isnull = true;
		return true;
	}

	/* 现在可以检查是否提供了该操作符。 */
	catlist = SearchSysCacheList1(AMOPSTRATEGY,
								  ObjectIdGetDatum(opfamily));

	*res = false;

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	amoptup = &catlist->members[i]->tuple;
		Form_pg_amop amopform = (Form_pg_amop) GETSTRUCT(amoptup);

		if (amopform->amoppurpose == AMOP_ORDER &&
			(amopform->amoplefttype == opcintype ||
			 amopform->amoprighttype == opcintype) &&
			opfamily_can_sort_type(amopform->amopsortfamily,
								   get_op_rettype(amopform->amopopr)))
		{
			*res = true;
			break;
		}
	}

	ReleaseSysCacheList(catlist);

	*isnull = false;

	return true;
}
