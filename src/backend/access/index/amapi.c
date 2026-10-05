/*-------------------------------------------------------------------------
 *
 * amapi.c
 *	  Postgres 索引访问方法 API 的支持例程。
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/amapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/*
 * GetIndexAmRoutine - 调用指定的访问方法处理器例程，取得
 * 其 IndexAmRoutine 结构，该结构在调用方上下文中 palloc。
 *
 * 注意：若 amhandler 函数是内置的，则不会访问任何系统目录。
 * 因此在为系统目录引导索引时可以安全使用。
 * relcache.c 依赖这一点。
 */
IndexAmRoutine *
GetIndexAmRoutine(Oid amhandler)
{
	Datum		datum;
	IndexAmRoutine *routine;

	datum = OidFunctionCall0(amhandler);
	routine = (IndexAmRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, IndexAmRoutine))
		elog(ERROR, "index access method handler function %u did not return an IndexAmRoutine struct",
			 amhandler);

	return routine;
}

/*
 * GetIndexAmRoutineByAmId - 按给定 OID 查找索引访问方法的
 * 处理器，并取得其 IndexAmRoutine 结构。
 *
 * 若给定 OID 不是有效的索引访问方法，则 noerror 为真时返回 NULL，
 * 否则抛出错误。
 */
IndexAmRoutine *
GetIndexAmRoutineByAmId(Oid amoid, bool noerror)
{
	HeapTuple	tuple;
	Form_pg_am	amform;
	regproc		amhandler;

	/* 取得该访问方法的处理器函数 OID */
	tuple = SearchSysCache1(AMOID, ObjectIdGetDatum(amoid));
	if (!HeapTupleIsValid(tuple))
	{
		if (noerror)
			return NULL;
		elog(ERROR, "cache lookup failed for access method %u",
			 amoid);
	}
	amform = (Form_pg_am) GETSTRUCT(tuple);

	/* 检查它是索引访问方法，而不是其他某种 AM */
	if (amform->amtype != AMTYPE_INDEX)
	{
		if (noerror)
		{
			ReleaseSysCache(tuple);
			return NULL;
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access method \"%s\" is not of type %s",
						NameStr(amform->amname), "INDEX")));
	}

	amhandler = amform->amhandler;

	/* 若处理器 OID 无效则报错 */
	if (!RegProcedureIsValid(amhandler))
	{
		if (noerror)
		{
			ReleaseSysCache(tuple);
			return NULL;
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index access method \"%s\" does not have a handler",
						NameStr(amform->amname))));
	}

	ReleaseSysCache(tuple);

	/* 最后调用处理器函数以取得 API 结构。 */
	return GetIndexAmRoutine(amhandler);
}


/*
 * 请适当的访问方法校验指定的操作符类。
 */
Datum
amvalidate(PG_FUNCTION_ARGS)
{
	Oid			opclassoid = PG_GETARG_OID(0);
	bool		result;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			amoid;
	IndexAmRoutine *amroutine;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	amoid = classform->opcmethod;

	ReleaseSysCache(classtup);

	amroutine = GetIndexAmRoutineByAmId(amoid, false);

	if (amroutine->amvalidate == NULL)
		elog(ERROR, "function amvalidate is not defined for index access method %u",
			 amoid);

	result = amroutine->amvalidate(opclassoid);

	pfree(amroutine);

	PG_RETURN_BOOL(result);
}
