/*-------------------------------------------------------------------------
 *
 * heaptoast.h
 *	  变长属性的外部与压缩存储的
 *	  堆专用定义。
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/include/access/heaptoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPTOAST_H
#define HEAPTOAST_H

#include "access/htup_details.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

/*
 * 求每页容纳 N 个元组时单个元组的最大尺寸。
 */
#define MaximumBytesPerTuple(tuplesPerPage) \
	MAXALIGN_DOWN((BLCKSZ - \
				   MAXALIGN(SizeOfPageHeaderData + (tuplesPerPage) * sizeof(ItemIdData))) \
				  / (tuplesPerPage))

/*
 * 这些符号控制 toaster 的激活。若元组大于
 * TOAST_TUPLE_THRESHOLD，则尝试把它 toast 到不超过
 * TOAST_TUPLE_TARGET 字节，做法是压缩可压缩字段，并
 * 把 EXTENDED 和 EXTERNAL 数据外置。
 *
 * 两个数值不必相同，尽管目前相同。TARGET
 * 超过 THRESHOLD 没有意义，但把它设得更小
 * 也可取。
 *
 * 目前把两个值都取为最大元组尺寸，即堆页上可放下
 * TOAST_TUPLES_PER_PAGE 个元组时的元组尺寸。
 *
 * XXX 这些值虽可不经 initdb 就修改，但在随意改动前，
 * 需先斟酌 toasting.c 中的 needs_toast_table()。
 * 另见 large_object.h 中的 LOBLKSIZE，它*必须*
 * 经 initdb 才能改。
 */
#define TOAST_TUPLES_PER_PAGE	4

#define TOAST_TUPLE_THRESHOLD	MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE)

#define TOAST_TUPLE_TARGET		TOAST_TUPLE_THRESHOLD

/*
 * 代码还会考虑把 MAIN 数据外置，但仅作为
 * 最后手段，即前面各步骤仍未达到目标元组尺寸时。
 * 此阶段我们使用另一个目标尺寸，当前等于
 * 能放入堆页的最大元组。这样合理，因为
 * 用户已告知我们尽可能把数据保持在行内。
 */
#define TOAST_TUPLES_PER_PAGE_MAIN	1

#define TOAST_TUPLE_TARGET_MAIN MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE_MAIN)

/*
 * 若索引值大于 TOAST_INDEX_TARGET，我们会尝试
 * 压缩它(但不能移到行外)。注意，为简化
 * index_form_tuple()，此数值按每个 datum 而非每个元组计。
 */
#define TOAST_INDEX_TARGET		(MaxHeapTupleSize / 16)

/*
 * 当我们在外部存储超长 datum 时，把它切分成块，
 * 每块至多含 TOAST_MAX_CHUNK_SIZE 个数据字节。该数*必须*
 * 足够小，使完成的 toast 表元组(含
 * ID 与序号字段及全部开销)能放进一页。
 * 这里的代码按如下设想定尺寸：希望每页能放下
 * EXTERN_TUPLES_PER_PAGE 个最大尺寸的元组。
 *
 * 注意：改动 TOAST_MAX_CHUNK_SIZE 需要 initdb。
 */
#define EXTERN_TUPLES_PER_PAGE	4	/* 仅调整此值 */

#define EXTERN_TUPLE_MAX_SIZE	MaximumBytesPerTuple(EXTERN_TUPLES_PER_PAGE)

#define TOAST_MAX_CHUNK_SIZE	\
	(EXTERN_TUPLE_MAX_SIZE -							\
	 MAXALIGN(SizeofHeapTupleHeader) -					\
	 sizeof(Oid) -										\
	 sizeof(int32) -									\
	 VARHDRSZ)

/* ----------
 * heap_toast_insert_or_update -
 *
 *	由 heap_insert()、heap_update() 调用
 * ----------
 */
extern HeapTuple heap_toast_insert_or_update(Relation rel, HeapTuple newtup,
											 HeapTuple oldtup, int options);

/* ----------
 * heap_toast_delete -
 *
 *	由 heap_delete() 调用
 * ----------
 */
extern void heap_toast_delete(Relation rel, HeapTuple oldtup,
							  bool is_speculative);

/* ----------
 * toast_flatten_tuple -
 *
 *	把元组"展平"，使其不含行外已 TOAST 字段。
 *	(这并不消除压缩的或短头的 datum。)
 * ----------
 */
extern HeapTuple toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc);

/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	把含行外已 TOAST 字段的元组"展平"成一个 Datum。
 * ----------
 */
extern Datum toast_flatten_tuple_to_datum(HeapTupleHeader tup,
										  uint32 tup_len,
										  TupleDesc tupleDesc);

/* ----------
 * toast_build_flattened_tuple -
 *
 *	构造一个不含行外已 TOAST 字段的元组。
 *	(这并不消除压缩的或短头的 datum。)
 * ----------
 */
extern HeapTuple toast_build_flattened_tuple(TupleDesc tupleDesc,
											 Datum *values,
											 bool *isnull);

/* ----------
 * heap_fetch_toast_slice
 *
 *	从堆表中存储的 TOAST 值取出一个切片。
 * ----------
 */
extern void heap_fetch_toast_slice(Relation toastrel, Oid valueid,
								   int32 attrsize, int32 sliceoffset,
								   int32 slicelength, struct varlena *result);

#endif							/* HEAPTOAST_H */
