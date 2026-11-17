/*-------------------------------------------------------------------------
 *
 * multirangetypes.c
 *	  多范围类型的 I/O 函数、算子与支持函数。
 *
 * 多范围值的存储（序列化）格式如下：
 *
 *	12 字节：MultirangeType 结构，包括 varlena 头、多范围
 *			  类型的 OID 以及多范围中范围的个数。
 *	4 * (rangesCount - 1) 字节：32 位条目，指向多范围中从
 *								 第二个范围起的
 *								 各范围。
 *	1 * rangesCount 字节：多范围中每个范围的 8 位标志
 *	多范围其余部分为这些条目所指向的范围边界
 *	值。
 *
 *	大多数条目包含相应范围边界值的长度。
 *	因此条目通常是很小的数字。这使多范围
 *	利于压缩。每 MULTIRANGE_ITEM_OFFSET_STRIDE 个条目
 *	包含一个相应范围边界值的偏移量。这样可以快速
 *	查找特定的范围下标。偏移量从按边界类型对齐后的
 *	标志结尾处开始计数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/multirangetypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/multirangetypes.h"
#include "utils/array.h"
#include "utils/memutils.h"

/* 某个范围 I/O 函数的 fn_extra 缓存条目 */
typedef struct MultirangeIOData
{
	TypeCacheEntry *typcache;	/* 多范围类型 typcache 条目 */
	FmgrInfo	typioproc;		/* 范围 I/O 过程 */
	Oid			typioparam;		/* 范围类型的 I/O 参数 */
} MultirangeIOData;

typedef enum
{
	MULTIRANGE_BEFORE_RANGE,
	MULTIRANGE_IN_RANGE,
	MULTIRANGE_IN_RANGE_ESCAPED,
	MULTIRANGE_IN_RANGE_QUOTED,
	MULTIRANGE_IN_RANGE_QUOTED_ESCAPED,
	MULTIRANGE_AFTER_RANGE,
	MULTIRANGE_FINISHED,
} MultirangeParseState;

/*
 * 用于访问多范围中 MultirangeType 之后各部分：条目、标志
 * 与边界。
 */
#define MultirangeGetItemsPtr(mr) ((uint32 *) ((Pointer) (mr) + \
	sizeof(MultirangeType)))
#define MultirangeGetFlagsPtr(mr) ((uint8 *) ((Pointer) (mr) + \
	sizeof(MultirangeType) + ((mr)->rangeCount - 1) * sizeof(uint32)))
#define MultirangeGetBoundariesPtr(mr, align) ((Pointer) (mr) + \
	att_align_nominal(sizeof(MultirangeType) + \
		((mr)->rangeCount - 1) * sizeof(uint32) + \
		(mr)->rangeCount * sizeof(uint8), (align)))

#define MULTIRANGE_ITEM_OFF_BIT 0x80000000
#define MULTIRANGE_ITEM_GET_OFFLEN(item) ((item) & 0x7FFFFFFF)
#define MULTIRANGE_ITEM_HAS_OFF(item) ((item) & MULTIRANGE_ITEM_OFF_BIT)
#define MULTIRANGE_ITEM_OFFSET_STRIDE 4

typedef int (*multirange_bsearch_comparison) (TypeCacheEntry *typcache,
											  RangeBound *lower,
											  RangeBound *upper,
											  void *key,
											  bool *match);

static MultirangeIOData *get_multirange_io_data(FunctionCallInfo fcinfo,
												Oid mltrngtypid,
												IOFuncSelector func);
static int32 multirange_canonicalize(TypeCacheEntry *rangetyp,
									 int32 input_range_count,
									 RangeType **ranges);

/*
 *----------------------------------------------------------
 * I/O 函数
 *----------------------------------------------------------
 */

/*
 * 把字符串转为多范围。
 *
 * 我们期望花括号界定列表，其中包含零个或多个
 * 以逗号分隔的范围。我们接受空白出现在任何地方：
 * 括号前后以及逗号周围。范围可以是空字面值，
 * 或是圆括号/方括号内的内容。我们大多把解析单个
 * 范围内容交给 range_in 解析，但须检测范围边界
 * 可能出现的引号与反斜杠转义。反斜杠既可以在
 * 带引号字符串之内也可以在其之外转义字符，而带
 * 引号字符串可用反斜杠或连续两个双引号转义引号。
 */
Datum
multirange_in(PG_FUNCTION_ARGS)
{
	char	   *input_str = PG_GETARG_CSTRING(0);
	Oid			mltrngtypoid = PG_GETARG_OID(1);
	Oid			typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	TypeCacheEntry *rangetyp;
	int32		ranges_seen = 0;
	int32		range_count = 0;
	int32		range_capacity = 8;
	RangeType  *range;
	RangeType **ranges = palloc(range_capacity * sizeof(RangeType *));
	MultirangeIOData *cache;
	MultirangeType *ret;
	MultirangeParseState parse_state;
	const char *ptr = input_str;
	const char *range_str_begin = NULL;
	int32		range_str_len;
	char	   *range_str;
	Datum		range_datum;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_input);
	rangetyp = cache->typcache->rngtype;

	/* 消耗空白 */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (*ptr == '{')
		ptr++;
	else
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed multirange literal: \"%s\"",
						input_str),
				 errdetail("Missing left brace.")));

	/* 消耗范围 */
	parse_state = MULTIRANGE_BEFORE_RANGE;
	for (; parse_state != MULTIRANGE_FINISHED; ptr++)
	{
		char		ch = *ptr;

		if (ch == '\0')
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed multirange literal: \"%s\"",
							input_str),
					 errdetail("Unexpected end of input.")));

		/* 跳过空白 */
		if (isspace((unsigned char) ch))
			continue;

		switch (parse_state)
		{
			case MULTIRANGE_BEFORE_RANGE:
				if (ch == '[' || ch == '(')
				{
					range_str_begin = ptr;
					parse_state = MULTIRANGE_IN_RANGE;
				}
				else if (ch == '}' && ranges_seen == 0)
					parse_state = MULTIRANGE_FINISHED;
				else if (pg_strncasecmp(ptr, RANGE_EMPTY_LITERAL,
										strlen(RANGE_EMPTY_LITERAL)) == 0)
				{
					ranges_seen++;
					/* 对空范围无需任何处理 */
					ptr += strlen(RANGE_EMPTY_LITERAL) - 1;
					parse_state = MULTIRANGE_AFTER_RANGE;
				}
				else
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed multirange literal: \"%s\"",
									input_str),
							 errdetail("Expected range start.")));
				break;
			case MULTIRANGE_IN_RANGE:
				if (ch == ']' || ch == ')')
				{
					range_str_len = ptr - range_str_begin + 1;
					range_str = pnstrdup(range_str_begin, range_str_len);
					if (range_capacity == range_count)
					{
						range_capacity *= 2;
						ranges = (RangeType **)
							repalloc(ranges, range_capacity * sizeof(RangeType *));
					}
					ranges_seen++;
					if (!InputFunctionCallSafe(&cache->typioproc,
											   range_str,
											   cache->typioparam,
											   typmod,
											   escontext,
											   &range_datum))
						PG_RETURN_NULL();
					range = DatumGetRangeTypeP(range_datum);
					if (!RangeIsEmpty(range))
						ranges[range_count++] = range;
					parse_state = MULTIRANGE_AFTER_RANGE;
				}
				else
				{
					if (ch == '"')
						parse_state = MULTIRANGE_IN_RANGE_QUOTED;
					else if (ch == '\\')
						parse_state = MULTIRANGE_IN_RANGE_ESCAPED;

					/*
					 * 一旦找到范围值结尾，我们就
					 * 会把它纳入 range_str。
					 */
				}
				break;
			case MULTIRANGE_IN_RANGE_ESCAPED:

				/*
				 * 一旦找到范围值的结尾，我们就会
				 * 把它纳入 range_str。
				 */
				parse_state = MULTIRANGE_IN_RANGE;
				break;
			case MULTIRANGE_IN_RANGE_QUOTED:
				if (ch == '"')
					if (*(ptr + 1) == '"')
					{
						/* 连续两个引号表示转义过的引号 */
						ptr++;
					}
					else
						parse_state = MULTIRANGE_IN_RANGE;
				else if (ch == '\\')
					parse_state = MULTIRANGE_IN_RANGE_QUOTED_ESCAPED;

				/*
				 * 一旦找到范围值的结尾，我们就会
				 * 把它纳入 range_str。
				 */
				break;
			case MULTIRANGE_AFTER_RANGE:
				if (ch == ',')
					parse_state = MULTIRANGE_BEFORE_RANGE;
				else if (ch == '}')
					parse_state = MULTIRANGE_FINISHED;
				else
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed multirange literal: \"%s\"",
									input_str),
							 errdetail("Expected comma or end of multirange.")));
				break;
			case MULTIRANGE_IN_RANGE_QUOTED_ESCAPED:

				/*
				 * 一旦找到范围值的结尾，我们就会
				 * 把它纳入 range_str。
				 */
				parse_state = MULTIRANGE_IN_RANGE_QUOTED;
				break;
			default:
				elog(ERROR, "unknown parse state: %d", parse_state);
		}
	}

	/* 消耗空白 */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (*ptr != '\0')
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed multirange literal: \"%s\"",
						input_str),
				 errdetail("Junk after closing right brace.")));

	ret = make_multirange(mltrngtypoid, rangetyp, range_count, ranges);
	PG_RETURN_MULTIRANGE_P(ret);
}

Datum
multirange_out(PG_FUNCTION_ARGS)
{
	MultirangeType *multirange = PG_GETARG_MULTIRANGE_P(0);
	Oid			mltrngtypoid = MultirangeTypeGetOid(multirange);
	MultirangeIOData *cache;
	StringInfoData buf;
	RangeType  *range;
	char	   *rangeStr;
	int32		range_count;
	int32		i;
	RangeType **ranges;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_output);

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '{');

	multirange_deserialize(cache->typcache->rngtype, multirange, &range_count, &ranges);
	for (i = 0; i < range_count; i++)
	{
		if (i > 0)
			appendStringInfoChar(&buf, ',');
		range = ranges[i];
		rangeStr = OutputFunctionCall(&cache->typioproc, RangeTypePGetDatum(range));
		appendStringInfoString(&buf, rangeStr);
	}

	appendStringInfoChar(&buf, '}');

	PG_RETURN_CSTRING(buf.data);
}

/*
 * 二进制表示：先是一个 int32 大小的范围个数，
 * 随后是按原生二进制表示的范围。
 */
Datum
multirange_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			mltrngtypoid = PG_GETARG_OID(1);
	int32		typmod = PG_GETARG_INT32(2);
	MultirangeIOData *cache;
	uint32		range_count;
	RangeType **ranges;
	MultirangeType *ret;
	StringInfoData tmpbuf;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_receive);

	range_count = pq_getmsgint(buf, 4);
	ranges = palloc(range_count * sizeof(RangeType *));

	initStringInfo(&tmpbuf);
	for (int i = 0; i < range_count; i++)
	{
		uint32		range_len = pq_getmsgint(buf, 4);
		const char *range_data = pq_getmsgbytes(buf, range_len);

		resetStringInfo(&tmpbuf);
		appendBinaryStringInfo(&tmpbuf, range_data, range_len);

		ranges[i] = DatumGetRangeTypeP(ReceiveFunctionCall(&cache->typioproc,
														   &tmpbuf,
														   cache->typioparam,
														   typmod));
	}
	pfree(tmpbuf.data);

	pq_getmsgend(buf);

	ret = make_multirange(mltrngtypoid, cache->typcache->rngtype,
						  range_count, ranges);
	PG_RETURN_MULTIRANGE_P(ret);
}

Datum
multirange_send(PG_FUNCTION_ARGS)
{
	MultirangeType *multirange = PG_GETARG_MULTIRANGE_P(0);
	Oid			mltrngtypoid = MultirangeTypeGetOid(multirange);
	StringInfo	buf = makeStringInfo();
	RangeType **ranges;
	int32		range_count;
	MultirangeIOData *cache;

	cache = get_multirange_io_data(fcinfo, mltrngtypoid, IOFunc_send);

	/* 构造输出 */
	pq_begintypsend(buf);

	pq_sendint32(buf, multirange->rangeCount);

	multirange_deserialize(cache->typcache->rngtype, multirange, &range_count, &ranges);
	for (int i = 0; i < range_count; i++)
	{
		Datum		range;

		range = RangeTypePGetDatum(ranges[i]);
		range = PointerGetDatum(SendFunctionCall(&cache->typioproc, range));

		pq_sendint32(buf, VARSIZE(range) - VARHDRSZ);
		pq_sendbytes(buf, VARDATA(range), VARSIZE(range) - VARHDRSZ);
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(buf));
}

/*
 * get_multirange_io_data：获取多范围类型 I/O 所需的缓存信息
 *
 * 多范围 I/O 函数比其他多范围函数需要更多的缓存
 * 信息，所以它们在 fn_extra 中存储 MultirangeIOData 结构，
 * 而非仅指针。
 */
static MultirangeIOData *
get_multirange_io_data(FunctionCallInfo fcinfo, Oid mltrngtypid, IOFuncSelector func)
{
	MultirangeIOData *cache = (MultirangeIOData *) fcinfo->flinfo->fn_extra;

	if (cache == NULL || cache->typcache->type_id != mltrngtypid)
	{
		Oid			typiofunc;
		int16		typlen;
		bool		typbyval;
		char		typalign;
		char		typdelim;

		cache = (MultirangeIOData *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
														sizeof(MultirangeIOData));
		cache->typcache = lookup_type_cache(mltrngtypid, TYPECACHE_MULTIRANGE_INFO);
		if (cache->typcache->rngtype == NULL)
			elog(ERROR, "type %u is not a multirange type", mltrngtypid);

		/* get_type_io_data 做的事多于所需，但很方便 */
		get_type_io_data(cache->typcache->rngtype->type_id,
						 func,
						 &typlen,
						 &typbyval,
						 &typalign,
						 &typdelim,
						 &cache->typioparam,
						 &typiofunc);

		if (!OidIsValid(typiofunc))
		{
			/* 这只可能发生在接收或发送时 */
			if (func == IOFunc_receive)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no binary input function available for type %s",
								format_type_be(cache->typcache->rngtype->type_id))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no binary output function available for type %s",
								format_type_be(cache->typcache->rngtype->type_id))));
		}
		fmgr_info_cxt(typiofunc, &cache->typioproc,
					  fcinfo->flinfo->fn_mcxt);

		fcinfo->flinfo->fn_extra = (void *) cache;
	}

	return cache;
}

/*
 * 把任意范围的列表转换成已排序且已合并的列表。
 * 会改变 `ranges` 的内容。
 *
 * 返回实际使用的槽位数，可能小于
 * input_range_count，绝不超。
 *
 * 假定输入范围均非 NULL，但允许空范围。
 */
static int32
multirange_canonicalize(TypeCacheEntry *rangetyp, int32 input_range_count,
						RangeType **ranges)
{
	RangeType  *lastRange = NULL;
	RangeType  *currentRange;
	int32		i;
	int32		output_range_count = 0;

	/* 对范围排序，以便找出重叠/相接的范围。 */
	qsort_arg(ranges, input_range_count, sizeof(RangeType *), range_compare,
			  rangetyp);

	/* 现在尽可能合并： */
	for (i = 0; i < input_range_count; i++)
	{
		currentRange = ranges[i];
		if (RangeIsEmpty(currentRange))
			continue;

		if (lastRange == NULL)
		{
			ranges[output_range_count++] = lastRange = currentRange;
			continue;
		}

		/*
		 * 只要 *A 相接于 B* 或 *B 相接于 A* 之一，
		 * range_adjacent_internal 就返回真，这并非所要，但
		 * 靠排序可排除 B 相接于 A。
		 */
		if (range_adjacent_internal(rangetyp, lastRange, currentRange))
		{
			/* 两个范围相触（不重叠），故合并它们： */
			ranges[output_range_count - 1] = lastRange =
				range_union_internal(rangetyp, lastRange, currentRange, false);
		}
		else if (range_before_internal(rangetyp, lastRange, currentRange))
		{
			/* 有间隙，故创建新条目： */
			lastRange = ranges[output_range_count] = currentRange;
			output_range_count++;
		}
		else
		{
			/* 它们必然重叠，故合并： */
			ranges[output_range_count - 1] = lastRange =
				range_union_internal(rangetyp, lastRange, currentRange, true);
		}
	}

	return output_range_count;
}

/*
 *----------------------------------------------------------
 * 支持函数
 *
 *	 这些函数不在 pg_proc 中，但对 C 里定义
 *	 新的通用多范围函数很有用。
 *----------------------------------------------------------
 */

/*
 * multirange_get_typcache：获取多范围类型的缓存信息
 *
 * 供遵循以下惯例的多范围相关函数使用：把 fn_extra
 * 字段用作指向多范围类型的类型缓存条目的指针。
 * 需要缓存更多信息的多范围函数
 * 必须自行设法。
 */
TypeCacheEntry *
multirange_get_typcache(FunctionCallInfo fcinfo, Oid mltrngtypid)
{
	TypeCacheEntry *typcache = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;

	if (typcache == NULL ||
		typcache->type_id != mltrngtypid)
	{
		typcache = lookup_type_cache(mltrngtypid, TYPECACHE_MULTIRANGE_INFO);
		if (typcache->rngtype == NULL)
			elog(ERROR, "type %u is not a multirange type", mltrngtypid);
		fcinfo->flinfo->fn_extra = (void *) typcache;
	}

	return typcache;
}


/*
 * 估算序列化多范围占用的空间大小。
 */
static Size
multirange_size_estimate(TypeCacheEntry *rangetyp, int32 range_count,
						 RangeType **ranges)
{
	char		elemalign = rangetyp->rngelemtype->typalign;
	Size		size;
	int32		i;

	/*
	 * 为 MultirangeType 结构/条目/标志计空间。
	 */
	size = att_align_nominal(sizeof(MultirangeType) +
							 Max(range_count - 1, 0) * sizeof(uint32) +
							 range_count * sizeof(uint8), elemalign);

	/* 为范围边界值计空间 */
	for (i = 0; i < range_count; i++)
		size += att_align_nominal(VARSIZE(ranges[i]) -
								  sizeof(RangeType) -
								  sizeof(char), elemalign);

	return size;
}

/*
 * 把多范围数据写入预分配的空间。
 */
static void
write_multirange_data(MultirangeType *multirange, TypeCacheEntry *rangetyp,
					  int32 range_count, RangeType **ranges)
{
	uint32	   *items;
	uint32		prev_offset = 0;
	uint8	   *flags;
	int32		i;
	Pointer		begin,
				ptr;
	char		elemalign = rangetyp->rngelemtype->typalign;

	items = MultirangeGetItemsPtr(multirange);
	flags = MultirangeGetFlagsPtr(multirange);
	ptr = begin = MultirangeGetBoundariesPtr(multirange, elemalign);
	for (i = 0; i < range_count; i++)
	{
		uint32		len;

		if (i > 0)
		{
			/*
			 * 除首个范围外，各范围均有一个条目。
			 * 每 MULTIRANGE_ITEM_OFFSET_STRIDE 个条目有偏移，其余
			 * 是长度。
			 */
			items[i - 1] = ptr - begin;
			if ((i % MULTIRANGE_ITEM_OFFSET_STRIDE) != 0)
				items[i - 1] -= prev_offset;
			else
				items[i - 1] |= MULTIRANGE_ITEM_OFF_BIT;
			prev_offset = ptr - begin;
		}
		flags[i] = *((Pointer) ranges[i] + VARSIZE(ranges[i]) - sizeof(char));
		len = VARSIZE(ranges[i]) - sizeof(RangeType) - sizeof(char);
		memcpy(ptr, (Pointer) (ranges[i] + 1), len);
		ptr += att_align_nominal(len, elemalign);
	}
}


/*
 * 本函数从非 NULL 范围的列表序列化多范围。它还会
 * 对范围排序并合并相触的范围。范围应已被
 * 解 TOAST，且其中不应有 NULL。大多数调用者都应使用
 * 它。
 *
 * 注意：我们可能改变 `ranges` 参数（仅指针，
 * 已存在 RangeType 内容不变）。
 */
MultirangeType *
make_multirange(Oid mltrngtypoid, TypeCacheEntry *rangetyp, int32 range_count,
				RangeType **ranges)
{
	MultirangeType *multirange;
	Size		size;

	/* 排序合并输入范围。 */
	range_count = multirange_canonicalize(rangetyp, range_count, ranges);

	/* 注意：此处需零填充，就像堆元组一样 */
	size = multirange_size_estimate(rangetyp, range_count, ranges);
	multirange = palloc0(size);
	SET_VARSIZE(multirange, size);

	/* 现在填入 datum */
	multirange->multirangetypid = mltrngtypoid;
	multirange->rangeCount = range_count;

	write_multirange_data(multirange, rangetyp, range_count, ranges);

	return multirange;
}

/*
 * 获取多范围中第 i 个范围边界值的偏移量。
 */
static uint32
multirange_get_bounds_offset(const MultirangeType *multirange, int32 i)
{
	uint32	   *items = MultirangeGetItemsPtr(multirange);
	uint32		offset = 0;

	/*
	 * 累加长度，直至遇到偏移量。
	 */
	while (i > 0)
	{
		offset += MULTIRANGE_ITEM_GET_OFFLEN(items[i - 1]);
		if (MULTIRANGE_ITEM_HAS_OFF(items[i - 1]))
			break;
		i--;
	}
	return offset;
}

/*
 * 从多范围中取出第 i 个范围。
 */
RangeType *
multirange_get_range(TypeCacheEntry *rangetyp,
					 const MultirangeType *multirange, int i)
{
	uint32		offset;
	uint8		flags;
	Pointer		begin,
				ptr;
	int16		typlen = rangetyp->rngelemtype->typlen;
	char		typalign = rangetyp->rngelemtype->typalign;
	uint32		len;
	RangeType  *range;

	Assert(i < multirange->rangeCount);

	offset = multirange_get_bounds_offset(multirange, i);
	flags = MultirangeGetFlagsPtr(multirange)[i];
	ptr = begin = MultirangeGetBoundariesPtr(multirange, typalign) + offset;

	/*
	 * 计算边界值的大小。原则上我们可以取下一
	 * 个范围边界值的偏移量来推算。但范围边界
	 * 值是对齐的，所以必须逐一遍历方能知其大
	 * 小。
	 */
	if (RANGE_HAS_LBOUND(flags))
		ptr = (Pointer) att_addlength_pointer(ptr, typlen, ptr);
	if (RANGE_HAS_UBOUND(flags))
	{
		ptr = (Pointer) att_align_pointer(ptr, typalign, typlen, ptr);
		ptr = (Pointer) att_addlength_pointer(ptr, typlen, ptr);
	}
	len = (ptr - begin) + sizeof(RangeType) + sizeof(uint8);

	range = palloc0(len);
	SET_VARSIZE(range, len);
	range->rangetypid = rangetyp->type_id;

	memcpy(range + 1, begin, ptr - begin);
	*((uint8 *) (range + 1) + (ptr - begin)) = flags;

	return range;
}

/*
 * 从多范围的第 i 个范围取出边界。这相当于把
 * multirange_get_range() + range_deserialize() 合
 * 而为一，操作更少。
 */
void
multirange_get_bounds(TypeCacheEntry *rangetyp,
					  const MultirangeType *multirange,
					  uint32 i, RangeBound *lower, RangeBound *upper)
{
	uint32		offset;
	uint8		flags;
	Pointer		ptr;
	int16		typlen = rangetyp->rngelemtype->typlen;
	char		typalign = rangetyp->rngelemtype->typalign;
	bool		typbyval = rangetyp->rngelemtype->typbyval;
	Datum		lbound;
	Datum		ubound;

	Assert(i < multirange->rangeCount);

	offset = multirange_get_bounds_offset(multirange, i);
	flags = MultirangeGetFlagsPtr(multirange)[i];
	ptr = MultirangeGetBoundariesPtr(multirange, typalign) + offset;

	/* 多范围不能包含空范围 */
	Assert((flags & RANGE_EMPTY) == 0);

	/* 若有则取下边界 */
	if (RANGE_HAS_LBOUND(flags))
	{
		/* att_align_pointer 在此处绝无必要 */
		lbound = fetch_att(ptr, typbyval, typlen);
		ptr = (Pointer) att_addlength_pointer(ptr, typlen, ptr);
	}
	else
		lbound = (Datum) 0;

	/* 若有则取上边界 */
	if (RANGE_HAS_UBOUND(flags))
	{
		ptr = (Pointer) att_align_pointer(ptr, typalign, typlen, ptr);
		ubound = fetch_att(ptr, typbyval, typlen);
		/* 无需 att_addlength_pointer */
	}
	else
		ubound = (Datum) 0;

	/* 输出结果 */
	lower->val = lbound;
	lower->infinite = (flags & RANGE_LB_INF) != 0;
	lower->inclusive = (flags & RANGE_LB_INC) != 0;
	lower->lower = true;

	upper->val = ubound;
	upper->infinite = (flags & RANGE_UB_INF) != 0;
	upper->inclusive = (flags & RANGE_UB_INC) != 0;
	upper->lower = false;
}

/*
 * 从多范围构造并集范围。
 */
RangeType *
multirange_get_union_range(TypeCacheEntry *rangetyp,
						   const MultirangeType *mr)
{
	RangeBound	lower,
				upper,
				tmp;

	if (MultirangeIsEmpty(mr))
		return make_empty_range(rangetyp);

	multirange_get_bounds(rangetyp, mr, 0, &lower, &tmp);
	multirange_get_bounds(rangetyp, mr, mr->rangeCount - 1, &tmp, &upper);

	return make_range(rangetyp, &lower, &upper, false, NULL);
}


/*
 * multirange_deserialize：解构多范围值
 *
 * 注意：给定的多范围对象必须完全解 TOAST，无法有
 * 短 varlena 头。
 */
void
multirange_deserialize(TypeCacheEntry *rangetyp,
					   const MultirangeType *multirange, int32 *range_count,
					   RangeType ***ranges)
{
	*range_count = multirange->rangeCount;

	/* 把每个 ShortRangeType 转换成 RangeType */
	if (*range_count > 0)
	{
		int			i;

		*ranges = palloc(*range_count * sizeof(RangeType *));
		for (i = 0; i < *range_count; i++)
			(*ranges)[i] = multirange_get_range(rangetyp, multirange, i);
	}
	else
	{
		*ranges = NULL;
	}
}

MultirangeType *
make_empty_multirange(Oid mltrngtypoid, TypeCacheEntry *rangetyp)
{
	return make_multirange(mltrngtypoid, rangetyp, 0, NULL);
}

/*
 * 与 range_overlaps_internal() 类似，但参数取范围
 * 边界而非范围.
 */
static bool
range_bounds_overlaps(TypeCacheEntry *typcache,
					  RangeBound *lower1, RangeBound *upper1,
					  RangeBound *lower2, RangeBound *upper2)
{
	if (range_cmp_bounds(typcache, lower1, lower2) >= 0 &&
		range_cmp_bounds(typcache, lower1, upper2) <= 0)
		return true;

	if (range_cmp_bounds(typcache, lower2, lower1) >= 0 &&
		range_cmp_bounds(typcache, lower2, upper1) <= 0)
		return true;

	return false;
}

/*
 * 与 range_contains_internal() 类似，但参数取范围
 * 边界而非范围.
 */
static bool
range_bounds_contains(TypeCacheEntry *typcache,
					  RangeBound *lower1, RangeBound *upper1,
					  RangeBound *lower2, RangeBound *upper2)
{
	if (range_cmp_bounds(typcache, lower1, lower2) <= 0 &&
		range_cmp_bounds(typcache, upper1, upper2) >= 0)
		return true;

	return false;
}

/*
 * 使用二分查找检查给定键是否匹配多范围中的
 * 某个范围。若未找到所需的范围，则视为不匹配。
 * 即使找到所需的范围，比较函数仍可能报告
 * 匹配或不匹配。例如，若我们搜索的是包含关系，
 * 可能找到一个与键范围重叠但不包含它的范围，
 * 这将视为不匹配。
 */
static bool
multirange_bsearch_match(TypeCacheEntry *typcache, const MultirangeType *mr,
						 void *key, multirange_bsearch_comparison cmp_func)
{
	uint32		l,
				u,
				idx;
	int			comparison;
	bool		match = false;

	l = 0;
	u = mr->rangeCount;
	while (l < u)
	{
		RangeBound	lower,
					upper;

		idx = (l + u) / 2;
		multirange_get_bounds(typcache, mr, idx, &lower, &upper);
		comparison = (*cmp_func) (typcache, &lower, &upper, key, &match);

		if (comparison < 0)
			u = idx;
		else if (comparison > 0)
			l = idx + 1;
		else
			return match;
	}

	return false;
}

/*
 *----------------------------------------------------------
 * 通用函数
 *----------------------------------------------------------
 */

/*
 * 从零个或多个范围构造多范围值。由于这是
 * 可变参数函数，我们收到的是一个数组。该数组
 * 须包含与返回值匹配的范围，且无 NULL。
 */
Datum
multirange_constructor2(PG_FUNCTION_ARGS)
{
	Oid			mltrngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	Oid			rngtypid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	ArrayType  *rangeArray;
	int			range_count;
	Datum	   *elements;
	bool	   *nulls;
	RangeType **ranges;
	int			dims;
	int			i;

	typcache = multirange_get_typcache(fcinfo, mltrngtypid);
	rangetyp = typcache->rngtype;

	/*
	 * 无参调用本应改调 multirange_constructor0，
	 * 但那样做返回的也是空范围。
	 */

	if (PG_NARGS() == 0)
		PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, 0, NULL));

	/*
	 * 这个检查本应由我们的签名保证，但仍执行以防万一
	 * 即可
	 */

	if (PG_ARGISNULL(0))
		elog(ERROR,
			 "multirange values cannot contain null members");

	rangeArray = PG_GETARG_ARRAYTYPE_P(0);

	dims = ARR_NDIM(rangeArray);
	if (dims > 1)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("multiranges cannot be constructed from multidimensional arrays")));

	rngtypid = ARR_ELEMTYPE(rangeArray);
	if (rngtypid != rangetyp->type_id)
		elog(ERROR, "type %u does not match constructor type", rngtypid);

	/*
	 * 注意：我们仍可能以零个范围被调用，像这样：
	 * `int4multirange(variadic '{}'::int4range[])
	 */
	if (dims == 0)
	{
		range_count = 0;
		ranges = NULL;
	}
	else
	{
		deconstruct_array(rangeArray, rngtypid, rangetyp->typlen, rangetyp->typbyval,
						  rangetyp->typalign, &elements, &nulls, &range_count);

		ranges = palloc0(range_count * sizeof(RangeType *));
		for (i = 0; i < range_count; i++)
		{
			if (nulls[i])
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("multirange values cannot contain null members")));

			/* make_multirange 会自行复制 */
			ranges[i] = DatumGetRangeTypeP(elements[i]);
		}
	}

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, range_count, ranges));
}

/*
 * 从单个范围构造多范围值。要是这种情况也能用
 * multirange_constructor2 就好了，但我们需要一个
 * 非可变参数的单参函数来定义从范围到其多范围的 CAST。
 */
Datum
multirange_constructor1(PG_FUNCTION_ARGS)
{
	Oid			mltrngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	Oid			rngtypid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	RangeType  *range;

	typcache = multirange_get_typcache(fcinfo, mltrngtypid);
	rangetyp = typcache->rngtype;

	/*
	 * 这个检查本应由我们的签名保证，但仍执行以防万一
	 * 即可
	 */

	if (PG_ARGISNULL(0))
		elog(ERROR,
			 "multirange values cannot contain null members");

	range = PG_GETARG_RANGE_P(0);

	/* 确保范围类型匹配。 */
	rngtypid = RangeTypeGetOid(range);
	if (rngtypid != rangetyp->type_id)
		elog(ERROR, "type %u does not match constructor type", rngtypid);

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, 1, &range));
}

/*
 * 与 multirange_constructor1 一样的构造函数，但 opr_sanity
 * 会生
 * 气。
 */
Datum
multirange_constructor0(PG_FUNCTION_ARGS)
{
	Oid			mltrngtypid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;

	/* 本函数应当总是以无参形式调用 */
	if (PG_NARGS() != 0)
		elog(ERROR,
			 "niladic multirange constructor must not receive arguments");

	mltrngtypid = get_fn_expr_rettype(fcinfo->flinfo);
	typcache = multirange_get_typcache(fcinfo, mltrngtypid);
	rangetyp = typcache->rngtype;

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypid, rangetyp, 0, NULL));
}


/* 多范围、多范围 -> 多范围类型函数 */

/* 多范围并集 */
Datum
multirange_union(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	int32		range_count1;
	int32		range_count2;
	int32		range_count3;
	RangeType **ranges1;
	RangeType **ranges2;
	RangeType **ranges3;

	if (MultirangeIsEmpty(mr1))
		PG_RETURN_MULTIRANGE_P(mr2);
	if (MultirangeIsEmpty(mr2))
		PG_RETURN_MULTIRANGE_P(mr1);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	multirange_deserialize(typcache->rngtype, mr1, &range_count1, &ranges1);
	multirange_deserialize(typcache->rngtype, mr2, &range_count2, &ranges2);

	range_count3 = range_count1 + range_count2;
	ranges3 = palloc0(range_count3 * sizeof(RangeType *));
	memcpy(ranges3, ranges1, range_count1 * sizeof(RangeType *));
	memcpy(ranges3 + range_count1, ranges2, range_count2 * sizeof(RangeType *));
	PG_RETURN_MULTIRANGE_P(make_multirange(typcache->type_id, typcache->rngtype,
										   range_count3, ranges3));
}

/* 多范围差集 */
Datum
multirange_minus(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	Oid			mltrngtypoid = MultirangeTypeGetOid(mr1);
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	int32		range_count1;
	int32		range_count2;
	RangeType **ranges1;
	RangeType **ranges2;

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);
	rangetyp = typcache->rngtype;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_MULTIRANGE_P(mr1);

	multirange_deserialize(typcache->rngtype, mr1, &range_count1, &ranges1);
	multirange_deserialize(typcache->rngtype, mr2, &range_count2, &ranges2);

	PG_RETURN_MULTIRANGE_P(multirange_minus_internal(mltrngtypoid,
													 rangetyp,
													 range_count1,
													 ranges1,
													 range_count2,
													 ranges2));
}

MultirangeType *
multirange_minus_internal(Oid mltrngtypoid, TypeCacheEntry *rangetyp,
						  int32 range_count1, RangeType **ranges1,
						  int32 range_count2, RangeType **ranges2)
{
	RangeType  *r1;
	RangeType  *r2;
	RangeType **ranges3;
	int32		range_count3;
	int32		i1;
	int32		i2;

	/*
	 * 最坏情形：ranges1 各范围对 ranges2 各做一次不同
	 * 切割。
	 */
	ranges3 = palloc0((range_count1 + range_count2) * sizeof(RangeType *));
	range_count3 = 0;

	/*
	 * 对 mr1 中的每个范围，不断做减法，直到它消失
	 * 或 mr2 中的范围已越过它。每次减法后我们把
	 * 剩余部分赋回 r1。mr1 与 mr2 平行推进的方式同
	 * multirange_overlaps_multirange_internal.
	 */
	r2 = ranges2[0];
	for (i1 = 0, i2 = 0; i1 < range_count1; i1++)
	{
		r1 = ranges1[i1];

		/* 在 r2 << r1 时丢弃 r2 */
		while (r2 != NULL && range_before_internal(rangetyp, r2, r1))
		{
			r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
		}

		while (r2 != NULL)
		{
			if (range_split_internal(rangetyp, r1, r2, &ranges3[range_count3], &r1))
			{
				/*
				 * 若 r2 从 r1 中间咬掉一口，就需要两个
				 * 输出
				 */
				range_count3++;
				r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
			}
			else if (range_overlaps_internal(rangetyp, r1, r2))
			{
				/*
				 * 若 r2 与 r1 重叠，用 r1-r2 替换。
				 */
				r1 = range_minus_internal(rangetyp, r1, r2);

				/*
				 * 若 r2 越过了 r1，那么我们要留在它上面，
				 * 以防它碰到后面的 r1。否则我们要保留 r1，
				 * 以防后面的 r2 碰到它。既然已经做过减法，
				 * 用 overright/overleft 调用便无意义。
				 */
				if (RangeIsEmpty(r1) || range_before_internal(rangetyp, r1, r2))
					break;
				else
					r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
			}
			else
			{
				/*
				 * 这个以及所有后续的 r2 都已越过 r1，
				 * 故保留之，r1 所余计入结果。
				 */
				break;
			}
		}

		/*
		 * 再无其他可从 r1 移除部分，故保留它。即使
		 * r1 为空，make_multirange 也会移除。
		 */
		ranges3[range_count3++] = r1;
	}

	return make_multirange(mltrngtypoid, rangetyp, range_count3, ranges3);
}

/* 多范围交集 */
Datum
multirange_intersect(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	Oid			mltrngtypoid = MultirangeTypeGetOid(mr1);
	TypeCacheEntry *typcache;
	TypeCacheEntry *rangetyp;
	int32		range_count1;
	int32		range_count2;
	RangeType **ranges1;
	RangeType **ranges2;

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);
	rangetyp = typcache->rngtype;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_MULTIRANGE_P(make_empty_multirange(mltrngtypoid, rangetyp));

	multirange_deserialize(rangetyp, mr1, &range_count1, &ranges1);
	multirange_deserialize(rangetyp, mr2, &range_count2, &ranges2);

	PG_RETURN_MULTIRANGE_P(multirange_intersect_internal(mltrngtypoid,
														 rangetyp,
														 range_count1,
														 ranges1,
														 range_count2,
														 ranges2));
}

MultirangeType *
multirange_intersect_internal(Oid mltrngtypoid, TypeCacheEntry *rangetyp,
							  int32 range_count1, RangeType **ranges1,
							  int32 range_count2, RangeType **ranges2)
{
	RangeType  *r1;
	RangeType  *r2;
	RangeType **ranges3;
	int32		range_count3;
	int32		i1;
	int32		i2;

	if (range_count1 == 0 || range_count2 == 0)
		return make_multirange(mltrngtypoid, rangetyp, 0, NULL);

	/*-----------------------------------------------
	 * 最坏情形是这样的拼接图案：
	 *
	 * mr1: --- --- --- ---
	 * mr2:   --- --- ---
	 * mr3:   - - - - - -
	 *
	 * 看起来是 range_count1 + range_count2 - 1，
	 * 但多留一个无妨。
	 *-----------------------------------------------
	 */
	ranges3 = palloc0((range_count1 + range_count2) * sizeof(RangeType *));
	range_count3 = 0;

	/*
	 * 对 mr1 中每个范围，不断求交，直到 mr2 中的范围
	 * 越过它。mr1 与 mr2 上的平行推进类似于
	 * multirange_minus_multirange_internal，但不必赋回
	 * r1。
	 */
	r2 = ranges2[0];
	for (i1 = 0, i2 = 0; i1 < range_count1; i1++)
	{
		r1 = ranges1[i1];

		/* 在 r2 << r1 时丢弃 r2 */
		while (r2 != NULL && range_before_internal(rangetyp, r2, r1))
		{
			r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];
		}

		while (r2 != NULL)
		{
			if (range_overlaps_internal(rangetyp, r1, r2))
			{
				/* 保留重叠部分 */
				ranges3[range_count3++] = range_intersect_internal(rangetyp, r1, r2);

				/* 若 r2 已被"用尽"，则转到下一个…… */
				if (range_overleft_internal(rangetyp, r2, r1))
					r2 = ++i2 >= range_count2 ? NULL : ranges2[i2];

				/* ……否则转到下一个 r1 */
				else
					break;
			}
			else
				/* 已越过 r1，故移到下一个 */
				break;
		}

		/* 若 r2 已用尽，则不可能再有交集 */
		if (r2 == NULL)
			break;
	}

	return make_multirange(mltrngtypoid, rangetyp, range_count3, ranges3);
}

/*
 * range_agg_transfn：合并相邻/重叠的范围。
 *
 * 此处只是把输入范围收进一个数组，
 * 以便 finalfn 能排序并合并它们。
 */
Datum
range_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			rngtypoid;
	ArrayBuildState *state;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "range_agg_transfn called in non-aggregate context");

	rngtypoid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (!type_is_range(rngtypoid))
		elog(ERROR, "range_agg must be called with a range");

	if (PG_ARGISNULL(0))
		state = initArrayResult(rngtypoid, aggContext, false);
	else
		state = (ArrayBuildState *) PG_GETARG_POINTER(0);

	/* 略 NULL */
	if (!PG_ARGISNULL(1))
		accumArrayResult(state, PG_GETARG_DATUM(1), false, rngtypoid, aggContext);

	PG_RETURN_POINTER(state);
}

/*
 * range_agg_finalfn：用内部数组合并相触的范围。
 *
 * 由 range_agg_finalfn(anyrange) 与
 * multirange_agg_finalfn(anymultirange).
 */
Datum
range_agg_finalfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			mltrngtypoid;
	TypeCacheEntry *typcache;
	ArrayBuildState *state;
	int32		range_count;
	RangeType **ranges;
	int			i;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "range_agg_finalfn called in non-aggregate context");

	state = PG_ARGISNULL(0) ? NULL : (ArrayBuildState *) PG_GETARG_POINTER(0);
	if (state == NULL)
		/* 这本不该发生，但以防万一…… */
		PG_RETURN_NULL();

	/* 若输入为零，也像其他聚集一样返回 NULL */
	range_count = state->nelems;
	if (range_count == 0)
		PG_RETURN_NULL();

	mltrngtypoid = get_fn_expr_rettype(fcinfo->flinfo);
	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);

	ranges = palloc0(range_count * sizeof(RangeType *));
	for (i = 0; i < range_count; i++)
		ranges[i] = DatumGetRangeTypeP(state->dvalues[i]);

	PG_RETURN_MULTIRANGE_P(make_multirange(mltrngtypoid, typcache->rngtype, range_count, ranges));
}

/*
 * multirange_agg_transfn：合并相邻/重叠的多范围。
 *
 * 此处所做只是把各输入多范围的范围收进一
 * 个数组，供 finalfn 排序合并。
 */
Datum
multirange_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			mltrngtypoid;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rngtypcache;
	ArrayBuildState *state;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "multirange_agg_transfn called in non-aggregate context");

	mltrngtypoid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (!type_is_multirange(mltrngtypoid))
		elog(ERROR, "range_agg must be called with a multirange");

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);
	rngtypcache = typcache->rngtype;

	if (PG_ARGISNULL(0))
		state = initArrayResult(rngtypcache->type_id, aggContext, false);
	else
		state = (ArrayBuildState *) PG_GETARG_POINTER(0);

	/* 略 NULL */
	if (!PG_ARGISNULL(1))
	{
		MultirangeType *current;
		int32		range_count;
		RangeType **ranges;

		current = PG_GETARG_MULTIRANGE_P(1);
		multirange_deserialize(rngtypcache, current, &range_count, &ranges);
		if (range_count == 0)
		{
			/*
			 * 加入一个空范围，使结果为空而非
			 * NULL。
			 */
			accumArrayResult(state,
							 RangeTypePGetDatum(make_empty_range(rngtypcache)),
							 false, rngtypcache->type_id, aggContext);
		}
		else
		{
			for (int32 i = 0; i < range_count; i++)
				accumArrayResult(state, RangeTypePGetDatum(ranges[i]), false, rngtypcache->type_id, aggContext);
		}
	}

	PG_RETURN_POINTER(state);
}

Datum
multirange_intersect_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext aggContext;
	Oid			mltrngtypoid;
	TypeCacheEntry *typcache;
	MultirangeType *result;
	MultirangeType *current;
	int32		range_count1;
	int32		range_count2;
	RangeType **ranges1;
	RangeType **ranges2;

	if (!AggCheckCallContext(fcinfo, &aggContext))
		elog(ERROR, "multirange_intersect_agg_transfn called in non-aggregate context");

	mltrngtypoid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (!type_is_multirange(mltrngtypoid))
		elog(ERROR, "range_intersect_agg must be called with a multirange");

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);

	/* 严格性保证这些非 NULL */
	result = PG_GETARG_MULTIRANGE_P(0);
	current = PG_GETARG_MULTIRANGE_P(1);

	multirange_deserialize(typcache->rngtype, result, &range_count1, &ranges1);
	multirange_deserialize(typcache->rngtype, current, &range_count2, &ranges2);

	result = multirange_intersect_internal(mltrngtypoid,
										   typcache->rngtype,
										   range_count1,
										   ranges1,
										   range_count2,
										   ranges2);
	PG_RETURN_MULTIRANGE_P(result);
}


/* 多范围 -> 元素类型函数 */

/* 提取下边界值 */
Datum
multirange_lower(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_NULL();

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, 0,
						  &lower, &upper);

	if (!lower.infinite)
		PG_RETURN_DATUM(lower.val);
	else
		PG_RETURN_NULL();
}

/* 提取上边界值 */
Datum
multirange_upper(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_NULL();

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower, &upper);

	if (!upper.infinite)
		PG_RETURN_DATUM(upper.val);
	else
		PG_RETURN_NULL();
}


/* 多范围 -> bool 函数 */

/* 多范围为空？ */
Datum
multirange_empty(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);

	PG_RETURN_BOOL(MultirangeIsEmpty(mr));
}

/* 下边界是否包含 */
Datum
multirange_lower_inc(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, 0,
						  &lower, &upper);

	PG_RETURN_BOOL(lower.inclusive);
}

/* 上边界是否包含 */
Datum
multirange_upper_inc(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower, &upper);

	PG_RETURN_BOOL(upper.inclusive);
}

/* 下边界是否为无穷 */
Datum
multirange_lower_inf(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, 0,
						  &lower, &upper);

	PG_RETURN_BOOL(lower.infinite);
}

/* 上边界是否为无穷 */
Datum
multirange_upper_inf(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;

	if (MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower, &upper);

	PG_RETURN_BOOL(upper.infinite);
}



/* 多范围、元素 -> bool 函数 */

/* 包含？ */
Datum
multirange_contains_elem(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	Datum		val = PG_GETARG_DATUM(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_elem_internal(typcache->rngtype, mr, val));
}

/* 被包含？ */
Datum
elem_contained_by_multirange(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_elem_internal(typcache->rngtype, mr, val));
}

/*
 * 用于以二分查找检查多范围的任一范围是否包含给
 * 定键元素的比较函数。
 */
static int
multirange_elem_bsearch_comparison(TypeCacheEntry *typcache,
								   RangeBound *lower, RangeBound *upper,
								   void *key, bool *match)
{
	Datum		val = *((Datum *) key);
	int			cmp;

	if (!lower->infinite)
	{
		cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
											  typcache->rng_collation,
											  lower->val, val));
		if (cmp > 0 || (cmp == 0 && !lower->inclusive))
			return -1;
	}

	if (!upper->infinite)
	{
		cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo,
											  typcache->rng_collation,
											  upper->val, val));
		if (cmp < 0 || (cmp == 0 && !upper->inclusive))
			return 1;
	}

	*match = true;
	return 0;
}

/*
 * 测试多范围 mr 是否包含特定的元素值。
 */
bool
multirange_contains_elem_internal(TypeCacheEntry *rangetyp,
								  const MultirangeType *mr, Datum val)
{
	if (MultirangeIsEmpty(mr))
		return false;

	return multirange_bsearch_match(rangetyp, mr, &val,
									multirange_elem_bsearch_comparison);
}

/* 多范围、范围 -> bool 函数 */

/* 包含？ */
Datum
multirange_contains_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_range_internal(typcache->rngtype, mr, r));
}

Datum
range_contains_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_contains_multirange_internal(typcache->rngtype, r, mr));
}

/* 被包含？ */
Datum
range_contained_by_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(multirange_contains_range_internal(typcache->rngtype, mr, r));
}

Datum
multirange_contained_by_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_contains_multirange_internal(typcache->rngtype, r, mr));
}

/*
 * 用于以二分查找检查多范围的任一范围是否包含给
 * 定键范围的比较函数。
 */
static int
multirange_range_contains_bsearch_comparison(TypeCacheEntry *typcache,
											 RangeBound *lower, RangeBound *upper,
											 void *key, bool *match)
{
	RangeBound *keyLower = (RangeBound *) key;
	RangeBound *keyUpper = (RangeBound *) key + 1;

	/* 检查键范围是否严格位于左侧或右侧 */
	if (range_cmp_bounds(typcache, keyUpper, lower) < 0)
		return -1;
	if (range_cmp_bounds(typcache, keyLower, upper) > 0)
		return 1;

	/*
	 * 此时我们找到了重叠的范围。但还必须检查它
	 * 是否真的包含键范围。无论如何，搜索必须在此
	 * 停止，因为多范围只包含互不重叠的范围。
	 */
	*match = range_bounds_contains(typcache, lower, upper, keyLower, keyUpper);

	return 0;
}

/*
 * 测试多范围 mr 是否包含特定的范围 r。
 */
bool
multirange_contains_range_internal(TypeCacheEntry *rangetyp,
								   const MultirangeType *mr,
								   const RangeType *r)
{
	RangeBound	bounds[2];
	bool		empty;

	/*
	 * 每个多范围都含无穷多个空范围，即使它本身为空亦
	 * 然。
	 */
	if (RangeIsEmpty(r))
		return true;

	if (MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &bounds[0], &bounds[1], &empty);
	Assert(!empty);

	return multirange_bsearch_match(rangetyp, mr, bounds,
									multirange_range_contains_bsearch_comparison);
}

/*
 * 测试范围 r 是否包含多范围 mr。
 */
bool
range_contains_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2,
				tmp;
	bool		empty;

	/*
	 * 每个范围都含无穷多个空多范围，即使它本身为空亦
	 * 然。
	 */
	if (MultirangeIsEmpty(mr))
		return true;

	if (RangeIsEmpty(r))
		return false;

	/* 范围包含多范围当且仅当其包含并集范围。 */
	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);
	multirange_get_bounds(rangetyp, mr, 0, &lower2, &tmp);
	multirange_get_bounds(rangetyp, mr, mr->rangeCount - 1, &tmp, &upper2);

	return range_bounds_contains(rangetyp, &lower1, &upper1, &lower2, &upper2);
}


/* 多范围、多范围 -> bool 函数 */

/* 相等（内部版本） */
bool
multirange_eq_internal(TypeCacheEntry *rangetyp,
					   const MultirangeType *mr1,
					   const MultirangeType *mr2)
{
	int32		range_count_1;
	int32		range_count_2;
	int32		i;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	/* 不同类型本应由 ANYMULTIRANGE 匹配规则来防止 */
	if (MultirangeTypeGetOid(mr1) != MultirangeTypeGetOid(mr2))
		elog(ERROR, "multirange types do not match");

	range_count_1 = mr1->rangeCount;
	range_count_2 = mr2->rangeCount;

	if (range_count_1 != range_count_2)
		return false;

	for (i = 0; i < range_count_1; i++)
	{
		multirange_get_bounds(rangetyp, mr1, i, &lower1, &upper1);
		multirange_get_bounds(rangetyp, mr2, i, &lower2, &upper2);

		if (range_cmp_bounds(rangetyp, &lower1, &lower2) != 0 ||
			range_cmp_bounds(rangetyp, &upper1, &upper2) != 0)
			return false;
	}

	return true;
}

/* 相等 */
Datum
multirange_eq(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_eq_internal(typcache->rngtype, mr1, mr2));
}

/* 不等（内部版本） */
bool
multirange_ne_internal(TypeCacheEntry *rangetyp,
					   const MultirangeType *mr1,
					   const MultirangeType *mr2)
{
	return (!multirange_eq_internal(rangetyp, mr1, mr2));
}

/* 不等 */
Datum
multirange_ne(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_ne_internal(typcache->rngtype, mr1, mr2));
}

/* 重叠？ */
Datum
range_overlaps_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overlaps_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overlaps_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overlaps_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overlaps_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_overlaps_multirange_internal(typcache->rngtype, mr1, mr2));
}

/*
 * 用于以二分查找检查多范围任一范围与给定键
 * 范围重叠的比较函数。
 */
static int
multirange_range_overlaps_bsearch_comparison(TypeCacheEntry *typcache,
											 RangeBound *lower, RangeBound *upper,
											 void *key, bool *match)
{
	RangeBound *keyLower = (RangeBound *) key;
	RangeBound *keyUpper = (RangeBound *) key + 1;

	if (range_cmp_bounds(typcache, keyUpper, lower) < 0)
		return -1;
	if (range_cmp_bounds(typcache, keyLower, upper) > 0)
		return 1;

	*match = true;
	return 0;
}

bool
range_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	bounds[2];
	bool		empty;

	/*
	 * 空范围从不重叠，即使对空范围。（看似奇怪，
	 * 它们*确实*互相包含，但我们遵循范围的做法。）
	 */
	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &bounds[0], &bounds[1], &empty);
	Assert(!empty);

	return multirange_bsearch_match(rangetyp, mr, bounds,
									multirange_range_overlaps_bsearch_comparison);
}

bool
multirange_overlaps_multirange_internal(TypeCacheEntry *rangetyp,
										const MultirangeType *mr1,
										const MultirangeType *mr2)
{
	int32		range_count1;
	int32		range_count2;
	int32		i1;
	int32		i2;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	/*
	 * 空范围从不重叠，即使对空范围。（看似奇怪，
	 * 它们*确实*互相包含，但我们遵循范围的做法。）
	 */
	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		return false;

	range_count1 = mr1->rangeCount;
	range_count2 = mr2->rangeCount;

	/*
	 * mr1 中的每个范围都有机会与 mr2 中的范围重叠，
	 * 但可用其排序避免 O(n^2)。这类似于
	 * range_overlaps_multirange（r1 : r2 :: mrr : r），但那里
	 * 找不到与 r 的重叠就结束，而这里找不到与 r2 的
	 * 重叠则尝试下一个 r2。
	 */
	i1 = 0;
	multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
	for (i1 = 0, i2 = 0; i2 < range_count2; i2++)
	{
		multirange_get_bounds(rangetyp, mr2, i2, &lower2, &upper2);

		/* 在 r1 << r2 时丢弃 r1 */
		while (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0)
		{
			if (++i1 >= range_count1)
				return false;
			multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
		}

		/*
		 * 若 r1 && r2 则完成；否则未为 r2 找到
		 * 重叠，转下一个。
		 */
		if (range_bounds_overlaps(rangetyp, &lower1, &upper1, &lower2, &upper2))
			return true;
	}

	/* 查遍 mr2 也未找到重叠 */
	return false;
}

/* 是否不延伸到右侧？ */
bool
range_overleft_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);


	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);
	multirange_get_bounds(rangetyp, mr, mr->rangeCount - 1,
						  &lower2, &upper2);

	PG_RETURN_BOOL(range_cmp_bounds(rangetyp, &upper1, &upper2) <= 0);
}

Datum
range_overleft_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overleft_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overleft_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (MultirangeIsEmpty(mr) || RangeIsEmpty(r))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
						  &lower1, &upper1);
	range_deserialize(typcache->rngtype, r, &lower2, &upper2, &empty);
	Assert(!empty);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &upper1, &upper2) <= 0);
}

Datum
multirange_overleft_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	multirange_get_bounds(typcache->rngtype, mr1, mr1->rangeCount - 1,
						  &lower1, &upper1);
	multirange_get_bounds(typcache->rngtype, mr2, mr2->rangeCount - 1,
						  &lower2, &upper2);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &upper1, &upper2) <= 0);
}

/* 是否不延伸到左侧？ */
bool
range_overright_multirange_internal(TypeCacheEntry *rangetyp,
									const RangeType *r,
									const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		PG_RETURN_BOOL(false);

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);
	multirange_get_bounds(rangetyp, mr, 0, &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &lower1, &lower2) >= 0);
}

Datum
range_overright_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_overright_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_overright_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (MultirangeIsEmpty(mr) || RangeIsEmpty(r))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	multirange_get_bounds(typcache->rngtype, mr, 0, &lower1, &upper1);
	range_deserialize(typcache->rngtype, r, &lower2, &upper2, &empty);
	Assert(!empty);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &lower1, &lower2) >= 0);
}

Datum
multirange_overright_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		PG_RETURN_BOOL(false);

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	multirange_get_bounds(typcache->rngtype, mr1, 0, &lower1, &upper1);
	multirange_get_bounds(typcache->rngtype, mr2, 0, &lower2, &upper2);

	PG_RETURN_BOOL(range_cmp_bounds(typcache->rngtype, &lower1, &lower2) >= 0);
}

/* 包含？ */
Datum
multirange_contains_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_contains_multirange_internal(typcache->rngtype, mr1, mr2));
}

/* 被包含？ */
Datum
multirange_contained_by_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_contains_multirange_internal(typcache->rngtype, mr2, mr1));
}

/*
 * 测试多范围 mr1 是否包含另一个多范围 mr2 中的每个范围。
 */
bool
multirange_contains_multirange_internal(TypeCacheEntry *rangetyp,
										const MultirangeType *mr1,
										const MultirangeType *mr2)
{
	int32		range_count1 = mr1->rangeCount;
	int32		range_count2 = mr2->rangeCount;
	int			i1,
				i2;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	/*
	 * 对空多范围，我们沿用与范围相同的逻辑：
	 * 空多范围包含空范围/空多范围；空多范围
	 * 不能包含任何其他范围/多范围；空多范围会被任何
	 * 范围所包含。
	 */

	if (range_count2 == 0)
		return true;
	if (range_count1 == 0)
		return false;

	/*
	 * mr2 中的每个范围都必须被 mr1 中的某个范围包含。
	 * 为避免 O(n^2)，同步遍历两者。
	 */
	i1 = 0;
	multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
	for (i2 = 0; i2 < range_count2; i2++)
	{
		multirange_get_bounds(rangetyp, mr2, i2, &lower2, &upper2);

		/* 在 r1 << r2 时丢弃 r1 */
		while (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0)
		{
			if (++i1 >= range_count1)
				return false;
			multirange_get_bounds(rangetyp, mr1, i1, &lower1, &upper1);
		}

		/*
		 * 若 r1 @> r2 则转到下一个 r2，否则返回 false
		 * （各 r1[n] 与 r1[n+1] 之间必有间隙）。注意：
		 * 若不规范化则结果怪异，例如自定义的
		 * int2multirange {[1,1], [2,2]} 就存在 "间隙"。但这
		 * 与其他范围算子一致，例如 '[1,1]'::int2range -|-
		 * '[2,2]'::int2range为false.
		 */
		if (!range_bounds_contains(rangetyp, &lower1, &upper1,
								   &lower2, &upper2))
			return false;
	}

	/* mr2 所有范围均已满足 */
	return true;
}

/* 严格在左？ */
Datum
range_before_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_before_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_before_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_after_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_before_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_before_multirange_internal(typcache->rngtype, mr1, mr2));
}

/* 严格在右？ */
Datum
range_after_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_after_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_after_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_before_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_after_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	PG_RETURN_BOOL(multirange_before_multirange_internal(typcache->rngtype, mr2, mr1));
}

/* 严格在左？（内部版） */
bool
range_before_multirange_internal(TypeCacheEntry *rangetyp,
								 const RangeType *r,
								 const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);

	multirange_get_bounds(rangetyp, mr, 0, &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0);
}

bool
multirange_before_multirange_internal(TypeCacheEntry *rangetyp,
									  const MultirangeType *mr1,
									  const MultirangeType *mr2)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		return false;

	multirange_get_bounds(rangetyp, mr1, mr1->rangeCount - 1,
						  &lower1, &upper1);
	multirange_get_bounds(rangetyp, mr2, 0,
						  &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &upper1, &lower2) < 0);
}

/* 严格在右？（内部版） */
bool
range_after_multirange_internal(TypeCacheEntry *rangetyp,
								const RangeType *r,
								const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;
	int32		range_count;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);

	range_count = mr->rangeCount;
	multirange_get_bounds(rangetyp, mr, range_count - 1,
						  &lower2, &upper2);

	return (range_cmp_bounds(rangetyp, &lower1, &upper2) > 0);
}

bool
range_adjacent_multirange_internal(TypeCacheEntry *rangetyp,
								   const RangeType *r,
								   const MultirangeType *mr)
{
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;
	bool		empty;
	int32		range_count;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	range_deserialize(rangetyp, r, &lower1, &upper1, &empty);
	Assert(!empty);

	range_count = mr->rangeCount;
	multirange_get_bounds(rangetyp, mr, 0,
						  &lower2, &upper2);

	if (bounds_adjacent(rangetyp, upper1, lower2))
		return true;

	if (range_count > 1)
		multirange_get_bounds(rangetyp, mr, range_count - 1,
							  &lower2, &upper2);

	if (bounds_adjacent(rangetyp, upper2, lower1))
		return true;

	return false;
}

/* 相邻？ */
Datum
range_adjacent_multirange(PG_FUNCTION_ARGS)
{
	RangeType  *r = PG_GETARG_RANGE_P(0);
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_adjacent_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_adjacent_range(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	RangeType  *r = PG_GETARG_RANGE_P(1);
	TypeCacheEntry *typcache;

	if (RangeIsEmpty(r) || MultirangeIsEmpty(mr))
		return false;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));

	PG_RETURN_BOOL(range_adjacent_multirange_internal(typcache->rngtype, r, mr));
}

Datum
multirange_adjacent_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	TypeCacheEntry *typcache;
	int32		range_count1;
	int32		range_count2;
	RangeBound	lower1,
				upper1,
				lower2,
				upper2;

	if (MultirangeIsEmpty(mr1) || MultirangeIsEmpty(mr2))
		return false;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	range_count1 = mr1->rangeCount;
	range_count2 = mr2->rangeCount;
	multirange_get_bounds(typcache->rngtype, mr1, range_count1 - 1,
						  &lower1, &upper1);
	multirange_get_bounds(typcache->rngtype, mr2, 0,
						  &lower2, &upper2);
	if (bounds_adjacent(typcache->rngtype, upper1, lower2))
		PG_RETURN_BOOL(true);

	if (range_count1 > 1)
		multirange_get_bounds(typcache->rngtype, mr1, 0,
							  &lower1, &upper1);
	if (range_count2 > 1)
		multirange_get_bounds(typcache->rngtype, mr2, range_count2 - 1,
							  &lower2, &upper2);
	if (bounds_adjacent(typcache->rngtype, upper2, lower1))
		PG_RETURN_BOOL(true);
	PG_RETURN_BOOL(false);
}

/* Btree 支持 */

/* btree 比较器 */
Datum
multirange_cmp(PG_FUNCTION_ARGS)
{
	MultirangeType *mr1 = PG_GETARG_MULTIRANGE_P(0);
	MultirangeType *mr2 = PG_GETARG_MULTIRANGE_P(1);
	int32		range_count_1;
	int32		range_count_2;
	int32		range_count_max;
	int32		i;
	TypeCacheEntry *typcache;
	int			cmp = 0;		/* 两者皆空时用此值。 */

	/* 不同类型本应由 ANYMULTIRANGE 匹配规则来防止 */
	if (MultirangeTypeGetOid(mr1) != MultirangeTypeGetOid(mr2))
		elog(ERROR, "multirange types do not match");

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr1));

	range_count_1 = mr1->rangeCount;
	range_count_2 = mr2->rangeCount;

	/* 遍历源数据 */
	range_count_max = Max(range_count_1, range_count_2);
	for (i = 0; i < range_count_max; i++)
	{
		RangeBound	lower1,
					upper1,
					lower2,
					upper2;

		/*
		 * 若一个多范围更短，就好比它在末尾有空范围来
		 * 延长其长度。空范围比任何其他范围都小，
		 * 因此较短的多范围排在较长者之前。
		 * 这与其他类型的行为一致，例如字符串 'aaa' <
		 * 'aaaaaa'。
		 */
		if (i >= range_count_1)
		{
			cmp = -1;
			break;
		}
		if (i >= range_count_2)
		{
			cmp = 1;
			break;
		}

		multirange_get_bounds(typcache->rngtype, mr1, i, &lower1, &upper1);
		multirange_get_bounds(typcache->rngtype, mr2, i, &lower2, &upper2);

		cmp = range_cmp_bounds(typcache->rngtype, &lower1, &lower2);
		if (cmp == 0)
			cmp = range_cmp_bounds(typcache->rngtype, &upper1, &upper2);
		if (cmp != 0)
			break;
	}

	PG_FREE_IF_COPY(mr1, 0);
	PG_FREE_IF_COPY(mr2, 1);

	PG_RETURN_INT32(cmp);
}

/* 使用 multirange_cmp 函数的不等算子 */
Datum
multirange_lt(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
multirange_le(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
multirange_ge(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
multirange_gt(PG_FUNCTION_ARGS)
{
	int			cmp = multirange_cmp(fcinfo);

	PG_RETURN_BOOL(cmp > 0);
}

/* 多范围 -> 范围函数 */

/* 找出包含多范围中一切的最小范围 */
Datum
range_merge_from_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	Oid			mltrngtypoid = MultirangeTypeGetOid(mr);
	TypeCacheEntry *typcache;
	RangeType  *result;

	typcache = multirange_get_typcache(fcinfo, mltrngtypoid);

	if (MultirangeIsEmpty(mr))
	{
		result = make_empty_range(typcache->rngtype);
	}
	else if (mr->rangeCount == 1)
	{
		result = multirange_get_range(typcache->rngtype, mr, 0);
	}
	else
	{
		RangeBound	firstLower,
					firstUpper,
					lastLower,
					lastUpper;

		multirange_get_bounds(typcache->rngtype, mr, 0,
							  &firstLower, &firstUpper);
		multirange_get_bounds(typcache->rngtype, mr, mr->rangeCount - 1,
							  &lastLower, &lastUpper);

		result = make_range(typcache->rngtype, &firstLower, &lastUpper,
							false, NULL);
	}

	PG_RETURN_RANGE_P(result);
}

/* 把多范围变成一组范围 */
Datum
multirange_unnest(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		MultirangeType *mr;
		TypeCacheEntry *typcache;
		int			index;
	} multirange_unnest_fctx;

	FuncCallContext *funcctx;
	multirange_unnest_fctx *fctx;
	MemoryContext oldcontext;

	/* 仅在函数首次调用时做的事 */
	if (SRF_IS_FIRSTCALL())
	{
		MultirangeType *mr;

		/* 创建用于跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * 取得多范围值，并按需解 TOAST。我们不能更早
		 * 做这件事，因为若必须解 TOAST，我们会希望解
		 * TOAST 后的副本位于 multi_call_memory_ctx 中，
		 * 它要待我们完成时才会消失（若未发生解
		 * TOAST，则原多范围会留到那时。）
		 */
		mr = PG_GETARG_MULTIRANGE_P(0);

		/* 为用户上下文分配内存 */
		fctx = (multirange_unnest_fctx *) palloc(sizeof(multirange_unnest_fctx));

		/* 初始化状态 */
		fctx->mr = mr;
		fctx->index = 0;
		fctx->typcache = lookup_type_cache(MultirangeTypeGetOid(mr),
										   TYPECACHE_MULTIRANGE_INFO);

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 每次函数调用都要做的事 */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	if (fctx->index < fctx->mr->rangeCount)
	{
		RangeType  *range;

		range = multirange_get_range(fctx->typcache->rngtype,
									 fctx->mr,
									 fctx->index);
		fctx->index++;

		SRF_RETURN_NEXT(funcctx, RangeTypePGetDatum(range));
	}
	else
	{
		/* 没有剩余内容时执行 */
		SRF_RETURN_DONE(funcctx);
	}
}

/* 哈希支持 */

/* 对多范围值哈希 */
Datum
hash_multirange(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	uint32		result = 1;
	TypeCacheEntry *typcache,
			   *scache;
	int32		range_count,
				i;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	scache = typcache->rngtype->rngelemtype;
	if (!OidIsValid(scache->hash_proc_finfo.fn_oid))
	{
		scache = lookup_type_cache(scache->type_id,
								   TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(scache->hash_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(scache->type_id))));
	}

	range_count = mr->rangeCount;
	for (i = 0; i < range_count; i++)
	{
		RangeBound	lower,
					upper;
		uint8		flags = MultirangeGetFlagsPtr(mr)[i];
		uint32		lower_hash;
		uint32		upper_hash;
		uint32		range_hash;

		multirange_get_bounds(typcache->rngtype, mr, i, &lower, &upper);

		if (RANGE_HAS_LBOUND(flags))
			lower_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo,
														  typcache->rngtype->rng_collation,
														  lower.val));
		else
			lower_hash = 0;

		if (RANGE_HAS_UBOUND(flags))
			upper_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo,
														  typcache->rngtype->rng_collation,
														  upper.val));
		else
			upper_hash = 0;

		/* 合并标志与边界的哈希 */
		range_hash = hash_uint32((uint32) flags);
		range_hash ^= lower_hash;
		range_hash = pg_rotate_left32(range_hash, 1);
		range_hash ^= upper_hash;

		/*
		 * 采用与 hash_array 相同的方法合并各个
		 * 元素的哈希值：
		 */
		result = (result << 5) - result + range_hash;
	}

	PG_FREE_IF_COPY(mr, 0);

	PG_RETURN_UINT32(result);
}

/*
 * 将值哈希为 64 位值（带种子）并返回之。
 * 其余同 hash_multirange。
 */
Datum
hash_multirange_extended(PG_FUNCTION_ARGS)
{
	MultirangeType *mr = PG_GETARG_MULTIRANGE_P(0);
	Datum		seed = PG_GETARG_DATUM(1);
	uint64		result = 1;
	TypeCacheEntry *typcache,
			   *scache;
	int32		range_count,
				i;

	typcache = multirange_get_typcache(fcinfo, MultirangeTypeGetOid(mr));
	scache = typcache->rngtype->rngelemtype;
	if (!OidIsValid(scache->hash_extended_proc_finfo.fn_oid))
	{
		scache = lookup_type_cache(scache->type_id,
								   TYPECACHE_HASH_EXTENDED_PROC_FINFO);
		if (!OidIsValid(scache->hash_extended_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(scache->type_id))));
	}

	range_count = mr->rangeCount;
	for (i = 0; i < range_count; i++)
	{
		RangeBound	lower,
					upper;
		uint8		flags = MultirangeGetFlagsPtr(mr)[i];
		uint64		lower_hash;
		uint64		upper_hash;
		uint64		range_hash;

		multirange_get_bounds(typcache->rngtype, mr, i, &lower, &upper);

		if (RANGE_HAS_LBOUND(flags))
			lower_hash = DatumGetUInt64(FunctionCall2Coll(&scache->hash_extended_proc_finfo,
														  typcache->rngtype->rng_collation,
														  lower.val,
														  seed));
		else
			lower_hash = 0;

		if (RANGE_HAS_UBOUND(flags))
			upper_hash = DatumGetUInt64(FunctionCall2Coll(&scache->hash_extended_proc_finfo,
														  typcache->rngtype->rng_collation,
														  upper.val,
														  seed));
		else
			upper_hash = 0;

		/* 合并标志与边界的哈希 */
		range_hash = DatumGetUInt64(hash_uint32_extended((uint32) flags,
														 DatumGetInt64(seed)));
		range_hash ^= lower_hash;
		range_hash = ROTATE_HIGH_AND_LOW_32BITS(range_hash);
		range_hash ^= upper_hash;

		/*
		 * 采用与 hash_array 相同的方法合并各个
		 * 元素的哈希值：
		 */
		result = (result << 5) - result + range_hash;
	}

	PG_FREE_IF_COPY(mr, 0);

	PG_RETURN_UINT64(result);
}
