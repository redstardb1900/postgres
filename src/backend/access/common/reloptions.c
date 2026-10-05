/*-------------------------------------------------------------------------
 *
 * reloptions.c
 *	  关系选项（pg_class.reloptions）的核心支持
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/reloptions.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>

#include "access/gist_private.h"
#include "access/hash.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/spgist_private.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "commands/view.h"
#include "nodes/makefuncs.h"
#include "postmaster/postmaster.h"
#include "utils/array.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * pg_class.reloptions 的内容
 *
 * 增加一个选项的步骤：
 *
 * (i) 确定类型（整数、实数、布尔、字符串）、名称、默认值，
 * 以及上下界（若适用）；对于字符串，考虑准备一个校验
 * 例程。
 * (ii) 在下面增加一条记录（或使用 add_<type>_reloption）。
 * (iii) 把它加入合适的选项结构体（或许是 StdRdOptions）
 * (iv) 把它加入合适的处理例程（或许是
 * default_reloptions）
 * (v) 确保该操作的锁级别设置正确
 * (vi) 不要忘记为该选项编写文档
 *
 * 任何新选项的默认选择都应是 AccessExclusiveLock。
 * 某些情况下可以从这里降低锁级别，但所选的锁
 * 级别必须始终与自身冲突，这样在尝试并发修改时，多次
 * 修改才不会丢失。
 * 锁级别的选择完全取决于该参数
 * 在服务器内部如何使用，而不是你希望如何以及何时修改它。
 * 安全优先。现有选择记录在这里，以及使用这些参数的
 * 其他后端代码中。
 *
 * 一般而言，任何会影响 SELECT 所得结果的内容都必须由
 * AccessExclusiveLock 保护。
 *
 * 与自动清理相关的参数可以在 ShareUpdateExclusiveLock 下设置，
 * 因为它们只被自动清理进程使用，并且不会改变任何
 * 当前正在执行的工作。
 *
 * 填充因子可以设置，因为它只作用于此后对
 * 数据块所做的修改，如 hio.c 中所记载
 *
 * n_distinct 选项可以在 ShareUpdateExclusiveLock 下设置，因为它们
 * 只在 ANALYZE 期间使用，而 ANALYZE 使用 ShareUpdateExclusiveLock，
 * 因此进行中的修改不会影响这次 ANALYZE。修改这些
 * 值要到下一次 ANALYZE 才生效，因此不必用更强的锁。
 *
 * 与规划器相关的参数可以用 ShareUpdateExclusiveLock 设置，因为
 * 它们只影响规划，不影响执行的正确性。计划
 * 无法在执行途中改变，因此这里的修改也无法轻易产生
 * 新的改进计划。于是我们允许现有查询继续
 * 执行、现有计划继续保留；为了允许更好的
 * 计划并发引入且不干扰用户，这点代价值得付出。
 *
 * 设置 parallel_workers 是安全的，因为它的作用与
 * max_parallel_workers_per_gather 相同，后者是 USERSET 参数，不会
 * 影响现有计划或查询。
 *
 * vacuum_truncate 可以在 ShareUpdateExclusiveLock 下设置，因为它
 * 只在 VACUUM 期间使用，而 VACUUM 使用 ShareUpdateExclusiveLock，
 * 因此进行中的修改不会影响这次 VACUUM。修改它的
 * 值要到下一次 VACUUM 才生效，因此不必用更强的锁。
 */

static relopt_bool boolRelOpts[] =
{
	{
		{
			"autosummarize",
			"Enables automatic summarization on this BRIN index",
			RELOPT_KIND_BRIN,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"autovacuum_enabled",
			"Enables autovacuum in this relation",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		true
	},
	{
		{
			"user_catalog_table",
			"Declare a table as an additional catalog table, e.g. for the purpose of logical replication",
			RELOPT_KIND_HEAP,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"fastupdate",
			"Enables \"fast update\" feature for this GIN index",
			RELOPT_KIND_GIN,
			AccessExclusiveLock
		},
		true
	},
	{
		{
			"security_barrier",
			"View acts as a row security barrier",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"security_invoker",
			"Privileges on underlying relations are checked as the invoking user, not the view owner",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"vacuum_truncate",
			"Enables vacuum to truncate empty pages at the end of this table",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		true
	},
	{
		{
			"deduplicate_items",
			"Enables \"deduplicate items\" feature for this btree index",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock	/* 因为它只作用于之后的
										 * 插入 */
		},
		true
	},
	/* 列表终止符 */
	{{NULL}}
};

static relopt_int intRelOpts[] =
{
	{
		{
			"fillfactor",
			"Packs table pages only to this percentage",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock	/* 因为它只作用于之后的
										 * 插入 */
		},
		HEAP_DEFAULT_FILLFACTOR, HEAP_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs btree index pages only to this percentage",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock	/* 因为它只作用于之后的
										 * 插入 */
		},
		BTREE_DEFAULT_FILLFACTOR, BTREE_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs hash index pages only to this percentage",
			RELOPT_KIND_HASH,
			ShareUpdateExclusiveLock	/* 因为它只作用于之后的
										 * 插入 */
		},
		HASH_DEFAULT_FILLFACTOR, HASH_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs gist index pages only to this percentage",
			RELOPT_KIND_GIST,
			ShareUpdateExclusiveLock	/* 因为它只作用于之后的
										 * 插入 */
		},
		GIST_DEFAULT_FILLFACTOR, GIST_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs spgist index pages only to this percentage",
			RELOPT_KIND_SPGIST,
			ShareUpdateExclusiveLock	/* 因为它只作用于之后的
										 * 插入 */
		},
		SPGIST_DEFAULT_FILLFACTOR, SPGIST_MIN_FILLFACTOR, 100
	},
	{
		{
			"autovacuum_vacuum_threshold",
			"Minimum number of tuple updates or deletes prior to vacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, INT_MAX
	},
	{
		{
			"autovacuum_vacuum_insert_threshold",
			"Minimum number of tuple inserts prior to vacuum, or -1 to disable insert vacuums",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-2, -1, INT_MAX
	},
	{
		{
			"autovacuum_analyze_threshold",
			"Minimum number of tuple inserts, updates or deletes prior to analyze",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0, INT_MAX
	},
	{
		{
			"autovacuum_vacuum_cost_limit",
			"Vacuum cost amount available before napping, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 1, 10000
	},
	{
		{
			"autovacuum_freeze_min_age",
			"Minimum age at which VACUUM should freeze a table row, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1000000000
	},
	{
		{
			"autovacuum_multixact_freeze_min_age",
			"Minimum multixact age at which VACUUM should freeze a row multixact's, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1000000000
	},
	{
		{
			"autovacuum_freeze_max_age",
			"Age at which to autovacuum a table to prevent transaction ID wraparound",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 100000, 2000000000
	},
	{
		{
			"autovacuum_multixact_freeze_max_age",
			"Multixact age at which to autovacuum a table to prevent multixact wraparound",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 10000, 2000000000
	},
	{
		{
			"autovacuum_freeze_table_age",
			"Age at which VACUUM should perform a full table sweep to freeze row versions",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		}, -1, 0, 2000000000
	},
	{
		{
			"autovacuum_multixact_freeze_table_age",
			"Age of multixact at which VACUUM should perform a full table sweep to freeze row versions",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		}, -1, 0, 2000000000
	},
	{
		{
			"log_autovacuum_min_duration",
			"Sets the minimum execution time above which autovacuum actions will be logged",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, -1, INT_MAX
	},
	{
		{
			"toast_tuple_target",
			"Sets the target tuple length at which external columns will be toasted",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		TOAST_TUPLE_TARGET, 128, TOAST_TUPLE_TARGET_MAIN
	},
	{
		{
			"pages_per_range",
			"Number of pages that each page range covers in a BRIN index",
			RELOPT_KIND_BRIN,
			AccessExclusiveLock
		}, 128, 1, 131072
	},
	{
		{
			"gin_pending_list_limit",
			"Maximum size of the pending list for this GIN index, in kilobytes.",
			RELOPT_KIND_GIN,
			AccessExclusiveLock
		},
		-1, 64, MAX_KILOBYTES
	},
	{
		{
			"effective_io_concurrency",
			"Number of simultaneous requests that can be handled efficiently by the disk subsystem.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
#ifdef USE_PREFETCH
		-1, 0, MAX_IO_CONCURRENCY
#else
		0, 0, 0
#endif
	},
	{
		{
			"maintenance_io_concurrency",
			"Number of simultaneous requests that can be handled efficiently by the disk subsystem for maintenance work.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
#ifdef USE_PREFETCH
		-1, 0, MAX_IO_CONCURRENCY
#else
		0, 0, 0
#endif
	},
	{
		{
			"parallel_workers",
			"Number of parallel processes that can be used per executor node for this relation.",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1024
	},

	/* 列表终止符 */
	{{NULL}}
};

static relopt_real realRelOpts[] =
{
	{
		{
			"autovacuum_vacuum_cost_delay",
			"Vacuum cost delay in milliseconds, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_vacuum_scale_factor",
			"Number of tuple updates or deletes prior to vacuum as a fraction of reltuples",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_vacuum_insert_scale_factor",
			"Number of tuple inserts prior to vacuum as a fraction of reltuples",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_analyze_scale_factor",
			"Number of tuple inserts, updates or deletes prior to analyze as a fraction of reltuples",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"seq_page_cost",
			"Sets the planner's estimate of the cost of a sequentially fetched disk page.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, DBL_MAX
	},
	{
		{
			"random_page_cost",
			"Sets the planner's estimate of the cost of a nonsequentially fetched disk page.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, DBL_MAX
	},
	{
		{
			"n_distinct",
			"Sets the planner's estimate of the number of distinct values appearing in a column (excluding child relations).",
			RELOPT_KIND_ATTRIBUTE,
			ShareUpdateExclusiveLock
		},
		0, -1.0, DBL_MAX
	},
	{
		{
			"n_distinct_inherited",
			"Sets the planner's estimate of the number of distinct values appearing in a column (including child relations).",
			RELOPT_KIND_ATTRIBUTE,
			ShareUpdateExclusiveLock
		},
		0, -1.0, DBL_MAX
	},
	{
		{
			"vacuum_cleanup_index_scale_factor",
			"Deprecated B-Tree parameter.",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 1e10
	},
	/* 列表终止符 */
	{{NULL}}
};

/* 取值来自 StdRdOptIndexCleanup */
static relopt_enum_elt_def StdRdOptIndexCleanupValues[] =
{
	{"auto", STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO},
	{"on", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"off", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{"true", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"false", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{"yes", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"no", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{"1", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"0", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{(const char *) NULL}		/* 列表终止符 */
};

/* 取值来自 GistOptBufferingMode */
static relopt_enum_elt_def gistBufferingOptValues[] =
{
	{"auto", GIST_OPTION_BUFFERING_AUTO},
	{"on", GIST_OPTION_BUFFERING_ON},
	{"off", GIST_OPTION_BUFFERING_OFF},
	{(const char *) NULL}		/* 列表终止符 */
};

/* 取值来自 ViewOptCheckOption */
static relopt_enum_elt_def viewCheckOptValues[] =
{
	/* NOT_SET 没有对应取值 */
	{"local", VIEW_OPTION_CHECK_OPTION_LOCAL},
	{"cascaded", VIEW_OPTION_CHECK_OPTION_CASCADED},
	{(const char *) NULL}		/* 列表终止符 */
};

static relopt_enum enumRelOpts[] =
{
	{
		{
			"vacuum_index_cleanup",
			"Controls index vacuuming and index cleanup",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		StdRdOptIndexCleanupValues,
		STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO,
		gettext_noop("Valid values are \"on\", \"off\", and \"auto\".")
	},
	{
		{
			"buffering",
			"Enables buffering build for this GiST index",
			RELOPT_KIND_GIST,
			AccessExclusiveLock
		},
		gistBufferingOptValues,
		GIST_OPTION_BUFFERING_AUTO,
		gettext_noop("Valid values are \"on\", \"off\", and \"auto\".")
	},
	{
		{
			"check_option",
			"View has WITH CHECK OPTION defined (local or cascaded).",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		viewCheckOptValues,
		VIEW_OPTION_CHECK_OPTION_NOT_SET,
		gettext_noop("Valid values are \"local\" and \"cascaded\".")
	},
	/* 列表终止符 */
	{{NULL}}
};

static relopt_string stringRelOpts[] =
{
	/* 列表终止符 */
	{{NULL}}
};

static relopt_gen **relOpts = NULL;
static bits32 last_assigned_kind = RELOPT_KIND_LAST_DEFAULT;

static int	num_custom_options = 0;
static relopt_gen **custom_options = NULL;
static bool need_initialization = true;

static void initialize_reloptions(void);
static void parse_one_reloption(relopt_value *option, char *text_str,
								int text_len, bool validate);

/*
 * 取得字符串关系选项的长度（默认值或用户定义的
 * 值）。构造一组关系选项时，用它来
 * 分配空间。
 */
#define GET_STRING_RELOPTION_LEN(option) \
	((option).isset ? strlen((option).values.string_val) : \
	 ((relopt_string *) (option).gen)->default_len)

/*
 * initialize_reloptions
 *		初始化例程，必须在解析之前调用
 *
 * 初始化 relOpts 数组，并填入每个变量的类型与名称长度。
 */
static void
initialize_reloptions(void)
{
	int			i;
	int			j;

	j = 0;
	for (i = 0; boolRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(boolRelOpts[i].gen.lockmode,
								   boolRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; intRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(intRelOpts[i].gen.lockmode,
								   intRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; realRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(realRelOpts[i].gen.lockmode,
								   realRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; enumRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(enumRelOpts[i].gen.lockmode,
								   enumRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; stringRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(stringRelOpts[i].gen.lockmode,
								   stringRelOpts[i].gen.lockmode));
		j++;
	}
	j += num_custom_options;

	if (relOpts)
		pfree(relOpts);
	relOpts = MemoryContextAlloc(TopMemoryContext,
								 (j + 1) * sizeof(relopt_gen *));

	j = 0;
	for (i = 0; boolRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &boolRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_BOOL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; intRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &intRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_INT;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; realRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &realRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_REAL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; enumRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &enumRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_ENUM;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; stringRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &stringRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_STRING;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; i < num_custom_options; i++)
	{
		relOpts[j] = custom_options[i];
		j++;
	}

	/* 加上列表终止符 */
	relOpts[j] = NULL;

	/* 标记这项工作已完成 */
	need_initialization = false;
}

/*
 * add_reloption_kind
 *		创建一个新的 relopt_kind 值，供用户定义的访问方法
 *		在自定义关系选项中使用。
 */
relopt_kind
add_reloption_kind(void)
{
	/* 不交出最后一位，以便该枚举的行为可移植 */
	if (last_assigned_kind >= RELOPT_KIND_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("user-defined relation parameter types limit exceeded")));
	last_assigned_kind <<= 1;
	return (relopt_kind) last_assigned_kind;
}

/*
 * add_reloption
 *		把已创建的自定义关系选项加入列表，并重新计算
 *		主解析表。
 */
static void
add_reloption(relopt_gen *newoption)
{
	static int	max_custom_options = 0;

	if (num_custom_options >= max_custom_options)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		if (max_custom_options == 0)
		{
			max_custom_options = 8;
			custom_options = palloc(max_custom_options * sizeof(relopt_gen *));
		}
		else
		{
			max_custom_options *= 2;
			custom_options = repalloc(custom_options,
									  max_custom_options * sizeof(relopt_gen *));
		}
		MemoryContextSwitchTo(oldcxt);
	}
	custom_options[num_custom_options++] = newoption;

	need_initialization = true;
}

/*
 * init_local_reloptions
 *		初始化局部关系选项，它们将被解析进大小为
 * 		'relopt_struct_size' 的 bytea 结构。
 */
void
init_local_reloptions(local_relopts *relopts, Size relopt_struct_size)
{
	relopts->options = NIL;
	relopts->validators = NIL;
	relopts->relopt_struct_size = relopt_struct_size;
}

/*
 * register_reloptions_validator
 *		注册自定义校验回调，它会在
 *		build_local_reloptions() 结束时被调用。
 */
void
register_reloptions_validator(local_relopts *relopts, relopts_validator validator)
{
	relopts->validators = lappend(relopts->validators, validator);
}

/*
 * add_local_reloption
 *		把已创建的自定义关系选项加入局部列表。
 */
static void
add_local_reloption(local_relopts *relopts, relopt_gen *newoption, int offset)
{
	local_relopt *opt = palloc(sizeof(*opt));

	Assert(offset < relopts->relopt_struct_size);

	opt->option = newoption;
	opt->offset = offset;

	relopts->options = lappend(relopts->options, opt);
}

/*
 * allocate_reloption
 *		分配一个新的关系选项，并初始化与类型无关的字段
 *		（字符串类型除外）
 */
static relopt_gen *
allocate_reloption(bits32 kinds, int type, const char *name, const char *desc,
				   LOCKMODE lockmode)
{
	MemoryContext oldcxt;
	size_t		size;
	relopt_gen *newoption;

	if (kinds != RELOPT_KIND_LOCAL)
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	else
		oldcxt = NULL;

	switch (type)
	{
		case RELOPT_TYPE_BOOL:
			size = sizeof(relopt_bool);
			break;
		case RELOPT_TYPE_INT:
			size = sizeof(relopt_int);
			break;
		case RELOPT_TYPE_REAL:
			size = sizeof(relopt_real);
			break;
		case RELOPT_TYPE_ENUM:
			size = sizeof(relopt_enum);
			break;
		case RELOPT_TYPE_STRING:
			size = sizeof(relopt_string);
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", type);
			return NULL;		/* 让编译器保持安静 */
	}

	newoption = palloc(size);

	newoption->name = pstrdup(name);
	if (desc)
		newoption->desc = pstrdup(desc);
	else
		newoption->desc = NULL;
	newoption->kinds = kinds;
	newoption->namelen = strlen(name);
	newoption->type = type;
	newoption->lockmode = lockmode;

	if (oldcxt != NULL)
		MemoryContextSwitchTo(oldcxt);

	return newoption;
}

/*
 * init_bool_reloption
 *		分配并初始化一个新的布尔关系选项
 */
static relopt_bool *
init_bool_reloption(bits32 kinds, const char *name, const char *desc,
					bool default_val, LOCKMODE lockmode)
{
	relopt_bool *newoption;

	newoption = (relopt_bool *) allocate_reloption(kinds, RELOPT_TYPE_BOOL,
												   name, desc, lockmode);
	newoption->default_val = default_val;

	return newoption;
}

/*
 * add_bool_reloption
 *		增加一个新的布尔关系选项
 */
void
add_bool_reloption(bits32 kinds, const char *name, const char *desc,
				   bool default_val, LOCKMODE lockmode)
{
	relopt_bool *newoption = init_bool_reloption(kinds, name, desc,
												 default_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_bool_reloption
 *		增加一个新的局部布尔关系选项
 *
 * 'offset' 是布尔类型字段的偏移。
 */
void
add_local_bool_reloption(local_relopts *relopts, const char *name,
						 const char *desc, bool default_val, int offset)
{
	relopt_bool *newoption = init_bool_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 default_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}


/*
 * init_real_reloption
 *		分配并初始化一个新的整数关系选项
 */
static relopt_int *
init_int_reloption(bits32 kinds, const char *name, const char *desc,
				   int default_val, int min_val, int max_val,
				   LOCKMODE lockmode)
{
	relopt_int *newoption;

	newoption = (relopt_int *) allocate_reloption(kinds, RELOPT_TYPE_INT,
												  name, desc, lockmode);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	return newoption;
}

/*
 * add_int_reloption
 *		增加一个新的整数关系选项
 */
void
add_int_reloption(bits32 kinds, const char *name, const char *desc, int default_val,
				  int min_val, int max_val, LOCKMODE lockmode)
{
	relopt_int *newoption = init_int_reloption(kinds, name, desc,
											   default_val, min_val,
											   max_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_int_reloption
 *		增加一个新的局部整数关系选项
 *
 * 'offset' 是整数类型字段的偏移。
 */
void
add_local_int_reloption(local_relopts *relopts, const char *name,
						const char *desc, int default_val, int min_val,
						int max_val, int offset)
{
	relopt_int *newoption = init_int_reloption(RELOPT_KIND_LOCAL,
											   name, desc, default_val,
											   min_val, max_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_real_reloption
 *		分配并初始化一个新的实数关系选项
 */
static relopt_real *
init_real_reloption(bits32 kinds, const char *name, const char *desc,
					double default_val, double min_val, double max_val,
					LOCKMODE lockmode)
{
	relopt_real *newoption;

	newoption = (relopt_real *) allocate_reloption(kinds, RELOPT_TYPE_REAL,
												   name, desc, lockmode);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	return newoption;
}

/*
 * add_real_reloption
 *		增加一个新的浮点关系选项
 */
void
add_real_reloption(bits32 kinds, const char *name, const char *desc,
				   double default_val, double min_val, double max_val,
				   LOCKMODE lockmode)
{
	relopt_real *newoption = init_real_reloption(kinds, name, desc,
												 default_val, min_val,
												 max_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_real_reloption
 *		增加一个新的局部浮点关系选项
 *
 * 'offset' 是 double 类型字段的偏移。
 */
void
add_local_real_reloption(local_relopts *relopts, const char *name,
						 const char *desc, double default_val,
						 double min_val, double max_val, int offset)
{
	relopt_real *newoption = init_real_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 default_val, min_val,
												 max_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_enum_reloption
 *		分配并初始化一个新的枚举关系选项
 */
static relopt_enum *
init_enum_reloption(bits32 kinds, const char *name, const char *desc,
					relopt_enum_elt_def *members, int default_val,
					const char *detailmsg, LOCKMODE lockmode)
{
	relopt_enum *newoption;

	newoption = (relopt_enum *) allocate_reloption(kinds, RELOPT_TYPE_ENUM,
												   name, desc, lockmode);
	newoption->members = members;
	newoption->default_val = default_val;
	newoption->detailmsg = detailmsg;

	return newoption;
}


/*
 * add_enum_reloption
 *		增加一个新的枚举关系选项
 *
 * members 数组必须以一个 NULL 项结尾。
 *
 * 传入不支持的值时会显示 detailmsg，其形式为
 *    "Valid values are \"foo\", \"bar\", and \"bar\"."
 *
 * members 数组和 detailmsg 不会被复制——调用者必须确保
 * 它们在进程的整个生命周期内一直有效。
 */
void
add_enum_reloption(bits32 kinds, const char *name, const char *desc,
				   relopt_enum_elt_def *members, int default_val,
				   const char *detailmsg, LOCKMODE lockmode)
{
	relopt_enum *newoption = init_enum_reloption(kinds, name, desc,
												 members, default_val,
												 detailmsg, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_enum_reloption
 *		增加一个新的局部枚举关系选项
 *
 * 'offset' 是整数类型字段的偏移。
 */
void
add_local_enum_reloption(local_relopts *relopts, const char *name,
						 const char *desc, relopt_enum_elt_def *members,
						 int default_val, const char *detailmsg, int offset)
{
	relopt_enum *newoption = init_enum_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 members, default_val,
												 detailmsg, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_string_reloption
 *		分配并初始化一个新的字符串关系选项
 */
static relopt_string *
init_string_reloption(bits32 kinds, const char *name, const char *desc,
					  const char *default_val,
					  validate_string_relopt validator,
					  fill_string_relopt filler,
					  LOCKMODE lockmode)
{
	relopt_string *newoption;

	/* 确保 validator 与默认值的组合是合理的 */
	if (validator)
		(validator) (default_val);

	newoption = (relopt_string *) allocate_reloption(kinds, RELOPT_TYPE_STRING,
													 name, desc, lockmode);
	newoption->validate_cb = validator;
	newoption->fill_cb = filler;
	if (default_val)
	{
		if (kinds == RELOPT_KIND_LOCAL)
			newoption->default_val = strdup(default_val);
		else
			newoption->default_val = MemoryContextStrdup(TopMemoryContext, default_val);
		newoption->default_len = strlen(default_val);
		newoption->default_isnull = false;
	}
	else
	{
		newoption->default_val = "";
		newoption->default_len = 0;
		newoption->default_isnull = true;
	}

	return newoption;
}

/*
 * add_string_reloption
 *		增加一个新的字符串关系选项
 *
 * "validator" 是可选的函数指针，可用于检验
 * 值是否有效。当参数字符串不被该变量接受时，必须
 * elog(ERROR)。注意，默认值必须通过
 * 校验。
 */
void
add_string_reloption(bits32 kinds, const char *name, const char *desc,
					 const char *default_val, validate_string_relopt validator,
					 LOCKMODE lockmode)
{
	relopt_string *newoption = init_string_reloption(kinds, name, desc,
													 default_val,
													 validator, NULL,
													 lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_string_reloption
 *		增加一个新的局部字符串关系选项
 *
 * 'offset' 是整数类型字段的偏移，该字段存放字符串值
 * 在结果 bytea 结构中的偏移。
 */
void
add_local_string_reloption(local_relopts *relopts, const char *name,
						   const char *desc, const char *default_val,
						   validate_string_relopt validator,
						   fill_string_relopt filler, int offset)
{
	relopt_string *newoption = init_string_reloption(RELOPT_KIND_LOCAL,
													 name, desc,
													 default_val,
													 validator, filler,
													 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * 把关系选项列表（DefElem 的列表）转换成保存在
 * pg_class.reloptions 中的文本数组格式，只包含
 * 传入命名空间里的那些选项。输出值不包含
 * 命名空间。
 *
 * 这用于三种情形：CREATE TABLE/INDEX、ALTER TABLE SET，以及
 * ALTER TABLE RESET。在 ALTER 情形中，oldOptions 是现有的
 * reloptions 值（可能为 NULL），我们按需要替换或删除
 * 条目。
 *
 * 若 acceptOidsOff 为真，则允许 oids = false，但为 on 时抛出错误。
 * 这仅仅是为了向后兼容。
 *
 * 注意，这里不负责判断这些选项是否
 * 有效，但会检查所给选项的命名空间是否都
 * 列在 validnsps 中。NULL 命名空间始终有效，不必
 * 显式列出。传入 NULL 指针意味着只有 NULL
 * 命名空间有效。
 *
 * oldOptions 和结果都是文本数组（NULL 表示默认），
 * 但我们把它们声明为 Datum，以避免在 reloptions.h 中包含 array.h。
 */
Datum
transformRelOptions(Datum oldOptions, List *defList, const char *namspace,
					char *validnsps[], bool acceptOidsOff, bool isReset)
{
	Datum		result;
	ArrayBuildState *astate;
	ListCell   *cell;

	/* 空列表则不变 */
	if (defList == NIL)
		return oldOptions;

	/* 用 accumArrayResult 构造新数组 */
	astate = NULL;

	/* 复制所有不会被替换的 oldOptions */
	if (PointerIsValid(DatumGetPointer(oldOptions)))
	{
		ArrayType  *array = DatumGetArrayTypeP(oldOptions);
		Datum	   *oldoptions;
		int			noldoptions;
		int			i;

		deconstruct_array_builtin(array, TEXTOID, &oldoptions, NULL, &noldoptions);

		for (i = 0; i < noldoptions; i++)
		{
			char	   *text_str = VARDATA(oldoptions[i]);
			int			text_len = VARSIZE(oldoptions[i]) - VARHDRSZ;

			/* 在 defList 中查找匹配 */
			foreach(cell, defList)
			{
				DefElem    *def = (DefElem *) lfirst(cell);
				int			kw_len;

				/* 若不在同一命名空间则忽略 */
				if (namspace == NULL)
				{
					if (def->defnamespace != NULL)
						continue;
				}
				else if (def->defnamespace == NULL)
					continue;
				else if (strcmp(def->defnamespace, namspace) != 0)
					continue;

				kw_len = strlen(def->defname);
				if (text_len > kw_len && text_str[kw_len] == '=' &&
					strncmp(text_str, def->defname, kw_len) == 0)
					break;
			}
			if (!cell)
			{
				/* 没有匹配，因此保留旧选项 */
				astate = accumArrayResult(astate, oldoptions[i],
										  false, TEXTOID,
										  CurrentMemoryContext);
			}
		}
	}

	/*
	 * 若是 CREATE/SET，把新选项加入数组；若是 RESET，只检查用户
	 * 没有写成 RESET (option=val)。（必须这样做，因为语法
	 * 并不强制这一点。）
	 */
	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (isReset)
		{
			if (def->arg != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("RESET must not include values for parameters")));
		}
		else
		{
			const char *name;
			const char *value;
			text	   *t;
			Size		len;

			/*
			 * 若命名空间无效则报错。NULL 命名空间
			 * 始终有效。
			 */
			if (def->defnamespace != NULL)
			{
				bool		valid = false;
				int			i;

				if (validnsps)
				{
					for (i = 0; validnsps[i]; i++)
					{
						if (strcmp(def->defnamespace, validnsps[i]) == 0)
						{
							valid = true;
							break;
						}
					}
				}

				if (!valid)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized parameter namespace \"%s\"",
									def->defnamespace)));
			}

			/* 若不在同一命名空间则忽略 */
			if (namspace == NULL)
			{
				if (def->defnamespace != NULL)
					continue;
			}
			else if (def->defnamespace == NULL)
				continue;
			else if (strcmp(def->defnamespace, namspace) != 0)
				continue;

			/*
			 * 把 DefElem 展平成类似 "name=arg" 的文本字符串。若
			 * 只有 "name"，则假定意为 "name=true"。注意：
			 * 不输出命名空间。
			 */
			name = def->defname;
			if (def->arg != NULL)
				value = defGetString(def);
			else
				value = "true";

			/* 坚持 name 不得包含 "="，否则 "a=b=c" 有歧义 */
			if (strchr(name, '=') != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid option name \"%s\": must not contain \"=\"",
								name)));

			/*
			 * 这里并不是做这项检查的好地方，但没有其他
			 * 方便的地方能把该选项滤掉。由于 WITH (oids =
			 * false) 将来会被去掉，这点难看似乎还在
			 * 可接受的范围内。
			 */
			if (acceptOidsOff && def->defnamespace == NULL &&
				strcmp(name, "oids") == 0)
			{
				if (defGetBoolean(def))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("tables declared WITH OIDS are not supported")));
				/* 跳过该选项，关系选项机制不认识它 */
				continue;
			}

			len = VARHDRSZ + strlen(name) + 1 + strlen(value);
			/* +1 为 sprintf 的结尾空字符留出空间 */
			t = (text *) palloc(len + 1);
			SET_VARSIZE(t, len);
			sprintf(VARDATA(t), "%s=%s", name, value);

			astate = accumArrayResult(astate, PointerGetDatum(t),
									  false, TEXTOID,
									  CurrentMemoryContext);
		}
	}

	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;

	return result;
}


/*
 * 把 reloptions 的文本数组格式转换成 DefElem 的 List。
 * 这是 transformRelOptions() 的逆操作。
 */
List *
untransformRelOptions(Datum options)
{
	List	   *result = NIL;
	ArrayType  *array;
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	/* 没有选项则无需处理 */
	if (!PointerIsValid(DatumGetPointer(options)))
		return result;

	array = DatumGetArrayTypeP(options);

	deconstruct_array_builtin(array, TEXTOID, &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *s;
		char	   *p;
		Node	   *val = NULL;

		s = TextDatumGetCString(optiondatums[i]);
		p = strchr(s, '=');
		if (p)
		{
			*p++ = '\0';
			val = (Node *) makeString(p);
		}
		result = lappend(result, makeDefElem(s, val, -1));
	}

	return result;
}

/*
 * 从 pg_class 元组中提取并解析 reloptions。
 *
 * 这是低层例程，预期由 relcache 代码以及
 * 没有表的 relcache 项的调用者使用（例如自动清理）。对于
 * 其他用途，应改为从 relcache 项取得 rd_options 指针
 * 来代替本函数。
 *
 * tupdesc 是 pg_class 的元组描述符。若元组对应一个
 * 索引，amoptions 是该索引访问方法的选项解析函数指针，
 * 否则为 NULL。
 */
bytea *
extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
				  amoptions_function amoptions)
{
	bytea	   *options;
	bool		isnull;
	Datum		datum;
	Form_pg_class classForm;

	datum = fastgetattr(tuple,
						Anum_pg_class_reloptions,
						tupdesc,
						&isnull);
	if (isnull)
		return NULL;

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/* 解析成合适的格式；这里不要报错 */
	switch (classForm->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_MATVIEW:
			options = heap_reloptions(classForm->relkind, datum, false);
			break;
		case RELKIND_PARTITIONED_TABLE:
			options = partitioned_table_reloptions(datum, false);
			break;
		case RELKIND_VIEW:
			options = view_reloptions(datum, false);
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			options = index_reloptions(amoptions, datum, false);
			break;
		case RELKIND_FOREIGN_TABLE:
			options = NULL;
			break;
		default:
			Assert(false);		/* 到不了这里 */
			options = NULL;		/* 让编译器保持安静 */
			break;
	}

	return options;
}

static void
parseRelOptionsInternal(Datum options, bool validate,
						relopt_value *reloptions, int numoptions)
{
	ArrayType  *array = DatumGetArrayTypeP(options);
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	deconstruct_array_builtin(array, TEXTOID, &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *text_str = VARDATA(optiondatums[i]);
		int			text_len = VARSIZE(optiondatums[i]) - VARHDRSZ;
		int			j;

		/* 在 reloptions 中查找匹配 */
		for (j = 0; j < numoptions; j++)
		{
			int			kw_len = reloptions[j].gen->namelen;

			if (text_len > kw_len && text_str[kw_len] == '=' &&
				strncmp(text_str, reloptions[j].gen->name, kw_len) == 0)
			{
				parse_one_reloption(&reloptions[j], text_str, text_len,
									validate);
				break;
			}
		}

		if (j >= numoptions && validate)
		{
			char	   *s;
			char	   *p;

			s = TextDatumGetCString(optiondatums[i]);
			p = strchr(s, '=');
			if (p)
				*p = '\0';
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized parameter \"%s\"", s)));
		}
	}

	/* 这个函数里值得避免内存泄漏 */
	pfree(optiondatums);

	if (((void *) array) != DatumGetPointer(options))
		pfree(array);
}

/*
 * 解释以文本数组格式给出的 reloptions。
 *
 * options 是由 transformRelOptions 构造的关系选项文本数组。
 * kind 指定要处理的选项族。
 *
 * 返回值是 relopt_value * 数组，选项数组中实际
 * 设置的选项标记为 isset=true。该数组的长度
 * 经 *numrelopts 返回。未设置的选项也在该
 * 数组中，以便调用者容易找到默认值。
 *
 * 若给定种类没有任何选项，则把 numrelopts 置为 0 并
 * 返回 NULL（除非在一个都没有定义时仍非法提供了选项，
 * 此时会报错）。
 *
 * 注意：int、bool 和 real 类型的值作为
 * 返回数组的一部分分配。string 类型的值单独分配，必须
 * 由调用者释放。
 */
static relopt_value *
parseRelOptions(Datum options, bool validate, relopt_kind kind,
				int *numrelopts)
{
	relopt_value *reloptions = NULL;
	int			numoptions = 0;
	int			i;
	int			j;

	if (need_initialization)
		initialize_reloptions();

	/* 按 kind 构造预期选项的列表 */

	for (i = 0; relOpts[i]; i++)
		if (relOpts[i]->kinds & kind)
			numoptions++;

	if (numoptions > 0)
	{
		reloptions = palloc(numoptions * sizeof(relopt_value));

		for (i = 0, j = 0; relOpts[i]; i++)
		{
			if (relOpts[i]->kinds & kind)
			{
				reloptions[j].gen = relOpts[i];
				reloptions[j].isset = false;
				j++;
			}
		}
	}

	/* 没有选项则结束 */
	if (PointerIsValid(DatumGetPointer(options)))
		parseRelOptionsInternal(options, validate, reloptions, numoptions);

	*numrelopts = numoptions;
	return reloptions;
}

/* 解析局部未注册选项。 */
static relopt_value *
parseLocalRelOptions(local_relopts *relopts, Datum options, bool validate)
{
	int			nopts = list_length(relopts->options);
	relopt_value *values = palloc(sizeof(*values) * nopts);
	ListCell   *lc;
	int			i = 0;

	foreach(lc, relopts->options)
	{
		local_relopt *opt = lfirst(lc);

		values[i].gen = opt->option;
		values[i].isset = false;

		i++;
	}

	if (options != (Datum) 0)
		parseRelOptionsInternal(options, validate, values, nopts);

	return values;
}

/*
 * parseRelOptions 的子例程，用于解析并校验单个选项的
 * 值
 */
static void
parse_one_reloption(relopt_value *option, char *text_str, int text_len,
					bool validate)
{
	char	   *value;
	int			value_len;
	bool		parsed;
	bool		nofree = false;

	if (option->isset && validate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" specified more than once",
						option->gen->name)));

	value_len = text_len - option->gen->namelen - 1;
	value = (char *) palloc(value_len + 1);
	memcpy(value, text_str + option->gen->namelen + 1, value_len);
	value[value_len] = '\0';

	switch (option->gen->type)
	{
		case RELOPT_TYPE_BOOL:
			{
				parsed = parse_bool(value, &option->values.bool_val);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for boolean option \"%s\": %s",
									option->gen->name, value)));
			}
			break;
		case RELOPT_TYPE_INT:
			{
				relopt_int *optint = (relopt_int *) option->gen;

				parsed = parse_int(value, &option->values.int_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for integer option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.int_val < optint->min ||
								 option->values.int_val > optint->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%d\" and \"%d\".",
									   optint->min, optint->max)));
			}
			break;
		case RELOPT_TYPE_REAL:
			{
				relopt_real *optreal = (relopt_real *) option->gen;

				parsed = parse_real(value, &option->values.real_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for floating point option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.real_val < optreal->min ||
								 option->values.real_val > optreal->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%f\" and \"%f\".",
									   optreal->min, optreal->max)));
			}
			break;
		case RELOPT_TYPE_ENUM:
			{
				relopt_enum *optenum = (relopt_enum *) option->gen;
				relopt_enum_elt_def *elt;

				parsed = false;
				for (elt = optenum->members; elt->string_val; elt++)
				{
					if (pg_strcasecmp(value, elt->string_val) == 0)
					{
						option->values.enum_val = elt->symbol_val;
						parsed = true;
						break;
					}
				}
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for enum option \"%s\": %s",
									option->gen->name, value),
							 optenum->detailmsg ?
							 errdetail_internal("%s", _(optenum->detailmsg)) : 0));

				/*
				 * 若值不在允许的字符串值之中，但并未
				 * 要求我们校验，则直接使用默认的数值。
				 */
				if (!parsed)
					option->values.enum_val = optenum->default_val;
			}
			break;
		case RELOPT_TYPE_STRING:
			{
				relopt_string *optstring = (relopt_string *) option->gen;

				option->values.string_val = value;
				nofree = true;
				if (validate && optstring->validate_cb)
					(optstring->validate_cb) (value);
				parsed = true;
			}
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", option->gen->type);
			parsed = true;		/* 让编译器保持安静 */
			break;
	}

	if (parsed)
		option->isset = true;
	if (!nofree)
		pfree(value);
}

/*
 * 给定 parseRelOptions 的结果，分配一个结构体，其大小为
 * 指定的基础大小，再加上字符串变量所需的额外空间。
 *
 * "base" 应为关系选项结构体的 sizeof(struct)（StdRdOptions 或
 * 与之相当的结构）。
 */
static void *
allocateReloptStruct(Size base, relopt_value *options, int numoptions)
{
	Size		size = base;
	int			i;

	for (i = 0; i < numoptions; i++)
	{
		relopt_value *optval = &options[i];

		if (optval->gen->type == RELOPT_TYPE_STRING)
		{
			relopt_string *optstr = (relopt_string *) optval->gen;

			if (optstr->fill_cb)
			{
				const char *val = optval->isset ? optval->values.string_val :
					optstr->default_isnull ? NULL : optstr->default_val;

				size += optstr->fill_cb(val, NULL);
			}
			else
				size += GET_STRING_RELOPTION_LEN(*optval) + 1;
		}
	}

	return palloc0(size);
}

/*
 * 给定 parseRelOptions 的结果和一张解析表，把解析出的
 * 值填入结构体（先前已用 allocateReloptStruct
 * 分配）。
 *
 * rdopts 是待填充分配结构体的指针。
 * basesize 是传给 allocateReloptStruct 的 sizeof(struct)。
 * options 长度为 numoptions，是 parseRelOptions 的输出。
 * elems 长度为 numelems，是描述允许选项的表。
 * validate 为真时，预期所有选项都出现在 elems 中。
 */
static void
fillRelOptions(void *rdopts, Size basesize,
			   relopt_value *options, int numoptions,
			   bool validate,
			   const relopt_parse_elt *elems, int numelems)
{
	int			i;
	int			offset = basesize;

	for (i = 0; i < numoptions; i++)
	{
		int			j;
		bool		found = false;

		for (j = 0; j < numelems; j++)
		{
			if (strcmp(options[i].gen->name, elems[j].optname) == 0)
			{
				relopt_string *optstring;
				char	   *itempos = ((char *) rdopts) + elems[j].offset;
				char	   *string_val;

				switch (options[i].gen->type)
				{
					case RELOPT_TYPE_BOOL:
						*(bool *) itempos = options[i].isset ?
							options[i].values.bool_val :
							((relopt_bool *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_INT:
						*(int *) itempos = options[i].isset ?
							options[i].values.int_val :
							((relopt_int *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_REAL:
						*(double *) itempos = options[i].isset ?
							options[i].values.real_val :
							((relopt_real *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_ENUM:
						*(int *) itempos = options[i].isset ?
							options[i].values.enum_val :
							((relopt_enum *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_STRING:
						optstring = (relopt_string *) options[i].gen;
						if (options[i].isset)
							string_val = options[i].values.string_val;
						else if (!optstring->default_isnull)
							string_val = optstring->default_val;
						else
							string_val = NULL;

						if (optstring->fill_cb)
						{
							Size		size =
								optstring->fill_cb(string_val,
												   (char *) rdopts + offset);

							if (size)
							{
								*(int *) itempos = offset;
								offset += size;
							}
							else
								*(int *) itempos = 0;
						}
						else if (string_val == NULL)
							*(int *) itempos = 0;
						else
						{
							strcpy((char *) rdopts + offset, string_val);
							*(int *) itempos = offset;
							offset += strlen(string_val) + 1;
						}
						break;
					default:
						elog(ERROR, "unsupported reloption type %d",
							 options[i].gen->type);
						break;
				}
				found = true;
				break;
			}
		}
		if (validate && !found)
			elog(ERROR, "reloption \"%s\" not found in parse table",
				 options[i].gen->name);
	}
	SET_VARSIZE(rdopts, offset);
}


/*
 * 使用 StdRdOptions 的选项解析器。
 */
bytea *
default_reloptions(Datum reloptions, bool validate, relopt_kind kind)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(StdRdOptions, fillfactor)},
		{"autovacuum_enabled", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, enabled)},
		{"autovacuum_vacuum_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_threshold)},
		{"autovacuum_vacuum_insert_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_ins_threshold)},
		{"autovacuum_analyze_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, analyze_threshold)},
		{"autovacuum_vacuum_cost_limit", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_cost_limit)},
		{"autovacuum_freeze_min_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_min_age)},
		{"autovacuum_freeze_max_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_max_age)},
		{"autovacuum_freeze_table_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_table_age)},
		{"autovacuum_multixact_freeze_min_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_min_age)},
		{"autovacuum_multixact_freeze_max_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_max_age)},
		{"autovacuum_multixact_freeze_table_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_table_age)},
		{"log_autovacuum_min_duration", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, log_min_duration)},
		{"toast_tuple_target", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, toast_tuple_target)},
		{"autovacuum_vacuum_cost_delay", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_cost_delay)},
		{"autovacuum_vacuum_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_scale_factor)},
		{"autovacuum_vacuum_insert_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_ins_scale_factor)},
		{"autovacuum_analyze_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, analyze_scale_factor)},
		{"user_catalog_table", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, user_catalog_table)},
		{"parallel_workers", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, parallel_workers)},
		{"vacuum_index_cleanup", RELOPT_TYPE_ENUM,
		offsetof(StdRdOptions, vacuum_index_cleanup)},
		{"vacuum_truncate", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, vacuum_truncate)}
	};

	return (bytea *) build_reloptions(reloptions, validate, kind,
									  sizeof(StdRdOptions),
									  tab, lengthof(tab));
}

/*
 * build_reloptions
 *
 * 解析调用者提供的 "reloptions"，放在一个
 * 含有已解析选项的结构体中返回。解析借助
 * 一张描述允许选项的解析表完成，该表由
 * 长度为 "num_relopt_elems" 的 "relopt_elems" 定义。
 *
 * 若 reloptions 值是刚由 transformRelOptions() 构造的，则 "validate"
 * 必须为真；与此相对，若是从系统目录读出，则其中
 * 所含的值必须已经有效。
 *
 * 若传入的选项与解析表中的任何选项都不匹配，
 * 则返回 NULL，除非 validate 为真，此时会
 * 报告错误。
 */
void *
build_reloptions(Datum reloptions, bool validate,
				 relopt_kind kind,
				 Size relopt_struct_size,
				 const relopt_parse_elt *relopt_elems,
				 int num_relopt_elems)
{
	int			numoptions;
	relopt_value *options;
	void	   *rdopts;

	/* 解析给定关系选项种类所特有的选项 */
	options = parseRelOptions(reloptions, validate, kind, &numoptions);
	Assert(numoptions <= num_relopt_elems);

	/* 若一个都没有，则结束 */
	if (numoptions == 0)
	{
		Assert(options == NULL);
		return NULL;
	}

	/* 分配并填充该结构体 */
	rdopts = allocateReloptStruct(relopt_struct_size, options, numoptions);
	fillRelOptions(rdopts, relopt_struct_size, options, numoptions,
				   validate, relopt_elems, num_relopt_elems);

	pfree(options);

	return rdopts;
}

/*
 * 解析局部选项，分配一个 bytea 结构，大小为指定的
 * 'base_size' 再加上字符串变量所需的额外空间，
 * 按给定偏移填入其选项字段并返回。
 */
void *
build_local_reloptions(local_relopts *relopts, Datum options, bool validate)
{
	int			noptions = list_length(relopts->options);
	relopt_parse_elt *elems = palloc(sizeof(*elems) * noptions);
	relopt_value *vals;
	void	   *opts;
	int			i = 0;
	ListCell   *lc;

	foreach(lc, relopts->options)
	{
		local_relopt *opt = lfirst(lc);

		elems[i].optname = opt->option->name;
		elems[i].opttype = opt->option->type;
		elems[i].offset = opt->offset;

		i++;
	}

	vals = parseLocalRelOptions(relopts, options, validate);
	opts = allocateReloptStruct(relopts->relopt_struct_size, vals, noptions);
	fillRelOptions(opts, relopts->relopt_struct_size, vals, noptions, validate,
				   elems, noptions);

	if (validate)
		foreach(lc, relopts->validators)
			((relopts_validator) lfirst(lc)) (opts, vals, noptions);

	if (elems)
		pfree(elems);

	return opts;
}

/*
 * 分区表的选项解析器
 */
bytea *
partitioned_table_reloptions(Datum reloptions, bool validate)
{
	if (validate && reloptions)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("cannot specify storage parameters for a partitioned table"),
				errhint("Specify storage parameters for its leaf partitions instead."));
	return NULL;
}

/*
 * 视图的选项解析器
 */
bytea *
view_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"security_barrier", RELOPT_TYPE_BOOL,
		offsetof(ViewOptions, security_barrier)},
		{"security_invoker", RELOPT_TYPE_BOOL,
		offsetof(ViewOptions, security_invoker)},
		{"check_option", RELOPT_TYPE_ENUM,
		offsetof(ViewOptions, check_option)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_VIEW,
									  sizeof(ViewOptions),
									  tab, lengthof(tab));
}

/*
 * 解析堆、视图和 TOAST 表的选项。
 */
bytea *
heap_reloptions(char relkind, Datum reloptions, bool validate)
{
	StdRdOptions *rdopts;

	switch (relkind)
	{
		case RELKIND_TOASTVALUE:
			rdopts = (StdRdOptions *)
				default_reloptions(reloptions, validate, RELOPT_KIND_TOAST);
			if (rdopts != NULL)
			{
				/* 调整 TOAST 关系上只取默认值的参数 */
				rdopts->fillfactor = 100;
				rdopts->autovacuum.analyze_threshold = -1;
				rdopts->autovacuum.analyze_scale_factor = -1;
			}
			return (bytea *) rdopts;
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
			return default_reloptions(reloptions, validate, RELOPT_KIND_HEAP);
		default:
			/* 不支持其他 relkind */
			return NULL;
	}
}


/*
 * 解析索引的选项。
 *
 *	amoptions	索引访问方法的选项解析函数
 *	reloptions	作为 text[] datum 的选项
 *	validate	是否报错的标志
 */
bytea *
index_reloptions(amoptions_function amoptions, Datum reloptions, bool validate)
{
	Assert(amoptions != NULL);

	/* 假定该函数是 strict 的 */
	if (!PointerIsValid(DatumGetPointer(reloptions)))
		return NULL;

	return amoptions(reloptions, validate);
}

/*
 * 属性关系选项的解析器
 */
bytea *
attribute_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"n_distinct", RELOPT_TYPE_REAL, offsetof(AttributeOpts, n_distinct)},
		{"n_distinct_inherited", RELOPT_TYPE_REAL, offsetof(AttributeOpts, n_distinct_inherited)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_ATTRIBUTE,
									  sizeof(AttributeOpts),
									  tab, lengthof(tab));
}

/*
 * 表空间关系选项的解析器
 */
bytea *
tablespace_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"random_page_cost", RELOPT_TYPE_REAL, offsetof(TableSpaceOpts, random_page_cost)},
		{"seq_page_cost", RELOPT_TYPE_REAL, offsetof(TableSpaceOpts, seq_page_cost)},
		{"effective_io_concurrency", RELOPT_TYPE_INT, offsetof(TableSpaceOpts, effective_io_concurrency)},
		{"maintenance_io_concurrency", RELOPT_TYPE_INT, offsetof(TableSpaceOpts, maintenance_io_concurrency)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_TABLESPACE,
									  sizeof(TableSpaceOpts),
									  tab, lengthof(tab));
}

/*
 * 根据选项列表确定所需的 LOCKMODE。
 *
 * 由 AlterTableGetLockLevel() 调用，该函数中有
 * 关于其工作方式的更长说明。
 */
LOCKMODE
AlterTableGetRelOptionsLockLevel(List *defList)
{
	LOCKMODE	lockmode = NoLock;
	ListCell   *cell;

	if (defList == NIL)
		return AccessExclusiveLock;

	if (need_initialization)
		initialize_reloptions();

	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);
		int			i;

		for (i = 0; relOpts[i]; i++)
		{
			if (strncmp(relOpts[i]->name,
						def->defname,
						relOpts[i]->namelen + 1) == 0)
			{
				if (lockmode < relOpts[i]->lockmode)
					lockmode = relOpts[i]->lockmode;
			}
		}
	}

	return lockmode;
}
