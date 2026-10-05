/*-------------------------------------------------------------------------
 *
 * bernoulli.c
 *	  BERNOULLI 表采样方法的支持例程
 *
 * 为保证样本可重复，对给定元组的选择必须
 * 与历史无关；否则同步扫描会破坏可重复性，
 * 更不必说逻辑上无关的维护，例如关系的
 * 物理扩展或缩短。
 *
 * 为此，我们把每个候选 TID 与当前生效的种子
 * 一起做哈希，若哈希小于 BeginSampleScan 按选择
 * 概率算出的截止值，就选中它。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/tablesample/bernoulli.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/tsmapi.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"


/* 私有状态 */
typedef struct
{
	uint64		cutoff;			/* 选择哈希小于此值的元组 */
	uint32		seed;			/* 随机种子 */
	OffsetNumber lt;			/* 当前块已返回的最后一个元组 */
} BernoulliSamplerData;


static void bernoulli_samplescangetsamplesize(PlannerInfo *root,
											  RelOptInfo *baserel,
											  List *paramexprs,
											  BlockNumber *pages,
											  double *tuples);
static void bernoulli_initsamplescan(SampleScanState *node,
									 int eflags);
static void bernoulli_beginsamplescan(SampleScanState *node,
									  Datum *params,
									  int nparams,
									  uint32 seed);
static OffsetNumber bernoulli_nextsampletuple(SampleScanState *node,
											  BlockNumber blockno,
											  OffsetNumber maxoffset);


/*
 * 为 BERNOULLI 方法创建 TsmRoutine 描述符。
 */
Datum
tsm_bernoulli_handler(PG_FUNCTION_ARGS)
{
	TsmRoutine *tsm = makeNode(TsmRoutine);

	tsm->parameterTypes = list_make1_oid(FLOAT4OID);
	tsm->repeatable_across_queries = true;
	tsm->repeatable_across_scans = true;
	tsm->SampleScanGetSampleSize = bernoulli_samplescangetsamplesize;
	tsm->InitSampleScan = bernoulli_initsamplescan;
	tsm->BeginSampleScan = bernoulli_beginsamplescan;
	tsm->NextSampleBlock = NULL;
	tsm->NextSampleTuple = bernoulli_nextsampletuple;
	tsm->EndSampleScan = NULL;

	PG_RETURN_POINTER(tsm);
}

/*
 * 采样规模估计。
 */
static void
bernoulli_samplescangetsamplesize(PlannerInfo *root,
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

	/* 我们将访问 baserel 的全部页 */
	*pages = baserel->pages;

	*tuples = clamp_row_est(baserel->tuples * samplefract);
}

/*
 * 在执行器建立期间初始化。
 */
static void
bernoulli_initsamplescan(SampleScanState *node, int eflags)
{
	node->tsm_state = palloc0(sizeof(BernoulliSamplerData));
}

/*
 * 检查参数，并准备一次采样扫描。
 */
static void
bernoulli_beginsamplescan(SampleScanState *node,
						  Datum *params,
						  int nparams,
						  uint32 seed)
{
	BernoulliSamplerData *sampler = (BernoulliSamplerData *) node->tsm_state;
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
	sampler->lt = InvalidOffsetNumber;

	/*
	 * 使用批量读，因为我们要扫描全部页。但页模式可见性
	 * 检查只在较大的采样比例下才划算。这里的 25% 截止
	 * 值基于非常有限的实验。
	 */
	node->use_bulkread = true;
	node->use_pagemode = (percent >= 25);
}

/*
 * 选择当前块中下一个被采样的元组。
 *
 * 这里可以在不知道元组是否可见（甚至是否
 * 存在）的情况下返回偏移。原因是我们对表中每个
 * 元组偏移都做抛硬币判定。由于所有元组被返回的
 * 概率相同，对不可见元组多做几次抛硬币也无妨。
 *
 * 到达块尾时返回 InvalidOffsetNumber，告知
 * SampleScan 转到下一块。
 */
static OffsetNumber
bernoulli_nextsampletuple(SampleScanState *node,
						  BlockNumber blockno,
						  OffsetNumber maxoffset)
{
	BernoulliSamplerData *sampler = (BernoulliSamplerData *) node->tsm_state;
	OffsetNumber tupoffset = sampler->lt;
	uint32		hashinput[3];

	/* 前进到块中的第一个或下一个元组 */
	if (tupoffset == InvalidOffsetNumber)
		tupoffset = FirstOffsetNumber;
	else
		tupoffset++;

	/*
	 * 我们把包含块号、偏移和种子的 3 个 uint32
	 * 数组交给 hash_any 来计算哈希。这样建立起来很高效，
	 * 而且按 hash_any 的当前实现，它给出
	 * 与机器无关的结果，这对回归
	 * 测试是个好性质。
	 *
	 * 哈希输入中的这些字在整个块内保持不变：
	 */
	hashinput[0] = blockno;
	hashinput[2] = sampler->seed;

	/*
	 * 遍历元组偏移，直到找到合适的 TID，或到达
	 * 块尾。
	 */
	for (; tupoffset <= maxoffset; tupoffset++)
	{
		uint32		hash;

		hashinput[1] = tupoffset;

		hash = DatumGetUInt32(hash_any((const unsigned char *) hashinput,
									   (int) sizeof(hashinput)));
		if (hash < sampler->cutoff)
			break;
	}

	if (tupoffset > maxoffset)
		tupoffset = InvalidOffsetNumber;

	sampler->lt = tupoffset;

	return tupoffset;
}
