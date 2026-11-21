/*-------------------------------------------------------------------------
 *
 * spell.h
 *
 * ISpell 词典的声明
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/tsearch/dicts/spell.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef __SPELL_H__
#define __SPELL_H__

#include "regex/regex.h"
#include "tsearch/dicts/regis.h"
#include "tsearch/ts_public.h"

/*
 * SPNode/SPNodeData 表示存词表的前缀树
 * （Trie）。
 */
struct SPNode;

typedef struct
{
	uint32		val:8,
				isword:1,
	/*  存储下列复合标志  */
				compoundflag:4,
	/*  对 AffixData 字段某项的引用  */
				affix:19;
	struct SPNode *node;
} SPNodeData;

/*
 * FF_ 名称与词缀文件中 Hunspell 选项对应
 * https://hunspell.github.io/
 */
#define FF_COMPOUNDONLY		0x01
#define FF_COMPOUNDBEGIN	0x02
#define FF_COMPOUNDMIDDLE	0x04
#define FF_COMPOUNDLAST		0x08
#define FF_COMPOUNDFLAG		( FF_COMPOUNDBEGIN | FF_COMPOUNDMIDDLE | \
							FF_COMPOUNDLAST )
#define FF_COMPOUNDFLAGMASK		0x0f

typedef struct SPNode
{
	uint32		length;
	SPNodeData	data[FLEXIBLE_ARRAY_MEMBER];
} SPNode;

#define SPNHDRSZ	(offsetof(SPNode,data))

/*
 * 表示词表中的一项。
 */
typedef struct spell_struct
{
	union
	{
		/*
		 * flag 由 NIImportDictionary() 填充。经
		 * NISortDictionary() 后改用 d 而非 flag。
		 */
		char	   *flag;
		/*  d 用于 mkSPNode()  */
		struct
		{
			/*  对 AffixData 字段某项的引用  */
			int			affix;
			/*  词长  */
			int			len;
		}			d;
	}			p;
	char		word[FLEXIBLE_ARRAY_MEMBER];
} SPELL;

#define SPELLHDRSZ	(offsetof(SPELL, word))

/*
 * 表示词缀列表中的一项。
 */
typedef struct aff_struct
{
	char	   *flag;
	/*  FF_SUFFIX/FF_PREFIX  */
	uint32		type:1,
				flagflags:7,
				issimple:1,
				isregis:1,
				replen:14;
	char	   *find;
	char	   *repl;
	union
	{
		/*
		 * AFFIX 数组会被移动与排序。用 regex_t 指针
		 * 保持本结构体小，并避免假定 regex_t
		 * 可移。
		 */
		regex_t    *pregex;
		Regis		regis;
	}			reg;
} AFFIX;

/*
 * 词缀也使用词典标志
 */
#define FF_COMPOUNDPERMITFLAG	0x10
#define FF_COMPOUNDFORBIDFLAG	0x20
#define FF_CROSSPRODUCT			0x40

/*
 * 勿改这些顺序。初始化按此排序，并期望
 * 排序后前缀在前。
 */
#define FF_SUFFIX				1
#define FF_PREFIX				0

/*
 * AffixNode/AffixNodeData 表示存词缀列表的
 * （Trie）。
 */
struct AffixNode;

typedef struct
{
	uint32		val:8,
				naff:24;
	AFFIX	  **aff;
	struct AffixNode *node;
} AffixNodeData;

typedef struct AffixNode
{
	uint32		isvoid:1,
				length:31;
	AffixNodeData data[FLEXIBLE_ARRAY_MEMBER];
} AffixNode;

#define ANHRDSZ		   (offsetof(AffixNode, data))

typedef struct
{
	char	   *affix;
	int			len;
	bool		issuffix;
} CMPDAffix;

/*
 * Hunspell 词典中词缀标志的编码类型
 */
typedef enum
{
	FM_CHAR,					/*  单字符（如 ispell）  */
	FM_LONG,					/*  两字符  */
	FM_NUM						/*  数字 >=0 且 <65536  */
} FlagMode;

/*
 * 存储 Hunspell 选项的结构。标志表示依赖标志
 * 类型。这些标志关乎复合词支持。
 */
typedef struct CompoundAffixFlag
{
	union
	{
		/* 。 */
		char	   *s;
		/* 。 */
		uint32		i;
	}			flag;
	/*  无 bsearch_arg 版，故复制 FlagMode  */
	FlagMode	flagMode;
	uint32		value;
} CompoundAffixFlag;

#define FLAGNUM_MAXSIZE		(1 << 16)

typedef struct
{
	int			maffixes;
	int			naffixes;
	AFFIX	   *Affix;

	AffixNode  *Suffix;
	AffixNode  *Prefix;

	SPNode	   *Dictionary;
	/*  词缀集合数组  */
	char	  **AffixData;
	int			lenAffixData;
	int			nAffixData;
	bool		useFlagAliases;

	CMPDAffix  *CompoundAffix;

	bool		usecompound;
	FlagMode	flagMode;

	/*
	 * 下列字段实际仅初始化时需要
	 */

	/*  词缀文件中 Hunspell 选项数组  */
	CompoundAffixFlag *CompoundAffixFlags;
	/*  CompoundAffixFlags 数组项数  */
	int			nCompoundAffixFlag;
	/*  CompoundAffixFlags 数组已分配长度  */
	int			mCompoundAffixFlag;

	/*
	 * 其余字段仅词典构建时使用；由
	 * NIStartBuild 设置、NIFinishBuild 清除。
	 */
	MemoryContext buildCxt;		/*  构建用临时上下文  */

	/*  词典文件中全部词的临时数组  */
	SPELL	  **Spell;
	int			nspell;			/*  Spell 数组中有效项数  */
	int			mspell;			/*  Spell 数组已分配长度  */

	/*  用于无 palloc 开销地分配"紧凑"数据  */
	char	   *firstfree;		/* 。 */
	size_t		avail;			/*  firstfree 处剩余空闲空间  */
} IspellDict;

extern TSLexeme *NINormalizeWord(IspellDict *Conf, char *word);

extern void NIStartBuild(IspellDict *Conf);
extern void NIImportAffixes(IspellDict *Conf, const char *filename);
extern void NIImportDictionary(IspellDict *Conf, const char *filename);
extern void NISortDictionary(IspellDict *Conf);
extern void NISortAffixes(IspellDict *Conf);
extern void NIFinishBuild(IspellDict *Conf);

#endif
