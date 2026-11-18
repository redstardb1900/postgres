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
 *	Returns the scale, i.e. the count of decimal digits in the fractional part
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
 * Calculate minimum scale for value.
 */
static int
get_min_scale(NumericVar *var)
{
	int			min_scale;
	int			last_digit_pos;

	/*
	 * Ordinarily, the input value will be "stripped" so that the last
	 * NumericDigit is nonzero.  But we don't want to get into an infinite
	 * loop if it isn't, so explicitly find the last nonzero digit.
	 */
	last_digit_pos = var->ndigits - 1;
	while (last_digit_pos >= 0 &&
		   var->digits[last_digit_pos] == 0)
		last_digit_pos--;

	if (last_digit_pos >= 0)
	{
		/* compute min_scale assuming that last ndigit has no zeroes */
		min_scale = (last_digit_pos - var->weight) * DEC_DIGITS;

		/*
		 * We could get a negative result if there are no digits after the
		 * decimal point.  In this case the min_scale must be zero.
		 */
		if (min_scale > 0)
		{
			/*
			 * Reduce min_scale if trailing digit(s) in last NumericDigit are
			 * zero.
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
		min_scale = 0;			/* result if input is zero */

	return min_scale;
}

/*
 * Returns minimum scale required to represent supplied value without loss.
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
 * Reduce scale of numeric value to represent supplied value without loss.
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
 * Type conversion functions
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
 * Convert val1/(10**log10val2) to numeric.  This is much faster than normal
 * numeric division.
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

	/* result scale */
	rscale = log10val2 < 0 ? 0 : log10val2;

	/* how much to decrease the weight by */
	w = log10val2 / DEC_DIGITS;
	/* how much is left to divide by */
	m = log10val2 % DEC_DIGITS;
	if (m < 0)
	{
		m += DEC_DIGITS;
		w--;
	}

	/*
	 * If there is anything left to divide by (10^m with 0 < m < DEC_DIGITS),
	 * multiply the dividend by 10^(DEC_DIGITS - m), and shift the weight by
	 * one more.
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
			/* do the multiplication using 128-bit integers */
			int128		tmp;

			tmp = (int128) val1 * (int128) factor;

			int128_to_numericvar(tmp, &result);
#else
			/* do the multiplication using numerics */
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

	/* Convert to variable format, then convert to int4 */
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
 * Given a NumericVar, convert it to an int32. If the NumericVar
 * exceeds the range of an int32, false is returned, otherwise true is returned.
 * The input NumericVar is *not* free'd.
 */
static bool
numericvar_to_int32(const NumericVar *var, int32 *result)
{
	int64		val;

	if (!numericvar_to_int64(var, &val))
		return false;

	if (unlikely(val < PG_INT32_MIN) || unlikely(val > PG_INT32_MAX))
		return false;

	/* Down-convert to int4 */
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

	/* Convert to variable format and thence to int8 */
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

	/* Convert to variable format and thence to int8 */
	init_var_from_num(num, &x);

	if (!numericvar_to_int64(&x, &val))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	if (unlikely(val < PG_INT16_MIN) || unlikely(val > PG_INT16_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	/* Down-convert to int2 */
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

	/* Assume we need not worry about leading/trailing spaces */
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
 * Convert numeric to float8; if out of range, return +/- HUGE_VAL
 *
 * (internal helper function, not directly callable from SQL)
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

	/* Assume we need not worry about leading/trailing spaces */
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

	/* Convert to variable format and thence to pg_lsn */
	init_var_from_num(num, &x);

	if (!numericvar_to_uint64(&x, (uint64 *) &result))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_lsn out of range")));

	PG_RETURN_LSN(result);
}


/* ----------------------------------------------------------------------
 *
 * Aggregate functions
 *
 * The transition datatype for all these aggregates is declared as INTERNAL.
 * Actually, it's a pointer to a NumericAggState allocated in the aggregate
 * context.  The digit buffers for the NumericVars will be there too.
 *
 * On platforms which support 128-bit integers some aggregates instead use a
 * 128-bit integer based transition datatype to speed up calculations.
 *
 * ----------------------------------------------------------------------
 */

typedef struct NumericAggState
{
	bool		calcSumX2;		/* if true, calculate sumX2 */
	MemoryContext agg_context;	/* context we're calculating in */
	int64		N;				/* count of processed numbers */
	NumericSumAccum sumX;		/* sum of processed numbers */
	NumericSumAccum sumX2;		/* sum of squares of processed numbers */
	int			maxScale;		/* maximum scale seen so far */
	int64		maxScaleCount;	/* number of values seen with maximum scale */
	/* These counts are *not* included in N!  Use NA_TOTAL_COUNT() as needed */
	int64		NaNcount;		/* count of NaN values */
	int64		pInfcount;		/* count of +Inf values */
	int64		nInfcount;		/* count of -Inf values */
} NumericAggState;

#define NA_TOTAL_COUNT(na) \
	((na)->N + (na)->NaNcount + (na)->pInfcount + (na)->nInfcount)

/*
 * Prepare state data for a numeric aggregate function that needs to compute
 * sum, count and optionally sum of squares of the input.
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
 * Like makeNumericAggState(), but allocate the state in the current memory
 * context.
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
 * Accumulate a new input value for numeric aggregate functions.
 */
static void
do_numeric_accum(NumericAggState *state, Numeric newval)
{
	NumericVar	X;
	NumericVar	X2;
	MemoryContext old_context;

	/* Count NaN/infinity inputs separately from all else */
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

	/* load processed number in short-lived context */
	init_var_from_num(newval, &X);

	/*
	 * Track the highest input dscale that we've seen, to support inverse
	 * transitions (see do_numeric_discard).
	 */
	if (X.dscale > state->maxScale)
	{
		state->maxScale = X.dscale;
		state->maxScaleCount = 1;
	}
	else if (X.dscale == state->maxScale)
		state->maxScaleCount++;

	/* if we need X^2, calculate that in short-lived context */
	if (state->calcSumX2)
	{
		init_var(&X2);
		mul_var(&X, &X, &X2, X.dscale * 2);
	}

	/* The rest of this needs to work in the aggregate context */
	old_context = MemoryContextSwitchTo(state->agg_context);

	state->N++;

	/* Accumulate sums */
	accum_sum_add(&(state->sumX), &X);

	if (state->calcSumX2)
		accum_sum_add(&(state->sumX2), &X2);

	MemoryContextSwitchTo(old_context);
}

/*
 * Attempt to remove an input value from the aggregated state.
 *
 * If the value cannot be removed then the function will return false; the
 * possible reasons for failing are described below.
 *
 * If we aggregate the values 1.01 and 2 then the result will be 3.01.
 * If we are then asked to un-aggregate the 1.01 then we must fail as we
 * won't be able to tell what the new aggregated value's dscale should be.
 * We don't want to return 2.00 (dscale = 2), since the sum's dscale would
 * have been zero if we'd really aggregated only 2.
 *
 * Note: alternatively, we could count the number of inputs with each possible
 * dscale (up to some sane limit).  Not yet clear if it's worth the trouble.
 */
static bool
do_numeric_discard(NumericAggState *state, Numeric newval)
{
	NumericVar	X;
	NumericVar	X2;
	MemoryContext old_context;

	/* Count NaN/infinity inputs separately from all else */
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

	/* load processed number in short-lived context */
	init_var_from_num(newval, &X);

	/*
	 * state->sumX's dscale is the maximum dscale of any of the inputs.
	 * Removing the last input with that dscale would require us to recompute
	 * the maximum dscale of the *remaining* inputs, which we cannot do unless
	 * no more non-NaN inputs remain at all.  So we report a failure instead,
	 * and force the aggregation to be redone from scratch.
	 */
	if (X.dscale == state->maxScale)
	{
		if (state->maxScaleCount > 1 || state->maxScale == 0)
		{
			/*
			 * Some remaining inputs have same dscale, or dscale hasn't gotten
			 * above zero anyway
			 */
			state->maxScaleCount--;
		}
		else if (state->N == 1)
		{
			/* No remaining non-NaN inputs at all, so reset maxScale */
			state->maxScale = 0;
			state->maxScaleCount = 0;
		}
		else
		{
			/* Correct new maxScale is uncertain, must fail */
			return false;
		}
	}

	/* if we need X^2, calculate that in short-lived context */
	if (state->calcSumX2)
	{
		init_var(&X2);
		mul_var(&X, &X, &X2, X.dscale * 2);
	}

	/* The rest of this needs to work in the aggregate context */
	old_context = MemoryContextSwitchTo(state->agg_context);

	if (state->N-- > 1)
	{
		/* Negate X, to subtract it from the sum */
		X.sign = (X.sign == NUMERIC_POS ? NUMERIC_NEG : NUMERIC_POS);
		accum_sum_add(&(state->sumX), &X);

		if (state->calcSumX2)
		{
			/* Negate X^2. X^2 is always positive */
			X2.sign = NUMERIC_NEG;
			accum_sum_add(&(state->sumX2), &X2);
		}
	}
	else
	{
		/* Zero the sums */
		Assert(state->N == 0);

		accum_sum_reset(&state->sumX);
		if (state->calcSumX2)
			accum_sum_reset(&state->sumX2);
	}

	MemoryContextSwitchTo(old_context);

	return true;
}

/*
 * Generic transition function for numeric aggregates that require sumX2.
 */
Datum
numeric_accum(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* Create the state data on the first call */
	if (state == NULL)
		state = makeNumericAggState(fcinfo, true);

	if (!PG_ARGISNULL(1))
		do_numeric_accum(state, PG_GETARG_NUMERIC(1));

	PG_RETURN_POINTER(state);
}

/*
 * Generic combine function for numeric aggregates which require sumX2
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

	/* manually copy all fields from state2 to state1 */
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
		 * These are currently only needed for moving aggregates, but let's do
		 * the right thing anyway...
		 */
		if (state2->maxScale > state1->maxScale)
		{
			state1->maxScale = state2->maxScale;
			state1->maxScaleCount = state2->maxScaleCount;
		}
		else if (state2->maxScale == state1->maxScale)
			state1->maxScaleCount += state2->maxScaleCount;

		/* The rest of this needs to work in the aggregate context */
		old_context = MemoryContextSwitchTo(agg_context);

		/* Accumulate sums */
		accum_sum_combine(&state1->sumX, &state2->sumX);
		accum_sum_combine(&state1->sumX2, &state2->sumX2);

		MemoryContextSwitchTo(old_context);
	}
	PG_RETURN_POINTER(state1);
}

/*
 * Generic transition function for numeric aggregates that don't require sumX2.
 */
Datum
numeric_avg_accum(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* Create the state data on the first call */
	if (state == NULL)
		state = makeNumericAggState(fcinfo, false);

	if (!PG_ARGISNULL(1))
		do_numeric_accum(state, PG_GETARG_NUMERIC(1));

	PG_RETURN_POINTER(state);
}

/*
 * Combine function for numeric aggregates which don't require sumX2
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

	/* manually copy all fields from state2 to state1 */
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
		 * These are currently only needed for moving aggregates, but let's do
		 * the right thing anyway...
		 */
		if (state2->maxScale > state1->maxScale)
		{
			state1->maxScale = state2->maxScale;
			state1->maxScaleCount = state2->maxScaleCount;
		}
		else if (state2->maxScale == state1->maxScale)
			state1->maxScaleCount += state2->maxScaleCount;

		/* The rest of this needs to work in the aggregate context */
		old_context = MemoryContextSwitchTo(agg_context);

		/* Accumulate sums */
		accum_sum_combine(&state1->sumX, &state2->sumX);

		MemoryContextSwitchTo(old_context);
	}
	PG_RETURN_POINTER(state1);
}

/*
 * numeric_avg_serialize
 *		Serialize NumericAggState for numeric aggregates that don't require
 *		sumX2.
 */
Datum
numeric_avg_serialize(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* Ensure we disallow calling when not in aggregate context */
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
 *		Deserialize bytea into NumericAggState for numeric aggregates that
 *		don't require sumX2.
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
	 * Copy the bytea into a StringInfo so that we can "receive" it using the
	 * standard recv-function infrastructure.
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
 *		Serialization function for NumericAggState for numeric aggregates that
 *		require sumX2.
 */
Datum
numeric_serialize(PG_FUNCTION_ARGS)
{
	NumericAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* Ensure we disallow calling when not in aggregate context */
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
 *		Deserialization function for NumericAggState for numeric aggregates that
 *		require sumX2.
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
	 * Copy the bytea into a StringInfo so that we can "receive" it using the
	 * standard recv-function infrastructure.
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
 * Generic inverse transition function for numeric aggregates
 * (with or without requirement for X^2).
 */
Datum
numeric_accum_inv(PG_FUNCTION_ARGS)
{
	NumericAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (NumericAggState *) PG_GETARG_POINTER(0);

	/* Should not get here with no state */
	if (state == NULL)
		elog(ERROR, "numeric_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
		/* If we fail to perform the inverse transition, return NULL */
		if (!do_numeric_discard(state, PG_GETARG_NUMERIC(1)))
			PG_RETURN_NULL();
	}

	PG_RETURN_POINTER(state);
}


/*
 * Integer data types in general use Numeric accumulators to share code
 * and avoid risk of overflow.
 *
 * However for performance reasons optimized special-purpose accumulator
 * routines are used when possible.
 *
 * On platforms with 128-bit integer support, the 128-bit routines will be
 * used when sum(X) or sum(X*X) fit into 128-bit.
 *
 * For 16 and 32 bit inputs, the N and sum(X) fit into 64-bit so the 64-bit
 * accumulators will be used for SUM and AVG of these data types.
 */

#ifdef HAVE_INT128
typedef struct Int128AggState
{
	bool		calcSumX2;		/* if true, calculate sumX2 */
	int64		N;				/* count of processed numbers */
	int128		sumX;			/* sum of processed numbers */
	int128		sumX2;			/* sum of squares of processed numbers */
} Int128AggState;

/*
 * Prepare state data for a 128-bit aggregate function that needs to compute
 * sum, count and optionally sum of squares of the input.
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
 * Like makeInt128AggState(), but allocate the state in the current memory
 * context.
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
 * Accumulate a new input value for 128-bit aggregate functions.
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
 * Remove an input value from the aggregated state.
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

	/* Create the state data on the first call */
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

	/* Create the state data on the first call */
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

	/* Create the state data on the first call */
	if (state == NULL)
		state = makeNumericAggState(fcinfo, true);

	if (!PG_ARGISNULL(1))
		do_numeric_accum(state, int64_to_numeric(PG_GETARG_INT64(1)));

	PG_RETURN_POINTER(state);
}

/*
 * Combine function for numeric aggregates which require sumX2
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

	/* manually copy all fields from state2 to state1 */
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
		/* The rest of this needs to work in the aggregate context */
		old_context = MemoryContextSwitchTo(agg_context);

		/* Accumulate sums */
		accum_sum_combine(&state1->sumX, &state2->sumX);
		accum_sum_combine(&state1->sumX2, &state2->sumX2);

		MemoryContextSwitchTo(old_context);
#endif

	}
	PG_RETURN_POINTER(state1);
}

/*
 * numeric_poly_serialize
 *		Serialize PolyNumAggState into bytea for aggregate functions which
 *		require sumX2.
 */
Datum
numeric_poly_serialize(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* Ensure we disallow calling when not in aggregate context */
	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (PolyNumAggState *) PG_GETARG_POINTER(0);

	/*
	 * If the platform supports int128 then sumX and sumX2 will be a 128 bit
	 * integer type. Here we'll convert that into a numeric type so that the
	 * combine state is in the same format for both int128 enabled machines
	 * and machines which don't support that type. The logic here is that one
	 * day we might like to send these over to another server for further
	 * processing and we want a standard format to work with.
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
 *		Deserialize PolyNumAggState from bytea for aggregate functions which
 *		require sumX2.
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
	 * Copy the bytea into a StringInfo so that we can "receive" it using the
	 * standard recv-function infrastructure.
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
 * Transition function for int8 input when we don't need sumX2.
 */
Datum
int8_avg_accum(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* Create the state data on the first call */
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
 * Combine function for PolyNumAggState for aggregates which don't require
 * sumX2
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

	/* manually copy all fields from state2 to state1 */
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
		/* The rest of this needs to work in the aggregate context */
		old_context = MemoryContextSwitchTo(agg_context);

		/* Accumulate sums */
		accum_sum_combine(&state1->sumX, &state2->sumX);

		MemoryContextSwitchTo(old_context);
#endif

	}
	PG_RETURN_POINTER(state1);
}

/*
 * int8_avg_serialize
 *		Serialize PolyNumAggState into bytea using the standard
 *		recv-function infrastructure.
 */
Datum
int8_avg_serialize(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;
	StringInfoData buf;
	bytea	   *result;
	NumericVar	tmp_var;

	/* Ensure we disallow calling when not in aggregate context */
	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (PolyNumAggState *) PG_GETARG_POINTER(0);

	/*
	 * If the platform supports int128 then sumX will be a 128 integer type.
	 * Here we'll convert that into a numeric type so that the combine state
	 * is in the same format for both int128 enabled machines and machines
	 * which don't support that type. The logic here is that one day we might
	 * like to send these over to another server for further processing and we
	 * want a standard format to work with.
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
 *		Deserialize bytea back into PolyNumAggState.
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
	 * Copy the bytea into a StringInfo so that we can "receive" it using the
	 * standard recv-function infrastructure.
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
 * Inverse transition functions to go with the above.
 */

Datum
int2_accum_inv(PG_FUNCTION_ARGS)
{
	PolyNumAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (PolyNumAggState *) PG_GETARG_POINTER(0);

	/* Should not get here with no state */
	if (state == NULL)
		elog(ERROR, "int2_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_discard(state, (int128) PG_GETARG_INT16(1));
#else
		/* Should never fail, all inputs have dscale 0 */
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

	/* Should not get here with no state */
	if (state == NULL)
		elog(ERROR, "int4_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_discard(state, (int128) PG_GETARG_INT32(1));
#else
		/* Should never fail, all inputs have dscale 0 */
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

	/* Should not get here with no state */
	if (state == NULL)
		elog(ERROR, "int8_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
		/* Should never fail, all inputs have dscale 0 */
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

	/* Should not get here with no state */
	if (state == NULL)
		elog(ERROR, "int8_avg_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
	{
#ifdef HAVE_INT128
		do_int128_discard(state, (int128) PG_GETARG_INT64(1));
#else
		/* Should never fail, all inputs have dscale 0 */
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

	/* If there were no non-null inputs, return NULL */
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

	/* If there were no non-null inputs, return NULL */
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

	/* If there were no non-null inputs, return NULL */
	if (state == NULL || NA_TOTAL_COUNT(state) == 0)
		PG_RETURN_NULL();

	if (state->NaNcount > 0)	/* there was at least one NaN input */
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/* adding plus and minus infinities gives NaN */
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

	/* If there were no non-null inputs, return NULL */
	if (state == NULL || NA_TOTAL_COUNT(state) == 0)
		PG_RETURN_NULL();

	if (state->NaNcount > 0)	/* there was at least one NaN input */
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/* adding plus and minus infinities gives NaN */
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
 * Workhorse routine for the standard deviance and variance
 * aggregates. 'state' is aggregate's transition state.
 * 'variance' specifies whether we should calculate the
 * variance or the standard deviation. 'sample' indicates whether the
 * caller is interested in the sample or the population
 * variance/stddev.
 *
 * If appropriate variance statistic is undefined for the input,
 * *is_null is set to true and NULL is returned.
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
	 * Sample stddev and variance are undefined when N <= 1; population stddev
	 * is undefined when N == 0.  Return NULL in either case (note that NaNs
	 * and infinities count as normal inputs for this purpose).
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
	 * Deal with NaN and infinity cases.  By analogy to the behavior of the
	 * float8 functions, any infinity input produces NaN output.
	 */
	if (state->NaNcount > 0 || state->pInfcount > 0 || state->nInfcount > 0)
		return make_result(&const_nan);

	/* OK, normal calculation applies */
	init_var(&vN);
	init_var(&vsumX);
	init_var(&vsumX2);

	int64_to_numericvar(state->N, &vN);
	accum_sum_final(&(state->sumX), &vsumX);
	accum_sum_final(&(state->sumX2), &vsumX2);

	init_var(&vNminus1);
	sub_var(&vN, &const_one, &vNminus1);

	/* compute rscale for mul_var calls */
	rscale = vsumX.dscale * 2;

	mul_var(&vsumX, &vsumX, &vsumX, rscale);	/* vsumX = sumX * sumX */
	mul_var(&vN, &vsumX2, &vsumX2, rscale); /* vsumX2 = N * sumX2 */
	sub_var(&vsumX2, &vsumX, &vsumX2);	/* N * sumX2 - sumX * sumX */

	if (cmp_var(&vsumX2, &const_zero) <= 0)
	{
		/* Watch out for roundoff error producing a negative numerator */
		res = make_result(&const_zero);
	}
	else
	{
		if (sample)
			mul_var(&vN, &vNminus1, &vNminus1, 0);	/* N * (N - 1) */
		else
			mul_var(&vN, &vN, &vNminus1, 0);	/* N * N */
		rscale = select_div_scale(&vsumX2, &vNminus1);
		div_var(&vsumX2, &vNminus1, &vsumX, rscale, true);	/* variance */
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

	/* Initialize an empty agg state */
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
 * SUM transition functions for integer datatypes.
 *
 * To avoid overflow, we use accumulators wider than the input datatype.
 * A Numeric accumulator is needed for int8 input; for int4 and int2
 * inputs, we use int8 accumulators which should be sufficient for practical
 * purposes.  (The latter two therefore don't really belong in this file,
 * but we keep them here anyway.)
 *
 * Because SQL defines the SUM() of no values to be NULL, not zero,
 * the initial condition of the transition data value needs to be NULL. This
 * means we can't rely on ExecAgg to automatically insert the first non-null
 * data value into the transition data: it doesn't know how to do the type
 * conversion.  The upshot is that these routines have to be marked non-strict
 * and handle substitution of the first non-null input themselves.
 *
 * Note: these functions are used only in plain aggregation mode.
 * In moving-aggregate mode, we use intX_avg_accum and intX_avg_accum_inv.
 */

Datum
int2_sum(PG_FUNCTION_ARGS)
{
	int64		newval;

	if (PG_ARGISNULL(0))
	{
		/* No non-null input seen so far... */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* still no non-null */
		/* This is the first non-null input. */
		newval = (int64) PG_GETARG_INT16(1);
		PG_RETURN_INT64(newval);
	}

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to avoid palloc overhead. If not, we need to return
	 * the new value of the transition variable. (If int8 is pass-by-value,
	 * then of course this is useless as well as incorrect, so just ifdef it
	 * out.)
	 */
#ifndef USE_FLOAT8_BYVAL		/* controls int8 too */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *oldsum = (int64 *) PG_GETARG_POINTER(0);

		/* Leave the running sum unchanged in the new input is null */
		if (!PG_ARGISNULL(1))
			*oldsum = *oldsum + (int64) PG_GETARG_INT16(1);

		PG_RETURN_POINTER(oldsum);
	}
	else
#endif
	{
		int64		oldsum = PG_GETARG_INT64(0);

		/* Leave sum unchanged if new input is null. */
		if (PG_ARGISNULL(1))
			PG_RETURN_INT64(oldsum);

		/* OK to do the addition. */
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
		/* No non-null input seen so far... */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* still no non-null */
		/* This is the first non-null input. */
		newval = (int64) PG_GETARG_INT32(1);
		PG_RETURN_INT64(newval);
	}

	/*
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to avoid palloc overhead. If not, we need to return
	 * the new value of the transition variable. (If int8 is pass-by-value,
	 * then of course this is useless as well as incorrect, so just ifdef it
	 * out.)
	 */
#ifndef USE_FLOAT8_BYVAL		/* controls int8 too */
	if (AggCheckCallContext(fcinfo, NULL))
	{
		int64	   *oldsum = (int64 *) PG_GETARG_POINTER(0);

		/* Leave the running sum unchanged in the new input is null */
		if (!PG_ARGISNULL(1))
			*oldsum = *oldsum + (int64) PG_GETARG_INT32(1);

		PG_RETURN_POINTER(oldsum);
	}
	else
#endif
	{
		int64		oldsum = PG_GETARG_INT64(0);

		/* Leave sum unchanged if new input is null. */
		if (PG_ARGISNULL(1))
			PG_RETURN_INT64(oldsum);

		/* OK to do the addition. */
		newval = oldsum + (int64) PG_GETARG_INT32(1);

		PG_RETURN_INT64(newval);
	}
}

/*
 * Note: this function is obsolete, it's no longer used for SUM(int8).
 */
Datum
int8_sum(PG_FUNCTION_ARGS)
{
	Numeric		oldsum;

	if (PG_ARGISNULL(0))
	{
		/* No non-null input seen so far... */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* still no non-null */
		/* This is the first non-null input. */
		PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT64(1)));
	}

	/*
	 * Note that we cannot special-case the aggregate case here, as we do for
	 * int2_sum and int4_sum: numeric is of variable size, so we cannot modify
	 * our first parameter in-place.
	 */

	oldsum = PG_GETARG_NUMERIC(0);

	/* Leave sum unchanged if new input is null. */
	if (PG_ARGISNULL(1))
		PG_RETURN_NUMERIC(oldsum);

	/* OK to do the addition. */
	PG_RETURN_DATUM(DirectFunctionCall2(numeric_add,
										NumericGetDatum(oldsum),
										NumericGetDatum(int64_to_numeric(PG_GETARG_INT64(1)))));
}


/*
 * Routines for avg(int2) and avg(int4).  The transition datatype
 * is a two-element int8 array, holding count and sum.
 *
 * These functions are also used for sum(int2) and sum(int4) when
 * operating in moving-aggregate mode, since for correct inverse transitions
 * we need to count the inputs.
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
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we need to make
	 * a copy of it before scribbling on it.
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
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we need to make
	 * a copy of it before scribbling on it.
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
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we need to make
	 * a copy of it before scribbling on it.
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
	 * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we need to make
	 * a copy of it before scribbling on it.
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

	/* SQL defines AVG of no values to be NULL */
	if (transdata->count == 0)
		PG_RETURN_NULL();

	countd = NumericGetDatum(int64_to_numeric(transdata->count));
	sumd = NumericGetDatum(int64_to_numeric(transdata->sum));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sumd, countd));
}

/*
 * SUM(int2) and SUM(int4) both return int8, so we can use this
 * final function for both.
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

	/* SQL defines SUM of no values to be NULL */
	if (transdata->count == 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(Int64GetDatumFast(transdata->sum));
}


/* ----------------------------------------------------------------------
 *
 * Debug support
 *
 * ----------------------------------------------------------------------
 */

#ifdef NUMERIC_DEBUG

/*
 * dump_numeric() - Dump a value in the db storage format for debugging
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
 * dump_var() - Dump a value in the variable format for debugging
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
 * Local functions follow
 *
 * In general, these do not support "special" (NaN or infinity) inputs;
 * callers should handle those possibilities first.
 * (There are one or two exceptions, noted in their header comments.)
 *
 * ----------------------------------------------------------------------
 */


/*
 * alloc_var() -
 *
 *	Allocate a digit buffer of ndigits digits (plus a spare digit for rounding)
 */
static void
alloc_var(NumericVar *var, int ndigits)
{
	digitbuf_free(var->buf);
	var->buf = digitbuf_alloc(ndigits + 1);
	var->buf[0] = 0;			/* spare digit for rounding */
	var->digits = var->buf + 1;
	var->ndigits = ndigits;
}


/*
 * free_var() -
 *
 *	Return the digit buffer of a variable to the free pool
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
 *	Set a variable to ZERO.
 *	Note: its dscale is not touched.
 */
static void
zero_var(NumericVar *var)
{
	digitbuf_free(var->buf);
	var->buf = NULL;
	var->digits = NULL;
	var->ndigits = 0;
	var->weight = 0;			/* by convention; doesn't really matter */
	var->sign = NUMERIC_POS;	/* anything but NAN... */
}


/*
 * set_var_from_str()
 *
 *	Parse a string and put the number into a variable
 *
 * This function does not handle leading or trailing spaces.  It returns
 * the end+1 position parsed into *endptr, so that caller can check for
 * trailing spaces/garbage if deemed necessary.
 *
 * cp is the place to actually start parsing; str is what to use in error
 * reports.  (Typically cp would be the same except advanced over spaces.)
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
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
	 * We first parse the string to extract decimal digits and determine the
	 * correct decimal weight.  Then convert to NBASE representation.
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

	/* leading padding for digit alignment later */
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
			/* decimal point must not be followed by underscore */
			if (*cp == '_')
				goto invalid_syntax;
		}
		else if (*cp == '_')
		{
			/* underscore must be followed by more digits */
			cp++;
			if (!isdigit((unsigned char) *cp))
				goto invalid_syntax;
		}
		else
			break;
	}

	ddigits = i - DEC_DIGITS;
	/* trailing padding for digit alignment later */
	memset(decdigits + i, 0, DEC_DIGITS - 1);

	/* Handle exponent, if any */
	if (*cp == 'e' || *cp == 'E')
	{
		int64		exponent = 0;
		bool		neg = false;

		/*
		 * At this point, dweight and dscale can't be more than about
		 * INT_MAX/2 due to the MaxAllocSize limit on string length, so
		 * constraining the exponent similarly should be enough to prevent
		 * integer overflow in this function.  If the value is too large to
		 * fit in storage format, make_result() will complain about it later;
		 * for consistency use the same ereport errcode/text as make_result().
		 */

		/* exponent sign */
		cp++;
		if (*cp == '+')
			cp++;
		else if (*cp == '-')
		{
			neg = true;
			cp++;
		}

		/* exponent digits */
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
				/* underscore must be followed by more digits */
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
	 * Okay, convert pure-decimal representation to base NBASE.  First we need
	 * to determine the converted weight and ndigits.  offset is the number of
	 * decimal zeroes to insert before the first given digit to have a
	 * correctly aligned first NBASE digit.
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

	/* Strip any leading/trailing zeroes, and normalize weight if zero */
	strip_var(dest);

	/* Return end+1 position for caller */
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
 * Return the numeric value of a single hex digit.
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
 *	Parse a string containing a non-decimal integer
 *
 * This function does not handle leading or trailing spaces.  It returns
 * the end+1 position parsed into *endptr, so that caller can check for
 * trailing spaces/garbage if deemed necessary.
 *
 * cp is the place to actually start parsing; str is what to use in error
 * reports.  The number's sign and base prefix indicator (e.g., "0x") are
 * assumed to have already been parsed, so cp should point to the number's
 * first digit in the base specified.
 *
 * base is expected to be 2, 8 or 16.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
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
	 * Process input digits in groups that fit in int64.  Here "tmp" is the
	 * value of the digits in the group, and "mul" is base^n, where n is the
	 * number of digits in the group.  Thus tmp < mul, and we must start a new
	 * group when mul * base threatens to overflow PG_INT64_MAX.
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
					/* Add the contribution from this group of digits */
					int64_to_numericvar(mul, &tmp_var);
					mul_var(dest, &tmp_var, dest, 0);
					int64_to_numericvar(tmp, &tmp_var);
					add_var(dest, &tmp_var, dest);

					/* Result will overflow if weight overflows int16 */
					if (dest->weight > NUMERIC_WEIGHT_MAX)
						goto out_of_range;

					/* Begin a new group */
					tmp = 0;
					mul = 1;
				}

				tmp = tmp * 16 + xdigit_value(*cp++);
				mul = mul * 16;
			}
			else if (*cp == '_')
			{
				/* Underscore must be followed by more digits */
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
					/* Add the contribution from this group of digits */
					int64_to_numericvar(mul, &tmp_var);
					mul_var(dest, &tmp_var, dest, 0);
					int64_to_numericvar(tmp, &tmp_var);
					add_var(dest, &tmp_var, dest);

					/* Result will overflow if weight overflows int16 */
					if (dest->weight > NUMERIC_WEIGHT_MAX)
						goto out_of_range;

					/* Begin a new group */
					tmp = 0;
					mul = 1;
				}

				tmp = tmp * 8 + (*cp++ - '0');
				mul = mul * 8;
			}
			else if (*cp == '_')
			{
				/* Underscore must be followed by more digits */
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
					/* Add the contribution from this group of digits */
					int64_to_numericvar(mul, &tmp_var);
					mul_var(dest, &tmp_var, dest, 0);
					int64_to_numericvar(tmp, &tmp_var);
					add_var(dest, &tmp_var, dest);

					/* Result will overflow if weight overflows int16 */
					if (dest->weight > NUMERIC_WEIGHT_MAX)
						goto out_of_range;

					/* Begin a new group */
					tmp = 0;
					mul = 1;
				}

				tmp = tmp * 2 + (*cp++ - '0');
				mul = mul * 2;
			}
			else if (*cp == '_')
			{
				/* Underscore must be followed by more digits */
				cp++;
				if (*cp < '0' || *cp > '1')
					goto invalid_syntax;
			}
			else
				break;
		}
	}
	else
		/* Should never happen; treat as invalid input */
		goto invalid_syntax;

	/* Check that we got at least one digit */
	if (unlikely(cp == firstdigit))
		goto invalid_syntax;

	/* Add the contribution from the final group of digits */
	int64_to_numericvar(mul, &tmp_var);
	mul_var(dest, &tmp_var, dest, 0);
	int64_to_numericvar(tmp, &tmp_var);
	add_var(dest, &tmp_var, dest);

	if (dest->weight > NUMERIC_WEIGHT_MAX)
		goto out_of_range;

	dest->sign = sign;

	free_var(&tmp_var);

	/* Return end+1 position for caller */
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
 *	Convert the packed db format into a variable
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
 *	Initialize a variable from packed db format. The digits array is not
 *	copied, which saves some cycles when the resulting var is not modified.
 *	Also, there's no need to call free_var(), as long as you don't assign any
 *	other value to it (with set_var_* functions, or by using the var as the
 *	destination of a function like add_var())
 *
 *	CAUTION: Do not modify the digits buffer of a var initialized with this
 *	function, e.g by calling round_var() or trunc_var(), as the changes will
 *	propagate to the original Numeric! It's OK to use it as the destination
 *	argument of one of the calculational functions, though.
 */
static void
init_var_from_num(Numeric num, NumericVar *dest)
{
	dest->ndigits = NUMERIC_NDIGITS(num);
	dest->weight = NUMERIC_WEIGHT(num);
	dest->sign = NUMERIC_SIGN(num);
	dest->dscale = NUMERIC_DSCALE(num);
	dest->digits = NUMERIC_DIGITS(num);
	dest->buf = NULL;			/* digits array is not palloc'd */
}


/*
 * set_var_from_var() -
 *
 *	Copy one variable into another
 */
static void
set_var_from_var(const NumericVar *value, NumericVar *dest)
{
	NumericDigit *newbuf;

	newbuf = digitbuf_alloc(value->ndigits + 1);
	newbuf[0] = 0;				/* spare digit for rounding */
	if (value->ndigits > 0)		/* else value->digits might be null */
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
 *	Convert a var to text representation (guts of numeric_out).
 *	The var is displayed to the number of digits indicated by its dscale.
 *	Returns a palloc'd string.
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
	 * Allocate space for the result.
	 *
	 * i is set to the # of decimal digits before decimal point. dscale is the
	 * # of decimal digits we will print after decimal point. We may generate
	 * as many as DEC_DIGITS-1 excess digits at the end, and in addition we
	 * need room for sign, decimal point, null terminator.
	 */
	i = (var->weight + 1) * DEC_DIGITS;
	if (i <= 0)
		i = 1;

	str = palloc(i + dscale + DEC_DIGITS + 2);
	cp = str;

	/*
	 * Output a dash for negative values
	 */
	if (var->sign == NUMERIC_NEG)
		*cp++ = '-';

	/*
	 * Output all digits before the decimal point
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
			/* In the first digit, suppress extra leading decimal zeroes */
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
	 * If requested, output a decimal point and all the digits that follow it.
	 * We initially put out a multiple of DEC_DIGITS digits, then truncate if
	 * needed.
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
	 * terminate the string and return it
	 */
	*cp = '\0';
	return str;
}

/*
 * get_str_from_var_sci() -
 *
 *	Convert a var to a normalised scientific notation text representation.
 *	This function does the heavy lifting for numeric_out_sci().
 *
 *	This notation has the general form a * 10^b, where a is known as the
 *	"significand" and b is known as the "exponent".
 *
 *	Because we can't do superscript in ASCII (and because we want to copy
 *	printf's behaviour) we display the exponent using E notation, with a
 *	minimum of two exponent digits.
 *
 *	For example, the value 1234 could be output as 1.2e+03.
 *
 *	We assume that the exponent can fit into an int32.
 *
 *	rscale is the number of decimal digits desired after the decimal point in
 *	the output, negative values will be treated as meaning zero.
 *
 *	Returns a palloc'd string.
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
	 * Determine the exponent of this number in normalised form.
	 *
	 * This is the exponent required to represent the number with only one
	 * significant digit before the decimal place.
	 */
	if (var->ndigits > 0)
	{
		exponent = (var->weight + 1) * DEC_DIGITS;

		/*
		 * Compensate for leading decimal zeroes in the first numeric digit by
		 * decrementing the exponent.
		 */
		exponent -= DEC_DIGITS - (int) log10(var->digits[0]);
	}
	else
	{
		/*
		 * If var has no digits, then it must be zero.
		 *
		 * Zero doesn't technically have a meaningful exponent in normalised
		 * notation, but we just display the exponent as zero for consistency
		 * of output.
		 */
		exponent = 0;
	}

	/*
	 * Divide var by 10^exponent to get the significand, rounding to rscale
	 * decimal digits in the process.
	 */
	init_var(&tmp_var);

	power_ten_int(exponent, &tmp_var);
	div_var(var, &tmp_var, &tmp_var, rscale, true);
	sig_out = get_str_from_var(&tmp_var);

	free_var(&tmp_var);

	/*
	 * Allocate space for the result.
	 *
	 * In addition to the significand, we need room for the exponent
	 * decoration ("e"), the sign of the exponent, up to 10 digits for the
	 * exponent itself, and of course the null terminator.
	 */
	len = strlen(sig_out) + 13;
	str = palloc(len);
	snprintf(str, len, "%se%+03d", sig_out, exponent);

	pfree(sig_out);

	return str;
}


/*
 * numericvar_serialize - serialize NumericVar to binary format
 *
 * At variable level, no checks are performed on the weight or dscale, allowing
 * us to pass around intermediate values with higher precision than supported
 * by the numeric type.  Note: this is incompatible with numeric_send/recv(),
 * which use 16-bit integers for these fields.
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
 * numericvar_deserialize - deserialize binary format to NumericVar
 */
static void
numericvar_deserialize(StringInfo buf, NumericVar *var)
{
	int			len,
				i;

	len = pq_getmsgint(buf, sizeof(int32));

	alloc_var(var, len);		/* sets var->ndigits */

	var->weight = pq_getmsgint(buf, sizeof(int32));
	var->sign = pq_getmsgint(buf, sizeof(int32));
	var->dscale = pq_getmsgint(buf, sizeof(int32));
	for (i = 0; i < len; i++)
		var->digits[i] = pq_getmsgint(buf, sizeof(int16));
}


/*
 * duplicate_numeric() - copy a packed-format Numeric
 *
 * This will handle NaN and Infinity cases.
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
 *	Create the packed db numeric format in palloc()'d memory from
 *	a variable.  This will handle NaN and Infinity cases.
 *
 *	If "have_error" isn't NULL, on overflow *have_error is set to true and
 *	NULL is returned.  This is helpful when caller needs to handle errors.
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
		 * Verify valid special value.  This could be just an Assert, perhaps,
		 * but it seems worthwhile to expend a few cycles to ensure that we
		 * never write any nonzero reserved bits to disk.
		 */
		if (!(sign == NUMERIC_NAN ||
			  sign == NUMERIC_PINF ||
			  sign == NUMERIC_NINF))
			elog(ERROR, "invalid numeric sign value 0x%x", sign);

		result = (Numeric) palloc(NUMERIC_HDRSZ_SHORT);

		SET_VARSIZE(result, NUMERIC_HDRSZ_SHORT);
		result->choice.n_header = sign;
		/* the header word is all we need */

		dump_numeric("make_result()", result);
		return result;
	}

	n = var->ndigits;

	/* truncate leading zeroes */
	while (n > 0 && *digits == 0)
	{
		digits++;
		weight--;
		n--;
	}
	/* truncate trailing zeroes */
	while (n > 0 && digits[n - 1] == 0)
		n--;

	/* If zero result, force to weight=0 and positive sign */
	if (n == 0)
	{
		weight = 0;
		sign = NUMERIC_POS;
	}

	/* Build the result */
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

	/* Check for overflow of int16 fields */
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
 *	An interface to make_result_opt_error() without "have_error" argument.
 */
static Numeric
make_result(const NumericVar *var)
{
	return make_result_opt_error(var, NULL);
}


/*
 * apply_typmod() -
 *
 *	Do bounds checking and rounding according to the specified typmod.
 *	Note that this is only applied to normal finite values.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
static bool
apply_typmod(NumericVar *var, int32 typmod, Node *escontext)
{
	int			precision;
	int			scale;
	int			maxdigits;
	int			ddigits;
	int			i;

	/* Do nothing if we have an invalid typmod */
	if (!is_valid_numeric_typmod(typmod))
		return true;

	precision = numeric_typmod_precision(typmod);
	scale = numeric_typmod_scale(typmod);
	maxdigits = precision - scale;

	/* Round to target scale (and set var->dscale) */
	round_var(var, scale);

	/* but don't allow var->dscale to be negative */
	if (var->dscale < 0)
		var->dscale = 0;

	/*
	 * Check for overflow - note we can't do this before rounding, because
	 * rounding could raise the weight.  Also note that the var's weight could
	 * be inflated by leading zeroes, which will be stripped before storage
	 * but perhaps might not have been yet. In any case, we must recognize a
	 * true zero, whose weight doesn't mean anything.
	 */
	ddigits = (var->weight + 1) * DEC_DIGITS;
	if (ddigits > maxdigits)
	{
		/* Determine true weight; and check for all-zero result */
		for (i = 0; i < var->ndigits; i++)
		{
			NumericDigit dig = var->digits[i];

			if (dig)
			{
				/* Adjust for any high-order decimal zero digits */
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
				/* no adjustment */
#else
#error unsupported NBASE
#endif
				if (ddigits > maxdigits)
					ereturn(escontext, false,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("numeric field overflow"),
							 errdetail("A field with precision %d, scale %d must round to an absolute value less than %s%d.",
									   precision, scale,
					/* Display 10^0 as 1 */
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
 *	Do bounds checking according to the specified typmod, for an Inf or NaN.
 *	For convenience of most callers, the value is presented in packed form.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
static bool
apply_typmod_special(Numeric num, int32 typmod, Node *escontext)
{
	int			precision;
	int			scale;

	Assert(NUMERIC_IS_SPECIAL(num));	/* caller error if not */

	/*
	 * NaN is allowed regardless of the typmod; that's rather dubious perhaps,
	 * but it's a longstanding behavior.  Inf is rejected if we have any
	 * typmod restriction, since an infinity shouldn't be claimed to fit in
	 * any finite number of digits.
	 */
	if (NUMERIC_IS_NAN(num))
		return true;

	/* Do nothing if we have a default typmod (-1) */
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
 * Convert numeric to int8, rounding if needed.
 *
 * If overflow, return false (no error is raised).  Return true if okay.
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

	/* Round to nearest integer */
	init_var(&rounded);
	set_var_from_var(var, &rounded);
	round_var(&rounded, 0);

	/* Check for zero input */
	strip_var(&rounded);
	ndigits = rounded.ndigits;
	if (ndigits == 0)
	{
		*result = 0;
		free_var(&rounded);
		return true;
	}

	/*
	 * For input like 10000000000, we must treat stripped digits as real. So
	 * the loop assumes there are weight+1 digits before the decimal point.
	 */
	weight = rounded.weight;
	Assert(weight >= 0 && ndigits <= weight + 1);

	/*
	 * Construct the result. To avoid issues with converting a value
	 * corresponding to INT64_MIN (which can't be represented as a positive 64
	 * bit two's complement integer), accumulate value as a negative number.
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
 * Convert int8 value to numeric.
 */
static void
int64_to_numericvar(int64 val, NumericVar *var)
{
	uint64		uval,
				newuval;
	NumericDigit *ptr;
	int			ndigits;

	/* int64 can require at most 19 decimal digits; add one for safety */
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
 * Convert numeric to uint64, rounding if needed.
 *
 * If overflow, return false (no error is raised).  Return true if okay.
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

	/* Round to nearest integer */
	init_var(&rounded);
	set_var_from_var(var, &rounded);
	round_var(&rounded, 0);

	/* Check for zero input */
	strip_var(&rounded);
	ndigits = rounded.ndigits;
	if (ndigits == 0)
	{
		*result = 0;
		free_var(&rounded);
		return true;
	}

	/* Check for negative input */
	if (rounded.sign == NUMERIC_NEG)
	{
		free_var(&rounded);
		return false;
	}

	/*
	 * For input like 10000000000, we must treat stripped digits as real. So
	 * the loop assumes there are weight+1 digits before the decimal point.
	 */
	weight = rounded.weight;
	Assert(weight >= 0 && ndigits <= weight + 1);

	/* Construct the result */
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
 * Convert numeric to int128, rounding if needed.
 *
 * If overflow, return false (no error is raised).  Return true if okay.
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

	/* Round to nearest integer */
	init_var(&rounded);
	set_var_from_var(var, &rounded);
	round_var(&rounded, 0);

	/* Check for zero input */
	strip_var(&rounded);
	ndigits = rounded.ndigits;
	if (ndigits == 0)
	{
		*result = 0;
		free_var(&rounded);
		return true;
	}

	/*
	 * For input like 10000000000, we must treat stripped digits as real. So
	 * the loop assumes there are weight+1 digits before the decimal point.
	 */
	weight = rounded.weight;
	Assert(weight >= 0 && ndigits <= weight + 1);

	/* Construct the result */
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
		 * The overflow check is a bit tricky because we want to accept
		 * INT128_MIN, which will overflow the positive accumulator.  We can
		 * detect this case easily though because INT128_MIN is the only
		 * nonzero value for which -val == val (on a two's complement machine,
		 * anyway).
		 */
		if ((val / NBASE) != oldval)	/* possible overflow? */
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
 * Convert 128 bit integer to numeric.
 */
static void
int128_to_numericvar(int128 val, NumericVar *var)
{
	uint128		uval,
				newuval;
	NumericDigit *ptr;
	int			ndigits;

	/* int128 can require at most 39 decimal digits; add one for safety */
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
 * Convert a NumericVar to float8; if out of range, return +/- HUGE_VAL
 */
static double
numericvar_to_double_no_overflow(const NumericVar *var)
{
	char	   *tmp;
	double		val;
	char	   *endptr;

	tmp = get_str_from_var(var);

	/* unlike float8in, we ignore ERANGE from strtod */
	val = strtod(tmp, &endptr);
	if (*endptr != '\0')
	{
		/* shouldn't happen ... */
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
 *	Compare two values on variable level.  We assume zeroes have been
 *	truncated to no digits.
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
 *	Main routine of cmp_var(). This function can be used by both
 *	NumericVar and Numeric.
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
 *	Full version of add functionality on variable level (handling signs).
 *	result might point to one of the operands too without danger.
 */
static void
add_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	/*
	 * Decide on the signs of the two variables what to do
	 */
	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_POS)
		{
			/*
			 * Both are positive result = +(ABS(var1) + ABS(var2))
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_POS;
		}
		else
		{
			/*
			 * var1 is positive, var2 is negative Must compare absolute values
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
			 * var1 is negative, var2 is positive
			 * Must compare absolute values
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
			 * Both are negative
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
 *	Full version of sub functionality on variable level (handling signs).
 *	result might point to one of the operands too without danger.
 */
static void
sub_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	/*
	 * Decide on the signs of the two variables what to do
	 */
	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_NEG)
		{
			/* ----------
			 * var1 is positive, var2 is negative
			 * result = +(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_POS;
		}
		else
		{
			/* ----------
			 * Both are positive
			 * Must compare absolute values
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
			 * Both are negative
			 * Must compare absolute values
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
			 * var1 is negative, var2 is positive
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
 *	Multiplication on variable level. Product of var1 * var2 is stored
 *	in result.  Result is rounded to no more than rscale fractional digits.
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
	 * Arrange for var1 to be the shorter of the two numbers.  This improves
	 * performance because the inner multiplication loop is much simpler than
	 * the outer loop, so it's better to have a smaller number of iterations
	 * of the outer loop.  This also reduces the number of times that the
	 * accumulator array needs to be normalized.
	 */
	if (var1->ndigits > var2->ndigits)
	{
		const NumericVar *tmp = var1;

		var1 = var2;
		var2 = tmp;
	}

	/* copy these values into local vars for speed in inner loop */
	var1ndigits = var1->ndigits;
	var2ndigits = var2->ndigits;
	var1digits = var1->digits;
	var2digits = var2->digits;

	if (var1ndigits == 0 || var2ndigits == 0)
	{
		/* one or both inputs is zero; so is result */
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/* Determine result sign and (maximum possible) weight */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight + var2->weight + 2;

	/*
	 * Determine the number of result digits to compute.  If the exact result
	 * would have more than rscale fractional digits, truncate the computation
	 * with MUL_GUARD_DIGITS guard digits, i.e., ignore input digits that
	 * would only contribute to the right of that.  (This will give the exact
	 * rounded-to-rscale answer unless carries out of the ignored positions
	 * would have propagated through more than MUL_GUARD_DIGITS digits.)
	 *
	 * Note: an exact computation could not produce more than var1ndigits +
	 * var2ndigits digits, but we allocate one extra output digit in case
	 * rscale-driven rounding produces a carry out of the highest exact digit.
	 */
	res_ndigits = var1ndigits + var2ndigits + 1;
	maxdigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS +
		MUL_GUARD_DIGITS;
	res_ndigits = Min(res_ndigits, maxdigits);

	if (res_ndigits < 3)
	{
		/* All input digits will be ignored; so result is zero */
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * We do the arithmetic in an array "dig[]" of signed int's.  Since
	 * INT_MAX is noticeably larger than NBASE*NBASE, this gives us headroom
	 * to avoid normalizing carries immediately.
	 *
	 * maxdig tracks the maximum possible value of any dig[] entry; when this
	 * threatens to exceed INT_MAX, we take the time to propagate carries.
	 * Furthermore, we need to ensure that overflow doesn't occur during the
	 * carry propagation passes either.  The carry values could be as much as
	 * INT_MAX/NBASE, so really we must normalize when digits threaten to
	 * exceed INT_MAX - INT_MAX/NBASE.
	 *
	 * To avoid overflow in maxdig itself, it actually represents the max
	 * possible value divided by NBASE-1, ie, at the top of the loop it is
	 * known that no dig[] entry exceeds maxdig * (NBASE-1).
	 */
	dig = (int *) palloc0(res_ndigits * sizeof(int));
	maxdig = 0;

	/*
	 * The least significant digits of var1 should be ignored if they don't
	 * contribute directly to the first res_ndigits digits of the result that
	 * we are computing.
	 *
	 * Digit i1 of var1 and digit i2 of var2 are multiplied and added to digit
	 * i1+i2+2 of the accumulator array, so we need only consider digits of
	 * var1 for which i1 <= res_ndigits - 3.
	 */
	for (i1 = Min(var1ndigits - 1, res_ndigits - 3); i1 >= 0; i1--)
	{
		NumericDigit var1digit = var1digits[i1];

		if (var1digit == 0)
			continue;

		/* Time to normalize? */
		maxdig += var1digit;
		if (maxdig > (INT_MAX - INT_MAX / NBASE) / (NBASE - 1))
		{
			/* Yes, do it */
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
			/* Reset maxdig to indicate new worst-case */
			maxdig = 1 + var1digit;
		}

		/*
		 * Add the appropriate multiple of var2 into the accumulator.
		 *
		 * As above, digits of var2 can be ignored if they don't contribute,
		 * so we only include digits for which i1+i2+2 < res_ndigits.
		 *
		 * This inner loop is the performance bottleneck for multiplication,
		 * so we want to keep it simple enough so that it can be
		 * auto-vectorized.  Accordingly, process the digits left-to-right
		 * even though schoolbook multiplication would suggest right-to-left.
		 * Since we aren't propagating carries in this loop, the order does
		 * not matter.
		 */
		{
			int			i2limit = Min(var2ndigits, res_ndigits - i1 - 2);
			int		   *dig_i1_2 = &dig[i1 + 2];

			for (i2 = 0; i2 < i2limit; i2++)
				dig_i1_2[i2] += var1digit * var2digits[i2];
		}
	}

	/*
	 * Now we do a final carry propagation pass to normalize the result, which
	 * we combine with storing the result digits into the output. Note that
	 * this is still done at full precision w/guard digits.
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
	 * Finally, round the result to the requested precision.
	 */
	result->weight = res_weight;
	result->sign = res_sign;

	/* Round to target rscale (and set result->dscale) */
	round_var(result, rscale);

	/* Strip leading and trailing zeroes */
	strip_var(result);
}


/*
 * div_var() -
 *
 *	Division on variable level. Quotient of var1 / var2 is stored in result.
 *	The quotient is figured to exactly rscale fractional digits.
 *	If round is true, it is rounded at the rscale'th digit; if false, it
 *	is truncated (towards zero) at that digit.
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

	/* copy these values into local vars for speed in inner loop */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;

	/*
	 * First of all division by zero check; we must not be handed an
	 * unnormalized divisor.
	 */
	if (var2ndigits == 0 || var2->digits[0] == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	/*
	 * If the divisor has just one or two digits, delegate to div_var_int(),
	 * which uses fast short division.
	 *
	 * Similarly, on platforms with 128-bit integer support, delegate to
	 * div_var_int64() for divisors with three or four digits.
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
	 * Otherwise, perform full long division.
	 */

	/* Result zero check */
	if (var1ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * Determine the result sign, weight and number of digits to calculate.
	 * The weight figured here is correct if the emitted quotient has no
	 * leading zero digits; otherwise strip_var() will fix things up.
	 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight - var2->weight;
	/* The number of accurate result digits we need to produce: */
	res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* ... but always at least 1 */
	res_ndigits = Max(res_ndigits, 1);
	/* If rounding needed, figure one more digit to ensure correct result */
	if (round)
		res_ndigits++;

	/*
	 * The working dividend normally requires res_ndigits + var2ndigits
	 * digits, but make it at least var1ndigits so we can load all of var1
	 * into it.  (There will be an additional digit dividend[0] in the
	 * dividend space, but for consistency with Knuth's notation we don't
	 * count that in div_ndigits.)
	 */
	div_ndigits = res_ndigits + var2ndigits;
	div_ndigits = Max(div_ndigits, var1ndigits);

	/*
	 * We need a workspace with room for the working dividend (div_ndigits+1
	 * digits) plus room for the possibly-normalized divisor (var2ndigits
	 * digits).  It is convenient also to have a zero at divisor[0] with the
	 * actual divisor data in divisor[1 .. var2ndigits].  Transferring the
	 * digits into the workspace also allows us to realloc the result (which
	 * might be the same as either input var) before we begin the main loop.
	 * Note that we use palloc0 to ensure that divisor[0], dividend[0], and
	 * any additional dividend positions beyond var1ndigits, start out 0.
	 */
	dividend = (NumericDigit *)
		palloc0((div_ndigits + var2ndigits + 2) * sizeof(NumericDigit));
	divisor = dividend + (div_ndigits + 1);
	memcpy(dividend + 1, var1->digits, var1ndigits * sizeof(NumericDigit));
	memcpy(divisor + 1, var2->digits, var2ndigits * sizeof(NumericDigit));

	/*
	 * Now we can realloc the result to hold the generated quotient digits.
	 */
	alloc_var(result, res_ndigits);
	res_digits = result->digits;

	/*
	 * The full multiple-place algorithm is taken from Knuth volume 2,
	 * Algorithm 4.3.1D.
	 *
	 * We need the first divisor digit to be >= NBASE/2.  If it isn't, make it
	 * so by scaling up both the divisor and dividend by the factor "d".  (The
	 * reason for allocating dividend[0] above is to leave room for possible
	 * carry here.)
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
		/* at this point only var1ndigits of dividend can be nonzero */
		for (i = var1ndigits; i >= 0; i--)
		{
			carry += dividend[i] * d;
			dividend[i] = carry % NBASE;
			carry = carry / NBASE;
		}
		Assert(carry == 0);
		Assert(divisor[1] >= HALF_NBASE);
	}
	/* First 2 divisor digits are used repeatedly in main loop */
	divisor1 = divisor[1];
	divisor2 = divisor[2];

	/*
	 * Begin the main loop.  Each iteration of this loop produces the j'th
	 * quotient digit by dividing dividend[j .. j + var2ndigits] by the
	 * divisor; this is essentially the same as the common manual procedure
	 * for long division.
	 */
	for (j = 0; j < res_ndigits; j++)
	{
		/* Estimate quotient digit from the first two dividend digits */
		int			next2digits = dividend[j] * NBASE + dividend[j + 1];
		int			qhat;

		/*
		 * If next2digits are 0, then quotient digit must be 0 and there's no
		 * need to adjust the working dividend.  It's worth testing here to
		 * fall out ASAP when processing trailing zeroes in a dividend.
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
		 * Adjust quotient digit if it's too large.  Knuth proves that after
		 * this step, the quotient digit will be either correct or just one
		 * too large.  (Note: it's OK to use dividend[j+2] here because we
		 * know the divisor length is at least 2.)
		 */
		while (divisor2 * qhat >
			   (next2digits - qhat * divisor1) * NBASE + dividend[j + 2])
			qhat--;

		/* As above, need do nothing more when quotient digit is 0 */
		if (qhat > 0)
		{
			NumericDigit *dividend_j = &dividend[j];

			/*
			 * Multiply the divisor by qhat, and subtract that from the
			 * working dividend.  The multiplication and subtraction are
			 * folded together here, noting that qhat <= NBASE (since it might
			 * be one too large), and so the intermediate result "tmp_result"
			 * is in the range [-NBASE^2, NBASE - 1], and "borrow" is in the
			 * range [0, NBASE].
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
			 * If we got a borrow out of the top dividend digit, then indeed
			 * qhat was one too large.  Fix it, and add back the divisor to
			 * correct the working dividend.  (Knuth proves that this will
			 * occur only about 3/NBASE of the time; hence, it's a good idea
			 * to test this code with small NBASE to be sure this section gets
			 * exercised.)
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
				/* A carry should occur here to cancel the borrow above */
				Assert(carry == 1);
			}
		}

		/* And we're done with this quotient digit */
		res_digits[j] = qhat;
	}

	pfree(dividend);

	/*
	 * Finally, round or truncate the result to the requested precision.
	 */
	result->weight = res_weight;
	result->sign = res_sign;

	/* Round or truncate to target rscale (and set result->dscale) */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* Strip leading and trailing zeroes */
	strip_var(result);
}


/*
 * div_var_fast() -
 *
 *	This has the same API as div_var, but is implemented using the division
 *	algorithm from the "FM" library, rather than Knuth's schoolbook-division
 *	approach.  This is significantly faster but can produce inaccurate
 *	results, because it sometimes has to propagate rounding to the left,
 *	and so we can never be entirely sure that we know the requested digits
 *	exactly.  We compute DIV_GUARD_DIGITS extra digits, but there is
 *	no certainty that that's enough.  We use this only in the transcendental
 *	function calculation routines, where everything is approximate anyway.
 *
 *	Although we provide a "round" argument for consistency with div_var,
 *	it is unwise to use this function with round=false.  In truncation mode
 *	it is possible to get a result with no significant digits, for example
 *	with rscale=0 we might compute 0.99999... and truncate that to 0 when
 *	the correct answer is 1.
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

	/* copy these values into local vars for speed in inner loop */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	/*
	 * First of all division by zero check; we must not be handed an
	 * unnormalized divisor.
	 */
	if (var2ndigits == 0 || var2digits[0] == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	/*
	 * If the divisor has just one or two digits, delegate to div_var_int(),
	 * which uses fast short division.
	 *
	 * Similarly, on platforms with 128-bit integer support, delegate to
	 * div_var_int64() for divisors with three or four digits.
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
	 * Otherwise, perform full long division.
	 */

	/* Result zero check */
	if (var1ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * Determine the result sign, weight and number of digits to calculate
	 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight - var2->weight + 1;
	/* The number of accurate result digits we need to produce: */
	div_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* Add guard digits for roundoff error */
	div_ndigits += DIV_GUARD_DIGITS;
	if (div_ndigits < DIV_GUARD_DIGITS)
		div_ndigits = DIV_GUARD_DIGITS;

	/*
	 * We do the arithmetic in an array "div[]" of signed int's.  Since
	 * INT_MAX is noticeably larger than NBASE*NBASE, this gives us headroom
	 * to avoid normalizing carries immediately.
	 *
	 * We start with div[] containing one zero digit followed by the
	 * dividend's digits (plus appended zeroes to reach the desired precision
	 * including guard digits).  Each step of the main loop computes an
	 * (approximate) quotient digit and stores it into div[], removing one
	 * position of dividend space.  A final pass of carry propagation takes
	 * care of any mistaken quotient digits.
	 *
	 * Note that div[] doesn't necessarily contain all of the digits from the
	 * dividend --- the desired precision plus guard digits might be less than
	 * the dividend's precision.  This happens, for example, in the square
	 * root algorithm, where we typically divide a 2N-digit number by an
	 * N-digit number, and only require a result with N digits of precision.
	 */
	div = (int *) palloc0((div_ndigits + 1) * sizeof(int));
	load_ndigits = Min(div_ndigits, var1ndigits);
	for (i = 0; i < load_ndigits; i++)
		div[i + 1] = var1digits[i];

	/*
	 * We estimate each quotient digit using floating-point arithmetic, taking
	 * the first four digits of the (current) dividend and divisor.  This must
	 * be float to avoid overflow.  The quotient digits will generally be off
	 * by no more than one from the exact answer.
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
	 * maxdiv tracks the maximum possible absolute value of any div[] entry;
	 * when this threatens to exceed INT_MAX, we take the time to propagate
	 * carries.  Furthermore, we need to ensure that overflow doesn't occur
	 * during the carry propagation passes either.  The carry values may have
	 * an absolute value as high as INT_MAX/NBASE + 1, so really we must
	 * normalize when digits threaten to exceed INT_MAX - INT_MAX/NBASE - 1.
	 *
	 * To avoid overflow in maxdiv itself, it represents the max absolute
	 * value divided by NBASE-1, ie, at the top of the loop it is known that
	 * no div[] entry has an absolute value exceeding maxdiv * (NBASE-1).
	 *
	 * Actually, though, that holds good only for div[] entries after div[qi];
	 * the adjustment done at the bottom of the loop may cause div[qi + 1] to
	 * exceed the maxdiv limit, so that div[qi] in the next iteration is
	 * beyond the limit.  This does not cause problems, as explained below.
	 */
	maxdiv = 1;

	/*
	 * Outer loop computes next quotient digit, which will go into div[qi]
	 */
	for (qi = 0; qi < div_ndigits; qi++)
	{
		/* Approximate the current dividend value */
		fdividend = (double) div[qi];
		for (i = 1; i < 4; i++)
		{
			fdividend *= NBASE;
			if (qi + i <= div_ndigits)
				fdividend += (double) div[qi + i];
		}
		/* Compute the (approximate) quotient digit */
		fquotient = fdividend * fdivisorinverse;
		qdigit = (fquotient >= 0.0) ? ((int) fquotient) :
			(((int) fquotient) - 1);	/* truncate towards -infinity */

		if (qdigit != 0)
		{
			/* Do we need to normalize now? */
			maxdiv += abs(qdigit);
			if (maxdiv > (INT_MAX - INT_MAX / NBASE - 1) / (NBASE - 1))
			{
				/*
				 * Yes, do it.  Note that if var2ndigits is much smaller than
				 * div_ndigits, we can save a significant amount of effort
				 * here by noting that we only need to normalise those div[]
				 * entries touched where prior iterations subtracted multiples
				 * of the divisor.
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
				 * All the div[] digits except possibly div[qi] are now in the
				 * range 0..NBASE-1.  We do not need to consider div[qi] in
				 * the maxdiv value anymore, so we can reset maxdiv to 1.
				 */
				maxdiv = 1;

				/*
				 * Recompute the quotient digit since new info may have
				 * propagated into the top four dividend digits
				 */
				fdividend = (double) div[qi];
				for (i = 1; i < 4; i++)
				{
					fdividend *= NBASE;
					if (qi + i <= div_ndigits)
						fdividend += (double) div[qi + i];
				}
				/* Compute the (approximate) quotient digit */
				fquotient = fdividend * fdivisorinverse;
				qdigit = (fquotient >= 0.0) ? ((int) fquotient) :
					(((int) fquotient) - 1);	/* truncate towards -infinity */
				maxdiv += abs(qdigit);
			}

			/*
			 * Subtract off the appropriate multiple of the divisor.
			 *
			 * The digits beyond div[qi] cannot overflow, because we know they
			 * will fall within the maxdiv limit.  As for div[qi] itself, note
			 * that qdigit is approximately trunc(div[qi] / vardigits[0]),
			 * which would make the new value simply div[qi] mod vardigits[0].
			 * The lower-order terms in qdigit can change this result by not
			 * more than about twice INT_MAX/NBASE, so overflow is impossible.
			 *
			 * This inner loop is the performance bottleneck for division, so
			 * code it in the same way as the inner loop of mul_var() so that
			 * it can be auto-vectorized.  We cast qdigit to NumericDigit
			 * before multiplying to allow the compiler to generate more
			 * efficient code (using 16-bit multiplication), which is safe
			 * since we know that the quotient digit is off by at most one, so
			 * there is no overflow risk.
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
		 * The dividend digit we are about to replace might still be nonzero.
		 * Fold it into the next digit position.
		 *
		 * There is no risk of overflow here, although proving that requires
		 * some care.  Much as with the argument for div[qi] not overflowing,
		 * if we consider the first two terms in the numerator and denominator
		 * of qdigit, we can see that the final value of div[qi + 1] will be
		 * approximately a remainder mod (vardigits[0]*NBASE + vardigits[1]).
		 * Accounting for the lower-order terms is a bit complicated but ends
		 * up adding not much more than INT_MAX/NBASE to the possible range.
		 * Thus, div[qi + 1] cannot overflow here, and in its role as div[qi]
		 * in the next loop iteration, it can't be large enough to cause
		 * overflow in the carry propagation step (if any), either.
		 *
		 * But having said that: div[qi] can be more than INT_MAX/NBASE, as
		 * noted above, which means that the product div[qi] * NBASE *can*
		 * overflow.  When that happens, adding it to div[qi + 1] will always
		 * cause a canceling overflow so that the end result is correct.  We
		 * could avoid the intermediate overflow by doing the multiplication
		 * and addition in int64 arithmetic, but so far there appears no need.
		 */
		div[qi + 1] += div[qi] * NBASE;

		div[qi] = qdigit;
	}

	/*
	 * Approximate and store the last quotient digit (div[div_ndigits])
	 */
	fdividend = (double) div[qi];
	for (i = 1; i < 4; i++)
		fdividend *= NBASE;
	fquotient = fdividend * fdivisorinverse;
	qdigit = (fquotient >= 0.0) ? ((int) fquotient) :
		(((int) fquotient) - 1);	/* truncate towards -infinity */
	div[qi] = qdigit;

	/*
	 * Because the quotient digits might be off by one, some of them might be
	 * -1 or NBASE at this point.  The represented value is correct in a
	 * mathematical sense, but it doesn't look right.  We do a final carry
	 * propagation pass to normalize the digits, which we combine with storing
	 * the result digits into the output.  Note that this is still done at
	 * full precision w/guard digits.
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
	 * Finally, round the result to the requested precision.
	 */
	result->weight = res_weight;
	result->sign = res_sign;

	/* Round to target rscale (and set result->dscale) */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* Strip leading and trailing zeroes */
	strip_var(result);
}


/*
 * div_var_int() -
 *
 *	Divide a numeric variable by a 32-bit integer with the specified weight.
 *	The quotient var / (ival * NBASE^ival_weight) is stored in result.
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

	/* Guard against division by zero */
	if (ival == 0)
		ereport(ERROR,
				errcode(ERRCODE_DIVISION_BY_ZERO),
				errmsg("division by zero"));

	/* Result zero check */
	if (var_ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * Determine the result sign, weight and number of digits to calculate.
	 * The weight figured here is correct if the emitted quotient has no
	 * leading zero digits; otherwise strip_var() will fix things up.
	 */
	if (var->sign == NUMERIC_POS)
		res_sign = ival > 0 ? NUMERIC_POS : NUMERIC_NEG;
	else
		res_sign = ival > 0 ? NUMERIC_NEG : NUMERIC_POS;
	res_weight = var->weight - ival_weight;
	/* The number of accurate result digits we need to produce: */
	res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* ... but always at least 1 */
	res_ndigits = Max(res_ndigits, 1);
	/* If rounding needed, figure one more digit to ensure correct result */
	if (round)
		res_ndigits++;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* spare digit for later rounding */
	res_digits = res_buf + 1;

	/*
	 * Now compute the quotient digits.  This is the short division algorithm
	 * described in Knuth volume 2, section 4.3.1 exercise 16, except that we
	 * allow the divisor to exceed the internal base.
	 *
	 * In this algorithm, the carry from one digit to the next is at most
	 * divisor - 1.  Therefore, while processing the next digit, carry may
	 * become as large as divisor * NBASE - 1, and so it requires a 64-bit
	 * integer if this exceeds UINT_MAX.
	 */
	divisor = abs(ival);

	if (divisor <= UINT_MAX / NBASE)
	{
		/* carry cannot overflow 32 bits */
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
		/* carry may exceed 32 bits */
		uint64		carry = 0;

		for (i = 0; i < res_ndigits; i++)
		{
			carry = carry * NBASE + (i < var_ndigits ? var_digits[i] : 0);
			res_digits[i] = (NumericDigit) (carry / divisor);
			carry = carry % divisor;
		}
	}

	/* Store the quotient in result */
	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->sign = res_sign;

	/* Round or truncate to target rscale (and set result->dscale) */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* Strip leading/trailing zeroes */
	strip_var(result);
}


#ifdef HAVE_INT128
/*
 * div_var_int64() -
 *
 *	Divide a numeric variable by a 64-bit integer with the specified weight.
 *	The quotient var / (ival * NBASE^ival_weight) is stored in result.
 *
 *	This duplicates the logic in div_var_int(), so any changes made there
 *	should be made here too.
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

	/* Guard against division by zero */
	if (ival == 0)
		ereport(ERROR,
				errcode(ERRCODE_DIVISION_BY_ZERO),
				errmsg("division by zero"));

	/* Result zero check */
	if (var_ndigits == 0)
	{
		zero_var(result);
		result->dscale = rscale;
		return;
	}

	/*
	 * Determine the result sign, weight and number of digits to calculate.
	 * The weight figured here is correct if the emitted quotient has no
	 * leading zero digits; otherwise strip_var() will fix things up.
	 */
	if (var->sign == NUMERIC_POS)
		res_sign = ival > 0 ? NUMERIC_POS : NUMERIC_NEG;
	else
		res_sign = ival > 0 ? NUMERIC_NEG : NUMERIC_POS;
	res_weight = var->weight - ival_weight;
	/* The number of accurate result digits we need to produce: */
	res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS - 1) / DEC_DIGITS;
	/* ... but always at least 1 */
	res_ndigits = Max(res_ndigits, 1);
	/* If rounding needed, figure one more digit to ensure correct result */
	if (round)
		res_ndigits++;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* spare digit for later rounding */
	res_digits = res_buf + 1;

	/*
	 * Now compute the quotient digits.  This is the short division algorithm
	 * described in Knuth volume 2, section 4.3.1 exercise 16, except that we
	 * allow the divisor to exceed the internal base.
	 *
	 * In this algorithm, the carry from one digit to the next is at most
	 * divisor - 1.  Therefore, while processing the next digit, carry may
	 * become as large as divisor * NBASE - 1, and so it requires a 128-bit
	 * integer if this exceeds PG_UINT64_MAX.
	 */
	divisor = i64abs(ival);

	if (divisor <= PG_UINT64_MAX / NBASE)
	{
		/* carry cannot overflow 64 bits */
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
		/* carry may exceed 64 bits */
		uint128		carry = 0;

		for (i = 0; i < res_ndigits; i++)
		{
			carry = carry * NBASE + (i < var_ndigits ? var_digits[i] : 0);
			res_digits[i] = (NumericDigit) (carry / divisor);
			carry = carry % divisor;
		}
	}

	/* Store the quotient in result */
	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->sign = res_sign;

	/* Round or truncate to target rscale (and set result->dscale) */
	if (round)
		round_var(result, rscale);
	else
		trunc_var(result, rscale);

	/* Strip leading/trailing zeroes */
	strip_var(result);
}
#endif


/*
 * Default scale selection for division
 *
 * Returns the appropriate result scale for the division result.
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
	 * The result scale of a division isn't specified in any SQL standard. For
	 * PostgreSQL we select a result scale that will give at least
	 * NUMERIC_MIN_SIG_DIGITS significant digits, so that numeric gives a
	 * result no less accurate than float8; but use a scale not less than
	 * either input's display scale.
	 */

	/* Get the actual (normalized) weight and first digit of each input */

	weight1 = 0;				/* values to use if var1 is zero */
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

	weight2 = 0;				/* values to use if var2 is zero */
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
	 * Estimate weight of quotient.  If the two first digits are equal, we
	 * can't be sure, but assume that var1 is less than var2.
	 */
	qweight = weight1 - weight2;
	if (firstdigit1 <= firstdigit2)
		qweight--;

	/* Select result scale */
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
 *	Calculate the modulo of two numerics at variable level
 */
static void
mod_var(const NumericVar *var1, const NumericVar *var2, NumericVar *result)
{
	NumericVar	tmp;

	init_var(&tmp);

	/* ---------
	 * We do this using the equation
	 *		mod(x,y) = x - trunc(x/y)*y
	 * div_var can be persuaded to give us trunc(x/y) directly.
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
 *	Calculate the truncated integer quotient and numeric remainder of two
 *	numeric variables.  The remainder is precise to var2's dscale.
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
	 * Use div_var_fast() to get an initial estimate for the integer quotient.
	 * This might be inaccurate (per the warning in div_var_fast's comments),
	 * but we can correct it below.
	 */
	div_var_fast(var1, var2, &q, 0, false);

	/* Compute initial estimate of remainder using the quotient estimate. */
	mul_var(var2, &q, &r, var2->dscale);
	sub_var(var1, &r, &r);

	/*
	 * Adjust the results if necessary --- the remainder should have the same
	 * sign as var1, and its absolute value should be less than the absolute
	 * value of var2.
	 */
	while (r.ndigits != 0 && r.sign != var1->sign)
	{
		/* The absolute value of the quotient is too large */
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
		/* The absolute value of the quotient is too small */
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
 *	Return the smallest integer greater than or equal to the argument
 *	on variable level
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
 *	Return the largest integer equal to or less than the argument
 *	on variable level
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
 *	Calculate the greatest common divisor of two numerics at variable level
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
	 * Arrange for var1 to be the number with the greater absolute value.
	 *
	 * This would happen automatically in the loop below, but avoids an
	 * expensive modulo operation.
	 */
	cmp = cmp_abs(var1, var2);
	if (cmp < 0)
	{
		const NumericVar *tmp = var1;

		var1 = var2;
		var2 = tmp;
	}

	/*
	 * Also avoid the taking the modulo if the inputs have the same absolute
	 * value, or if the smaller input is zero.
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

	/* Use the Euclidean algorithm to find the GCD */
	set_var_from_var(var1, &tmp_arg);
	set_var_from_var(var2, result);

	for (;;)
	{
		/* this loop can take a while, so allow it to be interrupted */
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
 *	Compute the square root of x using the Karatsuba Square Root algorithm.
 *	NOTE: we allow rscale < 0 here, implying rounding before the decimal
 *	point.
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
	 * SQL2003 defines sqrt() in terms of power, so we need to emit the right
	 * SQLSTATE error code if the operand is negative.
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
	 * The result weight is half the input weight, rounded towards minus
	 * infinity --- res_weight = floor(arg->weight / 2).
	 */
	if (arg->weight >= 0)
		res_weight = arg->weight / 2;
	else
		res_weight = -((-arg->weight - 1) / 2 + 1);

	/*
	 * Number of NBASE digits to compute.  To ensure correct rounding, compute
	 * at least 1 extra decimal digit.  We explicitly allow rscale to be
	 * negative here, but must always compute at least 1 NBASE digit.  Thus
	 * res_ndigits = res_weight + 1 + ceil((rscale + 1) / DEC_DIGITS) or 1.
	 */
	if (rscale + 1 >= 0)
		res_ndigits = res_weight + 1 + (rscale + DEC_DIGITS) / DEC_DIGITS;
	else
		res_ndigits = res_weight + 1 - (-rscale - 1) / DEC_DIGITS;
	res_ndigits = Max(res_ndigits, 1);

	/*
	 * Number of source NBASE digits logically required to produce a result
	 * with this precision --- every digit before the decimal point, plus 2
	 * for each result digit after the decimal point (or minus 2 for each
	 * result digit we round before the decimal point).
	 */
	src_ndigits = arg->weight + 1 + (res_ndigits - res_weight - 1) * 2;
	src_ndigits = Max(src_ndigits, 1);

	/* ----------
	 * From this point on, we treat the input and the result as integers and
	 * compute the integer square root and remainder using the Karatsuba
	 * Square Root algorithm, which may be written recursively as follows:
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
	 * See "Karatsuba Square Root", Paul Zimmermann, INRIA Research Report
	 * RR-3805, November 1999.  At the time of writing this was available
	 * on the net at <https://hal.inria.fr/inria-00072854>.
	 *
	 * The way to read the assumption "n = a3*b^3 + a2*b^2 + a1*b + a0" is
	 * "choose a base b such that n requires at least four base-b digits to
	 * express; then those digits are a3,a2,a1,a0, with a3 possibly larger
	 * than b".  For optimal performance, b should have approximately a
	 * quarter the number of digits in the input, so that the outer square
	 * root computes roughly twice as many digits as the inner one.  For
	 * simplicity, we choose b = NBASE^blen, an integer power of NBASE.
	 *
	 * We implement the algorithm iteratively rather than recursively, to
	 * allow the working variables to be reused.  With this approach, each
	 * digit of the input is read precisely once --- src_idx tracks the number
	 * of input digits used so far.
	 *
	 * The array ndigits[] holds the number of NBASE digits of the input that
	 * will have been used at the end of each iteration, which roughly doubles
	 * each time.  Note that the array elements are stored in reverse order,
	 * so if the final iteration requires src_ndigits = 37 input digits, the
	 * array will contain [37,19,11,7,5,3], and we would start by computing
	 * the square root of the 3 most significant NBASE digits.
	 *
	 * In each iteration, we choose blen to be the largest integer for which
	 * the input number has a3 >= b/4, when written in the form above.  In
	 * general, this means blen = src_ndigits / 4 (truncated), but if
	 * src_ndigits is a multiple of 4, that might lead to the coefficient a3
	 * being less than b/4 (if the first input digit is less than NBASE/4), in
	 * which case we choose blen = src_ndigits / 4 - 1.  The number of digits
	 * in the inner square root is then src_ndigits - 2*blen.  So, for
	 * example, if we have src_ndigits = 26 initially, the array ndigits[]
	 * will be either [26,14,8,4] or [26,14,8,6,4], depending on the size of
	 * the first input digit.
	 *
	 * Additionally, we can put an upper bound on the number of steps required
	 * as follows --- suppose that the number of source digits is an n-bit
	 * number in the range [2^(n-1), 2^n-1], then blen will be in the range
	 * [2^(n-3)-1, 2^(n-2)-1] and the number of digits in the inner square
	 * root will be in the range [2^(n-2), 2^(n-1)+1].  In the next step, blen
	 * will be in the range [2^(n-4)-1, 2^(n-3)] and the number of digits in
	 * the next inner square root will be in the range [2^(n-3), 2^(n-2)+1].
	 * This pattern repeats, and in the worst case the array ndigits[] will
	 * contain [2^n-1, 2^(n-1)+1, 2^(n-2)+1, ... 9, 5, 3], and the computation
	 * will require n steps.  Therefore, since all digit array sizes are
	 * signed 32-bit integers, the number of steps required is guaranteed to
	 * be less than 32.
	 * ----------
	 */
	step = 0;
	while ((ndigits[step] = src_ndigits) > 4)
	{
		/* Choose b so that a3 >= b/4, as described above */
		blen = src_ndigits / 4;
		if (blen * 4 == src_ndigits && arg->digits[0] < NBASE / 4)
			blen--;

		/* Number of digits in the next step (inner square root) */
		src_ndigits -= 2 * blen;
		step++;
	}

	/*
	 * First iteration (innermost square root and remainder):
	 *
	 * Here src_ndigits <= 4, and the input fits in an int64.  Its square root
	 * has at most 9 decimal digits, so estimate it using double precision
	 * arithmetic, which will in fact almost certainly return the correct
	 * result with no further correction required.
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
	 * Use Newton's method to correct the result, if necessary.
	 *
	 * This uses integer division with truncation to compute the truncated
	 * integer square root by iterating using the formula x -> (x + n/x) / 2.
	 * This is known to converge to isqrt(n), unless n+1 is a perfect square.
	 * If n+1 is a perfect square, the sequence will oscillate between the two
	 * values isqrt(n) and isqrt(n)+1, so we can be assured of convergence by
	 * checking the remainder.
	 */
	while (r_int64 < 0 || r_int64 > 2 * s_int64)
	{
		s_int64 = (s_int64 + arg_int64 / s_int64) / 2;
		r_int64 = arg_int64 - s_int64 * s_int64;
	}

	/*
	 * Iterations with src_ndigits <= 8:
	 *
	 * The next 1 or 2 iterations compute larger (outer) square roots with
	 * src_ndigits <= 8, so the result still fits in an int64 (even though the
	 * input no longer does) and we can continue to compute using int64
	 * variables to avoid more expensive numeric computations.
	 *
	 * It is fairly easy to see that there is no risk of the intermediate
	 * values below overflowing 64-bit integers.  In the worst case, the
	 * previous iteration will have computed a 3-digit square root (of a
	 * 6-digit input less than NBASE^6 / 4), so at the start of this
	 * iteration, s will be less than NBASE^3 / 2 = 10^12 / 2, and r will be
	 * less than 10^12.  In this case, blen will be 1, so numer will be less
	 * than 10^17, and denom will be less than 10^12 (and hence u will also be
	 * less than 10^12).  Finally, since q^2 = u*b + a0 - r, we can also be
	 * sure that q^2 < 10^17.  Therefore all these quantities fit comfortably
	 * in 64-bit integers.
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

		/* Extract a1 and a0, and compute b */
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

		/* Compute (q,u) = DivRem(r*b + a1, 2*s) */
		numer = r_int64 * b + a1;
		denom = 2 * s_int64;
		q = numer / denom;
		u = numer - q * denom;

		/* Compute s = s*b + q and r = u*b + a0 - q^2 */
		s_int64 = s_int64 * b + q;
		r_int64 = u * b + a0 - q * q;

		if (r_int64 < 0)
		{
			/* s is too large by 1; set r += s, s--, r += s */
			r_int64 += s_int64;
			s_int64--;
			r_int64 += s_int64;
		}

		Assert(src_idx == src_ndigits); /* All input digits consumed */
		step--;
	}

	/*
	 * On platforms with 128-bit integer support, we can further delay the
	 * need to use numeric variables.
	 */
#ifdef HAVE_INT128
	if (step >= 0)
	{
		int128		s_int128;
		int128		r_int128;

		s_int128 = s_int64;
		r_int128 = r_int64;

		/*
		 * Iterations with src_ndigits <= 16:
		 *
		 * The result fits in an int128 (even though the input doesn't) so we
		 * use int128 variables to avoid more expensive numeric computations.
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

			/* Extract a1 and a0, and compute b */
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

			/* Compute (q,u) = DivRem(r*b + a1, 2*s) */
			numer = r_int128 * b + a1;
			denom = 2 * s_int128;
			q = numer / denom;
			u = numer - q * denom;

			/* Compute s = s*b + q and r = u*b + a0 - q^2 */
			s_int128 = s_int128 * b + q;
			r_int128 = u * b + a0 - q * q;

			if (r_int128 < 0)
			{
				/* s is too large by 1; set r += s, s--, r += s */
				r_int128 += s_int128;
				s_int128--;
				r_int128 += s_int128;
			}

			Assert(src_idx == src_ndigits); /* All input digits consumed */
			step--;
		}

		/*
		 * All remaining iterations require numeric variables.  Convert the
		 * integer values to NumericVar and continue.  Note that in the final
		 * iteration we don't need the remainder, so we can save a few cycles
		 * there by not fully computing it.
		 */
		int128_to_numericvar(s_int128, &s_var);
		if (step >= 0)
			int128_to_numericvar(r_int128, &r_var);
	}
	else
	{
		int64_to_numericvar(s_int64, &s_var);
		/* step < 0, so we certainly don't need r */
	}
#else							/* !HAVE_INT128 */
	int64_to_numericvar(s_int64, &s_var);
	if (step >= 0)
		int64_to_numericvar(r_int64, &r_var);
#endif							/* HAVE_INT128 */

	/*
	 * The remaining iterations with src_ndigits > 8 (or 16, if have int128)
	 * use numeric variables.
	 */
	while (step >= 0)
	{
		int			tmp_len;

		src_ndigits = ndigits[step];
		blen = (src_ndigits - src_idx) / 2;

		/* Extract a1 and a0 */
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

		/* Compute (q,u) = DivRem(r*b + a1, 2*s) */
		set_var_from_var(&r_var, &q_var);
		q_var.weight += blen;
		add_var(&q_var, &a1_var, &q_var);
		add_var(&s_var, &s_var, &u_var);
		div_mod_var(&q_var, &u_var, &q_var, &u_var);

		/* Compute s = s*b + q */
		s_var.weight += blen;
		add_var(&s_var, &q_var, &s_var);

		/*
		 * Compute r = u*b + a0 - q^2.
		 *
		 * In the final iteration, we don't actually need r; we just need to
		 * know whether it is negative, so that we know whether to adjust s.
		 * So instead of the final subtraction we can just compare.
		 */
		u_var.weight += blen;
		add_var(&u_var, &a0_var, &u_var);
		mul_var(&q_var, &q_var, &q_var, 0);

		if (step > 0)
		{
			/* Need r for later iterations */
			sub_var(&u_var, &q_var, &r_var);
			if (r_var.sign == NUMERIC_NEG)
			{
				/* s is too large by 1; set r += s, s--, r += s */
				add_var(&r_var, &s_var, &r_var);
				sub_var(&s_var, &const_one, &s_var);
				add_var(&r_var, &s_var, &r_var);
			}
		}
		else
		{
			/* Don't need r anymore, except to test if s is too large by 1 */
			if (cmp_var(&u_var, &q_var) < 0)
				sub_var(&s_var, &const_one, &s_var);
		}

		Assert(src_idx == src_ndigits); /* All input digits consumed */
		step--;
	}

	/*
	 * Construct the final result, rounding it to the requested precision.
	 */
	set_var_from_var(&s_var, result);
	result->weight = res_weight;
	result->sign = NUMERIC_POS;

	/* Round to target rscale (and set result->dscale) */
	round_var(result, rscale);

	/* Strip leading and trailing zeroes */
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
 *	Raise e to the power of x, computed to rscale fractional digits
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
	 * Estimate the dweight of the result using floating point arithmetic, so
	 * that we can choose an appropriate local rscale for the calculation.
	 */
	val = numericvar_to_double_no_overflow(&x);

	/* Guard against overflow/underflow */
	/* If you change this limit, see also power_var()'s limit */
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

	/* decimal weight = log10(e^x) = x * log10(e) */
	dweight = (int) (val * 0.434294481903252);

	/*
	 * Reduce x to the range -0.01 <= x <= 0.01 (approximately) by dividing by
	 * 2^ndiv2, to improve the convergence rate of the Taylor series.
	 *
	 * Note that the overflow check above ensures that fabs(x) < 6000, which
	 * means that ndiv2 <= 20 here.
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
	 * Set the scale for the Taylor series expansion.  The final result has
	 * (dweight + rscale + 1) significant digits.  In addition, we have to
	 * raise the Taylor series result to the power 2^ndiv2, which introduces
	 * an error of up to around log10(2^ndiv2) digits, so work with this many
	 * extra digits of precision (plus a few more for good measure).
	 */
	sig_digits = 1 + dweight + rscale + (int) (ndiv2 * 0.301029995663981);
	sig_digits = Max(sig_digits, 0) + 8;

	local_rscale = sig_digits - 1;

	/*
	 * Use the Taylor series
	 *
	 * exp(x) = 1 + x + x^2/2! + x^3/3! + ...
	 *
	 * Given the limited range of x, this should converge reasonably quickly.
	 * We run the series until the terms fall below the local_rscale limit.
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
	 * Compensate for the argument range reduction.  Since the weight of the
	 * result doubles with each multiplication, we can reduce the local rscale
	 * as we proceed.
	 */
	while (ndiv2-- > 0)
	{
		local_rscale = sig_digits - result->weight * 2 * DEC_DIGITS;
		local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);
		mul_var(result, result, result, local_rscale);
	}

	/* Round to requested rscale */
	round_var(result, rscale);

	free_var(&x);
	free_var(&elem);
}


/*
 * Estimate the dweight of the most significant decimal digit of the natural
 * logarithm of a number.
 *
 * Essentially, we're approximating log10(abs(ln(var))).  This is used to
 * determine the appropriate rscale when computing natural logarithms.
 *
 * Note: many callers call this before range-checking the input.  Therefore,
 * we must be robust against values that are invalid to apply ln() to.
 * We don't wish to throw an error here, so just return zero in such cases.
 */
static int
estimate_ln_dweight(const NumericVar *var)
{
	int			ln_dweight;

	/* Caller should fail on ln(negative), but for the moment return zero */
	if (var->sign != NUMERIC_POS)
		return 0;

	if (cmp_var(var, &const_zero_point_nine) >= 0 &&
		cmp_var(var, &const_one_point_one) <= 0)
	{
		/*
		 * 0.9 <= var <= 1.1
		 *
		 * ln(var) has a negative weight (possibly very large).  To get a
		 * reasonably accurate result, estimate it using ln(1+x) ~= x.
		 */
		NumericVar	x;

		init_var(&x);
		sub_var(var, &const_one, &x);

		if (x.ndigits > 0)
		{
			/* Use weight of most significant decimal digit of x */
			ln_dweight = x.weight * DEC_DIGITS + (int) log10(x.digits[0]);
		}
		else
		{
			/* x = 0.  Since ln(1) = 0 exactly, we don't need extra digits */
			ln_dweight = 0;
		}

		free_var(&x);
	}
	else
	{
		/*
		 * Estimate the logarithm using the first couple of digits from the
		 * input number.  This will give an accurate result whenever the input
		 * is not too close to 1.
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
			 * We have var ~= digits * 10^dweight
			 * so ln(var) ~= ln(digits) + dweight * ln(10)
			 *----------
			 */
			ln_var = log((double) digits) + dweight * 2.302585092994046;
			ln_dweight = (int) log10(fabs(ln_var));
		}
		else
		{
			/* Caller should fail on ln(0), but for the moment return zero */
			ln_dweight = 0;
		}
	}

	return ln_dweight;
}


/*
 * ln_var() -
 *
 *	Compute the natural log of x
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
	 * Reduce input into range 0.9 < x < 1.1 with repeated sqrt() operations.
	 *
	 * The final logarithm will have up to around rscale+6 significant digits.
	 * Each sqrt() will roughly halve the weight of x, so adjust the local
	 * rscale as we work so that we keep this many significant digits at each
	 * step (plus a few more for good measure).
	 *
	 * Note that we allow local_rscale < 0 during this input reduction
	 * process, which implies rounding before the decimal point.  sqrt_var()
	 * explicitly supports this, and it significantly reduces the work
	 * required to reduce very large inputs to the required range.  Once the
	 * input reduction is complete, x.weight will be 0 and its display scale
	 * will be non-negative again.
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
	 * We use the Taylor series for 0.5 * ln((1+z)/(1-z)),
	 *
	 * z + z^3/3 + z^5/5 + ...
	 *
	 * where z = (x-1)/(x+1) is in the range (approximately) -0.053 .. 0.048
	 * due to the above range-reduction of x.
	 *
	 * The convergence of this is not as fast as one would like, but is
	 * tolerable given that z is small.
	 *
	 * The Taylor series result will be multiplied by 2^(nsqrt+1), which has a
	 * decimal weight of (nsqrt+1) * log10(2), so work with this many extra
	 * digits of precision (plus a few more for good measure).
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

	/* Compensate for argument range reduction, round to requested rscale */
	mul_var(result, &fact, result, rscale);

	free_var(&x);
	free_var(&xx);
	free_var(&elem);
	free_var(&fact);
}


/*
 * log_var() -
 *
 *	Compute the logarithm of num in a given base.
 *
 *	Note: this routine chooses dscale of the result.
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

	/* Estimated dweights of ln(base), ln(num) and the final result */
	ln_base_dweight = estimate_ln_dweight(base);
	ln_num_dweight = estimate_ln_dweight(num);
	result_dweight = ln_num_dweight - ln_base_dweight;

	/*
	 * Select the scale of the result so that it will have at least
	 * NUMERIC_MIN_SIG_DIGITS significant digits and is not less than either
	 * input's display scale.
	 */
	rscale = NUMERIC_MIN_SIG_DIGITS - result_dweight;
	rscale = Max(rscale, base->dscale);
	rscale = Max(rscale, num->dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/*
	 * Set the scales for ln(base) and ln(num) so that they each have more
	 * significant digits than the final result.
	 */
	ln_base_rscale = rscale + result_dweight - ln_base_dweight + 8;
	ln_base_rscale = Max(ln_base_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	ln_num_rscale = rscale + result_dweight - ln_num_dweight + 8;
	ln_num_rscale = Max(ln_num_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	/* Form natural logarithms */
	ln_var(base, &ln_base, ln_base_rscale);
	ln_var(num, &ln_num, ln_num_rscale);

	/* Divide and round to the required scale */
	div_var_fast(&ln_num, &ln_base, result, rscale, true);

	free_var(&ln_num);
	free_var(&ln_base);
}


/*
 * power_var() -
 *
 *	Raise base to the power of exp
 *
 *	Note: this routine chooses dscale of the result.
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

	/* If exp can be represented as an integer, use power_var_int */
	if (exp->ndigits == 0 || exp->ndigits <= exp->weight + 1)
	{
		/* exact integer, but does it fit in int? */
		int64		expval64;

		if (numericvar_to_int64(exp, &expval64))
		{
			if (expval64 >= PG_INT32_MIN && expval64 <= PG_INT32_MAX)
			{
				/* Okay, use power_var_int */
				power_var_int(base, (int) expval64, exp->dscale, result);
				return;
			}
		}
	}

	/*
	 * This avoids log(0) for cases of 0 raised to a non-integer.  0 ^ 0 is
	 * handled by power_var_int().
	 */
	if (cmp_var(base, &const_zero) == 0)
	{
		set_var_from_var(&const_zero, result);
		result->dscale = NUMERIC_MIN_SIG_DIGITS;	/* no need to round */
		return;
	}

	init_var(&abs_base);
	init_var(&ln_base);
	init_var(&ln_num);

	/*
	 * If base is negative, insist that exp be an integer.  The result is then
	 * positive if exp is even and negative if exp is odd.
	 */
	if (base->sign == NUMERIC_NEG)
	{
		/*
		 * Check that exp is an integer.  This error code is defined by the
		 * SQL standard, and matches other errors in numeric_power().
		 */
		if (exp->ndigits > 0 && exp->ndigits > exp->weight + 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
					 errmsg("a negative number raised to a non-integer power yields a complex result")));

		/* Test if exp is odd or even */
		if (exp->ndigits > 0 && exp->ndigits == exp->weight + 1 &&
			(exp->digits[exp->ndigits - 1] & 1))
			res_sign = NUMERIC_NEG;
		else
			res_sign = NUMERIC_POS;

		/* Then work with abs(base) below */
		set_var_from_var(base, &abs_base);
		abs_base.sign = NUMERIC_POS;
		base = &abs_base;
	}
	else
		res_sign = NUMERIC_POS;

	/*----------
	 * Decide on the scale for the ln() calculation.  For this we need an
	 * estimate of the weight of the result, which we obtain by doing an
	 * initial low-precision calculation of exp * ln(base).
	 *
	 * We want result = e ^ (exp * ln(base))
	 * so result dweight = log10(result) = exp * ln(base) * log10(e)
	 *
	 * We also perform a crude overflow test here so that we can exit early if
	 * the full-precision result is sure to overflow, and to guard against
	 * integer overflow when determining the scale for the real calculation.
	 * exp_var() supports inputs up to NUMERIC_MAX_RESULT_SCALE * 3, so the
	 * result will overflow if exp * ln(base) >= NUMERIC_MAX_RESULT_SCALE * 3.
	 * Since the values here are only approximations, we apply a small fuzz
	 * factor to this overflow test and let exp_var() determine the exact
	 * overflow threshold so that it is consistent for all inputs.
	 *----------
	 */
	ln_dweight = estimate_ln_dweight(base);

	/*
	 * Set the scale for the low-precision calculation, computing ln(base) to
	 * around 8 significant digits.  Note that ln_dweight may be as small as
	 * -NUMERIC_DSCALE_MAX, so the scale may exceed NUMERIC_MAX_DISPLAY_SCALE
	 * here.
	 */
	local_rscale = 8 - ln_dweight;
	local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	ln_var(base, &ln_base, local_rscale);

	mul_var(&ln_base, exp, &ln_num, local_rscale);

	val = numericvar_to_double_no_overflow(&ln_num);

	/* initial overflow/underflow test with fuzz factor */
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

	val *= 0.434294481903252;	/* approximate decimal result weight */

	/* choose the result scale */
	rscale = NUMERIC_MIN_SIG_DIGITS - (int) val;
	rscale = Max(rscale, base->dscale);
	rscale = Max(rscale, exp->dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/* significant digits required in the result */
	sig_digits = rscale + (int) val;
	sig_digits = Max(sig_digits, 0);

	/* set the scale for the real exp * ln(base) calculation */
	local_rscale = sig_digits - ln_dweight + 8;
	local_rscale = Max(local_rscale, NUMERIC_MIN_DISPLAY_SCALE);

	/* and do the real calculation */

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
 *	Raise base to the power of exp, where exp is an integer.
 *
 *	Note: this routine chooses dscale of the result.
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
	 * Choose the result scale.  For this we need an estimate of the decimal
	 * weight of the result, which we obtain by approximating using double
	 * precision arithmetic.
	 *
	 * We also perform crude overflow/underflow tests here so that we can exit
	 * early if the result is sure to overflow/underflow, and to guard against
	 * integer overflow when choosing the result scale.
	 */
	if (base->ndigits != 0)
	{
		/*----------
		 * Choose f (double) and p (int) such that base ~= f * 10^p.
		 * Then log10(result) = log10(base^exp) ~= exp * (log10(f) + p).
		 *----------
		 */
		f = base->digits[0];
		p = base->weight * DEC_DIGITS;

		for (i = 1; i < base->ndigits && i * DEC_DIGITS < 16; i++)
		{
			f = f * NBASE + base->digits[i];
			p -= DEC_DIGITS;
		}

		f = exp * (log10(f) + p);	/* approximate decimal result weight */
	}
	else
		f = 0;					/* result is 0 or 1 (weight 0), or error */

	/* overflow/underflow tests with fuzz factors */
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
	 * Choose the result scale in the same way as power_var(), so it has at
	 * least NUMERIC_MIN_SIG_DIGITS significant digits and is not less than
	 * either input's display scale.
	 */
	rscale = NUMERIC_MIN_SIG_DIGITS - (int) f;
	rscale = Max(rscale, base->dscale);
	rscale = Max(rscale, exp_dscale);
	rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
	rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);

	/* Handle some common special cases, as well as corner cases */
	switch (exp)
	{
		case 0:

			/*
			 * While 0 ^ 0 can be either 1 or indeterminate (error), we treat
			 * it as 1 because most programming languages do this. SQL:2003
			 * also requires a return value of 1.
			 * https://en.wikipedia.org/wiki/Exponentiation#Zero_to_the_zero_power
			 */
			set_var_from_var(&const_one, result);
			result->dscale = rscale;	/* no need to round */
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

	/* Handle the special case where the base is zero */
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
	 * The general case repeatedly multiplies base according to the bit
	 * pattern of exp.
	 *
	 * The local rscale used for each multiplication is varied to keep a fixed
	 * number of significant digits, sufficient to give the required result
	 * scale.
	 */

	/*
	 * Approximate number of significant digits in the result.  Note that the
	 * underflow test above, together with the choice of rscale, ensures that
	 * this approximation is necessarily > 0.
	 */
	sig_digits = 1 + rscale + (int) f;

	/*
	 * The multiplications to produce the result may introduce an error of up
	 * to around log10(abs(exp)) digits, so work with this many extra digits
	 * of precision (plus a few more for good measure).
	 */
	sig_digits += (int) log(fabs((double) exp)) + 8;

	/*
	 * Now we can proceed with the multiplications.
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
		 * Do the multiplications using rscales large enough to hold the
		 * results to the required number of significant digits, but don't
		 * waste time by exceeding the scales of the numbers themselves.
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
		 * When abs(base) > 1, the number of digits to the left of the decimal
		 * point in base_prod doubles at each iteration, so if exp is large we
		 * could easily spend large amounts of time and memory space doing the
		 * multiplications.  But once the weight exceeds what will fit in
		 * int16, the final result is guaranteed to overflow (or underflow, if
		 * exp < 0), so we can give up before wasting too many cycles.
		 */
		if (base_prod.weight > NUMERIC_WEIGHT_MAX ||
			result->weight > NUMERIC_WEIGHT_MAX)
		{
			/* overflow, unless neg, in which case result should be 0 */
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

	/* Compensate for input sign, and round to requested rscale */
	if (neg)
		div_var_fast(&const_one, result, result, rscale, true);
	else
		round_var(result, rscale);
}

/*
 * power_ten_int() -
 *
 *	Raise ten to the power of exp, where exp is an integer.  Note that unlike
 *	power_var_int(), this does no overflow/underflow checking or rounding.
 */
static void
power_ten_int(int exp, NumericVar *result)
{
	/* Construct the result directly, starting from 10^0 = 1 */
	set_var_from_var(&const_one, result);

	/* Scale needed to represent the result exactly */
	result->dscale = exp < 0 ? -exp : 0;

	/* Base-NBASE weight of result and remaining exponent */
	if (exp >= 0)
		result->weight = exp / DEC_DIGITS;
	else
		result->weight = (exp + 1) / DEC_DIGITS - 1;

	exp -= result->weight * DEC_DIGITS;

	/* Final adjustment of the result's single NBASE digit */
	while (exp-- > 0)
		result->digits[0] *= 10;
}


/* ----------------------------------------------------------------------
 *
 * Following are the lowest level functions that operate unsigned
 * on the variable level
 *
 * ----------------------------------------------------------------------
 */


/* ----------
 * cmp_abs() -
 *
 *	Compare the absolute values of var1 and var2
 *	Returns:	-1 for ABS(var1) < ABS(var2)
 *				0  for ABS(var1) == ABS(var2)
 *				1  for ABS(var1) > ABS(var2)
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
 *	Main routine of cmp_abs(). This function can be used by both
 *	NumericVar and Numeric.
 * ----------
 */
static int
cmp_abs_common(const NumericDigit *var1digits, int var1ndigits, int var1weight,
			   const NumericDigit *var2digits, int var2ndigits, int var2weight)
{
	int			i1 = 0;
	int			i2 = 0;

	/* Check any digits before the first common digit */

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

	/* At this point, either w1 == w2 or we've run out of digits */

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
	 * At this point, we've run out of digits on one side or the other; so any
	 * remaining nonzero digits imply that side is larger
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
 *	Add the absolute values of two variables into result.
 *	result might point to one of the operands without danger.
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

	/* copy these values into local vars for speed in inner loop */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	res_weight = Max(var1->weight, var2->weight) + 1;

	res_dscale = Max(var1->dscale, var2->dscale);

	/* Note: here we are figuring rscale in base-NBASE digits */
	rscale1 = var1->ndigits - var1->weight - 1;
	rscale2 = var2->ndigits - var2->weight - 1;
	res_rscale = Max(rscale1, rscale2);

	res_ndigits = res_rscale + res_weight + 1;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* spare digit for later rounding */
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

	Assert(carry == 0);			/* else we failed to allow for carry out */

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->dscale = res_dscale;

	/* Remove leading/trailing zeroes */
	strip_var(result);
}


/*
 * sub_abs()
 *
 *	Subtract the absolute value of var2 from the absolute value of var1
 *	and store in result. result might point to one of the operands
 *	without danger.
 *
 *	ABS(var1) MUST BE GREATER OR EQUAL ABS(var2) !!!
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

	/* copy these values into local vars for speed in inner loop */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	res_weight = var1->weight;

	res_dscale = Max(var1->dscale, var2->dscale);

	/* Note: here we are figuring rscale in base-NBASE digits */
	rscale1 = var1->ndigits - var1->weight - 1;
	rscale2 = var2->ndigits - var2->weight - 1;
	res_rscale = Max(rscale1, rscale2);

	res_ndigits = res_rscale + res_weight + 1;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	res_buf = digitbuf_alloc(res_ndigits + 1);
	res_buf[0] = 0;				/* spare digit for later rounding */
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

	Assert(borrow == 0);		/* else caller gave us var1 < var2 */

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->dscale = res_dscale;

	/* Remove leading/trailing zeroes */
	strip_var(result);
}

/*
 * round_var
 *
 * Round the value of a variable to no more than rscale decimal digits
 * after the decimal point.  NOTE: we allow rscale < 0 here, implying
 * rounding before the decimal point.
 */
static void
round_var(NumericVar *var, int rscale)
{
	NumericDigit *digits = var->digits;
	int			di;
	int			ndigits;
	int			carry;

	var->dscale = rscale;

	/* decimal digits wanted */
	di = (var->weight + 1) * DEC_DIGITS + rscale;

	/*
	 * If di = 0, the value loses all digits, but could round up to 1 if its
	 * first extra digit is >= 5.  If di < 0 the result must be 0.
	 */
	if (di < 0)
	{
		var->ndigits = 0;
		var->weight = 0;
		var->sign = NUMERIC_POS;
	}
	else
	{
		/* NBASE digits wanted */
		ndigits = (di + DEC_DIGITS - 1) / DEC_DIGITS;

		/* 0, or number of decimal digits to keep in last NBASE digit */
		di %= DEC_DIGITS;

		if (ndigits < var->ndigits ||
			(ndigits == var->ndigits && di > 0))
		{
			var->ndigits = ndigits;

#if DEC_DIGITS == 1
			/* di must be zero */
			carry = (digits[ndigits] >= HALF_NBASE) ? 1 : 0;
#else
			if (di == 0)
				carry = (digits[ndigits] >= HALF_NBASE) ? 1 : 0;
			else
			{
				/* Must round within last NBASE digit */
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

			/* Propagate carry if needed */
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
				Assert(ndigits == -1);	/* better not have added > 1 digit */
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
 * Truncate (towards zero) the value of a variable at rscale decimal digits
 * after the decimal point.  NOTE: we allow rscale < 0 here, implying
 * truncation before the decimal point.
 */
static void
trunc_var(NumericVar *var, int rscale)
{
	int			di;
	int			ndigits;

	var->dscale = rscale;

	/* decimal digits wanted */
	di = (var->weight + 1) * DEC_DIGITS + rscale;

	/*
	 * If di <= 0, the value loses all digits.
	 */
	if (di <= 0)
	{
		var->ndigits = 0;
		var->weight = 0;
		var->sign = NUMERIC_POS;
	}
	else
	{
		/* NBASE digits wanted */
		ndigits = (di + DEC_DIGITS - 1) / DEC_DIGITS;

		if (ndigits <= var->ndigits)
		{
			var->ndigits = ndigits;

#if DEC_DIGITS == 1
			/* no within-digit stuff to worry about */
#else
			/* 0, or number of decimal digits to keep in last NBASE digit */
			di %= DEC_DIGITS;

			if (di > 0)
			{
				/* Must truncate within last NBASE digit */
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
 * Strip any leading and trailing zeroes from a numeric variable
 */
static void
strip_var(NumericVar *var)
{
	NumericDigit *digits = var->digits;
	int			ndigits = var->ndigits;

	/* Strip leading zeroes */
	while (ndigits > 0 && *digits == 0)
	{
		digits++;
		var->weight--;
		ndigits--;
	}

	/* Strip trailing zeroes */
	while (ndigits > 0 && digits[ndigits - 1] == 0)
		ndigits--;

	/* If it's zero, normalize the sign and weight */
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
 * Fast sum accumulator functions
 *
 * ----------------------------------------------------------------------
 */

/*
 * Reset the accumulator's value to zero.  The buffers to hold the digits
 * are not free'd.
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
 * Accumulate a new value.
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
	 * If we have accumulated too many values since the last carry
	 * propagation, do it now, to avoid overflowing.  (We could allow more
	 * than NBASE - 1, if we reserved two extra digits, rather than one, for
	 * carry propagation.  But even with NBASE - 1, this needs to be done so
	 * seldom, that the performance difference is negligible.)
	 */
	if (accum->num_uncarried == NBASE - 1)
		accum_sum_carry(accum);

	/*
	 * Adjust the weight or scale of the old value, so that it can accommodate
	 * the new value.
	 */
	accum_sum_rescale(accum, val);

	/* */
	if (val->sign == NUMERIC_POS)
		accum_digits = accum->pos_digits;
	else
		accum_digits = accum->neg_digits;

	/* copy these values into local vars for speed in loop */
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
 * Propagate carries.
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
	 * If no new values have been added since last carry propagation, nothing
	 * to do.
	 */
	if (accum->num_uncarried == 0)
		return;

	/*
	 * We maintain that the weight of the accumulator is always one larger
	 * than needed to hold the current value, before carrying, to make sure
	 * there is enough space for the possible extra digit when carry is
	 * propagated.  We cannot expand the buffer here, unless we require
	 * callers of accum_sum_final() to switch to the right memory context.
	 */
	Assert(accum->pos_digits[0] == 0 && accum->neg_digits[0] == 0);

	ndigits = accum->ndigits;

	/* Propagate carry in the positive sum */
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
	/* Did we use up the digit reserved for carry propagation? */
	if (newdig > 0)
		accum->have_carry_space = false;

	/* And the same for the negative sum */
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
 * Re-scale accumulator to accommodate new value.
 *
 * If the new value has more digits than the current digit buffers in the
 * accumulator, enlarge the buffers.
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
	 * Does the new value have a larger weight? If so, enlarge the buffers,
	 * and shift the existing value to the new weight, by adding leading
	 * zeros.
	 *
	 * We enforce that the accumulator always has a weight one larger than
	 * needed for the inputs, so that we have space for an extra digit at the
	 * final carry-propagation phase, if necessary.
	 */
	if (val->weight >= accum_weight)
	{
		accum_weight = val->weight + 1;
		accum_ndigits = accum_ndigits + (accum_weight - old_weight);
	}

	/*
	 * Even though the new value is small, we might've used up the space
	 * reserved for the carry digit in the last call to accum_sum_carry().  If
	 * so, enlarge to make room for another one.
	 */
	else if (!accum->have_carry_space)
	{
		accum_weight++;
		accum_ndigits++;
	}

	/* Is the new value wider on the right side? */
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
 * Return the current value of the accumulator.  This perform final carry
 * propagation, and adds together the positive and negative sums.
 *
 * Unlike all the other routines, the caller is not required to switch to
 * the memory context that holds the accumulator.
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

	/* Perform final carry */
	accum_sum_carry(accum);

	/* Create NumericVars representing the positive and negative sums */
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

	/* And add them together */
	add_var(&pos_var, &neg_var, result);

	/* Remove leading/trailing zeroes */
	strip_var(result);
}

/*
 * Copy an accumulator's state.
 *
 * 'dst' is assumed to be uninitialized beforehand.  No attempt is made at
 * freeing old values.
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
 * Add the current value of 'accum2' into 'accum'.
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
