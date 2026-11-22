/*-------------------------------------------------------------------------
 *
 * tuplestore.h
 *	  临时元组存储的通用例程。
 *
 * 本模块处理诸如 Materialize 节点、hashjoin 批
 * 文件等用途的临时元组存储。本质是 tuplesort.c
 * 的简化版；不对元组排序，只能存取并回吐序列。
 * 但因无需排序，允许在写完前就开始读。这对游标
 * 尤其有用，可在已扫描部分内随机访问而无需把
 * 底层扫描跑完。也支持多个独立读指针。
 * 见上。
 * 见上。
 * 见上。
 *
 * 若数据超出调用方指定空间限制，则使用临时
 * 文件。
 *
 * 自 Postgres 8.2 起，存储的只是 MinimalTuples；
 * 调用方不能指望回吐元组含有效系统列。API 也改
 * 为在 TupleTableSlots 中返回元组，以防访问系统
 * 列。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/tuplestore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPLESTORE_H
#define TUPLESTORE_H

#include "executor/tuptable.h"


/* Tuplestorestate 是不透明类型，细节见
 * tuplestore.c.
 */
typedef struct Tuplestorestate Tuplestorestate;

/*
 * 目前只需存 MinimalTuples，但很容易扩展到对
 * IndexTuples 和/或裸 Datum 支持相同行为。
 */

extern Tuplestorestate *tuplestore_begin_heap(bool randomAccess,
											  bool interXact,
											  int maxKBytes);

extern void tuplestore_set_eflags(Tuplestorestate *state, int eflags);

extern void tuplestore_puttupleslot(Tuplestorestate *state,
									TupleTableSlot *slot);
extern void tuplestore_puttuple(Tuplestorestate *state, HeapTuple tuple);
extern void tuplestore_putvalues(Tuplestorestate *state, TupleDesc tdesc,
								 Datum *values, bool *isnull);

/* 向后兼容宏 */
#define tuplestore_donestoring(state)	((void) 0)

extern int	tuplestore_alloc_read_pointer(Tuplestorestate *state, int eflags);

extern void tuplestore_select_read_pointer(Tuplestorestate *state, int ptr);

extern void tuplestore_copy_read_pointer(Tuplestorestate *state,
										 int srcptr, int destptr);

extern void tuplestore_trim(Tuplestorestate *state);

extern bool tuplestore_in_memory(Tuplestorestate *state);

extern bool tuplestore_gettupleslot(Tuplestorestate *state, bool forward,
									bool copy, TupleTableSlot *slot);

extern bool tuplestore_advance(Tuplestorestate *state, bool forward);

extern bool tuplestore_skiptuples(Tuplestorestate *state,
								  int64 ntuples, bool forward);

extern int64 tuplestore_tuple_count(Tuplestorestate *state);

extern bool tuplestore_ateof(Tuplestorestate *state);

extern void tuplestore_rescan(Tuplestorestate *state);

extern void tuplestore_clear(Tuplestorestate *state);

extern void tuplestore_end(Tuplestorestate *state);

#endif							/* TUPLESTORE_H */
