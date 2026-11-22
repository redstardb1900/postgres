/* src/include/port/win32.h */

/*
 * 我们总依赖构建系统设置 WIN32 宏,
 * 但 _WIN32 才是编译器预定义宏。故在 _WIN32
 * 已设时确保定义 WIN32,以便独立构建。
 */
#if defined(_WIN32) && !defined(WIN32)
#define WIN32
#endif

/*
 * 确保 _WIN32_WINNT 具有最低要求值。
 * 保留更高值。最低要求为 Windows 10。
 */
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif

#define _WIN32_WINNT 0x0A00

/*
 * 需防止 <crtdefs.h> 定义与我们 errcode() 冲突的
 * 符号。因其可能被标准系统头包含,故
 * 现抢先包含。
 */
#if defined(_MSC_VER) || defined(HAVE_CRTDEFS_H)
#define errcode __msvc_errcode
#include <crtdefs.h>
#undef errcode
#endif

/*
 * Win32 平台动态链接相关定义
 */

/*
 * Variables declared in the core backend and referenced by loadable
 * modules need to be marked "dllimport" in the core build, but
 * "dllexport" when the declaration is read in a loadable module.
 * No special markings should be used when compiling frontend code.
 */
#ifndef FRONTEND
#ifdef BUILDING_DLL
#define PGDLLIMPORT __declspec (dllexport)
#else
#define PGDLLIMPORT __declspec (dllimport)
#endif
#endif

/*
 * Functions exported by a loadable module must be marked "dllexport".
 *
 * While mingw would otherwise fall back to
 * __attribute__((visibility("default"))), that appears to only work as long
 * as no symbols are declared with __declspec(dllexport). But we can end up
 * with some, e.g. plpython's Py_Init.
 */
#define PGDLLEXPORT __declspec (dllexport)
