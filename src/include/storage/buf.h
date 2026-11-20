/*-------------------------------------------------------------------------
 *
 * buf.h
 *	  缓冲管理器数据类型。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUF_H
#define BUF_H

/*
 * 缓冲区标识。
 *
 * 零为无效，正数为共享缓冲区下标（1..NBuffers），
 * 负数为本地缓冲区下标（-1 .. -NLocBuffer）。
 */
typedef int Buffer;

#define InvalidBuffer	0

/*
 * BufferIsInvalid
 *		缓冲区无效则为真。
 */
#define BufferIsInvalid(buffer) ((buffer) == InvalidBuffer)

/*
 * BufferIsLocal
 *		缓冲区为本地（其他后端不可见）则为真。
 */
#define BufferIsLocal(buffer)	((buffer) < 0)

/*
 * 缓冲区访问策略对象。
 *
 * BufferAccessStrategyData 为 freelist.c 私有
 */
typedef struct BufferAccessStrategyData *BufferAccessStrategy;

#endif							/* BUF_H */
