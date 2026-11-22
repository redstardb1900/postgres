/*-------------------------------------------------------------------------
 *
 * array.h
 *	  Declarations for Postgres arrays.
 *
 * A standard varlena array has the following internal structure:
 *	  <vl_len_>		- standard varlena header word
 *	  <ndim>		- number of dimensions of the array
 *	  <dataoffset>	- offset to stored data, or 0 if no nulls bitmap
 *	  <elemtype>	- element type OID
 *	  <dimensions>	- length of each array axis (C array of int)
 *	  <lower bnds>	- lower boundary of each dimension (C array of int)
 *	  <null bitmap> - bitmap showing locations of nulls (OPTIONAL)
 *	  <actual data> - whatever is the stored data
 *
 * The <dimensions> and <lower bnds> arrays each have ndim elements.
 *
 * The <null bitmap> may be omitted if the array contains no NULL elements.
 * If it is absent, the <dataoffset> field is zero and the offset to the
 * stored data must be computed on-the-fly.  If the bitmap is present,
 * <dataoffset> is nonzero and is equal to the offset from the array start
 * to the first data element (including any alignment padding).  The bitmap
 * follows the same conventions as tuple null bitmaps, ie, a 1 indicates
 * a non-null entry and the LSB of each bitmap byte is used first.
 *
 * The actual data starts on a MAXALIGN boundary.  Individual items in the
 * array are aligned as specified by the array element type.  They are
 * stored in row-major order (last subscript varies most rapidly).
 *
 * NOTE: it is important that array elements of toastable datatypes NOT be
 * toasted, since the tupletoaster won't know they are there.  (We could
 * support compressed toasted items; only out-of-line items are dangerous.
 * However, it seems preferable to store such items uncompressed and allow
 * the toaster to compress the whole array as one input.)
 *
 *
 * The OIDVECTOR and INT2VECTOR datatypes are storage-compatible with
 * generic arrays, but they support only one-dimensional arrays with no
 * nulls (and no null bitmap).  They don't support being toasted, either.
 *
 * There are also some "fixed-length array" datatypes, such as NAME and
 * POINT.  These are simply a sequence of a fixed number of items each
 * of a fixed-length datatype, with no overhead; the item size must be
 * a multiple of its alignment requirement, because we do no padding.
 * We support subscripting on these types, but array_in() and array_out()
 * only work with varlena arrays.
 *
 * In addition, arrays are a major user of the "expanded object" TOAST
 * infrastructure.  This allows a varlena array to be converted to a
 * separate representation that may include "deconstructed" Datum/isnull
 * arrays holding the elements.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/array.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ARRAY_H
#define ARRAY_H

#include "fmgr.h"
#include "utils/expandeddatum.h"

/*  避免在此包含 execnodes.h  */
struct ExprState;
struct ExprContext;


/*
 * 数组下标最大个数（任意上限）
 */
#define MAXDIM 6

/*
 * 数组中元素的最大个数。限制在约两亿五千万以内，
 * 以免多处不必检查溢出——例如为 Datum 数组
 * palloc 时。
 */
#define MaxArraySize ((Size) (MaxAllocSize / sizeof(Datum)))

/*
 * Arrays are varlena objects, so must meet the varlena convention that
 * the first int32 of the object contains the total object size in bytes.
 * Be sure to use VARSIZE() and SET_VARSIZE() to access it, though!
 *
 * CAUTION: if you change the header for ordinary arrays you will also
 * need to change the headers for oidvector and int2vector!
 */
typedef struct ArrayType
{
	int32		vl_len_;		/*  varlena 头（勿直接碰！）  */
	int			ndim;			/*  维数  */
	int32		dataoffset;		/*  数据偏移,无位图则为 0  */
	Oid			elemtype;		/* element type OID */
} ArrayType;

/*
 * An expanded array is contained within a private memory context (as
 * all expanded objects must be) and has a control structure as below.
 *
 * The expanded array might contain a regular "flat" array if that was the
 * original input and we've not modified it significantly.  Otherwise, the
 * contents are represented by Datum/isnull arrays plus dimensionality and
 * type information.  We could also have both forms, if we've deconstructed
 * the original array for access purposes but not yet changed it.  For pass-
 * by-reference element types, the Datums would point into the flat array in
 * this situation.  Once we start modifying array elements, new pass-by-ref
 * elements are separately palloc'd within the memory context.
 */
#define EA_MAGIC 689375833		/*  调试交叉核对用 ID  */

typedef struct ExpandedArrayHeader
{
	/*  展开对象的标准头  */
	ExpandedObjectHeader hdr;

	/*  标识展开数组的魔数（仅调试）  */
	int			ea_magic;

	/*  维数信息（始终有效）  */
	int			ndims;			/*  维数  */
	int		   *dims;			/*  各维长度  */
	int		   *lbound;			/*  各维下标下界  */

	/* Element type info (always valid) */
	Oid			element_type;	/* element type OID */
	int16		typlen;			/*  元素数据类型所需信息  */
	bool		typbyval;
	char		typalign;

	/*
	 * If we have a Datum-array representation of the array, it's kept here;
	 * else dvalues/dnulls are NULL.  The dvalues and dnulls arrays are always
	 * palloc'd within the object private context, but may change size from
	 * time to time.  For pass-by-ref element types, dvalues entries might
	 * point either into the fstartptr..fendptr area, or to separately
	 * palloc'd chunks.  Elements should always be fully detoasted, as they
	 * are in the standard flat representation.
	 *
	 * Even when dvalues is valid, dnulls can be NULL if there are no null
	 * elements.
	 */
	Datum	   *dvalues;		/*  Datum 数组  */
	bool	   *dnulls;			/*  Datum 的空值标志数组  */
	int			dvalueslen;		/*  上述数组的已分配长度  */
	int			nelems;			/*  上述数组中的有效项数  */

	/*
	 * flat_size 是展开数组扁平等价物的当前空间需求
	 * (若已知)；否则为 0。存此以使连续调用
	 * get_flat_size 廉价。
	 */
	Size		flat_size;

	/*
	 * fvalue points to the flat representation if it is valid, else it is
	 * NULL.  If we have or ever had a flat representation then
	 * fstartptr/fendptr point to the start and end+1 of its data area; this
	 * is so that we can tell which Datum pointers point into the flat
	 * representation rather than being pointers to separately palloc'd data.
	 */
	ArrayType  *fvalue;			/* must be a fully detoasted array */
	char	   *fstartptr;		/*  其数据区起始  */
	char	   *fendptr;		/*  其数据区结束+1  */
} ExpandedArrayHeader;

/*
 * Functions that can handle either a "flat" varlena array or an expanded
 * array use this union to work with their input.  Don't refer to "flt";
 * instead, cast to ArrayType.  This struct nominally requires 8-byte
 * alignment on 64-bit, but it's often used for an ArrayType having 4-byte
 * alignment.  UBSan complains about referencing "flt" in such cases.
 */
typedef union AnyArrayType
{
	ArrayType	flt;
	ExpandedArrayHeader xpn;
} AnyArrayType;

/*
 * accumArrayResult() 及同伴的工作状态
 * 注意输入必须是标量（合法数组元素）
 */
typedef struct ArrayBuildState
{
	MemoryContext mcontext;		/*  全部临时物所在之处  */
	Datum	   *dvalues;		/*  已累积 Datum 的数组  */
	bool	   *dnulls;			/*  Datum 的空值标志数组  */
	int			alen;			/*  上述数组的已分配长度  */
	int			nelems;			/*  上述数组中的有效项数  */
	Oid			element_type;	/* data type of the Datums */
	int16		typlen;			/* needed info about datatype */
	bool		typbyval;
	char		typalign;
	bool		private_cxt;	/* use private memory context */
} ArrayBuildState;

/*
 * accumArrayResultArr() 及同伴的工作状态
 * 注意输入必须是数组，并返回相同数组类型
 */
typedef struct ArrayBuildStateArr
{
	MemoryContext mcontext;		/*  全部临时物所在之处  */
	char	   *data;			/* accumulated data */
	bits8	   *nullbitmap;		/*  空值标志位图，无则为 NULL  */
	int			abytes;			/* allocated length of "data" */
	int			nbytes;			/*  已用字节数  */
	int			aitems;			/*  位图已分配长度(以元素计)  */
	int			nitems;			/*  结果中元素总数  */
	int			ndims;			/*  结果当前维数  */
	int			dims[MAXDIM];
	int			lbs[MAXDIM];
	Oid			array_type;		/* data type of the arrays */
	Oid			element_type;	/*  数组元素的数据类型  */
	bool		private_cxt;	/* use private memory context */
} ArrayBuildStateArr;

/*
 * accumArrayResultAny() 及同伴的工作状态
 * 这些函数处理两种情形
 */
typedef struct ArrayBuildStateAny
{
	/*  以下恰有一个非 NULL：  */
	ArrayBuildState *scalarstate;
	ArrayBuildStateArr *arraystate;
} ArrayBuildStateAny;

/*
 * 缓存数组操作所需类型元数据的结构
 */
typedef struct ArrayMetaState
{
	Oid			element_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			typiofunc;
	FmgrInfo	proc;
} ArrayMetaState;

/*
 * array_map 所需私有状态（因调用方须提供）
 */
typedef struct ArrayMapState
{
	ArrayMetaState inp_extra;
	ArrayMetaState ret_extra;
} ArrayMapState;

/*  ArrayIteratorData 为 arrayfuncs.c 私有  */
typedef struct ArrayIteratorData *ArrayIterator;

/*  普通 varlena 数组对象的 fmgr 宏  */
#define DatumGetArrayTypeP(X)		  ((ArrayType *) PG_DETOAST_DATUM(X))
#define DatumGetArrayTypePCopy(X)	  ((ArrayType *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_ARRAYTYPE_P(n)	  DatumGetArrayTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_ARRAYTYPE_P_COPY(n) DatumGetArrayTypePCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_ARRAYTYPE_P(x)	  PG_RETURN_POINTER(x)

/*  展开数组对象的 fmgr 宏  */
#define PG_GETARG_EXPANDED_ARRAY(n)  DatumGetExpandedArray(PG_GETARG_DATUM(n))
#define PG_GETARG_EXPANDED_ARRAYX(n, metacache) \
	DatumGetExpandedArrayX(PG_GETARG_DATUM(n), metacache)
#define PG_RETURN_EXPANDED_ARRAY(x)  PG_RETURN_DATUM(EOHPGetRWDatum(&(x)->hdr))

/*  AnyArrayType 的 fmgr 宏（取 varlena 或展开形式）  */
#define PG_GETARG_ANY_ARRAY_P(n)	DatumGetAnyArrayP(PG_GETARG_DATUM(n))

/*
 * Access macros for varlena array header fields.
 *
 * ARR_DIMS returns a pointer to an array of array dimensions (number of
 * elements along the various array axes).
 *
 * ARR_LBOUND returns a pointer to an array of array lower bounds.
 *
 * That is: if the third axis of an array has elements 5 through 8, then
 * ARR_DIMS(a)[2] == 4 and ARR_LBOUND(a)[2] == 5.
 *
 * Unlike C, the default lower bound is 1.
 */
#define ARR_SIZE(a)				VARSIZE(a)
#define ARR_NDIM(a)				((a)->ndim)
#define ARR_HASNULL(a)			((a)->dataoffset != 0)
#define ARR_ELEMTYPE(a)			((a)->elemtype)

#define ARR_DIMS(a) \
		((int *) (((char *) (a)) + sizeof(ArrayType)))
#define ARR_LBOUND(a) \
		((int *) (((char *) (a)) + sizeof(ArrayType) + \
				  sizeof(int) * ARR_NDIM(a)))

#define ARR_NULLBITMAP(a) \
		(ARR_HASNULL(a) ? \
		 (bits8 *) (((char *) (a)) + sizeof(ArrayType) + \
					2 * sizeof(int) * ARR_NDIM(a)) \
		 : (bits8 *) NULL)

/*
 * 具有指定维数与总项数的数组的总头大小（字节）。
 * 见上。
 */
#define ARR_OVERHEAD_NONULLS(ndims) \
		MAXALIGN(sizeof(ArrayType) + 2 * sizeof(int) * (ndims))
#define ARR_OVERHEAD_WITHNULLS(ndims, nitems) \
		MAXALIGN(sizeof(ArrayType) + 2 * sizeof(int) * (ndims) + \
				 ((nitems) + 7) / 8)

#define ARR_DATA_OFFSET(a) \
		(ARR_HASNULL(a) ? (a)->dataoffset : ARR_OVERHEAD_NONULLS(ARR_NDIM(a)))

/*
 * 返回指向实际数组数据的指针。
 */
#define ARR_DATA_PTR(a) \
		(((char *) (a)) + ARR_DATA_OFFSET(a))

/*
 * 处理 AnyArrayType 输入的宏。当心多次求值！
 */
#define AARR_NDIM(a) \
	(VARATT_IS_EXPANDED_HEADER(a) ? \
	 (a)->xpn.ndims : ARR_NDIM((ArrayType *) (a)))
#define AARR_HASNULL(a) \
	(VARATT_IS_EXPANDED_HEADER(a) ? \
	 ((a)->xpn.dvalues != NULL ? (a)->xpn.dnulls != NULL : ARR_HASNULL((a)->xpn.fvalue)) : \
	 ARR_HASNULL((ArrayType *) (a)))
#define AARR_ELEMTYPE(a) \
	(VARATT_IS_EXPANDED_HEADER(a) ? \
	 (a)->xpn.element_type : ARR_ELEMTYPE((ArrayType *) (a)))
#define AARR_DIMS(a) \
	(VARATT_IS_EXPANDED_HEADER(a) ? \
	 (a)->xpn.dims : ARR_DIMS((ArrayType *) (a)))
#define AARR_LBOUND(a) \
	(VARATT_IS_EXPANDED_HEADER(a) ? \
	 (a)->xpn.lbound : ARR_LBOUND((ArrayType *) (a)))


/*
 * GUC 参数
 */
extern PGDLLIMPORT bool Array_nulls;

/*
 * arrayfuncs.c 中定义的函数原型
 */
extern void CopyArrayEls(ArrayType *array,
						 Datum *values,
						 bool *nulls,
						 int nitems,
						 int typlen,
						 bool typbyval,
						 char typalign,
						 bool freedata);

extern Datum array_get_element(Datum arraydatum, int nSubscripts, int *indx,
							   int arraytyplen, int elmlen, bool elmbyval, char elmalign,
							   bool *isNull);
extern Datum array_set_element(Datum arraydatum, int nSubscripts, int *indx,
							   Datum dataValue, bool isNull,
							   int arraytyplen, int elmlen, bool elmbyval, char elmalign);
extern Datum array_get_slice(Datum arraydatum, int nSubscripts,
							 int *upperIndx, int *lowerIndx,
							 bool *upperProvided, bool *lowerProvided,
							 int arraytyplen, int elmlen, bool elmbyval, char elmalign);
extern Datum array_set_slice(Datum arraydatum, int nSubscripts,
							 int *upperIndx, int *lowerIndx,
							 bool *upperProvided, bool *lowerProvided,
							 Datum srcArrayDatum, bool isNull,
							 int arraytyplen, int elmlen, bool elmbyval, char elmalign);

extern Datum array_ref(ArrayType *array, int nSubscripts, int *indx,
					   int arraytyplen, int elmlen, bool elmbyval, char elmalign,
					   bool *isNull);
extern ArrayType *array_set(ArrayType *array, int nSubscripts, int *indx,
							Datum dataValue, bool isNull,
							int arraytyplen, int elmlen, bool elmbyval, char elmalign);

extern Datum array_map(Datum arrayd,
					   struct ExprState *exprstate, struct ExprContext *econtext,
					   Oid retType, ArrayMapState *amstate);

extern void array_bitmap_copy(bits8 *destbitmap, int destoffset,
							  const bits8 *srcbitmap, int srcoffset,
							  int nitems);

extern ArrayType *construct_array(Datum *elems, int nelems,
								  Oid elmtype,
								  int elmlen, bool elmbyval, char elmalign);
extern ArrayType *construct_array_builtin(Datum *elems, int nelems, Oid elmtype);
extern ArrayType *construct_md_array(Datum *elems,
									 bool *nulls,
									 int ndims,
									 int *dims,
									 int *lbs,
									 Oid elmtype, int elmlen, bool elmbyval, char elmalign);
extern ArrayType *construct_empty_array(Oid elmtype);
extern ExpandedArrayHeader *construct_empty_expanded_array(Oid element_type,
														   MemoryContext parentcontext,
														   ArrayMetaState *metacache);
extern void deconstruct_array(ArrayType *array,
							  Oid elmtype,
							  int elmlen, bool elmbyval, char elmalign,
							  Datum **elemsp, bool **nullsp, int *nelemsp);
extern void deconstruct_array_builtin(ArrayType *array,
									  Oid elmtype,
									  Datum **elemsp, bool **nullsp, int *nelemsp);
extern bool array_contains_nulls(ArrayType *array);

extern ArrayBuildState *initArrayResult(Oid element_type,
										MemoryContext rcontext, bool subcontext);
extern ArrayBuildState *initArrayResultWithSize(Oid element_type,
												MemoryContext rcontext,
												bool subcontext, int initsize);
extern ArrayBuildState *accumArrayResult(ArrayBuildState *astate,
										 Datum dvalue, bool disnull,
										 Oid element_type,
										 MemoryContext rcontext);
extern Datum makeArrayResult(ArrayBuildState *astate,
							 MemoryContext rcontext);
extern Datum makeMdArrayResult(ArrayBuildState *astate, int ndims,
							   int *dims, int *lbs, MemoryContext rcontext, bool release);

extern ArrayBuildStateArr *initArrayResultArr(Oid array_type, Oid element_type,
											  MemoryContext rcontext, bool subcontext);
extern ArrayBuildStateArr *accumArrayResultArr(ArrayBuildStateArr *astate,
											   Datum dvalue, bool disnull,
											   Oid array_type,
											   MemoryContext rcontext);
extern Datum makeArrayResultArr(ArrayBuildStateArr *astate,
								MemoryContext rcontext, bool release);

extern ArrayBuildStateAny *initArrayResultAny(Oid input_type,
											  MemoryContext rcontext, bool subcontext);
extern ArrayBuildStateAny *accumArrayResultAny(ArrayBuildStateAny *astate,
											   Datum dvalue, bool disnull,
											   Oid input_type,
											   MemoryContext rcontext);
extern Datum makeArrayResultAny(ArrayBuildStateAny *astate,
								MemoryContext rcontext, bool release);

extern ArrayIterator array_create_iterator(ArrayType *arr, int slice_ndim, ArrayMetaState *mstate);
extern bool array_iterate(ArrayIterator iterator, Datum *value, bool *isnull);
extern void array_free_iterator(ArrayIterator iterator);

/*
 * arrayutils.c 中定义的函数原型
 */

extern int	ArrayGetOffset(int n, const int *dim, const int *lb, const int *indx);
extern int	ArrayGetOffset0(int n, const int *tup, const int *scale);
extern int	ArrayGetNItems(int ndim, const int *dims);
extern int	ArrayGetNItemsSafe(int ndim, const int *dims,
							   struct Node *escontext);
extern void ArrayCheckBounds(int ndim, const int *dims, const int *lb);
extern bool ArrayCheckBoundsSafe(int ndim, const int *dims, const int *lb,
								 struct Node *escontext);
extern void mda_get_range(int n, int *span, const int *st, const int *endp);
extern void mda_get_prod(int n, const int *range, int *prod);
extern void mda_get_offset_values(int n, int *dist, const int *prod, const int *span);
extern int	mda_next_tuple(int n, int *curr, const int *span);
extern int32 *ArrayGetIntegerTypmods(ArrayType *arr, int *n);

/*
 * array_expanded.c 中定义的函数原型
 */
extern Datum expand_array(Datum arraydatum, MemoryContext parentcontext,
						  ArrayMetaState *metacache);
extern ExpandedArrayHeader *DatumGetExpandedArray(Datum d);
extern ExpandedArrayHeader *DatumGetExpandedArrayX(Datum d,
												   ArrayMetaState *metacache);
extern AnyArrayType *DatumGetAnyArrayP(Datum d);
extern void deconstruct_expanded_array(ExpandedArrayHeader *eah);

#endif							/* ARRAY_H */
