/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  进程间锁存器例程
 *
 * 锁存器是布尔变量，其操作让进程睡眠
 * 直到它被置位。可由另一进程置位，或由同一进程内
 * 的信号处理函数置位。
 *
 * 锁存器接口可可靠替代常见模式：用
 * pg_usleep() 或 select() 等到信号到来，由
 * 信号处理函数设置标志变量。因为有些平台上
 * 到来的信号并不打断睡眠，即便在会打断的平台上，
 * 若信号恰在进入睡眠前到达也有竞态，
 * 故常见模式必须定期醒来并轮询
 * 标志变量。pselect() 系统调用就是为解决
 * 此问题而发明的，但可移植性不够。锁存器旨在
 * 克服这些限制，让你无需轮询即可睡眠，并
 * 确保能迅速响应其他进程的信号。
 *
 * 锁存器分两种：本地与共享。本地锁存器由
 * InitLatch 初始化，且只能由同一进程置位。
 * 本地锁存器可用于等待信号到达，做法是在
 * 信号处理函数中调用 SetLatch。共享锁存器位于共享内存
 * 且必须在 postmaster 启动时由 InitSharedLatch 初始化。在
 * 共享锁存器可被等待前，必须用 OwnLatch 把它与某
 * 进程关联。仅拥有该锁存器者能等待，但任何进程
 * 都能置位它。
 *
 * 锁存器有三种基本操作：
 *
 * SetLatch	- 置位锁存器
 * ResetLatch	- 清除锁存器，使其可再被置位
 * WaitLatch	- 等待锁存器被置位
 *
 * WaitLatch 提供超时机制（应尽量避免，
 * 因其带来额外开销），并让
 * postmaster 子进程在 postmaster 死亡时立即醒来。
 * 导出函数的详细规格见 latch.c。
 *
 * 等待 event(s) 的正确模式是：
 *
 * for (;;)
 * {
 *	   ResetLatch();
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitLatch();
 * }
 *
 * 必须在检查是否有工作*之前*复位锁存器。
 * 否则，若有人在检查与 ResetLatch 调用之间置位
 * 锁存器，你会错过它，Wait 会错误地阻塞。
 *
 * 另一种有效编码模式如下：
 *
 * for (;;)
 * {
 *	   if (work to do)
 *		   Do Stuff(); // in particular, exit loop if some condition satisfied
 *	   WaitLatch();
 *	   ResetLatch();
 * }
 *
 * 若预期循环的终止条件常在首次迭代就满足，
 * 这有助于减少锁存器流量；不满足时的代价是
 * 阻塞前多一轮循环。
 * 切勿在 WaitLatch 之后、ResetLatch 之前放置任何
 * 异步事件检查，因为那会制造竞态。
 *
 * 要唤醒等待者，必须先设置全局标志或
 * 等待循环在 "if (work to do)" 部分测试的其他东西，并
 * 在那*之后*调用 SetLatch。锁存器已置位时 SetLatch
 * 迅速返回。
 *
 * 在某些平台上，信号本身不会打断锁存器等待原语。
 * 因此，任何意在结束 WaitLatch 等待的信号处理函数
 * 必须调用 SetLatch。
 *
 * 注意：用进程锁存器（PGPROC.procLatch）给辅助进程发信号
 * 通常优于临时共享锁存器。因为
 * 通用信号处理函数只会在进程锁存器上调用
 * SetLatch，故用进程锁存器以外的锁存器实际上排除了使用
 * 任何通用处理函数。
 *
 *
 * WaitEventSets 允许同时等待锁存器被置位及其他事件——
 * 目前是 postmaster 死亡与若干套接字就绪。
 * 在许多平台上，使用长寿命事件集比
 * WaitLatch 或 WaitLatchOrSocket 更高效。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/latch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LATCH_H
#define LATCH_H

#include <signal.h>

/*
 * Latch 结构应视为不透明，仅通过
 * 公开函数访问。在此定义以便把 Latches 嵌入
 * 更大的结构体。
 */
typedef struct Latch
{
	sig_atomic_t is_set;
	sig_atomic_t maybe_sleeping;
	bool		is_shared;
	int			owner_pid;
#ifdef WIN32
	HANDLE		event;
#endif
} Latch;

/*
 * 可唤醒 WaitLatch()、WaitLatchOrSocket() 或 WaitEventSetWait()
 * 之位掩码。
 */
#define WL_LATCH_SET		 (1 << 0)
#define WL_SOCKET_READABLE	 (1 << 1)
#define WL_SOCKET_WRITEABLE  (1 << 2)
#define WL_TIMEOUT			 (1 << 3)	/* 非 WaitEventSetWait() 用 */
#define WL_POSTMASTER_DEATH  (1 << 4)
#define WL_EXIT_ON_PM_DEATH	 (1 << 5)
#ifdef WIN32
#define WL_SOCKET_CONNECTED  (1 << 6)
#else
/* 避免在不需要的平台上处理该情形 */
#define WL_SOCKET_CONNECTED  WL_SOCKET_WRITEABLE
#endif
#define WL_SOCKET_CLOSED 	 (1 << 7)
#ifdef WIN32
#define WL_SOCKET_ACCEPT	 (1 << 8)
#else
/* 避免在不需要的平台上处理该情形 */
#define WL_SOCKET_ACCEPT	WL_SOCKET_READABLE
#endif
#define WL_SOCKET_MASK		(WL_SOCKET_READABLE | \
							 WL_SOCKET_WRITEABLE | \
							 WL_SOCKET_CONNECTED | \
							 WL_SOCKET_ACCEPT | \
							 WL_SOCKET_CLOSED)

typedef struct WaitEvent
{
	int			pos;			/* 在事件数据结构中的位置 */
	uint32		events;			/* 触发的事件 */
	pgsocket	fd;				/* 与事件关联的套接字 fd */
	void	   *user_data;		/* AddWaitEventToSet 中提供的指针 */
#ifdef WIN32
	bool		reset;			/* 是否需要复位该事件？ */
#endif
} WaitEvent;

/* 前向声明，以免暴露 latch.c 实现细节 */
typedef struct WaitEventSet WaitEventSet;

/*
 * latch.c 中函数的原型
 */
extern void InitializeLatchSupport(void);
extern void InitLatch(Latch *latch);
extern void InitSharedLatch(Latch *latch);
extern void OwnLatch(Latch *latch);
extern void DisownLatch(Latch *latch);
extern void SetLatch(Latch *latch);
extern void ResetLatch(Latch *latch);
extern void ShutdownLatchSupport(void);

extern WaitEventSet *CreateWaitEventSet(MemoryContext context, int nevents);
extern void FreeWaitEventSet(WaitEventSet *set);
extern void FreeWaitEventSetAfterFork(WaitEventSet *set);
extern int	AddWaitEventToSet(WaitEventSet *set, uint32 events, pgsocket fd,
							  Latch *latch, void *user_data);
extern void ModifyWaitEvent(WaitEventSet *set, int pos, uint32 events, Latch *latch);

extern int	WaitEventSetWait(WaitEventSet *set, long timeout,
							 WaitEvent *occurred_events, int nevents,
							 uint32 wait_event_info);
extern int	WaitLatch(Latch *latch, int wakeEvents, long timeout,
					  uint32 wait_event_info);
extern int	WaitLatchOrSocket(Latch *latch, int wakeEvents,
							  pgsocket sock, long timeout, uint32 wait_event_info);
extern void InitializeLatchWaitSet(void);
extern int	GetNumRegisteredWaitEvents(WaitEventSet *set);
extern bool WaitEventSetCanReportClosed(void);

#endif							/* LATCH_H */
