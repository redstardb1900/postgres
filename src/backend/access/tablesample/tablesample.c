/*-------------------------------------------------------------------------
 *
 * tablesample.c
 *		  TABLESAMPLE 特性的支持函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		  src/backend/access/tablesample/tablesample.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tsmapi.h"


/*
 * GetTsmRoutine --- 调用处理函数以取得 TsmRoutine 结构。
 *
 * 这是一个仅用于检查错误的便捷例程。
 */
TsmRoutine *
GetTsmRoutine(Oid tsmhandler)
{
	Datum		datum;
	TsmRoutine *routine;

	datum = OidFunctionCall1(tsmhandler, PointerGetDatum(NULL));
	routine = (TsmRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, TsmRoutine))
		elog(ERROR, "tablesample handler function %u did not return a TsmRoutine struct",
			 tsmhandler);

	return routine;
}
