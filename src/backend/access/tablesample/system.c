/*-------------------------------------------------------------------------
 *
 * system.c
 *	  SYSTEM 表采样方法的支持例程
 *
 * 为保证样本可重复，对给定元组的选择必须
 * 与历史无关；否则同步扫描会破坏可重复性，
 * 更不必说逻辑上无关的维护，例如关系的
 * 物理扩展或缩短。
 *
 * 为此，我们把每个候选块号与当前生效的种子
 * 一起做哈希，若哈希小于 BeginSampleScan 按选择
 * 概率算出的截止值，就选中它。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/tablesample/system.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/relscan.h"
#include "access/tsmapi.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"


/* 私有状态 */
typedef struct
{
	uint64		cutoff;			/* 选择哈希小于此值的块 */
	uint32		seed;			/* 随机种子 */
	BlockNumber nextblock;		/* 下一个考虑采样的块 */
	OffsetNumber lt;			/* 当前块已返回的最后一个元组 */
} SystemSamplerData;


static void system_samplescangetsamplesize(PlannerInfo *root,
										   RelOptInfo *baserel,
										   List *paramexprs,
										   BlockNumber *pages,
										   double *tuples);
static void system_initsamplescan(SampleScanState *node,
								  int eflags);
static void system_beginsamplescan(SampleScanState *node,
								   Datum *params,
								   int nparams,
								   uint32 seed);
static BlockNumber system_nextsampleblock(SampleScanState *node, BlockNumber nblocks);
static OffsetNumber system_nextsampletuple(SampleScanState *node,
										   BlockNumber blockno,
										   OffsetNumber maxoffset);


/*
 * 为 SYSTEM 方法创建 TsmRoutine 描述符。
 */
Datum
tsm_system_handler(PG_FUNCTION_ARGS)
{
	TsmRoutine *tsm = makeNode(TsmRoutine);

	tsm->parameterTypes = list_make1_oid(FLOAT4OID);
	tsm->repeatable_across_queries = true;
	tsm->repeatable_across_scans = true;
	tsm->SampleScanGetSampleSize = system_samplescangetsamplesize;
	tsm->InitSampleScan = system_initsamplescan;
	tsm->BeginSampleScan = system_beginsamplescan;
	tsm->NextSampleBlock = system_nextsampleblock;
	tsm->NextSampleTuple = system_nextsampletuple;
	tsm->EndSampleScan = NULL;

	PG_RETURN_POINTER(tsm);
}

/*
 * 采样规模估计。
 */
static void
system_samplescangetsamplesize(PlannerInfo *root,
							   RelOptInfo *baserel,
							   List *paramexprs,
							   BlockNumber *pages,
							   double *tuples)
{
	Node	   *pctnode;
	float4		samplefract;

	/* 尝试提取采样百分比的估计值 */
	pctnode = (Node *) linitial(paramexprs);
	pctnode = estimate_expression_value(root, pctnode);

	if (IsA(pctnode, Const) &&
		!((Const *) pctnode)->constisnull)
	{
		samplefract = DatumGetFloat4(((Const *) pctnode)->constvalue);
		if (samplefract >= 0 && samplefract <= 100 && !isnan(samplefract))
			samplefract /= 100.0f;
		else
		{
			/* 若该值无效，则使用默认的 samplefract */
			samplefract = 0.1f;
		}
	}
	else
	{
		/* 若未得到非空 Const，则使用默认的 samplefract */
		samplefract = 0.1f;
	}

	/* 我们将访问这些页中的一个样本 ... */
	*pages = clamp_row_est(baserel->pages * samplefract);

	/* ... 并有望从中得到数量有代表性的元组 */
	*tuples = clamp_row_est(baserel->tuples * samplefract);
}

/*
 * 在执行器建立期间初始化。
 */
static void
system_initsamplescan(SampleScanState *node, int eflags)
{
	node->tsm_state = palloc0(sizeof(SystemSamplerData));
}

/*
 * 检查参数，并准备一次采样扫描。
 */
static void
system_beginsamplescan(SampleScanState *node,
					   Datum *params,
					   int nparams,
					   uint32 seed)
{
	SystemSamplerData *sampler = (SystemSamplerData *) node->tsm_state;
	double		percent = DatumGetFloat4(params[0]);
	double		dcutoff;

	if (percent < 0 || percent > 100 || isnan(percent))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
				 errmsg("sample percentage must be between 0 and 100")));

	/*
	 * 截止值是采样概率乘以 (PG_UINT32_MAX + 1)；当然必须
	 * 把它存成 uint64。注意，这在概率为 0 或 1 的
	 * 边界上给出严格正确的行为。
	 */
	dcutoff = rint(((double) PG_UINT32_MAX + 1) * percent / 100);
	sampler->cutoff = (uint64) dcutoff;
	sampler->seed = seed;
	sampler->nextblock = 0;
	sampler->lt = InvalidOffsetNumber;

	/*
	 * 除非扫描的只是表中很小的一部分，否则批量读缓冲区
	 * 访问策略大概是合适的。这里的 1% 截止值只是
	 * 猜测。应当使用页模式可见性检查，因为我们会扫描
	 * 每个选中页上的全部元组。
	 */
	node->use_bulkread = (percent >= 1);
	node->use_pagemode = true;
}

/*
 * 选择下一个要采样的块。
 */
static BlockNumber
system_nextsampleblock(SampleScanState *node, BlockNumber nblocks)
{
	SystemSamplerData *sampler = (SystemSamplerData *) node->tsm_state;
	BlockNumber nextblock = sampler->nextblock;
	uint32		hashinput[2];

	/*
	 * 我们把包含块号和种子的 2 个 uint32
	 * 数组交给 hash_any 来计算哈希。这样建立起来很高效，
	 * 而且按 hash_any 的当前实现，它给出
	 * 与机器无关的结果，这对回归
	 * 测试是个好性质。
	 *
	 * 哈希输入中的这些字在整个块内保持不变：
	 */
	hashinput[1] = sampler->seed;

	/*
	 * 遍历块号，直到找到合适的块，或到达
	 * 关系末尾。
	 */
	for (; nextblock < nblocks; nextblock++)
	{
		uint32		hash;

		hashinput[0] = nextblock;

		hash = DatumGetUInt32(hash_any((const unsigned char *) hashinput,
									   (int) sizeof(hashinput)));
		if (hash < sampler->cutoff)
			break;
	}

	if (nextblock < nblocks)
	{
		/* 找到合适的块；记住下次应当从哪里开始 */
		sampler->nextblock = nextblock + 1;
		return nextblock;
	}

	/* 已经完成，但为安全起见把 nextblock 重置为 0。 */
	sampler->nextblock = 0;
	return InvalidBlockNumber;
}

/*
 * 选择当前块中下一个被采样的元组。
 *
 * 在块采样中，我们只想采样每个选中块里的
 * 全部元组。
 *
 * 这里可以在不知道元组是否可见（甚至是否存在）的
 * 情况下返回偏移；nodeSamplescan.c 会处理这一点。
 *
 * 到达块尾时返回 InvalidOffsetNumber，告知
 * SampleScan 转到下一块。
 */
static OffsetNumber
system_nextsampletuple(SampleScanState *node,
					   BlockNumber blockno,
					   OffsetNumber maxoffset)
{
	SystemSamplerData *sampler = (SystemSamplerData *) node->tsm_state;
	OffsetNumber tupoffset = sampler->lt;

	/* 前进到页上下一个可能的偏移 */
	if (tupoffset == InvalidOffsetNumber)
		tupoffset = FirstOffsetNumber;
	else
		tupoffset++;

	/* 完成了？ */
	if (tupoffset > maxoffset)
		tupoffset = InvalidOffsetNumber;

	sampler->lt = tupoffset;

	return tupoffset;
}
