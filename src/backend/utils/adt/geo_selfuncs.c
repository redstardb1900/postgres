/*-------------------------------------------------------------------------
 *
 * geo_selfuncs.c
 *	  选择性例程，注册于算子目录的
 *	  "oprrest" 与 "oprjoin" 属性。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/geo_selfuncs.c
 *
 *	XXX 这些完全是在瞎估。或许将来有人会让它们
 *	做点像样的事。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"
#include "utils/geo_decls.h"


/*
 *	几何算子的选择性函数。这些都是瞎估的——除非
 *	我们了解索引中键的实际分布，否则就无法
 *	对这些算子的选择性做出良好预测。
 *
 *	注：这里用的值可能显得小得离谱。也许确实
 *	如此。眼下，我们要确保优化器在几何索引
 *	可用时会用到它，所以选择性最好设得
 *	相当小。
 *
 *	一般而言，GiST 需要搜索多棵子树，才能保证已
 *	找到同一键的所有出现位置。正因如此，扫描索引
 *	的估计开销应当高于输出选择性所
 *	显示的水平。gistcostestimate()（在 selfuncs.c 中）
 *	也应当相应调整——但在能产生更接近真实
 *	数字之前，这里基本无关紧要...
 */


/*
 * 依赖于面积的算子（例如 "overlap"）的选择性。
 */

Datum
areasel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.005);
}

Datum
areajoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.005);
}

/*
 *	positionsel
 *
 * 一个 box 有多大可能严格在给定 box 左（右、上、
 * 下）侧？
 */

Datum
positionsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.1);
}

Datum
positionjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.1);
}

/*
 *	contsel -- box 有多大可能包含（被包含于）给定 box？
 *
 * 这比 "overlap" 约束更紧，因此给出的估计值
 * 要小于 areasel。
 */

Datum
contsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}

Datum
contjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}
