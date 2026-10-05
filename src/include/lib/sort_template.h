/*-------------------------------------------------------------------------
 *
 * sort_template.h
 *
 *	  一种支持不同程度
 *	  专用化的排序算法模板。
 *
 * Copyright (c) 2021-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1992-1994, Regents of the University of California
 *
 * 用法说明：
 *
 *	  要生成针对某种类型专用化的函数，应在包含本文件之前
 *	  #define 下列参数宏。
 *
 *	  - ST_SORT：要生成的排序函数名
 *	  - ST_ELEMENT_TYPE：所引用元素的类型
 *	  - ST_DECLARE：若定义，则声明这些函数和类型
 *	  - ST_DEFINE：若定义，则定义这些函数和类型
 *	  - ST_SCOPE：函数的作用域（例如 extern、static inline）
 *	  - ST_CHECK_FOR_INTERRUPTS：若定义，则排序可被中断
 *
 *	  也可以不定义 ST_ELEMENT_TYPE，而定义 ST_ELEMENT_TYPE_VOID。那样
 *	  生成的函数会自动多一个 "element_size"
 *	  参数。这样我们就能生成传统的 qsort 函数。
 *
 *	  必须定义下列宏之一，用来说明如何比较
 *	  元素。前两个选项是任意表达式，取决于
 *	  是否需要额外的直通参数；若排序函数应在
 *	  运行时接收一个
 *	  函数指针，则应定义第三个选项。
 *
 *	  - ST_COMPARE(a, b)：简单的比较表达式
 *	  - ST_COMPARE(a, b, arg)：多带一个参数的变体
 *	  - ST_COMPARE_RUNTIME_POINTER：排序函数接收一个函数指针
 *
 *	  若要让比较器以及排序函数
 *	  接收一个额外的直通参数，请指定该
 *	  参数的类型。
 *
 *	  - ST_COMPARE_ARG_TYPE：额外参数的类型
 *
 *	  生成的排序函数原型是：
 *
 *	  void ST_SORT(ST_ELEMENT_TYPE *data, size_t n,
 *				   [size_t element_size,]
 *				   [ST_SORT_compare_function compare,]
 *				   [ST_COMPARE_ARG_TYPE *arg]);
 *
 *	  ST_SORT_compare_function 是下列类型的函数指针：
 *
 *	  int (*)(const ST_ELEMENT_TYPE *a, const ST_ELEMENT_TYPE *b,
 *			  [ST_COMPARE_ARG_TYPE *arg])
 *
 * 沿革
 *
 *	  相对原版 NetBSD 源代码的修改：
 *	  - 增加 do ... while() 宏修正
 *	  - 去掉 __inline、_DIAGASSERTs、__P
 *	  - 去掉考虑不周的 "swap_cnt" 插入排序切换，改为
 *		对已排好序的输入做简单检查。
 *	  - 对较小的划分递归，以限制栈用量
 *	  - 改成能够生成专用函数的头文件
 *
 * IDENTIFICATION
 *		src/include/lib/sort_template.h
 *
 *-------------------------------------------------------------------------
 */

/*	  $NetBSD: qsort.c,v 1.13 2003/08/07 16:43:42 agc Exp $   */

/*-
 * Copyright (c) 1992, 1993
 *	  The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * 快速排序例程，基于 J. L. Bentley 与 M. D. McIlroy 的
 * "Engineering a sort function",
 * Software--Practice and Experience 23 (1993) 1249-1265。
 *
 * 我们在其原文上加了对输入已经有序的检查。根据大约
 * 2006-03-21 pgsql-hackers 上的讨论，这看来是合算的。
 *
 * 另外，我们对较小的分区递归，对较大的分区迭代，
 * 从而保证递归层数无法超过 log(N)（因为递归进去的
 * 分区肯定不超过输入的一半）。Bentley 与 McIlroy 明确
 * 拒绝这样做，理由是 "不值得花这份力气"，但我们在
 * 现场见过因栈溢出而崩溃，所以那个判断看来是错的。
 *
 *
 */

#define ST_MAKE_PREFIX(a) CppConcat(a,_)
#define ST_MAKE_NAME(a,b) ST_MAKE_NAME_(ST_MAKE_PREFIX(a),b)
#define ST_MAKE_NAME_(a,b) CppConcat(a,b)

/*
 * 若元素类型是 void，还需要 element_size 参数，
 * 因为我们不知道元素大小。
 */
#ifdef ST_ELEMENT_TYPE_VOID
#define ST_ELEMENT_TYPE void
#define ST_SORT_PROTO_ELEMENT_SIZE , size_t element_size
#define ST_SORT_INVOKE_ELEMENT_SIZE , element_size
#else
#define ST_SORT_PROTO_ELEMENT_SIZE
#define ST_SORT_INVOKE_ELEMENT_SIZE
#endif

/*
 * 若使用者希望在运行时传入比较函数，就需要把它
 * 做成 sort 与 med3 函数的参数。
 */
#ifdef ST_COMPARE_RUNTIME_POINTER
/*
 * ST_SORT 将接受的比较函数指针类型，除非你已经
 * 手工声明了类型名，并希望使用那个类型，而不是
 * 再定义一个新的。
 */
#ifndef ST_COMPARATOR_TYPE_NAME
#define ST_COMPARATOR_TYPE_NAME ST_MAKE_NAME(ST_SORT, compare_function)
#endif
#define ST_COMPARE compare
#ifndef ST_COMPARE_ARG_TYPE
#define ST_SORT_PROTO_COMPARE , ST_COMPARATOR_TYPE_NAME compare
#define ST_SORT_INVOKE_COMPARE , compare
#else
#define ST_SORT_PROTO_COMPARE , ST_COMPARATOR_TYPE_NAME compare
#define ST_SORT_INVOKE_COMPARE , compare
#endif
#else
#define ST_SORT_PROTO_COMPARE
#define ST_SORT_INVOKE_COMPARE
#endif

/*
 * 若使用者希望比较函数或表达式带一个额外参数，
 * 就需要把它做成 sort、compare 与 med3 函数的
 * 参数。
 */
#ifdef ST_COMPARE_ARG_TYPE
#define ST_SORT_PROTO_ARG , ST_COMPARE_ARG_TYPE *arg
#define ST_SORT_INVOKE_ARG , arg
#else
#define ST_SORT_PROTO_ARG
#define ST_SORT_INVOKE_ARG
#endif

#ifdef ST_DECLARE

#ifdef ST_COMPARE_RUNTIME_POINTER
typedef int (*ST_COMPARATOR_TYPE_NAME) (const ST_ELEMENT_TYPE *,
										const ST_ELEMENT_TYPE * ST_SORT_PROTO_ARG);
#endif

/* 声明排序函数。注意末尾的可选参数。 */
ST_SCOPE void ST_SORT(ST_ELEMENT_TYPE * first, size_t n
					  ST_SORT_PROTO_ELEMENT_SIZE
					  ST_SORT_PROTO_COMPARE
					  ST_SORT_PROTO_ARG);

#endif

#ifdef ST_DEFINE

/* 排序用的私有辅助函数 */
#define ST_MED3 ST_MAKE_NAME(ST_SORT, med3)
#define ST_SWAP ST_MAKE_NAME(ST_SORT, swap)
#define ST_SWAPN ST_MAKE_NAME(ST_SORT, swapn)

/* 预期运行非常大规模排序的使用者，可能需要排序可被中断。 */
#ifdef ST_CHECK_FOR_INTERRUPTS
#define DO_CHECK_FOR_INTERRUPTS() CHECK_FOR_INTERRUPTS()
#else
#define DO_CHECK_FOR_INTERRUPTS()
#endif

/*
 * 建立包装宏，使它们知道如何用正确的参数调用
 * compare、med3 和 sort。
 */
#ifdef ST_COMPARE_RUNTIME_POINTER
#define DO_COMPARE(a_, b_) ST_COMPARE((a_), (b_) ST_SORT_INVOKE_ARG)
#elif defined(ST_COMPARE_ARG_TYPE)
#define DO_COMPARE(a_, b_) ST_COMPARE((a_), (b_), arg)
#else
#define DO_COMPARE(a_, b_) ST_COMPARE((a_), (b_))
#endif
#define DO_MED3(a_, b_, c_)												\
	ST_MED3((a_), (b_), (c_)											\
			ST_SORT_INVOKE_COMPARE										\
			ST_SORT_INVOKE_ARG)
#define DO_SORT(a_, n_)													\
	ST_SORT((a_), (n_)													\
			ST_SORT_INVOKE_ELEMENT_SIZE									\
			ST_SORT_INVOKE_COMPARE										\
			ST_SORT_INVOKE_ARG)

/*
 * 若处理的是 void 指针，将基于 uint8 做指针运算，并用运行时的
 * element_size 在数组中步进和交换元素。否则将使用
 * ST_ELEMENT_TYPE。
 */
#ifndef ST_ELEMENT_TYPE_VOID
#define ST_POINTER_TYPE ST_ELEMENT_TYPE
#define ST_POINTER_STEP 1
#define DO_SWAPN(a_, b_, n_) ST_SWAPN((a_), (b_), (n_))
#define DO_SWAP(a_, b_) ST_SWAP((a_), (b_))
#else
#define ST_POINTER_TYPE uint8
#define ST_POINTER_STEP element_size
#define DO_SWAPN(a_, b_, n_) ST_SWAPN((a_), (b_), (n_))
#define DO_SWAP(a_, b_) DO_SWAPN((a_), (b_), element_size)
#endif

/*
 * 求三个值的中位数。目前看来，把比较函数在此处内联
 * 性能最好，但 med3 函数不要内联进 qsort 函数。
 *
 */
static pg_noinline ST_ELEMENT_TYPE *
ST_MED3(ST_ELEMENT_TYPE * a,
		ST_ELEMENT_TYPE * b,
		ST_ELEMENT_TYPE * c
		ST_SORT_PROTO_COMPARE
		ST_SORT_PROTO_ARG)
{
	return DO_COMPARE(a, b) < 0 ?
		(DO_COMPARE(b, c) < 0 ? b : (DO_COMPARE(a, c) < 0 ? c : a))
		: (DO_COMPARE(b, c) > 0 ? b : (DO_COMPARE(a, c) < 0 ? a : c));
}

static inline void
ST_SWAP(ST_POINTER_TYPE * a, ST_POINTER_TYPE * b)
{
	ST_POINTER_TYPE tmp = *a;

	*a = *b;
	*b = tmp;
}

static inline void
ST_SWAPN(ST_POINTER_TYPE * a, ST_POINTER_TYPE * b, size_t n)
{
	for (size_t i = 0; i < n; ++i)
		ST_SWAP(&a[i], &b[i]);
}

/*
 * 对数组排序。
 */
ST_SCOPE void
ST_SORT(ST_ELEMENT_TYPE * data, size_t n
		ST_SORT_PROTO_ELEMENT_SIZE
		ST_SORT_PROTO_COMPARE
		ST_SORT_PROTO_ARG)
{
	ST_POINTER_TYPE *a = (ST_POINTER_TYPE *) data,
			   *pa,
			   *pb,
			   *pc,
			   *pd,
			   *pl,
			   *pm,
			   *pn;
	size_t		d1,
				d2;
	int			r,
				presorted;

loop:
	DO_CHECK_FOR_INTERRUPTS();
	if (n < 7)
	{
		for (pm = a + ST_POINTER_STEP; pm < a + n * ST_POINTER_STEP;
			 pm += ST_POINTER_STEP)
			for (pl = pm; pl > a && DO_COMPARE(pl - ST_POINTER_STEP, pl) > 0;
				 pl -= ST_POINTER_STEP)
				DO_SWAP(pl, pl - ST_POINTER_STEP);
		return;
	}
	presorted = 1;
	for (pm = a + ST_POINTER_STEP; pm < a + n * ST_POINTER_STEP;
		 pm += ST_POINTER_STEP)
	{
		DO_CHECK_FOR_INTERRUPTS();
		if (DO_COMPARE(pm - ST_POINTER_STEP, pm) > 0)
		{
			presorted = 0;
			break;
		}
	}
	if (presorted)
		return;
	pm = a + (n / 2) * ST_POINTER_STEP;
	if (n > 7)
	{
		pl = a;
		pn = a + (n - 1) * ST_POINTER_STEP;
		if (n > 40)
		{
			size_t		d = (n / 8) * ST_POINTER_STEP;

			pl = DO_MED3(pl, pl + d, pl + 2 * d);
			pm = DO_MED3(pm - d, pm, pm + d);
			pn = DO_MED3(pn - 2 * d, pn - d, pn);
		}
		pm = DO_MED3(pl, pm, pn);
	}
	DO_SWAP(a, pm);
	pa = pb = a + ST_POINTER_STEP;
	pc = pd = a + (n - 1) * ST_POINTER_STEP;
	for (;;)
	{
		while (pb <= pc && (r = DO_COMPARE(pb, a)) <= 0)
		{
			if (r == 0)
			{
				DO_SWAP(pa, pb);
				pa += ST_POINTER_STEP;
			}
			pb += ST_POINTER_STEP;
			DO_CHECK_FOR_INTERRUPTS();
		}
		while (pb <= pc && (r = DO_COMPARE(pc, a)) >= 0)
		{
			if (r == 0)
			{
				DO_SWAP(pc, pd);
				pd -= ST_POINTER_STEP;
			}
			pc -= ST_POINTER_STEP;
			DO_CHECK_FOR_INTERRUPTS();
		}
		if (pb > pc)
			break;
		DO_SWAP(pb, pc);
		pb += ST_POINTER_STEP;
		pc -= ST_POINTER_STEP;
	}
	pn = a + n * ST_POINTER_STEP;
	d1 = Min(pa - a, pb - pa);
	DO_SWAPN(a, pb - d1, d1);
	d1 = Min(pd - pc, pn - pd - ST_POINTER_STEP);
	DO_SWAPN(pb, pn - d1, d1);
	d1 = pb - pa;
	d2 = pd - pc;
	if (d1 <= d2)
	{
		/* 对左分区递归，然后对右分区迭代 */
		if (d1 > ST_POINTER_STEP)
			DO_SORT(a, d1 / ST_POINTER_STEP);
		if (d2 > ST_POINTER_STEP)
		{
			/* 迭代而不是递归，以节省栈空间 */
			/* DO_SORT(pn - d2, d2 / ST_POINTER_STEP) */
			a = pn - d2;
			n = d2 / ST_POINTER_STEP;
			goto loop;
		}
	}
	else
	{
		/* 对右分区递归，然后对左分区迭代 */
		if (d2 > ST_POINTER_STEP)
			DO_SORT(pn - d2, d2 / ST_POINTER_STEP);
		if (d1 > ST_POINTER_STEP)
		{
			/* 迭代而不是递归，以节省栈空间 */
			/* DO_SORT(a, d1 / ST_POINTER_STEP) */
			n = d1 / ST_POINTER_STEP;
			goto loop;
		}
	}
}
#endif

#undef DO_CHECK_FOR_INTERRUPTS
#undef DO_COMPARE
#undef DO_MED3
#undef DO_SORT
#undef DO_SWAP
#undef DO_SWAPN
#undef ST_CHECK_FOR_INTERRUPTS
#undef ST_COMPARATOR_TYPE_NAME
#undef ST_COMPARE
#undef ST_COMPARE_ARG_TYPE
#undef ST_COMPARE_RUNTIME_POINTER
#undef ST_ELEMENT_TYPE
#undef ST_ELEMENT_TYPE_VOID
#undef ST_MAKE_NAME
#undef ST_MAKE_NAME_
#undef ST_MAKE_PREFIX
#undef ST_MED3
#undef ST_POINTER_STEP
#undef ST_POINTER_TYPE
#undef ST_SCOPE
#undef ST_SORT
#undef ST_SORT_INVOKE_ARG
#undef ST_SORT_INVOKE_COMPARE
#undef ST_SORT_INVOKE_ELEMENT_SIZE
#undef ST_SORT_PROTO_ARG
#undef ST_SORT_PROTO_COMPARE
#undef ST_SORT_PROTO_ELEMENT_SIZE
#undef ST_SWAP
#undef ST_SWAPN
