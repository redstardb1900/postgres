/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES 关系扫描描述符定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/relscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/htup_details.h"
#include "access/itup.h"
#include "port/atomics.h"
#include "storage/buf.h"
#include "storage/spin.h"
#include "utils/relcache.h"


struct ParallelTableScanDescData;

/*
 * 表扫描的通用描述符。这是表扫描的基类，
 * 它需嵌入各个访问方法(AM)的扫描结构中。
 */
typedef struct TableScanDescData
{
	/* 扫描参数 */
	Relation	rs_rd;			/* 堆关系描述符 */
	struct SnapshotData *rs_snapshot;	/* 可见性快照 */
	int			rs_nkeys;		/* 扫描键数量 */
	struct ScanKeyData *rs_key; /* 扫描键描述符数组 */

	/* table_scan_getnextslot_tidrange() 扫描的 ItemPointer 范围。 */
	ItemPointerData rs_mintid;
	ItemPointerData rs_maxtid;

	/*
	 * 扫描的类型与行为信息，为位掩码，其成员取自
	 * ScanOptions 枚举(见 tableam.h)。
	 */
	uint32		rs_flags;

	struct ParallelTableScanDescData *rs_parallel;	/* 并行扫描
													 * 信息*/
} TableScanDescData;
typedef struct TableScanDescData *TableScanDesc;

/*
 * 并行表扫描的共享状态。
 *
 * 参与并行表扫描的每个后端，在后端私有内存中
 * 各有自己的 TableScanDesc，这些对象都含有指向本
 * 结构体的指针。此处信息须足以在
 * 工作进程加入扫描时正确初始化每个新的 TableScanDesc，且
 * 须充当告知这些工作进程扫描内容的信息。
 */
typedef struct ParallelTableScanDescData
{
	Oid			phs_relid;		/* 待扫描关系的 OID */
	bool		phs_syncscan;	/* 向 syncscan 逻辑报告位置？ */
	bool		phs_snapshot_any;	/* SnapshotAny 非 phs_snapshot_data? */
	Size		phs_snapshot_off;	/* 快照数据 */
} ParallelTableScanDescData;
typedef struct ParallelTableScanDescData *ParallelTableScanDesc;

/*
 * 面向块存储的并行表扫描共享状态。
 */
typedef struct ParallelBlockTableScanDescData
{
	ParallelTableScanDescData base;

	BlockNumber phs_nblocks;	/* # 扫描开始时关系中的块数 */
	slock_t		phs_mutex;		/* 设置 startblock 时的互斥 */
	BlockNumber phs_startblock; /* 起始块号 */
	pg_atomic_uint64 phs_nallocated;	/* 迄今分配给工作进程
										 * 的块数。*/
}			ParallelBlockTableScanDescData;
typedef struct ParallelBlockTableScanDescData *ParallelBlockTableScanDesc;

/*
 * 面向块存储的并行表扫描，各后端私有状态。
 */
typedef struct ParallelBlockTableScanWorkerData
{
	uint64		phsw_nallocated;	/* 当前扫描已推进的块数 */
	uint32		phsw_chunk_remaining;	/* # 本块组剩余的块数 */
	uint32		phsw_chunk_size;	/* 扫描时每个 I/O 块组中
									 * 要分配的块数*/
} ParallelBlockTableScanWorkerData;
typedef struct ParallelBlockTableScanWorkerData *ParallelBlockTableScanWorker;

/*
 * 通过索引从表中获取数据的基类。这是此类扫描
 * 的基类，它需嵌入各个访问方法的
 * 结构体中。
 */
typedef struct IndexFetchTableData
{
	Relation	rel;
} IndexFetchTableData;

/*
 * 基于 amgettuple 与基于 amgetbitmap 的索引扫描均使用
 * 同一 IndexScanDescData 结构体。部分字段仅与 amgettuple
 * 扫描有关。
 */
typedef struct IndexScanDescData
{
	/* 扫描参数 */
	Relation	heapRelation;	/* 堆关系描述符，或 NULL */
	Relation	indexRelation;	/* 索引关系描述符 */
	struct SnapshotData *xs_snapshot;	/* 可见性快照 */
	int			numberOfKeys;	/* 索引限定条件数量 */
	int			numberOfOrderBys;	/* 排序操作符数量 */
	struct ScanKeyData *keyData;	/* 索引限定条件描述符数组 */
	struct ScanKeyData *orderByData;	/* 排序操作符描述符数组 */
	bool		xs_want_itup;	/* 调用方请求索引元组 */
	bool		xs_temp_snap;	/* 扫描结束时注销快照？ */

	/* 向 AM 通告杀死索引元组 */
	bool		kill_prior_tuple;	/* 上次返回的元组已死 */
	bool		ignore_killed_tuples;	/* 不返回已杀条目 */
	bool		xactStartedInRecovery;	/* 防止杀死或看到已杀
										 * 元组*/

	/* 索引访问方法的私有状态 */
	void	   *opaque;			/* 访问方法专有信息 */

	/*
	 * 在仅索引扫描中，成功的 amgettuple 调用必须填充
	 * xs_itup(及 xs_itupdesc)或 xs_hitup(及 xs_hitupdesc)之一，
	 * 以提供扫描返回的数据。两者都可填充，此时将
	 * 采用堆格式。
	 */
	IndexTuple	xs_itup;		/* AM 返回的索引元组 */
	struct TupleDescData *xs_itupdesc;	/* xs_itup 的行类型描述符 */
	HeapTuple	xs_hitup;		/* AM 返回的索引数据(HeapTuple) */
	struct TupleDescData *xs_hitupdesc; /* xs_hitup 的行类型描述符 */

	ItemPointerData xs_heaptid; /* 结果 */
	bool		xs_heap_continue;	/* T 表示须继续遍历，可能
									 * 有更多结果*/
	IndexFetchTableData *xs_heapfetch;

	bool		xs_recheck;		/* T 表示扫描键须重检 */

	/*
	 * 使用排序操作符获取时，上一返回元组的 ORDER BY
	 * 表达式的值，按索引所得。若
	 * xs_recheckorderby 为真，则这些值需像
	 * 扫描键一样重检，且此处返回的值不会大于实际
	 * 值。
	 */
	Datum	   *xs_orderbyvals;
	bool	   *xs_orderbynulls;
	bool		xs_recheckorderby;

	/* 并行索引扫描信息，位于共享内存中 */
	struct ParallelIndexScanDescData *parallel_scan;
}			IndexScanDescData;

/* 并行扫描的通用结构体 */
typedef struct ParallelIndexScanDescData
{
	Oid			ps_relid;
	Oid			ps_indexid;
	Size		ps_offset;		/* AM 专有结构体的字节偏移量 */
	char		ps_snapshot_data[FLEXIBLE_ARRAY_MEMBER];
}			ParallelIndexScanDescData;

struct TupleTableSlot;

/* 系统表的存储或索引扫描所用结构体 */
typedef struct SysScanDescData
{
	Relation	heap_rel;		/* 被扫描的目录 */
	Relation	irel;			/* 堆扫描时为 NULL */
	struct TableScanDescData *scan; /* 仅在存储扫描时有效 */
	struct IndexScanDescData *iscan;	/* 仅在索引扫描时有效 */
	struct SnapshotData *snapshot;	/* 扫描结束时要注销的快照 */
	struct TupleTableSlot *slot;
}			SysScanDescData;

#endif							/* RELSCAN_H */
