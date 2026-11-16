/*-------------------------------------------------------------------------
 *
 * domains.c
 *	  域类型的 I/O 函数。
 *
 * 域类型的输出函数就是由其底层基础类型所提供的
 * 同一批函数。但输入函数则必须准备好应用该
 * 类型所定义的任何约束。因此我们创建了特殊的
 * 输入函数，它们会调用基础类型的输入函数，
 * 然后检查约束。
 *
 * 约束检查所需的开销可能很高，因为翻查目录来找出
 * 给定域的约束并不便宜。以下三种机制可
 * 把这一开销降至最低：
 *	依赖 typcache 保存约束的最新副本。
 *	在域的嵌套中，我们把所有层级的检查
 *		摊平成仅仅一次操作（typcache 代劳）。
 *	若存在 CHECK 约束，则缓存独立 ExprContext
 *		供其求值。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/domains.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * 跨多次调用缓存状态的结构
 */
typedef struct DomainIOData
{
	Oid			domain_type;
	/* 调用基础类型输入函数所需的数据 */
	Oid			typiofunc;
	Oid			typioparam;
	int32		typtypmod;
	FmgrInfo	proc;
	/* 指向已缓存待检查约束项列表的引用 */
	DomainConstraintRef constraint_ref;
	/* 用于求值 CHECK 约束的上下文 */
	ExprContext *econtext;
	/* 缓存所在内存上下文 */
	MemoryContext mcxt;
} DomainIOData;


/*
 * domain_state_setup - 为新域类型初始化缓存。
 *
 * 注意：不能为新域类型复用同一个缓存结构体，
 * 因为没有提供释放 DomainConstraintRef 的机制。
 * 若调用点需要处理新域类型，只需在查询期间
 * 泄漏旧结构体。
 */
static DomainIOData *
domain_state_setup(Oid domainType, bool binary, MemoryContext mcxt)
{
	DomainIOData *my_extra;
	TypeCacheEntry *typentry;
	Oid			baseType;

	my_extra = (DomainIOData *) MemoryContextAlloc(mcxt, sizeof(DomainIOData));

	/*
	 * 验证 domainType 表示的是有效的域类型。这里必须
	 * 小心，因为 domain_in 与 domain_recv 可能从 SQL 中被
	 * 调用，且参数可能不正确。我们使用 lookup_type_cache
	 * 主要是它会对错误的 OID 抛出友好的用户错误；但它
	 * 还能缓存底层基础类型的信息。
	 */
	typentry = lookup_type_cache(domainType, TYPECACHE_DOMAIN_BASE_INFO);
	if (typentry->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type %s is not a domain",
						format_type_be(domainType))));

	/* 找出基础类型 */
	baseType = typentry->domainBaseType;
	my_extra->typtypmod = typentry->domainBaseTypmod;

	/* 查找底层 I/O 函数 */
	if (binary)
		getTypeBinaryInputInfo(baseType,
							   &my_extra->typiofunc,
							   &my_extra->typioparam);
	else
		getTypeInputInfo(baseType,
						 &my_extra->typiofunc,
						 &my_extra->typioparam);
	fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc, mcxt);

	/* 查找域的约束 */
	InitDomainConstraintRef(domainType, &my_extra->constraint_ref, mcxt, true);

	/* 直到需要时才创建 ExprContext */
	my_extra->econtext = NULL;
	my_extra->mcxt = mcxt;

	/* 置有效标志 */
	my_extra->domain_type = domainType;

	return my_extra;
}

/*
 * domain_check_input - 应用已缓存检查。
 *
 * 这大致类似于 execExpr*.c 中对 CoerceToDomain 节点的
 * 处理，但我们逐个单独执行各约束，而不是把它们
 * 内联编译到更大的表达式中。
 *
 * 若 escontext 指向 ErrorSaveContext，则任何失败都报告
 * 到那里，否则以 ereport 报告。注意我们并不尝试对
 * 执行 CHECK 约束期间引发的错误做软报告。
 */
static void
domain_check_input(Datum value, bool isnull, DomainIOData *my_extra,
				   Node *escontext)
{
	ExprContext *econtext = my_extra->econtext;
	ListCell   *l;

	/* 确保我们持有最新约束 */
	UpdateDomainConstraintRef(&my_extra->constraint_ref);

	foreach(l, my_extra->constraint_ref.constraints)
	{
		DomainConstraintState *con = (DomainConstraintState *) lfirst(l);

		switch (con->constrainttype)
		{
			case DOM_CONSTRAINT_NOTNULL:
				if (isnull)
				{
					errsave(escontext,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("domain %s does not allow null values",
									format_type_be(my_extra->domain_type)),
							 errdatatype(my_extra->domain_type)));
					goto fail;
				}
				break;
			case DOM_CONSTRAINT_CHECK:
				{
					/* 若尚未创建则创建 econtext */
					if (econtext == NULL)
					{
						MemoryContext oldcontext;

						oldcontext = MemoryContextSwitchTo(my_extra->mcxt);
						econtext = CreateStandaloneExprContext();
						MemoryContextSwitchTo(oldcontext);
						my_extra->econtext = econtext;
					}

					/*
					 * 设置 CoerceToDomain 节点要返回的值。与
					 * 通用表达式情形不同，此 econtext 无法
					 * 与任何东西共享，不必保存恢复字段。
					 * 但我们确需保护传入的值不被被调函数
					 * 改动。对大多数用途而言它不可能是
					 * 读写扩展对象，但对 domain_check()
					 * 而言这似乎也是
					 * 可能的。
					 */
					econtext->domainValue_datum =
						MakeExpandedObjectReadOnly(value, isnull,
												   my_extra->constraint_ref.tcache->typlen);
					econtext->domainValue_isNull = isnull;

					if (!ExecCheck(con->check_exprstate, econtext))
					{
						errsave(escontext,
								(errcode(ERRCODE_CHECK_VIOLATION),
								 errmsg("value for domain %s violates check constraint \"%s\"",
										format_type_be(my_extra->domain_type),
										con->name),
								 errdomainconstraint(my_extra->domain_type,
													 con->name)));
						goto fail;
					}
					break;
				}
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->constrainttype);
				break;
		}
	}

	/*
	 * 退出前，调用所有关闭回调并重置 econtext 的
	 * 元组内存。这避免泄漏非内存资源——若表达
	 * 式中有任何此类资源。
	 */
fail:
	if (econtext)
		ReScanExprContext(econtext);
}


/*
 * domain_in		- 任意域类型的输入例程。
 */
Datum
domain_in(PG_FUNCTION_ARGS)
{
	char	   *string;
	Oid			domainType;
	Node	   *escontext = fcinfo->context;
	DomainIOData *my_extra;
	Datum		value;

	/*
	 * 由于 domain_in 不是严格的，我们必须检查空输入。
	 * 正常系统使用中 typioparam 参数绝不应为 null，但在
	 * 手工调用时可能为 null——若是，直接返回 null。
	 */
	if (PG_ARGISNULL(0))
		string = NULL;
	else
		string = PG_GETARG_CSTRING(0);
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	domainType = PG_GETARG_OID(1);

	/*
	 * 我们安排为整段调用只查找一次所需的信息，
	 * 并假定域类型不会在我们脚下改变（这确实不应
	 * 发生，若发生亦能应对）。
	 */
	my_extra = (DomainIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL || my_extra->domain_type != domainType)
	{
		my_extra = domain_state_setup(domainType, false,
									  fcinfo->flinfo->fn_mcxt);
		fcinfo->flinfo->fn_extra = (void *) my_extra;
	}

	/*
	 * 调用基础类型的 typinput 过程来转换数据。
	 */
	if (!InputFunctionCallSafe(&my_extra->proc,
							   string,
							   my_extra->typioparam,
							   my_extra->typtypmod,
							   escontext,
							   &value))
		PG_RETURN_NULL();

	/*
	 * 执行必要的检查以确保它是有效的域值。
	 */
	domain_check_input(value, (string == NULL), my_extra, escontext);

	if (string == NULL)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(value);
}

/*
 * domain_recv		- 域类型的二进制输入例程。
 */
Datum
domain_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf;
	Oid			domainType;
	DomainIOData *my_extra;
	Datum		value;

	/*
	 * 由于 domain_recv 不是严格的，我们必须检查空输入。
	 * 正常系统使用中 typioparam 参数绝不应为 null，但在
	 * 手工调用时可能为 null——若是，直接返回 null。
	 */
	if (PG_ARGISNULL(0))
		buf = NULL;
	else
		buf = (StringInfo) PG_GETARG_POINTER(0);
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	domainType = PG_GETARG_OID(1);

	/*
	 * 我们安排为整段调用只查找一次所需的信息，
	 * 并假定域类型不会在我们脚下改变（这确实不应
	 * 发生，若发生亦能应对）。
	 */
	my_extra = (DomainIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL || my_extra->domain_type != domainType)
	{
		my_extra = domain_state_setup(domainType, true,
									  fcinfo->flinfo->fn_mcxt);
		fcinfo->flinfo->fn_extra = (void *) my_extra;
	}

	/*
	 * 调用基础类型的 typreceive 过程来转换数据。
	 */
	value = ReceiveFunctionCall(&my_extra->proc,
								buf,
								my_extra->typioparam,
								my_extra->typtypmod);

	/*
	 * 执行必要的检查以确保它是有效的域值。
	 */
	domain_check_input(value, (buf == NULL), my_extra, NULL);

	if (buf == NULL)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(value);
}

/*
 * domain_check - 检查 datum 是否满足某个域的约束。
 * 若 extra 与 mcxt 可从诸如 FmgrInfo 结构之类的来源
 * 获得，则可传入，否则可传 NULL，此时每次调用都
 * 要重复一次设置。
 */
void
domain_check(Datum value, bool isnull, Oid domainType,
			 void **extra, MemoryContext mcxt)
{
	DomainIOData *my_extra = NULL;

	if (mcxt == NULL)
		mcxt = CurrentMemoryContext;

	/*
	 * 我们安排为整段调用只查找一次所需的信息，
	 * 并假定域类型不会在我们脚下改变（这确实不应
	 * 发生，若发生亦能应对）。
	 */
	if (extra)
		my_extra = (DomainIOData *) *extra;
	if (my_extra == NULL || my_extra->domain_type != domainType)
	{
		my_extra = domain_state_setup(domainType, true, mcxt);
		if (extra)
			*extra = (void *) my_extra;
	}

	/*
	 * 执行必要的检查以确保它是有效的域值。
	 */
	domain_check_input(value, isnull, my_extra, NULL);
}

/*
 * errdatatype --- 在当前 errordata 中保存数据类型
 * 的模式名与类型名。
 */
int
errdatatype(Oid datatypeOid)
{
	HeapTuple	tup;
	Form_pg_type typtup;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(datatypeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", datatypeOid);
	typtup = (Form_pg_type) GETSTRUCT(tup);

	err_generic_string(PG_DIAG_SCHEMA_NAME,
					   get_namespace_name(typtup->typnamespace));
	err_generic_string(PG_DIAG_DATATYPE_NAME, NameStr(typtup->typname));

	ReleaseSysCache(tup);

	return 0;					/* 返回值无关紧要 */
}

/*
 * errdomainconstraint --- 在当前 errordata 中保存
 * 域相关约束的模式名、类型名与约束名。
 */
int
errdomainconstraint(Oid datatypeOid, const char *conname)
{
	errdatatype(datatypeOid);
	err_generic_string(PG_DIAG_CONSTRAINT_NAME, conname);

	return 0;					/* 返回值无关紧要 */
}
