/*-------------------------------------------------------------------------
 *
 * numeric.c
 *	Postgres 数据库系统的精确数值数据类型
 *
 * 1998 年 Jan Wieck 首创，2003 年 Tom Lane 大幅修订。
 *
 * 许多算法思想都借鉴自 David M. Smith 的"FM"多精度
 * 数学库，该库最近发表为 Algorithm 786: Multiple-Precision
 *  Complex Arithmetic and Functions, ACM Transactions on
 * Mathematical Software, Vol. 24, No. 4, 1998 年 12 月，
 * 359-367 页。
 *
 * Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/numeric.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "funcapi.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"
#include "utils/sortsupport.h"

/* ----------
 * 取消下面的注释，即可编译 dump_numeric()
 * 与 dump_var()，并转储 make_result() 产生的任何结果。
 * ----------
#define NUMERIC_DEBUG
 */


/* ----------
 * 局部类型
 *
 * 数值以基数为 NBASE 的浮点格式表示。
 * 每个"数字位"取值范围从 0 到 NBASE-1。NumericDigit 类型
 * 为有符号且足以存放一个数字位。假定 NBASE*NBASE 可
 * 容纳于一个 int 中。虽然纯计算例程本可处理任何小于
 * sqrt(INT_MAX) 的偶数 NBASE，但实际我们只关心 NBASE
 * 为 10 的幂，这样 I/O 转换与十进制四舍五入都简单。
 * 另外，NBASE 若明显小于 sqrt(INT_MAX)，实际上也更高效，
 * 这样 mul_var 与 div_var_fast 就有"余量"可以推迟
 * 进位的处理。
 *
 * 除 10000 以外的 NBASE 取值仅具有历史意义，如今在
 * 任何意义上都不再受支持；客户端没有任何机制能探知
 * 基数，因此支持二进制模式的客户端都预期 base-10000
 * 格式。若打算改变此点，也要注意 numeric 缩略代码，
 * 其假定 NBASE=10000。
 * ----------
 */

#if 0
#define NBASE		10
#define HALF_NBASE	5
#define DEC_DIGITS	1			/* 每 NBASE 位十进制位数 */
#define MUL_GUARD_DIGITS	4	/* 以 NBASE 数字位为单位计量 */
#define DIV_GUARD_DIGITS	8

typedef signed char NumericDigit;
#endif

#if 0
#define NBASE		100
#define HALF_NBASE	50
#define DEC_DIGITS	2			/* 每 NBASE 位十进制位数 */
#define MUL_GUARD_DIGITS	3	/* 以 NBASE 数字位为单位计量 */
#define DIV_GUARD_DIGITS	6

typedef signed char NumericDigit;
#endif

#if 1
#define NBASE		10000
#define HALF_NBASE	5000
#define DEC_DIGITS	4			/* 每 NBASE 位十进制位数 */
#define MUL_GUARD_DIGITS	2	/* 以 NBASE 数字位为单位计量 */
#define DIV_GUARD_DIGITS	4

typedef int16 NumericDigit;
#endif

/*
 * Numeric 的磁盘存储形式。
 *
 * 若 NumericChoice 首字的高位（n_header，或
 * n_short.n_header，或 n_long.n_sign_dscale）为 NUMERIC_SHORT，
 * 则该数值遵循 NumericShort 格式；若为 NUMERIC_POS 或
 * NUMERIC_NEG，则遵循 NumericLong 格式。若为 NUMERIC_SPECIAL，
 * 则值为 NaN 或无穷大。目前我们总是只用两个字节存储
 * SPECIAL 值（即仅 n_header），但旧版本只使用 NumericLong
 * 格式，因此若数据库经 pg_upgrade 迁移，磁盘上可能仍有
 * 4 字节的 NaN（但不会有无穷大）。无论哪种情形，
 * 特殊值头部的低位是保留位，目前
 * 应始终将其置为零。
 *
 * 在 NumericShort 格式下，头部字（n_short.n_header）
 * 其余 14 位分配如下：1 位用于符号（正或负），6 位
 * 用于动态标度，7 位用于权重。实践中多数常见值
 * 都能以这种方式表示。
 *
 * 在 NumericLong 格式下，头部字
 * （n_long.n_sign_dscale）的其余 14 位表示显示标度；而
 * 权重存于 n_weight。
 *
 * 注意：惯例上打包形式的值已剥除全部前导与尾随
 * 零数字位（此处"数字位"即以 NBASE 为基）。
 * 特别地，若值为零，将完全不含任何数字位！
 * 此时权重可为任意值，但我们通常将其置为零。
 */

struct NumericShort
{
	uint16		n_header;		/* 符号+显示标度+权重 */
	NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER]; /* 数位 */
};

struct NumericLong
{
	uint16		n_sign_dscale;	/* 符号+显示标度 */
	int16		n_weight;		/* 首位数字权重*/
	NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER]; /* 数位 */
};

union NumericChoice
{
	uint16		n_header;		/* 头部字 */
	struct NumericLong n_long;	/* 长形式（4字节头） */
	struct NumericShort n_short;	/* 短形式（2字节头） */
};

struct NumericData
{
	int32		vl_len_;		/* varlena 头（切勿直接触碰！） */
	union NumericChoice choice; /* 格式选择 */
};


/*
 * 高位的解释。
 */

#define NUMERIC_SIGN_MASK	0xC000
#define NUMERIC_POS			0x0000
#define NUMERIC_NEG			0x4000
#define NUMERIC_SHORT		0x8000
#define NUMERIC_SPECIAL		0xC000

#define NUMERIC_FLAGBITS(n) ((n)->choice.n_header & NUMERIC_SIGN_MASK)
#define NUMERIC_IS_SHORT(n)		(NUMERIC_FLAGBITS(n) == NUMERIC_SHORT)
#define NUMERIC_IS_SPECIAL(n)	(NUMERIC_FLAGBITS(n) == NUMERIC_SPECIAL)

#define NUMERIC_HDRSZ	(VARHDRSZ + sizeof(uint16) + sizeof(int16))
#define NUMERIC_HDRSZ_SHORT (VARHDRSZ + sizeof(uint16))

/*
 * 若标志位为 NUMERIC_SHORT 或 NUMERIC_SPECIAL，我们要的
 * 是短头部；否则要长头部。与其逐一比较每个取值，
 * 不如只看高位，还能略有效率提升。
 */
#define NUMERIC_HEADER_IS_SHORT(n)	(((n)->choice.n_header & 0x8000) != 0)
#define NUMERIC_HEADER_SIZE(n) \
	(VARHDRSZ + sizeof(uint16) + \
	 (NUMERIC_HEADER_IS_SHORT(n) ? 0 : sizeof(int16)))

/*
 * 特殊值的定义（NaN、正无穷大、负无穷大）。
 *
 * NUMERIC_SPECIAL 位之后的两位：00 表示 NaN，01 表示正
 * 无穷大，11 表示负无穷大。（这使得符号位与短格式值
 * 中符号位的位置一致，不过目前并未利用这一点。）
 * 本可先屏蔽其余位再测试有效位，但目前这些位必须
 * 为零，因此屏蔽只会平添开销。
 */
#define NUMERIC_EXT_SIGN_MASK	0xF000	/* 高位加 NaN/Inf 标志位 */
#define NUMERIC_NAN				0xC000
#define NUMERIC_PINF			0xD000
#define NUMERIC_NINF			0xF000
#define NUMERIC_INF_SIGN_MASK	0x2000

#define NUMERIC_EXT_FLAGBITS(n)	((n)->choice.n_header & NUMERIC_EXT_SIGN_MASK)
#define NUMERIC_IS_NAN(n)		((n)->choice.n_header == NUMERIC_NAN)
#define NUMERIC_IS_PINF(n)		((n)->choice.n_header == NUMERIC_PINF)
#define NUMERIC_IS_NINF(n)		((n)->choice.n_header == NUMERIC_NINF)
#define NUMERIC_IS_INF(n) \
	(((n)->choice.n_header & ~NUMERIC_INF_SIGN_MASK) == NUMERIC_PINF)

/*
 * 短格式定义。
 */

#define NUMERIC_SHORT_SIGN_MASK			0x2000
#define NUMERIC_SHORT_DSCALE_MASK		0x1F80
#define NUMERIC_SHORT_DSCALE_SHIFT		7
#define NUMERIC_SHORT_DSCALE_MAX		\
	(NUMERIC_SHORT_DSCALE_MASK >> NUMERIC_SHORT_DSCALE_SHIFT)
#define NUMERIC_SHORT_WEIGHT_SIGN_MASK	0x0040
#define NUMERIC_SHORT_WEIGHT_MASK		0x003F
#define NUMERIC_SHORT_WEIGHT_MAX		NUMERIC_SHORT_WEIGHT_MASK
#define NUMERIC_SHORT_WEIGHT_MIN		(-(NUMERIC_SHORT_WEIGHT_MASK+1))

/*
 * 提取符号、显示标度与权重。这些宏把字段值从 Numeric
 * （磁盘上）格式转换为适配 NumericVar 格式的形式。
 *
 * 注意，我们并不刻意保证无穷大的 dscale 与 weight 读为
 * 零；不过这无关紧要，因为我们从不把"特殊"值转成
 * NumericVar 形式。只有下面定义的常量（const_nan 等）
 * 才会把非有限值表示为 NumericVar。
 */

#define NUMERIC_DSCALE_MASK			0x3FFF
#define NUMERIC_DSCALE_MAX			NUMERIC_DSCALE_MASK

#define NUMERIC_SIGN(n) \
	(NUMERIC_IS_SHORT(n) ? \
		(((n)->choice.n_short.n_header & NUMERIC_SHORT_SIGN_MASK) ? \
		 NUMERIC_NEG : NUMERIC_POS) : \
		(NUMERIC_IS_SPECIAL(n) ? \
		 NUMERIC_EXT_FLAGBITS(n) : NUMERIC_FLAGBITS(n)))
#define NUMERIC_DSCALE(n)	(NUMERIC_HEADER_IS_SHORT((n)) ? \
	((n)->choice.n_short.n_header & NUMERIC_SHORT_DSCALE_MASK) \
		>> NUMERIC_SHORT_DSCALE_SHIFT \
	: ((n)->choice.n_long.n_sign_dscale & NUMERIC_DSCALE_MASK))
#define NUMERIC_WEIGHT(n)	(NUMERIC_HEADER_IS_SHORT((n)) ? \
	(((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_SIGN_MASK ? \
		~NUMERIC_SHORT_WEIGHT_MASK : 0) \
	 | ((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_MASK)) \
	: ((n)->choice.n_long.n_weight))

/*
 * 已存储 Numeric 值的最大权重（基于 NumericLong 中权重
 * 使用 int16 的事实）。注意，NumericVar 与 NumericSumAccum
 * 变量保存的中间值权重可能大得多。
 */
#define NUMERIC_WEIGHT_MAX			PG_INT16_MAX

/* ----------
 * NumericVar 是做算术运算所用的格式。数字位数组部分
 * 与 NumericData 存储格式相同，但头部更为复杂。
 *
 *
 * NumericVar 所表示的值由符号、权重、ndigits 与
 * digits[] 数组共同决定。若为"特殊"值（NaN 或 Inf），
 * 则只有符号字段有意义；ndigits 应为零，weight 与
 * dscale 字段则被忽略。
 *
 * 注意：假定 NumericVar 值的首个数字位要乘以
 * NBASE ** weight。换个说法，小数点前有 weight+1
 * 个数字位。weight 也可以小于 0。
 *
 * buf 指向 NumericVar 经 palloc 的数字位缓冲区的物理
 * 起始处。digits 指向实际使用的首个数字位（具有
 * 指定权重的那一个）。通常会在 buf 与 digits 之间
 * 留一两个未用数字位（预置为零），以便有空间存放
 * 最高数字位的进位而无需重新分配空间。只需将 digits
 * 递减（并递增 weight）就能为进位数字位腾出空间。
 * （数据库中存储的 numeric 值没有这种额外空间，它
 * 只在内存的 NumericVar。）
 *
 * 若 buf 为 NULL，则数字位缓冲区并未真正 palloc，因此
 * 不应被释放 --- 参见下面常量中的示例。
 *
 * dscale 即显示标度，为小数点后数字位个数的标称精度
 * （目前它必须始终 >= 0）。dscale 可以大于实际存储的
 * 小数数字位个数，这意味着我们已抑制了有意义的尾随
 * 零的存储。
 * 它绝不应小于已存储数字位的个数，因为那意味着隐藏
 * 实际存在的数字位。注意 dscale 始终以十进制数字位
 * 表示，因此可能对应非整数个的 NBASE 数字位 --- 除以
 * DEC_DIGITS 即可换算成 NBASE 数字位。
 *
 * rscale 即结果标度，是计算的目标精度。
 * 与 dscale 一样，以小数点后的十进制数字位个数表示，
 * 且目前始终 >= 0。
 * 注意 rscale 并不存入变量 --- 它是根据各输入的
 * dscale 现算出来的。
 *
 * 我们始终用"weight"指 numeric 值的 NBASE 基权重，但在
 * 某些标度相关的计算中，用 10 基权重（即数值的近似
 * log10）会更方便。为避免混淆，这种十进制单位的权重
 * 称为"dweight"。
 *
 * 注意：所有变量级函数的编写风格都允许将同一个变量
 * 既作参数又作目标。这可行是因为数字位缓冲区与
 * 变量本身是彼此分开的。
 * ----------
 */
typedef struct NumericVar
{
	int			ndigits;		/* digits[] 的位数 - 可为 0！ */
	int			weight;			/* 首数字位权重 */
	int			sign;			/* NUMERIC_POS, _NEG, _NAN, _PINF, or _NINF */
	int			dscale;			/* 显示标度 */
	NumericDigit *buf;			/* digits[] palloc 空间的起始处 */
	NumericDigit *digits;		/* NBASE 数字位 */
} NumericVar;


/* ----------
 * generate_series 数据
 * ----------
 */
typedef struct
{
	NumericVar	current;
	NumericVar	stop;
	NumericVar	step;
} generate_series_numeric_fctx;


/* ----------
 * 排序支持
 * ----------
 */
typedef struct
{
	void	   *buf;			/* 短 varlena 的缓冲区 */
	int64		input_count;	/* 已见到的非空值个数 */
	bool		estimating;		/* 估算基数时为 true */

	hyperLogLogState abbr_card; /* 基数估算器 */
} NumericSortSupport;


/* ----------
 * 快速和累加器。
 *
 * NumericSumAccum 用于实现 SUM() 以及其他跟踪输入值总和
 * 的标准聚合。它以 32 位整数存数字位，而非普通的
 * 16 位整数（NBASE=10000 时）。这样我们可以安全地累加
 * 最多 NBASE - 1 个值而不传播进位，然后才有任何数字位
 * 溢出的风险。'num_uncarried' 记录已累加了多少个值而
 * 未传播进位。
 *
 * 正值与负值分别累加在 'pos_digits' 与 'neg_digits' 中。
 * 这比对每个新值判断该做加法还是减法更简单、更快速
 * （这样避免的逻辑见 sub_var()）。两缓冲区大小相同，
 * 且权重与标度相同。
 * 在 accum_sum_final() 中，正和与负
 * 和相加以产生最终结果。
 *
 * 当新值的 ndigits 或 weight 大于累加器当前的对应值
 * 时，累加器会扩大以容纳该新值。
 * 我们通常预留一个零数字位用于进位传播，这一点由
 * 'have_carry_space' 标志表示。当 accum_sum_carry() 用尽
 * 该保留数字位时，会清除 'have_carry_space' 标志。下一次
 * 调用 accum_sum_add() 时会扩大缓冲区，为多出的数字位
 * 腾出空间，再置位标志。
 *
 * 要初始化新的累加器，只需把所有字段重置为零。
 *
 * 累加器不处理 NaN。
 * ----------
 */
typedef struct NumericSumAccum
{
	int			ndigits;
	int			weight;
	int			dscale;
	int			num_uncarried;
	bool		have_carry_space;
	int32	   *pos_digits;
	int32	   *neg_digits;
} NumericSumAccum;


/*
 * 为 numeric 缩略键表示自定义的打包/解包宏，以免
 * 依赖于 USE_FLOAT8_BYVAL。所用缩略的类型只取决于
 * datum 的大小，
 * 而非 float8 的参数传递约定。
 *
 * 有限值的缩略范围从 +PG_INT64/32_MAX 到
 * -PG_INT64/32_MAX。NaN 的缩略为 PG_INT64/32_MIN，我们
 * 定义排序顺序使其成立（详见下方注释）。
 * PINF 与 NINF 共用最大与最小有限缩略类的缩略值。
 *
 */
#define NUMERIC_ABBREV_BITS (SIZEOF_DATUM * BITS_PER_BYTE)
#if SIZEOF_DATUM == 8
#define NumericAbbrevGetDatum(X) ((Datum) (X))
#define DatumGetNumericAbbrev(X) ((int64) (X))
#define NUMERIC_ABBREV_NAN		 NumericAbbrevGetDatum(PG_INT64_MIN)
#define NUMERIC_ABBREV_PINF		 NumericAbbrevGetDatum(-PG_INT64_MAX)
#define NUMERIC_ABBREV_NINF		 NumericAbbrevGetDatum(PG_INT64_MAX)
#else
#define NumericAbbrevGetDatum(X) ((Datum) (X))
#define DatumGetNumericAbbrev(X) ((int32) (X))
#define NUMERIC_ABBREV_NAN		 NumericAbbrevGetDatum(PG_INT32_MIN)
#define NUMERIC_ABBREV_PINF		 NumericAbbrevGetDatum(-PG_INT32_MAX)
#define NUMERIC_ABBREV_NINF		 NumericAbbrevGetDatum(PG_INT32_MAX)
#endif


/* ----------
 * 一些预初始化的常量
 * ----------
 */
static const NumericDigit const_zero_data[1] = {0};
static const NumericVar const_zero =
{0, 0, NUMERIC_POS, 0, NULL, (NumericDigit *) const_zero_data};

static const NumericDigit const_one_data[1] = {1};
static const NumericVar const_one =
{1, 0, NUMERIC_POS, 0, NULL, (NumericDigit *) const_one_data};

static const NumericVar const_minus_one =
{1, 0, NUMERIC_NEG, 0, NULL, (NumericDigit *) const_one_data};

static const NumericDigit const_two_data[1] = {2};
static const NumericVar const_two =
{1, 0, NUMERIC_POS, 0, NULL, (NumericDigit *) const_two_data};

#if DEC_DIGITS == 4
static const NumericDigit const_zero_point_nine_data[1] = {9000};
#elif DEC_DIGITS == 2
static const NumericDigit const_zero_point_nine_data[1] = {90};
#elif DEC_DIGITS == 1
static const NumericDigit const_zero_point_nine_data[1] = {9};
#endif
static const NumericVar const_zero_point_nine =
{1, -1, NUMERIC_POS, 1, NULL, (NumericDigit *) const_zero_point_nine_data};

#if DEC_DIGITS == 4
static const NumericDigit const_one_point_one_data[2] = {1, 1000};
#elif DEC_DIGITS == 2
static const NumericDigit const_one_point_one_data[2] = {1, 10};
#elif DEC_DIGITS == 1
static const NumericDigit const_one_point_one_data[2] = {1, 1};
#endif
static const NumericVar const_one_point_one =
{2, 0, NUMERIC_POS, 1, NULL, (NumericDigit *) const_one_point_one_data};

static const NumericVar const_nan =
{0, 0, NUMERIC_NAN, 0, NULL, NULL};

static const NumericVar const_pinf =
{0, 0, NUMERIC_PINF, 0, NULL, NULL};

static const NumericVar const_ninf =
{0, 0, NUMERIC_NINF, 0, NULL, NULL};

#if DEC_DIGITS == 4
static const int round_powers[4] = {0, 1000, 100, 10};
#endif


/* ----------
 * 局部函数
 * ----------
 */

#ifdef NUMERIC_DEBUG
static void dump_numeric(const char *str, Numeric num);
static void dump_var(const char *str, NumericVar *var);
#else
#define dump_numeric(s,n)
#define dump_var(s,v)
#endif

#define digitbuf_alloc(ndigits)  \
	((NumericDigit *) palloc((ndigits) * sizeof(NumericDigit)))
#define digitbuf_free(buf)	\
	do { \
		 if ((buf) != NULL) \
			 pfree(buf); \
	} while (0)

#define init_var(v)		memset(v, 0, sizeof(NumericVar))

#define NUMERIC_DIGITS(num) (NUMERIC_HEADER_IS_SHORT(num) ? \
	(num)->choice.n_short.n_data : (num)->choice.n_long.n_data)
#define NUMERIC_NDIGITS(num) \
	((VARSIZE(num) - NUMERIC_HEADER_SIZE(num)) / sizeof(NumericDigit))
#define NUMERIC_CAN_BE_SHORT(scale,weight) \
	((scale) <= NUMERIC_SHORT_DSCALE_MAX && \
	(weight) <= NUMERIC_SHORT_WEIGHT_MAX && \
	(weight) >= NUMERIC_SHORT_WEIGHT_MIN)

static void alloc_var(NumericVar *var, int ndigits);
static void free_var(NumericVar *var);
static void zero_var(NumericVar *var);

static bool set_var_from_str(const char *str, const char *cp,
							 NumericVar *dest, const char **endptr,
							 Node *escontext);
static bool set_var_from_non_decimal_integer_str(const char *str,
												 const char *cp, int sign,
												 int base, NumericVar *dest,
												 const char **endptr,
												 Node *escontext);
static void set_var_from_num(Numeric num, NumericVar *dest);
static void init_var_from_num(Numeric num, NumericVar *dest);
static void set_var_from_var(const NumericVar *value, NumericVar *dest);
static char *get_str_from_var(const NumericVar *var);
static char *get_str_from_var_sci(const NumericVar *var, int rscale);

static void numericvar_serialize(StringInfo buf, const NumericVar *var);
static void numericvar_deserialize(StringInfo buf, NumericVar *var);

static Numeric duplicate_numeric(Numeric num);
static Numeric make_result(const NumericVar *var);
static Numeric make_result_opt_error(const NumericVar *var, bool *have_error);

static bool apply_typmod(NumericVar *var, int32 typmod, Node *escontext);
static bool apply_typmod_special(Numeric num, int32 typmod, Node *escontext);

static bool numericvar_to_int32(const NumericVar *var, int32 *result);
static bool numericvar_to_int64(const NumericVar *var, int64 *result);
static void int64_to_numericvar(int64 val, NumericVar *var);
static bool numericvar_to_uint64(const NumericVar *var, uint64 *result);
#ifdef HAVE_INT128
static bool numericvar_to_int128(const NumericVar *var, int128 *result);
static void int128_to_numericvar(int128 val, NumericVar *var);
#endif
static double numericvar_to_double_no_overflow(const NumericVar *var);

static Datum numeric_abbrev_convert(Datum original_datum, SortSupport ssup);
static bool numeric_abbrev_abort(int memtupcount, SortSupport ssup);
static int	numeric_fast_cmp(Datum x, Datum y, SortSupport ssup);
static int	numeric_cmp_abbrev(Datum x, Datum y, SortSupport ssup);

static Datum numeric_abbrev_convert_var(const NumericVar *var,
										NumericSortSupport *nss);

static int	cmp_numerics(Numeric num1, Numeric num2);
static int	cmp_var(const NumericVar *var1, const NumericVar *var2);
static int	cmp_var_common(const NumericDigit *var1digits, int var1ndigits,
						   int var1weight, int var1sign,
						   const NumericDigit *var2digits, int var2ndigits,
						   int var2weight, int var2sign);
static void add_var(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result);
static void sub_var(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result);
static void mul_var(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result,
					int rscale);
static void div_var(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result,
					int rscale, bool round);
static void div_var_fast(const NumericVar *var1, const NumericVar *var2,
						 NumericVar *result, int rscale, bool round);
static void div_var_int(const NumericVar *var, int ival, int ival_weight,
						NumericVar *result, int rscale, bool round);
#ifdef HAVE_INT128
static void div_var_int64(const NumericVar *var, int64 ival, int ival_weight,
						  NumericVar *result, int rscale, bool round);
#endif
static int	select_div_scale(const NumericVar *var1, const NumericVar *var2);
static void mod_var(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result);
static void div_mod_var(const NumericVar *var1, const NumericVar *var2,
						NumericVar *quot, NumericVar *rem);
static void ceil_var(const NumericVar *var, NumericVar *result);
static void floor_var(const NumericVar *var, NumericVar *result);

static void gcd_var(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result);
static void sqrt_var(const NumericVar *arg, NumericVar *result, int rscale);
static void exp_var(const NumericVar *arg, NumericVar *result, int rscale);
static int	estimate_ln_dweight(const NumericVar *var);
static void ln_var(const NumericVar *arg, NumericVar *result, int rscale);
static void log_var(const NumericVar *base, const NumericVar *num,
					NumericVar *result);
static void power_var(const NumericVar *base, const NumericVar *exp,
					  NumericVar *result);
static void power_var_int(const NumericVar *base, int exp, int exp_dscale,
						  NumericVar *result);
static void power_ten_int(int exp, NumericVar *result);

static int	cmp_abs(const NumericVar *var1, const NumericVar *var2);
static int	cmp_abs_common(const NumericDigit *var1digits, int var1ndigits,
						   int var1weight,
						   const NumericDigit *var2digits, int var2ndigits,
						   int var2weight);
static void add_abs(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result);
static void sub_abs(const NumericVar *var1, const NumericVar *var2,
					NumericVar *result);
static void round_var(NumericVar *var, int rscale);
static void trunc_var(NumericVar *var, int rscale);
static void strip_var(NumericVar *var);
static void compute_bucket(Numeric operand, Numeric bound1, Numeric bound2,
						   const NumericVar *count_var, bool reversed_bounds,
						   NumericVar *result_var);

static void accum_sum_add(NumericSumAccum *accum, const NumericVar *val);
static void accum_sum_rescale(NumericSumAccum *accum, const NumericVar *val);
static void accum_sum_carry(NumericSumAccum *accum);
static void accum_sum_reset(NumericSumAccum *accum);
static void accum_sum_final(NumericSumAccum *accum, NumericVar *result);
static void accum_sum_copy(NumericSumAccum *dst, NumericSumAccum *src);
static void accum_sum_combine(NumericSumAccum *accum, NumericSumAccum *accum2);


/* ----------------------------------------------------------------------
 *
 * 输入、输出与四舍五入函数
 *
 * ----------------------------------------------------------------------
 */


/*
 * numeric_in() -
 *
 *	numeric 数据类型的输入函数
 */
Datum
numeric_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	Numeric		res;
	const char *cp;
	const char *numstart;
	int			sign;

	/* 跳过前导空格 */
	cp = str;
	while (*cp)
	{
		if (!isspace((unsigned char) *cp))
			break;
		cp++;
	}

	/*
	 * 处理数值的符号。这与 set_var_from_str() 中的逻辑重复，
	 * 但此处值得重做，以简化无穷大与非十进制
	 * 整数的处理。
	 */
	numstart = cp;
	sign = NUMERIC_POS;

	if (*cp == '+')
		cp++;
	else if (*cp == '-')
	{
		sign = NUMERIC_NEG;
		cp++;
	}

	/*
	 * 检查 NaN 与无穷大。识别的字符串与 float8in() 相同。
	 *
	 *
	 * 由于其余合法输入符号后都有数字位或小数点，仅需在
	 * 不是这种情形时才检查 NaN/无穷大。
	 */
	if (!isdigit((unsigned char) *cp) && *cp != '.')
	{
		/*
		 * 该数必为 NaN 或无穷大；其余只能是语法错误。
		 * 注意 NaN 绝不能带符号。
		 */
		if (pg_strncasecmp(numstart, "NaN", 3) == 0)
		{
			res = make_result(&const_nan);
			cp = numstart + 3;
		}
		else if (pg_strncasecmp(cp, "Infinity", 8) == 0)
		{
			res = make_result(sign == NUMERIC_POS ? &const_pinf : &const_ninf);
			cp += 8;
		}
		else if (pg_strncasecmp(cp, "inf", 3) == 0)
		{
			res = make_result(sign == NUMERIC_POS ? &const_pinf : &const_ninf);
			cp += 3;
		}
		else
			goto invalid_syntax;

		/*
		 * 检查尾随垃圾；之后应当只剩空格。
		 *
		 * 特意在 typmod 前检查，想让尾随垃圾语法错误先于
		 * apply_typmod_special() 所产生的语义错误。
		 *
		 */
		while (*cp)
		{
			if (!isspace((unsigned char) *cp))
				goto invalid_syntax;
			cp++;
		}

		if (!apply_typmod_special(res, typmod, escontext))
			PG_RETURN_NULL();
	}
	else
	{
		/*
		 * 此时是普通 numeric 值，可能是非十进制整数或常规
		 * 十进制数。
		 */
		NumericVar	value;
		int			base;
		bool		have_error;

		init_var(&value);

		/*
		 * 看非十进制前缀（"0x"、"0o" 或 "0b"）判定基数。
		 *
		 */
		if (cp[0] == '0')
		{
			switch (cp[1])
			{
				case 'x':
				case 'X':
					base = 16;
					break;
				case 'o':
				case 'O':
					base = 8;
					break;
				case 'b':
				case 'B':
					base = 2;
					break;
				default:
					base = 10;
			}
		}
		else
			base = 10;

		/* 解析数值的其余部分并应用符号 */
		if (base == 10)
		{
			if (!set_var_from_str(str, cp, &value, &cp, escontext))
				PG_RETURN_NULL();
			value.sign = sign;
		}
		else
		{
			if (!set_var_from_non_decimal_integer_str(str, cp + 2, sign, base,
													  &value, &cp, escontext))
				PG_RETURN_NULL();
		}

		/*
		 * 此时应只剩空格。同上文，完成语法检查后再抛
		 * typmod 错误。
		 */
		while (*cp)
		{
			if (!isspace((unsigned char) *cp))
				goto invalid_syntax;
			cp++;
		}

		if (!apply_typmod(&value, typmod, escontext))
			PG_RETURN_NULL();

		res = make_result_opt_error(&value, &have_error);

		if (have_error)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value overflows numeric format")));

		free_var(&value);
	}

	PG_RETURN_NUMERIC(res);

invalid_syntax:
	ereturn(escontext, (Datum) 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"numeric", str)));
}


/*
 * numeric_out() -
 *
 *	numeric 数据类型的输出函数
 */
Datum
numeric_out(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	char	   *str;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_PINF(num))
			PG_RETURN_CSTRING(pstrdup("Infinity"));
		else if (NUMERIC_IS_NINF(num))
			PG_RETURN_CSTRING(pstrdup("-Infinity"));
		else
			PG_RETURN_CSTRING(pstrdup("NaN"));
	}

	/*
	 * 取得变量格式的数值。
	 */
	init_var_from_num(num, &x);

	str = get_str_from_var(&x);

	PG_RETURN_CSTRING(str);
}

/*
 * numeric_is_nan() -
 *
 *	Numeric 值为 NaN？
 */
bool
numeric_is_nan(Numeric num)
{
	return NUMERIC_IS_NAN(num);
}

/*
 * numeric_is_inf() -
 *
 *	Numeric 值是无穷大吗？
 */
bool
numeric_is_inf(Numeric num)
{
	return NUMERIC_IS_INF(num);
}

/*
 * numeric_is_integral() -
 *
 *	Numeric 值是整数吗？
 */
static bool
numeric_is_integral(Numeric num)
{
	NumericVar	arg;

	/* 拒绝 NaN，但无穷大视为整数 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_NAN(num))
			return false;
		return true;
	}

	/* 若小数点右侧没有数字位则为整数 */
	init_var_from_num(num, &arg);

	return (arg.ndigits == 0 || arg.ndigits <= arg.weight + 1);
}

/*
 * make_numeric_typmod() -
 *
 *	把 numeric 的精度与标度值打包进一个 typmod。高 16 位
 *	用于精度（实际上并非所有位都需要，因最大允许精度
 *	为 1000）。低 16 位用于标度，但由于标度被限制在
 *	[-1000, 1000] 区间内，我们只使用这 16 位中的低 11 位，
 *	其余 5 位留空未用，以备将来使用。
 *
 *
 *	由于纯历史原因，结果还要加上 VARHDRSZ，因此高 16 位
 *	中未用空间并不像看上去那样全部可用。（不能让结
 *	果溢出为负 int32，否则系统其他部分会将其视为无效
 *	的 typmod。）
 */
static inline int32
make_numeric_typmod(int precision, int scale)
{
	return ((precision << 16) | (scale & 0x7ff)) + VARHDRSZ;
}

/*
 * 由于这一偏移，有效的 numeric typmod 至少为 VARHDRSZ
 */
static inline bool
is_valid_numeric_typmod(int32 typmod)
{
	return typmod >= (int32) VARHDRSZ;
}

/*
 * numeric_typmod_precision() -
 *
 *	从 numeric typmod 中提取精度 --- 见 make_numeric_typmod()。
 */
static inline int
numeric_typmod_precision(int32 typmod)
{
	return ((typmod - VARHDRSZ) >> 16) & 0xffff;
}

/*
 * numeric_typmod_scale() -
 *
 *	从 numeric typmod 中提取标度 --- 见 make_numeric_typmod()。
 *
 *	注意标度可能为负，所以解包时必须做符号扩展。
 *	使用位技巧 (x^1024)-1024 来完成，它可将 11 位二进制
 *	补码数 x 做符号扩展。
 */
static inline int
numeric_typmod_scale(int32 typmod)
{
	return (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
}

/*
 * numeric_maximum_size() -
 *
 *	给定 typmod 的 numeric 最大尺寸，无限制/未知时为 -1。
 */
int32
numeric_maximum_size(int32 typmod)
{
	int			precision;
	int			numeric_digits;

	if (!is_valid_numeric_typmod(typmod))
		return -1;

	/* 精度（即最大数字位个数）在 typmod 高位 */
	precision = numeric_typmod_precision(typmod);

	/*
	 * 此公式算出存储指定个数十进制数字位所需
	 * NumericDigit 个数上限。由于权重以 NumericDigit 个数而
	 * 非十进制位数存储，首个 NumericDigit 可能只含一个
	 * 十进制数字位。故前两个十进制位可能需两个
	 * NumericDigit 来存放；到 DEC_DIGITS + 2 个十进制
	 * 位时，才可能需要第三个 NumericDigit。
	 *
	 *
	 */
	numeric_digits = (precision + 2 * (DEC_DIGITS - 1)) / DEC_DIGITS;

	/*
	 * 多数情况下 numeric 尺寸会小于下面算出的值，因
	 * varlena 头存盘前通常被解 TOAST 至单字节，
	 * 也可能用短头部。
	 * 但此处我们的任务是计算最坏情形。
	 *
	 */
	return NUMERIC_HDRSZ + (numeric_digits * sizeof(NumericDigit));
}

/*
 * numeric_out_sci() -
 *
 *	以科学记数法输出 numeric 数据类型的函数。
 */
char *
numeric_out_sci(Numeric num, int scale)
{
	NumericVar	x;
	char	   *str;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_PINF(num))
			return pstrdup("Infinity");
		else if (NUMERIC_IS_NINF(num))
			return pstrdup("-Infinity");
		else
			return pstrdup("NaN");
	}

	init_var_from_num(num, &x);

	str = get_str_from_var_sci(&x, scale);

	return str;
}

/*
 * numeric_normalize() -
 *
 *	numeric 输出函数，抑制无意义的尾随零，并去掉随后
 *	的尾随小数点。意图是当且仅当输入 numeric 值比较
 *	相等时，输出字符串才相等。
 *
 */
char *
numeric_normalize(Numeric num)
{
	NumericVar	x;
	char	   *str;
	int			last;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_PINF(num))
			return pstrdup("Infinity");
		else if (NUMERIC_IS_NINF(num))
			return pstrdup("-Infinity");
		else
			return pstrdup("NaN");
	}

	init_var_from_num(num, &x);

	str = get_str_from_var(&x);

	/* 若无小数点，则必然没有可去除的内容。 */
	if (strchr(str, '.') != NULL)
	{
		/*
		 * 回退越过尾随小数零。既然小数点存在，必能安全
		 * 终止。
		 */
		last = strlen(str) - 1;
		while (str[last] == '0')
			last--;

		/* 若小数点已在最后，也要将其去除。 */
		if (str[last] == '.')
			last--;

		/* 删除回退越过的内容。 */
		str[last + 1] = '\0';
	}

	return str;
}

/*
 *		numeric_recv			- 把外部二进制格式转换为 numeric
 *
 * 外部格式是一串 int16：
 * ndigits, weight, sign, dscale, NumericDigits.
 */
Datum
numeric_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	NumericVar	value;
	Numeric		res;
	int			len,
				i;

	init_var(&value);

	len = (uint16) pq_getmsgint(buf, sizeof(uint16));

	alloc_var(&value, len);

	value.weight = (int16) pq_getmsgint(buf, sizeof(int16));
	/* 权重允许任意 int16，行吗？ */

	value.sign = (uint16) pq_getmsgint(buf, sizeof(uint16));
	if (!(value.sign == NUMERIC_POS ||
		  value.sign == NUMERIC_NEG ||
		  value.sign == NUMERIC_NAN ||
		  value.sign == NUMERIC_PINF ||
		  value.sign == NUMERIC_NINF))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid sign in external \"numeric\" value")));

	value.dscale = (uint16) pq_getmsgint(buf, sizeof(uint16));
	if ((value.dscale & NUMERIC_DSCALE_MASK) != value.dscale)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid scale in external \"numeric\" value")));

	for (i = 0; i < len; i++)
	{
		NumericDigit d = pq_getmsgint(buf, sizeof(NumericDigit));

		if (d < 0 || d >= NBASE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid digit in external \"numeric\" value")));
		value.digits[i] = d;
	}

	/*
	 * 若给定 dscale 会隐藏数字位，就把那些数字位截断。
	 * 我们也可以改为抛错，但那需要一大堆额外代码（与
	 * trunc_var 相当），还可能引发客户端兼容性问题。
	 * 勿对特殊值应用 trunc_var，因其可能出错；反正
	 * 也用不着它，因为 make_result 会忽略除符号外的
	 * 所有字段。
	 *
	 * 做完之后，务必检查 typmod 限制。
	 */
	if (value.sign == NUMERIC_POS ||
		value.sign == NUMERIC_NEG)
	{
		trunc_var(&value, value.dscale);

		(void) apply_typmod(&value, typmod, NULL);

		res = make_result(&value);
	}
	else
	{
		/* apply_typmod_special 要求我们先构造好 Numeric */
		res = make_result(&value);

		(void) apply_typmod_special(res, typmod, NULL);
	}

	free_var(&value);

	PG_RETURN_NUMERIC(res);
}

/*
 *		numeric_send		- 把 numeric 转为二进制格式
 */
Datum
numeric_send(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	StringInfoData buf;
	int			i;

	init_var_from_num(num, &x);

	pq_begintypsend(&buf);

	pq_sendint16(&buf, x.ndigits);
	pq_sendint16(&buf, x.weight);
	pq_sendint16(&buf, x.sign);
	pq_sendint16(&buf, x.dscale);
	for (i = 0; i < x.ndigits; i++)
		pq_sendint16(&buf, x.digits[i]);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * numeric_support()
 *
 * numeric() 长度强转函数的规划器支持函数。
 *
 * 展平那些仅表示允许精度提升的调用。
 * 标度变化会改变每个 datum，因此不可优化。有些值（如
 * 1E-1001）只能放入无约束 numeric，因此从无约束 numeric
 * 改为任何有约束 numeric 同样不可优化。
 */
Datum
numeric_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *expr = req->fcall;
		Node	   *typmod;

		Assert(list_length(expr->args) >= 2);

		typmod = (Node *) lsecond(expr->args);

		if (IsA(typmod, Const) && !((Const *) typmod)->constisnull)
		{
			Node	   *source = (Node *) linitial(expr->args);
			int32		old_typmod = exprTypmod(source);
			int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
			int32		old_scale = numeric_typmod_scale(old_typmod);
			int32		new_scale = numeric_typmod_scale(new_typmod);
			int32		old_precision = numeric_typmod_precision(old_typmod);
			int32		new_precision = numeric_typmod_precision(new_typmod);

			/*
			 * 若 new_typmod 无效，则目标无约束；没问题。
			 * 若 old_typmod 有效，则源受约束，只要标度不变
			 * 且精度未减小即可。
			 * 更多说明见函数头部注释。
			 *
			 */
			if (!is_valid_numeric_typmod(new_typmod) ||
				(is_valid_numeric_typmod(old_typmod) &&
				 new_scale == old_scale && new_precision >= old_precision))
				ret = relabel_to_typmod(source, new_typmod);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * numeric() -
 *
 *	这是 Postgres 数据库系统在值存入元组属性之前调
 *	用的特殊函数。属性的精度与标度须施加于该值。
 *
 */
Datum
numeric		(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	int32		typmod = PG_GETARG_INT32(1);
	Numeric		new;
	int			precision;
	int			scale;
	int			ddigits;
	int			maxdigits;
	int			dscale;
	NumericVar	var;

	/*
	 * 处理 NaN 与无穷大：若 apply_typmod_special 不抱怨，
	 * 就返回输入的副本。
	 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		(void) apply_typmod_special(num, typmod, NULL);
		PG_RETURN_NUMERIC(duplicate_numeric(num));
	}

	/*
	 * 若值不是有效的类型修饰符，直接返回输入值的副本
	 *
	 */
	if (!is_valid_numeric_typmod(typmod))
		PG_RETURN_NUMERIC(duplicate_numeric(num));

	/*
	 * 从 typmod 值中取出精度与标度
	 */
	precision = numeric_typmod_precision(typmod);
	scale = numeric_typmod_scale(typmod);
	maxdigits = precision - scale;

	/* 目标显示标度为非负值 */
	dscale = Max(scale, 0);

	/*
	 * 若数值必在界内且目标标度无需四舍五入，只需
	 * 复制输入并改其标度字段，除非更大的标度迫使我们放弃
	 * 短表示。
	 * （注意我们假定现有 dscale 如实…）
	 *
	 */
	ddigits = (NUMERIC_WEIGHT(num) + 1) * DEC_DIGITS;
	if (ddigits <= maxdigits && scale >= NUMERIC_DSCALE(num)
		&& (NUMERIC_CAN_BE_SHORT(dscale, NUMERIC_WEIGHT(num))
			|| !NUMERIC_IS_SHORT(num)))
	{
		new = duplicate_numeric(num);
		if (NUMERIC_IS_SHORT(num))
			new->choice.n_short.n_header =
				(num->choice.n_short.n_header & ~NUMERIC_SHORT_DSCALE_MASK)
				| (dscale << NUMERIC_SHORT_DSCALE_SHIFT);
		else
			new->choice.n_long.n_sign_dscale = NUMERIC_SIGN(new) |
				((uint16) dscale & NUMERIC_DSCALE_MASK);
		PG_RETURN_NUMERIC(new);
	}

	/*
	 * 确实需要动些手脚 - 把数值解包为变量，交给
	 * apply_typmod() 去做。
	 */
	init_var(&var);

	set_var_from_num(num, &var);
	(void) apply_typmod(&var, typmod, NULL);
	new = make_result(&var);

	free_var(&var);

	PG_RETURN_NUMERIC(new);
}

Datum
numerictypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;
	int32		typmod;

	tl = ArrayGetIntegerTypmods(ta, &n);

	if (n == 2)
	{
		if (tl[0] < 1 || tl[0] > NUMERIC_MAX_PRECISION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("NUMERIC precision %d must be between 1 and %d",
							tl[0], NUMERIC_MAX_PRECISION)));
		if (tl[1] < NUMERIC_MIN_SCALE || tl[1] > NUMERIC_MAX_SCALE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("NUMERIC scale %d must be between %d and %d",
							tl[1], NUMERIC_MIN_SCALE, NUMERIC_MAX_SCALE)));
		typmod = make_numeric_typmod(tl[0], tl[1]);
	}
	else if (n == 1)
	{
		if (tl[0] < 1 || tl[0] > NUMERIC_MAX_PRECISION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("NUMERIC precision %d must be between 1 and %d",
							tl[0], NUMERIC_MAX_PRECISION)));
		/* 标度默认为零 */
		typmod = make_numeric_typmod(tl[0], 0);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid NUMERIC type modifier")));
		typmod = 0;				/* 让编译器安静 */
	}

	PG_RETURN_INT32(typmod);
}

Datum
numerictypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);
	char	   *res = (char *) palloc(64);

	if (is_valid_numeric_typmod(typmod))
		snprintf(res, 64, "(%d,%d)",
				 numeric_typmod_precision(typmod),
				 numeric_typmod_scale(typmod));
	else
		*res = '\0';

	PG_RETURN_CSTRING(res);
}


/* ----------------------------------------------------------------------
 *
 * 符号处理、四舍五入等函数
 *
 * ----------------------------------------------------------------------
 */

Datum
numeric_abs(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;

	/*
	 * 直接在打包格式上用简单方式处理
	 */
	res = duplicate_numeric(num);

	if (NUMERIC_IS_SHORT(num))
		res->choice.n_short.n_header =
			num->choice.n_short.n_header & ~NUMERIC_SHORT_SIGN_MASK;
	else if (NUMERIC_IS_SPECIAL(num))
	{
		/* 这会把 -Inf 变为 Inf，且不影响 NaN */
		res->choice.n_short.n_header =
			num->choice.n_short.n_header & ~NUMERIC_INF_SIGN_MASK;
	}
	else
		res->choice.n_long.n_sign_dscale = NUMERIC_POS | NUMERIC_DSCALE(num);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_uminus(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;

	/*
	 * 直接在打包格式上用简单方式处理
	 */
	res = duplicate_numeric(num);

	if (NUMERIC_IS_SPECIAL(num))
	{
		/* 若为 Inf/-Inf 则翻转符号 */
		if (!NUMERIC_IS_NAN(num))
			res->choice.n_short.n_header =
				num->choice.n_short.n_header ^ NUMERIC_INF_SIGN_MASK;
	}

	/*
	 * 打包格式总已完全去除零数字位。所以一旦排除特
	 * 殊值，即可凭全无数字位识别零。对零不做处理。
	 *
	 */
	else if (NUMERIC_NDIGITS(num) != 0)
	{
		/* 否则翻转符号 */
		if (NUMERIC_IS_SHORT(num))
			res->choice.n_short.n_header =
				num->choice.n_short.n_header ^ NUMERIC_SHORT_SIGN_MASK;
		else if (NUMERIC_SIGN(num) == NUMERIC_POS)
			res->choice.n_long.n_sign_dscale =
				NUMERIC_NEG | NUMERIC_DSCALE(num);
		else
			res->choice.n_long.n_sign_dscale =
				NUMERIC_POS | NUMERIC_DSCALE(num);
	}

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_uplus(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);

	PG_RETURN_NUMERIC(duplicate_numeric(num));
}


/*
 * numeric_sign_internal() -
 *
 * 参数小于 0 时返回 -1，参数等于 0 时返回 0，参数大
 * 于 0 时返回 1。调用者必须已处理 NaN 情形，但我们
 * 可以在此处处理无穷大。
 */
static int
numeric_sign_internal(Numeric num)
{
	if (NUMERIC_IS_SPECIAL(num))
	{
		Assert(!NUMERIC_IS_NAN(num));
		/* 必为 Inf 或 -Inf */
		if (NUMERIC_IS_PINF(num))
			return 1;
		else
			return -1;
	}

	/*
	 * 打包格式总已完全去除零数字位。所以一旦排除特
	 * 殊值，即可凭全无数字位识别零。
	 *
	 */
	else if (NUMERIC_NDIGITS(num) == 0)
		return 0;
	else if (NUMERIC_SIGN(num) == NUMERIC_NEG)
		return -1;
	else
		return 1;
}

/*
 * numeric_sign() -
 *
 * 参数小于 0 时返回 -1，参数等于 0 时返回 0，参数大
 * 于 0 时返回 1。
 */
Datum
numeric_sign(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);

	/*
	 * 处理 NaN（无穷大可正常处理）
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	switch (numeric_sign_internal(num))
	{
		case 0:
			PG_RETURN_NUMERIC(make_result(&const_zero));
		case 1:
			PG_RETURN_NUMERIC(make_result(&const_one));
		case -1:
			PG_RETURN_NUMERIC(make_result(&const_minus_one));
	}

	Assert(false);
	return (Datum) 0;
}


/*
 * numeric_round() -
 *
 *	将值四舍五入为小数点后有 'scale' 个数字位。
 *	允许负 'scale'，即小数点前四舍五入 --- Oracle 将
 *	这样解释四舍五入。
 */
Datum
numeric_round(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	int32		scale = PG_GETARG_INT32(1);
	Numeric		res;
	NumericVar	arg;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NUMERIC(duplicate_numeric(num));

	/*
	 * 限制标度值以避免计算中可能出现的溢出。
	 *
	 * 这些限制基于 Numeric 值小数点前后可有的最大数字位
	 * 个数，但必须为整数部分多留一位，以防最高位四舍
	 * 五入进位；我们必须检查这是否导致 Numeric 溢出。
	 *
	 */
	scale = Max(scale, -(NUMERIC_WEIGHT_MAX + 1) * DEC_DIGITS - 1);
	scale = Min(scale, NUMERIC_DSCALE_MAX);

	/*
	 * 解包参数并在正确的数字位位置四舍五入
	 */
	init_var(&arg);
	set_var_from_num(num, &arg);

	round_var(&arg, scale);

	/* 不允许输出 dscale 为负 */
	if (scale < 0)
		arg.dscale = 0;

	/*
	 * 返回四舍五入结果
	 */
	res = make_result(&arg);

	free_var(&arg);
	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_trunc() -
 *
 *	将值截断为小数点后有 'scale' 个数字位。
 *	允许负的 'scale'，表示在小数点之前截断 ---
 *	Oracle 就是这样解释截断的。
 */
Datum
numeric_trunc(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	int32		scale = PG_GETARG_INT32(1);
	Numeric		res;
	NumericVar	arg;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NUMERIC(duplicate_numeric(num));

	/*
	 * 限制标度值以避免计算中可能出现的溢出。
	 *
	 * 这些限制基于 Numeric 值小数点前后可有的最大数字位
	 * 个数。
	 */
	scale = Max(scale, -(NUMERIC_WEIGHT_MAX + 1) * DEC_DIGITS);
	scale = Min(scale, NUMERIC_DSCALE_MAX);

	/*
	 * 解包参数并在正确的数字位位置截断
	 */
	init_var(&arg);
	set_var_from_num(num, &arg);

	trunc_var(&arg, scale);

	/* 不允许输出 dscale 为负 */
	if (scale < 0)
		arg.dscale = 0;

	/*
	 * 返回截断后的结果
	 */
	res = make_result(&arg);

	free_var(&arg);
	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_ceil() -
 *
 *	返回大于等于参数的最小整数
 */
Datum
numeric_ceil(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	result;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NUMERIC(duplicate_numeric(num));

	init_var_from_num(num, &result);
	ceil_var(&result, &result);

	res = make_result(&result);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_floor() -
 *
 *	返回小于等于参数的最大整数
 */
Datum
numeric_floor(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	result;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NUMERIC(duplicate_numeric(num));

	init_var_from_num(num, &result);
	floor_var(&result, &result);

	res = make_result(&result);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * generate_series_numeric() -
 *
 *	生成 numeric 序列。
 */
Datum
generate_series_numeric(PG_FUNCTION_ARGS)
{
	return generate_series_step_numeric(fcinfo);
}

Datum
generate_series_step_numeric(PG_FUNCTION_ARGS)
{
	generate_series_numeric_fctx *fctx;
	FuncCallContext *funcctx;
	MemoryContext oldcontext;

	if (SRF_IS_FIRSTCALL())
	{
		Numeric		start_num = PG_GETARG_NUMERIC(0);
		Numeric		stop_num = PG_GETARG_NUMERIC(1);
		NumericVar	steploc = const_one;

		/* 拒绝 start 与 stop 值中的 NaN 与无穷大 */
		if (NUMERIC_IS_SPECIAL(start_num))
		{
			if (NUMERIC_IS_NAN(start_num))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("start value cannot be NaN")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("start value cannot be infinity")));
		}
		if (NUMERIC_IS_SPECIAL(stop_num))
		{
			if (NUMERIC_IS_NAN(stop_num))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("stop value cannot be NaN")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("stop value cannot be infinity")));
		}

		/* 看是否给定了显式的步长 */
		if (PG_NARGS() == 3)
		{
			Numeric		step_num = PG_GETARG_NUMERIC(2);

			if (NUMERIC_IS_SPECIAL(step_num))
			{
				if (NUMERIC_IS_NAN(step_num))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("step size cannot be NaN")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("step size cannot be infinity")));
			}

			init_var_from_num(step_num, &steploc);

			if (cmp_var(&steploc, &const_zero) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("step size cannot equal zero")));
		}

		/* 创建跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文。
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为用户上下文分配内存 */
		fctx = (generate_series_numeric_fctx *)
			palloc(sizeof(generate_series_numeric_fctx));

		/*
		 * 用 fctx 在调用间保存状态。用原始起始值初始化
		 * current。必须复制 start_num 与 stop_num 值而非
		 * 指向它们，因为可能已在调用上下文中执行了解
		 * TOAST。
		 */
		init_var(&fctx->current);
		init_var(&fctx->stop);
		init_var(&fctx->step);

		set_var_from_num(start_num, &fctx->current);
		set_var_from_num(stop_num, &fctx->stop);
		set_var_from_var(&steploc, &fctx->step);

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* 函数每次调用都要做的事项 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * 取回保存的状态，把 current 用作本轮迭代结果。
	 *
	 */
	fctx = funcctx->user_fctx;

	if ((fctx->step.sign == NUMERIC_POS &&
		 cmp_var(&fctx->current, &fctx->stop) <= 0) ||
		(fctx->step.sign == NUMERIC_NEG &&
		 cmp_var(&fctx->current, &fctx->stop) >= 0))
	{
		Numeric		result = make_result(&fctx->current);

		/* 切换到适合迭代计算的内存上下文 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 递增 current 为下一次迭代做准备 */
		add_var(&fctx->current, &fctx->step, &fctx->current);
		MemoryContextSwitchTo(oldcontext);

		/* 还有更多值要返回时执行 */
		SRF_RETURN_NEXT(funcctx, NumericGetDatum(result));
	}
	else
		/* 没有更多值时执行 */
		SRF_RETURN_DONE(funcctx);
}


/*
 * 实现 width_bucket() 函数的 numeric 版本，该函数由
 * SQL2003 定义。另见 width_bucket_float8()。
 *
 * 'bound1' 与 'bound2' 为直方图范围的下、上界。
 * 'count' 为直方图桶数。width_bucket() 返回一个整
 * 数，即 'operand' 在指定特征等宽直方图所属的
 * 桶号。小于下界的操作数归入 0 号桶。大于或
 * 等于上界的操作数归入额外的桶（编号为
 * count+1）。任何 numeric 输入都不允许"NaN"，直方图
 * 的两个边界也都不允许为 +/- 无穷大。
 *
 *
 */
Datum
width_bucket_numeric(PG_FUNCTION_ARGS)
{
	Numeric		operand = PG_GETARG_NUMERIC(0);
	Numeric		bound1 = PG_GETARG_NUMERIC(1);
	Numeric		bound2 = PG_GETARG_NUMERIC(2);
	int32		count = PG_GETARG_INT32(3);
	NumericVar	count_var;
	NumericVar	result_var;
	int32		result;

	if (count <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
				 errmsg("count must be greater than zero")));

	if (NUMERIC_IS_SPECIAL(operand) ||
		NUMERIC_IS_SPECIAL(bound1) ||
		NUMERIC_IS_SPECIAL(bound2))
	{
		if (NUMERIC_IS_NAN(operand) ||
			NUMERIC_IS_NAN(bound1) ||
			NUMERIC_IS_NAN(bound2))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
					 errmsg("operand, lower bound, and upper bound cannot be NaN")));
		/* 允许 "operand" 为无穷大；cmp_numerics 能处理 */
		if (NUMERIC_IS_INF(bound1) || NUMERIC_IS_INF(bound2))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
					 errmsg("lower and upper bounds must be finite")));
	}

	init_var(&result_var);
	init_var(&count_var);

	/* 把 'count' 转成 numeric，便于后文使用 */
	int64_to_numericvar((int64) count, &count_var);

	switch (cmp_numerics(bound1, bound2))
	{
		case 0:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
					 errmsg("lower bound cannot equal upper bound")));
			break;

			/* bound1 < bound2 */
		case -1:
			if (cmp_numerics(operand, bound1) < 0)
				set_var_from_var(&const_zero, &result_var);
			else if (cmp_numerics(operand, bound2) >= 0)
				add_var(&count_var, &const_one, &result_var);
			else
				compute_bucket(operand, bound1, bound2, &count_var, false,
							   &result_var);
			break;

			/* bound1 > bound2 */
		case 1:
			if (cmp_numerics(operand, bound1) > 0)
				set_var_from_var(&const_zero, &result_var);
			else if (cmp_numerics(operand, bound2) <= 0)
				add_var(&count_var, &const_one, &result_var);
			else
				compute_bucket(operand, bound1, bound2, &count_var, true,
							   &result_var);
			break;
	}

	/* 若结果超出合法 int4 的范围，则在此 ereport */
	if (!numericvar_to_int32(&result_var, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	free_var(&count_var);
	free_var(&result_var);

	PG_RETURN_INT32(result);
}

/*
 * 'operand' 在桶范围内，故确定其应归入的正确
 * 桶。计算直接源自 SQL2003，注意我们先乘以
 * count 再相除，以避免不必要的舍入误差。
 *
 */
static void
compute_bucket(Numeric operand, Numeric bound1, Numeric bound2,
			   const NumericVar *count_var, bool reversed_bounds,
			   NumericVar *result_var)
{
	NumericVar	bound1_var;
	NumericVar	bound2_var;
	NumericVar	operand_var;

	init_var_from_num(bound1, &bound1_var);
	init_var_from_num(bound2, &bound2_var);
	init_var_from_num(operand, &operand_var);

	if (!reversed_bounds)
	{
		sub_var(&operand_var, &bound1_var, &operand_var);
		sub_var(&bound2_var, &bound1_var, &bound2_var);
	}
	else
	{
		sub_var(&bound1_var, &operand_var, &operand_var);
		sub_var(&bound1_var, &bound2_var, &bound2_var);
	}

	mul_var(&operand_var, count_var, &operand_var,
			operand_var.dscale + count_var->dscale);
	div_var(&operand_var, &bound2_var, result_var,
			select_div_scale(&operand_var, &bound2_var), true);

	/*
	 * 除法的舍入可能给出恰好等于"count"的商，该值过
	 * 大。需截断以使结果不大于"count"。
	 *
	 */
	if (cmp_var(result_var, count_var) >= 0)
		set_var_from_var(count_var, result_var);
	else
	{
		add_var(result_var, &const_one, result_var);
		floor_var(result_var, result_var);
	}

	free_var(&bound1_var);
	free_var(&bound2_var);
	free_var(&operand_var);
}

/* ----------------------------------------------------------------------
 *
 * 比较函数
 *
 * 注意：btree 索引要求这些例程不泄漏内存；因此，请
 * 小心释放已 TOAST 的 datum 的工作副本。大多数地方
 * 无需如此小心。
 *
 * 排序支持
 *
 * 我们实现 sortsupport 策略例程是为了获得缩略的收益。
 * 普通 numeric 比较可能相当慢，原因是 palloc/pfree 循环
 * （为对齐而解 TOAST 打包值）；虽本身可改进，但缩略
 * 策略在许多常见情形下带来更多加速。
 *
 *
 * 缩略形式使用两种不同的表示，一种为 int32，一种为
 * int64，取能放入按值传递 Datum 的那一种。两种情况下
 * 表示都相对原值取负，因为我们把最大负值用于 NaN，
 * 而 NaN 的排序高于其他值。我们把 numeric 的绝对值转
 * 换成 31 位或 63 位的正值，若原数为正则再取负。
 *
 *
 * 若缩略基数低于行数的 0.01%（每 1 万非空行 1 个），
 * 即中止缩略过程。实际盈亏平衡点略低于该值，也许
 * 为每 3 万行 1 个（每 10 万行亏一点），但我们
 * 不想在首次中止测试前积累太多缩略值，故取略显悲
 * 观的数字。我们不去估算真实值的基数，因为它在此处
 * 的代价模型中不起作用（若缩略值相等，底层值相等与
 * 不等的比较代价相当）。若估计基数达到 10 万，我们
 * 乃至不再检查中止（省掉哈希开销）。这已足以支持数
 *
 * 十亿行，且效果不劣于盈亏平衡。
 *
 *
 * ----------------------------------------------------------------------
 */

/*
 * 排序支持策略例程。
 */
Datum
numeric_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = numeric_fast_cmp;

	if (ssup->abbreviate)
	{
		NumericSortSupport *nss;
		MemoryContext oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		nss = palloc(sizeof(NumericSortSupport));

		/*
		 * 额外 palloc 一个缓冲区来处理非对齐打包值，以及
		 * 支持结构。
		 */
		nss->buf = palloc(VARATT_SHORT_MAX + VARHDRSZ + 1);

		nss->input_count = 0;
		nss->estimating = true;
		initHyperLogLog(&nss->abbr_card, 10);

		ssup->ssup_extra = nss;

		ssup->abbrev_full_comparator = ssup->comparator;
		ssup->comparator = numeric_cmp_abbrev;
		ssup->abbrev_converter = numeric_abbrev_convert;
		ssup->abbrev_abort = numeric_abbrev_abort;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * 缩略一个 numeric datum，处理 NaN 与解 TOAST
 * （不得泄漏！）
 */
static Datum
numeric_abbrev_convert(Datum original_datum, SortSupport ssup)
{
	NumericSortSupport *nss = ssup->ssup_extra;
	void	   *original_varatt = PG_DETOAST_DATUM_PACKED(original_datum);
	Numeric		value;
	Datum		result;

	nss->input_count += 1;

	/*
	 * 这是为了处理打包 datum 而无需 palloc/pfree 循环；
	 * 我们保留并复用足以处理任何短 datum 的缓冲区。
	 */
	if (VARATT_IS_SHORT(original_varatt))
	{
		void	   *buf = nss->buf;
		Size		sz = VARSIZE_SHORT(original_varatt) - VARHDRSZ_SHORT;

		Assert(sz <= VARATT_SHORT_MAX - VARHDRSZ_SHORT);

		SET_VARSIZE(buf, VARHDRSZ + sz);
		memcpy(VARDATA(buf), VARDATA_SHORT(original_varatt), sz);

		value = (Numeric) buf;
	}
	else
		value = (Numeric) original_varatt;

	if (NUMERIC_IS_SPECIAL(value))
	{
		if (NUMERIC_IS_PINF(value))
			result = NUMERIC_ABBREV_PINF;
		else if (NUMERIC_IS_NINF(value))
			result = NUMERIC_ABBREV_NINF;
		else
			result = NUMERIC_ABBREV_NAN;
	}
	else
	{
		NumericVar	var;

		init_var_from_num(value, &var);

		result = numeric_abbrev_convert_var(&var, nss);
	}

	/* 应只发生在外部/压缩 TOAST 上 */
	if ((Pointer) original_varatt != DatumGetPointer(original_datum))
		pfree(original_varatt);

	return result;
}

/*
 * 考虑是否中止缩略。
 *
 * 我们不关注非缩略数据的基数。没有理由关注它：与
 * text 不同，我们没有快速检查值相等的方法，因此无论
 * 底层值是否相等，只要缩略值相等就要付出全部开销。
 *
 */
static bool
numeric_abbrev_abort(int memtupcount, SortSupport ssup)
{
	NumericSortSupport *nss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || nss->input_count < 10000 || !nss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&nss->abbr_card);

	/*
	 * 若已有 >100k 个不同值，那么即使要排序数十亿行，
	 * 我们很可能仍然不亏，而撤销这么多行的缩略的代价
	 * 多半也不值得。到该点为止，连计数也停止。
	 *
	 */
	if (abbr_card > 100000.0)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "numeric_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, nss->input_count, memtupcount);
#endif
		nss->estimating = false;
		return false;
	}

	/*
	 * 目标最小基数约为每 1 万非空输入 1 个。（平衡点
	 * 介于每 10 万行 1 个与每 1 万行 1 个之间，前者
	 * 略有微小代价，后者按百分比胜出。）我们采用
	 * 相对悲观的 1 万阈值，并加 0.5 行修正，如此可
	 * 在真正病态的数据上更早中止，即头 1 万非空行
	 * 中恰好只有 1 个缩略值时。
	 *
	 */
	if (abbr_card < nss->input_count / 10000.0 + 0.5)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "numeric_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, nss->input_count / 10000.0 + 0.5,
				 nss->input_count, memtupcount);
#endif
		return true;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "numeric_abbrev: cardinality %f"
			 " after " INT64_FORMAT " values (%d rows)",
			 abbr_card, nss->input_count, memtupcount);
#endif

	return false;
}

/*
 * 比较例程的非 fmgr 接口，让 sortsupport 能省掉 fmgr
 * 调用。鉴于 numeric 比较本身就很慢，这里的节省很小，
 * 但这是执行缩略时排序支持 API 的必备部分。
 *
 *
 * 这里若用持久缓冲区对齐短 varlena 输入，本可再省
 * 去两次 palloc/pfree 循环，但迄今认为不值这番工夫。
 *
 */
static int
numeric_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	Numeric		nx = DatumGetNumeric(x);
	Numeric		ny = DatumGetNumeric(y);
	int			result;

	result = cmp_numerics(nx, ny);

	if ((Pointer) nx != DatumGetPointer(x))
		pfree(nx);
	if ((Pointer) ny != DatumGetPointer(y))
		pfree(ny);

	return result;
}

/*
 * 比较各值的缩略。（真实值不同之处缩略可能相等，但
 * 若缩略不同，则必定反映真实值的排序。）
 *
 */
static int
numeric_cmp_abbrev(Datum x, Datum y, SortSupport ssup)
{
	/*
	 * 特别注意：此处有意反向，因为缩略相对原值取负，
	 * 以处理 NaN/无穷大情形。
	 */
	if (DatumGetNumericAbbrev(x) < DatumGetNumericAbbrev(y))
		return 1;
	if (DatumGetNumericAbbrev(x) > DatumGetNumericAbbrev(y))
		return -1;
	return 0;
}

/*
 * 按可用位长缩略 NumericVar。
 *
 * 31 位值的构造方式如下：
 *
 *	0 + 7bits digit weight + 24 bits digit value
 *
 * 其中数字位权重以单个十进制数字位计，而非数字位字
 * 且以偏移 44 表示[1] 存储。24 位数字位值是该值转换
 * 为二进制后的前 7 个最高有效十进制数字位。权重超出可
 * 表示范围的值会被舍入为零（这也用于表示真正的零）
 * 或舍入为 0x7FFFFFFF（否则不会出现该值）。因此在
 * 10^-44 到 10^83 范围之外的值上，缩略无法获得任何
 * 优势，这不被视为严重的局限；或者当两值量级相同且
 * 前 7 个十进制数字位相等时也是如此，鉴于可用位数，
 * 这被认为是不可避免的局限。（再偷 3 位比较另
 * 一个数字位会把可表示权重的范围缩小到 1/8，这开始
 * 像真正的限制因素了。）
 *
 *
 * （偏移值 44 基本上是任意选定的）
 *
 * 63 位值的构造方式如下：
 *
 *	0 + 7bits weight + 4 x 14-bit packed digit words
 *
 * 此情形下权重同样以偏移 44 存储，但这次存的是以数
 * 字位字为单位的原始权重（即 10000 的幂）。该值的前
 * 四个数字位字（若存在；必要时假定尾随零）各自打包
 * 成 14 位以构成其余部分。同样越界值舍入为 0 或
 * 0x7FFFFFFFFFFFFFFF。此情形下可表示的范围为
 * 10^-176 到 10^332，这被认为足以应对一切实际用途；
 * 而比较 4 个字意味着至少比较 13 个十进制数字位，
 * 这被认为是效果与计算缩略效率之间合理的折中。
 *
 *
 *
 * （偏移值 44 在这里更加随意，选它只是为了与 31 位
 * 情形所用的值一致）
 *
 * [1] - 偏移 k 表示是指给值加上 'k' 形成偏移，然后
 * 按无符号数对待，因此最小可表示值以全零位存储。这
 * 使得简单比较可在复合值上进行。
 *
 */

#if NUMERIC_ABBREV_BITS == 64

static Datum
numeric_abbrev_convert_var(const NumericVar *var, NumericSortSupport *nss)
{
	int			ndigits = var->ndigits;
	int			weight = var->weight;
	int64		result;

	if (ndigits == 0 || weight < -44)
	{
		result = 0;
	}
	else if (weight > 83)
	{
		result = PG_INT64_MAX;
	}
	else
	{
		result = ((int64) (weight + 44) << 56);

		switch (ndigits)
		{
			default:
				result |= ((int64) var->digits[3]);
				/* FALLTHROUGH */
			case 3:
				result |= ((int64) var->digits[2]) << 14;
				/* FALLTHROUGH */
			case 2:
				result |= ((int64) var->digits[1]) << 28;
				/* FALLTHROUGH */
			case 1:
				result |= ((int64) var->digits[0]) << 42;
				break;
		}
	}

	/* 缩略值相对原值取负 */
	if (var->sign == NUMERIC_POS)
		result = -result;

	if (nss->estimating)
	{
		uint32		tmp = ((uint32) result
						   ^ (uint32) ((uint64) result >> 32));

		addHyperLogLog(&nss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	return NumericAbbrevGetDatum(result);
}

#endif							/* NUMERIC_ABBREV_BITS == 64 */

#if NUMERIC_ABBREV_BITS == 32

static Datum
numeric_abbrev_convert_var(const NumericVar *var, NumericSortSupport *nss)
{
	int			ndigits = var->ndigits;
	int			weight = var->weight;
	int32		result;

	if (ndigits == 0 || weight < -11)
	{
		result = 0;
	}
	else if (weight > 20)
	{
		result = PG_INT32_MAX;
	}
	else
	{
		NumericDigit nxt1 = (ndigits > 1) ? var->digits[1] : 0;

		weight = (weight + 11) * 4;

		result = var->digits[0];

		/*
		 * "result" 现有 1 到 4 个非零十进制位。我们打包更
		 * 多数字位凑足 7 个（24 位内最大）。
		 */

		if (result > 999)
		{
			/* 已有 4 位，再加 3 位 */
			result = (result * 1000) + (nxt1 / 10);
			weight += 3;
		}
		else if (result > 99)
		{
			/* 已有 3 位，再加 4 位 */
			result = (result * 10000) + nxt1;
			weight += 2;
		}
		else if (result > 9)
		{
			NumericDigit nxt2 = (ndigits > 2) ? var->digits[2] : 0;

			/* 已有 2 位，再加 5 位 */
			result = (result * 100000) + (nxt1 * 10) + (nxt2 / 1000);
			weight += 1;
		}
		else
		{
			NumericDigit nxt2 = (ndigits > 2) ? var->digits[2] : 0;

			/* 已有 1 位，再加 6 位 */
			result = (result * 1000000) + (nxt1 * 100) + (nxt2 / 100);
		}

		result = result | (weight << 24);
	}

	/* 缩略值相对原值取负 */
	if (var->sign == NUMERIC_POS)
		result = -result;

	if (nss->estimating)
	{
		uint32		tmp = (uint32) result;

		addHyperLogLog(&nss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	return NumericAbbrevGetDatum(result);
}

#endif							/* NUMERIC_ABBREV_BITS == 32 */

/*
 * 下面是普通（非 sortsupport）比较。
 */

Datum
numeric_cmp(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	int			result;

	result = cmp_numerics(num1, num2);

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_INT32(result);
}


Datum
numeric_eq(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) == 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_ne(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) != 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_gt(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) > 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_ge(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) >= 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_lt(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) < 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_le(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) <= 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

static int
cmp_numerics(Numeric num1, Numeric num2)
{
	int			result;

	/*
	 * 我们视所有 NaN 相等且大于任何非 NaN（含无穷）。
	 * 这有些任意；关键是要有一致的排序顺序。
	 *
	 */
	if (NUMERIC_IS_SPECIAL(num1))
	{
		if (NUMERIC_IS_NAN(num1))
		{
			if (NUMERIC_IS_NAN(num2))
				result = 0;		/* NAN = NAN */
			else
				result = 1;		/* NAN > non-NAN */
		}
		else if (NUMERIC_IS_PINF(num1))
		{
			if (NUMERIC_IS_NAN(num2))
				result = -1;	/* PINF < NAN */
			else if (NUMERIC_IS_PINF(num2))
				result = 0;		/* PINF = PINF */
			else
				result = 1;		/* PINF > 其他一切 */
		}
		else					/* num1 必为 NINF */
		{
			if (NUMERIC_IS_NINF(num2))
				result = 0;		/* NINF = NINF */
			else
				result = -1;	/* NINF < 其他一切 */
		}
	}
	else if (NUMERIC_IS_SPECIAL(num2))
	{
		if (NUMERIC_IS_NINF(num2))
			result = 1;			/* 正常 > NINF */
		else
			result = -1;		/* 正常 < NAN/PINF */
	}
	else
	{
		result = cmp_var_common(NUMERIC_DIGITS(num1), NUMERIC_NDIGITS(num1),
								NUMERIC_WEIGHT(num1), NUMERIC_SIGN(num1),
								NUMERIC_DIGITS(num2), NUMERIC_NDIGITS(num2),
								NUMERIC_WEIGHT(num2), NUMERIC_SIGN(num2));
	}

	return result;
}

/*
 * numeric 的 in_range 支持函数。
 */
Datum
in_range_numeric_numeric(PG_FUNCTION_ARGS)
{
	Numeric		val = PG_GETARG_NUMERIC(0);
	Numeric		base = PG_GETARG_NUMERIC(1);
	Numeric		offset = PG_GETARG_NUMERIC(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	bool		result;

	/*
	 * 拒绝负（含 -Inf）或 NaN 偏移量。负值系规范所定，
	 * 而拒绝 NaN 是因为其合适语义并不明显。
	 */
	if (NUMERIC_IS_NAN(offset) ||
		NUMERIC_IS_NINF(offset) ||
		NUMERIC_SIGN(offset) == NUMERIC_NEG)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * 处理 val 和/或 base 为 NaN 的情形，遵循 NaN 排在非
	 * NaN 之后的规则（参见 cmp_numerics）。偏移量不影响
	 * 结论。
	 */
	if (NUMERIC_IS_NAN(val))
	{
		if (NUMERIC_IS_NAN(base))
			result = true;		/* NAN = NAN */
		else
			result = !less;		/* NAN > non-NAN */
	}
	else if (NUMERIC_IS_NAN(base))
	{
		result = less;			/* non-NAN < NAN */
	}

	/*
	 * 处理无穷偏移量（此时必为 +Inf）。
	 */
	else if (NUMERIC_IS_SPECIAL(offset))
	{
		Assert(NUMERIC_IS_PINF(offset));
		if (sub ? NUMERIC_IS_PINF(base) : NUMERIC_IS_NINF(base))
		{
			/*
			 * base +/- offset 会得 NaN，故对任何 val 都返回 true
			 * （理由见 in_range_float8_float8()）。
			 */
			result = true;
		}
		else if (sub)
		{
			/* base - offset 必为 -inf */
			if (less)
				result = NUMERIC_IS_NINF(val);	/* 仅 -inf <= 和 */
			else
				result = true;	/* 任何 val >= 和 */
		}
		else
		{
			/* base + offset 必为 +inf */
			if (less)
				result = true;	/* 任何 val <= 和 */
			else
				result = NUMERIC_IS_PINF(val);	/* 仅 +inf >= 和 */
		}
	}

	/*
	 * 处理 val 和/或 base 为无穷的情形。偏移量此时已知
	 * 为有限，不会影响结论。
	 */
	else if (NUMERIC_IS_SPECIAL(val))
	{
		if (NUMERIC_IS_PINF(val))
		{
			if (NUMERIC_IS_PINF(base))
				result = true;	/* PINF = PINF */
			else
				result = !less; /* PINF > 其他非 NAN */
		}
		else					/* val 必为 NINF */
		{
			if (NUMERIC_IS_NINF(base))
				result = true;	/* NINF = NINF */
			else
				result = less;	/* NINF < 其他一切 */
		}
	}
	else if (NUMERIC_IS_SPECIAL(base))
	{
		if (NUMERIC_IS_NINF(base))
			result = !less;		/* 正常 > NINF */
		else
			result = less;		/* 正常 < PINF */
	}
	else
	{
		/*
		 * 否则就直接计算 base +/- offset。虽然可能超出
		 * numeric 范围，可能性小到我们无需防范。
		 *
		 */
		NumericVar	valv;
		NumericVar	basev;
		NumericVar	offsetv;
		NumericVar	sum;

		init_var_from_num(val, &valv);
		init_var_from_num(base, &basev);
		init_var_from_num(offset, &offsetv);
		init_var(&sum);

		if (sub)
			sub_var(&basev, &offsetv, &sum);
		else
			add_var(&basev, &offsetv, &sum);

		if (less)
			result = (cmp_var(&valv, &sum) <= 0);
		else
			result = (cmp_var(&valv, &sum) >= 0);

		free_var(&sum);
	}

	PG_FREE_IF_COPY(val, 0);
	PG_FREE_IF_COPY(base, 1);
	PG_FREE_IF_COPY(offset, 2);

	PG_RETURN_BOOL(result);
}

Datum
hash_numeric(PG_FUNCTION_ARGS)
{
	Numeric		key = PG_GETARG_NUMERIC(0);
	Datum		digit_hash;
	Datum		result;
	int			weight;
	int			start_offset;
	int			end_offset;
	int			i;
	int			hash_len;
	NumericDigit *digits;

	/* 若是 NaN 或无穷大，就不要对其余字段做哈希 */
	if (NUMERIC_IS_SPECIAL(key))
		PG_RETURN_UINT32(0);

	weight = NUMERIC_WEIGHT(key);
	start_offset = 0;
	end_offset = 0;

	/*
	 * 从哈希输入略去前导与尾随零。numeric 本应保证前
	 * 导与尾随零被抑制，但我们不惮以最坏打算。
	 * 注意我们度量起止偏移时以 NumericDigit 为单位，
	 * 而非字节。
	 */
	digits = NUMERIC_DIGITS(key);
	for (i = 0; i < NUMERIC_NDIGITS(key); i++)
	{
		if (digits[i] != (NumericDigit) 0)
			break;

		start_offset++;

		/*
		 * 权重实为小数点前的数字位数，每跳过一个前导零
		 * 就将其减 1。
		 */
		weight--;
	}

	/*
	 * 若没有任何非零数字位，则数值就是零，与其余字段
	 * 无关。
	 */
	if (NUMERIC_NDIGITS(key) == start_offset)
		PG_RETURN_UINT32(-1);

	for (i = NUMERIC_NDIGITS(key) - 1; i >= 0; i--)
	{
		if (digits[i] != (NumericDigit) 0)
			break;

		end_offset++;
	}

	/* 走到这里时，应当至少有一个非零数字位 */
	Assert(start_offset + end_offset < NUMERIC_NDIGITS(key));

	/*
	 * 注意我们不对 numeric 的标度做哈希，因为两个数值可
	 * 能相等而标度不同。也不对符号哈希，虽然本可这
	 * 样：符号相异即不等，故这不应影响正确性。
	 *
	 */
	hash_len = NUMERIC_NDIGITS(key) - start_offset - end_offset;
	digit_hash = hash_any((unsigned char *) (NUMERIC_DIGITS(key) + start_offset),
						  hash_len * sizeof(NumericDigit));

	/* 以 XOR 混入权重 */
	result = digit_hash ^ weight;

	PG_RETURN_DATUM(result);
}

/*
 * 通过带种子把值哈希为 64 位值来返回 64 位值。
 * 其余与 hash_numeric 相似。
 */
Datum
hash_numeric_extended(PG_FUNCTION_ARGS)
{
	Numeric		key = PG_GETARG_NUMERIC(0);
	uint64		seed = PG_GETARG_INT64(1);
	Datum		digit_hash;
	Datum		result;
	int			weight;
	int			start_offset;
	int			end_offset;
	int			i;
	int			hash_len;
	NumericDigit *digits;

	/* 若是 NaN 或无穷大，就不要对其余字段做哈希 */
	if (NUMERIC_IS_SPECIAL(key))
		PG_RETURN_UINT64(seed);

	weight = NUMERIC_WEIGHT(key);
	start_offset = 0;
	end_offset = 0;

	digits = NUMERIC_DIGITS(key);
	for (i = 0; i < NUMERIC_NDIGITS(key); i++)
	{
		if (digits[i] != (NumericDigit) 0)
			break;

		start_offset++;

		weight--;
	}

	if (NUMERIC_NDIGITS(key) == start_offset)
		PG_RETURN_UINT64(seed - 1);

	for (i = NUMERIC_NDIGITS(key) - 1; i >= 0; i--)
	{
		if (digits[i] != (NumericDigit) 0)
			break;

		end_offset++;
	}

	Assert(start_offset + end_offset < NUMERIC_NDIGITS(key));

	hash_len = NUMERIC_NDIGITS(key) - start_offset - end_offset;
	digit_hash = hash_any_extended((unsigned char *) (NUMERIC_DIGITS(key)
													  + start_offset),
								   hash_len * sizeof(NumericDigit),
								   seed);

	result = UInt64GetDatum(DatumGetUInt64(digit_hash) ^ weight);

	PG_RETURN_DATUM(result);
}


/* ----------------------------------------------------------------------
 *
 * 基本算术函数
 *
 * ----------------------------------------------------------------------
 */


/*
 * numeric_add() -
 *
 *	两数值相加
 */
Datum
numeric_add(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;

	res = numeric_add_opt_error(num1, num2, NULL);

	PG_RETURN_NUMERIC(res);
}

/*
 * numeric_add_opt_error() -
 *
 *	numeric_add() 的内部版本。若提供了"*have_error"标志，
 *	出错时置为 true 并返回 NULL。调用者需要自行处理错
 *	误时会有帮助。
 */
Numeric
numeric_add_opt_error(Numeric num1, Numeric num2, bool *have_error)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
			return make_result(&const_nan);
		if (NUMERIC_IS_PINF(num1))
		{
			if (NUMERIC_IS_NINF(num2))
				return make_result(&const_nan); /* Inf + -Inf */
			else
				return make_result(&const_pinf);
		}
		if (NUMERIC_IS_NINF(num1))
		{
			if (NUMERIC_IS_PINF(num2))
				return make_result(&const_nan); /* -Inf + Inf */
			else
				return make_result(&const_ninf);
		}
		/* 此处 num1 必为有限，num2 则不是 */
		if (NUMERIC_IS_PINF(num2))
			return make_result(&const_pinf);
		Assert(NUMERIC_IS_NINF(num2));
		return make_result(&const_ninf);
	}

	/*
	 * 解包各值，让 add_var() 计算结果并返回它。
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);
	add_var(&arg1, &arg2, &result);

	res = make_result_opt_error(&result, have_error);

	free_var(&result);

	return res;
}


/*
 * numeric_sub() -
 *
 *	从一个数值中减去另一个
 */
Datum
numeric_sub(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;

	res = numeric_sub_opt_error(num1, num2, NULL);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_sub_opt_error() -
 *
 *	numeric_sub() 的内部版本。若提供了"*have_error"标志，
 *	出错时置为 true 并返回 NULL。调用者需要自行处理错
 *	误时会有帮助。
 */
Numeric
numeric_sub_opt_error(Numeric num1, Numeric num2, bool *have_error)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
			return make_result(&const_nan);
		if (NUMERIC_IS_PINF(num1))
		{
			if (NUMERIC_IS_PINF(num2))
				return make_result(&const_nan); /* Inf - Inf */
			else
				return make_result(&const_pinf);
		}
		if (NUMERIC_IS_NINF(num1))
		{
			if (NUMERIC_IS_NINF(num2))
				return make_result(&const_nan); /* -Inf - -Inf */
			else
				return make_result(&const_ninf);
		}
		/* 此处 num1 必为有限，num2 则不是 */
		if (NUMERIC_IS_PINF(num2))
			return make_result(&const_ninf);
		Assert(NUMERIC_IS_NINF(num2));
		return make_result(&const_pinf);
	}

	/*
	 * 解包各值，让 sub_var() 计算结果并返回它。
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);
	sub_var(&arg1, &arg2, &result);

	res = make_result_opt_error(&result, have_error);

	free_var(&result);

	return res;
}


/*
 * numeric_mul() -
 *
 *	计算两个数值的乘积
 */
Datum
numeric_mul(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;

	res = numeric_mul_opt_error(num1, num2, NULL);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_mul_opt_error() -
 *
 *	numeric_mul() 的内部版本。若提供了"*have_error"标志，
 *	出错时置为 true 并返回 NULL。调用者需要自行处理错
 *	误时会有帮助。
 */
Numeric
numeric_mul_opt_error(Numeric num1, Numeric num2, bool *have_error)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
			return make_result(&const_nan);
		if (NUMERIC_IS_PINF(num1))
		{
			switch (numeric_sign_internal(num2))
			{
				case 0:
					return make_result(&const_nan); /* Inf * 0 */
				case 1:
					return make_result(&const_pinf);
				case -1:
					return make_result(&const_ninf);
			}
			Assert(false);
		}
		if (NUMERIC_IS_NINF(num1))
		{
			switch (numeric_sign_internal(num2))
			{
				case 0:
					return make_result(&const_nan); /* -Inf * 0 */
				case 1:
					return make_result(&const_ninf);
				case -1:
					return make_result(&const_pinf);
			}
			Assert(false);
		}
		/* 此处 num1 必为有限，num2 则不是 */
		if (NUMERIC_IS_PINF(num2))
		{
			switch (numeric_sign_internal(num1))
			{
				case 0:
					return make_result(&const_nan); /* 0 * Inf */
				case 1:
					return make_result(&const_pinf);
				case -1:
					return make_result(&const_ninf);
			}
			Assert(false);
		}
		Assert(NUMERIC_IS_NINF(num2));
		switch (numeric_sign_internal(num1))
		{
			case 0:
				return make_result(&const_nan); /* 0 * -Inf */
			case 1:
				return make_result(&const_ninf);
			case -1:
				return make_result(&const_pinf);
		}
		Assert(false);
	}

	/*
	 * 解包各值，让 mul_var() 计算结果并返回它。
	 * 与 add_var() 和 sub_var() 不同，mul_var() 会对结果
	 * 四舍五入。numeric_mul() 是为对数值使用的 * 算子而
	 * 调用的，我们要求乘积的精确表示（rscale = arg1 的
	 * dscale 与 arg2 的 dscale 之和）。若精确结果小数点后
	 * 的数字位多于 numeric 能存储的数量，我们就将其四舍
	 * 五入。先算精确值再舍入，可确保结果被正确
	 * 舍入（mul_var() 中以截断乘积舍入则无法保证这一
	 * 点）。
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);
	mul_var(&arg1, &arg2, &result, arg1.dscale + arg2.dscale);

	if (result.dscale > NUMERIC_DSCALE_MAX)
		round_var(&result, NUMERIC_DSCALE_MAX);

	res = make_result_opt_error(&result, have_error);

	free_var(&result);

	return res;
}


/*
 * numeric_div() -
 *
 *	用一个数值除以另一个
 */
Datum
numeric_div(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;

	res = numeric_div_opt_error(num1, num2, NULL);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_div_opt_error() -
 *
 *	numeric_div() 的内部版本。若提供了"*have_error"标志，
 *	出错时置为 true 并返回 NULL。调用者需要自行处理错
 *	误时会有帮助。
 */
Numeric
numeric_div_opt_error(Numeric num1, Numeric num2, bool *have_error)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;
	int			rscale;

	if (have_error)
		*have_error = false;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
			return make_result(&const_nan);
		if (NUMERIC_IS_PINF(num1))
		{
			if (NUMERIC_IS_SPECIAL(num2))
				return make_result(&const_nan); /* Inf / [-]Inf */
			switch (numeric_sign_internal(num2))
			{
				case 0:
					if (have_error)
					{
						*have_error = true;
						return NULL;
					}
					ereport(ERROR,
							(errcode(ERRCODE_DIVISION_BY_ZERO),
							 errmsg("division by zero")));
					break;
				case 1:
					return make_result(&const_pinf);
				case -1:
					return make_result(&const_ninf);
			}
			Assert(false);
		}
		if (NUMERIC_IS_NINF(num1))
		{
			if (NUMERIC_IS_SPECIAL(num2))
				return make_result(&const_nan); /* -Inf / [-]Inf */
			switch (numeric_sign_internal(num2))
			{
				case 0:
					if (have_error)
					{
						*have_error = true;
						return NULL;
					}
					ereport(ERROR,
							(errcode(ERRCODE_DIVISION_BY_ZERO),
							 errmsg("division by zero")));
					break;
				case 1:
					return make_result(&const_ninf);
				case -1:
					return make_result(&const_pinf);
			}
			Assert(false);
		}
		/* 此处 num1 必为有限，num2 则不是 */

		/*
		 * 按 POSIX，若 num1 为零应返回零或负零，否则应抛出
		 * 下溢错误。但 numeric 类型其实不会下溢，故直接
		 * 回零。
		 */
		return make_result(&const_zero);
	}

	/*
	 * 解包各参数
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);

	/*
	 * 为除法结果选择标度
	 */
	rscale = select_div_scale(&arg1, &arg2);

	/*
	 * 若提供了"have_error"，则在此检查除零
	 */
	if (have_error && (arg2.ndigits == 0 || arg2.digits[0] == 0))
	{
		*have_error = true;
		return NULL;
	}

	/*
	 * 执行除法并返回结果
	 */
	div_var(&arg1, &arg2, &result, rscale, true);

	res = make_result_opt_error(&result, have_error);

	free_var(&result);

	return res;
}


/*
 * numeric_div_trunc() -
 *
 *	用一个数值除以另一个，并将结果截断为整数
 */
Datum
numeric_div_trunc(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
			PG_RETURN_NUMERIC(make_result(&const_nan));
		if (NUMERIC_IS_PINF(num1))
		{
			if (NUMERIC_IS_SPECIAL(num2))
				PG_RETURN_NUMERIC(make_result(&const_nan)); /* Inf / [-]Inf */
			switch (numeric_sign_internal(num2))
			{
				case 0:
					ereport(ERROR,
							(errcode(ERRCODE_DIVISION_BY_ZERO),
							 errmsg("division by zero")));
					break;
				case 1:
					PG_RETURN_NUMERIC(make_result(&const_pinf));
				case -1:
					PG_RETURN_NUMERIC(make_result(&const_ninf));
			}
			Assert(false);
		}
		if (NUMERIC_IS_NINF(num1))
		{
			if (NUMERIC_IS_SPECIAL(num2))
				PG_RETURN_NUMERIC(make_result(&const_nan)); /* -Inf / [-]Inf */
			switch (numeric_sign_internal(num2))
			{
				case 0:
					ereport(ERROR,
							(errcode(ERRCODE_DIVISION_BY_ZERO),
							 errmsg("division by zero")));
					break;
				case 1:
					PG_RETURN_NUMERIC(make_result(&const_ninf));
				case -1:
					PG_RETURN_NUMERIC(make_result(&const_pinf));
			}
			Assert(false);
		}
		/* 此处 num1 必为有限，num2 则不是 */

		/*
		 * 按 POSIX，若 num1 为零应返回零或负零，否则应抛出
		 * 下溢错误。但 numeric 类型其实不会下溢，故直接
		 * 回零。
		 */
		PG_RETURN_NUMERIC(make_result(&const_zero));
	}

	/*
	 * 解包各参数
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);

	/*
	 * 执行除法并返回结果
	 */
	div_var(&arg1, &arg2, &result, 0, false);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_mod() -
 *
 *	计算两个数值的模
 */
Datum
numeric_mod(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;

	res = numeric_mod_opt_error(num1, num2, NULL);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_mod_opt_error() -
 *
 *	numeric_mod() 的内部版本。若提供了"*have_error"标志，
 *	出错时置为 true 并返回 NULL。调用者需要自行处理错
 *	误时会有帮助。
 */
Numeric
numeric_mod_opt_error(Numeric num1, Numeric num2, bool *have_error)
{
	Numeric		res;
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;

	if (have_error)
		*have_error = false;

	/*
	 * 处理 NaN 与无穷大。此处遵循 POSIX fmod()，但有一点
	 * 例外：POSIX 对 x 为无穷与 y 为零同等对待，置 EDOM
	 * 并返回 NaN。我们只选择在 y 为零时报错。
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
			return make_result(&const_nan);
		if (NUMERIC_IS_INF(num1))
		{
			if (numeric_sign_internal(num2) == 0)
			{
				if (have_error)
				{
					*have_error = true;
					return NULL;
				}
				ereport(ERROR,
						(errcode(ERRCODE_DIVISION_BY_ZERO),
						 errmsg("division by zero")));
			}
			/* Inf % any nonzero = NaN */
			return make_result(&const_nan);
		}
		/* num2 必为 [-]Inf；无论 num2 符号如何结果都是 num1 */
		return duplicate_numeric(num1);
	}

	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);

	/*
	 * 若提供了"have_error"，则在此检查除零
	 */
	if (have_error && (arg2.ndigits == 0 || arg2.digits[0] == 0))
	{
		*have_error = true;
		return NULL;
	}

	mod_var(&arg1, &arg2, &result);

	res = make_result_opt_error(&result, NULL);

	free_var(&result);

	return res;
}


/*
 * numeric_inc() -
 *
 *	将数值加一
 */
Datum
numeric_inc(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	arg;
	Numeric		res;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NUMERIC(duplicate_numeric(num));

	/*
	 * 计算结果并返回它
	 */
	init_var_from_num(num, &arg);

	add_var(&arg, &const_one, &arg);

	res = make_result(&arg);

	free_var(&arg);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_smaller() -
 *
 *	返回两数中较小者
 */
Datum
numeric_smaller(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);

	/*
	 * 使用 cmp_numerics，使其与比较算子的结果一致，尤其
	 * 是涉及 NaN 的比较。
	 */
	if (cmp_numerics(num1, num2) < 0)
		PG_RETURN_NUMERIC(num1);
	else
		PG_RETURN_NUMERIC(num2);
}


/*
 * numeric_larger() -
 *
 *	返回两数中较大者
 */
Datum
numeric_larger(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);

	/*
	 * 使用 cmp_numerics，使其与比较算子的结果一致，尤其
	 * 是涉及 NaN 的比较。
	 */
	if (cmp_numerics(num1, num2) > 0)
		PG_RETURN_NUMERIC(num1);
	else
		PG_RETURN_NUMERIC(num2);
}


/* ----------------------------------------------------------------------
 *
 * 高等数学函数
 *
 * ----------------------------------------------------------------------
 */

/*
 * numeric_gcd() -
 *
 *	计算两个数值的最大公约数
 */
Datum
numeric_gcd(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * 处理 NaN 与无穷大：这类情形的结果我们都视为
	 * NaN。
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * 解包各参数
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);

	/*
	 * 求出 GCD 并返回结果
	 */
	gcd_var(&arg1, &arg2, &result);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_lcm() -
 *
 *	计算两个数值的最小公倍数
 */
Datum
numeric_lcm(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * 处理 NaN 与无穷大：这类情形的结果我们都视为
	 * NaN。
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * 解包各参数
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	init_var(&result);

	/*
	 * 用 lcm(x, y) = abs(x / gcd(x, y) * y) 计算结果，若
	 * 输入为零则返回零。
	 *
	 * 注意该除法保证是精确的，会返回整数结果，因此 LCM
	 * 是 x 和 y 的整数倍。显示标度取 Min(x.dscale,
	 * y.dscale) 本足以表示它，但与其他 numeric 函数一样，
	 * 我们选择返回显示标度不小于任一输入的结果。
	 *
	 */
	if (arg1.ndigits == 0 || arg2.ndigits == 0)
		set_var_from_var(&const_zero, &result);
	else
	{
		gcd_var(&arg1, &arg2, &result);
		div_var(&arg1, &result, &result, 0, false);
		mul_var(&arg2, &result, &result, arg2.dscale);
		result.sign = NUMERIC_POS;
	}

	result.dscale = Max(arg1.dscale, arg2.dscale);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_fac()
 *
 * 计算阶乘
 */
Datum
numeric_fac(PG_FUNCTION_ARGS)
{
	int64		num = PG_GETARG_INT64(0);
	Numeric		res;
	NumericVar	fact;
	NumericVar	result;

	if (num < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("factorial of a negative number is undefined")));
	if (num <= 1)
	{
		res = make_result(&const_one);
		PG_RETURN_NUMERIC(res);
	}
	/* 若结果将溢出则立即失败 */
	if (num > 32177)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value overflows numeric format")));

	init_var(&fact);
	init_var(&result);

	int64_to_numericvar(num, &result);

	for (num = num - 1; num > 1; num--)
	{
		/* 此循环可能耗时较久，故允许它被中断 */
		CHECK_FOR_INTERRUPTS();

		int64_to_numericvar(num, &fact);

		mul_var(&result, &fact, &result, 0);
	}

	res = make_result(&result);

	free_var(&fact);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_sqrt() -
 *
 *	计算 numeric 的平方根。
 */
Datum
numeric_sqrt(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	arg;
	NumericVar	result;
	int			sweight;
	int			rscale;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		/* 错误应与 sqrt_var() 中的一致 */
		if (NUMERIC_IS_NINF(num))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
					 errmsg("cannot take square root of a negative number")));
		/* 对 NAN 或 PINF，直接复制输入 */
		PG_RETURN_NUMERIC(duplicate_numeric(num));
	}

	/*
	 * 解包参数并确定结果标度。所选标度至少要给出
	 * NUMERIC_MIN_SIG_DIGITS 个有效数字；但任何情况下都
	 * 不低于输入的 dscale。
	 */
	init_var_from_num(num, &arg);

	init_var(&result);

	/*
	 * 假定输入已规范化，故 arg.weight 是准确的。于是结果
	 * 至少有 sweight = floor(arg.weight * DEC_DIGITS / 2
	 * + 1) 个小数点前数字位。当 DEC_DIGITS 为偶数时可以省
	 * 下几个循环，因为除法是精确的，无需向负无穷
	 * 取整。
	 */
#if DEC_DIGITS == ((DEC_DIGITS / 2) * 2)
	sweight = arg.weight * DEC_DIGITS / 2 + 1;
#else
	if (arg.weight >= 0)
		sweight = arg.weight * DEC_DIGITS / 2 + 1;
	else
		sweight = 1 - (1 - arg.weight * DEC_DIGITS) / 2;
#endif

	rscale = NUMERIC_MIN_SIG_DIGITS - sweight;
	rscale = Max(rscale, arg.dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/*
	 * 让 sqrt_var() 执行计算并返回结果。
	 */
	sqrt_var(&arg, &result, rscale);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_exp() -
 *
 *	求 e 的 x 次幂
 */
Datum
numeric_exp(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	arg;
	NumericVar	result;
	int			rscale;
	double		val;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		/* 按 POSIX，exp(-Inf) 为零 */
		if (NUMERIC_IS_NINF(num))
			PG_RETURN_NUMERIC(make_result(&const_zero));
		/* 对 NAN 或 PINF，直接复制输入 */
		PG_RETURN_NUMERIC(duplicate_numeric(num));
	}

	/*
	 * 解包参数并确定结果标度。所选标度至少要给出
	 * NUMERIC_MIN_SIG_DIGITS 个有效数字；但任何情况下都
	 * 不低于输入的 dscale。
	 */
	init_var_from_num(num, &arg);

	init_var(&result);

	/* 把输入转成 float8，忽略溢出 */
	val = numericvar_to_double_no_overflow(&arg);

	/*
	 * log10(result) = num * log10(e)，所以这近似是结果的
	 * 十进制权重：
	 */
	val *= 0.434294481903252;

	/* 限制到不会导致整数溢出的值 */
	val = Max(val, -NUMERIC_MAX_RESULT_SCALE);
	val = Min(val, NUMERIC_MAX_RESULT_SCALE);

	rscale = NUMERIC_MIN_SIG_DIGITS - (int) val;
	rscale = Max(rscale, arg.dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/*
	 * 让 exp_var() 执行计算并返回结果。
	 */
	exp_var(&arg, &result, rscale);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_ln() -
 *
 *	计算 x 的自然对数
 */
Datum
numeric_ln(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	arg;
	NumericVar	result;
	int			ln_dweight;
	int			rscale;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_NINF(num))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
					 errmsg("cannot take logarithm of a negative number")));
		/* 对 NAN 或 PINF，直接复制输入 */
		PG_RETURN_NUMERIC(duplicate_numeric(num));
	}

	init_var_from_num(num, &arg);
	init_var(&result);

	/* 对数的估算 dweight */
	ln_dweight = estimate_ln_dweight(&arg);

	rscale = NUMERIC_MIN_SIG_DIGITS - ln_dweight;
	rscale = Max(rscale, arg.dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	ln_var(&arg, &result, rscale);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_log() -
 *
 *	计算 x 以给定底数的对数
 */
Datum
numeric_log(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		int			sign1,
					sign2;

		if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
			PG_RETURN_NUMERIC(make_result(&const_nan));
		/* 对含 -Inf 的负输入报错，如同 log_var 那样 */
		sign1 = numeric_sign_internal(num1);
		sign2 = numeric_sign_internal(num2);
		if (sign1 < 0 || sign2 < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
					 errmsg("cannot take logarithm of a negative number")));
		/* 对零输入报错，如 log_var */
		if (sign1 == 0 || sign2 == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
					 errmsg("cannot take logarithm of zero")));
		if (NUMERIC_IS_PINF(num1))
		{
			/* log(Inf, Inf) 归约为 Inf/Inf，故为 NaN */
			if (NUMERIC_IS_PINF(num2))
				PG_RETURN_NUMERIC(make_result(&const_nan));
			/* log(Inf, 有限正值) 为零（我们不抛下溢） */
			PG_RETURN_NUMERIC(make_result(&const_zero));
		}
		Assert(NUMERIC_IS_PINF(num2));
		/* log(有限正值, Inf) 为 Inf */
		PG_RETURN_NUMERIC(make_result(&const_pinf));
	}

	/*
	 * 初始化各量
	 */
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);
	init_var(&result);

	/*
	 * 调用 log_var() 计算并返回结果；注意它自行处理标度
	 * 选择。
	 */
	log_var(&arg1, &arg2, &result);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/*
 * numeric_power() -
 *
 *	求 x 的 y 次幂
 */
Datum
numeric_power(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	int			sign1,
				sign2;

	/*
	 * 处理 NaN 与无穷大
	 */
	if (NUMERIC_IS_SPECIAL(num1) || NUMERIC_IS_SPECIAL(num2))
	{
		/*
		 * 我们遵循 POSIX pow(3) 规范：NaN ^ 0 = 1，
		 * 1 ^ NaN = 1，而其他含 NaN 输入的情形都得 NaN
		 * （无错）。
		 */
		if (NUMERIC_IS_NAN(num1))
		{
			if (!NUMERIC_IS_SPECIAL(num2))
			{
				init_var_from_num(num2, &arg2);
				if (cmp_var(&arg2, &const_zero) == 0)
					PG_RETURN_NUMERIC(make_result(&const_one));
			}
			PG_RETURN_NUMERIC(make_result(&const_nan));
		}
		if (NUMERIC_IS_NAN(num2))
		{
			if (!NUMERIC_IS_SPECIAL(num1))
			{
				init_var_from_num(num1, &arg1);
				if (cmp_var(&arg1, &const_one) == 0)
					PG_RETURN_NUMERIC(make_result(&const_one));
			}
			PG_RETURN_NUMERIC(make_result(&const_nan));
		}
		/* 至少一输入为无穷大，错误规则仍适用 */
		sign1 = numeric_sign_internal(num1);
		sign2 = numeric_sign_internal(num2);
		if (sign1 == 0 && sign2 < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
					 errmsg("zero raised to a negative power is undefined")));
		if (sign1 < 0 && !numeric_is_integral(num2))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
					 errmsg("a negative number raised to a non-integer power yields a complex result")));

		/*
		 * POSIX 对含无穷输入的 pow(3) 给出如下一列规则：
		 *
		 * 对任意 y 值，若 x 为 +1，应返回 1.0。
		 */
		if (!NUMERIC_IS_SPECIAL(num1))
		{
			init_var_from_num(num1, &arg1);
			if (cmp_var(&arg1, &const_one) == 0)
				PG_RETURN_NUMERIC(make_result(&const_one));
		}

		/*
		 * 对任意 x 值，若 y 为 [-]0，应返回 1.0。
		 */
		if (sign2 == 0)
			PG_RETURN_NUMERIC(make_result(&const_one));

		/*
		 * 对 y > 0 的任意奇整数，若 x 为 [-]0，应返回
		 * [-]0。对 y > 0 且非奇整数的，若 x 为 [-]0，应返回
		 * +0。（我们不处理负零，无需区分两情形。）
		 *
		 */
		if (sign1 == 0 && sign2 > 0)
			PG_RETURN_NUMERIC(make_result(&const_zero));

		/*
		 * 若 x 为 -1 且 y 为 [-]Inf，应返回 1.0。
		 *
		 * 若 |x| < 1 且 y 为 -Inf，应返回 +Inf。
		 *
		 * 若 |x| > 1 且 y 为 -Inf，应返回 +0。
		 *
		 * 若 |x| < 1 且 y 为 +Inf，应返回 +0。
		 *
		 * 若 |x| > 1 且 y 为 +Inf，应返回 +Inf。
		 */
		if (NUMERIC_IS_INF(num2))
		{
			bool		abs_x_gt_one;

			if (NUMERIC_IS_SPECIAL(num1))
				abs_x_gt_one = true;	/* x 为 Inf 或 -Inf */
			else
			{
				init_var_from_num(num1, &arg1);
				if (cmp_var(&arg1, &const_minus_one) == 0)
					PG_RETURN_NUMERIC(make_result(&const_one));
				arg1.sign = NUMERIC_POS;	/* 即 arg1 = abs(x) */
				abs_x_gt_one = (cmp_var(&arg1, &const_one) > 0);
			}
			if (abs_x_gt_one == (sign2 > 0))
				PG_RETURN_NUMERIC(make_result(&const_pinf));
			else
				PG_RETURN_NUMERIC(make_result(&const_zero));
		}

		/*
		 * 若 y < 0 且 x 为 +Inf，应返回 +0。
		 *
		 * 若 y > 0 且 x 为 +Inf，应返回 +Inf。
		 */
		if (NUMERIC_IS_PINF(num1))
		{
			if (sign2 > 0)
				PG_RETURN_NUMERIC(make_result(&const_pinf));
			else
				PG_RETURN_NUMERIC(make_result(&const_zero));
		}

		Assert(NUMERIC_IS_NINF(num1));

		/*
		 * 若 y 为 < 0 的奇整数且 x 为 -Inf，应返回 -0。若
		 * y < 0 且非奇整数，若 x 为 -Inf，应返回 +0。（再
		 * 次，我们无需区分这两种情形。）
		 */
		if (sign2 < 0)
			PG_RETURN_NUMERIC(make_result(&const_zero));

		/*
		 * 若 y 为 > 0 的奇整数且 x 为 -Inf，应返回 -Inf。对
		 * y > 0 且非奇整数，若 x 为 -Inf，应返回 +Inf。
		 */
		init_var_from_num(num2, &arg2);
		if (arg2.ndigits > 0 && arg2.ndigits == arg2.weight + 1 &&
			(arg2.digits[arg2.ndigits - 1] & 1))
			PG_RETURN_NUMERIC(make_result(&const_ninf));
		else
			PG_RETURN_NUMERIC(make_result(&const_pinf));
	}

	/*
	 * SQL 规范要求我们对某些错误条件发出特定的 SQLSTATE
	 * 错误码。具体说，0 ^ -1 不返回除零码；
	 * 数的非整数次幂必须产生同一错误码，但该情形在
	 * power_var() 中处理。
	 *
	 */
	sign1 = numeric_sign_internal(num1);
	sign2 = numeric_sign_internal(num2);

	if (sign1 == 0 && sign2 < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("zero raised to a negative power is undefined")));

	/*
	 * 初始化各量
	 */
	init_var(&result);
	init_var_from_num(num1, &arg1);
	init_var_from_num(num2, &arg2);

	/*
	 * 调用 power_var() 计算并返回结果；注意它自己处理
	 * 标度选择。
	 */
	power_var(&arg1, &arg2, &result);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}

/*
 * numeric_scale() -
 *
 *	返回标度，即小数部分中十进制数字位的个数
 */
Datum
numeric_scale(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);

	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NULL();

	PG_RETURN_INT32(NUMERIC_DSCALE(num));
}

/*
 * 计算给定值的最小标度。
 */
static int
get_min_scale(NumericVar *var)
{
	int			min_scale;
	int			last_digit_pos;

	/*
	 * 通常，输入值会被"修剪"，使最后一个
	 * NumericDigit 为非零。但若并非如此，我们不想陷入
	 * 无限循环，故显式找出最后一个非零数字位。
	 */
	last_digit_pos = var->ndigits - 1;
	while (last_digit_pos >= 0 &&
		   var->digits[last_digit_pos] == 0)
		last_digit_pos--;

	if (last_digit_pos >= 0)
	{
		/* 假定最后一个 ndigit 不含零来计算 min_scale */
		min_scale = (last_digit_pos - var->weight) * DEC_DIGITS;

		/*
		 * 若小数点后没有数字位，我们可能得到负结果。
		 * 这种情况下 min_scale 必须为零。
		 */
		if (min_scale > 0)
		{
			/*
			 * 若最后 NumericDigit 末尾数字位为零则减小
			 * min_scale。
			 */
			NumericDigit last_digit = var->digits[last_digit_pos];

			while (last_digit % 10 == 0)
			{
				min_scale--;
				last_digit /= 10;
			}
		}
		else
			min_scale = 0;
	}
	else
		min_scale = 0;			/* 输入为零时结果 */

	return min_scale;
}

/*
 * 返回无损表示给定值所需的最小标度。
 */
Datum
numeric_min_scale(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	arg;
	int			min_scale;

	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NULL();

	init_var_from_num(num, &arg);
	min_scale = get_min_scale(&arg);
	free_var(&arg);

	PG_RETURN_INT32(min_scale);
}

/*
 * 减小 numeric 值的标度，以无损表示给定值。
 */
Datum
numeric_trim_scale(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	result;

	if (NUMERIC_IS_SPECIAL(num))
		PG_RETURN_NUMERIC(duplicate_numeric(num));

	init_var_from_num(num, &result);
	result.dscale = get_min_scale(&result);
	res = make_result(&result);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/* ----------------------------------------------------------------------
 *
 * 类型转换函数
 *
 * ----------------------------------------------------------------------
 */

Numeric
int64_to_numeric(int64 val)
{
	Numeric		res;
	NumericVar	result;

	init_var(&result);

	int64_to_numericvar(val, &result);

	res = make_result(&result);

	free_var(&result);

	return res;
}

/*
 * 将 val1/(10**log10val2) 转换为 numeric。这远快于
 * 普通除法。
 */
Numeric
int64_div_fast_to_numeric(int64 val1, int log10val2)
{
	Numeric		res;
	NumericVar	result;
	int			rscale;
	int			w;
	int			m;

	init_var(&result);

	/* 结果标度 */
	rscale = log10val2 < 0 ? 0 : log10val2;

	/* 权重要减去的量 */
	w = log10val2 / DEC_DIGITS;
	/* 仍需继续除以的量 */
	m = log10val2 % DEC_DIGITS;
	if (m < 0)
	{
		m += DEC_DIGITS;
		w--;
	}

	/*
	 * 若仍有要除的余量（10^m，其中 0 < m < DEC_DIGITS），
	 * 则将被除数乘以 10^(DEC_DIGITS - m)，并把权重多移
	 * 一位。
	 */
	if (m > 0)
	{
#if DEC_DIGITS == 4
		static const int pow10[] = {1, 10, 100, 1000};
#elif DEC_DIGITS == 2
		static const int pow10[] = {1, 10};
#elif DEC_DIGITS == 1
		static const int pow10[] = {1};
#else
#error unsupported NBASE
#endif
		int64		factor = pow10[DEC_DIGITS - m];
		int64		new_val1;

		StaticAssertDecl(lengthof(pow10) == DEC_DIGITS, "mismatch with DEC_DIGITS");

		if (unlikely(pg_mul_s64_overflow(val1, factor, &new_val1)))
		{
#ifdef HAVE_INT128
			/* 使用 128 位整数执行乘法 */
			int128		tmp;

			tmp = (int128) val1 * (int128) factor;

			int128_to_numericvar(tmp, &result);
#else
			/* 使用 numeric 执行乘法 */
			NumericVar	tmp;

			init_var(&tmp);

			int64_to_numericvar(val1, &result);
			int64_to_numericvar(factor, &tmp);
			mul_var(&result, &tmp, &result, 0);

			free_var(&tmp);
#endif
		}
		else
			int64_to_numericvar(new_val1, &result);

		w++;
	}
	else
		int64_to_numericvar(val1, &result);

	result.weight -= w;
	result.dscale = rscale;

	res = make_result(&result);

	free_var(&result);

	return res;
}

Datum
int4_numeric(PG_FUNCTION_ARGS)
{
	int32		val = PG_GETARG_INT32(0);

	PG_RETURN_NUMERIC(int64_to_numeric(val));
}

int32
numeric_int4_opt_error(Numeric num, bool *have_error)
{
	NumericVar	x;
	int32		result;

	if (have_error)
		*have_error = false;

	if (NUMERIC_IS_SPECIAL(num))
	{
		if (have_error)
		{
			*have_error = true;
			return 0;
		}
		else
		{
			if (NUMERIC_IS_NAN(num))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert NaN to %s", "integer")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert infinity to %s", "integer")));
		}
	}

	/* 先转换为变量格式，再转换为 int4 */
	init_var_from_num(num, &x);

	if (!numericvar_to_int32(&x, &result))
	{
		if (have_error)
		{
			*have_error = true;
			return 0;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		}
	}

	return result;
}

Datum
numeric_int4(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);

	PG_RETURN_INT32(numeric_int4_opt_error(num, NULL));
}

/*
 * 将给定的 NumericVar 转换为 int32。若 NumericVar
 * 超出 int32 的范围则返回 false，否则返回 true。
 * 输入 NumericVar *不会*释放。
 */
static bool
numericvar_to_int32(const NumericVar *var, int32 *result)
{
	int64		val;

	if (!numericvar_to_int64(var, &val))
		return false;

	if (unlikely(val < PG_INT32_MIN) || unlikely(val > PG_INT32_MAX))
		return false;

	/* 降档转换为 int4 */
	*result = (int32) val;

	return true;
}

Datum
int8_numeric(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);

	PG_RETURN_NUMERIC(int64_to_numeric(val));
}


Datum
numeric_int8(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	int64		result;

	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_NAN(num))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert NaN to %s", "bigint")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert infinity to %s", "bigint")));
	}

	/* 转换为变量格式，进而转为 int8 */
	init_var_from_num(num, &x);

	if (!numericvar_to_int64(&x, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64(result);
}


Datum
int2_numeric(PG_FUNCTION_ARGS)
{
	int16		val = PG_GETARG_INT16(0);

	PG_RETURN_NUMERIC(int64_to_numeric(val));
}


Datum
numeric_int2(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	int64		val;
	int16		result;

	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_NAN(num))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert NaN to %s", "smallint")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert infinity to %s", "smallint")));
	}

	/* 转换为变量格式，进而转为 int8 */
	init_var_from_num(num, &x);

	if (!numericvar_to_int64(&x, &val))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	if (unlikely(val < PG_INT16_MIN) || unlikely(val > PG_INT16_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	/* 降档转换为 int2 */
	result = (int16) val;

	PG_RETURN_INT16(result);
}


Datum
float8_numeric(PG_FUNCTION_ARGS)
{
	float8		val = PG_GETARG_FLOAT8(0);
	Numeric		res;
	NumericVar	result;
	char		buf[DBL_DIG + 100];
	const char *endptr;

	if (isnan(val))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	if (isinf(val))
	{
		if (val < 0)
			PG_RETURN_NUMERIC(make_result(&const_ninf));
		else
			PG_RETURN_NUMERIC(make_result(&const_pinf));
	}

	snprintf(buf, sizeof(buf), "%.*g", DBL_DIG, val);

	init_var(&result);

	/* 假定无需担心前导/尾随空格 */
	(void) set_var_from_str(buf, buf, &result, &endptr, NULL);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_float8(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	char	   *tmp;
	Datum		result;

	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_PINF(num))
			PG_RETURN_FLOAT8(get_float8_infinity());
		else if (NUMERIC_IS_NINF(num))
			PG_RETURN_FLOAT8(-get_float8_infinity());
		else
			PG_RETURN_FLOAT8(get_float8_nan());
	}

	tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(num)));

	result = DirectFunctionCall1(float8in, CStringGetDatum(tmp));

	pfree(tmp);

	PG_RETURN_DATUM(result);
}


/*
 * 把 numeric 转为 float8；若超出范围返回 +/- HUGE_VAL
 *
 * （内部辅助函数，不能从 SQL 直接调用）
 */
Datum
numeric_float8_no_overflow(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	double		val;

	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_PINF(num))
			val = HUGE_VAL;
		else if (NUMERIC_IS_NINF(num))
			val = -HUGE_VAL;
		else
			val = get_float8_nan();
	}
	else
	{
		NumericVar	x;

		init_var_from_num(num, &x);
		val = numericvar_to_double_no_overflow(&x);
	}

	PG_RETURN_FLOAT8(val);
}

Datum
float4_numeric(PG_FUNCTION_ARGS)
{
	float4		val = PG_GETARG_FLOAT4(0);
	Numeric		res;
	NumericVar	result;
	char		buf[FLT_DIG + 100];
	const char *endptr;

	if (isnan(val))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	if (isinf(val))
	{
		if (val < 0)
			PG_RETURN_NUMERIC(make_result(&const_ninf));
		else
			PG_RETURN_NUMERIC(make_result(&const_pinf));
	}

	snprintf(buf, sizeof(buf), "%.*g", FLT_DIG, val);

	init_var(&result);

	/* 假定无需担心前导/尾随空格 */
	(void) set_var_from_str(buf, buf, &result, &endptr, NULL);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_float4(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	char	   *tmp;
	Datum		result;

	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_PINF(num))
			PG_RETURN_FLOAT4(get_float4_infinity());
		else if (NUMERIC_IS_NINF(num))
			PG_RETURN_FLOAT4(-get_float4_infinity());
		else
			PG_RETURN_FLOAT4(get_float4_nan());
	}

	tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(num)));

	result = DirectFunctionCall1(float4in, CStringGetDatum(tmp));

	pfree(tmp);

	PG_RETURN_DATUM(result);
}


Datum
numeric_pg_lsn(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	XLogRecPtr	result;

	if (NUMERIC_IS_SPECIAL(num))
	{
		if (NUMERIC_IS_NAN(num))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert NaN to %s", "pg_lsn")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot convert infinity to %s", "pg_lsn")));
	}

	/* 转换为变量格式，进而转为 pg_lsn */
	init_var_from_num(num, &x);

	if (!numericvar_to_uint64(&x, (uint64 *) &result))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_lsn out of range")));

	PG_RETURN_LSN(result);
}


/* ----------------------------------------------------------------------
 *
 * 聚合函数
 *
 * 所有这些聚合的转移数据类型都声明为 INTERNAL。
 * 实际上它是指向在聚合上下文中分配的 NumericAggState
 * 的指针。NumericVar 的数字位缓冲区也在那里。
 *
 * 在支持 128 位整数的平台上，一些聚合改用基于 128 位
 * 整数的转移数据类型来加速计算。
 *
 * ----------------------------------------------------------------------
 */

typedef struct NumericAggState
{
	bool		calcSumX2;		/* 为真时计算 sumX2 */
	MemoryContext agg_context;	/* 所在计算上下文 */
	int64		N;				/* 已处理数字的个数 */
	NumericSumAccum sumX;		/* 已处理数字之和 */
	NumericSumAccum sumX2;		/* 已处理数字的平方之和 */
	int			maxScale;		/* 迄今所见最大标度 */
	int64		maxScaleCount;	/* 所见具有最大标度的值的个数 */
	/* 这些计数*并不*包含在 N 中！需要时用 NA_TOTAL_COUNT() */
	int64		NaNcount;		/* NaN 值的个数 */
	int64		pInfcount;		/* +Inf 值的个数 */
	int64		nInfcount;		/* -Inf 值的个数 */
} NumericAggState;

#define NA_TOTAL_COUNT(na) \
	((na)->N + (na)->NaNcount + (na)->pInfcount + (na)->nInfcount)

/*
 * 为需计算输入之和、个数及可选平方之和的 numeric
 * 聚合函数准备状态数据。
 */
static NumericAggState *
makeNumericAggState(FunctionCallInfo fcinfo, bool calcSumX2)
{
	NumericAggState *state;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	old_context = MemoryContextSwitchTo(agg_context);

	state = (NumericAggState *) palloc0(sizeof(NumericAggState));
	state->calcSumX2 = calcSumX2;
	state->agg_context = agg_context;

	MemoryContextSwitchTo(old_context);

	return state;
}

/*
 * 同 makeNumericAggState()，但在当前内存上下文分配状态。
 *
 */
static NumericAggState *
makeNumericAggStateCurrentContext(bool calcSumX2)
{
	NumericAggState *state;

	state = (NumericAggState *) palloc0(sizeof(NumericAggState));
	state->calcSumX2 = calcSumX2;
	state->agg_context = CurrentMemoryContext;

	return state;
}

/*
 * 为 numeric 聚合函数累积一个新的输入值。
 */
static void
do_numeric_accum(NumericAggState *state, Numeric newval)
{
	NumericVar	X;
	NumericVar	X2;
	MemoryContext old_context;

	/* 将 NaN/无穷大输入与其他输入分开计数 */
	if (NUMERIC_IS_SPECIAL(newval))
	{
		if (NUMERIC_IS_PINF(newval))
			state->pInfcount++;
		else if (NUMERIC_IS_NINF(newval))
			state->nInfcount++;
		else
			state->NaNcount++;
		return;
	}

	/* 在短命上下文中加载已处理数字 */
	init_var_from_num(newval, &X);

	/*
	 * 记录见过的最高输入显示标度，以支持反向转移
	 * （见 do_numeric_discard）。
	 */
	if (X.dscale > state->maxScale)
	{
		state->maxScale = X.dscale;
		state->maxScaleCount = 1;
	}
	else if (X.dscale == state->maxScale)
		state->maxScaleCount++;

	/* 若需要 X^2，则在短命上下文中计算 */
	if (state->calcSumX2)
	{
		init_var(&X2);
		mul_var(&X, &X, &X2, X.dscale * 2);
	}

	/* 其余部分需要在聚合上下文中进行 */
	old_context = MemoryContextSwitchTo(state->agg_context);

	state->N++;

	/* 累积各和值 */
	accum_sum_add(&(state->sumX), &X);

	if (state->calcSumX2)
		accum_sum_add(&(state->sumX2), &X2);

	MemoryContextSwitchTo(old_context);
}

/*
 * 尝试从聚合状态中移除一个输入值。
 *
 * 若值无法移除，函数将返回 false；
 * 可能的原因在下面说明。
 *
 * 若我们聚合值 1.01 和 2，则结果将是 3.01。
 * 之后若被要求反聚合掉 1.01，我们必须失败，因为
 * 无法判断新的聚合值的显示标度应当是多少。
 * 我们不想返回 2.00（显示标度 = 2），因为若真只聚合
 * 了 2，和的显示标度本应为零。
 *
 * 注意：另一做法是为每种可能的显示标度统计输入个数，
 * （最多到某个合理的上限）。尚不清楚这是否值得。
 */
static bool
do_numeric_discard(NumericAggState *state, Numeric newval)
{
	NumericVar	X;
	NumericVar	X2;
	MemoryContext old_context;

	/* 将 NaN/无穷大输入与其他输入分开计数 */
	if (NUMERIC_IS_SPECIAL(newval))
	{
		if (NUMERIC_IS_PINF(newval))
			state->pInfcount--;
		else if (NUMERIC_IS_NINF(newval))
			state->nInfcount--;
		else
			state->NaNcount--;
		return true;
	}

	/* 在短命上下文中加载已处理数字 */
	init_var_from_num(newval, &X);

	/*
	 * state->sumX 的显示标度为各输入显示标度最大值。
	 * 移除具有该显示标度的最后输入会要求我们重新计算
	 * *剩余*输入的最大显示标度，而这只有当完全不再有非
	 * NaN 输入时才可能做到。所以我们改为报告失败，
	 * 并强制从头重做聚合。
	 */
	if (X.dscale == state->maxScale)
	{
		if (state->maxScaleCount > 1 || state->maxScale == 0)
		{
			/*
			 * 仍有些输入具相同显示标度，或显示标度尚未升
			 * 到零以上。
			 */
			state->maxScaleCount--;
		}
		else if (state->N == 1)
		{
			/* 完全不再有非 NaN 输入，故重置 maxScale */
			state->maxScale = 0;
			state->maxScaleCount = 0;
		}
		else
		{
			/* maxScale 新值无法确定，必须失败 */
			return false;
		}
	}

	/* 若需要 X^2，则在短命上下文中计算 */
	if (state->calcSumX2)
	{
		init_var(&X2);
		mul_var(&X, &X, &X2, X.dscale * 2);
	}

	/* 其余部分需要在聚合上下文中进行 */
	old_context = MemoryContextSwitchTo(state->agg_context);

	if (state->N-- > 1)
	{
		/* 将 X 取负以便从和减去它 */
		X.sign = (X.sign == NUMERIC_POS ? NUMERIC_NEG : NUMERIC_POS);
		accum_sum_add(&(state->sumX), &X);

		if (state->calcSumX2)
		{
			/* 将 X^2 取负。X^2 恒为正 */
			X2.sign = NUMERIC_NEG;
			accum_sum_add(&(state->sumX2), &X2);
		}
	}
	else
	{
		/* 各和清零 */
		Assert(state->N == 0);

		accum_sum_reset(&state->sumX);
		if (state->calcSumX2)
			accum_sum_reset(&state->sumX2);
	}

	MemoryContextSwitchTo(old_context);

	return true;
}

/*
 * 需要 sumX2 的 numeric 聚合的通用转移函数。
 */
Datum
numeric_accum(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* 首次调用时创建状态数据 */
	if (state == NULL)
		state = makeNumericAggState(fcinfo, true);

	if (!PG_ARGISNULL(1))
		do_numeric_accum(state, PG_GETARG_NUMERIC(1));

	PG_RETURN_POINTER(state);
}

/*
 * 需要 sumX2 的 numeric 聚合的通用合并函数
 */
Datum
numeric_combine(PG_FUNCTION_ARGS)
{
	NumericAggState *state1;
	NumericAggState *state2;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state1 = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (NumericAggState *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
		PG_RETURN_POINTER(state1);

	/* 手动把 state2 字段全复制到 state1 */
	if (state1 == NULL)
	{
		old_context = MemoryContextSwitchTo(agg_context);

		state1 = makeNumericAggStateCurrentContext(true);
		state1->N = state2->N;
		state1->NaNcount = state2->NaNcount;
		state1->pInfcount = state2->pInfcount;
		state1->nInfcount = state2->nInfcount;
		state1->maxScale = state2->maxScale;
		state1->maxScaleCount = state2->maxScaleCount;

		accum_sum_copy(&state1->sumX, &state2->sumX);
		accum_sum_copy(&state1->sumX2, &state2->sumX2);

		MemoryContextSwitchTo(old_context);

		PG_RETURN_POINTER(state1);
	}

	state1->N += state2->N;
	state1->NaNcount += state2->NaNcount;
	state1->pInfcount += state2->pInfcount;
	state1->nInfcount += state2->nInfcount;

	if (state2->N > 0)
	{
		/*
		 * 这些目前只有移动聚合才需要，但我们还是把事情
		 * 做对……
		 */
		if (state2->maxScale > state1->maxScale)
		{
			state1->maxScale = state2->maxScale;
			state1->maxScaleCount = state2->maxScaleCount;
		}
		else if (state2->maxScale == state1->maxScale)
			state1->maxScaleCount += state2->maxScaleCount;

		/* 其余部分需要在聚合上下文中进行 */
		old_context = MemoryContextSwitchTo(agg_context);

		/* 累积各和值 */
		accum_sum_combine(&state1->sumX, &state2->sumX);
		accum_sum_combine(&state1->sumX2, &state2->sumX2);

		MemoryContextSwitchTo(old_context);
	}
	PG_RETURN_POINTER(state1);
}

/*
 * 不需要 sumX2 的 numeric 聚合的通用转移函数。
 */
Datum
numeric_avg_accum(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* 首次调用时创建状态数据 */
	if (state == NULL)
		state = makeNumericAggState(fcinfo, false);

	if (!PG_ARGISNULL(1))
		do_numeric_accum(state, PG_GETARG_NUMERIC(1));

	PG_RETURN_POINTER(state);
}

/*
 * 不需要 sumX2 的 numeric 聚合的合并函数
 */
Datum
numeric_avg_combine(PG_FUNCTION_ARGS)
{
	NumericAggState *state1;
	NumericAggState *state2;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state1 = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (NumericAggState *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
		PG_RETURN_POINTER(state1);

	/* 手动把 state2 字段全复制到 state1 */
	if (state1 == NULL)
	{
		old_context = MemoryContextSwitchTo(agg_context);

		state1 = makeNumericAggStateCurrentContext(false);
		state1->N = state2->N;
		state1->NaNcount = state2->NaNcount;
		state1->pInfcount = state2->pInfcount;
		state1->nInfcount = state2->nInfcount;
		state1->maxScale = state2->maxScale;
		state1->maxScaleCount = state2->maxScaleCount;

		accum_sum_copy(&state1->sumX, &state2->sumX);

		MemoryContextSwitchTo(old_context);

		PG_RETURN_POINTER(state1);
	}

	state1->N += state2->N;
	state1->NaNcount += state2->NaNcount;
	state1->pInfcount += state2->pInfcount;
	state1->nInfcount += state2->nInfcount;

	if (state2->N > 0)
	{
		/*
		 * 这些目前只有移动聚合才需要，但我们还是把事情
		 * 做对……
		 */
		if (state2->maxScale > state1->maxScale)
		{
			state1->maxScale = state2->maxScale;
			state1->maxScaleCount = state2->maxScaleCount;
		}
		else if (state2->maxScale == state1->maxScale)
			state1->maxScaleCount += state2->maxScaleCount;

		/* 其余部分需要在聚合上下文中进行 */
		old_context = MemoryContextSwitchTo(agg_context);

		/* 累积各和值 */
		accum_sum_combine(&state1->sumX, &state2->sumX);

		MemoryContextSwitchTo(old_context);
	}
	PG_RETURN_POINTER(state1);
}

/*
 * numeric_avg_serialize
 *		为不需要 sumX2 的 numeric 聚合序列化 NumericAggState。
 *
 */
Datum
numeric_avg_serialize(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* 确保不在聚合上下文中时禁止调用 */
	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (NumericAggState *) PG_GETARG_POINTER(0);

	init_var(&tmp_var);

	pq_begintypsend(&buf);

	/* N */
	pq_sendint64(&buf, state->N);

	/* sumX */
	accum_sum_final(&state->sumX, &tmp_var);
	numericvar_serialize(&buf, &tmp_var);

	/* maxScale */
	pq_sendint32(&buf, state->maxScale);

	/* maxScaleCount */
	pq_sendint64(&buf, state->maxScaleCount);

	/* NaNcount */
	pq_sendint64(&buf, state->NaNcount);

	/* pInfcount */
	pq_sendint64(&buf, state->pInfcount);

	/* nInfcount */
	pq_sendint64(&buf, state->nInfcount);

	result = pq_endtypsend(&buf);

	free_var(&tmp_var);

	PG_RETURN_BYTEA_P(result);
}

/*
 * numeric_avg_deserialize
 *		为不需要 sumX2 的 numeric 聚合把 bytea 反序列化为
 *		NumericAggState。
 */
Datum
numeric_avg_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	NumericAggState *result;
	StringInfoData buf;
	NumericVar	tmp_var;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	sstate = PG_GETARG_BYTEA_PP(0);

	init_var(&tmp_var);

	/*
	 * 把 bytea 复制进一个 StringInfo，以便能用标准 recv
	 * 基础设施来"接收"它。
	 */
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf,
						   VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

	result = makeNumericAggStateCurrentContext(false);

	/* N */
	result->N = pq_getmsgint64(&buf);

	/* sumX */
	numericvar_deserialize(&buf, &tmp_var);
	accum_sum_add(&(result->sumX), &tmp_var);

	/* maxScale */
	result->maxScale = pq_getmsgint(&buf, 4);

	/* maxScaleCount */
	result->maxScaleCount = pq_getmsgint64(&buf);

	/* NaNcount */
	result->NaNcount = pq_getmsgint64(&buf);

	/* pInfcount */
	result->pInfcount = pq_getmsgint64(&buf);

	/* nInfcount */
	result->nInfcount = pq_getmsgint64(&buf);

	pq_getmsgend(&buf);
	pfree(buf.data);

	free_var(&tmp_var);

	PG_RETURN_POINTER(result);
}

/*
 * numeric_serialize
 *		为要求 sumX2 的 numeric 聚合序列化 NumericAggState。
 *
 */
Datum
numeric_serialize(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* 确保不在聚合上下文中时禁止调用 */
	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (NumericAggState *) PG_GETARG_POINTER(0);

	init_var(&tmp_var);

	pq_begintypsend(&buf);

	/* N */
	pq_sendint64(&buf, state->N);

	/* sumX */
	accum_sum_final(&state->sumX, &tmp_var);
	numericvar_serialize(&buf, &tmp_var);

	/* sumX2 */
	accum_sum_final(&state->sumX2, &tmp_var);
	numericvar_serialize(&buf, &tmp_var);

	/* maxScale */
	pq_sendint32(&buf, state->maxScale);

	/* maxScaleCount */
	pq_sendint64(&buf, state->maxScaleCount);

	/* NaNcount */
	pq_sendint64(&buf, state->NaNcount);

	/* pInfcount */
	pq_sendint64(&buf, state->pInfcount);

	/* nInfcount */
	pq_sendint64(&buf, state->nInfcount);

	result = pq_endtypsend(&buf);

	free_var(&tmp_var);

	PG_RETURN_BYTEA_P(result);
}

/*
 * numeric_deserialize
 *		为要求 sumX2 的 numeric 聚合反序列化出 NumericAggState。
 *
 */
Datum
numeric_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	NumericAggState *result;
	StringInfoData buf;
	NumericVar	tmp_var;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	sstate = PG_GETARG_BYTEA_PP(0);

	init_var(&tmp_var);

	/*
	 * 把 bytea 复制进一个 StringInfo，以便能用标准 recv
	 * 基础设施来"接收"它。
	 */
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf,
						   VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

	result = makeNumericAggStateCurrentContext(false);

	/* N */
	result->N = pq_getmsgint64(&buf);

	/* sumX */
	numericvar_deserialize(&buf, &tmp_var);
	accum_sum_add(&(result->sumX), &tmp_var);

	/* sumX2 */
	numericvar_deserialize(&buf, &tmp_var);
	accum_sum_add(&(result->sumX2), &tmp_var);

	/* maxScale */
	result->maxScale = pq_getmsgint(&buf, 4);

	/* maxScaleCount */
	result->maxScaleCount = pq_getmsgint64(&buf);

	/* NaNcount */
	result->NaNcount = pq_getmsgint64(&buf);

	/* pInfcount */
	result->pInfcount = pq_getmsgint64(&buf);

	/* nInfcount */
	result->nInfcount = pq_getmsgint64(&buf);

	pq_getmsgend(&buf);
	pfree(buf.data);

	free_var(&tmp_var);

	PG_RETURN_POINTER(result);
}

/*
 * numeric 聚合的通用反向转移函数
 * （要求或不要求 X^2 均可）。
 */
Datum
numeric_accum_inv(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* 没有状态时不应到达此处 */
	if (state == NULL)
		elog(ERROR, "numeric_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
		/* 若无法执行反向转移，则返回 NULL */
		if (!do_numeric_discard(state, PG_GETARG_NUMERIC(1)))
			PG_RETURN_NULL();
	}

	PG_RETURN_POINTER(state);
}


/*
 * 整数数据类型一般使用 Numeric 累加器，以共享代码
 * 并避免溢出风险。
 *
 * 不过出于性能原因，会尽可能使用优化的专用累加器
 * 例程。
 *
 * 在支持 128 位整数的平台上，当 sum(X) 或 sum(X*X) 能放入
 * 128 位时将使用 128 位例程。
 *
 * 对 16 和 32 位输入而言，N 与 sum(X) 可放入 64 位，因此
 * 这些数据类型的 SUM 与 AVG 会使用 64 位累加器。
 */

#ifdef HAVE_INT128
typedef struct Int128AggState
{
	bool		calcSumX2;		/* 为真时计算 sumX2 */
	int64		N;				/* 已处理数字的个数 */
	int128		sumX;			/* 已处理数字之和 */
	int128		sumX2;			/* 已处理数字的平方之和 */
} Int128AggState;

/*
 * 为需计算输入之和、个数及可选平方之和的 128 位
 * 聚合函数准备状态数据。
 */
static Int128AggState *
makeInt128AggState(FunctionCallInfo fcinfo, bool calcSumX2)
{
	Int128AggState *state;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	old_context = MemoryContextSwitchTo(agg_context);

	state = (Int128AggState *) palloc0(sizeof(Int128AggState));
	state->calcSumX2 = calcSumX2;

	MemoryContextSwitchTo(old_context);

	return state;
}

/*
 * 同 makeInt128AggState()，但在当前内存上下文分配状态。
 *
 */
static Int128AggState *
makeInt128AggStateCurrentContext(bool calcSumX2)
{
	Int128AggState *state;

	state = (Int128AggState *) palloc0(sizeof(Int128AggState));
	state->calcSumX2 = calcSumX2;

	return state;
}

/*
 * 为 128 位聚合函数累积一个新的输入值。
 */
static void
do_int128_accum(Int128AggState *state, int128 newval)
{
	if (state->calcSumX2)
		state->sumX2 += newval * newval;

	state->sumX += newval;
	state->N++;
}

/*
 * 从聚合状态中移除一个输入值。
 */
static void
do_int128_discard(Int128AggState *state, int128 newval)
{
	if (state->calcSumX2)
		state->sumX2 -= newval * newval;

	state->sumX -= newval;
	state->N--;
}

typedef Int128AggState PolyNumAggState;
#define makePolyNumAggState makeInt128AggState
#define makePolyNumAggStateCurrentContext makeInt128AggStateCurrentContext
#else
typedef NumericAggState PolyNumAggState;
#define makePolyNumAggState makeNumericAggState
#define makePolyNumAggStateCurrentContext makeNumericAggStateCurrentContext
#endif

Datum
int2_accum(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 首次调用时创建状态数据 */
	if (state == NULL)
		state = makePolyNumAggState(fcinfo, true);

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_accum(state, (int128) PG_GETARG_INT16(1));
#else
		do_numeric_accum(state, int64_to_numeric(PG_GETARG_INT16(1)));
#endif
	}

	PG_RETURN_POINTER(state);
}

Datum
int4_accum(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 首次调用时创建状态数据 */
	if (state == NULL)
		state = makePolyNumAggState(fcinfo, true);

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_accum(state, (int128) PG_GETARG_INT32(1));
#else
		do_numeric_accum(state, int64_to_numeric(PG_GETARG_INT32(1)));
#endif
	}

	PG_RETURN_POINTER(state);
}

Datum
int8_accum(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* 首次调用时创建状态数据 */
	if (state == NULL)
		state = makeNumericAggState(fcinfo, true);

	if (!PG_ARGISNULL(1))
		do_numeric_accum(state, int64_to_numeric(PG_GETARG_INT64(1)));

	PG_RETURN_POINTER(state);
}

/*
 * 需要 sumX2 的 numeric 聚合的合并函数
 */
Datum
numeric_poly_combine(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state1;
	PolyNumAggState *state2;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state1 = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
		PG_RETURN_POINTER(state1);

	/* 手动把 state2 字段全复制到 state1 */
	if (state1 == NULL)
	{
		old_context = MemoryContextSwitchTo(agg_context);

		state1 = makePolyNumAggState(fcinfo, true);
		state1->N = state2->N;

#ifdef HAVE_INT128
		state1->sumX = state2->sumX;
		state1->sumX2 = state2->sumX2;
#else
		accum_sum_copy(&state1->sumX, &state2->sumX);
		accum_sum_copy(&state1->sumX2, &state2->sumX2);
#endif

		MemoryContextSwitchTo(old_context);

		PG_RETURN_POINTER(state1);
	}

	if (state2->N > 0)
	{
		state1->N += state2->N;

#ifdef HAVE_INT128
		state1->sumX += state2->sumX;
		state1->sumX2 += state2->sumX2;
#else
		/* 其余部分需要在聚合上下文中进行 */
		old_context = MemoryContextSwitchTo(agg_context);

		/* 累积各和值 */
		accum_sum_combine(&state1->sumX, &state2->sumX);
		accum_sum_combine(&state1->sumX2, &state2->sumX2);

		MemoryContextSwitchTo(old_context);
#endif

	}
	PG_RETURN_POINTER(state1);
}

/*
 * numeric_poly_serialize
 *		把要求 sumX2 聚合的 PolyNumAggState 序列化进 bytea。
 *
 */
Datum
numeric_poly_serialize(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* 确保不在聚合上下文中时禁止调用 */
	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (PolyNumAggState *) PG_GETARG_POINTER(0);

	/*
	 * 若平台支持 int128，则 sumX 与 sumX2 将是 128 位整数
	 * 类型。将其转成 numeric，使合并状态对支持 int128 的
	 * 机器与不支持该型的机器是同一格式。这里的考量
	 * 是，或许有一天会把这些送到另一服务器继续处理，
	 * 所以我们需要一个可用的标准格式。
	 *
	 */

	init_var(&tmp_var);

	pq_begintypsend(&buf);

	/* N */
	pq_sendint64(&buf, state->N);

	/* sumX */
#ifdef HAVE_INT128
	int128_to_numericvar(state->sumX, &tmp_var);
#else
	accum_sum_final(&state->sumX, &tmp_var);
#endif
	numericvar_serialize(&buf, &tmp_var);

	/* sumX2 */
#ifdef HAVE_INT128
	int128_to_numericvar(state->sumX2, &tmp_var);
#else
	accum_sum_final(&state->sumX2, &tmp_var);
#endif
	numericvar_serialize(&buf, &tmp_var);

	result = pq_endtypsend(&buf);

	free_var(&tmp_var);

	PG_RETURN_BYTEA_P(result);
}

/*
 * numeric_poly_deserialize
 *		把要求 sumX2 聚合的 PolyNumAggState 自 bytea 反序列化。
 *
 */
Datum
numeric_poly_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	PolyNumAggState *result;
	StringInfoData buf;
	NumericVar	tmp_var;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	sstate = PG_GETARG_BYTEA_PP(0);

	init_var(&tmp_var);

	/*
	 * 把 bytea 复制进一个 StringInfo，以便能用标准 recv
	 * 基础设施来"接收"它。
	 */
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf,
						   VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

	result = makePolyNumAggStateCurrentContext(false);

	/* N */
	result->N = pq_getmsgint64(&buf);

	/* sumX */
	numericvar_deserialize(&buf, &tmp_var);
#ifdef HAVE_INT128
	numericvar_to_int128(&tmp_var, &result->sumX);
#else
	accum_sum_add(&result->sumX, &tmp_var);
#endif

	/* sumX2 */
	numericvar_deserialize(&buf, &tmp_var);
#ifdef HAVE_INT128
	numericvar_to_int128(&tmp_var, &result->sumX2);
#else
	accum_sum_add(&result->sumX2, &tmp_var);
#endif

	pq_getmsgend(&buf);
	pfree(buf.data);

	free_var(&tmp_var);

	PG_RETURN_POINTER(result);
}

/*
 * 不需要 sumX2 的 int8 输入转移函数。
 */
Datum
int8_avg_accum(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 首次调用时创建状态数据 */
	if (state == NULL)
		state = makePolyNumAggState(fcinfo, false);

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_accum(state, (int128) PG_GETARG_INT64(1));
#else
		do_numeric_accum(state, int64_to_numeric(PG_GETARG_INT64(1)));
#endif
	}

	PG_RETURN_POINTER(state);
}

/*
 * 对不要求 sumX2 的聚合，PolyNumAggState 的合并函数
 *
 */
Datum
int8_avg_combine(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state1;
	PolyNumAggState *state2;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state1 = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
		PG_RETURN_POINTER(state1);

	/* 手动把 state2 字段全复制到 state1 */
	if (state1 == NULL)
	{
		old_context = MemoryContextSwitchTo(agg_context);

		state1 = makePolyNumAggState(fcinfo, false);
		state1->N = state2->N;

#ifdef HAVE_INT128
		state1->sumX = state2->sumX;
#else
		accum_sum_copy(&state1->sumX, &state2->sumX);
#endif
		MemoryContextSwitchTo(old_context);

		PG_RETURN_POINTER(state1);
	}

	if (state2->N > 0)
	{
		state1->N += state2->N;

#ifdef HAVE_INT128
		state1->sumX += state2->sumX;
#else
		/* 其余部分需要在聚合上下文中进行 */
		old_context = MemoryContextSwitchTo(agg_context);

		/* 累积各和值 */
		accum_sum_combine(&state1->sumX, &state2->sumX);

		MemoryContextSwitchTo(old_context);
#endif

	}
	PG_RETURN_POINTER(state1);
}

/*
 * int8_avg_serialize
 *		以标准 recv 函数设施序列化 PolyNumAggState
 *		进 bytea。
 */
Datum
int8_avg_serialize(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* 确保不在聚合上下文中时禁止调用 */
	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (PolyNumAggState *) PG_GETARG_POINTER(0);

	/*
	 * 若平台支持 int128，则 sumX 将是 128 位整数类型。这
	 * 里会把它转换成 numeric 类型，使合并状态对支持
	 * int128 机器与不支持的机器均为相同格式。这里的考量
	 * 是，或许有一天会把这些送到另一服务器继续处理，
	 * 所以我们需要一个可用的标准格式。
	 *
	 */

	init_var(&tmp_var);

	pq_begintypsend(&buf);

	/* N */
	pq_sendint64(&buf, state->N);

	/* sumX */
#ifdef HAVE_INT128
	int128_to_numericvar(state->sumX, &tmp_var);
#else
	accum_sum_final(&state->sumX, &tmp_var);
#endif
	numericvar_serialize(&buf, &tmp_var);

	result = pq_endtypsend(&buf);

	free_var(&tmp_var);

	PG_RETURN_BYTEA_P(result);
}

/*
 * int8_avg_deserialize
 *		从 bytea 反序列化 PolyNumAggState。
 */
Datum
int8_avg_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	PolyNumAggState *result;
	StringInfoData buf;
	NumericVar	tmp_var;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	sstate = PG_GETARG_BYTEA_PP(0);

	init_var(&tmp_var);

	/*
	 * 把 bytea 复制进一个 StringInfo，以便能用标准 recv
	 * 基础设施来"接收"它。
	 */
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf,
						   VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

	result = makePolyNumAggStateCurrentContext(false);

	/* N */
	result->N = pq_getmsgint64(&buf);

	/* sumX */
	numericvar_deserialize(&buf, &tmp_var);
#ifdef HAVE_INT128
	numericvar_to_int128(&tmp_var, &result->sumX);
#else
	accum_sum_add(&result->sumX, &tmp_var);
#endif

	pq_getmsgend(&buf);
	pfree(buf.data);

	free_var(&tmp_var);

	PG_RETURN_POINTER(result);
}

/*
 * 与上述配套的反向转移函数。
 */

Datum
int2_accum_inv(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 没有状态时不应到达此处 */
	if (state == NULL)
		elog(ERROR, "int2_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_discard(state, (int128) PG_GETARG_INT16(1));
#else
		/* 绝不应失败：显示标度全为 0 */
		if (!do_numeric_discard(state, int64_to_numeric(PG_GETARG_INT16(1))))
			elog(ERROR, "do_numeric_discard failed unexpectedly");
#endif
	}

	PG_RETURN_POINTER(state);
}

Datum
int4_accum_inv(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 没有状态时不应到达此处 */
	if (state == NULL)
		elog(ERROR, "int4_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_discard(state, (int128) PG_GETARG_INT32(1));
#else
		/* 绝不应失败：显示标度全为 0 */
		if (!do_numeric_discard(state, int64_to_numeric(PG_GETARG_INT32(1))))
			elog(ERROR, "do_numeric_discard failed unexpectedly");
#endif
	}

	PG_RETURN_POINTER(state);
}

Datum
int8_accum_inv(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* 没有状态时不应到达此处 */
	if (state == NULL)
		elog(ERROR, "int8_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
		/* 绝不应失败：显示标度全为 0 */
		if (!do_numeric_discard(state, int64_to_numeric(PG_GETARG_INT64(1))))
			elog(ERROR, "do_numeric_discard failed unexpectedly");
	}

	PG_RETURN_POINTER(state);
}

Datum
int8_avg_accum_inv(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 没有状态时不应到达此处 */
	if (state == NULL)
		elog(ERROR, "int8_avg_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_discard(state, (int128) PG_GETARG_INT64(1));
#else
		/* 绝不应失败：显示标度全为 0 */
		if (!do_numeric_discard(state, int64_to_numeric(PG_GETARG_INT64(1))))
			elog(ERROR, "do_numeric_discard failed unexpectedly");
#endif
	}

	PG_RETURN_POINTER(state);
}

Datum
numeric_poly_sum(PG_FUNCTION_ARGS)
{
#ifdef HAVE_INT128
	PolyNumAggState *state;
	Numeric		res;
	NumericVar	result;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 若没有非空输入，则返回 NULL */
	if (state == NULL || state->N == 0)
		PG_RETURN_NULL();

	init_var(&result);

	int128_to_numericvar(state->sumX, &result);

	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
#else
	return numeric_sum(fcinfo);
#endif
}

Datum
numeric_poly_avg(PG_FUNCTION_ARGS)
{
#ifdef HAVE_INT128
	PolyNumAggState *state;
	NumericVar	result;
	Datum		countd,
				sumd;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* 若没有非空输入，则返回 NULL */
	if (state == NULL || state->N == 0)
		PG_RETURN_NULL();

	init_var(&result);

	int128_to_numericvar(state->sumX, &result);

	countd = NumericGetDatum(int64_to_numeric(state->N));
	sumd = NumericGetDatum(make_result(&result));

	free_var(&result);

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sumd, countd));
#else
	return numeric_avg(fcinfo);
#endif
}

Datum
numeric_avg(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	Datum		N_datum;
	Datum		sumX_datum;
	NumericVar	sumX_var;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* 若没有非空输入，则返回 NULL */
	if (state == NULL || NA_TOTAL_COUNT(state) == 0)
		PG_RETURN_NULL();

	if (state->NaNcount > 0)	/* 至少有一个 NaN 输入 */
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/* 正无穷大与负无穷大相加得 NaN */
	if (state->pInfcount > 0 && state->nInfcount > 0)
		PG_RETURN_NUMERIC(make_result(&const_nan));
	if (state->pInfcount > 0)
		PG_RETURN_NUMERIC(make_result(&const_pinf));
	if (state->nInfcount > 0)
		PG_RETURN_NUMERIC(make_result(&const_ninf));

	N_datum = NumericGetDatum(int64_to_numeric(state->N));

	init_var(&sumX_var);
	accum_sum_final(&state->sumX, &sumX_var);
	sumX_datum = NumericGetDatum(make_result(&sumX_var));
	free_var(&sumX_var);

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sumX_datum, N_datum));
}

Datum
numeric_sum(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	NumericVar	sumX_var;
	Numeric		result;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* 若没有非空输入，则返回 NULL */
	if (state == NULL || NA_TOTAL_COUNT(state) == 0)
		PG_RETURN_NULL();

	if (state->NaNcount > 0)	/* 至少有一个 NaN 输入 */
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/* 正无穷大与负无穷大相加得 NaN */
	if (state->pInfcount > 0 && state->nInfcount > 0)
		PG_RETURN_NUMERIC(make_result(&const_nan));
	if (state->pInfcount > 0)
		PG_RETURN_NUMERIC(make_result(&const_pinf));
	if (state->nInfcount > 0)
		PG_RETURN_NUMERIC(make_result(&const_ninf));

	init_var(&sumX_var);
	accum_sum_final(&state->sumX, &sumX_var);
	result = make_result(&sumX_var);
	free_var(&sumX_var);

	PG_RETURN_NUMERIC(result);
}

/*
 * 标准差与方差聚合的主力例程。
 * 'state' 是聚合的转移状态。
 * 'variance' 指定我们应计算的是方差还是
 * 标准差。'sample' 表明调用方关心样本还是总体
 * 的方差/标准差。
 *
 *
 * 若对输入而言相应的方差统计量无定义，
 * *is_null 被置为 true 并返回 NULL。
 */
static Numeric
numeric_stddev_internal(NumericAggState *state,
						bool variance, bool sample,
						bool *is_null)
{
	Numeric		res;
	NumericVar	vN,
				vsumX,
				vsumX2,
				vNminus1;
	int64		totCount;
	int			rscale;

	/*
	 * 样本标准差与方差在 N <= 1 时无定义；总体标准差在
	 * N == 0 时无定义。两种情况下都返回 NULL（注意 NaN
	 * 与无穷大在此都算作正常输入）。
	 */
	if (state == NULL || (totCount = NA_TOTAL_COUNT(state)) == 0)
	{
		*is_null = true;
		return NULL;
	}

	if (sample && totCount <= 1)
	{
		*is_null = true;
		return NULL;
	}

	*is_null = false;

	/*
	 * 处理 NaN 与无穷大的情形。参照 float8 函数的行为，
	 * 任何无穷大输入都会产生 NaN 输出。
	 */
	if (state->NaNcount > 0 || state->pInfcount > 0 || state->nInfcount > 0)
		return make_result(&const_nan);

	/* 好，进行正常计算 */
	init_var(&vN);
	init_var(&vsumX);
	init_var(&vsumX2);

	int64_to_numericvar(state->N, &vN);
	accum_sum_final(&(state->sumX), &vsumX);
	accum_sum_final(&(state->sumX2), &vsumX2);

	init_var(&vNminus1);
	sub_var(&vN, &const_one, &vNminus1);

	/* 为 mul_var 调用计算 rscale */
	rscale = vsumX.dscale * 2;

	mul_var(&vsumX, &vsumX, &vsumX, rscale);	/* vsumX = sumX * sumX */
	mul_var(&vN, &vsumX2, &vsumX2, rscale); /* vsumX2 = N * sumX2 */
	sub_var(&vsumX2, &vsumX, &vsumX2);	/* N * sumX2 - sumX * sumX */

	if (cmp_var(&vsumX2, &const_zero) <= 0)
	{
		/* 提防四舍五入误差使分子变负 */
		res = make_result(&const_zero);
	}
	else
	{
		if (sample)
			mul_var(&vN, &vNminus1, &vNminus1, 0);	/* N * (N - 1) */
		else
			mul_var(&vN, &vN, &vNminus1, 0);	/* N * N */
		rscale = select_div_scale(&vsumX2, &vNminus1);
		div_var(&vsumX2, &vNminus1, &vsumX, rscale, true);	/* 方差 */
		if (!variance)
			sqrt_var(&vsumX, &vsumX, rscale);	/* stddev */

		res = make_result(&vsumX);
	}

	free_var(&vNminus1);
	free_var(&vsumX);
	free_var(&vsumX2);

	return res;
}

Datum
numeric_var_samp(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	res = numeric_stddev_internal(state, true, true, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
}

Datum
numeric_stddev_samp(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	res = numeric_stddev_internal(state, false, true, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
}

Datum
numeric_var_pop(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	res = numeric_stddev_internal(state, true, false, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
}

Datum
numeric_stddev_pop(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	res = numeric_stddev_internal(state, false, false, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
}

#ifdef HAVE_INT128
static Numeric
numeric_poly_stddev_internal(Int128AggState *state,
							 bool variance, bool sample,
							 bool *is_null)
{
	NumericAggState numstate;
	Numeric		res;

	/* 初始化空的聚合状态 */
	memset(&numstate, 0, sizeof(NumericAggState));

	if (state)
	{
		NumericVar	tmp_var;

		numstate.N = state->N;

		init_var(&tmp_var);

		int128_to_numericvar(state->sumX, &tmp_var);
		accum_sum_add(&numstate.sumX, &tmp_var);

		int128_to_numericvar(state->sumX2, &tmp_var);
		accum_sum_add(&numstate.sumX2, &tmp_var);

		free_var(&tmp_var);
	}

	res = numeric_stddev_internal(&numstate, variance, sample, is_null);

	if (numstate.sumX.ndigits > 0)
	{
		pfree(numstate.sumX.pos_digits);
		pfree(numstate.sumX.neg_digits);
	}
	if (numstate.sumX2.ndigits > 0)
	{
		pfree(numstate.sumX2.pos_digits);
		pfree(numstate.sumX2.neg_digits);
	}

	return res;
}
#endif

Datum
numeric_poly_var_samp(PG_FUNCTION_ARGS)
{
#ifdef HAVE_INT128
	PolyNumAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	res = numeric_poly_stddev_internal(state, true, true, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
#else
	return numeric_var_samp(fcinfo);
#endif
}

Datum
numeric_poly_stddev_samp(PG_FUNCTION_ARGS)
{
#ifdef HAVE_INT128
	PolyNumAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	res = numeric_poly_stddev_internal(state, false, true, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
#else
	return numeric_stddev_samp(fcinfo);
#endif
}

Datum
numeric_poly_var_pop(PG_FUNCTION_ARGS)
{
#ifdef HAVE_INT128
	PolyNumAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	res = numeric_poly_stddev_internal(state, true, false, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
#else
	return numeric_var_pop(fcinfo);
#endif
}

Datum
numeric_poly_stddev_pop(PG_FUNCTION_ARGS)
{
#ifdef HAVE_INT128
	PolyNumAggState *state;
	Numeric		res;
	bool		is_null;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	res = numeric_poly_stddev_internal(state, false, false, &is_null);

	if (is_null)
		PG_RETURN_NULL();
	else
		PG_RETURN_NUMERIC(res);
#else
	return numeric_stddev_pop(fcinfo);
#endif
}

/*
 * 整数数据类型的 SUM 转移函数。
 *
 * 为避免溢出，使用比输入数据类型更宽的累加器。
 * int8 输入需要 Numeric 累加器；对 int4 与 int2
 * 输入，我们使用 int8 累加器，就实际用途应当足够。
 * （后两者因此其实并不属于本文件，
 * 但仍然留在这里。）
 *
 * 由于 SQL 把无值的 SUM() 定义为 NULL 而非零，
 * 转移数据值的初始条件需要为 NULL。这意味着
 * 我们不能指望 ExecAgg 自动把第一个非空数据值插
 * 入转移数据：它不知道如何做类型转换。
 * 其结果是这些例程必须标记为非严格（non-strict），
 * 并自行处理第一个非空输入的替换。
 *
 * 注意：这些函数只在普通聚合模式下使用。
 * 移动聚合模式下改用 intX_avg_accum 与 intX_avg_accum_inv。
 */

Datum
int2_sum(PG_FUNCTION_ARGS)
{
	int64		newval;

	if (PG_ARGISNULL(0))
	{
		/* 迄今未见非空输入…… */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* 无非空输入 */
		/* 这是第一个非空输入。 */
		newval = (int64) PG_GETARG_INT16(1);
		PG_RETURN_INT64(newval);
	}

	/*
	 * 若作为聚合被调用，可取巧就地修改第一个参数，
	 * 以避免 palloc 开销。否则我们需要返回
	 * 转移变量的新值。（若 int8 为按值传递，这既无用
	 * 也不正确，所以直接用条件编译把它排除掉。）
	 *
	 */
#ifndef USE_FLOAT8_BYVAL		/* 也控制 int8 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *oldsum = (int64 *) PG_GETARG_POINTER(0);

		/* 若新输入为 NULL，运行和保持不变 */
		if (!PG_ARGISNULL(1))
			*oldsum = *oldsum + (int64) PG_GETARG_INT16(1);

		PG_RETURN_POINTER(oldsum);
	}
	else
#endif
	{
		int64		oldsum = PG_GETARG_INT64(0);

		/* 新输入为 NULL 则和不变。 */
		if (PG_ARGISNULL(1))
			PG_RETURN_INT64(oldsum);

		/* 可以执行加法。 */
		newval = oldsum + (int64) PG_GETARG_INT16(1);

		PG_RETURN_INT64(newval);
	}
}

Datum
int4_sum(PG_FUNCTION_ARGS)
{
	int64		newval;

	if (PG_ARGISNULL(0))
	{
		/* 迄今未见非空输入…… */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* 无非空输入 */
		/* 这是第一个非空输入。 */
		newval = (int64) PG_GETARG_INT32(1);
		PG_RETURN_INT64(newval);
	}

	/*
	 * 若作为聚合被调用，可取巧就地修改第一个参数，
	 * 以避免 palloc 开销。否则我们需要返回
	 * 转移变量的新值。（若 int8 为按值传递，这既无用
	 * 也不正确，所以直接用条件编译把它排除掉。）
	 *
	 */
#ifndef USE_FLOAT8_BYVAL		/* 也控制 int8 */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *oldsum = (int64 *) PG_GETARG_POINTER(0);

		/* 若新输入为 NULL，运行和保持不变 */
		if (!PG_ARGISNULL(1))
			*oldsum = *oldsum + (int64) PG_GETARG_INT32(1);

		PG_RETURN_POINTER(oldsum);
	}
	else
#endif
	{
		int64		oldsum = PG_GETARG_INT64(0);

		/* 新输入为 NULL 则和不变。 */
		if (PG_ARGISNULL(1))
			PG_RETURN_INT64(oldsum);

		/* 可以执行加法。 */
		newval = oldsum + (int64) PG_GETARG_INT32(1);

		PG_RETURN_INT64(newval);
	}
}

/*
 * 注意：此函数已过时，不再用于 SUM(int8)。
 */
Datum
int8_sum(PG_FUNCTION_ARGS)
{
	Numeric		oldsum;

	if (PG_ARGISNULL(0))
	{
		/* 迄今未见非空输入…… */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* 无非空输入 */
		/* 这是第一个非空输入。 */
		PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT64(1)));
	}

	/*
	 * 注意我们不能像对 int2_sum 与 int4_sum 那样在这里
	 * 特判聚合情形：numeric 大小可变，不能就地修改第一
	 * 个参数。
	 */

	oldsum = PG_GETARG_NUMERIC(0);

	/* 新输入为 NULL 则和不变。 */
	if (PG_ARGISNULL(1))
		PG_RETURN_NUMERIC(oldsum);

	/* 可以执行加法。 */
	PG_RETURN_DATUM(DirectFunctionCall2(numeric_add,
										NumericGetDatum(oldsum),
										NumericGetDatum(int64_to_numeric(PG_GETARG_INT64(1)))));
}


/*
 * avg(int2) 与 avg(int4) 的例程。其转移数据类型是
 * 保存个数与和的二元 int8 数组。
 *
 * 这些函数也用于 sum(int2) 与 sum(int4)，此时
 * 处于移动聚合模式，因为正确的反向转移需要统计输
 * 入个数。
 */

typedef struct Int8TransTypeData
{
	int64		count;
	int64		sum;
} Int8TransTypeData;

Datum
int2_avg_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray;
	int16		newval = PG_GETARG_INT16(1);
	Int8TransTypeData *transdata;

	/*
	 * 若作为聚合被调用，可取巧就地修改第一个参数，
	 * 以降低 palloc 开销。否则我们需要在涂改它之前先复制
	 * 一份副本。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
		transarray = PG_GETARG_ARRAYTYPE_P(0);
	else
		transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	if (ARR_HASNULL(transarray) ||
		ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");

	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);
	transdata->count++;
	transdata->sum += newval;

	PG_RETURN_ARRAYTYPE_P(transarray);
}

Datum
int4_avg_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray;
	int32		newval = PG_GETARG_INT32(1);
	Int8TransTypeData *transdata;

	/*
	 * 若作为聚合被调用，可取巧就地修改第一个参数，
	 * 以降低 palloc 开销。否则我们需要在涂改它之前先复制
	 * 一份副本。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
		transarray = PG_GETARG_ARRAYTYPE_P(0);
	else
		transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	if (ARR_HASNULL(transarray) ||
		ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");

	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);
	transdata->count++;
	transdata->sum += newval;

	PG_RETURN_ARRAYTYPE_P(transarray);
}

Datum
int4_avg_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray1;
	ArrayType  *transarray2;
	Int8TransTypeData *state1;
	Int8TransTypeData *state2;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	transarray1 = PG_GETARG_ARRAYTYPE_P(0);
	transarray2 = PG_GETARG_ARRAYTYPE_P(1);

	if (ARR_HASNULL(transarray1) ||
		ARR_SIZE(transarray1) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");

	if (ARR_HASNULL(transarray2) ||
		ARR_SIZE(transarray2) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");

	state1 = (Int8TransTypeData *) ARR_DATA_PTR(transarray1);
	state2 = (Int8TransTypeData *) ARR_DATA_PTR(transarray2);

	state1->count += state2->count;
	state1->sum += state2->sum;

	PG_RETURN_ARRAYTYPE_P(transarray1);
}

Datum
int2_avg_accum_inv(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray;
	int16		newval = PG_GETARG_INT16(1);
	Int8TransTypeData *transdata;

	/*
	 * 若作为聚合被调用，可取巧就地修改第一个参数，
	 * 以降低 palloc 开销。否则我们需要在涂改它之前先复制
	 * 一份副本。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
		transarray = PG_GETARG_ARRAYTYPE_P(0);
	else
		transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	if (ARR_HASNULL(transarray) ||
		ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");

	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);
	transdata->count--;
	transdata->sum -= newval;

	PG_RETURN_ARRAYTYPE_P(transarray);
}

Datum
int4_avg_accum_inv(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray;
	int32		newval = PG_GETARG_INT32(1);
	Int8TransTypeData *transdata;

	/*
	 * 若作为聚合被调用，可取巧就地修改第一个参数，
	 * 以降低 palloc 开销。否则我们需要在涂改它之前先复制
	 * 一份副本。
	 */
	if (AggCheckCallContext(fcinfo, NULL))
		transarray = PG_GETARG_ARRAYTYPE_P(0);
	else
		transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

	if (ARR_HASNULL(transarray) ||
		ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");

	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);
	transdata->count--;
	transdata->sum -= newval;

	PG_RETURN_ARRAYTYPE_P(transarray);
}

Datum
int8_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Int8TransTypeData *transdata;
	Datum		countd,
				sumd;

	if (ARR_HASNULL(transarray) ||
		ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");
	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);

	/* SQL 定义无值的 AVG 为 NULL */
	if (transdata->count == 0)
		PG_RETURN_NULL();

	countd = NumericGetDatum(int64_to_numeric(transdata->count));
	sumd = NumericGetDatum(int64_to_numeric(transdata->sum));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sumd, countd));
}

/*
 * SUM(int2) 与 SUM(int4) 都返回 int8，因此两者
 * 均可用此终函数。
 */
Datum
int2int4_sum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Int8TransTypeData *transdata;

	if (ARR_HASNULL(transarray) ||
		ARR_SIZE(transarray) != ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "expected 2-element int8 array");
	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);

	/* SQL 定义无值的 SUM 为 NULL */
	if (transdata->count == 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(Int64GetDatumFast(transdata->sum));
}


/* ----------------------------------------------------------------------
 *
 * 调试支持
 *
 * ----------------------------------------------------------------------
 */

#ifdef NUMERIC_DEBUG

/*
 * dump_numeric() - 以数据库存储格式转储值以便调试
 */
static void
dump_numeric(const char *str, Numeric num)
{
	NumericDigit *digits = NUMERIC_DIGITS(num);
	int			ndigits;
	int			i;

	ndigits = NUMERIC_NDIGITS(num);

	printf("%s: NUMERIC w=%d d=%d ", str,
		   NUMERIC_WEIGHT(num), NUMERIC_DSCALE(num));
	switch (NUMERIC_SIGN(num))
	{
		case NUMERIC_POS:
			printf("POS");
			break;
		case NUMERIC_NEG:
			printf("NEG");
			break;
		case NUMERIC_NAN:
			printf("NaN");
			break;
		case NUMERIC_PINF:
			printf("Infinity");
			break;
		case NUMERIC_NINF:
			printf("-Infinity");
			break;
		default:
			printf("SIGN=0x%x", NUMERIC_SIGN(num));
			break;
	}

	for (i = 0; i < ndigits; i++)
		printf(" %0*d", DEC_DIGITS, digits[i]);
	printf("\n");
}


/*
 * dump_var() - 以变量格式转储值以便调试
 */
static void
dump_var(const char *str, NumericVar *var)
{
	int			i;

	printf("%s: VAR w=%d d=%d ", str, var->weight, var->dscale);
	switch (var->sign)
	{
		case NUMERIC_POS:
			printf("POS");
			break;
		case NUMERIC_NEG:
			printf("NEG");
			break;
		case NUMERIC_NAN:
			printf("NaN");
			break;
		case NUMERIC_PINF:
			printf("Infinity");
			break;
		case NUMERIC_NINF:
			printf("-Infinity");
			break;
		default:
			printf("SIGN=0x%x", var->sign);
			break;
	}

	for (i = 0; i < var->ndigits; i++)
		printf(" %0*d", DEC_DIGITS, var->digits[i]);

	printf("\n");
}
#endif							/* NUMERIC_DEBUG */


/* ----------------------------------------------------------------------
 *
 * 以下为局部函数
 *
 * 通常它们不支持"特殊"（NaN 或无穷大）输入；
 * 调用方应先处理那些可能性。
 * （有一两个例外，见它们各自头部的注释。）
 *
 * ----------------------------------------------------------------------
 */


/*
 * alloc_var() -
 *
 *	分配 ndigits 个数字位的缓冲区（外加一个四舍五入位）
 */
static void
alloc_var(NumericVar *var, int ndigits)
{
	digitbuf_free(var->buf);
	var->buf = digitbuf_alloc(ndigits + 1);
	var->buf[0] = 0;			/* 四舍五入备用位 */
	var->digits = var->buf + 1;
	var->ndigits = ndigits;
}


/*
 * free_var() -
 *
 *	把变量的数字位缓冲区归还空闲池
 */
static void
free_var(NumericVar *var)
{
	digitbuf_free(var->buf);
	var->buf = NULL;
	var->digits = NULL;
	var->sign = NUMERIC_NAN;
}


/*
 * zero_var() -
 *
 *	把变量设为零。
 *	注意：显示标度不变。
 */
static void
zero_var(NumericVar *var)
{
	digitbuf_free(var->buf);
	var->buf = NULL;
	var->digits = NULL;
	var->ndigits = 0;
	var->weight = 0;			/* 按惯例；实际无关紧要 */
	var->sign = NUMERIC_POS;	/* 非 NAN 值…… */
}


/*
 * set_var_from_str()
 *
 *	解析字符串并把数字放入变量
 *
 * 本函数不处理前导或尾随空格。它把解析得到的 end+1
 * 位置存入 *endptr，以便调用方在需要时检查尾随空
 * 格/垃圾。
 *
 * cp 是实际开始解析的位置；str 是错误报告所用的
 * 字符串。（通常 cp 与之相同，只是跳过了空格。）
 *
 * 成功返回 true，失败返回 false（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 */
static bool
set_var_from_str(const char *str, const char *cp,
				 NumericVar *dest, const char **endptr,
				 Node *escontext)
{
	bool		have_dp = false;
	int			i;
	unsigned char *decdigits;
	int			sign = NUMERIC_POS;
	int			dweight = -1;
	int			ddigits;
	int			dscale = 0;
	int			weight;
	int			ndigits;
	int			offset;
	NumericDigit *digits;

	/*
	 * 我们解析字符串以提取十进制数字位并确定正确的十
	 * 进制权重。然后转换为 NBASE 表示。
	 */
	switch (*cp)
	{
		case '+':
			sign = NUMERIC_POS;
			cp++;
			break;

		case '-':
			sign = NUMERIC_NEG;
			cp++;
			break;
	}

	if (*cp == '.')
	{
		have_dp = true;
		cp++;
	}

	if (!isdigit((unsigned char) *cp))
		goto invalid_syntax;

	decdigits = (unsigned char *) palloc(strlen(cp) + DEC_DIGITS * 2);

	/* 为稍后数字位对齐做前导填充 */
	memset(decdigits, 0, DEC_DIGITS);
	i = DEC_DIGITS;

	while (*cp)
	{
		if (isdigit((unsigned char) *cp))
		{
			decdigits[i++] = *cp++ - '0';
			if (!have_dp)
				dweight++;
			else
				dscale++;
		}
		else if (*cp == '.')
		{
			if (have_dp)
				goto invalid_syntax;
			have_dp = true;
			cp++;
			/* 小数点后不得紧跟下划线 */
			if (*cp == '_')
				goto invalid_syntax;
		}
		else if (*cp == '_')
		{
			/* 下划线后必须还有更多数字位 */
			cp++;
			if (!isdigit((unsigned char) *cp))
				goto invalid_syntax;
		}
		else
			break;
	}

	ddigits = i - DEC_DIGITS;
	/* 为稍后数字位对齐做尾随填充 */
	memset(decdigits + i, 0, DEC_DIGITS - 1);

	/* 如有指数则处理 */
	if (*cp == 'e' || *cp == 'E')
	{
		int64		exponent = 0;
		bool		neg = false;

		/*
		 * 此时，dweight 与 dscale 至多约为 INT_MAX/2，因
		 * 字符串长度受 MaxAllocSize 限制，类似约束指数
		 * 即可防止本函数整数溢出。若值过大放不进存储
		 * 格式，make_result() 稍后会报错；为一致起见，
		 * 使用与 make_result() 相同的 ereport errcode/text。
		 *
		 */

		/* 指数符号 */
		cp++;
		if (*cp == '+')
			cp++;
		else if (*cp == '-')
		{
			neg = true;
			cp++;
		}

		/* 指数数字位 */
		if (!isdigit((unsigned char) *cp))
			goto invalid_syntax;

		while (*cp)
		{
			if (isdigit((unsigned char) *cp))
			{
				exponent = exponent * 10 + (*cp++ - '0');
				if (exponent > PG_INT32_MAX / 2)
					goto out_of_range;
			}
			else if (*cp == '_')
			{
				/* 下划线后必须还有更多数字位 */
				cp++;
				if (!isdigit((unsigned char) *cp))
					goto invalid_syntax;
			}
			else
				break;
		}

		if (neg)
			exponent = -exponent;

		dweight += (int) exponent;
		dscale -= (int) exponent;
		if (dscale < 0)
			dscale = 0;
	}

	/*
	 * 好，把纯十进制表示转换成 NBASE 基。首先须确定转换
	 * 后的权重与 ndigits。offset 是在第一个给定数字位之前
	 * 要插入的十进制零个数，以便首个 NBASE 位对齐。
	 *
	 */
	if (dweight >= 0)
		weight = (dweight + 1 + DEC_DIGITS - 1) / DEC_DIGITS - 1;
	else
		weight = -((-dweight - 1) / DEC_DIGITS + 1);
	offset = (weight + 1) * DEC_DIGITS - (dweight + 1);
	ndigits = (ddigits + offset + DEC_DIGITS - 1) / DEC_DIGITS;

	alloc_var(dest, ndigits);
	dest->sign = sign;
	dest->weight = weight;
	dest->dscale = dscale;

	i = DEC_DIGITS - offset;
	digits = dest->digits;

	while (ndigits-- > 0)
	{
#if DEC_DIGITS == 4
		*digits++ = ((decdigits[i] * 10 + decdigits[i + 1]) * 10 +
					 decdigits[i + 2]) * 10 + decdigits[i + 3];
#elif DEC_DIGITS == 2
		*digits++ = decdigits[i] * 10 + decdigits[i + 1];
#elif DEC_DIGITS == 1
		*digits++ = decdigits[i];
#else
#error unsupported NBASE
#endif
		i += DEC_DIGITS;
	}

	pfree(decdigits);

	/* 去除所有前导/尾随零，若为零则归一化权重 */
	strip_var(dest);

	/* 返回 end+1 位置给调用方 */
	*endptr = cp;

	return true;

out_of_range:
	ereturn(escontext, false,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value overflows numeric format")));

invalid_syntax:
	ereturn(escontext, false,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"numeric", str)));
}


/*
 * 返回单个十六进制数字位的数值。
 */
static inline int
xdigit_value(char dig)
{
	return dig >= '0' && dig <= '9' ? dig - '0' :
		dig >= 'a' && dig <= 'f' ? dig - 'a' + 10 :
		dig >= 'A' && dig <= 'F' ? dig - 'A' + 10 : -1;
}

/*
 * set_var_from_non_decimal_integer_str()
 *
 *	解析包含非十进制整数的字符串
 *
 * 本函数不处理前导或尾随空格。它把解析得到的 end+1
 * 位置存入 *endptr，以便调用方在需要时检查尾随空
 * 格/垃圾。
 *
 * cp 是实际开始解析的位置；str 是错误报告所用的
 * 字符串。数字的符号与进制前缀指示符（如 "0x"）被
 * 假定已解析完毕，所以 cp 应指向数字在指定进制下的
 * 第一个数字位。
 *
 * base 应为 2、8 或 16。
 *
 * 成功返回 true，失败返回 false（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 */
static bool
set_var_from_non_decimal_integer_str(const char *str, const char *cp, int sign,
									 int base, NumericVar *dest,
									 const char **endptr, Node *escontext)
{
	const char *firstdigit = cp;
	int64		tmp;
	int64		mul;
	NumericVar	tmp_var;

	init_var(&tmp_var);

	zero_var(dest);

	/*
	 * 以能放进 int64 的组为单位处理输入数字位。"tmp"是
	 * 该组数字位的值，"mul" 为 base^n，其中 n 是组内数字
	 * 位的个数。因此 tmp < mul，当 mul * base 可能溢出
	 * PG_INT64_MAX 时必须开新组。
	 */
	tmp = 0;
	mul = 1;

	if (base == 16)
	{
		while (*cp)
		{
			if (isxdigit((unsigned char) *cp))
			{
				if (mul > PG_INT64_MAX / 16)
				{
					/* 加上这组数字位的贡献 */
					int64_to_numericvar(mul, &tmp_var);
					mul_var(dest, &tmp_var, dest, 0);
					int64_to_numericvar(tmp, &tmp_var);
					add_var(dest, &tmp_var, dest);

					/* 若权重溢出 int16 则结果将溢出 */
					if (dest->weight > NUMERIC_WEIGHT_MAX)
						goto out_of_range;

					/* 开新组 */
					tmp = 0;
					mul = 1;
				}

				tmp = tmp * 16 + xdigit_value(*cp++);
				mul = mul * 16;
			}
			else if (*cp == '_')
			{
				/* 下划线后必须还有更多数字位 */
				cp++;
				if (!isxdigit((unsigned char) *cp))
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (base == 8)
	{
		while (*cp)
		{
			if (*cp >= '0' && *cp <= '7')
			{
				if (mul > PG_INT64_MAX / 8)
				{
					/* 加上这组数字位的贡献 */
					int64_to_numericvar(mul, &tmp_var);
					mul_var(dest, &tmp_var, dest, 0);
					int64_to_numericvar(tmp, &tmp_var);
					add_var(dest, &tmp_var, dest);

					/* 若权重溢出 int16 则结果将溢出 */
					if (dest->weight > NUMERIC_WEIGHT_MAX)
						goto out_of_range;

					/* 开新组 */
					tmp = 0;
					mul = 1;
				}

				tmp = tmp * 8 + (*cp++ - '0');
				mul = mul * 8;
			}
			else if (*cp == '_')
			{
				/* 下划线后必须还有更多数字位 */
				cp++;
				if (*cp < '0' || *cp > '7')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else if (base == 2)
	{
		while (*cp)
		{
			if (*cp >= '0' && *cp <= '1')
			{
				if (mul > PG_INT64_MAX / 2)
				{
					/* 加上这组数字位的贡献 */
					int64_to_numericvar(mul, &tmp_var);
					mul_var(dest, &tmp_var, dest, 0);
					int64_to_numericvar(tmp, &tmp_var);
					add_var(dest, &tmp_var, dest);

					/* 若权重溢出 int16 则结果将溢出 */
					if (dest->weight > NUMERIC_WEIGHT_MAX)
						goto out_of_range;

					/* 开新组 */
					tmp = 0;
					mul = 1;
				}

				tmp = tmp * 2 + (*cp++ - '0');
				mul = mul * 2;
			}
			else if (*cp == '_')
			{
				/* 下划线后必须还有更多数字位 */
				cp++;
				if (*cp < '0' || *cp > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
		/* 不应发生；视为无效输入 */
		goto invalid_syntax;

	/* 检查至少得到一个数字位 */
	if (unlikely(cp == firstdigit))
		goto invalid_syntax;

	/* 加上最后一组数字位的贡献 */
	int64_to_numericvar(mul, &tmp_var);
	mul_var(dest, &tmp_var, dest, 0);
	int64_to_numericvar(tmp, &tmp_var);
	add_var(dest, &tmp_var, dest);

	if (dest->weight > NUMERIC_WEIGHT_MAX)
		goto out_of_range;

	dest->sign = sign;

	free_var(&tmp_var);

	/* 返回 end+1 位置给调用方 */
	*endptr = cp;

	return true;

out_of_range:
	ereturn(escontext, false,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value overflows numeric format")));

invalid_syntax:
	ereturn(escontext, false,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"numeric", str)));
}


/*
 * set_var_from_num() -
 *
 *	把打包的数据库格式转换为变量
 */
static void
set_var_from_num(Numeric num, NumericVar *dest)
{
	int			ndigits;

	ndigits = NUMERIC_NDIGITS(num);

	alloc_var(dest, ndigits);

	dest->weight = NUMERIC_WEIGHT(num);
	dest->sign = NUMERIC_SIGN(num);
	dest->dscale = NUMERIC_DSCALE(num);

	memcpy(dest->digits, NUMERIC_DIGITS(num), ndigits * sizeof(NumericDigit));
}


/*
 * init_var_from_num() -
 *
 *	从打包 db 格式初始化变量。数字位数组并不复制，
 *	当所得变量不被修改时可省去一些开销。
 *	此外，只要不给它赋任何其他值（用 set_var_* 函数，或
 *	把它当作诸如 add_var() 这类函数的目的地），就无
 *	需调用 free_var()。
 *
 *	注意：不要修改用本函数初始化的变量的数字位缓冲
 *	区，例如调用 round_var() 或 trunc_var()，否则改动会
 *	传播回原 Numeric！不过把它用作计算类函数的
 *	目的参数则没有问题。
 */
static void
init_var_from_num(Numeric num, NumericVar *dest)
{
	dest->ndigits = NUMERIC_NDIGITS(num);
	dest->weight = NUMERIC_WEIGHT(num);
	dest->sign = NUMERIC_SIGN(num);
	dest->dscale = NUMERIC_DSCALE(num);
	dest->digits = NUMERIC_DIGITS(num);
	dest->buf = NULL;			/* 数字位数组未经 palloc */
}


/*
 * set_var_from_var() -
 *
 *	把变量复制进另一个
 */
static void
set_var_from_var(const NumericVar *value, NumericVar *dest)
{
	NumericDigit *newbuf;

	newbuf = digitbuf_alloc(value->ndigits + 1);
	newbuf[0] = 0;				/* 四舍五入备用位 */
	if (value->ndigits > 0)		/* 否则 value->digits 或为空 */
		memcpy(newbuf + 1, value->digits,
			   value->ndigits * sizeof(NumericDigit));

	digitbuf_free(dest->buf);

	memmove(dest, value, sizeof(NumericVar));
	dest->buf = newbuf;
	dest->digits = newbuf + 1;
}


/*
 * get_str_from_var() -
 *
 *	把变量转换成文本表示（numeric_out 核心）。
 *	变量按其显示标度所指示的数字位个数显示。
 *	返回 palloc 的串。
 */
static char *
get_str_from_var(const NumericVar *var)
{
	int			dscale;
	char	   *str;
	char	   *cp;
	char	   *endcp;
	int			i;
	int			d;
	NumericDigit dig;

#if DEC_DIGITS > 1
	NumericDigit d1;
#endif

	dscale = var->dscale;

	/*
	 * 为结果分配空间。
	 *
	 * i 被设为小数点前的十进制数字位个数。dscale 是我们
	 * 将在小数点后打印的十进制数字位个数。我们可能
	 * 在末尾多生成多达 DEC_DIGITS-1 个多余数字位，另外
	 * 还需要符号、小数点与结尾 NUL 空间。
	 */
	i = (var->weight + 1) * DEC_DIGITS;
	if (i <= 0)
		i = 1;

	str = palloc(i + dscale + DEC_DIGITS + 2);
	cp = str;

	/*
	 * 负值输出一个减号
	 */
	if (var->sign == NUMERIC_NEG)
		*cp++ = '-';

	/*
	 * 输出小数点前的所有数字位
	 */
	if (var->weight < 0)
	{
		d = var->weight + 1;
		*cp++ = '0';
	}
	else
	{
		for (d = 0; d <= var->weight; d++)
		{
			dig = (d < var->ndigits) ? var->digits[d] : 0;
			/* 首个数字位中压制多余的前导十进制零 */
#if DEC_DIGITS == 4
			{
				bool		putit = (d > 0);

				d1 = dig / 1000;
				dig -= d1 * 1000;
				putit |= (d1 > 0);
				if (putit)
					*cp++ = d1 + '0';
				d1 = dig / 100;
				dig -= d1 * 100;
				putit |= (d1 > 0);
				if (putit)
					*cp++ = d1 + '0';
				d1 = dig / 10;
				dig -= d1 * 10;
				putit |= (d1 > 0);
				if (putit)
					*cp++ = d1 + '0';
				*cp++ = dig + '0';
			}
#elif DEC_DIGITS == 2
			d1 = dig / 10;
			dig -= d1 * 10;
			if (d1 > 0 || d > 0)
				*cp++ = d1 + '0';
			*cp++ = dig + '0';
#elif DEC_DIGITS == 1
			*cp++ = dig + '0';
#else
#error unsupported NBASE
#endif
		}
	}

	/*
	 * 若需要，输出小数点及其后面的所有数字位。最初
	 * 会先输出 DEC_DIGITS 的倍数个数字位，若需要再截断。
	 *
	 */
	if (dscale > 0)
	{
		*cp++ = '.';
		endcp = cp + dscale;
		for (i = 0; i < dscale; d++, i += DEC_DIGITS)
		{
			dig = (d >= 0 && d < var->ndigits) ? var->digits[d] : 0;
#if DEC_DIGITS == 4
			d1 = dig / 1000;
			dig -= d1 * 1000;
			*cp++ = d1 + '0';
			d1 = dig / 100;
			dig -= d1 * 100;
			*cp++ = d1 + '0';
			d1 = dig / 10;
			dig -= d1 * 10;
			*cp++ = d1 + '0';
			*cp++ = dig + '0';
#elif DEC_DIGITS == 2
			d1 = dig / 10;
			dig -= d1 * 10;
			*cp++ = d1 + '0';
			*cp++ = dig + '0';
#elif DEC_DIGITS == 1
			*cp++ = dig + '0';
#else
#error unsupported NBASE
#endif
		}
		cp = endcp;
	}

	/*
	 * 终止字符串并返回它
	 */
	*cp = '\0';
	return str;
}

/*
 * get_str_from_var_sci() -
 *
 *	把变量转换成规范化科学计数法文本表示。
 *	本函数为 numeric_out_sci() 承担重活。
 *
 *	这种记法的一般形式为 a * 10^b，其中 a 称为
 *	"尾数"，b 称为"指数"。
 *
 *	由于 ASCII 中无法使用上标（也因为我们想复刻 printf
 *	的行为），我们用 E 记法显示指数，指数数字位至
 *	少两位。
 *
 *	例如值 1234 可输出为 1.2e+03。
 *
 *	我们假定指数能放入 int32。
 *
 *	rscale 是输出中希望在小数点后保留的十进制数字位个
 *	数，负值将被视为零。
 *
 *	返回 palloc 的串。
 */
static char *
get_str_from_var_sci(const NumericVar *var, int rscale)
{
	int32		exponent;
	NumericVar	tmp_var;
	size_t		len;
	char	   *str;
	char	   *sig_out;

	if (rscale < 0)
		rscale = 0;

	/*
	 * 以规范化形式确定此数的指数。
	 *
	 * 这是该数以小数点前仅一位有效数字表示时所需的
	 * 指数。
	 */
	if (var->ndigits > 0)
	{
		exponent = (var->weight + 1) * DEC_DIGITS;

		/*
		 * 递减指数以补偿首个 numeric 数字位的前导十进制零。
		 *
		 */
		exponent -= DEC_DIGITS - (int) log10(var->digits[0]);
	}
	else
	{
		/*
		 * 若变量无数字位，则它必为零。
		 *
		 * 严格说零没有有意义的指数，但为了输出一致，
		 * 我们只把指数显示为零。
		 *
		 */
		exponent = 0;
	}

	/*
	 * 把变量除以 10^exponent 得到尾数，过程中四舍五入到
	 * rscale 位小数。
	 */
	init_var(&tmp_var);

	power_ten_int(exponent, &tmp_var);
	div_var(var, &tmp_var, &tmp_var, rscale, true);
	sig_out = get_str_from_var(&tmp_var);

	free_var(&tmp_var);

	/*
	 * 为结果分配空间。
	 *
	 * 除尾数，还需为指数装饰（"e"）、指数符号
	 * 与指数本身（至多 10 位数字）留出空间，还有结尾
	 * NUL 终止符。
	 */
	len = strlen(sig_out) + 13;
	str = palloc(len);
	snprintf(str, len, "%se%+03d", sig_out, exponent);

	pfree(sig_out);

	return str;
}


/*
 * numericvar_serialize - 序列化 NumericVar 为二进制格式
 *
 * 在变量层面，不对权重或显示标度做检查，允许
 * 我们传递精度高于 numeric 类型所支持程度的中间值。
 * 注意：这与 numeric_send/recv() 不兼容，后者对这些
 * 字段使用 16 位整数。
 */
static void
numericvar_serialize(StringInfo buf, const NumericVar *var)
{
	int			i;

	pq_sendint32(buf, var->ndigits);
	pq_sendint32(buf, var->weight);
	pq_sendint32(buf, var->sign);
	pq_sendint32(buf, var->dscale);
	for (i = 0; i < var->ndigits; i++)
		pq_sendint16(buf, var->digits[i]);
}

/*
 * numericvar_deserialize - 把二进制反序列化为 NumericVar
 */
static void
numericvar_deserialize(StringInfo buf, NumericVar *var)
{
	int			len,
				i;

	len = pq_getmsgint(buf, sizeof(int32));

	alloc_var(var, len);		/* 设 var->ndigits */

	var->weight = pq_getmsgint(buf, sizeof(int32));
	var->sign = pq_getmsgint(buf, sizeof(int32));
	var->dscale = pq_getmsgint(buf, sizeof(int32));
	for (i = 0; i < len; i++)
		var->digits[i] = pq_getmsgint(buf, sizeof(int16));
}


/*
 * duplicate_numeric() - 复制打包格式 Numeric
 *
 * 这会处理 NaN 与无穷大情形。
 */
static Numeric
duplicate_numeric(Numeric num)
{
	Numeric		res;

	res = (Numeric) palloc(VARSIZE(num));
	memcpy(res, num, VARSIZE(num));
	return res;
}

/*
 * make_result_opt_error() -
 *
 *	在 palloc() 内存中由变量创建打包 numeric 格式。
 *	这会处理 NaN 与无穷大情形。
 *
 *	若"have_error"非 NULL，溢出时 *have_error 被置为
 *	true 并返回 NULL。调用方需要自己处理错误时很有用。
 */
static Numeric
make_result_opt_error(const NumericVar *var, bool *have_error)
{
	Numeric		result;
	NumericDigit *digits = var->digits;
	int			weight = var->weight;
	int			sign = var->sign;
	int			n;
	Size		len;

	if (have_error)
		*have_error = false;

	if ((sign & NUMERIC_SIGN_MASK) == NUMERIC_SPECIAL)
	{
		/*
		 * 验证特殊值有效。这本可只是 Assert，但花几个周期
		 * 确保绝不向磁盘写入非零保留位是值得的。
		 *
		 */
		if (!(sign == NUMERIC_NAN ||
			  sign == NUMERIC_PINF ||
			  sign == NUMERIC_NINF))
			elog(ERROR, "invalid numeric sign value 0x%x", sign);

		result = (Numeric) palloc(NUMERIC_HDRSZ_SHORT);

		SET_VARSIZE(result, NUMERIC_HDRSZ_SHORT);
		result->choice.n_header = sign;
		/* 头字即所需全部 */

		dump_numeric("make_result()", result);
		return result;
	}

	n = var->ndigits;

	/* 截去前导零 */
	while (n > 0 && *digits == 0)
	{
		digits++;
		weight--;
		n--;
	}
	/* 截去尾随零 */
	while (n > 0 && digits[n - 1] == 0)
		n--;

	/* 若结果为零，强制 weight=0 且符号为正 */
	if (n == 0)
	{
		weight = 0;
		sign = NUMERIC_POS;
	}

	/* 构建结果 */
	if (NUMERIC_CAN_BE_SHORT(var->dscale, weight))
	{
		len = NUMERIC_HDRSZ_SHORT + n * sizeof(NumericDigit);
		result = (Numeric) palloc(len);
		SET_VARSIZE(result, len);
		result->choice.n_short.n_header =
			(sign == NUMERIC_NEG ? (NUMERIC_SHORT | NUMERIC_SHORT_SIGN_MASK)
			 : NUMERIC_SHORT)
			| (var->dscale << NUMERIC_SHORT_DSCALE_SHIFT)
			| (weight < 0 ? NUMERIC_SHORT_WEIGHT_SIGN_MASK : 0)
			| (weight & NUMERIC_SHORT_WEIGHT_MASK);
	}
	else
	{
		len = NUMERIC_HDRSZ + n * sizeof(NumericDigit);
		result = (Numeric) palloc(len);
		SET_VARSIZE(result, len);
		result->choice.n_long.n_sign_dscale =
			sign | (var->dscale & NUMERIC_DSCALE_MASK);
		result->choice.n_long.n_weight = weight;
	}

	Assert(NUMERIC_NDIGITS(result) == n);
	if (n > 0)
		memcpy(NUMERIC_DIGITS(result), digits, n * sizeof(NumericDigit));

	/* 检查 int16 字段是否溢出 */
	if (NUMERIC_WEIGHT(result) != weight ||
		NUMERIC_DSCALE(result) != var->dscale)
	{
		if (have_error)
		{
			*have_error = true;
			return NULL;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value overflows numeric format")));
		}
	}

	dump_numeric("make_result()", result);
	return result;
}


/*
 * make_result() -
 *
 *	make_result_opt_error() 的无 "have_error" 参数接口。
 */
static Numeric
make_result(const NumericVar *var)
{
	return make_result_opt_error(var, NULL);
}


/*
 * apply_typmod() -
 *
 *	按指定 typmod 做边界检查与四舍五入。
 *	注意这只应用于普通有限值。
 *
 * 成功返回 true，失败返回 false（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 */
static bool
apply_typmod(NumericVar *var, int32 typmod, Node *escontext)
{
	int			precision;
	int			scale;
	int			maxdigits;
	int			ddigits;
	int			i;

	/* 若 typmod 无效则不做事 */
	if (!is_valid_numeric_typmod(typmod))
		return true;

	precision = numeric_typmod_precision(typmod);
	scale = numeric_typmod_scale(typmod);
	maxdigits = precision - scale;

	/* 舍入到目标标度并设 var->dscale */
	round_var(var, scale);

	/* 但不允许 var->dscale 为负 */
	if (var->dscale < 0)
		var->dscale = 0;

	/*
	 * 检查溢出 - 注意不能在四舍五入之前做这一步，
	 * 四舍五入可能抬高权重。还要注意变量权重可能因前
	 * 导零而虚高，存盘前会去除，但可能尚未去除。
	 * 无论如何，我们必须识别真零，其权重没有意义。
	 *
	 */
	ddigits = (var->weight + 1) * DEC_DIGITS;
	if (ddigits > maxdigits)
	{
		/* 确定真权重；并检查全零结果 */
		for (i = 0; i < var->ndigits; i++)
		{
			NumericDigit dig = var->digits[i];

			if (dig)
			{
				/* 针对高位十进制零数字位作调整 */
#if DEC_DIGITS == 4
				if (dig < 10)
					ddigits -= 3;
				else if (dig < 100)
					ddigits -= 2;
				else if (dig < 1000)
					ddigits -= 1;
#elif DEC_DIGITS == 2
				if (dig < 10)
					ddigits -= 1;
#elif DEC_DIGITS == 1
				/* 不调整 */
#else
#error unsupported NBASE
#endif
				if (ddigits > maxdigits)
					ereturn(escontext, false,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("numeric field overflow"),
							 errdetail("A field with precision %d, scale %d must round to an absolute value less than %s%d.",
									   precision, scale,
					/* 10^0 显示为 1 */
									   maxdigits ? "10^" : "",
									   maxdigits ? maxdigits : 1
									   )));
				break;
			}
			ddigits -= DEC_DIGITS;
		}
	}

	return true;
}

/*
 * apply_typmod_special() -
 *
 *	对 Inf 或 NaN 按指定 typmod 做边界检查。
 *	为方便多数调用方，值以打包形式提供。
 *
 * 成功返回 true，失败返回 false（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 */
static bool
apply_typmod_special(Numeric num, int32 typmod, Node *escontext)
{
	int			precision;
	int			scale;

	Assert(NUMERIC_IS_SPECIAL(num));	/* 否则调用方错 */

	/*
	 * 无论 typmod 为何都允许 NaN；这也许颇为可疑，但这是
	 * 长期行为。有任何 typmod 限制时 Inf 会被拒绝，
	 * 因为无穷大不应被声称能放进任何有限位数中。
	 *
	 */
	if (NUMERIC_IS_NAN(num))
		return true;

	/* typmod 为默认值（-1）时不做事 */
	if (!is_valid_numeric_typmod(typmod))
		return true;

	precision = numeric_typmod_precision(typmod);
	scale = numeric_typmod_scale(typmod);

	ereturn(escontext, false,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("numeric field overflow"),
			 errdetail("A field with precision %d, scale %d cannot hold an infinite value.",
					   precision, scale)));
}


/*
 * numeric 转 int8，必要时四舍五入。
 *
 * 若溢出，返回 false（不抛错）。正常则返回 true。
 */
static bool
numericvar_to_int64(const NumericVar *var, int64 *result)
{
	NumericDigit *digits;
	int			ndigits;
	int			weight;
	int			i;
	int64		val;
	bool		neg;
	NumericVar	rounded;

	/* 舍入到最近整数 */
	init_var(&rounded);
	set_var_from_var(var, &rounded);
	round_var(&rounded, 0);

	/* 检查零输入 */
	strip_var(&rounded);
	ndigits = rounded.ndigits;
	if (ndigits == 0)
	{
		*result = 0;
		free_var(&rounded);
		return true;
	}

	/*
	 * 对 10000000000 这类输入，必须把已去除的数字位当作
	 * 真实。故循环假定小数点前有 weight+1 个数字位。
	 */
	weight = rounded.weight;
	Assert(weight >= 0 && ndigits <= weight + 1);

	/*
	 * 构造结果。为免 INT64_MIN 转换问题，按负数累
	 * 加该值（它无法表示为正的 64 位二进制补码整数）。
	 *
	 */
	digits = rounded.digits;
	neg = (rounded.sign == NUMERIC_NEG);
	val = -digits[0];
	for (i = 1; i <= weight; i++)
	{
		if (unlikely(pg_mul_s64_overflow(val, NBASE, &val)))
		{
			free_var(&rounded);
			return false;
		}

		if (i < ndigits)
		{
			if (unlikely(pg_sub_s64_overflow(val, digits[i], &val)))
			{
				free_var(&rounded);
				return false;
			}
		}
	}

	free_var(&rounded);

	if (!neg)
	{
		if (unlikely(val == PG_INT64_MIN))
			return false;
		val = -val;
	}
	*result = val;

	return true;
}

/*
 * 把 int8 值转为 numeric。
 */
static void
int64_to_numericvar(int64 val, NumericVar *var)
{
	uint64		uval,
				newuval;
	NumericDigit *ptr;
	int			ndigits;

	/* int64 至多需要 19 个十进制数字位；为安全加一 */
	alloc_var(var, 20 / DEC_DIGITS);
	if (val < 0)
	{
		var->sign = NUMERIC_NEG;
		uval = -val;
	}
	else
	{
		var->sign = NUMERIC_POS;
		uval = val;
	}
	var->dscale = 0;
	if (val == 0)
	{
		var->ndigits = 0;
		var->weight = 0;
		return;
	}
	ptr = var->digits + var->ndigits;
	ndigits = 0;
	do
	{
		ptr--;
		ndigits++;
		newuval = uval / NBASE;
		*ptr = uval - newuval * NBASE;
		uval = newuval;
	} while (uval);
	var->digits = ptr;
	var->ndigits = ndigits;
	var->weight = ndigits - 1;
}

/*
 * numeric 转 uint64，必要时四舍五入。
 *
 * 若溢出，返回 false（不抛错）。正常则返回 true。
 */
static bool
numericvar_to_uint64(const NumericVar *var, uint64 *result)
{
	NumericDigit *digits;
	int			ndigits;
	int			weight;
	int			i;
	uint64		val;
	NumericVar	rounded;

	/* 舍入到最近整数 */
	init_var(&rounded);
	set_var_from_var(var, &rounded);
	round_var(&rounded, 0);

	/* 检查零输入 */
	strip_var(&rounded);
	ndigits = rounded.ndigits;
	if (ndigits == 0)
	{
		*result = 0;
		free_var(&rounded);
		return true;
	}

	/* 检查负输入 */
	if (rounded.sign == NUMERIC_NEG)
	{
		free_var(&rounded);
		return false;
	}

	/*
	 * 对 10000000000 这类输入，必须把已去除的数字位当作
	 * 真实。故循环假定小数点前有 weight+1 个数字位。
	 */
	weight = rounded.weight;
	Assert(weight >= 0 && ndigits <= weight + 1);

	/* 构造结果 */
	digits = rounded.digits;
	val = digits[0];
	for (i = 1; i <= weight; i++)
	{
		if (unlikely(pg_mul_u64_overflow(val, NBASE, &val)))
		{
			free_var(&rounded);
			return false;
		}

		if (i < ndigits)
		{
			if (unlikely(pg_add_u64_overflow(val, digits[i], &val)))
			{
				free_var(&rounded);
				return false;
			}
		}
	}

	free_var(&rounded);

	*result = val;

	return true;
}

#ifdef HAVE_INT128
/*
 * numeric 转 int128，必要时四舍五入。
 *
 * 若溢出，返回 false（不抛错）。正常则返回 true。
 */
static bool
numericvar_to_int128(const NumericVar *var, int128 *result)
{
	NumericDigit *digits;
	int			ndigits;
	int			weight;
	int			i;
	int128		val,
				oldval;
	bool		neg;
	NumericVar	rounded;

	/* 舍入到最近整数 */
	init_var(&rounded);
	set_var_from_var(var, &rounded);
	round_var(&rounded, 0);

	/* 检查零输入 */
	strip_var(&rounded);
	ndigits = rounded.ndigits;
	if (ndigits == 0)
	{
		*result = 0;
		free_var(&rounded);
		return true;
	}

	/*
	 * 对 10000000000 这类输入，必须把已去除的数字位当作
	 * 真实。故循环假定小数点前有 weight+1 个数字位。
	 */
	weight = rounded.weight;
	Assert(weight >= 0 && ndigits <= weight + 1);

	/* 构造结果 */
	digits = rounded.digits;
	neg = (rounded.sign == NUMERIC_NEG);
	val = digits[0];
	for (i = 1; i <= weight; i++)
	{
		oldval = val;
		val *= NBASE;
		if (i < ndigits)
			val += digits[i];

		/*
		 * 溢出检查有点微妙，因为要接受 INT128_MIN，
		 * 它使正累加器溢出。但这情况很容易检测，因为
		 * INT128_MIN 是唯一满足 -val == val 的非零值（在二
		 * 进制补码机器上）。
		 *
		 */
		if ((val / NBASE) != oldval)	/* 可能溢出？ */
		{
			if (!neg || (-val) != val || val == 0 || oldval < 0)
			{
				free_var(&rounded);
				return false;
			}
		}
	}

	free_var(&rounded);

	*result = neg ? -val : val;
	return true;
}

/*
 * 把 128 位整数转为 numeric。
 */
static void
int128_to_numericvar(int128 val, NumericVar *var)
{
	uint128		uval,
				newuval;
	NumericDigit *ptr;
	int			ndigits;

	/* int128 至多需要 39 个十进制数字位；为安全加一 */
	alloc_var(var, 40 / DEC_DIGITS);
	if (val < 0)
	{
		var->sign = NUMERIC_NEG;
		uval = -val;
	}
	else
	{
		var->sign = NUMERIC_POS;
		uval = val;
	}
	var->dscale = 0;
	if (val == 0)
	{
		var->ndigits = 0;
		var->weight = 0;
		return;
	}
	ptr = var->digits + var->ndigits;
	ndigits = 0;
	do
	{
		ptr--;
		ndigits++;
		newuval = uval / NBASE;
		*ptr = uval - newuval * NBASE;
		uval = newuval;
	} while (uval);
	var->digits = ptr;
	var->ndigits = ndigits;
	var->weight = ndigits - 1;
}
#endif

/*
 * 把 NumericVar 转换为 float8；若超出范围返回 +/- HUGE_VAL
 */
static double
numericvar_to_double_no_overflow(const NumericVar *var)
{
	char	   *tmp;
	double		val;
	char	   *endptr;

	tmp = get_str_from_var(var);

	/* 与 float8in 不同，忽略 strtod ERANGE */
	val = strtod(tmp, &endptr);
	if (*endptr != '\0')
	{
		/* 不应发生…… */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"double precision", tmp)));
	}

	pfree(tmp);

	return val;
}


/*
 * cmp_var() -
 *
 *	在变量层面比较两个值。假定零值已被截断为
 *	没有数字位。
 */
static int
cmp_var(const NumericVar *var1, const NumericVar *var2)
{
	return cmp_var_common(var1->digits, var1->ndigits,
						  var1->weight, var1->sign,
						  var2->digits, var2->ndigits,
						  var2->weight, var2->sign);
}

/*
 * cmp_var_common() -
 *
 *	cmp_var() 的主例程。NumericVar 和
 *	Numeric 皆可用。
 */
static int
cmp_var_common(const NumericDigit *var1digits, int var1ndigits,
			   int var1weight, int var1sign,
			   const NumericDigit *var2digits, int var2ndigits,
			   int var2weight, int var2sign)
{
	if (var1ndigits == 0)
	{
		if (var2ndigits == 0)
			return 0;
		if (var2sign == NUMERIC_NEG)
			return 1;
		return -1;
	}
	if (var2ndigits == 0)
	{
		if (var1sign == NUMERIC_POS)
			return 1;
		return -1;
	}

	if (var1sign == NUMERIC_POS)
	{
		if (var2sign == NUMERIC_NEG)
			return 1;
		return cmp_abs_common(var1digits, var1ndigits, var1weight,
							  var2digits, var2ndigits, var2weight);
	}

	if (var2sign == NUMERIC_POS)
		return -1;

	return cmp_abs_common(var2digits, var2ndigits, var2weight,
						  var1digits, var1ndigits, var1weight);
}


/*
 * add_var() -
 *
 *	变量层面加法功能的完整版本（处理符号）。
 *	result 也可以安全地指向某个操作数。
 */
static void
add_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	/*
	 * 根据两个变量的符号决定要做什么
	 */
	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_POS)
		{
			/*
			 * 两者均为正 result = +(ABS(var1) + ABS(var2))
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_POS;
		}
		else
		{
			/*
			 * var1 为正，var2 为负。必须比较绝对值
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = +(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_POS;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = -(ABS(var2) - ABS(var1))
					 * ----------
					 */
					sub_abs(var2, var1, result);
					result->sign = NUMERIC_NEG;
					break;
			}
		}
	}
	else
	{
		if (var2->sign == NUMERIC_POS)
		{
			/* ----------
			 * var1 为负，var2 为正
			 * 必须比较绝对值
			 * ----------
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = -(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_NEG;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = +(ABS(var2) - ABS(var1))
					 * ----------
					 */
					sub_abs(var2, var1, result);
					result->sign = NUMERIC_POS;
					break;
			}
		}
		else
		{
			/* ----------
			 * 两者均为负
			 * result = -(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_NEG;
		}
	}
}


/*
 * sub_var() -
 *
 *	变量层面减法功能的完整版本（处理符号）。
 *	result 也可以安全地指向某个操作数。
 */
static void
sub_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	/*
	 * 根据两个变量的符号决定要做什么
	 */
	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_NEG)
		{
			/* ----------
			 * var1 为正，var2 为负
			 * result = +(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_POS;
		}
		else
		{
			/* ----------
			 * 两者均为正
			 * 必须比较绝对值
			 * ----------
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = +(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_POS;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = -(ABS(var2) - ABS(var1))
					 * ----------
					 */
					sub_abs(var2, var1, result);
					result->sign = NUMERIC_NEG;
					break;
			}
		}
	}
	else
	{
		if (var2->sign == NUMERIC_NEG)
		{
			/* ----------
			 * 两者均为负
			 * 必须比较绝对值
			 * ----------
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = -(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_NEG;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = +(ABS(var2) - ABS(var1))
					 * ----------
					 */
					sub_abs(var2, var1, result);
					result->sign = NUMERIC_POS;
					break;
			}
		}
		else
		{
			/* ----------
			 * var1 为负，var2 为正
			 * result = -(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_NEG;
		}
	}
}


/*
 * mul_var() -
 *
 *	变量层面的乘法。var1 * var2 的乘积存入
 *	result。结果被舍入到不超过 rscale 位小数。
 */
static void
mul_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result,
		int rscale)
{
	int			res_ndigits;
	int			res_sign;
	int			res_weight;
	int			maxdigits;
	int		   *dig;
	int			carry;
	int			maxdig;
	int			newdig;
	int			var1ndigits;
	int			var2ndigits;
	NumericDigit *var1digits;
	NumericDigit *var2digits;
	NumericDigit *res_digits;
	int			i,
				i1,
				i2;

	/*
	 * 安排 var1 作为两数中较短的一个。这能改善性能，
	 * 因为内层乘法循环比外层循环简单得多，
	 * 所以让外层循环的迭代次数更少更好。
	 * 这也减少了累加器数组需要被
	 * 规范化的次数。
	 */
	if (var1->ndigits > var2->ndigits)
	{
		const NumericVar *tmp = var1;

		var1 = var2;
		var2 = tmp;
	}

	/* 把这些值拷贝到局部变量以加速内层循环 */
	var1ndigits = var1->ndigits;
	var2ndigits = var2->ndigits;
	var1digits = var1->digits;
	var2digits = var2->digits;

	if (var1ndigits == 0 || var2ndigits == 0)
	{
		/* 任一输入为零；结果也为零 */
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/* 确定结果符号与（最大可能的）权重 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight + var2->weight + 2;

	/*
	 * 确定要计算的结果数字位数。若精确结果
	 * 的小数位会超过 rscale 位，则截断计算，
	 * 并用 MUL_GUARD_DIGITS 个保护位，即忽略只会
	 * 贡献到其右侧的输入数字位。（这样将给出精确
	 * 的舍入到 rscale 的答案，除非被忽略位置产生的
	 * 进位传播超过 MUL_GUARD_DIGITS 个数字位。）
	 *
	 * 注意：精确计算不可能产生多于 var1ndigits +
	 * var2ndigits 个数字位，但会多分配一个输出数字位，
	 * 以防 rscale 舍入在最高精确位产生进位。
	 */
	res_ndigits = var1ndigits + var2ndigits + 1;
	maxdigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS +
		MUL_GUARD_DIGITS;
	res_ndigits = Min(res_ndigits, maxdigits);

	if (res_ndigits < 3)
	{
		/* 所有输入数字位都将被忽略；结果为零 */
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * 我们在有符号 int 数组 "dig[]" 中做算术。由于
	 * INT_MAX 明显大于 NBASE*NBASE，这就留出了余量
	 * 使我们无需立即规范化进位。
	 *
	 * maxdig 跟踪任何 dig[] 元素可能的最大值；当该值
	 * 将要超过 INT_MAX 时，我们才花时间传播进位。
	 * 此外，还需确保在进位传播过程中也不会
	 * 发生溢出。进位值可能高达 INT_MAX/NBASE，
	 * 所以实际上当数字位将要超过
	 * INT_MAX - INT_MAX/NBASE。
	 *
	 * 为避免 maxdig 自身溢出，它实际表示的是最大
	 * 可能值除以 NBASE-1 后的值，也就是说在循环
	 * 顶部即知无元素超过 maxdig * (NBASE-1)。
	 */
	dig = (int *) palloc0(res_ndigits * sizeof(int));
	maxdig = 0;

	/*
	 * var1 的最低有效数字位应被忽略，如果它们不
	 * 直接贡献于我们所计算结果的头 res_ndigits 个
	 * 数字位。
	 *
	 * var1 的数字位 i1 与 var2 的数字位 i2 相乘后加到
	 * 累加器数组的 i1+i2+2 位上，故只需考虑 var1 中
	 * 符合 i1 <= res_ndigits - 3 的数字位。
	 */
	for (i1 = Min(var1ndigits - 1, res_ndigits - 3); i1 >= 0; i1--)
	{
		NumericDigit var1digit = var1digits[i1];

		if (var1digit == 0)
			continue;

		/* 要规范化？ */
		maxdig += var1digit;
		if (maxdig > (INT_MAX - INT_MAX / NBASE) / (NBASE - 1))
		{
			/* 执行 */
			carry = 0;
			for (i = res_ndigits - 1; i >= 0; i--)
			{
				newdig = dig[i] + carry;
				if (newdig >= NBASE)
				{
					carry = newdig / NBASE;
					newdig -= carry * NBASE;
				}
				else
					carry = 0;
				dig[i] = newdig;
			}
			Assert(carry == 0);
			/* 重置 maxdig 反映新最坏情况 */
			maxdig = 1 + var1digit;
		}

		/*
		 * 把 var2 的适当倍数加进累加器。
		 *
		 * 如上，var2 无贡献的数字位可忽略，故只纳入
		 * 满足 i1+i2+2 < res_ndigits 的数字位。
		 *
		 * 这个内层循环是乘法的性能瓶颈，
		 * 所以我们希望它保持足够简单以便能被
		 * 自动向量化。因此，按从左到右处理数字位，
		 * 尽管竖式乘法暗示的是从右到左。
		 * 由于我们在这个循环中不传播进位，顺序
		 * 无关。
		 */
		{
			int			i2limit = Min(var2ndigits, res_ndigits - i1 - 2);
			int		   *dig_i1_2 = &dig[i1 + 2];

			for (i2 = 0; i2 < i2limit; i2++)
				dig_i1_2[i2] += var1digit * var2digits[i2];
		}
	}

	/*
	 * 现在我们做最后一轮进位传播来规范化结果，
	 * 并将其与把结果数字位存入输出合并进行。注意
	 * 这一步仍以带保护位的全精度进行。
	 */
	alloc_var(result, res_ndigits);
	res_digits = result->digits;
	carry = 0;
	for (i = res_ndigits - 1; i >= 0; i--)
	{
		newdig = dig[i] + carry;
		if (newdig >= NBASE)
		{
			carry = newdig / NBASE;
			newdig -= carry * NBASE;
		}
		else
			carry = 0;
		res_digits[i] = newdig;
	}
	Assert(carry == 0);

	pfree(dig);

	/*
	 * 最后，把结果舍入到要求的精度。
	 */
	result->weight = res_weight;
	result->sign = res_sign;

	/* 舍入到目标 rscale（设 result->dscale） */
	round_var(result, rscale);

	/* 去除前导与末尾零 */
	strip_var(result);
}


/*
 * div_var() -
 *
 *	变量层面的除法。var1 / var2 的商存入 result。
 *	商精确地算到 rscale 位小数。
 *	若 round 为真，则在第 rscale 位做舍入；若为假，
 *	则在该位向零截断。
 */
static void
div_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result,
		int rscale, bool round)
{
	int			div_ndigits;
	int			res_ndigits;
	int			res_sign;
	int			res_weight;
	int			carry;
	int			borrow;
	int			divisor1;
	int			divisor2;
	NumericDigit *dividend;
	NumericDigit *divisor;
	NumericDigit *res_digits;
	int			i;
	int			j;

	/* 把这些值拷贝到局部变量以加速内层循环 */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;

	/*
	 * 首先检查除零；不得传入未规范化的
	 * 除数。
	 */
	if (var2ndigits == 0 || var2->digits[0] == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	/*
	 * 若除数只有一或两个数字位，则转交 div_var_int()，
	 * 它使用快速的短除法。
	 *
	 * 类似地，在支持 128 位整数的平台上，三或四个
	 * 数字位的除数则转交 div_var_int64()。
	 */
	if (var2ndigits <= 2)
	{
		int			idivisor;
		int			idivisor_weight;

		idivisor = var2->digits[0];
		idivisor_weight = var2->weight;
		if (var2ndigits == 2)
		{
			idivisor = idivisor * NBASE + var2->digits[1];
			idivisor_weight--;
		}
		if (var2->sign == NUMERIC_NEG)
			idivisor = -idivisor;

		div_var_int(var1, idivisor, idivisor_weight, result, rscale, round);
		return;
	}
#ifdef HAVE_INT128
	if (var2ndigits <= 4)
	{
		int64		idivisor;
		int			idivisor_weight;

		idivisor = var2->digits[0];
		idivisor_weight = var2->weight;
		for (i = 1; i < var2ndigits; i++)
		{
			idivisor = idivisor * NBASE + var2->digits[i];
			idivisor_weight--;
		}
		if (var2->sign == NUMERIC_NEG)
			idivisor = -idivisor;

		div_var_int64(var1, idivisor, idivisor_weight, result, rscale, round);
		return;
	}
#endif

	/*
	 * 否则执行完整的长除法。
	 */

	/* 零结果检查 */
	if (var1ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * 确定结果符号、权重以及要计算的数字位个数。
	 * 此处算得的权重在输出商没有前导零数字位时
	 * 才是正确的；否则由 strip_var() 修正。
	 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight - var2->weight;
	/* 我们需要产生的精确结果数字位个数： */
	res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* ...但至少为 1 */
	res_ndigits = Max(res_ndigits, 1);
	/* 若需舍入，则多算一位以确保结果正确 */
	if (round)
		res_ndigits++;

	/*
	 * 工作被除数通常需要 res_ndigits + var2ndigits 个
	 * 数字位，但至少设为 var1ndigits，以便能装下
	 * var1 的全部。（被除数空间中另有一个额外
	 * 数字位 dividend[0]，但为与 Knuth 的记法一致，
	 * 不计入 div_ndigits。）
	 */
	div_ndigits = res_ndigits + var2ndigits;
	div_ndigits = Max(div_ndigits, var1ndigits);

	/*
	 * 我们需要一个工作区，容纳工作被除数（div_ndigits+1
	 * 个数字位）加上可能规范化后的除数（var2ndigits
	 * 个数字位）。另在 divisor[0] 放一个零也很方便，
	 * 实际除数数据则放在 divisor[1 .. var2ndigits]。把数字
	 * 位搬入工作区还使我们能在开始主循环之前重新
	 * 分配 result（它可能与任一输入 var 是同一个）。
	 * 注意我们用 palloc0 来确保 divisor[0]、dividend[0]
	 * 以及 var1ndigits 之外的额外被除数位置都从 0 开始。
	 */
	dividend = (NumericDigit *)
		palloc0((div_ndigits + var2ndigits + 2) * sizeof(NumericDigit));
	divisor = dividend + (div_ndigits + 1);
	memcpy(dividend + 1, var1->digits, var1ndigits * sizeof(NumericDigit));
	memcpy(divisor + 1, var2->digits, var2ndigits * sizeof(NumericDigit));

	/*
	 * 现在可以重新分配 result 来容纳生成的商数字位。
	 */
	alloc_var(result, res_ndigits);
	res_digits = result->digits;

	/*
	 * 完整的多位算法取自 Knuth 第二卷
	 * 算法 4.3.1D。
	 *
	 * 我们要求首个除数数字位 >= NBASE/2。若不是，
	 * 就把除数与被除数同乘因子 "d" 来放大。（在上面
	 * 分配 dividend[0] 的原因就是为这里的可能进位留出
	 * 空间。）
	 */
	if (divisor[1] < HALF_NBASE)
	{
		int			d = NBASE / (divisor[1] + 1);

		carry = 0;
		for (i = var2ndigits; i > 0; i--)
		{
			carry += divisor[i] * d;
			divisor[i] = carry % NBASE;
			carry = carry / NBASE;
		}
		Assert(carry == 0);
		carry = 0;
		/* 此刻被除数仅 var1ndigits 个数字位可能非零 */
		for (i = var1ndigits; i >= 0; i--)
		{
			carry += dividend[i] * d;
			dividend[i] = carry % NBASE;
			carry = carry / NBASE;
		}
		Assert(carry == 0);
		Assert(divisor[1] >= HALF_NBASE);
	}
	/* 头两个除数数字位在主循环中被反复使用 */
	divisor1 = divisor[1];
	divisor2 = divisor[2];

	/*
	 * 开始主循环。此循环的每次迭代都以 dividend[j ..
	 * j + var2ndigits] 除以除数，产生第 j 个
	 * 商数字位；本质上与手工长除法的常规做法一致。
	 *
	 */
	for (j = 0; j < res_ndigits; j++)
	{
		/* 由头两个被除数数字位估计商数字位 */
		int			next2digits = dividend[j] * NBASE + dividend[j + 1];
		int			qhat;

		/*
		 * 若 next2digits 为 0，则商数字位必为 0，无需调整
		 * 工作被除数。此处做测试是值得的，可在处理
		 * 被除数尾部零时尽快退出。
		 */
		if (next2digits == 0)
		{
			res_digits[j] = 0;
			continue;
		}

		if (dividend[j] == divisor1)
			qhat = NBASE - 1;
		else
			qhat = next2digits / divisor1;

		/*
		 * 若商数字位过大则调整。Knuth 证明经此步后，
		 * 商数字位要么正确，要么只是大了 1。
		 * （注意：这里可以使用 dividend[j+2]，因为我们
		 * 知道除数长度至少为 2。）
		 */
		while (divisor2 * qhat >
			   (next2digits - qhat * divisor1) * NBASE + dividend[j + 2])
			qhat--;

		/* 如上，商数字位为 0 时无需再做任何事 */
		if (qhat > 0)
		{
			NumericDigit *dividend_j = &dividend[j];

			/*
			 * 将除数乘 qhat，并从工作被除数减去它。
			 * 乘法与减法在此合而为一。注意 qhat <= NBASE
			 * （它可能大了 1），故中间结果 "tmp_result" 的
			 * 范围是 [-NBASE^2, NBASE - 1]，而 "borrow" 的范围
			 *
			 * 是 [0, NBASE]。
			 */
			borrow = 0;
			for (i = var2ndigits; i >= 0; i--)
			{
				int			tmp_result;

				tmp_result = dividend_j[i] - borrow - divisor[i] * qhat;
				borrow = (NBASE - 1 - tmp_result) / NBASE;
				dividend_j[i] = tmp_result + borrow * NBASE;
			}

			/*
			 * 若最高被除数数字位发生借位，则 qhat 确实
			 * 是大了 1。修正它，并把除数加回以校正
			 * 工作被除数。（Knuth 证明这只约 3/NBASE 的
			 * 概率发生；因此，最好用小 NBASE 测试这段代码，
			 *
			 * 确保它被演练到。）
			 */
			if (borrow)
			{
				qhat--;
				carry = 0;
				for (i = var2ndigits; i >= 0; i--)
				{
					carry += dividend_j[i] + divisor[i];
					if (carry >= NBASE)
					{
						dividend_j[i] = carry - NBASE;
						carry = 1;
					}
					else
					{
						dividend_j[i] = carry;
						carry = 0;
					}
				}
				/* 这里应产生一个进位来抵消上面的借位 */
				Assert(carry == 1);
			}
		}

		/* 这个商数字位就处理完了 */
		res_digits[j] = qhat;
	}

	pfree(dividend);

	/*
	 * 最后，把结果舍入或截断到要求的精度。
	 */
	result->weight = res_weight;
	result->sign = res_sign;

	/* 舍入或截断到目标 rscale（设 result->dscale） */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* 去除前导与末尾零 */
	strip_var(result);
}


/*
 * div_var_fast() -
 *
 *	它与 div_var 接口相同，但实现用的是 "FM" 库的除法
 *	算法，而非 Knuth 竖式除法方案。这显著更快，但可能
 *	产生不准确的结果，因为它有时须向左传播舍入，
 *	所以我们永远无法完全确知所求的数字位是
 *	精确的。我们会多算 DIV_GUARD_DIGITS 个数字位，
 *	但并不能肯定这就够了。我们只在超越函数计算
 *	例程中使用它，那里的一切本来就是近似的。
 *
 *
 *	尽管为了与 div_var 一致而提供了 "round" 参数，
 *	但在 round=false 时使用本函数并不明智。在截断模式
 *	下可能得到没有有效数字的结果，例如 rscale=0 时
 *	可能算出 0.99999... 并把它截断成 0，而正确答案是 1。
 *
 */
static void
div_var_fast(const NumericVar *var1, const NumericVar *var2,
			 NumericVar *result, int rscale, bool round)
{
	int			div_ndigits;
	int			load_ndigits;
	int			res_sign;
	int			res_weight;
	int		   *div;
	int			qdigit;
	int			carry;
	int			maxdiv;
	int			newdig;
	NumericDigit *res_digits;
	double		fdividend,
				fdivisor,
				fdivisorinverse,
				fquotient;
	int			qi;
	int			i;

	/* 把这些值拷贝到局部变量以加速内层循环 */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	/*
	 * 首先检查除零；不得传入未规范化的
	 * 除数。
	 */
	if (var2ndigits == 0 || var2digits[0] == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	/*
	 * 若除数只有一或两个数字位，则转交 div_var_int()，
	 * 它使用快速的短除法。
	 *
	 * 类似地，在支持 128 位整数的平台上，三或四个
	 * 数字位的除数则转交 div_var_int64()。
	 */
	if (var2ndigits <= 2)
	{
		int			idivisor;
		int			idivisor_weight;

		idivisor = var2->digits[0];
		idivisor_weight = var2->weight;
		if (var2ndigits == 2)
		{
			idivisor = idivisor * NBASE + var2->digits[1];
			idivisor_weight--;
		}
		if (var2->sign == NUMERIC_NEG)
			idivisor = -idivisor;

		div_var_int(var1, idivisor, idivisor_weight, result, rscale, round);
		return;
	}
#ifdef HAVE_INT128
	if (var2ndigits <= 4)
	{
		int64		idivisor;
		int			idivisor_weight;

		idivisor = var2->digits[0];
		idivisor_weight = var2->weight;
		for (i = 1; i < var2ndigits; i++)
		{
			idivisor = idivisor * NBASE + var2->digits[i];
			idivisor_weight--;
		}
		if (var2->sign == NUMERIC_NEG)
			idivisor = -idivisor;

		div_var_int64(var1, idivisor, idivisor_weight, result, rscale, round);
		return;
	}
#endif

	/*
	 * 否则执行完整的长除法。
	 */

	/* 零结果检查 */
	if (var1ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * 确定结果符号、权重以及要计算的数字位个数
	 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight - var2->weight + 1;
	/* 我们需要产生的精确结果数字位个数： */
	div_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* 为舍入误差增加保护位 */
	div_ndigits += DIV_GUARD_DIGITS;
	if (div_ndigits < DIV_GUARD_DIGITS)
		div_ndigits = DIV_GUARD_DIGITS;

	/*
	 * 我们在有符号 int 数组 "div[]" 中做算术。由于
	 * INT_MAX 明显大于 NBASE*NBASE，这就留出了余量
	 * 使我们无需立即规范化进位。
	 *
	 * 初始 div[] 含一个零数字位，其后是被除数
	 * 的数字位（外加补零以凑足含保护位的精度）。
	 * 主循环每步产生一个（近似）商数字位并写入
	 * div[]，从而腾出一个被除数位置。最后一轮进位
	 *
	 * 传播会处理好任何错误的商数字位。
	 *
	 * 注意 div[] 未必包含被除数的全部数字位 --- 目标精度
	 * 加保护位可能小于被除数自身的精度。例如在平方根
	 * 算法中就是这种情况：通常以 2N 位数字的数除以
	 *
	 * N 个数字位的数，只要求 N 个数字位的精度。
	 */
	div = (int *) palloc0((div_ndigits + 1) * sizeof(int));
	load_ndigits = Min(div_ndigits, var1ndigits);
	for (i = 0; i < load_ndigits; i++)
		div[i + 1] = var1digits[i];

	/*
	 * 我们用浮点运算估计各商数字位，取（当前）被除数
	 * 与除数的头四个数字位。这必须是 float 以免溢出。
	 * 商数字位一般与精确答案相差不过 1。
	 *
	 */
	fdivisor = (double) var2digits[0];
	for (i = 1; i < 4; i++)
	{
		fdivisor *= NBASE;
		if (i < var2ndigits)
			fdivisor += (double) var2digits[i];
	}
	fdivisorinverse = 1.0 / fdivisor;

	/*
	 * maxdiv 跟踪 div[] 各元素可能的最大绝对值；当该值
	 * 将要超过 INT_MAX 时，我们才花时间传播进位。
	 * 此外，还需确保在进位传播过程中也不会
	 * 发生溢出。进位值的绝对值可能高达 INT_MAX/NBASE + 1，
	 * 所以实际上当数字位将要超过
	 * INT_MAX - INT_MAX/NBASE - 1。
	 *
	 * 为避免 maxdiv 自身溢出，它表示最大绝对值除以
	 * NBASE-1 后的值，即循环顶部时可确信任何 div[] 元素
	 * 的绝对值都不会超过 maxdiv * (NBASE-1)。
	 *
	 * 不过实际上该结论只对 div[qi] 之后的 div[] 元素成立；
	 * 循环底部做的调整可能导致 div[qi + 1] 超出 maxdiv 限
	 * 制，于是下一轮迭代中 div[qi] 会超出限制。这并
	 * 不会造成问题，原因见下文解释。
	 */
	maxdiv = 1;

	/*
	 * 外层循环计算下一个商数字位，它将放入 div[qi]
	 */
	for (qi = 0; qi < div_ndigits; qi++)
	{
		/* 近似计算当前被除数的值 */
		fdividend = (double) div[qi];
		for (i = 1; i < 4; i++)
		{
			fdividend *= NBASE;
			if (qi + i <= div_ndigits)
				fdividend += (double) div[qi + i];
		}
		/* 计算（近似）商数字位 */
		fquotient = fdividend * fdivisorinverse;
		qdigit = (fquotient >= 0.0) ? ((int) fquotient) :
			(((int) fquotient) - 1);	/* 向负无穷方向截断 */

		if (qdigit != 0)
		{
			/* 现在需要规范化吗？ */
			maxdiv += abs(qdigit);
			if (maxdiv > (INT_MAX - INT_MAX / NBASE - 1) / (NBASE - 1))
			{
				/*
				 * 是，执行。若 var2ndigits 远小于 div_ndigits，
				 * 则能省很多工夫：只需规范化前几轮中
				 * 减过除数倍数的位置触及的 div[] 元素。
				 *
				 *
				 */
				carry = 0;
				for (i = Min(qi + var2ndigits - 2, div_ndigits); i > qi; i--)
				{
					newdig = div[i] + carry;
					if (newdig < 0)
					{
						carry = -((-newdig - 1) / NBASE) - 1;
						newdig -= carry * NBASE;
					}
					else if (newdig >= NBASE)
					{
						carry = newdig / NBASE;
						newdig -= carry * NBASE;
					}
					else
						carry = 0;
					div[i] = newdig;
				}
				newdig = div[qi] + carry;
				div[qi] = newdig;

				/*
				 * 现在除 div[qi] 外，所有 div[] 数字位都已在
				 * 0..NBASE-1 内。maxdiv 不再计入 div[qi]，
				 * 因此可以把 maxdiv 重置为 1。
				 */
				maxdiv = 1;

				/*
				 * 重算商数字位，因新信息可能已传到
				 * 被除数的头四位数字中。
				 */
				fdividend = (double) div[qi];
				for (i = 1; i < 4; i++)
				{
					fdividend *= NBASE;
					if (qi + i <= div_ndigits)
						fdividend += (double) div[qi + i];
				}
				/* 计算（近似）商数字位 */
				fquotient = fdividend * fdivisorinverse;
				qdigit = (fquotient >= 0.0) ? ((int) fquotient) :
					(((int) fquotient) - 1);	/* 向负无穷方向截断 */
				maxdiv += abs(qdigit);
			}

			/*
			 * 减去除数的适当倍数。
			 *
			 * div[qi] 之后的数字位不会溢出，因为它们必然落
			 * 在 maxdiv 限制之内。至于 div[qi] 本身，注意 qdigit
			 * 约为 trunc(div[qi] / vardigits[0])，这会使新值恰
			 * 为 div[qi] mod vardigits[0]。qdigit 中的低阶项对
			 * 这一结果的改变量至多约两倍 INT_MAX/NBASE，故
			 * 不可能发生。
			 *
			 * 此内层循环是除法的性能瓶颈，故把它写成与
			 * mul_var() 内层循环相同的方式，使其能被自动向
			 * 量化。乘法前将 qdigit 转型为 NumericDigit，
			 * 让编译器生成更高效代码（16 位乘法），这
			 * 很安全，因为商数字位至多偏差 1，所以
			 *
			 * 并无溢出风险。
			 */
			if (qdigit != 0)
			{
				int			istop = Min(var2ndigits, div_ndigits - qi + 1);
				int		   *div_qi = &div[qi];

				for (i = 0; i < istop; i++)
					div_qi[i] -= ((NumericDigit) qdigit) * var2digits[i];
			}
		}

		/*
		 * 我们即将替换掉的被除数数字位可能仍非零。
		 * 把它折入下一个数字位上。
		 *
		 * 此处并无溢出风险，尽管要证明这点还须细心。
		 * 这与 div[qi] 不溢出的论证大体相同：若考察分子
		 * 与分母的头两项，可见 div[qi + 1] 的最终值将近似
		 * 等于一个模 (vardigits[0]*NBASE + vardigits[1]) 的余数。
		 * 计入低阶项略复杂，但最终给可能范围增加的
		 * 量不过比 INT_MAX/NBASE 多多少。
		 * 因此 div[qi + 1] 在此不可能溢出，且它在下一轮循
		 * 迭代中作为 div[qi] 时，也不至于大到会在进位
		 *
		 * 传播步骤（如果有的话）中造成溢出。
		 *
		 * 不过话虽如此：div[qi] 可能大于 INT_MAX/NBASE，如
		 * 上文所述，这意味着乘积 div[qi] * NBASE *确实*
		 * 溢出。一旦发生，把它加到 div[qi + 1] 上总会引起
		 * 一次抵消性溢出，使最终结果仍正确。我们本可
		 * 以 int64 算术完成乘法与加法来避免中间溢出，但
		 * 到目前为止似乎并无必要。
		 */
		div[qi + 1] += div[qi] * NBASE;

		div[qi] = qdigit;
	}

	/*
	 * 近似算出并存储末个商数字位（div[div_ndigits]）
	 */
	fdividend = (double) div[qi];
	for (i = 1; i < 4; i++)
		fdividend *= NBASE;
	fquotient = fdividend * fdivisorinverse;
	qdigit = (fquotient >= 0.0) ? ((int) fquotient) :
		(((int) fquotient) - 1);	/* 向负无穷方向截断 */
	div[qi] = qdigit;

	/*
	 * 由于商数字位可能偏差 1，此时其中某些可能是 -1 或
	 * NBASE。数值在数学上正确，但看上去不对。我们
	 * 要做最后一轮进位传播来规范化这些数字位，并将
	 * 其与把结果数字位存入输出合并进行。注意这一步
	 * 仍是以带保护位的全精度完成的。
	 *
	 */
	alloc_var(result, div_ndigits + 1);
	res_digits = result->digits;
	carry = 0;
	for (i = div_ndigits; i >= 0; i--)
	{
		newdig = div[i] + carry;
		if (newdig < 0)
		{
			carry = -((-newdig - 1) / NBASE) - 1;
			newdig -= carry * NBASE;
		}
		else if (newdig >= NBASE)
		{
			carry = newdig / NBASE;
			newdig -= carry * NBASE;
		}
		else
			carry = 0;
		res_digits[i] = newdig;
	}
	Assert(carry == 0);

	pfree(div);

	/*
	 * 最后，把结果舍入到要求的精度。
	 */
	result->weight = res_weight;
	result->sign = res_sign;

	/* 舍入到目标 rscale（设 result->dscale） */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* 去除前导与末尾零 */
	strip_var(result);
}


/*
 * div_var_int() -
 *
 *	把一个 numeric 变量除以给定了权重的 32 位整数。
 *	商 var / (ival * NBASE^ival_weight) 存入 result。
 */
static void
div_var_int(const NumericVar *var, int ival, int ival_weight,
			NumericVar *result, int rscale, bool round)
{
	NumericDigit *var_digits = var->digits;
	int			var_ndigits = var->ndigits;
	int			res_sign;
	int			res_weight;
	int			res_ndigits;
	NumericDigit *res_buf;
	NumericDigit *res_digits;
	uint32		divisor;
	int			i;

	/* 防止除以零 */
	if (ival == 0)
		ereport(ERROR,
				errcode(ERRCODE_DIVISION_BY_ZERO),
				errmsg("division by zero"));

	/* 零结果检查 */
	if (var_ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * 确定结果符号、权重以及要计算的数字位个数。
	 * 此处算得的权重在输出商没有前导零数字位时
	 * 才是正确的；否则由 strip_var() 修正。
	 */
	if (var->sign == NUMERIC_POS)
		res_sign = ival > 0 ? NUMERIC_POS : NUMERIC_NEG;
	else
		res_sign = ival > 0 ? NUMERIC_NEG : NUMERIC_POS;
	res_weight = var->weight - ival_weight;
	/* 我们需要产生的精确结果数字位个数： */
	res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* ...但至少为 1 */
	res_ndigits = Max(res_ndigits, 1);
	/* 若需舍入，则多算一位以确保结果正确 */
	if (round)
		res_ndigits++;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* 留待舍入的数字位 */
	res_digits = res_buf + 1;

	/*
	 * 现在计算商数字位。这是 Knuth 第二卷 4.3.1 节习题 16
	 * 所述的短除法算法，
	 * 只是我们允许除数超过内部基数。
	 *
	 * 该算法中，从一个数字位传到下一位的进位至多
	 * divisor - 1。因此在处理下一个数字位时，carry 可能
	 * 变得大至 divisor * NBASE - 1，所以若超过 UINT_MAX 就
	 * 需要 64 位整数。
	 */
	divisor = abs(ival);

	if (divisor <= UINT_MAX / NBASE)
	{
		/* carry 不会溢出 32 位 */
		uint32		carry = 0;

		for (i = 0; i < res_ndigits; i++)
		{
			carry = carry * NBASE + (i < var_ndigits ? var_digits[i] : 0);
			res_digits[i] = (NumericDigit) (carry / divisor);
			carry = carry % divisor;
		}
	}
	else
	{
		/* carry 可超 32 位 */
		uint64		carry = 0;

		for (i = 0; i < res_ndigits; i++)
		{
			carry = carry * NBASE + (i < var_ndigits ? var_digits[i] : 0);
			res_digits[i] = (NumericDigit) (carry / divisor);
			carry = carry % divisor;
		}
	}

	/* 把商存入 result */
	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->sign = res_sign;

	/* 舍入或截断到目标 rscale（设 result->dscale） */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* 去除前导/末尾零 */
	strip_var(result);
}


#ifdef HAVE_INT128
/*
 * div_var_int64() -
 *
 *	把一个 numeric 变量除以给定了权重的 64 位整数。
 *	商 var / (ival * NBASE^ival_weight) 存入 result。
 *
 *	这复制了 div_var_int() 中的逻辑，因此对该处所做的
 *	修改亦须同步。
 */
static void
div_var_int64(const NumericVar *var, int64 ival, int ival_weight,
			  NumericVar *result, int rscale, bool round)
{
	NumericDigit *var_digits = var->digits;
	int			var_ndigits = var->ndigits;
	int			res_sign;
	int			res_weight;
	int			res_ndigits;
	NumericDigit *res_buf;
	NumericDigit *res_digits;
	uint64		divisor;
	int			i;

	/* 防止除以零 */
	if (ival == 0)
		ereport(ERROR,
				errcode(ERRCODE_DIVISION_BY_ZERO),
				errmsg("division by zero"));

	/* 零结果检查 */
	if (var_ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * 确定结果符号、权重以及要计算的数字位个数。
	 * 此处算得的权重在输出商没有前导零数字位时
	 * 才是正确的；否则由 strip_var() 修正。
	 */
	if (var->sign == NUMERIC_POS)
		res_sign = ival > 0 ? NUMERIC_POS : NUMERIC_NEG;
	else
		res_sign = ival > 0 ? NUMERIC_NEG : NUMERIC_POS;
	res_weight = var->weight - ival_weight;
	/* 我们需要产生的精确结果数字位个数： */
	res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* ...但至少为 1 */
	res_ndigits = Max(res_ndigits, 1);
	/* 若需舍入，则多算一位以确保结果正确 */
	if (round)
		res_ndigits++;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* 留待舍入的数字位 */
	res_digits = res_buf + 1;

	/*
	 * 现在计算商数字位。这是 Knuth 第二卷 4.3.1 节习题 16
	 * 所述的短除法算法，
	 * 只是我们允许除数超过内部基数。
	 *
	 * 该算法中，从一个数字位传到下一位的进位至多
	 * divisor - 1。因此在处理下一个数字位时，carry 可能
	 * 变得大至 divisor * NBASE - 1，所以若超过 PG_UINT64_MAX
	 * 就需要 128 位整数。
	 */
	divisor = i64abs(ival);

	if (divisor <= PG_UINT64_MAX / NBASE)
	{
		/* carry 不会溢出 64 位 */
		uint64		carry = 0;

		for (i = 0; i < res_ndigits; i++)
		{
			carry = carry * NBASE + (i < var_ndigits ? var_digits[i] : 0);
			res_digits[i] = (NumericDigit) (carry / divisor);
			carry = carry % divisor;
		}
	}
	else
	{
		/* carry 可超 64 位 */
		uint128		carry = 0;

		for (i = 0; i < res_ndigits; i++)
		{
			carry = carry * NBASE + (i < var_ndigits ? var_digits[i] : 0);
			res_digits[i] = (NumericDigit) (carry / divisor);
			carry = carry % divisor;
		}
	}

	/* 把商存入 result */
	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->sign = res_sign;

	/* 舍入或截断到目标 rscale（设 result->dscale） */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* 去除前导/末尾零 */
	strip_var(result);
}
#endif


/*
 * 除法的默认标度选择
 *
 * 返回适合除法结果的结果标度。
 */
static int
select_div_scale(const NumericVar *var1, const NumericVar *var2)
{
	int			weight1,
				weight2,
				qweight,
				i;
	NumericDigit firstdigit1,
				firstdigit2;
	int			rscale;

	/*
	 * 除法的结果标度在任何 SQL 标准中都没有规定。对于
	 * PostgreSQL，我们选择的结果标度至少给出
	 * NUMERIC_MIN_SIG_DIGITS 个有效数字，使 numeric 给出的
	 * 结果不逊于 float8 的精度；但标度不得低于任一
	 * 输入的显示标度。
	 */

	/* 取每个输入实际的（规范化）权重与首个数字位 */

	weight1 = 0;				/* var1 为零时使用的值 */
	firstdigit1 = 0;
	for (i = 0; i < var1->ndigits; i++)
	{
		firstdigit1 = var1->digits[i];
		if (firstdigit1 != 0)
		{
			weight1 = var1->weight - i;
			break;
		}
	}

	weight2 = 0;				/* var2 为零时使用的值 */
	firstdigit2 = 0;
	for (i = 0; i < var2->ndigits; i++)
	{
		firstdigit2 = var2->digits[i];
		if (firstdigit2 != 0)
		{
			weight2 = var2->weight - i;
			break;
		}
	}

	/*
	 * 估计商的权重。若两数首位数字位相等，我们无法
	 * 确定，但可假定 var1 小于 var2。
	 */
	qweight = weight1 - weight2;
	if (firstdigit1 <= firstdigit2)
		qweight--;

	/* 选择结果标度 */
	rscale = NUMERIC_MIN_SIG_DIGITS - qweight * DEC_DIGITS;
	rscale = Max(rscale, var1->dscale);
	rscale = Max(rscale, var2->dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	return rscale;
}


/*
 * mod_var() -
 *
 *	在变量层面计算两个 numeric 的模
 */
static void
mod_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	NumericVar	tmp;

	init_var(&tmp);

	/* ---------
	 * 我们利用如下等式
	 *		mod(x,y) = x - trunc(x/y)*y
	 * 可以让 div_var 直接给出 trunc(x/y)。
	 * ----------
	 */
	div_var(var1, var2, &tmp, 0, false);

	mul_var(var2, &tmp, &tmp, var2->dscale);

	sub_var(var1, &tmp, result);

	free_var(&tmp);
}


/*
 * div_mod_var() -
 *
 *	计算两个 numeric 变量的截断整数商与 numeric 余数。
 *	余数精确到 var2 的 dscale。
 */
static void
div_mod_var(const NumericVar *var1, const NumericVar *var2,
			NumericVar *quot, NumericVar *rem)
{
	NumericVar	q;
	NumericVar	r;

	init_var(&q);
	init_var(&r);

	/*
	 * 用 div_var_fast() 得到整数商的初始估计值。
	 * 这可能有偏差（见 div_var_fast 注释中的警告），
	 * 但下文可以修正它。
	 */
	div_var_fast(var1, var2, &q, 0, false);

	/* 用商的估计值计算余数的初始估计值。 */
	mul_var(var2, &q, &r, var2->dscale);
	sub_var(var1, &r, &r);

	/*
	 * 如有必要则调整结果 --- 余数应与 var1 同号，且其绝
	 * 对值应小于 var2 的绝对值。
	 *
	 */
	while (r.ndigits != 0 && r.sign != var1->sign)
	{
		/* 商的绝对值过大 */
		if (var1->sign == var2->sign)
		{
			sub_var(&q, &const_one, &q);
			add_var(&r, var2, &r);
		}
		else
		{
			add_var(&q, &const_one, &q);
			sub_var(&r, var2, &r);
		}
	}

	while (cmp_abs(&r, var2) >= 0)
	{
		/* 商的绝对值过小 */
		if (var1->sign == var2->sign)
		{
			add_var(&q, &const_one, &q);
			sub_var(&r, var2, &r);
		}
		else
		{
			sub_var(&q, &const_one, &q);
			add_var(&r, var2, &r);
		}
	}

	set_var_from_var(&q, quot);
	set_var_from_var(&r, rem);

	free_var(&q);
	free_var(&r);
}


/*
 * ceil_var() -
 *
 *	返回大于等于参数的最小整数
 *	（变量层面）
 */
static void
ceil_var(const NumericVar *var, NumericVar *result)
{
	NumericVar	tmp;

	init_var(&tmp);
	set_var_from_var(var, &tmp);

	trunc_var(&tmp, 0);

	if (var->sign == NUMERIC_POS && cmp_var(var, &tmp) != 0)
		add_var(&tmp, &const_one, &tmp);

	set_var_from_var(&tmp, result);
	free_var(&tmp);
}


/*
 * floor_var() -
 *
 *	返回小于等于参数的最大整数
 *	（变量层面）
 */
static void
floor_var(const NumericVar *var, NumericVar *result)
{
	NumericVar	tmp;

	init_var(&tmp);
	set_var_from_var(var, &tmp);

	trunc_var(&tmp, 0);

	if (var->sign == NUMERIC_NEG && cmp_var(var, &tmp) != 0)
		sub_var(&tmp, &const_one, &tmp);

	set_var_from_var(&tmp, result);
	free_var(&tmp);
}


/*
 * gcd_var() -
 *
 *	在变量层面计算两个 numeric 的最大公约数
 */
static void
gcd_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	int			res_dscale;
	int			cmp;
	NumericVar	tmp_arg;
	NumericVar	mod;

	res_dscale = Max(var1->dscale, var2->dscale);

	/*
	 * 安排 var1 为绝对值较大的那个数。
	 *
	 * 这本来会在下方循环中自动发生，但能避免一次
	 * 代价高昂的取模。
	 */
	cmp = cmp_abs(var1, var2);
	if (cmp < 0)
	{
		const NumericVar *tmp = var1;

		var1 = var2;
		var2 = tmp;
	}

	/*
	 * 另外，如果两输入绝对值相同，或较小输入为零，
	 * 也要避免做取模。
	 */
	if (cmp == 0 || var2->ndigits == 0)
	{
		set_var_from_var(var1, result);
		result->sign = NUMERIC_POS;
		result->dscale = res_dscale;
		return;
	}

	init_var(&tmp_arg);
	init_var(&mod);

	/* 用欧几里得算法求最大公约数 */
	set_var_from_var(var1, &tmp_arg);
	set_var_from_var(var2, result);

	for (;;)
	{
		/* 这个循环可能耗时较久，因此允许它被中断 */
		CHECK_FOR_INTERRUPTS();

		mod_var(&tmp_arg, result, &mod);
		if (mod.ndigits == 0)
			break;
		set_var_from_var(result, &tmp_arg);
		set_var_from_var(&mod, result);
	}
	result->sign = NUMERIC_POS;
	result->dscale = res_dscale;

	free_var(&tmp_arg);
	free_var(&mod);
}


/*
 * sqrt_var() -
 *
 *	使用 Karatsuba 平方根算法计算 x 的平方根。
 *	注意：这里允许 rscale < 0，即在小数点前舍入。
 *
 */
static void
sqrt_var(const NumericVar *arg, NumericVar *result, int rscale)
{
	int			stat;
	int			res_weight;
	int			res_ndigits;
	int			src_ndigits;
	int			step;
	int			ndigits[32];
	int			blen;
	int64		arg_int64;
	int			src_idx;
	int64		s_int64;
	int64		r_int64;
	NumericVar	s_var;
	NumericVar	r_var;
	NumericVar	a0_var;
	NumericVar	a1_var;
	NumericVar	q_var;
	NumericVar	u_var;

	stat = cmp_var(arg, &const_zero);
	if (stat == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * SQL2003 用幂来定义 sqrt()，因此若操作数为负，我们
	 * 需要发出正确的 SQLSTATE 错误码。
	 */
	if (stat < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
				 errmsg("cannot take square root of a negative number")));

	init_var(&s_var);
	init_var(&r_var);
	init_var(&a0_var);
	init_var(&a1_var);
	init_var(&q_var);
	init_var(&u_var);

	/*
	 * 结果权重为输入权重之半，向负无穷舍入 ---
	 * res_weight = floor(arg->weight / 2)。
	 */
	if (arg->weight >= 0)
		res_weight = arg->weight / 2;
	else
		res_weight = -((-arg->weight - 1) / 2 + 1);

	/*
	 * 要计算的 NBASE 数字位个数。为确保舍入正确，至少
	 * 多算 1 位十进制数字。这里明确允许 rscale 为负，
	 * 但必须始终至少计算 1 个 NBASE 数字位。因此
	 * res_ndigits = res_weight + 1 + ceil((rscale + 1) / DEC_DIGITS) 或 1。
	 */
	if (rscale + 1 >= 0)
		res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS) / DEC_DIGITS;
	else
		res_ndigits = res_weight + 1 - (-rscale - 1) / DEC_DIGITS;
	res_ndigits = Max(res_ndigits, 1);

	/*
	 * 逻辑上要产生具有此精度的结果所需的源 NBASE 数字
	 * 位个数 --- 小数点前的每个数字位，加上小数点后
	 * 每个结果数字位加 2（或对每个在小数点前即舍入
	 * 的结果数字位减 2）。
	 */
	src_ndigits = arg->weight + 1 + (res_ndigits - res_weight - 1) * 2;
	src_ndigits = Max(src_ndigits, 1);

	/* ----------
	 * 从这里开始，我们把输入和结果均当作整数，并以
	 * Karatsuba 平方根算法计算整数平方根与余数。该
	 * 算法可递归地写成如下形式：
	 *
	 *	SqrtRem(n = a3*b^3 + a2*b^2 + a1*b + a0):
	 *		[ for some base b, and coefficients a0,a1,a2,a3 chosen so that
	 *		  0 <= a0,a1,a2 < b and a3 >= b/4 ]
	 *		Let (s,r) = SqrtRem(a3*b + a2)
	 *		Let (q,u) = DivRem(r*b + a1, 2*s)
	 *		Let s = s*b + q
	 *		Let r = u*b + a0 - q^2
	 *		If r < 0 Then
	 *			Let r = r + s
	 *			Let s = s - 1
	 *			Let r = r + s
	 *		Return (s,r)
	 *
	 * 参见 "Karatsuba Square Root"，Paul Zimmermann，INRIA
	 * 研究报告 RR-3805，1999 年 11 月。撰写时可在网络
	 * 上取得：<https://hal.inria.fr/inria-00072854>。
	 *
	 * 对假设 "n = a3*b^3 + a2*b^2 + a1*b + a0" 的读法是：
	 * "选定一个基 b，使 n 至少要用四个以 b 为基的数字
	 * 位来表达；随后那些数字位就是 a3,a2,a1,a0，其中 a3
	 * 可能大于 b"。最优性能下，b 应约为输入数字位个
	 * 数的四分之一，令外层平方根约计算两倍于内层的
	 * 数字位个数。为简单起见，我们选 b = NBASE^blen，即
	 * NBASE 的整数次幂。
	 *
	 * 我们以迭代而非递归实现该算法，以便各工作
	 * 变量得以复用。采用这一做法，输入的每个数字位
	 * 都恰好被读取一次 --- src_idx 跟踪到目前为止已用
	 * 的输入数字位个数。
	 *
	 * 数组 ndigits[] 存放输入在每次迭代结束时将被用到的
	 * NBASE 数字位个数，该个数每次约翻倍。注意数组元素
	 * 按逆序存放，因此若最
	 * 后一次迭代需要 src_ndigits = 37 个输入数字位，那么
	 * 数组将包含 [37,19,11,7,5,3]，且我们会从 3 个最高的
	 * NBASE 数字位开始计算平方根。
	 *
	 * 每次迭代中，我们把 blen 选成按上述形式书写时能使
	 * a3 >= b/4 成立的最大整数。一般而言，这意味着
	 * blen = src_ndigits / 4（截断）；但如果 src_ndigits
	 * 是 4 的倍数，可能使系数 a3 小于 b/4（若首个输入数字
	 * 位小于 NBASE/4），此时我们选 blen = src_ndigits / 4 - 1。
	 * 内层平方根的数字位个数随后为 src_ndigits - 2*blen。
	 * 例如，若最初 src_ndigits = 26，则数组 ndigits[]
	 * 将视首个输入数字位的大小而为 [26,14,8,4] 或
	 * [26,14,8,6,4]。
	 *
	 *
	 * 此外，我们可为所需步数给出上界，方法如下 ---
	 * 假定源数字位个数是一个处于 [2^(n-1), 2^n-1] 范围的
	 * n 位数值，则 blen 将处于 [2^(n-3)-1, 2^(n-2)-1] 范围，
	 * 而内层平方根的数字位个数将处于 [2^(n-2),
	 * 2^(n-1)+1] 范围。下一步中 blen 将处于 [2^(n-4)-1,
	 * 2^(n-3)] 范围，而下一个内层平方根的数字位个数
	 * 将处于 [2^(n-3), 2^(n-2)+1] 范围。此模式不断重复，
	 * 最坏情况下数组 ndigits[] 将包含 [2^n-1,
	 * 2^(n-1)+1, 2^(n-2)+1, ... 9, 5, 3]，计算需要 n 步。
	 * 故由于所有数字位数组的大小均为有符号 32 位整
	 * 数，因此可以保证所需步数必少于
	 * 32。
	 * ----------
	 */
	step = 0;
	while ((ndigits[step] = src_ndigits) > 4)
	{
		/* 按上文所述选择 b 使 a3 >= b/4 */
		blen = src_ndigits / 4;
		if (blen * 4 == src_ndigits && arg->digits[0] < NBASE / 4)
			blen--;

		/* 下一步（内层平方根）的数字位个数 */
		src_ndigits -= 2 * blen;
		step++;
	}

	/*
	 * 第一次迭代（最内层平方根与余数）：
	 *
	 * 这里 src_ndigits <= 4，输入能装进 int64。其平方根至多
	 * 有 9 位十进制数字，所以用双精度算术来估计它，
	 * 实际上这几乎必然返回正确结果，无须再修正。
	 *
	 */
	arg_int64 = arg->digits[0];
	for (src_idx = 1; src_idx < src_ndigits; src_idx++)
	{
		arg_int64 *= NBASE;
		if (src_idx < arg->ndigits)
			arg_int64 += arg->digits[src_idx];
	}

	s_int64 = (int64) sqrt((double) arg_int64);
	r_int64 = arg_int64 - s_int64 * s_int64;

	/*
	 * 如有必要，用牛顿法修正结果。
	 *
	 * 这里用带截断的整数除法，以公式 x -> (x + n/x) / 2
	 * 进行迭代来计算截断的整数平方根。已知该迭代会收
	 * 敛到 isqrt(n)，除非 n+1 是完全平方数。若 n+1 是完全
	 * 平方数，序列就会在 isqrt(n) 与 isqrt(n)+1 两个值之间
	 * 振荡，因此通过检查余数我们就能确保收敛。
	 *
	 */
	while (r_int64 < 0 || r_int64 > 2 * s_int64)
	{
		s_int64 = (s_int64 + arg_int64 / s_int64) / 2;
		r_int64 = arg_int64 - s_int64 * s_int64;
	}

	/*
	 * src_ndigits <= 8 的迭代：
	 *
	 * 接下来的 1 或 2 次迭代计算更大的（外层）平方根，
	 * 其 src_ndigits <= 8，所以结果仍能装进 int64（即便输入
	 * 已装不下），可继续用 int64 变量来计算，避免
	 * 更昂贵的 numeric 计算。
	 *
	 * 不难看出下列中间值没有溢出 64 位整数的风险。
	 * 最坏情况下，上轮迭代算出的是 3 位数字平方根
	 * （输入为小于 NBASE^6 / 4 的 6 位数字），故本轮
	 * 迭代开始时 s < NBASE^3 / 2 = 10^12 / 2，而 r 小于
	 * 10^12。此时 blen 将为 1，所以 numer 将小于 10^17，
	 * 而 denom 将小于 10^12（因而 u 也将小于 10^12）。
	 *
	 * 最后，由于 q^2 = u*b + a0 - r，我们还能确信
	 * q^2 < 10^17。因此所有这些量都能轻松装进
	 * 64 位整数。
	 */
	step--;
	while (step >= 0 && (src_ndigits = ndigits[step]) <= 8)
	{
		int			b;
		int			a0;
		int			a1;
		int			i;
		int64		numer;
		int64		denom;
		int64		q;
		int64		u;

		blen = (src_ndigits - src_idx) / 2;

		/* 提取 a1 与 a0，并计算 b */
		a0 = 0;
		a1 = 0;
		b = 1;

		for (i = 0; i < blen; i++, src_idx++)
		{
			b *= NBASE;
			a1 *= NBASE;
			if (src_idx < arg->ndigits)
				a1 += arg->digits[src_idx];
		}

		for (i = 0; i < blen; i++, src_idx++)
		{
			a0 *= NBASE;
			if (src_idx < arg->ndigits)
				a0 += arg->digits[src_idx];
		}

		/* 计算 (q,u) = DivRem(r*b + a1, 2*s) */
		numer = r_int64 * b + a1;
		denom = 2 * s_int64;
		q = numer / denom;
		u = numer - q * denom;

		/* 计算 s = s*b + q 与 r = u*b + a0 - q^2 */
		s_int64 = s_int64 * b + q;
		r_int64 = u * b + a0 - q * q;

		if (r_int64 < 0)
		{
			/* s 大了 1；设 r += s、s--、r += s */
			r_int64 += s_int64;
			s_int64--;
			r_int64 += s_int64;
		}

		Assert(src_idx == src_ndigits); /* 输入数字位已用尽 */
		step--;
	}

	/*
	 * 在支持 128 位整数的平台上，还能进一步推迟
	 * 求助于 numeric 变量。
	 */
#ifdef HAVE_INT128
	if (step >= 0)
	{
		int128		s_int128;
		int128		r_int128;

		s_int128 = s_int64;
		r_int128 = r_int64;

		/*
		 * src_ndigits <= 16 的迭代：
		 *
		 * 结果能装进 int128（即便输入不行），故我们用
		 * int128 变量来计算，避免更昂贵的 numeric 计算。
		 */
		while (step >= 0 && (src_ndigits = ndigits[step]) <= 16)
		{
			int64		b;
			int64		a0;
			int64		a1;
			int64		i;
			int128		numer;
			int128		denom;
			int128		q;
			int128		u;

			blen = (src_ndigits - src_idx) / 2;

			/* 提取 a1 与 a0，并计算 b */
			a0 = 0;
			a1 = 0;
			b = 1;

			for (i = 0; i < blen; i++, src_idx++)
			{
				b *= NBASE;
				a1 *= NBASE;
				if (src_idx < arg->ndigits)
					a1 += arg->digits[src_idx];
			}

			for (i = 0; i < blen; i++, src_idx++)
			{
				a0 *= NBASE;
				if (src_idx < arg->ndigits)
					a0 += arg->digits[src_idx];
			}

			/* 计算 (q,u) = DivRem(r*b + a1, 2*s) */
			numer = r_int128 * b + a1;
			denom = 2 * s_int128;
			q = numer / denom;
			u = numer - q * denom;

			/* 计算 s = s*b + q 与 r = u*b + a0 - q^2 */
			s_int128 = s_int128 * b + q;
			r_int128 = u * b + a0 - q * q;

			if (r_int128 < 0)
			{
				/* s 大了 1；设 r += s、s--、r += s */
				r_int128 += s_int128;
				s_int128--;
				r_int128 += s_int128;
			}

			Assert(src_idx == src_ndigits); /* 输入数字位已用尽 */
			step--;
		}

		/*
		 * 其余迭代都需要 numeric 变量。把整数值转换成
		 * NumericVar 并继续。注意在最后一次迭代中我们不需
		 * 要余数，因此可以在那里不把它算完来省下
		 * 几个周期。
		 */
		int128_to_numericvar(s_int128, &s_var);
		if (step >= 0)
			int128_to_numericvar(r_int128, &r_var);
	}
	else
	{
		int64_to_numericvar(s_int64, &s_var);
		/* step < 0，肯定不需要 r */
	}
#else							/* !HAVE_INT128 */
	int64_to_numericvar(s_int64, &s_var);
	if (step >= 0)
		int64_to_numericvar(r_int64, &r_var);
#endif							/* HAVE_INT128 */

	/*
	 * src_ndigits > 8（如果有 int128 则为 16）的其余迭代
	 * 使用 numeric 变量。
	 */
	while (step >= 0)
	{
		int			tmp_len;

		src_ndigits = ndigits[step];
		blen = (src_ndigits - src_idx) / 2;

		/* 提取 a1 与 a0 */
		if (src_idx < arg->ndigits)
		{
			tmp_len = Min(blen, arg->ndigits - src_idx);
			alloc_var(&a1_var, tmp_len);
			memcpy(a1_var.digits, arg->digits + src_idx,
				   tmp_len * sizeof(NumericDigit));
			a1_var.weight = blen - 1;
			a1_var.sign = NUMERIC_POS;
			a1_var.dscale = 0;
			strip_var(&a1_var);
		}
		else
		{
			zero_var(&a1_var);
			a1_var.dscale = 0;
		}
		src_idx += blen;

		if (src_idx < arg->ndigits)
		{
			tmp_len = Min(blen, arg->ndigits - src_idx);
			alloc_var(&a0_var, tmp_len);
			memcpy(a0_var.digits, arg->digits + src_idx,
				   tmp_len * sizeof(NumericDigit));
			a0_var.weight = blen - 1;
			a0_var.sign = NUMERIC_POS;
			a0_var.dscale = 0;
			strip_var(&a0_var);
		}
		else
		{
			zero_var(&a0_var);
			a0_var.dscale = 0;
		}
		src_idx += blen;

		/* 计算 (q,u) = DivRem(r*b + a1, 2*s) */
		set_var_from_var(&r_var, &q_var);
		q_var.weight += blen;
		add_var(&q_var, &a1_var, &q_var);
		add_var(&s_var, &s_var, &u_var);
		div_mod_var(&q_var, &u_var, &q_var, &u_var);

		/* 计算 s = s*b + q */
		s_var.weight += blen;
		add_var(&s_var, &q_var, &s_var);

		/*
		 * 求 r = u*b + a0 - q^2。
		 *
		 * 最后一轮迭代中并不真正需要 r，只需知道它是
		 * 否为负以便知道要不要调 s。因此最后一次减法可
		 * 以换成一次比较。
		 */
		u_var.weight += blen;
		add_var(&u_var, &a0_var, &u_var);
		mul_var(&q_var, &q_var, &q_var, 0);

		if (step > 0)
		{
			/* 后续迭代需要 r */
			sub_var(&u_var, &q_var, &r_var);
			if (r_var.sign == NUMERIC_NEG)
			{
				/* s 大了 1；设 r += s、s--、r += s */
				add_var(&r_var, &s_var, &r_var);
				sub_var(&s_var, &const_one, &s_var);
				add_var(&r_var, &s_var, &r_var);
			}
		}
		else
		{
			/* 不再需要 r，除非要测试 s 是否大了 1 */
			if (cmp_var(&u_var, &q_var) < 0)
				sub_var(&s_var, &const_one, &s_var);
		}

		Assert(src_idx == src_ndigits); /* 输入数字位已用尽 */
		step--;
	}

	/*
	 * 构造最终结果，并把它舍入到要求的精度。
	 */
	set_var_from_var(&s_var, result);
	result->weight = res_weight;
	result->sign = NUMERIC_POS;

	/* 舍入到目标 rscale（设 result->dscale） */
	round_var(result, rscale);

	/* 去除前导与末尾零 */
	strip_var(result);

	free_var(&s_var);
	free_var(&r_var);
	free_var(&a0_var);
	free_var(&a1_var);
	free_var(&q_var);
	free_var(&u_var);
}


/*
 * exp_var() -
 *
 *	计算 e 的 x 次幂，精确到 rscale 位小数
 */
static void
exp_var(const NumericVar *arg, NumericVar *result, int rscale)
{
	NumericVar	x;
	NumericVar	elem;
	int			ni;
	double		val;
	int			dweight;
	int			ndiv2;
	int			sig_digits;
	int			local_rscale;

	init_var(&x);
	init_var(&elem);

	set_var_from_var(arg, &x);

	/*
	 * 用浮点算术估计结果的 dweight，以便能为计算选择
	 * 合适的局部 rscale。
	 */
	val = numericvar_to_double_no_overflow(&x);

	/* 防止溢出/下溢 */
	/* 若改此限制，另见 power_var() 的限制 */
	if (fabs(val) >= NUMERIC_MAX_RESULT_SCALE * 3)
	{
		if (val > 0)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value overflows numeric format")));
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/* 十进制权重=log10(e^x)=x*log10(e) */
	dweight = (int) (val * 0.434294481903252);

	/*
	 * 通过除以 2^ndiv2 把 x 缩减到约 -0.01 <= x <= 0.01 的
	 * 范围，以改善泰勒级数的收敛速度。
	 *
	 * 注意上面的溢出检查确保了 fabs(x) < 6000，这意味着
	 * 此处 ndiv2 <= 20。
	 */
	if (fabs(val) > 0.01)
	{
		ndiv2 = 1;
		val /= 2;

		while (fabs(val) > 0.01)
		{
			ndiv2++;
			val /= 2;
		}

		local_rscale = x.dscale + ndiv2;
		div_var_int(&x, 1 << ndiv2, 0, &x, local_rscale, true);
	}
	else
		ndiv2 = 0;

	/*
	 * 为泰勒级数展开设置标度。最终结果有 (dweight +
	 * rscale + 1) 个有效数字。此外，我们还须把泰勒级数
	 * 的结果要自乘 2^ndiv2 次幂，这会带来最多约
	 * log10(2^ndiv2) 位数字的误差，所以要多用这么多位
	 * 额外的精度数字（外加几分保险余量）。
	 */
	sig_digits = 1 + dweight + rscale + (int) (ndiv2 * 0.301029995663981);
	sig_digits = Max(sig_digits, 0) + 8;

	local_rscale = sig_digits - 1;

	/*
	 * 使用泰勒级数
	 *
	 * exp(x) = 1 + x + x^2/2! + x^3/3! + ...
	 *
	 * 鉴于 x 范围有限，该级数应收敛得相当快。我们一直
	 * 运行级数直到各项低于 local_rscale 限制为止。
	 */
	add_var(&const_one, &x, result);

	mul_var(&x, &x, &elem, local_rscale);
	ni = 2;
	div_var_int(&elem, ni, 0, &elem, local_rscale, true);

	while (elem.ndigits != 0)
	{
		add_var(result, &elem, result);

		mul_var(&elem, &x, &elem, local_rscale);
		ni++;
		div_var_int(&elem, ni, 0, &elem, local_rscale, true);
	}

	/*
	 * 补偿参数范围缩减。结果的权重每乘一次就翻
	 * 倍，所以可以随着进行而缩减局部 rscale。
	 *
	 */
	while (ndiv2-- > 0)
	{
		local_rscale = sig_digits - result->weight * 2 * DEC_DIGITS;
		local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);
		mul_var(result, result, result, local_rscale);
	}

	/* 舍入到要求的 rscale */
	round_var(result, rscale);

	free_var(&x);
	free_var(&elem);
}


/*
 * 估计一个数自然对数的最高有效十进制数字位的
 * dweight。
 *
 * 本质上，我们是在近似 log10(abs(ln(var)))。这用于
 * 在计算自然对数时确定合适的 rscale。
 *
 * 注意：很多调用方会在对输入做范围检查之前调用本函
 * 数。因此我们必须对不适用于 ln() 的值保持稳健。
 * 我们不想在这里抛出错误，所以这种情况直接返回零。
 */
static int
estimate_ln_dweight(const NumericVar *var)
{
	int			ln_dweight;

	/* 调用方对 ln(负数) 应报错，暂返回零 */
	if (var->sign != NUMERIC_POS)
		return 0;

	if (cmp_var(var, &const_zero_point_nine) >= 0 &&
		cmp_var(var, &const_one_point_one) <= 0)
	{
		/*
		 * 0.9 <= var <= 1.1
		 *
		 * ln(var) 权重为负（可能很大）。要得到较精确的
		 * 确的结果，可用 ln(1+x) ~= x 来估计。
		 */
		NumericVar	x;

		init_var(&x);
		sub_var(var, &const_one, &x);

		if (x.ndigits > 0)
		{
			/* 用 x 的最高十进制数字位权重 */
			ln_dweight = x.weight * DEC_DIGITS + (int) log10(x.digits[0]);
		}
		else
		{
			/* x = 0。ln(1) = 0 是精确的，无需额外数字位 */
			ln_dweight = 0;
		}

		free_var(&x);
	}
	else
	{
		/*
		 * 用输入数的头几位数字位估计对数。只要输入不
		 * 太接近 1，这都能给出精确的结果。
		 *
		 */
		if (var->ndigits > 0)
		{
			int			digits;
			int			dweight;
			double		ln_var;

			digits = var->digits[0];
			dweight = var->weight * DEC_DIGITS;

			if (var->ndigits > 1)
			{
				digits = digits * NBASE + var->digits[1];
				dweight -= DEC_DIGITS;
			}

			/*----------
			 * 即 var ~= digits * 10^dweight
			 * 故 ln(var) ~= ln(digits) + dweight * ln(10)
			 *----------
			 */
			ln_var = log((double) digits) + dweight * 2.302585092994046;
			ln_dweight = (int) log10(fabs(ln_var));
		}
		else
		{
			/* 调用方对 ln(0) 应报错，暂返回零 */
			ln_dweight = 0;
		}
	}

	return ln_dweight;
}


/*
 * ln_var() -
 *
 *	计算 x 的自然对数
 */
static void
ln_var(const NumericVar *arg, NumericVar *result, int rscale)
{
	NumericVar	x;
	NumericVar	xx;
	int			ni;
	NumericVar	elem;
	NumericVar	fact;
	int			nsqrt;
	int			local_rscale;
	int			cmp;

	cmp = cmp_var(arg, &const_zero);
	if (cmp == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of zero")));
	else if (cmp < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
				 errmsg("cannot take logarithm of a negative number")));

	init_var(&x);
	init_var(&xx);
	init_var(&elem);
	init_var(&fact);

	set_var_from_var(arg, &x);
	set_var_from_var(&const_two, &fact);

	/*
	 * 通过反复 sqrt() 把输入缩减到 0.9 < x < 1.1 范围。
	 *
	 * 最终的对数将有约 rscale+6 个有效数字。每次 sqrt() 都
	 * 会大致把 x 的权重减半，所以工作中要调整局部
	 * rscale，以便每一步都保持这么多有效数字（外加几
	 * 分保险余量）。
	 *
	 * 注意在输入缩减过程中允许 local_rscale < 0，这
	 * 意味着在小数点之前舍入。sqrt_var() 明确支持这一
	 * 点，并显著减少了把超大输入缩到所需范围所
	 * 需的工作。输入缩减一旦完成，x.weight 将为 0，且
	 * 其显示标度将重新变为非负。
	 *
	 */
	nsqrt = 0;
	while (cmp_var(&x, &const_zero_point_nine) <= 0)
	{
		local_rscale = rscale - x.weight * DEC_DIGITS / 2 + 8;
		sqrt_var(&x, &x, local_rscale);
		mul_var(&fact, &const_two, &fact, 0);
		nsqrt++;
	}
	while (cmp_var(&x, &const_one_point_one) >= 0)
	{
		local_rscale = rscale - x.weight * DEC_DIGITS / 2 + 8;
		sqrt_var(&x, &x, local_rscale);
		mul_var(&fact, &const_two, &fact, 0);
		nsqrt++;
	}

	/*
	 * 我们用 0.5 * ln((1+z)/(1-z)) 的泰勒级数，
	 *
	 * z + z^3/3 + z^5/5 + ...
	 *
	 * 其中 z = (x-1)/(x+1) 因上述对 x 的范围缩减而处于
	 * 约 -0.053 .. 0.048 的范围内。
	 *
	 * 其收敛速度不如人意，但鉴于 z 很小尚可忍受。
	 *
	 *
	 * 泰勒级数的结果将乘以 2^(nsqrt+1)，其十进制权重为
	 * (nsqrt+1) * log10(2)，所以要多用这么多位额外的精
	 * 度数字（外加几分保险余量）。
	 */
	local_rscale = rscale + (int) ((nsqrt + 1) * 0.301029995663981) + 8;

	sub_var(&x, &const_one, result);
	add_var(&x, &const_one, &elem);
	div_var_fast(result, &elem, result, local_rscale, true);
	set_var_from_var(result, &xx);
	mul_var(result, result, &x, local_rscale);

	ni = 1;

	for (;;)
	{
		ni += 2;
		mul_var(&xx, &x, &xx, local_rscale);
		div_var_int(&xx, ni, 0, &elem, local_rscale, true);

		if (elem.ndigits == 0)
			break;

		add_var(result, &elem, result);

		if (elem.weight < (result->weight - local_rscale * 2 / DEC_DIGITS))
			break;
	}

	/* 补偿参数范围缩减，舍入到要求的 rscale */
	mul_var(result, &fact, result, rscale);

	free_var(&x);
	free_var(&xx);
	free_var(&elem);
	free_var(&fact);
}


/*
 * log_var() -
 *
 *	以给定的基计算 num 的对数。
 *
 *	注意：本例程会选择结果的 dscale。
 */
static void
log_var(const NumericVar *base, const NumericVar *num, NumericVar *result)
{
	NumericVar	ln_base;
	NumericVar	ln_num;
	int			ln_base_dweight;
	int			ln_num_dweight;
	int			result_dweight;
	int			rscale;
	int			ln_base_rscale;
	int			ln_num_rscale;

	init_var(&ln_base);
	init_var(&ln_num);

	/* ln(base)、ln(num) 与最终结果的估计 dweight */
	ln_base_dweight = estimate_ln_dweight(base);
	ln_num_dweight = estimate_ln_dweight(num);
	result_dweight = ln_num_dweight - ln_base_dweight;

	/*
	 * 选择结果标度，使其至少有
	 * NUMERIC_MIN_SIG_DIGITS 个有效数字，且不低于任一输入
	 * 的显示标度。
	 */
	rscale = NUMERIC_MIN_SIG_DIGITS - result_dweight;
	rscale = Max(rscale, base->dscale);
	rscale = Max(rscale, num->dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/*
	 * 设置 ln(base)/ln(num) 的标度，使其各自的有效数字
	 * 个数都多于最终结果。
	 */
	ln_base_rscale = rscale + result_dweight - ln_base_dweight + 8;
	ln_base_rscale = Max(ln_base_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	ln_num_rscale = rscale + result_dweight - ln_num_dweight + 8;
	ln_num_rscale = Max(ln_num_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	/* 求出自然对数 */
	ln_var(base, &ln_base, ln_base_rscale);
	ln_var(num, &ln_num, ln_num_rscale);

	/* 相除并舍入到要求的标度 */
	div_var_fast(&ln_num, &ln_base, result, rscale, true);

	free_var(&ln_num);
	free_var(&ln_base);
}


/*
 * power_var() -
 *
 *	计算 base 的 exp 次幂
 *
 *	注意：本例程会选择结果的 dscale。
 */
static void
power_var(const NumericVar *base, const NumericVar *exp, NumericVar *result)
{
	int			res_sign;
	NumericVar	abs_base;
	NumericVar	ln_base;
	NumericVar	ln_num;
	int			ln_dweight;
	int			rscale;
	int			sig_digits;
	int			local_rscale;
	double		val;

	/* 若 exp 能表示为整数，则用 power_var_int */
	if (exp->ndigits == 0 || exp->ndigits <= exp->weight + 1)
	{
		/* 精确整数，int 装得下？ */
		int64		expval64;

		if (numericvar_to_int64(exp, &expval64))
		{
			if (expval64 >= PG_INT32_MIN && expval64 <= PG_INT32_MAX)
			{
				/* 好，用 power_var_int */
				power_var_int(base, (int) expval64, exp->dscale, result);
				return;
			}
		}
	}

	/*
	 * 这避免了 0 的非整数次幂情形下的 log(0)。0 ^ 0 由
	 * power_var_int() 处理。
	 */
	if (cmp_var(base, &const_zero) == 0)
	{
		set_var_from_var(&const_zero, result);
		result->dscale = NUMERIC_MIN_SIG_DIGITS;	/* 无需舍入 */
		return;
	}

	init_var(&abs_base);
	init_var(&ln_base);
	init_var(&ln_num);

	/*
	 * 若 base 为负，则坚持要求 exp 为整数。此时若 exp 为偶
	 * 数则结果为正，exp 为奇数则为负。
	 */
	if (base->sign == NUMERIC_NEG)
	{
		/*
		 * 检查 exp 是否为整数。此错误码由 SQL 标准定义，
		 * 并与 numeric_power() 中的其他错误一致。
		 */
		if (exp->ndigits > 0 && exp->ndigits > exp->weight + 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
					 errmsg("a negative number raised to a non-integer power yields a complex result")));

		/* 测试 exp 为奇还是偶 */
		if (exp->ndigits > 0 && exp->ndigits == exp->weight + 1 &&
			(exp->digits[exp->ndigits - 1] & 1))
			res_sign = NUMERIC_NEG;
		else
			res_sign = NUMERIC_POS;

		/* 下文改用 abs(base) 计算 */
		set_var_from_var(base, &abs_base);
		abs_base.sign = NUMERIC_POS;
		base = &abs_base;
	}
	else
		res_sign = NUMERIC_POS;

	/*----------
	 * 选定 ln() 计算的标度。为此我们需要估计结果的
	 * 估计值，其来源是先做一次 exp * ln(base) 的低精度
	 * 初始计算。
	 *
	 * 即 result = e ^ (exp * ln(base))
	 * 故 result dweight = log10(result) = exp * ln(base) * log10(e)
	 *
	 * 这里还做了一个粗略溢出测试，以便在全精度结果
	 * 必然溢出时提前退出，并防止真实计算定标度时发
	 * 生整数溢出。
	 * exp_var() 支持最大到 NUMERIC_MAX_RESULT_SCALE * 3 的
	 * 输入，故当 exp * ln(base) >= NUMERIC_MAX_RESULT_SCALE * 3
	 * 时结果会溢出。由于这里的值只是近似，我们对此
	 * 溢出测试施加小容差因子，并让 exp_var() 确定精确
	 * 的溢出阈值，以使其对所有输入都一致。
	 *----------
	 */
	ln_dweight = estimate_ln_dweight(base);

	/*
	 * 为低精度计算设置标度，把 ln(base) 算到约 8 个有效数
	 * 字。注意 ln_dweight 可能小到 -NUMERIC_DSCALE_MAX，
	 * 所以此处的标度可能超过 NUMERIC_MAX_DISPLAY_SCALE。
	 *
	 */
	local_rscale = 8 - ln_dweight;
	local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	ln_var(base, &ln_base, local_rscale);

	mul_var(&ln_base, exp, &ln_num, local_rscale);

	val = numericvar_to_double_no_overflow(&ln_num);

	/* 带容差因子的初始溢出/下溢测试 */
	if (fabs(val) > NUMERIC_MAX_RESULT_SCALE * 3.01)
	{
		if (val > 0)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value overflows numeric format")));
		zero_var(result);
		result->dscale = NUMERIC_MAX_DISPLAY_SCALE;
		return;
	}

	val *= 0.434294481903252;	/* 十进制结果权重的近似值 */

	/* 选择结果标度 */
	rscale = NUMERIC_MIN_SIG_DIGITS - (int) val;
	rscale = Max(rscale, base->dscale);
	rscale = Max(rscale, exp->dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/* 结果中要求的有效数字位数 */
	sig_digits = rscale + (int) val;
	sig_digits = Max(sig_digits, 0);

	/* 为真实的 exp * ln(base) 计算设置标度 */
	local_rscale = sig_digits - ln_dweight + 8;
	local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	/* 然后做真正的计算 */

	ln_var(base, &ln_base, local_rscale);

	mul_var(&ln_base, exp, &ln_num, local_rscale);

	exp_var(&ln_num, result, rscale);

	if (res_sign == NUMERIC_NEG && result->ndigits > 0)
		result->sign = NUMERIC_NEG;

	free_var(&ln_num);
	free_var(&ln_base);
	free_var(&abs_base);
}

/*
 * power_var_int() -
 *
 *	计算 base 的 exp 次幂，其中 exp 为整数。
 *
 *	注意：本例程会选择结果的 dscale。
 */
static void
power_var_int(const NumericVar *base, int exp, int exp_dscale,
			  NumericVar *result)
{
	double		f;
	int			p;
	int			i;
	int			rscale;
	int			sig_digits;
	unsigned int mask;
	bool		neg;
	NumericVar	base_prod;
	int			local_rscale;

	/*
	 * 选定结果标度。为此需要估计结果的十进制权重，
	 * 其来源是用双精度算术做近似而得。
	 *
	 *
	 * 这里还做粗略的溢出/下溢测试，以便在结果必
	 * 然溢出/下溢时提前退出，并防止选择结果标度时发生
	 * 整数溢出。
	 */
	if (base->ndigits != 0)
	{
		/*----------
		 * 选 f（double）和 p（int）使 base ~= f * 10^p。
		 * 则 log10(result) = log10(base^exp) ~= exp * (log10(f) + p)。
		 *----------
		 */
		f = base->digits[0];
		p = base->weight * DEC_DIGITS;

		for (i = 1; i < base->ndigits && i * DEC_DIGITS < 16; i++)
		{
			f = f * NBASE + base->digits[i];
			p -= DEC_DIGITS;
		}

		f = exp * (log10(f) + p);	/* 十进制结果权重的近似值 */
	}
	else
		f = 0;					/* 结果为 0/1（权重 0）或出错 */

	/* 带容差因子的溢出/下溢测试 */
	if (f > (NUMERIC_WEIGHT_MAX + 1) * DEC_DIGITS)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value overflows numeric format")));
	if (f + 1 < -NUMERIC_MAX_DISPLAY_SCALE)
	{
		zero_var(result);
		result->dscale = NUMERIC_MAX_DISPLAY_SCALE;
		return;
	}

	/*
	 * 按与 power_var() 相同的方式选择结果标度，使其至少
	 * 有 NUMERIC_MIN_SIG_DIGITS 个有效数字，且不低于任一
	 * 输入的显示标度。
	 */
	rscale = NUMERIC_MIN_SIG_DIGITS - (int) f;
	rscale = Max(rscale, base->dscale);
	rscale = Max(rscale, exp_dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/* 处理一些常见的特殊情形以及边角情形 */
	switch (exp)
	{
		case 0:

			/*
			 * 0 ^ 0 可为 1 或不确定（出错），多数编程语言都
			 * 把它当 1，我们也如此处理。SQL:2003 也要求返
			 * 回值为 1。
			 * https://en.wikipedia.org/wiki/Exponentiation#Zero_to_the_zero_power
			 */
			set_var_from_var(&const_one, result);
			result->dscale = rscale;	/* 无需舍入 */
			return;
		case 1:
			set_var_from_var(base, result);
			round_var(result, rscale);
			return;
		case -1:
			div_var(&const_one, base, result, rscale, true);
			return;
		case 2:
			mul_var(base, base, result, rscale);
			return;
		default:
			break;
	}

	/* 处理底数为零的特殊情形 */
	if (base->ndigits == 0)
	{
		if (exp < 0)
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * 一般情形是根据 exp 的位模式反复相乘 base。
	 *
	 *
	 * 每次乘法所用局部 rscale 会动态调整，以保持固定数
	 * 量的有效数字，足以给出所需的结果标度。
	 *
	 */

	/*
	 * 结果中有效数字个数的近似值。注意上面的下溢测试
	 * 连同 rscale 的选取保证了该近似值必然 > 0。
	 *
	 */
	sig_digits = 1 + rscale + (int) f;

	/*
	 * 为产生结果而做的乘法可能引入至多约 log10(abs(exp))
	 * 位数字的误差，故要多用这么多位额外的精度数字
	 * （外加几分保险余量）。
	 */
	sig_digits += (int) log(fabs((double) exp)) + 8;

	/*
	 * 现在可以开始进行乘法了。
	 */
	neg = (exp < 0);
	mask = abs(exp);

	init_var(&base_prod);
	set_var_from_var(base, &base_prod);

	if (mask & 1)
		set_var_from_var(base, result);
	else
		set_var_from_var(&const_one, result);

	while ((mask >>= 1) > 0)
	{
		/*
		 * 用能容下所需有效数字的 rscale 做乘法，但不浪
		 * 费时间把标度超出数字本身的标度。
		 *
		 */
		local_rscale = sig_digits - 2 * base_prod.weight * DEC_DIGITS;
		local_rscale = Min(local_rscale, 2 * base_prod.dscale);
		local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);

		mul_var(&base_prod, &base_prod, &base_prod, local_rscale);

		if (mask & 1)
		{
			local_rscale = sig_digits -
				(base_prod.weight + result->weight) * DEC_DIGITS;
			local_rscale = Min(local_rscale,
							   base_prod.dscale + result->dscale);
			local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);

			mul_var(&base_prod, result, result, local_rscale);
		}

		/*
		 * 当 abs(base) > 1 时，base_prod 小数点左边的数字位数
		 * 每轮迭代翻倍，若 exp 很大，可能耗费大量时间与
		 * 内存来做这些乘法。但一旦权重超过 int16 能容纳
		 * 的量，最终结果必然溢出（exp < 0 时则下溢），
		 * 所以我们能在浪费过多时间之前放弃。
		 *
		 */
		if (base_prod.weight > NUMERIC_WEIGHT_MAX ||
			result->weight > NUMERIC_WEIGHT_MAX)
		{
			/* 溢出，除非 neg 为真，那时结果应为 0 */
			if (!neg)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value overflows numeric format")));
			zero_var(result);
			neg = false;
			break;
		}
	}

	free_var(&base_prod);

	/* 补偿输入符号，并舍入到要求的 rscale */
	if (neg)
		div_var_fast(&const_one, result, result, rscale, true);
	else
		round_var(result, rscale);
}

/*
 * power_ten_int() -
 *
 *	计算 10 的 exp 次幂，其中 exp 为整数。注意与
 *	power_var_int() 不同，这里不做溢出/下溢检查或舍入。
 */
static void
power_ten_int(int exp, NumericVar *result)
{
	/* 从 10^0 = 1 出发直接构造结果 */
	set_var_from_var(&const_one, result);

	/* 精确表示结果所需的标度 */
	result->dscale = exp < 0 ? -exp : 0;

	/* 结果的 base-NBASE 权重与剩余指数 */
	if (exp >= 0)
		result->weight = exp / DEC_DIGITS;
	else
		result->weight = (exp + 1) / DEC_DIGITS - 1;

	exp -= result->weight * DEC_DIGITS;

	/* 对结果唯一 NBASE 数字位的最终调整 */
	while (exp-- > 0)
		result->digits[0] *= 10;
}


/* ----------------------------------------------------------------------
 *
 * 以下为变量层面无符号运作的最低层各函数。
 *
 *
 * ----------------------------------------------------------------------
 */


/* ----------
 * cmp_abs() -
 *
 *	比较 var1 与 var2 的绝对值
 *	返回：	-1 ABS(var1) < ABS(var2)
 *				0 ABS(var1) == ABS(var2)
 *				1 ABS(var1) > ABS(var2)
 * ----------
 */
static int
cmp_abs(const NumericVar *var1, const NumericVar *var2)
{
	return cmp_abs_common(var1->digits, var1->ndigits, var1->weight,
						  var2->digits, var2->ndigits, var2->weight);
}

/* ----------
 * cmp_abs_common() -
 *
 *	cmp_abs() 的主例程。NumericVar 和
 *	Numeric 皆可用。
 * ----------
 */
static int
cmp_abs_common(const NumericDigit *var1digits, int var1ndigits, int var1weight,
			   const NumericDigit *var2digits, int var2ndigits, int var2weight)
{
	int			i1 = 0;
	int			i2 = 0;

	/* 检查首个共同数字位前的各数字位 */

	while (var1weight > var2weight && i1 < var1ndigits)
	{
		if (var1digits[i1++] != 0)
			return 1;
		var1weight--;
	}
	while (var2weight > var1weight && i2 < var2ndigits)
	{
		if (var2digits[i2++] != 0)
			return -1;
		var2weight--;
	}

	/* 此时要么 w1 == w2，要么数字位已用尽 */

	if (var1weight == var2weight)
	{
		while (i1 < var1ndigits && i2 < var2ndigits)
		{
			int			stat = var1digits[i1++] - var2digits[i2++];

			if (stat)
			{
				if (stat > 0)
					return 1;
				return -1;
			}
		}
	}

	/*
	 * 此时某一边的数字位已用尽；因此另一边任何剩下的
	 * 非零数字位都意味着该侧更大。
	 */
	while (i1 < var1ndigits)
	{
		if (var1digits[i1++] != 0)
			return 1;
	}
	while (i2 < var2ndigits)
	{
		if (var2digits[i2++] != 0)
			return -1;
	}

	return 0;
}


/*
 * add_abs() -
 *
 *	把两个变量的绝对值相加存入 result。
 *	result 指向某个操作数也没有危险。
 */
static void
add_abs(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	NumericDigit *res_buf;
	NumericDigit *res_digits;
	int			res_ndigits;
	int			res_weight;
	int			res_rscale,
				rscale1,
				rscale2;
	int			res_dscale;
	int			i,
				i1,
				i2;
	int			carry = 0;

	/* 把这些值拷贝到局部变量以加速内层循环 */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	res_weight = Max(var1->weight, var2->weight) + 1;

	res_dscale = Max(var1->dscale, var2->dscale);

	/* 注意：此处按 base-NBASE 数字位计算 rscale */
	rscale1 = var1->ndigits - var1->weight - 1;
	rscale2 = var2->ndigits - var2->weight - 1;
	res_rscale = Max(rscale1, rscale2);

	res_ndigits = res_rscale + res_weight + 1;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* 留待舍入的数字位 */
	res_digits = res_buf + 1;

	i1 = res_rscale + var1->weight + 1;
	i2 = res_rscale + var2->weight + 1;
	for (i = res_ndigits - 1; i >= 0; i--)
	{
		i1--;
		i2--;
		if (i1 >= 0 && i1 < var1ndigits)
			carry += var1digits[i1];
		if (i2 >= 0 && i2 < var2ndigits)
			carry += var2digits[i2];

		if (carry >= NBASE)
		{
			res_digits[i] = carry - NBASE;
			carry = 1;
		}
		else
		{
			res_digits[i] = carry;
			carry = 0;
		}
	}

	Assert(carry == 0);			/* 否则就是未为进位留出空间 */

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->dscale = res_dscale;

	/* 移除前导/末尾零 */
	strip_var(result);
}


/*
 * sub_abs()
 *
 *	从 var1 的绝对值中减去 var2 的绝对值并存入 result。
 *	result 指向某个操作数也没有
 *	危险。
 *
 *	ABS(var1) 必须大于等于 ABS(var2) ！！！
 */
static void
sub_abs(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	NumericDigit *res_buf;
	NumericDigit *res_digits;
	int			res_ndigits;
	int			res_weight;
	int			res_rscale,
				rscale1,
				rscale2;
	int			res_dscale;
	int			i,
				i1,
				i2;
	int			borrow = 0;

	/* 把这些值拷贝到局部变量以加速内层循环 */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	res_weight = var1->weight;

	res_dscale = Max(var1->dscale, var2->dscale);

	/* 注意：此处按 base-NBASE 数字位计算 rscale */
	rscale1 = var1->ndigits - var1->weight - 1;
	rscale2 = var2->ndigits - var2->weight - 1;
	res_rscale = Max(rscale1, rscale2);

	res_ndigits = res_rscale + res_weight + 1;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* 留待舍入的数字位 */
	res_digits = res_buf + 1;

	i1 = res_rscale + var1->weight + 1;
	i2 = res_rscale + var2->weight + 1;
	for (i = res_ndigits - 1; i >= 0; i--)
	{
		i1--;
		i2--;
		if (i1 >= 0 && i1 < var1ndigits)
			borrow += var1digits[i1];
		if (i2 >= 0 && i2 < var2ndigits)
			borrow -= var2digits[i2];

		if (borrow < 0)
		{
			res_digits[i] = borrow + NBASE;
			borrow = -1;
		}
		else
		{
			res_digits[i] = borrow;
			borrow = 0;
		}
	}

	Assert(borrow == 0);		/* 调用方给错了 var1 < var2 */

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->dscale = res_dscale;

	/* 移除前导/末尾零 */
	strip_var(result);
}

/*
 * round_var
 *
 * 把变量值舍入到小数点后至多 rscale 位小数。
 * 注意：这里允许 rscale < 0，意味着在小数点之前
 * 进行舍入。
 */
static void
round_var(NumericVar *var, int rscale)
{
	NumericDigit *digits = var->digits;
	int			di;
	int			ndigits;
	int			carry;

	var->dscale = rscale;

	/* 所需十进制位 */
	di = (var->weight + 1) * DEC_DIGITS + rscale;

	/*
	 * 若 di = 0，值失去全部数字位，但若首个额外数字位
	 * 不小于 5 则可向上舍入为 1。di < 0 时必为 0。
	 */
	if (di < 0)
	{
		var->ndigits = 0;
		var->weight = 0;
		var->sign = NUMERIC_POS;
	}
	else
	{
		/* 所需的 NBASE 位 */
		ndigits = (di + DEC_DIGITS - 1) / DEC_DIGITS;

		/* 0，或末位 NBASE 数字位中保留的十进制位数 */
		di %= DEC_DIGITS;

		if (ndigits < var->ndigits ||
			(ndigits == var->ndigits && di > 0))
		{
			var->ndigits = ndigits;

#if DEC_DIGITS == 1
			/* di 必为零 */
			carry = (digits[ndigits] >= HALF_NBASE) ? 1 : 0;
#else
			if (di == 0)
				carry = (digits[ndigits] >= HALF_NBASE) ? 1 : 0;
			else
			{
				/* 须在末个 NBASE 位内舍入 */
				int			extra,
							pow10;

#if DEC_DIGITS == 4
				pow10 = round_powers[di];
#elif DEC_DIGITS == 2
				pow10 = 10;
#else
#error unsupported NBASE
#endif
				extra = digits[--ndigits] % pow10;
				digits[ndigits] -= extra;
				carry = 0;
				if (extra >= pow10 / 2)
				{
					pow10 += digits[ndigits];
					if (pow10 >= NBASE)
					{
						pow10 -= NBASE;
						carry = 1;
					}
					digits[ndigits] = pow10;
				}
			}
#endif

			/* 需要时传播进位 */
			while (carry)
			{
				carry += digits[--ndigits];
				if (carry >= NBASE)
				{
					digits[ndigits] = carry - NBASE;
					carry = 1;
				}
				else
				{
					digits[ndigits] = carry;
					carry = 0;
				}
			}

			if (ndigits < 0)
			{
				Assert(ndigits == -1);	/* 不应多加超过 1 个位 */
				Assert(var->digits > var->buf);
				var->digits--;
				var->ndigits++;
				var->weight++;
			}
		}
	}
}

/*
 * trunc_var
 *
 * 把变量的值在 rscale 位小数处向零截断。
 * 注意：这里允许 rscale < 0，意味着在小数点之前
 * 进行截断。
 */
static void
trunc_var(NumericVar *var, int rscale)
{
	int			di;
	int			ndigits;

	var->dscale = rscale;

	/* 所需十进制位 */
	di = (var->weight + 1) * DEC_DIGITS + rscale;

	/*
	 * di <= 0 时值失去全部数字位。
	 */
	if (di <= 0)
	{
		var->ndigits = 0;
		var->weight = 0;
		var->sign = NUMERIC_POS;
	}
	else
	{
		/* 所需的 NBASE 位 */
		ndigits = (di + DEC_DIGITS - 1) / DEC_DIGITS;

		if (ndigits <= var->ndigits)
		{
			var->ndigits = ndigits;

#if DEC_DIGITS == 1
			/* 无需担心数字位内的处理 */
#else
			/* 0，或末位 NBASE 数字位中保留的十进制位数 */
			di %= DEC_DIGITS;

			if (di > 0)
			{
				/* 须在末个 NBASE 数字位内截断 */
				NumericDigit *digits = var->digits;
				int			extra,
							pow10;

#if DEC_DIGITS == 4
				pow10 = round_powers[di];
#elif DEC_DIGITS == 2
				pow10 = 10;
#else
#error unsupported NBASE
#endif
				extra = digits[--ndigits] % pow10;
				digits[ndigits] -= extra;
			}
#endif
		}
	}
}

/*
 * strip_var
 *
 * 去除 numeric 变量的所有前导与末尾零
 */
static void
strip_var(NumericVar *var)
{
	NumericDigit *digits = var->digits;
	int			ndigits = var->ndigits;

	/* 去除前导零 */
	while (ndigits > 0 && *digits == 0)
	{
		digits++;
		var->weight--;
		ndigits--;
	}

	/* 去除末尾零 */
	while (ndigits > 0 && digits[ndigits - 1] == 0)
		ndigits--;

	/* 若其为零，则规范化符号与权重 */
	if (ndigits == 0)
	{
		var->sign = NUMERIC_POS;
		var->weight = 0;
	}

	var->digits = digits;
	var->ndigits = ndigits;
}


/* ----------------------------------------------------------------------
 *
 * 快速求和累加器函数
 *
 * ----------------------------------------------------------------------
 */

/*
 * 把累加器的值重置为零。存放数字位的缓冲区不会被
 * 释放。
 */
static void
accum_sum_reset(NumericSumAccum *accum)
{
	int			i;

	accum->dscale = 0;
	for (i = 0; i < accum->ndigits; i++)
	{
		accum->pos_digits[i] = 0;
		accum->neg_digits[i] = 0;
	}
}

/*
 * 累加一个新值。
 */
static void
accum_sum_add(NumericSumAccum *accum, const NumericVar *val)
{
	int32	   *accum_digits;
	int			i,
				val_i;
	int			val_ndigits;
	NumericDigit *val_digits;

	/*
	 * 若上次进位传播后累积过多，就现在做，
	 * 以免溢出。（若为进位传播预留两个额外数字位，
	 * 而非一个，则可以允许多于 NBASE - 1。但即便预留
	 * NBASE - 1，这件事也很少需要做，性能差异可以忽略
	 * 不计。）
	 */
	if (accum->num_uncarried == NBASE - 1)
		accum_sum_carry(accum);

	/*
	 * 调整旧值的权重或标度，使其能容纳新值。
	 *
	 */
	accum_sum_rescale(accum, val);

	/* */
	if (val->sign == NUMERIC_POS)
		accum_digits = accum->pos_digits;
	else
		accum_digits = accum->neg_digits;

	/* 把这些值拷贝到局部变量以加速循环 */
	val_ndigits = val->ndigits;
	val_digits = val->digits;

	i = accum->weight - val->weight;
	for (val_i = 0; val_i < val_ndigits; val_i++)
	{
		accum_digits[i] += (int32) val_digits[val_i];
		i++;
	}

	accum->num_uncarried++;
}

/*
 * 传播进位。
 */
static void
accum_sum_carry(NumericSumAccum *accum)
{
	int			i;
	int			ndigits;
	int32	   *dig;
	int32		carry;
	int32		newdig = 0;

	/*
	 * 若上次进位传播以来没有加入新值，则无事可做。
	 *
	 */
	if (accum->num_uncarried == 0)
		return;

	/*
	 * 我们让累加器权重总比进位前容纳当前值所需的大
	 * 1，以确保进位传播时可能的额外数字位有足够的空
	 * 间。除非要求 accum_sum_final() 的调用方自行切换
	 * 到正确的内存上下文，否则我们无法在此扩大缓冲区。
	 *
	 */
	Assert(accum->pos_digits[0] == 0 && accum->neg_digits[0] == 0);

	ndigits = accum->ndigits;

	/* 在正数和中传播进位 */
	dig = accum->pos_digits;
	carry = 0;
	for (i = ndigits - 1; i >= 0; i--)
	{
		newdig = dig[i] + carry;
		if (newdig >= NBASE)
		{
			carry = newdig / NBASE;
			newdig -= carry * NBASE;
		}
		else
			carry = 0;
		dig[i] = newdig;
	}
	/* 是否用掉进位传播预留的数字位？ */
	if (newdig > 0)
		accum->have_carry_space = false;

	/* 负数和也同样处理 */
	dig = accum->neg_digits;
	carry = 0;
	for (i = ndigits - 1; i >= 0; i--)
	{
		newdig = dig[i] + carry;
		if (newdig >= NBASE)
		{
			carry = newdig / NBASE;
			newdig -= carry * NBASE;
		}
		else
			carry = 0;
		dig[i] = newdig;
	}
	if (newdig > 0)
		accum->have_carry_space = false;

	accum->num_uncarried = 0;
}

/*
 * 重新缩放累加器以容纳新值。
 *
 * 如果新值的数字位多于累加器当前的数字位缓冲区，
 * 则扩大缓冲区。
 */
static void
accum_sum_rescale(NumericSumAccum *accum, const NumericVar *val)
{
	int			old_weight = accum->weight;
	int			old_ndigits = accum->ndigits;
	int			accum_ndigits;
	int			accum_weight;
	int			accum_rscale;
	int			val_rscale;

	accum_weight = old_weight;
	accum_ndigits = old_ndigits;

	/*
	 * 新值的权重更大吗？若是则扩大缓冲区，把现有值移
	 * 到新权重，即添加前导零。
	 *
	 *
	 * 我们强制累加器权重总比输入所需大 1，从而在最
	 * 后进位传播阶段有余量容纳一个额外数字位（如有必
	 * 要）。
	 */
	if (val->weight >= accum_weight)
	{
		accum_weight = val->weight + 1;
		accum_ndigits = accum_ndigits + (accum_weight - old_weight);
	}

	/*
	 * 即便新值很小，上次调用 accum_sum_carry() 时可能
	 * 已用掉为进位数字预留的空间。若如此，则扩大空间
	 * 以再容纳一个。
	 */
	else if (!accum->have_carry_space)
	{
		accum_weight++;
		accum_ndigits++;
	}

	/* 新值在右侧是否更宽？ */
	accum_rscale = accum_ndigits - accum_weight - 1;
	val_rscale = val->ndigits - val->weight - 1;
	if (val_rscale > accum_rscale)
		accum_ndigits = accum_ndigits + (val_rscale - accum_rscale);

	if (accum_ndigits != old_ndigits ||
		accum_weight != old_weight)
	{
		int32	   *new_pos_digits;
		int32	   *new_neg_digits;
		int			weightdiff;

		weightdiff = accum_weight - old_weight;

		new_pos_digits = palloc0(accum_ndigits * sizeof(int32));
		new_neg_digits = palloc0(accum_ndigits * sizeof(int32));

		if (accum->pos_digits)
		{
			memcpy(&new_pos_digits[weightdiff], accum->pos_digits,
				   old_ndigits * sizeof(int32));
			pfree(accum->pos_digits);

			memcpy(&new_neg_digits[weightdiff], accum->neg_digits,
				   old_ndigits * sizeof(int32));
			pfree(accum->neg_digits);
		}

		accum->pos_digits = new_pos_digits;
		accum->neg_digits = new_neg_digits;

		accum->weight = accum_weight;
		accum->ndigits = accum_ndigits;

		Assert(accum->pos_digits[0] == 0 && accum->neg_digits[0] == 0);
		accum->have_carry_space = true;
	}

	if (val->dscale > accum->dscale)
		accum->dscale = val->dscale;
}

/*
 * 返回累加器的当前值。这里会执行最后的进位传播，
 * 并把正数和与负数和相加。
 *
 * 与所有其他例程不同，这里不要求调用方切换到保存
 * 累加器的内存上下文。
 */
static void
accum_sum_final(NumericSumAccum *accum, NumericVar *result)
{
	int			i;
	NumericVar	pos_var;
	NumericVar	neg_var;

	if (accum->ndigits == 0)
	{
		set_var_from_var(&const_zero, result);
		return;
	}

	/* 做最后进位 */
	accum_sum_carry(accum);

	/* 创建表示正数和与负数和的 NumericVars */
	init_var(&pos_var);
	init_var(&neg_var);

	pos_var.ndigits = neg_var.ndigits = accum->ndigits;
	pos_var.weight = neg_var.weight = accum->weight;
	pos_var.dscale = neg_var.dscale = accum->dscale;
	pos_var.sign = NUMERIC_POS;
	neg_var.sign = NUMERIC_NEG;

	pos_var.buf = pos_var.digits = digitbuf_alloc(accum->ndigits);
	neg_var.buf = neg_var.digits = digitbuf_alloc(accum->ndigits);

	for (i = 0; i < accum->ndigits; i++)
	{
		Assert(accum->pos_digits[i] < NBASE);
		pos_var.digits[i] = (int16) accum->pos_digits[i];

		Assert(accum->neg_digits[i] < NBASE);
		neg_var.digits[i] = (int16) accum->neg_digits[i];
	}

	/* 然后把它们相加 */
	add_var(&pos_var, &neg_var, result);

	/* 移除前导/末尾零 */
	strip_var(result);
}

/*
 * 拷贝累加器的状态。
 *
 * 假定 'dst' 事先未初始化。不做任何释放旧值的尝试。
 *
 */
static void
accum_sum_copy(NumericSumAccum *dst, NumericSumAccum *src)
{
	dst->pos_digits = palloc(src->ndigits * sizeof(int32));
	dst->neg_digits = palloc(src->ndigits * sizeof(int32));

	memcpy(dst->pos_digits, src->pos_digits, src->ndigits * sizeof(int32));
	memcpy(dst->neg_digits, src->neg_digits, src->ndigits * sizeof(int32));
	dst->num_uncarried = src->num_uncarried;
	dst->ndigits = src->ndigits;
	dst->weight = src->weight;
	dst->dscale = src->dscale;
}

/*
 * 把 'accum2' 的当前值加进 'accum'。
 */
static void
accum_sum_combine(NumericSumAccum *accum, NumericSumAccum *accum2)
{
	NumericVar	tmp_var;

	init_var(&tmp_var);

	accum_sum_final(accum2, &tmp_var);
	accum_sum_add(accum, &tmp_var);

	free_var(&tmp_var);
}
