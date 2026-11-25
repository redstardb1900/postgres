/*
 * src/pl/plpython/plpy_subxactobject.h
 */

#ifndef PLPY_SUBXACTOBJECT
#define PLPY_SUBXACTOBJECT

#include "nodes/pg_list.h"
#include "plpython.h"
#include "utils/resowner.h"

/* 嵌套显式子事务的列表 */
extern List *explicit_subtransactions;


typedef struct PLySubtransactionObject
{
	PyObject_HEAD
	bool		started;
	bool		exited;
} PLySubtransactionObject;

/* 显式子事务数据 */
typedef struct PLySubtransactionData
{
	MemoryContext oldcontext;
	ResourceOwner oldowner;
} PLySubtransactionData;

extern void PLy_subtransaction_init_type(void);
extern PyObject *PLy_subtransaction_new(PyObject *self, PyObject *unused);

#endif							/* PLPY_SUBXACTOBJECT */
