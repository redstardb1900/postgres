/*-------------------------------------------------------------------------
 *
 * basebackup_sink.h
 *	  用于过滤或发往最终目的地的 API：基备
 *	  份过程产生的归档
 *
 * 做基备份会为每个表空间目录产生一个归档，
 * 另加备份清单（除非已禁用该功能）。备份
 * 过程的目标是把这些归档与清单放到某处，
 * 可能先做某种后处理。bbsink 是可接收这些
 * 归档以及（若有）清单的对象。
 * 。
 *
 * 实践中往往是一串 bbsink 而非单个，回调
 * 从前一个转发到下一个，可能带修改。每个
 * 对象负责一项任务，如命令进度报告、限速
 * 或与客户端通信。
 * 。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup_sink.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BASEBACKUP_SINK_H
#define BASEBACKUP_SINK_H

#include "access/xlogdefs.h"
#include "common/compression.h"
#include "nodes/pg_list.h"

/*  前向声明。  */
struct bbsink;
struct bbsink_ops;
typedef struct bbsink bbsink;
typedef struct bbsink_ops bbsink_ops;

/*
 * 一次备份中各 bbsink 共享的总体备份状态。
 *
 * 调用 bbstate_begin_backup 前，调用者须创建
 * 贯穿备份生命周期的 bbsink_state，并在每次
 * 调用 bbsink 方法前按需更新。bbsink 会保留
 * 对该状态对象的指针，并据此了解备份进度。
 * 。
 *
 * 'tablespaces' 是 tablespaceinfo 对象列表。须在
 * 调用 bbstate_begin_backup() 前设置，之后不得改。
 *
 * 'tablespace_num' 是当前表空间在 'tablespaces'
 * 列表中的下标。
 *
 * 'bytes_done' 是迄今从 $PGDATA 读出的字节数。
 *
 * 'bytes_total' 是对 $PGDATA 中估计总字节数，
 * 若已做过估计。
 *
 * 'bytes_total_is_valid' 为真当且仅当已把正当
 * 存入 'bytes_total'。
 *
 * 'startptr' 与 'starttli' 标识备份开始时的 WAL
 * 流位置。须在调用 bbstate_begin_backup() 前设置，
 * 之后不得修改。
 */
typedef struct bbsink_state
{
	List	   *tablespaces;
	int			tablespace_num;
	uint64		bytes_done;
	uint64		bytes_total;
	bool		bytes_total_is_valid;
	XLogRecPtr	startptr;
	TimeLineID	starttli;
} bbsink_state;

/*
 * 基备份接收器的公共数据。
 *
 * 'bbs_ops' 是相关回调表。
 *
 * 'bbs_buffer' 是应写入发往 bbsink 之数据的缓冲。
 * 长度须为 BLCKSZ 的倍数。
 *
 * 'bbs_buffer_length' 是缓冲的已分配长度。
 *
 * 'bbs_next' 指向本 bbsink 正把部分或全部操作
 * 转发给的另一 bbsink。
 *
 * 'bbs_state' 指向本备份的 bbsink_state。
 * 与本备份关联的每个 bbsink 应指向同一
 * 底层状态对象。
 *
 * 一般期望这些字段在创建 bbsink 时设定，
 * 之后不再改变。可修改 bbs_buffer 或 bbs_state
 * 指向的数据，但不得改本结构体内容。
 * 。
 */
struct bbsink
{
	const bbsink_ops *bbs_ops;
	char	   *bbs_buffer;
	size_t		bbs_buffer_length;
	bbsink	   *bbs_next;
	bbsink_state *bbs_state;
};

/*
 * 基备份接收器的回调。
 *
 * 这些回调皆必需。若某回调只需转发到
 * sink->bbs_next，可用 bbsink_forward_<callback_name>
 * 作回调。
 *
 * 调用者应始终经 bbsink_* 内联函数调用这些
 * 回调，而非直接调用。
 */
struct bbsink_ops
{
	/*
	 * 本回调仅在备份最开始调用一次。须将
	 * bbs_buffer 设为至少可写 bbs_buffer_length
	 * 字节的存储块。
	 */
	void		(*begin_backup) (bbsink *sink);

	/*
	 * 发往 bbsink 的每个归档会调用一次
	 * begin_archive()，若干次 archive_contents()，
	 * 再一次 end_archive()。
	 * 。
	 *
	 * 调用 archive_contents() 前，调用者应把长度
	 * 等于即将传入的 len、且不超过 bbs_buffer_length
	 * 的字节拷入 bbs_buffer。
	 *
	 * 调用 archive_contents() 前尽量填满缓冲为佳，
	 * 但不值得花额外周期确保绝对 100% 满。
	 * 。
	 */
	void		(*begin_archive) (bbsink *sink, const char *archive_name);
	void		(*archive_contents) (bbsink *sink, size_t len);
	void		(*end_archive) (bbsink *sink);

	/*
	 * 若要把备份清单发给 bbsink，会调用一次
	 * begin_manifest()、若干次 manifest_contents()，
	 * 再一次 end_manifest()。这些调用发生在全部
	 * 归档传完之后。
	 *
	 * 调用 manifest_contents() 的规则与上文
	 * archive_contents() 相同。
	 */
	void		(*begin_manifest) (bbsink *sink);
	void		(*manifest_contents) (bbsink *sink, size_t len);
	void		(*end_manifest) (bbsink *sink);

	/*
	 * 本回调仅在全部归档与清单都发出后调用一次。
	 * 。
	 */
	void		(*end_backup) (bbsink *sink, XLogRecPtr endptr, TimeLineID endtli);

	/*
	 * 若备份因错误中止，在销毁 bbsink 前调用本
	 * 回调，以便释放不会自动释放的资源。若无错，
	 * 则在 end_backup 回调之后调用。
	 * 。
	 */
	void		(*cleanup) (bbsink *sink);
};

/*  开始备份  */
static inline void
bbsink_begin_backup(bbsink *sink, bbsink_state *state, int buffer_length)
{
	Assert(sink != NULL);

	Assert(buffer_length > 0);

	sink->bbs_state = state;
	sink->bbs_buffer_length = buffer_length;
	sink->bbs_ops->begin_backup(sink);

	Assert(sink->bbs_buffer != NULL);
	Assert((sink->bbs_buffer_length % BLCKSZ) == 0);
}

/*  开始归档。  */
static inline void
bbsink_begin_archive(bbsink *sink, const char *archive_name)
{
	Assert(sink != NULL);

	sink->bbs_ops->begin_archive(sink, archive_name);
}

/*  处理归档的部分内容。  */
static inline void
bbsink_archive_contents(bbsink *sink, size_t len)
{
	Assert(sink != NULL);

	/*
	 * 调用者应合理尝试填满缓冲再调本函数，故缓冲
	 * 不应完全空，也不得超过容量。
	 * 。
	 */
	Assert(len > 0 && len <= sink->bbs_buffer_length);

	sink->bbs_ops->archive_contents(sink, len);
}

/*  结束归档。  */
static inline void
bbsink_end_archive(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->end_archive(sink);
}

/*  开始备份清单。  */
static inline void
bbsink_begin_manifest(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->begin_manifest(sink);
}

/*  处理清单的部分内容。  */
static inline void
bbsink_manifest_contents(bbsink *sink, size_t len)
{
	Assert(sink != NULL);

	/*  见 bbsink_archive_contents 注释。  */
	Assert(len > 0 && len <= sink->bbs_buffer_length);

	sink->bbs_ops->manifest_contents(sink, len);
}

/*  结束备份清单。  */
static inline void
bbsink_end_manifest(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->end_manifest(sink);
}

/* 。 */
static inline void
bbsink_end_backup(bbsink *sink, XLogRecPtr endptr, TimeLineID endtli)
{
	Assert(sink != NULL);
	Assert(sink->bbs_state->tablespace_num == list_length(sink->bbs_state->tablespaces));

	sink->bbs_ops->end_backup(sink, endptr, endtli);
}

/*  销毁前释放资源。  */
static inline void
bbsink_cleanup(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->cleanup(sink);
}

/*  转发回调。用于把操作传到下一接收器。  */
extern void bbsink_forward_begin_backup(bbsink *sink);
extern void bbsink_forward_begin_archive(bbsink *sink,
										 const char *archive_name);
extern void bbsink_forward_archive_contents(bbsink *sink, size_t len);
extern void bbsink_forward_end_archive(bbsink *sink);
extern void bbsink_forward_begin_manifest(bbsink *sink);
extern void bbsink_forward_manifest_contents(bbsink *sink, size_t len);
extern void bbsink_forward_end_manifest(bbsink *sink);
extern void bbsink_forward_end_backup(bbsink *sink, XLogRecPtr endptr,
									  TimeLineID endtli);
extern void bbsink_forward_cleanup(bbsink *sink);

/*  各类接收器的构造函数。  */
extern bbsink *bbsink_copystream_new(bool send_to_client);
extern bbsink *bbsink_gzip_new(bbsink *next, pg_compress_specification *);
extern bbsink *bbsink_lz4_new(bbsink *next, pg_compress_specification *);
extern bbsink *bbsink_zstd_new(bbsink *next, pg_compress_specification *);
extern bbsink *bbsink_progress_new(bbsink *next, bool estimate_backup_size);
extern bbsink *bbsink_server_new(bbsink *next, char *pathname);
extern bbsink *bbsink_throttle_new(bbsink *next, uint32 maxrate);

/*  进度报告的额外接口函数。  */
extern void basebackup_progress_wait_checkpoint(void);
extern void basebackup_progress_estimate_backup_size(void);
extern void basebackup_progress_wait_wal_archive(bbsink_state *);
extern void basebackup_progress_transfer_wal(void);
extern void basebackup_progress_done(void);

#endif
