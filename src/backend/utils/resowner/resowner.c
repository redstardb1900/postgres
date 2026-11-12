/*-------------------------------------------------------------------------
 *
 * resowner.c
 *	  POSTGRES 资源所有者管理代码。
 *
 * 查询生命周期资源会通过与 ResourceOwner 对象
 * 关联来跟踪。这提供了一种简单机制，可确保此类
 * 资源在恰当时机被释放。
 * 详情参见 utils/resowner/README。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/resowner/resowner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "common/hmac.h"
#include "jit/jit.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner_private.h"
#include "utils/snapmgr.h"


/*
 * 本代码管理的所有资源 ID 都必须能放入一个 Datum 中，
 * 由于它们通常是指针或整数，这不成问题。
 *
 * 为几样实际上就是 "int" 类型的东西提供 Datum 转换
 * 宏。
 */
#define FileGetDatum(file) Int32GetDatum(file)
#define DatumGetFile(datum) ((File) DatumGetInt32(datum))
#define BufferGetDatum(buffer) Int32GetDatum(buffer)
#define DatumGetBuffer(datum) ((Buffer) DatumGetInt32(datum))

/*
 * ResourceArray 是存储所有类型资源 ID 的通用结构。
 *
 * 我们用简单数组来管理较小的资源 ID 集合：
 * itemsarr[k] 保存 ID，0 <= k < nitems <= maxitems = capacity。
 *
 * 如果集合变大，我们就切换到使用开放寻址哈希。
 * 此时 itemsarr[] 成为含 "capacity" 个槽的哈希表，
 * 每个槽要么持有 ID，要么持有 "invalidval"。nitems 为有效
 * 条目数；若将超过 maxitems，则扩大数组并
 * 重新哈希。此模式下 maxitems 应明显小于 capacity，
 * 以免浪费太多时间寻找空槽。
 *
 * 在这两种模式下，lastidx 都记住最后插入或由 GetAny
 * 返回条目位置；让 ResourceArrayRemove 中搜索更快。
 */
typedef struct ResourceArray
{
	Datum	   *itemsarr;		/* 存储值的缓冲区 */
	Datum		invalidval;		/* 被视为无效的值 */
	uint32		capacity;		/* itemsarr[] 的已分配长度 */
	uint32		nitems;			/* items 数组中存储的条目数 */
	uint32		maxitems;		/* 扩容前对 nitems 的当前限制 */
	uint32		lastidx;		/* GetAny 最后返回条目的索引 */
} ResourceArray;

/*
 * ResourceArray 初始分配的大小。必须是 2 的幂，因为
 * 要用 (arraysize - 1) 作为哈希掩码。
 */
#define RESARRAY_INIT_SIZE 16

/*
 * ResourceArray 中何时切换到哈希，而非简单数组逻辑。
 */
#define RESARRAY_MAX_ARRAY 64
#define RESARRAY_IS_ARRAY(resarr) ((resarr)->capacity <= RESARRAY_MAX_ARRAY)

/*
 * 给定 capacity 的资源数组中可存储多少条目。
 * 达到该数目时就必须调整大小。
 */
#define RESARRAY_MAX_ITEMS(capacity) \
	((capacity) <= RESARRAY_MAX_ARRAY ? (capacity) : (capacity)/4 * 3)

/*
 * 为了加速从资源所有者向其父所有者批量释放或重新
 * 新分配，每个资源所有者都有一个所持锁的小缓存。
 * 锁管理器在其本地锁哈希表中也有同样的信息，如果
 * 缓存溢出时退回它，但哈希表中有很多属于其他资源
 * 所有者的锁时，遍历会更慢。
 *
 * MAX_RESOWNER_LOCKS 是每个资源所有者缓存的大小。它
 * 是根据对 pg_dump 大型 schema 的测试选定的。进行测试
 * 时（在 9.2 上），一次 pg_dump 运行中的资源所有者最多
 * 持有 9 把锁，与 schema 大小无关，但顶层资源所有者
 * 持有的锁要多得多（缓存溢出）。15 是个不错的整数，
 * 比 pg_dump 所需的稍高一些。注意，把这个数调大并非
 * 没有代价——缓存越大，逐个释放锁（资源所有者持有
 * 很多锁时）会更慢。
 */
#define MAX_RESOWNER_LOCKS 15

/*
 * ResourceOwner 对象结构如下
 */
typedef struct ResourceOwnerData
{
	ResourceOwner parent;		/* 无父时为 NULL（顶层） */
	ResourceOwner firstchild;	/* 子所有者链表的头 */
	ResourceOwner nextchild;	/* 同一父的下一个子 */
	const char *name;			/* 名称（调试用） */

	/* 我们内建支持记住以下内容： */
	ResourceArray bufferarr;	/* 缓冲区 */
	ResourceArray bufferioarr;	/* 缓冲区 IO */
	ResourceArray catrefarr;	/* catcache 引用 */
	ResourceArray catlistrefarr;	/* catcache 列表pin */
	ResourceArray relrefarr;	/* relcache 引用 */
	ResourceArray planrefarr;	/* plancache 引用 */
	ResourceArray tupdescarr;	/* tupdesc 引用 */
	ResourceArray snapshotarr;	/* 快照引用 */
	ResourceArray filearr;		/* 打开临时文件 */
	ResourceArray dsmarr;		/* 动态共享内存段 */
	ResourceArray jitarr;		/* JIT 上下文 */
	ResourceArray cryptohasharr;	/* 加密哈希上下文 */
	ResourceArray hmacarr;		/* HMAC 上下文 */

	/* 最多可记住 MAX_RESOWNER_LOCKS 个本地锁引用 */
	int			nlocks;			/* 持有的锁数量 */
	LOCALLOCK  *locks[MAX_RESOWNER_LOCKS];	/* 持有锁列表 */
}			ResourceOwnerData;


/*****************************************************************************
 *	  全局内存                 *
 *****************************************************************************/

ResourceOwner CurrentResourceOwner = NULL;
ResourceOwner CurTransactionResourceOwner = NULL;
ResourceOwner TopTransactionResourceOwner = NULL;
ResourceOwner AuxProcessResourceOwner = NULL;

/*
 * 用于资源释放的附加回调列表
 */
typedef struct ResourceReleaseCallbackItem
{
	struct ResourceReleaseCallbackItem *next;
	ResourceReleaseCallback callback;
	void	   *arg;
} ResourceReleaseCallbackItem;

static ResourceReleaseCallbackItem *ResourceRelease_callbacks = NULL;


/* 内部例程 */
static void ResourceArrayInit(ResourceArray *resarr, Datum invalidval);
static void ResourceArrayEnlarge(ResourceArray *resarr);
static void ResourceArrayAdd(ResourceArray *resarr, Datum value);
static bool ResourceArrayRemove(ResourceArray *resarr, Datum value);
static bool ResourceArrayGetAny(ResourceArray *resarr, Datum *value);
static void ResourceArrayFree(ResourceArray *resarr);
static void ResourceOwnerReleaseInternal(ResourceOwner owner,
										 ResourceReleasePhase phase,
										 bool isCommit,
										 bool isTopLevel);
static void ReleaseAuxProcessResourcesCallback(int code, Datum arg);
static void PrintRelCacheLeakWarning(Relation rel);
static void PrintPlanCacheLeakWarning(CachedPlan *plan);
static void PrintTupleDescLeakWarning(TupleDesc tupdesc);
static void PrintSnapshotLeakWarning(Snapshot snapshot);
static void PrintFileLeakWarning(File file);
static void PrintDSMLeakWarning(dsm_segment *seg);
static void PrintCryptoHashLeakWarning(Datum handle);
static void PrintHMACLeakWarning(Datum handle);


/*****************************************************************************
 *	  内部例程                    *
 *****************************************************************************/


/*
 * 初始化 ResourceArray
 */
static void
ResourceArrayInit(ResourceArray *resarr, Datum invalidval)
{
	/* 断言为空 */
	Assert(resarr->itemsarr == NULL);
	Assert(resarr->capacity == 0);
	Assert(resarr->nitems == 0);
	Assert(resarr->maxitems == 0);
	/* 记住相应的 "invalid" 值 */
	resarr->invalidval = invalidval;
	/* 在需要之前我们不分配任何存储 */
}

/*
 * 确保数组中至少还有一个资源的空间。
 *
 * 这一步与实际插入资源是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
static void
ResourceArrayEnlarge(ResourceArray *resarr)
{
	uint32		i,
				oldcap,
				newcap;
	Datum	   *olditemsarr;
	Datum	   *newitemsarr;

	if (resarr->nitems < resarr->maxitems)
		return;					/* 无需操作 */

	olditemsarr = resarr->itemsarr;
	oldcap = resarr->capacity;

	/* 将数组容量翻倍（capacity 必须保持为 2 的幂！） */
	newcap = (oldcap > 0) ? oldcap * 2 : RESARRAY_INIT_SIZE;
	newitemsarr = (Datum *) MemoryContextAlloc(TopMemoryContext,
											   newcap * sizeof(Datum));
	for (i = 0; i < newcap; i++)
		newitemsarr[i] = resarr->invalidval;

	/* 假设从此处往后不会失败，故可随意改写 resarr */
	resarr->itemsarr = newitemsarr;
	resarr->capacity = newcap;
	resarr->maxitems = RESARRAY_MAX_ITEMS(newcap);
	resarr->nitems = 0;

	if (olditemsarr != NULL)
	{
		/*
		 * 把已有条目转移到新数组；它们不一定在原处，
		 * 所以这种简单逻辑是最好的办法。注意，如果我们
		 * 之前是把该集合作为简单数组管理，那么 nitems 之
		 * 后条目就是垃圾，但这应无关紧要，因为只有
		 * 当 nitems 等于 oldcap 时我们才会到这里。
		 */
		for (i = 0; i < oldcap; i++)
		{
			if (olditemsarr[i] != resarr->invalidval)
				ResourceArrayAdd(resarr, olditemsarr[i]);
		}

		/* 然后释放旧数组 */
		pfree(olditemsarr);
	}

	Assert(resarr->nitems < resarr->maxitems);
}

/*
 * 向 ResourceArray 添加资源
 *
 * 调用者必须已经执行过 ResourceArrayEnlarge()
 */
static void
ResourceArrayAdd(ResourceArray *resarr, Datum value)
{
	uint32		idx;

	Assert(value != resarr->invalidval);
	Assert(resarr->nitems < resarr->maxitems);

	if (RESARRAY_IS_ARRAY(resarr))
	{
		/* 追加到线性数组 */
		idx = resarr->nitems;
	}
	else
	{
		/* 插入到哈希位置或其后首个空闲槽。 */
		uint32		mask = resarr->capacity - 1;

		idx = DatumGetUInt32(hash_any((void *) &value, sizeof(value))) & mask;
		for (;;)
		{
			if (resarr->itemsarr[idx] == resarr->invalidval)
				break;
			idx = (idx + 1) & mask;
		}
	}
	resarr->lastidx = idx;
	resarr->itemsarr[idx] = value;
	resarr->nitems++;
}

/*
 * 从 ResourceArray 移除资源
 *
 * 成功返回 true，未找到资源则返回 false。
 *
 * 注意：若同一资源 ID 出现多次，则移除其中一个。
 */
static bool
ResourceArrayRemove(ResourceArray *resarr, Datum value)
{
	uint32		i,
				idx,
				lastidx = resarr->lastidx;

	Assert(value != resarr->invalidval);

	/* 遍历所有条目，但先尝试 lastidx。 */
	if (RESARRAY_IS_ARRAY(resarr))
	{
		if (lastidx < resarr->nitems &&
			resarr->itemsarr[lastidx] == value)
		{
			resarr->itemsarr[lastidx] = resarr->itemsarr[resarr->nitems - 1];
			resarr->nitems--;
			/* 更新 lastidx 以使逆序移除更快。 */
			resarr->lastidx = resarr->nitems - 1;
			return true;
		}
		for (i = 0; i < resarr->nitems; i++)
		{
			if (resarr->itemsarr[i] == value)
			{
				resarr->itemsarr[i] = resarr->itemsarr[resarr->nitems - 1];
				resarr->nitems--;
				/* 更新 lastidx 以使逆序移除更快。 */
				resarr->lastidx = resarr->nitems - 1;
				return true;
			}
		}
	}
	else
	{
		uint32		mask = resarr->capacity - 1;

		if (lastidx < resarr->capacity &&
			resarr->itemsarr[lastidx] == value)
		{
			resarr->itemsarr[lastidx] = resarr->invalidval;
			resarr->nitems--;
			return true;
		}
		idx = DatumGetUInt32(hash_any((void *) &value, sizeof(value))) & mask;
		for (i = 0; i < resarr->capacity; i++)
		{
			if (resarr->itemsarr[idx] == value)
			{
				resarr->itemsarr[idx] = resarr->invalidval;
				resarr->nitems--;
				return true;
			}
			idx = (idx + 1) & mask;
		}
	}

	return false;
}

/*
 * 获取 ResourceArray 中任一条目。
 *
 * "便利"的定义是"便于 ResourceArrayRemove 移除"；
 * 我们通过相应设置 lastidx 来促成，从而避免在销毁
 * ResourceOwner 时移除全部 ResourceArray 条目的 O(N^2) 开销。
 *
 * 如果找到元素则返回 true，数组为空则返回 false。
 */
static bool
ResourceArrayGetAny(ResourceArray *resarr, Datum *value)
{
	if (resarr->nitems == 0)
		return false;

	if (RESARRAY_IS_ARRAY(resarr))
	{
		/* 线性数组：直接返回第一个元素 */
		resarr->lastidx = 0;
	}
	else
	{
		/* 哈希：从上次所在位置向后搜索。 */
		uint32		mask = resarr->capacity - 1;

		for (;;)
		{
			resarr->lastidx &= mask;
			if (resarr->itemsarr[resarr->lastidx] != resarr->invalidval)
				break;
			resarr->lastidx++;
		}
	}

	*value = resarr->itemsarr[resarr->lastidx];
	return true;
}

/*
 * 废弃一个 ResourceArray（之后不关心其状态）
 */
static void
ResourceArrayFree(ResourceArray *resarr)
{
	if (resarr->itemsarr)
		pfree(resarr->itemsarr);
}


/*****************************************************************************
 *	  导出例程                    *
 *****************************************************************************/


/*
 * ResourceOwnerCreate
 *		创建空的 ResourceOwner。
 *
 * 所有 ResourceOwner 对象都保存在 TopMemoryContext 中，因为
 * 只能被显式释放。
 */
ResourceOwner
ResourceOwnerCreate(ResourceOwner parent, const char *name)
{
	ResourceOwner owner;

	owner = (ResourceOwner) MemoryContextAllocZero(TopMemoryContext,
												   sizeof(ResourceOwnerData));
	owner->name = name;

	if (parent)
	{
		owner->parent = parent;
		owner->nextchild = parent->firstchild;
		parent->firstchild = owner;
	}

	ResourceArrayInit(&(owner->bufferarr), BufferGetDatum(InvalidBuffer));
	ResourceArrayInit(&(owner->bufferioarr), BufferGetDatum(InvalidBuffer));
	ResourceArrayInit(&(owner->catrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->catlistrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->relrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->planrefarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->tupdescarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->snapshotarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->filearr), FileGetDatum(-1));
	ResourceArrayInit(&(owner->dsmarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->jitarr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->cryptohasharr), PointerGetDatum(NULL));
	ResourceArrayInit(&(owner->hmacarr), PointerGetDatum(NULL));

	return owner;
}

/*
 * ResourceOwnerRelease
 *		释放一个 ResourceOwner 及其后代所拥有的全部资源，
 *		但不删除所有者对象本身。
 *
 * 注意，这仅执行释放的一个阶段，因此通常必须调
 * 用三次。这是因为：(a) 我们想对每个阶段
 * 分别进行递归，从而保持所需的操作顺序；以及
 * (b) xact.c 在各阶段之间可能还有其他操作
 * 要执行。
 *
 * phase: 要执行的释放阶段
 * isCommit: 查询或事务成功完成时为 true，
 *			否则为 false
 * isTopLevel: 完成主事务时为 true，否则为 false
 *
 * 传递 isCommit 是因为某些模块可能期望：如果事务或
 * portal 正常结束，其资源就都已被释放。若是这样，
 * 存在未释放资源时给警告（而非错误）是合理的。
 * 当 isCommit 为 false 时，此类警告
 * 通常是不合适的。
 *
 * 传递 isTopLevel 是在我们于主事务结束时释放
 * TopTransactionResourceOwner 时传入。这通常意味着*所有*
 * 资源都将被释放，因此我们可以稍微优化一下。
 */
void
ResourceOwnerRelease(ResourceOwner owner,
					 ResourceReleasePhase phase,
					 bool isCommit,
					 bool isTopLevel)
{
	/* 目前递归之前不需要任何设置 */
	ResourceOwnerReleaseInternal(owner, phase, isCommit, isTopLevel);
}

static void
ResourceOwnerReleaseInternal(ResourceOwner owner,
							 ResourceReleasePhase phase,
							 bool isCommit,
							 bool isTopLevel)
{
	ResourceOwner child;
	ResourceOwner save;
	ResourceReleaseCallbackItem *item;
	ResourceReleaseCallbackItem *next;
	Datum		foundres;

	/* 递归以处理后代 */
	for (child = owner->firstchild; child != NULL; child = child->nextchild)
		ResourceOwnerReleaseInternal(child, phase, isCommit, isTopLevel);

	/*
	 * 让 CurrentResourceOwner 指向我，使 ReleaseBuffer 等函数
	 * 不致混淆
	 */
	save = CurrentResourceOwner;
	CurrentResourceOwner = owner;

	if (phase == RESOURCE_RELEASE_BEFORE_LOCKS)
	{
		/*
		 * 中止失败的缓冲区 IO。AbortBufferIO()->TerminateBufferIO()
		 * 调用 ResourceOwnerForgetBufferIO()，只需迭代到没有
		 * 为止。
		 *
		 * 必须放在释放缓冲区 pin 之前。
		 *
		 * 提交期间不应有任何进行中的 IO。
		 */
		while (ResourceArrayGetAny(&(owner->bufferioarr), &foundres))
		{
			Buffer		res = DatumGetBuffer(foundres);

			if (isCommit)
				elog(PANIC, "lost track of buffer IO on buffer %d", res);
			AbortBufferIO(res);
		}

		/*
		 * 释放缓冲区 pin。注意 ReleaseBuffer 会把缓冲区从
		 * 数组中移除，因此我们只需迭代到没有
		 * 为止。
		 *
		 * 提交期间不应留有任何 pin —— 那将意味着
		 * 执行器未被正确清理 —— 因此要发出警告。
		 * 中止的情况下只需安静地清理。
		 */
		while (ResourceArrayGetAny(&(owner->bufferarr), &foundres))
		{
			Buffer		res = DatumGetBuffer(foundres);

			if (isCommit)
				PrintBufferLeakWarning(res);
			ReleaseBuffer(res);
		}

		/* relcache 引用同理 */
		while (ResourceArrayGetAny(&(owner->relrefarr), &foundres))
		{
			Relation	res = (Relation) DatumGetPointer(foundres);

			if (isCommit)
				PrintRelCacheLeakWarning(res);
			RelationClose(res);
		}

		/* 动态共享内存段同理 */
		while (ResourceArrayGetAny(&(owner->dsmarr), &foundres))
		{
			dsm_segment *res = (dsm_segment *) DatumGetPointer(foundres);

			if (isCommit)
				PrintDSMLeakWarning(res);
			dsm_detach(res);
		}

		/* JIT 上下文同理 */
		while (ResourceArrayGetAny(&(owner->jitarr), &foundres))
		{
			JitContext *context = (JitContext *) DatumGetPointer(foundres);

			jit_release_context(context);
		}

		/* 加密哈希上下文同理 */
		while (ResourceArrayGetAny(&(owner->cryptohasharr), &foundres))
		{
			pg_cryptohash_ctx *context =
				(pg_cryptohash_ctx *) DatumGetPointer(foundres);

			if (isCommit)
				PrintCryptoHashLeakWarning(foundres);
			pg_cryptohash_free(context);
		}

		/* HMAC 上下文同理 */
		while (ResourceArrayGetAny(&(owner->hmacarr), &foundres))
		{
			pg_hmac_ctx *context = (pg_hmac_ctx *) DatumGetPointer(foundres);

			if (isCommit)
				PrintHMACLeakWarning(foundres);
			pg_hmac_free(context);
		}
	}
	else if (phase == RESOURCE_RELEASE_LOCKS)
	{
		if (isTopLevel)
		{
			/*
			 * 顶层事务将释放所有锁（或至少所有非会话
			 * 话锁），因此只需在递归的顶部做一次 lmgr
			 * 调用。
			 */
			if (owner == TopTransactionResourceOwner)
			{
				ProcReleaseLocks(isCommit);
				ReleasePredicateLocks(isCommit, false);
			}
		}
		else
		{
			/*
			 * 逐个释放锁。注意，若提交的是子事务，
			 * 我们还不会释放其锁，而是把它们转移到
			 * 父所有者。
			 */
			LOCALLOCK **locks;
			int			nlocks;

			Assert(owner->parent != NULL);

			/*
			 * 把此资源所有者的锁列表传给锁管理器，除非
			 * 其缓存已溢出。
			 */
			if (owner->nlocks > MAX_RESOWNER_LOCKS)
			{
				locks = NULL;
				nlocks = 0;
			}
			else
			{
				locks = owner->locks;
				nlocks = owner->nlocks;
			}

			if (isCommit)
				LockReassignCurrentOwner(locks, nlocks);
			else
				LockReleaseCurrentOwner(locks, nlocks);
		}
	}
	else if (phase == RESOURCE_RELEASE_AFTER_LOCKS)
	{
		/*
		 * 释放 catcache 引用。ReleaseCatCache 会把 catref 条目
		 * 从我们的数组中移除，所以我们只需迭代到没有
		 * 为止。
		 *
		 * 与缓冲区 pin 一样，提交时有残留则警告。
		 */
		while (ResourceArrayGetAny(&(owner->catrefarr), &foundres))
		{
			HeapTuple	res = (HeapTuple) DatumGetPointer(foundres);

			if (isCommit)
				PrintCatCacheLeakWarning(res);
			ReleaseCatCache(res);
		}

		/* catcache 列表同理 */
		while (ResourceArrayGetAny(&(owner->catlistrefarr), &foundres))
		{
			CatCList   *res = (CatCList *) DatumGetPointer(foundres);

			if (isCommit)
				PrintCatCacheListLeakWarning(res);
			ReleaseCatCacheList(res);
		}

		/* plancache 引用同理 */
		while (ResourceArrayGetAny(&(owner->planrefarr), &foundres))
		{
			CachedPlan *res = (CachedPlan *) DatumGetPointer(foundres);

			if (isCommit)
				PrintPlanCacheLeakWarning(res);
			ReleaseCachedPlan(res, owner);
		}

		/* tupdesc 引用同理 */
		while (ResourceArrayGetAny(&(owner->tupdescarr), &foundres))
		{
			TupleDesc	res = (TupleDesc) DatumGetPointer(foundres);

			if (isCommit)
				PrintTupleDescLeakWarning(res);
			DecrTupleDescRefCount(res);
		}

		/* 快照引用同理 */
		while (ResourceArrayGetAny(&(owner->snapshotarr), &foundres))
		{
			Snapshot	res = (Snapshot) DatumGetPointer(foundres);

			if (isCommit)
				PrintSnapshotLeakWarning(res);
			UnregisterSnapshot(res);
		}

		/* 临时文件同理 */
		while (ResourceArrayGetAny(&(owner->filearr), &foundres))
		{
			File		res = DatumGetFile(foundres);

			if (isCommit)
				PrintFileLeakWarning(res);
			FileClose(res);
		}
	}

	/* 让附加模块也有机会 */
	for (item = ResourceRelease_callbacks; item; item = next)
	{
		/* 允许回调在被调用时注销自身 */
		next = item->next;
		item->callback(phase, isCommit, isTopLevel, item->arg);
	}

	CurrentResourceOwner = save;
}

/*
 * ResourceOwnerReleaseAllPlanCacheRefs
 *		释放此所有者持有的 plancache 引用（仅此）。
 *
 * 我们最终可能会为其他资源类型添加类似的函数，
 * 但目前只需要这一个。
 */
void
ResourceOwnerReleaseAllPlanCacheRefs(ResourceOwner owner)
{
	Datum		foundres;

	while (ResourceArrayGetAny(&(owner->planrefarr), &foundres))
	{
		CachedPlan *res = (CachedPlan *) DatumGetPointer(foundres);

		ReleaseCachedPlan(res, owner);
	}
}

/*
 * ResourceOwnerDelete
 *		删除一个所有者对象及其后代。
 *
 * 调用者必须已经释放了对象树中的所有资源。
 */
void
ResourceOwnerDelete(ResourceOwner owner)
{
	/* 我们最好不要删除 CurrentResourceOwner... */
	Assert(owner != CurrentResourceOwner);

	/* 而且它最好也不拥有任何资源 */
	Assert(owner->bufferarr.nitems == 0);
	Assert(owner->bufferioarr.nitems == 0);
	Assert(owner->catrefarr.nitems == 0);
	Assert(owner->catlistrefarr.nitems == 0);
	Assert(owner->relrefarr.nitems == 0);
	Assert(owner->planrefarr.nitems == 0);
	Assert(owner->tupdescarr.nitems == 0);
	Assert(owner->snapshotarr.nitems == 0);
	Assert(owner->filearr.nitems == 0);
	Assert(owner->dsmarr.nitems == 0);
	Assert(owner->jitarr.nitems == 0);
	Assert(owner->cryptohasharr.nitems == 0);
	Assert(owner->hmacarr.nitems == 0);
	Assert(owner->nlocks == 0 || owner->nlocks == MAX_RESOWNER_LOCKS + 1);

	/*
	 * 删除子所有者。递归调用会把子所有者与我解除链
	 * 接，尚有子所有者则迭代。
	 */
	while (owner->firstchild != NULL)
		ResourceOwnerDelete(owner->firstchild);

	/*
	 * 在删除所有者之前先将其与父所有者解除链接，
	 * 这样出错时就不会有已删除/损坏的所有者仍
	 * 挂在树上。泄漏总比崩溃好。
	 */
	ResourceOwnerNewParent(owner, NULL);

	/* 然后释放对象 */
	ResourceArrayFree(&(owner->bufferarr));
	ResourceArrayFree(&(owner->bufferioarr));
	ResourceArrayFree(&(owner->catrefarr));
	ResourceArrayFree(&(owner->catlistrefarr));
	ResourceArrayFree(&(owner->relrefarr));
	ResourceArrayFree(&(owner->planrefarr));
	ResourceArrayFree(&(owner->tupdescarr));
	ResourceArrayFree(&(owner->snapshotarr));
	ResourceArrayFree(&(owner->filearr));
	ResourceArrayFree(&(owner->dsmarr));
	ResourceArrayFree(&(owner->jitarr));
	ResourceArrayFree(&(owner->cryptohasharr));
	ResourceArrayFree(&(owner->hmacarr));

	pfree(owner);
}

/*
 * 获取 ResourceOwner 的父（顶层所有者则返回 NULL）
 */
ResourceOwner
ResourceOwnerGetParent(ResourceOwner owner)
{
	return owner->parent;
}

/*
 * 重新指定 ResourceOwner 的父所有者
 */
void
ResourceOwnerNewParent(ResourceOwner owner,
					   ResourceOwner newparent)
{
	ResourceOwner oldparent = owner->parent;

	if (oldparent)
	{
		if (owner == oldparent->firstchild)
			oldparent->firstchild = owner->nextchild;
		else
		{
			ResourceOwner child;

			for (child = oldparent->firstchild; child; child = child->nextchild)
			{
				if (owner == child->nextchild)
				{
					child->nextchild = owner->nextchild;
					break;
				}
			}
		}
	}

	if (newparent)
	{
		Assert(owner != newparent);
		owner->parent = newparent;
		owner->nextchild = newparent->firstchild;
		newparent->firstchild = owner;
	}
	else
	{
		owner->parent = NULL;
		owner->nextchild = NULL;
	}
}

/*
 * 注册或注销用于资源清理的回调函数
 *
 * 这些函数供动态加载的模块使用。
 * 对于内建模块，我们通常直接硬编码相应的调用。
 *
 * 注意回调发生在提交后或中止后，因此回调函数只
 * 能执行非关键的清理。
 */
void
RegisterResourceReleaseCallback(ResourceReleaseCallback callback, void *arg)
{
	ResourceReleaseCallbackItem *item;

	item = (ResourceReleaseCallbackItem *)
		MemoryContextAlloc(TopMemoryContext,
						   sizeof(ResourceReleaseCallbackItem));
	item->callback = callback;
	item->arg = arg;
	item->next = ResourceRelease_callbacks;
	ResourceRelease_callbacks = item;
}

void
UnregisterResourceReleaseCallback(ResourceReleaseCallback callback, void *arg)
{
	ResourceReleaseCallbackItem *item;
	ResourceReleaseCallbackItem *prev;

	prev = NULL;
	for (item = ResourceRelease_callbacks; item; prev = item, item = item->next)
	{
		if (item->callback == callback && item->arg == arg)
		{
			if (prev)
				prev->next = item->next;
			else
				ResourceRelease_callbacks = item->next;
			pfree(item);
			break;
		}
	}
}

/*
 * 为当前进程建立一个 AuxProcessResourceOwner。
 */
void
CreateAuxProcessResourceOwner(void)
{
	Assert(AuxProcessResourceOwner == NULL);
	Assert(CurrentResourceOwner == NULL);
	AuxProcessResourceOwner = ResourceOwnerCreate(NULL, "AuxiliaryProcess");
	CurrentResourceOwner = AuxProcessResourceOwner;

	/*
	 * 注册一个 shmem-exit 回调来清理辅助进程的资源所
	 * 有者（需在例如 ShutdownXLOG 之后运行）。
	 */
	on_shmem_exit(ReleaseAuxProcessResourcesCallback, 0);
}

/*
 * 便捷例程：释放 AuxProcessResourceOwner 跟踪的
 * 全部资源（但此处不销毁该 resowner）。如果
 * 为 true 则对泄漏资源发警告。
 */
void
ReleaseAuxProcessResources(bool isCommit)
{
	/*
	 * 就目前而言，真正可能被释放的只有缓冲区 pin；
	 * 但我们不妨还是执行完整的释放协议。
	 */
	ResourceOwnerRelease(AuxProcessResourceOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 isCommit, true);
	ResourceOwnerRelease(AuxProcessResourceOwner,
						 RESOURCE_RELEASE_LOCKS,
						 isCommit, true);
	ResourceOwnerRelease(AuxProcessResourceOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 isCommit, true);
}

/*
 * 同用途的 shmem-exit 回调。
 * 进程退出码为零（即正常）时对泄漏资源发出警告。
 */
static void
ReleaseAuxProcessResourcesCallback(int code, Datum arg)
{
	bool		isCommit = (code == 0);

	ReleaseAuxProcessResources(isCommit);
}


/*
 * 确保 ResourceOwner 的缓冲区数组中还有至少一个
 * 空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeBuffers(ResourceOwner owner)
{
	/* 以前允许无 resowner 时 pin 缓冲区，现在不行 */
	Assert(owner != NULL);
	ResourceArrayEnlarge(&(owner->bufferarr));
}

/*
 * 记住缓冲区 pin 由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeBuffers()
 */
void
ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceArrayAdd(&(owner->bufferarr), BufferGetDatum(buffer));
}

/*
 * 忘掉缓冲区 pin 由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer)
{
	if (!ResourceArrayRemove(&(owner->bufferarr), BufferGetDatum(buffer)))
		elog(ERROR, "buffer %d is not owned by resource owner %s",
			 buffer, owner->name);
}


/*
 * 确保 ResourceOwner 的缓冲区数组中还有至少一个
 * 空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeBufferIOs(ResourceOwner owner)
{
	/* 以前允许无 resowner 时 pin 缓冲区，现在不行 */
	Assert(owner != NULL);
	ResourceArrayEnlarge(&(owner->bufferioarr));
}

/*
 * 记住缓冲区 IO 由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeBufferIOs()
 */
void
ResourceOwnerRememberBufferIO(ResourceOwner owner, Buffer buffer)
{
	ResourceArrayAdd(&(owner->bufferioarr), BufferGetDatum(buffer));
}

/*
 * 忘掉缓冲区 IO 由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetBufferIO(ResourceOwner owner, Buffer buffer)
{
	if (!ResourceArrayRemove(&(owner->bufferioarr), BufferGetDatum(buffer)))
		elog(PANIC, "buffer IO %d is not owned by resource owner %s",
			 buffer, owner->name);
}

/*
 * 记住本地锁由 ResourceOwner 拥有
 *
 * 这与其他 Remember 函数不同：锁列表只是一个有损
 * 缓存。它最多可容纳 MAX_RESOWNER_LOCKS 个条目，当溢出
 * 时我们就停止跟踪锁。只记住最多 MAX_RESOWNER_LOCKS 个
 * 条目的意义在于：当持有大量锁时，
 * ResourceOwnerForgetLock 无需扫描大数组来找到该
 * 条目。
 */
void
ResourceOwnerRememberLock(ResourceOwner owner, LOCALLOCK *locallock)
{
	Assert(locallock != NULL);

	if (owner->nlocks > MAX_RESOWNER_LOCKS)
		return;					/* 我们已经溢出了 */

	if (owner->nlocks < MAX_RESOWNER_LOCKS)
		owner->locks[owner->nlocks] = locallock;
	else
	{
		/* 已溢出 */
	}
	owner->nlocks++;
}

/*
 * 忘掉本地锁由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetLock(ResourceOwner owner, LOCALLOCK *locallock)
{
	int			i;

	if (owner->nlocks > MAX_RESOWNER_LOCKS)
		return;					/* 我们已溢出 */

	Assert(owner->nlocks > 0);
	for (i = owner->nlocks - 1; i >= 0; i--)
	{
		if (locallock == owner->locks[i])
		{
			owner->locks[i] = owner->locks[owner->nlocks - 1];
			owner->nlocks--;
			return;
		}
	}
	elog(ERROR, "lock reference %p is not owned by resource owner %s",
		 locallock, owner->name);
}

/*
 * 确保 ResourceOwner 的 catcache 引用数组中还有至少一个
 * 空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeCatCacheRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->catrefarr));
}

/*
 * 记住 catcache 引用由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeCatCacheRefs()
 */
void
ResourceOwnerRememberCatCacheRef(ResourceOwner owner, HeapTuple tuple)
{
	ResourceArrayAdd(&(owner->catrefarr), PointerGetDatum(tuple));
}

/*
 * 忘掉 catcache 引用由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetCatCacheRef(ResourceOwner owner, HeapTuple tuple)
{
	if (!ResourceArrayRemove(&(owner->catrefarr), PointerGetDatum(tuple)))
		elog(ERROR, "catcache reference %p is not owned by resource owner %s",
			 tuple, owner->name);
}

/*
 * 确保 ResourceOwner 的 catcache 列表引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeCatCacheListRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->catlistrefarr));
}

/*
 * 记住 catcache 列表引用由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeCatCacheListRefs()
 */
void
ResourceOwnerRememberCatCacheListRef(ResourceOwner owner, CatCList *list)
{
	ResourceArrayAdd(&(owner->catlistrefarr), PointerGetDatum(list));
}

/*
 * 忘掉 catcache 列表引用由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetCatCacheListRef(ResourceOwner owner, CatCList *list)
{
	if (!ResourceArrayRemove(&(owner->catlistrefarr), PointerGetDatum(list)))
		elog(ERROR, "catcache list reference %p is not owned by resource owner %s",
			 list, owner->name);
}

/*
 * 确保 ResourceOwner 的 relcache 引用数组中还有至少一个
 * 空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeRelationRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->relrefarr));
}

/*
 * 记住 relcache 引用由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeRelationRefs()
 */
void
ResourceOwnerRememberRelationRef(ResourceOwner owner, Relation rel)
{
	ResourceArrayAdd(&(owner->relrefarr), PointerGetDatum(rel));
}

/*
 * 忘掉 relcache 引用由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetRelationRef(ResourceOwner owner, Relation rel)
{
	if (!ResourceArrayRemove(&(owner->relrefarr), PointerGetDatum(rel)))
		elog(ERROR, "relcache reference %s is not owned by resource owner %s",
			 RelationGetRelationName(rel), owner->name);
}

/*
 * 调试子例程
 */
static void
PrintRelCacheLeakWarning(Relation rel)
{
	elog(WARNING, "relcache reference leak: relation \"%s\" not closed",
		 RelationGetRelationName(rel));
}

/*
 * 确保 ResourceOwner 的 plancache 引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargePlanCacheRefs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->planrefarr));
}

/*
 * 记住 plancache 引用由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargePlanCacheRefs()
 */
void
ResourceOwnerRememberPlanCacheRef(ResourceOwner owner, CachedPlan *plan)
{
	ResourceArrayAdd(&(owner->planrefarr), PointerGetDatum(plan));
}

/*
 * 忘掉 plancache 引用由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetPlanCacheRef(ResourceOwner owner, CachedPlan *plan)
{
	if (!ResourceArrayRemove(&(owner->planrefarr), PointerGetDatum(plan)))
		elog(ERROR, "plancache reference %p is not owned by resource owner %s",
			 plan, owner->name);
}

/*
 * 调试子例程
 */
static void
PrintPlanCacheLeakWarning(CachedPlan *plan)
{
	elog(WARNING, "plancache reference leak: plan %p not closed", plan);
}

/*
 * 确保 ResourceOwner 的 tupdesc 引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeTupleDescs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->tupdescarr));
}

/*
 * 记住 tupdesc 引用由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeTupleDescs()
 */
void
ResourceOwnerRememberTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	ResourceArrayAdd(&(owner->tupdescarr), PointerGetDatum(tupdesc));
}

/*
 * 忘掉 tupdesc 引用由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetTupleDesc(ResourceOwner owner, TupleDesc tupdesc)
{
	if (!ResourceArrayRemove(&(owner->tupdescarr), PointerGetDatum(tupdesc)))
		elog(ERROR, "tupdesc reference %p is not owned by resource owner %s",
			 tupdesc, owner->name);
}

/*
 * 调试子例程
 */
static void
PrintTupleDescLeakWarning(TupleDesc tupdesc)
{
	elog(WARNING,
		 "TupleDesc reference leak: TupleDesc %p (%u,%d) still referenced",
		 tupdesc, tupdesc->tdtypeid, tupdesc->tdtypmod);
}

/*
 * 确保 ResourceOwner 的快照引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeSnapshots(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->snapshotarr));
}

/*
 * 记住快照引用由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeSnapshots()
 */
void
ResourceOwnerRememberSnapshot(ResourceOwner owner, Snapshot snapshot)
{
	ResourceArrayAdd(&(owner->snapshotarr), PointerGetDatum(snapshot));
}

/*
 * 忘掉快照引用由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetSnapshot(ResourceOwner owner, Snapshot snapshot)
{
	if (!ResourceArrayRemove(&(owner->snapshotarr), PointerGetDatum(snapshot)))
		elog(ERROR, "snapshot reference %p is not owned by resource owner %s",
			 snapshot, owner->name);
}

/*
 * 调试子例程
 */
static void
PrintSnapshotLeakWarning(Snapshot snapshot)
{
	elog(WARNING, "Snapshot reference leak: Snapshot %p still referenced",
		 snapshot);
}


/*
 * 确保 ResourceOwner 的文件引用数组中还有
 * 一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeFiles(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->filearr));
}

/*
 * 记住临时文件由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeFiles()
 */
void
ResourceOwnerRememberFile(ResourceOwner owner, File file)
{
	ResourceArrayAdd(&(owner->filearr), FileGetDatum(file));
}

/*
 * 忘掉临时文件由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetFile(ResourceOwner owner, File file)
{
	if (!ResourceArrayRemove(&(owner->filearr), FileGetDatum(file)))
		elog(ERROR, "temporary file %d is not owned by resource owner %s",
			 file, owner->name);
}

/*
 * 调试子例程
 */
static void
PrintFileLeakWarning(File file)
{
	elog(WARNING, "temporary file leak: File %d still referenced",
		 file);
}

/*
 * 确保 ResourceOwner 的动态共享内存段引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽
 * 了内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeDSMs(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->dsmarr));
}

/*
 * 记住动态共享内存段由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeDSMs()
 */
void
ResourceOwnerRememberDSM(ResourceOwner owner, dsm_segment *seg)
{
	ResourceArrayAdd(&(owner->dsmarr), PointerGetDatum(seg));
}

/*
 * 忘掉动态共享内存段由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetDSM(ResourceOwner owner, dsm_segment *seg)
{
	if (!ResourceArrayRemove(&(owner->dsmarr), PointerGetDatum(seg)))
		elog(ERROR, "dynamic shared memory segment %u is not owned by resource owner %s",
			 dsm_segment_handle(seg), owner->name);
}

/*
 * 调试子例程
 */
static void
PrintDSMLeakWarning(dsm_segment *seg)
{
	elog(WARNING, "dynamic shared memory leak: segment %u still referenced",
		 dsm_segment_handle(seg));
}

/*
 * 确保 ResourceOwner 的 JIT 上下文引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽了
 * 内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeJIT(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->jitarr));
}

/*
 * 记住 JIT 上下文由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeJIT()
 */
void
ResourceOwnerRememberJIT(ResourceOwner owner, Datum handle)
{
	ResourceArrayAdd(&(owner->jitarr), handle);
}

/*
 * 忘掉 JIT 上下文由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetJIT(ResourceOwner owner, Datum handle)
{
	if (!ResourceArrayRemove(&(owner->jitarr), handle))
		elog(ERROR, "JIT context %p is not owned by resource owner %s",
			 DatumGetPointer(handle), owner->name);
}

/*
 * 确保 ResourceOwner 的加密哈希上下文引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽了
 * 内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeCryptoHash(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->cryptohasharr));
}

/*
 * 记住加密哈希上下文由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeCryptoHash()
 */
void
ResourceOwnerRememberCryptoHash(ResourceOwner owner, Datum handle)
{
	ResourceArrayAdd(&(owner->cryptohasharr), handle);
}

/*
 * 忘掉加密哈希上下文由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetCryptoHash(ResourceOwner owner, Datum handle)
{
	if (!ResourceArrayRemove(&(owner->cryptohasharr), handle))
		elog(ERROR, "cryptohash context %p is not owned by resource owner %s",
			 DatumGetPointer(handle), owner->name);
}

/*
 * 调试子例程
 */
static void
PrintCryptoHashLeakWarning(Datum handle)
{
	elog(WARNING, "cryptohash context reference leak: context %p still referenced",
		 DatumGetPointer(handle));
}

/*
 * 确保 ResourceOwner 的 HMAC 上下文引用数组中还有
 * 至少一个空位。
 *
 * 这一步与实际插入条目是分开的，因为如果我们耗尽了
 * 内存，在获取资源*之前*这样做是至关重要的。
 */
void
ResourceOwnerEnlargeHMAC(ResourceOwner owner)
{
	ResourceArrayEnlarge(&(owner->hmacarr));
}

/*
 * 记住 HMAC 上下文由 ResourceOwner 拥有
 *
 * 调用者必须已经执行过 ResourceOwnerEnlargeHMAC()
 */
void
ResourceOwnerRememberHMAC(ResourceOwner owner, Datum handle)
{
	ResourceArrayAdd(&(owner->hmacarr), handle);
}

/*
 * 忘掉 HMAC 上下文由 ResourceOwner 拥有
 */
void
ResourceOwnerForgetHMAC(ResourceOwner owner, Datum handle)
{
	if (!ResourceArrayRemove(&(owner->hmacarr), handle))
		elog(ERROR, "HMAC context %p is not owned by resource owner %s",
			 DatumGetPointer(handle), owner->name);
}

/*
 * 调试子例程
 */
static void
PrintHMACLeakWarning(Datum handle)
{
	elog(WARNING, "HMAC context reference leak: context %p still referenced",
		 DatumGetPointer(handle));
}
