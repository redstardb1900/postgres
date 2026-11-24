/*-------------------------------------------------------------------------
 *
 * timer.c
 *	  Microsoft Windows Win32 定时器实现
 *
 *	  本实现的限制：
 *
 *	  - 不支持间隔定时器（value->it_interval）
 *	  - 仅支持 ITIMER_REAL
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32/timer.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"


/* 线程间通信区 */
typedef struct timerCA
{
	struct itimerval value;
	HANDLE		event;
	CRITICAL_SECTION crit_sec;
} timerCA;

static timerCA timerCommArea;
static HANDLE timerThreadHandle = INVALID_HANDLE_VALUE;


/* 定时器管理线程 */
static DWORD WINAPI
pg_timer_thread(LPVOID param)
{
	DWORD		waittime;

	Assert(param == NULL);

	waittime = INFINITE;

	for (;;)
	{
		int			r;

		r = WaitForSingleObjectEx(timerCommArea.event, waittime, FALSE);
		if (r == WAIT_OBJECT_0)
		{
			/* 主线程发信号的事件，用于改定时器 */
			EnterCriticalSection(&timerCommArea.crit_sec);
			if (timerCommArea.value.it_value.tv_sec == 0 &&
				timerCommArea.value.it_value.tv_usec == 0)
				waittime = INFINITE;	/* 取消中断 */
			else
			{
				/* WaitForSingleObjectEx() 用毫秒，向上取整 */
				waittime = (timerCommArea.value.it_value.tv_usec + 999) / 1000 +
					timerCommArea.value.it_value.tv_sec * 1000;
			}
			ResetEvent(timerCommArea.event);
			LeaveCriticalSection(&timerCommArea.crit_sec);
		}
		else if (r == WAIT_TIMEOUT)
		{
			/* 超时到期，发 SIGALRM 并关掉定时器 */
			pg_queue_signal(SIGALRM);
			waittime = INFINITE;
		}
		else
		{
			/* 不应发生 */
			Assert(false);
		}
	}

	return 0;
}

/*
 * 用持久线程仿真 Win32 setitimer，
 * 负责设定定时器并在超时通知。
 */
int
setitimer(int which, const struct itimerval *value, struct itimerval *ovalue)
{
	Assert(value != NULL);
	Assert(value->it_interval.tv_sec == 0 && value->it_interval.tv_usec == 0);
	Assert(which == ITIMER_REAL);

	if (timerThreadHandle == INVALID_HANDLE_VALUE)
	{
		/* 本 backend 首次调用：创建事件与定时器线程 */
		timerCommArea.event = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (timerCommArea.event == NULL)
			ereport(FATAL,
					(errmsg_internal("could not create timer event: error code %lu",
									 GetLastError())));

		MemSet(&timerCommArea.value, 0, sizeof(struct itimerval));

		InitializeCriticalSection(&timerCommArea.crit_sec);

		timerThreadHandle = CreateThread(NULL, 0, pg_timer_thread, NULL, 0, NULL);
		if (timerThreadHandle == INVALID_HANDLE_VALUE)
			ereport(FATAL,
					(errmsg_internal("could not create timer thread: error code %lu",
									 GetLastError())));
	}

	/* 请求定时器线程更改设置 */
	EnterCriticalSection(&timerCommArea.crit_sec);
	if (ovalue)
		*ovalue = timerCommArea.value;
	timerCommArea.value = *value;
	LeaveCriticalSection(&timerCommArea.crit_sec);
	SetEvent(timerCommArea.event);

	return 0;
}
