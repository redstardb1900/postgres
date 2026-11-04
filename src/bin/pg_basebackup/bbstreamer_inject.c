/*-------------------------------------------------------------------------
 *
 * bbstreamer_inject.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
 * 创建一个能将恢复数据编辑到归档流中的 bbstreamer。  
 *
 * 输入应为一系列类型化的块（非 BBSTREAMER_UNKNOWN），     
 * bbstreamer.h 中所述的约定；转发给下一个 bbstreamer 的块
 * 类型也将类似；若我们编辑过归档流， 
 * BBSTREAMER_MEMBER_HEADER 块就可能为零长度。                   
 *                           
 *
 * 我们的目标是对 recoveryconfcontents 传入的内容执行以下三  
 * 种操作之一：(1) 如果 is_recovery_guc_supported 为 false，则   
 * 把内容放入 recovery.conf，替换任何同名归档成员；        
 * (2) 如果 is_recovery_guc_supported 为 true 且         
 * postgresql.auto.conf 存在于归档中，则将提供的内容    
 * 附加到现有文件；(3) 如果 is_recovery_guc_supported 为      
 * true 但归档中不存在 postgresql.auto.conf，则用指定的内容 
 * 内容创建它。            
 *
 * 此外，如果 is_recovery_guc_supported 为 true，则会创建  
 * 零长度的 standby.signal 文件，并从归档删除同名文件。
 *             
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
			/* 必须复制提供的数据，保留修改它的选项。   */
			memcpy(&mystreamer->member, member, sizeof(bbstreamer_member));

			/*
			 * v12+ 上跳过 standby.signal 并编辑 postgresql.auto.conf  
			 * ；旧版跳过 recovery.conf。   
			 */
			if (mystreamer->is_recovery_guc_supported)
			{
				mystreamer->skip_file =
					(strcmp(member->pathname, "standby.signal") == 0);
				mystreamer->is_postgresql_auto_conf =
					(strcmp(member->pathname, "postgresql.auto.conf") == 0);
				if (mystreamer->is_postgresql_auto_conf)
				{
					/* 记住已见过它，以免重复添加。   */
					mystreamer->found_postgresql_auto_conf = true;

					/* 将要注入的数据长度递增。     */
					mystreamer->member.size +=
						mystreamer->recoveryconfcontents->len;

					/*
					 * 清除 data 和 len，因为归档头已失效；
					 * 某些后续的 bbstreamer 必须重新生成
					 * 它们（如有必要）。     
					 */
					data = NULL;
					len = 0;
				}
			}
			else
				mystreamer->skip_file =
					(strcmp(member->pathname, "recovery.conf") == 0);

			/* 若该文件将被跳过，则不转发。   */
			if (mystreamer->skip_file)
				return;
			break;

		case BBSTREAMER_MEMBER_CONTENTS:
			/* 若该文件将被跳过，则不转发。   */
			if (mystreamer->skip_file)
				return;
			break;

		case BBSTREAMER_MEMBER_TRAILER:
			/* 若该文件将被跳过，则不转发。   */
			if (mystreamer->skip_file)
				return;

			/* 把提供的内容追加到已发送内容之后。  */
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
				 * 若尚未找到（并因此修改过）    
				 * postgresql.auto.conf，则注入为附加归档         
				 * 成员     
				 */
				if (!mystreamer->found_postgresql_auto_conf)
					bbstreamer_inject_file(mystreamer->base.bbs_next,
										   "postgresql.auto.conf",
										   mystreamer->recoveryconfcontents->data,
										   mystreamer->recoveryconfcontents->len);

				/* 注入空 standby.signal 文件   */
				bbstreamer_inject_file(mystreamer->base.bbs_next,
									   "standby.signal", "", 0);
			}
			else
			{
				/* 注入带指定内容的 recovery.conf 文件。   */
				bbstreamer_inject_file(mystreamer->base.bbs_next,
									   "recovery.conf",
									   mystreamer->recoveryconfcontents->data,
									   mystreamer->recoveryconfcontents->len);
			}

			/* 此处无事可做  */
			break;

		default:
			/* 不应发生。   */
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
	 * 这些值似乎没有原则性的依据，但它们是 PostgreSQL      
	 * 历来沿用的值。                 
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
