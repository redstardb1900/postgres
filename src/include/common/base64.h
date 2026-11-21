/*
 * base64.h
 *	  无空白支持的 base64 编解码例程
 *	  。
 *
 * Portions Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * src/include/common/base64.h
 */
#ifndef BASE64_H
#define BASE64_H

/* 略 */
extern int	pg_b64_encode(const char *src, int len, char *dst, int dstlen);
extern int	pg_b64_decode(const char *src, int len, char *dst, int dstlen);
extern int	pg_b64_enc_len(int srclen);
extern int	pg_b64_dec_len(int srclen);

#endif							/* BASE64_H */
