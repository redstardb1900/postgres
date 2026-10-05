/*-------------------------------------------------------------------------
 *
 * sinval.c
 *	  POSTGRES 共享缓存失效通信代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/sinval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "nodes/memnodes.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinvaladt.h"
#include "utils/inval.h"


uint64		SharedInvalidMessageCounter;


/*
 * 空闲后端不会读取 sinval 事件，因此需要
 * 一种办法迅速督促空闲后端跟上，以免 sinval
 * 队列溢出并迫使它走一遍缓存重置。做法是向
 * 任何落后过多的后端发送
 * PROCSIG_CATCHUP_INTERRUPT。
 *
 * 信号处理函数会设置一个中断待处理标志，并设置该进程的
 * 锁存器。每当开始从客户端读取，或在读取过程中被打断时，
 * ProcessClientReadInterrupt() 都会调用
 * ProcessCatchupEvent()。
 */
volatile sig_atomic_t catchupInterruptPending = false;


/*
 * SendSharedInvalidMessages
 *	把共享缓存失效消息（message(s)）加入全局 SI 消息队列。
 */
void
SendSharedInvalidMessages(const SharedInvalidationMessage *msgs, int n)
{
	SIInsertDataEntries(msgs, n);
}

/*
 * ReceiveSharedInvalidMessages
 *		处理正在等待本后端的共享缓存失效消息
 *
 * 我们保证处理进入本例程之前已经入队的全部消息。
 * 当然，在最后一次 SIGetDataEntries 调用之后，
 * 仍可能马上又有更多消息入队。
 *
 * 注意：本例程完全可能被递归调用，作为 invalFunction
 * 或 resetFunction 内部处理的结果。而且，这样的递归调用
 * 在退出前必须保证所有未处理的失效消息都已处理完。
 * 这就是为何看起来奇怪地选用静态分配的
 * 缓冲区数组
 * 和计数器；这样递归调用才能处理
 * 已经从 sinvaladt.c 抽出来的消息。
 */
void
ReceiveSharedInvalidMessages(void (*invalFunction) (SharedInvalidationMessage *msg),
							 void (*resetFunction) (void))
{
#define MAXINVALMSGS 32
	static SharedInvalidationMessage messages[MAXINVALMSGS];

	/*
	 * 这里使用 volatile，以防编译器没有意识到
	 * 递归是可能的 ...
	 */
	static volatile int nextmsg = 0;
	static volatile int nummsgs = 0;

	/* 处理外层递归中仍然待处理的消息 */
	while (nextmsg < nummsgs)
	{
		SharedInvalidationMessage msg = messages[nextmsg++];

		SharedInvalidMessageCounter++;
		invalFunction(&msg);
	}

	do
	{
		int			getResult;

		nextmsg = nummsgs = 0;

		/* 尝试再取一些消息 */
		getResult = SIGetDataEntries(messages, MAXINVALMSGS);

		if (getResult < 0)
		{
			/* 收到了重置消息 */
			elog(DEBUG4, "cache state reset");
			SharedInvalidMessageCounter++;
			resetFunction();
			break;				/* 没有更多事情要做 */
		}

		/* 处理它们，同时当心递归调用可能吃掉一部分 */
		nextmsg = 0;
		nummsgs = getResult;

		while (nextmsg < nummsgs)
		{
			SharedInvalidationMessage msg = messages[nextmsg++];

			SharedInvalidMessageCounter++;
			invalFunction(&msg);
		}

		/*
		 * 只有当最后一次 SIGetDataEntries 调用（可能发生在
		 * 递归调用内部）返回了满的缓冲区时，才需要循环。
		 */
	} while (nummsgs == MAXINVALMSGS);

	/*
	 * 现在已经追上。若收到了追赶信号，就复位该标志，
	 * 并调用 SICleanupQueue()。这不太是因为此刻需要
	 * 冲掉死消息，而是因为我们想把追赶信号传给下一个
	 * 最慢的后端。这样把追赶信号 "逐级传递" 下去，
	 * 可以避免为本来只是后台维护活动的事情
	 * 造成系统负载尖峰。
	 */
	if (catchupInterruptPending)
	{
		catchupInterruptPending = false;
		elog(DEBUG4, "sinval catchup complete, cleaning queue");
		SICleanupQueue(false, 0);
	}
}


/*
 * HandleCatchupInterrupt
 *
 * 收到 PROCSIG_CATCHUP_INTERRUPT 时调用本函数。
 *
 * 过去空闲时会直接调用 ProcessCatchupEvent。现在只是
 * 设置一个标志以便稍后处理，并通过设置该进程的锁存器
 * 把这一事实通知该进程。
 */
void
HandleCatchupInterrupt(void)
{
	/*
	 * 注意：本函数由 SIGNAL HANDLER 调用。这里做什么
	 * 都必须非常小心。
	 */

	catchupInterruptPending = true;

	/* 确保该事件会在适当的时候得到处理 */
	SetLatch(MyLatch);
}

/*
 * ProcessCatchupInterrupt
 *
 * 追赶中断处理中运行在信号处理函数之外的那一部分，
 * 因此它实际上可以处理待处理的失效消息。
 */
void
ProcessCatchupInterrupt(void)
{
	while (catchupInterruptPending)
	{
		/*
		 * 这里要做的是让 ReceiveSharedInvalidMessages()
		 * 运行，它会完成必要的工作，并复位
		 * catchupInterruptPending 标志。若我们正处于事务中，
		 * 直接调用 AcceptInvalidationMessages() 即可。若不是，
		 * 就启动并立即结束一个事务；对 AcceptInvalidationMessages()
		 * 的调用发生在事务启动的内部。这样做时务必保留调用方的
		 * 内存上下文。
		 *
		 * 只调用 AcceptInvalidationMessages()、省掉其余的
		 * 事务启动/停止开销非常诱人，我认为正常情况下
		 * 那样其实能工作；但我不确定中途出错时
		 * 清理是否会干净。
		 */
		if (IsTransactionOrTransactionBlock())
		{
			elog(DEBUG4, "ProcessCatchupEvent inside transaction");
			AcceptInvalidationMessages();
		}
		else
		{
			MemoryContext oldcontext = CurrentMemoryContext;

			elog(DEBUG4, "ProcessCatchupEvent outside transaction");
			StartTransactionCommand();
			CommitTransactionCommand();
			/* 调用方的上下文最好不是事务局部的 */
			Assert(MemoryContextIsValid(oldcontext));
			MemoryContextSwitchTo(oldcontext);
		}
	}
}
