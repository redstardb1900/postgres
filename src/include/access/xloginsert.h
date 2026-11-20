/*
 * xloginsert.h
 *
 * 生成 WAL 记录的函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xloginsert.h
 */
#ifndef XLOGINSERT_H
#define XLOGINSERT_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/*
 * WAL 构造工作区的最小尺寸。若需
 * 注册超过 XLR_NORMAL_MAX_BLOCK_ID 个块引用，或有超过
 * XLR_NORMAL_RDATAS 个数据块于单条 WAL 记录，须先调用
 * XLogEnsureRecordSpace() 以分配更多工作内存。
 */
#define XLR_NORMAL_MAX_BLOCK_ID		4
#define XLR_NORMAL_RDATAS			20

/* XLogRegisterBuffer 的标志 */
#define REGBUF_FORCE_IMAGE	0x01	/* 强制整页映像 */
#define REGBUF_NO_IMAGE		0x02	/* 不拍摄整页映像 */
#define REGBUF_WILL_INIT	(0x04 | 0x02)	/* 页将在重放时重初始化
											 * （隐含 NO_IMAGE）*/
#define REGBUF_STANDARD		0x08	/* 页遵循标准页布局，
									 * （pd_lower 与 pd_upper 间数据
									 * 将被跳过）*/
#define REGBUF_KEEP_DATA	0x10	/* 即使取了整页映像也
									 * 含数据*/

/* xloginsert.c 中公开函数的原型： */
extern void XLogBeginInsert(void);
extern void XLogSetRecordFlags(uint8 flags);
extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info);
extern void XLogEnsureRecordSpace(int max_block_id, int ndatas);
extern void XLogRegisterData(char *data, uint32 len);
extern void XLogRegisterBuffer(uint8 block_id, Buffer buffer, uint8 flags);
extern void XLogRegisterBlock(uint8 block_id, RelFileLocator *rlocator,
							  ForkNumber forknum, BlockNumber blknum, char *page,
							  uint8 flags);
extern void XLogRegisterBufData(uint8 block_id, char *data, uint32 len);
extern void XLogResetInsertion(void);
extern bool XLogCheckBufferNeedsBackup(Buffer buffer);

extern XLogRecPtr log_newpage(RelFileLocator *rlocator, ForkNumber forknum,
							  BlockNumber blkno, char *page, bool page_std);
extern void log_newpages(RelFileLocator *rlocator, ForkNumber forknum, int num_pages,
						 BlockNumber *blknos, char **pages, bool page_std);
extern XLogRecPtr log_newpage_buffer(Buffer buffer, bool page_std);
extern void log_newpage_range(Relation rel, ForkNumber forknum,
							  BlockNumber startblk, BlockNumber endblk, bool page_std);
extern XLogRecPtr XLogSaveBufferForHint(Buffer buffer, bool buffer_std);

extern void InitXLogInsert(void);

#endif							/* XLOGINSERT_H */
