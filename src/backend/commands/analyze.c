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
 *	do_analyze_rel() -- 分析一个关系，递归或不递归 
 *
 * 注意 "acquirefunc" 仅对于非继承情况有意义。           
 * 而对于继承情况，acquire_inherited_sample_rows() 会确定     
 * 适合每个子表的 acquirefunc。         
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
 * acquire_sample_rows -- 从表中获取一个随机行采样          
 *
 * 选中的行返回到调用者分配的数组 rows[] 中，该数组  
 * 至少要有 targrows 个条目。  
 * 实际选中的行数作为函数结果返回。                      
 * 我们还估算表中活元组和死元组的总数，                
 * 并分别返回到 *totalrows 和 *totaldeadrows 中。          
 *
 * 返回的元组列表按其在表中的物理位置排序。              
 * （后面推导相关度估计时将依赖这一点。）    
 *
 * 2004 年 5 月起改用新的两阶段方法：阶段一至多选  
 * 取 targrows 个随机块（若块数不足则取全部块）。   
 * 阶段二扫描这些块，并使用 Vitter 算法来构造         
 * 由 targrows 行组成的随机采样（若块采样中行数不足
 * 则取更少的行）。两个阶段同时执行：每一个        
 * 块都会在阶段一返回其编号后立即被处理，并且当  
 * 读取行时由阶段二控制哪些行将被插入               
 * 到采样中。 
 *
 * 虽然每一行进入最终采样的机会都相等，          
 * 但这种采样方法并不完美：并非每种可能的采样
 * 被选中的机会都相等。对于大型关系，               
 * 采样所代表的不同块的数量往往会偏少。              
 * 我们能暂时接受这一点。欢迎改进。                     
 *
 * 这种采样方法的一个重要性质是：因为我们确实    
 * 查看的是统计上无偏的块集合，应能得到       
 * 每块平均活元组数和死元组数的无偏                   
 * 估计。以前的采样方法过于相信                           
 * 靠近表开头的行密度。      
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
	 * 如果我们正在做预取，那么就提前告诉内核我们     
	 * 将要使用的第一批页。这也会把我们的              
	 * 迭代器移到正在使用的主迭代器之前，我们将让它     
	 * 留在那里，使我们始终超前 prefetch_maximum 个块进行  
	 * 预取
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

				/* 加入计数      */
				numrows += childrows;
				*totalrows += trows;
				*totaldeadrows += tdrows;
			}
		}

		/*
		 * 注意：我们无法释放子表锁，因为我们可能持有 
		 * 采样行中指向其 TOAST 表的指针。        
		 */
		table_close(childrel, NoLock);
		pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_DONE,
									 i + 1);
	}

	return numrows;
}


/*
 *	update_attstats() -- 更新一个关系的属性统计信息     
 *
 *		统计信息存于多处：该关系的 pg_class 行含有整    
 *		个关系统计信息，且每个（非系统）属性曾  
 *		被分析过，就有一个对应的 pg_statistic 行。      
 *		pg_class 的值由 VACUUM 更新，而非在此处更新。         
 *
 *		pg_statistic 行只是被正常地插入或更新。这意味    
 *		着在 vacuum 周期结束时 pg_statistic 很可能包含      
 *		一些已删除的行，除非它碰巧最后被 vacuum。           
 *
 *		为简单起见，我们对 pg_statistic 采取回避，不   
 *		尝试在 pg_statistic 中存储 pg_statistic 本身的行。    
 *		这或许可以设法做到，但不值得麻烦。                 
 *		注意 analyze_rel() 已确保对 pg_statistic 本身         
 *		做 vacuum 时不会来此。 
 *
 *		注意：如果两个后端进程能够并发地 ANALYZE 同    
 *		一张表，就会有竞态条件。目前我们通过在        
 *		analyze_rel() 中对该关系取自斥锁来防止这一点。   
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

		/* 无法收集统计信息时忽略该属性      */
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

		/* 该属性是否已有一个 pg_statistic 元组？        */
		oldtup = SearchSysCache3(STATRELATTINH,
								 ObjectIdGetDatum(relid),
								 Int16GetDatum(stats->attr->attnum),
								 BoolGetDatum(inh));

		/* 在确知需要时才打开索引信息        */
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
 * 供 compute_stats 子例程使用的标准获取函数。    
 *
 * 它的存在是为了在 compute_stats 例程与采样数据的      
 * 实际存储之间提供一些隔离。   
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
 * 用于分析索引表达式的获取函数。  
 *
 * 我们没有费心构造索引元组，数据只是存于          
 * Datum 数组。      
 */
static Datum
ind_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			i;

	/* exprvals 与 exprnulls 已按正确列做好偏移          */
	i = rownum * stats->rowstride;
	*isNull = stats->exprnulls[i];
	return stats->exprvals[i];
}


/*==========================================================================
 *
 * 此点以下的代码代表"标准"的类型特定统计信息          
 * 分析算法。通过对 pg_type.typanalyze 设置非零值，          
 * 即可按数据类型逐一替换这些代码。 
 *
 *==========================================================================
 */


/*
 * 为避免分析期间消耗过多内存，以及/或在得到的        
 * pg_statistic 行里占用过多空间，我们忽略去 toast              
 * 之后宽于 WIDTH_THRESHOLD 的 varlena datum。对                 
 * MCV 和不同值计算这是合理的，因为很宽的值不太    
 * 可能被重复，更不太会是最常见值。出于同样的理由，  
 * 忽略很宽的值不会太影响我们对直方图桶             
 * 边界的估计。   
 */
#define WIDTH_THRESHOLD  1024

#define swapInt(a,b)	do {int _tmp; _tmp=a; a=b; b=_tmp;} while(0)
#define swapDatum(a,b)	do {Datum _tmp; _tmp=a; a=b; b=_tmp;} while(0)

/*
 * 默认分析例程所使用的额外信息             
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
 * std_typanalyze -- 默认的类型特定 typanalyze 函数      
 */
bool
std_typanalyze(VacAttrStats *stats)
{
	Form_pg_attribute attr = stats->attr;
	Oid			ltopr;
	Oid			eqopr;
	StdAnalyzeData *mystats;

	/* 若 attstattarget 列为负值，则使用默认值            */
	/* 注意：stats->attr 是副本，可随意改写            */
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
	 * 确定要使用哪种标准统计信息算法       
	 */
	if (OidIsValid(eqopr) && OidIsValid(ltopr))
	{
		/* 应是标量数据类型      */
		stats->compute_stats = compute_scalar_stats;
		/*--------------------
		 * minrows 的如下选择基于论文                  
		 * "Random sampling for histogram construction: how much is enough?"
		 * by Surajit Chaudhuri, Rajeev Motwani and Vivek Narasayya, in
		 * 载于 ACM SIGMOD 数据管理国际会议论文集，1998      
		 * 年，第 436-447 页。他们由定理 5 得到的推论 1  
		 * 指出：对表大小 n、直方图大小 k、桶大小最大 
		 * 相对误差 f、错误概率 gamma，最小随机采样     
		 * 规模为            
		 *		r = 4 * k * ln(2*n/gamma) / f^2
		 * 取 f = 0.5、gamma = 0.01、n = 10^6 行，可得    
		 *		r = 305.82 * k
		 * 注意由于对数函数，对 n 的依赖很弱；即使在 
		 * n = 10^12 时，300*k 的采样给出的桶大小误差    
		 * 概率为 0.99。因此没有必要针对 n 进行缩放，这   
		 * 是件好事，因为在此阶段我们未必                  
		 * 知道 n 的值。    
		 *--------------------
		 */
		stats->minrows = 300 * attr->attstattarget;
	}
	else if (OidIsValid(eqopr))
	{
		/* 我们仍可识别不同值            */
		stats->compute_stats = compute_distinct_stats;
		/* 不妨使用与上面相同的 minrows      */
		stats->minrows = 300 * attr->attstattarget;
	}
	else
	{
		/* 只能做些平凡处理            */
		stats->compute_stats = compute_trivial_stats;
		/* 不妨使用与上面相同的 minrows      */
		stats->minrows = 300 * attr->attstattarget;
	}

	return true;
}


/*
 *	compute_trivial_stats() -- 计算很基本的列统计信息   
 *
 *	当无法为该数据类型找到哈希 "=" 操作符时使用。    
 *
 *	我们确定非空行的比例以及平均 datum 宽度。             
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
		 * 如果它是变宽字段，则把宽度累加起来用于平均
		 * 宽度计算。注意若值已被 TOAST，则我们使用 TOAST    
		 * 后的宽度。若它是固定宽度类                            
		 * 型  
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));
		}
		else if (is_varwidth)
		{
			/* 必为 cstring  */
			total_width += strlen(DatumGetCString(value)) + 1;
		}
	}

	/* 只有找到一些非空值才能计算平均宽度                 */
	if (nonnull_cnt > 0)
	{
		stats->stats_valid = true;
		/* 做简单的空值比例与宽度统计 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}
	else if (null_cnt > 0)
	{
		/* 只发现空值；假定该列整个为空              */
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
 *	compute_distinct_stats() -- 计算包括 ndistinct 的列统计信息    
 *
 *	当对该数据类型只能找到 "=" 操作符时使用。        
 *
 *	我们确定非空行的比例、平均宽度、最常见值      
 *	以及（估计的）不同值数量。                           
 *
 *	最常见值通过蛮力方式确定：我们维护一个列表，  
 *	其中存放之前见过的值，并按见到的次数排序，       
 *	在扫描采样时。新见到的值被插到最后一个       
 *	多次见到的值之后，这使得最底部（最旧）的仅见    
 *	过一次的值掉出列表。这一方法的精度及其成本       
 *	主要取决于我们愿意保留的列表长度。            
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
	 * 对 n 元素的 MCV 列表最多跟踪 2*n 个值；但至少 10    
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
		 * 若为变宽字段，则累加宽度以计算平均宽度。   
		 * 注意：若值已被 TOAST，我们采用 TOAST 后的宽度。   
		 * 若为定宽类型则无需费心做此计算                      
		 * 。  
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * 若值已被 TOAST，我们希望只解 TOAST 一次以免 
			 * 在比较期间反复解 TOAST 及其带来的过量内存  
			 * 占用。另外检查值是否过宽，若是则完全不  
			 * 做解 TOAST --- 直接                                  
			 * 忽略该值。  
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
			/* 必为 cstring  */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/*
		 * 看看该值是否与我们已跟踪的某个值匹配。
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
			/* 此值现在可能需要在跟踪列表中"冒泡"了    */
			while (j > 0 && track[j].count > track[j - 1].count)
			{
				swapDatum(track[j].value, track[j - 1].value);
				swapInt(track[j].count, track[j - 1].count);
				j--;
			}
		}
		else
		{
			/* 无匹配。插入 count-1 列表头部   */
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

	/* 只有找到非空值，我们才能算出真正的统计信息  */
	if (nonnull_cnt > 0)
	{
		int			nmultiple,
					summultiple;

		stats->stats_valid = true;
		/* 做简单的空值比例与宽度统计 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		/* 统计多次出现的值的数目                  */
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
			 * 若未发现重复的非空值，则假定它是唯一列； 
			 * 但务必对我们发现的任何空值打折扣。    
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (track_cnt < track_max && toowide_cnt == 0 &&
				 nmultiple == track_cnt)
		{
			/*
			 * 我们的跟踪列表包含采样中的每一个值，且每
			 * 个值都不止出现一次。假定该列只有这       
			 * 些值。（本情况旨在处理取值为小型固定    
			 * 集合的列，例如 boolean 或 enum 列。若采样中    
			 * 有任何值只出现一次（包括过宽的值），我们  
			 * 就应假定我们要处理的                                 
			 * 并非这种情况。）     
			 */
			stats->stadistinct = track_cnt;
		}
		else
		{
			/*----------
			 * 使用 Haas 与 Stokes 提出的估计器来估计        
			 * 不同值的个数，见 IBM 研究报告 RJ 10025：       
			 *		n*d / (n - f1 + f1*n/N)
			 * 其中 f1 是 n 行样本（取自共 N 行）中恰好
			 * 出现一次的不同值的个数，而 d 是            
			 * 样本中不同值的总数。                             
			 * 这是他们的 Duj1 估计器；他们推荐的其他 
			 * 估计器要复杂得多，且在 n 远小于 N 时数值   
			 * 上非常不稳定。                       
			 *
			 * 本计算中我们只考虑非空值。过去我们曾把   
			 * 含空值的行计入 n 与 N 的计数，但这会在空    
			 * 值很多的列中导致不准确的结果；而且考虑  
			 * 到期望结果是不同非空值的个数，这本来就不   
			 * 合常理。                           
			 *
			 * 我们（不太可靠地！）假定所有多次出现的     
			 * 值都体现在最终的 track[] 列表中，而其他非空 
			 * 值则都只出现过一次。（XXX 这通常          
			 * 会大幅高估 ndistinct。我们能否做得更         
			 * 好？)     
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

			/* 若有舍入误差，则钳制到合理范围 */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* 再舍入为整数   */
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * 若我们估计的不同值个数超过总行数的 10%（       
		 * 相当随意的界限），则假定 stadistinct 应随行数 
		 * 缩放而不是取固定值                                       
		 * 。   
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * 决定多少值值得存为最常见值。如果我们能够生成
		 * 一个完整的 MCV 列表（采样中的所有值都放得下， 
		 * 且我们认为这些就是表中的全部值），那就这样    
		 * 做。否则只存储那些比不在列表中的值     
		 * 明显更常见的值。                                  
		 *
		 * 注：第一种情况旨在处理可能取值为小型固定集
		 * 合的列，例如 boolean 或 enum 列。若我们能用一 
		 * 个能放进统计信息目标的 MCV 列表*完全*表示该      
		 * 列的全体取值，那我们就应当这么做，从而为      
		 * 规划器提供完整的信息。但如果 MCV 列表不完       
		 * 整，通常更值得有所取舍，而不是把它一路    
		 * 填满直至统计信息目标                                
		 * 。    
		 */
		if (track_cnt < track_max && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* 跟踪列表含所有已见值，且全部放得下   */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* 列表不完整；决定有多少值值得保留          */
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

		/* 生成 MCV 槽位条目 */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* 必须把目标值复制到 anl_context 中  */
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
			 * 接受 stats->statypid 等字段的默认值。              
			 * 在调用前已设置好（见 vacuum.h）   
			 */
		}
	}
	else if (null_cnt > 0)
	{
		/* 我们只发现了空值；假定该列完全为空     */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/* 我们无需费心清理我们的任何临时 palloc 分配       */
}


/*
 *	compute_scalar_stats() -- 计算列统计信息    
 *
 *	当能为该数据类型找到 "=" 与 "<" 操作符时使用。    
 *
 *	我们确定非空行比例、平均宽度、最常见值、      
 *	（估计的）不同值个数、分布直方图，以及物理   
 *	顺序与逻辑顺序的相关度。                                     
 *
 *	将数据值排序后，所需的统计信息便可相当容易    
 *	地确定。           
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
	 * 目前不执行缩写键转换，因为 MCV 槽的生成需要        
	 * 完整值。要支持该优化，就得教会 compare_scalars() 调
	 * 用平局决胜函数。                                           
	 */
	ssup.abbreviate = false;

	PrepareSortSupportFromOrderingOp(mystats->ltopr, &ssup);

	/* 初次扫描以找出可排序的值 */
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
		 * 若为变宽字段，则累加宽度以计算平均宽度。   
		 * 注意：若值已被 TOAST，我们采用 TOAST 后的宽度。   
		 * 若为定宽类型则无需费心做此计算                      
		 * 。  
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * 若值已被 TOAST，我们希望只解 TOAST 一次以免 
			 * 在比较期间反复解 TOAST 及其带来的过量内存  
			 * 占用。另外检查值是否过宽，若是则完全不  
			 * 做解 TOAST --- 直接                                  
			 * 忽略该值。  
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
			/* 必为 cstring  */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/* 将其加入待排序列表     */
		values[values_cnt].value = value;
		values[values_cnt].tupno = values_cnt;
		tupnoLink[values_cnt] = values_cnt;
		values_cnt++;
	}

	/* 只有找到可排序值，才能算出真正的统计信息     */
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
		 * 现在按顺序扫描各值，找出最常见者，同时还     
		 * 累积顺序相关度统计信息。       
		 *
		 * 为确定哪些值最常见，首先必须统计每个值的  
		 * 重复数目。各重复值在已排序列表中是相邻          
		 * 的，所以暴力做法是连续比较各 datum 值，直到      
		 * 发现两个不相等的值为止。然而，这需要          
		 * 调用 N-1 次 datum 比较例程，这与排序期间已做       
		 * 工作完全重复。（排序算法在某个时刻必定比       
		 * 较过已排序顺序中相邻的每一对元素；否则它      
		 * 不可能知道该顺序是否正确。）我们利用这一      
		 * 点：让 compare_scalars 记住每个 ScalarItem 被发现等  
		 * 于的最高 tupno 索引。排序结束时，一个      
		 * ScalarItem 的 tupnoLink 若仍指向自身，当且仅当它 
		 * 是其重复组中的最后一个元素                            
		 * （因为该组将按 tupno 排序）                            
		 * 。                  
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
				/* 已到达该值重复项的末尾       */
				ndistinct++;
				if (dups_cnt > 1)
				{
					nmultiple++;
					if (track_cnt < num_mcv ||
						dups_cnt > track[track_cnt - 1].count)
					{
						/*
						 * 为 mcv 列表找到一个新条目；     
						 * 确定其位置，必要时将旧条目下移。 
						 * 循环不变式是 j 指向空/可替换槽位     
						 * 。  
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
		/* 做简单的空值比例与宽度统计 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		if (nmultiple == 0)
		{
			/*
			 * 若未发现重复的非空值，则假定它是唯一列； 
			 * 但务必对我们发现的任何空值打折扣。    
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (toowide_cnt == 0 && nmultiple == ndistinct)
		{
			/*
			 * 采样中的每个值都出现了不止一次。该列只     
			 * 有这些值。（本情况旨在处理可能取值为小型 
			 * 固定集合的列，例如 boolean 或 enum 列。若    
			 * 有任何值只在采样中出现一次（包括过宽的    
			 * 值），我们就应假定我们要处理的并非这种    
			 * 情况。）                                    
			 */
			stats->stadistinct = ndistinct;
		}
		else
		{
			/*----------
			 * 用估计器估算不同值的数量，该估计器由    
			 * Haas 与 Stokes 提出，见 IBM 研究报告 RJ 10025：   
			 *		n*d / (n - f1 + f1*n/N)
			 * 其中 f1 为在 n 行样本（取自总共 N 行）   
			 * 中恰好只出现一次的不同值个数；d 则为    
			 * 样本中的不同值总数。                             
			 * 这是他们的 Duj1 估计器；                      
			 * 他们推荐的其余估计器复杂得多，且数值上   
			 * 极不稳定，当 n 远小于 N 时。     
			 *
			 * 在该计算中我们只考虑非空值。我们曾         
			 * 把空值行计入 n 与 N 的计数，但这在空值多    
			 * 的列上导致不准确的结果；且无论怎么说，  
			 * 既然想要的结果是非空不同值的数量，这种      
			 * 做法在直觉上就是错的。      
			 *
			 * 超宽值均被假定原本就互不相同。      
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

			/* 以防舍入误差，钳制到合理范围    */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* 接着舍入为整数*/
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * 如果我们估算出的不同值数量超过总行数的        
		 * 10%（一个非常任意的界限），那么就假定        
		 * stadistinct 应随行数伸缩，而不是一个固定            
		 * 值。
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * 决定有多少个值值得存为最常见值。如果我们      
		 * 能够生成一个完整的 MCV 列表（样本中的所有值    
		 * 都放得下，且我们认为这些就是表里的全部值）， 
		 * 那就这么做。否则只存储那些比不在列表  
		 * 中的值明显更常见的那些值。                   
		 *
		 * 注意：第一种情况旨在应对具有少量、固定的   
		 * 可能值集合的列，比如 boolean 或 enum              
		 * 列。如果放得进统计信息目标的一个 MCV 列表就能  
		 * *完整地* 表示列的取值总体，那我们就应当这么   
		 * 做，从而向规划器提供完整的信息。但如果 MCV     
		 * 列表不完整，那么通常值得更有选择性，而    
		 * 不是一味把它一路填满到统计信息目              
		 * 标。 
		 */
		if (track_cnt == ndistinct && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* 跟踪列表含全部所见值，且全部放得下   */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* 列表不完整；决定有多少个值值得保留       */
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

		/* 生成 MCV 槽位条目 */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* 必须把目标值复制进 anl_context      */
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
			 * 接受 stats->statypid 等字段的默认值。它们在     
			 * 我们被调用前已设置（见 vacuum.h）
			 */
			slot_idx++;
		}

		/*
		 * 如果有至少两个未被 MCV 列表涵盖的不同值，       
		 * 则生成直方图槽位条目。（这确保直方图不会
		 * 塌缩为空或只剩单个值。）              
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

			/* 把 MCV 项按位置排序，以加速下一个循环     */
			qsort_interruptible(track, num_mcv, sizeof(ScalarMCVItem),
								compare_mcvs, NULL);

			/*
			 * 从 values[] 数组中剔除 MCV 项。            
			 *
			 * 注意此处我们破坏了 values[] 数组……但不再     
			 * 用它做任何事。不过我们仍需要 values_cnt。  
			 * nvals 将是 values[] 中剩余条目的数量。         
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
							/* 越过此 MCV 项          */
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

			/* 必须把目标值复制进 anl_context      */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * 本循环的目的是复制 values[] 的第一个和最后一 
			 * 个条目以及其间等距的值。因此第 i 个值是  
			 * values[(i * (nvals - 1)) / (num_hist - 1)]。但              
			 * 直接计算该下标在统计信息目标超过几千时有 
			 * 整数溢出风险。于是我们改为每步给 pos 加上 
			 * (nvals - 1) / (num_hist - 1)，并分别跟踪                
			 * 和的整数部分与小数部分。                    
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
					/* 小数部分超过 1，进位到整数部分     */
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
			 * 接受 stats->statypid 等字段的默认值。它们在     
			 * 我们被调用前已设置（见 vacuum.h）
			 */
			slot_idx++;
		}

		/* 如果有多个值，则生成一个相关度条目       */
		if (values_cnt > 1)
		{
			MemoryContext old_context;
			float4	   *corrs;
			double		corr_xsum,
						corr_x2sum;

			/* 必须把目标值复制进 anl_context      */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			corrs = (float4 *) palloc(sizeof(float4));
			MemoryContextSwitchTo(old_context);

			/*----------
			 * 既然已知 x 与 y 的取值集合均为   
			 *		0, 1, ..., values_cnt-1
			 * 可得 sum(x) = sum(y) = 
			 *		(values_cnt-1)*values_cnt / 2
			 * 且 sum(x^2) = sum(y^2) =
			 *		(values_cnt-1)*values_cnt*(2*values_cnt-1) / 6.
			 *----------
			 */
			corr_xsum = ((double) (values_cnt - 1)) *
				((double) values_cnt) / 2.0;
			corr_x2sum = ((double) (values_cnt - 1)) *
				((double) values_cnt) * (double) (2 * values_cnt - 1) / 6.0;

			/* 于是相关度系数就简化为          */
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
		/* 我们找到了一些非空值，但它们全都太宽    */
		Assert(nonnull_cnt == toowide_cnt);
		stats->stats_valid = true;
		/* 做简单的空值比例与宽度统计 */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		/* 假定所有过宽值互不相同，因此这是一个唯一列  */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
	}
	else if (null_cnt > 0)
	{
		/* 我们只找到了空值；假定该列全为空值     */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/* 我们无需费心清理任何用 palloc 分配的临时内存    */
}

/*
 * 排序 ScalarItems 的比较器   
 *
 * 除了对条目排序外，我们还更新 tupnoLink[] 数   
 * 组——每当发现两个 ScalarItems 含有相等 datum 时。该  
 * 组以 tupno 索引；对每个 ScalarItem，它存有该        
 * datum 被发现与之相等的最大 tupno。这让我们能            
 * 避免在 compute_scalar_stats() 中做额外的比较。    
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
	 * 两个 datum 相等，故更新 cxt->tupnoLink[]。  
	 */
	if (cxt->tupnoLink[ta] < tb)
		cxt->tupnoLink[ta] = tb;
	if (cxt->tupnoLink[tb] < ta)
		cxt->tupnoLink[tb] = ta;

	/*
	 * 相等 datum 按 tupno 排序  
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
 * 分析采样中常见值的列表，并决定其中有多少个        
 * 值得存入表的 MCV 列表。      
 *
 * mcv_counts 被假定为最常见值的出现次数组成的列表，      
 * 按从最常见开始排列。返回值是其中比不在列表            
 * 中的值明显更常见、且因此被认为值得存入                 
 * 表的 MCV 列表的那些值的数量。                            
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
	 * 如果整张表都被采样，则保留整个列表。这也    
	 * 保护我们免于在下面代码中出现除零。    
	 */
	if (samplerows == totalrows || totalrows <= 1.0)
		return num_mcv;

	/* 重新提取表中非空不同值数量的估算值                 */
	ndistinct_table = stadistinct;
	if (ndistinct_table < 0)
		ndistinct_table = -ndistinct_table * totalrows;

	/*
	 * 从 MCV 列表中剔除最不常见的值，如果它们并不    
	 * 明显比它们不在列表中时的估计选择率更常见。    
	 * 所有非 MCV 值都被假定为同等常见，这是在考虑了      
	 * MCV 列表中所有值的频率以及空值数量之后得出       
	 * 的（参见 eqsel()）。                                    
	 *
	 * 这里 sumcount 跟踪 MCV 列表中除最后一个（最不           
	 * 常见）值之外所有值的总计数，让我们能确定剔除     
	 * 该值的影响。       
	 *
	 * 注意我们有意通过从完整列表中移除值来完成，   
	 * 而不是从一个空列表开始逐个加入值，因为后者 
	 * 这种做法在所有最常见值的频率彼此接近并且构       
	 * 成表的多数时，可能一个值也加不进去，               
	 * 以至于所有值的总体平均频率与常见值的频率        
	 * 大致相同。这会导致任何不常见值                         
	 * 被显著高估。                                
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
		 * 最不常见的值如果不在 MCV 列表中时应有的估  
		 * 计选择率（参见 eqsel()）。   
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
		 * 如果该值保留在 MCV 列表中，则假定其总体频率   
		 * 等于其采样频率。我们使用一个教科书式的经过  
		 * 连续性校正的 Wald 型置信区间的下端来确定     
		 * 它是否明显比非 MCV 频率更常见——具体而       
		 * 言，我们假定总体频率极有可能落在采样频率    
		 * 附近约 2 个标准误差的范围内——这等同于采样  
		 * 计数两侧各 2 个标准差的区间，外加为连续性    
		 * 校正而额外加上的 0.5。由于我们的采样是不     
		 * 放回的，所以这是一个                                    
		 * 超几何分布。                    
		 *
		 * XXX: 从经验上看，这种方法效果相当好，但        
		 * 或许值得考虑用更先进的技术来估计超几何       
		 * 分布的置信区间。                                   
		 */
		N = totalrows;
		n = samplerows;
		K = N * mcv_counts[num_mcv - 1] / n;
		variance = n * K * (N - K) * (N - n) / (N * N * (N - 1));
		stddev = sqrt(variance);

		if (mcv_counts[num_mcv - 1] > selec * samplerows + 2 * stddev + 0.5)
		{
			/*
			 * 该值明显比非 MCV 选择率暗示的更常见。  
			 * 保留它，以及列表中所有其他                    
			 * 更常见的值。        
			 */
			break;
		}
		else
		{
			/* 丢弃该值，然后考虑下一个最不常见的值      */
			num_mcv--;
			if (num_mcv == 0)
				break;
			sumcount -= mcv_counts[num_mcv - 1];
		}
	}
	return num_mcv;
}
