/*-------------------------------------------------------------------------
 *
 * c.h
 *	基本 C 定义。PostgreSQL 中每个 .c 文件皆经
 *	postgres.h 或 postgres_fe.h（视情形）包含本文件。
 *
 *	注意此处定义无意暴露给前端接口库的
 *	客户端——故不太担心用大量内容
 *	污染命名空间…
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/c.h
 *
 *-------------------------------------------------------------------------
 */
/*
 *----------------------------------------------------------------
 *	 TABLE OF CONTENTS
 *
 *		When adding stuff to this file, please try to put stuff
 *		into the relevant section, or add new sections as appropriate.
 *
 *	  section	description
 *	  -------	------------------------------------------------
 *		0)		pg_config.h and standard system headers
 *		1)		compiler characteristics
 *		2)		bool, true, false
 *		3)		standard system types
 *		4)		IsValid macros for system types
 *		5)		lengthof, alignment
 *		6)		assertions
 *		7)		widely useful macros
 *		8)		random stuff
 *		9)		system-specific hacks
 *
 * NOTE: since this file is included by both frontend and backend modules,
 * it's usually wrong to put an "extern" declaration here, unless it's
 * ifdef'd so that it's seen in only one case or the other.
 * typedefs and macros are the kind of thing that might go here.
 *
 *----------------------------------------------------------------
 */
#ifndef C_H
#define C_H

#include "postgres_ext.h"

/* 含 pg_config.h 前须先 undef pg_config_ext.h 符号 */
#undef PG_INT64_TYPE

#include "pg_config.h"
#include "pg_config_manual.h"	/* 须在 pg_config.h 之后 */
#include "pg_config_os.h"		/* 须在任何系统头文件之前 */

/* Postgres 各处皆应可用的系统头文件 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#if defined(WIN32) || defined(__CYGWIN__)
#include <fcntl.h>				/* 确保 O_BINARY 可用 */
#endif
#include <locale.h>
#ifdef ENABLE_NLS
#include <libintl.h>
#endif


/* ----------------------------------------------------------------
 *	第 1 节：编译器特性
 *
 * 类型前缀(const/signed/volatile/inline)在 pg_config.h 中处理。
 * ----------------------------------------------------------------
 */

/*
 * 若定义 PG_FORCE_DISABLE_INLINE 则禁用 "inline"。
 * 用于绕过编译器缺陷，亦可能有助于
 * 调查目的。
 */
#ifdef PG_FORCE_DISABLE_INLINE
#undef inline
#define inline
#endif

/*
 * 属性宏
 *
 * GCC: https://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
 * GCC: https://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html
 * Clang: https://clang.llvm.org/docs/AttributeReference.html
 * Sunpro: https://docs.oracle.com/cd/E18659_01/html/821-1384/gjzke.html
 * XLC: https://www.ibm.com/support/knowledgecenter/SSGH2K_13.1.2/com.ibm.xlc131.aix.doc/language_ref/function_attributes.html
 * XLC: https://www.ibm.com/support/knowledgecenter/SSGH2K_13.1.2/com.ibm.xlc131.aix.doc/language_ref/type_attrib.html
 */

/*
 * 对不支持 __has_attribute 的编译器，我们仅把
 * __has_attribute(x) 定义为 0，以便下文更易定义各种
 * __attribute__s 宏。
 */
#ifndef __has_attribute
#define __has_attribute(attribute) 0
#endif

/* 仅 GCC 支持 unused 属性 */
#ifdef __GNUC__
#define pg_attribute_unused() __attribute__((unused))
#else
#define pg_attribute_unused()
#endif

/*
 * pg_nodiscard 表示若忽略函数调用结果，编译器应警告。
 * 名 "nodiscard" 与（可能将来的）C/C++ 标准对齐。
 * 为最大兼容，用作函数声明说明符，放在
 * 返回类型之前。
 */
#ifdef __GNUC__
#define pg_nodiscard __attribute__((warn_unused_result))
#else
#define pg_nodiscard
#endif

/*
 * 放在允许不对齐访问的函数之前。
 * 在非 x86 专用代码上使用前请三思！
 * 可用 clang 对齐 sanitize 选项测试，
 * 或 gcc 对齐 sanitize 选项。
 */
#if __clang_major__ >= 7 || __GNUC__ >= 8
#define pg_attribute_no_sanitize_alignment() __attribute__((no_sanitize("alignment")))
#else
#define pg_attribute_no_sanitize_alignment()
#endif

/*
 * pg_attribute_nonnull means the compiler should warn if the function is
 * called with the listed arguments set to NULL.  If no arguments are
 * listed, the compiler should warn if any pointer arguments are set to NULL.
 */
#if __has_attribute (nonnull)
#define pg_attribute_nonnull(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define pg_attribute_nonnull(...)
#endif

/*
 * 仅在启用断言的构建中使用的变量定义后追加
 * PG_USED_FOR_ASSERTS_ONLY，以免在禁用断言的构建中
 * 出现未使用变量警告。
 */
#ifdef USE_ASSERT_CHECKING
#define PG_USED_FOR_ASSERTS_ONLY
#else
#define PG_USED_FOR_ASSERTS_ONLY pg_attribute_unused()
#endif

/* GCC 与 XLC 支持 format 属性 */
#if defined(__GNUC__) || defined(__IBMC__)
#define pg_attribute_format_arg(a) __attribute__((format_arg(a)))
#define pg_attribute_printf(f,a) __attribute__((format(PG_PRINTF_ATTRIBUTE, f, a)))
#else
#define pg_attribute_format_arg(a)
#define pg_attribute_printf(f,a)
#endif

/* GCC、Sunpro、XLC 支持 aligned、packed、noreturn */
#if defined(__GNUC__) || defined(__SUNPRO_C) || defined(__IBMC__)
#define pg_attribute_aligned(a) __attribute__((aligned(a)))
#define pg_attribute_noreturn() __attribute__((noreturn))
#define pg_attribute_packed() __attribute__((packed))
#define HAVE_PG_ATTRIBUTE_NORETURN 1
#elif defined(_MSC_VER)
/*
 * MSVC supports aligned.  noreturn is also possible but in MSVC it is
 * declared before the definition while pg_attribute_noreturn() macro
 * is currently used after the definition.
 *
 * Packing is also possible but only by wrapping the entire struct definition
 * which doesn't fit into our current macro declarations.
 */
#define pg_attribute_aligned(a) __declspec(align(a))
#define pg_attribute_noreturn()
#else
/*
 * NB：aligned 与 packed 不给默认定义，因其影响
 * 代码功能；要用则*必须*由编译器
 * 实现。
 */
#define pg_attribute_noreturn()
#endif

/*
 * Use "pg_attribute_always_inline" in place of "inline" for functions that
 * we wish to force inlining of, even when the compiler's heuristics would
 * choose not to.  But, if possible, don't force inlining in unoptimized
 * debug builds.
 */
#if (defined(__GNUC__) && __GNUC__ > 3 && defined(__OPTIMIZE__)) || defined(__SUNPRO_C) || defined(__IBMC__)
/* GCC > 3、Sunpro 与 XLC 经 __attribute__ 支持 always_inline */
#define pg_attribute_always_inline __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
/* MSVC 有专用关键字 */
#define pg_attribute_always_inline __forceinline
#else
/* 否则最多只能说 "inline" */
#define pg_attribute_always_inline inline
#endif

/*
 * Forcing a function not to be inlined can be useful if it's the slow path of
 * a performance-critical function, or should be visible in profiles to allow
 * for proper cost attribution.  Note that unlike the pg_attribute_XXX macros
 * above, this should be placed before the function's return type and name.
 */
/* GCC、Sunpro 与 XLC 经 __attribute__ 支持 noinline */
#if (defined(__GNUC__) && __GNUC__ > 2) || defined(__SUNPRO_C) || defined(__IBMC__)
#define pg_noinline __attribute__((noinline))
/* msvc 经 declspec */
#elif defined(_MSC_VER)
#define pg_noinline __declspec(noinline)
#else
#define pg_noinline
#endif

/*
 * For now, just define pg_attribute_cold and pg_attribute_hot to be empty
 * macros on minGW 8.1.  There appears to be a compiler bug that results in
 * compilation failure.  At this time, we still have at least one buildfarm
 * animal running that compiler, so this should make that green again. It's
 * likely this compiler is not popular enough to warrant keeping this code
 * around forever, so let's just remove it once the last buildfarm animal
 * upgrades.
 */
#if defined(__MINGW64__) && __GNUC__ == 8 && __GNUC_MINOR__ == 1

#define pg_attribute_cold
#define pg_attribute_hot

#else
/*
 * 将某些函数标为 "hot" 或 "cold" 有助于编译器把
 * 汇编排得更高效。
 */
#if __has_attribute (cold)
#define pg_attribute_cold __attribute__((cold))
#else
#define pg_attribute_cold
#endif

#if __has_attribute (hot)
#define pg_attribute_hot __attribute__((hot))
#else
#define pg_attribute_hot
#endif

#endif							/* defined(__MINGW64__) && __GNUC__ == 8 &&
								 * __GNUC_MINOR__ == 1 */
/*
 * 以可移植方式标出不可达点。最好是编译器能
 * 理解的东西，以助代码生成。
 * 启用断言的构建中，为调试更愿用 abort()。
 */
#if defined(HAVE__BUILTIN_UNREACHABLE) && !defined(USE_ASSERT_CHECKING)
#define pg_unreachable() __builtin_unreachable()
#elif defined(_MSC_VER) && !defined(USE_ASSERT_CHECKING)
#define pg_unreachable() __assume(0)
#else
#define pg_unreachable() abort()
#endif

/*
 * 向编译器提示分支可能性。likely() 与
 * unlikely() 皆返回所含表达式的布尔值。
 *
 * 仅应在极热路径上少用。很容易
 * 误估可能性。
 */
#if __GNUC__ >= 3
#define likely(x)	__builtin_expect((x) != 0, 1)
#define unlikely(x) __builtin_expect((x) != 0, 0)
#else
#define likely(x)	((x) != 0)
#define unlikely(x) ((x) != 0)
#endif

/*
 * CppAsString
 *	用 C 预处理器把参数转为字符串。
 * CppAsString2
 *	宏展开一轮后再把参数转为字符串。
 * CppConcat
 *	用 C 预处理器拼接两个参数。
 *
 * 注意：昔日曾支持无 # 与 ## 的前 ANSI C 编译器。
 * 如今这些宏仅为清晰及/或与既有
 * PostgreSQL 代码向后兼容。
 */
#define CppAsString(identifier) #identifier
#define CppAsString2(x)			CppAsString(x)
#define CppConcat(x, y)			x##y

/*
 * VA_ARGS_NARGS
 *	返回传入的宏参数个数。
 *
 * 空参数仍计为一个，故实际是
 * "参数表中逗号数加一"。
 *
 * 最多支持 63 个参数。内部向 VA_ARGS_NARGS_() 传入
 * 64+N 个参数，而 C99 仅要求宏最多允许
 * 127 个，故无法可移植地再高。实现很
 * 简单：VA_ARGS_NARGS_() 返回其第 64 个参数，我们安排
 * 调用使该位恰为常数表中合适的一项。
 * 此主意出自 Laurent Deniau。
 *
 * MSVC 的 __VA_ARGS__ 实现不合标准，除非用
 * /Zc:preprocessor 开关，但 VS 2019 前
 * 不可用。暂用另一套在旧编译器上
 * 亦可用的定义。
 */
#ifdef _MSC_VER
#define EXPAND(args) args
#define VA_ARGS_NARGS(...) \
	VA_ARGS_NARGS_ EXPAND((__VA_ARGS__, \
				   63,62,61,60,                   \
				   59,58,57,56,55,54,53,52,51,50, \
				   49,48,47,46,45,44,43,42,41,40, \
				   39,38,37,36,35,34,33,32,31,30, \
				   29,28,27,26,25,24,23,22,21,20, \
				   19,18,17,16,15,14,13,12,11,10, \
				   9, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#else

#define VA_ARGS_NARGS(...) \
	VA_ARGS_NARGS_(__VA_ARGS__, \
				   63,62,61,60,                   \
				   59,58,57,56,55,54,53,52,51,50, \
				   49,48,47,46,45,44,43,42,41,40, \
				   39,38,37,36,35,34,33,32,31,30, \
				   29,28,27,26,25,24,23,22,21,20, \
				   19,18,17,16,15,14,13,12,11,10, \
				   9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#endif

#define VA_ARGS_NARGS_( \
	_01,_02,_03,_04,_05,_06,_07,_08,_09,_10, \
	_11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
	_21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
	_31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
	_41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
	_51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
	_61,_62,_63,  N, ...) \
	(N)

/*
 * 通用函数指针。罕见情形下须把函数指针转为看似
 * 不兼容的函数指针类型，并避开 gcc 的
 * -Wcast-function-type 警告时可用。
 */
typedef void (*pg_funcptr_t) (void);

/*
 * 我们要求 C99，故编译器应理解柔性数组成员。
 * 但为文档目的，项目风格仍写
 * "field[FLEXIBLE_ARRAY_MEMBER]" 而非仅 "field[]"。
 * 计算此类对象大小时，为可移植性用
 * "offsetof(struct s, f)"。勿用 "offsetof(struct s, f[0])"，因
 * MSVC 与 C++ 编译器上不可用。
 */
#define FLEXIBLE_ARRAY_MEMBER	/* 空 */

/*
 * 编译器是否支持 #pragma GCC system_header？我们可选地用它
 * 避免无法修复的警告（如 perl 头中）。
 * See https://gcc.gnu.org/onlinedocs/cpp/System-Headers.html
 *
 * 不想显示编译器警告的头可有条件地
 * 用 #pragma GCC system_header 避免警告。显然
 * 仅应用于我们无法控制的外部头。
 *
 *
 * 此处测试 pragma 支持，而非在 configure 中，因 gcc
 * 亦会对 .c 文件中使用该 pragma 发出警告。让 autoconf
 * 用 .h 作文件后缀出奇地难。gcc 自
 * 约 2000 年起即实现该 pragma，故本测试应够。
 *
 *
 * 或者，可用 -isystem 加入有问题头的包含路径，
 * 但那是更大的锤子，也更难搜索。
 *
 * 更细粒度可用 #pragma GCC diagnostic
 * push/ignored/pop，但 gcc 会对忽略未知警告再警告，故
 * 每个临时忽略的警告都需自有 pg_config.h
 * 符号与 #ifdef。
 */
#ifdef __GNUC__
#define HAVE_PRAGMA_GCC_SYSTEM_HEADER	1
#endif


/* ----------------------------------------------------------------
 *				Section 2:	bool, true, false
 * ----------------------------------------------------------------
 */

/*
 * bool
 *	布尔值，为 true 或 false。
 *
 * 若含入后 bool 大小为 1，则用 stdbool.h。利于
 * 更好的编译器与调试器输出，并与第三方
 * 库兼容。但 PostgreSQL 目前无法处理其他大小的 bool；
 * 代码中有静态断言防止之。
 *
 * 对 C++ 编译器，假定其有兼容的内建
 * bool 定义。
 *
 * 亦见 src/interfaces/ecpg/include/ecpglib.h 中的对应代码。
 */

#ifndef __cplusplus

#ifdef PG_USE_STDBOOL
#include <stdbool.h>
#else

#ifndef bool
typedef unsigned char bool;
#endif

#ifndef true
#define true	((bool) 1)
#endif

#ifndef false
#define false	((bool) 0)
#endif

#endif							/* not PG_USE_STDBOOL */
#endif							/* not C++ */


/* ----------------------------------------------------------------
 *	第 3 节：	标准系统类型
 * ----------------------------------------------------------------
 */

/*
 * Pointer
 *	保存任一内存驻留对象地址的变量。
 *
 *	XXX 用此做指针算术，故在"真" ANSI 编译器下
 *	不能是 void *。
 */
typedef char *Pointer;

/*
 * intN
 *		Signed integer, EXACTLY N BITS IN SIZE,
 *		used for numerical computations and the
 *		frontend/backend protocol.
 */
#ifndef HAVE_INT8
typedef signed char int8;		/* == 8 位 */
typedef signed short int16;		/* == 16 位 */
typedef signed int int32;		/* == 32 位 */
#endif							/* not HAVE_INT8 */

/*
 * uintN
 *		Unsigned integer, EXACTLY N BITS IN SIZE,
 *		used for numerical computations and the
 *		frontend/backend protocol.
 */
#ifndef HAVE_UINT8
typedef unsigned char uint8;	/* == 8 位 */
typedef unsigned short uint16;	/* == 16 位 */
typedef unsigned int uint32;	/* == 32 位 */
#endif							/* not HAVE_UINT8 */

/*
 * bitsN
 *	按位运算单位，AT LEAST N BITS IN SIZE。
 */
typedef uint8 bits8;			/* >= 8 位 */
typedef uint16 bits16;			/* >= 16 位 */
typedef uint32 bits32;			/* >= 32 位 */

/*
 * 64 位整数
 */
#ifdef HAVE_LONG_INT_64
/* 普通 long 够用则用之 */

#ifndef HAVE_INT64
typedef long int int64;
#endif
#ifndef HAVE_UINT64
typedef unsigned long int uint64;
#endif
#define INT64CONST(x)  (x##L)
#define UINT64CONST(x) (x##UL)
#elif defined(HAVE_LONG_LONG_INT_64)
/* We have working support for "long long int", use that */

#ifndef HAVE_INT64
typedef long long int int64;
#endif
#ifndef HAVE_UINT64
typedef unsigned long long int uint64;
#endif
#define INT64CONST(x)  (x##LL)
#define UINT64CONST(x) (x##ULL)
#else
/* neither HAVE_LONG_INT_64 nor HAVE_LONG_LONG_INT_64 */
#error must have a working 64-bit integer datatype
#endif

/* 64 位整数所用的 snprintf 格式串 */
#define INT64_FORMAT "%" INT64_MODIFIER "d"
#define UINT64_FORMAT "%" INT64_MODIFIER "u"

/*
 * 128 位有符号与无符号整数
 *	对此类类型目前仅有有限支持。
 *	例如不支持 128 位字面量与 snprintf；但支持算术。
 *	又因选择 MAXIMUM_ALIGNOF 时排除此类类型，
 *	必须能迫使编译器将其分配在不超过
 *	MAXALIGN 的边界上。
 */
#if defined(PG_INT128_TYPE)
#if defined(pg_attribute_aligned) || ALIGNOF_PG_INT128_TYPE <= MAXIMUM_ALIGNOF
#define HAVE_INT128 1

typedef PG_INT128_TYPE int128
#if defined(pg_attribute_aligned)
			pg_attribute_aligned(MAXIMUM_ALIGNOF)
#endif
		   ;

typedef unsigned PG_INT128_TYPE uint128
#if defined(pg_attribute_aligned)
			pg_attribute_aligned(MAXIMUM_ALIGNOF)
#endif
		   ;

#endif
#endif

/*
 * stdint.h 的限值不保证与我们的定宽类型类型兼容。
 * 故自行定义。
 */
#define PG_INT8_MIN		(-0x7F-1)
#define PG_INT8_MAX		(0x7F)
#define PG_UINT8_MAX	(0xFF)
#define PG_INT16_MIN	(-0x7FFF-1)
#define PG_INT16_MAX	(0x7FFF)
#define PG_UINT16_MAX	(0xFFFF)
#define PG_INT32_MIN	(-0x7FFFFFFF-1)
#define PG_INT32_MAX	(0x7FFFFFFF)
#define PG_UINT32_MAX	(0xFFFFFFFFU)
#define PG_INT64_MIN	(-INT64CONST(0x7FFFFFFFFFFFFFFF) - 1)
#define PG_INT64_MAX	INT64CONST(0x7FFFFFFFFFFFFFFF)
#define PG_UINT64_MAX	UINT64CONST(0xFFFFFFFFFFFFFFFF)

/*
 * 现总用 int64 时间戳，但保留本符号，便于
 * 可能测试它的外部代码。
 */
#define HAVE_INT64_TIMESTAMP

/*
 * Size
 *	sizeof 返回的任一内存驻留对象的大小。
 */
typedef size_t Size;

/*
 * Index
 *	内存驻留数组中的下标。
 *
 * Note:
 *	下标非负。
 */
typedef unsigned int Index;

/*
 * Offset
 *	任一内存驻留数组中的偏移。
 *
 * Note:
 *	与 Index 的差别在于 Index 总是
 *	非负，而 Offset 可为负。
 */
typedef signed int Offset;

/*
 * 常用 Postgres 数据类型名（目录中所用）
 */
typedef float float4;
typedef double float8;

#ifdef USE_FLOAT8_BYVAL
#define FLOAT8PASSBYVAL true
#else
#define FLOAT8PASSBYVAL false
#endif

/*
 * Oid, RegProcedure, TransactionId, SubTransactionId, MultiXactId,
 * CommandId
 */

/* typedef Oid is in postgres_ext.h */

/*
 * include/catalog 头中用类型名 regproc，但
 * C 代码中更宜用 RegProcedure。
 */
typedef Oid regproc;
typedef regproc RegProcedure;

typedef uint32 TransactionId;

typedef uint32 LocalTransactionId;

typedef uint32 SubTransactionId;

#define InvalidSubTransactionId		((SubTransactionId) 0)
#define TopSubTransactionId			((SubTransactionId) 1)

/* MultiXactId 须等价于 TransactionId，以装入 t_xmax */
typedef TransactionId MultiXactId;

typedef uint32 MultiXactOffset;

typedef uint32 CommandId;

#define FirstCommandId	((CommandId) 0)
#define InvalidCommandId	(~(CommandId)0)


/* ----------------
 *	变长类型皆共享 varlena 头。
 *
 * 注意：对 TOASTable 类型，这是简化，因值可能
 * 被压缩或移出线。但类型专用例程
 * 多半只处理去 TOAST 的值，且客户端
 * 例程当然不应看到 TOASTed 值。即便在
 * 去 TOAST 值中，亦勿直接碰 vl_len_，因其
 * 表示已不再方便。建议代码总是
 * 用宏 VARDATA_ANY、VARSIZE_ANY、VARSIZE_ANY_EXHDR、VARDATA、VARSIZE
 * 与 SET_VARSIZE，而不依赖直接提结构字段。
 * TOAST 形态细节见 postgres.h。
 * ----------------
 */
struct varlena
{
	char		vl_len_[4];		/* Do not touch this field directly! */
	char		vl_dat[FLEXIBLE_ARRAY_MEMBER];	/* Data content is here */
};

#define VARHDRSZ		((int32) sizeof(int32))

/*
 * These widely-used datatypes are just a varlena header and the data bytes.
 * There is no terminating null or anything like that --- the data length is
 * always VARSIZE_ANY_EXHDR(ptr).
 */
typedef struct varlena bytea;
typedef struct varlena text;
typedef struct varlena BpChar;	/* blank-padded char, ie SQL char(n) */
typedef struct varlena VarChar; /* var-length char, ie SQL varchar(n) */

/*
 * Specialized array types.  These are physically laid out just the same
 * as regular arrays (so that the regular array subscripting code works
 * with them).  They exist as distinct types mostly for historical reasons:
 * they have nonstandard I/O behavior which we don't want to change for fear
 * of breaking applications that look at the system catalogs.  There is also
 * an implementation issue for oidvector: it's part of the primary key for
 * pg_proc, and we can't use the normal btree array support routines for that
 * without circularity.
 */
typedef struct
{
	int32		vl_len_;		/* these fields must match ArrayType! */
	int			ndim;			/* always 1 for int2vector */
	int32		dataoffset;		/* always 0 for int2vector */
	Oid			elemtype;
	int			dim1;
	int			lbound1;
	int16		values[FLEXIBLE_ARRAY_MEMBER];
} int2vector;

typedef struct
{
	int32		vl_len_;		/* these fields must match ArrayType! */
	int			ndim;			/* always 1 for oidvector */
	int32		dataoffset;		/* always 0 for oidvector */
	Oid			elemtype;
	int			dim1;
	int			lbound1;
	Oid			values[FLEXIBLE_ARRAY_MEMBER];
} oidvector;

/*
 * Representation of a Name: effectively just a C string, but null-padded to
 * exactly NAMEDATALEN bytes.  The use of a struct is historical.
 */
typedef struct nameData
{
	char		data[NAMEDATALEN];
} NameData;
typedef NameData *Name;

#define NameStr(name)	((name).data)


/* ----------------------------------------------------------------
 *				Section 4:	IsValid macros for system types
 * ----------------------------------------------------------------
 */
/*
 * BoolIsValid
 *		True iff bool is valid.
 */
#define BoolIsValid(boolean)	((boolean) == false || (boolean) == true)

/*
 * PointerIsValid
 *		True iff pointer is valid.
 */
#define PointerIsValid(pointer) ((const void*)(pointer) != NULL)

/*
 * PointerIsAligned
 *		True iff pointer is properly aligned to point to the given type.
 */
#define PointerIsAligned(pointer, type) \
		(((uintptr_t)(pointer) % (sizeof (type))) == 0)

#define OffsetToPointer(base, offset) \
		((void *)((char *) base + offset))

#define OidIsValid(objectId)  ((bool) ((objectId) != InvalidOid))

#define RegProcedureIsValid(p)	OidIsValid(p)


/* ----------------------------------------------------------------
 *				Section 5:	lengthof, alignment
 * ----------------------------------------------------------------
 */
/*
 * lengthof
 *		Number of elements in an array.
 */
#define lengthof(array) (sizeof (array) / sizeof ((array)[0]))

/* ----------------
 * Alignment macros: align a length or address appropriately for a given type.
 * The fooALIGN() macros round up to a multiple of the required alignment,
 * while the fooALIGN_DOWN() macros round down.  The latter are more useful
 * for problems like "how many X-sized structures will fit in a page?".
 *
 * NOTE: TYPEALIGN[_DOWN] will not work if ALIGNVAL is not a power of 2.
 * That case seems extremely unlikely to be needed in practice, however.
 *
 * NOTE: MAXIMUM_ALIGNOF, and hence MAXALIGN(), intentionally exclude any
 * larger-than-8-byte types the compiler might have.
 * ----------------
 */

#define TYPEALIGN(ALIGNVAL,LEN)  \
	(((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) & ~((uintptr_t) ((ALIGNVAL) - 1)))

#define SHORTALIGN(LEN)			TYPEALIGN(ALIGNOF_SHORT, (LEN))
#define INTALIGN(LEN)			TYPEALIGN(ALIGNOF_INT, (LEN))
#define LONGALIGN(LEN)			TYPEALIGN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN(LEN)		TYPEALIGN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN(LEN)			TYPEALIGN(MAXIMUM_ALIGNOF, (LEN))
/* MAXALIGN covers only built-in types, not buffers */
#define BUFFERALIGN(LEN)		TYPEALIGN(ALIGNOF_BUFFER, (LEN))
#define CACHELINEALIGN(LEN)		TYPEALIGN(PG_CACHE_LINE_SIZE, (LEN))

#define TYPEALIGN_DOWN(ALIGNVAL,LEN)  \
	(((uintptr_t) (LEN)) & ~((uintptr_t) ((ALIGNVAL) - 1)))

#define SHORTALIGN_DOWN(LEN)	TYPEALIGN_DOWN(ALIGNOF_SHORT, (LEN))
#define INTALIGN_DOWN(LEN)		TYPEALIGN_DOWN(ALIGNOF_INT, (LEN))
#define LONGALIGN_DOWN(LEN)		TYPEALIGN_DOWN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN_DOWN(LEN)	TYPEALIGN_DOWN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN_DOWN(LEN)		TYPEALIGN_DOWN(MAXIMUM_ALIGNOF, (LEN))
#define BUFFERALIGN_DOWN(LEN)	TYPEALIGN_DOWN(ALIGNOF_BUFFER, (LEN))

/*
 * The above macros will not work with types wider than uintptr_t, like with
 * uint64 on 32-bit platforms.  That's not problem for the usual use where a
 * pointer or a length is aligned, but for the odd case that you need to
 * align something (potentially) wider, use TYPEALIGN64.
 */
#define TYPEALIGN64(ALIGNVAL,LEN)  \
	(((uint64) (LEN) + ((ALIGNVAL) - 1)) & ~((uint64) ((ALIGNVAL) - 1)))

/* we don't currently need wider versions of the other ALIGN macros */
#define MAXALIGN64(LEN)			TYPEALIGN64(MAXIMUM_ALIGNOF, (LEN))


/* ----------------------------------------------------------------
 *				Section 6:	assertions
 * ----------------------------------------------------------------
 */

/*
 * USE_ASSERT_CHECKING, if defined, turns on all the assertions.
 * - plai  9/5/90
 *
 * It should _NOT_ be defined in releases or in benchmark copies
 */

/*
 * Assert() can be used in both frontend and backend code. In frontend code it
 * just calls the standard assert, if it's available. If use of assertions is
 * not configured, it does nothing.
 */
#ifndef USE_ASSERT_CHECKING

#define Assert(condition)	((void)true)
#define AssertMacro(condition)	((void)true)

#elif defined(FRONTEND)

#include <assert.h>
#define Assert(p) assert(p)
#define AssertMacro(p)	((void) assert(p))

#else							/* USE_ASSERT_CHECKING && !FRONTEND */

/*
 * Assert
 *		Generates a fatal exception if the given condition is false.
 */
#define Assert(condition) \
	do { \
		if (!(condition)) \
			ExceptionalCondition(#condition, __FILE__, __LINE__); \
	} while (0)

/*
 * AssertMacro is the same as Assert but it's suitable for use in
 * expression-like macros, for example:
 *
 *		#define foo(x) (AssertMacro(x != 0), bar(x))
 */
#define AssertMacro(condition) \
	((void) ((condition) || \
			 (ExceptionalCondition(#condition, __FILE__, __LINE__), 0)))

#endif							/* USE_ASSERT_CHECKING && !FRONTEND */

/*
 * Check that `ptr' is `bndr' aligned.
 */
#define AssertPointerAlignment(ptr, bndr) \
	Assert(TYPEALIGN(bndr, (uintptr_t)(ptr)) == (uintptr_t)(ptr))

/*
 * ExceptionalCondition is compiled into the backend whether or not
 * USE_ASSERT_CHECKING is defined, so as to support use of extensions
 * that are built with that #define with a backend that isn't.  Hence,
 * we should declare it as long as !FRONTEND.
 */
#ifndef FRONTEND
extern void ExceptionalCondition(const char *conditionName,
								 const char *fileName, int lineNumber) pg_attribute_noreturn();
#endif

/*
 * Macros to support compile-time assertion checks.
 *
 * If the "condition" (a compile-time-constant expression) evaluates to false,
 * throw a compile error using the "errmessage" (a string literal).
 *
 * C11 has _Static_assert(), and most C99 compilers already support that.  For
 * portability, we wrap it into StaticAssertDecl().  _Static_assert() is a
 * "declaration", and so it must be placed where for example a variable
 * declaration would be valid.  As long as we compile with
 * -Wno-declaration-after-statement, that also means it cannot be placed after
 * statements in a function.  Macros StaticAssertStmt() and StaticAssertExpr()
 * make it safe to use as a statement or in an expression, respectively.
 *
 * For compilers without _Static_assert(), we fall back on a kluge that
 * assumes the compiler will complain about a negative width for a struct
 * bit-field.  This will not include a helpful error message, but it beats not
 * getting an error at all.
 */
#ifndef __cplusplus
#ifdef HAVE__STATIC_ASSERT
#define StaticAssertDecl(condition, errmessage) \
	_Static_assert(condition, errmessage)
#define StaticAssertStmt(condition, errmessage) \
	do { _Static_assert(condition, errmessage); } while(0)
#define StaticAssertExpr(condition, errmessage) \
	((void) ({ StaticAssertStmt(condition, errmessage); true; }))
#else							/* !HAVE__STATIC_ASSERT */
#define StaticAssertDecl(condition, errmessage) \
	extern void static_assert_func(int static_assert_failure[(condition) ? 1 : -1])
#define StaticAssertStmt(condition, errmessage) \
	((void) sizeof(struct { int static_assert_failure : (condition) ? 1 : -1; }))
#define StaticAssertExpr(condition, errmessage) \
	StaticAssertStmt(condition, errmessage)
#endif							/* HAVE__STATIC_ASSERT */
#else							/* C++ */
#if defined(__cpp_static_assert) && __cpp_static_assert >= 200410
#define StaticAssertDecl(condition, errmessage) \
	static_assert(condition, errmessage)
#define StaticAssertStmt(condition, errmessage) \
	static_assert(condition, errmessage)
#define StaticAssertExpr(condition, errmessage) \
	({ static_assert(condition, errmessage); })
#else							/* !__cpp_static_assert */
#define StaticAssertDecl(condition, errmessage) \
	extern void static_assert_func(int static_assert_failure[(condition) ? 1 : -1])
#define StaticAssertStmt(condition, errmessage) \
	do { struct static_assert_struct { int static_assert_failure : (condition) ? 1 : -1; }; } while(0)
#define StaticAssertExpr(condition, errmessage) \
	((void) ({ StaticAssertStmt(condition, errmessage); }))
#endif							/* __cpp_static_assert */
#endif							/* C++ */


/*
 * Compile-time checks that a variable (or expression) has the specified type.
 *
 * AssertVariableIsOfType() can be used as a statement.
 * AssertVariableIsOfTypeMacro() is intended for use in macros, eg
 *		#define foo(x) (AssertVariableIsOfTypeMacro(x, int), bar(x))
 *
 * If we don't have __builtin_types_compatible_p, we can still assert that
 * the types have the same size.  This is far from ideal (especially on 32-bit
 * platforms) but it provides at least some coverage.
 */
#ifdef HAVE__BUILTIN_TYPES_COMPATIBLE_P
#define AssertVariableIsOfType(varname, typename) \
	StaticAssertStmt(__builtin_types_compatible_p(__typeof__(varname), typename), \
	CppAsString(varname) " does not have type " CppAsString(typename))
#define AssertVariableIsOfTypeMacro(varname, typename) \
	(StaticAssertExpr(__builtin_types_compatible_p(__typeof__(varname), typename), \
	 CppAsString(varname) " does not have type " CppAsString(typename)))
#else							/* !HAVE__BUILTIN_TYPES_COMPATIBLE_P */
#define AssertVariableIsOfType(varname, typename) \
	StaticAssertStmt(sizeof(varname) == sizeof(typename), \
	CppAsString(varname) " does not have type " CppAsString(typename))
#define AssertVariableIsOfTypeMacro(varname, typename) \
	(StaticAssertExpr(sizeof(varname) == sizeof(typename), \
	 CppAsString(varname) " does not have type " CppAsString(typename)))
#endif							/* HAVE__BUILTIN_TYPES_COMPATIBLE_P */


/* ----------------------------------------------------------------
 *				Section 7:	widely useful macros
 * ----------------------------------------------------------------
 */
/*
 * Max
 *		Return the maximum of two numbers.
 */
#define Max(x, y)		((x) > (y) ? (x) : (y))

/*
 * Min
 *		Return the minimum of two numbers.
 */
#define Min(x, y)		((x) < (y) ? (x) : (y))


/* Get a bit mask of the bits set in non-long aligned addresses */
#define LONG_ALIGN_MASK (sizeof(long) - 1)

/*
 * MemSet
 *	Exactly the same as standard library function memset(), but considerably
 *	faster for zeroing small word-aligned structures (such as parsetree nodes).
 *	This has to be a macro because the main point is to avoid function-call
 *	overhead.   However, we have also found that the loop is faster than
 *	native libc memset() on some platforms, even those with assembler
 *	memset() functions.  More research needs to be done, perhaps with
 *	MEMSET_LOOP_LIMIT tests in configure.
 */
#define MemSet(start, val, len) \
	do \
	{ \
		/* must be void* because we don't know if it is integer aligned yet */ \
		void   *_vstart = (void *) (start); \
		int		_val = (val); \
		Size	_len = (len); \
\
		if ((((uintptr_t) _vstart) & LONG_ALIGN_MASK) == 0 && \
			(_len & LONG_ALIGN_MASK) == 0 && \
			_val == 0 && \
			_len <= MEMSET_LOOP_LIMIT && \
			/* \
			 *	If MEMSET_LOOP_LIMIT == 0, optimizer should find \
			 *	the whole "if" false at compile time. \
			 */ \
			MEMSET_LOOP_LIMIT != 0) \
		{ \
			long *_start = (long *) _vstart; \
			long *_stop = (long *) ((char *) _start + _len); \
			while (_start < _stop) \
				*_start++ = 0; \
		} \
		else \
			memset(_vstart, _val, _len); \
	} while (0)

/*
 * MemSetAligned is the same as MemSet except it omits the test to see if
 * "start" is word-aligned.  This is okay to use if the caller knows a-priori
 * that the pointer is suitably aligned (typically, because he just got it
 * from palloc(), which always delivers a max-aligned pointer).
 */
#define MemSetAligned(start, val, len) \
	do \
	{ \
		long   *_start = (long *) (start); \
		int		_val = (val); \
		Size	_len = (len); \
\
		if ((_len & LONG_ALIGN_MASK) == 0 && \
			_val == 0 && \
			_len <= MEMSET_LOOP_LIMIT && \
			MEMSET_LOOP_LIMIT != 0) \
		{ \
			long *_stop = (long *) ((char *) _start + _len); \
			while (_start < _stop) \
				*_start++ = 0; \
		} \
		else \
			memset(_start, _val, _len); \
	} while (0)


/*
 * MemSetTest/MemSetLoop are a variant version that allow all the tests in
 * MemSet to be done at compile time in cases where "val" and "len" are
 * constants *and* we know the "start" pointer must be word-aligned.
 * If MemSetTest succeeds, then it is okay to use MemSetLoop, otherwise use
 * MemSetAligned.  Beware of multiple evaluations of the arguments when using
 * this approach.
 */
#define MemSetTest(val, len) \
	( ((len) & LONG_ALIGN_MASK) == 0 && \
	(len) <= MEMSET_LOOP_LIMIT && \
	MEMSET_LOOP_LIMIT != 0 && \
	(val) == 0 )

#define MemSetLoop(start, val, len) \
	do \
	{ \
		long * _start = (long *) (start); \
		long * _stop = (long *) ((char *) _start + (Size) (len)); \
	\
		while (_start < _stop) \
			*_start++ = 0; \
	} while (0)

/*
 * Macros for range-checking float values before converting to integer.
 * We must be careful here that the boundary values are expressed exactly
 * in the float domain.  PG_INTnn_MIN is an exact power of 2, so it will
 * be represented exactly; but PG_INTnn_MAX isn't, and might get rounded
 * off, so avoid using that.
 * The input must be rounded to an integer beforehand, typically with rint(),
 * else we might draw the wrong conclusion about close-to-the-limit values.
 * These macros will do the right thing for Inf, but not necessarily for NaN,
 * so check isnan(num) first if that's a possibility.
 */
#define FLOAT4_FITS_IN_INT16(num) \
	((num) >= (float4) PG_INT16_MIN && (num) < -((float4) PG_INT16_MIN))
#define FLOAT4_FITS_IN_INT32(num) \
	((num) >= (float4) PG_INT32_MIN && (num) < -((float4) PG_INT32_MIN))
#define FLOAT4_FITS_IN_INT64(num) \
	((num) >= (float4) PG_INT64_MIN && (num) < -((float4) PG_INT64_MIN))
#define FLOAT8_FITS_IN_INT16(num) \
	((num) >= (float8) PG_INT16_MIN && (num) < -((float8) PG_INT16_MIN))
#define FLOAT8_FITS_IN_INT32(num) \
	((num) >= (float8) PG_INT32_MIN && (num) < -((float8) PG_INT32_MIN))
#define FLOAT8_FITS_IN_INT64(num) \
	((num) >= (float8) PG_INT64_MIN && (num) < -((float8) PG_INT64_MIN))


/* ----------------------------------------------------------------
 *				Section 8:	random stuff
 * ----------------------------------------------------------------
 */

/*
 * Invert the sign of a qsort-style comparison result, ie, exchange negative
 * and positive integer values, being careful not to get the wrong answer
 * for INT_MIN.  The argument should be an integral variable.
 */
#define INVERT_COMPARE_RESULT(var) \
	((var) = ((var) < 0) ? 1 : -(var))

/*
 * Use this, not "char buf[BLCKSZ]", to declare a field or local variable
 * holding a page buffer, if that page might be accessed as a page.  Otherwise
 * the variable might be under-aligned, causing problems on alignment-picky
 * hardware.  We include both "double" and "int64" in the union to ensure that
 * the compiler knows the value must be MAXALIGN'ed (cf. configure's
 * computation of MAXIMUM_ALIGNOF).
 */
typedef union PGAlignedBlock
{
	char		data[BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGAlignedBlock;

/*
 * Use this to declare a field or local variable holding a page buffer, if that
 * page might be accessed as a page or passed to an SMgr I/O function.  If
 * allocating using the MemoryContext API, the aligned allocation functions
 * should be used with PG_IO_ALIGN_SIZE.  This alignment may be more efficient
 * for I/O in general, but may be strictly required on some platforms when
 * using direct I/O.
 */
typedef union PGIOAlignedBlock
{
#ifdef pg_attribute_aligned
	pg_attribute_aligned(PG_IO_ALIGN_SIZE)
#endif
	char		data[BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGIOAlignedBlock;

/* Same, but for an XLOG_BLCKSZ-sized buffer */
typedef union PGAlignedXLogBlock
{
#ifdef pg_attribute_aligned
	pg_attribute_aligned(PG_IO_ALIGN_SIZE)
#endif
	char		data[XLOG_BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGAlignedXLogBlock;

/* msb for char */
#define HIGHBIT					(0x80)
#define IS_HIGHBIT_SET(ch)		((unsigned char)(ch) & HIGHBIT)

/*
 * Support macros for escaping strings.  escape_backslash should be true
 * if generating a non-standard-conforming string.  Prefixing a string
 * with ESCAPE_STRING_SYNTAX guarantees it is non-standard-conforming.
 * Beware of multiple evaluation of the "ch" argument!
 */
#define SQL_STR_DOUBLE(ch, escape_backslash)	\
	((ch) == '\'' || ((ch) == '\\' && (escape_backslash)))

#define ESCAPE_STRING_SYNTAX	'E'


#define STATUS_OK				(0)
#define STATUS_ERROR			(-1)
#define STATUS_EOF				(-2)

/*
 * gettext support
 */

#ifndef ENABLE_NLS
/* stuff we'd otherwise get from <libintl.h> */
#define gettext(x) (x)
#define dgettext(d,x) (x)
#define ngettext(s,p,n) ((n) == 1 ? (s) : (p))
#define dngettext(d,s,p,n) ((n) == 1 ? (s) : (p))
#endif

#define _(x) gettext(x)

/*
 *	Use this to mark string constants as needing translation at some later
 *	time, rather than immediately.  This is useful for cases where you need
 *	access to the original string and translated string, and for cases where
 *	immediate translation is not possible, like when initializing global
 *	variables.
 *
 *	https://www.gnu.org/software/gettext/manual/html_node/Special-cases.html
 */
#define gettext_noop(x) (x)

/*
 * To better support parallel installations of major PostgreSQL
 * versions as well as parallel installations of major library soname
 * versions, we mangle the gettext domain name by appending those
 * version numbers.  The coding rule ought to be that wherever the
 * domain name is mentioned as a literal, it must be wrapped into
 * PG_TEXTDOMAIN().  The macros below do not work on non-literals; but
 * that is somewhat intentional because it avoids having to worry
 * about multiple states of premangling and postmangling as the values
 * are being passed around.
 *
 * Make sure this matches the installation rules in nls-global.mk.
 */
#ifdef SO_MAJOR_VERSION
#define PG_TEXTDOMAIN(domain) (domain CppAsString2(SO_MAJOR_VERSION) "-" PG_MAJORVERSION)
#else
#define PG_TEXTDOMAIN(domain) (domain "-" PG_MAJORVERSION)
#endif

/*
 * Macro that allows to cast constness and volatile away from an expression, but doesn't
 * allow changing the underlying type.  Enforcement of the latter
 * currently only works for gcc like compilers.
 *
 * Please note IT IS NOT SAFE to cast constness away if the result will ever
 * be modified (it would be undefined behaviour). Doing so anyway can cause
 * compiler misoptimizations or runtime crashes (modifying readonly memory).
 * It is only safe to use when the result will not be modified, but API
 * design or language restrictions prevent you from declaring that
 * (e.g. because a function returns both const and non-const variables).
 *
 * Note that this only works in function scope, not for global variables (it'd
 * be nice, but not trivial, to improve that).
 */
#if defined(HAVE__BUILTIN_TYPES_COMPATIBLE_P)
#define unconstify(underlying_type, expr) \
	(StaticAssertExpr(__builtin_types_compatible_p(__typeof(expr), const underlying_type), \
					  "wrong cast"), \
	 (underlying_type) (expr))
#define unvolatize(underlying_type, expr) \
	(StaticAssertExpr(__builtin_types_compatible_p(__typeof(expr), volatile underlying_type), \
					  "wrong cast"), \
	 (underlying_type) (expr))
#else
#define unconstify(underlying_type, expr) \
	((underlying_type) (expr))
#define unvolatize(underlying_type, expr) \
	((underlying_type) (expr))
#endif

/* ----------------------------------------------------------------
 *				Section 9: system-specific hacks
 *
 *		This should be limited to things that absolutely have to be
 *		included in every source file.  The port-specific header file
 *		is usually a better place for this sort of thing.
 * ----------------------------------------------------------------
 */

/*
 *	NOTE:  this is also used for opening text files.
 *	WIN32 treats Control-Z as EOF in files opened in text mode.
 *	Therefore, we open files in binary mode on Win32 so we can read
 *	literal control-Z.  The other affect is that we see CRLF, but
 *	that is OK because we can already handle those cleanly.
 */
#if defined(WIN32) || defined(__CYGWIN__)
#define PG_BINARY	O_BINARY
#define PG_BINARY_A "ab"
#define PG_BINARY_R "rb"
#define PG_BINARY_W "wb"
#else
#define PG_BINARY	0
#define PG_BINARY_A "a"
#define PG_BINARY_R "r"
#define PG_BINARY_W "w"
#endif

/*
 * Provide prototypes for routines not present in a particular machine's
 * standard C library.
 */

#if !HAVE_DECL_FDATASYNC
extern int	fdatasync(int fildes);
#endif

/*
 * Thin wrappers that convert strings to exactly 64-bit integers, matching our
 * definition of int64.  (For the naming, compare that POSIX has
 * strtoimax()/strtoumax() which return intmax_t/uintmax_t.)
 */
#ifdef HAVE_LONG_INT_64
#define strtoi64(str, endptr, base) ((int64) strtol(str, endptr, base))
#define strtou64(str, endptr, base) ((uint64) strtoul(str, endptr, base))
#else
#define strtoi64(str, endptr, base) ((int64) strtoll(str, endptr, base))
#define strtou64(str, endptr, base) ((uint64) strtoull(str, endptr, base))
#endif

/*
 * Similarly, wrappers around labs()/llabs() matching our int64.
 */
#ifdef HAVE_LONG_INT_64
#define i64abs(i) labs(i)
#else
#define i64abs(i) llabs(i)
#endif

/*
 * Use "extern PGDLLIMPORT ..." to declare variables that are defined
 * in the core backend and need to be accessible by loadable modules.
 * No special marking is required on most ports.
 */
#ifndef PGDLLIMPORT
#define PGDLLIMPORT
#endif

/*
 * Use "extern PGDLLEXPORT ..." to declare functions that are defined in
 * loadable modules and need to be callable by the core backend or other
 * loadable modules.
 * If the compiler knows __attribute__((visibility("*"))), we use that,
 * unless we already have a platform-specific definition.  Otherwise,
 * no special marking is required.
 */
#ifndef PGDLLEXPORT
#ifdef HAVE_VISIBILITY_ATTRIBUTE
#define PGDLLEXPORT __attribute__((visibility("default")))
#else
#define PGDLLEXPORT
#endif
#endif

/*
 * The following is used as the arg list for signal handlers.  Any ports
 * that take something other than an int argument should override this in
 * their pg_config_os.h file.  Note that variable names are required
 * because it is used in both the prototypes as well as the definitions.
 * Note also the long name.  We expect that this won't collide with
 * other names causing compiler warnings.
 */

#ifndef SIGNAL_ARGS
#define SIGNAL_ARGS  int postgres_signal_arg
#endif

/*
 * When there is no sigsetjmp, its functionality is provided by plain
 * setjmp.  We now support the case only on Windows.  However, it seems
 * that MinGW-64 has some longstanding issues in its setjmp support,
 * so on that toolchain we cheat and use gcc's builtins.
 */
#ifdef WIN32
#ifdef __MINGW64__
typedef intptr_t sigjmp_buf[5];
#define sigsetjmp(x,y) __builtin_setjmp(x)
#define siglongjmp __builtin_longjmp
#else							/* !__MINGW64__ */
#define sigjmp_buf jmp_buf
#define sigsetjmp(x,y) setjmp(x)
#define siglongjmp longjmp
#endif							/* __MINGW64__ */
#endif							/* WIN32 */

/* /port compatibility functions */
#include "port.h"

#endif							/* C_H */
