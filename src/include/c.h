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
	char		vl_len_[4];		/* 勿直接碰此字段！ */
	char		vl_dat[FLEXIBLE_ARRAY_MEMBER];	/* 数据内容在此 */
};

#define VARHDRSZ		((int32) sizeof(int32))

/*
 * 这些常用数据类型仅为 varlena 头加数据字节。
 * 无终止空之类——数据长度
 * VARSIZE_ANY_EXHDR(ptr)。
 */
typedef struct varlena bytea;
typedef struct varlena text;
typedef struct varlena BpChar;	/* 空白填充字符，SQL char(n) */
typedef struct varlena VarChar; /* 变长字符，即 SQL varchar(n) */

/*
 * 专用数组类型。物理布局与普通数组完全相同
 * （故普通数组下标代码对其
 * 可用）。作为独立类型存在多半因历史：
 * 其 I/O 行为非标准，我们不想改，以免
 * 破坏查看系统目录的应用。另有
 * oidvector 的实现问题：它是 pg_proc 主键的一部分，
 * 不能为此用普通 btree 数组支持例程，
 * 否则成环。
 */
typedef struct
{
	int32		vl_len_;		/* 这些字段须匹配 ArrayType！ */
	int			ndim;			/* 对 int2vector 总为 1 */
	int32		dataoffset;		/* 对 int2vector 总为 0 */
	Oid			elemtype;
	int			dim1;
	int			lbound1;
	int16		values[FLEXIBLE_ARRAY_MEMBER];
} int2vector;

typedef struct
{
	int32		vl_len_;		/* 这些字段须匹配 ArrayType！ */
	int			ndim;			/* 对 oidvector 总为 1 */
	int32		dataoffset;		/* 对 oidvector 总为 0 */
	Oid			elemtype;
	int			dim1;
	int			lbound1;
	Oid			values[FLEXIBLE_ARRAY_MEMBER];
} oidvector;

/*
 * Name 的表示：实质即 C 字符串，但空填充至
 * 恰好 NAMEDATALEN 字节。用结构是历史原因。
 */
typedef struct nameData
{
	char		data[NAMEDATALEN];
} NameData;
typedef NameData *Name;

#define NameStr(name)	((name).data)


/* ----------------------------------------------------------------
 *	第 4 节：	系统类型的 IsValid 宏
 * ----------------------------------------------------------------
 */
/*
 * BoolIsValid
 *	bool 有效则为真。
 */
#define BoolIsValid(boolean)	((boolean) == false || (boolean) == true)

/*
 * PointerIsValid
 *	指针有效则为真。
 */
#define PointerIsValid(pointer) ((const void*)(pointer) != NULL)

/*
 * PointerIsAligned
 *	指针已正确对齐到给定类型则为真。
 */
#define PointerIsAligned(pointer, type) \
		(((uintptr_t)(pointer) % (sizeof (type))) == 0)

#define OffsetToPointer(base, offset) \
		((void *)((char *) base + offset))

#define OidIsValid(objectId)  ((bool) ((objectId) != InvalidOid))

#define RegProcedureIsValid(p)	OidIsValid(p)


/* ----------------------------------------------------------------
 *	第 5 节：	lengthof、对齐
 * ----------------------------------------------------------------
 */
/*
 * lengthof
 *	数组中的元素个数。
 */
#define lengthof(array) (sizeof (array) / sizeof ((array)[0]))

/* ----------------
 * 对齐宏：按给定类型适当对齐长度或地址。
 * fooALIGN() 宏上取整到所需对齐的倍数，
 * fooALIGN_DOWN() 则下取整。后者更适于
 * 如"一页能装多少个 X 大小结构"等问题。
 *
 * 注意：若 ALIGNVAL 非 2 的幂，TYPEALIGN[_DOWN] 不可用。
 * 实际极少需要该情形。
 *
 * 注意：MAXIMUM_ALIGNOF 及因而 MAXALIGN() 有意排除编译器
 * 可能有的大于 8 字节的类型。
 * ----------------
 */

#define TYPEALIGN(ALIGNVAL,LEN)  \
	(((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) & ~((uintptr_t) ((ALIGNVAL) - 1)))

#define SHORTALIGN(LEN)			TYPEALIGN(ALIGNOF_SHORT, (LEN))
#define INTALIGN(LEN)			TYPEALIGN(ALIGNOF_INT, (LEN))
#define LONGALIGN(LEN)			TYPEALIGN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN(LEN)		TYPEALIGN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN(LEN)			TYPEALIGN(MAXIMUM_ALIGNOF, (LEN))
/* MAXALIGN 仅覆盖内建类型，不含缓冲 */
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
 * 上述宏对宽于 uintptr_t 的类型不可用，如 32 位平台上的
 * uint64。对齐指针或长度的常用途无妨，但若需对齐
 * （可能）更宽的东西，用
 * TYPEALIGN64。
 */
#define TYPEALIGN64(ALIGNVAL,LEN)  \
	(((uint64) (LEN) + ((ALIGNVAL) - 1)) & ~((uint64) ((ALIGNVAL) - 1)))

/* 目前不需其他 ALIGN 宏的更宽版本 */
#define MAXALIGN64(LEN)			TYPEALIGN64(MAXIMUM_ALIGNOF, (LEN))


/* ----------------------------------------------------------------
 *	第 6 节：	断言
 * ----------------------------------------------------------------
 */

/*
 * 若定义 USE_ASSERT_CHECKING，则打开全部断言。
 * - plai  9/5/90
 *
 * 发布版或基准副本中 _NOT_ 应定义它
 */

/*
 * Assert() 可用于前后端。前端中它只调用标准
 * assert（若可用）。若未配置使用断言，
 * 则什么也不做。
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
 *	若给定条件为假则产生致命异常。
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
 * 检查 ptr 是否按 bndr 对齐。
 */
#define AssertPointerAlignment(ptr, bndr) \
	Assert(TYPEALIGN(bndr, (uintptr_t)(ptr)) == (uintptr_t)(ptr))

/*
 * 无论是否定义 USE_ASSERT_CHECKING，后端皆编译
 * ExceptionalCondition，以便支持用该 #define 构建的扩展
 * 配未定义它的后端。故只要
 * !FRONTEND 即应声明它。
 */
#ifndef FRONTEND
extern void ExceptionalCondition(const char *conditionName,
								 const char *fileName, int lineNumber) pg_attribute_noreturn();
#endif

/*
 * 支持编译期断言检查的宏。
 *
 * 若"条件"（编译期常量表达式）求值为假，
 * 则以错误消息（字符串字面量）抛出编译错误。
 *
 * C11 有 _Static_assert()，多数 C99 编译器已支持。为
 * 可移植性，我们包成 StaticAssertDecl()。_Static_assert() 是
 * "声明"，故须放在例如变量声明
 * 合法之处。只要用
 * -Wno-declaration-after-statement 编译，亦不可放在函数中
 * 语句之后。宏 StaticAssertStmt() 与 StaticAssertExpr()
 * 分别使其可安全用作语句或表达式。
 *
 * 无 _Static_assert() 的编译器上回退到假定编译器会对
 * 结构位域负宽度抱怨的权宜做法。
 * 不会含有用错误消息，但总比
 * 完全无错误强。
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
 *	第 7 节：	广泛有用的宏
 * ----------------------------------------------------------------
 */
/*
 * Max
 *	返回两数中的较大者。
 */
#define Max(x, y)		((x) > (y) ? (x) : (y))

/*
 * Min
 *	返回两数中的较小者。
 */
#define Min(x, y)		((x) < (y) ? (x) : (y))


/* 取非 long 对齐地址中置位位的掩码 */
#define LONG_ALIGN_MASK (sizeof(long) - 1)

/*
 * MemSet
 *	与标准库 memset() 完全相同，但对清零小的字对齐
 *	结构（如解析树节点）明显更快。
 *	须为宏，因要点是避免函数调用
 *	开销。不过亦发现该循环在某些平台上比本机
 *	memset() 更快，即便有汇编
 *	实现。还需更多研究，或许在 configure 中
 *	做 MEMSET_LOOP_LIMIT 测试。
 */
#define MemSet(start, val, len) \
	do \
	{ \
		/* 须为 void*，因尚不知是否整数对齐 */ \
		void   *_vstart = (void *) (start); \
		int		_val = (val); \
		Size	_len = (len); \
\
		if ((((uintptr_t) _vstart) & LONG_ALIGN_MASK) == 0 && \
			(_len & LONG_ALIGN_MASK) == 0 && \
			_val == 0 && \
			_len <= MEMSET_LOOP_LIMIT && \
			/*
			 *	MEMSET_LOOP_LIMIT==0 时优化器应发现 \
			 *	整个 if 在编译期为假。 \
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
 * MemSetAligned 与 MemSet 相同，但省略检查 "start"
 * 是否字对齐。若调用方先验知道指针已适当
 * 对齐则可用（典型因刚从
 * palloc() 取得，总交付最大对齐指针）。
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
 * MemSetTest/MemSetLoop 是变体，在 "val" 与 "len" 为常量
 * *且* 已知 "start" 指针必字对齐时，可把 MemSet 中全部
 * 测试放在编译期做。
 * 若 MemSetTest 成功，即可用 MemSetLoop，否则用
 * MemSetAligned。用此法时当心参数
 * 多次求值
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
 * 把浮点转为整数前做范围检查的宏。
 * 须小心使边界值在浮点域中精确表达。
 * PG_INTnn_MIN 是 2 的精确幂，会精确表示；
 * 但 PG_INTnn_MAX 不是，可能被舍入，
 * 故避免用之。
 * 输入须事先舍入为整数，通常用 rint()，
 * 否则对接近极限的值可能判断错误。
 * 这些宏对 Inf 正确，对 NaN 则未必，
 * 若可能为 NaN 请先查 isnan(num)。
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
 *	第 8 节：	杂项
 * ----------------------------------------------------------------
 */

/*
 * 反转 qsort 式比较结果的符号，即交换负与
 * 正整数值，并小心勿对 INT_MIN 得错答。
 * 参数应为整型变量。
 */
#define INVERT_COMPARE_RESULT(var) \
	((var) = ((var) < 0) ? 1 : -(var))

/*
 * 声明保存页缓冲的字段或局部变量时用此，勿用
 * "char buf[BLCKSZ]"，若该页可能按页访问。否则
 * 变量可能对齐不足，在挑剔对齐的硬件上出问题。
 * 联合中含 "double" 与 "int64"，以确保编译器知道
 * 值须 MAXALIGN（对照 configure 对
 * MAXIMUM_ALIGNOF 的计算）。
 */
typedef union PGAlignedBlock
{
	char		data[BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGAlignedBlock;

/*
 * 声明保存页缓冲的字段或局部变量时用此，若该页
 * 可能按页访问或传给 SMgr I/O 函数。若用
 * MemoryContext API 分配，应对齐分配函数配合
 * PG_IO_ALIGN_SIZE。此对齐一般可提高 I/O 效率，
 * 在某些平台上使用直接 I/O 时则
 * 严格必需。
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

/* 同，但 XLOG_BLCKSZ 缓冲 */
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
 * 转义字符串的支持宏。若生成非标准符合
 * 字符串则 escape_backslash 应为 true。给字符串加
 * ESCAPE_STRING_SYNTAX 前缀可保证其非标准符合。
 * 当心 "ch" 参数被多次求值！
 */
#define SQL_STR_DOUBLE(ch, escape_backslash)	\
	((ch) == '\'' || ((ch) == '\\' && (escape_backslash)))

#define ESCAPE_STRING_SYNTAX	'E'


#define STATUS_OK				(0)
#define STATUS_ERROR			(-1)
#define STATUS_EOF				(-2)

/*
 * gettext 支持
 */

#ifndef ENABLE_NLS
/* 否则会从 <libintl.h> 得到的内容 */
#define gettext(x) (x)
#define dgettext(d,x) (x)
#define ngettext(s,p,n) ((n) == 1 ? (s) : (p))
#define dngettext(d,s,p,n) ((n) == 1 ? (s) : (p))
#endif

#define _(x) gettext(x)

/*
 *	用此标记字符串常量，使其在稍后而非立即
 *	需要翻译。适用于需要同时访问原文与译文，
 *	以及无法立即翻译的情形，如初始化
 *	无法立即翻译时，如初始化全局
 *	变量。
 *
 *	https://www.gnu.org/software/gettext/manual/html_node/Special-cases.html
 */
#define gettext_noop(x) (x)

/*
 * 为更好支持 PostgreSQL 主版本并行安装以及
 * 主库 soname 版本并行安装，我们通过追加那些
 * 版本号来改写 gettext 域名。编码规则应是：凡
 * 以字面量提及域名处，须包进
 * PG_TEXTDOMAIN()。下列宏对非字面量无效；但这
 * 多少有意为之，以免在值传来传去时操心
 * 多少有意，以免值传来传去时操心
 * 预/后改写的多种状态。
 *
 *
 * 须与 nls-global.mk 中的安装规则一致。
 */
#ifdef SO_MAJOR_VERSION
#define PG_TEXTDOMAIN(domain) (domain CppAsString2(SO_MAJOR_VERSION) "-" PG_MAJORVERSION)
#else
#define PG_TEXTDOMAIN(domain) (domain "-" PG_MAJORVERSION)
#endif

/*
 * 可从表达式去掉 const/volatile 的宏，但不允许
 * 改变底层类型。后者目前仅对
 * 类 gcc 编译器强制。
 *
 * 请注意：若结果会被修改，去掉 const 不安全
 * （属未定义行为）。仍这样做可导致编译器
 * 误优化或运行时崩溃（改只读内存）。
 * 仅当结果不会被修改，但 API 设计或语言限制
 * 使你无法如此声明时才安全
 * （例如函数既返回 const 又返回非 const 变量）。
 *
 * 注意这仅在函数作用域有效，不适用于全局变量（若能
 * 改进会很好，但不平凡）。
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
 *	第 9 节：系统相关技巧
 *
 *	应限于绝对必须出现在每个源文件中的
 *	内容。此类东西通常更适合放在
 *	移植相关头文件中。
 * ----------------------------------------------------------------
 */

/*
 *	注意：打开文本文件亦用此。
 *	WIN32 文本模式视 Control-Z 为 EOF。
 *	故在 Win32 上以二进制模式打开文件，以便能读
 *	字面 Control-Z。另一影响是会看到 CRLF，但
 *	无妨，因我们已能干净处理。
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
 * 为机器标准库所无例程提供原型。
 *
 */

#if !HAVE_DECL_FDATASYNC
extern int	fdatasync(int fildes);
#endif

/*
 * 把字符串转为恰好 64 位整数的薄包装，匹配我们对
 * int64 的定义。（命名对照 POSIX 的
 * strtoimax()/strtoumax()→intmax_t/uintmax_t。）
 */
#ifdef HAVE_LONG_INT_64
#define strtoi64(str, endptr, base) ((int64) strtol(str, endptr, base))
#define strtou64(str, endptr, base) ((uint64) strtoul(str, endptr, base))
#else
#define strtoi64(str, endptr, base) ((int64) strtoll(str, endptr, base))
#define strtou64(str, endptr, base) ((uint64) strtoull(str, endptr, base))
#endif

/*
 * 同理，匹配我们 int64 的 labs()/llabs() 包装。
 */
#ifdef HAVE_LONG_INT_64
#define i64abs(i) labs(i)
#else
#define i64abs(i) llabs(i)
#endif

/*
 * 用 extern PGDLLIMPORT 声明核心后端定义且需
 * 可被可加载模块访问的变量。
 * 多数移植无需特殊标记。
 */
#ifndef PGDLLIMPORT
#define PGDLLIMPORT
#endif

/*
 * 用 extern PGDLLEXPORT 声明模块中定义且需
 * 可被核心后端或其他可加载模块调用的
 * 函数。
 * 若编译器认识 __attribute__((visibility("*")))，则用之，
 * 除非已有平台专用定义。否则
 * 无需特殊标记。
 */
#ifndef PGDLLEXPORT
#ifdef HAVE_VISIBILITY_ATTRIBUTE
#define PGDLLEXPORT __attribute__((visibility("default")))
#else
#define PGDLLEXPORT
#endif
#endif

/*
 * 下列用作信号处理程序的参数表。若某移植
 * 接受非 int 参数，应在其 pg_config_os.h 中
 * 覆盖。注意需要变量名，因其同时用于
 * 原型与定义。另注意长名。我们期望它不会与
 * 其他名字冲突而引起编译器
 * 警告。
 */

#ifndef SIGNAL_ARGS
#define SIGNAL_ARGS  int postgres_signal_arg
#endif

/*
 * 无 sigsetjmp 时，其功能由普通 setjmp 提供。
 * 现仅在 Windows 上支持该情形。不过 MinGW-64 的
 * setjmp 支持似有长期问题，故在该工具链上
 * 取巧改用 gcc 内建。
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

/* /port 兼容函数 */
#include "port.h"

#endif							/* C_H */
