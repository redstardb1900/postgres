/*-------------------------------------------------------------------------
 *
 * instrument.h
 *	  运行时统计收集的定义
 *
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * src/include/executor/instrument.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include "portability/instr_time.h"


/*
 * BufferUsage and WalUsage counters keep being incremented infinitely,
 * i.e., must never be reset to zero, so that we can calculate how much
 * the counters are incremented in an arbitrary period.
 */
typedef struct BufferUsage
{
	int64		shared_blks_hit;	/*  共享缓冲命中数  */
	int64		shared_blks_read;	/*  共享磁盘块读数  */
	int64		shared_blks_dirtied;	/*  共享块弄脏数  */
	int64		shared_blks_written;	/*  共享磁盘块写数  */
	int64		local_blks_hit; /* # of local buffer hits */
	int64		local_blks_read;	/*  本地磁盘块读数  */
	int64		local_blks_dirtied; /*  本地块弄脏数  */
	int64		local_blks_written; /*  本地磁盘块写数  */
	int64		temp_blks_read; /*  临时块读数  */
	int64		temp_blks_written;	/*  临时块写数  */
	instr_time	blk_read_time;	/*  读块花费的时间  */
	instr_time	blk_write_time; /*  写块花费的时间  */
	instr_time	temp_blk_read_time; /*  读临时块花费的时间  */
	instr_time	temp_blk_write_time;	/*  写临时块花费的时间  */
} BufferUsage;

/*
 * WalUsage tracks only WAL activity like WAL records generation that
 * can be measured per query and is displayed by EXPLAIN command,
 * pg_stat_statements extension, etc. It does not track other WAL activity
 * like WAL writes that it's not worth measuring per query. That's tracked
 * by WAL global statistics counters in WalStats, instead.
 */
typedef struct WalUsage
{
	int64		wal_records;	/*  产生的 WAL 记录数  */
	int64		wal_fpi;		/*  产生的 WAL 整页映像数  */
	uint64		wal_bytes;		/*  产生的 WAL 记录大小  */
} WalUsage;

/*  InstrAlloc 的 instrument_options 位掩码中的标志位  */
typedef enum InstrumentOption
{
	INSTRUMENT_TIMER = 1 << 0,	/* needs timer (and row counts) */
	INSTRUMENT_BUFFERS = 1 << 1,	/* needs buffer usage */
	INSTRUMENT_ROWS = 1 << 2,	/* needs row count */
	INSTRUMENT_WAL = 1 << 3,	/* needs WAL usage */
	INSTRUMENT_ALL = PG_INT32_MAX
} InstrumentOption;

typedef struct Instrumentation
{
	/* Parameters set at node creation: */
	bool		need_timer;		/* true if we need timer data */
	bool		need_bufusage;	/* true if we need buffer usage data */
	bool		need_walusage;	/* true if we need WAL usage data */
	bool		async_mode;		/* true if node is in async mode */
	/* Info about current plan cycle: */
	bool		running;		/* true if we've completed first tuple */
	instr_time	starttime;		/*  节点当前迭代的开始时间  */
	instr_time	counter;		/*  本节点累计运行时间  */
	double		firsttuple;		/* time for first tuple of this cycle */
	double		tuplecount;		/*  本周期迄今发出的元组数  */
	BufferUsage bufusage_start; /* buffer usage at start */
	WalUsage	walusage_start; /* WAL usage at start */
	/*  所有已完成周期的累计统计:  */
	double		startup;		/*  总启动时间(秒)  */
	double		total;			/*  总时间(秒)  */
	double		ntuples;		/* total tuples produced */
	double		ntuples2;		/*  节点专用次要元组计数器  */
	double		nloops;			/*  本节点运行周期数  */
	double		nfiltered1;		/* # of tuples removed by scanqual or joinqual */
	double		nfiltered2;		/* # of tuples removed by "other" quals */
	BufferUsage bufusage;		/*  总缓冲用量  */
	WalUsage	walusage;		/* total WAL usage */
} Instrumentation;

typedef struct WorkerInstrumentation
{
	int			num_workers;	/*  后续结构数  */
	Instrumentation instrument[FLEXIBLE_ARRAY_MEMBER];
} WorkerInstrumentation;

extern PGDLLIMPORT BufferUsage pgBufferUsage;
extern PGDLLIMPORT WalUsage pgWalUsage;

extern Instrumentation *InstrAlloc(int n, int instrument_options,
								   bool async_mode);
extern void InstrInit(Instrumentation *instr, int instrument_options);
extern void InstrStartNode(Instrumentation *instr);
extern void InstrStopNode(Instrumentation *instr, double nTuples);
extern void InstrUpdateTupleCount(Instrumentation *instr, double nTuples);
extern void InstrEndLoop(Instrumentation *instr);
extern void InstrAggNode(Instrumentation *dst, Instrumentation *add);
extern void InstrStartParallelQuery(void);
extern void InstrEndParallelQuery(BufferUsage *bufusage, WalUsage *walusage);
extern void InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage);
extern void BufferUsageAccumDiff(BufferUsage *dst,
								 const BufferUsage *add, const BufferUsage *sub);
extern void WalUsageAccumDiff(WalUsage *dst, const WalUsage *add,
							  const WalUsage *sub);

#endif							/* INSTRUMENT_H */
