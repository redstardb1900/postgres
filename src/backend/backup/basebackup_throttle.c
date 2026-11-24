/*-------------------------------------------------------------------------
 *
 * basebackup_throttle.c
 *	  实现限速的基备份 sink。数据转发给链中下
 *	  一个基备份 sink，速率不超过所配置的
 *	  最大值。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_throttle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "backup/basebackup_sink.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/timestamp.h"

typedef struct bbsink_throttle
{
	/* 各类 sink 的公共信息。 */
	bbsink		base;

	/* 实际字节数，传这么多就可能引发睡眠。 */
	uint64		throttling_sample;

	/* 已传输但尚未做限速处理的数据量。  */
	int64		throttling_counter;

	/* 传输 throttling_sample 字节所需的最短时间。 */
	TimeOffset	elapsed_min_unit;

	/* 上次检查传输速率的时刻。 */
	TimestampTz throttled_last;
} bbsink_throttle;

static void bbsink_throttle_begin_backup(bbsink *sink);
static void bbsink_throttle_archive_contents(bbsink *sink, size_t len);
static void bbsink_throttle_manifest_contents(bbsink *sink, size_t len);
static void throttle(bbsink_throttle *sink, size_t increment);

static const bbsink_ops bbsink_throttle_ops = {
	.begin_backup = bbsink_throttle_begin_backup,
	.begin_archive = bbsink_forward_begin_archive,
	.archive_contents = bbsink_throttle_archive_contents,
	.end_archive = bbsink_forward_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_throttle_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

/*
 * 限速检查频率，为所配置"每秒速率"的一个分数。
 */
#define THROTTLING_FREQUENCY	8

/*
 * 创建执行限速并把数据转发给后继 sink 的
 * 新基备份 sink。
 */
bbsink *
bbsink_throttle_new(bbsink *next, uint32 maxrate)
{
	bbsink_throttle *sink;

	Assert(next != NULL);
	Assert(maxrate > 0);

	sink = palloc0(sizeof(bbsink_throttle));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_throttle_ops;
	sink->base.bbs_next = next;

	sink->throttling_sample =
		(int64) maxrate * (int64) 1024 / THROTTLING_FREQUENCY;

	/*
	 * 传输 throttling_sample 字节所需的最短
	 * 时间。
	 */
	sink->elapsed_min_unit = USECS_PER_SEC / THROTTLING_FREQUENCY;

	return &sink->base;
}

/*
 * 此处无实质工作，但需记录当前时间供后续计算
 * 使用。
 */
static void
bbsink_throttle_begin_backup(bbsink *sink)
{
	bbsink_throttle *mysink = (bbsink_throttle *) sink;

	bbsink_forward_begin_backup(sink);

	/* 真实数据自此开始（头已忽略）。 */
	mysink->throttled_last = GetCurrentTimestamp();
}

/*
 * 先限速，再把归档内容交给下一 sink。
 */
static void
bbsink_throttle_archive_contents(bbsink *sink, size_t len)
{
	throttle((bbsink_throttle *) sink, len);

	bbsink_forward_archive_contents(sink, len);
}

/*
 * 先限速，再把清单内容交给下一 sink。
 */
static void
bbsink_throttle_manifest_contents(bbsink *sink, size_t len)
{
	throttle((bbsink_throttle *) sink, len);

	bbsink_forward_manifest_contents(sink, len);
}

/*
 * 把网络传输计数器增加给定字节数，并在必要时
 * 睡眠以符合请求的网络传输速
 * 率.
 */
static void
throttle(bbsink_throttle *sink, size_t increment)
{
	TimeOffset	elapsed_min;

	Assert(sink->throttling_counter >= 0);

	sink->throttling_counter += increment;
	if (sink->throttling_counter < sink->throttling_sample)
		return;

	/* 按最小应已流逝多少时间？ */
	elapsed_min = sink->elapsed_min_unit *
		(sink->throttling_counter / sink->throttling_sample);

	/*
	 * 因锁存器可被并发 WAL 活动反复置位，故在循环中
	 * 睡眠以确保已过足够时间。
	 */
	for (;;)
	{
		TimeOffset	elapsed,
					sleep;
		int			wait_result;

		/* 自上次测量（及可能的唤醒）以来流逝的时间。 */
		elapsed = GetCurrentTimestamp() - sink->throttled_last;

		/* 若传输快于应有速度则睡眠 */
		sleep = elapsed_min - elapsed;
		if (sleep <= 0)
			break;

		ResetLatch(MyLatch);

		/* 我们可能吞掉已置位的锁存器，故检查中断 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * (TAR_SEND_SIZE / throttling_sample * elapsed_min_unit)
		 * 应是最长睡眠时间，故转为 long 是安全的。
		 */
		wait_result = WaitLatch(MyLatch,
								WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								(long) (sleep / 1000),
								WAIT_EVENT_BASE_BACKUP_THROTTLE);

		if (wait_result & WL_LATCH_SET)
			CHECK_FOR_INTERRUPTS();

		/* 等完了? */
		if (wait_result & WL_TIMEOUT)
			break;
	}

	/*
	 * 因用整数运算，只处理了 throttling_sample 的整倍
	 * 数。余量留待本函数下次调用处理。
	 */
	sink->throttling_counter %= sink->throttling_sample;

	/*
	 * 剩余量及后续可能增量的时间区间
	 * 此刻起.
	 */
	sink->throttled_last = GetCurrentTimestamp();
}
