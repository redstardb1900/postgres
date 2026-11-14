/*-------------------------------------------------------------------------
 *
 * timeout.c
 *	  Routines to multiplex SIGALRM interrupts for multiple timeout reasons.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/timeout.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h>

#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"


/* 任一超时原因的数据 */
typedef struct timeout_params
{
	TimeoutId	index;			/* 超时原因的标识符 */

	/* volatile 因为信号处理器可能改动它们 */
	volatile bool active;		/* 在 active_timeouts[] 中则为 true */
	volatile bool indicator;	/* 超时已发生则为 true */

	/* 超时的回调函数，超时未注册时为 NULL */
	timeout_handler_proc timeout_handler;

	TimestampTz start_time;		/* 超时上次被激活的时间 */
	TimestampTz fin_time;		/* 当前或上次应触发的时间 */
	int			interval_in_ms; /* 触发间隔，仅一次则为 0 */
} timeout_params;

/*
 * 按 enum TimeoutId 顺序排列的可能超时原因列表。
 */
static timeout_params all_timeouts[MAX_TIMEOUTS];
static bool all_timeouts_initialized = false;

/*
 * 按 fin_time 和优先级排序的活动超时列表。
 * 该列表可能被中断处理器改动，因此它是 volatile。
 */
static volatile int num_active_timeouts = 0;
static timeout_params *volatile active_timeouts[MAX_TIMEOUTS];

/*
 * 控制信号处理器是否允许做任何事的标志。
 * 这有助于避免与处理器之间的竞态条件。特别
 * 是它让我们无需繁琐地禁用再重新启用定时器
 * 信号就能修改数据结构。大多数情况下，这些
 * 临界区内本来不会发生中断，但若发生了，
 * 这条规则也保证安全。让信号跨多个操作保持
 * 启用，可大幅减少我们发起内核调用的次数，
 * 详见 schedule_alarm() 中的注释。
 *
 * 我们不期望中断时把它留为 "false"，以防万一。
 */
static volatile sig_atomic_t alarm_enabled = false;

#define disable_alarm() (alarm_enabled = false)
#define enable_alarm()	(alarm_enabled = true)

/*
 * 记录我们下次预期中断何时触发的状态。
 * （signal_pending 为 true 时 signal_due_at 有效。）
 * 注意信号处理器会无条件地把 signal_pending 重置
 * 为 false，故即使 alarm_enabled 为 false 它也会异步改变。
 */
static volatile sig_atomic_t signal_pending = false;
static volatile TimestampTz signal_due_at = 0;


/*****************************************************************************
 * 内部辅助函数
 *
 * 对所有这些函数，调用方有责任保护它们不
 * 被信号处理器中断。通常先调用 disable_alarm()
 * 以阻止中断，然后更新状态，最后调用
 * schedule_alarm()，它会在需要时重新启用信号处理器。
 *****************************************************************************/

/*
 * 在活动数组中找到给定超时原因的索引。
 * 不在其中则返回 -1。
 */
static int
find_active_timeout(TimeoutId id)
{
	int			i;

	for (i = 0; i < num_active_timeouts; i++)
	{
		if (active_timeouts[i]->index == id)
			return i;
	}

	return -1;
}

/*
 * 把指定的超时原因插入活动超时列表的
 * 给定索引处。
 */
static void
insert_timeout(TimeoutId id, int index)
{
	int			i;

	if (index < 0 || index > num_active_timeouts)
		elog(FATAL, "timeout index %d out of range 0..%d", index,
			 num_active_timeouts);

	Assert(!all_timeouts[id].active);
	all_timeouts[id].active = true;

	for (i = num_active_timeouts - 1; i >= index; i--)
		active_timeouts[i + 1] = active_timeouts[i];

	active_timeouts[index] = &all_timeouts[id];

	num_active_timeouts++;
}

/*
 * 从超时列表中移除第 index 个元素。
 */
static void
remove_timeout_index(int index)
{
	int			i;

	if (index < 0 || index >= num_active_timeouts)
		elog(FATAL, "timeout index %d out of range 0..%d", index,
			 num_active_timeouts - 1);

	Assert(active_timeouts[index]->active);
	active_timeouts[index]->active = false;

	for (i = index + 1; i < num_active_timeouts; i++)
		active_timeouts[i - 1] = active_timeouts[i];

	num_active_timeouts--;
}

/*
 * 启用指定的超时原因
 */
static void
enable_timeout(TimeoutId id, TimestampTz now, TimestampTz fin_time,
			   int interval_in_ms)
{
	int			i;

	/* 断言请求合理 */
	Assert(all_timeouts_initialized);
	Assert(all_timeouts[id].timeout_handler != NULL);

	/*
	 * 如果该超时已处于活动状态，先暂时禁用它。
	 * 该调用被理解为重新调度此超时的指令。
	 */
	if (all_timeouts[id].active)
		remove_timeout_index(find_active_timeout(id));

	/*
	 * 找出新超时的插入位置索引。我们按 fin_time
	 * 排序，fin_time 相同则按优先级。
	 */
	for (i = 0; i < num_active_timeouts; i++)
	{
		timeout_params *old_timeout = active_timeouts[i];

		if (fin_time < old_timeout->fin_time)
			break;
		if (fin_time == old_timeout->fin_time && id < old_timeout->index)
			break;
	}

	/*
	 * 把该超时标记为活动，并插入活动列表。
	 */
	all_timeouts[id].indicator = false;
	all_timeouts[id].start_time = now;
	all_timeouts[id].fin_time = fin_time;
	all_timeouts[id].interval_in_ms = interval_in_ms;

	insert_timeout(id, i);
}

/*
 * 为下一个活动超时调度报警（若有）
 *
 * 我们假定调用方已取得当前时间，或足够
 * 接近的近似值。（若距 "now" 已过了一两个节拍，
 * 或者在我们到达内核调用前又流逝了一点时间，
 * 都没关系；那只会让我们请求的中断比最近的
 * 超时晚一两个节拍而已，无关紧要。但传入未来的 "now"
 * 值就糟了。）
 */
static void
schedule_alarm(TimestampTz now)
{
	if (num_active_timeouts > 0)
	{
		struct itimerval timeval;
		TimestampTz nearest_timeout;
		long		secs;
		int			usecs;

		MemSet(&timeval, 0, sizeof(struct itimerval));

		/*
		 * 如果我们认为有信号待触发，但当前时间已比
		 * 信号应触发时间晚了 10ms 以上，则假定超时
		 * 请求以某种方式丢失了；清除 signal_pending 以便
		 * 在下面重新设置中断请求。（10ms 对应现代系统
		 * 上最坏情况的超时粒度。）即使中断碰巧在我
		 * 们到达 setitimer() 调用之前触发，我们也不会
		 * 受影响。
		 */
		if (signal_pending && now > signal_due_at + 10 * 1000)
			signal_pending = false;

		/*
		 * 获取到最近的待触发超时为止的剩余时间。若
		 * 为负，则假定我们错过了某次中断，并清除
		 * signal_pending。这样若内核因某种原因丢失了
		 * 超时请求，我们就还有机会恢复。
		 */
		nearest_timeout = active_timeouts[0]->fin_time;
		if (now > nearest_timeout)
		{
			signal_pending = false;
			/* 尽快强制产生一次中断 */
			secs = 0;
			usecs = 1;
		}
		else
		{
			TimestampDifference(now, nearest_timeout,
								&secs, &usecs);

			/*
			 * 差值有可能小于一微秒；
			 * 确保我们是在设置中断而非取消中断。
			 */
			if (secs == 0 && usecs == 0)
				usecs = 1;
		}

		timeval.it_value.tv_sec = secs;
		timeval.it_value.tv_usec = usecs;

		/*
		 * 我们必须在调用 setitimer() 之前启用信号处理器；
		 * 若顺序相反，就会有竞态条件：中断可能在
		 * 我们设置 alarm_enabled 之前发生，导致信号处理
		 * 器什么也做不了。
		 *
		 * 由于没在 disable_alarm() 中禁用定时器，
		 * 之前设置的定时器中断有可能恰在 enable_alarm()
		 * 与 setitimer() 之间触发。不过这是安全的。
		 * 有两种结果：
		 *
		 * 1. 信号处理器发现无事可做（因为最近的超时
		 * 事件尚在未来）。它会重设定时器并返回。
		 * 之后我们会用一个新值覆盖定时器值。这意味
		 * 着定时器触发得比我们预期的稍晚，但只晚
		 * 信号处理器做完无用功所需的那点时间，
		 * 应该不会太长。
		 *
		 * 2. 信号处理器执行并移除一个或多个超时
		 * 事件。它返回时，队列要么已空，要么最前
		 * 面的事件比我们上面看过的更晚。所以我们会
		 * 用一个过早（±信号处理器执行时间）的值覆盖
		 * 定时器值，从而导致一次无用的中断。
		 * 但处理器随后会重设定时器，一切仍会
		 * 按预期工作。
		 *
		 * 由于这些情况概率极低（这里的窗口非常
		 * 窄），不值得在主流程代码中耗费周期去阻止
		 * 偶发的浪费中断。
		 */
		enable_alarm();

		/*
		 * 如果已有一个待触发中断，其时间不晚于所需
		 * 时间，我们就无需再做任何事。信号处理器会
		 * 在第一种情况下做出正确处理，在第二种情况
		 * 下把中断重调度到稍后。多出的中断看似
		 * 是浪费，但其工作量并不大，而且这种方法在
		 * 常见的用例（反复设置一个预期到不了的超时
		 * 再取消它）中优势非常显著。无需在每次超时
		 * 被设置或取消时调用 setitimer()，只需以大约等于
		 * 超时延迟的间隔，执行一次中断和一次重新调度
		 * 的 setitimer() 调用即可。
		 * 例如，在 statement_timeout = 1s 且吞吐量为每秒
		 * 数千个查询的情况下，该方法只需要大约每秒
		 * 一次中断和一次 setitimer() 调用，而不是每秒数千
		 * 次 setitimer() 调用。
		 *
		 * 由于从我们取得 "now" 到到达 setitimer() 之间
		 * 可能还会流逝时间，内核对何时触发中断的判断
		 * 很可能比 signal_due_at 稍晚。
		 * 这没关系，理由同上。
		 */
		if (signal_pending && nearest_timeout >= signal_due_at)
			return;

		/*
		 * 与调用 enable_alarm() 一样，我们必须在调用
		 * setitimer() *之前* 设置 signal_pending；若其后设置，
		 * 信号处理器可能在我们设置之前触发，让我们误
		 * 以为信号会来。
		 *
		 * 设置/检查 signal_pending 涉及的其他竞态条件
		 * 都是安全的，理由如上。还有一点：
		 * 信号处理器可能在我们设置 signal_due_at 之后、
		 * 但仍在 setitimer() 前触发。此时处理器可能
		 * 用它自己计算的值覆盖 signal_due_at，该值与
		 * 我们刚算出的值相同或更晚。在我们执行
		 * setitimer() 之后，净效果将是 signal_due_at 给出的
		 * 时间晚于中断真正发生的时刻；
		 * 这是安全的情形。
		 */
		signal_due_at = nearest_timeout;
		signal_pending = true;

		/* 设报警定时器 */
		if (setitimer(ITIMER_REAL, &timeval, NULL) != 0)
		{
			/*
			 * 在这里清除 signal_pending 有点走形式，但也
			 * 不尽然，因为 FATAL 退出路径中的代码可能试图
			 * 使用超时设施。
			 */
			signal_pending = false;
			elog(FATAL, "could not enable SIGALRM timer: %m");
		}
	}
}


/*****************************************************************************
 *信号处理器
 *****************************************************************************/

/*
 * SIGALRM 信号处理器
 *
 * 处理各活动超时原因，然后按需重新调度中断。
 *
 */
static void
handle_sig_alarm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * 递增 holdoff 计数器，确保我们调用的任何函数
	 * 都不会直接处理中断。超时处理器都不该这么
	 * 做，这类故障难调试，宁可保险。
	 */
	HOLD_INTERRUPTS();

	/*
	 * SIGALRM 总是要唤醒在进程锁存器上等待的一切。
	 *
	 */
	SetLatch(MyLatch);

	/*
	 * 总是重置 signal_pending，即使 !alarm_enabled，因此时
	 * 无信号待触发。
	 */
	signal_pending = false;

	/*
	 * 触发各待触发超时，但仅在我们被启用时。
	 */
	if (alarm_enabled)
	{
		/*
		 * 禁用报警，以防某些平台允许信号处理器
		 * 中断自身。schedule_alarm() 会酌情重新启用。
		 *
		 */
		disable_alarm();

		if (num_active_timeouts > 0)
		{
			TimestampTz now = GetCurrentTimestamp();

			/* 当第一个待触发超时已到期时…… */
			while (num_active_timeouts > 0 &&
				   now >= active_timeouts[0]->fin_time)
			{
				timeout_params *this_timeout = active_timeouts[0];

				/* 把它从活动列表移除 */
				remove_timeout_index(0);

				/* 标记已触发 */
				this_timeout->indicator = true;

				/* 并调用其处理器函数 */
				this_timeout->timeout_handler();

				/* 若应重复触发，则重新启用。 */
				if (this_timeout->interval_in_ms > 0)
				{
					TimestampTz new_fin_time;

					/*
					 * 为防漂移，超时下实例应基于预期触发
					 * 时间调度而非实际触发。但若超时过迟
					 * 以致错过整轮周期，则退回按实际触发
					 * 时间调度。
					 *
					 */
					new_fin_time =
						TimestampTzPlusMilliseconds(this_timeout->fin_time,
													this_timeout->interval_in_ms);
					if (new_fin_time < now)
						new_fin_time =
							TimestampTzPlusMilliseconds(now,
														this_timeout->interval_in_ms);
					enable_timeout(this_timeout->index, now, new_fin_time,
								   this_timeout->interval_in_ms);
				}

				/*
				 * 处理器耗时可能不可忽略（比如 CheckDeadLock
				 * 就不算便宜），所以我们每次都更新对
				 * "now" 的认知。
				 */
				now = GetCurrentTimestamp();
			}

			/* 超时触发完毕，若还有则重新调度下次中断 */
			schedule_alarm(now);
		}
	}

	RESUME_INTERRUPTS();

	errno = save_errno;
}


/*****************************************************************************
 * 公共 API
 *****************************************************************************/

/*
 * 初始化超时模块。
 *
 * 每个想使用超时的进程都必须调用它。
 *
 * 如果本进程是从另一个同样在使用本模块的
 * 进程分叉出来的，务必在重新启用信号前调用；
 * 否则本应在父进程运行的处理器可能在此运行。
 */
void
InitializeTimeouts(void)
{
	int			i;

	/* 初始化或重新初始化所有本地状态 */
	disable_alarm();

	num_active_timeouts = 0;

	for (i = 0; i < MAX_TIMEOUTS; i++)
	{
		all_timeouts[i].index = i;
		all_timeouts[i].active = false;
		all_timeouts[i].indicator = false;
		all_timeouts[i].timeout_handler = NULL;
		all_timeouts[i].start_time = 0;
		all_timeouts[i].fin_time = 0;
		all_timeouts[i].interval_in_ms = 0;
	}

	all_timeouts_initialized = true;

	/* 现在安装信号处理器 */
	pqsignal(SIGALRM, handle_sig_alarm);
}

/*
 * 注册一个超时原因
 *
 * 对预定义超时，这只是注册回调函数。
 *
 * 对用户自定义超时，传入 id == USER_TIMEOUT；我们会分配
 * 出一个 ID。
 */
TimeoutId
RegisterTimeout(TimeoutId id, timeout_handler_proc handler)
{
	Assert(all_timeouts_initialized);

	/* 这里无须禁用信号处理器。 */

	if (id >= USER_TIMEOUT)
	{
		/* 分配用户自定义超时原因 */
		for (id = USER_TIMEOUT; id < MAX_TIMEOUTS; id++)
			if (all_timeouts[id].timeout_handler == NULL)
				break;
		if (id >= MAX_TIMEOUTS)
			ereport(FATAL,
					(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
					 errmsg("cannot add more timeout reasons")));
	}

	Assert(all_timeouts[id].timeout_handler == NULL);

	all_timeouts[id].timeout_handler = handler;

	return id;
}

/*
 * 重新调度待触发 SIGALRM 中断。
 *
 * 它可用于错误恢复期间，以防查询取消导致丢失了
 * SIGALRM 事件（因在 handle_sig_alarm 有机会做任何
 * 事情前就 longjmp 跳出）。但注意若同一区域调用了
 * 公共的 enable_ 或 disable_timeout 函数则无必要，
 * 它们内部都会按需调用 schedule_alarm()。
 */
void
reschedule_timeouts(void)
{
	/* 灵活起见，允许在初始化之前调用它。 */
	if (!all_timeouts_initialized)
		return;

	/* 为安全起见禁用超时中断。 */
	disable_alarm();

	/* 若仍有活动超时，重新调度中断。 */
	if (num_active_timeouts > 0)
		schedule_alarm(GetCurrentTimestamp());
}

/*
 * 启用指定超时在指定延迟后触发。
 *
 * 延迟以毫秒为单位。
 */
void
enable_timeout_after(TimeoutId id, int delay_ms)
{
	TimestampTz now;
	TimestampTz fin_time;

	/* 为安全起见禁用超时中断。 */
	disable_alarm();

	/* 在适当时间把超时加入队列。 */
	now = GetCurrentTimestamp();
	fin_time = TimestampTzPlusMilliseconds(now, delay_ms);
	enable_timeout(id, now, fin_time, 0);

	/* 设置定时器中断。 */
	schedule_alarm(now);
}

/*
 * 启用指定超时周期性触发，以指定延迟作为
 * 两次触发之间的时间。
 *
 * 延迟以毫秒为单位。
 */
void
enable_timeout_every(TimeoutId id, TimestampTz fin_time, int delay_ms)
{
	TimestampTz now;

	/* 为安全起见禁用超时中断。 */
	disable_alarm();

	/* 在适当时间把超时加入队列。 */
	now = GetCurrentTimestamp();
	enable_timeout(id, now, fin_time, delay_ms);

	/* 设置定时器中断。 */
	schedule_alarm(now);
}

/*
 * 启用指定超时在指定时刻触发。
 *
 * 提供它是为了支持需要参照 "now" 以外的时点来
 * 计算超时的场景。若无此种理由，请改用
 * enable_timeout_after()，以免调用两次 GetCurrentTimestamp()。
 */
void
enable_timeout_at(TimeoutId id, TimestampTz fin_time)
{
	TimestampTz now;

	/* 为安全起见禁用超时中断。 */
	disable_alarm();

	/* 在适当时间把超时加入队列。 */
	now = GetCurrentTimestamp();
	enable_timeout(id, now, fin_time, 0);

	/* 设置定时器中断。 */
	schedule_alarm(now);
}

/*
 * 一次启用多个超时。
 *
 * 其效果如同调用 enable_timeout_after() 和/或
 * enable_timeout_at() 多次。用它来减少建立多个超时
 * 所需的 GetCurrentTimestamp() 和 setitimer() 调用数。
 */
void
enable_timeouts(const EnableTimeoutParams *timeouts, int count)
{
	TimestampTz now;
	int			i;

	/* 为安全起见禁用超时中断。 */
	disable_alarm();

	/* 适当时间把各 timeout(s) 入队。 */
	now = GetCurrentTimestamp();

	for (i = 0; i < count; i++)
	{
		TimeoutId	id = timeouts[i].id;
		TimestampTz fin_time;

		switch (timeouts[i].type)
		{
			case TMPARAM_AFTER:
				fin_time = TimestampTzPlusMilliseconds(now,
													   timeouts[i].delay_ms);
				enable_timeout(id, now, fin_time, 0);
				break;

			case TMPARAM_AT:
				enable_timeout(id, now, timeouts[i].fin_time, 0);
				break;

			case TMPARAM_EVERY:
				fin_time = TimestampTzPlusMilliseconds(now,
													   timeouts[i].delay_ms);
				enable_timeout(id, now, fin_time, timeouts[i].delay_ms);
				break;

			default:
				elog(ERROR, "unrecognized timeout type %d",
					 (int) timeouts[i].type);
				break;
		}
	}

	/* 设置定时器中断。 */
	schedule_alarm(now);
}

/*
 * 取消指定的超时。
 *
 * 超时的"已触发"标志会重置，除非
 * keep_indicator 为 true。
 *
 * 取消一个超时时，其他活动超时仍然有效。
 * 禁用未启用的超时并不是错误。
 */
void
disable_timeout(TimeoutId id, bool keep_indicator)
{
	/* 断言请求合理 */
	Assert(all_timeouts_initialized);
	Assert(all_timeouts[id].timeout_handler != NULL);

	/* 为安全起见禁用超时中断。 */
	disable_alarm();

	/* 找到该超时并把它从活动列表移除。 */
	if (all_timeouts[id].active)
		remove_timeout_index(find_active_timeout(id));

	/* 标为不活动（无论是否活动过）。 */
	if (!keep_indicator)
		all_timeouts[id].indicator = false;

	/* 若仍有活动超时，重新调度中断。 */
	if (num_active_timeouts > 0)
		schedule_alarm(GetCurrentTimestamp());
}

/*
 * 一次取消多个超时。
 *
 * 超时的"已触发"标志会重置，除非
 * timeouts[i].keep_indicator 为 true。
 *
 * 其效果如同多次调用 disable_timeout()。
 * 用它来减少取消多个超时所需的
 * GetCurrentTimestamp() 和 setitimer() 调用数。
 */
void
disable_timeouts(const DisableTimeoutParams *timeouts, int count)
{
	int			i;

	Assert(all_timeouts_initialized);

	/* 为安全起见禁用超时中断。 */
	disable_alarm();

	/* 取消 timeout(s)。 */
	for (i = 0; i < count; i++)
	{
		TimeoutId	id = timeouts[i].id;

		Assert(all_timeouts[id].timeout_handler != NULL);

		if (all_timeouts[id].active)
			remove_timeout_index(find_active_timeout(id));

		if (!timeouts[i].keep_indicator)
			all_timeouts[id].indicator = false;
	}

	/* 若仍有活动超时，重新调度中断。 */
	if (num_active_timeouts > 0)
		schedule_alarm(GetCurrentTimestamp());
}

/*
 * 禁用信号处理器，把所有超时从活动列表移除，
 * 并可选地重置它们的超时标志。
 */
void
disable_all_timeouts(bool keep_indicators)
{
	int			i;

	disable_alarm();

	/*
	 * 我们过去在这里禁用定时器中断，但在常见
	 * 的使用模式下让它保持启用反而更省；这可以
	 * 省得很快又要重新启用。见 schedule_alarm()。
	 */

	num_active_timeouts = 0;

	for (i = 0; i < MAX_TIMEOUTS; i++)
	{
		all_timeouts[i].active = false;
		if (!keep_indicators)
			all_timeouts[i].indicator = false;
	}
}

/*
 * 若超时处于活动状态（已启用且尚未触发）则
 *
 * 返回 true。当然这受竞态条件影响：超时可能就在我们
 * 查看后立即触发。
 */
bool
get_timeout_active(TimeoutId id)
{
	return all_timeouts[id].active;
}

/*
 * 返回超时的"已触发"标志
 *
 * 若 reset_indicator 为 true，返回 true 时重置标志。
 * 为避免竞态条件导致错过超时，返回 false 时
 * 我们会小心不重置标志。
 */
bool
get_timeout_indicator(TimeoutId id, bool reset_indicator)
{
	if (all_timeouts[id].indicator)
	{
		if (reset_indicator)
			all_timeouts[id].indicator = false;
		return true;
	}
	return false;
}

/*
 * 返回超时最近一次被激活的时间
 *
 * 注意：若该超时在本进程中从未激活过，将返回 0。
 * 但我们*绝不*在超时发生时重置 start_time，以免当
 * 某些代码正要读取该值时 SIGALRM 恰好触发而
 * 产生竞态条件。
 */
TimestampTz
get_timeout_start_time(TimeoutId id)
{
	return all_timeouts[id].start_time;
}

/*
 * 返回超时当前或最近应触发的时间
 *
 * 注意：若该超时在本进程中从未激活过，将返回 0。
 * 但我们*绝不*在超时发生时重置 fin_time，以免当
 * 某些代码正要读取该值时 SIGALRM 恰好触发而
 * 产生竞态条件。
 */
TimestampTz
get_timeout_finish_time(TimeoutId id)
{
	return all_timeouts[id].fin_time;
}
