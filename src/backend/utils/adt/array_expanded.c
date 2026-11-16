/*-------------------------------------------------------------------------
 *
 * array_expanded.c
 *	  操作展开数组的基本函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_expanded.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


/* 展开对象所需的 "Methods" */
static Size EA_get_flat_size(ExpandedObjectHeader *eohptr);
static void EA_flatten_into(ExpandedObjectHeader *eohptr,
							void *result, Size allocated_size);

static const ExpandedObjectMethods EA_methods =
{
	EA_get_flat_size,
	EA_flatten_into
};

/* 其他本地函数 */
static void copy_byval_expanded_array(ExpandedArrayHeader *eah,
									  ExpandedArrayHeader *oldeah);


/*
 * expand_array：把数组 Datum 转换成展开数组
 *
 * 展开对象将成为 parentcontext 的子对象。
 *
 * 一些调用者可以提供缓存空间，以避免跨调用反
 * 复查找元素类型数据；若是，传入 metacache 指针，
 * 确保 metacache->element_type 在首次调用前初始化
 * 为 InvalidOid。无需跨调用缓存时传 NULL。
 */
Datum
expand_array(Datum arraydatum, MemoryContext parentcontext,
			 ArrayMetaState *metacache)
{
	ArrayType  *array;
	ExpandedArrayHeader *eah;
	MemoryContext objcxt;
	MemoryContext oldcxt;
	ArrayMetaState fakecache;

	/*
	 * 为展开对象分配私有上下文。我们起初假定数
	 * 组不会很大；但如果它真的增长很多，也不要限
	 * 制 aset.c 的大上下文行为。
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded array",
								   ALLOCSET_START_SMALL_SIZES);

	/* 设置展开数组头部 */
	eah = (ExpandedArrayHeader *)
		MemoryContextAlloc(objcxt, sizeof(ExpandedArrayHeader));

	EOH_init_header(&eah->hdr, &EA_methods, objcxt);
	eah->ea_magic = EA_MAGIC;

	/* 若源是展开数组，我们或许可以优化 */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(arraydatum)))
	{
		ExpandedArrayHeader *oldeah = (ExpandedArrayHeader *) DatumGetEOHP(arraydatum);

		Assert(oldeah->ea_magic == EA_MAGIC);

		/*
		 * 若调用者给了缓存则更新；这次虽用不到，
		 * 但下一次调用可能是非展开源数组。此外，
		 * 若未提供缓存区，也用本地存储缓存，从而
		 * 在落入平面拷贝代码路径时避免
		 * 目录查找。
		 */
		if (metacache == NULL)
			metacache = &fakecache;
		metacache->element_type = oldeah->element_type;
		metacache->typlen = oldeah->typlen;
		metacache->typbyval = oldeah->typbyval;
		metacache->typalign = oldeah->typalign;

		/*
		 * 若元素类型按值传递且有 Datum 数组，
		 * 就直接拷贝源的元数据与 Datum/isnull 数组。
		 * 原平面数组即使存在也无额外信息，
		 * 无需拷贝它。
		 */
		if (oldeah->typbyval && oldeah->dvalues != NULL)
		{
			copy_byval_expanded_array(eah, oldeah);
			/* 返回指向展开数组的读/写指针 */
			return EOHPGetRWDatum(&eah->hdr);
		}

		/*
		 * 否则，或只有平面表示，或元素按引用传
		 * 递。无论哪种情况，最好都是先将源按平面表
		 * 示拷贝，之后必要时再解构。对按引用的
		 * 情况，或许可以用自定义代码在拷贝值时同
		 * 步生成解构表示可省些开销，但要为此写大
		 * 量额外代码而收益却相当有限。因此，直接
		 * 落进平面源的代
		 * 码路径。
		 */
	}

	/*
	 * 把源数组解 TOAST 后作为平面数组拷入私有上下文。
	 *
	 * 注意这样写法可能在私有上下文中泄漏一些内
	 * 存；若必须从 TOAST 表取数据的话。不过实验表
	 * 明这样泄漏极少。这样做省去一次拷贝步骤，看
	 * 来是值得的，尤其当数组大到需要外部存
	 * 储时。
	 */
	oldcxt = MemoryContextSwitchTo(objcxt);
	array = DatumGetArrayTypePCopy(arraydatum);
	MemoryContextSwitchTo(oldcxt);

	eah->ndims = ARR_NDIM(array);
	/* 注意这些指针指向 fvalue 头部！ */
	eah->dims = ARR_DIMS(array);
	eah->lbound = ARR_LBOUND(array);

	/* 保存数组的元素类型数据以备后用 */
	eah->element_type = ARR_ELEMTYPE(array);
	if (metacache && metacache->element_type == eah->element_type)
	{
		/* 我们已有表示数据的有效缓存 */
		eah->typlen = metacache->typlen;
		eah->typbyval = metacache->typbyval;
		eah->typalign = metacache->typalign;
	}
	else
	{
		/* 否则查找 */
		get_typlenbyvalalign(eah->element_type,
							 &eah->typlen,
							 &eah->typbyval,
							 &eah->typalign);
		/* 如有缓存则更新 */
		if (metacache)
		{
			metacache->element_type = eah->element_type;
			metacache->typlen = eah->typlen;
			metacache->typbyval = eah->typbyval;
			metacache->typalign = eah->typalign;
		}
	}

	/* 现在不构建解构后的表示 */
	eah->dvalues = NULL;
	eah->dnulls = NULL;
	eah->dvalueslen = 0;
	eah->nelems = 0;
	eah->flat_size = 0;

	/* 记住我们已有平面表示 */
	eah->fvalue = array;
	eah->fstartptr = ARR_DATA_PTR(array);
	eah->fendptr = ((char *) array) + ARR_SIZE(array);

	/* 返回指向展开数组的读/写指针 */
	return EOHPGetRWDatum(&eah->hdr);
}

/*
 * expand_array() 辅助例程：拷贝按值 Datum 数组表示
 */
static void
copy_byval_expanded_array(ExpandedArrayHeader *eah,
						  ExpandedArrayHeader *oldeah)
{
	MemoryContext objcxt = eah->hdr.eoh_context;
	int			ndims = oldeah->ndims;
	int			dvalueslen = oldeah->dvalueslen;

	/* 拷贝数组维度信息 */
	eah->ndims = ndims;
	/* 可以用一次 palloc 分配两个维度数组 */
	eah->dims = (int *) MemoryContextAlloc(objcxt, ndims * 2 * sizeof(int));
	eah->lbound = eah->dims + ndims;
	/* 但不要假定源的数组是连续的 */
	memcpy(eah->dims, oldeah->dims, ndims * sizeof(int));
	memcpy(eah->lbound, oldeah->lbound, ndims * sizeof(int));

	/* 拷元素类型数据 */
	eah->element_type = oldeah->element_type;
	eah->typlen = oldeah->typlen;
	eah->typbyval = oldeah->typbyval;
	eah->typalign = oldeah->typalign;

	/* 拷贝解构后的表示 */
	eah->dvalues = (Datum *) MemoryContextAlloc(objcxt,
												dvalueslen * sizeof(Datum));
	memcpy(eah->dvalues, oldeah->dvalues, dvalueslen * sizeof(Datum));
	if (oldeah->dnulls)
	{
		eah->dnulls = (bool *) MemoryContextAlloc(objcxt,
												  dvalueslen * sizeof(bool));
		memcpy(eah->dnulls, oldeah->dnulls, dvalueslen * sizeof(bool));
	}
	else
		eah->dnulls = NULL;
	eah->dvalueslen = dvalueslen;
	eah->nelems = oldeah->nelems;
	eah->flat_size = oldeah->flat_size;

	/* 我们不构建平面表示 */
	eah->fvalue = NULL;
	eah->fstartptr = NULL;
	eah->fendptr = NULL;
}

/*
 * 展开数组的 get_flat_size 方法
 */
static Size
EA_get_flat_size(ExpandedObjectHeader *eohptr)
{
	ExpandedArrayHeader *eah = (ExpandedArrayHeader *) eohptr;
	int			nelems;
	int			ndims;
	Datum	   *dvalues;
	bool	   *dnulls;
	Size		nbytes;
	int			i;

	Assert(eah->ea_magic == EA_MAGIC);

	/* 已有有效的扁平值时很简单 */
	if (eah->fvalue)
		return ARR_SIZE(eah->fvalue);

	/* 如果缓存了大小值，就相信它 */
	if (eah->flat_size)
		return eah->flat_size;

	/*
	 * 检查 dvalues/dnulls 来计算所需空间。注意结果数
	 * 组若 dnulls 非 NULL 就会带有空值位图，即使该数
	 * 组现在并不真含空值。
	 */
	nelems = eah->nelems;
	ndims = eah->ndims;
	Assert(nelems == ArrayGetNItems(ndims, eah->dims));
	dvalues = eah->dvalues;
	dnulls = eah->dnulls;
	nbytes = 0;
	for (i = 0; i < nelems; i++)
	{
		if (dnulls && dnulls[i])
			continue;
		nbytes = att_addlength_datum(nbytes, eah->typlen, dvalues[i]);
		nbytes = att_align_nominal(nbytes, eah->typalign);
		/* 检查总请求是否溢出 */
		if (!AllocSizeIsValid(nbytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxAllocSize)));
	}

	if (dnulls)
		nbytes += ARR_OVERHEAD_WITHNULLS(ndims, nelems);
	else
		nbytes += ARR_OVERHEAD_NONULLS(ndims);

	/* 缓存供下次用 */
	eah->flat_size = nbytes;

	return nbytes;
}

/*
 * 展开数组的 flatten_into 方法
 */
static void
EA_flatten_into(ExpandedObjectHeader *eohptr,
				void *result, Size allocated_size)
{
	ExpandedArrayHeader *eah = (ExpandedArrayHeader *) eohptr;
	ArrayType  *aresult = (ArrayType *) result;
	int			nelems;
	int			ndims;
	int32		dataoffset;

	Assert(eah->ea_magic == EA_MAGIC);

	/* 已有有效的扁平值时很简单 */
	if (eah->fvalue)
	{
		Assert(allocated_size == ARR_SIZE(eah->fvalue));
		memcpy(result, eah->fvalue, allocated_size);
		return;
	}

	/* 否则分配应与之前 get_flat_size 结果一致 */
	Assert(allocated_size == eah->flat_size);

	/* 由 dvalues/dnulls 填结果数组 */
	nelems = eah->nelems;
	ndims = eah->ndims;

	if (eah->dnulls)
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nelems);
	else
		dataoffset = 0;			/* 无空值位图标记 */

	/* 必须确保任何填充空间都清零 */
	memset(aresult, 0, allocated_size);

	SET_VARSIZE(aresult, allocated_size);
	aresult->ndim = ndims;
	aresult->dataoffset = dataoffset;
	aresult->elemtype = eah->element_type;
	memcpy(ARR_DIMS(aresult), eah->dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(aresult), eah->lbound, ndims * sizeof(int));

	CopyArrayEls(aresult,
				 eah->dvalues, eah->dnulls, nelems,
				 eah->typlen, eah->typbyval, eah->typalign,
				 false);
}

/*
 * 参数获取支持代码
 */

/*
 * DatumGetExpandedArray：从输入参数取得可写的展开数组
 *
 * 注意：若输入是读/写指针，这里会返回该输入参
 * 数本身；因此调用者必须确保修改是"安全"的，即
 * 它们不会把数组留在损坏状态。
 */
ExpandedArrayHeader *
DatumGetExpandedArray(Datum d)
{
	/* 如果已是可写的展开数组，直接返回它 */
	if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
	{
		ExpandedArrayHeader *eah = (ExpandedArrayHeader *) DatumGetEOHP(d);

		Assert(eah->ea_magic == EA_MAGIC);
		return eah;
	}

	/* 否则费劲展开 */
	d = expand_array(d, CurrentMemoryContext, NULL);
	return (ExpandedArrayHeader *) DatumGetEOHP(d);
}

/*
 * 同上，用于调用者能缓存元素类型信息的情况
 */
ExpandedArrayHeader *
DatumGetExpandedArrayX(Datum d, ArrayMetaState *metacache)
{
	/* 如果已是可写的展开数组，直接返回它 */
	if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
	{
		ExpandedArrayHeader *eah = (ExpandedArrayHeader *) DatumGetEOHP(d);

		Assert(eah->ea_magic == EA_MAGIC);
		/* 如有缓存则更新 */
		if (metacache)
		{
			metacache->element_type = eah->element_type;
			metacache->typlen = eah->typlen;
			metacache->typbyval = eah->typbyval;
			metacache->typalign = eah->typalign;
		}
		return eah;
	}

	/* 否则用调用者缓存来展开 */
	d = expand_array(d, CurrentMemoryContext, metacache);
	return (ExpandedArrayHeader *) DatumGetEOHP(d);
}

/*
 * DatumGetAnyArrayP：返回展开数组或者解 TOAST 后的
 * varlena 数组。结果不得被就地修改。
 */
AnyArrayType *
DatumGetAnyArrayP(Datum d)
{
	ExpandedArrayHeader *eah;

	/*
	 * 若是展开数组（读/写或只读），返回头指针。
	 */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(d)))
	{
		eah = (ExpandedArrayHeader *) DatumGetEOHP(d);
		Assert(eah->ea_magic == EA_MAGIC);
		return (AnyArrayType *) eah;
	}

	/* 否则按需做常规的解 TOAST */
	return (AnyArrayType *) PG_DETOAST_DATUM(d);
}

/*
 * 为展开数组对象创建 Datum/isnull 表示
 * （若此前未做过）
 */
void
deconstruct_expanded_array(ExpandedArrayHeader *eah)
{
	if (eah->dvalues == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(eah->hdr.eoh_context);
		Datum	   *dvalues;
		bool	   *dnulls;
		int			nelems;

		dnulls = NULL;
		deconstruct_array(eah->fvalue,
						  eah->element_type,
						  eah->typlen, eah->typbyval, eah->typalign,
						  &dvalues,
						  ARR_HASNULL(eah->fvalue) ? &dnulls : NULL,
						  &nelems);

		/*
		 * 只有在这一步成功完成后才更新头部。如果
		 * deconstruct_array 中途失败，最坏后果只是对象上
		 * 下文泄漏内存。调用者若在更晚时点失败也没
		 * 关系，因为解构后的表示无论如何是
		 * 有效的。
		 */
		eah->dvalues = dvalues;
		eah->dnulls = dnulls;
		eah->dvalueslen = eah->nelems = nelems;
		MemoryContextSwitchTo(oldcxt);
	}
}
