/*-------------------------------------------------------------------------
 *
 * be-fsstubs.h
 *
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/be-fsstubs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BE_FSSTUBS_H
#define BE_FSSTUBS_H

/*
 * 这些不能作为 fmgr 调用，但可供 C 代码使用。
 * 也许它们本该用没有下划线的名字，
 * 但现在已经太晚了...
 */
extern int	lo_read(int fd, char *buf, int len);
extern int	lo_write(int fd, const char *buf, int len);

/*
 * 在事务提交或中止时清理大对象
 */
extern void AtEOXact_LargeObject(bool isCommit);
extern void AtEOSubXact_LargeObject(bool isCommit, SubTransactionId mySubid,
									SubTransactionId parentSubid);

#endif							/* BE_FSSTUBS_H */
