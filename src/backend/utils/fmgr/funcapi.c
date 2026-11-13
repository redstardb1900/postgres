/*-------------------------------------------------------------------------
 *
 * funcapi.c
 *	  为 fmgr 函数提供实用与便利函数，返回集合和/
 *	  或复合类型，或处理 VARIADIC 输入。
 *
 * Copyright (c) 2002-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/fmgr/funcapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"


typedef struct polymorphic_actuals
{
	Oid			anyelement_type;	/* 若已知 anyelement 映射 */
	Oid			anyarray_type;	/* 若已知 anyarray 映射 */
	Oid			anyrange_type;	/* 若已知 anyrange 映射 */
	Oid			anymultirange_type; /* 若已知 anymultirange 映射 */
} polymorphic_actuals;

static void shutdown_MultiFuncCall(Datum arg);
static TypeFuncClass internal_get_result_type(Oid funcid,
											  Node *call_expr,
											  ReturnSetInfo *rsinfo,
											  Oid *resultTypeId,
											  TupleDesc *resultTupleDesc);
static void resolve_anyelement_from_others(polymorphic_actuals *actuals);
static void resolve_anyarray_from_others(polymorphic_actuals *actuals);
static void resolve_anyrange_from_others(polymorphic_actuals *actuals);
static void resolve_anymultirange_from_others(polymorphic_actuals *actuals);
static bool resolve_polymorphic_tupdesc(TupleDesc tupdesc,
										oidvector *declared_args,
										Node *call_expr);
static TypeFuncClass get_type_func_class(Oid typid, Oid *base_typeid);


/*
 * InitMaterializedSRF
 *
 * 以物化模式在单次调用语境下构建结果集返回函数
 * 状态的辅助函数。此代码对 ReturnSetInfo 做
 * 完整性检查，并创建该函数使用的 Tuplestore 与
 * TupleDesc，并将它们存入函数的
 * ReturnSetInfo。
 *
 * "flags" 可设置为 MAT_SRF_USE_EXPECTED_DESC，
 * 以使用来自 expectedDesc 的元组描述符，即调用者
 * 期望的元组描述符。MAT_SRF_BLESS 可置位以补全
 * 元组描述符的相关信息，某些情况下这是必需的
 * 即当元组描述符来自瞬态的
 * RECORD类型。
 */
void
InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags)
{
	bool		random_access;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	MemoryContext old_context,
				per_query_ctx;
	TupleDesc	stored_tupdesc;

	/* 检查调用者是否支持返回 tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		((flags & MAT_SRF_USE_EXPECTED_DESC) != 0 && rsinfo->expectedDesc == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/*
	 * 将 tuplestore 与元组描述符存入 ReturnSetInfo，
	 * 须在每查询内存上下文中进行
	 */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	old_context = MemoryContextSwitchTo(per_query_ctx);

	/* 为我们结果类型构建元组描述符 */
	if ((flags & MAT_SRF_USE_EXPECTED_DESC) != 0)
		stored_tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	else
	{
		if (get_call_result_type(fcinfo, NULL, &stored_tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
	}

	/* 如请求则 bless 元组描述符 */
	if ((flags & MAT_SRF_BLESS) != 0)
		BlessTupleDesc(stored_tupdesc);

	random_access = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;

	tupstore = tuplestore_begin_heap(random_access, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = stored_tupdesc;
	MemoryContextSwitchTo(old_context);
}


/*
 * init_MultiFuncCall
 * 创建空的 FuncCallContext 数据结构
 * 并做一些其他基本的多函数调用设置
 * 与错误检查
 */
FuncCallContext *
init_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval;

	/*
	 * 若在错误语境中被调用则中止
	 */
	if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/*
		 * 首轮
		 */
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
		MemoryContext multi_call_ctx;

		/*
		 * 创建足够长寿的上下文来保存跨调用数据
		 */
		multi_call_ctx = AllocSetContextCreate(fcinfo->flinfo->fn_mcxt,
											   "SRF multi-call context",
											   ALLOCSET_SMALL_SIZES);

		/*
		 * 分配足够长寿的空间并将其清零
		 */
		retval = (FuncCallContext *)
			MemoryContextAllocZero(multi_call_ctx,
								   sizeof(FuncCallContext));

		/*
		 * 初始化各元素
		 */
		retval->call_cntr = 0;
		retval->max_calls = 0;
		retval->user_fctx = NULL;
		retval->attinmeta = NULL;
		retval->tuple_desc = NULL;
		retval->multi_call_memory_ctx = multi_call_ctx;

		/*
		 * 保存该指针供跨调用使用
		 */
		fcinfo->flinfo->fn_extra = retval;

		/*
		 * 确保当 exprcontext 未运行到完成时也能干净地
		 * 完成。
		 */
		RegisterExprContextCallback(rsi->econtext,
									shutdown_MultiFuncCall,
									PointerGetDatum(fcinfo->flinfo));
	}
	else
	{
		/* 第二次及后续调用 */
		elog(ERROR, "init_MultiFuncCall cannot be called more than once");

		/* 不会到此，但让编译器满意 */
		retval = NULL;
	}

	return retval;
}

/*
 * per_MultiFuncCall
 *
 * 多函数逐调用设置
 */
FuncCallContext *
per_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval = (FuncCallContext *) fcinfo->flinfo->fn_extra;

	return retval;
}

/*
 * end_MultiFuncCall
 * init_MultiFuncCall 后清理
 */
void
end_MultiFuncCall(PG_FUNCTION_ARGS, FuncCallContext *funcctx)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	/* 注销关闭回调 */
	UnregisterExprContextCallback(rsi->econtext,
								  shutdown_MultiFuncCall,
								  PointerGetDatum(fcinfo->flinfo));

	/* 但用它来做真正的工作 */
	shutdown_MultiFuncCall(PointerGetDatum(fcinfo->flinfo));
}

/*
 * shutdown_MultiFuncCall
 * 关闭函数，清理 init_MultiFuncCall 的产物
 */
static void
shutdown_MultiFuncCall(Datum arg)
{
	FmgrInfo   *flinfo = (FmgrInfo *) DatumGetPointer(arg);
	FuncCallContext *funcctx = (FuncCallContext *) flinfo->fn_extra;

	/* 从 flinfo 解绑 */
	flinfo->fn_extra = NULL;

	/*
	 * 删除持有全部多调用数据的上下文，含
	 * FuncCallContext
	 */
	MemoryContextDelete(funcctx->multi_call_memory_ctx);
}


/*
 * get_call_result_type
 *		给定函数的调用信息记录，判定其应返回的
 *		数据类型种类。若 resultTypeId 非 NULL，则
 *		*resultTypeId 收到实际数据类型 OID（这主要
 *		对标量结果类型有用）。若 resultTupleDesc 不为
 *		NULL，则当结果为复合类型时 *resultTupleDesc
 *		收到 TupleDesc，标量则为 NULL。
 *
 * 此处处理的一个难点是解析返回 RECORD 的函数的
 * 实际行类型（来源为函数的 OUT 参数列表，或
 * ReturnSetInfo 上下文节点）。仅当我们因信息
 * 不足而无法解析实际行类型时才返回 TYPEFUNC_RECORD
 *
 * 此处处理的另一难点是多态解析。我们绝不会
 * 返回多态伪类型（ANYELEMENT 等），无论是作为
 * 标量结果类型还是行类型的组成部分
 *
 * 此函数开销相对较大 --- 在返回集合的函数中，
 * 尽量只在首次进入时调用一次
 */
TypeFuncClass
get_call_result_type(FunctionCallInfo fcinfo,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	return internal_get_result_type(fcinfo->flinfo->fn_oid,
									fcinfo->flinfo->fn_expr,
									(ReturnSetInfo *) fcinfo->resultinfo,
									resultTypeId,
									resultTupleDesc);
}

/*
 * get_expr_result_type
 *		同上，但从调用表达式节点树入手
 *
 * 注意：勿对带 coldeflist 的 RTE 使用其 funcexpr
 * 在此类情形下，正确的结论总是：函数返回 RECORD，
 * 且其列由 coldeflist 字段（funccolnames 等）定义
 * 若结论不同，则由执行器负责在运行时捕获这种差异；
 * 但在该时点之前处理查询的代码若检查实际表达式，
 * 可能会得出不一致的结论
 */
TypeFuncClass
get_expr_result_type(Node *expr,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	TypeFuncClass result;

	if (expr && IsA(expr, FuncExpr))
		result = internal_get_result_type(((FuncExpr *) expr)->funcid,
										  expr,
										  NULL,
										  resultTypeId,
										  resultTupleDesc);
	else if (expr && IsA(expr, OpExpr))
		result = internal_get_result_type(get_opcode(((OpExpr *) expr)->opno),
										  expr,
										  NULL,
										  resultTypeId,
										  resultTupleDesc);
	else if (expr && IsA(expr, RowExpr) &&
			 ((RowExpr *) expr)->row_typeid == RECORDOID)
	{
		/* 我们可以直接生成 tupdesc 来解析记录类型 */
		RowExpr    *rexpr = (RowExpr *) expr;
		TupleDesc	tupdesc;
		AttrNumber	i = 1;
		ListCell   *lcc,
				   *lcn;

		tupdesc = CreateTemplateTupleDesc(list_length(rexpr->args));
		Assert(list_length(rexpr->args) == list_length(rexpr->colnames));
		forboth(lcc, rexpr->args, lcn, rexpr->colnames)
		{
			Node	   *col = (Node *) lfirst(lcc);
			char	   *colname = strVal(lfirst(lcn));

			TupleDescInitEntry(tupdesc, i,
							   colname,
							   exprType(col),
							   exprTypmod(col),
							   0);
			TupleDescInitEntryCollation(tupdesc, i,
										exprCollation(col));
			i++;
		}
		if (resultTypeId)
			*resultTypeId = rexpr->row_typeid;
		if (resultTupleDesc)
			*resultTupleDesc = BlessTupleDesc(tupdesc);
		return TYPEFUNC_COMPOSITE;
	}
	else if (expr && IsA(expr, Const) &&
			 ((Const *) expr)->consttype == RECORDOID &&
			 !((Const *) expr)->constisnull)
	{
		/*
		 * 在对带 SEARCH/CYCLE 子句的某些查询做 EXPLAIN
		 * 时，可能需要解析 RECORD 类型 Const 的字段名。
		 * datum 应带有能说明此事的 typmod
		 */
		HeapTupleHeader rec;
		Oid			tupType;
		int32		tupTypmod;

		rec = DatumGetHeapTupleHeader(((Const *) expr)->constvalue);
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
		if (resultTypeId)
			*resultTypeId = tupType;
		if (tupType != RECORDOID || tupTypmod >= 0)
		{
			/* 应该能查到它 */
			if (resultTupleDesc)
				*resultTupleDesc = lookup_rowtype_tupdesc_copy(tupType,
															   tupTypmod);
			return TYPEFUNC_COMPOSITE;
		}
		else
		{
			/* 这实际上不应发生…… */
			if (resultTupleDesc)
				*resultTupleDesc = NULL;
			return TYPEFUNC_RECORD;
		}
	}
	else
	{
		/* 按通用表达式处理；没有机会解析 RECORD */
		Oid			typid = exprType(expr);
		Oid			base_typid;

		if (resultTypeId)
			*resultTypeId = typid;
		if (resultTupleDesc)
			*resultTupleDesc = NULL;
		result = get_type_func_class(typid, &base_typid);
		if ((result == TYPEFUNC_COMPOSITE ||
			 result == TYPEFUNC_COMPOSITE_DOMAIN) &&
			resultTupleDesc)
			*resultTupleDesc = lookup_rowtype_tupdesc_copy(base_typid, -1);
	}

	return result;
}

/*
 * get_func_result_type
 *		同上，但仅从函数的 OID 入手
 *
 * 这将无法解析纯 RECORD 结果，也无法解析多态
 */
TypeFuncClass
get_func_result_type(Oid functionId,
					 Oid *resultTypeId,
					 TupleDesc *resultTupleDesc)
{
	return internal_get_result_type(functionId,
									NULL,
									NULL,
									resultTypeId,
									resultTupleDesc);
}

/*
 * internal_get_result_type -- 实现上述各函数的主力代码
 *
 * funcid 必须始终提供。若不可用，call_expr 与
 * rsinfo 可为 NULL。此时将返回 TYPEFUNC_RECORD，
 * 并在 *resultTupleDesc 中存入 NULL，若无法据
 * 现有信息完整推断
 */
static TypeFuncClass
internal_get_result_type(Oid funcid,
						 Node *call_expr,
						 ReturnSetInfo *rsinfo,
						 Oid *resultTypeId,
						 TupleDesc *resultTupleDesc)
{
	TypeFuncClass result;
	HeapTuple	tp;
	Form_pg_proc procform;
	Oid			rettype;
	Oid			base_rettype;
	TupleDesc	tupdesc;

	/* 先取出该函数的 pg_proc 行以查看其 rettype */
	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	rettype = procform->prorettype;

	/* 检查定义 RECORD 结果的 OUT 参数 */
	tupdesc = build_function_result_tupdesc_t(tp);
	if (tupdesc)
	{
		/*
		 * 它有 OUT 参数，故基本如同普通复合类型，但须
		 * 能够解析任何多态的 OUT
		 * 参数。
		 */
		if (resultTypeId)
			*resultTypeId = rettype;

		if (resolve_polymorphic_tupdesc(tupdesc,
										&procform->proargtypes,
										call_expr))
		{
			if (tupdesc->tdtypeid == RECORDOID &&
				tupdesc->tdtypmod < 0)
				assign_record_type_typmod(tupdesc);
			if (resultTupleDesc)
				*resultTupleDesc = tupdesc;
			result = TYPEFUNC_COMPOSITE;
		}
		else
		{
			if (resultTupleDesc)
				*resultTupleDesc = NULL;
			result = TYPEFUNC_RECORD;
		}

		ReleaseSysCache(tp);

		return result;
	}

	/*
	 * 若为标量多态结果，尝试解析它。
	 */
	if (IsPolymorphicType(rettype))
	{
		Oid			newrettype = exprType(call_expr);

		if (newrettype == InvalidOid)	/* 此情形多半不应发生 */
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not determine actual result type for function \"%s\" declared to return type %s",
							NameStr(procform->proname),
							format_type_be(rettype))));
		rettype = newrettype;
	}

	if (resultTypeId)
		*resultTypeId = rettype;
	if (resultTupleDesc)
		*resultTupleDesc = NULL;	/* 默认结果 */

	/* 对结果类型分类 */
	result = get_type_func_class(rettype, &base_rettype);
	switch (result)
	{
		case TYPEFUNC_COMPOSITE:
		case TYPEFUNC_COMPOSITE_DOMAIN:
			if (resultTupleDesc)
				*resultTupleDesc = lookup_rowtype_tupdesc_copy(base_rettype, -1);
			/* 命名复合类型不能含任何多态列 */
			break;
		case TYPEFUNC_SCALAR:
			break;
		case TYPEFUNC_RECORD:
			/* 必须从调用语境获取元组描述符 */
			if (rsinfo && IsA(rsinfo, ReturnSetInfo) &&
				rsinfo->expectedDesc != NULL)
			{
				result = TYPEFUNC_COMPOSITE;
				if (resultTupleDesc)
					*resultTupleDesc = rsinfo->expectedDesc;
				/* 此处同样假定无多态列 */
			}
			break;
		default:
			break;
	}

	ReleaseSysCache(tp);

	return result;
}

/*
 * get_expr_result_tupdesc
 *		获取描述复合值表达式结果的元组描述符
 *
 * 若表达式非复合类型或行类型不定，noError 为真
 * 时返回 NULL，否则抛错
 *
 * 这是 get_expr_result_type() 的简化版本，适用于
 * 调用者只关心可确定的行类型结果的场合。与该函数
 * 一样，勿对带 coldeflist 的 RTE 使用其 funcexpr
 */
TupleDesc
get_expr_result_tupdesc(Node *expr, bool noError)
{
	TupleDesc	tupleDesc;
	TypeFuncClass functypclass;

	functypclass = get_expr_result_type(expr, NULL, &tupleDesc);

	if (functypclass == TYPEFUNC_COMPOSITE ||
		functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		return tupleDesc;

	if (!noError)
	{
		Oid			exprTypeId = exprType(expr);

		if (exprTypeId != RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(exprTypeId))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("record type has not been registered")));
	}

	return NULL;
}

/*
 * 从其他多态输入解析 ANYELEMENT 的实际类型
 *
 * 注意：此处及下方同族函数的错误情形并非真正
 * 面向用户；只有当函数签名不正确，或解析器未能
 * 强制实参类型一致时才可能发生。因此，对于错误
 * 消息不必过于讲究
 */
static void
resolve_anyelement_from_others(polymorphic_actuals *actuals)
{
	if (OidIsValid(actuals->anyarray_type))
	{
		/* 使用与实际类型对应的元素类型 */
		Oid			array_base_type = getBaseType(actuals->anyarray_type);
		Oid			array_typelem = get_element_type(array_base_type);

		if (!OidIsValid(array_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not an array but type %s",
							"anyarray",
							format_type_be(array_base_type))));
		actuals->anyelement_type = array_typelem;
	}
	else if (OidIsValid(actuals->anyrange_type))
	{
		/* 使用与实际类型对应的元素类型 */
		Oid			range_base_type = getBaseType(actuals->anyrange_type);
		Oid			range_typelem = get_range_subtype(range_base_type);

		if (!OidIsValid(range_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not a range type but type %s",
							"anyrange",
							format_type_be(range_base_type))));
		actuals->anyelement_type = range_typelem;
	}
	else if (OidIsValid(actuals->anymultirange_type))
	{
		/* 依据多范围类型使用对应的元素类型 */
		Oid			multirange_base_type;
		Oid			multirange_typelem;
		Oid			range_base_type;
		Oid			range_typelem;

		multirange_base_type = getBaseType(actuals->anymultirange_type);
		multirange_typelem = get_multirange_range(multirange_base_type);
		if (!OidIsValid(multirange_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not a multirange type but type %s",
							"anymultirange",
							format_type_be(multirange_base_type))));

		range_base_type = getBaseType(multirange_typelem);
		range_typelem = get_range_subtype(range_base_type);

		if (!OidIsValid(range_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s does not contain a range type but type %s",
							"anymultirange",
							format_type_be(range_base_type))));
		actuals->anyelement_type = range_typelem;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * 从其他多态输入解析 ANYARRAY 的实际类型
 */
static void
resolve_anyarray_from_others(polymorphic_actuals *actuals)
{
	/* 若不知 ANYELEMENT，先解析它 */
	if (!OidIsValid(actuals->anyelement_type))
		resolve_anyelement_from_others(actuals);

	if (OidIsValid(actuals->anyelement_type))
	{
		/* 使用与实际类型对应的数组类型 */
		Oid			array_typeid = get_array_type(actuals->anyelement_type);

		if (!OidIsValid(array_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find array type for data type %s",
							format_type_be(actuals->anyelement_type))));
		actuals->anyarray_type = array_typeid;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * 从其他多态输入解析 ANYRANGE 的实际类型
 */
static void
resolve_anyrange_from_others(polymorphic_actuals *actuals)
{
	/*
	 * 无法从其他多态数组或基础类型推断范围
	 * 类型，因为同一子类型可能存在多个范围类型，
	 * 但可以从多态多范围类型推断它
	 */
	if (OidIsValid(actuals->anymultirange_type))
	{
		/* 依据多范围类型使用对应的元素类型 */
		Oid			multirange_base_type = getBaseType(actuals->anymultirange_type);
		Oid			multirange_typelem = get_multirange_range(multirange_base_type);

		if (!OidIsValid(multirange_typelem))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("argument declared %s is not a multirange type but type %s",
							"anymultirange",
							format_type_be(multirange_base_type))));
		actuals->anyrange_type = multirange_typelem;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * 从其他多态输入解析 ANYMULTIRANGE 的实际类型
 */
static void
resolve_anymultirange_from_others(polymorphic_actuals *actuals)
{
	/*
	 * 我们无法从多态数组或基础类型推断多范围类型，
	 * 因为同一子类型可能存在多个范围类型，但可以
	 * 从多态范围类型推断它
	 */
	if (OidIsValid(actuals->anyrange_type))
	{
		Oid			range_base_type = getBaseType(actuals->anyrange_type);
		Oid			multirange_typeid = get_range_multirange(range_base_type);

		if (!OidIsValid(multirange_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find multirange type for data type %s",
							format_type_be(actuals->anyrange_type))));
		actuals->anymultirange_type = multirange_typeid;
	}
	else
		elog(ERROR, "could not determine polymorphic type");
}

/*
 * 对于带 OUT 参数的函数的结果元组描述符，请将
 * 其中所有多态列类型（ANYELEMENT 等）替换为从
 * 输入实参推断出的具体数据类型
 * declared_args 是函数声明的输入参数类型的
 * oidvector（指明哪些是多态），call_expr 是调用表达式
 *
 * 若能推断全部类型则返回真；若必要信息缺失
 * （call_expr 为 NULL 或实参类型无法识别）则返回假
 */
static bool
resolve_polymorphic_tupdesc(TupleDesc tupdesc, oidvector *declared_args,
							Node *call_expr)
{
	int			natts = tupdesc->natts;
	int			nargs = declared_args->dim1;
	bool		have_polymorphic_result = false;
	bool		have_anyelement_result = false;
	bool		have_anyarray_result = false;
	bool		have_anyrange_result = false;
	bool		have_anymultirange_result = false;
	bool		have_anycompatible_result = false;
	bool		have_anycompatible_array_result = false;
	bool		have_anycompatible_range_result = false;
	bool		have_anycompatible_multirange_result = false;
	polymorphic_actuals poly_actuals;
	polymorphic_actuals anyc_actuals;
	Oid			anycollation = InvalidOid;
	Oid			anycompatcollation = InvalidOid;
	int			i;

	/* 看看是否存在多态输出；若否则快速返回 */
	for (i = 0; i < natts; i++)
	{
		switch (TupleDescAttr(tupdesc, i)->atttypid)
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				have_polymorphic_result = true;
				have_anyelement_result = true;
				break;
			case ANYARRAYOID:
				have_polymorphic_result = true;
				have_anyarray_result = true;
				break;
			case ANYRANGEOID:
				have_polymorphic_result = true;
				have_anyrange_result = true;
				break;
			case ANYMULTIRANGEOID:
				have_polymorphic_result = true;
				have_anymultirange_result = true;
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				have_polymorphic_result = true;
				have_anycompatible_result = true;
				break;
			case ANYCOMPATIBLEARRAYOID:
				have_polymorphic_result = true;
				have_anycompatible_array_result = true;
				break;
			case ANYCOMPATIBLERANGEOID:
				have_polymorphic_result = true;
				have_anycompatible_range_result = true;
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				have_polymorphic_result = true;
				have_anycompatible_multirange_result = true;
				break;
			default:
				break;
		}
	}
	if (!have_polymorphic_result)
		return true;

	/*
	 * 否则，从输入实参提取实际数据类型。（我们假定
	 * 解析器已校验过实参的一致性。另外，对于
	 * ANYCOMPATIBLE 伪类型族，我们预期所有匹配的
	 * 实参都已被强制转换为选定的公共超类型，因此
	 * 查看哪一个的暴露类型都无所谓）
	 */
	if (!call_expr)
		return false;			/* 无望 */

	memset(&poly_actuals, 0, sizeof(poly_actuals));
	memset(&anyc_actuals, 0, sizeof(anyc_actuals));

	for (i = 0; i < nargs; i++)
	{
		switch (declared_args->values[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				if (!OidIsValid(poly_actuals.anyelement_type))
				{
					poly_actuals.anyelement_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anyelement_type))
						return false;
				}
				break;
			case ANYARRAYOID:
				if (!OidIsValid(poly_actuals.anyarray_type))
				{
					poly_actuals.anyarray_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anyarray_type))
						return false;
				}
				break;
			case ANYRANGEOID:
				if (!OidIsValid(poly_actuals.anyrange_type))
				{
					poly_actuals.anyrange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anyrange_type))
						return false;
				}
				break;
			case ANYMULTIRANGEOID:
				if (!OidIsValid(poly_actuals.anymultirange_type))
				{
					poly_actuals.anymultirange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(poly_actuals.anymultirange_type))
						return false;
				}
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				if (!OidIsValid(anyc_actuals.anyelement_type))
				{
					anyc_actuals.anyelement_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anyelement_type))
						return false;
				}
				break;
			case ANYCOMPATIBLEARRAYOID:
				if (!OidIsValid(anyc_actuals.anyarray_type))
				{
					anyc_actuals.anyarray_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anyarray_type))
						return false;
				}
				break;
			case ANYCOMPATIBLERANGEOID:
				if (!OidIsValid(anyc_actuals.anyrange_type))
				{
					anyc_actuals.anyrange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anyrange_type))
						return false;
				}
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				if (!OidIsValid(anyc_actuals.anymultirange_type))
				{
					anyc_actuals.anymultirange_type =
						get_call_expr_argtype(call_expr, i);
					if (!OidIsValid(anyc_actuals.anymultirange_type))
						return false;
				}
				break;
			default:
				break;
		}
	}

	/* 如有需要，从其余类型推断多态类型 */
	if (have_anyelement_result && !OidIsValid(poly_actuals.anyelement_type))
		resolve_anyelement_from_others(&poly_actuals);

	if (have_anyarray_result && !OidIsValid(poly_actuals.anyarray_type))
		resolve_anyarray_from_others(&poly_actuals);

	if (have_anyrange_result && !OidIsValid(poly_actuals.anyrange_type))
		resolve_anyrange_from_others(&poly_actuals);

	if (have_anymultirange_result && !OidIsValid(poly_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&poly_actuals);

	if (have_anycompatible_result && !OidIsValid(anyc_actuals.anyelement_type))
		resolve_anyelement_from_others(&anyc_actuals);

	if (have_anycompatible_array_result && !OidIsValid(anyc_actuals.anyarray_type))
		resolve_anyarray_from_others(&anyc_actuals);

	if (have_anycompatible_range_result && !OidIsValid(anyc_actuals.anyrange_type))
		resolve_anyrange_from_others(&anyc_actuals);

	if (have_anycompatible_multirange_result && !OidIsValid(anyc_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&anyc_actuals);

	/*
	 * 确定为多态 OUT 参数使用的排序规则。（对
	 * anyelement 与 anyarray 必然相同，anycompatible
	 * 与 anycompatiblearray 也一样。）注意范围类型
	 * 不可排序，故范围类型内部的排序规则不在此
	 * 处考虑
	 */
	if (OidIsValid(poly_actuals.anyelement_type))
		anycollation = get_typcollation(poly_actuals.anyelement_type);
	else if (OidIsValid(poly_actuals.anyarray_type))
		anycollation = get_typcollation(poly_actuals.anyarray_type);

	if (OidIsValid(anyc_actuals.anyelement_type))
		anycompatcollation = get_typcollation(anyc_actuals.anyelement_type);
	else if (OidIsValid(anyc_actuals.anyarray_type))
		anycompatcollation = get_typcollation(anyc_actuals.anyarray_type);

	if (OidIsValid(anycollation) || OidIsValid(anycompatcollation))
	{
		/*
		 * 这些类型可排序，因此考虑是否使用非默认排
		 * 序规则。若能确定该函数使用的输入排序规则，
		 * 则使用之
		 */
		Oid			inputcollation = exprInputCollation(call_expr);

		if (OidIsValid(inputcollation))
		{
			if (OidIsValid(anycollation))
				anycollation = inputcollation;
			if (OidIsValid(anycompatcollation))
				anycompatcollation = inputcollation;
		}
	}

	/* 最后按需替换元组列类型 */
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		switch (att->atttypid)
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anyelement_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycollation);
				break;
			case ANYARRAYOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anyarray_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycollation);
				break;
			case ANYRANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anyrange_type,
								   -1,
								   0);
				/* 范围类型不应附加排序规则 */
				break;
			case ANYMULTIRANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   poly_actuals.anymultirange_type,
								   -1,
								   0);
				/* 多范围类型不应附加排序规则 */
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anyelement_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycompatcollation);
				break;
			case ANYCOMPATIBLEARRAYOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anyarray_type,
								   -1,
								   0);
				TupleDescInitEntryCollation(tupdesc, i + 1, anycompatcollation);
				break;
			case ANYCOMPATIBLERANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anyrange_type,
								   -1,
								   0);
				/* 范围类型不应附加排序规则 */
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				TupleDescInitEntry(tupdesc, i + 1,
								   NameStr(att->attname),
								   anyc_actuals.anymultirange_type,
								   -1,
								   0);
				/* 多范围类型不应附加排序规则 */
				break;
			default:
				break;
		}
	}

	return true;
}

/*
 * 给定函数声明的参数类型与模式，将 argtypes[]
 * 中的多态类型（ANYELEMENT 等）替换为从 call_expr
 * 中找到的输入实参推断出的具体类型
 *
 * 若能推断全部类型则返回真；若必要信息缺失
 * （call_expr 为 NULL 或实参类型无法识别）则返回假
 *
 * 与 resolve_polymorphic_tupdesc 逻辑相同，但参数
 * 表示形式不同，输出职责也略有差异
 *
 * argmodes 可为 NULL，此时假定所有参数都是 IN 模式
 */
bool
resolve_polymorphic_argtypes(int numargs, Oid *argtypes, char *argmodes,
							 Node *call_expr)
{
	bool		have_polymorphic_result = false;
	bool		have_anyelement_result = false;
	bool		have_anyarray_result = false;
	bool		have_anyrange_result = false;
	bool		have_anymultirange_result = false;
	bool		have_anycompatible_result = false;
	bool		have_anycompatible_array_result = false;
	bool		have_anycompatible_range_result = false;
	bool		have_anycompatible_multirange_result = false;
	polymorphic_actuals poly_actuals;
	polymorphic_actuals anyc_actuals;
	int			inargno;
	int			i;

	/*
	 * 第一遍：解析多态输入，检查输出。与
	 * resolve_polymorphic_tupdesc 一样，我们依赖解析器
	 * 强制类型一致并把 ANYCOMPATIBLE 实参转为公共超类型
	 */
	memset(&poly_actuals, 0, sizeof(poly_actuals));
	memset(&anyc_actuals, 0, sizeof(anyc_actuals));
	inargno = 0;
	for (i = 0; i < numargs; i++)
	{
		char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

		switch (argtypes[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anyelement_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anyelement_type))
					{
						poly_actuals.anyelement_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anyelement_type))
							return false;
					}
					argtypes[i] = poly_actuals.anyelement_type;
				}
				break;
			case ANYARRAYOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anyarray_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anyarray_type))
					{
						poly_actuals.anyarray_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anyarray_type))
							return false;
					}
					argtypes[i] = poly_actuals.anyarray_type;
				}
				break;
			case ANYRANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anyrange_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anyrange_type))
					{
						poly_actuals.anyrange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anyrange_type))
							return false;
					}
					argtypes[i] = poly_actuals.anyrange_type;
				}
				break;
			case ANYMULTIRANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anymultirange_result = true;
				}
				else
				{
					if (!OidIsValid(poly_actuals.anymultirange_type))
					{
						poly_actuals.anymultirange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(poly_actuals.anymultirange_type))
							return false;
					}
					argtypes[i] = poly_actuals.anymultirange_type;
				}
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anyelement_type))
					{
						anyc_actuals.anyelement_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anyelement_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anyelement_type;
				}
				break;
			case ANYCOMPATIBLEARRAYOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_array_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anyarray_type))
					{
						anyc_actuals.anyarray_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anyarray_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anyarray_type;
				}
				break;
			case ANYCOMPATIBLERANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_range_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anyrange_type))
					{
						anyc_actuals.anyrange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anyrange_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anyrange_type;
				}
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				if (argmode == PROARGMODE_OUT || argmode == PROARGMODE_TABLE)
				{
					have_polymorphic_result = true;
					have_anycompatible_multirange_result = true;
				}
				else
				{
					if (!OidIsValid(anyc_actuals.anymultirange_type))
					{
						anyc_actuals.anymultirange_type =
							get_call_expr_argtype(call_expr, inargno);
						if (!OidIsValid(anyc_actuals.anymultirange_type))
							return false;
					}
					argtypes[i] = anyc_actuals.anymultirange_type;
				}
				break;
			default:
				break;
		}
		if (argmode != PROARGMODE_OUT && argmode != PROARGMODE_TABLE)
			inargno++;
	}

	/* Done? */
	if (!have_polymorphic_result)
		return true;

	/* 如有需要，从其余类型推断多态类型 */
	if (have_anyelement_result && !OidIsValid(poly_actuals.anyelement_type))
		resolve_anyelement_from_others(&poly_actuals);

	if (have_anyarray_result && !OidIsValid(poly_actuals.anyarray_type))
		resolve_anyarray_from_others(&poly_actuals);

	if (have_anyrange_result && !OidIsValid(poly_actuals.anyrange_type))
		resolve_anyrange_from_others(&poly_actuals);

	if (have_anymultirange_result && !OidIsValid(poly_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&poly_actuals);

	if (have_anycompatible_result && !OidIsValid(anyc_actuals.anyelement_type))
		resolve_anyelement_from_others(&anyc_actuals);

	if (have_anycompatible_array_result && !OidIsValid(anyc_actuals.anyarray_type))
		resolve_anyarray_from_others(&anyc_actuals);

	if (have_anycompatible_range_result && !OidIsValid(anyc_actuals.anyrange_type))
		resolve_anyrange_from_others(&anyc_actuals);

	if (have_anycompatible_multirange_result && !OidIsValid(anyc_actuals.anymultirange_type))
		resolve_anymultirange_from_others(&anyc_actuals);

	/* 最后按需替换输出列类型 */
	for (i = 0; i < numargs; i++)
	{
		switch (argtypes[i])
		{
			case ANYELEMENTOID:
			case ANYNONARRAYOID:
			case ANYENUMOID:
				argtypes[i] = poly_actuals.anyelement_type;
				break;
			case ANYARRAYOID:
				argtypes[i] = poly_actuals.anyarray_type;
				break;
			case ANYRANGEOID:
				argtypes[i] = poly_actuals.anyrange_type;
				break;
			case ANYMULTIRANGEOID:
				argtypes[i] = poly_actuals.anymultirange_type;
				break;
			case ANYCOMPATIBLEOID:
			case ANYCOMPATIBLENONARRAYOID:
				argtypes[i] = anyc_actuals.anyelement_type;
				break;
			case ANYCOMPATIBLEARRAYOID:
				argtypes[i] = anyc_actuals.anyarray_type;
				break;
			case ANYCOMPATIBLERANGEOID:
				argtypes[i] = anyc_actuals.anyrange_type;
				break;
			case ANYCOMPATIBLEMULTIRANGEOID:
				argtypes[i] = anyc_actuals.anymultirange_type;
				break;
			default:
				break;
		}
	}

	return true;
}

/*
 * get_type_func_class
 *		给定类型 OID，获取其 TYPEFUNC 分类
 *		另外，若是域，则返回基础类型 OID
 *
 * 此函数旨在集中管理过去零散的类型分类
 * 代码。此处使用的类别有助于决定如何处理
 * 返回该数据类型的函数
 */
static TypeFuncClass
get_type_func_class(Oid typid, Oid *base_typeid)
{
	*base_typeid = typid;

	switch (get_typtype(typid))
	{
		case TYPTYPE_COMPOSITE:
			return TYPEFUNC_COMPOSITE;
		case TYPTYPE_BASE:
		case TYPTYPE_ENUM:
		case TYPTYPE_RANGE:
		case TYPTYPE_MULTIRANGE:
			return TYPEFUNC_SCALAR;
		case TYPTYPE_DOMAIN:
			*base_typeid = typid = getBaseType(typid);
			if (get_typtype(typid) == TYPTYPE_COMPOSITE)
				return TYPEFUNC_COMPOSITE_DOMAIN;
			else				/* 域的基础类型不能是伪类型 */
				return TYPEFUNC_SCALAR;
		case TYPTYPE_PSEUDO:
			if (typid == RECORDOID)
				return TYPEFUNC_RECORD;

			/*
			 * 我们将 VOID 与 CSTRING 视为合法的标量数据
			 * 类型，主要是为了方便 JDBC 驱动（它希望
			 * 能对所有合法的用户可调用函数执行 "
			 * SELECT * FROM foo()"）
			 */
			if (typid == VOIDOID || typid == CSTRINGOID)
				return TYPEFUNC_SCALAR;
			return TYPEFUNC_OTHER;
	}
	/* 多半不应走到这里 */
	return TYPEFUNC_OTHER;
}


/*
 * get_func_arg_info
 *
 * 从 pg_proc 元组获取参数类型、名称与 IN/OUT
 * 模式等信息。返回值为参数总数
 * 其他结果均为 palloc 分配。*p_argtypes 总会填
 * 充，但 *p_argnames 与 *p_argmodes 在默认情形
 * （无名称，全为 IN 参数）下置 NULL
 *
 * 注意：此函数只是取回 pg_proc 元组中的内容，
 * 并不对多态类型做任何解释
 */
int
get_func_arg_info(HeapTuple procTup,
				  Oid **p_argtypes, char ***p_argnames, char **p_argmodes)
{
	Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	Datum		proallargtypes;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isNull;
	ArrayType  *arr;
	int			numargs;
	Datum	   *elems;
	int			nelems;
	int			i;

	/* 先查明参数总数并获取其类型 */
	proallargtypes = SysCacheGetAttr(PROCOID, procTup,
									 Anum_pg_proc_proallargtypes,
									 &isNull);
	if (!isNull)
	{
		/*
		 * 期望这些数组是类型正确的 1 维数组；需验证
		 * 对 OID 与 char 数组无需使用
		 * deconstruct_array()，因为数组数据只是
		 * C 值数组。
		 */
		arr = DatumGetArrayTypeP(proallargtypes);	/* 未经TOAST化 */
		numargs = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numargs < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
		Assert(numargs >= procStruct->pronargs);
		*p_argtypes = (Oid *) palloc(numargs * sizeof(Oid));
		memcpy(*p_argtypes, ARR_DATA_PTR(arr),
			   numargs * sizeof(Oid));
	}
	else
	{
		/* 若无 proallargtypes 用 proargtypes */
		numargs = procStruct->proargtypes.dim1;
		Assert(numargs == procStruct->pronargs);
		*p_argtypes = (Oid *) palloc(numargs * sizeof(Oid));
		memcpy(*p_argtypes, procStruct->proargtypes.values,
			   numargs * sizeof(Oid));
	}

	/* 若可用，获取参数名 */
	proargnames = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_proargnames,
								  &isNull);
	if (isNull)
		*p_argnames = NULL;
	else
	{
		deconstruct_array_builtin(DatumGetArrayTypeP(proargnames), TEXTOID,
								  &elems, NULL, &nelems);
		if (nelems != numargs)	/* 不应发生 */
			elog(ERROR, "proargnames must have the same number of elements as the function has arguments");
		*p_argnames = (char **) palloc(sizeof(char *) * numargs);
		for (i = 0; i < numargs; i++)
			(*p_argnames)[i] = TextDatumGetCString(elems[i]);
	}

	/* 若可用，获取参数模式 */
	proargmodes = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_proargmodes,
								  &isNull);
	if (isNull)
		*p_argmodes = NULL;
	else
	{
		arr = DatumGetArrayTypeP(proargmodes);	/* 未经TOAST化 */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		*p_argmodes = (char *) palloc(numargs * sizeof(char));
		memcpy(*p_argmodes, ARR_DATA_PTR(arr),
			   numargs * sizeof(char));
	}

	return numargs;
}

/*
 * get_func_trftypes
 *
 * 返回该函数使用的转换类型数量
 * 若有，则返回一个 palloc 的类型 OID 数组到
 * *p_trftypes
 */
int
get_func_trftypes(HeapTuple procTup,
				  Oid **p_trftypes)
{
	Datum		protrftypes;
	ArrayType  *arr;
	int			nelems;
	bool		isNull;

	protrftypes = SysCacheGetAttr(PROCOID, procTup,
								  Anum_pg_proc_protrftypes,
								  &isNull);
	if (!isNull)
	{
		/*
		 * 期望这些数组是类型正确的 1 维数组；需验证
		 * 对 OID 与 char 数组无需使用
		 * deconstruct_array()，因为数组数据只是
		 * C 值数组。
		 */
		arr = DatumGetArrayTypeP(protrftypes);	/* 未经TOAST化 */
		nelems = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			nelems < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != OIDOID)
			elog(ERROR, "protrftypes is not a 1-D Oid array or it contains nulls");
		*p_trftypes = (Oid *) palloc(nelems * sizeof(Oid));
		memcpy(*p_trftypes, ARR_DATA_PTR(arr),
			   nelems * sizeof(Oid));

		return nelems;
	}
	else
		return 0;
}

/*
 * get_func_input_arg_names
 *
 * 仅提取输入参数的名称，给定函数的 Datum 形
 * 式的 proargnames 与 proargmodes 条目
 *
 * 返回输入参数的数量，即返回给 *arg_names 的
 * palloc 数组的长度。未命名参数的条目置为
 * NULL。若 proargnames 为 NULL 则一无所获
 */
int
get_func_input_arg_names(Datum proargnames, Datum proargmodes,
						 char ***arg_names)
{
	ArrayType  *arr;
	int			numargs;
	Datum	   *argnames;
	char	   *argmodes;
	char	  **inargnames;
	int			numinargs;
	int			i;

	/* proargnames 为空则跳过 */
	if (proargnames == PointerGetDatum(NULL))
	{
		*arg_names = NULL;
		return 0;
	}

	/*
	 * 期望这些数组是类型正确的 1 维数组；需验证。
	 * 对于 proargmodes，无需使用 deconstruct_array()
	 * 因为数组数据看起来就是 C 值数组
	 */
	arr = DatumGetArrayTypeP(proargnames);	/* 未经TOAST化 */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != TEXTOID)
		elog(ERROR, "proargnames is not a 1-D text array or it contains nulls");
	deconstruct_array_builtin(arr, TEXTOID, &argnames, NULL, &numargs);
	if (proargmodes != PointerGetDatum(NULL))
	{
		arr = DatumGetArrayTypeP(proargmodes);	/* 未经TOAST化 */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		argmodes = (char *) ARR_DATA_PTR(arr);
	}
	else
		argmodes = NULL;

	/* 零元素多半不应发生，但也要优雅地处理 */
	if (numargs <= 0)
	{
		*arg_names = NULL;
		return 0;
	}

	/* 提取输入参数名 */
	inargnames = (char **) palloc(numargs * sizeof(char *));
	numinargs = 0;
	for (i = 0; i < numargs; i++)
	{
		if (argmodes == NULL ||
			argmodes[i] == PROARGMODE_IN ||
			argmodes[i] == PROARGMODE_INOUT ||
			argmodes[i] == PROARGMODE_VARIADIC)
		{
			char	   *pname = TextDatumGetCString(argnames[i]);

			if (pname[0] != '\0')
				inargnames[numinargs] = pname;
			else
				inargnames[numinargs] = NULL;
			numinargs++;
		}
	}

	*arg_names = inargnames;
	return numinargs;
}


/*
 * get_func_result_name
 *
 * 若函数恰好有一个输出参数，且该参数有名字，
 * 则返回该名字（palloc 字符串）。否则返回 NULL
 *
 * 此函数用于为返回标量类型的函数确定默认的
 * 输出列名
 */
char *
get_func_result_name(Oid functionId)
{
	char	   *result;
	HeapTuple	procTuple;
	Datum		proargmodes;
	Datum		proargnames;
	ArrayType  *arr;
	int			numargs;
	char	   *argmodes;
	Datum	   *argnames;
	int			numoutargs;
	int			nargnames;
	int			i;

	/* 先取出函数的 pg_proc 行 */
	procTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
	if (!HeapTupleIsValid(procTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);

	/* 若没有命名的 OUT 参数，返回 NULL */
	if (heap_attisnull(procTuple, Anum_pg_proc_proargmodes, NULL) ||
		heap_attisnull(procTuple, Anum_pg_proc_proargnames, NULL))
		result = NULL;
	else
	{
		/* 从元组取出数据 */
		proargmodes = SysCacheGetAttrNotNull(PROCOID, procTuple,
											 Anum_pg_proc_proargmodes);
		proargnames = SysCacheGetAttrNotNull(PROCOID, procTuple,
											 Anum_pg_proc_proargnames);

		/*
		 * 期望数组是类型正确的 1 维数组；需加以验证
		 * 对于 char 数组，无需使用 deconstruct_array()
		 * 因为数组数据形如 C 值
		 * 数组
		 */
		arr = DatumGetArrayTypeP(proargmodes);	/* 未经TOAST化 */
		numargs = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numargs < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array or it contains nulls");
		argmodes = (char *) ARR_DATA_PTR(arr);
		arr = DatumGetArrayTypeP(proargnames);	/* 未经TOAST化 */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != TEXTOID)
			elog(ERROR, "proargnames is not a 1-D text array of length %d or it contains nulls",
				 numargs);
		deconstruct_array_builtin(arr, TEXTOID, &argnames, NULL, &nargnames);
		Assert(nargnames == numargs);

		/* 扫描输出参数 */
		result = NULL;
		numoutargs = 0;
		for (i = 0; i < numargs; i++)
		{
			if (argmodes[i] == PROARGMODE_IN ||
				argmodes[i] == PROARGMODE_VARIADIC)
				continue;
			Assert(argmodes[i] == PROARGMODE_OUT ||
				   argmodes[i] == PROARGMODE_INOUT ||
				   argmodes[i] == PROARGMODE_TABLE);
			if (++numoutargs > 1)
			{
				/* 多个输出参数，故放弃 */
				result = NULL;
				break;
			}
			result = TextDatumGetCString(argnames[i]);
			if (result == NULL || result[0] == '\0')
			{
				/* 参数未命名，故放弃 */
				result = NULL;
				break;
			}
		}
	}

	ReleaseSysCache(procTuple);

	return result;
}


/*
 * build_function_result_tupdesc_t
 *
 * 给定函数的 pg_proc 行，返回结果行类型的元组
 * 描述符；若函数没有 OUT 参数则返回 NULL
 *
 * 注意此函数不处理多态类型的解析；
 * 这是有意为之
 */
TupleDesc
build_function_result_tupdesc_t(HeapTuple procTuple)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(procTuple);
	Datum		proallargtypes;
	Datum		proargmodes;
	Datum		proargnames;
	bool		isnull;

	/* 若函数未声明返回 RECORD，返回 NULL */
	if (procform->prorettype != RECORDOID)
		return NULL;

	/* 若无 OUT 参数，返回 NULL */
	if (heap_attisnull(procTuple, Anum_pg_proc_proallargtypes, NULL) ||
		heap_attisnull(procTuple, Anum_pg_proc_proargmodes, NULL))
		return NULL;

	/* 从元组取出数据 */
	proallargtypes = SysCacheGetAttrNotNull(PROCOID, procTuple,
											Anum_pg_proc_proallargtypes);
	proargmodes = SysCacheGetAttrNotNull(PROCOID, procTuple,
										 Anum_pg_proc_proargmodes);
	proargnames = SysCacheGetAttr(PROCOID, procTuple,
								  Anum_pg_proc_proargnames,
								  &isnull);
	if (isnull)
		proargnames = PointerGetDatum(NULL);	/* 仅为确认 */

	return build_function_result_tupdesc_d(procform->prokind,
										   proallargtypes,
										   proargmodes,
										   proargnames);
}

/*
 * build_function_result_tupdesc_d
 *
 * 从 pg_proc 的 proallargtypes、proargmodes 与
 * proargnames 数组构建 RECORD 函数的元组描述符。
 * 之所以独立出来，是为了方便 ProcedureCreate，
 * 它需在创建函数前算出元组描述符
 *
 * 对函数（但非过程），若 OUT 或 INOUT 参数不足
 * 两个，则返回 NULL
 */
TupleDesc
build_function_result_tupdesc_d(char prokind,
								Datum proallargtypes,
								Datum proargmodes,
								Datum proargnames)
{
	TupleDesc	desc;
	ArrayType  *arr;
	int			numargs;
	Oid		   *argtypes;
	char	   *argmodes;
	Datum	   *argnames = NULL;
	Oid		   *outargtypes;
	char	  **outargnames;
	int			numoutargs;
	int			nargnames;
	int			i;

	/* 列数组为空则不可能有输出参数 */
	if (proallargtypes == PointerGetDatum(NULL) ||
		proargmodes == PointerGetDatum(NULL))
		return NULL;

	/*
	 * 期望这些数组是类型正确的 1 维数组；需验证。
	 * 对 OID 与 char 数组无需使用 deconstruct_array()
	 * 因为数组数据只是 C 值数组
	 */
	arr = DatumGetArrayTypeP(proallargtypes);	/* 未经TOAST化 */
	numargs = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numargs < 0 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
	argtypes = (Oid *) ARR_DATA_PTR(arr);
	arr = DatumGetArrayTypeP(proargmodes);	/* 未经TOAST化 */
	if (ARR_NDIM(arr) != 1 ||
		ARR_DIMS(arr)[0] != numargs ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
			 numargs);
	argmodes = (char *) ARR_DATA_PTR(arr);
	if (proargnames != PointerGetDatum(NULL))
	{
		arr = DatumGetArrayTypeP(proargnames);	/* 未经TOAST化 */
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != TEXTOID)
			elog(ERROR, "proargnames is not a 1-D text array of length %d or it contains nulls",
				 numargs);
		deconstruct_array_builtin(arr, TEXTOID, &argnames, NULL, &nargnames);
		Assert(nargnames == numargs);
	}

	/* 零元素多半不应发生，但也要优雅地处理 */
	if (numargs <= 0)
		return NULL;

	/* 提取输出参数的类型与名称 */
	outargtypes = (Oid *) palloc(numargs * sizeof(Oid));
	outargnames = (char **) palloc(numargs * sizeof(char *));
	numoutargs = 0;
	for (i = 0; i < numargs; i++)
	{
		char	   *pname;

		if (argmodes[i] == PROARGMODE_IN ||
			argmodes[i] == PROARGMODE_VARIADIC)
			continue;
		Assert(argmodes[i] == PROARGMODE_OUT ||
			   argmodes[i] == PROARGMODE_INOUT ||
			   argmodes[i] == PROARGMODE_TABLE);
		outargtypes[numoutargs] = argtypes[i];
		if (argnames)
			pname = TextDatumGetCString(argnames[i]);
		else
			pname = NULL;
		if (pname == NULL || pname[0] == '\0')
		{
			/* 参数未命名，故现造一个列名 */
			pname = psprintf("column%d", numoutargs + 1);
		}
		outargnames[numoutargs] = pname;
		numoutargs++;
	}

	/*
	 * 若没有输出参数或只有一个，函数将不返回
	 * 元组
	 */
	if (numoutargs < 2 && prokind != PROKIND_PROCEDURE)
		return NULL;

	desc = CreateTemplateTupleDesc(numoutargs);
	for (i = 0; i < numoutargs; i++)
	{
		TupleDescInitEntry(desc, i + 1,
						   outargnames[i],
						   outargtypes[i],
						   -1,
						   0);
	}

	return desc;
}


/*
 * RelationNameGetTupleDesc
 *
 * 给定（可能带限定符的）关系名，构建 TupleDesc
 *
 * 注意：虽然此函数确实如宣称的那样可用，但它
 * 很少是为函数结果类型构建元组描述符的最佳
 * 方式。保留它只是为了与既有用户代码向后兼容
 */
TupleDesc
RelationNameGetTupleDesc(const char *relname)
{
	RangeVar   *relvar;
	Relation	rel;
	TupleDesc	tupdesc;
	List	   *relname_list;

	/* 打开关系并复制元组描述 */
	relname_list = stringToQualifiedNameList(relname, NULL);
	relvar = makeRangeVarFromNameList(relname_list);
	rel = relation_openrv(relvar, AccessShareLock);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	relation_close(rel, AccessShareLock);

	return tupdesc;
}

/*
 * TypeGetTupleDesc
 *
 * 给定类型 Oid，构建 TupleDesc。（多数情况下应
 * 改用 get_call_result_type 或其同族函数，以便
 * 处理 OUT 参数、RECORD 结果类型与多态
 * 结果）。
 *
 * 若类型是复合类型 *且* 提供了 colaliases List，
 * *且* 该 List 长度为 natts，则使用别名而非关系
 * 的 attnames。（注意：此用法已弃用，因为它可能
 * 导致创建不必要的瞬态记录类型）
 *
 * 若类型是基础类型，则需要单项别名的 List
 */
TupleDesc
TypeGetTupleDesc(Oid typeoid, List *colaliases)
{
	Oid			base_typeoid;
	TypeFuncClass functypclass = get_type_func_class(typeoid, &base_typeoid);
	TupleDesc	tupdesc = NULL;

	/*
	 * 构建表示输出行的合适元组描述符。我们
	 * 有意不支持 TYPEFUNC_COMPOSITE_DOMAIN，因为
	 * 这个废弃函数的遗留调用者多半不准备应用
	 * 域约束
	 */
	if (functypclass == TYPEFUNC_COMPOSITE)
	{
		/* 复合数据类型，如表的行类型 */
		tupdesc = lookup_rowtype_tupdesc_copy(base_typeoid, -1);

		if (colaliases != NIL)
		{
			int			natts = tupdesc->natts;
			int			varattno;

			/* 列表长度是否与属性数量匹配？ */
			if (list_length(colaliases) != natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("number of aliases does not match number of columns")));

			/* 好，改用别名 */
			for (varattno = 0; varattno < natts; varattno++)
			{
				char	   *label = strVal(list_nth(colaliases, varattno));
				Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno);

				if (label != NULL)
					namestrcpy(&(attr->attname), label);
			}

			/* 元组类型此时为匿名记录类型 */
			tupdesc->tdtypeid = RECORDOID;
			tupdesc->tdtypmod = -1;
		}
	}
	else if (functypclass == TYPEFUNC_SCALAR)
	{
		/* 基础类型，即标量 */
		char	   *attname;

		/* 基础类型需要别名列表 */
		if (colaliases == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("no column alias was provided")));

		/* 别名列表长度必须为 1 */
		if (list_length(colaliases) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("number of aliases does not match number of columns")));

		/* 好，取列别名 */
		attname = strVal(linitial(colaliases));

		tupdesc = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(tupdesc,
						   (AttrNumber) 1,
						   attname,
						   typeoid,
						   -1,
						   0);
	}
	else if (functypclass == TYPEFUNC_RECORD)
	{
		/* XXX 无法支持此情形，因为未传入 typmod…… */
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not determine row description for function returning record")));
	}
	else
	{
		/* 错误消息很蹩脚，但解析器本应捕获 */
		elog(ERROR, "function in FROM has unsupported return type");
	}

	return tupdesc;
}

/*
 * extract_variadic_args
 *
 * 为一组实参提取值、类型与 NULL 标记，给定一
 * 个使用 VARIADIC 输入且实参列表取决于调用者
 * 语境的输入函数。做 VARIADIC 调用时，调用者
 * 提供的是由一组值构成的数组的单个实参，因此
 * 先解构数组数据再做后续处理。若并非 VARIADIC
 * 调用，则只需按调用者给定的全部实参填入状态
 * 数据即可
 *
 * 此函数返回生成的实参数量，若为 "VARIADIC
 * NULL" 则返回 -1
 */
int
extract_variadic_args(FunctionCallInfo fcinfo, int variadic_start,
					  bool convert_unknown, Datum **args, Oid **types,
					  bool **nulls)
{
	bool		variadic = get_fn_expr_variadic(fcinfo->flinfo);
	Datum	   *args_res;
	bool	   *nulls_res;
	Oid		   *types_res;
	int			nargs,
				i;

	*args = NULL;
	*types = NULL;
	*nulls = NULL;

	if (variadic)
	{
		ArrayType  *array_in;
		Oid			element_type;
		bool		typbyval;
		char		typalign;
		int16		typlen;

		Assert(PG_NARGS() == variadic_start + 1);

		if (PG_ARGISNULL(variadic_start))
			return -1;

		array_in = PG_GETARG_ARRAYTYPE_P(variadic_start);
		element_type = ARR_ELEMTYPE(array_in);

		get_typlenbyvalalign(element_type,
							 &typlen, &typbyval, &typalign);
		deconstruct_array(array_in, element_type, typlen, typbyval,
						  typalign, &args_res, &nulls_res,
						  &nargs);

		/* 数组所有元素类型相同 */
		types_res = (Oid *) palloc0(nargs * sizeof(Oid));
		for (i = 0; i < nargs; i++)
			types_res[i] = element_type;
	}
	else
	{
		nargs = PG_NARGS() - variadic_start;
		Assert(nargs > 0);
		nulls_res = (bool *) palloc0(nargs * sizeof(bool));
		args_res = (Datum *) palloc0(nargs * sizeof(Datum));
		types_res = (Oid *) palloc0(nargs * sizeof(Oid));

		for (i = 0; i < nargs; i++)
		{
			nulls_res[i] = PG_ARGISNULL(i + variadic_start);
			types_res[i] = get_fn_expr_argtype(fcinfo->flinfo,
											   i + variadic_start);

			/*
			 * 把一个未知类型的（多半是字面）常量值按需
			 * 转为 text。未知类型以 cstring 指针传入。
			 * 注意：对声明为接受 "any" 类型的函数，解析器
			 * 不会对未知类型字面量（即无修饰的字符串或
			 * NULL）做任何类型转换
			 */
			if (convert_unknown &&
				types_res[i] == UNKNOWNOID &&
				get_fn_expr_arg_stable(fcinfo->flinfo, i + variadic_start))
			{
				types_res[i] = TEXTOID;

				if (PG_ARGISNULL(i + variadic_start))
					args_res[i] = (Datum) 0;
				else
					args_res[i] =
						CStringGetTextDatum(PG_GETARG_POINTER(i + variadic_start));
			}
			else
			{
				/* 无需转换，直接按原样取用该 datum */
				args_res[i] = PG_GETARG_DATUM(i + variadic_start);
			}

			if (!OidIsValid(types_res[i]) ||
				(convert_unknown && types_res[i] == UNKNOWNOID))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not determine data type for argument %d",
								i + 1)));
		}
	}

	/* 填入结果 */
	*args = args_res;
	*nulls = nulls_res;
	*types = types_res;

	return nargs;
}
