/*-------------------------------------------------------------------------
 *
 * relptr.h
 *	  相对指针的基本声明。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/relptr.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef RELPTR_H
#define RELPTR_H

/*
 * 相对指针用于存放可能相对于进程地址空间基址
 * 或映射于其中的某动态共享内存段的地址。
 * 见上。
 *
 * 用法是把相对指针声明为 relptr(type)，再用
 * relptr_access 解引用、用 relptr_store 修改。
 * 此处用联合是权宜之计，因为 relptr 中存的
 * 始终是 Size，从不是真正指针。但联合中含指针
 * 使我们能用拙劣宏技巧提供某种类型安全。
 * 见上。
 */
#define relptr(type)	 union { type *relptr_type; Size relptr_off; }

/*
 * pgindent gets confused by declarations that use "relptr(type)" directly,
 * so preferred style is to write
 *		typedef struct ... SomeStruct;
 *		relptr_declare(SomeStruct, RelptrSomeStruct);
 * and then declare pointer variables as "RelptrSomeStruct someptr".
 */
#define relptr_declare(type, relptrtype) \
	typedef relptr(type) relptrtype

#ifdef HAVE__BUILTIN_TYPES_COMPATIBLE_P
#define relptr_access(base, rp) \
	(AssertVariableIsOfTypeMacro(base, char *), \
	 (__typeof__((rp).relptr_type)) ((rp).relptr_off == 0 ? NULL : \
		(base) + (rp).relptr_off - 1))
#else
/*
 * 若无 __builtin_types_compatible_p，假定也可能无
 * __typeof__。
 */
#define relptr_access(base, rp) \
	(AssertVariableIsOfTypeMacro(base, char *), \
	 (void *) ((rp).relptr_off == 0 ? NULL : (base) + (rp).relptr_off - 1))
#endif

#define relptr_is_null(rp) \
	((rp).relptr_off == 0)

#define relptr_offset(rp) \
	((rp).relptr_off - 1)

/* 用此内联以免 relptr_store 中对 "val" 双重求值 */
static inline Size
relptr_store_eval(char *base, char *val)
{
	if (val == NULL)
		return 0;
	else
	{
		Assert(val >= base);
		return val - base + 1;
	}
}

#ifdef HAVE__BUILTIN_TYPES_COMPATIBLE_P
#define relptr_store(base, rp, val) \
	(AssertVariableIsOfTypeMacro(base, char *), \
	 AssertVariableIsOfTypeMacro(val, __typeof__((rp).relptr_type)), \
	 (rp).relptr_off = relptr_store_eval((base), (char *) (val)))
#else
/*
 * 若无 __builtin_types_compatible_p，假定也可能无
 * __typeof__。
 */
#define relptr_store(base, rp, val) \
	(AssertVariableIsOfTypeMacro(base, char *), \
	 (rp).relptr_off = relptr_store_eval((base), (char *) (val)))
#endif

#define relptr_copy(rp1, rp2) \
	((rp1).relptr_off = (rp2).relptr_off)

#endif							/* RELPTR_H */
