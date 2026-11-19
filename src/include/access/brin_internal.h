/*
 * brin_internal.h
 *		BRIN 索引的内部声明
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin_internal.h
 */
#ifndef BRIN_INTERNAL_H
#define BRIN_INTERNAL_H

#include "access/amapi.h"
#include "storage/bufpage.h"
#include "utils/typcache.h"


/*
 * BrinDesc 结构是设计用于把 BRIN 元组从
 * 磁盘格式解码为内存元组，反之亦然。
 */

/* "OpcInfo" amproc 所返回的结构 */
typedef struct BrinOpcInfo
{
	/* 本操作符类索引列中存储的列数 */
	uint16		oi_nstored;

	/* BrinValues 中常规处理 NULLs? */
	bool		oi_regular_nulls;

	/* 操作符类私用的不透明指针 */
	void	   *oi_opaque;

	/* 所存列的类型缓存条目 */
	TypeCacheEntry *oi_typcache[FLEXIBLE_ARRAY_MEMBER];
} BrinOpcInfo;

/* 给定列数时 BrinOpcInfo 的大小 */
#define SizeofBrinOpcInfo(ncols) \
	(offsetof(BrinOpcInfo, oi_typcache) + sizeof(TypeCacheEntry *) * ncols)

typedef struct BrinDesc
{
	/* 所属内存上下文 */
	MemoryContext bd_context;

	/* 索引关系本身 */
	Relation	bd_index;

	/* 索引关系的元组描述符 */
	TupleDesc	bd_tupdesc;

	/* 磁盘元组的缓存副本；首次使用时生成 */
	TupleDesc	bd_disktdesc;

	/* 全部列在磁盘上存储的 Datum 项总数 */
	int			bd_totalstored;

	/* 每列信息；长为 bd_tupdesc->natts 项 */
	BrinOpcInfo *bd_info[FLEXIBLE_ARRAY_MEMBER];
} BrinDesc;

/*
 * BRIN 索引的全局已知支持函数编号。各
 * 操作符类可再定义更多支持函数编号，且须落入
 * BRIN_FIRST_OPTIONAL_PROCNUM .. BRIN_LAST_OPTIONAL_PROCNUM.
 */
#define BRIN_PROCNUM_OPCINFO		1
#define BRIN_PROCNUM_ADDVALUE		2
#define BRIN_PROCNUM_CONSISTENT		3
#define BRIN_PROCNUM_UNION			4
#define BRIN_MANDATORY_NPROCS		4
#define BRIN_PROCNUM_OPTIONS 		5	/* 可选 */
/* 过程编号至 10 为 BRIN 未来扩展保留 */
#define BRIN_FIRST_OPTIONAL_PROCNUM 11
#define BRIN_LAST_OPTIONAL_PROCNUM	15

#undef BRIN_DEBUG

#ifdef BRIN_DEBUG
#define BRIN_elog(args)			elog args
#else
#define BRIN_elog(args)			((void) 0)
#endif

/* brin.c */
extern BrinDesc *brin_build_desc(Relation rel);
extern void brin_free_desc(BrinDesc *bdesc);
extern IndexBuildResult *brinbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);
extern void brinbuildempty(Relation index);
extern bool brininsert(Relation idxRel, Datum *values, bool *nulls,
					   ItemPointer heaptid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
extern IndexScanDesc brinbeginscan(Relation r, int nkeys, int norderbys);
extern int64 bringetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void brinrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);
extern void brinendscan(IndexScanDesc scan);
extern IndexBulkDeleteResult *brinbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);
extern IndexBulkDeleteResult *brinvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);
extern bytea *brinoptions(Datum reloptions, bool validate);

/* brin_validate.c */
extern bool brinvalidate(Oid opclassoid);

#endif							/* BRIN_INTERNAL_H */
