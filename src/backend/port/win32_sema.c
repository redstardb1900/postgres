/*-------------------------------------------------------------------------
 *
 * win32_sema.c
 *	  Microsoft Windows Win32 信号量仿真
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32_sema.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"

static HANDLE *mySemSet;		/* 已获取的信号量集 ID */
static int	numSems;			/* 已获取的信号量集个数 */
static int	maxSems;			/* mySemaSet 数组已分配大小 */

static void ReleaseSemaphores(int code, Datum arg);


/*
 * 报告信号量所需的共享内存空间
 */
Size
PGSemaphoreShmemSize(int maxSemas)
{
	/* Windows 上不需要共享内存 */
	return 0;
}

/*
 * PGReserveSemaphores --- 初始化信号量支持
 *
 * Win32 实现按需获取信号量；maxSemas 仅用于
 * 确定跟踪已获取信号量以便随后释放的数组大小。
 * 使用匿名信号量，因而最后一个引用进程退出时
 * 信号量会自动释放。
 *
 */
void
PGReserveSemaphores(int maxSemas)
{
	mySemSet = (HANDLE *) malloc(maxSemas * sizeof(HANDLE));
	if (mySemSet == NULL)
		elog(PANIC, "out of memory");
	numSems = 0;
	maxSems = maxSemas;

	on_shmem_exit(ReleaseSemaphores, 0);
}

/*
 * 关机或共享内存重初始化时释放信号量
 *
 * 注：作为 on_shmem_exit 回调，故参数列表古怪
 */
static void
ReleaseSemaphores(int code, Datum arg)
{
	int			i;

	for (i = 0; i < numSems; i++)
		CloseHandle(mySemSet[i]);
	free(mySemSet);
}

/*
 * PGSemaphoreCreate
 *
 * 分配初始计数为 1 的 PGSemaphore 结构
 */
PGSemaphore
PGSemaphoreCreate(void)
{
	HANDLE		cur_handle;
	SECURITY_ATTRIBUTES sec_attrs;

	/* 不能在 backend 中做：静态状态属 postmaster */
	Assert(!IsUnderPostmaster);

	if (numSems >= maxSems)
		elog(PANIC, "too many semaphores created");

	ZeroMemory(&sec_attrs, sizeof(sec_attrs));
	sec_attrs.nLength = sizeof(sec_attrs);
	sec_attrs.lpSecurityDescriptor = NULL;
	sec_attrs.bInheritHandle = TRUE;

	/* 不需要命名信号量 */
	cur_handle = CreateSemaphore(&sec_attrs, 1, 32767, NULL);
	if (cur_handle)
	{
		/* 成功完成 */
		mySemSet[numSems++] = cur_handle;
	}
	else
		ereport(PANIC,
				(errmsg("could not create semaphore: error code %lu",
						GetLastError())));

	return (PGSemaphore) cur_handle;
}

/*
 * PGSemaphoreReset
 *
 * 把已初始化的 PGSemaphore 复位为计数 0
 */
void
PGSemaphoreReset(PGSemaphore sema)
{
	/*
	 * Win32 无直接 API，只好反复 tryLock 把
	 * 信号量递减到 0。
	 */
	while (PGSemaphoreTryLock(sema))
		 /* loop */ ;
}

/*
 * PGSemaphoreLock
 *
 * 锁定信号量（减计数），若计数将 < 0 则阻塞。
 */
void
PGSemaphoreLock(PGSemaphore sema)
{
	HANDLE		wh[2];
	bool		done = false;

	/*
	 * 注意：pgwin32_signal_event 应排在最前，以确保
	 * 多事件同时置位时仍会报告它。我们要保证
	 * 挂起信号得到处理。
	 */
	wh[0] = pgwin32_signal_event;
	wh[1] = sema;

	/*
	 * 与其他 PGSemaphoreLock 实现一样，每次循环须检查
	 * cancel/die 中断。但此处没有系统调用会暗中代为
	 * 处理信号的隐藏魔法——
	 * 我们自己处理信号。
	 */
	while (!done)
	{
		DWORD		rc;

		CHECK_FOR_INTERRUPTS();

		rc = WaitForMultipleObjectsEx(2, wh, FALSE, INFINITE, TRUE);
		switch (rc)
		{
			case WAIT_OBJECT_0:
				/* 信号事件已置位——有信号待投递 */
				pgwin32_dispatch_queued_signals();
				break;
			case WAIT_OBJECT_0 + 1:
				/* 拿到了! */
				done = true;
				break;
			case WAIT_IO_COMPLETION:

				/*
				 * 系统中断等待，在本线程执行 I/O
				 * 完成例程或异步过程调用。PostgreSQL 不
				 * 会引发这两者，但非典型加载的 DLL 乃至
				 * 其他进程也可能如此。
				 * 继续等待。
				 */
				break;
			case WAIT_FAILED:
				ereport(FATAL,
						(errmsg("could not lock semaphore: error code %lu",
								GetLastError())));
				break;
			default:
				elog(FATAL, "unexpected return code from WaitForMultipleObjectsEx(): %lu", rc);
				break;
		}
	}
}

/*
 * PGSemaphoreUnlock
 *
 * 解锁信号量（增计数）
 */
void
PGSemaphoreUnlock(PGSemaphore sema)
{
	if (!ReleaseSemaphore(sema, 1, NULL))
		ereport(FATAL,
				(errmsg("could not unlock semaphore: error code %lu",
						GetLastError())));
}

/*
 * PGSemaphoreTryLock
 *
 * 仅在能不阻塞时锁定信号量
 */
bool
PGSemaphoreTryLock(PGSemaphore sema)
{
	DWORD		ret;

	ret = WaitForSingleObject(sema, 0);

	if (ret == WAIT_OBJECT_0)
	{
		/* 拿到了! */
		return true;
	}
	else if (ret == WAIT_TIMEOUT)
	{
		/* 拿不到 */
		errno = EAGAIN;
		return false;
	}

	/* 否则就麻烦了 */
	ereport(FATAL,
			(errmsg("could not try-lock semaphore: error code %lu",
					GetLastError())));

	/* 让编译器安静 */
	return false;
}
