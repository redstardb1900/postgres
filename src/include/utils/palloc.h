/*-------------------------------------------------------------------------
 *
 * palloc.h
 *	  POSTGRES 内存分配器定义。
 *
 * 本文件含几乎每个后端模块所需的基本内存分配
 * 接口。由 postgres.h 直接包含，故此处定义处处
 * 可用。保持精简！
 * 见上。
 *
 * 内存分配发生在"上下文"内。从 palloc()/
 * MemoryContextAlloc() 取得的每块都分配在特定
 * 上下文中。重置或删除上下文即可轻松快速释放其
 * 全部内容——比逐块释放更快，也更不易泄漏。
 * 见上。
 * 我们把上下文组织成树，以便精细控制块寿命，
 * 同时仍能确保应释放的都会被释放。详见
 * utils/mmgr/README。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/palloc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PALLOC_H
#define PALLOC_H

/*
 * 类型 MemoryContextData 在 nodes/memnodes.h 中
 * 声明。多数内存分配用户只需把它当作抽象类型，
 * 故此处不提供结构内容。
 */
typedef struct MemoryContextData *MemoryContext;

/*
 * 内存上下文可注册回调函数。任一此类函数会在
 * 上下文下次重置或删除前恰好调用一次。描述该
 * 回调的 MemoryContextCallback 结构通常就分配
 * 在上下文自身内，从而无需显式管理（重置/删除
 * 动作会释放它）。
 */
typedef void (*MemoryContextCallbackFunction) (void *arg);

typedef struct MemoryContextCallback
{
	MemoryContextCallbackFunction func; /* 待调函数 */
	void	   *arg;			/* 传给它的参数 */
	struct MemoryContextCallback *next; /* 回调列表下一项 */
} MemoryContextCallback;

/*
 * CurrentMemoryContext 是 palloc() 的默认分配
 * 上下文。避免直接访问！改用 MemoryContextSwitchTo()
 * 来更改设置。
 */
extern PGDLLIMPORT MemoryContext CurrentMemoryContext;

/*
 * MemoryContextAllocExtended 标志。
 */
#define MCXT_ALLOC_HUGE			0x01	/* 允许巨大分配（> 1 GB） */
#define MCXT_ALLOC_NO_OOM		0x02	/* 内存不足时不失败 */
#define MCXT_ALLOC_ZERO			0x04	/* 清零已分配内存 */

/*
 * 基本内存分配操作（更多见 utils/memutils.h）
 */
extern void *MemoryContextAlloc(MemoryContext context, Size size);
extern void *MemoryContextAllocZero(MemoryContext context, Size size);
extern void *MemoryContextAllocZeroAligned(MemoryContext context, Size size);
extern void *MemoryContextAllocExtended(MemoryContext context,
										Size size, int flags);
extern void *MemoryContextAllocAligned(MemoryContext context,
									   Size size, Size alignto, int flags);

extern void *palloc(Size size);
extern void *palloc0(Size size);
extern void *palloc_extended(Size size, int flags);
extern void *palloc_aligned(Size size, Size alignto, int flags);
extern pg_nodiscard void *repalloc(void *pointer, Size size);
extern pg_nodiscard void *repalloc_extended(void *pointer,
											Size size, int flags);
extern pg_nodiscard void *repalloc0(void *pointer, Size oldsize, Size size);
extern void pfree(void *pointer);

/*
 * 更易写、类型更安全的变体
 */

/*
 * 为类型 "type" 的一个对象分配空间
 */
#define palloc_object(type) ((type *) palloc(sizeof(type)))
#define palloc0_object(type) ((type *) palloc0(sizeof(type)))

/*
 * 为 count 个类型 type 的对象分配空间
 */
#define palloc_array(type, count) ((type *) palloc(sizeof(type) * (count)))
#define palloc0_array(type, count) ((type *) palloc0(sizeof(type) * (count)))

/*
 * 把 pointer 所指分配改为能容纳 count 个
 * 类型 type 的对象
 */
#define repalloc_array(pointer, type, count) ((type *) repalloc(pointer, sizeof(type) * (count)))
#define repalloc0_array(pointer, type, oldcount, count) ((type *) repalloc0(pointer, sizeof(type) * (oldcount), sizeof(type) * (count)))

/*
 * palloc() 结果总是字对齐的，故决定用哪个 MemSet
 * 变体时可不测指针对齐。注意本变体无优势，除非
 * "sz" 参数是编译期常量，否则不应使用；因此它
 * 多次求值参数在实践中不是问题。
 * 见上。
 * 见上。
 */
#define palloc0fast(sz) \
	( MemSetTest(0, sz) ? \
		MemoryContextAllocZeroAligned(CurrentMemoryContext, sz) : \
		MemoryContextAllocZero(CurrentMemoryContext, sz) )

/* 更高上限分配器。 */
extern void *MemoryContextAllocHuge(MemoryContext context, Size size);
extern pg_nodiscard void *repalloc_huge(void *pointer, Size size);

/*
 * 虽本头名义上仅后端用，但某些前端程序如
 * pg_controldata 会经 postgres.h 包含它。对某些
 * 编译器须在此场景隐藏 MemoryContextSwitchTo 的
 * 内联定义；故有 #ifndef FRONTEND。
 */

#ifndef FRONTEND
static inline MemoryContext
MemoryContextSwitchTo(MemoryContext context)
{
	MemoryContext old = CurrentMemoryContext;

	CurrentMemoryContext = context;
	return old;
}
#endif							/* FRONTEND */

/* 内存上下文重置/删除回调的注册 */
extern void MemoryContextRegisterResetCallback(MemoryContext context,
											   MemoryContextCallback *cb);

/*
 * 类似标准 strdup()，但拷贝串分配在上下文
 * 中，而非用 malloc()。
 */
extern char *MemoryContextStrdup(MemoryContext context, const char *string);
extern char *pstrdup(const char *in);
extern char *pnstrdup(const char *in, Size len);

extern char *pchomp(const char *in);

/* sprintf 到 palloc 缓冲区——这些在 psprintf.c */
extern char *psprintf(const char *fmt,...) pg_attribute_printf(1, 2);
extern size_t pvsnprintf(char *buf, size_t len, const char *fmt, va_list args) pg_attribute_printf(3, 0);

#endif							/* PALLOC_H */
