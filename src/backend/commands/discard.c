/*-------------------------------------------------------------------------
 *
 * discard.c
 *	  DISCARD 命令的实现
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/discard.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/async.h"
#include "commands/discard.h"
#include "commands/prepare.h"
#include "commands/sequence.h"
#include "utils/guc.h"
#include "utils/portal.h"

static void DiscardAll(bool isTopLevel);

/*
 * DISCARD { ALL | SEQUENCES | TEMP | PLANS }
 */
void
DiscardCommand(DiscardStmt *stmt, bool isTopLevel)
{
	switch (stmt->target)
	{
		case DISCARD_ALL:
			DiscardAll(isTopLevel);
			break;

		case DISCARD_PLANS:
			ResetPlanCache();
			break;

		case DISCARD_SEQUENCES:
			ResetSequenceCaches();
			break;

		case DISCARD_TEMP:
			ResetTempTableNamespace();
			break;

		default:
			elog(ERROR, "unrecognized DISCARD target: %d", stmt->target);
	}
}

static void
DiscardAll(bool isTopLevel)
{
	/*
	 * 禁止在事务块中使用 DISCARD ALL。这可以说是不一致的
	 * （我们没有在与 DISCARD ALL 等效的那串命令中进行类似的检查），
	 * 但这样做的目的是为了捕捉错误：
	 * 在事务块中使用 DISCARD ALL 会让事务
	 * 仍然处于未提交状态。
	 */
	PreventInTransactionBlock(isTopLevel, "DISCARD ALL");

	/* 关闭门户可能会运行用户定义的代码，因此先做这件事。 */
	PortalHashTableDeleteAll();
	SetPGVariable("session_authorization", NIL, false);
	ResetAllOptions();
	DropAllPreparedStatements();
	Async_UnlistenAll();
	LockReleaseAll(USER_LOCKMETHOD, true);
	ResetPlanCache();
	ResetTempTableNamespace();
	ResetSequenceCaches();
}
