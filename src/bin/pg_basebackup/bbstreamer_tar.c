/*-------------------------------------------------------------------------
 *
 * bbstreamer_tar.c
 *
 * 此模块实现了三种类型的 tar 处理。tar 解析器期望无标签的数据块（例如
 * BBSTREAMER_UNKNOWN）并将其拆分为有标签的块（bbstreamer_archive_context 的
 * 其他任何值）。tar 归档器做相反的事情：它接受一组有标签的块并生成 tar 文件，
 * 可选择地替换成员头和尾部，以便上游 bbstreamer 对象可以在不了解 tar
 * 格式细节的情况下对 tar 文件内容进行修改。tar 结束器仅仅在文件末尾添加
 * 两个 NUL 字节块，因为较旧的服务器版本产生的文件省略了此结束符。
 *
 * 部分版权 (c) 1996-2023, PostgreSQL 全球开发组
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/bbstreamer_tar.c
 *-------------------------------------------------------------------------
                                                                         */

#include "postgres_fe.h"

#include <time.h>

#include "bbstreamer.h"
#include "common/logging.h"
#include "pgtar.h"

typedef struct bbstreamer_tar_parser
{
	bbstreamer	base;
	bbstreamer_archive_context next_context;
	bbstreamer_member member;
	size_t		file_bytes_sent;
	size_t		pad_bytes_expected;
} bbstreamer_tar_parser;

typedef struct bbstreamer_tar_archiver
{
	bbstreamer	base;
	bool		rearchive_member;
} bbstreamer_tar_archiver;

static void bbstreamer_tar_parser_content(bbstreamer *streamer,
										  bbstreamer_member *member,
										  const char *data, int len,
										  bbstreamer_archive_context context);
static void bbstreamer_tar_parser_finalize(bbstreamer *streamer);
static void bbstreamer_tar_parser_free(bbstreamer *streamer);
static bool bbstreamer_tar_header(bbstreamer_tar_parser *mystreamer);

const bbstreamer_ops bbstreamer_tar_parser_ops = {
	.content = bbstreamer_tar_parser_content,
	.finalize = bbstreamer_tar_parser_finalize,
	.free = bbstreamer_tar_parser_free
};

static void bbstreamer_tar_archiver_content(bbstreamer *streamer,
											bbstreamer_member *member,
											const char *data, int len,
											bbstreamer_archive_context context);
static void bbstreamer_tar_archiver_finalize(bbstreamer *streamer);
static void bbstreamer_tar_archiver_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_tar_archiver_ops = {
	.content = bbstreamer_tar_archiver_content,
	.finalize = bbstreamer_tar_archiver_finalize,
	.free = bbstreamer_tar_archiver_free
};

static void bbstreamer_tar_terminator_content(bbstreamer *streamer,
											  bbstreamer_member *member,
											  const char *data, int len,
											  bbstreamer_archive_context context);
static void bbstreamer_tar_terminator_finalize(bbstreamer *streamer);
static void bbstreamer_tar_terminator_free(bbstreamer *streamer);

const bbstreamer_ops bbstreamer_tar_terminator_ops = {
	.content = bbstreamer_tar_terminator_content,
	.finalize = bbstreamer_tar_terminator_finalize,
	.free = bbstreamer_tar_terminator_free
};

/*
 * 创建一个可以将内容流解析为 tar 数据的 bbstreamer。
 *
 * 输入应该是一系列 BBSTREAMER_UNKNOWN 块；由 'next' 指定的 bbstreamer
 * 将接收一系列类型化块，按照 bbstreamer.h 中描述的约定。
                              */
extern bbstreamer *
bbstreamer_tar_parser_new(bbstreamer *next)
{
	bbstreamer_tar_parser *streamer;

	streamer = palloc0(sizeof(bbstreamer_tar_parser));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_tar_parser_ops;
	streamer->base.bbs_next = next;
	initStringInfo(&streamer->base.bbs_buffer);
	streamer->next_context = BBSTREAMER_MEMBER_HEADER;

	return &streamer->base;
}

/*
 * Parse unknown content as tar data.
 */
static void
bbstreamer_tar_parser_content(bbstreamer *streamer, bbstreamer_member *member,
							  const char *data, int len,
							  bbstreamer_archive_context context)
{
	bbstreamer_tar_parser *mystreamer = (bbstreamer_tar_parser *) streamer;
	size_t		nbytes;

	/* Expect unparsed input. */
	Assert(member == NULL);
	Assert(context == BBSTREAMER_UNKNOWN);

	while (len > 0)
	{
		switch (mystreamer->next_context)
		{
			case BBSTREAMER_MEMBER_HEADER:

				/*
			 * 如果期望归档成员头，在进一步处理之前先积累完整的数据块。
			                                 */
				if (!bbstreamer_buffer_until(streamer, &data, &len,
											 TAR_BLOCK_SIZE))
					return;

				/*
			 * 现在我们可以处理头部并准备处理文件内容；但我们可能会发现
			 * 我们以为是下一个文件头的内容实际上是归档尾部的开始。
			 * 相应地切换模式。
			                                          */
				if (bbstreamer_tar_header(mystreamer))
				{
					if (mystreamer->member.size == 0)
					{
						/* 无内容；尾部长度为零。   */
						bbstreamer_content(mystreamer->base.bbs_next,
										   &mystreamer->member,
										   NULL, 0,
										   BBSTREAMER_MEMBER_TRAILER);

						/* Expect next header. */
						mystreamer->next_context = BBSTREAMER_MEMBER_HEADER;
					}
					else
					{
						/* 期望内容。  */
						mystreamer->next_context = BBSTREAMER_MEMBER_CONTENTS;
					}
					mystreamer->base.bbs_buffer.len = 0;
					mystreamer->file_bytes_sent = 0;
				}
				else
					mystreamer->next_context = BBSTREAMER_ARCHIVE_TRAILER;
				break;

			case BBSTREAMER_MEMBER_CONTENTS:

				/*
			 * 发送我们拥有的所有内容，但不超过剩余文件长度。
			                  */
				Assert(mystreamer->file_bytes_sent < mystreamer->member.size);
				nbytes = mystreamer->member.size - mystreamer->file_bytes_sent;
				nbytes = Min(nbytes, len);
				Assert(nbytes > 0);
				bbstreamer_content(mystreamer->base.bbs_next,
								   &mystreamer->member,
								   data, nbytes,
								   BBSTREAMER_MEMBER_CONTENTS);
				mystreamer->file_bytes_sent += nbytes;
				data += nbytes;
				len -= nbytes;

				/*
			 * 如果还没有发送完整个文件，则还有更多内容要来；
			 * 否则，是时候期望文件尾部了。
			                    */
				Assert(mystreamer->file_bytes_sent <= mystreamer->member.size);
				if (mystreamer->file_bytes_sent == mystreamer->member.size)
				{
					if (mystreamer->pad_bytes_expected == 0)
					{
						/* 尾部长度为零。   */
						bbstreamer_content(mystreamer->base.bbs_next,
										   &mystreamer->member,
										   NULL, 0,
										   BBSTREAMER_MEMBER_TRAILER);

						/* Expect next header. */
						mystreamer->next_context = BBSTREAMER_MEMBER_HEADER;
					}
					else
					{
						/* 尾部长度不为零。    */
						mystreamer->next_context = BBSTREAMER_MEMBER_TRAILER;
					}
					mystreamer->base.bbs_buffer.len = 0;
				}
				break;

			case BBSTREAMER_MEMBER_TRAILER:

				/*
			 * 如果期望归档成员尾部，在发送任何内容之前先积累预期数量
			 * 的填充字节。
			                                    */
				if (!bbstreamer_buffer_until(streamer, &data, &len,
											 mystreamer->pad_bytes_expected))
					return;

				/* OK, now we can send it. */
				bbstreamer_content(mystreamer->base.bbs_next,
								   &mystreamer->member,
								   data, mystreamer->pad_bytes_expected,
								   BBSTREAMER_MEMBER_TRAILER);

				/* Expect next file header. */
				mystreamer->next_context = BBSTREAMER_MEMBER_HEADER;
				mystreamer->base.bbs_buffer.len = 0;
				break;

			case BBSTREAMER_ARCHIVE_TRAILER:

				/*
				 * We've seen an end-of-archive indicator, so anything more is
				 * buffered and sent as part of the archive trailer. But we
				 * don't expect more than 2 blocks.
				 */
				bbstreamer_buffer_bytes(streamer, &data, &len, len);
				if (len > 2 * TAR_BLOCK_SIZE)
					pg_fatal("tar file trailer exceeds 2 blocks");
				return;

			default:
				/* Shouldn't happen. */
				pg_fatal("unexpected state while parsing tar archive");
		}
	}
}

/*
 * 解析 tar 流中的文件头部。
 *
 * 如果找到了文件头部并将其传递给了下一个 bbstreamer，返回值为 true；
 * 如果已到达归档尾部，则为 false。
            */
static bool
bbstreamer_tar_header(bbstreamer_tar_parser *mystreamer)
{
	bool		has_nonzero_byte = false;
	int			i;
	bbstreamer_member *member = &mystreamer->member;
	char	   *buffer = mystreamer->base.bbs_buffer.data;

	Assert(mystreamer->base.bbs_buffer.len == TAR_BLOCK_SIZE);

	/* 检查是否得到了一个全零字节的块。   */
	for (i = 0; i < TAR_BLOCK_SIZE; ++i)
	{
		if (buffer[i] != '\0')
		{
			has_nonzero_byte = true;
			break;
		}
	}

	/*
	 * 如果整个块都是零，这是归档的结束，而不是下一个文件的开始。
	            */
	if (!has_nonzero_byte)
		return false;

	/*
	 * 从头部中解析关键字段。
	 *
	 * FIXME：我们在这里使用硬编码值而不是某种更原则性的方法，这很糟糕。
	 * 这种情况已经存在很久了，但我们应该做得更好。
	      */
	strlcpy(member->pathname, &buffer[0], MAXPGPATH);
	if (member->pathname[0] == '\0')
		pg_fatal("tar member has empty name");
	member->size = read_tar_number(&buffer[124], 12);
	member->mode = read_tar_number(&buffer[100], 8);
	member->uid = read_tar_number(&buffer[108], 8);
	member->gid = read_tar_number(&buffer[116], 8);
	member->is_directory = (buffer[156] == '5');
	member->is_link = (buffer[156] == '2');
	if (member->is_link)
		strlcpy(member->linktarget, &buffer[157], 100);

	/* 计算填充字节数。         */
	mystreamer->pad_bytes_expected = tarPaddingBytesRequired(member->size);

	/* 将整个头部转发给下一个 bbstreamer。   */
	bbstreamer_content(mystreamer->base.bbs_next, member,
					   buffer, TAR_BLOCK_SIZE,
					   BBSTREAMER_MEMBER_HEADER);

	return true;
}

/*
 * tar 解析器的流结束处理。
         */
static void
bbstreamer_tar_parser_finalize(bbstreamer *streamer)
{
	bbstreamer_tar_parser *mystreamer = (bbstreamer_tar_parser *) streamer;

	if (mystreamer->next_context != BBSTREAMER_ARCHIVE_TRAILER &&
		(mystreamer->next_context != BBSTREAMER_MEMBER_HEADER ||
		 mystreamer->base.bbs_buffer.len > 0))
		pg_fatal("COPY stream ended before last file was finished");

	/* 发送归档尾部，即使为空。     */
	bbstreamer_content(streamer->bbs_next, NULL,
					   streamer->bbs_buffer.data, streamer->bbs_buffer.len,
					   BBSTREAMER_ARCHIVE_TRAILER);

	/* Now finalize successor. */
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * 释放与 tar 解析器关联的内存。
 */
static void
bbstreamer_tar_parser_free(bbstreamer *streamer)
{
	pfree(streamer->bbs_buffer.data);
	bbstreamer_free(streamer->bbs_next);
}

/*
 * 创建一个可以生成 tar 归档的 bbstreamer。
 *
 * 这既可用于生成全新的 tar 归档，也可用于在运行中修改现有的归档。
 * 输入应该是一系列类型化块（即不是 BBSTREAMER_UNKNOWN）。
 * 另请参阅 bbstreamer_tar_parser_content 的注释。
                        */
extern bbstreamer *
bbstreamer_tar_archiver_new(bbstreamer *next)
{
	bbstreamer_tar_archiver *streamer;

	streamer = palloc0(sizeof(bbstreamer_tar_archiver));
	*((const bbstreamer_ops **) &streamer->base.bbs_ops) =
		&bbstreamer_tar_archiver_ops;
	streamer->base.bbs_next = next;

	return &streamer->base;
}

/*
 * 修复输入块流以创建有效的 tar 文件。
 *
 * 如果 BBSTREAMER_MEMBER_HEADER 块的大小为 0，则用新构建的 tar 头替换它。
 * 如果大小为 TAR_BLOCK_SIZE，则原样传递。任何其他大小都是致命错误（表示存在 bug）。
 *
 * 每当构建新的 BBSTREAMER_MEMBER_HEADER 块时，相应的 BBSTREAMER_MEMBER_TRAILER
 * 块也从头构建。具体来说，我们构建一个全零字节块，足以填充到块边界，
 * 如 tar 格式所要求的。其他 BBSTREAMER_MEMBER_TRAILER 块原样传递。
 *
 * 任何 BBSTREAMER_MEMBER_CONTENTS 块原样传递。
 *
 * BBSTREAMER_ARCHIVE_TRAILER 块被替换为两个全零字节块。并非所有 tar 程序
 * 都需要这个，但显然有些程序需要。服务器不提供此尾部。如果没有归档尾部，
 * bbstreamer_tar_parser_finalize 会添加一个。
                                                                                                                                                                    */
static void
bbstreamer_tar_archiver_content(bbstreamer *streamer,
								bbstreamer_member *member,
								const char *data, int len,
								bbstreamer_archive_context context)
{
	bbstreamer_tar_archiver *mystreamer = (bbstreamer_tar_archiver *) streamer;
	char		buffer[2 * TAR_BLOCK_SIZE];

	Assert(context != BBSTREAMER_UNKNOWN);

	if (context == BBSTREAMER_MEMBER_HEADER && len != TAR_BLOCK_SIZE)
	{
		Assert(len == 0);

		/* 用新构建的头部替换零长度 tar 头。              */
		tarCreateHeader(buffer, member->pathname, NULL,
						member->size, member->mode, member->uid, member->gid,
						time(NULL));
		data = buffer;
		len = TAR_BLOCK_SIZE;

		/* 同时标记需要替换填充，以防大小改变。     */
		mystreamer->rearchive_member = true;
	}
	else if (context == BBSTREAMER_MEMBER_TRAILER &&
			 mystreamer->rearchive_member)
	{
		int			pad_bytes = tarPaddingBytesRequired(member->size);

		/* 如果重新生成了头部，同时替换填充。 */
		memset(buffer, 0, pad_bytes);
		data = buffer;
		len = pad_bytes;

		/* Don't do this again unless we replace another header. */
		mystreamer->rearchive_member = false;
	}
	else if (context == BBSTREAMER_ARCHIVE_TRAILER)
	{
		/* 尾部始终应为两个全零字节块。         */
		memset(buffer, 0, 2 * TAR_BLOCK_SIZE);
		data = buffer;
		len = 2 * TAR_BLOCK_SIZE;
	}

	bbstreamer_content(streamer->bbs_next, member, data, len, context);
}

/*
 * tar 归档器的流结束处理。
           */
static void
bbstreamer_tar_archiver_finalize(bbstreamer *streamer)
{
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * 释放与 tar 归档器关联的内存。
   */
static void
bbstreamer_tar_archiver_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer);
}

/*
 * 创建一个 bbstreamer，盲目地在服务器可能发送给我们的不完整 tar 文件
 * 末尾添加两个 NUL 字节块。
  */
bbstreamer *
bbstreamer_tar_terminator_new(bbstreamer *next)
{
	bbstreamer *streamer;

	streamer = palloc0(sizeof(bbstreamer));
	*((const bbstreamer_ops **) &streamer->bbs_ops) =
		&bbstreamer_tar_terminator_ops;
	streamer->bbs_next = next;

	return streamer;
}

/*
 * 原样传递所有内容。
                  */
static void
bbstreamer_tar_terminator_content(bbstreamer *streamer,
								  bbstreamer_member *member,
								  const char *data, int len,
								  bbstreamer_archive_context context)
{
	/* Expect unparsed input. */
	Assert(member == NULL);
	Assert(context == BBSTREAMER_UNKNOWN);

	/* 直接转发。  */
	bbstreamer_content(streamer->bbs_next, member, data, len, context);
}

/*
 * 在结束时，盲目地添加服务器未能提供的两个 NUL 字节块。
            */
static void
bbstreamer_tar_terminator_finalize(bbstreamer *streamer)
{
	char		buffer[2 * TAR_BLOCK_SIZE];

	memset(buffer, 0, 2 * TAR_BLOCK_SIZE);
	bbstreamer_content(streamer->bbs_next, NULL, buffer,
					   2 * TAR_BLOCK_SIZE, BBSTREAMER_UNKNOWN);
	bbstreamer_finalize(streamer->bbs_next);
}

/*
 * 释放与 tar 结束器关联的内存。
     */
static void
bbstreamer_tar_terminator_free(bbstreamer *streamer)
{
	bbstreamer_free(streamer->bbs_next);
	pfree(streamer);
}
