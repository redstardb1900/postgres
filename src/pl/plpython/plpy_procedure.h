/*
 * src/pl/plpython/plpy_procedure.h
 */

#ifndef PLPY_PROCEDURE_H
#define PLPY_PROCEDURE_H

#include "plpy_typeio.h"


extern void init_procedure_caches(void);


/* 为外层递归级或集合返回函数保存的参数 */
typedef struct PLySavedArgs
{
	struct PLySavedArgs *next;	/* 链表指针 */
	PyObject   *args;			/* 全局字典的 args 元素 */
	PyObject   *td;				/* 触发器时全局字典的 TD 元素 */
	int			nargs;			/* namedargs 数组长度 */
	PyObject   *namedargs[FLEXIBLE_ARRAY_MEMBER];	/* 参数名 */
} PLySavedArgs;

/* 缓存的过程数据 */
typedef struct PLyProcedure
{
	MemoryContext mcxt;			/* 存放此 PLyProcedure 及其
								 * 附属数据 */
	char	   *proname;		/* 过程的 SQL 名 */
	char	   *pyname;			/* 过程的 Python 名 */
	TransactionId fn_xmin;
	ItemPointerData fn_tid;
	bool		fn_readonly;
	bool		is_setof;		/* 若函数返回结果集则为真 */
	bool		is_procedure;
	bool		is_trigger;		/* 作触发器？ */
	PLyObToDatum result;		/* 函数结果输出转换信息 */
	PLyDatumToOb result_in;		/* 用于转换触发器中的输入元组 */
	char	   *src;			/* 改写之后的过程代码文本 */
	char	  **argnames;		/* 参数名 */
	PLyDatumToOb *args;			/* 参数输入转换信息 */
	int			nargs;			/* 上述数组中的元素个数 */
	Oid			langid;			/* plpython 的 pg_language 项 OID */
	List	   *trftypes;		/* 各 transform 类型 OID */
	PyObject   *code;			/* 已编译过程代码 */
	PyObject   *statics;		/* 调用间保存的局部数据 */
	PyObject   *globals;		/* 调用间保存的全局数据 */
	long		calldepth;		/* 函数递归调用的深度 */
	PLySavedArgs *argstack;		/* 外层调用参数的栈 */
} PLyProcedure;

/* 过程缓存键 */
typedef struct PLyProcedureKey
{
	Oid			fn_oid;			/* 函数 OID */
	Oid			fn_rel;			/* 触发所在关系，或 InvalidOid */
} PLyProcedureKey;

/* 过程缓存项 */
typedef struct PLyProcedureEntry
{
	PLyProcedureKey key;		/* hash键 */
	PLyProcedure *proc;
} PLyProcedureEntry;

/* PLyProcedure 操作 */
extern char *PLy_procedure_name(PLyProcedure *proc);
extern PLyProcedure *PLy_procedure_get(Oid fn_oid, Oid fn_rel, bool is_trigger);
extern void PLy_procedure_compile(PLyProcedure *proc, const char *src);
extern void PLy_procedure_delete(PLyProcedure *proc);

#endif							/* PLPY_PROCEDURE_H */
