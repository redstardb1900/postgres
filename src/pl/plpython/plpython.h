/*-------------------------------------------------------------------------
 *
 * plpython.h：Python 作为 PostgreSQL 的过程语言
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/pl/plpython/plpython.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLPYTHON_H
#define PLPYTHON_H

/* 照例须在 Python.h 之前包含 postgres.h */
#if !defined(POSTGRES_H)
#error postgres.h must be included before plpython.h
#elif defined(Py_PYTHON_H)
#error Python.h must be included via plpython.h
#endif

/*
 * 经包装头文件引入 Python 头，以控制
 * 其中 system_header 的范围。
 */
#include "plpython_system.h"

/* 定义我们用于翻译的文本域 */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpython")

/*
 * 到处都用，所以干脆到处都包含它。
 */
#include "plpy_util.h"

#endif							/* PLPYTHON_H */
