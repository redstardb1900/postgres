/*-------------------------------------------------------------------------
 *
 * uuid.c
 *	  内置类型 "uuid" 的函数。
 *
 * Copyright (c) 2007-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/uuid.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/sortsupport.h"
#include "utils/uuid.h"

/* uuid 的 sortsupport */
typedef struct
{
	int64		input_count;	/* 已见到的非空值个数 */
	bool		estimating;		/* 正在估算基数时为 true */

	hyperLogLogState abbr_card; /* 基数估算器 */
} uuid_sortsupport_state;

static void string_to_uuid(const char *source, pg_uuid_t *uuid, Node *escontext);
static int	uuid_internal_cmp(const pg_uuid_t *arg1, const pg_uuid_t *arg2);
static int	uuid_fast_cmp(Datum x, Datum y, SortSupport ssup);
static bool uuid_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum uuid_abbrev_convert(Datum original, SortSupport ssup);

Datum
uuid_in(PG_FUNCTION_ARGS)
{
	char	   *uuid_str = PG_GETARG_CSTRING(0);
	pg_uuid_t  *uuid;

	uuid = (pg_uuid_t *) palloc(sizeof(*uuid));
	string_to_uuid(uuid_str, uuid, fcinfo->context);
	PG_RETURN_UUID_P(uuid);
}

Datum
uuid_out(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	static const char hex_chars[] = "0123456789abcdef";
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	for (i = 0; i < UUID_LEN; i++)
	{
		int			hi;
		int			lo;

		/*
		 * 我们把 uuid 值打印为 8、4、4、4、12
		 * 个十六进制字符，各组之间以连字符分隔。
		 * 因此我们要在此处的适当位置加上连字符。
		 */
		if (i == 4 || i == 6 || i == 8 || i == 10)
			appendStringInfoChar(&buf, '-');

		hi = uuid->data[i] >> 4;
		lo = uuid->data[i] & 0x0F;

		appendStringInfoChar(&buf, hex_chars[hi]);
		appendStringInfoChar(&buf, hex_chars[lo]);
	}

	PG_RETURN_CSTRING(buf.data);
}

/*
 * 我们允许 UUID 是 32 个十六进制数字的序列，每 4 个
 * 十六进制数字之后可选跟一个连字符，且可选用 {} 包
 * 围。（规范格式 8x-4x-4x-4x-12x，其中 "nx" 表示 n
 * 个十六进制数字仅作输出。）
 */
static void
string_to_uuid(const char *source, pg_uuid_t *uuid, Node *escontext)
{
	const char *src = source;
	bool		braces = false;
	int			i;

	if (src[0] == '{')
	{
		src++;
		braces = true;
	}

	for (i = 0; i < UUID_LEN; i++)
	{
		char		str_buf[3];

		if (src[0] == '\0' || src[1] == '\0')
			goto syntax_error;
		memcpy(str_buf, src, 2);
		if (!isxdigit((unsigned char) str_buf[0]) ||
			!isxdigit((unsigned char) str_buf[1]))
			goto syntax_error;

		str_buf[2] = '\0';
		uuid->data[i] = (unsigned char) strtoul(str_buf, NULL, 16);
		src += 2;
		if (src[0] == '-' && (i % 2) == 1 && i < UUID_LEN - 1)
			src++;
	}

	if (braces)
	{
		if (*src != '}')
			goto syntax_error;
		src++;
	}

	if (*src != '\0')
		goto syntax_error;

	return;

syntax_error:
	ereturn(escontext,,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"uuid", source)));
}

Datum
uuid_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buffer = (StringInfo) PG_GETARG_POINTER(0);
	pg_uuid_t  *uuid;

	uuid = (pg_uuid_t *) palloc(UUID_LEN);
	memcpy(uuid->data, pq_getmsgbytes(buffer, UUID_LEN), UUID_LEN);
	PG_RETURN_POINTER(uuid);
}

Datum
uuid_send(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	StringInfoData buffer;

	pq_begintypsend(&buffer);
	pq_sendbytes(&buffer, uuid->data, UUID_LEN);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

/* 内部 uuid 比较函数 */
static int
uuid_internal_cmp(const pg_uuid_t *arg1, const pg_uuid_t *arg2)
{
	return memcmp(arg1->data, arg2->data, UUID_LEN);
}

Datum
uuid_lt(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) < 0);
}

Datum
uuid_le(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) <= 0);
}

Datum
uuid_eq(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) == 0);
}

Datum
uuid_ge(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) >= 0);
}

Datum
uuid_gt(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) > 0);
}

Datum
uuid_ne(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) != 0);
}

/* btree 索引算子的处理器 */
Datum
uuid_cmp(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_INT32(uuid_internal_cmp(arg1, arg2));
}

/*
 * 排序支持策略例程
 */
Datum
uuid_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = uuid_fast_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		uuid_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(uuid_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = uuid_abbrev_convert;
		ssup->abbrev_abort = uuid_abbrev_abort;
		ssup->abbrev_full_comparator = uuid_fast_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport 比较函数
 */
static int
uuid_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	pg_uuid_t  *arg1 = DatumGetUUIDP(x);
	pg_uuid_t  *arg2 = DatumGetUUIDP(y);

	return uuid_internal_cmp(arg1, arg2);
}

/*
 * 用于估算缩写键优化效果的回调。
 *
 * 我们完全不关心非缩写数据的基数，因为权威的
 * uuid 比较器内没有相等快速路径。
 */
static bool
uuid_abbrev_abort(int memtupcount, SortSupport ssup)
{
	uuid_sortsupport_state *uss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

	/*
	 * 若已有 >100k 个不同值，那么即使要排序数十亿
	 * 行，我们大概率仍不吃亏，而撤销这么多行的缩
	 * 写操作的代价很可能不值得。到那时甚至可
	 * 停止计数。
	 */
	if (abbr_card > 100000.0)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "uuid_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count, memtupcount);
#endif
		uss->estimating = false;
		return false;
	}

	/*
	 * 目标最小基数为每 ~2k 个非空输入 1 个。0.5 行
	 * 的容差因子使我们在真正病态的数据上更早放
	 * 弃——即在前 2k 个（非空）行中恰好只有一个
	 * 缩写值。
	 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "uuid_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				 memtupcount);
#endif
		return true;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "uuid_abbrev: cardinality %f after " INT64_FORMAT
			 " values (%d rows)", abbr_card, uss->input_count, memtupcount);
#endif

	return false;
}

/*
 * sortsupport 的转换例程。把原始 uuid 表示转换为
 * 缩写键表示。我们的编码策略很简单——把 uuid 数据
 * 的前 sizeof(Datum) 个字节打包进一个 Datum（在
 * 小端机器上字节按相反顺序存放），并将其视为
 * 无符号数。
 */
static Datum
uuid_abbrev_convert(Datum original, SortSupport ssup)
{
	uuid_sortsupport_state *uss = ssup->ssup_extra;
	pg_uuid_t  *authoritative = DatumGetUUIDP(original);
	Datum		res;

	memcpy(&res, authoritative->data, sizeof(Datum));
	uss->input_count += 1;

	if (uss->estimating)
	{
		uint32		tmp;

#if SIZEOF_DATUM == 8
		tmp = (uint32) res ^ (uint32) ((uint64) res >> 32);
#else							/* Datum 非8字节 */
		tmp = (uint32) res;
#endif

		addHyperLogLog(&uss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	/*
	 * 小端机器上做字节交换。
	 *
	 * 这是为了让 ssup_datum_unsigned_cmp()（一个无符
	 * 号整数三路比较器）在所有平台上都正确工作。
	 * 若不这样做，比较器将不得不以指向各缩
	 * 写键首字节的指针对来调用 memcmp()，那样更慢。
	 */
	res = DatumBigEndianToNative(res);

	return res;
}

/* hash 索引支持 */
Datum
uuid_hash(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *key = PG_GETARG_UUID_P(0);

	return hash_any(key->data, UUID_LEN);
}

Datum
uuid_hash_extended(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *key = PG_GETARG_UUID_P(0);

	return hash_any_extended(key->data, UUID_LEN, PG_GETARG_INT64(1));
}

Datum
gen_random_uuid(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = palloc(UUID_LEN);

	if (!pg_strong_random(uuid, UUID_LEN))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random values")));

	/*
	 * 为 "version 4"（伪随机）UUID 设置魔数，见
	 * http://tools.ietf.org/html/rfc4122#section-4.4
	 */
	uuid->data[6] = (uuid->data[6] & 0x0f) | 0x40;	/* time_hi_and_version */
	uuid->data[8] = (uuid->data[8] & 0x3f) | 0x80;	/* clock_seq_hi_and_reserved */

	PG_RETURN_UUID_P(uuid);
}
