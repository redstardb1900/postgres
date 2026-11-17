/* -----------------------------------------------------------------------
 * formatting.c
 *
 * src/backend/utils/adt/formatting.c
 *
 *
 *	 Portions Copyright (c) 1999-2023, PostgreSQL Global Development Group
 *
 *
 *	 TO_CHAR(); TO_TIMESTAMP(); TO_DATE(); TO_NUMBER();
 *
 *	 PostgreSQL 为 timestamp/int/float/numeric 提供格式化例程，
 *	 灵感来自 Oracle TO_CHAR() / TO_DATE() / TO_NUMBER() 例程。
 *
 *
 *	缓存与内存
 *	例程使用自身内部的缓存存放格式模式。
 *
 *	该缓存使用静态缓冲区，在事务间持久存在。  若格式
 *	模式大于缓存缓冲区，则总是调用解析器。
 *
 *
 *	 数字版说明：
 *	本版全部实现为关键字（即不使用后缀），
 *	因为一个格式模式只针对*一个*条目（数字）。
 *	timestamp 版则不同——那边关键字可带后缀。
 *
 *
 *	 timestamp 例程注意：
 *	本模块不使用 POSIX 的 'struct tm' 类型，而是使用
 *	PgSQL 类型——其 tm_mon 基于一（*非*零），
 *	年份不以 1900 为基，而是使用完整年份。
 *	模块支持 AD/BC/AM/PM。
 *
 *	to_char() 支持的类型：
 *
 *		Timestamp, Numeric, int4, int8, float4, float8
 *
 *	逆向转换支持的类型：
 *
 *		Timestamp	- to_timestamp()
 *		Date		- to_date()
 *		Numeric		- to_number()
 *
 *
 *	Karel Zak
 *
 * TODO
 *	- 更好的数字构造（格式化）/解析，现尚不
 *		  理想。
 *	- 用 Assert()
 *	- 增加罗马数字转标准数字的支持
 *	- 增加数字拼写支持
 *	- 增加串到串格式化的支持（我们必须胜过
 *	  Oracle）。
 *		to_char('Hello', 'X X X X X') -> 'H e l l o'
 *
 * -----------------------------------------------------------------------
 */

#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_elog_output	DEBUG3
#endif

#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <wctype.h>

#ifdef USE_ICU
#include <unicode/ustring.h>
#endif

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "varatt.h"


/* ----------
 * 例程标志
 * ----------
 */
#define DCH_FLAG		0x1		/* 日期-时间 */
#define NUM_FLAG		0x2		/* 数字标志*/
#define STD_FLAG		0x4		/* 标准标志 */

/* ----------
 * 关键字索引（ASCII 从 32 (' ') 到 126 (~)）
 * ----------
 */
#define KeyWord_INDEX_SIZE		('~' - ' ')
#define KeyWord_INDEX_FILTER(_c)	((_c) <= ' ' || (_c) >= '~' ? 0 : 1)

/* ----------
 * 单节点最大长度
 * ----------
 */
#define DCH_MAX_ITEM_SIZ	   12	/* 本地化日名上限		*/
#define NUM_MAX_ITEM_SIZ		8	/* 罗马数字（RN 15 字符）	*/


/* ----------
 * 格式解析器结构
 * ----------
 */
typedef struct
{
	const char *name;			/* 后缀串		*/
	int			len,			/* 后缀长度		*/
				id,				/* 用于 node->suffix */
				type;			/* 前缀 / 后缀		*/
} KeySuffix;

/* ----------
 * FromCharDateMode
 * ----------
 *
 * 该值用于指名关键字可归属的数种不同（且互斥）
 * 的日期约定中的哪一种。
 */
typedef enum
{
	FROM_CHAR_DATE_NONE = 0,	/* 该值不影响日期模式。 */
	FROM_CHAR_DATE_GREGORIAN,	/* 公历（日、月、年）风格日期 */
	FROM_CHAR_DATE_ISOWEEK		/* ISO 8601 周日期 */
} FromCharDateMode;

typedef struct
{
	const char *name;
	int			len;
	int			id;
	bool		is_digit;
	FromCharDateMode date_mode;
} KeyWord;

typedef struct
{
	uint8		type;			/* NODE_TYPE_XXX，见下 */
	char		character[MAX_MULTIBYTE_CHAR_LEN + 1];	/* 若为 CHAR */
	uint8		suffix;			/* 关键字前后缀码（如有）*/
	const KeyWord *key;			/* 若为 ACTION */
} FormatNode;

#define NODE_TYPE_END		1
#define NODE_TYPE_ACTION	2
#define NODE_TYPE_CHAR		3
#define NODE_TYPE_SEPARATOR	4
#define NODE_TYPE_SPACE		5

#define SUFFTYPE_PREFIX		1
#define SUFFTYPE_POSTFIX	2

#define CLOCK_24_HOUR		0
#define CLOCK_12_HOUR		1


/* ----------
 * 全月名
 * ----------
 */
static const char *const months_full[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December", NULL
};

static const char *const days_short[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", NULL
};

/* ----------
 * AD / BC
 * ----------
 *	不存在公元 0 年。年份从公元前 1 年跨到
 *	公元 1 年，故将其取为正数：year == -1 映射为
 *	零年，负年份上移。interval 年份返回原值。
 */
#define ADJUST_YEAR(year, is_interval)	((is_interval) ? (year) : ((year) <= 0 ? -((year) - 1) : (year)))

#define A_D_STR		"A.D."
#define a_d_STR		"a.d."
#define AD_STR		"AD"
#define ad_STR		"ad"

#define B_C_STR		"B.C."
#define b_c_STR		"b.c."
#define BC_STR		"BC"
#define bc_STR		"bc"

/*
 * seq_search 用 AD/BC 串。
 *
 * 各有两种变体：带句点的长形式与无句点的标准形
 * 式。
 *
 * 数组的排布方式使得 AD 的匹配项都位于偶数下标，而
 * BC 的匹配项都位于奇数下标。因此 BC 的布尔值由匹配
 * 项的数组下标对 2 取模得出。
 */
static const char *const adbc_strings[] = {ad_STR, bc_STR, AD_STR, BC_STR, NULL};
static const char *const adbc_strings_long[] = {a_d_STR, b_c_STR, A_D_STR, B_C_STR, NULL};

/* ----------
 * AM / PM
 * ----------
 */
#define A_M_STR		"A.M."
#define a_m_STR		"a.m."
#define AM_STR		"AM"
#define am_STR		"am"

#define P_M_STR		"P.M."
#define p_m_STR		"p.m."
#define PM_STR		"PM"
#define pm_STR		"pm"

/*
 * seq_search 用 AM/PM 串。
 *
 * 各有两种变体：带句点的长形式与无句点的标准形
 * 式。
 *
 * 数组的排布方式使得 AM 的匹配项都位于偶数下标，而
 * PM 的匹配项都位于奇数下标。因此 PM 的布尔值由匹配
 * 项的数组下标对 2 取模得出。
 */
static const char *const ampm_strings[] = {am_STR, pm_STR, AM_STR, PM_STR, NULL};
static const char *const ampm_strings_long[] = {a_m_STR, p_m_STR, A_M_STR, P_M_STR, NULL};

/* ----------
 * 罗马数字月份
 * （FROM_CHAR 里 seq_search 必须逆序存放，因为
 *	'VIII' 的优先级必须高于 'V'）
 * ----------
 */
static const char *const rm_months_upper[] =
{"XII", "XI", "X", "IX", "VIII", "VII", "VI", "V", "IV", "III", "II", "I", NULL};

static const char *const rm_months_lower[] =
{"xii", "xi", "x", "ix", "viii", "vii", "vi", "v", "iv", "iii", "ii", "i", NULL};

/* ----------
 * 罗马数字
 * ----------
 */
static const char *const rm1[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", NULL};
static const char *const rm10[] = {"X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC", NULL};
static const char *const rm100[] = {"C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM", NULL};

/* ----------
 * 序数后缀
 * ----------
 */
static const char *const numTH[] = {"ST", "ND", "RD", "TH", NULL};
static const char *const numth[] = {"st", "nd", "rd", "th", NULL};

/* ----------
 * 标志与选项
 * ----------
 */
#define TH_UPPER		1
#define TH_LOWER		2

/* ----------
 * 数字描述结构体
 * ----------
 */
typedef struct
{
	int			pre,			/* 小数点前的位数 */
				post,			/* 小数点后的位数 */
				lsign,			/* 需要区域符号 */
				flag,			/* 数字参数	  */
				pre_lsign_num,	/* lsign 临时值		  */
				multi,			/* 'V' 的乘数		  */
				zero_start,		/* 首个零的位置		  */
				zero_end,		/* 末个零的位置		  */
				need_locale;	/* 需要区域		  */
} NUMDesc;

/* ----------
 * NUMBER 版的标志
 * ----------
 */
#define NUM_F_DECIMAL		(1 << 1)
#define NUM_F_LDECIMAL		(1 << 2)
#define NUM_F_ZERO			(1 << 3)
#define NUM_F_BLANK			(1 << 4)
#define NUM_F_FILLMODE		(1 << 5)
#define NUM_F_LSIGN			(1 << 6)
#define NUM_F_BRACKET		(1 << 7)
#define NUM_F_MINUS			(1 << 8)
#define NUM_F_PLUS			(1 << 9)
#define NUM_F_ROMAN			(1 << 10)
#define NUM_F_MULTI			(1 << 11)
#define NUM_F_PLUS_POST		(1 << 12)
#define NUM_F_MINUS_POST	(1 << 13)
#define NUM_F_EEEE			(1 << 14)

#define NUM_LSIGN_PRE	(-1)
#define NUM_LSIGN_POST	1
#define NUM_LSIGN_NONE	0

/* ----------
 * Tests
 * ----------
 */
#define IS_DECIMAL(_f)	((_f)->flag & NUM_F_DECIMAL)
#define IS_LDECIMAL(_f) ((_f)->flag & NUM_F_LDECIMAL)
#define IS_ZERO(_f) ((_f)->flag & NUM_F_ZERO)
#define IS_BLANK(_f)	((_f)->flag & NUM_F_BLANK)
#define IS_FILLMODE(_f) ((_f)->flag & NUM_F_FILLMODE)
#define IS_BRACKET(_f)	((_f)->flag & NUM_F_BRACKET)
#define IS_MINUS(_f)	((_f)->flag & NUM_F_MINUS)
#define IS_LSIGN(_f)	((_f)->flag & NUM_F_LSIGN)
#define IS_PLUS(_f) ((_f)->flag & NUM_F_PLUS)
#define IS_ROMAN(_f)	((_f)->flag & NUM_F_ROMAN)
#define IS_MULTI(_f)	((_f)->flag & NUM_F_MULTI)
#define IS_EEEE(_f)		((_f)->flag & NUM_F_EEEE)

/* ----------
 * 格式模式缓存
 *
 * 缓存最长 DCH_CACHE_SIZE 字节的日期时间格式模式；
 * 同样缓存最长 NUM_CACHE_SIZE 字节的数字格式模式。
 *
 * 简化起见，缓存条目为固定大小，故可容纳最坏情形
 * ——格式串的每个字节一个 FormatNode。
 *
 * CACHE_SIZE 常量的取值经过计算，使 sizeof(DCHCacheEntry)
 * 和 sizeof(NUMCacheEntry) 为 2 的幂或略小，这样
 * 单独 palloc 它们时不会浪费太多空间。
 * 若给这些结构体加字段，务必调整这些宏。
 *
 * 每个缓存的最大条目数为 DCH_CACHE_ENTRIES
 * 或 NUM_CACHE_ENTRIES。
 * ----------
 */
#define DCH_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int))
#define NUM_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int) + sizeof(NUMDesc))

#define DCH_CACHE_SIZE \
	((2048 - DCH_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)
#define NUM_CACHE_SIZE \
	((1024 - NUM_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)

#define DCH_CACHE_ENTRIES	20
#define NUM_CACHE_ENTRIES	20

typedef struct
{
	FormatNode	format[DCH_CACHE_SIZE + 1];
	char		str[DCH_CACHE_SIZE + 1];
	bool		std;
	bool		valid;
	int			age;
} DCHCacheEntry;

typedef struct
{
	FormatNode	format[NUM_CACHE_SIZE + 1];
	char		str[NUM_CACHE_SIZE + 1];
	bool		valid;
	int			age;
	NUMDesc		Num;
} NUMCacheEntry;

/* 日期/时间格式模式的全局缓存 */
static DCHCacheEntry *DCHCache[DCH_CACHE_ENTRIES];
static int	n_DCHCache = 0;		/* 当前条目数 */
static int	DCHCounter = 0;		/* 老化事件计数 */

/* 数字格式模式的全局缓存 */
static NUMCacheEntry *NUMCache[NUM_CACHE_ENTRIES];
static int	n_NUMCache = 0;		/* 当前条目数 */
static int	NUMCounter = 0;		/* 老化事件计数 */

/* ----------
 * 用于 char->date/time 转换
 * ----------
 */
typedef struct
{
	FromCharDateMode mode;
	int			hh,
				pm,
				mi,
				ss,
				ssss,
				d,				/* 存为 1-7，周日=1，0 表示缺失 */
				dd,
				ddd,
				mm,
				ms,
				year,
				bc,
				ww,
				w,
				cc,
				j,
				us,
				yysz,			/* YY 还是 YYYY？*/
				clock,			/* 12 或 24 小时制 */
				tzsign,			/* 无时区信息时为 +1、-1 或 0 */
				tzh,
				tzm,
				ff;				/* 小数部分精度 */
} TmFromChar;

#define ZERO_tmfc(_X) memset(_X, 0, sizeof(TmFromChar))

/* ----------
 * Debug
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_TMFC(_X) \
		elog(DEBUG_elog_output, "TMFC:\nmode %d\nhh %d\npm %d\nmi %d\nss %d\nssss %d\nd %d\ndd %d\nddd %d\nmm %d\nms: %d\nyear %d\nbc %d\nww %d\nw %d\ncc %d\nj %d\nus: %d\nyysz: %d\nclock: %d", \
			(_X)->mode, (_X)->hh, (_X)->pm, (_X)->mi, (_X)->ss, (_X)->ssss, \
			(_X)->d, (_X)->dd, (_X)->ddd, (_X)->mm, (_X)->ms, (_X)->year, \
			(_X)->bc, (_X)->ww, (_X)->w, (_X)->cc, (_X)->j, (_X)->us, \
			(_X)->yysz, (_X)->clock)
#define DEBUG_TM(_X) \
		elog(DEBUG_elog_output, "TM:\nsec %d\nyear %d\nmin %d\nwday %d\nhour %d\nyday %d\nmday %d\nnisdst %d\nmon %d\n",\
			(_X)->tm_sec, (_X)->tm_year,\
			(_X)->tm_min, (_X)->tm_wday, (_X)->tm_hour, (_X)->tm_yday,\
			(_X)->tm_mday, (_X)->tm_isdst, (_X)->tm_mon)
#else
#define DEBUG_TMFC(_X)
#define DEBUG_TM(_X)
#endif

/* ----------
 * 日期时间到 char 转换
 *
 * 为同时支持 interval 与 timestamp，我们使用自定义 "tm"
 * 结构体，近似 struct pg_tm，但有 64 位 tm_hour 字段。
 * 我们省略了此处不用的 tm_isdst 与 tm_zone 字段。
 * ----------
 */
struct fmt_tm
{
	int			tm_sec;
	int			tm_min;
	int64		tm_hour;
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
	int			tm_wday;
	int			tm_yday;
	long int	tm_gmtoff;
};

typedef struct TmToChar
{
	struct fmt_tm tm;			/* 近似经典的 'tm' 结构体 */
	fsec_t		fsec;			/* 小数秒 */
	const char *tzn;			/* 时区 */
} TmToChar;

#define tmtcTm(_X)	(&(_X)->tm)
#define tmtcTzn(_X) ((_X)->tzn)
#define tmtcFsec(_X)	((_X)->fsec)

/* 注意：用于把 pg_tm 复制到 fmt_tm，并非完全按位复制 */
#define COPY_tm(_DST, _SRC) \
do {	\
	(_DST)->tm_sec = (_SRC)->tm_sec; \
	(_DST)->tm_min = (_SRC)->tm_min; \
	(_DST)->tm_hour = (_SRC)->tm_hour; \
	(_DST)->tm_mday = (_SRC)->tm_mday; \
	(_DST)->tm_mon = (_SRC)->tm_mon; \
	(_DST)->tm_year = (_SRC)->tm_year; \
	(_DST)->tm_wday = (_SRC)->tm_wday; \
	(_DST)->tm_yday = (_SRC)->tm_yday; \
	(_DST)->tm_gmtoff = (_SRC)->tm_gmtoff; \
} while(0)

/* 当心：用于把 pg_tm 与 fmt_tm 结构体都清零 */
#define ZERO_tm(_X) \
do {	\
	memset(_X, 0, sizeof(*(_X))); \
	(_X)->tm_mday = (_X)->tm_mon = 1; \
} while(0)

#define ZERO_tmtc(_X) \
do { \
	ZERO_tm( tmtcTm(_X) ); \
	tmtcFsec(_X) = 0; \
	tmtcTzn(_X) = NULL; \
} while(0)

/*
 *	to_char(time) 在 to_char() 看来是 interval，所以此检查
 *	实为 interval 与 time 类型而设。
 */
#define INVALID_FOR_INTERVAL  \
do { \
	if (is_interval) \
		ereport(ERROR, \
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT), \
				 errmsg("invalid format specification for an interval value"), \
				 errhint("Intervals are not tied to specific calendar dates."))); \
} while(0)

/*****************************************************************************
 *			关键字定义
 *****************************************************************************/

/* ----------
 * 后缀（FormatNode.suffix 为这些码的 OR）
 * ----------
 */
#define DCH_S_FM	0x01
#define DCH_S_TH	0x02
#define DCH_S_th	0x04
#define DCH_S_SP	0x08
#define DCH_S_TM	0x10

/* ----------
 * 后缀测试
 * ----------
 */
#define S_THth(_s)	((((_s) & DCH_S_TH) || ((_s) & DCH_S_th)) ? 1 : 0)
#define S_TH(_s)	(((_s) & DCH_S_TH) ? 1 : 0)
#define S_th(_s)	(((_s) & DCH_S_th) ? 1 : 0)
#define S_TH_TYPE(_s)	(((_s) & DCH_S_TH) ? TH_UPPER : TH_LOWER)

/* Oracle 切换 FM，我们不；见文档。 */
#define S_FM(_s)	(((_s) & DCH_S_FM) ? 1 : 0)
#define S_SP(_s)	(((_s) & DCH_S_SP) ? 1 : 0)
#define S_TM(_s)	(((_s) & DCH_S_TM) ? 1 : 0)

/* ----------
 * DATE-TIME TO/FROM CHAR 的后缀定义
 * ----------
 */
#define TM_SUFFIX_LEN	2

static const KeySuffix DCH_suff[] = {
	{"FM", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"fm", 2, DCH_S_FM, SUFFTYPE_PREFIX},
	{"TM", TM_SUFFIX_LEN, DCH_S_TM, SUFFTYPE_PREFIX},
	{"tm", 2, DCH_S_TM, SUFFTYPE_PREFIX},
	{"TH", 2, DCH_S_TH, SUFFTYPE_POSTFIX},
	{"th", 2, DCH_S_th, SUFFTYPE_POSTFIX},
	{"SP", 2, DCH_S_SP, SUFFTYPE_POSTFIX},
	/* last */
	{NULL, 0, 0, 0}
};


/* ----------
 * 格式模式 (KeyWord)。
 *
 * KeyWord 字段按字母排序，*但*相似串的排序按
 *		  由难到易：
 *
 *	(示例："DDD","DD","Day","D")
 *
 * (这种排序需要串的顺序搜索算法来处理——匹配没有
 * 精确终点；→ "HH12blabla" 中的关键字是什么？："HH"
 * 还是 "HH12"？必须先试 "HH12"，因为 "HH" 在串中，
 * 但那不行。
 *
 * (!)
 *	 - 关键字的位置与 DCH/NUM_poz 枚举中的位置相对应。
 * (!)
 *
 * 快速搜索使用 'int index[]'，该索引是从位置 32 (' ') 到
 * 126 (~) 的 ASCII 表，索引中每个 ASCII 位置都存有 DCH_ /
 * NUM_ 值，若字符未被 KeyWord 使用则为 -1。搜索示例：
 * "MM" 串：
 *	1)	查 index['M' - 32]，
 *	2)	从索引取关键字位置（枚举 DCH_MI）
 *	3)	从该位置起在 keywords[] 中顺序搜索
 *
 * ----------
 */

typedef enum
{
	DCH_A_D,
	DCH_A_M,
	DCH_AD,
	DCH_AM,
	DCH_B_C,
	DCH_BC,
	DCH_CC,
	DCH_DAY,
	DCH_DDD,
	DCH_DD,
	DCH_DY,
	DCH_Day,
	DCH_Dy,
	DCH_D,
	DCH_FF1,					/* FFn 代码必须连续 */
	DCH_FF2,
	DCH_FF3,
	DCH_FF4,
	DCH_FF5,
	DCH_FF6,
	DCH_FX,						/* 全局后缀 */
	DCH_HH24,
	DCH_HH12,
	DCH_HH,
	DCH_IDDD,
	DCH_ID,
	DCH_IW,
	DCH_IYYY,
	DCH_IYY,
	DCH_IY,
	DCH_I,
	DCH_J,
	DCH_MI,
	DCH_MM,
	DCH_MONTH,
	DCH_MON,
	DCH_MS,
	DCH_Month,
	DCH_Mon,
	DCH_OF,
	DCH_P_M,
	DCH_PM,
	DCH_Q,
	DCH_RM,
	DCH_SSSSS,
	DCH_SSSS,
	DCH_SS,
	DCH_TZH,
	DCH_TZM,
	DCH_TZ,
	DCH_US,
	DCH_WW,
	DCH_W,
	DCH_Y_YYY,
	DCH_YYYY,
	DCH_YYY,
	DCH_YY,
	DCH_Y,
	DCH_a_d,
	DCH_a_m,
	DCH_ad,
	DCH_am,
	DCH_b_c,
	DCH_bc,
	DCH_cc,
	DCH_day,
	DCH_ddd,
	DCH_dd,
	DCH_dy,
	DCH_d,
	DCH_ff1,
	DCH_ff2,
	DCH_ff3,
	DCH_ff4,
	DCH_ff5,
	DCH_ff6,
	DCH_fx,
	DCH_hh24,
	DCH_hh12,
	DCH_hh,
	DCH_iddd,
	DCH_id,
	DCH_iw,
	DCH_iyyy,
	DCH_iyy,
	DCH_iy,
	DCH_i,
	DCH_j,
	DCH_mi,
	DCH_mm,
	DCH_month,
	DCH_mon,
	DCH_ms,
	DCH_of,
	DCH_p_m,
	DCH_pm,
	DCH_q,
	DCH_rm,
	DCH_sssss,
	DCH_ssss,
	DCH_ss,
	DCH_tzh,
	DCH_tzm,
	DCH_tz,
	DCH_us,
	DCH_ww,
	DCH_w,
	DCH_y_yyy,
	DCH_yyyy,
	DCH_yyy,
	DCH_yy,
	DCH_y,

	/* last */
	_DCH_last_
}			DCH_poz;

typedef enum
{
	NUM_COMMA,
	NUM_DEC,
	NUM_0,
	NUM_9,
	NUM_B,
	NUM_C,
	NUM_D,
	NUM_E,
	NUM_FM,
	NUM_G,
	NUM_L,
	NUM_MI,
	NUM_PL,
	NUM_PR,
	NUM_RN,
	NUM_SG,
	NUM_SP,
	NUM_S,
	NUM_TH,
	NUM_V,
	NUM_b,
	NUM_c,
	NUM_d,
	NUM_e,
	NUM_fm,
	NUM_g,
	NUM_l,
	NUM_mi,
	NUM_pl,
	NUM_pr,
	NUM_rn,
	NUM_sg,
	NUM_sp,
	NUM_s,
	NUM_th,
	NUM_v,

	/* last */
	_NUM_last_
}			NUM_poz;

/* ----------
 * DATE-TIME 版的 KeyWords
 * ----------
 */
static const KeyWord DCH_keywords[] = {
/*	name, len, id, is_digit, date_mode */
	{"A.D.", 4, DCH_A_D, false, FROM_CHAR_DATE_NONE},	/* A */
	{"A.M.", 4, DCH_A_M, false, FROM_CHAR_DATE_NONE},
	{"AD", 2, DCH_AD, false, FROM_CHAR_DATE_NONE},
	{"AM", 2, DCH_AM, false, FROM_CHAR_DATE_NONE},
	{"B.C.", 4, DCH_B_C, false, FROM_CHAR_DATE_NONE},	/* B */
	{"BC", 2, DCH_BC, false, FROM_CHAR_DATE_NONE},
	{"CC", 2, DCH_CC, true, FROM_CHAR_DATE_NONE},	/* C */
	{"DAY", 3, DCH_DAY, false, FROM_CHAR_DATE_NONE},	/* D */
	{"DDD", 3, DCH_DDD, true, FROM_CHAR_DATE_GREGORIAN},
	{"DD", 2, DCH_DD, true, FROM_CHAR_DATE_GREGORIAN},
	{"DY", 2, DCH_DY, false, FROM_CHAR_DATE_NONE},
	{"Day", 3, DCH_Day, false, FROM_CHAR_DATE_NONE},
	{"Dy", 2, DCH_Dy, false, FROM_CHAR_DATE_NONE},
	{"D", 1, DCH_D, true, FROM_CHAR_DATE_GREGORIAN},
	{"FF1", 3, DCH_FF1, true, FROM_CHAR_DATE_NONE}, /* F */
	{"FF2", 3, DCH_FF2, true, FROM_CHAR_DATE_NONE},
	{"FF3", 3, DCH_FF3, true, FROM_CHAR_DATE_NONE},
	{"FF4", 3, DCH_FF4, true, FROM_CHAR_DATE_NONE},
	{"FF5", 3, DCH_FF5, true, FROM_CHAR_DATE_NONE},
	{"FF6", 3, DCH_FF6, true, FROM_CHAR_DATE_NONE},
	{"FX", 2, DCH_FX, false, FROM_CHAR_DATE_NONE},
	{"HH24", 4, DCH_HH24, true, FROM_CHAR_DATE_NONE},	/* H */
	{"HH12", 4, DCH_HH12, true, FROM_CHAR_DATE_NONE},
	{"HH", 2, DCH_HH, true, FROM_CHAR_DATE_NONE},
	{"IDDD", 4, DCH_IDDD, true, FROM_CHAR_DATE_ISOWEEK},	/* I */
	{"ID", 2, DCH_ID, true, FROM_CHAR_DATE_ISOWEEK},
	{"IW", 2, DCH_IW, true, FROM_CHAR_DATE_ISOWEEK},
	{"IYYY", 4, DCH_IYYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"IYY", 3, DCH_IYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"IY", 2, DCH_IY, true, FROM_CHAR_DATE_ISOWEEK},
	{"I", 1, DCH_I, true, FROM_CHAR_DATE_ISOWEEK},
	{"J", 1, DCH_J, true, FROM_CHAR_DATE_NONE}, /* J */
	{"MI", 2, DCH_MI, true, FROM_CHAR_DATE_NONE},	/* M */
	{"MM", 2, DCH_MM, true, FROM_CHAR_DATE_GREGORIAN},
	{"MONTH", 5, DCH_MONTH, false, FROM_CHAR_DATE_GREGORIAN},
	{"MON", 3, DCH_MON, false, FROM_CHAR_DATE_GREGORIAN},
	{"MS", 2, DCH_MS, true, FROM_CHAR_DATE_NONE},
	{"Month", 5, DCH_Month, false, FROM_CHAR_DATE_GREGORIAN},
	{"Mon", 3, DCH_Mon, false, FROM_CHAR_DATE_GREGORIAN},
	{"OF", 2, DCH_OF, false, FROM_CHAR_DATE_NONE},	/* O */
	{"P.M.", 4, DCH_P_M, false, FROM_CHAR_DATE_NONE},	/* P */
	{"PM", 2, DCH_PM, false, FROM_CHAR_DATE_NONE},
	{"Q", 1, DCH_Q, true, FROM_CHAR_DATE_NONE}, /* Q */
	{"RM", 2, DCH_RM, false, FROM_CHAR_DATE_GREGORIAN}, /* R */
	{"SSSSS", 5, DCH_SSSS, true, FROM_CHAR_DATE_NONE},	/* S */
	{"SSSS", 4, DCH_SSSS, true, FROM_CHAR_DATE_NONE},
	{"SS", 2, DCH_SS, true, FROM_CHAR_DATE_NONE},
	{"TZH", 3, DCH_TZH, false, FROM_CHAR_DATE_NONE},	/* T */
	{"TZM", 3, DCH_TZM, true, FROM_CHAR_DATE_NONE},
	{"TZ", 2, DCH_TZ, false, FROM_CHAR_DATE_NONE},
	{"US", 2, DCH_US, true, FROM_CHAR_DATE_NONE},	/* U */
	{"WW", 2, DCH_WW, true, FROM_CHAR_DATE_GREGORIAN},	/* W */
	{"W", 1, DCH_W, true, FROM_CHAR_DATE_GREGORIAN},
	{"Y,YYY", 5, DCH_Y_YYY, true, FROM_CHAR_DATE_GREGORIAN},	/* Y */
	{"YYYY", 4, DCH_YYYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"YYY", 3, DCH_YYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"YY", 2, DCH_YY, true, FROM_CHAR_DATE_GREGORIAN},
	{"Y", 1, DCH_Y, true, FROM_CHAR_DATE_GREGORIAN},
	{"a.d.", 4, DCH_a_d, false, FROM_CHAR_DATE_NONE},	/* a */
	{"a.m.", 4, DCH_a_m, false, FROM_CHAR_DATE_NONE},
	{"ad", 2, DCH_ad, false, FROM_CHAR_DATE_NONE},
	{"am", 2, DCH_am, false, FROM_CHAR_DATE_NONE},
	{"b.c.", 4, DCH_b_c, false, FROM_CHAR_DATE_NONE},	/* b */
	{"bc", 2, DCH_bc, false, FROM_CHAR_DATE_NONE},
	{"cc", 2, DCH_CC, true, FROM_CHAR_DATE_NONE},	/* c */
	{"day", 3, DCH_day, false, FROM_CHAR_DATE_NONE},	/* d */
	{"ddd", 3, DCH_DDD, true, FROM_CHAR_DATE_GREGORIAN},
	{"dd", 2, DCH_DD, true, FROM_CHAR_DATE_GREGORIAN},
	{"dy", 2, DCH_dy, false, FROM_CHAR_DATE_NONE},
	{"d", 1, DCH_D, true, FROM_CHAR_DATE_GREGORIAN},
	{"ff1", 3, DCH_FF1, true, FROM_CHAR_DATE_NONE}, /* f */
	{"ff2", 3, DCH_FF2, true, FROM_CHAR_DATE_NONE},
	{"ff3", 3, DCH_FF3, true, FROM_CHAR_DATE_NONE},
	{"ff4", 3, DCH_FF4, true, FROM_CHAR_DATE_NONE},
	{"ff5", 3, DCH_FF5, true, FROM_CHAR_DATE_NONE},
	{"ff6", 3, DCH_FF6, true, FROM_CHAR_DATE_NONE},
	{"fx", 2, DCH_FX, false, FROM_CHAR_DATE_NONE},
	{"hh24", 4, DCH_HH24, true, FROM_CHAR_DATE_NONE},	/* h */
	{"hh12", 4, DCH_HH12, true, FROM_CHAR_DATE_NONE},
	{"hh", 2, DCH_HH, true, FROM_CHAR_DATE_NONE},
	{"iddd", 4, DCH_IDDD, true, FROM_CHAR_DATE_ISOWEEK},	/* i */
	{"id", 2, DCH_ID, true, FROM_CHAR_DATE_ISOWEEK},
	{"iw", 2, DCH_IW, true, FROM_CHAR_DATE_ISOWEEK},
	{"iyyy", 4, DCH_IYYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"iyy", 3, DCH_IYY, true, FROM_CHAR_DATE_ISOWEEK},
	{"iy", 2, DCH_IY, true, FROM_CHAR_DATE_ISOWEEK},
	{"i", 1, DCH_I, true, FROM_CHAR_DATE_ISOWEEK},
	{"j", 1, DCH_J, true, FROM_CHAR_DATE_NONE}, /* j */
	{"mi", 2, DCH_MI, true, FROM_CHAR_DATE_NONE},	/* m */
	{"mm", 2, DCH_MM, true, FROM_CHAR_DATE_GREGORIAN},
	{"month", 5, DCH_month, false, FROM_CHAR_DATE_GREGORIAN},
	{"mon", 3, DCH_mon, false, FROM_CHAR_DATE_GREGORIAN},
	{"ms", 2, DCH_MS, true, FROM_CHAR_DATE_NONE},
	{"of", 2, DCH_OF, false, FROM_CHAR_DATE_NONE},	/* o */
	{"p.m.", 4, DCH_p_m, false, FROM_CHAR_DATE_NONE},	/* p */
	{"pm", 2, DCH_pm, false, FROM_CHAR_DATE_NONE},
	{"q", 1, DCH_Q, true, FROM_CHAR_DATE_NONE}, /* q */
	{"rm", 2, DCH_rm, false, FROM_CHAR_DATE_GREGORIAN}, /* r */
	{"sssss", 5, DCH_SSSS, true, FROM_CHAR_DATE_NONE},	/* s */
	{"ssss", 4, DCH_SSSS, true, FROM_CHAR_DATE_NONE},
	{"ss", 2, DCH_SS, true, FROM_CHAR_DATE_NONE},
	{"tzh", 3, DCH_TZH, false, FROM_CHAR_DATE_NONE},	/* t */
	{"tzm", 3, DCH_TZM, true, FROM_CHAR_DATE_NONE},
	{"tz", 2, DCH_tz, false, FROM_CHAR_DATE_NONE},
	{"us", 2, DCH_US, true, FROM_CHAR_DATE_NONE},	/* u */
	{"ww", 2, DCH_WW, true, FROM_CHAR_DATE_GREGORIAN},	/* w */
	{"w", 1, DCH_W, true, FROM_CHAR_DATE_GREGORIAN},
	{"y,yyy", 5, DCH_Y_YYY, true, FROM_CHAR_DATE_GREGORIAN},	/* y */
	{"yyyy", 4, DCH_YYYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"yyy", 3, DCH_YYY, true, FROM_CHAR_DATE_GREGORIAN},
	{"yy", 2, DCH_YY, true, FROM_CHAR_DATE_GREGORIAN},
	{"y", 1, DCH_Y, true, FROM_CHAR_DATE_GREGORIAN},

	/* last */
	{NULL, 0, 0, 0, 0}
};

/* ----------
 * NUMBER 版的 KeyWords
 *
 * is_digit 与 date_mode 字段在此处不相关。
 * ----------
 */
static const KeyWord NUM_keywords[] = {
/*	name, len, id			is in Index */
	{",", 1, NUM_COMMA},		/* , */
	{".", 1, NUM_DEC},			/* . */
	{"0", 1, NUM_0},			/* 0 */
	{"9", 1, NUM_9},			/* 9 */
	{"B", 1, NUM_B},			/* B */
	{"C", 1, NUM_C},			/* C */
	{"D", 1, NUM_D},			/* D */
	{"EEEE", 4, NUM_E},			/* E */
	{"FM", 2, NUM_FM},			/* F */
	{"G", 1, NUM_G},			/* G */
	{"L", 1, NUM_L},			/* L */
	{"MI", 2, NUM_MI},			/* M */
	{"PL", 2, NUM_PL},			/* P */
	{"PR", 2, NUM_PR},
	{"RN", 2, NUM_RN},			/* R */
	{"SG", 2, NUM_SG},			/* S */
	{"SP", 2, NUM_SP},
	{"S", 1, NUM_S},
	{"TH", 2, NUM_TH},			/* T */
	{"V", 1, NUM_V},			/* V */
	{"b", 1, NUM_B},			/* b */
	{"c", 1, NUM_C},			/* c */
	{"d", 1, NUM_D},			/* d */
	{"eeee", 4, NUM_E},			/* e */
	{"fm", 2, NUM_FM},			/* f */
	{"g", 1, NUM_G},			/* g */
	{"l", 1, NUM_L},			/* l */
	{"mi", 2, NUM_MI},			/* m */
	{"pl", 2, NUM_PL},			/* p */
	{"pr", 2, NUM_PR},
	{"rn", 2, NUM_rn},			/* r */
	{"sg", 2, NUM_SG},			/* s */
	{"sp", 2, NUM_SP},
	{"s", 1, NUM_S},
	{"th", 2, NUM_th},			/* t */
	{"v", 1, NUM_V},			/* v */

	/* last */
	{NULL, 0, 0}
};


/* ----------
 * DATE-TIME 版的 KeyWords 索引
 * ----------
 */
static const int DCH_index[KeyWord_INDEX_SIZE] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
	/*---- 前 0..31 个字符被跳过 ----*/

	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, DCH_A_D, DCH_B_C, DCH_CC, DCH_DAY, -1,
	DCH_FF1, -1, DCH_HH24, DCH_IDDD, DCH_J, -1, -1, DCH_MI, -1, DCH_OF,
	DCH_P_M, DCH_Q, DCH_RM, DCH_SSSSS, DCH_TZH, DCH_US, -1, DCH_WW, -1, DCH_Y_YYY,
	-1, -1, -1, -1, -1, -1, -1, DCH_a_d, DCH_b_c, DCH_cc,
	DCH_day, -1, DCH_ff1, -1, DCH_hh24, DCH_iddd, DCH_j, -1, -1, DCH_mi,
	-1, DCH_of, DCH_p_m, DCH_q, DCH_rm, DCH_sssss, DCH_tzh, DCH_us, -1, DCH_ww,
	-1, DCH_y_yyy, -1, -1, -1, -1

	/*---- 大于 126 的字符跳过 ----*/
};

/* ----------
 * NUMBER 版的 KeyWords 索引
 * ----------
 */
static const int NUM_index[KeyWord_INDEX_SIZE] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
	/*---- 前 0..31 个字符被跳过 ----*/

	-1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, NUM_COMMA, -1, NUM_DEC, -1, NUM_0, -1,
	-1, -1, -1, -1, -1, -1, -1, NUM_9, -1, -1,
	-1, -1, -1, -1, -1, -1, NUM_B, NUM_C, NUM_D, NUM_E,
	NUM_FM, NUM_G, -1, -1, -1, -1, NUM_L, NUM_MI, -1, -1,
	NUM_PL, -1, NUM_RN, NUM_SG, NUM_TH, -1, NUM_V, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, NUM_b, NUM_c,
	NUM_d, NUM_e, NUM_fm, NUM_g, -1, -1, -1, -1, NUM_l, NUM_mi,
	-1, -1, NUM_pl, -1, NUM_rn, NUM_sg, NUM_th, -1, NUM_v, -1,
	-1, -1, -1, -1, -1, -1

	/*---- 大于 126 的字符跳过 ----*/
};

/* ----------
 * 数字处理结构体
 * ----------
 */
typedef struct NUMProc
{
	bool		is_to_char;
	NUMDesc    *Num;			/* 数字描述		*/

	int			sign,			/* '-' 或 '+'		*/
				sign_wrote,		/* 已写符号		*/
				num_count,		/* 已写数字个数	*/
				num_in,			/* 位于数字内		*/
				num_curr,		/* 数字中当前位置	*/
				out_pre_spaces, /* 首数字前的空格	*/

				read_dec,		/* to_number - 已读小数点	*/
				read_post,		/* to_number - 小数位个数 */
				read_pre;		/* to_number - 非小数位个数 */

	char	   *number,			/* 数字串	*/
			   *number_p,		/* 当前数字位置的指针 */
			   *inout,			/* 输入/输出	*/
			   *inout_p,		/* 当前 inout 位置的指针 */
			   *last_relevant,	/* 小数点后的最后有效数字 */

			   *L_negative_sign,	/* Locale */
			   *L_positive_sign,
			   *decimal,
			   *L_thousands_sep,
			   *L_currency_symbol;
} NUMProc;

/* DCH_from_char() 的返回标志 */
#define DCH_DATED	0x01
#define DCH_TIMED	0x02
#define DCH_ZONED	0x04

/* ----------
 * 函数
 * ----------
 */
static const KeyWord *index_seq_search(const char *str, const KeyWord *kw,
									   const int *index);
static const KeySuffix *suff_search(const char *str, const KeySuffix *suf, int type);
static bool is_separator_char(const char *str);
static void NUMDesc_prepare(NUMDesc *num, FormatNode *n);
static void parse_format(FormatNode *node, const char *str, const KeyWord *kw,
						 const KeySuffix *suf, const int *index, uint32 flags, NUMDesc *Num);

static void DCH_to_char(FormatNode *node, bool is_interval,
						TmToChar *in, char *out, Oid collid);
static void DCH_from_char(FormatNode *node, const char *in, TmFromChar *out,
						  Oid collid, bool std, Node *escontext);

#ifdef DEBUG_TO_FROM_CHAR
static void dump_index(const KeyWord *k, const int *index);
static void dump_node(FormatNode *node, int max);
#endif

static const char *get_th(char *num, int type);
static char *str_numth(char *dest, char *num, int type);
static int	adjust_partial_year_to_2020(int year);
static int	strspace_len(const char *str);
static bool from_char_set_mode(TmFromChar *tmfc, const FromCharDateMode mode,
							   Node *escontext);
static bool from_char_set_int(int *dest, const int value, const FormatNode *node,
							  Node *escontext);
static int	from_char_parse_int_len(int *dest, const char **src, const int len,
									FormatNode *node, Node *escontext);
static int	from_char_parse_int(int *dest, const char **src, FormatNode *node,
								Node *escontext);
static int	seq_search_ascii(const char *name, const char *const *array, int *len);
static int	seq_search_localized(const char *name, char **array, int *len,
								 Oid collid);
static bool from_char_seq_search(int *dest, const char **src,
								 const char *const *array,
								 char **localized_array, Oid collid,
								 FormatNode *node, Node *escontext);
static bool do_to_timestamp(text *date_txt, text *fmt, Oid collid, bool std,
							struct pg_tm *tm, fsec_t *fsec, int *fprec,
							uint32 *flags, Node *escontext);
static char *fill_str(char *str, int c, int max);
static FormatNode *NUM_cache(int len, NUMDesc *Num, text *pars_str, bool *shouldFree);
static char *int_to_roman(int number);
static void NUM_prepare_locale(NUMProc *Np);
static char *get_last_relevant_decnum(char *num);
static void NUM_numpart_from_char(NUMProc *Np, int id, int input_len);
static void NUM_numpart_to_char(NUMProc *Np, int id);
static char *NUM_processor(FormatNode *node, NUMDesc *Num, char *inout,
						   char *number, int input_len, int to_char_out_pre_spaces,
						   int sign, bool is_to_char, Oid collid);
static DCHCacheEntry *DCH_cache_getnew(const char *str, bool std);
static DCHCacheEntry *DCH_cache_search(const char *str, bool std);
static DCHCacheEntry *DCH_cache_fetch(const char *str, bool std);
static NUMCacheEntry *NUM_cache_getnew(const char *str);
static NUMCacheEntry *NUM_cache_search(const char *str);
static NUMCacheEntry *NUM_cache_fetch(const char *str);


/* ----------
 * 快速顺序搜索，用索引预筛进入循环的数据
 * （对不需要的串非常快）
 * （格式解析中无法用二分搜索）
 * ----------
 */
static const KeyWord *
index_seq_search(const char *str, const KeyWord *kw, const int *index)
{
	int			poz;

	if (!KeyWord_INDEX_FILTER(*str))
		return NULL;

	if ((poz = *(index + (*str - ' '))) > -1)
	{
		const KeyWord *k = kw + poz;

		do
		{
			if (strncmp(str, k->name, k->len) == 0)
				return k;
			k++;
			if (!k->name)
				return NULL;
		} while (*str == *k->name);
	}
	return NULL;
}

static const KeySuffix *
suff_search(const char *str, const KeySuffix *suf, int type)
{
	const KeySuffix *s;

	for (s = suf; s->name != NULL; s++)
	{
		if (s->type != type)
			continue;

		if (strncmp(str, s->name, s->len) == 0)
			return s;
	}
	return NULL;
}

static bool
is_separator_char(const char *str)
{
	/* ASCII 可打印字符，但非字母或数字 */
	return (*str > 0x20 && *str < 0x7F &&
			!(*str >= 'A' && *str <= 'Z') &&
			!(*str >= 'a' && *str <= 'z') &&
			!(*str >= '0' && *str <= '9'));
}

/* ----------
 * 用 FormatNode 结构体准备 NUMDesc（数字描述结构）
 * ----------
 */
static void
NUMDesc_prepare(NUMDesc *num, FormatNode *n)
{
	if (n->type != NODE_TYPE_ACTION)
		return;

	if (IS_EEEE(num) && n->key->id != NUM_E)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("\"EEEE\" must be the last pattern used")));

	switch (n->key->id)
	{
		case NUM_9:
			if (IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"9\" must be ahead of \"PR\"")));
			if (IS_MULTI(num))
			{
				++num->multi;
				break;
			}
			if (IS_DECIMAL(num))
				++num->post;
			else
				++num->pre;
			break;

		case NUM_0:
			if (IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"0\" must be ahead of \"PR\"")));
			if (!IS_ZERO(num) && !IS_DECIMAL(num))
			{
				num->flag |= NUM_F_ZERO;
				num->zero_start = num->pre + 1;
			}
			if (!IS_DECIMAL(num))
				++num->pre;
			else
				++num->post;

			num->zero_end = num->pre + num->post;
			break;

		case NUM_B:
			if (num->pre == 0 && num->post == 0 && (!IS_ZERO(num)))
				num->flag |= NUM_F_BLANK;
			break;

		case NUM_D:
			num->flag |= NUM_F_LDECIMAL;
			num->need_locale = true;
			/* FALLTHROUGH */
		case NUM_DEC:
			if (IS_DECIMAL(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("multiple decimal points")));
			if (IS_MULTI(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"V\" and decimal point together")));
			num->flag |= NUM_F_DECIMAL;
			break;

		case NUM_FM:
			num->flag |= NUM_F_FILLMODE;
			break;

		case NUM_S:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" twice")));
			if (IS_PLUS(num) || IS_MINUS(num) || IS_BRACKET(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\"/\"MI\"/\"SG\"/\"PR\" together")));
			if (!IS_DECIMAL(num))
			{
				num->lsign = NUM_LSIGN_PRE;
				num->pre_lsign_num = num->pre;
				num->need_locale = true;
				num->flag |= NUM_F_LSIGN;
			}
			else if (num->lsign == NUM_LSIGN_NONE)
			{
				num->lsign = NUM_LSIGN_POST;
				num->need_locale = true;
				num->flag |= NUM_F_LSIGN;
			}
			break;

		case NUM_MI:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"MI\" together")));
			num->flag |= NUM_F_MINUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_MINUS_POST;
			break;

		case NUM_PL:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"PL\" together")));
			num->flag |= NUM_F_PLUS;
			if (IS_DECIMAL(num))
				num->flag |= NUM_F_PLUS_POST;
			break;

		case NUM_SG:
			if (IS_LSIGN(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"S\" and \"SG\" together")));
			num->flag |= NUM_F_MINUS;
			num->flag |= NUM_F_PLUS;
			break;

		case NUM_PR:
			if (IS_LSIGN(num) || IS_PLUS(num) || IS_MINUS(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"PR\" and \"S\"/\"PL\"/\"MI\"/\"SG\" together")));
			num->flag |= NUM_F_BRACKET;
			break;

		case NUM_rn:
		case NUM_RN:
			num->flag |= NUM_F_ROMAN;
			break;

		case NUM_L:
		case NUM_G:
			num->need_locale = true;
			break;

		case NUM_V:
			if (IS_DECIMAL(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"V\" and decimal point together")));
			num->flag |= NUM_F_MULTI;
			break;

		case NUM_E:
			if (IS_EEEE(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use \"EEEE\" twice")));
			if (IS_BLANK(num) || IS_FILLMODE(num) || IS_LSIGN(num) ||
				IS_BRACKET(num) || IS_MINUS(num) || IS_PLUS(num) ||
				IS_ROMAN(num) || IS_MULTI(num))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"EEEE\" is incompatible with other formats"),
						 errdetail("\"EEEE\" may only be used together with digit and decimal point patterns.")));
			num->flag |= NUM_F_EEEE;
			break;
	}
}

/* ----------
 * 格式解析器：查找小关键字与关键字后缀并构建格式
 * 节点树。
 *
 * DATE-TIME & NUMBER 版适用
 * ----------
 */
static void
parse_format(FormatNode *node, const char *str, const KeyWord *kw,
			 const KeySuffix *suf, const int *index, uint32 flags, NUMDesc *Num)
{
	FormatNode *n;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "to_char/number(): run parser");
#endif

	n = node;

	while (*str)
	{
		int			suffix = 0;
		const KeySuffix *s;

		/*
		 * 前缀
		 */
		if ((flags & DCH_FLAG) &&
			(s = suff_search(str, suf, SUFFTYPE_PREFIX)) != NULL)
		{
			suffix |= s->id;
			if (s->len)
				str += s->len;
		}

		/*
		 * Keyword
		 */
		if (*str && (n->key = index_seq_search(str, kw, index)) != NULL)
		{
			n->type = NODE_TYPE_ACTION;
			n->suffix = suffix;
			if (n->key->len)
				str += n->key->len;

			/*
			 * NUM 版：准备全局 NUMDesc 结构体
			 */
			if (flags & NUM_FLAG)
				NUMDesc_prepare(Num, n);

			/*
			 * 后缀
			 */
			if ((flags & DCH_FLAG) && *str &&
				(s = suff_search(str, suf, SUFFTYPE_POSTFIX)) != NULL)
			{
				n->suffix |= s->id;
				if (s->len)
					str += s->len;
			}

			n++;
		}
		else if (*str)
		{
			int			chlen;

			if ((flags & STD_FLAG) && *str != '"')
			{
				/*
				 * 标准模式，只允许以下分隔符："-./,':; "。
				 * 但即使在标准模式我们也支持双引号
				 * （见下）。此为我们对标准模式扩展。
				 */
				if (strchr("-./,':; ", *str) == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("invalid datetime format separator: \"%s\"",
									pnstrdup(str, pg_mblen(str)))));

				if (*str == ' ')
					n->type = NODE_TYPE_SPACE;
				else
					n->type = NODE_TYPE_SEPARATOR;

				n->character[0] = *str;
				n->character[1] = '\0';
				n->key = NULL;
				n->suffix = 0;
				n++;
				str++;
			}
			else if (*str == '"')
			{
				/*
				 * 若有双引号括起的字面串则处理
				 */
				str++;
				while (*str)
				{
					if (*str == '"')
					{
						str++;
						break;
					}
					/* 反斜杠转义后一个字符（如有） */
					if (*str == '\\' && *(str + 1))
						str++;
					chlen = pg_mblen(str);
					n->type = NODE_TYPE_CHAR;
					memcpy(n->character, str, chlen);
					n->character[chlen] = '\0';
					n->key = NULL;
					n->suffix = 0;
					n++;
					str += chlen;
				}
			}
			else
			{
				/*
				 * 在双引号串之外，反斜杠仅在其后
				 * 紧邻双引号时才特殊。
				 */
				if (*str == '\\' && *(str + 1) == '"')
					str++;
				chlen = pg_mblen(str);

				if ((flags & DCH_FLAG) && is_separator_char(str))
					n->type = NODE_TYPE_SEPARATOR;
				else if (isspace((unsigned char) *str))
					n->type = NODE_TYPE_SPACE;
				else
					n->type = NODE_TYPE_CHAR;

				memcpy(n->character, str, chlen);
				n->character[chlen] = '\0';
				n->key = NULL;
				n->suffix = 0;
				n++;
				str += chlen;
			}
		}
	}

	n->type = NODE_TYPE_END;
	n->suffix = 0;
}

/* ----------
 * DEBUG：转储 FormatNode 树(调试)
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR

#define DUMP_THth(_suf) (S_TH(_suf) ? "TH" : (S_th(_suf) ? "th" : " "))
#define DUMP_FM(_suf)	(S_FM(_suf) ? "FM" : " ")

static void
dump_node(FormatNode *node, int max)
{
	FormatNode *n;
	int			a;

	elog(DEBUG_elog_output, "to_from-char(): DUMP FORMAT");

	for (a = 0, n = node; a <= max; n++, a++)
	{
		if (n->type == NODE_TYPE_ACTION)
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_ACTION '%s'\t(%s,%s)",
				 a, n->key->name, DUMP_THth(n->suffix), DUMP_FM(n->suffix));
		else if (n->type == NODE_TYPE_CHAR)
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_CHAR '%s'",
				 a, n->character);
		else if (n->type == NODE_TYPE_END)
		{
			elog(DEBUG_elog_output, "%d:\t NODE_TYPE_END", a);
			return;
		}
		else
			elog(DEBUG_elog_output, "%d:\t unknown NODE!", a);
	}
}
#endif							/* DEBUG */

/*****************************************************************************
 *			私有工具
 *****************************************************************************/

/* ----------
 * 为简单数字（1..9）返回 ST/ND/RD/TH
 * type 0=大写 1=小写
 * ----------
 */
static const char *
get_th(char *num, int type)
{
	int			len = strlen(num),
				last;

	last = *(num + (len - 1));
	if (!isdigit((unsigned char) last))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("\"%s\" is not a number", num)));

	/*
	 * 所有 "teens"（<x>1[0-9]）得到 'TH/th'，而 <x>[02-9][123]
	 * 仍分别取'ST/st'、'ND/nd'、'RD/rd'
	 */
	if ((len > 1) && (num[len - 2] == '1'))
		last = 0;

	switch (last)
	{
		case '1':
			if (type == TH_UPPER)
				return numTH[0];
			return numth[0];
		case '2':
			if (type == TH_UPPER)
				return numTH[1];
			return numth[1];
		case '3':
			if (type == TH_UPPER)
				return numTH[2];
			return numth[2];
		default:
			if (type == TH_UPPER)
				return numTH[3];
			return numth[3];
	}
}

/* ----------
 * 把数字串转换为序数数字串
 * type 0=大写 1=小写
 * ----------
 */
static char *
str_numth(char *dest, char *num, int type)
{
	if (dest != num)
		strcpy(dest, num);
	strcat(dest, get_th(num, type));
	return dest;
}

/*****************************************************************************
 *			upper/lower/initcap 函数
 *****************************************************************************/

#ifdef USE_ICU

typedef int32_t (*ICU_Convert_Func) (UChar *dest, int32_t destCapacity,
									 const UChar *src, int32_t srcLength,
									 const char *locale,
									 UErrorCode *pErrorCode);

static int32_t
icu_convert_case(ICU_Convert_Func func, pg_locale_t mylocale,
				 UChar **buff_dest, UChar *buff_source, int32_t len_source)
{
	UErrorCode	status;
	int32_t		len_dest;

	len_dest = len_source;		/* 先以相同长度尝试 */
	*buff_dest = palloc(len_dest * sizeof(**buff_dest));
	status = U_ZERO_ERROR;
	len_dest = func(*buff_dest, len_dest, buff_source, len_source,
					mylocale->info.icu.locale, &status);
	if (status == U_BUFFER_OVERFLOW_ERROR)
	{
		/* 以调整后的长度重试 */
		pfree(*buff_dest);
		*buff_dest = palloc(len_dest * sizeof(**buff_dest));
		status = U_ZERO_ERROR;
		len_dest = func(*buff_dest, len_dest, buff_source, len_source,
						mylocale->info.icu.locale, &status);
	}
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("case conversion failed: %s", u_errorName(status))));
	return len_dest;
}

static int32_t
u_strToTitle_default_BI(UChar *dest, int32_t destCapacity,
						const UChar *src, int32_t srcLength,
						const char *locale,
						UErrorCode *pErrorCode)
{
	return u_strToTitle(dest, destCapacity, src, srcLength,
						NULL, locale, pErrorCode);
}

#endif							/* USE_ICU */

/*
 * 若系统提供宽字符操纵所需的各函数
 * （均由 C99 标准化），则 upper/lower/initcap
 * 必要时会用宽字符函数实现；否则用传统
 * <ctype.h> 函数，它们在多字节字符集下当然
 * 无法如愿。注意无论哪种情形，我们实际上
 * 都在假定数据库字符编码与 LC_CTYPE
 * 一致。
 *
 * 若系统提供 locale_t 及相关函数（由 Open Group 的 XBD
 * 标准化），可支持非默认非 C 的排序规则。代码
 * 编写为同时处理 have-wide-characters 与 have-locale_t
 * 两种组合，不过某平台有后者而无前者的情况
 * 相当罕见。
 */

/*
 * 感知排序规则与宽字符的 lower 函数
 *
 * 传入字节数，以便能把 varlena 和 char* 传给
 * 本函数。结果为 palloc 的、以 null 结尾的串。
 */
char *
str_tolower(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * 通常意味着解析器无法解决隐式排序规则
		 * 冲突，因此按该方式报告。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"lower()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	/* C/POSIX 排序规则无论数据库编码为何都走此路径 */
	if (lc_ctype_is_c(collid))
	{
		result = asc_tolower(buff, nbytes);
	}
	else
	{
		pg_locale_t mylocale;

		mylocale = pg_newlocale_from_collation(collid);

#ifdef USE_ICU
		if (mylocale && mylocale->provider == COLLPROVIDER_ICU)
		{
			int32_t		len_uchar;
			int32_t		len_conv;
			UChar	   *buff_uchar;
			UChar	   *buff_conv;

			len_uchar = icu_to_uchar(&buff_uchar, buff, nbytes);
			len_conv = icu_convert_case(u_strToLower, mylocale,
										&buff_conv, buff_uchar, len_uchar);
			icu_from_uchar(&result, buff_conv, len_conv);
			pfree(buff_uchar);
			pfree(buff_conv);
		}
		else
#endif
		{
			if (pg_database_encoding_max_length() > 1)
			{
				wchar_t    *workspace;
				size_t		curr_char;
				size_t		result_size;

				/* 溢出偏执 */
				if ((nbytes + 1) > (INT_MAX / sizeof(wchar_t)))
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));

				/* 输出工作区的字符数无法多于输入字节 */
				workspace = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

				char2wchar(workspace, nbytes + 1, buff, nbytes, mylocale);

				for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						workspace[curr_char] = towlower_l(workspace[curr_char], mylocale->info.lt);
					else
#endif
						workspace[curr_char] = towlower(workspace[curr_char]);
				}

				/*
				 * 结果要足够大；大小写改变可能改变字节
				 * 数。
				 */
				result_size = curr_char * pg_database_encoding_max_length() + 1;
				result = palloc(result_size);

				wchar2char(result, workspace, result_size, mylocale);
				pfree(workspace);
			}
			else
			{
				char	   *p;

				result = pnstrdup(buff, nbytes);

				/*
				 * 注意：我们假定 tolower_l() 不会坏到需要
				 * isupper_l() 守卫测试。使用默认排序规则时，
				 * 我们沿用 Postgres 传统，强制按 ASCII 风格
				 * 处理 I/i，但在非默认排序规则下，
				 * 你得到的正是排序规则所说的。
				 */
				for (p = result; *p; p++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						*p = tolower_l((unsigned char) *p, mylocale->info.lt);
					else
#endif
						*p = pg_tolower((unsigned char) *p);
				}
			}
		}
	}

	return result;
}

/*
 * 感知排序规则与宽字符的 upper 函数
 *
 * 传入字节数，以便能把 varlena 和 char* 传给
 * 本函数。结果为 palloc 的、以 null 结尾的串。
 */
char *
str_toupper(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * 通常意味着解析器无法解决隐式排序规则
		 * 冲突，因此按该方式报告。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"upper()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	/* C/POSIX 排序规则无论数据库编码为何都走此路径 */
	if (lc_ctype_is_c(collid))
	{
		result = asc_toupper(buff, nbytes);
	}
	else
	{
		pg_locale_t mylocale;

		mylocale = pg_newlocale_from_collation(collid);

#ifdef USE_ICU
		if (mylocale && mylocale->provider == COLLPROVIDER_ICU)
		{
			int32_t		len_uchar,
						len_conv;
			UChar	   *buff_uchar;
			UChar	   *buff_conv;

			len_uchar = icu_to_uchar(&buff_uchar, buff, nbytes);
			len_conv = icu_convert_case(u_strToUpper, mylocale,
										&buff_conv, buff_uchar, len_uchar);
			icu_from_uchar(&result, buff_conv, len_conv);
			pfree(buff_uchar);
			pfree(buff_conv);
		}
		else
#endif
		{
			if (pg_database_encoding_max_length() > 1)
			{
				wchar_t    *workspace;
				size_t		curr_char;
				size_t		result_size;

				/* 溢出偏执 */
				if ((nbytes + 1) > (INT_MAX / sizeof(wchar_t)))
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));

				/* 输出工作区的字符数无法多于输入字节 */
				workspace = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

				char2wchar(workspace, nbytes + 1, buff, nbytes, mylocale);

				for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						workspace[curr_char] = towupper_l(workspace[curr_char], mylocale->info.lt);
					else
#endif
						workspace[curr_char] = towupper(workspace[curr_char]);
				}

				/*
				 * 结果要足够大；大小写改变可能改变字节
				 * 数。
				 */
				result_size = curr_char * pg_database_encoding_max_length() + 1;
				result = palloc(result_size);

				wchar2char(result, workspace, result_size, mylocale);
				pfree(workspace);
			}
			else
			{
				char	   *p;

				result = pnstrdup(buff, nbytes);

				/*
				 * 注意：我们假定 toupper_l() 不会坏到需要
				 * islower_l() 守卫测试。使用默认排序规则时，
				 * 我们沿用 Postgres 传统，强制按 ASCII 风格
				 * 处理 I/i，但在非默认排序规则下，
				 * 你得到的正是排序规则所说的。
				 */
				for (p = result; *p; p++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
						*p = toupper_l((unsigned char) *p, mylocale->info.lt);
					else
#endif
						*p = pg_toupper((unsigned char) *p);
				}
			}
		}
	}

	return result;
}

/*
 * 感知排序规则与宽字符的 initcap 函数
 *
 * 传入字节数，以便能把 varlena 和 char* 传给
 * 本函数。结果为 palloc 的、以 null 结尾的串。
 */
char *
str_initcap(const char *buff, size_t nbytes, Oid collid)
{
	char	   *result;
	int			wasalnum = false;

	if (!buff)
		return NULL;

	if (!OidIsValid(collid))
	{
		/*
		 * 通常意味着解析器无法解决隐式排序规则
		 * 冲突，因此按该方式报告。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for %s function",
						"initcap()"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	/* C/POSIX 排序规则无论数据库编码为何都走此路径 */
	if (lc_ctype_is_c(collid))
	{
		result = asc_initcap(buff, nbytes);
	}
	else
	{
		pg_locale_t mylocale;

		mylocale = pg_newlocale_from_collation(collid);

#ifdef USE_ICU
		if (mylocale && mylocale->provider == COLLPROVIDER_ICU)
		{
			int32_t		len_uchar,
						len_conv;
			UChar	   *buff_uchar;
			UChar	   *buff_conv;

			len_uchar = icu_to_uchar(&buff_uchar, buff, nbytes);
			len_conv = icu_convert_case(u_strToTitle_default_BI, mylocale,
										&buff_conv, buff_uchar, len_uchar);
			icu_from_uchar(&result, buff_conv, len_conv);
			pfree(buff_uchar);
			pfree(buff_conv);
		}
		else
#endif
		{
			if (pg_database_encoding_max_length() > 1)
			{
				wchar_t    *workspace;
				size_t		curr_char;
				size_t		result_size;

				/* 溢出偏执 */
				if ((nbytes + 1) > (INT_MAX / sizeof(wchar_t)))
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));

				/* 输出工作区的字符数无法多于输入字节 */
				workspace = (wchar_t *) palloc((nbytes + 1) * sizeof(wchar_t));

				char2wchar(workspace, nbytes + 1, buff, nbytes, mylocale);

				for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
					{
						if (wasalnum)
							workspace[curr_char] = towlower_l(workspace[curr_char], mylocale->info.lt);
						else
							workspace[curr_char] = towupper_l(workspace[curr_char], mylocale->info.lt);
						wasalnum = iswalnum_l(workspace[curr_char], mylocale->info.lt);
					}
					else
#endif
					{
						if (wasalnum)
							workspace[curr_char] = towlower(workspace[curr_char]);
						else
							workspace[curr_char] = towupper(workspace[curr_char]);
						wasalnum = iswalnum(workspace[curr_char]);
					}
				}

				/*
				 * 结果要足够大；大小写改变可能改变字节
				 * 数。
				 */
				result_size = curr_char * pg_database_encoding_max_length() + 1;
				result = palloc(result_size);

				wchar2char(result, workspace, result_size, mylocale);
				pfree(workspace);
			}
			else
			{
				char	   *p;

				result = pnstrdup(buff, nbytes);

				/*
				 * 注意：我们假定 toupper_l()/tolower_l() 不会坏到
				 * 需要守卫测试。用默认排序规则时，
				 * 我们沿用 Postgres 传统，强制按 ASCII 风格
				 * 处理 I/i，但在非默认排序规则下，
				 * 你得到的正是排序规则所说的。
				 */
				for (p = result; *p; p++)
				{
#ifdef HAVE_LOCALE_T
					if (mylocale)
					{
						if (wasalnum)
							*p = tolower_l((unsigned char) *p, mylocale->info.lt);
						else
							*p = toupper_l((unsigned char) *p, mylocale->info.lt);
						wasalnum = isalnum_l((unsigned char) *p, mylocale->info.lt);
					}
					else
#endif
					{
						if (wasalnum)
							*p = pg_tolower((unsigned char) *p);
						else
							*p = pg_toupper((unsigned char) *p);
						wasalnum = isalnum((unsigned char) *p);
					}
				}
			}
		}
	}

	return result;
}

/*
 * 纯 ASCII lower 函数
 *
 * 传入字节数，以便能把 varlena 和 char* 传给
 * 本函数。结果为 palloc 的、以 null 结尾的串。
 */
char *
asc_tolower(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_tolower((unsigned char) *p);

	return result;
}

/*
 * 纯 ASCII upper 函数
 *
 * 传入字节数，以便能把 varlena 和 char* 传给
 * 本函数。结果为 palloc 的、以 null 结尾的串。
 */
char *
asc_toupper(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
		*p = pg_ascii_toupper((unsigned char) *p);

	return result;
}

/*
 * 纯 ASCII initcap 函数
 *
 * 传入字节数，以便能把 varlena 和 char* 传给
 * 本函数。结果为 palloc 的、以 null 结尾的串。
 */
char *
asc_initcap(const char *buff, size_t nbytes)
{
	char	   *result;
	char	   *p;
	int			wasalnum = false;

	if (!buff)
		return NULL;

	result = pnstrdup(buff, nbytes);

	for (p = result; *p; p++)
	{
		char		c;

		if (wasalnum)
			*p = c = pg_ascii_tolower((unsigned char) *p);
		else
			*p = c = pg_ascii_toupper((unsigned char) *p);
		/* 这里不信任 isalnum() */
		wasalnum = ((c >= 'A' && c <= 'Z') ||
					(c >= 'a' && c <= 'z') ||
					(c >= '0' && c <= '9'));
	}

	return result;
}

/* 输入以 null 结尾时使用的便利例程 */

static char *
str_tolower_z(const char *buff, Oid collid)
{
	return str_tolower(buff, strlen(buff), collid);
}

static char *
str_toupper_z(const char *buff, Oid collid)
{
	return str_toupper(buff, strlen(buff), collid);
}

static char *
str_initcap_z(const char *buff, Oid collid)
{
	return str_initcap(buff, strlen(buff), collid);
}

static char *
asc_tolower_z(const char *buff)
{
	return asc_tolower(buff, strlen(buff));
}

static char *
asc_toupper_z(const char *buff)
{
	return asc_toupper(buff, strlen(buff));
}

/* 目前尚不需要 asc_initcap_z */


/* ----------
 * FROM_CHAR 跳过 TM/th
 *
 * 若 S_THth 开启，跳过两字符（假定有两个可用）
 * ----------
 */
#define SKIP_THth(ptr, _suf) \
	do { \
		if (S_THth(_suf)) \
		{ \
			if (*(ptr)) (ptr) += pg_mblen(ptr); \
			if (*(ptr)) (ptr) += pg_mblen(ptr); \
		} \
	} while (0)


#ifdef DEBUG_TO_FROM_CHAR
/* -----------
 * DEBUG：调试与索引检查用；（显示 ASCII 字符
 * 与每个所用位置定义的关键字）
 * ----------
 */
static void
dump_index(const KeyWord *k, const int *index)
{
	int			i,
				count = 0,
				free_i = 0;

	elog(DEBUG_elog_output, "TO-FROM_CHAR: Dump KeyWord Index:");

	for (i = 0; i < KeyWord_INDEX_SIZE; i++)
	{
		if (index[i] != -1)
		{
			elog(DEBUG_elog_output, "\t%c: %s, ", i + 32, k[index[i]].name);
			count++;
		}
		else
		{
			free_i++;
			elog(DEBUG_elog_output, "\t(%d) %c %d", i, i + 32, index[i]);
		}
	}
	elog(DEBUG_elog_output, "\n\t\tUsed positions: %d,\n\t\tFree positions: %d",
		 count, free_i);
}
#endif							/* DEBUG */

/* ----------
 * 若下一个格式模式不是数字值则返回真
 * ----------
 */
static bool
is_next_separator(FormatNode *n)
{
	if (n->type == NODE_TYPE_END)
		return false;

	if (n->type == NODE_TYPE_ACTION && S_THth(n->suffix))
		return true;

	/*
	 * Next node
	 */
	n++;

	/* 格式串末尾视为非数字分隔符 */
	if (n->type == NODE_TYPE_END)
		return true;

	if (n->type == NODE_TYPE_ACTION)
	{
		if (n->key->is_digit)
			return false;

		return true;
	}
	else if (n->character[1] == '\0' &&
			 isdigit((unsigned char) n->character[0]))
		return false;

	return true;				/* 某非数字输入（分隔符）*/
}


static int
adjust_partial_year_to_2020(int year)
{
	/*
	 * 把所有日期调向 2020；这实际上等价于我们假定
	 * '70' 为 1970 且 '69' 为 2069。
	 */
	/* 0-69 归入 2000 年代 */
	if (year < 70)
		return year + 2000;
	/* 70-99 归入 1900 年代 */
	else if (year < 100)
		return year + 1900;
	/* 100-519 归入 2000 年代 */
	else if (year < 520)
		return year + 2000;
	/* 520-999 归入 1000 年代 */
	else if (year < 1000)
		return year + 1000;
	else
		return year;
}


static int
strspace_len(const char *str)
{
	int			len = 0;

	while (*str && isspace((unsigned char) *str))
	{
		str++;
		len++;
	}
	return len;
}

/*
 * 设置 from-char 转换的日期模式。
 *
 * 若日期模式已被设置，而调用者又试图将其设为
 * 冲突模式则报错。
 *
 * 成功返回真，失败返回假（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 */
static bool
from_char_set_mode(TmFromChar *tmfc, const FromCharDateMode mode,
				   Node *escontext)
{
	if (mode != FROM_CHAR_DATE_NONE)
	{
		if (tmfc->mode == FROM_CHAR_DATE_NONE)
			tmfc->mode = mode;
		else if (tmfc->mode != mode)
			ereturn(escontext, false,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("invalid combination of date conventions"),
					 errhint("Do not mix Gregorian and ISO week date "
							 "conventions in a formatting template.")));
	}
	return true;
}

/*
 * 把 'dest' 所指的整数设为给定值。
 *
 * 若目标整数此前已被设为其他非零值，
 * 则报错。
 *
 * 成功返回真，失败返回假（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 */
static bool
from_char_set_int(int *dest, const int value, const FormatNode *node,
				  Node *escontext)
{
	if (*dest != 0 && *dest != value)
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("conflicting values for \"%s\" field in formatting string",
						node->key->name),
				 errdetail("This value contradicts a previous setting "
						   "for the same field type.")));
	*dest = value;
	return true;
}

/*
 * 从源串读取单个整数存入 'dest' 所指的 int；若 'dest'
 * 为 NULL，则丢弃结果。
 *
 * 在定宽模式（节点不带 FM 后缀）下，最多消耗 'len' 个
 * 字符；不过前导空白不计入 'len'。
 *
 * 我们用 strtol() 从源串恢复整数值，以与给定
 * FormatNode 相符。
 *
 * 若转换成功完成，src 将前进到指向转换所用最后一个
 * 字符之后紧邻的字符。
 *
 *
 * 返回所消耗的字符数，出错时 -1（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 *
 * 注意 from_char_parse_int() 提供了更方便的包装，适用于
 * 字段长度与格式关键字长度相同的情形（如 DD 与 MI）。
 *
 */
static int
from_char_parse_int_len(int *dest, const char **src, const int len, FormatNode *node,
						Node *escontext)
{
	long		result;
	char		copy[DCH_MAX_ITEM_SIZ + 1];
	const char *init = *src;
	int			used;

	/*
	 * 解析整数前先跳过所有空白。
	 */
	*src += strspace_len(*src);

	Assert(len <= DCH_MAX_ITEM_SIZ);
	used = (int) strlcpy(copy, *src, len + 1);

	if (S_FM(node->suffix) || is_next_separator(node))
	{
		/*
		 * 本节点处于填充模式，或已知下一节点
		 * 是非数字值，因此我们只管尽量多地抓取字符。
		 */
		char	   *endptr;

		errno = 0;
		result = strtol(init, &endptr, 10);
		*src = endptr;
	}
	else
	{
		/*
		 * 我们需要从串中恰好拉出 'len' 给定的字符数，并
		 * 转换这些字符。
		 */
		char	   *last;

		if (used < len)
			ereturn(escontext, -1,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("source string too short for \"%s\" formatting field",
							node->key->name),
					 errdetail("Field requires %d characters, but only %d remain.",
							   len, used),
					 errhint("If your source string is not fixed-width, "
							 "try using the \"FM\" modifier.")));

		errno = 0;
		result = strtol(copy, &last, 10);
		used = last - copy;

		if (used > 0 && used < len)
			ereturn(escontext, -1,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("invalid value \"%s\" for \"%s\"",
							copy, node->key->name),
					 errdetail("Field requires %d characters, but only %d could be parsed.",
							   len, used),
					 errhint("If your source string is not fixed-width, "
							 "try using the \"FM\" modifier.")));

		*src += used;
	}

	if (*src == init)
		ereturn(escontext, -1,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid value \"%s\" for \"%s\"",
						copy, node->key->name),
				 errdetail("Value must be an integer.")));

	if (errno == ERANGE || result < INT_MIN || result > INT_MAX)
		ereturn(escontext, -1,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("value for \"%s\" in source string is out of range",
						node->key->name),
				 errdetail("Value must be in the range %d to %d.",
						   INT_MIN, INT_MAX)));

	if (dest != NULL)
	{
		if (!from_char_set_int(dest, (int) result, node, escontext))
			return -1;
	}

	return *src - init;
}

/*
 * 调用 from_char_parse_int_len()，以格式关键字的长度
 * 作为字段的期望长度。
 *
 * 若字段长度与格式关键字不同，则不要调用本函数
 * （如 HH24；关键字长度为 4，而字段长度为 2）。此时
 * 应改为调用 from_char_parse_int_len() 来显式指定
 * 所需长度。
 */
static int
from_char_parse_int(int *dest, const char **src, FormatNode *node,
					Node *escontext)
{
	return from_char_parse_int_len(dest, src, node->key->len, node, escontext);
}

/*
 * 在以 null 结尾的"数组"中顺序搜索，进行大小写不敏
 * 感匹配，与"name"起始字符。
 *
 * 返回匹配的下标，无匹配返回 -1。
 *
 * *len 被设为匹配的长度，无匹配则设为 0。
 *
 * 大小写不敏感以 pg_ascii_tolower 定义，故本函数只
 * 适合与 ASCII 串比较。
 */
static int
seq_search_ascii(const char *name, const char *const *array, int *len)
{
	unsigned char firstc;
	const char *const *a;

	*len = 0;

	/* 空串匹配不到任何内容 */
	if (!*name)
		return -1;

	/* 特殊处理首字符以获取一些速度 */
	firstc = pg_ascii_tolower((unsigned char) *name);

	for (a = array; *a != NULL; a++)
	{
		const char *p;
		const char *n;

		/* 比较首字符 */
		if (pg_ascii_tolower((unsigned char) **a) != firstc)
			continue;

		/* 比较串其余部分 */
		for (p = *a + 1, n = name + 1;; p++, n++)
		{
			/* 匹配整个数组条目则返回成功 */
			if (*p == '\0')
			{
				*len = n - name;
				return a - array;
			}
			/* 否则，"name" 中必然还有字符... */
			if (*n == '\0')
				break;
			/* ... 且必须匹配 */
			if (pg_ascii_tolower((unsigned char) *p) !=
				pg_ascii_tolower((unsigned char) *n))
				break;
		}
	}

	return -1;
}

/*
 * 在可能含非英文单词的数组中顺序搜索，
 * 与 "name" 起始字符做大小写不敏感的匹配。
 *
 * 其 API 与 seq_search_ascii() 相同，但我们使用更通用
 * 的大小写折叠变换来实现大小写不敏感。大小写折叠
 * 按 "collid" 所标识排序规则的规则完成。
 *
 * 数组按 const 对待，但不那样声明，因为 pg_locale.c
 * 导出的数组并非 const。
 */
static int
seq_search_localized(const char *name, char **array, int *len, Oid collid)
{
	char	  **a;
	char	   *upper_name;
	char	   *lower_name;

	*len = 0;

	/* 空串匹配不到任何内容 */
	if (!*name)
		return -1;

	/*
	 * 下面的大小写折叠处理相当昂贵，因此在做之前，
	 * 先快速过一遍，看是否有精确匹配。
	 */
	for (a = array; *a != NULL; a++)
	{
		int			element_len = strlen(*a);

		if (strncmp(name, *a, element_len) == 0)
		{
			*len = element_len;
			return a - array;
		}
	}

	/*
	 * 先折为大写，再折为小写，这样即使在大小写转换
	 * 非单射的语言中也能可靠匹配。
	 */
	upper_name = str_toupper(unconstify(char *, name), strlen(name), collid);
	lower_name = str_tolower(upper_name, strlen(upper_name), collid);
	pfree(upper_name);

	for (a = array; *a != NULL; a++)
	{
		char	   *upper_element;
		char	   *lower_element;
		int			element_len;

		/* 同样处理大小写数组元素 */
		upper_element = str_toupper(*a, strlen(*a), collid);
		lower_element = str_tolower(upper_element, strlen(upper_element),
									collid);
		pfree(upper_element);
		element_len = strlen(lower_element);

		/* Match? */
		if (strncmp(lower_name, lower_element, element_len) == 0)
		{
			*len = element_len;
			pfree(lower_element);
			pfree(lower_name);
			return a - array;
		}
		pfree(lower_element);
	}

	pfree(lower_name);
	return -1;
}

/*
 * 在 'array'（若 'localized_array' 非 NULL 则在其中）
 * 做顺序搜索，寻找与 'src' 串起始字符大小写不敏
 * 感匹配的条目。
 *
 * 'array' 被假定为英文单词（全 ASCII），但
 * 若给了 'localized_array'，那可能是非英文，
 * 因此需要更昂贵的大小写折叠变换
 * （将遵循排序规则 'collid' 的规则）。
 *
 * 若找到匹配，则把匹配的数组下标复制到 'dest' 所指
 * 的整数，并把 'src' 前进到串中匹配部分末尾之后。
 *
 *
 * 匹配返回真，失败返回假（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。
 *
 * 'node' 仅用于错误报告：node->key->name 标识我们
 * 要搜索的字段类型。
 */
static bool
from_char_seq_search(int *dest, const char **src, const char *const *array,
					 char **localized_array, Oid collid,
					 FormatNode *node, Node *escontext)
{
	int			len;

	if (localized_array == NULL)
		*dest = seq_search_ascii(*src, array, &len);
	else
		*dest = seq_search_localized(*src, localized_array, &len, collid);

	if (len <= 0)
	{
		/*
		 * 在错误报告中，把串截断到下一空白处（如有），
		 * 以避免包含无关数据。
		 */
		char	   *copy = pstrdup(*src);
		char	   *c;

		for (c = copy; *c; c++)
		{
			if (scanner_isspace(*c))
			{
				*c = '\0';
				break;
			}
		}

		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid value \"%s\" for \"%s\"",
						copy, node->key->name),
				 errdetail("The given value did not match any of "
						   "the allowed values for this field.")));
	}
	*src += len;
	return true;
}

/* ----------
 * 按 FormatNodes 列表所指明的处理 TmToChar 结构体。
 * 格式化后的数据写入 'out' 所指的串。
 * ----------
 */
static void
DCH_to_char(FormatNode *node, bool is_interval, TmToChar *in, char *out, Oid collid)
{
	FormatNode *n;
	char	   *s;
	struct fmt_tm *tm = &in->tm;
	int			i;

	/* 缓存本地化的日与月 */
	cache_locale_time();

	s = out;
	for (n = node; n->type != NODE_TYPE_END; n++)
	{
		if (n->type != NODE_TYPE_ACTION)
		{
			strcpy(s, n->character);
			s += strlen(s);
			continue;
		}

		switch (n->key->id)
		{
			case DCH_A_M:
			case DCH_P_M:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? P_M_STR : A_M_STR);
				s += strlen(s);
				break;
			case DCH_AM:
			case DCH_PM:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? PM_STR : AM_STR);
				s += strlen(s);
				break;
			case DCH_a_m:
			case DCH_p_m:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? p_m_STR : a_m_STR);
				s += strlen(s);
				break;
			case DCH_am:
			case DCH_pm:
				strcpy(s, (tm->tm_hour % HOURS_PER_DAY >= HOURS_PER_DAY / 2)
					   ? pm_STR : am_STR);
				s += strlen(s);
				break;
			case DCH_HH:
			case DCH_HH12:

				/*
				 * 时间按 12 小时制时钟显示，即便对
				 * interval
				 */
				sprintf(s, "%0*lld", S_FM(n->suffix) ? 0 : (tm->tm_hour >= 0) ? 2 : 3,
						tm->tm_hour % (HOURS_PER_DAY / 2) == 0 ?
						(long long) (HOURS_PER_DAY / 2) :
						(long long) (tm->tm_hour % (HOURS_PER_DAY / 2)));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_HH24:
				sprintf(s, "%0*lld", S_FM(n->suffix) ? 0 : (tm->tm_hour >= 0) ? 2 : 3,
						(long long) tm->tm_hour);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_MI:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_min >= 0) ? 2 : 3,
						tm->tm_min);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_SS:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_sec >= 0) ? 2 : 3,
						tm->tm_sec);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;

#define DCH_to_char_fsec(frac_fmt, frac_val) \
				sprintf(s, frac_fmt, (int) (frac_val)); \
				if (S_THth(n->suffix)) \
					str_numth(s, s, S_TH_TYPE(n->suffix)); \
				s += strlen(s)

			case DCH_FF1:		/* 十分之一秒 */
				DCH_to_char_fsec("%01d", in->fsec / 100000);
				break;
			case DCH_FF2:		/* 百分之一秒 */
				DCH_to_char_fsec("%02d", in->fsec / 10000);
				break;
			case DCH_FF3:
			case DCH_MS:		/* 毫秒 */
				DCH_to_char_fsec("%03d", in->fsec / 1000);
				break;
			case DCH_FF4:		/* 十分之一毫秒 */
				DCH_to_char_fsec("%04d", in->fsec / 100);
				break;
			case DCH_FF5:		/* 百分之一毫秒 */
				DCH_to_char_fsec("%05d", in->fsec / 10);
				break;
			case DCH_FF6:
			case DCH_US:		/* 微秒 */
				DCH_to_char_fsec("%06d", in->fsec);
				break;
#undef DCH_to_char_fsec
			case DCH_SSSS:
				sprintf(s, "%lld",
						(long long) (tm->tm_hour * SECS_PER_HOUR +
									 tm->tm_min * SECS_PER_MINUTE +
									 tm->tm_sec));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_tz:
				INVALID_FOR_INTERVAL;
				if (tmtcTzn(in))
				{
					/* 我们假定时区名未被本地化 */
					char	   *p = asc_tolower_z(tmtcTzn(in));

					strcpy(s, p);
					pfree(p);
					s += strlen(s);
				}
				break;
			case DCH_TZ:
				INVALID_FOR_INTERVAL;
				if (tmtcTzn(in))
				{
					strcpy(s, tmtcTzn(in));
					s += strlen(s);
				}
				break;
			case DCH_TZH:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%c%02d",
						(tm->tm_gmtoff >= 0) ? '+' : '-',
						abs((int) tm->tm_gmtoff) / SECS_PER_HOUR);
				s += strlen(s);
				break;
			case DCH_TZM:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%02d",
						(abs((int) tm->tm_gmtoff) % SECS_PER_HOUR) / SECS_PER_MINUTE);
				s += strlen(s);
				break;
			case DCH_OF:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%c%0*d",
						(tm->tm_gmtoff >= 0) ? '+' : '-',
						S_FM(n->suffix) ? 0 : 2,
						abs((int) tm->tm_gmtoff) / SECS_PER_HOUR);
				s += strlen(s);
				if (abs((int) tm->tm_gmtoff) % SECS_PER_HOUR != 0)
				{
					sprintf(s, ":%02d",
							(abs((int) tm->tm_gmtoff) % SECS_PER_HOUR) / SECS_PER_MINUTE);
					s += strlen(s);
				}
				break;
			case DCH_A_D:
			case DCH_B_C:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? B_C_STR : A_D_STR));
				s += strlen(s);
				break;
			case DCH_AD:
			case DCH_BC:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? BC_STR : AD_STR));
				s += strlen(s);
				break;
			case DCH_a_d:
			case DCH_b_c:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? b_c_STR : a_d_STR));
				s += strlen(s);
				break;
			case DCH_ad:
			case DCH_bc:
				INVALID_FOR_INTERVAL;
				strcpy(s, (tm->tm_year <= 0 ? bc_STR : ad_STR));
				s += strlen(s);
				break;
			case DCH_MONTH:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_toupper_z(months_full[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_Month:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							months_full[tm->tm_mon - 1]);
				s += strlen(s);
				break;
			case DCH_month:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_full_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_tolower_z(months_full[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_MON:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_toupper_z(months[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_Mon:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, months[tm->tm_mon - 1]);
				s += strlen(s);
				break;
			case DCH_mon:
				INVALID_FOR_INTERVAL;
				if (!tm->tm_mon)
					break;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_abbrev_months[tm->tm_mon - 1], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_tolower_z(months[tm->tm_mon - 1]));
				s += strlen(s);
				break;
			case DCH_MM:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (tm->tm_mon >= 0) ? 2 : 3,
						tm->tm_mon);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_DAY:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_toupper_z(days[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_Day:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							days[tm->tm_wday]);
				s += strlen(s);
				break;
			case DCH_day:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_full_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -9,
							asc_tolower_z(days[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_DY:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_toupper_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_toupper_z(days_short[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_Dy:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_initcap_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, days_short[tm->tm_wday]);
				s += strlen(s);
				break;
			case DCH_dy:
				INVALID_FOR_INTERVAL;
				if (S_TM(n->suffix))
				{
					char	   *str = str_tolower_z(localized_abbrev_days[tm->tm_wday], collid);

					if (strlen(str) <= (n->key->len + TM_SUFFIX_LEN) * DCH_MAX_ITEM_SIZ)
						strcpy(s, str);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("localized string format value too long")));
				}
				else
					strcpy(s, asc_tolower_z(days_short[tm->tm_wday]));
				s += strlen(s);
				break;
			case DCH_DDD:
			case DCH_IDDD:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 3,
						(n->key->id == DCH_DDD) ?
						tm->tm_yday :
						date2isoyearday(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_DD:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2, tm->tm_mday);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_D:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%d", tm->tm_wday + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_ID:
				INVALID_FOR_INTERVAL;
				sprintf(s, "%d", (tm->tm_wday == 0) ? 7 : tm->tm_wday);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_WW:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2,
						(tm->tm_yday - 1) / 7 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_IW:
				sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : 2,
						date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Q:
				if (!tm->tm_mon)
					break;
				sprintf(s, "%d", (tm->tm_mon - 1) / 3 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_CC:
				if (is_interval)	/* 直接计算 */
					i = tm->tm_year / 100;
				else
				{
					if (tm->tm_year > 0)
						/* 世纪 20 == 1901 - 2000 */
						i = (tm->tm_year - 1) / 100 + 1;
					else
						/* 世纪 6BC == 600BC - 501BC */
						i = tm->tm_year / 100 - 1;
				}
				if (i <= 99 && i >= -99)
					sprintf(s, "%0*d", S_FM(n->suffix) ? 0 : (i >= 0) ? 2 : 3, i);
				else
					sprintf(s, "%d", i);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Y_YYY:
				i = ADJUST_YEAR(tm->tm_year, is_interval) / 1000;
				sprintf(s, "%d,%03d", i,
						ADJUST_YEAR(tm->tm_year, is_interval) - (i * 1000));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YYYY:
			case DCH_IYYY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 4 : 5,
						(n->key->id == DCH_YYYY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YYY:
			case DCH_IYY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 3 : 4,
						(n->key->id == DCH_YYY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 1000);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_YY:
			case DCH_IY:
				sprintf(s, "%0*d",
						S_FM(n->suffix) ? 0 :
						(ADJUST_YEAR(tm->tm_year, is_interval) >= 0) ? 2 : 3,
						(n->key->id == DCH_YY ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 100);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_Y:
			case DCH_I:
				sprintf(s, "%1d",
						(n->key->id == DCH_Y ?
						 ADJUST_YEAR(tm->tm_year, is_interval) :
						 ADJUST_YEAR(date2isoyear(tm->tm_year,
												  tm->tm_mon,
												  tm->tm_mday),
									 is_interval)) % 10);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_RM:
				/* FALLTHROUGH */
			case DCH_rm:

				/*
				 * 对 interval，'12 month' 这类值会化简为
				 * 0 月和若干年，这些应得到处理。
				 */
				if (!tm->tm_mon && !tm->tm_year)
					break;
				else
				{
					int			mon = 0;
					const char *const *months;

					if (n->key->id == DCH_RM)
						months = rm_months_upper;
					else
						months = rm_months_lower;

					/*
					 * 计算在罗马数字数组中的位置。注意
					 * 数组内容是逆序的：十二月在首，
					 * 一月在末。
					 */
					if (tm->tm_mon == 0)
					{
						/*
						 * 此情形特殊，对应完整 interval 年
						 * 的情形。
						 */
						mon = tm->tm_year >= 0 ? 0 : MONTHS_PER_YEAR - 1;
					}
					else if (tm->tm_mon < 0)
					{
						/*
						 * 负值情形。此时计算是反向的：
						 * -1 十二月，-2 十一月，等等。
							 *
						 */
						mon = -1 * (tm->tm_mon + 1);
					}
					else
					{
						/*
						 * 常见情形，值为严格正数。数组中的
						 * 位置与 tm_mon 的值相对应。
							 *
						 */
						mon = MONTHS_PER_YEAR - tm->tm_mon;
					}

					sprintf(s, "%*s", S_FM(n->suffix) ? 0 : -4,
							months[mon]);
					s += strlen(s);
				}
				break;
			case DCH_W:
				sprintf(s, "%d", (tm->tm_mday - 1) / 7 + 1);
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
			case DCH_J:
				sprintf(s, "%d", date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (S_THth(n->suffix))
					str_numth(s, s, S_TH_TYPE(n->suffix));
				s += strlen(s);
				break;
		}
	}

	*s = '\0';
}

/*
 * 按 FormatNodes 数组 'node[]' 所指明的来处理串 'in'。
 * 结果填充到 'out' 所指的 TmFromChar 结构体中。
 *
 * 'collid' 标识所需排序规则（如需）。
 * 'std' 指定标准解析模式。
 *
 * 若 escontext 指向 ErrorSaveContext，数据错误将通过填充
 * 该结构体来报告；调用者必须测试 SOFT_ERROR_OCCURRED()
 * 来看是否发生了错误。否则抛出错误。
 *
 * 注意：目前还没有 to_interval() 函数，所以此处
 * 无需 INVALID_FOR_INTERVAL 检查。
 */
static void
DCH_from_char(FormatNode *node, const char *in, TmFromChar *out,
			  Oid collid, bool std, Node *escontext)
{
	FormatNode *n;
	const char *s;
	int			len,
				value;
	bool		fx_mode = std;

	/* 多跳过的字符数（多于格式串所给定的） */
	int			extra_skip = 0;

	/* 缓存本地化的日与月 */
	cache_locale_time();

	for (n = node, s = in; n->type != NODE_TYPE_END && *s != '\0'; n++)
	{
		/*
		 * 非 FX（定宽）模式下，忽略串开头以及字段之前的
		 * 空白。
		 */
		if (!fx_mode && (n->type != NODE_TYPE_ACTION || n->key->id != DCH_FX) &&
			(n->type == NODE_TYPE_ACTION || n == node))
		{
			while (*s != '\0' && isspace((unsigned char) *s))
			{
				s++;
				extra_skip++;
			}
		}

		if (n->type == NODE_TYPE_SPACE || n->type == NODE_TYPE_SEPARATOR)
		{
			if (std)
			{
				/*
				 * 标准模式要求格式串的分隔符/空格与
				 * 输入串严格匹配。
				 */
				Assert(n->character[0] && !n->character[1]);

				if (*s == n->character[0])
					s++;
				else
					ereturn(escontext,,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("unmatched format separator \"%c\"",
									n->character[0])));
			}
			else if (!fx_mode)
			{
				/*
				 * 非 FX（定格式）模式下，一个格式串
				 * 空格或分隔符，匹配输入串中一个空格或
				 * 分隔符。若输入当前位置无空格/分隔符，
				 * 则什么都不匹配。
				 */
				extra_skip--;
				if (isspace((unsigned char) *s) || is_separator_char(s))
				{
					s++;
					extra_skip++;
				}
			}
			else
			{
				/*
				 * FX 模式下，遇到格式串空格或分隔符时恰好
				 * 从输入串消耗一个字符。注意我们并不
				 * 坚持被消耗的字符与格式的字符匹配。
				 *
				 */
				s += pg_mblen(s);
			}
			continue;
		}
		else if (n->type != NODE_TYPE_ACTION)
		{
			/*
			 * 文本字符，因此从输入串消耗一个字符。
			 * 注意我们并不坚持被消耗的字符与格式
			 * 的字符匹配。
			 */
			if (!fx_mode)
			{
				/*
				 * 非 FX 模式下，此前可能已多跳过一些字符
				 * （多于格式串所指定的）。此时我们不
				 * 跳过输入串字符，因其可能属字段。
				 *
				 */
				if (extra_skip > 0)
					extra_skip--;
				else
					s += pg_mblen(s);
			}
			else
			{
				int			chlen = pg_mblen(s);

				/*
				 * 标准模式要求格式字符严格匹配。
				 */
				if (std && n->type == NODE_TYPE_CHAR &&
					strncmp(s, n->character, chlen) != 0)
					ereturn(escontext,,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("unmatched format character \"%s\"",
									n->character)));

				s += chlen;
			}
			continue;
		}

		if (!from_char_set_mode(out, n->key->date_mode, escontext))
			return;

		switch (n->key->id)
		{
			case DCH_FX:
				fx_mode = true;
				break;
			case DCH_A_M:
			case DCH_P_M:
			case DCH_a_m:
			case DCH_p_m:
				if (!from_char_seq_search(&value, &s, ampm_strings_long,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->pm, value % 2, n, escontext))
					return;
				out->clock = CLOCK_12_HOUR;
				break;
			case DCH_AM:
			case DCH_PM:
			case DCH_am:
			case DCH_pm:
				if (!from_char_seq_search(&value, &s, ampm_strings,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->pm, value % 2, n, escontext))
					return;
				out->clock = CLOCK_12_HOUR;
				break;
			case DCH_HH:
			case DCH_HH12:
				if (from_char_parse_int_len(&out->hh, &s, 2, n, escontext) < 0)
					return;
				out->clock = CLOCK_12_HOUR;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_HH24:
				if (from_char_parse_int_len(&out->hh, &s, 2, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_MI:
				if (from_char_parse_int(&out->mi, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_SS:
				if (from_char_parse_int(&out->ss, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_MS:		/* 毫秒 */
				len = from_char_parse_int_len(&out->ms, &s, 3, n, escontext);
				if (len < 0)
					return;

				/*
				 * 25 是 0.25，250 也是 0.25；025 是 0.025 而非 0.25
				 */
				out->ms *= len == 1 ? 100 :
					len == 2 ? 10 : 1;

				SKIP_THth(s, n->suffix);
				break;
			case DCH_FF1:
			case DCH_FF2:
			case DCH_FF3:
			case DCH_FF4:
			case DCH_FF5:
			case DCH_FF6:
				out->ff = n->key->id - DCH_FF1 + 1;
				/* fall through */
			case DCH_US:		/* 微秒 */
				len = from_char_parse_int_len(&out->us, &s,
											  n->key->id == DCH_US ? 6 :
											  out->ff, n, escontext);
				if (len < 0)
					return;

				out->us *= len == 1 ? 100000 :
					len == 2 ? 10000 :
					len == 3 ? 1000 :
					len == 4 ? 100 :
					len == 5 ? 10 : 1;

				SKIP_THth(s, n->suffix);
				break;
			case DCH_SSSS:
				if (from_char_parse_int(&out->ssss, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_tz:
			case DCH_TZ:
			case DCH_OF:
				ereturn(escontext,,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("formatting field \"%s\" is only supported in to_char",
								n->key->name)));
				break;
			case DCH_TZH:

				/*
				 * TZH 的值可能为负。问题在于我们可能把
				 * 负号当作分隔符吞掉。所以若我们跳过
				 * 的字符多于格式串指定，则考虑把最后
				 * 跳过的负号前置到 TZH。
				 *
				 */
				if (*s == '+' || *s == '-' || *s == ' ')
				{
					out->tzsign = *s == '-' ? -1 : +1;
					s++;
				}
				else
				{
					if (extra_skip > 0 && *(s - 1) == '-')
						out->tzsign = -1;
					else
						out->tzsign = +1;
				}

				if (from_char_parse_int_len(&out->tzh, &s, 2, n, escontext) < 0)
					return;
				break;
			case DCH_TZM:
				/* 若此前未见过 TZH 则赋正时区符号 */
				if (!out->tzsign)
					out->tzsign = +1;
				if (from_char_parse_int_len(&out->tzm, &s, 2, n, escontext) < 0)
					return;
				break;
			case DCH_A_D:
			case DCH_B_C:
			case DCH_a_d:
			case DCH_b_c:
				if (!from_char_seq_search(&value, &s, adbc_strings_long,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->bc, value % 2, n, escontext))
					return;
				break;
			case DCH_AD:
			case DCH_BC:
			case DCH_ad:
			case DCH_bc:
				if (!from_char_seq_search(&value, &s, adbc_strings,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->bc, value % 2, n, escontext))
					return;
				break;
			case DCH_MONTH:
			case DCH_Month:
			case DCH_month:
				if (!from_char_seq_search(&value, &s, months_full,
										  S_TM(n->suffix) ? localized_full_months : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->mm, value + 1, n, escontext))
					return;
				break;
			case DCH_MON:
			case DCH_Mon:
			case DCH_mon:
				if (!from_char_seq_search(&value, &s, months,
										  S_TM(n->suffix) ? localized_abbrev_months : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->mm, value + 1, n, escontext))
					return;
				break;
			case DCH_MM:
				if (from_char_parse_int(&out->mm, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_DAY:
			case DCH_Day:
			case DCH_day:
				if (!from_char_seq_search(&value, &s, days,
										  S_TM(n->suffix) ? localized_full_days : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->d, value, n, escontext))
					return;
				out->d++;
				break;
			case DCH_DY:
			case DCH_Dy:
			case DCH_dy:
				if (!from_char_seq_search(&value, &s, days_short,
										  S_TM(n->suffix) ? localized_abbrev_days : NULL,
										  collid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->d, value, n, escontext))
					return;
				out->d++;
				break;
			case DCH_DDD:
				if (from_char_parse_int(&out->ddd, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_IDDD:
				if (from_char_parse_int_len(&out->ddd, &s, 3, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_DD:
				if (from_char_parse_int(&out->dd, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_D:
				if (from_char_parse_int(&out->d, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_ID:
				if (from_char_parse_int_len(&out->d, &s, 1, n, escontext) < 0)
					return;
				/* 平移编号以匹配周日=1 的公历 */
				if (++out->d > 7)
					out->d = 1;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_WW:
			case DCH_IW:
				if (from_char_parse_int(&out->ww, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_Q:

				/*
				 * 转日期时我们忽略 'Q'，因为不清楚该使用
				 * 季度中的哪个日期，且有些人会同时指定
				 * 季度和月份，若遵从它可能与所给的
				 * 月份冲突。这也是我们不抛出错误的原因。
				 *
				 *
				 * 我们仍会解析源串中的整数，但不会
				 * 存入'out'任何位置。
				 */
				if (from_char_parse_int((int *) NULL, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_CC:
				if (from_char_parse_int(&out->cc, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_Y_YYY:
				{
					int			matched,
								years,
								millennia,
								nch;

					matched = sscanf(s, "%d,%03d%n", &millennia, &years, &nch);
					if (matched < 2)
						ereturn(escontext,,
								(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
								 errmsg("invalid input string for \"Y,YYY\"")));
					years += (millennia * 1000);
					if (!from_char_set_int(&out->year, years, n, escontext))
						return;
					out->yysz = 4;
					s += nch;
					SKIP_THth(s, n->suffix);
				}
				break;
			case DCH_YYYY:
			case DCH_IYYY:
				if (from_char_parse_int(&out->year, &s, n, escontext) < 0)
					return;
				out->yysz = 4;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_YYY:
			case DCH_IYY:
				len = from_char_parse_int(&out->year, &s, n, escontext);
				if (len < 0)
					return;
				if (len < 4)
					out->year = adjust_partial_year_to_2020(out->year);
				out->yysz = 3;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_YY:
			case DCH_IY:
				len = from_char_parse_int(&out->year, &s, n, escontext);
				if (len < 0)
					return;
				if (len < 4)
					out->year = adjust_partial_year_to_2020(out->year);
				out->yysz = 2;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_Y:
			case DCH_I:
				len = from_char_parse_int(&out->year, &s, n, escontext);
				if (len < 0)
					return;
				if (len < 4)
					out->year = adjust_partial_year_to_2020(out->year);
				out->yysz = 1;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_RM:
			case DCH_rm:
				if (!from_char_seq_search(&value, &s, rm_months_lower,
										  NULL, InvalidOid,
										  n, escontext))
					return;
				if (!from_char_set_int(&out->mm, MONTHS_PER_YEAR - value, n,
									   escontext))
					return;
				break;
			case DCH_W:
				if (from_char_parse_int(&out->w, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
			case DCH_J:
				if (from_char_parse_int(&out->j, &s, n, escontext) < 0)
					return;
				SKIP_THth(s, n->suffix);
				break;
		}

		/* 忽略字段后所有空格 */
		if (!fx_mode)
		{
			extra_skip = 0;
			while (*s != '\0' && isspace((unsigned char) *s))
			{
				s++;
				extra_skip++;
			}
		}
	}

	/*
	 * 标准解析模式不允许未匹配的格式模式或
	 * 输入串中的尾随字符。
	 */
	if (std)
	{
		if (n->type != NODE_TYPE_END)
			ereturn(escontext,,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("input string is too short for datetime format")));

		while (*s != '\0' && isspace((unsigned char) *s))
			s++;

		if (*s != '\0')
			ereturn(escontext,,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("trailing characters remain in input string after datetime format")));
	}
}

/*
 * DCH 缓存条目管理的不变式是：DCHCounter 等于现存
 * 条目中的最大年龄值，且每次访问都递增它。若接
 * 近溢出，就把所有年龄值减半处理，从而对哪些条目
 * 最老保持相当准确的判断。
 *
 */
static inline void
DCH_prevent_counter_overflow(void)
{
	if (DCHCounter >= (INT_MAX - 1))
	{
		for (int i = 0; i < n_DCHCache; i++)
			DCHCache[i]->age >>= 1;
		DCHCounter >>= 1;
	}
}

/*
 * 获取格式节点中日期/时间/时区成分的掩码。
 */
static int
DCH_datetime_type(FormatNode *node)
{
	FormatNode *n;
	int			flags = 0;

	for (n = node; n->type != NODE_TYPE_END; n++)
	{
		if (n->type != NODE_TYPE_ACTION)
			continue;

		switch (n->key->id)
		{
			case DCH_FX:
				break;
			case DCH_A_M:
			case DCH_P_M:
			case DCH_a_m:
			case DCH_p_m:
			case DCH_AM:
			case DCH_PM:
			case DCH_am:
			case DCH_pm:
			case DCH_HH:
			case DCH_HH12:
			case DCH_HH24:
			case DCH_MI:
			case DCH_SS:
			case DCH_MS:		/* 毫秒 */
			case DCH_US:		/* 微秒 */
			case DCH_FF1:
			case DCH_FF2:
			case DCH_FF3:
			case DCH_FF4:
			case DCH_FF5:
			case DCH_FF6:
			case DCH_SSSS:
				flags |= DCH_TIMED;
				break;
			case DCH_tz:
			case DCH_TZ:
			case DCH_OF:
			case DCH_TZH:
			case DCH_TZM:
				flags |= DCH_ZONED;
				break;
			case DCH_A_D:
			case DCH_B_C:
			case DCH_a_d:
			case DCH_b_c:
			case DCH_AD:
			case DCH_BC:
			case DCH_ad:
			case DCH_bc:
			case DCH_MONTH:
			case DCH_Month:
			case DCH_month:
			case DCH_MON:
			case DCH_Mon:
			case DCH_mon:
			case DCH_MM:
			case DCH_DAY:
			case DCH_Day:
			case DCH_day:
			case DCH_DY:
			case DCH_Dy:
			case DCH_dy:
			case DCH_DDD:
			case DCH_IDDD:
			case DCH_DD:
			case DCH_D:
			case DCH_ID:
			case DCH_WW:
			case DCH_Q:
			case DCH_CC:
			case DCH_Y_YYY:
			case DCH_YYYY:
			case DCH_IYYY:
			case DCH_YYY:
			case DCH_IYY:
			case DCH_YY:
			case DCH_IY:
			case DCH_Y:
			case DCH_I:
			case DCH_RM:
			case DCH_rm:
			case DCH_W:
			case DCH_J:
				flags |= DCH_DATED;
				break;
		}
	}

	return flags;
}

/* 选一个 DCHCacheEntry 来容纳给定的格式模式 */
static DCHCacheEntry *
DCH_cache_getnew(const char *str, bool std)
{
	DCHCacheEntry *ent;

	/* 确保下面能递增 DCHCounter */
	DCH_prevent_counter_overflow();

	/*
	 * 若缓存已满，移除最老条目（或回收首个无效条目）
	 */
	if (n_DCHCache >= DCH_CACHE_ENTRIES)
	{
		DCHCacheEntry *old = DCHCache[0];

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "cache is full (%d)", n_DCHCache);
#endif
		if (old->valid)
		{
			for (int i = 1; i < DCH_CACHE_ENTRIES; i++)
			{
				ent = DCHCache[i];
				if (!ent->valid)
				{
					old = ent;
					break;
				}
				if (ent->age < old->age)
					old = ent;
			}
		}
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "OLD: '%s' AGE: %d", old->str, old->age);
#endif
		old->valid = false;
		strlcpy(old->str, str, DCH_CACHE_SIZE + 1);
		old->age = (++DCHCounter);
		/* 期望调用者填好 format 后再置为有效 */
		return old;
	}
	else
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "NEW (%d)", n_DCHCache);
#endif
		Assert(DCHCache[n_DCHCache] == NULL);
		DCHCache[n_DCHCache] = ent = (DCHCacheEntry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(DCHCacheEntry));
		ent->valid = false;
		strlcpy(ent->str, str, DCH_CACHE_SIZE + 1);
		ent->std = std;
		ent->age = (++DCHCounter);
		/* 期望调用者填好 format 后再置为有效 */
		++n_DCHCache;
		return ent;
	}
}

/* 查找与给定格式模式匹配的既有 DCHCacheEntry */
static DCHCacheEntry *
DCH_cache_search(const char *str, bool std)
{
	/* 确保下面能递增 DCHCounter */
	DCH_prevent_counter_overflow();

	for (int i = 0; i < n_DCHCache; i++)
	{
		DCHCacheEntry *ent = DCHCache[i];

		if (ent->valid && strcmp(ent->str, str) == 0 && ent->std == std)
		{
			ent->age = (++DCHCounter);
			return ent;
		}
	}

	return NULL;
}

/* 为给定格式模式查找或创建 DCHCacheEntry */
static DCHCacheEntry *
DCH_cache_fetch(const char *str, bool std)
{
	DCHCacheEntry *ent;

	if ((ent = DCH_cache_search(str, std)) == NULL)
	{
		/*
		 * 不在缓存中，须运行解析器并把新格式模式存
		 * 缓存。解析成功前不要标记条目为有效。
		 *
		 */
		ent = DCH_cache_getnew(str, std);

		parse_format(ent->format, str, DCH_keywords, DCH_suff, DCH_index,
					 DCH_FLAG | (std ? STD_FLAG : 0), NULL);

		ent->valid = true;
	}
	return ent;
}

/*
 * 按 fmt 把日期/时间或 interval 格式化为串。
 * 我们把 fmt 解析为 FormatNodes 列表，随后传给 DCH_to_char
 * 去格式化。
 */
static text *
datetime_to_char_body(TmToChar *tmtc, text *fmt, bool is_interval, Oid collid)
{
	FormatNode *format;
	char	   *fmt_str,
			   *result;
	bool		incache;
	int			fmt_len;
	text	   *res;

	/*
	 * 把 fmt 转成 C 串
	 */
	fmt_str = text_to_cstring(fmt);
	fmt_len = strlen(fmt_str);

	/*
	 * 为 C 串形式的结果分配工作区
	 */
	result = palloc((fmt_len * DCH_MAX_ITEM_SIZ) + 1);
	*result = '\0';

	if (fmt_len > DCH_CACHE_SIZE)
	{
		/*
		 * 若格式模式大于静态缓存则分配新内存，并且
		 * 不使用缓存（总调用解析器）
		 */
		incache = false;

		format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));

		parse_format(format, fmt_str, DCH_keywords,
					 DCH_suff, DCH_index, DCH_FLAG, NULL);
	}
	else
	{
		/*
		 * 使用缓存
		 */
		DCHCacheEntry *ent = DCH_cache_fetch(fmt_str, false);

		incache = true;
		format = ent->format;
	}

	/* 真正的工作在此 */
	DCH_to_char(format, is_interval, tmtc, result, collid);

	if (!incache)
		pfree(format);

	pfree(fmt_str);

	/* 把 C 串结果转为 TEXT 格式 */
	res = cstring_to_text(result);

	pfree(result);
	return res;
}

/****************************************************************************
 *			公共例程
 ***************************************************************************/

/* -------------------
 * TIMESTAMP to_char()
 * -------------------
 */
Datum
timestamp_to_char(PG_FUNCTION_ARGS)
{
	Timestamp	dt = PG_GETARG_TIMESTAMP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1),
			   *res;
	TmToChar	tmtc;
	struct pg_tm tt;
	struct fmt_tm *tm;
	int			thisdate;

	if (VARSIZE_ANY_EXHDR(fmt) <= 0 || TIMESTAMP_NOT_FINITE(dt))
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	if (timestamp2tm(dt, NULL, &tt, &tmtcFsec(&tmtc), NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/* 计算 wday 和 yday，因为 timestamp2tm 不算 */
	thisdate = date2j(tt.tm_year, tt.tm_mon, tt.tm_mday);
	tt.tm_wday = (thisdate + 1) % 7;
	tt.tm_yday = thisdate - date2j(tt.tm_year, 1, 1) + 1;

	COPY_tm(tm, &tt);

	if (!(res = datetime_to_char_body(&tmtc, fmt, false, PG_GET_COLLATION())))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}

Datum
timestamptz_to_char(PG_FUNCTION_ARGS)
{
	TimestampTz dt = PG_GETARG_TIMESTAMP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1),
			   *res;
	TmToChar	tmtc;
	int			tz;
	struct pg_tm tt;
	struct fmt_tm *tm;
	int			thisdate;

	if (VARSIZE_ANY_EXHDR(fmt) <= 0 || TIMESTAMP_NOT_FINITE(dt))
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	if (timestamp2tm(dt, &tz, &tt, &tmtcFsec(&tmtc), &tmtcTzn(&tmtc), NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/* 计算 wday 和 yday，因为 timestamp2tm 不算 */
	thisdate = date2j(tt.tm_year, tt.tm_mon, tt.tm_mday);
	tt.tm_wday = (thisdate + 1) % 7;
	tt.tm_yday = thisdate - date2j(tt.tm_year, 1, 1) + 1;

	COPY_tm(tm, &tt);

	if (!(res = datetime_to_char_body(&tmtc, fmt, false, PG_GET_COLLATION())))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}


/* -------------------
 * INTERVAL to_char()
 * -------------------
 */
Datum
interval_to_char(PG_FUNCTION_ARGS)
{
	Interval   *it = PG_GETARG_INTERVAL_P(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1),
			   *res;
	TmToChar	tmtc;
	struct fmt_tm *tm;
	struct pg_itm tt,
			   *itm = &tt;

	if (VARSIZE_ANY_EXHDR(fmt) <= 0)
		PG_RETURN_NULL();

	ZERO_tmtc(&tmtc);
	tm = tmtcTm(&tmtc);

	interval2itm(*it, itm);
	tmtc.fsec = itm->tm_usec;
	tm->tm_sec = itm->tm_sec;
	tm->tm_min = itm->tm_min;
	tm->tm_hour = itm->tm_hour;
	tm->tm_mday = itm->tm_mday;
	tm->tm_mon = itm->tm_mon;
	tm->tm_year = itm->tm_year;

	/* wday 无意义，yday 近似以天计的总跨度 */
	tm->tm_yday = (tm->tm_year * MONTHS_PER_YEAR + tm->tm_mon) * DAYS_PER_MONTH + tm->tm_mday;

	if (!(res = datetime_to_char_body(&tmtc, fmt, true, PG_GET_COLLATION())))
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(res);
}

/* ---------------------
 * TO_TIMESTAMP()
 *
 * 从按参数 'fmt' 格式化的 date_str 构造 Timestamp
 * （to_timestamp 为 to_char() 逆）
 * ---------------------
 */
Datum
to_timestamp(PG_FUNCTION_ARGS)
{
	text	   *date_txt = PG_GETARG_TEXT_PP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	Oid			collid = PG_GET_COLLATION();
	Timestamp	result;
	int			tz;
	struct pg_tm tm;
	fsec_t		fsec;
	int			fprec;

	do_to_timestamp(date_txt, fmt, collid, false,
					&tm, &fsec, &fprec, NULL, NULL);

	/* 使用指定的时区（如有）。 */
	if (tm.tm_zone)
	{
		DateTimeErrorExtra extra;
		int			dterr = DecodeTimezone(tm.tm_zone, &tz);

		if (dterr)
			DateTimeParseError(dterr, &extra, text_to_cstring(date_txt),
							   "timestamptz", NULL);
	}
	else
		tz = DetermineTimeZoneOffset(&tm, session_timezone);

	if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	/* 使用指定的小数精度（如有）。 */
	if (fprec)
		AdjustTimestampForTypmod(&result, fprec, NULL);

	PG_RETURN_TIMESTAMP(result);
}

/* ----------
 * TO_DATE
 *	从按参数 'fmt' 格式化的 date_str 构造 Date
 * ----------
 */
Datum
to_date(PG_FUNCTION_ARGS)
{
	text	   *date_txt = PG_GETARG_TEXT_PP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	Oid			collid = PG_GET_COLLATION();
	DateADT		result;
	struct pg_tm tm;
	fsec_t		fsec;

	do_to_timestamp(date_txt, fmt, collid, false,
					&tm, &fsec, NULL, NULL, NULL);

	/* 防止儒略日例程溢出 */
	if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: \"%s\"",
						text_to_cstring(date_txt))));

	result = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

	/* 现在检查略超范围的日期 */
	if (!IS_VALID_DATE(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: \"%s\"",
						text_to_cstring(date_txt))));

	PG_RETURN_DATEADT(result);
}

/*
 * 使用参数 'fmt' 作为格式串，把 'date_txt' 输入转换为
 * datetime 类型。排序规则 'collid' 在某些情形下可用于
 * 大小写折叠规则。'strict' 指定标准解析模式。
 *
 * 实际数据类型（在 'typid'、'typmod' 中返回）由格式串
 * 中日期/时间/时区成分是否出现来决定。
 *
 * 当时区成分存在时，相应偏移量返回在 '*tz' 中。
 *
 *
 * 若 escontext 指向 ErrorSaveContext，数据错误将通过填充
 * 该结构体来报告；调用者必须测试 SOFT_ERROR_OCCURRED()
 * 来看是否发生了错误。否则抛出错误。
 */
Datum
parse_datetime(text *date_txt, text *fmt, Oid collid, bool strict,
			   Oid *typid, int32 *typmod, int *tz,
			   Node *escontext)
{
	struct pg_tm tm;
	fsec_t		fsec;
	int			fprec;
	uint32		flags;

	if (!do_to_timestamp(date_txt, fmt, collid, strict,
						 &tm, &fsec, &fprec, &flags, escontext))
		return (Datum) 0;

	*typmod = fprec ? fprec : -1;	/* 小数部分精度 */

	if (flags & DCH_DATED)
	{
		if (flags & DCH_TIMED)
		{
			if (flags & DCH_ZONED)
			{
				TimestampTz result;

				if (tm.tm_zone)
				{
					DateTimeErrorExtra extra;
					int			dterr = DecodeTimezone(tm.tm_zone, tz);

					if (dterr)
					{
						DateTimeParseError(dterr, &extra,
										   text_to_cstring(date_txt),
										   "timestamptz", escontext);
						return (Datum) 0;
					}
				}
				else
				{
					/*
					 * 格式串有时区成分但输入串没有。假定
					 * do_to_timestamp() 不报错，这只应在
					 * 非严格情形下发生。
					 */
					Assert(!strict);

					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
							 errmsg("missing time zone in input string for type timestamptz")));
				}

				if (tm2timestamp(&tm, fsec, tz, &result) != 0)
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamptz out of range")));

				AdjustTimestampForTypmod(&result, *typmod, escontext);

				*typid = TIMESTAMPTZOID;
				return TimestampTzGetDatum(result);
			}
			else
			{
				Timestamp	result;

				if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));

				AdjustTimestampForTypmod(&result, *typmod, escontext);

				*typid = TIMESTAMPOID;
				return TimestampGetDatum(result);
			}
		}
		else
		{
			if (flags & DCH_ZONED)
			{
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
						 errmsg("datetime format is zoned but not timed")));
			}
			else
			{
				DateADT		result;

				/* 防止儒略日例程溢出 */
				if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("date out of range: \"%s\"",
									text_to_cstring(date_txt))));

				result = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) -
					POSTGRES_EPOCH_JDATE;

				/* 现在检查略超范围的日期 */
				if (!IS_VALID_DATE(result))
					ereturn(escontext, (Datum) 0,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("date out of range: \"%s\"",
									text_to_cstring(date_txt))));

				*typid = DATEOID;
				return DateADTGetDatum(result);
			}
		}
	}
	else if (flags & DCH_TIMED)
	{
		if (flags & DCH_ZONED)
		{
			TimeTzADT  *result = palloc(sizeof(TimeTzADT));

			if (tm.tm_zone)
			{
				DateTimeErrorExtra extra;
				int			dterr = DecodeTimezone(tm.tm_zone, tz);

				if (dterr)
				{
					DateTimeParseError(dterr, &extra,
									   text_to_cstring(date_txt),
									   "timetz", escontext);
					return (Datum) 0;
				}
			}
			else
			{
				/*
				 * 格式串有时区成分但输入串没有。假定
				 * do_to_timestamp() 不报错，这只应在
				 * 非严格情形下发生。
				 */
				Assert(!strict);

				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
						 errmsg("missing time zone in input string for type timetz")));
			}

			if (tm2timetz(&tm, fsec, *tz, result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timetz out of range")));

			AdjustTimeForTypmod(&result->time, *typmod);

			*typid = TIMETZOID;
			return TimeTzADTPGetDatum(result);
		}
		else
		{
			TimeADT		result;

			if (tm2time(&tm, fsec, &result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("time out of range")));

			AdjustTimeForTypmod(&result, *typmod);

			*typid = TIMEOID;
			return TimeADTGetDatum(result);
		}
	}
	else
	{
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("datetime format is not dated and not timed")));
	}
}

/*
 * do_to_timestamp：to_timestamp 与 to_date 共用代码
 *
 * 按 'fmt' 解析 'date_txt'，把结果作为 struct pg_tm、
 * 小数秒与小数精度返回。
 *
 * 'collid' 标识所需排序规则（如需）。
 * 'std' 指定标准解析模式。
 *
 * 'fmt' 中找到的日期/时间/时区成分的位掩码在 'flags'
 * 若非 NULL。
 *
 * 成功返回真，失败返回假（若 escontext 指向
 * ErrorSaveContext；否则抛出错误）。注意当前仅为
 * 坏数据提供软错误行为，坏格式则没有。
 *
 * 我们把 'fmt' 解析为 FormatNodes 列表，随后传给
 * DCH_from_char，以 'date_txt' 的解析结果填充
 * TmFromChar
 *
 * 随后分析 TmFromChar 并转换为最终结果，存入结构体
 * 'tm'、'fsec' 与 'fprec'。
 */
static bool
do_to_timestamp(text *date_txt, text *fmt, Oid collid, bool std,
				struct pg_tm *tm, fsec_t *fsec, int *fprec,
				uint32 *flags, Node *escontext)
{
	FormatNode *format = NULL;
	TmFromChar	tmfc;
	int			fmt_len;
	char	   *date_str;
	int			fmask;
	bool		incache = false;

	Assert(tm != NULL);
	Assert(fsec != NULL);

	date_str = text_to_cstring(date_txt);

	ZERO_tmfc(&tmfc);
	ZERO_tm(tm);
	*fsec = 0;
	if (fprec)
		*fprec = 0;
	if (flags)
		*flags = 0;
	fmask = 0;					/* ValidateDate() 的位掩码 */

	fmt_len = VARSIZE_ANY_EXHDR(fmt);

	if (fmt_len)
	{
		char	   *fmt_str;

		fmt_str = text_to_cstring(fmt);

		if (fmt_len > DCH_CACHE_SIZE)
		{
			/*
			 * 若格式模式大于静态缓存则分配新内存，
			 * 不使用缓存（总调用解析器）
			 */
			format = (FormatNode *) palloc((fmt_len + 1) * sizeof(FormatNode));

			parse_format(format, fmt_str, DCH_keywords, DCH_suff, DCH_index,
						 DCH_FLAG | (std ? STD_FLAG : 0), NULL);
		}
		else
		{
			/*
			 * 使用缓存
			 */
			DCHCacheEntry *ent = DCH_cache_fetch(fmt_str, std);

			incache = true;
			format = ent->format;
		}

#ifdef DEBUG_TO_FROM_CHAR
		/* dump_node(format, fmt_len); */
		/* dump_index(DCH_keywords, DCH_index); */
#endif

		DCH_from_char(format, date_str, &tmfc, collid, std, escontext);
		pfree(fmt_str);
		if (SOFT_ERROR_OCCURRED(escontext))
			goto fail;

		if (flags)
			*flags = DCH_datetime_type(format);

		if (!incache)
		{
			pfree(format);
			format = NULL;
		}
	}

	DEBUG_TMFC(&tmfc);

	/*
	 * 把 to_date/to_timestamp 输入字段转换为标准 'tm'
	 */
	if (tmfc.ssss)
	{
		int			x = tmfc.ssss;

		tm->tm_hour = x / SECS_PER_HOUR;
		x %= SECS_PER_HOUR;
		tm->tm_min = x / SECS_PER_MINUTE;
		x %= SECS_PER_MINUTE;
		tm->tm_sec = x;
	}

	if (tmfc.ss)
		tm->tm_sec = tmfc.ss;
	if (tmfc.mi)
		tm->tm_min = tmfc.mi;
	if (tmfc.hh)
		tm->tm_hour = tmfc.hh;

	if (tmfc.clock == CLOCK_12_HOUR)
	{
		if (tm->tm_hour < 1 || tm->tm_hour > HOURS_PER_DAY / 2)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("hour \"%d\" is invalid for the 12-hour clock",
							tm->tm_hour),
					 errhint("Use the 24-hour clock, or give an hour between 1 and 12.")));
			goto fail;
		}

		if (tmfc.pm && tm->tm_hour < HOURS_PER_DAY / 2)
			tm->tm_hour += HOURS_PER_DAY / 2;
		else if (!tmfc.pm && tm->tm_hour == HOURS_PER_DAY / 2)
			tm->tm_hour = 0;
	}

	if (tmfc.year)
	{
		/*
		 * 若给出 CC 与 YY（或 Y），则用 YY 作给定世纪中
		 * 年份的低 2 位数字。注意公元 21 世纪是从 2001 到
		 * 2100，而非 2000-2099；公元前 6 世纪是从 600BC 到
		 * 501BC。
		 */
		if (tmfc.cc && tmfc.yysz <= 2)
		{
			if (tmfc.bc)
				tmfc.cc = -tmfc.cc;
			tm->tm_year = tmfc.year % 100;
			if (tm->tm_year)
			{
				if (tmfc.cc >= 0)
					tm->tm_year += (tmfc.cc - 1) * 100;
				else
					tm->tm_year = (tmfc.cc + 1) * 100 - tm->tm_year + 1;
			}
			else
			{
				/* 找出以"00"结尾日期的世纪年 */
				tm->tm_year = tmfc.cc * 100 + ((tmfc.cc >= 0) ? 0 : 1);
			}
		}
		else
		{
			/* 若给了 4 位年份，则用它并忽略 CC。 */
			tm->tm_year = tmfc.year;
			if (tmfc.bc)
				tm->tm_year = -tm->tm_year;
			/* 校正我们的公元前年表示法 */
			if (tm->tm_year < 0)
				tm->tm_year++;
		}
		fmask |= DTK_M(YEAR);
	}
	else if (tmfc.cc)
	{
		/* 使用世纪首年 */
		if (tmfc.bc)
			tmfc.cc = -tmfc.cc;
		if (tmfc.cc >= 0)
			/* +1 因为 21 世纪始于 2001 */
			tm->tm_year = (tmfc.cc - 1) * 100 + 1;
		else
			/* +1 因为 year == 599 是 600 BC */
			tm->tm_year = tmfc.cc * 100 + 1;
		fmask |= DTK_M(YEAR);
	}

	if (tmfc.j)
	{
		j2date(tmfc.j, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
		fmask |= DTK_DATE_M;
	}

	if (tmfc.ww)
	{
		if (tmfc.mode == FROM_CHAR_DATE_ISOWEEK)
		{
			/*
			 * 若 tmfc.d 未设置，则日期留在 ISO 周的开头
			 * （周一）。
			 */
			if (tmfc.d)
				isoweekdate2date(tmfc.ww, tmfc.d, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
			else
				isoweek2date(tmfc.ww, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
			fmask |= DTK_DATE_M;
		}
		else
			tmfc.ddd = (tmfc.ww - 1) * 7 + 1;
	}

	if (tmfc.w)
		tmfc.dd = (tmfc.w - 1) * 7 + 1;
	if (tmfc.dd)
	{
		tm->tm_mday = tmfc.dd;
		fmask |= DTK_M(DAY);
	}
	if (tmfc.mm)
	{
		tm->tm_mon = tmfc.mm;
		fmask |= DTK_M(MONTH);
	}

	if (tmfc.ddd && (tm->tm_mon <= 1 || tm->tm_mday <= 1))
	{
		/*
		 * 月和日字段未设置，故用年内日字段来
		 * 填充它们。依日期模式不同，该字段可能被解释
		 * 为公历年内日，或 ISO 周日期年内日。
		 *
		 */

		if (!tm->tm_year && !tmfc.bc)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("cannot calculate day of year without year information")));
			goto fail;
		}

		if (tmfc.mode == FROM_CHAR_DATE_ISOWEEK)
		{
			int			j0;		/* ISO 年的第零日（儒略日） */

			j0 = isoweek2j(tm->tm_year, 1) - 1;

			j2date(j0 + tmfc.ddd, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
			fmask |= DTK_DATE_M;
		}
		else
		{
			const int  *y;
			int			i;

			static const int ysum[2][13] = {
				{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
			{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

			y = ysum[isleap(tm->tm_year)];

			for (i = 1; i <= MONTHS_PER_YEAR; i++)
			{
				if (tmfc.ddd <= y[i])
					break;
			}
			if (tm->tm_mon <= 1)
				tm->tm_mon = i;

			if (tm->tm_mday <= 1)
				tm->tm_mday = tmfc.ddd - y[i - 1];

			fmask |= DTK_M(MONTH) | DTK_M(DAY);
		}
	}

	if (tmfc.ms)
		*fsec += tmfc.ms * 1000;
	if (tmfc.us)
		*fsec += tmfc.us;
	if (fprec)
		*fprec = tmfc.ff;		/* 小数部分精度（如指定） */

	/* 按上面算得的位掩码范围检查日期字段 */
	if (fmask != 0)
	{
		/* 已处理过 AD/BC，故传 isjulian = true */
		int			dterr = ValidateDate(fmask, true, false, false, tm);

		if (dterr != 0)
		{
			/*
			 * 即使 ValidateDate 说的是 DTERR_MD_FIELD_OVERFLOW，
			 * 也要把错误定为 DTERR_FIELD_OVERFLOW，以免打印
			 * 无关的 datestyle 提示。
			 */
			DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
							   date_str, "timestamp", escontext);
			goto fail;
		}
	}

	/* 也检查时间字段 */
	if (tm->tm_hour < 0 || tm->tm_hour >= HOURS_PER_DAY ||
		tm->tm_min < 0 || tm->tm_min >= MINS_PER_HOUR ||
		tm->tm_sec < 0 || tm->tm_sec >= SECS_PER_MINUTE ||
		*fsec < INT64CONST(0) || *fsec >= USECS_PER_SEC)
	{
		DateTimeParseError(DTERR_FIELD_OVERFLOW, NULL,
						   date_str, "timestamp", escontext);
		goto fail;
	}

	/* 若有指定则把解析出的时区存入 tm->tm_zone */
	if (tmfc.tzsign)
	{
		char	   *tz;

		if (tmfc.tzh < 0 || tmfc.tzh > MAX_TZDISP_HOUR ||
			tmfc.tzm < 0 || tmfc.tzm >= MINS_PER_HOUR)
		{
			DateTimeParseError(DTERR_TZDISP_OVERFLOW, NULL,
							   date_str, "timestamp", escontext);
			goto fail;
		}

		tz = psprintf("%c%02d:%02d",
					  tmfc.tzsign > 0 ? '+' : '-', tmfc.tzh, tmfc.tzm);

		tm->tm_zone = tz;
	}

	DEBUG_TM(tm);

	if (format && !incache)
		pfree(format);
	pfree(date_str);

	return true;

fail:
	if (format && !incache)
		pfree(format);
	pfree(date_str);

	return false;
}


/**********************************************************************
 *	NUMBER 版部分
 *********************************************************************/


static char *
fill_str(char *str, int c, int max)
{
	memset(str, c, max);
	*(str + max) = '\0';
	return str;
}

#define zeroize_NUM(_n) \
do { \
	(_n)->flag		= 0;	\
	(_n)->lsign		= 0;	\
	(_n)->pre		= 0;	\
	(_n)->post		= 0;	\
	(_n)->pre_lsign_num = 0;	\
	(_n)->need_locale	= 0;	\
	(_n)->multi		= 0;	\
	(_n)->zero_start	= 0;	\
	(_n)->zero_end		= 0;	\
} while(0)

/* 工作方式同 DCH_prevent_counter_overflow */
static inline void
NUM_prevent_counter_overflow(void)
{
	if (NUMCounter >= (INT_MAX - 1))
	{
		for (int i = 0; i < n_NUMCache; i++)
			NUMCache[i]->age >>= 1;
		NUMCounter >>= 1;
	}
}

/* 选一个 NUMCacheEntry 来容纳给定的格式模式 */
static NUMCacheEntry *
NUM_cache_getnew(const char *str)
{
	NUMCacheEntry *ent;

	/* 确保下面能递增 NUMCounter */
	NUM_prevent_counter_overflow();

	/*
	 * 若缓存已满，移除最老条目（或回收首个无效条目）
	 */
	if (n_NUMCache >= NUM_CACHE_ENTRIES)
	{
		NUMCacheEntry *old = NUMCache[0];

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Cache is full (%d)", n_NUMCache);
#endif
		if (old->valid)
		{
			for (int i = 1; i < NUM_CACHE_ENTRIES; i++)
			{
				ent = NUMCache[i];
				if (!ent->valid)
				{
					old = ent;
					break;
				}
				if (ent->age < old->age)
					old = ent;
			}
		}
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "OLD: \"%s\" AGE: %d", old->str, old->age);
#endif
		old->valid = false;
		strlcpy(old->str, str, NUM_CACHE_SIZE + 1);
		old->age = (++NUMCounter);
		/* 期望调用者填好 format 和 Num 后再置为有效 */
		return old;
	}
	else
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "NEW (%d)", n_NUMCache);
#endif
		Assert(NUMCache[n_NUMCache] == NULL);
		NUMCache[n_NUMCache] = ent = (NUMCacheEntry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(NUMCacheEntry));
		ent->valid = false;
		strlcpy(ent->str, str, NUM_CACHE_SIZE + 1);
		ent->age = (++NUMCounter);
		/* 期望调用者填好 format 和 Num 后再置为有效 */
		++n_NUMCache;
		return ent;
	}
}

/* 查找与给定格式模式匹配的既有 NUMCacheEntry */
static NUMCacheEntry *
NUM_cache_search(const char *str)
{
	/* 确保下面能递增 NUMCounter */
	NUM_prevent_counter_overflow();

	for (int i = 0; i < n_NUMCache; i++)
	{
		NUMCacheEntry *ent = NUMCache[i];

		if (ent->valid && strcmp(ent->str, str) == 0)
		{
			ent->age = (++NUMCounter);
			return ent;
		}
	}

	return NULL;
}

/* 为给定格式模式查找或创建 NUMCacheEntry */
static NUMCacheEntry *
NUM_cache_fetch(const char *str)
{
	NUMCacheEntry *ent;

	if ((ent = NUM_cache_search(str)) == NULL)
	{
		/*
		 * 不在缓存中，须运行解析器并把新格式模式存
		 * 缓存。解析成功前不要标记条目为有效。
		 *
		 */
		ent = NUM_cache_getnew(str);

		zeroize_NUM(&ent->Num);

		parse_format(ent->format, str, NUM_keywords,
					 NULL, NUM_index, NUM_FLAG, &ent->Num);

		ent->valid = true;
	}
	return ent;
}

/* ----------
 * NUM to_char 版的缓存例程
 * ----------
 */
static FormatNode *
NUM_cache(int len, NUMDesc *Num, text *pars_str, bool *shouldFree)
{
	FormatNode *format = NULL;
	char	   *str;

	str = text_to_cstring(pars_str);

	if (len > NUM_CACHE_SIZE)
	{
		/*
		 * 若格式模式大于静态缓存则分配新内存，并且
		 * 不使用缓存（总调用解析器）
		 */
		format = (FormatNode *) palloc((len + 1) * sizeof(FormatNode));

		*shouldFree = true;

		zeroize_NUM(Num);

		parse_format(format, str, NUM_keywords,
					 NULL, NUM_index, NUM_FLAG, Num);
	}
	else
	{
		/*
		 * 使用缓存
		 */
		NUMCacheEntry *ent = NUM_cache_fetch(str);

		*shouldFree = false;

		format = ent->format;

		/*
		 * 复制缓存到结构体
		 */
		Num->flag = ent->Num.flag;
		Num->lsign = ent->Num.lsign;
		Num->pre = ent->Num.pre;
		Num->post = ent->Num.post;
		Num->pre_lsign_num = ent->Num.pre_lsign_num;
		Num->need_locale = ent->Num.need_locale;
		Num->multi = ent->Num.multi;
		Num->zero_start = ent->Num.zero_start;
		Num->zero_end = ent->Num.zero_end;
	}

#ifdef DEBUG_TO_FROM_CHAR
	/* dump_node(format, len); */
	dump_index(NUM_keywords, NUM_index);
#endif

	pfree(str);
	return format;
}


static char *
int_to_roman(int number)
{
	int			len,
				num;
	char	   *p,
			   *result,
				numstr[12];

	result = (char *) palloc(16);
	*result = '\0';

	if (number > 3999 || number < 1)
	{
		fill_str(result, '#', 15);
		return result;
	}
	len = snprintf(numstr, sizeof(numstr), "%d", number);

	for (p = numstr; *p != '\0'; p++, --len)
	{
		num = *p - ('0' + 1);
		if (num < 0)
			continue;

		if (len > 3)
		{
			while (num-- != -1)
				strcat(result, "M");
		}
		else
		{
			if (len == 3)
				strcat(result, rm100[num]);
			else if (len == 2)
				strcat(result, rm10[num]);
			else if (len == 1)
				strcat(result, rm1[num]);
		}
	}
	return result;
}



/* ----------
 * Locale
 * ----------
 */
static void
NUM_prepare_locale(NUMProc *Np)
{
	if (Np->Num->need_locale)
	{
		struct lconv *lconv;

		/*
		 * 取区域
		 */
		lconv = PGLC_localeconv();

		/*
		 * 正/负数符号
		 */
		if (lconv->negative_sign && *lconv->negative_sign)
			Np->L_negative_sign = lconv->negative_sign;
		else
			Np->L_negative_sign = "-";

		if (lconv->positive_sign && *lconv->positive_sign)
			Np->L_positive_sign = lconv->positive_sign;
		else
			Np->L_positive_sign = "+";

		/*
		 * 数字小数点
		 */
		if (lconv->decimal_point && *lconv->decimal_point)
			Np->decimal = lconv->decimal_point;

		else
			Np->decimal = ".";

		if (!IS_LDECIMAL(Np->Num))
			Np->decimal = ".";

		/*
		 * 数字千位分隔符
		 *
		 * 某些区域设置（如 glibc pt_BR 的缺陷）小数点用
		 * 逗号，千位分隔符为""，故我们也设置之。
		 * http://archives.postgresql.org/pgsql-hackers/2007-11/msg00772.php
		 */
		if (lconv->thousands_sep && *lconv->thousands_sep)
			Np->L_thousands_sep = lconv->thousands_sep;
		/* 确保千位分隔符不与小数点符号匹配。 */
		else if (strcmp(Np->decimal, ",") != 0)
			Np->L_thousands_sep = ",";
		else
			Np->L_thousands_sep = ".";

		/*
		 * 货币符号
		 */
		if (lconv->currency_symbol && *lconv->currency_symbol)
			Np->L_currency_symbol = lconv->currency_symbol;
		else
			Np->L_currency_symbol = " ";
	}
	else
	{
		/*
		 * 默认值
		 */
		Np->L_negative_sign = "-";
		Np->L_positive_sign = "+";
		Np->decimal = ".";

		Np->L_thousands_sep = ",";
		Np->L_currency_symbol = " ";
	}
}

/* ----------
 * 返回小数点后最后一个有效数字的指针
 *	12.0500 --> 最后有效是 '5'
 *	12.0000 --> 最后有效是 '.'
 * 若无小数点，则返回 NULL（结果将与未指定 FM 时
 * 的行为相同）。
 * ----------
 */
static char *
get_last_relevant_decnum(char *num)
{
	char	   *result,
			   *p = strchr(num, '.');

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "get_last_relevant_decnum()");
#endif

	if (!p)
		return NULL;

	result = p;

	while (*(++p))
	{
		if (*p != '0')
			result = p;
	}

	return result;
}

/*
 * 这些宏用于 NUM_processor() 及其附属例程。
 * OVERLOAD_TEST：若已到达输入串末尾则为真
 * AMOUNT_TEST(s)：若串中至少剩 s 字节则为真
 */
#define OVERLOAD_TEST	(Np->inout_p >= Np->inout + input_len)
#define AMOUNT_TEST(s)	(Np->inout_p <= Np->inout + (input_len - (s)))

/* ----------
 * TO_NUMBER() 的数字提取
 * ----------
 */
static void
NUM_numpart_from_char(NUMProc *Np, int id, int input_len)
{
	bool		isread = false;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, " --- scan start --- id=%s",
		 (id == NUM_0 || id == NUM_9) ? "NUM_0/9" : id == NUM_DEC ? "NUM_DEC" : "???");
#endif

	if (OVERLOAD_TEST)
		return;

	if (*Np->inout_p == ' ')
		Np->inout_p++;

	if (OVERLOAD_TEST)
		return;

	/*
	 * 数字前读符号
	 */
	if (*Np->number == ' ' && (id == NUM_0 || id == NUM_9) &&
		(Np->read_pre + Np->read_post) == 0)
	{
#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Try read sign (%c), locale positive: %s, negative: %s",
			 *Np->inout_p, Np->L_positive_sign, Np->L_negative_sign);
#endif

		/*
		 * locale sign
		 */
		if (IS_LSIGN(Np->Num) && Np->Num->lsign == NUM_LSIGN_PRE)
		{
			int			x = 0;

#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read locale pre-sign (%c)", *Np->inout_p);
#endif
			if ((x = strlen(Np->L_negative_sign)) &&
				AMOUNT_TEST(x) &&
				strncmp(Np->inout_p, Np->L_negative_sign, x) == 0)
			{
				Np->inout_p += x;
				*Np->number = '-';
			}
			else if ((x = strlen(Np->L_positive_sign)) &&
					 AMOUNT_TEST(x) &&
					 strncmp(Np->inout_p, Np->L_positive_sign, x) == 0)
			{
				Np->inout_p += x;
				*Np->number = '+';
			}
		}
		else
		{
#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read simple sign (%c)", *Np->inout_p);
#endif

			/*
			 * simple + - < >
			 */
			if (*Np->inout_p == '-' || (IS_BRACKET(Np->Num) &&
										*Np->inout_p == '<'))
			{
				*Np->number = '-';	/* 置 - */
				Np->inout_p++;
			}
			else if (*Np->inout_p == '+')
			{
				*Np->number = '+';	/* 置 + */
				Np->inout_p++;
			}
		}
	}

	if (OVERLOAD_TEST)
		return;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output, "Scan for numbers (%c), current number: '%s'", *Np->inout_p, Np->number);
#endif

	/*
	 * 读取数字或小数点
	 */
	if (isdigit((unsigned char) *Np->inout_p))
	{
		if (Np->read_dec && Np->read_post == Np->Num->post)
			return;

		*Np->number_p = *Np->inout_p;
		Np->number_p++;

		if (Np->read_dec)
			Np->read_post++;
		else
			Np->read_pre++;

		isread = true;

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Read digit (%c)", *Np->inout_p);
#endif
	}
	else if (IS_DECIMAL(Np->Num) && Np->read_dec == false)
	{
		/*
		 * 此处无需显式测试 IS_LDECIMAL(Np->Num)，因为
		 * 若没有 D 格式 token，Np->decimal 就总是 "."。
		 * 所以仅无条件与 Np->decimal 匹配。
		 */
		int			x = strlen(Np->decimal);

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "Try read decimal point (%c)",
			 *Np->inout_p);
#endif
		if (x && AMOUNT_TEST(x) && strncmp(Np->inout_p, Np->decimal, x) == 0)
		{
			Np->inout_p += x - 1;
			*Np->number_p = '.';
			Np->number_p++;
			Np->read_dec = true;
			isread = true;
		}
	}

	if (OVERLOAD_TEST)
		return;

	/*
	 * 读"最后"数字后的符号
	 *
	 * 需要检测符号，因为后置符号的精确位置难以确定：
	 *
	 *
	 * FM9999.9999999S	   -> 123.001- 9.9S			   -> .5- FM9.999999MI ->
	 * 5.01-
	 */
	if (*Np->number == ' ' && Np->read_pre + Np->read_post > 0)
	{
		/*
		 * 区域符号（NUM_S）总锚定在末个数字后，若：
		 * 预期为区域符号 - 末读字符为 NUM_0/9 或 NUM_DEC
		 * 且下字符非数字
		 */
		if (IS_LSIGN(Np->Num) && isread &&
			(Np->inout_p + 1) < Np->inout + input_len &&
			!isdigit((unsigned char) *(Np->inout_p + 1)))
		{
			int			x;
			char	   *tmp = Np->inout_p++;

#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read locale post-sign (%c)", *Np->inout_p);
#endif
			if ((x = strlen(Np->L_negative_sign)) &&
				AMOUNT_TEST(x) &&
				strncmp(Np->inout_p, Np->L_negative_sign, x) == 0)
			{
				Np->inout_p += x - 1;	/* -1 .. NUM_processor()会 inout_p++ */
				*Np->number = '-';
			}
			else if ((x = strlen(Np->L_positive_sign)) &&
					 AMOUNT_TEST(x) &&
					 strncmp(Np->inout_p, Np->L_positive_sign, x) == 0)
			{
				Np->inout_p += x - 1;	/* -1 .. NUM_processor()会 inout_p++ */
				*Np->number = '+';
			}
			if (*Np->number == ' ')
				/* 未读符号 */
				Np->inout_p = tmp;
		}

		/*
		 * 尝试读非区域符号，仅当格式不精确时，如：
		 * 且我们无法确定 MI/PL/SG 的符号位置：
		 *
		 * FM9.999999MI			   -> 5.01-
		 *
		 * if (.... && IS_LSIGN(Np->Num)==false) 防止误读
		 * 如 to_number('1 -', '9S')，符号未锚定于最后
		 * 数字
		 */
		else if (isread == false && IS_LSIGN(Np->Num) == false &&
				 (IS_PLUS(Np->Num) || IS_MINUS(Np->Num)))
		{
#ifdef DEBUG_TO_FROM_CHAR
			elog(DEBUG_elog_output, "Try read simple post-sign (%c)", *Np->inout_p);
#endif

			/*
			 * 简单 +-
			 */
			if (*Np->inout_p == '-' || *Np->inout_p == '+')
				/* NUM_processor() inout_p++ */
				*Np->number = *Np->inout_p;
		}
	}
}

#define IS_PREDEC_SPACE(_n) \
		(IS_ZERO((_n)->Num)==false && \
		 (_n)->number == (_n)->number_p && \
		 *(_n)->number == '0' && \
				 (_n)->Num->post != 0)

/* ----------
 * 加数字或符号到数字串
 * ----------
 */
static void
NUM_numpart_to_char(NUMProc *Np, int id)
{
	int			end;

	if (IS_ROMAN(Np->Num))
		return;

	/* 注意：elog() 输出未在 'inout' 中置 '\0' */

#ifdef DEBUG_TO_FROM_CHAR

	/*
	 * Np->num_curr 是格式模式中当前条目的序号，它不是
	 * inout 的当前位置！
	 */
	elog(DEBUG_elog_output,
		 "SIGN_WROTE: %d, CURRENT: %d, NUMBER_P: \"%s\", INOUT: \"%s\"",
		 Np->sign_wrote,
		 Np->num_curr,
		 Np->number_p,
		 Np->inout);
#endif
	Np->num_in = false;

	/*
	 * 若实际数字将写往输出则写符号 注意：IS_PREDEC_SPACE()
	 * 处理 "9.9" --> " .1"
	 */
	if (Np->sign_wrote == false &&
		(Np->num_curr >= Np->out_pre_spaces || (IS_ZERO(Np->Num) && Np->Num->zero_start == Np->num_curr)) &&
		(IS_PREDEC_SPACE(Np) == false || (Np->last_relevant && *Np->last_relevant == '.')))
	{
		if (IS_LSIGN(Np->Num))
		{
			if (Np->Num->lsign == NUM_LSIGN_PRE)
			{
				if (Np->sign == '-')
					strcpy(Np->inout_p, Np->L_negative_sign);
				else
					strcpy(Np->inout_p, Np->L_positive_sign);
				Np->inout_p += strlen(Np->inout_p);
				Np->sign_wrote = true;
			}
		}
		else if (IS_BRACKET(Np->Num))
		{
			*Np->inout_p = Np->sign == '+' ? ' ' : '<';
			++Np->inout_p;
			Np->sign_wrote = true;
		}
		else if (Np->sign == '+')
		{
			if (!IS_FILLMODE(Np->Num))
			{
				*Np->inout_p = ' '; /* 写 + */
				++Np->inout_p;
			}
			Np->sign_wrote = true;
		}
		else if (Np->sign == '-')
		{						/* 写 - */
			*Np->inout_p = '-';
			++Np->inout_p;
			Np->sign_wrote = true;
		}
	}


	/*
	 * 数字 / FM / 零 / 小数点
	 */
	if (id == NUM_9 || id == NUM_0 || id == NUM_D || id == NUM_DEC)
	{
		if (Np->num_curr < Np->out_pre_spaces &&
			(Np->Num->zero_start > Np->num_curr || !IS_ZERO(Np->Num)))
		{
			/*
			 * 写空白
			 */
			if (!IS_FILLMODE(Np->Num))
			{
				*Np->inout_p = ' '; /* 写 ' ' */
				++Np->inout_p;
			}
		}
		else if (IS_ZERO(Np->Num) &&
				 Np->num_curr < Np->out_pre_spaces &&
				 Np->Num->zero_start <= Np->num_curr)
		{
			/*
			 * 写 ZERO
			 */
			*Np->inout_p = '0'; /* 写 '0' */
			++Np->inout_p;
			Np->num_in = true;
		}
		else
		{
			/*
			 * 写小数点
			 */
			if (*Np->number_p == '.')
			{
				if (!Np->last_relevant || *Np->last_relevant != '.')
				{
					strcpy(Np->inout_p, Np->decimal);	/* 写 DEC/D */
					Np->inout_p += strlen(Np->inout_p);
				}

				/*
				 * Ora 'n' -- FM9.9 --> 'n.'
				 */
				else if (IS_FILLMODE(Np->Num) &&
						 Np->last_relevant && *Np->last_relevant == '.')
				{
					strcpy(Np->inout_p, Np->decimal);	/* 写 DEC/D */
					Np->inout_p += strlen(Np->inout_p);
				}
			}
			else
			{
				/*
				 * 写数字
				 */
				if (Np->last_relevant && Np->number_p > Np->last_relevant &&
					id != NUM_0)
					;

				/*
				 * '0.1' -- 9.9 --> '  .1'
				 */
				else if (IS_PREDEC_SPACE(Np))
				{
					if (!IS_FILLMODE(Np->Num))
					{
						*Np->inout_p = ' ';
						++Np->inout_p;
					}

					/*
					 * '0' -- FM9.9 --> '0.'
					 */
					else if (Np->last_relevant && *Np->last_relevant == '.')
					{
						*Np->inout_p = '0';
						++Np->inout_p;
					}
				}
				else
				{
					*Np->inout_p = *Np->number_p;	/* 写数字 */
					++Np->inout_p;
					Np->num_in = true;
				}
			}
			/* 不超过串长度 */
			if (*Np->number_p)
				++Np->number_p;
		}

		end = Np->num_count + (Np->out_pre_spaces ? 1 : 0) + (IS_DECIMAL(Np->Num) ? 1 : 0);

		if (Np->last_relevant && Np->last_relevant == Np->number_p)
			end = Np->num_curr;

		if (Np->num_curr + 1 == end)
		{
			if (Np->sign_wrote == true && IS_BRACKET(Np->Num))
			{
				*Np->inout_p = Np->sign == '+' ? ' ' : '>';
				++Np->inout_p;
			}
			else if (IS_LSIGN(Np->Num) && Np->Num->lsign == NUM_LSIGN_POST)
			{
				if (Np->sign == '-')
					strcpy(Np->inout_p, Np->L_negative_sign);
				else
					strcpy(Np->inout_p, Np->L_positive_sign);
				Np->inout_p += strlen(Np->inout_p);
			}
		}
	}

	++Np->num_curr;
}

/*
 * 跳过 "n" 个输入字符，但仅当它们不是数字数据时
 */
static void
NUM_eat_non_data_chars(NUMProc *Np, int n, int input_len)
{
	while (n-- > 0)
	{
		if (OVERLOAD_TEST)
			break;				/* 输入结束 */
		if (strchr("0123456789.,+-", *Np->inout_p) != NULL)
			break;				/* 是数据字符 */
		Np->inout_p += pg_mblen(Np->inout_p);
	}
}

static char *
NUM_processor(FormatNode *node, NUMDesc *Num, char *inout,
			  char *number, int input_len, int to_char_out_pre_spaces,
			  int sign, bool is_to_char, Oid collid)
{
	FormatNode *n;
	NUMProc		_Np,
			   *Np = &_Np;
	const char *pattern;
	int			pattern_len;

	MemSet(Np, 0, sizeof(NUMProc));

	Np->Num = Num;
	Np->is_to_char = is_to_char;
	Np->number = number;
	Np->inout = inout;
	Np->last_relevant = NULL;
	Np->read_post = 0;
	Np->read_pre = 0;
	Np->read_dec = false;

	if (Np->Num->zero_start)
		--Np->Num->zero_start;

	if (IS_EEEE(Np->Num))
	{
		if (!Np->is_to_char)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"EEEE\" not supported for input")));
		return strcpy(inout, number);
	}

	/*
	 * 罗马修正
	 */
	if (IS_ROMAN(Np->Num))
	{
		if (!Np->is_to_char)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"RN\" not supported for input")));

		Np->Num->lsign = Np->Num->pre_lsign_num = Np->Num->post =
			Np->Num->pre = Np->out_pre_spaces = Np->sign = 0;

		if (IS_FILLMODE(Np->Num))
		{
			Np->Num->flag = 0;
			Np->Num->flag |= NUM_F_FILLMODE;
		}
		else
			Np->Num->flag = 0;
		Np->Num->flag |= NUM_F_ROMAN;
	}

	/*
	 * Sign
	 */
	if (is_to_char)
	{
		Np->sign = sign;

		/* MI/PL/SG - 写符号本身，不写入数字 */
		if (IS_PLUS(Np->Num) || IS_MINUS(Np->Num))
		{
			if (IS_PLUS(Np->Num) && IS_MINUS(Np->Num) == false)
				Np->sign_wrote = false; /* 需符号 */
			else
				Np->sign_wrote = true;	/* 无需符号 */
		}
		else
		{
			if (Np->sign != '-')
			{
				if (IS_FILLMODE(Np->Num))
					Np->Num->flag &= ~NUM_F_BRACKET;
			}

			if (Np->sign == '+' && IS_FILLMODE(Np->Num) && IS_LSIGN(Np->Num) == false)
				Np->sign_wrote = true;	/* 无需符号 */
			else
				Np->sign_wrote = false; /* 需符号 */

			if (Np->Num->lsign == NUM_LSIGN_PRE && Np->Num->pre == Np->Num->pre_lsign_num)
				Np->Num->lsign = NUM_LSIGN_POST;
		}
	}
	else
		Np->sign = false;

	/*
	 * Count
	 */
	Np->num_count = Np->Num->post + Np->Num->pre - 1;

	if (is_to_char)
	{
		Np->out_pre_spaces = to_char_out_pre_spaces;

		if (IS_FILLMODE(Np->Num) && IS_DECIMAL(Np->Num))
		{
			Np->last_relevant = get_last_relevant_decnum(Np->number);

			/*
			 * 若有 '0' 指示符，确保不剥除这些数字。
			 * 但别把 last_relevant 前进到越过 Np->number
			 * 串末字符——若数字因精度限制变短，这是
			 * 一种风险。
			 */
			if (Np->last_relevant && Np->Num->zero_end > Np->out_pre_spaces)
			{
				int			last_zero_pos;
				char	   *last_zero;

				/* 注意此处 Np->number 不可能为零长 */
				last_zero_pos = strlen(Np->number) - 1;
				last_zero_pos = Min(last_zero_pos,
									Np->Num->zero_end - Np->out_pre_spaces);
				last_zero = Np->number + last_zero_pos;
				if (Np->last_relevant < last_zero)
					Np->last_relevant = last_zero;
			}
		}

		if (Np->sign_wrote == false && Np->out_pre_spaces == 0)
			++Np->num_count;
	}
	else
	{
		Np->out_pre_spaces = 0;
		*Np->number = ' ';		/* 符号位 */
		*(Np->number + 1) = '\0';
	}

	Np->num_in = 0;
	Np->num_curr = 0;

#ifdef DEBUG_TO_FROM_CHAR
	elog(DEBUG_elog_output,
		 "\n\tSIGN: '%c'\n\tNUM: '%s'\n\tPRE: %d\n\tPOST: %d\n\tNUM_COUNT: %d\n\tNUM_PRE: %d\n\tSIGN_WROTE: %s\n\tZERO: %s\n\tZERO_START: %d\n\tZERO_END: %d\n\tLAST_RELEVANT: %s\n\tBRACKET: %s\n\tPLUS: %s\n\tMINUS: %s\n\tFILLMODE: %s\n\tROMAN: %s\n\tEEEE: %s",
		 Np->sign,
		 Np->number,
		 Np->Num->pre,
		 Np->Num->post,
		 Np->num_count,
		 Np->out_pre_spaces,
		 Np->sign_wrote ? "Yes" : "No",
		 IS_ZERO(Np->Num) ? "Yes" : "No",
		 Np->Num->zero_start,
		 Np->Num->zero_end,
		 Np->last_relevant ? Np->last_relevant : "<not set>",
		 IS_BRACKET(Np->Num) ? "Yes" : "No",
		 IS_PLUS(Np->Num) ? "Yes" : "No",
		 IS_MINUS(Np->Num) ? "Yes" : "No",
		 IS_FILLMODE(Np->Num) ? "Yes" : "No",
		 IS_ROMAN(Np->Num) ? "Yes" : "No",
		 IS_EEEE(Np->Num) ? "Yes" : "No"
		);
#endif

	/*
	 * Locale
	 */
	NUM_prepare_locale(Np);

	/*
	 * 处理器直通循环
	 */
	if (Np->is_to_char)
		Np->number_p = Np->number;
	else
		Np->number_p = Np->number + 1;	/* 首字符是符号的空位 */

	for (n = node, Np->inout_p = Np->inout; n->type != NODE_TYPE_END; n++)
	{
		if (!Np->is_to_char)
		{
			/*
			 * 检查至少还剩一个字节待扫描。（下方动作
			 * 中，若想读更多字节就必须用 AMOUNT_TEST。）
			 *
			 */
			if (OVERLOAD_TEST)
				break;
		}

		/*
		 * 格式模式动作
		 */
		if (n->type == NODE_TYPE_ACTION)
		{
			/*
			 * 创建/读取数字/零/空白/符号/特例
			 *
			 * 'NUM_S' 注：区域符号锚定于数字，我们只在
			 * 处理首个或末个数字时才读/写它
			 * （NUM_0/NUM_9）。故 switch() 无 NUM_S。
			 *
			 * 注意循环底部的 "Np->inout_p++"。这就是多数动
			 * 作为什么 inout_p 前进得比预期少一位。
			 * 在不想要该递增发生的情形，switch 分支以
			 * "continue" 结束而非 "break"。
			 */
			switch (n->key->id)
			{
				case NUM_9:
				case NUM_0:
				case NUM_DEC:
				case NUM_D:
					if (Np->is_to_char)
					{
						NUM_numpart_to_char(Np, n->key->id);
						continue;	/* for() */
					}
					else
					{
						NUM_numpart_from_char(Np, n->key->id, input_len);
						break;	/* switch() case: */
					}

				case NUM_COMMA:
					if (Np->is_to_char)
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
							else
								*Np->inout_p = ' ';
						}
						else
							*Np->inout_p = ',';
					}
					else
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
						}
						if (*Np->inout_p != ',')
							continue;
					}
					break;

				case NUM_G:
					pattern = Np->L_thousands_sep;
					pattern_len = strlen(pattern);
					if (Np->is_to_char)
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
							else
							{
								/* 以防有多字节字符 */
								pattern_len = pg_mbstrlen(pattern);
								memset(Np->inout_p, ' ', pattern_len);
								Np->inout_p += pattern_len - 1;
							}
						}
						else
						{
							strcpy(Np->inout_p, pattern);
							Np->inout_p += pattern_len - 1;
						}
					}
					else
					{
						if (!Np->num_in)
						{
							if (IS_FILLMODE(Np->Num))
								continue;
						}

						/*
						 * 因 L_thousands_sep 常含数据字符
						 * （'.' 或 ','），所以不能用
						 * NUM_eat_non_data_chars。只当输入匹配
						 * L_thousands_sep 时跳过。
						 */
						if (AMOUNT_TEST(pattern_len) &&
							strncmp(Np->inout_p, pattern, pattern_len) == 0)
							Np->inout_p += pattern_len - 1;
						else
							continue;
					}
					break;

				case NUM_L:
					pattern = Np->L_currency_symbol;
					if (Np->is_to_char)
					{
						strcpy(Np->inout_p, pattern);
						Np->inout_p += strlen(pattern) - 1;
					}
					else
					{
						NUM_eat_non_data_chars(Np, pg_mbstrlen(pattern), input_len);
						continue;
					}
					break;

				case NUM_RN:
					if (IS_FILLMODE(Np->Num))
					{
						strcpy(Np->inout_p, Np->number_p);
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					else
					{
						sprintf(Np->inout_p, "%15s", Np->number_p);
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					break;

				case NUM_rn:
					if (IS_FILLMODE(Np->Num))
					{
						strcpy(Np->inout_p, asc_tolower_z(Np->number_p));
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					else
					{
						sprintf(Np->inout_p, "%15s", asc_tolower_z(Np->number_p));
						Np->inout_p += strlen(Np->inout_p) - 1;
					}
					break;

				case NUM_th:
					if (IS_ROMAN(Np->Num) || *Np->number == '#' ||
						Np->sign == '-' || IS_DECIMAL(Np->Num))
						continue;

					if (Np->is_to_char)
					{
						strcpy(Np->inout_p, get_th(Np->number, TH_LOWER));
						Np->inout_p += 1;
					}
					else
					{
						/* 'th' 的所有变体占 2 个字符 */
						NUM_eat_non_data_chars(Np, 2, input_len);
						continue;
					}
					break;

				case NUM_TH:
					if (IS_ROMAN(Np->Num) || *Np->number == '#' ||
						Np->sign == '-' || IS_DECIMAL(Np->Num))
						continue;

					if (Np->is_to_char)
					{
						strcpy(Np->inout_p, get_th(Np->number, TH_UPPER));
						Np->inout_p += 1;
					}
					else
					{
						/* 'TH' 的所有变体占 2 个字符 */
						NUM_eat_non_data_chars(Np, 2, input_len);
						continue;
					}
					break;

				case NUM_MI:
					if (Np->is_to_char)
					{
						if (Np->sign == '-')
							*Np->inout_p = '-';
						else if (IS_FILLMODE(Np->Num))
							continue;
						else
							*Np->inout_p = ' ';
					}
					else
					{
						if (*Np->inout_p == '-')
							*Np->number = '-';
						else
						{
							NUM_eat_non_data_chars(Np, 1, input_len);
							continue;
						}
					}
					break;

				case NUM_PL:
					if (Np->is_to_char)
					{
						if (Np->sign == '+')
							*Np->inout_p = '+';
						else if (IS_FILLMODE(Np->Num))
							continue;
						else
							*Np->inout_p = ' ';
					}
					else
					{
						if (*Np->inout_p == '+')
							*Np->number = '+';
						else
						{
							NUM_eat_non_data_chars(Np, 1, input_len);
							continue;
						}
					}
					break;

				case NUM_SG:
					if (Np->is_to_char)
						*Np->inout_p = Np->sign;
					else
					{
						if (*Np->inout_p == '-')
							*Np->number = '-';
						else if (*Np->inout_p == '+')
							*Np->number = '+';
						else
						{
							NUM_eat_non_data_chars(Np, 1, input_len);
							continue;
						}
					}
					break;

				default:
					continue;
					break;
			}
		}
		else
		{
			/*
			 * TO_CHAR 中，格式里的非模式字符会复制到输出。
			 * TO_NUMBER 中，每个非模式格式字符对应跳过
			 * 一个输入字符，无论是否匹配该格式字符。
			 *
			 */
			if (Np->is_to_char)
			{
				strcpy(Np->inout_p, n->character);
				Np->inout_p += strlen(Np->inout_p);
			}
			else
			{
				Np->inout_p += pg_mblen(Np->inout_p);
			}
			continue;
		}
		Np->inout_p++;
	}

	if (Np->is_to_char)
	{
		*Np->inout_p = '\0';
		return Np->inout;
	}
	else
	{
		if (*(Np->number_p - 1) == '.')
			*(Np->number_p - 1) = '\0';
		else
			*Np->number_p = '\0';

		/*
		 * 修正——小数数字的精度
		 */
		Np->Num->post = Np->read_post;

#ifdef DEBUG_TO_FROM_CHAR
		elog(DEBUG_elog_output, "TO_NUMBER (number): '%s'", Np->number);
#endif
		return Np->number;
	}
}

/* ----------
 * MACRO：NUM 起始部分——所有 NUM to_char 变体
 *	（抱歉，讨厌复制同码，宏更好）
 * ----------
 */
#define NUM_TOCHAR_prepare \
do { \
	int len = VARSIZE_ANY_EXHDR(fmt); \
	if (len <= 0 || len >= (INT_MAX-VARHDRSZ)/NUM_MAX_ITEM_SIZ)		\
		PG_RETURN_TEXT_P(cstring_to_text("")); \
	result	= (text *) palloc0((len * NUM_MAX_ITEM_SIZ) + 1 + VARHDRSZ);	\
	format	= NUM_cache(len, &Num, fmt, &shouldFree);		\
} while (0)

/* ----------
 * MACRO：NUM 收尾部分
 * ----------
 */
#define NUM_TOCHAR_finish \
do { \
	int		len; \
									\
	NUM_processor(format, &Num, VARDATA(result), numstr, 0, out_pre_spaces, sign, true, PG_GET_COLLATION()); \
									\
	if (shouldFree)					\
		pfree(format);				\
									\
	/*								\
	 * 把以 null 结尾的结果表示转为标准 text。 \
	 * 结果通常比所需大得多，但把它重分配得更小 \
	 * 似乎没什么意义。 \
	 */								\
	len = strlen(VARDATA(result));	\
	SET_VARSIZE(result, len + VARHDRSZ); \
} while (0)

/* -------------------
 * NUMERIC to_number()（把串转为 numeric）
 * -------------------
 */
Datum
numeric_to_number(PG_FUNCTION_ARGS)
{
	text	   *value = PG_GETARG_TEXT_PP(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	Datum		result;
	FormatNode *format;
	char	   *numstr;
	bool		shouldFree;
	int			len = 0;
	int			scale,
				precision;

	len = VARSIZE_ANY_EXHDR(fmt);

	if (len <= 0 || len >= INT_MAX / NUM_MAX_ITEM_SIZ)
		PG_RETURN_NULL();

	format = NUM_cache(len, &Num, fmt, &shouldFree);

	numstr = (char *) palloc((len * NUM_MAX_ITEM_SIZ) + 1);

	NUM_processor(format, &Num, VARDATA_ANY(value), numstr,
				  VARSIZE_ANY_EXHDR(value), 0, 0, false, PG_GET_COLLATION());

	scale = Num.post;
	precision = Num.pre + Num.multi + scale;

	if (shouldFree)
		pfree(format);

	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(numstr),
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(((precision << 16) | scale) + VARHDRSZ));

	if (IS_MULTI(&Num))
	{
		Numeric		x;
		Numeric		a = int64_to_numeric(10);
		Numeric		b = int64_to_numeric(-Num.multi);

		x = DatumGetNumeric(DirectFunctionCall2(numeric_power,
												NumericGetDatum(a),
												NumericGetDatum(b)));
		result = DirectFunctionCall2(numeric_mul,
									 result,
									 NumericGetDatum(x));
	}

	pfree(numstr);
	return result;
}

/* ------------------
 * NUMERIC to_char()
 * ------------------
 */
Datum
numeric_to_char(PG_FUNCTION_ARGS)
{
	Numeric		value = PG_GETARG_NUMERIC(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum,
			   *p;
	Numeric		x;

	NUM_TOCHAR_prepare;

	/*
	 * DateType 相关段（numeric）
	 */
	if (IS_ROMAN(&Num))
	{
		x = DatumGetNumeric(DirectFunctionCall2(numeric_round,
												NumericGetDatum(value),
												Int32GetDatum(0)));
		numstr =
			int_to_roman(DatumGetInt32(DirectFunctionCall1(numeric_int4,
														   NumericGetDatum(x))));
	}
	else if (IS_EEEE(&Num))
	{
		orgnum = numeric_out_sci(value, Num.post);

		/*
		 * numeric_out_sci() 不为正数输出符号。此时我们需要
		 * 补一个空格，使正数与负数对齐。还必须检查
		 * NaN/无穷的情形，处理方式与 float8_to_char 相同。
		 *
		 */
		if (strcmp(orgnum, "NaN") == 0 ||
			strcmp(orgnum, "Infinity") == 0 ||
			strcmp(orgnum, "-Infinity") == 0)
		{
			/*
			 * 为前导符号、小数点、"e"、指数符号与
			 * 两位指数数字预留 6 个字符。
			 */
			numstr = (char *) palloc(Num.pre + Num.post + 7);
			fill_str(numstr, '#', Num.pre + Num.post + 6);
			*numstr = ' ';
			*(numstr + Num.pre + 1) = '.';
		}
		else if (*orgnum != '-')
		{
			numstr = (char *) palloc(strlen(orgnum) + 2);
			*numstr = ' ';
			strcpy(numstr + 1, orgnum);
		}
		else
		{
			numstr = orgnum;
		}
	}
	else
	{
		int			numstr_pre_len;
		Numeric		val = value;

		if (IS_MULTI(&Num))
		{
			Numeric		a = int64_to_numeric(10);
			Numeric		b = int64_to_numeric(Num.multi);

			x = DatumGetNumeric(DirectFunctionCall2(numeric_power,
													NumericGetDatum(a),
													NumericGetDatum(b)));
			val = DatumGetNumeric(DirectFunctionCall2(numeric_mul,
													  NumericGetDatum(value),
													  NumericGetDatum(x)));
			Num.pre += Num.multi;
		}

		x = DatumGetNumeric(DirectFunctionCall2(numeric_round,
												NumericGetDatum(val),
												Int32GetDatum(Num.post)));
		orgnum = DatumGetCString(DirectFunctionCall1(numeric_out,
													 NumericGetDatum(x)));

		if (*orgnum == '-')
		{
			sign = '-';
			numstr = orgnum + 1;
		}
		else
		{
			sign = '+';
			numstr = orgnum;
		}

		if ((p = strchr(numstr, '.')))
			numstr_pre_len = p - numstr;
		else
			numstr_pre_len = strlen(numstr);

		/* 需要填充? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* 前缀数字格式溢出? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* ---------------
 * INT4 to_char()
 * ---------------
 */
Datum
int4_to_char(PG_FUNCTION_ARGS)
{
	int32		value = PG_GETARG_INT32(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum;

	NUM_TOCHAR_prepare;

	/*
	 * DateType 相关段（int32）
	 */
	if (IS_ROMAN(&Num))
		numstr = int_to_roman(value);
	else if (IS_EEEE(&Num))
	{
		/* 轻松做到，因 float8 不丢任何精度 */
		float8		val = (float8) value;

		orgnum = (char *) psprintf("%+.*e", Num.post, val);

		/*
		 * 把前导正号换成空格。
		 */
		if (*orgnum == '+')
			*orgnum = ' ';

		numstr = orgnum;
	}
	else
	{
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			orgnum = DatumGetCString(DirectFunctionCall1(int4out,
														 Int32GetDatum(value * ((int32) pow((double) 10, (double) Num.multi)))));
			Num.pre += Num.multi;
		}
		else
		{
			orgnum = DatumGetCString(DirectFunctionCall1(int4out,
														 Int32GetDatum(value)));
		}

		if (*orgnum == '-')
		{
			sign = '-';
			orgnum++;
		}
		else
			sign = '+';

		numstr_pre_len = strlen(orgnum);

		/* 小数后有数字？用零补足。 */
		if (Num.post)
		{
			numstr = (char *) palloc(numstr_pre_len + Num.post + 2);
			strcpy(numstr, orgnum);
			*(numstr + numstr_pre_len) = '.';
			memset(numstr + numstr_pre_len + 1, '0', Num.post);
			*(numstr + numstr_pre_len + Num.post + 1) = '\0';
		}
		else
			numstr = orgnum;

		/* 需要填充? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* 前缀数字格式溢出? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* ---------------
 * INT8 to_char()
 * ---------------
 */
Datum
int8_to_char(PG_FUNCTION_ARGS)
{
	int64		value = PG_GETARG_INT64(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *orgnum;

	NUM_TOCHAR_prepare;

	/*
	 * DateType 相关段（int32）
	 */
	if (IS_ROMAN(&Num))
	{
		/* 目前不支持 int8 转罗马数字... */
		numstr = int_to_roman(DatumGetInt32(DirectFunctionCall1(int84, Int64GetDatum(value))));
	}
	else if (IS_EEEE(&Num))
	{
		/* 为避免精度损失，须经由 numeric 而非 float8 */
		orgnum = numeric_out_sci(int64_to_numeric(value),
								 Num.post);

		/*
		 * numeric_out_sci() 不为正数输出符号。此时我们需要
		 * 补一个空格，使正数与负数对齐。此处
		 * 无需担心 NaN/inf。
		 */
		if (*orgnum != '-')
		{
			numstr = (char *) palloc(strlen(orgnum) + 2);
			*numstr = ' ';
			strcpy(numstr + 1, orgnum);
		}
		else
		{
			numstr = orgnum;
		}
	}
	else
	{
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			double		multi = pow((double) 10, (double) Num.multi);

			value = DatumGetInt64(DirectFunctionCall2(int8mul,
													  Int64GetDatum(value),
													  DirectFunctionCall1(dtoi8,
																		  Float8GetDatum(multi))));
			Num.pre += Num.multi;
		}

		orgnum = DatumGetCString(DirectFunctionCall1(int8out,
													 Int64GetDatum(value)));

		if (*orgnum == '-')
		{
			sign = '-';
			orgnum++;
		}
		else
			sign = '+';

		numstr_pre_len = strlen(orgnum);

		/* 小数后有数字？用零补足。 */
		if (Num.post)
		{
			numstr = (char *) palloc(numstr_pre_len + Num.post + 2);
			strcpy(numstr, orgnum);
			*(numstr + numstr_pre_len) = '.';
			memset(numstr + numstr_pre_len + 1, '0', Num.post);
			*(numstr + numstr_pre_len + Num.post + 1) = '\0';
		}
		else
			numstr = orgnum;

		/* 需要填充? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* 前缀数字格式溢出? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* -----------------
 * FLOAT4 to_char()
 * -----------------
 */
Datum
float4_to_char(PG_FUNCTION_ARGS)
{
	float4		value = PG_GETARG_FLOAT4(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *p;

	NUM_TOCHAR_prepare;

	if (IS_ROMAN(&Num))
		numstr = int_to_roman((int) rint(value));
	else if (IS_EEEE(&Num))
	{
		if (isnan(value) || isinf(value))
		{
			/*
			 * 为前导符号、小数点、"e"、指数符号与
			 * 两位指数数字预留 6 个字符。
			 */
			numstr = (char *) palloc(Num.pre + Num.post + 7);
			fill_str(numstr, '#', Num.pre + Num.post + 6);
			*numstr = ' ';
			*(numstr + Num.pre + 1) = '.';
		}
		else
		{
			numstr = psprintf("%+.*e", Num.post, value);

			/*
			 * 把前导正号换成空格。
			 */
			if (*numstr == '+')
				*numstr = ' ';
		}
	}
	else
	{
		float4		val = value;
		char	   *orgnum;
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			float		multi = pow((double) 10, (double) Num.multi);

			val = value * multi;
			Num.pre += Num.multi;
		}

		orgnum = psprintf("%.0f", fabs(val));
		numstr_pre_len = strlen(orgnum);

		/* 调整小数位适配 float 最大位数 */
		if (numstr_pre_len >= FLT_DIG)
			Num.post = 0;
		else if (numstr_pre_len + Num.post > FLT_DIG)
			Num.post = FLT_DIG - numstr_pre_len;
		orgnum = psprintf("%.*f", Num.post, val);

		if (*orgnum == '-')
		{						/* < 0 */
			sign = '-';
			numstr = orgnum + 1;
		}
		else
		{
			sign = '+';
			numstr = orgnum;
		}

		if ((p = strchr(numstr, '.')))
			numstr_pre_len = p - numstr;
		else
			numstr_pre_len = strlen(numstr);

		/* 需要填充? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* 前缀数字格式溢出? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}

/* -----------------
 * FLOAT8 to_char()
 * -----------------
 */
Datum
float8_to_char(PG_FUNCTION_ARGS)
{
	float8		value = PG_GETARG_FLOAT8(0);
	text	   *fmt = PG_GETARG_TEXT_PP(1);
	NUMDesc		Num;
	FormatNode *format;
	text	   *result;
	bool		shouldFree;
	int			out_pre_spaces = 0,
				sign = 0;
	char	   *numstr,
			   *p;

	NUM_TOCHAR_prepare;

	if (IS_ROMAN(&Num))
		numstr = int_to_roman((int) rint(value));
	else if (IS_EEEE(&Num))
	{
		if (isnan(value) || isinf(value))
		{
			/*
			 * 为前导符号、小数点、"e"、指数符号与
			 * 两位指数数字预留 6 个字符。
			 */
			numstr = (char *) palloc(Num.pre + Num.post + 7);
			fill_str(numstr, '#', Num.pre + Num.post + 6);
			*numstr = ' ';
			*(numstr + Num.pre + 1) = '.';
		}
		else
		{
			numstr = psprintf("%+.*e", Num.post, value);

			/*
			 * 把前导正号换成空格。
			 */
			if (*numstr == '+')
				*numstr = ' ';
		}
	}
	else
	{
		float8		val = value;
		char	   *orgnum;
		int			numstr_pre_len;

		if (IS_MULTI(&Num))
		{
			double		multi = pow((double) 10, (double) Num.multi);

			val = value * multi;
			Num.pre += Num.multi;
		}

		orgnum = psprintf("%.0f", fabs(val));
		numstr_pre_len = strlen(orgnum);

		/* 调整小数位适配 double 最大位数 */
		if (numstr_pre_len >= DBL_DIG)
			Num.post = 0;
		else if (numstr_pre_len + Num.post > DBL_DIG)
			Num.post = DBL_DIG - numstr_pre_len;
		orgnum = psprintf("%.*f", Num.post, val);

		if (*orgnum == '-')
		{						/* < 0 */
			sign = '-';
			numstr = orgnum + 1;
		}
		else
		{
			sign = '+';
			numstr = orgnum;
		}

		if ((p = strchr(numstr, '.')))
			numstr_pre_len = p - numstr;
		else
			numstr_pre_len = strlen(numstr);

		/* 需要填充? */
		if (numstr_pre_len < Num.pre)
			out_pre_spaces = Num.pre - numstr_pre_len;
		/* 前缀数字格式溢出? */
		else if (numstr_pre_len > Num.pre)
		{
			numstr = (char *) palloc(Num.pre + Num.post + 2);
			fill_str(numstr, '#', Num.pre + Num.post + 1);
			*(numstr + Num.pre) = '.';
		}
	}

	NUM_TOCHAR_finish;
	PG_RETURN_TEXT_P(result);
}
