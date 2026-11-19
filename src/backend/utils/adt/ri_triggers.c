/*-------------------------------------------------------------------------
 *
 * ri_triggers.c
 *
 *	引用完整性约束检查的通用触发器过程。
 *	checks.
 *
 *	内存管理说明：这里保存的私有哈希表会跨查询与
 *	事务边界存活；事实上，它们与后端进程同寿。
 *	之所以可行，是因为这些哈希表结构本身是由
 *	dynahash.c 在其永久 DynaHashCxt 中分配的，
 *	其指向的 SPI 计划则以 SPI_keepplan() 保存。
 *	目前尚无丢弃不再需要的计划之机制 ---
 *	将来不妨改进这一点。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/ri_triggers.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 局部定义
 */

#define RI_MAX_NUMKEYS					INDEX_MAX_KEYS

#define RI_INIT_CONSTRAINTHASHSIZE		64
#define RI_INIT_QUERYHASHSIZE			(RI_INIT_CONSTRAINTHASHSIZE * 4)

#define RI_KEYS_ALL_NULL				0
#define RI_KEYS_SOME_NULL				1
#define RI_KEYS_NONE_NULL				2

/* RI 查询类型码 */
/* 这些查询针对主键（被引用）表执行： */
#define RI_PLAN_CHECK_LOOKUPPK			1
#define RI_PLAN_CHECK_LOOKUPPK_FROM_PK	2
#define RI_PLAN_LAST_ON_PK				RI_PLAN_CHECK_LOOKUPPK_FROM_PK
/* 这些查询针对外键（引用方）表执行： */
#define RI_PLAN_CASCADE_ONDELETE		3
#define RI_PLAN_CASCADE_ONUPDATE		4
/* 对于 RESTRICT，同一计划可同时用于 ON DELETE 与 ON UPDATE 触发器。 */
#define RI_PLAN_RESTRICT				5
#define RI_PLAN_SETNULL_ONDELETE		6
#define RI_PLAN_SETNULL_ONUPDATE		7
#define RI_PLAN_SETDEFAULT_ONDELETE		8
#define RI_PLAN_SETDEFAULT_ONUPDATE		9

#define MAX_QUOTED_NAME_LEN  (NAMEDATALEN*2+3)
#define MAX_QUOTED_REL_NAME_LEN  (MAX_QUOTED_NAME_LEN*2)

#define RIAttName(rel, attnum)	NameStr(*attnumAttName(rel, attnum))
#define RIAttType(rel, attnum)	attnumTypeId(rel, attnum)
#define RIAttCollation(rel, attnum) attnumCollationId(rel, attnum)

#define RI_TRIGTYPE_INSERT 1
#define RI_TRIGTYPE_UPDATE 2
#define RI_TRIGTYPE_DELETE 3


/*
 * RI_ConstraintInfo
 *
 * 从外键 pg_constraint 条目中提取的信息。它被缓存在
 * ri_constraint_cache.
 */
typedef struct RI_ConstraintInfo
{
	Oid			constraint_id;	/* pg_constraint OID（哈希键） */
	bool		valid;			/* 已成功初始化？ */
	Oid			constraint_root_id; /* 最顶层祖先约束的 OID；
									 * 未继承时与 constraint_id 相同 */
	uint32		oidHashValue;	/* constraint_id 哈希值 */
	uint32		rootHashValue;	/* constraint_root_id 哈希值 */
	NameData	conname;		/* 外键约束的名称 */
	Oid			pk_relid;		/* 被引用的关系 */
	Oid			fk_relid;		/* 引用方的关系 */
	char		confupdtype;	/* 外键的 ON UPDATE 动作 */
	char		confdeltype;	/* 外键的 ON DELETE 动作 */
	int			ndelsetcols;	/* ON DELETE 中引用的列数
								 * SET 子句 */
	int16		confdelsetcols[RI_MAX_NUMKEYS]; /* 待删除时设置列的
												 * attnum */
	char		confmatchtype;	/* 外键的匹配类型 */
	int			nkeys;			/* 键列数 */
	int16		pk_attnums[RI_MAX_NUMKEYS]; /* 被引用列的 attnum */
	int16		fk_attnums[RI_MAX_NUMKEYS]; /* 引用方列的 attnum */
	Oid			pf_eq_oprs[RI_MAX_NUMKEYS]; /* 相等算子(主键=外键) */
	Oid			pp_eq_oprs[RI_MAX_NUMKEYS]; /* 相等算子(主键=主键) */
	Oid			ff_eq_oprs[RI_MAX_NUMKEYS]; /* 相等算子(外键=外键) */
	dlist_node	valid_link;		/* 有效条目链表的链接 */
} RI_ConstraintInfo;

/*
 * RI_QueryKey
 *
 * 在查询哈希表中标识一个已准备 SPI 计划的键
 */
typedef struct RI_QueryKey
{
	Oid			constr_id;		/* pg_constraint 条目 OID */
	int32		constr_queryno; /* 查询类型 ID，见 RI_PLAN_XXX */
} RI_QueryKey;

/*
 * RI_QueryHashEntry
 */
typedef struct RI_QueryHashEntry
{
	RI_QueryKey key;
	SPIPlanPtr	plan;
} RI_QueryHashEntry;

/*
 * RI_CompareKey
 *
 * 标识一个说明如何比较两个值的条目的键
 */
typedef struct RI_CompareKey
{
	Oid			eq_opr;			/* 要应用的相等算子 */
	Oid			typeid;			/* 应用它的数据类型 */
} RI_CompareKey;

/*
 * RI_CompareHashEntry
 */
typedef struct RI_CompareHashEntry
{
	RI_CompareKey key;
	bool		valid;			/* 已成功初始化？ */
	FmgrInfo	eq_opr_finfo;	/* 相等函数调用信息 */
	FmgrInfo	cast_func_finfo;	/* 以备需强制转换输入 */
} RI_CompareHashEntry;


/*
 * Local data
 */
static HTAB *ri_constraint_cache = NULL;
static HTAB *ri_query_cache = NULL;
static HTAB *ri_compare_cache = NULL;
static dclist_head ri_constraint_cache_valid_list;


/*
 * 局部函数前置声明
 */
static bool ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
							  TupleTableSlot *oldslot,
							  const RI_ConstraintInfo *riinfo);
static Datum ri_restrict(TriggerData *trigdata, bool is_no_action);
static Datum ri_set(TriggerData *trigdata, bool is_set_null, int tgkind);
static void quoteOneName(char *buffer, const char *name);
static void quoteRelationName(char *buffer, Relation rel);
static void ri_GenerateQual(StringInfo buf,
							const char *sep,
							const char *leftop, Oid leftoptype,
							Oid opoid,
							const char *rightop, Oid rightoptype);
static void ri_GenerateQualCollation(StringInfo buf, Oid collation);
static int	ri_NullCheck(TupleDesc tupDesc, TupleTableSlot *slot,
						 const RI_ConstraintInfo *riinfo, bool rel_is_pk);
static void ri_BuildQueryKey(RI_QueryKey *key,
							 const RI_ConstraintInfo *riinfo,
							 int32 constr_queryno);
static bool ri_KeysEqual(Relation rel, TupleTableSlot *oldslot, TupleTableSlot *newslot,
						 const RI_ConstraintInfo *riinfo, bool rel_is_pk);
static bool ri_AttributesEqual(Oid eq_opr, Oid typeid,
							   Datum oldvalue, Datum newvalue);

static void ri_InitHashTables(void);
static void InvalidateConstraintCacheCallBack(Datum arg, int cacheid, uint32 hashvalue);
static SPIPlanPtr ri_FetchPreparedPlan(RI_QueryKey *key);
static void ri_HashPreparedPlan(RI_QueryKey *key, SPIPlanPtr plan);
static RI_CompareHashEntry *ri_HashCompareOp(Oid eq_opr, Oid typeid);

static void ri_CheckTrigger(FunctionCallInfo fcinfo, const char *funcname,
							int tgkind);
static const RI_ConstraintInfo *ri_FetchConstraintInfo(Trigger *trigger,
													   Relation trig_rel, bool rel_is_pk);
static const RI_ConstraintInfo *ri_LoadConstraintInfo(Oid constraintOid);
static Oid	get_ri_constraint_root(Oid constrOid);
static SPIPlanPtr ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
							   RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel);
static bool ri_PerformCheck(const RI_ConstraintInfo *riinfo,
							RI_QueryKey *qkey, SPIPlanPtr qplan,
							Relation fk_rel, Relation pk_rel,
							TupleTableSlot *oldslot, TupleTableSlot *newslot,
							bool detectNewRows, int expect_OK);
static void ri_ExtractValues(Relation rel, TupleTableSlot *slot,
							 const RI_ConstraintInfo *riinfo, bool rel_is_pk,
							 Datum *vals, char *nulls);
static void ri_ReportViolation(const RI_ConstraintInfo *riinfo,
							   Relation pk_rel, Relation fk_rel,
							   TupleTableSlot *violatorslot, TupleDesc tupdesc,
							   int queryno, bool partgone) pg_attribute_noreturn();


/*
 * RI_FKey_check -
 *
 * 检查外键存在性（INSERT 与 UPDATE 合并处理）。
 */
static Datum
RI_FKey_check(TriggerData *trigdata)
{
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *newslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, false);

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		newslot = trigdata->tg_newslot;
	else
		newslot = trigdata->tg_trigslot;

	/*
	 * 若该行已不再有效，我们根本不应考虑对它做检查，
	 * 因为它要么已被删除（此时应跳过延迟检查），
	 * 要么已被更新（此时只应检查该行的最新版本）。
	 * 依据 SnapshotSelf 检验其存活性。要调用
	 * HeapTupleSatisfiesVisibility 需要缓冲区上的 pin 与锁。
	 * 调用者持 pin，不持锁。
	 */
	if (!table_tuple_satisfies_snapshot(trigdata->tg_relation, newslot, SnapshotSelf))
		return PointerGetDatum(NULL);

	/*
	 * 获取外键表与主键表的关系描述符。
	 *
	 * pk_rel 以 RowShareLock 模式打开，因为我们最终的
	 * SELECT FOR KEY SHARE 所需。
	 */
	fk_rel = trigdata->tg_relation;
	pk_rel = table_open(riinfo->pk_relid, RowShareLock);

	switch (ri_NullCheck(RelationGetDescr(fk_rel), newslot, riinfo, false))
	{
		case RI_KEYS_ALL_NULL:

			/*
			 * 无需再做检查 - 全 NULL 键可通过任何类型的
			 * 外键约束。
			 */
			table_close(pk_rel, RowShareLock);
			return PointerGetDatum(NULL);

		case RI_KEYS_SOME_NULL:

			/*
			 * 这是三种 MATCH 之间唯一不同的情形。
			 * MATCH.
			 */
			switch (riinfo->confmatchtype)
			{
				case FKCONSTR_MATCH_FULL:

					/*
					 * 不允许 - MATCH FULL 要求要么全为 NULL，
					 * 要么全不为 NULL
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
							 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
									RelationGetRelationName(fk_rel),
									NameStr(riinfo->conname)),
							 errdetail("MATCH FULL does not allow mixing of null and nonnull key values."),
							 errtableconstraint(fk_rel,
												NameStr(riinfo->conname))));
					table_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case FKCONSTR_MATCH_SIMPLE:

					/*
					 * MATCH SIMPLE - 若任一列为 NULL，键即通过
					 * 该约束。
					 */
					table_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

#ifdef NOT_USED
				case FKCONSTR_MATCH_PARTIAL:

					/*
					 * MATCH PARTIAL - 全部非空列必须匹配。（
					 * 尚未实现；可改写下面的查询只纳入
					 * 非空列，或者在此处编写一个
					 * 特殊版本）
					 */
					break;
#endif
			}

		case RI_KEYS_NONE_NULL:

			/*
			 * 已有完整限定的键 - 下面对三种 MATCH 继续
			 * 处理。
			 */
			break;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/* 获取或准备真正检查用的已存计划 */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CHECK_LOOKUPPK);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *pk_only;

		/* ----------
		 * 构建的查询为
		 *	SELECT 1 FROM [ONLY] <pktable> x WHERE pkatt1 = $1 [AND ...]
		 *		   FOR KEY SHARE OF x
		 * $ 参数的类型 ID 即对应外键属性
		 * 的类型。
		 * ----------
		 */
		initStringInfo(&querybuf);
		pk_only = pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(pkrelname, pk_rel);
		appendStringInfo(&querybuf, "SELECT 1 FROM %s%s x",
						 pk_only, pkrelname);
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(pk_rel, riinfo->pk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							attname, pk_type,
							riinfo->pf_eq_oprs[i],
							paramname, fk_type);
			querysep = "AND";
			queryoids[i] = fk_type;
		}
		appendStringInfoString(&querybuf, " FOR KEY SHARE OF x");

		/* 准备并保存该计划 */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * 现在检查外键在主键表中是否存在
	 *
	 * XXX 当被引用侧为分区表时 detectNewRows 必须为真。
	 * 原因是我们的快照必须是新鲜的，这样
	 * find_inheritance_children() 中的 hack 才能工作。
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					NULL, newslot,
					pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE,
					SPI_OK_SELECT);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(pk_rel, RowShareLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_check_ins -
 *
 * 外键表插入事件时检查外键存在性。
 */
Datum
RI_FKey_check_ins(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_check_ins", RI_TRIGTYPE_INSERT);

	/* 与 UPDATE 共用代码 */
	return RI_FKey_check((TriggerData *) fcinfo->context);
}


/*
 * RI_FKey_check_upd -
 *
 * 外键表更新事件时检查外键存在性。
 */
Datum
RI_FKey_check_upd(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_check_upd", RI_TRIGTYPE_UPDATE);

	/* 与 INSERT 共用代码 */
	return RI_FKey_check((TriggerData *) fcinfo->context);
}


/*
 * ri_Check_Pk_Match
 *
 * 检查是否有其他主键行已被创建，能提供与
 * 触发事件中被修改或删除的 "oldslot" 相同的键值。
 * 若在主键表中找到匹配项则返回真。
 *
 * 我们假定调用者已检查过 oldslot 不含 NULL 键值，
 * 否则不可能有匹配。
 */
static bool
ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
				  TupleTableSlot *oldslot,
				  const RI_ConstraintInfo *riinfo)
{
	SPIPlanPtr	qplan;
	RI_QueryKey qkey;
	bool		result;

	/* 只为非空行调用 */
	Assert(ri_NullCheck(RelationGetDescr(pk_rel), oldslot, riinfo, true) == RI_KEYS_NONE_NULL);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * 获取或准备一个已保存的计划，以主键行的值
	 * 查主键表
	 */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CHECK_LOOKUPPK_FROM_PK);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		const char *pk_only;
		Oid			queryoids[RI_MAX_NUMKEYS];

		/* ----------
		 * 构建的查询为
		 *	SELECT 1 FROM [ONLY] <pktable> x WHERE pkatt1 = $1 [AND ...]
		 *		   FOR KEY SHARE OF x
		 * $ 参数的类型 ID 即主键属性本身
		 * 的类型。
		 * ----------
		 */
		initStringInfo(&querybuf);
		pk_only = pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(pkrelname, pk_rel);
		appendStringInfo(&querybuf, "SELECT 1 FROM %s%s x",
						 pk_only, pkrelname);
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(pk_rel, riinfo->pk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							attname, pk_type,
							riinfo->pp_eq_oprs[i],
							paramname, pk_type);
			querysep = "AND";
			queryoids[i] = pk_type;
		}
		appendStringInfoString(&querybuf, " FOR KEY SHARE OF x");

		/* 准备并保存该计划 */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * 已拿到计划，运行。
	 */
	result = ri_PerformCheck(riinfo, &qkey, qplan,
							 fk_rel, pk_rel,
							 oldslot, NULL,
							 true,	/* 按更新处理 */
							 SPI_OK_SELECT);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return result;
}


/*
 * RI_FKey_noaction_del -
 *
 * 若删除已导致违反给定的引用
 * 完整性约束，则报错并回滚
 * 当前事务。
 */
Datum
RI_FKey_noaction_del(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_del", RI_TRIGTYPE_DELETE);

	/* 与 RESTRICT/UPDATE 情形共用代码 */
	return ri_restrict((TriggerData *) fcinfo->context, true);
}

/*
 * RI_FKey_restrict_del -
 *
 * 将主键表删除限制在未被外键引用的行上。
 *
 * SQL 标准的意图是这种引用动作恰好发生在删除时，
 * 而非之后。这似乎是 "NO ACTION" 与 "RESTRICT"
 * 之间的唯一差别。在 Postgres 中，
 * 我们仍将其实现为 AFTER 触发器，但不可延迟。
 */
Datum
RI_FKey_restrict_del(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_del", RI_TRIGTYPE_DELETE);

	/* 与 NO ACTION/UPDATE 情形共用代码 */
	return ri_restrict((TriggerData *) fcinfo->context, false);
}

/*
 * RI_FKey_noaction_upd -
 *
 * 若更新已导致违反给定的引用
 * 完整性约束，则报错并回滚
 * 当前事务。
 */
Datum
RI_FKey_noaction_upd(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_upd", RI_TRIGTYPE_UPDATE);

	/* 与 RESTRICT/DELETE 情形共用代码 */
	return ri_restrict((TriggerData *) fcinfo->context, true);
}

/*
 * RI_FKey_restrict_upd -
 *
 * 将主键更新限制在未被外键引用的行上。
 *
 * SQL 标准的意图是这种引用动作恰好发生在更新时，
 * 而非之后。这似乎是 "NO ACTION" 与 "RESTRICT"
 * 之间的唯一差别。在 Postgres 中，
 * 我们仍将其实现为 AFTER 触发器，但不可延迟。
 */
Datum
RI_FKey_restrict_upd(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_upd", RI_TRIGTYPE_UPDATE);

	/* 与 NO ACTION/DELETE 情形共用代码 */
	return ri_restrict((TriggerData *) fcinfo->context, false);
}

/*
 * ri_restrict -
 *
 * 公共代码：ON DELETE RESTRICT、ON DELETE NO ACTION
 * ON UPDATE RESTRICT, ON UPDATE NO ACTION。
 */
static Datum
ri_restrict(TriggerData *trigdata, bool is_no_action)
{
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * 获取外键表与主键表的关系描述符以及旧元组。
	 *
	 * fk_rel 以 RowShareLock 模式打开，因为我们最终的
	 * SELECT FOR KEY SHARE 所需。
	 */
	fk_rel = table_open(riinfo->fk_relid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	oldslot = trigdata->tg_trigslot;

	/*
	 * 如果现在存在另一个提供旧键值的主键行，我们
	 * 就不应做任何事情。不过，该检查只应在 NO
	 * ACTION 情形下进行；在 RESTRICT 情形下我们不
	 * 愿替换。
	 */
	if (is_no_action &&
		ri_Check_Pk_Match(pk_rel, fk_rel, oldslot, riinfo))
	{
		table_close(fk_rel, RowShareLock);
		return PointerGetDatum(NULL);
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * 获取或准备用于限制查找的已保存计划（删除与
	 * 更新情形的查询相同）
	 */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_RESTRICT);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *fk_only;

		/* ----------
		 * 构建的查询为
		 *	SELECT 1 FROM [ONLY] <fktable> x WHERE $1 = fkatt1 [AND ...]
		 *		   FOR KEY SHARE OF x
		 * $ 参数的类型 ID 即对应主键属性
		 * 的类型。
		 * ----------
		 */
		initStringInfo(&querybuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "SELECT 1 FROM %s%s x",
						 fk_only, fkrelname);
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
			Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			if (pk_coll != fk_coll && !get_collation_isdeterministic(pk_coll))
				ri_GenerateQualCollation(&querybuf, pk_coll);
			querysep = "AND";
			queryoids[i] = pk_type;
		}
		appendStringInfoString(&querybuf, " FOR KEY SHARE OF x");

		/* 准备并保存该计划 */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * 现已获得计划。运行它以检查现存引用。
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, NULL,
					true,		/* 须探测新行 */
					SPI_OK_SELECT);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowShareLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_cascade_del -
 *
 * 在主键表的删除事件上级联删除外键引用。
 */
Datum
RI_FKey_cascade_del(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_del", RI_TRIGTYPE_DELETE);

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * 获取外键表与主键表的关系描述符以及旧元组。
	 *
	 * fk_rel 以 RowExclusiveLock 模式打开，因为我们的
	 * 最终 DELETE 所需。
	 */
	fk_rel = table_open(riinfo->fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	oldslot = trigdata->tg_trigslot;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/* 获取或准备级联删除用的已保存计划 */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CASCADE_ONDELETE);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *fk_only;

		/* ----------
		 * 构建的查询为
		 *	DELETE FROM [ONLY] <fktable> WHERE $1 = fkatt1 [AND ...]
		 * $ 参数的类型 ID 即对应主键属性
		 * 的类型。
		 * ----------
		 */
		initStringInfo(&querybuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "DELETE FROM %s%s",
						 fk_only, fkrelname);
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
			Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			if (pk_coll != fk_coll && !get_collation_isdeterministic(pk_coll))
				ri_GenerateQualCollation(&querybuf, pk_coll);
			querysep = "AND";
			queryoids[i] = pk_type;
		}

		/* 准备并保存该计划 */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * 现已获得计划。根据被删除主键元组中的键值
	 * 构建参数并删除引用方行
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, NULL,
					true,		/* 须探测新行 */
					SPI_OK_DELETE);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowExclusiveLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_cascade_upd -
 *
 * 在主键表的更新事件上级联更新外键引用。
 */
Datum
RI_FKey_cascade_upd(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *newslot;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_upd", RI_TRIGTYPE_UPDATE);

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * 获取外键表与主键表的关系描述符以及新与
	 * 旧元组
	 *
	 * fk_rel 以 RowExclusiveLock 模式打开，因为我们的
	 * 最终 UPDATE 所需。
	 */
	fk_rel = table_open(riinfo->fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	newslot = trigdata->tg_newslot;
	oldslot = trigdata->tg_trigslot;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/* 获取或准备级联更新用的已保存计划 */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CASCADE_ONUPDATE);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		StringInfoData qualbuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		const char *qualsep;
		Oid			queryoids[RI_MAX_NUMKEYS * 2];
		const char *fk_only;

		/* ----------
		 * 构建的查询为
		 *	UPDATE [ONLY] <fktable> SET fkatt1 = $1 [, ...]
		 *			WHERE $n = fkatt1 [AND ...]
		 * $ 参数的类型 ID 即对应主键属性
		 * 的类型。注意我们假定存在从主键类型到
		 * 外键类型的赋值转换；
		 * 否则解析器失败。
		 * ----------
		 */
		initStringInfo(&querybuf);
		initStringInfo(&qualbuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "UPDATE %s%s SET",
						 fk_only, fkrelname);
		querysep = "";
		qualsep = "WHERE";
		for (int i = 0, j = riinfo->nkeys; i < riinfo->nkeys; i++, j++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
			Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));
			appendStringInfo(&querybuf,
							 "%s %s = $%d",
							 querysep, attname, i + 1);
			sprintf(paramname, "$%d", j + 1);
			ri_GenerateQual(&qualbuf, qualsep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			if (pk_coll != fk_coll && !get_collation_isdeterministic(pk_coll))
				ri_GenerateQualCollation(&querybuf, pk_coll);
			querysep = ",";
			qualsep = "AND";
			queryoids[i] = pk_type;
			queryoids[j] = pk_type;
		}
		appendBinaryStringInfo(&querybuf, qualbuf.data, qualbuf.len);

		/* 准备并保存该计划 */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys * 2, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * 现已获得计划。运行它以更新现存引用。
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, newslot,
					true,		/* 须探测新行 */
					SPI_OK_UPDATE);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowExclusiveLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_setnull_del -
 *
 * 在主键表的删除事件上将外键引用设为 NULL。
 */
Datum
RI_FKey_setnull_del(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_del", RI_TRIGTYPE_DELETE);

	/* 与 UPDATE 情形共用 */
	return ri_set((TriggerData *) fcinfo->context, true, RI_TRIGTYPE_DELETE);
}

/*
 * RI_FKey_setnull_upd -
 *
 * 在主键表的更新事件上将外键引用设为 NULL。
 */
Datum
RI_FKey_setnull_upd(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_upd", RI_TRIGTYPE_UPDATE);

	/* 与 DELETE 情形共用 */
	return ri_set((TriggerData *) fcinfo->context, true, RI_TRIGTYPE_UPDATE);
}

/*
 * RI_FKey_setdefault_del -
 *
 * 在主键表的删除事件上将外键引用设为默认值。
 */
Datum
RI_FKey_setdefault_del(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_del", RI_TRIGTYPE_DELETE);

	/* 与 UPDATE 情形共用 */
	return ri_set((TriggerData *) fcinfo->context, false, RI_TRIGTYPE_DELETE);
}

/*
 * RI_FKey_setdefault_upd -
 *
 * 在主键表的更新事件上将外键引用设为默认值。
 */
Datum
RI_FKey_setdefault_upd(PG_FUNCTION_ARGS)
{
	/* 检查这是在正确时机与事件上的有效触发器调用。 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_upd", RI_TRIGTYPE_UPDATE);

	/* 与 DELETE 情形共用 */
	return ri_set((TriggerData *) fcinfo->context, false, RI_TRIGTYPE_UPDATE);
}

/*
 * ri_set -
 *
 * 公共代码：ON DELETE SET NULL、ON DELETE SET DEFAULT、
 * ON UPDATE SET NULL/DEFAULT。
 */
static Datum
ri_set(TriggerData *trigdata, bool is_set_null, int tgkind)
{
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int32		queryno;

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * 获取外键表与主键表的关系描述符以及旧元组。
	 *
	 * fk_rel 以 RowExclusiveLock 模式打开，因为我们的
	 * 最终 UPDATE 所需。
	 */
	fk_rel = table_open(riinfo->fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	oldslot = trigdata->tg_trigslot;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * 获取或准备触发器已保存计划。
	 */
	switch (tgkind)
	{
		case RI_TRIGTYPE_UPDATE:
			queryno = is_set_null
				? RI_PLAN_SETNULL_ONUPDATE
				: RI_PLAN_SETDEFAULT_ONUPDATE;
			break;
		case RI_TRIGTYPE_DELETE:
			queryno = is_set_null
				? RI_PLAN_SETNULL_ONDELETE
				: RI_PLAN_SETDEFAULT_ONDELETE;
			break;
		default:
			elog(ERROR, "invalid tgkind passed to ri_set");
	}

	ri_BuildQueryKey(&qkey, riinfo, queryno);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		const char *qualsep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *fk_only;
		int			num_cols_to_set;
		const int16 *set_cols;

		switch (tgkind)
		{
			case RI_TRIGTYPE_UPDATE:
				num_cols_to_set = riinfo->nkeys;
				set_cols = riinfo->fk_attnums;
				break;
			case RI_TRIGTYPE_DELETE:

				/*
				 * 若存在 confdelsetcols，则只更新该数组中
				 * 指定的列，否则更新全部引用
				 * 方列。
				 */
				if (riinfo->ndelsetcols != 0)
				{
					num_cols_to_set = riinfo->ndelsetcols;
					set_cols = riinfo->confdelsetcols;
				}
				else
				{
					num_cols_to_set = riinfo->nkeys;
					set_cols = riinfo->fk_attnums;
				}
				break;
			default:
				elog(ERROR, "invalid tgkind passed to ri_set");
		}

		/* ----------
		 * 构建的查询为
		 *	UPDATE [ONLY] <fktable> SET fkatt1 = {NULL|DEFAULT} [, ...]
		 *			WHERE $1 = fkatt1 [AND ...]
		 * $ 参数的类型 ID 即对应主键属性
		 * 的类型。
		 * ----------
		 */
		initStringInfo(&querybuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "UPDATE %s%s SET",
						 fk_only, fkrelname);

		/*
		 * 追加赋值子句
		 */
		querysep = "";
		for (int i = 0; i < num_cols_to_set; i++)
		{
			quoteOneName(attname, RIAttName(fk_rel, set_cols[i]));
			appendStringInfo(&querybuf,
							 "%s %s = %s",
							 querysep, attname,
							 is_set_null ? "NULL" : "DEFAULT");
			querysep = ",";
		}

		/*
		 * 补 WHERE 子句
		 */
		qualsep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
			Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));

			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, qualsep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			if (pk_coll != fk_coll && !get_collation_isdeterministic(pk_coll))
				ri_GenerateQualCollation(&querybuf, pk_coll);
			qualsep = "AND";
			queryoids[i] = pk_type;
		}

		/* 准备并保存该计划 */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * 现已获得计划。运行它以更新现存引用。
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, NULL,
					true,		/* 须探测新行 */
					SPI_OK_UPDATE);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowExclusiveLock);

	if (is_set_null)
		return PointerGetDatum(NULL);
	else
	{
		/*
		 * 如果我们刚删除或更新了其键值等于外键列默认
		 * 值的主键行，且外键表中存在一个引用行，那么
		 * 我们会把该行更新为与它已有值相同的值 ---
		 * 因此 RI_FKey_fk_upd_check_required 会认为
		 * 无需检查。所以现在需要再查一次，若引用仍
		 * 然存在，则中止该操作。这一点已
		 * 经在 NO ACTION 触发器中实现，所以直接运行它。
		 * （此复查仅在 SET DEFAULT 情形下需要，因为
		 * CASCADE 会在删除操作时移除此类行，或在更新
		 * 操作时改变外键键值，而 SET NULL 则必定得到
		 * 满足外键约束的行。）
		 */
		return ri_restrict(trigdata, true);
	}
}


/*
 * RI_FKey_pk_upd_check_required -
 *
 * 检查对主键关系的更新或删除是否确需触发 RI 触发器。
 * 该函数由 AFTER 触发器队列管理器调用，用于判断
 * 是否可以跳过某个 RI 触发器实例的排队。若必须
 * 触发该触发器则返回真；若能证明约束仍将被满足
 * 则返回假
 *
 * 若是因删除而调用，则 newslot 为 NULL。
 */
bool
RI_FKey_pk_upd_check_required(Trigger *trigger, Relation pk_rel,
							  TupleTableSlot *oldslot, TupleTableSlot *newslot)
{
	const RI_ConstraintInfo *riinfo;

	riinfo = ri_FetchConstraintInfo(trigger, pk_rel, true);

	/*
	 * 若任何旧键值为 NULL，该行不可能被外键行引用，
	 * 因此无需检查。
	 */
	if (ri_NullCheck(RelationGetDescr(pk_rel), oldslot, riinfo, true) != RI_KEYS_NONE_NULL)
		return false;

	/* 若全部新旧键值都相等，则无需检查 */
	if (newslot && ri_KeysEqual(pk_rel, oldslot, newslot, riinfo, true))
		return false;

	/* 否则需要触发该触发器。 */
	return true;
}

/*
 * RI_FKey_fk_upd_check_required -
 *
 * 检查对外键关系的更新是否确需触发 RI 触发器。
 * 该函数由 AFTER 触发器队列管理器调用，用于判断
 * 是否可以跳过某个 RI 触发器实例的排队。若必须
 * 触发该触发器则返回真；若能证明约束仍将被满足
 * 则返回假
 */
bool
RI_FKey_fk_upd_check_required(Trigger *trigger, Relation fk_rel,
							  TupleTableSlot *oldslot, TupleTableSlot *newslot)
{
	const RI_ConstraintInfo *riinfo;
	int			ri_nullcheck;
	Datum		xminDatum;
	TransactionId xmin;
	bool		isnull;

	/*
	 * AfterTriggerSaveEvent() 的处理确保本函数永远不会
	 * 针对分区表被调用。
	 */
	Assert(fk_rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE);

	riinfo = ri_FetchConstraintInfo(trigger, fk_rel, false);

	ri_nullcheck = ri_NullCheck(RelationGetDescr(fk_rel), newslot, riinfo, false);

	/*
	 * 若所有新键值均为 NULL，该行满足约束，因此无
	 * 需检查。
	 */
	if (ri_nullcheck == RI_KEYS_ALL_NULL)
		return false;

	/*
	 * 若部分新键值为 NULL，则行为取决于匹配类型。
	 * type.
	 */
	else if (ri_nullcheck == RI_KEYS_SOME_NULL)
	{
		switch (riinfo->confmatchtype)
		{
			case FKCONSTR_MATCH_SIMPLE:

				/*
				 * 若有新键值为 NULL，该行必须满足约束，
				 * 因此无需检查。
				 */
				return false;

			case FKCONSTR_MATCH_PARTIAL:

				/*
				 * 未知，须做完整检查。
				 */
				break;

			case FKCONSTR_MATCH_FULL:

				/*
				 * 若部分新键值为 NULL，该行违反约束，
				 * 不能在此处报错，因为该行可能在待检
				 * 查之前失效，但应将事件排队以便稍后
				 * 应用该检查。
				 * later.
				 */
				return true;
		}
	}

	/*
	 * 若无新键值为 NULL 或尚无法确定，则从此处继续。
	 * yet.
	 */

	/*
	 * 若原始行是由我们自己的事务插入的，则无论
	 * 键值是否相等都必须触发该触发器。这是因为
	 * 我们的 UPDATE 会使 INSERT 失效，以致 INSERT
	 * RI 触发器将不做任何事；所以最好执行 UPDATE
	 * 检查。（若已知 INSERT 触发器已触发则可跳过，
	 * 但难知晓。）
	 */
	xminDatum = slot_getsysattr(oldslot, MinTransactionIdAttributeNumber, &isnull);
	Assert(!isnull);
	xmin = DatumGetTransactionId(xminDatum);
	if (TransactionIdIsCurrentTransactionId(xmin))
		return true;

	/* 若全部新旧键值都相等，则无需检查 */
	if (ri_KeysEqual(fk_rel, oldslot, newslot, riinfo, false))
		return false;

	/* 否则需要触发该触发器。 */
	return true;
}

/*
 * RI_Initial_Check -
 *
 * 用单个查询检查整张表是否有不匹配的值。
 * 这不是触发器过程，而是在 ALTER TABLE ADD
 * FOREIGN KEY 期间调用，以验证表初始内容。
 *
 * 我们预期调用者已采取措施防止并发操作导致
 * 的问题。这可以通过以 ShareRowExclusiveLock
 * 或更高锁级别锁定 rel 与 pkrel，或以其他方式
 * 确保实现检查的触发器已处于激活状态。
 * 因此，我们无需为检查锁定单个行。
 *
 * 如果检查失败是因为当前用户没有读取这两张
 * 表的权限，则返回假，让调用者知道它们需要
 * 用其他办法来检查该约束。
 */
bool
RI_Initial_Check(Trigger *trigger, Relation fk_rel, Relation pk_rel)
{
	const RI_ConstraintInfo *riinfo;
	StringInfoData querybuf;
	char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		pkattname[MAX_QUOTED_NAME_LEN + 3];
	char		fkattname[MAX_QUOTED_NAME_LEN + 3];
	RangeTblEntry *rte;
	RTEPermissionInfo *pk_perminfo;
	RTEPermissionInfo *fk_perminfo;
	List	   *rtes = NIL;
	List	   *perminfos = NIL;
	const char *sep;
	const char *fk_only;
	const char *pk_only;
	int			save_nestlevel;
	char		workmembuf[32];
	int			spi_result;
	SPIPlanPtr	qplan;

	riinfo = ri_FetchConstraintInfo(trigger, fk_rel, false);

	/*
	 * 检查当前用户是否具有执行该测试查询所需的
	 * 足够权限。（如果没有，调用者可以退回到触
	 * 发器方法，因其会即时切换 ID。）
	 *
	 * XXX 还有其他需要检查的拦路条件吗？
	 */
	pk_perminfo = makeNode(RTEPermissionInfo);
	pk_perminfo->relid = RelationGetRelid(pk_rel);
	pk_perminfo->requiredPerms = ACL_SELECT;
	perminfos = lappend(perminfos, pk_perminfo);
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(pk_rel);
	rte->relkind = pk_rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;
	rte->perminfoindex = list_length(perminfos);
	rtes = lappend(rtes, rte);

	fk_perminfo = makeNode(RTEPermissionInfo);
	fk_perminfo->relid = RelationGetRelid(fk_rel);
	fk_perminfo->requiredPerms = ACL_SELECT;
	perminfos = lappend(perminfos, fk_perminfo);
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(fk_rel);
	rte->relkind = fk_rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;
	rte->perminfoindex = list_length(perminfos);
	rtes = lappend(rtes, rte);

	for (int i = 0; i < riinfo->nkeys; i++)
	{
		int			attno;

		attno = riinfo->pk_attnums[i] - FirstLowInvalidHeapAttributeNumber;
		pk_perminfo->selectedCols = bms_add_member(pk_perminfo->selectedCols, attno);

		attno = riinfo->fk_attnums[i] - FirstLowInvalidHeapAttributeNumber;
		fk_perminfo->selectedCols = bms_add_member(fk_perminfo->selectedCols, attno);
	}

	if (!ExecCheckPermissions(rtes, perminfos, false))
		return false;

	/*
	 * 若任一表启用了 RLS，除非该角色具有 bypassrls
	 * 权限或为相关已启用 RLS 表的属主，否则我们
	 * 也退回。
	 */
	if (!has_bypassrls_privilege(GetUserId()) &&
		((pk_rel->rd_rel->relrowsecurity &&
		  !object_ownercheck(RelationRelationId, RelationGetRelid(pk_rel),
							 GetUserId())) ||
		 (fk_rel->rd_rel->relrowsecurity &&
		  !object_ownercheck(RelationRelationId, RelationGetRelid(fk_rel),
							 GetUserId()))))
		return false;

	/*----------
	 * 构建的查询为：
	 *	SELECT fk.keycols FROM [ONLY] relname fk
	 *	 LEFT OUTER JOIN [ONLY] pkrelname pk
	 *	 ON (pk.pkkeycol1=fk.keycol1 [AND ...])
	 *	 WHERE pk.pkkeycol1 IS NULL AND
	 * For MATCH SIMPLE:
	 *	 (fk.keycol1 IS NOT NULL [AND ...])
	 * For MATCH FULL:
	 *	 (fk.keycol1 IS NOT NULL [OR ...])
	 *
	 * 当比较具有不同排序规则的列时，我们会给算子
	 * 附上 COLLATE 子句。
	 *----------
	 */
	initStringInfo(&querybuf);
	appendStringInfoString(&querybuf, "SELECT ");
	sep = "";
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf, "%sfk.%s", sep, fkattname);
		sep = ", ";
	}

	quoteRelationName(pkrelname, pk_rel);
	quoteRelationName(fkrelname, fk_rel);
	fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
		"" : "ONLY ";
	pk_only = pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
		"" : "ONLY ";
	appendStringInfo(&querybuf,
					 " FROM %s%s fk LEFT OUTER JOIN %s%s pk ON",
					 fk_only, fkrelname, pk_only, pkrelname);

	strcpy(pkattname, "pk.");
	strcpy(fkattname, "fk.");
	sep = "(";
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
		Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

		quoteOneName(pkattname + 3,
					 RIAttName(pk_rel, riinfo->pk_attnums[i]));
		quoteOneName(fkattname + 3,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		ri_GenerateQual(&querybuf, sep,
						pkattname, pk_type,
						riinfo->pf_eq_oprs[i],
						fkattname, fk_type);
		if (pk_coll != fk_coll)
			ri_GenerateQualCollation(&querybuf, pk_coll);
		sep = "AND";
	}

	/*
	 * 只要测试任一主键属性为 NULL 即可发现连接失败。
	 * failure.
	 */
	quoteOneName(pkattname, RIAttName(pk_rel, riinfo->pk_attnums[0]));
	appendStringInfo(&querybuf, ") WHERE pk.%s IS NULL AND (", pkattname);

	sep = "";
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname, RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf,
						 "%sfk.%s IS NOT NULL",
						 sep, fkattname);
		switch (riinfo->confmatchtype)
		{
			case FKCONSTR_MATCH_SIMPLE:
				sep = " AND ";
				break;
			case FKCONSTR_MATCH_FULL:
				sep = " OR ";
				break;
		}
	}
	appendStringInfoChar(&querybuf, ')');

	/*
	 * 临时提高 work_mem，使检查查询能更高效地执行。
	 * 这样做似乎没问题，因为该查询足够简单，不会
	 * 使用 work_mem 的倍数，而且通常不会有多个大型
	 * 外键验证并发发生。因此这似乎符合被视作
	 * "维护"操作的标准，因此我们使用
	 * maintenance_work_mem。但是，我们还必须把
	 * hash_mem_multiplier 设为 1，因为它肯定不该
	 * 被应用到 maintenance_work_mem 值上。
	 *
	 * 我们使用等效于函数 SET 选项的方式来让该设置
	 * 恰好在检查查询的持续期间内保持有效。guc.c
	 * 也会在出错时撤销它。
	 */
	save_nestlevel = NewGUCNestLevel();

	snprintf(workmembuf, sizeof(workmembuf), "%d", maintenance_work_mem);
	(void) set_config_option("work_mem", workmembuf,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("hash_mem_multiplier", "1",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * 生成计划。我们不需要缓存它，而且该计划没有
	 * 参数。
	 */
	qplan = SPI_prepare(querybuf.data, 0, NULL);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %s for %s",
			 SPI_result_code_string(SPI_result), querybuf.data);

	/*
	 * 运行计划。为安全起见，我们强制使用当前快
	 * 照。（在事务快照模式下，这可以说违反了事
	 * 务隔离规则，但我们确实没有太多选择。）我
	 * 们不需要注册快照，因为 SPI_execute_snapshot
	 * 会负责此事。至多一个元组，传 limit=1
	 */
	spi_result = SPI_execute_snapshot(qplan,
									  NULL, NULL,
									  GetLatestSnapshot(),
									  InvalidSnapshot,
									  true, false, 1);

	/* 检查结果 */
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_snapshot returned %s", SPI_result_code_string(spi_result));

	/* 我们是否找到了违反约束的元组？ */
	if (SPI_processed > 0)
	{
		TupleTableSlot *slot;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		RI_ConstraintInfo fake_riinfo;

		slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

		heap_deform_tuple(tuple, tupdesc,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		/*
		 * 结果元组中要查看的列是 1..N，而不是它们在
		 * fk_rel 中的编号。我们要改动 riinfo，使此处
		 * 调用的子程序行为正确。
		 *
		 * 除此之外，我们还必须把正确的 tupdesc 传给
		 * ri_ReportViolation，改变其惯用 pk_rel 或
		 * fk_rel tupdesc。
		 */
		memcpy(&fake_riinfo, riinfo, sizeof(RI_ConstraintInfo));
		for (int i = 0; i < fake_riinfo.nkeys; i++)
			fake_riinfo.fk_attnums[i] = i + 1;

		/*
		 * 若是 MATCH FULL 且外键键中有任何 NULL，
		 * 则应就该情况投诉，而非缺少匹配。MATCH FULL
		 * 不允许部分 NULL 的行。
		 */
		if (fake_riinfo.confmatchtype == FKCONSTR_MATCH_FULL &&
			ri_NullCheck(tupdesc, slot, &fake_riinfo, false) != RI_KEYS_NONE_NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
					 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
							RelationGetRelationName(fk_rel),
							NameStr(fake_riinfo.conname)),
					 errdetail("MATCH FULL does not allow mixing of null and nonnull key values."),
					 errtableconstraint(fk_rel,
										NameStr(fake_riinfo.conname))));

		/*
		 * 我们告诉 ri_ReportViolation 我们在做
		 * RI_PLAN_CHECK_LOOKUPPK 查询，并不属实，
		 * 用 fake_riinfo.fk_attnums 即可
		 */
		ri_ReportViolation(&fake_riinfo,
						   pk_rel, fk_rel,
						   slot, tupdesc,
						   RI_PLAN_CHECK_LOOKUPPK, false);

		ExecDropSingleTupleTableSlot(slot);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/*
	 * 恢复 work_mem、hash_mem_multiplier。
	 */
	AtEOXact_GUC(true, save_nestlevel);

	return true;
}

/*
 * RI_PartitionRemove_Check -
 *
 * 当分区从外键约束的被引用侧分离时，验证不
 * 存在引用方值。
 */
void
RI_PartitionRemove_Check(Trigger *trigger, Relation fk_rel, Relation pk_rel)
{
	const RI_ConstraintInfo *riinfo;
	StringInfoData querybuf;
	char	   *constraintDef;
	char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		pkattname[MAX_QUOTED_NAME_LEN + 3];
	char		fkattname[MAX_QUOTED_NAME_LEN + 3];
	const char *sep;
	const char *fk_only;
	int			save_nestlevel;
	char		workmembuf[32];
	int			spi_result;
	SPIPlanPtr	qplan;
	int			i;

	riinfo = ri_FetchConstraintInfo(trigger, fk_rel, false);

	/*
	 * 在显示错误消息之前我们不检查权限，依据是
	 * 分离分区的用户反正必须拥有足够的权限来
	 * 查看表内容。
	 */

	/*----------
	 * 构建的查询为：
	 *  SELECT fk.keycols FROM [ONLY] relname fk
	 *    JOIN pkrelname pk
	 *    ON (pk.pkkeycol1=fk.keycol1 [AND ...])
	 *    WHERE (<partition constraint>) AND
	 * For MATCH SIMPLE:
	 *   (fk.keycol1 IS NOT NULL [AND ...])
	 * For MATCH FULL:
	 *   (fk.keycol1 IS NOT NULL [OR ...])
	 *
	 * 当比较具有不同排序规则的列时，我们会给算子
	 * 附上 COLLATE 子句。
	 *----------
	 */
	initStringInfo(&querybuf);
	appendStringInfoString(&querybuf, "SELECT ");
	sep = "";
	for (i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf, "%sfk.%s", sep, fkattname);
		sep = ", ";
	}

	quoteRelationName(pkrelname, pk_rel);
	quoteRelationName(fkrelname, fk_rel);
	fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
		"" : "ONLY ";
	appendStringInfo(&querybuf,
					 " FROM %s%s fk JOIN %s pk ON",
					 fk_only, fkrelname, pkrelname);
	strcpy(pkattname, "pk.");
	strcpy(fkattname, "fk.");
	sep = "(";
	for (i = 0; i < riinfo->nkeys; i++)
	{
		Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
		Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

		quoteOneName(pkattname + 3,
					 RIAttName(pk_rel, riinfo->pk_attnums[i]));
		quoteOneName(fkattname + 3,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		ri_GenerateQual(&querybuf, sep,
						pkattname, pk_type,
						riinfo->pf_eq_oprs[i],
						fkattname, fk_type);
		if (pk_coll != fk_coll)
			ri_GenerateQualCollation(&querybuf, pk_coll);
		sep = "AND";
	}

	/*
	 * 以分区约束作为 WHERE 子句的开头（除非这是
	 * 默认分区且没有其他分区，因为此时的分区
	 * 约束为空字符串。）
	 */
	constraintDef = pg_get_partconstrdef_string(RelationGetRelid(pk_rel), "pk");
	if (constraintDef && constraintDef[0] != '\0')
		appendStringInfo(&querybuf, ") WHERE %s AND (",
						 constraintDef);
	else
		appendStringInfoString(&querybuf, ") WHERE (");

	sep = "";
	for (i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname, RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf,
						 "%sfk.%s IS NOT NULL",
						 sep, fkattname);
		switch (riinfo->confmatchtype)
		{
			case FKCONSTR_MATCH_SIMPLE:
				sep = " AND ";
				break;
			case FKCONSTR_MATCH_FULL:
				sep = " OR ";
				break;
		}
	}
	appendStringInfoChar(&querybuf, ')');

	/*
	 * 临时提高 work_mem，使检查查询能更高效地执行。
	 * 这样做似乎没问题，因为该查询足够简单，不会
	 * 使用 work_mem 的倍数，而且通常不会有多个大型
	 * 外键验证并发发生。因此这似乎符合被视作
	 * "维护"操作的标准，因此我们使用
	 * maintenance_work_mem。但是，我们还必须把
	 * hash_mem_multiplier 设为 1，因为它肯定不该
	 * 被应用到 maintenance_work_mem 值上。
	 *
	 * 我们使用等效于函数 SET 选项的方式来让该设置
	 * 恰好在检查查询的持续期间内保持有效。guc.c
	 * 也会在出错时撤销它。
	 */
	save_nestlevel = NewGUCNestLevel();

	snprintf(workmembuf, sizeof(workmembuf), "%d", maintenance_work_mem);
	(void) set_config_option("work_mem", workmembuf,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("hash_mem_multiplier", "1",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * 生成计划。我们不需要缓存它，而且该计划没有
	 * 参数。
	 */
	qplan = SPI_prepare(querybuf.data, 0, NULL);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %s for %s",
			 SPI_result_code_string(SPI_result), querybuf.data);

	/*
	 * 运行计划。为安全起见，我们强制使用当前快
	 * 照。（在事务快照模式下，这可以说违反了事
	 * 务隔离规则，但我们确实没有太多选择。）我
	 * 们不需要注册快照，因为 SPI_execute_snapshot
	 * 会负责此事。至多一个元组，传 limit=1
	 */
	spi_result = SPI_execute_snapshot(qplan,
									  NULL, NULL,
									  GetLatestSnapshot(),
									  InvalidSnapshot,
									  true, false, 1);

	/* 检查结果 */
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_snapshot returned %s", SPI_result_code_string(spi_result));

	/* 我们是否找到了将违反约束的元组？ */
	if (SPI_processed > 0)
	{
		TupleTableSlot *slot;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		RI_ConstraintInfo fake_riinfo;

		slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

		heap_deform_tuple(tuple, tupdesc,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		/*
		 * 结果元组中要查看的列是 1..N，而不是它们在
		 * fk_rel 中的编号。改动 riinfo，使 ri_ReportViolation
		 * 正常工作。
		 *
		 * 除此之外，我们还必须把正确的 tupdesc 传给
		 * ri_ReportViolation，改变其惯用 pk_rel 或
		 * fk_rel tupdesc。
		 */
		memcpy(&fake_riinfo, riinfo, sizeof(RI_ConstraintInfo));
		for (i = 0; i < fake_riinfo.nkeys; i++)
			fake_riinfo.pk_attnums[i] = i + 1;

		ri_ReportViolation(&fake_riinfo, pk_rel, fk_rel,
						   slot, tupdesc, 0, true);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/*
	 * 恢复 work_mem、hash_mem_multiplier。
	 */
	AtEOXact_GUC(true, save_nestlevel);
}


/* ----------
 * 局部函数如下
 * ----------
 */


/*
 * quoteOneName --- 安全引用单个 SQL 名称
 *
 * buffer 长度必须为 MAX_QUOTED_NAME_LEN（含 \0 空间）
 */
static void
quoteOneName(char *buffer, const char *name)
{
	/* 与其自作聪明，不如一律加引号。 */
	*buffer++ = '"';
	while (*name)
	{
		if (*name == '"')
			*buffer++ = '"';
		*buffer++ = *name++;
	}
	*buffer++ = '"';
	*buffer = '\0';
}

/*
 * quoteRelationName --- 安全引用完整限定的关系名
 *
 * buffer 长度必须为 MAX_QUOTED_REL_NAME_LEN（含 \0 空间）
 */
static void
quoteRelationName(char *buffer, Relation rel)
{
	quoteOneName(buffer, get_namespace_name(RelationGetNamespace(rel)));
	buffer += strlen(buffer);
	*buffer++ = '.';
	quoteOneName(buffer, RelationGetRelationName(rel));
}

/*
 * ri_GenerateQual --- 生成使两个变量相等的 WHERE 子句
 *
 * 它基本上是向 buf 追加 " sep leftop op rightop"，
 * 并按需添加类型转换与模式限定，以确保解析器会
 * 选择我们指定的算子。若 leftop 与 rightop 不是
 * 变量或参数则应加括号。
 */
static void
ri_GenerateQual(StringInfo buf,
				const char *sep,
				const char *leftop, Oid leftoptype,
				Oid opoid,
				const char *rightop, Oid rightoptype)
{
	appendStringInfo(buf, " %s ", sep);
	generate_operator_clause(buf, leftop, leftoptype, opoid,
							 rightop, rightoptype);
}

/*
 * ri_GenerateQualCollation --- 给 WHERE 子句加 COLLATE 规格
 *
 * 目前，对于把变量与 $n 参数做比较的 RI 查询，
 * 我们有意不使用本函数。由于参数符号始终具有
 * 默认排序规则，其效果将是使用变量的排序规则。
 * 只有在测试被引用列时这才严格正确，因为 SQL
 * 标准规定 RI 比较应使用被引用列的排序规则。
 * 不过，只要所有排序规则对相等性的理解相同
 * （事实如此，因为 texteq 归结为按位相等），
 * 那么在测试引用方列时使用引用方列的排序规则
 * 就不会有可见的语义影响，而且这样做是好事，
 * 因为它让我们能在引用方列上使用普通索引。
 * 但是，当直接比较引用方列与被引用列时，如果
 * 它们的排序规则不同，则必须使用本函数；否则
 * 无法解决应应用的排序规则。
 */
static void
ri_GenerateQualCollation(StringInfo buf, Oid collation)
{
	HeapTuple	tp;
	Form_pg_collation colltup;
	char	   *collname;
	char		onename[MAX_QUOTED_NAME_LEN];

	/* 若数据类型不可排序则无事可做 */
	if (!OidIsValid(collation))
		return;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for collation %u", collation);
	colltup = (Form_pg_collation) GETSTRUCT(tp);
	collname = NameStr(colltup->collname);

	/*
	 * 出于简单以及确保查询不依赖搜索路径的考虑，
	 * 总是限定名称。
	 */
	quoteOneName(onename, get_namespace_name(colltup->collnamespace));
	appendStringInfo(buf, " COLLATE %s", onename);
	quoteOneName(onename, collname);
	appendStringInfo(buf, ".%s", onename);

	ReleaseSysCache(tp);
}

/* ----------
 * ri_BuildQueryKey -
 *
 *	为外键约束的已准备 SPI 计划构建哈希表键。
 *
 *		key：输出参数，*key 依据其余参数填入
 *		riinfo：取自 pg_constraint 条目的信息
 *		constr_queryno：标识查询类型的内部编号
 *			（见文件头 RI_PLAN_XXX 常量）
 * ----------
 */
static void
ri_BuildQueryKey(RI_QueryKey *key, const RI_ConstraintInfo *riinfo,
				 int32 constr_queryno)
{
	/*
	 * 带共同祖先的继承约束可以共享 ri_query_cache
	 * 条目，RI_PLAN_CHECK_LOOKUPPK_FROM_PK 除外。
	 * 除该情形外，查询处理的是外键约束涉及的另一
	 * 张表（即触发器并非在被触发的表上），因此对
	 * 继承树的所有成员而言查询都相同。所以我们可
	 * 以在哈希键中使用根约束的 OID，而非约束自身
	 * 的 OID。这避免了创建重复的 SPI 计划，当存在
	 * 许多具有相似外键约束的分区时，可节省大量工
	 * 作与内存。
	 *
	 * （注意每个约束仍必须有各自的 RI_ConstraintInfo，
	 * 因为分区可能具有不同的列顺序，从而导致
	 * pk_attnums[] 或 fk_attnums[] 数组内容不同。）
	 *
	 * 我们假定结构体 RI_QueryKey 不含填充字节，否则
	 * 需 memset 清除它们。
	 */
	if (constr_queryno != RI_PLAN_CHECK_LOOKUPPK_FROM_PK)
		key->constr_id = riinfo->constraint_root_id;
	else
		key->constr_id = riinfo->constraint_id;
	key->constr_queryno = constr_queryno;
}

/*
 * 检查 RI 触发器函数是否在预期上下文中被调用
 */
static void
ri_CheckTrigger(FunctionCallInfo fcinfo, const char *funcname, int tgkind)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager", funcname)));

	/*
	 * 核查事件正当
	 */
	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired AFTER ROW", funcname)));

	switch (tgkind)
	{
		case RI_TRIGTYPE_INSERT:
			if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("function \"%s\" must be fired for INSERT", funcname)));
			break;
		case RI_TRIGTYPE_UPDATE:
			if (!TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("function \"%s\" must be fired for UPDATE", funcname)));
			break;
		case RI_TRIGTYPE_DELETE:
			if (!TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("function \"%s\" must be fired for DELETE", funcname)));
			break;
	}
}


/*
 * 获取触发器外键约束的 RI_ConstraintInfo 结构体。
 */
static const RI_ConstraintInfo *
ri_FetchConstraintInfo(Trigger *trigger, Relation trig_rel, bool rel_is_pk)
{
	Oid			constraintOid = trigger->tgconstraint;
	const RI_ConstraintInfo *riinfo;

	/*
	 * 检查外键约束的 OID 是否可用；如果我们经由
	 * 普通触发器或旧式 "约束触发器" 被调用，它可能
	 * 没有。
	 */
	if (!OidIsValid(constraintOid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("no pg_constraint entry for trigger \"%s\" on table \"%s\"",
						trigger->tgname, RelationGetRelationName(trig_rel)),
				 errhint("Remove this referential integrity trigger and its mates, then do ALTER TABLE ADD CONSTRAINT.")));

	/* 为该约束查找或创建哈希表条目 */
	riinfo = ri_LoadConstraintInfo(constraintOid);

	/* 对触发器调用数据做一些简单的交叉检查 */
	if (rel_is_pk)
	{
		if (riinfo->fk_relid != trigger->tgconstrrelid ||
			riinfo->pk_relid != RelationGetRelid(trig_rel))
			elog(ERROR, "wrong pg_constraint entry for trigger \"%s\" on table \"%s\"",
				 trigger->tgname, RelationGetRelationName(trig_rel));
	}
	else
	{
		if (riinfo->fk_relid != RelationGetRelid(trig_rel) ||
			riinfo->pk_relid != trigger->tgconstrrelid)
			elog(ERROR, "wrong pg_constraint entry for trigger \"%s\" on table \"%s\"",
				 trigger->tgname, RelationGetRelationName(trig_rel));
	}

	if (riinfo->confmatchtype != FKCONSTR_MATCH_FULL &&
		riinfo->confmatchtype != FKCONSTR_MATCH_PARTIAL &&
		riinfo->confmatchtype != FKCONSTR_MATCH_SIMPLE)
		elog(ERROR, "unrecognized confmatchtype: %d",
			 riinfo->confmatchtype);

	if (riinfo->confmatchtype == FKCONSTR_MATCH_PARTIAL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MATCH PARTIAL not yet implemented")));

	return riinfo;
}

/*
 * 获取或创建外键约束的 RI_ConstraintInfo 结构体。
 */
static const RI_ConstraintInfo *
ri_LoadConstraintInfo(Oid constraintOid)
{
	RI_ConstraintInfo *riinfo;
	bool		found;
	HeapTuple	tup;
	Form_pg_constraint conForm;

	/*
	 * 首次调用时初始化哈希表
	 */
	if (!ri_constraint_cache)
		ri_InitHashTables();

	/*
	 * 查找或创建哈希条目。若找到有效条目直接返回它
	 */
	riinfo = (RI_ConstraintInfo *) hash_search(ri_constraint_cache,
											   &constraintOid,
											   HASH_ENTER, &found);
	if (!found)
		riinfo->valid = false;
	else if (riinfo->valid)
		return riinfo;

	/*
	 * 获取 pg_constraint 行，以便填充该条目。
	 */
	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintOid));
	if (!HeapTupleIsValid(tup)) /* 不应发生 */
		elog(ERROR, "cache lookup failed for constraint %u", constraintOid);
	conForm = (Form_pg_constraint) GETSTRUCT(tup);

	if (conForm->contype != CONSTRAINT_FOREIGN) /* 不应发生 */
		elog(ERROR, "constraint %u is not a foreign key constraint",
			 constraintOid);

	/* 并提取数据 */
	Assert(riinfo->constraint_id == constraintOid);
	if (OidIsValid(conForm->conparentid))
		riinfo->constraint_root_id =
			get_ri_constraint_root(conForm->conparentid);
	else
		riinfo->constraint_root_id = constraintOid;
	riinfo->oidHashValue = GetSysCacheHashValue1(CONSTROID,
												 ObjectIdGetDatum(constraintOid));
	riinfo->rootHashValue = GetSysCacheHashValue1(CONSTROID,
												  ObjectIdGetDatum(riinfo->constraint_root_id));
	memcpy(&riinfo->conname, &conForm->conname, sizeof(NameData));
	riinfo->pk_relid = conForm->confrelid;
	riinfo->fk_relid = conForm->conrelid;
	riinfo->confupdtype = conForm->confupdtype;
	riinfo->confdeltype = conForm->confdeltype;
	riinfo->confmatchtype = conForm->confmatchtype;

	DeconstructFkConstraintRow(tup,
							   &riinfo->nkeys,
							   riinfo->fk_attnums,
							   riinfo->pk_attnums,
							   riinfo->pf_eq_oprs,
							   riinfo->pp_eq_oprs,
							   riinfo->ff_eq_oprs,
							   &riinfo->ndelsetcols,
							   riinfo->confdelsetcols);

	ReleaseSysCache(tup);

	/*
	 * 为了在下方高效处理失效消息，我们把所有当前
	 * 有效的条目保存在一个双链计数列表中。
	 */
	dclist_push_tail(&ri_constraint_cache_valid_list, &riinfo->valid_link);

	riinfo->valid = true;

	return riinfo;
}

/*
 * get_ri_constraint_root
 *		返回该约束根父约束的 OID
 */
static Oid
get_ri_constraint_root(Oid constrOid)
{
	for (;;)
	{
		HeapTuple	tuple;
		Oid			constrParentOid;

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrParentOid = ((Form_pg_constraint) GETSTRUCT(tuple))->conparentid;
		ReleaseSysCache(tuple);
		if (!OidIsValid(constrParentOid))
			break;				/* 已到达根约束 */
		constrOid = constrParentOid;
	}
	return constrOid;
}

/*
 * pg_constraint 失效事件的回调
 *
 * 虽然大多数 syscache 回调只是刷掉自己的全部条目，
 * 但 pg_constraint 的更新流量很大，可能值得做得
 * 更聪明一些。使 ri_constraint_cache 中与
 * 指定哈希值关联的条目失效；若为 0 则全失效。
 *
 * 注意：处理缓存失效消息时，缓存上可能存在活动
 * 引用。正因如此，我们绝不将条目移出缓存，而
 * 只是将其标记为无效，这对活动使用者是无害的。
 * （任何使用某条目的查询都应持有足够的锁，以防
 * 数据在其下变化 --- 但缓存仍可能被刷新。）
 */
static void
InvalidateConstraintCacheCallBack(Datum arg, int cacheid, uint32 hashvalue)
{
	dlist_mutable_iter iter;

	Assert(ri_constraint_cache != NULL);

	/*
	 * 如果当前有效条目列表变得过大，我们将把它们全
	 * 部标记为无效，以便清空该列表。这种安排避免了
	 * 在会话触碰许多外键又执行许多 ALTER TABLE 的
	 * 情形下（如 pg_dump 恢复）出现 O(N^2) 行为。
	 */
	if (dclist_count(&ri_constraint_cache_valid_list) > 1000)
		hashvalue = 0;			/* 假装这是缓存重置 */

	dclist_foreach_modify(iter, &ri_constraint_cache_valid_list)
	{
		RI_ConstraintInfo *riinfo = dclist_container(RI_ConstraintInfo,
													 valid_link, iter.cur);

		/*
		 * 不仅须使直接匹配给定哈希值的条目失效，
		 * 还须使子条目失效，以防失效涉及
		 * 的是根约束。
		 */
		if (hashvalue == 0 ||
			riinfo->oidHashValue == hashvalue ||
			riinfo->rootHashValue == hashvalue)
		{
			riinfo->valid = false;
			/* 同时从列表中移除已失效条目 */
			dclist_delete_from(&ri_constraint_cache_valid_list, iter.cur);
		}
	}
}


/*
 * 为强制执行 RI 限制的查询准备执行计划
 */
static SPIPlanPtr
ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
			 RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel)
{
	SPIPlanPtr	qplan;
	Relation	query_rel;
	Oid			save_userid;
	int			save_sec_context;

	/*
	 * 用查询类型代码判断查询是针对主键表还是外键
	 * 表运行；我们将以该表的属主身份执行检查
	 */
	if (qkey->constr_queryno <= RI_PLAN_LAST_ON_PK)
		query_rel = pk_rel;
	else
		query_rel = fk_rel;

	/* 切换到执行检查用的正确 UID */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(RelationGetForm(query_rel)->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_NOFORCE_RLS);

	/* 创建计划 */
	qplan = SPI_prepare(querystr, nargs, argtypes);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %s for %s", SPI_result_code_string(SPI_result), querystr);

	/* 恢复 UID 与安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* 保存计划 */
	SPI_keepplan(qplan);
	ri_HashPreparedPlan(qkey, qplan);

	return qplan;
}

/*
 * 执行强制 RI 限制的查询
 */
static bool
ri_PerformCheck(const RI_ConstraintInfo *riinfo,
				RI_QueryKey *qkey, SPIPlanPtr qplan,
				Relation fk_rel, Relation pk_rel,
				TupleTableSlot *oldslot, TupleTableSlot *newslot,
				bool detectNewRows, int expect_OK)
{
	Relation	query_rel,
				source_rel;
	bool		source_is_pk;
	Snapshot	test_snapshot;
	Snapshot	crosscheck_snapshot;
	int			limit;
	int			spi_result;
	Oid			save_userid;
	int			save_sec_context;
	Datum		vals[RI_MAX_NUMKEYS * 2];
	char		nulls[RI_MAX_NUMKEYS * 2];

	/*
	 * 用查询类型代码判断查询是针对主键表还是外键
	 * 表运行；我们将以该表的属主身份执行检查
	 */
	if (qkey->constr_queryno <= RI_PLAN_LAST_ON_PK)
		query_rel = pk_rel;
	else
		query_rel = fk_rel;

	/*
	 * 查询所用的值取自触发器被调用的那张表 - 它通
	 * 常是相对于 query_rel 的另一张表。一个例外是
	 * ri_Check_Pk_Match()，它对两者都使用主键表（并
	 * 把 queryno 设为 RI_PLAN_CHECK_LOOKUPPK_FROM_PK）。
	 * 或许终需更优雅的判定方式。
	 */
	if (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK)
	{
		source_rel = fk_rel;
		source_is_pk = false;
	}
	else
	{
		source_rel = pk_rel;
		source_is_pk = true;
	}

	/* 提取要传入查询的参数 */
	if (newslot)
	{
		ri_ExtractValues(source_rel, newslot, riinfo, source_is_pk,
						 vals, nulls);
		if (oldslot)
			ri_ExtractValues(source_rel, oldslot, riinfo, source_is_pk,
							 vals + riinfo->nkeys, nulls + riinfo->nkeys);
	}
	else
	{
		ri_ExtractValues(source_rel, oldslot, riinfo, source_is_pk,
						 vals, nulls);
	}

	/*
	 * 在 READ COMMITTED 模式下，我们只需使用最新的
	 * 常规快照即可看到所有可能有用的行。但在事务
	 * 快照模式下我们无法更改事务快照。如果调用者
	 * 传入 detectNewRows == false，那么用事务快照执
	 * 行查询是可以的；否则我们使用当前快照，并告
	 * 知执行器：若在当前快照下发现按事务快照不可
	 * 见的行就报错。注意
	 * SPI_execute_snapshot 会负责注册快照，因此我们
	 * 无需费心。
	 */
	if (IsolationUsesXactSnapshot() && detectNewRows)
	{
		CommandCounterIncrement();	/* 确保自身工作全部可见 */
		test_snapshot = GetLatestSnapshot();
		crosscheck_snapshot = GetTransactionSnapshot();
	}
	else
	{
		/* 默认的 SPI 行为即可 */
		test_snapshot = InvalidSnapshot;
		crosscheck_snapshot = InvalidSnapshot;
	}

	/*
	 * 如果这是选择查询（例如用于 'no action' 或
	 * 'restrict' 触发器），我们只需查看表中是否有
	 * 一行与键匹配。否则 limit = 0 - 因为我们希望
	 * 影响全部匹配的行。
	 */
	limit = (expect_OK == SPI_OK_SELECT) ? 1 : 0;

	/* 切换到执行检查用的正确 UID */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(RelationGetForm(query_rel)->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_NOFORCE_RLS);

	/* 现在可运行该查询。 */
	spi_result = SPI_execute_snapshot(qplan,
									  vals, nulls,
									  test_snapshot, crosscheck_snapshot,
									  false, false, limit);

	/* 恢复 UID 与安全上下文 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* 检查结果 */
	if (spi_result < 0)
		elog(ERROR, "SPI_execute_snapshot returned %s", SPI_result_code_string(spi_result));

	if (expect_OK >= 0 && spi_result != expect_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("referential integrity query on \"%s\" from constraint \"%s\" on \"%s\" gave unexpected result",
						RelationGetRelationName(pk_rel),
						NameStr(riinfo->conname),
						RelationGetRelationName(fk_rel)),
				 errhint("This is most likely due to a rule having rewritten the query.")));

	/* XXX 这部分放在调用者处做会不会更清晰？ */
	if (qkey->constr_queryno != RI_PLAN_CHECK_LOOKUPPK_FROM_PK &&
		expect_OK == SPI_OK_SELECT &&
		(SPI_processed == 0) == (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK))
		ri_ReportViolation(riinfo,
						   pk_rel, fk_rel,
						   newslot ? newslot : oldslot,
						   NULL,
						   qkey->constr_queryno, false);

	return SPI_processed != 0;
}

/*
 * 从元组中提取字段到 Datum/nulls 数组
 */
static void
ri_ExtractValues(Relation rel, TupleTableSlot *slot,
				 const RI_ConstraintInfo *riinfo, bool rel_is_pk,
				 Datum *vals, char *nulls)
{
	const int16 *attnums;
	bool		isnull;

	if (rel_is_pk)
		attnums = riinfo->pk_attnums;
	else
		attnums = riinfo->fk_attnums;

	for (int i = 0; i < riinfo->nkeys; i++)
	{
		vals[i] = slot_getattr(slot, attnums[i], &isnull);
		nulls[i] = isnull ? 'n' : ' ';
	}
}

/*
 * 生成错误报告
 *
 * 若失败的约束在外键表插入/更新时触发，
 * 我们希望键名与键值从该表提取，且错误消息呈
 * 现为 'key blah is not present in PK' 样式。
 * 否则，属性名与值来自主键表，消息呈现为
 * 'key blah is still referenced from FK' 样式。
 */
static void
ri_ReportViolation(const RI_ConstraintInfo *riinfo,
				   Relation pk_rel, Relation fk_rel,
				   TupleTableSlot *violatorslot, TupleDesc tupdesc,
				   int queryno, bool partgone)
{
	StringInfoData key_names;
	StringInfoData key_values;
	bool		onfk;
	const int16 *attnums;
	Oid			rel_oid;
	AclResult	aclresult;
	bool		has_perm = true;

	/*
	 * 确定要投诉哪个关系。如果调用者未传入 tupdesc，
	 * 则假定违规元组来自那里。
	 */
	onfk = (queryno == RI_PLAN_CHECK_LOOKUPPK);
	if (onfk)
	{
		attnums = riinfo->fk_attnums;
		rel_oid = fk_rel->rd_id;
		if (tupdesc == NULL)
			tupdesc = fk_rel->rd_att;
	}
	else
	{
		attnums = riinfo->pk_attnums;
		rel_oid = pk_rel->rd_id;
		if (tupdesc == NULL)
			tupdesc = pk_rel->rd_att;
	}

	/*
	 * 检查权限 - 如果用户无权查看任何键列中的数据，
	 * 那么我们就不包含下方的 errdetail()。
	 *
	 * 先检查该关系上是否启用了 RLS。若已启用，我
	 * 们就不返回细节以免泄密。
	 *
	 * 接着检查表级权限，若失败再检查列级权限。
	 * privileges.
	 *
	 * 当被引用侧的分区正被分离/删除时，我们无需
	 * 检查，因为用户必是表属主。
	 */
	if (partgone)
		has_perm = true;
	else if (check_enable_rls(rel_oid, InvalidOid, true) != RLS_ENABLED)
	{
		aclresult = pg_class_aclcheck(rel_oid, GetUserId(), ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
		{
			/* 尝试列级权限 */
			for (int idx = 0; idx < riinfo->nkeys; idx++)
			{
				aclresult = pg_attribute_aclcheck(rel_oid, attnums[idx],
												  GetUserId(),
												  ACL_SELECT);

				/* 无权访问键 */
				if (aclresult != ACLCHECK_OK)
				{
					has_perm = false;
					break;
				}
			}
		}
	}
	else
		has_perm = false;

	if (has_perm)
	{
		/* 获取相关键的可打印版本 */
		initStringInfo(&key_names);
		initStringInfo(&key_values);
		for (int idx = 0; idx < riinfo->nkeys; idx++)
		{
			int			fnum = attnums[idx];
			Form_pg_attribute att = TupleDescAttr(tupdesc, fnum - 1);
			char	   *name,
					   *val;
			Datum		datum;
			bool		isnull;

			name = NameStr(att->attname);

			datum = slot_getattr(violatorslot, fnum, &isnull);
			if (!isnull)
			{
				Oid			foutoid;
				bool		typisvarlena;

				getTypeOutputInfo(att->atttypid, &foutoid, &typisvarlena);
				val = OidOutputFunctionCall(foutoid, datum);
			}
			else
				val = "null";

			if (idx > 0)
			{
				appendStringInfoString(&key_names, ", ");
				appendStringInfoString(&key_values, ", ");
			}
			appendStringInfoString(&key_names, name);
			appendStringInfoString(&key_values, val);
		}
	}

	if (partgone)
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("removing partition \"%s\" violates foreign key constraint \"%s\"",
						RelationGetRelationName(pk_rel),
						NameStr(riinfo->conname)),
				 errdetail("Key (%s)=(%s) is still referenced from table \"%s\".",
						   key_names.data, key_values.data,
						   RelationGetRelationName(fk_rel)),
				 errtableconstraint(fk_rel, NameStr(riinfo->conname))));
	else if (onfk)
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
						RelationGetRelationName(fk_rel),
						NameStr(riinfo->conname)),
				 has_perm ?
				 errdetail("Key (%s)=(%s) is not present in table \"%s\".",
						   key_names.data, key_values.data,
						   RelationGetRelationName(pk_rel)) :
				 errdetail("Key is not present in table \"%s\".",
						   RelationGetRelationName(pk_rel)),
				 errtableconstraint(fk_rel, NameStr(riinfo->conname))));
	else
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("update or delete on table \"%s\" violates foreign key constraint \"%s\" on table \"%s\"",
						RelationGetRelationName(pk_rel),
						NameStr(riinfo->conname),
						RelationGetRelationName(fk_rel)),
				 has_perm ?
				 errdetail("Key (%s)=(%s) is still referenced from table \"%s\".",
						   key_names.data, key_values.data,
						   RelationGetRelationName(fk_rel)) :
				 errdetail("Key is still referenced from table \"%s\".",
						   RelationGetRelationName(fk_rel)),
				 errtableconstraint(fk_rel, NameStr(riinfo->conname))));
}


/*
 * ri_NullCheck -
 *
 * 判断元组中全部键值的 NULL 状态
 *
 * 返回 RI_KEYS_ALL_NULL、RI_KEYS_NONE_NULL 或 RI_KEYS_SOME_NULL。
 */
static int
ri_NullCheck(TupleDesc tupDesc,
			 TupleTableSlot *slot,
			 const RI_ConstraintInfo *riinfo, bool rel_is_pk)
{
	const int16 *attnums;
	bool		allnull = true;
	bool		nonenull = true;

	if (rel_is_pk)
		attnums = riinfo->pk_attnums;
	else
		attnums = riinfo->fk_attnums;

	for (int i = 0; i < riinfo->nkeys; i++)
	{
		if (slot_attisnull(slot, attnums[i]))
			nonenull = false;
		else
			allnull = false;
	}

	if (allnull)
		return RI_KEYS_ALL_NULL;

	if (nonenull)
		return RI_KEYS_NONE_NULL;

	return RI_KEYS_SOME_NULL;
}


/*
 * ri_InitHashTables -
 *
 * 初始化我们的内部哈希表。
 */
static void
ri_InitHashTables(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RI_ConstraintInfo);
	ri_constraint_cache = hash_create("RI constraint cache",
									  RI_INIT_CONSTRAINTHASHSIZE,
									  &ctl, HASH_ELEM | HASH_BLOBS);

	/* 安排在 pg_constraint 变更时刷新缓存 */
	CacheRegisterSyscacheCallback(CONSTROID,
								  InvalidateConstraintCacheCallBack,
								  (Datum) 0);

	ctl.keysize = sizeof(RI_QueryKey);
	ctl.entrysize = sizeof(RI_QueryHashEntry);
	ri_query_cache = hash_create("RI query cache",
								 RI_INIT_QUERYHASHSIZE,
								 &ctl, HASH_ELEM | HASH_BLOBS);

	ctl.keysize = sizeof(RI_CompareKey);
	ctl.entrysize = sizeof(RI_CompareHashEntry);
	ri_compare_cache = hash_create("RI compare cache",
								   RI_INIT_QUERYHASHSIZE,
								   &ctl, HASH_ELEM | HASH_BLOBS);
}


/*
 * ri_FetchPreparedPlan -
 *
 * 在私有的已准备并已保存 SPI 执行计划哈希表
 * 中查找查询键。找到即返回该计划，否则 NULL。
 */
static SPIPlanPtr
ri_FetchPreparedPlan(RI_QueryKey *key)
{
	RI_QueryHashEntry *entry;
	SPIPlanPtr	plan;

	/*
	 * 首次调用时初始化哈希表
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/*
	 * 查找该键
	 */
	entry = (RI_QueryHashEntry *) hash_search(ri_query_cache,
											  key,
											  HASH_FIND, NULL);
	if (entry == NULL)
		return NULL;

	/*
	 * 检查该计划是否仍然有效。若无效，我们不想仅
	 * 依赖 plancache.c 来重新生成它；而应从零开始，
	 * 并同时重建查询文本。这是为了覆盖诸如表/列
	 * 重命名等情况。不过，探测可能发生的失效仍要
	 * 依赖 plancache 机制。
	 *
	 * 注意：仅当调用者已锁定外键与主键两个
	 * 关系时才可信。
	 */
	plan = entry->plan;
	if (plan && SPI_plan_is_valid(plan))
		return plan;

	/*
	 * 否则我们不妨现在就刷掉缓存的计划，以便
	 * 创建新计划前先腾出内存。
	 */
	entry->plan = NULL;
	if (plan)
		SPI_freeplan(plan);

	return NULL;
}


/*
 * ri_HashPreparedPlan -
 *
 * 将另一计划加入私有 SPI 查询计划哈希表。
 */
static void
ri_HashPreparedPlan(RI_QueryKey *key, SPIPlanPtr plan)
{
	RI_QueryHashEntry *entry;
	bool		found;

	/*
	 * 首次调用时初始化哈希表
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/*
	 * 加入新计划。可能会覆盖先前被 ri_FetchPreparedPlan
	 * 判为无效的条目。
	 */
	entry = (RI_QueryHashEntry *) hash_search(ri_query_cache,
											  key,
											  HASH_ENTER, &found);
	Assert(!found || entry->plan == NULL);
	entry->plan = plan;
}


/*
 * ri_KeysEqual -
 *
 * 检查 OLD 与 NEW 的键值是否全部相等。
 *
 * 注意：将来我们可能希望把它重新定义为检查
 * "IS NOT DISTINCT" 而非 "="，即允许两个 NULL
 * 被视为相等。目前没有必要，因为所有调用者
 * 都已预先发现至少一行不含 NULL。
 */
static bool
ri_KeysEqual(Relation rel, TupleTableSlot *oldslot, TupleTableSlot *newslot,
			 const RI_ConstraintInfo *riinfo, bool rel_is_pk)
{
	const int16 *attnums;

	if (rel_is_pk)
		attnums = riinfo->pk_attnums;
	else
		attnums = riinfo->fk_attnums;

	/* XXX: 一次性取齐所有必要属性或许值得 */
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		Datum		oldvalue;
		Datum		newvalue;
		bool		isnull;

		/*
		 * 取一个属性的旧值。若为 NULL 则两者不等。
		 */
		oldvalue = slot_getattr(oldslot, attnums[i], &isnull);
		if (isnull)
			return false;

		/*
		 * 取一个属性的新值。若为 NULL 则两者不等。
		 */
		newvalue = slot_getattr(newslot, attnums[i], &isnull);
		if (isnull)
			return false;

		if (rel_is_pk)
		{
			/*
			 * 若是主键表，则按字节比较。若某值被
			 * 改成一个 "看起来" 不同、但用相等算子比
			 * 较会相等的值所替换，我们必须传播主键更
			 * 改。这只对 ON UPDATE CASCADE 有影响，
			 * 但为保持一致，我们对主键的所有更改都一
			 * 视同仁。
			 */
			Form_pg_attribute att = TupleDescAttr(oldslot->tts_tupleDescriptor, attnums[i] - 1);

			if (!datum_image_eq(oldvalue, newvalue, att->attbyval, att->attlen))
				return false;
		}
		else
		{
			/*
			 * 对外键表，则用相应的相等算子比较。
			 * 比较相等的更改在更新后仍将满足该
			 * 约束。
			 */
			if (!ri_AttributesEqual(riinfo->ff_eq_oprs[i], RIAttType(rel, attnums[i]),
									oldvalue, newvalue))
				return false;
		}
	}

	return true;
}


/*
 * ri_AttributesEqual -
 *
 * 为两个值调用适当的相等比较算子。
 *
 * 注意：已检查过两值均不为 NULL。
 */
static bool
ri_AttributesEqual(Oid eq_opr, Oid typeid,
				   Datum oldvalue, Datum newvalue)
{
	RI_CompareHashEntry *entry = ri_HashCompareOp(eq_opr, typeid);

	/* 需要对值做转换吗？ */
	if (OidIsValid(entry->cast_func_finfo.fn_oid))
	{
		oldvalue = FunctionCall3(&entry->cast_func_finfo,
								 oldvalue,
								 Int32GetDatum(-1), /* typmod */
								 BoolGetDatum(false));	/* 隐式转换 */
		newvalue = FunctionCall3(&entry->cast_func_finfo,
								 newvalue,
								 Int32GetDatum(-1), /* typmod */
								 BoolGetDatum(false));	/* 隐式转换 */
	}

	/*
	 * 应用比较算子。
	 *
	 * 注意：本函数是某个调用栈的一部分，该栈判
	 * 断对行的更新是否显著到需要在外键约束另
	 * 一侧做检查或采取动作。因此，这里的比较
	 * 本应使用 *另一* 张表的排序规则来做。为
	 * 简单起见（例如我们可能根本没打开另一张
	 * 表），我们将只在这里使用默认排序规则，这
	 * 可能导致一些假阴性。如果我们将来允许数
	 * 据库级排序规则不定，皆会失效。
	 */
	return DatumGetBool(FunctionCall2Coll(&entry->eq_opr_finfo,
										  DEFAULT_COLLATION_OID,
										  oldvalue, newvalue));
}

/*
 * ri_HashCompareOp -
 *
 * 查看我们是否知道如何比较两值，若无则新建
 * 哈希条目。
 */
static RI_CompareHashEntry *
ri_HashCompareOp(Oid eq_opr, Oid typeid)
{
	RI_CompareKey key;
	RI_CompareHashEntry *entry;
	bool		found;

	/*
	 * 首次调用时初始化哈希表
	 */
	if (!ri_compare_cache)
		ri_InitHashTables();

	/*
	 * 查找或创建哈希条目。注意我们假定 RI_CompareKey
	 * 不含结构填充。
	 */
	key.eq_opr = eq_opr;
	key.typeid = typeid;
	entry = (RI_CompareHashEntry *) hash_search(ri_compare_cache,
												&key,
												HASH_ENTER, &found);
	if (!found)
		entry->valid = false;

	/*
	 * 如果尚未初始化，则进行初始化。由于我们会把
	 * 该哈希条目保留到后端生命周期结束，附属
	 * 信息要放入 TopMemoryContext。
	 */
	if (!entry->valid)
	{
		Oid			lefttype,
					righttype,
					castfunc;
		CoercionPathType pathtype;

		/* 我们始终需要知道如何调用相等算子 */
		fmgr_info_cxt(get_opcode(eq_opr), &entry->eq_opr_finfo,
					  TopMemoryContext);

		/*
		 * 若我们选择使用从外键类型到主键类型的转换，
		 * 则可能须经转换函数得算子输入类型。
		 *
		 * XXX 最终最好能在这里与 ri_AttributesEqual()
		 * 中支持数组强制转换的情形。目前这样做没有
		 * 意义，因为涉及非相同数组类型的情形会在
		 * 创建约束时被拒绝。
		 *
		 * XXX 或许还要考虑支持 CoerceViaIO？目前不
		 * 需要，因为隐式转换绝不会用到它。
		 */
		op_input_types(eq_opr, &lefttype, &righttype);
		Assert(lefttype == righttype);
		if (typeid == lefttype)
			castfunc = InvalidOid;	/* 最简情形 */
		else
		{
			pathtype = find_coercion_pathway(lefttype, typeid,
											 COERCION_IMPLICIT,
											 &castfunc);
			if (pathtype != COERCION_PATH_FUNC &&
				pathtype != COERCION_PATH_RELABELTYPE)
			{
				/*
				 * eq_opr 声明的输入类型可能是多态，
				 * 如 ANYARRAY 或 ANYENUM，或其他特例如
				 * RECORD；find_coercion_pathway 目前并未
				 * 涵盖这些特例。
				 */
				if (!IsBinaryCoercible(typeid, lefttype))
					elog(ERROR, "no conversion function from %s to %s",
						 format_type_be(typeid),
						 format_type_be(lefttype));
			}
		}
		if (OidIsValid(castfunc))
			fmgr_info_cxt(castfunc, &entry->cast_func_finfo,
						  TopMemoryContext);
		else
			entry->cast_func_finfo.fn_oid = InvalidOid;
		entry->valid = true;
	}

	return entry;
}


/*
 * 给定触发器函数 OID，判断它是否为 RI 触发器，
 * 若是，再判其属于主键或外键关系。
 */
int
RI_FKey_trigger_type(Oid tgfoid)
{
	switch (tgfoid)
	{
		case F_RI_FKEY_CASCADE_DEL:
		case F_RI_FKEY_CASCADE_UPD:
		case F_RI_FKEY_RESTRICT_DEL:
		case F_RI_FKEY_RESTRICT_UPD:
		case F_RI_FKEY_SETNULL_DEL:
		case F_RI_FKEY_SETNULL_UPD:
		case F_RI_FKEY_SETDEFAULT_DEL:
		case F_RI_FKEY_SETDEFAULT_UPD:
		case F_RI_FKEY_NOACTION_DEL:
		case F_RI_FKEY_NOACTION_UPD:
			return RI_TRIGGER_PK;

		case F_RI_FKEY_CHECK_INS:
		case F_RI_FKEY_CHECK_UPD:
			return RI_TRIGGER_FK;
	}

	return RI_TRIGGER_NONE;
}
