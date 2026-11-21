/*-------------------------------------------------------------------------
 *
 * pgtar.h
 *	  操作 tar 文件数据结构的函数（src/port/tar.c）
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/pgtar.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TAR_H
#define PG_TAR_H

#define		TAR_BLOCK_SIZE	512

enum tarError
{
	TAR_OK = 0,
	TAR_NAME_TOO_LONG,
	TAR_SYMLINK_TOO_LONG
};

extern enum tarError tarCreateHeader(char *h, const char *filename,
									 const char *linktarget, pgoff_t size,
									 mode_t mode, uid_t uid, gid_t gid,
									 time_t mtime);
extern uint64 read_tar_number(const char *s, int len);
extern void print_tar_number(char *s, int len, uint64 val);
extern int	tarChecksum(char *header);

/*
 * 计算 tar 归档中一项所需的填充字节数。须填到
 * TAR_BLOCK_SIZE 的倍数。因其为 2 的幂，可用
 * TYPEALIGN()。
 */
static inline size_t
tarPaddingBytesRequired(size_t len)
{
	return TYPEALIGN(TAR_BLOCK_SIZE, len) - len;
}

#endif
