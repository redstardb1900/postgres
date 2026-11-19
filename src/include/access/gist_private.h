/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  GiST 的私有声明 -- 与内部实现相关的声明，
 *	  而非公共 API
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gist_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_PRIVATE_H
#define GIST_PRIVATE_H

#include "access/amapi.h"
#include "access/gist.h"
#include "access/itup.h"
#include "lib/pairingheap.h"
#include "storage/bufmgr.h"
#include "storage/buffile.h"
#include "utils/hsearch.h"
#include "access/genam.h"

/*
 * 一次操作中一页可被分裂成的"半页"最大数目。
 * 通常一次分裂产生 2 个半页，但若键长度差异很大，
 * 或一次操作插入多个键时（例如向内部节点插入
 * 下行指针）可更多。理论上对此没有上限，但
 * 实践中若一次分裂得到超过寥寥几个半页，
 * 则操作符类实现很可能有问题。
 * GIST_MAX_SPLIT_PAGES 是对此的任意上限，用于为
 * 分裂期间的若干局部数组定大小。注意还有对同时
 * 可持锁缓冲区数目的限制 MAX_SIMUL_LWLOCKS，
 * 因此若把本值抬得高于该限制，只会得到另一种
 * 错误
 */
#define GIST_MAX_SPLIT_PAGES		75

/* 缓冲锁模式 */
#define GIST_SHARE	BUFFER_LOCK_SHARE
#define GIST_EXCLUSIVE	BUFFER_LOCK_EXCLUSIVE
#define GIST_UNLOCK BUFFER_LOCK_UNLOCK

typedef struct
{
	BlockNumber prev;
	uint32		freespace;
	char		tupledata[FLEXIBLE_ARRAY_MEMBER];
} GISTNodeBufferPage;

#define BUFFER_PAGE_DATA_OFFSET MAXALIGN(offsetof(GISTNodeBufferPage, tupledata))
/* 返回节点缓冲页空闲空间 */
#define PAGE_FREE_SPACE(nbp) (nbp->freespace)
/* 检查节点缓冲页是否为空 */
#define PAGE_IS_EMPTY(nbp) (nbp->freespace == BLCKSZ - BUFFER_PAGE_DATA_OFFSET)
/* 检查节点缓冲页是否没有足够空间容纳索引元组 */
#define PAGE_NO_SPACE(nbp, itup) (PAGE_FREE_SPACE(nbp) < \
										MAXALIGN(IndexTupleSize(itup)))

/*
 * GISTSTATE: 任何 GiST 索引操作所需的信息
 *
 * 本结构保留索引操作符类专用支持函数的调用信息
 * （按索引列），以及索引的元组描述符。
 *
 * scanCxt 持有 GISTSTATE 本身以及存活于整个索引
 * 操作寿命期的任何数据。我们经 fn_mcxt 把它传给
 * 支持函数，以便它们可在其中存放扫描寿命数据。
 * 函数在 tempCxt 中调用，该上下文通常是短寿命的
 * （即每个元组之后复位）。不过若不做逐元组上下文
 * 复位，tempCxt 也可与 scanCxt 相同。
 */
typedef struct GISTSTATE
{
	MemoryContext scanCxt;		/* 扫描寿命数据的上下文 */
	MemoryContext tempCxt;		/* 调用函数用的短期上下文 */

	TupleDesc	leafTupdesc;	/* 索引的元组描述符 */
	TupleDesc	nonLeafTupdesc; /* 截断元组描述符,用于非叶子
								 * 页*/
	TupleDesc	fetchTupdesc;	/* 仅索引扫描所返回元组的
								 * 元组描述符*/

	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	unionFn[INDEX_MAX_KEYS];
	FmgrInfo	compressFn[INDEX_MAX_KEYS];
	FmgrInfo	decompressFn[INDEX_MAX_KEYS];
	FmgrInfo	penaltyFn[INDEX_MAX_KEYS];
	FmgrInfo	picksplitFn[INDEX_MAX_KEYS];
	FmgrInfo	equalFn[INDEX_MAX_KEYS];
	FmgrInfo	distanceFn[INDEX_MAX_KEYS];
	FmgrInfo	fetchFn[INDEX_MAX_KEYS];

	/* 传给支持函数的排序规则 */
	Oid			supportCollation[INDEX_MAX_KEYS];
} GISTSTATE;


/*
 * 在 GiST 索引搜索期间，我们必须维护未访问项队列，
 * 其可为单个堆元组或整页索引页。若是有序搜索，
 * 未访问项应按距离次序访问。相同距离的未访问项
 * 应按深度优先次序访问，即先堆项，再较低索引
 * 页，然后较高索引页；该规则避免在因 LIMIT 而
 * 提早结束的搜索中做多余
 * 工作。
 *
 * 为执行有序搜索，我们用配对堆管理按距离排序
 * 的队列。在非有序搜索中（无 order-by 操作符），
 * 用它在未访问索引页前返回堆元组，以确保深度
 * 优先次序，但其余方面所有条目都被视为
 * 相等
 */

/* 待访问的单个堆元组 */
typedef struct GISTSearchHeapItem
{
	ItemPointerData heapPtr;
	bool		recheck;		/* 若限定须重检则为 T */
	bool		recheckDistances;	/* 若距离须重检则为 T */
	HeapTuple	recontup;		/* 从索引重构的数据，用于
								 * 仅索引扫描*/
	OffsetNumber offnum;		/* 跟踪页内偏移以将元组标为
								 * LP_DEAD */
} GISTSearchHeapItem;

/* 未访问项，或为索引页或为堆元组 */
typedef struct GISTSearchItem
{
	pairingheap_node phNode;
	BlockNumber blkno;			/* 索引页号，或 InvalidBlockNumber */
	union
	{
		GistNSN		parentlsn;	/* 父页的 LSN，若为索引页 */
		/* 必须保存 parentlsn 以检测是否发生了分裂 */
		GISTSearchHeapItem heap;	/* 堆信息,若堆元组 */
	}			data;

	/* numberOfOrderBys 条目 */
	IndexOrderByDistance distances[FLEXIBLE_ARRAY_MEMBER];
} GISTSearchItem;

#define GISTSearchItemIsHeap(item)	((item).blkno == InvalidBlockNumber)

#define SizeOfGISTSearchItem(n_distances) \
	(offsetof(GISTSearchItem, distances) + \
	 sizeof(IndexOrderByDistance) * (n_distances))

/*
 * GISTScanOpaqueData: GiST 索引扫描的私有状态
 */
typedef struct GISTScanOpaqueData
{
	GISTSTATE  *giststate;		/* 索引信息，见上文 */
	Oid		   *orderByTypes;	/* ORDER BY 表达式的数据类型 */

	pairingheap *queue;			/* 未访问项的队列 */
	MemoryContext queueCxt;		/* 持有队列的上下文 */
	bool		qual_ok;		/* 若限定永无法满足则为假 */
	bool		firstCall;		/* 首次 gistgettuple 前为真 */

	/* 预分配的工作区数组 */
	IndexOrderByDistance *distances;	/* gistindex_keytest 的输出区 */

	/* 已杀项信息(有则;未用则 killedItems 为 NULL) */
	OffsetNumber *killedItems;	/* 已杀项的偏移号 */
	int			numKilled;		/* 当前已存项的数目 */
	BlockNumber curBlkno;		/* 当前块号 */
	GistNSN		curPageLSN;		/* 读页时在 WAL 流中的位置 */

	/* 非有序搜索中，可返回的堆项存放于此： */
	GISTSearchHeapItem pageData[BLCKSZ / sizeof(IndexTupleData)];
	OffsetNumber nPageData;		/* 数组中有效项的数目 */
	OffsetNumber curPageData;	/* 下一待返回项 */
	MemoryContext pageDataCxt;	/* 持有取回元组的上下文,用于
								 * 仅索引扫描*/
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;

/* 尽管名称如此，gistxlogPage 不属于任何 xlog 记录 */
typedef struct gistxlogPage
{
	BlockNumber blkno;
	int			num;			/* 其后索引元组的数目 */
} gistxlogPage;

/* SplitedPageLayout - gistSplit 函数的结果 */
typedef struct SplitedPageLayout
{
	gistxlogPage block;
	IndexTupleData *list;
	int			lenlist;
	IndexTuple	itup;			/* 页的联合键 */
	Page		page;			/* 供操作 */
	Buffer		buffer;			/* 全部处理完后写入 */

	struct SplitedPageLayout *next;
} SplitedPageLayout;

/*
 * GISTInsertStack 用于在插入期间锁定缓冲区并传递
 * 参数
 */
typedef struct GISTInsertStack
{
	/* 当前页 */
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	/*
	 * 取自 page->lsn 的日志序号，用于识别页更新，并与
	 * 页的 nsn 比较以识别页分裂
	 */
	GistNSN		lsn;

	/*
	 * 若置位，表示我们在沿树下行寻找插入目标时
	 * 分裂了该页。意味着需要从父重试，因为
	 * 该页的下行指针可能已不再覆盖新键。
	 */
	bool		retry_from_parent;

	/* 父页中指向本页的下行指针偏移 */
	OffsetNumber downlinkoffnum;

	/* 指向父指针 */
	struct GISTInsertStack *parent;
} GISTInsertStack;

/* gistsplit.c 中多列分裂逻辑的工作状态与结果 */
typedef struct GistSplitVector
{
	GIST_SPLITVEC splitVector;	/* 传入/传出用户 PickSplit 方法 */

	Datum		spl_lattr[INDEX_MAX_KEYS];	/* 子键联合,位于
											 * splitVector.spl_left */
	bool		spl_lisnull[INDEX_MAX_KEYS];

	Datum		spl_rattr[INDEX_MAX_KEYS];	/* 子键联合,位于
											 * splitVector.spl_right */
	bool		spl_risnull[INDEX_MAX_KEYS];

	bool	   *spl_dontcare;	/* 标记可零惩罚放到分裂任一侧之
								 * 无所谓元组*/
} GistSplitVector;

typedef struct
{
	Relation	r;
	Relation	heapRel;
	Size		freespace;		/* 须留的空闲空间 */
	bool		is_build;

	GISTInsertStack *stack;
} GISTInsertState;

/* gist 索引的根页 */
#define GIST_ROOT_BLKNO				0

/*
 * PostgreSQL 9.1 前，我们曾依赖内部页上所谓"无效元组"
 * 来完成未完成页分裂的崩溃恢复。若崩溃发生在页分裂
 * 中途以致下行指针尚未插入，崩溃恢复会插入特殊的
 * 下行指针。无效元组的语义是：扫描中若遇到
 * 一个，则必须始终跟随，因为我们不知道子页上
 * 的元组是否
 * 匹配。
 *
 * 我们不再创建此类无效元组，改为用 F_FOLLOW_RIGHT
 * 标志标记此类未完成分裂的左半，并在下次需要向该页
 * 插入时正确完成分裂。为保留盘上兼容性以利
 * pg_upgrade，我们仍把所有内部元组的偏移号存为
 * 0xffff。若在插入时遇到带 0xfffe 的任何无效元组，
 * 我们抛错，不过扫描仍会处理它们。只有当你用
 * pg_upgrade 升级一个因崩溃已含无效元组的 pre-9.1
 * gist 索引时，才应遇到无效元组。那应很少见，且若
 * 索引中有任何无效元组，仍建议 REINDEX，因此抛错已是
 * 我们对此支持的
 * 极限。
 */
#define TUPLE_IS_VALID		0xffff
#define TUPLE_IS_INVALID	0xfffe

#define  GistTupleIsInvalid(itup)	( ItemPointerGetOffsetNumber( &((itup)->t_tid) ) == TUPLE_IS_INVALID )
#define  GistTupleSetValid(itup)	ItemPointerSetOffsetNumber( &((itup)->t_tid), TUPLE_IS_VALID )




/*
 * 附着于内部节点的缓冲区，用于在缓冲模式下
 * 构建索引。
 */
typedef struct
{
	BlockNumber nodeBlocknum;	/* 本缓冲对应的索引块号 */
	int32		blocksCount;	/* 缓冲区当前占用的块数 */

	BlockNumber pageBlocknum;	/* 临时文件块号 */
	GISTNodeBufferPage *pageBuffer; /* 内存中的缓冲页 */

	/* 此缓冲是否已排队待排空? */
	bool		queuedForEmptying;

	/* 这是否为不在哈希表中的临时副本？ */
	bool		isTemp;

	int			level;			/* 0==叶子 */
} GISTNodeBuffer;

/*
 * 指定层是否有缓冲区?(当心参数被多次
 * 求值.)
 */
#define LEVEL_HAS_BUFFERS(nlevel, gfbb) \
	((nlevel) != 0 && (nlevel) % (gfbb)->levelStep == 0 && \
	 (nlevel) != (gfbb)->rootlevel)

/* 指定缓冲区是否至少半满（应排队等待排空）？ */
#define BUFFER_HALF_FILLED(nodeBuffer, gfbb) \
	((nodeBuffer)->blocksCount > (gfbb)->pagesPerBuffer / 2)

/*
 * 指定缓冲是否已满？缓冲实际可无限增长，
 * 超出"最大"大小，故这只表示缓冲区是否已增长到
 * 超过名义最大大小。
 */
#define BUFFER_OVERFLOWED(nodeBuffer, gfbb) \
	((nodeBuffer)->blocksCount > (gfbb)->pagesPerBuffer)

/*
 * 含构建缓冲区一般信息的数据结构。
 */
typedef struct GISTBuildBuffers
{
	/* 缓冲区与元数据的持久内存上下文。 */
	MemoryContext context;

	BufFile    *pfile;			/* 存放缓冲区的临时文件 */
	long		nFileBlocks;	/* 临时文件的当前大小 */

	/*
	 * 空闲块的可调数组。
	 */
	long	   *freeBlocks;
	int			nFreeBlocks;	/* 数组中当前空闲块数 */
	int			freeBlocksLen;	/* 数组当前已分配长度 */

	/* 按块号缓冲的哈希 */
	HTAB	   *nodeBuffersTab;

	/* 已安排排空的缓冲区列表 */
	List	   *bufferEmptyingQueue;

	/*
	 * 缓冲构建算法的参数。levelStep 决定树中哪些层
	 * 有缓冲区，pagesPerBuffer 决定每个缓冲区有
	 * 多大。
	 */
	int			levelStep;
	int			pagesPerBuffer;

	/* 每层缓冲区列表的数组，用于最终排空 */
	List	  **buffersOnLevels;
	int			buffersOnLevelsLen;

	/*
	 * 动态大小的缓冲区数组，其最后一页当前已装入
	 * 主存。
	 */
	GISTNodeBuffer **loadedBuffers;
	int			loadedBuffersCount; /* loadedBuffers 中的条目数 */
	int			loadedBuffersLen;	/* loadedBuffers 已分配大小 */

	/* 当前根节点的层（= 索引树高度 - 1） */
	int			rootlevel;
} GISTBuildBuffers;

/* GiSTOptions->buffering_mode 取值 */
typedef enum GistOptBufferingMode
{
	GIST_OPTION_BUFFERING_AUTO,
	GIST_OPTION_BUFFERING_ON,
	GIST_OPTION_BUFFERING_OFF
} GistOptBufferingMode;

/*
 * GiST 关系选项的存储类型
 */
typedef struct GiSTOptions
{
	int32		vl_len_;		/* varlena 头(勿直接碰!) */
	int			fillfactor;		/* 页填充因子，百分比 (0..100) */
	GistOptBufferingMode buffering_mode;	/* 缓冲构建模式 */
} GiSTOptions;

/* gist.c */
extern void gistbuildempty(Relation index);
extern bool gistinsert(Relation r, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
extern MemoryContext createTempGistContext(void);
extern GISTSTATE *initGISTstate(Relation index);
extern void freeGISTstate(GISTSTATE *giststate);
extern void gistdoinsert(Relation r,
						 IndexTuple itup,
						 Size freespace,
						 GISTSTATE *giststate,
						 Relation heapRel,
						 bool is_build);

/* gistplacetopage() 在 *splitinfo 中返回这些的 List */
typedef struct
{
	Buffer		buf;			/* 分裂页的"半页" */
	IndexTuple	downlink;		/* 此半的下行指针. */
} GISTPageSplitInfo;

extern bool gistplacetopage(Relation rel, Size freespace, GISTSTATE *giststate,
							Buffer buffer,
							IndexTuple *itup, int ntup,
							OffsetNumber oldoffnum, BlockNumber *newblkno,
							Buffer leftchildbuf,
							List **splitinfo,
							bool markfollowright,
							Relation heapRel,
							bool is_build);

extern SplitedPageLayout *gistSplit(Relation r, Page page, IndexTuple *itup,
									int len, GISTSTATE *giststate);

/* gistxlog.c */
extern XLogRecPtr gistXLogPageDelete(Buffer buffer,
									 FullTransactionId xid, Buffer parentBuffer,
									 OffsetNumber downlinkOffset);

extern void gistXLogPageReuse(Relation rel, Relation heaprel, BlockNumber blkno,
							  FullTransactionId deleteXid);

extern XLogRecPtr gistXLogUpdate(Buffer buffer,
								 OffsetNumber *todelete, int ntodelete,
								 IndexTuple *itup, int ituplen,
								 Buffer leftchildbuf);

extern XLogRecPtr gistXLogDelete(Buffer buffer, OffsetNumber *todelete,
								 int ntodelete, TransactionId snapshotConflictHorizon,
								 Relation heaprel);

extern XLogRecPtr gistXLogSplit(bool page_is_leaf,
								SplitedPageLayout *dist,
								BlockNumber origrlink, GistNSN orignsn,
								Buffer leftchildbuf, bool markfollowright);

extern XLogRecPtr gistXLogAssignLSN(void);

/* gistget.c */
extern bool gistgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 gistgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool gistcanreturn(Relation index, int attno);

/* gistvalidate.c */
extern bool gistvalidate(Oid opclassoid);
extern void gistadjustmembers(Oid opfamilyoid,
							  Oid opclassoid,
							  List *operators,
							  List *functions);

/* gistutil.c */

#define GiSTPageSize   \
	( BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(GISTPageOpaqueData)) )

#define GIST_MIN_FILLFACTOR			10
#define GIST_DEFAULT_FILLFACTOR		90

extern bytea *gistoptions(Datum reloptions, bool validate);
extern bool gistproperty(Oid index_oid, int attno,
						 IndexAMProperty prop, const char *propname,
						 bool *res, bool *isnull);
extern bool gistfitpage(IndexTuple *itvec, int len);
extern bool gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete, Size freespace);
extern void gistcheckpage(Relation rel, Buffer buf);
extern Buffer gistNewBuffer(Relation r, Relation heaprel);
extern bool gistPageRecyclable(Page page);
extern void gistfillbuffer(Page page, IndexTuple *itup, int len,
						   OffsetNumber off);
extern IndexTuple *gistextractpage(Page page, int *len /* out */ );
extern IndexTuple *gistjoinvector(IndexTuple *itvec, int *len,
								  IndexTuple *additvec, int addlen);
extern IndexTupleData *gistfillitupvec(IndexTuple *vec, int veclen, int *memlen);

extern IndexTuple gistunion(Relation r, IndexTuple *itvec,
							int len, GISTSTATE *giststate);
extern IndexTuple gistgetadjusted(Relation r,
								  IndexTuple oldtup,
								  IndexTuple addtup,
								  GISTSTATE *giststate);
extern IndexTuple gistFormTuple(GISTSTATE *giststate,
								Relation r, Datum *attdata, bool *isnull, bool isleaf);
extern void gistCompressValues(GISTSTATE *giststate, Relation r,
							   Datum *attdata, bool *isnull, bool isleaf, Datum *compatt);

extern OffsetNumber gistchoose(Relation r, Page p,
							   IndexTuple it,
							   GISTSTATE *giststate);

extern void GISTInitBuffer(Buffer b, uint32 f);
extern void gistinitpage(Page page, uint32 f);
extern void gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
						   Datum k, Relation r, Page pg, OffsetNumber o,
						   bool l, bool isNull);

extern float gistpenalty(GISTSTATE *giststate, int attno,
						 GISTENTRY *orig, bool isNullOrig,
						 GISTENTRY *add, bool isNullAdd);
extern void gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
							   Datum *attr, bool *isnull);
extern bool gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b);
extern void gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
							  OffsetNumber o, GISTENTRY *attdata, bool *isnull);
extern HeapTuple gistFetchTuple(GISTSTATE *giststate, Relation r,
								IndexTuple tuple);
extern void gistMakeUnionKey(GISTSTATE *giststate, int attno,
							 GISTENTRY *entry1, bool isnull1,
							 GISTENTRY *entry2, bool isnull2,
							 Datum *dst, bool *dstisnull);

extern XLogRecPtr gistGetFakeLSN(Relation rel);

/* gistvacuum.c */
extern IndexBulkDeleteResult *gistbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);
extern IndexBulkDeleteResult *gistvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);

/* gistsplit.c */
extern void gistSplitByKey(Relation r, Page page, IndexTuple *itup,
						   int len, GISTSTATE *giststate,
						   GistSplitVector *v,
						   int attno);

/* gistbuild.c */
extern IndexBuildResult *gistbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);

/* gistbuildbuffers.c */
extern GISTBuildBuffers *gistInitBuildBuffers(int pagesPerBuffer, int levelStep,
											  int maxLevel);
extern GISTNodeBuffer *gistGetNodeBuffer(GISTBuildBuffers *gfbb,
										 GISTSTATE *giststate,
										 BlockNumber nodeBlocknum, int level);
extern void gistPushItupToNodeBuffer(GISTBuildBuffers *gfbb,
									 GISTNodeBuffer *nodeBuffer, IndexTuple itup);
extern bool gistPopItupFromNodeBuffer(GISTBuildBuffers *gfbb,
									  GISTNodeBuffer *nodeBuffer, IndexTuple *itup);
extern void gistFreeBuildBuffers(GISTBuildBuffers *gfbb);
extern void gistRelocateBuildBuffersOnSplit(GISTBuildBuffers *gfbb,
											GISTSTATE *giststate, Relation r,
											int level, Buffer buffer,
											List *splitinfo);
extern void gistUnloadNodeBuffers(GISTBuildBuffers *gfbb);

#endif							/* GIST_PRIVATE_H */
