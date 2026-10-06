/*-------------------------------------------------------------------------
 *
 * pg_aggregate.c
 *	  支持操作 pg_aggregate 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_aggregate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static Oid	lookup_agg_function(List *fnName, int nargs, Oid *input_types,
								Oid variadicArgType,
								Oid *rettype);


/*
 * AggregateCreate
 */
ObjectAddress
AggregateCreate(const char *aggName,
				Oid aggNamespace,
				bool replace,
				char aggKind,
				int numArgs,
				int numDirectArgs,
				oidvector *parameterTypes,
				Datum allParameterTypes,
				Datum parameterModes,
				Datum parameterNames,
				List *parameterDefaults,
				Oid variadicArgType,
				List *aggtransfnName,
				List *aggfinalfnName,
				List *aggcombinefnName,
				List *aggserialfnName,
				List *aggdeserialfnName,
				List *aggmtransfnName,
				List *aggminvtransfnName,
				List *aggmfinalfnName,
				bool finalfnExtraArgs,
				bool mfinalfnExtraArgs,
				char finalfnModify,
				char mfinalfnModify,
				List *aggsortopName,
				Oid aggTransType,
				int32 aggTransSpace,
				Oid aggmTransType,
				int32 aggmTransSpace,
				const char *agginitval,
				const char *aggminitval,
				char proparallel)
{
	Relation	aggdesc;
	HeapTuple	tup;
	HeapTuple	oldtup;
	bool		nulls[Natts_pg_aggregate];
	Datum		values[Natts_pg_aggregate];
	bool		replaces[Natts_pg_aggregate];
	Form_pg_proc proc;
	Oid			transfn;
	Oid			finalfn = InvalidOid;	/* 可以省略 */
	Oid			combinefn = InvalidOid; /* 可以省略 */
	Oid			serialfn = InvalidOid;	/* 可以省略 */
	Oid			deserialfn = InvalidOid;	/* 可以省略 */
	Oid			mtransfn = InvalidOid;	/* 可以省略 */
	Oid			minvtransfn = InvalidOid;	/* 可以省略 */
	Oid			mfinalfn = InvalidOid;	/* 可以省略 */
	Oid			sortop = InvalidOid;	/* 可以省略 */
	Oid		   *aggArgTypes = parameterTypes->values;
	bool		mtransIsStrict = false;
	Oid			rettype;
	Oid			finaltype;
	Oid			fnArgs[FUNC_MAX_ARGS];
	int			nargs_transfn;
	int			nargs_finalfn;
	Oid			procOid;
	TupleDesc	tupDesc;
	char	   *detailmsg;
	int			i;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;
	AclResult	aclresult;

	/* 健全性检查（调用方本应已捕获这些） */
	if (!aggName)
		elog(ERROR, "no aggregate name supplied");

	if (!aggtransfnName)
		elog(ERROR, "aggregate must have a transition function");

	if (numDirectArgs < 0 || numDirectArgs > numArgs)
		elog(ERROR, "incorrect number of direct arguments for aggregate");

	/*
	 * 聚合最多可有 FUNC_MAX_ARGS-1 个参数，否则 transfn
	 * 和/或 finalfn 将无法在 pg_proc 中表示。现在必须检查，
	 * 以保护此处以及可能在被调函数中的固定大小数组。
	 */
	if (numArgs < 0 || numArgs > FUNC_MAX_ARGS - 1)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("aggregates cannot have more than %d argument",
							   "aggregates cannot have more than %d arguments",
							   FUNC_MAX_ARGS - 1,
							   FUNC_MAX_ARGS - 1)));

	/*
	 * 若 transtype 是多态的，也必须有多态参数；否则
	 * 我们将无法推导出实际的 transtype。
	 */
	detailmsg = check_valid_polymorphic_signature(aggTransType,
												  aggArgTypes,
												  numArgs);
	if (detailmsg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("cannot determine transition data type"),
				 errdetail_internal("%s", detailmsg)));

	/*
	 * 对移动聚合的 transtype（若有）同样如此
	 */
	if (OidIsValid(aggmTransType))
	{
		detailmsg = check_valid_polymorphic_signature(aggmTransType,
													  aggArgTypes,
													  numArgs);
		if (detailmsg)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("cannot determine transition data type"),
					 errdetail_internal("%s", detailmsg)));
	}

	/*
	 * 带 VARIADIC 的有序集聚合必须是 VARIADIC ANY。原则上
	 * 我们可以支持普通的可变参数类型，但这会使事情
	 * 复杂得多，因为我们得把正确的参数子集组装成
	 * 数组值。由于没有标准聚合需要这种情形，
	 * 目前我们就不费这个劲了。
	 */
	if (AGGKIND_IS_ORDERED_SET(aggKind) && OidIsValid(variadicArgType) &&
		variadicArgType != ANYOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a variadic ordered-set aggregate must use VARIADIC type ANY")));

	/*
	 * 若是假想集聚合，直接参数的数量必须至少与
	 * 聚合参数一样多，且最后 N 个直接参数
	 * 的类型必须与聚合参数匹配。（若涉及 ANY，在聚合被
	 * 调用时我们还得再查一次，但若已声明的参数类型对不上，
	 * 现在就拒绝聚合定义是合理的。）若
	 * numDirectArgs == numArgs，则无条件地可以，这表明文法
	 * 把两边相同的 VARIADIC 条目合并了。否则，若聚合是
	 * VARIADIC，则表示只在聚合侧有 VARIADIC，这是不允许的。
	 * 否则，要求两侧最后 N 个参数类型精确
	 * 匹配。
	 */
	if (aggKind == AGGKIND_HYPOTHETICAL &&
		numDirectArgs < numArgs)
	{
		int			numAggregatedArgs = numArgs - numDirectArgs;

		if (OidIsValid(variadicArgType) ||
			numDirectArgs < numAggregatedArgs ||
			memcmp(aggArgTypes + (numDirectArgs - numAggregatedArgs),
				   aggArgTypes + numDirectArgs,
				   numAggregatedArgs * sizeof(Oid)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("a hypothetical-set aggregate must have direct arguments matching its aggregated arguments")));
	}

	/*
	 * 查找 transfn。对普通聚合，它接受 transtype 加上所有
	 * 聚合参数。对有序集聚合，它接受 transtype 加上所有
	 * 聚合参数，但不含直接参数。不过，若尾随的
	 * VARIADIC 项被认为同时覆盖直接参数与聚合参数，
	 * 我们须特殊处理。
	 */
	if (AGGKIND_IS_ORDERED_SET(aggKind))
	{
		if (numDirectArgs < numArgs)
			nargs_transfn = numArgs - numDirectArgs + 1;
		else
		{
			/* 最后参数为 VARIADIC 的特殊情形 */
			Assert(variadicArgType != InvalidOid);
			nargs_transfn = 2;
		}
		fnArgs[0] = aggTransType;
		memcpy(fnArgs + 1, aggArgTypes + (numArgs - (nargs_transfn - 1)),
			   (nargs_transfn - 1) * sizeof(Oid));
	}
	else
	{
		nargs_transfn = numArgs + 1;
		fnArgs[0] = aggTransType;
		memcpy(fnArgs + 1, aggArgTypes, numArgs * sizeof(Oid));
	}
	transfn = lookup_agg_function(aggtransfnName, nargs_transfn,
								  fnArgs, variadicArgType,
								  &rettype);

	/*
	 * transfn 的返回类型（若 transtype 不是多态的，可能在
	 * enforce_generic_type_consistency 细化之后）必须
	 * 与声明的 transtype 精确匹配。
	 *
	 * 在非多态 transtype 情形下，或许允许与 transtype
	 * 二进制可强制转换的 rettype 也可以，但我不太确信
	 * 这既安全又有用。当 transtype 是
	 * 多态时，我们*必须*要求精确相等。
	 */
	if (rettype != aggTransType)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("return type of transition function %s is not %s",
						NameListToString(aggtransfnName),
						format_type_be(aggTransType))));

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(transfn));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", transfn);
	proc = (Form_pg_proc) GETSTRUCT(tup);

	/*
	 * 若 transfn 是 strict 且 initval 为 NULL，确保第一个输入
	 * 类型与 transtype 相同（或至少二进制兼容），以便
	 * 可以用第一个输入值作为初始 transValue。
	 */
	if (proc->proisstrict && agginitval == NULL)
	{
		if (numArgs < 1 ||
			!IsBinaryCoercible(aggArgTypes[0], aggTransType))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("must not omit initial value when transition function is strict and transition type is not compatible with input type")));
	}

	ReleaseSysCache(tup);

	/* 处理移动聚合的 transfn（若已提供） */
	if (aggmtransfnName)
	{
		/*
		 * 参数与常规 transfn 相同，只是
		 * 转移数据类型可能不同。因此复用上面设置的
		 * fnArgs 值，唯独那一项除外。
		 */
		Assert(OidIsValid(aggmTransType));
		fnArgs[0] = aggmTransType;

		mtransfn = lookup_agg_function(aggmtransfnName, nargs_transfn,
									   fnArgs, variadicArgType,
									   &rettype);

		/* 同上，返回类型必须与声明的 mtranstype 精确匹配。 */
		if (rettype != aggmTransType)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("return type of transition function %s is not %s",
							NameListToString(aggmtransfnName),
							format_type_be(aggmTransType))));

		tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(mtransfn));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for function %u", mtransfn);
		proc = (Form_pg_proc) GETSTRUCT(tup);

		/*
		 * 若 mtransfn 是 strict 且 minitval 为 NULL，检查第一个
		 * 输入类型与 mtranstype 是否二进制兼容。
		 */
		if (proc->proisstrict && aggminitval == NULL)
		{
			if (numArgs < 1 ||
				!IsBinaryCoercible(aggArgTypes[0], aggmTransType))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("must not omit initial value when transition function is strict and transition type is not compatible with input type")));
		}

		/* 记住 mtransfn 是否为 strict；下面可能需要 */
		mtransIsStrict = proc->proisstrict;

		ReleaseSysCache(tup);
	}

	/* 处理 minvtransfn（若已提供） */
	if (aggminvtransfnName)
	{
		/*
		 * 这必须与正向转移函数有相同数量、相同类型的参数，
		 * 因此直接复用 fnArgs 数据。
		 */
		Assert(aggmtransfnName);

		minvtransfn = lookup_agg_function(aggminvtransfnName, nargs_transfn,
										  fnArgs, variadicArgType,
										  &rettype);

		/* 同上，返回类型必须与声明的 mtranstype 精确匹配。 */
		if (rettype != aggmTransType)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("return type of inverse transition function %s is not %s",
							NameListToString(aggminvtransfnName),
							format_type_be(aggmTransType))));

		tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(minvtransfn));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for function %u", minvtransfn);
		proc = (Form_pg_proc) GETSTRUCT(tup);

		/*
		 * 我们需要正向与逆向转移函数的 strict 设置一致。
		 * 这样可避免在执行时处理各种
		 * 特殊情形。
		 */
		if (proc->proisstrict != mtransIsStrict)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("strictness of aggregate's forward and inverse transition functions must match")));

		ReleaseSysCache(tup);
	}

	/* 处理 finalfn（若已提供） */
	if (aggfinalfnName)
	{
		/*
		 * 若指定了 finalfnExtraArgs，则 transfn 接受 transtype
		 * 加上所有参数；否则只接受 transtype 加上任何
		 * 直接参数。（非直接参数在运行时无用，实际以 NULLs
		 * 传入，但为解析多态聚合的结果类型，我们可能需要它们
		 * 出现在函数签名中。）
		 */
		Oid			ffnVariadicArgType = variadicArgType;

		fnArgs[0] = aggTransType;
		memcpy(fnArgs + 1, aggArgTypes, numArgs * sizeof(Oid));
		if (finalfnExtraArgs)
			nargs_finalfn = numArgs + 1;
		else
		{
			nargs_finalfn = numDirectArgs + 1;
			if (numDirectArgs < numArgs)
			{
				/* 可变参数不影响 finalfn */
				ffnVariadicArgType = InvalidOid;
			}
		}

		finalfn = lookup_agg_function(aggfinalfnName, nargs_finalfn,
									  fnArgs, ffnVariadicArgType,
									  &finaltype);

		/*
		 * 当指定了 finalfnExtraArgs 时，finalfn 肯定会被
		 * 传入至少一个空参数，因此若它是 strict 则报错。
		 * 运行时不会有坏事发生（只会得到空结果），
		 * 但这肯定不是用户想要的，所以现在就抱怨。
		 */
		if (finalfnExtraArgs && func_strict(finalfn))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("final function with extra arguments must not be declared STRICT")));
	}
	else
	{
		/*
		 * 若无 finalfn，聚合结果类型即为状态值的类型
		 */
		finaltype = aggTransType;
	}
	Assert(OidIsValid(finaltype));

	/* 处理 combinefn（若已提供） */
	if (aggcombinefnName)
	{
		Oid			combineType;

		/*
		 * 合并函数必须有 2 个参数，每个都是转移
		 * 类型。VARIADIC 不影响它。
		 */
		fnArgs[0] = aggTransType;
		fnArgs[1] = aggTransType;

		combinefn = lookup_agg_function(aggcombinefnName, 2,
										fnArgs, InvalidOid,
										&combineType);

		/* 确保返回类型与聚合的转移类型匹配 */
		if (combineType != aggTransType)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("return type of combine function %s is not %s",
							NameListToString(aggcombinefnName),
							format_type_be(aggTransType))));

		/*
		 * 用于合并 INTERNAL 状态的合并函数必须接受空值，并
		 * 确保返回的状态处于正确的内存上下文。我们
		 * 无法直接检查后者，但可以检查前者。
		 */
		if (aggTransType == INTERNALOID && func_strict(combinefn))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("combine function with transition type %s must not be declared STRICT",
							format_type_be(aggTransType))));
	}

	/*
	 * 校验序列化函数（若存在）。
	 */
	if (aggserialfnName)
	{
		/* 签名始终为 serialize(internal) returns bytea */
		fnArgs[0] = INTERNALOID;

		serialfn = lookup_agg_function(aggserialfnName, 1,
									   fnArgs, InvalidOid,
									   &rettype);

		if (rettype != BYTEAOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("return type of serialization function %s is not %s",
							NameListToString(aggserialfnName),
							format_type_be(BYTEAOID))));
	}

	/*
	 * 校验反序列化函数（若存在）。
	 */
	if (aggdeserialfnName)
	{
		/* 签名始终为 deserialize(bytea, internal) returns internal */
		fnArgs[0] = BYTEAOID;
		fnArgs[1] = INTERNALOID;	/* 为类型安全准备的哑参数 */

		deserialfn = lookup_agg_function(aggdeserialfnName, 2,
										 fnArgs, InvalidOid,
										 &rettype);

		if (rettype != INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("return type of deserialization function %s is not %s",
							NameListToString(aggdeserialfnName),
							format_type_be(INTERNALOID))));
	}

	/*
	 * 若 finaltype（即聚合返回类型）是多态的，输入也必须
	 * 是多态的，否则解析器将无法推导结果类型。
	 * （注：鉴于先前对 transtype 与输入的测试，这不会
	 * 发生，除非有人把本身违反"无多态输入却有
	 * 多态结果"规则的 finalfn 定义偷偷塞进了
	 * 系统目录。）
	 */
	detailmsg = check_valid_polymorphic_signature(finaltype,
												  aggArgTypes,
												  numArgs);
	if (detailmsg)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot determine result data type"),
				 errdetail_internal("%s", detailmsg)));

	/*
	 * 另外，除非至少有一个 INTERNAL 参数，否则返回类型
	 * 不能是 INTERNAL。这与我们对普通函数强制的类型安全
	 * 限制相同，只是落在聚合层面。我们必须显式测试
	 * 这一点，因为我们允许 INTERNAL 作为 transtype。
	 */
	detailmsg = check_valid_internal_signature(finaltype,
											   aggArgTypes,
											   numArgs);
	if (detailmsg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("unsafe use of pseudo-type \"internal\""),
				 errdetail_internal("%s", detailmsg)));

	/*
	 * 若提供了移动聚合实现，查找其 finalfn（若有），
	 * 并检查隐含的聚合结果类型是否与
	 * 普通实现匹配。
	 */
	if (OidIsValid(aggmTransType))
	{
		/* 处理 finalfn（若已提供） */
		if (aggmfinalfnName)
		{
			/*
			 * 参数的确定方式与常规 finalfn 相同，
			 * 但使用 aggmTransType 与 mfinalfnExtraArgs。
			 */
			Oid			ffnVariadicArgType = variadicArgType;

			fnArgs[0] = aggmTransType;
			memcpy(fnArgs + 1, aggArgTypes, numArgs * sizeof(Oid));
			if (mfinalfnExtraArgs)
				nargs_finalfn = numArgs + 1;
			else
			{
				nargs_finalfn = numDirectArgs + 1;
				if (numDirectArgs < numArgs)
				{
					/* 可变参数不影响 finalfn */
					ffnVariadicArgType = InvalidOid;
				}
			}

			mfinalfn = lookup_agg_function(aggmfinalfnName, nargs_finalfn,
										   fnArgs, ffnVariadicArgType,
										   &rettype);

			/* 同上，若给出了 mfinalfnExtraArgs 则检查 strict 性 */
			if (mfinalfnExtraArgs && func_strict(mfinalfn))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("final function with extra arguments must not be declared STRICT")));
		}
		else
		{
			/*
			 * 若无 finalfn，聚合结果类型即为状态值的类型
			 */
			rettype = aggmTransType;
		}
		Assert(OidIsValid(rettype));
		if (rettype != finaltype)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("moving-aggregate implementation returns type %s, but plain implementation returns type %s",
							format_type_be(rettype),
							format_type_be(finaltype))));
	}

	/* 处理 sortop（若已提供） */
	if (aggsortopName)
	{
		if (numArgs != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("sort operator can only be specified for single-argument aggregates")));
		sortop = LookupOperName(NULL, aggsortopName,
								aggArgTypes[0], aggArgTypes[0],
								false, -1);
	}

	/*
	 * 对所用类型做权限检查
	 */
	for (i = 0; i < numArgs; i++)
	{
		aclresult = object_aclcheck(TypeRelationId, aggArgTypes[i], GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, aggArgTypes[i]);
	}

	aclresult = object_aclcheck(TypeRelationId, aggTransType, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, aggTransType);

	if (OidIsValid(aggmTransType))
	{
		aclresult = object_aclcheck(TypeRelationId, aggmTransType, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, aggmTransType);
	}

	aclresult = object_aclcheck(TypeRelationId, finaltype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, finaltype);


	/*
	 * 一切看起来没问题。尝试为该聚合创建 pg_proc
	 * 条目。（若已有冲突条目，这可能失败。）
	 */

	myself = ProcedureCreate(aggName,
							 aggNamespace,
							 replace,	/* 可能是替换 */
							 false, /* 不返回集合 */
							 finaltype, /* returnType */
							 GetUserId(),	/* proowner */
							 INTERNALlanguageId,	/* languageObjectId */
							 InvalidOid,	/* 无校验器 */
							 "aggregate_dummy", /* 占位（无此 proc） */
							 NULL,	/* probin */
							 NULL,	/* prosqlbody */
							 PROKIND_AGGREGATE,
							 false, /* security invoker（目前不可
									 * 为聚合定义） */
							 false, /* isLeakProof */
							 false, /* isStrict（聚合不需要） */
							 PROVOLATILE_IMMUTABLE, /* volatility（聚合不需要
													 * ） */
							 proparallel,
							 parameterTypes,	/* paramTypes */
							 allParameterTypes, /* allParamTypes */
							 parameterModes,	/* parameterModes */
							 parameterNames,	/* parameterNames */
							 parameterDefaults, /* parameterDefaults */
							 PointerGetDatum(NULL), /* trftypes */
							 PointerGetDatum(NULL), /* proconfig */
							 InvalidOid,	/* 无 prosupport */
							 1, /* procost */
							 0);	/* prorows */
	procOid = myself.objectId;

	/*
	 * 可以创建 pg_aggregate 条目了。
	 */
	aggdesc = table_open(AggregateRelationId, RowExclusiveLock);
	tupDesc = aggdesc->rd_att;

	/* 初始化 nulls 与 values */
	for (i = 0; i < Natts_pg_aggregate; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
		replaces[i] = true;
	}
	values[Anum_pg_aggregate_aggfnoid - 1] = ObjectIdGetDatum(procOid);
	values[Anum_pg_aggregate_aggkind - 1] = CharGetDatum(aggKind);
	values[Anum_pg_aggregate_aggnumdirectargs - 1] = Int16GetDatum(numDirectArgs);
	values[Anum_pg_aggregate_aggtransfn - 1] = ObjectIdGetDatum(transfn);
	values[Anum_pg_aggregate_aggfinalfn - 1] = ObjectIdGetDatum(finalfn);
	values[Anum_pg_aggregate_aggcombinefn - 1] = ObjectIdGetDatum(combinefn);
	values[Anum_pg_aggregate_aggserialfn - 1] = ObjectIdGetDatum(serialfn);
	values[Anum_pg_aggregate_aggdeserialfn - 1] = ObjectIdGetDatum(deserialfn);
	values[Anum_pg_aggregate_aggmtransfn - 1] = ObjectIdGetDatum(mtransfn);
	values[Anum_pg_aggregate_aggminvtransfn - 1] = ObjectIdGetDatum(minvtransfn);
	values[Anum_pg_aggregate_aggmfinalfn - 1] = ObjectIdGetDatum(mfinalfn);
	values[Anum_pg_aggregate_aggfinalextra - 1] = BoolGetDatum(finalfnExtraArgs);
	values[Anum_pg_aggregate_aggmfinalextra - 1] = BoolGetDatum(mfinalfnExtraArgs);
	values[Anum_pg_aggregate_aggfinalmodify - 1] = CharGetDatum(finalfnModify);
	values[Anum_pg_aggregate_aggmfinalmodify - 1] = CharGetDatum(mfinalfnModify);
	values[Anum_pg_aggregate_aggsortop - 1] = ObjectIdGetDatum(sortop);
	values[Anum_pg_aggregate_aggtranstype - 1] = ObjectIdGetDatum(aggTransType);
	values[Anum_pg_aggregate_aggtransspace - 1] = Int32GetDatum(aggTransSpace);
	values[Anum_pg_aggregate_aggmtranstype - 1] = ObjectIdGetDatum(aggmTransType);
	values[Anum_pg_aggregate_aggmtransspace - 1] = Int32GetDatum(aggmTransSpace);
	if (agginitval)
		values[Anum_pg_aggregate_agginitval - 1] = CStringGetTextDatum(agginitval);
	else
		nulls[Anum_pg_aggregate_agginitval - 1] = true;
	if (aggminitval)
		values[Anum_pg_aggregate_aggminitval - 1] = CStringGetTextDatum(aggminitval);
	else
		nulls[Anum_pg_aggregate_aggminitval - 1] = true;

	if (replace)
		oldtup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(procOid));
	else
		oldtup = NULL;

	if (HeapTupleIsValid(oldtup))
	{
		Form_pg_aggregate oldagg = (Form_pg_aggregate) GETSTRUCT(oldtup);

		/*
		 * 若我们在替换已有条目，需要校验我们没有
		 * 更改任何会破坏调用方的内容。具体而言
		 * 不得更改 aggkind 或 aggnumdirectargs，它们影响
		 * 解析分析中如何对待聚合调用。
		 */
		if (aggKind != oldagg->aggkind)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change routine kind"),
					 (oldagg->aggkind == AGGKIND_NORMAL ?
					  errdetail("\"%s\" is an ordinary aggregate function.", aggName) :
					  oldagg->aggkind == AGGKIND_ORDERED_SET ?
					  errdetail("\"%s\" is an ordered-set aggregate.", aggName) :
					  oldagg->aggkind == AGGKIND_HYPOTHETICAL ?
					  errdetail("\"%s\" is a hypothetical-set aggregate.", aggName) :
					  0)));
		if (numDirectArgs != oldagg->aggnumdirectargs)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("cannot change number of direct arguments of an aggregate function")));

		replaces[Anum_pg_aggregate_aggfnoid - 1] = false;
		replaces[Anum_pg_aggregate_aggkind - 1] = false;
		replaces[Anum_pg_aggregate_aggnumdirectargs - 1] = false;

		tup = heap_modify_tuple(oldtup, tupDesc, values, nulls, replaces);
		CatalogTupleUpdate(aggdesc, &tup->t_self, tup);
		ReleaseSysCache(oldtup);
	}
	else
	{
		tup = heap_form_tuple(tupDesc, values, nulls);
		CatalogTupleInsert(aggdesc, tup);
	}

	table_close(aggdesc, RowExclusiveLock);

	/*
	 * 为聚合创建依赖（超出 ProcedureCreate 已建立的
	 * 那些）。注：我们不需要显式依赖
	 * aggTransType，因为我们通过 transfn 间接依赖它。
	 * 对 aggmTransType 通过 mtransfn（若存在）同样如此。
	 *
	 * 若我们在替换已有定义，ProcedureCreate 删除了我们
	 * 所有已有依赖，因此无论哪种情况我们在这里都要做
	 * 同样的事情。
	 */

	addrs = new_object_addresses();

	/* 依赖于转移函数 */
	ObjectAddressSet(referenced, ProcedureRelationId, transfn);
	add_exact_object_address(&referenced, addrs);

	/* 依赖于最终函数（若有） */
	if (OidIsValid(finalfn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, finalfn);
		add_exact_object_address(&referenced, addrs);
	}

	/* 依赖于合并函数（若有） */
	if (OidIsValid(combinefn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, combinefn);
		add_exact_object_address(&referenced, addrs);
	}

	/* 依赖于序列化函数（若有） */
	if (OidIsValid(serialfn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, serialfn);
		add_exact_object_address(&referenced, addrs);
	}

	/* 依赖于反序列化函数（若有） */
	if (OidIsValid(deserialfn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, deserialfn);
		add_exact_object_address(&referenced, addrs);
	}

	/* 依赖于正向转移函数（若有） */
	if (OidIsValid(mtransfn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, mtransfn);
		add_exact_object_address(&referenced, addrs);
	}

	/* 依赖于逆向转移函数（若有） */
	if (OidIsValid(minvtransfn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, minvtransfn);
		add_exact_object_address(&referenced, addrs);
	}

	/* 依赖于最终函数（若有） */
	if (OidIsValid(mfinalfn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, mfinalfn);
		add_exact_object_address(&referenced, addrs);
	}

	/* 依赖于排序操作符（若有） */
	if (OidIsValid(sortop))
	{
		ObjectAddressSet(referenced, OperatorRelationId, sortop);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);
	return myself;
}

/*
 * lookup_agg_function
 * 查找聚合支持函数的公共代码
 *
 * fnName: 可能带模式限定的函数名
 * nargs, input_types: 期望的函数参数类型
 * variadicArgType: 可变参数类型（若有），否则为 InvalidOid
 *
 * 返回函数的 OID，并将其返回类型存入 *rettype
 *
 * NB: 不得涂改 input_types[]，因为我们可能复用那些
 */
static Oid
lookup_agg_function(List *fnName,
					int nargs,
					Oid *input_types,
					Oid variadicArgType,
					Oid *rettype)
{
	Oid			fnOid;
	bool		retset;
	int			nvargs;
	Oid			vatype;
	Oid		   *true_oid_array;
	FuncDetailCode fdresult;
	AclResult	aclresult;
	int			i;

	/*
	 * func_get_detail 在系统目录中查找函数，对多态函数做
	 * 消歧，处理继承，并返回函数返回值的
	 * funcid、类型以及集合或单值状态。它还
	 * 返回函数的真实参数
	 * 类型。
	 */
	fdresult = func_get_detail(fnName, NIL, NIL,
							   nargs, input_types, false, false, false,
							   &fnOid, rettype, &retset,
							   &nvargs, &vatype,
							   &true_oid_array, NULL);

	/* 唯一有效情形是不返回集合的普通函数 */
	if (fdresult != FUNCDETAIL_NORMAL || !OidIsValid(fnOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(fnName, nargs,
											  NIL, input_types))));
	if (retset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function %s returns a set",
						func_signature_string(fnName, nargs,
											  NIL, input_types))));

	/*
	 * 若聚合声明为接受 VARIADIC ANY，底层函数
	 * 最好也如此声明，否则它们可能收到过多
	 * 参数；但 func_get_detail 会对普通 ANY 感到满意。
	 * （大概不会发生很糟的事，但不会按用户期望工作。）
	 * 鉴于我们告诉 func_get_detail 不要展开 VARIADIC，
	 * 其他组合应无需特殊处理即可工作。
	 */
	if (variadicArgType == ANYOID && vatype != ANYOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function %s must accept VARIADIC ANY to be used in this aggregate",
						func_signature_string(fnName, nargs,
											  NIL, input_types))));

	/*
	 * 若涉及任何多态类型，强制一致性，并
	 * 可能细化结果类型。此时结果仍为
	 * 多态是可以的。
	 */
	*rettype = enforce_generic_type_consistency(input_types,
												true_oid_array,
												nargs,
												*rettype,
												true);

	/*
	 * func_get_detail 会找到需要运行时参数类型
	 * 强制转换的函数，但 nodeAgg.c 尚未准备好处理那种情况
	 */
	for (i = 0; i < nargs; i++)
	{
		if (!IsBinaryCoercible(input_types[i], true_oid_array[i]))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function %s requires run-time type coercion",
							func_signature_string(fnName, nargs,
												  NIL, true_oid_array))));
	}

	/* 检查聚合创建者是否有调用该函数的权限 */
	aclresult = object_aclcheck(ProcedureRelationId, fnOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(fnOid));

	return fnOid;
}
