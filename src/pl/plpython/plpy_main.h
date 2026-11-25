/*
 * src/pl/plpython/plpy_main.h
 */

#ifndef PLPY_MAIN_H
#define PLPY_MAIN_H

#include "plpy_procedure.h"

/* 解释器的全局字典 */
extern PyObject *PLy_interp_globals;

/*
 * 一叠 PL/Python 执行上下文。每当用户定义的 Python 代码
 * 被调用时，就创建执行上下文并压入栈。在该
 * Python 代码返回后，上下文销毁。
 */
typedef struct PLyExecutionContext
{
	PLyProcedure *curr_proc;	/* 当前正在执行的过程 */
	MemoryContext scratch_ctx;	/* 类型 I/O 之类的上下文 */
	struct PLyExecutionContext *next;	/* 上一层栈 */
} PLyExecutionContext;

/* 取得当前执行上下文 */
extern PLyExecutionContext *PLy_current_execution_context(void);

/* 取得指定执行上下文的暂存内存上下文 */
extern MemoryContext PLy_get_scratch_context(PLyExecutionContext *context);

#endif							/* PLPY_MAIN_H */
