/*-------------------------------------------------------------------------
 *
 * extended_stats.c
 *	  POSTGRES extended statistics
 *
 * Generic code supporting statistics objects created via CREATE STATISTICS.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/extended_stats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "executor/executor.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * 为避免分析期间消耗过多内存，以及/或者在得到的
 * pg_statistic 行里占用过多空间，我们忽略去 toast 之后宽于
 * WIDTH_THRESHOLD 的 varlena datum。对 MCV
 * 和不同值计算这是合理的，因为很宽的值不太可能
 * 被重复，更不太会是最常见值。出于同样的理由，
 * 忽略很宽的值不会太影响我们对直方图桶
 * 边界的估计。
 */
#define WIDTH_THRESHOLD  1024

/*
 * 内部用来指代单个统计对象，即
 * 一条 pg_statistic_ext 项。
 */
typedef struct StatExtEntry
{
	Oid			statOid;		/* pg_statistic_ext 项的 OID */
	char	   *schema;			/* 统计对象的模式 */
	char	   *name;			/* 统计对象的名字 */
	Bitmapset  *columns;		/* 该对象覆盖的属性号 */
	List	   *types;			/* 已启用统计种类的 char 列表 */
	int			stattarget;		/* 统计目标（-1 表示默认） */
	List	   *exprs;			/* 表达式 */
} StatExtEntry;


static List *fetch_statentries_for_relation(Relation pg_statext, Oid relid);
static VacAttrStats **lookup_var_attr_stats(Relation rel, Bitmapset *attrs, List *exprs,
											int nvacatts, VacAttrStats **vacatts);
static void statext_store(Oid statOid, bool inh,
						  MVNDistinct *ndistinct, MVDependencies *dependencies,
						  MCVList *mcv, Datum exprs, VacAttrStats **stats);
static int	statext_compute_stattarget(int stattarget,
									   int nattrs, VacAttrStats **stats);

/* 分析单个简单表达式所需的信息。 */
typedef struct AnlExprData
{
	Node	   *expr;			/* 要分析的表达式 */
	VacAttrStats *vacattrstat;	/* 要分析的统计属性 */
} AnlExprData;

static void compute_expr_stats(Relation onerel, double totalrows,
							   AnlExprData *exprdata, int nexprs,
							   HeapTuple *rows, int numrows);
static Datum serialize_expr_stats(AnlExprData *exprdata, int nexprs);
static Datum expr_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);
static AnlExprData *build_expr_data(List *exprs, int stattarget);

static StatsBuildData *make_build_data(Relation rel, StatExtEntry *stat,
									   int numrows, HeapTuple *rows,
									   VacAttrStats **stats, int stattarget);


/*
 * 计算所请求的扩展统计，使用为普通
 * （单列）统计采样的行。
 *
 * 这从 pg_statistic_ext 取出统计类型列表，计算
 * 所请求的统计，再把它们序列化写回系统目录。
 */
void
BuildRelationExtStatistics(Relation onerel, bool inh, double totalrows,
						   int numrows, HeapTuple *rows,
						   int natts, VacAttrStats **vacattrstats)
{
	Relation	pg_stext;
	ListCell   *lc;
	List	   *statslist;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int64		ext_cnt;

	/* 若没有要分析的列，就什么也不做。 */
	if (!natts)
		return;

	/* 统计列表必须在该内存上下文之外分配 */
	pg_stext = table_open(StatisticExtRelationId, RowExclusiveLock);
	statslist = fetch_statentries_for_relation(pg_stext, RelationGetRelid(onerel));

	/* 构造每个统计对象所用的内存上下文 */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"BuildRelationExtStatistics",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/* 报告这一阶段 */
	if (statslist != NIL)
	{
		const int	index[] = {
			PROGRESS_ANALYZE_PHASE,
			PROGRESS_ANALYZE_EXT_STATS_TOTAL
		};
		const int64 val[] = {
			PROGRESS_ANALYZE_PHASE_COMPUTE_EXT_STATS,
			list_length(statslist)
		};

		pgstat_progress_update_multi_param(2, index, val);
	}

	ext_cnt = 0;
	foreach(lc, statslist)
	{
		StatExtEntry *stat = (StatExtEntry *) lfirst(lc);
		MVNDistinct *ndistinct = NULL;
		MVDependencies *dependencies = NULL;
		MCVList    *mcv = NULL;
		Datum		exprstats = (Datum) 0;
		VacAttrStats **stats;
		ListCell   *lc2;
		int			stattarget;
		StatsBuildData *data;

		/*
		 * 检查能否基于已分析的列来建立这些统计。若
		 * 不能，就报告这一事实（autovacuum 中除外）并继续。
		 */
		stats = lookup_var_attr_stats(onerel, stat->columns, stat->exprs,
									  natts, vacattrstats);
		if (!stats)
		{
			if (!IsAutoVacuumWorkerProcess())
				ereport(WARNING,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("statistics object \"%s.%s\" could not be computed for relation \"%s.%s\"",
								stat->schema, stat->name,
								get_namespace_name(onerel->rd_rel->relnamespace),
								RelationGetRelationName(onerel)),
						 errtable(onerel)));
			continue;
		}

		/* 计算该统计对象的统计目标 */
		stattarget = statext_compute_stattarget(stat->stattarget,
												bms_num_members(stat->columns),
												stats);

		/*
		 * 不要重建统计目标被设为 0 的统计对象
		 * （我们就把现有值留在那里，就像对
		 * 普通的按列统计所做的那样）。
		 */
		if (stattarget == 0)
			continue;

		/* 对表达式求值（若该统计对象有表达式） */
		data = make_build_data(onerel, stat, numrows, rows, stats, stattarget);

		/* 计算每种所请求类型的统计 */
		foreach(lc2, stat->types)
		{
			char		t = (char) lfirst_int(lc2);

			if (t == STATS_EXT_NDISTINCT)
				ndistinct = statext_ndistinct_build(totalrows, data);
			else if (t == STATS_EXT_DEPENDENCIES)
				dependencies = statext_dependencies_build(data);
			else if (t == STATS_EXT_MCV)
				mcv = statext_mcv_build(data, totalrows, stattarget);
			else if (t == STATS_EXT_EXPRESSIONS)
			{
				AnlExprData *exprdata;
				int			nexprs;

				/* 不应当发生，这要归功于定义统计时的检查 */
				if (!stat->exprs)
					elog(ERROR, "requested expression stats, but there are no expressions");

				exprdata = build_expr_data(stat->exprs, stattarget);
				nexprs = list_length(stat->exprs);

				compute_expr_stats(onerel, totalrows,
								   exprdata, nexprs,
								   rows, numrows);

				exprstats = serialize_expr_stats(exprdata, nexprs);
			}
		}

		/* 把统计信息存入系统目录 */
		statext_store(stat->statOid, inh,
					  ndistinct, dependencies, mcv, exprstats, stats);

		/* 用于报告进度 */
		pgstat_progress_update_param(PROGRESS_ANALYZE_EXT_STATS_COMPUTED,
									 ++ext_cnt);

		/* 释放构造该统计对象所用的数据 */
		MemoryContextReset(cxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	list_free(statslist);

	table_close(pg_stext, RowExclusiveLock);
}

/*
 * ComputeExtStatisticsRows
 *		计算表上扩展统计所需的行数。
 *
 * 计算为了在表上建立扩展统计而需要采样的
 * 行数。这里只看我们实际能建立的统计信息。例如只分析
 * 部分列时，会跳过需要额外列的
 * 统计对象。
 *
 * 关于如何计算统计对象的统计目标（由对象目标、
 * 属性目标和默认统计目标得出），详见
 * statext_compute_stattarget。
 */
int
ComputeExtStatisticsRows(Relation onerel,
						 int natts, VacAttrStats **vacattrstats)
{
	Relation	pg_stext;
	ListCell   *lc;
	List	   *lstats;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int			result = 0;

	/* 若没有要分析的列，就返回 0。 */
	if (!natts)
		return 0;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"ComputeExtStatisticsRows",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	pg_stext = table_open(StatisticExtRelationId, RowExclusiveLock);
	lstats = fetch_statentries_for_relation(pg_stext, RelationGetRelid(onerel));

	foreach(lc, lstats)
	{
		StatExtEntry *stat = (StatExtEntry *) lfirst(lc);
		int			stattarget;
		VacAttrStats **stats;
		int			nattrs = bms_num_members(stat->columns);

		/*
		 * 检查能否基于已分析的列来建立该统计对象。
		 * 若不能，就忽略它（这里不报告，实际建立时会在
		 * BuildRelationExtStatistics 里报告）。
		 */
		stats = lookup_var_attr_stats(onerel, stat->columns, stat->exprs,
									  natts, vacattrstats);

		if (!stats)
			continue;

		/*
		 * 根据统计对象本身及其属性上设置的内容，
		 * 计算统计目标。
		 */
		stattarget = statext_compute_stattarget(stat->stattarget,
												nattrs, stats);

		/* 对所有统计对象使用最大的那个值。 */
		if (stattarget > result)
			result = stattarget;
	}

	table_close(pg_stext, RowExclusiveLock);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	/* 根据统计目标计算样本大小 */
	return (300 * result);
}

/*
 * statext_compute_stattarget
 *		计算一条扩展统计的统计目标。
 *
 * 计算扩展统计对象的目标时，我们考虑三个
 * 可能设置目标的地方：统计对象本身、
 * 该统计对象所定义于的属性，然后是默认
 * 统计目标。
 *
 * 首先看统计对象本身设置了什么，用的是
 * ALTER STATISTICS ... SET STATISTICS 命令。若在那里找到有效值
 * （即不是 -1），就完成了。否则看该统计所定义于的
 * 任何属性上设置的目标，若有些列
 * 定义了目标，就用最大值。我们这样做主要是为了
 * 向后兼容，因为在扩展统计有自己的
 * 统计目标之前，就是这么做的。
 *
 * 最后，若仍然没有统计目标，就用
 * default_statistics_target 里设置的值。
 */
static int
statext_compute_stattarget(int stattarget, int nattrs, VacAttrStats **stats)
{
	int			i;

	/*
	 * 若统计对象设置了统计目标，就用它。它
	 * 可能被设为 0，从而禁用该统计的建立。
	 */
	if (stattarget >= 0)
		return stattarget;

	/*
	 * 统计对象的目标被设为 -1，那种情况下我们
	 * 看该对象所定义于的任何属性上设置的
	 * 最大目标。
	 */
	for (i = 0; i < nattrs; i++)
	{
		/* 保留最大的统计目标 */
		if (stats[i]->attr->attstattarget > stattarget)
			stattarget = stats[i]->attr->attstattarget;
	}

	/*
	 * 若该值仍然为负（统计对象和
	 * 任何列都没有设置自定义统计目标），就用全局
	 * 默认目标。
	 */
	if (stattarget < 0)
		stattarget = default_statistics_target;

	/* 至此我们应当有一个有效的统计目标。 */
	Assert((stattarget >= 0) && (stattarget <= 10000));

	return stattarget;
}

/*
 * statext_is_kind_built
 *		给定的 pg_statistic_ext_data 元组里是否已经建立了这种统计？
 */
bool
statext_is_kind_built(HeapTuple htup, char type)
{
	AttrNumber	attnum;

	switch (type)
	{
		case STATS_EXT_NDISTINCT:
			attnum = Anum_pg_statistic_ext_data_stxdndistinct;
			break;

		case STATS_EXT_DEPENDENCIES:
			attnum = Anum_pg_statistic_ext_data_stxddependencies;
			break;

		case STATS_EXT_MCV:
			attnum = Anum_pg_statistic_ext_data_stxdmcv;
			break;

		case STATS_EXT_EXPRESSIONS:
			attnum = Anum_pg_statistic_ext_data_stxdexpr;
			break;

		default:
			elog(ERROR, "unexpected statistics type requested: %d", type);
	}

	return !heap_attisnull(htup, attnum, NULL);
}

/*
 * 返回给定关系的统计对象列表（StatExtEntry）。
 */
static List *
fetch_statentries_for_relation(Relation pg_statext, Oid relid)
{
	SysScanDesc scan;
	ScanKeyData skey;
	HeapTuple	htup;
	List	   *result = NIL;

	/*
	 * 准备扫描 pg_statistic_ext，查找 stxrelid 等于该
	 * 关系的项。
	 */
	ScanKeyInit(&skey,
				Anum_pg_statistic_ext_stxrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(pg_statext, StatisticExtRelidIndexId, true,
							  NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		StatExtEntry *entry;
		Datum		datum;
		bool		isnull;
		int			i;
		ArrayType  *arr;
		char	   *enabled;
		Form_pg_statistic_ext staForm;
		List	   *exprs = NIL;

		entry = palloc0(sizeof(StatExtEntry));
		staForm = (Form_pg_statistic_ext) GETSTRUCT(htup);
		entry->statOid = staForm->oid;
		entry->schema = get_namespace_name(staForm->stxnamespace);
		entry->name = pstrdup(NameStr(staForm->stxname));
		entry->stattarget = staForm->stxstattarget;
		for (i = 0; i < staForm->stxkeys.dim1; i++)
		{
			entry->columns = bms_add_member(entry->columns,
											staForm->stxkeys.values[i]);
		}

		/* 把 stxkind 字符数组解码成字符列表 */
		datum = SysCacheGetAttrNotNull(STATEXTOID, htup,
									   Anum_pg_statistic_ext_stxkind);
		arr = DatumGetArrayTypeP(datum);
		if (ARR_NDIM(arr) != 1 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "stxkind is not a 1-D char array");
		enabled = (char *) ARR_DATA_PTR(arr);
		for (i = 0; i < ARR_DIMS(arr)[0]; i++)
		{
			Assert((enabled[i] == STATS_EXT_NDISTINCT) ||
				   (enabled[i] == STATS_EXT_DEPENDENCIES) ||
				   (enabled[i] == STATS_EXT_MCV) ||
				   (enabled[i] == STATS_EXT_EXPRESSIONS));
			entry->types = lappend_int(entry->types, (int) enabled[i]);
		}

		/* 解码表达式（若有） */
		datum = SysCacheGetAttr(STATEXTOID, htup,
								Anum_pg_statistic_ext_stxexprs, &isnull);

		if (!isnull)
		{
			char	   *exprsString;

			exprsString = TextDatumGetCString(datum);
			exprs = (List *) stringToNode(exprsString);

			pfree(exprsString);

			/*
			 * 把这些表达式送过 eval_const_expressions。这不只是
			 * 优化，而且是必要的，因为规划器
			 * 会把它们与同样处理过的限定子句比较，若没有这一步
			 * 可能检测不出有效匹配。然而我们不得使用
			 * canonicalize_qual，因为这些并不是限定
			 * 表达式。
			 */
			exprs = (List *) eval_const_expressions(NULL, (Node *) exprs);

			/* 不妨也修正 opfuncid */
			fix_opfuncids((Node *) exprs);
		}

		entry->exprs = exprs;

		result = lappend(result, entry);
	}

	systable_endscan(scan);

	return result;
}

/*
 * examine_attribute：对单个列做分析前检查。
 *
 * 判断该列是否可分析；若可以，就为它创建并初始化
 * 一个 VacAttrStats 结构。若不可以，就返回 NULL。
 */
static VacAttrStats *
examine_attribute(Node *expr)
{
	HeapTuple	typtuple;
	VacAttrStats *stats;
	int			i;
	bool		ok;

	/*
	 * 创建 VacAttrStats 结构。注意我们只有
	 * pg_attribute 元组固定成员的一份副本。
	 */
	stats = (VacAttrStats *) palloc0(sizeof(VacAttrStats));

	/* 伪造该属性 */
	stats->attr = (Form_pg_attribute) palloc0(ATTRIBUTE_FIXED_PART_SIZE);
	stats->attr->attstattarget = -1;

	/*
	 * 分析表达式时，相信表达式树的类型，而不是
	 * 列数据类型：后者可能是操作符类的 opckeytype 存储类型，
	 * 对我们的目的没有意义。（注意：若
	 * 我们对非表达式的统计列做任何事，就需要
	 * 弄清从哪里取得正确的类型信息，但目前这
	 * 不是问题。）不清楚是否有人在意
	 * typmod，但为防万一我们也把它存下来。
	 */
	stats->attrtypid = exprType(expr);
	stats->attrtypmod = exprTypmod(expr);
	stats->attrcollid = exprCollation(expr);

	typtuple = SearchSysCacheCopy1(TYPEOID,
								   ObjectIdGetDatum(stats->attrtypid));
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup failed for type %u", stats->attrtypid);
	stats->attrtype = (Form_pg_type) GETSTRUCT(typtuple);

	/*
	 * 我们实际上不分析单个属性，因此不必设置
	 * 内存上下文。
	 */
	stats->anl_context = NULL;
	stats->tupattnum = InvalidAttrNumber;

	/*
	 * 描述 stats->stavalues[n] 元素类型的成员默认是
	 * 被分析数据的类型，但类型专用的 typanalyze
	 * 函数若想存储别的东西，可以改掉它们。
	 */
	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		stats->statypid[i] = stats->attrtypid;
		stats->statyplen[i] = stats->attrtype->typlen;
		stats->statypbyval[i] = stats->attrtype->typbyval;
		stats->statypalign[i] = stats->attrtype->typalign;
	}

	/*
	 * 调用类型专用的 typanalyze 函数。若没有指定，就用
	 * std_typanalyze()。
	 */
	if (OidIsValid(stats->attrtype->typanalyze))
		ok = DatumGetBool(OidFunctionCall1(stats->attrtype->typanalyze,
										   PointerGetDatum(stats)));
	else
		ok = std_typanalyze(stats);

	if (!ok || stats->compute_stats == NULL || stats->minrows <= 0)
	{
		heap_freetuple(typtuple);
		pfree(stats->attr);
		pfree(stats);
		return NULL;
	}

	return stats;
}

/*
 * examine_expression：对单个表达式做分析前检查。
 *
 * 判断该表达式是否可分析；若可以，就为它创建并初始化
 * 一个 VacAttrStats 结构。若不可以，就返回 NULL。
 */
static VacAttrStats *
examine_expression(Node *expr, int stattarget)
{
	HeapTuple	typtuple;
	VacAttrStats *stats;
	int			i;
	bool		ok;

	Assert(expr != NULL);

	/*
	 * 创建 VacAttrStats 结构。
	 */
	stats = (VacAttrStats *) palloc0(sizeof(VacAttrStats));

	/*
	 * 分析表达式时，相信表达式树的类型。
	 */
	stats->attrtypid = exprType(expr);
	stats->attrtypmod = exprTypmod(expr);

	/*
	 * CREATE STATISTICS 不允许指定排序规则，因此我们
	 * 必须使用表达式所指定的排序规则。可以在表达式
	 * "(col COLLATE "en_US")" 里指定排序规则，
	 * 那种情况下 exprCollation() 会做正确的事。
	 */
	stats->attrcollid = exprCollation(expr);

	/*
	 * 表达式没有 pg_attribute，因此我们往 attstattarget 里伪造一些
	 * 合理的东西，这是 std_typanalyze
	 * 唯一需要的。
	 */
	stats->attr = (Form_pg_attribute) palloc(ATTRIBUTE_FIXED_PART_SIZE);

	/*
	 * 表达式不能指定统计目标，因此我们可以
	 * 用 default_statistics_target，或者用为扩展统计
	 * 算出的目标。第二个选择似乎更合理。
	 */
	stats->attr->attstattarget = stattarget;

	/* 初始化一些基本成员 */
	stats->attr->attrelid = InvalidOid;
	stats->attr->attnum = InvalidAttrNumber;
	stats->attr->atttypid = stats->attrtypid;

	typtuple = SearchSysCacheCopy1(TYPEOID,
								   ObjectIdGetDatum(stats->attrtypid));
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup failed for type %u", stats->attrtypid);

	stats->attrtype = (Form_pg_type) GETSTRUCT(typtuple);
	stats->anl_context = CurrentMemoryContext;	/* XXX 应当使用
												 * 别的东西吗？ */
	stats->tupattnum = InvalidAttrNumber;

	/*
	 * 描述 stats->stavalues[n] 元素类型的成员默认是
	 * 被分析数据的类型，但类型专用的 typanalyze
	 * 函数若想存储别的东西，可以改掉它们。
	 */
	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		stats->statypid[i] = stats->attrtypid;
		stats->statyplen[i] = stats->attrtype->typlen;
		stats->statypbyval[i] = stats->attrtype->typbyval;
		stats->statypalign[i] = stats->attrtype->typalign;
	}

	/*
	 * 调用类型专用的 typanalyze 函数。若没有指定，就用
	 * std_typanalyze()。
	 */
	if (OidIsValid(stats->attrtype->typanalyze))
		ok = DatumGetBool(OidFunctionCall1(stats->attrtype->typanalyze,
										   PointerGetDatum(stats)));
	else
		ok = std_typanalyze(stats);

	if (!ok || stats->compute_stats == NULL || stats->minrows <= 0)
	{
		heap_freetuple(typtuple);
		pfree(stats);
		return NULL;
	}

	return stats;
}

/*
 * 以大小为 nvacatts 的 vacatts 作为输入数据，返回一个新建的
 * VacAttrStats 数组，其中只含与
 * attrs 所指示属性对应的项。若我们没有计算扩展统计所需的
 * 全部按列统计，就返回 NULL，
 * 向调用者表明不应当建立这些统计。
 */
static VacAttrStats **
lookup_var_attr_stats(Relation rel, Bitmapset *attrs, List *exprs,
					  int nvacatts, VacAttrStats **vacatts)
{
	int			i = 0;
	int			x = -1;
	int			natts;
	VacAttrStats **stats;
	ListCell   *lc;

	natts = bms_num_members(attrs) + list_length(exprs);

	stats = (VacAttrStats **) palloc(natts * sizeof(VacAttrStats *));

	/* 查找所请求列的 VacAttrStats 信息（相同的 attnum） */
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		int			j;

		stats[i] = NULL;
		for (j = 0; j < nvacatts; j++)
		{
			if (x == vacatts[j]->tupattnum)
			{
				stats[i] = vacatts[j];
				break;
			}
		}

		if (!stats[i])
		{
			/*
			 * 看起来某一列没有收集到统计。
			 * 没有这一列，我们将无法建立
			 * 扩展统计。
			 */
			pfree(stats);
			return NULL;
		}

		/*
		 * 健全性检查：该列没有被删除。那种情况下
		 * 统计应当已经被去掉。
		 */
		Assert(!stats[i]->attr->attisdropped);

		i++;
	}

	/* 同时加入表达式的信息 */
	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		stats[i] = examine_attribute(expr);

		/*
		 * XXX 我们稍后需要元组描述符，而代码只是从
		 * stats[0]->tupDesc 取它（例如见 statext_mcv_build）。但按现有写法
		 * examine_attribute 并不设置它，因此就从第一个
		 * vacatts 元素取。
		 */
		stats[i]->tupDesc = vacatts[0]->tupDesc;

		i++;
	}

	return stats;
}

/*
 * statext_store
 *	把统计信息序列化，并存入 pg_statistic_ext_data
 *	元组。
 */
static void
statext_store(Oid statOid, bool inh,
			  MVNDistinct *ndistinct, MVDependencies *dependencies,
			  MCVList *mcv, Datum exprs, VacAttrStats **stats)
{
	Relation	pg_stextdata;
	HeapTuple	stup;
	Datum		values[Natts_pg_statistic_ext_data];
	bool		nulls[Natts_pg_statistic_ext_data];

	pg_stextdata = table_open(StatisticExtDataRelationId, RowExclusiveLock);

	memset(nulls, true, sizeof(nulls));
	memset(values, 0, sizeof(values));

	/* 基本信息 */
	values[Anum_pg_statistic_ext_data_stxoid - 1] = ObjectIdGetDatum(statOid);
	nulls[Anum_pg_statistic_ext_data_stxoid - 1] = false;

	values[Anum_pg_statistic_ext_data_stxdinherit - 1] = BoolGetDatum(inh);
	nulls[Anum_pg_statistic_ext_data_stxdinherit - 1] = false;

	/*
	 * 构造一个新的 pg_statistic_ext_data 元组，替换计算出的
	 * 统计。
	 */
	if (ndistinct != NULL)
	{
		bytea	   *data = statext_ndistinct_serialize(ndistinct);

		nulls[Anum_pg_statistic_ext_data_stxdndistinct - 1] = (data == NULL);
		values[Anum_pg_statistic_ext_data_stxdndistinct - 1] = PointerGetDatum(data);
	}

	if (dependencies != NULL)
	{
		bytea	   *data = statext_dependencies_serialize(dependencies);

		nulls[Anum_pg_statistic_ext_data_stxddependencies - 1] = (data == NULL);
		values[Anum_pg_statistic_ext_data_stxddependencies - 1] = PointerGetDatum(data);
	}
	if (mcv != NULL)
	{
		bytea	   *data = statext_mcv_serialize(mcv, stats);

		nulls[Anum_pg_statistic_ext_data_stxdmcv - 1] = (data == NULL);
		values[Anum_pg_statistic_ext_data_stxdmcv - 1] = PointerGetDatum(data);
	}
	if (exprs != (Datum) 0)
	{
		nulls[Anum_pg_statistic_ext_data_stxdexpr - 1] = false;
		values[Anum_pg_statistic_ext_data_stxdexpr - 1] = exprs;
	}

	/*
	 * 若旧元组存在就删掉它，再插入一个新的。这比
	 * 根据各种条件去更新或插入更容易。
	 */
	RemoveStatisticsDataById(statOid, inh);

	/* 构造并插入一个新元组 */
	stup = heap_form_tuple(RelationGetDescr(pg_stextdata), values, nulls);
	CatalogTupleInsert(pg_stextdata, stup);

	heap_freetuple(stup);

	table_close(pg_stextdata, RowExclusiveLock);
}

/* 初始化多维排序 */
MultiSortSupport
multi_sort_init(int ndims)
{
	MultiSortSupport mss;

	Assert(ndims >= 2);

	mss = (MultiSortSupport) palloc0(offsetof(MultiSortSupportData, ssup)
									 + sizeof(SortSupportData) * ndims);

	mss->ndims = ndims;

	return mss;
}

/*
 * 用给定的排序操作符和排序规则，在位置 sortdim 处
 * 准备排序支持信息
 */
void
multi_sort_add_dimension(MultiSortSupport mss, int sortdim,
						 Oid oper, Oid collation)
{
	SortSupport ssup = &mss->ssup[sortdim];

	ssup->ssup_cxt = CurrentMemoryContext;
	ssup->ssup_collation = collation;
	ssup->ssup_nulls_first = false;

	PrepareSortSupportFromOrderingOp(oper, ssup);
}

/* 按选定顺序比较所有维度 */
int
multi_sort_compare(const void *a, const void *b, void *arg)
{
	MultiSortSupport mss = (MultiSortSupport) arg;
	SortItem   *ia = (SortItem *) a;
	SortItem   *ib = (SortItem *) b;
	int			i;

	for (i = 0; i < mss->ndims; i++)
	{
		int			compare;

		compare = ApplySortComparator(ia->values[i], ia->isnull[i],
									  ib->values[i], ib->isnull[i],
									  &mss->ssup[i]);

		if (compare != 0)
			return compare;
	}

	/* 默认相等 */
	return 0;
}

/* 比较选定的维度 */
int
multi_sort_compare_dim(int dim, const SortItem *a, const SortItem *b,
					   MultiSortSupport mss)
{
	return ApplySortComparator(a->values[dim], a->isnull[dim],
							   b->values[dim], b->isnull[dim],
							   &mss->ssup[dim]);
}

int
multi_sort_compare_dims(int start, int end,
						const SortItem *a, const SortItem *b,
						MultiSortSupport mss)
{
	int			dim;

	for (dim = start; dim <= end; dim++)
	{
		int			r = ApplySortComparator(a->values[dim], a->isnull[dim],
											b->values[dim], b->isnull[dim],
											&mss->ssup[dim]);

		if (r != 0)
			return r;
	}

	return 0;
}

int
compare_scalars_simple(const void *a, const void *b, void *arg)
{
	return compare_datums_simple(*(Datum *) a,
								 *(Datum *) b,
								 (SortSupport) arg);
}

int
compare_datums_simple(Datum a, Datum b, SortSupport ssup)
{
	return ApplySortComparator(a, false, b, false, ssup);
}

/*
 * build_attnums_array
 *		把 bitmap 转换成 AttrNumber 值的数组。
 *
 * 这只用于扩展统计，因此所有属性都必须是
 * 用户定义的。这意味着这里不必按 FirstLowInvalidHeapAttributeNumber
 * 做偏移（查询 bitmap 时也不必）。
 */
AttrNumber *
build_attnums_array(Bitmapset *attrs, int nexprs, int *numattrs)
{
	int			i,
				j;
	AttrNumber *attnums;
	int			num = bms_num_members(attrs);

	if (numattrs)
		*numattrs = num;

	/* 从 bitmapset 构造 attnum */
	attnums = (AttrNumber *) palloc(sizeof(AttrNumber) * num);
	i = 0;
	j = -1;
	while ((j = bms_next_member(attrs, j)) >= 0)
	{
		int			attnum = (j - nexprs);

		/*
		 * 确保 bitmap 只含用户定义的属性。由于
		 * bitmap 没法含有负值，这一点可能以两种
		 * 方式被违反。第一，bitmap 可能把 0 当作成员；第二，
		 * 整数值可能大于 MaxAttrNumber。
		 */
		Assert(AttributeNumberIsValid(attnum));
		Assert(attnum <= MaxAttrNumber);
		Assert(attnum >= (-nexprs));

		attnums[i++] = (AttrNumber) attnum;

		/* 防止溢出 */
		Assert(i <= num);
	}

	return attnums;
}

/*
 * build_sorted_items
 *		用行里的值构造排好序的 SortItem 数组。
 *
 * 注意：所有内存都分配在单个块里，因此调用者
 * 只需 pfree 返回值就能全部释放。
 */
SortItem *
build_sorted_items(StatsBuildData *data, int *nitems,
				   MultiSortSupport mss,
				   int numattrs, AttrNumber *attnums)
{
	int			i,
				j,
				len,
				nrows;
	int			nvalues = data->numrows * numattrs;

	SortItem   *items;
	Datum	   *values;
	bool	   *isnull;
	char	   *ptr;
	int		   *typlen;

	/* 计算我们需要的内存总量（项和值都算）。 */
	len = data->numrows * sizeof(SortItem) + nvalues * (sizeof(Datum) + sizeof(bool));

	/* 分配内存并把它分成几块。 */
	ptr = palloc0(len);

	/* 要排序的项 */
	items = (SortItem *) ptr;
	ptr += data->numrows * sizeof(SortItem);

	/* 值和空标志 */
	values = (Datum *) ptr;
	ptr += nvalues * sizeof(Datum);

	isnull = (bool *) ptr;
	ptr += nvalues * sizeof(bool);

	/* 确保我们恰好用完整个缓冲区 */
	Assert((ptr - (char *) items) == len);

	/* 修正指向 Datum 和 bool 数组的指针 */
	nrows = 0;
	for (i = 0; i < data->numrows; i++)
	{
		items[nrows].values = &values[nrows * numattrs];
		items[nrows].isnull = &isnull[nrows * numattrs];

		nrows++;
	}

	/* 为所有属性建立 typlen 的本地缓存 */
	typlen = (int *) palloc(sizeof(int) * data->nattnums);
	for (i = 0; i < data->nattnums; i++)
		typlen[i] = get_typlen(data->stats[i]->attrtypid);

	nrows = 0;
	for (i = 0; i < data->numrows; i++)
	{
		bool		toowide = false;

		/* 从样本行装载值/空标志 */
		for (j = 0; j < numattrs; j++)
		{
			Datum		value;
			bool		isnull;
			int			attlen;
			AttrNumber	attnum = attnums[j];

			int			idx;

			/* 把 attnum 与预先计算的数据对应起来 */
			for (idx = 0; idx < data->nattnums; idx++)
			{
				if (attnum == data->attnums[idx])
					break;
			}

			Assert(idx < data->nattnums);

			value = data->values[idx][i];
			isnull = data->nulls[idx][i];
			attlen = typlen[idx];

			/*
			 * 若这是 varlena 值，就检查它是否过宽，若是
			 * 就跳过整项。否则把该值去 toast。
			 *
			 * XXX 可能已经把当前项前面的一些
			 * 值去了 toast。我们不费心清理那些，
			 * 因为假定它们很小（低于 WIDTH_THRESHOLD），
			 * 并且会在分析结束时被丢弃。
			 */
			if ((!isnull) && (attlen == -1))
			{
				if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
				{
					toowide = true;
					break;
				}

				value = PointerGetDatum(PG_DETOAST_DATUM(value));
			}

			items[nrows].values[j] = value;
			items[nrows].isnull[j] = isnull;
		}

		if (toowide)
			continue;

		nrows++;
	}

	/* 存储实际的项数（忽略过宽的那些） */
	*nitems = nrows;

	/* 所有项都过宽 */
	if (nrows == 0)
	{
		/* 一切都作为单个块分配 */
		pfree(items);
		return NULL;
	}

	/* 用多键排序来排序 */
	qsort_interruptible(items, nrows, sizeof(SortItem),
						multi_sort_compare, mss);

	return items;
}

/*
 * has_stats_of_kind
 *		检查该列表是否含有给定种类的统计
 */
bool
has_stats_of_kind(List *stats, char requiredkind)
{
	ListCell   *l;

	foreach(l, stats)
	{
		StatisticExtInfo *stat = (StatisticExtInfo *) lfirst(l);

		if (stat->kind == requiredkind)
			return true;
	}

	return false;
}

/*
 * stat_find_expression
 *		在统计对象的表达式列表里搜索一个表达式。
 *
 * 返回该表达式在统计对象表达式列表里的
 * 下标，若未找到则返回 -1。
 */
static int
stat_find_expression(StatisticExtInfo *stat, Node *expr)
{
	ListCell   *lc;
	int			idx;

	idx = 0;
	foreach(lc, stat->exprs)
	{
		Node	   *stat_expr = (Node *) lfirst(lc);

		if (equal(stat_expr, expr))
			return idx;
		idx++;
	}

	/* 未找到表达式 */
	return -1;
}

/*
 * stat_covers_expressions
 * 		测试一个统计对象是否覆盖列表里的全部表达式。
 *
 * 若全部表达式都被覆盖就返回真。若 expr_idxs 非 NULL，就
 * 用找到的表达式下标填充它。
 */
static bool
stat_covers_expressions(StatisticExtInfo *stat, List *exprs,
						Bitmapset **expr_idxs)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		int			expr_idx;

		expr_idx = stat_find_expression(stat, expr);
		if (expr_idx == -1)
			return false;

		if (expr_idxs != NULL)
			*expr_idxs = bms_add_member(*expr_idxs, expr_idx);
	}

	/* 若执行到这里，全部表达式都被覆盖 */
	return true;
}

/*
 * choose_best_statistics
 *		寻找并返回具有指定 requiredkind、且其键至少匹配
 *		两个给定 attnum 的统计。若没有
 *		匹配就返回 NULL。
 *
 * 当前的选择标准非常简单：我们选择
 * 引用了被覆盖（且仍未估计的
 * 子句）中最多属性的统计对象，平局时偏向整体键更少的对象。
 *
 * clause_attnums 是 bitmap 数组，存放各个
 * 子句的 attnum。NULL 元素表示该子句要么不相容，要么已经
 * 估计过。
 *
 * XXX 若多个统计对象在两项标准上都平局，则选中哪一个
 * 取决于它们在统计列表里出现的顺序。也许
 * 还需要进一步的平局决胜。
 */
StatisticExtInfo *
choose_best_statistics(List *stats, char requiredkind, bool inh,
					   Bitmapset **clause_attnums, List **clause_exprs,
					   int nclauses)
{
	ListCell   *lc;
	StatisticExtInfo *best_match = NULL;
	int			best_num_matched = 2;	/* 目标 1：最大化 */
	int			best_match_keys = (STATS_MAX_DIMENSIONS + 1);	/* 目标 2：最小化 */

	foreach(lc, stats)
	{
		int			i;
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);
		Bitmapset  *matched_attnums = NULL;
		Bitmapset  *matched_exprs = NULL;
		int			num_matched;
		int			numkeys;

		/* 跳过类型不正确的统计信息 */
		if (info->kind != requiredkind)
			continue;

		/* 跳过继承标志不匹配的统计信息 */
		if (info->inherit != inh)
			continue;

		/*
		 * 收集剩余（未估计）子句里、被该统计对象完全
		 * 覆盖的属性和表达式。
		 *
		 * 我们已经知道已估计的子句把 clause_attnums 和
		 * clause_exprs 都设成了 NULL。若已经估计过，我们就把指针留为 NULL，
		 * 或者在估计该子句之后把它们重置为 NULL。
		 */
		for (i = 0; i < nclauses; i++)
		{
			Bitmapset  *expr_idxs = NULL;

			/* 忽略不相容或已估计的子句 */
			if (!clause_attnums[i] && !clause_exprs[i])
				continue;

			/* 忽略未被该对象覆盖的子句 */
			if (!bms_is_subset(clause_attnums[i], info->keys) ||
				!stat_covers_expressions(info, clause_exprs[i], &expr_idxs))
				continue;

			/* 记录被覆盖的 attnum 和表达式下标 */
			matched_attnums = bms_add_members(matched_attnums, clause_attnums[i]);
			matched_exprs = bms_add_members(matched_exprs, expr_idxs);
		}

		num_matched = bms_num_members(matched_attnums) + bms_num_members(matched_exprs);

		bms_free(matched_attnums);
		bms_free(matched_exprs);

		/*
		 * 保存统计里键的实际个数，以便我们能选择
		 * 匹配键最多的最窄统计。
		 */
		numkeys = bms_num_members(info->keys) + list_length(info->exprs);

		/*
		 * 当它增加了匹配的属性和表达式个数时使用该对象，
		 * 或者当它匹配的属性和表达式个数相同，
		 * 但这些统计的键比先前任何
		 * 匹配都少时也使用它。
		 */
		if (num_matched > best_num_matched ||
			(num_matched == best_num_matched && numkeys < best_match_keys))
		{
			best_match = info;
			best_num_matched = num_matched;
			best_match_keys = numkeys;
		}
	}

	return best_match;
}

/*
 * statext_is_compatible_clause_internal
 *		判断该子句是否与 MCV 列表相容。
 *
 * 要相容，给定子句必须是由 Var 或子表达式构成的受支持
 * 子句的组合（子表达式是
 * 与统计对象里找到的表达式完全匹配的东西）。
 * 本函数递归检查该子句，并提取任何
 * 需要与统计信息匹配的子表达式。
 *
 * 目前我们只支持下列类型的子句：
 *
 * (a) 形式为 (Var/Expr op Const) 或 (Const op Var/Expr) 的 OpExprs，其中
 * op 是（"="、"<"、">"、">="、"<="）之一。
 *
 * (b) (Var/Expr IS [NOT] NULL)。
 *
 * (c) 用 AND/OR/NOT 构成的组合。
 *
 * (d) 形式为 (Var/Expr op ANY (Const)) 或
 * (Var/Expr op ALL (Const)) 的 ScalarArrayOpExprs。
 *
 * 将来，受支持的子句范围可能扩展到更
 * 复杂的情形，例如 (Var op Var)。
 *
 * 参数：
 * clause：（子）子句，待检查（裸子句，不是 RestrictInfo）
 * relid：子句里所有 Var 必须属于的关系
 * *attnums：输入/输出参数，收集所有
 *		被提及的 Var 的属性号。注意我们不偏移属性号，
 *		因此没法应付系统列。
 * *exprs：输入/输出参数，收集子句树里的
 *		原始子子句
 * *leakproof：输入/输出参数，记录子句树的防泄漏性。
 *		一开始应当为真，若某个 OpExpr 里使用的操作符函数
 *		不是防泄漏的，就会被设为假。
 *
 * 若有我们明确没法处理的东西，就返回假。
 * 若返回真，就可以继续把 *exprs 与统计信息匹配。
 */
static bool
statext_is_compatible_clause_internal(PlannerInfo *root, Node *clause,
									  Index relid, Bitmapset **attnums,
									  List **exprs, bool *leakproof)
{
	/* 看进任何二进制兼容的重标记（如同 examine_variable） */
	if (IsA(clause, RelabelType))
		clause = (Node *) ((RelabelType *) clause)->arg;

	/* 普通 Var 引用（布尔 Var 或递归检查） */
	if (IsA(clause, Var))
	{
		Var		   *var = (Var *) clause;

		/* 确保 var 来自正确的关系 */
		if (var->varno != relid)
			return false;

		/* 最好也确保 Var 来自当前层次 */
		if (var->varlevelsup > 0)
			return false;

		/*
		 * 也拒绝系统属性和整行 Var（我们不允许
		 * 在那些上面建立统计）。
		 */
		if (!AttrNumberIsForUserDefinedAttr(var->varattno))
			return false;

		/* 好，记下 attnum，供稍后的权限检查。 */
		*attnums = bms_add_member(*attnums, var->varattno);

		return true;
	}

	/* (Var/Expr op Const) 或 (Const op Var/Expr) */
	if (is_opclause(clause))
	{
		OpExpr	   *expr = (OpExpr *) clause;
		Node	   *clause_expr;

		/* 只有两个参数的表达式才被认为相容。 */
		if (list_length(expr->args) != 2)
			return false;

		/* 检查该表达式的形状是否正确 */
		if (!examine_opclause_args(expr->args, &clause_expr, NULL, NULL))
			return false;

		/*
		 * 若它不是受支持的操作符之一（"="、"<"、">" 等），
		 * 就忽略该子句，因为它与 MCV 列表不相容。
		 *
		 * 这里用的是估计选择率的函数，而不是直接用
		 * 操作符（有点别扭，但也只能如此……）。
		 */
		switch (get_oprrest(expr->opno))
		{
			case F_EQSEL:
			case F_NEQSEL:
			case F_SCALARLTSEL:
			case F_SCALARLESEL:
			case F_SCALARGTSEL:
			case F_SCALARGESEL:
				/* 受支持，将继续检查 Var/Expr */
				break;

			default:
				/* 其他估计函数被视为未知/不受支持 */
				return false;
		}

		/* 检查该操作符是否防泄漏 */
		if (*leakproof)
			*leakproof = get_func_leakproof(get_opcode(expr->opno));

		/* 通过递归检查 (Var op Const) 或 (Const op Var) 子句。 */
		if (IsA(clause_expr, Var))
			return statext_is_compatible_clause_internal(root, clause_expr,
														 relid, attnums,
														 exprs, leakproof);

		/* 否则我们有 (Expr op Const) 或 (Const op Expr)。 */
		*exprs = lappend(*exprs, clause_expr);
		return true;
	}

	/* Var/Expr IN Array（在数组中） */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) clause;
		Node	   *clause_expr;
		bool		expronleft;

		/* 只有两个参数的表达式才被认为相容。 */
		if (list_length(expr->args) != 2)
			return false;

		/* 检查该表达式的形状是否正确（一个 Var，一个 Const） */
		if (!examine_opclause_args(expr->args, &clause_expr, NULL, &expronleft))
			return false;

		/* 我们只支持左边是 Var、右边是 Const */
		if (!expronleft)
			return false;

		/*
		 * 若它不是受支持的操作符之一（"="、"<"、">" 等），
		 * 就忽略该子句，因为它与 MCV 列表不相容。
		 *
		 * 这里用的是估计选择率的函数，而不是直接用
		 * 操作符（有点别扭，但也只能如此……）。
		 */
		switch (get_oprrest(expr->opno))
		{
			case F_EQSEL:
			case F_NEQSEL:
			case F_SCALARLTSEL:
			case F_SCALARLESEL:
			case F_SCALARGTSEL:
			case F_SCALARGESEL:
				/* 受支持，将继续检查 Var/Expr */
				break;

			default:
				/* 其他估计函数被视为未知/不受支持 */
				return false;
		}

		/* 检查该操作符是否防泄漏 */
		if (*leakproof)
			*leakproof = get_func_leakproof(get_opcode(expr->opno));

		/* 通过递归检查 Var IN Array 子句。 */
		if (IsA(clause_expr, Var))
			return statext_is_compatible_clause_internal(root, clause_expr,
														 relid, attnums,
														 exprs, leakproof);

		/* 否则我们有 Expr IN Array。 */
		*exprs = lappend(*exprs, clause_expr);
		return true;
	}

	/* AND/OR/NOT 子句 */
	if (is_andclause(clause) ||
		is_orclause(clause) ||
		is_notclause(clause))
	{
		/*
		 * 若所有子子句都受支持，则 AND/OR/NOT 子句受支持。
		 *
		 * 也许可以通过处理混合情形来改进：有些
		 * 子句受支持，有些不受支持。受支持子子句的选择率
		 * 将用扩展统计来计算，
		 * 其余子句则用传统
		 * 算法估计（选择率的乘积）。
		 *
		 * 不过这似乎过于复杂，而且在某种意义上我们已经那样做了，
		 * 因为若在这里把整个子句当作不受支持而拒绝，它最终
		 * 会被传给 clauselist_selectivity()，而那个函数做的正是
		 * 这件事（拆成受支持/不受支持的子句等等）。
		 */
		BoolExpr   *expr = (BoolExpr *) clause;
		ListCell   *lc;

		foreach(lc, expr->args)
		{
			/*
			 * 若在参数里发现不相容的子句，就把
			 * 整个子句当作不相容。
			 */
			if (!statext_is_compatible_clause_internal(root,
													   (Node *) lfirst(lc),
													   relid, attnums, exprs,
													   leakproof))
				return false;
		}

		return true;
	}

	/* Var/Expr IS NULL（为空） */
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		/* 通过递归检查 Var IS NULL 子句。 */
		if (IsA(nt->arg, Var))
			return statext_is_compatible_clause_internal(root,
														 (Node *) (nt->arg),
														 relid, attnums,
														 exprs, leakproof);

		/* 否则我们有 Expr IS NULL。 */
		*exprs = lappend(*exprs, nt->arg);
		return true;
	}

	/*
	 * 把任何其他表达式当作裸表达式，去与
	 * 统计对象里的表达式匹配。
	 */
	*exprs = lappend(*exprs, clause);
	return true;
}

/*
 * statext_is_compatible_clause
 *		判断该子句是否与 MCV 列表相容。
 *
 * 基本规则见上面的 statext_is_compatible_clause_internal。
 * 这一层处理 RestrictInfo 上层结构，并做权限
 * 检查，以验证可以检查所有被提及的 Var。
 *
 * 参数：
 * clause：待检查的子句（RestrictInfo 形式）
 * relid：子句里所有 Var 必须属于的关系
 * *attnums：输入/输出参数，收集所有
 *		被提及的 Var 的属性号。注意我们不偏移属性号，
 *		因此没法应付系统列。
 * *exprs：输入/输出参数，收集子句树里的
 *		原始子子句。
 *
 * 若有我们明确没法处理的东西，就返回假。
 * 若返回真，就可以继续把 *exprs 与统计信息匹配。
 */
static bool
statext_is_compatible_clause(PlannerInfo *root, Node *clause, Index relid,
							 Bitmapset **attnums, List **exprs)
{
	RestrictInfo *rinfo;
	int			clause_relid;
	bool		leakproof;

	/*
	 * 对裸 BoolExpr AND 子句做特殊处理，因为
	 * restrictinfo 机制不会在 AND
	 * 子句之上建立 RestrictInfos。
	 */
	if (is_andclause(clause))
	{
		BoolExpr   *expr = (BoolExpr *) clause;
		ListCell   *lc;

		/*
		 * 检查每个子子句是否相容。我们预期这些是
		 * RestrictInfos。
		 */
		foreach(lc, expr->args)
		{
			if (!statext_is_compatible_clause(root, (Node *) lfirst(lc),
											  relid, attnums, exprs))
				return false;
		}

		return true;
	}

	/* 否则它必须是 RestrictInfo。 */
	if (!IsA(clause, RestrictInfo))
		return false;
	rinfo = (RestrictInfo *) clause;

	/* 伪常量在这里其实没有意义。 */
	if (rinfo->pseudoconstant)
		return false;

	/* 引用其他 varno 的子句不相容。 */
	if (!bms_get_singleton_member(rinfo->clause_relids, &clause_relid) ||
		clause_relid != relid)
		return false;

	/*
	 * 检查该子句，确定它引用哪些属性，以及
	 * 它是否含有任何非防泄漏操作符。
	 */
	leakproof = true;
	if (!statext_is_compatible_clause_internal(root, (Node *) rinfo->clause,
											   relid, attnums, exprs,
											   &leakproof))
		return false;

	/*
	 * 若子句含有任何非防泄漏操作符，就检查用户
	 * 是否有权读取所有所需属性，否则这些操作符
	 * 可能泄露用户无权
	 * 看到的 MCV 列表里的值。我们需要所有行都可被选择：不得有
	 * 来自安全屏障视图或 RLS 策略的 securityQuals。见
	 * examine_variable()、examine_simple_variable() 和
	 * statistic_proc_security_check() 里的类似代码。
	 *
	 * 注意，对继承孩子，权限检查是在
	 * 继承根父关系上进行的，对父关系的整表选择权限
	 * 并不保证用户能读取孩子的所有列。
	 * 因此我们必须检查所有被引用的列。
	 */
	if (!leakproof)
	{
		Bitmapset  *clause_attnums = NULL;
		int			attnum = -1;

		/*
		 * 我们必须检查按列权限。*attnums 含我们见到的
		 * 各个 Var 的 attnum，但 *exprs 的子表达式里
		 * 也可能有 Var。我们可以用 pull_varattnos() 提取
		 * 那些，但存在阻抗不匹配：pull_varattnos() 返回的 attnum
		 * 按 FirstLowInvalidHeapAttributeNumber 偏移，
		 * 而 *attnums 里的 attnum 没有偏移。把 *attnums 转换成
		 * 偏移风格，以便合并结果。
		 */
		while ((attnum = bms_next_member(*attnums, attnum)) >= 0)
		{
			clause_attnums =
				bms_add_member(clause_attnums,
							   attnum - FirstLowInvalidHeapAttributeNumber);
		}

		/* 现在把 *exprs 里的 attnum 并入 clause_attnums */
		if (*exprs != NIL)
			pull_varattnos((Node *) *exprs, relid, &clause_attnums);

		/* 必须有权从这些列读取所有行 */
		if (!all_rows_selectable(root, relid, clause_attnums))
			return false;
	}

	/* 若执行到这里，该子句没问题 */
	return true;
}

/*
 * statext_mcv_clauselist_selectivity
 *		用最佳的多列统计信息估计子句。
 *
 * 把可用的扩展（多列）统计信息应用到表上。可能
 * 有多个适用的统计信息（相对于这些子句），那种
 * 情况下我们用贪心方法。每一轮我们选择表上最佳的统计信息
 * （以从子句提取并被它覆盖的属性个数来衡量），
 * 并为提供的子句计算选择率。
 * 我们用剩余的子句（若有）重复这个过程，直到没有
 * 可用的统计信息能被使用。
 *
 * 使用 MCV 列表的主要困难之一是如何把估计外推到
 * MCV 列表未覆盖的数据。为此，我们不仅计算
 * 匹配所提供子句的 MCV 项的选择率（MCV 选择率），
 * 还计算下列相关选择率：
 *
 * - 简单选择率：不使用扩展统计来计算，也就是当作
 * 列/子句相互独立。
 *
 * - 基选择率：与简单选择率类似，但用
 * 扩展统计来计算，方法是把匹配的 MCV 项的基频率
 * （我们为每个 MCV 项计算并存储）加起来。
 *
 * - 总选择率：整个 MCV 列表覆盖的选择率。
 *
 * 这些被传给 mcv_combine_selectivities()，它把它们组合起来，
 * 产生同时利用按列统计信息
 * 和多列 MCV 统计信息的选择率估计。
 *
 * estimatedclauses 是输入/输出参数。我们为所估计的
 * 从 0 起计的 clauses 下标置位，并跳过
 * 已经置了位的子句项。
 */
static Selectivity
statext_mcv_clauselist_selectivity(PlannerInfo *root, List *clauses, int varRelid,
								   JoinType jointype, SpecialJoinInfo *sjinfo,
								   RelOptInfo *rel, Bitmapset **estimatedclauses,
								   bool is_or)
{
	ListCell   *l;
	Bitmapset **list_attnums;	/* 从该子句提取的 attnum */
	List	  **list_exprs;		/* 匹配到任何统计的表达式 */
	int			listidx;
	Selectivity sel = (is_or) ? 0.0 : 1.0;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	/* 检查是否有任何可能对我们有用的统计信息。 */
	if (!has_stats_of_kind(rel->statlist, STATS_EXT_MCV))
		return sel;

	list_attnums = (Bitmapset **) palloc(sizeof(Bitmapset *) *
										 list_length(clauses));

	/* 从复杂表达式提取的表达式 */
	list_exprs = (List **) palloc(sizeof(Node *) * list_length(clauses));

	/*
	 * 预处理子句列表，提取每一项里见到的 attnum 和表达式。
	 * 我们需要判断是否有任何子句对用扩展统计做
	 * 选择率估计有用。沿途我们会把每个子句的
	 * 全部 attnum 和表达式记入列表，稍后引用它们，这样
	 * 就不必再重复
	 * 同样的工作。
	 *
	 * 我们也会跳过已经用其他类型的
	 * 统计信息估计过的子句（把它们当作不相容）。
	 */
	listidx = 0;
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		Bitmapset  *attnums = NULL;
		List	   *exprs = NIL;

		if (!bms_is_member(listidx, *estimatedclauses) &&
			statext_is_compatible_clause(root, clause, rel->relid, &attnums, &exprs))
		{
			list_attnums[listidx] = attnums;
			list_exprs[listidx] = exprs;
		}
		else
		{
			list_attnums[listidx] = NULL;
			list_exprs[listidx] = NIL;
		}

		listidx++;
	}

	/* 尽可能多地应用扩展统计 */
	while (true)
	{
		StatisticExtInfo *stat;
		List	   *stat_clauses;
		Bitmapset  *simple_clauses;

		/* 为这些 attnum 找出最合适的统计对象 */
		stat = choose_best_statistics(rel->statlist, STATS_EXT_MCV, rte->inh,
									  list_attnums, list_exprs,
									  list_length(clauses));

		/*
		 * 若找不到（额外的）匹配统计，就没有事情
		 * 可做
		 */
		if (!stat)
			break;

		/* 确保 choose_best_statistics 产生了预期的统计类型。 */
		Assert(stat->kind == STATS_EXT_MCV);

		/* 现在过滤出要用选定的 MCV 来估计的子句 */
		stat_clauses = NIL;

		/* 记录哪些子句是简单的（单列或表达式） */
		simple_clauses = NULL;

		listidx = -1;
		foreach(l, clauses)
		{
			/* 在决定是否跳过该子句之前先递增下标。 */
			listidx++;

			/*
			 * 忽略我们没有从中提取出任何 attnum 或
			 * 表达式的子句（这需要与
			 * choose_best_statistics 里的做法一致）。
			 *
			 * 这也会消除已经估计过的子句，包括那些
			 * 在应用扩展统计之前和期间估计的。
			 *
			 * XXX 需要这项检查，因为 bms_is_subset 和
			 * stat_covers_expressions 对空的 attnum 和
			 * 表达式都返回真。
			 */
			if (!list_attnums[listidx] && !list_exprs[listidx])
				continue;

			/*
			 * 该子句尚未估计，并且我们已经从中提取了
			 * attnum 或表达式。若它没有被选定的统计对象
			 * 完全覆盖，就忽略它。
			 *
			 * 我们需要同时检查属性和表达式，若
			 * 其中任一未被覆盖就拒绝。
			 */
			if (!bms_is_subset(list_attnums[listidx], stat->keys) ||
				!stat_covers_expressions(stat, list_exprs[listidx], NULL))
				continue;

			/*
			 * 现在我们知道该子句相容（我们已经从中提取了 attnum 或
			 * 表达式），并且尚未估计。
			 */

			/* 记录简单子句（单列或表达式） */
			if ((list_attnums[listidx] == NULL &&
				 list_length(list_exprs[listidx]) == 1) ||
				(list_exprs[listidx] == NIL &&
				 bms_membership(list_attnums[listidx]) == BMS_SINGLETON))
				simple_clauses = bms_add_member(simple_clauses,
												list_length(stat_clauses));

			/* 把子句加入列表并标记为已估计 */
			stat_clauses = lappend(stat_clauses, (Node *) lfirst(l));
			*estimatedclauses = bms_add_member(*estimatedclauses, listidx);

			/*
			 * 重置这些指针，以便 choose_best_statistics 知道该
			 * 子句已经估计过，不再考虑它。
			 */
			bms_free(list_attnums[listidx]);
			list_attnums[listidx] = NULL;

			list_free(list_exprs[listidx]);
			list_exprs[listidx] = NULL;
		}

		if (is_or)
		{
			bool	   *or_matches = NULL;
			Selectivity simple_or_sel = 0.0,
						stat_sel = 0.0;
			MCVList    *mcv_list;

			/* 装载存在该统计对象里的 MCV 列表 */
			mcv_list = statext_mcv_load(stat->statOid, rte->inh);

			/*
			 * 计算该统计对象所覆盖的 ORed 子句列表的选择率，
			 * 方法是逐个估计，再用公式
			 * P(A OR B) = P(A) + P(B) - P(A AND B) 组合。
			 * 这让我们能用多变量 MCV 统计更好地
			 * 估计各个项以及它们的重叠。
			 *
			 * 每次迭代该公式时，上面的子句 A
			 * 等于迄今处理过的全部子句用 OR 组合起来的结果。
			 */
			listidx = 0;
			foreach(l, stat_clauses)
			{
				Node	   *clause = (Node *) lfirst(l);
				Selectivity simple_sel,
							overlap_simple_sel,
							mcv_sel,
							mcv_basesel,
							overlap_mcvsel,
							overlap_basesel,
							mcv_totalsel,
							clause_sel,
							overlap_sel;

				/*
				 * 下一条子句的简单选择率，以及它与
				 * 任何前面子句的重叠。这些是我们对 P(B) 和 P(A AND B) 的初始
				 * 估计，假定
				 * 列/子句相互独立。
				 */
				simple_sel = clause_selectivity_ext(root, clause, varRelid,
													jointype, sjinfo, false);

				overlap_simple_sel = simple_or_sel * simple_sel;

				/*
				 * 迄今见过的全部子句的新简单选择率，
				 * 假定相互独立。
				 */
				simple_or_sel += simple_sel - overlap_simple_sel;
				CLAMP_PROBABILITY(simple_or_sel);

				/*
				 * 用 MCV 统计对该子句做的多列估计，
				 * 以及基选择率和总选择率，还有
				 * 重叠项 P(A AND B) 的对应选择率。
				 */
				mcv_sel = mcv_clause_selectivity_or(root, stat, mcv_list,
													clause, &or_matches,
													&mcv_basesel,
													&overlap_mcvsel,
													&overlap_basesel,
													&mcv_totalsel);

				/*
				 * 组合简单估计和多列估计。
				 *
				 * 若该子句是简单的单列子句，就
				 * 只用简单选择率估计，因为
				 * 多列统计不太可能改进它
				 * （实际上可能使它更差）。对重叠，我们
				 * 总是利用多列统计。
				 */
				if (bms_is_member(listidx, simple_clauses))
					clause_sel = simple_sel;
				else
					clause_sel = mcv_combine_selectivities(simple_sel,
														   mcv_sel,
														   mcv_basesel,
														   mcv_totalsel);

				overlap_sel = mcv_combine_selectivities(overlap_simple_sel,
														overlap_mcvsel,
														overlap_basesel,
														mcv_totalsel);

				/* 把这些计入该统计对象的结果 */
				stat_sel += clause_sel - overlap_sel;
				CLAMP_PROBABILITY(stat_sel);

				listidx++;
			}

			/*
			 * 把该统计对象的结果计入总
			 * 结果。我们把每个单独统计
			 * 对象的结果彼此当作独立的。
			 */
			sel = sel + stat_sel - sel * stat_sel;
		}
		else					/* 隐式 ANDed 的子句列表 */
		{
			Selectivity simple_sel,
						mcv_sel,
						mcv_basesel,
						mcv_totalsel,
						stat_sel;

			/*
			 * 简单选择率，也就是不用任何扩展统计，
			 * 本质上假定列/子句相互独立。
			 */
			simple_sel = clauselist_selectivity_ext(root, stat_clauses,
													varRelid, jointype,
													sjinfo, false);

			/*
			 * 用 MCV 统计做的多列估计，以及基选择率和
			 * 总选择率。
			 */
			mcv_sel = mcv_clauselist_selectivity(root, stat, stat_clauses,
												 varRelid, jointype, sjinfo,
												 rel, &mcv_basesel,
												 &mcv_totalsel);

			/* 组合简单估计和多列估计。 */
			stat_sel = mcv_combine_selectivities(simple_sel,
												 mcv_sel,
												 mcv_basesel,
												 mcv_totalsel);

			/* 把这个计入总结果 */
			sel *= stat_sel;
		}
	}

	return sel;
}

/*
 * statext_clauselist_selectivity
 *		用最佳的多列统计信息估计子句。
 */
Selectivity
statext_clauselist_selectivity(PlannerInfo *root, List *clauses, int varRelid,
							   JoinType jointype, SpecialJoinInfo *sjinfo,
							   RelOptInfo *rel, Bitmapset **estimatedclauses,
							   bool is_or)
{
	Selectivity sel;

	/* 首先，尝试用多变量 MCV 列表估计子句。 */
	sel = statext_mcv_clauselist_selectivity(root, clauses, varRelid, jointype,
											 sjinfo, rel, estimatedclauses, is_or);

	/*
	 * 函数依赖只对用 AND 连接的子句起作用，因此对
	 * OR 子句我们就做完了。
	 */
	if (is_or)
		return sel;

	/*
	 * 然后，通过调用 dependencies_clauselist_selectivity 把函数依赖应用到剩余子句上。
	 * 传入 estimatedclauses，以便该
	 * 函数能正确跳过上面已经估计过的子句。
	 *
	 * 最后才应用依赖，理由是更复杂的
	 * 统计能跟踪属性之间更复杂的相关性，
	 * 因此可以被认为更可靠。
	 *
	 * 例如，MCV 列表能给出两列里的值的精确选择率，
	 * 而函数依赖只能提供
	 * 该依赖总体强度的信息。
	 */
	sel *= dependencies_clauselist_selectivity(root, clauses, varRelid,
											   jointype, sjinfo, rel,
											   estimatedclauses);

	return sel;
}

/*
 * examine_opclause_args
 *		把操作符表达式的参数拆成 Expr 和 Const 两部分。
 *
 * 试图把参数匹配成 (Expr op Const) 或 (Const op
 * Expr)，上面可能有 RelabelType。当表达式匹配这种
 * 形式时返回真，否则返回假。
 *
 * 可选地，当传入非空指针（exprp、cstp 和 expronleftp）时，
 * 返回指向提取出的 Expr/Const 节点的指针。expronleftp 标志
 * 指明我们在操作符的哪一侧找到了表达式节点。
 */
bool
examine_opclause_args(List *args, Node **exprp, Const **cstp,
					  bool *expronleftp)
{
	Node	   *expr;
	Const	   *cst;
	bool		expronleft;
	Node	   *leftop,
			   *rightop;

	/* 由 statext_is_compatible_clause_internal 强制 */
	Assert(list_length(args) == 2);

	leftop = linitial(args);
	rightop = lsecond(args);

	/* 去掉表达式任一侧的 RelabelType */
	if (IsA(leftop, RelabelType))
		leftop = (Node *) ((RelabelType *) leftop)->arg;

	if (IsA(rightop, RelabelType))
		rightop = (Node *) ((RelabelType *) rightop)->arg;

	if (IsA(rightop, Const))
	{
		expr = (Node *) leftop;
		cst = (Const *) rightop;
		expronleft = true;
	}
	else if (IsA(leftop, Const))
	{
		expr = (Node *) rightop;
		cst = (Const *) leftop;
		expronleft = false;
	}
	else
		return false;

	/* 若请求，就返回指向提取出的部分的指针 */
	if (exprp)
		*exprp = expr;

	if (cstp)
		*cstp = cst;

	if (expronleftp)
		*expronleftp = expronleft;

	return true;
}


/*
 * 计算一个关系的表达式的统计信息。
 */
static void
compute_expr_stats(Relation onerel, double totalrows,
				   AnlExprData *exprdata, int nexprs,
				   HeapTuple *rows, int numrows)
{
	MemoryContext expr_context,
				old_context;
	int			ind,
				i;

	expr_context = AllocSetContextCreate(CurrentMemoryContext,
										 "Analyze Expression",
										 ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(expr_context);

	for (ind = 0; ind < nexprs; ind++)
	{
		AnlExprData *thisdata = &exprdata[ind];
		VacAttrStats *stats = thisdata->vacattrstat;
		Node	   *expr = thisdata->expr;
		TupleTableSlot *slot;
		EState	   *estate;
		ExprContext *econtext;
		Datum	   *exprvals;
		bool	   *exprnulls;
		ExprState  *exprstate;
		int			tcnt;

		/* 我们还在主上下文里吗？ */
		Assert(CurrentMemoryContext == expr_context);

		/*
		 * 对表达式求值需要一个 EState。在
		 * 按表达式的上下文里创建它，以确保它在循环
		 * 底部被清理。
		 */
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);

		/* 设置表达式求值状态 */
		exprstate = ExecPrepareExpr((Expr *) expr, estate);

		/* 也需要一个槽来保存当前的堆元组 */
		slot = MakeSingleTupleTableSlot(RelationGetDescr(onerel),
										&TTSOpsHeapTuple);

		/* 安排 econtext 的扫描元组为正在测试的元组 */
		econtext->ecxt_scantuple = slot;

		/* 计算并保存表达式的值 */
		exprvals = (Datum *) palloc(numrows * sizeof(Datum));
		exprnulls = (bool *) palloc(numrows * sizeof(bool));

		tcnt = 0;
		for (i = 0; i < numrows; i++)
		{
			Datum		datum;
			bool		isnull;

			/*
			 * 每次重置按元组上下文，以回收求值统计表达式后
			 * 留下的任何废弃物。
			 */
			ResetExprContext(econtext);

			/* 为表达式求值做好准备 */
			ExecStoreHeapTuple(rows[i], slot, false);

			/*
			 * 对表达式求值。我们在按元组上下文里做这件事，
			 * 以免泄漏内存，然后把结果复制到
			 * 本函数开头创建的上下文里。
			 */
			datum = ExecEvalExprSwitchContext(exprstate,
											  GetPerTupleExprContext(estate),
											  &isnull);
			if (isnull)
			{
				exprvals[tcnt] = (Datum) 0;
				exprnulls[tcnt] = true;
			}
			else
			{
				/* 确保我们把数据复制进该上下文。 */
				Assert(CurrentMemoryContext == expr_context);

				exprvals[tcnt] = datumCopy(datum,
										   stats->attrtype->typbyval,
										   stats->attrtype->typlen);
				exprnulls[tcnt] = false;
			}

			tcnt++;
		}

		/*
		 * 现在可以为表达式列计算统计信息。
		 *
		 * XXX 与 compute_index_stats 不同，这里不需要切换和重置
		 * 内存上下文，因为我们只为一个
		 * 表达式计算统计（而不是遍历许多索引），因此就在
		 * expr_context 里做。注意 compute_stats 会把结果复制
		 * 到 stats->anl_context，因此它不会消失。
		 */
		if (tcnt > 0)
		{
			AttributeOpts *aopt =
				get_attribute_options(stats->attr->attrelid,
									  stats->attr->attnum);

			stats->exprvals = exprvals;
			stats->exprnulls = exprnulls;
			stats->rowstride = 1;
			stats->compute_stats(stats,
								 expr_fetch_func,
								 tcnt,
								 tcnt);

			/*
			 * 若指定了 n_distinct 选项，它就覆盖上面的
			 * 计算。
			 */
			if (aopt != NULL && aopt->n_distinct != 0.0)
				stats->stadistinct = aopt->n_distinct;
		}

		/* 然后清理 */
		MemoryContextSwitchTo(expr_context);

		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
		MemoryContextResetAndDeleteChildren(expr_context);
	}

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(expr_context);
}


/*
 * 取得用于分析统计对象表达式的函数。
 *
 * 我们没有费心用这些数据构造元组，数据
 * 只是放在 Datum 数组里。
 */
static Datum
expr_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			i;

	/* exprvals 和 exprnulls 已经为正确的列做了偏移 */
	i = rownum * stats->rowstride;
	*isNull = stats->exprnulls[i];
	return stats->exprvals[i];
}

/*
 * 为一组表达式构造分析数据。由于这不直接
 * 绑在某个关系（表或索引）上，我们必须在 examine_expression() 里
 * 伪造一些成员。
 */
static AnlExprData *
build_expr_data(List *exprs, int stattarget)
{
	int			idx;
	int			nexprs = list_length(exprs);
	AnlExprData *exprdata;
	ListCell   *lc;

	exprdata = (AnlExprData *) palloc0(nexprs * sizeof(AnlExprData));

	idx = 0;
	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		AnlExprData *thisdata = &exprdata[idx];

		thisdata->expr = expr;
		thisdata->vacattrstat = examine_expression(expr, stattarget);
		idx++;
	}

	return exprdata;
}

/* 构造 pg_statistic 行的数组（按 update_attstats） */
static Datum
serialize_expr_stats(AnlExprData *exprdata, int nexprs)
{
	int			exprno;
	Oid			typOid;
	Relation	sd;

	ArrayBuildState *astate = NULL;

	sd = table_open(StatisticRelationId, RowExclusiveLock);

	/* 查找 pg_statistic 复合类型的 OID */
	typOid = get_rel_type_id(StatisticRelationId);
	if (!OidIsValid(typOid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" does not have a composite type",
						"pg_statistic")));

	for (exprno = 0; exprno < nexprs; exprno++)
	{
		int			i,
					k;
		VacAttrStats *stats = exprdata[exprno].vacattrstat;

		Datum		values[Natts_pg_statistic];
		bool		nulls[Natts_pg_statistic];
		HeapTuple	stup;

		if (!stats->stats_valid)
		{
			astate = accumArrayResult(astate,
									  (Datum) 0,
									  true,
									  typOid,
									  CurrentMemoryContext);
			continue;
		}

		/*
		 * 构造一个新的 pg_statistic 元组
		 */
		for (i = 0; i < Natts_pg_statistic; ++i)
		{
			nulls[i] = false;
		}

		values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(InvalidOid);
		values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(InvalidAttrNumber);
		values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(false);
		values[Anum_pg_statistic_stanullfrac - 1] = Float4GetDatum(stats->stanullfrac);
		values[Anum_pg_statistic_stawidth - 1] = Int32GetDatum(stats->stawidth);
		values[Anum_pg_statistic_stadistinct - 1] = Float4GetDatum(stats->stadistinct);
		i = Anum_pg_statistic_stakind1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = Int16GetDatum(stats->stakind[k]); /* stakindN（种类） */
		}
		i = Anum_pg_statistic_staop1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->staop[k]);	/* staopN（操作符） */
		}
		i = Anum_pg_statistic_stacoll1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->stacoll[k]);	/* stacollN（排序规则） */
		}
		i = Anum_pg_statistic_stanumbers1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			int			nnum = stats->numnumbers[k];

			if (nnum > 0)
			{
				int			n;
				Datum	   *numdatums = (Datum *) palloc(nnum * sizeof(Datum));
				ArrayType  *arry;

				for (n = 0; n < nnum; n++)
					numdatums[n] = Float4GetDatum(stats->stanumbers[k][n]);
				arry = construct_array_builtin(numdatums, nnum, FLOAT4OID);
				values[i++] = PointerGetDatum(arry);	/* stanumbersN（数值） */
			}
			else
			{
				nulls[i] = true;
				values[i++] = (Datum) 0;
			}
		}
		i = Anum_pg_statistic_stavalues1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			if (stats->numvalues[k] > 0)
			{
				ArrayType  *arry;

				arry = construct_array(stats->stavalues[k],
									   stats->numvalues[k],
									   stats->statypid[k],
									   stats->statyplen[k],
									   stats->statypbyval[k],
									   stats->statypalign[k]);
				values[i++] = PointerGetDatum(arry);	/* stavaluesN（值） */
			}
			else
			{
				nulls[i] = true;
				values[i++] = (Datum) 0;
			}
		}

		stup = heap_form_tuple(RelationGetDescr(sd), values, nulls);

		astate = accumArrayResult(astate,
								  heap_copy_tuple_as_datum(stup, RelationGetDescr(sd)),
								  false,
								  typOid,
								  CurrentMemoryContext);
	}

	table_close(sd, RowExclusiveLock);

	return makeArrayResult(astate, CurrentMemoryContext);
}

/*
 * 按提供的下标，从表达式统计里装载
 * pg_statistic 记录。
 */
HeapTuple
statext_expressions_load(Oid stxoid, bool inh, int idx)
{
	bool		isnull;
	Datum		value;
	HeapTuple	htup;
	ExpandedArrayHeader *eah;
	HeapTupleHeader td;
	HeapTupleData tmptup;
	HeapTuple	tup;

	htup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(stxoid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", stxoid);

	value = SysCacheGetAttr(STATEXTDATASTXOID, htup,
							Anum_pg_statistic_ext_data_stxdexpr, &isnull);
	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_EXPRESSIONS, stxoid);

	eah = DatumGetExpandedArray(value);

	deconstruct_expanded_array(eah);

	td = DatumGetHeapTupleHeader(eah->dvalues[idx]);

	/* 构造一个临时的 HeapTuple 控制结构 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = td;

	tup = heap_copytuple(&tmptup);

	ReleaseSysCache(htup);

	return tup;
}

/*
 * 对表达式求值，以便我们能用结果来建立
 * 所有请求的统计类型。这对
 * 昂贵的表达式尤其重要。
 */
static StatsBuildData *
make_build_data(Relation rel, StatExtEntry *stat, int numrows, HeapTuple *rows,
				VacAttrStats **stats, int stattarget)
{
	/* 已求值的表达式 */
	StatsBuildData *result;
	char	   *ptr;
	Size		len;

	int			i;
	int			k;
	int			idx;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	List	   *exprstates = NIL;
	int			nkeys = bms_num_members(stat->columns) + list_length(stat->exprs);
	ListCell   *lc;

	/* 把一切都分配成单个块，以便我们能轻松释放 */
	len = MAXALIGN(sizeof(StatsBuildData));
	len += MAXALIGN(sizeof(AttrNumber) * nkeys);	/* attnums（属性号） */
	len += MAXALIGN(sizeof(VacAttrStats *) * nkeys);	/* stats（统计） */

	/* values（值） */
	len += MAXALIGN(sizeof(Datum *) * nkeys);
	len += nkeys * MAXALIGN(sizeof(Datum) * numrows);

	/* nulls（空标志） */
	len += MAXALIGN(sizeof(bool *) * nkeys);
	len += nkeys * MAXALIGN(sizeof(bool) * numrows);

	ptr = palloc(len);

	/* 设置这些指针 */
	result = (StatsBuildData *) ptr;
	ptr += MAXALIGN(sizeof(StatsBuildData));

	/* attnums（属性号） */
	result->attnums = (AttrNumber *) ptr;
	ptr += MAXALIGN(sizeof(AttrNumber) * nkeys);

	/* stats（统计） */
	result->stats = (VacAttrStats **) ptr;
	ptr += MAXALIGN(sizeof(VacAttrStats *) * nkeys);

	/* values（值） */
	result->values = (Datum **) ptr;
	ptr += MAXALIGN(sizeof(Datum *) * nkeys);

	/* nulls（空标志） */
	result->nulls = (bool **) ptr;
	ptr += MAXALIGN(sizeof(bool *) * nkeys);

	for (i = 0; i < nkeys; i++)
	{
		result->values[i] = (Datum *) ptr;
		ptr += MAXALIGN(sizeof(Datum) * numrows);

		result->nulls[i] = (bool *) ptr;
		ptr += MAXALIGN(sizeof(bool) * numrows);
	}

	Assert((ptr - (char *) result) == len);

	/* 已经分配好了，因此把值填上 */
	result->nattnums = nkeys;
	result->numrows = numrows;

	/* 填充属性信息：先是属性，然后是表达式 */
	idx = 0;
	k = -1;
	while ((k = bms_next_member(stat->columns, k)) >= 0)
	{
		result->attnums[idx] = k;
		result->stats[idx] = stats[idx];

		idx++;
	}

	k = -1;
	foreach(lc, stat->exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		result->attnums[idx] = k;
		result->stats[idx] = examine_expression(expr, stattarget);

		idx++;
		k--;
	}

	/* 先为所有普通属性提取值 */
	for (i = 0; i < numrows; i++)
	{
		idx = 0;
		k = -1;
		while ((k = bms_next_member(stat->columns, k)) >= 0)
		{
			result->values[idx][i] = heap_getattr(rows[i], k,
												  result->stats[idx]->tupDesc,
												  &result->nulls[idx][i]);

			idx++;
		}
	}

	/* 对表达式求值需要一个 EState。 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);

	/* 也需要一个槽来保存当前的堆元组 */
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
									&TTSOpsHeapTuple);

	/* 安排 econtext 的扫描元组为正在测试的元组 */
	econtext->ecxt_scantuple = slot;

	/* 设置表达式求值状态 */
	exprstates = ExecPrepareExprList(stat->exprs, estate);

	for (i = 0; i < numrows; i++)
	{
		/*
		 * 每次重置按元组上下文，以回收求值统计对象表达式后留下的
		 * 任何废弃物。
		 */
		ResetExprContext(econtext);

		/* 为表达式求值做好准备 */
		ExecStoreHeapTuple(rows[i], slot, false);

		idx = bms_num_members(stat->columns);
		foreach(lc, exprstates)
		{
			Datum		datum;
			bool		isnull;
			ExprState  *exprstate = (ExprState *) lfirst(lc);

			/*
			 * XXX 这大概会泄漏内存。也许应当使用
			 * ExecEvalExprSwitchContext，但那样就需要把结果复制到
			 * 别的地方。
			 */
			datum = ExecEvalExpr(exprstate,
								 GetPerTupleExprContext(estate),
								 &isnull);
			if (isnull)
			{
				result->values[idx][i] = (Datum) 0;
				result->nulls[idx][i] = true;
			}
			else
			{
				result->values[idx][i] = (Datum) datum;
				result->nulls[idx][i] = false;
			}

			idx++;
		}
	}

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	return result;
}
