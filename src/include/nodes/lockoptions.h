/*-------------------------------------------------------------------------
 *
 * lockoptions.h
 *	若干加锁相关声明的公共头。
 *
 *
 * Copyright (c) 2014-2023, PostgreSQL Global Development Group
 *
 * src/include/nodes/lockoptions.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCKOPTIONS_H
#define LOCKOPTIONS_H

/*
 * 本枚举表示 FOR UPDATE/SHARE 子句的不同强度。
 * 此处顺序重要,因 RTE 被多种方式指定时取数值最大者。
 * 见 applyLockingClause。
 */
typedef enum LockClauseStrength
{
	LCS_NONE,					/* 无此子句 - 仅用于 PlanRowMark */
	LCS_FORKEYSHARE,			/* FOR KEY SHARE */
	LCS_FORSHARE,				/* FOR SHARE */
	LCS_FORNOKEYUPDATE,			/* FOR NO KEY UPDATE */
	LCS_FORUPDATE				/* FOR UPDATE */
} LockClauseStrength;

/*
 * 本枚举控制如何处理被 FOR UPDATE/SHARE 子句锁定的行
 * （即表示 NOWAIT 与 SKIP LOCKED 选项）。
 * 此处顺序重要,因 RTE 被多种方式指定时取数值最大者。
 * 见 applyLockingClause。
 */
typedef enum LockWaitPolicy
{
	/* 等待锁可用（默认行为） */
	LockWaitBlock,
	/* 跳过无法加锁的行 (SKIP LOCKED) */
	LockWaitSkip,
	/* 无法加锁则报错 (NOWAIT) */
	LockWaitError
} LockWaitPolicy;

/*
 * 元组的可能锁模式。
 */
typedef enum LockTupleMode
{
	/* SELECT FOR KEY SHARE */
	LockTupleKeyShare,
	/* SELECT FOR SHARE */
	LockTupleShare,
	/* SELECT FOR NO KEY UPDATE，及不改键列的 UPDATE */
	LockTupleNoKeyExclusive,
	/* SELECT FOR UPDATE、改键列的 UPDATE，以及 DELETE */
	LockTupleExclusive
} LockTupleMode;

#endif							/* LOCKOPTIONS_H */
