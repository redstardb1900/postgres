/*-------------------------------------------------------------------------
 *
 * timeout.h
 *	  为多种超时原因复用 SIGALRM 中断的例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/timeout.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMEOUT_H
#define TIMEOUT_H

#include "datatype/timestamp.h"

/*
 * 超时原因的标识符。若多种超时同时触发，按本
 * 枚举次序处理。
 */
typedef enum TimeoutId
{
	/* 预定义超时原因 */
	STARTUP_PACKET_TIMEOUT,
	DEADLOCK_TIMEOUT,
	LOCK_TIMEOUT,
	STATEMENT_TIMEOUT,
	STANDBY_DEADLOCK_TIMEOUT,
	STANDBY_TIMEOUT,
	STANDBY_LOCK_TIMEOUT,
	IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
	IDLE_SESSION_TIMEOUT,
	IDLE_STATS_UPDATE_TIMEOUT,
	CLIENT_CONNECTION_CHECK_TIMEOUT,
	STARTUP_PROGRESS_TIMEOUT,
	/* 首个用户可定义超时原因 */
	USER_TIMEOUT,
	/* 超时原因的最大数目 */
	MAX_TIMEOUTS = USER_TIMEOUT + 10
} TimeoutId;

/* 回调函数签名 */
typedef void (*timeout_handler_proc) (void);

/*
 * 一次设置多个超时的参数结构
 */
typedef enum TimeoutType
{
	TMPARAM_AFTER,
	TMPARAM_AT,
	TMPARAM_EVERY
} TimeoutType;

typedef struct
{
	TimeoutId	id;				/* 待设超时 */
	TimeoutType type;			/* TMPARAM_AFTER/TMPARAM_AT */
	int			delay_ms;		/* 仅用于 TMPARAM_AFTER/EVERY */
	TimestampTz fin_time;		/* 仅用于 TMPARAM_AT */
} EnableTimeoutParams;

/*
 * 一次清除多个超时的参数结构
 */
typedef struct
{
	TimeoutId	id;				/* 待清超时 */
	bool		keep_indicator; /* 保留指示标志？ */
} DisableTimeoutParams;

/* 超时设置 */
extern void InitializeTimeouts(void);
extern TimeoutId RegisterTimeout(TimeoutId id, timeout_handler_proc handler);
extern void reschedule_timeouts(void);

/* 超时操作 */
extern void enable_timeout_after(TimeoutId id, int delay_ms);
extern void enable_timeout_every(TimeoutId id, TimestampTz fin_time,
								 int delay_ms);
extern void enable_timeout_at(TimeoutId id, TimestampTz fin_time);
extern void enable_timeouts(const EnableTimeoutParams *timeouts, int count);
extern void disable_timeout(TimeoutId id, bool keep_indicator);
extern void disable_timeouts(const DisableTimeoutParams *timeouts, int count);
extern void disable_all_timeouts(bool keep_indicators);

/* 访问器 */
extern bool get_timeout_active(TimeoutId id);
extern bool get_timeout_indicator(TimeoutId id, bool reset_indicator);
extern TimestampTz get_timeout_start_time(TimeoutId id);
extern TimestampTz get_timeout_finish_time(TimeoutId id);

#endif							/* TIMEOUT_H */
