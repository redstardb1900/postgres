/*-------------------------------------------------------------------------
 *
 * basebackup_sink.c
 *	  bbsink（基备份 sink）回调的默认实现。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * src/backend/backup/basebackup_sink.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "backup/basebackup_sink.h"

/*
 * 转发 begin_backup 回调。
 *
 * 仅当希望所实现的 bbsink 与后继 bbsink 共享缓冲时，
 * 才使用本实现。
 */
void
bbsink_forward_begin_backup(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	Assert(sink->bbs_state != NULL);
	bbsink_begin_backup(sink->bbs_next, sink->bbs_state,
						sink->bbs_buffer_length);
	sink->bbs_buffer = sink->bbs_next->bbs_buffer;
}

/*
 * 转发 begin_archive 回调。
 */
void
bbsink_forward_begin_archive(bbsink *sink, const char *archive_name)
{
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, archive_name);
}

/*
 * 转发 archive_contents 回调。
 *
 * 欲用本实现者，应把自身 bbs_buffer 与
 * bbs_buffer_length 设为后继 sink 的值。缓冲不共享时，
 * 须在转发回调前复制数据。此处不做复制，因为没有
 * 理由为同一份相同数据分别再分配缓冲。
 *
 *
 */
void
bbsink_forward_archive_contents(bbsink *sink, size_t len)
{
	Assert(sink->bbs_next != NULL);
	Assert(sink->bbs_buffer == sink->bbs_next->bbs_buffer);
	Assert(sink->bbs_buffer_length == sink->bbs_next->bbs_buffer_length);
	bbsink_archive_contents(sink->bbs_next, len);
}

/*
 * 转发 end_archive 回调。
 */
void
bbsink_forward_end_archive(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_end_archive(sink->bbs_next);
}

/*
 * 转发 begin_manifest 回调。
 */
void
bbsink_forward_begin_manifest(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_begin_manifest(sink->bbs_next);
}

/*
 * 转发 manifest_contents 回调。
 *
 * 与 archive_contents 回调一样，预期缓冲是共享的。
 *
 */
void
bbsink_forward_manifest_contents(bbsink *sink, size_t len)
{
	Assert(sink->bbs_next != NULL);
	Assert(sink->bbs_buffer == sink->bbs_next->bbs_buffer);
	Assert(sink->bbs_buffer_length == sink->bbs_next->bbs_buffer_length);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * 转发 end_manifest 回调。
 */
void
bbsink_forward_end_manifest(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_end_manifest(sink->bbs_next);
}

/*
 * 转发 end_backup 回调。
 */
void
bbsink_forward_end_backup(bbsink *sink, XLogRecPtr endptr, TimeLineID endtli)
{
	Assert(sink->bbs_next != NULL);
	bbsink_end_backup(sink->bbs_next, endptr, endtli);
}

/*
 * 转发 cleanup 回调。
 */
void
bbsink_forward_cleanup(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_cleanup(sink->bbs_next);
}
