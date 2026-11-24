/*-------------------------------------------------------------------------
 *
 * basebackup_progress.c
 *	  实现进度跟踪的基备份 sink，含但不限于
 *	  命令进度报告。
 *
 * 即便复制命令 BASE_BACKUP 未指定 PROGRESS
 * 选项也应使用。无该选项时不会预先累计待备份
 * 文件大小，但仍可向命令进度报告设施报告已
 * 处理的数据量。
 *
 *
 * 此外也借此处更新 bbsink_state 的若干字段。
 * 这确属跟踪进度，但不止内省：字段须正确更新
 * 基备份才能正常工作。
 *
 *
 *
 * 本基备份 sink 需要多数 sink 没有的额外回调。
 * 与其塞进接口，不如在此提供若干供 basebackup.c
 * 调用的额外函数。（逻辑虽简单可直接放该文件，
 * 但把进度报告相关都放一处
 * 更清晰。）
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_progress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "backup/basebackup.h"
#include "backup/basebackup_sink.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/timestamp.h"

static void bbsink_progress_begin_backup(bbsink *sink);
static void bbsink_progress_archive_contents(bbsink *sink, size_t len);
static void bbsink_progress_end_archive(bbsink *sink);

static const bbsink_ops bbsink_progress_ops = {
	.begin_backup = bbsink_progress_begin_backup,
	.begin_archive = bbsink_forward_begin_archive,
	.archive_contents = bbsink_progress_archive_contents,
	.end_archive = bbsink_progress_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_forward_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

/*
 * 创建执行进度跟踪并把数据转发给后继 sink 的
 * 新基备份 sink。
 */
bbsink *
bbsink_progress_new(bbsink *next, bool estimate_backup_size)
{
	bbsink	   *sink;

	Assert(next != NULL);

	sink = palloc0(sizeof(bbsink));
	*((const bbsink_ops **) &sink->bbs_ops) = &bbsink_progress_ops;
	sink->bbs_next = next;

	/*
	 * 报告基备份进行中，并把备份总大小设为 -1，
	 * 其会被转成 NULL。若正在估算备份大小，有真实
	 * 估计后再填入。
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_BASEBACKUP, InvalidOid);
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_BACKUP_TOTAL, -1);

	return sink;
}

/*
 * 备份开始时的进度报告。
 */
static void
bbsink_progress_begin_backup(bbsink *sink)
{
	const int	index[] = {
		PROGRESS_BASEBACKUP_PHASE,
		PROGRESS_BASEBACKUP_BACKUP_TOTAL,
		PROGRESS_BASEBACKUP_TBLSPC_TOTAL
	};
	int64		val[3];

	/*
	 * 报告正在以基备份流式传送数据库文件。并公布
	 * 表空间个数，以及（若已知）估计的备份
	 * 总大小。
	 */
	val[0] = PROGRESS_BASEBACKUP_PHASE_STREAM_BACKUP;
	if (sink->bbs_state->bytes_total_is_valid)
		val[1] = sink->bbs_state->bytes_total;
	else
		val[1] = -1;
	val[2] = list_length(sink->bbs_state->tablespaces);
	pgstat_progress_update_multi_param(3, index, val);

	/* 转发下一 sink。 */
	bbsink_forward_begin_backup(sink);
}

/*
 * 归档结束时的进度报告。
 */
static void
bbsink_progress_end_archive(bbsink *sink)
{
	/*
	 * 预期每个表空间一个归档，故到达归档末也意味
	 * 到达表空间末。（将来或许有理由拆开这两个
	 * 概念。）
	 *
	 * 若备份含 WAL，会在最后一个归档结束前就标完
	 * 最后一个表空间，故此处须防护，确保已流式传送
	 * 的表空间数不超总数。
	 *
	 */
	if (sink->bbs_state->tablespace_num < list_length(sink->bbs_state->tablespaces))
		pgstat_progress_update_param(PROGRESS_BASEBACKUP_TBLSPC_STREAMED,
									 sink->bbs_state->tablespace_num + 1);

	/* 转发下一 sink。 */
	bbsink_forward_end_archive(sink);

	/*
	 * 此处便于更新 bbsink_state 对当前表空间的认识。
	 * 注意 bbsink_state 在相关 bbsink 间共享，而我们
	 * 是最外层，这是我们做的最后一件事。
	 * 最后一步。
	 */
	sink->bbs_state->tablespace_num++;
}

/*
 * 处理新归档内容的进度跟踪。
 *
 * 把已流式传送计数增加给定字节数，并更新
 * pg_stat_progress_basebackup
 * 进度报告。
 */
static void
bbsink_progress_archive_contents(bbsink *sink, size_t len)
{
	bbsink_state *state = sink->bbs_state;
	const int	index[] = {
		PROGRESS_BASEBACKUP_BACKUP_STREAMED,
		PROGRESS_BASEBACKUP_BACKUP_TOTAL
	};
	int64		val[2];
	int			nparam = 0;

	/* 先用已完成字节数更新 bbsink_state。 */
	state->bytes_done += len;

	/* 再转发下一 sink。 */
	bbsink_forward_archive_contents(sink, len);

	/* 准备为命令进度报告设置已完成字节数。 */
	val[nparam++] = state->bytes_done;

	/*
	 * 也可能要更新总字节数，以免超过 100% 或总额。
	 * 接近备份末时总额可能变（含 WAL 时估计总会偏），
	 * 但总好过"已完成"列大于"总计"。
	 *
	 *
	 */
	if (state->bytes_total_is_valid && state->bytes_done > state->bytes_total)
		val[nparam++] = state->bytes_done;

	pgstat_progress_update_multi_param(nparam, index, val);
}

/*
 * 公布正在等待备份开始检查点。
 */
void
basebackup_progress_wait_checkpoint(void)
{
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
								 PROGRESS_BASEBACKUP_PHASE_WAIT_CHECKPOINT);
}

/*
 * 公布正在估算备份大小。
 */
void
basebackup_progress_estimate_backup_size(void)
{
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
								 PROGRESS_BASEBACKUP_PHASE_ESTIMATE_BACKUP_SIZE);
}

/*
 * 公布正在等待备份结束时的 WAL 归档。
 */
void
basebackup_progress_wait_wal_archive(bbsink_state *state)
{
	const int	index[] = {
		PROGRESS_BASEBACKUP_PHASE,
		PROGRESS_BASEBACKUP_TBLSPC_STREAMED
	};
	int64		val[2];

	/*
	 * 此时报告所有表空间已完成，即便主表空间归档
	 * 仍打开；因接着加入的是 WAL 文件，而非真正
	 * 属于主表空间的文件。
	 * 表空间.
	 */
	val[0] = PROGRESS_BASEBACKUP_PHASE_WAIT_WAL_ARCHIVE;
	val[1] = list_length(state->tablespaces);
	pgstat_progress_update_multi_param(2, index, val);
}

/*
 * 公布正在把 WAL 文件转入最终归档。
 */
void
basebackup_progress_transfer_wal(void)
{
	pgstat_progress_update_param(PROGRESS_BASEBACKUP_PHASE,
								 PROGRESS_BASEBACKUP_PHASE_TRANSFER_WAL);
}

/*
 * 公布我们不再执行备份。
 */
void
basebackup_progress_done(void)
{
	pgstat_progress_end_command();
}
