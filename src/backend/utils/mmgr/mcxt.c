/*-------------------------------------------------------------------------
 *
 * mcxt.c
 *	  POSTGRES 内存上下文管理代码。
 *
 * 本模块处理与被操作上下文种类无关的管理操作。
 * 它经由某个上下文的 MemoryContextMethods 结构中
 * 的函数指针，调用特定上下文类型的操作。
 *
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/mcxt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/fmgrprotos.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/memutils_internal.h"
#include "utils/memutils_memorychunk.h"


static void BogusFree(void *pointer);
static void *BogusRealloc(void *pointer, Size size);
static MemoryContext BogusGetChunkContext(void *pointer);
static Size BogusGetChunkSpace(void *pointer);

/*****************************************************************************
 *	  全局内存 													 *
 *****************************************************************************/

static const MemoryContextMethods mcxt_methods[] = {
	/* aset.c */
	[MCTX_ASET_ID].alloc = AllocSetAlloc,
	[MCTX_ASET_ID].free_p = AllocSetFree,
	[MCTX_ASET_ID].realloc = AllocSetRealloc,
	[MCTX_ASET_ID].reset = AllocSetReset,
	[MCTX_ASET_ID].delete_context = AllocSetDelete,
	[MCTX_ASET_ID].get_chunk_context = AllocSetGetChunkContext,
	[MCTX_ASET_ID].get_chunk_space = AllocSetGetChunkSpace,
	[MCTX_ASET_ID].is_empty = AllocSetIsEmpty,
	[MCTX_ASET_ID].stats = AllocSetStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_ASET_ID].check = AllocSetCheck,
#endif

	/* generation.c */
	[MCTX_GENERATION_ID].alloc = GenerationAlloc,
	[MCTX_GENERATION_ID].free_p = GenerationFree,
	[MCTX_GENERATION_ID].realloc = GenerationRealloc,
	[MCTX_GENERATION_ID].reset = GenerationReset,
	[MCTX_GENERATION_ID].delete_context = GenerationDelete,
	[MCTX_GENERATION_ID].get_chunk_context = GenerationGetChunkContext,
	[MCTX_GENERATION_ID].get_chunk_space = GenerationGetChunkSpace,
	[MCTX_GENERATION_ID].is_empty = GenerationIsEmpty,
	[MCTX_GENERATION_ID].stats = GenerationStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_GENERATION_ID].check = GenerationCheck,
#endif

	/* slab.c */
	[MCTX_SLAB_ID].alloc = SlabAlloc,
	[MCTX_SLAB_ID].free_p = SlabFree,
	[MCTX_SLAB_ID].realloc = SlabRealloc,
	[MCTX_SLAB_ID].reset = SlabReset,
	[MCTX_SLAB_ID].delete_context = SlabDelete,
	[MCTX_SLAB_ID].get_chunk_context = SlabGetChunkContext,
	[MCTX_SLAB_ID].get_chunk_space = SlabGetChunkSpace,
	[MCTX_SLAB_ID].is_empty = SlabIsEmpty,
	[MCTX_SLAB_ID].stats = SlabStats,
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_SLAB_ID].check = SlabCheck,
#endif

	/* alignedalloc.c */
	[MCTX_ALIGNED_REDIRECT_ID].alloc = NULL,	/* 不需要 */
	[MCTX_ALIGNED_REDIRECT_ID].free_p = AlignedAllocFree,
	[MCTX_ALIGNED_REDIRECT_ID].realloc = AlignedAllocRealloc,
	[MCTX_ALIGNED_REDIRECT_ID].reset = NULL,	/* 不需要 */
	[MCTX_ALIGNED_REDIRECT_ID].delete_context = NULL,	/* 不需要 */
	[MCTX_ALIGNED_REDIRECT_ID].get_chunk_context = AlignedAllocGetChunkContext,
	[MCTX_ALIGNED_REDIRECT_ID].get_chunk_space = AlignedAllocGetChunkSpace,
	[MCTX_ALIGNED_REDIRECT_ID].is_empty = NULL, /* 不需要 */
	[MCTX_ALIGNED_REDIRECT_ID].stats = NULL,	/* 不需要 */
#ifdef MEMORY_CONTEXT_CHECKING
	[MCTX_ALIGNED_REDIRECT_ID].check = NULL,	/* 不需要 */
#endif


	/*
	 * 尚未使用的 ID 应在这里有哑条目。这样，若向 pfree
	 * 之类传入伪造指针时也能干净地失败。似乎只要
	 * 为可能从区块检视中触发的那些方法提供例程
	 * 就足够了（见下方的 MCXT_METHOD 调用）。
	 */

	[MCTX_UNUSED1_ID].free_p = BogusFree,
	[MCTX_UNUSED1_ID].realloc = BogusRealloc,
	[MCTX_UNUSED1_ID].get_chunk_context = BogusGetChunkContext,
	[MCTX_UNUSED1_ID].get_chunk_space = BogusGetChunkSpace,

	[MCTX_UNUSED2_ID].free_p = BogusFree,
	[MCTX_UNUSED2_ID].realloc = BogusRealloc,
	[MCTX_UNUSED2_ID].get_chunk_context = BogusGetChunkContext,
	[MCTX_UNUSED2_ID].get_chunk_space = BogusGetChunkSpace,

	[MCTX_UNUSED3_ID].free_p = BogusFree,
	[MCTX_UNUSED3_ID].realloc = BogusRealloc,
	[MCTX_UNUSED3_ID].get_chunk_context = BogusGetChunkContext,
	[MCTX_UNUSED3_ID].get_chunk_space = BogusGetChunkSpace,

	[MCTX_UNUSED4_ID].free_p = BogusFree,
	[MCTX_UNUSED4_ID].realloc = BogusRealloc,
	[MCTX_UNUSED4_ID].get_chunk_context = BogusGetChunkContext,
	[MCTX_UNUSED4_ID].get_chunk_space = BogusGetChunkSpace,
};

/*
 * CurrentMemoryContext
 *		分配所用的默认内存上下文。
 */
MemoryContext CurrentMemoryContext = NULL;

/*
 * 标准的顶层上下文。各上下文的用途说明请参阅
 * src/backend/utils/mmgr/README
 */
MemoryContext TopMemoryContext = NULL;
MemoryContext ErrorContext = NULL;
MemoryContext PostmasterContext = NULL;
MemoryContext CacheMemoryContext = NULL;
MemoryContext MessageContext = NULL;
MemoryContext TopTransactionContext = NULL;
MemoryContext CurTransactionContext = NULL;

/* 这是指向活动门户内存上下文的临时链接： */
MemoryContext PortalContext = NULL;

static void MemoryContextCallResetCallbacks(MemoryContext context);
static void MemoryContextStatsInternal(MemoryContext context, int level,
									   bool print, int max_children,
									   MemoryContextCounters *totals,
									   bool print_to_stderr);
static void MemoryContextStatsPrint(MemoryContext context, void *passthru,
									const char *stats_string,
									bool print_to_stderr);

/*
 * 不要在临界区内做内存分配，因为内存耗尽错误将
 * 升级为 PANIC。为了强制执行该规则，分配函数会用
 * Assert 来断言这一点。
 */
#define AssertNotInCriticalSection(context) \
	Assert(CritSectionCount == 0 || (context)->allowInCritSection)

/*
 * 在 'pointer' 所属内存上下文类型的 MemoryContextMethods
 * 中调用给定函数。
 */
#define MCXT_METHOD(pointer, method) \
	mcxt_methods[GetMemoryChunkMethodID(pointer)].method

/*
 * GetMemoryChunkMethodID
 *		从紧邻 'pointer' 之前的 uint64 区块头部中返回
 *		MemoryContextMethodID。
 */
static inline MemoryContextMethodID
GetMemoryChunkMethodID(const void *pointer)
{
	uint64		header;

	/*
	 * 尝试检测交给我们的伪造指针，虽能力有限。
	 * 据推测，未 MAXALIGNED 的指针并没有指向一个
	 * 已分配块。
	 */
	Assert(pointer == (const void *) MAXALIGN(pointer));

	/* 允许访问 uint64 头部 */
	VALGRIND_MAKE_MEM_DEFINED((char *) pointer - sizeof(uint64), sizeof(uint64));

	header = *((const uint64 *) ((const char *) pointer - sizeof(uint64)));

	/* 禁止访问 uint64 头部 */
	VALGRIND_MAKE_MEM_NOACCESS((char *) pointer - sizeof(uint64), sizeof(uint64));

	return (MemoryContextMethodID) (header & MEMORY_CONTEXT_METHODID_MASK);
}

/*
 * GetMemoryChunkHeader
 *		返回紧邻 'pointer' 之前的 uint64 区块头部。
 *
 * 它只在 GetMemoryChunkMethodID 之后使用，故无需错误检查。
 */
static inline uint64
GetMemoryChunkHeader(const void *pointer)
{
	uint64		header;

	/* 允许访问 uint64 头部 */
	VALGRIND_MAKE_MEM_DEFINED((char *) pointer - sizeof(uint64), sizeof(uint64));

	header = *((const uint64 *) ((const char *) pointer - sizeof(uint64)));

	/* 禁止访问 uint64 头部 */
	VALGRIND_MAKE_MEM_NOACCESS((char *) pointer - sizeof(uint64), sizeof(uint64));

	return header;
}

/*
 * 用于捕获无效内存上下文方法 ID 的支持例程
 *（对伪造指针调用 pfree 等所致）。作为可能的
 * 调试辅助，我们会连同指针地址一起报告头部字
 *（能走到这里就必有可访问的头部字）。
 */
static void
BogusFree(void *pointer)
{
	elog(ERROR, "pfree called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
}

static void *
BogusRealloc(void *pointer, Size size)
{
	elog(ERROR, "repalloc called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
	return NULL;				/* 让编译器安静 */
}

static MemoryContext
BogusGetChunkContext(void *pointer)
{
	elog(ERROR, "GetMemoryChunkContext called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
	return NULL;				/* 让编译器安静 */
}

static Size
BogusGetChunkSpace(void *pointer)
{
	elog(ERROR, "GetMemoryChunkSpace called with invalid pointer %p (header 0x%016llx)",
		 pointer, (unsigned long long) GetMemoryChunkHeader(pointer));
	return 0;					/* 让编译器安静 */
}


/*****************************************************************************
 *	  导出的例程  													 *
 *****************************************************************************/


/*
 * MemoryContextInit
 *		启动内存上下文子系统。
 *
 * 创建上下文或在其中分配内存之前，必须调用它。
 * TopMemoryContext 和 ErrorContext 在这里初始化；
 * 其余上下文必须在之后创建。
 *
 * 正常多后台运行时，本函数只在 postmaster
 * 启动时调用一次，各后台自身启动并不调用它
 *（因为后台 fork 自 postmaster，已继承了
 * 初始化完毕的上下文子系统）。但在 EXEC_BACKEND
 * 构建中每进程须自行完成此步。
 *
 * 独立后台中，本函数必须在后台启动时调用。
 */
void
MemoryContextInit(void)
{
	Assert(TopMemoryContext == NULL);

	/*
	 * 首先，初始化 TopMemoryContext，其他上下文之父。
	 */
	TopMemoryContext = AllocSetContextCreate((MemoryContext) NULL,
											 "TopMemoryContext",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * CurrentMemoryContext 没有别的可指向之处，让它指向
	 * TopMemoryContext。调用者应尽快更改！
	 */
	CurrentMemoryContext = TopMemoryContext;

	/*
	 * 将 ErrorContext 初始化为慢增长率的 AllocSetContext ---
	 * 我们并不指望其中会分配太多东西。更重要的是，
	 * 要求它在任何时刻都保有至少 8K。这是唯一必须
	 * 常驻内存的上下文 --- 我们要确保即使别处耗尽，
	 * ErrorContext 仍有内存！此外，允许在临界区内
	 * 对 ErrorContext 进行分配。否则 PANIC 会在错误
	 * 报告代码中导致断言失败，抢在打印真正失败
	 * 原因之前。
	 *
	 * 这必须是本函数的最后一步，因为一旦 ErrorContext
	 * 非 null，内存管理就要能工作。
	 */
	ErrorContext = AllocSetContextCreate(TopMemoryContext,
										 "ErrorContext",
										 8 * 1024,
										 8 * 1024,
										 8 * 1024);
	MemoryContextAllowInCriticalSection(ErrorContext, true);
}

/*
 * MemoryContextReset
 *		释放上下文内分配的全部空间，并删除其全部
 *		后代上下文（具名上下文本身除外）。
 */
void
MemoryContextReset(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/* 常见情形下没有子上下文时，省去一次函数调用 */
	if (context->firstchild != NULL)
		MemoryContextDeleteChildren(context);

	/* 启动后或上次重置以来无 palloc 时省去一次调用 */
	if (!context->isReset)
		MemoryContextResetOnly(context);
}

/*
 * MemoryContextResetOnly
 *		释放该上下文内分配的全部空间。
 *		对其后代上下文不做任何处理。
 */
void
MemoryContextResetOnly(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/* 启动或上次重置以来无 palloc 则无事可做 */
	if (!context->isReset)
	{
		MemoryContextCallResetCallbacks(context);

		/*
		 * 若 context->ident 指向上下文内的内存，它将变成
		 * 悬空指针。可在此置为 NULL，但这会破坏把
		 * ident 存于别处（如父上下文中）的有效编码
		 * 模式。目前我们假定程序员已处理妥当。
		 *
		 */

		context->methods->reset(context);
		context->isReset = true;
		VALGRIND_DESTROY_MEMPOOL(context);
		VALGRIND_CREATE_MEMPOOL(context, 0, false);
	}
}

/*
 * MemoryContextResetChildren
 *		释放上下文全部后代内已分配的空间，
 *		但不删除这些上下文本身。具名上下文
 *		本身不受影响。
 */
void
MemoryContextResetChildren(MemoryContext context)
{
	MemoryContext child;

	Assert(MemoryContextIsValid(context));

	for (child = context->firstchild; child != NULL; child = child->nextchild)
	{
		MemoryContextResetChildren(child);
		MemoryContextResetOnly(child);
	}
}

/*
 * MemoryContextDelete
 *		删除上下文及其全部后代，并释放其中
 *		已分配空间。
 *
 * 特定类型的删除例程会移除上下文的全部存储，
 * 但我们须递归处理其子上下文。
 * 还必须把上下文从其父上下文（若有）解链。
 */
void
MemoryContextDelete(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));
	/* 最好不要删除 TopMemoryContext…… */
	Assert(context != TopMemoryContext);
	/* 也不要删除 CurrentMemoryContext */
	Assert(context != CurrentMemoryContext);

	/* 常见情形下没有子上下文时，省去一次函数调用 */
	if (context->firstchild != NULL)
		MemoryContextDeleteChildren(context);

	/*
	 * 究竟是在解链之前还是之后做这件事并不完全清
	 * 楚；但回调中若发生错误，且我们在此之后才做，
	 * 整个上下文（若它不是根上下文）就很可能泄
	 * 漏，故在之前做。
	 */
	MemoryContextCallResetCallbacks(context);

	/*
	 * 我们在删除之前先把上下文从父上下文解链，
	 * 样若发生错误，上下文树中就不会仍挂有已删除
	 * 或已损坏的上下文。宁漏勿崩。
	 */
	MemoryContextSetParent(context, NULL);

	/*
	 * 同时重置上下文的 ident 指针，以防它指向上下文
	 * 内部。只有当有人试图对已解链的上下文获
	 * 取统计信息时才有影响，不常见，但求稳。
	 */
	context->ident = NULL;

	context->methods->delete_context(context);

	VALGRIND_DESTROY_MEMPOOL(context);
}

/*
 * MemoryContextDeleteChildren
 *		删除具名上下文的全部后代，并释放其中
 *		已分配的空间。具名上下文本身不受影响。
 */
void
MemoryContextDeleteChildren(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/*
	 * MemoryContextDelete 会把子上下文从我这里解链，
	 * 只管迭代即可。
	 */
	while (context->firstchild != NULL)
		MemoryContextDelete(context->firstchild);
}

/*
 * MemoryContextRegisterResetCallback
 *		注册下次上下文重置/删除前要调用的函数。
 *		这类回调按注册的逆序被调用。
 *
 * 调用者负责分配一个 MemoryContextCallback 结构来
 * 保存此次回调请求的信息，并填写结构中的 "func"
 * 和 "arg" 字段以表明要用什么参数调用什么函数。
 * 回调结构通常应在指定上下文中分配，因为这样它
 * 在不再需要时就会被自动释放。
 *
 *
 * 回调注册后没有注销用的 API。若不想让它再有
 * 所作为，就把其 "arg" 所指向的状态调整为表明
 * 这一点。
 */
void
MemoryContextRegisterResetCallback(MemoryContext context,
								   MemoryContextCallback *cb)
{
	Assert(MemoryContextIsValid(context));

	/* 插到头部，从而先于更早的注册者被调用。 */
	cb->next = context->reset_cbs;
	context->reset_cbs = cb;
	/* 标记上下文为未重置（多半已经是了）。 */
	context->isReset = false;
}

/*
 * MemoryContextCallResetCallbacks
 *		为上下文调用全部已注册回调的内部函数。
 */
static void
MemoryContextCallResetCallbacks(MemoryContext context)
{
	MemoryContextCallback *cb;

	/*
	 * 我们在调用之前先把每个回调从链表弹出。这样，
	 * 若回调内部发生错误，之后我们很可能重置或删除
	 * 该上下文时，就不会试图再调用它一次。
	 */
	while ((cb = context->reset_cbs) != NULL)
	{
		context->reset_cbs = cb->next;
		cb->func(cb->arg);
	}
}

/*
 * MemoryContextSetIdentifier
 *		设置内存上下文的标识符字符串。
 *
 * 可以提供标识符来帮助区分内存上下文统计转储中
 * 同类的不同上下文。标识符字符串的存活期至少要
 * 与其所属上下文一样长；通常它就分配在该上下文
 * 内，这样它就会在上下文删除时自动消失。传入
 * id = NULL 即可忘掉旧标识符。
 */
void
MemoryContextSetIdentifier(MemoryContext context, const char *id)
{
	Assert(MemoryContextIsValid(context));
	context->ident = id;
}

/*
 * MemoryContextSetParent
 *		把上下文改为属于新父上下文（或无父）。
 *
 * 我们把它作为 API 函数提供，因为有时在创建之后
 * 更改上下文的生命期很有用。例如，上下文可以先
 * 创建于某临时上下文之下，填入数据，然后改挂
 * 到 CacheMemoryContext 之下使其长期存活。这样，
 * 若在其内容完全建立之前发生失败，也无需专门
 * 费力清除该上下文。
 *
 * 调用者常假定本函数不会失败，故勿在其中放任何
 * elog(ERROR) 调用。
 *
 * 调用者可能犯的一个错误是把上下文改挂到它自己
 * 之下，在上下文图中形成环。我们在此断言
 * context != new_parent，但检查多级环似乎得不偿失。
 */
void
MemoryContextSetParent(MemoryContext context, MemoryContext new_parent)
{
	Assert(MemoryContextIsValid(context));
	Assert(context != new_parent);

	/* 已有正确父上下文时的快速路径 */
	if (new_parent == context->parent)
		return;

	/* 从现有父上下文解链 */
	if (context->parent)
	{
		MemoryContext parent = context->parent;

		if (context->prevchild != NULL)
			context->prevchild->nextchild = context->nextchild;
		else
		{
			Assert(parent->firstchild == context);
			parent->firstchild = context->nextchild;
		}

		if (context->nextchild != NULL)
			context->nextchild->prevchild = context->prevchild;
	}

	/* 重挂链 */
	if (new_parent)
	{
		Assert(MemoryContextIsValid(new_parent));
		context->parent = new_parent;
		context->prevchild = NULL;
		context->nextchild = new_parent->firstchild;
		if (new_parent->firstchild != NULL)
			new_parent->firstchild->prevchild = context;
		new_parent->firstchild = context;
	}
	else
	{
		context->parent = NULL;
		context->prevchild = NULL;
		context->nextchild = NULL;
	}
}

/*
 * MemoryContextAllowInCriticalSection
 *		允许/禁止在临界区内于此内存上下文中进行分配。
 *
 *
 * 通常不允许在临界区内进行内存分配，因为一旦失败
 * 就会导致 PANIC。但也有少数例外，比如与调试代码
 * 相关、生产环境中不应启用的分配。本函数可用于把
 * 特定内存上下文从 palloc() 的断言中豁免。
 *
 */
void
MemoryContextAllowInCriticalSection(MemoryContext context, bool allow)
{
	Assert(MemoryContextIsValid(context));

	context->allowInCritSection = allow;
}

/*
 * GetMemoryChunkContext
 *		给定一个当前已分配的区块，确定该区块所属的
 *		MemoryContext。
 */
MemoryContext
GetMemoryChunkContext(void *pointer)
{
	return MCXT_METHOD(pointer, get_chunk_context) (pointer);
}

/*
 * GetMemoryChunkSpace
 *		给定一个已分配的区块，确定其占用的总空间
 *		（包括全部内存分配开销）。
 *
 * 这对度量一组已分配区块占用的总空间很有用。
 *
 */
Size
GetMemoryChunkSpace(void *pointer)
{
	return MCXT_METHOD(pointer, get_chunk_space) (pointer);
}

/*
 * MemoryContextGetParent
 *		获取指定上下文的父上下文（若有）
 */
MemoryContext
MemoryContextGetParent(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	return context->parent;
}

/*
 * MemoryContextIsEmpty
 *		内存上下文是否不含已分配空间？
 */
bool
MemoryContextIsEmpty(MemoryContext context)
{
	Assert(MemoryContextIsValid(context));

	/*
	 * 目前，只要上下文有任何子上下文，我们就认为它
	 * 非空；此点日后或应修改。
	 */
	if (context->firstchild != NULL)
		return false;
	/* 否则使用特定类型的查询 */
	return context->methods->is_empty(context);
}

/*
 * 找出分配给此内存上下文各块的内存。recurse 为
 * true 时含子上下文。
 */
Size
MemoryContextMemAllocated(MemoryContext context, bool recurse)
{
	Size		total = context->mem_allocated;

	Assert(MemoryContextIsValid(context));

	if (recurse)
	{
		MemoryContext child;

		for (child = context->firstchild;
			 child != NULL;
			 child = child->nextchild)
			total += MemoryContextMemAllocated(child, true);
	}

	return total;
}

/*
 * MemoryContextStats
 *		打印具名上下文及其全部后代的统计信息。
 *
 * 这只是个调试工具，因此并不花哨。不过，当输出本
 * 来会非常长时，我们确实会做些汇总。统计信息被送
 * 往 stderr。
 */
void
MemoryContextStats(MemoryContext context)
{
	/* 对子上下文数量硬设上限通常就足够了 */
	MemoryContextStatsDetail(context, 100, true);
}

/*
 * MemoryContextStatsDetail
 *
 * 当你想改变显示的子上下文数量时的入口点。
 *
 * 若 print_to_stderr 为 true，用 fprintf(stderr) 打印
 * 内存上下文统计，否则用 ereport()。
 */
void
MemoryContextStatsDetail(MemoryContext context, int max_children,
						 bool print_to_stderr)
{
	MemoryContextCounters grand_totals;

	memset(&grand_totals, 0, sizeof(grand_totals));

	MemoryContextStatsInternal(context, 0, true, max_children, &grand_totals, print_to_stderr);

	if (print_to_stderr)
		fprintf(stderr,
				"Grand total: %zu bytes in %zu blocks; %zu free (%zu chunks); %zu used\n",
				grand_totals.totalspace, grand_totals.nblocks,
				grand_totals.freespace, grand_totals.freechunks,
				grand_totals.totalspace - grand_totals.freespace);
	else

		/*
		 * 用 LOG_SERVER_ONLY 防止把内存上下文发送给已
		 * 连接的客户端。
		 *
		 * 我们不把后台中全部内存上下文的信息缓冲
		 * 到 StringInfo 中再作一条消息记日志。那会
		 * 要扩大缓冲区，而后台里可能有大量内存上
		 * 下文，存在 OOM 风险。因此我们改为每个内存
		 * 上下文记一条消息。
		 */
		ereport(LOG_SERVER_ONLY,
				(errhidestmt(true),
				 errhidecontext(true),
				 errmsg_internal("Grand total: %zu bytes in %zu blocks; %zu free (%zu chunks); %zu used",
								 grand_totals.totalspace, grand_totals.nblocks,
								 grand_totals.freespace, grand_totals.freechunks,
								 grand_totals.totalspace - grand_totals.freespace)));
}

/*
 * MemoryContextStatsInternal
 *		MemoryContextStats 的一层递归
 *
 * 若 print 为 true 则打印本上下文；无论如何都会把计数
 * 累入 *totals。
 */
static void
MemoryContextStatsInternal(MemoryContext context, int level,
						   bool print, int max_children,
						   MemoryContextCounters *totals,
						   bool print_to_stderr)
{
	MemoryContextCounters local_totals;
	MemoryContext child;
	int			ichild;

	Assert(MemoryContextIsValid(context));

	/* 检视上下文本身 */
	context->methods->stats(context,
							print ? MemoryContextStatsPrint : NULL,
							(void *) &level,
							totals, print_to_stderr);

	/*
	 * 检视子上下文。若其数量超过 max_children，则其余
	 * 不显式打印，而只是汇总。
	 */
	memset(&local_totals, 0, sizeof(local_totals));

	for (child = context->firstchild, ichild = 0;
		 child != NULL;
		 child = child->nextchild, ichild++)
	{
		if (ichild < max_children)
			MemoryContextStatsInternal(child, level + 1,
									   print, max_children,
									   totals,
									   print_to_stderr);
		else
			MemoryContextStatsInternal(child, level + 1,
									   false, max_children,
									   &local_totals,
									   print_to_stderr);
	}

	/* 处理多余子上下文 */
	if (ichild > max_children)
	{
		if (print)
		{
			if (print_to_stderr)
			{
				int			i;

				for (i = 0; i <= level; i++)
					fprintf(stderr, "  ");
				fprintf(stderr,
						"%d more child contexts containing %zu total in %zu blocks; %zu free (%zu chunks); %zu used\n",
						ichild - max_children,
						local_totals.totalspace,
						local_totals.nblocks,
						local_totals.freespace,
						local_totals.freechunks,
						local_totals.totalspace - local_totals.freespace);
			}
			else
				ereport(LOG_SERVER_ONLY,
						(errhidestmt(true),
						 errhidecontext(true),
						 errmsg_internal("level: %d; %d more child contexts containing %zu total in %zu blocks; %zu free (%zu chunks); %zu used",
										 level,
										 ichild - max_children,
										 local_totals.totalspace,
										 local_totals.nblocks,
										 local_totals.freespace,
										 local_totals.freechunks,
										 local_totals.totalspace - local_totals.freespace)));
		}

		if (totals)
		{
			totals->nblocks += local_totals.nblocks;
			totals->freechunks += local_totals.freechunks;
			totals->totalspace += local_totals.totalspace;
			totals->freespace += local_totals.freespace;
		}
	}
}

/*
 * MemoryContextStatsPrint
 *		MemoryContextStatsInternal 打印回调
 *
 * 目前 passthru 只是指向 "int level"；今后可能会
 * 把它弄得更复杂。
 */
static void
MemoryContextStatsPrint(MemoryContext context, void *passthru,
						const char *stats_string,
						bool print_to_stderr)
{
	int			level = *(int *) passthru;
	const char *name = context->name;
	const char *ident = context->ident;
	char		truncated_ident[110];
	int			i;

	/*
	 * 似乎更可取的做法是只用哈希表名来标注 dynahash
	 * 上下文。它们本身就已足够唯一，所以 "dynahash"
	 * 一词无甚帮助，且这样与 v11 前的做法更一致。
	 */
	if (ident && strcmp(name, "dynahash") == 0)
	{
		name = ident;
		ident = NULL;
	}

	truncated_ident[0] = '\0';

	if (ident)
	{
		/*
		 * 有些上下文的标识符可能非常长（如 SQL 查询）。
		 * 一律在 100 字节处截断，但要小心不弄断
		 * 多字节字符。另外把换行等 ASCII 控制字符
		 * 换为空格。
		 */
		int			idlen = strlen(ident);
		bool		truncated = false;

		strcpy(truncated_ident, ": ");
		i = strlen(truncated_ident);

		if (idlen > 100)
		{
			idlen = pg_mbcliplen(ident, idlen, 100);
			truncated = true;
		}

		while (idlen-- > 0)
		{
			unsigned char c = *ident++;

			if (c < ' ')
				c = ' ';
			truncated_ident[i++] = c;
		}
		truncated_ident[i] = '\0';

		if (truncated)
			strcat(truncated_ident, "...");
	}

	if (print_to_stderr)
	{
		for (i = 0; i < level; i++)
			fprintf(stderr, "  ");
		fprintf(stderr, "%s: %s%s\n", name, stats_string, truncated_ident);
	}
	else
		ereport(LOG_SERVER_ONLY,
				(errhidestmt(true),
				 errhidecontext(true),
				 errmsg_internal("level: %d; %s: %s%s",
								 level, name, stats_string, truncated_ident)));
}

/*
 * MemoryContextCheck
 *		检查具名上下文各区块。
 *
 * 这只是个调试工具，因此并不花哨。
 */
#ifdef MEMORY_CONTEXT_CHECKING
void
MemoryContextCheck(MemoryContext context)
{
	MemoryContext child;

	Assert(MemoryContextIsValid(context));

	context->methods->check(context);
	for (child = context->firstchild; child != NULL; child = child->nextchild)
		MemoryContextCheck(child);
}
#endif

/*
 * MemoryContextCreate
 *		上下文创建中与类型无关的部分。
 *
 * 它只应由特定上下文类型的创建例程调用，
 * 而非供不明就里的大众调用。
 *
 * 内存上下文的创建流程如下：
 *	1.  特定类型的例程做一些初始空间分配，包括为上
 *		下文头部留足空间。若失败可 ereport()，
 *		而毫无损伤。
 *	2.	特定类型的例程设置头部的全部类型专属字段
 *		（MemoryContextData 本体之外的那些），以及它
 *		拥有一个完全有效上下文所需的其余管理字段。
 *		通常此步不会失败，但若可能失败，则应在
 *		ereport 之前释放初始分配的空间。
 *	3.	特定类型的例程调用 MemoryContextCreate() 来
 *		填写通用头部字段，并把上下文链入上下文树。
 *	4.  我们回到特定类型的例程，由它完成类型专属
 *		的初始化。此刻该例程可以做可能失败的事情
 *		（如分配更多内存），只要它确信节点留下的
 *		状态是删除能够处理的。
 *
 * node：上下文头部节点中尚未初始化的公共部分。
 * tag：标识内存上下文类型的 NodeTag 码。
 * method_id：所建上下文类型的 MemoryContextMethodID。
 * parent：父上下文；若这将是顶层上下文则为 NULL。
 * name：上下文名称（必须静态分配）。
 *
 * 上下文例程通常假定 MemoryContextCreate 不会失败，
 * 故可含 Assert 但不可含 elog/ereport。
 */
void
MemoryContextCreate(MemoryContext node,
					NodeTag tag,
					MemoryContextMethodID method_id,
					MemoryContext parent,
					const char *name)
{
	/* 临界区内不允许创建新的内存上下文 */
	Assert(CritSectionCount == 0);

	/* 初始化内存上下文头部的全部标准字段 */
	node->type = tag;
	node->isReset = true;
	node->methods = &mcxt_methods[method_id];
	node->parent = parent;
	node->firstchild = NULL;
	node->mem_allocated = 0;
	node->prevchild = NULL;
	node->name = name;
	node->ident = NULL;
	node->reset_cbs = NULL;

	/* 可以链入上下文树了 */
	if (parent)
	{
		node->nextchild = parent->firstchild;
		if (parent->firstchild != NULL)
			parent->firstchild->prevchild = node;
		parent->firstchild = node;
		/* 从父继承 allowInCritSection 标志 */
		node->allowInCritSection = parent->allowInCritSection;
	}
	else
	{
		node->nextchild = NULL;
		node->allowInCritSection = false;
	}

	VALGRIND_CREATE_MEMPOOL(node, 0, false);
}

/*
 * MemoryContextAlloc
 *		在指定上下文内分配空间。
 *
 * 它本可做成宏，但那样须把 nodes/memnodes.h
 * 引入 postgres.h，这似乎不是个好主意。
 */
void *
MemoryContextAlloc(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		MemoryContextStats(TopMemoryContext);

		/*
		 * 在本模块此处及其他地方，我们在用户可见的
		 * 错误消息中只显示目标上下文的 "name" 而非其
		 * "ident"（若有），其中可能含 SQL 命令的值等
		 * 安全敏感数据。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, context->name)));
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

/*
 * MemoryContextAllocZero
 *		类似 MemoryContextAlloc，但清零内存
 *
 *	本可以只调用 MemoryContextAlloc 再清除内存，但
 *	这是非常常见的组合，所以我们提供合并的操作。
 */
void *
MemoryContextAllocZero(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, context->name)));
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * MemoryContextAllocZeroAligned
 *		MemoryContextAllocZero 中长度适合 MemSetLoop
 *
 *	这看似过度专用，其实不然，因为 newNode() 经常
 *	以编译期常量大小被调用。
 */
void *
MemoryContextAllocZeroAligned(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, context->name)));
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetLoop(ret, 0, size);

	return ret;
}

/*
 * MemoryContextAllocExtended
 *		使用给定标志在指定上下文内分配空间。
 */
void *
MemoryContextAllocExtended(MemoryContext context, Size size, int flags)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!((flags & MCXT_ALLOC_HUGE) != 0 ? AllocHugeSizeIsValid(size) :
		  AllocSizeIsValid(size)))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %zu in memory context \"%s\".",
							   size, context->name)));
		}
		return NULL;
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * HandleLogMemoryContextInterrupt
 *		处理指示记录内存上下文日志的中断。
 *
 *
 * 全部实际工作都推迟到 ProcessLogMemoryContextInterrupt()，
 * 因为在信号处理器内无法安全地发出日志消息。
 */
void
HandleLogMemoryContextInterrupt(void)
{
	InterruptPending = true;
	LogMemoryContextPending = true;
	/* latch 由 procsignal_sigusr1_handler 设置 */
}

/*
 * ProcessLogMemoryContextInterrupt
 * 		记录此后台进程内存上下文的日志。
 *
 * 任何参与 ProcSignal 信号机制的后台进程都必须
 * 安排在看到 LogMemoryContextPending 置位时调用
 * 本函数。它由 CHECK_FOR_INTERRUPTS() 调用，这就
 * 足够，因为日志记录目标是某个后台进程。
 */
void
ProcessLogMemoryContextInterrupt(void)
{
	LogMemoryContextPending = false;

	/*
	 * 用 LOG_SERVER_ONLY 防止此消息被发送给已连接的
	 * 客户端。
	 */
	ereport(LOG_SERVER_ONLY,
			(errhidestmt(true),
			 errhidecontext(true),
			 errmsg("logging memory contexts of PID %d", MyProcPid)));

	/*
	 * 当后台进程消耗巨量内存时，记录其全部内存上下文
	 * 可能耗尽可用磁盘空间。为防止此点，我们把每
	 * 父上下文记录的子上下文数限为 100。
	 *
	 * 与 MemoryContextStats() 一样，我们认为转储变得
	 * 很长的实际情形，通常是同一父上下文下有巨量
	 * 兄弟上下文；而查看 100 个以上兄弟各自的详情，
	 * 其额外调试价值不会很大。
	 */
	MemoryContextStatsDetail(TopMemoryContext, 100, false);
}

void *
palloc(Size size)
{
	/* 复制 MemoryContextAlloc 以免增加开销 */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, context->name)));
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

void *
palloc0(Size size)
{
	/* 复制 MemoryContextAllocZero 以免增加开销 */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, context->name)));
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

void *
palloc_extended(Size size, int flags)
{
	/* 复制 MemoryContextAllocExtended 以免增加开销 */
	void	   *ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!((flags & MCXT_ALLOC_HUGE) != 0 ? AllocHugeSizeIsValid(size) :
		  AllocSizeIsValid(size)))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %zu in memory context \"%s\".",
							   size, context->name)));
		}
		return NULL;
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSetAligned(ret, 0, size);

	return ret;
}

/*
 * MemoryContextAllocAligned
 *		在 'context' 中分配按 'alignto' 对齐的 'size' 字节。
 *
 *
 * 目前，我们向 MemoryContext 的标准分配器函数申请
 * 额外的字节，然后把返回的地址按所需对齐取整。这
 * 意味着给定的 MemoryContext 必须能提供给我们一块
 * 大于 'size' 的内存区块。对 Slab 这类分配器来说
 * 这行不通，因为 slab 只允许创建上下文时指定大小
 * 的区块。
 *
 * 'alignto' 必须是 2 的幂。
 * 'flags' 可为 0，或与 MemoryContextAllocExtended() 相同。
 */
void *
MemoryContextAllocAligned(MemoryContext context,
						  Size size, Size alignto, int flags)
{
	MemoryChunk *alignedchunk;
	Size		alloc_size;
	void	   *unaligned;
	void	   *aligned;

	/* 浪费那么多空间没什么意义 */
	Assert(alignto < (128 * 1024 * 1024));

	/* 确保 alignto 是 2 的幂 */
	Assert((alignto & (alignto - 1)) == 0);

	/*
	 * 若对齐要求低于我们已保证的水平，那就只用
	 * 标准分配函数。
	 */
	if (unlikely(alignto <= MAXIMUM_ALIGNOF))
		return MemoryContextAllocExtended(context, size, flags);

	/*
	 * 我们实现对齐指针的方式是：简单地为请求大小加
	 * 上对齐量，再加一个额外的"重定向" MemoryChunk。
	 * 诸如 pfree 这类操作作用于本函数返回的指针时，
	 * 就需要这个额外的 MemoryChunk。我们用它来找到
	 * 下方 MemoryContextAllocExtended 调用所返回内存
	 * 的指针。方法是"借用"块偏移字段：不用它来找
	 * 出所属块内的偏移，而是用它来找到最初分配的
	 * 地址。
	 *
	 *
	 * 这里我们必须分配足够的额外内存，以便仍能把
	 * MemoryContextAllocExtended 返回的指针对齐，同时
	 * 又有为重定向 MemoryChunk 留出足够的空间。由于
	 * 分配至少已按 MAXIMUM_ALIGNOF 对齐，我们可以
	 * 把该量从分配大小减去以省点内存。
	 */
	alloc_size = size + PallocAlignedExtraBytes(alignto);

#ifdef MEMORY_CONTEXT_CHECKING
	/* 确保有哨兵字节的空间 */
	alloc_size += 1;
#endif

	/* 执行实际的分配 */
	unaligned = MemoryContextAllocExtended(context, alloc_size, flags);

	/* 设置对齐指针 */
	aligned = (void *) TYPEALIGN(alignto, (char *) unaligned +
								 sizeof(MemoryChunk));

	alignedchunk = PointerGetMemoryChunk(aligned);

	/*
	 * 我们这样设置重定向 MemoryChunk：让块偏移计算
	 * 指回 'unaligned' 已分配区块。这样当需要执行诸
	 * 如 pfree() 和 repalloc() 这类操作时，我们能用
	 * MemoryChunkGetBlock() 找到未对齐的区块。
	 *
	 * 我们把 'alignto' 存进 MemoryChunk 的 'value'，
	 * 这样日后要求 realloc 该指针时便知道对齐值。
	 *
	 */
	MemoryChunkSetHdrMask(alignedchunk, unaligned, alignto,
						  MCTX_ALIGNED_REDIRECT_ID);

	/* 复核我们产生的是正确对齐的指针 */
	Assert((void *) TYPEALIGN(alignto, aligned) == aligned);

#ifdef MEMORY_CONTEXT_CHECKING
	alignedchunk->requested_size = size;
	/* 设置标记以捕获"未用"空间覆写 */
	set_sentinel(aligned, size);
#endif

	/* 把重定向头部之前的字节标为不可访问 */
	VALGRIND_MAKE_MEM_NOACCESS(unaligned,
							   (char *) alignedchunk - (char *) unaligned);

	/* 禁止访问重定向区块头部。 */
	VALGRIND_MAKE_MEM_NOACCESS(alignedchunk, sizeof(MemoryChunk));

	return aligned;
}

/*
 * palloc_aligned
 *		在 'context' 中分配按 'alignto' 对齐的 'size' 字节。
 *
 *
 * 目前，我们向 MemoryContext 的标准分配器函数申请
 * 额外的字节，然后把返回的地址按所需对齐取整。这
 * 意味着给定的 MemoryContext 必须能提供给我们一块
 * 大于 'size' 的内存区块。对 Slab 这类分配器来说
 * 这行不通，因为 slab 只允许创建上下文时指定大小
 * 的区块。
 *
 * 'alignto' 必须是 2 的幂。
 * 'flags' 可为 0，或与 MemoryContextAllocExtended() 相同。
 */
void *
palloc_aligned(Size size, Size alignto, int flags)
{
	return MemoryContextAllocAligned(CurrentMemoryContext, size, alignto, flags);
}

/*
 * pfree
 *		释放已分配的区块。
 */
void
pfree(void *pointer)
{
#ifdef USE_VALGRIND
	MemoryContextMethodID method = GetMemoryChunkMethodID(pointer);
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif

	MCXT_METHOD(pointer, free_p) (pointer);

#ifdef USE_VALGRIND
	if (method != MCTX_ALIGNED_REDIRECT_ID)
		VALGRIND_MEMPOOL_FREE(context, pointer);
#endif
}

/*
 * repalloc
 *		调整此前分配的区块的大小。
 */
void *
repalloc(void *pointer, Size size)
{
#ifdef USE_VALGRIND
	MemoryContextMethodID method = GetMemoryChunkMethodID(pointer);
#endif
#if defined(USE_ASSERT_CHECKING) || defined(USE_VALGRIND)
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif
	void	   *ret;

	if (!AllocSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	AssertNotInCriticalSection(context);

	/* isReset 必须已为假 */
	Assert(!context->isReset);

	ret = MCXT_METHOD(pointer, realloc) (pointer, size);
	if (unlikely(ret == NULL))
	{
		MemoryContext cxt = GetMemoryChunkContext(pointer);

		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, cxt->name)));
	}

#ifdef USE_VALGRIND
	if (method != MCTX_ALIGNED_REDIRECT_ID)
		VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);
#endif

	return ret;
}

/*
 * repalloc_extended
 *		调整此前分配的区块的大小，
 *		带 HUGE 和 NO_OOM 选项。
 */
void *
repalloc_extended(void *pointer, Size size, int flags)
{
#if defined(USE_ASSERT_CHECKING) || defined(USE_VALGRIND)
	MemoryContext context = GetMemoryChunkContext(pointer);
#endif
	void	   *ret;

	if (!((flags & MCXT_ALLOC_HUGE) != 0 ? AllocHugeSizeIsValid(size) :
		  AllocSizeIsValid(size)))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	AssertNotInCriticalSection(context);

	/* isReset 必须已为假 */
	Assert(!context->isReset);

	ret = MCXT_METHOD(pointer, realloc) (pointer, size);
	if (unlikely(ret == NULL))
	{
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			MemoryContext cxt = GetMemoryChunkContext(pointer);

			MemoryContextStats(TopMemoryContext);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on request of size %zu in memory context \"%s\".",
							   size, cxt->name)));
		}
		return NULL;
	}

	VALGRIND_MEMPOOL_CHANGE(context, pointer, ret, size);

	return ret;
}

/*
 * repalloc0
 *		调整此前分配的区块的大小，并把新增空间清零。
 *
 */
void *
repalloc0(void *pointer, Size oldsize, Size size)
{
	void	   *ret;

	/* 捕捉参数顺序错误 */
	if (unlikely(oldsize > size))
		elog(ERROR, "invalid repalloc0 call: oldsize %zu, new size %zu",
			 oldsize, size);

	ret = repalloc(pointer, size);
	memset((char *) ret + oldsize, 0, (size - oldsize));
	return ret;
}

/*
 * MemoryContextAllocHuge
 *		在指定上下文内分配（可能很大的）空间。
 *
 * 参见 MaxAllocHugeSize 处注释中的考量。
 */
void *
MemoryContextAllocHuge(MemoryContext context, Size size)
{
	void	   *ret;

	Assert(MemoryContextIsValid(context));
	AssertNotInCriticalSection(context);

	if (!AllocHugeSizeIsValid(size))
		elog(ERROR, "invalid memory alloc request size %zu", size);

	context->isReset = false;

	ret = context->methods->alloc(context, size);
	if (unlikely(ret == NULL))
	{
		MemoryContextStats(TopMemoryContext);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on request of size %zu in memory context \"%s\".",
						   size, context->name)));
	}

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

/*
 * repalloc_huge
 *		调整此前分配区块的大小，允许很大的值。先前
 *		分配不必曾是 "huge"。
 */
void *
repalloc_huge(void *pointer, Size size)
{
	/* 这个似乎不值得单独实现 */
	return repalloc_extended(pointer, size, MCXT_ALLOC_HUGE);
}

/*
 * MemoryContextStrdup
 *		类似 strdup()，但从指定上下文分配
 */
char *
MemoryContextStrdup(MemoryContext context, const char *string)
{
	char	   *nstr;
	Size		len = strlen(string) + 1;

	nstr = (char *) MemoryContextAlloc(context, len);

	memcpy(nstr, string, len);

	return nstr;
}

char *
pstrdup(const char *in)
{
	return MemoryContextStrdup(CurrentMemoryContext, in);
}

/*
 * pnstrdup
 *		类似 pstrdup()，但会向
 *		未必 null 结尾的输入串补 null。
 */
char *
pnstrdup(const char *in, Size len)
{
	char	   *out;

	len = strnlen(in, len);

	out = palloc(len + 1);
	memcpy(out, in, len);
	out[len] = '\0';

	return out;
}

/*
 * 制作字符串副本并移除全部末尾换行字符。
 */
char *
pchomp(const char *in)
{
	size_t		n;

	n = strlen(in);
	while (n > 0 && in[n - 1] == '\n')
		n--;
	return pnstrdup(in, n);
}
