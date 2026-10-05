/*-------------------------------------------------------------------------
 *
 * s_lock.c
 *	   与硬件相关的自旋锁实现。
 *
 * 等待有竞争的自旋锁时，我们先紧密循环一会儿，然后
 * 用 pg_usleep() 延迟后再重试。最好让 "一会儿" 是
 * 我们预期自旋锁被持有的最长时间的一个小倍数。100
 * 次迭代作为最初的猜测似乎差不多。不过，在
 * 单处理器上这个循环是在浪费周期，而在多 CPU 情形下，
 * 通常多自旋一会儿比去调用内核更好，因此我们试图
 * 根据看起来是处在单处理器还是多处理器，
 * 来调整自旋循环的次数。
 *
 * 注意：你也许觉得 MIN_SPINS_PER_DELAY 只要设成 1 就行，但你
 * 错了；有些平台上那会导致 "卡住的自旋锁"
 * 失败。这在 Alpha 上尤其见过；似乎
 * 从内核空间返回之后的第一次 TAS，在那种硬件上总会
 * 失败。
 *
 * 一旦决定阻塞，我们就使用随机增大的 pg_usleep()
 * 延迟。第一次延迟是 1 毫秒，然后延迟随机增大到
 * 大约一秒，之后重置为 1 毫秒并重新开始。这里的
 * 想法是，竞争激烈时我们需要
 * 增大延迟，否则持有自旋锁的进程可能永远得不到运行，
 * 也就无法释放这把锁。（设想持有者已经被
 * 调度器调低了优先级——在所有想获取锁的进程都进入睡眠之前，
 * 它不会被调度；因此若我们总是只睡 1 毫秒，
 * 就确实可能发生饥饿。）但我们不能只是
 * 把延迟钳制在某个上界，否则要做合理次数的尝试
 * 就会花很长时间。
 *
 * 经过 NUM_DELAYS 次延迟后我们超时并报错（因此恰好是
 * 那么多次尝试）。按给定的设置，这通常要花 2
 * 分钟左右。固定尝试的总次数（从而固定
 * 意外失败的概率），似乎比固定所花费的总时间
 * 更好。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/s_lock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "common/pg_prng.h"
#include "port/atomics.h"
#include "storage/s_lock.h"
#include "utils/wait_event.h"

#define MIN_SPINS_PER_DELAY 10
#define MAX_SPINS_PER_DELAY 1000
#define NUM_DELAYS			1000
#define MIN_DELAY_USEC		1000L
#define MAX_DELAY_USEC		1000000L


slock_t		dummy_spinlock;

static int	spins_per_delay = DEFAULT_SPINS_PER_DELAY;


/*
 * s_lock_stuck() - 报告自旋锁卡住了
 */
static void
s_lock_stuck(const char *file, int line, const char *func)
{
	if (!func)
		func = "(unknown)";
#if defined(S_LOCK_TEST)
	fprintf(stderr,
			"\nStuck spinlock detected at %s, %s:%d.\n",
			func, file, line);
	exit(1);
#else
	elog(PANIC, "stuck spinlock detected at %s, %s:%d",
		 func, file, line);
#endif
}

/*
 * s_lock(lock) - 等待自旋锁时与平台无关的那一部分。
 */
int
s_lock(volatile slock_t *lock, const char *file, int line, const char *func)
{
	SpinDelayStatus delayStatus;

	init_spin_delay(&delayStatus, file, line, func);

	while (TAS_SPIN(lock))
	{
		perform_spin_delay(&delayStatus);
	}

	finish_spin_delay(&delayStatus);

	return delayStatus.delays;
}

#ifdef USE_DEFAULT_S_UNLOCK
void
s_unlock(volatile slock_t *lock)
{
#ifdef TAS_ACTIVE_WORD
	/* HP 的 PA-RISC */
	*TAS_ACTIVE_WORD(lock) = -1;
#else
	*lock = 0;
#endif
}
#endif

/*
 * 在有竞争的自旋锁上自旋等待。
 */
void
perform_spin_delay(SpinDelayStatus *status)
{
	/* 循环每一轮都做一次与 CPU 相关的延迟 */
	SPIN_DELAY();

	/* 每尝试 spins_per_delay 次，就阻塞一次进程 */
	if (++(status->spins) >= spins_per_delay)
	{
		if (++(status->delays) > NUM_DELAYS)
			s_lock_stuck(status->file, status->line, status->func);

		if (status->cur_delay == 0) /* 第一次要延迟？ */
			status->cur_delay = MIN_DELAY_USEC;

		/*
		 * 一旦开始睡眠，报告等待事件的开销就是
		 * 值得的。积极自旋在性能分析器里很容易显出来，但
		 * 带着指数退避去睡眠就更难被发现...
		 *
		 * 将来也许会想报告更细的内容，但
		 * 这总比没有好。
		 */
		pgstat_report_wait_start(WAIT_EVENT_SPIN_DELAY);
		pg_usleep(status->cur_delay);
		pgstat_report_wait_end();

#if defined(S_LOCK_TEST)
		fprintf(stdout, "*");
		fflush(stdout);
#endif

		/* 按 1X 到 2X 之间的一个随机比例增加延迟 */
		status->cur_delay += (int) (status->cur_delay *
									pg_prng_double(&pg_global_prng_state) + 0.5);
		/* 超过最大值时，折回到最小延迟 */
		if (status->cur_delay > MAX_DELAY_USEC)
			status->cur_delay = MIN_DELAY_USEC;

		status->spins = 0;
	}
}

/*
 * 取得自旋锁之后，更新对循环时长的估计。
 *
 * 若我们没有延迟就取得了锁，这很好地
 * 表明我们处在多处理器上。若不得不延迟，这是
 * 处在单处理器上的迹象（但并非确定）。因此，不得不延迟时我们
 * 缓慢减小 spins_per_delay，没有延迟时则
 * 迅速增大它。预期 spins_per_delay 会
 * 在单处理器上收敛到最小值，在多处理器上收敛到最大
 * 值。
 *
 * 注意：spins_per_delay 在当前进程内是本地的。我们希望
 * 跨多个后端对这些观察取平均，因为
 * 就连进入本函数都相对少见，因此单个
 * 后端也许活不到收敛出一个好值那么久。这件事
 * 由下面两个例程来处理。
 */
void
finish_spin_delay(SpinDelayStatus *status)
{
	if (status->cur_delay == 0)
	{
		/* 我们从未需要延迟 */
		if (spins_per_delay < MAX_SPINS_PER_DELAY)
			spins_per_delay = Min(spins_per_delay + 100, MAX_SPINS_PER_DELAY);
	}
	else
	{
		if (spins_per_delay > MIN_SPINS_PER_DELAY)
			spins_per_delay = Max(spins_per_delay - 1, MIN_SPINS_PER_DELAY);
	}
}

/*
 * 在后端启动期间设置 spins_per_delay 的本地副本。
 *
 * 注意：这里必须相当快，因为调用时正持有一把自旋锁
 */
void
set_spins_per_delay(int shared_spins_per_delay)
{
	spins_per_delay = shared_spins_per_delay;
}

/*
 * 在后端退出期间更新 spins_per_delay 的共享估计。
 *
 * 注意：这里必须相当快，因为调用时正持有一把自旋锁
 */
int
update_spins_per_delay(int shared_spins_per_delay)
{
	/*
	 * 我们使用适应速率相对较慢的指数移动平均，
	 * 这样任何一个后端结果里的噪声都不会过于影响共享
	 * 值。只要两个输入都在允许范围内，
	 * 结果也必定如此，因此不必担心去钳制结果。
	 *
	 * 我们有意截断而不是舍入；这样，后端内部的单次
	 * 调整就能影响到共享估计（见上面的
	 * 非对称调整规则）。
	 */
	return (shared_spins_per_delay * 15 + spins_per_delay) / 16;
}


/*****************************************************************************/
#if defined(S_LOCK_TEST)

/*
 * 用于验证某个移植的自旋锁支持的测试程序。
 */

struct test_lock_struct
{
	char		pad1;
	slock_t		lock;
	char		pad2;
};

volatile struct test_lock_struct test_lock;

int
main()
{
	pg_prng_seed(&pg_global_prng_state, (uint64) time(NULL));

	test_lock.pad1 = test_lock.pad2 = 0x44;

	S_INIT_LOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (!S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not initialized\n");
		return 1;
	}

	S_LOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not locked\n");
		return 1;
	}

	S_UNLOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (!S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not unlocked\n");
		return 1;
	}

	S_LOCK(&test_lock.lock);

	if (test_lock.pad1 != 0x44 || test_lock.pad2 != 0x44)
	{
		printf("S_LOCK_TEST: failed, declared datatype is wrong size\n");
		return 1;
	}

	if (S_LOCK_FREE(&test_lock.lock))
	{
		printf("S_LOCK_TEST: failed, lock not re-locked\n");
		return 1;
	}

	printf("S_LOCK_TEST: this will print %d stars and then\n", NUM_DELAYS);
	printf("             exit with a 'stuck spinlock' message\n");
	printf("             if S_LOCK() and TAS() are working.\n");
	fflush(stdout);

	s_lock(&test_lock.lock, __FILE__, __LINE__, __func__);

	printf("S_LOCK_TEST: failed, lock not locked\n");
	return 1;
}

#endif							/* S_LOCK_TEST（自旋锁测试） */
