/*-------------------------------------------------------------------------
 *
 * varlena.c
 *	  变长内建类型的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/varlena.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/unicode_norm.h"
#include "funcapi.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "parser/scansup.h"
#include "port/pg_bswap.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/sortsupport.h"
#include "utils/varlena.h"


/* GUC 变量 */
int			bytea_output = BYTEA_OUTPUT_HEX;

typedef struct varlena VarString;

/*
 * text_position_* 函数的状态。
 */
typedef struct
{
	bool		is_multibyte_char_in_char;	/* 需要检查字符边界? */

	char	   *str1;			/* 待搜索的串 */
	char	   *str2;			/* 模式串 */
	int			len1;			/* 以字节计的串长 */
	int			len2;

	/* Boyer-Moore-Horspool 搜索算法的跳过表： */
	int			skiptablemask;	/* 与跳过表下标做与运算的掩码 */
	int			skiptable[256]; /* 给定失配字符的跳过距离 */

	char	   *last_match;		/* 'str1' 中上次匹配处 */

	/*
	 * 有时我们需要把匹配的字节位置转换为
	 * 字符位置。这两个字段保存上次转换的位置，
	 * 以便下一次调用时从该点继续，而不必
	 * 从头开始数字符。
	 */
	char	   *refpoint;		/* 原始干草堆串内的指针 */
	int			refpos;			/* 同一位置的 0 起算字符偏移 */
} TextPositionState;

typedef struct
{
	char	   *buf1;			/* 第一串，或缩写的原始串缓冲
								 * 区 */
	char	   *buf2;			/* 第二串，或缩写 strxfrm() 缓冲 */
	int			buflen1;		/* buf1 的分配长度 */
	int			buflen2;		/* buf2 的分配长度 */
	int			last_len1;		/* 上次 buf1 串/strxfrm() 输入的长度 */
	int			last_len2;		/* 上次 buf2 串/strxfrm() 块长 */
	int			last_returned;	/* 上次比较结果（缓存） */
	bool		cache_blob;		/* buf2 是否包含 strxfrm() 块等? */
	bool		collate_c;
	Oid			typid;			/* 实际类型（text/bpchar/bytea/name） */
	hyperLogLogState abbr_card; /* 缩写键基数状态 */
	hyperLogLogState full_card; /* 完整键基数状态 */
	double		prop_card;		/* 所需的基数比例 */
	pg_locale_t locale;
} VarStringSortSupport;

/*
 * split_text() 的输出数据：我们要么输出到数组，
 * 要么输出到表。tupstore/tupdesc 需为表输出预先建好。
 */
typedef struct
{
	ArrayBuildState *astate;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
} SplitTextOutputData;

/*
 * 该值应大到足以容纳大多数字符串，但又小到
 * 让我们能放心地把它放在栈上
 */
#define TEXTBUFLEN		1024

#define DatumGetVarStringP(X)		((VarString *) PG_DETOAST_DATUM(X))
#define DatumGetVarStringPP(X)		((VarString *) PG_DETOAST_DATUM_PACKED(X))

static int	varstrfastcmp_c(Datum x, Datum y, SortSupport ssup);
static int	bpcharfastcmp_c(Datum x, Datum y, SortSupport ssup);
static int	namefastcmp_c(Datum x, Datum y, SortSupport ssup);
static int	varlenafastcmp_locale(Datum x, Datum y, SortSupport ssup);
static int	namefastcmp_locale(Datum x, Datum y, SortSupport ssup);
static int	varstrfastcmp_locale(char *a1p, int len1, char *a2p, int len2, SortSupport ssup);
static Datum varstr_abbrev_convert(Datum original, SortSupport ssup);
static bool varstr_abbrev_abort(int memtupcount, SortSupport ssup);
static int32 text_length(Datum str);
static text *text_catenate(text *t1, text *t2);
static text *text_substring(Datum str,
							int32 start,
							int32 length,
							bool length_not_specified);
static text *text_overlay(text *t1, text *t2, int sp, int sl);
static int	text_position(text *t1, text *t2, Oid collid);
static void text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state);
static bool text_position_next(TextPositionState *state);
static char *text_position_next_internal(char *start_ptr, TextPositionState *state);
static char *text_position_get_match_ptr(TextPositionState *state);
static int	text_position_get_match_pos(TextPositionState *state);
static void text_position_cleanup(TextPositionState *state);
static void check_collation_set(Oid collid);
static int	text_cmp(text *arg1, text *arg2, Oid collid);
static bytea *bytea_catenate(bytea *t1, bytea *t2);
static bytea *bytea_substring(Datum str,
							  int S,
							  int L,
							  bool length_not_specified);
static bytea *bytea_overlay(bytea *t1, bytea *t2, int sp, int sl);
static void appendStringInfoText(StringInfo str, const text *t);
static bool split_text(FunctionCallInfo fcinfo, SplitTextOutputData *tstate);
static void split_text_accum_result(SplitTextOutputData *tstate,
									text *field_value,
									text *null_string,
									Oid collation);
static text *array_to_text_internal(FunctionCallInfo fcinfo, ArrayType *v,
									const char *fldsep, const char *null_string);
static StringInfo makeStringAggState(FunctionCallInfo fcinfo);
static bool text_format_parse_digits(const char **ptr, const char *end_ptr,
									 int *value);
static const char *text_format_parse_format(const char *start_ptr,
											const char *end_ptr,
											int *argpos, int *widthpos,
											int *flags, int *width);
static void text_format_string_conversion(StringInfo buf, char conversion,
										  FmgrInfo *typOutputInfo,
										  Datum value, bool isNull,
										  int flags, int width);
static void text_format_append_string(StringInfo buf, const char *str,
									  int flags, int width);


/*****************************************************************************
 *	 供 C 代码使用的转换例程                     *
 *****************************************************************************/

/*
 * cstring_to_text
 *
 * 由以 null 结尾的 C 字符串创建 text 值。
 *
 * 新 text 值以完整 VARHDR 经 palloc 新分配。
 */
text *
cstring_to_text(const char *s)
{
	return cstring_to_text_with_len(s, strlen(s));
}

/*
 * cstring_to_text_with_len
 *
 * 与 cstring_to_text 相同，但由调用者指定串长；
 * 串不必以 null 结尾。
 */
text *
cstring_to_text_with_len(const char *s, int len)
{
	text	   *result = (text *) palloc(len + VARHDRSZ);

	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), s, len);

	return result;
}

/*
 * text_to_cstring
 *
 * 从 text 值创建 palloc'd、null 结尾的 C 字符串。
 *
 * 我们支持传入压缩或 toast 化的 text 值。
 * 这有点不妥，因为这样的值本不该被称作
 * "text *"，但看起来对健壮性有用。如果不在这里
 * 反正也需要一个做此事的例程。
 */
char *
text_to_cstring(const text *t)
{
	/* 很遗憾，必须抛弃 const */
	text	   *tunpacked = pg_detoast_datum_packed(unconstify(text *, t));
	int			len = VARSIZE_ANY_EXHDR(tunpacked);
	char	   *result;

	result = (char *) palloc(len + 1);
	memcpy(result, VARDATA_ANY(tunpacked), len);
	result[len] = '\0';

	if (tunpacked != t)
		pfree(tunpacked);

	return result;
}

/*
 * text_to_cstring_buffer
 *
 * 把 text 值复制到调用者给定的 dst_len 缓冲区。
 *
 * 必要时截断 text 串以适配，结果保证 null 结尾
 * （dst_len == 0 时除外）。
 *
 * 我们支持传入压缩或 toast 化的 text 值。
 * 这有点不妥，因为这样的值本不该被称作
 * "text *"，但看起来对健壮性有用。如果不在这里
 * 反正也需要一个做此事的例程。
 */
void
text_to_cstring_buffer(const text *src, char *dst, size_t dst_len)
{
	/* 很遗憾，必须抛弃 const */
	text	   *srcunpacked = pg_detoast_datum_packed(unconstify(text *, src));
	size_t		src_len = VARSIZE_ANY_EXHDR(srcunpacked);

	if (dst_len > 0)
	{
		dst_len--;
		if (dst_len >= src_len)
			dst_len = src_len;
		else					/* 确保截断是编码安全的 */
			dst_len = pg_mbcliplen(VARDATA_ANY(srcunpacked), src_len, dst_len);
		memcpy(dst, VARDATA_ANY(srcunpacked), dst_len);
		dst[dst_len] = '\0';
	}

	if (srcunpacked != src)
		pfree(srcunpacked);
}


/*****************************************************************************
 *	 用户 I/O 例程               *
 *****************************************************************************/


#define VAL(CH)			((CH) - '0')
#define DIG(VAL)		((VAL) + '0')

/*
 *		byteain			- 把字节数组的可打印表示转为内部形式
 *
 *		非打印字符必须以 \nnn（八进制）传入，并会被
 *		转换为内部形式。\ 必须写成 \\。
 *		格式错 ereport(ERROR, ...)。
 *
 *		BUGS:
 *				输入会被扫描两次。
 *				对输入的错误检查极少。
 */
Datum
byteain(PG_FUNCTION_ARGS)
{
	char	   *inputText = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	char	   *tp;
	char	   *rp;
	int			bc;
	bytea	   *result;

	/* 识别 hex 输入 */
	if (inputText[0] == '\\' && inputText[1] == 'x')
	{
		size_t		len = strlen(inputText);

		bc = (len - 2) / 2 + VARHDRSZ;	/* 最大可能长度 */
		result = palloc(bc);
		bc = hex_decode_safe(inputText + 2, len - 2, VARDATA(result),
							 escontext);
		SET_VARSIZE(result, bc + VARHDRSZ); /* 实际长度 */

		PG_RETURN_BYTEA_P(result);
	}

	/* 否则就是传统转义风格 */
	for (bc = 0, tp = inputText; *tp != '\0'; bc++)
	{
		if (tp[0] != '\\')
			tp++;
		else if ((tp[0] == '\\') &&
				 (tp[1] >= '0' && tp[1] <= '3') &&
				 (tp[2] >= '0' && tp[2] <= '7') &&
				 (tp[3] >= '0' && tp[3] <= '7'))
			tp += 4;
		else if ((tp[0] == '\\') &&
				 (tp[1] == '\\'))
			tp += 2;
		else
		{
			/*
			 * 一个反斜杠，后面不是反斜杠或 ### 八进制
			 */
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "bytea")));
		}
	}

	bc += VARHDRSZ;

	result = (bytea *) palloc(bc);
	SET_VARSIZE(result, bc);

	tp = inputText;
	rp = VARDATA(result);
	while (*tp != '\0')
	{
		if (tp[0] != '\\')
			*rp++ = *tp++;
		else if ((tp[0] == '\\') &&
				 (tp[1] >= '0' && tp[1] <= '3') &&
				 (tp[2] >= '0' && tp[2] <= '7') &&
				 (tp[3] >= '0' && tp[3] <= '7'))
		{
			bc = VAL(tp[1]);
			bc <<= 3;
			bc += VAL(tp[2]);
			bc <<= 3;
			*rp++ = bc + VAL(tp[3]);

			tp += 4;
		}
		else if ((tp[0] == '\\') &&
				 (tp[1] == '\\'))
		{
			*rp++ = '\\';
			tp += 2;
		}
		else
		{
			/*
			 * 不应到达此处：第一遍扫描应已阻止它。
			 */
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "bytea")));
		}
	}

	PG_RETURN_BYTEA_P(result);
}

/*
 *		byteaout		- 把字节数组转换为可打印表示形式
 *
 *		传统转义格式中不可打印字符以 \nnn（八进制）
 *		打印，而 \ 打印为 \\。
 */
Datum
byteaout(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_PP(0);
	char	   *result;
	char	   *rp;

	if (bytea_output == BYTEA_OUTPUT_HEX)
	{
		/* 打印 hex */
		rp = result = palloc(VARSIZE_ANY_EXHDR(vlena) * 2 + 2 + 1);
		*rp++ = '\\';
		*rp++ = 'x';
		rp += hex_encode(VARDATA_ANY(vlena), VARSIZE_ANY_EXHDR(vlena), rp);
	}
	else if (bytea_output == BYTEA_OUTPUT_ESCAPE)
	{
		/* 打印传统转义格式 */
		char	   *vp;
		uint64		len;
		int			i;

		len = 1;				/* 空串有 1 个字符 */
		vp = VARDATA_ANY(vlena);
		for (i = VARSIZE_ANY_EXHDR(vlena); i != 0; i--, vp++)
		{
			if (*vp == '\\')
				len += 2;
			else if ((unsigned char) *vp < 0x20 || (unsigned char) *vp > 0x7e)
				len += 4;
			else
				len++;
		}

		/*
		 * 原则上若输入在 1GB 内，len 就不会溢出 uint32，但
		 * 为安全起见自行检查，而非依赖 palloc 内部
		 *
		 */
		if (len > MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg_internal("result of bytea output conversion is too large")));
		rp = result = (char *) palloc(len);

		vp = VARDATA_ANY(vlena);
		for (i = VARSIZE_ANY_EXHDR(vlena); i != 0; i--, vp++)
		{
			if (*vp == '\\')
			{
				*rp++ = '\\';
				*rp++ = '\\';
			}
			else if ((unsigned char) *vp < 0x20 || (unsigned char) *vp > 0x7e)
			{
				int			val;	/* 存不可打印字符 */

				val = *vp;
				rp[0] = '\\';
				rp[3] = DIG(val & 07);
				val >>= 3;
				rp[2] = DIG(val & 07);
				val >>= 3;
				rp[1] = DIG(val & 03);
				rp += 4;
			}
			else
				*rp++ = *vp;
		}
	}
	else
	{
		elog(ERROR, "unrecognized bytea_output setting: %d",
			 bytea_output);
		rp = result = NULL;		/* 让编译器安静 */
	}
	*rp = '\0';
	PG_RETURN_CSTRING(result);
}

/*
 *		bytearecv			- 将外部二进制格式转换为 bytea
 */
Datum
bytearecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	bytea	   *result;
	int			nbytes;

	nbytes = buf->len - buf->cursor;
	result = (bytea *) palloc(nbytes + VARHDRSZ);
	SET_VARSIZE(result, nbytes + VARHDRSZ);
	pq_copymsgbytes(buf, VARDATA(result), nbytes);
	PG_RETURN_BYTEA_P(result);
}

/*
 *		byteasend			- 把 bytea 转为二进制格式
 *
 * 这是一个特例：只需复制输入...
 */
Datum
byteasend(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_P_COPY(0);

	PG_RETURN_BYTEA_P(vlena);
}

Datum
bytea_string_agg_transfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	/* 非 null 时附加该值，前置分隔符。 */
	if (!PG_ARGISNULL(1))
	{
		bytea	   *value = PG_GETARG_BYTEA_PP(1);
		bool		isfirst = false;

		/*
		 * 你可能认为可以径直丢弃第一个分隔符，然而
		 * 必须保留它：我们可能是并行 worker，
		 * 正在做部分聚合并构建要发给主进程的状态。每个
		 * 聚合的分隔符都要保留，让 combine 函数能正确
		 * 拼接两个分别部分聚合的串。
		 * 第一个分隔符只在最终函数中被剥离。要知道应从
		 * 串首剥掉多少，我们把第一个分隔符的长度存进
		 * StringInfo 的 cursor 字段，此处无其他用途。
		 *
		 */
		if (state == NULL)
		{
			state = makeStringAggState(fcinfo);
			isfirst = true;
		}

		if (!PG_ARGISNULL(2))
		{
			bytea	   *delim = PG_GETARG_BYTEA_PP(2);

			appendBinaryStringInfo(state, VARDATA_ANY(delim),
								   VARSIZE_ANY_EXHDR(delim));
			if (isfirst)
				state->cursor = VARSIZE_ANY_EXHDR(delim);
		}

		appendBinaryStringInfo(state, VARDATA_ANY(value),
							   VARSIZE_ANY_EXHDR(value));
	}

	/*
	 * string_agg() 的转换类型被声明为 "internal"，
	 * 这是一个与指针同尺寸的按值传递类型。
	 */
	if (state)
		PG_RETURN_POINTER(state);
	PG_RETURN_NULL();
}

Datum
bytea_string_agg_finalfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	/* 由于有 internal 类型参数，无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	if (state != NULL)
	{
		/* 按 transfn 注释所述，剥离 cursor 位置前的数据 */
		bytea	   *result;
		int			strippedlen = state->len - state->cursor;

		result = (bytea *) palloc(strippedlen + VARHDRSZ);
		SET_VARSIZE(result, strippedlen + VARHDRSZ);
		memcpy(VARDATA(result), &state->data[state->cursor], strippedlen);
		PG_RETURN_BYTEA_P(result);
	}
	else
		PG_RETURN_NULL();
}

/*
 *		textin			- 把 cstring 转为内部表示
 */
Datum
textin(PG_FUNCTION_ARGS)
{
	char	   *inputText = PG_GETARG_CSTRING(0);

	PG_RETURN_TEXT_P(cstring_to_text(inputText));
}

/*
 *		textout			- 把内部表示转为 cstring
 */
Datum
textout(PG_FUNCTION_ARGS)
{
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 *		textrecv			- 将外部二进制格式转换为 text
 */
Datum
textrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	text	   *result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	result = cstring_to_text_with_len(str, nbytes);
	pfree(str);
	PG_RETURN_TEXT_P(result);
}

/*
 *		textsend			- 把 text 转为二进制格式
 */
Datum
textsend(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 *		unknownin			- 把 cstring 转为内部表示
 */
Datum
unknownin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	/* 表示与 cstring 相同 */
	PG_RETURN_CSTRING(pstrdup(str));
}

/*
 *		unknownout			- 把内部表示转为 cstring
 */
Datum
unknownout(PG_FUNCTION_ARGS)
{
	/* 表示与 cstring 相同 */
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_CSTRING(pstrdup(str));
}

/*
 *		unknownrecv			- 将外部二进制格式转换为 unknown
 */
Datum
unknownrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	/* 表示与 cstring 相同 */
	PG_RETURN_CSTRING(str);
}

/*
 *		unknownsend			- 把 unknown 转为二进制格式
 */
Datum
unknownsend(PG_FUNCTION_ARGS)
{
	/* 表示与 cstring 相同 */
	char	   *str = PG_GETARG_CSTRING(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, str, strlen(str));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/* ========== 公共例程 ========== */

/*
 * textlen -
 *	  返回 text* 的逻辑长度
 *	   （小于该 text* 的 VARSIZE）
 */
Datum
textlen(PG_FUNCTION_ARGS)
{
	Datum		str = PG_GETARG_DATUM(0);

	/* 尽量避免解压缩参数 */
	PG_RETURN_INT32(text_length(str));
}

/*
 * text_length -
 *	做 textlen() 的实际工作
 *
 *	单独拆出此函数是为了让其他字符串处理函数可以直接
 *	调用它。注意参数以 Datum 传入，以表明它可能仍处于
 *	压缩形式。某些情况下我们可以完全避免解压缩它。
 *
 */
static int32
text_length(Datum str)
{
	/* 编码最大长度 1 时走快速路径 */
	if (pg_database_encoding_max_length() == 1)
		PG_RETURN_INT32(toast_raw_datum_size(str) - VARHDRSZ);
	else
	{
		text	   *t = DatumGetTextPP(str);

		PG_RETURN_INT32(pg_mbstrlen_with_len(VARDATA_ANY(t),
											 VARSIZE_ANY_EXHDR(t)));
	}
}

/*
 * textoctetlen -
 *	  返回 text* 的物理长度
 *	   （小于该 text* 的 VARSIZE）
 */
Datum
textoctetlen(PG_FUNCTION_ARGS)
{
	Datum		str = PG_GETARG_DATUM(0);

	/* 完全无需 detoast 输入 */
	PG_RETURN_INT32(toast_raw_datum_size(str) - VARHDRSZ);
}

/*
 * textcat -
 *	  取两个 text* 并返回两者拼接成的 text*。
 *
 *
 * Rewritten by Sapa, sapa@hq.icb.chel.su. 8-Jul-96.
 * Updated by Thomas, Thomas.Lockhart@jpl.nasa.gov 1997-07-10.
 * 无条件为输出分配空间。
 * XXX - thomas 1997-07-10
 */
Datum
textcat(PG_FUNCTION_ARGS)
{
	text	   *t1 = PG_GETARG_TEXT_PP(0);
	text	   *t2 = PG_GETARG_TEXT_PP(1);

	PG_RETURN_TEXT_P(text_catenate(t1, t2));
}

/*
 * text_catenate
 *	textcat() 的核心逻辑，拆出以便其他函数使用
 *
 * 参数可为短头部形式，但不可为压缩或行外存储形式
 */
static text *
text_catenate(text *t1, text *t2)
{
	text	   *result;
	int			len1,
				len2,
				len;
	char	   *ptr;

	len1 = VARSIZE_ANY_EXHDR(t1);
	len2 = VARSIZE_ANY_EXHDR(t2);

	/* 偏执起见……或许该改为抛出错误？ */
	if (len1 < 0)
		len1 = 0;
	if (len2 < 0)
		len2 = 0;

	len = len1 + len2 + VARHDRSZ;
	result = (text *) palloc(len);

	/* 设置结果串大小…… */
	SET_VARSIZE(result, len);

	/* 填充结果串数据字段…… */
	ptr = VARDATA(result);
	if (len1 > 0)
		memcpy(ptr, VARDATA_ANY(t1), len1);
	if (len2 > 0)
		memcpy(ptr + len1, VARDATA_ANY(t2), len2);

	return result;
}

/*
 * charlen_to_bytelen()
 *	计算从 *p 开始的 n 个字符占用的字节数
 *
 * 调用者须确保串中确实存在 n 个字符；串不必以
 * null 结尾。
 */
static int
charlen_to_bytelen(const char *p, int n)
{
	if (pg_database_encoding_max_length() == 1)
	{
		/* 单字节编码的优化 */
		return n;
	}
	else
	{
		const char *s;

		for (s = p; n > 0; n--)
			s += pg_mblen(s);

		return s - p;
	}
}

/*
 * text_substr()
 * 返回从指定位置开始的子串。
 * - thomas 1997-12-31
 *
 * Input:
 *	- string
 *	- 起始位置（1 起算）
 *	- 串长度
 *
 * 若起始位置为零或更小，则从串首开始返回，
 *	并按 SQL 的"负起始"约定调整长度。
 * 若长度小于零，则返回剩余串。
 *
 * 增加多字节支持。
 * - Tatsuo Ishii 1998-4-21
 * 为符合 SQL 行为，改变了起始位置小于 1 时的行为。
 * 以前返回整个串；现在返回其中一部分。
 * - Thomas Lockhart 1998-12-10
 * 现用更快的 TOAST 切片接口
 * - John Gray 2002-02-22
 * 移除 "#ifdef MULTIBYTE"，改为检查 encoding_max_length。
 * 将与 SQL 冲突的行为改为满足 SQL（若 E = S + L < S 则
 * 抛错误；若 E < 1，返回 ''，而非整个串）。修复了 MB
 * bug：S > LC 且 < LC+4 时有时返回垃圾字符。
 * - Joe Conway 2002-08-10
 */
Datum
text_substr(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									PG_GETARG_INT32(2),
									false));
}

/*
 * text_substr_no_len -
 *	  包装函数，避免 opr_sanity 因
 *	  同一函数接受不同数量的参数而失败
 */
Datum
text_substr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									-1, true));
}

/*
 * text_substring -
 *	text_substr() 与 text_substr_no_len() 的实际工作
 *
 *	单独拆出此函数是为了让其他字符串处理函数可以直接
 *	调用它。注意参数以 Datum 传入，以表明它可能仍处于
 *	压缩/toast 化形式。某些情况下可免于全部去 toast。
 *
 *
 *	结果总是新近 palloc 的 datum。
 */
static text *
text_substring(Datum str, int32 start, int32 length, bool length_not_specified)
{
	int32		eml = pg_database_encoding_max_length();
	int32		S = start;		/* 起始位置 */
	int32		S1;				/* 调整后起始位置 */
	int32		L1;				/* 调整后的子串长度 */
	int32		E;				/* 结束位置 */

	/*
	 * SQL99 说 S 可以为零或负，但我们仍须从串首开始
	 * 取数据。
	 */
	S1 = Max(S, 1);

	/* 若编码最大长度为 1，则事情简单 */
	if (eml == 1)
	{
		if (length_not_specified)	/* 特例——取长度至
									 * 串尾 */
			L1 = -1;
		else if (length < 0)
		{
			/* SQL99 规定 E < S（即长度为负）时抛出错误 */
			ereport(ERROR,
					(errcode(ERRCODE_SUBSTRING_ERROR),
					 errmsg("negative substring length not allowed")));
			L1 = -1;			/* 让较笨编译器安静 */
		}
		else if (pg_add_s32_overflow(S, length, &E))
		{
			/*
			 * L 可能大到使 S + L 溢出，此时子串必须延伸到
			 * 串尾。
			 */
			L1 = -1;
		}
		else
		{
			/*
			 * 结束位置出现零或负值的情形，可能发生在起始
			 * 位置为负或一时。SQL99 规定返回零长度串。
			 *
			 */
			if (E < 1)
				return cstring_to_text("");

			L1 = E - S1;
		}

		/*
		 * 若起始位置超过串尾，SQL99 规定返回零长度串——
		 * DatumGetTextPSlice() 会替我们做到。只需把 S1 转为
		 * 0 起算的起始位置即可。
		 */
		return DatumGetTextPSlice(str, S1 - 1, L1);
	}
	else if (eml > 1)
	{
		/*
		 * 当编码最大长度 > 1，需去 toast 才能取 LC，
		 * 所以现在先保守地取一大片切片，稍后再回来
		 * 正确处理
		 */
		int32		slice_start;
		int32		slice_size;
		int32		slice_strlen;
		text	   *slice;
		int32		E1;
		int32		i;
		char	   *p;
		char	   *s;
		text	   *ret;

		/*
		 * 必须从位置 0 开始，因为无法预知哪个字节偏移
		 * 对应给定的起始位置。
		 *
		 */
		slice_start = 0;

		if (length_not_specified)	/* 特例——取长度至
									 * 串尾 */
			slice_size = L1 = -1;
		else if (length < 0)
		{
			/* SQL99 规定 E < S（即长度为负）时抛出错误 */
			ereport(ERROR,
					(errcode(ERRCODE_SUBSTRING_ERROR),
					 errmsg("negative substring length not allowed")));
			slice_size = L1 = -1;	/* 让较笨编译器安静 */
		}
		else if (pg_add_s32_overflow(S, length, &E))
		{
			/*
			 * L 可能大到使 S + L 溢出，此时子串必须延伸到
			 * 串尾。
			 */
			slice_size = L1 = -1;
		}
		else
		{
			/*
			 * 结束位置出现零或负值的情形，可能发生在起始
			 * 位置为负或一时。SQL99 规定返回零长度串。
			 *
			 */
			if (E < 1)
				return cstring_to_text("");

			/*
			 * 若 E 超过串尾，TOAST 机（tuple toaster）
			 * 会替我们截断长度
			 */
			L1 = E - S1;

			/*
			 * 切片总字节数不会超过起始位置加上子串长度
			 * 乘以编码最大长度。若溢出，直接用 -1 即可。
			 *
			 */
			if (pg_mul_s32_overflow(E, eml, &slice_size))
				slice_size = -1;
		}

		/*
		 * 若源数据未被 toast，则无需额外的复制步骤。
		 *
		 */
		if (VARATT_IS_COMPRESSED(DatumGetPointer(str)) ||
			VARATT_IS_EXTERNAL(DatumGetPointer(str)))
			slice = DatumGetTextPSlice(str, slice_start, slice_size);
		else
			slice = (text *) DatumGetPointer(str);

		/* 看看是否取回了空串 */
		if (VARSIZE_ANY_EXHDR(slice) == 0)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

		/* 现在可以按多字节字符数取得切片的实际长度 */
		slice_strlen = pg_mbstrlen_with_len(VARDATA_ANY(slice),
											VARSIZE_ANY_EXHDR(slice));

		/*
		 * 检查起始位置是否大于 slice_strlen。若是，SQL99
		 * 规定返回零长度串。
		 */
		if (S1 > slice_strlen)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

		/*
		 * 现在已知切片串长，调整 L1 与 E1。再次记住 S1
		 * 以 1 起算，而 slice_start 以 0 起算。
		 */
		if (L1 > -1)
			E1 = Min(S1 + L1, slice_start + 1 + slice_strlen);
		else
			E1 = slice_start + 1 + slice_strlen;

		/*
		 * 在切片中找到起始位置；记住 S1 不是 0 起算的
		 */
		p = VARDATA_ANY(slice);
		for (i = 0; i < S1 - 1; i++)
			p += pg_mblen(p);

		/* 保留一个指向起始位置的指针 */
		s = p;

		/*
		 * 统计所请求长度的子串实际使用的字节数
		 *
		 */
		for (i = S1; i < E1; i++)
			p += pg_mblen(p);

		ret = (text *) palloc(VARHDRSZ + (p - s));
		SET_VARSIZE(ret, VARHDRSZ + (p - s));
		memcpy(VARDATA(ret), s, (p - s));

		if (slice != (text *) DatumGetPointer(str))
			pfree(slice);

		return ret;
	}
	else
		elog(ERROR, "invalid backend encoding: encoding max length < 1");

	/* 不会到达：抑制编译器警告 */
	return NULL;
}

/*
 * textoverlay
 *	用第二串替换第一串中指定的子串
 *
 * SQL 标准以子串与拼接来定义 OVERLAY()。
 * 此代码是对标准所述内容的直接实现。
 */
Datum
textoverlay(PG_FUNCTION_ARGS)
{
	text	   *t1 = PG_GETARG_TEXT_PP(0);
	text	   *t2 = PG_GETARG_TEXT_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* 子串起始位置 */
	int			sl = PG_GETARG_INT32(3);	/* 子串长度 */

	PG_RETURN_TEXT_P(text_overlay(t1, t2, sp, sl));
}

Datum
textoverlay_no_len(PG_FUNCTION_ARGS)
{
	text	   *t1 = PG_GETARG_TEXT_PP(0);
	text	   *t2 = PG_GETARG_TEXT_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* 子串起始位置 */
	int			sl;

	sl = text_length(PointerGetDatum(t2));	/* 缺省 length(t2) */
	PG_RETURN_TEXT_P(text_overlay(t1, t2, sp, sl));
}

static text *
text_overlay(text *t1, text *t2, int sp, int sl)
{
	text	   *result;
	text	   *s1;
	text	   *s2;
	int			sp_pl_sl;

	/*
	 * 检查可能的整数溢出情形。对负 sp，抛出"子串长度"
	 * 错误——按规范对 OVERLAY() 的定义这是应当
	 * 预期到的结果。
	 */
	if (sp <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
	if (pg_add_s32_overflow(sp, sl, &sp_pl_sl))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	s1 = text_substring(PointerGetDatum(t1), 1, sp - 1, false);
	s2 = text_substring(PointerGetDatum(t1), sp_pl_sl, -1, true);
	result = text_catenate(s1, t2);
	result = text_catenate(result, s2);

	return result;
}

/*
 * textpos -
 *	  返回指定子串的位置。
 *	  实现 SQL POSITION() 函数。
 *	  Ref: A Guide To The SQL Standard, Date & Darwen, 1997
 * - thomas 1997-07-27
 */
Datum
textpos(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	text	   *search_str = PG_GETARG_TEXT_PP(1);

	PG_RETURN_INT32((int32) text_position(str, search_str, PG_GET_COLLATION()));
}

/*
 * text_position -
 *	做 textpos() 的实际工作
 *
 * Inputs:
 *		t1 - 待搜索的串
 *		t2 - 在 t1 内匹配的模式
 * Result:
 *		首个匹配字符的字符索引，从 1 起算，
 *		无匹配则 0。
 *
 *	单独拆出此函数是为了让其他字符串处理函数可以直接
 *	调用。
 */
static int
text_position(text *t1, text *t2, Oid collid)
{
	TextPositionState state;
	int			result;

	/* 空 needle 总在位置 1 匹配 */
	if (VARSIZE_ANY_EXHDR(t2) < 1)
		return 1;

	/* 否则，若干草堆串比 needle 短则无法匹配 */
	if (VARSIZE_ANY_EXHDR(t1) < VARSIZE_ANY_EXHDR(t2))
		return 0;

	text_position_setup(t1, t2, collid, &state);
	if (!text_position_next(&state))
		result = 0;
	else
		result = text_position_get_match_pos(&state);
	text_position_cleanup(&state);
	return result;
}


/*
 * text_position_setup、text_position_next、
 *	text_position_cleanup 等步骤
 *
 * 这些步骤独立拆出，以便在一个串中高效地查找
 * 同一模式多次出现。text_position_next 可被
 * 调用多次，且每次调用都前进到下一个匹配处。
 * text_position_get_match_ptr() 与 text_position_
 * get_match_pos() 分别返回末次匹配的指针和字符位置。
 *
 * "state" 变量通常只是调用者中的一个局部变量。
 *
 * 注意：text_position_next 会跳过已匹配的部分。例如，
 * 在 "xxx" 内搜索 "xx" 只得一个匹配而非两个。
 */

static void
text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state)
{
	int			len1 = VARSIZE_ANY_EXHDR(t1);
	int			len2 = VARSIZE_ANY_EXHDR(t2);
	pg_locale_t mylocale = 0;

	check_collation_set(collid);

	if (!lc_collate_is_c(collid))
		mylocale = pg_newlocale_from_collation(collid);

	if (!pg_locale_deterministic(mylocale))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for substring searches")));

	Assert(len1 > 0);
	Assert(len2 > 0);

	/*
	 * 即便多字节编码，搜索仍按原始字节序列进行，
	 * 忽略多字节问题。对 UTF-8 无碍，因为 UTF-8 中
	 * 一个字符的字节序列不会包含另一字符。对其他
	 * 的多字节编码，先按简单字节搜索执行搜索，
	 * 同样忽略多字节问题，但事后验证匹配是否在
	 * 字符边界上，假匹配则继续搜索。
	 *
	 */
	if (pg_database_encoding_max_length() == 1)
		state->is_multibyte_char_in_char = false;
	else if (GetDatabaseEncoding() == PG_UTF8)
		state->is_multibyte_char_in_char = false;
	else
		state->is_multibyte_char_in_char = true;

	state->str1 = VARDATA_ANY(t1);
	state->str2 = VARDATA_ANY(t2);
	state->len1 = len1;
	state->len2 = len2;
	state->last_match = NULL;
	state->refpoint = state->str1;
	state->refpos = 0;

	/*
	 * 为 Boyer-Moore-Horspool 搜索准备跳过表。在这些
	 * 注释中我们的术语是："haystack"（干草堆）是被
	 * 搜索的串 t1，"needle"（针）是要找的模式 t2。
	 *
	 * 若 needle 为空或比 haystack 大，那么初始化表就是
	 * 浪费时间。对长度为 1 的 needle 我们也选择不用
	 * B-M-H，因为此时跳过表省不了任何东西。
	 *
	 */
	if (len1 >= len2 && len2 > 1)
	{
		int			searchlength = len1 - len2;
		int			skiptablemask;
		int			last;
		int			i;
		const char *str2 = state->str2;

		/*
		 * 首先必须确定跳过表要用多大。TextPositionState 的
		 * 声明允许最多 256 个元素，但对短搜索问题我们不
		 * 想初始化这么多元素——比起实际搜索时间，这样
		 * 做代价太高。故根据干草堆串长度减去
		 * needle 长度来选择一个有用的跳过表大小。needle
		 * 长度越接近干草堆串长度，跳过就越无用。
		 *
		 *
		 * 注意：我们用位掩码选择表元素，故跳过表大小
		 * 必须是 2 的幂，因此掩码必须是 2^N-1。
		 */
		if (searchlength < 16)
			skiptablemask = 3;
		else if (searchlength < 64)
			skiptablemask = 7;
		else if (searchlength < 128)
			skiptablemask = 15;
		else if (searchlength < 512)
			skiptablemask = 31;
		else if (searchlength < 2048)
			skiptablemask = 63;
		else if (searchlength < 4096)
			skiptablemask = 127;
		else
			skiptablemask = 255;
		state->skiptablemask = skiptablemask;

		/*
		 * 初始化跳过表：所有元素先设为 needle 长度，
		 * 因为这是任何不在 needle 中的字符的正确跳过
		 * 距离。
		 */
		for (i = 0; i <= skiptablemask; i++)
			state->skiptable[i] = len2;

		/*
		 * 现在检查 needle。对其最后一个字符之外的每个
		 * 字符，对应表元素设为相应跳过距离。注意
		 * 当两个字符共用同一跳过表项时，必须由 needle 中
		 * 较后的那个字符决定跳过距离。
		 *
		 */
		last = len2 - 1;

		for (i = 0; i < last; i++)
			state->skiptable[(unsigned char) str2[i] & skiptablemask] = last - i;
	}
}

/*
 * 从上一个匹配的结尾开始前进到下一个匹配（首次调
 * 用时从串首开始）。若找到匹配则返回 true。
 *
 *
 * 注意此函数拒绝匹配空串 needle。大多数调用者会
 * 专门处理那种情况，所以这里不会见到它。
 */
static bool
text_position_next(TextPositionState *state)
{
	int			needle_len = state->len2;
	char	   *start_ptr;
	char	   *matchptr;

	if (needle_len <= 0)
		return false;			/* 空模式的结果 */

	/* 从紧挨上一个匹配之后的点开始。 */
	if (state->last_match)
		start_ptr = state->last_match + needle_len;
	else
		start_ptr = state->str1;

retry:
	matchptr = text_position_next_internal(start_ptr, state);

	if (!matchptr)
		return false;

	/*
	 * 找到了字节序列的匹配。若是多字节编码，其中一个
	 * 字符的字节序列可能出现在更长
	 * 的多字节字符内部，需要验证匹配是在
	 * 字符边界上，而非多字节字符中间。
	 */
	if (state->is_multibyte_char_in_char)
	{
		/* 一次走一个字符，直到到达匹配处。 */

		/* 搜索绝不应向后移动。 */
		Assert(state->refpoint <= matchptr);

		while (state->refpoint < matchptr)
		{
			/* 移到下一字符。 */
			state->refpoint += pg_mblen(state->refpoint);
			state->refpos++;

			/*
			 * 若跨过了匹配的起始位置，则是假阳性——
			 * 字节序列出现在多字节字符中间。跳过它，从
			 * 下一字符边界处继续搜索。
			 *
			 */
			if (state->refpoint > matchptr)
			{
				start_ptr = state->refpoint;
				goto retry;
			}
		}
	}

	state->last_match = matchptr;
	return true;
}

/*
 * text_position_next() 的子例程。此函数搜索原始字节
 * 序列，忽略任何多字节编码问题。返回自 start_ptr
 * 开始的第一个匹配，若无匹配则返回 NULL。
 */
static char *
text_position_next_internal(char *start_ptr, TextPositionState *state)
{
	int			haystack_len = state->len1;
	int			needle_len = state->len2;
	int			skiptablemask = state->skiptablemask;
	const char *haystack = state->str1;
	const char *needle = state->str2;
	const char *haystack_end = &haystack[haystack_len];
	const char *hptr;

	Assert(start_ptr >= haystack && start_ptr <= haystack_end);

	if (needle_len == 1)
	{
		/* 单字符 needle 无须使用 B-M-H */
		char		nchar = *needle;

		hptr = start_ptr;
		while (hptr < haystack_end)
		{
			if (*hptr == nchar)
				return (char *) hptr;
			hptr++;
		}
	}
	else
	{
		const char *needle_last = &needle[needle_len - 1];

		/* 从 startpos 加 needle 长度处开始 */
		hptr = start_ptr + needle_len - 1;
		while (hptr < haystack_end)
		{
			/* 按*反向*扫描匹配 needle */
			const char *nptr;
			const char *p;

			nptr = needle_last;
			p = hptr;
			while (*nptr == *p)
			{
				/* 全匹配？若是返回 1 起算位置 */
				if (nptr == needle)
					return (char *) p;
				nptr--, p--;
			}

			/*
			 * 无匹配，于是用干草堆串中 hptr 处的字符决定
			 * 前进多远。若 needle 的最后一个字符之前出现
			 * 过该字符（或跳过表项相同者），则前进使
			 * needle 中最后出现的该字符对齐此干草堆位置。
			 * 否则可以按整个 needle 长度前进。
			 *
			 */
			hptr += state->skiptable[(unsigned char) *hptr & skiptablemask];
		}
	}

	return 0;					/* 未找到 */
}

/*
 * 返回指向当前匹配的指针。
 *
 * 返回的指针指向原始干草堆串内部。
 */
static char *
text_position_get_match_ptr(TextPositionState *state)
{
	return state->last_match;
}

/*
 * 返回当前匹配的偏移。
 *
 * 偏移以字符计，1 起算。
 */
static int
text_position_get_match_pos(TextPositionState *state)
{
	/* 把字节位置转换为字符位置。 */
	state->refpos += pg_mbstrlen_with_len(state->refpoint,
										  state->last_match - state->refpoint);
	state->refpoint = state->last_match;
	return state->refpos + 1;
}

/*
 * 把搜索状态重置为 text_position_setup 安装的初始
 *
 * 状态。下一次调用 text_position_next 将从串首
 * 搜起。
 */
static void
text_position_reset(TextPositionState *state)
{
	state->last_match = NULL;
	state->refpoint = state->str1;
	state->refpos = 0;
}

static void
text_position_cleanup(TextPositionState *state)
{
	/* 无需清理 */
}


static void
check_collation_set(Oid collid)
{
	if (!OidIsValid(collid))
	{
		/*
		 * 通常意味着解析器无法解决隐式排序规则冲突，
		 * 因此按该方式报告。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string comparison"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

/* varstr_cmp()
 * 对给定长度的文本串进行比较的函数。
 * 包含区域设置支持，但须把串复制到临时内存，
 *	以让 strcoll() 输入 null 结尾。
 * 返回一个小于、等于或大于零的整数，表明 arg1
 * 小于、等于还是大于 arg2。
 *
 * 注意：许多依赖此函数的函数都被标记为 leakproof；
 * 因此抛错时务必避免报告输入的实际内容。
 * 总之此处的所有错误都应当是除了数据损坏之外
 * 不可能发生的情况；否则给会造成这些错误的串
 * 会出麻烦。
 */
int
varstr_cmp(const char *arg1, int len1, const char *arg2, int len2, Oid collid)
{
	int			result;

	check_collation_set(collid);

	/*
	 * 遗憾的是没有 strncoll()，所以在非 C 区域设置下
	 * 我们必须做一些内存复制。这显著变慢，因此我们
	 * 对 LC_COLLATE 为 C 的情形做了优化。我们还尽量
	 * 通过避免 palloc/pfree 开销来优化较短的串。
	 */
	if (lc_collate_is_c(collid))
	{
		result = memcmp(arg1, arg2, Min(len1, len2));
		if ((result == 0) && (len1 != len2))
			result = (len1 < len2) ? -1 : 1;
	}
	else
	{
		pg_locale_t mylocale;

		mylocale = pg_newlocale_from_collation(collid);

		/*
		 * memcmp() 无法告知两个不等串哪个先排序，
		 * 但它是判断两者是否相等的廉价方法。测试表明
		 * memcmp()+strcoll() 只比单独 strcoll() 慢一点，
		 * 所以即使此法不常奏效我们也没损失多少；而若它
		 * 确实奏效——如输入中有很多相等的串——那么
		 * 通过避免昂贵的排序规则感知比较而大获其利。
		 *
		 */
		if (len1 == len2 && memcmp(arg1, arg2, len1) == 0)
			return 0;

		result = pg_strncoll(arg1, len1, arg2, len2, mylocale);

		/* 必要时破平局。 */
		if (result == 0 && pg_locale_deterministic(mylocale))
		{
			result = memcmp(arg1, arg2, Min(len1, len2));
			if ((result == 0) && (len1 != len2))
				result = (len1 < len2) ? -1 : 1;
		}
	}

	return result;
}

/* text_cmp()
 * 文本串的内部比较函数。
 * 返回 -1/0/1
 */
static int
text_cmp(text *arg1, text *arg2, Oid collid)
{
	char	   *a1p,
			   *a2p;
	int			len1,
				len2;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	return varstr_cmp(a1p, len1, a2p, len2, collid);
}

/*
 * 文本串的比较函数。
 *
 * 注意：btree 索引要求这些例程不泄漏内存；因此，
 * 对 toast 化 datum 的工作副本要小心释放。大多数地方
 * 不必如此小心。
 */

Datum
texteq(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GET_COLLATION();
	bool		locale_is_c = false;
	pg_locale_t mylocale = 0;
	bool		result;

	check_collation_set(collid);

	if (lc_collate_is_c(collid))
		locale_is_c = true;
	else
		mylocale = pg_newlocale_from_collation(collid);

	if (locale_is_c || pg_locale_deterministic(mylocale))
	{
		Datum		arg1 = PG_GETARG_DATUM(0);
		Datum		arg2 = PG_GETARG_DATUM(1);
		Size		len1,
					len2;

		/*
		 * 由于我们只关心相等或不相等，这里可以省去
		 * strcoll() 的开销，只做按位比较。事实上若能证明
		 * 两串长度不等，我们甚至无须做按位比较；这或许
		 * 能省去对其中一个或两个值去 toast。
		 *
		 */
		len1 = toast_raw_datum_size(arg1);
		len2 = toast_raw_datum_size(arg2);
		if (len1 != len2)
			result = false;
		else
		{
			text	   *targ1 = DatumGetTextPP(arg1);
			text	   *targ2 = DatumGetTextPP(arg2);

			result = (memcmp(VARDATA_ANY(targ1), VARDATA_ANY(targ2),
							 len1 - VARHDRSZ) == 0);

			PG_FREE_IF_COPY(targ1, 0);
			PG_FREE_IF_COPY(targ2, 1);
		}
	}
	else
	{
		text	   *arg1 = PG_GETARG_TEXT_PP(0);
		text	   *arg2 = PG_GETARG_TEXT_PP(1);

		result = (text_cmp(arg1, arg2, collid) == 0);

		PG_FREE_IF_COPY(arg1, 0);
		PG_FREE_IF_COPY(arg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
textne(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GET_COLLATION();
	bool		locale_is_c = false;
	pg_locale_t mylocale = 0;
	bool		result;

	check_collation_set(collid);

	if (lc_collate_is_c(collid))
		locale_is_c = true;
	else
		mylocale = pg_newlocale_from_collation(collid);

	if (locale_is_c || pg_locale_deterministic(mylocale))
	{
		Datum		arg1 = PG_GETARG_DATUM(0);
		Datum		arg2 = PG_GETARG_DATUM(1);
		Size		len1,
					len2;

		/* 见 texteq() 内注释 */
		len1 = toast_raw_datum_size(arg1);
		len2 = toast_raw_datum_size(arg2);
		if (len1 != len2)
			result = true;
		else
		{
			text	   *targ1 = DatumGetTextPP(arg1);
			text	   *targ2 = DatumGetTextPP(arg2);

			result = (memcmp(VARDATA_ANY(targ1), VARDATA_ANY(targ2),
							 len1 - VARHDRSZ) != 0);

			PG_FREE_IF_COPY(targ1, 0);
			PG_FREE_IF_COPY(targ2, 1);
		}
	}
	else
	{
		text	   *arg1 = PG_GETARG_TEXT_PP(0);
		text	   *arg2 = PG_GETARG_TEXT_PP(1);

		result = (text_cmp(arg1, arg2, collid) != 0);

		PG_FREE_IF_COPY(arg1, 0);
		PG_FREE_IF_COPY(arg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
text_lt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) < 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_le(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) <= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_gt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) > 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_ge(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = (text_cmp(arg1, arg2, PG_GET_COLLATION()) >= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
text_starts_with(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	Oid			collid = PG_GET_COLLATION();
	pg_locale_t mylocale = 0;
	bool		result;
	Size		len1,
				len2;

	check_collation_set(collid);

	if (!lc_collate_is_c(collid))
		mylocale = pg_newlocale_from_collation(collid);

	if (!pg_locale_deterministic(mylocale))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for substring searches")));

	len1 = toast_raw_datum_size(arg1);
	len2 = toast_raw_datum_size(arg2);
	if (len2 > len1)
		result = false;
	else
	{
		text	   *targ1 = text_substring(arg1, 1, len2, false);
		text	   *targ2 = DatumGetTextPP(arg2);

		result = (memcmp(VARDATA_ANY(targ1), VARDATA_ANY(targ2),
						 VARSIZE_ANY_EXHDR(targ2)) == 0);

		PG_FREE_IF_COPY(targ1, 0);
		PG_FREE_IF_COPY(targ2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
bttextcmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int32		result;

	result = text_cmp(arg1, arg2, PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

Datum
bttextsortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	Oid			collid = ssup->ssup_collation;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 用通用字符串 SortSupport */
	varstr_sortsupport(ssup, TEXTOID, collid);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}

/*
 * 字符类型算子类的通用 sortsupport 接口。包含区域
 * 设置支持，以及对 BpChar 语义的支持（即比较前移除
 * 尾部空格）。
 *
 * 依赖 text、VarChar、BpChar 与 bytea 具有相同表示形
 * 式的假设。始终使用 C 排序规则的调用者（例如
 * bytea 这类不可排序类型）的串中可能有 NUL 字节；
 * 但这在其他排序规则下行不通。
 */
void
varstr_sortsupport(SortSupport ssup, Oid typid, Oid collid)
{
	bool		abbreviate = ssup->abbreviate;
	bool		collate_c = false;
	VarStringSortSupport *sss;
	pg_locale_t locale = 0;

	check_collation_set(collid);

	/*
	 * 若可能，把 ssup->comparator 设为一个可直接比较两
	 * 个 datum 的函数。若能做到，就免了每次比较
	 * 都要经过 fmgr 层的开销，这可能相当可观。
	 *
	 *
	 * 最典型的是把比较器设为 varlenafastcmp_locale，
	 * 它用 strcoll() 执行比较。BpChar 情形我们也用它，
	 * 而类型 NAME 用 namefastcmp_locale。不过若
	 * LC_COLLATE = C，则可以用 varstrfastcmp_c、
	 * bpcharfastcmp_c 或 namefastcmp_c，它们都用
	 * memcmp() 而非 strcoll()。
	 */
	if (lc_collate_is_c(collid))
	{
		if (typid == BPCHAROID)
			ssup->comparator = bpcharfastcmp_c;
		else if (typid == NAMEOID)
		{
			ssup->comparator = namefastcmp_c;
			/* 目前不支持类型 NAME 使用缩写 */
			abbreviate = false;
		}
		else
			ssup->comparator = varstrfastcmp_c;

		collate_c = true;
	}
	else
	{
		/*
		 * 需要排序规则敏感的比较。为提速，将基于
		 * 区域设置 id 求出排序规则并缓存结果。
		 *
		 */
		locale = pg_newlocale_from_collation(collid);

		/*
		 * 类型 NAME 之外都用 varlenafastcmp_locale。
		 */
		if (typid == NAMEOID)
		{
			ssup->comparator = namefastcmp_locale;
			/* 目前不支持类型 NAME 使用缩写 */
			abbreviate = false;
		}
		else
			ssup->comparator = varlenafastcmp_locale;
	}

	/*
	 * 遗憾的是，非 C 排序规则缩写在很多常见平台上
	 * 似乎已损坏；参见 pg_strxfrm_enabled()。
	 *
	 * 即便撇开损坏的区域设置的风险不谈，也可能存在
	 * 这样的平台：缩写键的使用应当在编译期禁用。如
	 * 果只有 4 字节 datum，最坏情况性能就可能急剧
	 * 变差。此外，macOS 的 strxfrm() 实现已知不能
	 * 有效地把原串的大量熵集中到较早的变换块中。其
	 * 他受支持的平台可能同样受制。所以若我们
	 * 最终越过一揽子禁用它的阶段，可能仍然想要或需
	 * 要针对特定平台禁用它。
	 *
	 *
	 */
	if (!collate_c && !pg_strxfrm_enabled(locale))
		abbreviate = false;

	/*
	 * 若用缩写键，或用区域设置感知的比较，我们
	 * 需要初始化一个 VarStringSortSupport 对象。两种
	 * 情形都会用到我们在这里初始化的临时缓冲区（作
	 * 为暂存空间，并从调用者探测 BpChar 语义需求），
	 * 缩写情形还需要额外的状态。
	 */
	if (abbreviate || !collate_c)
	{
		sss = palloc(sizeof(VarStringSortSupport));
		sss->buf1 = palloc(TEXTBUFLEN);
		sss->buflen1 = TEXTBUFLEN;
		sss->buf2 = palloc(TEXTBUFLEN);
		sss->buflen2 = TEXTBUFLEN;
		/* 以无效值开始 */
		sss->last_len1 = -1;
		sss->last_len2 = -1;
		/* 初始化 */
		sss->last_returned = 0;
		sss->locale = locale;

		/*
		 * 为避免把 strxfrm() 块与原始串搞混，须持续跟踪
		 * buf1 与 buf2 当前包含的数据种类。
		 *
		 *
		 * 比较可能与转换调用交错。转换与比较常常分批成
		 * 两个不同阶段，但缓存正确性无法系于此。对
		 * 比较缓存方面，只有发现 cache_blob 为 false 时
		 * 缓冲区状态才可信；而 strxfrm() 缓存只在发现
		 * cache_blob 为 true 时才信任状态。
		 *
		 *
		 * cache_blob 随意初始化为 true。
		 */
		sss->cache_blob = true;
		sss->collate_c = collate_c;
		sss->typid = typid;
		ssup->ssup_extra = sss;

		/*
		 * 若可能，计划采用缩写键优化。缩写中止时核
		 * 心代码可以换回权威比较器。
		 *
		 */
		if (abbreviate)
		{
			sss->prop_card = 0.20;
			initHyperLogLog(&sss->abbr_card, 10);
			initHyperLogLog(&sss->full_card, 10);
			ssup->abbrev_full_comparator = ssup->comparator;
			ssup->comparator = ssup_datum_unsigned_cmp;
			ssup->abbrev_converter = varstr_abbrev_convert;
			ssup->abbrev_abort = varstr_abbrev_abort;
		}
	}
}

/*
 * sortsupport 比较函数（C 区域设置）
 */
static int
varstrfastcmp_c(Datum x, Datum y, SortSupport ssup)
{
	VarString  *arg1 = DatumGetVarStringPP(x);
	VarString  *arg2 = DatumGetVarStringPP(y);
	char	   *a1p,
			   *a2p;
	int			len1,
				len2,
				result;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	result = memcmp(a1p, a2p, Min(len1, len2));
	if ((result == 0) && (len1 != len2))
		result = (len1 < len2) ? -1 : 1;

	/* 此处不能容忍内存泄漏。 */
	if (PointerGetDatum(arg1) != x)
		pfree(arg1);
	if (PointerGetDatum(arg2) != y)
		pfree(arg2);

	return result;
}

/*
 * sortsupport 比较函数（BpChar C 区域设置）
 *
 * BpChar 把它的 sortsupport 委托给本模块。针对
 * varstr_sortsupport 的 BpChar 情形仿照
 * internal_bpchar_pattern_compare()
 */
static int
bpcharfastcmp_c(Datum x, Datum y, SortSupport ssup)
{
	BpChar	   *arg1 = DatumGetBpCharPP(x);
	BpChar	   *arg2 = DatumGetBpCharPP(y);
	char	   *a1p,
			   *a2p;
	int			len1,
				len2,
				result;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = bpchartruelen(a1p, VARSIZE_ANY_EXHDR(arg1));
	len2 = bpchartruelen(a2p, VARSIZE_ANY_EXHDR(arg2));

	result = memcmp(a1p, a2p, Min(len1, len2));
	if ((result == 0) && (len1 != len2))
		result = (len1 < len2) ? -1 : 1;

	/* 此处不能容忍内存泄漏。 */
	if (PointerGetDatum(arg1) != x)
		pfree(arg1);
	if (PointerGetDatum(arg2) != y)
		pfree(arg2);

	return result;
}

/*
 * sortsupport 比较函数（NAME C 区域设置）
 */
static int
namefastcmp_c(Datum x, Datum y, SortSupport ssup)
{
	Name		arg1 = DatumGetName(x);
	Name		arg2 = DatumGetName(y);

	return strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN);
}

/*
 * sortsupport 比较函数（所有 varlena 类型的区域设置）
 */
static int
varlenafastcmp_locale(Datum x, Datum y, SortSupport ssup)
{
	VarString  *arg1 = DatumGetVarStringPP(x);
	VarString  *arg2 = DatumGetVarStringPP(y);
	char	   *a1p,
			   *a2p;
	int			len1,
				len2,
				result;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	result = varstrfastcmp_locale(a1p, len1, a2p, len2, ssup);

	/* 此处不能容忍内存泄漏。 */
	if (PointerGetDatum(arg1) != x)
		pfree(arg1);
	if (PointerGetDatum(arg2) != y)
		pfree(arg2);

	return result;
}

/*
 * sortsupport 比较函数（NAME 类型的区域设置）
 */
static int
namefastcmp_locale(Datum x, Datum y, SortSupport ssup)
{
	Name		arg1 = DatumGetName(x);
	Name		arg2 = DatumGetName(y);

	return varstrfastcmp_locale(NameStr(*arg1), strlen(NameStr(*arg1)),
								NameStr(*arg2), strlen(NameStr(*arg2)),
								ssup);
}

/*
 * 区域设置 sortsupport 比较函数
 */
static int
varstrfastcmp_locale(char *a1p, int len1, char *a2p, int len2, SortSupport ssup)
{
	VarStringSortSupport *sss = (VarStringSortSupport *) ssup->ssup_extra;
	int			result;
	bool		arg1_match;

	/* 相等性快速预检查，如 varstr_cmp() 中所述 */
	if (len1 == len2 && memcmp(a1p, a2p, len1) == 0)
	{
		/*
		 * buf1 与 buf2 内容不变，避免改动 last_len1 或
		 * last_len2。缓冲区现有内容可能仍被下次调用
		 * 使用。
		 *
		 * 这里允许比较 BpChar 填充字节没问题，尽管这意
		 * 味着 memcmp() 通常会对 BpChar 调用者执行（
		 * 尽管多字节字符仍可能阻止这一点）。memcmp() 仍
		 * 然非常便宜，而且 BpChar 的奇特语义要求我们移
		 * 除尾部空格（不限于填充），所以我们无须区分填
		 * 充空格字符与"真实"空格字符。
		 *
		 */
		return 0;
	}

	if (sss->typid == BPCHAROID)
	{
		/* 取真实字节数，忽略尾部空格 */
		len1 = bpchartruelen(a1p, len1);
		len2 = bpchartruelen(a2p, len2);
	}

	if (len1 >= sss->buflen1)
	{
		sss->buflen1 = Max(len1 + 1, Min(sss->buflen1 * 2, MaxAllocSize));
		sss->buf1 = repalloc(sss->buf1, sss->buflen1);
	}
	if (len2 >= sss->buflen2)
	{
		sss->buflen2 = Max(len2 + 1, Min(sss->buflen2 * 2, MaxAllocSize));
		sss->buf2 = repalloc(sss->buf2, sss->buflen2);
	}

	/*
	 * 我们很可能被要求反复比较相同串，而 memcmp()
	 * 比 strcoll() 便宜得多，所以值得尝试缓存比较，
	 * 尽管一般没有理由认为它会奏效（每个串 datum 都
	 * 可能是唯一的）。不奏效时缓存不会显著拖慢，而
	 * 奏效时却能大幅提速。部分原因在于 memcmp() 比
	 * 较的数据来自 L1 缓存中本就需要的缓存行，即使
	 * 上次比较的结果无法复用。
	 *
	 */
	arg1_match = true;
	if (len1 != sss->last_len1 || memcmp(sss->buf1, a1p, len1) != 0)
	{
		arg1_match = false;
		memcpy(sss->buf1, a1p, len1);
		sss->buf1[len1] = '\0';
		sss->last_len1 = len1;
	}

	/*
	 * 若与上次比较的是相同的两个串，则可以直接返回
	 * 相同的答案而无须再调 strcoll()。这比看上去更可
	 * 能发生（至少在中低基数的集合中），因为快速排
	 * 序会用同一个枢轴与许多值比较。
	 */
	if (len2 != sss->last_len2 || memcmp(sss->buf2, a2p, len2) != 0)
	{
		memcpy(sss->buf2, a2p, len2);
		sss->buf2[len2] = '\0';
		sss->last_len2 = len2;
	}
	else if (arg1_match && !sss->cache_blob)
	{
		/* 用上次实际 strcoll() 调用后缓存的结果 */
		return sss->last_returned;
	}

	result = pg_strcoll(sss->buf1, sss->buf2, sss->locale);

	/* 必要时破平局。 */
	if (result == 0 && pg_locale_deterministic(sss->locale))
		result = strcmp(sss->buf1, sss->buf2);

	/* 缓存结果，或许省掉下次昂贵的 strcoll() 调用 */
	sss->cache_blob = false;
	sss->last_returned = result;
	return result;
}

/*
 * sortsupport 的转换例程。把原始值转换为缩写键
 * 表示。我们的编码策略很简单——把 strxfrm() 块
 * 的前 8 字节打包进一个 Datum（小端机上这 8 字节
 * 以反序存储），并将其当作无符号整数。使用 "C"
 * 区域设置或 bytea 的情形，直接从原始值 memcpy()。
 */
static Datum
varstr_abbrev_convert(Datum original, SortSupport ssup)
{
	const size_t max_prefix_bytes = sizeof(Datum);
	VarStringSortSupport *sss = (VarStringSortSupport *) ssup->ssup_extra;
	VarString  *authoritative = DatumGetVarStringPP(original);
	char	   *authoritative_data = VARDATA_ANY(authoritative);

	/* 工作状态 */
	Datum		res;
	char	   *pres;
	int			len;
	uint32		hash;

	pres = (char *) &res;
	/* 已 memset()，未覆写的字节均为 NUL */
	memset(pres, 0, max_prefix_bytes);
	len = VARSIZE_ANY_EXHDR(authoritative);

	/* 取字节数，忽略尾部空格 */
	if (sss->typid == BPCHAROID)
		len = bpchartruelen(authoritative_data, len);

	/*
	 * 若使用 C 排序规则，就用 memcpy() 而非 strxfrm()
	 * 缩写键。C 区域设置的完整比较器总是 memcmp()。
	 * 允许 bytea 调用者（始终强制 C 排序规则的调用者
	 * ——bytea 不是可排序类型，但这个做法很方便）使
	 * 用 strxfrm() 是不正确的。因为 bytea 串可能含有
	 * NUL 字节。此外这样也应该更快。
	 *
	 * 更一般地说，bytea 调用者的串中含有 NUL 字节也
	 * 没有问题，因为缩写比较无须区分结尾 NUL 字节
	 * 与代表权威表示中真实 NUL 的 NUL 字节。希望在
	 * 缩写键的结尾 NUL 字节处或之后的比较能够解决
	 * 比较而无须查询权威表示；具体来说，较长串中
	 * 某个后续的非 NUL 字节可以解决较长串与较短串
	 * 中后续结尾 NUL 的比较。那里通常会有一个实际
	 * 上"按长度"的解决。
	 *
	 *
	 *
	 * 若行不通——较长串中位于较短串（首个）
	 * 结尾 NUL 偏移处或之后的所有字节其实代表权威
	 * 二进制串中的 NUL 字节（若较长串恰好仍很小，
	 * 或许在较长串末尾附近有一些*结尾* NUL 字节）
	 * ——那么将进行权威平局裁决，并正确处理：显式
	 * 考虑串长度。
	 *
	 */
	if (sss->collate_c)
		memcpy(pres, authoritative_data, Min(len, max_prefix_bytes));
	else
	{
		Size		bsize;

		/*
		 * 不使用 C 排序规则时，退回 strxfrm/ICU 类似物。
		 *
		 */

		/* 按惯例用缓冲区 1 存放并 NUL 终止 */
		if (len >= sss->buflen1)
		{
			sss->buflen1 = Max(len + 1, Min(sss->buflen1 * 2, MaxAllocSize));
			sss->buf1 = repalloc(sss->buf1, sss->buflen1);
		}

		/* 或许能复用上次的 strxfrm() 块 */
		if (sss->last_len1 == len && sss->cache_blob &&
			memcmp(sss->buf1, authoritative_data, len) == 0)
		{
			memcpy(pres, sss->buf2, Min(max_prefix_bytes, sss->last_len2));
			/* 基数无变化，无须哈希 */
			goto done;
		}

		memcpy(sss->buf1, authoritative_data, len);

		/*
		 * pg_strxfrm() 与 pg_strxfrm_prefix 期望以 NUL 结尾。
		 */
		sss->buf1[len] = '\0';
		sss->last_len1 = len;

		if (pg_strxfrm_prefix_enabled(sss->locale))
		{
			if (sss->buflen2 < max_prefix_bytes)
			{
				sss->buflen2 = Max(max_prefix_bytes,
								   Min(sss->buflen2 * 2, MaxAllocSize));
				sss->buf2 = repalloc(sss->buf2, sss->buflen2);
			}

			bsize = pg_strxfrm_prefix(sss->buf2, sss->buf1,
									  max_prefix_bytes, sss->locale);
			sss->last_len2 = bsize;
		}
		else
		{
			/*
			 * 循环：调用 pg_strxfrm()，扩缓冲区后重试。
			 * 若放不下，pg_strxfrm() 会把结果缓冲区内容
			 * 置为未定义，故即使最终只需头几字节也要
			 * 必须重试直到全部放得下。
			 *
			 */
			for (;;)
			{
				bsize = pg_strxfrm(sss->buf2, sss->buf1, sss->buflen2,
								   sss->locale);

				sss->last_len2 = bsize;
				if (bsize < sss->buflen2)
					break;

				/*
				 * 扩缓冲重试。
				 */
				sss->buflen2 = Max(bsize + 1,
								   Min(sss->buflen2 * 2, MaxAllocSize));
				sss->buf2 = repalloc(sss->buf2, sss->buflen2);
			}
		}

		/*
		 * Datum 的每个字节都会被比较。这安全，因为
		 * strxfrm() 块本身以 NUL 结尾，不会误解释任何本
		 * 不该按逻辑代表结尾的 NUL 字节。
		 *
		 *
		 * （实际上即使块中有 NUL 字节也没关系。参见上
		 * 述 bytea 情形的说明。）
		 */
		memcpy(pres, sss->buf2, Min(max_prefix_bytes, bsize));
	}

	/*
	 * 用 HyperLogLog 维护缩写键与原始权威键两者的
	 * 近似基数。用作针对最坏情形的廉价保险——最坏
	 * 情形是做了很多串变换却在完整 strcoll() 比较上
	 * 毫无节省。这些统计被 varstr_abbrev_abort() 使用。
	 *
	 *
	 * 首先，对键本体或其重要部分做哈希。混入长度以
	 * 补偿差异位于 PG_CACHE_LINE_SIZE 字节之外的
	 * 情形，以限制哈希的开销。
	 */
	hash = DatumGetUInt32(hash_any((unsigned char *) authoritative_data,
								   Min(len, PG_CACHE_LINE_SIZE)));

	if (len > PG_CACHE_LINE_SIZE)
		hash ^= DatumGetUInt32(hash_uint32((uint32) len));

	addHyperLogLog(&sss->full_card, hash);

	/* 哈希缩写键 */
#if SIZEOF_DATUM == 8
	{
		uint32		lohalf,
					hihalf;

		lohalf = (uint32) res;
		hihalf = (uint32) (res >> 32);
		hash = DatumGetUInt32(hash_uint32(lohalf ^ hihalf));
	}
#else							/* SIZEOF_DATUM != 8 */
	hash = DatumGetUInt32(hash_uint32((uint32) res));
#endif

	addHyperLogLog(&sss->abbr_card, hash);

	/* 缓存结果，或许省掉下次昂贵的 strxfrm() 调用 */
	sss->cache_blob = true;
done:

	/*
	 * 小端机上进行字节交换。
	 *
	 * 这样做是为了让 ssup_datum_unsigned_cmp()（无
	 * 符号整数三路比较器）在所有平台上都正确工作。
	 * 若不这样做，比较器就得对指向每个缩写键首字
	 * 节的指针对调用 memcmp()，那会更慢。
	 */
	res = DatumBigEndianToNative(res);

	/* 不要泄漏内存 */
	if (PointerGetDatum(authoritative) != original)
		pfree(authoritative);

	return res;
}

/*
 * 用启发式规则估计缩写键优化有效性的回调。根
 * 据其预期有效性返回指示是否应中止缩写优化的
 * 值。
 */
static bool
varstr_abbrev_abort(int memtupcount, SortSupport ssup)
{
	VarStringSortSupport *sss = (VarStringSortSupport *) ssup->ssup_extra;
	double		abbrev_distinct,
				key_distinct;

	Assert(ssup->abbreviate);

	/* 稍微有点耐心 */
	if (memtupcount < 100)
		return false;

	abbrev_distinct = estimateHyperLogLog(&sss->abbr_card);
	key_distinct = estimateHyperLogLog(&sss->full_card);

	/*
	 * 把基数估计钳制到至少一个不同值。虽然通常忽略
	 * NULL，但若到目前为止只见过 NULL 值，不钳制的
	 * 话可能会错估成本。
	 */
	if (abbrev_distinct <= 1.0)
		abbrev_distinct = 1.0;

	if (key_distinct <= 1.0)
		key_distinct = 1.0;

	/*
	 * 最坏情况下所有缩写键都相同，同时完整键串中
	 * 存在缩写未能捕捉的差异。
	 *
	 */
#ifdef TRACE_SORT
	if (trace_sort)
	{
		double		norm_abbrev_card = abbrev_distinct / (double) memtupcount;

		elog(LOG, "varstr_abbrev: abbrev_distinct after %d: %f "
			 "(key_distinct: %f, norm_abbrev_card: %f, prop_card: %f)",
			 memtupcount, abbrev_distinct, key_distinct, norm_abbrev_card,
			 sss->prop_card);
	}
#endif

	/*
	 * 若不同缩写键的数量与不同权威原始键的数量大致
	 * 相符，那就有充分的理由继续。即使基数非常低，
	 * 只要多数平局裁决只做 memcmp() 就能赢。这是
	 * 迄今最重要的考量。
	 *
	 *
	 * 虽然在缩写键层面就能解决的比较远比用 memcmp()
	 * 解决的平局裁决便宜得多，但这两种结果都比排序
	 * 开始后的完整 strcoll() 便宜太多，所以似乎不值得
	 * 为了更准确地建模成本而把缩写基数与集合的整体
	 * 大小进行权衡。假定缩写比较与
	 * 带有廉价 memcmp() 权威裁决的缩写比较是等价的。
	 *
	 *
	 */
	if (abbrev_distinct > key_distinct * sss->prop_card)
	{
		/*
		 * 当元组数超过 10000 时，为下一次调用积极衰减
		 * 所需基数。
		 *
		 * 有用：平均比较次数按线性对数速率增长，且
		 * 在大约 10000 元组时该因素将开始压过串变换的
		 * 线性成本（这是保守估计）。衰减率选得比减半
		 * 略保守——由于我们是在 memtupcount 翻倍的时点
		 * 被调用，衰减后的首次调用之后成本模型实际永
		 * 不会再现中止。该衰减率主要防备缩写基数对
		 * 完整键基数的追随突然剧烈摆动。衰减还能防止
		 * 边缘情形过晚中止——那时已在串变换上投入
		 * 过多。
		 *
		 *
		 *
		 *
		 * 几百万不同串只配几万不同缩写键的集合仍
		 * 可能获益巨大。通常只要缩写键是集合中
		 * 大致均匀数量完整键的代理就会如此。若并非如
		 * 此，我们希望能及早发现并中止。若未及早发现，
		 * 等问题明显时再中止多半已不值得。
		 *
		 *
		 */
		if (memtupcount > 10000)
			sss->prop_card *= 0.65;

		return false;
	}

	/*
	 * 中止缩写策略。
	 *
	 * 最坏情形——缩写键全同而原始串全异——通常只
	 * 会让小到中型串列表的执行时间退化约 10%。
	 * 缓存停顿为主成本的现代 CPU 上常能期待非常
	 * 大的改进，特别是对中高到高缩写基数的串集合。
	 * 失之甚少得之甚多，我们的策略正体现这点。
	 *
	 *
	 */
#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "varstr_abbrev: aborted abbreviation at %d "
			 "(abbrev_distinct: %f, key_distinct: %f, prop_card: %f)",
			 memtupcount, abbrev_distinct, key_distinct, sss->prop_card);
#endif

	return true;
}

/*
 * 字符类型算子类的通用 equalimage 支持函数。禁止
 * 在非确定性排序规则下使用去重。
 */
Datum
btvarstrequalimage(PG_FUNCTION_ARGS)
{
	/* Oid		opcintype = PG_GETARG_OID(0); */
	Oid			collid = PG_GET_COLLATION();

	check_collation_set(collid);

	if (lc_collate_is_c(collid) ||
		collid == DEFAULT_COLLATION_OID ||
		get_collation_isdeterministic(collid))
		PG_RETURN_BOOL(true);
	else
		PG_RETURN_BOOL(false);
}

Datum
text_larger(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	text	   *result;

	result = ((text_cmp(arg1, arg2, PG_GET_COLLATION()) > 0) ? arg1 : arg2);

	PG_RETURN_TEXT_P(result);
}

Datum
text_smaller(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	text	   *result;

	result = ((text_cmp(arg1, arg2, PG_GET_COLLATION()) < 0) ? arg1 : arg2);

	PG_RETURN_TEXT_P(result);
}


/*
 * text 与 name 类型的跨类型比较函数。
 */

Datum
nameeqtext(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	size_t		len1 = strlen(NameStr(*arg1));
	size_t		len2 = VARSIZE_ANY_EXHDR(arg2);
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = (len1 == len2 &&
				  memcmp(NameStr(*arg1), VARDATA_ANY(arg2), len1) == 0);
	else
		result = (varstr_cmp(NameStr(*arg1), len1,
							 VARDATA_ANY(arg2), len2,
							 collid) == 0);

	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
texteqname(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	Name		arg2 = PG_GETARG_NAME(1);
	size_t		len1 = VARSIZE_ANY_EXHDR(arg1);
	size_t		len2 = strlen(NameStr(*arg2));
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = (len1 == len2 &&
				  memcmp(VARDATA_ANY(arg1), NameStr(*arg2), len1) == 0);
	else
		result = (varstr_cmp(VARDATA_ANY(arg1), len1,
							 NameStr(*arg2), len2,
							 collid) == 0);

	PG_FREE_IF_COPY(arg1, 0);

	PG_RETURN_BOOL(result);
}

Datum
namenetext(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	size_t		len1 = strlen(NameStr(*arg1));
	size_t		len2 = VARSIZE_ANY_EXHDR(arg2);
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = !(len1 == len2 &&
				   memcmp(NameStr(*arg1), VARDATA_ANY(arg2), len1) == 0);
	else
		result = !(varstr_cmp(NameStr(*arg1), len1,
							  VARDATA_ANY(arg2), len2,
							  collid) == 0);

	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
textnename(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	Name		arg2 = PG_GETARG_NAME(1);
	size_t		len1 = VARSIZE_ANY_EXHDR(arg1);
	size_t		len2 = strlen(NameStr(*arg2));
	Oid			collid = PG_GET_COLLATION();
	bool		result;

	check_collation_set(collid);

	if (collid == C_COLLATION_OID)
		result = !(len1 == len2 &&
				   memcmp(VARDATA_ANY(arg1), NameStr(*arg2), len1) == 0);
	else
		result = !(varstr_cmp(VARDATA_ANY(arg1), len1,
							  NameStr(*arg2), len2,
							  collid) == 0);

	PG_FREE_IF_COPY(arg1, 0);

	PG_RETURN_BOOL(result);
}

Datum
btnametextcmp(PG_FUNCTION_ARGS)
{
	Name		arg1 = PG_GETARG_NAME(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int32		result;

	result = varstr_cmp(NameStr(*arg1), strlen(NameStr(*arg1)),
						VARDATA_ANY(arg2), VARSIZE_ANY_EXHDR(arg2),
						PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

Datum
bttextnamecmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	Name		arg2 = PG_GETARG_NAME(1);
	int32		result;

	result = varstr_cmp(VARDATA_ANY(arg1), VARSIZE_ANY_EXHDR(arg1),
						NameStr(*arg2), strlen(NameStr(*arg2)),
						PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);

	PG_RETURN_INT32(result);
}

#define CmpCall(cmpfunc) \
	DatumGetInt32(DirectFunctionCall2Coll(cmpfunc, \
										  PG_GET_COLLATION(), \
										  PG_GETARG_DATUM(0), \
										  PG_GETARG_DATUM(1)))

Datum
namelttext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) < 0);
}

Datum
nameletext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) <= 0);
}

Datum
namegttext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) > 0);
}

Datum
namegetext(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(btnametextcmp) >= 0);
}

Datum
textltname(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) < 0);
}

Datum
textlename(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) <= 0);
}

Datum
textgtname(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) > 0);
}

Datum
textgename(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(CmpCall(bttextnamecmp) >= 0);
}

#undef CmpCall


/*
 * 下面的算子支持对 text datum 的逐字符比较，
 * 以便构建适合 LIKE 子句的索引。注意常规的
 * texteq/textne 比较算子以及带 "C" 排序规则的
 * 常规支持函数 1 和 2 被假定与这些兼容！
 *
 */

static int
internal_text_pattern_compare(text *arg1, text *arg2)
{
	int			result;
	int			len1,
				len2;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	result = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));
	if (result != 0)
		return result;
	else if (len1 < len2)
		return -1;
	else if (len1 > len2)
		return 1;
	else
		return 0;
}


Datum
text_pattern_lt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result < 0);
}


Datum
text_pattern_le(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result <= 0);
}


Datum
text_pattern_ge(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result >= 0);
}


Datum
text_pattern_gt(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result > 0);
}


Datum
bttext_pattern_cmp(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int			result;

	result = internal_text_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}


Datum
bttext_pattern_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 使用通用串 SortSupport，强制"C"排序规则 */
	varstr_sortsupport(ssup, TEXTOID, C_COLLATION_OID);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}


/*-------------------------------------------------------------
 * byteaoctetlen
 *
 * 取 'bytea' 类型实例所含的字节数
 *-------------------------------------------------------------
 */
Datum
byteaoctetlen(PG_FUNCTION_ARGS)
{
	Datum		str = PG_GETARG_DATUM(0);

	/* 完全无需 detoast 输入 */
	PG_RETURN_INT32(toast_raw_datum_size(str) - VARHDRSZ);
}

/*
 * byteacat -
 *	  取两个 bytea* 并返回两者拼接成的 bytea*。
 *
 *
 * 从 textcat 克隆并按需修改。
 */
Datum
byteacat(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);

	PG_RETURN_BYTEA_P(bytea_catenate(t1, t2));
}

/*
 * bytea_catenate
 *	byteacat() 的核心逻辑，拆出以便其他函数使用
 *
 * 参数可为短头部形式，但不可为压缩或行外存储形式
 */
static bytea *
bytea_catenate(bytea *t1, bytea *t2)
{
	bytea	   *result;
	int			len1,
				len2,
				len;
	char	   *ptr;

	len1 = VARSIZE_ANY_EXHDR(t1);
	len2 = VARSIZE_ANY_EXHDR(t2);

	/* 偏执起见……或许该改为抛出错误？ */
	if (len1 < 0)
		len1 = 0;
	if (len2 < 0)
		len2 = 0;

	len = len1 + len2 + VARHDRSZ;
	result = (bytea *) palloc(len);

	/* 设置结果串大小…… */
	SET_VARSIZE(result, len);

	/* 填充结果串数据字段…… */
	ptr = VARDATA(result);
	if (len1 > 0)
		memcpy(ptr, VARDATA_ANY(t1), len1);
	if (len2 > 0)
		memcpy(ptr + len1, VARDATA_ANY(t2), len2);

	return result;
}

#define PG_STR_GET_BYTEA(str_) \
	DatumGetByteaPP(DirectFunctionCall1(byteain, CStringGetDatum(str_)))

/*
 * bytea_substr()
 * 返回从指定位置开始的子串。
 * 从 text_substr 克隆并按需修改。
 *
 * Input:
 *	- string
 *	- 起始位置（1 起算）
 *	- 串长（可选）
 *
 * 若起始位置为零或更小，则从串首开始返回，
 * 并按 SQL 的"负起始"约定调整长度。
 * 若长度小于零则抛出错误。若未提供第三个参数
 * （长度），则假定长度到串尾为止。
 */
Datum
bytea_substr(PG_FUNCTION_ARGS)
{
	PG_RETURN_BYTEA_P(bytea_substring(PG_GETARG_DATUM(0),
									  PG_GETARG_INT32(1),
									  PG_GETARG_INT32(2),
									  false));
}

/*
 * bytea_substr_no_len -
 *	  包装函数，避免 opr_sanity 因
 *	  同一函数接受不同数量的参数而失败
 */
Datum
bytea_substr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_BYTEA_P(bytea_substring(PG_GETARG_DATUM(0),
									  PG_GETARG_INT32(1),
									  -1,
									  true));
}

static bytea *
bytea_substring(Datum str,
				int S,
				int L,
				bool length_not_specified)
{
	int32		S1;				/* 调整后起始位置 */
	int32		L1;				/* 调整后的子串长度 */
	int32		E;				/* 结束位置 */

	/*
	 * 此处逻辑通常应与 text_substring() 一致。
	 */
	S1 = Max(S, 1);

	if (length_not_specified)
	{
		/*
		 * 未传长度——若给 DatumGetByteaPSlice() 传负长度，
		 * 它会抓取到串尾的全部内容。
		 */
		L1 = -1;
	}
	else if (L < 0)
	{
		/* SQL99 规定 E < S（即长度为负）时抛出错误 */
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
		L1 = -1;				/* 让较笨编译器安静 */
	}
	else if (pg_add_s32_overflow(S, L, &E))
	{
		/*
		 * L 可能大到使 S + L 溢出，此时子串必须延伸到
		 * 串尾。
		 */
		L1 = -1;
	}
	else
	{
		/*
		 * 结束位置出现零或负值的情形，可能发生在起始
		 * 位置为负或一时。SQL99 规定返回零长度串。
		 *
		 */
		if (E < 1)
			return PG_STR_GET_BYTEA("");

		L1 = E - S1;
	}

	/*
	 * 若起始位置超过串尾，SQL99 规定返回零长度串——
	 * DatumGetByteaPSlice() 会替我们做到。只需把
	 * S1 转为 0 起算的起始位置。
	 */
	return DatumGetByteaPSlice(str, S1 - 1, L1);
}

/*
 * byteaoverlay
 *	用第二串替换第一串中指定的子串
 *
 * SQL 标准以子串与拼接来定义 OVERLAY()。
 * 此代码是对标准所述内容的直接实现。
 */
Datum
byteaoverlay(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* 子串起始位置 */
	int			sl = PG_GETARG_INT32(3);	/* 子串长度 */

	PG_RETURN_BYTEA_P(bytea_overlay(t1, t2, sp, sl));
}

Datum
byteaoverlay_no_len(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);
	int			sp = PG_GETARG_INT32(2);	/* 子串起始位置 */
	int			sl;

	sl = VARSIZE_ANY_EXHDR(t2); /* 缺省 length(t2) */
	PG_RETURN_BYTEA_P(bytea_overlay(t1, t2, sp, sl));
}

static bytea *
bytea_overlay(bytea *t1, bytea *t2, int sp, int sl)
{
	bytea	   *result;
	bytea	   *s1;
	bytea	   *s2;
	int			sp_pl_sl;

	/*
	 * 检查可能的整数溢出情形。对负 sp，抛出"子串长度"
	 * 错误——按规范对 OVERLAY() 的定义这是应当
	 * 预期到的结果。
	 */
	if (sp <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
	if (pg_add_s32_overflow(sp, sl, &sp_pl_sl))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	s1 = bytea_substring(PointerGetDatum(t1), 1, sp - 1, false);
	s2 = bytea_substring(PointerGetDatum(t1), sp_pl_sl, -1, true);
	result = bytea_catenate(s1, t2);
	result = bytea_catenate(result, s2);

	return result;
}

/*
 * bit_count
 */
Datum
bytea_bit_count(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);

	PG_RETURN_INT64(pg_popcount(VARDATA_ANY(t1), VARSIZE_ANY_EXHDR(t1)));
}

/*
 * byteapos -
 *	  返回指定子串的位置。
 *	  实现 SQL POSITION() 函数。
 * 从 textpos 克隆并按需修改。
 */
Datum
byteapos(PG_FUNCTION_ARGS)
{
	bytea	   *t1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *t2 = PG_GETARG_BYTEA_PP(1);
	int			pos;
	int			px,
				p;
	int			len1,
				len2;
	char	   *p1,
			   *p2;

	len1 = VARSIZE_ANY_EXHDR(t1);
	len2 = VARSIZE_ANY_EXHDR(t2);

	if (len2 <= 0)
		PG_RETURN_INT32(1);		/* 空模式的结果 */

	p1 = VARDATA_ANY(t1);
	p2 = VARDATA_ANY(t2);

	pos = 0;
	px = (len1 - len2);
	for (p = 0; p <= px; p++)
	{
		if ((*p2 == *p1) && (memcmp(p1, p2, len2) == 0))
		{
			pos = p + 1;
			break;
		};
		p1++;
	};

	PG_RETURN_INT32(pos);
}

/*-------------------------------------------------------------
 * byteaGetByte
 *
 * 此例程把 "bytea" 当作字节数组对待。
 * 它返回第 N 字节（0..255 的数）。
 *-------------------------------------------------------------
 */
Datum
byteaGetByte(PG_FUNCTION_ARGS)
{
	bytea	   *v = PG_GETARG_BYTEA_PP(0);
	int32		n = PG_GETARG_INT32(1);
	int			len;
	int			byte;

	len = VARSIZE_ANY_EXHDR(v);

	if (n < 0 || n >= len)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len - 1)));

	byte = ((unsigned char *) VARDATA_ANY(v))[n];

	PG_RETURN_INT32(byte);
}

/*-------------------------------------------------------------
 * byteaGetBit
 *
 * 此例程把 "bytea" 类型当作位数组对待。
 * 它返回第 N 位的值（0 或 1）。
 *
 *-------------------------------------------------------------
 */
Datum
byteaGetBit(PG_FUNCTION_ARGS)
{
	bytea	   *v = PG_GETARG_BYTEA_PP(0);
	int64		n = PG_GETARG_INT64(1);
	int			byteNo,
				bitNo;
	int			len;
	int			byte;

	len = VARSIZE_ANY_EXHDR(v);

	if (n < 0 || n >= (int64) len * 8)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %lld out of valid range, 0..%lld",
						(long long) n, (long long) len * 8 - 1)));

	/* n/8 已知 < len，可安全转成 int */
	byteNo = (int) (n / 8);
	bitNo = (int) (n % 8);

	byte = ((unsigned char *) VARDATA_ANY(v))[byteNo];

	if (byte & (1 << bitNo))
		PG_RETURN_INT32(1);
	else
		PG_RETURN_INT32(0);
}

/*-------------------------------------------------------------
 * byteaSetByte
 *
 * 给定 'bytea' 类型实例，创建一个新实例，
 * 把第 N 字节设为给定值。
 *
 *-------------------------------------------------------------
 */
Datum
byteaSetByte(PG_FUNCTION_ARGS)
{
	bytea	   *res = PG_GETARG_BYTEA_P_COPY(0);
	int32		n = PG_GETARG_INT32(1);
	int32		newByte = PG_GETARG_INT32(2);
	int			len;

	len = VARSIZE(res) - VARHDRSZ;

	if (n < 0 || n >= len)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %d out of valid range, 0..%d",
						n, len - 1)));

	/*
	 * 设置字节。
	 */
	((unsigned char *) VARDATA(res))[n] = newByte;

	PG_RETURN_BYTEA_P(res);
}

/*-------------------------------------------------------------
 * byteaSetBit
 *
 * 给定 'bytea' 类型实例，创建一个新实例，
 * 把第 N 位设为给定值。
 *
 *-------------------------------------------------------------
 */
Datum
byteaSetBit(PG_FUNCTION_ARGS)
{
	bytea	   *res = PG_GETARG_BYTEA_P_COPY(0);
	int64		n = PG_GETARG_INT64(1);
	int32		newBit = PG_GETARG_INT32(2);
	int			len;
	int			oldByte,
				newByte;
	int			byteNo,
				bitNo;

	len = VARSIZE(res) - VARHDRSZ;

	if (n < 0 || n >= (int64) len * 8)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("index %lld out of valid range, 0..%lld",
						(long long) n, (long long) len * 8 - 1)));

	/* n/8 已知 < len，可安全转成 int */
	byteNo = (int) (n / 8);
	bitNo = (int) (n % 8);

	/*
	 * 检查!
	 */
	if (newBit != 0 && newBit != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("new bit must be 0 or 1")));

	/*
	 * 更新字节。
	 */
	oldByte = ((unsigned char *) VARDATA(res))[byteNo];

	if (newBit == 0)
		newByte = oldByte & (~(1 << bitNo));
	else
		newByte = oldByte | (1 << bitNo);

	((unsigned char *) VARDATA(res))[byteNo] = newByte;

	PG_RETURN_BYTEA_P(res);
}


/* text_name()
 * 把 text 转为 Name 类型。
 */
Datum
text_name(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_PP(0);
	Name		result;
	int			len;

	len = VARSIZE_ANY_EXHDR(s);

	/* 截断超长输入 */
	if (len >= NAMEDATALEN)
		len = pg_mbcliplen(VARDATA_ANY(s), len, NAMEDATALEN - 1);

	/* 此处用 palloc0 以确保结果被零填充 */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), VARDATA_ANY(s), len);

	PG_RETURN_NAME(result);
}

/* name_text()
 * 把 Name 转为 text 类型。
 */
Datum
name_text(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_TEXT_P(cstring_to_text(NameStr(*s)));
}


/*
 * textToQualifiedNameList - 把 text 对象转换为名字列表
 *
 * 这实现了 nextval() 及其他接受表示限定名的 text
 * 参数的函数所需的输入解析。我们在点号处拆名，
 * 非双引号包裹时转为小写，名字过长则截断。
 *
 */
List *
textToQualifiedNameList(text *textval)
{
	char	   *rawname;
	List	   *result = NIL;
	List	   *namelist;
	ListCell   *l;

	/* 转为 C 串（处理可能有的去 toast）。 */
	/* 注意我们依赖能在下方修改 rawname。 */
	rawname = text_to_cstring(textval);

	if (!SplitIdentifierString(rawname, '.', &namelist))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	if (namelist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid name syntax")));

	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);

		result = lappend(result, makeString(pstrdup(curname)));
	}

	pfree(rawname);
	list_free(namelist);

	return result;
}

/*
 * SplitIdentifierString --- 解析包含标识符的串
 *
 * 这是 textToQualifiedNameList 的核心，并导出供其他
 * 情形使用，比如解析 GUC 变量。在 GUC 的场景下，
 * 避免内存泄漏很重要，所以此 API 被设计成尽量减少
 * 需要分配和释放的东西的数量。
 *
 * Inputs:
 *	rawstring: 输入串；必须可覆写！返回时它已
 *			   被改成包含拆分出的标识符。
 *	separator: 标识符之间期望的分隔标点（通常为
 *			   "." 或 ","）。标识符周围也可以有空白。
 *			   标识符。
 * Outputs:
 *	namelist: 填入指向 rawstring 内标识符的 palloc 列表
 *			  调用者即使出错返回也应 list_free() 它。
 *
 * 串语法正确返回 true，有语法错误返回 false。
 *
 * 注意此处空串视为合法，但 textToQualifiedNameList
 * 中却不行。
 */
bool
SplitIdentifierString(char *rawstring, char separator,
					  List **namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;

	*namelist = NIL;

	while (scanner_isspace(*nextp))
		nextp++;				/* 跳过前导空白 */

	if (*nextp == '\0')
		return true;			/* 允许空串 */

	/* 在循环顶部，我们位于新标识符的开始处。 */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* 带引号名 —— 折叠引号引号对，不转小写 */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* 引号不匹配 */
				if (endp[1] != '"')
					break;		/* 到达引号名结尾 */
				/* 把相邻引号折叠成一个引号，再看一遍 */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp 现在指向结尾引号 */
			nextp = endp + 1;
		}
		else
		{
			/* 不带引号名 —— 延伸到分隔符或空白 */
			char	   *downname;
			int			len;

			curname = nextp;
			while (*nextp && *nextp != separator &&
				   !scanner_isspace(*nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* 不允许不带引号的空名 */

			/*
			 * 用与主词法器相同的代码把标识符转小写。
			 *
			 * XXX 由于我们要原地覆写输入，无法支持会把串
			 * 长度变大的小写转换。就当前 downcase_truncate_
			 * identifier 的实现这不是问题，但将来某天
			 * 可能得为此做点什么。
			 *
			 */
			len = endp - curname;
			downname = downcase_truncate_identifier(curname, len, false);
			Assert(strlen(downname) <= len);
			strncpy(curname, downname, len);	/* 此处必须 strncpy */
			pfree(downname);
		}

		while (scanner_isspace(*nextp))
			nextp++;			/* 跳过尾部空白 */

		if (*nextp == separator)
		{
			nextp++;
			while (scanner_isspace(*nextp))
				nextp++;		/* 为下一个跳过前导空白 */
			/* 期待另一个名字，done 保持 false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* 语法无效 */

		/* 现在可安全将分隔符覆写为 null */
		*endp = '\0';

		/* 若名字超长则截断 */
		truncate_identifier(curname, strlen(curname), false);

		/*
		 * 当前名字隔离完毕 —— 加入列表
		 */
		*namelist = lappend(*namelist, curname);

		/* 若未到达串尾则循环回去 */
	} while (!done);

	return true;
}


/*
 * SplitDirectoriesString --- 解析包含文件/目录名的串
 *
 * 这同样适用于文件名；函数名是历史遗留。
 *
 * 这与 SplitIdentifierString 类似，但解析规则旨在
 * 处理路径名而非标识符：不转小写，允许空白，
 * 最大长度为 MAXPGPATH-1，并对每个提取出的串应用
 * canonicalize_path()。因此，返回的串是各自独立 palloc
 * 的，而不是指向 rawstring 的指针 —— 但我们仍然会
 * 涂改 rawstring。
 *
 * Inputs:
 *	rawstring: 输入串；必须可修改！
 *	separator: 目录之间期望的分隔标点（通常为
 *			   "," 或 ";"）。目录周围也可以有空白。
 *			   目录。
 * Outputs:
 *	namelist: 填入 palloc 的目录名列表。
 *			  出错返回时也须 list_free_deep() 它。
 *
 * 串语法正确返回 true，有语法错误返回 false。
 *
 * 注意这里空串被视为合法。
 */
bool
SplitDirectoriesString(char *rawstring, char separator,
					   List **namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;

	*namelist = NIL;

	while (scanner_isspace(*nextp))
		nextp++;				/* 跳过前导空白 */

	if (*nextp == '\0')
		return true;			/* 允许空串 */

	/* 在循环顶部，我们位于新目录的开始处。 */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* 带引号名 —— 折叠引号引号对 */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* 引号不匹配 */
				if (endp[1] != '"')
					break;		/* 到达引号名结尾 */
				/* 把相邻引号折叠成一个引号，再看一遍 */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp 现在指向结尾引号 */
			nextp = endp + 1;
		}
		else
		{
			/* 不带引号名 —— 延伸到分隔符或串尾 */
			curname = endp = nextp;
			while (*nextp && *nextp != separator)
			{
				/* 尾随空白不应计入名字 */
				if (!scanner_isspace(*nextp))
					endp = nextp + 1;
				nextp++;
			}
			if (curname == endp)
				return false;	/* 不允许不带引号的空名 */
		}

		while (scanner_isspace(*nextp))
			nextp++;			/* 跳过尾部空白 */

		if (*nextp == separator)
		{
			nextp++;
			while (scanner_isspace(*nextp))
				nextp++;		/* 为下一个跳过前导空白 */
			/* 期待另一个名字，done 保持 false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* 语法无效 */

		/* 现在可安全将分隔符覆写为 null */
		*endp = '\0';

		/* 若路径超长则截断 */
		if (strlen(curname) >= MAXPGPATH)
			curname[MAXPGPATH - 1] = '\0';

		/*
		 * 当前名字隔离完毕 —— 加入列表
		 */
		curname = pstrdup(curname);
		canonicalize_path(curname);
		*namelist = lappend(*namelist, curname);

		/* 若未到达串尾则循环回去 */
	} while (!done);

	return true;
}


/*
 * SplitGUCList --- 解析包含标识符或文件名的串
 *
 * 这用于拆分 GUC_LIST_QUOTE 类 GUC 变量的值，不必预
 * 先假定元素会被当作标识符还是文件名。我们假定输入
 * 已经过 flatten_set_variable_args() 处理，因此无须
 * 再转小写（若适用则已做过）。我们也不截断，因为
 * 不知道正确的最大长度。为简单起见不允许
 * 内嵌空白（应无妨，任何内嵌空白都应已
 * 导致加上双引号）。其余 API 与
 * SplitIdentifierString 相同。
 *
 * XXX 有这么多份串拆分逻辑的副本令人恼火。然而，
 * 并不清楚带一堆选项标志的单一函数会好多少。
 *
 *
 * XXX src/bin/pg_dump/dumputils.c 中有此函数的一个
 * 版本。必须改此处时务必一并更新。
 *
 * Inputs:
 *	rawstring: 输入串；必须可覆写！返回时它已
 *			   被改成包含拆分出的标识符。
 *	separator: 标识符之间期望的分隔标点（通常为
 *			   "." 或 ","）。标识符周围也可以有空白。
 *			   标识符。
 * Outputs:
 *	namelist: 填入指向 rawstring 内标识符的 palloc 列表
 *			  调用者即使出错返回也应 list_free() 它。
 *
 * 串语法正确返回 true，有语法错误返回 false。
 */
bool
SplitGUCList(char *rawstring, char separator,
			 List **namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;

	*namelist = NIL;

	while (scanner_isspace(*nextp))
		nextp++;				/* 跳过前导空白 */

	if (*nextp == '\0')
		return true;			/* 允许空串 */

	/* 在循环顶部，我们位于新标识符的开始处。 */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* 带引号名 —— 折叠引号引号对 */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* 引号不匹配 */
				if (endp[1] != '"')
					break;		/* 到达引号名结尾 */
				/* 把相邻引号折叠成一个引号，再看一遍 */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp 现在指向结尾引号 */
			nextp = endp + 1;
		}
		else
		{
			/* 不带引号名 —— 延伸到分隔符或空白 */
			curname = nextp;
			while (*nextp && *nextp != separator &&
				   !scanner_isspace(*nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* 不允许不带引号的空名 */
		}

		while (scanner_isspace(*nextp))
			nextp++;			/* 跳过尾部空白 */

		if (*nextp == separator)
		{
			nextp++;
			while (scanner_isspace(*nextp))
				nextp++;		/* 为下一个跳过前导空白 */
			/* 期待另一个名字，done 保持 false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* 语法无效 */

		/* 现在可安全将分隔符覆写为 null */
		*endp = '\0';

		/*
		 * 当前名字隔离完毕 —— 加入列表
		 */
		*namelist = lappend(*namelist, curname);

		/* 若未到达串尾则循环回去 */
	} while (!done);

	return true;
}


/*****************************************************************************
 *	bytea 用的比较函数
 *
 * 注意：btree 索引要求这些例程不泄漏内存；因此，
 * 对 toast 化 datum 的工作副本要小心释放。大多数地方
 * 不必如此小心。
 *****************************************************************************/

Datum
byteaeq(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	bool		result;
	Size		len1,
				len2;

	/*
	 * 长度不等时可走快速路径，这或许能让我们免于
	 * 对其中一值或两值去 toast。
	 */
	len1 = toast_raw_datum_size(arg1);
	len2 = toast_raw_datum_size(arg2);
	if (len1 != len2)
		result = false;
	else
	{
		bytea	   *barg1 = DatumGetByteaPP(arg1);
		bytea	   *barg2 = DatumGetByteaPP(arg2);

		result = (memcmp(VARDATA_ANY(barg1), VARDATA_ANY(barg2),
						 len1 - VARHDRSZ) == 0);

		PG_FREE_IF_COPY(barg1, 0);
		PG_FREE_IF_COPY(barg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
byteane(PG_FUNCTION_ARGS)
{
	Datum		arg1 = PG_GETARG_DATUM(0);
	Datum		arg2 = PG_GETARG_DATUM(1);
	bool		result;
	Size		len1,
				len2;

	/*
	 * 长度不等时可走快速路径，这或许能让我们免于
	 * 对其中一值或两值去 toast。
	 */
	len1 = toast_raw_datum_size(arg1);
	len2 = toast_raw_datum_size(arg2);
	if (len1 != len2)
		result = true;
	else
	{
		bytea	   *barg1 = DatumGetByteaPP(arg1);
		bytea	   *barg2 = DatumGetByteaPP(arg2);

		result = (memcmp(VARDATA_ANY(barg1), VARDATA_ANY(barg2),
						 len1 - VARHDRSZ) != 0);

		PG_FREE_IF_COPY(barg1, 0);
		PG_FREE_IF_COPY(barg2, 1);
	}

	PG_RETURN_BOOL(result);
}

Datum
bytealt(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp < 0) || ((cmp == 0) && (len1 < len2)));
}

Datum
byteale(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp < 0) || ((cmp == 0) && (len1 <= len2)));
}

Datum
byteagt(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp > 0) || ((cmp == 0) && (len1 > len2)));
}

Datum
byteage(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL((cmp > 0) || ((cmp == 0) && (len1 >= len2)));
}

Datum
byteacmp(PG_FUNCTION_ARGS)
{
	bytea	   *arg1 = PG_GETARG_BYTEA_PP(0);
	bytea	   *arg2 = PG_GETARG_BYTEA_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	cmp = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));
	if ((cmp == 0) && (len1 != len2))
		cmp = (len1 < len2) ? -1 : 1;

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(cmp);
}

Datum
bytea_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 使用通用串 SortSupport，强制"C"排序规则 */
	varstr_sortsupport(ssup, BYTEAOID, C_COLLATION_OID);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}

/*
 * appendStringInfoText
 *
 * text 附到 str。
 * 同 appendStringInfoString(str, text_to_cstring(t))但更快。
 */
static void
appendStringInfoText(StringInfo str, const text *t)
{
	appendBinaryStringInfo(str, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
}

/*
 * replace_text
 * 把 'orig_str' 中所有 'old_sub_str' 换成
 * 'new_sub_str'，即得 'new_str'
 *
 * 若 'old_sub_str' == '' 或 'orig_str' == ''，
 * 否则返回 'new_str'
 */
Datum
replace_text(PG_FUNCTION_ARGS)
{
	text	   *src_text = PG_GETARG_TEXT_PP(0);
	text	   *from_sub_text = PG_GETARG_TEXT_PP(1);
	text	   *to_sub_text = PG_GETARG_TEXT_PP(2);
	int			src_text_len;
	int			from_sub_text_len;
	TextPositionState state;
	text	   *ret_text;
	int			chunk_len;
	char	   *curr_ptr;
	char	   *start_ptr;
	StringInfoData str;
	bool		found;

	src_text_len = VARSIZE_ANY_EXHDR(src_text);
	from_sub_text_len = VARSIZE_ANY_EXHDR(from_sub_text);

	/* 源串或模式为空时返回未修改的源串 */
	if (src_text_len < 1 || from_sub_text_len < 1)
	{
		PG_RETURN_TEXT_P(src_text);
	}

	text_position_setup(src_text, from_sub_text, PG_GET_COLLATION(), &state);

	found = text_position_next(&state);

	/* 找不到 from_sub_text 时无事可做。 */
	if (!found)
	{
		text_position_cleanup(&state);
		PG_RETURN_TEXT_P(src_text);
	}
	curr_ptr = text_position_get_match_ptr(&state);
	start_ptr = VARDATA_ANY(src_text);

	initStringInfo(&str);

	do
	{
		CHECK_FOR_INTERRUPTS();

		/* 复制上次 text_position_next() 跳过的数据 */
		chunk_len = curr_ptr - start_ptr;
		appendBinaryStringInfo(&str, start_ptr, chunk_len);

		appendStringInfoText(&str, to_sub_text);

		start_ptr = curr_ptr + from_sub_text_len;

		found = text_position_next(&state);
		if (found)
			curr_ptr = text_position_get_match_ptr(&state);
	}
	while (found);

	/* 复制尾随数据 */
	chunk_len = ((char *) src_text + VARSIZE_ANY(src_text)) - start_ptr;
	appendBinaryStringInfo(&str, start_ptr, chunk_len);

	text_position_cleanup(&state);

	ret_text = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	PG_RETURN_TEXT_P(ret_text);
}

/*
 * check_replace_text_has_escape
 *
 * 若文本不含需要处理的反斜杠则返回 0。
 * 若文本含反斜杠但不含正则子匹配指示符则返回 1。
 * 若文本含正则子匹配指示符（\1 .. \9）则返回 2。
 */
static int
check_replace_text_has_escape(const text *replace_text)
{
	int			result = 0;
	const char *p = VARDATA_ANY(replace_text);
	const char *p_end = p + VARSIZE_ANY_EXHDR(replace_text);

	while (p < p_end)
	{
		/* 找下一个转义字符 */
		p = memchr(p, '\\', p_end - p);
		if (p == NULL)
			break;
		p++;
		/* 注意：末尾的反斜杠不需要额外处理。 */
		if (p < p_end)
		{
			if (*p >= '1' && *p <= '9')
				return 2;		/* 找到子匹配指示符，完成 */
			result = 1;			/* 找到其他序列，继续找 */
			p++;
		}
	}
	return result;
}

/*
 * appendStringInfoRegexpSubstr
 *
 * 把 replace_text 附加到 str，把其中的正则反向引用
 * 替换为 \n 转义。start_ptr 是源串中匹配的起点，
 * 在逻辑字符位置 data_pos 处。
 */
static void
appendStringInfoRegexpSubstr(StringInfo str, text *replace_text,
							 regmatch_t *pmatch,
							 char *start_ptr, int data_pos)
{
	const char *p = VARDATA_ANY(replace_text);
	const char *p_end = p + VARSIZE_ANY_EXHDR(replace_text);

	while (p < p_end)
	{
		const char *chunk_start = p;
		int			so;
		int			eo;

		/* 找下一个转义字符 */
		p = memchr(p, '\\', p_end - p);
		if (p == NULL)
			p = p_end;

		/* 若有，复制刚扫描过的文本。 */
		if (p > chunk_start)
			appendBinaryStringInfo(str, chunk_start, p - chunk_start);

		/* 到串尾即完成，否则前进越过转义字符。 */
		if (p >= p_end)
			break;
		p++;

		if (p >= p_end)
		{
			/* 输入最末尾的转义。与意外字符同样处理 */
			appendStringInfoChar(str, '\\');
			break;
		}

		if (*p >= '1' && *p <= '9')
		{
			/* 使用正则的反向引用。 */
			int			idx = *p - '0';

			so = pmatch[idx].rm_so;
			eo = pmatch[idx].rm_eo;
			p++;
		}
		else if (*p == '&')
		{
			/* 使用整个匹配的串。 */
			so = pmatch[0].rm_so;
			eo = pmatch[0].rm_eo;
			p++;
		}
		else if (*p == '\\')
		{
			/* \ 表示向输出传一个 \。 */
			appendStringInfoChar(str, '\\');
			p++;
			continue;
		}
		else
		{
			/*
			 * 若转义字符后未跟任何期望的字符，就把它当作
			 * 要复制的普通数据处理。（XXX 抛错是否
			 * 好？）
			 */
			appendStringInfoChar(str, '\\');
			continue;
		}

		if (so >= 0 && eo >= 0)
		{
			/*
			 * 复制作为正则反向引用的文本。注意 so 与 eo
			 * 以字符计而非以字节计。
			 */
			char	   *chunk_start;
			int			chunk_len;

			Assert(so >= data_pos);
			chunk_start = start_ptr;
			chunk_start += charlen_to_bytelen(chunk_start, so - data_pos);
			chunk_len = charlen_to_bytelen(chunk_start, eo - so);
			appendBinaryStringInfo(str, chunk_start, chunk_len);
		}
	}
}

/*
 * replace_text_regexp
 *
 * 把 src_text 中匹配 pattern 的子串替换为 replace_text。
 * replace_text 可含反斜杠标记，用以代入
 * （部分）匹配文本。
 *
 * cflags: 正则编译标志。
 * collation：排序规则。
 * search_start：src_text 中开始搜索的字符（非字节）
 * 偏移量。
 * n：0 时替换所有匹配；>0 时只换第 N 个匹配。
 */
text *
replace_text_regexp(text *src_text, text *pattern_text,
					text *replace_text,
					int cflags, Oid collation,
					int search_start, int n)
{
	text	   *ret_text;
	regex_t    *re;
	int			src_text_len = VARSIZE_ANY_EXHDR(src_text);
	int			nmatches = 0;
	StringInfoData buf;
	regmatch_t	pmatch[10];		/* 主匹配与 \1..\9 */
	int			nmatch = lengthof(pmatch);
	pg_wchar   *data;
	size_t		data_len;
	int			data_pos;
	char	   *start_ptr;
	int			escape_status;

	initStringInfo(&buf);

	/* 把数据串转换为宽字符。 */
	data = (pg_wchar *) palloc((src_text_len + 1) * sizeof(pg_wchar));
	data_len = pg_mb2wchar_with_len(VARDATA_ANY(src_text), data, src_text_len);

	/* 检查 replace_text 是否有转义，尤其正则子匹配。 */
	escape_status = check_replace_text_has_escape(replace_text);

	/* 无正则子匹配时可用 REG_NOSUB。 */
	if (escape_status < 2)
	{
		cflags |= REG_NOSUB;
		/* 并告知 pg_regexec 只需整个匹配的位置。 */
		nmatch = 1;
	}

	/* 准备正则。 */
	re = RE_compile_and_cache(pattern_text, cflags, collation);

	/* start_ptr 指向 src_text 的第 data_pos 个字符 */
	start_ptr = (char *) VARDATA_ANY(src_text);
	data_pos = 0;

	while (search_start <= data_len)
	{
		int			regexec_result;

		CHECK_FOR_INTERRUPTS();

		regexec_result = pg_regexec(re,
									data,
									data_len,
									search_start,
									NULL,	/* 无细节 */
									nmatch,
									pmatch,
									0);

		if (regexec_result == REG_NOMATCH)
			break;

		if (regexec_result != REG_OKAY)
		{
			char		errMsg[100];

			pg_regerror(regexec_result, re, errMsg, sizeof(errMsg));
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
					 errmsg("regular expression failed: %s", errMsg)));
		}

		/*
		 * 统计匹配次数，并决定是否替换此匹配。
		 */
		nmatches++;
		if (n > 0 && nmatches != n)
		{
			/*
			 * 否，那么推进 search_start，但不推进 start_ptr/
			 * data_pos。（于是我们视被匹配文本为未匹配，
			 * 稍后再复制。）
			 */
			search_start = pmatch[0].rm_eo;
			if (pmatch[0].rm_so == pmatch[0].rm_eo)
				search_start++;
			continue;
		}

		/*
		 * 复制匹配位置左侧的文本。注意给出的索引是
		 * 字符而非字节索引。
		 */
		if (pmatch[0].rm_so - data_pos > 0)
		{
			int			chunk_len;

			chunk_len = charlen_to_bytelen(start_ptr,
										   pmatch[0].rm_so - data_pos);
			appendBinaryStringInfo(&buf, start_ptr, chunk_len);

			/*
			 * 把 start_ptr 前移越过那段文本，以免在 replace_
			 * text 含多个反向引用时重复重扫它。
			 */
			start_ptr += chunk_len;
			data_pos = pmatch[0].rm_so;
		}

		/*
		 * 复制 replace_text，若其中含转义则进行处理。
		 */
		if (escape_status > 0)
			appendStringInfoRegexpSubstr(&buf, replace_text, pmatch,
										 start_ptr, data_pos);
		else
			appendStringInfoText(&buf, replace_text);

		/* 前移 start_ptr 与 data_pos 越过匹配文本。 */
		start_ptr += charlen_to_bytelen(start_ptr,
										pmatch[0].rm_eo - data_pos);
		data_pos = pmatch[0].rm_eo;

		/*
		 * 若只想替换一次出现，就完成了。
		 */
		if (n > 0)
			break;

		/*
		 * 推进搜索位置。通常下一次搜索从上一个匹配的
		 * 结尾开始；但如果匹配长度为零，就必须前进一个
		 * 字符，否则只会再次找到同一个匹配。
		 *
		 */
		search_start = data_pos;
		if (pmatch[0].rm_so == pmatch[0].rm_eo)
			search_start++;
	}

	/*
	 * 复制上一个匹配右侧的文本。
	 */
	if (data_pos < data_len)
	{
		int			chunk_len;

		chunk_len = ((char *) src_text + VARSIZE_ANY(src_text)) - start_ptr;
		appendBinaryStringInfo(&buf, start_ptr, chunk_len);
	}

	ret_text = cstring_to_text_with_len(buf.data, buf.len);
	pfree(buf.data);
	pfree(data);

	return ret_text;
}

/*
 * split_part
 * 按给定的字段分隔符解析输入串
 * 返回第 N 项（1 起算，负数从尾计数）
 */
Datum
split_part(PG_FUNCTION_ARGS)
{
	text	   *inputstring = PG_GETARG_TEXT_PP(0);
	text	   *fldsep = PG_GETARG_TEXT_PP(1);
	int			fldnum = PG_GETARG_INT32(2);
	int			inputstring_len;
	int			fldsep_len;
	TextPositionState state;
	char	   *start_ptr;
	char	   *end_ptr;
	text	   *result_text;
	bool		found;

	/* 字段号 1 起算 */
	if (fldnum == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field position must not be zero")));

	inputstring_len = VARSIZE_ANY_EXHDR(inputstring);
	fldsep_len = VARSIZE_ANY_EXHDR(fldsep);

	/* 输入串为空时返回空串 */
	if (inputstring_len < 1)
		PG_RETURN_TEXT_P(cstring_to_text(""));

	/* 处理空字段分隔符 */
	if (fldsep_len < 1)
	{
		/* 首末字段返回输入串，其余返回空串 */
		if (fldnum == 1 || fldnum == -1)
			PG_RETURN_TEXT_P(inputstring);
		else
			PG_RETURN_TEXT_P(cstring_to_text(""));
	}

	/* 找第一个字段分隔符 */
	text_position_setup(inputstring, fldsep, PG_GET_COLLATION(), &state);

	found = text_position_next(&state);

	/* 完全找不到 fldsep 的特例 */
	if (!found)
	{
		text_position_cleanup(&state);
		/* 首末字段返回输入串，其余返回空串 */
		if (fldnum == 1 || fldnum == -1)
			PG_RETURN_TEXT_P(inputstring);
		else
			PG_RETURN_TEXT_P(cstring_to_text(""));
	}

	/*
	 * 把负字段号（即从右往左数）转为正字段号来
	 * 处理；我们需要字段总数
	 */
	if (fldnum < 0)
	{
		/* 找到了 fldsep，至少有两个字段 */
		int			numfields = 2;

		while (text_position_next(&state))
			numfields++;

		/* 最后一个字段的特例无须额外一遍扫描 */
		if (fldnum == -1)
		{
			start_ptr = text_position_get_match_ptr(&state) + fldsep_len;
			end_ptr = VARDATA_ANY(inputstring) + inputstring_len;
			text_position_cleanup(&state);
			PG_RETURN_TEXT_P(cstring_to_text_with_len(start_ptr,
													  end_ptr - start_ptr));
		}

		/* 否则把 fldnum 转为正数记法 */
		fldnum += numfields + 1;

		/* 若字段不存在，返回空串 */
		if (fldnum <= 0)
		{
			text_position_cleanup(&state);
			PG_RETURN_TEXT_P(cstring_to_text(""));
		}

		/* 重置为指向第一个匹配处，但现在 fldnum 为正 */
		text_position_reset(&state);
		found = text_position_next(&state);
		Assert(found);
	}

	/* 确定首个字段边界 */
	start_ptr = VARDATA_ANY(inputstring);
	end_ptr = text_position_get_match_ptr(&state);

	while (found && --fldnum > 0)
	{
		/* 确定下一字段边界 */
		start_ptr = end_ptr + fldsep_len;
		found = text_position_next(&state);
		if (found)
			end_ptr = text_position_get_match_ptr(&state);
	}

	text_position_cleanup(&state);

	if (fldnum > 0)
	{
		/* 第 N 个分隔符未找到 */
		/* 若请求最后字段则返回它，否则空串 */
		if (fldnum == 1)
		{
			int			last_len = start_ptr - VARDATA_ANY(inputstring);

			result_text = cstring_to_text_with_len(start_ptr,
												   inputstring_len - last_len);
		}
		else
			result_text = cstring_to_text("");
	}
	else
	{
		/* 请求非最后字段 */
		result_text = cstring_to_text_with_len(start_ptr, end_ptr - start_ptr);
	}

	PG_RETURN_TEXT_P(result_text);
}

/*
 * 当两个 text 参数相等时返回 true 的便利函数。
 */
static bool
text_isequal(text *txt1, text *txt2, Oid collid)
{
	return DatumGetBool(DirectFunctionCall2Coll(texteq,
												collid,
												PointerGetDatum(txt1),
												PointerGetDatum(txt2)));
}

/*
 * text_to_array
 * 解析输入串并返回 text 元素数组，
 * 基于给定的字段分隔符
 */
Datum
text_to_array(PG_FUNCTION_ARGS)
{
	SplitTextOutputData tstate;

	/* 对数组输出，tstate 开始时应全为零 */
	memset(&tstate, 0, sizeof(tstate));

	if (!split_text(fcinfo, &tstate))
		PG_RETURN_NULL();

	if (tstate.astate == NULL)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));

	PG_RETURN_DATUM(makeArrayResult(tstate.astate,
									CurrentMemoryContext));
}

/*
 * text_to_array_null
 * 解析输入串并返回 text 元素数组，
 * 基于给定的字段分隔符与 null 串
 *
 * 设置单独入口只是为了防止回归测试抱怨同一个内部
 * 函数有不同的参数集合。
 */
Datum
text_to_array_null(PG_FUNCTION_ARGS)
{
	return text_to_array(fcinfo);
}

/*
 * text_to_table
 * 解析输入串并返回元素组成的表，
 * 基于给定的字段分隔符
 */
Datum
text_to_table(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SplitTextOutputData tstate;

	tstate.astate = NULL;
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	tstate.tupstore = rsi->setResult;
	tstate.tupdesc = rsi->setDesc;

	(void) split_text(fcinfo, &tstate);

	return (Datum) 0;
}

/*
 * text_to_table_null
 * 解析输入串并返回元素组成的表，
 * 基于给定的字段分隔符与 null 串
 *
 * 设置单独入口只是为了防止回归测试抱怨同一个内部
 * 函数有不同的参数集合。
 */
Datum
text_to_table_null(PG_FUNCTION_ARGS)
{
	return text_to_table(fcinfo);
}

/*
 * text_to_array、text_to_array_null、text_to_table 与
 * text_to_table_null 公共代码
 *
 * 这些函数非 strict，必须显式测试 null 输入。
 * 结果应为 null 则返回 false，否则 true。
 *
 * 注意若结果合法但为空（零个元素），我们不改动
 * *tstate 就返回 —— 调用者也必须处理该情形。
 */
static bool
split_text(FunctionCallInfo fcinfo, SplitTextOutputData *tstate)
{
	text	   *inputstring;
	text	   *fldsep;
	text	   *null_string;
	Oid			collation = PG_GET_COLLATION();
	int			inputstring_len;
	int			fldsep_len;
	char	   *start_ptr;
	text	   *result_text;

	/* 输入串为 NULL 时结果也为 NULL */
	if (PG_ARGISNULL(0))
		return false;

	inputstring = PG_GETARG_TEXT_PP(0);

	/* fldsep 可为 NULL */
	if (!PG_ARGISNULL(1))
		fldsep = PG_GETARG_TEXT_PP(1);
	else
		fldsep = NULL;

	/* null_string 可为 NULL/省略 */
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		null_string = PG_GETARG_TEXT_PP(2);
	else
		null_string = NULL;

	if (fldsep != NULL)
	{
		/*
		 * 有非 null fldsep 的正常情形。用 text_position 机制
		 * 搜索 fldsep 的出现位置。
		 */
		TextPositionState state;

		inputstring_len = VARSIZE_ANY_EXHDR(inputstring);
		fldsep_len = VARSIZE_ANY_EXHDR(fldsep);

		/* 输入串为空时返回空集合 */
		if (inputstring_len < 1)
			return true;

		/* 空字段分隔符：把输入串作为单元素集合返回 */
		if (fldsep_len < 1)
		{
			split_text_accum_result(tstate, inputstring,
									null_string, collation);
			return true;
		}

		text_position_setup(inputstring, fldsep, collation, &state);

		start_ptr = VARDATA_ANY(inputstring);

		for (;;)
		{
			bool		found;
			char	   *end_ptr;
			int			chunk_len;

			CHECK_FOR_INTERRUPTS();

			found = text_position_next(&state);
			if (!found)
			{
				/* 取最后字段 */
				chunk_len = ((char *) inputstring + VARSIZE_ANY(inputstring)) - start_ptr;
				end_ptr = NULL; /* 不使用，但有些编译器抱怨 */
			}
			else
			{
				/* 取非最后字段 */
				end_ptr = text_position_get_match_ptr(&state);
				chunk_len = end_ptr - start_ptr;
			}

			/* 构建临时 text datum 传给 split_text_accum_result */
			result_text = cstring_to_text_with_len(start_ptr, chunk_len);

			/* 暂存此字段 */
			split_text_accum_result(tstate, result_text,
									null_string, collation);

			pfree(result_text);

			if (!found)
				break;

			start_ptr = end_ptr + fldsep_len;
		}

		text_position_cleanup(&state);
	}
	else
	{
		/*
		 * fldsep 为 NULL 时，输入串的每个字符都成为结果
		 * 集合中的一个元素。分隔符实际上是字符之间的
		 * 空隙。
		 */
		inputstring_len = VARSIZE_ANY_EXHDR(inputstring);

		start_ptr = VARDATA_ANY(inputstring);

		while (inputstring_len > 0)
		{
			int			chunk_len = pg_mblen(start_ptr);

			CHECK_FOR_INTERRUPTS();

			/* 构建临时 text datum 传给 split_text_accum_result */
			result_text = cstring_to_text_with_len(start_ptr, chunk_len);

			/* 暂存此字段 */
			split_text_accum_result(tstate, result_text,
									null_string, collation);

			pfree(result_text);

			start_ptr += chunk_len;
			inputstring_len -= chunk_len;
		}
	}

	return true;
}

/*
 * text 项加入结果集（表/数组）。
 *
 * 还负责检查此项是否与 null_string 匹配，若是则
 * 应改为发出 NULL。
 */
static void
split_text_accum_result(SplitTextOutputData *tstate,
						text *field_value,
						text *null_string,
						Oid collation)
{
	bool		is_null = false;

	if (null_string && text_isequal(field_value, null_string, collation))
		is_null = true;

	if (tstate->tupstore)
	{
		Datum		values[1];
		bool		nulls[1];

		values[0] = PointerGetDatum(field_value);
		nulls[0] = is_null;

		tuplestore_putvalues(tstate->tupstore,
							 tstate->tupdesc,
							 values,
							 nulls);
	}
	else
	{
		tstate->astate = accumArrayResult(tstate->astate,
										  PointerGetDatum(field_value),
										  is_null,
										  TEXTOID,
										  CurrentMemoryContext);
	}
}

/*
 * array_to_text
 * 用给定的字段分隔符拼接输入数组元素的
 * Cstring 表示
 */
Datum
array_to_text(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	char	   *fldsep = text_to_cstring(PG_GETARG_TEXT_PP(1));

	PG_RETURN_TEXT_P(array_to_text_internal(fcinfo, v, fldsep, NULL));
}

/*
 * array_to_text_null
 * 用给定的字段分隔符拼接输入数组元素的
 * Cstring 表示（另给 null 串）
 *
 * 此版本不是 strict 的，所以必须显式测试 null 输入。
 */
Datum
array_to_text_null(PG_FUNCTION_ARGS)
{
	ArrayType  *v;
	char	   *fldsep;
	char	   *null_string;

	/* 第一或第二参数为 NULL 时返回 NULL */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	v = PG_GETARG_ARRAYTYPE_P(0);
	fldsep = text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* null 串为 NULL 时按空指针透传 */
	if (!PG_ARGISNULL(2))
		null_string = text_to_cstring(PG_GETARG_TEXT_PP(2));
	else
		null_string = NULL;

	PG_RETURN_TEXT_P(array_to_text_internal(fcinfo, v, fldsep, null_string));
}

/*
 * array_to_text 与 array_to_text_null 函数的公共代码
 */
static text *
array_to_text_internal(FunctionCallInfo fcinfo, ArrayType *v,
					   const char *fldsep, const char *null_string)
{
	text	   *result;
	int			nitems,
			   *dims,
				ndims;
	Oid			element_type;
	int			typlen;
	bool		typbyval;
	char		typalign;
	StringInfoData buf;
	bool		printed = false;
	char	   *p;
	bits8	   *bitmap;
	int			bitmask;
	int			i;
	ArrayMetaState *my_extra;

	ndims = ARR_NDIM(v);
	dims = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndims, dims);

	/* 若无元素，返回空串 */
	if (nitems == 0)
		return cstring_to_text_with_len("", 0);

	element_type = ARR_ELEMTYPE(v);
	initStringInfo(&buf);

	/*
	 * 我们安排每系列调用只查找一次元素类型的信息，
	 * 包括其输出转换过程，前提是元素类型没有在我们
	 * 底下发生变化。
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
		 * 取得元素类型的信息，包括其输出转换过程
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

	p = ARR_DATA_PTR(v);
	bitmap = ARR_NULLBITMAP(v);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		char	   *value;

		/* 取源元素，检查 NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			/* 若 null_string 为 NULL，直接忽略 null 元素 */
			if (null_string != NULL)
			{
				if (printed)
					appendStringInfo(&buf, "%s%s", fldsep, null_string);
				else
					appendStringInfoString(&buf, null_string);
				printed = true;
			}
		}
		else
		{
			itemvalue = fetch_att(p, typbyval, typlen);

			value = OutputFunctionCall(&my_extra->proc, itemvalue);

			if (printed)
				appendStringInfo(&buf, "%s%s", fldsep, value);
			else
				appendStringInfoString(&buf, value);
			printed = true;

			p = att_addlength_pointer(p, typlen, p);
			p = (char *) att_align_nominal(p, typalign);
		}

		/* 若有则前移位图指针 */
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

	result = cstring_to_text_with_len(buf.data, buf.len);
	pfree(buf.data);

	return result;
}

#define HEXBASE 16
/*
 * 把 int32 转换为包含该数十六进制（base 16）表示
 * 的串。
 */
Datum
to_hex32(PG_FUNCTION_ARGS)
{
	uint32		value = (uint32) PG_GETARG_INT32(0);
	char	   *ptr;
	const char *digits = "0123456789abcdef";
	char		buf[32];		/* 比所需大，但属合理 */

	ptr = buf + sizeof(buf) - 1;
	*ptr = '\0';

	do
	{
		*--ptr = digits[value % HEXBASE];
		value /= HEXBASE;
	} while (ptr > buf && value);

	PG_RETURN_TEXT_P(cstring_to_text(ptr));
}

/*
 * 把 int64 转换为包含该数十六进制（base 16）表示
 * 的串。
 */
Datum
to_hex64(PG_FUNCTION_ARGS)
{
	uint64		value = (uint64) PG_GETARG_INT64(0);
	char	   *ptr;
	const char *digits = "0123456789abcdef";
	char		buf[32];		/* 比所需大，但属合理 */

	ptr = buf + sizeof(buf) - 1;
	*ptr = '\0';

	do
	{
		*--ptr = digits[value % HEXBASE];
		value /= HEXBASE;
	} while (ptr > buf && value);

	PG_RETURN_TEXT_P(cstring_to_text(ptr));
}

/*
 * 返回 datum 的大小，可能已压缩
 *
 * 适用任意类型
 */
Datum
pg_column_size(PG_FUNCTION_ARGS)
{
	Datum		value = PG_GETARG_DATUM(0);
	int32		result;
	int			typlen;

	/* 首次调用时取输入类型的 typlen，并保存在 *fn_extra */
	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/* 查找所提供参数的数据类型 */
		Oid			argtypeid = get_fn_expr_argtype(fcinfo->flinfo, 0);

		typlen = get_typlen(argtypeid);
		if (typlen == 0)		/* 不应发生 */
			elog(ERROR, "cache lookup failed for type %u", argtypeid);

		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(int));
		*((int *) fcinfo->flinfo->fn_extra) = typlen;
	}
	else
		typlen = *((int *) fcinfo->flinfo->fn_extra);

	if (typlen == -1)
	{
		/* varlena 型，可能已 toast */
		result = toast_datum_size(value);
	}
	else if (typlen == -2)
	{
		/* cstring */
		result = strlen(DatumGetCString(value)) + 1;
	}
	else
	{
		/* 普通定长类型 */
		result = typlen;
	}

	PG_RETURN_INT32(result);
}

/*
 * 返回压缩属性中存储的压缩方法。非 varlena 类型或
 * 未压缩数据则返回 NULL。
 */
Datum
pg_column_compression(PG_FUNCTION_ARGS)
{
	int			typlen;
	char	   *result;
	ToastCompressionId cmid;

	/* 首次调用时取输入类型的 typlen，并保存在 *fn_extra */
	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/* 查找所提供参数的数据类型 */
		Oid			argtypeid = get_fn_expr_argtype(fcinfo->flinfo, 0);

		typlen = get_typlen(argtypeid);
		if (typlen == 0)		/* 不应发生 */
			elog(ERROR, "cache lookup failed for type %u", argtypeid);

		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(int));
		*((int *) fcinfo->flinfo->fn_extra) = typlen;
	}
	else
		typlen = *((int *) fcinfo->flinfo->fn_extra);

	if (typlen != -1)
		PG_RETURN_NULL();

	/* 取压缩 varlena 中存储的压缩方法 id */
	cmid = toast_get_compression_id((struct varlena *)
									DatumGetPointer(PG_GETARG_DATUM(0)));
	if (cmid == TOAST_INVALID_COMPRESSION_ID)
		PG_RETURN_NULL();

	/* 把压缩方法 id 转换为压缩方法名 */
	switch (cmid)
	{
		case TOAST_PGLZ_COMPRESSION_ID:
			result = "pglz";
			break;
		case TOAST_LZ4_COMPRESSION_ID:
			result = "lz4";
			break;
		default:
			elog(ERROR, "invalid compression method id %d", cmid);
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * string_agg - 拼接各值并返回字符串。
 *
 * 语法: string_agg(value text, delimiter text) RETURNS text
 *
 * 注意：NULL 值会被忽略。首次调用的分隔符其实并
 * 完全未被用到；后续调用中分隔符在相关值之前。
 *
 */

/* 初始化状态的子例程 */
static StringInfo
makeStringAggState(FunctionCallInfo fcinfo)
{
	StringInfo	state;
	MemoryContext aggcontext;
	MemoryContext oldcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* 由于有 internal 类型参数，无法直接调用 */
		elog(ERROR, "string_agg_transfn called in non-aggregate context");
	}

	/*
	 * 在聚合上下文中创建状态。它会在后续调用间留在那
	 * 里。
	 */
	oldcontext = MemoryContextSwitchTo(aggcontext);
	state = makeStringInfo();
	MemoryContextSwitchTo(oldcontext);

	return state;
}

Datum
string_agg_transfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	/* 非 null 时附加该值，前置分隔符。 */
	if (!PG_ARGISNULL(1))
	{
		text	   *value = PG_GETARG_TEXT_PP(1);
		bool		isfirst = false;

		/*
		 * 你可能认为可以径直丢弃第一个分隔符，然而
		 * 必须保留它：我们可能是并行 worker，
		 * 正在做部分聚合并构建要发给主进程的状态。每个
		 * 聚合的分隔符都要保留，让 combine 函数能正确
		 * 拼接两个分别部分聚合的串。
		 * 第一个分隔符只在最终函数中被剥离。要知道应从
		 * 串首剥掉多少，我们把第一个分隔符的长度存进
		 * StringInfo 的 cursor 字段，此处无其他用途。
		 *
		 */
		if (state == NULL)
		{
			state = makeStringAggState(fcinfo);
			isfirst = true;
		}

		if (!PG_ARGISNULL(2))
		{
			text	   *delim = PG_GETARG_TEXT_PP(2);

			appendStringInfoText(state, delim);
			if (isfirst)
				state->cursor = VARSIZE_ANY_EXHDR(delim);
		}

		appendStringInfoText(state, value);
	}

	/*
	 * string_agg() 的转换类型被声明为 "internal"，
	 * 这是一个与指针同尺寸的按值传递类型。
	 */
	if (state)
		PG_RETURN_POINTER(state);
	PG_RETURN_NULL();
}

/*
 * string_agg_combine
 *		string_agg(text) 与 string_agg(bytea) 的聚合组合函数
 */
Datum
string_agg_combine(PG_FUNCTION_ARGS)
{
	StringInfo	state1;
	StringInfo	state2;
	MemoryContext agg_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state1 = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (StringInfo) PG_GETARG_POINTER(1);

	if (state2 == NULL)
	{
		/*
		 * state2 为 NULL 时很简单，直接返回 state1，它已知
		 * 位于 agg_context
		 */
		if (state1 == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(state1);
	}

	if (state1 == NULL)
	{
		/* 必须把 state2 的数据复制进 agg_context */
		MemoryContext old_context;

		old_context = MemoryContextSwitchTo(agg_context);
		state1 = makeStringAggState(fcinfo);
		appendBinaryStringInfo(state1, state2->data, state2->len);
		state1->cursor = state2->cursor;
		MemoryContextSwitchTo(old_context);
	}
	else if (state2->len > 0)
	{
		/* 组合……此情形中 state1->cursor 不变 */
		appendBinaryStringInfo(state1, state2->data, state2->len);
	}

	PG_RETURN_POINTER(state1);
}

/*
 * string_agg_serialize
 *		string_agg(text) 与 string_agg(bytea) 的聚合序列化函数
 *
 * 这是 strict 的，无须处理 NULL 输入
 */
Datum
string_agg_serialize(PG_FUNCTION_ARGS)
{
	StringInfo	state;
	StringInfoData buf;
	bytea	   *result;

	/* 由于有 internal 类型参数，无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = (StringInfo) PG_GETARG_POINTER(0);

	pq_begintypsend(&buf);

	/* 游标 */
	pq_sendint(&buf, state->cursor, 4);

	/* data */
	pq_sendbytes(&buf, state->data, state->len);

	result = pq_endtypsend(&buf);

	PG_RETURN_BYTEA_P(result);
}

/*
 * string_agg_deserialize
 *		string_agg(text) 与 string_agg(bytea) 的聚合反序列化函数
 *
 * 这是 strict 的，无须处理 NULL 输入
 */
Datum
string_agg_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	StringInfo	result;
	StringInfoData buf;
	char	   *data;
	int			datalen;

	/* 由于有 internal 类型参数，无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	sstate = PG_GETARG_BYTEA_PP(0);

	/*
	 * 把 bytea 复制进 StringInfo，以便我们能用标准的 recv
	 * 函数基础设施来"接收"它。
	 */
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf,
						   VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

	result = makeStringAggState(fcinfo);

	/* 游标 */
	result->cursor = pq_getmsgint(&buf, 4);

	/* data */
	datalen = VARSIZE_ANY_EXHDR(sstate) - 4;
	data = (char *) pq_getmsgbytes(&buf, datalen);
	appendBinaryStringInfo(result, data, datalen);

	pq_getmsgend(&buf);
	pfree(buf.data);

	PG_RETURN_POINTER(result);
}

Datum
string_agg_finalfn(PG_FUNCTION_ARGS)
{
	StringInfo	state;

	/* 由于有 internal 类型参数，无法直接调用 */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = PG_ARGISNULL(0) ? NULL : (StringInfo) PG_GETARG_POINTER(0);

	if (state != NULL)
	{
		/* 按 transfn 注释所述，剥离 cursor 位置前的数据 */
		PG_RETURN_TEXT_P(cstring_to_text_with_len(&state->data[state->cursor],
												  state->len - state->cursor));
	}
	else
		PG_RETURN_NULL();
}

/*
 * 为 concat 类函数参数的数据类型的输出函数准备
 * fmgr 信息缓存，从参数 "argidx" 开始。
 * （之前的参数在结果 FmgrInfo 数组中会有对应槽位，
 * 但我们不填这些槽位。）
 */
static FmgrInfo *
build_concat_foutcache(FunctionCallInfo fcinfo, int argidx)
{
	FmgrInfo   *foutcache;
	int			i;

	/* 我们把信息保存在 fn_mcxt 中以跨调用存活 */
	foutcache = (FmgrInfo *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												PG_NARGS() * sizeof(FmgrInfo));

	for (i = argidx; i < PG_NARGS(); i++)
	{
		Oid			valtype;
		Oid			typOutput;
		bool		typIsVarlena;

		valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		if (!OidIsValid(valtype))
			elog(ERROR, "could not determine data type of concat() input");

		getTypeOutputInfo(valtype, &typOutput, &typIsVarlena);
		fmgr_info_cxt(typOutput, &foutcache[i], fcinfo->flinfo->fn_mcxt);
	}

	fcinfo->flinfo->fn_extra = foutcache;

	return foutcache;
}

/*
 * concat() 与 concat_ws() 两者的实现。
 *
 * sepstr 是放在各值之间的分隔串。
 * argidx 标识第一个要拼接的参数（从零计数）；
 * 注意在任一调用系列中它必须是常量。
 *
 * 结果应为 NULL 时返回 NULL，否则 text 值。
 */
static text *
concat_internal(const char *sepstr, int argidx,
				FunctionCallInfo fcinfo)
{
	text	   *result;
	StringInfoData str;
	FmgrInfo   *foutcache;
	bool		first_arg = true;
	int			i;

	/*
	 * concat(VARIADIC some-array) 本质上等同于
	 * array_to_text()，即用给定分隔符拼接数组元素。
	 * 所以我们直接把此情形交给它。
	 */
	if (get_fn_expr_variadic(fcinfo->flinfo))
	{
		ArrayType  *arr;

		/* 应当只有这一个参数 */
		Assert(argidx == PG_NARGS() - 1);

		/* concat(VARIADIC NULL) 定义为 NULL */
		if (PG_ARGISNULL(argidx))
			return NULL;

		/*
		 * 非 null 参数必须是数组。我们假定任何能让
		 * get_fn_expr_variadic 返回 true 的调用上下文都已
		 * 检查过带 VARIADIC 标记的参数确实是数组。因此
		 * 直接断言它是数组而不做完整错误检查应当
		 * 可以的。
		 */
		Assert(OidIsValid(get_base_element_type(get_fn_expr_argtype(fcinfo->flinfo, argidx))));

		/* 好，可以安全取数组值了 */
		arr = PG_GETARG_ARRAYTYPE_P(argidx);

		/*
		 * 然后序列化数组。让 array_to_text 忽略 null
		 * 元素，与下方循环行为一致。
		 */
		return array_to_text_internal(fcinfo, arr, sepstr, NULL);
	}

	/* 无显式 VARIADIC 标记的正常情形 */
	initStringInfo(&str);

	/* 取输出函数信息，首次经过时构建 */
	foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
	if (foutcache == NULL)
		foutcache = build_concat_foutcache(fcinfo, argidx);

	for (i = argidx; i < PG_NARGS(); i++)
	{
		if (!PG_ARGISNULL(i))
		{
			Datum		value = PG_GETARG_DATUM(i);

			/* 若合适则加分隔符 */
			if (first_arg)
				first_arg = false;
			else
				appendStringInfoString(&str, sepstr);

			/* 调用相应类型的输出函数，附加结果 */
			appendStringInfoString(&str,
								   OutputFunctionCall(&foutcache[i], value));
		}
	}

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	return result;
}

/*
 * 拼接所有参数。NULL 参数被忽略。
 */
Datum
text_concat(PG_FUNCTION_ARGS)
{
	text	   *result;

	result = concat_internal("", 0, fcinfo);
	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

/*
 * 用分隔符拼接除第一个之外的所有参数值。第一个
 * 参数被用作分隔符。NULL 参数被忽略。
 */
Datum
text_concat_ws(PG_FUNCTION_ARGS)
{
	char	   *sep;
	text	   *result;

	/* 分隔符为 NULL 时返回 NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	sep = text_to_cstring(PG_GETARG_TEXT_PP(0));

	result = concat_internal(sep, 1, fcinfo);
	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

/*
 * 返回串中前 n 个字符。n 为负时，返回除最后
 * |n| 个字符以外的字符。
 */
Datum
text_left(PG_FUNCTION_ARGS)
{
	int			n = PG_GETARG_INT32(1);

	if (n < 0)
	{
		text	   *str = PG_GETARG_TEXT_PP(0);
		const char *p = VARDATA_ANY(str);
		int			len = VARSIZE_ANY_EXHDR(str);
		int			rlen;

		n = pg_mbstrlen_with_len(p, len) + n;
		rlen = pg_mbcharcliplen(p, len, n);
		PG_RETURN_TEXT_P(cstring_to_text_with_len(p, rlen));
	}
	else
		PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0), 1, n, false));
}

/*
 * 返回串中后 n 个字符。n 为负时，返回除最前
 * |n| 个字符以外的字符。
 */
Datum
text_right(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	const char *p = VARDATA_ANY(str);
	int			len = VARSIZE_ANY_EXHDR(str);
	int			n = PG_GETARG_INT32(1);
	int			off;

	if (n < 0)
		n = -n;
	else
		n = pg_mbstrlen_with_len(p, len) - n;
	off = pg_mbcharcliplen(p, len, n);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(p + off, len - off));
}

/*
 * 返回反转的串
 */
Datum
text_reverse(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_PP(0);
	const char *p = VARDATA_ANY(str);
	int			len = VARSIZE_ANY_EXHDR(str);
	const char *endp = p + len;
	text	   *result;
	char	   *dst;

	result = palloc(len + VARHDRSZ);
	dst = (char *) VARDATA(result) + len;
	SET_VARSIZE(result, len + VARHDRSZ);

	if (pg_database_encoding_max_length() > 1)
	{
		/* 多字节版本 */
		while (p < endp)
		{
			int			sz;

			sz = pg_mblen(p);
			dst -= sz;
			memcpy(dst, p, sz);
			p += sz;
		}
	}
	else
	{
		/* 单字节版本 */
		while (p < endp)
			*(--dst) = *p++;
	}

	PG_RETURN_TEXT_P(result);
}


/*
 * text_format() 的支持宏
 */
#define TEXT_FORMAT_FLAG_MINUS	0x0001	/* 是否有减号标志? */

#define ADVANCE_PARSE_POINTER(ptr,end_ptr) \
	do { \
		if (++(ptr) >= (end_ptr)) \
			ereport(ERROR, \
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
					 errmsg("unterminated format() type specifier"), \
					 errhint("For a single \"%%\" use \"%%%%\"."))); \
	} while (0)

/*
 * 返回格式化后的串
 */
Datum
text_format(PG_FUNCTION_ARGS)
{
	text	   *fmt;
	StringInfoData str;
	const char *cp;
	const char *start_ptr;
	const char *end_ptr;
	text	   *result;
	int			arg;
	bool		funcvariadic;
	int			nargs;
	Datum	   *elements = NULL;
	bool	   *nulls = NULL;
	Oid			element_type = InvalidOid;
	Oid			prev_type = InvalidOid;
	Oid			prev_width_type = InvalidOid;
	FmgrInfo	typoutputfinfo;
	FmgrInfo	typoutputinfo_width;

	/* 格式串为 null 时立即返回 null */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	/* 若参数标记为 VARIADIC，把数组展开为元素 */
	if (get_fn_expr_variadic(fcinfo->flinfo))
	{
		ArrayType  *arr;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			nitems;

		/* 应当只有这一个参数 */
		Assert(PG_NARGS() == 2);

		/* 若参数为 NULL，视作零长度数组 */
		if (PG_ARGISNULL(1))
			nitems = 0;
		else
		{
			/*
			 * 非 null 参数必须是数组。我们假定任何能让
			 * get_fn_expr_variadic 返回 true 的调用上下文都已
			 * 检查过带 VARIADIC 标记的参数确实是数组。因此
			 * 直接断言它是数组而不做完整错误检查应当
			 * 可以的。
			 */
			Assert(OidIsValid(get_base_element_type(get_fn_expr_argtype(fcinfo->flinfo, 1))));

			/* 好，可以安全取数组值了 */
			arr = PG_GETARG_ARRAYTYPE_P(1);

			/* 取数组元素类型的信息 */
			element_type = ARR_ELEMTYPE(arr);
			get_typlenbyvalalign(element_type,
								 &elmlen, &elmbyval, &elmalign);

			/* 提取所有数组元素 */
			deconstruct_array(arr, element_type, elmlen, elmbyval, elmalign,
							  &elements, &nulls, &nitems);
		}

		nargs = nitems + 1;
		funcvariadic = true;
	}
	else
	{
		/* 非 VARIADIC 情形：我们将逐个处理参数 */
		nargs = PG_NARGS();
		funcvariadic = false;
	}

	/* 主循环准备。 */
	fmt = PG_GETARG_TEXT_PP(0);
	start_ptr = VARDATA_ANY(fmt);
	end_ptr = start_ptr + VARSIZE_ANY_EXHDR(fmt);
	initStringInfo(&str);
	arg = 1;					/* 下个要打印的参数位置 */

	/* 扫描格式串，寻找转换指示符。 */
	for (cp = start_ptr; cp < end_ptr; cp++)
	{
		int			argpos;
		int			widthpos;
		int			flags;
		int			width;
		Datum		value;
		bool		isNull;
		Oid			typid;

		/*
		 * 若不是转换指示符的开头，直接把它复制到输出
		 * 缓冲区。
		 */
		if (*cp != '%')
		{
			appendStringInfoCharMacro(&str, *cp);
			continue;
		}

		ADVANCE_PARSE_POINTER(cp, end_ptr);

		/* 简单情形：%% 输出单个 % */
		if (*cp == '%')
		{
			appendStringInfoCharMacro(&str, *cp);
			continue;
		}

		/* 解析格式指示符的可选部分 */
		cp = text_format_parse_format(cp, end_ptr,
									  &argpos, &widthpos,
									  &flags, &width);

		/*
		 * 接下来应看到主转换指示符。无论参数位置是否
		 * 出现过，此刻串中至少还剩一个字符。经验
		 * 表明值得先检查该字符是否为预期字符之一，再
		 * 去取参数，以便对格式错误的指示符给出最不令
		 * 人困惑的响应。
		 *
		 */
		if (strchr("sIL", *cp) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized format() type specifier \"%.*s\"",
							pg_mblen(cp), cp),
					 errhint("For a single \"%%\" use \"%%%%\".")));

		/* 若指定了间接宽度，取其值 */
		if (widthpos >= 0)
		{
			/* 收集指定或下一个参数位置 */
			if (widthpos > 0)
				arg = widthpos;
			if (arg >= nargs)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("too few arguments for format()")));

			/* 取所选参数的值和类型 */
			if (!funcvariadic)
			{
				value = PG_GETARG_DATUM(arg);
				isNull = PG_ARGISNULL(arg);
				typid = get_fn_expr_argtype(fcinfo->flinfo, arg);
			}
			else
			{
				value = elements[arg - 1];
				isNull = nulls[arg - 1];
				typid = element_type;
			}
			if (!OidIsValid(typid))
				elog(ERROR, "could not determine data type of format() input");

			arg++;

			/* NULL 宽度可当作零处理 */
			if (isNull)
				width = 0;
			else if (typid == INT4OID)
				width = DatumGetInt32(value);
			else if (typid == INT2OID)
				width = DatumGetInt16(value);
			else
			{
				/* 较不常用类型：先转 text 再转 int */
				char	   *str;

				if (typid != prev_width_type)
				{
					Oid			typoutputfunc;
					bool		typIsVarlena;

					getTypeOutputInfo(typid, &typoutputfunc, &typIsVarlena);
					fmgr_info(typoutputfunc, &typoutputinfo_width);
					prev_width_type = typid;
				}

				str = OutputFunctionCall(&typoutputinfo_width, value);

				/* 数据坏或溢出时 pg_strtoint32 会抱怨 */
				width = pg_strtoint32(str);

				pfree(str);
			}
		}

		/* 收集指定或下一个参数位置 */
		if (argpos > 0)
			arg = argpos;
		if (arg >= nargs)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("too few arguments for format()")));

		/* 取所选参数的值和类型 */
		if (!funcvariadic)
		{
			value = PG_GETARG_DATUM(arg);
			isNull = PG_ARGISNULL(arg);
			typid = get_fn_expr_argtype(fcinfo->flinfo, arg);
		}
		else
		{
			value = elements[arg - 1];
			isNull = nulls[arg - 1];
			typid = element_type;
		}
		if (!OidIsValid(typid))
			elog(ERROR, "could not determine data type of format() input");

		arg++;

		/*
		 * 取 typOutput 函数，与前一个参数同类型时
		 * 则重用上一个。这在变长数组情形下特别有用，但
		 * 对普通调用也常能省事。
		 */
		if (typid != prev_type)
		{
			Oid			typoutputfunc;
			bool		typIsVarlena;

			getTypeOutputInfo(typid, &typoutputfunc, &typIsVarlena);
			fmgr_info(typoutputfunc, &typoutputfinfo);
			prev_type = typid;
		}

		/*
		 * 现在可格式化该值了。
		 */
		switch (*cp)
		{
			case 's':
			case 'I':
			case 'L':
				text_format_string_conversion(&str, *cp, &typoutputfinfo,
											  value, isNull,
											  flags, width);
				break;
			default:
				/* 不应到达此处：前面已检查过 */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized format() type specifier \"%.*s\"",
								pg_mblen(cp), cp),
						 errhint("For a single \"%%\" use \"%%%%\".")));
				break;
		}
	}

	/* 不再需要 deconstruct_array 的结果。 */
	if (elements != NULL)
		pfree(elements);
	if (nulls != NULL)
		pfree(nulls);

	/* 生成结果。 */
	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	PG_RETURN_TEXT_P(result);
}

/*
 * 把连续数字解析为十进制数。
 *
 * 若能解析出一些数字则返回 true。
 * 值返回到 *value 中，*ptr 前进到下一个要解析的
 * 字符处。
 *
 * 注意解析不变式：入口处已知串尾（end_ptr）之前
 * 至少有一个字符可用，出口处仍然如此。
 */
static bool
text_format_parse_digits(const char **ptr, const char *end_ptr, int *value)
{
	bool		found = false;
	const char *cp = *ptr;
	int			val = 0;

	while (*cp >= '0' && *cp <= '9')
	{
		int8		digit = (*cp - '0');

		if (unlikely(pg_mul_s32_overflow(val, 10, &val)) ||
			unlikely(pg_add_s32_overflow(val, digit, &val)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("number is out of range")));
		ADVANCE_PARSE_POINTER(cp, end_ptr);
		found = true;
	}

	*ptr = cp;
	*value = val;

	return found;
}

/*
 * 解析格式指示符（大体遵循 SUS printf 规范）。
 *
 * 我们已经越过开头的 '%'，正在寻找
 * [argpos][flags][width]type（但类型字符不在此处消费）。
 *
 * 输入为 start_ptr（'%' 之后）与 end_ptr（串尾+1）。
 * 输出参数：
 *	argpos：要打印的值的参数位置。-1 表示未指定。
 *	widthpos：宽度的参数位置。零表示参数位置
 *			未指定（即取下一个参数），-1 表示没有宽度
 *			参数（宽度被省略或指定为常量）。
 *	flags: 标志位掩码
 *	width：直接指定的宽度值。零表示宽度被省略
 *			（无须把这个情形与显式的零宽度值区分开）。
 *
 *
 * 函数结果是要解析的下一个字符位置，即类型字符
 * 所在/应在的位置。
 *
 * 注意解析不变式：入口处已知串尾（end_ptr）之前
 * 至少有一个字符可用，出口处仍然如此。
 */
static const char *
text_format_parse_format(const char *start_ptr, const char *end_ptr,
						 int *argpos, int *widthpos,
						 int *flags, int *width)
{
	const char *cp = start_ptr;
	int			n;

	/* 为输出参数设置默认值 */
	*argpos = -1;
	*widthpos = -1;
	*flags = 0;
	*width = 0;

	/* 尝试识别第一个数字 */
	if (text_format_parse_digits(&cp, end_ptr, &n))
	{
		if (*cp != '$')
		{
			/* 必然只是宽度和类型，于是完成 */
			*width = n;
			return cp;
		}
		/* 该数字是参数位置 */
		*argpos = n;
		/* 参数索引显式为 0 立即拒绝 */
		if (n == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("format specifies argument 0, but arguments are numbered from 1")));
		ADVANCE_PARSE_POINTER(cp, end_ptr);
	}

	/* 处理标志（现在只支持减号） */
	while (*cp == '-')
	{
		*flags |= TEXT_FORMAT_FLAG_MINUS;
		ADVANCE_PARSE_POINTER(cp, end_ptr);
	}

	if (*cp == '*')
	{
		/* 处理间接宽度 */
		ADVANCE_PARSE_POINTER(cp, end_ptr);
		if (text_format_parse_digits(&cp, end_ptr, &n))
		{
			/* 此位置的数字必须以 $ 结尾 */
			if (*cp != '$')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("width argument position must be ended by \"$\"")));
			/* 该数字是宽度的参数位置 */
			*widthpos = n;
			/* 参数索引显式为 0 立即拒绝 */
			if (n == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("format specifies argument 0, but arguments are numbered from 1")));
			ADVANCE_PARSE_POINTER(cp, end_ptr);
		}
		else
			*widthpos = 0;		/* 宽度的参数位置未指定 */
	}
	else
	{
		/* 检查直接宽度指定 */
		if (text_format_parse_digits(&cp, end_ptr, &n))
			*width = n;
	}

	/* cp 现在应指向类型字符 */
	return cp;
}

/*
 * 格式化 %s、%I 或 %L 转换
 */
static void
text_format_string_conversion(StringInfo buf, char conversion,
							  FmgrInfo *typOutputInfo,
							  Datum value, bool isNull,
							  int flags, int width)
{
	char	   *str;

	/* 在尝试把值串化之前先处理 NULL 参数。 */
	if (isNull)
	{
		if (conversion == 's')
			text_format_append_string(buf, "", flags, width);
		else if (conversion == 'L')
			text_format_append_string(buf, "NULL", flags, width);
		else if (conversion == 'I')
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null values cannot be formatted as an SQL identifier")));
		return;
	}

	/* 串化。 */
	str = OutputFunctionCall(typOutputInfo, value);

	/* 转义 */
	if (conversion == 'I')
	{
		/* quote_identifier 或分配或不分配新串。 */
		text_format_append_string(buf, quote_identifier(str), flags, width);
	}
	else if (conversion == 'L')
	{
		char	   *qstr = quote_literal_cstr(str);

		text_format_append_string(buf, qstr, flags, width);
		/* quote_literal_cstr() 总是分配新串 */
		pfree(qstr);
	}
	else
		text_format_append_string(buf, str, flags, width);

	/* 清理 */
	pfree(str);
}

/*
 * 把 str 附加到 buf，按 flags/width 指示填充
 */
static void
text_format_append_string(StringInfo buf, const char *str,
						  int flags, int width)
{
	bool		align_to_left = false;
	int			len;

	/* 常见易例的快路径 */
	if (width == 0)
	{
		appendStringInfoString(buf, str);
		return;
	}

	if (width < 0)
	{
		/* 负宽度：隐含 '-' 标志，然后取绝对值 */
		align_to_left = true;
		/* -INT_MIN 未定义 */
		if (width <= INT_MIN)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("number is out of range")));
		width = -width;
	}
	else if (flags & TEXT_FORMAT_FLAG_MINUS)
		align_to_left = true;

	len = pg_mbstrlen(str);
	if (align_to_left)
	{
		/* 左对齐 */
		appendStringInfoString(buf, str);
		if (len < width)
			appendStringInfoSpaces(buf, width - len);
	}
	else
	{
		/* 右对齐 */
		if (len < width)
			appendStringInfoSpaces(buf, width - len);
		appendStringInfoString(buf, str);
	}
}

/*
 * text_format_nv - text_format 函数的非变长包装。
 *
 * 注意：opr_sanity 的健全性检查要求共享同一实现 C
 * 函数的内建函数接受相同数量的参数，所以需要此
 * 包装。
 */
Datum
text_format_nv(PG_FUNCTION_ARGS)
{
	return text_format(fcinfo);
}

/*
 * Levenshtein 距离函数的辅助函数。在此用例中比
 * memcmp() 更快。
 */
static inline bool
rest_of_char_same(const char *s1, const char *s2, int len)
{
	while (len > 0)
	{
		len--;
		if (s1[len] != s2[len])
			return false;
	}
	return true;
}

/* 展开每个 Levenshtein 距离变体 */
#include "levenshtein.c"
#define LEVENSHTEIN_LESS_EQUAL
#include "levenshtein.c"


/*
 * 下面的 *ClosestMatch() 函数可用来判断用户提供的串
 * 是否与任何已知合法值相似，这在为日志消息提供
 * 提示等方面很有用。这些函数的用法如下：
 *
 *
 *		initClosestMatch(&state, source_string, max_distance);
 *
 *		for (int i = 0; i < num_valid_strings; i++)
 *			updateClosestMatch(&state, valid_strings[i]);
 *
 *		closestMatch = getClosestMatch(&state);
 */

/*
 * 用源串和要考虑的最大 Levenshtein 距离初始化给定
 * 的状态。
 */
void
initClosestMatch(ClosestMatchState *state, const char *source, int max_d)
{
	Assert(state);
	Assert(max_d >= 0);

	state->source = source;
	state->min_d = -1;
	state->max_d = max_d;
	state->match = NULL;
}

/*
 * 若候选串比当前保存的匹配更近（或尚无保存的匹
 * 配），则将其保存为最接近的匹配。
 *
 * 若源串或候选串为 NULL、为空或过长，此函数不做
 * 任何动作。同样，若 Levenshtein 距离超过允许的
 * 最大值，或超过半数字符不同，则也不采取动作。
 */
void
updateClosestMatch(ClosestMatchState *state, const char *candidate)
{
	int			dist;

	Assert(state);

	if (state->source == NULL || state->source[0] == '\0' ||
		candidate == NULL || candidate[0] == '\0')
		return;

	/*
	 * 为避免报 ERROR，我们在这里检查长度，而不是在
	 * 调用 varstr_levenshtein_less_equal() 时设 false。
	 */
	if (strlen(state->source) > MAX_LEVENSHTEIN_STRLEN ||
		strlen(candidate) > MAX_LEVENSHTEIN_STRLEN)
		return;

	dist = varstr_levenshtein_less_equal(state->source, strlen(state->source),
										 candidate, strlen(candidate), 1, 1, 1,
										 state->max_d, true);
	if (dist <= state->max_d &&
		dist <= strlen(state->source) / 2 &&
		(state->min_d == -1 || dist < state->min_d))
	{
		state->min_d = dist;
		state->match = candidate;
	}
}

/*
 * 返回最接近的匹配。若未通过 updateClosestMatch()
 * 提供候选，则返回 NULL。
 */
const char *
getClosestMatch(ClosestMatchState *state)
{
	Assert(state);

	return state->match;
}


/*
 * Unicode 支持
 */

static UnicodeNormalizationForm
unicode_norm_form_from_string(const char *formstr)
{
	UnicodeNormalizationForm form = -1;

	/*
	 * 既然在此，不妨顺便检查。
	 */
	if (GetDatabaseEncoding() != PG_UTF8)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("Unicode normalization can only be performed if server encoding is UTF8")));

	if (pg_strcasecmp(formstr, "NFC") == 0)
		form = UNICODE_NFC;
	else if (pg_strcasecmp(formstr, "NFD") == 0)
		form = UNICODE_NFD;
	else if (pg_strcasecmp(formstr, "NFKC") == 0)
		form = UNICODE_NFKC;
	else if (pg_strcasecmp(formstr, "NFKD") == 0)
		form = UNICODE_NFKD;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid normalization form: %s", formstr)));

	return form;
}

Datum
unicode_normalize_func(PG_FUNCTION_ARGS)
{
	text	   *input = PG_GETARG_TEXT_PP(0);
	char	   *formstr = text_to_cstring(PG_GETARG_TEXT_PP(1));
	UnicodeNormalizationForm form;
	int			size;
	pg_wchar   *input_chars;
	pg_wchar   *output_chars;
	unsigned char *p;
	text	   *result;
	int			i;

	form = unicode_norm_form_from_string(formstr);

	/* 转换为 pg_wchar */
	size = pg_mbstrlen_with_len(VARDATA_ANY(input), VARSIZE_ANY_EXHDR(input));
	input_chars = palloc((size + 1) * sizeof(pg_wchar));
	p = (unsigned char *) VARDATA_ANY(input);
	for (i = 0; i < size; i++)
	{
		input_chars[i] = utf8_to_unicode(p);
		p += pg_utf_mblen(p);
	}
	input_chars[i] = (pg_wchar) '\0';
	Assert((char *) p == VARDATA_ANY(input) + VARSIZE_ANY_EXHDR(input));

	/* 动作 */
	output_chars = unicode_normalize(form, input_chars);

	/* 转换回 UTF-8 串 */
	size = 0;
	for (pg_wchar *wp = output_chars; *wp; wp++)
	{
		unsigned char buf[4];

		unicode_to_utf8(*wp, buf);
		size += pg_utf_mblen(buf);
	}

	result = palloc(size + VARHDRSZ);
	SET_VARSIZE(result, size + VARHDRSZ);

	p = (unsigned char *) VARDATA_ANY(result);
	for (pg_wchar *wp = output_chars; *wp; wp++)
	{
		unicode_to_utf8(*wp, p);
		p += pg_utf_mblen(p);
	}
	Assert((char *) p == (char *) result + size + VARHDRSZ);

	PG_RETURN_TEXT_P(result);
}

/*
 * 检查串是否处于指定的 Unicode 规范化形式。
 *
 * 做法是把串转换为指定的规范形式，然后与原串比
 * 较。为提速，我们还应用 UAX #15 规定的"快速检查"
 * 算法，它只扫一遍串就能对许多串给出是或否的
 * 答案。
 *
 * 此函数通常应针对串确实已规范化的情形优化。在
 * 那种情况下我们将查看整个串，所以进行任何增量
 * 转换等操作可能都不值得。
 */
Datum
unicode_is_normalized(PG_FUNCTION_ARGS)
{
	text	   *input = PG_GETARG_TEXT_PP(0);
	char	   *formstr = text_to_cstring(PG_GETARG_TEXT_PP(1));
	UnicodeNormalizationForm form;
	int			size;
	pg_wchar   *input_chars;
	pg_wchar   *output_chars;
	unsigned char *p;
	int			i;
	UnicodeNormalizationQC quickcheck;
	int			output_size;
	bool		result;

	form = unicode_norm_form_from_string(formstr);

	/* 转换为 pg_wchar */
	size = pg_mbstrlen_with_len(VARDATA_ANY(input), VARSIZE_ANY_EXHDR(input));
	input_chars = palloc((size + 1) * sizeof(pg_wchar));
	p = (unsigned char *) VARDATA_ANY(input);
	for (i = 0; i < size; i++)
	{
		input_chars[i] = utf8_to_unicode(p);
		p += pg_utf_mblen(p);
	}
	input_chars[i] = (pg_wchar) '\0';
	Assert((char *) p == VARDATA_ANY(input) + VARSIZE_ANY_EXHDR(input));

	/* 快速检查（UAX #15） */
	quickcheck = unicode_is_normalized_quickcheck(form, input_chars);
	if (quickcheck == UNICODE_NORM_QC_YES)
		PG_RETURN_BOOL(true);
	else if (quickcheck == UNICODE_NORM_QC_NO)
		PG_RETURN_BOOL(false);

	/* 规范化并与原串比较 */
	output_chars = unicode_normalize(form, input_chars);

	output_size = 0;
	for (pg_wchar *wp = output_chars; *wp; wp++)
		output_size++;

	result = (size == output_size) &&
		(memcmp(input_chars, output_chars, size * sizeof(pg_wchar)) == 0);

	PG_RETURN_BOOL(result);
}

/*
 * 检查前 n 字符是否十六进制数
 */
static bool
isxdigits_n(const char *instr, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!isxdigit((unsigned char) instr[i]))
			return false;

	return true;
}

static unsigned int
hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 0xA;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 0xA;
	elog(ERROR, "invalid hexadecimal digit");
	return 0;					/* 不可达 */
}

/*
 * 把含十六进制数字的串转换为数字
 */
static unsigned int
hexval_n(const char *instr, size_t n)
{
	unsigned int result = 0;

	for (size_t i = 0; i < n; i++)
		result += hexval(instr[i]) << (4 * (n - i - 1));

	return result;
}

/*
 * 把 Unicode 转义序列替换为 Unicode 字符
 */
Datum
unistr(PG_FUNCTION_ARGS)
{
	text	   *input_text = PG_GETARG_TEXT_PP(0);
	char	   *instr;
	int			len;
	StringInfoData str;
	text	   *result;
	pg_wchar	pair_first = 0;
	char		cbuf[MAX_UNICODE_EQUIVALENT_STRING + 1];

	instr = VARDATA_ANY(input_text);
	len = VARSIZE_ANY_EXHDR(input_text);

	initStringInfo(&str);

	while (len > 0)
	{
		if (instr[0] == '\\')
		{
			if (len >= 2 &&
				instr[1] == '\\')
			{
				if (pair_first)
					goto invalid_pair;
				appendStringInfoChar(&str, '\\');
				instr += 2;
				len -= 2;
			}
			else if ((len >= 5 && isxdigits_n(instr + 1, 4)) ||
					 (len >= 6 && instr[1] == 'u' && isxdigits_n(instr + 2, 4)))
			{
				pg_wchar	unicode;
				int			offset = instr[1] == 'u' ? 2 : 1;

				unicode = hexval_n(instr + offset, 4);

				if (!is_valid_unicode_codepoint(unicode))
					ereport(ERROR,
							errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("invalid Unicode code point: %04X", unicode));

				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) cbuf);
					appendStringInfoString(&str, cbuf);
				}

				instr += 4 + offset;
				len -= 4 + offset;
			}
			else if (len >= 8 && instr[1] == '+' && isxdigits_n(instr + 2, 6))
			{
				pg_wchar	unicode;

				unicode = hexval_n(instr + 2, 6);

				if (!is_valid_unicode_codepoint(unicode))
					ereport(ERROR,
							errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("invalid Unicode code point: %04X", unicode));

				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) cbuf);
					appendStringInfoString(&str, cbuf);
				}

				instr += 8;
				len -= 8;
			}
			else if (len >= 10 && instr[1] == 'U' && isxdigits_n(instr + 2, 8))
			{
				pg_wchar	unicode;

				unicode = hexval_n(instr + 2, 8);

				if (!is_valid_unicode_codepoint(unicode))
					ereport(ERROR,
							errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("invalid Unicode code point: %04X", unicode));

				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) cbuf);
					appendStringInfoString(&str, cbuf);
				}

				instr += 10;
				len -= 10;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid Unicode escape"),
						 errhint("Unicode escapes must be \\XXXX, \\+XXXXXX, \\uXXXX, or \\UXXXXXXXX.")));
		}
		else
		{
			if (pair_first)
				goto invalid_pair;

			appendStringInfoChar(&str, *instr++);
			len--;
		}
	}

	/* 未完成的代理对? */
	if (pair_first)
		goto invalid_pair;

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	PG_RETURN_TEXT_P(result);

invalid_pair:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid Unicode surrogate pair")));
	PG_RETURN_NULL();			/* 让编译器安静 */
}
