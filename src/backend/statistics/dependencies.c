/*-------------------------------------------------------------------------
 *
 * dependencies.c
 *	  POSTGRES functional dependencies
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/dependencies.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/bytea.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* 结构头成员的大小（magic、type、ndeps） */
#define SizeOfHeader		(3 * sizeof(uint32))

/* 一条序列化依赖的大小（degree、natts、atts） */
#define SizeOfItem(natts) \
	(sizeof(double) + sizeof(AttrNumber) * (1 + (natts)))

/* 一条依赖的最小大小（含两个属性） */
#define MinSizeOfItem	SizeOfItem(2)

/* 当所有依赖都取最小大小时，依赖数据的最小大小 */
#define MinSizeOfItems(ndeps) \
	(SizeOfHeader + (ndeps) * MinSizeOfItem)

/*
 * DependencyGenerator 的内部状态。函数依赖类似于
 * n 个元素的 k-排列，只是前 (k-1) 个元素的顺序
 * 无关紧要。也就是说，(a,b=>c) 与 (b,a=>c) 等价。
 */
typedef struct DependencyGeneratorData
{
	int			k;				/* 该依赖的大小 */
	int			n;				/* 可能的属性个数 */
	int			current;		/* 下一个要返回的依赖（下标） */
	AttrNumber	ndependencies;	/* 已生成的依赖个数 */
	AttrNumber *dependencies;	/* 预先生成的依赖数组	*/
} DependencyGeneratorData;

typedef DependencyGeneratorData *DependencyGenerator;

static void generate_dependencies_recurse(DependencyGenerator state,
										  int index, AttrNumber start, AttrNumber *current);
static void generate_dependencies(DependencyGenerator state);
static DependencyGenerator DependencyGenerator_init(int n, int k);
static void DependencyGenerator_free(DependencyGenerator state);
static AttrNumber *DependencyGenerator_next(DependencyGenerator state);
static double dependency_degree(StatsBuildData *data, int k, AttrNumber *dependency);
static bool dependency_is_fully_matched(MVDependency *dependency,
										Bitmapset *attnums);
static bool dependency_is_compatible_clause(Node *clause, Index relid,
											AttrNumber *attnum);
static bool dependency_is_compatible_expression(Node *clause, Index relid,
												List *statlist, Node **expr);
static MVDependency *find_strongest_dependency(MVDependencies **dependencies,
											   int ndependencies, Bitmapset *attnums);
static Selectivity clauselist_apply_dependencies(PlannerInfo *root, List *clauses,
												 int varRelid, JoinType jointype,
												 SpecialJoinInfo *sjinfo,
												 MVDependency **dependencies,
												 int ndependencies,
												 AttrNumber *list_attnums,
												 Bitmapset **estimatedclauses);

static void
generate_dependencies_recurse(DependencyGenerator state, int index,
							  AttrNumber start, AttrNumber *current)
{
	/*
	 * 生成器对前 (k-1) 个元素的处理与
	 * 最后一个元素不同。
	 */
	if (index < (state->k - 1))
	{
		AttrNumber	i;

		/*
		 * 前 (k-1) 个值必须按升序，我们
		 * 用递归来生成它们。
		 */

		for (i = start; i < state->n; i++)
		{
			current[index] = i;
			generate_dependencies_recurse(state, (index + 1), (i + 1), current);
		}
	}
	else
	{
		int			i;

		/*
		 * 最后一个元素是被蕴含的值，它不遵守
		 * 升序。我们只需检查该值不在
		 * 前 (k-1) 个元素里。
		 */

		for (i = 0; i < state->n; i++)
		{
			int			j;
			bool		match = false;

			current[index] = i;

			for (j = 0; j < index; j++)
			{
				if (current[j] == i)
				{
					match = true;
					break;
				}
			}

			/*
			 * 若在依赖的前半部分没有找到该值，
			 * 就完成了。
			 */
			if (!match)
			{
				state->dependencies = (AttrNumber *) repalloc(state->dependencies,
															  state->k * (state->ndependencies + 1) * sizeof(AttrNumber));
				memcpy(&state->dependencies[(state->k * state->ndependencies)],
					   current, state->k * sizeof(AttrNumber));
				state->ndependencies++;
			}
		}
	}
}

/* 生成全部依赖（n 个元素的 k-排列） */
static void
generate_dependencies(DependencyGenerator state)
{
	AttrNumber *current = (AttrNumber *) palloc0(sizeof(AttrNumber) * state->k);

	generate_dependencies_recurse(state, 0, 0, current);

	pfree(current);
}

/*
 * 初始化变体的 DependencyGenerator，并预先构造这些变体。
 *
 * 这里预先构造全部变体。也可以在
 * DependencyGenerator_next() 里生成它们，但这样似乎更简单。
 */
static DependencyGenerator
DependencyGenerator_init(int n, int k)
{
	DependencyGenerator state;

	Assert((n >= k) && (k > 0));

	/* 分配 DependencyGenerator 状态 */
	state = (DependencyGenerator) palloc0(sizeof(DependencyGeneratorData));
	state->dependencies = (AttrNumber *) palloc(k * sizeof(AttrNumber));

	state->ndependencies = 0;
	state->current = 0;
	state->k = k;
	state->n = n;

	/* 现在实际预先生成全部变体 */
	generate_dependencies(state);

	return state;
}

/* 释放 DependencyGenerator 状态 */
static void
DependencyGenerator_free(DependencyGenerator state)
{
	pfree(state->dependencies);
	pfree(state);
}

/* 生成下一种组合 */
static AttrNumber *
DependencyGenerator_next(DependencyGenerator state)
{
	if (state->current == state->ndependencies)
		return NULL;

	return &state->dependencies[state->k * state->current++];
}


/*
 * 在数据上验证函数依赖。
 *
 * 检测函数依赖的实际主力。给定 k 个属性的一种
 * 变体，它检查前 (k-1) 个是否足以决定
 * 最后一个。
 */
static double
dependency_degree(StatsBuildData *data, int k, AttrNumber *dependency)
{
	int			i,
				nitems;
	MultiSortSupport mss;
	SortItem   *items;
	AttrNumber *attnums_dep;

	/* 在一组内有效的计数器 */
	int			group_size = 0;
	int			n_violations = 0;

	/* 支持（与之一致）该依赖的行总数 */
	int			n_supporting_rows = 0;

	/* 确保至少有两个输入属性。 */
	Assert(k >= 2);

	/* 所有属性列的排序信息 */
	mss = multi_sort_init(k);

	/*
	 * 把下标数组翻译成该依赖的普通 attnum
	 * （在 StatsBuildData 里识别这些列时需要它）。
	 */
	attnums_dep = (AttrNumber *) palloc(k * sizeof(AttrNumber));
	for (i = 0; i < k; i++)
		attnums_dep[i] = data->attnums[dependency[i]];

	/*
	 * 用相当简单的算法验证依赖 (a,b,...)->z：
	 *
	 * (a) 按字典序对数据排序。
	 *
	 * (b) 按前 (k-1) 列把数据分成组。
	 *
	 * (c) 对每一组，统计最后一列里不同的值。
	 *
	 * 我们使用列数据类型的默认排序操作符和排序规则；
	 * 也许将来值得使用列专用的排序规则？
	 */

	/* 为各个维度准备排序函数 */
	for (i = 0; i < k; i++)
	{
		VacAttrStats *colstat = data->stats[dependency[i]];
		TypeCacheEntry *type;

		type = lookup_type_cache(colstat->attrtypid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid) /* 不应当发生 */
			elog(ERROR, "cache lookup failed for ordering operator for type %u",
				 colstat->attrtypid);

		/* 为这个维度准备排序函数 */
		multi_sort_add_dimension(mss, i, type->lt_opr, colstat->attrcollid);
	}

	/*
	 * 构造用多键排序支持排好序的 SortItem 数组。
	 *
	 * XXX 这依赖于所有统计项指向同一个元组
	 * 描述符。目前该假设成立，但将来可能改变，
	 * 例如若我们支持多张表上的统计信息。
	 */
	items = build_sorted_items(data, &nitems, mss, k, attnums_dep);

	/*
	 * 遍历排好序的数组，按前 (k-1) 列把它分成若干行。
	 * 若最后一列只有单个值，就把该组计为
	 * 支持该函数依赖。否则就把它计为
	 * 矛盾。
	 */

	/* 从构成一组的第一行开始 */
	group_size = 1;

	/* 循环到数组末尾之后 1 的位置，以便计入最后一组 */
	for (i = 1; i <= nitems; i++)
	{
		/*
		 * 检查该组是否结束。结束可能是因为我们已经处理了
		 * 全部项（i==nitems），也可能是因为第 i 项不等于
		 * 前一项。
		 */
		if (i == nitems ||
			multi_sort_compare_dims(0, k - 2, &items[i - 1], &items[i], mss) != 0)
		{
			/*
			 * 若该组里没有发现违反，就把该组的行
			 * 记为支持该函数依赖。
			 */
			if (n_violations == 0)
				n_supporting_rows += group_size;

			/* 为新的一组重置计数器 */
			n_violations = 0;
			group_size = 1;
			continue;
		}
		/* 前面的列匹配，但最后一列不匹配（因此矛盾） */
		else if (multi_sort_compare_dim(k - 1, &items[i - 1], &items[i], mss) != 0)
			n_violations++;

		group_size++;
	}

	/* 把有效度计算为（支持的/总数）。 */
	return (n_supporting_rows * 1.0 / data->numrows);
}

/*
 * 检测列组之间的函数依赖。
 *
 * 生成所有可能的列子集（变体），并为每一个计算
 * 有效度。例如在三列 (a,b,c) 上创建统计信息时，
 * 有 9 种可能的依赖。
 *
 *	   两列					三列：
 *	   -----------			  -------------
 *	   (a) -> b				  (a,b) -> c
 *	   (a) -> c				  (a,c) -> b
 *	   (b) -> a				  (b,c) -> a
 *	   (b) -> c
 *	   (c) -> a
 *	   (c) -> b
 */
MVDependencies *
statext_dependencies_build(StatsBuildData *data)
{
	int			i,
				k;

	/* 结果 */
	MVDependencies *dependencies = NULL;
	MemoryContext cxt;

	Assert(data->nattnums >= 2);

	/* 跟踪 dependency_degree 调用分配的内存 */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"dependency_degree cxt",
								ALLOCSET_DEFAULT_SIZES);

	/*
	 * 我们将尝试从最小的函数依赖开始构造，它们只覆盖
	 * 2 列，直到最大的、覆盖统计对象所含
	 * 全部列的那些。我们从最小的开始，
	 * 因为我们希望能跳过已经被蕴含的依赖。
	 */
	for (k = 2; k <= data->nattnums; k++)
	{
		AttrNumber *dependency; /* 含 k 个元素的数组 */

		/* 准备一个变体的 DependencyGenerator */
		DependencyGenerator DependencyGenerator = DependencyGenerator_init(data->nattnums, k);

		/* 生成 k 个值（从 n 个里取）的全部可能变体 */
		while ((dependency = DependencyGenerator_next(DependencyGenerator)))
		{
			double		degree;
			MVDependency *d;
			MemoryContext oldcxt;

			/* 释放依赖度计算使用的内存 */
			oldcxt = MemoryContextSwitchTo(cxt);

			/* 计算该依赖看起来有多有效 */
			degree = dependency_degree(data, k, dependency);

			MemoryContextSwitchTo(oldcxt);
			MemoryContextReset(cxt);

			/*
			 * 若该依赖看起来完全无效，就不要存储它
			 */
			if (degree == 0.0)
				continue;

			d = (MVDependency *) palloc0(offsetof(MVDependency, attributes)
										 + k * sizeof(AttrNumber));

			/* 复制该依赖（并保留指向 stxkeys 的下标） */
			d->degree = degree;
			d->nattributes = k;
			for (i = 0; i < k; i++)
				d->attributes[i] = data->attnums[dependency[i]];

			/* 初始化依赖列表 */
			if (dependencies == NULL)
			{
				dependencies
					= (MVDependencies *) palloc0(sizeof(MVDependencies));

				dependencies->magic = STATS_DEPS_MAGIC;
				dependencies->type = STATS_DEPS_TYPE_BASIC;
				dependencies->ndeps = 0;
			}

			dependencies->ndeps++;
			dependencies = (MVDependencies *) repalloc(dependencies,
													   offsetof(MVDependencies, deps)
													   + dependencies->ndeps * sizeof(MVDependency *));

			dependencies->deps[dependencies->ndeps - 1] = d;
		}

		/*
		 * k 个元素的变体已经处理完，因此释放
		 * DependencyGenerator
		 */
		DependencyGenerator_free(DependencyGenerator);
	}

	MemoryContextDelete(cxt);

	return dependencies;
}


/*
 * 把依赖列表序列化成一个 bytea 值。
 */
bytea *
statext_dependencies_serialize(MVDependencies *dependencies)
{
	int			i;
	bytea	   *output;
	char	   *tmp;
	Size		len;

	/* 我们需要存储 ndeps，以及每一条的属性个数 */
	len = VARHDRSZ + SizeOfHeader;

	/* 并且还要为实际的属性号和依赖度留出空间 */
	for (i = 0; i < dependencies->ndeps; i++)
		len += SizeOfItem(dependencies->deps[i]->nattributes);

	output = (bytea *) palloc0(len);
	SET_VARSIZE(output, len);

	tmp = VARDATA(output);

	/* 存储基本结构的值（magic、type、ndeps） */
	memcpy(tmp, &dependencies->magic, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &dependencies->type, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &dependencies->ndeps, sizeof(uint32));
	tmp += sizeof(uint32);

	/* 为每条依赖存储属性个数和属性号 */
	for (i = 0; i < dependencies->ndeps; i++)
	{
		MVDependency *d = dependencies->deps[i];

		memcpy(tmp, &d->degree, sizeof(double));
		tmp += sizeof(double);

		memcpy(tmp, &d->nattributes, sizeof(AttrNumber));
		tmp += sizeof(AttrNumber);

		memcpy(tmp, d->attributes, sizeof(AttrNumber) * d->nattributes);
		tmp += sizeof(AttrNumber) * d->nattributes;

		/* 防止溢出 */
		Assert(tmp <= ((char *) output + len));
	}

	/* 确保我们产生的数据量恰好正确 */
	Assert(tmp == ((char *) output + len));

	return output;
}

/*
 * 把序列化的依赖读入 MVDependencies 结构。
 */
MVDependencies *
statext_dependencies_deserialize(bytea *data)
{
	int			i;
	Size		min_expected_size;
	MVDependencies *dependencies;
	char	   *tmp;

	if (data == NULL)
		return NULL;

	if (VARSIZE_ANY_EXHDR(data) < SizeOfHeader)
		elog(ERROR, "invalid MVDependencies size %zu (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), SizeOfHeader);

	/* 读取 MVDependencies 头 */
	dependencies = (MVDependencies *) palloc0(sizeof(MVDependencies));

	/* 把指针初始化到数据部分（跳过 varlena 头） */
	tmp = VARDATA_ANY(data);

	/* 读取头成员并做基本的健全性检查 */
	memcpy(&dependencies->magic, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&dependencies->type, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&dependencies->ndeps, tmp, sizeof(uint32));
	tmp += sizeof(uint32);

	if (dependencies->magic != STATS_DEPS_MAGIC)
		elog(ERROR, "invalid dependency magic %d (expected %d)",
			 dependencies->magic, STATS_DEPS_MAGIC);

	if (dependencies->type != STATS_DEPS_TYPE_BASIC)
		elog(ERROR, "invalid dependency type %d (expected %d)",
			 dependencies->type, STATS_DEPS_TYPE_BASIC);

	if (dependencies->ndeps == 0)
		elog(ERROR, "invalid zero-length item array in MVDependencies");

	/* 对这些参数，我们预期的最小 bytea 大小是多少 */
	min_expected_size = SizeOfItem(dependencies->ndeps);

	if (VARSIZE_ANY_EXHDR(data) < min_expected_size)
		elog(ERROR, "invalid dependencies size %zu (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), min_expected_size);

	/* 为 MCV 项分配空间 */
	dependencies = repalloc(dependencies, offsetof(MVDependencies, deps)
							+ (dependencies->ndeps * sizeof(MVDependency *)));

	for (i = 0; i < dependencies->ndeps; i++)
	{
		double		degree;
		AttrNumber	k;
		MVDependency *d;

		/* 有效度 */
		memcpy(&degree, tmp, sizeof(double));
		tmp += sizeof(double);

		/* 属性个数 */
		memcpy(&k, tmp, sizeof(AttrNumber));
		tmp += sizeof(AttrNumber);

		/* 属性个数有效吗？ */
		Assert((k >= 2) && (k <= STATS_MAX_DIMENSIONS));

		/* 既然已经知道属性个数，就分配该依赖 */
		d = (MVDependency *) palloc0(offsetof(MVDependency, attributes)
									 + (k * sizeof(AttrNumber)));

		d->degree = degree;
		d->nattributes = k;

		/* 复制属性号 */
		memcpy(d->attributes, tmp, sizeof(AttrNumber) * d->nattributes);
		tmp += sizeof(AttrNumber) * d->nattributes;

		dependencies->deps[i] = d;

		/* 仍然在 bytea 之内 */
		Assert(tmp <= ((char *) data + VARSIZE_ANY(data)));
	}

	/* 我们应当恰好用完整个 bytea */
	Assert(tmp == ((char *) data + VARSIZE_ANY(data)));

	return dependencies;
}

/*
 * dependency_is_fully_matched
 *		检查在给定属性上的子句时，一条函数依赖是否被完全匹配
 *		（假定这些子句是合适的相等子句）
 */
static bool
dependency_is_fully_matched(MVDependency *dependency, Bitmapset *attnums)
{
	int			j;

	/*
	 * 检查该依赖是否真的被子句完全覆盖。我们
	 * 必须翻译所有属性号，因为引用的是那些号
	 */
	for (j = 0; j < dependency->nattributes; j++)
	{
		int			attnum = dependency->attributes[j];

		if (!bms_is_member(attnum, attnums))
			return false;
	}

	return true;
}

/*
 * statext_dependencies_load
 *		为指定的 pg_statistic_ext 元组装载函数依赖
 */
MVDependencies *
statext_dependencies_load(Oid mvoid, bool inh)
{
	MVDependencies *result;
	bool		isnull;
	Datum		deps;
	HeapTuple	htup;

	htup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(mvoid),
						   BoolGetDatum(inh));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	deps = SysCacheGetAttr(STATEXTDATASTXOID, htup,
						   Anum_pg_statistic_ext_data_stxddependencies, &isnull);
	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_DEPENDENCIES, mvoid);

	result = statext_dependencies_deserialize(DatumGetByteaPP(deps));

	ReleaseSysCache(htup);

	return result;
}

/*
 * pg_dependencies_in		- 类型 pg_dependencies 的输入例程。
 *
 * pg_dependencies 真实到足以作为表列，但它没有自己的操作，
 * 也不允许输入。
 */
Datum
pg_dependencies_in(PG_FUNCTION_ARGS)
{
	/*
	 * pg_node_list 以二进制形式存储数据，不需要解析文本输入，
	 * 因此不允许这样做。
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_dependencies")));

	PG_RETURN_VOID();			/* 消除编译器告警 */
}

/*
 * pg_dependencies		- 类型 pg_dependencies 的输出例程。
 */
Datum
pg_dependencies_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVDependencies *dependencies = statext_dependencies_deserialize(data);
	int			i,
				j;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	for (i = 0; i < dependencies->ndeps; i++)
	{
		MVDependency *dependency = dependencies->deps[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		appendStringInfoChar(&str, '"');
		for (j = 0; j < dependency->nattributes; j++)
		{
			if (j == dependency->nattributes - 1)
				appendStringInfoString(&str, " => ");
			else if (j > 0)
				appendStringInfoString(&str, ", ");

			appendStringInfo(&str, "%d", dependency->attributes[j]);
		}
		appendStringInfo(&str, "\": %f", dependency->degree);
	}

	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_dependencies_recv		- 类型 pg_dependencies 的二进制输入例程。
 */
Datum
pg_dependencies_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_dependencies")));

	PG_RETURN_VOID();			/* 消除编译器告警 */
}

/*
 * pg_dependencies_send		- 类型 pg_dependencies 的二进制输出例程。
 *
 * 函数依赖被序列化在一个 bytea 值里（尽管类型
 * 的名字不同），因此直接发送它即可。
 */
Datum
pg_dependencies_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}

/*
 * dependency_is_compatible_clause
 *		判断该子句是否与函数依赖相容。
 *
 * 目前只接受形式为与伪常量相等、或可以
 * 按这种方式解释的子句。此外，子句的变量
 * 部分必须是属于指定关系的简单 Var，成功时我们把其
 * 属性号返回到 *attnum。
 */
static bool
dependency_is_compatible_clause(Node *clause, Index relid, AttrNumber *attnum)
{
	Var		   *var;
	Node	   *clause_expr;

	if (IsA(clause, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) clause;

		/* 伪常量没有意义（它们没法含有 Var） */
		if (rinfo->pseudoconstant)
			return false;

		/* 引用多个 varno 或一个都不引用的子句不相容 */
		if (bms_membership(rinfo->clause_relids) != BMS_SINGLETON)
			return false;

		clause = (Node *) rinfo->clause;
	}

	if (is_opclause(clause))
	{
		/* 若它是操作子句，就检查 Var = Const 或 Const = Var。 */
		OpExpr	   *expr = (OpExpr *) clause;

		/* 只有两个参数的表达式才是候选。 */
		if (list_length(expr->args) != 2)
			return false;

		/* 确保未被选中的参数是伪常量。 */
		if (is_pseudo_constant_clause(lsecond(expr->args)))
			clause_expr = linitial(expr->args);
		else if (is_pseudo_constant_clause(linitial(expr->args)))
			clause_expr = lsecond(expr->args);
		else
			return false;

		/*
		 * 若它不是 "=" 操作符，就忽略该子句，因为它与
		 * 函数依赖不相容。
		 *
		 * 这里用的是估计选择率的函数，而不是直接用
		 * 操作符（有点别扭，但也只能如此……）。
		 *
		 * XXX 这相当可疑；也许最好检查 btree
		 * 或哈希操作符类成员资格，以免被自定义
		 * 选择率函数误导，并且与规划器
		 * 其他地方的决定更加一致。
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* 可以继续检查 var */
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* 若它是标量数组操作符，就检查 Var IN Const。 */
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) clause;

		/*
		 * 拒绝 ALL() 变体，我们只关心 ANY/IN。
		 *
		 * XXX 也许应当检查是否所有值都相同，并在那种情况下允许
		 * ALL？不过这似乎不太实用。
		 */
		if (!expr->useOr)
			return false;

		/* 只有两个参数的表达式才是候选。 */
		if (list_length(expr->args) != 2)
			return false;

		/*
		 * 我们知道它总是 (Var IN Const)，因此假定 var 是
		 * 第一个参数，伪常量是第二个。
		 */
		if (!is_pseudo_constant_clause(lsecond(expr->args)))
			return false;

		clause_expr = linitial(expr->args);

		/*
		 * 若它不是 "=" 操作符，就忽略该子句，因为它与
		 * 函数依赖不相容。识别该操作符的方法只是
		 * 看它用哪个函数来估计
		 * 选择率。这有点奇怪，但其他类似的
		 * 地方也是这么做的。
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* 可以继续检查 var */
	}
	else if (is_orclause(clause))
	{
		BoolExpr   *bool_expr = (BoolExpr *) clause;
		ListCell   *lc;

		/* 一开始没有属性号 */
		*attnum = InvalidAttrNumber;

		foreach(lc, bool_expr->args)
		{
			AttrNumber	clause_attnum;

			/*
			 * 若在参数里发现了不相容的子句，就把
			 * 整个子句当作不相容。
			 */
			if (!dependency_is_compatible_clause((Node *) lfirst(lc),
												 relid, &clause_attnum))
				return false;

			if (*attnum == InvalidAttrNumber)
				*attnum = clause_attnum;

			/* 确保所有变量都相同（相同的 attnum） */
			if (*attnum != clause_attnum)
				return false;
		}

		/* 该 Var 已经由递归调用检查过 */
		return true;
	}
	else if (is_notclause(clause))
	{
		/*
		 * "NOT x" 可以解释成 "x = false"，因此取出参数并
		 * 继续看它是否为合适的 Var。
		 */
		clause_expr = (Node *) get_notclausearg(clause);
	}
	else
	{
		/*
		 * 布尔表达式 "x" 可以解释成 "x = true"，因此
		 * 继续看它是否为合适的 Var。
		 */
		clause_expr = (Node *) clause;
	}

	/*
	 * 可以忽略操作数上方的任何 RelabelType 节点。（不会有
	 * 多于一个，因为已经应用过 eval_const_expressions。）
	 */
	if (IsA(clause_expr, RelabelType))
		clause_expr = (Node *) ((RelabelType *) clause_expr)->arg;

	/* 目前只支持普通 Var */
	if (!IsA(clause_expr, Var))
		return false;

	/* 好，我们知道有一个 Var */
	var = (Var *) clause_expr;

	/* 确保 Var 来自正确的关系 */
	if (var->varno != relid)
		return false;

	/* 最好也确保 Var 来自当前层次 */
	if (var->varlevelsup != 0)
		return false;

	/* 也忽略系统属性（我们不允许在那些属性上建立统计信息） */
	if (!AttrNumberIsForUserDefinedAttr(var->varattno))
		return false;

	*attnum = var->varattno;
	return true;
}

/*
 * find_strongest_dependency
 *		找出属性上最强的依赖。
 *
 * 应用函数依赖时，我们从最强的
 * 依赖开始。也就是说，我们选择这样的依赖：
 *
 * (a) 所有属性都被相等子句覆盖。
 *
 * (b) 属性最多。
 *
 * (c) 有效度最高。
 *
 * 这保证我们先消除最冗余的条件
 * （见 dependencies_clauselist_selectivity 里的注释）。
 */
static MVDependency *
find_strongest_dependency(MVDependencies **dependencies, int ndependencies,
						  Bitmapset *attnums)
{
	int			i,
				j;
	MVDependency *strongest = NULL;

	/* 子句里的 attnum 个数 */
	int			nattnums = bms_num_members(attnums);

	/*
	 * 遍历 MVDependency 项，从完全匹配的依赖里找出最强的一条。
	 * 我们先做便宜的检查，然后再
	 * 把它与 attnum 匹配。
	 */
	for (i = 0; i < ndependencies; i++)
	{
		for (j = 0; j < dependencies[i]->ndeps; j++)
		{
			MVDependency *dependency = dependencies[i]->deps[j];

			/*
			 * 跳过引用的属性比可用子句更多的依赖，
			 * 因为那些无法被完全匹配。
			 */
			if (dependency->nattributes > nattnums)
				continue;

			if (strongest)
			{
				/* 也跳过属性比最强依赖更少的依赖。 */
				if (dependency->nattributes < strongest->nattributes)
					continue;

				/* 属性个数相同时，也跳过更弱的依赖 */
				if (strongest->nattributes == dependency->nattributes &&
					strongest->degree > dependency->degree)
					continue;
			}

			/*
			 * 这条依赖更强，但仍必须检查它是否
			 * 与这些 attnum 完全匹配。我们把这项检查放在最后，因为
			 * 它比前面的检查略贵。
			 */
			if (dependency_is_fully_matched(dependency, attnums))
				strongest = dependency; /* 保存新的最佳匹配 */
		}
	}

	return strongest;
}

/*
 * clauselist_apply_dependencies
 *		把指定的函数依赖应用到子句列表，并
 *		返回与任一给定依赖相容的那些子句的
 *		估计选择率。
 *
 * 这里将估计所有尚未估计、且与函数依赖相容、
 * 并且有属性被任一给定依赖提及（无论是作为蕴含属性还是
 * 被蕴含属性）的子句。
 *
 * 给定属性 (a,b) 上的子句（列表）以及函数依赖
 * (a=>b)，按列选择率 P(a) 和 P(b) 在概念上用下面的公式
 * 组合起来。
 *
 *		P(a,b) = f * P(a) + (1-f) * P(a) * P(b)。
 *
 * 其中 f 是依赖度。这反映了这样一个事实：我们
 * 预期所有行中有比例为 f 的部分与依赖
 * (a=>b) 一致，因此选择率为 P(a)，而其余的行被
 * 当作独立的。
 *
 * 实践中我们使用该公式的一个略加修改的版本，它对依赖行使用
 * 选择率 Min(P(a), P(b))，因为结果
 * 显然不应当超过任一列各自的选择率。也就是说，
 * 我们实际用来组合选择率的公式是：
 *
 *		P(a,b) = f * Min(P(a), P(b)) + (1-f) * P(a) * P(b)。
 *
 * 若匹配子句的具体值与函数依赖不一致，
 * 这会带来相当大的差别。
 */
static Selectivity
clauselist_apply_dependencies(PlannerInfo *root, List *clauses,
							  int varRelid, JoinType jointype,
							  SpecialJoinInfo *sjinfo,
							  MVDependency **dependencies, int ndependencies,
							  AttrNumber *list_attnums,
							  Bitmapset **estimatedclauses)
{
	Bitmapset  *attnums;
	int			i;
	int			j;
	int			nattrs;
	Selectivity *attr_sel;
	int			attidx;
	int			listidx;
	ListCell   *l;
	Selectivity s1;

	/*
	 * 从所有给定依赖里取出全部蕴含属性和被蕴含属性的 attnum。
	 * 这些属性中的每一个都预期至少有
	 * 1 个尚未估计的相容子句，我们将在
	 * 这里估计它。
	 */
	attnums = NULL;
	for (i = 0; i < ndependencies; i++)
	{
		for (j = 0; j < dependencies[i]->nattributes; j++)
		{
			AttrNumber	attnum = dependencies[i]->attributes[j];

			attnums = bms_add_member(attnums, attnum);
		}
	}

	/*
	 * 为这些属性中的每一个计算按列选择率估计，
	 * 并把所有对应的子句标记为已估计。
	 */
	nattrs = bms_num_members(attnums);
	attr_sel = (Selectivity *) palloc(sizeof(Selectivity) * nattrs);

	attidx = 0;
	i = -1;
	while ((i = bms_next_member(attnums, i)) >= 0)
	{
		List	   *attr_clauses = NIL;
		Selectivity simple_sel;

		listidx = -1;
		foreach(l, clauses)
		{
			Node	   *clause = (Node *) lfirst(l);

			listidx++;
			if (list_attnums[listidx] == i)
			{
				attr_clauses = lappend(attr_clauses, clause);
				*estimatedclauses = bms_add_member(*estimatedclauses, listidx);
			}
		}

		simple_sel = clauselist_selectivity_ext(root, attr_clauses, varRelid,
												jointype, sjinfo, false);
		attr_sel[attidx++] = simple_sel;
	}

	/*
	 * 现在用依赖信息组合这些选择率。对于
	 * a -> b -> c 这样的依赖链，数组里 b -> c 依赖会
	 * 排在 a -> b 依赖之前，因此我们反向遍历
	 * 数组，以保证这类链按正确顺序计算。
	 *
	 * 如上所述，成对的选择率用下面的公式
	 * 组合。
	 *
	 * P(a,b) = f * Min(P(a), P(b)) + (1-f) * P(a) * P(b)。
	 *
	 * 以保证组合后的选择率永远不大于任一
	 * 单独的选择率。
	 *
	 * 在多个依赖适用时（例如 a -> b -> c），我们用
	 * 条件概率按下面的方式计算总结果：
	 *
	 * P(a,b,c) = P(c|a,b) * P(a,b) = P(c|a,b) * P(b|a) * P(a)。
	 *
	 * 因此我们把所有被蕴含属性的选择率替换成
	 * 条件概率，这些概率以它们全部的蕴含
	 * 属性为条件。所有其他未被蕴含属性的选择率
	 * 保持原样。
	 */
	for (i = ndependencies - 1; i >= 0; i--)
	{
		MVDependency *dependency = dependencies[i];
		AttrNumber	attnum;
		Selectivity s2;
		double		f;

		/* 全部蕴含属性的选择率 */
		s1 = 1.0;
		for (j = 0; j < dependency->nattributes - 1; j++)
		{
			attnum = dependency->attributes[j];
			attidx = bms_member_index(attnums, attnum);
			s1 *= attr_sel[attidx];
		}

		/* 被蕴含属性原来的选择率 */
		attnum = dependency->attributes[j];
		attidx = bms_member_index(attnums, attnum);
		s2 = attr_sel[attidx];

		/*
		 * 用公式 P(b|a) = P(a,b) / P(a) 计算给定 s1 时 s2 的条件概率来
		 * 替换 s2，该公式简化为：
		 *
		 * P(b|a) = f * Min(P(a), P(b)) / P(a) + (1-f) * P(b)。
		 *
		 * 其中 P(a) = s1，即蕴含属性的选择率，而
		 * P(b) = s2，即被蕴含属性的选择率。
		 */
		f = dependency->degree;

		if (s1 <= s2)
			attr_sel[attidx] = f + (1 - f) * s2;
		else
			attr_sel[attidx] = f * s2 / s1 + (1 - f) * s2;
	}

	/*
	 * 于是，所有这些属性上全部子句的总选择率
	 * 就是所有原来的（未被蕴含的）概率与
	 * 新的条件（被蕴含的）概率的乘积。
	 */
	s1 = 1.0;
	for (i = 0; i < nattrs; i++)
		s1 *= attr_sel[i];

	CLAMP_PROBABILITY(s1);

	pfree(attr_sel);
	bms_free(attnums);

	return s1;
}

/*
 * dependency_is_compatible_expression
 *		判断该表达式是否与函数依赖相容。
 *
 * 与 dependency_is_compatible_clause 类似，但不强制
 * 表达式是简单 Var。成功时，把匹配的统计
 * 表达式返回到 *expr。
 */
static bool
dependency_is_compatible_expression(Node *clause, Index relid, List *statlist, Node **expr)
{
	ListCell   *lc,
			   *lc2;
	Node	   *clause_expr;

	if (IsA(clause, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) clause;

		/* 伪常量没有意义（它们没法含有 Var） */
		if (rinfo->pseudoconstant)
			return false;

		/* 引用多个 varno 或一个都不引用的子句不相容 */
		if (bms_membership(rinfo->clause_relids) != BMS_SINGLETON)
			return false;

		clause = (Node *) rinfo->clause;
	}

	if (is_opclause(clause))
	{
		/* 若它是操作子句，就检查 Var = Const 或 Const = Var。 */
		OpExpr	   *expr = (OpExpr *) clause;

		/* 只有两个参数的表达式才是候选。 */
		if (list_length(expr->args) != 2)
			return false;

		/* 确保未被选中的参数是伪常量。 */
		if (is_pseudo_constant_clause(lsecond(expr->args)))
			clause_expr = linitial(expr->args);
		else if (is_pseudo_constant_clause(linitial(expr->args)))
			clause_expr = lsecond(expr->args);
		else
			return false;

		/*
		 * 若它不是 "=" 操作符，就忽略该子句，因为它与
		 * 函数依赖不相容。
		 *
		 * 这里用的是估计选择率的函数，而不是直接用
		 * 操作符（有点别扭，但也只能如此……）。
		 *
		 * XXX 这相当可疑；也许最好检查 btree
		 * 或哈希操作符类成员资格，以免被自定义
		 * 选择率函数误导，并且与规划器
		 * 其他地方的决定更加一致。
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* 可以继续检查 var */
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* 若它是标量数组操作符，就检查 Var IN Const。 */
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) clause;

		/*
		 * 拒绝 ALL() 变体，我们只关心 ANY/IN。
		 *
		 * FIXME 也许应当检查是否所有值都相同，并
		 * 在那种情况下允许 ALL？不过这似乎不太实用。
		 */
		if (!expr->useOr)
			return false;

		/* 只有两个参数的表达式才是候选。 */
		if (list_length(expr->args) != 2)
			return false;

		/*
		 * 我们知道它总是 (Var IN Const)，因此假定 var 是
		 * 第一个参数，伪常量是第二个。
		 */
		if (!is_pseudo_constant_clause(lsecond(expr->args)))
			return false;

		clause_expr = linitial(expr->args);

		/*
		 * 若它不是 "=" 操作符，就忽略该子句，因为它与
		 * 函数依赖不相容。识别该操作符的方法只是
		 * 看它用哪个函数来估计
		 * 选择率。这有点奇怪，但其他类似的
		 * 地方也是这么做的。
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* 可以继续检查 var */
	}
	else if (is_orclause(clause))
	{
		BoolExpr   *bool_expr = (BoolExpr *) clause;

		/* 一开始没有表达式（我们将使用第一个匹配） */
		*expr = NULL;

		foreach(lc, bool_expr->args)
		{
			Node	   *or_expr = NULL;

			/*
			 * 若在参数里发现了不相容的表达式，就把
			 * 整个表达式当作不相容。
			 */
			if (!dependency_is_compatible_expression((Node *) lfirst(lc), relid,
													 statlist, &or_expr))
				return false;

			if (*expr == NULL)
				*expr = or_expr;

			/* 确保所有表达式都相同 */
			if (!equal(or_expr, *expr))
				return false;
		}

		/* 该表达式已经由递归调用检查过 */
		return true;
	}
	else if (is_notclause(clause))
	{
		/*
		 * "NOT x" 可以解释成 "x = false"，因此取出参数并
		 * 继续看它是否为合适的 Var。
		 */
		clause_expr = (Node *) get_notclausearg(clause);
	}
	else
	{
		/*
		 * 布尔表达式 "x" 可以解释成 "x = true"，因此
		 * 继续看它是否为合适的 Var。
		 */
		clause_expr = (Node *) clause;
	}

	/*
	 * 可以忽略操作数上方的任何 RelabelType 节点。（不会有
	 * 多于一个，因为已经应用过 eval_const_expressions。）
	 */
	if (IsA(clause_expr, RelabelType))
		clause_expr = (Node *) ((RelabelType *) clause_expr)->arg;

	/*
	 * 搜索匹配的统计表达式。
	 */
	foreach(lc, statlist)
	{
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);

		/* 忽略没有依赖的统计信息 */
		if (info->kind != STATS_EXT_DEPENDENCIES)
			continue;

		foreach(lc2, info->exprs)
		{
			Node	   *stat_expr = (Node *) lfirst(lc2);

			if (equal(clause_expr, stat_expr))
			{
				*expr = stat_expr;
				return true;
			}
		}
	}

	return false;
}

/*
 * dependencies_clauselist_selectivity
 *		用函数依赖统计信息返回给定子句（的一个子集）的
 *		估计选择率，若没有有用的函数
 *		依赖统计信息则返回 1.0。
 *
 * estimatedclauses 是输入/输出参数，对纳入
 * 估计选择率的每个子句，按其（从零起计的）列表下标
 * 置上相应的位。
 *
 * 给定属性 (a,b) 上的相等子句，我们找出它们之间最强的依赖，
 * 即 (a=>b) 或 (b=>a)。假定选中的依赖是 (a=>b)，
 * 然后用下面的公式组合按子句的选择率。
 *
 *	   P(a,b) = f * P(a) + (1-f) * P(a) * P(b)。
 *
 * 其中 f 是依赖度。（实际上我们使用该公式的一个略加
 * 修改的版本，见 clauselist_apply_dependencies()。）
 *
 * 当子句涉及两个以上的属性时，依赖被
 * 递归应用，从最宽/最强的依赖开始。例如
 * P(a,b,c) 首先被这样拆开：
 *
 *	   P(a,b,c) = f * P(a,b) + (1-f) * P(a,b) * P(c)。
 *
 * 假定 (a,b=>c) 是最强的依赖。
 */
Selectivity
dependencies_clauselist_selectivity(PlannerInfo *root,
									List *clauses,
									int varRelid,
									JoinType jointype,
									SpecialJoinInfo *sjinfo,
									RelOptInfo *rel,
									Bitmapset **estimatedclauses)
{
	Selectivity s1 = 1.0;
	ListCell   *l;
	Bitmapset  *clauses_attnums = NULL;
	AttrNumber *list_attnums;
	int			listidx;
	MVDependencies **func_dependencies;
	int			nfunc_dependencies;
	int			total_ndeps;
	MVDependency **dependencies;
	int			ndependencies;
	int			i;
	AttrNumber	attnum_offset;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	/* 唯一表达式 */
	Node	  **unique_exprs;
	int			unique_exprs_cnt;

	/* 检查是否有任何可能对我们有用的统计信息。 */
	if (!has_stats_of_kind(rel->statlist, STATS_EXT_DEPENDENCIES))
		return 1.0;

	list_attnums = (AttrNumber *) palloc(sizeof(AttrNumber) *
										 list_length(clauses));

	/*
	 * 我们按每个子句都是唯一表达式来分配空间，尽管
	 * 这大概是过剩的。有些将是简单的列引用，我们会
	 * 把它们翻译成 attnum，而且可能有重复。但一次分配
	 * 比以后再 repalloc 更简单也更便宜。
	 */
	unique_exprs = (Node **) palloc(sizeof(Node *) * list_length(clauses));
	unique_exprs_cnt = 0;

	/*
	 * 预处理子句列表，提取每一项里见到的 attnum。
	 * 我们需要判断是否有任何子句对
	 * 依赖选择率估计有用。沿途我们会把每个子句的
	 * 全部 attnum 记入一个列表，稍后引用它，这样
	 * 就不必再重复同样的工作。我们也会跟踪见到的全部
	 * attnum。
	 *
	 * 我们也会跳过已经用其他类型的
	 * 统计信息估计过的子句（把它们当作不相容）。
	 *
	 * 为了处理表达式，我们给它们分配负的 attnum，就像它是
	 * 系统属性一样（这没问题，因为我们只允许在用户
	 * 属性上建立扩展统计）。然后我们把一切都偏移表达式的
	 * 个数，以便把这些值存进 bitmapset。
	 */
	listidx = 0;
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		AttrNumber	attnum;
		Node	   *expr = NULL;

		/* 默认忽略该子句 */
		list_attnums[listidx] = InvalidAttrNumber;

		if (!bms_is_member(listidx, *estimatedclauses))
		{
			/*
			 * 若它是简单的列引用，就只提取 attnum。若
			 * 它是表达式，就分配一个负的 attnum，就像它是
			 * 系统属性一样。
			 */
			if (dependency_is_compatible_clause(clause, rel->relid, &attnum))
			{
				list_attnums[listidx] = attnum;
			}
			else if (dependency_is_compatible_expression(clause, rel->relid,
														 rel->statlist,
														 &expr))
			{
				/* 分配给该表达式的特殊 attnum */
				attnum = InvalidAttrNumber;

				Assert(expr != NULL);

				/* 若该表达式是重复的，就使用同一个 attnum。 */
				for (i = 0; i < unique_exprs_cnt; i++)
				{
					if (equal(unique_exprs[i], expr))
					{
						/* 从负属性号到表达式 */
						attnum = -(i + 1);
						break;
					}
				}

				/* 列表里没有找到，因此加入它 */
				if (attnum == InvalidAttrNumber)
				{
					unique_exprs[unique_exprs_cnt++] = expr;

					/* 在递增该值之后，以得到 -1、-2、…… */
					attnum = (-unique_exprs_cnt);
				}

				/* 记住分配给该子句的 attnum */
				list_attnums[listidx] = attnum;
			}
		}

		listidx++;
	}

	Assert(listidx == list_length(clauses));

	/*
	 * attnum 需要偏移多少？若没有表达式，
	 * 就不需要偏移。否则需要偏移到足以让
	 * 最低值（-unique_exprs_cnt）变成 1。
	 */
	if (unique_exprs_cnt > 0)
		attnum_offset = (unique_exprs_cnt + 1);
	else
		attnum_offset = 0;

	/*
	 * 既然已经知道有多少表达式，就可以把这些值
	 * 偏移到刚好能构造 bitmapset。
	 */
	for (i = 0; i < list_length(clauses); i++)
	{
		AttrNumber	attnum;

		/* 忽略不相容或已经估计过的子句 */
		if (list_attnums[i] == InvalidAttrNumber)
			continue;

		/* 确保 attnum 在预期范围内 */
		Assert(list_attnums[i] >= (-unique_exprs_cnt));
		Assert(list_attnums[i] <= MaxHeapAttributeNumber);

		/* 确保 attnum 为正（有效的 AttrNumber） */
		attnum = list_attnums[i] + attnum_offset;

		/*
		 * 要么它是普通属性，要么它是表达式，那种
		 * 情况下我们不得曾经见过它（表达式是唯一的）。
		 *
		 * XXX 检查它是否为普通属性必须用
		 * 原来的 attnum，而第二项检查必须用
		 * 带偏移的值。
		 */
		Assert(AttrNumberIsForUserDefinedAttr(list_attnums[i]) ||
			   !bms_is_member(attnum, clauses_attnums));

		/*
		 * 记住偏移后的 attnum，属性和表达式都要记。
		 * 我们将把 list_attnums 传给 clauselist_apply_dependencies，它
		 * 用这个来在 bitmap 里识别子句。我们也可以传入
		 * 偏移，但这样更方便。
		 */
		list_attnums[i] = attnum;

		clauses_attnums = bms_add_member(clauses_attnums, attnum);
	}

	/*
	 * 若不同的 attnum 和表达式不足两个，就
	 * 拒绝整个子句列表。我们必须返回 1.0，以免调用
	 * 函数的选择率受到影响。
	 */
	if (bms_membership(clauses_attnums) != BMS_MULTIPLE)
	{
		bms_free(clauses_attnums);
		pfree(list_attnums);
		return 1.0;
	}

	/*
	 * 装载所有至少匹配两个参数的函数依赖。我们可以
	 * 简单地同时考虑全部依赖，而不必搜索
	 * 最佳的统计对象。
	 *
	 * 为了不浪费周期和内存，我们只为
	 * 至少匹配两个属性的统计信息反序列化依赖。数组按
	 * 所有对象都匹配的假设来分配。我们可以增长数组，使它
	 * 恰好合适，但由于把释放的块移到空闲列表等原因，
	 * 那反正可能是浪费。
	 */
	func_dependencies = (MVDependencies **) palloc(sizeof(MVDependencies *) *
												   list_length(rel->statlist));
	nfunc_dependencies = 0;
	total_ndeps = 0;

	foreach(l, rel->statlist)
	{
		StatisticExtInfo *stat = (StatisticExtInfo *) lfirst(l);
		int			nmatched;
		int			nexprs;
		int			k;
		MVDependencies *deps;

		/* 跳过类型不正确的统计信息 */
		if (stat->kind != STATS_EXT_DEPENDENCIES)
			continue;

		/* 跳过 stxdinherit 值不匹配的统计信息 */
		if (stat->inherit != rte->inh)
			continue;

		/*
		 * 统计匹配的属性：我们必须撤销 attnum 偏移。输入的
		 * 属性号没有偏移（表达式不
		 * 包含在 stat->keys 里，因此没有必要）。但在对照
		 * clauses_attnums 检查之前，我们需要先偏移它。
		 */
		nmatched = 0;
		k = -1;
		while ((k = bms_next_member(stat->keys, k)) >= 0)
		{
			AttrNumber	attnum = (AttrNumber) k;

			/* 跳过表达式 */
			if (!AttrNumberIsForUserDefinedAttr(attnum))
				continue;

			/* 应用与上面相同的偏移 */
			attnum += attnum_offset;

			if (bms_is_member(attnum, clauses_attnums))
				nmatched++;
		}

		/* 统计匹配的表达式 */
		nexprs = 0;
		for (i = 0; i < unique_exprs_cnt; i++)
		{
			ListCell   *lc;

			foreach(lc, stat->exprs)
			{
				Node	   *stat_expr = (Node *) lfirst(lc);

				/* 试着匹配它 */
				if (equal(stat_expr, unique_exprs[i]))
					nexprs++;
			}
		}

		/*
		 * 跳过匹配到的子句属性/表达式少于两个的
		 * 对象。
		 */
		if (nmatched + nexprs < 2)
			continue;

		deps = statext_dependencies_load(stat->statOid, rte->inh);

		/*
		 * 表达式在统计信息里可能由不同的 attnum
		 * 表示，我们需要重映射它们，使之与子句一致。
		 * 这会让后面的步骤（例如挑选最强的项
		 * 等等）简单得多也便宜得多，因为它完全不必关心
		 * 偏移。
		 *
		 * 既然如此，我们可以忽略没有被子句完全
		 * 匹配的依赖（也就是引用了不在子句里的
		 * 属性或表达式的那些）。
		 *
		 * 只要有任何表达式，我们就必须对所有统计信息做这件事，
		 * 因为我们需要移动所有依赖里的 attnum。
		 *
		 * XXX 也许应当总是做这件事，因为它也会提前消除一些
		 * 依赖。这可能比以后在 find_strongest_dependency 里走
		 * 更长的列表更便宜，尤其是
		 * 我们需要反复做这件事？
		 *
		 * XXX 即使子句里没有表达式，我们也必须做这件事，
		 * 否则对带表达式的统计信息，find_strongest_dependency 可能失败
		 * （因为在 bitmap 里查找负值）。因此我们
		 * 至少需要滤掉那些依赖。也许可以用
		 * 更便宜的方式做（若没有表达式子句，我们可以直接
		 * 丢掉所有负 attnum，而不做任何查找）。
		 */
		if (unique_exprs_cnt > 0 || stat->exprs != NIL)
		{
			int			ndeps = 0;

			for (i = 0; i < deps->ndeps; i++)
			{
				bool		skip = false;
				MVDependency *dep = deps->deps[i];
				int			j;

				for (j = 0; j < dep->nattributes; j++)
				{
					int			idx;
					Node	   *expr;
					AttrNumber	unique_attnum = InvalidAttrNumber;
					AttrNumber	attnum;

					/* 撤销按统计信息的偏移 */
					attnum = dep->attributes[j];

					/*
					 * 对普通属性，我们可以简单地检查它是否
					 * 匹配任何子句。若没有匹配的子句，就可以
					 * 直接忽略它。不过我们需要偏移
					 * attnum。
					 */
					if (AttrNumberIsForUserDefinedAttr(attnum))
					{
						dep->attributes[j] = attnum + attnum_offset;

						if (!bms_is_member(dep->attributes[j], clauses_attnums))
						{
							skip = true;
							break;
						}

						continue;
					}

					/*
					 * attnum 应当是有效的系统 attnum（-1、-2，
					 * ...)
					 */
					Assert(AttributeNumberIsValid(attnum));

					/*
					 * 对表达式，我们需要做两次翻译。首先
					 * 必须把负 attnum 翻译成表达式列表
					 * （在统计对象里）的下标。
					 * 然后需要看是否有匹配的子句。唯一
					 * 表达式的下标决定 attnum
					 * （并且我们把它偏移）。
					 */
					idx = -(1 + attnum);

					/* 表达式下标有效吗？ */
					Assert((idx >= 0) && (idx < list_length(stat->exprs)));

					expr = (Node *) list_nth(stat->exprs, idx);

					/* 试着在唯一列表里找出该表达式 */
					for (int m = 0; m < unique_exprs_cnt; m++)
					{
						/*
						 * 找到了匹配的唯一表达式，就使用该 attnum
						 * （由唯一表达式的下标导出）
						 */
						if (equal(unique_exprs[m], expr))
						{
							unique_attnum = -(m + 1) + attnum_offset;
							break;
						}
					}

					/*
					 * 没有找到匹配的表达式，因此可以直接跳过
					 * 这条依赖，因为它没有机会被
					 * 完全覆盖。
					 */
					if (unique_attnum == InvalidAttrNumber)
					{
						skip = true;
						break;
					}

					/* 否则就把它重映射到新的 attnum */
					dep->attributes[j] = unique_attnum;
				}

				/* 若找到了匹配的依赖，就保留它 */
				if (!skip)
				{
					/* 也许前面跳过了一些，因此把它移过来 */
					if (ndeps != i)
						deps->deps[ndeps] = deps->deps[i];

					ndeps++;
				}
			}

			deps->ndeps = ndeps;
		}

		/*
		 * 有可能我们已经去掉了全部依赖，那种情况下
		 * 就不必把它加入列表。
		 */
		if (deps->ndeps > 0)
		{
			func_dependencies[nfunc_dependencies] = deps;
			total_ndeps += deps->ndeps;
			nfunc_dependencies++;
		}
	}

	/* 若找不到匹配的统计信息，就没有事情可做 */
	if (nfunc_dependencies == 0)
	{
		pfree(func_dependencies);
		bms_free(clauses_attnums);
		pfree(list_attnums);
		pfree(unique_exprs);
		return 1.0;
	}

	/*
	 * 算出可以应用哪些依赖，从最宽/最强的
	 * 开始，再处理更小/更弱的。
	 */
	dependencies = (MVDependency **) palloc(sizeof(MVDependency *) *
											total_ndeps);
	ndependencies = 0;

	while (true)
	{
		MVDependency *dependency;
		AttrNumber	attnum;

		/* 最宽/最强、且被子句完全匹配的依赖 */
		dependency = find_strongest_dependency(func_dependencies,
											   nfunc_dependencies,
											   clauses_attnums);
		if (!dependency)
			break;

		dependencies[ndependencies++] = dependency;

		/* 在后面的循环里忽略使用这个被蕴含属性的依赖 */
		attnum = dependency->attributes[dependency->nattributes - 1];
		clauses_attnums = bms_del_member(clauses_attnums, attnum);
	}

	/*
	 * 若找到了可应用的依赖，就用它们来估计它们所引用属性上的
	 * 全部相容子句。
	 */
	if (ndependencies != 0)
		s1 = clauselist_apply_dependencies(root, clauses, varRelid, jointype,
										   sjinfo, dependencies, ndependencies,
										   list_attnums, estimatedclauses);

	/* 释放反序列化的函数依赖（然后释放该数组） */
	for (i = 0; i < nfunc_dependencies; i++)
		pfree(func_dependencies[i]);

	pfree(dependencies);
	pfree(func_dependencies);
	bms_free(clauses_attnums);
	pfree(list_attnums);
	pfree(unique_exprs);

	return s1;
}
