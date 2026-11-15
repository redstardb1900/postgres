/*-------------------------------------------------------------------------
 *
 * inval.c
 *	  POSTGRES 缓存失效分发代码。
 *
 *	这是很微妙的东西，请注意：
 *
 *	当元组被更新或删除时，我们的标准可见性规则
 *	认为它*仍然有效*，只要我们还在同一命令内，即直到
 *	下一个 CommandCounterIncrement() 或事务提交。
 *	（参见 access/heap/heapam_visibility.c，并注意系统目录会
 *  通常以当前可用的最新快照扫描，而非以事务快照
 *  扫描）。在命令边界处，旧元组停止有效
 *	而新版本（若有）则变为有效，
 *	因此无法在 heap_update() 期间就刷新系统缓存中的元组，
 *	或 heap_delete() 时进行。该元组此时仍然有效；而且，
 *	即便我们真的刷掉了它，它也可能被同一命令中稍后的
 *	请求重新加载进缓存。所以正确做法是保留一个列表
 *	记下过时（更新/删除）元组，再在下一命令边界
 *	执行所需的缓存刷新。还须跟踪插入的元组，
 *	以便能刷新与新元组匹配的"负"缓存项；同样，这件事
 *	也必须等到命令结束才能做。
 *
 *	命令完成后，仍需记住被插入的元组（含被更新元组
 *	的新版本），以便中止事务时能将其从缓存中刷掉。
 *	类似地，我们最好还能刷掉那些
 *	可能已替代被删除元组而载入的"负"缓存项，
 *	所以仍然需要保留被删除的那些元组。
 *
 *	若事务成功完成，就必须把所有这些失效事件广播给
 *	其他后端（经 SI 消息队列），使它们能刷掉自己
 *	缓存中的过期项。注意，必须在发送 SI 消息之前
 *	记下事务提交，否则其他后端不会视我们更新过的
 *	元组为有效。
 *
 *	子事务中止时，可以处理并丢弃其排队的
 *	事件。当子事务提交时，我们只需把其事件加
 *	入父事务的待处理列表中。
 *
 *	简言之，事务结束前我们须记住系统缓存中可能
 *	存在的元组的每次插入或删除。简单起见，更新算作
 *	两个事件（删除 + 插入）。（若更新未改变元组的
 *	哈希值，catcache.c 会把这一对优化成单个事件。）
 *
 *	我们不需要以这种方式登记每一个元组操作，只需登记
 *	那些带有关联 catcache 的关系中的元组。不过，我们
 *	必须登记*可能*进入 catcache 的每个元组的操作，
 *	不管它当前是否在我们的缓存中。另外，若元组
 *	的关系有多个 catcache，我们就需要为每个这样的
 *	catcache 登记一条失效消息。catcache.c 的
 *	PrepareToInvalidateCacheTuple() 例程知道对于给定的
 *	元组可能需要使哪些 catcache 失效。
 *
 *	另外，每当我们看到对 pg_class、pg_attribute 或
 *	pg_index 元组的操作时，就按 CacheInvalidateHeapTuple()
 *	规定为该元组所描述的关系登记 relcache 刷新操作。
 *	对 pg_constraint 元组（关系上的外键）亦然。
 *
 *	我们把 relcache 刷新请求存放在与 catcache 元组刷新
 *	请求分开的列表中。这样能先发出全部待处理的
 *	catcache 刷新，然后再发出 relcache 刷新，从而避免
 *	在加载 relcache 时载入某 catcache 元组却马上刷掉。
 *	另外，我们避免为同一关系排队多条
 *	relcache 刷新，因为其开销相对较大。
 *	（XXX 是否也值得测试重复的 catcache 刷新项？多半
 *	不值）
 *
 *	许多子系统拥有依赖 relcache 和/或 catcache 的更高层
 *	缓存，它们在此注册回调以使其缓存失效。
 *	在构建更高级缓存项时，后端可能收到针对构建中
 *	条目或其依赖项的回调。这意味着新的更高级条目
 *	生来陈旧，且可能在整个后端生命期内保持陈旧，
 *	许多缓存并不防止这一点。它们依赖这样的 DDL：
 *	对不容错过的目录变更，于合适对象取得
 *	AccessExclusiveLock。（用更少锁做出的变更，后端
 *	可能永远读不到该变更。）但关系缓存需要在下一
 *	事务开始之前就反映出 CREATE INDEX CONCURRENTLY 带
 *	来的变更。因此，当相关失效回调在构建期间到
 *	达时，relcache.c 会重新尝试该构建。有类似需求
 *	的缓存同样可以这样做。
 *
 *	如果对某个我们从 relcache 初始化文件预加载的系统
 *	关系发出 relcache 刷新，那么我们也必须删除该初始化
 *	文件，以便其在下次后端重启时被重建。操作初始化
 *	文件的实际工作在 relcache.c 中，但我们在这里跟踪
 *	这种需求。
 *
 *	目前，失效消息的发送并未考虑目录元组描述的对象
 *	可能是会话本地对象（如临时表）的可能性。原因是
 *	（1）此代码无法切实区分，（2）无法确定其他
 *	后端是否就没有这类表的目录缓存甚至 relcache 项；
 *	反正没有什么能阻止这一点。或许就值得在未
 *	来尝试避免发送这类失效消息流量，前提是那
 *	些问题能够被廉价地
 *	克服。
 *
 *	当 wal_level=logical 时，在每个命令结束处把失效写入 WAL，
 *	以支持进行中事务的解码。参见
 *	CommandEndInvalidationMessages.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/inval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/pg_constraint.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/catcache.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * 待处理请求以就绪待发的 SharedInvalidationMessages 存储。
 * 消息本身保存在 TopTransactionContext 中的数组内
 * （catcache 消息与 relcache 消息各有独立数组）。控制
 * 信息则保存在 TransInvalidationInfo 结构体链中，同样
 * 分配在 TopTransactionContext 中。（我们本可以把子
 * 事务的 TransInvalidationInfo 保存在其
 * CurTransactionContext 中；但这反而更浪费而非更省，
 * 多数场景下它只是子事务唯一的分配。）
 *
 * 消息数组可以存储得很紧凑，同时又不必在数组内部
 * 移动数据，因为在任一子事务内我们只需区分先前
 * 命令所发的消息与当前命令所发的消息。一旦命令完
 * 成并对其消息完成本地处理后，就可以仅通过
 * 修改 TransInvalidationInfo 结构体中的数组下标，把
 * 那些消息并入先前命令的消息。类似地，我们只需
 * 在子事务完成之前区分先前子事务与当前子事务
 * 的消息，直到子事务完成为止；此后我们调整
 * 父事务 TransInvalidationInfo 中的数组下标，
 * 使之包含子事务的消息。
 *
 * 各消息在命令或子事务输出中的次序不重要，
 * 虽然本实现恰好会保留它们入队时的
 * 顺序。
 * （早期版本的代码并不保留该顺序。）
 *
 * 为了书写方便，控制信息保存在二元数组中：第一个
 * 元素对应 catcache 消息，第二个元素对应 relcache
 * 消息。
 */
#define CatCacheMsgs 0
#define RelCacheMsgs 1

/* TopTransactionContext 中主数组的指针 */
typedef struct InvalMessageArray
{
	SharedInvalidationMessage *msgs;	/* palloc 数组（可扩展） */
	int			maxmsgs;		/* 数组当前已分配的大小 */
} InvalMessageArray;

static InvalMessageArray InvalMessageArrays[2];

/* 单个逻辑消息组的控制信息 */
typedef struct InvalidationMsgsGroup
{
	int			firstmsg[2];	/* 相关数组中的首下标 */
	int			nextmsg[2];		/* 末+1 下标 */
} InvalidationMsgsGroup;

/* 帮助维护 InvalidationMsgsGroup 抽象的宏 */
#define SetSubGroupToFollow(targetgroup, priorgroup, subgroup) \
	do { \
		(targetgroup)->firstmsg[subgroup] = \
			(targetgroup)->nextmsg[subgroup] = \
			(priorgroup)->nextmsg[subgroup]; \
	} while (0)

#define SetGroupToFollow(targetgroup, priorgroup) \
	do { \
		SetSubGroupToFollow(targetgroup, priorgroup, CatCacheMsgs); \
		SetSubGroupToFollow(targetgroup, priorgroup, RelCacheMsgs); \
	} while (0)

#define NumMessagesInSubGroup(group, subgroup) \
	((group)->nextmsg[subgroup] - (group)->firstmsg[subgroup])

#define NumMessagesInGroup(group) \
	(NumMessagesInSubGroup(group, CatCacheMsgs) + \
	 NumMessagesInSubGroup(group, RelCacheMsgs))


/*----------------
 * 失效消息被划分为两个组：
 *	1) 当前命令至今的事件，尚未反映到缓存。
 *	2) 当前事务先前命令的事件；这些事件已反映到
 *	   本地缓存，在提交或中止事务时必须要么广播
 *	   给其他后端，要么从本地缓存撤销。嵌套事务
 *	   各层级都需要。
 * 以便我们能丢弃已中止子事务的事件。当
 * 子事务提交时，我们将其事件追加到父事务
 * 的组中。
 *
 * relcache-file-invalidated 标志用简单布尔值即可，
 * 因为只在事务提交时才处理它；至于事务中哪条
 * 命令设置它并不关心。
 *----------------
 */

typedef struct TransInvalidationInfo
{
	/* 指回父事务信息的后向链接 */
	struct TransInvalidationInfo *parent;

	/* 子事务嵌套深度 */
	int			my_level;

	/* 当前命令发出的事件 */
	InvalidationMsgsGroup CurrentCmdInvalidMsgs;

	/* 本（子）事务先前命令发出的事件 */
	InvalidationMsgsGroup PriorCmdInvalidMsgs;

	/* 初始化文件须失效？ */
	bool		RelcacheInitFileInval;
} TransInvalidationInfo;

static TransInvalidationInfo *transInvalInfo = NULL;

/* GUC 存储 */
int			debug_discard_caches = 0;

/*
 * 动态注册的回调函数。当前实现假定数量不致多到
 * 需要动态可调整数组的地步；如果有需要，改进起来
 * 也很容易。
 *
 * 为了避免在 CallSyscacheCallbacks 中查找，给定
 * syscache 的全部回调都链接入一链表。
 * 链头为 syscache_callback_links[id]，值为列表下标+1，无则0。
 */

#define MAX_SYSCACHE_CALLBACKS 64
#define MAX_RELCACHE_CALLBACKS 10

static struct SYSCACHECALLBACK
{
	int16		id;				/* 缓存编号 */
	int16		link;			/* 同一缓存下一回调下标+1 */
	SyscacheCallbackFunction function;
	Datum		arg;
}			syscache_callback_list[MAX_SYSCACHE_CALLBACKS];

static int16 syscache_callback_links[SysCacheSize];

static int	syscache_callback_count = 0;

static struct RELCACHECALLBACK
{
	RelcacheCallbackFunction function;
	Datum		arg;
}			relcache_callback_list[MAX_RELCACHE_CALLBACKS];

static int	relcache_callback_count = 0;

/* ----------------------------------------------------------------
 *				失效子组支持函数
 * ----------------------------------------------------------------
 */

/*
 * AddInvalidationMessage
 *		向（子）组添加失效消息。
 *
 * 该组必须是最后活跃组，因为我们假定能直接加到
 * 相关 InvalMessageArray 的末尾。
 *
 * subgroup 须为 CatCacheMsgs/RelCacheMsgs。
 */
static void
AddInvalidationMessage(InvalidationMsgsGroup *group, int subgroup,
					   const SharedInvalidationMessage *msg)
{
	InvalMessageArray *ima = &InvalMessageArrays[subgroup];
	int			nextindex = group->nextmsg[subgroup];

	if (nextindex >= ima->maxmsgs)
	{
		if (ima->msgs == NULL)
		{
			/* 在 TopTransactionContext 中建存储数组 */
			int			reqsize = 32;	/* 任意值 */

			ima->msgs = (SharedInvalidationMessage *)
				MemoryContextAlloc(TopTransactionContext,
								   reqsize * sizeof(SharedInvalidationMessage));
			ima->maxmsgs = reqsize;
			Assert(nextindex == 0);
		}
		else
		{
			/* 扩大存储数组 */
			int			reqsize = 2 * ima->maxmsgs;

			ima->msgs = (SharedInvalidationMessage *)
				repalloc(ima->msgs,
						 reqsize * sizeof(SharedInvalidationMessage));
			ima->maxmsgs = reqsize;
		}
	}
	/* 好，将消息加入当前组 */
	ima->msgs[nextindex] = *msg;
	group->nextmsg[subgroup]++;
}

/*
 * 将一个失效消息子组追加到另一个子组，源子组被
 * 重置为空。
 */
static void
AppendInvalidationMessageSubGroup(InvalidationMsgsGroup *dest,
								  InvalidationMsgsGroup *src,
								  int subgroup)
{
	/* 消息在主数组中必须相邻 */
	Assert(dest->nextmsg[subgroup] == src->firstmsg[subgroup]);

	/* 这让事情变简单： */
	dest->nextmsg[subgroup] = src->nextmsg[subgroup];

	/*
	 * 这对某些调用者方便，对另一些则无关紧要。但我们
	 * 总是这么做，理由是让不同组指向消息数组的同一片
	 * 区域并不好。
	 */
	SetSubGroupToFollow(src, dest, subgroup);
}

/*
 * 处理一组失效消息子组。
 *
 * 这是一个宏，它对消息子组中的每条消息执行给定的
 * 代码片段。片段中应以 *msg 引用该消息。
 */
#define ProcessMessageSubGroup(group, subgroup, codeFragment) \
	do { \
		int		_msgindex = (group)->firstmsg[subgroup]; \
		int		_endmsg = (group)->nextmsg[subgroup]; \
		for (; _msgindex < _endmsg; _msgindex++) \
		{ \
			SharedInvalidationMessage *msg = \
				&InvalMessageArrays[subgroup].msgs[_msgindex]; \
			codeFragment; \
		} \
	} while (0)

/*
 * 把一组失效消息子组当作数组来处理。
 *
 * 同上，但代码片段可处理整个消息数组。
 * 片段中应以 msgs[] 引用这些消息，共 n 条。
 */
#define ProcessMessageSubGroupMulti(group, subgroup, codeFragment) \
	do { \
		int		n = NumMessagesInSubGroup(group, subgroup); \
		if (n > 0) { \
			SharedInvalidationMessage *msgs = \
				&InvalMessageArrays[subgroup].msgs[(group)->firstmsg[subgroup]]; \
			codeFragment; \
		} \
	} while (0)


/* ----------------------------------------------------------------
 *				失效组支持函数
 *
 * 这些例程了解逻辑失效组会被划分为针对 catcache 与
 * relcache 项的多个物理数组这一情况。
 * ----------------------------------------------------------------
 */

/*
 * 加 catcache 失效项
 */
static void
AddCatcacheInvalidationMessage(InvalidationMsgsGroup *group,
							   int id, uint32 hashValue, Oid dbId)
{
	SharedInvalidationMessage msg;

	Assert(id < CHAR_MAX);
	msg.cc.id = (int8) id;
	msg.cc.dbId = dbId;
	msg.cc.hashValue = hashValue;

	/*
	 * 需要在 SharedInvalidationMessage 结构体中定义填充
	 * 字节，否则会被多个进程访问的 sinvaladt.c 环形缓冲
	 * 区会因使用未定义内存触发 valgrind 虚假
	 * 警告。这是因为 valgrind 记住的是上一本地进
	 * 程存储中的未定义字节，并没有意识到另一进程
	 * 此后已经写入并填满了先前未初始化的字节
	 * bytes
	 */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, CatCacheMsgs, &msg);
}

/*
 * 添加整目录失效项
 */
static void
AddCatalogInvalidationMessage(InvalidationMsgsGroup *group,
							  Oid dbId, Oid catId)
{
	SharedInvalidationMessage msg;

	msg.cat.id = SHAREDINVALCATALOG_ID;
	msg.cat.dbId = dbId;
	msg.cat.catId = catId;
	/* 含义参见 AddCatcacheInvalidationMessage() */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, CatCacheMsgs, &msg);
}

/*
 * 加 relcache 失效项
 */
static void
AddRelcacheInvalidationMessage(InvalidationMsgsGroup *group,
							   Oid dbId, Oid relId)
{
	SharedInvalidationMessage msg;

	/*
	 * 不要添加重复项。假定 dbId 无须检查，因为它绝不
	 * 会改变。relId 为 InvalidOid 代表所有关系，因此当
	 * 它存在时无需逐个添加。
	 */
	ProcessMessageSubGroup(group, RelCacheMsgs,
						   if (msg->rc.id == SHAREDINVALRELCACHE_ID &&
							   (msg->rc.relId == relId ||
								msg->rc.relId == InvalidOid))
						   return);

	/* 加入该项 */
	msg.rc.id = SHAREDINVALRELCACHE_ID;
	msg.rc.dbId = dbId;
	msg.rc.relId = relId;
	/* 含义参见 AddCatcacheInvalidationMessage() */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, RelCacheMsgs, &msg);
}

/*
 * 添加快照失效项
 *
 * 为简单起见将其放入 relcache 子组。
 */
static void
AddSnapshotInvalidationMessage(InvalidationMsgsGroup *group,
							   Oid dbId, Oid relId)
{
	SharedInvalidationMessage msg;

	/* 不要添加重复项 */
	/* 我们假定 dbId 无须检查，因为它永远不会改变 */
	ProcessMessageSubGroup(group, RelCacheMsgs,
						   if (msg->sn.id == SHAREDINVALSNAPSHOT_ID &&
							   msg->sn.relId == relId)
						   return);

	/* 加入该项 */
	msg.sn.id = SHAREDINVALSNAPSHOT_ID;
	msg.sn.dbId = dbId;
	msg.sn.relId = relId;
	/* 含义参见 AddCatcacheInvalidationMessage() */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	AddInvalidationMessage(group, RelCacheMsgs, &msg);
}

/*
 * 将一个失效消息组追加到另一个组，源组被重置
 * 为空。
 */
static void
AppendInvalidationMessages(InvalidationMsgsGroup *dest,
						   InvalidationMsgsGroup *src)
{
	AppendInvalidationMessageSubGroup(dest, src, CatCacheMsgs);
	AppendInvalidationMessageSubGroup(dest, src, RelCacheMsgs);
}

/*
 * 对失效组中的所有消息执行给定的函数。
 * 该组不被改动。
 *
 * catcache 项先被处理，原因如前文所述。
 */
static void
ProcessInvalidationMessages(InvalidationMsgsGroup *group,
							void (*func) (SharedInvalidationMessage *msg))
{
	ProcessMessageSubGroup(group, CatCacheMsgs, func(msg));
	ProcessMessageSubGroup(group, RelCacheMsgs, func(msg));
}

/*
 * 同上，但该函数能处理整个消息数组，
 * 而非一次一条。
 */
static void
ProcessInvalidationMessagesMulti(InvalidationMsgsGroup *group,
								 void (*func) (const SharedInvalidationMessage *msgs, int n))
{
	ProcessMessageSubGroupMulti(group, CatCacheMsgs, func(msgs, n));
	ProcessMessageSubGroupMulti(group, RelCacheMsgs, func(msgs, n));
}

/* ----------------------------------------------------------------
 *					  私有支持函数
 * ----------------------------------------------------------------
 */

/*
 * RegisterCatcacheInvalidation
 *
 * 为一个 catcache 元组项登记失效事件。
 */
static void
RegisterCatcacheInvalidation(int cacheId,
							 uint32 hashValue,
							 Oid dbId)
{
	AddCatcacheInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								   cacheId, hashValue, dbId);
}

/*
 * RegisterCatalogInvalidation
 *
 * 为来自某个目录的所有 catcache 项登记失效事件。
 */
static void
RegisterCatalogInvalidation(Oid dbId, Oid catId)
{
	AddCatalogInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								  dbId, catId);
}

/*
 * RegisterRelcacheInvalidation
 *
 * 同上，但登记的是 relcache 失效事件。
 */
static void
RegisterRelcacheInvalidation(Oid dbId, Oid relId)
{
	AddRelcacheInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								   dbId, relId);

	/*
	 * 多数时候 relcache 失效伴随系统目录更新，但也有
	 * 少数例外。这里用一个小技巧，让下一次
	 * CommandCounterIncrement() 认为我们需要执行
	 * CommandEndInvalidationMessages()。
	 */
	(void) GetCurrentCommandId(true);

	/*
	 * 如果被失效的关系是缓存在 relcache 初始化文件中的
	 * 关系之一，就标记需在提交时清除该文件。为简单起
	 * 见，针对特定数据库的失效总是同时使共享文件也失
	 * 效。整个 relcache 失效时也照此删除。
	 */
	if (relId == InvalidOid || RelationIdIsInInitFile(relId))
		transInvalInfo->RelcacheInitFileInval = true;
}

/*
 * RegisterSnapshotInvalidation
 *
 * 为针对给定目录的 MVCC 扫描登记失效事件。
 * 只有没有 catcache 的目录才需要。
 */
static void
RegisterSnapshotInvalidation(Oid dbId, Oid relId)
{
	AddSnapshotInvalidationMessage(&transInvalInfo->CurrentCmdInvalidMsgs,
								   dbId, relId);
}

/*
 * LocalExecuteInvalidationMessage
 *
 * 处理单条失效消息（可以是任何类型）。
 * 只刷新本地缓存；此函数不会把消息传给
 * 其他后端。
 */
void
LocalExecuteInvalidationMessage(SharedInvalidationMessage *msg)
{
	if (msg->id >= 0)
	{
		if (msg->cc.dbId == MyDatabaseId || msg->cc.dbId == InvalidOid)
		{
			InvalidateCatalogSnapshot();

			SysCacheInvalidate(msg->cc.id, msg->cc.hashValue);

			CallSyscacheCallbacks(msg->cc.id, msg->cc.hashValue);
		}
	}
	else if (msg->id == SHAREDINVALCATALOG_ID)
	{
		if (msg->cat.dbId == MyDatabaseId || msg->cat.dbId == InvalidOid)
		{
			InvalidateCatalogSnapshot();

			CatalogCacheFlushCatalog(msg->cat.catId);

			/* CatalogCacheFlushCatalog 按需调用 CallSyscacheCallbacks */
		}
	}
	else if (msg->id == SHAREDINVALRELCACHE_ID)
	{
		if (msg->rc.dbId == MyDatabaseId || msg->rc.dbId == InvalidOid)
		{
			int			i;

			if (msg->rc.relId == InvalidOid)
				RelationCacheInvalidate(false);
			else
				RelationCacheInvalidateEntry(msg->rc.relId);

			for (i = 0; i < relcache_callback_count; i++)
			{
				struct RELCACHECALLBACK *ccitem = relcache_callback_list + i;

				ccitem->function(ccitem->arg, msg->rc.relId);
			}
		}
	}
	else if (msg->id == SHAREDINVALSMGR_ID)
	{
		/*
		 * 可能拥有其他数据库关系的 smgr 项，所以这里无
		 * 法做短路测试。
		 */
		RelFileLocatorBackend rlocator;

		rlocator.locator = msg->sm.rlocator;
		rlocator.backend = (msg->sm.backend_hi << 16) | (int) msg->sm.backend_lo;
		smgrcloserellocator(rlocator);
	}
	else if (msg->id == SHAREDINVALRELMAP_ID)
	{
		/* 只关心本数据库和共享目录 */
		if (msg->rm.dbId == InvalidOid)
			RelationMapInvalidate(true);
		else if (msg->rm.dbId == MyDatabaseId)
			RelationMapInvalidate(false);
	}
	else if (msg->id == SHAREDINVALSNAPSHOT_ID)
	{
		/* 只关心本数据库和共享目录 */
		if (msg->sn.dbId == InvalidOid)
			InvalidateCatalogSnapshot();
		else if (msg->sn.dbId == MyDatabaseId)
			InvalidateCatalogSnapshot();
	}
	else
		elog(FATAL, "unrecognized SI message ID: %d", msg->id);
}

/*
 *		InvalidateSystemCaches
 *
 *		清空系统目录缓存里全部元组、全部缓存的
 *		关系描述符和 smgr 缓存项。
 *		引用计数为正的关系描述符随后会被重建。
 *
 *		收到共享失效队列溢出信号时调用它，因为
 *		这表明我们丢了一些共享失效消息，因而不知道
 *		需要失效什么。
 */
void
InvalidateSystemCaches(void)
{
	InvalidateSystemCachesExtended(false);
}

void
InvalidateSystemCachesExtended(bool debug_discard)
{
	int			i;

	InvalidateCatalogSnapshot();
	ResetCatalogCachesExt(debug_discard);
	RelationCacheInvalidate(debug_discard); /* 也取 smgr 和 relmap */

	for (i = 0; i < syscache_callback_count; i++)
	{
		struct SYSCACHECALLBACK *ccitem = syscache_callback_list + i;

		ccitem->function(ccitem->arg, ccitem->id, 0);
	}

	for (i = 0; i < relcache_callback_count; i++)
	{
		struct RELCACHECALLBACK *ccitem = relcache_callback_list + i;

		ccitem->function(ccitem->arg, InvalidOid);
	}
}


/* ----------------------------------------------------------------
 *					  公开函数
 * ----------------------------------------------------------------
 */

/*
 * AcceptInvalidationMessages
 *		从共享失效消息队列读取并处理失效
 *		消息。
 *
 * Note:
 *		应在处理事务时作为第一步被调用。
 */
void
AcceptInvalidationMessages(void)
{
	ReceiveSharedInvalidMessages(LocalExecuteInvalidationMessage,
								 InvalidateSystemCaches);

	/*----------
	 * 测试代码：一有可能刷新就强制刷新缓存。
	 *
	 * 这有助于检测一类间歇性故障：先读缓存项、再执
	 * 行可能使该项失效、却很少真会如此的动作。这
	 * 能发现一些否则只有在并发 DDL 时机不巧时才
	 * 会出现（例如）。
	 *
	 * 默认的 debug_discard_caches = 0 不做强制缓存刷新。
	 *
	 * 配合 CLOBBER_FREED_MEMORY 时，
	 * debug_discard_caches = 1（旧称 CLOBBER_CACHE_ALWAYS）
	 * 提供相当彻底的检验，可证明系统无缓存刷新隐患。
	 * 不过，它也会让系统慢得难以置信 --- 回归测试约需
	 * 正常时间的 100 倍左右。
	 *
	 * 如果你想自讨苦吃，试试
	 * debug_discard_caches = 3（旧称 CLOBBER_CACHE_RECURSIVELY）。
	 * 这至少会拖慢 10000 倍，所以我不会建议以此跑完整
	 * 个回归测试。跑几个简单测试是有用的，可确认缓存
	 * 重载不受内部刷新隐患影响；但在做了几千次
	 * 递归重载之后，你多半就学不到
	 * 更多东西了。
	 *----------
	 */
#ifdef DISCARD_CACHES_ENABLED
	{
		static int	recursion_depth = 0;

		if (recursion_depth < debug_discard_caches)
		{
			recursion_depth++;
			InvalidateSystemCachesExtended(true);
			recursion_depth--;
		}
	}
#endif
}

/*
 * PrepareInvalidationState
 *		为当前（子）事务初始化失效数据。
 */
static void
PrepareInvalidationState(void)
{
	TransInvalidationInfo *myInfo;

	if (transInvalInfo != NULL &&
		transInvalInfo->my_level == GetCurrentTransactionNestLevel())
		return;

	myInfo = (TransInvalidationInfo *)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(TransInvalidationInfo));
	myInfo->parent = transInvalInfo;
	myInfo->my_level = GetCurrentTransactionNestLevel();

	/* 现在，前一个栈项存在吗？ */
	if (transInvalInfo != NULL)
	{
		/* 存在；这一项应对应更深的嵌套层级。 */
		Assert(myInfo->my_level > transInvalInfo->my_level);

		/*
		 * 父（子）事务不得还有任何当前（即尚未本地
		 * 处理过的）消息。若有，就会产生语义问题：
		 * 新子事务按理还不应能看到那些事件，但由于
		 * CommandCounter 是线性的，子事务一旦推进
		 * 计数器就行不通了。在这里检查这一点很方
		 * 便，而且对保持消息数组管理的简单性也很
		 * 重要。
		 */
		if (NumMessagesInGroup(&transInvalInfo->CurrentCmdInvalidMsgs) != 0)
			elog(ERROR, "cannot start a subtransaction when there are unprocessed inval messages");

		/*
		 * MemoryContextAllocZero 把每个组的 firstmsg 与
		 * nextmsg 都设为 0，这对第一个（子）事务没问题，但
		 * 否则就须更新它们以跟随数组已有内容。
		 */
		SetGroupToFollow(&myInfo->PriorCmdInvalidMsgs,
						 &transInvalInfo->CurrentCmdInvalidMsgs);
		SetGroupToFollow(&myInfo->CurrentCmdInvalidMsgs,
						 &myInfo->PriorCmdInvalidMsgs);
	}
	else
	{
		/*
		 * 这里只需清除上一个事务遗留下来的数组
		 * 的指针。
		 */
		InvalMessageArrays[CatCacheMsgs].msgs = NULL;
		InvalMessageArrays[CatCacheMsgs].maxmsgs = 0;
		InvalMessageArrays[RelCacheMsgs].msgs = NULL;
		InvalMessageArrays[RelCacheMsgs].maxmsgs = 0;
	}

	transInvalInfo = myInfo;
}

/*
 * PostPrepare_Inval
 *		在 PREPARE 成功后清理。
 *
 * 这里我们要装作事务已中止的样子，从而撤销它所做
 * 出的全部 syscache 修改，从而与外部世界重新同步；
 * 外部世界还不认为该事务已提交。
 *
 * 若预备事务之后中止，则再无事情要做；若它提交，
 * 我们将和其他所有人一样收到随之而来的失效
 * 消息。
 */
void
PostPrepare_Inval(void)
{
	AtEOXact_Inval(false);
}

/*
 * xactGetCommittedInvalidationMessages() 由
 * RecordTransactionCommit() 调用，用于收集要加入提交
 * 记录的失效消息。只适用于提交记录，绝不适用于
 * 中止记录。必须总是在 AtEOXact_Inval() 之前运行，
 * 因其会删除所需数据。
 *
 * 记住它运行于正式提交之前，所以绝不能在这里做
 * 任何可能改变下述情况的事情：万一我们在这一步
 * 与真正提交之间失败。
 *
 * 另见 xact_redo_commit() 与 xact_desc_commit()
 */
int
xactGetCommittedInvalidationMessages(SharedInvalidationMessage **msgs,
									 bool *RelcacheInitFileInval)
{
	SharedInvalidationMessage *msgarray;
	int			nummsgs;
	int			nmsgs;

	/* 若尚未对失效消息做过任何处理则快速退出。 */
	if (transInvalInfo == NULL)
	{
		*RelcacheInitFileInval = false;
		*msgs = NULL;
		return 0;
	}

	/* 必须位于栈顶 */
	Assert(transInvalInfo->my_level == 1 && transInvalInfo->parent == NULL);

	/*
	 * relcache 初始化文件失效需要在发送 SI 消息之前和
	 * 之后都进行处理。不过，除非我们提交了事务，否则
	 * 无需做。
	 */
	*RelcacheInitFileInval = transInvalInfo->RelcacheInitFileInval;

	/*
	 * 将全部待处理消息收集为一个连续失效消息数组，
	 * 简化构建提交 WAL 消息期间要做的事。维持它们
	 * 原本会被 AtEOXact_Inval() 处理的顺序，确保重
	 * 放中的模拟行为尽可能接近原始。我们要同样的
	 * bug，
	 * 非新 bug。
	 */
	nummsgs = NumMessagesInGroup(&transInvalInfo->PriorCmdInvalidMsgs) +
		NumMessagesInGroup(&transInvalInfo->CurrentCmdInvalidMsgs);

	*msgs = msgarray = (SharedInvalidationMessage *)
		MemoryContextAlloc(CurTransactionContext,
						   nummsgs * sizeof(SharedInvalidationMessage));

	nmsgs = 0;
	ProcessMessageSubGroupMulti(&transInvalInfo->PriorCmdInvalidMsgs,
								CatCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	ProcessMessageSubGroupMulti(&transInvalInfo->CurrentCmdInvalidMsgs,
								CatCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	ProcessMessageSubGroupMulti(&transInvalInfo->PriorCmdInvalidMsgs,
								RelCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	ProcessMessageSubGroupMulti(&transInvalInfo->CurrentCmdInvalidMsgs,
								RelCacheMsgs,
								(memcpy(msgarray + nmsgs,
										msgs,
										n * sizeof(SharedInvalidationMessage)),
								 nmsgs += n));
	Assert(nmsgs == nummsgs);

	return nmsgs;
}

/*
 * ProcessCommittedInvalidationMessages 由
 * xact_redo_commit() 或 standby_redo() 执行以处理失效消息，
 * 仅在事务末。
 *
 * relcache 初始化文件失效需在 SI 消息之前和
 * 之后都进行处理（参见 AtEOXact_Inval()）
 */
void
ProcessCommittedInvalidationMessages(SharedInvalidationMessage *msgs,
									 int nmsgs, bool RelcacheInitFileInval,
									 Oid dbid, Oid tsid)
{
	if (nmsgs <= 0)
		return;

	elog(trace_recovery(DEBUG4), "replaying commit with %d messages%s", nmsgs,
		 (RelcacheInitFileInval ? " and relcache file invalidation" : ""));

	if (RelcacheInitFileInval)
	{
		elog(trace_recovery(DEBUG4), "removing relcache init files for database %u",
			 dbid);

		/*
		 * RelationCacheInitFilePreInvalidate 在失效消息针对
		 * 特定数据库时需要已设 DatabasePath，但恢复期间
		 * 不能用 SetDatabasePath，它仅供普通后端用一
		 * 次。因此用快捷手段：直接设置 DatabasePath，用
		 * 完后再取消设置。
		 */
		if (OidIsValid(dbid))
			DatabasePath = GetDatabasePath(dbid, tsid);

		RelationCacheInitFilePreInvalidate();

		if (OidIsValid(dbid))
		{
			pfree(DatabasePath);
			DatabasePath = NULL;
		}
	}

	SendSharedInvalidMessages(msgs, nmsgs);

	if (RelcacheInitFileInval)
		RelationCacheInitFilePostInvalidate();
}

/*
 * AtEOXact_Inval
 *		在主事务结束时处理排队的失效消息。
 *
 * 若 isCommit 为真，我们必须把 PriorCmdInvalidMsgs 列表
 * 里的消息发往共享失效消息队列。注意这些消息不仅
 * 会被其他后端读取，也会被本后端在下一次事
 * 务开始时（经 AcceptInvalidationMessages）读取。这
 * 意味着我们可以跳过对仍在 CurrentCmdInvalidMsgs 中
 * 的即时本地处理，直接把该列表发出。
 *
 * 若 isCommit 为假，我们正在中止，必须在本地
 * 处理 PriorCmdInvalidMsgs 中的消息。不需要向其他
 * 后端发送任何消息，因为它们反正看不到我们改的
 * 元组。CurrentCmdInvalidMsgs 也可忘掉，因其改动尚未
 * 触及缓存。
 *
 * 总之要把状态重置为空。这里无需物理释放内
 * 存，因为 TopTransactionContext 马上就要被清空了。
 * anyway.
 *
 * Note:
 *		应在处理事务时作为最后一步被调用。
 */
void
AtEOXact_Inval(bool isCommit)
{
	/* 无消息则快速退出 */
	if (transInvalInfo == NULL)
		return;

	/* 必须位于栈顶 */
	Assert(transInvalInfo->my_level == 1 && transInvalInfo->parent == NULL);

	if (isCommit)
	{
		/*
		 * relcache 初始化文件失效需要在发送 SI 消息之前和
		 * 之后都要处理。不过除非我们已提交，否则不必
		 * 做任何事。
		 */
		if (transInvalInfo->RelcacheInitFileInval)
			RelationCacheInitFilePreInvalidate();

		AppendInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
								   &transInvalInfo->CurrentCmdInvalidMsgs);

		ProcessInvalidationMessagesMulti(&transInvalInfo->PriorCmdInvalidMsgs,
										 SendSharedInvalidMessages);

		if (transInvalInfo->RelcacheInitFileInval)
			RelationCacheInitFilePostInvalidate();
	}
	else
	{
		ProcessInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
									LocalExecuteInvalidationMessage);
	}

	/* 无需显式释放任何东西 */
	transInvalInfo = NULL;
}

/*
 * AtEOSubXact_Inval
 *		在子事务结束时处理排队的失效消息。
 *
 * 若 isCommit 为真，处理 CurrentCmdInvalidMsgs（如
 * 果有——多半没有），则把这两个列表都挂到父事务
 * 的 PriorCmdInvalidMsgs 上。
 *
 * 若 isCommit 为假，我们正在中止，必须在本地
 * 处理 PriorCmdInvalidMsgs。无消息需发往其他后端。
 * 我们也可以忘掉 CurrentCmdInvalidMsgs，因为这些改动
 * 尚未触及缓存。
 *
 * 无论如何，都要弹出事务栈。这里无需物理释放内存，
 * 因为 CurTransactionContext 反正马上就要被清空
 * （若中止）。不过要当心同一嵌套层级被中止两
 * 次的可能性。
 */
void
AtEOSubXact_Inval(bool isCommit)
{
	int			my_level;
	TransInvalidationInfo *myInfo = transInvalInfo;

	/* 无消息则速退。 */
	if (myInfo == NULL)
		return;

	/* 若消息不属于本层级也要快速退出。 */
	my_level = GetCurrentTransactionNestLevel();
	if (myInfo->my_level != my_level)
	{
		Assert(myInfo->my_level < my_level);
		return;
	}

	if (isCommit)
	{
		/* 若 CurrentCmdInvalidMsgs 还有内容则修正它 */
		CommandEndInvalidationMessages();

		/*
		 * 失效栈项是懒惰创建的，所以父事务可能还没有
		 * 栈项。与其新建、搬数据再释放自身，不如直接
		 * 调整自身这一项的层级即可。
		 * entry.
		 */
		if (myInfo->parent == NULL || myInfo->parent->my_level < my_level - 1)
		{
			myInfo->my_level--;
			return;
		}

		/*
		 * 把我的失效消息上交父事务。注意我们把它们放进
		 * PriorCmdInvalidMsgs 而不是 CurrentCmdInvalidMsgs，因
		 * 为它们已被本地处理过。（若父事务的
		 * CurrentCmdInvalidMsgs 非空，就会触发
		 * AppendInvalidationMessageSubGroup 的 Assert；本检查
		 * 此前已做过）
		 */
		AppendInvalidationMessages(&myInfo->parent->PriorCmdInvalidMsgs,
								   &myInfo->PriorCmdInvalidMsgs);

		/* 须重新调整父事务 CurrentCmdInvalidMsgs 下标 */
		SetGroupToFollow(&myInfo->parent->CurrentCmdInvalidMsgs,
						 &myInfo->parent->PriorCmdInvalidMsgs);

		/* 待处理的 relcache 失效也归父事务管了 */
		if (myInfo->RelcacheInitFileInval)
			myInfo->parent->RelcacheInitFileInval = true;

		/* 弹出事务状态栈 */
		transInvalInfo = myInfo->parent;

		/* 其余内容无需显式释放 */
		pfree(myInfo);
	}
	else
	{
		ProcessInvalidationMessages(&myInfo->PriorCmdInvalidMsgs,
									LocalExecuteInvalidationMessage);

		/* 弹出事务状态栈 */
		transInvalInfo = myInfo->parent;

		/* 其余内容无需显式释放 */
		pfree(myInfo);
	}
}

/*
 * CommandEndInvalidationMessages
 *		在事务中一条命令结束时处理排队的失效
 *		消息。
 *
 * 这里我们不会向共享队列发送任何消息，因为尚不知
 * 道自己是否会提交。但确实需要在本地处理
 * CurrentCmdInvalidMsgs 列表，以便把当前命令中已被我
 * 们过时化的项从缓存里刷掉。然后把当前命令列表并
 * 归入先前列表。
 *
 * Note:
 *		应在 CommandCounterIncrement() 中调用，
 *		且在推进命令 ID 之后。
 */
void
CommandEndInvalidationMessages(void)
{
	/*
	 * 你可能认为不该在事务之外调用它，但 bootstrap 会这
	 * 样做，不在事务中时发出的 ABORT 也会这样做。所以
	 * 无可处理状态就悄悄返回。
	 */
	if (transInvalInfo == NULL)
		return;

	ProcessInvalidationMessages(&transInvalInfo->CurrentCmdInvalidMsgs,
								LocalExecuteInvalidationMessage);

	/* 为 wal_level=logical 记 WAL 记录每命令失效消息 */
	if (XLogLogicalInfoActive())
		LogLogicalInvalidations();

	AppendInvalidationMessages(&transInvalInfo->PriorCmdInvalidMsgs,
							   &transInvalInfo->CurrentCmdInvalidMsgs);
}


/*
 * CacheInvalidateHeapTuple
 *		把给定元组登记为在命令结束时失效
 *		（即当前命令正在创建或淘汰该元组）。
 *		同时检测是否隐含 relcache 失效。
 *
 * 对于插入或删除，tuple 是目标元组，newtuple 为 NULL。
 * 对于更新，我们只被调用一次，tuple 为旧元组版本，
 * newtuple 为新版本。这样就避免了更新期间的重复
 * 劳动。
 */
void
CacheInvalidateHeapTuple(Relation relation,
						 HeapTuple tuple,
						 HeapTuple newtuple)
{
	Oid			tupleRelId;
	Oid			databaseId;
	Oid			relationId;

	/* bootstrap 中不做事 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * 我们只关心系统目录里元组的失效；用户关系元组
	 * 从不进入 catcache，也不能影响
	 * relcache。
	 */
	if (!IsCatalogRelation(relation))
		return;

	/*
	 * IsCatalogRelation() 对系统目录的 TOAST 表也会返回
	 * 真，但我们同样不关心它们。
	 */
	if (IsToastRelation(relation))
		return;

	/*
	 * 若该层级尚未准备好排队失效消息，现在就先
	 * 准备就绪。
	 */
	PrepareInvalidationState();

	/*
	 * 先让 catcache 干它该干的活
	 */
	tupleRelId = RelationGetRelid(relation);
	if (RelationInvalidatesSnapshotsOnly(tupleRelId))
	{
		databaseId = IsSharedRelation(tupleRelId) ? InvalidOid : MyDatabaseId;
		RegisterSnapshotInvalidation(databaseId, tupleRelId);
	}
	else
		PrepareToInvalidateCacheTuple(relation, tuple, newtuple,
									  RegisterCatcacheInvalidation);

	/*
	 * 现在，该元组是不是某个 relcache 项的主要定义者？
	 * 深入解释见文件头注释。
	 *
	 * 注意此处忽略 newtuple；我们假定更新无法把元组
	 * 从一个 relcache 项挪为另一个项的组成部分。
	 */
	if (tupleRelId == RelationRelationId)
	{
		Form_pg_class classtup = (Form_pg_class) GETSTRUCT(tuple);

		relationId = classtup->oid;
		if (classtup->relisshared)
			databaseId = InvalidOid;
		else
			databaseId = MyDatabaseId;
	}
	else if (tupleRelId == AttributeRelationId)
	{
		Form_pg_attribute atttup = (Form_pg_attribute) GETSTRUCT(tuple);

		relationId = atttup->attrelid;

		/*
		 * KLUGE 警告：我们总是用 MyDatabaseId 发送 relcache
		 * 事件，即使涉事关系是共享的（不易分辨）。
		 * 这实际上意味着只有同一数据库中的后端才会
		 * 响应 relcache 刷新。这确实合适，因为只有
		 * 它们能看见我们的 pg_attribute 改动。不过这
		 * 看起来有点丑。（实际中，共享关系在
		 * bootstrap 之后不该有模式变更，所以我们根本
		 * 不该因共享关系走到这里）
		 */
		databaseId = MyDatabaseId;
	}
	else if (tupleRelId == IndexRelationId)
	{
		Form_pg_index indextup = (Form_pg_index) GETSTRUCT(tuple);

		/*
		 * 当 pg_index 行更新时，应为索引关系发出一条
		 * relcache 失效。同上，我们不知道索引是否共享，
		 * 但实际上无关紧要，因为共享目录的索引不可能
		 * 有这类更新。
		 */
		relationId = indextup->indexrelid;
		databaseId = MyDatabaseId;
	}
	else if (tupleRelId == ConstraintRelationId)
	{
		Form_pg_constraint constrtup = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * 外键也属于 relcache 项，故要为 FK 应用的表
		 * 发出一条失效。
		 */
		if (constrtup->contype == CONSTRAINT_FOREIGN &&
			OidIsValid(constrtup->conrelid))
		{
			relationId = constrtup->conrelid;
			databaseId = MyDatabaseId;
		}
		else
			return;
	}
	else
		return;

	/*
	 * 是。需要登记一条 relcache 失效事件。
	 */
	RegisterRelcacheInvalidation(databaseId, relationId);
}

/*
 * CacheInvalidateCatalog
 *		登记系统目录全部内容的失效。
 *
 * 这通常用于 VACUUM FULL/CLUSTER，其中我们与其说是
 * 改动了元组，不如说是移动了它们。catcache 项的某些
 * 用法要求 TIDs 正确，所以必须清掉这些项。
 *
 * 注意：期望调用者核实该 rel 确实是系统
 * 目录。若不是，也无大碍，只浪费一条 sinval 消息。
 */
void
CacheInvalidateCatalog(Oid catalogId)
{
	Oid			databaseId;

	PrepareInvalidationState();

	if (IsSharedRelation(catalogId))
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;

	RegisterCatalogInvalidation(databaseId, catalogId);
}

/*
 * CacheInvalidateRelcache
 *		登记指定关系的 relcache 项在命令结束时的
 *		失效。
 *
 * 它用于需要强制重建 relcache 但不改动任何被
 * CacheInvalidateHeapTuple 认定为 relcache 项贡献者的
 * 元组的地方（例如删除索引）。
 */
void
CacheInvalidateRelcache(Relation relation)
{
	Oid			databaseId;
	Oid			relationId;

	PrepareInvalidationState();

	relationId = RelationGetRelid(relation);
	if (relation->rd_rel->relisshared)
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;

	RegisterRelcacheInvalidation(databaseId, relationId);
}

/*
 * CacheInvalidateRelcacheAll
 *		在命令结束时登记整个 relcache 的失效。
 *
 * 更改发布的操作会用到它，因为发布中的改动可能影
 * 响大量表。
 */
void
CacheInvalidateRelcacheAll(void)
{
	PrepareInvalidationState();

	RegisterRelcacheInvalidation(InvalidOid, InvalidOid);
}

/*
 * CacheInvalidateRelcacheByTuple
 *		同上，但通过传入 pg_class 元组来标识关系。
 */
void
CacheInvalidateRelcacheByTuple(HeapTuple classTuple)
{
	Form_pg_class classtup = (Form_pg_class) GETSTRUCT(classTuple);
	Oid			databaseId;
	Oid			relationId;

	PrepareInvalidationState();

	relationId = classtup->oid;
	if (classtup->relisshared)
		databaseId = InvalidOid;
	else
		databaseId = MyDatabaseId;
	RegisterRelcacheInvalidation(databaseId, relationId);
}

/*
 * CacheInvalidateRelcacheByRelid
 *		同上，但通过传入 OID 来标识关系。
 *		这是三者中效率最低的；若已有 Relation 或
 *		pg_class 元组，请用上面两个例程。
 */
void
CacheInvalidateRelcacheByRelid(Oid relid)
{
	HeapTuple	tup;

	PrepareInvalidationState();

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	CacheInvalidateRelcacheByTuple(tup);
	ReleaseSysCache(tup);
}


/*
 * CacheInvalidateSmgr
 *		登记对物理关系的 smgr 引用的失效。
 *
 * 发送这种失效消息会迫使其他后端关闭该关系已打开
 * 的 smgr 项。当物理关系被删除或截断时，应当这样做
 * 以清除悬挂的打开文件引用。因为这些是非事务性的
 * （即不可回退）操作，我们直接立即发送失效消息而不
 * 做任何排队。
 *
 * 注意：多数情况下，在逻辑层面已经会有针对该关系的
 * relcache 刷新。我们之所以需要单独的 smgr 层面刷新，
 * 是因为后端可能对没有 relcache 项的关系持有打开的
 * smgr 项，例如因为它们对该关系唯一做过的事就是把
 * 脏共享缓冲区写出。
 *
 * 注意：因为这些消息是非事务性的，它们不会被捕获在
 * 提交/中止 WAL 记录中。取而代之，CacheInvalidateSmgr()
 * 的调用应当发生在底层 smgr.c 例程中，这些例程既在
 * 重放或生成 WAL 时均会执行。
 *
 * 注意：为了避免 SharedInvalidationMessage 膨胀，我们
 * 只借用原本作为填充空间的空间来存后端 ID 三字节。
 * 因此后端 ID 的最大可能值为 2^23-1。
 */
void
CacheInvalidateSmgr(RelFileLocatorBackend rlocator)
{
	SharedInvalidationMessage msg;

	msg.sm.id = SHAREDINVALSMGR_ID;
	msg.sm.backend_hi = rlocator.backend >> 16;
	msg.sm.backend_lo = rlocator.backend & 0xffff;
	msg.sm.rlocator = rlocator.locator;
	/* 含义参见 AddCatcacheInvalidationMessage() */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	SendSharedInvalidMessages(&msg, 1);
}

/*
 * CacheInvalidateRelmap
 *		登记某个数据库的关系映射的失效；
 *		如果 databaseId 为零则针对共享目录。
 *
 * 发送这种失效消息会迫使其他后端重读指定的关系映
 * 射文件；还有必要为映射被改的那些关系发出
 * relcache 失效，否则 relcache 不会用新的 filenode 数
 * 据更新。
 *
 * 注意：因为这些消息是非事务性的，它们不会被捕获在
 * 提交/中止 WAL 记录中。取而代之，CacheInvalidateRelmap()
 * 的调用应当发生在底层 relmapper.c 例程中，这些例程
 * 重放或生成 WAL 时均会执行。
 */
void
CacheInvalidateRelmap(Oid databaseId)
{
	SharedInvalidationMessage msg;

	msg.rm.id = SHAREDINVALRELMAP_ID;
	msg.rm.dbId = databaseId;
	/* 含义参见 AddCatcacheInvalidationMessage() */
	VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));

	SendSharedInvalidMessages(&msg, 1);
}


/*
 * CacheRegisterSyscacheCallback
 *		登记指定函数，令其处理指定缓存今后
 *		所有失效事件。缓存 ID 以及被失效元组的
 *		哈希值都会传给该
 *		函数。
 *
 * 注意：如果收到缓存重置请求，传出的哈希值将为零。
 * 此时被调用的例程应刷新全部缓存状态。
 * 是的，确实存在与零误匹配的可能性，但似乎不值得
 * 为之烦恼，尤其因为目前多数被调用者反正都只
 * 刷新全部缓存状态。
 */
void
CacheRegisterSyscacheCallback(int cacheid,
							  SyscacheCallbackFunction func,
							  Datum arg)
{
	if (cacheid < 0 || cacheid >= SysCacheSize)
		elog(FATAL, "invalid cache ID: %d", cacheid);
	if (syscache_callback_count >= MAX_SYSCACHE_CALLBACKS)
		elog(FATAL, "out of syscache_callback_list slots");

	if (syscache_callback_links[cacheid] == 0)
	{
		/* 该缓存的首个回调 */
		syscache_callback_links[cacheid] = syscache_callback_count + 1;
	}
	else
	{
		/* 加到链尾，使较早的回调先被调用 */
		int			i = syscache_callback_links[cacheid] - 1;

		while (syscache_callback_list[i].link > 0)
			i = syscache_callback_list[i].link - 1;
		syscache_callback_list[i].link = syscache_callback_count + 1;
	}

	syscache_callback_list[syscache_callback_count].id = cacheid;
	syscache_callback_list[syscache_callback_count].link = 0;
	syscache_callback_list[syscache_callback_count].function = func;
	syscache_callback_list[syscache_callback_count].arg = arg;

	++syscache_callback_count;
}

/*
 * CacheRegisterRelcacheCallback
 *		登记指定函数，令其处理指定缓存今后
 *		所有 relcache 失效。被失效关系的 OID 传给
 *		该函数。
 *
 * 注意：如果收到缓存重置请求，将传入 InvalidOid。
 * 此时被调用的例程应刷新全部缓存状态。
 */
void
CacheRegisterRelcacheCallback(RelcacheCallbackFunction func,
							  Datum arg)
{
	if (relcache_callback_count >= MAX_RELCACHE_CALLBACKS)
		elog(FATAL, "out of relcache_callback_list slots");

	relcache_callback_list[relcache_callback_count].function = func;
	relcache_callback_list[relcache_callback_count].arg = arg;

	++relcache_callback_count;
}

/*
 * CallSyscacheCallbacks
 *
 * 导出此函数是为了让 CatalogCacheFlushCatalog 能调用
 * 它，免得本模块需要知道哪些 catcache ID 对应哪些目录。
 */
void
CallSyscacheCallbacks(int cacheid, uint32 hashvalue)
{
	int			i;

	if (cacheid < 0 || cacheid >= SysCacheSize)
		elog(ERROR, "invalid cache ID: %d", cacheid);

	i = syscache_callback_links[cacheid] - 1;
	while (i >= 0)
	{
		struct SYSCACHECALLBACK *ccitem = syscache_callback_list + i;

		Assert(ccitem->id == cacheid);
		ccitem->function(ccitem->arg, cacheid, hashvalue);
		i = ccitem->link - 1;
	}
}

/*
 * LogLogicalInvalidations
 *
 * 把当前命令引发的失效记录到 WAL。
 *
 * 这目前只用于在命令结束时记录失效，或者在提交时
 * 若有失效挂起则一并记录。
 */
void
LogLogicalInvalidations(void)
{
	xl_xact_invals xlrec;
	InvalidationMsgsGroup *group;
	int			nmsgs;

	/* 若尚未对失效消息做过任何处理则快速退出。 */
	if (transInvalInfo == NULL)
		return;

	group = &transInvalInfo->CurrentCmdInvalidMsgs;
	nmsgs = NumMessagesInGroup(group);

	if (nmsgs > 0)
	{
		/* 准备记录 */
		memset(&xlrec, 0, MinSizeOfXactInvals);
		xlrec.nmsgs = nmsgs;

		/* 执行插入 */
		XLogBeginInsert();
		XLogRegisterData((char *) (&xlrec), MinSizeOfXactInvals);
		ProcessMessageSubGroupMulti(group, CatCacheMsgs,
									XLogRegisterData((char *) msgs,
													 n * sizeof(SharedInvalidationMessage)));
		ProcessMessageSubGroupMulti(group, RelCacheMsgs,
									XLogRegisterData((char *) msgs,
													 n * sizeof(SharedInvalidationMessage)));
		XLogInsert(RM_XACT_ID, XLOG_XACT_INVALIDATIONS);
	}
}
