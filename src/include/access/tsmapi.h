/*-------------------------------------------------------------------------
 *
 * tsmapi.h
 *	  表采样方法的 API
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * src/include/access/tsmapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TSMAPI_H
#define TSMAPI_H

#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"


/*
 * 回调函数签名 --- 详见 tablesample-method.sgml。
 */

typedef void (*SampleScanGetSampleSize_function) (PlannerInfo *root,
												  RelOptInfo *baserel,
												  List *paramexprs,
												  BlockNumber *pages,
												  double *tuples);

typedef void (*InitSampleScan_function) (SampleScanState *node,
										 int eflags);

typedef void (*BeginSampleScan_function) (SampleScanState *node,
										  Datum *params,
										  int nparams,
										  uint32 seed);

typedef BlockNumber (*NextSampleBlock_function) (SampleScanState *node,
												 BlockNumber nblocks);

typedef OffsetNumber (*NextSampleTuple_function) (SampleScanState *node,
												  BlockNumber blockno,
												  OffsetNumber maxoffset);

typedef void (*EndSampleScan_function) (SampleScanState *node);

/*
 * TsmRoutine 是表采样方法处理函数所返回的
 * 结构体。它提供规划器与执行器所需回调函数的
 * 指针，以及关于该方法的额外信息。
 *
 * 将来很可能再增加更多函数指针。
 * 因此建议处理函数用 
 * makeNode(TsmRoutine) 初始化该结构，使各字段为 NULL。
 * 这会确保没有字段被意外留作未定义。
 */
typedef struct TsmRoutine
{
	NodeTag		type;

	/* TABLESAMPLE 子句参数数据类型 OIDs 列表 */
	List	   *parameterTypes;

	/* 方法能否跨查询、甚至查询内产生可重复样本？ */
	bool		repeatable_across_queries;
	bool		repeatable_across_scans;

	/* 在物理表上规划 SampleScan 的函数 */
	SampleScanGetSampleSize_function SampleScanGetSampleSize;

	/* 在物理表上执行 SampleScan 的函数 */
	InitSampleScan_function InitSampleScan; /* 可为 NULL */
	BeginSampleScan_function BeginSampleScan;
	NextSampleBlock_function NextSampleBlock;	/* 可为 NULL */
	NextSampleTuple_function NextSampleTuple;
	EndSampleScan_function EndSampleScan;	/* 可为 NULL */
} TsmRoutine;


/* access/tablesample/tablesample.c 中的函数 */
extern TsmRoutine *GetTsmRoutine(Oid tsmhandler);

#endif							/* TSMAPI_H */
