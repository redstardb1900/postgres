/*
 * src/include/port/aix.h
 */
#define CLASS_CONFLICT
#define DISABLE_XOPEN_NLS

/*
 * "IBM XL C/C++ for AIX, V12.1" 在 32 位下会误编译某些
 * ginCompareItemPointers() "long long" 算术的内联展开。
 * 为利用内联,请构建 64 位 PostgreSQL。
 */
#if defined(__ILP32__) && defined(__IBMC__)
#define PG_FORCE_DISABLE_INLINE
#endif
