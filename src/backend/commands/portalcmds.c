/*-------------------------------------------------------------------------
 *
 * portalcmds.c
 *	  影响门户（即 SQL 游标命令）的实用命令
 *
 * 注意：另见 tcop/pquery.c，它为 FE/BE 协议实现门户操作。
 * 本模块的一些操作使用 pquery.c。
 * 这两个模块都依赖 utils/mmgr/portalmem.c，后者控制
 * 门户的存储管理（但不在门户中运行任何查询）。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/portalcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "commands/portalcmds.h"
#include "executor/executor.h"
#include "executor/tstoreReceiver.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * PerformCursorOpen
 *		执行 SQL DECLARE CURSOR 命令。
 */
void
PerformCursorOpen(ParseState *pstate, DeclareCursorStmt *cstmt, ParamListInfo params,
				  bool isTopLevel)
{
	Query	   *query = castNode(Query, cstmt->query);
	List	   *rewritten;
	PlannedStmt *plan;
	Portal		portal;
	MemoryContext oldContext;
	char	   *queryString;

	/*
	 * 禁止空字符串游标名（与协议级未命名门户
	 * 冲突）。
	 */
	if (!cstmt->portalname || cstmt->portalname[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/*
	 * 若这是不可保持的游标，本语句必须已在
	 * 事务块内执行（否则它将没有任何
	 * 用户可见的效果）。
	 */
	if (!(cstmt->options & CURSOR_OPT_HOLD))
		RequireTransactionBlock(isTopLevel, "DECLARE CURSOR");
	else if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot create a cursor WITH HOLD within security-restricted operation")));

	/*
	 * 解析分析已完成，但我们仍须运行规则重写器。
	 * 我们不调用 AcquireRewriteLocks：假定查询要么直接
	 * 来自解析器，要么已由 plancache.c 获取了
	 * 合适的锁。
	 */
	rewritten = QueryRewrite(query);

	/* SELECT 重写后应该不多不少恰为一个查询 */
	if (list_length(rewritten) != 1)
		elog(ERROR, "non-SELECT statement in DECLARE CURSOR");

	query = linitial_node(Query, rewritten);

	if (query->commandType != CMD_SELECT)
		elog(ERROR, "non-SELECT statement in DECLARE CURSOR");

	/* 规划查询，应用指定选项 */
	plan = pg_plan_query(query, pstate->p_sourcetext, cstmt->options, params);

	/*
	 * 创建门户，并把计划和查询字符串复制进它的内存。
	 */
	portal = CreatePortal(cstmt->portalname, false, false);

	oldContext = MemoryContextSwitchTo(portal->portalContext);

	plan = copyObject(plan);

	queryString = pstrdup(pstate->p_sourcetext);

	PortalDefineQuery(portal,
					  NULL,
					  queryString,
					  CMDTAG_SELECT,	/* 游标的查询总是 SELECT */
					  list_make1(plan),
					  NULL);

	/*----------
	 * 还要把外部门户的参数列表复制到内部门户的
	 * 内存上下文中。我们希望把参数值传下去，以防有
	 * 这样的命令：
	 *		DECLARE c CURSOR FOR SELECT ... WHERE foo = $1
	 * 该命令将使用外部参数集解析，参数值需要
	 * 保留下来，以便游标执行时
	 * 使用。
	 *----------
	 */
	params = copyParamList(params);

	MemoryContextSwitchTo(oldContext);

	/*
	 * 设置门户的选项。
	 *
	 * 若用户未指定 SCROLL 类型，就根据滚动是否需要
	 * 额外的运行时开销来决定允许或禁止滚动。
	 * 另外，我们禁止 FOR UPDATE 游标滚动。
	 */
	portal->cursorOptions = cstmt->options;
	if (!(portal->cursorOptions & (CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL)))
	{
		if (plan->rowMarks == NIL &&
			ExecSupportsBackwardScan(plan->planTree))
			portal->cursorOptions |= CURSOR_OPT_SCROLL;
		else
			portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;
	}

	/*
	 * 开始执行，若有参数就插入。
	 */
	PortalStart(portal, params, 0, GetActiveSnapshot());

	Assert(portal->strategy == PORTAL_ONE_SELECT);

	/*
	 * 我们完成了；查询实际上要到 PerformPortalFetch
	 * 被调用时才会运行。
	 */
}

/*
 * PerformPortalFetch
 *		执行 SQL FETCH 或 MOVE 命令。
 *
 *	stmt: 命令的解析树节点
 *	dest: 结果发送到哪里
 *	qc: 在哪里存储命令完成状态数据。
 *
 * 若调用方不想要状态数据，qc 可为 NULL。
 */
void
PerformPortalFetch(FetchStmt *stmt,
				   DestReceiver *dest,
				   QueryCompletion *qc)
{
	Portal		portal;
	uint64		nprocessed;

	/*
	 * 禁止空字符串游标名（与协议级未命名门户
	 * 冲突）。
	 */
	if (!stmt->portalname || stmt->portalname[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/* 从门户名获取门户 */
	portal = GetPortalByName(stmt->portalname);
	if (!PortalIsValid(portal))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", stmt->portalname)));
		return;					/* 让编译器满意 */
	}

	/* 如有需要调整 dest。MOVE 想要目的地 DestNone */
	if (stmt->ismove)
		dest = None_Receiver;

	/* 执行 */
	nprocessed = PortalRunFetch(portal,
								stmt->direction,
								stmt->howMany,
								dest);

	/* 若需要就返回命令状态 */
	if (qc)
		SetQueryCompletion(qc, stmt->ismove ? CMDTAG_MOVE : CMDTAG_FETCH,
						   nprocessed);
}

/*
 * PerformPortalClose
 *		关闭游标。
 */
void
PerformPortalClose(const char *name)
{
	Portal		portal;

	/* NULL 表示 CLOSE ALL */
	if (name == NULL)
	{
		PortalHashTableDeleteAll();
		return;
	}

	/*
	 * 禁止空字符串游标名（与协议级未命名门户
	 * 冲突）。
	 */
	if (name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/*
	 * 从门户名获取门户
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", name)));
		return;					/* 让编译器满意 */
	}

	/*
	 * 注意：若尚未完成，PortalCleanup 会作为副作用被调用。
	 */
	PortalDrop(portal, false);
}

/*
 * PortalCleanup
 *
 * 门户被丢弃时清理它。这是门户的标准
 * 清理钩子。
 *
 * 注意：若 portal->status 是 PORTAL_FAILED，我们很可能是在
 * 错误中止期间被调用的，必须小心避免做任何
 * 可能再次失败的事情。
 */
void
PortalCleanup(Portal portal)
{
	QueryDesc  *queryDesc;

	/*
	 * 健全性检查
	 */
	Assert(PortalIsValid(portal));
	Assert(portal->cleanup == PortalCleanup);

	/*
	 * 若执行器仍在运行就关闭它。错误中止期间
	 * 我们跳过这一步，因为其他机制会负责释放
	 * 执行器资源，而且我们无法确定 ExecutorEnd 本身不会失败。
	 */
	queryDesc = portal->queryDesc;
	if (queryDesc)
	{
		/*
		 * 先把 queryDesc 重置，再做其他事。这防止我们
		 * 在下方出错的情况下两次关闭执行器。
		 * 事务中止机制会在那种情况下负责
		 * 资源清理。
		 */
		portal->queryDesc = NULL;

		if (portal->status != PORTAL_FAILED)
		{
			ResourceOwner saveResourceOwner;

			/* 我们必须把门户的资源所有者设为当前 */
			saveResourceOwner = CurrentResourceOwner;
			if (portal->resowner)
				CurrentResourceOwner = portal->resowner;

			ExecutorFinish(queryDesc);
			ExecutorEnd(queryDesc);
			FreeQueryDesc(queryDesc);

			CurrentResourceOwner = saveResourceOwner;
		}
	}
}

/*
 * PersistHoldablePortal
 *
 * 准备指定的 Portal，以便在当前事务之外访问它。
 * 当本函数返回时，对门户的所有后续访问必须
 * 通过 Tuplestore 完成（而不是调用
 * 执行器）。
 */
void
PersistHoldablePortal(Portal portal)
{
	QueryDesc  *queryDesc = portal->queryDesc;
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldcxt;

	/*
	 * 若我们在保存一个可保持的门户，最好处于最初
	 * 创建它的那个事务中。
	 */
	Assert(portal->createSubid != InvalidSubTransactionId);
	Assert(queryDesc != NULL);

	/*
	 * 调用方一定已经创建了 tuplestore …… 但没有快照。
	 */
	Assert(portal->holdContext != NULL);
	Assert(portal->holdStore != NULL);
	Assert(portal->holdSnapshot == NULL);

	/*
	 * 关闭执行器之前，我们必须把 tupdesc 复制到
	 * 长期内存中，因为它是在执行器内存中创建的。
	 */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	portal->tupDesc = CreateTupleDescCopy(portal->tupDesc);

	MemoryContextSwitchTo(oldcxt);

	/*
	 * 检查门户使用是否不当，并把门户标记为活动。
	 */
	MarkPortalActive(portal);

	/*
	 * 设置全局门户上下文指针。
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ScanDirection direction = ForwardScanDirection;

		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		MemoryContextSwitchTo(PortalContext);

		PushActiveSnapshot(queryDesc->snapshot);

		/*
		 * 若门户被标记为可滚动，我们需要把整个结果集
		 * 存入 tuplestore，以便后续的向后 FETCH
		 * 能够被处理。否则，只存储尚未取出的行。
		 * （后者不仅更高效，而且若查询的输出不稳定，
		 * 还能避免语义问题。）
		 *
		 * 在不可滚动的情况下，tuplestore 中的元组索引不会
		 * 匹配游标的标称位置（portalPos）。目前这不会
		 * 造成困难，因为我们只按相对位置在 tuplestore 中
		 * 导航，除了下面的 tuplestore_skiptuples 调用和
		 * DoPortalRewind 中的 tuplestore_rescan 调用，这两个调用
		 * 对不可滚动游标都被禁用。但有一天我们可能
		 * 需要显式跟踪 holdStore 与游标标称位置
		 * 之间的偏移。
		 */
		if (portal->cursorOptions & CURSOR_OPT_SCROLL)
		{
			ExecutorRewind(queryDesc);
		}
		else
		{
			/*
			 * 若我们已到达查询末尾，就把方向设为 NoMovement，
			 * 避免再取任何元组。（做这个检查是因为并非
			 * 所有计划节点类型在已返回过 NULL 一次后
			 * 都能稳健地再次被调用。）不过我们仍会
			 * 设置一个空 tuplestore，免得后面把它
			 * 当成特殊情况。
			 */
			if (portal->atEnd)
				direction = NoMovementScanDirection;
		}

		/*
		 * 把目的地改为输出到 tuplestore。注意我们让 tuplestore
		 * 接收器对所有经过它的数据做 detoast；这样不为数据
		 * 保留关联快照也安全。
		 */
		queryDesc->dest = CreateDestReceiver(DestTuplestore);
		SetTuplestoreDestReceiverParams(queryDesc->dest,
										portal->holdStore,
										portal->holdContext,
										true,
										NULL,
										NULL);

		/* 把结果集取入 tuplestore */
		ExecutorRun(queryDesc, direction, 0, false);

		queryDesc->dest->rDestroy(queryDesc->dest);
		queryDesc->dest = NULL;

		/*
		 * 现在关闭内部执行器。
		 */
		portal->queryDesc = NULL;	/* 防止二次关闭 */
		ExecutorFinish(queryDesc);
		ExecutorEnd(queryDesc);
		FreeQueryDesc(queryDesc);

		/*
		 * 设置结果集中的位置。
		 */
		MemoryContextSwitchTo(portal->holdContext);

		if (portal->atEnd)
		{
			/*
			 * 直接强制 tuplestore 前进到末尾。这里
			 * 跳过请求的大小是任意的。
			 */
			while (tuplestore_skiptuples(portal->holdStore, 1000000, true))
				 /* 继续 */ ;
		}
		else
		{
			tuplestore_rescan(portal->holdStore);

			/*
			 * 在不可滚动的情况下，tuplestore 的起点正是
			 * 我们想要的位置，所以不需要重新定位。
			 */
			if (portal->cursorOptions & CURSOR_OPT_SCROLL)
			{
				if (!tuplestore_skiptuples(portal->holdStore,
										   portal->portalPos,
										   true))
					elog(ERROR, "unexpected end of tuple stream");
			}
		}
	}
	PG_CATCH();
	{
		/* 执行门户时出现未捕获的错误：把它标记为死亡 */
		MarkPortalFailed(portal);

		/* 恢复全局变量并传播错误 */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);

	/* 把门户标记为不活动 */
	portal->status = PORTAL_READY;

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	PopActiveSnapshot();

	/*
	 * 现在可以释放门户上下文的任何附属内存；我们
	 * 再也不会用到它。执行器已经丢弃了它的上下文，
	 * 但这样会清理任何经由 PortalContext
	 * 吸附到门户上下文上的东西。
	 */
	MemoryContextDeleteChildren(portal->portalContext);
}
