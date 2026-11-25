/*
 * src/pl/plpython/plpy_resultobject.h
 */

#ifndef PLPY_RESULTOBJECT_H
#define PLPY_RESULTOBJECT_H

#include "access/tupdesc.h"

#include "plpython.h"


typedef struct PLyResultObject
{
	PyObject_HEAD
	/* HeapTuple *tuples; */
	PyObject   *nrows;			/* 查询返回的行数 */
	PyObject   *rows;			/* 数据行，或无数据时的空
								 * 列表 */
	PyObject   *status;			/* 查询状态 SPI_OK_* 或 SPI_ERR_* */
	TupleDesc	tupdesc;
} PLyResultObject;

extern void PLy_result_init_type(void);
extern PyObject *PLy_result_new(void);

#endif							/* PLPY_RESULTOBJECT_H */
