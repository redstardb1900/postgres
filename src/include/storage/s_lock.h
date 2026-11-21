/*-------------------------------------------------------------------------
 *
 * s_lock.h
 *	   自旋锁的硬件相关实现。
 *
 *	注意：本文件中的宏均不打算被直接调用。
 *	请通过 spin.h 中的硬件无关宏调用它们。
 *
 *	须为每个受支持平台提供下列硬件相关宏。
 *	。
 *
 *	void S_INIT_LOCK(slock_t *lock)
 *	初始化自旋锁（至未锁定状态）。
 *
 *	int S_LOCK(slock_t *lock)
 *	获取自旋锁，必要时等待。
 *	若无法获取锁则超时并中止，时限为
 *	"合理"时长 —— 通常约 1 分钟。
 *	应返回"延迟"次数；见 s_lock.c
 *
 *	void S_UNLOCK(slock_t *lock)
 *	解锁先前已获取的锁。
 *
 *	bool S_LOCK_FREE(slock_t *lock)
 *	测试锁是否空闲。空闲则 true，已锁则 false。
 *	这*不*改变锁的状态。
 *
 *	void SPIN_DELAY(void)
 *	在自旋锁等待循环内执行的延迟操作。
 *
 *	致实现者：文件末尾有这些宏的默认实现。
 *	请检查平台能否用它们，或须覆盖。
 *	。
 *
 * 通常 S_LOCK() 用更低层宏 TAS() 与
 *	TAS_SPIN() 实现：
 *
 *	int TAS(slock_t *lock)
 *	原子测试并设置指令。尝试获取锁，
 *	但*不*等待。成功返回 0，无法获取则
 *	返回非零。
 *
 *	int TAS_SPIN(slock_t *lock)
 *	类似 TAS()，但用于等待先前已发现
 *	争用的锁。默认与 TAS() 相同，但在
 *	某些体系结构上，用非锁定指令轮询
 *	争用锁、仅在看起来空闲时再重试
 *	原子测试并设置更佳。
 *
 *	TAS()/TAS_SPIN() 非 API 一部分，切勿直接调用。
 *	。
 *
 *	CAUTION：某些平台上 TAS() 和/或 TAS_SPIN() 有时会
 *	在锁未锁定时仍报告获取失败。例如，
 *	在 Alpha 上若被中断 TAS() 会"失败"。故必须
 *	总用重试循环，即使你确信锁空闲。
 *
 *	这些宏有责任确保编译器不会把对共享内存
 *	的访问重排到实际获取锁之前，或跟在
 *	释放锁之后。PostgreSQL 9.5 之前，这是
 *	调用者的责任，意味着调用者须用
 *	volatile 限定指针引用自旋锁本身及自旋锁
 *	临界区内访问的共享数据。这在记法上
 *	别扭、易忘（因而易错），且妨碍
 *	某些有用的编译器优化。因此我们现在需要
 *	宏自身阻止编译器重排，使调用者无需
 *	采取特殊预防措施。
 *
 *	在弱内存序平台上，TAS()、TAS_SPIN() 与
 *	S_UNLOCK() 还必须含硬件级内存屏障指令，
 *	以防硬件层出现类似重排。
 *	TAS() 与 TAS_SPIN() 必须保证宏之后发出的
 *	装入与存储在获得锁之前不执行。反之，
 *	S_UNLOCK() 必须保证宏之前发出的装入与
 *	存储在释放锁之前已执行。
 *
 *	多数受支持平台上，TAS() 用汇编写成的
 *	tas() 函数执行硬件原子测试并设置
 *	指令。也可用等价的 OS 互斥例程。
 *
 *	若无系统专用 TAS()（即未定义
 *	HAVE_SPINLOCKS），则回退到用 SysV 信号量
 *	的仿真（见 spin.c）。比正确 TAS() 慢得多，
 *	因每次加锁/解锁都有内核调用开销。
 *	旧报告称使用 SysV 信号量代码时 Postgres
 *	约 40% 时间花在 semop(2)。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  src/include/storage/s_lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef S_LOCK_H
#define S_LOCK_H

#ifdef FRONTEND
#error "s_lock.h may not be included from frontend code"
#endif

#ifdef HAVE_SPINLOCKS	/* 若请求则跳过自旋锁 */

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
/*************************************************************************
 * 全部 gcc 内联
 * Gcc 统一把 CPU 定义为 __cpu__。
 * 其他编译器用 __cpu 或 __cpu__，那些情形两者都测。
 */

/*----------
 * Standard gcc asm format (assuming "volatile slock_t *lock"):

	__asm__ __volatile__(
		"	instruction	\n"
		"	instruction	\n"
		"	instruction	\n"
:		"=r"(_res), "+m"(*lock)		// return register, in/out lock value
:		"r"(lock)					// lock pointer, in input register
:		"memory", "cc");			// show clobbered registers here

 * The output-operands list (after first colon) should always include
 * "+m"(*lock), whether or not the asm code actually refers to this
 * operand directly.  This ensures that gcc believes the value in the
 * lock variable is used and set by the asm code.  Also, the clobbers
 * list (after third colon) should always include "memory"; this prevents
 * gcc from thinking it can cache the values of shared-memory fields
 * across the asm code.  Add "cc" if your asm code changes the condition
 * code register, and also list any temp registers the code uses.
 *----------
 */


#ifdef __i386__		/* 32-bit i386 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res = 1;

	/*
	 * 在断言总线锁之前先做非锁定测试。注意该
	 * 额外测试在某些 x86 平台上略亏、在另一些上
	 * 略盈；远不清楚我们是否应保留它。
	 *
	 * 上次测试时我们还没有分开的 TAS() 与
	 * TAS_SPIN() 宏。如今多半更好在 TAS_SPIN()
	 * 中做非锁定测试而 TAS() 中不做，如 x86_64，
	 * 但无人做测试验证。无实证前，最好
	 * 保持不动。
	 */
	__asm__ __volatile__(
		"	cmpb	$0,%1	\n"
		"	jne		1f		\n"
		"	lock			\n"
		"	xchgb	%0,%1	\n"
		"1: \n"
:		"+q"(_res), "+m"(*lock)
:		/* no inputs */
:		"memory", "cc");
	return (int) _res;
}

#define SPIN_DELAY() spin_delay()

static __inline__ void
spin_delay(void)
{
	/*
	 * 此序列等价于 PAUSE 指令（若后续指令不是
	 * 串操作，旧 IA32 处理器会忽略 "rep"）；
	 * IA-32 体系结构软件开发者手册第 3 卷
	 * 第 7.7.2 节说明为何在自旋锁内循环中使用
	 * PAUSE 对良好性能是必要的：
	 * 。
	 *
	 *     PAUSE 指令改善支持超线程技术的 IA-32
	 *     处理器在执行自旋等待循环及其他例程
	 *     （其中一线程在紧轮询循环中访问共享锁
	 *     或信号量）时的性能。执行自旋等待循环
	 *     时，处理器退出循环时可能遭受严重
	 *     性能惩罚，因其检测到可能的内存序违例
	 *     并冲刷核心处理器的流水线。PAUSE 指令
	 *     向处理器提示该序列是自旋等待循环。
	 *     处理器用此提示避免内存序违例并防止
	 *     流水线冲刷。此外，PAUSE 指令还对自旋
	 *     等待循环去流水线化，以防其过度消耗
	 *     执行资源。
	 *     。
	 *     。
	 */
	__asm__ __volatile__(
		" rep; nop			\n");
}

#endif	 /* __i386__ */


#ifdef __x86_64__		/* AMD Opteron, Intel EM64T */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

/*
 * 在 Intel EM64T 上，xchg 之前先做非锁定测试有利，
 * 但仅当自旋时。
 *
 * 另见 Chynoweth 与 Lee 关于多核 Intel EM64T
 * 与 IA32 可扩展原子锁的文章。撰写时
 * 见于：
 * http://software.intel.com/en-us/articles/implementing-scalable-atomic-locks-for-multi-core-intel-em64t-and-ia32-architectures
 */
#define TAS_SPIN(lock)    (*(lock) ? 1 : TAS(lock))

static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res = 1;

	__asm__ __volatile__(
		"	lock			\n"
		"	xchgb	%0,%1	\n"
:		"+q"(_res), "+m"(*lock)
:		/* no inputs */
:		"memory", "cc");
	return (int) _res;
}

#define SPIN_DELAY() spin_delay()

static __inline__ void
spin_delay(void)
{
	/*
	 * 在自旋延迟循环中加 PAUSE 在 Opteron 上可证为
	 * 空操作，但在 EM64T 上或有用，故保留。
	 */
	__asm__ __volatile__(
		" rep; nop			\n");
}

#endif	 /* __x86_64__ */


/*
 * ARM/ARM64 上若可用则用 __sync_lock_test_and_set(int *, int)。
 *
 * 我们用内建的 int 宽变体，因其在更多芯片上
 * 可用。
 */
#if defined(__arm__) || defined(__arm) || defined(__aarch64__)
#ifdef HAVE_GCC__SYNC_INT32_TAS
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef int slock_t;

static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

/*
 * 在自旋锁循环中用 ISB 指令延迟，在高核心数
 * ARM64 处理器上看来有益。对较小设备多半持平，
 * 且 ISB 在 v7 前 ARM 上不存在。
 */
#if defined(__aarch64__)

#define SPIN_DELAY() spin_delay()

static __inline__ void
spin_delay(void)
{
	__asm__ __volatile__(
		" isb;				\n");
}

#endif	 /* __aarch64__ */
#endif	 /* HAVE_GCC__SYNC_INT32_TAS */
#endif	 /* __arm__ || __arm || __aarch64__ */


/* S/390 与 S/390x Linux（32/64 位 zSeries） */
#if defined(__s390__) || defined(__s390x__)
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock)	   tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	int			_res = 0;

	__asm__	__volatile__(
		"	cs 	%0,%3,0(%2)		\n"
:		"+d"(_res), "+m"(*lock)
:		"a"(lock), "d"(1)
:		"memory", "cc");
	return _res;
}

#endif	 /* __s390__ || __s390x__ */


#if defined(__sparc__)		/* Sparc */
/*
 * Solaris 总在 TSO（总存储）模式跑 sparc 处理器，但
 * linux 过去不是，*BSDs 至今仍不是。故获取/释放
 * 语义须小心。CPU 会把多余成员当 NOPs，
 * 故只占代码空间。
 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res;

	/*
	 *	为何用 "ldstub" 见 src/backend/port/tas/sunstudio_sparc.s
	 *	注释；该文件用 "cas"。gcc 当前生成
	 *	sparcv7 目标二进制，故无法用 "cas"。
	 */
	__asm__ __volatile__(
		"	ldstub	[%2], %0	\n"
:		"=r"(_res), "+m"(*lock)
:		"r"(lock)
:		"memory");
#if defined(__sparcv7) || defined(__sparc_v7__)
	/*
	 * 无 stbar 或 membar 可用，幸好实际硬件不需要
	 * 屏障。
	 */
#elif defined(__sparcv8) || defined(__sparc_v8__)
	/* stbar 可用（PSO、RMO 都需要），membar 不可用 */
	__asm__ __volatile__ ("stbar	 \n":::"memory");
#else
	/*
	 * #LoadStore (RMO) | #LoadLoad (RMO) 合为 sparcv8+
	 * 向上的合适获取屏障
	 */
	__asm__ __volatile__ ("membar #LoadStore | #LoadLoad \n":::"memory");
#endif
	return (int) _res;
}

#if defined(__sparcv7) || defined(__sparc_v7__)
/*
 * 无 stbar 或 membar 可用，幸好实际硬件不需要
 * 屏障。此时落到 gcc 默认的 S_UNLOCK
 * 定义。
 */
#elif defined(__sparcv8) || defined(__sparc_v8__)
/* stbar 可用（PSO、RMO 都需要），membar 不可用 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("stbar	 \n":::"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)
#else
/*
 * #LoadStore (RMO) | #StoreStore (RMO, PSO) 合为
 * sparcv8+ 向上合适释放屏障。
 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("membar #LoadStore | #StoreStore \n":::"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)
#endif

#endif	 /* __sparc__ */


/* PowerPC */
#if defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

/* PPC 上在 lwarx 前做非锁定测试有利 */
#define TAS_SPIN(lock)	(*(lock) ? 1 : TAS(lock))

/*
 * addi 第二操作数可持常量零或寄存器号，
 * 故用约束 "=&b" 以避免分配 r0。"b" 表示
 * "地址基址寄存器"；多数有此寄存器或零属性的
 * 地址基址，如 lwax 第二操作数。
 *
 * 注意：据 Enhanced PowerPC Architecture 手册
 * （v1.0，2002-05-07），lwarx/stwcx 循环后 isync
 * 已够。但自旋锁在普通内存时可用 lwsync
 * 以求更佳性能
 *
 * 通常这里的分支会用 GNU 风格局部符号，即
 * "1f" 引用 "1:" 等。但有人在 AIX 上用 gcc 且以
 * IBM 汇编器为后端，而 IBM 汇编器不做局部符号。
 * 故手写分支偏移；幸好全部 PPC 指令都恰为
 * 4 字节，不难数。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t _t;
	int _res;

	__asm__ __volatile__(
"	lwarx   %0,0,%3,1	\n"
"	cmpwi   %0,0		\n"
"	bne     $+16		\n"		/* branch to li %1,1 */
"	addi    %0,%0,1		\n"
"	stwcx.  %0,0,%3		\n"
"	beq     $+12		\n"		/* branch to lwsync */
"	li      %1,1		\n"
"	b       $+12		\n"		/* branch to end of asm sequence */
"	lwsync				\n"
"	li      %1,0		\n"

:	"=&b"(_t), "=r"(_res), "+m"(*lock)
:	"r"(lock)
:	"memory", "cc");
	return _res;
}

/*
 * PowerPC 的 S_UNLOCK 几乎标准，但需要 "sync"
 * 指令。可用 lwsync 以更佳性能。
 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("	lwsync \n" ::: "memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* powerpc */


#if defined(__mips__) && !defined(__sgi)	/* non-SGI MIPS */
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

/*
 * 原始 MIPS-I 处理器缺少 LL/SC 指令，但若不幸
 * 跑在其上，我们期望内核对非法指令陷阱仿真。
 * 在任何更新的机器上（MIPS-I 实已灭绝）LL/SC
 * 是唯一合理选择，因任何其他同步方法都须
 * 内核调用。不幸许多工具链仍默认以 MIPS-I 为
 * 代码生成目标；若 __mips 显示如此，我们必须
 * 代码生成目标；若 __mips 显示如此，我们须
 * 强制汇编器接受 LL/SC。
 *
 * R10000 及以上需单独 SYNC，与 LL/SC
 * 同类问题。
 */
#if __mips < 2
#define MIPS_SET_MIPS2	"       .set mips2          \n"
#else
#define MIPS_SET_MIPS2
#endif

static __inline__ int
tas(volatile slock_t *lock)
{
	volatile slock_t *_l = lock;
	int			_res;
	int			_tmp;

	__asm__ __volatile__(
		"       .set push           \n"
		MIPS_SET_MIPS2
		"       .set noreorder      \n"
		"       .set nomacro        \n"
		"       ll      %0, %2      \n"
		"       or      %1, %0, 1   \n"
		"       sc      %1, %2      \n"
		"       xori    %1, 1       \n"
		"       or      %0, %0, %1  \n"
		"       sync                \n"
		"       .set pop              "
:		"=&r" (_res), "=&r" (_tmp), "+R" (*_l)
:		/* no inputs */
:		"memory");
	return _res;
}

/* MIPS 的 S_UNLOCK 几乎标准，但需要 "sync" 指令 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__( \
		"       .set push           \n" \
		MIPS_SET_MIPS2 \
		"       .set noreorder      \n" \
		"       .set nomacro        \n" \
		"       sync                \n" \
		"       .set pop              " \
:		/* no outputs */ \
:		/* no inputs */	\
:		"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* __mips__ && !__sgi */


#if defined(__hppa) || defined(__hppa__)	/* HP PA-RISC */
/*
 * HP PA-RISC
 *
 * 因 LDCWX 需要 16 字节对齐地址，我们把 slock_t
 * 声明为 16 字节结构。结构中活跃字为对齐的那
 * 一个；另三字只坐在 -1。
 */
#define HAS_TEST_AND_SET

typedef struct
{
	int			sema[4];
} slock_t;

#define TAS_ACTIVE_WORD(lock)	((volatile int *) (((uintptr_t) (lock) + 15) & ~15))

static __inline__ int
tas(volatile slock_t *lock)
{
	volatile int *lockword = TAS_ACTIVE_WORD(lock);
	int			lockval;

	/*
	 * LDCWX 指令原子地清目标字并返回先前值。
	 * 因此若指令返回 0，则在我们测试之前已有人
	 * 取得锁（即我们失败）。
	 * 。
	 *
	 * 注意这意味着我们实际清字以加锁、置字以
	 * 解锁。这与 SPARC LDSTUB 指令行为相反。
	 * 不知为何 H-P 做的每件事都相当巴洛克……
	 * 。
	 *
	 * LDCWX 细节见《精确体系结构与指令
	 * 参考手册》（1987 年 6 月 09740-90014）
	 * 第 5-38 页。
	 */
	__asm__ __volatile__(
		"	ldcwx	0(0,%2),%0	\n"
:		"=r"(lockval), "+m"(*lockword)
:		"r"(lockword)
:		"memory");
	return (lockval == 0);
}

#define S_UNLOCK(lock)	\
	do { \
		__asm__ __volatile__("" : : : "memory"); \
		*TAS_ACTIVE_WORD(lock) = -1; \
	} while (0)

#define S_INIT_LOCK(lock) \
	do { \
		volatile slock_t *lock_ = (lock); \
		lock_->sema[0] = -1; \
		lock_->sema[1] = -1; \
		lock_->sema[2] = -1; \
		lock_->sema[3] = -1; \
	} while (0)

#define S_LOCK_FREE(lock)	(*TAS_ACTIVE_WORD(lock) != 0)

#endif	 /* __hppa || __hppa__ */


/*
 * 若无平台专用知识，但发现编译器提供
 * __sync_lock_test_and_set()，则用之。若两者都有，
 * 宁用 int 宽而非 char 宽版本，理由颇牵强：
 * 已知在 ARM 生态中更可能管用。（ARM 已在
 * 上文处理。）
 */
#if !defined(HAS_TEST_AND_SET)

#if defined(HAVE_GCC__SYNC_INT32_TAS)
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef int slock_t;

static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

#elif defined(HAVE_GCC__SYNC_CHAR_TAS)
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef char slock_t;

static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

#endif	 /* HAVE_GCC__SYNC_INT32_TAS */

#endif	/* !defined(HAS_TEST_AND_SET) */


/*
 * gcc/icc 的默认 S_UNLOCK() 实现。
 *
 * 注意：对任何能把内存访问（装入或存储）重排到
 * 随后存储之后的平台，此实现不安全。x86 与多数
 * 遗留体系结构碰巧不会（有的是单处理器！），但
 * 许多现代系统有更弱内存序。那些必须自定义
 * S_UNLOCK()，勿依赖本实现。
 * 。
 */
#if !defined(S_UNLOCK)
#define S_UNLOCK(lock)	\
	do { __asm__ __volatile__("" : : : "memory");  *(lock) = 0; } while (0)
#endif

#endif	/* defined(__GNUC__) || defined(__INTEL_COMPILER) */


/*
 * ---------------------------------------------------------------------
 * 使用非 gcc 内联汇编的平台：
 * ---------------------------------------------------------------------
 */

#if !defined(HAS_TEST_AND_SET)	/* 上面未触发，试这里 */

#if defined(_AIX)	/* AIX */
/*
 * AIX (POWER)
 */
#define HAS_TEST_AND_SET

#include <sys/atomic_op.h>

typedef int slock_t;

#define TAS(lock)			_check_lock((slock_t *) (lock), 0, 1)
#define S_UNLOCK(lock)		_clear_lock((slock_t *) (lock), 0)
#endif	 /* _AIX */


/* 在 sunstudio_(sparc|x86).s 中 */

#if defined(__SUNPRO_C) && (defined(__i386) || defined(__x86_64__) || defined(__sparc__) || defined(__sparc))
#define HAS_TEST_AND_SET

#if defined(__i386) || defined(__x86_64__) || defined(__sparcv9) || defined(__sparcv8plus)
typedef unsigned int slock_t;
#else
typedef unsigned char slock_t;
#endif

extern slock_t pg_atomic_cas(volatile slock_t *lock, slock_t with,
									  slock_t cmp);

#define TAS(a) (pg_atomic_cas((a), 1, 0) != 0)
#endif


#ifdef _MSC_VER
typedef LONG slock_t;

#define HAS_TEST_AND_SET
#define TAS(lock) (InterlockedCompareExchange(lock, 1, 0))

#define SPIN_DELAY() spin_delay()

/* 若在 Win64 上用 Visual C++，内联汇编不可用。
 * 用 _mm_pause 内建替代 rep nop。
 */
#if defined(_WIN64)
static __forceinline void
spin_delay(void)
{
	_mm_pause();
}
#else
static __forceinline void
spin_delay(void)
{
	/* 见 gcc 注释。相同代码，MASM 语法 */
	__asm rep nop;
}
#endif

#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)

#define S_UNLOCK(lock)	\
	do { _ReadWriteBarrier(); (*(lock)) = 0; } while (0)

#endif


#endif	/* !defined(HAS_TEST_AND_SET) */


/* 若无任何自旋锁实现方式则炸开 */
#ifndef HAS_TEST_AND_SET
#error PostgreSQL does not have native spinlock support on this platform.  To continue the compilation, rerun configure using --disable-spinlocks.  However, performance will be poor.  Please report this to pgsql-bugs@lists.postgresql.org.
#endif


#else	/* !HAVE_SPINLOCKS */


/*
 * 用信号量伪装的自旋锁实现 —— 又慢又易
 * 触碰内核信号量数量限制，非不得已勿用！
 * 子程序在 spin.c。
 */
typedef int slock_t;

extern bool s_lock_free_sema(volatile slock_t *lock);
extern void s_unlock_sema(volatile slock_t *lock);
extern void s_init_lock_sema(volatile slock_t *lock, bool nested);
extern int	tas_sema(volatile slock_t *lock);

#define S_LOCK_FREE(lock)	s_lock_free_sema(lock)
#define S_UNLOCK(lock)	 s_unlock_sema(lock)
#define S_INIT_LOCK(lock)	s_init_lock_sema(lock, false)
#define TAS(lock)	tas_sema(lock)


#endif	/* HAVE_SPINLOCKS */


/*
 * 默认定义 —— 按需在上方覆盖。
 */

#if !defined(S_LOCK)
#define S_LOCK(lock) \
	(TAS(lock) ? s_lock((lock), __FILE__, __LINE__, __func__) : 0)
#endif	 /* S_LOCK */

#if !defined(S_LOCK_FREE)
#define S_LOCK_FREE(lock)	(*(lock) == 0)
#endif	 /* S_LOCK_FREE */

#if !defined(S_UNLOCK)
/*
 * 默认 S_UNLOCK 本质是 *(lock) = 0。若平台能把
 * 内存访问（装入或存储）重排到随后存储之后，
 * 这不安全；可能如此的平台必须自定义
 * S_UNLOCK。但 CPU 重排并非唯一顾虑：若把
 * S_UNLOCK() 简单定义为内联宏，编译器可能把
 * 临界区内指令重排到锁释放之后。因编译器
 * 多半不知外部函数 s_unlock 在做什么，把相同
 * 逻辑放那里应足够。
 * 足够聪明的全局优化编译器或可打破该假定，
 * 且每次自旋锁释放的函数调用可能显著损害
 * 性能，故仅在不知合适内建的平台用本实现。
 * 那些多半是较冷门的平台/编译器组合，
 * PostgreSQL 项目无法接触。
 * 。
 */
#define USE_DEFAULT_S_UNLOCK
extern void s_unlock(volatile slock_t *lock);
#define S_UNLOCK(lock)		s_unlock(lock)
#endif	 /* S_UNLOCK */

#if !defined(S_INIT_LOCK)
#define S_INIT_LOCK(lock)	S_UNLOCK(lock)
#endif	 /* S_INIT_LOCK */

#if !defined(SPIN_DELAY)
#define SPIN_DELAY()	((void) 0)
#endif	 /* SPIN_DELAY */

#if !defined(TAS)
extern int	tas(volatile slock_t *lock);		/* in port/.../tas.s, or
												 * s_lock.c */

#define TAS(lock)		tas(lock)
#endif	 /* TAS */

#if !defined(TAS_SPIN)
#define TAS_SPIN(lock)	TAS(lock)
#endif	 /* TAS_SPIN */

extern PGDLLIMPORT slock_t dummy_spinlock;

/*
 * 平台无关的离线支持例程
 */
extern int s_lock(volatile slock_t *lock, const char *file, int line, const char *func);

/* 支持动态调整 spins_per_delay */
#define DEFAULT_SPINS_PER_DELAY  100

extern void set_spins_per_delay(int shared_spins_per_delay);
extern int	update_spins_per_delay(int shared_spins_per_delay);

/*
 * 支持自旋延迟，在各类类似自旋锁过程发生
 * 处有用。
 */
typedef struct
{
	int			spins;
	int			delays;
	int			cur_delay;
	const char *file;
	int			line;
	const char *func;
} SpinDelayStatus;

static inline void
init_spin_delay(SpinDelayStatus *status,
				const char *file, int line, const char *func)
{
	status->spins = 0;
	status->delays = 0;
	status->cur_delay = 0;
	status->file = file;
	status->line = line;
	status->func = func;
}

#define init_local_spin_delay(status) init_spin_delay(status, __FILE__, __LINE__, __func__)
extern void perform_spin_delay(SpinDelayStatus *status);
extern void finish_spin_delay(SpinDelayStatus *status);

#endif	 /* S_LOCK_H */
