/*--------------------------------------------------------------------------
 * gin_private.h
 *	  Postgres 倒排索引访问方法实现的头文件。
 *
 *	Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *	src/include/access/gin_private.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_PRIVATE_H
#define GIN_PRIVATE_H

#include "access/amapi.h"
#include "access/gin.h"
#include "access/ginblock.h"
#include "access/itup.h"
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "lib/rbtree.h"
#include "storage/bufmgr.h"

/*
 * GIN 关系选项的存储类型
 */
typedef struct GinOptions
{
	int32		vl_len_;		/* varlena 头(勿直接碰!) */
	bool		useFastUpdate;	/* 用快速更新? */
	int			pendingListCleanupSize; /* 挂起列表的最大尺寸 */
} GinOptions;

#define GIN_DEFAULT_USE_FASTUPDATE	true
#define GinGetUseFastUpdate(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GIN_AM_OID), \
	 (relation)->rd_options ? \
	 ((GinOptions *) (relation)->rd_options)->useFastUpdate : GIN_DEFAULT_USE_FASTUPDATE)
#define GinGetPendingListCleanupSize(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == GIN_AM_OID), \
	 (relation)->rd_options && \
	 ((GinOptions *) (relation)->rd_options)->pendingListCleanupSize != -1 ? \
	 ((GinOptions *) (relation)->rd_options)->pendingListCleanupSize : \
	 gin_pending_list_limit)


/* 缓冲区加锁/解锁操作的宏 */
#define GIN_UNLOCK	BUFFER_LOCK_UNLOCK
#define GIN_SHARE	BUFFER_LOCK_SHARE
#define GIN_EXCLUSIVE  BUFFER_LOCK_EXCLUSIVE


/*
 * GinState: 描述当前所处理索引的工作数据结构
 */
typedef struct GinState
{
	Relation	index;
	bool		oneCol;			/* 若为单列索引则为真 */

	/*
	 * origTupdesc 是索引的名义元组描述符，即第 i 个
	 * 属性给出第 i 个索引列键类型(非输入数据类型!)。
	 * 在单列索引中它描述实际的叶子索引
	 * 元组。在多列索引中，实际叶子元组含有
	 * 一个 smallint 列号，后跟该列相应类型的
	 * 键 datum。我们设 tupdesc[i] 以描述实际的
	 * 第 i 列索引元组行类型，即 (int2, keytype)。
	 * 注意无论何种情况，叶子元组都含有比
	 * TupleDesc 所知更多；见 access/gin/README。
	 */
	TupleDesc	origTupdesc;
	TupleDesc	tupdesc[INDEX_MAX_KEYS];

	/*
	 * 每索引列的操作符类支持函数
	 */
	FmgrInfo	compareFn[INDEX_MAX_KEYS];
	FmgrInfo	extractValueFn[INDEX_MAX_KEYS];
	FmgrInfo	extractQueryFn[INDEX_MAX_KEYS];
	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	triConsistentFn[INDEX_MAX_KEYS];
	FmgrInfo	comparePartialFn[INDEX_MAX_KEYS];	/* 可选方法 */
	/* canPartialMatch[i] 在 comparePartialFn[i] 有效时为真 */
	bool		canPartialMatch[INDEX_MAX_KEYS];
	/* 传给支持函数的排序规则 */
	Oid			supportCollation[INDEX_MAX_KEYS];
} GinState;


/* ginutil.c */
extern bytea *ginoptions(Datum reloptions, bool validate);
extern void initGinState(GinState *state, Relation index);
extern Buffer GinNewBuffer(Relation index);
extern void GinInitBuffer(Buffer b, uint32 f);
extern void GinInitPage(Page page, uint32 f, Size pageSize);
extern void GinInitMetabuffer(Buffer b);
extern int	ginCompareEntries(GinState *ginstate, OffsetNumber attnum,
							  Datum a, GinNullCategory categorya,
							  Datum b, GinNullCategory categoryb);
extern int	ginCompareAttEntries(GinState *ginstate,
								 OffsetNumber attnuma, Datum a, GinNullCategory categorya,
								 OffsetNumber attnumb, Datum b, GinNullCategory categoryb);
extern Datum *ginExtractEntries(GinState *ginstate, OffsetNumber attnum,
								Datum value, bool isNull,
								int32 *nentries, GinNullCategory **categories);

extern OffsetNumber gintuple_get_attrnum(GinState *ginstate, IndexTuple tuple);
extern Datum gintuple_get_key(GinState *ginstate, IndexTuple tuple,
							  GinNullCategory *category);

/* gininsert.c */
extern IndexBuildResult *ginbuild(Relation heap, Relation index,
								  struct IndexInfo *indexInfo);
extern void ginbuildempty(Relation index);
extern bool gininsert(Relation index, Datum *values, bool *isnull,
					  ItemPointer ht_ctid, Relation heapRel,
					  IndexUniqueCheck checkUnique,
					  bool indexUnchanged,
					  struct IndexInfo *indexInfo);
extern void ginEntryInsert(GinState *ginstate,
						   OffsetNumber attnum, Datum key, GinNullCategory category,
						   ItemPointerData *items, uint32 nitem,
						   GinStatsData *buildStats);

/* ginbtree.c */

typedef struct GinBtreeStack
{
	BlockNumber blkno;
	Buffer		buffer;
	OffsetNumber off;
	ItemPointerData iptr;
	/* predictNumber 含当前层上预测的页数 */
	uint32		predictNumber;
	struct GinBtreeStack *parent;
} GinBtreeStack;

typedef struct GinBtreeData *GinBtree;

/* GinBtreeData.beginPlaceToPage 方法的返回码 */
typedef enum
{
	GPTP_NO_WORK,
	GPTP_INSERT,
	GPTP_SPLIT
} GinPlaceToPageRC;

typedef struct GinBtreeData
{
	/* 搜索方法 */
	BlockNumber (*findChildPage) (GinBtree, GinBtreeStack *);
	BlockNumber (*getLeftMostChild) (GinBtree, Page);
	bool		(*isMoveRight) (GinBtree, Page);
	bool		(*findItem) (GinBtree, GinBtreeStack *);

	/* 插入方法 */
	OffsetNumber (*findChildPtr) (GinBtree, Page, BlockNumber, OffsetNumber);
	GinPlaceToPageRC (*beginPlaceToPage) (GinBtree, Buffer, GinBtreeStack *, void *, BlockNumber, void **, Page *, Page *);
	void		(*execPlaceToPage) (GinBtree, Buffer, GinBtreeStack *, void *, BlockNumber, void *);
	void	   *(*prepareDownlink) (GinBtree, Buffer);
	void		(*fillRoot) (GinBtree, Page, BlockNumber, Page, BlockNumber, Page);

	bool		isData;

	Relation	index;
	BlockNumber rootBlkno;
	GinState   *ginstate;		/* 在数据扫描中无效 */
	bool		fullScan;
	bool		isBuild;

	/* 条目树的搜索键 */
	OffsetNumber entryAttnum;
	Datum		entryKey;
	GinNullCategory entryCategory;

	/* 数据树(倒排树)的搜索键 */
	ItemPointerData itemptr;
} GinBtreeData;

/* 这表示将插入条目树的一个元组。 */
typedef struct
{
	IndexTuple	entry;			/* 待插入元组 */
	bool		isDelete;		/* 删除同偏移处的旧元组? */
} GinBtreeEntryInsertData;

/*
 * 这表示一个项指针，或多个项指针，将插入到
 * 数据(倒排树)叶子页
 */
typedef struct
{
	ItemPointerData *items;
	uint32		nitem;
	uint32		curitem;
} GinBtreeDataLeafInsertData;

/*
 * 对内部数据(倒排树)页，插入有效载荷是一个
 * PostingItem
 */

extern GinBtreeStack *ginFindLeafPage(GinBtree btree, bool searchMode,
									  bool rootConflictCheck, Snapshot snapshot);
extern Buffer ginStepRight(Buffer buffer, Relation index, int lockmode);
extern void freeGinBtreeStack(GinBtreeStack *stack);
extern void ginInsertValue(GinBtree btree, GinBtreeStack *stack,
						   void *insertdata, GinStatsData *buildStats);

/* ginentrypage.c */
extern IndexTuple GinFormTuple(GinState *ginstate,
							   OffsetNumber attnum, Datum key, GinNullCategory category,
							   Pointer data, Size dataSize, int nipd, bool errorTooBig);
extern void ginPrepareEntryScan(GinBtree btree, OffsetNumber attnum,
								Datum key, GinNullCategory category,
								GinState *ginstate);
extern void ginEntryFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage);
extern ItemPointer ginReadTuple(GinState *ginstate, OffsetNumber attnum,
								IndexTuple itup, int *nitems);

/* gindatapage.c */
extern ItemPointer GinDataLeafPageGetItems(Page page, int *nitems, ItemPointerData advancePast);
extern int	GinDataLeafPageGetItemsToTbm(Page page, TIDBitmap *tbm);
extern BlockNumber createPostingTree(Relation index,
									 ItemPointerData *items, uint32 nitems,
									 GinStatsData *buildStats, Buffer entrybuffer);
extern void GinDataPageAddPostingItem(Page page, PostingItem *data, OffsetNumber offset);
extern void GinPageDeletePostingItem(Page page, OffsetNumber offset);
extern void ginInsertItemPointers(Relation index, BlockNumber rootBlkno,
								  ItemPointerData *items, uint32 nitem,
								  GinStatsData *buildStats);
extern GinBtreeStack *ginScanBeginPostingTree(GinBtree btree, Relation index, BlockNumber rootBlkno, Snapshot snapshot);
extern void ginDataFillRoot(GinBtree btree, Page root, BlockNumber lblkno, Page lpage, BlockNumber rblkno, Page rpage);

/*
 * 这在 ginvacuum.c 中声明，但作为不透明结构在
 * ginVacuumItemPointers 与 ginVacuumPostingTreeLeaf 间传递，故
 * 需前向声明。
 */
typedef struct GinVacuumState GinVacuumState;

extern void ginVacuumPostingTreeLeaf(Relation indexrel, Buffer buffer,
									 GinVacuumState *gvs);

/* ginscan.c */

/*
 * GinScanKeyData 描述单个 GIN 索引限定表达式。
 *
 * 自每个限定表达式提取一个或多个具体的索引搜索
 * 条件，它们由 GinScanEntryData 表示。多个限定式
 * 完全可能请求相同的搜索条件；此时我们合并这些
 * 条件，使只有一个唯一的 GinScanEntry --- 这对处理
 * 全索引扫描条目时的效率尤为重要。因此可以有多个
 * GinScanKeyData.scanEntry 指针指向同一个
 * GinScanEntryData。
 *
 * 在每个 GinScanKeyData 中，nentries 是条目真实数目，而
 * nuserentries 是 extractQueryFn 返回的数目(也就是我们
 * 报告给 consistentFn 的)。"user" 条目必须在前。
 */
typedef struct GinScanKeyData *GinScanKey;

typedef struct GinScanEntryData *GinScanEntry;

typedef struct GinScanKeyData
{
	/* scanEntry[] 中条目的真实数目(恒 > 0) */
	uint32		nentries;
	/* extractQueryFn 与 consistentFn 所知的条目数 */
	uint32		nuserentries;

	/* GinScanEntry 指针数组，每个提取的搜索条件一个 */
	GinScanEntry *scanEntry;

	/*
	 * requiredEntries 中至少有一个条目必须存在，元组才
	 * 能匹配整体限定。
	 *
	 * additionalEntries 含 consistent 函数判定项是否匹配
	 * 所需的条目，但若无 requiredEntries 中的条目则
	 * 不足以单独满足限定。
	 */
	GinScanEntry *requiredEntries;
	int			nrequired;
	GinScanEntry *additionalEntries;
	int			nadditional;

	/* check 标志数组，报告给 consistentFn */
	GinTernaryValue *entryRes;
	bool		(*boolConsistentFn) (GinScanKey key);
	GinTernaryValue (*triConsistentFn) (GinScanKey key);
	FmgrInfo   *consistentFmgrInfo;
	FmgrInfo   *triConsistentFmgrInfo;
	Oid			collation;

	/* 调用 consistentFn 所需的其他数据 */
	Datum		query;
	/* NB: 这三个数组只有 nuserentries 个元素! */
	Datum	   *queryValues;
	GinNullCategory *queryCategories;
	Pointer    *extra_data;
	StrategyNumber strategy;
	int32		searchMode;
	OffsetNumber attnum;

	/*
	 * excludeOnly 扫描键无法枚举全部匹配元组。
	 * 也就是说，若要单独在语义上正确，它本应有一个
	 * GIN_CAT_EMPTY_QUERY scanEntry，但它没有。此类键仍可
	 * 用来过滤其他扫描键返回的元组，因此只要搜索所
	 * 考虑的每个索引属性至少有一个非 excludeOnly 扫描键，
	 * 我们就能得到正确结果。出于效率
	 * 考虑，我们不想有多余的 GIN_CAT_EMPTY_QUERY 条目，
	 * 故仅当无其他非 excludeOnly 扫描键时，才把
	 * excludeOnly 键转成非 excludeOnly(加 GIN_CAT_EMPTY_QUERY
	 * scanEntry)。
	 */
	bool		excludeOnly;

	/*
	 * 匹配状态数据。curItem 是最近测试的 TID(可能是
	 * 有损页指针)。若它通过 consistentFn 测试则
	 * curItemMatches 为真；recheckCurItem 即 recheck 标志。
	 * isFinished 表示全部输入条目流已结束，因此本
	 * 键对更晚的TIDs均无法成功。
	 */
	ItemPointerData curItem;
	bool		curItemMatches;
	bool		recheckCurItem;
	bool		isFinished;
}			GinScanKeyData;

typedef struct GinScanEntryData
{
	/* 查询键及 extractQueryFn 的其他信息 */
	Datum		queryKey;
	GinNullCategory queryCategory;
	bool		isPartialMatch;
	Pointer		extra_data;
	StrategyNumber strategy;
	int32		searchMode;
	OffsetNumber attnum;

	/* 倒排树中的当前页 */
	Buffer		buffer;

	/* 堆上当前 ItemPointer */
	ItemPointerData curItem;

	/* 对部分匹配或全扫描查询，我们在此累计全部 TIDs */
	TIDBitmap  *matchBitmap;
	TBMIterator *matchIterator;
	TBMIterateResult *matchResult;

	/* 用于倒排列表及倒排树中的一页 */
	ItemPointerData *list;
	int			nlist;
	OffsetNumber offset;

	bool		isFinished;
	bool		reduceResult;
	uint32		predictNumberResult;
	GinBtreeData btree;
}			GinScanEntryData;

typedef struct GinScanOpaqueData
{
	MemoryContext tempCtx;
	GinState	ginstate;

	GinScanKey	keys;			/* 每个扫描限定式一个 */
	uint32		nkeys;

	GinScanEntry *entries;		/* 每个索引搜索条件一个 */
	uint32		totalentries;
	uint32		allocentries;	/* entries[] 的已分配长度 */

	MemoryContext keyCtx;		/* 用于存放键与条目数据 */

	bool		isVoidRes;		/* 若查询不可满足则为真 */
} GinScanOpaqueData;

typedef GinScanOpaqueData *GinScanOpaque;

extern IndexScanDesc ginbeginscan(Relation rel, int nkeys, int norderbys);
extern void ginendscan(IndexScanDesc scan);
extern void ginrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					  ScanKey orderbys, int norderbys);
extern void ginNewScanKey(IndexScanDesc scan);
extern void ginFreeScanKeys(GinScanOpaque so);

/* ginget.c */
extern int64 gingetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/* ginlogic.c */
extern void ginInitConsistentFunction(GinState *ginstate, GinScanKey key);

/* ginvacuum.c */
extern IndexBulkDeleteResult *ginbulkdelete(IndexVacuumInfo *info,
											IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
											void *callback_state);
extern IndexBulkDeleteResult *ginvacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);
extern ItemPointer ginVacuumItemPointers(GinVacuumState *gvs,
										 ItemPointerData *items, int nitem, int *nremaining);

/* ginvalidate.c */
extern bool ginvalidate(Oid opclassoid);
extern void ginadjustmembers(Oid opfamilyoid,
							 Oid opclassoid,
							 List *operators,
							 List *functions);

/* ginbulk.c */
typedef struct GinEntryAccumulator
{
	RBTNode		rbtnode;
	Datum		key;
	GinNullCategory category;
	OffsetNumber attnum;
	bool		shouldSort;
	ItemPointerData *list;
	uint32		maxcount;		/* list[] 已分配大小 */
	uint32		count;			/* list[] 当前条目数 */
} GinEntryAccumulator;

typedef struct
{
	GinState   *ginstate;
	Size		allocatedMemory;
	GinEntryAccumulator *entryallocator;
	uint32		eas_used;
	RBTree	   *tree;
	RBTreeIterator tree_walk;
} BuildAccumulator;

extern void ginInitBA(BuildAccumulator *accum);
extern void ginInsertBAEntries(BuildAccumulator *accum,
							   ItemPointer heapptr, OffsetNumber attnum,
							   Datum *entries, GinNullCategory *categories,
							   int32 nentries);
extern void ginBeginBAScan(BuildAccumulator *accum);
extern ItemPointerData *ginGetBAEntry(BuildAccumulator *accum,
									  OffsetNumber *attnum, Datum *key, GinNullCategory *category,
									  uint32 *n);

/* ginfast.c */

typedef struct GinTupleCollector
{
	IndexTuple *tuples;
	uint32		ntuples;
	uint32		lentuples;
	uint32		sumsize;
} GinTupleCollector;

extern void ginHeapTupleFastInsert(GinState *ginstate,
								   GinTupleCollector *collector);
extern void ginHeapTupleFastCollect(GinState *ginstate,
									GinTupleCollector *collector,
									OffsetNumber attnum, Datum value, bool isNull,
									ItemPointer ht_ctid);
extern void ginInsertCleanup(GinState *ginstate, bool full_clean,
							 bool fill_fsm, bool forceCleanup, IndexBulkDeleteResult *stats);

/* ginpostinglist.c */

extern GinPostingList *ginCompressPostingList(const ItemPointer ipd, int nipd,
											  int maxsize, int *nwritten);
extern int	ginPostingListDecodeAllSegmentsToTbm(GinPostingList *ptr, int len, TIDBitmap *tbm);

extern ItemPointer ginPostingListDecodeAllSegments(GinPostingList *segment, int len,
												   int *ndecoded_out);
extern ItemPointer ginPostingListDecode(GinPostingList *plist, int *ndecoded_out);
extern ItemPointer ginMergeItemPointers(ItemPointerData *a, uint32 na,
										ItemPointerData *b, uint32 nb,
										int *nmerged);

/*
 * 合并多次 gin 扫描的结果会大量比较项指针，
 * 故希望此函数内联。
 */
static inline int
ginCompareItemPointers(ItemPointer a, ItemPointer b)
{
	uint64		ia = (uint64) GinItemPointerGetBlockNumber(a) << 32 | GinItemPointerGetOffsetNumber(a);
	uint64		ib = (uint64) GinItemPointerGetBlockNumber(b) << 32 | GinItemPointerGetOffsetNumber(b);

	if (ia == ib)
		return 0;
	else if (ia > ib)
		return 1;
	else
		return -1;
}

extern int	ginTraverseLock(Buffer buffer, bool searchMode);

#endif							/* GIN_PRIVATE_H */
