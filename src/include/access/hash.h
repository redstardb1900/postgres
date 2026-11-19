/*-------------------------------------------------------------------------
 *
 * hash.h
 *	  postgres hash 访问方法实现的头文件
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hash.h
 *
 * NOTES
 *		仿自 Margo Seltzer 的 unix hash 实现。
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_H
#define HASH_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "catalog/pg_am_d.h"
#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"

/*
 * 由 hash 桶号映射到该桶起始页的物理块号。
 * 当心参数被多次求值！
 */
typedef uint32 Bucket;

#define InvalidBucket	((Bucket) 0xFFFFFFFF)

#define BUCKET_TO_BLKNO(metap,B) \
		((BlockNumber) ((B) + ((B) ? (metap)->hashm_spares[_hash_spareindex((B)+1)-1] : 0)) + 1)

/*
 * hash 索引页的特殊空间。
 *
 * hasho_flag 的 LH_PAGE_TYPE 位告诉我们正看的页类型。
 * 标志字中的其余位用于更短暂的用途。
 *
 * 测试页类型：做 (hasho_flag & LH_PAGE_TYPE) == LH_xxx_PAGE。
 * 不过我们保证每种已用页类型有一独立位，以便
 * 可将页类型 OR 在一起，例如用作 _hash_checkpage() 的
 * 可允许页类型参数。
 */
#define LH_UNUSED_PAGE			(0)
#define LH_OVERFLOW_PAGE		(1 << 0)
#define LH_BUCKET_PAGE			(1 << 1)
#define LH_BITMAP_PAGE			(1 << 2)
#define LH_META_PAGE			(1 << 3)
#define LH_BUCKET_BEING_POPULATED	(1 << 4)
#define LH_BUCKET_BEING_SPLIT	(1 << 5)
#define LH_BUCKET_NEEDS_SPLIT_CLEANUP	(1 << 6)
#define LH_PAGE_HAS_DEAD_TUPLES (1 << 7)

#define LH_PAGE_TYPE \
	(LH_OVERFLOW_PAGE | LH_BUCKET_PAGE | LH_BITMAP_PAGE | LH_META_PAGE)

/*
 * 在溢出页中，hasho_prevblkno 存放桶链中上一页的块号；
 * 在桶页中，hasho_prevblkno 存放该桶上次分裂时的
 * hashm_maxbucket 值，或者
 * 桶创建之时的该值。后一约定用来
 * 判定元页的缓存副本是否过旧而不能使用
 * (判定时无需对元页加锁或钉住)。
 *
 * hasho_nextblkno 恒为桶链中下一页的块号，
 * 若再无此类页则为 InvalidBlockNumber。
 */
typedef struct HashPageOpaqueData
{
	BlockNumber hasho_prevblkno;	/* 见上文 */
	BlockNumber hasho_nextblkno;	/* 见上文 */
	Bucket		hasho_bucket;	/* 本页所属的桶号 */
	uint16		hasho_flag;		/* 页类型码 + 标志位，见上文 */
	uint16		hasho_page_id;	/* 用于识别 hash 索引 */
} HashPageOpaqueData;

typedef HashPageOpaqueData *HashPageOpaque;

#define HashPageGetOpaque(page) ((HashPageOpaque) PageGetSpecialPointer(page))

#define H_NEEDS_SPLIT_CLEANUP(opaque)	(((opaque)->hasho_flag & LH_BUCKET_NEEDS_SPLIT_CLEANUP) != 0)
#define H_BUCKET_BEING_SPLIT(opaque)	(((opaque)->hasho_flag & LH_BUCKET_BEING_SPLIT) != 0)
#define H_BUCKET_BEING_POPULATED(opaque)	(((opaque)->hasho_flag & LH_BUCKET_BEING_POPULATED) != 0)
#define H_HAS_DEAD_TUPLES(opaque)		(((opaque)->hasho_flag & LH_PAGE_HAS_DEAD_TUPLES) != 0)

/*
 * 页 ID 是为方便 pg_filedump 及类似工具，
 * 否则它们很难区分不同索引类型的
 * 页。它应是页上最后 2 字节。这因对齐考
 * 虑而或多或少是"免费"的。
 */
#define HASHO_PAGE_ID		0xFF80

typedef struct HashScanPosItem	/* 每次匹配所记住的内容 */
{
	ItemPointerData heapTid;	/* 所引用堆项的 TID */
	OffsetNumber indexOffset;	/* 索引项在页内的位置 */
} HashScanPosItem;

typedef struct HashScanPosData
{
	Buffer		buf;			/* 若有效则缓冲区已钉住 */
	BlockNumber currPage;		/* 当前 hash 索引页 */
	BlockNumber nextPage;		/* 下一溢出页 */
	BlockNumber prevPage;		/* 上一溢出页或桶页 */

	/*
	 * items 数组恒按索引序排列（即 indexoffset
	 * 递增）。反向扫描时便于自后向前填充，
	 * 故从最后一个槽位起向下填。
	 * 因此需要首个有效条目与末个有效条目两个计数。
	 * itemIndex 是游标，标出上次返回给调用者的条目。
	 */
	int			firstItem;		/* items[]首个有效下标 */
	int			lastItem;		/* items[]末个有效下标 */
	int			itemIndex;		/* items[]当前下标 */

	HashScanPosItem items[MaxIndexTuplesPerPage];	/* 必须最后 */
} HashScanPosData;

#define HashScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)

#define HashScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)

#define HashScanPosInvalidate(scanpos) \
	do { \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).prevPage = InvalidBlockNumber; \
		(scanpos).firstItem = 0; \
		(scanpos).lastItem = 0; \
		(scanpos).itemIndex = 0; \
	} while (0)

/*
 *	HashScanOpaqueData 是 hash 索引扫描的私有状态。
 */
typedef struct HashScanOpaqueData
{
	/* 扫描键的 hash 值，即我们查找的 hash 键 */
	uint32		hashso_sk_hash;

	/* 记住与主桶关联的缓冲区 */
	Buffer		hashso_bucket_buf;

	/*
	 * 记住与正被分裂之桶的主桶页关联的缓冲区。
	 * 在扫描分裂操作期间正被填充的那个桶时
	 * 需要它。
	 */
	Buffer		hashso_split_bucket_buf;

	/* 扫描是否始于因分裂而正被填充的桶 */
	bool		hashso_buc_populated;

	/*
	 * 是否正在扫描被分裂的桶？仅当 hashso_buc_populated
	 * 为真时才参考此参数的值。
	 */
	bool		hashso_buc_split;
	/* 已杀项信息(killedItems未用则为NULL) */
	int		   *killedItems;	/* currPos.items中已杀项下标 */
	int			numKilled;		/* 当前已存项的数目 */

	/*
	 * 找出页上全部匹配项并保存到
	 * HashScanPosData
	 */
	HashScanPosData currPos;	/* 当前位置数据 */
} HashScanOpaqueData;

typedef HashScanOpaqueData *HashScanOpaque;

/*
 * 元页的定义。
 */

#define HASH_METAPAGE	0		/* 元页恒为块 0 */

#define HASH_MAGIC		0x6440640
#define HASH_VERSION	4

/*
 * spares[]保存某分裂点及之前已分配的溢出页数。
 * 例如若 spares[3] = 7，则分裂点 3 之前有
 * 7 个溢出页（对照 BUCKET_TO_BLKNO 宏）。
 * spares[ovflpoint] 的值随索引末尾添加溢出页而
 * 增大。一旦 ovflpoint 增大（即我们已实际分配了
 * 属于该分裂点的桶页），先前分裂点处的备用数便
 * 无法再改变。
 *
 * 已回收待复用的溢出页，可通过查看
 * 存放在专用于此目的的溢出页内的位图找到。
 * 这些位图页的块号保存在 mapp[]；nmaps 是
 * 当前已有位图的数目。
 *
 * spares[] 大小受限，是因为仅有 uint32 散列码时
 * 拥有超过 2^32 个桶并无意义。
 * （注：HASH_MAX_SPLITPOINTS 即 spares[] 的大小，
 * 经调整以容纳 HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE
 * 之后的多阶段桶分配。）
 *
 * mapp[] 大小并无特定上限，除了
 * 须能放入元页。（8K 块大小下，1024 个位图
 * 把我们限在 256 GB 溢出空间…）。对更小块大小我们
 * 无法使用 1024 个位图，否则元页数据会越过
 * 块大小边界。故用 BLCKSZ 决定位图的最大
 * 数目。
 */
#define HASH_MAX_BITMAPS			Min(BLCKSZ / 8, 1024)

#define HASH_SPLITPOINT_PHASE_BITS	2
#define HASH_SPLITPOINT_PHASES_PER_GRP	(1 << HASH_SPLITPOINT_PHASE_BITS)
#define HASH_SPLITPOINT_PHASE_MASK		(HASH_SPLITPOINT_PHASES_PER_GRP - 1)
#define HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE	10

/* 定义 hash 索引可有的最大分裂点阶段数 */
#define HASH_MAX_SPLITPOINT_GROUP	32
#define HASH_MAX_SPLITPOINTS \
	(((HASH_MAX_SPLITPOINT_GROUP - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) * \
	  HASH_SPLITPOINT_PHASES_PER_GRP) + \
	 HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE)

typedef struct HashMetaPageData
{
	uint32		hashm_magic;	/* hash 表的魔数 */
	uint32		hashm_version;	/* 版本 ID */
	double		hashm_ntuples;	/* 表中存储的元组数 */
	uint16		hashm_ffactor;	/* 目标填充因子（元组/桶） */
	uint16		hashm_bsize;	/* 索引页大小(字节) */
	uint16		hashm_bmsize;	/* 位图数组大小(字节)-须为
								 * 2幂*/
	uint16		hashm_bmshift;	/* log2(位图数组BITS) */
	uint32		hashm_maxbucket;	/* 在用最大桶的 ID */
	uint32		hashm_highmask; /* 对整表取模的掩码 */
	uint32		hashm_lowmask;	/* 对表下半部取模的掩码 */
	uint32		hashm_ovflpoint;	/* 正分配溢出页所自之分裂点
									 * */
	uint32		hashm_firstfree;	/* 最低号空闲溢页(bit#) */
	uint32		hashm_nmaps;	/* 位图页数目 */
	RegProcedure hashm_procid;	/* pg_proc中hash函数id */
	uint32		hashm_spares[HASH_MAX_SPLITPOINTS]; /* 各分裂点前备用
													 * 页*/
	BlockNumber hashm_mapp[HASH_MAX_BITMAPS];	/* 溢出位图的块号 */
} HashMetaPageData;

typedef HashMetaPageData *HashMetaPage;

typedef struct HashOptions
{
	int32		varlena_header_;	/* varlena 头（勿直接触碰！） */
	int			fillfactor;		/* 页填充因子百分数(0..100) */
} HashOptions;

#define HashGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == HASH_AM_OID), \
	 (relation)->rd_options ? \
	 ((HashOptions *) (relation)->rd_options)->fillfactor :	\
	 HASH_DEFAULT_FILLFACTOR)
#define HashGetTargetPageUsage(relation) \
	(BLCKSZ * HashGetFillFactor(relation) / 100)

/*
 * hash 索引项的最大尺寸（每页仅一项亦可）
 */
#define HashMaxItemSize(page) \
	MAXALIGN_DOWN(PageGetPageSize(page) - \
				  SizeOfPageHeaderData - \
				  sizeof(ItemIdData) - \
				  MAXALIGN(sizeof(HashPageOpaqueData)))

#define INDEX_MOVED_BY_SPLIT_MASK	INDEX_AM_RESERVED_BIT

#define HASH_MIN_FILLFACTOR			10
#define HASH_DEFAULT_FILLFACTOR		75

/*
 * 常量
 */
#define BYTE_TO_BIT				3	/* 2^3位/字节 */
#define ALL_SET					((uint32) ~0)

/*
 * 位图页不含元组。它们确含标准的
 * 页头与页尾；但二者之间全是一个
 * 巨大位数组。一页能放下的位数显然
 * 取决于页大小及头/尾开销。我们要求
 * 每页位数须为 2 的幂。
 */
#define BMPGSZ_BYTE(metap)		((metap)->hashm_bmsize)
#define BMPGSZ_BIT(metap)		((metap)->hashm_bmsize << BYTE_TO_BIT)
#define BMPG_SHIFT(metap)		((metap)->hashm_bmshift)
#define BMPG_MASK(metap)		(BMPGSZ_BIT(metap) - 1)

#define HashPageGetBitmap(page) \
	((uint32 *) PageGetContents(page))

#define HashGetMaxBitmapSize(page) \
	(PageGetPageSize((Page) page) - \
	 (MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(HashPageOpaqueData))))

#define HashPageGetMeta(page) \
	((HashMetaPage) PageGetContents(page))

/*
 * 溢出页位图字中的位数。
 */
#define BITS_PER_MAP	32		/* uint32 中的位数 */

/* 给定位图起始地址，清除/设置第 n 位 */
#define CLRBIT(A, N)	((A)[(N)/BITS_PER_MAP] &= ~(1<<((N)%BITS_PER_MAP)))
#define SETBIT(A, N)	((A)[(N)/BITS_PER_MAP] |= (1<<((N)%BITS_PER_MAP)))
#define ISSET(A, N)		((A)[(N)/BITS_PER_MAP] & (1<<((N)%BITS_PER_MAP)))

/*
 * 页级与高层加锁模式（见 README）
 */
#define HASH_READ		BUFFER_LOCK_SHARE
#define HASH_WRITE		BUFFER_LOCK_EXCLUSIVE
#define HASH_NOLOCK		(-1)

/*
 * 声明新操作符类时，我们要求用户向我们提供
 * 一个 amproc 函数，用于散列新类型的键并返回
 * 32 位 hash 值。我们称此为"标准" hash 函数。我们
 * 也允许可选的"扩展" hash 函数，它接受盐并
 * 返回 64 位 hash 值。强烈推荐，但出于向后
 * 兼容原因，可选。
 *
 * 当盐为 0 时，扩展 hash 函数返回值的低 32 位
 * 应与标准 hash 函数本会返回的值
 * 相符。
 */
#define HASHSTANDARD_PROC		1
#define HASHEXTENDED_PROC		2
#define HASHOPTIONS_PROC		3
#define HASHNProcs				3


/* 公开例程 */

extern IndexBuildResult *hashbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);
extern void hashbuildempty(Relation index);
extern bool hashinsert(Relation rel, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
extern bool hashgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 hashgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern IndexScanDesc hashbeginscan(Relation rel, int nkeys, int norderbys);
extern void hashrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);
extern void hashendscan(IndexScanDesc scan);
extern IndexBulkDeleteResult *hashbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);
extern IndexBulkDeleteResult *hashvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);
extern bytea *hashoptions(Datum reloptions, bool validate);
extern bool hashvalidate(Oid opclassoid);
extern void hashadjustmembers(Oid opfamilyoid,
							  Oid opclassoid,
							  List *operators,
							  List *functions);

/* 私有例程 */

/* hashinsert.c */
extern void _hash_doinsert(Relation rel, IndexTuple itup, Relation heapRel,
						   bool sorted);
extern OffsetNumber _hash_pgaddtup(Relation rel, Buffer buf,
								   Size itemsize, IndexTuple itup,
								   bool appendtup);
extern void _hash_pgaddmultitup(Relation rel, Buffer buf, IndexTuple *itups,
								OffsetNumber *itup_offsets, uint16 nitups);

/* hashovfl.c */
extern Buffer _hash_addovflpage(Relation rel, Buffer metabuf, Buffer buf, bool retain_pin);
extern BlockNumber _hash_freeovflpage(Relation rel, Buffer bucketbuf, Buffer ovflbuf,
									  Buffer wbuf, IndexTuple *itups, OffsetNumber *itup_offsets,
									  Size *tups_size, uint16 nitups, BufferAccessStrategy bstrategy);
extern void _hash_initbitmapbuffer(Buffer buf, uint16 bmsize, bool initpage);
extern void _hash_squeezebucket(Relation rel,
								Bucket bucket, BlockNumber bucket_blkno,
								Buffer bucket_buf,
								BufferAccessStrategy bstrategy);
extern uint32 _hash_ovflblkno_to_bitno(HashMetaPage metap, BlockNumber ovflblkno);

/* hashpage.c */
extern Buffer _hash_getbuf(Relation rel, BlockNumber blkno,
						   int access, int flags);
extern Buffer _hash_getbuf_with_condlock_cleanup(Relation rel,
												 BlockNumber blkno, int flags);
extern HashMetaPage _hash_getcachedmetap(Relation rel, Buffer *metabuf,
										 bool force_refresh);
extern Buffer _hash_getbucketbuf_from_hashkey(Relation rel, uint32 hashkey,
											  int access,
											  HashMetaPage *cachedmetap);
extern Buffer _hash_getinitbuf(Relation rel, BlockNumber blkno);
extern void _hash_initbuf(Buffer buf, uint32 max_bucket, uint32 num_bucket,
						  uint32 flag, bool initpage);
extern Buffer _hash_getnewbuf(Relation rel, BlockNumber blkno,
							  ForkNumber forkNum);
extern Buffer _hash_getbuf_with_strategy(Relation rel, BlockNumber blkno,
										 int access, int flags,
										 BufferAccessStrategy bstrategy);
extern void _hash_relbuf(Relation rel, Buffer buf);
extern void _hash_dropbuf(Relation rel, Buffer buf);
extern void _hash_dropscanbuf(Relation rel, HashScanOpaque so);
extern uint32 _hash_init(Relation rel, double num_tuples,
						 ForkNumber forkNum);
extern void _hash_init_metabuffer(Buffer buf, double num_tuples,
								  RegProcedure procid, uint16 ffactor, bool initpage);
extern void _hash_pageinit(Page page, Size size);
extern void _hash_expandtable(Relation rel, Buffer metabuf);
extern void _hash_finish_split(Relation rel, Buffer metabuf, Buffer obuf,
							   Bucket obucket, uint32 maxbucket, uint32 highmask,
							   uint32 lowmask);

/* hashsearch.c */
extern bool _hash_next(IndexScanDesc scan, ScanDirection dir);
extern bool _hash_first(IndexScanDesc scan, ScanDirection dir);

/* hashsort.c */
typedef struct HSpool HSpool;	/* hashsort.c不透明结构 */

extern HSpool *_h_spoolinit(Relation heap, Relation index, uint32 num_buckets);
extern void _h_spooldestroy(HSpool *hspool);
extern void _h_spool(HSpool *hspool, ItemPointer self,
					 Datum *values, bool *isnull);
extern void _h_indexbuild(HSpool *hspool, Relation heapRel);

/* hashutil.c */
extern bool _hash_checkqual(IndexScanDesc scan, IndexTuple itup);
extern uint32 _hash_datum2hashkey(Relation rel, Datum key);
extern uint32 _hash_datum2hashkey_type(Relation rel, Datum key, Oid keytype);
extern Bucket _hash_hashkey2bucket(uint32 hashkey, uint32 maxbucket,
								   uint32 highmask, uint32 lowmask);
extern uint32 _hash_spareindex(uint32 num_bucket);
extern uint32 _hash_get_totalbuckets(uint32 splitpoint_phase);
extern void _hash_checkpage(Relation rel, Buffer buf, int flags);
extern uint32 _hash_get_indextuple_hashkey(IndexTuple itup);
extern bool _hash_convert_tuple(Relation index,
								Datum *user_values, bool *user_isnull,
								Datum *index_values, bool *index_isnull);
extern OffsetNumber _hash_binsearch(Page page, uint32 hash_value);
extern OffsetNumber _hash_binsearch_last(Page page, uint32 hash_value);
extern BlockNumber _hash_get_oldblock_from_newbucket(Relation rel, Bucket new_bucket);
extern BlockNumber _hash_get_newblock_from_oldbucket(Relation rel, Bucket old_bucket);
extern Bucket _hash_get_newbucket_from_oldbucket(Relation rel, Bucket old_bucket,
												 uint32 lowmask, uint32 maxbucket);
extern void _hash_kill_items(IndexScanDesc scan);

/* hash.c */
extern void hashbucketcleanup(Relation rel, Bucket cur_bucket,
							  Buffer bucket_buf, BlockNumber bucket_blkno,
							  BufferAccessStrategy bstrategy,
							  uint32 maxbucket, uint32 highmask, uint32 lowmask,
							  double *tuples_removed, double *num_index_tuples,
							  bool split_cleanup,
							  IndexBulkDeleteCallback callback, void *callback_state);

#endif							/* HASH_H */
