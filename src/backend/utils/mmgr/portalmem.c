/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  后台门户的内存管理
 *
 * 门户（portal）表示查询的执行状态。本模块
 * 为门户提供内存管理服务，但并不真的为它们运行
 * 执行器。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/portalmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/portalcmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

/*
 * 对用户可能打开的门户数上限的估计，在
 * EnablePortalManager() 中初始化 PortalHashTable 大小
 * 由于哈希表可以扩展，没必要把这个值定得过于
 * 宽裕；保持较小的值可避免事务结束时执行的
 * hash_seq_search() 调用带来不必要的开销。
 */
#define PORTALS_PER_USER	   16


/* ----------------
 *		全局状态
 * ----------------
 */

#define MAX_PORTALNAME_LEN		NAMEDATALEN

typedef struct portalhashent
{
	char		portalname[MAX_PORTALNAME_LEN];
	Portal		portal;
} PortalHashEnt;

static HTAB *PortalHashTable = NULL;

#define PortalHashTableLookup(NAME, PORTAL) \
do { \
	PortalHashEnt *hentry; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   (NAME), HASH_FIND, NULL); \
	if (hentry) \
		PORTAL = hentry->portal; \
	else \
		PORTAL = NULL; \
} while(0)

#define PortalHashTableInsert(PORTAL, NAME) \
do { \
	PortalHashEnt *hentry; bool found; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   (NAME), HASH_ENTER, &found); \
	if (found) \
		elog(ERROR, "duplicate portal name"); \
	hentry->portal = PORTAL; \
	/* 为避免重复存储，让 PORTAL->name 指向 htab 条目 */ \
	PORTAL->name = hentry->portalname; \
} while(0)

#define PortalHashTableDelete(PORTAL) \
do { \
	PortalHashEnt *hentry; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   PORTAL->name, HASH_REMOVE, NULL); \
	if (hentry == NULL) \
		elog(WARNING, "trying to delete portal name that does not exist"); \
} while(0)

static MemoryContext TopPortalContext = NULL;


/* ----------------------------------------------------------------
 *			   公共门户接口函数
 * ----------------------------------------------------------------
 */

/*
 * EnablePortalManager
 *		在后台启动时启用门户管理模块。
 */
void
EnablePortalManager(void)
{
	HASHCTL		ctl;

	Assert(TopPortalContext == NULL);

	TopPortalContext = AllocSetContextCreate(TopMemoryContext,
											 "TopPortalContext",
											 ALLOCSET_DEFAULT_SIZES);

	ctl.keysize = MAX_PORTALNAME_LEN;
	ctl.entrysize = sizeof(PortalHashEnt);

	/*
	 * 用 PORTALS_PER_USER 作为初始要创建多少哈希表
	 * 条目数猜值
	 */
	PortalHashTable = hash_create("Portal hash", PORTALS_PER_USER,
								  &ctl, HASH_ELEM | HASH_STRINGS);
}

/*
 * GetPortalByName
 *		按门户名返回门户，找不到则返回 NULL。
 */
Portal
GetPortalByName(const char *name)
{
	Portal		portal;

	if (PointerIsValid(name))
		PortalHashTableLookup(name, portal);
	else
		portal = NULL;

	return portal;
}

/*
 * PortalGetPrimaryStmt
 *		取门户内的"主" stmt，即标记了 canSetTag 的那个。
 *
 * 若无这样的 stmt 则返回 NULL。若门户内有多个
 * PlannedStmt 标记了 canSetTag，则返回第一个。在
 * 本函数现有用法中两情况都不应发生。
 */
PlannedStmt *
PortalGetPrimaryStmt(Portal portal)
{
	ListCell   *lc;

	foreach(lc, portal->stmts)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);

		if (stmt->canSetTag)
			return stmt;
	}
	return NULL;
}

/*
 * CreatePortal
 *		按给定名称返回新门户。
 *
 * allowDup: 若为 true，自动丢弃已有的同名门户
 *（若为 false 则报错）。
 *
 * dupSilent: 若为 true，WARNING 也不发。
 */
Portal
CreatePortal(const char *name, bool allowDup, bool dupSilent)
{
	Portal		portal;

	Assert(PointerIsValid(name));

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		if (!allowDup)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("cursor \"%s\" already exists", name)));
		if (!dupSilent)
			ereport(WARNING,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("closing existing cursor \"%s\"",
							name)));
		PortalDrop(portal, false);
	}

	/* 制作新门户结构 */
	portal = (Portal) MemoryContextAllocZero(TopPortalContext, sizeof *portal);

	/* 初始化门户上下文；通常它不会存储太多 */
	portal->portalContext = AllocSetContextCreate(TopPortalContext,
												  "PortalContext",
												  ALLOCSET_SMALL_SIZES);

	/* 为门户创建资源所有者 */
	portal->resowner = ResourceOwnerCreate(CurTransactionResourceOwner,
										   "Portal");

	/* 初始化并非从零起步的门户字段 */
	portal->status = PORTAL_NEW;
	portal->cleanup = PortalCleanup;
	portal->createSubid = GetCurrentSubTransactionId();
	portal->activeSubid = portal->createSubid;
	portal->createLevel = GetCurrentTransactionNestLevel();
	portal->strategy = PORTAL_MULTI_QUERY;
	portal->cursorOptions = CURSOR_OPT_NO_SCROLL;
	portal->atStart = true;
	portal->atEnd = true;		/* 查询设好前禁止抓取 */
	portal->visible = true;
	portal->creation_time = GetCurrentStatementStartTimestamp();

	/* 放入表中（设置 portal->name） */
	PortalHashTableInsert(portal, name);

	/* 具名门户复用 portal->name 副本 */
	MemoryContextSetIdentifier(portal->portalContext, portal->name[0] ? portal->name : "<unnamed>");

	return portal;
}

/*
 * CreateNewPortal
 *		创建新门户，为其分配不冲突的随机名称。
 */
Portal
CreateNewPortal(void)
{
	static unsigned int unnamed_portal_count = 0;

	char		portalname[MAX_PORTALNAME_LEN];

	/* 选个不冲突的名称 */
	for (;;)
	{
		unnamed_portal_count++;
		sprintf(portalname, "<unnamed portal %u>", unnamed_portal_count);
		if (GetPortalByName(portalname) == NULL)
			break;
	}

	return CreatePortal(portalname, false, false);
}

/*
 * PortalDefineQuery
 *		建立一个门户查询的简单子例程。
 *
 * 注：自 PG 8.4 起，调用者必须提供 sourceText 字符串；
 * 再也不能传 NULL。（若实在没有源文本，可以传一个
 * 常量字符串，比如 "(query not available)"。）
 *
 * commandTag 当且仅当原始查询串（重写前）是空串时
 * 才应为 NULL。另外，传入的 commandTag 必须是指向
 * 常量字符串的指针，因为它不会被复制。
 *
 * 若提供 cplan，则它是包含这些 stmt 的缓存计划，且调
 * 用者必须已执行过 GetCachedPlan()，使引用计数增加。
 * 门户被销毁时该引用计数会被释放。
 *
 * 若 cplan 为 NULL，则确保传入的计划树寿命足够是调
 * 用者的责任。通常做法是把它们复制到门户的上下
 * 文中。
 *
 * 调用者还负责确保传入的 prepStmtName（若非 NULL）
 * 和 sourceText 有足够的寿命。
 *
 * 注意：本函数除了保存传入的值之外不应做太多事；
 * 尤其不要做任何可能触发 elog(ERROR) 的事情。如果
 * 这发生在保存 cplan 引用之前，我们会泄漏调用者想
 * 移交给我们的 plancache 引用计数。
 */
void
PortalDefineQuery(Portal portal,
				  const char *prepStmtName,
				  const char *sourceText,
				  CommandTag commandTag,
				  List *stmts,
				  CachedPlan *cplan)
{
	Assert(PortalIsValid(portal));
	Assert(portal->status == PORTAL_NEW);

	Assert(sourceText != NULL);
	Assert(commandTag != CMDTAG_UNKNOWN || stmts == NIL);

	portal->prepStmtName = prepStmtName;
	portal->sourceText = sourceText;
	portal->qc.commandTag = commandTag;
	portal->qc.nprocessed = 0;
	portal->commandTag = commandTag;
	portal->stmts = stmts;
	portal->cplan = cplan;
	portal->status = PORTAL_DEFINED;
}

/*
 * PortalReleaseCachedPlan
 *		释放门户对其缓存计划的引用（若有）。
 */
static void
PortalReleaseCachedPlan(Portal portal)
{
	if (portal->cplan)
	{
		ReleaseCachedPlan(portal->cplan, NULL);
		portal->cplan = NULL;

		/*
		 * 我们还必须清掉 portal->stmts，它现在是指向缓存
		 * 计划的计划列表的悬空引用。这可保护日后试图
		 * 检视该 Portal 的代码。
		 */
		portal->stmts = NIL;
	}
}

/*
 * PortalCreateHoldStore
 *		为门户创建 tuplestore。
 */
void
PortalCreateHoldStore(Portal portal)
{
	MemoryContext oldcxt;

	Assert(portal->holdContext == NULL);
	Assert(portal->holdStore == NULL);
	Assert(portal->holdSnapshot == NULL);

	/*
	 * 创建用于存储元组集的内存上下文。注意这并不
	 * 是门户 portalContext 的子上下文。
	 */
	portal->holdContext =
		AllocSetContextCreate(TopPortalContext,
							  "PortalHoldContext",
							  ALLOCSET_DEFAULT_SIZES);

	/*
	 * 创建元组存储（tuple store），选用跨事务临时文件
	 * 且仅当游标需要滚动时才启用随机访问。
	 *
	 * XXX：门户大小是否该用 maintenance_work_mem？
	 */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	portal->holdStore =
		tuplestore_begin_heap(portal->cursorOptions & CURSOR_OPT_SCROLL,
							  true, work_mem);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * PinPortal
 *		保护门户不被丢弃。
 *
 * 被钉住的门户在事务或子事务中止时仍会被解除
 * 钉住并丢弃。
 */
void
PinPortal(Portal portal)
{
	if (portal->portalPinned)
		elog(ERROR, "portal already pinned");

	portal->portalPinned = true;
}

void
UnpinPortal(Portal portal)
{
	if (!portal->portalPinned)
		elog(ERROR, "portal not pinned");

	portal->portalPinned = false;
}

/*
 * MarkPortalActive
 *		把门户从 READY 转为 ACTIVE 状态。
 *
 * 勿直接设置 portal->status = PORTAL_ACTIVE；应调用本函数。
 */
void
MarkPortalActive(Portal portal)
{
	/* 为安全起见，此为运行时测试而非 Assert */
	if (portal->status != PORTAL_READY)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("portal \"%s\" cannot be run", portal->name)));
	/* 执行状态转换 */
	portal->status = PORTAL_ACTIVE;
	portal->activeSubid = GetCurrentSubTransactionId();
}

/*
 * MarkPortalDone
 *		把门户从 ACTIVE 转为 DONE 状态。
 *
 * 勿直接设置 portal->status = PORTAL_DONE；应调用本函数。
 */
void
MarkPortalDone(Portal portal)
{
	/* 执行状态转换 */
	Assert(portal->status == PORTAL_ACTIVE);
	portal->status = PORTAL_DONE;

	/*
	 * 让 portalcmds.c 清理它所知道的状态。反正现在就
	 * 可以做，因为门户再也不能被执行了。
	 *
	 * 在某些涉及在已中止事务中执行 ROLLBACK 命令的
	 * 情形下，这一步是必需的，否则我们会带着尚未
	 * 执行钩子到达 AtCleanup_Portals。
	 */
	if (PointerIsValid(portal->cleanup))
	{
		portal->cleanup(portal);
		portal->cleanup = NULL;
	}
}

/*
 * MarkPortalFailed
 *		把门户转为 FAILED 状态。
 *
 * 勿直接设置 portal->status = PORTAL_FAILED；应调用本函数。
 */
void
MarkPortalFailed(Portal portal)
{
	/* 执行状态转换 */
	Assert(portal->status != PORTAL_DONE);
	portal->status = PORTAL_FAILED;

	/*
	 * 让 portalcmds.c 清理它所知道的状态。反正现在就
	 * 可以做，因为门户再也不能被执行了。
	 *
	 * 在某些涉及清理已中止事务的情形下，这一步是
	 * 必需的，否则我们到达 AtCleanup_Portals 时，
	 * 钩子未跑。
	 */
	if (PointerIsValid(portal->cleanup))
	{
		portal->cleanup(portal);
		portal->cleanup = NULL;
	}
}

/*
 * PortalDrop
 *		销毁门户。
 */
void
PortalDrop(Portal portal, bool isTopCommit)
{
	Assert(PortalIsValid(portal));

	/*
	 * 不允许丢弃被钉住的门户，钉住者仍需要
	 * 它。
	 */
	if (portal->portalPinned)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cannot drop pinned portal \"%s\"", portal->name)));

	/*
	 * 不确定 PORTAL_ACTIVE 情形是否真的可能发生……
	 */
	if (portal->status == PORTAL_ACTIVE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cannot drop active portal \"%s\"", portal->name)));

	/*
	 * 让 portalcmds.c 清理它所知道的状态，尤其是若执行
	 * 器仍在运行则将其关闭。这一步可能运行用户定义
	 * 代码，所以失败是必须预期的。若失败发生且我们
	 * 之后在事务中止期间再次回来丢弃门户，清理钩子
	 * 有责任保证不再次那样做。
	 *
	 *
	 * 注：在多数控制路径中，这已在 MarkPortalDone 或
	 * MarkPortalFailed 中做过。我们只是再确认一遍。
	 */
	if (PointerIsValid(portal->cleanup))
	{
		portal->cleanup(portal);
		portal->cleanup = NULL;
	}

	/* 除出错之后外，不应再有活动快照 */
	Assert(portal->portalSnapshot == NULL || !isTopCommit);

	/*
	 * 把门户从哈希表中移除。因为在这里做这件事，
	 * 后续步骤中若发生任何错误，我们也不会回来再
	 * 次尝试移除门户。泄漏一点内存也比陷入无限的
	 * 错误恢复循环强。
	 */
	PortalHashTableDelete(portal);

	/* 丢缓存计划引用（若有） */
	PortalReleaseCachedPlan(portal);

	/*
	 * 若门户有保护其数据的快照，释放它。这需要一点
	 * 小心，因为注册会挂在门户的资源所有者上；若
	 * 门户失败，我们在事务中止期间将已经释放了
	 * 资源所有者（和快照）。
	 */
	if (portal->holdSnapshot)
	{
		if (portal->resowner)
			UnregisterSnapshotFromOwner(portal->holdSnapshot,
										portal->resowner);
		portal->holdSnapshot = NULL;
	}

	/*
	 * 释放仍挂在门户上的所有资源。这里涵盖几种
	 * 情形：
	 *
	 * 顶层事务提交（由 isTopCommit 指示）：通常我们
	 * 在这里什么都不做，让常规的端事务资源释放
	 * 机制也处理这些资源。然而，若我们有一个 FAILED
	 * 门户（如出错的游标），最好清理其资源，以免
	 * 收到资源泄漏警告消息。
	 *
	 * 子事务提交：根本不会到这里，因为我们不杀
	 * AtSubCommit_Portals() 门户。
	 *
	 * 主或子事务中止：我们在这里什么都不做，
	 * 因为 portal->resowner 已被置为 NULL；资源已在
	 * 中止时清理完毕。
	 *
	 * 普通门户丢弃：必须释放资源。但若门户不是
	 * FAILED，则不释放其锁。锁将转为事务的
	 * ResourceOwner（因为它是门户所有者的父）的
	 * 责任，并将在事务最终结束时被释放。
	 *
	 */
	if (portal->resowner &&
		(!isTopCommit || portal->status == PORTAL_FAILED))
	{
		bool		isCommit = (portal->status != PORTAL_FAILED);

		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_AFTER_LOCKS,
							 isCommit, false);
		ResourceOwnerDelete(portal->resowner);
	}
	portal->resowner = NULL;

	/*
	 * 若有 tuplestore 则删除。即便在出错条件下也应
	 * 这样做；由于 tuplestore 会使用跨事务存储，
	 * 其临时文件需要被显式删除。
	 */
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_end(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
		portal->holdStore = NULL;
	}

	/* 删 tuplestore 存储（若有） */
	if (portal->holdContext)
		MemoryContextDelete(portal->holdContext);

	/* 释放附属存储 */
	MemoryContextDelete(portal->portalContext);

	/* 释放门户结构（在 TopPortalContext 中） */
	pfree(portal);
}

/*
 * 删除已声明的游标。
 *
 * 由 CLOSE ALL、DISCARD ALL 使用
 */
void
PortalHashTableDeleteAll(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	if (PortalHashTable == NULL)
		return;

	hash_seq_init(&status, PortalHashTable);
	while ((hentry = hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* 无法关闭活动门户（正在运行命令的那个） */
		if (portal->status == PORTAL_ACTIVE)
			continue;

		PortalDrop(portal, false);

		/* 若那导致其他丢弃，则重启迭代 */
		hash_seq_term(&status);
		hash_seq_init(&status, PortalHashTable);
	}
}

/*
 * "保持"门户。为后续事务的访问做好准备。
 */
static void
HoldPortal(Portal portal)
{
	/*
	 * 注意 PersistHoldablePortal() 必须释放门户所用的、
	 * 属于创建事务的本地资源。
	 */
	PortalCreateHoldStore(portal);
	PersistHoldablePortal(portal);

	/* 丢缓存计划引用（若有） */
	PortalReleaseCachedPlan(portal);

	/*
	 * 属于门户的所有资源将在接下来的全事务清理中
	 * 释放；门户将不再拥有自己的资源。
	 *
	 */
	portal->resowner = NULL;

	/*
	 * 成功导出可保持游标后，把它标记为不再属于
	 * 本事务。
	 */
	portal->createSubid = InvalidSubTransactionId;
	portal->activeSubid = InvalidSubTransactionId;
	portal->createLevel = 0;
}

/*
 * 门户的提交前处理。
 *
 * 本事务中创建的可保持游标需要转换为物化形式，
 * 因为我们即将关闭执行器并释放锁。本事务中创建
 * 的非可保持门户直接移除。先前事务遗留的门户
 * 应保持原样。
 *
 *
 * 若有门户改变状态（可能引发用户定义代码），
 * 返回 true，否则 false。
 */
bool
PreCommit_Portals(bool isPrepare)
{
	bool		result = false;
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * 不应再有被钉住的门户。若有人泄漏了一个，
		 * 就报错。自动保持（auto-held）的门户是允许
		 * 钉住者自会管理。
		 */
		if (portal->portalPinned && !portal->autoHeld)
			elog(ERROR, "cannot commit while a portal is pinned");

		/*
		 * 不要碰活动门户 --- 这只可能发生在多事务
		 * 实用命令（如 VACUUM）或过程中的提交的情形。
		 *
		 *
		 * 但请注意，挂在此类门户上的资源所有者仍将
		 * 消失，所以不要留下悬空指针。同时注销门户
		 * 持有的快照，主要是为了避免来自
		 * ResourceOwnerRelease() 的快照泄漏警告。
		 */
		if (portal->status == PORTAL_ACTIVE)
		{
			if (portal->holdSnapshot)
			{
				if (portal->resowner)
					UnregisterSnapshotFromOwner(portal->holdSnapshot,
												portal->resowner);
				portal->holdSnapshot = NULL;
			}
			portal->resowner = NULL;
			/* 也把 portalSnapshot 清理干净 */
			portal->portalSnapshot = NULL;
			continue;
		}

		/* 是在当前事务中创建的可保持门户吗？ */
		if ((portal->cursorOptions & CURSOR_OPT_HOLD) &&
			portal->createSubid != InvalidSubTransactionId &&
			portal->status == PORTAL_READY)
		{
			/*
			 * 我们正在退出创建可保持游标的事务。这里
			 * 不丢弃门户，而是为后续事务的访问做好
			 * 准备。
			 *
			 * 然而，若这是 PREPARE TRANSACTION 而非
			 * COMMIT，拒绝 PREPARE，因语义相当不清晰。
			 */
			if (isPrepare)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has created a cursor WITH HOLD")));

			HoldPortal(portal);

			/* 报告状态已改变 */
			result = true;
		}
		else if (portal->createSubid == InvalidSubTransactionId)
		{
			/*
			 * 对先前事务遗留的游标什么都不做（包括
			 * 我们在本循环先前一轮中刚冻结的那些）
			 */
			continue;
		}
		else
		{
			/* 干掉非可保持门户 */
			PortalDrop(portal, true);

			/* 报告状态已改变 */
			result = true;
		}

		/*
		 * 在冻结或丢弃门户之后，必须重启迭代，因为
		 * 我们可能已触发用户定义代码，导致哈希链中
		 * 下一个门户被丢弃。
		 */
		hash_seq_term(&status);
		hash_seq_init(&status, PortalHashTable);
	}

	return result;
}

/*
 * 门户的中止处理。
 *
 * 此时若存在清理钩子我们会运行它，但在清理调用
 * 之前不能释放门户的内存。
 */
void
AtAbort_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * 当 elog(FATAL) 进行期间，需把活动门户置为
		 * failed，让 PortalCleanup() 不跑执行器关闭。
		 */
		if (portal->status == PORTAL_ACTIVE && shmem_exit_inprogress)
			MarkPortalFailed(portal);

		/*
		 * 对先前事务遗留的游标其余什么都不做。
		 */
		if (portal->createSubid == InvalidSubTransactionId)
			continue;

		/*
		 * 对自动保持的游标什么都不做。这类似于前一
		 * 个事务的游标的情形，但它也可能是本事务中
		 * 被自动保持的游标，所以它想要继续存活。
		 */
		if (portal->autoHeld)
			continue;

		/*
		 * 若它是在当前事务中创建的，我们也不能对
		 * READY 门户做正常关闭；它可能引用失败事
		 * 务中创建的对象，见 AtSubAbort_Portals
		 * 的注释。
		 */
		if (portal->status == PORTAL_READY)
			MarkPortalFailed(portal);

		/*
		 * 让 portalcmds.c 清理它所知道的状态，如果我们
		 * 还没做。
		 */
		if (PointerIsValid(portal->cleanup))
		{
			portal->cleanup(portal);
			portal->cleanup = NULL;
		}

		/* 丢缓存计划引用（若有） */
		PortalReleaseCachedPlan(portal);

		/*
		 * 属于门户的所有资源将在接下来的全事务清理
		 * 中释放；在我们运行 PortalDrop 之前它们就会
		 * 消失。
		 */
		portal->resowner = NULL;

		/*
		 * 虽然我们不能删除门户数据结构本体，但可以
		 * 释放附属上下文中的内存，如执行器状态。清理
		 * 钩子是最后一个可能需要那里数据的东西。但
		 * 活动门户除外。
		 */
		if (portal->status != PORTAL_ACTIVE)
			MemoryContextDeleteChildren(portal->portalContext);
	}
}

/*
 * 门户的中止后清理。
 *
 * 删除所有非先前事务遗留的门户。 */
void
AtCleanup_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * 不要碰活动门户 --- 这只可能发生在多事务
		 * 命令的情形。
		 */
		if (portal->status == PORTAL_ACTIVE)
			continue;

		/*
		 * 对先前事务遗留或自动保持游标一律
		 * 不动。
		 */
		if (portal->createSubid == InvalidSubTransactionId || portal->autoHeld)
		{
			Assert(portal->status != PORTAL_ACTIVE);
			Assert(portal->resowner == NULL);
			continue;
		}

		/*
		 * 若门户仍被钉住，则强制解除钉住。否则
		 * PortalDrop 不会让我们丢弃该门户。钉住门
		 * 户的人也被中止所打断，不会再试图使用它。
		 */
		if (portal->portalPinned)
			portal->portalPinned = false;

		/*
		 * 我们最好不要在清理期间调用任何用户定义
		 * 代码，所以若清理钩子尚未运行，只好跳过它。
		 */
		if (PointerIsValid(portal->cleanup))
		{
			elog(WARNING, "skipping cleanup for portal \"%s\"", portal->name);
			portal->cleanup = NULL;
		}

		/* 删。 */
		PortalDrop(portal, false);
	}
}

/*
 * 出错返回主循环时的门户相关清理。
 *
 * 这不同于事务中止时的清理。自动保持的门户会
 * 出错时被清理，事务中止时则不会。
 */
void
PortalErrorCleanup(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->autoHeld)
		{
			portal->portalPinned = false;
			PortalDrop(portal, false);
		}
	}
}

/*
 * 门户的子提交前处理。
 *
 * 把当前子事务中创建或使用的门户改派给父
 * 子事务。
 */
void
AtSubCommit_Portals(SubTransactionId mySubid,
					SubTransactionId parentSubid,
					int parentLevel,
					ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->createSubid == mySubid)
		{
			portal->createSubid = parentSubid;
			portal->createLevel = parentLevel;
			if (portal->resowner)
				ResourceOwnerNewParent(portal->resowner, parentXactOwner);
		}
		if (portal->activeSubid == mySubid)
			portal->activeSubid = parentSubid;
	}
}

/*
 * 门户的子事务中止处理。
 *
 * 使失败的子事务中创建或使用的门户失效。按
 * AtSubCommit_Portals，这也会捕获在子事务的后
 * 代中创建/使用的门户。
 *
 * 这里不销毁任何门户；那在 AtSubCleanup_Portals 做。
 */
void
AtSubAbort_Portals(SubTransactionId mySubid,
				   SubTransactionId parentSubid,
				   ResourceOwner myXactOwner,
				   ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* 是在本子事务中创建的吗？ */
		if (portal->createSubid != mySubid)
		{
			/* 不；但也许本子事务中用过它？ */
			if (portal->activeSubid == mySubid)
			{
				/* 在门户被移除前维护 activeSubid */
				portal->activeSubid = parentSubid;

				/*
				 * 一个 MarkPortalActive() 调用者在本子事
				 * 务中运行了上层门户并让其保持 ACTIVE。
				 * 这本不该发生，但以同样的理由把门
				 * 户强制为 FAILED。
				 *
				 * 我们假定不强制上层 READY 门户失败也
				 * 能侥幸无事，即使它们被运行过然后又
				 * 被挂起。理论上一个被挂起的上层门户
				 * 可能已获得对即将销毁对象的某些引
				 * 用，但对此类情形应当有足够的防
				 * 御：门户的原始查询不可能包含此类
				 * 引用，而诸如 PL/pgSQL 函数缓存计划
				 * 内的引用并非来自活动查询，且应受
				 * 重新验证逻辑保护。
				 */
				if (portal->status == PORTAL_ACTIVE)
					MarkPortalFailed(portal);

				/*
				 * 另外，若我们在本子事务期间（就在上
				 * 面或更早）使其失败，则把它的资源所
				 * 有者重挂到当前子事务的资源所有者，
				 * 这样它仍持有的任何资源会在清理本子事
				 * 务时被释放。这可避免一些极端情形，其
				 * 中我们在清理本子事务期间创建的对象时
				 * 可能遇到 Assert 或更糟的情况（因为这
				 * 些对象仍被本门户引用）。
				 */
				if (portal->status == PORTAL_FAILED && portal->resowner)
				{
					ResourceOwnerNewParent(portal->resowner, myXactOwner);
					portal->resowner = NULL;
				}
			}
			/* 若不是本子事务中创建的就到此为止 */
			continue;
		}

		/*
		 * 把我自己子事务的任何存活门户强制为 FAILED
		 * 态。必须这样做，因为它们可能引用了失败
		 * 子事务中创建或修改的对象，导致
		 * portalcmds.c 试图关闭门户时 ExecutorEnd
		 * 内崩溃。目前每个 MarkPortalActive() 调用者
		 * 都确保在放弃控制前再次更新门户状态，所
		 * 以 ACTIVE 不会在此出现。若真出现，就按
		 * 现有调用者的做法处理。
		 */
		if (portal->status == PORTAL_READY ||
			portal->status == PORTAL_ACTIVE)
			MarkPortalFailed(portal);

		/*
		 * 让 portalcmds.c 清理它所知道的状态，如果我们
		 * 还没做。
		 */
		if (PointerIsValid(portal->cleanup))
		{
			portal->cleanup(portal);
			portal->cleanup = NULL;
		}

		/* 丢缓存计划引用（若有） */
		PortalReleaseCachedPlan(portal);

		/*
		 * 属于门户的所有资源将在接下来的全事务清理
		 * 中释放；在我们运行 PortalDrop 之前它们就会
		 * 消失。
		 */
		portal->resowner = NULL;

		/*
		 * 虽然我们不能删除门户数据结构本体，但可以
		 * 释放附属上下文中的内存，如执行器状态。清理
		 * 钩子是最后一个可能需要那里数据的东西。
		 *
		 */
		MemoryContextDeleteChildren(portal->portalContext);
	}
}

/*
 * 门户的子中止后清理。
 *
 * 删除失败的子事务中创建的所有门户（但注意
 * 我们不会删除上面改派给父事务的那些）。
 */
void
AtSubCleanup_Portals(SubTransactionId mySubid)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->createSubid != mySubid)
			continue;

		/*
		 * 若门户仍被钉住，则强制解除钉住。否则
		 * PortalDrop 不会让我们丢弃该门户。钉住门
		 * 户的人也被中止所打断，不会再试图使用它。
		 */
		if (portal->portalPinned)
			portal->portalPinned = false;

		/*
		 * 我们最好不要在清理期间调用任何用户定义
		 * 代码，所以若清理钩子尚未运行，只好跳过它。
		 */
		if (PointerIsValid(portal->cleanup))
		{
			elog(WARNING, "skipping cleanup for portal \"%s\"", portal->name);
			portal->cleanup = NULL;
		}

		/* 删。 */
		PortalDrop(portal, false);
	}
}

/* 找出全部可用游标 */
Datum
pg_cursor(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hash_seq;
	PortalHashEnt *hentry;

	/*
	 * 我们把所有元组在哈希表的一次扫描中放入
	 * tuplestore，避免哈希表在调用间可能改变的问题。
	 */
	InitMaterializedSRF(fcinfo, 0);

	hash_seq_init(&hash_seq, PortalHashTable);
	while ((hentry = hash_seq_search(&hash_seq)) != NULL)
	{
		Portal		portal = hentry->portal;
		Datum		values[6];
		bool		nulls[6] = {0};

		/* 只报告"可见"的条目 */
		if (!portal->visible)
			continue;
		/* 若 PortalDefineQuery 尚未被调用也忽略它 */
		if (!portal->sourceText)
			continue;

		values[0] = CStringGetTextDatum(portal->name);
		values[1] = CStringGetTextDatum(portal->sourceText);
		values[2] = BoolGetDatum(portal->cursorOptions & CURSOR_OPT_HOLD);
		values[3] = BoolGetDatum(portal->cursorOptions & CURSOR_OPT_BINARY);
		values[4] = BoolGetDatum(portal->cursorOptions & CURSOR_OPT_SCROLL);
		values[5] = TimestampTzGetDatum(portal->creation_time);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

bool
ThereAreNoReadyPortals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->status == PORTAL_READY)
			return false;
	}

	return true;
}

/*
 * 钉住的门户保持。
 *
 * 在过程内部发起 COMMIT 或 ROLLBACK 时，必须调用
 * 本函数，以保护内部生成的游标在事务关闭期间
 * 不被丢弃。目前 SPI 会自动调用本函数；以其他
 * 方式发起 COMMIT 或 ROLLBACK 的 PL 则有责任自
 * 行调用。（注意我们不能在诸如 AtAbort_Portals
 * 中做这件事，因为持久化门户时需要运行用户
 * 定义代码。事务中止一旦开始就为时已晚。）
 *
 * 我们把这些门户转换为已保持的游标来保护它们。
 * 我们把它们标记为"自动保持"，让异常退出知道
 * 要清理它们。（在正常的非异常代码路径中，PL 须
 * 自行清理，事务收尾不再代劳；这本就是惯例。）
 *
 */
void
HoldPinnedPortals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->portalPinned && !portal->autoHeld)
		{
			/*
			 * 在非只读的游标循环内做事务控制，尤其
			 * 是中止，例如使用 UPDATE ... RETURNING，
			 * 会有怪异的语义问题。且这种实现也
			 * 行不通，因为这类门户无法被保持。（核心
			 * 语法强制只有 SELECT 语句能驱动游标，但
			 * 例如 PL/pgSQL 并不限制这一点。）
			 *
			 */
			if (portal->strategy != PORTAL_ONE_SELECT)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot perform transaction commands inside a cursor loop that is not read-only")));

			/* 校验它处于适合被保持的状态 */
			if (portal->status != PORTAL_READY)
				elog(ERROR, "pinned portal is not ready to be auto-held");

			HoldPortal(portal);
			portal->autoHeld = true;
		}
	}
}

/*
 * 丢弃所有门户的外层活动快照，不留活动快照。
 *
 *
 * 与 HoldPinnedPortals 一样，本函数必须在过程内
 * 部发起 COMMIT 或 ROLLBACK 时调用。它必须与此
 * 分开，因为在可能失败的步骤完成之前不应运行。
 *
 * 把它并入 PreCommit_Portals 虽然诱人，但那样我
 * 们就需要在 VACUUM 或许还有其他地方清理快照。
 */
void
ForgetPortalSnapshots(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;
	int			numPortalSnaps = 0;
	int			numActiveSnaps = 0;

	/* 先扫描 PortalHashTable 并清掉 portalSnapshot 字段 */
	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->portalSnapshot != NULL)
		{
			portal->portalSnapshot = NULL;
			numPortalSnaps++;
		}
		/* portal->holdSnapshot 将在 PreCommit_Portals 中清理 */
	}

	/*
	 * 现在，弹出所有活动快照，它们应该恰好都是门
	 * 户快照。理想情况下我们会直接从门户扫描来
	 * 驱动这一过程，但没有好办法按正确顺序访问
	 * 各门户。故只能事后交叉校验。
	 */
	while (ActiveSnapshotSet())
	{
		PopActiveSnapshot();
		numActiveSnaps++;
	}

	if (numPortalSnaps != numActiveSnaps)
		elog(ERROR, "portal snapshots (%d) did not account for all active snapshots (%d)",
			 numPortalSnaps, numActiveSnaps);
}
