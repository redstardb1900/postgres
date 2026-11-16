/*-------------------------------------------------------------------------
 *
 * windowfuncs.c
 *	  SQL 规范定义的标准窗口函数。
 *
 * Portions Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/windowfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "windowapi.h"

/*
 * 排名处理信息
 */
typedef struct rank_context
{
	int64		rank;			/* 当前排名 */
} rank_context;

/*
 * ntile 处理信息
 */
typedef struct
{
	int32		ntile;			/* 当前结果 */
	int64		rows_per_bucket;	/* 当前桶的行号 */
	int64		boundary;		/* 每个桶应有多少行 */
	int64		remainder;		/* (总行数) % (桶数) */
} ntile_context;

static bool rank_up(WindowObject winobj);
static Datum leadlag_common(FunctionCallInfo fcinfo,
							bool forward, bool withoffset, bool withdefault);


/*
 * *_rank 函数的工具例程。
 */
static bool
rank_up(WindowObject winobj)
{
	bool		up = false;		/* 排名要增加吗？ */
	int64		curpos = WinGetCurrentPosition(winobj);
	rank_context *context;

	context = (rank_context *)
		WinGetPartitionLocalMemory(winobj, sizeof(rank_context));

	if (context->rank == 0)
	{
		/* 首次调用：第一行的排名总为 1 */
		Assert(curpos == 0);
		context->rank = 1;
	}
	else
	{
		Assert(curpos > 0);
		/* 当前行与前行按 ORDER BY 子句是否匹配？ */
		if (!WinRowsArePeers(winobj, curpos - 1, curpos))
			up = true;
	}

	/* 可推进标记，但只能在访问前行*之后*进行 */
	WinSetMarkPosition(winobj, curpos);

	return up;
}


/*
 * row_number
 * 从 1 起一直递增，直到当前分区结束。
 */
Datum
window_row_number(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	int64		curpos = WinGetCurrentPosition(winobj);

	WinSetMarkPosition(winobj, curpos);
	PG_RETURN_INT64(curpos + 1);
}

/*
 * window_row_number_support
 *		window_row_number() 的支持函数
 */
Datum
window_row_number_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;

		/* row_number() 单调递增 */
		req->monotonic = MONOTONICFUNC_INCREASING;
		PG_RETURN_POINTER(req);
	}

	if (IsA(rawreq, SupportRequestOptimizeWindowClause))
	{
		SupportRequestOptimizeWindowClause *req = (SupportRequestOptimizeWindowClause *) rawreq;

		/*
		 * 帧选项总是可以变成"ROWS BETWEEN UNBOUNDED
		 * PRECEDING AND CURRENT ROW"。row_number() 总是每
		 * 行加 1。使用 ROWS 而不是 RANGE
		 * 可省去执行期间检查同级行的工作。
		 */
		req->frameOptions = (FRAMEOPTION_NONDEFAULT |
							 FRAMEOPTION_ROWS |
							 FRAMEOPTION_START_UNBOUNDED_PRECEDING |
							 FRAMEOPTION_END_CURRENT_ROW);

		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}

/*
 * rank
 * 键列变化时排名变化。
 * 新排名号就是当前行号。
 */
Datum
window_rank(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	rank_context *context;
	bool		up;

	up = rank_up(winobj);
	context = (rank_context *)
		WinGetPartitionLocalMemory(winobj, sizeof(rank_context));
	if (up)
		context->rank = WinGetCurrentPosition(winobj) + 1;

	PG_RETURN_INT64(context->rank);
}

/*
 * window_rank_support
 *		window_rank() 的支持函数
 */
Datum
window_rank_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;

		/* rank() 单调递增 */
		req->monotonic = MONOTONICFUNC_INCREASING;
		PG_RETURN_POINTER(req);
	}

	if (IsA(rawreq, SupportRequestOptimizeWindowClause))
	{
		SupportRequestOptimizeWindowClause *req = (SupportRequestOptimizeWindowClause *) rawreq;

		/*
		 * rank() 的实现方式使其返回"(COUNT (*) OVER
		 * (<opt> RANGE UNBOUNDED PRECEDING) - COUNT (*) OVER (<opt> RANGE
		 * CURRENT ROW) + 1)"，与帧选项无关。我们把帧选项
		 * 设为"ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW"
		 * 以便与 window_row_number_support() 优化出的
		 * 帧选项一致。使用 ROWS 而不是 RANGE 可免于
		 * 执行期间的同级行检查。
		 */
		req->frameOptions = (FRAMEOPTION_NONDEFAULT |
							 FRAMEOPTION_ROWS |
							 FRAMEOPTION_START_UNBOUNDED_PRECEDING |
							 FRAMEOPTION_END_CURRENT_ROW);

		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}

/*
 * dense_rank
 * 键列变化时排名加 1。
 */
Datum
window_dense_rank(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	rank_context *context;
	bool		up;

	up = rank_up(winobj);
	context = (rank_context *)
		WinGetPartitionLocalMemory(winobj, sizeof(rank_context));
	if (up)
		context->rank++;

	PG_RETURN_INT64(context->rank);
}

/*
 * window_dense_rank_support
 *		window_dense_rank() 的支持函数
 */
Datum
window_dense_rank_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;

		/* dense_rank() 单调递增 */
		req->monotonic = MONOTONICFUNC_INCREASING;
		PG_RETURN_POINTER(req);
	}

	if (IsA(rawreq, SupportRequestOptimizeWindowClause))
	{
		SupportRequestOptimizeWindowClause *req = (SupportRequestOptimizeWindowClause *) rawreq;

		/*
		 * dense_rank() 不受帧选项影响。这里我们将帧选项
		 * 设成与 row_number 支持函数中相同的值。
		 * 使用 ROWS 而不是 RANGE（默认值）可让执行器
		 * 免于检查同级行。
		 */
		req->frameOptions = (FRAMEOPTION_NONDEFAULT |
							 FRAMEOPTION_ROWS |
							 FRAMEOPTION_START_UNBOUNDED_PRECEDING |
							 FRAMEOPTION_END_CURRENT_ROW);

		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}

/*
 * percent_rank
 * 返回 0 到 1 之间（含）的分数，
 * 按规范描述为 (RK - 1) / (NR - 1)，其中 RK 是当前行
 * 的排名，NR 是总行数。
 */
Datum
window_percent_rank(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	rank_context *context;
	bool		up;
	int64		totalrows = WinGetPartitionRowCount(winobj);

	Assert(totalrows > 0);

	up = rank_up(winobj);
	context = (rank_context *)
		WinGetPartitionLocalMemory(winobj, sizeof(rank_context));
	if (up)
		context->rank = WinGetCurrentPosition(winobj) + 1;

	/* 若只有一行则按规范返回零 */
	if (totalrows <= 1)
		PG_RETURN_FLOAT8(0.0);

	PG_RETURN_FLOAT8((float8) (context->rank - 1) / (float8) (totalrows - 1));
}

/*
 * window_percent_rank_support
 *		window_percent_rank() 的支持函数
 */
Datum
window_percent_rank_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;

		/* percent_rank() 单调递增 */
		req->monotonic = MONOTONICFUNC_INCREASING;
		PG_RETURN_POINTER(req);
	}

	if (IsA(rawreq, SupportRequestOptimizeWindowClause))
	{
		SupportRequestOptimizeWindowClause *req = (SupportRequestOptimizeWindowClause *) rawreq;

		/*
		 * percent_rank() 不受帧选项影响。这里我们将帧选项
		 * 设成与 row_number 支持函数中相同的值。
		 * 使用 ROWS 而不是 RANGE（默认值）可让执行器
		 * 免于检查同级行。
		 */
		req->frameOptions = (FRAMEOPTION_NONDEFAULT |
							 FRAMEOPTION_ROWS |
							 FRAMEOPTION_START_UNBOUNDED_PRECEDING |
							 FRAMEOPTION_END_CURRENT_ROW);

		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}


/*
 * cume_dist
 * 返回 0 到 1 之间（含）的分数，
 * 按规范描述为 NP / NR，其中 NP 是先行于或同级
 * 于当前行的行数，NR 是总行数。
 */
Datum
window_cume_dist(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	rank_context *context;
	bool		up;
	int64		totalrows = WinGetPartitionRowCount(winobj);

	Assert(totalrows > 0);

	up = rank_up(winobj);
	context = (rank_context *)
		WinGetPartitionLocalMemory(winobj, sizeof(rank_context));
	if (up || context->rank == 1)
	{
		/*
		 * 当前行与前一行不同级，或者就是第一行，因此
		 * 要累计与当前行同级的行数。
		 */
		int64		row;

		context->rank = WinGetCurrentPosition(winobj) + 1;

		/*
		 * 从当前 + 1 开始
		 */
		for (row = context->rank; row < totalrows; row++)
		{
			if (!WinRowsArePeers(winobj, row - 1, row))
				break;
			context->rank++;
		}
	}

	PG_RETURN_FLOAT8((float8) context->rank / (float8) totalrows);
}

/*
 * window_cume_dist_support
 *		window_cume_dist() 的支持函数
 */
Datum
window_cume_dist_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;

		/* cume_dist() 单调递增 */
		req->monotonic = MONOTONICFUNC_INCREASING;
		PG_RETURN_POINTER(req);
	}

	if (IsA(rawreq, SupportRequestOptimizeWindowClause))
	{
		SupportRequestOptimizeWindowClause *req = (SupportRequestOptimizeWindowClause *) rawreq;

		/*
		 * cume_dist() 不受帧选项影响。这里我们将帧选项
		 * 设成与 row_number 支持函数中相同的值。
		 * 使用 ROWS 而不是 RANGE（默认值）可让执行器
		 * 免于检查同级行。
		 */
		req->frameOptions = (FRAMEOPTION_NONDEFAULT |
							 FRAMEOPTION_ROWS |
							 FRAMEOPTION_START_UNBOUNDED_PRECEDING |
							 FRAMEOPTION_END_CURRENT_ROW);

		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}

/*
 * ntile
 * 计算一个标度为 0（零）的精确数值，
 * 从 1（一）到 n，见规范。
 */
Datum
window_ntile(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	ntile_context *context;

	context = (ntile_context *)
		WinGetPartitionLocalMemory(winobj, sizeof(ntile_context));

	if (context->ntile == 0)
	{
		/* 首次 */
		int64		total;
		int32		nbuckets;
		bool		isnull;

		total = WinGetPartitionRowCount(winobj);
		nbuckets = DatumGetInt32(WinGetFuncArgCurrent(winobj, 0, &isnull));

		/*
		 * 按规范：若 NT 是空值，则结果为空
		 * 值。
		 */
		if (isnull)
			PG_RETURN_NULL();

		/*
		 * 按规范：若 NT 小于等于 0（零），则
		 * 会引发异常条件。
		 */
		if (nbuckets <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ARGUMENT_FOR_NTILE),
					 errmsg("argument of ntile must be greater than zero")));

		context->ntile = 1;
		context->rows_per_bucket = 0;
		context->boundary = total / nbuckets;
		if (context->boundary <= 0)
			context->boundary = 1;
		else
		{
			/*
			 * 若不能整除，则给前面的桶各加
			 * 1 行。
			 */
			context->remainder = total % nbuckets;
			if (context->remainder != 0)
				context->boundary++;
		}
	}

	context->rows_per_bucket++;
	if (context->boundary < context->rows_per_bucket)
	{
		/* 递增 */
		if (context->remainder != 0 && context->ntile == context->remainder)
		{
			context->remainder = 0;
			context->boundary -= 1;
		}
		context->ntile += 1;
		context->rows_per_bucket = 1;
	}

	PG_RETURN_INT32(context->ntile);
}

/*
 * window_ntile_support
 *		window_ntile() 的支持函数
 */
Datum
window_ntile_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestWFuncMonotonic))
	{
		SupportRequestWFuncMonotonic *req = (SupportRequestWFuncMonotonic *) rawreq;
		WindowFunc *wfunc = req->window_func;

		if (list_length(wfunc->args) == 1)
		{
			Node *expr = eval_const_expressions(NULL, linitial(wfunc->args));

			/*
			 * 由于 v17 之前版本中 WindowClause runConditions 的
			 * Node 表示形式，必须坚持 ntile 参数为 Const，
			 * 以便安全地应用 runCondition 优化。
			 */
			if (IsA(expr, Const))
			{
				/*
				 * ntile() 单调递增，因为桶的数量在
				 * 首次调用之后不会改变
				 */
				req->monotonic = MONOTONICFUNC_INCREASING;
				PG_RETURN_POINTER(req);
			}
		}

		PG_RETURN_POINTER(NULL);
	}

	if (IsA(rawreq, SupportRequestOptimizeWindowClause))
	{
		SupportRequestOptimizeWindowClause *req = (SupportRequestOptimizeWindowClause *) rawreq;

		/*
		 * ntile() 不受帧选项影响。这里我们将帧选项设
		 * 成与 row_number 支持函数中相同的值。
		 * 使用 ROWS 而不是 RANGE（默认值）可让执行器免于
		 * 检查同级行。
		 */
		req->frameOptions = (FRAMEOPTION_NONDEFAULT |
							 FRAMEOPTION_ROWS |
							 FRAMEOPTION_START_UNBOUNDED_PRECEDING |
							 FRAMEOPTION_END_CURRENT_ROW);

		PG_RETURN_POINTER(req);
	}

	PG_RETURN_POINTER(NULL);
}

/*
 * leadlag_common
 * lead() 与 lag() 的公共操作
 * 对 lead() forward 为 true，而对 lag() 则为 false。
 * withoffset 表示有第二个偏移量参数。
 * withdefault 表示有第三个默认值参数。
 */
static Datum
leadlag_common(FunctionCallInfo fcinfo,
			   bool forward, bool withoffset, bool withdefault)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	int32		offset;
	bool		const_offset;
	Datum		result;
	bool		isnull;
	bool		isout;

	if (withoffset)
	{
		offset = DatumGetInt32(WinGetFuncArgCurrent(winobj, 1, &isnull));
		if (isnull)
			PG_RETURN_NULL();
		const_offset = get_fn_expr_arg_stable(fcinfo->flinfo, 1);
	}
	else
	{
		offset = 1;
		const_offset = true;
	}

	result = WinGetFuncArgInPartition(winobj, 0,
									  (forward ? offset : -offset),
									  WINDOW_SEEK_CURRENT,
									  const_offset,
									  &isnull, &isout);

	if (isout)
	{
		/*
		 * 目标行超出分区；若给定了默认值则用它，
		 * 否则将保持为 NULL
		 */
		if (withdefault)
			result = WinGetFuncArgCurrent(winobj, 2, &isnull);
	}

	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

/*
 * lag
 * 返回对分区内当前行之前第 1 行求值的
 * VE 值，
 * 按规范
 */
Datum
window_lag(PG_FUNCTION_ARGS)
{
	return leadlag_common(fcinfo, false, false, false);
}

/*
 * lag_with_offset
 * 返回对分区内当前行之前第 OFFSET 行求值的
 * VE 值，
 * 按规范
 */
Datum
window_lag_with_offset(PG_FUNCTION_ARGS)
{
	return leadlag_common(fcinfo, false, true, false);
}

/*
 * lag_with_offset_and_default
 * 与 lag_with_offset 相同，但接受默认值
 * 作为第三参数。
 */
Datum
window_lag_with_offset_and_default(PG_FUNCTION_ARGS)
{
	return leadlag_common(fcinfo, false, true, true);
}

/*
 * lead
 * 返回对分区内当前行之后第 1 行求值的
 * VE 值，
 * 按规范
 */
Datum
window_lead(PG_FUNCTION_ARGS)
{
	return leadlag_common(fcinfo, true, false, false);
}

/*
 * lead_with_offset
 * 返回对分区内当前行之后第 OFFSET 行求值的
 * VE 值，
 * 按规范
 */
Datum
window_lead_with_offset(PG_FUNCTION_ARGS)
{
	return leadlag_common(fcinfo, true, true, false);
}

/*
 * lead_with_offset_and_default
 * 与 lead_with_offset 相同，但接受默认值
 * 作为第三参数。
 */
Datum
window_lead_with_offset_and_default(PG_FUNCTION_ARGS)
{
	return leadlag_common(fcinfo, true, true, true);
}

/*
 * first_value
 * 返回对窗口帧第一行求值的
 * VE 值，按规范。
 */
Datum
window_first_value(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	Datum		result;
	bool		isnull;

	result = WinGetFuncArgInFrame(winobj, 0,
								  0, WINDOW_SEEK_HEAD, true,
								  &isnull, NULL);
	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

/*
 * last_value
 * 返回对窗口帧最后一行求值的
 * VE 值，按规范。
 */
Datum
window_last_value(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	Datum		result;
	bool		isnull;

	result = WinGetFuncArgInFrame(winobj, 0,
								  0, WINDOW_SEEK_TAIL, true,
								  &isnull, NULL);
	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}

/*
 * nth_value
 * 返回对从窗口帧第一行起的第 n 行求值的
 * VE 值，按规范。
 */
Datum
window_nth_value(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	bool		const_offset;
	Datum		result;
	bool		isnull;
	int32		nth;

	nth = DatumGetInt32(WinGetFuncArgCurrent(winobj, 1, &isnull));
	if (isnull)
		PG_RETURN_NULL();
	const_offset = get_fn_expr_arg_stable(fcinfo->flinfo, 1);

	if (nth <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_NTH_VALUE),
				 errmsg("argument of nth_value must be greater than zero")));

	result = WinGetFuncArgInFrame(winobj, 0,
								  nth - 1, WINDOW_SEEK_HEAD, const_offset,
								  &isnull, NULL);
	if (isnull)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(result);
}
