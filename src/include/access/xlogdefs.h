/*
 * xlogdefs.h
 *
 * Postgres 预写式日志管理器的记录指针与
 * 时间线编号定义
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogdefs.h
 */
#ifndef XLOG_DEFS_H
#define XLOG_DEFS_H

#include <fcntl.h>				/* 需 open() 标志 */

/*
 * 指向 XLOG 中某处的指针。这些指针宽 64 位，
 * 因为我们不希望它们发生溢出。
 */
typedef uint64 XLogRecPtr;

/*
 * 零用来表示无效指针。引导跳过第一个可能的
 * WAL 段，在 WAL 段大小处初始化首个 WAL 页，故无 XLOG 
 * 记录可始于零。
 */
#define InvalidXLogRecPtr		0
#define XLogRecPtrIsValid(r)	((r) != InvalidXLogRecPtr)
#define XLogRecPtrIsInvalid(r)	((r) == InvalidXLogRecPtr)

/*
 * 用于"伪造" LSNs 的首 LSN。
 *
 * 小于此值者可用于各 AM 的特殊用途。
 */
#define FirstNormalUnloggedLSN	((XLogRecPtr) 1000)

/*
 * 以常规格式打印 XLogRecPtr 的便捷宏，例如，
 *
 * printf("%X/%X", LSN_FORMAT_ARGS(lsn));
 */
#define LSN_FORMAT_ARGS(lsn) (AssertVariableIsOfTypeMacro((lsn), XLogRecPtr), (uint32) ((lsn) >> 32)), ((uint32) (lsn))

/*
 * XLogSegNo - 物理日志文件序号。
 */
typedef uint64 XLogSegNo;

/*
 * TimeLineID (TLI) - 标识不同数据库历史以防
 * 在恢复数据库安装的先前状态后产生混淆。
 * TLI 在数据库正常停机/重启时不变（包括
 * 崩溃并恢复情形）；但我们必须在做完
 * 恢复到先前状态（即时间点恢复）后赋新 TLI。这使
 * 我们生成的新 WAL 日志文件序列可区别于
 * 先前化身中所生成的序列。
 */
typedef uint32 TimeLineID;

/*
 * 复制源 id - 放于此文件以免必须
 * 在多处 xlog 相关位置包含 origin.h。
 */
typedef uint16 RepOriginId;

/*
 * 这段 hackery 试图判定哪些文件同步方法
 * 在当前平台上可用，并选择合适的
 * 默认方法。
 *
 * 注意：在 Windows 上自定 O_DSYNC，但不定 O_SYNC。
 */
#if defined(PLATFORM_DEFAULT_SYNC_METHOD)
#define DEFAULT_SYNC_METHOD		PLATFORM_DEFAULT_SYNC_METHOD
#elif defined(O_DSYNC) && (!defined(O_SYNC) || O_DSYNC != O_SYNC)
#define DEFAULT_SYNC_METHOD		SYNC_METHOD_OPEN_DSYNC
#else
#define DEFAULT_SYNC_METHOD		SYNC_METHOD_FDATASYNC
#endif

#endif							/* XLOG_DEFS_H */
