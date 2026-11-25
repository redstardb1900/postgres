/*-------------------------------------------------------------------------
 *
 * plpython_system.h：引入 Python 的系统头文件
 *
 * 我们把它拆成单独的头文件，以便精确控制
 * system_header 编译指示的范围。不含 Postgres 专有
 * 声明不该放在这里。不过我们确实包含一些东西
 * 用来防止我们的代码与 Python 冲突。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/pl/plpython/plpython_system.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLPYTHON_SYSTEM_H
#define PLPYTHON_SYSTEM_H

/*
 * 较新 Python 头在我们首选编译
 * 标志下警告很多（至少已知
 * -Wdeclaration-after-statement 有问题）。system_header
 * 若支持就隐藏文件余下警告
 */
#ifdef HAVE_PRAGMA_GCC_SYSTEM_HEADER
#pragma GCC system_header
#endif

/*
 * 否则 Python <= 3.8 会定义一个替代，导致宏
 * 重定义警告。
 */
#define HAVE_SNPRINTF 1

#if defined(_MSC_VER) && defined(_DEBUG)
/* Python 在 VC++ 上用 #pragma 引入非默认 libpython，若
 * 定义了 _DEBUG */
#undef _DEBUG
/* 也藏起 errcode，因先加载 Python.h 后才是 postgres.h */
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#define _DEBUG
#elif defined (_MSC_VER)
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#else
#include <Python.h>
#endif

#endif							/* PLPYTHON_SYSTEM_H */
