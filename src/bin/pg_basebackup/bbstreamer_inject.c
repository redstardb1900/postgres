/*-------------------------------------------------------------------------
 *
 * bbstreamer_inject.c
 *
 * 部分版权 (c) 1996-2023, PostgreSQL 全球开发组
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_inject.c
 *-------------------------------------------------------------------------
                */

#include "postgres_fe.h"

#include "bbstreamer.h"
#include "common/file_perm.h"
#include "common/logging.h"

typedef struct bbstreamer_recovery_injector
{
	bbstreamer	base;
	bool		skip_file;
	bool		is_recovery_guc_supported;
	bool		is_postgresql_auto_conf;
	bool		found_postgresql_auto_conf;
	PQExpBuffer recoveryconfcontents;
	bbstreamer_member member;
} bbstreamer_recovery_injector;

static void bbstreamer_recovery_injector_content(bbstreamer *streamer,
												 bbstreamer_member *member,
												 const char *data, int len,
												 bbstreamer_archive_context context);
static void bbstreamer_recovery_injector_finalize(bbstreamer *streamer);
static void bbstreamer_recovery_injector_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_recovery_injector_ops = {
	.content = bbstreamer_recovery_injector_content,
	.finalize = bbstreamer_recovery_injector_finalize,
	.free = bbstreamer_recovery_injector_free
};

/*
 * 创建一个可以将恢复数据编辑到归档流中的 bbstreamer。
 *
 * 输入应该是一系列类型化块（不是 BBSTREAMER_UNKNOWN），遵循 bbstreamer.h 中
 * 描述的约定；转发给下一个 bbstreamer 的块将是类似类型化的，但在我们编辑
 * 了归档流的情况下，BBSTREAMER_MEMBER_HEADER 块可能为零长度。
 *
 * 我们的目标是对通过 recoveryconfcontents 传入的内容执行以下三种操作之一：
 * (1) 如果 is_recovery_guc_supported 为 false，则将内容放入 recovery.conf，
 * 替换任何同名的现有归档成员；(2) 如果 is_recovery_guc_supported 为 true
 * 且 postgresql.auto.conf 存在于归档中，则将提供的内容附加到现有文件；
 * (3) 如果 is_recovery_guc_supported 为 true 但 postgresql.auto.conf 不存在
 * 于归档中，则使用指定的内容创建它。
 *
 * 此外，如果 is_recovery_guc_supported 为 true，则创建一个零长度的
 * standby.signal 文件，并从归档中删除任何同名文件。
                                                                          */
extern bbstreamer *
bbstreamer_recovery_injector_new(bbstreamer *next,
								 bool is_recovery_guc_supported,
								 PQExpBuffer recoveryconfcontents)
{
	bbstreamer_recovery_injector *streamer;

	streamer = palloc0(sizeof(bbstreamer_recovery_injector));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_recovery_injector_ops;
	streamer->base.bbs_next = next;
	streamer->is_recovery_guc_supported = is_recovery_guc_supported;
	streamer->recoveryconfcontents = recoveryconfcontents;

	return &streamer->base;
}

/*
 * 在注入恢复配置时处理每个 tar 内容块。
                    */
static void
bbstreamer_recovery_injector_content(bbstreamer *streamer,
									 bbstreamer_member *member,
									 const char *data, int len,
									 bbstreamer_archive_context context)
{
	bbstreamer_recovery_injector *mystreamer;

	mystreamer = (bbstreamer_recovery_injector *) streamer;
	Assert(member != NULL || context == BBSTREAMER_ARCHIVE_TRAILER);

	switch (context)
	{
		case BBSTREAMER_MEMBER_HEADER:
			/* Must copy provided data so we have the option to modify it. */
			memcpy(&mystreamer->member, member, sizeof(bbstreamer_member));

			/*
			 * On v12+, skip standby.signal and edit postgresql.auto.conf; on
			 * older versions, skip recovery.conf.
			 */
			if (mystreamer->is_recovery_guc_supported)
			{
				mystreamer->skip_file =
					(strcmp(member->pathname, "standby.signal") == 0);
				mystreamer->is_postgresql_auto_conf =
					(strcmp(member->pathname, "postgresql.auto.conf") == 0);
				if (mystreamer->is_postgresql_auto_conf)
				{
					/* Remember we saw it so we don't add it again. */
					mystreamer->found_postgresql_auto_conf = true;

					/* 将要注入的数据长度递增。     */
					mystreamer->member.size +=
						mystreamer->recoveryconfcontents->len;

					/*
					 * 清除 data 和 len，因为归档头不再有效；如果需要，
					 * 后续的 bbstreamer 必须重新生成它。
					                        */
					data = NULL;
					len = 0;
				}
			}
			else
				mystreamer->skip_file =
					(strcmp(member->pathname, "recovery.conf") == 0);

			/* Do not forward if the file is to be skipped. */
			if (mystreamer->skip_file)
				return;
			break;

		case BBSTREAMER_MEMBER_CONTENTS:
			/* Do not forward if the file is to be skipped. */
			if (mystreamer->skip_file)
				return;
			break;

		case BBSTREAMER_MEMBER_TRAILER:
			/* Do not forward it the file is to be skipped. */
			if (mystreamer->skip_file)
				return;

			/* Append provided content to whatever we already sent. */
			if (mystreamer->is_postgresql_auto_conf)
				bbstreamer_content(mystreamer->base.bbs_next, member,
								   mystreamer->recoveryconfcontents->data,
								   mystreamer->recoveryconfcontents->len,
								   BBSTREAMER_MEMBER_CONTENTS);
			break;

		case BBSTREAMER_ARCHIVE_TRAILER:
			if (mystreamer->is_recovery_guc_supported)
			{
				/*
				 * 如果尚未找到（并因此修改）postgresql.auto.conf，
				 * 现在将其作为额外的归档成员注入。
				         */
				if (!mystreamer->found_postgresql_auto_conf)
					bbstreamer_inject_file(mystreamer->base.bbs_next,
										   "postgresql.auto.conf",
										   mystreamer->recoveryconfcontents->data,
										   mystreamer->recoveryconfcontents->len);

				/* Inject empty standby.signal file. */
				bbstreamer_inject_file(mystreamer->base.bbs_next,
									   "standby.signal", "", 0);
			}
			else
			{
				/* Inject recovery.conf file with specified contents. */
				bbstreamer_inject_file(mystreamer->base.bbs_next,
									   "recovery.conf",
									   mystreamer->recoveryconfcontents->data,
									   mystreamer->recoveryconfcontents->len);
			}

			/* Nothing to do here. */
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while injecting recovery settings");
	}

	bbstreamer_content(mystreamer->base.bbs_next, &mystreamer->member,
					   data, len, context);
}

/*
 * 此 bbstreamer 的流结束处理。
          */
static void
bbstreamer_recovery_injector_finalize(bbstreamer *streamer)
{
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * 释放与此 bbstreamer 关联的内存。
   */
static void
bbstreamer_recovery_injector_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer);
}

/*
 * 将具有指定内容的成员注入归档中。
          */
void
bbstreamer_inject_file(bbstreamer *streamer, char *pathname, char *data,
					   int len)
{
	bbstreamer_member member;

	strlcpy(member.pathname, pathname, MAXPGPATH);
	member.size = len;
	member.mode = pg_file_create_mode;
	member.is_directory = false;
	member.is_link = false;
	member.linktarget[0] = '\0';

	/*
	 * 这些值似乎没有原则性的依据，但它们是 PostgreSQL 历史上使用的值。
	                         */
	member.uid = 04000;
	member.gid = 02000;

	/*
	 * 这里不知道如何为正在使用的归档格式生成有效的成员头和尾部，
	 * 因此如果需要它们，某个后继 bbstreamer 必须使用 'member' 中的
	 * 数据来生成它们。
	                 */
	bbstreamer_content(streamer, &member, NULL, 0,
					   BBSTREAMER_MEMBER_HEADER);
	bbstreamer_content(streamer, &member, data, len,
					   BBSTREAMER_MEMBER_CONTENTS);
	bbstreamer_content(streamer, &member, NULL, 0,
					   BBSTREAMER_MEMBER_TRAILER);
}
