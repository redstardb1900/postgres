/*-------------------------------------------------------------------------
 *
 * expandedrecord.c
 *	  操作复合展开对象的函数。
 *
 * 本模块支持"展开对象"（参见 expandeddatum.h），它
 * 能够存储命名复合类型、命名复合类型之上的域以及
 * 记录类型（已注册或匿名）。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/expandedrecord.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandedrecord.h"
#include "utils/memutils.h"
#include "utils/typcache.h"


/* 展开对象所需的 "Methods" */
static Size ER_get_flat_size(ExpandedObjectHeader *eohptr);
static void ER_flatten_into(ExpandedObjectHeader *eohptr,
							void *result, Size allocated_size);

static const ExpandedObjectMethods ER_methods =
{
	ER_get_flat_size,
	ER_flatten_into
};

/* 其他本地函数 */
static void ER_mc_callback(void *arg);
static MemoryContext get_short_term_cxt(ExpandedRecordHeader *erh);
static void build_dummy_expanded_header(ExpandedRecordHeader *main_erh);
static pg_noinline void check_domain_for_new_field(ExpandedRecordHeader *erh,
												   int fnumber,
												   Datum newValue, bool isnull);
static pg_noinline void check_domain_for_new_tuple(ExpandedRecordHeader *erh,
												   HeapTuple tuple);


/*
 * 构建指定复合类型的展开记录
 *
 * type_id 可为 RECORDOID，但前提是给出了正的 typmod。
 *
 * 展开记录初始为"空"，其状态在逻辑上等价于
 * NULL 复合值（而不是 ROW(NULL, NULL, ...)）。
 * 注意这对域类型而言可能不是有效状态；
 * 若调用者需要检查，请调用
 * expanded_record_set_tuple(erh, NULL, false, false).
 *
 * 展开对象将成为 parentcontext 的子对象。
 */
ExpandedRecordHeader *
make_expanded_record_from_typeid(Oid type_id, int32 typmod,
								 MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	int			flags = 0;
	TupleDesc	tupdesc;
	uint64		tupdesc_id;
	MemoryContext objcxt;
	char	   *chunk;

	if (type_id != RECORDOID)
	{
		/*
		 * 查询 typcache 看它是否为复合之上的域，同时
		 * 都取得元组描述符及其标识符。
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id,
									 TYPECACHE_TUPDESC |
									 TYPECACHE_DOMAIN_BASE_INFO);
		if (typentry->typtype == TYPTYPE_DOMAIN)
		{
			flags |= ER_FLAG_IS_DOMAIN;
			typentry = lookup_type_cache(typentry->domainBaseType,
										 TYPECACHE_TUPDESC);
		}
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		tupdesc = typentry->tupDesc;
		tupdesc_id = typentry->tupDesc_identifier;
	}
	else
	{
		/*
		 * RECORD 类型则从 typcache 取得描述符与标识符。
		 */
		tupdesc = lookup_rowtype_tupdesc(type_id, typmod);
		tupdesc_id = assign_record_type_identifier(type_id, typmod);
	}

	/*
	 * 为展开对象分配私有上下文。我们使用常规大小
	 * 的上下文而非小上下文，以提高能不借助额外 malloc
	 * 块就能容纳元组描述符的几率。（此代码路径其实
	 * 从不需要把元组描述符拷入展开记录，但为了与
	 * 其他构建展开记录的方式保持一致。）
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/*
	 * 既然已知元组描述符中的字段数，就可以
	 * 把 dvalues/dnulls 数组与记录头部一起分配。这
	 * 样即使从不需要这些数组也没有用处，但它几乎
	 * 没有成本，若需要还能省一次 palloc。
	 */
	erh = (ExpandedRecordHeader *)
		MemoryContextAlloc(objcxt, MAXALIGN(sizeof(ExpandedRecordHeader))
						   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

	/* 确保所有头部字段都初始化为 0/null */
	memset(erh, 0, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/* 设置 dvalues/dnulls，目前尚无有效内容 */
	chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
	erh->dvalues = (Datum *) chunk;
	erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
	erh->nfields = tupdesc->natts;

	/* 填入复合类型标识信息 */
	erh->er_decltypeid = type_id;
	erh->er_typeid = tupdesc->tdtypeid;
	erh->er_typmod = tupdesc->tdtypmod;
	erh->er_tupdesc_id = tupdesc_id;

	erh->flags = flags;

	/*
	 * 若我们从 typcache 得到的是引用计数的元组描述符，
	 * 就需要对它获取自己的引用计数。我们用内存上下文
	 * 回调来管理引用计数，而不是假定 CurrentResourceOwner
	 * 比这个展开对象活得更久。
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* 注册回调以释放引用计数 */
		erh->er_mcb.func = ER_mc_callback;
		erh->er_mcb.arg = (void *) erh;
		MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
										   &erh->er_mcb);

		/* 并保存该指针 */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;

		/* 若调用过 lookup_rowtype_tupdesc，则释放其 pin */
		if (type_id == RECORDOID)
			ReleaseTupleDesc(tupdesc);
	}
	else
	{
		/*
		 * 若它不是引用计数的，就假定它会比展开对象
		 * 活得更久。（例如共享记录类型就可能发生。）
		 */
		erh->er_tupdesc = tupdesc;
	}

	/*
	 * 我们不设置 ER_FLAG_DVALUES_VALID 或 ER_FLAG_FVALUE_VALID，
	 * 故记录逻辑上仍为空。
	 */

	return erh;
}

/*
 * 按元组描述符定义的行类型构建展开记录
 *
 * 必要时会拷贝元组描述符（即当无法只递增其
 * 引用计数时）。
 *
 * 展开记录初始为"空"，其状态在逻辑上等价于
 * NULL 复合值（而不是 ROW(NULL, NULL, ...)）。
 *
 * 展开对象将成为 parentcontext 的子对象。
 */
ExpandedRecordHeader *
make_expanded_record_from_tupdesc(TupleDesc tupdesc,
								  MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	uint64		tupdesc_id;
	MemoryContext objcxt;
	MemoryContext oldcxt;
	char	   *chunk;

	if (tupdesc->tdtypeid != RECORDOID)
	{
		/*
		 * 若为命名复合类型（非 RECORD），我们更倾向引用
		 * typcache 中的元组描述符副本，它必有引用计数
		 * （给定的元组描述符则未必）。无论如何，我们都
		 * 需要经 typcache 取得正确的元组描述符标识符。
		 *
		 * 注意 tdtypeid 不可能是域类型，这里无需
		 * 考虑这种情况。
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(tupdesc->tdtypeid, TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(tupdesc->tdtypeid))));
		tupdesc = typentry->tupDesc;
		tupdesc_id = typentry->tupDesc_identifier;
	}
	else
	{
		/*
		 * 对 RECORD 类型，取得适当的唯一标识符（可能
		 * 新分配的）。
		 */
		tupdesc_id = assign_record_type_identifier(tupdesc->tdtypeid,
												   tupdesc->tdtypmod);
	}

	/*
	 * 为展开对象分配私有上下文。我们使用常规大小
	 * 的上下文而非小上下文，以提高能不借助额外 malloc
	 * 块就能容纳元组描述符的几率。
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/*
	 * 既然已知元组描述符中的字段数，就可以
	 * 把 dvalues/dnulls 数组与记录头部一起分配。这
	 * 样即使从不需要这些数组也没有用处，但它几乎
	 * 没有成本，若需要还能省一次 palloc。
	 */
	erh = (ExpandedRecordHeader *)
		MemoryContextAlloc(objcxt, MAXALIGN(sizeof(ExpandedRecordHeader))
						   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

	/* 确保所有头部字段都初始化为 0/null */
	memset(erh, 0, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/* 设置 dvalues/dnulls，目前尚无有效内容 */
	chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
	erh->dvalues = (Datum *) chunk;
	erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
	erh->nfields = tupdesc->natts;

	/* 填入复合类型标识信息 */
	erh->er_decltypeid = erh->er_typeid = tupdesc->tdtypeid;
	erh->er_typmod = tupdesc->tdtypmod;
	erh->er_tupdesc_id = tupdesc_id;

	/*
	 * 必要时拷贝元组描述符，但若可能则优先递增其
	 * 引用计数。我们以内存上下文回调管理它，
	 * 而不是假定 CurrentResourceOwner
	 * 更长寿。
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* 注册回调以释放引用计数 */
		erh->er_mcb.func = ER_mc_callback;
		erh->er_mcb.arg = (void *) erh;
		MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
										   &erh->er_mcb);

		/* 并保存该指针 */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;
	}
	else
	{
		/* 拷贝即可 */
		oldcxt = MemoryContextSwitchTo(objcxt);
		erh->er_tupdesc = CreateTupleDescCopy(tupdesc);
		erh->flags |= ER_FLAG_TUPDESC_ALLOCED;
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 我们不设置 ER_FLAG_DVALUES_VALID 或 ER_FLAG_FVALUE_VALID，
	 * 故记录逻辑上仍为空。
	 */

	return erh;
}

/*
 * 构建与给定展开记录相同行类型的展开记录
 *
 * 这比上面两个例程都快，因为可以绕过
 * typcache 查找。
 *
 * 展开记录初始为"空"——我们不会拷贝源记录
 * 中可能存在的元组。
 *
 * 展开对象将成为 parentcontext 的子对象。
 */
ExpandedRecordHeader *
make_expanded_record_from_exprecord(ExpandedRecordHeader *olderh,
									MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	TupleDesc	tupdesc = expanded_record_get_tupdesc(olderh);
	MemoryContext objcxt;
	MemoryContext oldcxt;
	char	   *chunk;

	/*
	 * 为展开对象分配私有上下文。我们使用常规大小
	 * 的上下文而非小上下文，以提高能不借助额外 malloc
	 * 块就能容纳元组描述符的几率。
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/*
	 * 既然已知元组描述符中的字段数，就可以
	 * 把 dvalues/dnulls 数组与记录头部一起分配。这
	 * 样即使从不需要这些数组也没有用处，但它几乎
	 * 没有成本，若需要还能省一次 palloc。
	 */
	erh = (ExpandedRecordHeader *)
		MemoryContextAlloc(objcxt, MAXALIGN(sizeof(ExpandedRecordHeader))
						   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

	/* 确保所有头部字段都初始化为 0/null */
	memset(erh, 0, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/* 设置 dvalues/dnulls，目前尚无有效内容 */
	chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
	erh->dvalues = (Datum *) chunk;
	erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
	erh->nfields = tupdesc->natts;

	/* 填入复合类型标识信息 */
	erh->er_decltypeid = olderh->er_decltypeid;
	erh->er_typeid = olderh->er_typeid;
	erh->er_typmod = olderh->er_typmod;
	erh->er_tupdesc_id = olderh->er_tupdesc_id;

	/* 唯一被带过来的标志位是 IS_DOMAIN */
	erh->flags = olderh->flags & ER_FLAG_IS_DOMAIN;

	/*
	 * 必要时拷贝元组描述符，但若可能则优先递增其
	 * 引用计数。我们以内存上下文回调管理它，
	 * 而不是假定 CurrentResourceOwner
	 * 更长寿。
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* 注册回调以释放引用计数 */
		erh->er_mcb.func = ER_mc_callback;
		erh->er_mcb.arg = (void *) erh;
		MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
										   &erh->er_mcb);

		/* 并保存该指针 */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;
	}
	else if (olderh->flags & ER_FLAG_TUPDESC_ALLOCED)
	{
		/* 我们需自行拷贝元组描述符 */
		oldcxt = MemoryContextSwitchTo(objcxt);
		erh->er_tupdesc = CreateTupleDescCopy(tupdesc);
		erh->flags |= ER_FLAG_TUPDESC_ALLOCED;
		MemoryContextSwitchTo(oldcxt);
	}
	else
	{
		/*
		 * 假定元组描述符会比此展开对象活得久，正如
		 * 我们正假定它比源对象活得久一样。
		 */
		erh->er_tupdesc = tupdesc;
	}

	/*
	 * 我们不设置 ER_FLAG_DVALUES_VALID 或 ER_FLAG_FVALUE_VALID，
	 * 故记录逻辑上仍为空。
	 */

	return erh;
}

/*
 * 把给定元组插入为展开记录的值
 *
 * 调用者有责任确保元组匹配该记录此前指派的
 * 行类型。（不过若有域约束，会在这里
 * 进行检查。）
 *
 * 若 "copy" 为真，元组会被物理拷贝进展开记录的
 * 本地存储中；否则调用者有责任确保元组的存活期
 * 与展开记录一样长。
 *
 * 元组中的行外字段值会自动内联，前提是
 * "expand_external" 为真；否则不会。（copy = false、
 * expand_external = true 不合理，也不受支持。）
 *
 * 或者，元组可以为 NULL，此时我们只是把展开记录
 * 置为空。
 */
void
expanded_record_set_tuple(ExpandedRecordHeader *erh,
						  HeapTuple tuple,
						  bool copy,
						  bool expand_external)
{
	int			oldflags;
	HeapTuple	oldtuple;
	char	   *oldfstartptr;
	char	   *oldfendptr;
	int			newflags;
	HeapTuple	newtuple;
	MemoryContext oldcxt;

	/* 绝不应试图向虚设头部赋予新数据 */
	Assert(!(erh->flags & ER_FLAG_IS_DUMMY));

	/*
	 * 在执行赋值之前，先看结果是否会满足域约束。
	 */
	if (erh->flags & ER_FLAG_IS_DOMAIN)
		check_domain_for_new_tuple(erh, tuple);

	/*
	 * 如需清除行外字段值，就使用短期上下文来做，
	 * 以免 toast 取数可能产生的垃圾
	 * 泄漏。
	 */
	if (expand_external && tuple)
	{
		/* 断言调用者未要求不支持的情形 */
		Assert(copy);
		if (HeapTupleHasExternal(tuple))
		{
			oldcxt = MemoryContextSwitchTo(get_short_term_cxt(erh));
			tuple = toast_flatten_tuple(tuple, erh->er_tupdesc);
			MemoryContextSwitchTo(oldcxt);
		}
		else
			expand_external = false;	/* 下方无需清理 */
	}

	/*
	 * 初始化新标志，仅保留非数据的状态位。
	 */
	oldflags = erh->flags;
	newflags = oldflags & ER_FLAGS_NON_DATA;

	/*
	 * 若需要就把元组拷入本地存储。我们必须确保
	 * 这一步成功，才能开始修改记录状态。
	 */
	if (copy && tuple)
	{
		oldcxt = MemoryContextSwitchTo(erh->hdr.eoh_context);
		newtuple = heap_copytuple(tuple);
		newflags |= ER_FLAG_FVALUE_ALLOCED;
		MemoryContextSwitchTo(oldcxt);

		/* 现在可以清刷解 TOAST 可能泄漏的任何内容。 */
		if (expand_external)
			MemoryContextReset(erh->er_short_term_cxt);
	}
	else
		newtuple = tuple;

	/* 拷贝即将被覆盖的字段 */
	oldtuple = erh->fvalue;
	oldfstartptr = erh->fstartptr;
	oldfendptr = erh->fendptr;

	/*
	 * 现在可以安全更新展开记录的状态了。
	 */
	if (newtuple)
	{
		/* 保存平面表示 */
		erh->fvalue = newtuple;
		erh->fstartptr = (char *) newtuple->t_data;
		erh->fendptr = ((char *) newtuple->t_data) + newtuple->t_len;
		newflags |= ER_FLAG_FVALUE_VALID;

		/* 记住我们是否有行外字段值 */
		if (HeapTupleHasExternal(newtuple))
			newflags |= ER_FLAG_HAVE_EXTERNAL;
	}
	else
	{
		erh->fvalue = NULL;
		erh->fstartptr = erh->fendptr = NULL;
	}

	erh->flags = newflags;

	/* 重置平面大小信息；现在不费心使其有效 */
	erh->flat_size = 0;

	/*
 * 现在，释放属于旧字段值的任何存储。这样做
 * 是安全的，因 erh->flags 不再设置 ER_FLAG_DVALUES_VALID；
	 * 即使中途失败，记录也是有效的，最坏情况也只
	 * 不过是没回收一些空间。
	 */
	if (oldflags & ER_FLAG_DVALUES_ALLOCED)
	{
		TupleDesc	tupdesc = erh->er_tupdesc;
		int			i;

		for (i = 0; i < erh->nfields; i++)
		{
			if (!erh->dnulls[i] &&
				!(TupleDescAttr(tupdesc, i)->attbyval))
			{
				char	   *oldValue = (char *) DatumGetPointer(erh->dvalues[i]);

				if (oldValue < oldfstartptr || oldValue >= oldfendptr)
					pfree(oldValue);
			}
		}
	}

	/* 同样，若旧元组是本地分配的就将其释放 */
	if (oldflags & ER_FLAG_FVALUE_ALLOCED)
		heap_freetuple(oldtuple);

	/* 直到/除非需要，否则不会生成新的解构表示 */
}

/*
 * make_expanded_record_from_datum：从复合 Datum 构建展开记录
 *
 * 它把 make_expanded_record_from_typeid 与
 * expanded_record_set_tuple 的功能合在一起。但我们不强制
 * 立即查找元组描述符，它可能永远用不到。
 *
 * 展开对象将成为 parentcontext 的子对象。
 *
 * 注意：复合 datum 无法自我标识为域类型，
 * 因此这里无需考虑域的情形。
 */
Datum
make_expanded_record_from_datum(Datum recorddatum, MemoryContext parentcontext)
{
	ExpandedRecordHeader *erh;
	HeapTupleHeader tuphdr;
	HeapTupleData tmptup;
	HeapTuple	newtuple;
	MemoryContext objcxt;
	MemoryContext oldcxt;

	/*
	 * 为展开对象分配私有上下文。我们使用常规大小
	 * 的上下文而非小上下文，以提高能不借助额外 malloc
	 * 块就能容纳元组描述符的几率。
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded record",
								   ALLOCSET_DEFAULT_SIZES);

	/* 设置展开记录头部，把字段初始化为 0/null */
	erh = (ExpandedRecordHeader *)
		MemoryContextAllocZero(objcxt, sizeof(ExpandedRecordHeader));

	EOH_init_header(&erh->hdr, &ER_methods, objcxt);
	erh->er_magic = ER_MAGIC;

	/*
	 * 把源记录解 TOAST 并作为 HeapTuple 拷入私有上下文。
	 * （若确实需要解 TOAST 源，会在调用者的上下文
	 * 中泄漏一些内存，但这似乎不值得担心。）
	 */
	tuphdr = DatumGetHeapTupleHeader(recorddatum);

	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuphdr;

	oldcxt = MemoryContextSwitchTo(objcxt);
	newtuple = heap_copytuple(&tmptup);
	erh->flags |= ER_FLAG_FVALUE_ALLOCED;
	MemoryContextSwitchTo(oldcxt);

	/* 填入复合类型标识信息 */
	erh->er_decltypeid = erh->er_typeid = HeapTupleHeaderGetTypeId(tuphdr);
	erh->er_typmod = HeapTupleHeaderGetTypMod(tuphdr);

	/* 记住我们已有平面表示 */
	erh->fvalue = newtuple;
	erh->fstartptr = (char *) newtuple->t_data;
	erh->fendptr = ((char *) newtuple->t_data) + newtuple->t_len;
	erh->flags |= ER_FLAG_FVALUE_VALID;

	/* 不应需要设置 ER_FLAG_HAVE_EXTERNAL */
	Assert(!HeapTupleHeaderHasExternal(tuphdr));

	/*
	 * 我们会把元组描述符的查找推迟到非查不可之时，
	 * 也不生成解构表示。我们没有足够信息填 flat_size
	 * 相关的值。
	 */

	/* 返回指向展开记录的读/写指针 */
	return EOHPGetRWDatum(&erh->hdr);
}

/*
 * 展开记录的 get_flat_size 方法
 *
 * 注意：要在存活期较短的内存上下文中调用它，以防
 * 解 TOAST 之类的活动造成内存泄漏。
 */
static Size
ER_get_flat_size(ExpandedObjectHeader *eohptr)
{
	ExpandedRecordHeader *erh = (ExpandedRecordHeader *) eohptr;
	TupleDesc	tupdesc;
	Size		len;
	Size		data_len;
	int			hoff;
	bool		hasnull;
	int			i;

	Assert(erh->er_magic == ER_MAGIC);

	/*
	 * 平面表示必须是有效的复合 datum。务必确保我们
	 * 拥有的是已注册而非匿名的 RECORD 类型。
	 */
	if (erh->er_typeid == RECORDOID &&
		erh->er_typmod < 0)
	{
		tupdesc = expanded_record_get_tupdesc(erh);
		assign_record_type_typmod(tupdesc);
		erh->er_typmod = tupdesc->tdtypmod;
	}

	/*
	 * 如果我们的展平值有效且没有行外字段，就
	 * 直接使用它。
	 */
	if (erh->flags & ER_FLAG_FVALUE_VALID &&
		!(erh->flags & ER_FLAG_HAVE_EXTERNAL))
		return erh->fvalue->t_len;

	/* 若有缓存的大小值就采用它 */
	if (erh->flat_size)
		return erh->flat_size;

	/* 若尚未解构元组，就先解构 */
	if (!(erh->flags & ER_FLAG_DVALUES_VALID))
		deconstruct_expanded_record(erh);

	/* 此时元组描述符必须有效 */
	tupdesc = erh->er_tupdesc;

	/*
	 * 复合 datum 绝不能包含任何行外值。
	 */
	if (erh->flags & ER_FLAG_HAVE_EXTERNAL)
	{
		for (i = 0; i < erh->nfields; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (!erh->dnulls[i] &&
				!attr->attbyval && attr->attlen == -1 &&
				VARATT_IS_EXTERNAL(DatumGetPointer(erh->dvalues[i])))
			{
				/*
				 * expanded_record_set_field_internal 可以完成解
				 * TOAST 的实际工作。无需重查域约束。
				 */
				expanded_record_set_field_internal(erh, i + 1,
												   erh->dvalues[i], false,
												   true,
												   false);
			}
		}

		/*
		 * 现在所有外部字段值都已移除，因此可以清除相应
		 * 的标志。这不会使 ER_flatten_into() 错走快速路径，
		 * 因为 expanded_record_set_field() 已清除
		 * ER_FLAG_FVALUE_VALID。
		 */
		erh->flags &= ~ER_FLAG_HAVE_EXTERNAL;
	}

	/* 测试当前是否有任何空值 */
	hasnull = false;
	for (i = 0; i < erh->nfields; i++)
	{
		if (erh->dnulls[i])
		{
			hasnull = true;
			break;
		}
	}

	/* 确定所需的总空间 */
	len = offsetof(HeapTupleHeaderData, t_bits);

	if (hasnull)
		len += BITMAPLEN(tupdesc->natts);

	hoff = len = MAXALIGN(len); /* 安全对齐数据 */

	data_len = heap_compute_data_size(tupdesc, erh->dvalues, erh->dnulls);

	len += data_len;

	/* 留作下次使用 */
	erh->flat_size = len;
	erh->data_len = data_len;
	erh->hoff = hoff;
	erh->hasnull = hasnull;

	return len;
}

/*
 * 展开记录的 flatten_into 方法
 */
static void
ER_flatten_into(ExpandedObjectHeader *eohptr,
				void *result, Size allocated_size)
{
	ExpandedRecordHeader *erh = (ExpandedRecordHeader *) eohptr;
	HeapTupleHeader tuphdr = (HeapTupleHeader) result;
	TupleDesc	tupdesc;

	Assert(erh->er_magic == ER_MAGIC);

	/* 若已有无行外字段的有效展平值则很容易 */
	if (erh->flags & ER_FLAG_FVALUE_VALID &&
		!(erh->flags & ER_FLAG_HAVE_EXTERNAL))
	{
		Assert(allocated_size == erh->fvalue->t_len);
		memcpy(tuphdr, erh->fvalue->t_data, allocated_size);
		/* 原展平值可能没有 datum 头部字段 */
		HeapTupleHeaderSetDatumLength(tuphdr, allocated_size);
		HeapTupleHeaderSetTypeId(tuphdr, erh->er_typeid);
		HeapTupleHeaderSetTypMod(tuphdr, erh->er_typmod);
		return;
	}

	/* 否则分配应与之前 get_flat_size 结果一致 */
	Assert(allocated_size == erh->flat_size);

	/* 我们会需要元组描述符 */
	tupdesc = expanded_record_get_tupdesc(erh);

	/* 必须确保所有填充空间均为零 */
	memset(tuphdr, 0, allocated_size);

	/* 设置复合 Datum 的头部字段 */
	HeapTupleHeaderSetDatumLength(tuphdr, allocated_size);
	HeapTupleHeaderSetTypeId(tuphdr, erh->er_typeid);
	HeapTupleHeaderSetTypMod(tuphdr, erh->er_typmod);
	/* 我们还确保 t_ctid 除非被显式设置否则无效 */
	ItemPointerSetInvalid(&(tuphdr->t_ctid));

	HeapTupleHeaderSetNatts(tuphdr, tupdesc->natts);
	tuphdr->t_hoff = erh->hoff;

	/* 并从 dvalues/dnulls 填充数据区 */
	heap_fill_tuple(tupdesc,
					erh->dvalues,
					erh->dnulls,
					(char *) tuphdr + erh->hoff,
					erh->data_len,
					&tuphdr->t_infomask,
					(erh->hasnull ? tuphdr->t_bits : NULL));
}

/*
 * 查找展开记录实际类型的元组描述符
 *
 * 注意：本模块内部的代码可以直接取用
 * erh->er_tupdesc（当 ER_FLAG_DVALUES_VALID 已设置时）；
 * 否则应调用 expanded_record_get_tupdesc。本函数
 * 是其行外部分。
 */
TupleDesc
expanded_record_fetch_tupdesc(ExpandedRecordHeader *erh)
{
	TupleDesc	tupdesc;

	/* 若已有则很容易（但调用者本应已检查过） */
	if (erh->er_tupdesc)
		return erh->er_tupdesc;

	/* 用 typcache 查找该复合类型的元组描述符 */
	tupdesc = lookup_rowtype_tupdesc(erh->er_typeid, erh->er_typmod);

	/*
	 * 若其为引用计数的元组描述符而非静态分配，
	 * 我们以内存上下文回调管理引用计数，
	 * 而不是假定 CurrentResourceOwner
	 * 更长寿。
	 */
	if (tupdesc->tdrefcount >= 0)
	{
		/* 若尚未注册则注册回调 */
		if (erh->er_mcb.arg == NULL)
		{
			erh->er_mcb.func = ER_mc_callback;
			erh->er_mcb.arg = (void *) erh;
			MemoryContextRegisterResetCallback(erh->hdr.eoh_context,
											   &erh->er_mcb);
		}

		/* 记住自己的指针 */
		erh->er_tupdesc = tupdesc;
		tupdesc->tdrefcount++;

		/* 释放 lookup_rowtype_tupdesc 获取的 pin */
		ReleaseTupleDesc(tupdesc);
	}
	else
	{
		/* 只要记住指针即可 */
		erh->er_tupdesc = tupdesc;
	}

	/* 无论如何，都取这个元组描述符的进程全局 ID */
	erh->er_tupdesc_id = assign_record_type_identifier(tupdesc->tdtypeid,
													   tupdesc->tdtypmod);

	return tupdesc;
}

/*
 * 取得表示展开记录当前值的 HeapTuple
 *
 * 若有效，则返回原先存储的元组，因此调用者绝不能
 * 乱改它。否则，我们返回在当前内存上下文中创建的
 * HeapTuple。无论哪种情况，都没有尝试把行外的
 * TOAST 值内联，所以该元组不能用作复合
 * datum
 *
 * 若展开记录为空则返回 NULL。
 */
HeapTuple
expanded_record_get_tuple(ExpandedRecordHeader *erh)
{
	/* 若仍保有原元组则很容易 */
	if (erh->flags & ER_FLAG_FVALUE_VALID)
		return erh->fvalue;

	/* 否则直接从 datum 构建元组 */
	if (erh->flags & ER_FLAG_DVALUES_VALID)
		return heap_form_tuple(erh->er_tupdesc, erh->dvalues, erh->dnulls);

	/* 展开记录为空 */
	return NULL;
}

/*
 * 用于清理外部资源的内存上下文重置回调
 */
static void
ER_mc_callback(void *arg)
{
	ExpandedRecordHeader *erh = (ExpandedRecordHeader *) arg;
	TupleDesc	tupdesc = erh->er_tupdesc;

	/* 释放自己的元组描述符引用计数（若有） */
	if (tupdesc)
	{
		erh->er_tupdesc = NULL; /* 只为保险 */
		if (tupdesc->tdrefcount > 0)
		{
			if (--tupdesc->tdrefcount == 0)
				FreeTupleDesc(tupdesc);
		}
	}
}

/*
 * DatumGetExpandedRecord：从输入参数取得可写的展开记录
 *
 * 小心：输入若是读/写指针，会返回输入参数；
 * 因此调用者必须确保自己的修改是"安全的"，也就是
 * 无法把记录留在损坏状态。
 */
ExpandedRecordHeader *
DatumGetExpandedRecord(Datum d)
{
	/* 若已是可写的展开记录就直接返回它 */
	if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(d);

		Assert(erh->er_magic == ER_MAGIC);
		return erh;
	}

	/* 否则走复杂路径 */
	d = make_expanded_record_from_datum(d, CurrentMemoryContext);
	return (ExpandedRecordHeader *) DatumGetEOHP(d);
}

/*
 * 创建展开记录对象的 Datum/isnull 表示
 * （如果尚未创建的话）。调用它之后，就可以直接
 * 读取 dvalues/dnulls 数组，而不必经过 get_field。
 *
 * 注意如果对象当前为空（"null"），它会改为
 * 表示一行空值。
 */
void
deconstruct_expanded_record(ExpandedRecordHeader *erh)
{
	TupleDesc	tupdesc;
	Datum	   *dvalues;
	bool	   *dnulls;
	int			nfields;

	if (erh->flags & ER_FLAG_DVALUES_VALID)
		return;					/* 已有效，无事可做 */

	/* 我们会需要元组描述符 */
	tupdesc = expanded_record_get_tupdesc(erh);

	/*
	 * 如果还没有这些数组，就在私有上下文中分配。我们
	 * 并不预期这里 nfields 会发生变化，所以即便它能
	 * 应对，也不费心避免旧数组的泄漏（它们可能并
	 * 不是单独 palloc 的）。
	 */
	nfields = tupdesc->natts;
	if (erh->dvalues == NULL || erh->nfields != nfields)
	{
		char	   *chunk;

		/*
		 * 为了节省一次 palloc，我们把 Datum 与 isnull
		 * 数组共用一次 palloc
		 */
		chunk = MemoryContextAlloc(erh->hdr.eoh_context,
								   nfields * (sizeof(Datum) + sizeof(bool)));
		dvalues = (Datum *) chunk;
		dnulls = (bool *) (chunk + nfields * sizeof(Datum));
		erh->dvalues = dvalues;
		erh->dnulls = dnulls;
		erh->nfields = nfields;
	}
	else
	{
		dvalues = erh->dvalues;
		dnulls = erh->dnulls;
	}

	if (erh->flags & ER_FLAG_FVALUE_VALID)
	{
		/* 解构元组 */
		heap_deform_tuple(erh->fvalue, tupdesc, dvalues, dnulls);
	}
	else
	{
		/* 若记录为空，则将其创建为一行空值 */
		memset(dvalues, 0, nfields * sizeof(Datum));
		memset(dnulls, true, nfields * sizeof(bool));
	}

	/* 将 dvalues 标为有效 */
	erh->flags |= ER_FLAG_DVALUES_VALID;
}

/*
 * 按名称查找记录字段
 *
 * 如果存在名为 "fieldname" 的字段，就填入 finfo 的
 * 内容并返回 "true"。否则不改 *finfo 返回 "false"。
 */
bool
expanded_record_lookup_field(ExpandedRecordHeader *erh, const char *fieldname,
							 ExpandedRecordFieldInfo *finfo)
{
	TupleDesc	tupdesc;
	int			fno;
	Form_pg_attribute attr;
	const FormData_pg_attribute *sysattr;

	tupdesc = expanded_record_get_tupdesc(erh);

	/* 首先，检查用户定义的属性 */
	for (fno = 0; fno < tupdesc->natts; fno++)
	{
		attr = TupleDescAttr(tupdesc, fno);
		if (namestrcmp(&attr->attname, fieldname) == 0 &&
			!attr->attisdropped)
		{
			finfo->fnumber = attr->attnum;
			finfo->ftypeid = attr->atttypid;
			finfo->ftypmod = attr->atttypmod;
			finfo->fcollation = attr->attcollation;
			return true;
		}
	}

	/* 那系统属性呢？ */
	sysattr = SystemAttributeByName(fieldname);
	if (sysattr != NULL)
	{
		finfo->fnumber = sysattr->attnum;
		finfo->ftypeid = sysattr->atttypid;
		finfo->ftypmod = sysattr->atttypmod;
		finfo->fcollation = sysattr->attcollation;
		return true;
	}

	return false;
}

/*
 * 取记录字段的值
 *
 * expanded_record_get_field 是它的前端；负责处理
 * 可内联的情形。
 */
Datum
expanded_record_fetch_field(ExpandedRecordHeader *erh, int fnumber,
							bool *isnull)
{
	if (fnumber > 0)
	{
		/* 空记录的字段为空值 */
		if (ExpandedRecordIsEmpty(erh))
		{
			*isnull = true;
			return (Datum) 0;
		}
		/* 确保我们已有解构形式 */
		deconstruct_expanded_record(erh);
		/* 超出范围的字段号读作空值 */
		if (unlikely(fnumber > erh->nfields))
		{
			*isnull = true;
			return (Datum) 0;
		}
		*isnull = erh->dnulls[fnumber - 1];
		return erh->dvalues[fnumber - 1];
	}
	else
	{
		/* 若还没有平面元组，系统列读作空值 */
		if (erh->fvalue == NULL)
		{
			*isnull = true;
			return (Datum) 0;
		}
		/* heap_getsysattr 并不使用描述符，直接传 null */
		return heap_getsysattr(erh->fvalue, fnumber, NULL, isnull);
	}
}

/*
 * 设置记录字段的值
 *
 * 如果展开记录属于域类型，那么当域约束会被违反
 * 时，赋值将被拒绝（且不改变记录的
 * 状态）。
 *
 * 如果 expand_external 为真且 newValue 是行外值，
 * 我们将强制对其解 TOAST，使记录不依赖外部存储。
 *
 * 内部调用者可以传入 check_constraints = false 来跳过
 * 域约束的应用。外部调用者绝不该这样做。
 */
void
expanded_record_set_field_internal(ExpandedRecordHeader *erh, int fnumber,
								   Datum newValue, bool isnull,
								   bool expand_external,
								   bool check_constraints)
{
	TupleDesc	tupdesc;
	Form_pg_attribute attr;
	Datum	   *dvalues;
	bool	   *dnulls;
	char	   *oldValue;

	/*
	 * 绝不应试图向虚设头部赋予新数据，除非是为
	 * 字段内联的内部调用。
	 */
	Assert(!(erh->flags & ER_FLAG_IS_DUMMY) || !check_constraints);

	/* 执行赋值之前，先看结果是否满足域约束 */
	if ((erh->flags & ER_FLAG_IS_DOMAIN) && check_constraints)
		check_domain_for_new_field(erh, fnumber, newValue, isnull);

	/* 若尚未解构元组，就先解构 */
	if (!(erh->flags & ER_FLAG_DVALUES_VALID))
		deconstruct_expanded_record(erh);

	/* 此时元组描述符必须有效 */
	tupdesc = erh->er_tupdesc;
	Assert(erh->nfields == tupdesc->natts);

	/* fnumber 为系统列或不存在列时属调用者错误 */
	if (unlikely(fnumber <= 0 || fnumber > erh->nfields))
		elog(ERROR, "cannot assign to field %d of expanded record", fnumber);

	/*
	 * 把新的字段值拷入记录上下文，并按需解
	 * TOAST。
	 */
	attr = TupleDescAttr(tupdesc, fnumber - 1);
	if (!isnull && !attr->attbyval)
	{
		MemoryContext oldcxt;

		/* 如有要求就解外部值的 TOAST */
		if (expand_external)
		{
			if (attr->attlen == -1 &&
				VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
			{
				/* 解 TOAST 应在短期上下文中进行。 */
				oldcxt = MemoryContextSwitchTo(get_short_term_cxt(erh));
				newValue = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(newValue)));
				MemoryContextSwitchTo(oldcxt);
			}
			else
				expand_external = false;	/* 下方无需清理 */
		}

		/* 把值拷入记录的上下文 */
		oldcxt = MemoryContextSwitchTo(erh->hdr.eoh_context);
		newValue = datumCopy(newValue, false, attr->attlen);
		MemoryContextSwitchTo(oldcxt);

		/* 现在可以清刷解 TOAST 可能泄漏的内容 */
		if (expand_external)
			MemoryContextReset(erh->er_short_term_cxt);

		/* 记住我们有字段可能需要 pfree */
		erh->flags |= ER_FLAG_DVALUES_ALLOCED;

		/*
		 * 趁便记下它是否为外部 TOAST 值，因为这意味着
		 * 之后可能需要内联它。（不要想着把它并进前面
		 * 的 expand_external 逻辑：datumCopy 本身就可能
		 * 已把值变成非外部的。）
		 */
		if (attr->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
			erh->flags |= ER_FLAG_HAVE_EXTERNAL;
	}

	/*
	 * 我们已准备好做不可逆修改。
	 */
	dvalues = erh->dvalues;
	dnulls = erh->dnulls;

	/* 展平值将不再准确表示该记录 */
	erh->flags &= ~ER_FLAG_FVALUE_VALID;
	/* 而且我们也不知道展平大小 */
	erh->flat_size = 0;

	/* 按需保存旧字段值以便 pfree。 */
	if (!attr->attbyval && !dnulls[fnumber - 1])
		oldValue = (char *) DatumGetPointer(dvalues[fnumber - 1]);
	else
		oldValue = NULL;

	/* 最后终于可以插入新字段了。 */
	dvalues[fnumber - 1] = newValue;
	dnulls[fnumber - 1] = isnull;

	/*
	 * 按需释放旧字段；这样可避免反复替换字段使
	 * 记录的存储膨胀。若 pfree 万一失败，也不会
	 * 损坏记录。
	 *
	 * 如果更新的是虚设头部，就不能冒险 pfree 旧值，
	 * 因为展开记录的主头部很可能仍然持有指向它的
	 * 指针。这不会造成任何持续的内存泄漏，因为我们
	 * 刚分配的内容都在短存活期的域检查上下文
	 * 中。
	 */
	if (oldValue && !(erh->flags & ER_FLAG_IS_DUMMY))
	{
		/* 不要试图 pfree 原平面记录的一部分 */
		if (oldValue < erh->fstartptr || oldValue >= erh->fendptr)
			pfree(oldValue);
	}
}

/*
 * 设置各记录字段
 *
 * 调用者必须确保所提供的 datum 类型正确，
 * 能匹配该记录此前指派的行类型。
 *
 * 如果 expand_external 为真，我们将强制对行外字段值
 * 解 TOAST，使记录不依赖外部存储。
 *
 * 与反复调用 expanded_record_set_field() 不同，
 * 它并不保证发生错误时展开记录仍处于未损坏
 * 状态。它通常只用于初始化新展开记录。此外，
 * 由于我们预计它至多在展开记录的生命期中
 * 应用一次，所以我们不担心解 TOAST 可能
 * 泄漏的任何垃圾。
 */
void
expanded_record_set_fields(ExpandedRecordHeader *erh,
						   const Datum *newValues, const bool *isnulls,
						   bool expand_external)
{
	TupleDesc	tupdesc;
	Datum	   *dvalues;
	bool	   *dnulls;
	int			fnumber;
	MemoryContext oldcxt;

	/* 绝不应试图向虚设头部赋予新数据 */
	Assert(!(erh->flags & ER_FLAG_IS_DUMMY));

	/* 若尚未解构元组，就先解构 */
	if (!(erh->flags & ER_FLAG_DVALUES_VALID))
		deconstruct_expanded_record(erh);

	/* 此时元组描述符必须有效 */
	tupdesc = erh->er_tupdesc;
	Assert(erh->nfields == tupdesc->natts);

	/* 展平值将不再准确表示该记录 */
	erh->flags &= ~ER_FLAG_FVALUE_VALID;
	/* 而且我们也不知道展平大小 */
	erh->flat_size = 0;

	oldcxt = MemoryContextSwitchTo(erh->hdr.eoh_context);

	dvalues = erh->dvalues;
	dnulls = erh->dnulls;

	for (fnumber = 0; fnumber < erh->nfields; fnumber++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, fnumber);
		Datum		newValue;
		bool		isnull;

		/* 忽略已删除列 */
		if (attr->attisdropped)
			continue;

		newValue = newValues[fnumber];
		isnull = isnulls[fnumber];

		if (!attr->attbyval)
		{
			/*
			 * 把新字段值拷入记录的上下文，并视需要
			 * 处理解 TOAST。
			 */
			if (!isnull)
			{
				/* 它是外部 TOAST 值吗？ */
				if (attr->attlen == -1 &&
					VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
				{
					if (expand_external)
					{
						/* 拷贝值的同时按要求解 TOAST */
						newValue = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(newValue)));
					}
					else
					{
						/* 直接拷贝值 */
						newValue = datumCopy(newValue, false, -1);
						/* 若仍是外部值，记住这点 */
						if (VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
							erh->flags |= ER_FLAG_HAVE_EXTERNAL;
					}
				}
				else
				{
					/* 非外部值，直接拷贝 */
					newValue = datumCopy(newValue, false, attr->attlen);
				}

				/* 记住我们有需要 pfree 的字段 */
				erh->flags |= ER_FLAG_DVALUES_ALLOCED;
			}

			/*
			 * 释放旧字段值（若有）（不太可能，因为我们
			 * 理应是在向空记录插入）。
			 */
			if (unlikely(!dnulls[fnumber]))
			{
				char	   *oldValue;

				oldValue = (char *) DatumGetPointer(dvalues[fnumber]);
				/* 不要试图 pfree 原平面记录的一部分 */
				if (oldValue < erh->fstartptr || oldValue >= erh->fendptr)
					pfree(oldValue);
			}
		}

		/* 最后即可插入新字段了。 */
		dvalues[fnumber] = newValue;
		dnulls[fnumber] = isnull;
	}

	/*
	 * 因为我们不保证 set_fields() 的原子性，可以把
	 * 域约束检查留作最后一步；若它抛出错误，那
	 * 也没办法。
	 */
	if (erh->flags & ER_FLAG_IS_DOMAIN)
	{
		/* 在短期上下文中运行 domain_check 以限制垃圾 */
		MemoryContextSwitchTo(get_short_term_cxt(erh));

		domain_check(ExpandedRecordGetRODatum(erh), false,
					 erh->er_decltypeid,
					 &erh->er_domaininfo,
					 erh->hdr.eoh_context);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * 为短期操作构建（或重置）工作内存上下文。
 *
 * 该上下文用于域检查求值以及解 TOAST。
 *
 * 如果没有短期内存上下文就建一个；如果已有，
 * 就重置它以清除残留的垃圾。（需要为它专门建
 * 个完整上下文有点烦人，因为它常常用不到 —— 但
 * 不用它很难避免内存泄漏。至少可以把上下文设小。）
 */
static MemoryContext
get_short_term_cxt(ExpandedRecordHeader *erh)
{
	if (erh->er_short_term_cxt == NULL)
		erh->er_short_term_cxt =
			AllocSetContextCreate(erh->hdr.eoh_context,
								  "expanded record short-term context",
								  ALLOCSET_SMALL_SIZES);
	else
		MemoryContextReset(erh->er_short_term_cxt);
	return erh->er_short_term_cxt;
}

/*
 * 构建用于检查域约束的"虚设头部"。
 *
 * 由于在验证完约束之前我们不想修改展开记录的
 * 状态，我们的做法是建立一个含有新字段值的虚设
 * 记录头部，然后把它传给 domain_check。为节省
 * palloc 周期，我们保留虚设头部作为展开记录状态
 * 的一部分，但每次使用时都会重新初始化
 * 其大部分内容。
 */
static void
build_dummy_expanded_header(ExpandedRecordHeader *main_erh)
{
	ExpandedRecordHeader *erh;
	TupleDesc	tupdesc = expanded_record_get_tupdesc(main_erh);

	/* 确保我们已有短期上下文 */
	(void) get_short_term_cxt(main_erh);

	/*
	 * 首次经过时分配虚设头部，或者在字段数发生变
	 * 化的不太可能情形下分配（此时我们直接泄漏旧
	 * 的那个）。请求中要包含其字段值的空间。
	 */
	erh = main_erh->er_dummy_header;
	if (erh == NULL || erh->nfields != tupdesc->natts)
	{
		char	   *chunk;

		erh = (ExpandedRecordHeader *)
			MemoryContextAlloc(main_erh->hdr.eoh_context,
							   MAXALIGN(sizeof(ExpandedRecordHeader))
							   + tupdesc->natts * (sizeof(Datum) + sizeof(bool)));

		/* 确保所有头部字段都初始化为 0/null */
		memset(erh, 0, sizeof(ExpandedRecordHeader));

		/*
		 * 我们建立虚设头部时指明其内存上下文是短期
		 * 上下文。这样，如果有任何行外值因试图
		 * 提取复合 datum 而发生解 TOAST，解出来的
		 * 内容就会落在短期上下文中而不会造成泄漏。
		 * 这在展开对象协议上有点作弊；但由于
		 * 我们从不把虚设对象的读/写指针传给其他代码，
		 * 所以没有其他代码被授权删除或转移该对象上下文
		 * 的所有权，因此应该是足够
		 * 安全的。
		 */
		EOH_init_header(&erh->hdr, &ER_methods, main_erh->er_short_term_cxt);
		erh->er_magic = ER_MAGIC;

		/* 设置 dvalues/dnulls，目前尚无有效内容 */
		chunk = (char *) erh + MAXALIGN(sizeof(ExpandedRecordHeader));
		erh->dvalues = (Datum *) chunk;
		erh->dnulls = (bool *) (chunk + tupdesc->natts * sizeof(Datum));
		erh->nfields = tupdesc->natts;

		/*
		 * 刚设置的字段被假定在虚设头部的多次使用
		 * （用于域约束检查）期间保持不变。所有其他虚设
		 * 头部字段都应在下方显式重置，确保一次检查
		 * 不会对下一次检查产生意外影响。
		 */

		main_erh->er_dummy_header = erh;
	}

	/*
	 * 如果有任何东西查询虚设头部的声明类型，它应当
	 * 报告复合基础类型而非域类型（因为域检查约束中
	 * 的 VALUE 属于基础类型而非域）。因此我们
	 * 不转移 IS_DOMAIN 标志，实际上主头部的任何标志
	 * 也都不转移，因为此时虚设头部不包含数据。但别
	 * 忘了把头部标为虚设。
	 */
	erh->flags = ER_FLAG_IS_DUMMY;

	/* 拷贝复合类型标识信息 */
	erh->er_decltypeid = erh->er_typeid = main_erh->er_typeid;
	erh->er_typmod = main_erh->er_typmod;

	/* 虚设头部无需元组描述符引用计数 */
	erh->er_tupdesc = tupdesc;
	erh->er_tupdesc_id = main_erh->er_tupdesc_id;

	/*
	 * 把我们已知的平面大小信息拷过来很诱人，但这
	 * 没有意义，因为我们肯定马上就要修改虚设记录的
	 * 字段。因此只需清除上一轮使用周期的遗留东
	 * 西。
	 */
	erh->flat_size = 0;

	/* 若有 fvalue 则一并拷贝，使系统列可用 */
	erh->fvalue = main_erh->fvalue;
	erh->fstartptr = main_erh->fstartptr;
	erh->fendptr = main_erh->fendptr;
}

/*
 * 为 set_field 操作预检域约束
 */
static pg_noinline void
check_domain_for_new_field(ExpandedRecordHeader *erh, int fnumber,
						   Datum newValue, bool isnull)
{
	ExpandedRecordHeader *dummy_erh;
	MemoryContext oldcxt;

	/* 构建容纳拟定新字段集的虚设头部 */
	build_dummy_expanded_header(erh);
	dummy_erh = erh->er_dummy_header;

	/*
	 * 如果记录非空，只需解构它（如有需要）并拷贝
	 * 现有的字段值。如果为空，就手动用空值填充字段
	 * —— 不要过早调用 deconstruct_expanded_record。
	 */
	if (!ExpandedRecordIsEmpty(erh))
	{
		deconstruct_expanded_record(erh);
		memcpy(dummy_erh->dvalues, erh->dvalues,
			   dummy_erh->nfields * sizeof(Datum));
		memcpy(dummy_erh->dnulls, erh->dnulls,
			   dummy_erh->nfields * sizeof(bool));
		/* 里面可能有外部值…… */
		dummy_erh->flags |= erh->flags & ER_FLAG_HAVE_EXTERNAL;
	}
	else
	{
		memset(dummy_erh->dvalues, 0, dummy_erh->nfields * sizeof(Datum));
		memset(dummy_erh->dnulls, true, dummy_erh->nfields * sizeof(bool));
	}

	/* 反正现在 dvalues 都有效 */
	dummy_erh->flags |= ER_FLAG_DVALUES_VALID;

	/* fnumber 为系统列或不存在列时属调用者错误 */
	if (unlikely(fnumber <= 0 || fnumber > dummy_erh->nfields))
		elog(ERROR, "cannot assign to field %d of expanded record", fnumber);

	/* 把拟定新值插入虚设字段数组 */
	dummy_erh->dvalues[fnumber - 1] = newValue;
	dummy_erh->dnulls[fnumber - 1] = isnull;

	/*
	 * 拟定的新值可能是外部的，此时我们最好在
	 * dummy_erh 中为它设置相应标志。（这一点很重要，
	 * 以防域检查表达式试图从虚设头部提取平面值。
	 * ）。
	 */
	if (!isnull)
	{
		Form_pg_attribute attr = TupleDescAttr(erh->er_tupdesc, fnumber - 1);

		if (!attr->attbyval && attr->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(newValue)))
			dummy_erh->flags |= ER_FLAG_HAVE_EXTERNAL;
	}

	/*
	 * 我们在短期上下文中调用 domain_check，以便
	 * 表达式求值泄漏的垃圾可被回收。
	 */
	oldcxt = MemoryContextSwitchTo(erh->er_short_term_cxt);

	/*
	 * 现在可以应用检查了。注意我们使用主头部的域
	 * 缓存空间，使缓存在多次使用间延续。
	 */
	domain_check(ExpandedRecordGetRODatum(dummy_erh), false,
				 erh->er_decltypeid,
				 &erh->er_domaininfo,
				 erh->hdr.eoh_context);

	MemoryContextSwitchTo(oldcxt);

	/* 不妨立即清理垃圾。 */
	MemoryContextReset(erh->er_short_term_cxt);
}

/*
 * 为 set_tuple 操作预检域约束
 */
static pg_noinline void
check_domain_for_new_tuple(ExpandedRecordHeader *erh, HeapTuple tuple)
{
	ExpandedRecordHeader *dummy_erh;
	MemoryContext oldcxt;

	/* 若要求把记录置为空，只需看 NULL 是否可行 */
	if (tuple == NULL)
	{
		/* 在短期上下文中运行 domain_check 以限制垃圾 */
		oldcxt = MemoryContextSwitchTo(get_short_term_cxt(erh));

		domain_check((Datum) 0, true,
					 erh->er_decltypeid,
					 &erh->er_domaininfo,
					 erh->hdr.eoh_context);

		MemoryContextSwitchTo(oldcxt);

		/* 不妨立即清理垃圾。 */
		MemoryContextReset(erh->er_short_term_cxt);

		return;
	}

	/* 构建容纳替换元组的虚设头部 */
	build_dummy_expanded_header(erh);
	dummy_erh = erh->er_dummy_header;

	/* 插入元组，但现在不费心解构其字段 */
	dummy_erh->fvalue = tuple;
	dummy_erh->fstartptr = (char *) tuple->t_data;
	dummy_erh->fendptr = ((char *) tuple->t_data) + tuple->t_len;
	dummy_erh->flags |= ER_FLAG_FVALUE_VALID;

	/* 记住我们是否有行外字段值 */
	if (HeapTupleHasExternal(tuple))
		dummy_erh->flags |= ER_FLAG_HAVE_EXTERNAL;

	/*
	 * 我们在短期上下文中调用 domain_check，以便
	 * 表达式求值泄漏的垃圾可被回收。
	 */
	oldcxt = MemoryContextSwitchTo(erh->er_short_term_cxt);

	/*
	 * 现在可以应用检查了。注意我们使用主头部的域
	 * 缓存空间，使缓存在多次使用间延续。
	 */
	domain_check(ExpandedRecordGetRODatum(dummy_erh), false,
				 erh->er_decltypeid,
				 &erh->er_domaininfo,
				 erh->hdr.eoh_context);

	MemoryContextSwitchTo(oldcxt);

	/* 不妨立即清理垃圾。 */
	MemoryContextReset(erh->er_short_term_cxt);
}
