/*-------------------------------------------------------------------------
 *
 * arrayfuncs.c
 *	  数组的支持函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/arrayfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"


/*
 * GUC 参数
 */
bool		Array_nulls = true;

/*
 * 局部定义
 */
#define ASSGN	 "="

#define AARR_FREE_IF_COPY(array,n) \
	do { \
		if (!VARATT_IS_EXPANDED_HEADER(array)) \
			PG_FREE_IF_COPY(array, n); \
	} while (0)

typedef enum
{
	ARRAY_NO_LEVEL,
	ARRAY_LEVEL_STARTED,
	ARRAY_ELEM_STARTED,
	ARRAY_ELEM_COMPLETED,
	ARRAY_QUOTED_ELEM_STARTED,
	ARRAY_QUOTED_ELEM_COMPLETED,
	ARRAY_ELEM_DELIMITED,
	ARRAY_LEVEL_COMPLETED,
	ARRAY_LEVEL_DELIMITED
} ArrayParseState;

/* array_iterate() 的工作状态 */
typedef struct ArrayIteratorData
{
	/* 数组的基本信息，在 array_create_iterator() 期间设置 */
	ArrayType  *arr;			/* 我们正在遍历的数组 */
	bits8	   *nullbitmap;		/* 空值位图，若有 */
	int			nitems;			/* 数组中元素的总数 */
	int16		typlen;			/* 元素类型长度 */
	bool		typbyval;		/* 元素类型的 byval 属性 */
	char		typalign;		/* 元素类型的 align 属性 */

	/* 所请求切片大小的信息 */
	int			slice_ndim;		/* 切片维度，不做切片时为 0 */
	int			slice_len;		/* 每个切片的元素数 */
	int		   *slice_dims;		/* 切片维度表 */
	int		   *slice_lbound;	/* 切片下界表 */
	Datum	   *slice_values;	/* 长为 slice_len 的工作区 */
	bool	   *slice_nulls;	/* 长为 slice_len 的工作区 */

	/* 当前位置信息，每轮迭代都会更新 */
	char	   *data_ptr;		/* 我们在数组中的当前位置 */
	int			current_item;	/* 数组中当前所处元素号 */
}			ArrayIteratorData;

static bool array_isspace(char ch);
static int	ArrayCount(const char *str, int *dim, char typdelim,
					   Node *escontext);
static bool ReadArrayStr(char *arrayStr, const char *origStr,
						 int nitems, int ndim, int *dim,
						 FmgrInfo *inputproc, Oid typioparam, int32 typmod,
						 char typdelim,
						 int typlen, bool typbyval, char typalign,
						 Datum *values, bool *nulls,
						 bool *hasnulls, int32 *nbytes, Node *escontext);
static void ReadArrayBinary(StringInfo buf, int nitems,
							FmgrInfo *receiveproc, Oid typioparam, int32 typmod,
							int typlen, bool typbyval, char typalign,
							Datum *values, bool *nulls,
							bool *hasnulls, int32 *nbytes);
static Datum array_get_element_expanded(Datum arraydatum,
										int nSubscripts, int *indx,
										int arraytyplen,
										int elmlen, bool elmbyval, char elmalign,
										bool *isNull);
static Datum array_set_element_expanded(Datum arraydatum,
										int nSubscripts, int *indx,
										Datum dataValue, bool isNull,
										int arraytyplen,
										int elmlen, bool elmbyval, char elmalign);
static bool array_get_isnull(const bits8 *nullbitmap, int offset);
static void array_set_isnull(bits8 *nullbitmap, int offset, bool isNull);
static Datum ArrayCast(char *value, bool byval, int len);
static int	ArrayCastAndSet(Datum src,
							int typlen, bool typbyval, char typalign,
							char *dest);
static char *array_seek(char *ptr, int offset, bits8 *nullbitmap, int nitems,
						int typlen, bool typbyval, char typalign);
static int	array_nelems_size(char *ptr, int offset, bits8 *nullbitmap,
							  int nitems, int typlen, bool typbyval, char typalign);
static int	array_copy(char *destptr, int nitems,
					   char *srcptr, int offset, bits8 *nullbitmap,
					   int typlen, bool typbyval, char typalign);
static int	array_slice_size(char *arraydataptr, bits8 *arraynullsptr,
							 int ndim, int *dim, int *lb,
							 int *st, int *endp,
							 int typlen, bool typbyval, char typalign);
static void array_extract_slice(ArrayType *newarray,
								int ndim, int *dim, int *lb,
								char *arraydataptr, bits8 *arraynullsptr,
								int *st, int *endp,
								int typlen, bool typbyval, char typalign);
static void array_insert_slice(ArrayType *destArray, ArrayType *origArray,
							   ArrayType *srcArray,
							   int ndim, int *dim, int *lb,
							   int *st, int *endp,
							   int typlen, bool typbyval, char typalign);
static int	array_cmp(FunctionCallInfo fcinfo);
static ArrayType *create_array_envelope(int ndims, int *dimv, int *lbsv, int nbytes,
										Oid elmtype, int dataoffset);
static ArrayType *array_fill_internal(ArrayType *dims, ArrayType *lbs,
									  Datum value, bool isnull, Oid elmtype,
									  FunctionCallInfo fcinfo);
static ArrayType *array_replace_internal(ArrayType *array,
										 Datum search, bool search_isnull,
										 Datum replace, bool replace_isnull,
										 bool remove, Oid collation,
										 FunctionCallInfo fcinfo);
static int	width_bucket_array_float8(Datum operand, ArrayType *thresholds);
static int	width_bucket_array_fixed(Datum operand,
									 ArrayType *thresholds,
									 Oid collation,
									 TypeCacheEntry *typentry);
static int	width_bucket_array_variable(Datum operand,
										ArrayType *thresholds,
										Oid collation,
										TypeCacheEntry *typentry);


/*
 * array_in :
 *		  把数组从 "string" 中的外部格式转换为
 *		  其内部格式。
 *
 * 返回值 :
 *		  输入数组的内部表示形式
 */
Datum
array_in(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0);	/* 外部形式 */
	Oid			element_type = PG_GETARG_OID(1);	/* 数组的元素
													 * 类型 */
	int32		typmod = PG_GETARG_INT32(2);	/* 数组元素的 typmod */
	Node	   *escontext = fcinfo->context;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	char	   *string_save,
			   *p;
	int			i,
				nitems;
	Datum	   *dataPtr;
	bool	   *nullsPtr;
	bool		hasnulls;
	int32		nbytes;
	int32		dataoffset;
	ArrayType  *retval;
	int			ndim,
				dim[MAXDIM],
				lBound[MAXDIM];
	ArrayMetaState *my_extra;

	/*
	 * 我们安排对元素类型（包括其输入转换过
	 * 程）的查找每轮调用只做一次，前提是元素
	 * 类型不在中途被更换。
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
		/*
		 * 获取元素类型的信息，包括其输入转换过程
		 */
		get_type_io_data(element_type, IOFunc_input,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typdelim = my_extra->typdelim;
	typioparam = my_extra->typioparam;

	/* 制作输入的可修改副本 */
	string_save = pstrdup(string);

	/*
	 * 若输入串以维度信息开头，则读取并使用之。
	 * 否则，我们要求输入为花括号风格，且要预
	 * 扫描输入以确定维度。
	 *
	 * 维度信息的形态是一个或多个 [n] 或 [m:n] 项。
	 * 外层循环每维度项迭代一次。
	 */
	p = string_save;
	ndim = 0;
	for (;;)
	{
		char	   *q;
		int			ub;

		/*
		 * 注：目前允许在维度项之间有空白，但在
		 * 项内不许。
		 */
		while (array_isspace(*p))
			p++;
		if (*p != '[')
			break;				/* 没有更多维度项 */
		p++;
		if (ndim >= MAXDIM)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
							ndim + 1, MAXDIM)));

		for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++)
			 /* skip */ ;
		if (q == p)				/* 无数字 */
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("\"[\" must introduce explicitly-specified array dimensions.")));

		if (*q == ':')
		{
			/* [m:n] 格式 */
			*q = '\0';
			lBound[ndim] = atoi(p);
			p = q + 1;
			for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++)
				 /* skip */ ;
			if (q == p)			/* 无数字 */
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed array literal: \"%s\"", string),
						 errdetail("Missing array dimension value.")));
		}
		else
		{
			/* [n] 格式 */
			lBound[ndim] = 1;
		}
		if (*q != ']')
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Missing \"%s\" after array dimensions.",
							   "]")));

		*q = '\0';
		ub = atoi(p);
		p = q + 1;
		if (ub < lBound[ndim])
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("upper bound cannot be less than lower bound")));

		dim[ndim] = ub - lBound[ndim] + 1;
		ndim++;
	}

	if (ndim == 0)
	{
		/* 无数组维度信息，故从花括号结构推断 */
		if (*p != '{')
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Array value must start with \"{\" or dimension information.")));
		ndim = ArrayCount(p, dim, typdelim, escontext);
		if (ndim < 0)
			PG_RETURN_NULL();
		for (i = 0; i < ndim; i++)
			lBound[i] = 1;
	}
	else
	{
		int			ndim_braces,
					dim_braces[MAXDIM];

		/* 若给定了数组维度，则期待 '=' 操作符 */
		if (strncmp(p, ASSGN, strlen(ASSGN)) != 0)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Missing \"%s\" after array dimensions.",
							   ASSGN)));
		p += strlen(ASSGN);
		while (array_isspace(*p))
			p++;

		/*
		 * 从花括号结构推断维度 —— 它必须与所给值
		 * 一致。
		 */
		if (*p != '{')
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Array contents must start with \"{\".")));
		ndim_braces = ArrayCount(p, dim_braces, typdelim, escontext);
		if (ndim_braces < 0)
			PG_RETURN_NULL();
		if (ndim_braces != ndim)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Specified array dimensions do not match array contents.")));
		for (i = 0; i < ndim; ++i)
		{
			if (dim[i] != dim_braces[i])
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed array literal: \"%s\"", string),
						 errdetail("Specified array dimensions do not match array contents.")));
		}
	}

#ifdef ARRAYDEBUG
	printf("array_in- ndim %d (", ndim);
	for (i = 0; i < ndim; i++)
	{
		printf(" %d", dim[i]);
	};
	printf(") for %s\n", string);
#endif

	/* 此处检查数组维度是否溢出 */
	nitems = ArrayGetNItemsSafe(ndim, dim, escontext);
	if (nitems < 0)
		PG_RETURN_NULL();
	if (!ArrayCheckBoundsSafe(ndim, dim, lBound, escontext))
		PG_RETURN_NULL();

	/* 空数组 */
	if (nitems == 0)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(element_type));

	dataPtr = (Datum *) palloc(nitems * sizeof(Datum));
	nullsPtr = (bool *) palloc(nitems * sizeof(bool));
	if (!ReadArrayStr(p, string,
					  nitems, ndim, dim,
					  &my_extra->proc, typioparam, typmod,
					  typdelim,
					  typlen, typbyval, typalign,
					  dataPtr, nullsPtr,
					  &hasnulls, &nbytes, escontext))
		PG_RETURN_NULL();
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* 无空值位图的标记 */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	retval = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(retval, nbytes);
	retval->ndim = ndim;
	retval->dataoffset = dataoffset;

	/*
	 * 这来自数组的 pg_type.typelem（它指向基础
	 * 数据类型的 pg_type.oid），把系统 oid 存入用户表。
	 * 该 oid 必须由二进制升级保留。
	 */
	retval->elemtype = element_type;
	memcpy(ARR_DIMS(retval), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(retval), lBound, ndim * sizeof(int));

	CopyArrayEls(retval,
				 dataPtr, nullsPtr, nitems,
				 typlen, typbyval, typalign,
				 true);

	pfree(dataPtr);
	pfree(nullsPtr);
	pfree(string_save);

	PG_RETURN_ARRAYTYPE_P(retval);
}

/*
 * array_isspace() --- 非区域相关的 isspace()
 *
 * 以前我们用 isspace() 解析数组值，但这会产
 * 生不良结果：数组值可能因区域设置不同而
 * 被不同地静默解读。现在我们改为硬编码
 * isspace() 的传统 ASCII 定义。
 */
static bool
array_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\v' ||
		ch == '\f')
		return true;
	return false;
}

/*
 * ArrayCount
 *	 确定数组串的维度。
 *
 * 作为函数结果返回维度数。各轴长度返回在
 * dim[] 中，其大小必须为 MAXDIM。
 *
 * 若检测到错误，则填充 *escontext 错误详情并返回 -1
 * （除非未提供 escontext，此时错误将被抛出）。
 */
static int
ArrayCount(const char *str, int *dim, char typdelim, Node *escontext)
{
	int			nest_level = 0,
				i;
	int			ndim = 1,
				temp[MAXDIM],
				nelems[MAXDIM],
				nelems_last[MAXDIM];
	bool		in_quotes = false;
	bool		eoArray = false;
	bool		empty_array = true;
	const char *ptr;
	ArrayParseState parse_state = ARRAY_NO_LEVEL;

	for (i = 0; i < MAXDIM; ++i)
	{
		temp[i] = dim[i] = nelems_last[i] = 0;
		nelems[i] = 1;
	}

	ptr = str;
	while (!eoArray)
	{
		bool		itemdone = false;

		while (!itemdone)
		{
			if (parse_state == ARRAY_ELEM_STARTED ||
				parse_state == ARRAY_QUOTED_ELEM_STARTED)
				empty_array = false;

			switch (*ptr)
			{
				case '\0':
					/* 报告串提前结束 */
					ereturn(escontext, -1,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed array literal: \"%s\"", str),
							 errdetail("Unexpected end of input.")));
				case '\\':

					/*
					 * 转义必须出现在层级开始、元素开始或元
					 * 素分隔符之后。无论如何此时必已越过
					 * 元素起点。
					 */
					if (parse_state != ARRAY_LEVEL_STARTED &&
						parse_state != ARRAY_ELEM_STARTED &&
						parse_state != ARRAY_QUOTED_ELEM_STARTED &&
						parse_state != ARRAY_ELEM_DELIMITED)
						ereturn(escontext, -1,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"", str),
								 errdetail("Unexpected \"%c\" character.",
										   '\\')));
					if (parse_state != ARRAY_QUOTED_ELEM_STARTED)
						parse_state = ARRAY_ELEM_STARTED;
					/* 跳过被转义的字符 */
					if (*(ptr + 1))
						ptr++;
					else
						ereturn(escontext, -1,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"", str),
								 errdetail("Unexpected end of input.")));
					break;
				case '"':

					/*
					 * 引号必须出现在层级开始之后、
					 * 被引元素开始之后或元素分隔符之后。
					 * 此时必已越过元素起点。
					 */
					if (parse_state != ARRAY_LEVEL_STARTED &&
						parse_state != ARRAY_QUOTED_ELEM_STARTED &&
						parse_state != ARRAY_ELEM_DELIMITED)
						ereturn(escontext, -1,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"", str),
								 errdetail("Unexpected array element.")));
					in_quotes = !in_quotes;
					if (in_quotes)
						parse_state = ARRAY_QUOTED_ELEM_STARTED;
					else
						parse_state = ARRAY_QUOTED_ELEM_COMPLETED;
					break;
				case '{':
					if (!in_quotes)
					{
						/*
						 * 左花括号仅能在尚未嵌套、层级开始
						 * 之后或层级分隔符之后出现。
						 *
						 */
						if (parse_state != ARRAY_NO_LEVEL &&
							parse_state != ARRAY_LEVEL_STARTED &&
							parse_state != ARRAY_LEVEL_DELIMITED)
							ereturn(escontext, -1,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Unexpected \"%c\" character.",
											   '{')));
						parse_state = ARRAY_LEVEL_STARTED;
						if (nest_level >= MAXDIM)
							ereturn(escontext, -1,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
											nest_level + 1, MAXDIM)));
						temp[nest_level] = 0;
						nest_level++;
						if (ndim < nest_level)
							ndim = nest_level;
					}
					break;
				case '}':
					if (!in_quotes)
					{
						/*
						 * 右花括号可在元素开始、元素完成、
						 * 被引元素完成或层级完成之后出现。
						 *
						 */
						if (parse_state != ARRAY_ELEM_STARTED &&
							parse_state != ARRAY_ELEM_COMPLETED &&
							parse_state != ARRAY_QUOTED_ELEM_COMPLETED &&
							parse_state != ARRAY_LEVEL_COMPLETED &&
							!(nest_level == 1 && parse_state == ARRAY_LEVEL_STARTED))
							ereturn(escontext, -1,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Unexpected \"%c\" character.",
											   '}')));
						parse_state = ARRAY_LEVEL_COMPLETED;
						if (nest_level == 0)
							ereturn(escontext, -1,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Unmatched \"%c\" character.", '}')));
						nest_level--;

						if (nelems_last[nest_level] != 0 &&
							nelems[nest_level] != nelems_last[nest_level])
							ereturn(escontext, -1,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Multidimensional arrays must have "
											   "sub-arrays with matching "
											   "dimensions.")));
						nelems_last[nest_level] = nelems[nest_level];
						nelems[nest_level] = 1;
						if (nest_level == 0)
							eoArray = itemdone = true;
						else
						{
							/*
							 * 此处不设置 itemdone；见
							 * ReadArrayStr
							 */
							temp[nest_level - 1]++;
						}
					}
					break;
				default:
					if (!in_quotes)
					{
						if (*ptr == typdelim)
						{
							/*
							 * 分隔符可在元素开始、元素完成、
							 * 被引完成或层级完成后。
							 *
							 */
							if (parse_state != ARRAY_ELEM_STARTED &&
								parse_state != ARRAY_ELEM_COMPLETED &&
								parse_state != ARRAY_QUOTED_ELEM_COMPLETED &&
								parse_state != ARRAY_LEVEL_COMPLETED)
								ereturn(escontext, -1,
										(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
										 errmsg("malformed array literal: \"%s\"", str),
										 errdetail("Unexpected \"%c\" character.",
												   typdelim)));
							if (parse_state == ARRAY_LEVEL_COMPLETED)
								parse_state = ARRAY_LEVEL_DELIMITED;
							else
								parse_state = ARRAY_ELEM_DELIMITED;
							itemdone = true;
							nelems[nest_level - 1]++;
						}
						else if (!array_isspace(*ptr))
						{
							/*
							 * 其他非空白字符须在层级开始、
							 * 元素开始或元素分隔符之后。无论
							 * 如何必已越过元素起点。
							 *
							 */
							if (parse_state != ARRAY_LEVEL_STARTED &&
								parse_state != ARRAY_ELEM_STARTED &&
								parse_state != ARRAY_ELEM_DELIMITED)
								ereturn(escontext, -1,
										(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
										 errmsg("malformed array literal: \"%s\"", str),
										 errdetail("Unexpected array element.")));
							parse_state = ARRAY_ELEM_STARTED;
						}
					}
					break;
			}
			if (!itemdone)
				ptr++;
		}
		temp[ndim - 1]++;
		ptr++;
	}

	/* 闭合花括号之后只允许空白 */
	while (*ptr)
	{
		if (!array_isspace(*ptr++))
			ereturn(escontext, -1,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", str),
					 errdetail("Junk after closing right brace.")));
	}

	/* 空数组的特殊情形 */
	if (empty_array)
		return 0;

	for (i = 0; i < ndim; ++i)
		dim[i] = temp[i];

	return ndim;
}

/*
 * ReadArrayStr :
 *	 解析 "arrayStr" 所指向的数组串，并把值转换
 *	 为内部格式。未指定的元素初始化为 null。
 *	 数组维度必须已经确定。
 *
 * 输入:
 *	arrayStr: 待解析的串。
 *			  注意："arrayStr" 的内容将被修改！
 *	origStr: 未修改的输入串，仅用于错误消息。
 *	nitems: 数组元素总数（已预先确定）。
 *	ndim: 数组维度数
 *	dim[]: 数组各轴长度
 *	inputproc: 元素数据类型的专用输入过程。
 *	typioparam, typmod: 传给 inputproc 的辅助值。
 *	typdelim: 值分隔符（类型相关）。
 *	typlen, typbyval, typalign: 元素数据类型的存储参数。
 *
 * 输出:
 *	values[]: 填入转换后的数据值。
 *	nulls[]: 填入空值标记。
 *	*hasnulls: 存在空元素时设为真。
 *	*nbytes: 设为所需数据区总大小（含对齐
 *		填充但不含数组头部开销）。
 *	*escontext: 若其指向 ErrorSaveContext，则任何
 *		错误详情报告于此。
 *
 * 结果:
 *	成功为 true，失败为 false（若提供 escontext）。
 *
 * 注意 values[] 与 nulls[] 由调用者分配，且必须拥有
 * nitems 元素。
 */
static bool
ReadArrayStr(char *arrayStr,
			 const char *origStr,
			 int nitems,
			 int ndim,
			 int *dim,
			 FmgrInfo *inputproc,
			 Oid typioparam,
			 int32 typmod,
			 char typdelim,
			 int typlen,
			 bool typbyval,
			 char typalign,
			 Datum *values,
			 bool *nulls,
			 bool *hasnulls,
			 int32 *nbytes,
			 Node *escontext)
{
	int			i,
				nest_level = 0;
	char	   *srcptr;
	bool		in_quotes = false;
	bool		eoArray = false;
	bool		hasnull;
	int32		totbytes;
	int			indx[MAXDIM] = {0},
				prod[MAXDIM];

	mda_get_prod(ndim, dim, prod);

	/* 空值标记初始化为真 */
	memset(nulls, true, nitems * sizeof(bool));

	/*
	 * 我们必须去掉双引号与反斜杠，以构造干净
	 * 的项值传给数据类型输入例程。为此我们将
	 * 每一项值在 arrayStr 内就地覆写。srcptr 是当前
	 * 扫描点，dstptr 是拷贝目标。
	 *
	 * 我们还希望抑制前导与尾部的未加引号空白。
	 * 我们用 leadingspace 标志来抑制前导空格。尾
	 * 部空白则通过让 dstendptr 指向最后有效输出
	 * 字符。
	 *
	 * 此例程中的错误检查大多是走过场，因为我们
	 * 预期 ArrayCount() 已验证过该串。所以不费心
	 * errdetail 消息。
	 */
	srcptr = arrayStr;
	while (!eoArray)
	{
		bool		itemdone = false;
		bool		leadingspace = true;
		bool		hasquoting = false;
		char	   *itemstart;
		char	   *dstptr;
		char	   *dstendptr;

		i = -1;
		itemstart = dstptr = dstendptr = srcptr;

		while (!itemdone)
		{
			switch (*srcptr)
			{
				case '\0':
					/* 报告串提前结束 */
					ereturn(escontext, false,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed array literal: \"%s\"",
									origStr)));
					break;
				case '\\':
					/* 跳过反斜杠并原样拷贝下个字符 */
					srcptr++;
					if (*srcptr == '\0')
						ereturn(escontext, false,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"",
										origStr)));
					*dstptr++ = *srcptr++;
					/* 把被转义字符当作非空白 */
					leadingspace = false;
					dstendptr = dstptr;
					hasquoting = true;	/* 不为 NULL 标记 */
					break;
				case '"':
					in_quotes = !in_quotes;
					if (in_quotes)
						leadingspace = false;
					else
					{
						/*
						 * 在 in_quotes 退出时前进 dstendptr，
						 * 免得在所有其他 in_quotes 分支中再
						 * 做。
						 */
						dstendptr = dstptr;
					}
					hasquoting = true;	/* 不为 NULL 标记 */
					srcptr++;
					break;
				case '{':
					if (!in_quotes)
					{
						if (nest_level >= ndim)
							ereturn(escontext, false,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"",
											origStr)));
						nest_level++;
						indx[nest_level - 1] = 0;
						srcptr++;
					}
					else
						*dstptr++ = *srcptr++;
					break;
				case '}':
					if (!in_quotes)
					{
						if (nest_level == 0)
							ereturn(escontext, false,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"",
											origStr)));
						if (i == -1)
							i = ArrayGetOffset0(ndim, indx, prod);
						indx[nest_level - 1] = 0;
						nest_level--;
						if (nest_level == 0)
							eoArray = itemdone = true;
						else
							indx[nest_level - 1]++;
						srcptr++;
					}
					else
						*dstptr++ = *srcptr++;
					break;
				default:
					if (in_quotes)
						*dstptr++ = *srcptr++;
					else if (*srcptr == typdelim)
					{
						if (i == -1)
							i = ArrayGetOffset0(ndim, indx, prod);
						itemdone = true;
						indx[ndim - 1]++;
						srcptr++;
					}
					else if (array_isspace(*srcptr))
					{
						/*
						 * 若为前导空白则立即丢弃，否则拷贝
						 * 但不前进 dstendptr。
						 */
						if (leadingspace)
							srcptr++;
						else
							*dstptr++ = *srcptr++;
					}
					else
					{
						*dstptr++ = *srcptr++;
						leadingspace = false;
						dstendptr = dstptr;
					}
					break;
			}
		}

		Assert(dstptr < srcptr);
		*dstendptr = '\0';

		if (i < 0 || i >= nitems)
			ereturn(escontext, false,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"",
							origStr)));

		if (Array_nulls && !hasquoting &&
			pg_strcasecmp(itemstart, "NULL") == 0)
		{
			/* 为 NULL 项 */
			if (!InputFunctionCallSafe(inputproc, NULL,
									   typioparam, typmod,
									   escontext,
									   &values[i]))
				return false;
			nulls[i] = true;
		}
		else
		{
			if (!InputFunctionCallSafe(inputproc, itemstart,
									   typioparam, typmod,
									   escontext,
									   &values[i]))
				return false;
			nulls[i] = false;
		}
	}

	/*
	 * 检查空值并计算所需的总数据空间
	 */
	hasnull = false;
	totbytes = 0;
	for (i = 0; i < nitems; i++)
	{
		if (nulls[i])
			hasnull = true;
		else
		{
			/* 顺带确认数据未被 TOAST */
			if (typlen == -1)
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			totbytes = att_addlength_datum(totbytes, typlen, values[i]);
			totbytes = att_align_nominal(totbytes, typalign);
			/* 检查总请求是否溢出 */
			if (!AllocSizeIsValid(totbytes))
				ereturn(escontext, false,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
		}
	}
	*hasnulls = hasnull;
	*nbytes = totbytes;
	return true;
}


/*
 * 把数据从临时 Datum 数组拷入数组对象。
 *
 * array: 数组对象（头部字段已填好）
 * values: 待拷贝的 Datum 数组
 * nulls: 空值标志数组（无空值时可为 NULL）
 * nitems: 待拷贝的 Datum 个数
 * typbyval, typlen, typalign: 元素数据类型信息
 * freedata: 若为真且元素类型是引用传递，则在
 * 拷完 Datum 后释放其数据值。
 *
 * 若输入数据是 varlena 类型，调用者必须已确保
 * 值未被 TOAST 压缩。（在这里做没用，因为调用
 * 者已为数组分配好了空间……）
 */
void
CopyArrayEls(ArrayType *array,
			 Datum *values,
			 bool *nulls,
			 int nitems,
			 int typlen,
			 bool typbyval,
			 char typalign,
			 bool freedata)
{
	char	   *p = ARR_DATA_PTR(array);
	bits8	   *bitmap = ARR_NULLBITMAP(array);
	int			bitval = 0;
	int			bitmask = 1;
	int			i;

	if (typbyval)
		freedata = false;

	for (i = 0; i < nitems; i++)
	{
		if (nulls && nulls[i])
		{
			if (!bitmap)		/* 不应发生 */
				elog(ERROR, "null array element where not supported");
			/* 该位保持 0 */
		}
		else
		{
			bitval |= bitmask;
			p += ArrayCastAndSet(values[i], typlen, typbyval, typalign, p);
			if (freedata)
				pfree(DatumGetPointer(values[i]));
		}
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				*bitmap++ = bitval;
				bitval = 0;
				bitmask = 1;
			}
		}
	}

	if (bitmap && bitmask != 1)
		*bitmap = bitval;
}

/*
 * array_out :
 *		   取得数组的内部表示并返回一个字符串，其中
 *		  含外部格式的数组。
 */
Datum
array_out(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	Oid			element_type = AARR_ELEMTYPE(v);
	int			typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	char	   *p,
			   *tmp,
			   *retval,
			  **values,
				dims_str[(MAXDIM * 33) + 2];

	/*
	 * 每维 33，假设每数字 15 位外加 ':' +'[]'
	 *
	 * +2 为赋值操作符与尾部 null 留出空间
	 */
	bool	   *needquotes,
				needdims = false;
	size_t		overall_length;
	int			nitems,
				i,
				j,
				k,
				indx[MAXDIM];
	int			ndim,
			   *dims,
			   *lb;
	array_iter	iter;
	ArrayMetaState *my_extra;

	/*
	 * 我们安排对元素类型（包括其输出转换过
	 * 程）的查找每轮调用只做一次，前提是元素
	 * 类型不在中途被更换。
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
		/*
		 * 获取元素类型的信息，包括其输出转换过程
		 */
		get_type_io_data(element_type, IOFunc_output,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typdelim = my_extra->typdelim;

	ndim = AARR_NDIM(v);
	dims = AARR_DIMS(v);
	lb = AARR_LBOUND(v);
	nitems = ArrayGetNItems(ndim, dims);

	if (nitems == 0)
	{
		retval = pstrdup("{}");
		PG_RETURN_CSTRING(retval);
	}

	/*
	 * 若任一维的下界不是 1，就需要添加显式维度
	 *
	 */
	for (i = 0; i < ndim; i++)
	{
		if (lb[i] != 1)
		{
			needdims = true;
			break;
		}
	}

	/*
	 * 把所有值转换为串形式，计算所需总空间（含
	 * 反斜杠转义等开销），并检测每个项是否需要
	 * 双引号。
	 */
	values = (char **) palloc(nitems * sizeof(char *));
	needquotes = (bool *) palloc(nitems * sizeof(bool));
	overall_length = 0;

	array_iter_setup(&iter, v);

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		bool		isnull;
		bool		needquote;

		/* 取得源元素，检查 NULL */
		itemvalue = array_iter_next(&iter, &isnull, i,
									typlen, typbyval, typalign);

		if (isnull)
		{
			values[i] = pstrdup("NULL");
			overall_length += 4;
			needquote = false;
		}
		else
		{
			values[i] = OutputFunctionCall(&my_extra->proc, itemvalue);

			/* 统计数据加反斜杠；检测需要引号的字符 */
			if (values[i][0] == '\0')
				needquote = true;	/* 空串须加引号 */
			else if (pg_strcasecmp(values[i], "NULL") == 0)
				needquote = true;	/* 字面量 NULL 须加引号 */
			else
				needquote = false;

			for (tmp = values[i]; *tmp != '\0'; tmp++)
			{
				char		ch = *tmp;

				overall_length += 1;
				if (ch == '"' || ch == '\\')
				{
					needquote = true;
					overall_length += 1;
				}
				else if (ch == '{' || ch == '}' || ch == typdelim ||
						 array_isspace(ch))
					needquote = true;
			}
		}

		needquotes[i] = needquote;

		/* 若需要则统计成对的双引号 */
		if (needquote)
			overall_length += 2;
		/* 以及逗号或 typdelim 分隔符 */
		overall_length += 1;
	}

	/*
	 * 最后一个数组元素之后没有 typdelim 分隔符，
	 * 但没关系；该空间是尾部 '\0' 所需要的。
	 *
	 * 现在统计输出串中花括号对的总数。
	 */
	for (i = j = 0, k = 1; i < ndim; i++)
	{
		j += k, k *= dims[i];
	}
	overall_length += 2 * j;

	/* 需要时格式化显式维度 */
	dims_str[0] = '\0';
	if (needdims)
	{
		char	   *ptr = dims_str;

		for (i = 0; i < ndim; i++)
		{
			sprintf(ptr, "[%d:%d]", lb[i], lb[i] + dims[i] - 1);
			ptr += strlen(ptr);
		}
		*ptr++ = *ASSGN;
		*ptr = '\0';
		overall_length += ptr - dims_str;
	}

	/* 现在构建输出串 */
	retval = (char *) palloc(overall_length);
	p = retval;

#define APPENDSTR(str)	(strcpy(p, (str)), p += strlen(p))
#define APPENDCHAR(ch)	(*p++ = (ch), *p = '\0')

	if (needdims)
		APPENDSTR(dims_str);
	APPENDCHAR('{');
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = 0;
	k = 0;
	do
	{
		for (i = j; i < ndim - 1; i++)
			APPENDCHAR('{');

		if (needquotes[k])
		{
			APPENDCHAR('"');
			for (tmp = values[k]; *tmp; tmp++)
			{
				char		ch = *tmp;

				if (ch == '"' || ch == '\\')
					*p++ = '\\';
				*p++ = ch;
			}
			*p = '\0';
			APPENDCHAR('"');
		}
		else
			APPENDSTR(values[k]);
		pfree(values[k++]);

		for (i = ndim - 1; i >= 0; i--)
		{
			if (++(indx[i]) < dims[i])
			{
				APPENDCHAR(typdelim);
				break;
			}
			else
			{
				indx[i] = 0;
				APPENDCHAR('}');
			}
		}
		j = i;
	} while (j != -1);

#undef APPENDSTR
#undef APPENDCHAR

	/* 断言我们准确计算了串长度 */
	Assert(overall_length == (p - retval + 1));

	pfree(values);
	pfree(needquotes);

	PG_RETURN_CSTRING(retval);
}

/*
 * array_recv :
 *		  把外部二进制格式的数组转换为
 *		  其内部格式。
 *
 * 返回值 :
 *		  输入数组的内部表示形式
 */
Datum
array_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			spec_element_type = PG_GETARG_OID(1);	/* 数组的元素
														 * 类型 */
	int32		typmod = PG_GETARG_INT32(2);	/* 数组元素的 typmod */
	Oid			element_type;
	int			typlen;
	bool		typbyval;
	char		typalign;
	Oid			typioparam;
	int			i,
				nitems;
	Datum	   *dataPtr;
	bool	   *nullsPtr;
	bool		hasnulls;
	int32		nbytes;
	int32		dataoffset;
	ArrayType  *retval;
	int			ndim,
				flags,
				dim[MAXDIM],
				lBound[MAXDIM];
	ArrayMetaState *my_extra;

	/* 取得数组头部信息 */
	ndim = pq_getmsgint(buf, 4);
	if (ndim < 0)				/* 我们确实允许零维度数组 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid number of dimensions: %d", ndim)));
	if (ndim > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						ndim, MAXDIM)));

	flags = pq_getmsgint(buf, 4);
	if (flags != 0 && flags != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid array flags")));

	/* 检查数据记录的元素类型 */
	element_type = pq_getmsgint(buf, sizeof(Oid));

	/*
	 * 从安全角度看，输入的元素类型是否与我们
	 * 预期相符并不重要：元素类型的接收函数必
	 * 须足够健壮以处理无效数据。然而，从用户
	 * 友好角度看，抱怨类型不匹配比抛出"二进制
	 * 格式不当"错误更好。但有个问题：只有内建
	 * 类型的 OID 才稳定到足以相信不匹配是真问题。
	 * 所以只有当两个 OID 都在内建范围内时才抱怨。
	 * 否则，就带着元素类型继续，也就是我们本应
	 * 得到的类型。
	 */
	if (element_type != spec_element_type)
	{
		if (element_type < FirstGenbkiObjectId &&
			spec_element_type < FirstGenbkiObjectId)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("binary data has array element type %u (%s) instead of expected %u (%s)",
							element_type,
							format_type_extended(element_type, -1,
												 FORMAT_TYPE_ALLOW_INVALID),
							spec_element_type,
							format_type_extended(spec_element_type, -1,
												 FORMAT_TYPE_ALLOW_INVALID))));
		element_type = spec_element_type;
	}

	for (i = 0; i < ndim; i++)
	{
		dim[i] = pq_getmsgint(buf, 4);
		lBound[i] = pq_getmsgint(buf, 4);
	}

	/* 检查数组维度是否溢出 */
	nitems = ArrayGetNItems(ndim, dim);
	ArrayCheckBounds(ndim, dim, lBound);

	/*
	 * 我们安排对元素类型（包括其接收转换过
	 * 程）的查找每轮调用只做一次，前提是元素
	 * 类型不在中途被更换。
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
		/* 获取元素类型的信息，包括其接收过程 */
		get_type_io_data(element_type, IOFunc_receive,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		if (!OidIsValid(my_extra->typiofunc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("no binary input function available for type %s",
							format_type_be(element_type))));
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}

	if (nitems == 0)
	{
		/* 返回空数组 …… 但要先验证 element_type */
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(element_type));
	}

	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typioparam = my_extra->typioparam;

	dataPtr = (Datum *) palloc(nitems * sizeof(Datum));
	nullsPtr = (bool *) palloc(nitems * sizeof(bool));
	ReadArrayBinary(buf, nitems,
					&my_extra->proc, typioparam, typmod,
					typlen, typbyval, typalign,
					dataPtr, nullsPtr,
					&hasnulls, &nbytes);
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* 无空值位图的标记 */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	retval = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(retval, nbytes);
	retval->ndim = ndim;
	retval->dataoffset = dataoffset;
	retval->elemtype = element_type;
	memcpy(ARR_DIMS(retval), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(retval), lBound, ndim * sizeof(int));

	CopyArrayEls(retval,
				 dataPtr, nullsPtr, nitems,
				 typlen, typbyval, typalign,
				 true);

	pfree(dataPtr);
	pfree(nullsPtr);

	PG_RETURN_ARRAYTYPE_P(retval);
}

/*
 * ReadArrayBinary:
 *	 以二进制风格收集正在读取的数组的数据元素。
 *
 * 输入:
 *	buf: 源数据缓冲区。
 *	nitems: 数组元素总数（已读取）。
 *	receiveproc: 元素数据类型的专用接收过程。
 *	typioparam, typmod: 传给 receiveproc 的辅助值。
 *	typlen, typbyval, typalign: 元素数据类型的存储参数。
 *
 * 输出:
 *	values[]: 填入转换后的数据值。
 *	nulls[]: 填入空值标记。
 *	*hasnulls: 存在空元素时设为真。
 *	*nbytes: 设为所需数据区总大小（含对齐
 *		填充但不含数组头部开销）。
 *
 * 注意 values[] 与 nulls[] 由调用者分配，且必须拥有
 * nitems 元素。
 */
static void
ReadArrayBinary(StringInfo buf,
				int nitems,
				FmgrInfo *receiveproc,
				Oid typioparam,
				int32 typmod,
				int typlen,
				bool typbyval,
				char typalign,
				Datum *values,
				bool *nulls,
				bool *hasnulls,
				int32 *nbytes)
{
	int			i;
	bool		hasnull;
	int32		totbytes;

	for (i = 0; i < nitems; i++)
	{
		int			itemlen;
		StringInfoData elem_buf;
		char		csave;

		/* 取得并检查项长度 */
		itemlen = pq_getmsgint(buf, 4);
		if (itemlen < -1 || itemlen > (buf->len - buf->cursor))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("insufficient data left in message")));

		if (itemlen == -1)
		{
			/* -1 长度为 NULL */
			values[i] = ReceiveFunctionCall(receiveproc, NULL,
											typioparam, typmod);
			nulls[i] = true;
			continue;
		}

		/*
		 * 与其拷来拷去数据，我们只设一个伪 StringInfo
		 * 指向输入缓冲区的正确部分。我们假定可以
		 * 随意改写输入缓冲区，以维持 StringInfo 带尾
		 * 部 null 的约定。
		 */
		elem_buf.data = &buf->data[buf->cursor];
		elem_buf.maxlen = itemlen + 1;
		elem_buf.len = itemlen;
		elem_buf.cursor = 0;

		buf->cursor += itemlen;

		csave = buf->data[buf->cursor];
		buf->data[buf->cursor] = '\0';

		/* 调用元素的 receiveproc */
		values[i] = ReceiveFunctionCall(receiveproc, &elem_buf,
										typioparam, typmod);
		nulls[i] = false;

		/* 若未消费整个缓冲区则出错 */
		if (elem_buf.cursor != itemlen)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("improper binary format in array element %d",
							i + 1)));

		buf->data[buf->cursor] = csave;
	}

	/*
	 * 检查空值并计算所需的总数据空间
	 */
	hasnull = false;
	totbytes = 0;
	for (i = 0; i < nitems; i++)
	{
		if (nulls[i])
			hasnull = true;
		else
		{
			/* 顺带确认数据未被 TOAST */
			if (typlen == -1)
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			totbytes = att_addlength_datum(totbytes, typlen, values[i]);
			totbytes = att_align_nominal(totbytes, typalign);
			/* 检查总请求是否溢出 */
			if (!AllocSizeIsValid(totbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
		}
	}
	*hasnulls = hasnull;
	*nbytes = totbytes;
}


/*
 * array_send :
 *		  取得数组的内部表示并返回一个 bytea，其中
 *		  含外部二进制格式的数组。
 */
Datum
array_send(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	Oid			element_type = AARR_ELEMTYPE(v);
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			nitems,
				i;
	int			ndim,
			   *dim,
			   *lb;
	StringInfoData buf;
	array_iter	iter;
	ArrayMetaState *my_extra;

	/*
	 * 我们安排对元素类型（包括其发送转换过
	 * 程）的查找每轮调用只做一次，前提是元素
	 * 类型不在中途被更换。
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
		/* 获取元素类型信息，包括其发送过程 */
		get_type_io_data(element_type, IOFunc_send,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		if (!OidIsValid(my_extra->typiofunc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("no binary output function available for type %s",
							format_type_be(element_type))));
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;

	ndim = AARR_NDIM(v);
	dim = AARR_DIMS(v);
	lb = AARR_LBOUND(v);
	nitems = ArrayGetNItems(ndim, dim);

	pq_begintypsend(&buf);

	/* 发送数组头部信息 */
	pq_sendint32(&buf, ndim);
	pq_sendint32(&buf, AARR_HASNULL(v) ? 1 : 0);
	pq_sendint32(&buf, element_type);
	for (i = 0; i < ndim; i++)
	{
		pq_sendint32(&buf, dim[i]);
		pq_sendint32(&buf, lb[i]);
	}

	/* 用元素自身的 sendproc 发送数组元素 */
	array_iter_setup(&iter, v);

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		bool		isnull;

		/* 取得源元素，检查 NULL */
		itemvalue = array_iter_next(&iter, &isnull, i,
									typlen, typbyval, typalign);

		if (isnull)
		{
			/* -1 长度为 NULL */
			pq_sendint32(&buf, -1);
		}
		else
		{
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&my_extra->proc, itemvalue);
			pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
		}
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * array_ndims :
 *		  返回 "v" 所指向数组的维度数
 */
Datum
array_ndims(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);

	/* 合理性检查：看起来像数组吗? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	PG_RETURN_INT32(AARR_NDIM(v));
}

/*
 * array_dims :
 *		  以 "text" 返回 "v" 所指向数组的维度
 */
Datum
array_dims(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	char	   *p;
	int			i;
	int		   *dimv,
			   *lb;

	/*
	 * 33，假设每个数字 15 位外加 ':' +'[]'
	 *
	 * +1 为尾部 null
	 */
	char		buf[MAXDIM * 33 + 1];

	/* 合理性检查：看起来像数组吗? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	dimv = AARR_DIMS(v);
	lb = AARR_LBOUND(v);

	p = buf;
	for (i = 0; i < AARR_NDIM(v); i++)
	{
		sprintf(p, "[%d:%d]", lb[i], dimv[i] + lb[i] - 1);
		p += strlen(p);
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * array_lower :
 *		返回 "v" 所指向数组的指定 DIM 的下界，
 *		结果为 int4。
 */
Datum
array_lower(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *lb;
	int			result;

	/* 合理性检查：看起来像数组吗? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* 合理性检查：维度有效? */
	if (reqdim <= 0 || reqdim > AARR_NDIM(v))
		PG_RETURN_NULL();

	lb = AARR_LBOUND(v);
	result = lb[reqdim - 1];

	PG_RETURN_INT32(result);
}

/*
 * array_upper :
 *		返回 "v" 所指向数组的指定 DIM 的上界，
 *		结果为 int4。
 */
Datum
array_upper(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *dimv,
			   *lb;
	int			result;

	/* 合理性检查：看起来像数组吗? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* 合理性检查：维度有效? */
	if (reqdim <= 0 || reqdim > AARR_NDIM(v))
		PG_RETURN_NULL();

	lb = AARR_LBOUND(v);
	dimv = AARR_DIMS(v);

	result = dimv[reqdim - 1] + lb[reqdim - 1] - 1;

	PG_RETURN_INT32(result);
}

/*
 * array_length :
 *		返回 "v" 所指向数组指定维度的长度，
 *		结果为 int4。
 */
Datum
array_length(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *dimv;
	int			result;

	/* 合理性检查：看起来像数组吗? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* 合理性检查：维度有效? */
	if (reqdim <= 0 || reqdim > AARR_NDIM(v))
		PG_RETURN_NULL();

	dimv = AARR_DIMS(v);

	result = dimv[reqdim - 1];

	PG_RETURN_INT32(result);
}

/*
 * array_cardinality:
 *		返回数组中元素的总数
 */
Datum
array_cardinality(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);

	PG_RETURN_INT32(ArrayGetNItems(AARR_NDIM(v), AARR_DIMS(v)));
}


/*
 * array_get_element :
 *	  本例程取得一个数组 datum 和一个下标数组，
 *	  返回所引用的项作为 Datum。注意对于引用传
 *	  递类型，返回的 Datum 是数组对象内的指针。
 *
 * 本函数同时处理普通 varlena 数组与定长数组。
 *
 * 输入:
 *	arraydatum: 数组对象（不得为 NULL）
 *	nSubscripts: 提供的下标个数
 *	indx[]: 下标值
 *	arraytyplen: 数组类型的 pg_type.typlen
 *	elmlen: 数组的元素类型 pg_type.typlen
 *	elmbyval: 数组的元素类型 pg_type.typbyval
 *	elmalign: 数组的元素类型 pg_type.typalign
 *
 * 输出:
 *	返回值就是元素 Datum。
 *	设置 *isNull 来指明元素是否为 NULL。
 */
Datum
array_get_element(Datum arraydatum,
				  int nSubscripts,
				  int *indx,
				  int arraytyplen,
				  int elmlen,
				  bool elmbyval,
				  char elmalign,
				  bool *isNull)
{
	int			i,
				ndim,
			   *dim,
			   *lb,
				offset,
				fixedDim[1],
				fixedLb[1];
	char	   *arraydataptr,
			   *retptr;
	bits8	   *arraynullsptr;

	if (arraytyplen > 0)
	{
		/*
		 * 定长数组 —— 假定为一维、以 0 为基
		 */
		ndim = 1;
		fixedDim[0] = arraytyplen / elmlen;
		fixedLb[0] = 0;
		dim = fixedDim;
		lb = fixedLb;
		arraydataptr = (char *) DatumGetPointer(arraydatum);
		arraynullsptr = NULL;
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(arraydatum)))
	{
		/* 展开数组：放到单独的函数中处理 */
		return array_get_element_expanded(arraydatum,
										  nSubscripts,
										  indx,
										  arraytyplen,
										  elmlen,
										  elmbyval,
										  elmalign,
										  isNull);
	}
	else
	{
		/* 必要时解 TOAST 数组，产生普通 varlena 输入 */
		ArrayType  *array = DatumGetArrayTypeP(arraydatum);

		ndim = ARR_NDIM(array);
		dim = ARR_DIMS(array);
		lb = ARR_LBOUND(array);
		arraydataptr = ARR_DATA_PTR(array);
		arraynullsptr = ARR_NULLBITMAP(array);
	}

	/*
	 * 下标无效时返回 NULL
	 */
	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
	{
		*isNull = true;
		return (Datum) 0;
	}
	for (i = 0; i < ndim; i++)
	{
		if (indx[i] < lb[i] || indx[i] >= (dim[i] + lb[i]))
		{
			*isNull = true;
			return (Datum) 0;
		}
	}

	/*
	 * 计算元素编号
	 */
	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	/*
	 * 检查 NULL 数组元素
	 */
	if (array_get_isnull(arraynullsptr, offset))
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * 好，取得元素
	 */
	*isNull = false;
	retptr = array_seek(arraydataptr, 0, arraynullsptr, offset,
						elmlen, elmbyval, elmalign);
	return ArrayCast(retptr, elmbyval, elmlen);
}

/*
 * 针对展开数组的 array_get_element() 实现
 */
static Datum
array_get_element_expanded(Datum arraydatum,
						   int nSubscripts, int *indx,
						   int arraytyplen,
						   int elmlen, bool elmbyval, char elmalign,
						   bool *isNull)
{
	ExpandedArrayHeader *eah;
	int			i,
				ndim,
			   *dim,
			   *lb,
				offset;
	Datum	   *dvalues;
	bool	   *dnulls;

	eah = (ExpandedArrayHeader *) DatumGetEOHP(arraydatum);
	Assert(eah->ea_magic == EA_MAGIC);

	/* 对照对象校验调用者信息 */
	Assert(arraytyplen == -1);
	Assert(elmlen == eah->typlen);
	Assert(elmbyval == eah->typbyval);
	Assert(elmalign == eah->typalign);

	ndim = eah->ndims;
	dim = eah->dims;
	lb = eah->lbound;

	/*
	 * 下标无效时返回 NULL
	 */
	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
	{
		*isNull = true;
		return (Datum) 0;
	}
	for (i = 0; i < ndim; i++)
	{
		if (indx[i] < lb[i] || indx[i] >= (dim[i] + lb[i]))
		{
			*isNull = true;
			return (Datum) 0;
		}
	}

	/*
	 * 计算元素编号
	 */
	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	/*
	 * 若尚未解构数组则解构之。注意即使输入名义
	 * 上是只读的也应用此操作：这应当足够安全。
	 */
	deconstruct_expanded_array(eah);

	dvalues = eah->dvalues;
	dnulls = eah->dnulls;

	/*
	 * 检查 NULL 数组元素
	 */
	if (dnulls && dnulls[offset])
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * 好，取得元素。把引用传递的值作为指向展开
	 * 数组的指针返回是可以的，理由与普通
	 * array_get_element 可以返回指向平铺数组的指
	 * 针相同：只要 Datum 引用还在，即假定值不变。
	 */
	*isNull = false;
	return dvalues[offset];
}

/*
 * array_get_slice :
 *		   本例程取得一个数组和一组索引（upperIndx
 *		    与 lowerIndx），为所引用的元素创建新的数组
 *		   结构并返回其指针。
 *
 * 本函数同时处理普通 varlena 数组与定长数组。
 *
 * 输入:
 *	arraydatum: 数组对象（不得为 NULL）
 *	nSubscripts: 提供的下标个数（upper/lower 必须相同）
 *	upperIndx[]: 上界下标值
 *	lowerIndx[]: 下界下标值
 *	upperProvided[]: 上界下标值已提供时为真
 *	lowerProvided[]: 下界下标值已提供时为真
 *	arraytyplen: 数组类型的 pg_type.typlen
 *	elmlen: 数组的元素类型 pg_type.typlen
 *	elmbyval: 数组的元素类型 pg_type.typbyval
 *	elmalign: 数组的元素类型 pg_type.typalign
 *
 * 输出:
 *	返回值是新数组的 Datum（永不为 NULL）
 *
 * 省略的上界与下界下标值由对应的数组边界
 * 替换。
 *
 * 注意：我们假定可以随意改写所提供的下标数组
 *  lowerIndx[] 与 upperIndx[]；而且这些数组大小必须
 * 是 MAXDIM（即使 nSubscripts 更小）。多为临时值。
 */
Datum
array_get_slice(Datum arraydatum,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				bool *upperProvided,
				bool *lowerProvided,
				int arraytyplen,
				int elmlen,
				bool elmbyval,
				char elmalign)
{
	ArrayType  *array;
	ArrayType  *newarray;
	int			i,
				ndim,
			   *dim,
			   *lb,
			   *newlb;
	int			fixedDim[1],
				fixedLb[1];
	Oid			elemtype;
	char	   *arraydataptr;
	bits8	   *arraynullsptr;
	int32		dataoffset;
	int			bytes,
				span[MAXDIM];

	if (arraytyplen > 0)
	{
		/*
		 * 定长数组 —— 目前无法对其切片，因为解析器把
		 * 输出标为定长数组类型！下方代码展示了如果
		 * 解析器改为把输出标为合适的 varlena 数组类
		 * 型，我们可以怎样支持它。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("slices of fixed-length arrays not implemented")));

		/*
		 * 定长数组 —— 假定为一维、以 0 为基
		 *
		 * XXX 我们从哪里能取到正确的 ELEMTYPE？
		 */
		ndim = 1;
		fixedDim[0] = arraytyplen / elmlen;
		fixedLb[0] = 0;
		dim = fixedDim;
		lb = fixedLb;
		elemtype = InvalidOid;	/* XXX */
		arraydataptr = (char *) DatumGetPointer(arraydatum);
		arraynullsptr = NULL;
	}
	else
	{
		/* 必要时解 TOAST 输入数组 */
		array = DatumGetArrayTypeP(arraydatum);

		ndim = ARR_NDIM(array);
		dim = ARR_DIMS(array);
		lb = ARR_LBOUND(array);
		elemtype = ARR_ELEMTYPE(array);
		arraydataptr = ARR_DATA_PTR(array);
		arraynullsptr = ARR_NULLBITMAP(array);
	}

	/*
	 * 检查所提供的下标。超出当前数组限度的切片
	 * 会被静默截断到数组限度。若最终得到空切片，
	 * 则返回空数组。
	 */
	if (ndim < nSubscripts || ndim <= 0 || ndim > MAXDIM)
		return PointerGetDatum(construct_empty_array(elemtype));

	for (i = 0; i < nSubscripts; i++)
	{
		if (!lowerProvided[i] || lowerIndx[i] < lb[i])
			lowerIndx[i] = lb[i];
		if (!upperProvided[i] || upperIndx[i] >= (dim[i] + lb[i]))
			upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			return PointerGetDatum(construct_empty_array(elemtype));
	}
	/* 用完整数组范围填补缺失的下标位置 */
	for (; i < ndim; i++)
	{
		lowerIndx[i] = lb[i];
		upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			return PointerGetDatum(construct_empty_array(elemtype));
	}

	mda_get_range(ndim, span, lowerIndx, upperIndx);

	bytes = array_slice_size(arraydataptr, arraynullsptr,
							 ndim, dim, lb,
							 lowerIndx, upperIndx,
							 elmlen, elmbyval, elmalign);

	/*
	 * 目前若源带空值位图，我们就在结果中放入
	 * 空值位图 ……
	 */
	if (arraynullsptr)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, ArrayGetNItems(ndim, span));
		bytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* 无空值位图的标记 */
		bytes += ARR_OVERHEAD_NONULLS(ndim);
	}

	newarray = (ArrayType *) palloc0(bytes);
	SET_VARSIZE(newarray, bytes);
	newarray->ndim = ndim;
	newarray->dataoffset = dataoffset;
	newarray->elemtype = elemtype;
	memcpy(ARR_DIMS(newarray), span, ndim * sizeof(int));

	/*
	 * 新数组的下界设为 1。以前（7.3 之前）我们
	 * 会拷贝给定的 lowerIndx 值……但这易混淆。
	 */
	newlb = ARR_LBOUND(newarray);
	for (i = 0; i < ndim; i++)
		newlb[i] = 1;

	array_extract_slice(newarray,
						ndim, dim, lb,
						arraydataptr, arraynullsptr,
						lowerIndx, upperIndx,
						elmlen, elmbyval, elmalign);

	return PointerGetDatum(newarray);
}

/*
 * array_set_element :
 *		  本例程把一个数组元素的值（该元素由一个下
 *		  标数组指定）设置为 "dataValue" 指定的新值。
 *
 * 本函数同时处理普通 varlena 数组与定长数组。
 *
 * 输入:
 *	arraydatum: 初始数组对象（不得为 NULL）
 *	nSubscripts: 提供的下标个数
 *	indx[]: 下标值
 *	dataValue: 要在给定位置插入的 datum
 *	isNull: dataValue 是否为 NULL
 *	arraytyplen: 数组类型的 pg_type.typlen
 *	elmlen: 数组的元素类型 pg_type.typlen
 *	elmbyval: 数组的元素类型 pg_type.typbyval
 *	elmalign: 数组的元素类型 pg_type.typalign
 *
 * 结果:
 *		  返回一个新数组，它与旧数组除被修改的一
 *		  项之外完全相同。原数组对象不会被改变，
 *		  除非传入的是对展开数组对象的读写引用；
 *		  此时展开数组会被就地更新。
 *
 * 仅对一维数组，我们允许通过给现有下标范
 * 围之外的赋值来扩展数组；现有元素与新元
 * 素之间的位置都被设为 NULL。
 * （XXX TODO：允许多维数组有对应行为）
 *
 * 注意：对赋值而言，下标无效等情形会抛
 * 出错误，而不是像获取操作那样返回 NULL。
 */
Datum
array_set_element(Datum arraydatum,
				  int nSubscripts,
				  int *indx,
				  Datum dataValue,
				  bool isNull,
				  int arraytyplen,
				  int elmlen,
				  bool elmbyval,
				  char elmalign)
{
	ArrayType  *array;
	ArrayType  *newarray;
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				offset;
	char	   *elt_ptr;
	bool		newhasnulls;
	bits8	   *oldnullbitmap;
	int			oldnitems,
				newnitems,
				olddatasize,
				newsize,
				olditemlen,
				newitemlen,
				overheadlen,
				oldoverheadlen,
				addedbefore,
				addedafter,
				lenbefore,
				lenafter;

	if (arraytyplen > 0)
	{
		/*
		 * 定长数组 —— 假定为一维、以 0 为基。而且
		 * 我们无法扩展它们。
		 */
		char	   *resultarray;

		if (nSubscripts != 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));

		if (indx[0] < 0 || indx[0] >= arraytyplen / elmlen)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("array subscript out of range")));

		if (isNull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("cannot assign null value to an element of a fixed-length array")));

		resultarray = (char *) palloc(arraytyplen);
		memcpy(resultarray, DatumGetPointer(arraydatum), arraytyplen);
		elt_ptr = (char *) resultarray + indx[0] * elmlen;
		ArrayCastAndSet(dataValue, elmlen, elmbyval, elmalign, elt_ptr);
		return PointerGetDatum(resultarray);
	}

	if (nSubscripts <= 0 || nSubscripts > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/* 确保待插入的项未被 TOAST */
	if (elmlen == -1 && !isNull)
		dataValue = PointerGetDatum(PG_DETOAST_DATUM(dataValue));

	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(arraydatum)))
	{
		/* 展开数组：放到单独的函数中处理 */
		return array_set_element_expanded(arraydatum,
										  nSubscripts,
										  indx,
										  dataValue,
										  isNull,
										  arraytyplen,
										  elmlen,
										  elmbyval,
										  elmalign);
	}

	/* 必要时解 TOAST 输入数组 */
	array = DatumGetArrayTypeP(arraydatum);

	ndim = ARR_NDIM(array);

	/*
	 * 若维度数为零（即空数组），则创建一个具有
	 * nSubscripts 个维度的数组，并把下界设为给定
	 * 下标。
	 */
	if (ndim == 0)
	{
		Oid			elmtype = ARR_ELEMTYPE(array);

		for (i = 0; i < nSubscripts; i++)
		{
			dim[i] = 1;
			lb[i] = indx[i];
		}

		return PointerGetDatum(construct_md_array(&dataValue, &isNull,
												  nSubscripts, dim, lb,
												  elmtype,
												  elmlen, elmbyval, elmalign));
	}

	if (ndim != nSubscripts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/* 拷贝 dim/lb 因可能要修改 */
	memcpy(dim, ARR_DIMS(array), ndim * sizeof(int));
	memcpy(lb, ARR_LBOUND(array), ndim * sizeof(int));

	newhasnulls = (ARR_HASNULL(array) || isNull);
	addedbefore = addedafter = 0;

	/*
	 * 检查下标。我们假定传入的现有下标已通过
	 * ArrayCheckBounds，因此 dim[i] + lb[i] 的计算不
	 * 会溢出。但我们必须提防计算新 dim[] 值时的
	 * 其他溢出。
	 */
	if (ndim == 1)
	{
		if (indx[0] < lb[0])
		{
			/* addedbefore = lb[0] - indx[0]; */
			/* dim[0] += addedbefore; */
			if (pg_sub_s32_overflow(lb[0], indx[0], &addedbefore) ||
				pg_add_s32_overflow(dim[0], addedbefore, &dim[0]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxArraySize)));
			lb[0] = indx[0];
			if (addedbefore > 1)
				newhasnulls = true; /* 将插入空值 */
		}
		if (indx[0] >= (dim[0] + lb[0]))
		{
			/* addedafter = indx[0] - (dim[0] + lb[0]) + 1; */
			/* dim[0] += addedafter; */
			if (pg_sub_s32_overflow(indx[0], dim[0] + lb[0], &addedafter) ||
				pg_add_s32_overflow(addedafter, 1, &addedafter) ||
				pg_add_s32_overflow(dim[0], addedafter, &dim[0]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxArraySize)));
			if (addedafter > 1)
				newhasnulls = true; /* 将插入空值 */
		}
	}
	else
	{
		/*
		 * XXX 目前我们不支持在赋值期间扩展多维数组
		 *
		 */
		for (i = 0; i < ndim; i++)
		{
			if (indx[i] < lb[i] ||
				indx[i] >= (dim[i] + lb[i]))
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array subscript out of range")));
		}
	}

	/* 此处检查数组维度是否溢出 */
	newnitems = ArrayGetNItems(ndim, dim);
	ArrayCheckBounds(ndim, dim, lb);

	/*
	 * 计算各项与待拷贝区域的大小
	 */
	if (newhasnulls)
		overheadlen = ARR_OVERHEAD_WITHNULLS(ndim, newnitems);
	else
		overheadlen = ARR_OVERHEAD_NONULLS(ndim);
	oldnitems = ArrayGetNItems(ndim, ARR_DIMS(array));
	oldnullbitmap = ARR_NULLBITMAP(array);
	oldoverheadlen = ARR_DATA_OFFSET(array);
	olddatasize = ARR_SIZE(array) - oldoverheadlen;
	if (addedbefore)
	{
		offset = 0;
		lenbefore = 0;
		olditemlen = 0;
		lenafter = olddatasize;
	}
	else if (addedafter)
	{
		offset = oldnitems;
		lenbefore = olddatasize;
		olditemlen = 0;
		lenafter = 0;
	}
	else
	{
		offset = ArrayGetOffset(nSubscripts, dim, lb, indx);
		elt_ptr = array_seek(ARR_DATA_PTR(array), 0, oldnullbitmap, offset,
							 elmlen, elmbyval, elmalign);
		lenbefore = (int) (elt_ptr - ARR_DATA_PTR(array));
		if (array_get_isnull(oldnullbitmap, offset))
			olditemlen = 0;
		else
		{
			olditemlen = att_addlength_pointer(0, elmlen, elt_ptr);
			olditemlen = att_align_nominal(olditemlen, elmalign);
		}
		lenafter = (int) (olddatasize - lenbefore - olditemlen);
	}

	if (isNull)
		newitemlen = 0;
	else
	{
		newitemlen = att_addlength_datum(0, elmlen, dataValue);
		newitemlen = att_align_nominal(newitemlen, elmalign);
	}

	newsize = overheadlen + lenbefore + newitemlen + lenafter;

	/*
	 * 好，创建新数组并填入头部/维度
	 */
	newarray = (ArrayType *) palloc0(newsize);
	SET_VARSIZE(newarray, newsize);
	newarray->ndim = ndim;
	newarray->dataoffset = newhasnulls ? overheadlen : 0;
	newarray->elemtype = ARR_ELEMTYPE(array);
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));

	/*
	 * 填入数据
	 */
	memcpy((char *) newarray + overheadlen,
		   (char *) array + oldoverheadlen,
		   lenbefore);
	if (!isNull)
		ArrayCastAndSet(dataValue, elmlen, elmbyval, elmalign,
						(char *) newarray + overheadlen + lenbefore);
	memcpy((char *) newarray + overheadlen + lenbefore + newitemlen,
		   (char *) array + oldoverheadlen + lenbefore + olditemlen,
		   lenafter);

	/*
	 * 若需要则填入空值位图
	 *
	 * 注意：我们可能刚把最后一个 NULL 替换为非
	 * NULL，从而可去掉位图。但似乎不值得为此测试。
	 */
	if (newhasnulls)
	{
		bits8	   *newnullbitmap = ARR_NULLBITMAP(newarray);

		/* 上面的 palloc0 已把插入位置标为空值 */
		/* 修正插入值 */
		if (addedafter)
			array_set_isnull(newnullbitmap, newnitems - 1, isNull);
		else
			array_set_isnull(newnullbitmap, offset, isNull);
		/* 修正拷贝范围 */
		if (addedbefore)
			array_bitmap_copy(newnullbitmap, addedbefore,
							  oldnullbitmap, 0,
							  oldnitems);
		else
		{
			array_bitmap_copy(newnullbitmap, 0,
							  oldnullbitmap, 0,
							  offset);
			if (addedafter == 0)
				array_bitmap_copy(newnullbitmap, offset + 1,
								  oldnullbitmap, offset + 1,
								  oldnitems - offset - 1);
		}
	}

	return PointerGetDatum(newarray);
}

/*
 * 针对展开数组的 array_set_element() 实现
 *
 * 注意：与对读写展开对象的任何操作一样，
 * 我们必须小心避免中途失败时把对象留在损坏状态
 *
 */
static Datum
array_set_element_expanded(Datum arraydatum,
						   int nSubscripts, int *indx,
						   Datum dataValue, bool isNull,
						   int arraytyplen,
						   int elmlen, bool elmbyval, char elmalign)
{
	ExpandedArrayHeader *eah;
	Datum	   *dvalues;
	bool	   *dnulls;
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				offset;
	bool		dimschanged,
				newhasnulls;
	int			addedbefore,
				addedafter;
	char	   *oldValue;

	/* 若非如此则转为读写对象 */
	eah = DatumGetExpandedArray(arraydatum);

	/* 对照对象校验调用者信息；此外不再使用它 */
	Assert(arraytyplen == -1);
	Assert(elmlen == eah->typlen);
	Assert(elmbyval == eah->typbyval);
	Assert(elmalign == eah->typalign);

	/*
	 * 把维度信息拷贝到局部存储。这允许我们在需
	 * 要时修改维度，同时不会在途中失败时弄乱展
	 * 开值。
	 */
	ndim = eah->ndims;
	Assert(ndim >= 0 && ndim <= MAXDIM);
	memcpy(dim, eah->dims, ndim * sizeof(int));
	memcpy(lb, eah->lbound, ndim * sizeof(int));
	dimschanged = false;

	/*
	 * 若维度数为零（即空数组），则创建一个具有
	 * nSubscripts 个维度的数组，并把下界设为给定
	 * 下标。
	 */
	if (ndim == 0)
	{
		/*
		 * 为新维度信息分配足够的空间。即使我们稍后
		 * 失败无妨。
		 */
		Assert(nSubscripts > 0 && nSubscripts <= MAXDIM);
		eah->dims = (int *) MemoryContextAllocZero(eah->hdr.eoh_context,
												   nSubscripts * sizeof(int));
		eah->lbound = (int *) MemoryContextAllocZero(eah->hdr.eoh_context,
													 nSubscripts * sizeof(int));

		/* 更新维度信息局部副本 */
		ndim = nSubscripts;
		for (i = 0; i < nSubscripts; i++)
		{
			dim[i] = 0;
			lb[i] = indx[i];
		}
		dimschanged = true;
	}
	else if (ndim != nSubscripts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/*
	 * 若尚未解构数组则解构之。（也许某天可以为
	 * 定长无空值情形增加一条专用路径，从而可以
	 * 无需解构就就地覆写元素。但今天尚非其时。）
	 *
	 */
	deconstruct_expanded_array(eah);

	/*
	 * 若需要则把新元素拷入数组的上下文（我们假
	 * 定它已解 TOAST，因此不应产生垃圾）。先做这
	 * 一步可确保即使在源是对同一数组某元素的引
	 * 用时我们的行为也是合理的。
	 * 若我们稍后失败，这块内存会泄漏，但那基本
	 * 无害。
	 */
	if (!eah->typbyval && !isNull)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(eah->hdr.eoh_context);

		dataValue = datumCopy(dataValue, false, eah->typlen);
		MemoryContextSwitchTo(oldcxt);
	}

	dvalues = eah->dvalues;
	dnulls = eah->dnulls;

	newhasnulls = ((dnulls != NULL) || isNull);
	addedbefore = addedafter = 0;

	/*
	 * 检查下标（此逻辑必须与 array_set_element
	 * 一致）。我们假定传入的现有下标已通过
	 * ArrayCheckBounds，因此 dim[i]+lb[i] 可无溢出地
	 * 计算，但须防新 dim[] 值其他溢出。
	 */
	if (ndim == 1)
	{
		if (indx[0] < lb[0])
		{
			/* addedbefore = lb[0] - indx[0]; */
			/* dim[0] += addedbefore; */
			if (pg_sub_s32_overflow(lb[0], indx[0], &addedbefore) ||
				pg_add_s32_overflow(dim[0], addedbefore, &dim[0]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxArraySize)));
			lb[0] = indx[0];
			dimschanged = true;
			if (addedbefore > 1)
				newhasnulls = true; /* 将插入空值 */
		}
		if (indx[0] >= (dim[0] + lb[0]))
		{
			/* addedafter = indx[0] - (dim[0] + lb[0]) + 1; */
			/* dim[0] += addedafter; */
			if (pg_sub_s32_overflow(indx[0], dim[0] + lb[0], &addedafter) ||
				pg_add_s32_overflow(addedafter, 1, &addedafter) ||
				pg_add_s32_overflow(dim[0], addedafter, &dim[0]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxArraySize)));
			dimschanged = true;
			if (addedafter > 1)
				newhasnulls = true; /* 将插入空值 */
		}
	}
	else
	{
		/*
		 * XXX 目前我们不支持在赋值期间扩展多维数组
		 *
		 */
		for (i = 0; i < ndim; i++)
		{
			if (indx[i] < lb[i] ||
				indx[i] >= (dim[i] + lb[i]))
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array subscript out of range")));
		}
	}

	/* 检查数组维度是否溢出 */
	if (dimschanged)
	{
		(void) ArrayGetNItems(ndim, dim);
		ArrayCheckBounds(ndim, dim, lb);
	}

	/* 现在可计算目标项在数组中的线性偏移 */
	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	/* 若需要则物理扩容现有 dvalues/dnulls 数组 */
	if (dim[0] > eah->dvalueslen)
	{
		/* 若在扩容则多要一些空间 */
		int			newlen = dim[0] + dim[0] / 8;

		newlen = Max(newlen, dim[0]);	/* 整数溢出保护 */
		eah->dvalues = dvalues = (Datum *)
			repalloc(dvalues, newlen * sizeof(Datum));
		if (dnulls)
			eah->dnulls = dnulls = (bool *)
				repalloc(dnulls, newlen * sizeof(bool));
		eah->dvalueslen = newlen;
	}

	/*
	 * 若需要空值位图而尚没有，则创建它，务必把
	 * 所有现有项都标为非空。
	 */
	if (newhasnulls && dnulls == NULL)
		eah->dnulls = dnulls = (bool *)
			MemoryContextAllocZero(eah->hdr.eoh_context,
								   eah->dvalueslen * sizeof(bool));

	/*
	 * 现在所有需要的空间都已分配，可以开始做不
	 * 可逆的改动了。务必严防在此之后发生失败。
	 */

	/* 平铺值将不再准确表示该数组 */
	eah->fvalue = NULL;
	/* 也不知道平铺后的大小 */
	eah->flat_size = 0;

	/* 若需要则更新维度信息 */
	if (dimschanged)
	{
		eah->ndims = ndim;
		memcpy(eah->dims, dim, ndim * sizeof(int));
		memcpy(eah->lbound, lb, ndim * sizeof(int));
	}

	/* 若需要则重排项，并用空值填补 addedbefore 项 */
	if (addedbefore > 0)
	{
		memmove(dvalues + addedbefore, dvalues, eah->nelems * sizeof(Datum));
		for (i = 0; i < addedbefore; i++)
			dvalues[i] = (Datum) 0;
		if (dnulls)
		{
			memmove(dnulls + addedbefore, dnulls, eah->nelems * sizeof(bool));
			for (i = 0; i < addedbefore; i++)
				dnulls[i] = true;
		}
		eah->nelems += addedbefore;
	}

	/* 空值填补 addedafter 项 */
	if (addedafter > 0)
	{
		for (i = 0; i < addedafter; i++)
			dvalues[eah->nelems + i] = (Datum) 0;
		if (dnulls)
		{
			for (i = 0; i < addedafter; i++)
				dnulls[eah->nelems + i] = true;
		}
		eah->nelems += addedafter;
	}

	/* 需要时抓取旧元素值以便 pfree。 */
	if (!eah->typbyval && (dnulls == NULL || !dnulls[offset]))
		oldValue = (char *) DatumGetPointer(dvalues[offset]);
	else
		oldValue = NULL;

	/* 最后终于可插入新元素了。 */
	dvalues[offset] = dataValue;
	if (dnulls)
		dnulls[offset] = isNull;

	/*
	 * 若需要则释放旧元素；这可以防止反复替换元
	 * 素导致数组存储膨胀。若 pfree 万一失败，也
	 * 不会损坏数组。
	 */
	if (oldValue)
	{
		/* 不要试图 pfree 原平铺数组的一部分 */
		if (oldValue < eah->fstartptr || oldValue >= eah->fendptr)
			pfree(oldValue);
	}

	/* 完成，返回对象的标准 TOAST 指针 */
	return EOHPGetRWDatum(&eah->hdr);
}

/*
 * array_set_slice :
 *		  本例程把一片数组位置的值（由上下界下标值
 *		  指定）设置为以另一个数组传入的新值。
 *
 *
 * 本函数同时处理普通 varlena 数组与定长数组。
 *
 * 输入:
 *	arraydatum: 初始数组对象（不得为 NULL）
 *	nSubscripts: 提供的下标个数（upper/lower 必须相同）
 *	upperIndx[]: 上界下标值
 *	lowerIndx[]: 下界下标值
 *	upperProvided[]: 上界下标值已提供时为真
 *	lowerProvided[]: 下界下标值已提供时为真
 *	srcArrayDatum: 要插入值的来源
 *	isNull: 指明 srcArrayDatum 是否为 NULL
 *	arraytyplen: 数组类型的 pg_type.typlen
 *	elmlen: 数组的元素类型 pg_type.typlen
 *	elmbyval: 数组的元素类型 pg_type.typbyval
 *	elmalign: 数组的元素类型 pg_type.typalign
 *
 * 结果:
 *		  返回一个新数组，它与旧数组除被修改的
 *		  范围外完全相同。原数组对象不会被改变。
 *
 * 省略的上界与下界下标值由对应的数组边界
 * 替换。
 *
 * 仅对一维数组，我们允许通过给现有下标范
 * 围之外的位置赋值来扩展数组；现有元素与
 * 新元素之间的位置都被设为 NULL。
 * （XXX TODO：允许多维数组有对应行为）
 *
 * 注意：我们假定可以随意改写所提供的索引
 *  lowerIndx[] 与 upperIndx[]；而且这些数组大小必须
 * 是 MAXDIM（即使 nSubscripts 更小）。多为临时值。
 *
 * 注意：对赋值而言，荒谬下标等情形会抛出
 * 错误，而非如获取操作那样返回 NULL 或空数组。
 */
Datum
array_set_slice(Datum arraydatum,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				bool *upperProvided,
				bool *lowerProvided,
				Datum srcArrayDatum,
				bool isNull,
				int arraytyplen,
				int elmlen,
				bool elmbyval,
				char elmalign)
{
	ArrayType  *array;
	ArrayType  *srcArray;
	ArrayType  *newarray;
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				span[MAXDIM];
	bool		newhasnulls;
	int			nitems,
				nsrcitems,
				olddatasize,
				newsize,
				olditemsize,
				newitemsize,
				overheadlen,
				oldoverheadlen,
				addedbefore,
				addedafter,
				lenbefore,
				lenafter,
				itemsbefore,
				itemsafter,
				nolditems;

	/* 目前从 NULL 源数组赋值是空操作 */
	if (isNull)
		return arraydatum;

	if (arraytyplen > 0)
	{
		/*
		 * 定长数组 —— 还没空做这个 ……
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("updates on slices of fixed-length arrays not implemented")));
	}

	/* 必要时解两数组 TOAST */
	array = DatumGetArrayTypeP(arraydatum);
	srcArray = DatumGetArrayTypeP(srcArrayDatum);

	/* 注：假定 srcArray 无被 TOAST 的元素 */

	ndim = ARR_NDIM(array);

	/*
	 * 若维度数为零（即空数组），则创建一个具有
	 * nSubscripts 个维度的数组，并把上下界设为所
	 * 供的下标。
	 */
	if (ndim == 0)
	{
		Datum	   *dvalues;
		bool	   *dnulls;
		int			nelems;
		Oid			elmtype = ARR_ELEMTYPE(array);

		deconstruct_array(srcArray, elmtype, elmlen, elmbyval, elmalign,
						  &dvalues, &dnulls, &nelems);

		for (i = 0; i < nSubscripts; i++)
		{
			if (!upperProvided[i] || !lowerProvided[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array slice subscript must provide both boundaries"),
						 errdetail("When assigning to a slice of an empty array value,"
								   " slice boundaries must be fully specified.")));

			/* 计算 "upperIndx[i] - lowerIndx[i] + 1"，检测溢出 */
			if (pg_sub_s32_overflow(upperIndx[i], lowerIndx[i], &dim[i]) ||
				pg_add_s32_overflow(dim[i], 1, &dim[i]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxArraySize)));

			lb[i] = lowerIndx[i];
		}

		/* 源项过少则抱怨；不过多余的项会被忽略 */
		if (nelems < ArrayGetNItems(nSubscripts, dim))
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("source array too small")));

		return PointerGetDatum(construct_md_array(dvalues, dnulls, nSubscripts,
												  dim, lb, elmtype,
												  elmlen, elmbyval, elmalign));
	}

	if (ndim < nSubscripts || ndim <= 0 || ndim > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/* 拷贝 dim/lb 因可能要修改 */
	memcpy(dim, ARR_DIMS(array), ndim * sizeof(int));
	memcpy(lb, ARR_LBOUND(array), ndim * sizeof(int));

	newhasnulls = (ARR_HASNULL(array) || ARR_HASNULL(srcArray));
	addedbefore = addedafter = 0;

	/*
	 * 检查下标。我们假定传入的现有下标已通过
	 * ArrayCheckBounds，因此 dim[i] + lb[i] 的计算不
	 * 会溢出。但我们必须提防计算新 dim[] 值时的
	 * 其他溢出。
	 */
	if (ndim == 1)
	{
		Assert(nSubscripts == 1);
		if (!lowerProvided[0])
			lowerIndx[0] = lb[0];
		if (!upperProvided[0])
			upperIndx[0] = dim[0] + lb[0] - 1;
		if (lowerIndx[0] > upperIndx[0])
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("upper bound cannot be less than lower bound")));
		if (lowerIndx[0] < lb[0])
		{
			/* addedbefore = lb[0] - lowerIndx[0]; */
			/* dim[0] += addedbefore; */
			if (pg_sub_s32_overflow(lb[0], lowerIndx[0], &addedbefore) ||
				pg_add_s32_overflow(dim[0], addedbefore, &dim[0]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxArraySize)));
			lb[0] = lowerIndx[0];
			if (addedbefore > 1)
				newhasnulls = true; /* 将插入空值 */
		}
		if (upperIndx[0] >= (dim[0] + lb[0]))
		{
			/* addedafter = upperIndx[0] - (dim[0] + lb[0]) + 1; */
			/* dim[0] += addedafter; */
			if (pg_sub_s32_overflow(upperIndx[0], dim[0] + lb[0], &addedafter) ||
				pg_add_s32_overflow(addedafter, 1, &addedafter) ||
				pg_add_s32_overflow(dim[0], addedafter, &dim[0]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxArraySize)));
			if (addedafter > 1)
				newhasnulls = true; /* 将插入空值 */
		}
	}
	else
	{
		/*
		 * XXX 目前我们不支持在赋值期间扩展多维数组
		 *
		 */
		for (i = 0; i < nSubscripts; i++)
		{
			if (!lowerProvided[i])
				lowerIndx[i] = lb[i];
			if (!upperProvided[i])
				upperIndx[i] = dim[i] + lb[i] - 1;
			if (lowerIndx[i] > upperIndx[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("upper bound cannot be less than lower bound")));
			if (lowerIndx[i] < lb[i] ||
				upperIndx[i] >= (dim[i] + lb[i]))
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array subscript out of range")));
		}
		/* 用完整数组范围填补缺失的下标位置 */
		for (; i < ndim; i++)
		{
			lowerIndx[i] = lb[i];
			upperIndx[i] = dim[i] + lb[i] - 1;
			if (lowerIndx[i] > upperIndx[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("upper bound cannot be less than lower bound")));
		}
	}

	/* 此举主要为检查溢出 */
	nitems = ArrayGetNItems(ndim, dim);
	ArrayCheckBounds(ndim, dim, lb);

	/*
	 * 确保源数组有足够多的项。注意我们忽略源数
	 * 组的形状，只是串行读取各项。
	 */
	mda_get_range(ndim, span, lowerIndx, upperIndx);
	nsrcitems = ArrayGetNItems(ndim, span);
	if (nsrcitems > ArrayGetNItems(ARR_NDIM(srcArray), ARR_DIMS(srcArray)))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source array too small")));

	/*
	 * 计算新项占用的空间、被替换项占用的空间，
	 * 以及新数组所需的空间。
	 */
	if (newhasnulls)
		overheadlen = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
	else
		overheadlen = ARR_OVERHEAD_NONULLS(ndim);
	newitemsize = array_nelems_size(ARR_DATA_PTR(srcArray), 0,
									ARR_NULLBITMAP(srcArray), nsrcitems,
									elmlen, elmbyval, elmalign);
	oldoverheadlen = ARR_DATA_OFFSET(array);
	olddatasize = ARR_SIZE(array) - oldoverheadlen;
	if (ndim > 1)
	{
		/*
		 * 此处无需处理数组的扩展；如果必须处理的话
		 * 那会复杂得多 ……
		 */
		olditemsize = array_slice_size(ARR_DATA_PTR(array),
									   ARR_NULLBITMAP(array),
									   ndim, dim, lb,
									   lowerIndx, upperIndx,
									   elmlen, elmbyval, elmalign);
		lenbefore = lenafter = 0;	/* 让编译器安静 */
		itemsbefore = itemsafter = nolditems = 0;
	}
	else
	{
		/*
		 * 此处必须考虑到切片大于原数组和/或不与原
		 * 数组下标相邻的情形。
		 */
		int			oldlb = ARR_LBOUND(array)[0];
		int			oldub = oldlb + ARR_DIMS(array)[0] - 1;
		int			slicelb = Max(oldlb, lowerIndx[0]);
		int			sliceub = Min(oldub, upperIndx[0]);
		char	   *oldarraydata = ARR_DATA_PTR(array);
		bits8	   *oldarraybitmap = ARR_NULLBITMAP(array);

		/* 将在切片之前的旧数组项的个数/大小 */
		itemsbefore = Min(slicelb, oldub + 1) - oldlb;
		lenbefore = array_nelems_size(oldarraydata, 0, oldarraybitmap,
									  itemsbefore,
									  elmlen, elmbyval, elmalign);
		/* 将被切片替换的旧数组项的个数/大小 */
		if (slicelb > sliceub)
		{
			nolditems = 0;
			olditemsize = 0;
		}
		else
		{
			nolditems = sliceub - slicelb + 1;
			olditemsize = array_nelems_size(oldarraydata + lenbefore,
											itemsbefore, oldarraybitmap,
											nolditems,
											elmlen, elmbyval, elmalign);
		}
		/* 将在切片之后的旧数组项的个数/大小 */
		itemsafter = oldub + 1 - Max(sliceub + 1, oldlb);
		lenafter = olddatasize - lenbefore - olditemsize;
	}

	newsize = overheadlen + olddatasize - olditemsize + newitemsize;

	newarray = (ArrayType *) palloc0(newsize);
	SET_VARSIZE(newarray, newsize);
	newarray->ndim = ndim;
	newarray->dataoffset = newhasnulls ? overheadlen : 0;
	newarray->elemtype = ARR_ELEMTYPE(array);
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));

	if (ndim > 1)
	{
		/*
		 * 此处无需处理数组的扩展；如果必须处理的话
		 * 那会复杂得多 ……
		 */
		array_insert_slice(newarray, array, srcArray,
						   ndim, dim, lb,
						   lowerIndx, upperIndx,
						   elmlen, elmbyval, elmalign);
	}
	else
	{
		/* 填数据 */
		memcpy((char *) newarray + overheadlen,
			   (char *) array + oldoverheadlen,
			   lenbefore);
		memcpy((char *) newarray + overheadlen + lenbefore,
			   ARR_DATA_PTR(srcArray),
			   newitemsize);
		memcpy((char *) newarray + overheadlen + lenbefore + newitemsize,
			   (char *) array + oldoverheadlen + lenbefore + olditemsize,
			   lenafter);
		/* 若需要填空值位图 */
		if (newhasnulls)
		{
			bits8	   *newnullbitmap = ARR_NULLBITMAP(newarray);
			bits8	   *oldnullbitmap = ARR_NULLBITMAP(array);

			/* 上面的 palloc0 已把插入位置标为空值 */
			array_bitmap_copy(newnullbitmap, addedbefore,
							  oldnullbitmap, 0,
							  itemsbefore);
			array_bitmap_copy(newnullbitmap, lowerIndx[0] - lb[0],
							  ARR_NULLBITMAP(srcArray), 0,
							  nsrcitems);
			array_bitmap_copy(newnullbitmap, addedbefore + itemsbefore + nolditems,
							  oldnullbitmap, itemsbefore + nolditems,
							  itemsafter);
		}
	}

	return PointerGetDatum(newarray);
}

/*
 * array_ref：array_get_element 的向后兼容包装
 *
 * 这只对已解 TOAST/平铺的 varlena 数组有效，
 * 因为数组参数被声明为 "ArrayType *"。不过
 * 代码够多，值得保留 API。
 */
Datum
array_ref(ArrayType *array, int nSubscripts, int *indx,
		  int arraytyplen, int elmlen, bool elmbyval, char elmalign,
		  bool *isNull)
{
	return array_get_element(PointerGetDatum(array), nSubscripts, indx,
							 arraytyplen, elmlen, elmbyval, elmalign,
							 isNull);
}

/*
 * array_set：array_set_element 的向后兼容包装
 *
 * 这只对已解 TOAST/平铺的 varlena 数组有效，
 * 参数与结果都被声明为 "ArrayType *"。不过
 * 代码够多，值得保留 API。
 */
ArrayType *
array_set(ArrayType *array, int nSubscripts, int *indx,
		  Datum dataValue, bool isNull,
		  int arraytyplen, int elmlen, bool elmbyval, char elmalign)
{
	return DatumGetArrayTypeP(array_set_element(PointerGetDatum(array),
												nSubscripts, indx,
												dataValue, isNull,
												arraytyplen,
												elmlen, elmbyval, elmalign));
}

/*
 * array_map()
 *
 * 让一个数组穿过任意表达式。返回一个新数组，
 * 其维度与源相同，且每个源元素都被给定的、
 * 已编译的表达式所转换。每个源元素会被放入
 * ExprState 的 innermost_caseval/innermost_casenull。
 *
 * 参数为:
 * * arrayd: 表示数组参数的 Datum。
 * * exprstate: 表示逐元素转换的 ExprState。
 * * econtext: 求值表达式的上下文。
 * * retType: 输出数组元素类型的 OID。它必须与
 *	 表达式的结果类型相同或二进制兼容。它可能
 *	 与输入数组的元素类型不同。
 * * amstate: array_map 的工作区。调用者必须在
 *	 调用前清零，此后别动。
 *
 * 每次调用都传入一个全新清零的 ArrayMapState
 * 是合法的；但若状态能跨一系列调用保持，则
 * 性能更好。
 *
 * 注意：调用者必须确保输入数组非 NULL。数组
 * NULL 元素则无妨。
 * 注意：调用者应处于 econtext 每元组内存上下文中。
 */
Datum
array_map(Datum arrayd,
		  ExprState *exprstate, ExprContext *econtext,
		  Oid retType, ArrayMapState *amstate)
{
	AnyArrayType *v = DatumGetAnyArrayP(arrayd);
	ArrayType  *result;
	Datum	   *values;
	bool	   *nulls;
	int		   *dim;
	int			ndim;
	int			nitems;
	int			i;
	int32		nbytes = 0;
	int32		dataoffset;
	bool		hasnulls;
	Oid			inpType;
	int			inp_typlen;
	bool		inp_typbyval;
	char		inp_typalign;
	int			typlen;
	bool		typbyval;
	char		typalign;
	array_iter	iter;
	ArrayMetaState *inp_extra;
	ArrayMetaState *ret_extra;
	Datum	   *transform_source = exprstate->innermost_caseval;
	bool	   *transform_source_isnull = exprstate->innermost_casenull;

	inpType = AARR_ELEMTYPE(v);
	ndim = AARR_NDIM(v);
	dim = AARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	/* 检查空数组 */
	if (nitems <= 0)
	{
		/* 返回空数组 */
		return PointerGetDatum(construct_empty_array(retType));
	}

	/*
	 * 对输入与返回元素类型的信息，我们安排每轮
	 * 调用只查找一次，前提是元素类型不在中途
	 * 被更换。
	 */
	inp_extra = &amstate->inp_extra;
	ret_extra = &amstate->ret_extra;

	if (inp_extra->element_type != inpType)
	{
		get_typlenbyvalalign(inpType,
							 &inp_extra->typlen,
							 &inp_extra->typbyval,
							 &inp_extra->typalign);
		inp_extra->element_type = inpType;
	}
	inp_typlen = inp_extra->typlen;
	inp_typbyval = inp_extra->typbyval;
	inp_typalign = inp_extra->typalign;

	if (ret_extra->element_type != retType)
	{
		get_typlenbyvalalign(retType,
							 &ret_extra->typlen,
							 &ret_extra->typbyval,
							 &ret_extra->typalign);
		ret_extra->element_type = retType;
	}
	typlen = ret_extra->typlen;
	typbyval = ret_extra->typbyval;
	typalign = ret_extra->typalign;

	/* 为新值分配临时数组 */
	values = (Datum *) palloc(nitems * sizeof(Datum));
	nulls = (bool *) palloc(nitems * sizeof(bool));

	/* 遍历源数据 */
	array_iter_setup(&iter, v);
	hasnulls = false;

	for (i = 0; i < nitems; i++)
	{
		/* 取得源元素，检查 NULL */
		*transform_source =
			array_iter_next(&iter, transform_source_isnull, i,
							inp_typlen, inp_typbyval, inp_typalign);

		/* 对源元素应用给定表达式 */
		values[i] = ExecEvalExpr(exprstate, econtext, &nulls[i]);

		if (nulls[i])
			hasnulls = true;
		else
		{
			/* 确保数据未被 TOAST */
			if (typlen == -1)
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			/* 更新总结果大小 */
			nbytes = att_addlength_datum(nbytes, typlen, values[i]);
			nbytes = att_align_nominal(nbytes, typalign);
			/* 检查总请求是否溢出 */
			if (!AllocSizeIsValid(nbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
		}
	}

	/* 分配并填充结果数组 */
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* 无空值位图的标记 */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndim;
	result->dataoffset = dataoffset;
	result->elemtype = retType;
	memcpy(ARR_DIMS(result), AARR_DIMS(v), ndim * sizeof(int));
	memcpy(ARR_LBOUND(result), AARR_LBOUND(v), ndim * sizeof(int));

	CopyArrayEls(result,
				 values, nulls, nitems,
				 typlen, typbyval, typalign,
				 false);

	/*
	 * 注意：不要冒险试图 pfree 被调用表达式的结果
	 */
	pfree(values);
	pfree(nulls);

	return PointerGetDatum(result);
}

/*
 * construct_array --- 构造数组对象的简单方法
 *
 * elems: 将成为数组内容的 Datum 项数组
 *		  （不支持 NULL 元素值）。
 * nelems: 项数
 * elmtype, elmlen, elmbyval, elmalign: 各项数据类型的信息
 *
 * 构造并返回一个 palloc 的 1-D 数组对象。注意
 * 即使元素是引用传递类型也会被拷入对象。
 * 另注意 nelems = 0 时结果将是 0-D 而非 1-D。
 *
 * 注意：若给定 elmtype，从系统目录查找
 * elmlen/elmbval/elmalign 信息本会更整洁。不过
 * 调用者更适于把这些信息在多次使用间缓存，
 * 甚至在元素类型硬编码时直接写死值。
 */
ArrayType *
construct_array(Datum *elems, int nelems,
				Oid elmtype,
				int elmlen, bool elmbyval, char elmalign)
{
	int			dims[1];
	int			lbs[1];

	dims[0] = nelems;
	lbs[0] = 1;

	return construct_md_array(elems, NULL, 1, dims, lbs,
							  elmtype, elmlen, elmbyval, elmalign);
}

/*
 * 类似 construct_array()，但 elmtype 必须是内建
 * 类型，且 elmlen/elmbyval/elmalign 从硬编码数
 * 据查找。操纵系统目录的数组时很有用。
 */
ArrayType *
construct_array_builtin(Datum *elems, int nelems, Oid elmtype)
{
	int			elmlen;
	bool		elmbyval;
	char		elmalign;

	switch (elmtype)
	{
		case CHAROID:
			elmlen = 1;
			elmbyval = true;
			elmalign = TYPALIGN_CHAR;
			break;

		case CSTRINGOID:
			elmlen = -2;
			elmbyval = false;
			elmalign = TYPALIGN_CHAR;
			break;

		case FLOAT4OID:
			elmlen = sizeof(float4);
			elmbyval = true;
			elmalign = TYPALIGN_INT;
			break;

		case INT2OID:
			elmlen = sizeof(int16);
			elmbyval = true;
			elmalign = TYPALIGN_SHORT;
			break;

		case INT4OID:
			elmlen = sizeof(int32);
			elmbyval = true;
			elmalign = TYPALIGN_INT;
			break;

		case INT8OID:
			elmlen = sizeof(int64);
			elmbyval = FLOAT8PASSBYVAL;
			elmalign = TYPALIGN_DOUBLE;
			break;

		case NAMEOID:
			elmlen = NAMEDATALEN;
			elmbyval = false;
			elmalign = TYPALIGN_CHAR;
			break;

		case OIDOID:
		case REGTYPEOID:
			elmlen = sizeof(Oid);
			elmbyval = true;
			elmalign = TYPALIGN_INT;
			break;

		case TEXTOID:
			elmlen = -1;
			elmbyval = false;
			elmalign = TYPALIGN_INT;
			break;

		case TIDOID:
			elmlen = sizeof(ItemPointerData);
			elmbyval = false;
			elmalign = TYPALIGN_SHORT;
			break;

		default:
			elog(ERROR, "type %u not supported by construct_array_builtin()", elmtype);
			/* 让编译器安静 */
			elmlen = 0;
			elmbyval = false;
			elmalign = 0;
	}

	return construct_array(elems, nelems, elmtype, elmlen, elmbyval, elmalign);
}

/*
 * construct_md_array --- 构造具有任意维度、
 *							可能含 NULL 数组对象的简单方法
 *
 * elems: 将成为数组内容的 Datum 项数组
 * nulls: 空值标志数组（无空值时可为 NULL）
 * ndims: 维度数
 * dims: 整数数组，给出每维大小
 * lbs: 整数数组，给出每维下界
 * elmtype, elmlen, elmbyval, elmalign: 各项数据类型的信息
 *
 * 构造并返回一个 palloc 的 ndims-D 数组对象。注意
 * 即使元素是引用传递类型也会被拷入对象。
 * 另注意任一 dims[i]=0 时结果将是 0-D 而非 ndims-D。
 *
 * 注意：若给定 elmtype，从系统目录查找
 * elmlen/elmbval/elmalign 信息本会更整洁。不过
 * 调用者更适于把这些信息在多次使用间缓存，
 * 甚至在元素类型硬编码时直接写死值。
 */
ArrayType *
construct_md_array(Datum *elems,
				   bool *nulls,
				   int ndims,
				   int *dims,
				   int *lbs,
				   Oid elmtype, int elmlen, bool elmbyval, char elmalign)
{
	ArrayType  *result;
	bool		hasnulls;
	int32		nbytes;
	int32		dataoffset;
	int			i;
	int			nelems;

	if (ndims < 0)				/* 我们确实允许零维度数组 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid number of dimensions: %d", ndims)));
	if (ndims > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						ndims, MAXDIM)));

	/* 此处检查数组维度是否溢出 */
	nelems = ArrayGetNItems(ndims, dims);
	ArrayCheckBounds(ndims, dims, lbs);

	/* ndims <= 0 或 dims[i] == 0 时返回空数组 */
	if (nelems <= 0)
		return construct_empty_array(elmtype);

	/* 计算所需空间 */
	nbytes = 0;
	hasnulls = false;
	for (i = 0; i < nelems; i++)
	{
		if (nulls && nulls[i])
		{
			hasnulls = true;
			continue;
		}
		/* 确保数据未被 TOAST */
		if (elmlen == -1)
			elems[i] = PointerGetDatum(PG_DETOAST_DATUM(elems[i]));
		nbytes = att_addlength_datum(nbytes, elmlen, elems[i]);
		nbytes = att_align_nominal(nbytes, elmalign);
		/* 检查总请求是否溢出 */
		if (!AllocSizeIsValid(nbytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxAllocSize)));
	}

	/* 分配并初始化结果数组 */
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nelems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* 无空值位图的标记 */
		nbytes += ARR_OVERHEAD_NONULLS(ndims);
	}
	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndims;
	result->dataoffset = dataoffset;
	result->elemtype = elmtype;
	memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));

	CopyArrayEls(result,
				 elems, nulls, nelems,
				 elmlen, elmbyval, elmalign,
				 false);

	return result;
}

/*
 * construct_empty_array --- 构造给定类型的零维数组
 */
ArrayType *
construct_empty_array(Oid elmtype)
{
	ArrayType  *result;

	result = (ArrayType *) palloc0(sizeof(ArrayType));
	SET_VARSIZE(result, sizeof(ArrayType));
	result->ndim = 0;
	result->dataoffset = 0;
	result->elemtype = elmtype;
	return result;
}

/*
 * construct_empty_expanded_array: 仅凭类型信息
 * 构造空的展开数组。（metacache 无需时为 NULL）。
 */
ExpandedArrayHeader *
construct_empty_expanded_array(Oid element_type,
							   MemoryContext parentcontext,
							   ArrayMetaState *metacache)
{
	ArrayType  *array = construct_empty_array(element_type);
	Datum		d;

	d = expand_array(PointerGetDatum(array), parentcontext, metacache);
	pfree(array);
	return (ExpandedArrayHeader *) DatumGetEOHP(d);
}

/*
 * deconstruct_array --- 从数组提取数据的简单方法
 *
 * array: 要检查数组对象（不得为 NULL）
 * elmtype, elmlen, elmbyval, elmalign: 各项数据类型的信息
 * elemssp: 返回值，设为指向 palloc 的 Datum 值数组
 * nullsp: 返回值，设为指向 palloc 的空值标记数组
 * nelemsp: 返回值，设为提取出的值的个数
 *
 * 若调用者不支持数组含 NULL，可传 nullsp ==
 * NULL。注意这会产生一条信息量很低的错误消
 * 息，仅在确实不预期 NULL 时才这么做。
 *
 * 若数组元素是引用传递数据类型，返回的 Datum
 * 是指向数组对象的指针。
 *
 * 注意：若给定 elmtype，从系统目录查找
 * elmlen/elmbval/elmalign 信息本会更整洁。不过
 * 调用者更适于把这些信息在多次使用间缓存，
 * 甚至在元素类型硬编码时直接写死值。
 */
void
deconstruct_array(ArrayType *array,
				  Oid elmtype,
				  int elmlen, bool elmbyval, char elmalign,
				  Datum **elemsp, bool **nullsp, int *nelemsp)
{
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	char	   *p;
	bits8	   *bitmap;
	int			bitmask;
	int			i;

	Assert(ARR_ELEMTYPE(array) == elmtype);

	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));
	*elemsp = elems = (Datum *) palloc(nelems * sizeof(Datum));
	if (nullsp)
		*nullsp = nulls = (bool *) palloc0(nelems * sizeof(bool));
	else
		nulls = NULL;
	*nelemsp = nelems;

	p = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;

	for (i = 0; i < nelems; i++)
	{
		/* 取得源元素，检查 NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			elems[i] = (Datum) 0;
			if (nulls)
				nulls[i] = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("null array element not allowed in this context")));
		}
		else
		{
			elems[i] = fetch_att(p, elmbyval, elmlen);
			p = att_addlength_pointer(p, elmlen, p);
			p = (char *) att_align_nominal(p, elmalign);
		}

		/* 若有则前移位图 */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}
}

/*
 * 类似 deconstruct_array()，但 elmtype 必须是内建
 * 类型，且 elmlen/elmbyval/elmalign 从硬编码数
 * 据查找。操纵系统目录的数组时很有用。
 */
void
deconstruct_array_builtin(ArrayType *array,
						  Oid elmtype,
						  Datum **elemsp, bool **nullsp, int *nelemsp)
{
	int			elmlen;
	bool		elmbyval;
	char		elmalign;

	switch (elmtype)
	{
		case CHAROID:
			elmlen = 1;
			elmbyval = true;
			elmalign = TYPALIGN_CHAR;
			break;

		case CSTRINGOID:
			elmlen = -2;
			elmbyval = false;
			elmalign = TYPALIGN_CHAR;
			break;

		case FLOAT8OID:
			elmlen = sizeof(float8);
			elmbyval = FLOAT8PASSBYVAL;
			elmalign = TYPALIGN_DOUBLE;
			break;

		case INT2OID:
			elmlen = sizeof(int16);
			elmbyval = true;
			elmalign = TYPALIGN_SHORT;
			break;

		case OIDOID:
			elmlen = sizeof(Oid);
			elmbyval = true;
			elmalign = TYPALIGN_INT;
			break;

		case TEXTOID:
			elmlen = -1;
			elmbyval = false;
			elmalign = TYPALIGN_INT;
			break;

		case TIDOID:
			elmlen = sizeof(ItemPointerData);
			elmbyval = false;
			elmalign = TYPALIGN_SHORT;
			break;

		default:
			elog(ERROR, "type %u not supported by deconstruct_array_builtin()", elmtype);
			/* 让编译器安静 */
			elmlen = 0;
			elmbyval = false;
			elmalign = 0;
	}

	deconstruct_array(array, elmtype, elmlen, elmbyval, elmalign, elemsp, nullsp, nelemsp);
}

/*
 * array_contains_nulls --- 检测数组是否含任何空元素
 *
 * 这给出的是准确答案，而只测 ARR_HASNULL 则
 * 仅说明数组*可能*含空值。
 */
bool
array_contains_nulls(ArrayType *array)
{
	int			nelems;
	bits8	   *bitmap;
	int			bitmask;

	/* 无空值位图则答案简单 */
	if (!ARR_HASNULL(array))
		return false;

	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

	bitmap = ARR_NULLBITMAP(array);

	/* 按整字节逐个检查位图的字节 */
	while (nelems >= 8)
	{
		if (*bitmap != 0xFF)
			return true;
		bitmap++;
		nelems -= 8;
	}

	/* 查残余字节 */
	bitmask = 1;
	while (nelems > 0)
	{
		if ((*bitmap & bitmask) == 0)
			return true;
		bitmask <<= 1;
		nelems--;
	}

	return false;
}


/*
 * array_eq :
 *		  比较两个数组是否相等
 * 结果 :
 *		  数组相等时返回 true，否则返回 false。
 *
 * 注意：这里不使用 array_cmp，因为在没有全序
 * （因而无 btree 支持）的数据类型中相等可能有意义。
 */
Datum
array_eq(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 2);
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	int			ndims1 = AARR_NDIM(array1);
	int			ndims2 = AARR_NDIM(array2);
	int		   *dims1 = AARR_DIMS(array1);
	int		   *dims2 = AARR_DIMS(array2);
	int		   *lbs1 = AARR_LBOUND(array1);
	int		   *lbs2 = AARR_LBOUND(array2);
	Oid			element_type = AARR_ELEMTYPE(array1);
	bool		result = true;
	int			nitems;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	array_iter	it1;
	array_iter	it2;
	int			i;

	if (element_type != AARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/* 若两数组维度数不同则走快速路径 */
	if (ndims1 != ndims2 ||
		memcmp(dims1, dims2, ndims1 * sizeof(int)) != 0 ||
		memcmp(lbs1, lbs2, ndims1 * sizeof(int)) != 0)
		result = false;
	else
	{
		/*
		 * 相等函数我们安排每轮调用只查找一次，前提
		 * 是元素类型不在中途被更换。使用 typcache 是
		 * 为了在被用作索引支持函数时不发生内存泄
		 * 漏。
		 */
		typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
		if (typentry == NULL ||
			typentry->type_id != element_type)
		{
			typentry = lookup_type_cache(element_type,
										 TYPECACHE_EQ_OPR_FINFO);
			if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify an equality operator for type %s",
								format_type_be(element_type))));
			fcinfo->flinfo->fn_extra = (void *) typentry;
		}
		typlen = typentry->typlen;
		typbyval = typentry->typbyval;
		typalign = typentry->typalign;

		/*
		 * 把该操作符应用于每一对数组元素。
		 */
		InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
								 collation, NULL, NULL);

		/* 遍历源数据 */
		nitems = ArrayGetNItems(ndims1, dims1);
		array_iter_setup(&it1, array1);
		array_iter_setup(&it2, array2);

		for (i = 0; i < nitems; i++)
		{
			Datum		elt1;
			Datum		elt2;
			bool		isnull1;
			bool		isnull2;
			bool		oprresult;

			/* 取得元素，检查 NULL */
			elt1 = array_iter_next(&it1, &isnull1, i,
								   typlen, typbyval, typalign);
			elt2 = array_iter_next(&it2, &isnull2, i,
								   typlen, typbyval, typalign);

			/*
			 * 我们认为两个 NULL 相等；NULL 与非 NULL 不等。
			 */
			if (isnull1 && isnull2)
				continue;
			if (isnull1 || isnull2)
			{
				result = false;
				break;
			}

			/*
			 * 把操作符应用于元素对；把 NULL 当作假
			 */
			locfcinfo->args[0].value = elt1;
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = elt2;
			locfcinfo->args[1].isnull = false;
			locfcinfo->isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
			if (locfcinfo->isnull || !oprresult)
			{
				result = false;
				break;
			}
		}
	}

	/* 传入 TOAST 输入时避免内存泄漏。 */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}


/*-----------------------------------------------------------------------------
 * 数组间布尔操作符:
 *		给定两个数组，在其上迭代比较操
 *		作符，使用类似文本比较的逻辑，
 *		只是逐元素而非
 *		逐字符。
 *----------------------------------------------------------------------------
 */

Datum
array_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!DatumGetBool(array_eq(fcinfo)));
}

Datum
array_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) < 0);
}

Datum
array_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) > 0);
}

Datum
array_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) <= 0);
}

Datum
array_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) >= 0);
}

Datum
btarraycmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(array_cmp(fcinfo));
}

/*
 * array_cmp()
 * 数组的内部比较函数。
 *
 * 返回 -1、0、1
 */
static int
array_cmp(FunctionCallInfo fcinfo)
{
	LOCAL_FCINFO(locfcinfo, 2);
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	int			ndims1 = AARR_NDIM(array1);
	int			ndims2 = AARR_NDIM(array2);
	int		   *dims1 = AARR_DIMS(array1);
	int		   *dims2 = AARR_DIMS(array2);
	int			nitems1 = ArrayGetNItems(ndims1, dims1);
	int			nitems2 = ArrayGetNItems(ndims2, dims2);
	Oid			element_type = AARR_ELEMTYPE(array1);
	int			result = 0;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			min_nitems;
	array_iter	it1;
	array_iter	it2;
	int			i;

	if (element_type != AARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/*
	 * 比较函数我们安排每轮调用只查找一次，前提
	 * 是元素类型不在中途被更换。使用 typcache 是
	 * 为了在被用作索引支持函数时不发生内存泄
	 * 漏。
	 */
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_CMP_PROC_FINFO);
		if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a comparison function for type %s",
							format_type_be(element_type))));
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}
	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	/*
	 * 把该操作符应用于每一对数组元素。
	 */
	InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
							 collation, NULL, NULL);

	/* 遍历源数据 */
	min_nitems = Min(nitems1, nitems2);
	array_iter_setup(&it1, array1);
	array_iter_setup(&it2, array2);

	for (i = 0; i < min_nitems; i++)
	{
		Datum		elt1;
		Datum		elt2;
		bool		isnull1;
		bool		isnull2;
		int32		cmpresult;

		/* 取得元素，检查 NULL */
		elt1 = array_iter_next(&it1, &isnull1, i, typlen, typbyval, typalign);
		elt2 = array_iter_next(&it2, &isnull2, i, typlen, typbyval, typalign);

		/*
		 * 两个 NULL 相等；NULL > 非 NULL。
		 */
		if (isnull1 && isnull2)
			continue;
		if (isnull1)
		{
			/* arg1 大于 arg2 */
			result = 1;
			break;
		}
		if (isnull2)
		{
			/* arg1 小于 arg2 */
			result = -1;
			break;
		}

		/* 比较这对元素 */
		locfcinfo->args[0].value = elt1;
		locfcinfo->args[0].isnull = false;
		locfcinfo->args[1].value = elt2;
		locfcinfo->args[1].isnull = false;
		cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

		/* 我们不期望比较支持函数返回 null */
		Assert(!locfcinfo->isnull);

		if (cmpresult == 0)
			continue;			/* equal */

		if (cmpresult < 0)
		{
			/* arg1 小于 arg2 */
			result = -1;
			break;
		}
		else
		{
			/* arg1 大于 arg2 */
			result = 1;
			break;
		}
	}

	/*
	 * 若两数组数据相同（到较短者为止），则应用
	 * 按维度数排序的附加规则。各信息位之间的相
	 * 对重要性是历史原因；主要我们只在乎不对维
	 * 度数不同的数组说"相等"。
	 */
	if (result == 0)
	{
		if (nitems1 != nitems2)
			result = (nitems1 < nitems2) ? -1 : 1;
		else if (ndims1 != ndims2)
			result = (ndims1 < ndims2) ? -1 : 1;
		else
		{
			for (i = 0; i < ndims1; i++)
			{
				if (dims1[i] != dims2[i])
				{
					result = (dims1[i] < dims2[i]) ? -1 : 1;
					break;
				}
			}
			if (result == 0)
			{
				int		   *lbound1 = AARR_LBOUND(array1);
				int		   *lbound2 = AARR_LBOUND(array2);

				for (i = 0; i < ndims1; i++)
				{
					if (lbound1[i] != lbound2[i])
					{
						result = (lbound1[i] < lbound2[i]) ? -1 : 1;
						break;
					}
				}
			}
		}
	}

	/* 传入 TOAST 输入时避免内存泄漏。 */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	return result;
}


/*-----------------------------------------------------------------------------
 * 数组哈希
 *		对各元素哈希并合并结果。
 *----------------------------------------------------------------------------
 */

Datum
hash_array(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 1);
	AnyArrayType *array = PG_GETARG_ANY_ARRAY_P(0);
	int			ndims = AARR_NDIM(array);
	int		   *dims = AARR_DIMS(array);
	Oid			element_type = AARR_ELEMTYPE(array);
	uint32		result = 1;
	int			nitems;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			i;
	array_iter	iter;

	/*
	 * 哈希函数我们安排每轮调用只查找一次，前提
	 * 是元素类型不在中途被更换。使用 typcache 是
	 * 为了在被用作索引支持函数时不发生内存泄
	 * 漏。
	 */
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(typentry->hash_proc_finfo.fn_oid) && element_type != RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(element_type))));

		/*
		 * 类型缓存不相信 record 可哈希（见
		 * cache_record_field_properties()），但既然走到了
		 * 这里，我们就已承诺要哈希，因此可假定它可哈
		 * 希。最坏情形，若 record 任一成分不支持哈希则
		 * 失败。
		 */
		if (element_type == RECORDOID)
		{
			MemoryContext oldcontext;
			TypeCacheEntry *record_typentry;

			oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

			/*
			 * 构造伪类型缓存表项结构。注意我们不能只修
			 * 改 typentry，因为它直接指向类型缓存。
			 *
			 */
			record_typentry = palloc0(sizeof(*record_typentry));
			record_typentry->type_id = element_type;

			/* 填入下面所需项 */
			record_typentry->typlen = typentry->typlen;
			record_typentry->typbyval = typentry->typbyval;
			record_typentry->typalign = typentry->typalign;
			fmgr_info(F_HASH_RECORD, &record_typentry->hash_proc_finfo);

			MemoryContextSwitchTo(oldcontext);

			typentry = record_typentry;
		}

		fcinfo->flinfo->fn_extra = (void *) typentry;
	}

	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	/*
	 * 把哈希函数应用于每个数组元素。
	 */
	InitFunctionCallInfoData(*locfcinfo, &typentry->hash_proc_finfo, 1,
							 PG_GET_COLLATION(), NULL, NULL);

	/* 遍历源数据 */
	nitems = ArrayGetNItems(ndims, dims);
	array_iter_setup(&iter, array);

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		bool		isnull;
		uint32		elthash;

		/* 取得元素，检查 NULL */
		elt = array_iter_next(&iter, &isnull, i, typlen, typbyval, typalign);

		if (isnull)
		{
			/* 把空值视为哈希值为 0 */
			elthash = 0;
		}
		else
		{
			/* 应用哈希函数 */
			locfcinfo->args[0].value = elt;
			locfcinfo->args[0].isnull = false;
			elthash = DatumGetUInt32(FunctionCallInvoke(locfcinfo));
			/* 我们不期望哈希函数返回 null */
			Assert(!locfcinfo->isnull);
		}

		/*
		 * 通过把当前值乘以 31 并加上新元素的哈希值，
		 * 来合并相继元素的哈希值。
		 *
		 * 结果是一个和，每个元素的哈希值被乘
		 * 以 31 的不同次幂。这是模 2^32 算术，31 的
		 * 次幂模 2^32 构成一个 2^27 阶的循环群。因此对
		 * 最多 2^27 个元素的数组，每个元素的哈希值被
		 * 乘以一个不同的（奇）数，从而使所有元素的
		 * 哈希值得到良好的混合。
		 */
		result = (result << 5) - result + elthash;
	}

	/* 传入 TOAST 输入时避免内存泄漏。 */
	AARR_FREE_IF_COPY(array, 0);

	PG_RETURN_UINT32(result);
}

/*
 * 通过对一个值哈希得到 64 位值（带种子）。
 * 其余与 hash_array 相似。
 */
Datum
hash_array_extended(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 2);
	AnyArrayType *array = PG_GETARG_ANY_ARRAY_P(0);
	uint64		seed = PG_GETARG_INT64(1);
	int			ndims = AARR_NDIM(array);
	int		   *dims = AARR_DIMS(array);
	Oid			element_type = AARR_ELEMTYPE(array);
	uint64		result = 1;
	int			nitems;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			i;
	array_iter	iter;

	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_HASH_EXTENDED_PROC_FINFO);
		if (!OidIsValid(typentry->hash_extended_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an extended hash function for type %s",
							format_type_be(element_type))));
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}
	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	InitFunctionCallInfoData(*locfcinfo, &typentry->hash_extended_proc_finfo, 2,
							 PG_GET_COLLATION(), NULL, NULL);

	/* 遍历源数据 */
	nitems = ArrayGetNItems(ndims, dims);
	array_iter_setup(&iter, array);

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		bool		isnull;
		uint64		elthash;

		/* 取得元素，检查 NULL */
		elt = array_iter_next(&iter, &isnull, i, typlen, typbyval, typalign);

		if (isnull)
		{
			elthash = 0;
		}
		else
		{
			/* 应用哈希函数 */
			locfcinfo->args[0].value = elt;
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = Int64GetDatum(seed);
			locfcinfo->args[1].isnull = false;
			elthash = DatumGetUInt64(FunctionCallInvoke(locfcinfo));
			/* 我们不期望哈希函数返回 null */
			Assert(!locfcinfo->isnull);
		}

		result = (result << 5) - result + elthash;
	}

	AARR_FREE_IF_COPY(array, 0);

	PG_RETURN_UINT64(result);
}


/*-----------------------------------------------------------------------------
 * 数组重叠/包含比较
 *		这些使用与 array_eq 相同的数组元素比较方法。
 *		我们只考虑数组的元素，忽略维度数。
 *----------------------------------------------------------------------------
 */

/*
 * array_contain_compare :
 *		  比较两个数组的重叠/包含
 *
 * matchall 为真时，array1 全部成员在 array2 中则返回真；
 * matchall 为假时，array1 任一成员在 array2 中即返回真。
 */
static bool
array_contain_compare(AnyArrayType *array1, AnyArrayType *array2, Oid collation,
					  bool matchall, void **fn_extra)
{
	LOCAL_FCINFO(locfcinfo, 2);
	bool		result = matchall;
	Oid			element_type = AARR_ELEMTYPE(array1);
	TypeCacheEntry *typentry;
	int			nelems1;
	Datum	   *values2;
	bool	   *nulls2;
	int			nelems2;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			i;
	int			j;
	array_iter	it1;

	if (element_type != AARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/*
	 * 相等函数我们安排每轮调用只查找一次，前提
	 * 是元素类型不在中途被更换。使用 typcache 是
	 * 为了在被用作索引支持函数时不发生内存泄
	 * 漏。
	 */
	typentry = (TypeCacheEntry *) *fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an equality operator for type %s",
							format_type_be(element_type))));
		*fn_extra = (void *) typentry;
	}
	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	/*
	 * 因为我们可能要多次扫描 array2，对它使用
	 * deconstruct_array 是值得的。不过 array1 我们用
	 * 笨办法扫描，因为我们可能不需要看它全部。
	 */
	if (VARATT_IS_EXPANDED_HEADER(array2))
	{
		/* 即使输入是只读的这也应当安全 */
		deconstruct_expanded_array(&(array2->xpn));
		values2 = array2->xpn.dvalues;
		nulls2 = array2->xpn.dnulls;
		nelems2 = array2->xpn.nelems;
	}
	else
		deconstruct_array((ArrayType *) array2,
						  element_type, typlen, typbyval, typalign,
						  &values2, &nulls2, &nelems2);

	/*
	 * 把比较操作符应用于每一对数组元素。
	 */
	InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
							 collation, NULL, NULL);

	/* 遍历源数据 */
	nelems1 = ArrayGetNItems(AARR_NDIM(array1), AARR_DIMS(array1));
	array_iter_setup(&it1, array1);

	for (i = 0; i < nelems1; i++)
	{
		Datum		elt1;
		bool		isnull1;

		/* 取得元素，检查 NULL */
		elt1 = array_iter_next(&it1, &isnull1, i, typlen, typbyval, typalign);

		/*
		 * 我们假定比较操作符是严格的，因此 NULL 无法
		 * 匹配任何东西。XXX 这与 array_eq 的 "NULL=NULL"
		 * 行为有分歧，该照做吗?
		 */
		if (isnull1)
		{
			if (matchall)
			{
				result = false;
				break;
			}
			continue;
		}

		for (j = 0; j < nelems2; j++)
		{
			Datum		elt2 = values2[j];
			bool		isnull2 = nulls2 ? nulls2[j] : false;
			bool		oprresult;

			if (isnull2)
				continue;		/* 不匹配 */

			/*
			 * 把操作符应用于元素对；把 NULL 当作假
			 */
			locfcinfo->args[0].value = elt1;
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = elt2;
			locfcinfo->args[1].isnull = false;
			locfcinfo->isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
			if (!locfcinfo->isnull && oprresult)
				break;
		}

		if (j < nelems2)
		{
			/* 为 elt1 找到匹配 */
			if (!matchall)
			{
				result = true;
				break;
			}
		}
		else
		{
			/* elt1 无匹配 */
			if (matchall)
			{
				result = false;
				break;
			}
		}
	}

	return result;
}

Datum
arrayoverlap(PG_FUNCTION_ARGS)
{
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	bool		result;

	result = array_contain_compare(array1, array2, collation, false,
								   &fcinfo->flinfo->fn_extra);

	/* 传入 TOAST 输入时避免内存泄漏。 */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}

Datum
arraycontains(PG_FUNCTION_ARGS)
{
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	bool		result;

	result = array_contain_compare(array2, array1, collation, true,
								   &fcinfo->flinfo->fn_extra);

	/* 传入 TOAST 输入时避免内存泄漏。 */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}

Datum
arraycontained(PG_FUNCTION_ARGS)
{
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	bool		result;

	result = array_contain_compare(array1, array2, collation, true,
								   &fcinfo->flinfo->fn_extra);

	/* 传入 TOAST 输入时避免内存泄漏。 */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}


/*-----------------------------------------------------------------------------
 * 数组迭代函数
 *		这些函数用于高效地遍历数组
 *-----------------------------------------------------------------------------
 */

/*
 * array_create_iterator --- 准备遍历一个数组
 *
 * 若 slice_ndim 为零，我们将逐元素迭代；返回的
 * datum 属于数组的元素类型。
 *
 * 若 slice_ndim 为 1..ARR_NDIM(arr)，我们将按切
 * 片迭代：返回的 datum 与 'arr' 属于相同的数组
 * 类型，大小为 'arr' 最右 N 维。
 *
 * 传入的数组必须在迭代器的整个生命期内保持有效。
 */
ArrayIterator
array_create_iterator(ArrayType *arr, int slice_ndim, ArrayMetaState *mstate)
{
	ArrayIterator iterator = palloc0(sizeof(ArrayIteratorData));

	/*
	 * 对输入做合理性检查——调用者应早已做对
	 */
	Assert(PointerIsValid(arr));
	if (slice_ndim < 0 || slice_ndim > ARR_NDIM(arr))
		elog(ERROR, "invalid arguments to array_create_iterator");

	/*
	 * 记住数组及其元素类型的基本信息
	 */
	iterator->arr = arr;
	iterator->nullbitmap = ARR_NULLBITMAP(arr);
	iterator->nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

	if (mstate != NULL)
	{
		Assert(mstate->element_type == ARR_ELEMTYPE(arr));

		iterator->typlen = mstate->typlen;
		iterator->typbyval = mstate->typbyval;
		iterator->typalign = mstate->typalign;
	}
	else
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &iterator->typlen,
							 &iterator->typbyval,
							 &iterator->typalign);

	/*
	 * 记住切片参数。
	 */
	iterator->slice_ndim = slice_ndim;

	if (slice_ndim > 0)
	{
		/*
		 * 取得指向数组 dims 与 lbound 数组的指针来表
		 * 示切片的 dims/lbound 数组。它们与数组的最
		 * 右 N 个维度相同。
		 */
		iterator->slice_dims = ARR_DIMS(arr) + ARR_NDIM(arr) - slice_ndim;
		iterator->slice_lbound = ARR_LBOUND(arr) + ARR_NDIM(arr) - slice_ndim;

		/*
		 * 计算切片的元素数。
		 */
		iterator->slice_len = ArrayGetNItems(slice_ndim,
											 iterator->slice_dims);

		/*
		 * 创建构建子数组的工作区。
		 */
		iterator->slice_values = (Datum *)
			palloc(iterator->slice_len * sizeof(Datum));
		iterator->slice_nulls = (bool *)
			palloc(iterator->slice_len * sizeof(bool));
	}

	/*
	 * 初始化我们的数据指针与线性元素号。它们将
	 * 在 array_iterate() 期间在数组中前进。
	 */
	iterator->data_ptr = ARR_DATA_PTR(arr);
	iterator->current_item = 0;

	return iterator;
}

/*
 * 遍历 'iterator' 所引用的数组。
 *
 * 只要还有下一个元素（或切片），就把它放入
 * *value / *isnull 并返回 true。没有更多数据时返回假。
 */
bool
array_iterate(ArrayIterator iterator, Datum *value, bool *isnull)
{
	/* 若已到达数组末尾则完成 */
	if (iterator->current_item >= iterator->nitems)
		return false;

	if (iterator->slice_ndim == 0)
	{
		/*
		 * 标量：返回一个元素。
		 */
		if (array_get_isnull(iterator->nullbitmap, iterator->current_item++))
		{
			*isnull = true;
			*value = (Datum) 0;
		}
		else
		{
			/* 非 NULL，取回要返回的单个 Datum */
			char	   *p = iterator->data_ptr;

			*isnull = false;
			*value = fetch_att(p, iterator->typbyval, iterator->typlen);

			/* 把数据指针前移到下一元素 */
			p = att_addlength_pointer(p, iterator->typlen, p);
			p = (char *) att_align_nominal(p, iterator->typalign);
			iterator->data_ptr = p;
		}
	}
	else
	{
		/*
		 * 切片情形：构建并返回请求大小的数组。
		 */
		ArrayType  *result;
		Datum	   *values = iterator->slice_values;
		bool	   *nulls = iterator->slice_nulls;
		char	   *p = iterator->data_ptr;
		int			i;

		for (i = 0; i < iterator->slice_len; i++)
		{
			if (array_get_isnull(iterator->nullbitmap,
								 iterator->current_item++))
			{
				nulls[i] = true;
				values[i] = (Datum) 0;
			}
			else
			{
				nulls[i] = false;
				values[i] = fetch_att(p, iterator->typbyval, iterator->typlen);

				/* 把数据指针前移到下一元素 */
				p = att_addlength_pointer(p, iterator->typlen, p);
				p = (char *) att_align_nominal(p, iterator->typalign);
			}
		}

		iterator->data_ptr = p;

		result = construct_md_array(values,
									nulls,
									iterator->slice_ndim,
									iterator->slice_dims,
									iterator->slice_lbound,
									ARR_ELEMTYPE(iterator->arr),
									iterator->typlen,
									iterator->typbyval,
									iterator->typalign);

		*isnull = false;
		*value = PointerGetDatum(result);
	}

	return true;
}

/*
 * 释放一个 ArrayIterator 数据结构
 */
void
array_free_iterator(ArrayIterator iterator)
{
	if (iterator->slice_ndim > 0)
	{
		pfree(iterator->slice_values);
		pfree(iterator->slice_nulls);
	}
	pfree(iterator);
}


/***************************************************************************/
/******************|		  Support  Routines			  |*****************/
/***************************************************************************/

/*
 * 检查特定数组元素是否为 NULL
 *
 * nullbitmap: 数组空值位图指针（无则 NULL）
 * offset: 数组元素的 0 基线性元素号
 */
static bool
array_get_isnull(const bits8 *nullbitmap, int offset)
{
	if (nullbitmap == NULL)
		return false;			/* 假定非空 */
	if (nullbitmap[offset / 8] & (1 << (offset % 8)))
		return false;			/* 非空 */
	return true;
}

/*
 * 设置特定数组元素的空位图项
 *
 * nullbitmap: 数组空值位图指针（不得为 NULL）
 * offset: 数组元素的 0 基线性元素号
 * isNull: 要设的空状态
 */
static void
array_set_isnull(bits8 *nullbitmap, int offset, bool isNull)
{
	int			bitmask;

	nullbitmap += offset / 8;
	bitmask = 1 << (offset % 8);
	if (isNull)
		*nullbitmap &= ~bitmask;
	else
		*nullbitmap |= bitmask;
}

/*
 * 取得指针处的数组元素，正确转换为 Datum
 *
 * 调用者必须已处理 NULL 元素情形
 */
static Datum
ArrayCast(char *value, bool byval, int len)
{
	return fetch_att(value, byval, len);
}

/*
 * 把 datum 拷贝到 *dest 并返回所用总空间（含对齐填充）
 *
 * 调用者必须已处理 NULL 元素情形
 */
static int
ArrayCastAndSet(Datum src,
				int typlen,
				bool typbyval,
				char typalign,
				char *dest)
{
	int			inc;

	if (typlen > 0)
	{
		if (typbyval)
			store_att_byval(dest, src, typlen);
		else
			memmove(dest, DatumGetPointer(src), typlen);
		inc = att_align_nominal(typlen, typalign);
	}
	else
	{
		Assert(!typbyval);
		inc = att_addlength_datum(0, typlen, src);
		memmove(dest, DatumGetPointer(src), inc);
		inc = att_align_nominal(inc, typalign);
	}

	return inc;
}

/*
 * 让 ptr 前进 nitems 个数组元素
 *
 * ptr: 数组中的起始位置
 * offset: 第一个元素（位于 *ptr 处的那个）的 0
 * 基线性元素号 nullbitmap: 数组空值位图起点
 * 无则 NULL；nitems: 要前进的元素个数（>=0）
 * typlen, typbyval, typalign: 数据类型的存储参数
 *
 * 确保 nitems 在范围内是调用者的责任
 */
static char *
array_seek(char *ptr, int offset, bits8 *nullbitmap, int nitems,
		   int typlen, bool typbyval, char typalign)
{
	int			bitmask;
	int			i;

	/* 定长元素且无 NULL 时很简单 */
	if (typlen > 0 && !nullbitmap)
		return ptr + nitems * ((Size) att_align_nominal(typlen, typalign));

	/* NULL 与非 NULL 情形分设循环看来值得 */
	if (nullbitmap)
	{
		nullbitmap += offset / 8;
		bitmask = 1 << (offset % 8);

		for (i = 0; i < nitems; i++)
		{
			if (*nullbitmap & bitmask)
			{
				ptr = att_addlength_pointer(ptr, typlen, ptr);
				ptr = (char *) att_align_nominal(ptr, typalign);
			}
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				nullbitmap++;
				bitmask = 1;
			}
		}
	}
	else
	{
		for (i = 0; i < nitems; i++)
		{
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			ptr = (char *) att_align_nominal(ptr, typalign);
		}
	}
	return ptr;
}

/*
 * 计算 *ptr 起始的 nitems 个数组元素的总大小
 *
 * 参数与 array_seek 相同
 */
static int
array_nelems_size(char *ptr, int offset, bits8 *nullbitmap, int nitems,
				  int typlen, bool typbyval, char typalign)
{
	return array_seek(ptr, offset, nullbitmap, nitems,
					  typlen, typbyval, typalign) - ptr;
}

/*
 * 把 nitems 个元素从 srcptr 拷到 destptr
 *
 * destptr: 目标起始位置（必须有足够空间！）
 * nitems: 要拷贝的数组元素个数（>=0）
 * srcptr: 源数组中的起始位置
 * offset: 首元素（*srcptr 处）的 0 基线性元素号
 * nullbitmap: 源空值位图起点或 NULL
 * typlen, typbyval, typalign: 数据类型的存储参数
 *
 * 返回拷贝的字节数
 *
 * 注意：此函数不负责设置目标的空值位图！
 */
static int
array_copy(char *destptr, int nitems,
		   char *srcptr, int offset, bits8 *nullbitmap,
		   int typlen, bool typbyval, char typalign)
{
	int			numbytes;

	numbytes = array_nelems_size(srcptr, offset, nullbitmap, nitems,
								 typlen, typbyval, typalign);
	memcpy(destptr, srcptr, numbytes);
	return numbytes;
}

/*
 * 把 nitems 个空值位图位从源拷到目标
 *
 * destbitmap: 目标数组空值位图的起点（不得为
 * NULL） destoffset: 第一个目标元素的 0 基线性元
 * 素号 srcbitmap: 源数组空值位图的起点，无则为
 * NULL srcoffset: 第一个源元素的 0 基线性元素号
 * nitems: 要拷贝的位数（>= 0）
 *
 * 若 srcbitmap 为 NULL 则我们假定源全为非空，
 * 并把 1 填入目标位图。注意目标位图中只有指
 * 定的位会被改变，之前或之后的不变。
 *
 * 注：这当然可以用标准 bitblt 方法来优化。
 * 然而尚不清楚典型的 Postgres 数组是否有足够
 * 多的元素值得为此多虑。眼下从简（KISS）。
 */
void
array_bitmap_copy(bits8 *destbitmap, int destoffset,
				  const bits8 *srcbitmap, int srcoffset,
				  int nitems)
{
	int			destbitmask,
				destbitval,
				srcbitmask,
				srcbitval;

	Assert(destbitmap);
	if (nitems <= 0)
		return;					/* 勿越内存末尾读取 */
	destbitmap += destoffset / 8;
	destbitmask = 1 << (destoffset % 8);
	destbitval = *destbitmap;
	if (srcbitmap)
	{
		srcbitmap += srcoffset / 8;
		srcbitmask = 1 << (srcoffset % 8);
		srcbitval = *srcbitmap;
		while (nitems-- > 0)
		{
			if (srcbitval & srcbitmask)
				destbitval |= destbitmask;
			else
				destbitval &= ~destbitmask;
			destbitmask <<= 1;
			if (destbitmask == 0x100)
			{
				*destbitmap++ = destbitval;
				destbitmask = 1;
				if (nitems > 0)
					destbitval = *destbitmap;
			}
			srcbitmask <<= 1;
			if (srcbitmask == 0x100)
			{
				srcbitmap++;
				srcbitmask = 1;
				if (nitems > 0)
					srcbitval = *srcbitmap;
			}
		}
		if (destbitmask != 1)
			*destbitmap = destbitval;
	}
	else
	{
		while (nitems-- > 0)
		{
			destbitval |= destbitmask;
			destbitmask <<= 1;
			if (destbitmask == 0x100)
			{
				*destbitmap++ = destbitval;
				destbitmask = 1;
				if (nitems > 0)
					destbitval = *destbitmap;
			}
		}
		if (destbitmask != 1)
			*destbitmap = destbitval;
	}
}

/*
 * 计算数组一个切片所需的空间
 *
 * 我们假定调用者已验证切片坐标有效。
 */
static int
array_slice_size(char *arraydataptr, bits8 *arraynullsptr,
				 int ndim, int *dim, int *lb,
				 int *st, int *endp,
				 int typlen, bool typbyval, char typalign)
{
	int			src_offset,
				span[MAXDIM],
				prod[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	char	   *ptr;
	int			i,
				j,
				inc;
	int			count = 0;

	mda_get_range(ndim, span, st, endp);

	/* 定长元素且无空值时相当简单 …… */
	if (typlen > 0 && !arraynullsptr)
		return ArrayGetNItems(ndim, span) * att_align_nominal(typlen, typalign);

	/* 否则得用笨办法 */
	src_offset = ArrayGetOffset(ndim, dim, lb, st);
	ptr = array_seek(arraydataptr, 0, arraynullsptr, src_offset,
					 typlen, typbyval, typalign);
	mda_get_prod(ndim, dim, prod);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = ndim - 1;
	do
	{
		if (dist[j])
		{
			ptr = array_seek(ptr, src_offset, arraynullsptr, dist[j],
							 typlen, typbyval, typalign);
			src_offset += dist[j];
		}
		if (!array_get_isnull(arraynullsptr, src_offset))
		{
			inc = att_addlength_pointer(0, typlen, ptr);
			inc = att_align_nominal(inc, typalign);
			ptr += inc;
			count += inc;
		}
		src_offset++;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);
	return count;
}

/*
 * 把数组的一个切片提取为目标数组中的连续元
 * 素。
 *
 * 我们假定调用者已验证切片坐标有效、为结果
 * 分配了足够的存储并初始化了新数组的头部。
 *
 */
static void
array_extract_slice(ArrayType *newarray,
					int ndim,
					int *dim,
					int *lb,
					char *arraydataptr,
					bits8 *arraynullsptr,
					int *st,
					int *endp,
					int typlen,
					bool typbyval,
					char typalign)
{
	char	   *destdataptr = ARR_DATA_PTR(newarray);
	bits8	   *destnullsptr = ARR_NULLBITMAP(newarray);
	char	   *srcdataptr;
	int			src_offset,
				dest_offset,
				prod[MAXDIM],
				span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	int			i,
				j,
				inc;

	src_offset = ArrayGetOffset(ndim, dim, lb, st);
	srcdataptr = array_seek(arraydataptr, 0, arraynullsptr, src_offset,
							typlen, typbyval, typalign);
	mda_get_prod(ndim, dim, prod);
	mda_get_range(ndim, span, st, endp);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	dest_offset = 0;
	j = ndim - 1;
	do
	{
		if (dist[j])
		{
			/* 跳过多余元素 */
			srcdataptr = array_seek(srcdataptr, src_offset, arraynullsptr,
									dist[j],
									typlen, typbyval, typalign);
			src_offset += dist[j];
		}
		inc = array_copy(destdataptr, 1,
						 srcdataptr, src_offset, arraynullsptr,
						 typlen, typbyval, typalign);
		if (destnullsptr)
			array_bitmap_copy(destnullsptr, dest_offset,
							  arraynullsptr, src_offset,
							  1);
		destdataptr += inc;
		srcdataptr += inc;
		src_offset++;
		dest_offset++;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);
}

/*
 * 把切片插入数组。
 *
 * ndim/dim[]/lb[] 是原数组的维度。将构建一个
 * 具有相同维度的新数组。destArray 必须已分配
 * 且其头部已初始化。
 *
 * st[]/endp[] 标识要被替换的切片。切片体积内
 * 的元素取自 srcArray 的连续元素；体积外的元
 * 素则从 origArray 拷贝。
 *
 * 我们假定调用者已验证切片坐标有效。
 */
static void
array_insert_slice(ArrayType *destArray,
				   ArrayType *origArray,
				   ArrayType *srcArray,
				   int ndim,
				   int *dim,
				   int *lb,
				   int *st,
				   int *endp,
				   int typlen,
				   bool typbyval,
				   char typalign)
{
	char	   *destPtr = ARR_DATA_PTR(destArray);
	char	   *origPtr = ARR_DATA_PTR(origArray);
	char	   *srcPtr = ARR_DATA_PTR(srcArray);
	bits8	   *destBitmap = ARR_NULLBITMAP(destArray);
	bits8	   *origBitmap = ARR_NULLBITMAP(origArray);
	bits8	   *srcBitmap = ARR_NULLBITMAP(srcArray);
	int			orignitems = ArrayGetNItems(ARR_NDIM(origArray),
											ARR_DIMS(origArray));
	int			dest_offset,
				orig_offset,
				src_offset,
				prod[MAXDIM],
				span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	int			i,
				j,
				inc;

	dest_offset = ArrayGetOffset(ndim, dim, lb, st);
	/* 拷贝切片起点前的项 */
	inc = array_copy(destPtr, dest_offset,
					 origPtr, 0, origBitmap,
					 typlen, typbyval, typalign);
	destPtr += inc;
	origPtr += inc;
	if (destBitmap)
		array_bitmap_copy(destBitmap, 0, origBitmap, 0, dest_offset);
	orig_offset = dest_offset;
	mda_get_prod(ndim, dim, prod);
	mda_get_range(ndim, span, st, endp);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	src_offset = 0;
	j = ndim - 1;
	do
	{
		/* 拷贝/越过此处到切片下一部分间的元素 */
		if (dist[j])
		{
			inc = array_copy(destPtr, dist[j],
							 origPtr, orig_offset, origBitmap,
							 typlen, typbyval, typalign);
			destPtr += inc;
			origPtr += inc;
			if (destBitmap)
				array_bitmap_copy(destBitmap, dest_offset,
								  origBitmap, orig_offset,
								  dist[j]);
			dest_offset += dist[j];
			orig_offset += dist[j];
		}
		/* 在此切片位置放入新元素 */
		inc = array_copy(destPtr, 1,
						 srcPtr, src_offset, srcBitmap,
						 typlen, typbyval, typalign);
		if (destBitmap)
			array_bitmap_copy(destBitmap, dest_offset,
							  srcBitmap, src_offset,
							  1);
		destPtr += inc;
		srcPtr += inc;
		dest_offset++;
		src_offset++;
		/* 越过此切片位置处的旧元素 */
		origPtr = array_seek(origPtr, orig_offset, origBitmap, 1,
							 typlen, typbyval, typalign);
		orig_offset++;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);

	/* 别漏掉尾部数据 */
	array_copy(destPtr, orignitems - orig_offset,
			   origPtr, orig_offset, origBitmap,
			   typlen, typbyval, typalign);
	if (destBitmap)
		array_bitmap_copy(destBitmap, dest_offset,
						  origBitmap, orig_offset,
						  orignitems - orig_offset);
}

/*
 * initArrayResult - 初始化一个空 ArrayBuildState
 *
 *	element_type 是数组元素类型（须为有效数组元素类型）
 *	rcontext 是保存工作状态之处
 *	subcontext 是决定是否使用独立内存上下文的标志
 *
 * 注意：使用 accumArrayResult() 有两种常见方案。
 * 较旧的方案：以 NULL 的 ArrayBuildState 指针开
 * 始，每元素调用一次 accumArrayResult。此方案中
 * 若没有元素会得到 NULL 指针，你需要为其写特
 * 例处理。较新的方案：先调用 initArrayResult，
 * 然后每元素调用一次 accumArrayResult。此方案
 * 中总以非 NULL 指针结束，可传给
 * makeArrayResult；无元素时得空数组。想要空数组时首选。
 *
 * 可以为数组构建状态选择创建独立的内存上下
 * 文，还是直接在 rcontext 中分配。
 *
 * 当有很多并发的小状态时（例如 array_agg() 用
 * 以哈希聚合处理许多小组），为每个使用独立内
 * 存上下文可能导致严重的内存膨胀。此时用同
 * 一内存上下文初始化这类构建状态，并传
 * subcontext=false.
 *
 * 在数组构建状态生命期不同的情形下，使用单
 * 一内存上下文并不实际。改为传 subcontext=
 * true，使各构建状态可单独释放。
 */
ArrayBuildState *
initArrayResult(Oid element_type, MemoryContext rcontext, bool subcontext)
{
	/*
	 * 使用子上下文时，我们可以负担较大的初始数
	 * 组大小。不用子上下文时，最好指望多数状态
	 * 保持较小 ……
	 */
	return initArrayResultWithSize(element_type, rcontext, subcontext,
								   subcontext ? 64 : 8);
}

/*
 * initArrayResultWithSize
 *		同 initArrayResult，但允许指定分配数组的初始大小
 *
 */
ArrayBuildState *
initArrayResultWithSize(Oid element_type, MemoryContext rcontext,
						bool subcontext, int initsize)
{
	ArrayBuildState *astate;
	MemoryContext arr_context = rcontext;

	/* 建一个临时上下文来装所有杂物 */
	if (subcontext)
		arr_context = AllocSetContextCreate(rcontext,
											"accumArrayResult",
											ALLOCSET_DEFAULT_SIZES);

	astate = (ArrayBuildState *)
		MemoryContextAlloc(arr_context, sizeof(ArrayBuildState));
	astate->mcontext = arr_context;
	astate->private_cxt = subcontext;
	astate->alen = initsize;
	astate->dvalues = (Datum *)
		MemoryContextAlloc(arr_context, astate->alen * sizeof(Datum));
	astate->dnulls = (bool *)
		MemoryContextAlloc(arr_context, astate->alen * sizeof(bool));
	astate->nelems = 0;
	astate->element_type = element_type;
	get_typlenbyvalalign(element_type,
						 &astate->typlen,
						 &astate->typbyval,
						 &astate->typalign);

	return astate;
}

/*
 * accumArrayResult - 为数组结果累积（又一）个 Datum
 *
 *	astate 是工作状态（首次调用可为 NULL）
 *	dvalue/disnull 表示要追加到数组的新 Datum
 *	element_type 是 Datum 的类型（须为有效元素类型）
 *	rcontext 是保存工作状态之处
 */
ArrayBuildState *
accumArrayResult(ArrayBuildState *astate,
				 Datum dvalue, bool disnull,
				 Oid element_type,
				 MemoryContext rcontext)
{
	MemoryContext oldcontext;

	if (astate == NULL)
	{
		/* 首次执行 —— 初始化 */
		astate = initArrayResult(element_type, rcontext, true);
	}
	else
	{
		Assert(astate->element_type == element_type);
	}

	oldcontext = MemoryContextSwitchTo(astate->mcontext);

	/* 需要时扩容 dvalues[]/dnulls[] */
	if (astate->nelems >= astate->alen)
	{
		astate->alen *= 2;
		astate->dvalues = (Datum *)
			repalloc(astate->dvalues, astate->alen * sizeof(Datum));
		astate->dnulls = (bool *)
			repalloc(astate->dnulls, astate->alen * sizeof(bool));
	}

	/*
	 * 确保把引用传递的内容拷入 mcontext；若它是
	 * varlena 还要解 TOAST。（你可能认为此处无需解
	 * TOAST，因为 construct_md_array 之后可以解数组元
	 * 素的 TOAST。但是绝不能让 construct_md_array 修改
	 * ArrayBuildState，否则 array_agg_finalfn 会损坏其
	 * 输入，这是被禁止的。且常省一步拷贝。）
	 */
	if (!disnull && !astate->typbyval)
	{
		if (astate->typlen == -1)
			dvalue = PointerGetDatum(PG_DETOAST_DATUM_COPY(dvalue));
		else
			dvalue = datumCopy(dvalue, astate->typbyval, astate->typlen);
	}

	astate->dvalues[astate->nelems] = dvalue;
	astate->dnulls[astate->nelems] = disnull;
	astate->nelems++;

	MemoryContextSwitchTo(oldcontext);

	return astate;
}

/*
 * makeArrayResult - 产出 accumArrayResult 的 1-D 最终结果
 *
 * 注意：仅当 astate 在独立内存上下文中初始化
 * （即调用 initArrayResult 传了 subcontext=true）时才释放
 *
 *	astate 是工作状态（不得为 NULL）
 *	rcontext 是构建结果之处
 */
Datum
makeArrayResult(ArrayBuildState *astate,
				MemoryContext rcontext)
{
	int			ndims;
	int			dims[1];
	int			lbs[1];

	/* 若未提供任何元素，我们要创建空数组 */
	ndims = (astate->nelems > 0) ? 1 : 0;
	dims[0] = astate->nelems;
	lbs[0] = 1;

	return makeMdArrayResult(astate, ndims, dims, lbs, rcontext,
							 astate->private_cxt);
}

/*
 * makeMdArrayResult - 产出 accumArrayResult 的多维最终结果
 *
 * 注意：不检查指定的维度是否与已累积的值个
 * 数相符。
 *
 * 注意：若 astate 不是在独立内存上下文中初始
 * 化的（即调用 initArrayResult 时 subcontext=false），
 * 则 release=true 非法。改为在恰当时机把 astate
 * 随其上下文释放。
 *
 *	astate 是工作状态（不得为 NULL）
 *	rcontext 是构建结果之处
 *	release 为真表示可以释放工作状态
 */
Datum
makeMdArrayResult(ArrayBuildState *astate,
				  int ndims,
				  int *dims,
				  int *lbs,
				  MemoryContext rcontext,
				  bool release)
{
	ArrayType  *result;
	MemoryContext oldcontext;

	/* 在 rcontext 中构建最终数组结果 */
	oldcontext = MemoryContextSwitchTo(rcontext);

	result = construct_md_array(astate->dvalues,
								astate->dnulls,
								ndims,
								dims,
								lbs,
								astate->element_type,
								astate->typlen,
								astate->typbyval,
								astate->typalign);

	MemoryContextSwitchTo(oldcontext);

	/* 清理杂物 */
	if (release)
	{
		Assert(astate->private_cxt);
		MemoryContextDelete(astate->mcontext);
	}

	return PointerGetDatum(result);
}

/*
 * 下面三个函数提供的 API 与
 * initArrayResult/accumArrayResult/makeArrayResult 基本
 * 相同，但接受的输入是数组而非数组元素，并
 * 产生一个多出一维（N+1 维）的输出数组。输入
 * 必须全部维度数与元素类型相同。
 */

/*
 * initArrayResultArr - 初始化一个空 ArrayBuildStateArr
 *
 *	array_type 是数组类型（须为有效 varlena 数组类型）
 *	element_type 是元素类型（InvalidOid 则查找）
 *	rcontext 是保存工作状态之处
 *	subcontext 是决定是否使用独立内存上下文的标志
 */
ArrayBuildStateArr *
initArrayResultArr(Oid array_type, Oid element_type, MemoryContext rcontext,
				   bool subcontext)
{
	ArrayBuildStateArr *astate;
	MemoryContext arr_context = rcontext;	/* 默认用父上下文 */

	/* 除非已提供 element_type，否则查元素类型 */
	if (!OidIsValid(element_type))
	{
		element_type = get_element_type(array_type);

		if (!OidIsValid(element_type))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("data type %s is not an array type",
							format_type_be(array_type))));
	}

	/* 建一个临时上下文来装所有杂物 */
	if (subcontext)
		arr_context = AllocSetContextCreate(rcontext,
											"accumArrayResultArr",
											ALLOCSET_DEFAULT_SIZES);

	/* 注意所有字段均清零 */
	astate = (ArrayBuildStateArr *)
		MemoryContextAllocZero(arr_context, sizeof(ArrayBuildStateArr));
	astate->mcontext = arr_context;
	astate->private_cxt = subcontext;

	/* 保存相关数据类型信息 */
	astate->array_type = array_type;
	astate->element_type = element_type;

	return astate;
}

/*
 * accumArrayResultArr - 为数组结果累积（又一）个子数组
 *
 *	astate 是工作状态（首次调用可为 NULL）
 *	dvalue/disnull 表示要追加到数组的新子数组
 *	array_type 是数组类型（须为有效 varlena 数组类型）
 *	rcontext 是保存工作状态之处
 */
ArrayBuildStateArr *
accumArrayResultArr(ArrayBuildStateArr *astate,
					Datum dvalue, bool disnull,
					Oid array_type,
					MemoryContext rcontext)
{
	ArrayType  *arg;
	MemoryContext oldcontext;
	int		   *dims,
			   *lbs,
				ndims,
				nitems,
				ndatabytes;
	char	   *data;
	int			i;

	/*
	 * 我们不允许累积 NULL 子数组。另一个合理定义
	 * 是忽略它们，但想要的调用者只需跳过本函
	 * 数调用。
	 */
	if (disnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cannot accumulate null arrays")));

	/* 在调用者上下文解数组 TOAST */
	arg = DatumGetArrayTypeP(dvalue);

	if (astate == NULL)
		astate = initArrayResultArr(array_type, InvalidOid, rcontext, true);
	else
		Assert(astate->array_type == array_type);

	oldcontext = MemoryContextSwitchTo(astate->mcontext);

	/* 收集此输入的维度 */
	ndims = ARR_NDIM(arg);
	dims = ARR_DIMS(arg);
	lbs = ARR_LBOUND(arg);
	data = ARR_DATA_PTR(arg);
	nitems = ArrayGetNItems(ndims, dims);
	ndatabytes = ARR_SIZE(arg) - ARR_DATA_OFFSET(arg);

	if (astate->ndims == 0)
	{
		/* 首个输入；检查/保存维度数信息 */

		/* 我们是否应允许空输入并直接产出空输出?  */
		if (ndims == 0)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("cannot accumulate empty arrays")));
		if (ndims + 1 > MAXDIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
							ndims + 1, MAXDIM)));

		/*
		 * 输出数组将有 N+1 个维度，第一个之后的维度
		 * 与输入的维度匹配。
		 */
		astate->ndims = ndims + 1;
		astate->dims[0] = 0;
		memcpy(&astate->dims[1], dims, ndims * sizeof(int));
		astate->lbs[0] = 1;
		memcpy(&astate->lbs[1], lbs, ndims * sizeof(int));

		/* 为此项分配至少足够的数据空间 */
		astate->abytes = pg_nextpower2_32(Max(1024, ndatabytes + 1));
		astate->data = (char *) palloc(astate->abytes);
	}
	else
	{
		/* 第二及更后输入：须匹配首个输入的维度数 */
		if (astate->ndims != ndims + 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("cannot accumulate arrays of different dimensionality")));
		for (i = 0; i < ndims; i++)
		{
			if (astate->dims[i + 1] != dims[i] || astate->lbs[i + 1] != lbs[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot accumulate arrays of different dimensionality")));
		}

		/* 需要时扩容数据空间 */
		if (astate->nbytes + ndatabytes > astate->abytes)
		{
			astate->abytes = Max(astate->abytes * 2,
								 astate->nbytes + ndatabytes);
			astate->data = (char *) repalloc(astate->data, astate->abytes);
		}
	}

	/*
	 * 拷贝子数组的数据部分。注意我们假定子数组
	 * 对外声明的数据长度已正确对齐。子数组中的
	 * 元素无需担心解 TOAST，因为其中内容应当已
	 * 然是完好的。
	 */
	memcpy(astate->data + astate->nbytes, data, ndatabytes);
	astate->nbytes += ndatabytes;

	/* 需要时处理空值位图 */
	if (astate->nullbitmap || ARR_HASNULL(arg))
	{
		int			newnitems = astate->nitems + nitems;

		if (astate->nullbitmap == NULL)
		{
			/*
			 * 首个带空值输入；须把先前所有输入的各
			 * 项都标为非空。
			 */
			astate->aitems = pg_nextpower2_32(Max(256, newnitems + 1));
			astate->nullbitmap = (bits8 *) palloc((astate->aitems + 7) / 8);
			array_bitmap_copy(astate->nullbitmap, 0,
							  NULL, 0,
							  astate->nitems);
		}
		else if (newnitems > astate->aitems)
		{
			astate->aitems = Max(astate->aitems * 2, newnitems);
			astate->nullbitmap = (bits8 *)
				repalloc(astate->nullbitmap, (astate->aitems + 7) / 8);
		}
		array_bitmap_copy(astate->nullbitmap, astate->nitems,
						  ARR_NULLBITMAP(arg), 0,
						  nitems);
	}

	astate->nitems += nitems;
	astate->dims[0] += 1;

	MemoryContextSwitchTo(oldcontext);

	/* 有则释放解 TOAST 副本 */
	if ((Pointer) arg != DatumGetPointer(dvalue))
		pfree(arg);

	return astate;
}

/*
 * makeArrayResultArr - 产出 accumArrayResultArr 的 N+1维最终结果
 *
 *	astate 是工作状态（不得为 NULL）
 *	rcontext 是构建结果之处
 *	release 为真表示可以释放工作状态
 */
Datum
makeArrayResultArr(ArrayBuildStateArr *astate,
				   MemoryContext rcontext,
				   bool release)
{
	ArrayType  *result;
	MemoryContext oldcontext;

	/* 在 rcontext 中构建最终数组结果 */
	oldcontext = MemoryContextSwitchTo(rcontext);

	if (astate->ndims == 0)
	{
		/* 无输入则返回空数组 */
		result = construct_empty_array(astate->element_type);
	}
	else
	{
		int			dataoffset,
					nbytes;

		/* 检查数组维度是否溢出 */
		(void) ArrayGetNItems(astate->ndims, astate->dims);
		ArrayCheckBounds(astate->ndims, astate->dims, astate->lbs);

		/* 计算所需空间 */
		nbytes = astate->nbytes;
		if (astate->nullbitmap != NULL)
		{
			dataoffset = ARR_OVERHEAD_WITHNULLS(astate->ndims, astate->nitems);
			nbytes += dataoffset;
		}
		else
		{
			dataoffset = 0;
			nbytes += ARR_OVERHEAD_NONULLS(astate->ndims);
		}

		result = (ArrayType *) palloc0(nbytes);
		SET_VARSIZE(result, nbytes);
		result->ndim = astate->ndims;
		result->dataoffset = dataoffset;
		result->elemtype = astate->element_type;

		memcpy(ARR_DIMS(result), astate->dims, astate->ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), astate->lbs, astate->ndims * sizeof(int));
		memcpy(ARR_DATA_PTR(result), astate->data, astate->nbytes);

		if (astate->nullbitmap != NULL)
			array_bitmap_copy(ARR_NULLBITMAP(result), 0,
							  astate->nullbitmap, 0,
							  astate->nitems);
	}

	MemoryContextSwitchTo(oldcontext);

	/* 清理杂物 */
	if (release)
	{
		Assert(astate->private_cxt);
		MemoryContextDelete(astate->mcontext);
	}

	return PointerGetDatum(result);
}

/*
 * 下面三个函数提供的 API 与
 * initArrayResult/accumArrayResult/makeArrayResult 相同，
 * 但可接受标量或数组输入，调用上述相应函数组。
 */

/*
 * initArrayResultAny - 初始化一个空 ArrayBuildStateAny
 *
 *	input_type 是输入数据类型（元素或数组类型）
 *	rcontext 是保存工作状态之处
 *	subcontext 是决定是否使用独立内存上下文的标志
 */
ArrayBuildStateAny *
initArrayResultAny(Oid input_type, MemoryContext rcontext, bool subcontext)
{
	ArrayBuildStateAny *astate;

	/*
	 * int2vector 与 oidvector 会同时满足
	 * get_element_type 与 get_array_type。我们倾向把它们
	 * 当作标量与 get_promoted_array_type 一致，故查
	 * get_array_type。
	 */
	if (!OidIsValid(get_array_type(input_type)))
	{
		/* 数组 */
		ArrayBuildStateArr *arraystate;

		arraystate = initArrayResultArr(input_type, InvalidOid, rcontext, subcontext);
		astate = (ArrayBuildStateAny *)
			MemoryContextAlloc(arraystate->mcontext,
							   sizeof(ArrayBuildStateAny));
		astate->scalarstate = NULL;
		astate->arraystate = arraystate;
	}
	else
	{
		/* 标量 */
		ArrayBuildState *scalarstate;

		scalarstate = initArrayResult(input_type, rcontext, subcontext);
		astate = (ArrayBuildStateAny *)
			MemoryContextAlloc(scalarstate->mcontext,
							   sizeof(ArrayBuildStateAny));
		astate->scalarstate = scalarstate;
		astate->arraystate = NULL;
	}

	return astate;
}

/*
 * accumArrayResultAny - 为数组结果累积（又一个）输入
 *
 *	astate 是工作状态（首次调用可为 NULL）
 *	dvalue/disnull 表示要追加到数组的新输入
 *	input_type 是输入数据类型（元素或数组类型）
 *	rcontext 是保存工作状态之处
 */
ArrayBuildStateAny *
accumArrayResultAny(ArrayBuildStateAny *astate,
					Datum dvalue, bool disnull,
					Oid input_type,
					MemoryContext rcontext)
{
	if (astate == NULL)
		astate = initArrayResultAny(input_type, rcontext, true);

	if (astate->scalarstate)
		(void) accumArrayResult(astate->scalarstate,
								dvalue, disnull,
								input_type, rcontext);
	else
		(void) accumArrayResultArr(astate->arraystate,
								   dvalue, disnull,
								   input_type, rcontext);

	return astate;
}

/*
 * makeArrayResultAny - 产出 accumArrayResultAny 的最终结果
 *
 *	astate 是工作状态（不得为 NULL）
 *	rcontext 是构建结果之处
 *	release 为真表示可以释放工作状态
 */
Datum
makeArrayResultAny(ArrayBuildStateAny *astate,
				   MemoryContext rcontext, bool release)
{
	Datum		result;

	if (astate->scalarstate)
	{
		/* 须用 makeMdArrayResult 才支持 "release" 参数 */
		int			ndims;
		int			dims[1];
		int			lbs[1];

		/* 若未提供任何元素，我们要创建空数组 */
		ndims = (astate->scalarstate->nelems > 0) ? 1 : 0;
		dims[0] = astate->scalarstate->nelems;
		lbs[0] = 1;

		result = makeMdArrayResult(astate->scalarstate, ndims, dims, lbs,
								   rcontext, release);
	}
	else
	{
		result = makeArrayResultArr(astate->arraystate,
									rcontext, release);
	}
	return result;
}


Datum
array_larger(PG_FUNCTION_ARGS)
{
	if (array_cmp(fcinfo) > 0)
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	else
		PG_RETURN_DATUM(PG_GETARG_DATUM(1));
}

Datum
array_smaller(PG_FUNCTION_ARGS)
{
	if (array_cmp(fcinfo) < 0)
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	else
		PG_RETURN_DATUM(PG_GETARG_DATUM(1));
}


typedef struct generate_subscripts_fctx
{
	int32		lower;
	int32		upper;
	bool		reverse;
} generate_subscripts_fctx;

/*
 * generate_subscripts(array anyarray, dim int [, reverse bool])
 *		返回数组任一维的所有下标
 */
Datum
generate_subscripts(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	MemoryContext oldcontext;
	generate_subscripts_fctx *fctx;

	/* 仅在函数首次调用时做的事 */
	if (SRF_IS_FIRSTCALL())
	{
		AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
		int			reqdim = PG_GETARG_INT32(1);
		int		   *lb,
				   *dimv;

		/* 创建跨调用持久的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/* 合理性检查：看起来像数组吗? */
		if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
			SRF_RETURN_DONE(funcctx);

		/* 合理性检查：维度有效? */
		if (reqdim <= 0 || reqdim > AARR_NDIM(v))
			SRF_RETURN_DONE(funcctx);

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		fctx = (generate_subscripts_fctx *) palloc(sizeof(generate_subscripts_fctx));

		lb = AARR_LBOUND(v);
		dimv = AARR_DIMS(v);

		fctx->lower = lb[reqdim - 1];
		fctx->upper = dimv[reqdim - 1] + lb[reqdim - 1] - 1;
		fctx->reverse = (PG_NARGS() < 3) ? false : PG_GETARG_BOOL(2);

		funcctx->user_fctx = fctx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	fctx = funcctx->user_fctx;

	if (fctx->lower <= fctx->upper)
	{
		if (!fctx->reverse)
			SRF_RETURN_NEXT(funcctx, Int32GetDatum(fctx->lower++));
		else
			SRF_RETURN_NEXT(funcctx, Int32GetDatum(fctx->upper--));
	}
	else
		/* 没有剩余元素时完成 */
		SRF_RETURN_DONE(funcctx);
}

/*
 * generate_subscripts_nodir
 *		实现 generate_subscripts 的双参数版本
 */
Datum
generate_subscripts_nodir(PG_FUNCTION_ARGS)
{
	/* 直接调另一个 —— 它可处理两情形 */
	return generate_subscripts(fcinfo);
}

/*
 * array_fill_with_lower_bounds
 *		创建并填充具有指定下界的数组。
 */
Datum
array_fill_with_lower_bounds(PG_FUNCTION_ARGS)
{
	ArrayType  *dims;
	ArrayType  *lbs;
	ArrayType  *result;
	Oid			elmtype;
	Datum		value;
	bool		isnull;

	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dimension array or low bound array cannot be null")));

	dims = PG_GETARG_ARRAYTYPE_P(1);
	lbs = PG_GETARG_ARRAYTYPE_P(2);

	if (!PG_ARGISNULL(0))
	{
		value = PG_GETARG_DATUM(0);
		isnull = false;
	}
	else
	{
		value = 0;
		isnull = true;
	}

	elmtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (!OidIsValid(elmtype))
		elog(ERROR, "could not determine data type of input");

	result = array_fill_internal(dims, lbs, value, isnull, elmtype, fcinfo);
	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * array_fill
 *		创建并填充具有默认下界的数组。
 */
Datum
array_fill(PG_FUNCTION_ARGS)
{
	ArrayType  *dims;
	ArrayType  *result;
	Oid			elmtype;
	Datum		value;
	bool		isnull;

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dimension array or low bound array cannot be null")));

	dims = PG_GETARG_ARRAYTYPE_P(1);

	if (!PG_ARGISNULL(0))
	{
		value = PG_GETARG_DATUM(0);
		isnull = false;
	}
	else
	{
		value = 0;
		isnull = true;
	}

	elmtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (!OidIsValid(elmtype))
		elog(ERROR, "could not determine data type of input");

	result = array_fill_internal(dims, NULL, value, isnull, elmtype, fcinfo);
	PG_RETURN_ARRAYTYPE_P(result);
}

static ArrayType *
create_array_envelope(int ndims, int *dimv, int *lbsv, int nbytes,
					  Oid elmtype, int dataoffset)
{
	ArrayType  *result;

	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndims;
	result->dataoffset = dataoffset;
	result->elemtype = elmtype;
	memcpy(ARR_DIMS(result), dimv, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbsv, ndims * sizeof(int));

	return result;
}

static ArrayType *
array_fill_internal(ArrayType *dims, ArrayType *lbs,
					Datum value, bool isnull, Oid elmtype,
					FunctionCallInfo fcinfo)
{
	ArrayType  *result;
	int		   *dimv;
	int		   *lbsv;
	int			ndims;
	int			nitems;
	int			deflbs[MAXDIM];
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	ArrayMetaState *my_extra;

	/*
	 * 参数检查
	 */
	if (ARR_NDIM(dims) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts"),
				 errdetail("Dimension array must be one dimensional.")));

	if (array_contains_nulls(dims))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dimension values cannot be null")));

	dimv = (int *) ARR_DATA_PTR(dims);
	ndims = (ARR_NDIM(dims) > 0) ? ARR_DIMS(dims)[0] : 0;

	if (ndims < 0)				/* 我们确实允许零维度数组 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid number of dimensions: %d", ndims)));
	if (ndims > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						ndims, MAXDIM)));

	if (lbs != NULL)
	{
		if (ARR_NDIM(lbs) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts"),
					 errdetail("Dimension array must be one dimensional.")));

		if (array_contains_nulls(lbs))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("dimension values cannot be null")));

		if (ndims != ((ARR_NDIM(lbs) > 0) ? ARR_DIMS(lbs)[0] : 0))
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts"),
					 errdetail("Low bound array has different size than dimensions array.")));

		lbsv = (int *) ARR_DATA_PTR(lbs);
	}
	else
	{
		int			i;

		for (i = 0; i < MAXDIM; i++)
			deflbs[i] = 1;

		lbsv = deflbs;
	}

	/* 此处检查数组维度是否溢出 */
	nitems = ArrayGetNItems(ndims, dimv);
	ArrayCheckBounds(ndims, dimv, lbsv);

	/* 空数组捷径 */
	if (nitems <= 0)
		return construct_empty_array(elmtype);

	/*
	 * 元素类型的信息我们安排每轮调用只查找一次，前提
	 * 是元素类型不在中途被更换。
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = InvalidOid;
	}

	if (my_extra->element_type != elmtype)
	{
		/* 获取元素类型信息 */
		get_typlenbyvalalign(elmtype,
							 &my_extra->typlen,
							 &my_extra->typbyval,
							 &my_extra->typalign);
		my_extra->element_type = elmtype;
	}

	elmlen = my_extra->typlen;
	elmbyval = my_extra->typbyval;
	elmalign = my_extra->typalign;

	/* 计算所需空间 */
	if (!isnull)
	{
		int			i;
		char	   *p;
		int			nbytes;
		int			totbytes;

		/* 确保数据未被 TOAST */
		if (elmlen == -1)
			value = PointerGetDatum(PG_DETOAST_DATUM(value));

		nbytes = att_addlength_datum(0, elmlen, value);
		nbytes = att_align_nominal(nbytes, elmalign);
		Assert(nbytes > 0);

		totbytes = nbytes * nitems;

		/* 检查乘法或总请求是否溢出 */
		if (totbytes / nbytes != nitems ||
			!AllocSizeIsValid(totbytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxAllocSize)));

		/*
		 * 此加法不会溢出，但可能使我们超过
		 * MaxAllocSize。那种情况留给 palloc 抱怨。
		 */
		totbytes += ARR_OVERHEAD_NONULLS(ndims);

		result = create_array_envelope(ndims, dimv, lbsv, totbytes,
									   elmtype, 0);

		p = ARR_DATA_PTR(result);
		for (i = 0; i < nitems; i++)
			p += ArrayCastAndSet(value, elmlen, elmbyval, elmalign, p);
	}
	else
	{
		int			nbytes;
		int			dataoffset;

		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
		nbytes = dataoffset;

		result = create_array_envelope(ndims, dimv, lbsv, nbytes,
									   elmtype, dataoffset);

		/* create_array_envelope 已把位图清零，因此完成 */
	}

	return result;
}


/*
 * UNNEST
 */
Datum
array_unnest(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		array_iter	iter;
		int			nextelem;
		int			numelems;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
	} array_unnest_fctx;

	FuncCallContext *funcctx;
	array_unnest_fctx *fctx;
	MemoryContext oldcontext;

	/* 仅在函数首次调用时做的事 */
	if (SRF_IS_FIRSTCALL())
	{
		AnyArrayType *arr;

		/* 创建跨调用持久的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * 取得数组值并在需要时解 TOAST。不能更早做这
		 * 件事，因为若必须解 TOAST，我们希望解 TOAST 后
		 * 的副本位于 multi_call_memory_ctx，这样它会在我们
		 * 完成时消失而不是更早。（若未发生解 TOAST，我
		 * 们假定原数组会保留到那时。）
		 */
		arr = PG_GETARG_ANY_ARRAY_P(0);

		/* 为用户上下文分内存 */
		fctx = (array_unnest_fctx *) palloc(sizeof(array_unnest_fctx));

		/* 初始化 */
		array_iter_setup(&fctx->iter, arr);
		fctx->nextelem = 0;
		fctx->numelems = ArrayGetNItems(AARR_NDIM(arr), AARR_DIMS(arr));

		if (VARATT_IS_EXPANDED_HEADER(arr))
		{
			/* 可直接从展开数组抓取类型数据 */
			fctx->elmlen = arr->xpn.typlen;
			fctx->elmbyval = arr->xpn.typbyval;
			fctx->elmalign = arr->xpn.typalign;
		}
		else
			get_typlenbyvalalign(AARR_ELEMTYPE(arr),
								 &fctx->elmlen,
								 &fctx->elmbyval,
								 &fctx->elmalign);

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 每次函数调用都要做的事 */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	if (fctx->nextelem < fctx->numelems)
	{
		int			offset = fctx->nextelem++;
		Datum		elem;

		elem = array_iter_next(&fctx->iter, &fcinfo->isnull, offset,
							   fctx->elmlen, fctx->elmbyval, fctx->elmalign);

		SRF_RETURN_NEXT(funcctx, elem);
	}
	else
	{
		/* 没有剩余时执行 */
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * array_unnest(anyarray) 的规划器支持函数
 */
Datum
array_unnest_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		/* 尝试估计将返回的行数 */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node))	/* 稳妥 */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1;

			/* 此处可以用估算的参数值 */
			arg1 = estimate_expression_value(req->root, linitial(args));

			req->rows = estimate_array_length(arg1);
			ret = (Node *) req;
		}
	}

	PG_RETURN_POINTER(ret);
}


/*
 * array_replace/array_remove 支持
 *
 * 找出数组中所有与 search/search_isnull 匹配（即
 * IS NOT DISTINCT FROM）的项，若 remove 为真则删
 * 除它们，否则用 replace/replace_isnull 替换。比较
 * 用指定排序规则。fcinfo 仅为缓存而传。
 */
static ArrayType *
array_replace_internal(ArrayType *array,
					   Datum search, bool search_isnull,
					   Datum replace, bool replace_isnull,
					   bool remove, Oid collation,
					   FunctionCallInfo fcinfo)
{
	LOCAL_FCINFO(locfcinfo, 2);
	ArrayType  *result;
	Oid			element_type;
	Datum	   *values;
	bool	   *nulls;
	int		   *dim;
	int			ndim;
	int			nitems,
				nresult;
	int			i;
	int32		nbytes = 0;
	int32		dataoffset;
	bool		hasnulls;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *arraydataptr;
	bits8	   *bitmap;
	int			bitmask;
	bool		changed = false;
	TypeCacheEntry *typentry;

	element_type = ARR_ELEMTYPE(array);
	ndim = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	nitems = ArrayGetNItems(ndim, dim);

	/* 若输入数组为空则原样返回 */
	if (nitems <= 0)
		return array;

	/*
	 * 我们无法从多维数组移除元素，因为结果可能
	 * 不再是矩形。
	 */
	if (remove && ndim > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("removing elements from multidimensional arrays is not supported")));

	/*
	 * 相等函数我们安排每轮调用只查找一次，前提
	 * 是元素类型不在中途被更换。
	 */
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an equality operator for type %s",
							format_type_be(element_type))));
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}
	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	/*
	 * 若值被 TOAST 过则解 TOAST。替换值必须解
	 * TOAST 后才能插入结果数组，而查找值只解一次
	 * 可以省去开销。
	 */
	if (typlen == -1)
	{
		if (!search_isnull)
			search = PointerGetDatum(PG_DETOAST_DATUM(search));
		if (!replace_isnull)
			replace = PointerGetDatum(PG_DETOAST_DATUM(replace));
	}

	/* 准备应用比较操作符 */
	InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
							 collation, NULL, NULL);

	/* 为新值分配临时数组 */
	values = (Datum *) palloc(nitems * sizeof(Datum));
	nulls = (bool *) palloc(nitems * sizeof(bool));

	/* 遍历源数据 */
	arraydataptr = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;
	hasnulls = false;
	nresult = 0;

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		bool		isNull;
		bool		oprresult;
		bool		skip = false;

		/* 取得源元素，检查 NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			isNull = true;
			/* 若在查找 NULL，则已匹配 */
			if (search_isnull)
			{
				if (remove)
				{
					skip = true;
					changed = true;
				}
				else if (!replace_isnull)
				{
					values[nresult] = replace;
					isNull = false;
					changed = true;
				}
			}
		}
		else
		{
			isNull = false;
			elt = fetch_att(arraydataptr, typbyval, typlen);
			arraydataptr = att_addlength_datum(arraydataptr, typlen, elt);
			arraydataptr = (char *) att_align_nominal(arraydataptr, typalign);

			if (search_isnull)
			{
				/* 不可能匹配，保留元素 */
				values[nresult] = elt;
			}
			else
			{
				/*
				 * 把操作符应用于元素对；把 NULL 当作假
				 */
				locfcinfo->args[0].value = elt;
				locfcinfo->args[0].isnull = false;
				locfcinfo->args[1].value = search;
				locfcinfo->args[1].isnull = false;
				locfcinfo->isnull = false;
				oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
				if (locfcinfo->isnull || !oprresult)
				{
					/* 无匹配保留元素 */
					values[nresult] = elt;
				}
				else
				{
					/* 匹配，故替换或删除 */
					changed = true;
					if (remove)
						skip = true;
					else
					{
						values[nresult] = replace;
						isNull = replace_isnull;
					}
				}
			}
		}

		if (!skip)
		{
			nulls[nresult] = isNull;
			if (isNull)
				hasnulls = true;
			else
			{
				/* 更新总结果大小 */
				nbytes = att_addlength_datum(nbytes, typlen, values[nresult]);
				nbytes = att_align_nominal(nbytes, typalign);
				/* 检查总请求是否溢出 */
				if (!AllocSizeIsValid(nbytes))
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("array size exceeds the maximum allowed (%d)",
									(int) MaxAllocSize)));
			}
			nresult++;
		}

		/* 若有则前移位图 */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	/*
	 * 若未改变则直接返回原数组
	 */
	if (!changed)
	{
		pfree(values);
		pfree(nulls);
		return array;
	}

	/* 若所有元素都被移除则返回空数组 */
	if (nresult == 0)
	{
		pfree(values);
		pfree(nulls);
		return construct_empty_array(element_type);
	}

	/* 分配并初始化结果数组 */
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nresult);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* 无空值位图的标记 */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndim;
	result->dataoffset = dataoffset;
	result->elemtype = element_type;
	memcpy(ARR_DIMS(result), ARR_DIMS(array), ndim * sizeof(int));
	memcpy(ARR_LBOUND(result), ARR_LBOUND(array), ndim * sizeof(int));

	if (remove)
	{
		/* 调整结果长度 */
		ARR_DIMS(result)[0] = nresult;
	}

	/* 数据插入结果数组 */
	CopyArrayEls(result,
				 values, nulls, nresult,
				 typlen, typbyval, typalign,
				 false);

	pfree(values);
	pfree(nulls);

	return result;
}

/*
 * 从数组中移除元素的所有出现
 *
 * 若用于多维数组这将报错。
 */
Datum
array_remove(PG_FUNCTION_ARGS)
{
	ArrayType  *array;
	Datum		search = PG_GETARG_DATUM(1);
	bool		search_isnull = PG_ARGISNULL(1);

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	array = PG_GETARG_ARRAYTYPE_P(0);

	array = array_replace_internal(array,
								   search, search_isnull,
								   (Datum) 0, true,
								   true, PG_GET_COLLATION(),
								   fcinfo);
	PG_RETURN_ARRAYTYPE_P(array);
}

/*
 * 替换数组中元素的所有出现
 */
Datum
array_replace(PG_FUNCTION_ARGS)
{
	ArrayType  *array;
	Datum		search = PG_GETARG_DATUM(1);
	bool		search_isnull = PG_ARGISNULL(1);
	Datum		replace = PG_GETARG_DATUM(2);
	bool		replace_isnull = PG_ARGISNULL(2);

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	array = PG_GETARG_ARRAYTYPE_P(0);

	array = array_replace_internal(array,
								   search, search_isnull,
								   replace, replace_isnull,
								   false, PG_GET_COLLATION(),
								   fcinfo);
	PG_RETURN_ARRAYTYPE_P(array);
}

/*
 * 实现 width_bucket(anyelement, anyarray)。
 *
 * 'thresholds' 是包含每个桶下界值的数组；
 * 这些必须从小到大排序，否则会产生伪结果。
 * 若提供了 N 个阈值，输出范围是从 0 到 N：
 * 0 表示 < 首阈值的输入，N 表示 >= 末阈值的输入。
 */
Datum
width_bucket_array(PG_FUNCTION_ARGS)
{
	Datum		operand = PG_GETARG_DATUM(0);
	ArrayType  *thresholds = PG_GETARG_ARRAYTYPE_P(1);
	Oid			collation = PG_GET_COLLATION();
	Oid			element_type = ARR_ELEMTYPE(thresholds);
	int			result;

	/* 查输入 */
	if (ARR_NDIM(thresholds) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("thresholds must be one-dimensional array")));

	if (array_contains_nulls(thresholds))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("thresholds array must not contain NULLs")));

	/* 对 float8 数据有专用实现 */
	if (element_type == FLOAT8OID)
		result = width_bucket_array_float8(operand, thresholds);
	else
	{
		TypeCacheEntry *typentry;

		/* 缓存输入类型的信息 */
		typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
		if (typentry == NULL ||
			typentry->type_id != element_type)
		{
			typentry = lookup_type_cache(element_type,
										 TYPECACHE_CMP_PROC_FINFO);
			if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify a comparison function for type %s",
								format_type_be(element_type))));
			fcinfo->flinfo->fn_extra = (void *) typentry;
		}

		/*
		 * 定长与变宽类型有分开的实现路径，因为前者的
		 * 数组索引便宜得多。
		 */
		if (typentry->typlen > 0)
			result = width_bucket_array_fixed(operand, thresholds,
											  collation, typentry);
		else
			result = width_bucket_array_variable(operand, thresholds,
												 collation, typentry);
	}

	/* 传入 TOAST 输入时避免内存泄漏。 */
	PG_FREE_IF_COPY(thresholds, 1);

	PG_RETURN_INT32(result);
}

/*
 * width_bucket_array 用于 float8。
 */
static int
width_bucket_array_float8(Datum operand, ArrayType *thresholds)
{
	float8		op = DatumGetFloat8(operand);
	float8	   *thresholds_data;
	int			left;
	int			right;

	/*
	 * 既然已知数组不含 NULL，可直接对它进行
	 * 索引。
	 */
	thresholds_data = (float8 *) ARR_DATA_PTR(thresholds);

	left = 0;
	right = ArrayGetNItems(ARR_NDIM(thresholds), ARR_DIMS(thresholds));

	/*
	 * 若探测值是 NaN，它大于等于所有可能阈值
	 * （包括其他 NaN），因此无需查找。注意即使
	 * 数组含多个 NaN，结果也与查找相同（只要它们
	 * 已被正确排序），因为循环逻辑会找出多个相等
	 * 阈值中最右的那个。
	 */
	if (isnan(op))
		return right;

	/* 找到桶 */
	while (left < right)
	{
		int			mid = (left + right) / 2;

		if (isnan(thresholds_data[mid]) || op < thresholds_data[mid])
			right = mid;
		else
			left = mid + 1;
	}

	return left;
}

/*
 * width_bucket_array 用于通用定宽数据类型。
 */
static int
width_bucket_array_fixed(Datum operand,
						 ArrayType *thresholds,
						 Oid collation,
						 TypeCacheEntry *typentry)
{
	LOCAL_FCINFO(locfcinfo, 2);
	char	   *thresholds_data;
	int			typlen = typentry->typlen;
	bool		typbyval = typentry->typbyval;
	int			left;
	int			right;

	/*
	 * 既然已知数组不含 NULL，可直接对它进行
	 * 索引。
	 */
	thresholds_data = (char *) ARR_DATA_PTR(thresholds);

	InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
							 collation, NULL, NULL);

	/* 找到桶 */
	left = 0;
	right = ArrayGetNItems(ARR_NDIM(thresholds), ARR_DIMS(thresholds));
	while (left < right)
	{
		int			mid = (left + right) / 2;
		char	   *ptr;
		int32		cmpresult;

		ptr = thresholds_data + mid * typlen;

		locfcinfo->args[0].value = operand;
		locfcinfo->args[0].isnull = false;
		locfcinfo->args[1].value = fetch_att(ptr, typbyval, typlen);
		locfcinfo->args[1].isnull = false;

		cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

		/* 我们不期望比较支持函数返回 null */
		Assert(!locfcinfo->isnull);

		if (cmpresult < 0)
			right = mid;
		else
			left = mid + 1;
	}

	return left;
}

/*
 * width_bucket_array 用于通用变宽数据类型。
 */
static int
width_bucket_array_variable(Datum operand,
							ArrayType *thresholds,
							Oid collation,
							TypeCacheEntry *typentry)
{
	LOCAL_FCINFO(locfcinfo, 2);
	char	   *thresholds_data;
	int			typlen = typentry->typlen;
	bool		typbyval = typentry->typbyval;
	char		typalign = typentry->typalign;
	int			left;
	int			right;

	thresholds_data = (char *) ARR_DATA_PTR(thresholds);

	InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
							 collation, NULL, NULL);

	/* 找到桶 */
	left = 0;
	right = ArrayGetNItems(ARR_NDIM(thresholds), ARR_DIMS(thresholds));
	while (left < right)
	{
		int			mid = (left + right) / 2;
		char	   *ptr;
		int			i;
		int32		cmpresult;

		/* 从左元素前进以定位第 mid 个数组元素 */
		ptr = thresholds_data;
		for (i = left; i < mid; i++)
		{
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			ptr = (char *) att_align_nominal(ptr, typalign);
		}

		locfcinfo->args[0].value = operand;
		locfcinfo->args[0].isnull = false;
		locfcinfo->args[1].value = fetch_att(ptr, typbyval, typlen);
		locfcinfo->args[1].isnull = false;

		cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

		/* 我们不期望比较支持函数返回 null */
		Assert(!locfcinfo->isnull);

		if (cmpresult < 0)
			right = mid;
		else
		{
			left = mid + 1;

			/*
			 * 把阈值指针移到与新的 "left" 下标匹配处，
			 * 省得再次跳过那些元素。该技巧确保
			 * 我们只做 O(N) 的数组索引工作而非 O(N^2)。
			 */
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			thresholds_data = (char *) att_align_nominal(ptr, typalign);
		}
	}

	return left;
}

/*
 * 通过构建合适的切片来修剪数组末尾的 N 个元
 * 素。只修剪第一维。
 */
Datum
trim_array(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	int			n = PG_GETARG_INT32(1);
	int			array_length = (ARR_NDIM(v) > 0) ? ARR_DIMS(v)[0] : 0;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			lower[MAXDIM];
	int			upper[MAXDIM];
	bool		lowerProvided[MAXDIM];
	bool		upperProvided[MAXDIM];
	Datum		result;

	/* 按规范，越界则报错 */
	if (n < 0 || n > array_length)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("number of elements to trim must be between 0 and %d",
						array_length)));

	/* 除第一个上界外把所有界都设为未提供 */
	memset(lowerProvided, false, sizeof(lowerProvided));
	memset(upperProvided, false, sizeof(upperProvided));
	if (ARR_NDIM(v) > 0)
	{
		upper[0] = ARR_LBOUND(v)[0] + array_length - n - 1;
		upperProvided[0] = true;
	}

	/* 取得元素类型所需的信息 */
	get_typlenbyvalalign(ARR_ELEMTYPE(v), &elmlen, &elmbyval, &elmalign);

	/* 取切片 */
	result = array_get_slice(PointerGetDatum(v), 1,
							 upper, lower, upperProvided, lowerProvided,
							 -1, elmlen, elmbyval, elmalign);

	PG_RETURN_DATUM(result);
}
