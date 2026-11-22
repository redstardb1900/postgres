/* src/include/port/cygwin.h */

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
 * Cygwin 的 strtof() 字面就是 (float)strtod(),因而会
 * 误舍入 _and_ 静默上/下溢。用我们的包装不能
 * 修误舍入但能修错误检查,从而减少所需
 * 测试变体文件数量。
 */
#define HAVE_BUGGY_STRTOF 1
