/*-------------------------------------------------------------------------
 *
 * prepagg.c
 *	  预处理聚合函数调用的例程
 *
 * 若查询中有相同的聚合调用，它们只需要
 * 被计算一次。另外，有些聚合函数可以共享同一个
 * 转换状态，于是我们只需分别为它们调用最终函数。
 * 这些处理与聚合如何执行
 * 无关。
 *
 * preprocess_aggrefs() 检测这些情况，为每个需要
 * 计算的聚合和转换状态创建 AggInfo 与
 * AggTransInfo 结构，并相应地设置 Aggrefs 中的 'aggno' 与 'transno' 字段。
 * 它也解析多态转换类型，并相应地设置
 * 'aggtranstype' 字段。
 *
 * XXX：规划结束后 AggInfo 与 AggTransInfo 结构就被丢掉，
 * 因此执行器启动时不得不做一些与我们这里相同的查找，
 * 去找转换函数和初值。将来我们也许
 * 想把这些信息带到 Agg 节点上，以省掉
 * 执行器启动时的这番工夫。不过 Agg 节点要到规划的
 * 很后面才构造，所以这并不简单。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepagg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "parser/parse_agg.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static bool preprocess_aggrefs_walker(Node *node, PlannerInfo *root);
static int	find_compatible_agg(PlannerInfo *root, Aggref *newagg,
								List **same_input_transnos);
static int	find_compatible_trans(PlannerInfo *root, Aggref *newagg,
								  bool shareable,
								  Oid aggtransfn, Oid aggtranstype,
								  int transtypeLen, bool transtypeByVal,
								  Oid aggcombinefn,
								  Oid aggserialfn, Oid aggdeserialfn,
								  Datum initValue, bool initValueIsNull,
								  List *transnos);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);

/* -----------------
 * 解析全部 Aggref 的转换类型，并判断哪些 Aggref
 * 可以共享聚合或转换状态。
 *
 * 关于聚合和转换函数的信息收集在
 * root->agginfos 和 root->aggtransinfos 列表里。每个 Aggref 的 'aggtranstype'、
 * 'aggno' 和 'aggtransno' 字段会被填上。
 *
 * 注意：这会就地修改输入表达式里的 Aggref！
 *
 * 我们试着通过检测重复的聚合函数来优化，使
 * 它们的状态和最终值被复用，而不是无谓地
 * 被独立重新计算。我们也检测并不
 * 相同、但可以共享同一转换状态的聚合。
 *
 * 情形：
 *
 * 1. 查询里出现相同的聚合函数调用：
 *
 *	    SELECT SUM(x) FROM ... HAVING SUM(x) > 0
 *
 *	    由于这些聚合相同，我们只需要计算
 *	    一次值。两个聚合会共享同一个 'aggno' 值。
 *
 * 2. 查询里出现两个不同的聚合函数，但这些
 *	    聚合有相同的参数、转换函数和
 *	    初始值（并且想必有不同的最终函数）：
 *
 *	    SELECT AVG(x), STDDEV(x) FROM ...
 *
 *	    这种情况下必须为变化的聚合建立一个新的 AggInfo，
 *	    并且需要分别调用最终函数，但只需要
 *	    跑一次转换函数。（这需要最终函数
 *	    不破坏转换状态，但
 *	    反正出于其他原因也需要如此。）
 *
 * 要使这两种优化都有效，转换阶段使用的全部聚合属性
 * 都必须相同，包括任何修饰符，
 * 例如 ORDER BY、DISTINCT 和 FILTER，并且参数不得
 * 含有任何易变函数。
 * -----------------
 */
void
preprocess_aggrefs(PlannerInfo *root, Node *clause)
{
	(void) preprocess_aggrefs_walker(clause, root);
}

static void
preprocess_aggref(Aggref *aggref, PlannerInfo *root)
{
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggtransfn;
	Oid			aggfinalfn;
	Oid			aggcombinefn;
	Oid			aggserialfn;
	Oid			aggdeserialfn;
	Oid			aggtranstype;
	int32		aggtranstypmod;
	int32		aggtransspace;
	bool		shareable;
	int			aggno;
	int			transno;
	List	   *same_input_transnos;
	int16		resulttypeLen;
	bool		resulttypeByVal;
	Datum		textInitVal;
	Datum		initValue;
	bool		initValueIsNull;
	bool		transtypeByVal;
	int16		transtypeLen;
	Oid			inputTypes[FUNC_MAX_ARGS];
	int			numArguments;

	Assert(aggref->agglevelsup == 0);

	/*
	 * 从 pg_aggregate 取出关于该聚合的信息。注意忽略
	 * 移动聚合变体是对的，因为我们关心的
	 * 是聚合而不是窗口函数。
	 */
	aggTuple = SearchSysCache1(AGGFNOID,
							   ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u",
			 aggref->aggfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggtransfn = aggform->aggtransfn;
	aggfinalfn = aggform->aggfinalfn;
	aggcombinefn = aggform->aggcombinefn;
	aggserialfn = aggform->aggserialfn;
	aggdeserialfn = aggform->aggdeserialfn;
	aggtranstype = aggform->aggtranstype;
	aggtransspace = aggform->aggtransspace;

	/*
	 * 解析可能多态的聚合转换类型。
	 */

	/* 抽出参数类型（忽略任何 ORDER BY 表达式） */
	numArguments = get_aggregate_argtypes(aggref, inputTypes);

	/* 若多态，解析转换状态的实际类型 */
	aggtranstype = resolve_aggregate_transtype(aggref->aggfnoid,
											   aggtranstype,
											   inputTypes,
											   numArguments);
	aggref->aggtranstype = aggtranstype;

	/*
	 * 若转换状态与第一个被聚合的输入类型相同，就假定
	 * 它的 typmod（宽度）也相同。这对
	 * MAX/MIN 这类情形有效，其他情况下大概也还算合理。
	 */
	aggtranstypmod = -1;
	if (aggref->args)
	{
		TargetEntry *tle = (TargetEntry *) linitial(aggref->args);

		if (aggtranstype == exprType((Node *) tle->expr))
			aggtranstypmod = exprTypmod((Node *) tle->expr);
	}

	/*
	 * 若 finalfn 标成可读写，就不能共享转换状态；但
	 * 对 AGGMODIFY_SHAREABLE 的聚合，共享状态是可以的。
	 *
	 * 原则上，在部分聚合里，即使最终函数标成可读写，
	 * 我们也可以共享转换
	 * 状态，因为部分聚合不执行最终函数。但现在
	 * 还太早，不知道我们是否会做部分聚合。
	 */
	shareable = (aggform->aggfinalmodify != AGGMODIFY_READ_WRITE);

	/* 取得输出值数据类型的信息 */
	get_typlenbyval(aggref->aggtype,
					&resulttypeLen,
					&resulttypeByVal);

	/* 取得初始值 */
	textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
								  Anum_pg_aggregate_agginitval,
								  &initValueIsNull);
	if (initValueIsNull)
		initValue = (Datum) 0;
	else
		initValue = GetAggInitVal(textInitVal, aggtranstype);

	ReleaseSysCache(aggTuple);

	/*
	 * 1. 看这是否与我们已经见过的另一个聚合函数调用
	 * 相同。
	 */
	aggno = find_compatible_agg(root, aggref, &same_input_transnos);
	if (aggno != -1)
	{
		AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, aggno);

		agginfo->aggrefs = lappend(agginfo->aggrefs, aggref);
		transno = agginfo->transno;
	}
	else
	{
		AggInfo    *agginfo = makeNode(AggInfo);

		agginfo->finalfn_oid = aggfinalfn;
		agginfo->aggrefs = list_make1(aggref);
		agginfo->shareable = shareable;

		aggno = list_length(root->agginfos);
		root->agginfos = lappend(root->agginfos, agginfo);

		/*
		 * 计数，并检查需要有序输入的情形。注意
		 * 有序集聚合的 aggorder 总是非空。任何有序输入
		 * 情形也会使部分聚合失效。
		 */
		if (aggref->aggorder != NIL || aggref->aggdistinct != NIL)
		{
			root->numOrderedAggs++;
			root->hasNonPartialAggs = true;
		}

		get_typlenbyval(aggtranstype,
						&transtypeLen,
						&transtypeByVal);

		/*
		 * 2. 看这个聚合能否与另一个
		 * 我们已经初始化的聚合共享转换状态。
		 */
		transno = find_compatible_trans(root, aggref, shareable,
										aggtransfn, aggtranstype,
										transtypeLen, transtypeByVal,
										aggcombinefn,
										aggserialfn, aggdeserialfn,
										initValue, initValueIsNull,
										same_input_transnos);
		if (transno == -1)
		{
			AggTransInfo *transinfo = makeNode(AggTransInfo);

			transinfo->args = aggref->args;
			transinfo->aggfilter = aggref->aggfilter;
			transinfo->transfn_oid = aggtransfn;
			transinfo->combinefn_oid = aggcombinefn;
			transinfo->serialfn_oid = aggserialfn;
			transinfo->deserialfn_oid = aggdeserialfn;
			transinfo->aggtranstype = aggtranstype;
			transinfo->aggtranstypmod = aggtranstypmod;
			transinfo->transtypeLen = transtypeLen;
			transinfo->transtypeByVal = transtypeByVal;
			transinfo->aggtransspace = aggtransspace;
			transinfo->initValue = initValue;
			transinfo->initValueIsNull = initValueIsNull;

			transno = list_length(root->aggtransinfos);
			root->aggtransinfos = lappend(root->aggtransinfos, transinfo);

			/*
			 * 检查部分聚合是否可行，除非我们已经
			 * 发现做不到。
			 */
			if (!root->hasNonPartialAggs)
			{
				/*
				 * 若没有合并函数，则部分聚合
				 * 不可能。
				 */
				if (!OidIsValid(transinfo->combinefn_oid))
					root->hasNonPartialAggs = true;

				/*
				 * 若有任何 transtype 为 INTERNAL 的聚合，就必须
				 * 检查它们是否有序列化/反序列化
				 * 函数；若没有，就没法序列化部分聚合
				 * 结果。
				 */
				else if (transinfo->aggtranstype == INTERNALOID)
				{

					if (!OidIsValid(transinfo->serialfn_oid) ||
						!OidIsValid(transinfo->deserialfn_oid))
						root->hasNonSerialAggs = true;

					/*
					 * array_agg_serialize 和 array_agg_deserialize 使用
					 * 聚合的非传值输入类型的 send 和
					 * receive 函数。被聚合的类型
					 * 有可能缺少其中一个或两个这些
					 * 函数。这种情况下不得允许使用
					 * 该聚合的 serial 和 deserial 函数。
					 * 最好不要为此做特殊处理，而是
					 * 在聚合内部提供某种支撑函数
					 * 来做这件事，但目前，对这一个情形来说那似乎
					 * 是用力过猛。
					 */
					if ((transinfo->serialfn_oid == F_ARRAY_AGG_SERIALIZE ||
						 transinfo->deserialfn_oid == F_ARRAY_AGG_DESERIALIZE) &&
						!agg_args_support_sendreceive(aggref))
						root->hasNonSerialAggs = true;
				}
			}
		}
		agginfo->transno = transno;
	}

	/*
	 * 填上 Aggref 里的字段（aggtranstype 上面已经设过）
	 */
	aggref->aggno = aggno;
	aggref->aggtransno = transno;
}

static bool
preprocess_aggrefs_walker(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		preprocess_aggref(aggref, root);

		/*
		 * 我们假定解析器已经检查过被聚合的参数、直接参数
		 * 或过滤子句里没有（至少这一层的）聚合。
		 * 因此不必递归进其中任何一个。
		 */
		return false;
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, preprocess_aggrefs_walker,
								  (void *) root);
}


/*
 * find_compatible_agg - 搜索先前初始化的按聚合结构
 *
 * 在先前看过的聚合里搜索与这个兼容、
 * 且输入参数相同的一个。若找不到兼容的聚合，
 * 返回 -1。
 *
 * 作为副作用，这也收集一份输入匹配的、现有可共享按转换
 * 结构的列表。若找不到相同的 Aggref，这份列表以后
 * 会传给 find_compatible_trans，看至少能否复用
 * 另一个聚合的状态值。
 */
static int
find_compatible_agg(PlannerInfo *root, Aggref *newagg,
					List **same_input_transnos)
{
	ListCell   *lc;
	int			aggno;

	*same_input_transnos = NIL;

	/* 若该 aggref 含有易变函数调用，就不得复用它 */
	if (contain_volatile_functions((Node *) newagg))
		return -1;

	/*
	 * 搜索已经见过的聚合列表。若找到一个
	 * 现有的相同聚合调用，就可以复用那个。搜索时
	 * 也会收集一份输入参数相同的 Aggref 列表。
	 * 若找不到匹配的 Aggref，调用方仍可能
	 * 复用其中一个的转换状态。（在这个阶段我们
	 * 只比较解析树；不同聚合是否共享
	 * 同一转换函数要到以后再检查。）
	 */
	aggno = -1;
	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *existingRef;

		aggno++;

		existingRef = linitial_node(Aggref, agginfo->aggrefs);

		/* 下面这些都必须相同，否则就不算匹配 */
		if (newagg->inputcollid != existingRef->inputcollid ||
			newagg->aggtranstype != existingRef->aggtranstype ||
			newagg->aggstar != existingRef->aggstar ||
			newagg->aggvariadic != existingRef->aggvariadic ||
			newagg->aggkind != existingRef->aggkind ||
			!equal(newagg->args, existingRef->args) ||
			!equal(newagg->aggorder, existingRef->aggorder) ||
			!equal(newagg->aggdistinct, existingRef->aggdistinct) ||
			!equal(newagg->aggfilter, existingRef->aggfilter))
			continue;

		/* 若是同一个聚合函数，就报告精确匹配 */
		if (newagg->aggfnoid == existingRef->aggfnoid &&
			newagg->aggtype == existingRef->aggtype &&
			newagg->aggcollid == existingRef->aggcollid &&
			equal(newagg->aggdirectargs, existingRef->aggdirectargs))
		{
			list_free(*same_input_transnos);
			*same_input_transnos = NIL;
			return aggno;
		}

		/*
		 * 不相同，但输入相同。若最终函数
		 * 允许共享，就把其 transno 返回给调用方，以防我们可以
		 * 复用它的按转换状态。（若已经在共享，
		 * 我们也许会多次报告同一个 transno。find_compatible_trans 足够
		 * 便宜，不值得花周期去避免那个。）
		 */
		if (agginfo->shareable)
			*same_input_transnos = lappend_int(*same_input_transnos,
											   agginfo->transno);
	}

	return -1;
}

/*
 * find_compatible_trans - 搜索先前初始化的按转换
 * 结构
 *
 * 在 transno 列表里搜索转换函数和初始条件相同的按转换结构。
 * （输入已经
 * 验证过匹配。）
 */
static int
find_compatible_trans(PlannerInfo *root, Aggref *newagg, bool shareable,
					  Oid aggtransfn, Oid aggtranstype,
					  int transtypeLen, bool transtypeByVal,
					  Oid aggcombinefn,
					  Oid aggserialfn, Oid aggdeserialfn,
					  Datum initValue, bool initValueIsNull,
					  List *transnos)
{
	ListCell   *lc;

	/* 若这个聚合不能共享转换状态，就放弃 */
	if (!shareable)
		return -1;

	foreach(lc, transnos)
	{
		int			transno = lfirst_int(lc);
		AggTransInfo *pertrans = list_nth_node(AggTransInfo,
											   root->aggtransinfos,
											   transno);

		/*
		 * 若转换函数或转换状态类型不相同，则
		 * 状态不能共享。
		 */
		if (aggtransfn != pertrans->transfn_oid ||
			aggtranstype != pertrans->aggtranstype)
			continue;

		/*
		 * 序列化和反序列化函数若存在就必须匹配，
		 * 因为我们没法共享那些会序列化或反序列化成不同格式的聚合的
		 * 转换状态。
		 * 记住，若这个聚合节点不需要它们，这些将是
		 * InvalidOid。
		 */
		if (aggserialfn != pertrans->serialfn_oid ||
			aggdeserialfn != pertrans->deserialfn_oid)
			continue;

		/*
		 * 合并函数也必须匹配。我们只在部分聚合时关心合并
		 * 函数，但规划阶段现在还太早，
		 * 不知道会不会做部分聚合，因此
		 * 保守处理。
		 */
		if (aggcombinefn != pertrans->combinefn_oid)
			continue;

		/*
		 * 也检查初始条件是否匹配。
		 */
		if (initValueIsNull && pertrans->initValueIsNull)
			return transno;

		if (!initValueIsNull && !pertrans->initValueIsNull &&
			datumIsEqual(initValue, pertrans->initValue,
						 transtypeByVal, transtypeLen))
			return transno;
	}
	return -1;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}


/*
 * get_agg_clause_costs
 *	    处理 PlannerInfo 的 'aggtransinfos' 和 'agginfos' 列表，
 *	    累积关于它们的代价信息。
 *
 * 'aggsplit' 告诉我们预期的部分聚合模式，这会影响
 * 代价估计。
 *
 * 注意：代价是 ADDED 到 *costs 里已有的那些之上……因此调用方
 * 负责一开始把该结构体清零。
 *
 * 对每个 AggTransInfo，根据 'aggsplit' 的值使用 transfn 或 combinefn，
 * 加上一次聚合转换的代价。我们也
 * 计入任何 aggfilter 的代价，以及转换状态的任何序列化和
 * 反序列化，并估计所需的总空间，
 * 就好像每个聚合的状态都同时存放在
 * 内存里（像 HashAgg 计划那样）。
 *
 * 对 'agginfos' 列表里的每个 AggInfo，我们加上运行
 * 最终函数和直接参数（若有）的代价。
 */
void
get_agg_clause_costs(PlannerInfo *root, AggSplit aggsplit, AggClauseCosts *costs)
{
	ListCell   *lc;

	foreach(lc, root->aggtransinfos)
	{
		AggTransInfo *transinfo = lfirst_node(AggTransInfo, lc);

		/*
		 * 把适当的组成函数执行代价加到
		 * 适当的合计里。
		 */
		if (DO_AGGSPLIT_COMBINE(aggsplit))
		{
			/* 计入合并先前已聚合状态的费用 */
			add_function_cost(root, transinfo->combinefn_oid, NULL,
							  &costs->transCost);
		}
		else
			add_function_cost(root, transinfo->transfn_oid, NULL,
							  &costs->transCost);
		if (DO_AGGSPLIT_DESERIALIZE(aggsplit) &&
			OidIsValid(transinfo->deserialfn_oid))
			add_function_cost(root, transinfo->deserialfn_oid, NULL,
							  &costs->transCost);
		if (DO_AGGSPLIT_SERIALIZE(aggsplit) &&
			OidIsValid(transinfo->serialfn_oid))
			add_function_cost(root, transinfo->serialfn_oid, NULL,
							  &costs->finalCost);

		/*
		 * 这些代价只由最初的聚合节点承担，因此
		 * 不得在上层再次计入它们。
		 */
		if (!DO_AGGSPLIT_COMBINE(aggsplit))
		{
			/* 把输入表达式的代价加到每输入行代价里 */
			QualCost	argcosts;

			cost_qual_eval_node(&argcosts, (Node *) transinfo->args, root);
			costs->transCost.startup += argcosts.startup;
			costs->transCost.per_tuple += argcosts.per_tuple;

			/*
			 * 把任何过滤器的代价加到每输入行代价里。
			 *
			 * XXX 理想情况下应当按过滤器选择率减少输入表达式代价，
			 * 但不清楚是否
			 * 值得费这个劲。
			 */
			if (transinfo->aggfilter)
			{
				cost_qual_eval_node(&argcosts, (Node *) transinfo->aggfilter,
									root);
				costs->transCost.startup += argcosts.startup;
				costs->transCost.per_tuple += argcosts.per_tuple;
			}
		}

		/*
		 * 若转换类型是传值的，它就不会给
		 * 哈希表所需大小增加任何东西。若是
		 * 传引用的，就必须加上该值本身的估计大小，
		 * 外加 palloc 开销。
		 */
		if (!transinfo->transtypeByVal)
		{
			int32		avgwidth;

			/* 若聚合定义给了平均宽度，就用它 */
			if (transinfo->aggtransspace > 0)
				avgwidth = transinfo->aggtransspace;
			else if (transinfo->transfn_oid == F_ARRAY_APPEND)
			{
				/*
				 * 若转换函数是 array_append()，它会用
				 * 展开数组作为 transvalue，那至少会占用
				 * ALLOCSET_SMALL_INITSIZE，也许更多。在没有更好主意时，用那个作为
				 * 估计。
				 */
				avgwidth = ALLOCSET_SMALL_INITSIZE;
			}
			else
			{
				avgwidth = get_typavgwidth(transinfo->aggtranstype, transinfo->aggtranstypmod);
			}

			avgwidth = MAXALIGN(avgwidth);
			costs->transitionSpace += avgwidth + 2 * sizeof(void *);
		}
		else if (transinfo->aggtranstype == INTERNALOID)
		{
			/*
			 * INTERNAL 转换类型是特例：虽然 INTERNAL
			 * 是传值的，它几乎肯定被用作指向
			 * 某个大数据结构的指针。聚合定义可以
			 * 提供大小估计。若没有，我们就假定
			 * ALLOCSET_DEFAULT_INITSIZE，若数据保存在
			 * 私有内存上下文里，这是个不错的猜测，例如
			 * array_agg() 就是这么做的。
			 */
			if (transinfo->aggtransspace > 0)
				costs->transitionSpace += transinfo->aggtransspace;
			else
				costs->transitionSpace += ALLOCSET_DEFAULT_INITSIZE;
		}
	}

	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);

		/*
		 * 把适当的组成函数执行代价加到
		 * 适当的合计里。
		 */
		if (!DO_AGGSPLIT_SKIPFINAL(aggsplit) &&
			OidIsValid(agginfo->finalfn_oid))
			add_function_cost(root, agginfo->finalfn_oid, NULL,
							  &costs->finalCost);

		/*
		 * 若有直接参数，就把它们的求值代价当作
		 * finalfn 的代价来处理。
		 */
		if (aggref->aggdirectargs)
		{
			QualCost	argcosts;

			cost_qual_eval_node(&argcosts, (Node *) aggref->aggdirectargs,
								root);
			costs->finalCost.startup += argcosts.startup;
			costs->finalCost.per_tuple += argcosts.per_tuple;
		}
	}
}
