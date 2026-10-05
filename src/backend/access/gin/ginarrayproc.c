/*-------------------------------------------------------------------------
 *
 * ginarrayproc.c
 *	  GIN 对任意数组建索引的支持函数
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gin/ginarrayproc.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


#define GinOverlapStrategy		1
#define GinContainsStrategy		2
#define GinContainedStrategy	3
#define GinEqualStrategy		4


/*
 * extractValue 支持函数
 */
Datum
ginarrayextract(PG_FUNCTION_ARGS)
{
	/* 复制数组输入，确保它在使用期间不会消失 */
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P_COPY(0);
	int32	   *nkeys = (int32 *) PG_GETARG_POINTER(1);
	bool	  **nullFlags = (bool **) PG_GETARG_POINTER(2);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &elmlen, &elmbyval, &elmalign);

	deconstruct_array(array,
					  ARR_ELEMTYPE(array),
					  elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	*nkeys = nelems;
	*nullFlags = nulls;

	/* 不应释放数组，elems[i] 指向其中 */
	PG_RETURN_POINTER(elems);
}

/*
 * 从前 ginarrayextract 只有两个参数。现在它有三个，
 * 但仍需要一条有两个参数的 pg_proc 条目，以便重新加载
 * 9.1 之前 contrib/intarray 操作符类的声明。这个兼容
 * 函数最终应当去掉。
 */
Datum
ginarrayextract_2args(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() < 3)			/* 不应发生 */
		elog(ERROR, "ginarrayextract requires three arguments");
	return ginarrayextract(fcinfo);
}

/*
 * extractQuery 支持函数
 */
Datum
ginqueryarrayextract(PG_FUNCTION_ARGS)
{
	/* 复制数组输入，确保它在使用期间不会消失 */
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P_COPY(0);
	int32	   *nkeys = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);

	/* 未使用：bool   **pmatch = (bool **) PG_GETARG_POINTER(3); */
	/* 未使用：Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	  **nullFlags = (bool **) PG_GETARG_POINTER(5);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &elmlen, &elmbyval, &elmalign);

	deconstruct_array(array,
					  ARR_ELEMTYPE(array),
					  elmlen, elmbyval, elmalign,
					  &elems, &nulls, &nelems);

	*nkeys = nelems;
	*nullFlags = nulls;

	switch (strategy)
	{
		case GinOverlapStrategy:
			*searchMode = GIN_SEARCH_MODE_DEFAULT;
			break;
		case GinContainsStrategy:
			if (nelems > 0)
				*searchMode = GIN_SEARCH_MODE_DEFAULT;
			else				/* 一切都包含空集 */
				*searchMode = GIN_SEARCH_MODE_ALL;
			break;
		case GinContainedStrategy:
			/* 空集被一切包含 */
			*searchMode = GIN_SEARCH_MODE_INCLUDE_EMPTY;
			break;
		case GinEqualStrategy:
			if (nelems > 0)
				*searchMode = GIN_SEARCH_MODE_DEFAULT;
			else
				*searchMode = GIN_SEARCH_MODE_INCLUDE_EMPTY;
			break;
		default:
			elog(ERROR, "ginqueryarrayextract: unknown strategy number: %d",
				 strategy);
	}

	/* 不应释放数组，elems[i] 指向其中 */
	PG_RETURN_POINTER(elems);
}

/*
 * consistent 支持函数
 */
Datum
ginarrayconsistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* 未使用：ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* 未使用：Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);

	/* 未使用：Datum	   *queryKeys = (Datum *) PG_GETARG_POINTER(6); */
	bool	   *nullFlags = (bool *) PG_GETARG_POINTER(7);
	bool		res;
	int32		i;

	switch (strategy)
	{
		case GinOverlapStrategy:
			/* 结果不是有损的 */
			*recheck = false;
			/* 必须至少有一个非空元素匹配 */
			res = false;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] && !nullFlags[i])
				{
					res = true;
					break;
				}
			}
			break;
		case GinContainsStrategy:
			/* 结果不是有损的 */
			*recheck = false;
			/* check[] 中的元素必须全部为真，且没有空值 */
			res = true;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i] || nullFlags[i])
				{
					res = false;
					break;
				}
			}
			break;
		case GinContainedStrategy:
			/* 需要重新检查 */
			*recheck = true;
			/* 这里做不了其他有用的事 */
			res = true;
			break;
		case GinEqualStrategy:
			/* 需要重新检查 */
			*recheck = true;

			/*
			 * check[] 中的元素必须全部为真；这里不因空值而排除。
			 * 这是因为 array_contain_compare 与 array_eq
			 * 对空值的处理并不相同 ...
			 */
			res = true;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i])
				{
					res = false;
					break;
				}
			}
			break;
		default:
			elog(ERROR, "ginarrayconsistent: unknown strategy number: %d",
				 strategy);
			res = false;
	}

	PG_RETURN_BOOL(res);
}

/*
 * triconsistent 支持函数
 */
Datum
ginarraytriconsistent(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* 未使用：ArrayType  *query = PG_GETARG_ARRAYTYPE_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* 未使用：Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	/* 未使用：Datum	   *queryKeys = (Datum *) PG_GETARG_POINTER(5); */
	bool	   *nullFlags = (bool *) PG_GETARG_POINTER(6);
	GinTernaryValue res;
	int32		i;

	switch (strategy)
	{
		case GinOverlapStrategy:
			/* 必须至少有一个非空元素匹配 */
			res = GIN_FALSE;
			for (i = 0; i < nkeys; i++)
			{
				if (!nullFlags[i])
				{
					if (check[i] == GIN_TRUE)
					{
						res = GIN_TRUE;
						break;
					}
					else if (check[i] == GIN_MAYBE && res == GIN_FALSE)
					{
						res = GIN_MAYBE;
					}
				}
			}
			break;
		case GinContainsStrategy:
			/* check[] 中的元素必须全部为真，且没有空值 */
			res = GIN_TRUE;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] == GIN_FALSE || nullFlags[i])
				{
					res = GIN_FALSE;
					break;
				}
				if (check[i] == GIN_MAYBE)
				{
					res = GIN_MAYBE;
				}
			}
			break;
		case GinContainedStrategy:
			/* 这里做不了其他有用的事 */
			res = GIN_MAYBE;
			break;
		case GinEqualStrategy:

			/*
			 * check[] 中的元素必须全部为真；这里不因空值而排除。
			 * 这是因为 array_contain_compare 与 array_eq
			 * 对空值的处理并不相同 ...
			 */
			res = GIN_MAYBE;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] == GIN_FALSE)
				{
					res = GIN_FALSE;
					break;
				}
			}
			break;
		default:
			elog(ERROR, "ginarrayconsistent: unknown strategy number: %d",
				 strategy);
			res = false;
	}

	PG_RETURN_GIN_TERNARY_VALUE(res);
}
