/*-------------------------------------------------------------------------
 *
 * spgist_private.h
 *	  SP-GiST 访问方法的私有声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgist_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGIST_PRIVATE_H
#define SPGIST_PRIVATE_H

#include "access/itup.h"
#include "access/spgist.h"
#include "catalog/pg_am_d.h"
#include "nodes/tidbitmap.h"
#include "storage/buf.h"
#include "utils/geo_decls.h"
#include "utils/relcache.h"


typedef struct SpGistOptions
{
	int32		varlena_header_;	/* varlena 头(勿直接碰!) */
	int			fillfactor;		/* 页填充因子百分数(0..100) */
} SpGistOptions;

#define SpGistGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == SPGIST_AM_OID), \
	 (relation)->rd_options ? \
	 ((SpGistOptions *) (relation)->rd_options)->fillfactor : \
	 SPGIST_DEFAULT_FILLFACTOR)
#define SpGistGetTargetPageFreeSpace(relation) \
	(BLCKSZ * (100 - SpGistGetFillFactor(relation)) / 100)


/* SPGiST 叶子元组有一键列,可选含包含列 */
#define spgKeyColumn 0
#define spgFirstIncludeColumn 1

/* 固定位置页的页号 */
#define SPGIST_METAPAGE_BLKNO	 (0)	/* 元页 */
#define SPGIST_ROOT_BLKNO		 (1)	/* 普通条目的根 */
#define SPGIST_NULL_BLKNO		 (2)	/* 空值条目的根 */
#define SPGIST_LAST_FIXED_BLKNO  SPGIST_NULL_BLKNO

#define SpGistBlockIsRoot(blkno) \
	((blkno) == SPGIST_ROOT_BLKNO || (blkno) == SPGIST_NULL_BLKNO)
#define SpGistBlockIsFixed(blkno) \
	((BlockNumber) (blkno) <= (BlockNumber) SPGIST_LAST_FIXED_BLKNO)

/*
 * SPGiST 索引页特殊空间的内容
 */
typedef struct SpGistPageOpaqueData
{
	uint16		flags;			/* 见下方位定义 */
	uint16		nRedirection;	/* 页上重定向元组数目 */
	uint16		nPlaceholder;	/* 页上占位元组数目 */
	/* 注意无 LIVE 或 DEAD 元组计数 ... */
	uint16		spgist_page_id; /* 用于识别 SP-GiST 索引 */
} SpGistPageOpaqueData;

typedef SpGistPageOpaqueData *SpGistPageOpaque;

/* 页特殊空间中的标志位 */
#define SPGIST_META			(1<<0)
#define SPGIST_DELETED		(1<<1)	/* 从不置位,但保留向后
									 * 兼容性*/
#define SPGIST_LEAF			(1<<2)
#define SPGIST_NULLS		(1<<3)

#define SpGistPageGetOpaque(page) ((SpGistPageOpaque) PageGetSpecialPointer(page))
#define SpGistPageIsMeta(page) (SpGistPageGetOpaque(page)->flags & SPGIST_META)
#define SpGistPageIsDeleted(page) (SpGistPageGetOpaque(page)->flags & SPGIST_DELETED)
#define SpGistPageIsLeaf(page) (SpGistPageGetOpaque(page)->flags & SPGIST_LEAF)
#define SpGistPageStoresNulls(page) (SpGistPageGetOpaque(page)->flags & SPGIST_NULLS)

/*
 * 页 ID 是为方便 pg_filedump 及类似工具而设，
 * 否则它们很难区分不同索引类型的
 * 页。它应位于页上最后 2 字节。这差不多因
 * 对齐考虑而"免费"。
 *
 * 见上文 GinPageOpaqueData 注释。
 */
#define SPGIST_PAGE_ID		0xFF82

/*
 * 每个后端在其 index->rd_amcache 区域中缓存最近使用页
 * 信息。自元页共享存储初始化,并偶尔写回
 * 那里。
 */
typedef struct SpGistLastUsedPage
{
	BlockNumber blkno;			/* 块号,或 InvalidBlockNumber */
	int			freeSpace;		/* 页的空闲空间(可能已过时!) */
} SpGistLastUsedPage;

/* 注意: cachedPage[] 下标与 SpGistGetBuffer 的标志赋值一致 */
#define SPGIST_CACHED_PAGES 8

typedef struct SpGistLUPCache
{
	SpGistLastUsedPage cachedPage[SPGIST_CACHED_PAGES];
} SpGistLUPCache;

/*
 * 元页
 */
typedef struct SpGistMetaPageData
{
	uint32		magicNumber;	/* 供身份交叉校验 */
	SpGistLUPCache lastUsedPages;	/* 最近用信息的共享存储 */
} SpGistMetaPageData;

#define SPGIST_MAGIC_NUMBER (0xBA0BABEE)

#define SpGistPageGetMeta(p) \
	((SpGistMetaPageData *) PageGetContents(p))

/*
 * 索引 AM 的私有状态。SpGistState 通用于插入与
 * 搜索代码;SpGistScanOpaque 仅用于搜索。
 */

typedef struct SpGistLeafTupleData *SpGistLeafTuple;	/* 前向引用 */

/* SpGistState 所需每数据类型信息 */
typedef struct SpGistTypeDesc
{
	Oid			type;
	int16		attlen;
	bool		attbyval;
	char		attalign;
	char		attstorage;
} SpGistTypeDesc;

typedef struct SpGistState
{
	Relation	index;			/* 正在处理的索引 */

	spgConfigOut config;		/* 由操作符类配置方法填入 */

	SpGistTypeDesc attType;		/* 待索引或还原值类型 */
	SpGistTypeDesc attLeafType; /* 叶子元组值的类型 */
	SpGistTypeDesc attPrefixType;	/* 内部元组前缀值的类型 */
	SpGistTypeDesc attLabelType;	/* 节点标签值的类型 */

	/* leafTupDesc 通常指向索引的 tupdesc,但并非总是 */
	TupleDesc	leafTupDesc;	/* 叶子级元组的描述符 */

	char	   *deadTupleStorage;	/* spgFormDeadTuple 的工作区 */

	TransactionId myXid;		/* 创建重定向元组的XID */
	bool		isBuild;		/* 做索引构建时为真 */
} SpGistState;

/* 搜索中稍后待重检的项 */
typedef struct SpGistSearchItem
{
	pairingheap_node phNode;	/* 配对堆节点 */
	Datum		value;			/* 自父重构的值,或
								 * leafValue 若 isLeaf*/
	SpGistLeafTuple leafTuple;	/* 整叶子元组,若需要 */
	void	   *traversalValue; /* 操作符类专用遍历值 */
	int			level;			/* 本页项的层级 */
	ItemPointerData heapPtr;	/* 堆信息,若堆元组 */
	bool		isNull;			/* SearchItem 是 NULL 项 */
	bool		isLeaf;			/* SearchItem 是堆项 */
	bool		recheck;		/* 限定需重检 */
	bool		recheckDistances;	/* 距离需重检 */

	/* numberOfOrderBys 个条目的数组 */
	double		distances[FLEXIBLE_ARRAY_MEMBER];
} SpGistSearchItem;

#define SizeOfSpGistSearchItem(n_distances) \
	(offsetof(SpGistSearchItem, distances) + sizeof(double) * (n_distances))

/*
 * 索引扫描的私有状态
 */
typedef struct SpGistScanOpaqueData
{
	SpGistState state;			/* 见上文 */
	pairingheap *scanQueue;		/* 待访问项的队列 */
	MemoryContext tempCxt;		/* 短寿命内存上下文 */
	MemoryContext traversalCxt; /* 单次扫描寿命内存上下文 */

	/* 控制是否搜索空值与/或非空值的标志 */
	bool		searchNulls;	/* 扫描匹配(全部)空条目 */
	bool		searchNonNulls; /* 扫描匹配(部分)非空条目 */

	/* 传给操作符类的索引限定(已去掉空值相关限定) */
	int			numberOfKeys;	/* 索引限定条件的数目 */
	ScanKey		keyData;		/* 索引限定描述符数组 */
	int			numberOfOrderBys;	/* 排序操作符的数目 */
	int			numberOfNonNullOrderBys;	/* 排序操作符的数目
											 * 带非 NULL 参数者*/
	ScanKey		orderByData;	/* 排序操作符描述符数组 */
	Oid		   *orderByTypes;	/* 排序操作符返回类型数组 */
	int		   *nonNullOrderByOffsets;	/* 非 NULL 排序键在原
										 * 数组中的偏移数组*/
	Oid			indexCollation; /* 索引列的排序规则 */

	/* 操作符类定义函数: */
	FmgrInfo	innerConsistentFn;
	FmgrInfo	leafConsistentFn;

	/* 预分配的工作区数组: */
	double	   *zeroDistances;
	double	   *infDistances;

	/* 这些字段仅用于 amgetbitmap 扫描: */
	TIDBitmap  *tbm;			/* 填充中的位图 */
	int64		ntids;			/* 传给位图的 TIDs 数 */

	/* 这些字段仅用于 amgettuple 扫描: */
	bool		want_itup;		/* 是否在重构元组? */
	TupleDesc	reconTupDesc;	/* 若是,则重构元组的描述符 */
	int			nPtrs;			/* 当前页上找到的 TIDs 数 */
	int			iPtr;			/* 扫描同一数组的下标 */
	ItemPointerData heapPtrs[MaxIndexTuplesPerPage];	/* 当前页TIDs */
	bool		recheck[MaxIndexTuplesPerPage]; /* 其重检标志 */
	bool		recheckDistances[MaxIndexTuplesPerPage];	/* 距离重检
															 * 标志*/
	HeapTuple	reconTups[MaxIndexTuplesPerPage];	/* 重构的元组 */

	/* 距离(供重检) */
	IndexOrderByDistance *distances[MaxIndexTuplesPerPage];

	/*
	 * 注意: 上文用 MaxIndexTuplesPerPage 有点别扭,因 
	 * SpGistLeafTuples 并非严格的 IndexTuples;但它们更大,
	 * 故此安全。
	 */
} SpGistScanOpaqueData;

typedef SpGistScanOpaqueData *SpGistScanOpaque;

/*
 * 本结构才是实际保存在 index->rd_amcache 中的内容。它含
 * 静态配置信息以及 lastUsedPages 缓存。
 */
typedef struct SpGistCache
{
	spgConfigOut config;		/* 由操作符类配置方法填入 */

	SpGistTypeDesc attType;		/* 待索引或还原值类型 */
	SpGistTypeDesc attLeafType; /* 叶子元组值的类型 */
	SpGistTypeDesc attPrefixType;	/* 内部元组前缀值的类型 */
	SpGistTypeDesc attLabelType;	/* 节点标签值的类型 */

	SpGistLUPCache lastUsedPages;	/* 最近用信息的本地存储 */
} SpGistCache;


/*
 * SPGiST 元组类型。注意:内部、叶子与死元组结构
 * 必须在相同位置有相同的 tupstate 字段!真实的内部与
 * 叶子元组始终 tupstate = LIVE;若状态是别的,
 * 用 SpGistDeadTuple 结构检查该元组。
 */

/* tupstate 取值(详见 README) */
#define SPGIST_LIVE			0	/* 正常存活元组(内部或叶子) */
#define SPGIST_REDIRECT		1	/* 临时重定向占位 */
#define SPGIST_DEAD			2	/* 已死,因链接而无法移除 */
#define SPGIST_PLACEHOLDER	3	/* 占位,用于保留偏移 */

/*
 * SPGiST 内部元组: 细分一组元组的"节点"列表
 *
 * 内部元组布局:
 * 头/可选前缀/节点数组,节点为 SpGistNodeTuples
 *
 * size 与 prefixSize 必须是 MAXALIGN 的倍数
 *
 * 若前缀 datum 为按值传递类型,则以其
 * Datum 表示存储,即其盘上表示长度为
 * sizeof(Datum)。这是相当不幸的选择,因为 Postgres
 * 别处从不把 Datum 用作盘上表示;它造成
 * 32 位与 64 位构建间不必要的不兼容。但该
 * 兼容性损失多半是理论上的,因 MAXIMUM_ALIGNOF 通常
 * 在这类构建间也不同。反正我们现在已被拴住了。
 */
typedef struct SpGistInnerTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				allTheSame:1,	/* 元组内全部节点等价 */
				nNodes:13,		/* 内部元组内的节点数 */
				prefixSize:16;	/* 前缀大小,无则为 0 */
	uint16		size;			/* 内部元组总大小 */
	/* 在多数机器上这里会浪费几个字节 */
	/* 后跟前缀 datum,再是节点 */
} SpGistInnerTupleData;

typedef SpGistInnerTupleData *SpGistInnerTuple;

/* 这些必须匹配上文位域声明所能容纳的最大值 */
#define SGITMAXNNODES		0x1FFF
#define SGITMAXPREFIXSIZE	0xFFFF
#define SGITMAXSIZE			0xFFFF

#define SGITHDRSZ			MAXALIGN(sizeof(SpGistInnerTupleData))
#define _SGITDATA(x)		(((char *) (x)) + SGITHDRSZ)
#define SGITDATAPTR(x)		((x)->prefixSize ? _SGITDATA(x) : NULL)
#define SGITDATUM(x, s)		((x)->prefixSize ? \
							 ((s)->attPrefixType.attbyval ? \
							  *(Datum *) _SGITDATA(x) : \
							  PointerGetDatum(_SGITDATA(x))) \
							 : (Datum) 0)
#define SGITNODEPTR(x)		((SpGistNodeTuple) (_SGITDATA(x) + (x)->prefixSize))

/* 遍历内部元组节点的宏 */
#define SGITITERATE(x, i, nt)	\
	for ((i) = 0, (nt) = SGITNODEPTR(x); \
		 (i) < (x)->nNodes; \
		 (i)++, (nt) = (SpGistNodeTuple) (((char *) (nt)) + IndexTupleSize(nt)))

/*
 * SPGiST 节点元组: 内部元组内一节点
 *
 * 节点元组使用与普通 Postgres IndexTuples 相同的头,但
 * 我们不用空值位图,因已知只有一列,
 * 故 INDEX_NULL_MASK 位已够。另外,按值传递 datum
 * 以 Datum 形式存储,与内部元组前缀的约定相同。
 */

typedef IndexTupleData SpGistNodeTupleData;

typedef SpGistNodeTupleData *SpGistNodeTuple;

#define SGNTHDRSZ			MAXALIGN(sizeof(SpGistNodeTupleData))
#define SGNTDATAPTR(x)		(((char *) (x)) + SGNTHDRSZ)
#define SGNTDATUM(x, s)		((s)->attLabelType.attbyval ? \
							 *(Datum *) SGNTDATAPTR(x) : \
							 PointerGetDatum(SGNTDATAPTR(x)))

/*
 * SPGiST 叶子元组: 携带叶子 datum 与堆元组 TID,
 * 并可选带若干"包含"列。
 *
 * 最简单情形下,叶子 datum 与被索引值相同;
 * 但也可以是后缀或其他某种增量,从而在知晓
 * 到达此处所经前缀路径后允许重构。
 * 任何包含列均原样存储、不加修改。
 *
 * 若存在包含列且任一 datum 为 NULL,则有
 * 空值位图。对无包含列的空叶子 datum,我们不
 * 需要空值位图,因可从元组是否存于空值页推断叶子 
 * datum 是否为空。(此项规定主要为向后兼容,但在
 * 32 位机上确能省空间。)与其他 PG 索引元组设计一样,
 * 若空值位图存在,则其大小为 INDEX_MAX_KEYS 位,与
 * 实际属性数目无关。对 INDEX_MAX_KEYS 的通常选取,
 * 因对齐考虑这不
 * 费额外空间。
 *
 * size 字段比盘上叶子元组可能需要的更宽,
 * 但这允许即便 datum 过宽无法立即存储时仍构造叶子
 * 元组,且因对齐考虑并无
 * 额外代价。
 *
 * t_info 持有 nextOffset 字段(14 位宽,足以覆盖受支持的
 * 页大小)加上 has-nulls-bitmap 标志位;另一标志位空闲。
 *
 * 通常,nextOffset 链到属于同一父节点的下一元组
 * (必须在同一页上),若无下一元组则为 0。
 * 但当根页是叶子页时,我们不链其元组,
 * 故根上 nextOffset 恒为 0。
 *
 * size 必须是 MAXALIGN 的倍数;且必须至少为 SGDTSIZE,
 * 以便稍后可将该元组转为 REDIRECT 状态。(此
 * 限制仅在 32 位机器上为存于其上的 NULL 叶子 datum
 * 增加字节;否则对齐限制反正会强制如此。)
 */
typedef struct SpGistLeafTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				size:30;		/* 足以容纳任何可 palloc 的值 */
	uint16		t_info;			/* nextOffset,链到链中下一
								 * 元组,外加两个标志位*/
	ItemPointerData heapPtr;	/* 所表示堆元组的 TID */
	/* 若对应标志位置位则后跟空值位图 */
	/* 叶子 datum,再是任何包含 datum,落在 MAXALIGN 边界 */
} SpGistLeafTupleData;

/* 访问 t_info 内 nextOffset 与位域的宏 */
#define SGLT_GET_NEXTOFFSET(spgLeafTuple) \
	((spgLeafTuple)->t_info & 0x3FFF)
#define SGLT_GET_HASNULLMASK(spgLeafTuple) \
	(((spgLeafTuple)->t_info & 0x8000) ? true : false)
#define SGLT_SET_NEXTOFFSET(spgLeafTuple, offsetNumber) \
	((spgLeafTuple)->t_info = \
	 ((spgLeafTuple)->t_info & 0xC000) | ((offsetNumber) & 0x3FFF))
#define SGLT_SET_HASNULLMASK(spgLeafTuple, hasnulls) \
	((spgLeafTuple)->t_info = \
	 ((spgLeafTuple)->t_info & 0x7FFF) | ((hasnulls) ? 0x8000 : 0))

#define SGLTHDRSZ(hasnulls) \
	((hasnulls) ? MAXALIGN(sizeof(SpGistLeafTupleData) + \
						   sizeof(IndexAttributeBitMapData)) : \
	 MAXALIGN(sizeof(SpGistLeafTupleData)))
#define SGLTDATAPTR(x)		(((char *) (x)) + SGLTHDRSZ(SGLT_GET_HASNULLMASK(x)))
#define SGLTDATUM(x, s)		fetch_att(SGLTDATAPTR(x), \
									  (s)->attLeafType.attbyval, \
									  (s)->attLeafType.attlen)

/*
 * SPGiST 死元组: 用于检查非存活元组的声明
 *
 * 本结构的 tupstate 字段必须与常规内部与
 * 叶子元组一致,且其 size 须匹配叶子元组的。
 * 另,pointer 字段须与叶子元组的 heapPtr 
 * 字段同处,以满足若干 Asserts,当我们把叶子元组替换
 * 为死元组。
 * 我们不用 t_info,但需要它来对齐 pointer 字段。
 * pointer 与 xid 仅在 tupstate = REDIRECT 时有效,且有些
 * 情形下 xid 仍可为 InvalidTransactionId;见 initSpGistState。
 */
typedef struct SpGistDeadTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				size:30;
	uint16		t_info;			/* 死元组中不用 */
	ItemPointerData pointer;	/* 索引内重定向 */
	TransactionId xid;			/* 插入本元组的事务 ID */
} SpGistDeadTupleData;

typedef SpGistDeadTupleData *SpGistDeadTuple;

#define SGDTSIZE		MAXALIGN(sizeof(SpGistDeadTupleData))

/*
 * 做空闲空间计算的宏。注意在累计元组所需
 * 空间时,我们总把每个元组算作需要该元组的 
 * size 加 sizeof(ItemIdData)(用于行指针)。只要元组
 * 大小始终 maxaligned,这就正确。
 */

/* 扣除固定头与特殊空间后的页容量 */
#define SPGIST_PAGE_CAPACITY  \
	MAXALIGN_DOWN(BLCKSZ - \
				  SizeOfPageHeaderData - \
				  MAXALIGN(sizeof(SpGistPageOpaqueData)))

/*
 * 计算页上空闲空间,假定至多可回收 n 个占位
 * (若存在;n 应为待插入元组数目)
 */
#define SpGistPageGetFreeSpace(p, n) \
	(PageGetExactFreeSpace(p) + \
	 Min(SpGistPageGetOpaque(p)->nPlaceholder, n) * \
	 (SGDTSIZE + sizeof(ItemIdData)))

/*
 * XLOG杂项
 */

#define STORE_STATE(s, d)  \
	do { \
		(d).myXid = (s)->myXid; \
		(d).isBuild = (s)->isBuild; \
	} while(0)

/*
 * SpGistGetBuffer 的 "flags" 参数应为 GBUF_LEAF 以
 * 取得叶子页,或 GBUF_INNER_PARITY(blockNumber) 以取得与
 * 指定块号同一三奇偶组中的内部页。
 * (通常应为 GBUF_INNER_PARITY(parentBlockNumber + 1)
 * 以遵循 spgist/README 所述规则。)
 * 此外可 OR 上 GBUF_NULLS 以取得存放
 * 空值元组页。
 *
 * 注意: 这些标志值用作 lastUsedPages 的下标。
 */
#define GBUF_LEAF				0x03
#define GBUF_INNER_PARITY(x)	((x) % 3)
#define GBUF_NULLS				0x04

#define GBUF_PARITY_MASK		0x03
#define GBUF_REQ_LEAF(flags)	(((flags) & GBUF_PARITY_MASK) == GBUF_LEAF)
#define GBUF_REQ_NULLS(flags)	((flags) & GBUF_NULLS)

/* spgutils.c */

/* 关系选项参数 */
#define SPGIST_MIN_FILLFACTOR			10
#define SPGIST_DEFAULT_FILLFACTOR		80

extern SpGistCache *spgGetCache(Relation index);
extern TupleDesc getSpGistTupleDesc(Relation index, SpGistTypeDesc *keyType);
extern void initSpGistState(SpGistState *state, Relation index);
extern Buffer SpGistNewBuffer(Relation index);
extern void SpGistUpdateMetaPage(Relation index);
extern Buffer SpGistGetBuffer(Relation index, int flags,
							  int needSpace, bool *isNew);
extern void SpGistSetLastUsedPage(Relation index, Buffer buffer);
extern void SpGistInitPage(Page page, uint16 f);
extern void SpGistInitBuffer(Buffer b, uint16 f);
extern void SpGistInitMetapage(Page page);
extern unsigned int SpGistGetInnerTypeSize(SpGistTypeDesc *att, Datum datum);
extern Size SpGistGetLeafTupleSize(TupleDesc tupleDescriptor,
								   Datum *datums, bool *isnulls);
extern SpGistLeafTuple spgFormLeafTuple(SpGistState *state,
										ItemPointer heapPtr,
										Datum *datums, bool *isnulls);
extern SpGistNodeTuple spgFormNodeTuple(SpGistState *state,
										Datum label, bool isnull);
extern SpGistInnerTuple spgFormInnerTuple(SpGistState *state,
										  bool hasPrefix, Datum prefix,
										  int nNodes, SpGistNodeTuple *nodes);
extern SpGistDeadTuple spgFormDeadTuple(SpGistState *state, int tupstate,
										BlockNumber blkno, OffsetNumber offnum);
extern void spgDeformLeafTuple(SpGistLeafTuple tup, TupleDesc tupleDescriptor,
							   Datum *datums, bool *isnulls,
							   bool keyColumnIsNull);
extern Datum *spgExtractNodeLabels(SpGistState *state,
								   SpGistInnerTuple innerTuple);
extern OffsetNumber SpGistPageAddNewItem(SpGistState *state, Page page,
										 Item item, Size size,
										 OffsetNumber *startOffset,
										 bool errorOK);
extern bool spgproperty(Oid index_oid, int attno,
						IndexAMProperty prop, const char *propname,
						bool *res, bool *isnull);

/* spgdoinsert.c */
extern void spgUpdateNodeLink(SpGistInnerTuple tup, int nodeN,
							  BlockNumber blkno, OffsetNumber offset);
extern void spgPageIndexMultiDelete(SpGistState *state, Page page,
									OffsetNumber *itemnos, int nitems,
									int firststate, int reststate,
									BlockNumber blkno, OffsetNumber offnum);
extern bool spgdoinsert(Relation index, SpGistState *state,
						ItemPointer heapPtr, Datum *datums, bool *isnulls);

/* spgproc.c */
extern double *spg_key_orderbys_distances(Datum key, bool isLeaf,
										  ScanKey orderbys, int norderbys);
extern BOX *box_copy(BOX *orig);

#endif							/* SPGIST_PRIVATE_H */
