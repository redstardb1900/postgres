/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  Postgres 统计信息生成器
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_statistic_ext.h"
#include "commands/dbcommands.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/sampling.h"
#include "utils/sortsupport.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/* ANALYZE 的每个索引的数据 */
typedef struct AnlIndexData
{
	IndexInfo  *indexInfo;		/* BuildIndexInfo 结果 */
	double		tupleFract;		/* 部分索引的行占比 */
	VacAttrStats **vacattrstats;	/* 索引 attrs 到分析 */
	int			attr_cnt;
} AnlIndexData;


/* 默认统计信息目标（GUC 参数） */
int			default_statistics_target = 100;

/* 一些不值得作为参数传递的变量 */
static MemoryContext anl_context = NULL;
static BufferAccessStrategy vac_strategy;


static void do_analyze_rel(Relation onerel,
						   VacuumParams *params, List *va_cols,
						   AcquireSampleRowsFunc acquirefunc, BlockNumber relpages,
						   bool inh, bool in_outer_xact, int elevel);
static void compute_index_stats(Relation onerel, double totalrows,
								AnlIndexData *indexdata, int nindexes,
								HeapTuple *rows, int numrows,
								MemoryContext col_context);
static VacAttrStats *examine_attribute(Relation onerel, int attnum,
									   Node *index_expr);
static int	acquire_sample_rows(Relation onerel, int elevel,
								HeapTuple *rows, int targrows,
								double *totalrows, double *totaldeadrows);
static int	compare_rows(const void *a, const void *b, void *arg);
static int	acquire_inherited_sample_rows(Relation onerel, int elevel,
										  HeapTuple *rows, int targrows,
										  double *totalrows, double *totaldeadrows);
static void update_attstats(Oid relid, bool inh,
							int natts, VacAttrStats **vacattrstats);
static Datum std_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);
static Datum ind_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);


/*
 *	analyze_rel() —— 分析一个关系
 *
 * relid 标识要分析的关系。如果提供了关系，
 * 使用其中的名称报告任何打开/锁定关系的失败；
 * 一旦成功打开关系就不再使用它，因为它可能已过时。
 */
void
analyze_rel(Oid relid, RangeVar *relation,
			VacuumParams *params, List *va_cols, bool in_outer_xact,
			BufferAccessStrategy bstrategy)
{
	Relation	onerel;
	int			elevel;
	AcquireSampleRowsFunc acquirefunc = NULL;
	BlockNumber relpages = 0;

	/* 选择日志级别 */
	if (params->options & VACOPT_VERBOSE)
		elevel = INFO;
	else
		elevel = DEBUG2;

	/* 设置静态变量 */
	vac_strategy = bstrategy;

	/*
	 * 检查用户请求的中止。
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * 打开关系，获取 ShareUpdateExclusiveLock 以确保两个
	 * ANALYZE 不会并发运行在它上面。（这也会阻止
	 * 并发 VACUUM，目前影响不大，但可能
	 * 在我们尝试累积死元组统计信息时有影响。）如果关系
	 * 自上次看到后已被删除，则无需处理。
	 *
	 * 在这种情况下确保仅为 ANALYZE 生成日志。
	 */
	onerel = vacuum_open_relation(relid, relation, params->options & ~(VACOPT_VACUUM),
								  params->log_min_duration >= 0,
								  ShareUpdateExclusiveLock);

	/* 如果无法打开或锁定关系则离开 */
	if (!onerel)
		return;

	/*
	 * 基于所有权检查关系是否需要跳过。此检查
	 * 在构建手动操作的分析关系列表时也会进行，
	 * 需要在此额外执行，因为 ANALYZE 可能
	 * 跨多个事务发生，其中关系所有权可能
	 * 在此期间发生变化。在这种情况下确保仅为
	 * ANALYZE 生成日志。
	 */
	if (!vacuum_is_relation_owner(RelationGetRelid(onerel),
								  onerel->rd_rel,
								  params->options & VACOPT_ANALYZE))
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * 静默忽略其他后端的临时表 ---
	 * 尝试分析这些表毫无意义，因为它们的内容
	 * 在磁盘上可能不是最新的。（不在此处抛出警告；
	 * 这只会导致全数据库 ANALYZE 期间的噪音。）
	 */
	if (RELATION_IS_OTHER_TEMP(onerel))
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * 我们可以 ANALYZE 除 pg_statistic 之外的任何表。参见 update_attstats
	 */
	if (RelationGetRelid(onerel) == StatisticRelationId)
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * 检查它是否属于可分析的 relkind，并进行适当设置。
	 */
	if (onerel->rd_rel->relkind == RELKIND_RELATION ||
		onerel->rd_rel->relkind == RELKIND_MATVIEW)
	{
		/* 普通表，因此使用普通的行获取函数 */
		acquirefunc = acquire_sample_rows;
		/* 同时获取普通表的大小 */
		relpages = RelationGetNumberOfBlocks(onerel);
	}
	else if (onerel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * 对于外部表，调用 FDW 的钩子函数来查看它是否
		 * 支持分析。
		 */
		FdwRoutine *fdwroutine;
		bool		ok = false;

		fdwroutine = GetFdwRoutineForRelation(onerel, false);

		if (fdwroutine->AnalyzeForeignTable != NULL)
			ok = fdwroutine->AnalyzeForeignTable(onerel,
												 &acquirefunc,
												 &relpages);

		if (!ok)
		{
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot analyze this foreign table",
							RelationGetRelationName(onerel))));
			relation_close(onerel, ShareUpdateExclusiveLock);
			return;
		}
	}
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * 对于分区表，我们想在下面执行递归 ANALYZE。
		 */
	}
	else
	{
		/* 如果在 VACUUM 期间已警告过，则无需 WARNING */
		if (!(params->options & VACOPT_VACUUM))
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot analyze non-tables or special system tables",
							RelationGetRelationName(onerel))));
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * 好的，开始。首先，初始化进度报告。
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_ANALYZE,
								  RelationGetRelid(onerel));

	/*
	 * 执行普通的非递归 ANALYZE。对于分区表可以跳过，
	 * 因为它们不包含任何行。
	 */
	if (onerel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		do_analyze_rel(onerel, params, va_cols, acquirefunc,
					   relpages, false, in_outer_xact, elevel);

	/*
	 * 如果有子表，则执行递归 ANALYZE。
	 */
	if (onerel->rd_rel->relhassubclass)
		do_analyze_rel(onerel, params, va_cols, acquirefunc, relpages,
					   true, in_outer_xact, elevel);

	/*
	 * 现在关闭源关系，但保留锁，防止被删除
	 * （如果被删除，将无法清理我们在 pg_statistic 中
	 * 创建的条目。此外，在提交前释放锁会导致
	 * update_attstats 中出现并发更新失败。）
	 */
	relation_close(onerel, NoLock);

	pgstat_progress_end_command();
}

/*
 *	do_analyze_rel() —— 分析一个关系，递归或不递归
 *
 * Note 那个 "acquirefunc" is 仅 relevant 对于 the non-inherited case.
 * For the inherited case, acquire_inherited_sample_rows() determines the
 * appropriate acquirefunc 对于每个子表.
 */
static void
do_analyze_rel(Relation onerel, VacuumParams *params,
			   List *va_cols, AcquireSampleRowsFunc acquirefunc,
			   BlockNumber relpages, bool inh, bool in_outer_xact,
			   int elevel)
{
	int			attr_cnt,
				tcnt,
				i,
				ind;
	Relation   *Irel;
	int			nindexes;
	bool		hasindex;
	VacAttrStats **vacattrstats;
	AnlIndexData *indexdata;
	int			targrows,
				numrows,
				minrows;
	double		totalrows,
				totaldeadrows;
	HeapTuple  *rows;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	MemoryContext caller_context;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	int64		AnalyzePageHit = VacuumPageHit;
	int64		AnalyzePageMiss = VacuumPageMiss;
	int64		AnalyzePageDirty = VacuumPageDirty;
	PgStat_Counter startreadtime = 0;
	PgStat_Counter startwritetime = 0;

	if (inh)
		ereport(elevel,
				(errmsg("analyzing \"%s.%s\" inheritance tree",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
	else
		ereport(elevel,
				(errmsg("analyzing \"%s.%s\"",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));

	/*
	 * 设置工作上下文，以便轻松释放过程中产生的
	 * 任何临时内容。
	 */
	anl_context = AllocSetContextCreate(CurrentMemoryContext,
										"Analyze",
										ALLOCSET_DEFAULT_SIZES);
	caller_context = MemoryContextSwitchTo(anl_context);

	/*
	 * 切换到表所有者的用户 ID，以便索引函数以该用户身份
	 * 运行。同时锁定安全受限操作，并
	 * 安排使 GUC 变量更改仅限于此命令。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(onerel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/* 仅在自动清理日志记录需要时测量经过时间 */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0)
	{
		if (track_io_timing)
		{
			startreadtime = pgStatBlockReadTime;
			startwritetime = pgStatBlockWriteTime;
		}

		pg_rusage_init(&ru0);
		starttime = GetCurrentTimestamp();
	}

	/*
	 * 确定要分析哪些列
	 *
	 * 注意，系统属性从不被分析，因此直接在
	 * 查找阶段拒绝它们。我们也拒绝重复的列引用。（
	 * 可以忽略重复项，但分析一列两次
	 * 行不通；最终会在 pg_statistic 中产生冲突更新。）
	 */
	if (va_cols != NIL)
	{
		Bitmapset  *unique_cols = NULL;
		ListCell   *le;

		vacattrstats = (VacAttrStats **) palloc(list_length(va_cols) *
												sizeof(VacAttrStats *));
		tcnt = 0;
		foreach(le, va_cols)
		{
			char	   *col = strVal(lfirst(le));

			i = attnameAttNum(onerel, col, false);
			if (i == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								col, RelationGetRelationName(onerel))));
			if (bms_is_member(i, unique_cols))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" appears more than once",
								col, RelationGetRelationName(onerel))));
			unique_cols = bms_add_member(unique_cols, i);

			vacattrstats[tcnt] = examine_attribute(onerel, i, NULL);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}
	else
	{
		attr_cnt = onerel->rd_att->natts;
		vacattrstats = (VacAttrStats **)
			palloc(attr_cnt * sizeof(VacAttrStats *));
		tcnt = 0;
		for (i = 1; i <= attr_cnt; i++)
		{
			vacattrstats[tcnt] = examine_attribute(onerel, i, NULL);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}

	/*
	 * 打开关系的所有索引，查看是否有任何可分析的
	 * 列在索引中。如果 ANALYZE 命令中有显式的列列表，
	 * 则我们不分析索引列。
	 *
	 * 如果执行递归扫描，我们不想触及父表的
	 * 索引。如果处理的是分区表，我们需要
	 * 知道是否有索引，但不想处理它们。
	 */
	if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *idxs = RelationGetIndexList(onerel);

		Irel = NULL;
		nindexes = 0;
		hasindex = idxs != NIL;
		list_free(idxs);
	}
	else if (!inh)
	{
		vac_open_indexes(onerel, AccessShareLock, &nindexes, &Irel);
		hasindex = nindexes > 0;
	}
	else
	{
		Irel = NULL;
		nindexes = 0;
		hasindex = false;
	}
	indexdata = NULL;
	if (nindexes > 0)
	{
		indexdata = (AnlIndexData *) palloc0(nindexes * sizeof(AnlIndexData));
		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];
			IndexInfo  *indexInfo;

			thisdata->indexInfo = indexInfo = BuildIndexInfo(Irel[ind]);
			thisdata->tupleFract = 1.0; /* 如果是部分索引稍后修正 */
			if (indexInfo->ii_Expressions != NIL && va_cols == NIL)
			{
				ListCell   *indexpr_item = list_head(indexInfo->ii_Expressions);

				thisdata->vacattrstats = (VacAttrStats **)
					palloc(indexInfo->ii_NumIndexAttrs * sizeof(VacAttrStats *));
				tcnt = 0;
				for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
				{
					int			keycol = indexInfo->ii_IndexAttrNumbers[i];

					if (keycol == 0)
					{
						/* 发现索引表达式 */
						Node	   *indexkey;

						if (indexpr_item == NULL)	/* 不应发生 */
							elog(ERROR, "too few entries in indexprs list");
						indexkey = (Node *) lfirst(indexpr_item);
						indexpr_item = lnext(indexInfo->ii_Expressions,
											 indexpr_item);
						thisdata->vacattrstats[tcnt] =
							examine_attribute(Irel[ind], i + 1, indexkey);
						if (thisdata->vacattrstats[tcnt] != NULL)
							tcnt++;
					}
				}
				thisdata->attr_cnt = tcnt;
			}
		}
	}

	/*
	 * 使用所有可分析列的最坏情况，确定需要采样
	 * 多少行。使用 100 行的下限以避免
	 * Vitter 算法中可能的溢出。（注意：在没有
	 * 可分析列的边界情况下，这也将是目标值。）
	 */
	targrows = 100;
	for (i = 0; i < attr_cnt; i++)
	{
		if (targrows < vacattrstats[i]->minrows)
			targrows = vacattrstats[i]->minrows;
	}
	for (ind = 0; ind < nindexes; ind++)
	{
		AnlIndexData *thisdata = &indexdata[ind];

		for (i = 0; i < thisdata->attr_cnt; i++)
		{
			if (targrows < thisdata->vacattrstats[i]->minrows)
				targrows = thisdata->vacattrstats[i]->minrows;
		}
	}

	/*
	 * 同时检查扩展统计信息对象，因为它们可能定义自定义
	 * 统计信息目标。因此可能需要采样更多行，然后构建
	 * 足够详细的统计信息。
	 */
	minrows = ComputeExtStatisticsRows(onerel, attr_cnt, vacattrstats);

	if (targrows < minrows)
		targrows = minrows;

	/*
	 * 获取采样行
	 */
	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
								 inh ? PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS_INH :
								 PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS);
	if (inh)
		numrows = acquire_inherited_sample_rows(onerel, elevel,
												rows, targrows,
												&totalrows, &totaldeadrows);
	else
		numrows = (*acquirefunc) (onerel, elevel,
								  rows, targrows,
								  &totalrows, &totaldeadrows);

	/*
	 * 计算统计信息。每列计算期间的临时结果
	 * 存储在子上下文中。计算函数负责确保
	 * 存储到 VacAttrStats 结构中的任何内容
	 * 都分配在 anl_context 中。
	 */
	if (numrows > 0)
	{
		MemoryContext col_context,
					old_context;

		pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
									 PROGRESS_ANALYZE_PHASE_COMPUTE_STATS);

		col_context = AllocSetContextCreate(anl_context,
											"Analyze Column",
											ALLOCSET_DEFAULT_SIZES);
		old_context = MemoryContextSwitchTo(col_context);

		for (i = 0; i < attr_cnt; i++)
		{
			VacAttrStats *stats = vacattrstats[i];
			AttributeOpts *aopt;

			stats->rows = rows;
			stats->tupDesc = onerel->rd_att;
			stats->compute_stats(stats,
								 std_fetch_func,
								 numrows,
								 totalrows);

			/*
			 * 如果指定了适当类型的 n_distinct 选项，
			 * 则用相应的值覆盖。
			 */
			aopt = get_attribute_options(onerel->rd_id, stats->attr->attnum);
			if (aopt != NULL)
			{
				float8		n_distinct;

				n_distinct = inh ? aopt->n_distinct_inherited : aopt->n_distinct;
				if (n_distinct != 0.0)
					stats->stadistinct = n_distinct;
			}

			MemoryContextResetAndDeleteChildren(col_context);
		}

		if (nindexes > 0)
			compute_index_stats(onerel, totalrows,
								indexdata, nindexes,
								rows, numrows,
								col_context);

		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(col_context);

		/*
		 * 将完成的统计信息行写入 pg_statistic，替换目标列
		 * 之前的任何统计信息。（如果 pg_statistic 中有
		 * 我们未处理的列的统计信息，则保持不动。）
		 */
		update_attstats(RelationGetRelid(onerel), inh,
						attr_cnt, vacattrstats);

		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];

			update_attstats(RelationGetRelid(Irel[ind]), false,
							thisdata->attr_cnt, thisdata->vacattrstats);
		}

		/* 构建扩展统计信息（如果有的话）。 */
		BuildRelationExtStatistics(onerel, inh, totalrows, numrows, rows,
								   attr_cnt, vacattrstats);
	}

	pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
								 PROGRESS_ANALYZE_PHASE_FINALIZE_ANALYZE);

	/*
	 * 更新 pg_class 中的页/元组统计信息...但如果是
	 * 继承统计信息则不更新。
	 *
	 * 我们假设 VACUUM 尚未设置 pg_class.reltuples，即使
	 * 在 VACUUM ANALYZE 期间也是如此。尽管 VACUUM 经常更新 pg_class，
	 * 但存在例外。"VACUUM (ANALYZE, INDEX_CLEANUP OFF)" 命令
	 * 永远不会更新索引关系的 pg_class 条目。也有可能
	 * 个别索引的 pg_class 条目在 VACUUM 期间不会被更新，
	 * 如果索引 AM 的 amvacuumcleanup() 函数返回 NULL。
	 */
	if (!inh)
	{
		BlockNumber relallvisible;

		visibilitymap_count(onerel, &relallvisible, NULL);

		/*
		 * 更新表关系的 pg_class。先执行 CCI，以防 acquirefunc
		 * 更新了 pg_class。
		 */
		CommandCounterIncrement();
		vac_update_relstats(onerel,
							relpages,
							totalrows,
							relallvisible,
							hasindex,
							InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL,
							in_outer_xact);

		/* 索引同理 */
		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];
			double		totalindexrows;

			totalindexrows = ceil(thisdata->tupleFract * totalrows);
			vac_update_relstats(Irel[ind],
								RelationGetNumberOfBlocks(Irel[ind]),
								totalindexrows,
								0,
								false,
								InvalidTransactionId,
								InvalidMultiXactId,
								NULL, NULL,
								in_outer_xact);
		}
	}
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * 分区表没有存储，因此不在其 pg_class 条目中
		 * 设置任何字段，除了 reltuples 和 relhasindex。
		 */
		CommandCounterIncrement();
		vac_update_relstats(onerel, -1, totalrows,
							0, hasindex, InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL,
							in_outer_xact);
	}

	/*
	 * 现在向累积统计系统报告 ANALYZE。对于普通表，
	 * 仅在非继承统计时执行。对于分区表，
	 * 仅在继承统计时执行。（我们不会被以非继承方式
	 * 调用分区表的统计信息。）
	 *
	 * 仅在分析了所有列时才重置 changes_since_analyze
	 * 计数器；否则，自动分析仍有工作要做。
	 */
	if (!inh)
		pgstat_report_analyze(onerel, totalrows, totaldeadrows,
							  (va_cols == NIL));
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		pgstat_report_analyze(onerel, 0, 0, (va_cols == NIL));

	/*
	 * 如果这不是 VACUUM ANALYZE 的一部分，让索引 AM 进行清理。
	 *
	 * 注意，大多数索引 AM 在仅 ANALYZE 模式下调用
	 * amvacuumcleanup() 时默认执行空操作。核心索引 AM 中
	 * 唯一的例外是 GIN/ginvacuumcleanup()。
	 */
	if (!(params->options & VACOPT_VACUUM))
	{
		for (ind = 0; ind < nindexes; ind++)
		{
			IndexBulkDeleteResult *stats;
			IndexVacuumInfo ivinfo;

			ivinfo.index = Irel[ind];
			ivinfo.heaprel = onerel;
			ivinfo.analyze_only = true;
			ivinfo.estimated_count = true;
			ivinfo.message_level = elevel;
			ivinfo.num_heap_tuples = onerel->rd_rel->reltuples;
			ivinfo.strategy = vac_strategy;

			stats = index_vacuum_cleanup(&ivinfo, NULL);

			if (stats)
				pfree(stats);
		}
	}

	/* 索引处理完成 */
	vac_close_indexes(nindexes, Irel, NoLock);

	/* 在适当情况下记录操作 */
	if (IsAutoVacuumWorkerProcess() && params->log_min_duration >= 0)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (params->log_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params->log_min_duration))
		{
			long		delay_in_ms;
			double		read_rate = 0;
			double		write_rate = 0;
			StringInfoData buf;

			/*
			 * 计算分析过程中发生的页命中/未命中/脏页的
			 * 差值，通过减去上面保存的
			 * 分析前值。
			 */
			AnalyzePageHit = VacuumPageHit - AnalyzePageHit;
			AnalyzePageMiss = VacuumPageMiss - AnalyzePageMiss;
			AnalyzePageDirty = VacuumPageDirty - AnalyzePageDirty;

			/*
			 * 我们不认为分析会超过 25 天，而且使用
			 * TimestampDifferenceMilliseconds 可简化一些操作。
			 */
			delay_in_ms = TimestampDifferenceMilliseconds(starttime, endtime);

			/*
			 * 注意，我们以与 VACUUM 相同的方式报告这些读/写速率，
			 * 这意味着虽然这里的 'avg 读取速率' 实际上对应于
			 * 页缺失和由此产生的读取（同时被 track_io_timing
			 * 在启用时捕获），
			 * 'avg 写入速率' 实际上是指页被标记为脏的速率
			 * 而非写出速率，因此通常在 I/O 计时仅报告
			 * 读取时有非零的 'avg 写入速率'。
			 * 
			 *
			 * 不清楚 ANALYZE 是否会导致
			 * 调用 FlushBuffer()，但我们跟踪并支持报告
			 * I/O 写入时间，以防这发生变化，因为这实际上
			 * 不花什么成本。
			 */

			if (delay_in_ms > 0)
			{
				read_rate = (double) BLCKSZ * AnalyzePageMiss / (1024 * 1024) /
					(delay_in_ms / 1000.0);
				write_rate = (double) BLCKSZ * AnalyzePageDirty / (1024 * 1024) /
					(delay_in_ms / 1000.0);
			}

			/*
			 * 我们将此拆分，以便在未启用
			 * track_io_timing 时不输出空的 I/O 计时值。
			 */

			initStringInfo(&buf);
			appendStringInfo(&buf, _("automatic analyze of table \"%s.%s.%s\"\n"),
							 get_database_name(MyDatabaseId),
							 get_namespace_name(RelationGetNamespace(onerel)),
							 RelationGetRelationName(onerel));
			if (track_io_timing)
			{
				double		read_ms = (double) (pgStatBlockReadTime - startreadtime) / 1000;
				double		write_ms = (double) (pgStatBlockWriteTime - startwritetime) / 1000;

				appendStringInfo(&buf, _("I/O timings: read: %.3f ms, write: %.3f ms\n"),
								 read_ms, write_ms);
			}
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			appendStringInfo(&buf, _("buffer usage: %lld hits, %lld misses, %lld dirtied\n"),
							 (long long) AnalyzePageHit,
							 (long long) AnalyzePageMiss,
							 (long long) AnalyzePageDirty);
			appendStringInfo(&buf, _("system usage: %s"), pg_rusage_show(&ru0));

			ereport(LOG,
					(errmsg_internal("%s", buf.data)));

			pfree(buf.data);
		}
	}

	/* 回滚索引函数执行的任何 GUC 更改 */
	AtEOXact_GUC(false, save_nestlevel);

	/* 恢复 userid 和安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* 恢复当前上下文并释放内存 */
	MemoryContextSwitchTo(caller_context);
	MemoryContextDelete(anl_context);
	anl_context = NULL;
}

/*
 * 计算关系索引的统计信息
 */
static void
compute_index_stats(Relation onerel, double totalrows,
					AnlIndexData *indexdata, int nindexes,
					HeapTuple *rows, int numrows,
					MemoryContext col_context)
{
	MemoryContext ind_context,
				old_context;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			ind,
				i;

	ind_context = AllocSetContextCreate(anl_context,
										"Analyze Index",
										ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(ind_context);

	for (ind = 0; ind < nindexes; ind++)
	{
		AnlIndexData *thisdata = &indexdata[ind];
		IndexInfo  *indexInfo = thisdata->indexInfo;
		int			attr_cnt = thisdata->attr_cnt;
		TupleTableSlot *slot;
		EState	   *estate;
		ExprContext *econtext;
		ExprState  *predicate;
		Datum	   *exprvals;
		bool	   *exprnulls;
		int			numindexrows,
					tcnt,
					rowno;
		double		totalindexrows;

		/* 如果没有要分析的列且不是部分索引，则忽略该索引 */
		if (attr_cnt == 0 && indexInfo->ii_Predicate == NIL)
			continue;

		/*
		 * 需要 EState 用于求值索引表达式和
		 * 部分索引谓词。在逐索引上下文中创建以确保
		 * 在循环底部被清理。
		 */
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);
		/* 还需要一个槽来存放当前堆元组 */
		slot = MakeSingleTupleTableSlot(RelationGetDescr(onerel),
										&TTSOpsHeapTuple);

		/* 安排 econtext 的扫描元组为被测元组 */
		econtext->ecxt_scantuple = slot;

		/* 设置谓词的执行状态。 */
		predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

		/* 计算并保存索引表达式值 */
		exprvals = (Datum *) palloc(numrows * attr_cnt * sizeof(Datum));
		exprnulls = (bool *) palloc(numrows * attr_cnt * sizeof(bool));
		numindexrows = 0;
		tcnt = 0;
		for (rowno = 0; rowno < numrows; rowno++)
		{
			HeapTuple	heapTuple = rows[rowno];

			vacuum_delay_point();

			/*
			 * 每次重置逐元组上下文，以回收先前求值
			 * 谓词或索引表达式时留下的任何无用内容。
			 */
			ResetExprContext(econtext);

			/* 设置谓词或表达式求值 */
			ExecStoreHeapTuple(heapTuple, slot, false);

			/* 如果索引是部分的，检查谓词 */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}
			numindexrows++;

			if (attr_cnt > 0)
			{
				/*
				 * 求值索引行以计算表达式值。我们可以
				 * 手动完成，但 FormIndexDatum 更方便。
				 */
				FormIndexDatum(indexInfo,
							   slot,
							   estate,
							   values,
							   isnull);

				/*
				 * 仅保存我们关心的列。我们将值复制
				 * 到 ind_context 中，从 estate 的逐元组上下文。
				 */
				for (i = 0; i < attr_cnt; i++)
				{
					VacAttrStats *stats = thisdata->vacattrstats[i];
					int			attnum = stats->attr->attnum;

					if (isnull[attnum - 1])
					{
						exprvals[tcnt] = (Datum) 0;
						exprnulls[tcnt] = true;
					}
					else
					{
						exprvals[tcnt] = datumCopy(values[attnum - 1],
												   stats->attrtype->typbyval,
												   stats->attrtype->typlen);
						exprnulls[tcnt] = false;
					}
					tcnt++;
				}
			}
		}

		/*
		 * 计算出采样中通过谓词的行数后，
		 * 我们可以估计索引中的总行数。
		 */
		thisdata->tupleFract = (double) numindexrows / (double) numrows;
		totalindexrows = ceil(thisdata->tupleFract * totalrows);

		/*
		 * 现在可以计算表达式列的统计信息。
		 */
		if (numindexrows > 0)
		{
			MemoryContextSwitchTo(col_context);
			for (i = 0; i < attr_cnt; i++)
			{
				VacAttrStats *stats = thisdata->vacattrstats[i];

				stats->exprvals = exprvals + i;
				stats->exprnulls = exprnulls + i;
				stats->rowstride = attr_cnt;
				stats->compute_stats(stats,
									 ind_fetch_func,
									 numindexrows,
									 totalindexrows);

				MemoryContextResetAndDeleteChildren(col_context);
			}
		}

		/* 然后清理 */
		MemoryContextSwitchTo(ind_context);

		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
		MemoryContextResetAndDeleteChildren(ind_context);
	}

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(ind_context);
}

/*
 * examine_attribute —— 单个列的预分析
 *
 * 确定列是否可分析；如果可以，创建并初始化
 * 它的 VacAttrStats 结构体。如果不可分析，返回 NULL。
 *
 * 如果 index_expr 不为 NULL，则我们正尝试分析表达式索引，
 * 且 index_expr 是表示该列数据的表达式树。
 */
static VacAttrStats *
examine_attribute(Relation onerel, int attnum, Node *index_expr)
{
	Form_pg_attribute attr = TupleDescAttr(onerel->rd_att, attnum - 1);
	HeapTuple	typtuple;
	VacAttrStats *stats;
	int			i;
	bool		ok;

	/* 从不分析已删除的列 */
	if (attr->attisdropped)
		return NULL;

	/* 如果用户指定不分析，则不分析该列 */
	if (attr->attstattarget == 0)
		return NULL;

	/*
	 * 创建 VacAttrStats 结构体。注意，我们只有
	 * pg_attribute 元组的固定字段的副本。
	 */
	stats = (VacAttrStats *) palloc0(sizeof(VacAttrStats));
	stats->attr = (Form_pg_attribute) palloc(ATTRIBUTE_FIXED_PART_SIZE);
	memcpy(stats->attr, attr, ATTRIBUTE_FIXED_PART_SIZE);

	/*
	 * 分析表达式索引时，相信表达式树的类型
	 * 而非列数据类型 —— 后者可能是 opclass 的 opckeytype
	 * 存储类型，这并非我们关心的。（注意：
	 * 如果我们处理非表达式索引列，需要
	 * 弄清楚从何处获取正确的类型信息，但目前
	 * 这不是问题。）	不清楚是否有人关心
	 * typmod，但为防万一我们也存储它。
	 */
	if (index_expr)
	{
		stats->attrtypid = exprType(index_expr);
		stats->attrtypmod = exprTypmod(index_expr);

		/*
		 * 如果为索引列指定了排序规则，优先使用它；
		 * 如果没有，则回退到我们能从
		 * 表达式中获取的任何内容。
		 */
		if (OidIsValid(onerel->rd_indcollation[attnum - 1]))
			stats->attrcollid = onerel->rd_indcollation[attnum - 1];
		else
			stats->attrcollid = exprCollation(index_expr);
	}
	else
	{
		stats->attrtypid = attr->atttypid;
		stats->attrtypmod = attr->atttypmod;
		stats->attrcollid = attr->attcollation;
	}

	typtuple = SearchSysCacheCopy1(TYPEOID,
								   ObjectIdGetDatum(stats->attrtypid));
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup failed for type %u", stats->attrtypid);
	stats->attrtype = (Form_pg_type) GETSTRUCT(typtuple);
	stats->anl_context = anl_context;
	stats->tupattnum = attnum;

	/*
	 * 描述统计信息->stavalues[n] 元素类型的字段默认为
	 * 被分析数据的类型，但类型特定的 typanalyze
	 * 函数可在需要存储其他内容时更改它们。
	 */
	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		stats->statypid[i] = stats->attrtypid;
		stats->statyplen[i] = stats->attrtype->typlen;
		stats->statypbyval[i] = stats->attrtype->typbyval;
		stats->statypalign[i] = stats->attrtype->typalign;
	}

	/*
	 * 调用类型特定的 typanalyze 函数。如果未指定，则使用
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
 * acquire_sample_rows —— 从表中获取随机行采样
 *
 * 选中的行返回在调用者分配的数组 rows[] 中，
 * 该数组必须至少有 targrows 个条目。
 * The actual 数字 of rows selected is returned 作为 the 函数 result.
 * 我们还估计表中的活行和死行总数，
 * 并将它们分别返回到 *totalrows 和 *totaldeadrows 中。
 *
 * 返回的元组列表按表中物理位置排序。
 * （我们稍后将依赖此排序来推导相关度估计。）
 *
 * 自 2004 年 5 月起我们使用新的两阶段方法：第一阶段选择最多
 * targrows 个随机块（如果没有那么多，则选择所有块）。
 * 第二阶段扫描这些块并使用 Vitter 算法创建
 * targrows 行的随机采样（如果块采样中行数较少则更少）。
 * 两个阶段同时执行：每个块
 * 在阶段一返回其编号后立即处理，同时
 * 在读取行时阶段二控制哪些行被插入
 * 到采样中。
 *
 * 尽管每行有均等的机会出现在最终
 * 采样中，此采样方法并不完美：并非每个可能的
 * 采样都有均等的被选中机会。对于大关系，
 * 采样所代表的不同块的数量往往
 * 太少。目前我们能接受。欢迎改进。
 *
 * 此采样方法的一个重要特性是，因为我们确实
 * 查看了统计无偏的块集，我们应当获得
 * 每个块平均活行和死行数的无偏估计。
 * 之前的采样方法过于相信表起始处附近的
 * 行密度。
 */
static int
acquire_sample_rows(Relation onerel, int elevel,
					HeapTuple *rows, int targrows,
					double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;	/* 水库中当前的行数 */
	double		samplerows = 0; /* 收集的总行数 */
	double		liverows = 0;	/* 看到的活行数 */
	double		deadrows = 0;	/* # 死行 seen */
	double		rowstoskip = -1;	/* -1 means 未设置尚未 */
	uint32		randseed;		/* Seed 对于块 sampler(s) */
	BlockNumber totalblocks;
	TransactionId OldestXmin;
	BlockSamplerData bs;
	ReservoirStateData rstate;
	TupleTableSlot *slot;
	TableScanDesc scan;
	BlockNumber nblocks;
	BlockNumber blksdone = 0;
#ifdef USE_PREFETCH
	int			prefetch_maximum = 0;	/* blocks 到 prefetch 如果 enabled */
	BlockSamplerData prefetch_bs;
#endif

	Assert(targrows > 0);

	totalblocks = RelationGetNumberOfBlocks(onerel);

	/* 需要 HeapTupleSatisfiesVacuum 的截断 xmin */
	OldestXmin = GetOldestNonRemovableTransactionId(onerel);

	/* 准备采样块号 */
	randseed = pg_prng_uint32(&pg_global_prng_state);
	nblocks = BlockSampler_Init(&bs, totalblocks, targrows, randseed);

#ifdef USE_PREFETCH
	prefetch_maximum = get_tablespace_maintenance_io_concurrency(onerel->rd_rel->reltablespace);
	/* 创建另一个 BlockSampler，使用相同的种子，用于预取 */
	if (prefetch_maximum)
		(void) BlockSampler_Init(&prefetch_bs, totalblocks, targrows, randseed);
#endif

	/* 报告采样块号 */
	pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_TOTAL,
								 nblocks);

	/* 准备采样行 */
	reservoir_init_selection_state(&rstate, targrows);

	scan = table_beginscan_analyze(onerel);
	slot = table_slot_create(onerel, NULL);

#ifdef USE_PREFETCH

	/*
	 * If we are doing prefetching, 然后 go ahead 和 tell the kernel about
	 * the 首先集合 of pages we are going 到 want.  This 还 moves our
	 * iterator out ahead of the main 一 being used, where we 将 keep it so
	 * 那个 we're 始终 pre-fetching out prefetch_maximum 数字 of blocks
	 * ahead.
	 */
	if (prefetch_maximum)
	{
		for (int i = 0; i < prefetch_maximum; i++)
		{
			BlockNumber prefetch_block;

			if (!BlockSampler_HasMore(&prefetch_bs))
				break;

			prefetch_block = BlockSampler_Next(&prefetch_bs);
			PrefetchBuffer(scan->rs_rd, MAIN_FORKNUM, prefetch_block);
		}
	}
#endif

	/* 要采样的块的外层循环 */
	while (BlockSampler_HasMore(&bs))
	{
		bool		block_accepted;
		BlockNumber targblock = BlockSampler_Next(&bs);
#ifdef USE_PREFETCH
		BlockNumber prefetch_targblock = InvalidBlockNumber;

		/*
		 * 确保每次主 BlockSampler 向前移动时，
		 * 预取 BlockSampler 也向前移动，以便我们
		 * 始终保持在前面。
		 */
		if (prefetch_maximum && BlockSampler_HasMore(&prefetch_bs))
			prefetch_targblock = BlockSampler_Next(&prefetch_bs);
#endif

		vacuum_delay_point();

		block_accepted = table_scan_analyze_next_block(scan, targblock, vac_strategy);

#ifdef USE_PREFETCH

		/*
		 * 预取时，获取一个块后，告诉内核下一个
		 * 我们想要的块，如果还有剩余的话。
		 *
		 * 即使 table_scan_analyze_next_block() 调用
		 * 决定不分析其选择的块，我们也想这样做。
		 */
		if (prefetch_maximum && prefetch_targblock != InvalidBlockNumber)
			PrefetchBuffer(scan->rs_rd, MAIN_FORKNUM, prefetch_targblock);
#endif

		/*
		 * 如果 table_scan_analyze_next_block() 指示此块
		 * 不适合分析，则不分析。
		 */
		if (!block_accepted)
			continue;

		while (table_scan_analyze_next_tuple(scan, OldestXmin, &liverows, &deadrows, slot))
		{
			/*
			 * 前 targrows 个采样行直接复制到
			 * 水库中。然后我们开始替换采样中的元组，直到
			 * 到达关系的末尾。此算法来自 Jeff
			 * Vitter 的论文（完整引用见 utils/misc/采样.c）。它
			 * 通过重复计算在选取一个元组之前需要跳过的
			 * 元组数来工作，该元组替换水库（当前元组集）中
			 * 随机选取的一个元素。在任何时候，
			 * 水库都是我们迄今遍历过的元组的真正随机采样，
			 * 因此当我们到达关系末尾时，
			 * 就完成了。
			 */
			if (numrows < targrows)
				rows[numrows++] = ExecCopySlotHeapTuple(slot);
			else
			{
				/*
				 * Vitter 论文中的 t 是已处理的记录数。
				 * 如果需要计算新的 S 值，必须
				 * 使用 samplerows 尚未递增的值作为 t。
				 */
				if (rowstoskip < 0)
					rowstoskip = reservoir_get_next_S(&rstate, samplerows, targrows);

				if (rowstoskip <= 0)
				{
					/*
					 * 找到合适的元组，保存它，随机替换一个
					 * 旧元组
					 */
					int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

					Assert(k >= 0 && k < targrows);
					heap_freetuple(rows[k]);
					rows[k] = ExecCopySlotHeapTuple(slot);
				}

				rowstoskip -= 1;
			}

			samplerows += 1;
		}

		pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_DONE,
									 ++blksdone);
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	/*
	 * 如果没有找到足够多的元组，则已完成。不需要
	 * 排序，因为它们已经有序。
	 *
	 * 否则需要按位置对收集的元组进行排序
	 * （itempointer）。不需要担心元组已经排序的
	 * 边界情况。
	 */
	if (numrows == targrows)
		qsort_interruptible(rows, numrows, sizeof(HeapTuple),
							compare_rows, NULL);

	/*
	 * 估计关系中的活行和死行总数，基于以下假设
	 * 外推：我们未扫描的页中的平均元组密度
	 * 与已扫描的页相同。由于我们扫描的是
	 * 关系中页的随机采样，这应该是一个好的
	 * 假设。
	 */
	if (bs.m > 0)
	{
		*totalrows = floor((liverows / bs.m) * totalblocks + 0.5);
		*totaldeadrows = floor((deadrows / bs.m) * totalblocks + 0.5);
	}
	else
	{
		*totalrows = 0.0;
		*totaldeadrows = 0.0;
	}

	/*
	 * 输出一些有趣的关系信息
	 */
	ereport(elevel,
			(errmsg("\"%s\": scanned %d of %u pages, "
					"containing %.0f live rows and %.0f dead rows; "
					"%d rows in sample, %.0f estimated total rows",
					RelationGetRelationName(onerel),
					bs.m, totalblocks,
					liverows, deadrows,
					numrows, *totalrows)));

	return numrows;
}

/*
 * 用于排序 rows[] 数组的比较器
 */
static int
compare_rows(const void *a, const void *b, void *arg)
{
	HeapTuple	ha = *(const HeapTuple *) a;
	HeapTuple	hb = *(const HeapTuple *) b;
	BlockNumber ba = ItemPointerGetBlockNumber(&ha->t_self);
	OffsetNumber oa = ItemPointerGetOffsetNumber(&ha->t_self);
	BlockNumber bb = ItemPointerGetBlockNumber(&hb->t_self);
	OffsetNumber ob = ItemPointerGetOffsetNumber(&hb->t_self);

	if (ba < bb)
		return -1;
	if (ba > bb)
		return 1;
	if (oa < ob)
		return -1;
	if (oa > ob)
		return 1;
	return 0;
}


/*
 * acquire_inherited_sample_rows —— 从继承树获取采样行
 *
 * API 与 acquire_sample_rows 相同，只是行是从
 * 所有继承子表和指定表中收集的。
 * 如果没有继承子表，或所有子表都是
 * 不支持 ANALYZE 的外部表，则失败并返回零。
 */
static int
acquire_inherited_sample_rows(Relation onerel, int elevel,
							  HeapTuple *rows, int targrows,
							  double *totalrows, double *totaldeadrows)
{
	List	   *tableOIDs;
	Relation   *rels;
	AcquireSampleRowsFunc *acquirefuncs;
	double	   *relblocks;
	double		totalblocks;
	int			numrows,
				nrels,
				i;
	ListCell   *lc;
	bool		has_child;

	/* 立即将输出参数初始化为零，以应对提前退出的情况 */
	*totalrows = 0;
	*totaldeadrows = 0;

	/*
	 * 查找继承集的所有成员。我们只需要在子表上
	 * 持有 AccessShareLock。
	 */
	tableOIDs =
		find_all_inheritors(RelationGetRelid(onerel), AccessShareLock, NULL);

	/*
	 * 检查是否至少有一个后代，否则失败。这可能
	 * 发生尽管 analyze_rel 检查了 relhassubclass，如果表曾经
	 * 有子表但现在没有了。在这种情况下，我们可以清除
	 * relhassubclass 字段，以免后续再次犯同样的错误。
	 * （这是安全的，因为我们持有 ShareUpdateExclusiveLock。）
	 */
	if (list_length(tableOIDs) < 2)
	{
		/* CCI，因为我们已在此命令中更新了 pg_class 行 */
		CommandCounterIncrement();
		SetRelationHasSubclass(RelationGetRelid(onerel), false);
		ereport(elevel,
				(errmsg("skipping analyze of \"%s.%s\" inheritance tree --- this inheritance tree contains no child tables",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
		return 0;
	}

	/*
	 * 识别要使用的 acquirefuncs，并统计所有关系中的块数。
	 * 结果可能溢出 BlockNumber，因此使用 double 运算。
	 */
	rels = (Relation *) palloc(list_length(tableOIDs) * sizeof(Relation));
	acquirefuncs = (AcquireSampleRowsFunc *)
		palloc(list_length(tableOIDs) * sizeof(AcquireSampleRowsFunc));
	relblocks = (double *) palloc(list_length(tableOIDs) * sizeof(double));
	totalblocks = 0;
	nrels = 0;
	has_child = false;
	foreach(lc, tableOIDs)
	{
		Oid			childOID = lfirst_oid(lc);
		Relation	childrel;
		AcquireSampleRowsFunc acquirefunc = NULL;
		BlockNumber relpages = 0;

		/* 我们已经获得了所需的锁 */
		childrel = table_open(childOID, NoLock);

		/* 如果是其他后端的临时表则忽略 */
		if (RELATION_IS_OTHER_TEMP(childrel))
		{
			/* ... 但释放其上的锁 */
			Assert(childrel != onerel);
			table_close(childrel, AccessShareLock);
			continue;
		}

		/* 检查表类型（MATVIEW 不可能出现，但不妨允许） */
		if (childrel->rd_rel->relkind == RELKIND_RELATION ||
			childrel->rd_rel->relkind == RELKIND_MATVIEW)
		{
			/* 普通表，因此使用普通的行获取函数 */
			acquirefunc = acquire_sample_rows;
			relpages = RelationGetNumberOfBlocks(childrel);
		}
		else if (childrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			/*
			 * 对于外部表，调用 FDW 的钩子函数来查看
			 * 它是否支持分析。
			 */
			FdwRoutine *fdwroutine;
			bool		ok = false;

			fdwroutine = GetFdwRoutineForRelation(childrel, false);

			if (fdwroutine->AnalyzeForeignTable != NULL)
				ok = fdwroutine->AnalyzeForeignTable(childrel,
													 &acquirefunc,
													 &relpages);

			if (!ok)
			{
				/* 忽略，但释放其上的锁 */
				Assert(childrel != onerel);
				table_close(childrel, AccessShareLock);
				continue;
			}
		}
		else
		{
			/*
			 * 忽略，但释放其上的锁。不要尝试解锁
			 * 传入的关系
			 */
			Assert(childrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			if (childrel != onerel)
				table_close(childrel, AccessShareLock);
			else
				table_close(childrel, NoLock);
			continue;
		}

		/* 好的，我们将处理这个子表 */
		has_child = true;
		rels[nrels] = childrel;
		acquirefuncs[nrels] = acquirefunc;
		relblocks[nrels] = (double) relpages;
		totalblocks += (double) relpages;
		nrels++;
	}

	/*
	 * 如果没有至少一个子表可供考虑，则失败。如果
	 * 关系是分区表，则不视为子表。
	 */
	if (!has_child)
	{
		ereport(elevel,
				(errmsg("skipping analyze of \"%s.%s\" inheritance tree --- this inheritance tree contains no analyzable child tables",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
		return 0;
	}

	/*
	 * 现在从每个关系中按其在总块数中的比例采样行。
	 * （如果子表的空闲空间百分比差异很大，这可能
	 * 不太理想，但不清楚是否值得
	 * 更费力地处理。）
	 */
	pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_TOTAL,
								 nrels);
	numrows = 0;
	for (i = 0; i < nrels; i++)
	{
		Relation	childrel = rels[i];
		AcquireSampleRowsFunc acquirefunc = acquirefuncs[i];
		double		childblocks = relblocks[i];

		/*
		 * 报告进度。采样函数通常会在获取到行之前
		 * 报告已完成/总块数，但我们需要在此将其重置为 0，
		 * 以免在此之前显示旧值。
		 */
		{
			const int	progress_index[] = {
				PROGRESS_ANALYZE_CURRENT_CHILD_TABLE_RELID,
				PROGRESS_ANALYZE_BLOCKS_DONE,
				PROGRESS_ANALYZE_BLOCKS_TOTAL
			};
			const int64 progress_vals[] = {
				RelationGetRelid(childrel),
				0,
				0,
			};

			pgstat_progress_update_multi_param(3, progress_index, progress_vals);
		}

		if (childblocks > 0)
		{
			int			childtargrows;

			childtargrows = (int) rint(targrows * childblocks / totalblocks);
			/* 确保不会因舍入误差而超出范围 */
			childtargrows = Min(childtargrows, targrows - numrows);
			if (childtargrows > 0)
			{
				int			childrows;
				double		trows,
							tdrows;

				/* 获取子表行的随机采样 */
				childrows = (*acquirefunc) (childrel, elevel,
											rows + numrows, childtargrows,
											&trows, &tdrows);

				/* 可能需要将子表的行类型转换为父表的行类型 */
				if (childrows > 0 &&
					!equalTupleDescs(RelationGetDescr(childrel),
									 RelationGetDescr(onerel)))
				{
					TupleConversionMap *map;

					map = convert_tuples_by_name(RelationGetDescr(childrel),
												 RelationGetDescr(onerel));
					if (map != NULL)
					{
						int			j;

						for (j = 0; j < childrows; j++)
						{
							HeapTuple	newtup;

							newtup = execute_attr_map_tuple(rows[numrows + j], map);
							heap_freetuple(rows[numrows + j]);
							rows[numrows + j] = newtup;
						}
						free_conversion_map(map);
					}
				}

				/* And add 到 counts */
				numrows += childrows;
				*totalrows += trows;
				*totaldeadrows += tdrows;
			}
		}

		/*
		 * Note: we 不能释放 the child-表 locks, 由于 we 可以 have
		 * pointers 到 their TOAST tables in the sampled rows.
		 */
		table_close(childrel, NoLock);
		pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_DONE,
									 i + 1);
	}

	return numrows;
}


/*
 *	update_attstats() —— 更新一个关系的属性统计信息
 *
 *		Statistics are stored in several places: the pg_class 行对于 the
 *		关系 has 统计信息 about the whole 关系, 和 there is a
 *		pg_statistic 行对于每个 (non-system) attribute 那个 has 曾经
 *		been analyzed.  The pg_class values are updated 通过 VACUUM, 不 here.
 *
 *		pg_statistic rows are 只是 added 或 updated 通常.  This means
 *		那个 pg_statistic 将 probably contain 一些 deleted rows at the
 *		completion of a 清理 cycle, 除非 it happens 到 get vacuumed 最后.
 *
 *		To keep things simple, we punt 对于 pg_statistic, 和 don't try
 *		到计算或 store rows 对于 pg_statistic itself in pg_statistic.
 *		This 可以 possibly be made 到 work, 但 it's 不 worth the trouble.
 *		Note analyze_rel() has seen 到 it 那个 we won't come here 当
 *		vacuuming pg_statistic itself.
 *
 *		Note: there 会 be a race condition here 如果二 backends 可以
 *		ANALYZE the 相同表 concurrently.  Presently, we 锁那个 out
 *		通过 taking a self-ExclusiveLock on the 关系 in analyze_rel().
 */
static void
update_attstats(Oid relid, bool inh, int natts, VacAttrStats **vacattrstats)
{
	Relation	sd;
	int			attno;
	CatalogIndexState indstate = NULL;

	if (natts <= 0)
		return;					/* nothing 到 do */

	sd = table_open(StatisticRelationId, RowExclusiveLock);

	for (attno = 0; attno < natts; attno++)
	{
		VacAttrStats *stats = vacattrstats[attno];
		HeapTuple	stup,
					oldtup;
		int			i,
					k,
					n;
		Datum		values[Natts_pg_statistic];
		bool		nulls[Natts_pg_statistic];
		bool		replaces[Natts_pg_statistic];

		/* Ignore attr 如果 we weren't able 到 collect 统计信息 */
		if (!stats->stats_valid)
			continue;

		/*
		 * Construct a 新 pg_statistic 元组
		 */
		for (i = 0; i < Natts_pg_statistic; ++i)
		{
			nulls[i] = false;
			replaces[i] = true;
		}

		values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(relid);
		values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(stats->attr->attnum);
		values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(inh);
		values[Anum_pg_statistic_stanullfrac - 1] = Float4GetDatum(stats->stanullfrac);
		values[Anum_pg_statistic_stawidth - 1] = Int32GetDatum(stats->stawidth);
		values[Anum_pg_statistic_stadistinct - 1] = Float4GetDatum(stats->stadistinct);
		i = Anum_pg_statistic_stakind1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = Int16GetDatum(stats->stakind[k]); /* stakindN */
		}
		i = Anum_pg_statistic_staop1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->staop[k]);	/* staopN */
		}
		i = Anum_pg_statistic_stacoll1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->stacoll[k]);	/* stacollN */
		}
		i = Anum_pg_statistic_stanumbers1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			int			nnum = stats->numnumbers[k];

			if (nnum > 0)
			{
				Datum	   *numdatums = (Datum *) palloc(nnum * sizeof(Datum));
				ArrayType  *arry;

				for (n = 0; n < nnum; n++)
					numdatums[n] = Float4GetDatum(stats->stanumbers[k][n]);
				arry = construct_array_builtin(numdatums, nnum, FLOAT4OID);
				values[i++] = PointerGetDatum(arry);	/* stanumbersN */
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
				values[i++] = PointerGetDatum(arry);	/* stavaluesN */
			}
			else
			{
				nulls[i] = true;
				values[i++] = (Datum) 0;
			}
		}

		/* Is there 已经 a pg_statistic 元组对于此 attribute? */
		oldtup = SearchSysCache3(STATRELATTINH,
								 ObjectIdGetDatum(relid),
								 Int16GetDatum(stats->attr->attnum),
								 BoolGetDatum(inh));

		/* Open 索引 information 当 we know we 需要 it */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(sd);

		if (HeapTupleIsValid(oldtup))
		{
			/* Yes, replace it */
			stup = heap_modify_tuple(oldtup,
									 RelationGetDescr(sd),
									 values,
									 nulls,
									 replaces);
			ReleaseSysCache(oldtup);
			CatalogTupleUpdateWithInfo(sd, &stup->t_self, stup, indstate);
		}
		else
		{
			/* No, 插入新元组 */
			stup = heap_form_tuple(RelationGetDescr(sd), values, nulls);
			CatalogTupleInsertWithInfo(sd, stup, indstate);
		}

		heap_freetuple(stup);
	}

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);
	table_close(sd, RowExclusiveLock);
}

/*
 * Standard fetch 函数对于 use 通过计算统计信息 subroutines.
 *
 * This exists 到 provide 一些 insulation 之间计算统计信息 routines
 * 和 the actual 存储 of the 采样数据.
 */
static Datum
std_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			attnum = stats->tupattnum;
	HeapTuple	tuple = stats->rows[rownum];
	TupleDesc	tupDesc = stats->tupDesc;

	return heap_getattr(tuple, attnum, tupDesc, isNull);
}

/*
 * Fetch 函数对于 analyzing 索引表达式s.
 *
 * We have 不 bothered 到 construct 索引 tuples, instead the 数据 is
 * 只是 in Datum arrays.
 */
static Datum
ind_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			i;

	/* exprvals 和 exprnulls are 已经 offset 对于 proper 列 */
	i = rownum * stats->rowstride;
	*isNull = stats->exprnulls[i];
	return stats->exprvals[i];
}


/*==========================================================================
 *
 * Code 以下此 point represents the "standard" 类型-specific 统计信息
 * 分析 algorithms.  This code 可以 be replaced on a per-数据-类型 basis
 * 通过设置 a nonzero 值 in pg_type.typanalyze.
 *
 *==========================================================================
 */


/*
 * To avoid consuming too much 内存期间分析和/或 too much space
 * in the resulting pg_statistic rows, we 忽略 varlena datums 那个 are wider
 * than WIDTH_THRESHOLD (之后 detoasting!).  This is legitimate 对于 MCV
 * 和 distinct-值 calculations 由于 a 宽值 is unlikely 到 be
 * duplicated at 所有, much 更少 be a 最多-common 值.  For the 相同 reason,
 * ignoring 宽 values 将不 affect our estimates of 直方图 bin
 * boundaries 非常 much.
 */
#define WIDTH_THRESHOLD  1024

#define swapInt(a,b)	do {int _tmp; _tmp=a; a=b; b=_tmp;} while(0)
#define swapDatum(a,b)	do {Datum _tmp; _tmp=a; a=b; b=_tmp;} while(0)

/*
 * Extra information used 通过 the default 分析 routines
 */
typedef struct
{
	int			count;			/* # of duplicates */
	int			first;			/* values[] 索引 of 首先 occurrence */
} ScalarMCVItem;

typedef struct
{
	SortSupport ssup;
	int		   *tupnoLink;
} CompareScalarsContext;


static void compute_trivial_stats(VacAttrStatsP stats,
								  AnalyzeAttrFetchFunc fetchfunc,
								  int samplerows,
								  double totalrows);
static void compute_distinct_stats(VacAttrStatsP stats,
								   AnalyzeAttrFetchFunc fetchfunc,
								   int samplerows,
								   double totalrows);
static void compute_scalar_stats(VacAttrStatsP stats,
								 AnalyzeAttrFetchFunc fetchfunc,
								 int samplerows,
								 double totalrows);
static int	compare_scalars(const void *a, const void *b, void *arg);
static int	compare_mcvs(const void *a, const void *b, void *arg);
static int	analyze_mcv_list(int *mcv_counts,
							 int num_mcv,
							 double stadistinct,
							 double stanullfrac,
							 int samplerows,
							 double totalrows);


/*
 * std_typanalyze -- the default 类型-specific typanalyze 函数
 */
bool
std_typanalyze(VacAttrStats *stats)
{
	Form_pg_attribute attr = stats->attr;
	Oid			ltopr;
	Oid			eqopr;
	StdAnalyzeData *mystats;

	/* If the attstattarget 列 is negative, use the default 值 */
	/* NB: it is okay 到 scribble on 统计信息->attr 由于 it's a copy */
	if (attr->attstattarget < 0)
		attr->attstattarget = default_statistics_target;

	/* Look 对于 default "<" 和 "=" operators 对于列's 类型 */
	get_sort_group_operators(stats->attrtypid,
							 false, false, false,
							 &ltopr, &eqopr, NULL,
							 NULL);

	/* 保存操作符信息，供统计信息计算例程使用 */
	mystats = (StdAnalyzeData *) palloc(sizeof(StdAnalyzeData));
	mystats->eqopr = eqopr;
	mystats->eqfunc = OidIsValid(eqopr) ? get_opcode(eqopr) : InvalidOid;
	mystats->ltopr = ltopr;
	stats->extra_data = mystats;

	/*
	 * Determine which standard 统计信息 algorithm 到 use
	 */
	if (OidIsValid(eqopr) && OidIsValid(ltopr))
	{
		/* Seems 到 be a scalar datatype */
		stats->compute_stats = compute_scalar_stats;
		/*--------------------
		 * The following choice of minrows is 基于 the paper
		 * "Random 采样对于直方图 construction: how much is enough?"
		 * 通过 Surajit Chaudhuri, Rajeev Motwani 和 Vivek Narasayya, in
		 * Proceedings of ACM SIGMOD International Conference on Management
		 * of Data, 1998, Pages 436-447.  Their Corollary 1 到 Theorem 5
		 * says 那个对于表 size n, 直方图 size k, maximum relative
		 * 错误 in bin size f, 和错误 probability gamma, the minimum
		 * 随机采样 size is
		 *		r = 4 * k * ln(2*n/gamma) / f^2
		 * Taking f = 0.5, gamma = 0.01, n = 10^6 rows, we obtain
		 *		r = 305.82 * k
		 * Note 那个因为 the 日志函数, the dependence on n is
		 * 相当 weak; even at n = 10^12, a 300*k 采样 gives <= 0.66
		 * bin size 错误带有 probability 0.99.  So there's no real 需要到
		 * scale 对于 n, which is a 好 thing 因为 we don't necessarily
		 * know it at 此 point.
		 *--------------------
		 */
		stats->minrows = 300 * attr->attstattarget;
	}
	else if (OidIsValid(eqopr))
	{
		/* We 可以仍然 recognize distinct values */
		stats->compute_stats = compute_distinct_stats;
		/* Might 作为 well use the 相同 minrows 作为以上 */
		stats->minrows = 300 * attr->attstattarget;
	}
	else
	{
		/* Can't do much 但 the trivial stuff */
		stats->compute_stats = compute_trivial_stats;
		/* Might 作为 well use the 相同 minrows 作为以上 */
		stats->minrows = 300 * attr->attstattarget;
	}

	return true;
}


/*
 *	compute_trivial_stats() -- 计算非常 basic 列统计信息
 *
 *	We use 此当 we 不能 find a 哈希 "=" operator 对于 the datatype.
 *
 *	We determine the fraction of non-NULL rows 和 the average datum width.
 */
static void
compute_trivial_stats(VacAttrStatsP stats,
					  AnalyzeAttrFetchFunc fetchfunc,
					  int samplerows,
					  double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);

	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;

		vacuum_delay_point();

		value = fetchfunc(stats, i, &isnull);

		/* Check 对于 NULL/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a 变量-width 字段, add up widths 对于 average width
		 * calculation.  Note 那个如果 the 值 is toasted, we use the toasted
		 * width.  We don't bother 带有此 calculation 如果 it's a fixed-width
		 * 类型.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));
		}
		else if (is_varwidth)
		{
			/* 必须 be cstring */
			total_width += strlen(DatumGetCString(value)) + 1;
		}
	}

	/* We 可以仅计算 average width 如果 we found 一些 non-NULL values. */
	if (nonnull_cnt > 0)
	{
		stats->stats_valid = true;
		/* Do the simple NULL-frac 和 width 统计信息 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}
	else if (null_cnt > 0)
	{
		/* We found 仅 nulls; assume the 列 is 完全 NULL */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}
}


/*
 *	compute_distinct_stats() -- 计算列统计信息 including ndistinct
 *
 *	We use 此当 we 可以 find 仅 an "=" operator 对于 the datatype.
 *
 *	We determine the fraction of non-NULL rows, the average width, the
 *	最多 common values, 和 the (estimated) 数字 of distinct values.
 *
 *	The 最多 common values are determined 通过 brute force: we keep a 列表
 *	of previously seen values, ordered 通过数字 of times seen, 作为 we 扫描
 *	the 采样.  A newly seen 值 is inserted 只是之后 the 最后
 *	multiply-seen 值, causing the bottommost (oldest) singly-seen 值
 *	到删除 off the 列表.  The accuracy of 此 method, 和还 its 成本,
 *	depend mainly on the length of the 列表 we are willing 到 keep.
 */
static void
compute_distinct_stats(VacAttrStatsP stats,
					   AnalyzeAttrFetchFunc fetchfunc,
					   int samplerows,
					   double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);
	FmgrInfo	f_cmpeq;
	typedef struct
	{
		Datum		value;
		int			count;
	} TrackItem;
	TrackItem  *track;
	int			track_cnt,
				track_max;
	int			num_mcv = stats->attr->attstattarget;
	StdAnalyzeData *mystats = (StdAnalyzeData *) stats->extra_data;

	/*
	 * We track up 到 2*n values 对于 an n-element MCV 列表; 但至少 10
	 */
	track_max = 2 * num_mcv;
	if (track_max < 10)
		track_max = 10;
	track = (TrackItem *) palloc(track_max * sizeof(TrackItem));
	track_cnt = 0;

	fmgr_info(mystats->eqfunc, &f_cmpeq);

	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;
		bool		match;
		int			firstcount1,
					j;

		vacuum_delay_point();

		value = fetchfunc(stats, i, &isnull);

		/* Check 对于 NULL/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a 变量-width 字段, add up widths 对于 average width
		 * calculation.  Note 那个如果 the 值 is toasted, we use the toasted
		 * width.  We don't bother 带有此 calculation 如果 it's a fixed-width
		 * 类型.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * If the 值 is toasted, we want 到 detoast it 只是一次到
			 * avoid repeated detoastings 和 resultant excess 内存 usage
			 * 期间 the comparisons.  Also, 检查到 see 如果 the 值 is
			 * excessively 宽, 和如果 so don't detoast at 所有 --- 只是
			 * 忽略 the 值.
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}
		else if (is_varwidth)
		{
			/* 必须 be cstring */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/*
		 * See 如果 the 值 matches anything we're 已经 tracking.
		 */
		match = false;
		firstcount1 = track_cnt;
		for (j = 0; j < track_cnt; j++)
		{
			if (DatumGetBool(FunctionCall2Coll(&f_cmpeq,
											   stats->attrcollid,
											   value, track[j].value)))
			{
				match = true;
				break;
			}
			if (j < firstcount1 && track[j].count == 1)
				firstcount1 = j;
		}

		if (match)
		{
			/* Found a match */
			track[j].count++;
			/* This 值可以现在需要到 "bubble up" in the track 列表 */
			while (j > 0 && track[j].count > track[j - 1].count)
			{
				swapDatum(track[j].value, track[j - 1].value);
				swapInt(track[j].count, track[j - 1].count);
				j--;
			}
		}
		else
		{
			/* No match.  Insert at head of count-1 列表 */
			if (track_cnt < track_max)
				track_cnt++;
			for (j = track_cnt - 1; j > firstcount1; j--)
			{
				track[j].value = track[j - 1].value;
				track[j].count = track[j - 1].count;
			}
			if (firstcount1 < track_cnt)
			{
				track[firstcount1].value = value;
				track[firstcount1].count = 1;
			}
		}
	}

	/* We 可以仅计算 real 统计信息如果 we found 一些 non-NULL values. */
	if (nonnull_cnt > 0)
	{
		int			nmultiple,
					summultiple;

		stats->stats_valid = true;
		/* Do the simple NULL-frac 和 width 统计信息 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		/* Count the 数字 of values we found 多个 times */
		summultiple = 0;
		for (nmultiple = 0; nmultiple < track_cnt; nmultiple++)
		{
			if (track[nmultiple].count == 1)
				break;
			summultiple += track[nmultiple].count;
		}

		if (nmultiple == 0)
		{
			/*
			 * If we found no repeated non-NULL values, assume it's a unique
			 * 列; 但 be sure 到 discount 对于任何 nulls we found.
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (track_cnt < track_max && toowide_cnt == 0 &&
				 nmultiple == track_cnt)
		{
			/*
			 * Our track 列表 includes 每个值 in the 采样, 和每个
			 * 值 appeared 更多 than 一次.  Assume the 列 has 只是
			 * 这些 values.  (This case is meant 到 address columns 带有
			 * 小, fixed sets of possible values, 如布尔或 enum
			 * columns.  If there are 任何 values 那个 appear 只是一次 in the
			 * 采样, including too-宽 values, we 应该 assume 那个那个's
			 * 不 what we're dealing 带有.)
			 */
			stats->stadistinct = track_cnt;
		}
		else
		{
			/*----------
			 * Estimate the 数字 of distinct values using the estimator
			 * proposed 通过 Haas 和 Stokes in IBM Research Report RJ 10025:
			 *		n*d / (n - f1 + f1*n/N)
			 * where f1 is the 数字 of distinct values 那个 occurred
			 * 恰好一次 in our 采样 of n rows (从 a total of N),
			 * 和 d is the total 数字 of distinct values in the 采样.
			 * This is their Duj1 estimator; the other estimators they
			 * recommend are considerably 更复杂, 和 are numerically
			 * 非常 unstable 当 n is much smaller than N.
			 *
			 * In 此 calculation, we consider 仅 non-nulls.  We used 到
			 * include rows 带有 NULL values in the n 和 N counts, 但那个
			 * leads 到 inaccurate answers in columns 带有许多 nulls, 和
			 * it's intuitively bogus anyway considering the desired result is
			 * the 数字 of distinct non-NULL values.
			 *
			 * We assume (不非常 reliably!) 那个所有 the multiply-occurring
			 * values are reflected in the final track[] 列表, 和 the other
			 * nonnull values 所有 appeared 但一次.  (XXX 此通常
			 * results in a drastic overestimate of ndistinct.  Can we do
			 * 任何更好?)
			 *----------
			 */
			int			f1 = nonnull_cnt - summultiple;
			int			d = f1 + nmultiple;
			double		n = samplerows - null_cnt;
			double		N = totalrows * (1.0 - stats->stanullfrac);
			double		stadistinct;

			/* N == 0 不应发生, 但以防 ... */
			if (N > 0)
				stadistinct = (n * d) / ((n - f1) + f1 * n / N);
			else
				stadistinct = 0;

			/* Clamp 到 sane range 以防 of 舍入误差 */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* And round 到整数 */
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * If we estimated the 数字 of distinct values at 更多 than 10% of
		 * the total 行 count (a 非常 arbitrary limit), 然后 assume 那个
		 * stadistinct 应该 scale 带有 the 行 count 而非 be a fixed
		 * 值.
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * Decide how 许多 values are worth storing 作为最多-common values. If
		 * we are able 到 generate a complete MCV 列表 (所有 the values in the
		 * 采样将 fit, 和 we think 这些 are 所有 the ones in the 表),
		 * 然后 do so.  Otherwise, store 仅那些 values 那个 are
		 * significantly 更多 common than the values 不 in the 列表.
		 *
		 * Note: the 首先 of 这些 cases is meant 到 address columns 带有
		 * 小, fixed sets of possible values, 如布尔或 enum
		 * columns.  If we 可以 *完全* represent the 列 population 通过
		 * an MCV 列表那个将 fit 进入 the 统计信息 target, 然后 we 应该 do
		 * so 和 thus provide the 规划器带有 complete information.  But 如果
		 * the MCV 列表 is 不 complete, it's generally worth being 更多
		 * selective, 和不只是 filling it 所有 the way up 到 the 统计信息
		 * target.
		 */
		if (track_cnt < track_max && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* Track 列表 includes 所有 values seen, 和所有将 fit */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* Incomplete 列表; decide how 许多 values are worth keeping */
			if (num_mcv > track_cnt)
				num_mcv = track_cnt;

			if (num_mcv > 0)
			{
				mcv_counts = (int *) palloc(num_mcv * sizeof(int));
				for (i = 0; i < num_mcv; i++)
					mcv_counts[i] = track[i].count;

				num_mcv = analyze_mcv_list(mcv_counts, num_mcv,
										   stats->stadistinct,
										   stats->stanullfrac,
										   samplerows, totalrows);
			}
		}

		/* Generate MCV slot 条目 */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* Must copy the target values 进入 anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(track[i].value,
										  stats->attrtype->typbyval,
										  stats->attrtype->typlen);
				mcv_freqs[i] = (double) track[i].count / (double) samplerows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[0] = STATISTIC_KIND_MCV;
			stats->staop[0] = mystats->eqopr;
			stats->stacoll[0] = stats->attrcollid;
			stats->stanumbers[0] = mcv_freqs;
			stats->numnumbers[0] = num_mcv;
			stats->stavalues[0] = mcv_values;
			stats->numvalues[0] = num_mcv;

			/*
			 * Accept the defaults 对于统计信息->statypid 和 others. They have
			 * been 集合之前 we were called (see 清理.h)
			 */
		}
	}
	else if (null_cnt > 0)
	{
		/* We found 仅 nulls; assume the 列 is 完全 NULL */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/* We don't 需要到 bother cleaning up 任何 of our temporary palloc's */
}


/*
 *	compute_scalar_stats() -- 计算列统计信息
 *
 *	We use 此当 we 可以 find "=" 和 "<" operators 对于 the datatype.
 *
 *	We determine the fraction of non-NULL rows, the average width, the
 *	最多 common values, the (estimated) 数字 of distinct values, the
 *	distribution 直方图, 和 the 相关度 of physical 到 logical order.
 *
 *	The desired 统计信息可以 be determined fairly easily 之后 sorting the
 *	数据 values 进入 order.
 */
static void
compute_scalar_stats(VacAttrStatsP stats,
					 AnalyzeAttrFetchFunc fetchfunc,
					 int samplerows,
					 double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);
	double		corr_xysum;
	SortSupportData ssup;
	ScalarItem *values;
	int			values_cnt = 0;
	int		   *tupnoLink;
	ScalarMCVItem *track;
	int			track_cnt = 0;
	int			num_mcv = stats->attr->attstattarget;
	int			num_bins = stats->attr->attstattarget;
	StdAnalyzeData *mystats = (StdAnalyzeData *) stats->extra_data;

	values = (ScalarItem *) palloc(samplerows * sizeof(ScalarItem));
	tupnoLink = (int *) palloc(samplerows * sizeof(int));
	track = (ScalarMCVItem *) palloc(num_mcv * sizeof(ScalarMCVItem));

	memset(&ssup, 0, sizeof(ssup));
	ssup.ssup_cxt = CurrentMemoryContext;
	ssup.ssup_collation = stats->attrcollid;
	ssup.ssup_nulls_first = false;

	/*
	 * For 现在, don't perform abbreviated key conversion, 因为 full values
	 * are required 对于 MCV slot generation.  Supporting 那个 optimization
	 * 会 necessitate teaching compare_scalars() 到 call a tie-breaker.
	 */
	ssup.abbreviate = false;

	PrepareSortSupportFromOrderingOp(mystats->ltopr, &ssup);

	/* Initial 扫描到 find sortable values */
	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;

		vacuum_delay_point();

		value = fetchfunc(stats, i, &isnull);

		/* Check 对于 NULL/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a 变量-width 字段, add up widths 对于 average width
		 * calculation.  Note 那个如果 the 值 is toasted, we use the toasted
		 * width.  We don't bother 带有此 calculation 如果 it's a fixed-width
		 * 类型.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * If the 值 is toasted, we want 到 detoast it 只是一次到
			 * avoid repeated detoastings 和 resultant excess 内存 usage
			 * 期间 the comparisons.  Also, 检查到 see 如果 the 值 is
			 * excessively 宽, 和如果 so don't detoast at 所有 --- 只是
			 * 忽略 the 值.
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}
		else if (is_varwidth)
		{
			/* 必须 be cstring */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/* Add it 到 the 列表到 be sorted */
		values[values_cnt].value = value;
		values[values_cnt].tupno = values_cnt;
		tupnoLink[values_cnt] = values_cnt;
		values_cnt++;
	}

	/* We 可以仅计算 real 统计信息如果 we found 一些 sortable values. */
	if (values_cnt > 0)
	{
		int			ndistinct,	/* # distinct values in 采样 */
					nmultiple,	/* # 那个 appear 多个 times */
					num_hist,
					dups_cnt;
		int			slot_idx = 0;
		CompareScalarsContext cxt;

		/* 排序收集到的值 */
		cxt.ssup = &ssup;
		cxt.tupnoLink = tupnoLink;
		qsort_interruptible(values, values_cnt, sizeof(ScalarItem),
							compare_scalars, &cxt);

		/*
		 * Now 扫描 the values in order, find the 最多 common ones, 和还
		 * accumulate ordering-相关度统计信息.
		 *
		 * To determine which are 最多 common, we 首先 have 到 count the
		 * 数字 of duplicates of 每个值.  The duplicates are adjacent in
		 * the sorted 列表, so a brute-force approach is 到 compare successive
		 * datum values 直到 we find 二那个 are 不 equal. However, 那个
		 * requires N-1 invocations of the datum comparison routine, which are
		 * 完全 redundant 带有 work 那个 was done 期间 the 排序.  (The
		 * 排序 algorithm 必须 at 一些 point have compared 每个 pair of items
		 * 那个 are adjacent in the sorted order; otherwise it 可以不 know
		 * 那个 it's ordered the pair correctly.) We exploit 此通过 having
		 * compare_scalars remember the highest tupno 索引那个每个
		 * ScalarItem has been found equal 到.  At the 结束 of the 排序, a
		 * ScalarItem's tupnoLink 将仍然 point 到 itself 如果和仅如果 it
		 * is the 最后 item of its group of duplicates (由于 the group 将
		 * be ordered 通过 tupno).
		 */
		corr_xysum = 0;
		ndistinct = 0;
		nmultiple = 0;
		dups_cnt = 0;
		for (i = 0; i < values_cnt; i++)
		{
			int			tupno = values[i].tupno;

			corr_xysum += ((double) i) * ((double) tupno);
			dups_cnt++;
			if (tupnoLink[tupno] == tupno)
			{
				/* Reached 结束 of duplicates of 此值 */
				ndistinct++;
				if (dups_cnt > 1)
				{
					nmultiple++;
					if (track_cnt < num_mcv ||
						dups_cnt > track[track_cnt - 1].count)
					{
						/*
						 * Found a 新 item 对于 the mcv 列表; find its
						 * position, bubbling down 旧 items 如果 needed. Loop
						 * invariant is 那个 j points at an empty/ replaceable
						 * slot.
						 */
						int			j;

						if (track_cnt < num_mcv)
							track_cnt++;
						for (j = track_cnt - 1; j > 0; j--)
						{
							if (dups_cnt <= track[j - 1].count)
								break;
							track[j].count = track[j - 1].count;
							track[j].first = track[j - 1].first;
						}
						track[j].count = dups_cnt;
						track[j].first = i + 1 - dups_cnt;
					}
				}
				dups_cnt = 0;
			}
		}

		stats->stats_valid = true;
		/* Do the simple NULL-frac 和 width 统计信息 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		if (nmultiple == 0)
		{
			/*
			 * If we found no repeated non-NULL values, assume it's a unique
			 * 列; 但 be sure 到 discount 对于任何 nulls we found.
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (toowide_cnt == 0 && nmultiple == ndistinct)
		{
			/*
			 * Every 值 in the 采样 appeared 更多 than 一次.  Assume the
			 * 列 has 只是这些 values.  (This case is meant 到 address
			 * columns 带有小, fixed sets of possible values, 如
			 * 布尔或 enum columns.  If there are 任何 values 那个 appear
			 * 只是一次 in the 采样, including too-宽 values, we 应该
			 * assume 那个那个's 不 what we're dealing 带有.)
			 */
			stats->stadistinct = ndistinct;
		}
		else
		{
			/*----------
			 * Estimate the 数字 of distinct values using the estimator
			 * proposed 通过 Haas 和 Stokes in IBM Research Report RJ 10025:
			 *		n*d / (n - f1 + f1*n/N)
			 * where f1 is the 数字 of distinct values 那个 occurred
			 * 恰好一次 in our 采样 of n rows (从 a total of N),
			 * 和 d is the total 数字 of distinct values in the 采样.
			 * This is their Duj1 estimator; the other estimators they
			 * recommend are considerably 更复杂, 和 are numerically
			 * 非常 unstable 当 n is much smaller than N.
			 *
			 * In 此 calculation, we consider 仅 non-nulls.  We used 到
			 * include rows 带有 NULL values in the n 和 N counts, 但那个
			 * leads 到 inaccurate answers in columns 带有许多 nulls, 和
			 * it's intuitively bogus anyway considering the desired result is
			 * the 数字 of distinct non-NULL values.
			 *
			 * Overwidth values are assumed 到 have been distinct.
			 *----------
			 */
			int			f1 = ndistinct - nmultiple + toowide_cnt;
			int			d = f1 + nmultiple;
			double		n = samplerows - null_cnt;
			double		N = totalrows * (1.0 - stats->stanullfrac);
			double		stadistinct;

			/* N == 0 不应发生, 但以防 ... */
			if (N > 0)
				stadistinct = (n * d) / ((n - f1) + f1 * n / N);
			else
				stadistinct = 0;

			/* Clamp 到 sane range 以防 of 舍入误差 */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* And round 到整数 */
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * If we estimated the 数字 of distinct values at 更多 than 10% of
		 * the total 行 count (a 非常 arbitrary limit), 然后 assume 那个
		 * stadistinct 应该 scale 带有 the 行 count 而非 be a fixed
		 * 值.
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * Decide how 许多 values are worth storing 作为最多-common values. If
		 * we are able 到 generate a complete MCV 列表 (所有 the values in the
		 * 采样将 fit, 和 we think 这些 are 所有 the ones in the 表),
		 * 然后 do so.  Otherwise, store 仅那些 values 那个 are
		 * significantly 更多 common than the values 不 in the 列表.
		 *
		 * Note: the 首先 of 这些 cases is meant 到 address columns 带有
		 * 小, fixed sets of possible values, 如布尔或 enum
		 * columns.  If we 可以 *完全* represent the 列 population 通过
		 * an MCV 列表那个将 fit 进入 the 统计信息 target, 然后 we 应该 do
		 * so 和 thus provide the 规划器带有 complete information.  But 如果
		 * the MCV 列表 is 不 complete, it's generally worth being 更多
		 * selective, 和不只是 filling it 所有 the way up 到 the 统计信息
		 * target.
		 */
		if (track_cnt == ndistinct && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* Track 列表 includes 所有 values seen, 和所有将 fit */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* Incomplete 列表; decide how 许多 values are worth keeping */
			if (num_mcv > track_cnt)
				num_mcv = track_cnt;

			if (num_mcv > 0)
			{
				mcv_counts = (int *) palloc(num_mcv * sizeof(int));
				for (i = 0; i < num_mcv; i++)
					mcv_counts[i] = track[i].count;

				num_mcv = analyze_mcv_list(mcv_counts, num_mcv,
										   stats->stadistinct,
										   stats->stanullfrac,
										   samplerows, totalrows);
			}
		}

		/* Generate MCV slot 条目 */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* Must copy the target values 进入 anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(values[track[i].first].value,
										  stats->attrtype->typbyval,
										  stats->attrtype->typlen);
				mcv_freqs[i] = (double) track[i].count / (double) samplerows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_MCV;
			stats->staop[slot_idx] = mystats->eqopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stanumbers[slot_idx] = mcv_freqs;
			stats->numnumbers[slot_idx] = num_mcv;
			stats->stavalues[slot_idx] = mcv_values;
			stats->numvalues[slot_idx] = num_mcv;

			/*
			 * Accept the defaults 对于统计信息->statypid 和 others. They have
			 * been 集合之前 we were called (see 清理.h)
			 */
			slot_idx++;
		}

		/*
		 * Generate a 直方图 slot 条目如果 there are 至少二 distinct
		 * values 不 accounted 对于 in the MCV 列表.  (This ensures the
		 * 直方图 won't collapse 到 empty 或 a singleton.)
		 */
		num_hist = ndistinct - num_mcv;
		if (num_hist > num_bins)
			num_hist = num_bins + 1;
		if (num_hist >= 2)
		{
			MemoryContext old_context;
			Datum	   *hist_values;
			int			nvals;
			int			pos,
						posfrac,
						delta,
						deltafrac;

			/* Sort the MCV items 进入 position order 到 speed 下一个循环 */
			qsort_interruptible(track, num_mcv, sizeof(ScalarMCVItem),
								compare_mcvs, NULL);

			/*
			 * Collapse out the MCV items 从 the values[] 数组.
			 *
			 * Note we destroy the values[] 数组 here... 但 we don't 需要 it
			 * 对于 anything 更多.  We do, 但是, 仍然需要 values_cnt.
			 * nvals 将 be the 数字 of remaining entries in values[].
			 */
			if (num_mcv > 0)
			{
				int			src,
							dest;
				int			j;

				src = dest = 0;
				j = 0;			/* 索引 of 下一个 interesting MCV item */
				while (src < values_cnt)
				{
					int			ncopy;

					if (j < num_mcv)
					{
						int			first = track[j].first;

						if (src >= first)
						{
							/* advance past 此 MCV item */
							src = first + track[j].count;
							j++;
							continue;
						}
						ncopy = first - src;
					}
					else
						ncopy = values_cnt - src;
					memmove(&values[dest], &values[src],
							ncopy * sizeof(ScalarItem));
					src += ncopy;
					dest += ncopy;
				}
				nvals = dest;
			}
			else
				nvals = values_cnt;
			Assert(nvals >= num_hist);

			/* Must copy the target values 进入 anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * The object of 此循环 is 到 copy the 首先和最后 values[]
			 * entries along 带有 evenly-spaced values in 之间.  So the
			 * i'th 值 is values[(i * (nvals - 1)) / (num_hist - 1)].  But
			 * computing 那个 subscript directly risks 整数 overflow 当
			 * the 统计信息 target is 更多 than a couple thousand.  Instead we
			 * add (nvals - 1) / (num_hist - 1) 到 pos at 每个 step, tracking
			 * the integral 和 fractional parts of the sum separately.
			 */
			delta = (nvals - 1) / (num_hist - 1);
			deltafrac = (nvals - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				hist_values[i] = datumCopy(values[pos].value,
										   stats->attrtype->typbyval,
										   stats->attrtype->typlen);
				pos += delta;
				posfrac += deltafrac;
				if (posfrac >= (num_hist - 1))
				{
					/* fractional part exceeds 1, carry 到整数 part */
					pos++;
					posfrac -= (num_hist - 1);
				}
			}

			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_HISTOGRAM;
			stats->staop[slot_idx] = mystats->ltopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stavalues[slot_idx] = hist_values;
			stats->numvalues[slot_idx] = num_hist;

			/*
			 * Accept the defaults 对于统计信息->statypid 和 others. They have
			 * been 集合之前 we were called (see 清理.h)
			 */
			slot_idx++;
		}

		/* Generate a 相关度条目如果 there are 多个 values */
		if (values_cnt > 1)
		{
			MemoryContext old_context;
			float4	   *corrs;
			double		corr_xsum,
						corr_x2sum;

			/* Must copy the target values 进入 anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			corrs = (float4 *) palloc(sizeof(float4));
			MemoryContextSwitchTo(old_context);

			/*----------
			 * Since we know the x 和 y 值 sets are both
			 *		0, 1, ..., values_cnt-1
			 * we have sum(x) = sum(y) =
			 *		(values_cnt-1)*values_cnt / 2
			 * 和 sum(x^2) = sum(y^2) =
			 *		(values_cnt-1)*values_cnt*(2*values_cnt-1) / 6.
			 *----------
			 */
			corr_xsum = ((double) (values_cnt - 1)) *
				((double) values_cnt) / 2.0;
			corr_x2sum = ((double) (values_cnt - 1)) *
				((double) values_cnt) * (double) (2 * values_cnt - 1) / 6.0;

			/* And the 相关度 coefficient reduces 到 */
			corrs[0] = (values_cnt * corr_xysum - corr_xsum * corr_xsum) /
				(values_cnt * corr_x2sum - corr_xsum * corr_xsum);

			stats->stakind[slot_idx] = STATISTIC_KIND_CORRELATION;
			stats->staop[slot_idx] = mystats->ltopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stanumbers[slot_idx] = corrs;
			stats->numnumbers[slot_idx] = 1;
			slot_idx++;
		}
	}
	else if (nonnull_cnt > 0)
	{
		/* We found 一些 non-NULL values, 但 they were 所有 too 宽 */
		Assert(nonnull_cnt == toowide_cnt);
		stats->stats_valid = true;
		/* Do the simple NULL-frac 和 width 统计信息 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		/* Assume 所有 too-宽 values are distinct, so it's a unique 列 */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
	}
	else if (null_cnt > 0)
	{
		/* We found 仅 nulls; assume the 列 is 完全 NULL */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/* We don't 需要到 bother cleaning up 任何 of our temporary palloc's */
}

/*
 * Comparator 对于 sorting ScalarItems
 *
 * Aside 从 sorting the items, we 更新 the tupnoLink[] 数组
 * whenever 二 ScalarItems are found 到 contain equal datums.  The 数组
 * is indexed 通过 tupno; 对于每个 ScalarItem, it contains the highest
 * tupno 那个那个 item's datum has been found 到 be equal 到.  This allows
 * us 到 avoid additional comparisons in compute_scalar_stats().
 */
static int
compare_scalars(const void *a, const void *b, void *arg)
{
	Datum		da = ((const ScalarItem *) a)->value;
	int			ta = ((const ScalarItem *) a)->tupno;
	Datum		db = ((const ScalarItem *) b)->value;
	int			tb = ((const ScalarItem *) b)->tupno;
	CompareScalarsContext *cxt = (CompareScalarsContext *) arg;
	int			compare;

	compare = ApplySortComparator(da, false, db, false, cxt->ssup);
	if (compare != 0)
		return compare;

	/*
	 * The 二 datums are equal, so 更新 cxt->tupnoLink[].
	 */
	if (cxt->tupnoLink[ta] < tb)
		cxt->tupnoLink[ta] = tb;
	if (cxt->tupnoLink[tb] < ta)
		cxt->tupnoLink[tb] = ta;

	/*
	 * For equal datums, 排序通过 tupno
	 */
	return ta - tb;
}

/*
 * Comparator 对于 sorting ScalarMCVItems 通过 position
 */
static int
compare_mcvs(const void *a, const void *b, void *arg)
{
	int			da = ((const ScalarMCVItem *) a)->first;
	int			db = ((const ScalarMCVItem *) b)->first;

	return da - db;
}

/*
 * Analyze the 列表 of common values in the 采样和 decide how 许多 are
 * worth storing in the 表's MCV 列表.
 *
 * mcv_counts is assumed 到 be a 列表 of the counts of the 最多 common values
 * seen in the 采样, starting 带有 the 最多 common.  The 返回值 is the
 * 数字那个 are significantly 更多 common than the values 不 in the 列表,
 * 和 which are therefore deemed worth storing in the 表's MCV 列表.
 */
static int
analyze_mcv_list(int *mcv_counts,
				 int num_mcv,
				 double stadistinct,
				 double stanullfrac,
				 int samplerows,
				 double totalrows)
{
	double		ndistinct_table;
	double		sumcount;
	int			i;

	/*
	 * If the entire 表 was sampled, keep the whole 列表.  This 还
	 * protects us against division 通过 zero in the code 以下.
	 */
	if (samplerows == totalrows || totalrows <= 1.0)
		return num_mcv;

	/* Re-extract the estimated 数字 of distinct nonnull values in 表 */
	ndistinct_table = stadistinct;
	if (ndistinct_table < 0)
		ndistinct_table = -ndistinct_table * totalrows;

	/*
	 * Exclude the least common values 从 the MCV 列表, 如果 they are 不
	 * significantly 更多 common than the estimated selectivity they 会
	 * have 如果 they weren't in the 列表.  All non-MCV values are assumed 到 be
	 * equally common, 之后 taking 进入 account the frequencies of 所有 the
	 * values in the MCV 列表和 the 数字 of nulls (c.f. eqsel()).
	 *
	 * Here sumcount tracks the total count of 所有但 the 最后 (least common)
	 * 值 in the MCV 列表, allowing us 到 determine the effect of excluding
	 * 那个值从 the 列表.
	 *
	 * Note 那个 we deliberately do 此通过 removing values 从 the full
	 * 列表, 而非 starting 带有 an empty 列表和 adding values,
	 * 因为 the latter approach 可以 fail 到 add 任何 values 如果所有 the 最多
	 * common values have around the 相同 frequency 和 make up the majority
	 * of the 表, 以便 the overall average frequency of 所有 values is
	 * 大致 the 相同作为那个 of the common values.  This 会 lead 到任何
	 * uncommon values being significantly overestimated.
	 */
	sumcount = 0.0;
	for (i = 0; i < num_mcv - 1; i++)
		sumcount += mcv_counts[i];

	while (num_mcv > 0)
	{
		double		selec,
					otherdistinct,
					N,
					n,
					K,
					variance,
					stddev;

		/*
		 * Estimated selectivity the least common 值会 have 如果 it
		 * wasn't in the MCV 列表 (c.f. eqsel()).
		 */
		selec = 1.0 - sumcount / samplerows - stanullfrac;
		if (selec < 0.0)
			selec = 0.0;
		if (selec > 1.0)
			selec = 1.0;
		otherdistinct = ndistinct_table - (num_mcv - 1);
		if (otherdistinct > 1)
			selec /= otherdistinct;

		/*
		 * If the 值 is kept in the MCV 列表, its population frequency is
		 * assumed 到 equal its 采样 frequency.  We use the lower 结束 of a
		 * textbook continuity-corrected Wald-类型 confidence interval 到
		 * determine 如果那个 is significantly 更多 common than the non-MCV
		 * frequency --- specifically we assume the population frequency is
		 * 高度 likely 到 be 在...内 around 2 standard errors of the 采样
		 * frequency, which equates 到 an interval of 2 standard deviations
		 * either side of the 采样 count, plus an additional 0.5 对于 the
		 * continuity correction.  Since we are 采样没有 replacement,
		 * 此 is a hypergeometric distribution.
		 *
		 * XXX: Empirically, 此 approach seems 到 work 相当 well, 但 it
		 * 可以 be worth considering 更多 advanced techniques 对于 estimating
		 * the confidence interval of the hypergeometric distribution.
		 */
		N = totalrows;
		n = samplerows;
		K = N * mcv_counts[num_mcv - 1] / n;
		variance = n * K * (N - K) * (N - n) / (N * N * (N - 1));
		stddev = sqrt(variance);

		if (mcv_counts[num_mcv - 1] > selec * samplerows + 2 * stddev + 0.5)
		{
			/*
			 * The 值 is significantly 更多 common than the non-MCV
			 * selectivity 会 suggest.  Keep it, 和所有 the other 更多
			 * common values in the 列表.
			 */
			break;
		}
		else
		{
			/* Discard 此值和 consider the 下一个 least common 值 */
			num_mcv--;
			if (num_mcv == 0)
				break;
			sumcount -= mcv_counts[num_mcv - 1];
		}
	}
	return num_mcv;
}
