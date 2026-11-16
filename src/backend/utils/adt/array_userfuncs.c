/*-------------------------------------------------------------------------
 *
 * array_userfuncs.c
 *	  用户可见的数组支持函数。
 *
 * Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_userfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "common/int.h"
#include "common/pg_prng.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

/*
 * SerialIOData
 *		用于在 array_agg_serialize 中缓存元素类型数据
 */
typedef struct SerialIOData
{
	FmgrInfo	typsend;
} SerialIOData;

/*
 * DeserialIOData
 *		用于在 array_agg_deserialize 中缓存元素类型数据
 */
typedef struct DeserialIOData
{
	FmgrInfo	typreceive;
	Oid			typioparam;
} DeserialIOData;

static Datum array_position_common(FunctionCallInfo fcinfo);


/*
 * fetch_array_arg_replace_nulls
 *
 * 以展开形式取数组值参数；如果它为空，就构建
 * 一个相应数据类型的空数组值。同时把基本元素类型
 * 信息缓存在其中。
 *
 * 小心：输入若是读/写指针，会返回输入参数；
 * 因此调用者必须确保自己的修改是"安全的"，也就是
 * 无法把数组留在损坏状态。
 *
 * 如果我们是作为聚合函数被调用，务必确保任何新
 * 构建的展开数组都分配在聚合状态上下文中，从而
 * 节省拷贝。
 */
static ExpandedArrayHeader *
fetch_array_arg_replace_nulls(FunctionCallInfo fcinfo, int argno)
{
	ExpandedArrayHeader *eah;
	Oid			element_type;
	ArrayMetaState *my_extra;
	MemoryContext resultcxt;

	/* 首次经过时创建数据类型缓存结构 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		my_extra = (ArrayMetaState *)
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   sizeof(ArrayMetaState));
		my_extra->element_type = InvalidOid;
		fcinfo->flinfo->fn_extra = my_extra;
	}

	/* 确定我们想让结果处于哪个上下文 */
	if (!AggCheckCallContext(fcinfo, &resultcxt))
		resultcxt = CurrentMemoryContext;

	/* 现在收集数组值 */
	if (!PG_ARGISNULL(argno))
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(resultcxt);

		eah = PG_GETARG_EXPANDED_ARRAYX(argno, my_extra);
		MemoryContextSwitchTo(oldcxt);
	}
	else
	{
		/* 我们必须查找数组类型与元素类型 */
		Oid			arr_typeid = get_fn_expr_argtype(fcinfo->flinfo, argno);

		if (!OidIsValid(arr_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));
		element_type = get_element_type(arr_typeid);
		if (!OidIsValid(element_type))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("input data type is not an array")));

		eah = construct_empty_expanded_array(element_type,
											 resultcxt,
											 my_extra);
	}

	return eah;
}

/*-----------------------------------------------------------------------------
 * array_append :
 *		把一个元素压入一维数组的末尾
 *----------------------------------------------------------------------------
 */
Datum
array_append(PG_FUNCTION_ARGS)
{
	ExpandedArrayHeader *eah;
	Datum		newelem;
	bool		isNull;
	Datum		result;
	int		   *dimv,
			   *lb;
	int			indx;
	ArrayMetaState *my_extra;

	eah = fetch_array_arg_replace_nulls(fcinfo, 0);
	isNull = PG_ARGISNULL(1);
	if (isNull)
		newelem = (Datum) 0;
	else
		newelem = PG_GETARG_DATUM(1);

	if (eah->ndims == 1)
	{
		/* 追加 newelem */
		lb = eah->lbound;
		dimv = eah->dims;

		/* 被加元素索引在 lb[0] + (dimv[0] - 1) + 1 */
		if (pg_add_s32_overflow(lb[0], dimv[0], &indx))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}
	else if (eah->ndims == 0)
		indx = 1;
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must be empty or one-dimensional array")));

	/* 执行元素插入 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;

	result = array_set_element(EOHPGetRWDatum(&eah->hdr),
							   1, &indx, newelem, isNull,
							   -1, my_extra->typlen, my_extra->typbyval, my_extra->typalign);

	PG_RETURN_DATUM(result);
}

/*-----------------------------------------------------------------------------
 * array_prepend :
 *		把一个元素压入一维数组的前端
 *----------------------------------------------------------------------------
 */
Datum
array_prepend(PG_FUNCTION_ARGS)
{
	ExpandedArrayHeader *eah;
	Datum		newelem;
	bool		isNull;
	Datum		result;
	int		   *lb;
	int			indx;
	int			lb0;
	ArrayMetaState *my_extra;

	isNull = PG_ARGISNULL(0);
	if (isNull)
		newelem = (Datum) 0;
	else
		newelem = PG_GETARG_DATUM(0);
	eah = fetch_array_arg_replace_nulls(fcinfo, 1);

	if (eah->ndims == 1)
	{
		/* 前置 newelem */
		lb = eah->lbound;
		lb0 = lb[0];

		if (pg_sub_s32_overflow(lb0, 1, &indx))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}
	else if (eah->ndims == 0)
	{
		indx = 1;
		lb0 = 1;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must be empty or one-dimensional array")));

	/* 执行元素插入 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;

	result = array_set_element(EOHPGetRWDatum(&eah->hdr),
							   1, &indx, newelem, isNull,
							   -1, my_extra->typlen, my_extra->typbyval, my_extra->typalign);

	/* 重设结果的 LB 以匹配输入，prepend 需如此 */
	Assert(result == EOHPGetRWDatum(&eah->hdr));
	if (eah->ndims == 1)
	{
		/* 无论是否已解构都没问题 */
		eah->lbound[0] = lb0;
	}

	PG_RETURN_DATUM(result);
}

/*-----------------------------------------------------------------------------
 * array_cat :
 *		把两个 nD 数组连接成 nD 数组，或者
 *		把 (n-1)D 数组压入 nD 数组的末尾
 *----------------------------------------------------------------------------
 */
Datum
array_cat(PG_FUNCTION_ARGS)
{
	ArrayType  *v1,
			   *v2;
	ArrayType  *result;
	int		   *dims,
			   *lbs,
				ndims,
				nitems,
				ndatabytes,
				nbytes;
	int		   *dims1,
			   *lbs1,
				ndims1,
				nitems1,
				ndatabytes1;
	int		   *dims2,
			   *lbs2,
				ndims2,
				nitems2,
				ndatabytes2;
	int			i;
	char	   *dat1,
			   *dat2;
	bits8	   *bitmap1,
			   *bitmap2;
	Oid			element_type;
	Oid			element_type1;
	Oid			element_type2;
	int32		dataoffset;

	/* 连接空数组是无操作，直接返回另一个输入 */
	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();
		result = PG_GETARG_ARRAYTYPE_P(1);
		PG_RETURN_ARRAYTYPE_P(result);
	}
	if (PG_ARGISNULL(1))
	{
		result = PG_GETARG_ARRAYTYPE_P(0);
		PG_RETURN_ARRAYTYPE_P(result);
	}

	v1 = PG_GETARG_ARRAYTYPE_P(0);
	v2 = PG_GETARG_ARRAYTYPE_P(1);

	element_type1 = ARR_ELEMTYPE(v1);
	element_type2 = ARR_ELEMTYPE(v2);

	/* 检查两者元素类型是否匹配 */
	if (element_type1 != element_type2)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot concatenate incompatible arrays"),
				 errdetail("Arrays with element types %s and %s are not "
						   "compatible for concatenation.",
						   format_type_be(element_type1),
						   format_type_be(element_type2))));

	/* 就用它 */
	element_type = element_type1;

	/*----------
	 * 我们必须有以下输入组合之一：
	 * 1) 一个空数组与一个非空数组
	 * 2) 两数组皆空
	 * 3) 两数组满足 ndims1 == ndims2
	 * 4) ndims1 == ndims2 - 1
	 * 5) ndims1 == ndims2 + 1
	 *----------
	 */
	ndims1 = ARR_NDIM(v1);
	ndims2 = ARR_NDIM(v2);

	/*
	 * 短路 —— 如果一个输入数组为空而另一个不为空，
	 * 就返回非空的那个作为结果
	 *
	 * 若两者皆空，返回第一个
	 */
	if (ndims1 == 0 && ndims2 > 0)
		PG_RETURN_ARRAYTYPE_P(v2);

	if (ndims2 == 0)
		PG_RETURN_ARRAYTYPE_P(v1);

	/* 其余都属规则 3、4 或 5 */
	if (ndims1 != ndims2 &&
		ndims1 != ndims2 - 1 &&
		ndims1 != ndims2 + 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("cannot concatenate incompatible arrays"),
				 errdetail("Arrays of %d and %d dimensions are not "
						   "compatible for concatenation.",
						   ndims1, ndims2)));

	/* 取参数数组详情 */
	lbs1 = ARR_LBOUND(v1);
	lbs2 = ARR_LBOUND(v2);
	dims1 = ARR_DIMS(v1);
	dims2 = ARR_DIMS(v2);
	dat1 = ARR_DATA_PTR(v1);
	dat2 = ARR_DATA_PTR(v2);
	bitmap1 = ARR_NULLBITMAP(v1);
	bitmap2 = ARR_NULLBITMAP(v2);
	nitems1 = ArrayGetNItems(ndims1, dims1);
	nitems2 = ArrayGetNItems(ndims2, dims2);
	ndatabytes1 = ARR_SIZE(v1) - ARR_DATA_OFFSET(v1);
	ndatabytes2 = ARR_SIZE(v2) - ARR_DATA_OFFSET(v2);

	if (ndims1 == ndims2)
	{
		/*
		 * 结果数组由输入参数数组的元素（可能是
		 * 数组本身）构成
		 */
		ndims = ndims1;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));

		dims[0] = dims1[0] + dims2[0];
		lbs[0] = lbs1[0];

		for (i = 1; i < ndims; i++)
		{
			if (dims1[i] != dims2[i] || lbs1[i] != lbs2[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot concatenate incompatible arrays"),
						 errdetail("Arrays with differing element dimensions are "
								   "not compatible for concatenation.")));

			dims[i] = dims1[i];
			lbs[i] = lbs1[i];
		}
	}
	else if (ndims1 == ndims2 - 1)
	{
		/*
		 * 结果数组以第二个参数为外层数组，第一个参数
		 * 被插入在外层维度的前端
		 */
		ndims = ndims2;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));
		memcpy(dims, dims2, ndims * sizeof(int));
		memcpy(lbs, lbs2, ndims * sizeof(int));

		/* 递增外层数组的元素个数 */
		dims[0] += 1;

		/* 确保被加元素与我们已有的元素匹配 */
		for (i = 0; i < ndims1; i++)
		{
			if (dims1[i] != dims[i + 1] || lbs1[i] != lbs[i + 1])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot concatenate incompatible arrays"),
						 errdetail("Arrays with differing dimensions are not "
								   "compatible for concatenation.")));
		}
	}
	else
	{
		/*
		 * (ndims1 == ndims2 + 1)
		 *
		 * 结果数组以第一个参数作为外层数组，第二个参数
		 * 被追加在外层维度的末尾
		 */
		ndims = ndims1;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));
		memcpy(dims, dims1, ndims * sizeof(int));
		memcpy(lbs, lbs1, ndims * sizeof(int));

		/* 递增外层数组的元素个数 */
		dims[0] += 1;

		/* 确保被加元素与我们已有的元素匹配 */
		for (i = 0; i < ndims2; i++)
		{
			if (dims2[i] != dims[i + 1] || lbs2[i] != lbs[i + 1])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot concatenate incompatible arrays"),
						 errdetail("Arrays with differing dimensions are not "
								   "compatible for concatenation.")));
		}
	}

	/* 这样做主要为了溢出检查 */
	nitems = ArrayGetNItems(ndims, dims);
	ArrayCheckBounds(ndims, dims, lbs);

	/* 构建结果数组 */
	ndatabytes = ndatabytes1 + ndatabytes2;
	if (ARR_HASNULL(v1) || ARR_HASNULL(v2))
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
		nbytes = ndatabytes + dataoffset;
	}
	else
	{
		dataoffset = 0;			/* 无空值位图的标记 */
		nbytes = ndatabytes + ARR_OVERHEAD_NONULLS(ndims);
	}
	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndims;
	result->dataoffset = dataoffset;
	result->elemtype = element_type;
	memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));
	/* 数据区先 arg1 后 arg2 */
	memcpy(ARR_DATA_PTR(result), dat1, ndatabytes1);
	memcpy(ARR_DATA_PTR(result) + ndatabytes1, dat2, ndatabytes2);
	/* 需要时处理空值位图 */
	if (ARR_HASNULL(result))
	{
		array_bitmap_copy(ARR_NULLBITMAP(result), 0,
						  bitmap1, 0,
						  nitems1);
		array_bitmap_copy(ARR_NULLBITMAP(result), nitems1,
						  bitmap2, 0,
						  nitems2);
	}

	PG_RETURN_ARRAYTYPE_P(result);
}


/*
 * ARRAY_AGG(anynonarray) 聚合函数
 */
Datum
array_agg_transfn(PG_FUNCTION_ARGS)
{
	Oid			arg1_typeid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	MemoryContext aggcontext;
	ArrayBuildState *state;
	Datum		elem;

	if (arg1_typeid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	/*
	 * 注意：我们不需要运行时检查来确认 arg1_typeid
	 * 是否为有效的数组元素类型，因为解析器在解析
	 * 这个多态聚合的输入/结果类型时就已经核实过了。
	 */

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* 因参数为 internal 类型而无法直接调用 */
		elog(ERROR, "array_agg_transfn called in non-aggregate context");
	}

	if (PG_ARGISNULL(0))
		state = initArrayResult(arg1_typeid, aggcontext, false);
	else
		state = (ArrayBuildState *) PG_GETARG_POINTER(0);

	elem = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);

	state = accumArrayResult(state,
							 elem,
							 PG_ARGISNULL(1),
							 arg1_typeid,
							 aggcontext);

	/*
	 * array_agg() 的转换类型被声明为 "internal"，这是
	 * 一种与指针同样大小的按值传递类型。所以我们
	 * 放心地把 ArrayBuildState 指针穿过 nodeAgg.c 的机关。
	 */
	PG_RETURN_POINTER(state);
}

Datum
array_agg_combine(PG_FUNCTION_ARGS)
{
	ArrayBuildState *state1;
	ArrayBuildState *state2;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state1 = PG_ARGISNULL(0) ? NULL : (ArrayBuildState *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (ArrayBuildState *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
	{
		/*
		 * state2 为 NULL 很简单，直接返回 state1，它必然
		 * 在 agg_context
		 */
		if (state1 == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(state1);
	}

	if (state1 == NULL)
	{
		/* 必须把 state2 的数据拷入 agg_context */
		state1 = initArrayResultWithSize(state2->element_type, agg_context,
										 false, state2->alen);

		old_context = MemoryContextSwitchTo(agg_context);

		for (int i = 0; i < state2->nelems; i++)
		{
			if (!state2->dnulls[i])
				state1->dvalues[i] = datumCopy(state2->dvalues[i],
											   state1->typbyval,
											   state1->typlen);
			else
				state1->dvalues[i] = (Datum) 0;
		}

		MemoryContextSwitchTo(old_context);

		memcpy(state1->dnulls, state2->dnulls, sizeof(bool) * state2->nelems);

		state1->nelems = state2->nelems;

		PG_RETURN_POINTER(state1);
	}
	else if (state2->nelems > 0)
	{
		/* 只有当 state2 还有元素时才需要合并两个状态 */
		int			reqsize = state1->nelems + state2->nelems;
		MemoryContext oldContext = MemoryContextSwitchTo(state1->mcontext);

		Assert(state1->element_type == state2->element_type);

		/* 按需扩大 state1 数组 */
		if (state1->alen < reqsize)
		{
			/* 用 2 的幂大小而不是只分配 reqsize */
			state1->alen = pg_nextpower2_32(reqsize);
			state1->dvalues = (Datum *) repalloc(state1->dvalues,
												 state1->alen * sizeof(Datum));
			state1->dnulls = (bool *) repalloc(state1->dnulls,
											   state1->alen * sizeof(bool));
		}

		/* 把 state2 的元素拷到 state1 数组末尾 */
		for (int i = 0; i < state2->nelems; i++)
		{
			if (!state2->dnulls[i])
				state1->dvalues[i + state1->nelems] =
					datumCopy(state2->dvalues[i],
							  state1->typbyval,
							  state1->typlen);
			else
				state1->dvalues[i + state1->nelems] = (Datum) 0;
		}

		memcpy(&state1->dnulls[state1->nelems], state2->dnulls,
			   sizeof(bool) * state2->nelems);

		state1->nelems = reqsize;

		MemoryContextSwitchTo(oldContext);
	}

	PG_RETURN_POINTER(state1);
}

/*
 * array_agg_serialize
 *		ArrayBuildState 序列化为 bytea。
 */
Datum
array_agg_serialize(PG_FUNCTION_ARGS)
{
	ArrayBuildState *state;
	StringInfoData buf;
	bytea	   *result;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = (ArrayBuildState *) PG_GETARG_POINTER(0);

	pq_begintypsend(&buf);

	/*
	 * element_type。把它放在最前面，在反序列化时更方便。
	 */
	pq_sendint32(&buf, state->element_type);

	/*
	 * nelems —— 先发送它，让我们知道 dvalues 与
	 * dnulls 数组该建多大。
	 */
	pq_sendint64(&buf, state->nelems);

	/* alen 可在反序列化期间决定 */

	/* typlen */
	pq_sendint16(&buf, state->typlen);

	/* typbyval */
	pq_sendbyte(&buf, state->typbyval);

	/* typalign */
	pq_sendbyte(&buf, state->typalign);

	/* dnulls */
	pq_sendbytes(&buf, state->dnulls, sizeof(bool) * state->nelems);

	/*
	 * dvalues。与 array_agg_deserialize 约定：当元素
	 * 类型按值传递时，原样传送 Datum 数组，包括
	 * 任何空元素。对按引用类型，必须调用元素类型的
 * send 函数，并跳过空元素（因此空值标志
 * 须先发送）。
	 */
	if (state->typbyval)
		pq_sendbytes(&buf, state->dvalues, sizeof(Datum) * state->nelems);
	else
	{
		SerialIOData *iodata;
		int			i;

		/* 避免对 typsend 函数重复查找目录 */
		iodata = (SerialIOData *) fcinfo->flinfo->fn_extra;
		if (iodata == NULL)
		{
			Oid			typsend;
			bool		typisvarlena;

			iodata = (SerialIOData *)
				MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
								   sizeof(SerialIOData));
			getTypeBinaryOutputInfo(state->element_type, &typsend,
									&typisvarlena);
			fmgr_info_cxt(typsend, &iodata->typsend,
						  fcinfo->flinfo->fn_mcxt);
			fcinfo->flinfo->fn_extra = (void *) iodata;
		}

		for (i = 0; i < state->nelems; i++)
		{
			bytea	   *outputbytes;

			if (state->dnulls[i])
				continue;
			outputbytes = SendFunctionCall(&iodata->typsend,
										   state->dvalues[i]);
			pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	result = pq_endtypsend(&buf);

	PG_RETURN_BYTEA_P(result);
}

Datum
array_agg_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	ArrayBuildState *result;
	StringInfoData buf;
	Oid			element_type;
	int64		nelems;
	const char *temp;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	sstate = PG_GETARG_BYTEA_PP(0);

	/*
	 * 把 bytea 拷贝进 StringInfo，这样我们就能用标准
	 * recv 函数设施来"接收"它。
	 */
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf,
						   VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

	/* element_type */
	element_type = pq_getmsgint(&buf, 4);

	/* nelems */
	nelems = pq_getmsgint64(&buf);

	/* 以所需元素个数创建输出 ArrayBuildState */
	result = initArrayResultWithSize(element_type, CurrentMemoryContext,
									 false, nelems);
	result->nelems = nelems;

	/* typlen */
	result->typlen = pq_getmsgint(&buf, 2);

	/* typbyval */
	result->typbyval = pq_getmsgbyte(&buf);

	/* typalign */
	result->typalign = pq_getmsgbyte(&buf);

	/* dnulls */
	temp = pq_getmsgbytes(&buf, sizeof(bool) * nelems);
	memcpy(result->dnulls, temp, sizeof(bool) * nelems);

	/* dvalues（见 array_agg_serialize 注释） */
	if (result->typbyval)
	{
		temp = pq_getmsgbytes(&buf, sizeof(Datum) * nelems);
		memcpy(result->dvalues, temp, sizeof(Datum) * nelems);
	}
	else
	{
		DeserialIOData *iodata;

		/* 避免对 typreceive 函数重复查找目录 */
		iodata = (DeserialIOData *) fcinfo->flinfo->fn_extra;
		if (iodata == NULL)
		{
			Oid			typreceive;

			iodata = (DeserialIOData *)
				MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
								   sizeof(DeserialIOData));
			getTypeBinaryInputInfo(element_type, &typreceive,
								   &iodata->typioparam);
			fmgr_info_cxt(typreceive, &iodata->typreceive,
						  fcinfo->flinfo->fn_mcxt);
			fcinfo->flinfo->fn_extra = (void *) iodata;
		}

		for (int i = 0; i < nelems; i++)
		{
			int			itemlen;
			StringInfoData elem_buf;
			char		csave;

			if (result->dnulls[i])
			{
				result->dvalues[i] = (Datum) 0;
				continue;
			}

			itemlen = pq_getmsgint(&buf, 4);
			if (itemlen < 0 || itemlen > (buf.len - buf.cursor))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("insufficient data left in message")));

			/*
			 * 与其拷贝数据，不如直接建立一个伪
			 * StringInfo，指向输入缓冲区的正确部分。
			 * 我们假定可以在输入缓冲区上涂写，以维持
			 * StringInfo 末尾有终止空字节这一惯例。
			 */
			elem_buf.data = &buf.data[buf.cursor];
			elem_buf.maxlen = itemlen + 1;
			elem_buf.len = itemlen;
			elem_buf.cursor = 0;

			buf.cursor += itemlen;

			csave = buf.data[buf.cursor];
			buf.data[buf.cursor] = '\0';

			/* 调用该元素的 receiveproc */
			result->dvalues[i] = ReceiveFunctionCall(&iodata->typreceive,
													 &elem_buf,
													 iodata->typioparam,
													 -1);

			buf.data[buf.cursor] = csave;
		}
	}

	pq_getmsgend(&buf);
	pfree(buf.data);

	PG_RETURN_POINTER(result);
}

Datum
array_agg_finalfn(PG_FUNCTION_ARGS)
{
	Datum		result;
	ArrayBuildState *state;
	int			dims[1];
	int			lbs[1];

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ? NULL : (ArrayBuildState *) PG_GETARG_POINTER(0);

	if (state == NULL)
		PG_RETURN_NULL();		/* 若无输入值则返回空 */

	dims[0] = state->nelems;
	lbs[0] = 1;

	/*
	 * 生成结果。我们无法释放 ArrayBuildState，
	 * 因为聚合的最终函数有时会被重新执行。所以，
	 * 在安全时重置 aggcontext 是 nodeAgg.c 的职
	 * 责
	 */
	result = makeMdArrayResult(state, 1, dims, lbs,
							   CurrentMemoryContext,
							   false);

	PG_RETURN_DATUM(result);
}

/*
 * ARRAY_AGG(anyarray) 聚合函数
 */
Datum
array_agg_array_transfn(PG_FUNCTION_ARGS)
{
	Oid			arg1_typeid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	MemoryContext aggcontext;
	ArrayBuildStateArr *state;

	if (arg1_typeid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	/*
	 * 注意：我们不需要运行时检查来确认 arg1_typeid
	 * 是否为有效的数组类型，因为解析器在解析
	 * 这个多态聚合的输入/结果类型时就核实过了。
	 */

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* 因参数为 internal 类型而无法直接调用 */
		elog(ERROR, "array_agg_array_transfn called in non-aggregate context");
	}


	if (PG_ARGISNULL(0))
		state = initArrayResultArr(arg1_typeid, InvalidOid, aggcontext, false);
	else
		state = (ArrayBuildStateArr *) PG_GETARG_POINTER(0);

	state = accumArrayResultArr(state,
								PG_GETARG_DATUM(1),
								PG_ARGISNULL(1),
								arg1_typeid,
								aggcontext);

	/*
	 * array_agg() 的转换类型被声明为 "internal"，这是
	 * 一种与指针同样大小的按值传递类型。所以我们
	 * 放心地把 ArrayBuildStateArr 指针穿过 nodeAgg.c 的机关。
	 */
	PG_RETURN_POINTER(state);
}

Datum
array_agg_array_combine(PG_FUNCTION_ARGS)
{
	ArrayBuildStateArr *state1;
	ArrayBuildStateArr *state2;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state1 = PG_ARGISNULL(0) ? NULL : (ArrayBuildStateArr *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (ArrayBuildStateArr *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
	{
		/*
		 * state2 为 NULL 很简单，直接返回 state1，它必然
		 * 在 agg_context
		 */
		if (state1 == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(state1);
	}

	if (state1 == NULL)
	{
		/* 必须把 state2 的数据拷入 agg_context */
		old_context = MemoryContextSwitchTo(agg_context);

		state1 = initArrayResultArr(state2->array_type, InvalidOid,
									agg_context, false);

		state1->abytes = state2->abytes;
		state1->data = (char *) palloc(state1->abytes);

		if (state2->nullbitmap)
		{
			int			size = (state2->aitems + 7) / 8;

			state1->nullbitmap = (bits8 *) palloc(size);
			memcpy(state1->nullbitmap, state2->nullbitmap, size);
		}

		memcpy(state1->data, state2->data, state2->nbytes);
		state1->nbytes = state2->nbytes;
		state1->aitems = state2->aitems;
		state1->nitems = state2->nitems;
		state1->ndims = state2->ndims;
		memcpy(state1->dims, state2->dims, sizeof(state2->dims));
		memcpy(state1->lbs, state2->lbs, sizeof(state2->lbs));
		state1->array_type = state2->array_type;
		state1->element_type = state2->element_type;

		MemoryContextSwitchTo(old_context);

		PG_RETURN_POINTER(state1);
	}

	/* 只有当 state2 还有条目时才需要合并两个状态 */
	else if (state2->nitems > 0)
	{
		MemoryContext oldContext;
		int			reqsize = state1->nbytes + state2->nbytes;
		int			i;

		/*
		 * 检查两个状态是否彼此兼容。确保使用与
		 * accumArrayResultArr 中列出的相同错误消息，这样
		 * 即使我们没用合并函数，也会显示同样的
		 * 错误。
		 */
		if (state1->ndims != state2->ndims)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("cannot accumulate arrays of different dimensionality")));

		/* 检查维度是否匹配，忽略第一维。 */
		for (i = 1; i < state1->ndims; i++)
		{
			if (state1->dims[i] != state2->dims[i] || state1->lbs[i] != state2->lbs[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot accumulate arrays of different dimensionality")));
		}


		oldContext = MemoryContextSwitchTo(state1->mcontext);

		/*
		 * 如果 state1 的空间不足，就得重新分
		 * 配
		 */
		if (state1->abytes < reqsize)
		{
			/* 用 2 的幂大小而不是只分配 reqsize */
			state1->abytes = pg_nextpower2_32(reqsize);
			state1->data = (char *) repalloc(state1->data, state1->abytes);
		}

		if (state2->nullbitmap)
		{
			int			newnitems = state1->nitems + state2->nitems;

			if (state1->nullbitmap == NULL)
			{
				/*
				 * 首个带空值的输入；须事后追补，把之前
				 * 输入的所有条目标记为非空。
				 */
				state1->aitems = pg_nextpower2_32(Max(256, newnitems + 1));
				state1->nullbitmap = (bits8 *) palloc((state1->aitems + 7) / 8);
				array_bitmap_copy(state1->nullbitmap, 0,
								  NULL, 0,
								  state1->nitems);
			}
			else if (newnitems > state1->aitems)
			{
				int			newaitems = state1->aitems + state2->aitems;

				state1->aitems = pg_nextpower2_32(newaitems);
				state1->nullbitmap = (bits8 *)
					repalloc(state1->nullbitmap, (state1->aitems + 7) / 8);
			}
			array_bitmap_copy(state1->nullbitmap, state1->nitems,
							  state2->nullbitmap, 0,
							  state2->nitems);
		}

		memcpy(state1->data + state1->nbytes, state2->data, state2->nbytes);
		state1->nbytes += state2->nbytes;
		state1->nitems += state2->nitems;

		state1->dims[0] += state2->dims[0];
		/* 如上所测，其余维度已匹配 */

		Assert(state1->array_type == state2->array_type);
		Assert(state1->element_type == state2->element_type);

		MemoryContextSwitchTo(oldContext);
	}

	PG_RETURN_POINTER(state1);
}

/*
 * array_agg_array_serialize
 *		ArrayBuildStateArr 序列化为 bytea。
 */
Datum
array_agg_array_serialize(PG_FUNCTION_ARGS)
{
	ArrayBuildStateArr *state;
	StringInfoData buf;
	bytea	   *result;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = (ArrayBuildStateArr *) PG_GETARG_POINTER(0);

	pq_begintypsend(&buf);

	/*
	 * element_type。把它放在最前面，在反序列化时
	 * 以便能更早初始化新状态。
	 */
	pq_sendint32(&buf, state->element_type);

	/* array_type */
	pq_sendint32(&buf, state->array_type);

	/* nbytes */
	pq_sendint32(&buf, state->nbytes);

	/* data */
	pq_sendbytes(&buf, state->data, state->nbytes);

	/* abytes */
	pq_sendint32(&buf, state->abytes);

	/* aitems */
	pq_sendint32(&buf, state->aitems);

	/* nullbitmap */
	if (state->nullbitmap)
	{
		Assert(state->aitems > 0);
		pq_sendbytes(&buf, state->nullbitmap, (state->aitems + 7) / 8);
	}

	/* nitems */
	pq_sendint32(&buf, state->nitems);

	/* ndims */
	pq_sendint32(&buf, state->ndims);

	/* dims：XXX 要只发送 ndims 个吗？ */
	pq_sendbytes(&buf, state->dims, sizeof(state->dims));

	/* lbs */
	pq_sendbytes(&buf, state->lbs, sizeof(state->lbs));

	result = pq_endtypsend(&buf);

	PG_RETURN_BYTEA_P(result);
}

Datum
array_agg_array_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	ArrayBuildStateArr *result;
	StringInfoData buf;
	Oid			element_type;
	Oid			array_type;
	int			nbytes;
	const char *temp;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	sstate = PG_GETARG_BYTEA_PP(0);

	/*
	 * 把 bytea 拷贝进 StringInfo，这样我们就能用标准
	 * recv 函数设施来"接收"它。
	 */
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf,
						   VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

	/* element_type */
	element_type = pq_getmsgint(&buf, 4);

	/* array_type */
	array_type = pq_getmsgint(&buf, 4);

	/* nbytes */
	nbytes = pq_getmsgint(&buf, 4);

	result = initArrayResultArr(array_type, element_type,
								CurrentMemoryContext, false);

	result->abytes = 1024;
	while (result->abytes < nbytes)
		result->abytes *= 2;

	result->data = (char *) palloc(result->abytes);

	/* data */
	temp = pq_getmsgbytes(&buf, nbytes);
	memcpy(result->data, temp, nbytes);
	result->nbytes = nbytes;

	/* abytes */
	result->abytes = pq_getmsgint(&buf, 4);

	/* aitems：可为 0 */
	result->aitems = pq_getmsgint(&buf, 4);

	/* nullbitmap */
	if (result->aitems > 0)
	{
		int			size = (result->aitems + 7) / 8;

		result->nullbitmap = (bits8 *) palloc(size);
		temp = pq_getmsgbytes(&buf, size);
		memcpy(result->nullbitmap, temp, size);
	}
	else
		result->nullbitmap = NULL;

	/* nitems */
	result->nitems = pq_getmsgint(&buf, 4);

	/* ndims */
	result->ndims = pq_getmsgint(&buf, 4);

	/* dims */
	temp = pq_getmsgbytes(&buf, sizeof(result->dims));
	memcpy(result->dims, temp, sizeof(result->dims));

	/* lbs */
	temp = pq_getmsgbytes(&buf, sizeof(result->lbs));
	memcpy(result->lbs, temp, sizeof(result->lbs));

	pq_getmsgend(&buf);
	pfree(buf.data);

	PG_RETURN_POINTER(result);
}

Datum
array_agg_array_finalfn(PG_FUNCTION_ARGS)
{
	Datum		result;
	ArrayBuildStateArr *state;

	/* 因参数为 internal 类型而无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ? NULL : (ArrayBuildStateArr *) PG_GETARG_POINTER(0);

	if (state == NULL)
		PG_RETURN_NULL();		/* 若无输入值则返回空 */

	/*
	 * 生成结果。我们无法释放 ArrayBuildStateArr，
	 * 因为聚合的最终函数有时会被重新执行。所以，
	 * 在安全时重置 aggcontext 是 nodeAgg.c 的职
	 * 责
	 */
	result = makeArrayResultArr(state, CurrentMemoryContext, false);

	PG_RETURN_DATUM(result);
}

/*-----------------------------------------------------------------------------
 * array_position, array_position_start :
 *			返回值在数组中的偏移量。
 *
 * 比较使用 IS NOT DISTINCT FROM 语义。找不到值时
 * 返回 NULL。
 *-----------------------------------------------------------------------------
 */
Datum
array_position(PG_FUNCTION_ARGS)
{
	return array_position_common(fcinfo);
}

Datum
array_position_start(PG_FUNCTION_ARGS)
{
	return array_position_common(fcinfo);
}

/*
 * array_position_common
 *		array_position 与 array_position_start 的公共代码
 *
 * 分开成包装函数是为了 opr_sanity 回归测试；
 * 它们不是严格的，所以必须显式测试空输入。
 */
static Datum
array_position_common(FunctionCallInfo fcinfo)
{
	ArrayType  *array;
	Oid			collation = PG_GET_COLLATION();
	Oid			element_type;
	Datum		searched_element,
				value;
	bool		isnull;
	int			position,
				position_min;
	bool		found = false;
	TypeCacheEntry *typentry;
	ArrayMetaState *my_extra;
	bool		null_search;
	ArrayIterator array_iterator;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	array = PG_GETARG_ARRAYTYPE_P(0);

	/*
	 * 我们拒绝在多维数组中搜索元素，因为
	 * 没有好的办法来报告元素在数组中的位置。
	 */
	if (ARR_NDIM(array) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("searching for elements in multidimensional arrays is not supported")));

	/* 不过在空数组中搜索是有明确定义的：总是失败 */
	if (ARR_NDIM(array) < 1)
		PG_RETURN_NULL();

	if (PG_ARGISNULL(1))
	{
		/* 数组没有空值时快速返回 */
		if (!array_contains_nulls(array))
			PG_RETURN_NULL();
		searched_element = (Datum) 0;
		null_search = true;
	}
	else
	{
		searched_element = PG_GETARG_DATUM(1);
		null_search = false;
	}

	element_type = ARR_ELEMTYPE(array);
	position = (ARR_LBOUND(array))[0] - 1;

	/* 确定从哪里开始 */
	if (PG_NARGS() == 3)
	{
		if (PG_ARGISNULL(2))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("initial position must not be null")));

		position_min = PG_GETARG_INT32(2);
	}
	else
		position_min = (ARR_LBOUND(array))[0];

	/*
	 * 我们安排为 array_create_iterator 查找类型信息
	 * 每次调用序列只做一次，假定元素类型不会
	 * 变
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		get_typlenbyvalalign(element_type,
							 &my_extra->typlen,
							 &my_extra->typbyval,
							 &my_extra->typalign);

		typentry = lookup_type_cache(element_type, TYPECACHE_EQ_OPR_FINFO);

		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an equality operator for type %s",
							format_type_be(element_type))));

		my_extra->element_type = element_type;
		fmgr_info_cxt(typentry->eq_opr_finfo.fn_oid, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
	}

	/* 逐个检查数组元素直到找到匹配。 */
	array_iterator = array_create_iterator(array, 0, my_extra);
	while (array_iterate(array_iterator, &value, &isnull))
	{
		position++;

		/* 若调用者要求则跳过起始元素 */
		if (position < position_min)
			continue;

		/*
		 * 如果元素为空就不能查看其值；但如果搜索的
		 * 是空值，那么就算命中并完成。
		 */
		if (isnull || null_search)
		{
			if (isnull && null_search)
			{
				found = true;
				break;
			}
			else
				continue;
		}

		/* 非空值，所以运行算子 */
		if (DatumGetBool(FunctionCall2Coll(&my_extra->proc, collation,
										   searched_element, value)))
		{
			found = true;
			break;
		}
	}

	array_free_iterator(array_iterator);

	/* 避免被给入 TOAST 输入时泄漏内存 */
	PG_FREE_IF_COPY(array, 0);

	if (!found)
		PG_RETURN_NULL();

	PG_RETURN_INT32(position);
}

/*-----------------------------------------------------------------------------
 * array_positions :
 *			返回值在数组中各位置的数组。
 *
 * 比较使用 IS NOT DISTINCT FROM 语义。当输入数组为
 * NULL 时返回 NULL。数组中找不到值时返回空
 * 数组。
 *
 * 它不是严格的，所以必须显式测试空输入。
 *-----------------------------------------------------------------------------
 */
Datum
array_positions(PG_FUNCTION_ARGS)
{
	ArrayType  *array;
	Oid			collation = PG_GET_COLLATION();
	Oid			element_type;
	Datum		searched_element,
				value;
	bool		isnull;
	int			position;
	TypeCacheEntry *typentry;
	ArrayMetaState *my_extra;
	bool		null_search;
	ArrayIterator array_iterator;
	ArrayBuildState *astate = NULL;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	array = PG_GETARG_ARRAYTYPE_P(0);

	/*
	 * 我们拒绝在多维数组中搜索元素，因为
	 * 没有好的办法来报告元素在数组中的位置。
	 */
	if (ARR_NDIM(array) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("searching for elements in multidimensional arrays is not supported")));

	astate = initArrayResult(INT4OID, CurrentMemoryContext, false);

	/* 不过在空数组中搜索是有明确定义的：总是失败 */
	if (ARR_NDIM(array) < 1)
		PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));

	if (PG_ARGISNULL(1))
	{
		/* 数组没有空值时快速返回 */
		if (!array_contains_nulls(array))
			PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
		searched_element = (Datum) 0;
		null_search = true;
	}
	else
	{
		searched_element = PG_GETARG_DATUM(1);
		null_search = false;
	}

	element_type = ARR_ELEMTYPE(array);
	position = (ARR_LBOUND(array))[0] - 1;

	/*
	 * 我们安排为 array_create_iterator 查找类型信息
	 * 每次调用序列只做一次，假定元素类型不会
	 * 变
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		get_typlenbyvalalign(element_type,
							 &my_extra->typlen,
							 &my_extra->typbyval,
							 &my_extra->typalign);

		typentry = lookup_type_cache(element_type, TYPECACHE_EQ_OPR_FINFO);

		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an equality operator for type %s",
							format_type_be(element_type))));

		my_extra->element_type = element_type;
		fmgr_info_cxt(typentry->eq_opr_finfo.fn_oid, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
	}

	/*
	 * 每当元素与给定元素匹配，就累积该数组位
	 * 置。
	 */
	array_iterator = array_create_iterator(array, 0, my_extra);
	while (array_iterate(array_iterator, &value, &isnull))
	{
		position += 1;

		/*
		 * 如果元素为空就不能查看其值；但如果搜索的
		 * 是空值，就算命中。
		 */
		if (isnull || null_search)
		{
			if (isnull && null_search)
				astate =
					accumArrayResult(astate, Int32GetDatum(position), false,
									 INT4OID, CurrentMemoryContext);

			continue;
		}

		/* 非空值，所以运行算子 */
		if (DatumGetBool(FunctionCall2Coll(&my_extra->proc, collation,
										   searched_element, value)))
			astate =
				accumArrayResult(astate, Int32GetDatum(position), false,
								 INT4OID, CurrentMemoryContext);
	}

	array_free_iterator(array_iterator);

	/* 避免被给入 TOAST 输入时泄漏内存 */
	PG_FREE_IF_COPY(array, 0);

	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}

/*
 * array_shuffle_n
 *		返回数组中随机选 n 项的拷贝。
 *
 * 项数不得超过数组第一维的大小。若 keep_lb 为真，
 * 则保留第一维的下界，否则设为 1。无论如何，
 * 低阶维度都会保留。
 *
 * 注意：更干净的做法是从系统目录查找
 * elmlen/elmbval/elmalign 信息（只给 elmtyp 时）。
 * 但调用者更适合在多次调用间缓存该信息。
 */
static ArrayType *
array_shuffle_n(ArrayType *array, int n, bool keep_lb,
				Oid elmtyp, TypeCacheEntry *typentry)
{
	ArrayType  *result;
	int			ndim,
			   *dims,
			   *lbs,
				nelm,
				nitem,
				rdims[MAXDIM],
				rlbs[MAXDIM];
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elms,
			   *ielms;
	bool	   *nuls,
			   *inuls;

	ndim = ARR_NDIM(array);
	dims = ARR_DIMS(array);
	lbs = ARR_LBOUND(array);

	elmlen = typentry->typlen;
	elmbyval = typentry->typbyval;
	elmalign = typentry->typalign;

	/* 若目标数组为空则快速退出 */
	if (ndim < 1 || dims[0] < 1 || n < 1)
		return construct_empty_array(elmtyp);

	deconstruct_array(array, elmtyp, elmlen, elmbyval, elmalign,
					  &elms, &nuls, &nelm);

	nitem = dims[0];			/* 条目总数 */
	nelm /= nitem;				/* 每条目的元素个数 */

	Assert(n <= nitem);			/* 调用者错误 */

	/*
	 * 用 Fisher-Yates 算法洗牌数组。扫描数组并在
	 * 每轮迭代中把当前项（ielms 起的 nelm 个 datum）
	 * 与随机选中的后一项（jelms 起的 nelm 个 datum）
	 * 交换。完成 n 轮后即可停止；前 n 项就是结果。
	 */
	ielms = elms;
	inuls = nuls;
	for (int i = 0; i < n; i++)
	{
		int			j = (int) pg_prng_uint64_range(&pg_global_prng_state, i, nitem - 1) * nelm;
		Datum	   *jelms = elms + j;
		bool	   *jnuls = nuls + j;

		/* 交换第 i 与 j 项；ielms/inuls 前进到下一项 */
		for (int k = 0; k < nelm; k++)
		{
			Datum		elm = *ielms;
			bool		nul = *inuls;

			*ielms++ = *jelms;
			*inuls++ = *jnuls;
			*jelms++ = elm;
			*jnuls++ = nul;
		}
	}

	/* 设置结果的维度 */
	memcpy(rdims, dims, ndim * sizeof(int));
	memcpy(rlbs, lbs, ndim * sizeof(int));
	rdims[0] = n;
	if (!keep_lb)
		rlbs[0] = 1;

	result = construct_md_array(elms, nuls, ndim, rdims, rlbs,
								elmtyp, elmlen, elmbyval, elmalign);

	pfree(elms);
	pfree(nuls);

	return result;
}

/*
 * array_shuffle
 *
 * 返回一个与输入数组维度相同的数组，其第一维
 * 元素处于随机顺序。
 */
Datum
array_shuffle(PG_FUNCTION_ARGS)
{
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *result;
	Oid			elmtyp;
	TypeCacheEntry *typentry;

	/*
	 * 空数组或条目少于两个的数组，洗牌
	 * 没意义
	 */
	if (ARR_NDIM(array) < 1 || ARR_DIMS(array)[0] < 2)
		PG_RETURN_ARRAYTYPE_P(array);

	elmtyp = ARR_ELEMTYPE(array);
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL || typentry->type_id != elmtyp)
	{
		typentry = lookup_type_cache(elmtyp, 0);
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}

	result = array_shuffle_n(array, ARR_DIMS(array)[0], true, elmtyp, typentry);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * array_sample
 *
 * 返回一个数组：从输入数组随机选出 n 个第一
 * 维元素。
 */
Datum
array_sample(PG_FUNCTION_ARGS)
{
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(0);
	int			n = PG_GETARG_INT32(1);
	ArrayType  *result;
	Oid			elmtyp;
	TypeCacheEntry *typentry;
	int			nitem;

	nitem = (ARR_NDIM(array) < 1) ? 0 : ARR_DIMS(array)[0];

	if (n < 0 || n > nitem)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("sample size must be between 0 and %d", nitem)));

	elmtyp = ARR_ELEMTYPE(array);
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL || typentry->type_id != elmtyp)
	{
		typentry = lookup_type_cache(elmtyp, 0);
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}

	result = array_shuffle_n(array, n, false, elmtyp, typentry);

	PG_RETURN_ARRAYTYPE_P(result);
}
