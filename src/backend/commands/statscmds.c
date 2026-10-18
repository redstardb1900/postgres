/*-------------------------------------------------------------------------
 *
 * statscmds.c
 *	  创建和更改扩展统计对象的命令
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/statscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "statistics/statistics.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static char *ChooseExtendedStatisticName(const char *name1, const char *name2,
										 const char *label, Oid namespaceid);
static char *ChooseExtendedStatisticNameAddition(List *exprs);


/* CreateStatistics 中 attnums 的 qsort 比较器 */
static int
compare_int16(const void *a, const void *b)
{
	int			av = *(const int16 *) a;
	int			bv = *(const int16 *) b;

	/* 若 int 比 int16 宽，这不会溢出 */
	return (av - bv);
}

/*
 *		CREATE STATISTICS
 */
ObjectAddress
CreateStatistics(CreateStatsStmt *stmt, bool check_rights)
{
	int16		attnums[STATS_MAX_DIMENSIONS];
	int			nattnums = 0;
	int			numcols;
	char	   *namestr;
	NameData	stxname;
	Oid			statoid;
	Oid			namespaceId;
	Oid			stxowner = GetUserId();
	HeapTuple	htup;
	Datum		values[Natts_pg_statistic_ext];
	bool		nulls[Natts_pg_statistic_ext];
	int2vector *stxkeys;
	List	   *stxexprs = NIL;
	Datum		exprsDatum;
	Relation	statrel;
	Relation	rel = NULL;
	Oid			relid;
	ObjectAddress parentobject,
				myself;
	Datum		types[4];		/* 每种可能的统计类型一个 */
	int			ntypes;
	ArrayType  *stxkind;
	bool		build_ndistinct;
	bool		build_dependencies;
	bool		build_mcv;
	bool		build_expressions;
	bool		requested_type = false;
	int			i;
	ListCell   *cell;
	ListCell   *cell2;

	Assert(IsA(stmt, CreateStatsStmt));

	/*
	 * 检查 FROM 子句。目前我们只允许它是单个简单表，
	 * 但以后可能会允许多个表和 JOIN 语法。语法
	 * 已为此做好准备，所以我们必须在这里检查
	 * 我们得到的是我们能支持的。
	 */
	if (list_length(stmt->relations) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only a single relation is allowed in CREATE STATISTICS")));

	foreach(cell, stmt->relations)
	{
		Node	   *rln = (Node *) lfirst(cell);

		if (!IsA(rln, RangeVar))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only a single relation is allowed in CREATE STATISTICS")));

		/*
		 * CREATE STATISTICS 会影响未来的执行计划，但不
		 * 干扰正在执行的计划。所以只对关系取
		 * ShareUpdateExclusiveLock 应该就够了，它与 ANALYZE 和
		 * 其他设置统计信息的 DDL 冲突，但不与
		 * 普通查询冲突。
		 */
		rel = relation_openrv((RangeVar *) rln, ShareUpdateExclusiveLock);

		/* 限制为允许的关系类型 */
		if (rel->rd_rel->relkind != RELKIND_RELATION &&
			rel->rd_rel->relkind != RELKIND_MATVIEW &&
			rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
			rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot define statistics for relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

		/* 必须拥有该关系才能在其上创建统计信息 */
		if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), stxowner))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		/* 不允许在系统目录上创建统计信息 */
		if (!allowSystemTableMods && IsSystemRelation(rel))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: \"%s\" is a system catalog",
							RelationGetRelationName(rel))));
	}

	Assert(rel);
	relid = RelationGetRelid(rel);

	/*
	 * 若节点有名称，就拆分它并确定创建命名空间。
	 * 若没有，把对象放到与关系相同的命名空间中，并为它
	 * 编造一个名称。（这可以经由 "CREATE STATISTICS ..."
	 * 或 "CREATE TABLE ... (LIKE)" 发生。）
	 */
	if (stmt->defnames)
		namespaceId = QualifiedNameGetCreationNamespace(stmt->defnames,
														&namestr);
	else
	{
		namespaceId = RelationGetNamespace(rel);
		namestr = ChooseExtendedStatisticName(RelationGetRelationName(rel),
											  ChooseExtendedStatisticNameAddition(stmt->exprs),
											  "stat",
											  namespaceId);
	}
	namestrcpy(&stxname, namestr);

	/*
	 * 检查我们在目标命名空间中有创建权限。
	 * 若调用方不想要，就跳过检查。
	 */
	if (check_rights)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(NamespaceRelationId, namespaceId,
									GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(namespaceId));
	}

	/*
	 * 处理统计对象已存在的可能性。
	 */
	if (SearchSysCacheExists2(STATEXTNAMENSP,
							  CStringGetDatum(namestr),
							  ObjectIdGetDatum(namespaceId)))
	{
		if (stmt->if_not_exists)
		{
			/*
			 * 由于统计对象不是扩展的成员（见下面的注释），
			 * 这里无需 checkMembershipInCurrentExtension。
			 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("statistics object \"%s\" already exists, skipping",
							namestr)));
			relation_close(rel, NoLock);
			return InvalidObjectAddress;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("statistics object \"%s\" already exists", namestr)));
	}

	/*
	 * 确保使用的列数不超过 STATS_MAX_DIMENSIONS。可能
	 * 有重复等等，但我们稍后再处理那些。
	 */
	numcols = list_length(stmt->exprs);
	if (numcols > STATS_MAX_DIMENSIONS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("cannot have more than %d columns in statistics",
						STATS_MAX_DIMENSIONS)));

	/*
	 * 把表达式列表转换为简单的 attnum 数组，但同时也
	 * 保留更复杂表达式的列表。顺便施加一些约束：
	 * 我们不允许对系统属性做扩展统计，而且需要
	 * 数据类型有小于操作符。
	 *
	 * 有很多办法把简单的属性引用"伪装"成表达式，
	 * 例如 "(a+0)" 等。我们不可能全部检测出来，
	 * 但至少处理属性加括号的简单情形。若用户
	 * 执意要绕过（像 "(a+0)" 的例子），总有办法，
	 * 但这使它在一定程度上与索引处理属性/表达式
	 * 的方式一致。
	 */
	foreach(cell, stmt->exprs)
	{
		StatsElem  *selem = lfirst_node(StatsElem, cell);

		if (selem->name)		/* 列引用 */
		{
			char	   *attname;
			HeapTuple	atttuple;
			Form_pg_attribute attForm;
			TypeCacheEntry *type;

			attname = selem->name;

			atttuple = SearchSysCacheAttName(relid, attname);
			if (!HeapTupleIsValid(atttuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist",
								attname)));
			attForm = (Form_pg_attribute) GETSTRUCT(atttuple);

			/* 禁止在扩展统计中使用系统属性 */
			if (attForm->attnum <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("statistics creation on system columns is not supported")));

			/* 禁止没有小于操作符的数据类型 */
			type = lookup_type_cache(attForm->atttypid, TYPECACHE_LT_OPR);
			if (type->lt_opr == InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("column \"%s\" cannot be used in statistics because its type %s has no default btree operator class",
								attname, format_type_be(attForm->atttypid))));

			attnums[nattnums] = attForm->attnum;
			nattnums++;
			ReleaseSysCache(atttuple);
		}
		else if (IsA(selem->expr, Var)) /* 加括号的列引用 */
		{
			Var		   *var = (Var *) selem->expr;
			TypeCacheEntry *type;

			/* 禁止在扩展统计中使用系统属性 */
			if (var->varattno <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("statistics creation on system columns is not supported")));

			/* 禁止没有小于操作符的数据类型 */
			type = lookup_type_cache(var->vartype, TYPECACHE_LT_OPR);
			if (type->lt_opr == InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("column \"%s\" cannot be used in statistics because its type %s has no default btree operator class",
								get_attname(relid, var->varattno, false), format_type_be(var->vartype))));

			attnums[nattnums] = var->varattno;
			nattnums++;
		}
		else					/* 表达式 */
		{
			Node	   *expr = selem->expr;
			Oid			atttype;
			TypeCacheEntry *type;
			Bitmapset  *attnums = NULL;
			int			k;

			Assert(expr != NULL);

			/* 禁止引用系统属性的表达式。 */
			pull_varattnos(expr, 1, &attnums);

			k = -1;
			while ((k = bms_next_member(attnums, k)) >= 0)
			{
				AttrNumber	attnum = k + FirstLowInvalidHeapAttributeNumber;

				if (attnum <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("statistics creation on system columns is not supported")));
			}

			/*
			 * 禁止没有小于操作符的数据类型。
			 *
			 * 对单个表达式上的统计，我们忽略这一点，
			 * 那种情况下我们只构建常规统计（那个代码
			 * 能处理这类数据类型）。
			 */
			if (list_length(stmt->exprs) > 1)
			{
				atttype = exprType(expr);
				type = lookup_type_cache(atttype, TYPECACHE_LT_OPR);
				if (type->lt_opr == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("expression cannot be used in multivariate statistics because its type %s has no default btree operator class",
									format_type_be(atttype))));
			}

			stxexprs = lappend(stxexprs, expr);
		}
	}

	/*
	 * 解析统计类型。
	 *
	 * 首先检查：若是单个表达式的情形，就没有
	 * 指定任何统计类型（对简单的 CREATE STATISTICS
	 * 形式我们不允许那样做）。
	 */
	if ((list_length(stmt->exprs) == 1) && (list_length(stxexprs) == 1))
	{
		/* 未指定统计类型 */
		if (stmt->stat_types != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("when building statistics on a single expression, statistics kinds may not be specified")));
	}

	/* 好，检查我们是否认识这些统计类型。 */
	build_ndistinct = false;
	build_dependencies = false;
	build_mcv = false;
	foreach(cell, stmt->stat_types)
	{
		char	   *type = strVal(lfirst(cell));

		if (strcmp(type, "ndistinct") == 0)
		{
			build_ndistinct = true;
			requested_type = true;
		}
		else if (strcmp(type, "dependencies") == 0)
		{
			build_dependencies = true;
			requested_type = true;
		}
		else if (strcmp(type, "mcv") == 0)
		{
			build_mcv = true;
			requested_type = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized statistics kind \"%s\"",
							type)));
	}

	/*
	 * 若未指定统计类型，就全部构建（但仅在统计定义
	 * 于多列/多表达式之上时）。
	 */
	if ((!requested_type) && (numcols >= 2))
	{
		build_ndistinct = true;
		build_dependencies = true;
		build_mcv = true;
	}

	/*
	 * 有非平凡表达式时，自动构建表达式统计。
	 * 这允许为考虑逐子句估计的统计计算
	 * 良好估计（例如函数依赖）。
	 */
	build_expressions = (stxexprs != NIL);

	/*
	 * 检查语句中至少指定了两列，或者我们是在
	 * 单个表达式上构建统计。
	 */
	if ((numcols < 2) && (list_length(stxexprs) != 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("extended statistics require at least 2 columns")));

	/*
	 * 对 attnum 排序，这使检测重复稍微容易些，
	 * 而且没有坏处（与索引不同，它不影响
	 * 内容）。
	 */
	qsort(attnums, nattnums, sizeof(int16), compare_int16);

	/*
	 * 检查列列表中的重复。attnum 已排序，
	 * 所以只检查相邻元素。
	 */
	for (i = 1; i < nattnums; i++)
	{
		if (attnums[i] == attnums[i - 1])
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("duplicate column name in statistics definition")));
	}

	/*
	 * 检查重复的表达式。我们做两个循环，统计每个
	 * 表达式的出现次数。这是 O(N^2)，但我们只允许少量
	 * 表达式，而且它不会经常执行。
	 *
	 * XXX 我们不交叉检查属性与表达式，因为似乎不值得。
	 * 原则上我们可以检查表达式不包含像 "(a)" 这样
	 * 平凡的属性引用，但理由类似于为什么我们
	 * 不费心从表达式中提取列。要么代价昂贵，
	 * 要么对执意的用户来说很容易绕过，而且允许
	 * 这样的统计没有风险（统计无用，
	 * 但无害）。
	 */
	foreach(cell, stxexprs)
	{
		Node	   *expr1 = (Node *) lfirst(cell);
		int			cnt = 0;

		foreach(cell2, stxexprs)
		{
			Node	   *expr2 = (Node *) lfirst(cell2);

			if (equal(expr1, expr2))
				cnt += 1;
		}

		/* 每个表达式至少应该找到它自己 */
		Assert(cnt >= 1);

		if (cnt > 1)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("duplicate expression in statistics definition")));
	}

	/* 构造排序列列表的 int2vector 表示 */
	stxkeys = buildint2vector(attnums, nattnums);

	/* 构造已启用统计类型的 char 数组 */
	ntypes = 0;
	if (build_ndistinct)
		types[ntypes++] = CharGetDatum(STATS_EXT_NDISTINCT);
	if (build_dependencies)
		types[ntypes++] = CharGetDatum(STATS_EXT_DEPENDENCIES);
	if (build_mcv)
		types[ntypes++] = CharGetDatum(STATS_EXT_MCV);
	if (build_expressions)
		types[ntypes++] = CharGetDatum(STATS_EXT_EXPRESSIONS);
	Assert(ntypes > 0 && ntypes <= lengthof(types));
	stxkind = construct_array_builtin(types, ntypes, CHAROID);

	/* 把表达式（若有）转换为 text datum */
	if (stxexprs != NIL)
	{
		char	   *exprsString;

		exprsString = nodeToString(stxexprs);
		exprsDatum = CStringGetTextDatum(exprsString);
		pfree(exprsString);
	}
	else
		exprsDatum = (Datum) 0;

	statrel = table_open(StatisticExtRelationId, RowExclusiveLock);

	/*
	 * 一切似乎正常，构建 pg_statistic_ext 元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	statoid = GetNewOidWithIndex(statrel, StatisticExtOidIndexId,
								 Anum_pg_statistic_ext_oid);
	values[Anum_pg_statistic_ext_oid - 1] = ObjectIdGetDatum(statoid);
	values[Anum_pg_statistic_ext_stxrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_statistic_ext_stxname - 1] = NameGetDatum(&stxname);
	values[Anum_pg_statistic_ext_stxnamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_statistic_ext_stxstattarget - 1] = Int32GetDatum(-1);
	values[Anum_pg_statistic_ext_stxowner - 1] = ObjectIdGetDatum(stxowner);
	values[Anum_pg_statistic_ext_stxkeys - 1] = PointerGetDatum(stxkeys);
	values[Anum_pg_statistic_ext_stxkind - 1] = PointerGetDatum(stxkind);

	values[Anum_pg_statistic_ext_stxexprs - 1] = exprsDatum;
	if (exprsDatum == (Datum) 0)
		nulls[Anum_pg_statistic_ext_stxexprs - 1] = true;

	/* 把它插入 pg_statistic_ext */
	htup = heap_form_tuple(statrel->rd_att, values, nulls);
	CatalogTupleInsert(statrel, htup);
	heap_freetuple(htup);

	relation_close(statrel, RowExclusiveLock);

	/*
	 * 我们以前也创建 pg_statistic_ext_data 元组，但不清楚
	 * stxdinherit 标志应该是什么值（它取决于 rel 是否分区、
	 * 是否包含数据，等等）。
	 */

	InvokeObjectPostCreateHook(StatisticExtRelationId, statoid, 0);

	/*
	 * 使 relcache 失效，让其他人看到新的统计对象。
	 */
	CacheInvalidateRelcache(rel);

	relation_close(rel, NoLock);

	/*
	 * 对统计中使用的每一列添加 AUTO 依赖，这样若其中
	 * 任何或全部列被删除，统计对象也会消失。
	 */
	ObjectAddressSet(myself, StatisticExtRelationId, statoid);

	/* 为普通列引用添加依赖 */
	for (i = 0; i < nattnums; i++)
	{
		ObjectAddressSubSet(parentobject, RelationRelationId, relid, attnums[i]);
		recordDependencyOn(&myself, &parentobject, DEPENDENCY_AUTO);
	}

	/*
	 * 若没有对任何列的依赖，就给统计对象一个对整个表的
	 * auto 依赖。多数情况下这是冗余的，但若统计表达式
	 * 不含任何 Var（看似奇怪但可能），它可能不冗余。
	 * 这与我们在 index_create 中对索引所做的
	 * 一致。
	 *
	 * XXX 添加这个依赖之前我们故意不考虑表达式，
	 * 因为 recordDependencyOnSingleRelExpr 可能不为
	 * 整行 Var 创建任何依赖。
	 */
	if (!nattnums)
	{
		ObjectAddressSet(parentobject, RelationRelationId, relid);
		recordDependencyOn(&myself, &parentobject, DEPENDENCY_AUTO);
	}

	/*
	 * 存储对统计表达式中提到的一切的依赖，
	 * 就像我们对索引表达式所做的那样。
	 */
	if (stxexprs)
		recordDependencyOnSingleRelExpr(&myself,
										(Node *) stxexprs,
										relid,
										DEPENDENCY_NORMAL,
										DEPENDENCY_AUTO, false);

	/*
	 * 还要添加对命名空间和所有者的依赖。这些是必需的，
	 * 因为统计对象的命名空间和/或所有者可能不同于
	 * 底层表。
	 */
	ObjectAddressSet(parentobject, NamespaceRelationId, namespaceId);
	recordDependencyOn(&myself, &parentobject, DEPENDENCY_NORMAL);

	recordDependencyOnOwner(StatisticExtRelationId, statoid, stxowner);

	/*
	 * XXX 这里可能也应该有 recordDependencyOnCurrentExtension 调用，
	 * 但我们必须添加对 ALTER EXTENSION ADD/DROP STATISTICS 的支持，
	 * 那比看起来值得的工作量更大。
	 */

	/* 添加任何请求的注释 */
	if (stmt->stxcomment != NULL)
		CreateComments(statoid, StatisticExtRelationId, 0,
					   stmt->stxcomment);

	/* 返回统计对象的地址 */
	return myself;
}

/*
 *		ALTER STATISTICS
 */
ObjectAddress
AlterStatistics(AlterStatsStmt *stmt)
{
	Relation	rel;
	Oid			stxoid;
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Datum		repl_val[Natts_pg_statistic_ext];
	bool		repl_null[Natts_pg_statistic_ext];
	bool		repl_repl[Natts_pg_statistic_ext];
	ObjectAddress address;
	int			newtarget = stmt->stxstattarget;

	/* 把统计目标限制在合理范围内 */
	if (newtarget < -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("statistics target %d is too low",
						newtarget)));
	}
	else if (newtarget > 10000)
	{
		newtarget = 10000;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lowering statistics target to %d",
						newtarget)));
	}

	/* 查找统计对象的 OID */
	stxoid = get_statistics_object_oid(stmt->defnames, stmt->missing_ok);

	/*
	 * 若走到这里且 OID 无效，意味着统计对象不存在，
	 * 但命令指定了 IF EXISTS。所以把它报告为
	 * 一条简单 NOTICE，然后我们就完成了。
	 */
	if (!OidIsValid(stxoid))
	{
		char	   *schemaname;
		char	   *statname;

		Assert(stmt->missing_ok);

		DeconstructQualifiedName(stmt->defnames, &schemaname, &statname);

		if (schemaname)
			ereport(NOTICE,
					(errmsg("statistics object \"%s.%s\" does not exist, skipping",
							schemaname, statname)));
		else
			ereport(NOTICE,
					(errmsg("statistics object \"%s\" does not exist, skipping",
							statname)));

		return InvalidObjectAddress;
	}

	/* 搜索 pg_statistic_ext */
	rel = table_open(StatisticExtRelationId, RowExclusiveLock);

	oldtup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(stxoid));
	if (!HeapTupleIsValid(oldtup))
		elog(ERROR, "cache lookup failed for extended statistics object %u", stxoid);

	/* 必须是现有统计对象的所有者 */
	if (!object_ownercheck(StatisticExtRelationId, stxoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_STATISTIC_EXT,
					   NameListToString(stmt->defnames));

	/* 构建新元组。 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* 替换 stxstattarget 列 */
	repl_repl[Anum_pg_statistic_ext_stxstattarget - 1] = true;
	repl_val[Anum_pg_statistic_ext_stxstattarget - 1] = Int32GetDatum(newtarget);

	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   repl_val, repl_null, repl_repl);

	/* 更新系统目录。 */
	CatalogTupleUpdate(rel, &newtup->t_self, newtup);

	InvokeObjectPostAlterHook(StatisticExtRelationId, stxoid, 0);

	ObjectAddressSet(address, StatisticExtRelationId, stxoid);

	/*
	 * 注意：因为我们只支持更改统计目标，不支持
	 * 其他字段，所以无需更新依赖。
	 */

	heap_freetuple(newtup);
	ReleaseSysCache(oldtup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * 删除 pg_statistic_ext_data 目录中的条目。我们不知道
 * 行是否存在，所以不要报错。
 */
void
RemoveStatisticsDataById(Oid statsOid, bool inh)
{
	Relation	relation;
	HeapTuple	tup;

	relation = table_open(StatisticExtDataRelationId, RowExclusiveLock);

	tup = SearchSysCache2(STATEXTDATASTXOID, ObjectIdGetDatum(statsOid),
						  BoolGetDatum(inh));

	/* 我们不知道 inh 值对应的数据行是否存在。 */
	if (HeapTupleIsValid(tup))
	{
		CatalogTupleDelete(relation, &tup->t_self);

		ReleaseSysCache(tup);
	}

	table_close(relation, RowExclusiveLock);
}

/*
 * 统计对象删除的核心。
 */
void
RemoveStatisticsById(Oid statsOid)
{
	Relation	relation;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_statistic_ext statext;
	Oid			relid;

	/*
	 * 删除 pg_statistic_ext 元组。同时向关联表发出
	 * 缓存失效，使依赖的计划被重建。
	 */
	relation = table_open(StatisticExtRelationId, RowExclusiveLock);

	tup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statsOid));

	if (!HeapTupleIsValid(tup)) /* 不应该发生 */
		elog(ERROR, "cache lookup failed for statistics object %u", statsOid);

	statext = (Form_pg_statistic_ext) GETSTRUCT(tup);
	relid = statext->stxrelid;

	/*
	 * 删除保存实际统计数据的 pg_statistic_ext_data 元组。
	 * 可能有带/不带继承的数据，所以尝试删除两者。
	 * 我们先锁用户表，防止其他进程（例如
	 * DROP STATISTICS）并发移除行。
	 */
	rel = table_open(relid, ShareUpdateExclusiveLock);

	RemoveStatisticsDataById(statsOid, true);
	RemoveStatisticsDataById(statsOid, false);

	CacheInvalidateRelcacheByRelid(relid);

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	/* 把锁保留到事务结束。 */
	table_close(rel, NoLock);

	table_close(relation, RowExclusiveLock);
}

/*
 * 为新统计对象选择一个不冲突的名称。
 *
 * name1、name2 和 label 的用法与 makeObjectName() 相同，
 * 只是 label 不能为 NULL；必要时会在 label 后面追加
 * 数字，以创建在指定命名空间内唯一的名称。
 *
 * 返回 palloc 分配的字符串。
 *
 * 注意：理论上仍可能发生冲突，比如有人并发地选择了
 * 相同的名称。实际上这不太可能成为问题，尤其是
 * 若你在 name1 标识的关系上持有 share update exclusive
 * 锁。不过，若在单个命令内选择多个名称，最好先
 * 创建新对象并做 CommandCounterIncrement，
 * 再选择下一个！
 */
static char *
ChooseExtendedStatisticName(const char *name1, const char *name2,
							const char *label, Oid namespaceid)
{
	int			pass = 0;
	char	   *stxname = NULL;
	char		modlabel[NAMEDATALEN];

	/* 先尝试未修改的 label */
	strlcpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		Oid			existingstats;

		stxname = makeObjectName(name1, name2, modlabel);

		existingstats = GetSysCacheOid2(STATEXTNAMENSP, Anum_pg_statistic_ext_oid,
										PointerGetDatum(stxname),
										ObjectIdGetDatum(namespaceid));
		if (!OidIsValid(existingstats))
			break;

		/* 发现冲突，尝试新的名称组成部分 */
		pfree(stxname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	return stxname;
}

/*
 * 给定新统计对象的列名列表，生成它的 "name2"。
 * 它将与父表名和合适的 label 一起传给
 * ChooseExtendedStatisticName。
 *
 * 我们知道实际使用的字符数少于 NAMEDATALEN，
 * 所以生成到那么多之后就可以截断结果。
 *
 * XXX 另见 ChooseForeignKeyConstraintNameAddition 和
 * ChooseIndexNameAddition。
 */
static char *
ChooseExtendedStatisticNameAddition(List *exprs)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, exprs)
	{
		StatsElem  *selem = (StatsElem *) lfirst(lc);
		const char *name;

		/* 它应该是其中之一，但若不是就直接跳过 */
		if (!IsA(selem, StatsElem))
			continue;

		name = selem->name;

		if (buflen > 0)
			buf[buflen++] = '_';	/* 在名称之间插入 _ */

		/*
		 * 对列名为空的表达式，我们使用固定的 'expr'。
		 * 对索引这由 ChooseIndexColumnNames 处理，但我们
		 * 没有用于统计的此类函数，似乎也不值得添加。
		 * 若需要更好的名称，用户可以显式指定。
		 */
		if (!name)
			name = "expr";

		/*
		 * 此时 buflen <= NAMEDATALEN。name 应该已经小于
		 * NAMEDATALEN，但为多疑起见用 strlcpy。
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * StatisticsGetRelation: 给定统计对象的 OID，获取其
 * 定义所在关系的 OID。使用系统缓存。
 */
Oid
StatisticsGetRelation(Oid statId, bool missing_ok)
{
	HeapTuple	tuple;
	Form_pg_statistic_ext stx;
	Oid			result;

	tuple = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statId));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return InvalidOid;
		elog(ERROR, "cache lookup failed for statistics object %u", statId);
	}
	stx = (Form_pg_statistic_ext) GETSTRUCT(tuple);
	Assert(stx->oid == statId);

	result = stx->stxrelid;
	ReleaseSysCache(tuple);
	return result;
}
