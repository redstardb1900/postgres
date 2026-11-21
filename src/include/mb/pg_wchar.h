/*-------------------------------------------------------------------------
 *
 * pg_wchar.h
 *	  多字节字符支持
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/mb/pg_wchar.h
 *
 *	NOTES
 *		后端与前端皆用，但不应被
 *		libpq 客户端程序包含。尤其 libpq 客户端
 *		不应假定其所链 libpq 版本使用的编码 ID
 *		与此处声明的 ID 一致。
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_WCHAR_H
#define PG_WCHAR_H

/*
 * pg_wchar 类型
 */
typedef unsigned int pg_wchar;

/*
 * 任意后端编码中多字节字符的最大字节长度
 */
#define MAX_MULTIBYTE_CHAR_LEN	4

/*
 * EUC 的若干定义
 */
#define SS2 0x8e				/*  单移 2 (JIS0201)  */
#define SS3 0x8f				/*  单移 3 (JIS0212)  */

/*
 * SJIS 校验宏
 */
#define ISSJISHEAD(c) (((c) >= 0x81 && (c) <= 0x9f) || ((c) >= 0xe0 && (c) <= 0xfc))
#define ISSJISTAIL(c) (((c) >= 0x40 && (c) <= 0x7e) || ((c) >= 0x80 && (c) <= 0xfc))

/*----------------------------------------------------
 * MULE 内部编码 (MIC)
 *
 * 此编码沿用 XEmacs 内部设计；意在
 * 涵盖多种外部定义的字符集。每个字符含
 * 所属字符集标识，故编码通
 * 用但略显臃肿。
 *
 * 当前 PostgreSQL 支持 5 类 MULE 字符集：
 *
 * 1) 1 字节 ASCII。每字节低于 0x80。
 *
 * 2) "正式"单字节字符集，如 ISO-8859-1 (Latin1)。
 *	  每个 MULE 字符 2 字节：LC1 + C1，其中 LC1 是
 *	  字符集标识（范围 0x81–0x8d），C1
 *	  为字符码（范围 0xa0–0xff）。
 *
 * 3) "私有"单字节字符集，如 SISHENG。每个 MULE
 *	  字符 3 字节：LCPRV1 + LC12 + C1，其中 LCPRV1
 *	  为私有字符集标志，LC12 为字符集标识，
 *	  C1 为字符码（范围 0xa0–0xff）。
 *	  LCPRV1 为 0x9a（若 LC12 在 0xa0–0xdf）
 *	  或 0x9b（若 LC12 在 0xe0–0xef）。
 *
 * 4) "正式"多字节字符集，如 JIS X0208。每个 MULE
 *	  字符 3 字节：LC2 + C1 + C2，其中 LC2 是
 *	  字符集标识（范围 0x90–0x99），C1
 *	  与 C2 组成字符码（各在 0xa0–0xff）。
 *
 * 5) "私有"多字节字符集，如 CNS 11643-1992 平面 3。
 *	  每个 MULE 字符 4 字节：LCPRV2 + LC22 + C1 + C2，
 *	  其中 LCPRV2 为私有字符集标志，LC22 为
 *	  字符集标识，C1 与 C2 组成字符码（各在
 *	  0xa0–0xff）。LCPRV2 为 0x9c（若 LC22 在 0xf0
 *	  –0xf4）或 0x9d（若 LC22 在 0xf5–0xfe）。
 *
 * "正式"编码指已由 XEmacs 项目分配代号者；
 * "私有"编码则使用 Postgres 特有的字符集
 * 标识符。
 *
 * 详见 "XEmacs Internals Manual"（http://www.xemacs.org），
 * 更多细节。历史原因下 Postgres 的
 * 私有字符集标志值与 XEmacs 规定并不一致，
 * 故并非严格 MULE（私有字符集本也
 * 难互通）。
 *
 * 注意 XEmacs 实现与 emacs 不同。
 * 我们跟从 emacs 实现，而非 XEmacs。
 *----------------------------------------------------
 */

/*
 * 字符集标识（MULE 文档亦称 "leading bytes"）
 */

/*
 * 正式单字节编码的字符集 ID（0x81–0x8e）
 */
#define LC_ISO8859_1		0x81	/* ISO8859 Latin 1 */
#define LC_ISO8859_2		0x82	/* ISO8859 Latin 2 */
#define LC_ISO8859_3		0x83	/* ISO8859 Latin 3 */
#define LC_ISO8859_4		0x84	/* ISO8859 Latin 4 */
#define LC_TIS620			0x85	/*  泰文(未支持)  */
#define LC_ISO8859_7		0x86	/*  希腊文(未支持)  */
#define LC_ISO8859_6		0x87	/*  阿拉伯文(未支持)  */
#define LC_ISO8859_8		0x88	/*  希伯来文(未支持)  */
#define LC_JISX0201K		0x89	/*  日文假名  */
#define LC_JISX0201R		0x8a	/*  日文罗马字  */
/*  注意 0x8b 于 Emacs 20.7 似未用。
 * 但日后 Emacs 版本仍可能
 * 使用 0x8b。
 */
#define LC_KOI8_R			0x8b	/* Cyrillic KOI8-R */
#define LC_ISO8859_5		0x8c	/* ISO8859 Cyrillic */
#define LC_ISO8859_9		0x8d	/* ISO8859 Latin 5 (not supported yet) */
#define LC_ISO8859_15		0x8e	/* ISO8859 Latin 15 (not supported yet) */
/*  #define CONTROL_1		0x8f	控制符(未用)  */

/*  是否"正式"单字节编码引导字节？  */
#define IS_LC1(c)	((unsigned char)(c) >= 0x81 && (unsigned char)(c) <= 0x8d)

/*
 * 正式多字节编码的字符集 ID（0x90–0x99）
 * 0x9a–0x9d 空闲。0x9e 与 0x9f 保留。
 */
#define LC_JISX0208_1978	0x90	/* Japanese Kanji, old JIS (not supported) */
#define LC_GB2312_80		0x91	/* Chinese */
#define LC_JISX0208			0x92	/* Japanese Kanji (JIS X 0208) */
#define LC_KS5601			0x93	/* Korean */
#define LC_JISX0212			0x94	/* Japanese Kanji (JIS X 0212) */
#define LC_CNS11643_1		0x95	/* CNS 11643-1992 Plane 1 */
#define LC_CNS11643_2		0x96	/* CNS 11643-1992 Plane 2 */
#define LC_JISX0213_1		0x97	/*  日文汉字 (JIS X 0213 平面1)
									 * (未支持)  */
#define LC_BIG5_1			0x98	/*  平面1繁体中文（不
									 * 支持）  */
#define LC_BIG5_2			0x99	/*  平面1繁体中文（不
									 * 支持）  */

/*  是否"正式"多字节编码引导字节？  */
#define IS_LC2(c)	((unsigned char)(c) >= 0x90 && (unsigned char)(c) <= 0x99)

/*
 * Postgres 私有单字节编码的前缀字节
 * （按 MULE 文档本应使用 0x9e）
 */
#define LCPRV1_A		0x9a
#define LCPRV1_B		0x9b
#define IS_LCPRV1(c)	((unsigned char)(c) == LCPRV1_A || (unsigned char)(c) == LCPRV1_B)
#define IS_LCPRV1_A_RANGE(c)	\
	((unsigned char)(c) >= 0xa0 && (unsigned char)(c) <= 0xdf)
#define IS_LCPRV1_B_RANGE(c)	\
	((unsigned char)(c) >= 0xe0 && (unsigned char)(c) <= 0xef)

/*
 * Postgres 私有多字节编码的前缀字节
 * （按 MULE 文档本应使用 0x9f）
 */
#define LCPRV2_A		0x9c
#define LCPRV2_B		0x9d
#define IS_LCPRV2(c)	((unsigned char)(c) == LCPRV2_A || (unsigned char)(c) == LCPRV2_B)
#define IS_LCPRV2_A_RANGE(c)	\
	((unsigned char)(c) >= 0xf0 && (unsigned char)(c) <= 0xf4)
#define IS_LCPRV2_B_RANGE(c)	\
	((unsigned char)(c) >= 0xf5 && (unsigned char)(c) <= 0xfe)

/*
 * 私有单字节编码的字符集 ID（0xa0–0xef）
 */
#define LC_SISHENG			0xa0	/*  中文 SiSheng 字符供
									 * PinYin/ZhuYin（不支持）  */
#define LC_IPA				0xa1	/*  IPA（国际音标
									 * 协会）（不支持）  */
#define LC_VISCII_LOWER		0xa2	/*  越南文 VISCII1.1 小写（不
									 * 支持）  */
#define LC_VISCII_UPPER		0xa3	/*  越南文 VISCII1.1 大写（不
									 * 支持）  */
#define LC_ARABIC_DIGIT		0xa4	/*  阿拉伯数字(未支持)  */
#define LC_ARABIC_1_COLUMN	0xa5	/*  阿拉伯文1列(未支持)  */
#define LC_ASCII_RIGHT_TO_LEFT	0xa6	/*  ASCII（ISO8859-1 左半）带
										 * 从右到左方向（不
										 * 支持）  */
#define LC_LAO				0xa7	/*  老挝字符 (ISO10646 0E80..0EDF)
									 * (未支持)  */
#define LC_ARABIC_2_COLUMN	0xa8	/*  阿拉伯文1列(未支持)  */

/*
 * 私有多字节编码的字符集 ID（0xf0–0xff）
 */
#define LC_INDIAN_1_COLUMN	0xf0	/*  印度文1列宽字形
									 * 字符集(未支持)  */
#define LC_TIBETAN_1_COLUMN 0xf1	/*  藏文1列宽字形（不
									 * 支持）  */
#define LC_UNICODE_SUBSET_2 0xf2	/*  Unicode 字符范围
									 * U+2500..U+33FF.(未支持)  */
#define LC_UNICODE_SUBSET_3 0xf3	/*  Unicode 字符范围
									 * U+E000..U+FFFF.(未支持)  */
#define LC_UNICODE_SUBSET	0xf4	/*  Unicode 字符范围
									 * U+0100..U+24FF.(未支持)  */
#define LC_ETHIOPIC			0xf5	/*  埃塞俄比亚字符(未支持)  */
#define LC_CNS11643_3		0xf6	/* CNS 11643-1992 Plane 3 */
#define LC_CNS11643_4		0xf7	/* CNS 11643-1992 Plane 4 */
#define LC_CNS11643_5		0xf8	/* CNS 11643-1992 Plane 5 */
#define LC_CNS11643_6		0xf9	/* CNS 11643-1992 Plane 6 */
#define LC_CNS11643_7		0xfa	/* CNS 11643-1992 Plane 7 */
#define LC_INDIAN_2_COLUMN	0xfb	/*  印度文2列宽字形
									 * 字符集(未支持)  */
#define LC_TIBETAN			0xfc	/*  藏文（不支持）  */
/* #define FREE				0xfd	free (unused) */
/* #define FREE				0xfe	free (unused) */
/* #define FREE				0xff	free (unused) */

/*----------------------------------------------------
 * MULE 相关结束
 *----------------------------------------------------
 */

/*
 * PostgreSQL 编码标识符
 *
 * WARNING：此枚举顺序必须与
 *			pg_enc2name_tbl[]（src/common/encnames.c）及
 *			pg_wchar_table[](src/common/wchar.c)顺序一致！
 *
 *			若新增编码，勿忘检查
 *			PG_ENCODING_BE_LAST 宏。
 *
 * PG_SQL_ASCII 为默认编码且必须 = 0。
 *
 * XXX	libpq 主版本>5 前勿重编号任何后端
 * 编码；事实表明后端
 * 编码 ID 对 8.2 initdb 与
 * psql 即 ABI。
 */
typedef enum pg_enc
{
	PG_SQL_ASCII = 0,			/* SQL/ASCII */
	PG_EUC_JP,					/* EUC for Japanese */
	PG_EUC_CN,					/* EUC for Chinese */
	PG_EUC_KR,					/* EUC for Korean */
	PG_EUC_TW,					/* EUC for Taiwan */
	PG_EUC_JIS_2004,			/* EUC-JIS-2004 */
	PG_UTF8,					/* Unicode UTF8 */
	PG_MULE_INTERNAL,			/* Mule internal code */
	PG_LATIN1,					/* ISO-8859-1 Latin 1 */
	PG_LATIN2,					/* ISO-8859-2 Latin 2 */
	PG_LATIN3,					/* ISO-8859-3 Latin 3 */
	PG_LATIN4,					/* ISO-8859-4 Latin 4 */
	PG_LATIN5,					/* ISO-8859-9 Latin 5 */
	PG_LATIN6,					/* ISO-8859-10 Latin6 */
	PG_LATIN7,					/* ISO-8859-13 Latin7 */
	PG_LATIN8,					/* ISO-8859-14 Latin8 */
	PG_LATIN9,					/* ISO-8859-15 Latin9 */
	PG_LATIN10,					/* ISO-8859-16 Latin10 */
	PG_WIN1256,					/* windows-1256 */
	PG_WIN1258,					/* Windows-1258 */
	PG_WIN866,					/* (MS-DOS CP866) */
	PG_WIN874,					/* windows-874 */
	PG_KOI8R,					/* KOI8-R */
	PG_WIN1251,					/* windows-1251 */
	PG_WIN1252,					/* windows-1252 */
	PG_ISO_8859_5,				/* ISO-8859-5 */
	PG_ISO_8859_6,				/* ISO-8859-6 */
	PG_ISO_8859_7,				/* ISO-8859-7 */
	PG_ISO_8859_8,				/* ISO-8859-8 */
	PG_WIN1250,					/* windows-1250 */
	PG_WIN1253,					/* windows-1253 */
	PG_WIN1254,					/* windows-1254 */
	PG_WIN1255,					/* windows-1255 */
	PG_WIN1257,					/* windows-1257 */
	PG_KOI8U,					/* KOI8-U */
	/*  PG_ENCODING_BE_LAST 指向上述项  */

	/*  以下仅用于客户端编码  */
	PG_SJIS,					/* Shift JIS (Windows-932) */
	PG_BIG5,					/* Big5 (Windows-950) */
	PG_GBK,						/* GBK (Windows-936) */
	PG_UHC,						/* UHC (Windows-949) */
	PG_GB18030,					/* GB18030 */
	PG_JOHAB,					/* EUC for Korean JOHAB */
	PG_SHIFT_JIS_2004,			/* Shift-JIS-2004 */
	_PG_LAST_ENCODING_			/* mark only */

} pg_enc;

#define PG_ENCODING_BE_LAST PG_KOI8U

/*
 * 访问 pg_enc2name_tbl[] 或其他处之前
 * 请先用这些测试
 */
#define PG_VALID_BE_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) <= PG_ENCODING_BE_LAST)

#define PG_ENCODING_IS_CLIENT_ONLY(_enc) \
		((_enc) > PG_ENCODING_BE_LAST && (_enc) < _PG_LAST_ENCODING_)

#define PG_VALID_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) < _PG_LAST_ENCODING_)

/*  前端可使用全部编码  */
#define PG_VALID_FE_ENCODING(_enc)	PG_VALID_ENCODING(_enc)

/*
 * 在不同编码间转换字符串时，假定转换结果
 * 最坏按4倍预留。当前已支持
 * 编码对膨胀率≤3（SJIS JIS X0201 半角
 * 假名→UTF8 最坏）。故暂定"4"足够。
 *
 * 注意这与任一具体编码中的最大字符
 * 宽度不同。
 */
#define MAX_CONVERSION_GROWTH  4

/*
 * 为至少转换一个字符到任意其他编码，所需输入串的
 * 最大字节长度。换言之，若向任意编码转换函数喂入
 * MAX_CONVERSION_INPUT_LENGTH 字节，则保证
 * 无需更多输入即可转换出某些内容
 * （假定输入合法）。
 *
 * 当前最坏为 UTF8→SJIS JIS X0201 半角
 * 假名：一对 UTF-8 字符转为单个
 * SHIFT_JIS_2004 字符（恰为
 * MAX_CONVERSION_GROWTH 之逆）。需6字节输入。理论上
 * 用户定义转换可能更复杂，尽管
 * 逆向映射恐怕还得抬高
 * MAX_CONVERSION_GROWTH。此处不必抠门，故取
 * 从宽。
 */
#define MAX_CONVERSION_INPUT_LENGTH	16

/*
 * 任一 Unicode 码点在任一后端编码下的等价串最大
 * 字节长。当前值假定4字节 UTF-8
 * 字符按 MAX_CONVERSION_GROWTH 膨胀，属严重
 * 高估。但现用法不会大量倍乘此值，
 * 抠门意义不大。
 */
#define MAX_UNICODE_EQUIVALENT_STRING	16

/*
 * 将编码编号映射到正式编码名及
 * 可能的附属数据的表。访问表项前务必
 * 先检查编码编号！
 *
 * if (PG_VALID_ENCODING(encoding))
 *		pg_enc2name_tbl[ encoding ];
 */
typedef struct pg_enc2name
{
	const char *name;
	pg_enc		encoding;
#ifdef WIN32
	unsigned	codepage;		/*  WIN32 代码页  */
#endif
} pg_enc2name;

extern PGDLLIMPORT const pg_enc2name pg_enc2name_tbl[];

/*
 * gettext 用的编码名
 */
typedef struct pg_enc2gettext
{
	pg_enc		encoding;
	const char *name;
} pg_enc2gettext;

extern PGDLLIMPORT const pg_enc2gettext pg_enc2gettext_tbl[];

/*
 * pg_wchar
 */
typedef int (*mb2wchar_with_len_converter) (const unsigned char *from,
											pg_wchar *to,
											int len);

typedef int (*wchar2mb_with_len_converter) (const pg_wchar *from,
											unsigned char *to,
											int len);

typedef int (*mblen_converter) (const unsigned char *mbstr);

typedef int (*mbdisplaylen_converter) (const unsigned char *mbstr);

typedef bool (*mbcharacter_incrementer) (unsigned char *mbstr, int len);

typedef int (*mbchar_verifier) (const unsigned char *mbstr, int len);

typedef int (*mbstr_verifier) (const unsigned char *mbstr, int len);

typedef struct
{
	mb2wchar_with_len_converter mb2wchar_with_len;	/*  将多字节
													 * 串转为 wchar  */
	wchar2mb_with_len_converter wchar2mb_with_len;	/*  将 wchar 串
													 * 转多字节  */
	mblen_converter mblen;		/*  取字符字节长度  */
	mbdisplaylen_converter dsplen;	/*  取字符显示宽度  */
	mbchar_verifier mbverifychar;	/*  校验多字节字符  */
	mbstr_verifier mbverifystr; /*  校验多字节串  */
	int			maxmblen;		/*  此编码下字符最大字节数  */
} pg_wchar_tbl;

extern PGDLLIMPORT const pg_wchar_tbl pg_wchar_table[];

/*
 * UTF-8 与其他编码互转的数据结构
 * （UtfToLocal 等）。其中任一编码的字符
 * 皆用四字节整型字表示；故仅支持最长
 * 4字节字符。例如字节序列 C2 89
 * 表示为 0x0000C289，三字节例 0x00E8A2B4。
 *
 * 字符映射有三种可能方式：
 *
 * 1. 用基数树，从源码点到目标码点。
 * 2. 用已排序的源→目标码点对数组。此
 *	  法用于"组合"字符。数量极少，
 *	  建基数树浪费。
 * 3. 用转换函数。
 */

/*
 * 字符转换用的基数树。
 *
 * 逻辑上实为四棵基数树，分别对应1、
 * 2、3、4字节输入。1字节树是简单的源→目标
 * 查找表。2字节树有两层：
 * 第一字节的查找表，表中值指向
 * 第二字节的查找表。以此类推。
 *
 * 物理上所有树存于一个大数组，在 chars16 或
 * chars32 中，视所需表示的最大值而定。对
 * 每棵树每一层，还存允许值的上下界
 * ——界外视为非法，且不
 * 列入表中。
 *
 * 在树的中间层，所存值为
 * chars[16|32] 内偏移。
 *
 * chars[16|32] 数组开头总有若干
 * 零，故可安全跟随中间表的索引
 * 而无需显式检查零。跟随任意次零
 * 总会落到开头的哑元全零表。
 * 这有助于查找时节省若干周期。
 */
typedef struct
{
	/*
	 * 容纳全部值的数组。chars16 与 chars32 仅
	 * 用其一，取决于需表示的值有多宽。
	 */
	const uint16 *chars16;
	const uint32 *chars32;

	/*  1字节输入的基数树  */
	uint32		b1root;			/*  chars[16|32] 中表的偏移  */
	uint8		b1_lower;		/*  单字节输入允许的最小值  */
	uint8		b1_upper;		/*  单字节输入允许的最大值  */

	/*  2字节输入的基数树  */
	uint32		b2root;			/*  第1字节表的偏移  */
	uint8		b2_1_lower;		/*  第1输入字节允许最小/最大  */
	uint8		b2_1_upper;
	uint8		b2_2_lower;		/*  第2输入字节允许最小/最大  */
	uint8		b2_2_upper;

	/*  3字节输入的基数树  */
	uint32		b3root;			/*  第1字节表的偏移  */
	uint8		b3_1_lower;		/*  第1输入字节允许最小/最大  */
	uint8		b3_1_upper;
	uint8		b3_2_lower;		/*  第2输入字节允许最小/最大  */
	uint8		b3_2_upper;
	uint8		b3_3_lower;		/*  第3输入字节允许最小/最大  */
	uint8		b3_3_upper;

	/*  4字节输入的基数树  */
	uint32		b4root;			/*  第1字节表的偏移  */
	uint8		b4_1_lower;		/*  第1输入字节允许最小/最大  */
	uint8		b4_1_upper;
	uint8		b4_2_lower;		/*  第2输入字节允许最小/最大  */
	uint8		b4_2_upper;
	uint8		b4_3_lower;		/*  第3输入字节允许最小/最大  */
	uint8		b4_3_upper;
	uint8		b4_4_lower;		/*  第4输入字节允许最小/最大  */
	uint8		b4_4_upper;

} pg_mb_radix_tree;

/*
 * UTF-8 到本地码转换映射（组合字符）
 */
typedef struct
{
	uint32		utf1;			/* UTF-8 code 1 */
	uint32		utf2;			/* UTF-8 code 2 */
	uint32		code;			/* local code */
} pg_utf_to_local_combined;

/*
 * 本地码到 UTF-8 转换映射（组合字符）
 */
typedef struct
{
	uint32		code;			/* local code */
	uint32		utf1;			/* UTF-8 code 1 */
	uint32		utf2;			/* UTF-8 code 2 */
} pg_local_to_utf_combined;

/*
 * 算法型编码转换的回调（双向）
 *
 * 若函数返回零，表示不知如何转换该码
 */
typedef uint32 (*utf_local_conversion_func) (uint32 code);

/*
 * 供编码转换函数校验其
 * 参数的支持宏。（若含 fmgr.h 可更紧凑，
 * 但本头亦被前端使用，故不愿
 * 在此包含。）
 */
#define CHECK_ENCODING_CONVERSION_ARGS(srcencoding,destencoding) \
	check_encoding_conversion_args(PG_GETARG_INT32(0), \
								   PG_GETARG_INT32(1), \
								   PG_GETARG_INT32(4), \
								   (srcencoding), \
								   (destencoding))


/*
 * 若干 Unicode 专用测试的便捷函数。
 */
static inline bool
is_valid_unicode_codepoint(pg_wchar c)
{
	return (c > 0 && c <= 0x10FFFF);
}

static inline bool
is_utf16_surrogate_first(pg_wchar c)
{
	return (c >= 0xD800 && c <= 0xDBFF);
}

static inline bool
is_utf16_surrogate_second(pg_wchar c)
{
	return (c >= 0xDC00 && c <= 0xDFFF);
}

static inline pg_wchar
surrogate_pair_to_codepoint(pg_wchar first, pg_wchar second)
{
	return ((first & 0x3FF) << 10) + 0x10000 + (second & 0x3FF);
}


/*
 * 这些函数视为 libpq 导出 API 的一部分，
 * 亦在 libpq-fe.h 中声明。
 */
extern int	pg_char_to_encoding(const char *name);
extern const char *pg_encoding_to_char(int encoding);
extern int	pg_valid_server_encoding_id(int encoding);

/*
 * 链接 libpgcommon 的前端亦可使用这些函数
 * （另含刚述各项）。本文件先前声明的
 * 常量表亦可从 libpgcommon 取得。
 */
extern void pg_encoding_set_invalid(int encoding, char *dst);
extern int	pg_encoding_mblen(int encoding, const char *mbstr);
extern int	pg_encoding_mblen_or_incomplete(int encoding, const char *mbstr,
											size_t remaining);
extern int	pg_encoding_mblen_bounded(int encoding, const char *mbstr);
extern int	pg_encoding_dsplen(int encoding, const char *mbstr);
extern int	pg_encoding_verifymbchar(int encoding, const char *mbstr, int len);
extern int	pg_encoding_verifymbstr(int encoding, const char *mbstr, int len);
extern int	pg_encoding_max_length(int encoding);
extern int	pg_valid_client_encoding(const char *name);
extern int	pg_valid_server_encoding(const char *name);
extern bool is_encoding_supported_by_icu(int encoding);
extern const char *get_encoding_name_for_icu(int encoding);

extern unsigned char *unicode_to_utf8(pg_wchar c, unsigned char *utf8string);
extern pg_wchar utf8_to_unicode(const unsigned char *c);
extern bool pg_utf8_islegal(const unsigned char *source, int length);
extern int	pg_utf_mblen(const unsigned char *s);
extern int	pg_mule_mblen(const unsigned char *s);

/*
 * 其余函数仅限后端。
 */
extern int	pg_mb2wchar(const char *from, pg_wchar *to);
extern int	pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len);
extern int	pg_encoding_mb2wchar_with_len(int encoding,
										  const char *from, pg_wchar *to, int len);
extern int	pg_wchar2mb(const pg_wchar *from, char *to);
extern int	pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len);
extern int	pg_encoding_wchar2mb_with_len(int encoding,
										  const pg_wchar *from, char *to, int len);
extern int	pg_char_and_wchar_strcmp(const char *s1, const pg_wchar *s2);
extern int	pg_wchar_strncmp(const pg_wchar *s1, const pg_wchar *s2, size_t n);
extern int	pg_char_and_wchar_strncmp(const char *s1, const pg_wchar *s2, size_t n);
extern size_t pg_wchar_strlen(const pg_wchar *str);
extern int	pg_mblen(const char *mbstr);
extern int	pg_dsplen(const char *mbstr);
extern int	pg_mbstrlen(const char *mbstr);
extern int	pg_mbstrlen_with_len(const char *mbstr, int limit);
extern int	pg_mbcliplen(const char *mbstr, int len, int limit);
extern int	pg_encoding_mbcliplen(int encoding, const char *mbstr,
								  int len, int limit);
extern int	pg_mbcharcliplen(const char *mbstr, int len, int limit);
extern int	pg_database_encoding_max_length(void);
extern mbcharacter_incrementer pg_database_encoding_character_incrementer(void);

extern int	PrepareClientEncoding(int encoding);
extern int	SetClientEncoding(int encoding);
extern void InitializeClientEncoding(void);
extern int	pg_get_client_encoding(void);
extern const char *pg_get_client_encoding_name(void);

extern void SetDatabaseEncoding(int encoding);
extern int	GetDatabaseEncoding(void);
extern const char *GetDatabaseEncodingName(void);
extern void SetMessageEncoding(int encoding);
extern int	GetMessageEncoding(void);

#ifdef ENABLE_NLS
extern int	pg_bind_textdomain_codeset(const char *domainname);
#endif

extern unsigned char *pg_do_encoding_conversion(unsigned char *src, int len,
												int src_encoding,
												int dest_encoding);
extern int	pg_do_encoding_conversion_buf(Oid proc,
										  int src_encoding,
										  int dest_encoding,
										  unsigned char *src, int srclen,
										  unsigned char *dest, int destlen,
										  bool noError);

extern char *pg_client_to_server(const char *s, int len);
extern char *pg_server_to_client(const char *s, int len);
extern char *pg_any_to_server(const char *s, int len, int encoding);
extern char *pg_server_to_any(const char *s, int len, int encoding);

extern void pg_unicode_to_server(pg_wchar c, unsigned char *s);
extern bool pg_unicode_to_server_noerror(pg_wchar c, unsigned char *s);

extern unsigned short BIG5toCNS(unsigned short big5, unsigned char *lc);
extern unsigned short CNStoBIG5(unsigned short cns, unsigned char lc);

extern int	UtfToLocal(const unsigned char *utf, int len,
					   unsigned char *iso,
					   const pg_mb_radix_tree *map,
					   const pg_utf_to_local_combined *cmap, int cmapsize,
					   utf_local_conversion_func conv_func,
					   int encoding, bool noError);
extern int	LocalToUtf(const unsigned char *iso, int len,
					   unsigned char *utf,
					   const pg_mb_radix_tree *map,
					   const pg_local_to_utf_combined *cmap, int cmapsize,
					   utf_local_conversion_func conv_func,
					   int encoding, bool noError);

extern bool pg_verifymbstr(const char *mbstr, int len, bool noError);
extern bool pg_verify_mbstr(int encoding, const char *mbstr, int len,
							bool noError);
extern int	pg_verify_mbstr_len(int encoding, const char *mbstr, int len,
								bool noError);

extern void check_encoding_conversion_args(int src_encoding,
										   int dest_encoding,
										   int len,
										   int expected_src_encoding,
										   int expected_dest_encoding);

extern void report_invalid_encoding(int encoding, const char *mbstr, int len) pg_attribute_noreturn();
extern void report_untranslatable_char(int src_encoding, int dest_encoding,
									   const char *mbstr, int len) pg_attribute_noreturn();

extern int	local2local(const unsigned char *l, unsigned char *p, int len,
						int src_encoding, int dest_encoding,
						const unsigned char *tab, bool noError);
extern int	latin2mic(const unsigned char *l, unsigned char *p, int len,
					  int lc, int encoding, bool noError);
extern int	mic2latin(const unsigned char *mic, unsigned char *p, int len,
					  int lc, int encoding, bool noError);
extern int	latin2mic_with_table(const unsigned char *l, unsigned char *p,
								 int len, int lc, int encoding,
								 const unsigned char *tab, bool noError);
extern int	mic2latin_with_table(const unsigned char *mic, unsigned char *p,
								 int len, int lc, int encoding,
								 const unsigned char *tab, bool noError);

#ifdef WIN32
extern WCHAR *pgwin32_message_to_UTF16(const char *str, int len, int *utf16len);
#endif

#endif							/* PG_WCHAR_H */
