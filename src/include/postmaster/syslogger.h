/*-------------------------------------------------------------------------
 *
 * syslogger.h
 *	  导出自 postmaster/syslogger.c。
 *
 * Copyright (c) 2004-2023, PostgreSQL Global Development Group
 *
 * src/include/postmaster/syslogger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYSLOGGER_H
#define _SYSLOGGER_H

#include <limits.h>				/* for PIPE_BUF */


/*
 * 写入 syslogger 管道的原始协议结构。想法是把
 * 长消息分成不超过 PIPE_BUF 字节的块；按 POSIX
 * 规范，这些块必须原子写入管道。管道读者再用
 * 协议头把消息各部分重组成单一字符串。读者也能
 * 处理管道中的非协议数据，但无法保证长串不被
 * 拆开。
 * 。
 *
 * 在 is_last 中用非空字节，使协议对假双空字节
 * 前导稍稳健。但若不小心，仍可能在 len 和/或
 * pid 字节中找到假前导。
 */

#ifdef PIPE_BUF
/* 有 PIPE_BUF > 64K 的系统吗？不太可能，但…… */
#if PIPE_BUF > 65536
#define PIPE_CHUNK_SIZE  65536
#else
#define PIPE_CHUNK_SIZE  ((int) PIPE_BUF)
#endif
#else							/* 未定义 */
/* POSIX 说 PIPE_BUF 至少 512，故用该值 */
#define PIPE_CHUNK_SIZE  512
#endif

typedef struct
{
	char		nuls[2];		/* 总是 \0\0 */
	uint16		len;			/* 本块大小（只计数据） */
	int32		pid;			/* 写者 pid */
	bits8		flags;			/* PIPE_PROTO_* 位掩码 */
	char		data[FLEXIBLE_ARRAY_MEMBER];	/* 数据载荷自此开始 */
} PipeProtoHeader;

typedef union
{
	PipeProtoHeader proto;
	char		filler[PIPE_CHUNK_SIZE];
} PipeProtoChunk;

#define PIPE_HEADER_SIZE  offsetof(PipeProtoHeader, data)
#define PIPE_MAX_PAYLOAD  ((int) (PIPE_CHUNK_SIZE - PIPE_HEADER_SIZE))

/* PipeProtoHeader->flags 的标志位 */
#define PIPE_PROTO_IS_LAST	0x01	/* 消息最后一块？ */
/* 日志目的地 */
#define PIPE_PROTO_DEST_STDERR	0x10
#define PIPE_PROTO_DEST_CSVLOG	0x20
#define PIPE_PROTO_DEST_JSONLOG	0x40

/* GUC 选项 */
extern PGDLLIMPORT bool Logging_collector;
extern PGDLLIMPORT int Log_RotationAge;
extern PGDLLIMPORT int Log_RotationSize;
extern PGDLLIMPORT char *Log_directory;
extern PGDLLIMPORT char *Log_filename;
extern PGDLLIMPORT bool Log_truncate_on_rotation;
extern PGDLLIMPORT int Log_file_mode;

#ifndef WIN32
extern PGDLLIMPORT int syslogPipe[2];
#else
extern PGDLLIMPORT HANDLE syslogPipe[2];
#endif


extern int	SysLogger_Start(void);

extern void write_syslogger_file(const char *buffer, int count, int destination);

#ifdef EXEC_BACKEND
extern void SysLoggerMain(int argc, char *argv[]) pg_attribute_noreturn();
#endif

extern bool CheckLogrotateSignal(void);
extern void RemoveLogrotateSignalFiles(void);

/*
 * 保存 syslogger 当前使用的日志文件元数据
 * 信息的文件名
 */
#define LOG_METAINFO_DATAFILE  "current_logfiles"
#define LOG_METAINFO_DATAFILE_TMP  LOG_METAINFO_DATAFILE ".tmp"

#endif							/* _SYSLOGGER_H */
